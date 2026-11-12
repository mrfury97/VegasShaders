// Wetness Effects, ported from Community Shaders (features/Wetness Effects, GPL-3.0): surfaces getting
// wet in the rain and drying after it, as a water layer on the material.
//
//   - Rain wetness: open surfaces darken (porous ones more, metals not) and gain a water film, a
//     glossy layer over the material with its own highlight and reflection. Upward faces get
//     fully wet; walls and undersides MinRainWetness of it.
//   - Puddles: perlin noise over flat ground, filling as the rain goes on and lingering after it.
//     A puddle is a near-mirror film that flattens the surface's bumps.
//   - Raindrops (open, upward land near the camera): ripples, rings spreading on flat wet ground and
//     puddles. Only where the including template defines WETNESS_RAINDROPS (the terrain); objects
//     get none.
//   - Occlusion: nothing under a roof, a bridge or an overhang gets rained on (NVR's top-down ortho
//     depth map, as CS's precipitation occlusion map).
//
// The weather side (how wet, how puddled, how hard it rains) is in effects/WetnessEffects.cpp.
//
// Positions are camera-relative (GetShadowWorldPos), normals world-space. The including template
// defines WETNESS_ORTHO_REG (a free sampler) for occlusion, or leaves it undefined to go without.
//
// UNOFFICIAL 2026-11-04 VegasShaders port (from Physically-Based-Reloaded, Balebandro; in place of WetWorld's screen pass,
// [Shaders.WetnessEffects.Status] Enabled, restart: WETNESS_EFFECTS from ShaderRecord::BuildDefines). PBR NVR pins these on c200-c214,
// which VegasShaders' object shaders give to the RT lamps, CharacterObjectReach, LampFalloff, the character shadow map, ExactPassSums,
// SpecularFade and MaterialAware. Here c146-c159: the sun cross-fade's (Shadow.hlsl, SUN_CROSSFADE) in the object shaders, the land's
// RT lamp slots (TerrainTemplate.hlsl TERRAIN_LAMP_RT) in the land's, so the includers compile this in only without those
// (WETNESS_AVAILABLE). The sky reflection is SkyAmbient.hlsl's SkyAmbientRadiance (both SkylightingModes have it).

#ifndef WETNESS_HLSL
#define WETNESS_HLSL

// 2026-11-12 Puddles map mode (WETNESS_EFFECTS 3; src/effects/Puddles/PuddleMap.h, Compute/PuddleMap.comp): the large-scale terms (the
// field's noise, the rim wander, the rain cover as the top surface's height) come from TESR_PuddleMap, on the sampler the ortho map
// used, with its two constants on the ortho matrix's registers. Not for the D3D9 post pass (PUDDLES_RUNTIME).
// (PUDDLES_NOMAP: one record opts out, ShaderManager's lean retry: the in-shader path for it; SLS2027 with VanillaBridge + MicroShadow)
#if defined(WETNESS_EFFECTS) && !defined(PUDDLES_RUNTIME) && !defined(PUDDLES_NOMAP)
    #if WETNESS_EFFECTS >= 3
        #define PUDDLES_MAP
    #endif
#endif

// 2026-11-09 the registers, by name. WETNESS_ALT_REGS: the land's outdoor RT lamp-shadow record (TerrainTemplate.hlsl TERRAIN_LAMP_RT),
// whose lamp slots hold c146-c173: the water layer and the puddles move to c2, c4-c17, c98-c99 there (in no land table and written raw by
// nothing in the DLL: check src for SetPixelShaderConstantF before moving any of them). The constants are set by name, so each record may
// place them differently. Elsewhere the registers are the ones they always were (the compiled shaders unchanged); an includer may still
// move single ones by defining the macro first (ObjectTemplate / ParallaxTemplate: the objects' outdoor RT lamp-shadow record, whose
// lamp binding writes c174-c189 raw on every draw, puts PR_DATA / PR_LOOK on c160 / c161).
#ifdef WETNESS_ALT_REGS
    // (2026-11-09 second try: c192-c204 / c212-c215 are free in the land's tables, but the DLL writes c194, c197-c199 (material rules),
    // c200-c204, c206-c209, c212-c217 raw on every draw, which zeroed the flags. c2, c4-c17 and c98-c99 are in no land table and no raw write.)
    #define WR_DATA c4
    #define WR_PARAMS c5
    #define WR_PARAMS2 c6
    #define WR_DROPS c7
    #define WR_RIPPLE c8
    #define WR_CAMERA c9
    #define WR_FLAGS c10
    #define WR_ORTHO c11
    #define WR_MAP2 c12     // (map mode: TESR_PuddleMapFar, on the ortho matrix's second register)
    #define WR_SUN c15
    #define WR_HEIGHT c16
    #define PR_DATA c17
    #define PR_LOOK c2
    #define PR_SHAPE c98
    #define PR_STREAK c99
#else
    #ifndef WR_DATA
        #define WR_DATA c146
    #endif
    #ifndef WR_PARAMS
        #define WR_PARAMS c147
    #endif
    #ifndef WR_PARAMS2
        #define WR_PARAMS2 c148
    #endif
    #ifndef WR_DROPS
        #define WR_DROPS c149
    #endif
    #ifndef WR_RIPPLE
        #define WR_RIPPLE c150
    #endif
    #ifndef WR_CAMERA
        #define WR_CAMERA c151
    #endif
    #ifndef WR_FLAGS
        #define WR_FLAGS c152
    #endif
    #ifndef WR_ORTHO
        #define WR_ORTHO c153
    #endif
    #ifndef WR_MAP2
        #define WR_MAP2 c154
    #endif
    #ifndef WR_SUN
        #define WR_SUN c157
    #endif
    #ifndef WR_HEIGHT
        #define WR_HEIGHT c158
    #endif
    #ifndef PR_DATA
        #define PR_DATA c174
    #endif
    #ifndef PR_LOOK
        #define PR_LOOK c175
    #endif
    #ifndef PR_SHAPE
        #define PR_SHAPE c165
    #endif
    #ifndef PR_STREAK
        #define PR_STREAK c166
    #endif
