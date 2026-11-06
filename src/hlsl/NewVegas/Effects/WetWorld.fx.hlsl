// WetWorld fullscreen shader for Oblivion/NewVegas Reloaded
// Original contributors: Boomstick and Mac
// UNOFFICIAL 2026-10-13: shared with this build by its authors and used as is (rain splashes, streaks down walls, puddles
// in hollows, cloud-lit puddles, puddle SSR); only the texture paths take single backslashes like the other effects.
// Rain splashes, wet surfaces, puddles and screen-space reflections.
// Shader Model 3.0; requires the engine Includes directory.

// Engine parameters
float4 TESR_SunDirection;
float4 TESR_ReciprocalResolution;
float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformOrtho;

float4 TESR_GameTime;
float4 TESR_SkyColor;
float4 TESR_HorizonColor;
float4 TESR_SunColor;
float4 TESR_SunAmbient;
float4 TESR_OrthoData;
float4 TESR_WetWorldCoeffs;
float4 TESR_WaterSettings;
float4 TESR_WetWorldData;
// UNOFFICIAL 2026-10-16 x: KeepHighlights. This pass writes every pixel while it rains (the sky too, through the far branch) into
// the HDR scene before the tonemapper; saturate() cut everything brighter than 1 (sun, bright sky, lit wet highlights). On: only
// negative values are cut, so what this pass does not change reaches the tonemapper as it was.
float4 TESR_WetWorldLook;
float3 WetOut(float3 c) { return TESR_WetWorldLook.x > 0.5f ? max(c, 0.0f) : saturate(c); }
float4 TESR_SunPosition;
float4 TESR_SkyData;
float4 TESR_SkyLowColor;
float4 TESR_SunAmount;
float4 TESR_LightPosition[12];
float4 TESR_ShadowLightPosition[12];
float4 TESR_LightColor[24];

// Texture samplers
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RippleSampler : register(s3) < string ResourceName = "Precipitations\ripples.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_OrthoMapBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = NONE; MINFILTER = NONE; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBufferViewModel : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SplashSampler : register(s8) < string ResourceName = "Precipitations\splash.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SplashNormalSampler : register(s9) < string ResourceName = "Precipitations\splash_n.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_PuddleCloudSampler : register(s10) < string ResourceName = "water\water_d.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_PuddleNormalSampler : register(s11) < string ResourceName = "water\water_nrm.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_WaterStreakSampler : register(s12) < string ResourceName = "Precipitations\water-streaks_n.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-27 Persistent ([Shaders.WetWorld.Main] Persistent, CoverMap.h): wetness and puddle water kept per spot. Declared
// last (samplers bind in declaration order); only techniques 5 and 6 read it.
sampler2D TESR_CoverMapBuffer : register(s13) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#include "Includes/CoverMap.hlsl"
// UNOFFICIAL 2026-11-05 Model 1 ([Shaders.WetWorld.Main] Model; techniques 7 to 10, WetPBR below): what each pixel is made of, written by the
// mod's lit templates while the world draws (core/CapturedNormals.h; Shaders/Includes/CapturedNormals.hlsl): r = class + 0.49 x metal
// (1 object, 2 worn gear, 3 cloth, 4 land, 5 plant, 6 skin), b = roughness (-1 none); and the linear base colour (a 1 = written). Declared
// after s13 (samplers bind in declaration order). Techniques 0 to 6 do not read them.
sampler2D TESR_MaterialBuffer : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_AlbedoBuffer : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_MaterialBufferData;	// x: 1 = TESR_MaterialBuffer holds this frame's
float4 TESR_AlbedoBufferData;	// x: 1 = TESR_AlbedoBuffer holds this frame's

// General settings
static const float PI = 3.14159265;
static const float DrawD = TESR_OrthoData.x * 0.9;
static const float rippleScale = 120.0f;
static const float refractionScale = 50.0f;
static const float time1 = TESR_GameTime.z * 0.96f;
static const float time2 = TESR_GameTime.z * 0.97f;
static const float time3 = TESR_GameTime.z * 0.98f;
static const float time4 = TESR_GameTime.z * 0.99f;

// Splash settings
static const float SplashMaxDist    = 2000.0f;
static const float SplashFadeStart  = 1200.0f;
static const float SplashWorldScale = 8.0f;
static const float SplashAnimSpeed  = 1.35f;
static const float SplashIntensity  = 0.17f;

// Vertical streak settings
static const float2 StreakWorldSize = float2(64.0f, 90.0f);
static const float StreakFlowSpeed = 12.0f;
static const float StreakFlowMinMultiplier = 0.45f;
static const float StreakFlowMaxMultiplier = 1.65f;
static const float StreakFlowGroups = 8.0f;
static const float StreakNormalStrength = 0.25f;
static const float StreakNormalYSign = -1.0f;
static const float StreakMaskStrength = 1.0f;
static const float StreakDarken = 0.45f;
static const float StreakSpecularStrength = 1.20f;
static const float StreakSunSpecularPower = 80.0f;
static const float StreakHighlightLimit = 0.25f;
static const float StreakFadeStart = 800.0f;
static const float StreakFadeFull = 1000.0f;

// Puddle coverage settings
static const float PuddleAccumulationStart = 0.08f;
static const float PuddleAccumulationFull = 1.0f;
static const float PuddleBlurRadius = 12.0f;
static const float FlatPuddleBlurStrength = 0.0f;
static const float PuddleBlurDepthTolerance = 0.08f;
static const float CurvatureWorldRadius = 100.0f;
static const float CurvatureDepthSign = 1.0f;
static const float CurvatureSurfaceTolerance = 0.0005f;
static const float DepressionMinCurvature = 0.25f;
static const float DepressionFullCurvature = 2.0f;
static const float DepressionSlopeFadeStart = 0.906f;
static const float DepressionSlopeCutoff = 0.707f;
static const float DepressionMaxDepthJump = 80.0f;

// Puddle surface settings
static const float PuddleNormalStrength = 0.25f;
static const float FlatPuddleNormalStrength = 0.15f;
static const float PuddleNormalYSign = 1.0f;
static const float PuddleCloudStrength = 0.38f;
static const float PuddleCloudWorldSize = 256.0f;
static const float PuddleCloudBrightness = 1.65f;
static const float PuddleCloudContrast = 15.0f;
static const float PuddleCloudRippleDistortion = 0.035f;
static const float2 PuddleCloudDrift = float2(-0.02f, -0.01f);

// Puddle lighting settings
static const float PuddleSkyStrength   = 0.55f;
static const float PuddleSkyMinReflect = 0.12f;
static const float PuddleSkyMaxBlend   = 0.28f;
static const float PuddleSkyBrightness = 0.55f;
static const float PuddleSkyTintInfluence = 0.25f;
static const float PuddleSunStrength   = 2.25f;
static const float PuddleLightStrength = 2.50f;
static const float PuddleSunLimit      = 1.95f;
static const float PuddleLightLimit    = 0.90f;
static const float PuddleSunHighlightLimit = 1.90f;
static const float PuddlePointHighlightLimit = 1.8f;
static const float PuddleSkyLumaFloor = 0.015f;
static const float PuddleSkyLumaScale = 0.65f;

// Screen-space reflection settings
static const float PuddleSSRCandidateBlend = 0.55f;
static const float PuddleSSRMinTraceOpacity = 0.005f;
static const float PuddleSSRScreenFadeStart = 1.0f / 3.0f;
static const float PuddleSSRScreenFadeFull = 2.0f / 3.0f;
static const float PuddleSSRFadeStart = 0.20f;
static const float PuddleSSRFadeFull = 0.40f;
static const int PuddleSSRSteps = 8;
static const float PuddleSSRMaxDistance = 1800.0f;
static const float PuddleSSRSurfaceBias = 3.0f;
static const float PuddleSSRNormalInfluence = 0.25f;
static const int PuddleSSRRefinementSteps = 2;
static const float PuddleSSRRefinementTolerance = 12.0f;
// UNOFFICIAL 2026-10-17 FixedReflectionTrace: settings of the corrected hit test (see TraceWetReflection)
static const float PuddleSSRThickness = 24.0f;			// game units a hit may lie behind the depth surface (plus 30% of the step bracket)
static const float PuddleSSRThicknessPerStep = 0.30f;
static const float PuddleSSREdgeFade = 0.05f;			// the reflection fades to nothing over this share of the screen at its borders

// Shader types
struct VSOUT
{
    float4 vertPos : POSITION;
    float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
    float4 vertPos : POSITION0;
    float2 UVCoord : TEXCOORD0;
};

// Shared includes
#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Blur.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Sky.hlsl"
#include "Includes/PBR.hlsl"

// Surface reconstruction and exposure
float SampleOrthoDepth(float2 uv)
{
    return tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0.0f, 0.0f)).r;
}

float GetRoofMask(float4 OrthoPos)
{
    OrthoPos.xyz /= OrthoPos.w;

    float outofbounds =
    (OrthoPos.x < -1.0f || OrthoPos.x > 1.0f ||
        OrthoPos.y < -1.0f || OrthoPos.y > 1.0f ||
        OrthoPos.z <  0.0f || OrthoPos.z > 1.0f);

    OrthoPos.x = OrthoPos.x *  0.5f + 0.5f;
    OrthoPos.y = OrthoPos.y * -0.5f + 0.5f;

    float Ortho = SampleOrthoDepth(OrthoPos.xy);

    return (Ortho > OrthoPos.z - 0.0001f) * (1.0f - outofbounds);
}

float4 ScreenCoordToTexCoord(float4 coord, float mult)
{
    coord.xyz /= coord.w;
    coord.x = coord.x * 0.5f + 0.5f;
    coord.y = coord.y * -0.5f + 0.5f;
    coord *= mult;
    return coord;
}

float4 GetOrthoSurfacePosition(float2 screenUV)
{
    float3 viewPosition = reconstructPosition(screenUV);
    float3 relativeWorldPosition = mul(float4(viewPosition, 0.0f), TESR_InvViewTransform).xyz;
    return float4(relativeWorldPosition, 1.0f);
}

// Puddle placement
float GetOrthoDepthScale()
{
    float3 depthAxis = float3(TESR_ShadowCameraToLightTransformOrtho[0][2],
        TESR_ShadowCameraToLightTransformOrtho[1][2],
        TESR_ShadowCameraToLightTransformOrtho[2][2]);
    return max(length(depthAxis) / max(abs(TESR_ShadowCameraToLightTransformOrtho[3][3]),
        0.000001f), 0.00000001f);
}

float2 GetCurvatureSampleRadius()
{
    float3 axisX = float3(TESR_ShadowCameraToLightTransformOrtho[0][0],
        TESR_ShadowCameraToLightTransformOrtho[1][0],
        TESR_ShadowCameraToLightTransformOrtho[2][0]);
    float3 axisY = float3(TESR_ShadowCameraToLightTransformOrtho[0][1],
        TESR_ShadowCameraToLightTransformOrtho[1][1],
        TESR_ShadowCameraToLightTransformOrtho[2][1]);
    float invW = 1.0f / max(abs(TESR_ShadowCameraToLightTransformOrtho[3][3]), 0.000001f);
    return max(float2(length(axisX), length(axisY))
        * (0.5f * CurvatureWorldRadius * invW), 0.000001f.xx);
}

// First-person masks
float GetViewModelMask(float2 screenUV)
{
    float vmDepthX = tex2D(TESR_DepthBufferViewModel, screenUV).x;
    return ((TESR_DepthConstants.z == 0 && vmDepthX < 0.9f) ||
        (TESR_DepthConstants.z > 0 && vmDepthX > 0.01f)) ? 1.0f : 0.0f;
}

float GetPuddleViewModelExclusion(float2 uv, float centerMask)
{
    float2 pixel = TESR_ReciprocalResolution.xy;
    float mask = centerMask;
    mask = max(mask, GetViewModelMask(uv + float2(pixel.x, 0)));
    mask = max(mask, GetViewModelMask(uv - float2(pixel.x, 0)));
    mask = max(mask, GetViewModelMask(uv + float2(0, pixel.y)));
    mask = max(mask, GetViewModelMask(uv - float2(0, pixel.y)));
    return mask;
}

// Puddle coverage helper
float ComputeDepressionMask(float4 relativeSurface, float viewDepth, float vmMask)
{
    float4 mapPos = ScreenCoordToTexCoord(
        mul(relativeSurface, TESR_ShadowCameraToLightTransformOrtho), 1.0f);
    float2 uv = mapPos.xy;
    float2 radius = GetCurvatureSampleRadius();
    float valid = step(radius.x, uv.x) * step(uv.x, 1.0f - radius.x)
    * step(radius.y, uv.y) * step(uv.y, 1.0f - radius.y)
    * step(0.0f, mapPos.z) * step(mapPos.z, 1.0f);

    float c = SampleOrthoDepth(uv);
    float left = SampleOrthoDepth(uv + float2(-radius.x, 0));
    float right = SampleOrthoDepth(uv + float2(radius.x, 0));
    float top = SampleOrthoDepth(uv + float2(0, -radius.y));
    float bottom = SampleOrthoDepth(uv + float2(0, radius.y));
    float2 diagonalRadius = radius * 0.70710678f;
    float d1 = SampleOrthoDepth(uv - diagonalRadius);
    float d2 = SampleOrthoDepth(uv + diagonalRadius);
    float d3 = SampleOrthoDepth(uv + float2(-diagonalRadius.x, diagonalRadius.y));
    float d4 = SampleOrthoDepth(uv + float2(diagonalRadius.x, -diagonalRadius.y));
    float depthScale = GetOrthoDepthScale();
    float meanRing = (left + right + top + bottom + d1 + d2 + d3 + d4) * 0.125f;
    float signedCurve = (c - meanRing) * CurvatureDepthSign / depthScale;
    float concave = smoothstep(DepressionMinCurvature, DepressionFullCurvature, signedCurve);

    float2 depthGradient = float2(right - left, bottom - top) / (2.0f * radius);
    float3 clipGradient = float3(-0.5f * depthGradient.x, 0.5f * depthGradient.y, 1.0f);
    float3 heightNormal = mul((float3x3)TESR_ShadowCameraToLightTransformOrtho, clipGradient);
    heightNormal /= max(length(heightNormal), 0.000001f);
    float slopeFilter = smoothstep(DepressionSlopeCutoff, DepressionSlopeFadeStart, abs(heightNormal.z));

    float depthJump = max(max(abs(left - c), abs(right - c)),
        max(abs(top - c), abs(bottom - c))) / depthScale;
    float edgeFilter = 1.0f - smoothstep(DepressionMaxDepthJump * 0.5f, DepressionMaxDepthJump, depthJump);
    concave *= slopeFilter;
    concave *= edgeFilter;

    float surfaceMatch = 1.0f - smoothstep(CurvatureSurfaceTolerance,
        CurvatureSurfaceTolerance * 2.0f, abs(mapPos.z - c));
    valid *= surfaceMatch * step(0.0f, viewDepth);
    concave *= valid * (1.0f - vmMask);
    return concave;
}

