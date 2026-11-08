// UNOFFICIAL forward point-light shadows (interiors) -- PROTOTYPE, see NVR-Lunacy\docs\interior-forward-shadows.md.
//
// The screen-space interior pass (Effects\PointShadows.fx + ShadowsInteriors.fx) can only darken the finished image:
// one mask for all lights, ambient and glow included. Here each point light's own direct term is multiplied by its own
// cube-map shadow inside the object shader, the way GetSunShadow (Shadow.hlsl) treats the sun outdoors.
//
// Compiled in only when INTERIOR_SHADOWS is 1 (ShaderRecord::LoadShader, from the setting at game start), so with the
// setting off the object shaders are exactly what they were. Whether it RUNS is decided per draw by the DLL.
//
// Per draw (SetShadersHook, NewVegas\Hooks\Render.cpp), the DLL walks the game's render pass light list
// (BSShaderManager::pCurrentRenderPass, 0x011F91E0: +0x09 light count, +0x0C ShadowSceneLight* array; its order is
// the order of PSLightColor) and, for each light k that NVR rendered a shadow cube map for, writes:
//   NVR_PointShadowLight[k] = xyz: the light's position as the cube map was rendered, minus the camera position
//                             (the camera-relative space of GetShadowWorldPos); w: 1 / the radius the cube map
//                             stores distances over (0 = no shadow for this light, the default),
// and binds that cube map to NVR_PointShadowCube<k>. Not TESR_ names on purpose: ShaderRecord::SetCT binds TESR_
// constants by name when the shader changes, and these change per draw.
//
// Registers: c174-c187 sit above GunFX's c171-c173 and below every ps_3_0 limit; s8 and s11-s15 are free in every
// pixel shader that includes this (ObjectTemplate, SKIN2002/2006, SkinVPSTemplate, SM3002/3003/3005/3007: game textures
// use s0-s7, the sun atlas s9, its cross-fade copy s10).

#ifndef INTERIOR_SHADOWS
    #define INTERIOR_SHADOWS 0
#endif
// UNOFFICIAL 2026-11-04 POINT_LAMP_EXTRA(k): one more per-pixel factor on light k in POINT_SHADOW / SHADOWED (ObjectTemplate.hlsl: the
// flashlight's game lamp's cone, core/FlashlightLamp.h); 1 unless the includer defines it before the include.
#ifndef POINT_LAMP_EXTRA
    #define POINT_LAMP_EXTRA(k) 1.0f
#endif

// UNOFFICIAL 2026-10-31 LampCapsCensus view ([Main.Main.Misc] LampCapsCensus at startup, development builds; ShaderRecord::BuildDefines
// LAMP_CAPS_VIEW; docs/next/DEFERRED-RENDERING-PLAN.md stage 0b): NVR_CapsUnshadowed counts, per pixel, the lamps that light it with NO shadow
// source (no cube map, atlas tile or traced entry: the lookup's first early-out below, and every many-lamp loop lamp past the shadowed six,
// Includes/ManyLightsDirect.hlsl); ObjectTemplate.hlsl shows it red as debug view 4 ([Shaders.ShadowsInteriors.Forward] DebugView 4). 0 (the
// default): nothing added, the shaders byte for byte as before.
#ifndef LAMP_CAPS_VIEW
    #define LAMP_CAPS_VIEW 0
#endif
#if LAMP_CAPS_VIEW
static float NVR_CapsUnshadowed = 0.0f;
#define CAPS_NOTE_UNSHADOWED(att) NVR_CapsUnshadowed += (att) > 0.0f ? 1.0f : 0.0f;
#else
#define CAPS_NOTE_UNSHADOWED(att)
#endif

#if INTERIOR_SHADOWS

// UNOFFICIAL 2026-10-15 [Shaders.ShadowsInteriors.Forward] SmoothEdges (needs a restart): compiled in only when on
// (ShaderRecord::LoadShader), as it adds about 50 instruction slots to the 4-lamp object shaders (6 lamp slots each).
#ifndef POINT_SHADOW_SMOOTH
    #define POINT_SHADOW_SMOOTH 0
#endif

// UNOFFICIAL 2026-10-20 POINT_SHADOW_FIT (ShaderManager::LoadShader, PointShadowForward::FitLevel): a shorter version of the
// lookups below, for a shader the GPU refuses whole. D3D9 drivers take about 3,600 instruction slots in one pixel shader; with
// HandBlood on, the six-lamp shiny-object shaders came to 3,820-4,016 indoors and were refused (P82 tester, RTX 4070 Ti SUPER;
// the GTX 1070 refuses them too): those objects then drew without lamp shadows whenever a third lamp reached them, and the
// shadows came and went. 0: as before (the first try, always). 1: High/Ultra's extra taps in a loop (the same taps and weights,
// the same shadows; about 90 slots less per lamp). 2: no High/Ultra (the DLL then caps Quality at Medium for every object, so
// they all match). 3: also no blocker search (the DLL then turns contact hardening and LampSize off for every object's lamps).
#ifndef POINT_SHADOW_FIT
    #define POINT_SHADOW_FIT 0
#endif

// UNOFFICIAL 2026-10-21 POINT_SHADOW_MAG ([Shaders.ShadowsInteriors.Forward] MagnifiedSoftness above 0 at startup; ShaderRecord::
// BuildDefines): the spread is at least MagnifiedSoftness texels where one cube texel covers several screen pixels (c188), and
// NearFade's lookup folded into the 'no data' test (PointShadowKeep: the same values, fewer instructions, the room for it). 0: as
// before, byte for byte.
#ifndef POINT_SHADOW_MAG
    #define POINT_SHADOW_MAG 0
#endif

// UNOFFICIAL 2026-10-21 POINT_SHADOW_EVEN ([Shaders.ShadowsInteriors.Forward] EvenTaps at startup): High's and Ultra's extra taps
// spread evenly (PointShadowEven: High 8 taps, Ultra 16) instead of on the 5x5 grid (10 and 20). The grid gave an edge only five tap
// positions across it, eight of Ultra's twenty on the centre line: wide (LampSize) blurs showed as stacked copies of their edge.
// 0: as before, byte for byte.
#ifndef POINT_SHADOW_EVEN
    #define POINT_SHADOW_EVEN 0
#endif

// UNOFFICIAL 2026-10-21 POINT_SHADOW_TEXEL ([Shaders.ShadowsInteriors.Forward] TexelFilter at startup): the lamp shadows filtered the
// way the sun's PCF is (Shadow.hlsl ShadowPCF3x3): plain compares at exact texel centres, 3 x 3 of them round the point, weighed with a
// quadratic B-spline by where the point sits between them (PointShadowTexel). The edge is then a smooth curve, not the texel grid's
// stair-steps. Replaces Quality's taps (Medium, High and Ultra alike) and SmoothEdges' soft compare; NearFade does not apply. 0: as before.
#ifndef POINT_SHADOW_TEXEL
    #define POINT_SHADOW_TEXEL 0
#endif

// UNOFFICIAL 2026-10-30 POINT_SHADOW_ATLAS ([Shaders.ShadowsInteriors.Atlas] Enabled at startup; core/ShadowAtlas.h,
// docs/next/INTERIOR-SHADOWS-MODERN.md step 1): every lamp shadow NVR publishes (the live slots and the pool's still maps) is also an
// octahedral tile in ONE depth atlas (same contents: distance / radius, in the cube lookup's direction convention), read through one
// sampler on s8 instead of six cube maps. Each light's tile replaces its debug tint: NVR_PointShadowTint[k].rgb = (u, v of the tile's
// first texel centre, the tile's span in atlas uv); the debug views 1-2 then tint every lamp red. Every filter stays as it is (the taps
// are the same directions, read through PointShadowAtlasTap); TexelFilter's face grid has no atlas form: its taps are Quality's here.
// 0: as before, byte for byte.
#ifndef POINT_SHADOW_ATLAS
    #define POINT_SHADOW_ATLAS 0
#endif
#if POINT_SHADOW_ATLAS
    #undef POINT_SHADOW_TEXEL
    #define POINT_SHADOW_TEXEL 0
#endif

float4 NVR_PointShadowLight[6]  : register(c174); // per light of the draw, see above
float4 NVR_PointShadowParams    : register(c180); // x: strength (1 = a blocked light adds nothing), y: depth bias (share of the
                                                  // distance, 0.018 as PointShadows.fx), z: normal offset (world units per
                                                  // world unit of distance; 0 = none, as the screen-space pass), w: PCF tap
                                                  // spread (cube direction units per unit of the major axis; 0 = 1 tap)

