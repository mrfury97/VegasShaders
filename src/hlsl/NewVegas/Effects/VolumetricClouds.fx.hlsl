// UNOFFICIAL: volumetric clouds -- a layer of real 3D clouds high above the ground, marched through and lit by the sun,
// in place of the game's flat cloud pictures. After "The real-time volumetric cloudscapes of Horizon Zero Dawn"
// (Schneider, SIGGRAPH 2015) and "Physically based sky, atmosphere and cloud rendering in Frostbite" (Hillaire, 2016).
//
// Three techniques, driven by VolumetricCloudsEffect (src/effects/VolumetricClouds.cpp):
//   1 March     (one texel per 2 x 2 block of the quarter resolution, TESR_CloudCurrent): along the view ray of one
//               quarter-resolution pixel of each block (a different one each frame, all four in four frames) through the
//               cloud layer (a shell round a curved earth, so the clouds sink toward the horizon), at most Steps samples
//               of the cloud density, each lit by a short march toward the sun. Each frame starts the samples at a
//               different offset (blue noise). rgb: light the clouds send toward the eye (linear), a: how much of the sky
//               behind still shows.
//   2 Temporal  (quarter resolution, into the history): the pixel marched this frame blends its new value into its
//               history; the other three keep theirs. Either way the history is found again where the camera turned
//               (the clouds are far enough away that only the turn matters) and kept within this frame's neighbourhood,
//               so nothing smears. That averages the march's grain away at a quarter of its cost.
//   3 ShadowMap (UNOFFICIAL 2026-10-12, Shadows on; 256 x 256, TESR_CloudShadow): looking down on the cloud base round the
//               camera (ShadowRange across), how much sunlight gets through the layer at each point along the light: eight
//               density reads per texel (the full shape with its eroded edges, as the clouds seen). The centre snaps to whole texels so the map does not crawl as the camera moves.
//   0 Composite (full resolution, in the frame chain): the history over the sky (only where the depth buffer shows sky).
//               UNOFFICIAL 2026-10-12, everywhere else: the cloud shadow (the shadow map where the light from this point
//               meets the cloud base) and, on water, the clouds along the reflected view (WaterReflections): read from the
//               history where that direction is on screen and shows sky there, through the water's Fresnel term.
//
// Shapes come from three tiling noise textures made by tools/cloud_noise.cpp: CloudShape (big billows, 3D), CloudDetail
// (wisps, 3D) and CloudWeather (2D: where clouds may form, their type, their density). The weather's coverage, from the
// game's weather type, decides how much of the sky they fill. Units are metres (70 game units each).
// The frame here is gamma encoded HDR (VolumetricFog linearises it the same way): the clouds are added in linear light.

float4 TESR_ReciprocalResolution;
float4 TESR_SunDirection;     // the light: the sun by day, the moon's light by night (ShaderManager)
float4 TESR_SunColor;          // its colour
float4 TESR_SunAmount;         // x: day 0..1
float4 TESR_SkyColor;          // the sky's top colour
float4 TESR_SkyLowColor;
float4 TESR_GameMultisample;  // UNOFFICIAL 2026-11-05 (Balebandro 256de14) x: the game's MSAA samples (iMultiSample as read; 0 or 1 = off)
float4 TESR_HorizonColor;
float4 TESR_CloudsShape;       // x: base altitude (m), y: thickness (m), z: coverage 0..1, w: density
float4 TESR_CloudsNoise;       // x: shape tile (m), y: detail tile (m), z: detail erosion 0..1, w: weather map tile (m)
float4 TESR_CloudsLight;       // x: sun strength, y: ambient strength, z: forward scattering g, w: rain darkening 0..1
float4 TESR_CloudsWind;        // xy: wind offset (m), z: detail drift (m), w: cloud type bias -1..1
float4 TESR_CloudsMarch;       // x: steps, y: max distance (m), z: frame number, w: haze distance (m)
float4 TESR_CloudsLayout;      // xy: one quarter-resolution texel in screen UV, z: history weight (0 = start over), w: brightness
float4 TESR_CloudsBlocks;      // xy: one march texel (a 2 x 2 block) in UV, zw: the block's pixel marched this frame (0 or 1 each)
float4 TESR_CloudsLook;        // x: edge sharpness (1 = soft .. 8 = hard), y: shape noise mip bias (0 = full detail),
                               // z: turbulence (distortion of the noise lookups), w: share of smooth noise in the detail
float4x4 TESR_CloudsPrevViewProj; // last frame's view-projection, for the history
float4 TESR_CloudsDetail;      // UNOFFICIAL x: fine detail strength (0 off), y: fine detail tile (m), z: cloud shadow map size,
                               // w: (2026-10-20) cumulus shaping 0..1 (0 = the older slab profile)
float4 TESR_CloudsExtra;       // UNOFFICIAL x: shadow strength (0 off), y: shadow map width (m), z: 1 = reflection debug view, w: 1 = shadow debug view
float4x4 TESR_CloudsReflectInvVP;    // UNOFFICIAL: the water reflection camera's clip space -> world direction (no translation)
float4x4 TESR_CloudsHistoryViewProj; // UNOFFICIAL: the view-projection the history in TESR_CloudHistory was made with
float4 TESR_CloudsReflectData;       // UNOFFICIAL x: strength in the reflection, yz: its size (texels), w: the depth of the far plane
float4 TESR_CloudsMotion;       // UNOFFICIAL 2026-10-15 xy: the cloud pattern's move this frame (m), w: sharpness 0..1
float4 TESR_CloudsTint;         // UNOFFICIAL 2026-10-25 xyz: the weather's cloud colour now (linear, brightness 1), w: WeatherTint 0..1
float4 TESR_WaterSettings;     // x: height of the water in view (game units)
float4 TESR_CloudsTower;       // UNOFFICIAL 2026-10-25 Towering: x: the ordinary clouds' share of the (taller) layer (1 = off),
                               // y: towering 0..1, z: (2026-10-25) anvil 0..1
float4 TESR_CloudsHistoryScale; // UNOFFICIAL 2026-10-27 RenderScale v2: xy: the part of the history its frame drew (1 = all; the reflection's lookup); 2026-10-28 zw: the composite's jitter offset in screen uv (FSR / DLSS LowResEffects frames, else 0)
float4 TESR_CloudsPhysical;    // UNOFFICIAL x: 1 = PhysicalMoonlight, y: 1 = PhysicalSkyLight (with PHYSICAL_SKY_ON)

// Declared in register order: the effect binds TESR_ samplers by declaration order (a mismatch silently swaps them).
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler3D TESR_CloudShape : register(s2) < string ResourceName = "Effects\CloudShape.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler3D TESR_CloudDetail : register(s3) < string ResourceName = "Effects\CloudDetail.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_CloudWeather : register(s4) < string ResourceName = "Effects\CloudWeather.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BlueNoiseSampler : register(s5) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_CloudCurrent : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_CloudHistory : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// The general 3D noise (tools/cloud_noise.cpp): the turbulence that twists the shape and detail lookups. Last: stage 8.
sampler3D TESR_NoiseVolume : register(s8) < string ResourceName = "Effects\NoiseVolume.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-12: the water test's normals (s9) and the cloud shadow map (s10), after the others in register order.
sampler2D TESR_NormalsBuffer : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_CloudShadow : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-14 physical sky ([Shaders.Sky.Physical], Effects/SkyView.fx.hlsl, Sky.cpp): the clouds take their sunlight
// and sky light from the same air as the sky. Compiled in only when on at startup (PHYSICAL_SKY_ON, EffectRecord::LoadEffect).
// Its table is the next TESR_ sampler in register order (s11).
#ifndef PHYSICAL_SKY_ON
    #define PHYSICAL_SKY_ON 0
