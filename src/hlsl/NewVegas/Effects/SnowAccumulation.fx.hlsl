// Snow build-up on upward-facing surfaces: a coverage mask from the ortho height map, then the snow look (diffuse, specular, sparkle and point light terms) blended over the scene.

uniform float4x4 TESR_WorldViewProjectionTransform;
uniform float4x4 TESR_ShadowCameraToLightTransformOrtho;
uniform float4 TESR_ReciprocalResolution;
uniform float4 TESR_SunDirection;
uniform float4 TESR_SunColor;
uniform float4 TESR_SunAmbient;
uniform float4 TESR_SnowAccumulationParams; // x:BlurNormDropThreshhold, y:BlurRadiusMultiplier, z:SunPower, w:SnowAmount 
uniform float4 TESR_SnowAccumulationColor;
uniform float4 TESR_WaterSettings;
uniform float4 TESR_ShadowFade;
uniform float4 TESR_ShadowData;
uniform float4 TESR_ShadowScreenSpaceData;
uniform float4 TESR_OrthoData;

uniform float4 TESR_SnowAccumulationExtra;	// UNOFFICIAL 2026-10-28: x 1 = CapturedNormals, y CapturedNormalsDebugView, zw this frame's jitter (screen units; see SnowRay)
uniform float4 TESR_GameTime;	// (Look 1)
uniform float4 TESR_SnowData;	// (Look 1) x: active snowfall amount (0 = not snowing, 1 = full snow)

uniform float4 TESR_ShadowLightPosition[12];
uniform float4 TESR_LightPosition[12];
uniform float4 TESR_LightColor[24];

uniform sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_SourceBuffer : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_DepthBuffer : register(s2) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_OrthoMapBuffer : register(s3) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_NormalsBuffer : register(s4) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_PointShadowBuffer : register(s5) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_SnowNormSampler : register(s6) < string ResourceName = "Precipitations\snow_NRM.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
uniform sampler2D TESR_BlueNoiseSampler : register(s7) < string ResourceName = "Effects\bluenoise256.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-28 Look 1 (macintroll's version 5, below): the first-person view's depth (his flake impacts on the weapon).
uniform sampler2D TESR_DepthBufferViewModel : register(s8) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-27 Persistent ([Shaders.SnowAccumulation.Main] Persistent, CoverMap.h): snow depth kept per spot (samplers bind in
// declaration order; 2026-10-28 s8 -> s9). Only the persistent techniques read it.
uniform sampler2D TESR_CoverMapBuffer : register(s9) = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-28 Look 1's blue noise (macintroll's Snownoise256.dds, four independent channels).
uniform sampler2D TESR_SnowNoiseSampler : register(s10) < string ResourceName = "Effects\Snownoise256.dds"; > = sampler_state {
    ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): the surfaces' own smooth normals, point sampled.
uniform sampler2D TESR_CapturedNormalsBuffer : register(s11) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#include "includes/CoverMap.hlsl"

static const int KernelSize = 24;
static const float diffusePower = 0.56f * TESR_SnowAccumulationParams.z;
static const float specularPower = 1.0f * TESR_SnowAccumulationParams.z;
static const float fresnelPower = 0.65f * TESR_SnowAccumulationParams.z;
static const float DARKNESS = 1-TESR_ShadowData.y;
static const float orthoRadius = TESR_SnowAccumulationParams.x; // radius of the area to sample ortho from
static const float noiseSize = 200; // scale for the noise used for snow coverage
static const float useShadows = TESR_ShadowScreenSpaceData.x || TESR_ShadowFade.y;

struct VSOUT { float4 clipPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 clipPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
	VSOUT outV = (VSOUT)0.0f;
	outV.clipPos = px.clipPos;
	outV.UVCoord = px.UVCoord;
	return outV;
}

#include "includes/Helpers.hlsl"
#include "includes/Depth.hlsl"
#include "includes/Normals.hlsl"
#include "includes/BlurDepth.hlsl"
#include "includes/CapturedNormals.hlsl"

// UNOFFICIAL 2026-10-28 (user: "taa jitter is causing the same flickering we had with clouds with snow on the ground"): with FSR / DLSS
// LowResEffects this pass draws into the small JITTERED frame before the upscale, where the depth at a pixel is the jittered sample's,
// but toWorld / reconstructWorldPosition build the view ray with the unjittered camera: every world position (the coverage noise, the
// slope test, the snow normal map, the footprints, the sparkles) was off by the jitter, differently each frame, so the snow's pattern
// shimmered against the ground under it. As VolumetricClouds' composite (2026-10-28): a pixel of the jittered frame shows the
// unjittered screen at + TESR_SnowAccumulationExtra.zw (TAAEffect::LowResJitterUV, set per frame; 0 in any other frame).
float3 SnowRay(float2 uv) { return toWorld(uv + TESR_SnowAccumulationExtra.zw * TESR_RenderScale.xy); }
float4 SnowWorldPosition(float2 uv, out float view_depth) {
	float2 tmp_s01 = rectToScreen(uv) + TESR_SnowAccumulationExtra.zw;
	float4 clip_space = float4(tmp_s01.x * 2 - 1, (1 - tmp_s01.y) * 2 - 1, projectedDepthFromLinear(tex2D(TESR_DepthBuffer, uv).x), 1.0f);
	float4 view_space = mul(clip_space, TESR_InvProjectionTransform);
	view_space /= view_space.w;
	view_depth = view_space.z;
	return float4(mul(view_space, TESR_InvViewTransform).xyz, 1.0f);
}

// UNOFFICIAL 2026-10-28 CapturedNormals ([Shaders.SnowAccumulation.Main] CapturedNormals; docs/next/CAPTURED-NORMALS.md): macintroll's
// proposal. The snow's slope (where it may lie) and its lighting took the normal rebuilt from depth, one flat normal per triangle, so on
// rocks the snow came and went triangle by triangle. Where the surface wrote its smooth vertex normal while it drew (and the depth agrees),
// that normal instead; elsewhere the depth-rebuilt one, as before. distance: the pixel's camera distance (length of its camera vector).
bool SnowCapturedNormal(float2 uv, float flat_dist, float3 rebuilt_v, out float3 sn_n)
{
	sn_n = float3(0.0f, 0.0f, 1.0f);
	[branch] if (TESR_SnowAccumulationExtra.x <= 0.0f) return false;
	return CapturedNormalAgrees(uv, flat_dist, rebuilt_v, sn_n);
}

// CapturedNormalsDebugView: 1 the normal the snow takes, bright where captured, dimmed where it falls back to the depth-rebuilt one;
// 2 how far the two differ (white = 90 degrees or more; red: written but too far off, so not taken), black where nothing was captured.
float4 SnowCapturedView(float2 uv, float flat_dist)
{
	float3 rebuilt_v = GetWorldNormal(uv);
	float3 sn_n;
	bool captured_v = CapturedNormal(uv, flat_dist, sn_n);
	if (TESR_SnowAccumulationExtra.y > 1.5f) return captured_v && dot(sn_n, rebuilt_v) <= TESR_CapturedNormalsData.w ? float4(1.0f, 0.0f, 0.0f, 1.0f) : float4((captured_v ? saturate(1.0f - dot(sn_n, rebuilt_v)) : 0.0f).xxx, 1.0f);
	captured_v = captured_v && dot(sn_n, rebuilt_v) > TESR_CapturedNormalsData.w;
	return float4(captured_v ? sn_n * 0.5f + 0.5f : (rebuilt_v * 0.5f + 0.5f) * 0.3f, 1.0f);
}

float GetOrtho(float4 sn_wpos){
    float sn_thick = 0.002f; // thickness of the valid areas around the ortho map depth that will receive the effect (cancels out too far above or below ortho value)

	// get puddle mask from ortho map
	float4 sn_ort_p = mul(sn_wpos, TESR_ShadowCameraToLightTransformOrtho);

	// apply perspective (perspective division) and convert from -1/1 to range to 0/1 (shadowMap range);
	sn_ort_p.xyz /= sn_ort_p.w;
	sn_ort_p.x = sn_ort_p.x * 0.5f + 0.5f;
	sn_ort_p.y = sn_ort_p.y * -0.5f + 0.5f;

	float sn_out = (saturate(sn_ort_p.x) != sn_ort_p.x) || (saturate(sn_ort_p.y) != sn_ort_p.y);

	float sn_ort = tex2D(TESR_OrthoMapBuffer, sn_ort_p.xy).r; // ortho height

	float sn_above = sn_ort_p.z < sn_ort + sn_thick;
	float sn_below = sn_ort_p.z > sn_ort - sn_thick;

	return sn_out || (sn_below && sn_above);
}

// 3D value noise: a random value in [0, 1) at each whole-number lattice point, blended between the 8 corners of the cell
// with a smoothstep curve. Output in [0, 1), mean 0.5, one lattice cell per unit. Written for Vegas Shaders (2026-11-15);
// it replaces upstream's copy of a Shadertoy noise under CC BY-NC-SA 3.0 (not GPL-compatible), with the same range and scale.
// Lattice hash: Dave Hoskins' "Hash without Sine" hash13, MIT, Copyright (c) 2014 David Hoskins (shadertoy.com/view/4djSRW).
float snHash(float3 sn_p3)
{
    sn_p3 = frac(sn_p3 * 0.1031);
    sn_p3 += dot(sn_p3, sn_p3.zyx + 31.32);
    return frac((sn_p3.x + sn_p3.y) * sn_p3.z);
}