float4 NVR_PointShadowDebug     : register(c181); // x: debug view (0 off, 1 a lamp's blocked light in that lamp's colour, 2 the lit
                                                  // area of every lamp that has a shadow tinted in its colour, 3 PointShadowTrace);
                                                  // y: NearFade (world units; 0 off), see PointShadowNear; z (view 3): which light
                                                  // of the draw the focus lamp is, -1 none; z (every other view, UNOFFICIAL
                                                  // 2026-10-20): InvisibleLightSize / sqrt 2, the lamp size of lights whose fade
                                                  // (NVR_PointShadowTint w) is negative (view 3 replaces the lighting, so the
                                                  // shadows it would size are never seen); w: Quality (PCF taps with a
                                                  // spread: 1 Medium = 4, as before; 2 High = 10; 3 Ultra = 20; Low
                                                  // arrives as spread 0 = 1 tap)
float4 NVR_PointShadowTint[6]   : register(c182); // rgb: debug colour of light k's lamp (one per cube-map slot; white = no shadow);
                                                  // w: how far light k's shadow has faded in, 0..1 (a lamp that just got one);
                                                  // negative (the same amount): an invisible light with its own lamp size
                                                  // (InvisibleLightSize, c181.z)
float4 NVR_PointShadowFocus     : register(c188); // view 3: the focus lamp (cube slot 0) as NVR_PointShadowLight, whatever the draw's
                                                  // lamps; its cube map is on NVR_PointShadowCube5. UNOFFICIAL 2026-10-21, every
                                                  // other view (POINT_SHADOW_MAG): MagnifiedSoftness, x y: how many screen pixels
                                                  // one cube texel covers, as a 0..1 ramp = saturate(lamp distance / camera
                                                  // distance x x + y); z: the spread it then gives at least, per unit of the
                                                  // direction's major axis (0 = off); w (POINT_SHADOW_TEXEL): the cube maps'
                                                  // face resolution (ShadowCubeMapSize)
float4 NVR_PointShadowSoft      : register(c189); // UNOFFICIAL contact hardening ([Shaders.ShadowsInteriors.Forward] ContactHardening):
                                                  // x: strength (0 = off, the spread as before), y: the narrowest spread, as a
                                                  // share of the Softness spread (where a blocker touches what it shadows);
                                                  // z: LampSize / sqrt 2 (world units; 0 = the ratio above), w: the widest spread
                                                  // LampSize (or InvisibleLightSize) may give, per unit of the direction's major
                                                  // axis (MaxSoftness); 0 with contact hardening alone

// UNOFFICIAL 2026-10-28 POINT_SHADOW_RT ([Shaders.ShadowsInteriors.RT] Enabled at startup, on NVR DXVK with the RT core: ShaderRecord::
// BuildDefines; docs/next/FORK-RT-LAMPS.md): each lamp's shadow read from a traced result instead of its cube map. The RT core's 64-bit
// helper (32-bit Windows Vulkan drivers have no ray tracing) traces up to 9 shadowed lamps per pixel from a frame's depth
// (Compute/Rt/RtLampTrace.comp; core/RtLampShadows.cpp) and NVR copies the result into NVR_RtLampVis (G32R32F): 4-bit visibility for
// lamp slots 0-5 in r, 6-8 in g's low 12 bits, a 12-bit log code of the view depth above them. The next frame's points reproject into
// that frame (NVR_RtLampReproject) and take the traced value where the depth there is their own; elsewhere (hidden in that frame, off
// its screen, a slot past MaxLamps) the cube map as before.
// The cube path stays in the shader, for the A/B (c215.x 0) and the compare view. 0: as before, byte for byte.
// UNOFFICIAL 2026-10-30 POINT_SHADOW_RT_ONLY: the outdoor lamp-shadow records ([Shaders.ShadowsInteriors.RT] ExteriorForward,
// ShaderManager::LoadShader): the traced result only, no cube-map code or samplers (outdoors the stages and the instruction budget
// belong to the sun), the reprojection on c200-c203 (c206-c209 are the character shadow map's outdoors). A lamp not traced at a
// pixel stays lit, as every lamp outdoors was before.
#ifndef POINT_SHADOW_RT_ONLY
    #define POINT_SHADOW_RT_ONLY 0
#endif
#if POINT_SHADOW_RT_ONLY
    #undef POINT_SHADOW_RT
    #define POINT_SHADOW_RT 1
#endif
#ifndef POINT_SHADOW_RT
    #define POINT_SHADOW_RT 0
#endif

#if POINT_SHADOW_RT
#if POINT_SHADOW_RT_ONLY
row_major float4x4 NVR_RtLampReproject : register(c200); // (outdoors: c206-c209 hold the character shadow map's; c200-c203 are free in the object pixel shaders, ps_3_0 ends at c223)
#else
row_major float4x4 NVR_RtLampReproject : register(c206); // camera-relative position (this frame) -> the traced frame's clip space
                                                // (w = its view depth); c206-c209 are the character shadow map's outdoors only
#endif
float4 NVR_PointShadowRT      : register(c215); // x: 0 = the cube maps (as without RT), 1 = the traced result, 2 = the compare view
                                                // (both; see PointShadowFactor); y: depth tolerance (share of the view depth); z:
                                                // log2(farZ) / 4095 (the depth code's unit); w: 1 = the darker of traced and cube map
                                                // (the RT scene has no people yet: their shadows come from the cube maps); (2026-11-07)
                                                // + 1 / the traced result's width (its fraction: the edge search's texel, 0 = no search)
float4 NVR_PointShadowRTSlot[2] : register(c216); // the draw's lights 0-3 and 4-5: their cube slot (0-62) when the lamp is in the trace's table, -1 = cube map; [1].z DebugView 3's slot
                                                // [1].w (2026-10-31): 1 / the result's packing unit, 1024 or 4096 (ManyLights: lamp ids 0-254 in the
                                                // slots, + 256 for a lamp with no shadow map of its own)
sampler2D NVR_RtLampVis : register(s15);          // point / clamp, set by the DLL (PointShadowForward::Bind; no sampler_state:
                                                // the POINT variants define POINT, and raw shader compiles ignore it anyway)
                                                // on s15, the sixth lamp's cube map's: every other sampler is taken in some variant
                                                // (s9 the sun atlas, s10 the live cube). So with RT the sixth light of a draw (rare: the
                                                // six-lamp OPT passes) has no cube map: traced where it can be, else lit; DebugView 3
                                                // (which reads the focus lamp's cube on s15) shows grey
#endif

#if POINT_SHADOW_RT_ONLY
// never sampled (PointShadowCube ends at the traced result): the compiler drops them, they take no stage
samplerCUBE NVR_PointShadowCube0;
samplerCUBE NVR_PointShadowCube1;
samplerCUBE NVR_PointShadowCube2;
samplerCUBE NVR_PointShadowCube3;
samplerCUBE NVR_PointShadowCube4;
#elif POINT_SHADOW_ATLAS
sampler2D NVR_PointShadowAtlas : register(s8);     // linear, clamp (PointShadowForward::Bind)
#define NVR_PointShadowCube0 NVR_PointShadowTint[0]
#define NVR_PointShadowCube1 NVR_PointShadowTint[1]
#define NVR_PointShadowCube2 NVR_PointShadowTint[2]
#define NVR_PointShadowCube3 NVR_PointShadowTint[3]
#define NVR_PointShadowCube4 NVR_PointShadowTint[4]
#define NVR_PointShadowCube5 NVR_PointShadowTint[5]
#else
samplerCUBE NVR_PointShadowCube0 : register(s8);
samplerCUBE NVR_PointShadowCube1 : register(s11);
samplerCUBE NVR_PointShadowCube2 : register(s12);
samplerCUBE NVR_PointShadowCube3 : register(s13);
samplerCUBE NVR_PointShadowCube4 : register(s14);
#if !POINT_SHADOW_RT
samplerCUBE NVR_PointShadowCube5 : register(s15);
#endif
#endif

