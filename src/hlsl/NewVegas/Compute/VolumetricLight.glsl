// VolumetricLight as Vulkan passes (needs NVR DXVK): what the passes of Effects/VolumetricLight.fx.hlsl share, line for line
// (with the parts of Includes/Depth.hlsl, Shadows.hlsl and SunCascades.hlsl they use). effects/VolumetricLight.cpp ExtRender.
// The matrices arrive as the D3D9 effect uploads them (SetMatrix keeps the logical matrix), so HLSL mul(v, M) is M * v
// here, and M[i][j] is the same element. One uniform block for every pass (VolumetricLightEffect::ExtRender).
// Each pass includes Common.glsl, declares its textures (t_depth with VL_DEPTH, t_atlas with VL_SHADOWS), then this file.

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 shadowNear;               // TESR_ShadowCameraToLightTransformNear
    mat4 shadowMiddle;             // TESR_ShadowCameraToLightTransformMiddle
    mat4 shadowFar;                // TESR_ShadowCameraToLightTransformFar
    mat4 shadowLod;                // TESR_ShadowCameraToLightTransformLod
    mat4 reproject;                // TESR_VolumetricLightReproject
    mat4 viewTransform;            // TESR_ViewTransform
    mat4 projectionTransform;      // TESR_ProjectionTransform
    vec4 shadowNearCenter;         // TESR_ShadowNearCenter (xyz centre, w radius)
    vec4 shadowMiddleCenter;       // TESR_ShadowMiddleCenter
    vec4 shadowFarCenter;          // TESR_ShadowFarCenter
    vec4 shadowLodCenter;          // TESR_ShadowLodCenter
    vec4 shadowFormatData;         // TESR_ShadowFormatData
    vec4 shadowBlur;               // TESR_ShadowBlur
    vec4 smoothedSunDir;           // TESR_SmoothedSunDir
    vec4 sunColor;                 // TESR_SunColor
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 cameraData;               // TESR_CameraData
    vec4 data;                     // TESR_VolumetricLightData
    vec4 data2;                    // TESR_VolumetricLightData2
    vec4 data3;                    // TESR_VolumetricLightData3
    vec4 lightLayout;              // TESR_VolumetricLightLayout
    vec4 gridLayout;               // TESR_VolumetricLightGridLayout
    vec4 gridData;                 // TESR_VolumetricLightGridData
    vec4 fogData;                  // TESR_FogData
    vec4 sunAmount;                // TESR_SunAmount
    vec4 gameTime;                 // TESR_GameTime
    vec4 fogDensity;               // TESR_VolumetricFogDensity
    vec4 fogShape;                 // TESR_VolumetricFogShape
    vec4 fogWind;                  // TESR_VolumetricFogWind
    vec4 fogScatter;               // TESR_VolumetricFogScatter
    vec4 fogWeather;               // TESR_VolumetricFogWeather
    vec4 fogNight;                 // TESR_VolumetricFogNight
    vec4 fogNightScatter;          // TESR_VolumetricFogNightScatter
    vec4 cameraPosition;           // TESR_CameraPosition
    vec4 gameMultisample;          // TESR_GameMultisample (UNOFFICIAL 2026-11-05: the composites' sky shares, MsaaSkyEdge.glsl)
    vec4 rt;                       // TESR_VolumetricLightRt (UNOFFICIAL 2026-11-07 ptaaa-fog: RT fog visibility, VL_RT below)
} cb;

layout(push_constant) uniform Push {
    uint pointIndex;               // 0: POINT, CLAMP (the effect's buffers, depth, the frame)
    uint linearIndex;              // 4: LINEAR, CLAMP (TESR_ShadowAtlas, TESR_VolumetricLightGrid)
    uint wrapIndex;                // 8: LINEAR, WRAP (NVR_FogNoise)
    uint pad;
    vec2 axis;                     // 16: the blur's direction (Blur X / Blur Y)
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.pointIndex)), (coord), 0.0)
#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearIndex)), (coord), 0.0)