// Rain ripple helper
float3 ComputeRipple(float2 UV, float CurrentTime, float Weight)
{
    float4 Ripple = tex2D(TESR_RippleSampler, UV);
    Ripple.yz = expand(Ripple.yz);

    float period     = frac(Ripple.w + CurrentTime);
    float TimeFrac   = period - 1.0f + Ripple.x;
    float DropFactor = saturate(0.2f + Weight * 0.8f - period);
    float FinalFactor = DropFactor * Ripple.x * sin(clamp(TimeFrac * 9.0f, 0.0f, 3.0f) * PI);

    return float3(Ripple.yz * FinalFactor * 0.35f, 1.0f);
}

// Rain splash helpers
float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p)
{
    float n = hash12(p);
    return float2(n, hash12(p + n * 19.19f));
}

float4 SampleSplash(float2 worldXY, float time, float density, out float splashAlpha, out float2 splashNXY)
{
    float2 cell = floor(worldXY / SplashWorldScale);
    float r = hash12(cell);

    float spawn = step(1.0f - density, r);
    float phase = frac(time * SplashAnimSpeed + r * 7.13f);

    float fade = smoothstep(0.00f, 0.10f, phase) * (1.0f - smoothstep(0.85f, 1.00f, phase));

    float frame = floor(phase * 16.0f);
    float2 frameXY = float2(fmod(frame, 4.0f), floor(frame / 4.0f));

    float2 local = frac(worldXY / SplashWorldScale);
    float2 jitter = (hash22(cell + 13.0f) - 0.5f) * 0.25f;
    local = frac(local + jitter);

    float2 uv = (local + frameXY) * 0.25f;

    float4 s = tex2D(TESR_SplashSampler, uv);

    float3 nTex = tex2D(TESR_SplashNormalSampler, uv).xyz;
    float2 nXY = nTex.xy * 2.0f - 1.0f;
    splashNXY = normalize(float3(nXY, 1)).xy;

    float a = saturate((s.a - 0.25f) * 1.6f);
    splashAlpha = a * spawn * fade;

    return s;
}

// Variable streak flow helper
float4 SampleScrollingStreaks(float2 uv, float flowTravel, float seed, float2 uvDx, float2 uvDy)
{
    float laneCoord = uv.x * max(StreakFlowGroups, 1.0f);
    float lane = floor(laneCoord);
    float blend = smoothstep(0.40f, 0.60f, frac(laneCoord));
    float2 randomSpeed = float2(hash12(float2(lane, seed)),
        hash12(float2(lane + 1.0f, seed)));
    float minSpeed = max(min(StreakFlowMinMultiplier, StreakFlowMaxMultiplier), 0.0f);
    float maxSpeed = max(max(StreakFlowMinMultiplier, StreakFlowMaxMultiplier), minSpeed);
    float2 speed = lerp(minSpeed, maxSpeed, randomSpeed);
    float2 uvA = uv - float2(0.0f, flowTravel * speed.x);
    float2 uvB = uv - float2(0.0f, flowTravel * speed.y);
    float4 sampleA = tex2Dgrad(TESR_WaterStreakSampler, uvA, uvDx, uvDy);
    float4 sampleB = tex2Dgrad(TESR_WaterStreakSampler, uvB, uvDx, uvDy);
    float alpha = lerp(sampleA.a, sampleB.a, blend);
    float3 rgb = lerp(sampleA.rgb * sampleA.a, sampleB.rgb * sampleB.a, blend)
        / max(alpha, 0.000001f);
    return float4(rgb, alpha);
}

// Vertical streak coordinates
float4 GetWaterStreakUVs(float3 surfacePosition, float3 surfaceNormal)
{
    float2 side = float2(surfaceNormal.x < 0.0f ? -1.0f : 1.0f,
        surfaceNormal.y < 0.0f ? -1.0f : 1.0f);
    float2 worldSize = max(StreakWorldSize, 1.0f.xx);
    float verticalUV = -surfacePosition.z / worldSize.y;
    float2 uvX = float2(surfacePosition.y * side.x / worldSize.x, verticalUV);
    float2 uvY = float2(-surfacePosition.x * side.y / worldSize.x + 0.37f, verticalUV + 0.19f);
    return float4(uvX, uvY);
}

// Vertical streak helper
float4 SampleWaterStreaks(float4 streakUV, float3 surfaceNormal, float4 uvDx, float4 uvDy)
{
    float2 projectionWeights = pow(abs(surfaceNormal.xy), 4.0f);
    projectionWeights /= max(projectionWeights.x + projectionWeights.y, 0.000001f);
    float2 side = float2(surfaceNormal.x < 0.0f ? -1.0f : 1.0f,
        surfaceNormal.y < 0.0f ? -1.0f : 1.0f);
    float2 worldSize = max(StreakWorldSize, 1.0f.xx);
    float flowTravel = TESR_GameTime.z * StreakFlowSpeed / worldSize.y;
    float4 sampleX = SampleScrollingStreaks(streakUV.xy, flowTravel, 17.0f, uvDx.xy, uvDy.xy);
    float4 sampleY = SampleScrollingStreaks(streakUV.zw, flowTravel, 53.0f, uvDx.zw, uvDy.zw);
    float2 normalX = sampleX.xy * 2.0f - 1.0f;
    float2 normalY = sampleY.xy * 2.0f - 1.0f;
    normalX.y *= StreakNormalYSign;
    normalY.y *= StreakNormalYSign;
    float2 coverage = saturate(float2(sampleX.a, sampleY.a) * StreakMaskStrength) * projectionWeights;
    float3 offsetX = float3(0.0f, normalX.x * side.x, -normalX.y);
    float3 offsetY = float3(-normalY.x * side.y, 0.0f, -normalY.y);
    float3 offset = offsetX * coverage.x + offsetY * coverage.y;
    offset -= surfaceNormal * dot(surfaceNormal, offset);
    return float4(offset * StreakNormalStrength, coverage.x + coverage.y);
}

// Screen-space reflection helpers
float3 GetSSRViewPosition(float2 uv)
{
    float4 d = tex2Dlod(TESR_DepthBuffer, float4(uv, 0, 0));
    float2 s = rectToScreen(uv);	// UNOFFICIAL 2026-10-27 RenderScale v2: NDC from the screen position
    float4 p = mul(float4(s.x * 2.0f - 1.0f, 1.0f - s.y * 2.0f,
        0.5f, 1.0f), TESR_InvProjectionTransform);
    float safeW = (p.w < 0.0f ? -1.0f : 1.0f) * max(abs(p.w), 0.000001f);
    float3 viewPosition = p.xyz / safeW;
    viewPosition *= (d.x * farZ) / max(abs(viewPosition.z), 0.000001f);
    return viewPosition;
}

float3 TraceWetReflection(float2 sourceUV, float3 waterNormal)
{
    float3 selectedHit = float3(0, 0, 0);
    float3 origin = GetSSRViewPosition(sourceUV);
    float4 sourceClip = mul(float4(origin, 1.0f), TESR_ProjectionTransform);
    if (abs(sourceClip.w) < 0.0001f || abs(origin.z) < 0.0001f)
        return float3(0, 0, 0);
    float viewSign = origin.z < 0.0f ? -1.0f : 1.0f;
    float clipSign = sourceClip.w < 0.0f ? -1.0f : 1.0f;
    float3 viewNormal = normalize(mul(float4(waterNormal, 0.0f), TESR_ViewTransform).xyz);

    if (dot(viewNormal, origin) > 0.0f) viewNormal = -viewNormal;
    float3 rayDirection = normalize(reflect(normalize(origin), viewNormal));
    origin += viewNormal * PuddleSSRSurfaceBias;
    float4 projectedOrigin = mul(float4(origin, 1.0f), TESR_ProjectionTransform);
    float4 projectedDirection = mul(float4(rayDirection, 0.0f), TESR_ProjectionTransform);
    float previousDelta = -PuddleSSRRefinementTolerance;
    float previousTravel = 0.0f;
    [loop]
    for (int ssrStep = 1; ssrStep <= PuddleSSRSteps; ssrStep++)
    {
        float fraction = (float)ssrStep / (float)PuddleSSRSteps;
        float travel = 8.0f + PuddleSSRMaxDistance * fraction * fraction;
        float3 probe = origin + rayDirection * travel;
        float4 projected = projectedOrigin + projectedDirection * travel;
        if (projected.w * clipSign <= 0.0001f || probe.z * viewSign <= 0.0001f) break;
        float2 hitS = projected.xy / projected.w * float2(0.5f, -0.5f) + 0.5f;	// UNOFFICIAL 2026-10-27 RenderScale v2: screen position, read in the rect
        if (hitS.x <= 0.001f || hitS.x >= 0.999f || hitS.y <= 0.001f || hitS.y >= 0.999f) break;
        float2 hitUV = screenToRect(hitS);
        float4 depthSample = tex2Dlod(TESR_DepthBuffer, float4(hitUV, 0, 0));
        float delta = probe.z * viewSign - depthSample.x * farZ;
        if (depthSample.x > 0.0f && depthSample.x < 0.99999f
            && delta >= 0.0f && previousDelta < 0.0f)
        {
            float lowerTravel = previousTravel;
            float upperTravel = travel;
            [loop]
            for (int refinement = 0; refinement < PuddleSSRRefinementSteps; refinement++)
            {
                float midTravel = (lowerTravel + upperTravel) * 0.5f;
                float3 midProbe = origin + rayDirection * midTravel;
                float4 midClip = projectedOrigin + projectedDirection * midTravel;
                float2 midUV = screenToRect(midClip.xy / midClip.w * float2(0.5f, -0.5f) + 0.5f);
                float midZ = tex2Dlod(TESR_DepthBuffer, float4(midUV, 0, 0)).x * farZ;
                float midDelta = midProbe.z * viewSign - midZ;
                if (midDelta >= 0.0f)
                {
                    upperTravel = midTravel;

                    if (midDelta <= PuddleSSRRefinementTolerance) break;
                }
                else lowerTravel = midTravel;
            }
            float3 hitProbe = origin + rayDirection * upperTravel;
            float4 hitClip = projectedOrigin + projectedDirection * upperTravel;
            float2 refinedS = hitClip.xy / hitClip.w * float2(0.5f, -0.5f) + 0.5f;	// UNOFFICIAL 2026-10-27 RenderScale v2
            float2 refinedUV = screenToRect(refinedS);
            float hitDepth = tex2Dlod(TESR_DepthBuffer, float4(refinedUV, 0, 0)).x;
            float hitDelta = hitProbe.z * viewSign - hitDepth * farZ;
            float vmHitDepth = tex2Dlod(TESR_DepthBufferViewModel, float4(refinedUV, 0, 0)).x;
            bool hitViewModel = (TESR_DepthConstants.z == 0 && vmHitDepth < 0.9f)
            || (TESR_DepthConstants.z > 0 && vmHitDepth > 0.01f);
            float2 pixelSeparation = (refinedUV - sourceUV) / TESR_ReciprocalResolution.xy;
            bool validHitDepth = hitDepth > 0.0f && hitDepth < 0.99999f;
            bool separatedHit = dot(pixelSeparation, pixelSeparation) >= 16.0f;

            if (hitDelta >= 0.0f && validHitDepth && !hitViewModel && separatedHit)
            {
                float edge = min(min(refinedS.x, 1.0f-refinedS.x), min(refinedS.y, 1.0f-refinedS.y));
                float edgeVisibility = smoothstep(0.0f, 0.02f, edge);
                float distanceVisibility = max(1.0f - smoothstep(PuddleSSRMaxDistance * 0.65f,
                    PuddleSSRMaxDistance, upperTravel), 0.25f);
                if (edgeVisibility * distanceVisibility > 0.0f)
                    return float3(0, 0, 0);
            }

            selectedHit = float3(0, 0, 0);
            if (validHitDepth && !hitViewModel && separatedHit && !(hitDelta < 0.0f))
                selectedHit = float3(refinedUV, 1.0f);
        }

        previousDelta = delta;
        previousTravel = travel;
    }
    return selectedHit;
}

