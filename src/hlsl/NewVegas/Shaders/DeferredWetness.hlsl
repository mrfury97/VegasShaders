// UNOFFICIAL 2026-11-19 DeferredWetness (core/DeferredWetness.h; docs/DESIGN.md "Deferred wetness design", step 1: the land; step 2, 2026-11-20: the
// objects and parallax too, DWObject below, with their own forward inputs).
// The land's water layer (Includes/Wetness.hlsl GetWetness), worked out ONCE per pixel after the G-buffer is resolved, for the pixels the
// near land's lighting-free records drew (LAMPS_DEFERRED 2 with DEFERRED_WETNESS: TerrainTemplate.hlsl TERRAIN_WET_DEFERRED, which drops its
// own water layer). It runs inside DeferredSun::Apply, after the frame is in linear light and before the sun is added, and does what the
// forward land record does BEFORE its lights see the surface, plus the film's sky reflection (which needs no light):
//   COLOR1 (the albedo target)   the albedo darkened (WetAlbedo), darker still under standing water, the floating debris
//   COLOR2 (the material target) the shading normal flattened under standing water; r back to the land's code (see "height" below)
//   COLOR0 (the frame)           the record's ambient made wet (its colour was the dry albedo's) and the film's sky reflection, under the
//                                record's own fog (the stored-curve fog lerp, as Includes/DeferredPixel.hlsl FDAddFogged rebuilds it)
//   COLOR3 (the film target)     the film for the light passes: r round(8 murk) + 0.99 gloss, g roughness, b / a the film normal (octahedral)
// DeferredSun then lights the darker albedo and flat normal with the sun and adds the film's sun highlight (DeferredSun.hlsl DS_WET_FILM);
// the lamp pass lights them with the lamps; the Wetness Effects screen pass takes the film from COLOR3 for the lamps' streaks and the
// screen-space reflection (Effects/WetnessReflections.fx.hlsl).
// The wetness is the land record's own: the same defines (WETNESS_EFFECTS from the global list: map mode, ripples, WETNESS_HEIGHT) and inputs:
// the position (the exact linear view z), the smooth normal (COLOR3, faced as the forward land's geoN), the normal-mapped normal (COLOR2.g / a)
// and the layers' height, which the record leaves in COLOR2.r's fraction: r = 4 + 0.99 x height (the GBufferV2 specular bit, 16, left out so
// the fraction keeps 1 / 256 steps). Every reader of COLOR2.r comes after this pass, which writes r back to 4 + 16 for every such pixel, wet
// or dry (DW_RESTORE: the dry frames' pass does only that).
// Pixels the pass leaves alone: anything but the near land's lighting-free records (the far land keeps 20 + 1 / 64, forward records 20 and
// albedo alpha 1). Where something nearer that writes no G-buffer covers the land (the depth copy nearer than the G-buffer's distance, as
// DeferredSun's DS_DEPTH_AGREE) the G-buffer is still made wet and its code put back, but the frame is left as it is (no depth test: every
// such pixel's code must be put back).

#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
    #if defined(LIN_PREDECODE_ON) && LIN_PREDECODE_ON
        #define LIN_PREDECODED
    #endif
#endif

// The forward land carries the water layer only under these (TerrainTemplate.hlsl TERRAIN_WETNESS); without them the pass must not load
// (core/DeferredWetness.h then leaves the land records as they were).
#if !defined(WETNESS_EFFECTS)
    #error DeferredWetness: Wetness Effects is not compiled in
#endif
#if !FORWARD_SHADOWS || (defined(SUN_CROSSFADE) && SUN_CROSSFADE)
    #error DeferredWetness: the land has no water layer with this define set (FORWARD_SHADOWS off or SUN_CROSSFADE)
#endif

#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"	// SkyAmbient, LinearLight (the sky reflection), luma
#include "includes/Shadow.hlsl"	// the normals' octahedral code, ShadowFacingNormal
#include "includes/DeferredDepth.hlsl"
#include "includes/DeferredPixel.hlsl"

// The pass's own (c0-c11).
float4 TESR_DeferredDepthFrame[6] : register(c0);	// Includes/DeferredDepth.hlsl's frame block
float4 DW_Ctrl  : register(c6);	// x 1 = the linear view z of this frame (DeferredDepth 2), y 1 = [Shaders.Puddles.Main] ObjectPuddles (static objects take standing water),
                                // z (2026-11-20) the Reflections effect's reach over the wet floors (DWSkyKeep; 0: none)