#if POINT_SHADOW_ATLAS && !POINT_SHADOW_RT_ONLY
// A direction to its octahedral square, 0..1 (z+ in the middle, z- folded out to the corners).
float2 PointShadowOct(float3 v) {
    float3 n = v / (abs(v.x) + abs(v.y) + abs(v.z));
    float2 sgn = float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    float2 p = n.z < 0.0f ? (1.0f - abs(n.yx)) * sgn : n.xy;
    return p * 0.5f + 0.5f;
}
// One tap: what the light's cube map would hold in direction v, from its atlas tile.
float PointShadowAtlasTap(float4 tile, float3 v) {
    return tex2Dlod(NVR_PointShadowAtlas, float4(tile.xy + PointShadowOct(v) * tile.z, 0.0f, 0.0f)).r;
}
#define PS_CUBE_T float4
#define PS_TAP(cube, v) PointShadowAtlasTap(cube, v)
// UNOFFICIAL 2026-11-01 TruePCF ([Shaders.ShadowsInteriors.Atlas] TruePCF, live; user: lamp shadows don't trace the caster's outline, they
// look like an imprint of its lit side, and blocky). The four Medium taps read the atlas with linear filtering and judged the blended
// distance: across an edge the caster's and the floor's distances blend, so every edge grew by up to a texel along the texel grid,
// and SmoothEdges blocked each tap by how far it lay below the NEAREST blocker of the four, so a caster's own depth relief (its lit
// side) showed through as lighter and darker patches inside its shadow. TruePCF compares the four texels round each tap with the
// threshold first and blends the results (bilinear PCF), so a shadow is the caster's outline, evenly dark, with a one-texel soft edge.
// NVR_PointShadowFocus.w (TexelFilter's face size, unused with the atlas): the atlas's size in texels, 0 = off (the old taps).
// Not in POINT_SHADOW_FIT 3 (the deferred lamp pass for lamps past a pixel's 16th has no temp registers to spare).
#if POINT_SHADOW_FIT < 3
#define POINT_SHADOW_PCF 1
#endif
#else
#define PS_CUBE_T samplerCUBE
#define PS_TAP(cube, v) texCUBElod(cube, float4(v, 0.0f)).r
#endif

// How much a blocker at the stored distance (distance / radius) shadows: [Shaders.ShadowsInteriors.Main] NearFade, less
// within that many world units of the lamp, nothing at the lamp itself. A real lamp is no point: a blocker that close to
// it casts a shadow too blurred to see, where a point casts it sharp, as large as the room, and on every side when the
// point is inside the blocker (a lamp at the player's head). lightW: 1 / the radius.
float4 PointShadowNear(float4 stored, float lightW) {
    return NVR_PointShadowDebug.y > 0.0f ? smoothstep(0.0f, NVR_PointShadowDebug.y * lightW, stored) : 1.0f;
}

#if POINT_SHADOW_FIT == 1
// POINT_SHADOW_FIT 1: High's six taps (along each axis at spread s), then Ultra's ten (the four diagonals and the six axes at half
// the spread), as PointShadowCube's unrolled code takes them; w: the share of the spread.
static const float4 PointShadowTaps[16] = {
    float4( 1.0f, 0.0f, 0.0f, 1.0f), float4(-1.0f, 0.0f, 0.0f, 1.0f), float4(0.0f,  1.0f, 0.0f, 1.0f), float4(0.0f, -1.0f, 0.0f, 1.0f),
    float4(0.0f, 0.0f,  1.0f, 1.0f), float4(0.0f, 0.0f, -1.0f, 1.0f),
    float4( 1.0f,  1.0f,  1.0f, 0.5f), float4(-1.0f, -1.0f,  1.0f, 0.5f), float4( 1.0f, -1.0f, -1.0f, 0.5f), float4(-1.0f,  1.0f, -1.0f, 0.5f),
    float4( 1.0f, 0.0f, 0.0f, 0.5f), float4(-1.0f, 0.0f, 0.0f, 0.5f), float4(0.0f,  1.0f, 0.0f, 0.5f), float4(0.0f, -1.0f, 0.0f, 0.5f),
    float4(0.0f, 0.0f,  1.0f, 0.5f), float4(0.0f, 0.0f, -1.0f, 0.5f) };
#endif

#if POINT_SHADOW_EVEN
// UNOFFICIAL 2026-10-21 EvenTaps: High's four extra taps (the first four) and Ultra's eight more, as offsets in units of the spread s
// (w: 1, the share of the spread, for the POINT_SHADOW_FIT 1 loop). A 3D Latin hypercube (every axis takes every level once), so on
// each cube face the taps land on the offset's two other components at distinct places, for edges at any angle; picked by
// bench\deep-roadmap\evidence\lampedge\taps.py (random search, fewest taps bunched together over faces, edge angles and the shear
// toward a face's edges), scaled so each Quality's blur width (per-axis variance with the four Medium taps) is as before. Fewer taps
// than the grid's because scattered taps share fewer texture-cache lines: Ultra with twenty cost 0.85 ms more than the grid in the
// Lucky 38, sixteen look the same as twenty (sim7.py).
static const float4 PointShadowEven[12] = {
    float4(-0.60000f, +0.60000f, +0.20000f, 1.0f), float4(-0.20000f, -0.20000f, -0.20000f, 1.0f),
    float4(+0.20000f, -0.60000f, +0.60000f, 1.0f), float4(+0.60000f, +0.20000f, -0.60000f, 1.0f),
    float4(-0.08452f, -0.25355f, -0.59161f, 1.0f), float4(-0.42258f, +0.42258f, +0.59161f, 1.0f),
    float4(+0.59161f, -0.08452f, -0.08452f, 1.0f), float4(+0.25355f, +0.08452f, +0.42258f, 1.0f),
    float4(-0.59161f, +0.25355f, -0.42258f, 1.0f), float4(+0.42258f, -0.59161f, +0.25355f, 1.0f),
    float4(+0.08452f, +0.59161f, -0.25355f, 1.0f), float4(-0.25355f, -0.42258f, +0.08452f, 1.0f) };
#endif

#if POINT_SHADOW_MAG || POINT_SHADOW_EVEN
// UNOFFICIAL 2026-10-21: how much a tap with stored distance v counts (before the compare): 0 for v = 0 (no data), NearFade's fade
// near the lamp. POINT_SHADOW_MAG: in one, t = saturate(v x nearK), smoothstep's t x t x (3 - 2 t), nearK = 1 / (NearFade as a share
// of the radius), or 2^60 with NearFade 0 (t is then 1 for any stored distance above 0, 0 for 0): the same values as
// (v > 0) x PointShadowNear, two instructions less per four taps.
float4 PointShadowKeep(float4 v, float nearK, float lightW) {
#if POINT_SHADOW_MAG
    float4 t = saturate(v * nearK);
    return t * t * (3.0f - 2.0f * t);
#else
    return (float4)(v > 0.0f) * PointShadowNear(v, lightW);
#endif
}
#endif

#if POINT_SHADOW_EVEN
// The blocked count of four even taps from PointShadowEven[i]: plain compares, as High and Ultra's extra taps always were.
float PointShadowEven4(PS_CUBE_T cube, float3 dir, float s, float threshold, float nearK, float lightW, int i) {
    float4 v = float4(
        PS_TAP(cube, dir + s * PointShadowEven[i].xyz),
        PS_TAP(cube, dir + s * PointShadowEven[i + 1].xyz),
        PS_TAP(cube, dir + s * PointShadowEven[i + 2].xyz),
        PS_TAP(cube, dir + s * PointShadowEven[i + 3].xyz));
    return dot((float4)(v <= threshold) * PointShadowKeep(v, nearK, lightW), 1.0f);
}
#endif

#if POINT_SHADOW_PCF
// UNOFFICIAL 2026-11-01 TruePCF (see POINT_SHADOW_PCF above): the blocked share at direction v, a bilinear PCF of the four atlas
// texels round it (NVR_PointShadowFocus.w: the atlas size in texels).
float PointShadowAtlasBlocked(float4 tile, float3 v, float threshold, float nearK, float lightW) {
    float size = NVR_PointShadowFocus.w;
    float2 p = (tile.xy + PointShadowOct(v) * tile.z) * size - 0.5f;
    float2 f = frac(p);
    float2 b = (p - f + 0.5f) / size;
    float t = 1.0f / size;
    // texel centres: the linear sampler returns each texel's own value there
    float4 s = float4(tex2Dlod(NVR_PointShadowAtlas, float4(b, 0.0f, 0.0f)).r, tex2Dlod(NVR_PointShadowAtlas, float4(b + float2(t, 0.0f), 0.0f, 0.0f)).r,
                      tex2Dlod(NVR_PointShadowAtlas, float4(b + float2(0.0f, t), 0.0f, 0.0f)).r, tex2Dlod(NVR_PointShadowAtlas, float4(b + t, 0.0f, 0.0f)).r);
#if POINT_SHADOW_MAG
    float4 blocked = (float4)(s <= threshold) * PointShadowKeep(s, nearK, lightW);
#else
    float4 blocked = (float4)(s <= threshold) * (float4)(s > 0.0f) * PointShadowNear(s, lightW);
#endif
    return lerp(lerp(blocked.x, blocked.y, f.x), lerp(blocked.z, blocked.w, f.x), f.y);
}
#endif