// UNOFFICIAL 2026-10-17 FixedReflectionTrace (setting [Shaders.WetWorld.Main] FixedReflectionTrace, off by default; compiled as
// separate techniques so the original program stays what it was).
// FOUND IN GAME (puddlessr debug maps, 2026-10-17): in the original trace above, none of the break / return statements inside the
// march loop stops anything on the real GPU path (the D3DX listing writes every exit as 'break_ne -c, -c', a comparison of a value with
// itself, i.e. never taken; the early 'return float3(0,0,0)' is therefore a flag that later iterations overwrite). The march always
// runs all steps, samples past the screen border as if they were hits (clamped depth), keeps the LAST bracket it finds instead of
// the first, and the edge / distance fade and the 'return no reflection' never act: puddles show hard-edged full-strength copies of
// the screen (clamped border pixels where the ray left it), which flip under any recompile.
// This version has no break / return inside a loop: a done flag guards each step, the refinement always runs both halvings, and
// the result is written once. Hit test: first depth crossing, thickness bound (McGuire and Mara, Efficient GPU Screen-Space Ray
// Tracing, JCGT 2014: a ray that passed behind a thin occluder is not a hit; the allowance grows with the step bracket because the
// 2-step refinement only locates the surface to a quarter of it), fade to nothing towards the screen border and with distance
// (Stachowiak, Stochastic Screen-Space Reflections, SIGGRAPH 2015). xy = hit UV, z = visibility (a weight, 0 = miss: the puddle
// keeps its analytic sky/cloud colour as before).
float3 TraceWetReflectionFixed(float2 sourceUV, float3 waterNormal)
{
    float3 result = float3(0, 0, 0);
    float3 origin = GetSSRViewPosition(sourceUV);
    float4 sourceClip = mul(float4(origin, 1.0f), TESR_ProjectionTransform);
    float viewSign = origin.z < 0.0f ? -1.0f : 1.0f;
    float clipSign = sourceClip.w < 0.0f ? -1.0f : 1.0f;
    float3 viewNormal = normalize(mul(float4(waterNormal, 0.0f), TESR_ViewTransform).xyz);
    if (dot(viewNormal, origin) > 0.0f) viewNormal = -viewNormal;
    float3 rayDirection = normalize(reflect(normalize(origin), viewNormal));
    origin += viewNormal * PuddleSSRSurfaceBias;
    float4 projectedOrigin = mul(float4(origin, 1.0f), TESR_ProjectionTransform);
    float4 projectedDirection = mul(float4(rayDirection, 0.0f), TESR_ProjectionTransform);
    float previousDelta = -PuddleSSRRefinementTolerance;
    float previousTravel = 0.0f;
    bool done = abs(sourceClip.w) < 0.0001f || abs(origin.z) < 0.0001f;
    [loop]
    for (int ssrStep = 1; ssrStep <= PuddleSSRSteps; ssrStep++)
    {
        float fraction = (float)ssrStep / (float)PuddleSSRSteps;
        float travel = 8.0f + PuddleSSRMaxDistance * fraction * fraction;
        float3 probe = origin + rayDirection * travel;
        float4 projected = projectedOrigin + projectedDirection * travel;
        float2 hitS = projected.xy / max(abs(projected.w), 0.0001f) * float2(0.5f, -0.5f) + 0.5f;	// UNOFFICIAL 2026-10-27 RenderScale v2
        bool onScreen = projected.w * clipSign > 0.0001f && probe.z * viewSign > 0.0001f
            && hitS.x > 0.001f && hitS.x < 0.999f && hitS.y > 0.001f && hitS.y < 0.999f;
        float2 hitUV = screenToRect(hitS);
        float4 depthSample = tex2Dlod(TESR_DepthBuffer, float4(hitUV, 0, 0));
        float delta = probe.z * viewSign - depthSample.x * farZ;
        // one branch per step; 'done' (a hit was taken, or the ray left the screen) replaces the loop exits that do not work
        [branch]
        if (!done && onScreen && depthSample.x > 0.0f && depthSample.x < 0.99999f && delta >= 0.0f && previousDelta < 0.0f)
        {
            float lowerTravel = previousTravel;
            float upperTravel = travel;
            [unroll]
            for (int refinement = 0; refinement < PuddleSSRRefinementSteps; refinement++)
            {
                float midTravel = (lowerTravel + upperTravel) * 0.5f;
                float3 midProbe = origin + rayDirection * midTravel;
                float4 midClip = projectedOrigin + projectedDirection * midTravel;
                float2 midUV = screenToRect(midClip.xy / midClip.w * float2(0.5f, -0.5f) + 0.5f);
                float midZ = tex2Dlod(TESR_DepthBuffer, float4(midUV, 0, 0)).x * farZ;
                bool behind = midProbe.z * viewSign - midZ >= 0.0f;
                upperTravel = behind ? midTravel : upperTravel;
                lowerTravel = behind ? lowerTravel : midTravel;
            }
            float3 hitProbe = origin + rayDirection * upperTravel;
            float4 hitClip = projectedOrigin + projectedDirection * upperTravel;
            float2 refinedS = hitClip.xy / hitClip.w * float2(0.5f, -0.5f) + 0.5f;	// UNOFFICIAL 2026-10-27 RenderScale v2
            float2 refinedUV = screenToRect(refinedS);
            float hitDepth = tex2Dlod(TESR_DepthBuffer, float4(refinedUV, 0, 0)).x;
            float hitDelta = hitProbe.z * viewSign - hitDepth * farZ;
            float vmHitDepth = tex2Dlod(TESR_DepthBufferViewModel, float4(refinedUV, 0, 0)).x;
            bool hitViewModel = (TESR_DepthConstants.z == 0 && vmHitDepth < 0.9f)
            || (TESR_DepthConstants.z > 0 && vmHitDepth > 0.01f);
            float2 pixelSeparation = (refinedUV - sourceUV) / TESR_ReciprocalResolution.xy;
            bool validHitDepth = hitDepth > 0.0f && hitDepth < 0.99999f;
            bool separatedHit = dot(pixelSeparation, pixelSeparation) >= 16.0f;
            float thickness = PuddleSSRThickness + PuddleSSRThicknessPerStep * (travel - previousTravel);
            float edge = min(min(refinedS.x, 1.0f - refinedS.x), min(refinedS.y, 1.0f - refinedS.y));
            float edgeVisibility = smoothstep(0.0f, PuddleSSREdgeFade, edge);
            float distanceVisibility = max(1.0f - smoothstep(PuddleSSRMaxDistance * 0.65f,
                PuddleSSRMaxDistance, upperTravel), 0.25f);
            float visibility = edgeVisibility * distanceVisibility;
            if (hitDelta >= 0.0f && hitDelta <= thickness && validHitDepth && !hitViewModel && separatedHit && visibility > 0.0f)
            {
                result = float3(refinedUV, visibility);
                done = true;
            }
        }
        done = done || !onScreen;
        previousDelta = delta;
        previousTravel = travel;
    }
    return result;
}

// Vertex shader
VSOUT FrameVS(VSIN IN)
{
    VSOUT OUT = (VSOUT)0.0f;
    OUT.vertPos = IN.vertPos;
    OUT.UVCoord = IN.UVCoord;
    return OUT;
}

// Wet-map pass
float4 WetMap(VSOUT IN) : COLOR0
{
    float depth = readDepth(IN.UVCoord);
    if (depth > DrawD)
        return float4(0, 0, 0, 1);
    float4 surfacePosition = GetOrthoSurfacePosition(IN.UVCoord);
    float vmMask = GetViewModelMask(IN.UVCoord);
    float coverage = ComputeDepressionMask(surfacePosition, depth, vmMask);
    float dotUp = dot(GetWorldNormal(IN.UVCoord), float3(0, 0, 1));
    float4 roofPosition = mul(surfacePosition, TESR_ShadowCameraToLightTransformOrtho);
    float flatCoverage = GetRoofMask(roofPosition) * smoothstep(0.65f, 0.92f, dotUp)
        * (1.0f - vmMask);
    float flatWetCoverage = flatCoverage * smoothstep(0.90f, 0.97f, dotUp);
    return float4(coverage, flatCoverage, flatWetCoverage, 1.0f);
}

// Wet-map blur pass
float4 BlurWetMap(VSOUT IN, uniform float2 OffsetMask, uniform float blurRadius) : COLOR0
{
    float2 uv = IN.UVCoord;
    float centerDepth = readDepth(uv);
    float2 blurStep = TESR_ReciprocalResolution.xy * OffsetMask * max(blurRadius, 0.0f) * 0.5f;
    float depthTolerance = max(PuddleBlurDepthTolerance * max(centerDepth, 1.0f), 0.0001f);
    float weightSum = 0.375f;
    float3 coverage = tex2D(TESR_RenderedBuffer, uv).rgb * weightSum;
    [unroll]
    for (int tap = -2; tap <= 2; tap++)
    {
        if (tap != 0)
        {
            float2 sampleUV = clampToRectIfOn(saturate(uv + blurStep * (float)tap), TESR_ReciprocalResolution.xy);	// UNOFFICIAL 2026-10-27 RenderScale v2
            float sampleDepth = readDepth(sampleUV);
            float depthWeight = 1.0f - smoothstep(depthTolerance * 0.5f,
                depthTolerance, abs(sampleDepth - centerDepth));
            float kernelWeight = abs(tap) == 1 ? 0.25f : 0.0625f;
            float weight = kernelWeight * depthWeight;
            coverage += tex2D(TESR_RenderedBuffer, sampleUV).rgb * weight;
            weightSum += weight;
        }
    }
    coverage /= weightSum;
    return float4(coverage, 1.0f);
}

// UNOFFICIAL 2026-10-17 FastPaths helpers (see Wet below)
float2 GetPuddleCloudUV(float4 worldPosRelRoof, float3 combinedNormals)
{
    float3 cloudSurface = TESR_CameraPosition.xyz
    + worldPosRelRoof.xyz;
    float2 cloudUV = cloudSurface.xy / max(PuddleCloudWorldSize, 1.0f);
    cloudUV += TESR_GameTime.z * PuddleCloudDrift;
    cloudUV += combinedNormals.xy * PuddleCloudRippleDistortion;
    return cloudUV;
}

// One lamp's highlight, as the main path's loop body; the half vector and the lobe only where the lamp reaches the pixel
// (att > 0: elsewhere both terms are x * 0). (Not one uniform branch per slot: 24 of them need more than the 16 boolean
// registers and pushed the pass past ps_3_0's 224 float constants. The whole loop sits behind one 'any lamp' boolean.)
void AddLampHighlightFast(float4 lightPos, float3 c, float3 worldPos, float3 V, float3 N, float specMask, float streakMask,
    inout float3 pointSpecCol, inout float3 streakPointCol)
{
    float3 L = lightPos.xyz - worldPos;
    float dist = length(L);
    float d = dist / max(lightPos.w, 1e-4);
    float att = saturate(1.0f - d);
    att *= att;
    att = lightPos.w > 0.0f ? att : 0.0f;	// an empty slot (w = 0) adds nothing, as the main path's test on w
    [branch]
    if (att > 0.0f)
    {
        L /= max(dist, 1e-4);
        float3 H = normalize(L - V);

        float ndh = saturate(dot(N, H));
        float specLobe = pow(ndh, 30.0f);
        float spec = specLobe * specMask;

        pointSpecCol += c * spec * att;
        float streakSpec = specLobe * streakMask * StreakSpecularStrength;
        streakPointCol += c * streakSpec * att;
    }
}

