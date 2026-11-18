// UNOFFICIAL 2026-11-15 DeferredTransparents ([Main.Main.Quality] DeferredTransparents, restart, test only; core/DeferredTransparents.h;
// docs/next/FD-GLASS-RESULTS.md; plan phase 5): forward+ for the blended (alpha) draws that stay forward under the deferred renderer (glass,
// bottles, windows, light shades). ObjectTemplate.hlsl's LAMPS_DEFERRED 4 record (a value of the existing define: the define list is near its
// cap) compiles the game's lamps of the draw out, as LAMPS_DEFERRED 3 does, and adds instead EVERY lamp of the pixel's froxel list from the
// frame's lamp table (core/ManyLamps.h BuildTable, the same table and lists Shaders/DeferredLamps.hlsl reads), each shadowed from its tile in
// the shadow atlas (LightCommon.hlsl TableLampShadow: the four-tap Medium filter). LAMPS_DEFERRED 5: the same without the shadow lookups (the
// fallback where 4 does not compile). No object pixel constant is added: the frame's camera, view axes and froxel grid sit in five texels at
// the right end of the table's second tile row (core/DeferredTransparents.h PutHeader: that row holds one texel a lamp, 1024 lamps, so its
// texels 1024-2047 were unused); the table is on s3 and the atlas on s8, bound by the hook (DeferredTransparents::OnBind), with c180 / c181
// (the lamp shadows' own registers). Lighting is done in camera-relative world space (the table's frame), with the bumped normal through the
// screen-derivative frame (CapturedNormals.hlsl CapturedDerivNormal, G-buffer v2) or the facing normal without it. The light model is the
// draw's own: getPointLightLightingAtt (Object.hlsl), so PBR with a highlight in the SPECULAR variants, PBRDiffuse in the others.
// Only included by the LAMPS_DEFERRED 4 / 5 records: every other record compiles exactly as before.
#ifndef NVR_FORWARD_PLUS_HLSL
#define NVR_FORWARD_PLUS_HLSL

// The table's layout as ManyLamps.h makes it with the froxel lists and the tile rows (DeferredLamps, which the deferred renderer forces):
// row 0 the lamps, then 7,680 froxels (40 x 24 tiles x 8 slices) x LIGHT_COMMON_LIST_TEXELS texels at 2,048 a row, then the two tile rows.
// Where the many-lamp block already set these (the fork's MANY_LAMPS records), its values are the same numbers (ManyLamps::TableRowsNow).
#ifndef LIGHT_COMMON_LIST_TEXELS
    #define LIGHT_COMMON_LIST_TEXELS 8.0f
#endif
#define FP_TILE_ROW (1.0f + 7680.0f * LIGHT_COMMON_LIST_TEXELS / 2048.0f)
#ifndef LIGHT_COMMON_ROWS
    #ifdef LIGHT_COMMON_ROWS_N
        #define LIGHT_COMMON_ROWS LIGHT_COMMON_ROWS_N	// (the very tokens ObjectTemplate.hlsl's many-lamp block defines it with, after this include)
    #else
        #define LIGHT_COMMON_ROWS (FP_TILE_ROW + 2.0f)
    #endif
#endif
#ifndef TABLE_LAMP_SHADOWS
    #define TABLE_LAMP_SHADOWS FP_TILE_ROW
#endif
#ifdef __INTELLISENSE__
    #include "LightCommon.hlsl"
#else
    #include "includes/LightCommon.hlsl"
#endif

#ifndef FP_MAX_LAMPS
    #define FP_MAX_LAMPS 63		// a froxel list holds 63 lamps with 16 texels (DeferredLamps), 31 with 8
#endif