// UNOFFICIAL Quality High/Ultra: the blocked share summed over six taps, one along each axis at spread s.
float PointShadowRing(PS_CUBE_T cube, float3 dir, float s, float threshold, float lightW) {
    float4 a = float4(
        PS_TAP(cube, dir + float3( s, 0.0f, 0.0f)),
        PS_TAP(cube, dir + float3(-s, 0.0f, 0.0f)),
        PS_TAP(cube, dir + float3(0.0f,  s, 0.0f)),
        PS_TAP(cube, dir + float3(0.0f, -s, 0.0f)));
    float2 b = float2(
        PS_TAP(cube, dir + float3(0.0f, 0.0f,  s)),
        PS_TAP(cube, dir + float3(0.0f, 0.0f, -s)));
    return dot((float4)(a <= threshold) * (float4)(a > 0.0f) * PointShadowNear(a, lightW), 1.0f) +
           dot((float2)(b <= threshold) * (float2)(b > 0.0f) * PointShadowNear(float4(b, 1.0f, 1.0f), lightW).xy, 1.0f);
}

#if POINT_SHADOW_MAG
// PointShadowRing with PointShadowKeep (the same values).
float PointShadowRingK(PS_CUBE_T cube, float3 dir, float s, float threshold, float nearK) {
    float4 a = float4(
        PS_TAP(cube, dir + float3( s, 0.0f, 0.0f)),
        PS_TAP(cube, dir + float3(-s, 0.0f, 0.0f)),
        PS_TAP(cube, dir + float3(0.0f,  s, 0.0f)),
        PS_TAP(cube, dir + float3(0.0f, -s, 0.0f)));
    float4 b = float4(
        PS_TAP(cube, dir + float3(0.0f, 0.0f,  s)),
        PS_TAP(cube, dir + float3(0.0f, 0.0f, -s)), 1.0f, 1.0f);
    return dot((float4)(a <= threshold) * PointShadowKeep(a, nearK, 0.0f), 1.0f) + dot(((float4)(b <= threshold) * PointShadowKeep(b, nearK, 0.0f)).xy, 1.0f);
}
#endif

#if POINT_SHADOW_TEXEL
// UNOFFICIAL 2026-10-21 TexelFilter: one lattice level of PointShadowTexel. q: the point in the face's texel grid (texel centres at whole
// numbers); k: the lattice spacing in texels (1, 2, 4 ...: lattice points at multiples of k, on texel centres); M, A, B (w 0): the
// major axis (the direction's own component) and the face's two axes x the major component, so M + u A + v B points at the face
// point (u, v); scale: 2 / resolution. The blocked share of the 3 x 3 lattice points round q, quadratic B-spline weights by where q
// sits between them (as the sun's Shadow.hlsl ShadowPCF3x3). A stored 0 (no data) is lit, as everywhere; NearFade is not applied
// here (it would cost the six-lamp shaders too many instructions; 0 by default).
float PointShadowTexelLevel(PS_CUBE_T cube, float2 q, float k, float4 M, float4 A, float4 B, float scale, float threshold) {
    float2 g = q / k;
    float2 c = floor(g + 0.5f);                     // the nearest lattice point
    float2 f = g - c;                               // -0.5 .. 0.5 from it
    float3 wx = float3(0.5f * (0.5f - f.x) * (0.5f - f.x), 0.75f - f.x * f.x, 0.5f * (0.5f + f.x) * (0.5f + f.x));
    float3 wy = float3(0.5f * (0.5f - f.y) * (0.5f - f.y), 0.75f - f.y * f.y, 0.5f * (0.5f + f.y) * (0.5f + f.y));
    float3 ux = ((c.x + float3(-1.0f, 0.0f, 1.0f)) * k + 0.5f) * scale;  // the lattice columns' face coordinates + 1, x the major component
    float3 vy = ((c.y + float3(-1.0f, 0.0f, 1.0f)) * k + 0.5f) * scale;  // and rows'
    float3 rows;
    [unroll] for (int y = 0; y < 3; y++) {
        float4 row = M + vy[y] * B;
        float3 v = float3(texCUBElod(cube, row + ux.x * A).r, texCUBElod(cube, row + ux.y * A).r, texCUBElod(cube, row + ux.z * A).r);
        rows[y] = dot((float3)(v <= threshold) * (float3)(v > 0.0f), wx);
    }
    return dot(rows, wy);
}

// UNOFFICIAL 2026-10-21 TexelFilter: the blocked share at spread s (direction units, as the taps' spread), filtered as the sun's PCF:
// plain compares at texel centres of a 3 x 3 lattice round the point, weighed with a quadratic B-spline, so the edge is a smooth curve
// instead of the texel grid's stair-steps. The lattice spacing is 1.22 x the spread in texels (the same blur width as Ultra's taps at
// that spread: the quadratic B-spline's sigma is half the spacing), at least one texel (the sun's own filter). A wider blur takes the
// coarser lattices 2, 4, 8 ... texels apart, blended between the two nearest by the fraction, so the blur widens smoothly where
// LampSize or contact hardening change the spread from pixel to pixel; the second lattice is read only there (and not at all in the
// shorter POINT_SHADOW_FIT versions, for a shader the GPU refused whole: there the nearest lattice alone). Taps past a face's edge
// read the next face (not on its texel centres: a little less smooth along the cube's seams only).
float PointShadowTexel(PS_CUBE_T cube, float3 dir, float major, float s, float threshold, float lightW) {
    float3 ad = abs(dir);
    float xm = (float)(ad.x >= max(ad.y, ad.z));
    float zm = (1.0f - xm) * (float)(ad.z > ad.y);
    float4 A = float4(1.0f - xm, xm, 0.0f, 0.0f) * major;       // the face's first axis: y on the X faces, x on the others
    float4 B = float4(0.0f, zm, 1.0f - zm, 0.0f) * major;       // and its second: y on the Z faces, z on the others
    float4 M = float4(dir * float3(xm, 1.0f - xm - zm, zm), 0.0f) - A - B;   // the major axis, less the face's (-1, -1) corner
    float resolution = NVR_PointShadowFocus.w;
    float2 q = (float2(dot(dir, A.xyz), dot(dir, B.xyz)) / (major * major) + 1.0f) * (0.5f * resolution) - 0.5f;   // texel centres at whole numbers
    float spacing = max(1.0f, 0.61f * s * resolution / major);  // 1.22 x the spread in texels (s / major x resolution / 2)
    float k = exp2(floor(log2(spacing)));
    float mix = spacing / k - 1.0f;
    float scale = 2.0f / resolution;                            // texels -> face coordinates (A and B carry the major component)
    float blocked = PointShadowTexelLevel(cube, q, k, M, A, B, scale, threshold);
#if POINT_SHADOW_FIT < 1
    [branch] if (mix > 0.001f) blocked = lerp(blocked, PointShadowTexelLevel(cube, q, 2.0f * k, M, A, B, scale, threshold), mix);
#endif
    return blocked;
}
#endif