// Wet-surface pass
// UNOFFICIAL 2026-10-17 FastPaths ([Shaders.WetWorld.Main] FastPaths: technique 2 compiles this with fast = true, technique 0
// is the original, byte for byte). The compiler had flattened every early return of this pass and the 24-lamp loop (cmp
// selects), so every pixel, sky included, ran all of it (~1600 instructions). With fast = true the same results are reached
// through real branches:
//  - the far/water return and the first-person return branch for real; so does a zero-weight return: a pixel that is not
//    first-person, with LODfade*wetMask == 0, no streak and no SSR trace, ends exactly as WetOut(base + splashAdd), alpha 1;
//  - the puddle colour (sky, clouds, sun and lamp highlights) is only built where LODfade*wetMask > 0 (its only way in);
//  - the lamp loop runs only when a lamp slot is in use (one uniform boolean), and per pixel only where the lamp's
//    attenuation is above 0 (an unlit lamp adds x * 0);
// Every texture read of the original (all tex2D with implicit derivatives: ripples, puddle normals, cloud, refraction,
// first-person mask, shadow buffer, puddle exclusion) still runs, the same way, on every pixel before the first new branch;
// the branches only skip arithmetic (and the SSR trace, which was already a branch).
// UNOFFICIAL 2026-10-27 persist ([Shaders.WetWorld.Main] Persistent; techniques 5 and 6, FastPaths with and without the fixed trace):
// how wet the flat ground is and how full the puddles are come from the cover map (Includes/CoverMap.hlsl: per spot, built up where
// the rain reached, dried by the sun) instead of the one value for the whole world (the rain fade and TESR_WetWorldData.z). The
// ripples, splashes and wall streaks still follow the rain that falls now. With persist = false every line is as before.
float4 Wet(VSOUT IN, uniform bool fast, uniform bool fixTrace, uniform bool persist) : COLOR0
{
    // Surface reconstruction and weather
    float4 baseColor = tex2D(TESR_SourceBuffer, IN.UVCoord);

    float depth = readDepth(IN.UVCoord);

    float4 worldPosRelRoof = GetOrthoSurfacePosition(IN.UVCoord);
    float4 orthoRoofPos = mul(worldPosRelRoof, TESR_ShadowCameraToLightTransformOrtho);
    float roofMask = GetRoofMask(orthoRoofPos);

    float3 eyeDirection = toWorld(IN.UVCoord);
    float3 camera_vector = eyeDirection * depth;
    float4 worldPos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);

    float3 normal = GetWorldNormal(IN.UVCoord);
    float3 up = float3(0, 0, 1);

    float floorAngle = smoothstep(0.94, 1, dot(normal, up));
    eyeDirection = normalize(eyeDirection);
    float3 V = normalize(eyeDirection);

    float rainAmt  = saturate(TESR_WetWorldData.x / max(TESR_WetWorldData.y, 0.0001f));
    float rainFade = smoothstep(0.05f, 0.35f, rainAmt);
    float rainVis = rainFade * roofMask;

    // First-person surface mask
    float vmDepthX = tex2D(TESR_DepthBufferViewModel, IN.UVCoord).x;
    float vmMask = GetViewModelMask(IN.UVCoord);
    float puddleVMExclude = GetPuddleViewModelExclusion(IN.UVCoord, vmMask);

    float2 d = float2(ddx(vmDepthX), ddy(vmDepthX));
    float grad = length(d);
    float vmFlatMask = 1.0f - saturate(grad * 80.0f);
    vmFlatMask = smoothstep(0.55f, 0.90f, vmFlatMask);

    // Rain splashes
    float2 splashBumpXY = float2(0.0f, 0.0f);

    float dotUp = dot(normal, up);
    float splashDistFade = 1.0f - smoothstep(SplashFadeStart, SplashMaxDist, depth);
    float splashFlatMask = smoothstep(0.7071f, 0.92f, dotUp);

    float2 splashXY = worldPos.xy;

    float rand = hash12(floor(worldPos.xy * 0.5f));

    float worldSizeVar = lerp(0.75f, 1.35f, rand);
    float2 worldSplashXY = splashXY / worldSizeVar;

    float vmSizeVar = lerp(0.70f, 1.30f, rand);

    float vmScale = 0.28f;

    float vmSplashSizeMul = 1.0f;

    float2 vmSplashXY = (splashXY / (vmScale * vmSplashSizeMul)) * vmSizeVar;

    splashXY = lerp(worldSplashXY, vmSplashXY, vmMask);

    float splashAlpha;
    float2 splashNXY;
    float4 splashSample = SampleSplash(splashXY, TESR_GameTime.z, (rainAmt * roofMask) * 0.395f, splashAlpha, splashNXY);
    splashAlpha *= roofMask;

    float splashStrength = 2.2f;
    splashStrength *= splashAlpha;
    splashStrength *= splashFlatMask;

    splashStrength *= saturate(0.5f + 0.5f * rainVis);
    splashBumpXY = splashNXY * splashStrength;

    float vmIntensity = lerp(1.0f, 0.90f, vmMask);

    float3 splashRGB = splashSample.rgb;
    splashRGB = saturate((splashRGB - 0.5f) * 1.6f + 0.5f);
    splashRGB = lerp(splashRGB, 1.0f.xxx, 0.35f);

    float3 splashAdd = splashRGB * (splashAlpha * splashDistFade * splashFlatMask * SplashIntensity * vmIntensity);
    splashAdd *= roofMask;
    splashAdd *= 0.6f;

    // Water exclusion and distance fade
    float waterThreshold = (depth / farZ) * 500;
    float isWaterSurface = (floorAngle > 0.9) &&
    (worldPos.z > TESR_WaterSettings.x - waterThreshold) &&
    (worldPos.z < TESR_WaterSettings.x + waterThreshold);

    if (!fast && (depth > DrawD || isWaterSurface))
        return float4(WetOut(baseColor.rgb + splashAdd), baseColor.a);
    float4 baseColorIn = baseColor;	// (fast: the far/water return below uses the colour before the streak darkening)

    float LODfade = smoothstep(DrawD, 0, depth);

    // Puddle coverage and wetness
    float3 blurredWetCoverage = saturate(tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb);
    float puddles = blurredWetCoverage.r;
    puddles *= (1.0f - puddleVMExclude) * (1.0f - vmMask);
    puddles *= smoothstep(DepressionSlopeCutoff, DepressionSlopeFadeStart, dotUp);

    // UNOFFICIAL 2026-10-27 persist: this spot's wetness (x) and puddle water (y) from the cover map
    float4 cover = persist ? CoverMapRead(camera_vector.xy) : float4(0, 0, 0, 0);
    float puddleFade = smoothstep(PuddleAccumulationStart, PuddleAccumulationFull,
        saturate(persist ? cover.y : TESR_WetWorldData.z));
    float puddlemask = puddles * puddleFade;

    float slopeMask = smoothstep(0.90f, 0.97f, dotUp);

    float depthish = saturate(puddlemask * 1.15f);
    depthish = depthish * depthish;

    float rim = 4.0f * puddlemask * (1.0f - puddlemask);
    float rimDark = rim * 0.15f;
    float rimReflBoost = rim * 0.08f;

    float rippleFlatMask = smoothstep(0.65f, 0.92f, dotUp);
    float rippleFlatFinal = lerp(rippleFlatMask, rippleFlatMask * vmFlatMask, vmMask);
    float rippleMask = rainVis * rippleFlatFinal;

    rippleMask *= lerp(1.0f, vmFlatMask, vmMask);

    float flatBlurStrength = saturate(FlatPuddleBlurStrength);
    float flatCoverage = lerp(roofMask * rippleFlatMask, blurredWetCoverage.g, flatBlurStrength);
    float flatWetCoverage = lerp(roofMask * rippleFlatMask * slopeMask,
        blurredWetCoverage.b, flatBlurStrength);
    rippleMask = lerp(rainFade * flatCoverage, rippleMask, vmMask);

    float wetSlope = lerp(slopeMask, 1.0f, vmMask);
    float wetRippleMask = lerp((persist ? cover.x : rainFade) * flatWetCoverage, rippleMask * wetSlope, vmMask);
    float wetMask = saturate(max(puddlemask, wetRippleMask * 0.85f));
    float specMask = saturate(max(puddlemask,
        wetRippleMask + rim * 0.15f * wetSlope));

    specMask = saturate(specMask + splashAlpha * 0.55f * splashFlatMask);

    // Vertical streaks
    float streakExposure = (1.0f - smoothstep(0.55f, 0.80f, abs(dotUp)))
        * (1.0f - vmMask) * rainVis;
    float streakDistance = length(camera_vector);
    streakExposure *= 1.0f - smoothstep(StreakFadeStart, StreakFadeFull, streakDistance);
    float3 streakSurfaceNormal = normalize(normal);
    float4 streakUV = GetWaterStreakUVs(worldPos.xyz, streakSurfaceNormal);
    float4 streakUVDx = ddx(streakUV);
    float4 streakUVDy = ddy(streakUV);
    float4 streakSample = float4(0, 0, 0, 0);
    [branch]
    if (streakDistance < StreakFadeFull && streakExposure > 0.0f)
    {
        streakSample = SampleWaterStreaks(streakUV, streakSurfaceNormal, streakUVDx, streakUVDy);
    }
    float streakMask = saturate(streakSample.a * streakExposure);
    float3 streakNormalOffset = streakSample.xyz * streakExposure;
    baseColor.rgb *= 1.0f - StreakDarken * streakMask;

    // Ripple normals
    float2 rippleUV_world = worldPos.xy / rippleScale;

    float vmRippleScale   = 0.28f;
    float vmRippleSizeMul = 0.2f;

    float2 rippleUV_vm = worldPos.xy / (rippleScale * vmRippleScale * vmRippleSizeMul);

    float2 rippleUV = lerp(rippleUV_world, rippleUV_vm, vmMask);

    float4 Weights = float4(1, 0.75, 0.5, 0.25) * TESR_WetWorldData.x;
    Weights = saturate(Weights * 4);

    float3 Ripple1 = ComputeRipple(rippleUV + float2( 0.25f, 0.0f), time1, Weights.x);
    float3 Ripple2 = ComputeRipple(rippleUV * 1.1 + float2(-0.55f, 0.3f), time2, Weights.y);
    float3 Ripple3 = ComputeRipple(rippleUV * 1.3 + float2( 0.6f, 0.85f), time3, Weights.z);
    float3 Ripple4 = ComputeRipple(rippleUV * 1.5 + float2( 0.5f,-0.75f), time4, Weights.w);

    float4 Z = lerp(1, float4(Ripple1.z, Ripple2.z, Ripple3.z, Ripple4.z), Weights);

    float3 ripple = float3(
        Weights.x * Ripple1.xy + Weights.y * Ripple2.xy + Weights.z * Ripple3.xy + Weights.w * Ripple4.xy,
        Z.x * Z.y * Z.z * Z.w
        );

    ripple.xy *= 2.2f;

    float3 combinedNormals = normalize(ripple);
    combinedNormals = normalize(lerp(float3(0,0,1), combinedNormals, LODfade * wetMask));

    combinedNormals.xy += splashBumpXY * 2.0f;
    combinedNormals = normalize(combinedNormals);

    // Puddle waves
    float t = TESR_GameTime.z;

    float2 windDir = float2(1.0f, 0.0f);
    float p = dot(worldPos.xy, windDir);

    float2 swell;
    swell.x =
    sin(p * 0.260f + t * 3.10f) +
    0.55f * sin((p + worldPos.y) * 0.200f + t * 3.90f);

    swell.y =
    sin(p * 0.240f + t * 3.40f) +
    0.55f * sin((p + worldPos.x) * 0.210f + t * 3.60f);

    float distFade = (1.0f - smoothstep(400.0f, 1800.0f, depth));
    float swellAmp = 0.0030f * pow(puddlemask, 1.35f) * distFade;

    combinedNormals.xy += swell * swellAmp;
    combinedNormals = normalize(combinedNormals);

    // Puddle texture normals
    float3 puddleNormalSurface = TESR_CameraPosition.xyz + worldPosRelRoof.xyz;
    float2 puddleSurfaceUV = puddleNormalSurface.xy / max(PuddleCloudWorldSize, 1.0f);
    float2 puddleNormalUV = puddleSurfaceUV + TESR_GameTime.z * PuddleCloudDrift;
    puddleNormalUV += combinedNormals.xy * PuddleCloudRippleDistortion;
    float2 puddleNormalUV2 = puddleSurfaceUV * 1.37f + float2(0.23f, 0.61f)
        - TESR_GameTime.z * PuddleCloudDrift * 0.65f;
    puddleNormalUV2 += combinedNormals.xy * PuddleCloudRippleDistortion;
    float2 puddleTexNormalA = tex2D(TESR_PuddleNormalSampler, puddleNormalUV).xy * 2.0f - 1.0f;
    float2 puddleTexNormalB = tex2D(TESR_PuddleNormalSampler, puddleNormalUV2).xy * 2.0f - 1.0f;
    float2 puddleTexNormal = lerp(puddleTexNormalA, puddleTexNormalB, 0.35f);
    puddleTexNormal.y *= PuddleNormalYSign;
    float flatNormalStrength = FlatPuddleNormalStrength * saturate(wetRippleMask) * (1.0f - vmMask);
    float textureNormalStrength = lerp(flatNormalStrength, PuddleNormalStrength, puddlemask);
    float2 puddleNormalOffset = puddleTexNormal * textureNormalStrength * LODfade;
    combinedNormals = normalize(float3(combinedNormals.xy + puddleNormalOffset,
        combinedNormals.z));

    // Surface normals
    float3 baseN = normalize(lerp(normal, up, puddlemask));
    float3 N = normalize(lerp(baseN, combinedNormals, specMask));

    N = normalize(N + streakNormalOffset);

    // Refraction
    float NdotV_refr = saturate(dot(N, -V));
    float viewRefr   = smoothstep(0.18f, 0.45f, NdotV_refr);

    float2 refrOff = expand(rectToScreen(projectPosition(combinedNormals).xy)) * TESR_ReciprocalResolution.xy * refractionScale;	// UNOFFICIAL 2026-10-27 RenderScale v2: the offset native frames get

    float splashRefr = 0.35f * splashAlpha * splashFlatMask;
    refrOff += splashBumpXY * (refractionScale * 0.35f) * TESR_ReciprocalResolution.xy * splashRefr;

    refrOff *= lerp(1.0f, 0.35f, puddlemask);

    refrOff *= viewRefr;

    float swellUV = (swellAmp * 0.50f) * viewRefr;
    refrOff += swell * swellUV;

    float2 refrClamp = TESR_ReciprocalResolution.xy * (refractionScale * 0.75f);
    refrOff = clamp(refrOff, -refrClamp, refrClamp);

    // FastPaths: every texture read the original makes with tex2D (implicit derivatives) is made here, the same way, on every
    // pixel before the first new branch: the mipmapped cloud read, and the refraction, first-person and shadow-buffer reads.
    // (A tex2Dlod at level 0 behind a branch is the same fetch only if no anisotropic filtering applies, e.g. a driver override:
    // the first build did that and left a small, local difference in game.) The branches below only skip arithmetic.
    float4 cloudSample = float4(0, 0, 0, 0);
    float wetWeight = LODfade * wetMask;
    float3 refrLinPre = float3(0, 0, 0);
    float vmSampleValidPre = 0.0f;
    float4 shadowsPre = float4(0, 0, 0, 0);
    if (fast)
    {
        cloudSample = tex2D(TESR_PuddleCloudSampler, GetPuddleCloudUV(worldPosRelRoof, combinedNormals));
        refrLinPre = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord + refrOff)).rgb;
        vmSampleValidPre = GetViewModelMask(saturate(IN.UVCoord + refrOff));
        shadowsPre = tex2D(TESR_PointShadowBuffer, IN.UVCoord);

        // (shadowsPre.y < -1: never true - the shadow buffer is G16R16 UNORM - it only keeps the compiler from sinking that
        // tex2D into a branch, where its implicit derivatives would no longer be the original's)
        [branch]
        if (depth > DrawD || isWaterSurface || shadowsPre.y < -1.0f)
            return float4(WetOut(baseColorIn.rgb + splashAdd), baseColorIn.a);

        // the SSR trace condition of the main path below (same expressions)
        float fLinger = puddleFade * flatCoverage;
        float fSsrMask = saturate(max(rippleMask, max(fLinger, puddlemask)));
        float fScreen = smoothstep(PuddleSSRScreenFadeStart, PuddleSSRScreenFadeFull, IN.UVCoord.y);
        float fAngle = 1.0f - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, saturate(dot(-V, up)));
        float fOpacity = fScreen * fAngle;
        float fTraceOpacity = saturate(PuddleSSRCandidateBlend) * saturate(fSsrMask * LODfade) * fOpacity;
        bool fTraces = fSsrMask > 0.001f && PuddleSSRCandidateBlend > 0.0f && fOpacity > 0.0f
            && fTraceOpacity > PuddleSSRMinTraceOpacity;

        [branch]
        // UNOFFICIAL 2026-10-18: without FixedReflectionTrace no trace runs (technique 0's returns 0 for every pixel), so a pixel that
        // would only have traced ends exactly as this zero-weight exit.
        if (vmMask <= 0.5f && !(wetWeight > 0.0f) && !(streakMask > 0.0f) && !(fixTrace && fTraces))
            return float4(WetOut(baseColor.rgb + splashAdd), 1.0f);
    }

    if (!fast)
    {
        float3 refrLin = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord + refrOff)).rgb;

        float3 baseLin = linearize(baseColor).rgb;
        float3 rippleColor = lerp(baseLin, refrLin, viewRefr);

        // First-person composition
        if (vmMask > 0.5f)
        {
            float2 vmRefrUV = saturate(IN.UVCoord + refrOff);
            float vmSampleValid = GetViewModelMask(vmRefrUV);

            float3 vmRefrLin = lerp(baseLin, refrLin, vmSampleValid);
            float vmRippleBlend = saturate(LODfade * rippleMask) * viewRefr;
            float3 vmWetLin = lerp(baseLin, vmRefrLin, vmRippleBlend);
            float3 vmWetRGB = delinearize(float4(vmWetLin, 1.0f)).rgb;
            return float4(WetOut(vmWetRGB + splashAdd), baseColor.a);
        }

        // Puddle sky lighting
        float sunAmbient = max(luma(linearize(TESR_SunAmbient)), 0.0001f);
        float4 shadows = tex2D(TESR_PointShadowBuffer, IN.UVCoord);
        float inShadow = saturate(pow((shadows.r + shadows.g) / sunAmbient, 5));

        float sunHeight = shade(TESR_SunPosition.xyz, up);

        float3 fresnelColor = GetSkyColor(
            0, 0.5, sunHeight, 0.0f, TESR_SkyData.z,
            TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, TESR_SunColor.rgb
            );

        float NdotV = saturate(dot(N, -V));
        float F = PuddleSkyMinReflect + (1.0f - PuddleSkyMinReflect)
        * pow(1.0f - NdotV, 5.0f);

        float reflDist = 1.0f - smoothstep(600.0f, DrawD, depth);
        float reflStrength = saturate(puddlemask * F * PuddleSkyStrength
            * lerp(1.0f, 1.15f, rainVis) * reflDist);

        float3 puddleBase = rippleColor.rgb * lerp(1.0f, 0.55f, TESR_WetWorldData.w);
        puddleBase = lerp(rippleColor.rgb, puddleBase, puddlemask);

        float3 waterTint = float3(0.92f, 0.97f, 1.05f);
        puddleBase *= lerp(1.0f, waterTint, depthish * 0.25f);
        puddleBase *= (1.0f - depthish * 0.18f);
        puddleBase *= (1.0f - rimDark);

        float reflMix = min(PuddleSkyMaxBlend, reflStrength * (1.0f + depthish * 0.20f + rimReflBoost));
        reflMix *= smoothstep(0.03f, 0.12f, puddlemask);

        float3 reflectionSky = max(fresnelColor, 0.0f.xxx);
        float reflectionSkyLuma = max(luma(reflectionSky), 0.0f);
        reflectionSky = lerp(reflectionSkyLuma.xxx, reflectionSky,
            saturate(PuddleSkyTintInfluence));
        float3 skyReflColor = reflectionSky * PuddleSkyBrightness;
        float skyPeak = max(skyReflColor.r, max(skyReflColor.g, skyReflColor.b));
        skyReflColor /= (1.0f + skyPeak);

        float skyLuma = max(luma(skyReflColor), 0.0001f);
        float skyLumaLimit = PuddleSkyLumaFloor + luma(baseLin) * PuddleSkyLumaScale;
        skyReflColor *= min(1.0f, skyLumaLimit / skyLuma);

        // Puddle cloud reflection
        float3 cloudSurface = TESR_CameraPosition.xyz
        + worldPosRelRoof.xyz;
        float2 cloudUV = cloudSurface.xy / max(PuddleCloudWorldSize, 1.0f);
        cloudUV += TESR_GameTime.z * PuddleCloudDrift;
        cloudUV += combinedNormals.xy * PuddleCloudRippleDistortion;
        cloudSample = tex2D(TESR_PuddleCloudSampler, cloudUV);

        float3 cloudDetail = saturate((cloudSample.rgb - 0.5f) * PuddleCloudContrast + 0.5f);
        float cloudAlpha = saturate((cloudSample.a - 0.5f) * PuddleCloudContrast + 0.5f);

        float3 cloudLight = reflectionSky;
        cloudLight /= (1.0f + max(cloudLight.r, max(cloudLight.g, cloudLight.b)));
        cloudLight *= min(1.0f, skyLumaLimit / max(luma(cloudLight), 0.0001f));
        float3 cloudReflection = cloudLight * PuddleCloudBrightness
        * lerp(0.12f.xxx, 1.0f.xxx, cloudDetail);
        float cloudCoverage = saturate(cloudAlpha * PuddleCloudStrength);
        skyReflColor = lerp(skyReflColor, cloudReflection, cloudCoverage);
        float3 puddleColor = lerp(puddleBase, skyReflColor, reflMix);

        // Screen-space reflections
        float lingeringReflectionMask = puddleFade * flatCoverage;
        float ssrMask = saturate(max(rippleMask, max(lingeringReflectionMask, puddlemask)));
        float ssrScreenOpacity = smoothstep(PuddleSSRScreenFadeStart,
            PuddleSSRScreenFadeFull, IN.UVCoord.y);
        float ssrDownAngle = saturate(dot(-V, up));
        float ssrAngleOpacity = 1.0f - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, ssrDownAngle);
        float ssrOpacity = ssrScreenOpacity * ssrAngleOpacity;
        float ssrTraceOpacity = saturate(PuddleSSRCandidateBlend)
            * saturate(ssrMask * LODfade) * ssrOpacity;
        float3 ssrHit = float3(0, 0, 0);
        if (ssrMask > 0.001f && PuddleSSRCandidateBlend > 0.0f && ssrOpacity > 0.0f
            && ssrTraceOpacity > PuddleSSRMinTraceOpacity)
        {
            float3 ssrNormal = normalize(lerp(baseN, N, saturate(PuddleSSRNormalInfluence)));
            if (fixTrace)
            ssrHit = TraceWetReflectionFixed(IN.UVCoord, ssrNormal);
        else
            ssrHit = TraceWetReflection(IN.UVCoord, ssrNormal);
        }
        float ssrCoverage = saturate(ssrHit.z * ssrMask * LODfade) * ssrOpacity;

        // Sun highlights
        float3 Ls = normalize(TESR_SunDirection.xyz);
        float3 Hs = normalize(Ls - V);

        float sunNdotH   = saturate(dot(N, Hs));
        float sunSpecPow = lerp(lerp(80.0f, 200.0f, specMask), 700.0f, puddlemask);
        float sunSpec    = pow(sunNdotH, sunSpecPow) * specMask;

        float sunSpecMul = lerp(0.25f, 0.70f, saturate(sunHeight));
        float3 sunSpecCol = linearize(TESR_SunColor).rgb * sunSpec * sunSpecMul * inShadow
        * lerp(1.0f, PuddleSunStrength, puddlemask);
        float sunHighlightLimit = lerp(PuddleSunLimit, PuddleSunHighlightLimit, puddlemask);
        sunSpecCol = min(sunSpecCol, sunHighlightLimit.xxx);

        float3 streakSunCol = float3(0, 0, 0);
        [branch]
        if (streakMask > 0.0f)
        {
            float streakSunSpec = pow(sunNdotH, StreakSunSpecularPower)
                * streakMask * StreakSpecularStrength;
            streakSunCol = linearize(TESR_SunColor).rgb * streakSunSpec * sunSpecMul * inShadow;
        }

        // Local light highlights
        float3 pointSpecCol = float3(0, 0, 0);
        float3 streakPointCol = float3(0, 0, 0);

        [unroll]
        for (int i = 0; i < 12; i++)
        {
            if (TESR_ShadowLightPosition[i].w > 0.0f)
            {
                float3 L = TESR_ShadowLightPosition[i].xyz - worldPos.xyz;
                float dist = length(L);
                L /= max(dist, 1e-4);

                float d = dist / max(TESR_ShadowLightPosition[i].w, 1e-4);
                float att = saturate(1.0f - d);
                att *= att;

                float3 H = normalize(L - V);

                float ndh = saturate(dot(N, H));
                float specLobe = pow(ndh, 30.0f);
                float spec = specLobe * specMask;

                float3 c = TESR_LightColor[i].rgb;
                pointSpecCol += c * spec * att;
                float streakSpec = specLobe * streakMask * StreakSpecularStrength;
                streakPointCol += c * streakSpec * att;
            }

            if (TESR_LightPosition[i].w > 0.0f)
            {
                float3 L = TESR_LightPosition[i].xyz - worldPos.xyz;
                float dist = length(L);
                L /= max(dist, 1e-4);

                float d = dist / max(TESR_LightPosition[i].w, 1e-4);
                float att = saturate(1.0f - d);
                att *= att;

                float3 H = normalize(L - V);

                float ndh = saturate(dot(N, H));
                float specLobe = pow(ndh, 30.0f);
                float spec = specLobe * specMask;

                float3 c = TESR_LightColor[i + 12].rgb;
                pointSpecCol += c * spec * att;
                float streakSpec = specLobe * streakMask * StreakSpecularStrength;
                streakPointCol += c * streakSpec * att;
            }
        }

        pointSpecCol *= lerp(1.0f, PuddleLightStrength, puddlemask);
        float pointHighlightLimit = lerp(PuddleLightLimit, PuddlePointHighlightLimit, puddlemask);
        pointSpecCol = min(pointSpecCol, pointHighlightLimit.xxx);

        // Final composition
        float3 finalPuddle = puddleColor + sunSpecCol + pointSpecCol;

        float4 outCol = float4(finalPuddle, 1.0f);
        outCol = delinearize(outCol);

        float3 wetRGB = lerp(baseColor.rgb, outCol.rgb, LODfade * wetMask);

        if (ssrHit.z > (fixTrace ? 0.001f : 0.5f) && PuddleSSRCandidateBlend > 0.0f)
        {
            float3 reflectionRGB = tex2Dlod(TESR_SourceBuffer, float4(ssrHit.xy, 0, 0)).rgb;
            float reflectionBlend = saturate(PuddleSSRCandidateBlend) * ssrCoverage;
            wetRGB = lerp(wetRGB, reflectionRGB, reflectionBlend);
        }

        // Streak lighting composition
        [branch]
        if (streakMask > 0.0f)
        {
            float3 streakLighting = min(streakSunCol + streakPointCol,
                (StreakHighlightLimit * streakMask).xxx);
            float3 wetLinear = linearize(float4(wetRGB, 1.0f)).rgb;
            wetRGB = delinearize(float4(wetLinear + streakLighting * LODfade, 1.0f)).rgb;
        }
        return float4(WetOut(wetRGB + splashAdd), 1.0f);
    }

    // FastPaths (fast == true): the same composition, with real branches. Pixels reaching this point are not far, not water,
    // and have a puddle weight, a streak or an SSR trace (or are first-person).
    float3 baseLin = linearize(baseColor).rgb;

    // First-person composition
    [branch]
    if (vmMask > 0.5f)
    {
        float3 vmRefrLinRaw = refrLinPre;
        float vmSampleValid = vmSampleValidPre;

        float3 vmRefrLin = lerp(baseLin, vmRefrLinRaw, vmSampleValid);
        float vmRippleBlend = saturate(LODfade * rippleMask) * viewRefr;
        float3 vmWetLin = lerp(baseLin, vmRefrLin, vmRippleBlend);
        float3 vmWetRGB = delinearize(float4(vmWetLin, 1.0f)).rgb;
        return float4(WetOut(vmWetRGB + splashAdd), baseColor.a);
    }

    float sunAmbient = max(luma(linearize(TESR_SunAmbient)), 0.0001f);
    float4 shadows = shadowsPre;
    float inShadow = saturate(pow((shadows.r + shadows.g) / sunAmbient, 5));

    float sunHeight = shade(TESR_SunPosition.xyz, up);

    // Screen-space reflections
    float lingeringReflectionMask = puddleFade * flatCoverage;
    float ssrMask = saturate(max(rippleMask, max(lingeringReflectionMask, puddlemask)));
    float ssrScreenOpacity = smoothstep(PuddleSSRScreenFadeStart,
        PuddleSSRScreenFadeFull, IN.UVCoord.y);
    float ssrDownAngle = saturate(dot(-V, up));
    float ssrAngleOpacity = 1.0f - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, ssrDownAngle);
    float ssrOpacity = ssrScreenOpacity * ssrAngleOpacity;
    float ssrTraceOpacity = saturate(PuddleSSRCandidateBlend)
        * saturate(ssrMask * LODfade) * ssrOpacity;
    float3 ssrHit = float3(0, 0, 0);
    [branch]
    if (ssrMask > 0.001f && PuddleSSRCandidateBlend > 0.0f && ssrOpacity > 0.0f
        && ssrTraceOpacity > PuddleSSRMinTraceOpacity)
    {
        float3 ssrNormal = normalize(lerp(baseN, N, saturate(PuddleSSRNormalInfluence)));
        if (fixTrace)
            ssrHit = TraceWetReflectionFixed(IN.UVCoord, ssrNormal);
        // UNOFFICIAL 2026-10-18: no 'else' trace. Technique 0's TraceWetReflection returns (0,0,0) for every pixel (a hit whose refined
        // UV is on screen returns 0, and refined UVs are always on screen). In this fast path the same code compiled with dead loop exits
        // (break_ne -c,-c; evidence loopexits\disasm\WetWorld.txt 4611/4626/4654/4706) and kept the LAST bracket at full strength:
        // grey patches on puddles at shallow angles that the original never showed. ssrHit = 0 is the original's result.
    }
    float ssrCoverage = saturate(ssrHit.z * ssrMask * LODfade) * ssrOpacity;

    // Sun highlights (the half vector and the strength are shared with the streaks)
    float3 Ls = normalize(TESR_SunDirection.xyz);
    float3 Hs = normalize(Ls - V);
    float sunNdotH   = saturate(dot(N, Hs));
    float sunSpecMul = lerp(0.25f, 0.70f, saturate(sunHeight));

    float3 streakSunCol = float3(0, 0, 0);
    [branch]
    if (streakMask > 0.0f)
    {
        float streakSunSpec = pow(sunNdotH, StreakSunSpecularPower)
            * streakMask * StreakSpecularStrength;
        streakSunCol = linearize(TESR_SunColor).rgb * streakSunSpec * sunSpecMul * inShadow;
    }

    // Local light highlights: a lamp slot that is empty (w = 0) is skipped for the whole screen, a lamp that does not reach
    // the pixel (attenuation 0) for that pixel. Same accumulation order as the main path.
    float3 pointSpecCol = float3(0, 0, 0);
    float3 streakPointCol = float3(0, 0, 0);

    float anyLamp = 0.0f;
    [unroll]
    for (int j = 0; j < 12; j++)
        anyLamp = max(anyLamp, max(TESR_ShadowLightPosition[j].w, TESR_LightPosition[j].w));
    [branch]
    if (anyLamp > 0.0f)	// one uniform test (a preshader boolean): no lamp slot in use -> the loop adds nothing
    {
        [unroll]
        for (int i = 0; i < 12; i++)
        {
            AddLampHighlightFast(TESR_ShadowLightPosition[i], TESR_LightColor[i].rgb, worldPos.xyz, V, N, specMask, streakMask,
                pointSpecCol, streakPointCol);
            AddLampHighlightFast(TESR_LightPosition[i], TESR_LightColor[i + 12].rgb, worldPos.xyz, V, N, specMask, streakMask,
                pointSpecCol, streakPointCol);
        }
    }

    float3 wetRGB = baseColor.rgb;
    [branch]
    if (wetWeight > 0.0f)
    {
        // Refraction
        float3 refrLin = refrLinPre;
        float3 rippleColor = lerp(baseLin, refrLin, viewRefr);

        // Puddle sky lighting
        float3 fresnelColor = GetSkyColor(
            0, 0.5, sunHeight, 0.0f, TESR_SkyData.z,
            TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, TESR_SunColor.rgb
            );

        float NdotV = saturate(dot(N, -V));
        float F = PuddleSkyMinReflect + (1.0f - PuddleSkyMinReflect)
        * pow(1.0f - NdotV, 5.0f);

        float reflDist = 1.0f - smoothstep(600.0f, DrawD, depth);
        float reflStrength = saturate(puddlemask * F * PuddleSkyStrength
            * lerp(1.0f, 1.15f, rainVis) * reflDist);

        float3 puddleBase = rippleColor.rgb * lerp(1.0f, 0.55f, TESR_WetWorldData.w);
        puddleBase = lerp(rippleColor.rgb, puddleBase, puddlemask);

        float3 waterTint = float3(0.92f, 0.97f, 1.05f);
        puddleBase *= lerp(1.0f, waterTint, depthish * 0.25f);
        puddleBase *= (1.0f - depthish * 0.18f);
        puddleBase *= (1.0f - rimDark);

        float reflMix = min(PuddleSkyMaxBlend, reflStrength * (1.0f + depthish * 0.20f + rimReflBoost));
        reflMix *= smoothstep(0.03f, 0.12f, puddlemask);

        float3 reflectionSky = max(fresnelColor, 0.0f.xxx);
        float reflectionSkyLuma = max(luma(reflectionSky), 0.0f);
        reflectionSky = lerp(reflectionSkyLuma.xxx, reflectionSky,
            saturate(PuddleSkyTintInfluence));
        float3 skyReflColor = reflectionSky * PuddleSkyBrightness;
        float skyPeak = max(skyReflColor.r, max(skyReflColor.g, skyReflColor.b));
        skyReflColor /= (1.0f + skyPeak);

        float skyLuma = max(luma(skyReflColor), 0.0001f);
        float skyLumaLimit = PuddleSkyLumaFloor + luma(baseLin) * PuddleSkyLumaScale;
        skyReflColor *= min(1.0f, skyLumaLimit / skyLuma);

        // Puddle cloud reflection (its texture read is above the branches)
        float3 cloudDetail = saturate((cloudSample.rgb - 0.5f) * PuddleCloudContrast + 0.5f);
        float cloudAlpha = saturate((cloudSample.a - 0.5f) * PuddleCloudContrast + 0.5f);

        float3 cloudLight = reflectionSky;
        cloudLight /= (1.0f + max(cloudLight.r, max(cloudLight.g, cloudLight.b)));
        cloudLight *= min(1.0f, skyLumaLimit / max(luma(cloudLight), 0.0001f));
        float3 cloudReflection = cloudLight * PuddleCloudBrightness
        * lerp(0.12f.xxx, 1.0f.xxx, cloudDetail);
        float cloudCoverage = saturate(cloudAlpha * PuddleCloudStrength);
        skyReflColor = lerp(skyReflColor, cloudReflection, cloudCoverage);
        float3 puddleColor = lerp(puddleBase, skyReflColor, reflMix);

        // Sun highlight
        float sunSpecPow = lerp(lerp(80.0f, 200.0f, specMask), 700.0f, puddlemask);
        float sunSpec    = pow(sunNdotH, sunSpecPow) * specMask;
        float3 sunSpecCol = linearize(TESR_SunColor).rgb * sunSpec * sunSpecMul * inShadow
        * lerp(1.0f, PuddleSunStrength, puddlemask);
        float sunHighlightLimit = lerp(PuddleSunLimit, PuddleSunHighlightLimit, puddlemask);
        sunSpecCol = min(sunSpecCol, sunHighlightLimit.xxx);

        pointSpecCol *= lerp(1.0f, PuddleLightStrength, puddlemask);
        float pointHighlightLimit = lerp(PuddleLightLimit, PuddlePointHighlightLimit, puddlemask);
        pointSpecCol = min(pointSpecCol, pointHighlightLimit.xxx);

        // Final composition
        float3 finalPuddle = puddleColor + sunSpecCol + pointSpecCol;

        float4 outCol = float4(finalPuddle, 1.0f);
        outCol = delinearize(outCol);

        wetRGB = lerp(baseColor.rgb, outCol.rgb, wetWeight);
    }

    if (ssrHit.z > (fixTrace ? 0.001f : 0.5f) && PuddleSSRCandidateBlend > 0.0f)
    {
        float3 reflectionRGB = tex2Dlod(TESR_SourceBuffer, float4(ssrHit.xy, 0, 0)).rgb;
        float reflectionBlend = saturate(PuddleSSRCandidateBlend) * ssrCoverage;
        wetRGB = lerp(wetRGB, reflectionRGB, reflectionBlend);
    }

    // Streak lighting composition
    [branch]
    if (streakMask > 0.0f)
    {
        float3 streakLighting = min(streakSunCol + streakPointCol,
            (StreakHighlightLimit * streakMask).xxx);
        float3 wetLinear = linearize(float4(wetRGB, 1.0f)).rgb;
        wetRGB = delinearize(float4(wetLinear + streakLighting * LODfade, 1.0f)).rgb;
    }
    return float4(WetOut(wetRGB + splashAdd), 1.0f);
}

