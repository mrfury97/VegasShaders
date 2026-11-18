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
// [Shaders.ShadowsExteriors.Deferred]. Still not carried: metal's bridged tint (metal takes its colour), the wet film's sun highlight, the
// land's vanilla (UsePBR off) highlight, parallax's vanilla sun (POM shadows off), blended (fading) draws' recorded terms (they write none).

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
#define PBR_SPEC_F0(baseColor, metal) lerp(DSSpecF0.xxx, (baseColor), (metal))
#define PBR_DIELECTRIC_F0 min(DSSpecF0, 0.04f)
#define FD_SUN_SPEC_FADE DSSpecFade

#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"
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
#define DD TESR_DeferredDepthFrame

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
#if FORWARD_CLOUD_SHADOWS && FORWARD_SHADOWS
    #define DS_CLOUD 1
float4 NVR_CloudSun : register(c169); // xy: the map's uv under the camera along the light, z: map uv per world unit, w: strength (0 off)
sampler2D TESR_CloudShadow : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// Includes/Shadow.hlsl CloudSunAt, the same expression (keep in step).
float DSCloudSunAt(float3 P) {
    float3 light = normalize(TESR_SmoothedSunDir.xyz);
    float2 suv = NVR_CloudSun.xy + (P.xy - light.xy * (P.z / max(light.z, 0.25f))) * NVR_CloudSun.z;
    float2 edge = min(suv, 1.0f - suv);
    float inside = saturate(min(edge.x, edge.y) * 10.0f);
    return lerp(1.0f, tex2Dlod(TESR_CloudShadow, float4(suv, 0.0f, 0.0f)).r, inside * NVR_CloudSun.w);
}
#else
    #define DS_CLOUD 0
#endif

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };

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

    float3 light;
#if FD_SKIN
    // UNOFFICIAL 2026-11-15 DeferredSkin: the lookup with the skin's options, whenever the sun is up (the skin's light wraps past the terminator);
    // the skin's light after it (its shadow is inside its model: the penumbra's scatter, the self-shadow's escape)
    [branch] if (skinPx) { ShadowSkinPixel = 1.0f; ShadowMinPenumbra = TESR_ShadowForwardData.z; light = sunLin; }
    else
#endif
#if FD_FOLIAGE
    [branch] if (foliage) {
        const float3 hueF = sunLin / max(dot(sunLin, float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-8f);
        light = albedo.rgb * hueF * FoliageSunWeightOf(m.b) + (grassF ? hueF * FoliageHighlightOf(FDFoliageHighlightEnc(m.g)) : 0.0f);
    }
    else
#endif
    [branch] if (land) light = LandSun(Lland, sunLand, V, N, albedo.rgb, r);
    else {
        SetDiffuseEnergyKeep(N, V, r);	// (the object shaders' EnergySplit; the land does not take it)
        light = getSunLightingModel(FDSunSpecular(albedo.a, m.r), L, sunLin, V, N, albedo.rgb, r, metal);
    }
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
    #if FD_FOLIAGE
        const bool cloudless = foliage || (abs(model - 5.0f) < 0.5f && DS_SunColor.w < 0.5f);
    #else
        const bool cloudless = abs(model - 5.0f) < 0.5f && DS_SunColor.w < 0.5f;
    #endif
        [branch] if (NVR_CloudSun.w > 0.0f && !cloudless) shadow *= DSCloudSunAt(P);
#endif
    }
#if FD_SKIN
    [branch] if (skinPx) {	// SkinTemplate.hlsl's sun: SkinLight(PBRLight(the sun), the shadow) + its highlight, in world space
        FDSkinSetup(normals.a, m.b);
        float3 skinSpecular;
        light = FDSkinLight(albedo.rgb, N, Nw, V, L, sunLin * LIN_SCALE_LIGHT, shadow, true, skinSpecular);
        light += skinSpecular;
        light += FDSkinSunTransmission(P, Nw, L, albedo.rgb, sunLin * LIN_SCALE_LIGHT);	// (SSS v2 Transmission; 0 without it)
        skinSpecOut = float4(skinSpecular + SkinSkyReflection(V, Nw, N), 1.0f);	// (the sky reflection is in the record's colour, COLOR0: its share here)
    }
    else
#endif
#if FD_FOLIAGE
    light *= shadow * (foliage ? 1.0f : sunVis);	// (2026-11-16 fd-sunvis: the recorded share, 1 where nothing was recorded)
#else
    light *= shadow * sunVis;	// (2026-11-16 fd-sunvis: the recorded share, 1 where nothing was recorded)
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
