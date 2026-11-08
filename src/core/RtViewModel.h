#pragma once
// UNOFFICIAL 2026-11-07 (ptaaa-firstperson; docs/next/RT-PATH-TRACING.md "ptaaa-firstperson") PTViewModel's prep pass, shared by the game
// (RtPathTrace.cpp ViewModelPrep, compiled ps_3_0 at run time) and tools/nvr-rt-host (scene `vmprep`: the same source run on the host's
// D3D9 device against a CPU reference). No game types.
//
// One full-screen D3D9 pass makes the path tracer's normals image (RtPathTrace.comp ScreenHit) from
//   s0 TESR_NormalsBuffer (view-space normal x 0.5 + 0.5), s1 TESR_CapturedNormalsBuffer (rg octahedral camera-relative world normal,
//   b the camera distance, a 1 written / 2+ actors), s2 TESR_DepthBufferViewModel (raw, reversed or not; the view model where drawn),
//   c0 = (1 / (P00 s), 1 / (P11 s), the view model's near plane, far), c1 = (1 reversed depth, s, the distance match's share, units),
//   c2-c5 TESR_ViewTransform (row major: the captured world normal to view space, as CapturedNormals::MergeIntoNormals);
//   s = tan(world FOV / 2) / tan(first-person FOV / 2).
// Out (RGBA16F): rgb the view-space normal x 0.5 + 0.5; a = 1 the world, 2 a person (captured a > 1.5: RtDenoise's PeopleHistory mask),
//   4 + s the view model where the first-person pass captured it (its captured distance = its depth's at the view model's FOV, within
//   share x d + units: the normal is then the captured one), -1 the view model where it did not (the game's own pixel stays).
namespace RtViewModel {
	inline const char* const kPrepPS =
		"sampler2D N : register(s0); sampler2D C : register(s1); sampler2D M : register(s2);"
		"float4 P : register(c0); float4 Q : register(c1); row_major float4x4 V : register(c2);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 nb = tex2D(N, uv); float4 c = tex2D(C, uv); float raw = tex2D(M, uv).x;"
		"  bool rev = Q.x > 0.5;"
		"  bool vm = rev ? raw > 0.01 : raw < 0.9;"
		"  if (!vm) return float4(nb.rgb, c.a > 1.5 ? 2.0 : 1.0);"
		"  float z = rev ? P.z * P.w / (P.z + raw * (P.w - P.z)) : P.z * P.w / (P.w - raw * (P.w - P.z));"
		"  float3 vp = float3((uv.x * 2.0 - 1.0) * P.x, (1.0 - uv.y * 2.0) * P.y, 1.0) * z;"
		"  float d = length(vp);"
		"  if (!(c.a > 0.99 && abs(c.b - d) <= d * Q.z + Q.w)) return float4(nb.rgb, -1.0);"
		"  float3 n = float3(c.rg, 1.0 - abs(c.r) - abs(c.g)); float t = saturate(-n.z);"
		"  n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t); n = normalize(n);"
		"  float3 nv = normalize(mul(n, (float3x3)V));"
		"  return float4(nv * 0.5 + 0.5, 4.0 + Q.y); }";
	constexpr float kMatchShare = 0.03f, kMatchUnits = 2.0f;	// the distance match (c1.zw)
}