// =============================================================================================================================
// UNOFFICIAL 2026-11-05 Model 1 ([Shaders.WetWorld.Main] Model = 1, the default; 0 = techniques 0 to 6 above, unchanged): a physically
// based, material-dependent wet surface (docs/rain/research/04-wet-surfaces.md rank 1, 04b for what was built). Techniques 7 to 10 (the
// FastPaths structure; with and without the fixed trace and the cover map). Per pixel:
//  - wetness w = rain history (rain fade, or the cover map's spot) x open sky (the roof test, taken a little out from the surface along
//    its normal, so walls are tested in front of themselves) x exposure (1 facing up, 0.35 on a vertical wall, 0 facing down) x the
//    distance fade; actors (material class 2 worn gear, 6 skin) and the first-person view stay dry (the skin shader wets skin itself);
//  - porosity p from the material class (land 0.6-0.9 by the albedo's brightness and grey-ness, objects 0.3-0.6 by chroma and roughness,
//    cloth 0.85, plants 0.15, metal share 0), or from the picture's chroma where nothing was written;
//  - the lit colour's diffuse share is darkened as the albedo would be (Lagarde 2013, "Water drop 3b": albedo x lerp(1, floor, p x w),
//    floor 0.3) and slightly saturated (the extra bounces inside the water layer filter by the albedo again: Lekner and Dorf 1988);
//  - a water layer on top: F0 0.02, roughness lerp(dry roughness, 0.08, w x (1 - p/2)) (0.05 in puddles); the sky (and the SSR hit) through
//    Karis's EnvBRDFApprox, the sun and the lamps through GGX / Smith / Schlick; the light passing the layer loses the same Fresnel share;
//  - the sheen's normal takes a bump from the albedo's brightness (surface gradient, Mikkelsen 2010: the captured normals hold no normal-map
//    detail) and the ripples on flat wet ground; refraction only in puddles. Puddles keep their structure (rim, depth tint, cloud texture,
//    Amount) with the same water BRDF and no 28 % cap or luma limiter.
// =============================================================================================================================
static const float WetFilmF0 = 0.02f;			// water's reflectance at normal incidence (n = 1.33)
static const float WetFilmRoughness = 0.08f;	// the perceptual roughness a fully wet sealed surface tends to
static const float WetPuddleRoughness = 0.05f;	// standing water (the ripples are normal detail, not roughness)
static const float WetLightMinRoughness = 0.10f;	// the sun and lamps are not points: their lobe never narrower than this
static const float WetDryRoughness = 0.70f;		// where the material buffer has no roughness
static const float WetDarkenFloor = 0.30f;		// diffuse left on a fully porous, fully wet surface (Lagarde: 0.2)
static const float WetSaturation = 0.35f;		// chroma exponent at p x w = 1
static const float WetWallExposure = 0.35f;		// wetness of a vertical open wall against open flat ground
static const float WetRoofOffset = 36.0f;		// game units the roof test steps out along a vertical surface's normal (3 ortho texels)
static const float WetBumpHeight = 1.5f;		// game units of height per unit of albedo brightness (the sheen's bump)
static const float WetBumpMaxTilt = 0.35f;
static const float WetSpecLimit = 4.0f;			// the sun + lamp highlights' sum, linear (KeepHighlights passes it to the tonemapper)
// 2026-11-05 QA P8: walls (N.z below 0.5) dry out between these view distances (game units): their 0.35-exposure sheen is invisible
// that far, and wet = 0 lets them take the early exit. Ground (N.z >= 0.5) is not touched.
static const float WetWallFadeStart = 1200.0f;
static const float WetWallFadeEnd = 1600.0f;
static const float WetLampMinFilm = 0.01f;		// P8: the 24-lamp loop runs only where the film (or a streak) is at least this
// 2026-11-05 QA finding 16: the sky visibility of the film's reflection (WetSkyVisibility)
static const float WetSkyProbeNear = 128.0f;	// game units along the reflection ray: the two height-field probes
static const float WetSkyProbeFar = 768.0f;
static const float WetSkyProbeSoft = 48.0f;	// a probe goes from open to blocked over this many units below the ortho map's top surface
static const float WetHorizonVis = 0.7f;		// the sky share left to a ray along the horizon (it mostly meets far terrain / buildings)