#define farZ (cb.cameraData.y)
#define Strength (cb.data.x)
#define Darken (cb.data.y)
#define Density (cb.data.z)
#define MarchDistance (cb.data.w)
#define Steps (cb.data2.x)
#define Anisotropy (cb.data2.y)
#define Fade (cb.data2.z)
#define NoiseFrame (cb.data2.w)
#define HistoryWeight (cb.data3.x)
#define BlurRadius (cb.data3.y)
#define LowTexel (cb.lightLayout.xy)
#define GridSize (cb.gridLayout.xy)
#define GridAtlasTexel (cb.gridLayout.zw)

// HLSL fmod (the sign of x), not GLSL mod.
float hlslFmod(float x, float y) { return x - y * trunc(x / y); }

// Includes/Depth.hlsl toWorld
vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

#ifdef VL_DEPTH
// Includes/Depth.hlsl readDepthLod (TESR_DepthBuffer, POINT)
float readDepthLod(vec2 coord) { return POINT_LOD(t_depth, coord).x * farZ; }
#endif

#ifdef VL_SHADOWS
// Includes/Shadows.hlsl, the parts MarchShadow uses
float Linstep(float a, float b, float v) { return saturate((v - a) / (b - a)); }
float ReduceLightBleeding(float pMax, float amount) { return Linstep(amount, 1.0, pMax); }

float ChebyshevUpperBound(vec2 moments, float mean, float minVariance, float lightBleedingReduction) {
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);
    float d = mean - moments.x;
    float pMax = variance / (variance + (d * d));
    pMax = ReduceLightBleeding(pMax, lightBleedingReduction);
    return (mean <= moments.x ? 1.0 : pMax);
}

float ShadowPCF2x2(vec2 coord, float depth, float texel) {
    vec2 p = coord / texel - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 uv0 = (base + 0.5) * texel;
    vec4 d = vec4(LINEAR_LOD(t_atlas, uv0).x, LINEAR_LOD(t_atlas, uv0 + vec2(texel, 0.0)).x,
                  LINEAR_LOD(t_atlas, uv0 + vec2(0.0, texel)).x, LINEAR_LOD(t_atlas, uv0 + vec2(texel)).x);
    vec4 lit = vec4(lessThanEqual(vec4(depth), d));
    return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

float GetLightAmountValueVSM(vec2 moments, float depth, float bias, float bleedReduction) {
    return ChebyshevUpperBound(moments, depth, bias, bleedReduction);
}

vec2 GetEVSMExponents(float positiveExponent, float negativeExponent, float formatBits) {
    float maxExponent = formatBits == 0.0 ? 5.54 : 42.0;
    vec2 lightSpaceExponents = vec2(positiveExponent, negativeExponent);
    return min(lightSpaceExponents, vec2(maxExponent));
}

vec2 WarpDepth(float depth, vec2 exponents) {
    depth = 2.0 * depth - 1.0;
    float pos = exp(exponents.x * depth);
    float neg = -exp(-exponents.y * depth);
    return vec2(pos, neg);
}

float GetLightAmountValueEVSM2(vec2 moments, float depth, float bias, float bleedReduction, float formatBits) {
    vec2 exponents = GetEVSMExponents(40.0, 5.0, formatBits);
    vec2 warpedDepth = WarpDepth(depth, exponents);
    vec2 depthScale = bias * exponents * warpedDepth;
    vec2 minVariance = depthScale * depthScale;
    return ChebyshevUpperBound(moments, warpedDepth.x, minVariance.x, bleedReduction);
}

// Includes/SunCascades.hlsl SunCascadeTexCoord
vec4 SunCascadeTexCoord(vec4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5 + 0.5;
    coord.y = coord.y * -0.5 + 0.5;
    return coord;
}

// VolumetricLight.fx.hlsl MarchShadow
float MarchShadow(vec3 position) {
    mat4 lightTransform;
    vec2 quadrant;
    float bleed;
    if (length(position - cb.shadowNearCenter.xyz) < cb.shadowNearCenter.w) {
        lightTransform = cb.shadowNear; quadrant = vec2(0.0, 0.0); bleed = 0.1;
    }
    else if (length(position - cb.shadowMiddleCenter.xyz) < cb.shadowMiddleCenter.w) {
        lightTransform = cb.shadowMiddle; quadrant = vec2(0.5, 0.0); bleed = 0.2;
    }
    else if (length(position - cb.shadowFarCenter.xyz) < cb.shadowFarCenter.w) {
        lightTransform = cb.shadowFar; quadrant = vec2(0.0, 0.5); bleed = 0.6;
    }
    else if (length(position - cb.shadowLodCenter.xyz) < cb.shadowLodCenter.w) {
        lightTransform = cb.shadowLod; quadrant = vec2(0.5, 0.5); bleed = 0.8;
    }
    else return 1.0;
    vec4 lightSpace = SunCascadeTexCoord(lightTransform * vec4(position, 1.0));
    if (cb.shadowFormatData.x == 3.0) return ShadowPCF2x2(lightSpace.xy * 0.5 + quadrant, lightSpace.z - 0.0002, cb.shadowBlur.x);
    vec4 moments = LINEAR_LOD(t_atlas, lightSpace.xy * 0.5 + quadrant);
    float bias = cb.shadowFormatData.x == 0.0 ? 0.00001 : 0.01;
    if (cb.shadowFormatData.x == 0.0) return GetLightAmountValueVSM(moments.xy, lightSpace.z, bias, bleed);
    if (cb.shadowFormatData.x == 1.0) return GetLightAmountValueEVSM2(moments.xy, lightSpace.z, bias, bleed, cb.shadowFormatData.y);
    vec2 exponents = GetEVSMExponents(40.0, 5.0, cb.shadowFormatData.y);
    float warped = WarpDepth(lightSpace.z, exponents).x;
    float depthScale = bias * exponents.x * warped;
    return ChebyshevUpperBound(moments.xz, warped, depthScale * depthScale, bleed);
}
#endif

#ifdef VL_COMPOSITE
// VolumetricLight.fx.hlsl CompositeAir
vec4 CompositeAir(vec2 coord, vec4 color, vec2 air) {
    float cosTheta = dot(normalize(toWorld(coord)), cb.smoothedSunDir.xyz);
    float g = Anisotropy;
    float phase = min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-4), 1.5), 8.0);

    vec3 lin = linearize(max(color.rgb, vec3(0.0)));
    float shadedAir = saturate(air.g - air.r);
    lin = lin * (1.0 - Darken * Fade * shadedAir) + linearize(cb.sunColor.rgb) * (Strength * Fade * phase * air.r);
    return vec4(delinearize(max(lin, vec3(0.0))), color.a);
}
#endif

