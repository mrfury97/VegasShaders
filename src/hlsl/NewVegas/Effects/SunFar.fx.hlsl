// UNOFFICIAL 2026-10-19 SunFar: the sun's shadows that the shadow maps cannot give, as a factor per pixel that the sun-shadow
// composites multiply the picture by (ShadowsExteriors.fx, VolumetricFog.fx CompositeSunShadow, AmbientLight.fx SunShadowApply):
//   - the land's shadow on the land and everything on it, far past the cascades ([Shaders.ShadowsExteriors.TerrainShadows],
//     effects/TerrainShadows.h): ported from Conor's (mrfury97) NVR fork, after Community Shaders' "Terrain Shadows";
//   - the shadows of the game's own cloud layers ([Shaders.CloudShadows.Main]), after Community Shaders' "Cloud Shadows" idea
//     (the clouds' cover where the light from the point meets the cloud layer, dimming the sun only). NVR's own D3D9 version:
//     the sky's four cloud layer textures are read straight from the sky, on a flat layer above the camera.
// Community Shaders: github.com/community-shaders/skyrim-community-shaders (GPL-3.0 with the Modding and Linking exceptions).
// Conor's fork: github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork (GPL-3.0-or-later). This file: GPL-3.0-or-later (NVR).
//
// Both block only the SUN. With ForwardShadows the object shaders light each surface as ambient + sky + sun x cascades, so
// the pixel is rescaled by (ambient + sky + sun x cascades x blocked) / (ambient + sky + sun x cascades), rebuilt from the
// same terms in the space they lit in, per colour channel: a surface in the land's shadow keeps its ambient and sky light,
// and so their colour. This is Conor's share composite (his ShadowsExteriors.fx, 6043062a), moved into its own pass so every
// composite can take it. The cascades' own shadow is looked up where something here blocks the sun, so a surface the
// cascades already shade is not darkened twice.
//
// Output (TESR_SunFarBuffer, A8R8G8B8): rgb the factor in the stored (gamma-encoded) space, 1 where nothing changes (linear
// composites take its 2.2 power, as they linearise the picture); a the share of the sun that gets through (1 all).

float4 TESR_ReciprocalResolution;
float4 TESR_WaterSettings;        // x: water height in view
float4 TESR_ShadowFade;           // x: sunrise / sunset fade of the cascades, y: shadow maps active
float4 TESR_ShadowSunLight;       // the sun and ambient colours as the object shaders receive them
float4 TESR_ShadowAmbientLight;   // (ShadowsExteriorEffect::UpdateLightColors, after Conor)
float4 TESR_PBRData;              // z: LightingScale, w: AmbientScale, as the object shaders apply them
float4 TESR_PBRExtraData;         // y: SkylightingScale
float4 TESR_TerrainData;          // z: LightingScale, w: AmbientScale -- terrain has its own
float4 TESR_TerrainSkyData;       // x: SkylightingScale
float4 TESR_SkyIrradianceObjects[9];     // the sky light the object shaders add (Shaders/Includes/SkyAmbient.hlsl, mode 0)
float4 TESR_SkyIrradianceTerrain[9];     // the sky light the terrain shaders add (Shaders/Includes/SkyAmbient.hlsl, mode 0)
float4 TESR_TerrainShadowData;    // x west edge, y north edge, z 1 / width, w strength (0 off)
float4 TESR_TerrainShadowRange;   // x lowest encoded height, y encoded range, z bias, w blur
float4 TESR_TerrainShadowParams;  // x 1 / height, y DebugView, z Darkness, w Contrast
float4 TESR_SunFarData;           // x: cloud shadow strength (0 off), y: debug view, z: linear lighting gamma (1: off), w: 1 / z
float4 TESR_SunFarClouds;         // x: the cloud layer's height above the camera, y: 1 / tile size (per unit), z: mip level, w: world-to-uv rotation angle
float4 TESR_SunFarCloudOffset[2]; // the layers' drift in uv: [0].xy layer 0, [0].zw layer 1, [1].xy layer 2, [1].zw layer 3
float4 TESR_SunFarCloudWeight;    // per layer: its share (0: no texture)
float4 TESR_SunFarCloudBlend;     // per layer: how far it has blended to the incoming weather's texture
float4 TESR_SunFarVolume;         // UNOFFICIAL 2026-10-21 the volumetric clouds' shadow: x strength (0 off), y map width (m), z map texels, w cloud base (m)
float4 TESR_SunDirection;         // the light the volumetric clouds' shadow map was made with
float4 TESR_FogData;              // UNOFFICIAL 2026-10-25: the game's fog as the object and land shaders apply it: x near, y far, w power
float4 TESR_FogColor;
float4 TESR_SunFarJitter;         // UNOFFICIAL 2026-10-31 xy: JitterFix's depth jitter in uv (0 off), z: 1 SkipCascadeCheck (test)
float4 TESR_SunFarLand;           // UNOFFICIAL 2026-10-25: xyz the light vector the land shaders lit with (core/LandLight.h), w 1 when read lately
float4 TESR_SunFarWater;          // UNOFFICIAL 2026-10-23 [Shaders.CloudShadows.Main] WaterShadows: x 1 on (0: water left alone), y deep water's share, z 1 the depth map is bound; w 1 OldSunModel (test, 2026-10-25)

