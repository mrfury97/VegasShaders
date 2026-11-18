#pragma once
// UNOFFICIAL 2026-11-12 the path tracer's far hand-over ([Main.Main.RayTracing] PTFarBlend / PTGameFog / PTFarMatch; core\RtPathTrace.cpp
// FarHandOver), shared by the game (compiled ps_3_0 at run time) and tools/nvr-rt-host (scene `farhandover`: the same sources on the
// host's D3D9 device against the CPU functions below). No game types.
//
// What it is for (a tester on an RTX card: "it stops abruptly at range ... leaving regular VS shaders, which look WAY different, to
// handle like, LOD and stuff"). The path traced picture covers the pixels whose colour the mod's own templates recorded: the land and
// the objects of the LOADED cells (a square of uGridsToLoad cells round the player's cell), and the OBJECTS the game draws past them
// (they go through the object templates too). Far land and the terrain's own fade band are drawn by shaders that record nothing: they
// stay the game's raster pixel. So the traced LAND ends on a line in the world, the loaded square's edge, while objects past it are
// still traced with almost no traced scene round them (dark slabs on a far hill in the bench). Across that line the light changes:
//   - the game's own fog, but ONLY with the VolumetricFog effect off: then the forward shaders blend every raster pixel toward the fog
//     colour by distance and the traced pixels had none of it. (With that effect on, the default, the game's shaders are handed a fog
//     property that fogs nothing, Render.cpp ShadowSceneNode__GetFogPropertyEx, and the mod's fog pass hazes every pixel later: no
//     difference there. Measured 2026-11-12: applying the weather's fog to the traced pixels with that effect on made them twice as
//     blue as the game's own picture of the same ground.)
//   - the sky's light (the tracer's sky x PTSky + its bounces) against the game's ambient colour + the terrain's own sky term;
//   - the sun's shadows (traced inside, the cascades / far-land factor outside).
// Three steps, each with its own key, all drawn AFTER the traced picture is in the scene (RtPathTrace::Replace), on the traced pixels only
// (the first-surface image's a >= 0.5, not the first-person model's kept pixels):
//   1. PTGameFog (only while the game draws its fog, see above): the game's fog on the traced pixels, the forward shaders' own formula
//      (pow(saturate((d - start) / (end - start)), power) toward the fog colour, in the stored curve, d = the clip position's length),
//      with the world scene's fog property's values.
//   2. PTFarMatch: the traced picture x a gain that rises from 1 (PTFarMatchReach of the loaded range inside the edge) to the measured
//      ratio game / traced at the edge (the game's own frame of the SAME pixels before the replace, median over the band, per channel,
//      clamped to PTFarMatchLimit, eased over ~2 s).
//   3. PTFarBlend: over the last PTFarBlendWidth of the loaded range the traced pixel fades into the game's own pixel (the frame copied
//      before the replace), and every traced pixel OUTSIDE the loaded square (the objects the game draws past it: they do write
//      records) is the game's own pixel again. So at the edge both sides are the same raster picture. With PTFarBlendShadows (off by
//      default) the deferred shadow buffer, which PTOwnShadows sets to "lit" on traced pixels, keeps the game's shadows by the same
//      share there (kShadowPS); without it the buffer stays "lit" on every traced pixel, as before.
//
// Inputs of every pass: s0 the trace's first-surface image (a >= 0.5: a traced pixel), s1 TESR_DepthBuffer (view z / farZ), s2
// TESR_DepthBufferViewModel (any texture with c0.w 0), s3 the game's frame before the replace (the stored curve; kBlendPS only).
//   c0 = (the view's forward x, y (world; z up), farZ, the first-person model's kept pixels: 0 none, 1 where s2 < 0.9, 2 where s2 > 0.01)
//   c1 = (right x, y / P00, up x, y / P11): a pixel's point relative to the camera = (forward + right' nx + up' ny) x view z
//   c2 = the loaded square relative to the camera: min x, min y, max x, max y (world units)
//   c3 = (1 / the band's width in units, 0 = no band; the game frame's scale (PTExposure in the stored curve); screen / render rect x, y)
//   c4 = (the fog's start, 1 / (end - start), power, strength 0-1; strength 0: no fog)
//   c5 = (the fog colour, stored curve, x the frame's scale; the view: 0 the picture, 1 the map, 2 the game's frame on every traced pixel)
//   c6 = (the level gain at the edge, stored curve, r g b; 1 / the ramp's length in units, 0 = the whole traced picture)
//   c7 = (kShadowPS: 0 the solid part, 1 the band's part; unused; kMapPS: its target's uv -> the inputs' uv, x y: the render rect's
//        share of the buffers when the small map covers only that rect (RenderScale below 1), else 1 1)
#include <cmath>
#include <algorithm>
#include <vector>

