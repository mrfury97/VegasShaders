// UNOFFICIAL 2026-11-14 DeferredSun (core/DeferredSun.h; docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.1 / 4.6, docs/next/FD-SUN-RESULTS.md):
// the full deferred renderer's directional light. The lighting-free records (LAMPS_DEFERRED 2, NVR_LIGHT_DEFERRED) compile the sun / moon
// outdoors and the cell's directional light indoors out of every object, parallax and land pixel; this ONE full-screen pass, run where
// DeferredLamps runs (after the main list's G-buffer is resolved, before LinearScene encodes the frame, in linear light) adds exactly that
// light back: the templates' own sun term (Includes/Object.hlsl getSunLightingModel = getSunLighting with its variant chosen per pixel; the
// land's PBR sun of Includes/Terrain.hlsl getSunLighting, see LandSun) times the templates' own forward shadow lookup (Includes/Shadow.hlsl
// GetSunShadow: the cascades with the compiled filter, contact hardening, the character map, the cloud shadows, and RT sun shadows'
// ForwardLookup in place of the cascades where it applies), times 1 - fog (the fog identity of plan 4.1: forward lerp(A + D, F, f) =
// deferred lerp(A, F, f) + D (1 - f)). Loaded like a template (ShaderRecord::LoadShader, the same global define list, so every compiled
// shadow option is the forward shaders' own); every TESR_ constant and sampler is bound by the record's SetCT, by name.
//
// (2026-11-16 fd-sunvis, FD-SUN-RESULTS.md section 4) Given back since: the records write in COLOR3.a's spare fraction (Includes/
// DeferredPixel.hlsl FDSunVisCode) the share of the sun their forward term would have let through besides the cascades (the game's canopy
// PROJ_SHADOW and STBB's 0.85, the parallax / land height self-shadow, the _rmaos micro-shadow) and the highlight's non-metal F0
// (VanillaBridge's authored strength, the _rmaos specular level); this pass multiplies the sun by the one and lights the highlight with the
// other, and rebuilds SpecularFade's highlight fade (PBR pop) from the pixel's distance (the game fades per object). Each by its key in
// [Shaders.ShadowsExteriors.Deferred]. Still not carried: metal's bridged tint (metal takes its colour), the wet film's sun highlight (except on
// the near land with DeferredWetness: DS_WET_FILM below), the land's vanilla (UsePBR off) highlight, parallax's vanilla sun (POM shadows off),
// blended (fading) draws' recorded terms (they write none).

#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
    #if defined(LIN_PREDECODE_ON) && LIN_PREDECODE_ON
        #define LIN_PREDECODED
    #endif
#endif
// The object shaders' opt-ins for the sun lookup (ObjectTemplate.hlsl): the character map looked up here. (2026-11-16 batch D: not the
// clouds' shadow map, SHADOW_CLOUD_IN: the pass looks it up itself, below at DS_CLOUD, so it can leave plants and foliage out per pixel.)
#define SHADOW_CHARACTER_MAP_IN 1
#ifndef FD_SKIN
    #define FD_SKIN 0
#endif
#if FD_SKIN
    // UNOFFICIAL 2026-11-15 DeferredSkin (Includes/SkinDeferred.hlsl; only with [Main.Main.Quality] DeferredSkin at startup): the skin shaders'
    // shadow options (the least penumbra, the reach cap), switched per pixel (Includes/Shadow.hlsl ShadowSkinPixel)
    #define SHADOW_SKIN_SOFTNESS 2
#endif

// UNOFFICIAL 2026-11-16 fd-sunvis (Includes/DeferredPixel.hlsl FDSunVisCode): the pixel's recorded highlight F0 and SpecularFade's fade, set
// per pixel in main before the sun is lit. PBR_SPEC_F0 gives the highlight that F0 (as VanillaBridge's PBR_SPEC_F0 does in the records:
// metal keeps its colour); the diffuse's (1 - F) share keeps the dielectric's 0.04, or less where the record's own was less (an _rmaos map's
// specular level). FD_SUN_SPEC_FADE: Includes/Object.hlsl getSunLightingModel's fade.
static float DSSpecF0 = 0.04f;
static float DSSpecFade = 1.0f;
static float DSClearcoat = 0.0f;
static float DSPearl = 0.0f;
#define PBR_SPEC_F0(baseColor, metal) lerp(DSSpecF0.xxx, (baseColor), (metal))
#define PBR_DIELECTRIC_F0 min(DSSpecF0, 0.04f)
#define FD_SUN_SPEC_FADE DSSpecFade
#define PBR_ROUGH_F90 1	// UNOFFICIAL 2026-11-19 (Finding 4.2): roughness-damped grazing F90 parity with RtPathTrace.comp
// UNOFFICIAL 2026-11-26 DeferredLandRoughF90 ([Shaders.PBR.Main], live; TESR_LinearScales.w = 1 - the key, PBR.cpp): false (default) = the land's
// sun takes the forward land's F90 (saturate(Reflectivity), Includes/Terrain.hlsl), not the damped one, so Reflectivity scales the ground's shine
// as in forward; set in LandSun only, so objects keep the damping. true (or a register never written) = the damped F90 exactly, as in 1.1.1.
static bool DSLandF90 = false;
#define PBR_ROUGH_F90_PICK(damped) (DSLandF90 ? REFLECTIVITY_F90 : (damped))
// D3D9 SM3.0 DeferredSun.hlsl overflows r0..r31 (X4505) when FD_SKIN + FD_FOLIAGE compile with multi-scatter/clearcoat/pearl;
// Vulkan Compute\DeferredSun.comp and forward Object.hlsl always keep full MultiScatter, Clearcoat, and Pearlescence.
#if !(defined(FD_SKIN) && FD_SKIN)
    #define PBR_MULTI_SCATTER 1
    #define PBR_CLEARCOAT_AMT DSClearcoat
    #define PBR_PEARL_AMT DSPearl
#endif

#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"
// UNOFFICIAL 2026-11-26 (bench, upstream 1.1.3 merge): the sun cross-fade's dynamic-caster atlas (Shadow.hlsl TESR_ShadowDynamic, s12 by
// default) clashed with this pass's wet film on s12 (DS_WetFilmMap, bound by stage in core/DeferredSun.h): X4500 in the SunFade record, which
// then fell back without the film's sun highlight. The atlas is bound by name, so it moves to s14 (free in every DeferredSun record).
#ifndef SHADOW_DYNAMIC_SAMPLER_REG
    #define SHADOW_DYNAMIC_SAMPLER_REG s14