#endif
#if PHYSICAL_SKY_ON
float4 TESR_PhysSky;        // x: share of the physical sky, y: brightness (the table's, matched to the weather's sky), z: weather tint, w: haze
float4 TESR_PhysSkySun;     // xyz: the sun's real direction
sampler2D TESR_SkyViewLUT : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#endif
// TESR_ samplers bind to stage = their position among this file's TESR_ samplers, so this one follows SkyViewLUT.
#if PHYSICAL_SKY_ON
sampler2D TESR_CloudCurrentFull : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#else
sampler2D TESR_CloudCurrentFull : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#endif
// UNOFFICIAL 2026-10-15 weather sync: one of the game's cloud layer textures, bound by hand (VolumetricClouds.cpp ProbeLayer).
sampler2D NVR_CloudLayerProbe : register(s13) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
texture NoTexture;   // never set: unbinds a technique's own render target from its sampler

static const float Base = TESR_CloudsShape.x;
static const float Thickness = max(TESR_CloudsShape.y, 10.0);
static const float Coverage = saturate(TESR_CloudsShape.z);
static const float Density = TESR_CloudsShape.w;
static const float ShapeTile = max(TESR_CloudsNoise.x, 1.0);
static const float DetailTile = max(TESR_CloudsNoise.y, 1.0);
static const float Erosion = saturate(TESR_CloudsNoise.z);
static const float WeatherTile = max(TESR_CloudsNoise.w, 1.0);
static const float SunStrength = TESR_CloudsLight.x;
static const float AmbientStrength = TESR_CloudsLight.y;
static const float Forward = TESR_CloudsLight.z;
static const float RainDark = saturate(TESR_CloudsLight.w);
static const float Steps = max(TESR_CloudsMarch.x, 4.0);
static const float MaxDistance = TESR_CloudsMarch.y;
static const float HazeDistance = max(TESR_CloudsMarch.w, 1.0);
static const float2 LowTexel = TESR_CloudsLayout.xy;
static const float Sharpness = max(TESR_CloudsMotion.w, 0.0);
// UNOFFICIAL 2026-10-20 CumulusShape: heaped fair-weather clouds. It fades out as the sky closes over (cloudy, rain, snow): an
// overcast deck is a layer, not heaps, and keeps the older look.
static const float Cumulus = saturate(TESR_CloudsDetail.w) * (1.0 - smoothstep(0.6, 0.9, saturate(TESR_CloudsShape.z)));   // UNOFFICIAL 2026-10-15: up to 3 (the sharpening; the rest uses up to 1)

// UNOFFICIAL 2026-10-25 Towering: the layer (Thickness above) is made taller on the CPU; the ordinary clouds keep to their old
// share of it at the bottom (TowerShare), and only the strongest heaps climb on toward the new top. Like Cumulus, it fades out as
// the sky closes over (an overcast deck has no towers standing out of it): the CPU eases both values back to off then.
// (Worked out once here, not per sample: this shader is close to ps_3_0's register limits.)
static const float TowerShare = TESR_CloudsTower.x > 0.0 ? clamp(TESR_CloudsTower.x, 0.05, 1.0) : 1.0;   // (never set: off)
static const float InvTowerShare = 1.0 / TowerShare;
static const float TowerGrow = InvTowerShare - 1.0;   // how much taller a full tower is, in ordinary layers, less one
static const float Towering = saturate(TESR_CloudsTower.y);
static const float Anvil = saturate(TESR_CloudsTower.z);
static const float HeapGrow = min(TowerGrow, 1.5);   // the heaps climb at most this many ordinary layers higher (towering cumulus)
static const float StormSpacing = 1.5 * WeatherTile;   // metres: one storm cell at most per square this wide
static const float StormRadius = 0.55 * Thickness;      // metres: a storm column's radius at its base (the tall layer's depth)
static const float BaseThickness = Thickness * TowerShare;   // the depth of the ordinary layer (the Thickness setting)

static const float EarthRadius = 6371000.0;
static const float MetresPerUnit = 1.0 / 70.0;
static const float Extinction = 0.04;   // per metre at density 1: about 25 m into a thick cloud half the light is gone

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
// UNOFFICIAL 2026-11-05 (Balebandro 256de14) the game's MSAA: sky edges. s14: free in both sky variants (s13 is NVR_CloudLayerProbe,
// bound by hand in VolumetricClouds.cpp; the fork's s13 with PHYSICAL_SKY_ON took its stage). EffectRecord binds TESR_ samplers by register.
#define MSAA_SKY_EDGE_REG s14
#include "Includes/MsaaSkyEdge.hlsl"
#if PHYSICAL_SKY_ON
    #include "Includes/Atmosphere.hlsl"
#endif

float Remap(float v, float lo, float hi, float newLo, float newHi) { return newLo + (v - lo) / max(hi - lo, 1e-5) * (newHi - newLo); }

// Where the ray leaves the camera's height h0 by climbing to the height h above it, on a curved earth: the height
// along the ray is h0 + t dz + t^2 / 2R, so t solves t^2 / 2R + t dz - (h - h0) = 0 (h above h0).
float ShellDistance(float dz, float above) {
	return EarthRadius * (-dz + sqrt(dz * dz + 2.0 * above / EarthRadius));
}

// The cloud's vertical profile by type (0 flat layer .. 1 tall heap), h 0 at the base and 1 at the top of the layer.
// hb: the height in the ordinary layer (the same as h but up a tower, Towering), for the base.
float HeightProfile(float h, float hb, float type) {
	float top = lerp(0.3, 1.0, type);
	float bottom = lerp(0.08, 0.12, type);
	return smoothstep(0.0, bottom, hb) * (1.0 - smoothstep(top * 0.6, top, h));
}

