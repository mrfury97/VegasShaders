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
#if defined(PS) && defined(OPT) && defined(SPECULAR) && !defined(ONLY_SPECULAR) && defined(LIGHTS) && defined(SPECULAR_LAMPS6)
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
#include "includes/Object.hlsl"
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
#if defined(PS) && !defined(NVR_SLOT_TIGHT) && !(defined(LIGHTS) && LIGHTS > 3)
    #define SHADOW_CHARACTER_MAP_IN 1
#endif
#include "includes/Shadow.hlsl"
#include "includes/CapturedNormals.hlsl"	// UNOFFICIAL 2026-10-28 CapturedNormals (COLOR3, see there)
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
    // UNOFFICIAL 2026-10-14: rain on the first-person weapon (Wet World HudDrops, includes/HudRain.hlsl, c221-c222 set per draw;
    // compiled in only when HudDrops is on at startup: HUD_RAIN_ON, ShaderRecord::LoadShader).
    #if defined(HUD_RAIN_ON) && HUD_RAIN_ON
        #define HUD_RAIN 1
    #endif
    #include "includes/HudRain.hlsl"

    // UNOFFICIAL 2026-10-17 UnlitShadowSkip ([Shaders.PBR.Main], restart: UNLIT_SHADOW_SKIP from ShaderRecord::BuildDefines): the
    // forward sun-shadow lookups (contact hardening's blocker search + 3 x 3 PCF, 14+ shadow-map reads) only where the sun can
    // light the pixel. The sun's light is getSunLighting(lightDir, colour x shadow, ..., normal): its diffuse part is
    // saturate(N.L) x colour, exactly 0 where the bumped normal faces away from the sun (dot(normal, lightDir) <= 0: normalize
    // keeps the sign), whatever the shadow, so the lookup is skipped there. In the SPECULAR variants PBRSun keeps N.S >= 1e-5, a
    // tail of the order of 1e-4 of the sun's colour on back-facing pixels (no difference measured: evidence\objects-LOG.md).
    // The HudRain drops' sun caustic follows the geometric face, not the bumped normal, so pixels under a drop always look up.
    // Compile time: a live switch costs its branch structure even when off. Off: the shaders exactly as before.
    #ifndef UNLIT_SHADOW_SKIP
        #define UNLIT_SHADOW_SKIP 0
    #endif
    bool SunShadowWanted(float3 normalTS, float3 lightTS) {
        bool wanted = dot(normalTS, lightTS) > 0.0f;
        #if HUD_RAIN
            wanted = wanted || hudRainCover > 0.0f;
        #endif
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
#if defined(OBJ_VCOLOR) || defined(CLOTH_WIND) || defined(NVR_FOLD)
    float4 vertexColor : COLOR0;	// (cloth wind: its red is the mask, read in every pass so all passes move alike)
#endif
#ifdef SKIN
    float3 blendWeight : BLENDWEIGHT;	// three bone weights (the fourth is 1 - their sum)
    float4 blendIndices : BLENDINDICES;	// bone indices as colour (x 765: three rows a bone)
#endif
#if defined(CLOTH_WIND)
    float3 clothWeight : TEXCOORD7;	// UNOFFICIAL 2026-10-19 cloth mask 5: vertex stream 1, bound only for worn cloth (read only in mask 5): weight, place on the hem ring, pendulum share (2026-10-20); still cloth (mask 6, 2026-10-20): weight, the billow's side
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

// Pieces both pixel-shader layouts use. Macros, so the constants they read (Toggles, AmbientColor) can be declared per layout below.
#ifdef PS
    // the game's alpha test against Toggles.w, skipped when the ambient's alpha marks the pass opaque
    #define OBJ_ALPHA_TEST(alphaIn) clip(AmbientColor.a >= 1 ? 0 : ((alphaIn) - alphaTestRef))
    #ifdef OBJ_TOGGLES
        #define OBJ_WITH_VCOLOR(c, vc) (useVertexColor <= 0 ? (c) : ((c) * (vc)))	// Toggles.x switches the vertex colour
        #define OBJ_WITH_FOG(c, f) (useFog <= 0.0 ? (c) : lerp((c), (f).rgb, (f).a))	// Toggles.y switches the fog
    #else
        #define OBJ_WITH_VCOLOR(c, vc) ((c) * (vc))
        #define OBJ_WITH_FOG(c, f) lerp((c), (f).rgb, (f).a)
    #endif
    #define OBJ_UNIT_NORMAL(t) normalize(expand(t))	// normal map texel (0..1) to a unit vector
    #define OBJ_MARKER_COLOR float3(1.0f, 0.0f, 1.0f)	// SHADOW_FORCE_MARKER: magenta wherever this shader ran
#endif

#if defined(VS) && LIGHTS < 4

struct VS_OUTPUT {
#ifdef OBJ_VCOLOR
    float4 vertexColor : COLOR0;	// rgb vertex colour
#endif
#if defined(NVR_FOLD) && !defined(OBJ_VCOLOR)
    float4 foldColor : COLOR0;	// UNOFFICIAL FoldHighlights: the vertex colour (the light-only passes had none)
#endif
#ifdef OBJ_FOG
    float4 fogColor : COLOR1;	// rgb fog colour, a fog amount
#endif
    float4 sPosition : POSITION;	// clip space
    float4 uv : TEXCOORD0; // .zw = local barrel position xy
    float4 lightDir : TEXCOORD1;	// light 0, tangent space

#ifdef OBJ_LAMP2
    float4 light2Dir : TEXCOORD2;	// light 1, tangent space; w its radius
#endif
    
#ifdef OBJ_LAMP3
    float4 light3Dir : TEXCOORD3;	// light 2, tangent space; w its radius
#endif
    
    float4 viewDir : TEXCOORD6; // .w = local barrel position z

    // Object-space squared distances for point-light attenuation (vanillaAttSq), bypassing
    // lightDir/light2Dir/light3Dir above -- those are tangent-space (TBN-transformed) and their
    // length is only correct if the TBN basis is orthonormal. .x = light0 (DIFFUSE/POINT only,
    // where light0 is itself a point light rather than the sun), .y = light2, .z = light3.
    // METAL_UP: .w = x of world up in tangent space (with lightDir.w and vertexColor.a, see main).
    float4 lightDistSq : TEXCOORD5;

    // TEXCOORD4 is free at LIGHTS < 4. .w carries SHADOW_VS_SENTINEL.
    float4 shadowWorldPos : TEXCOORD4;	// camera-relative world position; w SHADOW_VS_SENTINEL

#ifdef PROJ_SHADOW
    float4 shadowUVs : TEXCOORD7;	// xy canopy shadow map, zw its mask
#endif
#ifdef OBJ_SMOOTH_NORMAL_INTERP
    float4 smoothNormal : TEXCOORD8;	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal, camera-relative world; .w = SHADOW_VS_SENTINEL
#endif
};

#ifdef PROJ_SHADOW
    row_major float4x4 ShadowProj : register(c18);	// object space to the canopy shadow map
    float4 ShadowProjData : register(c22);	// xy origin, w size of the mask
    float4 ShadowProjTransform : register(c23);	// scale and offset of the projection
#endif


VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vout;

    // Zeroed unconditionally: the DIFFUSE/POINT (.x) and LIGHTS>1/LIGHTS>2 (.y/.z) blocks below
    // only overwrite the components they actually use, and vs_3_0 requires every component of
    // vout to be written before return -- a component that stays at this default is simply never
    // read by the PS either (same macro guards on both sides).
    vout.lightDistSq = 0;

    vout.uv.xy = vin.uv.xy;
    
    float4 pos = vin.position.xyzw;
    
    #ifndef SKIN
        float3x3 toTangent = float3x3(vin.tangent.xyz, vin.binormal.xyz, vin.normal.xyz);
        #ifdef CLOTH_WIND
            ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, float3(vin.clothWeight.y, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f);	// UNOFFICIAL cloth wind (2026-10-20: still cloth's mask 6 from stream 1)
        #endif
        #ifdef PLANT_WIND
            [branch] if (PlantA.x < -0.5f) pos.xyz += PlantWindMove(pos.xyz);	// UNOFFICIAL 2026-10-20 plant wind (static plants)
        #endif
    
        vout.sPosition.xyzw = mul(ModelViewProj, pos.xyzw);
    #else
        float4 boneOff = vin.blendIndices.zyxw * 765.01001;
        float4 boneW = vin.blendWeight.xyzz;
        boneW.w = 1 - weight(vin.blendWeight.xyz);
        float3x3 toTangent = BonesTransformTBN(Bones, boneOff, boneW, vin.tangent, vin.binormal, vin.normal);
    
        pos.w = 1;
        pos.xyz = BonesTransformPosition(Bones, boneOff, boneW, pos);
        pos.xyz = PipboyFitPosition(pos.xyz, boneOff, boneW);	// UNOFFICIAL 2026-11-03 PipboyFit (includes/PipboyFit.hlsl)
        #ifdef CLOTH_WIND
            float3 clothOut = 0.0f;	// mask 5's billow direction, carried by the same bones
            float4 clothLegL = 0.0f, clothLegR = 0.0f;	// mask 5 (round 8): each leg (ClothLegBind), carried by the same bones
            float3 clothUp = 0.0f;	// ...and the bind pose's up (the legs' direction)
            [branch] if (ClothIsMask5()) {
                [branch] if (ClothWalk.x > 0.5f) {	// 2026-10-20 round 9: hangs from the hips, the hem ring, the current legs
                    const float hw = saturate(vin.clothWeight.x) * ClothWalk.y;
                    pos.xyz = lerp(pos.xyz, ClothPelvisPoint(vin.position.xyz), hw);
                    toTangent = float3x3(normalize(lerp(toTangent[0], normalize(ClothPelvisVector(vin.tangent)), hw)),
                                   normalize(lerp(toTangent[1], normalize(ClothPelvisVector(vin.binormal)), hw)),
                                   normalize(lerp(toTangent[2], normalize(ClothPelvisVector(vin.normal)), hw)));
                    clothOut = ClothPelvisVector(ClothOutwardBind(vin.position.xyz));
                    clothUp = ClothPelvisVector(float3(0.0f, 0.0f, 1.0f));
                    clothLegL = ClothLegBind(pos.xyz, ClothLegNow[0], ClothLegNow[1], ClothLegNow[2]);
                    clothLegR = ClothLegBind(pos.xyz, ClothLegNow[3], ClothLegNow[4], ClothLegNow[5]);
                }
                else {
                    clothOut = BonesTransformVector(Bones, boneOff, boneW, ClothOutwardBind(vin.position.xyz));
                    const float4 legL = ClothLegBind(vin.position.xyz, ClothLimb[0], ClothLimb[1], ClothLimb[2]);
                    const float4 legR = ClothLegBind(vin.position.xyz, ClothLimb[4], ClothLimb[5], ClothLimb[6]);
                    clothLegL = float4(BonesTransformVector(Bones, boneOff, boneW, legL.xyz), legL.w);
                    clothLegR = float4(BonesTransformVector(Bones, boneOff, boneW, legR.xyz), legR.w);
                    clothUp = BonesTransformVector(Bones, boneOff, boneW, float3(0.0f, 0.0f, 1.0f));
                }
            }
            ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, clothOut, clothLegL, clothLegR, clothUp);	// UNOFFICIAL cloth wind
            [branch] if (ClothIsMask5() && ClothWalk.x > 0.5f) {	// round 9: the hem ring by the pendulum share, then out of the current legs
                pos.xyz += ClothRingAt(vin.clothWeight.y) * vin.clothWeight.z;
                pos.xyz += ClothLegPush(pos.xyz, clothOut);
            }
        #endif
    
        vout.sPosition.xyzw = mul(SkinModelViewProj, pos.xyzw);
    #endif
    vout.uv.zw = pos.xy;
    
    #ifdef OBJ_LAMP_FIRST
        float3 lPos = LightData[0].xyz - pos.xyz;
        vout.lightDistSq.x = dot(lPos, lPos);
    #else
        float3 lPos = LightData[0].xyz;
    #endif

    vout.lightDir.w = LightData[0].w;
    #ifdef OBJ_LAMP_FIRST
        vout.lightDir.xyz = LAMP_VS_DIR(toTangent, lPos);	// UNOFFICIAL LampDistanceFix (see the top): a lamp here
    #else
        vout.lightDir.xyz = mul(toTangent, lPos);
    #endif

    vout.viewDir.xyz = mul(toTangent, EyePosition.xyz - pos.xyz);
    vout.viewDir.w = pos.z;

    #ifdef OBJ_LAMP2
        lPos = LightData[1].xyz - pos.xyz;
        vout.light2Dir.w = LightData[1].w;
        vout.light2Dir.xyz = LAMP_VS_DIR(toTangent, lPos);	// UNOFFICIAL LampDistanceFix
        vout.lightDistSq.y = dot(lPos, lPos);
    #endif

    #ifdef OBJ_LAMP3
        lPos = LightData[2].xyz - pos.xyz;
        vout.light3Dir.w = LightData[2].w;
        vout.light3Dir.xyz = LAMP_VS_DIR(toTangent, lPos);	// UNOFFICIAL LampDistanceFix
        vout.lightDistSq.z = dot(lPos, lPos);
    #endif
    
    #ifdef OBJ_VCOLOR
        vout.vertexColor = clamp(vin.vertexColor, 0.0f, 1.0f);
    #endif
    #if defined(NVR_FOLD) && !defined(OBJ_VCOLOR)
        vout.foldColor = clamp(vin.vertexColor, 0.0f, 1.0f);	// UNOFFICIAL FoldHighlights
    #endif

    #ifdef OBJ_FOG
        float3 fogEye = vout.sPosition.xyz;
    
        #ifdef REVERSED_DEPTH
            fogEye.z = vout.sPosition.w - fogEye.z;
        #endif
    
        float fogT = 1 - saturate((FogParam.x - length(fogEye)) / FogParam.y);
        fogT = log2(fogT);
        vout.fogColor.a = exp2(fogT * FogParam.z);
        vout.fogColor.rgb = FogColor.rgb;
    #endif
    
    #ifdef PROJ_SHADOW
        float projW = dot(ShadowProj[3].xyzw, pos.xyzw);
        float2 projUV;
        projUV.x = dot(ShadowProj[0].xyzw, pos.xyzw);
        projUV.y = dot(ShadowProj[1].xyzw, pos.xyzw);
        vout.shadowUVs.xy = ((projW * ShadowProjTransform.xy) + projUV) / (projW * ShadowProjTransform.w);
        vout.shadowUVs.zw = ((projUV.xy - ShadowProjData.xy) / ShadowProjData.w) * float2(1, -1) + float2(0, 1);
    #endif

    // Model-space shaders: recover a camera-relative world pos from the clip pos.
    // Written unconditionally, or the interpolator is left undefined.
    vout.shadowWorldPos = float4(GetShadowWorldPos(vout.sPosition), SHADOW_VS_SENTINEL);
    #ifdef OBJ_SMOOTH_NORMAL_INTERP
        // UNOFFICIAL 2026-10-24 SmoothObjectNormals: tbn[2] is the vertex normal in the space pos is in (model, or the bones' with SKIN)
        #ifndef SKIN
            vout.smoothNormal = GetShadowSmoothNormal(ModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz);
        #else
            vout.smoothNormal = GetShadowSmoothNormal(SkinModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz);
        #endif
    #endif

    #ifdef METAL_UP
        // UNOFFICIAL: which way is world up in this vertex's tangent space, so the pixel shader can tell how far a
        // reflected view points at the sky (Object.hlsl getMetalAmbient). The world z of an object-space direction d is
        // d . objUp, objUp = upView x ModelViewProj, with upView the camera's inverse projection times the up column of
        // its inverse view (GetShadowWorldPos's matrices; exact for the world, a little off for the first-person model
        // when its field of view differs). The spare channels carry it: lightDir.w (read only by the DIFFUSE and POINT
        // passes), lightDistSq.w and vertexColor.a (unused by the pixel shader; COLOR0 is clamped, so encoded).
        float4 upView = mul(TESR_InvProjectionTransform, float4(TESR_InvViewTransform[0][2], TESR_InvViewTransform[1][2], TESR_InvViewTransform[2][2], 0.0f));
        #ifndef SKIN
            float3 objUp = mul(upView, ModelViewProj).xyz;
        #else
            float3 objUp = mul(upView, SkinModelViewProj).xyz;
        #endif
        float3 upTangent = normalize(mul(toTangent, objUp));
        vout.lightDistSq.w = upTangent.x;
        vout.lightDir.w = upTangent.y;
        vout.vertexColor.a = upTangent.z * 0.5f + 0.5f;
    #endif
    #ifdef OBJ_SMOOTH_NORMAL_W
        {	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal, camera-relative world, in METAL_UP's three channels
            // (2026-10-26: a plain vector, no longer octahedral: ShadowPlainNormal)
            #ifndef SKIN
                const float3 smoothN = GetShadowSmoothNormal(ModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz).xyz;
            #else
                const float3 smoothN = GetShadowSmoothNormal(SkinModelViewProj, pos.xyz, toTangent[2], vout.shadowWorldPos.xyz).xyz;
            #endif
            vout.lightDistSq.w = smoothN.x;
            vout.lightDir.w = smoothN.y;
            vout.vertexColor.a = smoothN.z * 0.5f + 0.5f;
        }
    #endif

    return vout;
};