float snNoise(float3 x)
{
    float3 sn_i = floor(x);
    float3 sn_u = frac(x);
    sn_u = sn_u * sn_u * (3.0 - 2.0 * sn_u);
    float sn_z0 = lerp(lerp(snHash(sn_i), snHash(sn_i + float3(1, 0, 0)), sn_u.x),
                       lerp(snHash(sn_i + float3(0, 1, 0)), snHash(sn_i + float3(1, 1, 0)), sn_u.x), sn_u.y);
    float sn_z1 = lerp(lerp(snHash(sn_i + float3(0, 0, 1)), snHash(sn_i + float3(1, 0, 1)), sn_u.x),
                       lerp(snHash(sn_i + float3(0, 1, 1)), snHash(sn_i + float3(1, 1, 1)), sn_u.x), sn_u.y);
    return lerp(sn_z0, sn_z1, sn_u.z);
}

// returns a point light contribution with no shadow map sampling
float GetPointLightContribution(float4 sn_wpos, float4 sn_lpos, float4 sn_nrm){
	if (sn_lpos.w == 0) return 0;

	float3 sn_ldir = sn_lpos.xyz - sn_wpos.xyz;
	float sn_dist = length(sn_ldir) / sn_lpos.w; // normalize distance over light range
	float4 sn_light = float4(sn_ldir, sn_dist);

	// radius based attenuation based on https://lisyarus.github.io/blog/graphics/2022/07/30/point-light-attenuation.html
	float sn_s = sn_dist * sn_dist; 
	float sn_att = saturate((1 - sn_s) / (1 + sn_s));

	sn_ldir = normalize(sn_ldir); // normalize
	float sn_diff = dot(sn_ldir, sn_nrm.xyz);

	float3 sn_eye_d = normalize(TESR_CameraPosition.xyz - sn_wpos.xyz);
	float3 sn_half = normalize(sn_ldir + sn_eye_d);
	float sn_spec = pows(shade(sn_half, sn_nrm.xyz), 20);

	return saturate((sn_diff + sn_spec) * sn_att);
}