#endif

float4 TESR_WetnessData    : register(WR_DATA);   // x: wetness, y: puddle wetness, z: raining, w: time (s)
float4 TESR_WetnessParams  : register(WR_PARAMS);   // x: MaxRainWetness, y: MaxPuddleWetness, z: MinRainWetness, w: PuddleRadius
float4 TESR_WetnessParams2 : register(WR_PARAMS2);   // x: PuddleMaxAngle, y: PuddleMinWetness, zw: unused
float4 TESR_WetnessDrops   : register(WR_DROPS);   // x: 1 / grid size, y: 1 / interval, z: drop chance, w: interval / ripple lifetime
float4 TESR_WetnessRipple  : register(WR_RIPPLE);   // x: ripple strength, y: radius, z: breadth, w: raindrop range
float4 TESR_WetnessCamera  : register(WR_CAMERA);   // xyz: camera position, w: unused
float4 TESR_WetnessFlags   : register(WR_FLAGS);   // x: on, z: ripples, w: ortho map texel size (0: no map)
#ifdef PUDDLES_MAP
float4 TESR_PuddleMapNear  : register(WR_ORTHO);   // xy near cascade corner (world), z 1 / its extent, w the camera z the heights are relative to
float4 TESR_PuddleMapFar   : register(WR_MAP2);    // xyz the far cascade's, w 1 = the map holds data
#else
row_major float4x4 TESR_ShadowCameraToLightTransformOrtho : register(WR_ORTHO);   // c153-c156
#endif
float4 TESR_WetnessSun     : register(WR_SUN);   // xyz: direction to the sun (world), w: ShowCover debug view
#ifdef WETNESS_HEIGHT
float4 TESR_WetnessHeight  : register(WR_HEIGHT);   // x: HeightAwareness, y: CreviceFill (the water line at full wetness), z: NormalAwareness
#endif

#ifdef WETNESS_ORTHO_PER_DRAW
// Set per draw by the hook that binds the map (Hooks/Shaders.cpp, LandRainCover), not through the constant
// table: x is 1 while the ortho map is on the sampler, 0 when the sampler holds something else.
float4 WetnessMapBound     : register(c159);
#define WETNESS_MAP_READY (WetnessMapBound.x > 0.5f)
#else
#define WETNESS_MAP_READY true
#endif