// Cloud density at a point: the weather map decides where and how much, the shape texture the billows and the detail
// texture wears away their edges (wispy underneath, billowy on top). cheap skips the detail (for the light march).
// h runs 0..1 through the whole layer (with Towering on, taller than the ordinary clouds). towers (a literal from the technique:
// the *Towers techniques, used only while Towering is on) adds the storms and the taller heaps; without it this is the code as it
// was before Towering, instruction for instruction (no cost for anyone with it off).
float CloudDensity(float3 p, float h, float lod, bool cheap, bool towers = false) {
	if (h <= 0.0 || h >= 1.0) return 0.0;
	float4 weather = tex2Dlod(TESR_CloudWeather, float4(p.xy / WeatherTile, 0.0, 0.0));
	float local = saturate(Remap(weather.r, 1.0 - Coverage, 1.0, 0.0, 1.0));
	float type = saturate(weather.g + TESR_CloudsWind.w);
	// UNOFFICIAL 2026-10-25 Towering (the huge desert cumulus of a western): where the weather map's cloud field is at its
	// strongest and of the heaped kind, that cloud's own top is raised from the ordinary layer's top toward the top of the
	// taller layer: h below becomes the height up this cloud's own column, so a tower is one of the ordinary heaps drawn taller
	// (the noise is read in world space, not stretched: more billows up its sides). The weather map is large and soft, so the
	// towers rise smoothly out of the middle of a field of ordinary heaps. Exactly the old h with Towering off.
	float tower = 0.0, hb = h, storm = 0.0;
	if (towers) {
	tower = saturate(local * type * 4.0 - 0.6) * Towering;
	hb = h * InvTowerShare;   // the height in the ordinary layer: the base's fades keep their depth in metres up a tower
	// UNOFFICIAL 2026-10-25 storms (cumulonimbus, after the user's photos): a few lone storm cells, at most one per StormSpacing
	// square (a random place in it, a random size; how many: Towering), each a broad column from the cloud base to the top of
	// the tall layer that flares out under the top into a flat-topped anvil (TowerAnvil), the anvil pushed a little to one side
	// as the high wind shears it. The outline is the shape noise (big billows), then the detail wears it like the other clouds.
	[branch] if (hb * Towering > 0.0) {
		float2 cell = floor(p.xy / StormSpacing);
		float4 r = tex3Dlod(TESR_NoiseVolume, float4((cell + 0.5) / 64.0, 0.5 / 64.0, 0.0));   // four smooth noises per cell
		float exists = saturate((0.75 * Towering - r.a) * 8.0);
		float2 centre = (cell + 0.3 + 0.4 * r.gb) * StormSpacing;
		float radius = StormRadius * (0.75 + 0.5 * r.r);
		float flare = smoothstep(0.72, 0.97, h);   // h: up the tall layer (the anvil: about its top quarter)
		flare *= flare * Anvil;                   // the underside slopes up and out, steepest near the top
		float2 shear = (r.bg - 0.5) * (2.0 * radius * flare);
		float width = radius * lerp(1.0 - 0.1 * h, 2.2, flare);
		float d = length(p.xy - centre - shear) / width;
		[branch] if (d < 1.6) {
			// big rounded billows on the column and the anvil's rim (the noises a storm's width across), so it is no cylinder
			float4 n = tex3Dlod(TESR_NoiseVolume, float4(p / (2.0 * width), 0.0));
			d += (0.5 - n.b) * 0.6 + (0.5 - n.g) * 0.4;
		}
		storm = saturate((1.0 - d) * 3.0) * exists * smoothstep(0.0, 0.03, hb);
	}
	}
	if (local <= 0.0 && storm <= 0.0) return 0.0;
	if (towers) {
		h = hb / (1.0 + tower * HeapGrow);   // up this heap's own column
		if (h >= 1.0 && storm <= 0.0) return 0.0;
		type += tower * (1.0 - type);   // a tower is the tallest heap of its kind
	}
	// Turbulence: the shape is read through a slow, large distortion, so the noise's cells do not line up into rows of
	// look-alike heaps.
	float3 s = p / ShapeTile;
	// (CumulusShape: less of it, the big twist pulled heaps into smoky streaks)
	s += (tex3Dlod(TESR_NoiseVolume, float4(s * 0.5, lod)).rga - 0.5) * (0.15 * TESR_CloudsLook.z * lerp(1.0, 0.4, Cumulus));
	float shape = tex3Dlod(TESR_CloudShape, float4(s, lod + TESR_CloudsLook.y)).r;   // a mip down: fewer lone puffs
	float base = saturate(Remap(shape * HeightProfile(h, hb, type), 1.0 - local, 1.0, 0.0, 1.0)) * local;
	float stormCloud = towers ? saturate((storm - 0.5 * (1.0 - shape)) * 2.0) : 0.0;   // solid inside, billowed at the rim
	// UNOFFICIAL 2026-10-20 CumulusShape (after photos of Mojave fair-weather cumulus): the older profile cut every cloud off at
	// the same top height, so they came out as flat slabs (boxy). Here the base stays flat and crisp (the condensation level)
	// but the cloud needs a stronger core the higher it goes, so it narrows as it rises: only the cores climb, into rounded
	// domes. Small, weak patches stay low and are dropped (no rows of ragged little puffs).
	// (Worked out always and blended: a branch on a setting takes one of ps_3_0's 16 bool registers, and they are all used.)
	{
		float top = lerp(0.45, 1.0, type);
		float rise = saturate(h / top);
		float threshold = lerp(1.0 - local, 1.0, 0.9 * rise * (1.0 - 0.5 * tower));   // (Towering: a tower stays broad as it climbs)
		float core = saturate(Remap(shape, threshold, 1.0, 0.0, 1.0));
		core *= smoothstep(0.0, 0.03, hb) * (1.0 - smoothstep(0.85, 1.0, rise));
		core = saturate(Remap(core, 0.1, 1.0, 0.0, 1.0)) * local;   // no faint fringe round the cores: those were the wisps
		base = lerp(base, core, Cumulus);
	}
	if (towers) base = max(base, stormCloud);
	if (base <= 0.0) return 0.0;
	if (!cheap) {
		float3 q = (p + float3(0.0, 0.0, TESR_CloudsWind.z)) / DetailTile;
		// Turbulence again, at the detail's own scale: its round Worley cells are twisted and stretched (the way curl noise
		// does it in Horizon Zero Dawn), and part of it is smooth fbm, so the cloud's surface is not a field of equal dots.
		float4 warp = tex3Dlod(TESR_NoiseVolume, float4(q * 0.125, lod));
		q += (warp.rga - 0.5) * TESR_CloudsLook.z * lerp(1.0, 0.5, Cumulus);   // (CumulusShape: half: fewer hooks and curls)
		float detail = lerp(tex3Dlod(TESR_CloudDetail, float4(q, lod)).r, warp.g, TESR_CloudsLook.w);
		// UNOFFICIAL 2026-10-14 FineDetail: the same detail texture again at a smaller size (and through the same twist), a
		// second octave, so the edges keep small wisps and curls close up instead of the one size of lump. One more read per
		// sample, only when on.
		[branch] if (TESR_CloudsDetail.x > 0.0) {
			float3 f = (p + float3(0.0, 0.0, 1.7 * TESR_CloudsWind.z)) / max(TESR_CloudsDetail.y, 1.0) + (warp.gar - 0.5) * TESR_CloudsLook.z;
			float fine = tex3Dlod(TESR_CloudDetail, float4(f, lod)).r;
			detail = lerp(detail, detail * 0.65 + fine * 0.35, TESR_CloudsDetail.x);
		}
		float wisps = lerp(detail, 1.0 - detail, saturate(hb * 6.0));
		float erosion = Erosion;
		// CumulusShape: billows all the way down (no wispy underside), and less wear near the base so it stays flat
		wisps = lerp(wisps, 1.0 - detail, Cumulus);
		erosion *= lerp(1.0, lerp(0.35, 1.0, smoothstep(0.0, 0.2, hb)), Cumulus);
		base = saturate(Remap(base, wisps * erosion, 1.0, 0.0, 1.0));
	}
	// edge sharpness: thin cloud at the rim reaches full density sooner, so the outline is crisp instead of a long fade
	// (CumulusShape: half as crisp again. Real heaps end in a clean edge; the soft rim read as smudges and wisps from below.)
	base = saturate(base * max(TESR_CloudsLook.x, 1.0) * (1.0 + 0.5 * Cumulus));
	return base * Density * lerp(0.6, 1.0, weather.b);
}

// Henyey-Greenstein phase, 1 on average over the sphere.
float Phase(float cosT, float g) {
	return (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosT, 1e-4), 1.5);
}