// Compute the areas of the screen that must be covered with snow
float4 SnowCoverage( VSOUT px ) : COLOR0
{
	// compute at quarter scale
    float2 uv = px.UVCoord * 4;
	if (uv.x > TESR_RenderScale.x || uv.y > TESR_RenderScale.y) return white;	// UNOFFICIAL 2026-10-27 RenderScale v2: a quarter of the drawn part
	
    float sn_depth;
    float4 sn_world = SnowWorldPosition(uv, sn_depth);	// UNOFFICIAL 2026-10-28: the jittered pixel's own ray (SnowRay)

	if (sn_depth > TESR_OrthoData.x) return white; // early out for the sky pixels

	// sample an average ortho
    float sn_ort = GetOrtho(sn_world);
    sn_ort += GetOrtho(sn_world + float4(-1, 0, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(1, 0, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0, -1, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0, 1, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(-0.7, -0.7, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(-0.7, 0.7, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0.7, -0.7, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0.7, 0.7, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(-0.3, 0, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0.3, 0, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0, -0.3, 0, 0) * orthoRadius);
    sn_ort += GetOrtho(sn_world + float4(0, 0.3, 0, 0) * orthoRadius);
	sn_ort /= 13;

	sn_ort = smoothstep(0.1, 0.9, sn_ort); // reduce glitches by removing outlier values 
    sn_ort = lerp(sn_ort, 1, smoothstep(0.6 * TESR_OrthoData.x, TESR_OrthoData.x, sn_depth)); // fade out ortho with distance

	return float4(sn_ort.xxx, 1);
}

// UNOFFICIAL 2026-10-28 NormalSmoothing ([Shaders.SnowAccumulation.Main], TESR_SnowAccumulationColor.w: game units, 0 = off). User:
// "snow + tessellation ... we get these facets": the snow takes its slope (where it may lie) and its shading from the normal rebuilt
// from depth, one flat normal per triangle, so on displaced land the snow came and went triangle by triangle. Here the normal is the
// average over a disc of that radius on the surface (12 points round the pixel, projected at its depth, each weighed by its distance
// in the world, so a wall or a rock a few units away hardly counts): snow lies on the ground's overall shape, as real snow does.
static const float2 SnowSmoothTaps[12] = {
	float2(0.35f, 0.0f), float2(-0.35f, 0.0f), float2(0.0f, 0.35f), float2(0.0f, -0.35f),
	float2(0.5f, 0.5f), float2(-0.5f, 0.5f), float2(0.5f, -0.5f), float2(-0.5f, -0.5f),
	float2(1.0f, 0.0f), float2(-1.0f, 0.0f), float2(0.0f, 1.0f), float2(0.0f, -1.0f) };

// UNOFFICIAL 2026-10-28 footprints (effects/CoverMap.h MaxPrints; user: "snow foot steps just kinda leaves a line behind when we walk
// like we're riding a bicycle"): the player's last prints, one per step, left and right in turn. The cover map keeps each print as a
// dot (8 units a texel), so near these prints this draws them instead, per pixel, as boot prints: the ball and toes and the heel, two
// ellipses along the step's heading, the big-toe side towards the path. -1: no print near (the map's trodden snow applies).
uniform float4 TESR_CoverMapFeet[24];	// xyz: centre (relative to the camera), w: side (+ left foot) x (heading in whole degrees + 0.99 x depth); 0: none
uniform float4 TESR_CoverMapFeetData;	// xy, z: a circle round all of them (relative to the camera; z <= 0: none), w: a print's half length

float SnowFootprint(float3 rel_v)
{
	// UNOFFICIAL 2026-10-28 (macintroll: "footsteps are a nice addition but shape is weird, should be a single blob"): one soft oval per
	// step by default; a negative half length (PersistentFootprintShape 1) = the boot shape below (sole and heel).
	float half = abs(TESR_CoverMapFeetData.w);
	bool boot_v = TESR_CoverMapFeetData.w < 0.0f;
	float2 from_all = rel_v.xy - TESR_CoverMapFeetData.xy;
	[branch] if (TESR_CoverMapFeetData.z <= 0.0f || dot(from_all, from_all) > TESR_CoverMapFeetData.z * TESR_CoverMapFeetData.z) return -1.0f;
	float best_v = 0.0f;
	float near_any = 0.0f;
	[loop] for (int i = 0; i < 24; i++) {
		float4 foot_v = TESR_CoverMapFeet[i];
		float2 d = rel_v.xy - foot_v.xy;
		[branch] if (foot_v.w != 0.0f && dot(d, d) < 4.0f * half * half && abs(rel_v.z - foot_v.z) < 40.0f) {
			float packed_v = abs(foot_v.w);
			float heading_v = floor(packed_v) * (3.14159265f / 180.0f);
			float sn_depth = frac(packed_v) / 0.99f;
			float2 ahead_v;
			sincos(heading_v, ahead_v.y, ahead_v.x);
			// along the heading, and sideways away from the path (mirrored for the right foot)
			float2 qq = float2(dot(d, ahead_v), dot(d, float2(-ahead_v.y, ahead_v.x)) * sign(foot_v.w));
			float shape_v;
			[branch] if (boot_v) {
				float toes_v = length((qq - float2(0.26f * half, -0.05f * half)) / float2(0.66f * half, 0.40f * half));
				float heel_v = length((qq - float2(-0.58f * half, 0.0f)) / float2(0.36f * half, 0.31f * half));
				shape_v = 1.0f - smoothstep(0.8f, 1.05f, min(toes_v, heel_v));
			}
			else shape_v = 1.0f - smoothstep(0.55f, 1.05f, length(qq / float2(0.95f * half, 0.45f * half)));	// one soft oval along the step
			best_v = max(best_v, sn_depth * shape_v);
			near_any = 1.0f;
		}
	}
	return near_any > 0.0f ? best_v : -1.0f;
}

float3 SnowSmoothNormal(float2 uv, float3 center_v, float3 sn_norm, float sn_depth)
{
	float radius_v = TESR_SnowAccumulationColor.w;
	[branch] if (radius_v <= 0.0f || sn_depth >= farZ * 0.99f) return sn_norm;
	float2 per_unit = 0.5f * float2(TESR_ProjectionTransform[0][0], TESR_ProjectionTransform[1][1]) / max(sn_depth, 1.0f);	// screen share per game unit
	float2 tmp_s01 = rectToScreen(uv);
	float3 sum_v = sn_norm;
	// UNOFFICIAL 2026-10-28 (macintroll: white sheets over cars and rocks, streaks across the ground and ferns): a tap counts only where
	// its own normal is near this pixel's (the land's facets differ by a few tens of degrees; a car's side and the ground under it, or a
	// fern and the ground, by far more: their normals no longer tilt each other), and only on screen (a tap clamped to the edge read the
	// same border column for a whole band of pixels: streaks along the rows).
	[unroll] for (int i = 0; i < 12; i++) {
		float2 raw_v = tmp_s01 + SnowSmoothTaps[i] * radius_v * per_unit;
		float2 tap_uv = screenToRect(saturate(raw_v));
		float on_screen = all(raw_v == saturate(raw_v)) ? 1.0f : 0.0f;
		float tap_depth = readDepthLod(tap_uv);
		float3 tap_pos = SnowRay(tap_uv) * tap_depth;
		float3 tap_normal = GetWorldNormalLod(tap_uv);
		float weight_v = saturate(1.0f - length(tap_pos - center_v) / (2.0f * radius_v)) * (tap_depth < farZ * 0.99f ? 1.0f : 0.0f) * on_screen *
			smoothstep(0.5f, 0.85f, dot(tap_normal, sn_norm));
		sum_v += tap_normal * weight_v;
	}
	return normalize(sum_v);
}

// UNOFFICIAL 2026-10-27 persist (technique 1, [Shaders.SnowAccumulation.Main] Persistent): the snow's amount comes from the cover map
// per spot (built up where the snow reached, melted by the sun, drifts) instead of the one amount for the whole world, and trodden
// snow (footprints) is drawn compacted; a spot with no snow left gets none (the noise pattern alone would leave flecks). With
// persist = false every line is as before.
float4 Snow( VSOUT px, uniform bool persist_v ) : COLOR0
{
	float4 sn_col = tex2D(TESR_SourceBuffer, px.UVCoord);
	float3 sn_world = SnowRay(px.UVCoord);	// UNOFFICIAL 2026-10-28: the jittered pixel's own ray
	
	float sn_depth = readDepth(px.UVCoord);
	float3 sn_cam_v = sn_world * sn_depth;
	float3 sn_eye = -1 * normalize(sn_world);
	float4 sn_wpos = float4(TESR_CameraPosition.xyz + sn_cam_v, 1.0f);

	[branch] if (TESR_SnowAccumulationExtra.y > 0.5f) return SnowCapturedView(px.UVCoord, length(sn_cam_v));	// UNOFFICIAL 2026-10-28
	// UNOFFICIAL 2026-10-28 (user: "standing next to a rock the snow seems to become visible through my character"; much more with Look 1):
	// the coverage is found at a quarter of the resolution and blurred, so beside a snowy rock it spread onto the character in front of it.
	// Actors' pixels (CapturedNormals: the skin and skinned armour and clothes write alpha 2) get no snow, as the ortho test meant.
	[branch] if (CapturedActor(px.UVCoord, length(sn_cam_v))) return sn_col;

	float sn_ort = tex2D(TESR_RenderedBuffer, px.UVCoord).x;
	float3 sn_norm = GetWorldNormal(px.UVCoord);
	float3 captured_norm;	// UNOFFICIAL 2026-10-28 CapturedNormals: the surface's own smooth normal where it was captured
	bool captured_v = SnowCapturedNormal(px.UVCoord, length(sn_cam_v), sn_norm, captured_norm);
	if (captured_v) sn_norm = captured_norm;

	// early out for the character gun, water surfaces/areas and surfaces above the ortho map (such as actors)
	float sn_water_t = (sn_depth/farZ) * 200;
	float sn_is_water = (dot(sn_norm, float3(0, 0, 1)) > 0.9) && (sn_wpos.z > TESR_WaterSettings.x - sn_water_t) && (sn_wpos.z < TESR_WaterSettings.x + sn_water_t);
	if (sn_is_water || !(sn_ort > 0)) return sn_col;
	[branch] if (!captured_v) sn_norm = SnowSmoothNormal(px.UVCoord, sn_cam_v, sn_norm, sn_depth);	// UNOFFICIAL 2026-10-28 NormalSmoothing (after the water test; captured normals are smooth already)

    sn_col = linearize(sn_col);
	float3 sn_sun_c = linearize(TESR_SunColor).rgb;

	float2 uv = sn_wpos.xy / 200.0f;
	// UNOFFICIAL 2026-10-27 persist: this spot's snow (z) and trodden snow (w) from the cover map. Trodden (footprint) snow stays snow,
	// compacted: flatter (less of the snow's own bumps, no sparkle, less rim light), greyer and darker; only in the middle of a
	// hard-trodden trail does the ground show through.
	float4 cover_v = float4(0, 0, 0, 0);
	float trodden_v = 0.0f;
	if (persist_v)
	{
		float cover_inside;
		cover_v = CoverMapReadAll(sn_cam_v.xy, cover_inside);
		trodden_v = saturate(CoverMapTrodden(cover_v.w));
		// UNOFFICIAL 2026-10-28 footprints: near the last prints, the boot prints themselves instead of the map's dots (no snow: none)
		float print_v = SnowFootprint(sn_cam_v);
		if (print_v >= 0.0f) trodden_v = print_v * TESR_CoverMapTrail.y * saturate(cover_v.z * 20.0f) * TESR_CoverMapGlobal.w;
	}
	float3 sn_loc_n = expand(tex2D(TESR_SnowNormSampler, uv).xyz);
	if (persist_v) sn_loc_n.xy *= 1.0f - 0.75f * trodden_v;
	float3 sn_surf_n = normalize(float3(sn_loc_n.xy + sn_norm.xy, sn_loc_n.z * sn_norm.z));
	float4 sn_nrm =  float4(sn_surf_n, 1);

	float3 sn_tex = TESR_SnowAccumulationColor.rgb;
	if (persist_v) sn_tex = lerp(sn_tex, luma(sn_tex) * float3(0.56f, 0.58f, 0.62f), 0.8f * trodden_v);

	float sn_fres_c = saturate(pow(1 - shade(sn_eye, sn_surf_n), 5));
	float3 sn_spec_c = pows(shades(normalize(TESR_SunDirection.xyz + sn_eye), sn_surf_n), 20) * sn_fres_c;

	float3 sn_amb = sn_tex * pows(TESR_SunAmbient.rgb,2.2); // linearise
	float2 sn_shad = tex2D(TESR_PointShadowBuffer, px.UVCoord).rg;
	sn_shad.r = lerp(1.0f, sn_shad.r, useShadows); // disable shadow sampling if shadows are disabled in game
	sn_shad.r = lerp(sn_shad.r, 1.0f, TESR_ShadowFade.x);	// fade shadows to light when sun is low

	float3 sn_diff = sn_tex * shade(TESR_SunDirection, sn_nrm) * sn_sun_c * diffusePower * sn_shad.r;
	float3 sn_spec = sn_spec_c * sn_sun_c * specularPower * sn_shad.r;
	float3 sn_fres = sn_fres_c * sn_sun_c * fresnelPower;
	float3 sn_spark = pows(shades(sn_eye, normalize(expand(tex2D(TESR_BlueNoiseSampler, sn_wpos.xy / 200).rgb))), 1000) * 0.2 * sn_shad.r;
	if (persist_v)
	{
		sn_fres *= 1.0f - 0.6f * trodden_v;
		sn_spark *= 1.0f - trodden_v;
	}

	// float4 snowColor = float4(ambient + diffuse + spec + fresnel, 1);
	float4 sn_snow_c = float4(sn_amb + sn_diff + sn_spec + sn_fres + sn_spark, 1);
	float sn_plp = 0.5;

	sn_shad.g = 1; // disable point light shadows for debug

	for (int i = 0; i<12; i++){
		sn_snow_c.rgb += GetPointLightContribution(sn_wpos, TESR_ShadowLightPosition[i], sn_nrm) * linearize(float4(TESR_LightColor[i].rgb * TESR_LightColor[i].a, 1)) * sn_plp * sn_shad.g;
		sn_snow_c.rgb += GetPointLightContribution(sn_wpos, TESR_LightPosition[i], sn_nrm) * linearize(float4(TESR_LightColor[12 + i].rgb * TESR_LightColor[12 + i].a, 1)) * sn_plp * sn_shad.g;
	}

	// create a noisy pattern of accumulation over time
	float snow_amount = persist_v ? cover_v.z : TESR_SnowAccumulationParams.w;
	float sn_cov = saturate(pows(snNoise(sn_wpos.xyz / (noiseSize * 0.5)) * snNoise(sn_wpos.xyz / (noiseSize * 3)) * snNoise(sn_wpos.xyz / noiseSize), 2 - 2 * snow_amount));
	sn_cov = saturate(smoothstep(0.2, 0.4, sn_cov));
	if (persist_v) sn_cov *= smoothstep(0.0, 0.05, snow_amount) * (1.0f - smoothstep(0.5f, 0.9f, trodden_v * (0.7f + 0.6f * snNoise(sn_wpos.xyz / 18.0f))));
	float sn_vert = smoothstep(0.5, 0.8, shade(float3(0, 0, 1), sn_norm));

	sn_col = lerp(sn_col, sn_snow_c, sn_cov * min(sn_vert, sn_ort));
	sn_col.a = 1;

    sn_col.rgb = delinearize(sn_col);
	return sn_col;
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 SnowCoverage( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Scale( TESR_RenderedBuffer, 4 );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskH, TESR_SnowAccumulationParams.y, 3500, TESR_OrthoData.x );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskV, TESR_SnowAccumulationParams.y, 3500, TESR_OrthoData.x );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Snow( false );
	}
}

// UNOFFICIAL 2026-10-27 Persistent (CoverMap.h): technique 1, the same passes with the snow amount per spot from the cover map.
technique PersistentTechnique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 SnowCoverage( );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Scale( TESR_RenderedBuffer, 4 );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskH, TESR_SnowAccumulationParams.y, 3500, TESR_OrthoData.x );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskV, TESR_SnowAccumulationParams.y, 3500, TESR_OrthoData.x );
	}
	pass
	{
		VertexShader = compile vs_3_0 FrameVS( );
		PixelShader = compile ps_3_0 Snow( true );
	}
}

