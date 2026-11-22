// Volumetric Fog fullscreen shader for New Vegas Reloaded
//
// Single unified density/scattering term (replaces the old Simple+Height fog stack), anchored to
// vanilla weather fog (TESR_FogData/TESR_FogColor) via WeatherImpact, coupled to the sun shadow
// cascades and the SH sky ambient, animated with wind-driven procedural 3D noise. Distant fog
// (horizon Z-fighting/sky-seam matte) is kept as its own separate blend, since it solves a
// different problem (hiding a depth-buffer artifact, not modelling participating media).

// UNOFFICIAL 2026-10-27 (user: "tint horizon still seems to do nothing"): the sky colours below are the fog's own copies, with the
// sky's tint, horizon tint and saturation already in them (Sky.cpp UpdateFogSky): near the horizon the fog lies over the sky and
// coloured itself with the weather's untinted sky, painting the horizon tint over. The same three registers as before.
#define TESR_HorizonColor TESR_FogHorizonColor
#define TESR_SkyLowColor TESR_FogSkyLowColor
#define TESR_SkyColor TESR_FogSkyColor
// UNOFFICIAL 2026-10-30 WeatherFogColor: the weather's fog colour is the fog's own copy too, with the sky look on top of it outdoors
// while today's fog takes it (ModernFogTerms); the raw colour indoors, with WeatherFogColor 0, LegacyFog, ClassicLook and the
// FogReference test (Sky.cpp UpdateFogSky). The same register as before.
#define TESR_FogColor TESR_FogWeatherColor

float4 TESR_FogColor;
float4 TESR_FogData;  // weather values : x: fogNear, y:fogFar, z: sunglare, w:fogpower
float4 TESR_ReciprocalResolution;
float4 TESR_GameTime; // x: game time (ms), y: game hour, z: time, w: delta time
float4 TESR_SmoothedSunDir;
float4 TESR_SunPosition;
float4 TESR_SunColor;
float4 TESR_SunDiskColor;
float4 TESR_SunAmbient;
float4 TESR_HorizonColor;
float4 TESR_SkyLowColor;
float4 TESR_SkyColor; // top sky color
float4 TESR_SkyData; // x: athmosphere thickness, y: sun influence, z: sun strength w: sky strength
float4 TESR_SunsetColor; // color boost for sun when near the horizon
float4 TESR_SunAmount; // x: isDaytime
float4 TESR_PBRData; // z: direct light scale, w: ambient scale -- shared with objects for tonal consistency
float4 TESR_SkyIrradiance[9]; // order-2 SH sky irradiance, linear radiance (Sky.cpp, only fresh while Shaders.Sky is enabled)
// UNOFFICIAL 2026-10-18 ClassicLook ([Shaders.VolumetricFog.Main]): the same projection of the game's own sky alone, as the
// original NVR fog (upstream master ab16e86c) had it; TESR_SkyIrradiance above also carries the physical sky when that is on.
// Only the *Classic techniques and FogReference read it, and only in their preshaders (no shader constant registers).
float4 TESR_SkyIrradianceGame[9];
#if NVR_SM6
// UNOFFICIAL 2026-10-26 (deep/fork-sm6) [Shaders.Sky.Main] sky look, ONLY in the SM6 build of these passes (NVR DXVK fork,
// tools/nvr-sm6: no 224-register limit). Near the horizon the fog lies over the sky and colours itself with the weather's untinted
// sky, so the sky's horizon tint and saturation were painted over. Here the fog's sky colour takes them as the sky does at the
// horizon (Includes/Sky.hlsl SkyLook): TESR_FogSkyLook.rgb = SkyTint x SkyTintHorizon (Sky.cpp), TESR_SkyLook.x = saturation.
// The ps_3_0 build (native D3D9, stock DXVK) cannot take it: with the physical sky the composite pass is at 224 constants and even
// one more register failed to compile (X4507, cfe15f2b reverted by 595a7c22), so there the fog stays as it was.
// 2026-10-27: no longer applied here: the sky colours come in with it (TESR_FogSkyColor and its two siblings, top of this file),
// in every build. Kept as an empty macro so the call sites need no #if.
#define NVR_FOG_SKY_LOOK(c)
#else
#define NVR_FOG_SKY_LOOK(c)
#endif

// sun shadow cascade data -- registered globally by ShadowsExteriorEffect, same constants
// SunShadows.fx.hlsl consumes. Declared here independently since each .fx effect compiles on
// its own; see GetFogShadowVisibility below.
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformMiddle;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformLod;
float4 TESR_ShadowNearCenter;   // xyz: center (world space), w: radius
float4 TESR_ShadowMiddleCenter;
float4 TESR_ShadowFarCenter;
float4 TESR_ShadowLodCenter;
float4 TESR_ShadowFormatData; // x: mode (0 VSM, 1 EVSM2, 2 EVSM4), y: format bits per pixel
float4 TESR_ShadowBlur;       // x: 1 / atlas resolution
float4 TESR_ShadowFade;       // y: shadow maps active

float4 TESR_VolumetricFogDensity;    // x: BaseDensity, y: WeatherImpact, z: MorningFogDip, w: SunriseSunsetBoost
float4 TESR_VolumetricFogShape;      // x: HeightFalloff, y: MaxHeight, z: Extinction, w: Inscattering
float4 TESR_VolumetricFogWind;       // x: WindDirX, y: WindDirY, z: WindSpeed, w: NoiseScale
float4 TESR_VolumetricFogScatter;    // x: PhaseAsymmetry, y: ShadowStrength, z: NoiseStrength, w: HeightInfluence
float4 TESR_VolumetricFogWeather;    // x: WeatherFilterBlend (animated 0-1), y: isExterior, z: SkyAmbientAvailable, w: FogSaturation
float4 TESR_VolumetricFogAerial;     // x: AerialStrength, y: AerialRangeStart, z: AerialTintBlend, w: AerialDayFadeStart
float4 TESR_VolumetricFogAerialTint; // xyz: manual aerial tint override
float4 TESR_VolumetricFogDistant;    // x: DistantFogRange, y: DistantFogBlend, z: DistantFogHeight, w: EdgeAA
float4 TESR_VolumetricFogGlobal;     // x: Amount, y: NightDisableSkyMask, z: MoonVisibility, w: MinDensityFloor
// Own settings-UI section/tab (Shaders.VolumetricFog.Night), not Main/Interiors-switched, so these
// don't crowd Main as more of them get added.
float4 TESR_VolumetricFogNight;        // x: DensityScale, y: AmountScale, z: HeightFalloffScale, w: MaxHeightOffset
float4 TESR_VolumetricFogNightScatter; // x: NoiseStrengthScale, y: WindSpeedScale, z: ExtinctionScale, w: InscatteringScale
float4 TESR_VolumetricFogSun;          // UNOFFICIAL 2026-10-15 x: SunGlow (1 = the glow round the sun as inherited from NVR); 2026-10-30 y: WeatherFogColor;
                                      // 2026-10-30 z: GameFogFloor this frame (0 = off / indoors), w: the weather's raw fog near
float4 TESR_VolumetricFogNightTint;    // xyz: multiplicative night fog color tint, w: NightAmbientStrength

sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_RenderedBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowAtlas : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL: the general 3D noise (Textures\Effects\NoiseVolume.dds, tools/cloud_noise.cpp; g: 4-octave fbm, base
// period 4 cells across the tile). Bound by VolumetricFogEffect::Render on stage 9: not a TESR_ sampler, because those
// take stages by declaration order and the next one (6) is the dedicated path's NVR_FogBuffer.
sampler3D NVR_FogNoise : register(s9) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

/*Height-based fogAcc settings*/
static const float vfFOG_GROUND = -10000;
static const float vfSUNINFLUENCE = 1 / TESR_SkyData.y;
static const float vfNearFog = TESR_FogData.x;
static const float vfFarFog = TESR_FogData.y;
static const float vfFogPower = TESR_FogData.w;

// scale settings for easier tuning
static const float vfFogAmount = max(0, TESR_VolumetricFogGlobal.x);
// Night-scoped (read from Shaders.VolumetricFog.Night, not the Main/Interiors switch), but packed
// into Global's spare slot rather than a whole new vector for one bool -- see skyMaskFactor's use
// of this below for why it's here instead of alongside the other Night* settings.
static const float vfNightDisableSkyMask = TESR_VolumetricFogGlobal.y;
// Computed in C++ (VolumetricFog.cpp), mirrors ShadowsExterior's own moon-phase shadow-fade curve.
static const float vfMoonVisibility = saturate(TESR_VolumetricFogGlobal.z);
// Height-agnostic floor on density -- see MinDensityFloor's use near the height/flat fog blend
// below for why this exists (a single exponential height falloff can't be tuned to read as rich
// ground fog in low-lying terrain AND still have any presence well above that reference).
static const float vfMinDensityFloor = saturate(TESR_VolumetricFogGlobal.w);
// Every Night* constant below fades in the same way, via the shared nightFactor computed at the
// top of VolumetricFog() (isExterior-gated, ramping through dusk via (1-isDayTime)) -- see there.
// All default to a neutral no-op (1.0 for scales/tint, 0.0 for the additive height offset), so
// none of this changes anything until a setting is actually moved off its default.
static const float vfNightDensityScale = max(0, TESR_VolumetricFogNight.x);
static const float vfNightAmountScale = max(0, TESR_VolumetricFogNight.y);
static const float vfNightHeightFalloffScale = max(0, TESR_VolumetricFogNight.z);
static const float vfNightMaxHeightOffset = TESR_VolumetricFogNight.w;
static const float vfNightNoiseStrengthScale = max(0, TESR_VolumetricFogNightScatter.x);
static const float vfNightWindSpeedScale = max(0, TESR_VolumetricFogNightScatter.y);
static const float vfNightExtinctionScale = max(0, TESR_VolumetricFogNightScatter.z);
static const float vfNightInscatteringScale = max(0, TESR_VolumetricFogNightScatter.w);
static const float3 vfNightTint = max(0, TESR_VolumetricFogNightTint.xyz);
// Multiplier on the moon-phase-driven night ambient floor (see MoonVisibility/ambientColor below).
// Lives here rather than in TESR_VolumetricFogGlobal since it fades via the same shared nightFactor
// as every other Night* setting -- gating is now purely shader-side (nightFactor's isExterior term),
// not C++-side zeroing for interiors like before.
static const float vfNightAmbientStrength = max(0, TESR_VolumetricFogNightTint.w);

static const float vfBaseDensity = max(0, TESR_VolumetricFogDensity.x);
static const float vfWeatherImpact = max(0, TESR_VolumetricFogDensity.y);
static const float vfMorningFogDip = saturate(TESR_VolumetricFogDensity.z);
static const float vfSunriseSunsetBoost = max(0, TESR_VolumetricFogDensity.w);

static const float vfHeightFalloff = max(0.0001, TESR_VolumetricFogShape.x);
static const float vfMaxHeight = TESR_VolumetricFogShape.y;
static const float vfExtinction = max(0, TESR_VolumetricFogShape.z) * 0.1;
static const float vfInscattering = max(0, TESR_VolumetricFogShape.w) * 0.1;

static const float2 vfWindDirection = TESR_VolumetricFogWind.xy;
static const float vfWindSpeed = TESR_VolumetricFogWind.z;
static const float vfNoiseScale = max(0.0001, TESR_VolumetricFogWind.w);

static const float vfPhaseAsymmetry = saturate(TESR_VolumetricFogScatter.x);
static const float vfShadowStrength = saturate(TESR_VolumetricFogScatter.y);
static const float vfNoiseStrength = saturate(TESR_VolumetricFogScatter.z);
// 1 = normal height-based density falloff (exterior default). 0 = no height dependence at all,
// density shaped purely by distance -- for interiors, where MaxHeight has no consistent meaning
// across arbitrary cell geometry unless an author explicitly tunes it for a known space.
static const float vfHeightInfluence = saturate(TESR_VolumetricFogScatter.w);

static const float vfWeatherFilterBlend = saturate(TESR_VolumetricFogWeather.x);
static const float vfIsExterior = TESR_VolumetricFogWeather.y; // 0 or 1 to activate/cancel fog in interiors
static const float vfSkyAmbientAvailable = TESR_VolumetricFogWeather.z;
static const float vfFogSaturation = max(0, TESR_VolumetricFogWeather.w);

static const float vfAerialStrength = max(0, TESR_VolumetricFogAerial.x);
static const float vfAerialRangeStart = saturate(TESR_VolumetricFogAerial.y);
static const float vfAerialTintBlend = saturate(TESR_VolumetricFogAerial.z);
// TESR_SunAmount.x value at which aerial perspective reaches full strength. Wider than
// isDayTimeFog's own 0.1-0.6 ramp (used for sun-scattering) so aerial stays suppressed through
// more of the sunrise/sunset transition, not just full night -- that's specifically when direct
// low-angle sun on distant terrain was reading as too bright/glowy.
static const float vfAerialDayFadeStart = saturate(TESR_VolumetricFogAerial.w);

static const float vfDistantFogRange = exp(-4 * clamp(TESR_VolumetricFogDistant.x, 0.00000001, 1.0));
static const float vfDistantFogBlend = TESR_VolumetricFogDistant.y;
static const float vfDistantFogHeight = TESR_VolumetricFogDistant.z;
static const float vfEdgeAA = max(0, TESR_VolumetricFogDistant.w);

// fixed tuning constant: how strongly a high density value pulls the fog's own color toward
// TESR_FogColor instead of the sky/ambient tint. Was two separate exposed settings
// (SimpleFogSkyColor/HeightFogSkyColor) before the two systems were unified.
static const float vfFogSkyColorCoeff = 6.0;

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"
#include "Includes/Sky.hlsl"
#include "Includes/Normals.hlsl"

// UNOFFICIAL 2026-10-14 physical sky ([Shaders.Sky.Physical], Effects/SkyView.fx.hlsl, Sky.cpp): the fog takes the sky colour it
// fades toward from the physical sky in the direction it is seen, as SKY.pso shows the sky itself, so fog and sky agree (a
// weather whose fog covers the whole dome would otherwise hide the physical sky entirely). Compiled in only when on at
// startup (PHYSICAL_SKY_ON, EffectRecord::LoadEffect). The sky's table is bound by hand on stage 10 (VolumetricFog.cpp):
// it is not a TESR_ sampler, as TESR_ samplers bind by their order in the file.
#ifndef PHYSICAL_SKY_ON
    #define PHYSICAL_SKY_ON 0
#endif
#if PHYSICAL_SKY_ON
float4 TESR_PhysSky;        // x: share of the physical sky, y: brightness, z: weather tint, w: haze
float4 TESR_PhysSkySun;     // xyz: the sun's real direction
sampler2D NVR_FogSkyView : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// SKY.pso.hlsl's blend for the direction dir, over the game's sky colour gameSky (already at the sky's strength).
float3 PhysicalFogSky(float3 dir, float3 gameSky) {
    float horizontal = length(dir.xy), sunHorizontal = length(TESR_PhysSkySun.xy);
    float cosBearing = (horizontal > 1e-4f && sunHorizontal > 1e-4f) ? dot(dir.xy, TESR_PhysSkySun.xy) / (horizontal * sunHorizontal) : 1.0f;
    float2 tableUV = float2(acos(clamp(cosBearing, -1.0f, 1.0f)) / 3.14159265f, sqrt(asin(saturate(dir.z)) / 1.57079633f));
    float3 phys = tex2Dlod(NVR_FogSkyView, float4(tableUV, 0.0f, 0.0f)).rgb * (0.001f * TESR_PhysSky.y * TESR_SunsetColor.w);
    float3 weatherHue = gameSky / max(luma(gameSky), 1e-6f);
    return lerp(gameSky, lerp(phys, luma(phys) * weatherHue, TESR_PhysSky.z), TESR_PhysSky.x);
}
#endif

// UNOFFICIAL 2026-10-14 froxel grid ([Shaders.VolumetricLight.Main] Froxels, VolumetricLight.fx.hlsl): when the volumetric
// light's grid is built this frame it is bound here by hand on stage 11 (VolumetricFog.cpp; not a TESR_ sampler, for the
// reason above). Then (NVR_FogGridFlags) x: the fog's sunlight is shaded by the share of the air along the view that the
// sun reaches, read from the grid, instead of by the shadow at the far end of the view (four cascade reads), and y: the
// volumetric light's composite is done here, after the fog, instead of in a full-resolution pass of its own.
float4 NVR_FogGridFlags;
float4 TESR_VolumetricLightData;       // x: strength, y: shade darkening, z: air density per game unit, w: march distance
float4 TESR_VolumetricLightData2;      // y: phase asymmetry g, z: fade (shadows on, rain)
float4 TESR_VolumetricLightGridLayout; // xy: grid columns and rows, zw: one atlas texel in UV
float4 TESR_VolumetricLightGridData;   // x: fog volume on (the grid's g, b, a are filled), y: scale of the lit share (r)
sampler2D NVR_FogGrid : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// The grid's sums from the eye out to the planar depth given (the sky's: the march distance), see VolumetricLight.fx.hlsl.
float4 FogGridSum(float2 uvV, float dpt) {
	float marchDistance = TESR_VolumetricLightData.w;
	float planar = dpt >= farZ * 0.996 ? marchDistance : min(dpt, marchDistance);
	float s = sqrt(planar / marchDistance) * 64.0;
	float z = min(floor(s), 63.0);
	float2 gridSize = TESR_VolumetricLightGridLayout.xy;
	float2 local = clamp(uvV * gridSize, 0.5, gridSize - 0.5);
	float2 farTile = float2(fmod(z, 8.0), floor(z / 8.0));
	float2 nearTile = float2(fmod(z - 1.0, 8.0), floor((z - 1.0) / 8.0));
	float4 farSide = tex2Dlod(NVR_FogGrid, float4((farTile * gridSize + local) * TESR_VolumetricLightGridLayout.zw, 0, 0));
	float4 nearSide = z > 0.5 ? tex2Dlod(NVR_FogGrid, float4((nearTile * gridSize + local) * TESR_VolumetricLightGridLayout.zw, 0, 0)) : 0.0;
	return lerp(nearSide, farSide, saturate(s - z));
}

// x the share of the view the lit air scatters towards the eye, y the same if all the air were lit (VolumetricLight's
// CompositeGridPS).
float2 FogGridAir(float2 uvV, float dpt) {
	float marchDistance = TESR_VolumetricLightData.w;
	float planar = dpt >= farZ * 0.996 ? marchDistance : min(dpt, marchDistance);
	float sigma = TESR_VolumetricLightData.z * length(toWorld(uvV));
	return float2(FogGridSum(uvV, dpt).r / TESR_VolumetricLightGridData.y, 1.0 - exp(-sigma * planar));
}

// UNOFFICIAL 2026-11-05 MsaaEdgeFix ([Main.Main.Quality], NVR_FogGridFlags.z = on and this frame's sky shares made): with the game's MSAA,
// each pixel's exact sky share (core/SkyCoverage.h), bound by hand on stage 15 (a TESR_ sampler would take stage 6, the fog buffer's).
// The air of a pixel by that share, as VolumetricLight.fx.hlsl MsaaAirMix (Includes/MsaaSkyEdge.hlsl): with transparency multisampling a
// see-through cutout (a bush, a pylon's lattice) covers part of each pixel's samples, but the one depth read here is the nearest
// sample's (NVR DXVK) or sample 0's: the see-through part took the air up to the cutout, not the sky's (the bush a solid shape in the
// light). A surface pixel's sky share takes the whole view's air. This file is at ps_3_0's 224 constants: a second grid lookup (the air at
// the far plane, to blend the two as MsaaAirMix does) took the composites past them, so the depth the one lookup is made at is blended
// instead, from the surface's toward the end of the grid by the sky share. The air builds up almost evenly along a view (at the default
// Density and MaxDistance a whole view is about a tenth opaque), so that is within a few percent of blending the two. (A sky pixel's
// surface share under a sample-0 resolve, MsaaAirMix's other half, is left out: the DXVK fork resolves the nearest sample.)
sampler2D NVR_FogSkyCoverage : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// The depth to read the grid at: the planar depth the grid would take (the march distance for the sky, else at most it), blended toward
// the march distance by the sky share. With share 0 the lookup is the one before (min(depth, MaxDistance) is what FogGridAir takes), the
// sky's is unchanged (share 1). (No branch and no flag read: either took a constant register this shader does not have. The fix off,
// VolumetricFog.cpp binds nothing on stage 15, which reads 0: share 0.)
float FogGridDepthMsaa(float2 uvV, float dpt) {
	float share = tex2Dlod(NVR_FogSkyCoverage, float4(uvV, 0, 0)).r;
	float marchDistance = TESR_VolumetricLightData.w;
	return lerp(min(dpt, marchDistance), marchDistance, share);
}

// The volumetric light's composite (VolumetricLight.fx.hlsl CompositeAir) on a gamma-encoded colour.
float3 FogGridLight(float3 gammaColor, float2 uvV) {
	float2 air = FogGridAir(uvV, FogGridDepthMsaa(uvV, readDepthLod(uvV)));	// (UNOFFICIAL 2026-11-05 MsaaEdgeFix, NVR_FogGridFlags.z)
	float cosTheta = dot(normalize(toWorld(uvV)), TESR_SmoothedSunDir.xyz);
	float g = TESR_VolumetricLightData2.y;
	float phase = min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-4), 1.5), 8.0);
	float fade = TESR_VolumetricLightData2.z;
	float3 lin = linearize(max(gammaColor, 0.0));
	float shadedAir = saturate(air.y - air.x);
	lin = lin * (1.0 - TESR_VolumetricLightData.y * fade * shadedAir) + linearize(TESR_SunColor.rgb) * (TESR_VolumetricLightData.x * fade * phase * air.x);
	return delinearize(max(lin, 0.0));
}

