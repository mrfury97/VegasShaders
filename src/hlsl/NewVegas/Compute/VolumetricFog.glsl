// VolumetricFog as Vulkan passes (needs NVR DXVK): what the passes of Effects/VolumetricFog.fx.hlsl share, line for line (with
// the parts of Includes/Depth.hlsl, Normals.hlsl, Shadows.hlsl, Sky.hlsl, SkyTake.hlsl and InteriorShadowApply.hlsl they use).
// effects/VolumetricFog.cpp ExtDedicated, ExtShadowAO, ExtRender. The techniques' compile-time switches (legacy, classic,
// interior; PHYSICAL_SKY_ON) arrive in the push block, so one shader serves each pass of every variant.
// The matrices arrive as the D3D9 effect uploads them (SetMatrix keeps the logical matrix), so HLSL mul(v, M) is M * v here,
// mul(M, v) is v * M, and M[i][j] is the same element. One uniform block for every pass.
// VolumetricFogEstimate.frag (the half-resolution coefficients, two targets) includes this as it is; VolumetricFogApply.frag
// (every full-resolution pass) defines FOG_APPLY first, for the fog buffers, the AO and the composites' other inputs.

layout(set = 1, binding = 0) uniform texture2D t_scene;        // TESR_SourceBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;        // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_atlas;        // TESR_ShadowAtlas (LINEAR, CLAMP; read at level 0)
layout(set = 1, binding = 3) uniform texture2D t_normals;      // TESR_NormalsBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_pointShadow;  // TESR_PointShadowBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 5) uniform texture3D t_noise;        // NVR_FogNoise (LINEAR, WRAP; read at level 0)
layout(set = 1, binding = 6) uniform texture2D t_skyView;      // NVR_FogSkyView (LINEAR, CLAMP)
layout(set = 1, binding = 7) uniform texture2D t_grid;         // NVR_FogGrid (LINEAR, CLAMP)
#ifdef FOG_APPLY
layout(set = 1, binding = 8) uniform texture2D t_fogMul;       // NVR_FogBuffer (POINT, CLAMP)
layout(set = 1, binding = 9) uniform texture2D t_fogAdd;       // NVR_FogBufferAdd (POINT, CLAMP)
layout(set = 1, binding = 10) uniform texture2D t_ao;          // NVR_AOBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 11) uniform texture2D t_skyVis;      // NVR_SkyVis (POINT, CLAMP)
layout(set = 1, binding = 12) uniform texture2D t_sunFar;      // NVR_SunFar (POINT, CLAMP)
layout(set = 1, binding = 13) uniform texture2D t_skyCov;      // NVR_FogSkyCoverage (POINT, CLAMP; UNOFFICIAL 2026-11-05 MsaaEdgeFix)
#endif

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 shadowNear;               // TESR_ShadowCameraToLightTransformNear
    mat4 shadowMiddle;             // TESR_ShadowCameraToLightTransformMiddle
    mat4 shadowFar;                // TESR_ShadowCameraToLightTransformFar
    mat4 shadowLod;                // TESR_ShadowCameraToLightTransformLod
    mat4 viewTransform;            // TESR_ViewTransform
    mat4 projectionTransform;      // TESR_ProjectionTransform
    vec4 fogColor;                 // TESR_FogColor
    vec4 fogData;                  // TESR_FogData
    vec4 reciprocalResolution;     // TESR_ReciprocalResolution
    vec4 gameTime;                 // TESR_GameTime
    vec4 smoothedSunDir;           // TESR_SmoothedSunDir
    vec4 sunPosition;              // TESR_SunPosition
    vec4 sunColor;                 // TESR_SunColor
    vec4 sunDiskColor;             // TESR_SunDiskColor
    vec4 sunAmbient;               // TESR_SunAmbient
    vec4 horizonColor;             // TESR_HorizonColor
    vec4 skyLowColor;              // TESR_SkyLowColor
    vec4 skyColor;                 // TESR_SkyColor
    vec4 skyData;                  // TESR_SkyData
    vec4 sunsetColor;              // TESR_SunsetColor
    vec4 sunAmount;                // TESR_SunAmount
    vec4 pbrData;                  // TESR_PBRData
    vec4 shadowNearCenter;         // TESR_ShadowNearCenter
    vec4 shadowMiddleCenter;       // TESR_ShadowMiddleCenter
    vec4 shadowFarCenter;          // TESR_ShadowFarCenter
    vec4 shadowLodCenter;          // TESR_ShadowLodCenter
    vec4 shadowFormatData;         // TESR_ShadowFormatData
    vec4 shadowBlur;               // TESR_ShadowBlur
    vec4 shadowFade;               // TESR_ShadowFade
    vec4 fogDensity;               // TESR_VolumetricFogDensity
    vec4 fogShape;                 // TESR_VolumetricFogShape
    vec4 fogWind;                  // TESR_VolumetricFogWind
    vec4 fogScatter;               // TESR_VolumetricFogScatter
    vec4 fogWeather;               // TESR_VolumetricFogWeather
    vec4 fogAerial;                // TESR_VolumetricFogAerial
    vec4 fogAerialTint;            // TESR_VolumetricFogAerialTint
    vec4 fogDistant;               // TESR_VolumetricFogDistant
    vec4 fogGlobal;                // TESR_VolumetricFogGlobal
    vec4 fogNight;                 // TESR_VolumetricFogNight
    vec4 fogNightScatter;          // TESR_VolumetricFogNightScatter
    vec4 fogSun;                   // TESR_VolumetricFogSun
    vec4 fogNightTint;             // TESR_VolumetricFogNightTint
    vec4 physSky;                  // TESR_PhysSky
    vec4 physSkySun;               // TESR_PhysSkySun
    vec4 vlData;                   // TESR_VolumetricLightData
    vec4 vlData2;                  // TESR_VolumetricLightData2
    vec4 vlGridLayout;             // TESR_VolumetricLightGridLayout
    vec4 vlGridData;               // TESR_VolumetricLightGridData
    vec4 fogLow;                   // TESR_VolumetricFogLow
    vec4 fogHigh;                  // TESR_VolumetricFogHigh
    vec4 fogSimple;                // TESR_VolumetricFogSimple
    vec4 fogBlend;                 // TESR_VolumetricFogBlend
    vec4 fogHeight;                // TESR_VolumetricFogHeight
    vec4 fogLegacyData;            // TESR_VolumetricFogData
    vec4 shadowData;               // TESR_ShadowData
    vec4 waterSettings;            // TESR_WaterSettings
    vec4 aoAOData;                 // TESR_AmbientOcclusionAOData
    vec4 aoData;                   // TESR_AmbientOcclusionData
    vec4 cameraData;               // TESR_CameraData
    vec4 cameraPosition;           // TESR_CameraPosition
    vec4 skyIrradiance[9];         // TESR_SkyIrradiance
    vec4 skyIrradianceGame[9];     // TESR_SkyIrradianceGame
    // UNOFFICIAL 2026-10-26 (VolumetricFog.cpp FogExtLinearNames): these colours linearized once a draw on the CPU, as the D3D9
    // effect's preshader does (linearized here on every pixel they cost most of the fog estimate's extra 0.04 ms, desert)
    vec4 fogColorLin, skyColorLin, skyLowColorLin, horizonColorLin, sunDiskColorLin, sunsetColorLin, sunAmbientLin, fogAerialTintLin, sunColorLin;
} cb;

// Push block: the samplers' heap indices (filled in by DXVK), the variant, and the effect's own NVR_ constants (set by the C++
// with Effect->SetVector in the D3D9 path).
layout(push_constant) uniform Push {
    uint pointIndex;               // 0: POINT, CLAMP
    uint linearIndex;              // 4: LINEAR, CLAMP
    uint wrapIndex;                // 8: LINEAR, WRAP (NVR_FogNoise)
    uint mode;                     // 12: VolumetricFogApply.frag: 0 DedicatedFogCombine, 1 DedicatedFogComposite,
                                   //     2 DedicatedFogCompositeInterior, 3 ShadowAOApply, 4 VolumetricFog
    uint legacy;                   // 16: the *Legacy techniques
    uint classic;                  // 20: the *Classic techniques
    uint interior;                 // 24: CompositeFogInterior* (the estimate's sky test reads the darkened scene)
    uint physicalSky;              // 28: PHYSICAL_SKY_ON (the effect was compiled with it)
    vec4 fogLayout;                // 32: NVR_FogLayout
    vec4 compositeFlags;           // 48: NVR_CompositeFlags
    vec4 compositeAOTexel;         // 64: NVR_CompositeAOTexel
    vec4 gridFlags;                // 80: NVR_FogGridFlags
    vec4 sunFarFlags;              // 96: NVR_SunFarFlags
} pc;