#if POINT_SHADOW_RT
// UNOFFICIAL 2026-10-28 RT lamp shadows: how much of the lamp in cube slot `slot` (0-62) reaches this point (1 lit, 0 blocked), or
// negative when the traced frame did not see this point: -1 off its screen or behind its camera, -2 - error (up to -3) another depth
// there (the sky, something in front). The caller then takes the cube map.
// Point sampled: the channels hold packed integers (exact below 2^24; dividing by powers of 2 keeps them exact).
float PointShadowTraced(float slot, float3 worldPos) {
    float4 clip = mul(float4(worldPos, 1.0f), NVR_RtLampReproject);
    [branch] if (!(clip.w > 1.0f)) return -1.0f;
    float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    [branch] if (any(abs(uv - 0.5f) > 0.5f)) return -1.0f;
    float2 traced = tex2Dlod(NVR_RtLampVis, float4(uv, 0.0f, 0.0f)).xy;
    // 2026-10-31: the packing unit U from c217.w (1 / U, a power of 2: every step below stays exact): 1024, or 4096 with ManyLights
    float unit = NVR_PointShadowRTSlot[1].w;
    float2 scaled = traced * unit;
    float2 high = floor(scaled);                    // entry 1, the depth code
    float depthError = abs(exp2(high.y * NVR_PointShadowRT.z) - clip.w) / clip.w;
    // (2026-11-07, a tester's red lines along every edge outdoors: ExteriorForward lights a lamp fully where the traced frame has no
    // answer) a silhouette pixel's own traced texel is often the other side of the edge (the ray through its centre + jitter met the
    // surface behind or in front): its four neighbours one texel away, the first whose depth is this point's, before giving up
    [branch] if (!(depthError <= NVR_PointShadowRT.y)) {
        float tx = frac(NVR_PointShadowRT.w);
        bool found = false;
        [branch] if (tx > 0.0f) {
            // the traced frame's texel height from its width: the projection's y / x scales (the reprojection's columns) = width / height
            float ty = tx * length(float3(NVR_RtLampReproject[0][1], NVR_RtLampReproject[1][1], NVR_RtLampReproject[2][1])) /
                max(length(float3(NVR_RtLampReproject[0][0], NVR_RtLampReproject[1][0], NVR_RtLampReproject[2][0])), 1.0e-6f);
            [loop] for (float i = 0.0f; i < 3.5f; i += 1.0f) {
                float2 o = i < 0.5f ? float2(tx, 0.0f) : i < 1.5f ? float2(-tx, 0.0f) : i < 2.5f ? float2(0.0f, ty) : float2(0.0f, -ty);
                float2 t2 = tex2Dlod(NVR_RtLampVis, float4(uv + o, 0.0f, 0.0f)).xy;
                float2 h2 = floor(t2 * unit);
                [branch] if (abs(exp2(h2.y * NVR_PointShadowRT.z) - clip.w) <= NVR_PointShadowRT.y * clip.w) {
                    traced = t2; scaled = t2 * unit; high = h2; found = true; break;
                }
            }
        }
        [branch] if (!found) return -2.0f - saturate(depthError);  // -2..-3: the depth did not match
    }
    // 2026-10-29 v4 (RtLampTrace.comp): three entries, slot + U / 16 x visibility (16 steps); the lamp's own slot, or -4: this pixel
    // traced other lamps (they bring it more light), the cube map then. ManyLights (RtLampTraceMany.comp): two entries, the third's
    // place holds R, the traced share of every other lamp at this pixel, which such a lamp takes instead.
    float3 entries = float3(scaled.x - high.x, high.x * unit, scaled.y - high.y) * 16.0f;    // entry / (U / 16)
    float3 visible = floor(entries);
    float3 slots = entries - visible;                                                       // slot / (U / 16)
    float lampSlot = slot > 255.5f ? slot - 256.0f : slot;                                  // ManyLights: + 256 = no map of its own
    float3 match = (float3)(abs(slots - lampSlot * 16.0f * unit) < 8.0f * unit);
    bool many = unit < 0.0005f;
    match.z = many ? 0.0f : match.z;
    [branch] if (!(dot(match, 1.0f) > 0.5f)) return many ? saturate(entries.z * (256.0f / 4095.0f)) : -4.0f;
    return dot(match, visible) / (15.0f * dot(match, 1.0f));
}

// UNOFFICIAL 2026-10-29 RT coverage view (NVR_PointShadowRT.x 3, [Shaders.ShadowsInteriors.RT] DebugView 2): the colour the object and
// skin shaders write INSTEAD of their lighting (ObjectTemplate.hlsl, SkinTemplate.hlsl), for the first of the draw's lights that has a
// traced slot: GREEN = its traced value is used (brightness = how lit), BLUE = off the traced frame's screen or behind its camera,
// YELLOW = this pixel traced other lamps (stronger ones there),
// RED = the traced frame saw another depth there (brighter = further off), GREY = no light of this draw is traced. Exact per pixel
// (an earlier version sent the classes through the lighting as encoded shadow values, which other lighting maths then distorted).
float3 PointShadowRtCoverage(float3 worldPos) {
    // UNOFFICIAL 2026-10-31 DebugView 4 (x 5, ManyLights): R, the traced share of the lamps not traced exactly: cyan = lit .. black =
    // blocked, MAGENTA = no traced data there, dark blue = not the ManyLights packing
    [branch] if (NVR_PointShadowRT.x > 4.5f) {
        float rest = PointShadowTraced(-8.0f, worldPos);
        return rest >= 0.0f ? float3(0.0f, rest, rest) : rest < -3.5f ? float3(0.0f, 0.0f, 0.3f) : float3(1.0f, 0.0f, 1.0f);
    }
    // UNOFFICIAL 2026-10-29 DebugView 3 (x 4): ONE traced lamp, NVR_PointShadowRTSlot[1].z, at every pixel whatever the draw's own lamps:
    // white = lit .. black = blocked, MAGENTA = no traced data there (off the traced frame, another depth).
    [branch] if (NVR_PointShadowRT.x > 3.5f) {
        float one = PointShadowTraced(NVR_PointShadowRTSlot[1].z, worldPos);
        return one >= 0.0f ? one : float3(1.0f, 0.0f, 1.0f);
    }
    float slot = NVR_PointShadowRTSlot[0].x;
    slot = slot >= 0.0f ? slot : NVR_PointShadowRTSlot[0].y;
    slot = slot >= 0.0f ? slot : NVR_PointShadowRTSlot[0].z;
    slot = slot >= 0.0f ? slot : NVR_PointShadowRTSlot[0].w;
    slot = slot >= 0.0f ? slot : NVR_PointShadowRTSlot[1].x;
    slot = slot >= 0.0f ? slot : NVR_PointShadowRTSlot[1].y;
    [branch] if (slot < 0.0f) return 0.25f;
    float traced = PointShadowTraced(slot, worldPos);
    return traced >= 0.0f ? float3(0.0f, 0.2f + 0.8f * traced, 0.0f) : traced > -1.5f ? float3(0.0f, 0.0f, 1.0f) :
           traced < -3.5f ? float3(0.8f, 0.8f, 0.0f) : float3(0.3f + 0.7f * (-2.0f - traced), 0.0f, 0.0f);   // yellow: not traced here
}

// The compare view (NVR_PointShadowRT.x 2): both answers packed into the factor PointShadowFactor decodes, the traced and the cube
// map's lit share in 16 steps each (where nothing was traced, the cube map's for both).
float PointShadowDone(float cubeLit, float rayLit, float fade) {
    [branch] if (NVR_PointShadowRT.x > 1.5f)
        return 2.0f + round(saturate(rayLit < 0.0f ? cubeLit : rayLit) * 15.0f) + 16.0f * round(saturate(cubeLit) * 15.0f);
    // c215.w: people's shadows from the cube maps (the darker of the two where something was traced)
    return lerp(1.0f, rayLit >= 0.0f ? min(cubeLit, rayLit) : cubeLit, NVR_PointShadowParams.x * abs(fade));
}
#define POINT_SHADOW_EARLY_LIT PointShadowDone(1.0f, rayLit, fade)
#define POINT_SHADOW_RT_PARAM , float rtSlot
#define POINT_SHADOW_RT_ARG(bit) , bit
#define POINT_SHADOW_DONE(lit) PointShadowDone(lit, rayLit, fade)
#else
#define POINT_SHADOW_EARLY_LIT 1.0f
#define POINT_SHADOW_RT_PARAM
#define POINT_SHADOW_RT_ARG(bit)
#define POINT_SHADOW_DONE(lit) lerp(1.0f, lit, NVR_PointShadowParams.x * abs(fade))
#endif