#elif defined(VS) && LIGHTS >= 4

#ifdef OBJ_TOGGLES
    float4 fvars0 : register(c17);	// the game's, unused here

    #define lightOffset 0	// first light slot that is a lamp
#else
    #define lightOffset 1	// first light slot that is a lamp (slot 0 is the sun)
#endif

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vout;
    
    vout.uv.xy = vin.uv.xy;
    
    float4 pos = vin.position.xyzw;
    
    #ifndef SKIN
    float3x3 toTangent = float3x3(vin.tangent.xyz, vin.binormal.xyz, vin.normal.xyz);
    #ifdef CLOTH_WIND
        ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, float3(vin.clothWeight.y, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f);	// UNOFFICIAL cloth wind (2026-10-20: still cloth's mask 6 from stream 1)
    #endif
    #ifdef PLANT_WIND
        [branch] if (PlantA.x < -0.5f) pos.xyz += PlantWindMove(pos.xyz);	// UNOFFICIAL 2026-10-20 plant wind (static plants)
    #endif
    
    vout.sPosition.xyzw = mul(ModelViewProj, pos.xyzw);
    #else
        float4 boneOff = vin.blendIndices.zyxw * 765.01001;
        float4 boneW = vin.blendWeight.xyzz;
        boneW.w = 1 - weight(vin.blendWeight.xyz);
        float3x3 toTangent = BonesTransformTBN(Bones, boneOff, boneW, vin.tangent, vin.binormal, vin.normal);
    
        pos.w = 1;
        pos.xyz = BonesTransformPosition(Bones, boneOff, boneW, pos);
        pos.xyz = PipboyFitPosition(pos.xyz, boneOff, boneW);	// UNOFFICIAL 2026-11-03 PipboyFit (includes/PipboyFit.hlsl)
        #ifdef CLOTH_WIND
            float3 clothOut = 0.0f;	// mask 5's billow direction, carried by the same bones
            float4 clothLegL = 0.0f, clothLegR = 0.0f;	// mask 5 (round 8): each leg (ClothLegBind), carried by the same bones
            float3 clothUp = 0.0f;	// ...and the bind pose's up (the legs' direction)
            [branch] if (ClothIsMask5()) {
                [branch] if (ClothWalk.x > 0.5f) {	// 2026-10-20 round 9: hangs from the hips, the hem ring, the current legs
                    const float hw = saturate(vin.clothWeight.x) * ClothWalk.y;
                    pos.xyz = lerp(pos.xyz, ClothPelvisPoint(vin.position.xyz), hw);
                    toTangent = float3x3(normalize(lerp(toTangent[0], normalize(ClothPelvisVector(vin.tangent)), hw)),
                                   normalize(lerp(toTangent[1], normalize(ClothPelvisVector(vin.binormal)), hw)),
                                   normalize(lerp(toTangent[2], normalize(ClothPelvisVector(vin.normal)), hw)));
                    clothOut = ClothPelvisVector(ClothOutwardBind(vin.position.xyz));
                    clothUp = ClothPelvisVector(float3(0.0f, 0.0f, 1.0f));
                    clothLegL = ClothLegBind(pos.xyz, ClothLegNow[0], ClothLegNow[1], ClothLegNow[2]);
                    clothLegR = ClothLegBind(pos.xyz, ClothLegNow[3], ClothLegNow[4], ClothLegNow[5]);
                }
                else {
                    clothOut = BonesTransformVector(Bones, boneOff, boneW, ClothOutwardBind(vin.position.xyz));
                    const float4 legL = ClothLegBind(vin.position.xyz, ClothLimb[0], ClothLimb[1], ClothLimb[2]);
                    const float4 legR = ClothLegBind(vin.position.xyz, ClothLimb[4], ClothLimb[5], ClothLimb[6]);
                    clothLegL = float4(BonesTransformVector(Bones, boneOff, boneW, legL.xyz), legL.w);
                    clothLegR = float4(BonesTransformVector(Bones, boneOff, boneW, legR.xyz), legR.w);
                    clothUp = BonesTransformVector(Bones, boneOff, boneW, float3(0.0f, 0.0f, 1.0f));
                }
            }
            ClothWindTBN(pos.xyz, toTangent, vin.vertexColor, vin.position.xyz, vin.clothWeight.x, clothOut, clothLegL, clothLegR, clothUp);	// UNOFFICIAL cloth wind
            [branch] if (ClothIsMask5() && ClothWalk.x > 0.5f) {	// round 9: the hem ring by the pendulum share, then out of the current legs
                pos.xyz += ClothRingAt(vin.clothWeight.y) * vin.clothWeight.z;
                pos.xyz += ClothLegPush(pos.xyz, clothOut);
            }
        #endif
        vout.sPosition.xyzw = mul(SkinModelViewProj, pos.xyzw);
    #endif
    
    float3 viewDir = mul(toTangent, EyePosition.xyz - pos.xyz);
    
    vout.lPosition.xyz = pos.xyz;
    vout.lPosition.w = LightData[0].w;
    
    #ifdef OBJ_TOGGLES
        float lightCount = min(MAX_LIGHTS, fvars0.z);
    #elif defined(SPECULAR)
        float lightCount = min(MAX_LIGHTS, EyePosition.w);
    #else
        float lightCount = min(MAX_LIGHTS, LightData[0].w);
    #endif
    float lightFrac = frac(lightCount);
    float lightThr = (lightCount < 0.0 ? (-lightFrac < lightFrac ? 1.0 : 0.0) : 0) + (lightCount - lightFrac);
    float lightOn;
    
    #ifdef OBJ_TOGGLES
        vout.lightDir.w = viewDir.x;
        vout.lightDir.xyz = mul(toTangent, LightData[0].xyz);
    #else
        lightOn = 0 < lightThr ? 1.0 : 0.0;
        vout.lightDir.xyz = lightOn * mul(toTangent, LightData[lightOffset + 0].xyz - pos.xyz);
        vout.lightDir.w = viewDir.x;
    #endif
    
    #ifdef MANY_LAMPS
        // UNOFFICIAL ManyLampsOnePass: the tangent frame's rows (written at the end); the view as before.
        vout.light2 = float4(0.0f, 0.0f, 0.0f, viewDir.y);
        vout.light3 = float4(0.0f, 0.0f, 0.0f, viewDir.z);
        vout.light4 = 0.0f;
    #else
    lightOn = 1 < lightThr ? 1.0 : 0.0;
    vout.light2.xyz = lightOn * mul(toTangent, LightData[lightOffset + 1].xyz - pos.xyz);
    vout.light2.w = viewDir.y;

    lightOn = 2 < lightThr ? 1.0 : 0.0;
    vout.light3.xyz = lightOn * mul(toTangent, LightData[lightOffset + 2].xyz - pos.xyz);
    vout.light3.w = viewDir.z;

    #if MAX_LIGHTS >= 4
        lightOn = 3 < lightThr ? 1.0 : 0.0;
        vout.light4.xyz = lightOn * mul(toTangent, LightData[lightOffset + 3].xyz - pos.xyz);
        vout.light4.w = lightOn * LightData[lightOffset + 3].w;
    #endif
    #endif
    
    #if MAX_LIGHTS >= 5
        lightOn = 4 < lightThr ? 1.0 : 0.0;
        vout.light5.xyz = lightOn * mul(toTangent, LightData[lightOffset + 4].xyz - pos.xyz);
        vout.light5.w = lightOn * LightData[lightOffset + 4].w;
        
        lightOn = 5 < lightThr ? 1.0 : 0.0;
        vout.light6.xyz = lightOn * mul(toTangent, LightData[lightOffset + 5].xyz - pos.xyz);
        vout.light6.w = lightOn * LightData[lightOffset + 5].w;
    #endif
    
    vout.vertexColor = clamp(vin.vertexColor, 0.0f, 1.0f);

    float3 fogEye = vout.sPosition.xyz;
    #ifdef REVERSED_DEPTH
        fogEye.z = vout.sPosition.w - fogEye.z;
    #endif
    float fogT = 1 - saturate((FogParam.x - length(fogEye)) / FogParam.y);
    fogT = log2(fogT);
    vout.fogColor.a = exp2(fogT * FogParam.z);
    vout.fogColor.rgb = FogColor.rgb;

    // Through SHADOW_WP_STORE: at MAX_LIGHTS 6 this overwrites light4/5/6's .w, which the PS
    // ignores.
    float3 shadowWorldPos = GetShadowWorldPos(vout.sPosition);
    SHADOW_WP_STORE(vout, shadowWorldPos);
    #if OBJECT_SMOOTH_NORMALS
        // UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal (tbn[2], in pos's space) in camera-relative world, in uv.zw and
        // vertexColor.a (2026-10-26: a plain vector, no longer octahedral: ShadowPlainNormal)
        #ifndef SKIN
            const float3 smoothN = GetShadowSmoothNormal(ModelViewProj, pos.xyz, toTangent[2], shadowWorldPos).xyz;
        #else
            const float3 smoothN = GetShadowSmoothNormal(SkinModelViewProj, pos.xyz, toTangent[2], shadowWorldPos).xyz;
        #endif
        vout.uv.zw = smoothN.xy;
        vout.vertexColor.a = smoothN.z * 0.5f + 0.5f;
    #endif
    #ifdef MANY_LAMPS
        // UNOFFICIAL ManyLampsOnePass: the frame's rows; the pixel shader takes mul(tbn, lamp - pos) per pixel, as this shader did per vertex.
        vout.light2.xyz = toTangent[0];
        vout.light3.xyz = toTangent[1];
        vout.light4.xyz = toTangent[2];
    #endif

    return vout;
};
#endif // Vertex shaders.