// =====================================================================================================================================
// UNOFFICIAL 2026-10-28 Look 1 ([Shaders.SnowAccumulation.Main] Look = 1; techniques 2 and 3, effects/SnowAccumulation.cpp Technique):
// macintroll's version (5) of this shader, shared 2026-10-28, as he wrote it except: his names prefixed SA5_ where they clash
// with Look 0's above, his blue noise as TESR_SnowNoiseSampler (Effects\Snownoise256.dds), RenderScale v2's drawn part in the coverage
// pass, and the captured normals (SnowCapturedNormal) in place of his depth-normal smoothing where they are valid. Plain D3D9: the NVR
// DXVK pass (Compute\SnowAccumulation.frag) runs Look 0 only, so Look 1 always runs these D3D9 passes.
// =====================================================================================================================================
static const float SA5_diffusePower    = 0.56f;
static const float SA5_specularPower   = 1.0f;
static const float SA5_fresnelPower    = 0.65f;
static const float INV_NOISE_SIZE       = 1.0f / 200.0f;
static const float INV_NOISE_SIZE_HALF  = 1.0f / 100.0f;
static const float INV_NOISE_SIZE_X3    = 1.0f / 600.0f;
static const float INV_200   = 1.0f / 200.0f;
static const float INV_64    = 1.0f / 64.0f;
static const float INV_96    = 1.0f / 96.0f;
static const float INV_512   = 1.0f / 512.0f;
static const float INV_2048  = 1.0f / 2048.0f;
// Delayed accumulation start: visible ground coverage begins only once the snowfall transition is essentially complete, then ramps
// in from the engine accumulation value (TESR_SnowData.x the active snowfall 0..1, TESR_SnowAccumulationParams.w the accumulation).
static const float ACCUM_SNOW_READY_START   = 0.94f;
static const float ACCUM_SNOW_READY_FULL    = 0.995f;
static const float ACCUM_DELAY_START        = 0.25f;
static const float ACCUM_DELAY_FULL         = 0.82f;
static const float ACCUM_PERSIST_START      = 0.72f;
static const float ACCUM_PERSIST_FULL       = 0.88f;

// Diagnostic: 0 shaded, 1 stored amount, 2 procedural noise, 3 base normal,
// 4 ortho shelter, 5 snow-noise texture. No lighting applied in diagnostic views.
static const int SA_Debug = 0;
// Appearance approximation only: no vertex displacement or recovered mesh normals.
static const float SA_SnowRoundness = 1.0f; // 0 original, 1 upward-biased snow lighting
static const float SA_SnowBroadDetail = 0.12f;
static const float SA_SnowBroadScale = 700.0f; // game units per snow-normal repeat
static const float SA_SnowSlopeSoftness = 0.26f;
static const float SA_SnowSlopeNoise = 0.12f;
static const float SA_SnowSlopeLift = 0.35f;
// Relative contour widths; brightness is reflected snow light, not emission.
static const float SA_FootInnerShadow = 0.32f; // 0 disables, 1 fully dark at peak
static const float SA_FootOuterGlow = 0.12f;   // fractional snow-light boost
static const float SA_FootInnerWidth = 0.82f;
static const float SA_FootOuterWidth = 0.88f;
static const float SA_FootprintRemoval = 0.90f; // 0 compressed snow, 1 bare-ground holes
// Fixed-world-radius normal smoothing; still an approximation from visible depth.
#define SA_SMOOTH_NORMALS 1
#define SA_SMOOTH_COVERAGE 1
static const float SA_NormalRadiusWorld = 48.0f; // game units, NOT pixels
static const float SA_NormalSmoothStrength = 1.0f;

float3 SA_SmoothSnowNormal(float2 uv, float center_depth, float3 original_v) {
#if SA_SMOOTH_NORMALS
    float radius_v = SA_NormalRadiusWorld;
    if (radius_v <= 0.0f || center_depth >= farZ * 0.99f) return original_v;
    float3 center_v = normalize(original_v);
    float3 center_pos = SnowRay(uv) * center_depth;
    float3 total_f = center_v * 2.0f;
    static const float2 offs[8] = {
        float2(0.5f,0), float2(-0.5f,0), float2(0,0.5f), float2(0,-0.5f),
        float2(0.7071f,0.7071f), float2(-0.7071f,0.7071f),
        float2(0.7071f,-0.7071f), float2(-0.7071f,-0.7071f)
    };
    // Same screen/rectangle convention as version (4)'s world-radius filter.
    float2 center_screen = rectToScreen(uv);
    float2 radius_screen = 0.5f * abs(float2(TESR_ProjectionTransform[0][0],
                                           TESR_ProjectionTransform[1][1]))
                         * radius_v / max(center_depth, 1.0f);
    [unroll] for (int i = 0; i < 8; ++i) {
        float2 tap_screen = center_screen + offs[i] * radius_screen;
        float in_screen = step(0.0f, tap_screen.x) * step(tap_screen.x, 1.0f)
                       * step(0.0f, tap_screen.y) * step(tap_screen.y, 1.0f);
        float2 sample_uv = screenToRect(saturate(tap_screen));
        float sample_depth = readDepthLod(sample_uv);
        float3 sampled_v = normalize(GetWorldNormalLod(sample_uv));
        float3 sample_pos = SnowRay(sample_uv) * sample_depth;
        // Fixed 3D-distance rejection replaces the depth-relative tolerance.
        float separation_v = length(sample_pos - center_pos);
        float weight_v = 1.0f - smoothstep(0.5f * radius_v, 2.0f * radius_v, separation_v);
        weight_v *= smoothstep(0.25f, 0.70f, dot(center_v, sampled_v));
        weight_v *= in_screen * (sample_depth < farZ * 0.99f ? 1.0f : 0.0f);
        total_f += sampled_v * weight_v;
    }
    return normalize(lerp(center_v, normalize(total_f), saturate(SA_NormalSmoothStrength)));
#else
    return original_v;
#endif
}

// ── Hash / noise ─────────────────────────────────────────────────────────

float SA_Hash21(float2 sn_pp) {
    sn_pp = frac(sn_pp * float2(123.34f, 345.45f));
    sn_pp += dot(sn_pp, sn_pp + 34.345f);
    return frac(sn_pp.x * sn_pp.y);
}

float2 SA_Rotate2D(float2 sn_pp) {
    return float2(0.8192f * sn_pp.x - 0.5736f * sn_pp.y,
                  0.5736f * sn_pp.x + 0.8192f * sn_pp.y);
}

float SA_ValueNoise2D(float2 sn_pp) {
    float2 i = floor(sn_pp);
    float2 sn_f = frac(sn_pp);
    sn_f = sn_f * sn_f * (3.0f - 2.0f * sn_f);
    float a = SA_Hash21(i);
    float b = SA_Hash21(i + float2(1, 0));
    float c = SA_Hash21(i + float2(0, 1));
    float d = SA_Hash21(i + float2(1, 1));
    return lerp(lerp(a, b, sn_f.x), lerp(c, d, sn_f.x), sn_f.y);
}

// Replace the correlated polynomial hash with the original sine hash.
float SA5_hash(float sn_n) {
    return frac(sin(sn_n) * 43758.5453f);
}

// Optimisation 3: coverage noise reduced from 3 calls to 2.
// Original multiplied three octaves; two octaves give the same breakup character
// with the third baked into the exponent tweak below.
float SA5_noise(float3 x) {
    // Rotate the lattice away from the world axes; retain world-stable sampling.
    x.xy = float2(0.8f*x.x - 0.6f*x.y, 0.6f*x.x + 0.8f*x.y);
    x.xz = float2(0.8f*x.x - 0.6f*x.z, 0.6f*x.x + 0.8f*x.z);
    float3 sn_pp = floor(x);
    float3 sn_f = frac(x);
    sn_f = sn_f * sn_f * (3.0f - 2.0f * sn_f);
    float sn_n = sn_pp.x + sn_pp.y * 57.0f + 113.0f * sn_pp.z;
    return lerp(
        lerp(lerp(SA5_hash(sn_n       ), SA5_hash(sn_n +  1.0f), sn_f.x),
             lerp(SA5_hash(sn_n + 57.0f), SA5_hash(sn_n + 58.0f), sn_f.x), sn_f.y),
        lerp(lerp(SA5_hash(sn_n +113.0f), SA5_hash(sn_n +114.0f), sn_f.x),
             lerp(SA5_hash(sn_n +170.0f), SA5_hash(sn_n +171.0f), sn_f.x), sn_f.y), sn_f.z);
}