// In register order: NVR binds TESR_ samplers in declaration order.
sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_TerrainShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_TerrainShadowGround : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// The sky's cloud layers (and the textures they are blending to), bound by SunFarEffect::Render. After the TESR_ samplers.
sampler2D NVR_CloudLayer0 : register(s5) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudLayer1 : register(s6) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudLayer2 : register(s7) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudLayer3 : register(s8) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudBlend0 : register(s9) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudBlend1 : register(s10) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudBlend2 : register(s11) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D NVR_CloudBlend3 : register(s12) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-21: the volumetric clouds' shadow map (VolumetricClouds.fx ShadowMap: the sunlight through the layer, 1 clear),
// bound by SunFarEffect::RenderFactor.
sampler2D NVR_VolumeCloudShadow : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-23: the game's water depth map (PlaceableWaterGroup::spDepthMap, as the water shaders read it per screen
// position: x 0 at the shore, 1 deep), bound by SunFarEffect::RenderFactor with WaterShadows on.
sampler2D NVR_WaterDepth : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-25: the world as drawn (the game's scene target, stored space), bound by SunFarEffect::RenderFactor.
sampler2D NVR_Scene : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"
#include "Includes/Shadows.hlsl"
#include "Includes/SunCascades.hlsl"

VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

// The cascades' sun visibility at a point: one lookup, in the cascade the point falls in (no cross-fade between them). It is only
// needed so a surface the cascades already shade is not darkened twice; GetSunCascadeLight's eight inlined lookups would need more
// of ps_3_0's 16 boolean registers (one per branch on a constant) than this pass has left.
float CascadeLight(float3 positionWS, float3 normal) {
	float4 radii = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);
	float4 distances = float4(length(positionWS - TESR_ShadowNearCenter.xyz), length(positionWS - TESR_ShadowMiddleCenter.xyz),
	                          length(positionWS - TESR_ShadowFarCenter.xyz), length(positionWS - TESR_ShadowLodCenter.xyz));
	float c = distances.x < radii.x ? 0.0f : distances.y < radii.y ? 1.0f : distances.z < radii.z ? 2.0f : distances.w < radii.w ? 3.0f : 4.0f;
	[branch] if (c > 3.5f) return 1.0f;
	float4x4 lightTransform = c < 0.5f ? TESR_ShadowCameraToLightTransformNear : c < 1.5f ? TESR_ShadowCameraToLightTransformMiddle :
	                          c < 2.5f ? TESR_ShadowCameraToLightTransformFar : TESR_ShadowCameraToLightTransformLod;
	float2 quadrant = c < 0.5f ? float2(0.0f, 0.0f) : c < 1.5f ? float2(0.5f, 0.0f) : c < 2.5f ? float2(0.0f, 0.5f) : float2(0.5f, 0.5f);
	float radius = c < 0.5f ? radii.x : c < 1.5f ? radii.y : c < 2.5f ? radii.z : radii.w;
	float bleed = c < 0.5f ? 0.1f : c < 1.5f ? 0.2f : c < 2.5f ? 0.6f : 0.8f;
	float offsetScale = saturate(1.0f - dot(normal, TESR_SmoothedSunDir.xyz));
	float offset = offsetScale * 2.5f * 4.0f * radius * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
	float bias = (TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f) * (1.0f + offsetScale);
	return SunCascadeValue(lightTransform, float4(positionWS + offset * normal, 1.0f), quadrant.x, quadrant.y, bias, bleed);
}