layout(location = 0) in vec2 uv;

// UNOFFICIAL 2026-10-26: the D3D9 techniques' switches (their uniform bool parameters and PHYSICAL_SKY_ON) are fixed when the
// .fx compiles, so each pass there is its own trimmed shader. A variant .spv does the same: compiled with -DFOG_VARIANT and
// FOG_MODE / FOG_LEGACY / FOG_CLASSIC / FOG_INTERIOR / FOG_PHYSICAL (tools/nvr-dxvk/compile-compute.sh; the C++ picks it,
// effects/VolumetricFog.cpp ExtVariant). Without FOG_VARIANT the push block decides at run time: the generic shader, kept as
// the fallback. Same code either way, so the same picture; a variant only drops the paths its pass never takes (one shader
// with all five passes and every look made the light ShadowAO pass carry the whole fog's registers: 2x slower, 2026-10-25).
#ifdef FOG_VARIANT
#define FOG_MODE_IS(m) (uint(FOG_MODE) == (m))
#define FOG_LEGACY_ON (FOG_LEGACY != 0)
#define FOG_CLASSIC_ON (FOG_CLASSIC != 0)
#define FOG_INTERIOR_ON (FOG_INTERIOR != 0)
#define FOG_PHYSICAL_ON (FOG_PHYSICAL != 0)
#else
#define FOG_MODE_IS(m) (pc.mode == (m))
#define FOG_LEGACY_ON (pc.legacy != 0u)
#define FOG_CLASSIC_ON (pc.classic != 0u)
#define FOG_INTERIOR_ON (pc.interior != 0u)
#define FOG_PHYSICAL_ON (pc.physicalSky != 0u)
#endif

#define POINT_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.pointIndex)), (coord), 0.0)
#define LINEAR_LOD(t, coord) textureLod(sampler2D(t, NVR_SAMPLER(pc.linearIndex)), (coord), 0.0)

// ---- Includes/Helpers.hlsl ----
#define compress(v) (((v) * 0.5) + 0.5)
#define shade(n, l) max(dot(n, l), 0.0)
#define invlerps(a, b, t) saturate(((t) - (a)) / ((b) - (a)))
#define scaledReinhard(a, b) (((a) * (b)) / (1.0 + (a) * (b)))
#define PI 3.1415926538

vec2 saturate(vec2 x) { return clamp(x, vec2(0.0), vec2(1.0)); }

// HLSL fmod (the sign of x), not GLSL mod.
float hlslFmod(float x, float y) { return x - y * trunc(x / y); }
// HLSL smoothstep, also for edge0 >= edge1 (undefined in GLSL).
float hlslSmoothstep(float a, float b, float x) { float t = saturate((x - a) / (b - a)); return t * t * (3.0 - 2.0 * t); }
// HLSL acos/asin as the effect compiler expands them for ps_3_0 (the cubic fit; no acos instruction there).
float hlslAcos(float x) {
    float a = abs(x);
    float r = (((-0.0187292993 * a + 0.0742610022) * a - 0.212114394) * a + 1.57072878) * sqrt(1.0 - a);
    return x < 0.0 ? 3.14159274 - r : r;
}
float hlslAsin(float x) {
    float a = abs(x);
    float r = (((-0.0187292993 * a + 0.0742610022) * a - 0.212114394) * a + 1.57072878) * sqrt(1.0 - a);
    return 1.57079637 - (x < 0.0 ? 3.14159274 - r : r);
}

// ---- the static consts of VolumetricFog.fx.hlsl ----
#define FOG_GROUND (-10000.0)
#define SUNINFLUENCE (1.0 / cb.skyData.y)
#define nearFog (cb.fogData.x)
#define farFog (cb.fogData.y)
#define FogPower (cb.fogData.w)

#define FogAmount max(0.0, cb.fogGlobal.x)
#define NightDisableSkyMask (cb.fogGlobal.y)
#define MoonVisibility saturate(cb.fogGlobal.z)
#define MinDensityFloor saturate(cb.fogGlobal.w)
#define NightDensityScale max(0.0, cb.fogNight.x)
#define NightAmountScale max(0.0, cb.fogNight.y)
#define NightHeightFalloffScale max(0.0, cb.fogNight.z)
#define NightMaxHeightOffset (cb.fogNight.w)
#define NightNoiseStrengthScale max(0.0, cb.fogNightScatter.x)
#define NightWindSpeedScale max(0.0, cb.fogNightScatter.y)
#define NightExtinctionScale max(0.0, cb.fogNightScatter.z)
#define NightInscatteringScale max(0.0, cb.fogNightScatter.w)
#define NightTint max(vec3(0.0), cb.fogNightTint.xyz)
#define NightAmbientStrength max(0.0, cb.fogNightTint.w)

#define BaseDensity max(0.0, cb.fogDensity.x)
#define WeatherImpact max(0.0, cb.fogDensity.y)
#define MorningFogDip saturate(cb.fogDensity.z)
#define SunriseSunsetBoost max(0.0, cb.fogDensity.w)

#define HeightFalloff max(0.0001, cb.fogShape.x)
#define MaxHeight (cb.fogShape.y)
#define Extinction (max(0.0, cb.fogShape.z) * 0.1)
#define Inscattering (max(0.0, cb.fogShape.w) * 0.1)

#define WindDirection (cb.fogWind.xy)
#define WindSpeed (cb.fogWind.z)
#define NoiseScale max(0.0001, cb.fogWind.w)

#define PhaseAsymmetry saturate(cb.fogScatter.x)
#define ShadowStrength saturate(cb.fogScatter.y)
#define NoiseStrength saturate(cb.fogScatter.z)
#define HeightInfluence saturate(cb.fogScatter.w)

#define WeatherFilterBlend saturate(cb.fogWeather.x)
#define isExterior (cb.fogWeather.y)
#define SkyAmbientAvailable (cb.fogWeather.z)
#define FogSaturation max(0.0, cb.fogWeather.w)

#define AerialStrength max(0.0, cb.fogAerial.x)
#define AerialRangeStart saturate(cb.fogAerial.y)
#define AerialTintBlend saturate(cb.fogAerial.z)
#define AerialDayFadeStart saturate(cb.fogAerial.w)

#define DistantFogRange exp(-4.0 * clamp(cb.fogDistant.x, 0.00000001, 1.0))
#define DistantFogBlend (cb.fogDistant.y)
#define DistantFogHeight (cb.fogDistant.z)
#define EdgeAA max(0.0, cb.fogDistant.w)

#define FogSkyColorCoeff 6.0

#define LegacyBaseFogStrength max(0.0, cb.fogLegacyData.x)
#define LegacyHeightFogSaturation max(0.0, cb.fogLegacyData.y)
#define LegacyFogAmount max(0.0, cb.fogLegacyData.z)
#define LegacyHeightFogInscattering (max(0.0, cb.fogLegacyData.w) * 0.1)
#define LegacyFogSaturation max(0.0, cb.fogLow.x)
#define LegacyWeatherImpact max(0.0, cb.fogLow.y)
#define LegacyDistantFogRange exp(-4.0 * cb.fogLow.z)
#define LegacySunPower max(0.0, cb.fogLow.w)
#define LegacyHeightFogDensity max(0.0, cb.fogHigh.x)
#define LegacyHeightFogFalloff max(0.0, cb.fogHigh.y)
#define LegacyHeightFogDist (cb.fogHigh.z)
#define LegacyHeightFogSkyColor (2.0 * cb.fogHigh.w)
#define LegacyDistantFogHeight (cb.fogHeight.x)
#define LegacyHeightFogHeight (cb.fogHeight.y)
#define LegacySimpleFogHeight (cb.fogHeight.z * 1.5)
#define LegacyIsExterior (cb.fogHeight.w)
#define LegacyExtinction (max(0.0, cb.fogSimple.x) * 0.1)
#define LegacyInscattering (max(0.0, cb.fogSimple.y) * 0.1)
#define LegacyFogNight max(0.0, cb.fogSimple.z)
#define LegacySimpleFogSkyColor max(0.0, cb.fogSimple.w)
#define LegacyDistantFogBlend (cb.fogBlend.x)
#define LegacyHeightFogBlend (cb.fogBlend.y)
#define LegacyHeightFogRolloff (cb.fogBlend.z)
#define LegacySimpleFogBlend (cb.fogBlend.w)