#if defined(PS) && (!defined(LIGHTS) || LIGHTS < 4)

struct PS_INPUT {
#if defined(NVR_FOLD) && !defined(OBJ_VCOLOR)
    float4 foldColor : COLOR0;	// UNOFFICIAL FoldHighlights: the vertex colour
#endif
#ifdef NVR_LAMP_SUMS
#if defined(NVR_SM4) && NVR_SM4
    float4 vpos4 : VPOS;	// the no-limits build (NVR DXVK fork, TerrainTemplate.hlsl): SV_Position, the pixel centre
    #define NVR_IN_VPOS (IN.vpos4.xy - 0.5f)
#else
    float2 vpos : VPOS;	// UNOFFICIAL LampSums: the pixel, to read the run's copy of the frame
    #define NVR_IN_VPOS IN.vpos
#endif
#endif
#ifdef OBJ_VCOLOR
    float4 vertexColor : COLOR0;    // METAL: .a = world up's z in tangent space, encoded (see the VS)
#endif
#ifdef OBJ_FOG
    float4 fogColor : COLOR1;	// rgb fog colour, a fog amount
#endif
    float4 uv : TEXCOORD0;	// xy texture coordinates
    float4 lightDir : TEXCOORD1_centroid;	// light 0, tangent space
#ifdef OBJ_LAMP2
    float4 light2Dir : TEXCOORD2_centroid;	// light 1, tangent space; w its radius
#endif
#ifdef OBJ_LAMP3
    float4 light3Dir : TEXCOORD3_centroid;	// light 2, tangent space; w its radius
#endif
    float4 viewDir : TEXCOORD6_centroid;
    float4 lightDistSq : TEXCOORD5;    // METAL: .w = world up's x in tangent space
    float4 shadowWorldPos : TEXCOORD4;	// camera-relative world position; w SHADOW_VS_SENTINEL
#ifdef PROJ_SHADOW
    float4 shadowUVs : TEXCOORD7;	// xy canopy shadow map, zw its mask
#endif
#ifdef OBJ_SMOOTH_NORMAL_INTERP
    float4 smoothNormal : TEXCOORD8;	// UNOFFICIAL 2026-10-24 SmoothObjectNormals
#endif
};

struct PS_OUTPUT {
    float4 color : COLOR0;
#ifdef ONLY_SPECULAR
    // UNOFFICIAL 2026-10-19 NewSkin: a copy of COLOR0. The game draws its own highlight passes on skin with these shaders;
    // while the screen-space skin blur runs, the SkinShader hook (Hooks/Shaders.cpp) binds the blur's skin target here, so
    // the same highlights add to its highlight share as to the scene: the blur takes them out before it blurs and puts them
    // back after, as it does the skin shader's own (Effects/SkinScattering.fx.hlsl). Anywhere else it is discarded.
    float4 highlightCopy : COLOR1;
#endif
#if CAPTURED_NORMALS_ON
    // (ps_3_0 needs COLOR0..COLOR3 contiguous: COLOR2 written as 0, discarded where nothing is bound there; COLOR1 is highlightCopy)
    #if !defined(ONLY_SPECULAR)
    float4 capturedPad1 : COLOR1;
    #endif
    float4 capturedPad2 : COLOR2;
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

#ifndef OBJ_AMBIENT
    sampler2D NormalMap : register(s0);	// passes without the colour texture take the normal map first
    #if defined(NVR_FOLD) && defined(DIFFUSE)
        sampler2D FoldBaseMap : register(s7);	// UNOFFICIAL FoldHighlights: the object's base texture, bound by NVR for folded draws (core\FoldHighlights.h kBaseStage)
    #endif
#else
    sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
    sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
#endif

#ifdef LINEAR_LIGHTING
    #ifdef OBJ_AMBIENT
        float4 AmbientColorIn : register(c1);
        static float4 AmbientColor;	// UNOFFICIAL linear lighting: AmbientColorIn decoded (rgb), at the start of main
    #endif
    float4 PSLightColorIn[10] : register(c3);
    static float4 PSLightColor[10];	// UNOFFICIAL linear lighting: PSLightColorIn decoded (rgb; .a as it is), at the start of main
    #ifdef LIN_PREDECODED
        float4 AmbientColorDec : register(c64);		// UNOFFICIAL LinearPreDecode: c1 decoded on the CPU (core/LinearPreDecode.h)
        float4 PSLightColorDec[10] : register(c66);	// c3-c12 decoded on the CPU
    #endif
#else
    #ifdef OBJ_AMBIENT
        float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
    #endif
    float4 PSLightColor[10] : register(c3);	// rgb per light (sun first where there is one)
#endif

#if (defined(SI) || defined(HAIR)) && !defined(ONLY_SPECULAR)
    #ifdef ONLY_LIGHT
        sampler2D GlowMap : register(s3);	// glow (SI) or hair tint (HAIR) map
    #else
        sampler2D GlowMap : register(s4);	// glow (SI) or hair tint (HAIR) map
    #endif
    float4 EmittanceColor : register(c2);	// glow or hair colour
#endif

#ifdef PROJ_SHADOW
    #ifdef ONLY_SPECULAR
        sampler2D ShadowMap : register(s4);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s5);	// where that shadow applies
    #elif defined(ONLY_LIGHT)
        sampler2D ShadowMap : register(s5);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s6);	// where that shadow applies
    #else
        sampler2D ShadowMap : register(s6);	// the game's projected (canopy) shadow
        sampler2D ShadowMaskMap : register(s7);	// where that shadow applies
    #endif
#endif

#ifdef OBJ_TOGGLES
    float4 Toggles : register(c27);	// x vertex colour on, y fog on, z gloss power, w alpha test reference
#endif

#ifdef OBJ_LAMP_FIRST
    float4 TESR_LampPassTrim : register(c196);   // UNOFFICIAL TrimLampPasses: x > 0 = on (pinned clear of the game's own registers)
#endif


PS_OUTPUT main(PS_INPUT IN) {
    #if SHADOW_CONTRAST_ON
        float3 scLost = 0.0f;	// UNOFFICIAL ShadowContrast: the light the lamps' shadows take here (SC_SPLIT)
    #endif
    PS_OUTPUT pout;
    
#ifdef LINEAR_LIGHTING
    // UNOFFICIAL linear lighting: the light colours decoded.
    #ifdef LIN_PREDECODED	// UNOFFICIAL LinearPreDecode: decoded on the CPU
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++) PSLightColor[lightIndex] = float4(PSLightColorDec[lightIndex].rgb, PSLightColorIn[lightIndex].a);	// (.a from the game's register: keeps it reserved)
    #else
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++)
        PSLightColor[lightIndex] = float4(LinColor(PSLightColorIn[lightIndex].rgb), PSLightColorIn[lightIndex].a);
    #endif
    #ifdef OBJ_AMBIENT
        #ifdef LIN_PREDECODED
            AmbientColor = float4(AmbientColorDec.rgb, AmbientColorIn.a);
        #else
        AmbientColor = float4(LinColor(AmbientColorIn.rgb), AmbientColorIn.a);
        #endif
    #endif
#endif

    // UNOFFICIAL: rain drops on the first-person weapon (includes/HudRain.hlsl), before the textures are read: they are read
    // through the drops (HUD_RAIN_TEX).
    HudRain(IN.uv.xy, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));

#ifdef OBJ_AD_PASS
    float3 heatMaterialFactor = 1.0f;