float4 DW_Fog   : register(c7);	// rgb the fog colour the records lerped with (stored curve), w 1 = the game's fog is drawn
float4 TESR_TerrainData      : register(c10);	// (Includes/Terrain.hlsl) w ambient scale
float4 TESR_TerrainExtraData : register(c11);	// y saturation (the land's ambient takes the graded albedo)
#define DD TESR_DeferredDepthFrame

// The copies the pass reads (bound by DeferredWetness::Apply, not by name: it writes the targets they are copies of) and the G-buffer's
// normals / the linear depth, by name.
sampler2D DW_Frame    : register(s0);
sampler2D DW_Albedo   : register(s1);
sampler2D DW_Material : register(s2);
sampler2D TESR_CapturedNormalsBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DeferredDepthLinear : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#define DW_LAND_SPEC_BIT 16.0f	// Includes/CapturedNormals.hlsl GBV2_SPEC_BIT: the land's lamps lit with a highlight
#define DW_HEIGHT_SCALE 0.99f	// TerrainTemplate.hlsl TERRAIN_WET_DEFERRED: r = 4 + DW_HEIGHT_SCALE x height

// UNOFFICIAL 2026-11-20 unified reflections step 2 (docs/unified-reflections-design.md; effects/Reflections/Reflections.cpp WetReach): how much of
// the film's sky reflection this pass still adds. While the Reflections effect mirrors the wet floors this frame (reach > 0: its DrawDistance)
// it is THE wet reflection there (the screen, its cube and grid, the sky where they find nothing), so a floor's sky term goes, by the effect's
// own floor share (the normal 0.75 .. 0.95 up) and its fade toward the reach; walls, steep slopes and the far floors keep it, the effect gives
// them no wet reflection. upZ: the smooth normal's world z.
float DWSkyKeep(float upZ, float viewZ, float reach) {
    if (!(reach > 0.0f)) return 1.0f;
    return 1.0f - saturate((upZ - 0.75f) / 0.2f) * (1.0f - smoothstep(0.7f * reach, reach, viewZ));
}

// A near-land lighting-free pixel that carries the height: albedo alpha 1 + keep (GBufferV2's lighting-free code with a sun; 1.0 itself where
// the game's fog covers it fully, keep 0), r in [4, 5) (no other writer puts r there: every land record writes 20, the far land 20 + 1 / 64).
float DWNearLand(float a, float r) { return a > 0.5f && a < 2.0005f && r > 3.999f && r < 4.999f ? 1.0f : 0.0f; }

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };

#ifdef DW_RESTORE
// The dry frames: only r back to the land's code, into the material target alone (core/DeferredWetness.h binds it as target 0).
float4 main(PS_IN IN) : COLOR0 {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    float4 m = tex2Dlod(DW_Material, uv4);
    clip(DWNearLand(tex2Dlod(DW_Albedo, uv4).a, m.r) - 0.5f);
    m.r = floor(m.r + 0.001f) + DW_LAND_SPEC_BIT;
    return m;
}
#else

#define WETNESS_ORTHO_REG s7
#define WETNESS_RAINDROPS
#define WETNESS_HEIGHT
// (2026-11-20 step 2, review) the water level per pixel, as the forward records got it: the land the real level; objects 0 unless ObjectPuddles
// is on, and never worn gear (2) or hair (8): actors and first person are always sent 0 (effects/Puddles/Puddles.cpp SetDrawConstants,
// NewVegas/Hooks/Render.cpp). Their rain dampness stays.
static float DWPuddleLevelMul = 1.0f;
#define PUDDLE_LEVEL (TESR_PuddleData.x * DWPuddleLevelMul)
#include "includes/Wetness.hlsl"

// UNOFFICIAL 2026-11-20 step 2: an object's or a parallax record's lighting-free pixel (ObjectTemplate.hlsl OBJ_WET_DEFERRED, ParallaxTemplate.hlsl:
// with the switch every such record leaves its water layer here): albedo alpha 1 + keep (with a sun: the lamp-first records, 3 + keep, never had
// the layer), a model the forward object templates gave the layer to (1 object, 2 worn gear, 3 cloth, 5 plants, 8 alpha-tested hair; not the
// land's 4, skin 6, grass 7, eyes 9, unlit 10), and not the land's height code.
float DWObject(float a, float r) {
    const float model = FDModel(r);
    return a > 1.0001f && a < 2.0005f && !(r > 3.999f && r < 4.999f) &&
        (model > 0.5f && model < 3.5f || abs(model - 5.0f) < 0.5f || abs(model - 8.0f) < 0.5f) ? 1.0f : 0.0f;
}