#define CompositeAOEndFade 8000.0
#define INTERIOR_SHADOW_MAXDISTANCE (cb.shadowFade.w)

// ---- Includes/Depth.hlsl ----
#define nearZ (cb.cameraData.x)
#define farZ (cb.cameraData.y)

float readDepthLod(vec2 coord) { return LINEAR_LOD(t_depth, coord).x * farZ; }

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

// ---- Includes/Normals.hlsl GetWorldNormalLod ----
vec3 GetWorldNormalLod(vec2 coord) {
    vec4 normal = vec4(LINEAR_LOD(t_normals, coord).xyz * 2.0 - 1.0, 1.0);
    return (normal * cb.viewTransform).xyz;
}

// ---- Includes/Sky.hlsl ----
// (2026-10-26) sunColor and sunsetColor arrive linear (cb.*Lin, linearized on the CPU once a draw)
vec3 GetSunColor(float sunHeight, float athmosphere, float dayTime, vec3 sunColor, vec3 sunsetColor) {
    vec3 color = (1.0 + sunHeight) * sunColor;
    float sunSet = saturate(pows(1.0 - sunHeight, 8.0)) * dayTime;
    color += sunsetColor.rgb * sunSet * athmosphere;
    return color;
}

// (2026-10-26) skyColor, skyLowColor and horizonColor arrive linear (cb.*Lin, linearized on the CPU once a draw)
vec3 GetSkyColor(float verticality, float athmosphere, float sunHeight, float sunInfluence, float sunStrength, vec3 skyColor, vec3 skyLowColor, vec3 horizonColor, vec3 sunColor) {
    float isDayTime = hlslSmoothstep(0.0, 0.5, cb.sunAmount.x);
    vec3 color = mix(skyLowColor.rgb, skyColor.rgb, verticality);
    color = mix(color, horizonColor.rgb, saturate(athmosphere * (0.5 + sunInfluence * 0.5)));
    color += mix(vec3(0.0), sunColor * sunInfluence * (1.0 - sunHeight) * (((1.0 - verticality) + athmosphere) / 2.0) * sunStrength, isDayTime);
    return color;
}

// ---- Includes/Shadows.hlsl, the parts the fog uses ----
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

float GetLightAmountValueEVSM4(vec4 moments, float depth, float bias, float bleedReduction, float formatBits) {
    vec2 exponents = GetEVSMExponents(40.0, 5.0, formatBits);
    vec2 warpedDepth = WarpDepth(depth, exponents);
    vec2 depthScale = bias * exponents * warpedDepth;
    vec2 minVariance = depthScale * depthScale;
    float posContrib = ChebyshevUpperBound(moments.xz, warpedDepth.x, minVariance.x, bleedReduction);
    float negContrib = ChebyshevUpperBound(moments.yw, warpedDepth.y, minVariance.y, bleedReduction);
    return min(posContrib, negContrib);
}

// ---- VolumetricFog.fx.hlsl ----
// PhysicalFogSky (PHYSICAL_SKY_ON)
vec3 PhysicalFogSky(vec3 dir, vec3 gameSky) {
    float horizontal = length(dir.xy), sunHorizontal = length(cb.physSkySun.xy);
    float cosBearing = (horizontal > 1e-4 && sunHorizontal > 1e-4) ? dot(dir.xy, cb.physSkySun.xy) / (horizontal * sunHorizontal) : 1.0;
    vec2 tableUV = vec2(hlslAcos(clamp(cosBearing, -1.0, 1.0)) / 3.14159265, sqrt(hlslAsin(saturate(dir.z)) / 1.57079633));
    vec3 phys = LINEAR_LOD(t_skyView, tableUV).rgb * (0.001 * cb.physSky.y * cb.sunsetColor.w);
    vec3 weatherHue = gameSky / max(luma(gameSky), 1e-6);
    return mix(gameSky, mix(phys, luma(phys) * weatherHue, cb.physSky.z), cb.physSky.x);
}

// FogGridSum, FogGridAir, FogGridLight (the volumetric light's froxel grid)
vec4 FogGridSum(vec2 coord, float depth) {
    float marchDistance = cb.vlData.w;
    float planar = depth >= farZ * 0.996 ? marchDistance : min(depth, marchDistance);
    float s = sqrt(planar / marchDistance) * 64.0;
    float z = min(floor(s), 63.0);
    vec2 gridSize = cb.vlGridLayout.xy;
    vec2 local = clamp(coord * gridSize, vec2(0.5), gridSize - 0.5);
    vec2 farTile = vec2(hlslFmod(z, 8.0), floor(z / 8.0));
    vec2 nearTile = vec2(hlslFmod(z - 1.0, 8.0), floor((z - 1.0) / 8.0));
    vec4 farSide = LINEAR_LOD(t_grid, (farTile * gridSize + local) * cb.vlGridLayout.zw);
    vec4 nearSide = z > 0.5 ? LINEAR_LOD(t_grid, (nearTile * gridSize + local) * cb.vlGridLayout.zw) : vec4(0.0);
    return mix(nearSide, farSide, saturate(s - z));
}

vec2 FogGridAir(vec2 coord, float depth) {
    float marchDistance = cb.vlData.w;
    float planar = depth >= farZ * 0.996 ? marchDistance : min(depth, marchDistance);
    float sigma = cb.vlData.z * length(toWorld(coord));
    return vec2(FogGridSum(coord, depth).r / cb.vlGridData.y, 1.0 - exp(-sigma * planar));
}

vec3 FogGridLight(vec3 gammaColor, vec2 coord) {
    float gridDepth = readDepthLod(coord);
#ifdef FOG_APPLY
    // UNOFFICIAL 2026-11-05 MsaaEdgeFix: the .fx's FogGridDepthMsaa (gridFlags.z = on, t_skyCov the exact sky shares; off, the binding is
    // a stand-in and is not read).
    if (pc.gridFlags.z > 0.5) gridDepth = mix(min(gridDepth, cb.vlData.w), cb.vlData.w, POINT_LOD(t_skyCov, coord).r);
#endif
    vec2 air = FogGridAir(coord, gridDepth);
    float cosTheta = dot(normalize(toWorld(coord)), cb.smoothedSunDir.xyz);
    float g = cb.vlData2.y;
    float phase = min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-4), 1.5), 8.0);
    float fade = cb.vlData2.z;
    vec3 lin = linearize(max(gammaColor, vec3(0.0)));
    float shadedAir = saturate(air.y - air.x);
    lin = lin * (1.0 - cb.vlData.y * fade * shadedAir) + cb.sunColorLin.rgb * (cb.vlData.x * fade * phase * air.x);
    return delinearize(max(lin, vec3(0.0)));
}

// hash, valueNoise, fbm3: master's procedural noise for the Classic look (VolumetricFog.fx.hlsl: hash, noise, fbm3).
// 3D value noise: a random value in [0, 1) at each whole-number lattice point, blended between the 8 corners of the cell
// with a smoothstep curve. Output in [0, 1), mean 0.5, one lattice cell per unit. Written for Vegas Shaders (2026-11-15);
// it replaces upstream's copy of a Shadertoy noise under CC BY-NC-SA 3.0 (not GPL-compatible), with the same range and scale.
// Lattice hash: Dave Hoskins' "Hash without Sine" hash13, MIT, Copyright (c) 2014 David Hoskins (shadertoy.com/view/4djSRW).
float hash(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec3 x) {
    vec3 i = floor(x);
    vec3 u = fract(x);
    u = u * u * (3.0 - 2.0 * u);
    float z0 = mix(mix(hash(i), hash(i + vec3(1, 0, 0)), u.x),
                   mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), u.x), u.y);
    float z1 = mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), u.x),
                   mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), u.x), u.y);
    return mix(z0, z1, u.z);
}

float fbm3(vec3 p) {
    float n = valueNoise(p) * 0.55 + valueNoise(p * 2.13) * 0.3 + valueNoise(p * 4.7) * 0.15;
    return saturate(n * 1.4 - 0.2);
}