#ifdef WETNESS_ORTHO_REG
#ifdef PUDDLES_MAP
// LINEAR, not POINT ("POINT" is an empty macro in the point-light passes)
sampler2D TESR_PuddleMap : register(WETNESS_ORTHO_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// The map at a world xy: r the field's large-scale part, g the top surface's height (relative to TESR_PuddleMapNear.w), b the rim wander.
// ONE read (2026-11-12: SLS2027 with VanillaBridge + MicroShadow ran out of temps with a near/far blend and two reads a pixel): the near
// cascade where the point is inside it, else the far one (both hold the same world noise; the cover differs only in its averaging).
// No map, or past the far cascade: no field, open sky. GetWetness reads it once (WetnessOcclusion) and keeps it in PuddleMapSample.
static float4 PuddleMapSample = float4(0.0f, -100000.0f, 0.5f, 0.0f);
float4 PuddleMapRead(float2 world) {
    const float edge = 0.5f / 1024.0f;
    float2 un = (world - TESR_PuddleMapNear.xy) * TESR_PuddleMapNear.z;
    float2 uf = (world - TESR_PuddleMapFar.xy) * TESR_PuddleMapFar.z;
    bool inNear = all(abs(un - 0.5f) < 0.5f - edge);
    float2 u = clamp(inNear ? un : uf, edge, 1.0f - edge);
    float4 m = tex2Dlod(TESR_PuddleMap, float4(u.x * 0.5f + (inNear ? 0.0f : 0.5f), u.y, 0.0f, 0.0f));
    bool none = TESR_PuddleMapFar.w < 0.5f || any(abs(uf - 0.5f) >= 0.5f - edge);
    return none ? float4(0.0f, -100000.0f, 0.5f, 0.0f) : m;
}
#else
sampler2D TESR_OrthoMapBuffer : register(WETNESS_ORTHO_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#endif
#endif

#define WETNESS_ON (TESR_WetnessFlags.x > 0.5f)

// 2026-11-08 Puddles (src/effects/Puddles/README.md, [Shaders.Puddles]): a water level that fills in the rain and dries from the edges,
// a world-space puddle field (noise + the land's height crevices + slope + the rain cover), a flat water layer with ripples, a soaked ring
// and wall streaks. Compiled into the surface shaders with WETNESS_EFFECTS 2 ([Shaders.Puddles.Status] Enabled at startup); the D3D9
// wetness reflection effect defines PUDDLES_RUNTIME. c165-c166 are the sun cross-fade's (never with Wetness Effects), c174-c175 free in the
// object and land pixel shaders (VEGAS_SHADERS.md section 3).
#if defined(PUDDLES_RUNTIME)
    #define PUDDLES_COMPILED
#elif defined(WETNESS_EFFECTS)
    #if WETNESS_EFFECTS >= 2
        #define PUDDLES_COMPILED
    #endif
#endif
#ifdef PUDDLES_COMPILED
float4 TESR_PuddleData   : register(PR_DATA);   // x water level 0-1, y Coverage, z 1 / Size, w Quality (0 off: the CS puddles)
float4 TESR_PuddleLook   : register(PR_LOOK);   // x roughness, y depth darkening, z edge softness (field units), w ripples in puddles
float4 TESR_PuddleShape  : register(PR_SHAPE);   // x slope start (up), y slope full, z land cavity strength, w wet ring width
float4 TESR_PuddleStreak : register(PR_STREAK);   // x streak strength x raining, y 1 / lane width, z speed, w debug view
#define PUDDLES_ON (TESR_PuddleData.w > 0.5f)
#define PUDDLE_QUALITY (TESR_PuddleData.w)
#define PUDDLE_DEBUG (TESR_PuddleStreak.w)
#define PUDDLES_NOT_ACTIVE_AND !PUDDLES_ON &&   // (empty without puddles: the land shaders stay byte-identical)
static float PuddleStandingOut = 0.0f;   // 2026-11-12 the standing water GetWetness found here, for the material buffer (Reflections' mask)
static float3 PuddleDebugColor = float3(-1.0f, -1.0f, -1.0f);   // >= 0: the debug view's colour for this pixel (the templates output it)
    // Objects get the rain rings too at Quality 3 (the land always has them, WETNESS_RAINDROPS); not the post-pass effect.
    #if !defined(PUDDLES_RUNTIME) && !defined(WETNESS_RAINDROPS)
        #define WETNESS_RAINDROPS
        #define PUDDLE_OBJECT_RIPPLES
    #endif
#endif

// "Hash without sine" (Dave Hoskins, MIT): stable for large inputs, unlike frac(sin()). CS hashes
// integers (pcg3d, murmur3); ps_3_0 has none.
float3 WetHash33(float3 p3) {
    p3 = frac(p3 * float3(0.1031f, 0.1030f, 0.0973f));
    p3 += dot(p3, p3.yxz + 33.33f);
    return frac((p3.xxy + p3.yxx) * p3.zyx);
}

// Perlin noise, as CS's Random::perlinNoise (gradients picked by hash, the wetness-specific fade).
float3 WetGradient(float3 cell) {
    float3 h = WetHash33(cell) * 2.0f - 1.0f;
    return normalize(h + 1e-4f);
}

float WetPerlin(float3 p) {
    float3 i = floor(p);
    float3 f = p - i;
    float v1 = dot(WetGradient(i), f);
    float v2 = dot(WetGradient(i + float3(1, 0, 0)), f - float3(1, 0, 0));
    float v3 = dot(WetGradient(i + float3(0, 1, 0)), f - float3(0, 1, 0));
    float v4 = dot(WetGradient(i + float3(1, 1, 0)), f - float3(1, 1, 0));
    float v5 = dot(WetGradient(i + float3(0, 0, 1)), f - float3(0, 0, 1));
    float v6 = dot(WetGradient(i + float3(1, 0, 1)), f - float3(1, 0, 1));
    float v7 = dot(WetGradient(i + float3(0, 1, 1)), f - float3(0, 1, 1));
    float v8 = dot(WetGradient(i + float3(1, 1, 1)), f - float3(1, 1, 1));
    float3 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(lerp(v1, v2, u.x), lerp(v3, v4, f.x), u.y),
                lerp(lerp(v5, v6, u.x), lerp(v7, v8, f.x), u.y), u.z);
}

// Reoriented normal mapping: detail n2 laid onto base n1 (CS's ReorientNormal).
float3 WetReorient(float3 n1, float3 n2) {
    n1 += float3(0.0f, 0.0f, 1.0f);
    n2 *= float3(-1.0f, -1.0f, 1.0f);
    return normalize(n1 * dot(n1, n2) / n1.z - n2);
}

float WetSmoothstepDeriv(float x) { return 6.0f * x * (1.0f - x); }

// 1 where the camera-relative point is open to the sky, 0 under something.
//
// The ortho map is coarse (about 12 game units a texel at Resolution 2), so a hard test draws the edge
// of a roof's dry patch as a staircase of texels. Here the edge is smooth and irregular instead:
//   - a tent filter, a 3x3 texel box blended by the point's place inside its texel (4x4 taps with
//     weights 1-f, 1, 1, f per axis): the fade runs smoothly over about three texels, no steps;
//   - the lookup nudged by world-space noise of up to a texel, so the line wanders with the ground
//     instead of following the map's rows and columns, as a real drip line does.
float WetnessOcclusion(float3 position, float3 normal) {
#if defined(WETNESS_ORTHO_REG) && defined(PUDDLES_MAP)
    // map mode: the top surface's height over the look-out point (a steep face looks up from up to 40 units out, as below), against the
    // point's own height: above it open, below it covered, a 16-unit fade (the map's averaging softens it sideways)
    float lift = lerp(2.0f, 40.0f, saturate(1.0f - abs(normal.z) * 1.5f));
    float3 p = TESR_WetnessCamera.xyz + position + normal * lift + float3(0.0f, 0.0f, 2.0f);
    PuddleMapSample = PuddleMapRead(p.xy);   // (the field and the wander take theirs from here too: one read a pixel)
    return saturate((p.z - (PuddleMapSample.g + TESR_PuddleMapNear.w)) / 16.0f + 0.5f);
#elif defined(WETNESS_ORTHO_REG)
    float texel = TESR_WetnessFlags.w;
    [branch] if (texel <= 0.0f || !WETNESS_MAP_READY) return 1.0f;
    // Lifted a little along the normal and up (CS jitters by the normal too), so a surface does not
    // occlude itself.
#ifdef PUDDLES_COMPILED
    // 2026-11-09 Puddles: a steep face looks up from up to 40 units out from it, so the roof edge right above a wall is not counted as cover
    // (it was: no wall in a town got streaks); only an overhang deeper than that keeps the wall dry.
    // 2026-11-11 (user): a wall under an overhang stays dry, so 40 stays: the puddle2 shack walls read covered (DebugView 4; their eave plus
    // this lookup's own spread, the tent filter and the wander, about +-37 units, reach past it; open at 120) and get no streaks, by choice.
    float lift = PUDDLES_ON ? lerp(2.0f, 40.0f, saturate(1.0f - abs(normal.z) * 1.5f)) : 2.0f;
    #define WET_COVER_LIFT lift
#else
    #define WET_COVER_LIFT 2.0f   // (a literal, not a const: the shaders without puddles stay byte-identical)
#endif
    float4 o = mul(float4(position + normal * WET_COVER_LIFT + float3(0.0f, 0.0f, 2.0f), 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    #undef WET_COVER_LIFT
    o.xyz /= o.w;
    if (any(abs(o.xy) > 1.0f) || o.z < 0.0f || o.z > 1.0f) return 1.0f;
    float2 uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);

    float3 worldPos = TESR_WetnessCamera.xyz + position;
    float2 wander = float2(WetPerlin(worldPos * 0.045f), WetPerlin(worldPos.yxz * 0.045f + 17.3f));
    uv += wander * texel * 1.6f;

    // Tent filter over the map's texels.
    float2 cell = uv / texel - 0.5f;
    float2 f = frac(cell);
    float2 base = (floor(cell) + 0.5f - 1.0f) * texel;   // centre of the first of the four taps per axis
    float4 wx = float4(1.0f - f.x, 1.0f, 1.0f, f.x);
    float4 wy = float4(1.0f - f.y, 1.0f, 1.0f, f.y);
    float depth = o.z - 0.0005f;
    float open = 0.0f;
    [unroll] for (int y = 0; y < 4; y++) {
        [unroll] for (int x = 0; x < 4; x++) {
            float2 tapUV = base + float2(x, y) * texel;
            float lit = tex2Dlod(TESR_OrthoMapBuffer, float4(tapUV, 0.0f, 0.0f)).r > depth ? 1.0f : 0.0f;
            open += lit * wx[x] * wy[y];
        }
    }
    return smoothstep(0.0f, 1.0f, open / 9.0f);
#else
    return 1.0f;
#endif
}

// [Shaders.WetnessEffects.Debug] ShowCover (TESR_WetnessSun.w): what the cover test sees. Green open to
// the sky, red covered, yellow outside the ortho map (counted open), blue no ortho map, dark blue a
// shader with no sampler for it (seven-texture land).
#define WETNESS_DEBUG_COVER (TESR_WetnessSun.w > 0.5f)
float3 WetnessCoverDebug(float3 position, float3 normal) {
#if defined(WETNESS_ORTHO_REG) && defined(PUDDLES_MAP)
    if (TESR_PuddleMapFar.w < 0.5f) return float3(0.0f, 0.0f, 1.0f);   // blue: the map holds nothing yet
    return lerp(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), WetnessOcclusion(position, normal));
#elif defined(WETNESS_ORTHO_REG)
    if (TESR_WetnessFlags.w <= 0.0f) return float3(0.0f, 0.0f, 1.0f);
    if (!WETNESS_MAP_READY) return float3(1.0f, 0.0f, 1.0f);   // magenta: the per-draw binding did not happen
    float4 o = mul(float4(position + normal * 2.0f + float3(0.0f, 0.0f, 2.0f), 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    o.xyz /= o.w;
    if (any(abs(o.xy) > 1.0f) || o.z < 0.0f || o.z > 1.0f) return float3(1.0f, 1.0f, 0.0f);
    return lerp(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), WetnessOcclusion(position, normal));
#else
    return float3(0.0f, 0.0f, 0.3f);
#endif
}

// CS's GetRainDrops, ripples only: the ripple normal (z up).
float3 WetRainDrops(float3 worldPos, float t, float3 normal, float rippleStrengthModifier) {
    float2 gridUV = worldPos.xy * TESR_WetnessDrops.x + normal.xy;
    float2 grid = floor(gridUV);
    gridUV -= grid;

    float3 rippleNormal = float3(0.0f, 0.0f, 1.0f);
    float chance = TESR_WetnessDrops.z;
    [branch] if (TESR_WetnessFlags.z <= 0.5f) return rippleNormal;

    [loop] for (int i = -1; i <= 1; i++) {
        [loop] for (int j = -1; j <= 1; j++) {
            float2 cell = grid + float2(i, j);
            float tOffset = WetHash33(float3(cell, 1.7f)).x;

            {
                float residual = t * TESR_WetnessDrops.y + tOffset + worldPos.z * 0.001f;
                float timestep = floor(residual);
                residual -= timestep;
                float3 h = WetHash33(float3(cell, timestep + 0.5f));
                float rippleT = residual * TESR_WetnessDrops.w;
                [branch] if (h.z < chance && rippleT < 1.0f) {
                    float2 toCentre = float2(i, j) + h.xy - gridUV;
                    float size = lerp(0.7f, 1.3f, WetHash33(h * 53.1f).y);
                    float rippleR = TESR_WetnessRipple.y * size * rippleT;
                    float inner = rippleR - TESR_WetnessRipple.z;
                    float band = (length(toCentre) - inner) / TESR_WetnessRipple.z;
                    [branch] if (band > 0.0f && band < 1.0f) {
                        float deriv = (band < 0.5f ? WetSmoothstepDeriv(band * 2.0f) : -WetSmoothstepDeriv(2.0f - band * 2.0f))
                                    * lerp(TESR_WetnessRipple.x * rippleStrengthModifier, 0.0f, rippleT * rippleT);
                        float3 grad = float3(normalize(toCentre + 1e-5f), -deriv);
                        float3 bitangent = float3(-grad.y, grad.x, 0.0f);
                        rippleNormal = WetReorient(normalize(cross(grad, bitangent)), rippleNormal);
                    }
                }
            }
        }
    }
    return rippleNormal;
}

#ifndef PUDDLES_COMPILED
    #define PUDDLES_NOT_ACTIVE_AND
#endif
#ifdef PUDDLES_COMPILED
// Value noise over the world's xy, 0-1.
float PuddleHash(float2 p) { return WetHash33(float3(p, 7.13f)).x; }
float PuddleValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = p - i;
    float2 u = f * f * (3.0f - 2.0f * f);
    float a = PuddleHash(i), b = PuddleHash(i + float2(1.0f, 0.0f));
    float c = PuddleHash(i + float2(0.0f, 1.0f)), d = PuddleHash(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Where water gathers, 0-1 (higher fills first): two octaves of noise at Size (three at Quality 3), stretched to 0-1, plus on the land
// (height >= 0, Quality 2+) the height map's crevices (Lagarde's flood level: the cracks fill before the open ground).
float PuddleField(float2 worldXY, float height) {
    float2 p = worldXY * TESR_PuddleData.z;
    float n = PuddleValueNoise(p) * 0.62f + PuddleValueNoise(p * 2.31f + 17.7f) * 0.38f;
    [branch] if (PUDDLE_QUALITY > 2.5f) n = n * 0.85f + PuddleValueNoise(p * 5.7f + 41.3f) * 0.15f;
    n = smoothstep(0.2f, 0.8f, n);
    // 2026-11-12: the height is relative now (each layer against its own average, TerrainTemplate), so it can carry the shape: x1 (was
    // x0.5 of an absolute height, which shifted whole textures up or down). A flat texture barely moves the field; a bumpy one rules it.
    // (x1.5 broke big puddles on gravel into islands everywhere in the slot C bench; the deep middle should drown the bumps.)
    if (height >= 0.0f && PUDDLE_QUALITY > 1.5f) n += TESR_PuddleShape.z * (0.5f - height);
    return n;
}

// Rain running down a steep face: lanes 1 / TESR_PuddleStreak.y wide across the face, one in three dry, each with drops running down at
// its own speed (a bright head, a fading tail above it). 0-1. tilt: how the water's surface leans (world), for the shading: a rivulet
// bulges across its lane, a drop's tail slopes up from its head, so both catch the sky as moving glints (a flat film on a wall reflects
// almost nothing, 2026-11-11: streaks on the dark puddle2 shack walls were there in the mask and invisible in the picture).
float PuddleStreaks(float3 worldPos, float3 n, float t, out float3 tilt) {
    float2 across = float2(-n.y, n.x);
    across /= max(length(across), 1e-4f);
    float u = dot(worldPos.xy, across) * TESR_PuddleStreak.y;
    float lane = floor(u);
    float3 r = WetHash33(float3(lane, 3.1f, 9.7f));
    float side = frac(u) - 0.5f;
    float sharp = 2.0f * (1.4f + r.y * 1.6f);
    float m = saturate(1.0f - abs(side) * sharp);
    float laneMask = m * m * (3.0f - 2.0f * m);   // a rounded rivulet
    float wet = r.x > 0.33f ? 1.0f : 0.0f;
    // The pattern moves down (the phase grows with z and with time); 2026-11-11 the head is now at the BOTTOM of each period (g = 1 just
    // above the wrap) and the tail fades above it (it was flow^8: the bright part at the top, trailing its tail).
    float flow = frac(worldPos.z * (0.012f + 0.01f * r.z) + t * TESR_PuddleStreak.z * (0.35f + r.y) + r.x * 13.0f);
    float g = 1.0f - flow;
    float g7 = g * g * g;
    g7 = g7 * g7 * g;
    float drip = g7 * g;   // g^8: a short bright head
    float body = wet * (0.6f + 0.4f * drip);   // 2026-11-11: a steadier rivulet body (was 0.4 + 0.6 drip: mostly faint between heads)
    // the surface's slopes: across the lane d(laneMask)/d(side), up the tail d(drip)/d(-z) (the tail thins upward: the normal leans up)
    float slopeAcross = (abs(side) * sharp < 1.0f ? 6.0f * m * (1.0f - m) * sharp : 0.0f) * -sign(side);
    tilt = (float3(across, 0.0f) * slopeAcross * 0.15f + float3(0.0f, 0.0f, 1.0f) * g7 * 0.6f * laneMask) * body;
    return laneMask * body;
}
#endif

// The water layer at a pixel, as CS computes it in Lighting.hlsl.
struct Wetness {
    float  darkening;   // wetnessGlossinessAlbedo: how much the albedo darkens (times porosity)
    float  gloss;       // wetnessGlossinessSpecular: how much water film there is
    float  roughness;   // the film's roughness
    float3 normal;      // the film's normal (world): ripples, flattened in puddles
#ifdef PUDDLES_COMPILED
    float  flatten;     // Puddles: how far standing water hides the material's own normal map (0-1; the templates flatten their normal)
    float  murk;        // 2026-11-12 Puddles: how deep and muddy the standing water is (0-1): the templates tint its reflection (PuddleMurkTint)
    float4 debris;      // 2026-11-12 Puddles: a floating speck or leaf (rgb its colour, a its cover), the land template lays it over the albedo
#endif
};

// position: camera-relative; vertexNormal: the geometric normal (world); normal: the normal-mapped
// world normal; height: the surface's height map value (0 deepest, 1 highest) where the template has
// one (WETNESS_HEIGHT, the land), else negative.
Wetness GetWetness(float3 position, float3 vertexNormal, float3 normal, float height = -1.0f) {
    Wetness w;
    w.darkening = 0.0f;
    w.gloss = 0.0f;
    w.roughness = 1.0f;
    w.normal = normal;
#ifdef PUDDLES_COMPILED
    w.flatten = 0.0f;
    w.murk = 0.0f;
    w.debris = 0.0f;
#endif
    [branch] if (!WETNESS_ON) return w;

    float3 worldPos = TESR_WetnessCamera.xyz + position;
    float distance = length(position);

    // How far the surface faces up decides how wet it gets and where puddles may sit. On the land
    // (WETNESS_HEIGHT) the normal map has its say (NormalAwareness): the tops of bumps and stones soak
    // like flat ground, their steep sides less, and puddles keep off rough patches.
    float3 angleNormal = vertexNormal;
#ifdef WETNESS_HEIGHT
    angleNormal = normalize(lerp(vertexNormal, normal, TESR_WetnessHeight.z));
#endif
    float minWetnessAngle = saturate(max(TESR_WetnessParams.z, angleNormal.z));
    float occlusion = WetnessOcclusion(position, vertexNormal);
    float flatness = smoothstep(TESR_WetnessParams2.x, 1.0f, minWetnessAngle);

    float3 raindrop = float3(0.0f, 0.0f, 1.0f);
#if defined(WETNESS_RAINDROPS) && !defined(PUDDLE_OBJECT_RIPPLES)
    [branch] if (PUDDLES_NOT_ACTIVE_AND vertexNormal.z > 0.0f && TESR_WetnessData.z > 0.0f && occlusion > 0.5f && distance < TESR_WetnessRipple.w)
        raindrop = WetRainDrops(worldPos, TESR_WetnessData.w, vertexNormal, flatness);
#endif

    float rainWetness = TESR_WetnessData.x * minWetnessAngle * TESR_WetnessParams.x;
    float wetness = rainWetness;

    // Puddles.
    float puddleWetness = TESR_WetnessData.y * minWetnessAngle;
    float puddle = wetness;
#ifndef PUDDLES_MAP	// (map mode: the puddles' own field below; Quality 0 there = rain wetness without CS's noise puddles)
    [branch] if (wetness > 0.0f || puddleWetness > 0.0f) {
        float3 coords = (worldPos * 0.5f + 0.5f) * 0.01f / TESR_WetnessParams.w;
        puddle = WetPerlin(coords) * 0.5f + 0.5f;
        puddle = puddle * ((minWetnessAngle / TESR_WetnessParams2.x) * TESR_WetnessParams.y * 0.25f) + 0.5f;
        puddle *= lerp(wetness, puddleWetness, saturate(puddle - 0.25f));
    }
#endif

#ifdef WETNESS_HEIGHT
    // The height map: water runs off the ridges and gathers in the crevices. A water line rises with
    // the wetness (CreviceFill at full); below it the surface stands under water, a flat puddle that
    // fills the crevices first. Above it, crevices hold more of the puddle water and ridges shed it.
    // Only where the ground is flat enough to hold water (flatness).
    [branch] if (height >= 0.0f && TESR_WetnessHeight.x > 0.0f) {
        float soak = saturate(max(wetness, puddleWetness));
        float level = TESR_WetnessHeight.y * soak;
        float filled = saturate((level - height) * 20.0f) * soak * flatness;
        float ridge = saturate(height * 2.0f - 0.5f);   // 0 at the bottom quarter, 1 at the top quarter
        float shaped = puddle * lerp(1.3f, 0.4f, ridge);
        puddle = lerp(puddle, max(shaped, filled), TESR_WetnessHeight.x);
    }
#endif

    // Under cover nothing gets rained on; far away, none (CS's nearFactor).
    float nearFactor = smoothstep(4096.0f * 2.5f, 0.0f, distance);
    puddle *= saturate(occlusion * 2.0f) * nearFactor;

#ifdef PUDDLES_COMPILED
    // Puddles (Quality > 0): the rain's porous wetness without the CS noise, the soaked ring and the standing water from the field.
    [branch] if (PUDDLES_ON) {
        // 2026-11-12 rework (the user's reference pictures: wet mud by containers, an overcast yard, a swampy meadow): three zones per pixel,
        //   damp ground  (the rain): a little darker, glossy (roughness ~0.3), the material's normal map KEPT: wet mud glints on its bumps;
        //   soaked rim   (around standing water, its width wandering with noise): darker, low roughness (~0.12), normal map kept;
        //   standing     (where the water level is above the ground): a flat mirror (Roughness), near-black and muddier with depth.
        // Where water stands comes from the land's own relief first (relative height, TerrainTemplate; else the texture's luminance
        // detail): crevices, ruts and dips fill, bumps poke out as islands. Flat textures (asphalt, concrete) have little relief, so
        // the large noise field shapes big irregular puddles there; bumpy ones (mud, dirt, rock) get many small pools: the material
        // tells itself apart through its own height map, no per-texture data needed.
        float open = saturate(occlusion * 2.0f) * nearFactor;
        float level = TESR_PuddleData.x;
        float standing = 0.0f, ring = 0.0f, streak = 0.0f, deep = 0.0f;
        float3 streakTilt = 0.0f;
        float rain = saturate(rainWetness) * open;
        puddle = rain;
        w.darkening = rain * rain * 0.7f;
        float rough = lerp(1.0f, 0.22f, rain);   // damp: glossy, keeps its bumps (0.3 read matte on the slot C mud under overcast)
        [branch] if (level > 0.001f) {
        #ifdef PUDDLES_MAP
            // map mode: the field's large-scale part and the rim wander from the map; the surface's own relief added here, per pixel
            // (the cover's read, at the look-out point: the same spot on flat ground; steep faces get no standing water anyway)
            float4 mapHere = PuddleMapSample;
            float field = mapHere.r + (height >= 0.0f && PUDDLE_QUALITY > 1.5f ? TESR_PuddleShape.z * (0.5f - height) : 0.0f);
        #else
            float2 p = worldPos.xy * TESR_PuddleData.z;
            float field = PuddleField(worldPos.xy, height);
        #endif
            float t = 1.0f - level * TESR_PuddleData.y;
        #if defined(PUDDLES_MAP)
            float wander = mapHere.b;
        #elif defined(PUDDLES_LEAN)
            const float wander = 0.5f;   // (the outdoor lamp-shadow object records: no room for the extra noise)
        #else
            float wander = PuddleValueNoise(p * 3.7f + 5.1f);                       // 0-1, varies the shoreline softness and the rim width
        #endif
            float soft = max(TESR_PuddleLook.z, 1e-3f) * (0.5f + wander);
            float slope = smoothstep(TESR_PuddleShape.x, TESR_PuddleShape.y, vertexNormal.z);
            float shore = saturate((field - t) / soft);
            standing = shore * shore * (3.0f - 2.0f * shore) * slope * open;
            deep = saturate((field - t) / (soft * 5.0f)) * standing;
            [branch] if (PUDDLE_QUALITY > 1.5f) {
                float ringW = max(TESR_PuddleShape.w, 1e-3f) * (0.35f + 1.3f * wander);
                ring = saturate((field - t + ringW) / ringW) * smoothstep(TESR_PuddleShape.x - 0.15f, TESR_PuddleShape.x, vertexNormal.z)
                     * open * saturate(level * 4.0f);
                ring *= ring;
            }
            // Floating bits on standing water (Quality 2+, the land only: PuddleDebris): sparse specks and leaves, hashed per cell.
        #if defined(WETNESS_HEIGHT) && !defined(PUDDLES_LEAN)
            [branch] if (PUDDLE_QUALITY > 1.5f && standing > 0.3f && distance < 1500.0f) {
                float2 cell = floor(worldPos.xy * 0.16f);
                float3 hsh = WetHash33(float3(cell, 19.7f));
                float2 at = (cell + 0.2f + 0.6f * hsh.xy) / 0.16f;
                float r = lerp(0.6f, 1.8f, hsh.z);
                float speck = (hsh.x * hsh.y < 0.012f) ? saturate((r - length(worldPos.xy - at)) * 1.5f) : 0.0f;
                float3 tint = hsh.z > 0.6f ? float3(0.05f, 0.07f, 0.02f) : float3(0.09f, 0.05f, 0.02f);   // dull green specks, brown leaves (linear)
                w.debris = float4(tint, speck * smoothstep(0.3f, 0.7f, standing) * (1.0f - smoothstep(1000.0f, 1500.0f, distance)));
                standing *= 1.0f - w.debris.a;
            }
        #endif
            w.flatten = standing;
            w.murk = deep;
            // what the water does to the material under it: the porous darkening, further with depth (absorption)
            puddle = max(rain, ring);
            w.darkening = lerp(max(rain * rain * 0.7f, ring * 0.85f), 1.0f + TESR_PuddleLook.y * deep, standing);
            rough = lerp(rough, 0.12f, ring);
        }
        // Rain streaks down steep faces open to the sky, while it rains.
        [branch] if (PUDDLE_QUALITY > 1.5f && TESR_PuddleStreak.x > 0.0f && abs(vertexNormal.z) < 0.6f && distance < 3000.0f) {
            float streakFade = (1.0f - smoothstep(0.35f, 0.6f, abs(vertexNormal.z))) * TESR_PuddleStreak.x * saturate(occlusion * 2.0f)
                             * (1.0f - smoothstep(2000.0f, 3000.0f, distance));
            streak = PuddleStreaks(worldPos, vertexNormal, TESR_WetnessData.w, streakTilt) * streakFade;
            streakTilt *= streakFade;
            // 2026-11-11 (user: "streaks not working"): a streak is more water on top of the wall's rain wetness, so it adds to it. It was
            // max(darkening, streak x 0.6) and max(film, streak): a wall's own rain wetness (MinRainWetness 0.65: darkening 0.42, film
            // 0.65) was as strong as most of a streak, which then hardly showed.
            w.darkening = lerp(w.darkening, 1.0f, streak * 0.8f);
        }
        float film = max(max(puddle, standing), lerp(puddle, 1.0f, streak));

        // The water's surface: the face's own normal under standing water (no bumps), the material's elsewhere; rain rings on top.
        float3 wetN = normalize(lerp(normal, vertexNormal, standing));
        wetN = normalize(lerp(wetN, vertexNormal, saturate(streak)) + streakTilt);   // 2026-11-11 a streak's water hides the bumps and bulges
        float3 ripple = float3(0.0f, 0.0f, 1.0f);
    #ifdef WETNESS_RAINDROPS
        #ifdef PUDDLE_OBJECT_RIPPLES
        [branch] if (PUDDLE_QUALITY > 2.5f && standing > 0.01f && TESR_WetnessData.z > 0.0f && distance < TESR_WetnessRipple.w)
        #else
        [branch] if (vertexNormal.z > 0.0f && TESR_WetnessData.z > 0.0f && occlusion > 0.5f && distance < TESR_WetnessRipple.w)
        #endif
            ripple = WetRainDrops(worldPos, TESR_WetnessData.w, vertexNormal, 1.0f);
    #endif
        float rippleW = saturate(max(standing * TESR_PuddleLook.w, smoothstep(TESR_WetnessParams2.y, 1.0f, puddle) * 0.5f));
        wetN = WetReorient(wetN, normalize(lerp(float3(0.0f, 0.0f, 1.0f), ripple, rippleW)));

        w.gloss = saturate(film);
        PuddleStandingOut = standing;
        rough = lerp(rough, 0.05f, streak);
        w.roughness = lerp(max(rough, 0.05f), max(TESR_PuddleLook.x, 0.01f), standing);
        w.normal = wetN;

        [branch] if (PUDDLE_DEBUG > 0.5f) {
            if (PUDDLE_DEBUG < 1.5f) PuddleDebugColor = float3(standing, ring * (1.0f - standing), streak);
            else if (PUDDLE_DEBUG < 2.5f) PuddleDebugColor = float3(saturate(ripple.xy * 2.0f + 0.5f), standing * 0.5f);
            else if (PUDDLE_DEBUG < 3.5f) PuddleDebugColor = float3(TESR_WetnessData.x, TESR_PuddleData.x, TESR_WetnessData.z);
            // 2026-11-11 4: the streaks' inputs: red open to the sky, green how steep the face's normal is (1 vertical), blue the raw lanes
            else if (PUDDLE_DEBUG < 4.5f) PuddleDebugColor = float3(saturate(occlusion * 2.0f), 1.0f - abs(vertexNormal.z), PuddleStreaks(worldPos, vertexNormal, TESR_WetnessData.w, streakTilt));
        #ifdef PUDDLES_MAP
            else { float d5 = (TESR_WetnessCamera.z + position.z) - (PuddleMapSample.g + TESR_PuddleMapNear.w);
                   PuddleDebugColor = float3(PuddleMapSample.r, saturate(0.5f + d5 / 256.0f), TESR_PuddleMapFar.w); }   // 5: red the map's field, green this point against the map's top surface (0.5 = on it, brighter = above), blue the map holds data
        #else
            else PuddleDebugColor = 0.0f;
        #endif
        }
        return w;
    }
#endif

    float3 wetNormal = normalize(lerp(normal, vertexNormal, saturate(puddle)));

    float glossAlbedo = puddle;
    glossAlbedo *= glossAlbedo;

    float glossSpecular = puddle;

    flatness *= smoothstep(TESR_WetnessParams2.y, 1.0f, glossSpecular);
    float3 rippleNormal = normalize(lerp(float3(0.0f, 0.0f, 1.0f), raindrop, lerp(flatness, 1.0f, 0.5f)));
    wetNormal = WetReorient(wetNormal, rippleNormal);

    w.darkening = glossAlbedo;
    w.gloss = saturate(glossSpecular);
    w.roughness = max(saturate(1.0f - glossSpecular), 0.05f);   // CS's floor: real water is never optically perfect
    w.normal = wetNormal;
    return w;
}

// Wet albedo (CS): lerp(albedo, albedo^(1 + porosity x darkening), 0.5). Linear albedo.
float3 WetAlbedo(float3 albedo, float darkening, float porosity) {
    float d = porosity * darkening;
    return lerp(albedo, pow(max(albedo, 0.0f), 1.0f + d), 0.5f);
}

// The water film's highlight from one light (CS's GetWetnessSpecular: GGX, F0 0.02), N.L included.
float3 WetSpecular(float3 N, float3 V, float3 L, float3 light, float roughness) {
    float3 H = normalize(L + V);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V)) + 1e-4f;
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    float D = a2 / (3.14159265f * d * d);
    float k = a * 0.5f;
    float vis = 0.25f / ((NdotL * (1.0f - k) + k) * (NdotV * (1.0f - k) + k));
    float F = 0.02f + 0.98f * pow(1.0f - VdotH, 5.0f);
    return light * (D * vis * F * NdotL);
}

#ifdef PUDDLES_COMPILED
// 2026-11-12 Puddles: a lamp's highlight on wet ground stretched toward the viewer, the wet-road streak (Frostbite's wet roads): the half
// vector's part along the view direction projected onto the surface is shrunk before the GGX lobe is read, so the lobe runs long down
// that line. stretch 1 = round (WetSpecular), smaller = longer.
float3 WetSpecularStretched(float3 N, float3 V, float3 L, float3 light, float roughness, float stretch) {
    float3 along = V - N * dot(V, N);
    float len = length(along);
    along = len > 1e-4f ? along / len : float3(0.0f, 0.0f, 0.0f);
    float3 H = normalize(L + V);
    H = normalize(H - along * dot(H, along) * (1.0f - stretch));
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V)) + 1e-4f;
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, normalize(L + V)));
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    float D = a2 / (3.14159265f * d * d);
    float k = a * 0.5f;
    float vis = 0.25f / ((NdotL * (1.0f - k) + k) * (NdotV * (1.0f - k) + k));
    float F = 0.02f + 0.98f * pow(1.0f - VdotH, 5.0f);
    return light * (D * vis * F * NdotL);
}

