// SpeedTree leaf VS -- BSSM_LEAVES_AD. Transcription of vanilla STLEAF001.vso.
// vs_2_0 -> vs_3_0 to pair with the ps_3_0 shadow lookup.
// Engine uploads stop at c81 (vanilla defs sit at c82+), so Shadow.hlsl's c100/c104 are clear.
// Added: TEXCOORD3 sun-only, TEXCOORD4 shadowWorldPos. TEXCOORD1 stays vanilla's combined
// lighting, so the PS matches vanilla at shadow = 1.
#include "includes/Shadow.hlsl"

// UNOFFICIAL 2026-11-16 fd-fwdgroups DeferredFoliageLamps (Includes/Leaf.hlsl LEAF_FORWARD_PLUS): with G-buffer v2 compiled in (the deferred
// renderer on), two more interpolators for the deferred leaf record that lights the leaf with every lamp of its froxel: the bent normal in
// the world (TEXCOORD5) and this shader's own point lamp's light apart (TEXCOORD6, so the record takes it out before adding the table's).
// The forward records read neither. Without v2: as before, byte for byte.
#if defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 4
    #define LEAF_VS_LAMPS 1
#else
    #define LEAF_VS_LAMPS 0
#endif

uniform row_major float4x4 ModelViewProj : register(c0);
uniform float4 AmbientColor  : register(c5);
uniform float4 DiffuseColor  : register(c6);
uniform float4 FogParam      : register(c8);   // x: start, y: range, z: power
uniform float4 FogColor      : register(c9);
uniform float4 SunScale      : register(c10);  // .x
uniform float4 SunDir        : register(c11);
uniform float4 CameraRight   : register(c13);
uniform float4 CameraUp      : register(c14);
uniform float4 WindA         : register(c15);
uniform float4 WindB         : register(c16);
uniform float4 NormalAdjust  : register(c17);  // .y

uniform float4 BranchRows[16] : register(c18);  // 4 matrices, row stride 1, matrix stride a0.y
uniform float4 LeafData[48]   : register(c34);
// UNOFFICIAL: backlit translucency settings (PBR.cpp, [Shaders.Foliage.Main]). c82 is the first register past LeafData; the
// engine's uploads stop at c81, and the compiler keeps its literal constants off this one.
uniform float4 TESR_Translucency : register(c82);
// UNOFFICIAL 2026-10-18 sandport: the sandstorm's wind on the leaves (SandstormEffect, TESR_LeafStorm; ported from
// Physically-Based-Reloaded's TESR_WindBoost, Balebandro / Alejandro Martinez, GPL-3.0): x the swing scale (1 no storm),
// y the flutter amount (0 no storm), z the flutter phase (rad). c134: past the shadow constants (c100-c133) and the
// translucency (c82); the compiler's literals sit low (c4, c7, c12) or just past c82.
uniform float4 TESR_LeafStorm : register(c134);
// UNOFFICIAL 2026-10-20 plant wind ([Shaders.Plants.Wind], core/PlantWind.h, Includes/PlantWind.hlsl): the grass field wind's gusts
// on the leaves (the same direction, clock and gust pattern). c135-c142: past the storm's c134, below the sun cross-fade's c146.
uniform float4 TESR_PlantLeafWind  : register(c135);
uniform float4 TESR_PlantLeafWind2 : register(c136);
uniform float4 TESR_PlantLeafWind3 : register(c137);
uniform float4 TESR_CameraPosition : register(c138);
uniform row_major float4x4 TESR_ViewProjectionTransform : register(c139);
#include "includes/PlantWind.hlsl"

struct VtxAttrs {
    float4 f_position     : POSITION;
    float3 f_normal       : NORMAL;
    float2 f_uv           : TEXCOORD0;
    float4 f_blend_indices : BLENDINDICES;   // x: branch blend, y: matrix idx, z: leaf idx + scale, w: corner
};

struct VtxVaryings {
    float4 f_position       : POSITION;
    float2 f_uv             : TEXCOORD0;
    float4 f_lighting       : TEXCOORD1;   // vanilla combined
    float4 f_fog            : TEXCOORD2;   // rgb colour, w amount
    float3 f_sun            : TEXCOORD3;   // shadowable part of .lighting
    float4 f_shadow_world_pos : TEXCOORD4;
#if LEAF_VS_LAMPS
    float4 f_bent_world     : TEXCOORD5;   // UNOFFICIAL 2026-11-16 the bent normal, camera-relative world axes (not unit)
    float4 f_point          : TEXCOORD6;   // UNOFFICIAL 2026-11-16 rgb: the point lamp's share of .lighting (0 without one)
#endif
};

