
uniform row_major float4x4 TESR_ShadowWorldTransform : register(c0);
uniform row_major float4x4 TESR_ShadowViewProjTransform : register(c4);
uniform float4 TESR_ShadowData : register(c8);
uniform float4 Bones[120] : register(c9);	// UNOFFICIAL 2026-11-05 (Balebandro ad32a3b) up to 40 bones (was 54 rows, 18: see ShadowMap.vso)
uniform float4 TESR_ShadowCubeMapLightPosition : register(c129);	// (set by name; c63 was overwritten by a 19th bone and on)

struct VtxAttrs {
    float4 f_position : POSITION;
	float4 f_texcoord_0 : TEXCOORD0;
    float4 f_blendweight : BLENDWEIGHT;
    float4 f_blendindexes : BLENDINDICES;
};

struct VtxVaryings {
    float4 f_position : POSITION;
    float4 f_texcoord_0 : TEXCOORD0;
	float4 f_texcoord_1 : TEXCOORD1;
};

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

#define	weight(v)		dot(v, 1)

	const float4 loc_const_0 = {1.0f, 765.01001f, 0.0f, 0.0f};
	
	float4 ofs;
    float4 tmp_q0;
    float4 tmp_q4;
    float4 tmp_q5;
    float4 tmp_q6;
    float4 tmp_q7;
    float4 tmp_q8;
	float4 tmp_r1 = 0.0f;
	float4 tmp_r0 = va.f_position;
	
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
	}
    if (TESR_ShadowData.x != 1.0f) tmp_r0 = mul(tmp_r0, TESR_ShadowWorldTransform);
	tmp_r1.xyz = TESR_ShadowCubeMapLightPosition.xyz - tmp_r0.xyz;
	tmp_r0 = mul(tmp_r0, TESR_ShadowViewProjTransform);
	vr.f_position = tmp_r0;
    vr.f_texcoord_0 = tmp_r1;
	vr.f_texcoord_1 = va.f_texcoord_0;
    return vr;
	
};