#endif
    #ifdef OBJ_AMBIENT
        float4 surface = HUD_RAIN_TEX(BaseMap, IN.uv.xy);
    
        #ifdef ONLY_LIGHT
            // Keep the sampled albedo before the lighting-only path discards it.
            // Bound the reciprocal on black texels to avoid extreme HDR values.
            heatMaterialFactor = max(surface.rgb, 0.03f);
            #ifdef NVR_FOLD
                const float3 foldTexture = surface.rgb;	// UNOFFICIAL FoldHighlights: the colour the texture pass multiplies in
            #endif
            #if CAPTURED_NORMALS_ON >= 3
                const float3 capBaseTex = surface.rgb;	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the texture the TEXTURE pass multiplies in (see the output)
            #endif
            surface.rgb = 1;
        #endif
    #else
        float4 surface = 1;
    #endif
    
    #if defined(OBJ_TOGGLES) && !defined(ONLY_SPECULAR)
        OBJ_ALPHA_TEST(surface.a);
    #endif
    
    float4 normal = HUD_RAIN_TEX(NormalMap, IN.uv.xy);
    normal.xyz = OBJ_UNIT_NORMAL(normal.xyz);
    
    #ifdef RMAOS
        RmaosRead(IN.uv.xy);	// UNOFFICIAL _rmaos map (Object.hlsl): read once, here at top level
    #endif
    float rough = getRoughness(normal.a);

    // Geometric specular AA -- see the comment on SpecularAA itself. Unconditional: this is a
    // quality fix for high-frequency normal maps (hair chief among them), not a debug toggle,
    // and ddx/ddy have to run here at top level regardless of anything below.
    rough = SpecularAA(normal.xyz, rough);
    // UNOFFICIAL 2026-10-13: blood on first-person geometry (Includes/HandBlood.hlsl); derivatives, so at top level.
    HandBlood(IN.uv.xy, IN.shadowWorldPos.xyz, SHADOW_VS_PRESENT(IN.shadowWorldPos.w));
    rough = HandBloodRoughness(rough);
    normal.xyz = HandBloodNormal(normal.xyz);
    rough = HudRainRoughness(rough);	// UNOFFICIAL: the rain drops, wet and rounded
    normal.xyz = HudRainNormal(normal.xyz);
    SetDiffuseEnergyKeep(normal.xyz, IN.viewDir.xyz, rough);	// UNOFFICIAL 2026-10-16 EnergySplit (Includes/PBR.hlsl): once per pixel, every lamp reuses it
    #ifdef NVR_FOLD
        // UNOFFICIAL FoldHighlights: at top level (ddx/ddy): the metal finish, and the base texture's gradients for the extra-lamp passes.
        const float foldMetalRoughness = SpecularAA(normal.xyz, getMetalRoughness(normal.a));
        #ifdef DIFFUSE
            const float2 foldUvDx = ddx(IN.uv.xy), foldUvDy = ddy(IN.uv.xy);
        #endif
    #endif

    //if (TESR_DebugVar.y > 0.0) {
    //    pout.color.a = 1;
    //    if (TESR_DebugVar.y > 0.1)
    //        pout.color.rgb = rough.xxx;
    //    else
    //        pout.color.rgb = normal.aaa;
    //    return pout;
    //}
    
    #ifdef OBJ_VCOLOR
        #ifdef HAIR
            float4 glowTexel = tex2D(GlowMap, IN.uv.xy);
            surface.rgb = (2 * ((IN.vertexColor.g * (EmittanceColor.rgb - 0.5)) + 0.5)) * lerp(surface.rgb, glowTexel.rgb, glowTexel.a);
        #else
            surface.rgb = OBJ_WITH_VCOLOR(surface.rgb, IN.vertexColor.rgb);
        #endif
    #endif
    #if !defined(DIFFUSE) && !defined(ONLY_SPECULAR) && !defined(ONLY_LIGHT)
        surface.rgb = HandBloodAlbedo(surface.rgb);	// UNOFFICIAL: the blood's colour (after the vertex colour: blood is not tinted by it)
    #elif HAND_BLOOD && !defined(ONLY_SPECULAR)
        // UNOFFICIAL 2026-10-13 (user: the blood on the gun looks clear in some lighting): the lighting-only passes (ONLY_LIGHT,
        // and the DIFFUSE lamp passes added to them) draw the light alone, and a later pass multiplies in the gun's texture, so
        // the blood's colour above never reached them: only its gloss and bumps showed. Here the light is scaled so that,
        // once multiplied by the texture, the blood comes out its own colour.
        #ifdef DIFFUSE
            // (No texture in these passes: the gun's texture taken as a dark grey, 0.25.)
            surface.rgb = handBloodCover > 0.0f ? HandBloodAlbedo((0.25f).xxx) * 4.0f : surface.rgb;
        #else
            surface.rgb = handBloodCover > 0.0f ? HandBloodAlbedo(heatMaterialFactor) / heatMaterialFactor : surface.rgb;	// (exactly as before where there is no blood)
        #endif
    #endif
    // UNOFFICIAL linear lighting: the albedo decoded for the lighting (where the texture is multiplied in by a later pass,
    // the factor above is, which comes out the same). The metal guess and the debug views keep the stored colour.
    const float3 albedoStored = surface.rgb;
    surface.rgb = LinColor(surface.rgb);
    surface.rgb *= HudRainLens(float3(0.0f, 0.0f, 1.0f)).x;	// UNOFFICIAL: a drop's rim bends the light away (darker)

    #ifdef METAL
        // UNOFFICIAL: metal (Object.hlsl), only while [Shaders.PBR.Metal] is on for this pass; see the lighting below.
        // Its finish, specular-anti-aliased here at top level (ddx/ddy).
        float metalRoughness = SpecularAA(normal.xyz, getMetalRoughness(normal.a));
        float metallic = 0.0f;
    #endif

    // Vanilla shadows.
    float3 sunAndProj = 1.0;
    #ifdef STBB
        sunAndProj = 0.85;
    #elif defined(PROJ_SHADOW)
        float3 projShade = tex2D(ShadowMap, IN.shadowUVs.xy).xyz;
        float projMask = tex2D(ShadowMaskMap, IN.shadowUVs.zw).x;
        sunAndProj = lerp(1, projShade, projMask);
    #endif
    
    // Applied to PSLightColor[0], the sun, only: ambient, emittance and point lights are
    // untouched. ddx/ddy must stay at top level, outside any dynamic branch.
    //
    // Computed once here and reused by the ambient block further down (that block's condition,
    // !DIFFUSE && !ONLY_SPECULAR, is a strict subset of this one, since POINT implies
    // ONLY_SPECULAR) -- two separate ddx/ddy evaluations of the identical
    // GetShadowGeometricNormal(IN.shadowWorldPos.xyz) call previously coexisted in the same
    // pixel shader whenever FORWARD_SHADOWS was compiled in, which is fragile enough on its own
    // to corrupt unrelated interpolator reads placed nearby.
    #ifndef OBJ_LAMP_FIRST
        #ifdef OBJ_SMOOTH_NORMAL_INTERP
            float3 faceNormal = ShadowFacingNormal(IN.smoothNormal.xyz, IN.shadowWorldPos.xyz);	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the vertex normal
        #elif defined(OBJ_SMOOTH_NORMAL_W)
            float3 faceNormal = ShadowFacingNormal(ShadowPlainNormal(float2(IN.lightDistSq.w, IN.lightDir.w), IN.vertexColor.a), IN.shadowWorldPos.xyz);	// UNOFFICIAL 2026-10-24
        #else
            float3 faceNormal = GetShadowGeometricNormal(IN.shadowWorldPos.xyz);
        #endif
        // Decline to projShade if a vanilla vertex shader ran: the interpolator is undefined.
        float sunVis = 1.0f;
        #if FORWARD_SHADOWS
        #if UNLIT_SHADOW_SKIP
        // UNOFFICIAL 2026-10-17 UnlitShadowSkip: no projShade-map lookups where the sun cannot light the pixel (SunShadowWanted).
        [branch] if (SHADOW_VS_PRESENT(IN.shadowWorldPos.w) && SunShadowWanted(normal.xyz, IN.lightDir.xyz))
            sunVis = GetSunShadow(IN.shadowWorldPos.xyz, faceNormal);
        #else
        sunVis = SHADOW_VS_PRESENT(IN.shadowWorldPos.w)
                  ? GetSunShadow(IN.shadowWorldPos.xyz, faceNormal)
                  : 1.0f;
        #endif
        #endif
        sunAndProj *= sunVis;
    #endif

    #if INTERIOR_SHADOWS
        // UNOFFICIAL forward point-light shadows (Includes/PointShadowForward.hlsl). The normal offset uses the sun's
        // geometric normal where the shader has one. The point-light-only passes (DIFFUSE, POINT) have none and do not
        // get one: its ddx/ddy would cost more than these short passes' own work, so they rely on the depth bias alone,
        // as the screen-space pass always did.
        float3 ptWorldPos = IN.shadowWorldPos.xyz;
        #ifndef OBJ_LAMP_FIRST
            float3 ptNormal = faceNormal;
        #elif defined(OBJ_SMOOTH_NORMAL_INTERP)
            float3 ptNormal = ShadowFacingNormal(IN.smoothNormal.xyz, IN.shadowWorldPos.xyz);	// UNOFFICIAL 2026-10-24 SmoothObjectNormals: free here, no derivatives
        #else
            float3 ptNormal = 0.0f;
        #endif
        float ptValid = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
        float ptShadow0 = 1.0f, ptShadow1 = 1.0f, ptShadow2 = 1.0f;
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            // Light k of the draw is PSLightColor[k]; light 0 is a point light only in the DIFFUSE and POINT passes.
            #ifdef OBJ_LAMP_FIRST
                ptShadow0 = POINT_SHADOW_LOOKUP(0, LAMP_ATT_SQ(IN.lightDistSq.x, IN.lightDir));
            #endif
            #ifdef OBJ_LAMP2
                ptShadow1 = POINT_SHADOW_LOOKUP(1, LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir));
            #endif
            #ifdef OBJ_LAMP3
                ptShadow2 = POINT_SHADOW_LOOKUP(2, LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir));
            #endif
        }
        float pointWeight = 0.0f;   // ONLY_SPECULAR POINT: this pass's blend weight without the point shadows (see the alpha)
    #endif

    #ifdef NVR_FOLD
    float3 lighting;
    #if CAPTURED_NORMALS_ON >= 3 && (defined(DIFFUSE) || defined(ONLY_LIGHT))
    float4 capFoldAlbedo = 0.0f;	// UNOFFICIAL 2026-10-31 AlbedoBuffer: the fold's own albedo, where the fold is on (see the output)
    #endif
    [branch] if (NVR_SpecFade.y > 0.5f) {
        // UNOFFICIAL FoldHighlights (see the top): these lamps' light as the one-pass shaders give it (Object.hlsl ...Full), divided
        // by the colour the texture pass multiplies in afterwards.
        #ifdef DIFFUSE
            float3 foldStored = tex2Dgrad(FoldBaseMap, IN.uv.xy, foldUvDx, foldUvDy).rgb;
        #else
            float3 foldStored = foldTexture;
        #endif
        foldStored *= NVR_SpecFade.z > 0.5f ? IN.foldColor.rgb : 1.0f;	// the game's TEXTURE_Vc pass multiplies the vertex colour in too
        const float3 foldAlbedo = LinColor(foldStored);
        #if CAPTURED_NORMALS_ON >= 3 && (defined(DIFFUSE) || defined(ONLY_LIGHT))
            capFoldAlbedo = float4(foldAlbedo, 1.0f);	// UNOFFICIAL 2026-10-31 AlbedoBuffer
        #endif
        const float foldValid = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
        float foldMetal = 0.0f;
        [branch] if (getMetalStrength() > 0.0f)
            foldMetal = getDerivedMetallic(normal.a, foldStored) * (foldValid > 0.0f ? getMetalFade(IN.shadowWorldPos.xyz) : 1.0f);
        const float foldRough = lerp(rough, foldMetalRoughness, foldMetal);
        #if !defined(DIFFUSE)
            lighting = getSunLightingFull(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
            lighting += getAmbientLighting(AmbientColor.rgb, foldAlbedo * (1.0f - foldMetal), faceNormal, foldValid);
            [branch] if (foldMetal > 0.0f)
                lighting += foldMetal * getMetalAmbient(AmbientColor.rgb, foldAlbedo, foldRough, saturate(dot(normal.xyz, normalize(IN.viewDir.xyz))),
                                                        0.0f, 0.0f, IN.shadowWorldPos.xyz, faceNormal);
            float foldReach = 1.0f;
        #else
            const float foldAtt0 = LAMP_ATT_SQ(IN.lightDistSq.x, IN.lightDir);
            lighting = 0.0f;
            LAMP_IF(foldAtt0)
#if SHADOW_CONTRAST_ON
                { lighting = 0.0f; SC_SPLIT(getPointLightLightingAttFull(IN.lightDir.xyz, foldAtt0, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal), POINT_SHADOW(0, foldAtt0)) }
#else
                lighting = getPointLightLightingAttFull(IN.lightDir.xyz, foldAtt0, SHADOWED(PSLightColor[0].rgb, 0, foldAtt0) * sunAndProj, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
#endif
            float foldReach = foldAtt0;
        #endif
        #ifdef OBJ_LAMP2
            const float foldAtt2 = LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir);
            LAMP_IF(foldAtt2)
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAttFull(IN.light2Dir.xyz, foldAtt2, PSLightColor[1].rgb, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal), POINT_SHADOW(1, foldAtt2))
#else
                lighting += getPointLightLightingAttFull(IN.light2Dir.xyz, foldAtt2, SHADOWED(PSLightColor[1].rgb, 1, foldAtt2), IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
#endif
            foldReach = max(foldReach, foldAtt2);
        #endif
        #ifdef OBJ_LAMP3
            const float foldAtt3 = LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir);
            LAMP_IF(foldAtt3)
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAttFull(IN.light3Dir.xyz, foldAtt3, PSLightColor[2].rgb, IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal), POINT_SHADOW(2, foldAtt3))
#else
                lighting += getPointLightLightingAttFull(IN.light3Dir.xyz, foldAtt3, SHADOWED(PSLightColor[2].rgb, 2, foldAtt3), IN.viewDir.xyz, normal.xyz, foldAlbedo, foldRough, foldMetal);
