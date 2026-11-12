// UNOFFICIAL 2026-11-04: Wetness Effects' HUD raindrops ([Shaders.WetnessEffects.HudRaindrops], effects/WetnessEffects/HudRaindrops.h),
// ported from Physically-Based-Reloaded (Balebandro, Shaders/Includes/HudRaindrops.hlsl) in place of Wet World's HudDrops (that
// version: VegasShaders-backups/before_hudraindrops_20261104). Raindrops on the first-person arms and weapon, and (AllObjects) on
// every other object but the land, drawn in their own shaders (SkinTemplate.hlsl, ObjectTemplate.hlsl, ParallaxTemplate.hlsl) so
// they stay on the surface however it moves. The technique is Screen Space Shaders' HUD raindrops (STALKER Anomaly, Ascii1457:
// screenspace_hud_raindrops.h, update 17), reworked for NVR:
//   - two layers of a drop texture (Textures\Effects\hud_raindrops.dds: r height, gb slope, a time offset)
//   - each drop shrinks toward its middle over its life and gives way to the next: its height less a threshold that grows with
//     time (and the Density setting)
//   - refraction by moving the texture coordinates under each drop before the material's textures are read, so the drop
//     magnifies the surface under it; reflection by tilting the normal map's normal along the drop's slope; the wet surface and
//     the drops made glossier
//   - surfaces facing up gather drops, the sides a few, the undersides none
// The drops follow the mesh's texture coordinates scaled by how many game units a texture unit covers, so a drop is about the same
// size on every mesh. The wetness (rain while not under cover, drying after) is one value on the GPU, eased every frame by
// Effects/HudRaindrops.fx; the per-draw constants say whether the draw is first-person geometry.
// VegasShaders: c221/c222/c170 (PBR NVR's c218-c220 are HandBlood's and Rmaos' here; c170 is the particles' sun colour, the
// land's lamp-shadow constants written after this in the land's passes), s13/s14 (free in the outdoor records: the
// interior ones, INTERIOR_SHADOWS, give s12-s15 to the lamps' shadow cubes, so they get no drops). How far the face looks up comes
// from the position's derivatives (PBR NVR passes it from its vertex shaders). Compiled in only when the drops are on at startup
// (HUD_RAIN_ON, ShaderRecord.cpp), so that otherwise the shaders are exactly as they were.
#ifndef HUDRAIN_INCLUDED
#define HUDRAIN_INCLUDED

#ifndef HUD_RAIN
	#define HUD_RAIN 0
#endif
#if HUD_RAIN && defined(INTERIOR_SHADOWS)
	#if INTERIOR_SHADOWS
		#undef HUD_RAIN
		#define HUD_RAIN 0
	#endif
#endif
#if HUD_RAIN

// Per draw (HudRaindropsEffect::SetDrawConstants): x 1 while the drops are on for this draw (0 otherwise, and for every other draw),
// y the drops' threshold (Density), z the drops' clock, w the refraction.
float4 HudRainDraw : register(c221);
// x how far the drops tilt the normal (Reflection), y the drops' gloss, z the wet surface's gloss, w a drop texture tile's size in
// game units (DropSize).
float4 HudRainLook : register(c222);
// x the world's wetness for any other object (AllObjects; 0 for first-person geometry, which takes the camera's own), y how much a
// drop darkens the surface under it (Darkness), z the top-down ortho map's texel size for the cover test (0: no map, counted open).
float4 HudRainWorld : register(c170);

// The world's objects are wet only where they are open to the sky: the top-down ortho map, as Wetness Effects reads it (declared
// there when it is included, else here, on the same registers; not with the sun cross-fade, whose matrices take c146-c166).
#if defined(WETNESS_HLSL) && defined(WETNESS_ORTHO_REG) && defined(PUDDLES_MAP)
	#define HUD_RAIN_COVER 2	// 2026-11-12 Puddles map mode: the puddle map holds the cover (Wetness.hlsl WetnessOcclusion)
