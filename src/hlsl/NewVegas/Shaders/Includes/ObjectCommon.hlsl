// Template for object lighting shaders.
//
// VS
//
// AD
// SLS2028 - LIGHTS = 2 (ONLY_LIGHT)
// SLS2029 - LIGHTS = 2, SKIN (ONLY_LIGHT)
// SLS2030 - LIGHTS = 2, PROJ_SHADOW (ONLY_LIGHT)
// SLS2031 - LIGHTS = 2, PROJ_SHADOW, SKIN (ONLY_LIGHT)
// SLS2032 - LIGHTS = 3 (ONLY_LIGHT)
// SLS2033 - LIGHTS = 3, SKIN (ONLY_LIGHT)
// SLS2034 - LIGHTS = 3, PROJ_SHADOW (ONLY_LIGHT)
// SLS2035 - LIGHTS = 3, PROJ_SHADOW, SKIN (ONLY_LIGHT)
//
// ADTS
// SLS2000 - Base
// SLS2001 - LOD
// SLS2003 - SKIN
// SLS2004 - PROJ_SHADOW
// SLS2006 - PROJ_SHADOW, SKIN
// SLS2007 - STBB
// SLS2008 - LIGHTS = 2
// SLS2009 - LIGHTS = 2, SKIN
// SLS2010 - LIGHTS = 2, PROJ_SHADOW
// SLS2011 - LIGHTS = 2, PROJ_SHADOW, SKIN
// SLS2012 - SPECULAR
// SLS2013 - SPECULAR, SKIN
// SLS2014 - SPECULAR, PROJ_SHADOW
// SLS2015 - SPECULAR, PROJ_SHADOW, SKIN
// SLS2016 - SPECULAR, LIGHTS = 2
// SLS2017 - SPECULAR, LIGHTS = 2, SKIN
// SLS2018 - SPECULAR, LIGHTS = 2, PROJ_SHADOW
// SLS2019 - SPECULAR, LIGHTS = 2, PROJ_SHADOW, SKIN
// (ignored) SLS2102 - LIGHTS = 0
//
// ADTS10
// SLS2020 - LIGHTS = 9
// SLS2021 - LIGHTS = 9, SKIN
// SLS2022 - LIGHTS = 4
// SLS2023 - LIGHTS = 4, OPT
// SLS2024 - LIGHTS = 4, SKIN
// SLS2025 - LIGHTS = 4, SPECULAR
// SLS2026 - LIGHTS = 4, SPECULAR, OPT
// SLS2027 - LIGHTS = 4, SPECULAR, SKIN
//
// DiffusePt
// SLS2036 - LIGHTS = 2 (DIFFUSE)
// SLS2037 - LIGHTS = 2, SKIN (DIFFUSE)
// SLS2038 - LIGHTS = 3 (DIFFUSE)
// SLS2039 - LIGHTS = 3, SKIN (DIFFUSE)
//
// Specular
// SLS2040 - Base (ONLY_SPECULAR)
// SLS2041 - SKIN (ONLY_SPECULAR)
// SLS2042 - PROJ_SHADOW (ONLY_SPECULAR)
// SLS2043 - PROJ_SHADOW, SKIN (ONLY_SPECULAR)
// SLS2044 - POINT (ONLY_SPECULAR)
// SLS2045 - POINT, SKIN (ONLY_SPECULAR)
// SLS2046 - POINT, NUM_PT_LIGHTS = 2 (ONLY_SPECULAR)
// SLS2047 - POINT, NUM_PT_LIGHTS = 2, SKIN (ONLY_SPECULAR)
// SLS2048 - POINT, NUM_PT_LIGHTS = 3 (ONLY_SPECULAR)
// SLS2049 - POINT, NUM_PT_LIGHTS = 3, SKIN (ONLY_SPECULAR)
//
// PS
//
// AD
// SLS2037 - LIGHTS = 2 (ONLY_LIGHT, OPT)
// SLS2038 - LIGHTS = 2, SI (ONLY_LIGHT, OPT)
// SLS2039 - LIGHTS = 2, PROJ_SHADOW (ONLY_LIGHT, OPT)
// SLS2040 - LIGHTS = 2, SI, PROJ_SHADOW (ONLY_LIGHT, OPT)
// SLS2041 - LIGHTS = 3 (ONLY_LIGHT, OPT)
// SLS2042 - LIGHTS = 3, SI (ONLY_LIGHT, OPT)
// SLS2043 - LIGHTS = 3, PROJ_SHADOW (ONLY_LIGHT, OPT)
// SLS2044 - LIGHTS = 3, SI, PROJ_SHADOW (ONLY_LIGHT, OPT)
//
// ADTS
// SLS2000 - Default
// SLS2001 - OPT
// SLS2002 - OPT, LOD
// SLS2004 - SI 
// SLS2005 - PROJ_SHADOW
// SLS2007 - SI, PROJ_SHADOW 
// SLS2008 - STBB
// SLS2009 - HAIR
// SLS2010 - HAIR, PROJ_SHADOW
// SLS2011 - LIGHTS = 2
// SLS2012 - LIGHTS = 2, SI
// SLS2013 - LIGHTS = 2, HAIR
// SLS2014 - LIGHTS = 2, PROJ_SHADOW
// SLS2015 - LIGHTS = 2, SI, PROJ_SHADOW
// SLS2016 - LIGHTS = 2, HAIR, PROJ_SHADOW
// SLS2017 - SPECULAR
// SLS2018 - SPECULAR, SI
// SLS2019 - SPECULAR, HAIR
// SLS2020 - SPECULAR, PROJ_SHADOW
// SLS2021 - SPECULAR, SI, PROJ_SHADOW
// SLS2022 - SPECULAR, HAIR, PROJ_SHADOW
// SLS2023 - SPECULAR, LIGHTS = 2
// SLS2024 - SPECULAR, LIGHTS = 2, SI
// SLS2026 - SPECULAR, LIGHTS = 2, PROJ_SHADOW
// SLS2027 - SPECULAR, LIGHTS = 2, SI, PROJ_SHADOW
// (ignored) SLS2151 - LIGHTS = 0, SILHOUETTE
//
// ADTS10
// SLS2029 - LIGHTS = 9
// SLS2030 - LIGHTS = 9, SI
// SLS2031 - LIGHTS = 4
// SLS2032 - LIGHTS = 4, OPT 
// SLS2033 - LIGHTS = 4, SI
// SLS2034 - LIGHTS = 4, SPECULAR
// SLS2035 - LIGHTS = 4, SPECULAR, OPT
// SLS2036 - LIGHTS = 4, SPECULAR, SI
//
// DiffusePt
// SLS2045 - LIGHTS = 2 (DIFFUSE)
// SLS2046 - LIGHTS = 3 (DIFFUSE)
//
// Specular
// SLS2047 - Base (ONLY_SPECULAR)
// SLS2048 - HAIR (ONLY_SPECULAR)
// SLS2049 - PROJ_SHADOW (ONLY_SPECULAR)
// SLS2050 - PROJ_SHADOW, HAIR (ONLY_SPECULAR)
// SLS2051 - POINT (ONLY_SPECULAR)
// SLS2052 - POINT, HAIR (ONLY_SPECULAR)
// SLS2053 - NUM_PT_LIGHTS = 2, POINT (ONLY_SPECULAR)
// SLS2054 - NUM_PT_LIGHTS = 2, POINT, HAIR (ONLY_SPECULAR)
// SLS2055 - NUM_PT_LIGHTS = 3, POINT (ONLY_SPECULAR)
// SLS2056 - NUM_PT_LIGHTS = 3, POINT, HAIR (ONLY_SPECULAR)

#ifdef __INTELLISENSE__	// editor preview: a 2-lamp extra-lamp vertex shader
    #define VS
    #define DIFFUSE
    #define LIGHTS 2
#endif

// The light-only passes (DIFFUSE the extra lamps, ONLY_SPECULAR the highlights) draw light alone: the texture pass multiplies the
// colour in and applies fog and vertex colour. The extra-lamp passes use the game's OPT layout (no Toggles).
#if defined(DIFFUSE) || defined(ONLY_SPECULAR)
    #define ONLY_LIGHT
#endif
#ifdef DIFFUSE
    #define OPT
#endif
#ifdef ONLY_SPECULAR
    #define SPECULAR
#endif

#ifdef ONLY_LIGHT
    #define NO_FOG	// (not in a light-only pass)
    #define NO_VERTEX_COLOR	// (likewise)
#else
    // UNOFFICIAL: metal (Object.hlsl getDerivedMetallic) in the passes that light an object fully at once (colour, the
    // lights and the ambient) with a highlight: METAL_UP, the vertex shaders send world up for its reflection; METAL,
    // the pixel shaders use it. Not on objects without specular (metal without a highlight is only darker), nor hair
    // (grey hair is no metal); the lighting-only and specular-only passes stay as they were.
    #define METAL_UP
    #if defined(SPECULAR) && !defined(HAIR)
        #define METAL
    #endif
#endif

// Variant shorthands, worked out once from the defines the shader is built with.
#if LIGHTS > 1 || NUM_PT_LIGHTS > 1
    #define OBJ_LAMP2	// a second light vector is interpolated
#endif
#if LIGHTS > 2 || NUM_PT_LIGHTS > 2
    #define OBJ_LAMP3	// and a third