// 2026-11-12 objects have no height map: their colour texture's brightness detail against its own average (the last mip) stands in, so
// dark gaps, cracks and seams of a concrete slab or a road piece fill first (the noise-only puddles of before). Read at a mip chosen by
// distance (no derivatives: the callers are inside branches), only while there is water; -1 = none (the field then ignores it).
float ObjectPuddleHeight(sampler2D baseMap, float2 uv, float3 position) {
    float h = -1.0f;
    [branch] if (TESR_PuddleData.x > 0.001f && PUDDLES_ON && PUDDLE_QUALITY > 1.5f) {
        const float3 lumW = float3(0.3f, 0.59f, 0.11f);
        float here = dot(tex2Dlod(baseMap, float4(uv, 0.0f, max(0.0f, log2(length(position) / 256.0f)))).rgb, lumW);
        float mean = dot(tex2Dlod(baseMap, float4(uv, 0.0f, 14.0f)).rgb, lumW);
        h = saturate(0.5f + 0.6f * (here - mean));
    }
    return h;
}

// The same stretched highlight, cheap enough for the lamp loops (a normalised Blinn lobe on the stretched half vector, the water's
// Schlick Fresnel; no visibility term): the land's lamp loop and the objects' lamp passes, where the GGX version above ran the ps_3_0
// shaders out of temps (the land's RT lamp-shadow record). V: the direction to the eye, worked out once per pixel by the caller.
float3 WetLampFilm(float3 N, float3 V, float3 L, float3 light, float roughness, float stretch) {
    float3 along = V - N * dot(V, N);
    along *= rsqrt(max(dot(along, along), 1e-8f));
    float3 H = normalize(L + V);
    float VdotH = saturate(dot(V, H));
    H = normalize(H - along * (dot(H, along) * (1.0f - stretch)));
    float r2 = max(roughness * roughness, 0.002f);
    float power = 2.0f / (r2 * r2) - 2.0f;
    float spec = pow(saturate(dot(N, H)), power) * ((power + 8.0f) * 0.0397887f);   // (n + 8) / (8 pi)
    float f = 1.0f - VdotH;
    float f2 = f * f;
    return light * (spec * (0.02f + 0.98f * f2 * f2 * f) * saturate(dot(N, L)));
}

// The standing water's colour over what it reflects: clear at the shore, a muddy brown-grey where it is deep.
float3 PuddleMurkTint(float murk) { return lerp(float3(1.0f, 1.0f, 1.0f), float3(0.62f, 0.55f, 0.44f), murk); }
#endif

// The water film's reflection weight (split sum for F0 0.02), times how much film there is.
float WetReflectance(float NdotV, float roughness, float gloss) {
    // Karis' EnvBRDFApprox, for F0 0.02.
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return (0.02f * AB.x + AB.y) * gloss;
}

// UNOFFICIAL VegasShaders: the sky the film reflects along r, in the lighting space (PBR NVR's SkyReflectionRadiance). SkyAmbient.hlsl's
// sky blurred toward the normal-weighted ambient; the includers have it (Object.hlsl, Terrain.hlsl: include this after them).
float3 WetSkyReflection(float3 r) {
    return LinSky(SkyAmbientRadiance(r, 1.0f));
}

#endif