#ifdef DW_FALLBACK
// UNOFFICIAL 2026-11-20 step 3 (docs/deferred-wetness-design.md "Step 3 as built"): the film for every pixel the pass below did not work out,
// drawn in the effect chain right before the Wetness Effects screen pass (core/DeferredWetness.h Fallback), on the lit frame, so that pass and
// the Reflections effect read ONE film for the whole screen. By what the G-buffer says drew the pixel (COLOR1.a, Includes/DeferredPixel.hlsl):
//   - a lighting-free record the pass below took (DW_FB.x 1: its film stands) or left out on purpose (skin, grass, eyes, unlit, the far land,
//     the hair over skin: a in (1, 4.5)): nothing;
//   - a forward record (a 1) or a "sun kept" record (5 + keep): their own water layer is in their colour already: the film only;
//   - nothing (a 0: the game's own shaders, LOD, trees without a record): the film, and the water layer on the lit colour, WetWorld-style:
//     the colour darkened as WetAlbedo darkens an albedo (x 0.4 under standing water) and the film's sky reflection added, as the pass below
//     does for the records' ambient. No sun highlight and no lamp streaks there (believed minor in rain; the Wetness Effects screen pass draws
//     their screen-space reflection from this film).
// The sky, the water plane, the first-person view model and anything past DW_FB.y are left alone. The wetness is GetWetness with the
// records' defines, from the frame's depth and its smooth normals (TESR_NormalsBuffer, view space), without a height.
sampler2D TESR_DepthBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_CameraData     : register(c12);	// y the far plane (TESR_DepthBuffer holds view z / far)
float4 TESR_WaterSettings  : register(c13);	// x the water's height (world z)
float4 TESR_DepthConstants : register(c14);	// z 0: the view model's depth buffer is empty at 1, else at 0 (reversed)
float4 DW_FB               : register(c15);	// x 1 = the pass below drew its film this frame, y the distance (game units) the fallback ends at,
                                            // z 1 = [Shaders.Puddles.Main] ObjectPuddles (as DW_Ctrl.y), w (2026-11-20) the Reflections effect's
                                            // reach over the wet floors (as DW_Ctrl.z)

struct FB_OUT {
    float4 frame : COLOR0;	// the chain's frame (its colour encoding: TESR_LinearLighting)
    float4 film  : COLOR1;	// the film target (as the pass below writes it)
};