#endif
#if defined(DIFFUSE) || defined(POINT)
    #define OBJ_LAMP_FIRST	// light 0 is a lamp (extra-lamp and point passes), not the sun
#endif
#if !defined(DIFFUSE) && !defined(ONLY_SPECULAR)
    #define OBJ_AMBIENT	// the pass adds the ambient (not the extra-lamp or the highlight-only passes)
#endif
#if defined(ONLY_LIGHT) && defined(OBJ_AMBIENT)
    #define OBJ_AD_PASS	// the light-only first pass (AD): lit later by the texture pass's multiply
#endif
#ifndef OPT
    #define OBJ_TOGGLES	// the game's non-OPT variants read Toggles (c27)
#endif
#ifndef NO_FOG
    #define OBJ_FOG
#endif
#ifndef NO_VERTEX_COLOR
    #define OBJ_VCOLOR
#endif

// UNOFFICIAL 2026-10-21 FoldHighlights ([Shaders.PBR.Main] FoldHighlights, restart: FOLD_HIGHLIGHTS_ON from ShaderRecord::BuildDefines; per
// draw NVR_SpecFade.y, core/FoldHighlights.h; bench\deep-roadmap\evidence\pbrtiles\LOG.md). A shiny object lit by 3 or more lamps is
// drawn in several passes: a light-only pass (the sun, the first lamps, the ambient), extra lamp passes, a texture pass that MULTIPLIES
// what is there by the object's colour, highlight passes ADDED after it, and fog. With linear lighting each added highlight pass was
// encoded on its own, so its highlight came out several times too strong (1% light -> 12% stored), and these passes had no metal and
// the plain diffuse: two identical floor pieces, one reached by 2 lamps (one exact pass) and the next by 3, looked different (user,
// Lucky 38 floor, 2026-10-21). On, for the objects NVR picks per draw, the light-only and extra-lamp passes work their lamps' light out
// as the one-pass shaders do (the PBR diffuse, the highlight, metal) and divide it by the colour the texture pass then multiplies in
// (the base texture, times the vertex colour where the game's texture pass multiplies that too: NVR_SpecFade.z), so after the
// multiply each pass holds that light exactly as one pass would, highlight included; NVR skips the object's highlight passes. Only in
// the light-only (AD) and extra-lamp (DIFFUSE) variants without a glow map; NVR_SpecFade.y 0 (every other draw): the old code exactly.
#ifndef FOLD_HIGHLIGHTS_ON
    #define FOLD_HIGHLIGHTS_ON 0
#endif
#if FOLD_HIGHLIGHTS_ON && defined(ONLY_LIGHT) && !defined(ONLY_SPECULAR) && !defined(HAIR) && !defined(SI)
    #define NVR_FOLD
    #define FOLD_DECL
#else
    #define FOLD_DECL float3
#endif

// UNOFFICIAL 2026-11-14 full deferred renderer, phase 0 (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.4, docs/next/FD-RECORDS-RESULTS.md):
// LAMPS_DEFERRED 2 = the lighting-free record. Every direct light is compiled out: the sun / cell light and its shadow (forward cascades,
// cloud shadows, contact hardening, the character map), every lamp (the game's slots, ManyLamps' constant block, the lamp table loop,
// their cube / atlas / RT shadows), the flashlight's cone and the highlights of all of them. Kept: the game's ambient (with metal's
// ambient), emittance / glow, fog, COLOR0.a's reflection mask and every G-buffer write; COLOR1.a carries 1 - fog (the share of added
// light the fog leaves), as the DeferredLamps records (LAMPS_DEFERRED 1, lamps only) write it. A value of the existing define, not a new
// one: the define list is near its cap of 62. Without it (every record today) nothing below changes.
#if defined(LAMPS_DEFERRED) && LAMPS_DEFERRED == 2
    #define NVR_LIGHT_DEFERRED
#endif
#if defined(LAMPS_DEFERRED) && LAMPS_DEFERRED
    #include "includes/DeferredPixel.hlsl"	// UNOFFICIAL 2026-11-14 DeferredSun: the deferred records' codes in COLOR1.a (values 1, 2, 3: see the outputs)
#endif
// UNOFFICIAL 2026-11-15 DeferredTransparents (core/DeferredTransparents.h, Includes/ForwardPlus.hlsl): LAMPS_DEFERRED 4 = the forward+ record
// of a blended draw (drawn after the main list, so no light pass reaches it): the game's lamps compiled out, the sun / cell light slot
// kept, and every lamp of the pixel's froxel from the frame's lamp table added (FPLampSum), shadowed from the atlas; 5 = the same unshadowed.
// No G-buffer mark (nothing lights it later).
#if defined(LAMPS_DEFERRED) && (LAMPS_DEFERRED == 4 || LAMPS_DEFERRED == 5)
    #define NVR_LAMPS_OUT
    #define NVR_FORWARD_PLUS
    #ifndef POINT_SHADOW_ATLAS
        #define POINT_SHADOW_ATLAS 1	// the table lamps' shadows are atlas tiles (native's other records keep the cube maps)
    #endif
#endif
#if defined(LAMPS_DEFERRED) && LAMPS_DEFERRED >= 2
    #define NVR_NO_LAMPS		// no lamp in the record (2 and 4/5)
    #define NVR_GBUFFER_LIT		// the light passes light this record's pixels (2; 4 / 5 write the same codes, read by nobody: no G-buffer is bound after the list)
#endif
// The slots that hold the sun / cell light in this shader (kept by LAMPS_DEFERRED 3): none where light 0 is a lamp (the extra-lamp and point
// passes, and the many-lamp OPT shaders, whose slot 0 is a lamp: Includes/ManyLightsDirect.hlsl).
#if defined(OBJ_LAMP_FIRST) || (defined(OPT) && defined(LIGHTS) && LIGHTS >= 4)
    #define NVR_SUN_SLOTS 0
#else
    #define NVR_SUN_SLOTS 1
#endif
// UNOFFICIAL 2026-11-16 fd-batchE (docs/next/FD-BATCH-E.md; audit items 19, 20, 26): the light-only base pass (AD) draws light alone; the
// game's TEXTURE pass multiplies the texture (and, on a mesh with the Vertex_Colors flag, the vertex colour) in afterwards and its FOG pass
// fogs the result. The AD vertex shaders now always send COLOR0 (FoldHighlights' foldColor, which they sent only with the fold compiled
// in): rgb the vertex colour, a the game's fog amount at the vertex (the fogged variants' formula, from the game's FogParam). Only the
// deferred records read it (OBJ_AD_GBUFFER): their G-buffer albedo takes the vertex colour where the draw's flag says the texture pass
// multiplies it (NVR_SpecFade.z, per draw: core/PbrPop.h) and, with [Main.Main.Quality] DeferredADFog on at startup (AD_FOG_KEEP_ON;
// default off, unproven in a fogged scene), their COLOR1.a keep is 1 - that fog (else 1, as before: their deferred light is not fogged
// by the game's fog). One vertex shader serves the forward and the deferred records; the forward pixel shaders do not read it.
#if defined(OBJ_AD_PASS) && defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 3 && defined(LIGHTS) && LIGHTS < 4
    #define OBJ_AD_EXTRA	// the vertex shader writes foldColor (vertex colour + fog)
    #if defined(PS) && defined(NVR_GBUFFER_LIT)
        #define OBJ_AD_GBUFFER	// the deferred AD record reads it
    #endif
#endif
#if !defined(OBJ_VCOLOR) && (defined(NVR_FOLD) || (defined(VS) && defined(OBJ_AD_EXTRA)) || defined(OBJ_AD_GBUFFER))
    #define OBJ_FOLD_COLOR	// foldColor : COLOR0, the vertex colour (FoldHighlights; fd-batchE: + the fog amount in a)
#endif

// UNOFFICIAL 2026-10-22 LampDistanceFix ([Shaders.PBR.Main], restart: LAMP_VECTOR_ATT_ON from ShaderRecord::BuildDefines;
// bench\deep-roadmap\evidence\pbrdark\NOTE.md). The 2- and 3-lamp shaders (this LIGHTS < 4 path) took each lamp's falloff from
// lightDistSq: the squared distance to the lamp worked out at each VERTEX and blended across the triangle. A squared distance does not
// blend linearly: on a big flat triangle with a lamp over its middle (a rug, a floor tile, a poster, a wall) every corner is far from the
// lamp, so the blended distance stays large everywhere and the lamp lit almost nothing (the Prospector saloon's rug: its lamps' light
// about 4x too weak, the room 3-4x darker than the game). The game's shaders blend the lamp VECTOR and square it per pixel, which is
// exact. On, the vertex shader sends the lamp vector (light2Dir / light3Dir, and lightDir in the lamp-only passes) at its object-space
// length (the tangent-space vector rescaled, so a sheared tangent basis -- what lightDistSq was for, 2026-09-07 -- still gives the
// right distance at the vertices) and the pixel shader squares it: the game's falloff exactly on flat surfaces. Off: as before.
#ifndef LAMP_VECTOR_ATT_ON
    #define LAMP_VECTOR_ATT_ON 0
#endif
#if LAMP_VECTOR_ATT_ON
    #define LAMP_ATT_SQ(distSq, dir) vanillaAttSq(dot((dir).xyz, (dir).xyz), (dir).w)
    #define LAMP_VS_DIR(tbnMat, lPos) LampVectorAtLength(mul(tbnMat, lPos), lPos)
    float3 LampVectorAtLength(float3 t, float3 lPos) { return t * sqrt(dot(lPos, lPos) / max(dot(t, t), 1e-12f)); }