// ---- 1: March, quarter resolution ----
// The clouds along one world direction from the camera: rgb the light they send toward it (linear), a how much of the sky
// behind still shows. stepScale < 1 marches with fewer steps (the reflection's fallback).
float4 MarchRay(float3 dir, float jitter, float stepScale, bool fixedExit = false, bool towers = false) {
	float steps = max(Steps * stepScale, 4.0);
	// UNOFFICIAL 2026-10-25 Towering (user: "very very noisy on the edges"): the layer is up to TowerHeight times deeper with the
	// storms, so the same number of steps were up to five times longer (hundreds of metres): where a ray met a cloud's edge came out
	// different in each pixel. The storm techniques march with all 64 steps the loop allows (Steps 48: a third shorter). (A search
	// for the edge between two steps, three extra density reads, took the game's compiler past five minutes: dropped.) Unchanged
	// with Towering off.
	// (UNOFFICIAL 2026-10-27: at least 64; more where Steps says so, which only the Vulkan march can: VolumetricCloudsMarch.frag. Here Steps
	// is at most 64, so this is 64 as before.)
	if (towers) steps = max(max(Steps, 64.0) * stepScale, 4.0);
	float3 camera = TESR_CameraPosition.xyz * MetresPerUnit;
	float baseAbove = max(Base - camera.z, 50.0);
	float tStart = ShellDistance(dir.z, baseAbove);
	float tEnd = ShellDistance(dir.z, baseAbove + Thickness);
	if (dir.z < -0.15 || tStart > MaxDistance) return float4(0.0, 0.0, 0.0, 1.0);
	tEnd = min(tEnd, tStart + 4.0 * Thickness + 8000.0);      // flat rays near the horizon cross a long slab: cap it
	float dt = (tEnd - tStart) / steps;

	float3 light = normalize(TESR_SunDirection.xyz);
	float day = saturate(TESR_SunAmount.x);
	float cosT = dot(dir, light);
	// UNOFFICIAL PhysicalMoonlight (TESR_CloudsPhysical.x): the moon lights the clouds with all the light the weather gives at night,
	// as the sun does by day; off, it is cut to 15% at night as before (lerp(0.15, 1.0, day)).
	float3 sun = linearize(TESR_SunColor.rgb) * SunStrength * (TESR_CloudsPhysical.x > 0.5 ? 1.0 : lerp(0.15, 1.0, day)) * (1.0 - 0.6 * RainDark);
	float3 skyTop = linearize(TESR_SkyColor.rgb);
	float3 skyLow = linearize(TESR_HorizonColor.rgb);
	float3 ambientTop = skyTop * AmbientStrength * (1.0 - 0.4 * RainDark);
	float3 ambientBottom = lerp(skyLow, skyTop, 0.5) * AmbientStrength * 0.5 * (1.0 - 0.6 * RainDark);
#if PHYSICAL_SKY_ON
	// UNOFFICIAL physical sky. The sunlight keeps the weather's brightness but takes the colour of what is left of it after
	// crossing the air to the middle of the cloud layer (redder and dimmer the lower the sun: the clouds' own sunrise and
	// sunset). The sky light on their tops takes the colour of the physical sky straight up, on their undersides that of the
	// sky low down (from the sky's table), each at the weather's brightness. WeatherTint keeps that much of the weather's colours, as on the sky.
	[branch] if (TESR_PhysSky.x > 0.0) {
		const float share = TESR_PhysSky.x, tint = TESR_PhysSky.z, tableScale = 0.001 * TESR_PhysSky.y;
		float3 through = exp(-AtmSunDepth(AtmGround + Base + 0.5 * Thickness, TESR_PhysSkySun.z, TESR_PhysSky.w));
		float3 physSun = luma(sun) * through / max(luma(through), 1e-4);
		sun = lerp(sun, lerp(physSun, sun, tint), share);
		// UNOFFICIAL PhysicalSkyLight (TESR_CloudsPhysical.y): the sky light at the physical sky's own brightness (below); off, as before.
		[branch] if (TESR_CloudsPhysical.y > 0.5) {
			// Physical sky light: NVR's sky-view table holds the physical sky radiance per unit of sunlight above the air
			// (x1000). The clouds' sun term is the sunlight at the cloud times an un-normalised phase (4 pi x the true one),
			// so the matching sky term is that same sunlight above the air times the sky radiance integrated over the
			// hemisphere each part of the cloud faces: the open sky above for the tops, the lit ground below for the bottoms.
			// The weather's sky colours no longer set the brightness; AmbientStrength becomes a multiplier (1 = physical).
			float sunTOA = luma(sun) / max(luma(through), 1e-4);
			float3 up = 0.0;
			[unroll] for (int ub = 0; ub < 3; ub++) {
				const float bandV = ub == 0 ? 0.35 : (ub == 1 ? 0.632 : 0.882);      // sun-relative table rows at 11, 36, 70 deg
				const float bandW = ub == 2 ? 0.234 : 0.383;                          // solid-angle shares of 0-22.5, 22.5-50, 50-90 deg
				up += (tex2Dlod(TESR_SkyViewLUT, float4(0.0, bandV, 0.0, 0.0)).rgb + tex2Dlod(TESR_SkyViewLUT, float4(0.5, bandV, 0.0, 0.0)).rgb
					+ tex2Dlod(TESR_SkyViewLUT, float4(1.0, bandV, 0.0, 0.0)).rgb) * (bandW / 3.0);
			}
			up *= 0.001;                                                               // mean sky radiance per unit sunlight
			float groundAlbedo = TESR_PhysSkySun.w > 0.0 ? TESR_PhysSkySun.w : 0.3 /* ground reflectance when the physical sky sets none (desert soil) */;
			float3 sunAtGround = exp(-AtmSunDepth(AtmGround, TESR_PhysSkySun.z, TESR_PhysSky.w)) * max(TESR_PhysSkySun.z, 0.0);
			float3 groundRad = groundAlbedo / 3.14159265 * (sunAtGround + 3.14159265 * up);
			float3 physTopAbs = up * (sunTOA * 6.2831853);
			float3 physLowAbs = groundRad * (sunTOA * 6.2831853);
			float3 physTop = lerp(physTopAbs, luma(physTopAbs) * skyTop / max(luma(skyTop), 1e-6), tint);
			float3 physLow = lerp(physLowAbs, luma(physLowAbs) * skyLow / max(luma(skyLow), 1e-6), tint);
			ambientTop = lerp(ambientTop, physTop * AmbientStrength * (1.0 - 0.4 * RainDark), share);
			ambientBottom = lerp(ambientBottom, physLow * AmbientStrength * (1.0 - 0.6 * RainDark), share);
		}
		else {
			float3 zenith = tex2Dlod(TESR_SkyViewLUT, float4(0.5, 1.0, 0.0, 0.0)).rgb * tableScale;
			float3 low = (tex2Dlod(TESR_SkyViewLUT, float4(0.0, 0.35, 0.0, 0.0)).rgb + tex2Dlod(TESR_SkyViewLUT, float4(0.5, 0.35, 0.0, 0.0)).rgb +
				tex2Dlod(TESR_SkyViewLUT, float4(1.0, 0.35, 0.0, 0.0)).rgb) * (tableScale / 3.0);
			// The colours only: each at the weather's own brightness for that part of the sky, so the clouds are as bright as before.
			zenith *= luma(skyTop) / max(luma(zenith), 1e-6);
			low *= luma(skyLow) / max(luma(low), 1e-6);
			float3 physTop = lerp(zenith, skyTop, tint);
			float3 physLow = lerp(low, skyLow, tint);
			ambientTop = lerp(ambientTop, physTop * AmbientStrength * (1.0 - 0.4 * RainDark), share);
			ambientBottom = lerp(ambientBottom, lerp(physLow, physTop, 0.5) * AmbientStrength * 0.5 * (1.0 - 0.6 * RainDark), share);
		}
	}
#endif
	float lightStep = (towers ? BaseThickness : Thickness) / 12.0;   // (Towering: the ordinary clouds keep their light march)
	float lightRise = max(light.z, 0.1);                     // a low sun still climbs out of the layer

	float3 wind = float3(TESR_CloudsWind.xy, 0.0);
	float3 scattered = 0.0;
	float transmittance = 1.0;
	float depthSum = 0.0, depthWeight = 0.0;
	// UNOFFICIAL 2026-10-15: at most 64 steps (Steps). The game's own compiler (D3DX) unrolls this loop: at 255 (or 1280 in two
	// nested loops) it took minutes at startup and the game hung on a black screen.
	// UNOFFICIAL 2026-10-17 FixedMarchExit (fixedExit, technique MarchFixedExit): in the March technique the game's compiler wrote
	// this 'break' as 'break_ne -c, -c' (the same value on both sides: never taken), so every ray ran all 64 steps (16 past
	// Steps = 48, i.e. past the end of the layer or the horizon cap) and kept integrating after it was opaque. The same test as
	// the loop's condition compiles to a real exit. Changes the look slightly (flat rays stop at the cap), so it is an option.
	// UNOFFICIAL 2026-10-27 Towering: the storm techniques always take this exit (as FixedMarchExit). In the main view their steps
	// fill the loop, so what changes is that a ray stops once the cloud is opaque (what is left to see is under 1 %) instead of
	// marching, and lighting, the hundreds of metres of storm behind its surface (with Steps 256 on the Vulkan march, most of a
	// storm's cost). (The water reflection's shorter march also stops at its own steps.) The ordinary techniques: as before.
	[loop] for (float i = 0.0; i < 64.0 && (!(fixedExit || towers) || !(i >= steps || transmittance < 0.01)); i += 1.0) {
		if (!(fixedExit || towers) && (i >= steps || transmittance < 0.01)) break;
		float t = tStart + (i + jitter) * dt;
		float altitude = camera.z + t * dir.z + t * t / (2.0 * EarthRadius);
		float h = (altitude - Base) / Thickness;
		float3 p = float3(camera.xy + dir.xy * t, altitude) + wind;
		float lod = saturate(t / 40000.0) * 2.0 * (1.0 - 0.5 * saturate(Sharpness));   // far away, smoother noise: less shimmer (Sharpness: less of it)
		float density = CloudDensity(p, h, lod, false, towers);
		[branch] if (density > 0.0) {
			// light reaching this point: six samples toward the light, each twice as far as the last
			float opticalDepth = 0.0;
			float reach = 0.0;
			[branch] if (towers) {
				// (UNOFFICIAL 2026-10-25 Towering: a real loop here: six inlined copies of CloudDensity with the storms ran out of
				// ps_3_0's temp registers. The same six samples. Without towers, the unrolled loop as before: it is faster.)
				float stepLength = lightStep * 0.5;
				[loop] for (float j = 0.0; j < 6.0; j += 1.0) {
					reach += stepLength;
					float3 q = p + light * reach;
					float hq = h + reach * lightRise / Thickness;
					opticalDepth += CloudDensity(q, hq, lod + 1.0, j > 2.5, true) * stepLength;
					stepLength *= 2.0;
				}
			}
			else {
				[unroll] for (int j = 0; j < 6; j++) {
					float stepLength = lightStep * exp2((float)j) * 0.5;
					reach += stepLength;
					float3 q = p + light * reach;
					float hq = h + reach * lightRise / Thickness;
					opticalDepth += CloudDensity(q, hq, lod + 1.0, j > 2) * stepLength;
				}
			}
			// UNOFFICIAL 2026-10-25 Towering: a storm is kilometres deep, and a march sample lands hundreds of metres inside its surface:
			// the sunlight there would be gone and the storm lit by the blue sky alone (a flat blue cut-out). Light scatters on through
			// the whole storm, so from half way up the ordinary layer the sun's way in counts for less, down to a fifth two ordinary
			// layers up (bright towers; the bases stay shaded). Unchanged with Towering off.
			if (towers) opticalDepth *= 1.0 - 0.8 * Towering * saturate((h * InvTowerShare - 0.5) / 1.5);
			// several scattering orders, each weaker, less shadowed and more even (Wrenninge's approximation):
			// thick clouds stay bright inside instead of going black
			float3 sunLight = 0.0;
			float a = 1.0, b = 1.0, c = 1.0;
			[unroll] for (int k = 0; k < 3; k++) {
				float g = Forward * c;
				sunLight += b * exp(-Extinction * opticalDepth * a) * lerp(Phase(cosT, g), Phase(cosT, -0.3 * c), 0.5);
				a *= 0.25; b *= 0.5; c *= 0.5;
			}
			// powder: the edges facing the light are darker than a plain exponential says
			float powder = 1.0 - exp(-2.0 * Extinction * density * 40.0);
			float hb = towers ? h * InvTowerShare : h;   // Towering: the height in the ordinary layer (above 1 up a tower)
			float3 ambient = lerp(ambientBottom, ambientTop, saturate(hb));
			// UNOFFICIAL 2026-10-25 Towering: inside a storm most of the light in the shade is sunlight scattered through the cloud
			// itself, not the blue sky: grey shaded sides, as in photos, instead of a sky-blue cut-out. Unchanged with Towering off.
			if (towers) ambient = lerp(ambient, luma(ambient) * 1.15, 0.7 * Towering * saturate((hb - 0.5) / 1.5));
			// UNOFFICIAL 2026-10-20 CumulusShape: the bottom of a heap gets less of the light scattered down to it from the bright
			// cloud above (Schneider's height term in Nubis): grey bases under white tops, as in the photos, instead of grey patches
			// in the middle of an evenly white slab.
			float lowLight = lerp(1.0, lerp(0.75, 1.0, smoothstep(0.0, 0.2, hb)), Cumulus);
			float3 radiance = (sun * sunLight * lerp(0.5, 1.0, powder) + ambient) * lowLight;
			float sigma = Extinction * density;
			float stepTransmittance = exp(-sigma * dt);
			scattered += transmittance * radiance * (1.0 - stepTransmittance);   // energy-conserving step (Hillaire)
			depthSum += t * transmittance * (1.0 - stepTransmittance);
			depthWeight += transmittance * (1.0 - stepTransmittance);
			transmittance *= stepTransmittance;
		}
	}
	// far clouds thin out into the sky (the sky's own colour is the haze): they hold near and fade quickly far off,
	// before the horizon squeezes them into slivers thinner than the march's texels
	// UNOFFICIAL 2026-10-25 WeatherTint: the colour the weather gives its own cloud layers at this hour (orange at sunset, grey
	// overcast, blue at night), over the light from the sun and sky; brightness unchanged (the tint's own is 1).
	scattered *= lerp(1.0, TESR_CloudsTint.rgb, TESR_CloudsTint.w);
	float distance = depthWeight > 0.0 ? depthSum / depthWeight : tStart;
	// UNOFFICIAL 2026-10-25 Towering: the haze is low air: the high tops of towers far off show through less of it (as the storm
	// on the horizon in a photo), so from half way up the ordinary layer the distance counts for less (down to a quarter of it, a third of the way up the tall layer). Unchanged with Towering off.
	if (towers) {
		float hitAltitude = camera.z + distance * dir.z + distance * distance / (2.0 * EarthRadius);
		distance *= 1.0 - 0.75 * Towering * saturate(3.0 * (hitAltitude - Base - 0.5 * BaseThickness) / max(Thickness - BaseThickness, 1.0));
	}
	float haze = exp(-(distance * distance) / (HazeDistance * HazeDistance)) * saturate((MaxDistance - distance) / (0.4 * MaxDistance));
	scattered *= haze;
	transmittance = lerp(1.0, transmittance, haze);
	return float4(scattered, transmittance);
}