// UNOFFICIAL 2026-11-16 fd-fwdgroups (docs/next/FD-FORWARD-GROUPS.md): one froxel walk for every group the lamp pass cannot reach (glass,
// the game's blended hair SM3002 / SM3003, the deferred leaves). The includer may define, before this include:
//   FP_LAMP_TERM(toLamp, att, color, V, n, albedo, rough, metal)  one lamp's light (default: the object model, getPointLightLightingAtt)
//   FP_ATT(toLamp, reach)                                         the falloff (default: vanillaAtt, Pointlights.hlsl via Object.hlsl)
//   FP_OWN_SHADOW 1 + FP_ATLAS_REG sN                             a shader without PointShadowForward.hlsl's INTERIOR_SHADOWS block: the
//                                                                 atlas tile lookup declared here (c180 / c181 as the lamp shadows' own)
//   FP_NO_BEAM                                                    no flashlight term (the includer lights the beam itself: the hair)
// The object records (glass) define none of them and compile as before, plus the flashlight (below).
#ifndef FP_LAMP_TERM
    #define FP_LAMP_TERM(toLamp, att, color, V, n, albedo, rough, metal) getPointLightLightingAtt(toLamp, att, color, V, n, albedo, rough, metal)
#endif
#ifndef FP_ATT
    #define FP_ATT(toLamp, reach) vanillaAtt(toLamp, reach)
#endif
#if defined(FP_OWN_SHADOW) && FP_OWN_SHADOW
    #ifndef FP_ATLAS_REG
        #define FP_ATLAS_REG s12
    #endif
    float4 FP_ShadowParams : register(c180);	// as NVR_PointShadowParams: x strength, y depth bias, z normal offset, w the taps' spread
    float4 FP_ShadowMore   : register(c181);	// as NVR_PointShadowDebug: y NearFade (world units, 0 off)
    sampler2D FP_ShadowAtlas : register(FP_ATLAS_REG);	// linear, clamp (the DLL binds the lamp shadow atlas here)
    float2 FPOct(float3 v) {
        const float3 n = v / (abs(v.x) + abs(v.y) + abs(v.z));
        const float2 sgn = float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        const float2 o = n.z < 0.0f ? (1.0f - abs(n.yx)) * sgn : n.xy;
        return o * 0.5f + 0.5f;
    }
    float FPAtlasTap(float4 tile, float3 v, float threshold, float lightW) {
        const float t = tex2Dlod(FP_ShadowAtlas, float4(tile.xy + FPOct(v) * tile.z, 0.0f, 0.0f)).r;
        const float nearFade = FP_ShadowMore.y > 0.0f ? smoothstep(0.0f, FP_ShadowMore.y * lightW, t) : 1.0f;
        return (float)(t <= threshold) * (float)(t > 0.0f) * nearFade;
    }
    // LightCommon.hlsl TableLampShadow's lookup (the four-tap Medium filter, NearFade, strength and fade), line for line, on this atlas stage
    float FPTableShadow(float i, float3 worldPos, float3 normal) {
        [branch] if (!(FP_ShadowParams.x > 0.0f)) return 1.0f;
        const float x = (i * 2.0f + 0.5f) / LIGHT_COMMON_TEXELS;
        const float y = (TABLE_LAMP_SHADOWS + 0.5f) / LIGHT_COMMON_ROWS;
        const float4 tile = tex2Dlod(NVR_LampTable, float4(x, y, 0.0f, 0.0f));
        [branch] if (!(tile.z > 0.0f)) return 1.0f;
        const float4 light = tex2Dlod(NVR_LampTable, float4(x + 1.0f / LIGHT_COMMON_TEXELS, y, 0.0f, 0.0f));
        float3 v = worldPos - light.xyz;
        const float len = length(v);
        [branch] if (!(len * light.w < 1.0f)) return 1.0f;
        v = (v + normal * (FP_ShadowParams.z * len)) * float3(1.0f, 1.0f, -1.0f);
        const float d = length(v) * light.w;
        const float threshold = d - FP_ShadowParams.y * d;
        const float s = FP_ShadowParams.w * max(abs(v.x), max(abs(v.y), abs(v.z)));
        float blocked = FPAtlasTap(tile, v + s * float3( 1.0f,  1.0f,  1.0f), threshold, light.w);
        blocked += FPAtlasTap(tile, v + s * float3(-1.0f, -1.0f,  1.0f), threshold, light.w);
        blocked += FPAtlasTap(tile, v + s * float3( 1.0f, -1.0f, -1.0f), threshold, light.w);
        blocked += FPAtlasTap(tile, v + s * float3(-1.0f,  1.0f, -1.0f), threshold, light.w);
        return lerp(1.0f, 1.0f - 0.25f * blocked, FP_ShadowParams.x * abs(tile.w));
    }
    #define FP_SHADOW(i, p, n) FPTableShadow(i, p, n)
