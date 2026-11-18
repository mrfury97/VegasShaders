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
// UNOFFICIAL 2026-11-11 PTScreenOwn ([Main.Main.RayTracing] PTScreenOwn; RtPathTrace.comp ScreenHit gScreenStale). The same pass, now run
// for every PTPrimary frame, also marks the world's pixels whose AlbedoBuffer / MaterialBuffer values belong to ANOTHER surface than the
// one the depth buffer holds: + 0.25 on the alpha (1.25 the world, 2.25 a person). Those two buffers are written only by NVR's templates
// drawn opaque in the world's main list (core/CapturedNormals.h); the captured normals' b is the writing surface's own camera distance:
//   - "in front": the depth buffer's surface is nearer than the captured one by more than share x d + units (c6.zw): the game drew
//     something over it later with depth but without the buffers (its blended hair, brows and beards; a head or body in the sorted
//     blended list; any shader of the game's own). One-sided: a surface whose depth lies BEHIND its captured distance (parallax /
//     displacement that writes depth) is not marked;
//   - bare skin: the skin template writes the captured normal (a > 1.5) but no albedo (its COLOR1 / COLOR2 are the skin blur's); its
//     colour is copied in after the list from the skin blur's albedo target (CapturedNormals::SkinAlbedoFill), which exists only where a
//     skin draw bound that target (SkinScattering::BindForSkinDraw: the effect on, ScreenSpace on, supported, an opaque or additive
//     pass). c8.x: 1 = no skin draw bound it this frame (every skin pixel keeps the colour of what was drawn BEFORE it), 2 = s4 holds
//     that target (its a = the albedo's luma; 0 = no skin wrote this pixel).
// More inputs: s3 TESR_DepthBuffer (view z / far), s4 the skin blur's albedo target (any texture with c8.x != 2);
//   c6 = (1 the view model's part on, 1 the marks on, the "in front" share, units), c7 = (uv scale x, y: screen size / render rect size,
//   the depth's jitter x, y: RtPathTrace.comp ScreenHit's uv), c8 = (the skin mode above, the displacement's reach, 0, 0). c6 = c8 = 0,
//   c7 = (1, 1, 0, 0) with s2 bound: exactly the pass as it was.
// 2026-11-11 (the same evening; a tester's video: displaced rock ground in two looks, raised bumps as the game's bright raster pixel or
// as smooth traced rock, the boundaries along the game's triangles): the tessellation's bumps moved a surface IN FRONT of its own
// captured distance. Compute/Tess/ObjectDisplace.tese displaces only the clip position; the object templates' captured distance comes
// from shadowWorldPos, a varying the evaluation shader passes through undisplaced (the land's follows its bumps: TEXCOORD6, but not
// always exactly: CastShadows / mapsFull). A raised bump was marked "another surface". Two marks now:
//   + 0.5  SURE: in front by more than the tolerance + the displacement's reach (c8.y: 1.25 x the largest Strength on + 1, 0 with no
//          tessellation), or bare skin: never the screen's colour / material (the traced record, else the game's pixel);
//   + 0.25 MAYBE: in front, but within the bumps' reach: the same surface displaced, or something small close before it. The trace
//          keeps the screen's colour and material there unless its camera ray meets hair / a cut-out or skin at that depth.
// UNOFFICIAL 2026-11-12 PTWaterMask ([Main.Main.RayTracing] PTWaterMask; core/WaterMask.h; docs/next/PT-COMPAT-AUDIT-2026-11-12.md item 4):
// the game's water is in none of the buffers above and not in the traced scene: its pixels showed the dry bed lit at the water's depth,
// a flat sheet or the game's water, by angle and depth. s5 = the frame's water mask (r: the share of the pixel's samples a water draw of
// the world scene passed the depth test on), c9 = (1 the mask on, its uv scale x, y: the mask's texel for this target's, 0). A world
// pixel (not the view model's, which keeps its own rules above) with a mask above 0.45 is coded -1: the game's own pixel, whatever the
// depth and the captured buffers say (ScreenHit: unwritten). c9 = 0 (the host's scenes that set nothing; no water drawn; the key off):
// s5 is not looked at and the pass is exactly as it was. (2026-11-13 PTAlbedoCoverage) c9.w: the mask's threshold when above 0 (0: 0.45).
// With the game's MSAA the mask is the share of the pixel's samples, and the trace takes a pixel as a surface only where ALL its samples
// were written: 0.1 there (one sample of eight counts), so a pixel with any water or unrecorded grass in it is the game's own.
// 2026-11-12 (ptgrass; a tester: "grass disappears / reappears depending on viewing angle") GRASS under the "in front" rule, written
// down because the rule's result for it depends on the view angle. Grass without records ([Main.Main.Quality] CapturedGrass off, the
// game's own grass shaders, lamp-lit grass) that is drawn AFTER the land has the land's records behind it; what the mark compares is the
// blade's distance before the land ALONG THE VIEW RAY: its height over the ground / sin(the angle between the ray and the ground).
// Seen along the ground that is many times the height (a sure mark: the game's own pixel); seen from above, or on a slope that faces
// the camera, it is the height itself, and with the tolerance growing by 1 % of the distance (22 units at 2000) the lower part of a
// tuft, or all of it, carries no mark: the land's colour at the blade's place. With the land's tessellation on, the reach adds to it.
// With the grass's own records the captured distance is the blade's: no mark at any angle (tools/nvr-rt-host scene `screenown`, GRASS).
namespace RtViewModel {
	inline const char* const kPrepPS =
		"sampler2D N : register(s0); sampler2D C : register(s1); sampler2D M : register(s2); sampler2D D : register(s3); sampler2D K : register(s4);"
		"sampler2D W : register(s5);"
		"float4 P : register(c0); float4 Q : register(c1); row_major float4x4 V : register(c2);"
		"float4 S : register(c6); float4 R : register(c7); float4 T : register(c8); float4 U : register(c9);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 nb = tex2D(N, uv); float4 c = tex2D(C, uv); float raw = tex2D(M, uv).x; float dw = tex2D(D, uv).x; float ka = tex2D(K, uv).a;"
		"  float wm = tex2D(W, uv * U.yz).x;"
		"  bool rev = Q.x > 0.5;"
		"  bool vm = S.x > 0.5 && (rev ? raw > 0.01 : raw < 0.9);"
		"  if (!vm) {"
		"    if (U.x > 0.5 && wm > (U.w > 0.0 ? U.w : 0.45)) return float4(nb.rgb, -1.0);"
		"    float a = c.a > 1.5 ? 2.0 : 1.0;"
		"    if (S.y > 0.5 && dw > 0.0 && dw < 0.9999 && c.a > 0.99) {"
		"      float2 r = uv * R.xy + R.zw;"
		"      float dist = length(float3((r.x * 2.0 - 1.0) * P.x * Q.y, (1.0 - r.y * 2.0) * P.y * Q.y, 1.0)) * dw * P.w;"
		"      float gap = c.b - dist; float tol = c.b * S.z + S.w;"
		"      bool bare = c.a > 1.5 && (T.x > 1.5 ? ka <= 0.0 : T.x > 0.5);"
		"      if (bare || gap > tol + T.y) a += 0.5;"
		"      else if (gap > tol) a += 0.25;"
		"    }"
		"    return float4(nb.rgb, a);"
		"  }"
		"  float z = rev ? P.z * P.w / (P.z + raw * (P.w - P.z)) : P.z * P.w / (P.w - raw * (P.w - P.z));"
		"  float3 vp = float3((uv.x * 2.0 - 1.0) * P.x, (1.0 - uv.y * 2.0) * P.y, 1.0) * z;"
		"  float d = length(vp);"
		"  if (!(c.a > 0.99 && abs(c.b - d) <= d * Q.z + Q.w)) return float4(nb.rgb, -1.0);"
		"  float3 n = float3(c.rg, 1.0 - abs(c.r) - abs(c.g)); float t = saturate(-n.z);"
		"  n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t); n = normalize(n);"
		"  float3 nv = normalize(mul(n, (float3x3)V));"
		"  return float4(nv * 0.5 + 0.5, 4.0 + Q.y); }";
	constexpr float kMatchShare = 0.03f, kMatchUnits = 2.0f;	// the distance match (c1.zw)
	constexpr float kFrontShare = 0.01f, kFrontUnits = 2.0f;	// (2026-11-11 PTScreenOwn) "in front" of the captured surface (c6.zw)
	constexpr float kOwnMaybe = 0.25f, kOwnSure = 0.5f;	// the marks on the alpha
	constexpr float kWaterShare = 0.45f;	// (2026-11-12 PTWaterMask) the water mask's threshold in kPrepPS (2 of 4 samples in 8 bits = 0.498)
	// the displacement's reach for a largest Strength on (0: none): a bump moves its point at most Strength along the normal
	inline float OwnReach(float strength) { return strength > 0.0f ? 1.25f * strength + 1.0f : 0.0f; }
}