struct VSOUT
{
	float4 clipPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 clipPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT outV = (VSOUT)0.0f;
	outV.clipPos = IN.clipPos;
	outV.UVCoord = IN.UVCoord;
	return outV;
}


// The same noise as SnowAccumulation.fx.hlsl's snow coverage.
// 3D value noise: a random value in [0, 1) at each whole-number lattice point, blended between the 8 corners of the cell
// with a smoothstep curve. Output in [0, 1), mean 0.5, one lattice cell per unit. Written for Vegas Shaders (2026-11-15);
// it replaces upstream's copy of a Shadertoy noise under CC BY-NC-SA 3.0 (not GPL-compatible), with the same range and scale.
// Lattice hash: Dave Hoskins' "Hash without Sine" hash13, MIT, Copyright (c) 2014 David Hoskins (shadertoy.com/view/4djSRW).
float hash(float3 p3)
{
	p3 = frac(p3 * 0.1031);
	p3 += dot(p3, p3.zyx + 31.32);
	return frac((p3.x + p3.y) * p3.z);
}

float noise(float3 x)
{
	float3 iV = floor(x);
	float3 uV = frac(x);
	uV = uV * uV * (3.0 - 2.0 * uV);
	float z0 = lerp(lerp(hash(iV), hash(iV + float3(1, 0, 0)), uV.x),
			lerp(hash(iV + float3(0, 1, 0)), hash(iV + float3(1, 1, 0)), uV.x), uV.y);
	float z1 = lerp(lerp(hash(iV + float3(0, 0, 1)), hash(iV + float3(1, 0, 1)), uV.x),
			lerp(hash(iV + float3(0, 1, 1)), hash(iV + float3(1, 1, 1)), uV.x), uV.y);
	return lerp(z0, z1, uV.z);
}

// 3-octave FBM, recentered from noise()'s [0,1] range around a mid value so it can modulate
// density both up and down rather than only ever thinning it.
float fbm3(float3 pV)
{
	float nV = noise(pV) * 0.55 + noise(pV * 2.13) * 0.3 + noise(pV * 4.7) * 0.15;
	return saturate(nV * 1.4 - 0.2);
}


// order-2 SH sky irradiance reconstruction. Mirrors Shaders/Includes/SkyAmbient.hlsl's mode-0
// path, but returns LINEAR radiance (no final sqrt/gamma-2 encode) since this file already
// works in true linear space throughout via linearize()/delinearize().
float3 SkyIrradianceLinear(float3 wNrm)
{
	float3 nV = wNrm;
	float3 irradiance = TESR_SkyIrradiance[0].rgb
		 + TESR_SkyIrradiance[1].rgb * nV.y
		 + TESR_SkyIrradiance[2].rgb * nV.z
		 + TESR_SkyIrradiance[3].rgb * nV.x
		 + TESR_SkyIrradiance[4].rgb * (nV.x * nV.y)
		 + TESR_SkyIrradiance[5].rgb * (nV.y * nV.z)
		 + TESR_SkyIrradiance[6].rgb * (3.0f * nV.z * nV.z - 1.0f)
		 + TESR_SkyIrradiance[7].rgb * (nV.x * nV.z)
		 + TESR_SkyIrradiance[8].rgb * (nV.x * nV.x - nV.y * nV.y);
	return max(irradiance, 0.0f);
}

// UNOFFICIAL 2026-10-18 ClassicLook: SkyIrradianceLinear on the game's own sky (TESR_SkyIrradianceGame), as master read it.
float3 SkyIrradianceGameLinear(float3 wNrm)
{
	float3 nV = wNrm;
	float3 irradiance = TESR_SkyIrradianceGame[0].rgb
		 + TESR_SkyIrradianceGame[1].rgb * nV.y
		 + TESR_SkyIrradianceGame[2].rgb * nV.z
		 + TESR_SkyIrradianceGame[3].rgb * nV.x
		 + TESR_SkyIrradianceGame[4].rgb * (nV.x * nV.y)
		 + TESR_SkyIrradianceGame[5].rgb * (nV.y * nV.z)
		 + TESR_SkyIrradianceGame[6].rgb * (3.0f * nV.z * nV.z - 1.0f)
		 + TESR_SkyIrradianceGame[7].rgb * (nV.x * nV.z)
		 + TESR_SkyIrradianceGame[8].rgb * (nV.x * nV.x - nV.y * nV.y);
	return max(irradiance, 0.0f);
}


#define stepnum 32
// exponential fog based on https://iquilezles.org/articles/fog/
//  (a/b) * exp(-ro.y*b) * (1.0-exp(-t*rd.y*b))/rd.y;
float getHeightFog(float dstV, float falV, float3 worldP, float heightOffset){
	float3 eyeVec = worldP - TESR_CameraPosition.xyz;
	float3 stepV = eyeVec / stepnum;
	float stepLen = length(stepV);

	// The original loop stepped pos += step and summed exp(-falloff * pos.z) * stepDist over the
	// stepnum samples z0 + k*dz, k = 1..stepnum. That is a geometric series with ratio
	// r = exp(-falloff * dz), so it equals exactly
	//   stepDist * exp(-falloff * (z0 + dz)) * (1 - r^N) / (1 - r).
	// For tiny exponents (1 - r) loses precision in fp32, so the ratio uses its Taylor series.
	float z0 = TESR_CameraPosition.z - (vfFOG_GROUND + heightOffset * 1000);
	float x = falV * stepV.z;
	const float nV = stepnum;
	float series;
	[branch] if (abs(x) < 0.01) {
		float a = nV * x;
		float numerator = 1 - a / 2 + a * a / 6 - a * a * a / 24 + a * a * a * a / 120;
		float denominator = 1 - x / 2 + x * x / 6 - x * x * x / 24 + x * x * x * x / 120;
		series = nV * numerator / denominator;
	}
	else {
		series = (1 - exp(-nV * x)) / (1 - exp(-x));
	}
	float fogAcc = stepLen * exp(-falV * (z0 + stepV.z)) * series;

	// distance here is fogDepth, the caller's vanilla-weather-warped pseudo-distance (raised to
	// FogPower, not the real one) -- everywhere else in this shader (getFogFlat, vanillaStrength)
	// extinction is built to grow with THAT warped value, not with true world distance. fog above
	// is the raw height-weighted path integral, which scales with the REAL distance traveled
	// (length(eyeVector)), so converting it to the same convention means shrinking it by
	// distance/length(eyeVector), not growing it by the inverse. The previous orientation divided
	// by fogDepth instead of multiplying by it: when FogPower > 1, fogDepth shrinks much faster
	// than real distance as the camera gets close (fogDepth/trueDistance = normalizedDepth^(FogPower-1)
	// -> 0), so dividing by it blew up toward infinity right next to the camera -- a solid white
	// fog wall -- instead of tapering off the way the flat/vanilla path already does.
	//
	// Flipping it only relocates the same instability to FogPower < 1 weathers, where this ratio
	// is now the one that grows unbounded as eyeVector -> 0 (and is a literal 0/0 exactly at the
	// camera). saturate() caps it at 1 -- the ratio's own value at the far clip plane, where it's
	// always exactly 1 regardless of FogPower -- so the height term can never be rescaled to more
	// than its raw, already-well-behaved (tapers to 0 at the camera on its own) magnitude, on any
	// weather's FogPower.
	float distanceModifier = saturate(dstV / max(length(eyeVec), 0.0001));
	return fogAcc * distanceModifier;
}

// Raw extinction/inscattering terms, not yet composited onto the scene -- MinDensityFloor (see the
// call site) needs to combine these with the flat/distance-only terms below via max() before any
// compositing happens, since maxing two already-scene-composited colors together isn't the same as
// fogging by the max of the two underlying densities.
void getHeightFogTerms(float dstV, float3 extinctionColor, float3 inscatteringColor, float densV, float falV, float3 worldP, float offset, out float3 extColor, out float3 insColor){
	float fogAcc = densV * 0.00001 * getHeightFog(dstV, falV * 0.0001, worldP, offset);
	// fog is an optical-depth-like accumulation (density integrated along the view ray), the same
	// role distance*density plays in getFogFlat below -- so it goes through the same Beer-Lambert
	// 1-exp(-x) curve rather than being used directly as a linear coverage fraction. The old linear
	// version was implicitly clamped by the caller's saturate(1-extColor): fine while fog*extinction
	// stayed well under 1, but the linear ramp hits that ceiling at exactly 1 and goes fully opaque
	// with a hard clip, whereas 1-exp(-x) only ever approaches 1 asymptotically. Practically: past a
	// certain density, raising Extinction (or NightExtinctionScale) stopped doing anything visible
	// under the old formula, because there was no headroom left for it to add. Also lets extColor/
	// insColor from the two paths (this and getFlatFogTerms) combine consistently, since they're now
	// both true coverage fractions from 0 up to just under 1, rather than one being an unbounded
	// linear multiple.
	extColor = 1 - exp(-fogAcc * extinctionColor);
	insColor = 1 - exp(-fogAcc * inscatteringColor);
}

// Flat, distance-only density -- no height term at all. This is the HeightInfluence=0 endpoint;
// it is NOT the same as feeding falloff=0 into getHeightFog/getHeightFogTerms above, since that
// function's outer distance/length(eyeVector) normalization is built assuming the integral is
// doing real work and doesn't collapse to a clean distance-only result at falloff=0.
float3 getFogFlat(float dstV, float3 densV){
	return 1 - exp(-dstV * densV * 0.0001);
}