// Karis 2014, "Physically Based Shading on Mobile": the split-sum environment BRDF, fitted.
float WetEnvBRDFApprox(float F0, float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// The water layer's highlight from one light: GGX D, Schlick-Smith visibility, Schlick F with F0 0.02, times N.L and PI (the game's
// lights are scaled so that a diffuse surface gives albedo x colour x N.L). toEye and L unit, from the surface. alpha2 = roughness^4
// (plus the normal variance, see WetPBR).
float WetGGX(float3 N, float3 toEye, float3 L, float alpha2)
{
    float3 H = normalize(L + toEye);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, toEye), 1e-4f);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(toEye, H));
    float dd = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / (PI * dd * dd);
    float k = sqrt(alpha2) * 0.5f;
    float vis = 0.25f / ((NdotL * (1.0f - k) + k) * (NdotV * (1.0f - k) + k));
    float F = WetFilmF0 + (1.0f - WetFilmF0) * pow(1.0f - VdotH, 5.0f);
    return D * vis * F * NdotL * PI;
}

// One lamp through WetGGX (as AddLampHighlightFast: the lobe only where the lamp reaches the pixel). Streaks keep their old lobe.
void AddLampHighlightPBR(float4 lightPos, float3 c, float3 worldPos, float3 toEye, float3 N, float alpha2, float streakMask,
    inout float3 pointSpecCol, inout float3 streakPointCol)
{
    float3 L = lightPos.xyz - worldPos;
    float dist = length(L);
    // The radius floor depends on the pixel (dist), so the compiler cannot make per-slot preshader values of it: unrolled, those (the
    // radius' reciprocal and the empty-slot test, 24 slots) pushed the Persistent + fixed trace variant past ps_3_0's 224 float constants.
    // An empty slot (w = 0) divides by about 1e-6 x dist: att = 0, as the old test on w.
    float att = saturate(1.0f - dist / max(lightPos.w, dist * 1e-6f + 1e-6f));
    att *= att;
    [branch]
    if (att > 0.0f)
    {
        L /= max(dist, 1e-4);
        pointSpecCol += c * (WetGGX(N, toEye, L, alpha2) * att);
        float streakLobe = pow(saturate(dot(N, normalize(L + toEye))), 30.0f);
        streakPointCol += c * (streakLobe * streakMask * StreakSpecularStrength * att);
    }
}

// The sky the water layer reflects in direction R (world, unit): the sky functions the puddles used, with height, horizon and the sun's
// side; below the horizon a dim copy (the ground).
float3 WetSkyEnvironment(float3 R, float sunHeight)
{
    float3 Ls = normalize(TESR_SunDirection.xyz);
    float up = saturate(R.z);
    float sunInfluence = pow(saturate(dot(R, Ls)), 6.0f);
    float3 sky = max(GetSkyColor(up, 1.0f - up, sunHeight, sunInfluence, TESR_SkyData.z,
        TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, TESR_SunColor.rgb), 0.0f.xxx);
    // (the sky functions give the game's display-encoded colours, as the sky is drawn: linear here, like the rest of the composition;
    // bench 2026-11-05: unconverted, the film reflected about twice the sky's light and the ground looked frosted)
    return linearize(float4(sky, 1.0f)).rgb * lerp(0.2f, 1.0f, smoothstep(-0.2f, 0.05f, R.z));
}