// ── Snowflake impacts (first-person) ──────────────────────────────────────

float SA_ImpactCell(float2 uv, float time_v)
{
    float2 gv_v = frac(uv) - 0.5f;
    float2 id_v = floor(uv);

    float rnd1_v = SA_Hash21(id_v);
    float rnd2_v = SA_Hash21(id_v + 17.23f);
    float rnd3_v = SA_Hash21(id_v + 43.71f);

    float2 center_v = float2(rnd1_v, rnd2_v) - 0.5f;
    float  cycle_v  = frac(time_v + rnd3_v);
    float  life_v   = saturate(cycle_v / 0.35f);
    float  alive_v  = 1.0f - step(0.35f, cycle_v);

    float fade_in  = smoothstep(0.0f, 0.05f, life_v);
    float fade_out = 1.0f - smoothstep(0.22f, 0.60f, life_v);
    float fade    = fade_in * fade_out * alive_v;

    float2 qq = gv_v - center_v;
    qq.y -= (0.01f + life_v * (0.08f + rnd1_v * 0.04f));

    float d      = length(qq);
    float radius_v = lerp(0.025f, 0.14f, life_v);
    float angle_v  = atan2(qq.y, qq.x);

    float curve_v    = sin(angle_v * 2.0f + id_v.y * 1.3f) * 0.35f;
    float spikes_v   = abs(sin((angle_v + curve_v) * 3.0f + id_v.x * 1.7f));
    float spike_mask = lerp(1.0f, spikes_v, 0.45f);

    // Edge warp: was SA_ValueNoise2D (4x SA_Hash21 + lerp). Single hash is enough
    // at the sub-pixel scale snow impacts render at.
    float grain_v    = SA_Hash21(floor(qq * 6.0f + id_v * 0.3f));
    float edge_warp = (grain_v - 0.5f) * radius_v * 0.35f;
    float warped_radius = radius_v * spike_mask + edge_warp;

    float core_v   = 1.0f - smoothstep(radius_v * 0.07f, radius_v * 0.18f, d);
    float splash_v = 1.0f - smoothstep(warped_radius * 0.6f, warped_radius, d);
    float shape_v  = splash_v * 0.7f + core_v * 0.3f;

    // Optimisation 4b: pow(core,4) -> core*core*core*core (no transcendental)
    float core2_v = core_v * core_v;
    shape_v += core2_v * core2_v * 0.6f;

    // Multiply by guard so invisible cells cost only the hash+fade math above
    return shape_v * fade * step(0.001f, fade);
}

// ── Ortho occlusion ───────────────────────────────────────────────────────

float SA5_GetOrtho(float4 sn_wpos) {
    const float sn_thick = 0.0025f;
    const float feather_v   = 0.02f;
    const float base_soft  = 0.0055f;

    float4 sn_ort_p = mul(sn_wpos, TESR_ShadowCameraToLightTransformOrtho);
    sn_ort_p.xyz /= sn_ort_p.w;
    sn_ort_p.xy = sn_ort_p.xy * float2(0.5f, -0.5f) + 0.5f.xx;

    float2 uv = sn_ort_p.xy;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 0.0f;

    float2 d       = min(uv, 1.0f - uv);
    float edge_mask = smoothstep(0.0f, feather_v, min(d.x, d.y));

    float ortho_depth = tex2D(TESR_OrthoMapBuffer, uv).r;
    float delta_v      = abs(sn_ort_p.z - ortho_depth);

    float soft_v = base_soft + 0.001f;
    float band_v = 1.0f - smoothstep(sn_thick, sn_thick + soft_v, delta_v);
    return band_v * edge_mask;
}

// Optimisation 1: 13-tap -> 9-tap ortho average.
// Removed the 4 inner cross taps at +-0.3 - they overlap heavily with the
// center sample and contribute <4% to the final value at typical orthoRadius.
float GetOrthoAverage(float4 sn_wpos, float orthoRadius) {
    const float2 offs[9] = {
        float2( 0.0f,  0.0f),
        float2(-1.0f,  0.0f), float2(1.0f,  0.0f),
        float2( 0.0f, -1.0f), float2(0.0f,  1.0f),
        float2(-0.7f, -0.7f), float2(-0.7f, 0.7f),
        float2( 0.7f, -0.7f), float2( 0.7f, 0.7f)
    };

    float sn_ort = 0.0f;
    for (int i = 0; i < 9; i++) {
        sn_ort += SA5_GetOrtho(sn_wpos + float4(offs[i] * orthoRadius, 0.0f, 0.0f));
    }
    return sn_ort * (1.0f / 9.0f);
}

// Optimisation 5a: point light helper - normalize() removed, invLen reused.
// UNOFFICIAL 2026-10-29 (P84 v4 did not start: Conor; a bench run with no frame in 180 s): no early returns. Inlined for 24 lamps (ps_3_0
// cannot index the lamp constants in a loop, so the loop is unrolled), the three returns plus the callers' ifs made ~100 nested branches
// and D3DX9_43 took 2+ minutes for this one pass (twice per effect: 5-10 minutes at startup, the main thread waiting on it). Same result:
// out of reach, (1 - s) / (1 + s) is already <= 0 (s = (distance / radius)^2 >= 1); an unused slot (radius 0) is multiplied by 0; the
// 0.001 cut is a select.
float SA5_PointLight(float4 sn_wpos, float4 sn_lpos, float3 sn_nrm, float3 sn_eye_d) {
    float used_v    = sn_lpos.w > 0.0f ? 1.0f : 0.0f;
    float radius_v  = max(sn_lpos.w, 1e-4f);

    float3 L      = sn_lpos.xyz - sn_wpos.xyz;
    float distSq  = dot(L, L);

    float inv_len  = rsqrt(max(distSq, 1e-6f));
    float dist_norm = (distSq * inv_len) / radius_v;
    float sn_s       = dist_norm * dist_norm;
    float sn_att   = saturate((1.0f - sn_s) / (1.0f + sn_s));
    sn_att         = sn_att > 0.001f ? sn_att * used_v : 0.0f;

    // invLen already computed - reuse instead of calling normalize()
    float3 light_dir = L * inv_len;
    float  sn_diff  = saturate(dot(light_dir, sn_nrm));

    // halfway still needs normalize (sum of two unit vectors is not unit)
    float3 half_vec = normalize(light_dir + sn_eye_d);
    float  sn_spec    = pow(saturate(dot(half_vec, sn_nrm)), 20.0f);

    return saturate((sn_diff + sn_spec) * sn_att);
}

// ── SA5_SnowCoverage pass (quarter-res ortho prepass - CHANGED TO 1-8) ─────────────────────────

float4 SA5_SnowCoverage(VSOUT px) : COLOR0 {
    float2 uv = px.UVCoord * 4.0f;
    if (uv.x > TESR_RenderScale.x || uv.y > TESR_RenderScale.y) return 1.0f.xxxx;	// RenderScale v2: a quarter of the drawn part (as Look 0)

    float  sn_depth;
    float4 sn_world = SnowWorldPosition(uv, sn_depth);	// UNOFFICIAL 2026-10-28: the jittered pixel's own ray (SnowRay)
    if (sn_depth > TESR_OrthoData.x) return 1.0f.xxxx;
    if (sn_depth >= 3500.0f)         return 1.0f.xxxx;

    float sn_ort = GetOrthoAverage(sn_world, TESR_SnowAccumulationParams.x);
    sn_ort = smoothstep(0.1f, 0.9f, sn_ort);
    sn_ort = lerp(sn_ort, 1.0f, smoothstep(0.6f * TESR_OrthoData.x, TESR_OrthoData.x, sn_depth));

    float ortho_fade = 1.0f - smoothstep(2500.0f, 3500.0f, sn_depth);
    sn_ort = lerp(1.0f, sn_ort, ortho_fade);

    return float4(sn_ort, sn_ort, sn_ort, 1.0f);
}

// ── Main Snow pass ────────────────────────────────────────────────────────

// (TESR_CoverMapFeet / TESR_CoverMapFeetData: declared above)

float SA_SmoothMin(float a, float b, float k) {
    float h_v = saturate(0.5f + 0.5f * (b - a) / k);
    return lerp(b, a, h_v) - k * h_v * (1.0f - h_v);
}