#endif
            foldReach = max(foldReach, foldAtt3);
        #endif
        // fp16 target: kept finite where the colour is (almost) black; the texture pass then multiplies it by (almost) nothing.
        lighting = min(lighting / max(foldAlbedo, 1e-6f), 60000.0f);
        #ifdef DIFFUSE
            clip((TESR_LampPassTrim.x > 0.0f && foldReach <= 0.0f && BarrelHeatMuzzle.w <= 0.0f) ? -1.0f : 0.0f);	// TrimLampPasses, as below
        #endif
    }
    else {
    #endif
    #if defined(METAL)
        // UNOFFICIAL metal (Object.hlsl). getMetalStrength() (TESR_PBRMetal.x, or the material rule's) is the same for every pixel of a draw, so this is one cheap
        // branch: off ([Shaders.PBR.Metal] 0 for this pass) runs the old lighting below exactly. On: how much of the pixel
        // is metal; the metal share has a tinted highlight (F0 = its colour) and no diffuse light, and reflects its
        // surroundings instead of the diffuse ambient (the sky along the reflected view, by world up from the VS).
        #ifdef SI
            float3 glowTexel = tex2D(GlowMap, IN.uv.xy).rgb;
        #endif
        float skyValid = SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f;
        #ifdef OBJ_LAMP2
            float att2 = LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir);
#if SHADOW_CONTRAST_ON
            float3 light2Color = PSLightColor[1].rgb;
            const float3 scShadow2 = POINT_SHADOW(1, att2);
#else
            float3 light2Color = SHADOWED(PSLightColor[1].rgb, 1, att2);
#endif
        #endif
        #ifdef OBJ_LAMP3
            float att3 = LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir);
#if SHADOW_CONTRAST_ON
            float3 light3Color = PSLightColor[2].rgb;
            const float3 scShadow3 = POINT_SHADOW(2, att3);
#else
            float3 light3Color = SHADOWED(PSLightColor[2].rgb, 2, att3);
#endif
        #endif
        float3 lighting;
        [branch] if (getMetalStrength() > 0.0f) {
            // Faded out with distance (getMetalFade); at full without the position (a vanilla vertex shader ran).
            metallic = HAND_BLOOD_METAL(getDerivedMetallic(normal.a, albedoStored) * (skyValid > 0.0f ? getMetalFade(IN.shadowWorldPos.xyz) : 1.0f));
            metalRoughness = lerp(rough, metalRoughness, metallic);
            lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic);
            #ifdef SI
                lighting += surface.rgb * LinColor(glowTexel.rgb * EmittanceColor.rgb);
            #endif
            lighting += getAmbientLighting(AmbientColor.rgb, surface.rgb * (1.0f - metallic), faceNormal, skyValid);
            [branch] if (metallic > 0.0f) {
                float3 metalView = normalize(IN.viewDir.xyz);
                #ifdef OBJ_SMOOTH_NORMAL_W
                // UNOFFICIAL 2026-10-24: lightDistSq.w / lightDir.w carry the smooth normal now; the reflection's height in world space from it
                // (camera-relative position: the eye is at the origin; world up is +z), without the normal map's detail.
                bool upKnown = skyValid > 0.0f;
                float upReflect = upKnown ? reflect(normalize(IN.shadowWorldPos.xyz), faceNormal).z : 0.0f;
                #else
                float3 upTangent = float3(IN.lightDistSq.w, IN.lightDir.w, IN.vertexColor.a * 2.0f - 1.0f);
                float upLength = length(upTangent);
                bool upKnown = upLength > 0.5f && skyValid > 0.0f;
                float upReflect = upKnown ? dot(reflect(-metalView, normal.xyz), upTangent / upLength) : 0.0f;
                #endif
                lighting += metallic * getMetalAmbient(AmbientColor.rgb, surface.rgb, metalRoughness, saturate(dot(normal.xyz, metalView)),
                                                       upReflect, upKnown ? 1.0f : 0.0f,
                                                       IN.shadowWorldPos.xyz, faceNormal);
            }
            #ifdef OBJ_LAMP2
                LAMP_IF(att2)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                    SC_SPLIT(getPointLightLightingAtt(IN.light2Dir.xyz, att2, light2Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic), scShadow2)
#else
                    lighting += getPointLightLightingAtt(IN.light2Dir.xyz, att2, light2Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic);
#endif
            #endif
            #ifdef OBJ_LAMP3
                LAMP_IF(att3)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                    SC_SPLIT(getPointLightLightingAtt(IN.light3Dir.xyz, att3, light3Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic), scShadow3)
#else
                    lighting += getPointLightLightingAtt(IN.light3Dir.xyz, att3, light3Color, IN.viewDir.xyz, normal.xyz, surface.rgb, metalRoughness, metallic);