void getFlatFogTerms(float dstV, float3 extinctionColor, float3 inscatteringColor, float densV, out float3 extColor, out float3 insColor){
	extColor = getFogFlat(dstV, densV * extinctionColor);
	insColor = getFogFlat(dstV, densV * inscatteringColor);
}

// UNOFFICIAL 2026-11-01 HeightLayer ([Shaders.VolumetricFog.Main] HeightLayer / LayerBase / LayerThickness / LayerFollowsCamera, outdoors;
// tester: the fog's height is hard to set since the rewrite). The height curve above (getHeightFog) falls off over about 333000 units /
// FogPower at the default HeightFalloff 50, so over the map the fog has no visible top and MaxHeight (1000-unit steps from z -10000)
// only scales its density. The layer instead: density 1 at and below LayerBase (world z, or the camera's height + LayerBase), falling
// off exponentially above it over LayerThickness units. FogLayerMean is its exact mean along the straight view from the camera (height
// zc) to the pixel (zp): the flat fog's distance x density times this mean is the layer's optical depth, so below the base it is the
// flat fog, a valley under the base fills, and a hilltop or the sky seen above it gets little.
// F(z), the integral of the density over height: (z - base) - T below the base, -T exp(-(z - base) / T) above (continuous at the base).
// Packed in TESR_VolumetricFogShape (a float4 of its own overflowed the composite pass's 224 registers; VolumetricFog.cpp ApplyLayer):
// x = -1 / LayerThickness (negative = on; HeightFalloff is never negative), y = the base in world z.
float FogLayerF(float z) { float h = z - TESR_VolumetricFogShape.y; return h <= 0.0 ? h - 1.0 / (-TESR_VolumetricFogShape.x) : -exp(-h * (-TESR_VolumetricFogShape.x)) / (-TESR_VolumetricFogShape.x); }
float FogLayerMean(float zc, float zp) {
	float dz = zp - zc;
	[flatten] if (abs(dz) < 1.0) return exp(-max(0.5 * (zc + zp) - TESR_VolumetricFogShape.y, 0.0) * (-TESR_VolumetricFogShape.x));
	return saturate((FogLayerF(zp) - FogLayerF(zc)) / dz);
}
// The layer's density at one height (the noise samples' weights: the noise counts where the fog is)
float FogLayerAt(float z) { return exp(-max(z - TESR_VolumetricFogShape.y, 0.0) * (-TESR_VolumetricFogShape.x)); }


// UNOFFICIAL 2026-11-03 LampHaze ([Shaders.VolumetricFog.Interiors] LampHaze, LampHazeAnisotropy, LampHazeCore, LampHazeMaxLamps; VolumetricFog.cpp
// SetLampHaze): indoors the fog's air was lit by the ambient only, so a room's lamps lit the walls but not the haze in front of them. Now the
// fog takes in each listed lamp's light along the view: a soft glow in the air round a lamp, the light falling through the haze toward it.
// The lamps are the frame's shadow-slot and tracked lamps (ShaderManager::GetNearbyLights, as AmbientLight's grid and the path tracer take
// them), nearest first, relative to the camera, their reach the game's own (Spec.r), colour linear x dimmer, in a 16 x 2 float texture on
// stage 14 (NVR_FogLamps, bound by hand: TESR_ samplers bind by their order in the file): constant arrays put the estimate and the composite
// passes over ps_3_0's 224 constant registers (X4507; the effect compiler's preshader already takes them to about c190-c222), the texture
// costs one register (TESR_VolumetricFogLamps) and two point reads a lamp. Each lamp's light in the air:
//     f(d) = (e^2 / (d^2 + e^2) - e^2 / (r^2 + e^2)) / (1 - e^2 / (r^2 + e^2)),   e = LampHazeCore x r
// inverse-square with a bright core of e (as [Shaders.PBR.LampFalloff] Falloff, 0.25 there too), 1 at the lamp, 0 at its reach r. Along the
// view, clipped to the lamp's sphere and to the visible distance, its integral is closed form (s along the view from the point nearest the
// lamp, h that point's distance to it, h' = sqrt(h^2 + e^2), phi = atan(s / h')):  e^2 / h' (phi_b - phi_a) - e^2 / (r^2 + e^2) (s_b - s_a).
// In phi the remaining factors are smooth, so the phase (Henyey-Greenstein, LampHazeAnisotropy, x 4 pi: 1 = even, capped at 8 as the
// volumetric light's) and the fog's transmittance to the camera are taken at two Gauss-Legendre points of [phi_a, phi_b]: no march, no
// banding however near the view passes to a lamp. The result is in core lengths (about 2.3 through a lamp's centre at the default core,
// whatever its size) times the fog's in-scattering density there (BaseDensity with its noise x Inscattering) x LampHaze, added to the
// fog's add term before Amount.
// No shadows: a lamp's sphere that reaches through a wall glows on this side too (see the README). Compiled in only where lamps is true
// (ModernFogTerms); at run time the whole term sits in one dynamic branch on TESR_VolumetricFogLamps.x, 0 outdoors, with LampHaze 0 or
// no lamp: the fog is then exactly as before.
#define LAMP_HAZE_MAX 16
float4 TESR_VolumetricFogLamps;   // x: LampHaze (0 = off), y: lamps listed, z: anisotropy g, w: LampHazeCore
// Texel (i, row 0): lamp i relative to the camera (xyz), its reach (w, the game's radius); (i, row 1): its colour (linear) x dimmer.
sampler2D NVR_FogLamps : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

float2 LampHazePhase(float2 cosTheta, float g) {
	float2 k = max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-4);
	return min((1.0 - g * g) * rsqrt(k * k * k), 8.0);
}

// UNOFFICIAL 2026-11-12 LampHazeShadows ([Shaders.VolumetricFog.Interiors] LampHazeShadows, off by default; VolumetricFog.cpp SetLampHaze,
// core/RtFogVisibility.h RtFogLamps, Compute/Rt/RtFogLamps.comp): the lamps' light in the air shadowed by the room. The RT core traces,
// per lamp and screen cell, the share of the lamp's light along the view that really reaches the air (weighed as the integral below
// weighs it), into 4 x 4 tiles of one texture, one tile a lamp. The lamp's code is its colour texel's alpha: 0 = no trace (the light
// as before: the branch below is not taken), > 0 = tile + 1 + 32 x the cell size in pixels, < 0 = traced but this lamp has no tile yet
// (no light in the air this frame). The tiles are bound on stage 11 and read through NVR_FogGrid's sampler (linear, clamp): indoors
// the volumetric light's grid is never bound, and a sampler of its own was one more than ps_3_0 has (s15 is the sky shares').
// s01: the pixel's screen position (0..1; rectToScreen of its texture coordinate), kept half a cell inside the tile so the linear read
// never takes the next lamp's tile in.
float LampHazeVisibility(float code, float2 s01) {
	const float cell = floor(code * (1.0 / 32.0));
	const float tile = code - 32.0 * cell - 1.0;
	const float2 margin = 0.5 * cell * TESR_ReciprocalResolution.xy;
	const float row = floor(tile * 0.25);
	const float2 tileUV = (float2(tile - 4.0 * row, row) + clamp(s01, margin, 1.0 - margin)) * 0.25;
	return code > 0.5 ? saturate(tex2Dlod(NVR_FogGrid, float4(tileUV, 0.0, 0.0)).r) : 0.0;
}

// The lamps' light scattered toward the camera along the unit view dir, out to dist (game units), through fog with extinction sigmaT per unit.
float3 LampHazeLight(float3 dir, float dist, float sigmaT, float2 s01) {
	float3 sum = 0.0;
	const float g = TESR_VolumetricFogLamps.z;
	const float core = TESR_VolumetricFogLamps.w;
	[loop] for (int i = 0; i < LAMP_HAZE_MAX; i++) {
		[branch] if ((float)i >= TESR_VolumetricFogLamps.y) break;
		const float u = ((float)i + 0.5) * (1.0 / LAMP_HAZE_MAX);
		const float4 lamp = tex2Dlod(NVR_FogLamps, float4(u, 0.25, 0.0, 0.0));
		const float t0 = dot(lamp.xyz, dir);                   // along the view to the point nearest the lamp
		const float h2 = max(dot(lamp.xyz, lamp.xyz) - t0 * t0, 0.0);
		const float r2 = lamp.w * lamp.w;
		[branch] if (h2 < r2) {
			const float chord = sqrt(r2 - h2);
			const float sa = max(t0 - chord, 0.0) - t0;         // the view inside the sphere, in front of the camera and the surface
			const float sb = min(t0 + chord, dist) - t0;
			[branch] if (sb > sa) {
				const float e2 = core * core * r2;
				const float hp = sqrt(h2 + e2);
				const float pa = atan(sa / hp), pb = atan(sb / hp);
				const float w = e2 / (r2 + e2);
				const float along = max(e2 / hp * (pb - pa) - w * (sb - sa), 0.0) / ((1.0 - w) * sqrt(e2));
				// phase and transmittance at the two Gauss-Legendre points of [pa, pb]
				const float pm = 0.5 * (pa + pb), pd = 0.28867513 * (pb - pa);
				float2 sn, cs;
				sincos(float2(pm - pd, pm + pd), sn, cs);
				const float2 s = hp * sn / cs;
				const float2 cosTheta = -s * rsqrt(h2 + s * s + 1.0e-4);	// light's direction at the point vs. toward the camera
				const float2 phaseT = LampHazePhase(cosTheta, g) * exp(-sigmaT * max(t0 + s, 0.0));
				const float4 lampColour = tex2Dlod(NVR_FogLamps, float4(u, 0.75, 0.0, 0.0));
				float3 lampLight = lampColour.rgb * (along * 0.5 * (phaseT.x + phaseT.y));
				// UNOFFICIAL 2026-11-12 LampHazeShadows: the lamp's traced visibility along this view (alpha 0, the key off or nothing traced: as before)
				[branch] if (lampColour.a != 0.0) lampLight *= LampHazeVisibility(lampColour.a, s01);
				sum += lampLight;
			}
		}
	}
	return sum;
}

// UNOFFICIAL 2026-10-18 InteriorShadowFold: in the interior composite the scene here is the one before the darkening; the sky test
// below reads its luma (where a pixel lies at the far plane), so there it takes the darkened colour the separate pass would have
// stored at that pixel instead (InteriorStoredColor, below). interior is a compile-time constant (false: as before).
float4 InteriorStoredColor(float2 uvV);
float getSky(float2 uvV, bool interior = false) {
	// Explicit-LOD reads throughout (FogTerms can run inside a dynamic branch, see FogApply); these buffers have one
	// mip level, so they read exactly what tex2D did.
	float4 scene = tex2Dlod(TESR_SourceBuffer, float4(uvV, 0, 0));
	if (interior) scene = InteriorStoredColor(uvV);
	float4 colV = linearize(scene);

	float dstV = 0.5 * TESR_ReciprocalResolution.xy;
	dstV *= 1;

	float3 coefV = float3(float2(1, -1) * dstV, 0.9961);
	float dpt = readDepthLod(uvV) / farZ >= coefV.z;
	float depth1 = readDepthLod(uvV + coefV.xx) / farZ >= coefV.z;
	float depth2 = readDepthLod(uvV + coefV.xy) / farZ >= coefV.z;
	float depth3 = readDepthLod(uvV + coefV.yx) / farZ >= coefV.z;
	float depth4 = readDepthLod(uvV + coefV.yy) / farZ >= coefV.z;

	float totV = (dpt || depth1 || depth2 || depth3 || depth4) * luma(colV); // we scale the result with the point luma since sky tends to be brighter

	return saturate(totV);
}

// scale the fog color between sky, purefog, and sun contribution
float4 fogTint(float4 skyC, float4 pureFog, float fogStrength, float skyColorCoeff, float4 sunScat, float saturation){
	float4 fogTint = lerp(skyC, pureFog, saturate(pows(fogStrength, skyColorCoeff)));    // modulate between sky and pure fog color if strength is high
	fogTint += sunScat;                              									   // add sun influence
	fogTint = lerp(luma(fogTint), fogTint, saturation/(1 + fogStrength));                       // boost fog color saturation
	return fogTint;
}


// Adapted from SunShadows.fx.hlsl's GetLightAmount, trimmed to a single atlas tap per cascade
// (the atlas is already Gaussian-prefiltered, so single-tap VSM/EVSM is the established default
// there too) and sampled once per pixel rather than per raymarch step -- GetLightAmount's own
// unconditional 4-cascade evaluation makes it too costly to call many times per pixel. Separate
// effects compile independently, so this can't be shared with SunShadows.fx.hlsl directly.
float4 ScreenCoordToTexCoord(float4 coord){
	coord.xyz /= coord.w;
	coord.x = coord.x * 0.5f + 0.5f;
	coord.y = coord.y * -0.5f + 0.5f;
	return coord;
}

float GetFogShadowValue(float4x4 lightTransform, float4 coord, float offsetX, float offsetY, float biasV) {
	float4 lsc = ScreenCoordToTexCoord(mul(coord, lightTransform));
	lsc.xy *= 0.5;
	lsc.x += offsetX;
	lsc.y += offsetY;

	[branch] if (TESR_ShadowFormatData.x == 3.0f) return ShadowPCF2x2(TESR_ShadowAtlas, lsc.xy, lsc.z - 0.0002f, TESR_ShadowBlur.x);	// UNOFFICIAL PCF
	float4 mom = tex2Dlod(TESR_ShadowAtlas, float4(lsc.xy, 0.0f, 0.0f));

	float modeV = TESR_ShadowFormatData.x;
	float fmtBits = TESR_ShadowFormatData.y;

	[branch]
	if (modeV == 0.0f)
		return GetLightAmountValueVSM(mom.xy, lsc.z, biasV, 0.2f);
	else if (modeV == 1.0f)
		return GetLightAmountValueEVSM2(mom.xy, lsc.z, biasV, 0.2f, fmtBits);
	else
		return GetLightAmountValueEVSM4(mom, lsc.z, biasV, 0.2f, fmtBits);
}

float GetFogShadowVisibility(float4 positionWS, float3 normal) {
	float nDL = dot(normal, TESR_SmoothedSunDir.xyz);
	float offK = saturate(1 - nDL);

	float4 radiiV = { TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w };
	float4 texWV = 4.0f * radiiV * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float4 offDst = offK * 2.5f * texWV; // 2.5 = normal-offset bias in texels, matches SunShadows.fx.hlsl

	float biasV = (TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f) * (1.0f + offK);
	const float blendK = 0.9f;

	float4 shV = {
		GetFogShadowValue(TESR_ShadowCameraToLightTransformNear,   float4(positionWS.xyz + offDst.x * normal, 1.0f), 0.0, 0.0, biasV),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformMiddle, float4(positionWS.xyz + offDst.y * normal, 1.0f), 0.5, 0.0, biasV),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformFar,    float4(positionWS.xyz + offDst.z * normal, 1.0f), 0.0, 0.5, biasV),
		GetFogShadowValue(TESR_ShadowCameraToLightTransformLod,    float4(positionWS.xyz + offDst.w * normal, 1.0f), 0.5, 0.5, biasV),
	};

	float4 dstsV = {
		length(positionWS.xyz - TESR_ShadowNearCenter.xyz),
		length(positionWS.xyz - TESR_ShadowMiddleCenter.xyz),
		length(positionWS.xyz - TESR_ShadowFarCenter.xyz),
		length(positionWS.xyz - TESR_ShadowLodCenter.xyz),
	};

	if (dstsV.x < TESR_ShadowNearCenter.w) {
		if (dstsV.x < TESR_ShadowNearCenter.w * blendK) return shV.x;
		return lerp(shV.x, shV.y, smoothstep(TESR_ShadowNearCenter.w * blendK, TESR_ShadowNearCenter.w, dstsV.x));
	}
	else if (dstsV.y < TESR_ShadowMiddleCenter.w) {
		if (dstsV.y < TESR_ShadowMiddleCenter.w * blendK) return shV.y;
		return lerp(shV.y, shV.z, smoothstep(TESR_ShadowMiddleCenter.w * blendK, TESR_ShadowMiddleCenter.w, dstsV.y));
	}
	else if (dstsV.z < TESR_ShadowFarCenter.w) {
		if (dstsV.z < TESR_ShadowFarCenter.w * blendK) return shV.z;
		return lerp(shV.z, shV.w, smoothstep(TESR_ShadowFarCenter.w * blendK, TESR_ShadowFarCenter.w, dstsV.z));
	}
	else if (dstsV.w < TESR_ShadowLodCenter.w) {
		if (dstsV.w < TESR_ShadowLodCenter.w * blendK) return shV.w;
		return lerp(shV.w, 1.0f, smoothstep(TESR_ShadowLodCenter.w * blendK, TESR_ShadowLodCenter.w, dstsV.w));
	}
	return 1.0f;
}


