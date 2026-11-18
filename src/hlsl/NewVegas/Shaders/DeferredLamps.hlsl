// UNOFFICIAL 2026-10-31 DeferredLamps (core/DeferredLamps.h; docs/next/DEFERRED-C1-SPEC.md): the lamps of the pixels the deferred records drew
// (the many-lamp family's Interior records with LAMPS_DEFERRED: ambient, emittance and fog only), added in ONE full-screen pass after the main
// list's G-buffer is resolved and before LinearScene encodes the frame. The maths is the object shaders' own (Object.hlsl
// getPointLightLightingAtt, PBR.hlsl, LampFalloff.hlsl, PointShadowForward.hlsl PointShadowCube): this file only gathers the inputs from the
// G-buffer instead of the draw's interpolators and constants. Loaded like a template (ShaderRecord::LoadShader, the same define list), compiled
// with INTERIOR_SHADOWS; the object shaders' frame-global constants are copied into their own registers by DeferredLamps::Apply.

// The object shaders' own preamble for what the includes read (ObjectTemplate.hlsl): linear lighting as the many-lamp records light (without it
// LampAtt keeps the falloff undecoded: att, not att^gamma, a far flatter and wider lamp), and SPECULAR (SLS2034-2036 are; without it the lamps
// take PBRDiffuse: no highlight, no Fresnel). Round 3 bench + review, 2026-10-31.
#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
    #if defined(LIN_PREDECODE_ON) && LIN_PREDECODE_ON
        #define LIN_PREDECODED
    #endif
#endif
#define SPECULAR

// UNOFFICIAL 2026-11-15 fd-lampfit (docs/next/FD-LAMPFIT.md): POINT_SHADOW_RT (RT lamp shadows on the fork, or CapsuleShadows on any runtime)
// is read by the shadow passes only (DLShadow's traced read, a pixel's first 16 lamps). Every other pass of this file looks lamps past the 16th
// up with rtId -1 (never traced), where the RT code in PointShadowCube adds nothing to the result but its compare-view encoding
// (PointShadowDone), which still took the lamp loop past ps_3_0's 32 temp registers: X4505, the lamp pass did not load and the whole deferred
// renderer drew forward (a tester's 4090, 2026-11-15). So those passes are compiled without it: the same result for every lamp (the traced
// compare view, c215.x 2, was never decoded here: it only scaled lamps past the 16th by 2 and more), and the same bytecode as without RT.
#if defined(POINT_SHADOW_RT) && !(defined(DL_SHADOW_PASS) && DL_SHADOW_PASS) && !(defined(POINT_SHADOW_RT_ONLY) && POINT_SHADOW_RT_ONLY)
    #undef POINT_SHADOW_RT
    #define POINT_SHADOW_RT 0
#endif

#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"
#include "includes/LampFalloff.hlsl"
#include "includes/Shadow.hlsl"
#include "includes/PointShadowForward.hlsl"
// UNOFFICIAL 2026-11-14 DeferredSun: with the lighting-free records made (FD_LIGHT_FREE from DeferredLamps.h) COLOR1.a may carry their code
// (Includes/DeferredPixel.hlsl: base + 1 - fog); the keep is decoded from it. Without them the alpha is the keep, as before.
#if defined(FD_LIGHT_FREE) && FD_LIGHT_FREE
    #include "includes/DeferredPixel.hlsl"
    #define DL_KEEP_OF(a) FDAddedLightKeep(a)
#elif defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 4
    #include "includes/DeferredPixel.hlsl"
    #define DL_KEEP_OF(a) FDAddedLightKeep(a)	// GBufferV2: 1 + (1 - fog), or the "sun kept" records' 5 + (1 - fog) (2026-11-14)
#else
    #define DL_KEEP_OF(a) (a)
#endif
#define DL_KEEP(uv4) DL_KEEP_OF(tex2Dlod(DL_Albedo, uv4).a)
// UNOFFICIAL 2026-11-15 DeferredSkin (Includes/SkinDeferred.hlsl; only with [Main.Main.Quality] DeferredSkin at startup): the deferred skin
// record's pixels (COLOR3.a 4 and up, COLOR2 model 6) take the skin's own lamp model, and pass the actor clip below
#ifndef FD_SKIN
    #define FD_SKIN 0
#endif
#if FD_SKIN
    #define FD_SKIN_PASS
    #include "includes/SkinDeferred.hlsl"
    #define DL_ACTOR_CLIP(a) clip((a) > 1.5f && (a) < FD_SKIN_CODE_MIN ? -1.0f : 1.0f)
    static float4 dlSkinUV = 0.0f;	// the pixel's uv for DLLampSkin's own reads (the loop passes the surface it needs; the skin needs COLOR3 too)
#endif
#if defined(DL_SKIN_ONLY) || (defined(DL_PAST16_LIT) && DL_PAST16_LIT)
    // the skin pass: a pixel's first 16 lamps take the shadow passes' results (the full filter) as in the lamp pass; lamps past the 16th are lit
    // as UnshadowedLamps says (no lookup in the loop: the skin's model and a lookup together run out of temp registers, X4505)
    // UNOFFICIAL 2026-11-15 fd-lampfit DL_PAST16_LIT: the lamp pass's fallback level (core/DeferredLamps.h EnsurePass), the same for every pixel,
    // when the full pass did not compile: one log line names it, and the deferred renderer stays on
    #define DL_SHADOW_PAST16(i, position, att, uv4) DL_Grid.w