vec3 SkyIrradianceLinear(vec3 n) {
    vec3 irradiance = cb.skyIrradiance[0].rgb
         + cb.skyIrradiance[1].rgb * n.y
         + cb.skyIrradiance[2].rgb * n.z
         + cb.skyIrradiance[3].rgb * n.x
         + cb.skyIrradiance[4].rgb * (n.x * n.y)
         + cb.skyIrradiance[5].rgb * (n.y * n.z)
         + cb.skyIrradiance[6].rgb * (3.0 * n.z * n.z - 1.0)
         + cb.skyIrradiance[7].rgb * (n.x * n.z)
         + cb.skyIrradiance[8].rgb * (n.x * n.x - n.y * n.y);
    return max(irradiance, vec3(0.0));
}

vec3 SkyIrradianceGameLinear(vec3 n) {
    vec3 irradiance = cb.skyIrradianceGame[0].rgb
         + cb.skyIrradianceGame[1].rgb * n.y
         + cb.skyIrradianceGame[2].rgb * n.z
         + cb.skyIrradianceGame[3].rgb * n.x
         + cb.skyIrradianceGame[4].rgb * (n.x * n.y)
         + cb.skyIrradianceGame[5].rgb * (n.y * n.z)
         + cb.skyIrradianceGame[6].rgb * (3.0 * n.z * n.z - 1.0)
         + cb.skyIrradianceGame[7].rgb * (n.x * n.z)
         + cb.skyIrradianceGame[8].rgb * (n.x * n.x - n.y * n.y);
    return max(irradiance, vec3(0.0));
}

#define stepnum 32
// getHeightFog: the 32-step sum in closed form (the geometric series), then the distance modifier.
float getHeightFog(float dist, float falloff, vec3 worldPos, float heightOffset) {
    vec3 eyeVector = worldPos - cb.cameraPosition.xyz;
    vec3 stepV = eyeVector / float(stepnum);
    float stepDist = length(stepV);
    float z0 = cb.cameraPosition.z - (FOG_GROUND + heightOffset * 1000.0);
    float x = falloff * stepV.z;
    const float n = float(stepnum);
    float series;
    if (abs(x) < 0.01) {
        float a = n * x;
        float numerator = 1.0 - a / 2.0 + a * a / 6.0 - a * a * a / 24.0 + a * a * a * a / 120.0;
        float denominator = 1.0 - x / 2.0 + x * x / 6.0 - x * x * x / 24.0 + x * x * x * x / 120.0;
        series = n * numerator / denominator;
    }
    else {
        series = (1.0 - exp(-n * x)) / (1.0 - exp(-x));
    }
    float fog = stepDist * exp(-falloff * (z0 + stepV.z)) * series;
    float distanceModifier = saturate(dist / max(length(eyeVector), 0.0001));
    return fog * distanceModifier;
}

void getHeightFogTerms(float dist, vec3 extinctionColor, vec3 inscatteringColor, float density, float falloff, vec3 worldPos, float offset, out vec3 extColor, out vec3 insColor) {
    float fog = density * 0.00001 * getHeightFog(dist, falloff * 0.0001, worldPos, offset);
    extColor = 1.0 - exp(-fog * extinctionColor);
    insColor = 1.0 - exp(-fog * inscatteringColor);
}

vec3 getFogFlat(float dist, vec3 density) {
    return 1.0 - exp(-dist * density * 0.0001);
}

void getFlatFogTerms(float dist, vec3 extinctionColor, vec3 inscatteringColor, float density, out vec3 extColor, out vec3 insColor) {
    extColor = getFogFlat(dist, density * extinctionColor);
    insColor = getFogFlat(dist, density * inscatteringColor);
}

// UNOFFICIAL 2026-11-01 HeightLayer: VolumetricFog.fx.hlsl FogLayerF / FogLayerMean / FogLayerAt line for line (see there; on = fogShape.x < 0)
float FogLayerF(float z) { float h = z - cb.fogShape.y; return h <= 0.0 ? h - 1.0 / (-cb.fogShape.x) : -exp(-h * (-cb.fogShape.x)) / (-cb.fogShape.x); }
float FogLayerMean(float zc, float zp) {
    float dz = zp - zc;
    if (abs(dz) < 1.0) return exp(-max(0.5 * (zc + zp) - cb.fogShape.y, 0.0) * (-cb.fogShape.x));
    return saturate((FogLayerF(zp) - FogLayerF(zc)) / dz);
}
float FogLayerAt(float z) { return exp(-max(z - cb.fogShape.y, 0.0) * (-cb.fogShape.x)); }

// ---- Interior composite: Includes/InteriorShadowApply.hlsl and the 16-bit rounding ----
vec4 ApplyInteriorShadowDepth(vec2 coord, vec4 color, float shadowSample, float depth) {
    color.rgb = pows(color.rgb, 2.2);
    vec3 eyeDir = toWorld(coord);
    float uniformDepth = length(depth * eyeDir);

    float Shadow = saturate(shadowSample);
    Shadow = mix(Shadow, 1.0, invlerps(300.0, INTERIOR_SHADOW_MAXDISTANCE, uniformDepth));
    float darkness = scaledReinhard(cb.shadowData.y, 5.0);
    darkness = saturate(1.0 - darkness * cb.shadowFade.y);

    Shadow = saturate(mix(darkness, 1.0, Shadow));
    float shadowPower = 1.0 - Shadow;

    vec4 finalColor = clamp(Shadow * (pow(clamp(color, 0.0, 1.0), vec4(1.0 - shadowPower * 0.2))), 0.0, 1.0);
    finalColor = mix(vec4(luma(finalColor)), finalColor, 1.0 + shadowPower * 0.3);

    finalColor += max(color - 1.0, vec4(0.0));
    finalColor.rgb = pows(finalColor.rgb, 1.0 / 2.2);

    return vec4(finalColor.rgb, 1.0);
}

vec3 HalfExponent(vec3 a) {
    vec3 e = floor(log2(a));
    vec3 p = exp2(e);
    e += vec3(greaterThanEqual(a, 2.0 * p));
    e -= vec3(lessThan(a, p));
    return max(e, vec3(-14.0));
}

vec3 RoundToHalf(vec3 x) {
    vec3 a = abs(x);
    vec3 e = HalfExponent(a);
    vec3 q = a * exp2(10.0 - e);
    vec3 r = floor(q + 0.5);
    r -= vec3(equal(r - q, vec3(0.5))) * vec3(notEqual(fract(r * 0.5), vec3(0.0)));
    return sign(x) * (r * exp2(e - 10.0));
}

vec3 TruncateToHalf(vec3 x) {
    vec3 a = abs(x);
    vec3 e = HalfExponent(a);
    return sign(x) * (floor(a * exp2(10.0 - e)) * exp2(e - 10.0));
}

vec3 InteriorStoreRound(vec3 color) {
    if (pc.compositeFlags.w < 0.5) color = RoundToHalf(color);
    else if (pc.compositeFlags.w > 1.5) color = TruncateToHalf(color);
    return color;
}

vec4 InteriorStoredColor(vec2 coord) {
    vec4 scene = LINEAR_LOD(t_scene, coord);
    return vec4(InteriorStoreRound(ApplyInteriorShadowDepth(coord, scene, LINEAR_LOD(t_pointShadow, coord).r, readDepthLod(coord)).rgb), scene.a);
}

// ---- the fog ----
float getSky(vec2 coord, bool interior) {
    vec4 scene = LINEAR_LOD(t_scene, coord);
    if (interior) scene = InteriorStoredColor(coord);
    vec4 color = linearize(scene);

    float dist = 0.5 * cb.reciprocalResolution.x;	// (the HLSL's float distance = 0.5 * TESR_ReciprocalResolution.xy: the x, truncated)
    dist *= 1.0;

    vec3 coeffs = vec3(vec2(1.0, -1.0) * dist, 0.9961);
    bool depth = readDepthLod(coord) / farZ >= coeffs.z;
    bool depth1 = readDepthLod(coord + coeffs.xx) / farZ >= coeffs.z;
    bool depth2 = readDepthLod(coord + coeffs.xy) / farZ >= coeffs.z;
    bool depth3 = readDepthLod(coord + coeffs.yx) / farZ >= coeffs.z;
    bool depth4 = readDepthLod(coord + coeffs.yy) / farZ >= coeffs.z;

    float total = float(depth || depth1 || depth2 || depth3 || depth4) * luma(color);
    return saturate(total);
}