// ---- Legacy fog (custom build): the Simple + Height fog stack from before the fog rewrite (upstream 25189928) ----
// Old presets (Uranium Haze and others) were tuned against it and set its keys (HeightFog*, SimpleFog*, MinimumBaseFog,
// SunPower, FogNight...). Same maths as that shader, written as the colour * fogMul + fogAdd terms the half-resolution
// passes need. Chosen by [Shaders.VolumetricFog.Main] LegacyFog: VolumetricFog.cpp then uses the *Legacy techniques,
// compiled with legacy = true (both fogs in one shader go over ps_3_0's 224 constant registers).
float4 TESR_VolumetricFogLow;    // x: FogSaturation, y: WeatherImpact, z: DistantFogRange, w: SunPower
float4 TESR_VolumetricFogHigh;   // x: HeightFogDensity, y: HeightFogFalloff, z: HeightFogDist, w: HeightFogSkyColor
float4 TESR_VolumetricFogSimple; // x: Extinction, y: Inscattering, z: FogNight, w: SimpleFogSkyColor
float4 TESR_VolumetricFogBlend;  // x: DistantFogBlend, y: HeightFogBlend, z: HeightFogRolloff, w: SimpleFogBlend
float4 TESR_VolumetricFogHeight; // x: DistantFogHeight, y: HeightFogHeight, z: SimpleFogHeight, w: isExterior
float4 TESR_VolumetricFogData;   // x: MinimumBaseFog, y: HeightFogSaturation, z: Amount, w: HeightFogInscattering

static const float LegacyBaseFogStrength = max(0, TESR_VolumetricFogData.x);
static const float LegacyHeightFogSaturation = max(0, TESR_VolumetricFogData.y);
static const float LegacyFogAmount = max(0, TESR_VolumetricFogData.z);
static const float LegacyHeightFogInscattering = max(0.0, TESR_VolumetricFogData.w) * 0.1;

static const float LegacyFogSaturation = max(0, TESR_VolumetricFogLow.x);
static const float LegacyWeatherImpact = max(0, TESR_VolumetricFogLow.y);
static const float LegacyDistantFogRange = exp(-4 * TESR_VolumetricFogLow.z);
static const float LegacySunPower = max(0, TESR_VolumetricFogLow.w);

static const float LegacyHeightFogDensity = max(0, TESR_VolumetricFogHigh.x);
static const float LegacyHeightFogFalloff = max(0, TESR_VolumetricFogHigh.y);
static const float LegacyHeightFogDist = TESR_VolumetricFogHigh.z;
static const float LegacyHeightFogSkyColor = 2 * TESR_VolumetricFogHigh.w;

static const float LegacyDistantFogHeight = TESR_VolumetricFogHeight.x;
static const float LegacyHeightFogHeight = TESR_VolumetricFogHeight.y;
static const float LegacySimpleFogHeight = TESR_VolumetricFogHeight.z * 1.5;
static const float LegacyIsExterior = TESR_VolumetricFogHeight.w;

static const float LegacyExtinction = max(0.0, TESR_VolumetricFogSimple.x) * 0.1;
static const float LegacyInscattering = max(0.0, TESR_VolumetricFogSimple.y) * 0.1;
static const float LegacyFogNight = max(0.0, TESR_VolumetricFogSimple.z);
static const float LegacySimpleFogSkyColor = max(0, TESR_VolumetricFogSimple.w);

static const float LegacyDistantFogBlend = TESR_VolumetricFogBlend.x;
static const float LegacyHeightFogBlend = TESR_VolumetricFogBlend.y;
static const float LegacyHeightFogRolloff = TESR_VolumetricFogBlend.z;
static const float LegacySimpleFogBlend = TESR_VolumetricFogBlend.w;

// The old getHeightFog: the 32-step sum in closed form (as getHeightFog above), then the old distance scaling
// length(eyeVector) / distance, which getHeightFog above changed.
float getLegacyHeightFog(float dstV, float falV, float3 worldP, float heightOffset){
	float3 eyeVec = worldP - TESR_CameraPosition.xyz;
	float3 stepV = eyeVec / stepnum;
	float stepLen = length(stepV);

	float z0 = TESR_CameraPosition.z - (vfFOG_GROUND + heightOffset * 1000);
	float x = falV * stepV.z;
	const float nV = stepnum;
	float series;
	[branch] if (abs(x) < 0.01) {
		float a = nV * x;
		float numerator = 1 - a / 2 + a * a / 6 - a * a * a / 24 + a * a * a * a / 120;
		float denominator = 1 - x / 2 + x * x / 6 - x * x * x / 24 + x * x * x * x / 120;
		series = nV * numerator / denominator;
	}
	else {
		series = (1 - exp(-nV * x)) / (1 - exp(-x));
	}
	float fogAcc = stepLen * exp(-falV * (z0 + stepV.z)) * series;
	return fogAcc * (length(eyeVec) / max(dstV, 0.0001));
}

void LegacyFogTerms(float2 uvV, out float3 fogMul, out float3 fogAdd, out float nightAmountOut, bool interior = false)
{
	float4 pureFog = linearize(TESR_FogColor);

	float dpt = readDepthLod(uvV);
	float isDay = smoothstep(0.4, 0.8, TESR_SunAmount.x);
	float isDayF = smoothstep(0.1, 0.6, TESR_SunAmount.x);
	float SunsetFog = sin(TESR_SunAmount.x * PI) * 0.001 * LegacyIsExterior;

	float3 eyeVec = toWorld(uvV);
	float3 eyeDirV = normalize(eyeVec);
	eyeVec *= dpt;
	float3 worldP = eyeVec + TESR_CameraPosition.xyz;
	float pointHeight = worldP.z;

	float nrmDepth = length(eyeVec) / farZ;
	float fogPw = lerp(1, vfFogPower, saturate(LegacyWeatherImpact)) / lerp(LegacyHeightFogRolloff, 1 / LegacyFogNight, (1 - isDayF) * LegacyIsExterior);
	float fogD = pows(nrmDepth, fogPw);
	fogD = fogD * farZ / (LegacyHeightFogDist * lerp(0.005, 1, LegacyIsExterior));

	float strV = pows((saturate(1 - vfFarFog / farZ) + saturate(1 - vfNearFog / farZ)) / 2, 2) / (vfFogPower + 1);
	strV = LegacyBaseFogStrength + LegacyWeatherImpact * strV + SunsetFog;
	float heightFade = exp(-pointHeight / (80000 * LegacySimpleFogHeight));

	// default values used by interiors
	float distFog = 0.0, distHFade = 0.0;
	float4 skyC = pureFog;
	float inScattering = LegacyInscattering;
	float extinction = LegacyExtinction;
	float4 sunV = black;

	if (LegacyIsExterior){
		float3 upV = blue.xyz;
		float sunH = shade(TESR_SunPosition.xyz, upV);
		float sunD = dot(eyeDirV, TESR_SunPosition.xyz);
		float sunInf = pows(compress(sunD), vfSUNINFLUENCE);
		float4 sunColor = float4(GetSunColor(sunH, 1, TESR_SunAmount.x, TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb), 1);
		sunColor *= isDay * LegacyIsExterior;

		skyC.rgb = GetSkyColor(0.5, 1, sunH, sunInf, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, black.rgb) * TESR_SunsetColor.w;
#if PHYSICAL_SKY_ON
		[branch] if (TESR_PhysSky.x > 0.0f) skyC.rgb = PhysicalFogSky(eyeDirV, skyC.rgb);	// UNOFFICIAL physical sky
#endif
		NVR_FOG_SKY_LOOK(skyC.rgb);	// UNOFFICIAL 2026-10-26 SM6 build only (see TESR_FogSkyLook)

		float sunStr = TESR_FogData.z / (1 + 2 * strV);
		sunStr = lerp(sunStr, max(sunStr * 2, 10), nrmDepth);

		float sunScat = pows(compress(sunD), 2 + sunStr);
		sunScat *= pow(1 - sunH, 2) * isDayF * TESR_FogData.z;
		sunV = sunColor * sunScat * LegacySunPower * 100;

		distFog = pows(smoothstep(LegacyDistantFogRange, 1.0, nrmDepth), 0.5);
		// The old shader's sky mask (only read here when DistantFogHeight is 0); it then set isSky to 0 for the rest.
		[branch] if (LegacyDistantFogHeight == 0) distHFade = 1.0 - (getSky(uvV, interior) > 0);
		else distHFade = saturate(exp(-pointHeight / (80000 * LegacyDistantFogHeight)));	// UNOFFICIAL 2026-10-30: at most 1, as below

		inScattering += SunsetFog;
		extinction *= 1 - distFog * isDayF;
	}

	// simple fog: lerp(color, color * (1 - ext) + fogColor * ins, SimpleFogBlend)
	float4 simpleFogColor = fogTint(skyC, pureFog, strV, LegacySimpleFogSkyColor, black, LegacyFogSaturation);
	float simpleDensity = strV * heightFade;
	float simpleExt = getFogFlat(fogD, simpleDensity * extinction).x;
	float simpleIns = getFogFlat(fogD, simpleDensity * inScattering).x;
	float simpleBlend = saturate(LegacySimpleFogBlend);
	fogMul = lerp(1.0, saturate(1 - simpleExt), simpleBlend);
	fogAdd = simpleFogColor.rgb * saturate(simpleIns) * simpleBlend;

	// distant fog: lerp toward the sky colour
	float distantBlend = distFog * saturate(LegacyDistantFogBlend) * distHFade * LegacyIsExterior;
	fogMul *= 1 - distantBlend;
	fogAdd = lerp(fogAdd, skyC.rgb, distantBlend);

	// height fog: lerp(color, color * (1 - ext) + fogColor * ins, HeightFogBlend), unclamped linear terms as before
	float4 heightFogColor = fogTint(skyC, pureFog, strV, LegacyHeightFogSkyColor, sunV, LegacyHeightFogSaturation);
	float heightFog = strV * LegacyHeightFogDensity * 0.00001 *
		getLegacyHeightFog(fogD, 1.5 / (fogPw * LegacyHeightFogFalloff) * 0.0001, worldP, LegacyHeightFogHeight);
	float heightBlend = saturate(LegacyHeightFogBlend);
	float heightScale = lerp(1.0, saturate(1 - heightFog * extinction), heightBlend);
	fogMul *= heightScale;
	fogAdd = fogAdd * heightScale + heightFogColor.rgb * saturate(heightFog * LegacyHeightFogInscattering) * heightBlend;

	fogMul = lerp(1.0, fogMul, LegacyFogAmount);
	fogAdd *= LegacyFogAmount;
	nightAmountOut = LegacyFogAmount;
}