// 2026-11-05 QA finding 16: how much sky the film's reflection ray sees (1 open, 0 blocked). With the default FixedReflectionTrace off
// there is no SSR, and a grazing street view showed a full sky sheen where facades would be reflected. The ortho map (the top-down
// shadow map the roof test reads) is used as a height field: the ray from the surface (pos: camera-relative, already a little out
// along the normal) in direction R is probed at WetSkyProbeNear and WetSkyProbeFar units. A probe below the map's top surface lies
// inside a facade or under a roof / awning (open = saturate(1 + (top - z) / (48 units in ortho depth))); outside the map it counts
// as open. vis = 0.5 (open(near) + open(far)) x lerp(WetHorizonVis, 1, smoothstep(0, 0.25, R.z)): the horizon term because a ray
// along the horizon mostly meets far terrain and buildings beyond the probes. Two point reads, no loop.
float WetSkyProbe(float3 pos)
{
    float4 o = mul(float4(pos, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    o.xyz /= o.w;
    float inside = (abs(o.x) < 1.0f && abs(o.y) < 1.0f && o.z > 0.0f && o.z < 1.0f) ? 1.0f : 0.0f;
    float top = SampleOrthoDepth(float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f));
    float open = saturate(1.0f + (top - o.z) / (GetOrthoDepthScale() * WetSkyProbeSoft));
    return lerp(1.0f, open, inside);
}

float WetSkyVisibility(float3 pos, float3 R)
{
    float probes = 0.5f * (WetSkyProbe(pos + R * WetSkyProbeNear) + WetSkyProbe(pos + R * WetSkyProbeFar));
    return probes * lerp(WetHorizonVis, 1.0f, smoothstep(0.0f, 0.25f, R.z));
}

// Surface-gradient bump (Mikkelsen 2010, "Bump Mapping Unparametrized Surfaces on the GPU") from a height field given per pixel.
// pos: camera-relative world position. The tilt is limited, so object borders (large height steps) do not flip the normal.
float3 WetBumpNormal(float3 N, float3 pos, float height)
{
    float3 dPdx = ddx(pos);
    float3 dPdy = ddy(pos);
    float dHdx = ddx(height);
    float dHdy = ddy(height);
    float3 R1 = cross(dPdy, N);
    float3 R2 = cross(N, dPdx);
    float det = dot(dPdx, R1);
    float3 grad = (dHdx * R1 + dHdy * R2) / ((det < 0.0f ? -1.0f : 1.0f) * max(abs(det), 1e-8f));
    float len = length(grad);
    grad *= min(1.0f, WetBumpMaxTilt / max(len, 1e-6f));
    return normalize(N - grad);
}

float4 WetPBR(VSOUT IN, uniform bool fixTrace, uniform bool persist) : COLOR0
{
    // Surface reconstruction and weather (as Wet)
    float4 baseColor = tex2D(TESR_SourceBuffer, IN.UVCoord);
    float depth = readDepth(IN.UVCoord);

    float4 worldPosRelRoof = GetOrthoSurfacePosition(IN.UVCoord);
    float4 orthoRoofPos = mul(worldPosRelRoof, TESR_ShadowCameraToLightTransformOrtho);
    float roofMask = GetRoofMask(orthoRoofPos);

    float3 eyeDirection = toWorld(IN.UVCoord);
    float3 camera_vector = eyeDirection * depth;
    float4 worldPos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);

    float3 normal = GetWorldNormal(IN.UVCoord);
    float3 up = float3(0, 0, 1);

    float floorAngle = smoothstep(0.94, 1, dot(normal, up));
    float3 V = normalize(eyeDirection);

    float rainAmt  = saturate(TESR_WetWorldData.x / max(TESR_WetWorldData.y, 0.0001f));
    float rainFade = smoothstep(0.05f, 0.35f, rainAmt);
    float rainVis = rainFade * roofMask;

    // First-person surface mask
    float vmDepthX = tex2D(TESR_DepthBufferViewModel, IN.UVCoord).x;
    float vmMask = GetViewModelMask(IN.UVCoord);
    float puddleVMExclude = GetPuddleViewModelExclusion(IN.UVCoord, vmMask);

    float2 d = float2(ddx(vmDepthX), ddy(vmDepthX));
    float grad = length(d);
    float vmFlatMask = 1.0f - saturate(grad * 80.0f);
    vmFlatMask = smoothstep(0.55f, 0.90f, vmFlatMask);

    // Rain splashes (as Wet)
    float dotUp = dot(normal, up);
    float splashDistFade = 1.0f - smoothstep(SplashFadeStart, SplashMaxDist, depth);
    float splashFlatMask = smoothstep(0.7071f, 0.92f, dotUp);
    float2 splashXY = worldPos.xy;
    float rand = hash12(floor(worldPos.xy * 0.5f));
    float worldSizeVar = lerp(0.75f, 1.35f, rand);
    float2 worldSplashXY = splashXY / worldSizeVar;
    float vmSizeVar = lerp(0.70f, 1.30f, rand);
    float2 vmSplashXY = (splashXY / 0.28f) * vmSizeVar;
    splashXY = lerp(worldSplashXY, vmSplashXY, vmMask);

    float splashAlpha;
    float2 splashNXY;
    float4 splashSample = SampleSplash(splashXY, TESR_GameTime.z, (rainAmt * roofMask) * 0.395f, splashAlpha, splashNXY);
    splashAlpha *= roofMask;
    float splashStrength = 2.2f * splashAlpha * splashFlatMask * saturate(0.5f + 0.5f * rainVis);
    float2 splashBumpXY = splashNXY * splashStrength;
    float vmIntensity = lerp(1.0f, 0.90f, vmMask);
    float3 splashRGB = saturate((splashSample.rgb - 0.5f) * 1.6f + 0.5f);
    splashRGB = lerp(splashRGB, 1.0f.xxx, 0.35f);
    float3 splashAdd = splashRGB * (splashAlpha * splashDistFade * splashFlatMask * SplashIntensity * vmIntensity);
    splashAdd *= roofMask * 0.6f;

    // Water exclusion and distance fade
    float waterThreshold = (depth / farZ) * 500;
    float isWaterSurface = (floorAngle > 0.9) &&
    (worldPos.z > TESR_WaterSettings.x - waterThreshold) &&
    (worldPos.z < TESR_WaterSettings.x + waterThreshold);
    float4 baseColorIn = baseColor;
    float LODfade = smoothstep(DrawD, 0, depth);

    // Puddle coverage (as Wet)
    float3 blurredWetCoverage = saturate(tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb);
    float puddles = blurredWetCoverage.r;
    puddles *= (1.0f - puddleVMExclude) * (1.0f - vmMask);
    puddles *= smoothstep(DepressionSlopeCutoff, DepressionSlopeFadeStart, dotUp);

    // The material (Model 1): class, metal share, roughness, albedo. TESR_WetWorldLook.y = 0 when the buffers do not match this frame
    // (a frame upscaled before the effects).
    float4 material = tex2Dlod(TESR_MaterialBuffer, float4(IN.UVCoord, 0.0f, 0.0f));
    float4 albedoSample = tex2Dlod(TESR_AlbedoBuffer, float4(IN.UVCoord, 0.0f, 0.0f));
    bool materialValid = TESR_MaterialBufferData.x > 0.5f && TESR_WetWorldLook.y > 0.5f && material.r > 0.5f;
    bool albedoValid = TESR_AlbedoBufferData.x > 0.5f && TESR_WetWorldLook.y > 0.5f && albedoSample.a > 0.5f;
    float materialClass = materialValid ? floor(material.r) : 0.0f;
    float metal = materialValid ? saturate(frac(material.r) / 0.49f) : 0.0f;
    float surfaceRough = (materialValid && material.b >= 0.0f) ? saturate(material.b) : -1.0f;
    bool actor = materialClass > 1.5f && (materialClass < 2.5f || materialClass > 5.5f);	// worn gear (2), skin (6)
    // 2026-11-05 QA finding 15: characters stay clear of the splashes and wall streaks too (Model 0 put both on them). Without this
    // frame's material buffer the C++ runs the Model 0 technique instead (WetWorldEffect::ModelTechnique), so no class-0 characters here.
    float notActor = actor ? 0.0f : 1.0f;
    splashAdd *= notActor;

    // Porosity
    float3 baseLinRaw = linearize(baseColor).rgb;
    float3 albedo = albedoValid ? max(albedoSample.rgb, 0.0f.xxx) : baseLinRaw;	// without the buffer only its chroma is used
    float albedoMax = max(albedo.r, max(albedo.g, albedo.b));
    float albedoMin = min(albedo.r, min(albedo.g, albedo.b));
    float chroma = (albedoMax - albedoMin) / max(albedoMax, 1e-4f);
    float albedoLuma = max(luma(albedo), 1e-4f);
    float greyness = 1.0f - saturate(chroma * 1.5f);	// saturated paints and plastics are sealed; grey and earthy surfaces porous
    float porosityLand = lerp(0.6f, 0.9f, greyness * (albedoValid ? smoothstep(0.06f, 0.30f, albedoLuma) : 0.5f));
    float porosityObject = lerp(0.3f, 0.6f, greyness) * (surfaceRough >= 0.0f ? lerp(0.35f, 1.0f, saturate((surfaceRough - 0.2f) / 0.5f)) : 1.0f);
    float porosity = lerp(0.4f, 0.7f, greyness);	// nothing written (the game's shaders, far LOD, no buffers)
    porosity = materialClass > 0.5f ? porosityObject : porosity;
    porosity = materialClass > 2.5f ? 0.85f : porosity;	// cloth
    porosity = materialClass > 3.5f ? porosityLand : porosity;
    porosity = materialClass > 4.5f ? 0.15f : porosity;	// plants (waxy)
    porosity *= 1.0f - metal;

    // Wetness: rain history x open sky in front of the surface x how much it faces up x distance
    float roofOffset = lerp(4.0f, WetRoofOffset, saturate(1.0f - dotUp));
    float3 roofProbe = worldPosRelRoof.xyz + normal * roofOffset;
    float roofOpen = GetRoofMask(mul(float4(roofProbe, 1.0f), TESR_ShadowCameraToLightTransformOrtho));
    float4 cover = persist ? CoverMapRead(roofProbe.xy) : float4(0, 0, 0, 0);
    float rainHistory = persist ? cover.x : rainFade;
    float exposure = saturate(WetWallExposure + (1.0f - WetWallExposure) * dotUp);
    // P8: walls dry out between WetWallFadeStart and WetWallFadeEnd (wet = 0 takes the early exit below); N.z >= 0.5 unchanged
    float wallFar = (1.0f - smoothstep(0.40f, 0.50f, dotUp)) * smoothstep(WetWallFadeStart, WetWallFadeEnd, depth);
    float wet = rainHistory * roofOpen * exposure * LODfade * (1.0f - vmMask) * notActor * (1.0f - wallFar);

    float puddleFade = smoothstep(PuddleAccumulationStart, PuddleAccumulationFull,
        saturate(persist ? CoverMapRead(camera_vector.xy).y : TESR_WetWorldData.z));
    float puddlemask = puddles * puddleFade * notActor;
    float puddleWeight = puddlemask * LODfade;

    float depthish = saturate(puddlemask * 1.15f);
    depthish = depthish * depthish;
    float rim = 4.0f * puddlemask * (1.0f - puddlemask);
    float rimDark = rim * 0.15f;

    // Ripples where rain lands on flat wet ground (as Wet's masks; the first-person ones are kept for its composition)
    float rippleFlatMask = smoothstep(0.65f, 0.92f, dotUp);
    float rippleFlatFinal = lerp(rippleFlatMask, rippleFlatMask * vmFlatMask, vmMask);
    float rippleMask = rainVis * rippleFlatFinal;
    rippleMask *= lerp(1.0f, vmFlatMask, vmMask);
    float flatCoverage = roofMask * rippleFlatMask;
    rippleMask = lerp(rainFade * flatCoverage, rippleMask, vmMask);
    float flatWet = wet * rippleFlatMask;
    float wetMask = saturate(max(puddlemask, flatWet * 0.85f));
    float specMask = saturate(max(puddlemask, flatWet + rim * 0.15f) + splashAlpha * 0.55f * splashFlatMask);

    // Vertical streaks (as Wet)
    float streakExposure = (1.0f - smoothstep(0.55f, 0.80f, abs(dotUp))) * (1.0f - vmMask) * rainVis * notActor;
    float streakDistance = length(camera_vector);
    streakExposure *= 1.0f - smoothstep(StreakFadeStart, StreakFadeFull, streakDistance);
    float3 streakSurfaceNormal = normalize(normal);
    float4 streakUV = GetWaterStreakUVs(worldPos.xyz, streakSurfaceNormal);
    float4 streakUVDx = ddx(streakUV);
    float4 streakUVDy = ddy(streakUV);
    float4 streakSample = float4(0, 0, 0, 0);
    [branch]
    if (streakDistance < StreakFadeFull && streakExposure > 0.0f)
    {
        streakSample = SampleWaterStreaks(streakUV, streakSurfaceNormal, streakUVDx, streakUVDy);
    }
    float streakMask = saturate(streakSample.a * streakExposure);
    float3 streakNormalOffset = streakSample.xyz * streakExposure;
    baseColor.rgb *= 1.0f - StreakDarken * streakMask;

    // Ripple normals (as Wet)
    float2 rippleUV_world = worldPos.xy / rippleScale;
    float2 rippleUV_vm = worldPos.xy / (rippleScale * 0.28f * 0.2f);
    float2 rippleUV = lerp(rippleUV_world, rippleUV_vm, vmMask);
    float4 Weights = saturate(float4(1, 0.75, 0.5, 0.25) * TESR_WetWorldData.x * 4);
    float3 Ripple1 = ComputeRipple(rippleUV + float2( 0.25f, 0.0f), time1, Weights.x);
    float3 Ripple2 = ComputeRipple(rippleUV * 1.1 + float2(-0.55f, 0.3f), time2, Weights.y);
    float3 Ripple3 = ComputeRipple(rippleUV * 1.3 + float2( 0.6f, 0.85f), time3, Weights.z);
    float3 Ripple4 = ComputeRipple(rippleUV * 1.5 + float2( 0.5f,-0.75f), time4, Weights.w);
    float4 Z = lerp(1, float4(Ripple1.z, Ripple2.z, Ripple3.z, Ripple4.z), Weights);
    float3 ripple = float3(
        Weights.x * Ripple1.xy + Weights.y * Ripple2.xy + Weights.z * Ripple3.xy + Weights.w * Ripple4.xy,
        Z.x * Z.y * Z.z * Z.w);
    ripple.xy *= 2.2f;
    float3 combinedNormals = normalize(ripple);
    combinedNormals = normalize(lerp(float3(0,0,1), combinedNormals, LODfade * wetMask));
    combinedNormals.xy += splashBumpXY * 2.0f;
    combinedNormals = normalize(combinedNormals);

    // Puddle waves (as Wet)
    float t = TESR_GameTime.z;
    float p = worldPos.x;
    float2 swell;
    swell.x = sin(p * 0.260f + t * 3.10f) + 0.55f * sin((p + worldPos.y) * 0.200f + t * 3.90f);
    swell.y = sin(p * 0.240f + t * 3.40f) + 0.55f * sin((p + worldPos.x) * 0.210f + t * 3.60f);
    float distFade = (1.0f - smoothstep(400.0f, 1800.0f, depth));
    float swellAmp = 0.0030f * pow(puddlemask, 1.35f) * distFade;
    combinedNormals.xy += swell * swellAmp;
    combinedNormals = normalize(combinedNormals);

    // Puddle texture normals (as Wet)
    float3 puddleNormalSurface = TESR_CameraPosition.xyz + worldPosRelRoof.xyz;
    float2 puddleSurfaceUV = puddleNormalSurface.xy / max(PuddleCloudWorldSize, 1.0f);
    float2 puddleNormalUV = puddleSurfaceUV + TESR_GameTime.z * PuddleCloudDrift;
    puddleNormalUV += combinedNormals.xy * PuddleCloudRippleDistortion;
    float2 puddleNormalUV2 = puddleSurfaceUV * 1.37f + float2(0.23f, 0.61f) - TESR_GameTime.z * PuddleCloudDrift * 0.65f;
    puddleNormalUV2 += combinedNormals.xy * PuddleCloudRippleDistortion;
    float2 puddleTexNormalA = tex2D(TESR_PuddleNormalSampler, puddleNormalUV).xy * 2.0f - 1.0f;
    float2 puddleTexNormalB = tex2D(TESR_PuddleNormalSampler, puddleNormalUV2).xy * 2.0f - 1.0f;
    float2 puddleTexNormal = lerp(puddleTexNormalA, puddleTexNormalB, 0.35f);
    puddleTexNormal.y *= PuddleNormalYSign;
    float flatNormalStrength = FlatPuddleNormalStrength * saturate(flatWet) * (1.0f - vmMask);
    float textureNormalStrength = lerp(flatNormalStrength, PuddleNormalStrength, puddlemask);
    float2 puddleNormalOffset = puddleTexNormal * textureNormalStrength * LODfade;
    combinedNormals = normalize(float3(combinedNormals.xy + puddleNormalOffset, combinedNormals.z));

    // Surface normals: the depth normal with the albedo's bump (Model 1), flattened to up in puddles, the ripples on flat wet ground
    float bumpHeight = albedoValid ? albedoLuma * WetBumpHeight : 0.0f;
    float3 bumpedNormal = WetBumpNormal(normal, worldPosRelRoof.xyz, bumpHeight);
    float3 baseN = normalize(lerp(bumpedNormal, up, puddlemask));
    float3 N = normalize(lerp(baseN, combinedNormals, specMask));
    N = normalize(N + streakNormalOffset);
    // Specular anti-aliasing (Tokuyoshi and Kaplanyan 2019): the normal's variance across the pixel widens the lobe, so the ripples and
    // the bump sparkle instead of flickering.
    float3 dNdx = ddx(N);
    float3 dNdy = ddy(N);
    float normalVariance = min(0.5f * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.18f);

    // Refraction (puddles only in Model 1)
    float NdotV_refr = saturate(dot(N, -V));
    float viewRefr   = smoothstep(0.18f, 0.45f, NdotV_refr);
    float2 refrOff = expand(rectToScreen(projectPosition(combinedNormals).xy)) * TESR_ReciprocalResolution.xy * refractionScale;
    float splashRefr = 0.35f * splashAlpha * splashFlatMask;
    refrOff += splashBumpXY * (refractionScale * 0.35f) * TESR_ReciprocalResolution.xy * splashRefr;
    refrOff *= 0.35f;
    refrOff *= viewRefr;
    refrOff += swell * ((swellAmp * 0.50f) * viewRefr);
    float2 refrClamp = TESR_ReciprocalResolution.xy * (refractionScale * 0.75f);
    refrOff = clamp(refrOff, -refrClamp, refrClamp);

    // Every implicit-derivative read before the first branch (as Wet's fast path)
    float4 cloudSample = tex2D(TESR_PuddleCloudSampler, GetPuddleCloudUV(worldPosRelRoof, combinedNormals));
    float3 refrLin = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord + refrOff)).rgb;
    float vmSampleValid = GetViewModelMask(saturate(IN.UVCoord + refrOff));
    float4 shadows = tex2D(TESR_PointShadowBuffer, IN.UVCoord);

    [branch]
    if (depth > DrawD || isWaterSurface || shadows.y < -1.0f)
        return float4(WetOut(baseColorIn.rgb + splashAdd), baseColorIn.a);

    // the SSR trace condition (Wet's)
    float lingeringReflectionMask = puddleFade * flatCoverage;
    float ssrMask = saturate(max(rippleMask, max(lingeringReflectionMask, puddlemask)));
    float ssrScreenOpacity = smoothstep(PuddleSSRScreenFadeStart, PuddleSSRScreenFadeFull, IN.UVCoord.y);
    float ssrAngleOpacity = 1.0f - smoothstep(PuddleSSRFadeStart, PuddleSSRFadeFull, saturate(dot(-V, up)));
    float ssrOpacity = ssrScreenOpacity * ssrAngleOpacity;
    float ssrTraceOpacity = saturate(PuddleSSRCandidateBlend) * saturate(ssrMask * LODfade) * ssrOpacity;
    bool traces = fixTrace && ssrMask > 0.001f && ssrOpacity > 0.0f && ssrTraceOpacity > PuddleSSRMinTraceOpacity;

    [branch]
    if (vmMask <= 0.5f && !(wet > 0.0f) && !(puddleWeight > 0.0f) && !(streakMask > 0.0f) && !traces)
        return float4(WetOut(baseColor.rgb + splashAdd), 1.0f);

    float3 baseLin = linearize(baseColor).rgb;

    // First-person composition (as Wet)
    [branch]
    if (vmMask > 0.5f)
    {
        float3 vmRefrLin = lerp(baseLin, refrLin, vmSampleValid);
        float vmRippleBlend = saturate(LODfade * rippleMask) * viewRefr;
        float3 vmWetLin = lerp(baseLin, vmRefrLin, vmRippleBlend);
        return float4(WetOut(delinearize(float4(vmWetLin, 1.0f)).rgb + splashAdd), baseColor.a);
    }

    float sunAmbient = max(luma(linearize(TESR_SunAmbient)), 0.0001f);
    float inShadow = saturate(pow((shadows.r + shadows.g) / sunAmbient, 5));
    float sunHeight = shade(TESR_SunPosition.xyz, up);

    // Screen-space reflection (fixed trace only, as Wet's fast path)
    float3 ssrHit = float3(0, 0, 0);
    [branch]
    if (traces)
    {
        float3 ssrNormal = normalize(lerp(baseN, N, saturate(PuddleSSRNormalInfluence)));
        ssrHit = TraceWetReflectionFixed(IN.UVCoord, ssrNormal);
    }
    float ssrCoverage = saturate(ssrHit.z * ssrMask * LODfade) * ssrOpacity;

    // The water layer's roughness and the darkened diffuse
    float dryRough = surfaceRough >= 0.0f ? surfaceRough : WetDryRoughness;
    // Porous ground soaks the water up (it darkens, its roughness barely changes) and only a saturated or non-porous surface holds a
    // smooth film (Lagarde, "Water drop 2"): the film's share and smoothness fall with porosity. Bench 2026-11-05: with a full film on
    // all wet ground, sand and dirt became sky-coloured mirrors (frost look).
    // (bench 2026-11-05 round 2: (1 - 0.85 p) still left a blue sky sheen on sand: sand 0.03, concrete 0.12, sealed 0.6 of the wetness now)
    float filmShare = wet * pow(saturate(1.0f - porosity), 1.5f);
    float wetRough = lerp(dryRough, WetFilmRoughness, filmShare);
    wetRough = materialClass > 2.5f && materialClass < 3.5f ? max(wetRough, 0.6f) : wetRough;	// cloth soaks, never glossy
    float roughness = lerp(wetRough, WetPuddleRoughness, puddlemask);
    float film = saturate(max(filmShare, puddleWeight));	// how much of the pixel the water layer covers

    float diffuseShare = (1.0f - metal) * (surfaceRough >= 0.0f ? lerp(0.8f, 1.0f, surfaceRough) : 0.95f);
    float3 chromaRatio = pow(max(albedo, 1e-4f.xxx) / albedoLuma, WetSaturation * porosity);	// at p x w = 1; raised to w below
    float3 darken = min(lerp(1.0f, WetDarkenFloor, porosity * wet) * pow(chromaRatio, wet), 1.0f.xxx);
    float3 surfaceLin = baseLin * lerp(1.0f.xxx, darken, diffuseShare);

    // Puddles: the refracted bottom, soaked through (w = 1), with Amount, the depth tint and the rim (as Wet)
    float3 puddleBase = lerp(baseLin, refrLin, viewRefr);
    puddleBase *= lerp(1.0f.xxx, min(lerp(1.0f, WetDarkenFloor, porosity) * chromaRatio, 1.0f.xxx), diffuseShare);
    puddleBase *= lerp(1.0f, 0.55f, TESR_WetWorldData.w);
    puddleBase *= lerp(1.0f, float3(0.92f, 0.97f, 1.05f), depthish * 0.25f);
    puddleBase *= (1.0f - depthish * 0.18f);
    puddleBase *= (1.0f - rimDark);
    float3 groundLin = lerp(surfaceLin, puddleBase, puddleWeight);

    // The environment: the sky in the (roughness-bent) mirror direction, the cloud texture over puddles, the SSR hit where it found one
    float NdotV = max(dot(N, -V), 1e-4f);
    float3 R = reflect(V, N);
    float roughAlpha = roughness * roughness;
    float3 Rd = normalize(lerp(N, R, (1.0f - roughAlpha) * (sqrt(1.0f - roughAlpha) + roughAlpha)));	// Karis' dominant direction
    float3 environment = WetSkyEnvironment(Rd, sunHeight);
    float3 cloudDetail = saturate((cloudSample.rgb - 0.5f) * PuddleCloudContrast + 0.5f);
    float cloudCoverage = saturate(saturate((cloudSample.a - 0.5f) * PuddleCloudContrast + 0.5f) * PuddleCloudStrength) * puddlemask;
    environment = lerp(environment, environment * lerp(0.35f.xxx, 1.25f.xxx, cloudDetail), cloudCoverage);
    // Finding 16: the sky only as far as the reflection ray reaches it (WetSkyVisibility); the rest is the facades and roofs it meets,
    // stood in for by this pixel's own dry lit colour (lit by the same light). The SSR hit, where there is one, still replaces both.
    float skyVis = WetSkyVisibility(worldPosRelRoof.xyz + normal * 4.0f, Rd);
    environment = lerp(baseLin, environment, skyVis);
    [branch]
    if (ssrHit.z > 0.001f)
    {
        float3 reflectionLin = linearize(tex2Dlod(TESR_SourceBuffer, float4(ssrHit.xy, 0, 0))).rgb;
        environment = lerp(environment, reflectionLin, ssrCoverage);
    }
    float fresnel = WetEnvBRDFApprox(WetFilmF0, roughness, NdotV) * film;

    // Highlights: the sun and the lamps through the same GGX lobe
    float lightRough = max(roughness, WetLightMinRoughness);
    float alpha2 = saturate(lightRough * lightRough * lightRough * lightRough + normalVariance);
    float3 toEye = -V;
    float3 Ls = normalize(TESR_SunDirection.xyz);
    float3 sunLin = linearize(TESR_SunColor).rgb;
    float3 sunSpecCol = sunLin * (WetGGX(N, toEye, Ls, alpha2) * inShadow);

    float sunSpecMul = lerp(0.25f, 0.70f, saturate(sunHeight));
    float3 streakSunCol = float3(0, 0, 0);
    [branch]
    if (streakMask > 0.0f)
    {
        float streakSunSpec = pow(saturate(dot(N, normalize(Ls - V))), StreakSunSpecularPower) * streakMask * StreakSpecularStrength;
        streakSunCol = sunLin * streakSunSpec * sunSpecMul * inShadow;
    }

    float3 pointSpecCol = float3(0, 0, 0);
    float3 streakPointCol = float3(0, 0, 0);
    float anyLamp = 0.0f;
    [unroll]
    for (int j = 0; j < 12; j++)
        anyLamp = max(anyLamp, max(TESR_ShadowLightPosition[j].w, TESR_LightPosition[j].w));
    [branch]
    if (anyLamp > 0.0f && (film > WetLampMinFilm || streakMask > 0.0f))	// P8: only where the highlights can show
    {
        [unroll]
        for (int i = 0; i < 12; i++)
        {
            AddLampHighlightPBR(TESR_ShadowLightPosition[i], TESR_LightColor[i].rgb, worldPos.xyz, toEye, N, alpha2, streakMask,
                pointSpecCol, streakPointCol);
            AddLampHighlightPBR(TESR_LightPosition[i], TESR_LightColor[i + 12].rgb, worldPos.xyz, toEye, N, alpha2, streakMask,
                pointSpecCol, streakPointCol);
        }
    }
    float3 highlights = min((sunSpecCol + pointSpecCol) * film, WetSpecLimit.xxx);

    // Composition: the light through the layer, plus what the layer reflects
    float3 outLin = groundLin * (1.0f - fresnel) + environment * fresnel + highlights;
    float3 wetRGB = delinearize(float4(max(outLin, 0.0f.xxx), 1.0f)).rgb;

    // Streak lighting (as Wet)
    [branch]
    if (streakMask > 0.0f)
    {
        float3 streakLighting = min(streakSunCol + streakPointCol, (StreakHighlightLimit * streakMask).xxx);
        float3 wetLinear = linearize(float4(wetRGB, 1.0f)).rgb;
        wetRGB = delinearize(float4(wetLinear + streakLighting * LODfade, 1.0f)).rgb;
    }
    return float4(WetOut(wetRGB + splashAdd), 1.0f);
}