vec4 fogColor(vec4 skyColor, vec4 pureFogColor, float fogStrength, float skyColorCoeff, vec4 sunScattering, float saturation) {
    vec4 c = mix(skyColor, pureFogColor, saturate(pows(fogStrength, skyColorCoeff)));
    c += sunScattering;
    c = mix(vec4(luma(c)), c, saturation / (1.0 + fogStrength));
    return c;
}

vec4 ScreenCoordToTexCoord(vec4 coord) {
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5 + 0.5;
    coord.y = coord.y * -0.5 + 0.5;
    return coord;
}

float GetFogShadowValue(mat4 lightTransform, vec4 coord, float offsetX, float offsetY, float bias) {
    vec4 LightSpaceCoord = ScreenCoordToTexCoord(lightTransform * coord);
    LightSpaceCoord.xy *= 0.5;
    LightSpaceCoord.x += offsetX;
    LightSpaceCoord.y += offsetY;

    if (cb.shadowFormatData.x == 3.0) return ShadowPCF2x2(LightSpaceCoord.xy, LightSpaceCoord.z - 0.0002, cb.shadowBlur.x);
    vec4 moments = LINEAR_LOD(t_atlas, LightSpaceCoord.xy);

    float Mode = cb.shadowFormatData.x;
    float FormatBits = cb.shadowFormatData.y;

    if (Mode == 0.0)
        return GetLightAmountValueVSM(moments.xy, LightSpaceCoord.z, bias, 0.2);
    else if (Mode == 1.0)
        return GetLightAmountValueEVSM2(moments.xy, LightSpaceCoord.z, bias, 0.2, FormatBits);
    else
        return GetLightAmountValueEVSM4(moments, LightSpaceCoord.z, bias, 0.2, FormatBits);
}

float GetFogShadowVisibility(vec4 positionWS, vec3 normal) {
    float NdotL = dot(normal, cb.smoothedSunDir.xyz);
    float offsetScale = saturate(1.0 - NdotL);

    vec4 radii = vec4(cb.shadowNearCenter.w, cb.shadowMiddleCenter.w, cb.shadowFarCenter.w, cb.shadowLodCenter.w);
    vec4 texelWorld = 4.0 * radii * max(cb.shadowBlur.x, 1.0 / 16384.0);
    vec4 offsetDistance = offsetScale * 2.5 * texelWorld;

    float bias = (cb.shadowFormatData.x == 0.0 ? 0.00001 : 0.01) * (1.0 + offsetScale);
    const float blend = 0.9;

    vec4 shadows = vec4(
        GetFogShadowValue(cb.shadowNear,   vec4(positionWS.xyz + offsetDistance.x * normal, 1.0), 0.0, 0.0, bias),
        GetFogShadowValue(cb.shadowMiddle, vec4(positionWS.xyz + offsetDistance.y * normal, 1.0), 0.5, 0.0, bias),
        GetFogShadowValue(cb.shadowFar,    vec4(positionWS.xyz + offsetDistance.z * normal, 1.0), 0.0, 0.5, bias),
        GetFogShadowValue(cb.shadowLod,    vec4(positionWS.xyz + offsetDistance.w * normal, 1.0), 0.5, 0.5, bias));

    vec4 distances = vec4(
        length(positionWS.xyz - cb.shadowNearCenter.xyz),
        length(positionWS.xyz - cb.shadowMiddleCenter.xyz),
        length(positionWS.xyz - cb.shadowFarCenter.xyz),
        length(positionWS.xyz - cb.shadowLodCenter.xyz));

    if (distances.x < cb.shadowNearCenter.w) {
        if (distances.x < cb.shadowNearCenter.w * blend) return shadows.x;
        return mix(shadows.x, shadows.y, hlslSmoothstep(cb.shadowNearCenter.w * blend, cb.shadowNearCenter.w, distances.x));
    }
    else if (distances.y < cb.shadowMiddleCenter.w) {
        if (distances.y < cb.shadowMiddleCenter.w * blend) return shadows.y;
        return mix(shadows.y, shadows.z, hlslSmoothstep(cb.shadowMiddleCenter.w * blend, cb.shadowMiddleCenter.w, distances.y));
    }
    else if (distances.z < cb.shadowFarCenter.w) {
        if (distances.z < cb.shadowFarCenter.w * blend) return shadows.z;
        return mix(shadows.z, shadows.w, hlslSmoothstep(cb.shadowFarCenter.w * blend, cb.shadowFarCenter.w, distances.z));
    }
    else if (distances.w < cb.shadowLodCenter.w) {
        if (distances.w < cb.shadowLodCenter.w * blend) return shadows.w;
        return mix(shadows.w, 1.0, hlslSmoothstep(cb.shadowLodCenter.w * blend, cb.shadowLodCenter.w, distances.w));
    }
    return 1.0;
}

// The legacy fog (custom build)
float getLegacyHeightFog(float dist, float falloff, vec3 worldPos, float heightOffset) {
    vec3 eyeVector = worldPos - cb.cameraPosition.xyz;
    vec3 stepV = eyeVector / float(stepnum);
    float stepDist = length(stepV);

    float z0 = cb.cameraPosition.z - (FOG_GROUND + heightOffset * 1000.0);
    float x = falloff * stepV.z;
    const float n = float(stepnum);
    float series;
    if (abs(x) < 0.01) {
        float a = n * x;
        float numerator = 1.0 - a / 2.0 + a * a / 6.0 - a * a * a / 24.0 + a * a * a * a / 120.0;
        float denominator = 1.0 - x / 2.0 + x * x / 6.0 - x * x * x / 24.0 + x * x * x * x / 120.0;
        series = n * numerator / denominator;
    }
    else {
        series = (1.0 - exp(-n * x)) / (1.0 - exp(-x));
    }
    float fog = stepDist * exp(-falloff * (z0 + stepV.z)) * series;
    return fog * (length(eyeVector) / max(dist, 0.0001));
}