// The space the object shaders light in (Shaders/Includes/LinearLight.hlsl): with linear lighting the colours and scales are
// decoded first.
// (No branches on constants here: ps_3_0 has 16 boolean registers. z is 1 with linear lighting off.)
float3 LightColor(float3 c) { return pow(max(c, 1e-6f), TESR_SunFarData.z); }
float LightScale(float s) { return pow(max(s, 1e-6f), TESR_SunFarData.z); }

// Mirrors SkyAmbientRadiance (mode 0) in Shaders/Includes/SkyAmbient.hlsl, encoded the same way. (Conor.)
float3 SkyIrradiance(float3 n) {
	float3 irradiance = TESR_SkyIrradianceObjects[0].rgb
		+ TESR_SkyIrradianceObjects[1].rgb * n.y
		+ TESR_SkyIrradianceObjects[2].rgb * n.z
		+ TESR_SkyIrradianceObjects[3].rgb * n.x
		+ TESR_SkyIrradianceObjects[4].rgb * (n.x * n.y)
		+ TESR_SkyIrradianceObjects[5].rgb * (n.y * n.z)
		+ TESR_SkyIrradianceObjects[6].rgb * (3.0f * n.z * n.z - 1.0f)
		+ TESR_SkyIrradianceObjects[7].rgb * (n.x * n.z)
		+ TESR_SkyIrradianceObjects[8].rgb * (n.x * n.x - n.y * n.y);
	return sqrt(max(irradiance, 0.0f));
}

float3 SkyIrradianceTerrain(float3 n) {
	float3 irradiance = TESR_SkyIrradianceTerrain[0].rgb
		+ TESR_SkyIrradianceTerrain[1].rgb * n.y
		+ TESR_SkyIrradianceTerrain[2].rgb * n.z
		+ TESR_SkyIrradianceTerrain[3].rgb * n.x
		+ TESR_SkyIrradianceTerrain[4].rgb * (n.x * n.y)
		+ TESR_SkyIrradianceTerrain[5].rgb * (n.y * n.z)
		+ TESR_SkyIrradianceTerrain[6].rgb * (3.0f * n.z * n.z - 1.0f)
		+ TESR_SkyIrradianceTerrain[7].rgb * (n.x * n.z)
		+ TESR_SkyIrradianceTerrain[8].rgb * (n.x * n.x - n.y * n.y);
	return sqrt(max(irradiance, 0.0f));
}

// Where the height map covers (DebugView 2): 1 inside, 0 outside or off. (Conor.)
float TerrainShadowCoverage(float3 worldPos) {
	if (TESR_TerrainShadowData.w <= 0.0f) return 0.0f;
	float2 uv = float2((worldPos.x - TESR_TerrainShadowData.x) * TESR_TerrainShadowData.z, (TESR_TerrainShadowData.y - worldPos.y) * TESR_TerrainShadowParams.x);
	return all(uv >= 0.0f) && all(uv <= 1.0f) ? 1.0f : 0.0f;
}