#elif defined(WETNESS_HLSL) && defined(WETNESS_ORTHO_REG)
	#define HUD_RAIN_COVER 1
#elif !defined(WETNESS_HLSL) && !(defined(SUN_CROSSFADE) && SUN_CROSSFADE)
	#define HUD_RAIN_COVER 1
row_major float4x4 TESR_ShadowCameraToLightTransformOrtho : register(c153);
// LINEAR, not POINT: "POINT" is a macro (empty) in the point-light passes, which this is declared in.
sampler2D TESR_OrthoMapBuffer : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#else
	#define HUD_RAIN_COVER 0
#endif

// 1 open to the sky, 0 under something; four taps so a roof's edge is a short fade. position: camera-relative, lifted a little so a
// surface does not cover itself.
float HudRainOpenSky(float3 position) {
#if HUD_RAIN_COVER == 2
    return WetnessOcclusion(position + float3(0.0f, 0.0f, 2.0f), float3(0.0f, 0.0f, 1.0f));
#elif HUD_RAIN_COVER
    float texel = HudRainWorld.z;
    [branch] if (texel <= 0.0f) return 1.0f;
    float4 o = mul(float4(position + float3(0.0f, 0.0f, 4.0f), 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    o.xyz /= o.w;
    if (any(abs(o.xy) > 1.0f) || o.z < 0.0f || o.z > 1.0f) return 1.0f;
    float2 uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
    float depth = o.z - 0.0005f;
    float open = 0.0f;
    open += tex2Dlod(TESR_OrthoMapBuffer, float4(uv + float2(-0.5f, -0.5f) * texel, 0.0f, 0.0f)).r > depth ? 0.25f : 0.0f;
    open += tex2Dlod(TESR_OrthoMapBuffer, float4(uv + float2( 0.5f, -0.5f) * texel, 0.0f, 0.0f)).r > depth ? 0.25f : 0.0f;
    open += tex2Dlod(TESR_OrthoMapBuffer, float4(uv + float2(-0.5f,  0.5f) * texel, 0.0f, 0.0f)).r > depth ? 0.25f : 0.0f;
    open += tex2Dlod(TESR_OrthoMapBuffer, float4(uv + float2( 0.5f,  0.5f) * texel, 0.0f, 0.0f)).r > depth ? 0.25f : 0.0f;
    return open;
#else
    return 1.0f;
#endif
}

// MUST stay on ONE line each (ShaderTextureValue::GetSamplerStateString).
sampler2D TESR_HudRainTexture : register(s13) < string ResourceName = "Effects\hud_raindrops.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_HudRainState : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };   // 1 x 1 ("POINT" is a macro in the point-light passes)

static float2 hudRainOffset = 0.0f;   // the texture coordinates' shift under the drops (refraction)
static float2 hudRainSlope = 0.0f;    // the drops' tilt of the normal, tangent space
static float hudRainGloss = 0.0f;     // 0-1: how much glossier the surface is here
static float hudRainStrength = 0.0f;  // 0-1: how much of a drop is here (the parallax shader flattens its relief under it)

// One layer: the drop's slope (xy) and strength (z) at this point of its life. threshold: how big and fresh a drop must be to show
// (Density, raised as the surface dries: see HudRainThreshold).
float3 HudRainLayer(float4 texel, float speed, float threshold) {
    float2 slope = texel.gb * 2.0f - 1.0f;
    float life = 1.0f - frac(texel.a + HudRainDraw.z * speed);
    float strength = saturate(life * texel.r - threshold);
    return float3(slope * strength, strength);
}

// The drops fade with the wetness by shrinking away rather than by thinning: as a surface dries a drop must be bigger and fresher
// to show, so the small ones go first and the last ones shrink to nothing (and, as it starts to rain, the drops grow in the same
// way). density: Density's threshold.
float HudRainThreshold(float density, float wet) {
    return lerp(1.0f, density, saturate(wet * 1.5f));
}

// Once per pixel at the TOP LEVEL (derivatives), BEFORE the material's textures are read: they are then read at HudRainUV(uv).
// position: the camera-relative world position (the drops' size, how far the face looks up, the cover test); valid: false when
// the vertex shader gave none (no drops).
void HudRain(float2 uv, float3 position, bool valid) {
    float2 uvDx = ddx(uv);
    float2 uvDy = ddy(uv);
    float3 posDx = ddx(position);
    float3 posDy = ddy(position);
    float unitsPerUV = (length(posDx) + length(posDy)) / max(length(uvDx) + length(uvDy), 1e-6f);
    // The face's normal, turned toward the camera (the side being drawn); its z: how far it looks up.
    float3 faceN = normalize(cross(posDx, posDy));
    float up = dot(faceN, position) > 0.0f ? -faceN.z : faceN.z;

    [branch] if (HudRainDraw.x > 0.0f && valid) {
        // First-person geometry: the camera's wetness (Effects/HudRaindrops.fx). Any other object: the world's, where this point
        // is open to the sky.
        float wet = 0.0f;
        [branch] if (HudRainWorld.x > 0.0f)
            wet = HudRainWorld.x * HudRainOpenSky(position);
        else
            wet = tex2Dlod(TESR_HudRainState, float4(0.5f, 0.5f, 0.0f, 0.0f)).r;
        [branch] if (wet > 0.0f) {
            // Drop texture tiles per texture unit, in whole powers of two so the drops keep their size from frame to frame.
            float tiles = exp2(round(log2(max(unitsPerUV, 1e-3f) / HudRainLook.w)));
            float2 p = uv * tiles;
            float2 dx = uvDx * tiles;
            float2 dy = uvDy * tiles;
            float threshold = HudRainThreshold(HudRainDraw.y, wet);
            float3 drops = HudRainLayer(tex2Dgrad(TESR_HudRainTexture, p, dx, dy), 0.2f, threshold)
                         + HudRainLayer(tex2Dgrad(TESR_HudRainTexture, (p + float2(0.23f, 0.46f)) * 1.87f, dx * 1.87f, dy * 1.87f), 0.1f, threshold);
            drops *= saturate(up + 0.2f) * saturate(wet * 2.0f);
            drops.xy = clamp(drops.xy, -1.0f, 1.0f);

            hudRainOffset = drops.xy * (HudRainDraw.w / tiles);
            hudRainSlope = drops.xy * HudRainLook.x;
            hudRainGloss = saturate(wet * HudRainLook.z + drops.z * HudRainLook.y);
            hudRainStrength = saturate(drops.z * 2.0f);
        }
    }
}

#define HudRainUV(uv) ((uv) + hudRainOffset)
// A material texture read through the drops: shifted under them, at the unshifted coordinates' mip level (the shift jumps at a
// drop's rim, and its derivatives there would pick a blurry mip).
#define HUD_RAIN_TEX(s, uv) tex2Dgrad(s, (uv) + hudRainOffset, ddx(uv), ddy(uv))

// The albedo under the drops, darkened (Darkness): water soaks into a surface and darkens it.
float3 HudRainAlbedo(float3 albedo) {
    return albedo * (1.0f - HudRainWorld.y * hudRainStrength);
}

// The normal map's normal with the drops' tilt.
float3 HudRainNormal(float3 normalTS) {
    return normalize(normalTS + float3(hudRainSlope, 0.0f));
}

// A roughness made wet: water is smooth.
float HudRainRoughness(float roughness) {
    return lerp(roughness, 0.06f, hudRainGloss);
}

#else
	#define HudRain(uv, position, valid)
	#define HudRainUV(uv) (uv)
	#define HUD_RAIN_TEX(s, uv) tex2D(s, uv)
	#define HudRainAlbedo(a) (a)
	#define HudRainNormal(n) (n)
	#define HudRainRoughness(r) (r)
	static const float hudRainStrength = 0.0f;
	static const float2 hudRainOffset = 0.0f;
#endif

#endif