float SA5_SnowFootprint(float3 rel_v, out float2 relief_v)
{
    relief_v = float2(0, 0);
    float occupied_v = 0.0f;
	float foot_half_size = TESR_CoverMapFeetData.w;
	float2 from_all = rel_v.xy - TESR_CoverMapFeetData.xy;
	[branch] if (TESR_CoverMapFeetData.z <= 0.0f || dot(from_all, from_all) > TESR_CoverMapFeetData.z * TESR_CoverMapFeetData.z) return -1.0f;
	float best_v = 0.0f;
	float near_any = 0.0f;
	[loop] for (int i = 0; i < 24; i++) {
		float4 foot_v = TESR_CoverMapFeet[i];
		float2 d = rel_v.xy - foot_v.xy;
		[branch] if (foot_v.w != 0.0f && dot(d, d) < 4.0f * foot_half_size * foot_half_size && abs(rel_v.z - foot_v.z) < 40.0f) {
			float packed_v = abs(foot_v.w);
			float heading_v = floor(packed_v) * (3.14159265f / 180.0f);
			float sn_depth = frac(packed_v) / 0.99f;
			float2 ahead_v;
			sincos(heading_v, ahead_v.y, ahead_v.x);
			float2 qq = float2(dot(d, ahead_v), dot(d, float2(-ahead_v.y, ahead_v.x)) * sign(foot_v.w));
            // Connected sole: rounded forefoot, narrower arch, rounded heel.
            // q.y is already mirrored for left/right by the original foot data.
            float2 sole_v = qq / max(foot_half_size, 0.001f);
            float toes_v = length((sole_v - float2(0.29f, -0.025f)) / float2(0.64f, 0.39f)) - 1.0f;
            float heel_v = length((sole_v - float2(-0.59f, 0.0f)) / float2(0.34f, 0.30f)) - 1.0f;
            float arch_v = length((sole_v - float2(-0.16f, 0.045f)) / float2(0.47f, 0.255f)) - 1.0f;
            float sole_distance = SA_SmoothMin(SA_SmoothMin(toes_v, arch_v, 0.20f), heel_v, 0.20f);
            best_v = max(best_v, sn_depth * (1.0f - smoothstep(-0.12f, 0.10f, sole_distance)));
            // Bell-shaped bands on opposite sides of the sole contour.
            float inner_width = max(SA_FootInnerWidth, 0.001f);
            float outer_width = max(SA_FootOuterWidth, 0.001f);
            float inner_band = smoothstep(-inner_width, -0.25f * inner_width, sole_distance)
                            * (1.0f - smoothstep(-0.25f * inner_width, 0.0f, sole_distance));
            float outer_band = smoothstep(0.0f, 0.25f * outer_width, sole_distance)
                            * (1.0f - smoothstep(0.25f * outer_width, outer_width, sole_distance));
            relief_v = max(relief_v, saturate(sn_depth) * float2(inner_band, outer_band));
            occupied_v = max(occupied_v, saturate(sn_depth) * (1.0f - smoothstep(-0.08f, 0.0f, sole_distance)));
			near_any = 1.0f;
		}
	}
    // Suppress a raised rim where another active footprint presses into it.
    relief_v.y *= 1.0f - occupied_v;
	return near_any > 0.0f ? best_v : -1.0f;
}