float4 MarchPS(VSOUT IN, float2 vpos : VPOS, uniform bool fixedExit, uniform bool towers) : COLOR0 {
	float2 pixel = vpos * 2.0 + TESR_CloudsBlocks.zw;          // this frame's quarter-resolution pixel of the block
	float2 uv = (pixel + 0.5) * LowTexel;
	// UNOFFICIAL 2026-10-27 Towering (user: "really big clouds being noisy even at higher resolutions"): a storm's edges are lumps of
	// dense cloud a few tens of metres across, the size of a march step, so where a pixel's samples fall decides what it shows (grain
	// that halves with each doubling of Steps). The history averages it over a pixel's last few marches, but each pixel is marched
	// only every fourth frame, so its offset moved on 4 x 0.618 = 2.47 of a step each time: two clumps of offsets, about half a step
	// apart, and the average of the last few had large gaps. The storm techniques move each pixel's offset on by the golden ratio
	// once per march (per four frames), which spreads any run of them evenly over the step: about a third less grain left after the
	// history (the same picture on average), at no cost. The ordinary techniques: as before.
	float frameStep = towers ? floor(TESR_CloudsMarch.z * 0.25) : TESR_CloudsMarch.z;
	float jitter = frac(tex2Dlod(TESR_BlueNoiseSampler, float4(pixel / 256.0, 0, 0)).r + frameStep * 0.61803398875);
	return MarchRay(normalize(toWorld(uv)), jitter, 1.0, fixedExit, towers);
}

// Catmull-Rom upsampling of the quarter-resolution history in nine bilinear taps (the sharp bicubic: plain bilinear
// smears each cloud texel over 4 x 4 screen pixels). It can overshoot a little at hard edges: clamped.
float4 CatmullRom(float2 uv) {
	float2 size = 1.0 / LowTexel;
	float2 position = uv * size;
	float2 centre = floor(position - 0.5) + 0.5;
	float2 f = position - centre;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 offset12 = w2 / w12;
	float2 t0 = (centre - 1.0) * LowTexel, t3 = (centre + 2.0) * LowTexel, t12 = (centre + offset12) * LowTexel;
	float4 result = 0.0;
	result += tex2Dlod(TESR_CloudHistory, float4(t0.x, t0.y, 0, 0)) * w0.x * w0.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t12.x, t0.y, 0, 0)) * w12.x * w0.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t3.x, t0.y, 0, 0)) * w3.x * w0.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t0.x, t12.y, 0, 0)) * w0.x * w12.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t12.x, t12.y, 0, 0)) * w12.x * w12.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t3.x, t12.y, 0, 0)) * w3.x * w12.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t0.x, t3.y, 0, 0)) * w0.x * w3.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t12.x, t3.y, 0, 0)) * w12.x * w3.y;
	result += tex2Dlod(TESR_CloudHistory, float4(t3.x, t3.y, 0, 0)) * w3.x * w3.y;
	return float4(max(result.rgb, 0.0), saturate(result.a));
}