#else
    #define LAMP_ATT_SQ(distSq, dir) vanillaAttSq(distSq, (dir).w)
    #define LAMP_VS_DIR(tbnMat, lPos) mul(tbnMat, lPos)
#endif

#include "includes/Helpers.hlsl"
// UNOFFICIAL: the live surroundings cube for metal (includes/LiveCube.hlsl), before Object.hlsl, whose getMetalAmbient reads
// it. Compiled in only when a CubeReflections setting is above 0 at startup (LIVE_CUBE, ShaderRecord::LoadShader), and not with
// the sun cross-fade, whose old atlas takes the same sampler.
#ifndef LIVE_CUBE
    #define LIVE_CUBE 0
#endif
#if defined(PS) && LIVE_CUBE && !(defined(SUN_CROSSFADE) && SUN_CROSSFADE)
    #define NVR_LIVE_CUBE
    #include "includes/LiveCube.hlsl"
#endif
// UNOFFICIAL material rules (Object.hlsl): the per-texture class the pixel shaders read (c197-c199). Compiled in only when
// [Shaders.PBR.MaterialRules] Enabled is on at startup (MATERIAL_RULES_ON, ShaderRecord::LoadShader).
#ifndef MATERIAL_RULES_ON
    #define MATERIAL_RULES_ON 0
#endif
#if defined(PS) && MATERIAL_RULES_ON
    #define MATERIAL_RULES
#endif
// UNOFFICIAL _rmaos maps (Object.hlsl): compiled in only when [Shaders.PBR.Rmaos] Enabled is on at startup (RMAOS_ON,
// ShaderRecord::LoadShader).
#ifndef RMAOS_ON
    #define RMAOS_ON 0
#endif
#if defined(PS) && RMAOS_ON
    #define RMAOS
#endif
// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl, [Shaders.PBR.Linear]): the pixel shaders light in linear space while
// it is on. Compiled in only when on at startup (LINEAR_LIGHTING_ON, ShaderRecord::LoadShader). The game's colour constants
// are then read under new names (...In) and decoded into the old names at the start of main.
#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if defined(PS) && LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
    // UNOFFICIAL 2026-10-17 LinearPreDecode ([Shaders.PBR.Linear] PreDecode, restart: LIN_PREDECODE_ON from ShaderRecord): the
    // game's per-draw colour constants (AmbientColor c1, PSLightColor c3-c12) are read from decoded copies in c64 / c66-c75 (NVR
    // decodes them on the CPU as the game uploads them, core/LinearPreDecode.h) and the strength settings come decoded in
    // TESR_LinearScales (Object.hlsl), instead of every pixel decoding the same constants again (pow per colour: ~80% of linear
    // lighting's cost indoors, measured in replay). Off: exactly as before.
    #if defined(LIN_PREDECODE_ON) && LIN_PREDECODE_ON
        #define LIN_PREDECODED
    #endif
#endif
// UNOFFICIAL 2026-10-21 LampSums ([Shaders.PBR.Main] LampSums, restart: LAMP_SUMS_ON from ShaderRecord::BuildDefines, only with linear
// lighting; per run c210 y, core/PassSums.h; bench\deep-roadmap\evidence\lampsums\LOG.md). An object lit by more lamps than its first
// pass holds gets extra lamp passes (these DIFFUSE shaders) ADDED onto its light-only pass, each encoded on its own: the frame held
// enc(A) + enc(L) instead of enc(A + L) (FoldHighlights' leftover: the Ultra-Luxe's lamp-rich floors still +60%). While NVR sets
// TESR_PassRedirect.y (the world's run of extra lamp passes), the pass reads the frame as it was when the run began (NVR_LampRunStart,
// a copy: the light-only pass's result) and adds exactly enough to get there in linear light: enc(dec(s) + L) - s. Exact where one
// extra lamp pass covers a pixel (3-5 lamps); where two do (6+ lamps), each starts from the run's s. s9 is the sun's atlas, which no
// DIFFUSE pass reads; no sampler_state (the DLL sets point / clamp). Off (not compiled): the shaders exactly as before.
#ifndef LAMP_SUMS_ON
    #define LAMP_SUMS_ON 0
#endif
#if defined(PS) && defined(DIFFUSE) && LAMP_SUMS_ON && LINEAR_LIGHTING_ON
    #define NVR_LAMP_SUMS
    sampler2D NVR_LampRunStart : register(s9);
#endif
// UNOFFICIAL 2026-10-25 HighlightSums ([Shaders.PBR.Main] HighlightSums, restart: HIGHLIGHT_SUMS_ON from ShaderRecord::BuildDefines; per run
// c210 y as LampSums, core/PassSums.h): the same increments for the HIGHLIGHT passes (these ONLY_SPECULAR shaders) that FoldHighlights
// leaves to the game -- parallax objects (it never folds them; ParallaxTemplate.hlsl has the same code), glow-mapped ones, hair, objects
// whose fold broke, or with it off. Each highlight pass was encoded on its own and ADDED: a 1% highlight came out 12% brighter (the
// Novac road and rubble pieces lit by 3+ lamps, tester report 2026-10-25). The copy is read on s6: free in every ONLY_SPECULAR variant
// (the game's normal map s0, its shadow maps s4/s5; s9 is the sun's atlas, which these passes read). Same output code as LampSums.
#ifndef HIGHLIGHT_SUMS_ON
    #define HIGHLIGHT_SUMS_ON 0
#endif
#if defined(PS) && defined(ONLY_SPECULAR) && HIGHLIGHT_SUMS_ON && LINEAR_LIGHTING_ON
    #define NVR_LAMP_SUMS
    sampler2D NVR_LampRunStart : register(s6);
#endif
// UNOFFICIAL 2026-10-18 SmithCorrelated ([Shaders.PBR.Main], restart: SMITH_CORRELATED_ON from ShaderRecord::BuildDefines): the object
// pixel shaders take the exact height-correlated Smith masking term and its coupled diffuse (Includes/PBR.hlsl BRDF and
// SmithCorrelatedKeep; idea from Physically-Based-Reloaded (Balebandro), GPL-3.0). Off: the shaders exactly as before.
#if defined(PS) && defined(SMITH_CORRELATED_ON) && SMITH_CORRELATED_ON
    #define PBR_SMITH_CORRELATED SMITH_CORRELATED_ON	// 1 on, 2 the test mode (PBR.hlsl)
#endif
// UNOFFICIAL 2026-10-18 SpecularFade ([Shaders.PBR.Main], restart: SPEC_FADE_ON from ShaderRecord::BuildDefines): the pixel shaders
// that draw a highlight (SPECULAR: the full passes and the highlight-only passes) scale it by the game's specular distance fade for
// the object (NVR_SpecFade, core/PbrPop.h; Includes/PBR.hlsl), so it is gone where the game swaps in the shaders without one. Off: the
// shaders exactly as before.
// The 6-lamp OPT specular 4+ shader (SLS2035) sits close to the slot count the driver refuses (see LAMP_IF below): NVR_SLOT_TIGHT keeps
// the 2026-10-18 pop-in fixes small there (SpecularFade: the highlight only, +12 slots in its interior record instead of +39; the _rmaos
// FadeIn left out: the map comes in at once on those objects).
#if defined(PS) && defined(OPT) && defined(SPECULAR) && !defined(ONLY_SPECULAR) && defined(LIGHTS) && defined(SPECULAR_LAMPS6) && !(defined(NO_SLOT_LIMIT) && NO_SLOT_LIMIT)
    #if LIGHTS > 3 && SPECULAR_LAMPS6
        #define NVR_SLOT_TIGHT
        #define RMAOS_NO_FADE
    #endif
#endif
#if defined(PS) && (defined(SPECULAR) || defined(NVR_FOLD)) && defined(SPEC_FADE_ON) && SPEC_FADE_ON
    #define PBR_SPEC_FADE
    #ifdef NVR_SLOT_TIGHT
        #define PBR_SPEC_FADE_HIGHLIGHT_ONLY
    #endif
#endif
// UNOFFICIAL 2026-11-05 VanillaBridge ([Shaders.PBR.Main] VanillaBridge, restart: VANILLA_BRIDGE_ON from ShaderRecord::BuildDefines;
// Includes/Object.hlsl): the highlight takes the authored Blinn-Phong highlight's width and strength (normal map alpha + the mesh's
// glossiness). In the pixel shaders that draw a highlight and know the glossiness: every SPECULAR variant with Toggles (c27.z), and
// the 4+ lamp OPT ones (PSLightColor[1].w). Not hair (its strands have their own highlight look), and not the 6-lamp OPT shader at
// the driver's slot limit (NVR_SLOT_TIGHT: the bridge costs ~46 slots there, ~40 in the one-sun SPECULAR shaders). Off: the shaders
// exactly as before.
#ifndef VANILLA_BRIDGE_ON
    #define VANILLA_BRIDGE_ON 0
#endif
#if defined(PS) && VANILLA_BRIDGE_ON && defined(SPECULAR) && !defined(HAIR) && !defined(NVR_SLOT_TIGHT)
    #ifndef OPT
        #define NVR_VANILLA_BRIDGE
    #elif defined(LIGHTS)
        #if LIGHTS > 3
            #define NVR_VANILLA_BRIDGE
        #endif
    #endif
