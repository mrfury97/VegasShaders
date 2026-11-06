
#include "../includes/Speedtree.hlsl"
// UNOFFICIAL 2026-10-16 cloth wind (../includes/ClothWind.hlsl): cloth's shadow moves with it (position only).
#ifndef CLOTH_WIND_ON
    #define CLOTH_WIND_ON 0
#endif
#if CLOTH_WIND_ON
    #include "../includes/ClothWind.hlsl"
#endif
// UNOFFICIAL 2026-10-20 plant wind (../includes/PlantWind.hlsl, core/PlantWind.h): the SpeedTree leaves' shadows move as the lit leaves
// (STLEAF001/003: the same gust boost, flutter and lean), and the static plants' as the plants (PLANT_WIND_ON, c248-c253). c131-c134:
// free (LeafBase ends at c130, the land LOD's constants start at c140).
uniform float4 TESR_PlantLeafWind  : register(c131);
uniform float4 TESR_PlantLeafWind2 : register(c132);
uniform float4 TESR_PlantLeafWind3 : register(c133);
uniform float4 TESR_CameraPosition : register(c134);
#include "../includes/PlantWind.hlsl"

uniform row_major float4x4 TESR_ShadowWorldTransform : register(c0);
uniform row_major float4x4 TESR_ShadowViewProjTransform : register(c4);
uniform float4 TESR_ShadowData : register(c8);
// UNOFFICIAL 2026-11-05 (from Balebandro's fork, mo2-dxvk-loader ad32a3b) one array for the bones and the SpeedTree leaf constants
// (same registers as before: the leaves' c63-c130 are set by number, RenderPass.cpp). With Bones[54] and the leaf constants declared
// after it, a skinned partition's bones past the 18th (ghoul arms have 20 and 21) read wrong under DXVK although the registers held
// them: those vertices were thrown towards the light and drew long bars into shadow maps. As one array the bones run to c130 (40).
uniform float4 Bones[122] : register(c9);
#define BillboardRight Bones[54]	// c63
#define BillboardUp Bones[55]		// c64
#define RockParams Bones[56]		// c65
#define RustleParams Bones[57]		// c66
#define WIND_MATRICES_BASE 58		// c67, 16 rows
#define LEAF_BASE_BASE 74			// c83, 48 rows

// Terrain LOD params for hiding it under normal terrain.
uniform row_major float4x4 WorldTranspose : register(c140);
uniform float4 HighDetailRange : register(c144);
uniform float4 LODLandParams : register(c145);

#define	weight(vv)		dot(vv, 1)

#if CLOTH_WIND_ON
// cloth mask 5 (2026-10-19 round 8): a bind-pose vector through the same blend of the bones' rotations as the position (w = 0 drops the
// translation); offset / bw as in main's skinned branch
float3 ClothBonesVector(float4 ofs, float3 bw_v, float3 vv) {
	const float4 vw_v = float4(vv, 0);
	const float3 tmp_v8 = mul(float3x4(Bones[0 + ofs.w], Bones[1 + ofs.w], Bones[2 + ofs.w]), vw_v);
	const float3 tmp_v6 = mul(float3x4(Bones[0 + ofs.z], Bones[1 + ofs.z], Bones[2 + ofs.z]), vw_v);
	const float3 tmp_v5 = mul(float3x4(Bones[0 + ofs.x], Bones[1 + ofs.x], Bones[2 + ofs.x]), vw_v);
	const float3 tmp_v4 = mul(float3x4(Bones[0 + ofs.y], Bones[1 + ofs.y], Bones[2 + ofs.y]), vw_v);
	return ((1 - weight(bw_v)) * tmp_v8) + (bw_v.z * tmp_v6) + (bw_v.x * tmp_v5) + (tmp_v4 * bw_v.y);
}
#endif

struct VtxAttrs {
    float4 f_position : POSITION;
	float4 f_texcoord_0 : TEXCOORD0;
    float4 f_texcoord_1 : TEXCOORD1;  // Terrain LOD, no idea what it represents.
    float4 f_blendweight : BLENDWEIGHT;
    float4 f_blendindexes : BLENDINDICES;
#if CLOTH_WIND_ON
    float4 f_color : COLOR0;	// cloth wind mask (red)
    float3 f_normal : NORMAL;	// cloth wind (2026-10-17): the displacement has a part along the normal (billow, flutter): the shadow needs it too
    float3 f_cloth_weight : TEXCOORD7;	// cloth wind mask 5 (2026-10-19): vertex stream 1, bound only for worn cloth (read only in mask 5): weight, place on the hem ring (2026-10-20)
#endif
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float4 f_texcoord_0 : TEXCOORD0;
	float4 f_texcoord_1 : TEXCOORD1;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

	const float4 loc_const_0 = {1.0f, 765.01001f, 0.0f, 0.0f};
	const float4 loc_const_4 = {(1.0 / (PI * 2)), 0.25, 0.5, 0};
	