// UNOFFICIAL 2026-10-15 Sharpness: the history read back sharp (Catmull-Rom) instead of bilinear, which blurs it a little more
// every frame the view or the clouds move, then held within the four texels around the point (no ringing that builds up).
float4 SharpHistory(float2 uv) {
	float2 size = 1.0 / LowTexel;
	float2 corner = (floor(uv * size - 0.5) + 0.5) * LowTexel;
	float4 a = tex2Dlod(TESR_CloudHistory, float4(corner, 0, 0));
	float4 b = tex2Dlod(TESR_CloudHistory, float4(corner + float2(LowTexel.x, 0.0), 0, 0));
	float4 c = tex2Dlod(TESR_CloudHistory, float4(corner + float2(0.0, LowTexel.y), 0, 0));
	float4 d = tex2Dlod(TESR_CloudHistory, float4(corner + LowTexel, 0, 0));
	return clamp(CatmullRom(uv), min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}

// ---- 2: Temporal, quarter resolution ----
float4 TemporalPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;
	float2 block = floor(vpos * 0.5);
	float2 blockUV = (block + 0.5) * TESR_CloudsBlocks.xy;
	float4 current = tex2Dlod(TESR_CloudCurrent, float4(blockUV, 0, 0));
	float4 lo = current, hi = current;
	[unroll] for (int y = -1; y <= 1; y++) {
		[unroll] for (int x = -1; x <= 1; x++) {
			float4 n = tex2Dlod(TESR_CloudCurrent, float4(blockUV + float2(x, y) * TESR_CloudsBlocks.xy, 0, 0));
			lo = min(lo, n); hi = max(hi, n);
		}
	}
	bool fresh = all(vpos - block * 2.0 == TESR_CloudsBlocks.zw);
	// the same direction last frame: the clouds are kilometres away, so only the camera's turn moves them on screen
	float3 dir = normalize(toWorld(uv));
	float3 previousDir = dir;
	[branch] if (Sharpness > 0.0) {
		// UNOFFICIAL 2026-10-15: and the wind. The cloud seen here was last frame where the pattern's move puts it back (taken at
		// a typical depth into the layer): without this, each pixel kept last frame's cloud until its turn to be marched came
		// round (one frame in four), so drifting clouds crept in small steps and trailed.
		float3 camera = TESR_CameraPosition.xyz * MetresPerUnit;
		float reach = ShellDistance(dir.z, max(Base + 0.4 * BaseThickness - camera.z, 50.0));
		previousDir = dir * reach + float3(TESR_CloudsMotion.xy, 0.0);
	}
	float4 clip = mul(float4(previousDir, 0.0), TESR_CloudsPrevViewProj);
	float weight = TESR_CloudsLayout.z;
	[branch] if (clip.w <= 0.0 || weight <= 0.0) return current;
	// UNOFFICIAL 2026-10-27 RenderScale v2: the projection gives a screen position, the history holds it at its rect uv (the CPU
	// starts the history over when the scale changes, so last frame's part is this frame's)
	float2 previous = screenToRect(clip.xy / clip.w * float2(0.5, -0.5) + 0.5);
	if (outsideRect(previous)) return current;
	float4 history = clamp(Sharpness > 0.0 ? SharpHistory(previous) : tex2Dlod(TESR_CloudHistory, float4(previous, 0, 0)), lo, hi);
	return fresh ? lerp(current, history, weight) : history;
}

// Stable (2026-10-31): every quarter-resolution pixel is marched every frame (fewer steps, early exit) instead
// of one in four, so no pixel shows a result up to four frames old. The history is clipped to this frame's
// neighbourhood mean +- spread (variance clipping) instead of its min/max, which jumps with the march noise.
// The CPU sets the per-frame history weight from the frame time, so the smoothing is the same at any frame rate.
float4 MarchFullPS(VSOUT IN, float2 vpos : VPOS, uniform bool towers) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;
	float jitter = frac(tex2Dlod(TESR_BlueNoiseSampler, float4(vpos / 256.0, 0, 0)).r + TESR_CloudsMarch.z * 0.61803398875);
	return MarchRay(normalize(toWorld(uv)), jitter, 0.5, true, towers);
}

float4 TemporalStablePS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;
	float4 current = tex2Dlod(TESR_CloudCurrentFull, float4(uv, 0, 0));
	float4 sum = 0.0, sumSq = 0.0;
	[unroll] for (int y = -1; y <= 1; y++) {
		[unroll] for (int x = -1; x <= 1; x++) {
			float4 n = tex2Dlod(TESR_CloudCurrentFull, float4(uv + float2(x, y) * LowTexel, 0, 0));
			sum += n; sumSq += n * n;
		}
	}
	float4 mean = sum / 9.0;
	float4 spread = sqrt(max(sumSq / 9.0 - mean * mean, 0.0)) * 1.25;
	float4 lo = min(mean - spread, current), hi = max(mean + spread, current);
	float3 dir = normalize(toWorld(uv));
	float3 previousDir = dir;
	[branch] if (Sharpness > 0.0) {
		float3 camera = TESR_CameraPosition.xyz * MetresPerUnit;
		float reach = ShellDistance(dir.z, max(Base + 0.4 * BaseThickness - camera.z, 50.0));
		previousDir = dir * reach + float3(TESR_CloudsMotion.xy, 0.0);
	}
	float4 clip = mul(float4(previousDir, 0.0), TESR_CloudsPrevViewProj);
	float weight = TESR_CloudsLayout.z;
	[branch] if (clip.w <= 0.0 || weight <= 0.0) return current;
	float2 previous = screenToRect(clip.xy / clip.w * float2(0.5, -0.5) + 0.5);
	if (outsideRect(previous)) return current;
	float4 history = clamp(Sharpness > 0.0 ? SharpHistory(previous) : tex2Dlod(TESR_CloudHistory, float4(previous, 0, 0)), lo, hi);
	return lerp(current, history, weight);
}

// ---- UNOFFICIAL 2026-10-12: cloud shadows and the clouds on water ----

static const float ShadowStrength = TESR_CloudsExtra.x;
static const float ShadowRange = max(TESR_CloudsExtra.y, 1000.0);
static const float ShadowMapSize = max(TESR_CloudsDetail.z, 256.0);   // UNOFFICIAL: ShadowResolution (was always 256)

// The shadow map's centre (metres, on the cloud base): the camera, snapped to whole texels.
float2 ShadowCentre() {
	float texel = ShadowRange / ShadowMapSize;
	return floor(TESR_CameraPosition.xy * MetresPerUnit / texel) * texel;
}

// The light's direction for the shadows: the sun's (or the moon's), never flatter than 0.1 so it still crosses the layer.
float3 ShadowLight() {
	float3 light = normalize(TESR_SunDirection.xyz);
	light.z = max(light.z, 0.25);
	return normalize(light);
}