namespace RtFarHandOver {
	struct Frame {
		float forward[2] = { 0.0f, 1.0f };	// the view's forward, world x y
		float right[2] = { 1.0f, 0.0f };	// right / P00
		float up[2] = { 0.0f, 0.0f };		// up / P11
		float farZ = 1.0f;
		float keepViewModel = 0.0f;
		float square[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
		float band = 0.0f;					// units; 0 = no band
		float frameScale = 1.0f;
		float rectScale[2] = { 1.0f, 1.0f };
		float fogNear = 0.0f, fogFar = 1.0f, fogPower = 1.0f, fogStrength = 0.0f;
		float fogColour[3] = { 0.0f, 0.0f, 0.0f };	// stored curve (not yet x frameScale)
		float view = 0.0f;
		float gain[3] = { 1.0f, 1.0f, 1.0f };	// stored curve
		float reach = 0.0f;					// units; 0 = everywhere
	};
	inline void Pack(const Frame& f, float k[8][4]) {
		k[0][0] = f.forward[0]; k[0][1] = f.forward[1]; k[0][2] = f.farZ; k[0][3] = f.keepViewModel;
		k[1][0] = f.right[0]; k[1][1] = f.right[1]; k[1][2] = f.up[0]; k[1][3] = f.up[1];
		for (int i = 0; i < 4; i++) k[2][i] = f.square[i];
		k[3][0] = f.band > 0.0f ? 1.0f / f.band : 0.0f; k[3][1] = f.frameScale; k[3][2] = f.rectScale[0]; k[3][3] = f.rectScale[1];
		const float range = f.fogFar - f.fogNear;
		k[4][0] = f.fogNear; k[4][1] = range > 1.0f ? 1.0f / range : 0.0f; k[4][2] = (std::max)(f.fogPower, 0.01f); k[4][3] = range > 1.0f ? f.fogStrength : 0.0f;
		for (int i = 0; i < 3; i++) k[5][i] = f.fogColour[i] * f.frameScale;
		k[5][3] = f.view;
		for (int i = 0; i < 3; i++) k[6][i] = f.gain[i];
		k[6][3] = f.reach > 0.0f ? 1.0f / f.reach : 0.0f;
		k[7][0] = k[7][1] = 0.0f; k[7][2] = k[7][3] = 1.0f;
	}

	// The CPU's twins of the shaders' terms (the game's measurement and the RT host's reference).
	// u, v: the pixel centre in the target's uv; depth: TESR_DepthBuffer's value. Returns how far inside the loaded square the pixel's
	// point lies (negative: outside), fogDistance: the game's fog distance there.
	inline float Edge(const Frame& f, float u, float v, float depth, float* fogDistance = nullptr) {
		const float nx = u * f.rectScale[0] * 2.0f - 1.0f, ny = 1.0f - v * f.rectScale[1] * 2.0f;
		const float z = depth * f.farZ;
		const float px = (f.forward[0] + f.right[0] * nx + f.up[0] * ny) * z, py = (f.forward[1] + f.right[1] * nx + f.up[1] * ny) * z;
		if (fogDistance) *fogDistance = z * sqrtf(1.0f + nx * nx + ny * ny);
		return (std::min)((std::min)(px - f.square[0], f.square[2] - px), (std::min)(py - f.square[1], f.square[3] - py));
	}
	inline float Smooth(float x) { x = (std::min)((std::max)(x, 0.0f), 1.0f); return x * x * (3.0f - 2.0f * x); }
	inline float BandWeight(const Frame& f, float edge) { return f.band > 0.0f ? 1.0f - Smooth(edge / f.band) : 0.0f; }	// 1 = the game's pixel
	inline float Fog(const Frame& f, float fogDistance) {
		const float range = f.fogFar - f.fogNear;
		if (!(range > 1.0f) || f.fogStrength <= 0.0f) return 0.0f;
		const float t = (std::min)((std::max)((fogDistance - f.fogNear) / range, 0.0f), 1.0f);
		return f.fogStrength * powf(t, (std::max)(f.fogPower, 0.01f));
	}
	inline float Ramp(const Frame& f, float edge) { return f.reach > 0.0f ? 1.0f - Smooth(edge / f.reach) : 1.0f; }	// 1 = the full gain