#else
    #define DL_SHADOW_PAST16(i, position, att, uv4) DLShadow(i, position, att, uv4)
#endif
#if !FD_SKIN
    #define DL_ACTOR_CLIP(a) clip(1.5f - (a))
#endif
#ifdef NVR_DEFERRED_PIXEL_HLSL
    // UNOFFICIAL 2026-11-14 the stored-curve fog (Includes/DeferredPixel.hlsl FDAddFogged, core/DeferredFog.h): rgb the fog colour the records
    // lerped with (stored curve), w 1 = the game's fog is drawn. Then the lighting pass writes its raw light (x 1, not x keep) into its own
    // target and the resolve pass (DL_FOG_RESOLVE, below) adds FDAddFogged of it from the frame's copy on s0 (linear or stored, DL_Fwd.w):
    // two passes, because the lighting pass has no temp register left for the fog's maths next to its lamp loop (X4505).
    float4 DL_Fog : register(c12);
    #define DL_FOG_EXACT 1
    sampler2D DL_LampLight : register(s2);	// (the resolve pass) the lighting pass's raw light
#endif

// The pass's own constants (c0-c9: nothing the includes declare lives there).
float4 DL_Grid   : register(c0);	// x tiles across, y tiles down, z 1 = the froxel lists are this frame's, w UnshadowedLamps (1 lit, 0 skipped)
float4 DL_View   : register(c1);	// xy 1 / viewport size, zw the viewport's top-left corner (pixels)
float4 DL_Right  : register(c2);	// xyz the camera's right axis, w tan(half fov x)
float4 DL_Up     : register(c3);	// xyz up, w tan(half fov y)
float4 DL_Fwd    : register(c4);	// xyz forward, w 1 = the frame is linear now (else the copy path)
float4 DL_Rows[3] : register(c5);	// world -> camera-relative rows for LightCommonLamp (identity, minus the camera)
float4 DL_Table  : register(c8);	// x the table's rows, y its first tile row, z the gamma (copy path), w 1 / the gamma
float4 DL_Debug  : register(c9);	// x DebugView: 0 the light, 1 lamp colour x falloff only, 2 position (64-unit checker x distance), 3 the normal, 4 1 - fog, 5 lamps in the froxel / 31
                                	// zw tiles across / down the lists' viewport (Shadows off, a test: DeferredLamps::Apply sends shadow strength 0 in c180.x)
float4 DL_Shadow : register(c10);	// x the shadow pass's first lamp of the list (0, 4, 8, 12), y 1 = the lighting pass has the shadow targets (s9-s12),
								// z PrecisionBias x 0.001 (2026-11-01: the shadow lookup's push off the surface per unit of camera distance),
								// w 1 = this frame's exact linear view z is on s7 (2026-11-14 DL_LINEAR_DEPTH, below)
float4 DL_Depth  : register(c11);	// (lighting pass) x, y the projection's _33, _43 (depth = x + y / view z), z the share and w the units the surface is pulled nearer
#if (defined(DL_LAND_SPLIT) && DL_LAND_SPLIT) || defined(DL_LAND_ONLY)
float4 DL_Land   : register(c14);	// UNOFFICIAL 2026-11-16 fd-batchA (DLLampLand): x 1 = the land loop on, y UsePBR, z the land's metal, w its light scale (decoded)
#endif

#define LIGHT_COMMON_ROWS DL_Table.x
#include "includes/LightCommon.hlsl"

sampler2D DL_Frame    : register(s0);	// the frame's copy (only when it is not linear: enc(dec(v) + L))
sampler2D DL_Albedo   : register(s1);	// TESR_AlbedoBuffer: rgb linear base colour, a 1 - the draw's fog
sampler2D DL_Material : register(s4);	// TESR_MaterialBuffer: r class + 0.49 metal, g oct.x, b roughness, a 1.01..1.99 = deferred (oct.y); GBufferV2: see DL_NORMAL
sampler2D DL_Normals  : register(s5);	// TESR_CapturedNormalsBuffer: rg the smooth normal, b the camera distance

sampler2D DL_Vis0 : register(s9);	// UNOFFICIAL 2026-10-31 the shadow passes' results: lamps 0-3, 4-7, 8-11, 12-15 of the pixel's list
sampler2D DL_Vis1 : register(s10);
sampler2D DL_Vis2 : register(s11);
sampler2D DL_Vis3 : register(s12);

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };

// UNOFFICIAL 2026-11-14 DeferredDistant (core/DeferredDistant.h, docs/next/FD-LOD-RESULTS.md): DL_LINEAR_DEPTH 1 (DeferredLamps.h, with the switch
// on at startup) = the pixel's position from DeferredDepth's exact linear view z (TESR_DeferredDepthLinear, DeferredDepth 2, R32F, s7) while
// DeferredLamps::Apply says this frame has it (DL_Shadow.w 1), as DeferredSun takes it: the G-buffer's camera distance is a half float, a few
// units off past 2,000 and ~64 units at the far land. Taken only where it agrees with that distance to the half float's rounding (1 / 512 of it
// + half a unit), i.e. where the depth buffer's surface IS the G-buffer's: where something that writes no G-buffer was drawn nearer (hair, the
// distant trees) the G-buffer's own distance is kept, so the lamps land on the surface they light. Without the define: as before.
#if defined(DL_LINEAR_DEPTH) && DL_LINEAR_DEPTH
sampler2D DL_LinearZ : register(s7);	// no sampler_state: DeferredLamps::Apply binds it point / clamp
float DLViewZ(float4 uv4, float storedViewZ) {
    const float z = tex2Dlod(DL_LinearZ, uv4).x;
    return DL_Shadow.w > 0.5f && abs(z - storedViewZ) <= storedViewZ * (1.0f / 512.0f) + 0.5f ? z : storedViewZ;
}
#endif

// UNOFFICIAL 2026-11-14 GBufferV2 (CAPTURED_NORMALS_ON 4, Includes/CapturedNormals.hlsl): a deferred record's pixel is marked by the albedo's
// alpha, 1 + (1 - fog) (forward records write 1), and its normal-mapped normal is in the material's g / a at full precision (g 4 and up:
// none there, the smooth normal). Without it: the old codes, a in 1.01 .. 1.99 with the normal's y in it.
#if CAPTURED_NORMALS_ON >= 4
    #define DL_DEFERRED_CLIP(material0, uv4) clip(tex2Dlod(DL_Albedo, uv4).a - 1.0001f)
    #define DL_NORMAL(m, uv4) (abs((m).g) <= 1.001f ? ShadowDecodeNormal((m).ga) : ShadowDecodeNormal(tex2Dlod(DL_Normals, uv4).rg))
#elif defined(FD_LIGHT_FREE) && FD_LIGHT_FREE
    // UNOFFICIAL 2026-11-14 DeferredSun (Includes/DeferredPixel.hlsl): the lighting-free records' pixels too (their lamps are this pass's), and
    // their normal: the older code where a record wrote it (the table family, the land), else COLOR3's smooth normal
    #define DL_OLD_CODE(m) ((m).a > 1.005f && (m).a < 1.995f)
    #define DL_DEFERRED_CLIP(material0, uv4) clip(max(FDLightFreePixel(tex2Dlod(DL_Albedo, uv4).a), DL_OLD_CODE(material0) ? 1.0f : 0.0f) - 0.5f)
    #define DL_NORMAL(m, uv4) (DL_OLD_CODE(m) ? ShadowDecodeNormal(float2((m).g, ((m).a - 1.01f) / 0.49f - 1.0f)) : ShadowDecodeNormal(tex2Dlod(DL_Normals, uv4).rg))
#else
    #define DL_DEFERRED_CLIP(material0, uv4) clip((material0).a - 1.005f); clip(1.995f - (material0).a)
    #define DL_NORMAL(m, uv4) ShadowDecodeNormal(float2((m).g, ((m).a - 1.01f) / 0.49f - 1.0f))
#endif

// UNOFFICIAL 2026-11-14 (claude/fd-interiors, DeferredRenderer) one lamp's light on the pixel. Under GBufferV2 the record's specular bit
// (COLOR2.r + 16: the SPECULAR variants) picks the model as the forward shader's objLampTerm did: PBR (diffuse + highlight), else PBRDiffuse
// (the plain and light-only records; those objects' own highlight passes are then drawn by the game, core/DeferredRenderer.h Skip). Without
// GBufferV2 every pixel takes PBR, as before.
// (A branch on the bit inside the lamp loop ran out of temp registers, X4505: the loop already uses every one. So the bit is read once per
// pixel and the whole loop is written twice, DL_LAMP_LOOP with each model; only one runs.)
float3 DLLampDiffuse(float3 toLamp, float att, float3 color, float3 V, float3 N, float3 albedo, float rough, float metal) {
    const float3 scaled = color * LIN_SCALE_LIGHT;
    const float3 graded = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
    return LampAtt(att) * MICRO_SHADOW(N, toLamp) PBRDiffuse(metal, rough, graded, N, V, toLamp, scaled);
}

#if (defined(DL_LAND_SPLIT) && DL_LAND_SPLIT) || defined(DL_LAND_ONLY)
// UNOFFICIAL 2026-11-16 fd-batchA item 3 (docs/next/FD-BATCH-A.md; the owner: restore the forward ground look): a land pixel's lamp as the forward
// land shader lit it (Includes/Terrain.hlsl getPointLightLighting, TerrainTemplate.hlsl's lamp loop; keep in step): the falloff LampAttOther (att
// as it is at defaults, not the objects' LinFalloff: the wider pool), UsePBR: PBR with the land's metal (TerrainData.x), the roughness it wrote
// ((1 - gloss) x RoughnessScale) and its light scale (TerrainData.z), the colour NOT graded (the forward land's lamps took the plain base colour;
// only its sun takes Saturation), no MICRO_SHADOW, no energy keep (the land never took either); UsePBR off: the game's Lambert (its Blinn
// highlight needs the layers' gloss power, which the G-buffer lacks; DeferredSun.hlsl LandSun does the same). The roughness: b as written; none
// (the far land's -1) fully rough, as DeferredSun's DSRoughness.
float3 DLLampLand(float3 toLamp, float att, float3 color, float3 V, float3 N, float3 albedo, float rough, float metal) {
    const float3 radiance = color * LampAttOther(att);
    [branch] if (DL_Land.y > 0.5f) return max(0.0f, PBR(DL_Land.z, rough < 0.0f ? 1.0f : rough, albedo, N, V, toLamp, radiance * DL_Land.w));
    return albedo * shades(N, normalize(toLamp)) * radiance;
}
#endif