// ---- 3: ShadowMap ----
float4 ShadowMapPS(VSOUT IN, float2 vpos : VPOS, uniform bool towers) : COLOR0 {
	float2 at = ShadowCentre() + ((vpos + 0.5) / ShadowMapSize - 0.5) * ShadowRange;
	float3 light = ShadowLight();
	float length = Thickness / light.z;
	float stepLength = length / 8.0;
	float3 wind = float3(TESR_CloudsWind.xy, 0.0);
	float opticalDepth = 0.0;
	[branch] if (towers) {
		// UNOFFICIAL 2026-10-25 Towering: a real loop here (unrolled, eight copies of CloudDensity with the storms are past ps_3_0's
		// 224 constant registers). The same samples.
		[loop] for (float i = 0.5; i < 8.0; i += 1.0) {
			float3 p = float3(at, Base) + light * (i * stepLength);
			opticalDepth += CloudDensity(p + wind, (p.z - Base) / Thickness, 1.0, false, true) * stepLength;
		}
	}
	else {
		[unroll] for (int i = 0; i < 8; i++) {
			float s = (i + 0.5) * stepLength;
			float3 p = float3(at, Base) + light * s;
			opticalDepth += CloudDensity(p + wind, (p.z - Base) / Thickness, 1.0, false) * stepLength;   // the full shape, as the clouds seen
		}
	}
	return float4(exp(-Extinction * opticalDepth), 0.0, 0.0, 1.0);
}

// ---- 11: ShadowMapPrefilter ----
// UNOFFICIAL 2026-10-27 ShadowPrefilter (TESR_CloudsTower.w: ShadowSoftness in texels, 0 off): one point sample per ~120 m
// texel of the full shape (with its fine edge detail) aliases into speckles on faces and ground. Instead: the smooth shape
// only (cheap, no detail), averaged over four taps spread round the texel (a rotated grid), so the map is low-pass.
// Technique 11, only with ShadowPrefilter on (a branch in ShadowMapPS ran out of ps_3_0's bool registers); off = technique 3 as before.
float4 ShadowMapPrefilterPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 at = ShadowCentre() + ((vpos + 0.5) / ShadowMapSize - 0.5) * ShadowRange;
	float3 light = ShadowLight();
	float stepLength = Thickness / light.z / 8.0;
	float3 wind = float3(TESR_CloudsWind.xy, 0.0);
	float opticalDepth = 0.0;
	float spread = TESR_CloudsTower.w * ShadowRange / ShadowMapSize;
	[loop] for (int t = 0; t < 4; t++) {
		float2 o = t == 0 ? float2(-0.125, -0.375) : t == 1 ? float2(0.375, -0.125) : t == 2 ? float2(-0.375, 0.125) : float2(0.125, 0.375);
		float2 tap = at + o * spread;
		[loop] for (float i = 0.5; i < 8.0; i += 1.0) {
			float3 p = float3(tap, Base) + light * (i * stepLength);
			opticalDepth += CloudDensity(p + wind, (p.z - Base) / Thickness, 1.0, true) * stepLength;   // the smooth shape (no storms: the map stays soft)
		}
	}
	return float4(exp(-Extinction * opticalDepth * 0.25), 0.0, 0.0, 1.0);
}

// How much sunlight the clouds let through to a world point (metres): the shadow map where the light from there meets
// the cloud base, 1 outside the map (faded in over its last tenth).
float CloudShadowAt(float3 world) {
	float3 light = ShadowLight();
	float2 q = world.xy + light.xy * ((Base - world.z) / light.z);
	float2 suv = (q - ShadowCentre()) / ShadowRange + 0.5;
	float2 edge = min(suv, 1.0 - suv);
	float inside = saturate(min(edge.x, edge.y) * 10.0);
	return lerp(1.0, tex2Dlod(TESR_CloudShadow, float4(suv, 0, 0)).r, inside);
}

// ---- 0: Composite, full resolution ----
// The clouds of this pixel from the history: rgb the light they send, a how much of the sky shows through.
float4 CloudsHere(float2 huv) {
	// UNOFFICIAL 2026-10-15 Resolution 3-5 (larger than the screen): each pixel averages the history texels it covers (four
	// bilinear reads a quarter of a pixel out); Catmull-Rom would read only the few nearest.
	float4 clouds;
	[branch] if (LowTexel.x < TESR_ReciprocalResolution.x * 0.99) {
		float2 q = 0.25 * TESR_ReciprocalResolution.xy * TESR_RenderScale.zw;	// a quarter of this pixel (larger in a RenderScale frame)
		clouds = 0.25 * (tex2Dlod(TESR_CloudHistory, float4(huv + float2(-q.x, -q.y), 0, 0)) + tex2Dlod(TESR_CloudHistory, float4(huv + float2(q.x, -q.y), 0, 0)) +
			tex2Dlod(TESR_CloudHistory, float4(huv + float2(-q.x, q.y), 0, 0)) + tex2Dlod(TESR_CloudHistory, float4(huv + q, 0, 0)));
	}
	else clouds = CatmullRom(huv);
	[branch] if (Sharpness > 0.0) {
		// UNOFFICIAL 2026-10-15 Sharpness: contrast-adaptive sharpening of the clouds against their four neighbours a march texel
		// away, held within what those show (crisper edges and inner detail, no dark or bright halos).
		float4 n0 = tex2Dlod(TESR_CloudHistory, float4(huv + float2(LowTexel.x, 0.0), 0, 0));
		float4 n1 = tex2Dlod(TESR_CloudHistory, float4(huv - float2(LowTexel.x, 0.0), 0, 0));
		float4 n2 = tex2Dlod(TESR_CloudHistory, float4(huv + float2(0.0, LowTexel.y), 0, 0));
		float4 n3 = tex2Dlod(TESR_CloudHistory, float4(huv - float2(0.0, LowTexel.y), 0, 0));
		float4 lo = min(min(min(n0, n1), min(n2, n3)), clouds), hi = max(max(max(n0, n1), max(n2, n3)), clouds);
		float4 sharpened = clouds + (clouds - 0.25 * (n0 + n1 + n2 + n3)) * (1.5 * Sharpness);
		clouds = clamp(sharpened, lo, hi);
	}
	return clouds;
}

float4 CompositePS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	// UNOFFICIAL 2026-10-27: the clouds' history is drawn whole in every frame (RenderClouds runs the march as in a native frame), so in
	// a RenderScale frame it is read at the screen position of this (lower resolution) pixel. At native: the same uv.
	// 2026-10-28: + the frame's jitter in an FSR / DLSS LowResEffects frame (VolumetricClouds.cpp), so the upscaler sees the clouds jitter
	// with the rest of the picture instead of wobbling by it.
	float2 huv = rectToScreen(uv) + TESR_CloudsHistoryScale.zw;
	float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0));
	float depth = readDepthLod(uv);
	// UNOFFICIAL 2026-11-05 (Balebandro) the game's MSAA (Includes/MsaaSkyEdge.hlsl): a pixel on an edge against the sky took no clouds, and its sky
	// share kept the clear sky (a bright rim round every leaf and branch). It gets the clouds over that share.
	[branch] if (depth < farZ * 0.996) {
		float3 sky;
		float share = SkyShare(uv, color.rgb, sky);
		[branch] if (share > 0.0) {
			float4 clouds = CloudsHere(huv);
			float3 lin = linearize(max(color.rgb, 0.0)) + share * (sky * (clouds.a - 1.0) + clouds.rgb * TESR_CloudsLayout.w);
			return float4(delinearize(max(lin, 0.0)), color.a);
		}
	}
	[branch] if (depth < farZ * 0.996) {
		// not sky: the clouds' shadow, and their reflection on water
		[branch] if (ShadowStrength <= 0.0) return color;
		float3 eye = toWorld(uv);                                  // camera-relative world per unit of depth
		float3 world = (TESR_CameraPosition.xyz + eye * depth) * MetresPerUnit;
		float3 normal = mul((float3x3)TESR_ViewTransform, normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0, 0)).xyz * 2.0 - 1.0));
		float height = TESR_CameraPosition.z + eye.z * depth;
		bool water = normal.z > 0.7 && abs(height - TESR_WaterSettings.x) < max(2.0, depth * 0.002);
		// Water is not shaded: what it shows is the reflection of things far off, not sunlight on its surface (2026-10-12: at
		// dawn the whole lake went dark and its reflections looked cut out).
		[branch] if (ShadowStrength > 0.0 && !water) {
			// Only the sunlight is shaded: surfaces turned away from the sun (and what they show, lit signs on a shaded wall)
			// keep their light. In the open ground and roofs take the full strength.
			float3 sunDir = normalize(TESR_SunDirection.xyz);
			float day = saturate(TESR_SunAmount.x);
			// UNOFFICIAL 2026-10-21: only the surfaces turned (nearly) away from the sun are let off. The normal here is rebuilt from
			// depth (flat per triangle, striped by the depth's precision): weighting every surface by it (the old square root of N.L)
			// drew triangles and stripes on rocks. (With SunFar running, the shadow is put on the land there instead.)
			float facing = smoothstep(0.0, 0.25, dot(normal, sunDir));
			float strength = ShadowStrength * lerp(0.3, 1.0, day) * smoothstep(0.08, 0.35, sunDir.z) * facing;   // fades with a low sun
			float shadow = CloudShadowAt(world);
			[branch] if (TESR_CloudsExtra.w > 0.0) return float4(shadow.xxx * lerp(0.4, 1.0, facing), 1.0);   // ShadowDebug: white lit, black shaded
			// UNOFFICIAL 2026-10-13: the colour's trip into linear light and back only where the shadow changes it. With no
			// cloud overhead, or a surface turned from the sun (strength 0), the factor is exactly 1: the pixel is left as it
			// was (most of the screen under a clear sky; the round trip cost two pows per channel per pixel for nothing).
			float factor = lerp(1.0, shadow, strength);
			[branch] if (factor >= 1.0) return color;
			return float4(delinearize(max(linearize(max(color.rgb, 0.0)) * factor, 0.0)), color.a);
		}
		return color;
	}
	float4 clouds = CloudsHere(huv);
	float3 lin = linearize(max(color.rgb, 0.0)) * clouds.a + clouds.rgb * TESR_CloudsLayout.w;
	return float4(delinearize(max(lin, 0.0)), color.a);
}