#endif
// UNOFFICIAL 2026-11-09 FromGameData (Includes/Object.hlsl): the 4+ lamp SPECULAR interior lamp-shadow records (SLS2034-2036 with
// INTERIOR_SHADOWS) are at the 32-temp limit; reading the "_m" mask took SLS2034 / SLS2036 past it (X4505), so they never read it.
#if defined(PS) && defined(INTERIOR_SHADOWS) && defined(SPECULAR) && defined(LIGHTS)
    #if INTERIOR_SHADOWS && LIGHTS > 3
        #define ENV_NO_MASK
    #endif
#endif
// UNOFFICIAL 2026-11-20: roughness-damped grazing F90 (Lagarde 2014, matching DeferredSun.hlsl / DeferredLamps.hlsl) on forward
// passes (including the 1st-person viewmodel), preventing rough weapon surfaces turned toward a light from losing diffuse saturation
// under full white F90 = 1.0 grazing Fresnel. Guarded by !NVR_SLOT_TIGHT for the 6-lamp SLS2035 slot budget.
#if defined(PS) && !defined(NVR_SLOT_TIGHT)
    #define PBR_ROUGH_F90 1
#endif
// UNOFFICIAL 2026-11-26: keep 2-lamp specular forward shaders (SLS2023/2024/2026/2027) under ps_3_0's 32-temp register limit.
// Deferred Vulkan compute and 0/1-lamp forward shaders keep full dual-normal clearcoat, pearlescence, and clearcoat parallax.
#if defined(PS) && defined(SPECULAR) && defined(LIGHTS) && LIGHTS == 2 && !defined(ONLY_SPECULAR) && !defined(ONLY_LIGHT)
    #define NVR_COAT_LEAN 1
    #define NVR_PEARL_AMBIENT_LEAN 1
#endif
#include "includes/Object.hlsl"
// UNOFFICIAL 2026-11-09 the eye variant (open report "eyes of default races black"; [Shaders.PBR.Main] GameShaderEyes, EyeShaderOld; the
// records are made by Render.cpp GameEyes::RecordFor, from this file with NVR_EYE and no sun / lamp shadow code, and put on the device
// for the draws of geometry flagged Eye_Environment_Mapping only). The game draws an eyeball's lit passes with specular forced off
// (so no metal guess reaches them: METAL is compiled only into SPECULAR variants), then adds its own eye env pass (0x24B, SLS2052 +
// SLS2059, additive) on top: that highlight is kept as it is. Here the lit passes become: the ambient as for any object, plus the sun's
// and the lamps' light as a WRAPPED diffuse on the eyeball's own surface (the vertex normal: tangent space (0, 0, 1)), with no shadows.
// The normal map is not used: eye mods differ in what they put there (and some have none), and an eyeball has no bumps worth lighting.
// Nothing here assumes a UV layout. The output goes through LinearSceneOut as every lit shader's does, so no frame conversion is needed.
#if defined(PS) && defined(NVR_EYE)
    #undef METAL
    // UNOFFICIAL 2026-11-19: eyes now keep forward sun shadows (offset EYE_SHADOW_OFFSET units toward the sun to clear eyelids/brow
    // sockets while still catching hats, roofs, walls, and clouds). Skip screen-space RT sun lookup (s15 stays free for FlashBeam and
    // RT eyelids cannot be offset), dust, wetness, and HUD rain on eyeballs; FlashBeam moves from c108-c116 (now sun cascades) to
    // c146-c154 (free because SUN_CROSSFADE = 0, OBJECT_WETNESS off, HUD_RAIN off on NVR_EYE).
    #define RT_SUN_FORWARD_OFF 1
    #undef DUST_ON
    #define DUST_ON 0
    // Wrapped Lambert: the light reaches round the ball by EYE_WRAP of its quarter turn, as light scattering through the white would.
    #define EYE_WRAP 0.3f
    #define EYE_SHADOW_OFFSET 3.0f
    #define EYE_DEBUG 0	// (warm-slot diagnosis, 2026-11-09: 1 the light colour alone, 2 the wrap alone, 3 the plain N.L, 4 the albedo, 5 albedo x light x wrap)
    float3 EyeLight(float3 toLight, float3 radiance, float3 baseColor) {
        const float wrapped = saturate((normalize(toLight).z + EYE_WRAP) / (1.0f + EYE_WRAP));
        #if EYE_DEBUG == 1
            return radiance;
        #elif EYE_DEBUG == 2
            return wrapped.xxx;
        #elif EYE_DEBUG == 3
            return saturate(normalize(toLight).z).xxx;
        #elif EYE_DEBUG == 4
            return baseColor;
        #elif EYE_DEBUG == 5
            return baseColor * radiance * wrapped;
        #endif
        return lerp(luma(baseColor), baseColor, TESR_PBRExtraData.x) * radiance * LIN_SCALE_LIGHT * wrapped;
    }
    #define getSunLighting(toSun, radiance, toEye, N, baseColor, r) EyeLight((toSun), (radiance), (baseColor))
    #define getPointLightLightingAtt(toLight, falloff, radiance, toEye, N, baseColor, r) (LampAtt(falloff) * EyeLight((toLight), (radiance), (baseColor)))
    // UNOFFICIAL 2026-11-12 HairAndEyes (Includes/FlashBeam.hlsl; [Shaders.Flashlight.Deferred] HairAndEyes): the flashlight's light pass lit
    // the eyeball's records as a glossy object's (the full PBR lamp term with its highlight, which the eye above never gets from a lamp).
    // The eye takes the beam itself instead, by the same wrapped diffuse as its lamps, on the vertex normal in world space, with the beam's
    // cone, reach, falloff and shadow map (no lens pattern: no sampler stage is free in every variant), and records itself as far away
    // (see the outputs) so the light pass leaves it alone. c146-c154: SUN_CROSSFADE / Wetness / HudRain range, all off on NVR_EYE, and
    // restored by GameEyes::After() via own->SetCT(). s15: unused here (INTERIOR_SHADOWS = 0 and RT_SUN_FORWARD_OFF).
    #define FLASH_BEAM_REG c146
    #define FLASH_BEAM_MATRIX_REG c151
    #define FLASH_BEAM_SAMPLER s15
    #include "includes/FlashBeam.hlsl"
    float3 EyeBeam(float3 baseColor, float3 worldNormal, float3 worldPos, bool valid) {
        float3 light = 0.0f;
        [branch] if (FLASH_BEAM_ON && valid) {
            float3 toLight;
            const float beam = FlashBeamStrength(worldPos, toLight);
            const float wrapped = saturate((dot(worldNormal, toLight) * rsqrt(max(dot(toLight, toLight), 1.0e-6f)) + EYE_WRAP) / (1.0f + EYE_WRAP));
            light = lerp(luma(baseColor), baseColor, TESR_PBRExtraData.x) * TESR_FlashBeam[2].rgb * (wrapped * beam);
        }
        return light;
    }
#endif
// UNOFFICIAL: the camera matrices Shadow.hlsl rebuilds world positions with move out of c100-c107 in skinned vertex
// shaders: the game's bone upload reaches past Bones[54] (c97) and overwrote them, so actors sampled the sun shadow
// at the wrong place. c240-c247 is beyond any bone write.
#ifdef VS
    #define SHADOW_INVPROJ_REG c240
    #define SHADOW_INVVIEW_REG c244
#endif
// UNOFFICIAL 2026-10-23 CharacterObjectReach (Includes/Shadow.hlsl SHADOW_SLACK_CAP_ACTOR): defined only when on at startup.
#if defined(PS) && defined(SHADOW_CHARACTER_OBJECTS)
    #define SHADOW_SLACK_CAP_ACTOR 1
#endif
// UNOFFICIAL 2026-10-23 CharacterShadowMap (Includes/Shadow.hlsl CharacterMapShadow; compiled in only when on at startup): the object pixel
// shaders read the player's own sun shadow map too (hats, bandanas, face masks, armour: drawn with these shaders, not the skin's), except
// the shaders with four or more lamps (the six-lamp OPT specular 4+ shader is at the driver's slot limit, NVR_SLOT_TIGHT above, and the
// four-lamp ones were at ps_3_0's 32 temporaries with two cascade lookups): a hat lit by four lamps and the sun keeps the cascades.
#if defined(PS) && !defined(NVR_SLOT_TIGHT) && (!(defined(LIGHTS) && LIGHTS > 3) || defined(NVR_EYE))
    #define SHADOW_CHARACTER_MAP_IN 1
#endif
// UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): the clouds' shadow on the sun term (always compiled in; its strength switches it live).
// Every object pixel shader, the four- and nine-lamp ones too (one lookup and a few ALU, against the character map's second cascade
// read): without it the objects drawn with the many-lamp shaders kept the sun under a cloud, so the cloud's edge seemed to glide over
// them as it crossed the land round them.
#if defined(PS)
    #define SHADOW_CLOUD_IN 1
#endif
// (1.0.5) not where the live metal cube holds s10 AND s3 is taken too: the 4+ lamp shaders (lamp table; object LOD, the only draws
// that take the contact, is never drawn with lamps) and the light-only glow / hair-tint passes (GlowMap on s3). s4-s7 are the game's.
#if defined(NVR_LIVE_CUBE) && ((defined(LIGHTS) && LIGHTS > 3) || ((defined(SI) || defined(HAIR)) && !defined(ONLY_SPECULAR) && defined(ONLY_LIGHT)) || defined(NVR_FORWARD_PLUS))
    #define NVR_CONTACT_NO_SAMPLER 1
