// UNOFFICIAL 2026-11-14 full deferred renderer, phase 3: foliage (docs/next/FD-FOLIAGE-RESULTS.md; core/DeferredFoliage.h;
// [Main.Main.Quality] DeferredFoliage, restart, off; DeferredRenderer 2 turns it on). Included only by the foliage records' DEFERRED build
// (FOLIAGE_DEFERRED 1 with GBufferV2: Includes/Leaf.hlsl, GRASS23x000TMS.pso.hlsl); nothing else compiles this file.
//
// The foliage shading model. Leaves and grass are not lit like the object shaders: the game lights a SpeedTree leaf in its VERTEX shader
// (a normal bent toward the leaf's own direction, N.L with the backlit translucency added, the leaf's light scale, the tree's sun scale,
// STLEAF001/003.vso), and a grass tuft by its tuft axis (N.L + translucency, the tuft's light scale, the vertex colour on the sun alone,
// GRASS23x00N.vso, and per pixel by the normal map). None of these inputs is in the G-buffer, and a pass that re-derived them from COLOR3's
// card normal would light every leaf as a flat card (black from behind, where the game's bent normal and translucency keep it lit). So the
// record keeps the model: it works out, exactly as its forward twin does, the sun's light on this pixel WITHOUT the shadow, and hands it to
// DeferredSun as one number per pixel; the pass multiplies in what only it has (the screen-space sun shadow at the G-buffer's surface).
//
// The deferred record's three outputs (where it counts as written; elsewhere the forward colour, sun unshadowed, and no record):
//   COLOR0           the forward colour with the sun taken out: lerp(enc(A x rest), fog, f), rest = ambient + sky + the foliage's own lamps
//   COLOR1.rgb       the albedo A (the leaf texture decoded; grass: the texture x Brightness x the vertex colour's share of the sun x the
//                    tuft's shade), COLOR1.a 1 = a FORWARD record's code: DeferredLamps leaves it alone (the foliage's lamps are in COLOR0,
//                    as the game drew them: the leaf VS's one point light, the grass's own lamp list), and the object branch of DeferredSun
//                    (a in (1, 2]) does not take it.
//   COLOR2.b         FOLIAGE_SUN_CODE (2) + enc(w) (the colours' stored curve, so fp16's 1/512 steps there are fine steps in the dark;
//                    the pass decodes it with LinColor). w: the record's unshadowed, UNFOGGED sun light per unit of A x hue (a least-squares
//                    fit over the channels; exact where the light's hue is the sun's), hue = the record's sun colour over its luma.
//                    b >= 2 is a roughness of 1 to every reader (they clamp: DlssRRGuides, the path tracer, the Vulkan reflections;
//                    FlashlightDeferred takes 1 under FD_FOLIAGE); no other writer puts a b >= 1.5 there.
//   COLOR2.g         the keep (1 - fog) and, for the grass, its sun HIGHLIGHT (the sun's hue, not the albedo's): Includes/DeferredPixel.hlsl
//                    FDFoliageLeafCode / FDFoliageGrassCode (leaves g in [16, 32), grass [32, 1024); the layout and its precision are there).
//                    COLOR0 is fogged with the same rounded keep the code holds.
// DeferredSun adds D = A x (its sun colour / its luma) x w (+ hue x highlight on grass), times the shadow, through FDAddFogged with the keep:
// the forward stored-curve fog lerp redone exactly (tests/fd_foliage_math.py: fp16-exact without fog, and under fog, at shadow 0 and 1).
// Assumed: the record's sun hue is the pass's (both the game's directional light), the records' fog colour is the pass's DS_Fog.
#ifndef NVR_FOLIAGE_DEFERRED_HLSL
#define NVR_FOLIAGE_DEFERRED_HLSL

#ifdef __INTELLISENSE__
    #include "DeferredPixel.hlsl"
#else
    #include "includes/DeferredPixel.hlsl"	// the keep's code (FDFoliageLeafCode / FDFoliageGrassCode, FDFoliageKeep)
#endif

#define FOLIAGE_SUN_CODE 2.0f
#define FOLIAGE_HIGHLIGHT_MAX 1.0f	// (the default highlight peaks near 0.4: SpecularStrength 0.5 x the decoded sun)

float FoliageLuma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// The least-squares number s for which basis x s is nearest delta, per channel (0 where the basis is black).
float FoliageFit(float3 delta, float3 basis) {
    const float bb = dot(basis, basis);
    return bb > 1.0e-12f ? max(dot(delta, basis), 0.0f) / bb : 0.0f;
}
// The record's sun hue: its sun colour (linear) over its luma (0 for no sun).
float3 FoliageHue(float3 sunLight) {
    const float l = FoliageLuma(sunLight);
    return l > 1.0e-8f ? sunLight / l : 0.0f;
}

// COLOR2.b of a written foliage deferred pixel, and its reading in DeferredSun (the same LinEncode / LinColor pair: identity without linear lighting).
float FoliageSunCode(float w) { return FOLIAGE_SUN_CODE + LinEncode(max(w, 0.0f).xxx).x; }
float FoliageSunWeightOf(float b) { return LinColor(max(b - FOLIAGE_SUN_CODE, 0.0f).xxx).x; }
// The grass's highlight: its encoded value for FDFoliageGrassCode (40 levels to FOLIAGE_HIGHLIGHT_MAX), and back.
float FoliageHighlightEnc(float h) { return LinEncode(saturate(h / FOLIAGE_HIGHLIGHT_MAX).xxx).x; }
float FoliageHighlightOf(float enc) { return FOLIAGE_HIGHLIGHT_MAX * LinColor(saturate(enc).xxx).x; }

#endif