#if FD_SKIN
// UNOFFICIAL 2026-11-15 DeferredSkin: SkinTemplate.hlsl's SkinLamp on a deferred skin pixel: SkinLight with the lamp's decoded colour x the skin's
// falloff (LampAttOther, not the objects' LampAtt), unshadowed (the loop multiplies the lamp's shadow after, as SkinLamp does), + its highlight.
// The skin's numbers from COLOR3 (its code and the vertex normal), read here: no temp register is held across the loop for them. Two halves,
// two loops (the diffuse, then the highlight: Includes/SkinDeferred.hlsl FDSkinLampDiffuse / FDSkinLampSpecular; whole, X4505).
float3 DLLampSkinDiffuse(float3 toLamp, float att, float3 color, float3 V, float3 N, float3 albedo, float rough, float metal) {
    const float4 normals = tex2Dlod(DL_Normals, dlSkinUV);
    FDSkinSetup(normals.a, rough);
    return FDSkinLampDiffuse(albedo, N, ShadowDecodeNormal(normals.rg), V, toLamp, color * LIN_SCALE_LIGHT * LampAttOther(att));
}
float3 DLLampSkinSpecular(float3 toLamp, float att, float3 color, float3 V, float3 N, float3 albedo, float rough, float metal) {
    FDSkinSetup(tex2Dlod(DL_Normals, dlSkinUV).a, rough);
    return FDSkinLampSpecular(N, V, toLamp, color * LIN_SCALE_LIGHT * LampAttOther(att));
}
#endif

// The pixel's camera-relative position along its view ray (from the stored distance) and its froxel list (64-pixel tile x depth slice, as
// ManyLamps::BinFroxels made the lists; by screen share: the lists were made for the viewport of the frame's first world draw).
void DLPixel(PS_IN IN, float4 uv4, out float3 ray, out float3 position, out float base, out float count) {
    const float2 local = (IN.vpos - DL_View.zw + 0.5f) * DL_View.xy;	// 0..1 across the viewport
    // (2026-11-15 fd-sun-fsr) the exact position at the pixel's own centre, without the + 0.5 above (Includes/DeferredDepth.hlsl DDRay says
    // why: the D3D9 pixel at VPOS i was rasterised at ndc 2 (i - X) / W - 1); the tile below keeps local (the lists' binning).
    // (2026-11-15 fd-lightpass) the half-float distance's branch too (it still had the + 0.5: docs/next/FD-LIGHTPASS-RESULTS.md)
    const float2 exact = local - 0.5f * DL_View.xy;
    const float3 view = DL_Fwd.xyz + DL_Right.xyz * ((exact.x * 2.0f - 1.0f) * DL_Right.w) + DL_Up.xyz * ((1.0f - exact.y * 2.0f) * DL_Up.w);	// forward share 1
    ray = normalize(view);
    const float4 stored = tex2Dlod(DL_Normals, uv4);
#if defined(DL_LINEAR_DEPTH) && DL_LINEAR_DEPTH
    position = view * DLViewZ(uv4, stored.b * rsqrt(dot(view, view)));
#else
    position = ray * stored.b;
#endif
    // UNOFFICIAL 2026-11-01 PrecisionBias (user: lamp shadows on the Vault 21 stair rail flicker unless close): the distance is a half float
    // (about 1 / 2000 of it), which TAA's jitter rounds differently every frame, so a lamp right on a surface flipped between lit and shadowed.
    // The position is pushed off the surface by that much once here (outside the lamp loop: no temp registers left in it); the light's falloff
    // moves by the same fraction of a unit. Forward shaders have the exact position.
    // (2026-11-17 fd-directlight, docs/next/FD-DIRECTLIGHT.md item 3) not a fraction of a unit since PrecisionBias went to 4 (0.4 % of the
    // camera distance): 4 units on a wall 1,000 units away. The lighting pass's lamps undo it (DL_LIT_VECTOR, the lamp loop below).
    // (2026-11-16 fd-roofedge, docs/next/FD-ROOFEDGE.md) the view-faced normal, as before: the object and parallax records now store the smooth
    // normal faced by the triangle's side; the shadow pushes keep forward's (ShadowFacingNormal, idempotent on every other record's normal)
    position += ShadowFacingNormal(ShadowDecodeNormal(stored.rg), position) * (stored.b * DL_Shadow.z);
    const float viewZ = dot(position, DL_Fwd.xyz);
    const float2 tile = min(floor(saturate(local) * DL_Debug.zw), DL_Grid.xy - 1.0f);	// (2026-11-15: never past the grid; the tile size keeps it inside)
    const float slice = viewZ < 64.0f ? 0.0f : min(7.0f, floor(log2(viewZ)) - 5.0f);
    base = (slice * DL_Grid.x * DL_Grid.y + tile.y * DL_Grid.x + tile.x) * LIGHT_COMMON_LIST_TEXELS;
    count = DL_Grid.z > 0.5f ? LightCommonListTexel(base).x : 0.0f;
}