#ifdef VL_GRID
// VolumetricLight.fx.hlsl GridDepth, GridUV
float GridDepth(float s) { s /= 64.0; return MarchDistance * s * s; }

vec2 GridUV(float z, vec2 local) {
    vec2 tile = vec2(hlslFmod(z, 8.0), floor(z / 8.0));
    return (tile * GridSize + clamp(local, vec2(0.5), GridSize - 0.5)) * GridAtlasTexel;
}
#endif

#ifdef VL_RT
// UNOFFICIAL 2026-11-07 (ptaaa-fog) VolumetricLight.fx.hlsl RtShare, RtFogVisAt, SunVisibility (needs VL_SHADOWS, VL_GRID and t_rtvis:
// TESR_RtFogVisibility, LINEAR, CLAMP)
float RtShare(vec3 position) {
    return cb.rt.x > 0.5 ? saturate((cb.rt.y - length(position)) / max(cb.rt.z, 1.0)) : 0.0;
}

float RtFogVisAt(vec3 position) {
    vec3 f = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    vec3 r = vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    vec3 u = vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    float planar = max(dot(position, f), 1.0e-3);
    vec2 ndc = vec2(dot(position, r) * cb.projectionTransform[0][0], dot(position, u) * cb.projectionTransform[1][1]) / planar;
    vec2 local = vec2(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y) * GridSize;
    float s = sqrt(saturate(planar / max(cb.rt.w, 1.0))) * 64.0 - 0.5;
    float z = clamp(floor(s), 0.0, 63.0);
    float a = LINEAR_LOD(t_rtvis, GridUV(z, local)).r;
    float b = LINEAR_LOD(t_rtvis, GridUV(min(z + 1.0, 63.0), local)).r;
    return mix(a, b, saturate(s - z));
}

float SunVisibility(vec3 position) {
    float share = RtShare(position);
    float visibility = 1.0;
    if (share > 0.0) visibility = RtFogVisAt(position);
    if (share < 1.0) visibility = mix(MarchShadow(position), visibility, share);
    return visibility;
}
#endif