// Every step of the composite below is affine in the scene colour, per channel, so the whole
// fog reduces to linear(result) = max(linear(scene) * fogMul + fogAdd, 0). The coefficients
// depend only on depth, the sky mask and constants -- never on scene detail beyond getSky's
// luma gate -- which lets the dedicated path evaluate them at half resolution and apply them
// to the full-resolution scene instead of reconstructing the scene itself from half resolution.
//
// UNOFFICIAL 2026-10-18 ClassicLook ([Shaders.VolumetricFog.Main] ClassicLook; classic is a compile-time constant, one per
// technique: the *Classic techniques): the look of the original NVR fog, upstream master ab16e86c, exactly: its density noise
// (procedural fbm3 at the pixel's surface point, not the 3D noise texture averaged along the view or the froxel grid's fog
// volume), its sun shadow on the in-scattering (the cascades at the surface point, not the froxel grid's lit share), its sky
// colour and sky ambient (the game's own sky, not the physical sky) and its glow round the sun (SunGlow 1). Everything else is
// master's maths as it was; the speed work stays (half-resolution coefficients with the full-resolution apply, the shadow and
// AO applies folded in, the closed-form height fog).
void ModernFogTerms(float2 uvV, out float3 fogMul, out float3 fogAdd, out float nightAmountOut, bool interior = false, bool classic = false, bool gameFloor = true, bool lamps = true)
{
	VSOUT IN = (VSOUT)0;
	IN.UVCoord = uvV;
	float4 pureFog = linearize(TESR_FogColor);

	float dpt = readDepthLod(IN.UVCoord);   // explicit LOD: see getSky
	float isDay = smoothstep(0.4, 0.8, TESR_SunAmount.x);
	float isDayF = smoothstep(0.1, 0.6, TESR_SunAmount.x);
	// 0 for interiors (no consistent day/night concept) or full daylight; ramps toward 1 through
	// dusk for exteriors. Shared by every Night* setting so they all fade in together, in step,
	// rather than each re-deriving its own slightly-different version of the same curve.
	float nightFactor = vfIsExterior * (1 - isDay);

	float3 eyeVec = toWorld(IN.UVCoord);
	float3 eyeDirV = normalize(eyeVec);
	eyeVec *= dpt;
	float3 worldP = eyeVec + TESR_CameraPosition.xyz;

	float nrmDepth = length(eyeVec) / farZ; // make sure depth is the same at the center and edges of the screen
	float fogPw = lerp(1, vfFogPower, saturate(vfWeatherImpact));
	float fogD = pows(nrmDepth, fogPw) * farZ;

	float isSk = getSky(IN.UVCoord, interior);
	// getSky's 5-tap test also lights up on pixels bordering the sky depth threshold, so this
	// doubles as an edge-dilated mask usable for anti-aliasing that boundary further down.
	float skyMaskF = lerp(1.0, 1.0 - isSk, vfWeatherFilterBlend); // weather can disable the sky exclusion so fog blends fully into an overcast sky
	// Same idea, but time-of-day driven instead of weather driven: lets the sky dome itself pick up
	// fog color/haze at night (a starless, hazy horizon look) independent of WeatherFilterBlend.
	// nightFactor already carries isExterior, so this is a no-op in interiors regardless of the
	// setting.
	skyMaskF = lerp(skyMaskF, 1.0, nightFactor * vfNightDisableSkyMask);

	// Pure depth-threshold sky test, unweighted by luma -- getSky()'s own luma weighting makes
	// isSky fall toward 0 on a dark sky (night, heavy overcast) even though the pixel genuinely
	// is the skydome, which let the aerial tint paint over it. The aerial exclusion needs a hard
	// gate regardless of sky brightness. Reuses `depth`, already sampled above via readDepth().
	float isDome = dpt / farZ >= 0.9961;

	// ---- vanilla-anchored density, derived from the active weather's own authored fog shape ----
	float vanStr = pows((saturate(1 - vfFarFog / farZ) + saturate(1 - vfNearFog / farZ)) / 2, 2) / (vfFogPower + 1);

	// ---- NVR-driven density: time-of-day curve, sunrise/sunset boost, animated 3D noise ----
	float sunsetB = sin(saturate(TESR_SunAmount.x) * PI) * vfIsExterior;
	float noonT = 1 - saturate(abs(TESR_GameTime.y - 12) / 6); // 0 at 6am/6pm, 1 at solar noon
	float todScale = lerp(1.0, lerp(1.0, 1.0 - vfMorningFogDip, noonT), vfIsExterior);
	float nDensS = lerp(1.0, vfNightDensityScale, nightFactor);

	float nWind = vfWindSpeed * lerp(1.0, vfNightWindSpeedScale, nightFactor);
	float3 winOff = float3(vfWindDirection * nWind * TESR_GameTime.x * 0.002, 0);
	// UNOFFICIAL: the noise is read from the 3D noise texture at four points along the view ray and averaged, so its
	// patches sit in the air in front of things instead of being painted onto the surface behind them (it used to be
	// read once, at that surface). The nearest 300 units are left out, where the pattern would swim with the camera;
	// the farthest point is at most 12000 units out. A texture read costs less than one evaluation of the old
	// procedural fbm3 (24 hashes). Averaging flattens the contrast, which the last line restores.
	float nVal = 0.0;
	// UNOFFICIAL 2026-10-14 fog volume ([Shaders.VolumetricFog.Main] FogVolume, with the volumetric light's froxel grid): the
	// noise averaged over the whole view out to the grid's depth, each point weighed by the fog's density there (the grid's
	// b / g), instead of four points along it; and below, the sun's shadow averaged the same way (a / g).
	float4 fogVolume = 0.0;
	[branch] if (NVR_FogGridFlags.x > 0.5 && TESR_VolumetricLightGridData.x > 0.5) {
		fogVolume = FogGridSum(IN.UVCoord, dpt);
		nVal = fogVolume.g > 1.0e-6 ? fogVolume.b / fogVolume.g : 0.5;
	}
	else {
		float noiseReach = min(length(eyeVec), 12000.0);
		float noiseWeight = 0.0;
		[unroll] for (int noiseStep = 0; noiseStep < 4; noiseStep++) {
			float noiseT = lerp(min(300.0, noiseReach), noiseReach, (noiseStep + 0.5) / 4.0);
			float3 noisePos = (TESR_CameraPosition.xyz + eyeDirV * noiseT + winOff) / (1500 * vfNoiseScale);
			// UNOFFICIAL 2026-11-01 HeightLayer: each sample weighed by the layer's density at its height (1 with the layer off)
			float w = (gameFloor && TESR_VolumetricFogShape.x < 0.0) ? FogLayerAt(TESR_CameraPosition.z + eyeDirV.z * noiseT) : 1.0;
			nVal += w * tex3Dlod(NVR_FogNoise, float4(noisePos / 4.0, 0.0)).g;
			noiseWeight += w;
		}
		nVal = noiseWeight > 0.0 ? nVal / noiseWeight : 0.5;	// (all four far above a thin layer: no fog there to weigh)
	}
	nVal = saturate((nVal - 0.5) * 1.8 + 0.5);
	// ClassicLook: master's noise instead, read once at the surface the pixel shows (patches 'painted' on the scene, as in the
	// original NVR fog). (In the *Classic techniques everything above is then unused and compiled out, texture reads included.)
	if (classic) nVal = fbm3((worldP + winOff) / (1500 * vfNoiseScale));
	float nNoiseS = vfNoiseStrength * lerp(1.0, vfNightNoiseStrengthScale, nightFactor);

	// Zero the noise's contribution to density on true sky pixels via isSkyDome (a hard depth
	// test, not brightness-based) -- strength still feeds the exterior sun-scattering term even
	// on sky pixels (it's only excluded from the *fog color* blend later, via skyMaskFactor), so
	// without this the noise pattern visibly distorted the sun glow's shape as it crossed the sky.
	// A luma-based version of this was tried first and reverted: it also caught ordinary bright
	// non-sky surfaces (sunlit terrain, snow), crushing the noise's animated look almost everywhere
	// in daylight instead of just on the sky. isSkyDome has no such false positives.
	float noiseSkyM = 1 - isDome;
	float nvrDens = vfBaseDensity * todScale * nDensS * lerp(1.0, nVal, nNoiseS * noiseSkyM);
	// Not gated by WeatherFilterBlend (unlike skyMaskFactor above): that gate exists so the
	// composited fog can blend into an overcast sky instead of excluding the sky dome, which is a
	// property of the final composite, not of density. nvrDensity itself -- including the wind/
	// noise animation -- used to be multiplied by WeatherFilterBlend too, which meant Rainy/Cloudy
	// weather (RainyDisablesSkyFilter/CloudyDisablesSkyFilter, both default on) zeroed it entirely.
	// Since FNV has no distinct "Foggy" weather type, an actual fog weather is almost always
	// authored as Cloudy, so that silently killed the one place the animation lives on exactly the
	// weather it matters most for. The sky-dome distortion concern that originally motivated
	// tying density to weather is already independently covered above by noiseSkyMask/isSkyDome,
	// so nvrDensity no longer needs WeatherFilterBlend's gate at all -- SunriseSunsetBoost included,
	// for the same reason.
	nvrDens += vfSunriseSunsetBoost * sunsetB;

	float strV = max(0, nvrDens + vfWeatherImpact * vanStr);

	// ---- shadow-coupled sun in-scattering, sampled once per pixel at the fog's terminating point ----
	float shVis = 1.0;
	[branch]
	if (vfIsExterior > 0.5 && vfShadowStrength > 0.0 && TESR_ShadowFade.y > 0.5) {
		// (ClassicLook: always the last case, master's shadow at the surface point.)
		[branch] if (!classic && fogVolume.g > 1.0e-6) {
			// UNOFFICIAL fog volume: the sun's share of the fog along the view, weighed by the fog's density.
			shVis = lerp(1.0, saturate(fogVolume.a / fogVolume.g), vfShadowStrength);
		}
		else if (!classic && NVR_FogGridFlags.x > 0.5) {
			// UNOFFICIAL froxel grid: the sun's share of the air along the view (see FogGridAir).
			float2 air = FogGridAir(IN.UVCoord, dpt);
			shVis = lerp(1.0, air.y > 1.0e-6 ? saturate(air.x / air.y) : 1.0, vfShadowStrength);
		}
		else {
			float3 wNrm = GetWorldNormalLod(IN.UVCoord);
			shVis = lerp(1.0, GetFogShadowVisibility(float4(worldP, 1.0), wNrm), vfShadowStrength);
		}
	}

	// default values used by interiors
	float distFog = 0.0, distHFade = 0.0;
	float4 skyC = pureFog;
	float4 sunV = black;

	// sun/sky/distant fog coloring specific to exteriors
	if (vfIsExterior){
		float3 upV = blue.xyz;
		float sunH = shade(TESR_SunPosition.xyz, upV);
		float sunD = dot(eyeDirV, TESR_SunPosition.xyz);
		float sunInf = pows(compress(sunD), vfSUNINFLUENCE);
		float4 sunColV = float4(GetSunColor(sunH, 1, TESR_SunAmount.x, TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb), 1);
		sunColV *= isDay * vfIsExterior;

		skyC.rgb = GetSkyColor(0.5, 1, sunH, sunInf, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, black.rgb) * TESR_SunsetColor.w;
#if PHYSICAL_SKY_ON
		if (!classic) {	// (ClassicLook: the game's sky colour, as master)
			[branch] if (TESR_PhysSky.x > 0.0f) skyC.rgb = PhysicalFogSky(eyeDirV, skyC.rgb);	// UNOFFICIAL physical sky
		}
#endif
		NVR_FOG_SKY_LOOK(skyC.rgb);	// UNOFFICIAL 2026-10-26 SM6 build only (see TESR_FogSkyLook)

		// add sun influence/scattering, narrowed/widened by PhaseAsymmetry, gated by the shadow cascades
		float sunStr = TESR_FogData.z / (1 + 2 * strV);
		sunStr = lerp(sunStr, max(sunStr * 2, 10), nrmDepth);

		float sunScat = pows(compress(sunD), lerp(8.0, 1.0, vfPhaseAsymmetry) + sunStr);
		sunScat *= pow(1 - sunH, 2) * isDayF * TESR_FogData.z;
		// UNOFFICIAL 2026-10-15 SunGlow: this glow (inherited from NVR, up to 100 times the sun's colour where the fog lies
		// toward a low sun) blew the sun out with the fog on; SunGlow scales it (1 = as it was).
		// UNOFFICIAL 2026-11-19: gate sunV off the sky dome (the sky/clouds draw the sun there); where the constant budget allows
		// (lamps: Estimate, Combine, Full, Packed — everything except DedicatedFogComposite's 224-const edge fallback), also
		// attenuate in overcast weather (vfWeatherFilterBlend) and scale by SkyMultiplier (TESR_SunsetColor.w). (ClassicLook: unchanged.)
		sunV = sunColV * sunScat * shVis * TESR_PBRData.z * 100 * (classic ? 1.0 : TESR_VolumetricFogSun.x * (1.0 - isDome));
		if (lamps && !classic) sunV *= lerp(0.25, 1.0, vfWeatherFilterBlend) * TESR_SunsetColor.w;

		distFog = pows(smoothstep(vfDistantFogRange, 1.0, nrmDepth), 0.5);
		distHFade = (vfDistantFogHeight == 0) ? (1.0 - isSk) : saturate(exp(-worldP.z / (80000 * vfDistantFogHeight)));	// UNOFFICIAL 2026-10-30: at most 1 (below z 0 the exp passed 1 and the sky lerp ran past the sky colour: white, then inf in the TAA history)
	}

	// ---- sky ambient in-scattering (SH-reconstructed, or flat fallback), unshadowed ----
	// (ClassicLook: the game's own sky, as master; without the physical sky the two are the same.)
	float3 ambC = vfSkyAmbientAvailable > 0.5
		? (classic ? SkyIrradianceGameLinear(float3(0, 0, 1)) : SkyIrradianceLinear(float3(0, 0, 1)))
		: linearize(TESR_SkyColor).rgb;
	ambC *= TESR_PBRData.w;

	// Moon-phase night ambient floor: skyUpper/skyLowColor/horizonColor (what ambientColor above
	// is built from) go near-black at night for most vanilla weathers, since none of them account
	// for moon/starlight -- without this, fog collapses toward black after dark regardless of
	// density/color tuning. TESR_SunAmbient is the weather's own Ambient color (already time-blended
	// by the vanilla engine), scaled by MoonVisibility (the current lunar-cycle brightness) and
	// NightAmbientStrength, faded in via the shared nightFactor (isExterior-gated, ramping through
	// dusk) so the transition is smooth rather than a hard cutoff and interiors get exactly zero.
	// max(), not lerp/add: at night ambientColor is already near-zero in every channel, so this only
	// raises the floor; at nightFactor=0 nightAmbient is exactly zero, so daytime/interior
	// ambientColor is completely unaffected regardless of MoonVisibility/NightAmbientStrength.
	float3 nAmb = linearize(TESR_SunAmbient).rgb * vfMoonVisibility * vfNightAmbientStrength * nightFactor;
	ambC = max(ambC, nAmb);

	// UNOFFICIAL 2026-10-30 WeatherFogColor ([Shaders.VolumetricFog.Main], TESR_VolumetricFogSun.y, 0 indoors; tester: "a weather's fog
	// colour set to 100% red changes nothing"): the weather's side of this blend is the weather's own fog colour (the sky look on
	// top, at the sky's strength) instead of the sky's colour, by that share (1; 0 = as before). Before, the fog colour reached the
	// fog only through fogColor()'s pow(strength, 6) below: about 1 % at the usual density. Indoors skyColor is the fog colour.
	// Not on the sky dome (isSkyDome): where the fog lies over the sky (Rainy / CloudyDisablesSkyFilter, NightDisableSkyMask) it keeps
	// the sky's colour, as the game's own fog leaves the sky alone; else a cloudy weather's whole sky took its fog colour (slot run).
	// (ClassicLook: 0, master's sky colour, as master.)
	float3 weatherColor = lerp(skyC.rgb, pureFog.rgb * TESR_SunsetColor.w, classic ? 0.0 : TESR_VolumetricFogSun.y * (1.0 - isDome));
	float4 ambSkyC = float4(lerp(ambC, weatherColor, saturate(vfWeatherImpact)), 1);

	float4 fogFinal = fogTint(ambSkyC, pureFog, strV, vfFogSkyColorCoeff, sunV, vfFogSaturation);
	// Multiplicative recolor, same idea as aerialTint below -- fades in via nightFactor, so daytime
	// color is completely unaffected and NightTint's default (1,1,1) is a no-op at any time.
	fogFinal.rgb *= lerp(float3(1, 1, 1), vfNightTint, nightFactor);

	// Blend between flat (distance-only, no height dependence) and height-integrated density.
	// HeightInfluence=1 (exterior default) reduces to exactly the prior height-only behavior;
	// HeightInfluence=0 (interior default) gives uniform density regardless of MaxHeight, safe
	// for a generic preset with no per-cell tuning. Per-cell presets can raise HeightInfluence
	// alongside a hand-tuned MaxHeight for spaces where a real gradient is known to make sense.
	float nExt = vfExtinction * lerp(1.0, vfNightExtinctionScale, nightFactor);
	float nIns = vfInscattering * lerp(1.0, vfNightInscatteringScale, nightFactor);
	float nHFall = vfHeightFalloff * lerp(1.0, vfNightHeightFalloffScale, nightFactor);
	float nMaxH = vfMaxHeight + vfNightMaxHeightOffset * nightFactor;

	float3 flExt, flIns, hExt, hIns;
	getFlatFogTerms(fogD, nExt, nIns, strV, flExt, flIns);
	float falArg = 1.5 / (fogPw * nHFall);
	// UNOFFICIAL 2026-11-01 HeightLayer (above FogLayerMean): the flat fog times the layer's mean density along the view. (ClassicLook: never.
	// gameFloor false, the physical-sky composite pass's per-pixel fallback at the 224-register limit, as GameFogFloor below: the old curve.)
	// (No branch: both terms and a select; a dynamic branch here put the composite pass over ps_3_0's register limits.)
	getHeightFogTerms(fogD, nExt, nIns, strV, falArg, worldP, nMaxH, hExt, hIns);
	if (gameFloor && !classic) {
		float3 layerExt, layerIns;
		const bool layerOn = TESR_VolumetricFogShape.x < 0.0;
		getFlatFogTerms(fogD, nExt, nIns, layerOn ? strV * FogLayerMean(TESR_CameraPosition.z, worldP.z) : 0.0, layerExt, layerIns);
		hExt = layerOn ? layerExt : hExt;
		hIns = layerOn ? layerIns : hIns;
	}

	float3 blExt = lerp(flExt, hExt, vfHeightInfluence);
	float3 blIns = lerp(flIns, hIns, vfHeightInfluence);

	// Height-agnostic floor: a single exponential height falloff can't be tuned to read as rich,
	// pooling ground fog in low-lying terrain (Extinction/HeightFalloff/MaxHeight tuned for that)
	// AND still have any presence on terrain well above that reference -- the same curve that makes
	// low ground look convincing crushes density toward zero a few multiples of 1/HeightFalloff
	// above it. max(), not another lerp: it only ever raises density where the height-shaped term
	// has decayed below MinDensityFloor's fraction of the flat/distance-only term, so it guarantees
	// a baseline everywhere without diluting the height curve's shape near the ground, where the
	// height term is already higher than the floor and this is a no-op.
	float3 fiExt = max(blExt, flExt * vfMinDensityFloor);
	float3 fiIns = max(blIns, flIns * vfMinDensityFloor);

	// color * mul + add form of: fogged = color * (1 - ext) + fogColor * ins; lerp(color, fogged, skyMaskFactor)
	fogMul = lerp(1.0, saturate(1 - fiExt), skyMaskF);
	fogAdd = fogFinal.rgb * saturate(fiIns) * skyMaskF;
	// UNOFFICIAL 2026-11-03 LampHaze (LampHazeLight above; indoors only: TESR_VolumetricFogLamps.x is 0 outdoors, with LampHaze 0 or no lamp):
	// the lamps' light in the air, by the fog's in-scattering density here (Inscattering x the density with its noise; Inscattering is
	// stored x 0.1, so x 10 is the setting) and its extinction per unit (as getFogFlat's), on the same sky mask; Amount below scales it as
	// the rest. (ClassicLook: never, the original fog had no lamp light.)
	if (lamps && !classic) {
		[branch] if (TESR_VolumetricFogLamps.x > 0.0) {
			fogAdd += LampHazeLight(eyeDirV, length(eyeVec), strV * nExt * 0.0001, rectToScreen(uvV)) * (TESR_VolumetricFogLamps.x * strV * nIns * 10.0 * skyMaskF);
		}
	}

	// ---- aerial perspective: mid-to-far distance tint on non-sky terrain ----
	// Own day-fade curve, wider than isDayTimeFog's: aerial haze is a daylight-scattering
	// phenomenon, so it's still suppressed through more of the sunrise/sunset transition than
	// sun-scattering's own 0.1-0.6 ramp -- direct low-angle sun was making it read as too bright
	// on distant terrain at that specific time.
	float aDayF = smoothstep(0.1, vfAerialDayFadeStart, TESR_SunAmount.x);
	float aFac = smoothstep(vfAerialRangeStart, 1.0, nrmDepth) * (1.0 - isDome) * vfIsExterior;
	float3 aTint = lerp(ambC, linearize(TESR_VolumetricFogAerialTint).rgb, vfAerialTintBlend);

	// Multiplicative recolor, not a lerp toward an absolute color. aerialTint is a roughly-fixed
	// color (sky ambient or a manual override) that doesn't dim on its own at night, so blending
	// straight toward it pulled dark distant terrain up to that brightness -- a glow, at any
	// AerialStrength/AerialTintBlend that was visible during the day, however far aerialDayFade
	// had already suppressed it.
	// Clamp to <= 1 per channel and multiply directly, rather than normalizing to luma 1 and
	// multiplying (tried first, reverted): dividing by aerialTint's own luma is numerically
	// unstable exactly when ambient light is dim but nonzero -- dawn/dusk -- spiking the
	// multiplier and amplifying whatever variation was already in finalColor.rgb, including the
	// density noise, which is what caused both the noise artifacts to reappear and the lingering
	// sunrise glow. Clamped and multiplied directly, tinted <= finalColor.rgb per channel always,
	// so amplification is impossible regardless of lighting or tuning.
	float3 aTintC = saturate(aTint);
	float3 aerialScale = lerp(1.0, aTintC, aFac * vfAerialStrength * skyMaskF * aDayF);
	fogMul *= aerialScale;
	fogAdd *= aerialScale;

	// ---- distant fog: horizon Z-fighting/sky-seam matte ----
	// UNOFFICIAL 2026-11-19: keep distantBlend off sky-dome pixels (classic unchanged) so overcast horizon clouds are not painted over with unclouded skyC.
	float distantBlend = distFog * saturate(vfDistantFogBlend) * distHFade * vfIsExterior * (classic ? 1.0 : (1.0 - isDome));
	fogMul *= 1 - distantBlend;
	fogAdd = lerp(fogAdd, skyC.rgb, distantBlend);

	// inverted edge term: getSky's dilated mask specifically feathers the terrain/sky silhouette,
	// independent of the weather-driven sky mask above since this is an anti-aliasing fix, not a
	// density effect -- it should keep working in rain/overcast just as much as on clear days.
	float edgeBlend = isSk * vfEdgeAA * vfIsExterior;
	fogMul *= 1 - edgeBlend;
	fogAdd = lerp(fogAdd, skyC.rgb, edgeBlend);

	float nAmt = vfFogAmount * lerp(1.0, vfNightAmountScale, nightFactor);
	fogMul = lerp(1.0, fogMul, nAmt);
	fogAdd *= nAmt;
	nightAmountOut = nAmt;

	// UNOFFICIAL 2026-10-30 GameFogFloor ([Shaders.VolumetricFog.Main], TESR_VolumetricFogSun.z, 0 = off and indoors; user: the fog is
	// nearly invisible in clear weather at noon): at least the game's own fog for the weather covers the picture, the game's curve
	// pow(saturate((d - near) / (far - near)), power) with the weather's raw near (w) times z; where NVR's fog already covers as much,
	// nothing changes. The top-up blends toward NVR's own fog colour without the sun's glow (blending a share of the picture toward the
	// glow, up to ~25x the sun's colour, would blow it out). Not on the sky dome: the game's fog leaves the sky alone. A weather without
	// fog distances (far <= near) has none, as in the game. (ClassicLook: off, as master.) gameFloor false: the per-pixel fallback of
	// the composite pass with the physical sky (DedicatedFogComposite), which sits at ps_3_0's 224 constant registers: any reference
	// to the floor's constant pushed it over, so those rare pixels (thin edges, foliage gaps) keep NVR's own fog there (the Vulkan
	// passes, on by default, have no such limit and floor every pixel).
	// UNOFFICIAL 2026-11-15 FogWall ([Shaders.VolumetricFog.Interiors] GameFogFloor, default 1; docs/next/FOG-WALL.md): indoors the same
	// constants carry the fade into the cell's fog before its clip distance (the camera's far plane, farZ). Past the clip the game draws
	// nothing (the cleared background, at the far depth: isDome), and the game's own fog, off while this effect draws, was full by the
	// cell's fog far and hid that cut. Indoors: the cell's curve, from its fog near to min(fog far, clip) (full fog by the clip even in
	// the few cells whose fog far lies past it), times a fade over the outer half of that way (t 0.5 to 1), so rooms whose far walls are
	// nearer keep the look they had; the background (the dome) included, and the cell's own fog colour (pureFog) on both sides of the
	// cut, so no edge is left. TESR_VolumetricFogSun.x indoors (SunGlow there, an exterior-only setting): 1 / (min(fog far, clip) - near),
	// 0 for a cell without fog distances (no fade, as the game) (VolumetricFog.cpp UpdateConstants; worked out there because every
	// constant expression here becomes a preshader register, and the composite passes sit at 224).
	// Outdoors as before (vfIsExterior 1: the weather's curve, no fade, never on the sky, the mod's fog colour).
	[branch] if (gameFloor && !classic && TESR_VolumetricFogSun.z > 0.0) {
		float gameNear = TESR_VolumetricFogSun.w;
		float dist = length(eyeVec);
		float tOut = vfFarFog > gameNear + 1.0 ? saturate((dist - gameNear) / (vfFarFog - gameNear)) : 0.0;
		float tIn = saturate((dist - gameNear) * TESR_VolumetricFogSun.x);
		float t = lerp(tIn, tOut, vfIsExterior);
		float gameFog = pows(t, vfFogPower);
		float clipT = saturate(t * 2.0 - 1.0);	// indoors: 0 halfway from the fog near to min(fog far, clip), 1 there
		float clipFade = lerp(clipT * clipT * (3.0 - 2.0 * clipT), 1.0, vfIsExterior);
		float want = saturate(TESR_VolumetricFogSun.z * gameFog * clipFade) * (1.0 - isDome * vfIsExterior);
		float kept = luma(fogMul);	// the share of the picture NVR's fog leaves (its luma weights: no new constant in this full pass)
		float topUp = saturate((want - (1.0 - kept)) / max(kept, 0.0001));
		float3 haze = lerp(pureFog.rgb, fogTint(ambSkyC, pureFog, strV, vfFogSkyColorCoeff, black, vfFogSaturation).rgb * lerp(float3(1, 1, 1), vfNightTint, nightFactor), vfIsExterior);
		fogMul *= 1.0 - topUp;
		fogAdd = lerp(fogAdd, haze, topUp);
	}
}