	// The loaded square from the game's cell grid: centre cell (cx, cy), cells a side (uGridsToLoad), relative to the camera's world
	// position. False (and a camera-centred square of the same size) when the camera is not inside it (a grid that is not this worldspace's).
	inline bool LoadedSquare(int cx, int cy, int cells, float camX, float camY, float square[4]) {
		const float cell = 4096.0f;
		const int half = (std::max)(cells, 1) / 2;
		const float x0 = (float)(cx - half) * cell, y0 = (float)(cy - half) * cell, side = (float)(2 * half + 1) * cell;
		if (camX >= x0 && camX <= x0 + side && camY >= y0 && camY <= y0 + side) {
			square[0] = x0 - camX; square[1] = y0 - camY; square[2] = x0 + side - camX; square[3] = y0 + side - camY;
			return true;
		}
		square[0] = square[1] = -0.5f * side; square[2] = square[3] = 0.5f * side;
		return false;
	}

#define NVR_FAR_COMMON \
	"sampler2D A : register(s0); sampler2D D : register(s1); sampler2D M : register(s2); sampler2D F : register(s3);" \
	"float4 K0 : register(c0); float4 K1 : register(c1); float4 K2 : register(c2); float4 K3 : register(c3);" \
	"float4 K4 : register(c4); float4 K5 : register(c5); float4 K6 : register(c6); float4 K7 : register(c7);" \
	"float Edge(float2 uv, float d, out float dist) {" \
	"  float2 n = float2(uv.x * K3.z * 2.0 - 1.0, 1.0 - uv.y * K3.w * 2.0);" \
	"  float z = d * K0.z;" \
	"  float2 p = (K0.xy + K1.xy * n.x + K1.zw * n.y) * z;" \
	"  dist = z * sqrt(1.0 + dot(n, n));" \
	"  return min(min(p.x - K2.x, K2.z - p.x), min(p.y - K2.y, K2.w - p.y)); }" \
	"bool Traced(float2 uv, float d) {" \
	"  float vm = tex2D(M, uv).x;" \
	"  if (uv.x * K3.z > 1.0 || uv.y * K3.w > 1.0) return false;" \
	"  return tex2D(A, uv).a >= 0.5 && d > 0.0 && d < 0.9999 && !(K0.w > 0.5 && (K0.w > 1.5 ? vm > 0.01 : vm < 0.9)); }" \
	"float Band(float e) { return K5.w > 1.5 ? 1.0 : K3.x > 0.0 ? 1.0 - smoothstep(0.0, 1.0, e * K3.x) : 0.0; }"

