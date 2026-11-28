// SunShadows as Vulkan passes (needs NVR DXVK): what every pass of Effects/SunShadows.fx.hlsl shares, with its includes
// (Depth.hlsl, Normals.hlsl, Shadows.hlsl, Blur.hlsl), line for line (effects/SunShadows.cpp ExtRender). Each pass function is
// its own .frag (SunShadowsMarch, ...Blur, ...Apply, ...Sharp, ...SharpFinish) including this file; all declare the same bindings.
// The matrices arrive as the D3D9 effect uploads them (ID3DXEffect::SetMatrix: the logical matrix), so HLSL M[i][j] is the
// same element here, mul(v, M) is M * v and mul(M, v) is v * M.
// Samplers as the .fx declares them: LINEAR/CLAMP for the depth, the atlas, the normals and the shadow buffer (all one mip level,
// so MIPFILTER LINEAR changes nothing and tex2D is textureLod 0), LINEAR/WRAP with linear mips for the blue noise.
// (Each .frag starts with #version 450 and GL_GOOGLE_include_directive before including this file.)
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;     // TESR_DepthBuffer (s0)
layout(set = 1, binding = 1) uniform texture2D t_atlas;     // TESR_ShadowAtlas (s1)
layout(set = 1, binding = 2) uniform texture2D t_normals;   // TESR_NormalsBuffer (s2)
layout(set = 1, binding = 3) uniform texture2D t_shadow;    // TESR_PointShadowBuffer (s3): the pass's source (buffer or scratch)
layout(set = 1, binding = 4) uniform texture2D t_noise;     // TESR_NoiseSampler (s4): Effects\bluenoise256.dds
layout(set = 1, binding = 5) uniform texture2D t_charShadow; // TESR_CharShadowMap (s5, 2026-10-26): read only by the CHARACTER_SHADOW_MAP variant

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 cascadeNear;           // TESR_ShadowCameraToLightTransformNear
    mat4 cascadeMiddle;         // TESR_ShadowCameraToLightTransformMiddle
    mat4 cascadeFar;            // TESR_ShadowCameraToLightTransformFar
    mat4 cascadeLod;            // TESR_ShadowCameraToLightTransformLod
    mat4 projectionTransform;   // TESR_ProjectionTransform
    mat4 invProjectionTransform;// TESR_InvProjectionTransform
    mat4 viewTransform;         // TESR_ViewTransform
    mat4 invViewTransform;      // TESR_InvViewTransform
    vec4 reciprocalResolution;  // TESR_ReciprocalResolution
    vec4 smoothedSunDir;        // TESR_SmoothedSunDir
    vec4 viewSpaceLightDir;     // TESR_ViewSpaceLightDir
    vec4 shadowFormatData;      // TESR_ShadowFormatData (x mode, y format bits)
    vec4 screenSpaceData;       // TESR_ShadowScreenSpaceData (x enabled, y blur radius, z render distance, w intensity)
    vec4 shadowFade;            // TESR_ShadowFade
    vec4 shadowBlur;            // TESR_ShadowBlur (x 1 / atlas resolution)
    vec4 forwardData;           // TESR_ShadowForwardData (x forward suppressed, w HandoverBlend)
    vec4 nearCenter;            // TESR_ShadowNearCenter
    vec4 middleCenter;          // TESR_ShadowMiddleCenter
    vec4 farCenter;             // TESR_ShadowFarCenter
    vec4 lodCenter;             // TESR_ShadowLodCenter
    vec4 cameraData;            // TESR_CameraData (x nearZ, y farZ)
    vec4 flags;                 // x: FORWARD_SHADOWS as the effect was compiled (1 or 0); y, z: NearFade's start and end (TESR_ShadowScreenSpaceFade, 2026-10-27); w: the RT range in RT sun shadows Mode 1 (2026-10-28), 0 = off
    mat4 charShadowCameraToLight; // TESR_CharShadowCameraToLight (2026-10-26, CharacterShadowMap)
    vec4 charShadowCenter;      // TESR_CharShadowCenter
    vec4 charShadowData;        // TESR_CharShadowData
    vec4 renderScale;           // TESR_RenderScale (xy scale, zw 1/scale)
} cb;