// Legacy fog (custom build) or the current fog: legacy is a compile-time constant, one per technique. So is classic
// (UNOFFICIAL 2026-10-18 ClassicLook, the *Classic techniques; see ModernFogTerms).
// (UNOFFICIAL 2026-11-03 LampHaze: lamps, a compile-time constant too, compiles the lamps' light in; the legacy fog never has it.)
void FogTerms(float2 uvV, out float3 fogMul, out float3 fogAdd, out float nightAmountOut, bool legacy, bool interior = false, bool classic = false, bool gameFloor = true, bool lamps = true)
{
	if (legacy)
		LegacyFogTerms(uvV, fogMul, fogAdd, nightAmountOut, interior);
	else
		ModernFogTerms(uvV, fogMul, fogAdd, nightAmountOut, interior, classic, gameFloor, lamps);
}

float4 VolumetricFog(VSOUT IN, uniform bool legacy, uniform bool classic = false) : COLOR0
{
	float4 colV = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float3 fogMul, fogAdd;
	float nAmt;
	FogTerms(IN.UVCoord, fogMul, fogAdd, nAmt, legacy, false, classic, true, true);	// (UNOFFICIAL 2026-11-03 LampHaze: lamps on)
	float4 finCol = max(float4(colV.rgb * fogMul + fogAdd, lerp(colV.a, 1.0, nAmt)), 0.0f);
	return delinearize(finCol);
}

float4 PackedFogEstimate(VSOUT IN, uniform bool legacy) : COLOR0
{
	float4 fogAcc = VolumetricFog(IN, legacy);
	fogAcc.a = saturate(readDepth(IN.UVCoord) / farZ);
	return fogAcc;
}

float4 PackedFogCombine(VSOUT IN) : COLOR0
{
	float2 texel = TESR_ReciprocalResolution.xy;
	float2 packedSize = 0.5 / texel;
	float2 position = IN.UVCoord * packedSize - 0.5;
	float2 fraction = frac(position);
	float2 baseUV = (floor(position) + 0.5) * texel;
	float2 minUV = texel * 0.5;
	float2 maxUV = 0.5 - minUV;

	float2 uv00 = clamp(baseUV, minUV, maxUV);
	float2 uv10 = clamp(baseUV + float2(texel.x, 0), minUV, maxUV);
	float2 uv01 = clamp(baseUV + float2(0, texel.y), minUV, maxUV);
	float2 uv11 = clamp(baseUV + texel, minUV, maxUV);
	float4 s00 = tex2D(TESR_RenderedBuffer, uv00);
	float4 s10 = tex2D(TESR_RenderedBuffer, uv10);
	float4 s01 = tex2D(TESR_RenderedBuffer, uv01);
	float4 s11 = tex2D(TESR_RenderedBuffer, uv11);

	float dpt = saturate(readDepth(IN.UVCoord) / farZ);
	float tolerance = max(dpt * 0.02, 0.0005);
	float4 difference = abs(float4(s00.a, s10.a, s01.a, s11.a) - dpt) / tolerance;
	float4 spatial = float4((1 - fraction.x) * (1 - fraction.y), fraction.x * (1 - fraction.y),
		(1 - fraction.x) * fraction.y, fraction.x * fraction.y);
	float4 weights = spatial / (1 + difference * difference * 8);
	float weightSum = max(dot(weights, 1.0), 0.00001);
	float3 colV = (s00.rgb * weights.x + s10.rgb * weights.y + s01.rgb * weights.z + s11.rgb * weights.w) / weightSum;
	return float4(colV, 1);
}


technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 VolumetricFog(false);
	}
}

technique PackedFog
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 PackedFogEstimate(false);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 PackedFogCombine();
	}
}

// Dedicated path: the estimate writes the fog coefficients (see FogTerms) at half resolution
// into two targets at once -- multiply + depth to s6, add to s7 -- and the reconstruct applies
// a depth-aware upsample of them to the FULL-resolution scene, so scene detail is never lost.
// The CPU binds TESR_RenderedBuffer (already equal to the render target) to s0 so no
// full-resolution SourceBuffer copy is needed.
//
// TESR_ samplers are bound by declaration order (EffectRecord::CreateCT), so this one must take
// the next register after TESR_NormalsBuffer (s4); the manually bound NVR_ samplers follow it.
sampler2D TESR_PointShadowBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
float4 NVR_FogLayout; // xy: half extent / full extent, zw: 1 / half-resolution dimensions
sampler2D NVR_FogBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_FogBufferAdd : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_AOBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct FogTermsOut
{
	float4 mulDepth : COLOR0;
	float4 add : COLOR1;
};

// Each half-resolution texel stands for a 2x2 block of screen pixels, and its centre (IN.UVCoord, see HalfVS) is the
// corner the four share. The fog is worked out at the block's NEAREST pixel, at that pixel's own centre, and stored with
// that pixel's exact depth. (It used to be worked out at the shared corner, where the depth buffer's linear filter blends
// the four: where a thin pole, branch or wire crossed the block, that was a distance nothing is at, which matched neither
// the object nor the sky in FogApply. Thin objects then took the sky's fog, or none, and it flickered as they moved.)
// The nearest pixel wins so thin things in front, which may cover only one pixel of a block, always get a sample of their
// own; the sky or ground behind them is covered by the neighbouring blocks or, where none of those is at their distance
// either (a gap in foliage), by FogApply working the fog out for that pixel itself.
FogTermsOut DedicatedFogEstimate(VSOUT IN, uniform bool legacy, uniform bool interior = false, uniform bool classic = false)
{
	FogTermsOut outV;
	float2 h = 0.5 * (NVR_FogLayout.zw - TESR_ReciprocalResolution.xy);	// UNOFFICIAL: half a pixel at half size, 0 at full size (Max Quality)
	float2 best = IN.UVCoord - h;
	float bestDepth = readDepthLod(best);
	const float2 others[3] = { float2(h.x, -h.y), float2(-h.x, h.y), float2(h.x, h.y) };
	[unroll] for (int i = 0; i < 3; i++) {
		float2 candidate = IN.UVCoord + others[i];
		float candidateDepth = readDepthLod(candidate);
		if (candidateDepth < bestDepth) { best = candidate; bestDepth = candidateDepth; }
	}
	float3 fogMul, fogAdd;
	float nAmt;
	FogTerms(best, fogMul, fogAdd, nAmt, legacy, interior, classic, true, true);	// (UNOFFICIAL 2026-11-03 LampHaze: lamps on)
	outV.mulDepth = float4(fogMul, saturate(bestDepth / farZ));
	outV.add = float4(fogAdd, 1);
	return outV;
}

VSOUT HalfVS(VSIN IN)
{
	// Move the shared quad's full-resolution half-texel offset to the half-resolution
	// texel centre, which is where FogApply assumes each estimate lives.
	VSOUT outV = FrameVS(IN);
	outV.UVCoord += 0.5 * (NVR_FogLayout.zw - TESR_ReciprocalResolution.xy);
	return outV;
}