	// The fog and the band in one blended draw (SRCALPHA, INVSRCALPHA; colour only): scene = scene x (1 - f)(1 - w) + fog f (1 - w) + frame w.
	// View 1 (no blend wanted: a = 1): traced pixels (w, f, 0.25 + 0.75 x the share of the half side inside the edge), others 0 0 and
	// 0.125 where the depth holds a surface (far land, grass, the game's own shaders), 0 the sky.
	inline const char* const kBlendPS = NVR_FAR_COMMON
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float d = tex2D(D, uv).x; float dist; float e = Edge(uv, d, dist);"
		"  bool traced = Traced(uv, d);"
		"  float w = Band(e);"
		"  float f = K5.w > 1.5 ? 0.0 : K4.w * pow(saturate((dist - K4.x) * K4.y), K4.z);"
		"  if (K5.w > 0.5 && K5.w < 1.5) {"
		"    if (traced) return float4(w, f, 0.25 + 0.75 * saturate(2.0 * e / max(K2.z - K2.x, 1.0)), 1.0);"
		"    return float4(0.0, 0.0, d > 0.0 && d < 0.9999 ? 0.125 : 0.0, 1.0); }"
		"  clip(traced ? 1.0 : -1.0);"
		"  float keep = (1.0 - f) * (1.0 - w);"
		"  float3 c = (K5.rgb * (f * (1.0 - w)) + tex2D(F, uv).rgb * (K3.y * w)) / max(1.0 - keep, 1.0e-6);"
		"  return float4(c, 1.0 - keep); }";
	// The level gain (DESTCOLOR, SRCCOLOR: scene x 2 x the output, so a gain up to 2 fits an output of 0-1; colour only)
	inline const char* const kGainPS = NVR_FAR_COMMON
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float d = tex2D(D, uv).x; float dist; float e = Edge(uv, d, dist);"
		"  clip(Traced(uv, d) ? 1.0 : -1.0);"
		"  float r = K6.w > 0.0 ? 1.0 - smoothstep(0.0, 1.0, e * K6.w) : 1.0;"
		"  return float4(lerp(float3(1.0, 1.0, 1.0), K6.rgb, r) * 0.5, 1.0); }";
	// PTOwnShadows with the band: c7.x 0 = "lit" on the traced pixels inside the band's inner edge (no blend, as the pass without a
	// band); 1 = the band's own pixels, blended (SRCALPHA, INVSRCALPHA; colour only): the buffer keeps the game's shadow by the share w
	inline const char* const kShadowPS = NVR_FAR_COMMON
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float d = tex2D(D, uv).x; float dist; float e = Edge(uv, d, dist);"
		"  float vm = tex2D(M, uv).x;"
		"  bool traced = tex2D(A, uv).a >= 0.5 && !(K0.w > 0.5 && (K0.w > 1.5 ? vm > 0.01 : vm < 0.9)) && uv.x * K3.z <= 1.0 && uv.y * K3.w <= 1.0;"
		"  float w = d > 0.0 && d < 0.9999 ? Band(e) : 0.0;"
		"  clip(traced && (K7.x > 0.5 ? w > 0.0 && w < 1.0 : w <= 0.0) ? 1.0 : -1.0);"
		"  return float4(1.0, 1.0, 1.0, 1.0 - w); }";
	// The measurement's map (a small target, no blend; c7.zw: the target covers that share of the inputs, the render rect): r 1 = a traced
	// pixel, g the fog's share there, b how far inside the edge (units / 65536; negative outside), a the depth
	inline const char* const kMapPS = NVR_FAR_COMMON
		"float4 main(float2 uv0 : TEXCOORD0) : COLOR0 {"
		"  float2 uv = uv0 * K7.zw;"
		"  float d = tex2D(D, uv).x; float dist; float e = Edge(uv, d, dist);"
		"  float f = K4.w * pow(saturate((dist - K4.x) * K4.y), K4.z);"
		"  return float4(Traced(uv, d) ? 1.0 : 0.0, f, e / 65536.0, d); }";
#undef NVR_FAR_COMMON

	// The measured ratio game / traced over the band: per channel the median of (game / traced-with-fog) in linear light. samples: n x 7
	// floats (traced r g b, game r g b in the stored curve, the fog's share); false with fewer than `least` usable samples.
	inline bool Ratio(const Frame& f, const float* samples, int n, int least, float ratio[3], int* used = nullptr) {
		static thread_local std::vector<float> v[3];
		for (int c = 0; c < 3; c++) v[c].clear();
		for (int i = 0; i < n; i++) {
			const float* s = samples + 7 * i;
			float t[3], g[3];
			bool ok = true;
			for (int c = 0; c < 3; c++) {
				t[c] = s[c] + (f.fogColour[c] * f.frameScale - s[c]) * s[6];
				g[c] = s[3 + c] * f.frameScale;
				if (!(t[c] > 0.02f) || !(g[c] > 0.02f) || !(t[c] < 64.0f) || !(g[c] < 64.0f)) ok = false;	// (near black, or not a number: no ratio)
			}
			if (!ok) continue;
			for (int c = 0; c < 3; c++) v[c].push_back(powf(g[c] / t[c], 2.2f));
		}
		if (used) *used = (int)v[0].size();
		if ((int)v[0].size() < (std::max)(least, 1)) return false;
		for (int c = 0; c < 3; c++) {
			std::nth_element(v[c].begin(), v[c].begin() + v[c].size() / 2, v[c].end());
			ratio[c] = v[c][v[c].size() / 2];
		}
		return true;
	}
}