	float4 ofs;
    float4 tmp_q0;
	float4 tmp_q1;
	float4 tmp_q2;
	float4 tmp_q3;
    float4 tmp_q4;
    float4 tmp_q5;
    float4 tmp_q6;
    float4 tmp_q7;
    float4 tmp_q8;
	float4 tmp_q9;
	float4 tmp_q10;
	float4 tmp_q11;
	float4 tmp_q28;
	float4 tmp_q59;
	float4 tmp_r1;
	float4 tmp_r2;
	float4 tmp_r3;
	float4 tmp_r4;
	float4 tmp_r5;
	float4 tmp_r0 = va.f_position;
	float plant_lean = 0.0f;	// UNOFFICIAL 2026-10-20 plant wind: SpeedTree leaves' lean downwind (world units)
#if CLOTH_WIND_ON
	float3 cloth_normal = va.f_normal;	// the vertex's normal in the space r0 ends in (model space; skinned: the bones' space)
	float3 cloth_out = 0.0f;			// mask 5's billow direction (skinned only), carried by the same bones
	float4 cloth_leg_l = 0.0f, cloth_leg_r = 0.0f;	// mask 5 (round 8): each leg (ClothLegBind), carried by the same bones
	float3 cloth_up = 0.0f;				// ...and the bind pose's up (the legs' direction)
#endif
	