// Depth-aware upsample of the half-resolution fog coefficients, applied to a gamma-space colour.
float3 FogApply(float3 gammaColor, float2 uvV, bool legacy, bool interior = false, bool classic = false, bool gameFloor = true, bool lamps = false)
{
	float2 texel = NVR_FogLayout.zw;
	float2 position = uvV / texel - 0.5;
	float2 fraction = frac(position);
	float2 baseUV = (floor(position) + 0.5) * texel;
	float2 minUV = texel * 0.5;
	float2 maxUV = TESR_RenderScale.xy - minUV;	// UNOFFICIAL 2026-10-27 RenderScale v2: the drawn part's edge (1 at native)

	float2 uv00 = clamp(baseUV, minUV, maxUV);
	float2 uv10 = clamp(baseUV + float2(texel.x, 0), minUV, maxUV);
	float2 uv01 = clamp(baseUV + float2(0, texel.y), minUV, maxUV);
	float2 uv11 = clamp(baseUV + texel, minUV, maxUV);
	float4 s00 = tex2Dlod(NVR_FogBuffer, float4(uv00, 0, 0));
	float4 s10 = tex2Dlod(NVR_FogBuffer, float4(uv10, 0, 0));
	float4 s01 = tex2Dlod(NVR_FogBuffer, float4(uv01, 0, 0));
	float4 s11 = tex2Dlod(NVR_FogBuffer, float4(uv11, 0, 0));

	float dpt = saturate(readDepthLod(uvV) / farZ);
	float tolerance = max(dpt * 0.02, 0.0005);
	float4 difference = abs(float4(s00.a, s10.a, s01.a, s11.a) - dpt) / tolerance;
	float4 spatial = float4((1 - fraction.x) * (1 - fraction.y), fraction.x * (1 - fraction.y),
		(1 - fraction.x) * fraction.y, fraction.x * fraction.y);
	float4 weights = spatial / (1 + difference * difference * 8);
	weights /= max(dot(weights, 1.0), 0.00001);
	float3 fogMul = s00.rgb * weights.x + s10.rgb * weights.y + s01.rgb * weights.z + s11.rgb * weights.w;
	float3 fogAdd = tex2Dlod(NVR_FogBufferAdd, float4(uv00, 0, 0)).rgb * weights.x + tex2Dlod(NVR_FogBufferAdd, float4(uv10, 0, 0)).rgb * weights.y +
		tex2Dlod(NVR_FogBufferAdd, float4(uv01, 0, 0)).rgb * weights.z + tex2Dlod(NVR_FogBufferAdd, float4(uv11, 0, 0)).rgb * weights.w;
	// None of the four estimates is at this pixel's distance (within about 6%): the edge of a thin object, or a gap in
	// foliage through to the sky. Blending them gave it the fog of something else (dark specks in pine trees, poles
	// with no fog, flickering as they moved), so the fog is worked out for this pixel itself. It is rare (edges and
	// gaps only), so the half-resolution saving stays.
	float bestDifference = min(min(difference.x, difference.y), min(difference.z, difference.w));
	[branch] if (bestDifference > 3.0) {
		float nAmt;
		FogTerms(uvV, fogMul, fogAdd, nAmt, legacy, interior, classic, gameFloor, lamps);
	}

	float3 colV = linearize(gammaColor);
	return delinearize(max(colV * fogMul + fogAdd, 0.0f));
}

float4 DedicatedFogCombine(VSOUT IN, uniform bool legacy, uniform bool classic = false) : COLOR0
{
	float3 colV = FogApply(tex2D(TESR_SourceBuffer, IN.UVCoord).rgb, IN.UVCoord, legacy, false, classic, true, true);	// (UNOFFICIAL 2026-11-03 LampHaze: lamps on)
	[branch] if (NVR_FogGridFlags.y > 0.5) colV = FogGridLight(colV, IN.UVCoord);	// UNOFFICIAL froxel grid
	return float4(colV, 1);
}

// ---- Composite apply: exterior sun shadows and AO folded into the fog reconstruct ----
// Each of these was a separate full-resolution pass that read and rewrote the whole HDR frame.
// The functions below are the same per-pixel maths as ShadowsExteriors.fx.hlsl Shadow() and
// AmbientOcclusion.fx.hlsl DedicatedCombine, applied in the same order (shadows, AO, fog), so
// the only difference is that intermediate colours stay in registers instead of 16-bit targets.
// Keep them in step with those files.
float4 NVR_CompositeFlags; // x: apply exterior sun shadows, y: apply AO (both deferred by the CPU)
float4 NVR_CompositeAOTexel; // xy: 1 / AO target dimensions (half, or quarter with AOLowRes); UNOFFICIAL 2026-10-20 (skyamb) zw: LightCap (Includes/SkyTake.hlsl: z the cap, w no limit)
float4 TESR_ShadowData;    // y: darkness
float4 TESR_WaterSettings; // x: water height, z: camera underwater
float4 TESR_AmbientOcclusionAOData; // z: clamp
float4 TESR_AmbientOcclusionData;   // y: luma threshold, z: blur drop threshold
static const float CompositeAOEndFade = 8000;
// UNOFFICIAL 2026-10-17 Skylighting v2 (NVR_CompositeFlags.z): the sky factor (AmbientOcclusion.fx SkyVisibilityV2: the sky's share
// of the light only, at the AO's resolution), bound by hand on stage 12 (VolumetricFog.cpp BindSky), upsampled with the AO's weights.
// No new constants or literals here: with the physical sky this pass already uses c0-c218 of 224.
sampler2D NVR_SkyVis : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-20 (skyamb) LightCap: the light the sky may take is limited (Includes/SkyTake.hlsl); its b holds the visibility.
#include "Includes/SkyTake.hlsl"
// UNOFFICIAL 2026-10-19 SunFar (SunFar.fx.hlsl): the land's and the clouds' shadow, a factor on the stored (gamma) colour, bound by
// hand on stage 13 while NVR_SunFarFlags.x is 1 (VolumetricFog.cpp BindSunFar). One more constant register (c219 of 224 at most).
float4 NVR_SunFarFlags;
sampler2D NVR_SunFar : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

float3 CompositeSunShadow(float3 linearColor, float2 uvV)
{
	// (No branch on the flag: the composites have no boolean register to spare with the physical sky compiled in; 0 or 1. A dynamic
	// branch on uv.x < the flag compiles, but measured slower with SunFar off: this pass 0.47 -> 0.52-0.53 ms at the dusk lake, two
	// launches each, while this read from the unbound stage costs nothing measurable, 0.47 as without SunFar at all. 2026-10-19.)
	linearColor *= lerp(1.0, pows(tex2Dlod(NVR_SunFar, float4(uvV, 0, 0)).rgb, 2.2), NVR_SunFarFlags.x);
	[branch] if (TESR_WaterSettings.z == 1) {
		float dpt = readDepthLod(uvV);
		float3 worldP = TESR_CameraPosition.xyz + toWorld(uvV) * dpt;
		float3 wNrm = GetWorldNormalLod(uvV);
		if (worldP.z < (TESR_WaterSettings.x + 2) && worldP.z > (TESR_WaterSettings.x - 2) && dot(wNrm, float3(0, 0, -1)) > 0.999)
			return linearColor;
	}

	float darkness = max(0.0, 1 - TESR_ShadowData.y);
	float2 shadow = tex2Dlod(TESR_PointShadowBuffer, float4(uvV, 0, 0)).rg;
	shadow.r = lerp(TESR_ShadowFade.x, 1.0f, shadow.r);
	float ambient = lerp(1, luma(TESR_SunAmbient), darkness * TESR_ShadowFade.z);
	shadow.r = lerp(0, ambient, shadow.r);
	shadow.r += shadow.g;
	shadow.r = saturate(lerp(darkness, 1.0, shadow.r));

	float3 skyC = pows(TESR_SkyColor.rgb, 2.2);
	float3 colorShadow = luma(linearColor) * shadow.r * skyC;
	colorShadow = lerp(colorShadow, linearColor * shadow.r, saturate(shadow.r + 0.5));
	return max(0.0, colorShadow);
}

float3 CompositeAO(float3 linearColor, float2 uvV)
{
	float dpt = readDepthLod(uvV);
	[branch] if (dpt >= CompositeAOEndFade) return linearColor;

	float2 texel = NVR_CompositeAOTexel.xy;
	float2 position = uvV / texel - 0.5;
	float2 base = floor(position);
	float2 fraction = frac(position);
	float blurDrop = TESR_AmbientOcclusionData.z;
	float sum = 0, weights = 0;
	float2 skySum = 0;	// UNOFFICIAL 2026-10-20 (skyamb): x the factor, y the visibility (LightCap)
	const bool sky = NVR_CompositeFlags.z > 0.5;	// UNOFFICIAL Skylighting v2
	[unroll] for (int y = 0; y < 2; ++y) {
		[unroll] for (int x = 0; x < 2; ++x) {
			float2 sampleUV = clampToRect((base + float2(x, y) + 0.5) * texel, texel);	// UNOFFICIAL 2026-10-27 RenderScale v2 (as before at native)
			float2 aoSample = tex2Dlod(NVR_AOBuffer, float4(sampleUV, 0, 0)).rg;
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1 + abs(aoSample.y - dpt) / max(blurDrop, 0.001);
			sum += aoSample.x * weight;
			weights += weight;
			[branch] if (sky) skySum += tex2Dlod(NVR_SkyVis, float4(sampleUV, 0, 0)).rb * weight;
		}
	}
	float ao = lerp(TESR_AmbientOcclusionAOData.z, 1, sum / max(weights, 1.0e-6));
	ao = lerp(ao, 1, saturate(luma(linearColor) - TESR_AmbientOcclusionData.y));
	// the sky factor (its own share; not part of the AO's clamp/fade), at most what LightCap allows (UNOFFICIAL 2026-10-20 skyamb)
	if (sky) { float2 skyAt = saturate(skySum / max(weights, 1.0e-6)); return SkyTake(linearColor * ao, max(skyAt.x, TESR_AmbientOcclusionAOData.z), skyAt.y, NVR_CompositeAOTexel.z, NVR_CompositeAOTexel.w); }
	return linearColor * ao;
}

float4 DedicatedFogComposite(VSOUT IN, uniform bool legacy, uniform bool classic = false) : COLOR0
{
	float3 colV = tex2D(TESR_SourceBuffer, IN.UVCoord).rgb;
	// Shadows and AO both use gamma 2.2: retain their shared linear value until
	// both finish. FogApply uses the distinct piecewise sRGB transfer function.
	colV = pows(colV, 2.2);
	[branch] if (NVR_CompositeFlags.x > 0.5) colV = CompositeSunShadow(colV, IN.UVCoord);
	[branch] if (NVR_CompositeFlags.y > 0.5) colV = CompositeAO(colV, IN.UVCoord);
	colV = pows(colV, 1.0 / 2.2);
	// (UNOFFICIAL 2026-10-30 GameFogFloor: not in this pass's per-pixel fallback with the physical sky: ModernFogTerms says why)
	// (UNOFFICIAL 2026-11-03 LampHaze: nor the lamps' light, with or without the physical sky: either way it put this pass over the 224
	// constant registers. Those rare pixels keep the fog without the lamps' light; see the README.)
	colV = FogApply(colV, IN.UVCoord, legacy, false, classic, !PHYSICAL_SKY_ON);
	[branch] if (NVR_FogGridFlags.y > 0.5) colV = FogGridLight(colV, IN.UVCoord);	// UNOFFICIAL froxel grid
	return float4(colV, 1);
}

technique DedicatedFog
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(false);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogCombine(false);
	}
}

technique CompositeFog
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(false);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogComposite(false);
	}
}

// ---- Custom build: the same techniques with the legacy fog (VolumetricFog.cpp picks them when LegacyFog is on) ----
technique VolumetricFogLegacy
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 VolumetricFog(true);
	}
}

technique PackedFogLegacy
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 PackedFogEstimate(true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 PackedFogCombine();
	}
}

technique DedicatedFogLegacy
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogCombine(true);
	}
}

technique CompositeFogLegacy
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogComposite(true);
	}
}

// ---- Shadow + AO apply, merged (UNOFFICIAL) ----
// When effects run between the shadow/AO apply and the fog (bounce light, specular, the flashlight...), the composite
// cannot go into the fog pass, and the two applies were two full-resolution passes. This one pass does both with the
// same functions in the same order (shadows, then AO), keeping the colour in registers in between: one read and write
// of the frame instead of two. (VolumetricFogEffect::RenderShadowAO; NVR_CompositeFlags x shadows, y AO.)
float4 ShadowAOApply(VSOUT IN) : COLOR0
{
	float3 colV = pows(tex2D(TESR_SourceBuffer, IN.UVCoord).rgb, 2.2);
	[branch] if (NVR_CompositeFlags.x > 0.5) colV = CompositeSunShadow(colV, IN.UVCoord);
	[branch] if (NVR_CompositeFlags.y > 0.5) colV = CompositeAO(colV, IN.UVCoord);
	return float4(pows(colV, 1.0 / 2.2), 1);
}

technique ApplyShadowAO
{
	pass Apply
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ShadowAOApply();
	}
}

// ---- Interior composite (UNOFFICIAL 2026-10-18, [Main.Main.Performance] InteriorShadowFold) ----
// Indoors with the forward lamp shadows on, the darkening of what no tracked lamp reaches (KeepDarkening, unblurred: no
// DarkeningBlur) was ShadowsInteriors.fx's own full-resolution pass (DedicatedInteriorShadows, last pass: the scene times the
// lamp mask, read straight from TESR_PointShadowBuffer), written to the 16-bit float frame and read back here. It is now done
// here, before the AO and the fog, the way the exterior composite does the sun shadows: the same function
// (Includes/InteriorShadowApply.hlsl, one source for both), on the same inputs, and its result rounded to 16-bit float exactly
// as that pass's render target stored it, so every following step sees the very value it read before. How a GPU rounds a float
// into a 16-bit float target is its own choice (the GTX 1070 truncates: evidence\intshadow\fp16test, 1,048,576 of 1,048,576 values),
// so VolumetricFogEffect::HalfStoreMode measures it once with technique HalfStoreProbe (below) and picks RoundToHalf or
// TruncateToHalf; neither matching = no fold. The picture stays bit for bit the same; the pass and its read and write of the whole
// HDR frame are saved.
// NVR_CompositeFlags here: y AO, z sky factor (as above), w 0 round to nearest even, 1 no rounding (diagnostic), 2 round toward zero.
static const float InteriorShadowMaxDistance = TESR_ShadowFade.w;
#define INTERIOR_SHADOW_MAXDISTANCE InteriorShadowMaxDistance
#include "Includes/InteriorShadowApply.hlsl"

// A value as a 16-bit float render target stores it (A16B16G16R16F: 10 mantissa bits, exponents down to -14, smaller values
// in steps of 2^-24), round to nearest, ties to even. Every step is exact in 32-bit float: the scales are powers of two, and
// the exponent from log2 is corrected against exp2 of itself (log2 may be a hair off at powers of two).
float3 HalfExponent(float3 a)
{
	float3 e = floor(log2(a));
	float3 pV = exp2(e);
	e += (a >= 2 * pV);
	e -= (a < pV);
	return max(e, -14);
}

float3 RoundToHalf(float3 x)
{
	float3 a = abs(x);
	float3 e = HalfExponent(a);
	float3 q = a * exp2(10 - e);	// the value in units of its 16-bit step
	float3 r = floor(q + 0.5);
	r -= ((r - q) == 0.5) * (frac(r * 0.5) != 0);	// a tie goes to the even step
	return sign(x) * (r * exp2(e - 10));
}