// Lamp k of the pixel's list: its table index, the vector to it, its falloff and colour.
void DLLamp(float k, float base, float3 position, out float i, out float3 toLamp, out float att, out float3 color) {
    const float e = k + 1.0f;
    const float t = floor((e + 0.5f) / 4.0f);
    const float c = e - t * 4.0f;
    const float4 v = LightCommonListTexel(base + t);
    i = dot(v, float4(c < 0.5f, abs(c - 1.0f) < 0.5f, abs(c - 2.0f) < 0.5f, c > 2.5f));
    float4 lamp;
    LightCommonLamp(i, float4(0.0f, 0.0f, 0.0f, 0.0f), DL_Rows[0], DL_Rows[1], DL_Rows[2], 1.0f, lamp, color);
    toLamp = lamp.xyz - position;
    att = lamp.w > 0.0f ? vanillaAtt(toLamp, max(lamp.w, 1.0e-3f)) : 0.0f;
}

// Table lamp i's shadow at this pixel from its tile (the compiled filter), UnshadowedLamps for a lamp without a tile.
float DLShadow(float i, float3 position, float att, float4 uv4) {
    const float x = (i * 2.0f + 0.5f) / LIGHT_COMMON_TEXELS;
    const float tileY = (DL_Table.y + 0.5f) / DL_Table.x;
    const float4 sTile = tex2Dlod(NVR_LampTable, float4(x, tileY, 0.0f, 0.0f));
    float shadow = DL_Grid.w;
#if POINT_SHADOW_RT && DL_SHADOW_PASS
    // UNOFFICIAL 2026-11-03 (rtfwd) the lamp's traced shadow where the RT lamp trace has it (the table's tile row + 1, .y: ManyLamps.h PutTiles,
    // the cube slot the trace knows it by, -1 none), as the forward shaders take it; the atlas where it has none (was always the atlas: RT lamp
    // shadows did nothing on the deferred pixels, most of an interior). The shadow passes only (a pixel's first 16 lamps, its strongest): the
    // lighting pass's own lookups (lamps past the 16th) have no temp registers left for the traced read (X4505), they keep the atlas.
    // (2026-11-06) read before the tile test: a traced lamp without an atlas tile (cube slots 32-63) takes its traced value too (span 0 = no
    // map: PointShadowCube lit where the traced frame did not see the point); it was unshadowed.
    const float rtId = tex2Dlod(NVR_LampTable, float4((i + 0.5f) / LIGHT_COMMON_TEXELS, (DL_Table.y + 1.5f) / DL_Table.x, 0.0f, 0.0f)).y;
    // UNOFFICIAL 2026-11-06 DebugView 8 (diagnostic, VS-AlphaLampTest): which way this lamp's shadow went, as a code the lighting pass shows
    // (lamps 0-2 in r, g, b): 0.05 no atlas tile and not traced (unshadowed), 0.25 no trace id, 0.15 off the traced frame, 0.35 the pixel
    // traced other lamps, 0.45 the traced depth did not match, 0.5 + 0.5 x the traced visibility where it was used
    // (2026-11-15 docs/next/FD-NATIVEFIX.md) DL_NO_RT_VIEW 1 (DeferredLamps::EnsureShadowShader's level 1): without this view, whose second
    // traced lookup per lamp took native + CapsuleShadows' shadow passes to 4,929 slots at FIT 0 (FIT 1 with it out: 3,584, the same shadows)
#if !(defined(DL_NO_RT_VIEW) && DL_NO_RT_VIEW)
    [branch] if (DL_Debug.x > 7.5f) {
        [branch] if (rtId < 0.0f) return sTile.z > 0.0f ? 0.25f : 0.05f;
        const float t = PointShadowTraced(rtId, position);
        return t >= 0.0f ? 0.5f + 0.5f * t : t > -1.5f ? 0.15f : t < -3.5f ? 0.35f : 0.45f;
    }
#endif
    [branch] if (sTile.z > 0.0f || rtId >= 0.0f) {
#else
    const float rtId = -1.0f;
    [branch] if (sTile.z > 0.0f) {
#endif
        const float4 sLight = tex2Dlod(NVR_LampTable, float4(x + 1.0f / LIGHT_COMMON_TEXELS, tileY, 0.0f, 0.0f));
        shadow = PointShadowCube(sTile, sLight, sTile.w, position, ShadowFacingNormal(ShadowDecodeNormal(tex2Dlod(DL_Normals, uv4).rg), position), att POINT_SHADOW_RT_ARG(rtId));	// (fd-roofedge: view-faced, above)
    }
    return shadow;
}

#if defined(DL_FOG_RESOLVE) && DL_FOG_RESOLVE && defined(DL_FOG_EXACT)
// UNOFFICIAL 2026-11-14 the fog resolve (DeferredLamps::Apply, after the lighting pass wrote its raw light into its own target): the exact
// forward stored-curve fog lerp of the pixel with the lamps' light added (Includes/DeferredPixel.hlsl FDAddFogged), added ONE / ONE.
float4 main(PS_IN IN) : COLOR0 {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float3 light = tex2Dlod(DL_LampLight, uv4).rgb;
    clip(dot(light, 1.0f) - 1.0e-9f);	// no lamp light here (not a deferred pixel, the depth test refused it, or no lamp reaches)
    const float keep = DL_KEEP(uv4);
    return float4(FDAddFogged(tex2Dlod(DL_Frame, uv4).rgb, light, keep, DL_Fog.rgb, DL_Table.z, DL_Fwd.w), 0.0f);
}
#elif DL_SHADOW_PASS
// UNOFFICIAL 2026-10-31 the shadow passes (DeferredLamps::Apply, before the lighting pass): lamps DL_Shadow.x .. + 3 of the pixel's list, their
// shadows with the full filter (the lighting pass has no temp registers left for it next to the specular lighting), one target a pass.
float4 main(PS_IN IN) : COLOR0 {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 material0 = tex2Dlod(DL_Material, uv4);
    DL_DEFERRED_CLIP(material0, uv4);
    // an actor's skin drawn over a deferred draw (sleeves, gear) keeps the gear's material flag (the skin blur owns stage 2 during skin draws):
    // the normals' actor mark (a >= 1.5; the deferred records write 1) says the pixel is not the deferred draw's any more (2026-10-31, pale skin)
    DL_ACTOR_CLIP(tex2Dlod(DL_Normals, uv4).a);
    float3 ray, position; float base, count;
    DLPixel(IN, uv4, ray, position, base, count);
    float s0 = 1.0f, s1 = 1.0f, s2 = 1.0f, s3 = 1.0f;
    float i, att; float3 toLamp, color;
    [branch] if (DL_Shadow.x < count) { DLLamp(DL_Shadow.x, base, position, i, toLamp, att, color); [branch] if (att > 0.0f) s0 = DLShadow(i, position, att, uv4); }
    [branch] if (DL_Shadow.x + 1.0f < count) { DLLamp(DL_Shadow.x + 1.0f, base, position, i, toLamp, att, color); [branch] if (att > 0.0f) s1 = DLShadow(i, position, att, uv4); }
    [branch] if (DL_Shadow.x + 2.0f < count) { DLLamp(DL_Shadow.x + 2.0f, base, position, i, toLamp, att, color); [branch] if (att > 0.0f) s2 = DLShadow(i, position, att, uv4); }
    [branch] if (DL_Shadow.x + 3.0f < count) { DLLamp(DL_Shadow.x + 3.0f, base, position, i, toLamp, att, color); [branch] if (att > 0.0f) s3 = DLShadow(i, position, att, uv4); }
    return float4(s0, s1, s2, s3);
}
#else

#ifdef DL_SKIN_ONLY
    // UNOFFICIAL 2026-11-15 SSS v2: the skin's lamp highlights (raw linear light) into COLOR1, the skin's highlight target, bound only while
    // DeferredSkinSSS 2 runs (core/DeferredLamps.h; an unbound COLOR1 is dropped): the scattering leaves them out of what it diffuses
    #define DL_SKIN_SPEC_OUT , out float4 skinSpecOut : COLOR1
    #define DL_SKIN_SPEC_SET(v) skinSpecOut = (v)
#else
    #define DL_SKIN_SPEC_OUT
    #define DL_SKIN_SPEC_SET(v)
#endif
float4 main(PS_IN IN, out float depth : DEPTH DL_SKIN_SPEC_OUT) : COLOR0 {
    DL_SKIN_SPEC_SET(0.0f);
    // Only the position, the lamp list and the sum stay live across the lamp loop; the surface is read again inside it (cached fetches): the
    // specular PBR() and the filtered shadow together need every temp register ps_3_0 has (X4505 otherwise).
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    // UNOFFICIAL 2026-11-04 (as FlashlightDeferred.hlsl; user: hair see-through in the flashlight): a draw that does not write the G-buffer
    // (hair, and anything else drawn over a deferred draw in the span) leaves that draw's surface behind it there, which was lit through it.
    // The pass is depth-tested at the G-buffer's surface (a little nearer, for its precision; per sample under MSAA): where something nearer
    // was drawn, it fails and the pixel is left as it is. From the stored distance alone, before the lamp loop (no temp register kept).
    {
        const float2 local = (IN.vpos - DL_View.zw) * DL_View.xy;	// (2026-11-15 fd-lightpass) the pixel's centre, as DLPixel's position (no + 0.5)
        const float3 view = DL_Fwd.xyz + DL_Right.xyz * ((local.x * 2.0f - 1.0f) * DL_Right.w) + DL_Up.xyz * ((1.0f - local.y * 2.0f) * DL_Up.w);
        const float viewZ = max(tex2Dlod(DL_Normals, uv4).b * rsqrt(dot(view, view)) * (1.0f - DL_Depth.z) - DL_Depth.w, 1.0f);	// (the ray's forward share: 1 / its length)
        depth = saturate(DL_Depth.x + DL_Depth.y / viewZ);
    }
    const float4 material0 = tex2Dlod(DL_Material, uv4);
    DL_DEFERRED_CLIP(material0, uv4);
    // an actor's skin drawn over a deferred draw (sleeves, gear) keeps the gear's material flag (the skin blur owns stage 2 during skin draws):
    // the normals' actor mark (a >= 1.5; the deferred records write 1) says the pixel is not the deferred draw's any more (2026-10-31, pale skin)
    DL_ACTOR_CLIP(tex2Dlod(DL_Normals, uv4).a);
#if FD_SKIN
    // UNOFFICIAL 2026-11-15 DeferredSkin: a deferred skin pixel (model 6 + its COLOR3 code) is lit by the skin pass (DL_SKIN_ONLY, drawn right after
    // this one with the same state: core/DeferredLamps.h Apply), every other pixel by this one
    {
        const bool dlSkin = abs(floor(material0.r + 0.01f) - 6.0f) < 0.5f && tex2Dlod(DL_Normals, uv4).a > FD_SKIN_CODE_MIN;
    #ifdef DL_SKIN_ONLY
        clip(dlSkin ? 1.0f : -1.0f);
    #else
        clip(dlSkin ? -1.0f : 1.0f);
    #endif
    }
#endif
#if (defined(DL_LAND_SPLIT) && DL_LAND_SPLIT) || defined(DL_LAND_ONLY)
    // UNOFFICIAL 2026-11-16 fd-batchA item 3 (DLLampLand): while the land pass runs (DL_Land.x 1: LandLampModel and the pass loaded), the land's
    // pixels (class 4 + the specular bit, 20; the far land writes it too) are lit by it (DL_LAND_ONLY, drawn right after this one with the same
    // state: core/DeferredLamps.h Apply), every other pixel by this one. DL_Land.x 0: this pass lights the land as before.
    {
        const bool dlLand = DL_Land.x > 0.5f && abs(floor(material0.r + 0.01f) - 20.0f) < 0.5f;
    #ifdef DL_LAND_ONLY
        clip(dlLand ? 1.0f : -1.0f);
    #else
        clip(dlLand ? -1.0f : 1.0f);
    #endif
    }
#endif
    const float keep = DL_KEEP(uv4);	// 1 - fog: the share of added light the draw's fog lets through
    clip(keep - 1.0e-4f);

    float3 ray, position; float base, count;
    DLPixel(IN, uv4, ray, position, base, count);
    SetDiffuseEnergyKeep(DL_NORMAL(material0, uv4), -ray, GBufferRoughness(material0.b));	// (2026-11-16 fd-sunvis: the deferred passes' one roughness decode, Includes/Object.hlsl)

    float3 light = 0.0f;
    // UNOFFICIAL 2026-11-17 fd-directlight (docs/next/FD-DIRECTLIGHT.md item 3): DLPixel's position is pushed off the surface for the shadow
    // lookups (PrecisionBias, 0.4 % of the camera distance by default), and every lamp used to light that pushed point: on a wall 1,000 units
    // away, a point 4 units off it, so a lamp standing a few units off the wall lit it at a grazing angle from 4 units nearer the lamp's plane
    // (Vault 21's uplit wall panel took about 0.6 of forward's lamp light; forward lights the exact surface). The light's direction is taken
    // from the surface again: the push added back along the shading normal N, faced to the camera (the push went along the smooth normal so
    // faced; where a normal map tilts N by a, N.L is then off by the push x (1 - cos a), second order). The falloff keeps the pushed point's
    // (its change is the push's share of the lamp's distance, along the normal: small for the grazing lamps the direction mattered for).
    // Exact (the smooth normal decoded again per lamp, or the unpushed position kept through the loop) does not fit the pass's temp registers:
    // X4505 in every variant tried offline (2026-11-17, template_compile, fork and native); this form adds 9 slots and compiles as before.
    #define DL_LIT_VECTOR(toLamp, N) ((toLamp) + (N) * ((dot((N), ray) > 0.0f ? -DL_Shadow.z : DL_Shadow.z) * dot(position, ray)))
    #define DL_LAMP_LOOP(LIGHT_FN) \
    [loop] for (int k = 0; k < 63; k++) { \
        [branch] if ((float)k >= count) break; \
        float i, att; float3 toLamp, color; \
        DLLamp((float)k, base, position, i, toLamp, att, color); \
        [branch] if (att > 0.0f) { \
            float shadow; \
            [branch] if (k < 16 && DL_Shadow.y > 0.5f) { \
                const float q = floor(((float)k + 0.5f) / 4.0f); \
                const float r = (float)k - q * 4.0f; \
                const float4 vis = q < 0.5f ? tex2Dlod(DL_Vis0, uv4) : q < 1.5f ? tex2Dlod(DL_Vis1, uv4) : q < 2.5f ? tex2Dlod(DL_Vis2, uv4) : tex2Dlod(DL_Vis3, uv4); \
                shadow = dot(vis, float4(r < 0.5f, abs(r - 1.0f) < 0.5f, abs(r - 2.0f) < 0.5f, r > 2.5f)); \
            } \
            else shadow = DL_SHADOW_PAST16(i, position, att, uv4); \
            [branch] if (shadow > 0.0f) { \
                const float4 m = tex2Dlod(DL_Material, uv4); \
                const float3 N = DL_NORMAL(m, uv4); \
                const float3 lit = DL_Debug.x > 0.5f && DL_Debug.x < 1.5f ? color * LampAtt(att) : LIGHT_FN(DL_LIT_VECTOR(toLamp, N), att, color, -ray, N, \
                    tex2Dlod(DL_Albedo, uv4).rgb, GBufferRoughness(m.b), saturate((m.r - floor(m.r)) / 0.49f)); \
                light += lit * shadow; \
            } \
        } \
    }
    // the lamp's shadow first, then its light (one value live between the two): lamps 0-15 from the shadow passes (the full filter), the rest
    // looked up here with the filter this pass could be compiled with. UNOFFICIAL 2026-11-14 GBufferV2: the record's specular bit picks the
    // model once per pixel (PBR for the SPECULAR records, PBRDiffuse for the others, as the forward shaders' objLampTerm).
#if CAPTURED_NORMALS_ON >= 4
    #ifdef DL_SKIN_ONLY	// UNOFFICIAL 2026-11-15 DeferredSkin: the skin pass (its own record: a third loop in the lamp pass ran out of temp registers, X4505)
    dlSkinUV = uv4;
    { DL_LAMP_LOOP(DLLampSkinSpecular) }
    DL_SKIN_SPEC_SET(float4(light, 1.0f));	// (SSS v2: the highlights alone, before the diffuse half joins them)
    { DL_LAMP_LOOP(DLLampSkinDiffuse) }
    #elif defined(DL_LAND_ONLY)	// UNOFFICIAL 2026-11-16 fd-batchA item 3: the land pass (its own record, as the skin pass: a third loop, X4505)
    PBRDiffuseKeep = 1.0f;	// (the land never took the diffuse energy keep)
    { DL_LAMP_LOOP(DLLampLand) }
    #else
    [branch] if (floor(material0.r + 0.01f) < 15.5f) { DL_LAMP_LOOP(DLLampDiffuse) }
    else { DL_LAMP_LOOP(getPointLightLightingAtt) }
    #endif
#else
    DL_LAMP_LOOP(getPointLightLightingAtt)
#endif
    #undef DL_LAMP_LOOP
    #undef DL_LIT_VECTOR
    // the inputs as the pass sees them (written over the frame, blend off: DeferredLamps::Apply)
    // (2026-11-03 rtspec) DebugView 7: the lamps' light ALONE over the frame (the game's ambient, emittance and other light left out), in the
    // frame's encoding: the direct lamp light to set beside the reference path tracer's PTDebugView 3 (with PTReplace: the same post chain)
    // UNOFFICIAL 2026-11-06 DebugView 8: the shadow pass's codes for the pixel's lamps 0-2 (DLShadow above; 0.05 no atlas tile), black = no lamp
    [branch] if (DL_Debug.x > 7.5f) return float4(tex2Dlod(DL_Vis0, uv4).rgb, 1.0f);
    [branch] if (DL_Debug.x > 6.5f) return float4(DL_Fwd.w > 0.5f ? light * keep : pow(max(light * keep, 0.0f), DL_Table.w), 1.0f);
    [branch] if (DL_Debug.x > 1.5f) {
        const float3 cell = floor(position / 64.0f);
        const float3 N = DL_NORMAL(material0, uv4);
        const float3 dbg = DL_Debug.x < 2.5f ? float3(frac((cell.x + cell.y + cell.z) * 0.5f) * 0.6f + 0.2f, saturate(length(position) / 4096.0f), 0.0f) :
            DL_Debug.x < 3.5f ? N * 0.5f + 0.5f : DL_Debug.x < 4.5f ? keep.xxx : (count / 31.0f).xxx;
        return float4(dbg, 1.0f);
    }
#ifdef DL_FOG_EXACT
    [branch] if (DL_Fog.w > 0.5f) return float4(light, 1.0f);	// the raw light into its own target (blend off): the resolve pass adds it under fog
#endif
    light *= keep;
    [branch] if (DL_Fwd.w > 0.5f) return float4(light, 0.0f);	// linear frame: ONE / ONE add, exact per sample
    // the frame left in the stored curve: enc(dec(v) + L). UNOFFICIAL 2026-11-04 (as FlashlightDeferred.hlsl): ADDED as the difference it
    // makes (ONE / ONE), not written over the pixel: written, every sample of a multisampled pixel took the resolved copy's one value
    // (alpha-to-coverage edges and terrain blending's per-sample blend flattened, even where the light was nothing); added, each keeps its own.
    const float3 stored = max(tex2Dlod(DL_Frame, float4(IN.uv, 0.0f, 0.0f)).rgb, 0.0f);
    return float4(max(pow(pow(stored, DL_Table.z) + light, DL_Table.w) - stored, 0.0f), 0.0f);
}
#endif