// 1 lit, 0 in the land's shadow. As CS's GetTerrainShadow, faded out toward the map's edge. (Conor.)
float TerrainShadow(float3 worldPos) {
	[branch] if (TESR_TerrainShadowData.w <= 0.0f) return 1.0f;
	float2 uv = float2((worldPos.x - TESR_TerrainShadowData.x) * TESR_TerrainShadowData.z, (TESR_TerrainShadowData.y - worldPos.y) * TESR_TerrainShadowParams.x);
	float2 edge = min(uv, 1.0f - uv);
	float inside = saturate(min(edge.x, edge.y) * 20.0f);
	[branch] if (inside <= 0.0f) return 1.0f;
	// The shadow heights (each sweep) and the ground (each map) in two textures, in the same 16-bit encoding.
	float3 encoded = float3(tex2Dlod(TESR_TerrainShadowBuffer, float4(uv, 0.0f, 0.0f)).rg, tex2Dlod(TESR_TerrainShadowGround, float4(uv, 0.0f, 0.0f)).r) * TESR_TerrainShadowRange.y + TESR_TerrainShadowRange.x;
	float2 heights = encoded.xy - TESR_TerrainShadowRange.z;
	// Never below the map's ground: the simplified LOD land on screen dips under it in places, and those pixels would be
	// shadowed by their own slope (flickering with TAA's jitter). Objects keep their height.
	float z = max(worldPos.z, encoded.z);
	float lower = heights.y - TESR_TerrainShadowRange.w;
	float transition = heights.x + TESR_TerrainShadowRange.w - lower;
	float lit = transition <= 0.0f ? (z >= heights.x ? 1.0f : 0.0f) : saturate((z - lower) / transition);
	lit = saturate((lit - 0.5f) * TESR_TerrainShadowParams.w + 0.5f);   // Contrast: a steeper edge
	return lerp(1.0f, lit, TESR_TerrainShadowData.w * inside);
}

// One cloud layer's cover at uv, blended toward the incoming weather's texture as the sky blends it (the CPU binds the layer's own
// texture there when nothing is incoming, and weight 0 for a missing layer).
float CloudCover(sampler2D layer, sampler2D incoming, float2 uv, float weight, float blend) {
	float a = tex2Dlod(layer, float4(uv, 0.0f, TESR_SunFarClouds.z)).a;
	a = lerp(a, tex2Dlod(incoming, float4(uv, 0.0f, TESR_SunFarClouds.z)).a, blend);
	return saturate(a * weight);
}

// 1 lit, toward 0 under the clouds: the light from this point (camera-relative) meets the cloud layer at the camera's height
// plus TESR_SunFarClouds.x; the four layers' cover there, stacked (each lets 1 - cover through).
float CloudShadow(float3 rel) {
	[branch] if (TESR_SunFarData.x <= 0.0f) return 1.0f;
	float3 light = TESR_SmoothedSunDir.xyz;
	[branch] if (light.z <= 0.02f) return 1.0f;
	float2 meet = rel.xy + light.xy * ((TESR_SunFarClouds.x - rel.z) / light.z);
	float2 world = meet + TESR_CameraPosition.xy;
	float s, c;
	sincos(TESR_SunFarClouds.w, s, c);
	float2 uv = float2(world.x * c - world.y * s, world.x * s + world.y * c) * TESR_SunFarClouds.y;
	float clear = (1.0f - CloudCover(NVR_CloudLayer0, NVR_CloudBlend0, uv + TESR_SunFarCloudOffset[0].xy, TESR_SunFarCloudWeight.x, TESR_SunFarCloudBlend.x))
	            * (1.0f - CloudCover(NVR_CloudLayer1, NVR_CloudBlend1, uv + TESR_SunFarCloudOffset[0].zw, TESR_SunFarCloudWeight.y, TESR_SunFarCloudBlend.y))
	            * (1.0f - CloudCover(NVR_CloudLayer2, NVR_CloudBlend2, uv + TESR_SunFarCloudOffset[1].xy, TESR_SunFarCloudWeight.z, TESR_SunFarCloudBlend.z))
	            * (1.0f - CloudCover(NVR_CloudLayer3, NVR_CloudBlend3, uv + TESR_SunFarCloudOffset[1].zw, TESR_SunFarCloudWeight.w, TESR_SunFarCloudBlend.w));
	return saturate(1.0f - (1.0f - clear) * TESR_SunFarData.x);
}