#endif
#include "includes/Shadow.hlsl"
#include "includes/DeferredDepth.hlsl"
#include "includes/DeferredPixel.hlsl"
#if FD_SKIN
    #define FD_SKIN_PASS
    #define FD_SKIN_SUN
    #include "includes/SkinDeferred.hlsl"
    // UNOFFICIAL 2026-11-15 SSS v2 (core/DeferredSkin.h): the skin's highlights (the sun's two lobes and the sky reflection, unfogged linear
    // light) into COLOR1, the skin's highlight target, bound only while DeferredSkinSSS 2 runs (an unbound COLOR1 is dropped); the scattering
    // takes them out of the frame before it diffuses and puts them back after, as forward's blur does with its highlight target
    #define DS_SKIN_SPEC_OUT , out float4 skinSpecOut : COLOR1
#else
    #define DS_SKIN_SPEC_OUT
#endif
#ifndef FD_FOLIAGE
    #define FD_FOLIAGE 0
#endif
#if FD_FOLIAGE
    #include "includes/LinearLight.hlsl"
    #include "includes/FoliageDeferred.hlsl"	// UNOFFICIAL 2026-11-14 (FOLIAGE_SUN_CODE; only with [Main.Main.Quality] DeferredFoliage at startup)
#endif

// The pass's own constants (c0-c12: nothing the includes declare lives there).
float4 TESR_DeferredDepthFrame[6] : register(c0);	// Includes/DeferredDepth.hlsl's frame block (DD below), from the depth copy
float4 DS_Sun      : register(c6);	// xyz the direction to the light (world, unit: the game's directional light reversed), w 1 = there is one
float4 DS_SunColor : register(c7);	// rgb the light's colour as the game uploads it to the object shaders (Diff x Dimmer, the stored curve)
float4 DS_Ctrl     : register(c8);	// x DebugView, y 1 = the linear view z of this frame (DeferredDepth 2), z 1 = shadows (0: the Shadows test), w 1 = the frame is linear
float4 DS_Depth    : register(c9);	// x, y the projection's _33, _43 (device depth = x + y / view z), z the share and w the units the surface is pulled nearer
float4 TESR_TerrainData      : register(c10);	// (Includes/Terrain.hlsl) x metallic scale, y roughness scale, z light scale, w ambient scale
float4 TESR_TerrainExtraData : register(c11);	// x use PBR, y saturation
float4 TESR_TerrainParallaxIso : register(c12);	// z ShadowBounce
float4 DS_LandSun   : register(c14);	// (2026-11-15) the land's own light as its pixel shaders got it: xyz SunDir (c18, to the light), w 1 = read back this frame
float4 DS_LandColor : register(c15);	// rgb SunColor (c3, the stored curve), w 1 = read back (core/LandLight.h); else the directional light (DS_Sun / DS_SunColor)
float4 DS_Vis      : register(c16);	// (2026-11-16 fd-sunvis) x 1 = the land's recorded height self-shadow, y 1 = the recorded highlight F0, z SpecularFade's start (game units), w 1 / its range (0: no fade)
float4 DS_Fog      : register(c13);	// (2026-11-14) rgb the fog colour the records lerped with (stored curve), w 1 = the game's fog is drawn: the exact stored-curve add (FDAddFogged) from the frame's copy on s0
// UNOFFICIAL 2026-11-26 SunSheen (core/DeferredSun.h FillSheenConst; ported by hand from upstream 1.1.5-test4, the same register there): x the
// strength (0 off: indoors, at night, the view model, wet weather), y the lobe's roughness (0.45-0.95), z / w upstream 1.1.5's clearcoat and
// pearlescence ([Shaders.PBR.Metal], PBR_CLEARCOAT_AMT / PBR_PEARL_AMT above)
float4 DS_Sheen    : register(c18);
// UNOFFICIAL 2026-11-26 Screen-Space Shadows (core/ScreenSpaceShadows.h, a one-to-one port of Bottled Shaders'): x 1 = this span's mask
// is to be taken (outdoors, made from this span's depth; 0 in the first-person pass). The mask: r 1 lit .. 0 shadowed, at the pixel.
float4 DS_SSS      : register(c19);
sampler2D TESR_ScreenSpaceShadows : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// (2026-11-27 upstream 1.1.5 merge) upstream's MaterialSSS constant (its DS_SSS c17) moved here: c17 is our wet film's (DS_WET_FILM) and the
// name DS_SSS our screen-space shadows' (c19). x 1 = MaterialSSS mode 1-3, y 1 = outdoors (core/DeferredSun.h Apply).
float4 DS_MatSSS   : register(c20);
// (2026-11-27 upstream 1.1.5 merge) SunSheen's standing-water gate (was DS_Sheen.z, now upstream's clearcoat): x 1 = drop the sheen on the
// land's smooth pixels (standing water without the film, DSSheenKeep below)
float4 DS_SheenGate : register(c21);
#define DD TESR_DeferredDepthFrame