void LegacyFogTerms(vec2 coord, out vec3 fogMul, out vec3 fogAdd, out float nightAmountOut, bool interior) {
    vec4 pureFogColor = cb.fogColorLin;

    float depth = readDepthLod(coord);
    float isDayTime = hlslSmoothstep(0.4, 0.8, cb.sunAmount.x);
    float isDayTimeFog = hlslSmoothstep(0.1, 0.6, cb.sunAmount.x);
    float SunsetFog = sin(cb.sunAmount.x * PI) * 0.001 * LegacyIsExterior;

    vec3 eyeVector = toWorld(coord);
    vec3 eyeDirection = normalize(eyeVector);
    eyeVector *= depth;
    vec3 worldPos = eyeVector + cb.cameraPosition.xyz;
    float pointHeight = worldPos.z;

    float normalizedDepth = length(eyeVector) / farZ;
    float fogPower = mix(1.0, FogPower, saturate(LegacyWeatherImpact)) / mix(LegacyHeightFogRolloff, 1.0 / LegacyFogNight, (1.0 - isDayTimeFog) * LegacyIsExterior);
    float fogDepth = pows(normalizedDepth, fogPower);
    fogDepth = fogDepth * farZ / (LegacyHeightFogDist * mix(0.005, 1.0, LegacyIsExterior));

    float strength = pows((saturate(1.0 - farFog / farZ) + saturate(1.0 - nearFog / farZ)) / 2.0, 2.0) / (FogPower + 1.0);
    strength = LegacyBaseFogStrength + LegacyWeatherImpact * strength + SunsetFog;
    float heightFade = exp(-pointHeight / (80000.0 * LegacySimpleFogHeight));

    float distantFog = 0.0, distantHeightFade = 0.0;
    vec4 skyColor = pureFogColor;
    float inScattering = LegacyInscattering;
    float extinction = LegacyExtinction;
    vec4 sun = vec4(0.0, 0.0, 0.0, 1.0);

    if (LegacyIsExterior != 0.0) {
        vec3 up = vec3(0.0, 0.0, 1.0);
        float sunHeight = shade(cb.sunPosition.xyz, up);
        float sunDir = dot(eyeDirection, cb.sunPosition.xyz);
        float sunInfluence = pows(compress(sunDir), SUNINFLUENCE);
        vec4 sunColor = vec4(GetSunColor(sunHeight, 1.0, cb.sunAmount.x, cb.sunDiskColorLin.rgb, cb.sunsetColorLin.rgb), 1.0);
        sunColor *= isDayTime * LegacyIsExterior;

        skyColor.rgb = GetSkyColor(0.5, 1.0, sunHeight, sunInfluence, cb.skyData.z, cb.skyColorLin.rgb, cb.skyLowColorLin.rgb, cb.horizonColorLin.rgb, vec3(0.0)) * cb.sunsetColor.w;
        if (FOG_PHYSICAL_ON) {
            if (cb.physSky.x > 0.0) skyColor.rgb = PhysicalFogSky(eyeDirection, skyColor.rgb);
        }

        float sunStrength = cb.fogData.z / (1.0 + 2.0 * strength);
        sunStrength = mix(sunStrength, max(sunStrength * 2.0, 10.0), normalizedDepth);

        float sunScattering = pows(compress(sunDir), 2.0 + sunStrength);
        sunScattering *= pow(1.0 - sunHeight, 2.0) * isDayTimeFog * cb.fogData.z;
        sun = sunColor * sunScattering * LegacySunPower * 100.0;

        distantFog = pows(hlslSmoothstep(LegacyDistantFogRange, 1.0, normalizedDepth), 0.5);
        if (LegacyDistantFogHeight == 0.0) distantHeightFade = 1.0 - float(getSky(coord, interior) > 0.0);
        else distantHeightFade = saturate(exp(-pointHeight / (80000.0 * LegacyDistantFogHeight)));    // UNOFFICIAL 2026-10-30: at most 1, as below

        inScattering += SunsetFog;
        extinction *= 1.0 - distantFog * isDayTimeFog;
    }

    vec4 simpleFogColor = fogColor(skyColor, pureFogColor, strength, LegacySimpleFogSkyColor, vec4(0.0, 0.0, 0.0, 1.0), LegacyFogSaturation);
    float simpleDensity = strength * heightFade;
    float simpleExt = getFogFlat(fogDepth, vec3(simpleDensity * extinction)).x;
    float simpleIns = getFogFlat(fogDepth, vec3(simpleDensity * inScattering)).x;
    float simpleBlend = saturate(LegacySimpleFogBlend);
    fogMul = vec3(mix(1.0, saturate(1.0 - simpleExt), simpleBlend));
    fogAdd = simpleFogColor.rgb * saturate(simpleIns) * simpleBlend;

    float distantBlend = distantFog * saturate(LegacyDistantFogBlend) * distantHeightFade * LegacyIsExterior;
    fogMul *= 1.0 - distantBlend;
    fogAdd = mix(fogAdd, skyColor.rgb, distantBlend);

    vec4 heightFogColor = fogColor(skyColor, pureFogColor, strength, LegacyHeightFogSkyColor, sun, LegacyHeightFogSaturation);
    float heightFog = strength * LegacyHeightFogDensity * 0.00001 *
        getLegacyHeightFog(fogDepth, 1.5 / (fogPower * LegacyHeightFogFalloff) * 0.0001, worldPos, LegacyHeightFogHeight);
    float heightBlend = saturate(LegacyHeightFogBlend);
    float heightScale = mix(1.0, saturate(1.0 - heightFog * extinction), heightBlend);
    fogMul *= heightScale;
    fogAdd = fogAdd * heightScale + heightFogColor.rgb * saturate(heightFog * LegacyHeightFogInscattering) * heightBlend;

    fogMul = mix(vec3(1.0), fogMul, LegacyFogAmount);
    fogAdd *= LegacyFogAmount;
    nightAmountOut = LegacyFogAmount;
}