// 1 lit, 0 fully shadowed (before strength). worldPos: camera-relative (GetShadowWorldPos in the VS). normal: the
// camera-facing geometric normal where the shader has one (GetShadowGeometricNormal), else 0. att: this light's
// attenuation here; 0 (out of reach, or no valid world position) skips the lookup. texCUBElod only, so it is legal
// inside the dynamic branches.
float PointShadowCube(PS_CUBE_T cube, float4 light, float fade, float3 worldPos, float3 normal, float att POINT_SHADOW_RT_PARAM) {
    // First the test on the constant alone (the same for the whole draw, so nearly free): a light without a cube map,
    // every light outdoors and every light while the feature is off end here.
    [branch] if (!(light.w > 0.0f)) { CAPS_NOTE_UNSHADOWED(att) return 1.0f; }
    float3 toPixel = worldPos - light.xyz;
    float len = length(toPixel);
    [branch] if (!(att > 0.0f) || !(len * light.w < 1.0f)) return 1.0f;   // beyond the cube map: nothing recorded
#if POINT_SHADOW_RT
    // RT lamp shadows: the traced result instead of the cube map (x 1), or both for the compare view (x 2). Same reach as the cube
    // map. Where the traced frame did not see this point, the cube map (and in the compare view both agree there: it shows the traced
    // area only).
    float rayLit = -1.0f;
    // (2026-11-07) the first-person model (arms, gun, Pip-Boy) is never in the traced scene: where its lamp has no map to fall back to
    // (outdoors, ManyLights' lamps without one) the DLL sends -2 - v, v = how much of the lamp the eye sees (RtLampEye.comp), for the
    // whole model; before, those lamps lit it fully however blocked they were (a red neon lamp inside its sign: red patches on the gun)
    [branch] if (rtSlot < -1.5f && NVR_PointShadowRT.x > 0.0f) {
        rayLit = saturate(-2.0f - rtSlot);
        return POINT_SHADOW_DONE(1.0f);
    }
    [branch] if (NVR_PointShadowRT.x > 0.0f && rtSlot >= 0.0f) {
        rayLit = PointShadowTraced(rtSlot, worldPos);
        [branch] if (NVR_PointShadowRT.x < 1.5f && rayLit >= 0.0f && NVR_PointShadowRT.w < 0.5f) return lerp(1.0f, rayLit, NVR_PointShadowParams.x * abs(fade));
    }
#endif
#if POINT_SHADOW_RT
    // 2026-10-31 ManyLights: a lamp with no shadow map of its own (+ 256): lit where the traced frame did not see the point
    [branch] if (rtSlot > 255.5f) return POINT_SHADOW_DONE(1.0f);
#endif
#if POINT_SHADOW_RT_ONLY
    return rayLit >= 0.0f ? lerp(1.0f, rayLit, NVR_PointShadowParams.x * abs(fade)) : 1.0f;   // not traced here: lit (as before)
#else
#if POINT_SHADOW_ATLAS && POINT_SHADOW_RT
    // UNOFFICIAL 2026-11-06 a traced lamp with no atlas tile (ShadowAtlas::LiveRect has tiles for cube slots 0-31 only; slots 32-63 have
    // none): it came here for its traced value alone (PointShadowForward.h / ManyLamps.h PutTiles); where the traced frame did not see the
    // point there is no map to read: lit, as such a lamp always was
    [branch] if (!(cube.z > 0.0f)) return POINT_SHADOW_DONE(1.0f);
#endif

    // Normal offset: a cube texel's world size grows with the distance, so does the push off the surface.
    float3 p = toPixel + normal * (NVR_PointShadowParams.z * len);
    float d = length(p) * light.w;                       // what the cube map stores: distance / radius
    float threshold = d - NVR_PointShadowParams.y * d;    // lit where stored + bias * d > d (Effects\Includes\Shadows.hlsl)
#if POINT_SHADOW_MAG
    float nearK = NVR_PointShadowDebug.y > 0.0f ? 1.0f / (NVR_PointShadowDebug.y * light.w) : 1152921504606846976.0f;	// PointShadowKeep
#elif POINT_SHADOW_EVEN
    float nearK = 0.0f;	// (unused: PointShadowKeep takes NearFade from light.w)
#endif

    // Face convention of ShadowCubeMap.vso: lookup = (light - point) * (-1, -1, 1) = (point - light) * (1, 1, -1).
    float3 dir = p * float3(1.0f, 1.0f, -1.0f);

    // Lit where something nearer than the point was not drawn. A texel nothing was drawn into holds 1, above any
    // threshold here (d < 1); 0 (a caster exactly at the light) counts as no data, as in PointShadows.fx. A blocker
    // near the lamp shadows less (PointShadowNear).
    float lit;
    [branch] if (NVR_PointShadowParams.w > 0.0f) {
        // Four taps on a rotated 2x2 grid: these four offsets give one on each diagonal of whichever face the
        // direction falls on. A face texel spans 2 * major / resolution in direction units.
        float major = max(abs(dir.x), max(abs(dir.y), abs(dir.z)));
        float s = NVR_PointShadowParams.w * major;
#if POINT_SHADOW_MAG
        // UNOFFICIAL 2026-10-21 MagnifiedSoftness (c188, PointShadowForward::Publish; z 0 = off, s exactly as before): a cube texel is
        // (2 / resolution) x the lamp's distance wide, a screen pixel (its angle) x the camera's distance, so far from its lamp and
        // close to the camera one texel covers many pixels and a narrow edge shows its stair-steps. There the spread is at least
        // MagnifiedSoftness texels, ramping in between 2 and 8 pixels per texel; the blocker search and LampSize take it as their
        // narrowest spread.
        s = max(s, saturate(len * rsqrt(dot(worldPos, worldPos)) * NVR_PointShadowFocus.x + NVR_PointShadowFocus.y) * NVR_PointShadowFocus.z * major);
#endif
        // UNOFFICIAL contact hardening (PCSS-style): find the blockers' mean distance over the full spread first, then
        // narrow the spread by how far the blocker is from this point compared with the lamp: sharp where an object
        // touches the floor, the full Softness spread further out. Nothing blocked in the search: lit, the PCF skipped.
        // UNOFFICIAL 2026-10-20 LampSize (z > 0): the spread a lamp of that radius gives instead, seen from the lamp
        // LampSize x (1 / blocker distance - 1 / this point's distance), between the Softness spread and MaxSoftness's (w),
        // searched over the widest: sharp where an object touches what it shadows, softer the further the shadow falls
        // from it, and blurred out where the blocker is right at the lamp (its fixture, a fan's blades above its globes).
        // Both in one form, t = 1 / blocker - 1 / d (so (d - blocker) / blocker = d t): the spread is
        // clamp(t (z / radius x major + x s d), y s, max(s, w major)), with z = w = 0 for contact hardening alone and
        // x = 0, y = 1 for LampSize (PointShadowForward::Publish), to keep the 6-lamp shaders' instruction count down.
        // UNOFFICIAL 2026-10-20 InvisibleLightSize: an invisible light (negative fade) takes its own size, c181.z.
#if POINT_SHADOW_FIT < 3
        [branch] if (NVR_PointShadowSoft.x + NVR_PointShadowSoft.w > 0.0f) {
            float wide = max(NVR_PointShadowSoft.w * major, s);
            float4 search = float4(
                PS_TAP(cube, dir + wide * float3( 1.0f,  1.0f,  1.0f)),
                PS_TAP(cube, dir + wide * float3(-1.0f, -1.0f,  1.0f)),
                PS_TAP(cube, dir + wide * float3( 1.0f, -1.0f, -1.0f)),
                PS_TAP(cube, dir + wide * float3(-1.0f,  1.0f, -1.0f)));
            float centre = PS_TAP(cube, dir);
            float4 isBlocker = (float4)(search <= threshold) * (float4)(search > 0.0f);
            float centreBlocker = (float)(centre <= threshold) * (float)(centre > 0.0f);
            float blockers = dot(isBlocker, 1.0f) + centreBlocker;
            [branch] if (!(blockers > 0.0f)) return POINT_SHADOW_EARLY_LIT;
            float blocker = (dot(search * isBlocker, 1.0f) + centre * centreBlocker) / blockers;
            float t = 1.0f / max(blocker, 0.0001f) - 1.0f / d;
            float size = fade < 0.0f ? NVR_PointShadowDebug.z : NVR_PointShadowSoft.z;
            s = clamp(t * (size * light.w * major + NVR_PointShadowSoft.x * s * d), NVR_PointShadowSoft.y * s, wide);
        }
#endif
#if POINT_SHADOW_TEXEL
        // UNOFFICIAL 2026-10-21 TexelFilter: the sun-style filter at this spread (PointShadowTexel) instead of Quality's taps.
        lit = 1.0f - PointShadowTexel(cube, dir, major, s, threshold, light.w);
#else
        float4 stored = float4(
            PS_TAP(cube, dir + s * float3( 1.0f,  1.0f,  1.0f)),
            PS_TAP(cube, dir + s * float3(-1.0f, -1.0f,  1.0f)),
            PS_TAP(cube, dir + s * float3( 1.0f, -1.0f, -1.0f)),
            PS_TAP(cube, dir + s * float3(-1.0f,  1.0f, -1.0f)));
        // UNOFFICIAL 2026-10-15 SmoothEdges (POINT_SHADOW_SMOOTH): the cube map is read with linear filtering, so across a
        // shadow's edge the stored distance slides from the blocker's to the lit surface's over one texel. A plain compare
        // turns that into a hard step that follows the texel grid (blocky edges, worst where contact hardening narrows the
        // taps). Instead each tap is blocked by how far its distance is below the threshold, against the nearest blocker
        // among the four taps: a tap on the blocker counts fully, one on the edge partly, so the edge fades over a texel
        // as with bilinear PCF (the four taps; High and Ultra's extra rings stay plain compares, to keep the 4-lamp shaders
        // as small as possible). Off: the plain compare, exactly as before.
#if POINT_SHADOW_SMOOTH && POINT_SHADOW_MAG
        float invRamp = 1.0f / max(threshold - min(min(stored.x, stored.y), min(stored.z, stored.w)), 0.0001f);
        float4 blocked = saturate((threshold - stored) * invRamp) * PointShadowKeep(stored, nearK, light.w);
#elif POINT_SHADOW_SMOOTH
        float invRamp = 1.0f / max(threshold - min(min(stored.x, stored.y), min(stored.z, stored.w)), 0.0001f);
        float4 blocked = saturate((threshold - stored) * invRamp) * (float4)(stored > 0.0f) * PointShadowNear(stored, light.w);
#elif POINT_SHADOW_MAG
        float4 blocked = (float4)(stored <= threshold) * PointShadowKeep(stored, nearK, light.w);
#else
        float4 blocked = (float4)(stored <= threshold) * (float4)(stored > 0.0f) * PointShadowNear(stored, light.w);
#endif
#if POINT_SHADOW_PCF
        // UNOFFICIAL 2026-11-01 TruePCF (above PointShadowAtlasBlocked): the same four taps, each a bilinear PCF of its four texels.
        [branch] if (NVR_PointShadowFocus.w > 0.0f) {
#if POINT_SHADOW_MAG || POINT_SHADOW_EVEN
            const float pcfNearK = nearK;
#else
            const float pcfNearK = 0.0f;
#endif
            blocked = float4(
                PointShadowAtlasBlocked(cube, dir + s * float3( 1.0f,  1.0f,  1.0f), threshold, pcfNearK, light.w),
                PointShadowAtlasBlocked(cube, dir + s * float3(-1.0f, -1.0f,  1.0f), threshold, pcfNearK, light.w),
                PointShadowAtlasBlocked(cube, dir + s * float3( 1.0f, -1.0f, -1.0f), threshold, pcfNearK, light.w),
                PointShadowAtlasBlocked(cube, dir + s * float3(-1.0f,  1.0f, -1.0f), threshold, pcfNearK, light.w));
        }
#endif
#if POINT_SHADOW_FIT < 2
        [branch] if (NVR_PointShadowDebug.w > 1.5f) {
            // UNOFFICIAL Quality 2 (High): six more taps, one along each axis: the four across the face fill the 3x3 grid's
            // edges, the two along the face's own axis land on the centre (it counts twice). Quality 3 (Ultra): the same ten
            // again at half the spread, for a smoother edge over the same width.
#if POINT_SHADOW_FIT == 1
            // (POINT_SHADOW_FIT 1: one tap a turn from PointShadowTaps; the sum in another order, the same within rounding.)
#if POINT_SHADOW_EVEN
            // EvenTaps: High's four even taps, Ultra's twelve (PointShadowEven).
            const int count = NVR_PointShadowDebug.w > 2.5f ? 12 : 4;
            float extra = 0.0f;
            [loop] for (int i = 0; i < 12; i++) {
                [branch] if (i >= count) break;
                const float4 tap = PointShadowEven[i];
#else
            const int count = NVR_PointShadowDebug.w > 2.5f ? 16 : 6;
            float extra = 0.0f;
            [loop] for (int i = 0; i < 16; i++) {
                [branch] if (i >= count) break;
                const float4 tap = PointShadowTaps[i];
#endif
                const float v = PS_TAP(cube, dir + (s * tap.w) * tap.xyz);
#if POINT_SHADOW_MAG || POINT_SHADOW_EVEN
                extra += (float)(v <= threshold) * PointShadowKeep(v.xxxx, nearK, light.w).x;
#else
                extra += (float)(v <= threshold) * (float)(v > 0.0f) * PointShadowNear(v.xxxx, light.w).x;
#endif
            }
            lit = 1.0f - (dot(blocked, 1.0f) + extra) / (4.0f + (float)count);
#elif POINT_SHADOW_EVEN
            // EvenTaps: High = the four Medium taps and PointShadowEven's first four, Ultra all twelve (equal weights).
            float sum = dot(blocked, 1.0f) + PointShadowEven4(cube, dir, s, threshold, nearK, light.w, 0);
            float taps = 8.0f;
            [branch] if (NVR_PointShadowDebug.w > 2.5f) {
                sum += PointShadowEven4(cube, dir, s, threshold, nearK, light.w, 4) + PointShadowEven4(cube, dir, s, threshold, nearK, light.w, 8);
                taps = 16.0f;
            }
            lit = 1.0f - sum / taps;
#elif POINT_SHADOW_MAG
            float sum = dot(blocked, 1.0f) + PointShadowRingK(cube, dir, s, threshold, nearK);
            float taps = 10.0f;
            [branch] if (NVR_PointShadowDebug.w > 2.5f) {
                float h = 0.5f * s;
                float4 inner = float4(
                    PS_TAP(cube, dir + h * float3( 1.0f,  1.0f,  1.0f)),
                    PS_TAP(cube, dir + h * float3(-1.0f, -1.0f,  1.0f)),
                    PS_TAP(cube, dir + h * float3( 1.0f, -1.0f, -1.0f)),
                    PS_TAP(cube, dir + h * float3(-1.0f,  1.0f, -1.0f)));
                sum += dot((float4)(inner <= threshold) * PointShadowKeep(inner, nearK, light.w), 1.0f) +
                       PointShadowRingK(cube, dir, h, threshold, nearK);
                taps = 20.0f;
            }
            lit = 1.0f - sum / taps;
#else
            float sum = dot(blocked, 1.0f) + PointShadowRing(cube, dir, s, threshold, light.w);
            float taps = 10.0f;
            [branch] if (NVR_PointShadowDebug.w > 2.5f) {
                float h = 0.5f * s;
                float4 inner = float4(
                    PS_TAP(cube, dir + h * float3( 1.0f,  1.0f,  1.0f)),
                    PS_TAP(cube, dir + h * float3(-1.0f, -1.0f,  1.0f)),
                    PS_TAP(cube, dir + h * float3( 1.0f, -1.0f, -1.0f)),
                    PS_TAP(cube, dir + h * float3(-1.0f,  1.0f, -1.0f)));
                sum += dot((float4)(inner <= threshold) * (float4)(inner > 0.0f) * PointShadowNear(inner, light.w), 1.0f) +
                       PointShadowRing(cube, dir, h, threshold, light.w);
                taps = 20.0f;
            }
            lit = 1.0f - sum / taps;
#endif
        }
        else lit = 1.0f - dot(blocked, 0.25f);
#else
        lit = 1.0f - dot(blocked, 0.25f);
#endif
#endif
    }
    else {
        float stored = PS_TAP(cube, dir);
#if POINT_SHADOW_MAG
        lit = 1.0f - (float)(stored <= threshold) * PointShadowKeep(stored.xxxx, nearK, light.w).x;
#else
        lit = 1.0f - (float)(stored <= threshold) * (float)(stored > 0.0f) * PointShadowNear(stored, light.w).x;
#endif
#if POINT_SHADOW_PCF
        // UNOFFICIAL 2026-11-01 TruePCF: one bilinear PCF tap (Quality Low / no spread)
#if POINT_SHADOW_MAG || POINT_SHADOW_EVEN
        [branch] if (NVR_PointShadowFocus.w > 0.0f) lit = 1.0f - PointShadowAtlasBlocked(cube, dir, threshold, nearK, light.w);
#else
        [branch] if (NVR_PointShadowFocus.w > 0.0f) lit = 1.0f - PointShadowAtlasBlocked(cube, dir, threshold, 0.0f, light.w);
#endif
#endif
    }
    return POINT_SHADOW_DONE(lit);
#endif
}

// What light k's colour is multiplied by: its shadow, or in the debug views (NVR_PointShadowDebug.x, the same for the
// whole draw, so one cheap branch) 1: blocked light drawn in the lamp's colour instead of removed, 2: the lamp's lit
// area tinted in its colour. Lamps without a shadow have white, so the views leave them as they are.
float3 PointShadowFactor(float shadow, float4 tint) {
#if POINT_SHADOW_RT
    // RT compare view (PointShadowDone): white where both are lit, black where both are blocked, RED where only the traced result
    // has a blocker, GREEN where only the cube map does (light the rays let through: a leak, or a cube-map artifact the rays fix).
    [branch] if (shadow > 1.5f) {
        float e = shadow - 2.0f;
        float cubeSteps = floor(e / 16.0f);
        float ray = (e - 16.0f * cubeSteps) / 15.0f;
        float cubeLit = cubeSteps / 15.0f;
        float both = min(ray, cubeLit);
        return float3(both + max(cubeLit - ray, 0.0f), both + max(ray - cubeLit, 0.0f), both);
    }
#endif
    float3 factor = shadow;
#if POINT_SHADOW_ATLAS
    tint.rgb = float3(1.0f, 0.3f, 0.2f);   // (the tint holds the light's atlas tile here)
#endif
    [branch] if (NVR_PointShadowDebug.x > 0.0f) {
        factor = NVR_PointShadowDebug.x < 1.5f ? shadow + (1.0f - shadow) * tint.rgb
                                               : shadow * lerp(1.0f, tint.rgb, 0.6f);
    }
    return factor;
}

// UNOFFICIAL DebugView 3 (diagnostic, 2026-10-08): what the cube map of the lamp in cube slot 0 (the only red lamp in views 1
// and 2) says at this pixel, looked up from the pixel's own position, whatever lamps the game gave the object. Forward
// shadows stopped dead where one floor piece met the next while the old screen-space pass (which works from the depth
// buffer, per pixel) did not; this tells the cube map and the position apart from the per-object lamp setup.
// False colour: green = what the cube map holds in this pixel's direction, blue = the pixel's own distance (both as a share
// of the cube's reach, x 2.5), red = blocked. A surface the cube map saw itself is cyan (green = blue), an empty cube map
// bright green everywhere. A 64-unit checker anchored on the lamp (must run on across object edges); the whole object at
// 40% when the lamp is not one of the draw's lights. Dark blue: beyond the cube's reach. Orange: no lamp data reached the
// draw. Grey: the vertex shader gave no position.
float3 PointShadowTrace(float3 worldPos, float3 normal, float valid) {
#if POINT_SHADOW_RT
    return 0.5f;    // (RT builds: the focus lamp's cube map has no sampler, see NVR_RtLampVis)
#else
    float4 light = NVR_PointShadowFocus;
    [branch] if (!(valid > 0.0f)) return 0.5f;
    [branch] if (!(light.w > 0.0f)) return float3(1.0f, 0.45f, 0.0f);
    float3 toPixel = worldPos - light.xyz;
    float3 p = toPixel + normal * (NVR_PointShadowParams.z * length(toPixel));
    float d = length(p) * light.w;
    float3 color = float3(0.0f, 0.0f, 0.15f);
    [branch] if (d < 1.0f) {
        float stored = PS_TAP(NVR_PointShadowCube5, p * float3(1.0f, 1.0f, -1.0f));   // (atlas: the focus lamp's tile in light 5's tint)
        float blocked = (float)(stored > 0.0f) * (float)(stored <= d - NVR_PointShadowParams.y * d);
        float3 cell = floor(toPixel / 64.0f);
        float checker = frac((cell.x + cell.y + cell.z) * 0.5f) > 0.25f ? 1.0f : 0.7f;
        color = float3(blocked, saturate(stored * 2.5f), saturate(d * 2.5f)) * checker;
    }
    return NVR_PointShadowDebug.z < 0.0f ? color * 0.4f : color;
#endif
}

// One function per light index of the draw: ps_3_0 cannot index samplers at run time.
float PointShadow0(float3 worldPos, float3 normal, float att) { return PointShadowCube(NVR_PointShadowCube0, NVR_PointShadowLight[0], NVR_PointShadowTint[0].w, worldPos, normal, att POINT_SHADOW_RT_ARG(NVR_PointShadowRTSlot[0].x)); }
float PointShadow1(float3 worldPos, float3 normal, float att) { return PointShadowCube(NVR_PointShadowCube1, NVR_PointShadowLight[1], NVR_PointShadowTint[1].w, worldPos, normal, att POINT_SHADOW_RT_ARG(NVR_PointShadowRTSlot[0].y)); }
float PointShadow2(float3 worldPos, float3 normal, float att) { return PointShadowCube(NVR_PointShadowCube2, NVR_PointShadowLight[2], NVR_PointShadowTint[2].w, worldPos, normal, att POINT_SHADOW_RT_ARG(NVR_PointShadowRTSlot[0].z)); }
float PointShadow3(float3 worldPos, float3 normal, float att) { return PointShadowCube(NVR_PointShadowCube3, NVR_PointShadowLight[3], NVR_PointShadowTint[3].w, worldPos, normal, att POINT_SHADOW_RT_ARG(NVR_PointShadowRTSlot[0].w)); }
float PointShadow4(float3 worldPos, float3 normal, float att) { return PointShadowCube(NVR_PointShadowCube4, NVR_PointShadowLight[4], NVR_PointShadowTint[4].w, worldPos, normal, att POINT_SHADOW_RT_ARG(NVR_PointShadowRTSlot[1].x)); }
#if POINT_SHADOW_RT
// RT builds: the sixth light has no cube map (NVR_RtLampVis took s15): the traced result where there is one, else lit.
float PointShadow5(float3 worldPos, float3 normal, float att) {
    float4 light = NVR_PointShadowLight[5];
    [branch] if (!(light.w > 0.0f) || !(att > 0.0f) || !(NVR_PointShadowRT.x > 0.0f) || NVR_PointShadowRTSlot[1].y > -1.5f && NVR_PointShadowRTSlot[1].y < 0.0f) return 1.0f;
    [branch] if (!(length(worldPos - light.xyz) * light.w < 1.0f)) return 1.0f;
    [branch] if (NVR_PointShadowRTSlot[1].y < -1.5f) return lerp(1.0f, saturate(-2.0f - NVR_PointShadowRTSlot[1].y), NVR_PointShadowParams.x * abs(NVR_PointShadowTint[5].w));   // (2026-11-07) the eye's
    float traced = PointShadowTraced(NVR_PointShadowRTSlot[1].y, worldPos);
    return traced < 0.0f ? 1.0f : lerp(1.0f, traced, NVR_PointShadowParams.x * abs(NVR_PointShadowTint[5].w));
}
#else
float PointShadow5(float3 worldPos, float3 normal, float att) { return PointShadowCube(NVR_PointShadowCube5, NVR_PointShadowLight[5], NVR_PointShadowTint[5].w, worldPos, normal, att POINT_SHADOW_RT_ARG(NVR_PointShadowRTSlot[1].y)); }
#endif

// The object shader looks every light's shadow up once, inside one branch on NVR_PointShadowParams.x (0 outdoors and
// whenever the feature is idle, so a draw without shadows pays for that one skipped branch), into float3 ptShadow0..5
// (float3 for the debug views; the plain shadow in all three channels otherwise):
//   POINT_SHADOW_LOOKUP(k, att): the lookup; ptWorldPos, ptNormal and ptValid (1 when an NVR vertex shader wrote the
//                                world position, 0 under a vanilla one) must be in scope -- see ObjectTemplate.hlsl.
//   POINT_SHADOW(k, att), SHADOWED(colour, k, att): light k's shadow, light k's colour with it (att is only there to
//                                keep the call sites readable; the lookup already used it).
//   POINT_LAMP_EXTRA(k): UNOFFICIAL 2026-11-04 one more per-pixel factor on light k (defined at the top of this file).
#ifdef POINT_SHADOW_SCALAR
// UNOFFICIAL 2026-10-14 (ObjectTemplate.hlsl): float ptShadow0..5, the debug tint applied at each use (same result, fewer
// temp registers held across the lighting).
#define POINT_SHADOW_LOOKUP(k, att) PointShadow##k(ptWorldPos, ptNormal, (att) * ptValid)
#define POINT_SHADOW(k, att) (PointShadowFactor(ptShadow##k, NVR_PointShadowTint[k]) * POINT_LAMP_EXTRA(k))
#define SHADOWED(color, k, att) ((color) * POINT_SHADOW(k, att))
#else
#define POINT_SHADOW_LOOKUP(k, att) PointShadowFactor(PointShadow##k(ptWorldPos, ptNormal, (att) * ptValid), NVR_PointShadowTint[k])
#define POINT_SHADOW(k, att) (ptShadow##k * POINT_LAMP_EXTRA(k))
#define SHADOWED(color, k, att) ((color) * POINT_SHADOW(k, att))
#endif

#else

#define POINT_SHADOW(k, att) POINT_LAMP_EXTRA(k)
#define SHADOWED(color, k, att) ((color) * POINT_LAMP_EXTRA(k))

#endif