// UNOFFICIAL 2026-11-26 SunSheen (upstream 1.1.5-test4's EvalSunSheen, the same maths; Compute/DeferredSun.comp keeps a copy in step): what the
// sun gives a dry, dusty, rough surface beyond the PBR lobe: a very broad GGX glaze (a fixed micro-roughness rSheen, a small grazing Fresnel)
// and an Oren-Nayar-like backscatter lift toward grazing view and the sun behind the viewer, tinted by a desaturated albedo. Metal takes none;
// on records with a highlight it fades in only past roughness 0.18-0.5, so polished surfaces keep their own highlight alone. Unshadowed: the
// caller adds it to the sun's light before the shadow and the recorded share multiply it.
float3 EvalSunSheen(float3 N, float3 V, float3 L, float3 albedo, float3 sunColor, float r, float metal, float isSpecular, float strength, float rSheen) {
    const float dry = saturate(1.0f - metal);
    const float NdotL = saturate(dot(N, L));
    [branch] if (strength <= 0.0001f || dry <= 0.001f || NdotL <= 1.0e-4f) return 0.0f;
    const float NdotV = saturate(dot(N, V));
    const float3 H = normalize(V + L);
    const float NdotH = saturate(dot(N, H));
    const float roughW = isSpecular > 0.5f ? smoothstep(0.18f, 0.50f, r) : 1.0f;
    const float a = rSheen * rSheen;
    const float a2 = a * a;
    const float dDenom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    const float broadD = a2 / max(3.14159265f * dDenom * dDenom, 1.0e-4f);
    const float fGraz = pow(1.0f - NdotV, 3.0f);
    const float fSheen = 0.04f + 0.14f * fGraz;
    const float3 glaze = sunColor * (broadD * fSheen * NdotL * 0.65f);
    const float litGate = smoothstep(0.0f, 0.22f, NdotL);
    const float graze = pow(1.0f - NdotV, 2.5f);
    const float retro = 0.5f + 0.5f * saturate(dot(V, L));
    const float orenLift = (0.28f * NdotL + 0.38f * graze * litGate) * (0.75f + 0.45f * retro);
    const float albLuma = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
    const float3 sheenTint = lerp(albedo, (albLuma * 0.70f + 0.30f).xxx, 0.40f);
    const float3 dustSheen = sunColor * sheenTint * orenLift;
    return (glaze + dustSheen) * (strength * dry * roughW);
}

// The G-buffer (one line each: ShaderTextureValue reads the sampler states to the end of the line).
sampler2D DS_Frame : register(s0);	// (2026-11-14) the frame's copy, linear (DeferredSun::Apply), while DS_Fog.w is 1
sampler2D TESR_AlbedoBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_MaterialBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_CapturedNormalsBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DeferredDepthLinear : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// UNOFFICIAL 2026-11-16 batch D (DEFERRED-GAPS-AUDIT item 22, docs/next/FD-BATCH-D.md): ForwardCloudShadows ([Shaders.VolumetricClouds.Main]
// ForwardShadows, restart; off by default) in this pass. Includes/Shadow.hlsl's cloud term (SHADOW_CLOUD_IN) is the last factor of
// GetSunShadow and multiplies every pixel alike; the forward shaders differ per draw: the object passes of plants get strength 0 with
// PlantShadows off (core/ForwardCloudShadows.h), and grass and the speedtree leaves never take the clouds at all. So the pass declares the
// same map and constant (c169, s8: what Shadow.hlsl declares for SHADOW_CLOUD_IN) and applies CloudSunAt's factor itself, after the lookup,
// to every pixel but those: the foliage records' pixels, and model 5 (plants) while DS_SunColor.w < 0.5.
//   DS_SunColor.w (c7) = 1 the clouds shade plants (PlantShadows on), 0 they do not: core/DeferredSun.h Apply sends PlantShadows there
//   (fd-merged-1116) and c169 as the frame's strength (ForwardCloudShadows::SendFrame, batch B).
// UNOFFICIAL 2026-11-16 fast-restart (testers: grass and bushes glowing in full sun under a cloud's shadow, docs/next/FAST-RESTART.md): with
// ForwardShadows on, the foliage records' pixels (grass, SpeedTree leaves) take the clouds' shadow here too, on the sun light they carry
// (COLOR2.b), as their forward records now do (before, they never took it: full sun under a cloud). DS_SunColor.w: the clouds' strength for
// the foliage (> 0 ForwardShadows on and PlantShadows on; 0 ForwardShadows off: SunFar shades them, as before), -1 PlantShadows off (the
// foliage and model 5 keep their sun, as before). c169.xyz is sent whenever the clouds draw a shadow (ForwardCloudShadows::SendFrame), its w
// only with ForwardShadows on (every other pixel's strength).
#if FORWARD_CLOUD_SHADOWS && FORWARD_SHADOWS
    #define DS_CLOUD 1
float4 NVR_CloudSun : register(c169); // xy: the map's uv under the camera along the light, z: map uv per world unit, w: strength (0 off)
sampler2D TESR_CloudShadow : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// Includes/Shadow.hlsl CloudSunAt, the same expression (keep in step).
float DSCloudSunAt(float3 P, float strength) {	// (2026-11-16 fast-restart: the strength a parameter, the foliage's own or c169.w)
    float3 light = normalize(TESR_SmoothedSunDir.xyz);
    float2 suv = NVR_CloudSun.xy + (P.xy - light.xy * (P.z / max(light.z, 0.25f))) * NVR_CloudSun.z;
    float2 edge = min(suv, 1.0f - suv);
    float inside = saturate(min(edge.x, edge.y) * 10.0f);
    return lerp(1.0f, tex2Dlod(TESR_CloudShadow, float4(suv, 0.0f, 0.0f)).r, inside * strength);
}
#else
    #define DS_CLOUD 0
#endif

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };

// (2026-11-27 upstream 1.1.5 merge) upstream leaves SunSheen and the screen-space shadows out of every sun-fade record (SUN_CROSSFADE_PASS).
// Ours keeps them there (no pop when a fade starts), except in the fade record with DeferredSkin (FD_SKIN), which runs out of temp registers
// with them (X4505, in main too; template_compile with the game's defines): there, as upstream.
#define DS_FADE_LEAN (SUN_CROSSFADE && SUN_CROSSFADE_PASS && FD_SKIN)

#ifndef DS_WET_FILM
    #define DS_WET_FILM 0
#endif
#if DS_WET_FILM
// UNOFFICIAL 2026-11-19 DeferredWetness (core/DeferredWetness.h, Shaders/DeferredWetness.hlsl; only with [Main.Main.Quality] DeferredWetness at
// startup): the near land's water film, as that pass left it in its film target this frame (r round(8 murk) + 0.99 gloss, g roughness, b / a
// the film normal, octahedral). The film's sun highlight is the forward land's (TerrainTemplate.hlsl: WetSpecular x smoothstep(0, 0.2, gloss),
// the land's sun x its light scale), lit here with this pass's shadow and the recorded crack shadow, as the forward land's sunMul.
float4 DS_WetFilm : register(c17);	// x 1 = the film target holds this frame's film (core/DeferredSun.h Apply)
sampler2D DS_WetFilmMap : register(s12);
    #define PUDDLES_NOMAP	// (no puddle map sampler here: only WetSpecular is used)
    #include "includes/Wetness.hlsl"	// WetSpecular (its constants are not read here)
