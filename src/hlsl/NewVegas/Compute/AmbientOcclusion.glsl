// Shared by the AmbientOcclusion Vulkan passes (needs NVR DXVK): the constants and helpers of Effects/AmbientOcclusion.fx.hlsl (the
// Dedicated techniques and SkyVisibilityV2) and its includes (Depth.hlsl, Blur.hlsl, Helpers.hlsl, Normals.hlsl, SkyShare.hlsl,
// SkyTake.hlsl), line for line. effects/AmbientOcclusion.cpp (ExtPrepare, Render, RenderSkyVisibility).
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

// The constants as the D3D9 constant table holds them for the pass (too big for push data). Matrices: the D3DX matrix as stored
// (SetMatrix keeps the logical matrix), so cb.m[i][j] is the HLSL's m[i][j], mul(v, M) is cb.m * v and mul(M, v) is v * cb.m.
layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;      // TESR_ProjectionTransform
    mat4 invProjectionTransform;   // TESR_InvProjectionTransform
    mat4 invViewTransform;         // TESR_InvViewTransform
    mat4 shadowCameraToLightOrtho; // TESR_ShadowCameraToLightTransformOrtho
    vec4 aoData;                   // TESR_AmbientOcclusionAOData: x samples, y strength, z clamp, w range
    vec4 data;                     // TESR_AmbientOcclusionData: x angle bias, y luminance threshold, z blur drop, w blur radius
    vec4 gtao;                     // TESR_AmbientOcclusionGTAO: w largest radius (pixels)
    vec4 sky;                      // TESR_AmbientOcclusionSky (Skylighting v1)
    vec4 sky2;                     // TESR_AmbientOcclusionSky2 (v2: x on, y strength, z sky light, w sun light)
    vec4 sky3;                     // TESR_AmbientOcclusionSky3
    vec4 sky4;                     // TESR_AmbientOcclusionSky4 (z sun mask bound, w factor bound)
    vec4 sky5;                     // TESR_AmbientOcclusionSky5 (LightCap; y OpenSides)
    vec4 smoothedSunDir;           // TESR_SmoothedSunDir
    vec4 shadowFade;               // TESR_ShadowFade
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 fogData;                  // TESR_FogData
    vec4 fogColor;                 // TESR_FogColor
    vec4 depthConstants;           // TESR_DepthConstants: z inverted depth
    vec4 cameraData;               // TESR_CameraData: x near, y far
    vec4 aoLayout;                 // NVR_AOLayout: xy the AO extent / full extent, zw 1 / the AO target's dimensions
    vec4 skyLayout;                // the sky pass: xy 1 / its target's dimensions (FrameVS's uv there), z NVR_SkySmooth.x
    // UNOFFICIAL 2026-10-28 (fork RT, [Shaders.AmbientOcclusion.RT]; read only by the RT passes, the others end above)
    mat4 rtReproject;              // this frame's view space -> last frame's clip space (BounceLightMath.h), as stored
    vec4 rtTemporal;               // x last frame's share (0: no history), y pixels of motion that halve it, z frame number (0-1023),
                                   // w [Shaders.AmbientOcclusion.RT] DebugView
    // UNOFFICIAL 2026-11-03 RT AO at RenderScale < 1 (docs/next/RT-RENDERSCALE.md): read by RtAOInput / AmbientOcclusionTemporalRT only
    vec4 rtRect;                   // xy this frame's RenderRect scale (the frame is the top-left xy part of the targets; 1 when off),
                                   // zw the scale the RT history was made at
} cb;

// Per pass (24 bytes).
layout(push_constant) uniform Push {
    vec2 axis;                     // 0: DedicatedBlur's uniform axis (io.xy, io.yx)
    uint gtaoEstimate;             // 8: 1 = GTAOEstimate (the GTAO techniques), 0 = PackedEstimate
    uint linearSampler;            // 12: LINEAR, CLAMP
    uint pointSampler;             // 16: POINT, CLAMP
    uint pointWrapSampler;         // 20: POINT, WRAP (TESR_BlueNoiseSampler: MIN/MAG/MIP NONE)
} pc;

// UNOFFICIAL 2026-10-26: the variant .spv fixes this (-DAO_GTAO, Compute/variants.txt, EffectRecord::ExtVariant), as the D3D9
// techniques are compiled one per mode; without it the push block decides (the general shader, the fallback).
#ifdef AO_GTAO
#define AO_GTAO_ESTIMATE (AO_GTAO != 0)
#else
#define AO_GTAO_ESTIMATE (pc.gtaoEstimate != 0u)
#endif