#elif defined(TABLE_LAMP_SHADOWS_ON) && !(defined(LAMPS_DEFERRED) && LAMPS_DEFERRED == 5)
    #define FP_SHADOW(i, p, n) TableLampShadow(i, p, n, 1.0f)
#else
    #define FP_SHADOW(i, p, n) 1.0f
#endif

// The header (DeferredTransparents::PutHeader), texel 2047 - k of the second tile row:
//   k 0: the camera's world position (xyz), w 1 = the froxel lists are this frame's world view (0: no lamp is added)
//   k 1: the view's forward axis (xyz), w tan(half fov x)
//   k 2: the view's right axis (xyz), w tan(half fov y)
//   k 3: the view's up axis (xyz), w unused;   k 4: xy the lists' viewport in 64-pixel tiles (fractional), z tiles across, w tiles down
//   (2026-11-16) k 5: the flashlight, camera-relative xyz, w its reach (0: off); k 6: its axis, w cos of the cone's outer edge; k 7: rgb its
//   light (linear, before the lamps' light scale, as a table lamp's colour), w cos of the full-strength edge (core/ForwardPlusHeader.h)
float4 FPHeader(float k) {
    return tex2Dlod(NVR_LampTable, float4((LIGHT_COMMON_TEXELS - 0.5f - k) / LIGHT_COMMON_TEXELS, (TABLE_LAMP_SHADOWS + 1.5f) / LIGHT_COMMON_ROWS, 0.0f, 0.0f));
}