// UNOFFICIAL 2026-10-21: 1 lit, toward 0 under the volumetric clouds. The same lookup as their own composite (VolumetricClouds.fx
// CloudShadowAt, in metres: the light from the point meets the cloud base; the map's centre is the camera snapped to whole texels),
// so the shadow lands where the clouds are. Their composite used to darken the whole pixel by it, weighted by the depth-rebuilt
// normal: flat per triangle and striped (a P82 tester's rocks), and on top of the cascades' shadow.
float VolumeCloudShadow(float3 world) {
	[branch] if (TESR_SunFarVolume.x <= 0.0f) return 1.0f;
	float3 light = normalize(TESR_SunDirection.xyz);
	light.z = max(light.z, 0.25f);
	light = normalize(light);
	float3 p = world / 70.0f;
	float texel = TESR_SunFarVolume.y / TESR_SunFarVolume.z;
	float2 centre = floor(TESR_CameraPosition.xy / 70.0f / texel) * texel;
	float2 q = p.xy + light.xy * ((TESR_SunFarVolume.w - p.z) / light.z);
	float2 suv = (q - centre) / TESR_SunFarVolume.y + 0.5f;
	float2 edge = min(suv, 1.0f - suv);
	float inside = saturate(min(edge.x, edge.y) * 10.0f);
	return lerp(1.0f, tex2Dlod(NVR_VolumeCloudShadow, float4(suv, 0.0f, 0.0f)).r, inside * TESR_SunFarVolume.x);
}
// The water's surface: facing up at the water's height (as the other effects test it).
bool OnWater(float3 world, float3 normal, float depth) {
	return normal.z > 0.7f && abs(world.z - TESR_WaterSettings.x) < max(2.0f, depth * 0.002f);
}

// UNOFFICIAL 2026-10-23 WaterShadows: how deep the water is here, 0 at the shore to 1 deep, from the game's water depth map;
// far off 1, as the water shader fades to its LOD colour there (WATER000 LODfade, 4096-8192 units).
float WaterDeep(float2 uv, float3 rel) {
	float deep = saturate(tex2Dlod(NVR_WaterDepth, float4(uv, 0.0f, 0.0f)).x);
	return lerp(deep, 1.0f, smoothstep(4096.0f, 8192.0f, length(rel.xy)));
}

// UNOFFICIAL 2026-10-31 JitterFix: with TAA's jitter the depth was drawn through a shifted frustum, so the pixel at uv lies on
// the unjittered ray through uv + TESR_SunFarJitter.xy (screen uv, as the clouds' composite reads its history). Without it the
// rebuilt position is off by up to a render pixel, and the cascade re-read below lands on its own surface: speckles that
// change with every jitter step. toWorld's sums, on the shifted screen position.
float3 toWorldDepthRay(float2 uv) {
	float2 tex = rectToScreen(uv) + TESR_SunFarJitter.xy;
	float3 v = float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]);
	v += (1 / TESR_ProjectionTransform[0][0] * (2 * tex.x - 1)).xxx * float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]);
	v += (-1 / TESR_ProjectionTransform[1][1] * (2 * tex.y - 1)).xxx * float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]);
	return v;
}

