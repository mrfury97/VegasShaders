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

#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"
#include "includes/LampFalloff.hlsl"
#include "includes/Shadow.hlsl"
#include "includes/PointShadowForward.hlsl"

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
								// z PrecisionBias x 0.001 (2026-11-01: the shadow lookup's push off the surface per unit of camera distance)
float4 DL_Depth  : register(c11);	// (lighting pass) x, y the projection's _33, _43 (depth = x + y / view z), z the share and w the units the surface is pulled nearer

#define LIGHT_COMMON_ROWS DL_Table.x
#include "includes/LightCommon.hlsl"

sampler2D DL_Frame    : register(s0);	// the frame's copy (only when it is not linear: enc(dec(v) + L))
sampler2D DL_Albedo   : register(s1);	// TESR_AlbedoBuffer: rgb linear base colour, a 1 - the draw's fog
sampler2D DL_Material : register(s4);	// TESR_MaterialBuffer: r class + 0.49 metal, g oct.x, b roughness, a 1.01..1.99 = deferred (oct.y)
sampler2D DL_Normals  : register(s5);	// TESR_CapturedNormalsBuffer: rg the smooth normal, b the camera distance

sampler2D DL_Vis0 : register(s9);	// UNOFFICIAL 2026-10-31 the shadow passes' results: lamps 0-3, 4-7, 8-11, 12-15 of the pixel's list
sampler2D DL_Vis1 : register(s10);
sampler2D DL_Vis2 : register(s11);
sampler2D DL_Vis3 : register(s12);

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };

// The pixel's camera-relative position along its view ray (from the stored distance) and its froxel list (64-pixel tile x depth slice, as
// ManyLamps::BinFroxels made the lists; by screen share: the lists were made for the viewport of the frame's first world draw).
void DLPixel(PS_IN IN, float4 uv4, out float3 ray, out float3 position, out float base, out float count) {
    const float2 local = (IN.vpos - DL_View.zw + 0.5f) * DL_View.xy;	// 0..1 across the viewport
    ray = normalize(DL_Fwd.xyz + DL_Right.xyz * ((local.x * 2.0f - 1.0f) * DL_Right.w) + DL_Up.xyz * ((1.0f - local.y * 2.0f) * DL_Up.w));
    const float4 stored = tex2Dlod(DL_Normals, uv4);
    position = ray * stored.b;
    // UNOFFICIAL 2026-11-01 PrecisionBias (user: lamp shadows on the Vault 21 stair rail flicker unless close): the distance is a half float
    // (about 1 / 2000 of it), which TAA's jitter rounds differently every frame, so a lamp right on a surface flipped between lit and shadowed.
    // The position is pushed off the surface by that much once here (outside the lamp loop: no temp registers left in it); the light's falloff
    // moves by the same fraction of a unit. Forward shaders have the exact position.
    position += ShadowDecodeNormal(stored.rg) * (stored.b * DL_Shadow.z);
    const float viewZ = dot(position, DL_Fwd.xyz);
    const float2 tile = floor(saturate(local) * DL_Debug.zw);
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
    [branch] if (DL_Debug.x > 7.5f) {
        [branch] if (rtId < 0.0f) return sTile.z > 0.0f ? 0.25f : 0.05f;
        const float t = PointShadowTraced(rtId, position);
        return t >= 0.0f ? 0.5f + 0.5f * t : t > -1.5f ? 0.15f : t < -3.5f ? 0.35f : 0.45f;
    }
    [branch] if (sTile.z > 0.0f || rtId >= 0.0f) {
#else
    const float rtId = -1.0f;
    [branch] if (sTile.z > 0.0f) {
#endif
        const float4 sLight = tex2Dlod(NVR_LampTable, float4(x + 1.0f / LIGHT_COMMON_TEXELS, tileY, 0.0f, 0.0f));
        shadow = PointShadowCube(sTile, sLight, sTile.w, position, ShadowDecodeNormal(tex2Dlod(DL_Normals, uv4).rg), att POINT_SHADOW_RT_ARG(rtId));
    }
    return shadow;
}

#if DL_SHADOW_PASS
// UNOFFICIAL 2026-10-31 the shadow passes (DeferredLamps::Apply, before the lighting pass): lamps DL_Shadow.x .. + 3 of the pixel's list, their
// shadows with the full filter (the lighting pass has no temp registers left for it next to the specular lighting), one target a pass.
float4 main(PS_IN IN) : COLOR0 {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 material0 = tex2Dlod(DL_Material, uv4);
    clip(material0.a - 1.005f);
    clip(1.995f - material0.a);
    // an actor's skin drawn over a deferred draw (sleeves, gear) keeps the gear's material flag (the skin blur owns stage 2 during skin draws):
    // the normals' actor mark (a >= 1.5; the deferred records write 1) says the pixel is not the deferred draw's any more (2026-10-31, pale skin)
    clip(1.5f - tex2Dlod(DL_Normals, uv4).a);
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

float4 main(PS_IN IN, out float depth : DEPTH) : COLOR0 {
    // Only the position, the lamp list and the sum stay live across the lamp loop; the surface is read again inside it (cached fetches): the
    // specular PBR() and the filtered shadow together need every temp register ps_3_0 has (X4505 otherwise).
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    // UNOFFICIAL 2026-11-04 (as FlashlightDeferred.hlsl; user: hair see-through in the flashlight): a draw that does not write the G-buffer
    // (hair, and anything else drawn over a deferred draw in the span) leaves that draw's surface behind it there, which was lit through it.
    // The pass is depth-tested at the G-buffer's surface (a little nearer, for its precision; per sample under MSAA): where something nearer
    // was drawn, it fails and the pixel is left as it is. From the stored distance alone, before the lamp loop (no temp register kept).
    {
        const float2 local = (IN.vpos - DL_View.zw + 0.5f) * DL_View.xy;
        const float3 view = DL_Fwd.xyz + DL_Right.xyz * ((local.x * 2.0f - 1.0f) * DL_Right.w) + DL_Up.xyz * ((1.0f - local.y * 2.0f) * DL_Up.w);
        const float viewZ = max(tex2Dlod(DL_Normals, uv4).b * rsqrt(dot(view, view)) * (1.0f - DL_Depth.z) - DL_Depth.w, 1.0f);	// (the ray's forward share: 1 / its length)
        depth = saturate(DL_Depth.x + DL_Depth.y / viewZ);
    }
    const float4 material0 = tex2Dlod(DL_Material, uv4);
    clip(material0.a - 1.005f);
    clip(1.995f - material0.a);
    // an actor's skin drawn over a deferred draw (sleeves, gear) keeps the gear's material flag (the skin blur owns stage 2 during skin draws):
    // the normals' actor mark (a >= 1.5; the deferred records write 1) says the pixel is not the deferred draw's any more (2026-10-31, pale skin)
    clip(1.5f - tex2Dlod(DL_Normals, uv4).a);
    const float keep = tex2Dlod(DL_Albedo, uv4).a;	// 1 - fog: the share of added light the draw's fog lets through
    clip(keep - 1.0e-4f);

    float3 ray, position; float base, count;
    DLPixel(IN, uv4, ray, position, base, count);
    SetDiffuseEnergyKeep(ShadowDecodeNormal(float2(material0.g, (material0.a - 1.01f) / 0.49f - 1.0f)), -ray, material0.b);

    float3 light = 0.0f;
    [loop] for (int k = 0; k < 63; k++) {
        [branch] if ((float)k >= count) break;
        float i, att; float3 toLamp, color;
        DLLamp((float)k, base, position, i, toLamp, att, color);
        [branch] if (att > 0.0f) {
            // the lamp's shadow first, then its light (one value live between the two): lamps 0-15 from the shadow passes (the full filter),
            // the rest looked up here with the filter this pass could be compiled with
            float shadow;
            [branch] if (k < 16 && DL_Shadow.y > 0.5f) {
                const float q = floor(((float)k + 0.5f) / 4.0f);
                const float r = (float)k - q * 4.0f;
                const float4 vis = q < 0.5f ? tex2Dlod(DL_Vis0, uv4) : q < 1.5f ? tex2Dlod(DL_Vis1, uv4) : q < 2.5f ? tex2Dlod(DL_Vis2, uv4) : tex2Dlod(DL_Vis3, uv4);
                shadow = dot(vis, float4(r < 0.5f, abs(r - 1.0f) < 0.5f, abs(r - 2.0f) < 0.5f, r > 2.5f));
            }
            else shadow = DLShadow(i, position, att, uv4);
            [branch] if (shadow > 0.0f) {
                const float4 m = tex2Dlod(DL_Material, uv4);
                const float3 N = ShadowDecodeNormal(float2(m.g, (m.a - 1.01f) / 0.49f - 1.0f));
                const float3 lit = DL_Debug.x > 0.5f && DL_Debug.x < 1.5f ? color * LampAtt(att) : getPointLightLightingAtt(toLamp, att, color, -ray, N,
                    tex2Dlod(DL_Albedo, uv4).rgb, m.b, saturate((m.r - floor(m.r)) / 0.49f));
                light += lit * shadow;
            }
        }
    }
    // the inputs as the pass sees them (written over the frame, blend off: DeferredLamps::Apply)
    // (2026-11-03 rtspec) DebugView 7: the lamps' light ALONE over the frame (the game's ambient, emittance and other light left out), in the
    // frame's encoding: the direct lamp light to set beside the reference path tracer's PTDebugView 3 (with PTReplace: the same post chain)
    // UNOFFICIAL 2026-11-06 DebugView 8: the shadow pass's codes for the pixel's lamps 0-2 (DLShadow above; 0.05 no atlas tile), black = no lamp
    [branch] if (DL_Debug.x > 7.5f) return float4(tex2Dlod(DL_Vis0, uv4).rgb, 1.0f);
    [branch] if (DL_Debug.x > 6.5f) return float4(DL_Fwd.w > 0.5f ? light * keep : pow(max(light * keep, 0.0f), DL_Table.w), 1.0f);
    [branch] if (DL_Debug.x > 1.5f) {
        const float3 cell = floor(position / 64.0f);
        const float3 N = ShadowDecodeNormal(float2(material0.g, (material0.a - 1.01f) / 0.49f - 1.0f));
        const float3 dbg = DL_Debug.x < 2.5f ? float3(frac((cell.x + cell.y + cell.z) * 0.5f) * 0.6f + 0.2f, saturate(length(position) / 4096.0f), 0.0f) :
            DL_Debug.x < 3.5f ? N * 0.5f + 0.5f : DL_Debug.x < 4.5f ? keep.xxx : (count / 31.0f).xxx;
        return float4(dbg, 1.0f);
    }
    light *= keep;
    [branch] if (DL_Fwd.w > 0.5f) return float4(light, 0.0f);	// linear frame: ONE / ONE add, exact per sample
    // the frame left in the stored curve: enc(dec(v) + L). UNOFFICIAL 2026-11-04 (as FlashlightDeferred.hlsl): ADDED as the difference it
    // makes (ONE / ONE), not written over the pixel: written, every sample of a multisampled pixel took the resolved copy's one value
    // (alpha-to-coverage edges and terrain blending's per-sample blend flattened, even where the light was nothing); added, each keeps its own.
    const float3 stored = max(tex2Dlod(DL_Frame, float4(IN.uv, 0.0f, 0.0f)).rgb, 0.0f);
    return float4(max(pow(pow(stored, DL_Table.z) + light, DL_Table.w) - stored, 0.0f), 0.0f);
}
#endif