#endif

// UNOFFICIAL 2026-11-26 SunSheen: how much of the dry sheen a pixel keeps (ours, not upstream's: a dusty glow must not sit on wet ground). The
// weather's wetness is already in DS_Sheen.x (core/DeferredSun.h FillSheenConst). Here, per pixel: DeferredWetness's film (wet ground,
// puddles, wet objects) by the weight its own sun highlight takes, smoothstep(0, 0.2, gloss), as 1 - it; and while Puddles' standing water
// is out with no film this frame (DS_SheenGate.x) the land's smooth pixels, where the land record wrote the water's gloss into its roughness.
// Called inside the land and object branches (upstream's places): evaluated after them, the sheen ran the sun-fade record out of temp
// registers (X4505).
float DSSheenKeep(float2 uv, bool land, float r) {
    float keep = land && DS_SheenGate.x > 0.5f ? smoothstep(0.08f, 0.25f, r) : 1.0f;
#if DS_WET_FILM
    [branch] if (DS_WetFilm.x > 0.5f) {
        const float film = tex2Dlod(DS_WetFilmMap, float4(uv, 0.0f, 0.0f)).r;
        keep *= 1.0f - smoothstep(0.0f, 0.2f, saturate((film - floor(film)) / 0.99f));
    }
#endif
    return keep;
}

#ifndef DS_RT_SUN_FIRST
    #define DS_RT_SUN_FIRST 0
#endif
#if DS_RT_SUN_FIRST && RT_SUN_LOOKUP
// UNOFFICIAL 2026-11-15 DeferredRtSunFirst (core/DeferredSun.h; docs/next/FD-GI-RESULTS.md): RT sun shadows as the pass's visibility term.
// GetSunShadow (Includes/Shadow.hlsl) looks the cascades up first and then lerps to the traced value by `weight`, because the templates have
// no temp registers to spare; this pass has. Where weight is 1 (the traced frame saw this point, inside 85 % of the RT range) GetSunShadow's
// result is the traced value alone, so it is returned without the cascades' filter; everywhere else (and in DebugView 15, the coverage view)
// GetSunShadow itself. The same expressions as GetSunShadow's lookup: keep them in step.
float DSSunShadowRtFirst(float3 P, float3 Nw) {
    [branch] if (TESR_RtSunData.x > 0.0f && !TESR_ShadowForwardData.x && TESR_RtSunData.y >= 0.0f) {
        float4 Q = float4(P, 1.0f);
        float w = dot(Q, TESR_RtSunRowW);
        float2 uv = float2(dot(Q, TESR_RtSunRowX), dot(Q, TESR_RtSunRowY)) / max(w, 1.0f);
        float2 texel = tex2Dlod(TESR_RtSunForward, float4(uv, 0.0f, 0.0f)).rg;
        bool seen = w > 1.0f && uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= TESR_RtSunData.z && uv.y <= TESR_RtSunData.w &&
                    abs(texel.g - w) <= abs(TESR_RtSunData.y) * w + 1.0f;
        float weight = seen ? saturate((TESR_RtSunData.x - w) / (0.15f * TESR_RtSunData.x)) : 0.0f;
        [branch] if (weight >= 1.0f)
            return lerp(saturate(texel.r), 1.0f, saturate(TESR_ShadowFade.x));	// (the clouds' share: main's DS_CLOUD, as after GetSunShadow)
    }
    return GetSunShadow(P, Nw);
}
    #define DS_SUN_SHADOW DSSunShadowRtFirst
#else
    #define DS_SUN_SHADOW GetSunShadow
#endif

// The normal the record lit with: GBufferV2's shading normal (COLOR2.g / .a, g 4+ = none), else the older deferred code (the table family's
// and the land's lighting-free records: g oct.x, a 1.01 + 0.49 (oct.y + 1)), else COLOR3's smooth normal. Returns 1 / 0 in `bumped`.
float3 DSShadingNormal(float4 m, float4 normals, out float bumped) {
#if FD_GBUFFER_V2
    bumped = abs(m.g) <= 1.001f ? 1.0f : 0.0f;
    return bumped > 0.5f ? ShadowDecodeNormal(m.ga) : ShadowDecodeNormal(normals.rg);
#else
    bumped = m.a > 1.005f && m.a < 1.995f ? 1.0f : 0.0f;
    return bumped > 0.5f ? ShadowDecodeNormal(float2(m.g, (m.a - 1.01f) / 0.49f - 1.0f)) : ShadowDecodeNormal(normals.rg);
#endif
}

// The land's sun (Includes/Terrain.hlsl getSunLighting's direct part, keep in step: the land cannot share Object.hlsl's register layout):
// UsePBR: PBRSun with the land's metal scale, its roughness (written by the lighting-free land record: (1 - gloss) x RoughnessScale), its
// saturation and light scale; UsePBR off: the game's Lambert (its Blinn highlight needs the layers' gloss power, which the G-buffer lacks).
float3 LandSun(float3 L, float3 sunLin, float3 V, float3 N, float3 albedo, float r) {
    [branch] if (TESR_TerrainExtraData.x) {
        const float3 graded = lerp(luma(albedo), albedo, TESR_TerrainExtraData.y);
        DSLandF90 = TESR_LinearScales.w > 0.5f;	// (2026-11-26 DeferredLandRoughF90 false: the forward land's F90, above)
        return max(0.0f, PBRSun(saturate(TESR_TerrainData.x), r, graded, N, V, L, sunLin * LinScale(TESR_TerrainData.z)));
    }
    return albedo * shades(N, L) * sunLin;
}