void ModernFogTerms(vec2 coord, out vec3 fogMul, out vec3 fogAdd, out float nightAmountOut, bool interior, bool classic) {
    vec4 pureFogColor = cb.fogColorLin;

    float depth = readDepthLod(coord);
    float isDayTime = hlslSmoothstep(0.4, 0.8, cb.sunAmount.x);
    float isDayTimeFog = hlslSmoothstep(0.1, 0.6, cb.sunAmount.x);
    float nightFactor = isExterior * (1.0 - isDayTime);

    vec3 eyeVector = toWorld(coord);
    vec3 eyeDirection = normalize(eyeVector);
    eyeVector *= depth;
    vec3 worldPos = eyeVector + cb.cameraPosition.xyz;

    float normalizedDepth = length(eyeVector) / farZ;
    float fogPower = mix(1.0, FogPower, saturate(WeatherImpact));
    float fogDepth = pows(normalizedDepth, fogPower) * farZ;

    float isSky = getSky(coord, interior);
    float skyMaskFactor = mix(1.0, 1.0 - isSky, WeatherFilterBlend);
    skyMaskFactor = mix(skyMaskFactor, 1.0, nightFactor * NightDisableSkyMask);

    float isSkyDome = float(depth / farZ >= 0.9961);

    float vanillaStrength = pows((saturate(1.0 - farFog / farZ) + saturate(1.0 - nearFog / farZ)) / 2.0, 2.0) / (FogPower + 1.0);

    float sunsetBump = sin(saturate(cb.sunAmount.x) * PI) * isExterior;
    float noonTrough = 1.0 - saturate(abs(cb.gameTime.y - 12.0) / 6.0);
    float timeOfDayScale = mix(1.0, mix(1.0, 1.0 - MorningFogDip, noonTrough), isExterior);
    float nightDensityScale = mix(1.0, NightDensityScale, nightFactor);

    float nightWindSpeed = WindSpeed * mix(1.0, NightWindSpeedScale, nightFactor);
    vec3 windOffset = vec3(WindDirection * nightWindSpeed * cb.gameTime.x * 0.002, 0.0);
    float noiseVal = 0.0;
    vec4 fogVolume = vec4(0.0);
    // (ClassicLook: master's noise instead; the HLSL compiles the rest out of the *Classic techniques, here it is skipped.)
    if (classic) noiseVal = fbm3((worldPos + windOffset) / (1500.0 * NoiseScale));
#ifdef FOG_TEST_NO_NOISE
    else if (true) noiseVal = 0.5;   // TEST variant (variants.txt, NvrDxvkTestSuffix): no noise reads
#endif
    else {
        if (pc.gridFlags.x > 0.5 && cb.vlGridData.x > 0.5) {
            fogVolume = FogGridSum(coord, depth);
            noiseVal = fogVolume.g > 1.0e-6 ? fogVolume.b / fogVolume.g : 0.5;
        }
        else {
            float noiseReach = min(length(eyeVector), 12000.0);
            float invNoiseScale = 1.0 / (1500.0 * NoiseScale);   // UNOFFICIAL 2026-10-26: once, not three divisions a step (fxc: rcp + mul)
            float noiseWeight = 0.0;
            for (int noiseStep = 0; noiseStep < 4; noiseStep++) {
                float noiseT = mix(min(300.0, noiseReach), noiseReach, (float(noiseStep) + 0.5) * 0.25);
                vec3 noisePos = (cb.cameraPosition.xyz + eyeDirection * noiseT + windOffset) * invNoiseScale;
                // UNOFFICIAL 2026-11-01 HeightLayer (the .fx line for line)
                float w = cb.fogShape.x < 0.0 ? FogLayerAt(cb.cameraPosition.z + eyeDirection.z * noiseT) : 1.0;
                noiseVal += w * textureLod(sampler3D(t_noise, NVR_SAMPLER(pc.wrapIndex)), noisePos * 0.25, 0.0).g;
                noiseWeight += w;
            }
            noiseVal = noiseWeight > 0.0 ? noiseVal / noiseWeight : 0.5;
        }
        noiseVal = saturate((noiseVal - 0.5) * 1.8 + 0.5);
    }
    float nightNoiseStrength = NoiseStrength * mix(1.0, NightNoiseStrengthScale, nightFactor);

    float noiseSkyMask = 1.0 - isSkyDome;
    float nvrDensity = BaseDensity * timeOfDayScale * nightDensityScale * mix(1.0, noiseVal, nightNoiseStrength * noiseSkyMask);
    nvrDensity += SunriseSunsetBoost * sunsetBump;

    float strength = max(0.0, nvrDensity + WeatherImpact * vanillaStrength);

    float shadowVisibility = 1.0;
#ifdef FOG_TEST_NO_SHADOW
    if (false) {   // TEST variant: no shadow visibility
#else
    if (isExterior > 0.5 && ShadowStrength > 0.0 && cb.shadowFade.y > 0.5) {
#endif
        if (!classic && fogVolume.g > 1.0e-6) {
            shadowVisibility = mix(1.0, saturate(fogVolume.a / fogVolume.g), ShadowStrength);
        }
        else if (!classic && pc.gridFlags.x > 0.5) {
            vec2 air = FogGridAir(coord, depth);
            shadowVisibility = mix(1.0, air.y > 1.0e-6 ? saturate(air.x / air.y) : 1.0, ShadowStrength);
        }
        else {
            vec3 worldNormal = GetWorldNormalLod(coord);
            shadowVisibility = mix(1.0, GetFogShadowVisibility(vec4(worldPos, 1.0), worldNormal), ShadowStrength);
        }
    }

    float distantFog = 0.0, distantHeightFade = 0.0;
    vec4 skyColor = pureFogColor;
    vec4 sun = vec4(0.0, 0.0, 0.0, 1.0);

    if (isExterior != 0.0) {
        vec3 up = vec3(0.0, 0.0, 1.0);
        float sunHeight = shade(cb.sunPosition.xyz, up);
        float sunDir = dot(eyeDirection, cb.sunPosition.xyz);
        float sunInfluence = pows(compress(sunDir), SUNINFLUENCE);
        vec4 sunColorV = vec4(GetSunColor(sunHeight, 1.0, cb.sunAmount.x, cb.sunDiskColorLin.rgb, cb.sunsetColorLin.rgb), 1.0);
        sunColorV *= isDayTime * isExterior;

#ifndef FOG_TEST_NO_SKY   // (TEST variant: no sky colour, GetSkyColor and PhysicalFogSky)
        skyColor.rgb = GetSkyColor(0.5, 1.0, sunHeight, sunInfluence, cb.skyData.z, cb.skyColorLin.rgb, cb.skyLowColorLin.rgb, cb.horizonColorLin.rgb, vec3(0.0)) * cb.sunsetColor.w;
        if (FOG_PHYSICAL_ON && !classic) {
            if (cb.physSky.x > 0.0) skyColor.rgb = PhysicalFogSky(eyeDirection, skyColor.rgb);
        }
#endif

        float sunStrength = cb.fogData.z / (1.0 + 2.0 * strength);
        sunStrength = mix(sunStrength, max(sunStrength * 2.0, 10.0), normalizedDepth);

        float sunScattering = pows(compress(sunDir), mix(8.0, 1.0, PhaseAsymmetry) + sunStrength);
        sunScattering *= pow(1.0 - sunHeight, 2.0) * isDayTimeFog * cb.fogData.z;
        sun = sunColorV * sunScattering * shadowVisibility * cb.pbrData.z * 100.0 * (classic ? 1.0 : cb.fogSun.x);

        distantFog = pows(hlslSmoothstep(DistantFogRange, 1.0, normalizedDepth), 0.5);
        // UNOFFICIAL 2026-10-30 (VolumetricFog.fx.hlsl, claude/terrain-flicker 80b1dd4b): at most 1 (below z 0 the exp passed 1 and the sky lerp
        // ran past the sky colour: white, then inf in the TAA history)
        distantHeightFade = (DistantFogHeight == 0.0) ? (1.0 - isSky) : saturate(exp(-worldPos.z / (80000.0 * DistantFogHeight)));
    }

    vec3 ambientColor = SkyAmbientAvailable > 0.5
        ? (classic ? SkyIrradianceGameLinear(vec3(0.0, 0.0, 1.0)) : SkyIrradianceLinear(vec3(0.0, 0.0, 1.0)))
        : cb.skyColorLin.rgb;
    ambientColor *= cb.pbrData.w;

    vec3 nightAmbient = cb.sunAmbientLin.rgb * MoonVisibility * NightAmbientStrength * nightFactor;
    ambientColor = max(ambientColor, nightAmbient);

    // UNOFFICIAL 2026-10-30 WeatherFogColor (VolumetricFog.fx.hlsl ModernFogTerms): the weather's own fog colour (fogColorLin, the
    // fog's copy with the sky look in it) on the weather's side of this blend, by fogSun.y (0 = the sky's colour, as before;
    // not on the sky dome, where the fog over the sky keeps the sky's colour; ClassicLook: 0, as master)
    vec3 weatherColor = mix(skyColor.rgb, pureFogColor.rgb * cb.sunsetColor.w, classic ? 0.0 : cb.fogSun.y * (1.0 - isSkyDome));
    vec4 ambientSkyColor = vec4(mix(ambientColor, weatherColor, saturate(WeatherImpact)), 1.0);

    vec4 fogColorFinal = fogColor(ambientSkyColor, pureFogColor, strength, FogSkyColorCoeff, sun, FogSaturation);
    fogColorFinal.rgb *= mix(vec3(1.0), NightTint, nightFactor);

    float nightExtinction = Extinction * mix(1.0, NightExtinctionScale, nightFactor);
    float nightInscattering = Inscattering * mix(1.0, NightInscatteringScale, nightFactor);
    float nightHeightFalloff = HeightFalloff * mix(1.0, NightHeightFalloffScale, nightFactor);
    float nightMaxHeight = MaxHeight + NightMaxHeightOffset * nightFactor;

    vec3 flatExt, flatIns, heightExt, heightIns;
    getFlatFogTerms(fogDepth, vec3(nightExtinction), vec3(nightInscattering), strength, flatExt, flatIns);
    float falloffArg = 1.5 / (fogPower * nightHeightFalloff);
    // UNOFFICIAL 2026-11-01 HeightLayer (the .fx line for line)
    if (!classic && cb.fogShape.x < 0.0)
        getFlatFogTerms(fogDepth, vec3(nightExtinction), vec3(nightInscattering), strength * FogLayerMean(cb.cameraPosition.z, worldPos.z), heightExt, heightIns);
    else
        getHeightFogTerms(fogDepth, vec3(nightExtinction), vec3(nightInscattering), strength, falloffArg, worldPos, nightMaxHeight, heightExt, heightIns);

    vec3 blendedExt = mix(flatExt, heightExt, HeightInfluence);
    vec3 blendedIns = mix(flatIns, heightIns, HeightInfluence);

    vec3 finalExt = max(blendedExt, flatExt * MinDensityFloor);
    vec3 finalIns = max(blendedIns, flatIns * MinDensityFloor);

    fogMul = mix(vec3(1.0), saturate(1.0 - finalExt), skyMaskFactor);
    fogAdd = fogColorFinal.rgb * saturate(finalIns) * skyMaskFactor;

    float aerialDayFade = hlslSmoothstep(0.1, AerialDayFadeStart, cb.sunAmount.x);
    float aerialFactor = hlslSmoothstep(AerialRangeStart, 1.0, normalizedDepth) * (1.0 - isSkyDome) * isExterior;
    vec3 aerialTint = mix(ambientColor, cb.fogAerialTintLin.rgb, AerialTintBlend);

    vec3 aerialTintClamped = saturate(aerialTint);
    vec3 aerialScale = mix(vec3(1.0), aerialTintClamped, aerialFactor * AerialStrength * skyMaskFactor * aerialDayFade);
    fogMul *= aerialScale;
    fogAdd *= aerialScale;

    float distantBlend = distantFog * saturate(DistantFogBlend) * distantHeightFade * isExterior;
    fogMul *= 1.0 - distantBlend;
    fogAdd = mix(fogAdd, skyColor.rgb, distantBlend);

    float edgeBlend = isSky * EdgeAA * isExterior;
    fogMul *= 1.0 - edgeBlend;
    fogAdd = mix(fogAdd, skyColor.rgb, edgeBlend);

    float nightAmount = FogAmount * mix(1.0, NightAmountScale, nightFactor);
    fogMul = mix(vec3(1.0), fogMul, nightAmount);
    fogAdd *= nightAmount;
    nightAmountOut = nightAmount;

    // UNOFFICIAL 2026-10-30 GameFogFloor (VolumetricFog.fx.hlsl ModernFogTerms): at least the game's own fog curve for the weather,
    // times cb.fogSun.z, with the weather's raw near (w); toward NVR's fog colour without the sun's glow; not on the sky dome.
    // UNOFFICIAL 2026-11-15 FogWall (VolumetricFog.fx.hlsl ModernFogTerms): indoors the fade into the cell's fog (its curve times a fade
    // ending at min(fog far, clip), over the outer half of the way from the fog near), the background past the clip included, toward the cell's fog colour.
    // cb.fogSun.x indoors: 1 / (min(fog far, clip) - near), 0 without fog distances (VolumetricFog.cpp UpdateConstants).
    if (!classic && cb.fogSun.z > 0.0) {
        float gameNear = cb.fogSun.w;
        float dist = length(eyeVector);
        float tOut = farFog > gameNear + 1.0 ? saturate((dist - gameNear) / (farFog - gameNear)) : 0.0;
        float tIn = saturate((dist - gameNear) * cb.fogSun.x);
        float t = mix(tIn, tOut, isExterior);
        float gameFog = pows(t, FogPower);
        float clipT = saturate(t * 2.0 - 1.0);
        float clipFade = mix(clipT * clipT * (3.0 - 2.0 * clipT), 1.0, isExterior);
        float want = saturate(cb.fogSun.z * gameFog * clipFade) * (1.0 - isSkyDome * isExterior);
        float kept = luma(fogMul);
        float topUp = saturate((want - (1.0 - kept)) / max(kept, 0.0001));
        vec3 haze = mix(pureFogColor.rgb, fogColor(ambientSkyColor, pureFogColor, strength, FogSkyColorCoeff, vec4(0.0), FogSaturation).rgb * mix(vec3(1.0), NightTint, nightFactor), isExterior);
        fogMul *= 1.0 - topUp;
        fogAdd = mix(fogAdd, haze, topUp);
    }
}

void FogTerms(vec2 coord, out vec3 fogMul, out vec3 fogAdd, out float nightAmountOut, bool legacy, bool interior, bool classic) {
    if (legacy)
        LegacyFogTerms(coord, fogMul, fogAdd, nightAmountOut, interior);
    else
        ModernFogTerms(coord, fogMul, fogAdd, nightAmountOut, interior, classic);
}

#ifdef FOG_APPLY
// FogApply: the depth-aware upsample of the half-resolution coefficients, applied to a gamma-space colour.
vec3 FogApply(vec3 gammaColor, vec2 coord, bool legacy, bool interior, bool classic) {
    vec2 texel = pc.fogLayout.zw;
    vec2 position = coord / texel - 0.5;
    vec2 fraction = fract(position);
    vec2 baseUV = (floor(position) + 0.5) * texel;
    vec2 minUV = texel * 0.5;
    vec2 maxUV = 1.0 - minUV;

    vec2 uv00 = clamp(baseUV, minUV, maxUV);
    vec2 uv10 = clamp(baseUV + vec2(texel.x, 0.0), minUV, maxUV);
    vec2 uv01 = clamp(baseUV + vec2(0.0, texel.y), minUV, maxUV);
    vec2 uv11 = clamp(baseUV + texel, minUV, maxUV);
    vec4 s00 = POINT_LOD(t_fogMul, uv00);
    vec4 s10 = POINT_LOD(t_fogMul, uv10);
    vec4 s01 = POINT_LOD(t_fogMul, uv01);
    vec4 s11 = POINT_LOD(t_fogMul, uv11);

    float depth = saturate(readDepthLod(coord) / farZ);
    float tolerance = max(depth * 0.02, 0.0005);
    vec4 difference = abs(vec4(s00.a, s10.a, s01.a, s11.a) - depth) / tolerance;
    vec4 spatial = vec4((1.0 - fraction.x) * (1.0 - fraction.y), fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y, fraction.x * fraction.y);
    vec4 weights = spatial / (1.0 + difference * difference * 8.0);
    weights /= max(dot(weights, vec4(1.0)), 0.00001);
    vec3 fogMul = s00.rgb * weights.x + s10.rgb * weights.y + s01.rgb * weights.z + s11.rgb * weights.w;
    vec3 fogAdd = POINT_LOD(t_fogAdd, uv00).rgb * weights.x + POINT_LOD(t_fogAdd, uv10).rgb * weights.y +
        POINT_LOD(t_fogAdd, uv01).rgb * weights.z + POINT_LOD(t_fogAdd, uv11).rgb * weights.w;
    float bestDifference = min(min(difference.x, difference.y), min(difference.z, difference.w));
    if (bestDifference > 3.0) {
        float nightAmount;
        FogTerms(coord, fogMul, fogAdd, nightAmount, legacy, interior, classic);
    }

    vec3 color = linearize(gammaColor);
    return delinearize(max(color * fogMul + fogAdd, vec3(0.0)));
}

// ---- the composites' shadow and AO applies ----
vec3 CompositeSunShadow(vec3 linearColor, vec2 coord) {
    linearColor *= mix(vec3(1.0), pows(POINT_LOD(t_sunFar, coord).rgb, 2.2), pc.sunFarFlags.x);
    if (cb.waterSettings.z == 1.0) {
        float depth = readDepthLod(coord);
        vec3 worldPos = cb.cameraPosition.xyz + toWorld(coord) * depth;
        vec3 worldNormal = GetWorldNormalLod(coord);
        if (worldPos.z < (cb.waterSettings.x + 2.0) && worldPos.z > (cb.waterSettings.x - 2.0) && dot(worldNormal, vec3(0.0, 0.0, -1.0)) > 0.999)
            return linearColor;
    }

    float darkness = max(0.0, 1.0 - cb.shadowData.y);
    vec2 shadow = LINEAR_LOD(t_pointShadow, coord).rg;
    shadow.r = mix(cb.shadowFade.x, 1.0, shadow.r);
    float ambient = mix(1.0, luma(cb.sunAmbient), darkness * cb.shadowFade.z);
    shadow.r = mix(0.0, ambient, shadow.r);
    shadow.r += shadow.g;
    shadow.r = saturate(mix(darkness, 1.0, shadow.r));

    vec3 skyColor = pows(cb.skyColor.rgb, 2.2);
    vec3 colorShadow = luma(linearColor) * shadow.r * skyColor;
    colorShadow = mix(colorShadow, linearColor * shadow.r, saturate(shadow.r + 0.5));
    return max(vec3(0.0), colorShadow);
}

// Includes/SkyTake.hlsl
vec3 SkyTake(vec3 linearColor, float factor, float visibility, float cap, float noLimit) {
    float take = min(1.0 - factor, ((1.0 - visibility) * cap + noLimit) / max(luma(linearColor), 1.0e-6));
    return linearColor * (1.0 - take);
}

vec3 CompositeAO(vec3 linearColor, vec2 coord) {
    float depth = readDepthLod(coord);
    if (depth >= CompositeAOEndFade) return linearColor;

    vec2 texel = pc.compositeAOTexel.xy;
    vec2 position = coord / texel - 0.5;
    vec2 base = floor(position);
    vec2 fraction = fract(position);
    float blurDrop = cb.aoData.z;
    float sum = 0.0, weights = 0.0;
    vec2 skySum = vec2(0.0);
    bool sky = pc.compositeFlags.z > 0.5;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 sampleUV = clamp((base + vec2(x, y) + 0.5) * texel, 0.5 * texel, 1.0 - 0.5 * texel);
            vec2 aoSample = LINEAR_LOD(t_ao, sampleUV).rg;
            float weight = (x != 0 ? fraction.x : 1.0 - fraction.x) * (y != 0 ? fraction.y : 1.0 - fraction.y);
            weight /= 1.0 + abs(aoSample.y - depth) / max(blurDrop, 0.001);
            sum += aoSample.x * weight;
            weights += weight;
            if (sky) skySum += POINT_LOD(t_skyVis, sampleUV).rb * weight;
        }
    }
    float ao = mix(cb.aoAOData.z, 1.0, sum / max(weights, 1.0e-6));
    ao = mix(ao, 1.0, saturate((luma(linearColor) - cb.aoData.y) * 3.0));
    if (sky) { vec2 skyAt = saturate(skySum / max(weights, 1.0e-6)); return SkyTake(linearColor * ao, skyAt.x, skyAt.y, pc.compositeAOTexel.z, pc.compositeAOTexel.w); }
    return linearColor * ao;
}
#endif