float3 TruncateToHalf(float3 x)
{
	float3 a = abs(x);
	float3 e = HalfExponent(a);
	return sign(x) * (floor(a * exp2(10 - e)) * exp2(e - 10));
}

// As the separate pass's render target stores it (NVR_CompositeFlags.w: the GPU's rounding, HalfStoreMode).
float3 InteriorStoreRound(float3 colV)
{
	[branch] if (NVR_CompositeFlags.w < 0.5) colV = RoundToHalf(colV);
	else if (NVR_CompositeFlags.w > 1.5) colV = TruncateToHalf(colV);
	return colV;
}

// What the separate pass stored at this pixel (for the fog's sky test, getSky): explicit-LOD reads, it runs in dynamic branches.
float4 InteriorStoredColor(float2 uvV)
{
	float4 scene = tex2Dlod(TESR_SourceBuffer, float4(uvV, 0, 0));
	return float4(InteriorStoreRound(ApplyInteriorShadowLod(uvV, scene, tex2Dlod(TESR_PointShadowBuffer, float4(uvV, 0, 0)).r).rgb), scene.a);
}

float4 DedicatedFogCompositeInterior(VSOUT IN, uniform bool legacy, uniform bool classic = false) : COLOR0
{
	float3 colV = InteriorStoreRound(ApplyInteriorShadow(IN.UVCoord, tex2D(TESR_SourceBuffer, IN.UVCoord), tex2D(TESR_PointShadowBuffer, IN.UVCoord).r).rgb);
	// From here as DedicatedFogComposite (no sun shadows indoors); the fog's sky test reads the darkened scene (getSky).
	colV = pows(colV, 2.2);
	[branch] if (NVR_CompositeFlags.y > 0.5) colV = CompositeAO(colV, IN.UVCoord);
	colV = pows(colV, 1.0 / 2.2);
	// (UNOFFICIAL 2026-11-03 LampHaze: not in this pass's per-pixel fallback: with it the pass went over ps_3_0's 224 constant registers. The
	// rare pixels that take it (edges matching none of the four half-size estimates) keep the fog without the lamps' light; see the README.)
	colV = FogApply(colV, IN.UVCoord, legacy, true, classic);
	[branch] if (NVR_FogGridFlags.y > 0.5) colV = FogGridLight(colV, IN.UVCoord);	// UNOFFICIAL froxel grid
	return float4(colV, 1);
}

technique CompositeFogInterior
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(false, true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogCompositeInterior(false);
	}
}

// One draw into a 64x16 A16B16G16R16F target, read back once (HalfStoreMode): r = the value as the GPU stores it, g = RoundToHalf,
// b = TruncateToHalf of it. The values sit exactly halfway between two 16-bit steps after an odd step (nearest-even rounds up,
// truncation down) or three quarters of the way (nearest rounds up), at exponents -12..11, half of them negative.
float4 HalfStoreProbePS(float2 vpos : VPOS) : COLOR0
{
	float i = floor(vpos.x) + 64 * floor(vpos.y);	// 0..1023
	float k = fmod(i, 256);
	float e = fmod(floor(i / 8), 24) - 12;
	float steps = fmod(i, 2) < 0.5 ? 2 * k + 1.5 : 2 * k + 0.75;
	float v = (1 + steps / 1024) * exp2(e);
	v = fmod(floor(i / 4), 2) < 0.5 ? v : -v;
	return float4(v, RoundToHalf(v.xxx).x, TruncateToHalf(v.xxx).x, 1);
}

technique HalfStoreProbe
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 HalfStoreProbePS();
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = none;
		AlphaBlendEnable = false;
		AlphaTestEnable = false;
		SRGBWriteEnable = false;
		ColorWriteEnable = 0xF;
		ScissorTestEnable = false;
	}
}

technique CompositeFogInteriorLegacy
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(true, true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogCompositeInterior(true);
	}
}

// ---- ClassicLook (UNOFFICIAL 2026-10-18, [Shaders.VolumetricFog.Main] ClassicLook) ----
// The look of the original NVR fog (upstream master ab16e86c; see ModernFogTerms), with the same speed work as the techniques
// above. VolumetricFog.cpp picks these when ClassicLook is on (and LegacyFog is off). ClassicLook 2 = VolumetricFogClassic only:
// every pixel worked out at full resolution in one pass, as master drew it (no half-resolution coefficients).
technique VolumetricFogClassic
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 VolumetricFog(false, true);
	}
}

technique DedicatedFogClassic
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(false, false, true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogCombine(false, true);
	}
}

technique CompositeFogClassic
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(false, false, true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogComposite(false, true);
	}
}

technique CompositeFogInteriorClassic
{
	pass Estimate
	{
		VertexShader = compile vs_3_0 HalfVS();
		PixelShader = compile ps_3_0 DedicatedFogEstimate(false, true, true);
	}
	pass Reconstruct
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DedicatedFogCompositeInterior(false, true);
	}
}

// ---- FogReference (UNOFFICIAL 2026-10-18, DIAGNOSTIC: [Main.Main.Performance] FogReferenceTest, development builds only) ----
// The original NVR fog's pixel shader, upstream master ab16e86c VolumetricFog.fx.hlsl VolumetricFog(), copied line for line, so
// ClassicLook can be compared with master's own maths on the very same frame (same inputs, same launch). Drawn at full resolution
// in one pass with the shadow and AO applies as passes of their own, as master drew it. Three differences from the copy, all
// forced by what changed around it: the 32-step height fog loop is master's (MasterHeightFog, below; the techniques above use
// its closed form); the sky ambient reads the game's own sky (TESR_SkyIrradianceGame, which is what master's TESR_SkyIrradiance
// held: ours also carries the physical sky); and the cascade shadow lookup (GetFogShadowValue) takes the PCF branch when the
// sun shadows are in Mode 3, whose atlas master's VSM/EVSM lookup cannot read. Never used in normal play.
float MasterHeightFog(float dstV, float falV, float3 worldP, float heightOffset){
	float3 eyeVec = worldP - TESR_CameraPosition.xyz;
	float3 stepV = eyeVec / stepnum;
	float stepLen = length(stepV);

	float3 pos = TESR_CameraPosition.xyz - float3(0, 0, vfFOG_GROUND + heightOffset * 1000);
	float fogAcc = 0;
	[unroll]
	for (int i = 0; i < stepnum; i++){
		pos += stepV;
		fogAcc += exp(-falV * pos.z) * stepLen;
	}
	float distanceModifier = saturate(dstV / max(length(eyeVec), 0.0001));
	return fogAcc * distanceModifier;
}

void MasterHeightFogTerms(float dstV, float3 extinctionColor, float3 inscatteringColor, float densV, float falV, float3 worldP, float offset, out float3 extColor, out float3 insColor){
	float fogAcc = densV * 0.00001 * MasterHeightFog(dstV, falV * 0.0001, worldP, offset);
	extColor = 1 - exp(-fogAcc * extinctionColor);
	insColor = 1 - exp(-fogAcc * inscatteringColor);
}

float4 MasterReferenceFog(VSOUT IN) : COLOR0
{
	float4 colV = linearize(tex2D(TESR_SourceBuffer, IN.UVCoord));
	float4 pureFog = linearize(TESR_FogColor);

	float dpt = readDepth(IN.UVCoord);
	float isDay = smoothstep(0.4, 0.8, TESR_SunAmount.x);
	float isDayF = smoothstep(0.1, 0.6, TESR_SunAmount.x);
	float nightFactor = vfIsExterior * (1 - isDay);

	float3 eyeVec = toWorld(IN.UVCoord);
	float3 eyeDirV = normalize(eyeVec);
	eyeVec *= dpt;
	float3 worldP = eyeVec + TESR_CameraPosition.xyz;

	float nrmDepth = length(eyeVec) / farZ; // make sure depth is the same at the center and edges of the screen
	float fogPw = lerp(1, vfFogPower, saturate(vfWeatherImpact));
	float fogD = pows(nrmDepth, fogPw) * farZ;

	float isSk = getSky(IN.UVCoord);
	float skyMaskF = lerp(1.0, 1.0 - isSk, vfWeatherFilterBlend); // weather can disable the sky exclusion so fog blends fully into an overcast sky
	skyMaskF = lerp(skyMaskF, 1.0, nightFactor * vfNightDisableSkyMask);

	float isDome = dpt / farZ >= 0.9961;

	// ---- vanilla-anchored density, derived from the active weather's own authored fog shape ----
	float vanStr = pows((saturate(1 - vfFarFog / farZ) + saturate(1 - vfNearFog / farZ)) / 2, 2) / (vfFogPower + 1);

	// ---- NVR-driven density: time-of-day curve, sunrise/sunset boost, animated 3D noise ----
	float sunsetB = sin(saturate(TESR_SunAmount.x) * PI) * vfIsExterior;
	float noonT = 1 - saturate(abs(TESR_GameTime.y - 12) / 6); // 0 at 6am/6pm, 1 at solar noon
	float todScale = lerp(1.0, lerp(1.0, 1.0 - vfMorningFogDip, noonT), vfIsExterior);
	float nDensS = lerp(1.0, vfNightDensityScale, nightFactor);

	float nWind = vfWindSpeed * lerp(1.0, vfNightWindSpeedScale, nightFactor);
	float3 winOff = float3(vfWindDirection * nWind * TESR_GameTime.x * 0.002, 0);
	float nVal = fbm3((worldP + winOff) / (1500 * vfNoiseScale));
	float nNoiseS = vfNoiseStrength * lerp(1.0, vfNightNoiseStrengthScale, nightFactor);

	float noiseSkyM = 1 - isDome;
	float nvrDens = vfBaseDensity * todScale * nDensS * lerp(1.0, nVal, nNoiseS * noiseSkyM);
	nvrDens += vfSunriseSunsetBoost * sunsetB;

	float strV = max(0, nvrDens + vfWeatherImpact * vanStr);

	// ---- shadow-coupled sun in-scattering, sampled once per pixel at the fog's terminating point ----
	float3 wNrm = GetWorldNormal(IN.UVCoord);
	float shVis = 1.0;
	[branch]
	if (vfIsExterior > 0.5 && vfShadowStrength > 0.0 && TESR_ShadowFade.y > 0.5) {
		shVis = lerp(1.0, GetFogShadowVisibility(float4(worldP, 1.0), wNrm), vfShadowStrength);
	}

	// default values used by interiors
	float distFog = 0.0, distHFade = 0.0;
	float4 skyC = pureFog;
	float4 sunV = black;

	// sun/sky/distant fog coloring specific to exteriors
	if (vfIsExterior){
		float3 upV = blue.xyz;
		float sunH = shade(TESR_SunPosition.xyz, upV);
		float sunD = dot(eyeDirV, TESR_SunPosition.xyz);
		float sunInf = pows(compress(sunD), vfSUNINFLUENCE);
		float4 sunColV = float4(GetSunColor(sunH, 1, TESR_SunAmount.x, TESR_SunDiskColor.rgb, TESR_SunsetColor.rgb), 1);
		sunColV *= isDay * vfIsExterior;

		skyC.rgb = GetSkyColor(0.5, 1, sunH, sunInf, TESR_SkyData.z, TESR_SkyColor.rgb, TESR_SkyLowColor.rgb, TESR_HorizonColor.rgb, black.rgb) * TESR_SunsetColor.w;

		// add sun influence/scattering, narrowed/widened by PhaseAsymmetry, gated by the shadow cascades
		float sunStr = TESR_FogData.z / (1 + 2 * strV);
		sunStr = lerp(sunStr, max(sunStr * 2, 10), nrmDepth);

		float sunScat = pows(compress(sunD), lerp(8.0, 1.0, vfPhaseAsymmetry) + sunStr);
		sunScat *= pow(1 - sunH, 2) * isDayF * TESR_FogData.z;
		sunV = sunColV * sunScat * shVis * TESR_PBRData.z * 100;

		distFog = pows(smoothstep(vfDistantFogRange, 1.0, nrmDepth), 0.5);
		distHFade = (vfDistantFogHeight == 0) ? (1.0 - isSk) : saturate(exp(-worldP.z / (80000 * vfDistantFogHeight)));	// UNOFFICIAL 2026-10-30: at most 1 (below z 0 the exp passed 1 and the sky lerp ran past the sky colour: white, then inf in the TAA history)
	}

	// ---- sky ambient in-scattering (SH-reconstructed, or flat fallback), unshadowed ----
	float3 ambC = vfSkyAmbientAvailable > 0.5
		? SkyIrradianceGameLinear(float3(0, 0, 1))
		: linearize(TESR_SkyColor).rgb;
	ambC *= TESR_PBRData.w;

	float3 nAmb = linearize(TESR_SunAmbient).rgb * vfMoonVisibility * vfNightAmbientStrength * nightFactor;
	ambC = max(ambC, nAmb);

	float4 ambSkyC = float4(lerp(ambC, skyC.rgb, saturate(vfWeatherImpact)), 1);

	float4 fogFinal = fogTint(ambSkyC, pureFog, strV, vfFogSkyColorCoeff, sunV, vfFogSaturation);
	fogFinal.rgb *= lerp(float3(1, 1, 1), vfNightTint, nightFactor);

	float nExt = vfExtinction * lerp(1.0, vfNightExtinctionScale, nightFactor);
	float nIns = vfInscattering * lerp(1.0, vfNightInscatteringScale, nightFactor);
	float nHFall = vfHeightFalloff * lerp(1.0, vfNightHeightFalloffScale, nightFactor);
	float nMaxH = vfMaxHeight + vfNightMaxHeightOffset * nightFactor;

	float3 flExt, flIns, hExt, hIns;
	getFlatFogTerms(fogD, nExt, nIns, strV, flExt, flIns);
	float falArg = 1.5 / (fogPw * nHFall);
	MasterHeightFogTerms(fogD, nExt, nIns, strV, falArg, worldP, nMaxH, hExt, hIns);

	float3 blExt = lerp(flExt, hExt, vfHeightInfluence);
	float3 blIns = lerp(flIns, hIns, vfHeightInfluence);

	float3 fiExt = max(blExt, flExt * vfMinDensityFloor);
	float3 fiIns = max(blIns, flIns * vfMinDensityFloor);

	float3 fogMix = colV.rgb * saturate(1 - fiExt) + fogFinal.rgb * saturate(fiIns);
	float4 finCol = float4(lerp(colV.rgb, fogMix, skyMaskF), 1);

	// ---- aerial perspective: mid-to-far distance tint on non-sky terrain ----
	float aDayF = smoothstep(0.1, vfAerialDayFadeStart, TESR_SunAmount.x);
	float aFac = smoothstep(vfAerialRangeStart, 1.0, nrmDepth) * (1.0 - isDome) * vfIsExterior;
	float3 aTint = lerp(ambC, linearize(TESR_VolumetricFogAerialTint).rgb, vfAerialTintBlend);

	float3 aTintC = saturate(aTint);
	finCol.rgb = lerp(finCol.rgb, finCol.rgb * aTintC, aFac * vfAerialStrength * skyMaskF * aDayF);

	// ---- distant fog: horizon Z-fighting/sky-seam matte ----
	finCol = lerp(finCol, skyC, distFog * saturate(vfDistantFogBlend) * distHFade * vfIsExterior);

	finCol.rgb = lerp(finCol.rgb, skyC.rgb, isSk * vfEdgeAA * vfIsExterior);

	float nAmt = vfFogAmount * lerp(1.0, vfNightAmountScale, nightFactor);
	finCol = max(lerp(colV, finCol, nAmt), 0.0f);

	return delinearize(finCol);
}

technique FogReference
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 MasterReferenceFog();
	}
}