FB_OUT main(PS_IN IN) {
    FB_OUT o;
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float a = tex2Dlod(DW_Albedo, uv4).a;
    const float r = tex2Dlod(DW_Material, uv4).r;
    const bool byPass = DW_FB.x > 0.5f && (DWNearLand(a, r) > 0.5f || DWObject(a, r) > 0.5f);
    const float viewZ = tex2Dlod(TESR_DepthBuffer, uv4).x * TESR_CameraData.y;
    const float vm = tex2Dlod(TESR_DepthBufferViewModel, uv4).x;
    const bool viewModel = TESR_DepthConstants.z == 0.0f ? vm < 0.9f : vm > 0.01f;
    const bool unwritten = a < 0.5f;
    // (2026-11-21, slot run: wet grass turned into a white-blue mirror) no film on the records the forward shaders never gave a water layer:
    // skin 6, grass 7, eyes 9, unlit 10 (COLOR2's class, read whenever COLOR2 was written: r above 0.5). The Reflections effect reads this
    // film as "water here", so a film on grass blades made a sharp sky mirror of them.
    const float cls = r > 0.5f ? FDModel(r) : 0.0f;
    const bool noLayer = abs(cls - 6.0f) < 0.5f || abs(cls - 7.0f) < 0.5f || cls > 8.5f;
    // (2026-11-21) a stale record: something drew over the G-buffer after the records (the game's grass, which writes no record outside path
    // tracing; water, glass), so COLOR3's camera distance is not this pixel's (core/CapturedNormals.h Data y z: 0.4 % + 2 units). The pass in
    // the sun pass gave the ground under it a film, which the readers would take for the grass blade's own: on grass that was a mirror of the
    // ground's water on normals rebuilt from depth, different per blade and per frame as the wind moves it. Such pixels, and the noLayer
    // records, get their film cleared (not clipped: clipping keeps the pass's film), the colour as it is.
    const float4 cn = tex2Dlod(TESR_CapturedNormalsBuffer, uv4);
    const float dist = length(DDRay(IN.vpos, DD[0], DD[1], DD[2], DD[3], DD[5]) * viewZ);
    const bool stale = a > 0.5f && !(cn.a > 0.99f && abs(cn.b - dist) <= dist * 0.004f + 2.0f);
    const bool clearFilm = stale || noLayer;
    const bool away = viewModel || viewZ >= TESR_CameraData.y * 0.999f || viewZ > DW_FB.y;
    const bool skip = away || (!clearFilm && (byPass || (a > 1.0001f && a < 4.5f)));
    clip(skip ? -1.0f : 1.0f);
    o.frame = float4(tex2Dlod(DW_Frame, uv4).rgb, 1.0f);
    o.film = 0.0f;
    // (clip does not end the shader: the work only where the pixel stays)
    [branch] if (!skip && !clearFilm) {
    const float3 P = DDRay(IN.vpos, DD[0], DD[1], DD[2], DD[3], DD[5]) * viewZ;
    const float3 nv = tex2Dlod(TESR_NormalsBuffer, uv4).xyz * 2.0f - 1.0f;	// view space (x right, y up, z forward)
    float3 N = normalize(DD[0].xyz * nv.x + DD[1].xyz * nv.y + DD[2].xyz * nv.z);
    if (dot(N, P) > 0.0f) N = -N;	// toward the camera
    // the water plane draws its own (Reflections.glsl's test: facing up, at the water's height)
    const bool water = N.z > 0.75f && abs(DD[4].z + P.z - TESR_WaterSettings.x) < max(2.0f, viewZ * 0.002f);
    [branch] if (!water) {
    // the water level as the forward records got it (review 2026-11-20; effects/Puddles/Puddles.cpp SetDrawConstants): the land the real level;
    // worn gear and hair 0; other objects, and the game's own shaders (LOD, statics without a record), only with ObjectPuddles
    // (2026-11-21) plants 5 also 0: damp leaves, no standing water on them
    const float model = FDModel(r);
    DWPuddleLevelMul = unwritten ? DW_FB.z : abs(model - 4.0f) < 0.5f ? 1.0f :
        abs(model - 2.0f) < 0.5f || abs(model - 8.0f) < 0.5f || abs(model - 5.0f) < 0.5f ? 0.0f : DW_FB.z;
    Wetness wet = GetWetness(P, N, N, -1.0f);
    // faded out toward DW_FB.y (no fog round trip: the fog the game's own shaders drew is not known here), the film with the colour (review:
    // the screen pass takes the film's sky back out where the film is)
    const float fade = 1.0f - smoothstep(0.7f * DW_FB.y, DW_FB.y, viewZ);
    [branch] if (unwritten) {
        const float gamma = max(TESR_LinearLighting.x, 1.0f);
        float3 lit = pow(max(o.frame.rgb, 0.0f), gamma);
        // the lit colour darkened as the albedo would be (WetAlbedo is a curve of the albedo: the colour, at most 1, stands for it)
        const float3 c = min(lit, 1.0f);
        lit *= c > 1.0e-4f ? WetAlbedo(c, wet.darkening, 1.0f) / max(c, 1.0e-4f) : 1.0f;
    #ifdef PUDDLES_COMPILED
        lit *= lerp(1.0f, 0.4f, wet.flatten);
    #endif
        [branch] if (wet.gloss > 0.001f) {
            const float3 V = -normalize(P);
            const float3 R = reflect(-V, wet.normal);
            const float horizon = saturate(1.0f + 1.2f * dot(R, N));
            lit += WetSkyReflection(R) * WetReflectance(saturate(dot(wet.normal, V)), wet.roughness, wet.gloss) * horizon * horizon * DWSkyKeep(N.z, viewZ, DW_FB.w);
        }
        o.frame.rgb = lerp(o.frame.rgb, pow(max(lit, 0.0f), 1.0f / gamma), fade);
    }
    const float2 fn = ShadowEncodeNormal(normalize(wet.normal));
    o.film = float4(0.99f * saturate(wet.gloss * fade), wet.roughness, fn.x, fn.y);	// (no murk: the land's only)
    }
    }
    return o;
}
#else

struct PS_OUT {
    float4 frame    : COLOR0;
    float4 albedo   : COLOR1;
    float4 material : COLOR2;
    float4 film     : COLOR3;
};