#endif
#if defined(PS) && !defined(NVR_SLOT_TIGHT) && !defined(NVR_CONTACT_NO_SAMPLER) && !defined(NVR_EYE)
    #define SHADOW_SUN_CONTACT_PERDRAW 1
    #define SHADOW_SUN_CONTACT_IN 1   // UNOFFICIAL 2026-11-06 forward contact shadows (Includes/Shadow.hlsl; compiled in only with Bend on at start): one lookup
#endif
#include "includes/Shadow.hlsl"
// 2026-11-04 dust on top surfaces ([Shaders.PBR.Dust], Includes/ObjectDust.hlsl): compiled in only when on at startup (DUST_ON).
#ifndef DUST_ON
    #define DUST_ON 0
#endif
#if defined(PS) && DUST_ON
#include "includes/ObjectDust.hlsl"
#endif
// UNOFFICIAL 2026-11-04 Wetness Effects (Includes/Wetness.hlsl, from Physically-Based-Reloaded; in place of WetWorld's screen pass): the
// passes that light the material's own albedo with the sun get the water layer; lamp-only, highlight-only and lamp-first passes do not,
// nor the interior records (no rain indoors). Its constants are the sun cross-fade's (c146-c159), so not with SUN_CROSSFADE; s12 is free
// in every outdoor variant, for the rain cover map.
#if defined(PS) && defined(WETNESS_EFFECTS) && !defined(WETNESS_OFF) && FORWARD_SHADOWS && !SUN_CROSSFADE && !defined(NVR_EYE) && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS && !(defined(POINT_SHADOW_RT_ONLY) && POINT_SHADOW_RT_ONLY && WETNESS_EFFECTS >= 2)) && \
    !defined(DIFFUSE) && !defined(POINT) && !defined(ONLY_SPECULAR) && !defined(OBJ_LAMP_FIRST)
    #define OBJECT_WETNESS
    #if defined(POINT_SHADOW_RT_ONLY) && POINT_SHADOW_RT_ONLY
        // 2026-11-09 the outdoor RT lamp-shadow record (INTERIOR_SHADOWS + POINT_SHADOW_RT_ONLY, ShaderManager ExteriorForward) gets the water
        // layer too; PointShadowForward writes c174-c189 raw on every draw there (NVR_PointShadowLight + params), so the puddles' two take
        // c160 / c161 (in none of those records' tables: Bend's c160-c164 is not compiled into them; no raw write)
        #define PR_DATA c160
        #define PR_LOOK c161
        // 2026-11-12 these records are full (32 temps): the puddles' lean path here (Wetness.hlsl PUDDLES_LEAN: no shoreline noise, no
        // texture-detail height); with the full one 20 SLS records no longer fit with the water layer and lost it near lamps. Not in map
        // mode (WETNESS_EFFECTS 3): the heavy terms come from the puddle map there and the full path fits.
        #if WETNESS_EFFECTS < 3
            #define PUDDLES_LEAN
        #endif
    #elif defined(SPECULAR) && defined(LIGHTS) && LIGHTS == 2 && WETNESS_EFFECTS < 3
        // P6A fix: keep 2-lamp specular variants (SLS2023/2024/2026/2027) cleanly under ps_3_0's 32-temp register limit.
        #define PUDDLES_LEAN
    #endif
    #define WETNESS_ORTHO_REG s12
    #include "includes/Wetness.hlsl"
    #if defined(PUDDLES_COMPILED) && !defined(PUDDLES_LEAN)	// 2026-11-12 Puddles: the colour texture's detail as the puddles' height (Wetness.hlsl ObjectPuddleHeight)
        #define OBJ_PUDDLE_HEIGHT(uv, p) , ObjectPuddleHeight(BaseMap, (uv), (p))
    #else
        #define OBJ_PUDDLE_HEIGHT(uv, p)
    #endif
    // The wet albedo (linear). In a light-only pass the albedo is 1 here and the texture pass multiplies the real one (dryTexture, stored)
    // in afterwards, so the light is scaled by how much darker the wet albedo is instead.
    float3 ObjectWetAlbedo(float3 albedo, float3 dryTexture, Wetness wet, float porosity) {
        #ifdef PUDDLES_COMPILED	// 2026-11-12 standing water darker than the ground round it (WetAlbedo blends only half way)
            #define PUDDLE_UNDER_MUL * lerp(1.0f, 0.4f, wet.flatten)
        #else
            #define PUDDLE_UNDER_MUL
        #endif
        #ifdef ONLY_LIGHT
            float3 dry = max(LinColor(dryTexture), 1.0f / 256.0f);
            return albedo * WetAlbedo(dry, wet.darkening, porosity) / dry PUDDLE_UNDER_MUL;
        #else
            return WetAlbedo(albedo, wet.darkening, porosity) PUDDLE_UNDER_MUL;
        #endif
    }
    // The water film over the material: the sun's highlight on it (sunRadiance: the sun as getSunLighting takes it, shadowed and scaled)
    // and its reflection of the sky. Undone by the texture a light-only pass's light is later multiplied by.
    #if defined(PUDDLES_COMPILED) && !defined(PUDDLES_LEAN)
    // 2026-11-12 Puddles: a lamp's highlight on the wet film, stretched toward the eye (wet-road streaks; Wetness.hlsl WetLampFilm), in
    // the space the lamp vectors are in (tangent space: N the flattened normal, V the view vector, L the lamp vector). Undone by the
    // texture a light-only pass's light is later multiplied by, as the sun's film.
    float3 ObjectLampFilm(Wetness wet, float3 N, float3 V, float3 L, float3 radiance, float3 dryTexture) {
        float3 film = WetLampFilm(N, V, normalize(L), radiance, wet.roughness, lerp(1.0f, 0.3f, wet.gloss)) * smoothstep(0.0f, 0.2f, wet.gloss);
        #ifdef ONLY_LIGHT
            film /= max(LinColor(dryTexture), 1.0f / 256.0f);
        #endif
        return film;
    }
    #endif
    float3 ObjectWetFilm(Wetness wet, float3 position, float3 geometricNormal, float3 sunRadiance, float3 dryTexture) {
        float3 wetV = -normalize(position);
        float3 film = WetSpecular(wet.normal, wetV, TESR_WetnessSun.xyz, sunRadiance, wet.roughness) * smoothstep(0.0f, 0.2f, wet.gloss);
        float3 r = reflect(-wetV, wet.normal);
        float horizon = saturate(1.0f + 1.2f * dot(r, geometricNormal));
        film += WetSkyReflection(r) * WetReflectance(saturate(dot(wet.normal, wetV)), wet.roughness, wet.gloss) * horizon * horizon;
        #ifdef ONLY_LIGHT
            film /= max(LinColor(dryTexture), 1.0f / 256.0f);
        #endif
        return film;
    }
#endif
#include "includes/CapturedNormals.hlsl"	// UNOFFICIAL 2026-10-28 CapturedNormals (COLOR3, see there)
// (2026-11-16 fd-merged-1116) the smooth normal's COLOR3 writer: faced by the triangle's side (fd-roofedge, CapturedNormalOutTri), except
// in the forward+ records (NVR_FORWARD_PLUS: DeferredTransparents' glass). Those draw in the sorted blended list, after the main list's span,
// when no G-buffer target is bound, so their COLOR3 is never read; the triangle test's derivatives ran native SLS2034 / SLS2036's indoor
// forward+ records out of temp registers (X4505) and those glass draws lost their record. They keep the view test (as before roof-edge).
#ifdef NVR_FORWARD_PLUS
    #define OBJ_CAPTURED_NORMAL_OUT CapturedNormalOut
#else
    #define OBJ_CAPTURED_NORMAL_OUT CapturedNormalOutTri
#endif
// UNOFFICIAL 2026-11-03 SmoothCutouts (Includes/AlphaHash.hlsl): the alpha test hashed under TAA / FSR / DLSS. Not in the slot-tight shader.
// (2026-11-16 fd-merged-1116) Not in the forward+ records either (NVR_FORWARD_PLUS, LAMPS_DEFERRED 4 / 5: DeferredTransparents' glass): they
// serve only the sorted BLENDED list, and AlphaHash::ForPass never marks a blended draw (b15 false: the hash and FarCoverage return the alpha
// unchanged there), so the code was dead at run time; with Smooth Cutouts on by default it ran native SLS2034 / SLS2036's indoor forward+
// records out of temp registers (X4505), and those glass draws lost their forward+ record.
#if defined(PS) && defined(ALPHA_HASH_ON) && !defined(NVR_SLOT_TIGHT) && !defined(NVR_FORWARD_PLUS)
    #define OBJ_ALPHA_HASH 1
    #include "includes/AlphaHash.hlsl"
#endif
#if defined(PS) && defined(ALPHA_COVERAGE_ON) && !defined(NVR_SLOT_TIGHT) && !defined(NVR_FORWARD_PLUS)	// UNOFFICIAL 2026-11-16 FarCoverage (Includes/AlphaHash.hlsl)
    #define OBJ_CUT_COVERAGE 1
    #include "includes/AlphaHash.hlsl"