// Every lamp of the pixel's froxel on the pixel. p: camera-relative world position (GetShadowWorldPos's), n: the shading normal in world axes
// (unit; also the shadow lookup's normal offset: with the facing geometric normal kept apart for it, SLS2034 / 2036 ran out of temp registers,
// X4505), valid: 1 where an NVR vertex shader wrote p.
// albedo, rough, metal: the draw's own inputs to its lamp terms. The froxel is found as ManyLamps::BinFroxels binned it: the screen share
// from the view axes (no VPOS: the draws keep their signatures), the slice from the view depth (slice 0 nearer than 64, slice k 2^(k+5)..).
float3 FPLampSum(float3 p, float3 n, float valid, float3 albedo, float rough, float metal) {
    const float4 cam = FPHeader(0.0f);
    [branch] if (!(cam.w > 0.5f) || !(valid > 0.0f)) return 0.0f;
    const float4 fwd = FPHeader(1.0f);
    const float4 right = FPHeader(2.0f);
    const float4 up = FPHeader(3.0f);
    const float4 grid = FPHeader(4.0f);
    const float viewZ = dot(p, fwd.xyz);
    [branch] if (!(viewZ > 0.0f)) return 0.0f;
    const float2 local = float2(0.5f + 0.5f * dot(p, right.xyz) / (viewZ * fwd.w), 0.5f - 0.5f * dot(p, up.xyz) / (viewZ * right.w));
    const float2 tile = min(floor(saturate(local) * grid.xy), grid.zw - 1.0f);	// (as LampTableCore::Rect: the share x the viewport / 64)
    const float slice = viewZ < 64.0f ? 0.0f : min(7.0f, floor(log2(viewZ)) - 5.0f);
    const float base = (slice * grid.z * grid.w + tile.y * grid.z + tile.x) * LIGHT_COMMON_LIST_TEXELS;
    const float count = LightCommonListTexel(base).x;
    const float3 q = cam.xyz + p;	// the pixel in world space (one value live across the loop, not the camera and p apart: temp registers)
    float3 sum = 0.0f;
    // UNOFFICIAL 2026-11-17 glassglow (docs/next/FD-GLASSGLOW.md): the list's length is the loop's own bound. It was "for k < 63 { if (k >=
    // count) break; ... }", which D3DX compiled in 20 of the 216 forward-group records (the indoor glass records of SLS2017-2027, 2034 and
    // 2036, and the hair's SM3002; fork and native) as "if_ge k, count / break_ne -c6.x, -c6.x / endif" with c6.x = -1: 1 != 1, a break never
    // taken (where D3DX picks a positive constant the same idiom works: break_ne c.x, -c.x). The walk then went on past the list into entries
    // that are no lamps of the froxel (the last texel's zero padding reads as table lamp 0; the texels after it are not written this frame),
    // and pale glass took lamp light several times over (the "pewter" bottles: the walk reached 7+ lamps where the froxel lists 4, 3 of them
    // reaching). A bound in the loop condition compiles to a compare of two registers (break_ge k, n), with no constant to get wrong. Hair
    // (SM3002 / SM3003) and the deferred leaves walk here too.
    const int lamps = (int)min(count, (float)FP_MAX_LAMPS);
    [loop] for (int k = 0; k < lamps; k++) {
        const float e = (float)k + 1.0f;
        const float t = floor((e + 0.5f) / 4.0f);
        const float c = e - t * 4.0f;
        const float i = dot(LightCommonListTexel(base + t), float4(c < 0.5f, abs(c - 1.0f) < 0.5f, abs(c - 2.0f) < 0.5f, c > 2.5f));
        const float x = (i * 2.0f + 0.5f) / LIGHT_COMMON_TEXELS;
        const float4 lamp = tex2Dlod(NVR_LampTable, float4(x, 0.5f / LIGHT_COMMON_ROWS, 0.0f, 0.0f));
        const float3 toLamp = lamp.xyz - q;
        const float att = lamp.w > 0.0f ? FP_ATT(toLamp, max(lamp.w, 1.0e-3f)) : 0.0f;
        [branch] if (att > 0.0f) {
            const float shadow = min(FP_SHADOW(i, p, n), 1.0f);
            [branch] if (shadow > 0.0f) {
                const float3 color = tex2Dlod(NVR_LampTable, float4(x + 1.0f / LIGHT_COMMON_TEXELS, 0.5f / LIGHT_COMMON_ROWS, 0.0f, 0.0f)).rgb;
                sum += FP_LAMP_TERM(toLamp, att, color * shadow, -p * rsqrt(max(dot(p, p), 1.0e-6f)), n, albedo, rough, metal);	// (the view vector made here: a temp less across the loop)
            }
        }
    }
#ifndef FP_NO_BEAM
    // (2026-11-16) the flashlight: the table leaves its game lamp out (the flashlight pass lights the G-buffer) and this record compiles the
    // draw's own lamps out, so glass lost the flashlight entirely. Its cone and reach from the header (no shadow: the game lamp had none)
    const float4 beam = FPHeader(5.0f);
    [branch] if (beam.w > 0.0f) {
        const float4 axis = FPHeader(6.0f);
        const float4 beamLight = FPHeader(7.0f);
        const float3 toBeam = beam.xyz - p;
        const float cone = smoothstep(axis.w, beamLight.w, dot(-toBeam, axis.xyz) * rsqrt(max(dot(toBeam, toBeam), 1.0e-6f)));
        const float att = FP_ATT(toBeam, max(beam.w, 1.0e-3f)) * cone;
        [branch] if (att > 0.0f) sum += FP_LAMP_TERM(toBeam, att, beamLight.rgb, -p * rsqrt(max(dot(p, p), 1.0e-6f)), n, albedo, rough, metal);
    }
#endif
    return sum;
}

// The shading normal for FPLampSum: n made unit (the template's G-buffer v2 shading normal), the facing normal where n is none (0: a vanilla
// vertex shader ran, or the derivative frame degenerated).
float3 FPUnit(float3 n, float3 fallback) {
    const float l2 = dot(n, n);
    return l2 > 1.0e-12f ? n * rsqrt(l2) : fallback * rsqrt(max(dot(fallback, fallback), 1.0e-20f));
}

#endif