// The frame's linear value -> the record's lit colour before its fog (linear), and back: the inverse of the templates' stored-curve fog
// (dec(lerp(enc(A), F, f)); Includes/DeferredPixel.hlsl FDAddFogged, keep in step).
float3 DWUnfog(float3 frame, float keep, float fogOn) {
    const float gamma = max(TESR_LinearLighting.x, 1.0f);
    const float f = fogOn > 0.5f ? 1.0f - keep : 0.0f;
    const float3 v = pow(max(frame, 0.0f), 1.0f / gamma);
    return pow(max(v - f * DW_Fog.rgb, 0.0f) / max(1.0f - f, 1.0e-4f), gamma);
}
float3 DWFog(float3 lit, float keep, float fogOn) {
    const float gamma = max(TESR_LinearLighting.x, 1.0f);
    const float f = fogOn > 0.5f ? 1.0f - keep : 0.0f;
    return pow(f * DW_Fog.rgb + (1.0f - f) * pow(max(lit, 0.0f), 1.0f / gamma), gamma);
}

PS_OUT main(PS_IN IN) {
    PS_OUT o;
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 albedo = tex2Dlod(DW_Albedo, uv4);
    const float4 m = tex2Dlod(DW_Material, uv4);
    const float4 normals = tex2Dlod(TESR_CapturedNormalsBuffer, uv4);
    // (review 2026-11-20) the forward skin drawn over the deferred record (its COLOR3.a 2.05 .. 2.45; DeferredSun.hlsl's guard) is not made wet; the
    // land's code under it is still put back
    const bool skinOver = normals.a > 2.02f && normals.a < 3.5f;
    const float land = DWNearLand(albedo.a, m.r);
    const float wetPx = skinOver ? 0.0f : max(land, DWObject(albedo.a, m.r));	// (2026-11-20 step 2: the near land or an object / parallax record's pixel)
    const float near = max(land, wetPx);
    clip(near - 0.5f);
    o.albedo = albedo;
    o.material = m;
    // the land's GBufferV2 code (TerrainTemplate.hlsl: CapturedMaterialOutClass(4 + GBV2_SPEC_BIT, -1)); an object's r is kept (its metal)
    o.material.r = land > 0.5f ? 4.0f + DW_LAND_SPEC_BIT : m.r;
    o.film = 0.0f;
    o.frame = float4(tex2Dlod(DW_Frame, uv4).rgb, 1.0f);
    // (clip does not end the shader: the wetness only where the pixel stays, so the sky and everything else skip it)
    [branch] if (wetPx > 0.5f) {
    const float keep = FDLightFreeKeep(albedo.a);
    const float model = FDModel(m.r);
    DWPuddleLevelMul = land > 0.5f || (DW_Ctrl.y > 0.5f && abs(model - 2.0f) > 0.5f && abs(model - 8.0f) > 0.5f) ? 1.0f : 0.0f;
    // the land's layers' height; objects none (theirs came from the colour texture's detail, ObjectPuddles only: not carried, the noise field
    // alone shapes their standing water)
    const float height = land > 0.5f ? saturate((m.r - 4.0f) / DW_HEIGHT_SCALE) : -1.0f;

    // The position, as DeferredSun.hlsl (DS_DEPTH_AGREE): the exact linear view z where it agrees with COLOR3's half-float distance; where the
    // depth copy holds something nearer (hair, anything drawn after without a G-buffer) the G-buffer's own distance, and the frame stays as it is.
    const float3 ray = DD_CAMERA_RELATIVE_Z(DD, IN.vpos, 1.0f);
    const float storedZ = normals.b * rsqrt(dot(ray, ray));
    const float linearZ = DD_LINEAR_VIEW_Z(DD, TESR_DeferredDepthLinear, IN.vpos);
    const bool agree = abs(linearZ - storedZ) <= storedZ * (1.0f / 512.0f) + 0.5f;
    const float viewZ = DW_Ctrl.x > 0.5f && agree ? linearZ : storedZ;
    const bool covered = DW_Ctrl.x > 0.5f && !agree && linearZ < storedZ;
    const float3 P = ray * viewZ;

    const float3 frame0 = o.frame.rgb;
    float3 frame = frame0;
    // The forward land's normals: geoN the smooth vertex normal faced to the camera (TerrainTemplate.hlsl with CapturedNormals), the shading
    // normal the layers' normal-mapped one in world axes (COLOR2.g / a; none recorded: the smooth one). The objects' forward layer takes the
    // face normal for both (ObjectTemplate.hlsl GetWetness(.., faceNormal, faceNormal)): geoN for both below.
    const float3 smoothN = ShadowDecodeNormal(normals.rg);
    const float3 geoN = ShadowFacingNormal(smoothN, P);
    const bool bumped = abs(m.g) <= 1.001f;
    const float3 Nb = bumped ? ShadowDecodeNormal(m.ga) : smoothN;

    Wetness wet = GetWetness(P, geoN, land > 0.5f ? Nb : geoN, height);
    // the albedo, as the forward templates darken it before any light sees it: porosity 1 on the land, 1 - sqrt(metal) on objects (metals
    // darken less: ObjectTemplate.hlsl's METAL path; the others wrote metal 0)
    const float porosity = land > 0.5f ? 1.0f : 1.0f - sqrt(saturate((m.r - floor(m.r)) / 0.49f));
    float3 wetAlbedo = WetAlbedo(albedo.rgb, wet.darkening, porosity);
#ifdef PUDDLES_COMPILED
    // standing water hides the normal map (TerrainTemplate.hlsl: combinedNormal toward the tangent frame's z, the smooth normal)
    const float3 Nf = normalize(lerp(Nb, smoothN, wet.flatten));
    if (bumped) { const float2 e = ShadowEncodeNormal(Nf); o.material.g = e.x; o.material.a = e.y; }
    wetAlbedo *= lerp(1.0f, 0.4f, wet.flatten);	// (objects as ObjectTemplate.hlsl ObjectWetAlbedo; the forward parallax has no such step: believed minor)
    // floating debris and the standing water's murk: the land's only (the forward objects have neither)
    if (land > 0.5f) wetAlbedo = lerp(wetAlbedo, wet.debris.rgb, wet.debris.a);
    const float murk = land > 0.5f ? wet.murk : 0.0f;
    #define DW_MURK_TINT PuddleMurkTint(murk)
#else
    const float murk = 0.0f;
    #define DW_MURK_TINT 1.0f
#endif
    o.albedo.rgb = wetAlbedo;

    // The record's lit colour (its ambient: the weather's and the sky's, x the graded albedo; Includes/Terrain.hlsl getSunLighting with the
    // sun compiled out) made wet: x the wet graded albedo / the dry one, per channel (0 where there was none). Then the film's sky reflection
    // (TerrainTemplate.hlsl, the same terms), over the land under the same fog.
    // (objects: their ambient and glow take the albedo itself, Includes/Object.hlsl getAmbientLighting: no grading)
    float3 lit = DWUnfog(frame, keep, DW_Fog.w);
    const float sat = land > 0.5f ? TESR_TerrainExtraData.y : 1.0f;
    const float3 gradedDry = lerp(luma(albedo.rgb), albedo.rgb, sat);
    const float3 gradedWet = lerp(luma(wetAlbedo), wetAlbedo, sat);
    lit *= gradedDry > 1.0e-5f ? min(gradedWet / max(gradedDry, 1.0e-5f), 4.0f) : 1.0f;
    [branch] if (wet.gloss > 0.001f) {
        const float3 wetV = -normalize(P);
        const float3 wetR = reflect(-wetV, wet.normal);
        const float wetHorizon = saturate(1.0f + 1.2f * dot(wetR, geoN));
        lit += WetSkyReflection(wetR) * WetReflectance(saturate(dot(wet.normal, wetV)), wet.roughness, wet.gloss) * wetHorizon * wetHorizon * DW_MURK_TINT *
            DWSkyKeep(geoN.z, viewZ, DW_Ctrl.z);
    }
    frame = DWFog(lit, keep, DW_Fog.w);
    // the forward land's debug views (they replace its fogged colour, which LinearSceneOut then decodes)
    [branch] if (WETNESS_DEBUG_COVER) frame = pow(max(WetnessCoverDebug(P, geoN), 1.0e-6f), max(TESR_LinearLighting.x, 1.0f));
#ifdef PUDDLES_COMPILED
    [branch] if (PuddleDebugColor.x >= 0.0f) frame = pow(max(PuddleDebugColor, 1.0e-6f), max(TESR_LinearLighting.x, 1.0f));
#endif
    o.frame = float4(covered ? frame0 : frame, 1.0f);

    // the film for the light passes (gloss 0: none)
    const float2 fn = ShadowEncodeNormal(normalize(wet.normal));
    o.film = float4(floor(saturate(murk) * 8.0f + 0.5f) + 0.99f * saturate(wet.gloss), wet.roughness, fn.x, fn.y);
    }
    return o;
}
#endif	// DW_FALLBACK
#endif