#endif
// UNOFFICIAL 2026-10-24 SmoothObjectNormals (Includes/Shadow.hlsl, OBJECT_SMOOTH_NORMALS): the vertex normal in camera-relative world space
// for the shadow lookups and the sky ambient, in place of the per-triangle derivative normal. The LIGHTS < 4 family carries it in its own
// interpolator (TEXCOORD8; vs_3_0 has 12 outputs and ps_3_0 10 inputs: with PROJ_SHADOW every one is taken, so those variants keep the flat
// normal); the LIGHTS >= 4 family has no interpolator spare and carries it in uv.zw + vertexColor.a (OBJ_WORLD_NORMAL below; 2026-10-26 a
// plain vector, Includes/Shadow.hlsl ShadowPlainNormal).
// 2026-10-24 (second round; owner: still faceted, skin and clothes, indoors, gone only with PBR off): characters are often drawn with the
// projected-shadow (PROJ_SHADOW) variants, which had no interpolator spare and kept the per-triangle normal for the sky term, the sun lookup
// and the metal reflection (grey cloth counts as metal, [Shaders.PBR.Metal] World). Now the full passes (not ONLY_LIGHT) carry the normal
// octahedrally in lightDistSq.w / lightDir.w, the two channels METAL_UP used for world up (the metal reflection's sky test now works in world
// space from it, OBJ_SMOOTH_NORMAL_W); the light-only passes (ONLY_LIGHT: no vertex colour, no fog) have TEXCOORD8 free even with PROJ_SHADOW.
#if OBJECT_SMOOTH_NORMALS && !(defined(LIGHTS) && LIGHTS >= 4)
    #ifdef ONLY_LIGHT
        #define OBJ_SMOOTH_NORMAL_INTERP 1
    #else
        #define OBJ_SMOOTH_NORMAL_W 1
    #endif
#endif
#ifdef PS
    // UNOFFICIAL forward point-light shadows (INTERIOR_SHADOWS): SHADOWED(colour, k, att) gives light k of the draw
    // its own cube-map shadow (looked up once per pixel near the top of main); without INTERIOR_SHADOWS it is just the
    // colour. POINT_SHADOW_SCALAR (2026-10-14): ptShadow0..5 held as plain numbers, the debug views' tint applied where
    // each is used: six float3 kept alive across the lighting put the 4+ lamp shader (SLS2035 with _rmaos maps, hand blood
    // and MoreLampsPerObject on) one temp register over ps_3_0's 32, and its interior record failed to compile.
    #define POINT_SHADOW_SCALAR
    // UNOFFICIAL 2026-11-04 the flashlight's game lamp per pixel (core/FlashlightLamp.h; user: "the way the game interacts with lights has no
    // blending"): the Pip-Boy light the flashlight drives lights the objects the light pass cannot see, but the game hands a lamp to a whole
    // object, so the CPU could only shape it by the cone at the object's bound (all of a plant lit, or none). Per draw c28-c30 say which slot
    // it has and the beam; light k's share here is the beam's cone at this pixel over the cone the CPU already gave the object.
    float4 NVR_FlashLamp[3] : register(c28);	// [0] x the lamp's slot + 1 (0: not in this draw), y 1 / the object's cone, z cos of the full-strength edge;
                                            	// [1] xyz the light, camera-relative; [2] xyz the beam's axis, w cos of its outer edge
    static float flashLampCone = 1.0f;
    float FlashLampCone(float3 worldPos, bool valid) {
        [branch] if (!valid) return 1.0f;	// (a vanilla vertex shader: the object's cone as the CPU gave it)
        const float3 toPixel = worldPos - NVR_FlashLamp[1].xyz;
        const float c = dot(toPixel, NVR_FlashLamp[2].xyz) * rsqrt(max(dot(toPixel, toPixel), 1.0f));
        const float cone = smoothstep(NVR_FlashLamp[2].w, NVR_FlashLamp[0].z, c);
        return min(cone * cone * NVR_FlashLamp[0].y, 8.0f);
    }
    #define POINT_LAMP_EXTRA(k) ((k) + 1 == NVR_FlashLamp[0].x ? flashLampCone : 1.0f)
    #include "includes/PointShadowForward.hlsl"

#ifndef SHADOW_CONTRAST_ON
    #define SHADOW_CONTRAST_ON 0
#endif
#if SHADOW_CONTRAST_ON
// UNOFFICIAL 2026-10-29 ShadowContrast ([Shaders.ShadowsInteriors.Forward] ShadowContrast, effects/PBR.cpp; ROOTPLAN round 5 step 14).
// A forward lamp shadow takes away only its own lamp's light, so where the other light (the ambient, lamps without a shadow) is a large
// share of the pixel the shadow comes out grey and specked (cpucheck / ROOTPLAN step 12: the P80 look needed lamps 23-51 % brighter).
// Here every shadowed lamp's light is split into what it keeps and what its shadow takes (SC_SPLIT; in the one-pass lamp shader in
// Includes/ManyLightsDirect.hlsl); in a pass that has the ambient (the one-pass shader, the game's base passes with lamps) the rest of
// the pixel's light is then darkened right there by the share
// of the pixel's would-be light that the shadows took: lighting x (1 - strength x lost / (lit + lost)). Fully lit pixels (lost 0) are
// untouched, so rooms keep their brightness; a pixel deep in a lamp's shadow loses that share of its fill light too.
// UNOFFICIAL 2026-10-29 (round 6 step 17, the game's passes: ManyLampsOnePass off): an added lamp pass (DIFFUSE) cannot dim the rest
// of the pixel, so it adds the light its shadows took to MaterialBuffer's alpha instead (core/CapturedNormals.h binds it for those
// passes, alpha only); one full-screen step after the main list then does the same darkening (ShadowContrastApply).
// Compiled in only when ShadowContrast > 0 at startup (ShaderRecord::BuildDefines); then live, x 0 = exactly no change.
// 2026-10-31: c93 (was c207, inside the RT lamp shadows' NVR_RtLampReproject c206-c209: every interior lamp-shadow shader with both
// failed to compile). c88-c97 hold only the land shader's constants; no object pixel shader uses them (ManyLamps: c37-c63, c76-c87, c98-c107).
float4 TESR_ShadowContrast : register(c93);	// x strength indoors this frame (0 outdoors / off)
#define SC_SPLIT(TERM, SH) { const float3 scTerm = (TERM); const float3 scShadow = (SH); lighting += scTerm * scShadow; scLost += scTerm * (1.0f - scShadow); }
float ShadowContrastKeep(float3 lit, float3 lost) {
    const float3 w = float3(0.2126f, 0.7152f, 0.0722f);
    const float l = dot(max(lost, 0.0f), w);
    const float share = l / max(dot(max(lit, 0.0f), w) + l, 1.0e-5f);
    return saturate(1.0f - TESR_ShadowContrast.x * share);
}
#endif

    // UNOFFICIAL 2026-10-13: blood on the first-person weapon and gloves (c218-c219, set per draw; off for every other draw).
    #include "includes/HandBlood.hlsl"
    // UNOFFICIAL 2026-10-14, 2026-11-04: rain drops on the first-person weapon and (AllObjects) the world's objects (Wetness Effects'
    // HudRaindrops, includes/HudRain.hlsl, c221, c222, c170 set per draw; compiled in only when on at startup: HUD_RAIN_ON, ShaderRecord).
    #if defined(HUD_RAIN_ON) && HUD_RAIN_ON && !defined(NVR_EYE)
        #define HUD_RAIN 1
    #endif
    #include "includes/HudRain.hlsl"

    // UNOFFICIAL 2026-10-17 UnlitShadowSkip ([Shaders.PBR.Main], restart: UNLIT_SHADOW_SKIP from ShaderRecord::BuildDefines): the
    // forward sun-shadow lookups (contact hardening's blocker search + 3 x 3 PCF, 14+ shadow-map reads) only where the sun can
    // light the pixel. The sun's light is getSunLighting(lightDir, colour x shadow, ..., normal): its diffuse part is
    // saturate(N.L) x colour, exactly 0 where the bumped normal faces away from the sun (dot(normal, lightDir) <= 0: normalize
    // keeps the sign), whatever the shadow, so the lookup is skipped there. In the SPECULAR variants PBRSun keeps N.S >= 1e-5, a
    // tail of the order of 1e-4 of the sun's colour on back-facing pixels (no difference measured: evidence\objects-LOG.md).
    // Compile time: a live switch costs its branch structure even when off. Off: the shaders exactly as before.
    #ifndef UNLIT_SHADOW_SKIP
        #define UNLIT_SHADOW_SKIP 0
    #endif
    bool SunShadowWanted(float3 normalTS, float3 lightTS) {
        bool wanted = dot(normalTS, lightTS) > 0.0f;
        return wanted;
    }
    // UNOFFICIAL 2026-10-17 LampReachSkip ([Shaders.PBR.Main], restart: LAMP_REACH_SKIP from ShaderRecord::BuildDefines): a lamp's
    // light is att x (...) and att (vanillaAtt / vanillaAttSq = 1 - saturate(...)) is exactly 0 beyond its radius, where the term
    // adds exactly 0; LAMP_IF(att) puts the term in a branch on att > 0 so its PBR math is skipped there. Compile time, not live:
    // a live test made the lamp terms branches even when off and cost 0.6 ms indoors (evidence\objects-LOG.md L-O5). Not in
    // the DIFFUSE passes (TrimLampPasses already drops their pixels out of every lamp's reach; the branches measured slower there),
    // nor the ONLY_SPECULAR lamp passes (the same: in game +0.05 ms, though the frame replay showed a gain: evidence L-O6),
    // nor in the 6-lamp OPT specular 4+ shader (SLS2035), which the driver refuses to create past ~3600-3870 slots.
    #ifndef LAMP_REACH_SKIP
        #define LAMP_REACH_SKIP 0
    #endif
    // UNOFFICIAL 2026-10-25 NO_SLOT_LIMIT ([Shaders.PBR.Main] DxvkLampBranches, restart; ShaderRecord::BuildDefines sets it only
    // when the game runs on DXVK, which creates shaders of any length): the 6-lamp shaders (LIGHTS 4 + SPECULAR + MoreLampsPerObject,
    // OPT or not) take the branches too, and in them a lamp slot the draw does not use (k >= lightsUsed: the game leaves its constants
    // stale, the term was computed and multiplied by 0) and its cube-map shadow lookup are skipped as well (LAMP_IFK).
    #ifndef NO_SLOT_LIMIT
        #define NO_SLOT_LIMIT 0
    #endif
    #if NO_SLOT_LIMIT && LIGHTS > 3 && defined(SPECULAR) && SPECULAR_LAMPS6 && !defined(DIFFUSE) && !defined(ONLY_SPECULAR)
        #define LAMP_IF(att) [branch] if ((att) > 0.0f)
        #define LAMP_IFK(k, att) [branch] if ((att) > 0.0f && (k) < lightsUsed)
        #define LAMP_SLOT_SKIP 1
    #elif LAMP_REACH_SKIP && !defined(DIFFUSE) && !defined(ONLY_SPECULAR) && !(defined(OPT) && defined(SPECULAR) && LIGHTS > 3 && SPECULAR_LAMPS6)
        #define LAMP_IF(att) [branch] if ((att) > 0.0f)
        #define LAMP_IFK(k, att) LAMP_IF(att)
    #else
        #define LAMP_IF(att)
        #define LAMP_IFK(k, att)
    #endif