#endif
            #endif
        }
        else {
            lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
            #ifdef SI
                lighting += surface.rgb * LinColor(glowTexel.rgb * EmittanceColor.rgb);
            #endif
            lighting += getAmbientLighting(AmbientColor.rgb, surface.rgb, faceNormal, skyValid);
            #ifdef OBJ_LAMP2
                LAMP_IF(att2)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                    SC_SPLIT(getPointLightLightingAtt(IN.light2Dir.xyz, att2, light2Color, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), scShadow2)
#else
                    lighting += getPointLightLightingAtt(IN.light2Dir.xyz, att2, light2Color, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
            #endif
            #ifdef OBJ_LAMP3
                LAMP_IF(att3)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                    SC_SPLIT(getPointLightLightingAtt(IN.light3Dir.xyz, att3, light3Color, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), scShadow3)
#else
                    lighting += getPointLightLightingAtt(IN.light3Dir.xyz, att3, light3Color, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
            #endif
        }
    #else
    #ifndef OBJ_LAMP_FIRST
        FOLD_DECL lighting = getSunLighting(IN.lightDir.xyz, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
    #else
        // Pointlights only. Attenuate from the object-space lightDistSq.x carried from the VS,
        // not length(IN.lightDir.xyz) -- that vector is tangent-space (TBN-transformed) and its
        // length is only correct if the TBN basis is orthonormal.
        float att0 = LAMP_ATT_SQ(IN.lightDistSq.x, IN.lightDir);
        #if INTERIOR_SHADOWS && defined(POINT)
            FOLD_DECL lighting = 0.0f;
            LAMP_IF(att0) {   // UNOFFICIAL LampReachSkip
                lighting = getPointLightLightingAtt(IN.lightDir.xyz, att0, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
                pointWeight += weight(lighting);
                lighting *= POINT_SHADOW(0, att0);
            }
        #else
            FOLD_DECL lighting = 0.0f;
            LAMP_IF(att0)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                { lighting = 0.0f; SC_SPLIT(getPointLightLightingAtt(IN.lightDir.xyz, att0, PSLightColor[0].rgb * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), POINT_SHADOW(0, att0)) }
#else
                lighting = getPointLightLightingAtt(IN.lightDir.xyz, att0, SHADOWED(PSLightColor[0].rgb, 0, att0) * sunAndProj, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
        #endif
    #endif
    
    // Self emmitance.
    #ifdef SI
        float3 glowTexel = tex2D(GlowMap, IN.uv.xy).rgb;
        lighting += surface.rgb * LinColor(glowTexel.rgb * EmittanceColor.rgb);
    #endif
    
    #ifdef OBJ_AMBIENT
        // Reuses faceNormal computed above -- see the comment there. Always in scope
        // here: POINT implies ONLY_SPECULAR, so !ONLY_SPECULAR implies !POINT.
        lighting += getAmbientLighting(AmbientColor.rgb, surface.rgb, faceNormal,
                                       SHADOW_VS_PRESENT(IN.shadowWorldPos.w) ? 1.0f : 0.0f);
    #endif

    // Other light sources. Same object-space attenuation fix as light0 above.
    #ifdef OBJ_LAMP2
        float att2 = LAMP_ATT_SQ(IN.lightDistSq.y, IN.light2Dir);
        #if INTERIOR_SHADOWS && defined(POINT)
            LAMP_IF(att2) {   // UNOFFICIAL LampReachSkip
                float3 light2Term = getPointLightLightingAtt(IN.light2Dir.xyz, att2, PSLightColor[1].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
                pointWeight += weight(light2Term);
                lighting += light2Term * POINT_SHADOW(1, att2);
            }
        #else
            LAMP_IF(att2)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAtt(IN.light2Dir.xyz, att2, PSLightColor[1].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), POINT_SHADOW(1, att2))
#else
                lighting += getPointLightLightingAtt(IN.light2Dir.xyz, att2, SHADOWED(PSLightColor[1].rgb, 1, att2), IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
        #endif
    #endif

    #ifdef OBJ_LAMP3
        float att3 = LAMP_ATT_SQ(IN.lightDistSq.z, IN.light3Dir);
        #if INTERIOR_SHADOWS && defined(POINT)
            LAMP_IF(att3) {   // UNOFFICIAL LampReachSkip
                float3 light3Term = getPointLightLightingAtt(IN.light3Dir.xyz, att3, PSLightColor[2].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
                pointWeight += weight(light3Term);
                lighting += light3Term * POINT_SHADOW(2, att3);
            }
        #else
            LAMP_IF(att3)   // UNOFFICIAL LampReachSkip
#if SHADOW_CONTRAST_ON
                SC_SPLIT(getPointLightLightingAtt(IN.light3Dir.xyz, att3, PSLightColor[2].rgb, IN.viewDir.xyz, normal.xyz, surface.rgb, rough), POINT_SHADOW(2, att3))
#else
                lighting += getPointLightLightingAtt(IN.light3Dir.xyz, att3, SHADOWED(PSLightColor[2].rgb, 2, att3), IN.viewDir.xyz, normal.xyz, surface.rgb, rough);
#endif
        #endif
    #endif

    #ifdef OBJ_LAMP_FIRST
        // UNOFFICIAL TrimLampPasses ([Main.Main.Performance], TESR_LampPassTrim.x, 2026-10-11): these passes add lamp light
        // on top of an object already drawn (additive, no fog, no depth write). Where the pixel is outside every lamp's
        // reach each lamp adds exactly 0 (vanillaAttSq), so the pixel is dropped instead of being blended in as 0
        // (POINT: alpha, the blend weight, is 0 there too). Not while the GunFX barrel glowTexel is on (it adds its own colour).
        float lampReach = att0;
        #ifdef OBJ_LAMP2
            lampReach = max(lampReach, att2);
        #endif
        #ifdef OBJ_LAMP3
            lampReach = max(lampReach, att3);
        #endif
        clip((TESR_LampPassTrim.x > 0.0f && lampReach <= 0.0f && BarrelHeatMuzzle.w <= 0.0f) ? -1.0f : 0.0f);
    #endif

    #endif // METAL
    #ifdef NVR_FOLD
    }
    #endif

    #ifndef OBJ_LAMP_FIRST
        // UNOFFICIAL: the sun focused through each rain drop into a bright spot on its far side (includes/HudRain.hlsl).
        lighting += PSLightColor[0].rgb * sunAndProj * surface.rgb * HudRainLens(IN.lightDir.xyz).y;
    #endif
    #if SHADOW_CONTRAST_ON && !defined(DIFFUSE) && !defined(POINT)
        lighting *= ShadowContrastKeep(lighting, scLost);	// UNOFFICIAL ShadowContrast: a base pass has the ambient: darkened here
    #endif
    float3 shaded = LinEncode(lighting.rgb);	// UNOFFICIAL linear lighting: back to the stored (gamma) space
    #ifdef ONLY_LIGHT
        // UNOFFICIAL 2026-10-16 ExactPassSums (core/PassSums.h): while NVR redirects this run of added passes into its own buffer,
        // (the extra lamp passes use the DIFFUSE and the plain light-only shaders; the flag is set only during a redirected run)
        // the pass gives its LINEAR light; the buffer is folded into the frame once, in linear light, when the run ends.
        [flatten] if (TESR_PassRedirect.x > 0.5f) shaded = max(lighting.rgb, 0.0f);
    #endif
    #ifdef NVR_LAMP_SUMS
        // UNOFFICIAL 2026-10-21 LampSums (see the top): c210 y 1 during the world's run of extra lamp passes, zw 1 / the frame's size.
        [branch] if (TESR_PassRedirect.y > 0.5f) {
            const float3 runStart = max(tex2Dlod(NVR_LampRunStart, float4((NVR_IN_VPOS + 0.5f) * TESR_PassRedirect.zw, 0.0f, 0.0f)).rgb, 0.0f);
            shaded = LinEncode(LinColor(runStart) + max(lighting.rgb, 0.0f)) - runStart;
        }
    #endif
    shaded = MaterialRulesDebug(shaded, albedoStored);   // UNOFFICIAL [Shaders.PBR.MaterialRules] DebugView
    shaded = RmaosDebug(shaded, albedoStored);           // UNOFFICIAL [Shaders.PBR.Rmaos] DebugView

    #ifdef METAL
        // [Shaders.PBR.Metal] DebugView: metal in orange over a dim grey image.
        [branch] if (TESR_PBRMetalLook.w > 0.5f) shaded = lerp(luma(albedoStored).xxx * 0.35f, float3(1.0f, 0.45f, 0.05f), metallic);
    #endif
    
    // Fog.
    #ifdef OBJ_FOG
        shaded.rgb = OBJ_WITH_FOG(shaded.rgb, IN.fogColor);
    #endif
    

#if SHADOW_FORCE_MARKER
    shaded.rgb = OBJ_MARKER_COLOR;
#endif

#ifdef OBJ_AD_PASS
    pout.color.rgb = ApplyBarrelHeat(shaded.rgb, BarrelHeatMask(float3(IN.uv.zw, IN.viewDir.w)), heatMaterialFactor);
#else
    pout.color.rgb = ApplyBarrelHeat(shaded.rgb, BarrelHeatMask(float3(IN.uv.zw, IN.viewDir.w)));
#endif

#if INTERIOR_SHADOWS
    // UNOFFICIAL DebugView 3 (PointShadowForward.hlsl PointShadowTrace): the base pass shows it, the passes that add more
    // light on top (DIFFUSE, ONLY_SPECULAR) add nothing.
    [branch] if (NVR_PointShadowDebug.x > 2.5f) {
        #ifndef OBJ_AMBIENT
            pout.color.rgb = 0.0f;
        #else
            pout.color.rgb = PointShadowTrace(ptWorldPos, ptNormal, ptValid);
        #endif
    }
    #if POINT_SHADOW_RT
        // UNOFFICIAL 2026-10-29 RT coverage view (PointShadowForward.hlsl PointShadowRtCoverage); the added passes add nothing
        [branch] if (NVR_PointShadowRT.x > 2.5f) {
            #ifndef OBJ_AMBIENT
                pout.color.rgb = 0.0f;
            #else
                pout.color.rgb = PointShadowRtCoverage(ptWorldPos);
            #endif
        }
    #endif
    #if LAMP_CAPS_VIEW
        // UNOFFICIAL 2026-10-31 LampCapsCensus view 4 (PointShadowForward.hlsl NVR_CapsUnshadowed): red where a lamp with no projShade source lights this pass
        [branch] if (NVR_PointShadowDebug.x > 3.5f) pout.color.rgb = float3(NVR_CapsUnshadowed > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f);
    #endif
#endif

    #ifdef DIFFUSE
        pout.color.a = 1;
    #elif defined(ONLY_SPECULAR)
        #ifndef OBJ_LAMP_FIRST
            // Alpha here is a blend WEIGHT (this pass's own brightness, used to fade
            // its additive specular/sheen layer smoothly), not a darkness value --
            // unlike the colour output, it must not shrink with sun projShade, or the
            // whole layer fades toward invisible in projShade instead of just going
            // dark. That read as "alpha blending broken" under Forward Shadows
            // specifically, because sunVis is the one new factor it added to
            // sunAndProj here (STBB/PROJ_SHADOW's own contribution predates
            // this and is left alone). Dividing it back out recovers the weight
            // this pass would have produced without Forward Shadows; harmless and
            // exact when Forward Shadows is compiled out, since sunVis is then
            // fixed at 1.0.
            pout.color.a = weight(shaded.rgb) / max(sunVis, 0.05f);
        #elif INTERIOR_SHADOWS
            // The same for the point lights' shadows: the weight is what the lights would give unshadowed.
            pout.color.a = pointWeight;
        #else
            pout.color.a = weight(shaded.rgb);
        #endif
    #elif defined(ONLY_LIGHT)
        pout.color.a = ReflMaterialAlpha(surface.a);	// UNOFFICIAL MaterialAware reflections (Object.hlsl)
    #else
        pout.color.a = ReflMaterialAlpha(surface.a * AmbientColor.a);
    #endif
    #ifdef ONLY_SPECULAR
        pout.highlightCopy = pout.color;   // UNOFFICIAL NewSkin: alpha too, so any blend the pass uses adds alike to both targets
    #endif
#if CAPTURED_NORMALS_ON
    #if !defined(ONLY_SPECULAR)
        // UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl): COLOR1 = the linear colour the light is multiplied by. The DIFFUSE
        // lamp passes have no texture of their own: unwritten, unless FoldHighlights bound the object's base map on s7 (then the fold's albedo);
        // the ONLY_LIGHT base passes take the texture the game's TEXTURE pass multiplies in afterwards (s0, as FoldHighlights' light pass
        // reads it: foldTexture) times the vertex colour, as the one-pass shader's surface is made.
        #ifdef DIFFUSE
            #if CAPTURED_NORMALS_ON >= 3 && defined(NVR_FOLD)
                pout.capturedPad1 = capFoldAlbedo;
            #else
                pout.capturedPad1 = CAPTURED_ALBEDO_NONE;
            #endif
        #elif defined(ONLY_LIGHT) && CAPTURED_NORMALS_ON >= 3
            #ifdef NO_VERTEX_COLOR
                float4 capAlbedo = CapturedAlbedoOut(LinColor(capBaseTex));
            #else
                float4 capAlbedo = CapturedAlbedoOut(LinColor(capBaseTex * IN.vertexColor.rgb));
            #endif
            #ifdef NVR_FOLD
                pout.capturedPad1 = NVR_SpecFade.y > 0.5f ? capFoldAlbedo : capAlbedo;
            #else
                pout.capturedPad1 = capAlbedo;
            #endif
        #else
            pout.capturedPad1 = CapturedAlbedoOut(surface.rgb);
        #endif
    #endif
    #ifdef METAL	// UNOFFICIAL 2026-10-31: the metal share and the finish it was lit with (metalRoughness is lerped where metallic > 0)
        pout.capturedPad2 = CapturedMaterialOutMetal(metallic > 0.0f ? metalRoughness : rough, metallic);
    #else
        pout.capturedPad2 = CapturedMaterialOut(rough);	// UNOFFICIAL 2026-10-29 MaterialBuffer (0 without it)
    #endif
    #if SHADOW_CONTRAST_ON && defined(DIFFUSE)
        // UNOFFICIAL ShadowContrast: an added lamp pass gives the light its shadows took (alpha only: core/CapturedNormals.h masks rgb off)
        pout.capturedPad2 = float4(0.0f, 0.0f, 0.0f, dot(max(scLost, 0.0f), float3(0.2126f, 0.7152f, 0.0722f)));
    #endif
    // UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex normal, before the normal map. The added lamp and highlight passes write
    // none (they are masked off anyway); nor does the slot-tight six-lamp shader (the depth-rebuilt normal stays in use there).
    #if defined(DIFFUSE) || defined(ONLY_SPECULAR) || defined(NVR_SLOT_TIGHT)
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;
    #elif defined(OBJ_SMOOTH_NORMAL_INTERP)
        pout.capturedNormal = CapturedNormalOut(IN.smoothNormal.xyz, IN.shadowWorldPos.xyz);
    #elif defined(OBJ_SMOOTH_NORMAL_W)
        pout.capturedNormal = CapturedNormalOut(ShadowPlainNormal(float2(IN.lightDistSq.w, IN.lightDir.w), IN.vertexColor.a), IN.shadowWorldPos.xyz);
    #else
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;	// SmoothObjectNormals off: no vertex normal here
    #endif
#endif
    pout.color.rgb = LinearSceneOut(pout.color.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)

    return pout;
}

#elif defined(PS) && LIGHTS >= 4

#ifdef MANY_LAMPS
    // UNOFFICIAL ManyLampsOnePass (core/ManyLamps.h): c37.x the lamp count, then per lamp k: c38 + 2k position in the object's space,
    // .w its reach; c39 + 2k its colour (already linear with linear lighting on). Free in every ObjectTemplate pixel shader.
    #define MANY_LAMPS_MAX 13
    float4 NVR_ManyLamps[1 + 2 * MANY_LAMPS_MAX] : register(c37);
    // 2026-10-27: lamps 14-19 in c76-c87 and 20-24 in c98-c107 (per lamp: position + reach, colour), free in every NVR pixel shader too.
    #define MANY_LAMPS_B 6
    #define MANY_LAMPS_C 5
    float4 NVR_ManyLampsB[2 * MANY_LAMPS_B] : register(c76);
    float4 NVR_ManyLampsC[2 * MANY_LAMPS_C] : register(c98);
    #if MANY_LAMPS_ON >= 2
        #ifdef LIGHT_COMMON_ROWS_N
            #define LIGHT_COMMON_ROWS LIGHT_COMMON_ROWS_N	// UNOFFICIAL 2026-10-31 LampTableStandalone / TableLamps: ManyLamps::TableRowsNow
        #elif MANY_LAMPS_ON == 3
            #define LIGHT_COMMON_ROWS 31.0f	// UNOFFICIAL 2026-10-30 ClusteredLamps: ManyLamps::TableRows
        #endif
        #include "includes/LightCommon.hlsl"	// UNOFFICIAL 2026-10-29 LightCommonLamps: the lamps from the frame's table (s3)
    #endif
#endif

// UNOFFICIAL 2026-10-24 SmoothObjectNormals: the world normal at a point of this draw, p its camera-relative world position: the vertex
// normal the vertex shader sent in uv.zw, facing the camera; off, the per-triangle derivative normal as before (top level only then).
#if OBJECT_SMOOTH_NORMALS
    #define OBJ_WORLD_NORMAL(p) ShadowFacingNormal(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), (p))
#else
    #define OBJ_WORLD_NORMAL(p) GetShadowGeometricNormal(p)
#endif

struct PS_OUTPUT {
    float4 color : COLOR0;
#if CAPTURED_NORMALS_ON
    // (ps_3_0 needs COLOR0..COLOR3 contiguous: COLOR1 / COLOR2 written as 0, discarded where nothing is bound there)
    float4 capturedPad1 : COLOR1;
    float4 capturedPad2 : COLOR2;
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

sampler2D BaseMap : register(s0);	// colour texture; alpha for the alpha test
sampler2D NormalMap : register(s1);	// rgb normal, alpha the highlight mask (gloss)
// UNOFFICIAL 2026-10-27 ManyLampsGlow (core/LampsInOnePass.h): glow-mapped objects in the one pass (BSSM_ADT10_G, ADT4_G, ADTS10_G and
// their _SG; the game binds the glow map on stage 4 and the emittance colour in c2 for these passes but never picked them before).
// (2026-10-28 port: only with MANY_LAMPS_ON, the only time ManyLampsGlow can patch them in; off = the shaders exactly as before.)
#if defined(SI) && !defined(OPT) && defined(MANY_LAMPS_ON) && MANY_LAMPS_ON
    sampler2D GlowMap : register(s4);	// glow (SI) or hair tint (HAIR) map
#endif

#ifdef LINEAR_LIGHTING
    float4 AmbientColorIn : register(c1);
    static float4 AmbientColor;	// UNOFFICIAL linear lighting: as in the LIGHTS < 4 variant
    float4 PSLightColorIn[10] : register(c3);
    static float4 PSLightColor[10];
    #ifdef LIN_PREDECODED
        float4 AmbientColorDec : register(c64);		// UNOFFICIAL LinearPreDecode, as in the LIGHTS < 4 variant
        float4 PSLightColorDec[10] : register(c66);
    #endif
#else
    float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
    float4 PSLightColor[10] : register(c3);	// rgb per light (sun first where there is one)
#endif
float4 PSLightPosition[8] : register(c19);	// per lamp: xyz object space, w radius


#ifdef OBJ_TOGGLES
    float4 EmittanceColor : register(c2);	// glow or hair colour

    float4 Toggles : register(c27);	// x vertex colour on, y fog on, z gloss power, w alpha test reference

    #define lightsUsed EmittanceColor.a	// how many of the lamp slots are live
    #define lightOffset 0	// first light slot that is a lamp
    #define glossPow Toggles.z	// the game's highlight exponent
#else
    #define lightsUsed PSLightColor[0].a	// how many of the lamp slots are live
    #define lightOffset 1	// first light slot that is a lamp (slot 0 is the sun)
    #define glossPow PSLightColor[1].w	// the game's highlight exponent
#endif

#if defined(MANY_LAMPS) && MANY_LAMPS_ON == 3
PS_OUTPUT main(PS_INPUT IN, float2 mlVPos : VPOS) {	// UNOFFICIAL 2026-10-30 ClusteredLamps: the pixel's froxel
#else
PS_OUTPUT main(PS_INPUT IN) {
#endif
    PS_OUTPUT pout;

#ifdef LINEAR_LIGHTING
    // UNOFFICIAL linear lighting: as in the LIGHTS < 4 variant.
    #ifdef LIN_PREDECODED	// UNOFFICIAL LinearPreDecode: decoded on the CPU
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++) PSLightColor[lightIndex] = float4(PSLightColorDec[lightIndex].rgb, PSLightColorIn[lightIndex].a);	// (.a from the game's register: keeps it reserved)
    #else
    [unroll] for (int lightIndex = 0; lightIndex < 10; lightIndex++)
        PSLightColor[lightIndex] = float4(LinColor(PSLightColorIn[lightIndex].rgb), PSLightColorIn[lightIndex].a);
    #endif
    #ifdef LIN_PREDECODED
        AmbientColor = float4(AmbientColorDec.rgb, AmbientColorIn.a);
    #else
    AmbientColor = float4(LinColor(AmbientColorIn.rgb), AmbientColorIn.a);
    #endif
#endif

    HudRain(IN.uv.xy, SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));	// UNOFFICIAL: rain drops, before the textures are read
    float4 baseColor = HUD_RAIN_TEX(BaseMap, IN.uv.xy);
    
    #ifdef OBJ_TOGGLES
        OBJ_ALPHA_TEST(baseColor.a);
    #endif
    
    baseColor.rgb = OBJ_WITH_VCOLOR(baseColor.rgb, IN.vertexColor.rgb);
    
    float4 normal = HUD_RAIN_TEX(NormalMap, IN.uv.xy);
    normal.xyz = OBJ_UNIT_NORMAL(normal.xyz);
    
    #ifdef RMAOS
        RmaosRead(IN.uv.xy);	// UNOFFICIAL _rmaos map (Object.hlsl): read once, here at top level
    #endif
    float roughness = getRoughness(normal.a);
    // UNOFFICIAL 2026-10-13 (user: polished marble walls in the casinos flickered round their lamps): the same geometric
    // specular AA as the other path above. This 4+ lamp path had none, so a glossy wall (material rules' stonepolished)
    // with a fine normal map sparkled from pixel to pixel as the view moved.
    roughness = SpecularAA(normal.xyz, roughness);
    // UNOFFICIAL 2026-10-13: blood on first-person geometry (Includes/HandBlood.hlsl); derivatives, so at top level.
    HandBlood(IN.uv.xy, SHADOW_WP_LOAD(IN), SHADOW_WP_VALID(IN));
    roughness = HandBloodRoughness(roughness);
    normal.xyz = HandBloodNormal(normal.xyz);
    roughness = HudRainRoughness(roughness);	// UNOFFICIAL: the rain drops, wet and rounded
    normal.xyz = HudRainNormal(normal.xyz);
    SetDiffuseEnergyKeep(normal.xyz, float3(IN.lightDir.w, IN.light2.w, IN.light3.w), roughness);	// UNOFFICIAL 2026-10-16 EnergySplit (Includes/PBR.hlsl): once per pixel, every lamp reuses it (view packed in .w)
    baseColor.rgb = HandBloodAlbedo(baseColor.rgb);
    const float3 albedoStored = baseColor.rgb;	// UNOFFICIAL linear lighting: as in the LIGHTS < 4 variant
    baseColor.rgb = LinColor(baseColor.rgb);
    baseColor.rgb *= HudRainLens(float3(0.0f, 0.0f, 1.0f)).x;	// UNOFFICIAL: a drop's rim bends the light away (darker)

    // Lighting.
    float3 viewDir = { IN.lightDir.w, IN.light2.w, IN.light3.w };

    #ifdef METAL
        // UNOFFICIAL metal (Object.hlsl), as in the LIGHTS < 4 variant. An object lit by four or more lamps at once is drawn
        // with this shader, which had none: metal (and its DebugView) came and went as lamps came into or out of an
        // object's reach, the player's armour walking through the TOPS casino above all (2026-10-08). Pixels with metal get
        // the lighting with the metal finish, the others the old lighting exactly (see the lighting below). The vertex
        // shaders here send no world up (no interpolator is spare at MAX_LIGHTS 6), so the reflection has no sky term;
        // indoors that term is off anyway. Finish specular-anti-aliased here at top level (ddx/ddy).
        float metalRoughness = SpecularAA(normal.xyz, getMetalRoughness(normal.a));
        float metallic = 0.0f;
        [branch] if (getMetalStrength() > 0.0f)
            metallic = HAND_BLOOD_METAL(getDerivedMetallic(normal.a, albedoStored) * (SHADOW_WP_VALID(IN) ? getMetalFade(SHADOW_WP_LOAD(IN)) : 1.0f));
        float lightRoughness = lerp(roughness, metalRoughness, metallic);
    #endif

    float att;

    #if INTERIOR_SHADOWS
        // UNOFFICIAL forward point-light shadows -- see the LIGHTS < 4 variant. One geometric normal (ddx/ddy) serves the
        // sun, the point lights and the ambient.
        float3 ptWorldPos = SHADOW_WP_LOAD(IN);
        float3 ptNormal = OBJ_WORLD_NORMAL(ptWorldPos);
        float ptValid = SHADOW_WP_VALID(IN) ? 1.0f : 0.0f;
        // Light k of the draw is PSLightColor[k] (k = 0 is the sun's slot without OPT); the attenuations below are the
        // ones the lighting further down computes, so the compiler shares them.
        #define PT_ATT(i) vanillaAtt(PSLightPosition[i].xyz - IN.lPosition.xyz, PSLightPosition[i].w)
        float ptShadow0 = 1.0f, ptShadow1 = 1.0f, ptShadow2 = 1.0f, ptShadow3 = 1.0f, ptShadow4 = 1.0f, ptShadow5 = 1.0f;
        #if LAMPS_DEFERRED
        // UNOFFICIAL 2026-10-31 DeferredLamps: no lamp here, no lookups (ptShadow0..5 stay 1; the sun keeps its own)
        #elif defined(LAMP_SLOT_SKIP) || defined(MANY_LAMPS)	// (ManyLampsOnePass: only the game's own lamps of the draw have cube slots)
        // UNOFFICIAL 2026-10-25 NO_SLOT_LIMIT (DXVK only, see LAMP_IFK): a slot's cube lookup only where its lamp is used and reaches.
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            float a;
            #ifdef MANY_LAMPS
            // UNOFFICIAL 2026-10-31 SteadyLampShadows (core/ManyLamps.h Steady): c37.w 1 = this draw's slots hold re-sorted lamps, so the
            // game's lamp k (PSLightPosition) is not slot k's lamp: its reach must not gate the lookup (the cube's own radius still does).
            const float slotsResorted = NVR_ManyLamps[0].w;
            #else
            const float slotsResorted = 0.0f;
            #endif
            #ifndef OBJ_TOGGLES
                a = max(PT_ATT(0), slotsResorted);				[branch] if (a > 0.0f && 0 < lightsUsed) ptShadow0 = POINT_SHADOW_LOOKUP(0, a);
            #endif
            a = max(PT_ATT(lightOffset + 0), slotsResorted);	[branch] if (a > 0.0f && 1 < lightsUsed) ptShadow1 = POINT_SHADOW_LOOKUP(1, a);
            a = max(PT_ATT(lightOffset + 1), slotsResorted);	[branch] if (a > 0.0f && 2 < lightsUsed) ptShadow2 = POINT_SHADOW_LOOKUP(2, a);
            a = max(PT_ATT(lightOffset + 2), slotsResorted);	[branch] if (a > 0.0f && 3 < lightsUsed) ptShadow3 = POINT_SHADOW_LOOKUP(3, a);
            a = max(PT_ATT(lightOffset + 3), slotsResorted);	[branch] if (a > 0.0f && 4 < lightsUsed) ptShadow4 = POINT_SHADOW_LOOKUP(4, a);
            a = max(PT_ATT(lightOffset + 4), slotsResorted);	[branch] if (a > 0.0f && 5 < lightsUsed) ptShadow5 = POINT_SHADOW_LOOKUP(5, a);
        }
        #else
        [branch] if (NVR_PointShadowParams.x > 0.0f) {
            #ifndef OBJ_TOGGLES
                ptShadow0 = POINT_SHADOW_LOOKUP(0, PT_ATT(0));
            #endif
            ptShadow1 = POINT_SHADOW_LOOKUP(1, PT_ATT(lightOffset + 0));
            ptShadow2 = POINT_SHADOW_LOOKUP(2, PT_ATT(lightOffset + 1));
            #if MAX_LIGHTS >= 4
                ptShadow3 = POINT_SHADOW_LOOKUP(3, PT_ATT(lightOffset + 2));
            #endif
            #if MAX_LIGHTS >= 5
                // UNOFFICIAL 2026-10-13: lightOffset, as for the lamps above (it was missing: in the OPT passes lamps 5 and 6
                // took the previous lamp's position).
                ptShadow4 = POINT_SHADOW_LOOKUP(4, PT_ATT(lightOffset + 3));
                ptShadow5 = POINT_SHADOW_LOOKUP(5, PT_ATT(lightOffset + 4));
            #endif
        }
        #endif
        #undef PT_ATT
    #endif

    // Forward sun shadows -- see the LIGHTS < 4 variant. Only the OPT-off path has a sun
    // term; with OPT the first slot is a point light and must not be shadowed by the sun.
    #ifdef OBJ_TOGGLES
        float3 sunWP = SHADOW_WP_LOAD(IN);
        #if INTERIOR_SHADOWS
            float3 sunFaceN = ptNormal;
        #else
            float3 sunFaceN = OBJ_WORLD_NORMAL(sunWP);
        #endif
        // Decline to shadow if a vanilla vertex shader ran: the interpolator is undefined.
        float sunShadow = 1.0f;
        #if FORWARD_SHADOWS
        #if UNLIT_SHADOW_SKIP
        // UNOFFICIAL 2026-10-17 UnlitShadowSkip (SunShadowWanted), as in the LIGHTS < 4 variant.
        [branch] if (SHADOW_WP_VALID(IN) && SunShadowWanted(normal.xyz, IN.lightDir.xyz))
            sunShadow = GetSunShadow(sunWP, sunFaceN);
        #else
        sunShadow = SHADOW_WP_VALID(IN)
                  ? GetSunShadow(sunWP, sunFaceN)
                  : 1.0f;
        #endif
        #endif
    #endif

    // ddx/ddy must stay at pixel-shader top level.
    #if INTERIOR_SHADOWS
        float3 skyNormal = ptNormal;
    #else
        float3 skyNormal = OBJ_WORLD_NORMAL(SHADOW_WP_LOAD(IN));
    #endif

    // The sun's and the lamps' light (includes/ManyLightsDirect.hlsl), then the ambient.
    float3 lighting;
    #if SHADOW_CONTRAST_ON
        float3 scLost = 0.0f;	// UNOFFICIAL ShadowContrast: the light the lamps' shadows take here (SC_SPLIT)
    #endif
    #ifdef METAL
    [branch] if (metallic > 0.0f) {
        // UNOFFICIAL metal: the metal finish, no diffuse ambient on the metal share, which reflects its surroundings instead
        // (see the LIGHTS < 4 variant; no world up here, so no sky term).
        #define LIGHT_FINISH lightRoughness, metallic
        #include "includes/ManyLightsDirect.hlsl"
        #undef LIGHT_FINISH
        lighting += getAmbientLighting(AmbientColor.rgb, baseColor.rgb * (1.0f - metallic), skyNormal,
                                       SHADOW_WP_VALID(IN) ? 1.0f : 0.0f);
        lighting += metallic * getMetalAmbient(AmbientColor.rgb, baseColor.rgb, lightRoughness,
                                               saturate(dot(normal.xyz, normalize(viewDir))), 0.0f, 0.0f,
                                               (0.0f).xxx, (0.0f).xxx);
    }
    else
    #endif
    {
        #define LIGHT_FINISH roughness
        #include "includes/ManyLightsDirect.hlsl"
        #undef LIGHT_FINISH
        lighting += getAmbientLighting(AmbientColor.rgb, baseColor.rgb, skyNormal,
                                       SHADOW_WP_VALID(IN) ? 1.0f : 0.0f);
    }

    #if SHADOW_CONTRAST_ON
        lighting *= ShadowContrastKeep(lighting, scLost);	// UNOFFICIAL ShadowContrast (see its constant); before the glow, which stays as it is
    #endif

    // UNOFFICIAL ManyLampsGlow: self emittance, as the LIGHTS < 4 variant adds it.
    #if defined(SI) && !defined(OPT) && defined(MANY_LAMPS_ON) && MANY_LAMPS_ON
        lighting += baseColor.rgb * LinColor(tex2D(GlowMap, IN.uv.xy).rgb * EmittanceColor.rgb);
    #endif

    // TODO: Vanilla attenuates the full specular term by IN.lPosition.w for some reason. Is this a problem?
    float3 shaded = LinEncode(lighting);	// UNOFFICIAL linear lighting
    shaded = MaterialRulesDebug(shaded, albedoStored);   // UNOFFICIAL [Shaders.PBR.MaterialRules] DebugView
    shaded = RmaosDebug(shaded, albedoStored);           // UNOFFICIAL [Shaders.PBR.Rmaos] DebugView

    #ifdef METAL
        // [Shaders.PBR.Metal] DebugView: metal in orange over a dim grey image (as in the LIGHTS < 4 variant).
        [branch] if (TESR_PBRMetalLook.w > 0.5f) shaded = lerp(luma(albedoStored).xxx * 0.35f, float3(1.0f, 0.45f, 0.05f), metallic);
    #endif
    
    shaded.rgb = OBJ_WITH_FOG(shaded.rgb, IN.fogColor);
    

#if SHADOW_FORCE_MARKER
    shaded.rgb = OBJ_MARKER_COLOR;
#endif

    pout.color.rgb = ApplyBarrelHeat(shaded.rgb, BarrelHeatMask(IN.lPosition.xyz));
    #if INTERIOR_SHADOWS
        // UNOFFICIAL DebugView 3 (PointShadowForward.hlsl PointShadowTrace).
        [branch] if (NVR_PointShadowDebug.x > 2.5f) pout.color.rgb = PointShadowTrace(ptWorldPos, ptNormal, ptValid);
        #if POINT_SHADOW_RT
            [branch] if (NVR_PointShadowRT.x > 2.5f) pout.color.rgb = PointShadowRtCoverage(ptWorldPos);	// UNOFFICIAL 2026-10-29 RT coverage view
        #endif
        #if LAMP_CAPS_VIEW
            // UNOFFICIAL 2026-10-31 LampCapsCensus view 4 (PointShadowForward.hlsl NVR_CapsUnshadowed): red where a lamp with no shadow source lights this pass
            [branch] if (NVR_PointShadowDebug.x > 3.5f) pout.color.rgb = float3(NVR_CapsUnshadowed > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f);
        #endif
    #endif
    pout.color.a = ReflMaterialAlpha(baseColor.a * AmbientColor.a);	// UNOFFICIAL MaterialAware reflections (Object.hlsl)
#if CAPTURED_NORMALS_ON
    pout.capturedPad1 = CapturedAlbedoOut(baseColor.rgb);	// UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl; the pad's 0 below level 3)
    #ifdef METAL	// UNOFFICIAL 2026-10-31: the metal share and the finish it was lit with
        pout.capturedPad2 = CapturedMaterialOutMetal(lightRoughness, metallic);
    #else
        pout.capturedPad2 = CapturedMaterialOut(roughness);	// UNOFFICIAL 2026-10-29 MaterialBuffer (0 without it)
    #endif
    #if LAMPS_DEFERRED && defined(MANY_LAMPS) && MANY_LAMPS_ON >= 2
    {
        // UNOFFICIAL 2026-10-31 DeferredLamps (docs/next/DEFERRED-C1-SPEC.md): what the DeferredLamps pass needs to light this pixel.
        // COLOR1.a = the share of added light that survives this draw's fog (1 - f: the factor the colour above was lerped with).
        #ifndef OBJ_TOGGLES
            pout.capturedPad1.a = 1.0f - IN.fogColor.a;
        #else
            pout.capturedPad1.a = useFog <= 0.0f ? 1.0f : 1.0f - IN.fogColor.a;
        #endif
        // COLOR2.g / .a = the bumped normal in world axes, octahedral: tangent -> object through the frame's rows (light2/3/4 = tbn rows),
        // object -> world through the transpose of the table's world -> object rows (c39-c41); a in 1.01..1.99 marks a deferred pixel.
        const float3 dnObj = normal.x * IN.light2.xyz + normal.y * IN.light3.xyz + normal.z * IN.light4.xyz;
        const float3 dnWorld = normalize(dnObj.x * NVR_ManyLamps[2].xyz + dnObj.y * NVR_ManyLamps[3].xyz + dnObj.z * NVR_ManyLamps[4].xyz);
        const float2 dnOct = ShadowEncodeNormal(dnWorld);
        pout.capturedPad2.g = dnOct.x;
        pout.capturedPad2.a = 1.01f + 0.49f * (dnOct.y + 1.0f);
    }
    #endif
    // UNOFFICIAL 2026-10-28 CapturedNormals: the smooth vertex normal, before the normal map (as in the LIGHTS < 4 variant)
    #if LAMPS_DEFERRED && INTERIOR_SHADOWS
        // UNOFFICIAL 2026-10-31 DeferredLamps: the deferred record always writes it, the pass takes the pixel's position from its distance.
        // SLS2035 (slot-tight) gets no smooth normal from its vertex shader: there the geometric normal the lamp shadows use (ptNormal).
        #if OBJECT_SMOOTH_NORMALS && !defined(NVR_SLOT_TIGHT)
            pout.capturedNormal = CapturedNormalOut(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), SHADOW_WP_LOAD(IN));
        #else
            pout.capturedNormal = CapturedNormalOut(ptNormal, SHADOW_WP_LOAD(IN));
        #endif
    #elif OBJECT_SMOOTH_NORMALS && !defined(NVR_SLOT_TIGHT)
        pout.capturedNormal = CapturedNormalOut(ShadowPlainNormal(IN.uv.zw, IN.vertexColor.a), SHADOW_WP_LOAD(IN));
    #else
        pout.capturedNormal = CAPTURED_NORMAL_INVALID;
    #endif
#endif
    #if LAMPS_DEFERRED && defined(MANY_LAMPS) && INTERIOR_SHADOWS
        // UNOFFICIAL 2026-10-31 DeferredLamps test view ([Shaders.ShadowsInteriors.Forward] DebugView 6): how far the tangent frame the forward
        // records light in (light2/3/4) is from orthonormal. Forward dots in that frame equal the pass's world-space dots only if it is;
        // red = the rows' worst cosine to each other x 4, green / blue = the tangent's / binormal's length against the normal's, -1, x 4.
        [branch] if (NVR_PointShadowDebug.x > 5.5f) {
            const float3 tT = IN.light2.xyz, tB = IN.light3.xyz, tN = IN.light4.xyz;
            const float lT = length(tT), lB = length(tB), lN = max(length(tN), 1.0e-6f);
            const float skew = max(max(abs(dot(tT, tB)) / max(lT * lB, 1.0e-6f), abs(dot(tT, tN)) / max(lT * lN, 1.0e-6f)), abs(dot(tB, tN)) / max(lB * lN, 1.0e-6f));
            pout.color.rgb = saturate(float3(skew, abs(lT / lN - 1.0f), abs(lB / lN - 1.0f)) * 4.0f);
        }
    #endif
    pout.color.rgb = LinearSceneOut(pout.color.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)

    return pout;
}

#endif // Pixel shaders.