	if (TESR_ShadowData.x == 1.0f) { // Skinned (Actors)
		ofs = va.f_blendindexes.zyxw * loc_const_0.y;
		tmp_r0.w = 1;
		tmp_q0 = (va.f_position.xyzx * loc_const_0.xxxz) + loc_const_0.zzzx;
		tmp_q8.xyz = mul(float3x4(Bones[0 + ofs.w], Bones[1 + ofs.w], Bones[2 + ofs.w]), tmp_q0);
		tmp_q6.xyz = mul(float3x4(Bones[0 + ofs.z], Bones[1 + ofs.z], Bones[2 + ofs.z]), tmp_q0);
		tmp_q5.xyz = mul(float3x4(Bones[0 + ofs.x], Bones[1 + ofs.x], Bones[2 + ofs.x]), tmp_q0);
		tmp_q4.xyz = mul(float3x4(Bones[0 + ofs.y], Bones[1 + ofs.y], Bones[2 + ofs.y]), tmp_q0);
		tmp_q7.xyz = (va.f_blendweight.z * tmp_q6.xyz) + ((va.f_blendweight.x * tmp_q5.xyz) + (tmp_q4.xyz * va.f_blendweight.y));
		tmp_r0.xyz = ((1 - weight(va.f_blendweight.xyz)) * tmp_q8.xyz) + tmp_q7.xyz;
#if CLOTH_WIND_ON
		{	// the same blend of the bones' rotations for the normal (the rows' xyz; w = 0 drops the translation)
			const float4 nw_v = float4(va.f_normal, 0);
			const float3 tmp_n8 = mul(float3x4(Bones[0 + ofs.w], Bones[1 + ofs.w], Bones[2 + ofs.w]), nw_v);
			const float3 tmp_n6 = mul(float3x4(Bones[0 + ofs.z], Bones[1 + ofs.z], Bones[2 + ofs.z]), nw_v);
			const float3 tmp_n5 = mul(float3x4(Bones[0 + ofs.x], Bones[1 + ofs.x], Bones[2 + ofs.x]), nw_v);
			const float3 tmp_n4 = mul(float3x4(Bones[0 + ofs.y], Bones[1 + ofs.y], Bones[2 + ofs.y]), nw_v);
			cloth_normal = ((1 - weight(va.f_blendweight.xyz)) * tmp_n8) + (va.f_blendweight.z * tmp_n6) + (va.f_blendweight.x * tmp_n5) + (tmp_n4 * va.f_blendweight.y);
		}
		[branch] if (ClothIsMask5() && ClothWalk.x > 0.5f) {	// 2026-10-20 round 9: hangs from the hips, the hem ring, the current legs
			tmp_r0.xyz = lerp(tmp_r0.xyz, ClothPelvisPoint(va.f_position.xyz), saturate(va.f_cloth_weight.x) * ClothWalk.y);
			cloth_out = ClothPelvisVector(ClothOutwardBind(va.f_position.xyz));
			cloth_up = ClothPelvisVector(float3(0.0f, 0.0f, 1.0f));
			cloth_leg_l = ClothLegBind(tmp_r0.xyz, ClothLegNow[0], ClothLegNow[1], ClothLegNow[2]);
			cloth_leg_r = ClothLegBind(tmp_r0.xyz, ClothLegNow[3], ClothLegNow[4], ClothLegNow[5]);
		}
		else [branch] if (ClothIsMask5()) {	// mask 5 (2026-10-19): the same for the direction away from the wearer's upright axis
			const float4 ow_v = float4(ClothOutwardBind(va.f_position.xyz), 0);
			const float3 tmp_o8 = mul(float3x4(Bones[0 + ofs.w], Bones[1 + ofs.w], Bones[2 + ofs.w]), ow_v);
			const float3 tmp_o6 = mul(float3x4(Bones[0 + ofs.z], Bones[1 + ofs.z], Bones[2 + ofs.z]), ow_v);
			const float3 tmp_o5 = mul(float3x4(Bones[0 + ofs.x], Bones[1 + ofs.x], Bones[2 + ofs.x]), ow_v);
			const float3 tmp_o4 = mul(float3x4(Bones[0 + ofs.y], Bones[1 + ofs.y], Bones[2 + ofs.y]), ow_v);
			cloth_out = ((1 - weight(va.f_blendweight.xyz)) * tmp_o8) + (va.f_blendweight.z * tmp_o6) + (va.f_blendweight.x * tmp_o5) + (tmp_o4 * va.f_blendweight.y);
			const float4 leg_l = ClothLegBind(va.f_position.xyz, ClothLimb[0], ClothLimb[1], ClothLimb[2]);
			const float4 leg_r = ClothLegBind(va.f_position.xyz, ClothLimb[4], ClothLimb[5], ClothLimb[6]);
			cloth_leg_l = float4(ClothBonesVector(ofs, va.f_blendweight.xyz, leg_l.xyz), leg_l.w);
			cloth_leg_r = float4(ClothBonesVector(ofs, va.f_blendweight.xyz, leg_r.xyz), leg_r.w);
			cloth_up = ClothBonesVector(ofs, va.f_blendweight.xyz, float3(0.0f, 0.0f, 1.0f));
		}
#endif
	}
	else if (TESR_ShadowData.x == 2.0f) { // Leaves (Speedtrees)
        // UNOFFICIAL 2026-10-20 plant wind: as the lit leaves (STLEAF001/003), a passing gust strengthens the game's swing (both swings x
        // boost: the swing is sin x params.x x params.z) and leans the leaves (plantLean, added in the world below); the gust is read at the
        // bush's base. (The lit leaves' small flutter is left out here: no way to add it through the game's parameters.)
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
    }
    else if (TESR_ShadowData.x == 3.0f) { // Terrain LOD.
        float4 tmp_r1 = va.f_position;
        tmp_r1.z = lerp(va.f_texcoord_1.x, va.f_position.z, LODLandParams.x);

        float tmp_q0 = (abs(dot(WorldTranspose[1], tmp_r1) - HighDetailRange.y) < HighDetailRange.w ? 1.0 : 0.0);
        float tmp_q1 = (abs(dot(WorldTranspose[0], tmp_r1) - HighDetailRange.x) < HighDetailRange.z ? 1.0 : 0.0);

        tmp_r0.z = tmp_r1.z - ((tmp_q0.x * tmp_q1.x) * LODLandParams.y);
    }
#if PLANT_WIND_ON
    [branch] if (TESR_ShadowData.x == 0.0f && PlantA.x < -0.5f) tmp_r0.xyz += PlantWindMove(tmp_r0.xyz);	// UNOFFICIAL 2026-10-20 plant wind (static plants)
#endif
#if CLOTH_WIND_ON
    if (TESR_ShadowData.x == 0.0f) cloth_out = float3(va.f_cloth_weight.y, 0.0f, 0.0f);	// 2026-10-20 still cloth (mask 6): the billow's side from stream 1
    if (TESR_ShadowData.x == 0.0f || TESR_ShadowData.x == 1.0f) tmp_r0.xyz = ClothWindPosition(tmp_r0.xyz, cloth_normal, va.f_color, va.f_position.xyz, va.f_cloth_weight.x, cloth_out, cloth_leg_l, cloth_leg_r, cloth_up);	// UNOFFICIAL cloth wind (normal and skinned geometry)
    if (TESR_ShadowData.x == 1.0f && ClothIsMask5() && ClothWalk.x > 0.5f) {	// round 9: the hem ring by the pendulum share, then out of the current legs
        tmp_r0.xyz += ClothRingAt(va.f_cloth_weight.y) * va.f_cloth_weight.z;
        tmp_r0.xyz += ClothLegPush(tmp_r0.xyz, cloth_out);
    }
#endif
    if (TESR_ShadowData.x != 1.0f) tmp_r0 = mul(tmp_r0, TESR_ShadowWorldTransform);
    [branch] if (plant_lean != 0.0f) tmp_r0.xy += TESR_PlantLeafWind3.xy * plant_lean;	// UNOFFICIAL 2026-10-20 plant wind: the leaves' lean (world)
	tmp_r0 = mul(tmp_r0, TESR_ShadowViewProjTransform);
	
	// Pancaking to ensure geometry outside of near plane is not culled.
    tmp_r0.z = max(tmp_r0.z, 0.0f);
	
	vr.f_position = tmp_r0;
    vr.f_texcoord_0 = tmp_r0;
	vr.f_texcoord_1 = va.f_texcoord_0;
    return vr;
	
};