float4 SunFarPS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	// Not on the sky (at the far plane, as the volumetric clouds test it) nor on nothing at all. From the linear depth (.x): with
	// SlimDepthBuffer the depth texture has no second channel (Conor's fork read the projected depth in .y).
	float depth = readDepthLod(uv);
	[branch] if (depth <= 0.0f || depth >= farZ * 0.996f) return 1.0f;
	float3 rel = toWorldDepthRay(uv) * depth;   // = toWorld(uv) * depth while JitterFix is off (jitter 0)
	float3 world = TESR_CameraPosition.xyz + rel;

	float terrain = TerrainShadow(world);
	float cloud = CloudShadow(rel) * VolumeCloudShadow(world);
	float blocked = terrain * cloud;
	// DebugView 4 (test): as a fully clouded sky over the whole screen at Strength 1: where the cascades already shade, nothing
	// changes; everywhere else the pixel should come out as dark as the cascades' own shadows next to it, never darker.
	blocked = TESR_SunFarData.y > 3.5f ? 0.0f : blocked;
	// [TerrainShadows] Darkness: in the land's shadow, a share of the ambient and sky light goes too (a valley under a
	// mountain sees less of the sky). (Conor.)
	float shade = (1.0f - terrain) * TESR_TerrainShadowParams.z;

	// Debug views: 1 the land's shadow in green and the clouds' in blue, through the composite (the picture tinted);
	// 2 also the height map's area in a faint blue.
	[branch] if (TESR_SunFarData.y > 0.5f && TESR_SunFarData.y < 3.5f) {
		float3 tint = lerp(1.0f, float3(0.25f, 1.0f, 0.25f), (1.0f - terrain) * 0.8f) * lerp(1.0f, float3(0.35f, 0.5f, 1.0f), (1.0f - cloud) * 0.8f);
		if (TESR_SunFarData.y > 1.5f) tint *= lerp(1.0f, float3(0.85f, 0.9f, 1.0f), TerrainShadowCoverage(world));
		// 3: also the water's depth as WaterShadows reads it, white at the shore to red deep (with the depth map bound).
		float water = TESR_SunFarWater.z * step(2.5f, TESR_SunFarData.y) * (OnWater(world, GetWorldNormalLod(uv), depth) ? 1.0f : 0.0f);
		tint *= lerp(1.0f, lerp(float3(1.0f, 1.0f, 1.0f), float3(1.0f, 0.15f, 0.15f), WaterDeep(uv, rel)), water);
		return float4(tint, 1.0f);
	}
	[branch] if (blocked >= 0.999f && shade <= 0.001f) return 1.0f;   // most of the screen
	float3 normal = GetWorldNormalLod(uv);   // (read only past the early-out above: nothing before it needs it)

	// Water is left alone, as the volumetric clouds' shadow leaves it: what it shows is mostly the reflection of things far off.
	// UNOFFICIAL 2026-10-23 WaterShadows (off: as before): shallow water shows the ground under it, which should be in the
	// shadow too (cut at the waterline, the shadow drew a hard line along every shore). The share it keeps fades from all of it
	// at the shore to WaterDeepShare in deep water, by the game's own water depth map.
	float keep = 1.0f;   // the share of the change this pixel takes (1 off the water)
	[branch] if (OnWater(world, normal, depth)) {
		keep = TESR_SunFarWater.x * lerp(1.0f, TESR_SunFarWater.y, WaterDeep(uv, rel));
		[branch] if (keep <= 0.0f) return float4(1.0f, 1.0f, 1.0f, blocked);
	}

	// What the cascades already took (faded as GetSunShadow fades them). Not on water (keep < 1): the outdoor water shaders
	// draw no sun shadow-map shadows, so nothing there was taken (and the lookup was most of the water's cost).
	float cascade = 1.0f;
	[branch] if (TESR_ShadowFade.y > 0.0f && keep >= 1.0f && TESR_SunFarJitter.z < 0.5f) cascade = saturate(lerp(CascadeLight(rel, normal), 1.0f, TESR_ShadowFade.x));

	// Terrain lights with its own [Shaders.Terrain.*] scales, and nothing in the frame says which pixels are terrain:
	// surfaces facing up take the terrain scales, walls and objects the PBR ones. The terrain constants are only filled
	// while the terrain shaders are enabled. (Conor.)
	float terrainWeight = (TESR_TerrainData.z + TESR_TerrainData.w > 0.0f) ? smoothstep(0.5f, 0.8f, normal.z) : 0.0f;
	float3 scales = lerp(float3(TESR_PBRData.z, TESR_PBRData.w, TESR_PBRExtraData.y),
	                     float3(TESR_TerrainData.z, TESR_TerrainData.w, TESR_TerrainSkyData.x), terrainWeight);
	// UNOFFICIAL 2026-10-25 (bug: a tree's shadow under a cloud's shadow came out lighter than the clouded ground round it): this
	// split must use the light vector the surface was lit with, or a cloud takes away more light than the sun gave and the clouded
	// ground comes out darker than the cascades' own full shadow (which this pass leaves alone).
	//  - The land: the game hands the land shaders their own SunDir (core/LandLight.h, read back from the land's draws:
	//    TESR_SunFarLand.w 1), pointing the way the sun does across the ground but much lower (Poseidon gas station, 17.4 h: 10.8
	//    degrees against the directional light's 27.4), so flat land's NdotL was well under half of what this pass took. This was
	//    the bug: under a full cloud (DebugView 4) the lit ground came out at 17 against a real full shadow of 27 (cloudy weather,
	//    8-bit picture), 27 now. The surfaces taken for land (terrainWeight) use it.
	//  - Everything else: the directional light (TESR_SunDirection), not the shadow maps' sun (with QuantizeSun it moves in
	//    15-degree steps; a small difference here). The cascades and the clouds' placement keep the shadow maps' sun.
	// TESR_SunFarWater.w 1 (OldSunModel, test only): as before.
	float3 lightDir = TESR_SunFarWater.w > 0.5f ? TESR_SmoothedSunDir.xyz : normalize(TESR_SunDirection.xyz);
	float3 landDir = TESR_SunFarLand.w > 0.5f && TESR_SunFarWater.w < 0.5f ? TESR_SunFarLand.xyz : lightDir;
	float NdotL = lerp(saturate(dot(normal, lightDir)), saturate(dot(normal, landDir)), terrainWeight);
	float3 sunLight = LightColor(TESR_ShadowSunLight.rgb) * (NdotL * LightScale(scales.x));
	float3 skyIrr = lerp(SkyIrradiance(normal), SkyIrradianceTerrain(normal), terrainWeight);
	float3 ambientLight = LightColor(TESR_ShadowAmbientLight.rgb) * LightScale(scales.y) + LightColor(skyIrr) * LightScale(scales.z);
	float3 litNow = ambientLight + sunLight * cascade;
	float3 factor = saturate((ambientLight * (1.0f - shade) + sunLight * (cascade * blocked)) / max(litNow, 0.0001f));
	factor = keep < 1.0f ? lerp(1.0f, factor, keep) : factor;   // (a select: off the water exactly as before)
	// Into the stored space: with linear lighting the shaders encoded their result with 1 / gamma (w; 1 with it off).
	factor = pow(max(factor, 1e-6f), TESR_SunFarData.w);
	// UNOFFICIAL 2026-10-25 (with the fix above): the object and land shaders end with the game's fog, lerp(lit, fog colour, fog
	// amount), and the composites multiply the whole pixel by this factor, so the fog's share was darkened as if it were sunlight.
	// Now that share keeps its brightness: the fog amount as the shaders work it out (ObjectTemplate.hlsl / TerrainTemplate.hlsl: by
	// the distance, between the fog's near and far, to its power) times the fog colour, as a share of the pixel as drawn (the
	// scene target, NVR_Scene). It matters with fog close by (it made most of the difference in a cloudy weather's haze, little in
	// clear weather). TESR_SunFarWater.w 1 (OldSunModel, test only, or no scene texture): as before.
	float fogAmount = pow(1.0f - saturate((TESR_FogData.y - length(rel)) / max(TESR_FogData.y - TESR_FogData.x, 1.0f)), max(TESR_FogData.w, 0.0001f));
	float3 fogShare = saturate(fogAmount * TESR_FogColor.rgb / max(tex2Dlod(NVR_Scene, float4(uv, 0.0f, 0.0f)).rgb, 0.0001f));
	factor = TESR_SunFarWater.w > 0.5f ? factor : factor + (1.0f - factor) * fogShare;
	// a: what reaches the sun (1 lit): the Specular effect takes it off the sun's highlight (Specular.fx NVR_SpecularSunFar).
	return float4(factor, blocked);
}

technique SunFar {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SunFarPS();
		ColorWriteEnable = 15;   // all four channels (a: the sun's share that gets through), whatever the game left set
	}
}