// Wrap to -pi..pi (mad/frc/mad).
float WrapPi(float vv, float scl) {
    return frac(vv * scl + 0.5f) * 6.28318548f - 3.14159274f;
}

// UNOFFICIAL: backlit translucency (docs/research/foliage-translucency.md). Light that reaches the eye through a blade
// from the far side: the surface faces away from the sun (back: N against Lback), the eye looks into the sun through it
// (align), and the sun is above the horizon (gate). p is the camera-relative world position, Lsun the world sun direction.
float FoliageTranslucency(float3 N, float3 lback, float3 pt, float3 lsun) {
    float3 V = -pt / max(length(pt), 1e-6f);
    float back_lit = saturate(-dot(N, lback));
    float align_v = pow(saturate(dot(V, -lsun)), TESR_Translucency.y);
    float gate_f = saturate(lsun.z * TESR_Translucency.z);
    return TESR_Translucency.x * back_lit * align_v * gate_f;
}

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    float4 tmp_v0 = va.f_position;
    float4 tmp_v3 = va.f_blend_indices;

    float aa = WrapPi(tmp_v3.z * 0.020833334f + WindB.y, 0.499999583f);
    float b_storm = WrapPi(tmp_v3.z * 0.020833334f + WindA.y, 0.499999583f);
    float swing_a = sin(aa) * WindB.z * WindB.x;
    float swing_b = sin(b_storm) * WindA.z * WindA.x;
    // UNOFFICIAL 2026-10-18 sandport: in a sandstorm the leaves swing harder and flutter fast. With no storm (y 0) the
    // branch is not taken and this is the game's motion exactly.
    [branch] if (TESR_LeafStorm.y > 0.0f) {
        float flutter_f = TESR_LeafStorm.y * sin(TESR_LeafStorm.z + tmp_v3.z * 1.7f);
        swing_a = swing_a * TESR_LeafStorm.x + flutter_f;
        swing_b = swing_b * TESR_LeafStorm.x + flutter_f * 0.7f;
    }
    // UNOFFICIAL 2026-10-20 plant wind (Includes/PlantWind.hlsl): as a gust of the grass field wind passes the bush, its crest
    // strengthens the game's swing and the leaves flutter (as the storm above), and with a lean set they lean downwind, more the
    // higher they sit. The gust is read at the bush's base (its model origin, through ModelViewProj and back to the world): no
    // per-tree constant reaches this shader. Off (TESR_PlantLeafWind.w 0) the branch is not taken: the game's motion exactly.
    float leaf_lean = 0.0f;
    [branch] if (TESR_PlantLeafWind.w > 0.0f) {
        const float2 base_col = GetShadowWorldPos(float4(ModelViewProj._m03, ModelViewProj._m13, ModelViewProj._m23, ModelViewProj._m33)).xy + TESR_CameraPosition.xy;
        const float gust_f = PlantLeafGust(base_col, TESR_PlantLeafWind.xy, TESR_PlantLeafWind.z);
        const float flutter_f = TESR_PlantLeafWind2.x * sin(2.0f * TESR_PlantLeafWind.z + tmp_v3.z * 1.7f + (base_col.x - base_col.y) * 0.0078125f);
        const float boost_f = 1.0f + TESR_PlantLeafWind2.w * gust_f;
        swing_a = swing_a * boost_f + flutter_f;
        swing_b = swing_b * boost_f + flutter_f * 0.7f;
        const float rise_f = saturate(tmp_v0.z * TESR_PlantLeafWind2.z);
        leaf_lean = TESR_PlantLeafWind2.y * gust_f * rise_f * rise_f;
    }
    aa = WrapPi(swing_a, 0.159154937f);
    // xy rotation of the camera basis. Identity at zero wind: offset = p*Right + q*Up.
    float3 loc_axis1 = float3(cos(aa), -sin(aa), 0.0f);
    float3 loc_tan1  = float3(sin(aa),  cos(aa), 0.0f);

    float bb = WrapPi(swing_b, 0.159154937f);
    float3 loc_axis2 = float3(0.0f, cos(bb), -sin(bb));
    float3 loc_tan2  = float3(0.0f, sin(bb), cos(bb));

    int loc_a0x = (int)(tmp_v3.z - frac(tmp_v3.z));
    int loc_a0y = (int)(tmp_v3.y - frac(tmp_v3.y));

    float4 leaf_v = tmp_v3.w * LeafData[loc_a0x];

    float tmp_d1 = dot(loc_axis1, CameraRight.xyz);
    float tmp_d2 = dot(loc_axis1, CameraUp.xyz);

    float pt = dot(loc_axis2, leaf_v.xyz);
    float4 tmp_r5;
    tmp_r5.x = tmp_d1 * pt;
    tmp_r5.y = pt * dot(loc_tan1, CameraRight.xyz);
    tmp_r5.zw = pt * CameraRight.zw;

    float qq = dot(loc_tan2, leaf_v.xyz);
    float4 tmp_r1;
    tmp_r1.x = tmp_d2 * qq;
    tmp_r1.y = dot(loc_tan1, CameraUp.xyz) * qq;
    tmp_r1.zw = qq * CameraUp.zw;

    float4 local_p = tmp_r5 + tmp_r1 + tmp_v0;

    float4 loc_skinned;
    loc_skinned.x = dot(BranchRows[loc_a0y + 0], local_p);
    loc_skinned.y = dot(BranchRows[loc_a0y + 1], local_p);
    loc_skinned.z = dot(BranchRows[loc_a0y + 2], local_p);
    loc_skinned.w = dot(BranchRows[loc_a0y + 3], local_p);

    float4 pos_w = lerp(local_p, loc_skinned, tmp_v3.x);

    // Nudged along the leaf dir. 4-component normalise (dp4 r4, r4).
    float3 leaf_dir = leaf_v.xyz * rsqrt(dot(leaf_v, leaf_v));
    float3 N = normalize(leaf_dir * NormalAdjust.y + va.f_normal);

    float n_dot_l = saturate(dot(N, SunDir.xyz));
    float light_scale = frac(tmp_v3.z);

    vr.f_position = mul(ModelViewProj, pos_w);
    // UNOFFICIAL 2026-10-20 plant wind: the lean, downwind in the world (the shadow position below follows it)
    [branch] if (leaf_lean != 0.0f) vr.f_position += mul(float4(TESR_PlantLeafWind3.xy * leaf_lean, 0.0f, 0.0f), TESR_ViewProjectionTransform);

    // UNOFFICIAL: backlit translucency joins the sun term, so TEXCOORD3 (sun) and TEXCOORD1 (lighting) both carry it and the
    // PS shadow takes it out in shade. N and SunDir are model space, as NdotL; the eye and TESR_SmoothedSunDir are world
    // space. Indoors (TESR_ShadowFormatData.z is 0) it is off. Skipped at Translucency 0.
    float3 shadow_world_pos = GetShadowWorldPos(vr.f_position);
    [branch] if (TESR_Translucency.x > 0.0f) {
        n_dot_l += FoliageTranslucency(N, SunDir.xyz, shadow_world_pos, TESR_SmoothedSunDir.xyz) * TESR_ShadowFormatData.z;
    }

    float3 sun_rgb = light_scale * (n_dot_l * DiffuseColor.rgb * SunScale.x);
    vr.f_sun = sun_rgb;
    vr.f_lighting = float4(sun_rgb + light_scale * AmbientColor.rgb, 1.0f);

    // Fog distance is (x, y, w-z), not xyz.
    float3 fog_vec = float3(vr.f_position.x, vr.f_position.y, vr.f_position.w - vr.f_position.z);
    float fog_dist = length(fog_vec);
    float tt = saturate((FogParam.x - fog_dist) / FogParam.y);
    vr.f_fog.rgb = FogColor.rgb;
    vr.f_fog.w = pow(1.0f - tt, FogParam.z);

    vr.f_uv = va.f_uv;
    vr.f_shadow_world_pos = float4(shadow_world_pos, SHADOW_VS_SENTINEL);
#if LEAF_VS_LAMPS
    // the model-space bent normal carried into the world as the point it leads to is (exact for the leaf's affine transform)
    vr.f_bent_world = float4(GetShadowWorldPos(vr.f_position + mul(ModelViewProj, float4(N, 0.0f))) - shadow_world_pos, 0.0f);
    vr.f_point = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif

    return vr;
};
