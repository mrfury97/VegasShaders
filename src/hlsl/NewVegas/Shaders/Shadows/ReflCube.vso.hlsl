// UNOFFICIAL 2026-11-24: Tier 2 360-degree dynamic world cubemap vertex shader (ShadowManager::RenderReflectionCubeFace).
// Matches ShadowMap.vso.hlsl's register layout so all five RenderPass caster passes work without register changes.
#include "../includes/Speedtree.hlsl"

uniform float4 TESR_PlantLeafWind  : register(c131);
uniform float4 TESR_PlantLeafWind2 : register(c132);
uniform float4 TESR_PlantLeafWind3 : register(c133);
uniform float4 TESR_CameraPosition : register(c134);
#include "../includes/PlantWind.hlsl"

uniform row_major float4x4 TESR_ShadowWorldTransform : register(c0);
uniform row_major float4x4 TESR_ShadowViewProjTransform : register(c4);
uniform float4 TESR_ShadowData : register(c8);
uniform float4 Bones[122] : register(c9);
#define BillboardRight Bones[54]   // c63
#define BillboardUp Bones[55]      // c64
#define RockParams Bones[56]       // c65
#define RustleParams Bones[57]     // c66
#define WIND_MATRICES_BASE 58      // c67, 16 rows
#define LEAF_BASE_BASE 74          // c83, 48 rows

uniform row_major float4x4 WorldTranspose : register(c140);
uniform float4 HighDetailRange : register(c144);
uniform float4 LODLandParams : register(c145);

#define weight(vv) dot(vv, 1)

struct VtxAttrs {
    float4 f_position : POSITION;
    float3 f_normal : NORMAL;
    float4 f_texcoord_0 : TEXCOORD0;
    float4 f_texcoord_1 : TEXCOORD1;
    float4 f_blendweight : BLENDWEIGHT;
    float4 f_blendindexes : BLENDINDICES;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float4 worldN_dist : TEXCOORD0;
    float4 uv : TEXCOORD1;
    float4 relPos_kind : TEXCOORD2;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;
    const float4 loc_const_0 = { 1.0f, 765.01001f, 0.0f, 0.0f };

    float4 tmp_r0 = float4(va.f_position.xyz, 1.0f);
    float3 worldN = va.f_normal;
    float plant_lean = 0.0f;

    if (TESR_ShadowData.x == 1.0f) { // Skinned (Actors)
        float4 ofs = va.f_blendindexes.zyxw * loc_const_0.y;
        float4 tmp_q0 = (va.f_position.xyzx * loc_const_0.xxxz) + loc_const_0.zzzx;
        float3 tmp_q8 = mul(float3x4(Bones[0 + ofs.w], Bones[1 + ofs.w], Bones[2 + ofs.w]), tmp_q0);
        float3 tmp_q6 = mul(float3x4(Bones[0 + ofs.z], Bones[1 + ofs.z], Bones[2 + ofs.z]), tmp_q0);
        float3 tmp_q5 = mul(float3x4(Bones[0 + ofs.x], Bones[1 + ofs.x], Bones[2 + ofs.x]), tmp_q0);
        float3 tmp_q4 = mul(float3x4(Bones[0 + ofs.y], Bones[1 + ofs.y], Bones[2 + ofs.y]), tmp_q0);
        float3 tmp_q7 = (va.f_blendweight.z * tmp_q6) + ((va.f_blendweight.x * tmp_q5) + (tmp_q4 * va.f_blendweight.y));
        tmp_r0.xyz = ((1.0f - weight(va.f_blendweight.xyz)) * tmp_q8) + tmp_q7;
        tmp_r0.w = 1.0f;

        float4 nw_v = float4(va.f_normal, 0.0f);
        float3 tmp_n8 = mul(float3x4(Bones[0 + ofs.w], Bones[1 + ofs.w], Bones[2 + ofs.w]), nw_v);
        float3 tmp_n6 = mul(float3x4(Bones[0 + ofs.z], Bones[1 + ofs.z], Bones[2 + ofs.z]), nw_v);
        float3 tmp_n5 = mul(float3x4(Bones[0 + ofs.x], Bones[1 + ofs.x], Bones[2 + ofs.x]), nw_v);
        float3 tmp_n4 = mul(float3x4(Bones[0 + ofs.y], Bones[1 + ofs.y], Bones[2 + ofs.y]), nw_v);
        worldN = ((1.0f - weight(va.f_blendweight.xyz)) * tmp_n8) + (va.f_blendweight.z * tmp_n6) + (va.f_blendweight.x * tmp_n5) + (tmp_n4 * va.f_blendweight.y);
    }
    else if (TESR_ShadowData.x == 2.0f) { // Leaves (SpeedTree)
        float4 rock_params = RockParams, rustle_params = RustleParams;
        [branch] if (TESR_PlantLeafWind.w > 0.0f) {
            const float2 base_col = float2(TESR_ShadowWorldTransform._m30, TESR_ShadowWorldTransform._m31) + TESR_CameraPosition.xy;
            const float gust_f = PlantLeafGust(base_col, TESR_PlantLeafWind.xy, TESR_PlantLeafWind.z);
            const float boost_f = 1.0f + TESR_PlantLeafWind2.w * gust_f;
            rock_params.x *= boost_f;
            rustle_params.x *= boost_f;
            const float rise_f = saturate(va.f_position.z * TESR_PlantLeafWind2.z);
            plant_lean = TESR_PlantLeafWind2.y * gust_f * rise_f * rise_f;
        }
        tmp_r0 = SpeedTreeTransform(BillboardUp, BillboardRight, rock_params, rustle_params, Bones, WIND_MATRICES_BASE, LEAF_BASE_BASE, va.f_blendindexes, tmp_r0);
        worldN = float3(0.0f, 0.0f, 1.0f);
    }
    else if (TESR_ShadowData.x == 3.0f) { // Terrain LOD
        float4 tmp_r1 = float4(va.f_position.xyz, 1.0f);
        tmp_r1.z = lerp(va.f_texcoord_1.x, va.f_position.z, LODLandParams.x);

        float tmp_q0 = (abs(dot(WorldTranspose[1], tmp_r1) - HighDetailRange.y) < HighDetailRange.w ? 1.0f : 0.0f);
        float tmp_q1 = (abs(dot(WorldTranspose[0], tmp_r1) - HighDetailRange.x) < HighDetailRange.z ? 1.0f : 0.0f);

        tmp_r0.z = tmp_r1.z - ((tmp_q0 * tmp_q1) * LODLandParams.y);
    }

#if PLANT_WIND_ON
    [branch] if (TESR_ShadowData.x == 0.0f && PlantA.x < -0.5f) tmp_r0.xyz += PlantWindMove(tmp_r0.xyz);
#endif

    if (TESR_ShadowData.x != 1.0f) {
        tmp_r0 = mul(tmp_r0, TESR_ShadowWorldTransform);
        if (TESR_ShadowData.x != 2.0f)
            worldN = mul(worldN, (float3x3)TESR_ShadowWorldTransform);
    }
    [branch] if (plant_lean != 0.0f) tmp_r0.xy += TESR_PlantLeafWind3.xy * plant_lean;

    float3 relPos = tmp_r0.xyz;
    vr.f_position = mul(tmp_r0, TESR_ShadowViewProjTransform);
    vr.worldN_dist = float4(worldN, length(relPos));
    vr.uv = va.f_texcoord_0;
    vr.relPos_kind = float4(relPos, TESR_ShadowData.x);
    return vr;
}