// ---- 4: Reflection (UNOFFICIAL 2026-10-12): the clouds drawn into the game's water reflection ----
// Right after the game's reflection pass (RenderReflectionsHook), into its own target with its own depth buffer: a quad at the
// far plane with the depth test on, so only what is still sky there (nothing drawn in front) takes the clouds. Each texel's
// direction comes from the reflection camera; the clouds along it from the history where the main view saw that direction
// last frame (the same clouds as on screen), else a shorter march. Blended over the reflection's sky by the clouds'
// coverage, in the reflection's own (display) encoding, so the water shader waves and tints them like the rest.
VSOUT ReflectionVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = float4(IN.vertPos.xy, TESR_CloudsReflectData.w, 1.0);
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 ReflectionPS(VSOUT IN, float2 vpos : VPOS, uniform bool towers) : COLOR0 {
	float2 ndc = float2((vpos.x + 0.5) / TESR_CloudsReflectData.y * 2.0 - 1.0, 1.0 - (vpos.y + 0.5) / TESR_CloudsReflectData.z * 2.0);
	float4 far = mul(float4(ndc, 0.5, 1.0), TESR_CloudsReflectInvVP);
	float3 dir = normalize(far.xyz / far.w);
	float4 clouds = float4(0.0, 0.0, 0.0, 1.0);
	// ReflectionDebug (TESR_CloudsExtra.z): red where the reflection looks up (sky), blue where down, full cover
	[branch] if (TESR_CloudsExtra.z > 0.0) return dir.z > 0.0 ? float4(1.0, 0.0, 0.0, 0.0) : float4(0.0, 0.0, 1.0, 0.0);
	float4 clip = mul(float4(dir, 0.0), TESR_CloudsHistoryViewProj);
	float2 at = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
	float2 edge = min(at, 1.0 - at);
	float seen = clip.w > 0.0 ? saturate(min(edge.x, edge.y) * 20.0) : 0.0;
	[branch] if (seen > 0.0) clouds = tex2Dlod(TESR_CloudHistory, float4(saturate(at) * TESR_CloudsHistoryScale.xy, 0, 0));	// (RenderScale v2: the history's drawn part)
	[branch] if (seen < 1.0) {
		float jitter = frac(tex2Dlod(TESR_BlueNoiseSampler, float4(vpos / 256.0, 0, 0)).r + TESR_CloudsMarch.z * 0.61803398875);
		clouds = lerp(MarchRay(dir, jitter, 0.5, false, towers), clouds, seen);
	}
	float strength = saturate(TESR_CloudsReflectData.x);
	float transmittance = lerp(1.0, saturate(clouds.a), strength);
	float coverage = 1.0 - pow(transmittance, 0.4545);          // the clouds' share in the display encoding
	float3 own = clouds.rgb * strength * TESR_CloudsLayout.w / max(1.0 - transmittance, 1e-3);
	return float4(delinearize(max(own, 0.0)) * coverage, 1.0 - coverage);   // blended ONE, SRCALPHA: own over the sky
}

// ---- 5: LayerProbe (UNOFFICIAL 2026-10-15), into a 1 x 1 target: how much of a game cloud texture is cloud ----
// The mean alpha of the bound layer texture, from 16 x 16 reads at the mip about 16 texels across (TESR_CloudsMotion.z). The
// game's textures sit in video memory, where the CPU cannot read them: this pixel is read back once per texture.
float4 LayerProbePS(VSOUT IN) : COLOR0 {
	float sum = 0.0;
	[loop] for (float y = 0.5; y < 16.0; y += 1.0) {
		[loop] for (float x = 0.5; x < 16.0; x += 1.0) sum += tex2Dlod(NVR_CloudLayerProbe, float4(x / 16.0, y / 16.0, 0.0, TESR_CloudsMotion.z)).a;
	}
	return (sum / 256.0).xxxx;
}

technique Composite {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositePS();
	}
}

technique March {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchPS(false, false);
		Texture[6] = <NoTexture>;
	}
}

technique Temporal {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 TemporalPS();
	}
}

technique Reflection {
	pass {
		VertexShader = compile vs_3_0 ReflectionVS();
		PixelShader = compile ps_3_0 ReflectionPS(false);
	}
}

technique ShadowMap {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ShadowMapPS(false);
		Texture[10] = <NoTexture>;   // writes the shadow map
	}
}

technique LayerProbe {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 LayerProbePS();
	}
}

// UNOFFICIAL 2026-10-17 FixedMarchExit ([Shaders.VolumetricClouds.Main] FixedMarchExit): technique 6 = March with the working exit.
technique MarchFixedExit {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchPS(true, false);
		Texture[6] = <NoTexture>;
	}
}

// UNOFFICIAL 2026-10-25 Towering ([Shaders.VolumetricClouds.Main] Towering above 0): techniques 7-10 = March, MarchFixedExit,
// Reflection and ShadowMap with the storms and the taller heaps. Without Towering the effect uses 1, 6, 3 and 4 as before.
technique MarchTowers {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchPS(false, true);
		Texture[6] = <NoTexture>;
	}
}

technique MarchTowersFixedExit {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchPS(true, true);
		Texture[6] = <NoTexture>;
	}
}

technique ReflectionTowers {
	pass {
		VertexShader = compile vs_3_0 ReflectionVS();
		PixelShader = compile ps_3_0 ReflectionPS(true);
	}
}

technique ShadowMapTowers {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ShadowMapPS(true);
		Texture[10] = <NoTexture>;   // writes the shadow map
	}
}

technique ShadowMapPrefilter {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 ShadowMapPrefilterPS();
		Texture[10] = <NoTexture>;   // writes the shadow map
	}
}

technique MarchFull {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchFullPS(false);
#if PHYSICAL_SKY_ON
		Texture[12] = <NoTexture>;
#else
		Texture[11] = <NoTexture>;
#endif
	}
}

technique MarchFullTowers {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchFullPS(true);
#if PHYSICAL_SKY_ON
		Texture[12] = <NoTexture>;
#else
		Texture[11] = <NoTexture>;
#endif
	}
}

technique TemporalStable {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 TemporalStablePS();
	}
}