// The cascade a point takes (GetSunShadowMaps' choice before its band dither): 0 Near .. 3 Lod, 4 past them.
float DSCascade(float3 P) {
    return length(P - TESR_ShadowNearCenter.xyz) < TESR_ShadowNearCenter.w ? 0.0f : length(P - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w ? 1.0f :
           length(P - TESR_ShadowFarCenter.xyz) < TESR_ShadowFarCenter.w ? 2.0f : length(P - TESR_ShadowLodCenter.xyz) < TESR_ShadowLodCenter.w ? 3.0f : 4.0f;
}

float4 main(PS_IN IN, out float depth : DEPTH DS_SKIN_SPEC_OUT) : COLOR0 {
#if FD_SKIN
    skinSpecOut = 0.0f;
#endif
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 albedo = tex2Dlod(TESR_AlbedoBuffer, uv4);
    const float4 m = tex2Dlod(TESR_MaterialBuffer, uv4);
#if FD_FOLIAGE
    // UNOFFICIAL 2026-11-14 foliage (Includes/FoliageDeferred.hlsl, core/DeferredFoliage.h): a leaf or grass DEFERRED record's pixel carries its
    // sun's unshadowed light in COLOR2.b (FOLIAGE_SUN_CODE + w) under a forward record's COLOR1.a (1): this pass adds A x hue x w x shadow.
    const bool foliage = m.r > 0.5f && m.b > 1.5f && albedo.a > 0.5f && albedo.a < 1.0005f;
    clip(max(FDSunDeferredPixel(albedo.a, m.r), foliage ? 1.0f : 0.0f) - 0.5f);
#else
    clip(FDSunDeferredPixel(albedo.a, m.r) - 0.5f);
#endif
    const float4 normals = tex2Dlod(TESR_CapturedNormalsBuffer, uv4);
    // the skin template drew over this pixel after the deferred draw (its record: 2.05 .. 2.45; ObjectTemplate's skinned gear writes 2.0 and
    // stays), lit forward with its own sun: not this pass's (the skin does not write COLOR1, whose code is the gear's under it)
#if FD_SKIN
    // (DeferredSkin: the deferred skin record writes every target, its COLOR3.a code from 4 up: lit here with the skin's model, below)
    clip(normals.a > 2.02f && normals.a < FD_SKIN_CODE_MIN ? -1.0f : 1.0f);
    const bool skinPx = normals.a > FD_SKIN_CODE_MIN && abs(FDModel(m.r) - 6.0f) < 0.5f;
#else
    clip(2.02f - normals.a);
#endif
#if FD_FOLIAGE
    const float keep = foliage ? FDFoliageKeep(m.g) : FDLightFreeKeep(albedo.a);	// (the foliage records' keep: Includes/DeferredPixel.hlsl FDFoliageKeep)
#else
    const float keep = FDLightFreeKeep(albedo.a);
#endif

    // The position: the exact linear view z (DeferredDepth 2) where this frame has it, else COLOR3's camera distance (a half float).
    const float3 ray = DD_CAMERA_RELATIVE_Z(DD, IN.vpos, 1.0f);	// forward component 1
#if defined(DS_DEPTH_AGREE) && DS_DEPTH_AGREE
    // UNOFFICIAL 2026-11-14 DeferredDistant (core/DeferredDistant.h; DeferredSun.h defines it with the switch on): the exact view z only where it
    // agrees with COLOR3's half-float distance (1 / 512 of it + half a unit: the depth buffer's surface IS the G-buffer's). Where something that
    // writes no G-buffer was drawn nearer (hair; the distant trees over the far land, now in the G-buffer) the depth copy holds THAT surface: lit
    // from it, the depth test below passed and the G-buffer's surface was lit onto it; the G-buffer's own distance keeps the test refusing it.
    const float storedZ = normals.b * rsqrt(dot(ray, ray));
    const float linearZ = DD_LINEAR_VIEW_Z(DD, TESR_DeferredDepthLinear, IN.vpos);
    const float viewZ = DS_Ctrl.y > 0.5f && abs(linearZ - storedZ) <= storedZ * (1.0f / 512.0f) + 0.5f ? linearZ : storedZ;
#else
    const float viewZ = DS_Ctrl.y > 0.5f ? DD_LINEAR_VIEW_Z(DD, TESR_DeferredDepthLinear, IN.vpos) : normals.b * rsqrt(dot(ray, ray));
#endif
    const float3 P = ray * viewZ;
    // Depth-tested at the G-buffer's surface, a little nearer (as DeferredLamps.hlsl): whatever was drawn nearer without writing the
    // G-buffer (hair) is not lit as the surface behind it.
    depth = saturate(DS_Depth.x + DS_Depth.y / max(viewZ * (1.0f - DS_Depth.z) - DS_Depth.w, 1.0f));

    const float3 V = -normalize(P);
    float bumped;
    const float3 N = DSShadingNormal(m, normals, bumped);
#if FD_FOLIAGE
    // UNOFFICIAL 2026-11-14 foliage: the record's unshadowed sun light (Includes/FoliageDeferred.hlsl): A x hue x w (+ hue x h, the grass's
    // highlight, kept in g with the keep), and the lookup's normal offset as the forward foliage shaders take it: the leaf the card's face normal
    // (COLOR3); the grass (g in its range) the world's up with [Shaders.Grass.Main] ShadowFix (DS_Ctrl.w 2), else the tuft's axis (COLOR3; the
    // forward grass took its face normal, which the G-buffer does not hold). One shadow lookup below serves both kinds of pixel.
    const bool grassF = FDFoliageIsGrass(m.g) > 0.5f;
    const float3 Nw = foliage && grassF && DS_Ctrl.w > 1.5f ? float3(0.0f, 0.0f, 1.0f) : ShadowDecodeNormal(normals.rg);
#else
    const float3 Nw = ShadowDecodeNormal(normals.rg);	// the lookup's normal offset: the smooth / face normal the templates pass to GetSunShadow
#endif
    const float model = FDModel(m.r);
    const bool land = abs(model - 4.0f) < 0.5f;
    const float r = GBufferRoughness(m.b);	// (2026-11-16 fd-sunvis: the deferred passes' one decode, Includes/Object.hlsl)
    const float metal = saturate((m.r - floor(m.r)) / 0.49f);
    const float3 L = DS_Sun.xyz;
#ifdef FD_TEST_SUN_COLOR
    const float3 sunLin = LinColor(FD_TEST_SUN_COLOR) * DS_Sun.w;	// TEST ONLY (frame replay A/B, FD-SUN-RESULTS.md section 13): never defined by the plugin
#else
    const float3 sunLin = LinColor(DS_SunColor.rgb) * DS_Sun.w;
#endif
    // the land with ITS light (batch 3: the land's shaders get their own SunDir / SunColor, not the directional light the objects get)
#ifdef FD_TEST_LAND_COLOR
    // TEST ONLY (tests/frame_replay A/B of a recorded frame, FD-SUN-RESULTS.md section 11): the land's light given at compile time, as a
    // bench log measured it (the recorded frame's c14 / c15 hold the fallback). Never defined by the plugin.
    const float3 Lland = L;
    const float3 sunLand = LinColor(FD_TEST_LAND_COLOR);
#else
    const float3 Lland = DS_LandSun.w > 0.5f ? DS_LandSun.xyz : L;
    const float3 sunLand = DS_LandColor.w > 0.5f ? LinColor(DS_LandColor.rgb) : sunLin;
#endif

    // UNOFFICIAL 2026-11-16 fd-sunvis (Includes/DeferredPixel.hlsl FDSunVisDecode): what the record recorded of its forward sun besides the
    // cascades: the share of the sun that reaches (objects / parallax: the canopy shadow, the parallax self-shadow, the micro-shadow, each by
    // its key in the record; the land: its height self-shadow, by LandCrackShadow here) and the highlight's F0 (AuthoredHighlight). The
    // highlight's fade: SpecularFade's band (the game's fSpecularLODStartFade .. fSpecularLODEnd) from this pixel's distance (the game fades
    // per object, from the object's; DS_Vis.w 0 = no fade).
    float sunVis, specF0;
    FDSunVisDecode(normals.a, sunVis, specF0);
    sunVis = land && DS_Vis.x < 0.5f ? 1.0f : sunVis;
    DSSpecF0 = DS_Vis.y > 0.5f && !land ? specF0 : 0.04f;
    DSSpecFade = DS_Vis.w > 0.0f ? saturate(1.0f - (length(P) - DS_Vis.z) * DS_Vis.w) : 1.0f;

    float sssAmbScalar = 0.0f;
    float3 light;
#if FD_SKIN
    // UNOFFICIAL 2026-11-15 DeferredSkin: the lookup with the skin's options, whenever the sun is up (the skin's light wraps past the terminator);
    // the skin's light after it (its shadow is inside its model: the penumbra's scatter, the self-shadow's escape)
    [branch] if (skinPx) { ShadowSkinPixel = 1.0f; ShadowMinPenumbra = TESR_ShadowForwardData.z; light = sunLin; }
    else {
#endif
#if FD_FOLIAGE
    [branch] if (foliage) {
        const float3 hueF = sunLin / max(dot(sunLin, float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-8f);
        light = albedo.rgb * hueF * FoliageSunWeightOf(m.b) + (grassF ? hueF * FoliageHighlightOf(FDFoliageHighlightEnc(m.g)) : 0.0f);
    }
    else
#endif
    [branch] if (land) {
        DSClearcoat = 0.0f;
        DSPearl = 0.0f;
        light = LandSun(Lland, sunLand, V, N, albedo.rgb, r);
#if !DS_FADE_LEAN
        [branch] if (DS_Sheen.x > 0.0001f) light += DSSheenKeep(IN.uv, true, r) * EvalSunSheen(N, V, Lland, albedo.rgb, sunLand * (TESR_TerrainExtraData.x ? LinScale(TESR_TerrainData.z) : 1.0f), r, saturate(TESR_TerrainData.x), 0.0f, DS_Sheen.x, DS_Sheen.y);
#endif
    }
    else {
        SetDiffuseEnergyKeep(N, V, r);	// (the object shaders' EnergySplit; the land does not take it)
        const float isSpec = FDSunSpecular(albedo.a, m.r);
        DSClearcoat = saturate(DS_Sheen.z);	// (upstream 1.1.5: clearcoat / pearlescence; 2026-11-27 merge: our puddle gate moved to DS_SheenGate)
        DSPearl = saturate(DS_Sheen.w);
        light = getSunLightingModel(isSpec, L, sunLin, V, N, albedo.rgb, r, metal);
#if !DS_FADE_LEAN
        [branch] if (DS_Sheen.x > 0.0001f) light += DSSheenKeep(IN.uv, false, r) * EvalSunSheen(N, V, L, albedo.rgb, sunLin * LIN_SCALE_LIGHT, r, metal, isSpec, DS_Sheen.x, DS_Sheen.y);
#endif
    }
#if DS_WET_FILM
    // (2026-11-19 / 2026-11-20 step 2) the wet film's sun highlight where DeferredWetness wrote a film this frame (the near land, objects and
    // parallax; 0 everywhere else): the land with its own sun x its light scale (TerrainTemplate.hlsl), objects with the sun x LIN_SCALE_LIGHT
    // (ObjectTemplate.hlsl ObjectWetFilm, ParallaxTemplate.hlsl); x the shadow and the recorded share below, as forward's sunAndProj / sunMul
    [branch] if (DS_WetFilm.x > 0.5f) {
        const float4 film = tex2Dlod(DS_WetFilmMap, uv4);
        const float gloss = saturate((film.r - floor(film.r)) / 0.99f);
        [branch] if (gloss > 0.001f)
            light += WetSpecular(ShadowDecodeNormal(film.ba), V, land ? Lland : L, land ? sunLand * LinScale(TESR_TerrainData.z) : sunLin * LIN_SCALE_LIGHT,
                film.g) * smoothstep(0.0f, 0.2f, gloss);
    }
#endif
    #if !(SUN_CROSSFADE && SUN_CROSSFADE_PASS)
    // UNOFFICIAL 2026-11-25 MaterialSSS Phase 2: direct/transmitted terms fold before shadow lookup (exact cascade+sunVis gating, minimal live regs).
    [branch] if (DS_MatSSS.x > 0.5f && abs(model - 8.0f) > 0.5f) {
#if FD_FOLIAGE
        const bool sssFol = foliage, sssGrass = grassF;
#else
        const bool sssFol = false, sssGrass = false;
#endif
        if (!sssFol && !sssGrass && metal < 0.25f && r > 0.42f) {
            const float nmr = (1.0f - metal) * smoothstep(0.42f, 0.88f, r);
            light += albedo.rgb * (land ? sunLand : sunLin) * (r * r * 0.20f * (0.35f + 0.65f * (1.0f - saturate(dot(N, V)))) * max(dot(N, land ? Lland : L), 0.0f) * nmr);
        }
        if (DS_MatSSS.y > 0.5f && L.z > 0.02f && metal < 0.35f) {
            const float gW = saturate(0.56f - 0.56f * N.z) * smoothstep(0.02f, 0.28f, L.z) * (land ? 0.35f : 1.0f) * ((sssFol || sssGrass) ? 0.65f : lerp(0.25f, 1.0f, smoothstep(0.35f, 0.85f, r)));
            const float bs = dot(land ? sunLand : sunLin, float3(0.2126f, 0.7152f, 0.0722f)) * 0.36f * gW * (1.0f - metal);
            light += albedo.rgb * float3(0.36f, 0.25f, 0.14f) * (bs * 0.70f);
            sssAmbScalar = bs * 0.30f * (sssFol ? 1.0f : sunVis);
        }
        if (abs(model - 3.0f) < 0.5f) {
            light += lerp(float3(1.0f, 1.0f, 1.0f), albedo.rgb, 0.55f) * sunLin * (0.12f * pow(1.0f - saturate(dot(N, V)), 3.2f) * saturate(dot(Nw, L) * 0.65f + 0.35f));
        }
        const bool isPlant = (sssFol && !sssGrass) || abs(model - 5.0f) < 0.5f;
        const bool isGrass = sssGrass || abs(model - 7.0f) < 0.5f;
        if ((isPlant || isGrass || abs(model - 3.0f) < 0.5f) && DS_MatSSS.y > 0.5f && L.z > 0.02f) {
            const float gnl = dot(Nw, L);
            const float ww = isPlant ? 0.32f : isGrass ? 0.28f : 0.22f;
            const float bp = pow(saturate(dot(-V, normalize(L + Nw * (abs(model - 3.0f) < 0.5f ? 0.45f : 0.35f)))), isPlant ? 3.2f : 3.0f);
            const float tt = (0.34f * max(0.0f, saturate((gnl + ww) / (1.0f + ww)) - saturate(gnl)) + bp * saturate(0.65f - 0.35f * gnl)) * (isPlant ? 0.85f : isGrass ? 0.70f : 0.28f);
            const float3 st = isPlant ? float3(1.15f, 1.32f, 0.45f) : isGrass ? float3(1.22f, 1.16f, 0.52f) : float3(1.05f, 0.98f, 0.88f);
            light += albedo.rgb * st * sunLin * tt;
        } else if (model > 10.5f && model < 14.5f) {
            const float gnl = dot(Nw, L);
            const float ww = abs(model - 12.0f) < 0.5f ? 0.18f : 0.32f;
            const float bp = pow(saturate(dot(-V, normalize(L + Nw * 0.35f))), 3.0f);
            const float tt = (0.34f * max(0.0f, saturate((gnl + ww) / (1.0f + ww)) - saturate(gnl)) + bp * saturate(0.65f - 0.35f * gnl)) * 0.44f;
            const float3 st = (model < 11.5f) ? float3(1.22f, 0.58f, 0.20f) : (model < 12.5f) ? float3(0.95f, 0.90f, 0.82f) : (model < 13.5f) ? float3(0.58f, 0.86f, 1.22f) : float3(1.22f, 0.36f, 0.16f);
            light += albedo.rgb * st * sunLin * tt;
        }
    }
    #endif
#if FD_SKIN
    }
#endif
    // the lookup only where the light reaches (UnlitShadowSkip's rule: exactly 0 there whatever the shadow)
    float shadow = 1.0f;
    // (2026-11-15 batch 4, frame replay of og-ground: the land came out shadowed in triangles and blobs where the forward land shader, with
    // its exact interpolated position, found no shadow: self-shadowing acne. The pass's position (the linear view z, or the half-float
    // distance) is off by up to ~6.5e-4 of the distance (FD-PHASE0 Q2), against a PCF depth bias of a fraction of a unit. The lookup's point
    // is pulled 1 / 1024 of its distance toward the camera: the replayed frame then matches forward (FD-SUN-RESULTS.md section 11).)
    // (2026-11-15 batch 6, the Goodsprings and desert-near recordings: a dark band across the ground at mid distance, the same acne further
    // out: the pull ramps to 1 / 512 between 256 and 1,024 units, which removed it there; FD-SUN-RESULTS.md section 13.)
    // (2026-11-15 fd-sun-fsr, docs/next/FD-SUN-FSR-RESULTS.md: the cause was the position's half-pixel offset (Includes/DeferredDepth.hlsl
    // DDRay), not its precision; the pull hid it at 1080p and not under FSR's RenderScale 0.67 (black triangles on the land). With the
    // offset gone the replayed og-ground, Goodsprings and desert-near frames match forward as well without any pull as with it: removed.)
    [branch] if (DS_Ctrl.z > 0.5f && dot(light, 1.0f) > 0.0f) {
        // (2026-11-16 fd-roofedge, docs/next/FD-ROOFEDGE.md) the lookup with the normal the forward shaders give it: the smooth normal faced to
        // the camera by the view test (Includes/Shadow.hlsl ShadowFacingNormal, as their faceNormal). The object and parallax records now store
        // it faced by the triangle's side (Includes/CapturedNormals.hlsl CapturedNormalOutTri), which lights a ridge seen from below correctly;
        // the shadow lookup keeps forward's normal offset, or at a low sun a slope facing away from it took light under its ridge that forward
        // shadows. Every other record's stored normal is already view-faced (no change); the grass's world up stays as it was.
#if FD_FOLIAGE
        shadow = DS_SUN_SHADOW(P, foliage ? Nw : ShadowFacingNormal(Nw, P));	// (2026-11-15 DS_SUN_SHADOW: GetSunShadow, or DeferredRtSunFirst's above)
#else
        shadow = DS_SUN_SHADOW(P, ShadowFacingNormal(Nw, P));	// (2026-11-15 DS_SUN_SHADOW: GetSunShadow, or DeferredRtSunFirst's above)
#endif
#if DS_CLOUD
        // (2026-11-16 batch D) the clouds' share, where GetSunShadow put it (its last factor), but not on what forward kept out of it
        // (2026-11-16 fast-restart) the foliage records' pixels: DS_SunColor.w (their own strength, ForwardShadows on; -1 PlantShadows off);
        // model 5 (plants) with PlantShadows off: none; every other pixel: c169.w (ForwardShadows on)
        const float cloudPlant = abs(model - 5.0f) < 0.5f && DS_SunColor.w < 0.0f ? 0.0f : NVR_CloudSun.w;
    #if FD_FOLIAGE
        const float cloudStrength = foliage ? max(DS_SunColor.w, 0.0f) : cloudPlant;
    #else
        const float cloudStrength = cloudPlant;
    #endif
        [branch] if (cloudStrength > 0.0f) shadow *= DSCloudSunAt(P, cloudStrength);
#endif
        // UNOFFICIAL 2026-11-26 Screen-Space Shadows (Bottled Shaders' Lighting.hlsl, DEFERRED: outdoors, dirLightAngle >= 0, the sun's
        // shadow times the mask at the pixel; their Load at the pixel = this point read at the pixel's centre)
#if !DS_FADE_LEAN
        [branch] if (DS_SSS.x > 0.5f && dot(N, L) >= 0.0f) shadow *= tex2Dlod(TESR_ScreenSpaceShadows, DDUv(IN.vpos, DD[5])).r;
#endif
    }
#if FD_SKIN
    [branch] if (skinPx) {	// SkinTemplate.hlsl's sun: SkinLight(PBRLight(the sun), the shadow) + its highlight, in world space
        FDSkinSetup(normals.a, m.b);
        float3 skinSpecular;
        light = FDSkinLight(albedo.rgb, N, Nw, V, L, sunLin * LIN_SCALE_LIGHT, shadow, true, skinSpecular);
        light += skinSpecular;
        light += FDSkinSunTransmission(P, Nw, V, L, albedo.rgb, sunLin * LIN_SCALE_LIGHT);	// (SSS v2 Transmission; 0 without it)
        skinSpecOut = float4(skinSpecular + SkinSkyReflection(V, Nw, N), 1.0f);	// (the sky reflection is in the record's colour, COLOR0: its share here)
    }
    else
#endif
#if FD_FOLIAGE
    { light *= shadow * (foliage ? 1.0f : sunVis); light += albedo.rgb * float3(0.36f, 0.25f, 0.14f) * sssAmbScalar; }	// (2026-11-16 fd-sunvis)
#else
    { light *= shadow * sunVis; light += albedo.rgb * float3(0.36f, 0.25f, 0.14f) * sssAmbScalar; }	// (2026-11-16 fd-sunvis)
#endif
    // the land's ShadowBounce (TerrainTemplate.hlsl): the sun its relief took, back x its colour, outside cast shadows. (2026-11-16 fd-sunvis)
    // the height self-shadow (crackShadow) is the recorded share (1 with LandCrackShadow off), as the land record's reliefLoss takes it.
    // (2026-11-16 batch D, DEFERRED-GAPS-AUDIT item 11) not on the far land (Includes/DeferredPixel.hlsl FDFarLand): its forward shader
    // (TerrainLODTemplate.hlsl) had no ShadowBounce, and slopes facing away from a low sun came out brighter and flatter than forward.
    [branch] if (land && TESR_TerrainParallaxIso.z > 0.0f && FDFarLand(m.r) < 0.5f) {
        const float reliefLoss = max(0.0f, saturate(dot(Nw, Lland)) - sunVis * saturate(dot(N, Lland)));
        light += sunLand * LinScale(TESR_TerrainData.z) * albedo.rgb * (reliefLoss * shadow * TESR_TerrainParallaxIso.z);
    }

    // DebugView (written over the frame, blend off: DeferredSun::Apply): 1 the sun alone x (1 - fog), 2 the shadow term, 3 the cascade (red
    // Near, green Middle, blue Far, white Lod, black past), 4 the fog factor (1 - fog), 5 the normal used, 6 the model (red PBRSun, green
    // PBRDiffuse, blue the land; brighter where the normal is the bumped one), 7 the position (64-unit checker x distance / 8192), 8 (2026-11-16
    // fd-sunvis) the recorded sun share (white = reaches) in red / green, the recorded F0 / 0.08 in blue
    [branch] if (DS_Ctrl.x > 0.5f) {
        const float v = DS_Ctrl.x;
        float3 dbg;
        if (v < 1.5f) dbg = light * keep;
        else if (v < 2.5f) dbg = shadow.xxx;
        else if (v < 3.5f) { const float c = DSCascade(P); dbg = c < 0.5f ? float3(1, 0, 0) : c < 1.5f ? float3(0, 1, 0) : c < 2.5f ? float3(0, 0, 1) : c < 3.5f ? float3(1, 1, 1) : float3(0, 0, 0); }
        else if (v < 4.5f) dbg = keep.xxx;
        else if (v < 5.5f) dbg = N * 0.5f + 0.5f;
        else if (v < 6.5f) dbg = (land ? float3(0, 0, 1) : FDSunSpecular(albedo.a, m.r) > 0.5f ? float3(1, 0, 0) : float3(0, 1, 0)) * (0.5f + 0.5f * bumped);
        else if (v < 7.5f) { const float3 cell = floor((P + DD[4].xyz) / 64.0f); dbg = float3(frac((cell.x + cell.y + cell.z) * 0.5f) * 0.6f + 0.2f, saturate(viewZ / 8192.0f), 0.0f); }
        else dbg = float3(sunVis, sunVis, saturate(DSSpecF0 / 0.08f));
        return float4(dbg, 1.0f);
    }
    // linear frame: ONE / ONE add. With the game's fog drawn, what makes the pixel exactly the forward stored-curve fog lerp
    // (Includes/DeferredPixel.hlsl FDAddFogged; the plain light x keep is exact only without fog). The foliage records' keep (FDFoliageKeep)
    // takes the same road (2026-11-14: their unfogged light, their colour fogged with exactly that keep).
#ifdef LINEAR_LIGHTING
    [branch] if (DS_Fog.w > 0.5f && keep < 0.9999f) return float4(FDAddFogged(tex2Dlod(DS_Frame, uv4).rgb, light, keep, DS_Fog.rgb, TESR_LinearLighting.x, 1.0f), 0.0f);
#endif
    return float4(light * keep, 0.0f);
}