#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearSampler)), (coord), 0.0)
#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.pointSampler)), (coord), 0.0)
#define POINT_WRAP_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.pointWrapSampler)), (coord), 0.0)

// Helpers.hlsl
vec2 aoRectScale() { return cb.rtRect.xy; }
vec2 aoInvRectScale() { return 1.0 / max(cb.rtRect.xy, vec2(1.0e-5)); }
vec2 aoClampToRect(vec2 u, vec2 texel) { return clamp(u, 0.5 * texel, cb.rtRect.xy - 0.5 * texel); }
vec2 aoClampToRectIfOn(vec2 u, vec2 texel) { return (cb.rtRect.x < 0.9999 || cb.rtRect.y < 0.9999) ? aoClampToRect(u, texel) : u; }

#define expand(v) (((v) - 0.5) / 0.5)
#define compress(v) (((v) * 0.5) + 0.5)
#define invlerp(a, b, t) ((t - a) / (b - a))
const float PI = 3.1415926538;

// The effect compiler's acos for ps_3_0 (fxc, checked by disassembly): the Cg polynomial, sqrt as rcp(rsq(|1 - |x||)).
float acosHlsl(float x) {
    float a = abs(x);
    float r = ((-0.0187292993 * a + 0.0742610022) * a - 0.212114394) * a + 1.57072878;
    r *= sqrt(abs(1.0 - a));
    return x < 0.0 ? 3.14159274 - r : r;
}

// AmbientOcclusion.fx.hlsl statics
#define AOstrength (cb.aoData.y)
#define AOclamp (cb.aoData.z)
#define AOrange (cb.aoData.w)
#define AOangleBias (cb.data.x)
#define AOlumThreshold (cb.data.y)
#define blurDrop (cb.data.z)
#define blurRadius (cb.data.w)
const float startFade = 2000.0;
const float endFade = 8000.0;
const int kernelSize = 5;

// Depth.hlsl
#define invertedDepth (cb.depthConstants.z != 0.0)
#define nearZ (cb.cameraData.x)
#define farZ (cb.cameraData.y)

float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

vec3 projectPosition(vec3 position) {
    vec4 projection = cb.projectionTransform * vec4(position, 1.0);
    projection.xyz /= projection.w;

    projection.x = projection.x * 0.5 + 0.5;
    projection.y = 0.5 - 0.5 * projection.y;
    projection.xy *= aoRectScale();

    return projection.xyz;
}

// Blur.hlsl
const int cKernelSize = 12;
const float BlurWeights[cKernelSize] = float[](
    0.057424882, 0.058107773, 0.061460144, 0.071020611, 0.088092873, 0.106530916,
    0.106530916, 0.088092873, 0.071020611, 0.061460144, 0.058107773, 0.057424882);
const float BlurSteps[cKernelSize] = float[](-6.0, -5.0, -4.0, -3.0, -2.0, -1.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0);
#define BlurOffsets(i) (vec2(BlurSteps[i] * cb.reciprocalResolution.x, BlurSteps[i] * cb.reciprocalResolution.y))

// AmbientOcclusion.fx.hlsl
vec2 DedicatedUV(vec2 uv) {
    return clamp(uv, 0.5 * cb.aoLayout.zw, aoRectScale() - 0.5 * cb.aoLayout.zw);
}

// SkyShare.hlsl
float SkyShare(vec3 worldNormal, float sunMask) {
    float ambientE = cb.sky2.z;
    float sunE = cb.sky2.w * saturate(dot(worldNormal, cb.smoothedSunDir.xyz)) * sunMask;
    return ambientE / max(ambientE + sunE, 1.0e-5);
}

float SkyFactor(float visibility, vec3 worldNormal, float sunMask, float covered) {
    float share = cb.sky3.z > 0.5 ? SkyShare(worldNormal, sunMask) : 1.0;
    float factor = 1.0 - saturate(cb.sky2.y * (1.0 - visibility) * share);
    float view = cb.sky3.w;
    if (view > 0.5) factor = view < 1.5 ? visibility : (view < 2.5 ? share : (view < 3.5 ? factor : 1.0 - covered));
    return factor;
}

// SkyTake.hlsl
vec3 SkyTake(vec3 linearColor, float factor, float visibility, float cap, float noLimit) {
    float take = min(1.0 - factor, ((1.0 - visibility) * cap + noLimit) / max(luma(linearColor), 1.0e-6));
    return linearColor * (1.0 - take);
}
