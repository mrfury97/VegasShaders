#pragma once
// UNOFFICIAL 2026-11-15 full deferred renderer, phase 6 (docs/next/FD-GI-RESULTS.md): the three small ps_3_0 passes of core/FdIndirect.h,
// as source strings only (no other include), so tests/fd_gi_passes.cpp compiles and runs the very same text on a D3D9 device against its
// CPU reference. Inputs are the G-buffer v2 targets (Includes/CapturedNormals.hlsl, GBufferV2 / CAPTURED_NORMALS_ON 4):
//   COLOR1 albedo: rgb linear base colour (before metal), a 0 unwritten, 1 forward, 1 + (1 - fog) deferred.
//   COLOR2 material: r = model + 16 x specular bit + 0.49 x metal; g / a the shading normal octahedral (world axes; g 4 and up = none here);
//                    b roughness (-1 none, the path tracer's -3 .. -2 code). Written = r > 0.5.
//   COLOR3 normals: rg the smooth normal octahedral (world axes), b the camera distance, a > 0.99 written (1 / 2 / skin code).
// The octahedral decode is Includes/Shadow.hlsl ShadowDecodeNormal's; the world -> view rotation is CapturedNormals::MergeIntoNormals' (the
// view transform's rows uploaded as stored, row_major, mul(n, (float3x3)V)).

namespace FdIndirectShaders {
	// The shading normals: TESR_NormalsBuffer (view space, n x 0.5 + 0.5; what Reflections and the DLSS RR guides read) with the G-buffer's
	// normal-mapped normal in place of the smooth one, where
	//  - the material texel holds a shading normal (r > 0.5, |g| <= 1.001),
	//  - the normals buffer's pixel is the captured surface's: COLOR3's camera distance matches the depth buffer's (the distance match
	//    CapturedNormals::MergeIntoNormals takes the captured normal with: share c6.x, units c6.y) and the buffer's normal is within the least
	//    cosine c0.z of COLOR3's smooth normal. Water, glass, grass and anything drawn over the G-buffer later keep the buffer's own normal,
	//  - the shading normal faces the camera (a normal map bent past the silhouette keeps the smooth one).
	// s0 the normals buffer, s1 COLOR3, s2 COLOR2, s3 TESR_DepthBuffer (linear: x far = view z). c0: 1 / proj _11, 1 / proj _22, least
	// cosine, far; c1: TESR_RenderScale (zw the reciprocal of the drawn share); c2-c5: TESR_ViewTransform's rows; c6: share, units. Alpha:
	// the normals buffer's own.
	inline const char* kShadingNormalsPS =
		"sampler2D N : register(s0); sampler2D C : register(s1); sampler2D M : register(s2); sampler2D D : register(s3);"
		"float4 P : register(c0); float4 R : register(c1); row_major float4x4 V : register(c2); float4 G : register(c6);"
		"float3 Oct(float2 e) { float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y)); float t = saturate(-n.z);"
		"  n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t); return normalize(n); }"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 nb = tex2D(N, uv); float4 c = tex2D(C, uv); float4 m = tex2D(M, uv); float z = tex2D(D, uv).x * P.w;"
		"  float3 vb = normalize(nb.xyz * 2.0f - 1.0f);"
		"  float3 vs = normalize(mul(Oct(c.rg), (float3x3)V));"
		"  float3 vn = normalize(mul(Oct(float2(m.g, m.a)), (float3x3)V));"
		"  float2 s = uv * R.zw;"
		"  float3 vp = float3((s.x * 2.0f - 1.0f) * P.x, (1.0f - s.y * 2.0f) * P.y, 1.0f) * z; float d = length(vp);"
		"  bool take = m.r > 0.5f && abs(m.g) <= 1.001f && c.a > 0.99f && abs(c.b - d) <= d * G.x + G.y && z < P.w * 0.99f &&"
		"    dot(vb, vs) > P.z && dot(vn, vp) < -0.02f * d;"
		"  return take ? float4(vn * 0.5f + 0.5f, nb.a) : nb; }";

	// The reflections' own material copy (Reflections' 8-bit target: r roughness, g metal, b 1 = reflects by its smoothness whatever way it
	// faces (MaterialAware's rule), b 0.5 = the floor guess with this roughness and metal; alpha 0) from G-buffer v2, blended
	// INVDESTALPHA / DESTALPHA over the _rmaos maps' pixels as CapturedNormals::MergeIntoReflMaterial is:
	//  - objects, worn gear and cloth (models 1-3) with the specular bit (the records that light lamps with a highlight: the game's shiny
	//    objects): b 1, their roughness and metal;
	//  - plants, skin, grass, hair, eyes (models 5-9): rough (roughness 1, metal 0, b 0.5): their b is not a roughness for every record
	//    (the foliage records keep their sun weight there) and none of them is a mirror;
	//  - everything else written (the land, matte objects, unlit): b 0.5 with its roughness and metal, as the old merge (none: clipped).
	// s0 COLOR2.
	inline const char* kReflMaterialPS =
		"sampler2D M : register(s0);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 m = tex2D(M, uv); clip(m.r - 0.5f);"
		"  float c = floor(m.r + 0.01f); float spec = c >= 16.0f ? 1.0f : 0.0f; float model = c - 16.0f * spec;"
		"  float metal = saturate(frac(m.r) / 0.49f);"
		"  if (model > 4.5f && model < 9.5f) return float4(1.0f, 0.0f, 0.5f, 0.0f);"
		"  float b = m.b < -1.99f && m.b > -3.01f ? -2.0f - m.b : m.b; clip(b);"
		"  return float4(saturate(b), metal, spec > 0.5f && model < 3.5f ? 1.0f : 0.5f, 0.0f); }";

	// AmbientLight's albedo (NVR_ALAlbedo, s12 of its Apply and balance passes): COLOR1 x (1 - metal) x the gain (c0.x). A metal has no
	// diffuse colour: the bounce it receives goes into its reflection, not into a diffuse colour (the DLSS RR guides split it the same way).
	// Alpha (the "written" mark AmbientLight tests with step(0.5, a)) as COLOR1's. s0 COLOR1, s1 COLOR2.
	inline const char* kALAlbedoPS =
		"sampler2D A : register(s0); sampler2D M : register(s1); float4 G : register(c0);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 a = tex2D(A, uv); float4 m = tex2D(M, uv);"
		"  float metal = m.r > 0.5f ? saturate(frac(m.r) / 0.49f) : 0.0f;"
		"  return float4(max(a.rgb, 0.0f) * (1.0f - metal) * G.x, a.a); }";
}