layout(push_constant) uniform Push {
    uint linearIndex;           // 0: LINEAR, CLAMP
    uint noiseIndex;            // 4: LINEAR, WRAP, linear between mips
    uint pass;                  // 8: the blur: 0 DepthBlurKeep H, 1 DepthBlurKeep V, 2 FinalContactBlur
    uint pad;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define LINEAR_S NVR_SAMPLER(pc.linearIndex)

// SunShadows.fx.hlsl statics
#define SSS_STEPNUM 5
const float SSS_DIST = 2000.0;
const float SSS_THICKNESS = 20.0;
#define SSS_MAXDEPTH (cb.screenSpaceData.z * cb.screenSpaceData.x)
#define Mode cb.shadowFormatData.x
#define FormatBits cb.shadowFormatData.y
const float NormalBiasTexels = 2.5;
const float SlopeBias = 1.0;

// Depth.hlsl statics
#define nearZ cb.cameraData.x
#define farZ cb.cameraData.y

// HLSL smoothstep (GLSL's is undefined for edge0 >= edge1)
float hlslSmoothstep(float a, float b, float x);
// (UNOFFICIAL 2026-10-27, as SunShadows.fx's ContactNearFade) the contact shadows fade in with distance: none nearer than flags.y, full from flags.z
float ContactNearFade(float z) { return cb.flags.z > 0.0 ? hlslSmoothstep(cb.flags.y, cb.flags.z, z) : 1.0; }
float hlslSmoothstep(float a, float b, float x) {
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

vec2 clampRectUV(vec2 ruv) {
    vec2 minUV = 0.5 * cb.reciprocalResolution.xy;
    vec2 maxUV = max(cb.renderScale.xy - minUV, minUV);
    return clamp(ruv, minUV, maxUV);
}

vec2 toRectUV(vec2 screenUV) {
    return clampRectUV(screenUV * cb.renderScale.xy);
}

// The single-level buffers: tex2D and tex2Dlod level 0 read the same texel
vec4 sampleShadow(vec2 ruv) { return textureLod(sampler2D(t_shadow, LINEAR_S), clampRectUV(ruv), 0.0); }

// --- Depth.hlsl ---
float projectedDepthFromLinear(float linear01) {
    float viewZ = max(linear01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

float readDepth(vec2 ruv) {
    return textureLod(sampler2D(t_depth, LINEAR_S), clampRectUV(ruv), 0.0).x * farZ;
}

float readDepthLod(vec2 ruv) {
    return textureLod(sampler2D(t_depth, LINEAR_S), clampRectUV(ruv), 0.0).x * farZ;
}

vec3 reconstructPosition(vec2 coord) {
    float x = coord.x * 2.0 - 1.0;
    float y = (1.0 - coord.y) * 2.0 - 1.0;
    float z = projectedDepthFromLinear(textureLod(sampler2D(t_depth, LINEAR_S), toRectUV(coord), 0.0).x);
    vec4 clipSpace = vec4(x, y, z, 1.0);

    vec4 viewSpace = cb.invProjectionTransform * clipSpace;

    viewSpace /= viewSpace.w;

    return viewSpace.xyz;
}

vec3 projectPosition(vec3 position) {
    vec4 projection = cb.projectionTransform * vec4(position, 1.0);
    projection.xyz /= projection.w;

    projection.x = projection.x * 0.5 + 0.5;
    projection.y = 0.5 - 0.5 * projection.y;

    return projection.xyz;
}

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += vec3(1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += vec3(-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

float getHomogenousDepth(vec2 coord) {
    float depth = readDepth(toRectUV(coord));
    vec3 camera_vector = toWorld(coord) * depth;
    return length(camera_vector);
}

vec4 reconstructWorldPositionLod(vec2 coord, out float viewDepth) {
    float x = coord.x * 2.0 - 1.0;
    float y = (1.0 - coord.y) * 2.0 - 1.0;
    float z = projectedDepthFromLinear(textureLod(sampler2D(t_depth, LINEAR_S), toRectUV(coord), 0.0).x);
    vec4 viewSpace = cb.invProjectionTransform * vec4(x, y, z, 1.0);

    viewSpace /= viewSpace.w;
    viewDepth = viewSpace.z;

    vec4 worldSpace = cb.invViewTransform * viewSpace;
    return vec4(worldSpace.xyz, 1.0);
}

// --- Normals.hlsl ---
vec3 GetNormal(vec2 ruv) {
    return textureLod(sampler2D(t_normals, LINEAR_S), clampRectUV(ruv), 0.0).xyz * 2.0 - 1.0;
}

vec3 GetWorldNormalLod(vec2 ruv) {
    vec4 normal = vec4(textureLod(sampler2D(t_normals, LINEAR_S), clampRectUV(ruv), 0.0).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// --- Blur.hlsl ---
const int cKernelSize = 12;

const float BlurWeights[cKernelSize] = float[cKernelSize](
    0.057424882,
    0.058107773,
    0.061460144,
    0.071020611,
    0.088092873,
    0.106530916,
    0.106530916,
    0.088092873,
    0.071020611,
    0.061460144,
    0.058107773,
    0.057424882
);

// BlurOffsets: -6..-1 then +1..+6 times TESR_ReciprocalResolution (no zero tap)
vec2 BlurOffsets(int i) {
    float k = i < 6 ? float(i - 6) : float(i - 5);
    return vec2(k * cb.reciprocalResolution.x, k * cb.reciprocalResolution.y);
}

// --- BlurDepth.hlsl DepthBlurKeep (buffer = TESR_PointShadowBuffer, depthDrop 3500, endFade SSS_MAXDEPTH) ---
vec4 DepthBlurKeep(vec2 ruv, vec2 OffsetMask, float blurRadius, float depthDrop, float endFade) {
    vec2 centerUV = clampRectUV(ruv);
    vec4 center = sampleShadow(centerUV);
    float depth1 = readDepthLod(centerUV);
    if (depth1 > endFade) return center;

    vec4 taps[cKernelSize];
    vec4 spread = vec4(0.0);
    for (int t = 0; t < cKernelSize; t++) {
        taps[t] = sampleShadow(clampRectUV(centerUV + (BlurOffsets(t) * OffsetMask) * blurRadius));
        spread = max(spread, abs(taps[t] - center));
    }
    if (max(spread.r, spread.g) < 0.0001) return vec4(center.rgb, 1.0);

    float WeightSum = 0.114725602;
    vec4 color1 = center * WeightSum;
    depthDrop *= (depth1 / farZ);

    for (int i = 0; i < cKernelSize; i++) {
        float depth2 = readDepthLod(clampRectUV(centerUV + (BlurOffsets(i) * OffsetMask) * blurRadius));
        float diff = abs(depth1 - depth2);

        float useForBlur = (diff <= depthDrop) ? 1.0 : 0.0;
        color1 += BlurWeights[i] * taps[i] * useForBlur;
        WeightSum += BlurWeights[i] * useForBlur;
    }
    color1 /= WeightSum;
    return vec4(color1.rgb, 1.0);
}