float4 SA5_Snow(VSOUT px, uniform bool persist_v) : COLOR0 {
    float4 sn_col = tex2D(TESR_SourceBuffer, px.UVCoord);

    float3 sn_world = SnowRay(px.UVCoord);	// UNOFFICIAL 2026-10-28: the jittered pixel's own ray
    float  sn_depth = readDepth(px.UVCoord);

    float vm_depth_x = tex2D(TESR_DepthBufferViewModel, px.UVCoord).x;
    float vm_mask   = 0.0f;

    // FIRST PERSON --------------------------------------------------------
    if ((TESR_DepthConstants.z == 0 && vm_depth_x < 0.9f) ||
        (TESR_DepthConstants.z > 0  && vm_depth_x > 0.01f))
        vm_mask = 1.0f;

    if (vm_mask > 0.5f && sn_depth < 120.0f) {
        float3 sn_norm  = GetWorldNormal(px.UVCoord);
        float  up_vec    = saturate(sn_norm.z);
        float  top_zone = smoothstep(0.72f, 0.94f, up_vec);

        float2 uv = px.UVCoord * float2(10.0f, 6.0f);
        float  t  = TESR_GameTime.x * 0.020f;

        float hit1_v = SA_ImpactCell(uv, t);
        float hit2_v = SA_ImpactCell(uv * 1.08f + 3.17f, t * 0.93f);

        float impact_v = saturate(hit1_v + hit2_v * 0.6f) * top_zone * 0.45f;

        // Gate impacts on ACTIVE snowfall (TESR_SnowData.x), not accumulation amount.
        // TESR_SnowData.x > 0 only while flakes are actually falling.
        // When snowfall stops, x drops to 0 and impacts fade out immediately,
        // while accumulated snow on the ground lingers and melts separately.
        //   0.15 upper edge: full impact rate reached quickly once snowing starts.
        //   Lower it (0.05) to trigger on the lightest flurries.
        //   Raise it (0.4)  to require heavier snowfall before impacts appear.
        float impact_allow = smoothstep(0.0f, 0.15f, TESR_SnowData.x);
        impact_v *= impact_allow;

        sn_col.rgb = lerp(sn_col.rgb, float3(0.92f, 0.94f, 0.96f), impact_v);
        return sn_col;
    }

    // Persistent data uses the same camera-relative coordinates as version (4).
    float3 sn_cam_v = sn_world * sn_depth;
    [branch] if (TESR_SnowAccumulationExtra.y > 0.5f) return SnowCapturedView(px.UVCoord, length(sn_cam_v));	// UNOFFICIAL 2026-10-28
    [branch] if (CapturedActor(px.UVCoord, length(sn_cam_v))) return sn_col;	// UNOFFICIAL 2026-10-28: no snow on actors (see Look 0)
    float4 cover_v = float4(0, 0, 0, 0);
    float trodden_v = 0.0f;
    float2 footprint_relief = float2(0, 0);
    if (persist_v) {
        float cover_inside;
        cover_v = CoverMapReadAll(sn_cam_v.xy, cover_inside);
        trodden_v = saturate(CoverMapTrodden(cover_v.w));
        float footprint_v = SA5_SnowFootprint(sn_cam_v, footprint_relief);
        footprint_relief *= saturate(TESR_CoverMapTrail.y * saturate(cover_v.z * 20.0f) * TESR_CoverMapGlobal.w);
        if (footprint_v >= 0.0f)
            trodden_v = footprint_v * TESR_CoverMapTrail.y * saturate(cover_v.z * 20.0f) * TESR_CoverMapGlobal.w;
    }

    if (SA_Debug == 1) {
        float amount_v = saturate(persist_v ? cover_v.z : TESR_SnowAccumulationParams.w);
        return float4(amount_v.xxx, 1);
    }
    if (SA_Debug == 2) {
        float sn_n = SA5_noise((TESR_CameraPosition.xyz + sn_cam_v) * INV_NOISE_SIZE);
        return float4(sn_n.xxx, 1);
    }
    if (SA_Debug == 3) return float4(normalize(GetWorldNormal(px.UVCoord)) * 0.5f + 0.5f, 1);
    if (SA_Debug == 4) return float4(tex2D(TESR_RenderedBuffer, px.UVCoord).xxx, 1);
    if (SA_Debug == 5) return float4(tex2D(TESR_SnowNoiseSampler,
        (TESR_CameraPosition.xy + sn_cam_v.xy) * INV_512).rgb, 1);

    // Delayed accumulation gate.
    // 1) During weather transition, no ground coverage is allowed until the
    //    falling snow is essentially at full strength.
    // 2) The accumulation value is remapped so visible coverage begins later.
    // 3) Once accumulation is already high enough, the persistence path keeps
    //    snow visible even if TESR_SnowData.x falls back to zero.
    float accumulation_raw = saturate(persist_v ? cover_v.z : TESR_SnowAccumulationParams.w);
    float snowfall_ready = smoothstep(
        ACCUM_SNOW_READY_START,
        ACCUM_SNOW_READY_FULL,
        TESR_SnowData.x
    );
    float delayed_accumulation = smoothstep(
        ACCUM_DELAY_START,
        ACCUM_DELAY_FULL,
        accumulation_raw
    );
    float persistence_gate = smoothstep(
        ACCUM_PERSIST_START,
        ACCUM_PERSIST_FULL,
        accumulation_raw
    );
    float accumulation_gate = max(delayed_accumulation * snowfall_ready, persistence_gate);
    // The persistent map owns accumulation/melting. Retain version (5)'s
    // amount ramp, but never erase stored snow merely because snowfall stops.
    if (persist_v) accumulation_gate = delayed_accumulation * smoothstep(0.0f, 0.05f, accumulation_raw);

    float detail_fade  = 1.0f - smoothstep(1200.0f, 2200.0f, sn_depth);
    float sparkle_fade = 1.0f - smoothstep( 800.0f, 1600.0f, sn_depth);
    float water_fade   = 1.0f - smoothstep(1000.0f, 1800.0f, sn_depth);

    float3 sn_eye  = -normalize(sn_world);
    float4 sn_wpos      = float4(TESR_CameraPosition.xyz + sn_cam_v, 1.0f);

    float ortho_raw = tex2D(TESR_RenderedBuffer, px.UVCoord).x;
    if (ortho_raw < 0.001f) return sn_col;

    float3 original_norm = GetWorldNormal(px.UVCoord);
    // UNOFFICIAL 2026-10-28 CapturedNormals: the surface's own smooth normal where it was captured (no smoothing needed there)
    float3 captured_norm;
    bool captured_v = SnowCapturedNormal(px.UVCoord, length(sn_cam_v), original_norm, captured_norm);
    if (captured_v) original_norm = captured_norm;
    float3 sn_norm = original_norm;
    [branch] if (!captured_v) sn_norm = SA_SmoothSnowNormal(px.UVCoord, sn_depth, original_norm);
#if SA_SMOOTH_COVERAGE
    float up_nbase = sn_norm.z;
#else
    float up_nbase = original_norm.z;
#endif
    float snow_roundness = saturate(SA_SnowRoundness);
    if (up_nbase <= lerp(0.15f, -0.10f, snow_roundness)) return sn_col;

    float water_threshold = (sn_depth / farZ) * 150.0f;
    float water_up_mask    = step(0.9f, dot(original_norm, float3(0.0f, 0.0f, 1.0f)));
    float water_height_mask = step(TESR_WaterSettings.x - water_threshold, sn_wpos.z) *
                            step(sn_wpos.z, TESR_WaterSettings.x + water_threshold);
    if (water_up_mask * water_height_mask > 0.5f) return sn_col;

    sn_col = linearize(sn_col);

    // Precompute world-space UV scales once
    float2 world_xy_200  = sn_wpos.xy * INV_200;
    float2 tmp_bn64         = sn_wpos.xy * INV_64;
    float2 tmp_bn512        = sn_wpos.xy * INV_512;
    float2 tmp_bn2048       = sn_wpos.xy * INV_2048;

    float3 sn_loc_n    = expand(tex2D(TESR_SnowNormSampler, world_xy_200).xyz);
    if (persist_v) sn_loc_n.xy *= 1.0f - 0.75f * trodden_v;
    float3 lighting_base = sn_norm;
    if (snow_roundness > 0.0f) {
        // Sample a second, coarser layer from the SAME snow normal DDS.
        // World anchoring avoids a camera-relative noise pattern.
        float2 broad_uv = SA_Rotate2D(sn_wpos.xy) / max(SA_SnowBroadScale, 1.0f) + float2(0.37f, 0.61f);
        float3 broad_detail = expand(tex2D(TESR_SnowNormSampler, broad_uv).xyz);
        float broad_strength = SA_SnowBroadDetail;
        if (persist_v) broad_strength *= 1.0f - 0.75f * trodden_v;
        float3 snow_base = normalize(float3(broad_detail.xy * broad_strength, 1.0f));
        lighting_base = normalize(lerp(normalize(sn_norm), snow_base, snow_roundness));
    }
    float3 sn_surf_n = normalize(float3(sn_loc_n.xy + lighting_base.xy, sn_loc_n.z * lighting_base.z));

    float3 snow_tex  = TESR_SnowAccumulationColor.rgb;
    if (persist_v) snow_tex = lerp(snow_tex, luma(snow_tex) * float3(0.64f, 0.72f, 0.82f), 0.55f * trodden_v);
    float3 sn_sun_c = linearize(TESR_SunColor).rgb;
    float3 sn_amb  = snow_tex * (TESR_SunAmbient.rgb * TESR_SunAmbient.rgb); // was pow(rgb,2.2) - x*x (gamma2.0) saves 3 log+exp

    float ndv_v          = saturate(dot(sn_eye, sn_surf_n));
    float sn_fres_c = saturate(pow(1.0f - ndv_v, 5.0f));
    float3 sun_half     = normalize(TESR_SunDirection.xyz + sn_eye);
    float  sn_spec_c    = pow(saturate(dot(sun_half, sn_surf_n)), 20.0f) * sn_fres_c;

    float2 sn_shad = tex2D(TESR_PointShadowBuffer, px.UVCoord).rg;
    sn_shad.r = lerp(sn_shad.r, 1.0f, TESR_ShadowFade.x);

    // TESR_SnowAccumulationParams.z = SunPower: scales all sun-driven lighting terms.
    // 1.0 = original intensity, 0.0 = flat ambient only, 2.0 = very bright sun on snow.
    float sun_power = TESR_SnowAccumulationParams.z;
    float  n_dot_l   = saturate(dot(TESR_SunDirection.xyz, sn_surf_n));
    float3 sn_diff = snow_tex * n_dot_l * sn_sun_c * SA5_diffusePower * sn_shad.r * sun_power;
    float3 sn_spec    = sn_spec_c * sn_sun_c * SA5_specularPower * sn_shad.r * sun_power;
    float3 sn_fres = sn_fres_c * sn_sun_c * SA5_fresnelPower * sun_power;

    float3 sparkle_norm = normalize(expand(tex2D(TESR_SnowNoiseSampler, world_xy_200).rgb));
    float  sn_spark    = pow(saturate(abs(dot(sn_eye, sparkle_norm))), 1000.0f)
                       * 0.2f * sn_shad.r * sparkle_fade;

    if (persist_v) {
        sn_fres *= 1.0f - 0.6f * trodden_v;
        sn_spark *= 1.0f - trodden_v;
    }
    float4 sn_snow_c = float4(sn_amb + sn_diff + sn_spec + sn_fres + sn_spark, 1.0f);

    // Point lights: linearize() inlined per-light (local arrays not allowed in ps_3_0)
    sn_shad.g = 1.0f;
    for (int i = 0; i < 12; i++) {
        sn_snow_c.rgb += SA5_PointLight(sn_wpos, TESR_ShadowLightPosition[i], sn_surf_n, sn_eye)
                           * linearize(float4(TESR_LightColor[i].rgb * TESR_LightColor[i].a, 1.0f)).rgb
                           * 0.5f * sn_shad.g;

        sn_snow_c.rgb += SA5_PointLight(sn_wpos, TESR_LightPosition[i], sn_surf_n, sn_eye)
                           * linearize(float4(TESR_LightColor[12 + i].rgb * TESR_LightColor[12 + i].a, 1.0f)).rgb
                           * 0.5f * sn_shad.g;
    }

    if (persist_v) {
        // Applied to lit snow only: subsequent coverage and shelter/shadow
        // compositing also gate this relief, so bare ground gets no halo.
        float inner_shade = 1.0f - saturate(SA_FootInnerShadow) * footprint_relief.x;
        float outer_light = 1.0f + max(SA_FootOuterGlow, 0.0f) * footprint_relief.y;
        sn_snow_c.rgb *= inner_shade * outer_light;
    }

    // Optimisation 3: coverage with 2 noise octaves instead of 3.
    // The third octave (INV_NOISE_SIZE_X3, coarsest) modulated the others
    // multiplicatively but its contribution is absorbed into the exponent.
    float cov0_v = SA5_noise(sn_wpos.xyz * INV_NOISE_SIZE_HALF);
    float cov1_v = SA5_noise(sn_wpos.xyz * INV_NOISE_SIZE);
    float sn_cov = saturate(pow(abs(cov0_v * cov1_v), 2.2f - 2.0f * delayed_accumulation));

    float2 tmp_p2    = SA_Rotate2D(sn_wpos.xz);
    float  cov_v   = smoothstep(0.05f, 0.6f, sn_cov);
    // pow(cov, 0.92f) removed: x^0.92 vs x^1.0 is <4% on a [0,1] mask, undetectable
    float breakup_v = SA_ValueNoise2D(tmp_p2 * 0.003f);
    cov_v          *= lerp(0.8f, 1.0f, breakup_v);
    float final_coverage = cov_v;

    float up_n = saturate(up_nbase);
    // Softer directional eligibility plus smooth world-space breakup.
    // Retain a gentle underside guard; do not turn downward faces into snow.
    float underside_guard = lerp(1.0f, smoothstep(-0.10f, 0.25f, up_nbase), snow_roundness);
    up_n = lerp(up_n, 1.0f, snow_roundness * saturate(SA_SnowSlopeLift));
    float slope_noise = ((cov0_v + cov1_v) - 1.0f) * SA_SnowSlopeNoise * snow_roundness;
    up_n = saturate(up_n + slope_noise);
    float req_v = lerp(0.92f, 0.40f, delayed_accumulation);
    float slope_width = lerp(0.10f, max(SA_SnowSlopeSoftness, 0.10f), snow_roundness);
    float allow_v = smoothstep(req_v - (slope_width - 0.10f), req_v + 0.10f, up_n) * underside_guard;
    final_coverage *= allow_v;

    // ── Blue noise fetches ────────────────────────────────────────────────────
    // The texture is 512x512 RGBA with 4 fully independent channels.
    // By reading different channels at different scales and offsets we get
    // completely different patterns from the same 3 texture samples.
    // No extra fetches needed - just swizzle channels differently per role.
    //
    // Sample A: fine grain        (bn64,  .r channel)
    // Sample B: medium breakup    (bn512, .r channel)
    // Sample C: large patches     (bn2048, RGBA - we read all 4 channels)
    //   .r -> primary large-scale patch map
    //   .g -> secondary large-scale map (spatially offset by texture wrap)
    //   .b -> tertiary variation (use for slope macro)
    //   .a -> quaternary (use for grain variation)
    // This gives variation that never tiles visibly across the landscape.

    float  noise64_v   = tex2D(TESR_SnowNoiseSampler, tmp_bn64).r;
    float  noise512_v  = tex2D(TESR_SnowNoiseSampler, tmp_bn512).r;
    float4 noise2048_rgba_v = tex2D(TESR_SnowNoiseSampler, tmp_bn2048); // all 4 channels, one fetch
    float  noise2048_v = noise2048_rgba_v.r;

    // Large-patch variation: blend 3 independent channels at the 2048 scale.
    // Each channel is a different blue noise pattern -> no repetition.
    // PATCH_A/B/C weights control how much each layer contributes.
    const float PATCH_A = 0.50f;  // primary large patch   (try 0.4-0.6)
    const float PATCH_B = 0.30f;  // secondary variation   (try 0.2-0.4)
    const float PATCH_C = 0.20f;  // tertiary breakup      (try 0.1-0.3)
    float patch_noise = noise2048_rgba_v.r * PATCH_A
                     + noise2048_rgba_v.g * PATCH_B
                     + noise2048_rgba_v.b * PATCH_C;

    float slope_v  = up_n;
    // Macro: large patch noise drives slope variation, noise512 adds mid breakup.
    const float MACRO_LARGE = 0.75f;   // patch weight  (try 0.5 - 0.8)
    const float MACRO_MID   = 0.50f;   // noise512 weight (try 0.2 - 0.5)
    float macro_v  = patch_noise * MACRO_LARGE + noise512_v * MACRO_MID;
    slope_v += (macro_v - 1.0f) * 0.3f;

    float sn_vert = smoothstep(lerp(0.35f, 0.20f, snow_roundness), 0.72f, slope_v);
    float edge_v     = 1.0f - abs(sn_vert * 2.0f - 1.0f);
    edge_v           = saturate(edge_v * edge_v);

    float d = noise64_v * 0.65f + noise512_v * 0.35f;
    sn_vert = saturate(sn_vert + (d - 0.5f) * 0.18f * edge_v * detail_fade);

    float snow_mask = final_coverage * min(sn_vert, ortho_raw);

    // Final delayed-start multiplier: zero during the initial snowfall
    // transition, then progressively reaches the original full coverage.
    snow_mask *= accumulation_gate;
    if (persist_v) {
        // Keep most of the snow in the impression instead of cutting a ground hole.
        snow_mask *= 1.0f - SA_FootprintRemoval * smoothstep(0.5f, 0.9f,
            trodden_v * (0.7f + 0.6f * SA5_noise(sn_wpos.xyz / 18.0f)));
    }

    // Optimisation 6: grain pow(x, 0.55) -> sqrt (single instruction on SM3).
    // sqrt(x) ≈ x^0.5, close enough to x^0.55 at snow-grain scale.
    // Grain: mix fine noise64 with the .a channel of noise2048 (independent pattern)
    // for variation in the surface texture across large distances.
    float grain_raw = (noise64_v * 0.70f + noise2048_rgba_v.a * 0.30f) - 0.5f;
    float grain_v    = sign(grain_raw) * sqrt(abs(grain_raw) * 2.0f) * 0.5f;
    grain_v         *= detail_fade;

    float partial_c = smoothstep(0.01f, 0.1f, snow_mask) * (1.0f - smoothstep(0.07f, 0.80f, snow_mask));
    snow_mask *= (1.0f + grain_v * partial_c);

    // Extra opacity multiplier for the noisy shoreline transition.
    // This is applied AFTER the regular edge breakup below, so the existing
    // noise pattern stays the same and only the final snow blend becomes softer.
    float shoreline_opacity = 1.0f;

    float water_level = TESR_WaterSettings.x;
    float len_v       = abs(sn_wpos.z - water_level);

    // Width of the noisy/transparent shoreline band around the water plane.
    // Higher value = the partial transparent snow transition reaches farther from water.
    const float WATER_EDGE_WIDTH = 5.0f; // was 5.0f

    if (len_v < WATER_EDGE_WIDTH) {
        float near_water   = 1.0f - saturate(len_v / WATER_EDGE_WIDTH);
        float solid_remove = smoothstep(0.75f, 1.0f, near_water);
        float keep_prob    = 1.0f - near_water;
        float water_noise  = lerp(0.5f, noise2048_v * 0.60f + noise64_v * 0.40f, water_fade);
        float keep_mask    = (1.0f - smoothstep(keep_prob - 0.001f, keep_prob + 0.001f, water_noise))
                          * (1.0f - solid_remove);
        snow_mask *= keep_mask;

        // Surviving snow pixels near the water plane are semi-transparent.
        // Lower WATER_EDGE_MIN_OPACITY for more visible terrain underneath.
        // Raise it for denser/whiter snow near the shoreline.
        const float WATER_EDGE_MIN_OPACITY = 0.05f;
        const float WATER_EDGE_MAX_OPACITY = 0.15f;
        float noisy_opacity = lerp(WATER_EDGE_MIN_OPACITY, WATER_EDGE_MAX_OPACITY, water_noise);
        shoreline_opacity  = lerp(1.0f, noisy_opacity, near_water);
    }

    const float edge_band = 0.12f;
    float near_edge = 1.0f - saturate(snow_mask / edge_band);
    float zone_v     = smoothstep(0.05f, 0.90f, near_edge);
    if (zone_v > 0.0f) {
        float solid_remove = smoothstep(0.70f, 0.98f, near_edge);
        float keep_prob    = 1.0f - near_edge * (1.0f + 0.15f * (1.0f - near_edge)); // was pow(nearEdge,1.15) - MAD approx, max err 0.004
        float edge_noise   = lerp(0.5f, noise2048_v * 0.60f + noise64_v * 0.40f, detail_fade);
        float keep_mask    = 1.0f - smoothstep(keep_prob - 0.001f, keep_prob + 0.001f, edge_noise);
        keep_mask         *= (1.0f - solid_remove);
        snow_mask         *= lerp(1.0f, keep_mask, zone_v);
    }

    // Apply shoreline transparency after the existing breakup masks.
    snow_mask *= shoreline_opacity;

    // ── Shadow-respecting snow composite ─────────────────────────────────────
    // Instead of lerp(scene, snowColor, mask) which replaces the shadowed pixel
    // with a bright snow colour, we blend in TWO steps:
    //
    //   1. lerp the surface toward white snow (coverage blend)
    //   2. multiply the result by the shadow value from TESR_PointShadowBuffer
    //      so shadows darken the snow just as they darken the scene
    //
    // shadow.r from TESR_PointShadowBuffer is the raw [0..1] shadow mask
    // (1 = fully lit, 0 = fully shadowed) already read above.
    // We scale it to [DARKNESS..1] to match ShadowsExteriors behaviour.
    //
    // Result: snow has full coverage everywhere, but is darkened in shadows.
    // No tuning constants needed - it automatically follows the shadow config.

    // shadow.r is the raw point-shadow mask [0..1].
    // ShadowsExteriors already darkened `color` by shadowFactor, so we must NOT
    // multiply the whole pixel again — that would double-darken shadows.
    // Instead we only darken the SNOW CONTRIBUTION by shadowFactor, then add it
    // on top of the already-shadowed scene colour.
    //
    //   color      = scene already in shadow  (untouched)
    //   snowColor  = what snow would look like fully lit
    //   snowMask   = coverage [0..1]
    //   shadowFactor = how lit this pixel is [DARKNESS..1]
    //
    // Final = scene_in_shadow  +  snow_contribution * shadowFactor
    //       = lerp(color, color + (snowColor-color)*shadowFactor, snowMask)

    float shadow_factor = lerp(DARKNESS, 1.0f, sn_shad.r);
    float4 shadowed_snow = float4(lerp(sn_col.rgb, sn_snow_c.rgb * shadow_factor, snow_mask), 1.0f);
    sn_col.rgb = shadowed_snow.rgb;
    sn_col.a  = 1.0f;
    sn_col.rgb = delinearize(sn_col.rgb);
    return sn_col;
}

technique LookTechnique {
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 SA5_SnowCoverage( );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 Scale( TESR_RenderedBuffer, 4 );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskH, TESR_SnowAccumulationParams.y, 8500, TESR_OrthoData.x );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskV, TESR_SnowAccumulationParams.y, 8500, TESR_OrthoData.x );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 SA5_Snow( false );
    }
}

// Selected by NVR when persistent coverage is enabled.
technique LookPersistentTechnique {
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 SA5_SnowCoverage( );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 Scale( TESR_RenderedBuffer, 4 );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskH, TESR_SnowAccumulationParams.y, 8500, TESR_OrthoData.x );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 DepthBlur( TESR_RenderedBuffer, OffsetMaskV, TESR_SnowAccumulationParams.y, 8500, TESR_OrthoData.x );
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader  = compile ps_3_0 SA5_Snow( true );
    }
}