#endif

// GunFX barrel glow (P67). Heat bridge from GunFX.dll. Coordinates are local to the drawn gun mesh, so
// viewmodel projection and camera motion cannot move the mask off the barrel.
float4 BarrelHeatMuzzle : register(c171); // local xyz, strength
float4 BarrelHeatAxis : register(c172);   // local unit vector toward the breech, length
float4 BarrelHeatShape : register(c173);  // local radius, forward reach

float BarrelHeatMask(float3 localPos) {
    float3 boneOff = localPos - BarrelHeatMuzzle.xyz;
    float along = dot(boneOff, BarrelHeatAxis.xyz);
    float radial = length(boneOff - BarrelHeatAxis.xyz * along);
    float span = max(BarrelHeatAxis.w, 1.0f);
    float radius = max(BarrelHeatShape.x, 1.0f);
    float forward = max(BarrelHeatShape.y, 1.0f);
    // Keep the muzzle hottest and taper continuously toward the breech.
    float rearFade = saturate(1.0f - max(along, 0.0f) / span);
    return smoothstep(-forward, -forward * 0.15f, along)
         * rearFade
         * (1.0f - smoothstep(radius * 0.375f, radius, radial))
         * BarrelHeatMuzzle.w;
}

#ifdef PS
#ifdef OBJ_AD_PASS
float3 ApplyBarrelHeat(float3 color, float mask, float3 materialFactor) {
#else
float3 ApplyBarrelHeat(float3 color, float mask) {
#endif
    if (BarrelHeatMuzzle.w <= 0.0f) return color;
    float heat = saturate(mask);
    float orange = smoothstep(0.5f, 1.0f, heat);
    float3 ember = lerp(float3(0.68f, 0.055f, 0.015f),
                        float3(1.25f, 0.23f, 0.035f), orange);
#ifdef OBJ_AD_PASS
    // Lighting-only passes are later modulated by the material texture.
    // Undo that modulation for the emissive term, keeping ordinary lighting intact.
    return lerp(color, ember / materialFactor, heat * heat * 0.75f);
#else
    return lerp(color, ember, heat * heat * 0.75f);
#endif
}
#endif

// Forward sun shadows. Enabled at COMPILE TIME via FORWARD_SHADOWS in Includes/Shadow.hlsl,
// deliberately not via a runtime constant -- see the note there.

#ifdef SKIN
    #include "includes/SkinHelpers.hlsl"
    #include "includes/PipboyFit.hlsl"	// UNOFFICIAL 2026-11-03 PipboyFit
#endif

// UNOFFICIAL 2026-10-16 cloth wind (Includes/ClothWind.hlsl, core/ClothWind.h): compiled into the vertex shaders only when
// [Shaders.Cloth.Main] Enabled is on at startup (CLOTH_WIND_ON); per draw it runs only for cloth (NVR sets c248-c253).
#ifndef CLOTH_WIND_ON
    #define CLOTH_WIND_ON 0
#endif
#if defined(VS) && CLOTH_WIND_ON
    #define CLOTH_WIND
    #include "includes/ClothWind.hlsl"
#endif
// UNOFFICIAL 2026-10-20 plant wind (Includes/PlantWind.hlsl, core/PlantWind.h): static plants bend in the grass field wind; compiled in
// only when [Shaders.Plants.Wind] Enabled is on at startup (PLANT_WIND_ON); per draw it runs only for plants (c248.x = -1).
#ifndef PLANT_WIND_ON
    #define PLANT_WIND_ON 0
#endif
#if defined(VS) && PLANT_WIND_ON
    #define PLANT_WIND
    #include "includes/PlantWind.hlsl"
#endif

// Toggles.
#define useVertexColor Toggles.x	// > 0: multiply the colour by the vertex colour
#define useFog Toggles.y	// > 0: apply the fog
#define glossPower Toggles.z	// the game's highlight exponent
#define alphaTestRef Toggles.w	// alpha test reference

struct VS_INPUT {
    float4 position : POSITION;	// object (or bind-pose) space
    float3 tangent : TANGENT;	// tangent frame, object space
    float3 binormal : BINORMAL;
    float3 normal : NORMAL;
    float4 uv : TEXCOORD0;	// xy texture coordinates
#if defined(OBJ_VCOLOR) || defined(CLOTH_WIND) || defined(NVR_FOLD) || defined(OBJ_FOLD_COLOR)
    float4 vertexColor : COLOR0;	// (cloth wind: its red is the mask, read in every pass so all passes move alike)
#endif
#ifdef SKIN
    float3 blendWeight : BLENDWEIGHT;	// three bone weights (the fourth is 1 - their sum)
    float4 blendIndices : BLENDINDICES;	// bone indices as colour (x 765: three rows a bone)
#endif
#if defined(CLOTH_WIND)
    float3 clothWeight : TEXCOORD7;	// UNOFFICIAL 2026-10-19 cloth mask 5: vertex stream 1, bound only for worn cloth (read only in mask 5): weight, place on the hem ring, pendulum share (2026-10-20); still cloth (mask 6, 2026-10-20): weight, the billow's side
#endif
#ifdef NVR_INSTANCED
    // UNOFFICIAL 2026-11-14 MergeCopies (core/MergeCopies.h): the per-copy vertex constants of the game's pass 0xFC from stream 1, one
    // set per copy: c0-c3 (ModelViewProj) and c25 (LightData[0], the sun direction in object space). Compiled only into NVR's
    // instanced copy of a vertex shader (October's deep/batchrender prototype, narrowed to what the 0xFC setup sets per object).
    float4 instM0 : TEXCOORD8;
    float4 instM1 : TEXCOORD9;
    float4 instM2 : TEXCOORD10;
    float4 instM3 : TEXCOORD11;
    float4 instEye : TEXCOORD12;	// (2026-11-14 v2) c16 (EyePosition): the non-Opt passes set it per object; slot 4 of MergeCopies' stream (lead[4])
    float4 instLight0 : TEXCOORD13;	// c25 (the sun direction): slot 5 (lead[5], out[20..22]); the two were swapped (review 2026-11-14)
#endif
};

// The 4-and-more-lamp variants, both stages: how many lamps they carry, where the camera-relative world position rides, and the
// one interpolator layout the vertex shader writes and the pixel shader reads (lamp vectors sampled at centroid there).
#if defined(LIGHTS) && LIGHTS >= 4
    // UNOFFICIAL MoreLampsPerObject (SPECULAR_LAMPS6, ShaderRecord): the specular LIGHTS 4 variants take 6 lamps, as many as the
    // game then gives their passes (it capped them at 2-3 and dropped the rest: lamps popped on and off objects).
    #ifndef SPECULAR_LAMPS6
        #define SPECULAR_LAMPS6 0
    #endif
    // UNOFFICIAL 2026-10-27 ManyLampsOnePass (core/ManyLamps.h; MANY_LAMPS_ON from ShaderRecord::BuildDefines, DXVK only): the shiny
    // 4-lamp-and-up shaders take up to 13 lamps. Six lamp directions filled every interpolator, so instead light2/3/4 carry the rows
    // of the tangent frame, in the vertex's own space like the game's lamp directions (light2/light3 .w still the view's y and z), and
    // the pixel shader lights each lamp of c37-c63 (object space, as the game's PSLightPosition) from lPosition. The interpolators
    // are those of MAX_LIGHTS 4 (shadowWorldPos in TEXCOORD6).
    #if defined(MANY_LAMPS_ON) && MANY_LAMPS_ON && LIGHTS == 4 && defined(SPECULAR) && SPECULAR_LAMPS6
        #define MANY_LAMPS 1
    #endif
    // lamps carried: 6 for the 5-and-more and the 6-lamp specular passes, 4 for diffuse-only 4-lamp passes, else 3
    #if defined(MANY_LAMPS)
        #define MAX_LIGHTS 4
    #elif LIGHTS >= 5 || (LIGHTS == 4 && defined(SPECULAR) && SPECULAR_LAMPS6)
        #define MAX_LIGHTS 6
    #elif !defined(SPECULAR)
        #define MAX_LIGHTS 4
    #else
        #define MAX_LIGHTS 3
    #endif

    // Where the camera-relative world position rides. At 6 lamps every TEXCOORD is taken: in light4/5/6.w (lamp radii the pixel
    // shader never reads, it takes PSLightPosition[i].w), the sentinel in lPosition.w (light 0's radius, also unread). Otherwise
    // an interpolator of its own.
    #if MAX_LIGHTS >= 5
        #define SHADOW_WP_STORE(O, v) O.light4.w = (v).x; O.light5.w = (v).y; O.light6.w = (v).z; O.lPosition.w = SHADOW_VS_SENTINEL
        #define SHADOW_WP_LOAD(I) float3((I).light4.w, (I).light5.w, (I).light6.w)
        #define SHADOW_WP_VALID(I) SHADOW_VS_PRESENT((I).lPosition.w)
    #else
        #define SHADOW_WP_DEDICATED
        #define SHADOW_WP_STORE(O, v) O.shadowWorldPos = float4((v), SHADOW_VS_SENTINEL)
        #define SHADOW_WP_LOAD(I) (I).shadowWorldPos.xyz
        #define SHADOW_WP_VALID(I) SHADOW_VS_PRESENT((I).shadowWorldPos.w)
    #endif

    #ifdef VS
        #define OBJ_MULTI_IO VS_OUTPUT
        #define OBJ_LAMP_TC(n) TEXCOORD##n
    #else
        #define OBJ_MULTI_IO PS_INPUT
        #define OBJ_LAMP_TC(n) TEXCOORD##n##_centroid
        #define OBJ_PS_SPOSITION 1	// (2026-11-14 mrfury97, port 2026-11-16) this PS_INPUT carries sPosition : POSITION (SV_Position in the no-limits build)
    #endif
    struct OBJ_MULTI_IO {
        float4 vertexColor: COLOR0;	// rgb the vertex colour (OBJECT_SMOOTH_NORMALS: a the normal's z)
        float4 fogColor: COLOR1;	// rgb fog colour, a fog amount
        float4 sPosition: POSITION;
    #if OBJECT_SMOOTH_NORMALS
        float4 uv: TEXCOORD0;	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: zw the vertex normal's xy, camera-relative world (no interpolator spare)
    #else
        float2 uv: TEXCOORD0;
    #endif
        float4 lPosition: TEXCOORD1;	// the vertex in object space
        float4 lightDir: OBJ_LAMP_TC(2);	// light 0 in tangent space; w the view vector's x
        float4 light2: OBJ_LAMP_TC(3);	// w the view vector's y
        float4 light3: OBJ_LAMP_TC(4);	// w the view vector's z
    #if MAX_LIGHTS >= 4
        float4 light4: OBJ_LAMP_TC(5);
    #endif
    #if MAX_LIGHTS >= 5
        float4 light5: OBJ_LAMP_TC(6);
        float4 light6: OBJ_LAMP_TC(7);
    #endif
    #if defined(SHADOW_WP_DEDICATED) && MAX_LIGHTS >= 4
        float4 shadowWorldPos: TEXCOORD6;
    #elif defined(SHADOW_WP_DEDICATED)
        float4 shadowWorldPos: TEXCOORD5;
    #endif
    };
#endif

// The game's vertex shader constants (both lamp layouts; constants a variant does not use stay out of its constant table).
#ifdef VS
    float3 FogColor: register(c15);
    float4 FogParam: register(c14);	// x fog far, y fog range, z fog power
    float4 LightData[10]: register(c25);	// per light: xyz position (object space), w radius
    #ifdef SKIN
        row_major float4x4 SkinModelViewProj: register(c1);
        float4 Bones[54]: register(c44);	// 3 rows per bone
    #else
        row_major float4x4 ModelViewProj: register(c0);
    #endif
    float4 EyePosition: register(c16);	// camera, object space
#endif
#ifdef NVR_INSTANCED
    // UNOFFICIAL 2026-11-14 MergeCopies: the same code with the per-copy constants read from the instance stream (VS_INPUT). The rest of
    // LightData stays the constants' (the 0xFC setup sets only LightData[0] per object). NvrInstancedLoad is written before the
    // defines, so it reads the real constants; everything after them reads the per-copy values.
    #if !defined(VS) || defined(SKIN) || (defined(LIGHTS) && LIGHTS >= 4)
        #error NVR_INSTANCED: only the rigid vertex shaders with fewer than 4 lamps have an instanced copy
    #endif
    static float4x4 InstModelViewProj;
    static float4 InstLightData[10];
    static float4 InstEyePosition;	// (2026-11-14 v2: c16 per copy too, for the non-Opt passes)
    void NvrInstancedLoad(VS_INPUT vin) {
        InstModelViewProj = float4x4(vin.instM0, vin.instM1, vin.instM2, vin.instM3);
        [unroll] for (int i = 1; i < 10; i++) InstLightData[i] = LightData[i];
        InstLightData[0] = vin.instLight0;
        InstEyePosition = vin.instEye;
    }
    #define ModelViewProj InstModelViewProj
    #define LightData InstLightData
    #define EyePosition InstEyePosition
#endif

// Pieces both pixel-shader layouts use. Macros, so the constants they read (Toggles, AmbientColor) can be declared per layout below.
#ifdef PS
    // the game's alpha test against Toggles.w, skipped when the ambient's alpha marks the pass opaque
    #ifdef OBJ_ALPHA_HASH
        // UNOFFICIAL 2026-11-03 SmoothCutouts: alphaIn becomes 1 / -1 on a hashed draw (it is written out after: the fixed-function test agrees)
        #define OBJ_ALPHA_TEST(alphaIn) alphaIn = AmbientColor.a >= 1 ? (alphaIn) : AlphaHash((alphaIn), alphaTestRef, OBJ_HASH_VPOS); clip(AmbientColor.a >= 1 ? 0 : ((alphaIn) - alphaTestRef))
    #else
    #define OBJ_ALPHA_TEST(alphaIn) clip(AmbientColor.a >= 1 ? 0 : ((alphaIn) - alphaTestRef))
    #endif
    // UNOFFICIAL 2026-11-16 far-foliage-aa (Includes/AlphaHash.hlsl AlphaCutOutput): the alpha the pass WRITES, which the game's own
    // fixed-function alpha test cuts on. The shader test above runs only on fading passes (AmbientColor.a < 1) and in the non-OPT
    // variants, so the hash and FarCoverage act here, on every variant (unmarked draws: unchanged).
    #if defined(OBJ_ALPHA_HASH)
        #ifdef OBJ_TOGGLES
            #define OBJ_CUT_OUT(a) AlphaCutOutput((a), alphaTestRef, IN.uv.xy, OBJ_HASH_VPOS)
        #else
            #define OBJ_CUT_OUT(a) AlphaCutOutput((a), 0.5f, IN.uv.xy, OBJ_HASH_VPOS)
        #endif
    #elif defined(OBJ_CUT_COVERAGE)
        #define OBJ_CUT_OUT(a) AlphaCutCoverage((a), IN.uv.xy)
    #else
        #define OBJ_CUT_OUT(a) (a)
    #endif
    #ifdef OBJ_TOGGLES
        #define OBJ_WITH_VCOLOR(c, vc) (useVertexColor <= 0 ? (c) : ((c) * (vc)))	// Toggles.x switches the vertex colour
        #define OBJ_WITH_FOG(c, f) (useFog <= 0.0 ? (c) : lerp((c), (f).rgb, (f).a))	// Toggles.y switches the fog
    #else
        #define OBJ_WITH_VCOLOR(c, vc) ((c) * (vc))
        #define OBJ_WITH_FOG(c, f) lerp((c), (f).rgb, (f).a)
    #endif
    #ifdef NVR_GBUFFER_LIT	// UNOFFICIAL 2026-11-14 the lighting-free / lamps-deferred records: 1 - the factor OBJ_WITH_FOG lerps with (light added later x this)
        #ifdef OBJ_TOGGLES
            #define OBJ_FOG_KEEP(f) (useFog <= 0.0 ? 1.0f : 1.0f - (f).a)
        #else
            #define OBJ_FOG_KEEP(f) (1.0f - (f).a)
        #endif
    #endif
    #define OBJ_UNIT_NORMAL(t) normalize(expand(t))	// normal map texel (0..1) to a unit vector
    #define OBJ_MARKER_COLOR float3(1.0f, 0.0f, 1.0f)	// SHADOW_FORCE_MARKER: magenta wherever this shader ran
#endif