// UNOFFICIAL 2026-10-14 (HudDrops, Shaders/Includes/HudRain.hlsl): is the camera under cover? One pixel, kept (counted by an
// occlusion query, WetWorldEffect::RenderCoverTest) only if something in the ortho map lies above the camera (48 units up:
// above the player's own head, below any roof worth the name). Outside the ortho map the sky is open. After mrfury97's HUD
// raindrops (Physically-Based-Reloaded testbed), which read the same map for the same test.
float4 CoverTest(VSOUT IN) : COLOR0
{
    float4 o = mul(float4(0.0f, 0.0f, 48.0f, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    o.xyz /= o.w;
    float2 ouv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
    float top = SampleOrthoDepth(ouv);
    bool inside = all(ouv > 0.0f) && all(ouv < 1.0f);
    clip(inside && o.z > top + 0.003f ? 1.0f : -1.0f);
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}

// Render technique
technique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Wet(false, false, false);
    }
}

technique CoverTestTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 CoverTest();
    }
}

// UNOFFICIAL 2026-10-17 FastPaths ([Shaders.WetWorld.Main] FastPaths): technique 2, the same passes with Wet(true).
technique FastPathsTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Wet(true, false, false);
    }
}

// UNOFFICIAL 2026-10-17 FixedReflectionTrace ([Shaders.WetWorld.Main]): techniques 3 (the original pass) and 4 (FastPaths) with the
// corrected puddle reflection hit test. The C++ side picks the technique (WetWorldEffect::MainTechnique).
technique FixedTraceTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Wet(false, true, false);
    }
}

technique FixedTraceFastPathsTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Wet(true, true, false);
    }
}

// UNOFFICIAL 2026-10-27 Persistent ([Shaders.WetWorld.Main] Persistent, CoverMap.h): techniques 5 (FastPaths) and 6 (FastPaths with the
// fixed reflection trace) read the wetness and the puddle water per spot from the cover map. (Persistent always takes the FastPaths
// pass: the same picture as the original, and one compile less.)
technique PersistentTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Wet(true, false, true);
    }
}

technique PersistentFixedTraceTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 Wet(true, true, true);
    }
}

// UNOFFICIAL 2026-11-05 Model 1 ([Shaders.WetWorld.Main] Model = 1): techniques 7 (FastPaths), 8 (fixed trace), 9 (Persistent) and 10
// (Persistent, fixed trace) draw the wet pass with WetPBR. WetWorldEffect::Render maps 0 and 2 to 7, 3 and 4 to 8, 5 to 9, 6 to 10.
technique PbrTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetPBR(false, false);
    }
}

technique PbrFixedTraceTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetPBR(true, false);
    }
}

technique PbrPersistentTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetPBR(false, true);
    }
}

technique PbrPersistentFixedTraceTechnique
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetMap();
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(1.0f, 0.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 BlurWetMap(float2(0.0f, 1.0f), PuddleBlurRadius);
    }
    pass
    {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader  = compile ps_3_0 WetPBR(true, true);
    }
}
