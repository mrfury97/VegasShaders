// NvrRtPlayground: a walkable test room for Vegas Shaders' ray tracing without the game (tools/nvr-rt-playground/README.md).
//
// A 32-bit D3D9 program on the NVR DXVK fork (d3d9.dll next to the exe) that plays the game's side of the RT core, as the RT test host
// does (tools/nvr-rt-host), but interactively: a window, a WASD + mouse-look camera, a room made of boxes, spheres and cards, and each
// frame the path tracer exactly as the game runs it under [Main.Main.RayTracing] PTRealtime (src/core/RtPathTrace.cpp + RtDenoise.cpp):
//   1. the G-buffer by D3D9 raster (TESR_DepthBuffer view z / farZ, view-space normals, linear albedo, MaterialBuffer, the "game's frame"
//      = a simple raster shading of it, the stored curve x^(1/2.2));
//   2. CopyIn of those + the lamp image (the lamps, the frame row: ReSTIR DI keys, the radiance cache keys, the GGX energy table,
//      PTContactShadows) into the shared images, RtPathTrace.spv (+ RtPathTraceCache.spv) in the real NvrRtHelper.exe;
//   3. CopyOut of the noisy picture + the first surface's colour, RtDenoiseTemporalRgb -> RtDenoiseAtrousRgb x2 (fork passes, the
//      game's push blocks and settings), RtPathTraceShow (PTDetail, modulation) into the HDR scene; a tonemap; a HUD.
// With the path tracer off: the raster shading with RT sun shadows (RtSunShadow.spv). Same .spv files as the game ships.
//
// Usage: NvrRtPlayground.exe --helper <NvrRtHelper.exe> --spv <folder of the trace .spv> --compute <folder of the fork passes' .spv>
//        [--size 640x360] [--window 1280x720] [--device <DXVK device name part>] [--out <folder>] [--demo <seconds>] [--cache-log2 N]
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "pg_host.h"
#include "pg_scene.h"
#include "RtRestirRow.h"      // the path tracer's frame row (src/core): ReSTIR DI / radiance cache keys, the GGX energy table
#include "RtTessContact.h"    // PTContactShadows' frame-row texels (src/core)
#include "RtMaterials.h"      // (2026-11-07) PTGlass / PTFoliage / PTSkin / PTClothSheen keys, frame-row texels 24-26 (src/core)
#include "RtEmissiveRow.h"    // (2026-11-07) PTEmissive: the emissive row (lamp image row 50) + its remap (src/core)

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ------------------------------------------------------------------------------------------------------------------------------
static FILE* gLog = nullptr;
void pgLog(const char* fmt, ...) {
	char line[2048]; va_list a; va_start(a, fmt); vsnprintf(line, sizeof(line), fmt, a); va_end(a);
	std::fputs(line, stdout); std::fflush(stdout);
	if (gLog) { std::fputs(line, gLog); std::fflush(gLog); }
}

// ------------------------------------------------------------------------------------------------------------------------------
// The shaders' constant blocks (as the game's C++ fills them; rt_layout_check.py checks the host's identical copies)
struct PathConstants {        // RtPathTrace.cpp PassConstants
	float size[4], forward[4], right[4], up[4], origin[4], accum[4], sunDir[4], sunColor[4], skyZenith[4], skyHorizon[4], ambient[4], misc[4];
	float lampPos[8][4], lampColor[8][4];
	float rect[4];
	float grid[3][4];
};
static_assert(sizeof(PathConstants) == 512, "RtPathTrace's block is full");
struct SunConstants {         // RtSunShadows.cpp PassConstants
	float size[4], forward[4], right[4], up[4], origin[4], sun[4], bias[4], range[4], alpha[4], full[4];
	float cloud[4], cloudCentre[4], cloudLight[4], camera[4];
	float slack[4], contact[4], contactGate[4];
};
static uint16_t ToHalf(float f) {
	uint32_t x; std::memcpy(&x, &f, 4);
	const uint32_t s = (x >> 16) & 0x8000u; const int e = int((x >> 23) & 0xff) - 127 + 15; const uint32_t m = x & 0x7fffffu;
	if ((x & 0x7fffffffu) == 0 || e <= 0) return uint16_t(s);
	if (e >= 31) return uint16_t(s | 0x7c00u);
	return uint16_t(s | (uint32_t(e) << 10) | (m >> 13));
}
static void put4(float* d, V3 v, float w) { d[0] = v.x; d[1] = v.y; d[2] = v.z; d[3] = w; }
static void put4(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }

// RtPathTrace.cpp's lamp image: 4 texels a lamp, 256 lamps + 32 glow lights a row; rows 1-48 the light grid; row 49 the frame row;
// (2026-11-07 ptaaa-emissive) row 50 the emissive row
static const int kLampsMax = 256, kGlowMax = 32, kLightTexels = 4 * (kLampsMax + kGlowMax), kGridRows = 48, kFrameRow = 1 + kGridRows;
static const int kEmissiveRow = kFrameRow + 1, kLampRows = kEmissiveRow + 1;
static_assert(kEmissiveRow == RtEmissiveRow::kRow && RtEmissiveRow::kRowFloats <= 2 * kLightTexels, "RtEmissiveRow.h kRow / the row's room");
// the scratch store (RtPathTrace.cpp): ReSTIR DI's reservoirs (two sets of w x h x 6 words since ptaaa-emissive), the radiance cache's
// table (16 words an entry), then PTEmissive's blocks + triangle list
static unsigned long long CacheBaseWords(UINT w, UINT h) { return ((12ull * w * h + 4095ull) / 4096ull) * 4096ull; }
static unsigned long long EmissiveBaseWords(UINT w, UINT h, int cacheLog2) { return CacheBaseWords(w, h) + (16ull << cacheLog2); }

// RtDenoise.cpp's push blocks (RGB mode; tools/nvr-rt-host/rt_layout_check.py checks the game's against the shaders)
struct TemporalPush { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4]; uint32_t samplerIndex; float clampK, peopleCap, antiLag, fireflyK, fastFrames; };
struct SpecPush { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4], spec[4]; uint32_t samplerIndex; float clampK, peopleCap, antiLag, fireflyK, fastFrames; };
struct AtrousPush { float params[4], size[4]; uint32_t samplerIndex; float guideSigma; uint32_t specular; float historyFix; };
struct FireflyPush { float size[4]; uint32_t samplerIndex, pad0, pad1, pad2; };
static_assert(sizeof(TemporalPush) == 200 && sizeof(SpecPush) == 216 && sizeof(AtrousPush) == 48 && sizeof(FireflyPush) == 32, "RtDenoise.cpp's push blocks");
static_assert(offsetof(SpecPush, spec) == offsetof(TemporalPush, samplerIndex), "SpecPush = TemporalPush's head + spec");

// ------------------------------------------------------------------------------------------------------------------------------
// Settings: what the keys toggle (README "Adding a feature toggle": a field here, a line in the toggle table, its use in TracePass)
struct Settings {
	int pt = 1;            // P: the path tracer (off: the raster shading + RT sun shadows)
	int ptMode = 0;        // N: 0 PTRealtime (denoised), 1 the reference (ReferencePT: a running mean while nothing moves), 2 raw (one path, no denoiser)
	int bounces = 3;       // B: PTBounces under PTRealtime (3; 1 turns PTLastSky / PTLastBounce on, as the game)
	int sun = 1;           // U
	float sunAz = 0.25f, sunEl = 0.45f;   // arrows: toward the sun, radians (azimuth from +x, elevation)
	int lamps = 1;         // K
	int lampAnim = 1;      // L
	int spin = 1;          // J: the spinning cube moves
	int glowLights = 0;    // G: PTGlowLights (the glowing panel sampled as a light)
	int detail = 1;        // H: PTDetail
	int specular = 1;      // X: PTSpecular
	int restirDI = 1;      // R: PTRestirDI (PTRealtime's auto)
	int cache = 1;         // Y: PTRadianceCache (PTRealtime's auto)
	int lampFalloff = 0;   // F: PTLampFalloff (0 the game's, 1 physical)
	int contact = 1;       // PTContactShadows 1 (the default; no key)
	int indirectClamp = 1; // Z: the bounce light's cap: 0 none / 4 (2026-11-09: PTRealtimeClamp's default under PTRealtime; was 0)
	int bounceRoughen = 1; // F10: PTBounceRoughen 0 off / 0.35 (2026-11-09: the default under PTRealtime; frame row texel 27)
	// (2026-11-07, the merged ptaaa branches; the game's defaults under PTRealtime)
	int emissive = 1;      // F2: PTEmissive (glowing triangles as lights: RtEmissiveBuild + the emissive row)
	int denoiseSplit = 1;  // F3: PTDenoiseSplit (diffuse + specular denoised apart, RtDenoiseTemporalSpec)
	int antiFirefly = 1;   // F4: PTDenoiseAntiFirefly (RtDenoiseFirefly, PTDenoiseFireflyK 2)
	int objectMotion = 1;  // 5: the object motion (2026-11-09: TESR_ObjectMotionBuffer's stand-in, PTDenoiseObjectMotion + MovingHistory 8, as the game) / off (before)
	int mirrorSurface = 1; // F11: PTDenoiseMirrorSurface 0.1 (2026-11-09 pt-mirrors: a smooth metal denoised as the thing it reflects) / off (as before)
	int fireflySpread = 1; // F9: PTDenoiseFireflySpread 0 no cap (as before 2026-11-09) / 1 = 4 (the default) / 2 = 1
	int glass = 2;         // F5: PTGlass 0 off / 1 the light / 2 the camera's rays too
	int foliage = 1;       // F6: PTFoliage 0 / 0.4
	int skin = 1;          // F7: PTSkin 0 / 1 (+ PTClothSheen 0.5 with it)
	int debugView = 0;     // V: index into kDebugViews
	int split = 0;         // O: PTSplit 0.5 (left the raster frame, right the path tracer)
	int rtSun = 1;         // I: with the path tracer off, RT sun shadows on the raster shading
	int tonemap = 1;       // T
	int hud = 1;           // F1
	float exposure = 1.0f; // + / -
	float ptGlow = 4.0f;   // PTGlow (the playground's default: the panel is its only traced glow)
	float ptSky = 0.25f;   // PTSky (2026-11-09: the game's default since 1.0.6, pt-look; was 1.0 here: the sky's light flattened sunlit faces)
	float denoiseHistory = 32, denoiseSigma = 1.0f, denoiseAntiLag = 3.0f, denoiseGuide = 0.35f, detailRadius = 4.0f;
	int denoiseIterations = 2;
	float denoiseHistoryFix = 3.0f, denoiseMeanVariance = 2.0f, fireflyK = 2.0f, mirrorHistory = 12.0f;   // PTDenoiseHistoryFix / MeanVariance / FireflyK / MirrorHistory
	RtMaterials::Keys Materials() const {
		RtMaterials::Keys k;
		k.glass = float(glass); k.glassTint = 0.5f; k.foliage = foliage ? 0.4f : 0.0f; k.skin = skin ? 1.0f : 0.0f; k.sheen = skin ? 0.5f : 0.0f;
		return k;
	}
};
static const int kDebugViews[] = { 0, 1, 2, 3, 4, 8, 9, 10 };
static const char* const kDebugNames[] = { "picture", "albedo", "normals", "direct only", "rough/metal", "direct highlight", "reflection only", "radiance cache" };

// ------------------------------------------------------------------------------------------------------------------------------
// The raster shaders (D3D9 HLSL, compiled at start with D3DCompile)
static const char* kVsGeo = R"(
float4 c_row0 : register(c0); float4 c_row1 : register(c1); float4 c_row2 : register(c2);
float4 c_eye : register(c3);   // camera world position
float4 c_R : register(c4);     // right (unit), w P00
float4 c_U : register(c5);     // up (unit), w P11
float4 c_F : register(c6);     // forward (unit), w farZ
float4 c_z : register(c7);     // depth mapping zA, zB; D3D9's half-pixel shift (-1/W, 1/H)
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 h : POSITION; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 rel : TEXCOORD2; };
VO main(VI i) {
	VO o;
	float4 p = float4(i.p, 1);
	float3 rel = float3(dot(c_row0, p), dot(c_row1, p), dot(c_row2, p)) - c_eye.xyz;
	float vz = dot(rel, c_F.xyz);
	o.h = float4(dot(rel, c_R.xyz) * c_R.w, dot(rel, c_U.xyz) * c_U.w, vz * c_z.x + c_z.y, vz);
	o.h.xy += c_z.zw * vz;    // pixel centres on the rays the trace shoots (uv = (pixel + 0.5) / size)
	o.n = float3(dot(c_row0.xyz, i.n), dot(c_row1.xyz, i.n), dot(c_row2.xyz, i.n));
	o.uv = i.uv; o.rel = rel;
	return o;
})";
// (2026-11-09 pt-mirrors, the owner's request) the object motion pass (core\ObjectMotion.h, TESR_ObjectMotionBuffer): each vertex placed
// with this frame's transform (c0-c2) and last frame's (c8-c10); out: how far that surface point moved in the world since last frame
static const char* kVsMotion = R"(
float4 c_row0 : register(c0); float4 c_row1 : register(c1); float4 c_row2 : register(c2);
float4 c_eye : register(c3); float4 c_R : register(c4); float4 c_U : register(c5); float4 c_F : register(c6); float4 c_z : register(c7);
float4 c_prev0 : register(c8); float4 c_prev1 : register(c9); float4 c_prev2 : register(c10);
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 h : POSITION; float3 move : TEXCOORD0; };
VO main(VI i) {
	VO o;
	float4 p = float4(i.p, 1);
	float3 world = float3(dot(c_row0, p), dot(c_row1, p), dot(c_row2, p));
	float3 rel = world - c_eye.xyz;
	float vz = dot(rel, c_F.xyz);
	o.h = float4(dot(rel, c_R.xyz) * c_R.w, dot(rel, c_U.xyz) * c_U.w, vz * c_z.x + c_z.y, vz);
	o.h.xy += c_z.zw * vz;
	o.move = world - float3(dot(c_prev0, p), dot(c_prev1, p), dot(c_prev2, p));
	return o;
})";
static const char* kPsMotion = R"(
float4 main(float3 move : TEXCOORD0) : COLOR0 { return float4(move, 1); }
)";
static const char* kPsGBuffer = R"(
sampler2D s_diffuse : register(s0); sampler2D s_glow : register(s1);
float4 c_albedo : register(c0);   // rgb linear, w 1: x the diffuse texture (sRGB)
float4 c_mat : register(c1);      // x rough (< 0 none), y metal, z alpha reference 0-1 (< 0: no test), w 1 = raster only (not in the RT scene)
float4 c_emit : register(c2);     // rgb emissive (linear), w 1: x the glow texture
float4 c_cls : register(c3);      // x the MaterialBuffer's class (1 plain, 3 cloth, 5 plants, 6 skin)
float4 c_R : register(c4); float4 c_U : register(c5); float4 c_F : register(c6);
struct PI { float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 rel : TEXCOORD2; };
struct PO { float4 normals : COLOR0; float4 albedo : COLOR1; float4 material : COLOR2; float4 emissive : COLOR3; };
float3 lin(float3 c) { return pow(max(c, 0), 2.2); }
PO main(PI i) {
	float4 tex = tex2D(s_diffuse, i.uv);
	if (c_mat.z >= 0) clip(tex.a - c_mat.z);
	float3 n = normalize(i.n);
	if (dot(n, i.rel) > 0) n = -n;
	PO o;
	o.normals = float4(dot(n, c_R.xyz) * 0.5 + 0.5, dot(n, c_U.xyz) * 0.5 + 0.5, dot(n, c_F.xyz) * 0.5 + 0.5, 1);
	float3 a = c_albedo.w > 0.5 ? lin(tex.rgb) : c_albedo.rgb;
	o.albedo = float4(a, c_mat.w > 0.5 ? 0 : 1);
	o.material = float4(c_cls.x + 0.49 * max(c_mat.y, 0), 0, c_mat.x >= 0 ? max(c_mat.x, 0.05) : -1, 1);   // MaterialBuffer: class + 0.49 metal, b roughness
	o.emissive = float4(c_emit.rgb * (c_emit.w > 0.5 ? lin(tex2D(s_glow, i.uv).rgb) : 1), 1);
	return o;
})";
static const char* kPsDepth = R"(
sampler2D s_diffuse : register(s0);
float4 c_mat : register(c1); float4 c_F : register(c6);
struct PI { float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 rel : TEXCOORD2; };
float4 main(PI i) : COLOR0 {
	if (c_mat.z >= 0) clip(tex2D(s_diffuse, i.uv).a - c_mat.z);
	return float4(dot(i.rel, c_F.xyz) / c_F.w, 0, 0, 1);    // TESR_DepthBuffer: view z / farZ
})";
// glass over the raster frame (the game draws its glass blended after the world): a faint tint + a sky-ish Fresnel sheen, alpha blended
static const char* kPsGlass = R"(
float4 c_albedo : register(c0); float4 c_F : register(c6);
float4 c_sky : register(c7);
struct PI { float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float3 rel : TEXCOORD2; };
float4 main(PI i) : COLOR0 {
	float3 n = normalize(i.n); float3 v = normalize(i.rel);
	float c = abs(dot(n, v)); float f = 0.04 + 0.96 * pow(1 - c, 5);
	float3 col = lerp(c_albedo.rgb * 0.3, c_sky.rgb, f);
	return float4(pow(max(col, 0), 1 / 2.2), 0.12 + 0.6 * f);
})";
static const char* kVsFull = R"(
float4 c_off : register(c0);
struct VO { float4 h : POSITION; float2 uv : TEXCOORD0; };
VO main(float2 p : POSITION) { VO o; o.h = float4(p + c_off.xy, 0, 1); o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); return o; })";
// The "game's frame": a plain raster shading of the G-buffer (sun x N.L [x the RT sun visibility], a sky ambient, the lamps with the
// game's falloff, unshadowed; emissive), the sky gradient of RtPathTrace.comp's Sky() + a sun disc where nothing was drawn
static const char* kPsDeferred = R"(
sampler2D s_depth : register(s0); sampler2D s_normals : register(s1); sampler2D s_albedo : register(s2); sampler2D s_emissive : register(s3);
sampler2D s_vis : register(s4);
float4 c_F : register(c0);     // forward (unit), w farZ
float4 c_Rp : register(c1);    // right / P00
float4 c_Up : register(c2);    // up / P11
float4 c_Rn : register(c3); float4 c_Un : register(c4);
float4 c_sunDir : register(c5);   // w 1: the sun lights
float4 c_sunCol : register(c6);   // w: the sun disc's cos
float4 c_zen : register(c7);      // w 1: outdoors
float4 c_hor : register(c8);
float4 c_amb : register(c9);      // x the sky ambient's share, w 1: x the RT sun visibility
float4 c_lampN : register(c10);
float4 c_lp[8] : register(c11);   // camera relative, w reach
float4 c_lc[8] : register(c19);
float3 Sky(float3 d) { if (d.z < 0) return c_hor.rgb * 0.3; return lerp(c_hor.rgb, c_zen.rgb, sqrt(d.z)); }
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
	float d = tex2D(s_depth, uv).r;
	float3 ray = c_F.xyz + c_Rp.xyz * (2 * uv.x - 1) - c_Up.xyz * (2 * uv.y - 1);
	float3 c;
	if (!(d > 0 && d < 0.9999)) {
		float3 dir = normalize(ray);
		c = Sky(dir);
		if (c_sunDir.w > 0.5 && dot(dir, c_sunDir.xyz) > c_sunCol.w) c += c_sunCol.rgb * 20;
	} else {
		float3 pos = ray * (d * c_F.w);
		float3 v = tex2D(s_normals, uv).rgb * 2 - 1;
		float3 n = normalize(c_Rn.xyz * v.x + c_Un.xyz * v.y + c_F.xyz * v.z);
		float4 a = tex2D(s_albedo, uv);
		float3 light = lerp(c_hor.rgb * 0.3, c_zen.rgb, saturate(n.z * 0.5 + 0.5)) * c_amb.x;
		if (c_sunDir.w > 0.5) light += c_sunCol.rgb * saturate(dot(n, c_sunDir.xyz)) * (c_amb.w > 0.5 ? tex2D(s_vis, uv).r : 1);
		for (int k = 0; k < 8; k++) {
			if (k < c_lampN.x) {
				float3 L = c_lp[k].xyz - pos; float dist = max(length(L), 1e-3); float x = dist / c_lp[k].w;
				light += c_lc[k].rgb * pow(saturate(1 - x * x), 2.2) * saturate(dot(n, L / dist));
			}
		}
		c = a.rgb * light + tex2D(s_emissive, uv).rgb;
	}
	return float4(pow(max(c, 0), 1 / 2.2), 1);    // the stored curve (the game's HDR scene)
})";
static const char* kPsTonemap = R"(
sampler2D s_scene : register(s0);
float4 c_tm : register(c0);   // x exposure, y 1: ACES fit (else clamp)
float3 Aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
	float3 c = pow(max(tex2D(s_scene, uv).rgb, 0), 2.2) * c_tm.x;
	c = c_tm.y > 0.5 ? Aces(c) : saturate(c);
	return float4(pow(c, 1 / 2.2), 1);
})";

// ------------------------------------------------------------------------------------------------------------------------------
// PNG (stored deflate blocks: no compression library needed)
static uint32_t Crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
	static uint32_t table[256]; static bool made = false;
	if (!made) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; table[i] = v; } made = true; }
	c = ~c; for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8); return ~c;
}
static bool WritePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
	std::vector<uint8_t> raw; raw.reserve(size_t(h) * (w * 3 + 1));
	for (uint32_t y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3); }
	std::vector<uint8_t> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
	for (size_t at = 0; at < raw.size(); at += 65535) {
		const size_t n = std::min<size_t>(65535, raw.size() - at);
		z.push_back(at + n >= raw.size() ? 1 : 0); z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
		z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
	}
	const uint32_t adler = (b << 16) | a;
	z.push_back(uint8_t(adler >> 24)); z.push_back(uint8_t(adler >> 16)); z.push_back(uint8_t(adler >> 8)); z.push_back(uint8_t(adler));
	FILE* f = nullptr; fopen_s(&f, path.c_str(), "wb");
	if (!f) return false;
	auto be = [&](uint32_t v) { uint8_t q[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) }; fwrite(q, 1, 4, f); };
	auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
		be(uint32_t(data.size())); std::vector<uint8_t> t(type, type + 4); t.insert(t.end(), data.begin(), data.end());
		fwrite(t.data(), 1, t.size(), f); be(Crc32(t.data(), t.size()));
	};
	const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 }; fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
	fclose(f);
	return true;
}

// ------------------------------------------------------------------------------------------------------------------------------
struct RtTex {               // a D3D9 render target (or plain) texture with its surface
	IDirect3DTexture9* t = nullptr; IDirect3DSurface9* s = nullptr; UINT w = 0, h = 0; D3DFORMAT f = D3DFMT_UNKNOWN;
};
struct FragPass { ID3D9NvrExtPipeline* pipe = nullptr; UINT images = 0, push = 0; };
struct RasterMesh { IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; UINT vertices = 0, triangles = 0; uint32_t rtId = 0; };

struct Camera {
	V3 eye = { -480, 380, 170 };
	float yaw = -0.62f, pitch = -0.12f;
	float fovY = 1.15f;   // ~66 degrees
	V3 f, r, u;
	float p00 = 1, p11 = 1, farZ = 20000;
	void update(float aspect) {
		f = V3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
		r = normalize(cross(f, V3(0, 0, 1))); u = cross(r, f);
		p11 = 1.0f / std::tan(fovY * 0.5f); p00 = p11 / aspect;
	}
};

// ------------------------------------------------------------------------------------------------------------------------------
struct App {
	// options
	std::string helperExe, spv, compute, device, outDir;
	UINT W = 640, H = 360, winW = 1280, winH = 720;
	double demoSeconds = 0;
	int cacheLog2 = 20;
	DWORD helperTimeout = 180000;
	// window / input
	HWND hwnd = nullptr;
	volatile bool quit = false;   // (set by the window, Esc, or the console handler's thread)
	bool captured = false, capturing = false;
	bool prevKey[256] = {};
	// the world
	PgHost host;
	Scene scene;
	Settings set;
	Camera cam, prevCam;
	bool prevCamValid = false;
	double simTime = 0;
	double demoTime = -1.0;   // the demo's script clock (>= 0 in --demo)
	// (2026-11-09 pt-mirrors) the demo's matched references: the camera and the script clock of each segment's screenshot, so a later
	// reference segment renders the SAME view with the animation frozen at that moment (freezeSim >= 0: the animation's clock)
	struct ShotPose { bool valid = false; V3 eye; float yaw = 0, pitch = 0; double time = 0; };
	std::vector<ShotPose> shotPoses;
	double freezeSim = -1.0;
	std::string hudTag;       // --tag, shown on the HUD (A/B frames carry their label)
	int hudBottom = 0;        // the HUD panel's height on the composite (the firefly count skips it)
	std::vector<RasterMesh> meshes;
	// shared images + trace shaders
	int imDepth = -1, imNormals = -1, imAlbedo = -1, imMaterial = -1, imFrame = -1, imA = -1, imB = -1, imDisplay = -1, imAlbedoOut = -1, imLamps = -1;
	int imAlpha = -1, imAlbedoAtlas = -1, imSunVis = -1, imSunHit = -1;
	int imSpec = -1;                // (2026-11-07) PTDenoiseSplit's specular signal (pass image 15, binding 19)
	int shPath = -1, shCache = -1, shSun = -1, shEmissive = -1;
	// D3D9 resources
	IDirect3DSurface9* ds = nullptr;
	RtTex gNormals, gAlbedo, gMaterial, gEmissive, gDepth, frame, display, albedoOut, sunVis, scene_, denoised, composite, park, white;
	RtTex specTex, specAlbedoA, specAlbedoB, specDenoised;   // (2026-11-07) PTDenoiseSplit: the specular light, its albedo (r, g | b, roughness), denoised
	// RtDenoise::Denoiser's textures (RGB mode; + history C with the specular mode)
	struct Denoiser { RtTex hist[2], histB[2], histC[2], chain[2], chainB[2], scratchB; bool valid = false, specular = false; uint32_t frame = 0; };
	Denoiser diffuseDn, specularDn;
	IDirect3DTexture9* lampTexture = nullptr;
	IDirect3DSurface9* backbuffer = nullptr;
	IDirect3DSurface9* shotSys = nullptr;
	IDirect3DVertexDeclaration9* declGeo = nullptr;
	IDirect3DVertexDeclaration9* declFull = nullptr;
	IDirect3DVertexBuffer9* fullQuad = nullptr;
	IDirect3DVertexShader9* vsGeo = nullptr; IDirect3DVertexShader9* vsFull = nullptr;
	IDirect3DVertexShader9* vsMotion = nullptr; IDirect3DPixelShader9* psMotion = nullptr;   // (2026-11-09) the object motion pass
	RtTex motion;                                   // (2026-11-09) TESR_ObjectMotionBuffer's stand-in: rgb world move since last frame, a 1
	std::vector<std::array<float, 12>> prevTransform;   // each object's transform last frame (the motion pass)
	bool motionThisFrame = false;
	IDirect3DPixelShader9* psGBuffer = nullptr; IDirect3DPixelShader9* psDepth = nullptr; IDirect3DPixelShader9* psDeferred = nullptr; IDirect3DPixelShader9* psTonemap = nullptr;
	IDirect3DPixelShader9* psGlass = nullptr;
	IDirect3DTexture9* fontTex = nullptr;
	int fontAdvance = 8, fontHeight = 16;
	// the fork's passes
	FragPass temporalRgb, temporalSpec, firefly, atrousRgb, show;
	ID3D9NvrExtSampler* pointClamp = nullptr;
	// path tracer state (as RtPathTrace.cpp's)
	RtRestirRow::State restirState;
	RtEmissiveRow::State emissiveState;
	int emissiveCount = 0;
	bool splitThisFrame = false;
	uint32_t samples = 0;           // the reference's running mean
	std::vector<float> lastKey;
	bool histValid = false;
	V3 prevEye; float prevBasis[3][4] = {}; float prevP[2] = { 1, 1 };
	uint64_t frameNumber = 0;
	// stats
	double fps = 0, frameMs = 0; int statFrames = 0; double statStart = 0;
	int shotCount = 0;
	std::string lastShot;

	// ---------------------------------------------------------------- setup
	bool MakeTex(RtTex& t, UINT w, UINT h, D3DFORMAT f, bool target = true) {
		t.w = w; t.h = h; t.f = f;
		PG_CHECK(host.dev->CreateTexture(w, h, 1, target ? D3DUSAGE_RENDERTARGET : 0, f, D3DPOOL_DEFAULT, &t.t, nullptr), "CreateTexture");
		t.t->GetSurfaceLevel(0, &t.s);
		return true;
	}
	bool Compile(const char* src, const char* profile, ID3DBlob** out) {
		ID3DBlob* err = nullptr;
		HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
		if (FAILED(hr)) { pgLog("  error: shader compile (%s): %s\n", profile, err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
		if (err) err->Release();
		return true;
	}
	bool MakeVs(const char* src, IDirect3DVertexShader9** vs) { ID3DBlob* b = nullptr; if (!Compile(src, "vs_3_0", &b)) return false; HRESULT hr = host.dev->CreateVertexShader((const DWORD*)b->GetBufferPointer(), vs); b->Release(); PG_CHECK(hr, "CreateVertexShader"); return true; }
	bool MakePs(const char* src, IDirect3DPixelShader9** ps) { ID3DBlob* b = nullptr; if (!Compile(src, "ps_3_0", &b)) return false; HRESULT hr = host.dev->CreatePixelShader((const DWORD*)b->GetBufferPointer(), ps); b->Release(); PG_CHECK(hr, "CreatePixelShader"); return true; }

	// a fork fragment pass (as RtDenoise.cpp Make / RtPathTrace.cpp ShowPipeline): images, push size, targets, the sampler index's offset
	bool MakeFrag(const char* name, UINT images, UINT push, UINT targets, UINT samplerOffset, FragPass& out) {
		FILE* f = nullptr; const std::string file = compute + "\\" + name + ".spv"; fopen_s(&f, file.c_str(), "rb");
		if (!f) { pgLog("  error: no %s\n", file.c_str()); return false; }
		std::vector<char> spirv; char buf[65536]; size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
		fclose(f);
		NVR_EXT_BINDING_DESC bindings[16] = {};
		for (UINT i = 0; i < images; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		bindings[images] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, samplerOffset };
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data(); desc.FragmentSpirvSize = UINT(spirv.size());
		desc.BindingCount = images + 1; desc.pBindings = bindings; desc.PushConstantSize = push;
		desc.UsesSamplerHeap = TRUE; desc.SamplerHeapSet = 0; desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = targets; desc.Blend = NVR_EXT_BLEND_NONE; desc.ColorWriteMask = targets == 1 && std::strcmp(name, "RtPathTraceShow") == 0 ? 0x7u : 0xFu;
		desc.pDebugName = name;
		PG_CHECK(host.ext->CreateGraphicsPipeline(&desc, &out.pipe), name);
		out.images = images; out.push = push;
		return true;
	}
	bool DrawFrag(const FragPass& p, IDirect3DSurface9* t0, IDirect3DSurface9* t1, IUnknown* const* images, const void* push,
		IDirect3DSurface9* t2 = nullptr, IDirect3DSurface9* t3 = nullptr) {
		NVR_EXT_RESOURCE_BINDING b[16] = {};
		for (UINT i = 0; i < p.images; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = images[i]; }
		b[p.images].Slot = 0; b[p.images].Type = NVR_EXT_BIND_SAMPLER; b[p.images].pResource = pointClamp;
		NVR_EXT_DRAW_DESC d = {};
		d.pPipeline = p.pipe; d.TargetCount = t3 ? 4 : t2 ? 3 : t1 ? 2 : 1;
		d.Targets[0].pResource = t0; d.Targets[1].pResource = t1; d.Targets[2].pResource = t2; d.Targets[3].pResource = t3;
		d.BindingCount = p.images + 1; d.pBindings = b; d.pPushData = push; d.PushDataSize = p.push;
		PG_CHECK(host.ext->Draw(&d), "fork Draw");
		return true;
	}

	bool MakeFont() {
		const int cols = 16, rows = 6, cell = 16, tw = cols * cell, th = rows * cell * 1;
		HDC dc = CreateCompatibleDC(nullptr);
		BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
		void* bits = nullptr;
		HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
		SelectObject(dc, bmp);
		HFONT font = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH, "Consolas");
		SelectObject(dc, font);
		SetBkColor(dc, RGB(0, 0, 0)); SetTextColor(dc, RGB(255, 255, 255));
		std::memset(bits, 0, size_t(tw) * th * 4);
		SIZE sz = {}; GetTextExtentPoint32A(dc, "M", 1, &sz); fontAdvance = sz.cx; fontHeight = std::min<int>(sz.cy, cell);
		for (int c = 32; c < 127; c++) { char ch = char(c); const int k = c - 32; TextOutA(dc, (k % cols) * cell, (k / cols) * cell, &ch, 1); }
		// char 127: a solid cell (the panel behind the text)
		{ const int k = 127 - 32; uint32_t* p = (uint32_t*)bits; for (int y = 0; y < cell; y++) for (int x = 0; x < cell; x++) p[((k / cols) * cell + y) * tw + (k % cols) * cell + x] = 0xFFFFFFFFu; }
		GdiFlush();
		IDirect3DTexture9* sys = nullptr;
		if (FAILED(host.dev->CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
			FAILED(host.dev->CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &fontTex, nullptr))) { pgLog("  error: font texture\n"); return false; }
		D3DLOCKED_RECT lr; sys->LockRect(0, &lr, nullptr, 0);
		for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
			const uint32_t v = ((const uint32_t*)bits)[y * tw + x] & 0xFF;
			((uint32_t*)((char*)lr.pBits + y * lr.Pitch))[x] = (v << 24) | 0xFFFFFFu;
		}
		sys->UnlockRect(0);
		host.dev->UpdateTexture(sys, fontTex); sys->Release();
		DeleteObject(font); DeleteObject(bmp); DeleteDC(dc);
		return true;
	}

	// a D3D9 texture of a CPU texture (all mips), for the raster
	bool MakeRasterTexture(CpuTexture& t) {
		IDirect3DTexture9* sys = nullptr; IDirect3DTexture9* gpu = nullptr;
		PG_CHECK(host.dev->CreateTexture(t.w, t.h, t.levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr), "texture sys");
		PG_CHECK(host.dev->CreateTexture(t.w, t.h, t.levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr), "texture");
		for (uint32_t l = 0; l < t.levels; l++) {
			const uint32_t s = std::max(t.w >> l, 1u);
			D3DLOCKED_RECT lr; sys->LockRect(l, &lr, nullptr, 0);
			for (uint32_t y = 0; y < s; y++) std::memcpy((char*)lr.pBits + y * lr.Pitch, &t.bytes[t.levelOffset[l] + size_t(y) * s * 4], s * 4);
			sys->UnlockRect(l);
		}
		host.dev->UpdateTexture(sys, gpu); sys->Release();
		t.d3d = gpu;
		return true;
	}
	// a shared image filled from floats once (through a DEFAULT texture, as the host's Upload)
	bool UploadOnce(int image, D3DFORMAT fmt, UINT w, UINT h, const std::function<void(char* row, UINT y)>& fill) {
		IDirect3DTexture9* sys = nullptr; IDirect3DTexture9* gpu = nullptr;
		PG_CHECK(host.dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_SYSTEMMEM, &sys, nullptr), "upload sys");
		PG_CHECK(host.dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_DEFAULT, &gpu, nullptr), "upload gpu");
		D3DLOCKED_RECT lr; sys->LockRect(0, &lr, nullptr, 0);
		for (UINT y = 0; y < h; y++) fill((char*)lr.pBits + y * lr.Pitch, y);
		sys->UnlockRect(0);
		PG_CHECK(host.dev->UpdateTexture(sys, gpu), "UpdateTexture");
		const bool ok = host.CopyIn(image, gpu, w, h);
		uploads.push_back(sys); uploads.push_back(gpu);
		return ok;
	}
	std::vector<IDirect3DTexture9*> uploads;

	bool Init() {
		// the window (client area winW x winH)
		WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nvrrtplayground";
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClassA(&wc);
		const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
		RECT rc = { 0, 0, LONG(winW), LONG(winH) }; AdjustWindowRect(&rc, style, FALSE);
		hwnd = CreateWindowExA(0, "nvrrtplayground", "Vegas Shaders RT playground", style, 40, 40, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
		SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)this);
		ShowWindow(hwnd, SW_SHOW);
		if (!host.Init(hwnd, winW, winH, device.c_str(), helperExe.c_str())) return false;
		host.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
		// the world
		BuildRoom(scene);
		// the shared images (one set; at most 64)
		imDepth = host.AddImage("depth", W, H, NVR_EXT_SHARED_R32F);
		imNormals = host.AddImage("normals", W, H, NVR_EXT_SHARED_RGBA16F);
		imAlbedo = host.AddImage("albedo", W, H, NVR_EXT_SHARED_RGBA16F);
		imMaterial = host.AddImage("material", W, H, NVR_EXT_SHARED_RGBA16F);
		imFrame = host.AddImage("frame", W, H, NVR_EXT_SHARED_RGBA16F);
		imA = host.AddImage("pt mean rg", W, H, NVR_EXT_SHARED_RG32F);
		imB = host.AddImage("pt mean b", W, H, NVR_EXT_SHARED_RG32F);
		imDisplay = host.AddImage("pt display", W, H, NVR_EXT_SHARED_RGBA16F);
		imAlbedoOut = host.AddImage("pt first albedo", W, H, NVR_EXT_SHARED_RGBA16F);
		imLamps = host.AddImage("pt lamps", kLightTexels, kLampRows, NVR_EXT_SHARED_RG32F);
		imAlpha = host.AddImage("alpha atlas", 32 * 64, 32 * 64, NVR_EXT_SHARED_R16F);       // AlphaDetail 64 (the shaders read the slot size off it)
		imAlbedoAtlas = host.AddImage("albedo atlas", 32 * 16, 32 * 16, NVR_EXT_SHARED_RGBA8);   // a stand-in: every textured object has a real texture
		imSunVis = host.AddImage("sun visibility", W, H, NVR_EXT_SHARED_R16F);
		imSunHit = host.AddImage("sun blocker", W, H, NVR_EXT_SHARED_R16F);
		imSpec = host.AddImage("pt specular", W, H, NVR_EXT_SHARED_RGBA16F);   // (always made: PTDenoiseSplit is live here)
		for (int i : { imDepth, imNormals, imAlbedo, imMaterial, imFrame, imA, imB, imDisplay, imAlbedoOut, imLamps, imAlpha, imAlbedoAtlas, imSunVis, imSunHit, imSpec })
			if (i < 0) return false;
		shPath = host.AddShader(spv + "\\RtPathTrace.spv");
		shCache = host.AddShader(spv + "\\RtPathTraceCache.spv");
		shSun = host.AddShader(spv + "\\RtSunShadow.spv");
		shEmissive = host.AddShader(spv + "\\RtEmissiveBuild.spv");
		for (auto& s : host.shaders) if (GetFileAttributesA(s.c_str()) == INVALID_FILE_ATTRIBUTES) { pgLog("  error: no %s\n", s.c_str()); return false; }
		// the scratch store: PTRestirDI's reservoirs + PTRadianceCache's table + PTEmissive's blocks and list (RtPathTrace.cpp Setup)
		host.scratchMB = (uint32_t)((EmissiveBaseWords(W, H, cacheLog2) * 4ull + 4ull * RtEmissiveRow::kScratchWords + (1ull << 20) - 1) >> 20) + 1u;
		pgLog("starting the helper (%s): %u images, %u shaders, scratch %u MB; the first start after a shader change compiles pipelines (up to a few minutes on an iGPU)\n",
			helperExe.c_str(), unsigned(host.images.size()), unsigned(host.shaders.size()), host.scratchMB);
		SetWindowTextA(hwnd, "Vegas Shaders RT playground - starting the RT helper (compiling pipelines)...");
		if (!host.Start(helperExe.c_str(), helperTimeout)) return false;

		// D3D9 resources
		PG_CHECK(host.dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, nullptr), "depth stencil");
		if (!MakeTex(gNormals, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(gAlbedo, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(gMaterial, W, H, D3DFMT_A16B16G16R16F) ||
			!MakeTex(gEmissive, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(gDepth, W, H, D3DFMT_R32F) || !MakeTex(frame, W, H, D3DFMT_A16B16G16R16F) ||
			!MakeTex(display, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(albedoOut, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(sunVis, W, H, D3DFMT_R16F) ||
			!MakeTex(scene_, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(denoised, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(composite, winW, winH, D3DFMT_A8R8G8B8) ||
			!MakeTex(park, 8, 8, D3DFMT_A8R8G8B8) || !MakeTex(white, 4, 4, D3DFMT_R16F) ||
			!MakeTex(specTex, W, H, D3DFMT_A16B16G16R16F) || !MakeTex(specAlbedoA, W, H, D3DFMT_G32R32F) || !MakeTex(specAlbedoB, W, H, D3DFMT_G32R32F) ||
			!MakeTex(specDenoised, W, H, D3DFMT_A16B16G16R16F)) return false;
		for (Denoiser* dn : { &diffuseDn, &specularDn }) {
			if (!MakeTex(dn->scratchB, W, H, D3DFMT_A16B16G16R16F)) return false;
			for (int i = 0; i < 2; i++)
				if (!MakeTex(dn->hist[i], W, H, D3DFMT_A16B16G16R16F) || !MakeTex(dn->histB[i], W, H, D3DFMT_A16B16G16R16F) || !MakeTex(dn->chain[i], W, H, D3DFMT_A16B16G16R16F) ||
					!MakeTex(dn->chainB[i], W, H, D3DFMT_A16B16G16R16F) || !MakeTex(dn->histC[i], W, H, D3DFMT_A16B16G16R16F)) return false;
		}
		specularDn.specular = true;
		host.dev->ColorFill(white.s, nullptr, D3DCOLOR_ARGB(255, 255, 255, 255));
		PG_CHECK(host.dev->CreateTexture(kLightTexels, kLampRows, 1, D3DUSAGE_DYNAMIC, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &lampTexture, nullptr), "lamp texture");
		PG_CHECK(host.dev->CreateOffscreenPlainSurface(winW, winH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &shotSys, nullptr), "screenshot surface");
		{
			const D3DVERTEXELEMENT9 geo[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0 },
				{ 0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
			const D3DVERTEXELEMENT9 full[] = { { 0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
			PG_CHECK(host.dev->CreateVertexDeclaration(geo, &declGeo), "decl"); PG_CHECK(host.dev->CreateVertexDeclaration(full, &declFull), "decl");
			PG_CHECK(host.dev->CreateVertexBuffer(6 * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &fullQuad, nullptr), "quad");
			float* q; fullQuad->Lock(0, 0, (void**)&q, 0);
			const float v[12] = { -1, -1, -1, 1, 1, 1, -1, -1, 1, 1, 1, -1 }; std::memcpy(q, v, sizeof(v)); fullQuad->Unlock();
		}
		if (!MakeVs(kVsMotion, &vsMotion) || !MakePs(kPsMotion, &psMotion) || !MakeTex(motion, W, H, D3DFMT_A16B16G16R16F)) return false;   // (2026-11-09)
		if (!MakeVs(kVsGeo, &vsGeo) || !MakeVs(kVsFull, &vsFull) || !MakePs(kPsGBuffer, &psGBuffer) || !MakePs(kPsDepth, &psDepth) ||
			!MakePs(kPsDeferred, &psDeferred) || !MakePs(kPsTonemap, &psTonemap) || !MakePs(kPsGlass, &psGlass) || !MakeFont()) return false;
		// the fork's passes, as RtDenoise.cpp Make (images, push size, targets, the sampler index's byte) and RtPathTrace.cpp ShowPipeline
		{ NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; PG_CHECK(host.ext->CreateSampler(&sd, &pointClamp), "CreateSampler"); }
		if (!MakeFrag("RtDenoiseTemporalRgb", 7, sizeof(TemporalPush), 3, offsetof(TemporalPush, samplerIndex), temporalRgb) ||
			!MakeFrag("RtDenoiseTemporalSpec", 10, sizeof(SpecPush), 4, offsetof(SpecPush, samplerIndex), temporalSpec) ||
			!MakeFrag("RtDenoiseFirefly", 3, sizeof(FireflyPush), 2, sizeof(FireflyPush) - 16, firefly) ||
			!MakeFrag("RtDenoiseAtrousRgb", 5, sizeof(AtrousPush), 2, sizeof(AtrousPush) - 16, atrousRgb) ||
			!MakeFrag("RtPathTraceShow", 10, 48, 1, 4, show)) return false;

		// the meshes: raster buffers + staged into the RT scene (one BUILD each), the real textures, the atlases; one exchange
		for (size_t m = 0; m < scene.meshes.size(); m++) {
			const Mesh& mesh = scene.meshes[m];
			RasterMesh rm; rm.vertices = UINT(mesh.pos.size()); rm.triangles = UINT(mesh.idx.size() / 3);
			PG_CHECK(host.dev->CreateVertexBuffer(rm.vertices * 32, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &rm.vb, nullptr), "vb");
			float* v; rm.vb->Lock(0, 0, (void**)&v, 0);
			std::vector<float> posUv; posUv.reserve(mesh.pos.size() * 5);
			for (size_t i = 0; i < mesh.pos.size(); i++) {
				const float row[8] = { mesh.pos[i].x, mesh.pos[i].y, mesh.pos[i].z, mesh.nrm[i].x, mesh.nrm[i].y, mesh.nrm[i].z, mesh.uv[2 * i], mesh.uv[2 * i + 1] };
				std::memcpy(v + i * 8, row, 32);
				posUv.insert(posUv.end(), { row[0], row[1], row[2], row[6], row[7] });
			}
			rm.vb->Unlock();
			PG_CHECK(host.dev->CreateIndexBuffer(UINT(mesh.idx.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &rm.ib, nullptr), "ib");
			void* p; rm.ib->Lock(0, 0, &p, 0); std::memcpy(p, mesh.idx.data(), mesh.idx.size() * 2); rm.ib->Unlock();
			// traced? (its op flags from the first object using it: texcoords for textured / cut-out ones)
			// (always staged with texcoords, as the RT host does: the op says so; the record says whether hits read them)
			const Object* user = nullptr;
			for (const Object& o : scene.objects) if (o.mesh == int(m) && o.traced) { user = &o; break; }
			// (glass: built non-opaque too, as RtScenePick::OpFlags: the rays see its candidates)
			const uint32_t opFlags = NVR_RT_OP_FLAG_TEXCOORDS | (user && (user->alphaTested || user->glass()) ? NVR_RT_OP_FLAG_ALPHA : 0u);
			if (user && !host.Stage(posUv, UINT(mesh.pos.size()), mesh.idx, opFlags, rm.rtId)) return false;
			meshes.push_back(rm);
		}
		for (CpuTexture& t : scene.textures) {
			if (!MakeRasterTexture(t)) return false;
			if (!host.StageTexture(t.id, 44 /* VK_FORMAT_B8G8R8A8_UNORM */, t.w, t.h, t.levels, t.bytes)) return false;
		}
		for (const Object& o : scene.objects) if (o.slot >= 0) host.SlotTextures(uint32_t(o.slot), o.diffuse ? o.diffuse->id : 0u, 0u, o.glowTex ? o.glowTex->id : 0u);
		// the alpha atlas: every slot 0 (alpha 0), the cut-outs' slots their 64-texel mip (the fallback; real textures read the full one)
		if (!UploadOnce(imAlpha, D3DFMT_R16F, 32 * 64, 32 * 64, [&](char* row, UINT y) {
			uint16_t* r = (uint16_t*)row; std::memset(r, 0, 32 * 64 * 2);
			for (const Object& o : scene.objects) {
				if (!o.alphaTested || !o.diffuse || int(y) / 64 != o.slot / 32) continue;
				const CpuTexture& t = *o.diffuse; uint32_t l = 0; while ((t.w >> l) > 64 && l + 1 < t.levels) l++;
				const uint32_t s = std::max(t.w >> l, 1u);
				for (int x = 0; x < 64; x++) {
					const float a = t.bytes[t.levelOffset[l] + ((size_t(y % 64) * s / 64) * s + size_t(x) * s / 64) * 4 + 3] / 255.0f;
					r[(o.slot % 32) * 64 + x] = ToHalf(a);
				}
			}
		})) return false;
		if (!UploadOnce(imAlbedoAtlas, D3DFMT_A8B8G8R8, 32 * 16, 32 * 16, [&](char* row, UINT) { std::memset(row, 0xBC, 32 * 16 * 4); })) return false;
		{ const double eye[3] = { 0, 0, 0 }; if (!host.Exchange({}, eye) || !host.Finish(60000)) return false; }
		host.ReleaseStaged();
		for (auto* t : uploads) t->Release();
		uploads.clear();
		pgLog("scene: %u meshes (%u traced), %u objects, %u lamps, %u real textures; helper: %u geometries, %llu triangles, %u failed ops\n",
			unsigned(scene.meshes.size()), unsigned(std::count_if(meshes.begin(), meshes.end(), [](const RasterMesh& m) { return m.rtId != 0; })),
			unsigned(scene.objects.size()), unsigned(scene.lamps.size()), unsigned(scene.textures.size()), host.shared->geometries,
			(unsigned long long)host.shared->triangles, host.shared->failedOps);
		ApplyView(0);
		return true;
	}

	// every D3D9 / fork object the app made (after host.Stop drained the GPU; then host.Release drops the device)
	template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
	static void Rel(RtTex& t) { Rel(t.s); Rel(t.t); }
	void ReleaseAll() {
		IDirect3DDevice9* d = host.dev;
		if (d) {   // nothing bound keeps a reference
			for (int i = 0; i < 4; i++) d->SetRenderTarget(i, i == 0 ? backbuffer : nullptr);
			for (int s = 0; s < 8; s++) d->SetTexture(s, nullptr);
			d->SetDepthStencilSurface(nullptr); d->SetStreamSource(0, nullptr, 0, 0); d->SetIndices(nullptr);
			d->SetVertexShader(nullptr); d->SetPixelShader(nullptr); d->SetVertexDeclaration(nullptr);
		}
		for (RtTex* t : { &gNormals, &gAlbedo, &gMaterial, &gEmissive, &gDepth, &frame, &display, &albedoOut, &sunVis, &scene_, &denoised, &composite, &park, &white,
			&specTex, &specAlbedoA, &specAlbedoB, &specDenoised }) Rel(*t);
		for (Denoiser* dn : { &diffuseDn, &specularDn }) {
			Rel(dn->scratchB);
			for (int i = 0; i < 2; i++) { Rel(dn->hist[i]); Rel(dn->histB[i]); Rel(dn->histC[i]); Rel(dn->chain[i]); Rel(dn->chainB[i]); }
		}
		Rel(ds); Rel(lampTexture); Rel(backbuffer); Rel(shotSys); Rel(declGeo); Rel(declFull); Rel(fullQuad); Rel(vsGeo); Rel(vsFull);
		Rel(vsMotion); Rel(psMotion); Rel(motion);
		Rel(psGBuffer); Rel(psDepth); Rel(psDeferred); Rel(psTonemap); Rel(psGlass); Rel(fontTex);
		for (FragPass* p : { &temporalRgb, &temporalSpec, &firefly, &atrousRgb, &show }) Rel(p->pipe);
		Rel(pointClamp);
		for (RasterMesh& m : meshes) { Rel(m.vb); Rel(m.ib); }
		for (CpuTexture& t : scene.textures) if (t.d3d) { ((IDirect3DTexture9*)t.d3d)->Release(); t.d3d = nullptr; }
		for (auto* t : uploads) t->Release();
		uploads.clear();
	}
	// the one way out (Esc, Alt+F4, the window's X, Ctrl+C / the console closing, the demo's end, an error): see PgHost::Stop
	bool Shutdown() {
		const bool clean = host.Stop();
		ReleaseAll();
		host.Release();
		if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
		return clean;
	}

	void UsePose(size_t step) {   // (pt-mirrors) a reference segment: the camera and the frozen clock of that step's screenshot
		if (step < shotPoses.size() && shotPoses[step].valid) { cam.eye = shotPoses[step].eye; cam.yaw = shotPoses[step].yaw; cam.pitch = shotPoses[step].pitch; freezeSim = shotPoses[step].time; }
	}
	void ApplyView(int i) {
		if (i < 0 || i >= int(scene.views.size())) return;
		cam.eye = scene.views[i].eye; cam.yaw = scene.views[i].yaw; cam.pitch = scene.views[i].pitch;
	}

	static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
		App* app = (App*)GetWindowLongPtrA(h, GWLP_USERDATA);
		if (msg == WM_CLOSE) { if (app) app->quit = true; return 0; }
		if (msg == WM_KILLFOCUS && app) app->CaptureMouse(false);
		return DefWindowProcA(h, msg, wp, lp);
	}
	void CaptureMouse(bool on) {
		if (on == captured) return;
		captured = on;
		ShowCursor(on ? FALSE : TRUE);
		if (on) { RECT r; GetClientRect(hwnd, &r); POINT c = { r.right / 2, r.bottom / 2 }; ClientToScreen(hwnd, &c); SetCursorPos(c.x, c.y); }
	}

	// ---------------------------------------------------------------- input (polled: GetAsyncKeyState, as the game's overlay under DXVK)
	bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
	bool Pressed(int vk) { const bool d = Down(vk); const bool p = d && !prevKey[vk]; prevKey[vk] = d; return p; }
	struct Toggle { int vk; const char* key; const char* name; int* v; int count; };
	std::vector<Toggle> toggles() {
		return {
			{ 'P', "P", "path tracer", &set.pt, 2 }, { 'N', "N", "PT mode", &set.ptMode, 3 }, { 'U', "U", "sun", &set.sun, 2 },
			{ 'K', "K", "lamps", &set.lamps, 2 }, { 'L', "L", "lamp moves", &set.lampAnim, 2 }, { 'J', "J", "cube spins", &set.spin, 2 },
			{ 'G', "G", "PTGlowLights", &set.glowLights, 2 }, { 'H', "H", "PTDetail", &set.detail, 2 }, { 'X', "X", "PTSpecular", &set.specular, 2 },
			{ 'R', "R", "PTRestirDI", &set.restirDI, 2 }, { 'Y', "Y", "PTRadianceCache", &set.cache, 2 }, { 'F', "F", "PTLampFalloff", &set.lampFalloff, 2 },
			{ 'Z', "Z", "PTRealtimeClamp 4", &set.indirectClamp, 2 }, { VK_F10, "F10", "PTBounceRoughen", &set.bounceRoughen, 2 },
			{ VK_F2, "F2", "PTEmissive", &set.emissive, 2 }, { VK_F3, "F3", "PTDenoiseSplit", &set.denoiseSplit, 2 },
			{ VK_F4, "F4", "PTDenoiseAntiFirefly", &set.antiFirefly, 2 }, { VK_F9, "F9", "FireflySpread 0/4/1", &set.fireflySpread, 3 },
			{ VK_F11, "F11", "PTDenoiseMirrorSurface", &set.mirrorSurface, 2 }, { '5', "5", "object motion", &set.objectMotion, 2 }, { VK_F5, "F5", "PTGlass", &set.glass, 3 },
			{ VK_F6, "F6", "PTFoliage", &set.foliage, 2 }, { VK_F7, "F7", "PTSkin", &set.skin, 2 }, { VK_F8, "F8", "PTContactShadows", &set.contact, 2 },
			{ 'O', "O", "PTSplit", &set.split, 2 }, { 'I', "I", "RT sun (PT off)", &set.rtSun, 2 }, { 'T', "T", "tonemap", &set.tonemap, 2 },
			{ 'V', "V", "PTDebugView", &set.debugView, int(sizeof(kDebugViews) / sizeof(kDebugViews[0])) }, { VK_F1, "F1", "HUD", &set.hud, 3 },
		};
	}
	void Input(float dt) {
		const bool focus = GetForegroundWindow() == hwnd;
		if (!focus) { std::memset(prevKey, 0, sizeof(prevKey)); return; }
		for (Toggle& t : toggles()) if (Pressed(t.vk)) { *t.v = (*t.v + 1) % t.count; }
		if (Pressed('B')) set.bounces = set.bounces >= 4 ? 1 : set.bounces + 1;
		for (int i = 0; i < 4; i++) if (Pressed('1' + i)) ApplyView(i);
		if (Pressed(VK_F12)) wantShot = "shot";
		if (Pressed('M')) CaptureMouse(!captured);
		if (Pressed(VK_ESCAPE)) { if (captured) CaptureMouse(false); else quit = true; }
		if (Down(VK_OEM_PLUS) || Down(VK_ADD)) set.exposure *= std::pow(2.0f, dt);
		if (Down(VK_OEM_MINUS) || Down(VK_SUBTRACT)) set.exposure *= std::pow(0.5f, dt);
		if (Down(VK_LEFT)) set.sunAz -= dt * 0.8f;
		if (Down(VK_RIGHT)) set.sunAz += dt * 0.8f;
		if (Down(VK_UP)) set.sunEl = std::min(set.sunEl + dt * 0.5f, 1.55f);
		if (Down(VK_DOWN)) set.sunEl = std::max(set.sunEl - dt * 0.5f, -0.2f);
		// mouse look: right button held, or captured (M)
		const bool rmb = Down(VK_RBUTTON);
		if (rmb && !captured) { CaptureMouse(true); capturing = true; }
		else if (!rmb && capturing) { CaptureMouse(false); capturing = false; }
		if (captured) {
			RECT r; GetClientRect(hwnd, &r); POINT c = { r.right / 2, r.bottom / 2 }; ClientToScreen(hwnd, &c);
			POINT p; GetCursorPos(&p);
			cam.yaw -= (p.x - c.x) * 0.0025f; cam.pitch -= (p.y - c.y) * 0.0025f;
			cam.pitch = std::min(std::max(cam.pitch, -1.5f), 1.5f);
			SetCursorPos(c.x, c.y);
		}
		const float speed = (Down(VK_SHIFT) ? 700.0f : 220.0f) * dt;
		const V3 fwd = normalize(V3(std::cos(cam.yaw), std::sin(cam.yaw), 0)), right = V3(fwd.y, -fwd.x, 0);
		if (Down('W')) cam.eye = cam.eye + fwd * speed;
		if (Down('S')) cam.eye = cam.eye - fwd * speed;
		if (Down('D')) cam.eye = cam.eye + right * speed;
		if (Down('A')) cam.eye = cam.eye - right * speed;
		if (Down(VK_SPACE) || Down('E')) cam.eye.z += speed;
		if (Down('C') || Down('Q')) cam.eye.z -= speed;
	}
	std::string wantShot;

	// ---------------------------------------------------------------- the frame
	V3 SunDir() const { return V3(std::cos(set.sunEl) * std::cos(set.sunAz), std::cos(set.sunEl) * std::sin(set.sunAz), std::sin(set.sunEl)); }
	bool SunUp() const { return set.sun && set.sunEl > 0.0f; }
	V3 SunColor() const { const float k = std::min(1.0f, std::max(0.0f, set.sunEl * 4.0f)); return V3(1.6f, 1.42f, 1.15f) * k; }   // linear (the game's sun x its light scale)
	V3 SkyZenith() const { return set.sun ? V3(0.20f, 0.33f, 0.62f) : V3(0.004f, 0.006f, 0.012f); }
	V3 SkyHorizon() const { return set.sun ? V3(0.52f, 0.56f, 0.62f) : V3(0.01f, 0.012f, 0.02f); }

	struct LampData { float pos[4], colour[4]; };
	std::vector<LampData> lampData;   // this frame's, nearest first (scene space = world - eye), as RtPathTrace.cpp Lamps()

	void Animate(double t) {
		for (Lamp& l : scene.lamps) { if (l.animate && set.lampAnim) l.animate(l, t); if (l.bulb >= 0) { scene.objects[l.bulb].pos = l.pos; scene.objects[l.bulb].visible = set.lamps != 0; } }
		for (Object& o : scene.objects) if (o.animate && set.spin) o.animate(o, t);
		lampData.clear();
		if (!set.lamps) return;
		struct L { LampData d; float dist; };
		std::vector<L> list;
		for (const Lamp& l : scene.lamps) {
			if (!l.on) continue;
			L x; const V3 q = l.pos - cam.eye;
			float reach = l.reach; V3 col = l.colour;
			if (set.lampFalloff == 1) { reach *= 2.0f; col = col * 1.6f; }   // PTLampFalloff 1: twice the reach, x PTLampGain 1.6 (RtPathTrace.cpp physical())
			put4(x.d.pos, q, reach); put4(x.d.colour, col, 0.0f);
			x.dist = std::max(length(q) - reach, 0.0f);
			list.push_back(x);
		}
		std::sort(list.begin(), list.end(), [](const L& a, const L& b) { return a.dist < b.dist; });
		for (const L& x : list) lampData.push_back(x.d);
	}

	std::vector<PgInstance> Instances() {
		std::vector<PgInstance> out;
		for (const Object& o : scene.objects) {
			if (!o.traced || !o.visible || meshes[o.mesh].rtId == 0) continue;
			PgInstance in;
			o.transform(in.transform);
			in.geometryId = meshes[o.mesh].rtId;
			in.customIndex = RtScenePick::CustomIndex(o.record());
			in.mask = o.mask();
			in.flags = 0x1u;
			in.albedo[0] = o.albedo.x; in.albedo[1] = o.albedo.y; in.albedo[2] = o.albedo.z;
			in.recordFlags = o.recordFlags();
			out.push_back(in);
		}
		return out;
	}

	void SetCameraVs() {
		const float n = 4.0f, f = cam.farZ;
		const float c[8][4] = { { cam.eye.x, cam.eye.y, cam.eye.z, 0 }, { cam.r.x, cam.r.y, cam.r.z, cam.p00 }, { cam.u.x, cam.u.y, cam.u.z, cam.p11 },
			{ cam.f.x, cam.f.y, cam.f.z, cam.farZ }, { f / (f - n), -n * f / (f - n), -1.0f / W, 1.0f / H } };
		host.dev->SetVertexShaderConstantF(3, &c[0][0], 5);
	}

	// 1. the G-buffer (D3D9 raster): MRT normals / albedo / material / emissive, then the depth (view z / farZ)
	void RasterGBuffer() {
		IDirect3DDevice9* d = host.dev;
		d->SetDepthStencilSurface(ds);
		d->SetVertexDeclaration(declGeo);
		d->SetVertexShader(vsGeo);
		SetCameraVs();
		d->SetRenderState(D3DRS_ZENABLE, TRUE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		for (int s = 0; s < 2; s++) {
			d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
			d->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 8); d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		}
		const float camPs[3][4] = { { cam.r.x, cam.r.y, cam.r.z, 0 }, { cam.u.x, cam.u.y, cam.u.z, 0 }, { cam.f.x, cam.f.y, cam.f.z, cam.farZ } };
		for (int pass = 0; pass < 2; pass++) {
			if (pass == 0) {
				for (IDirect3DSurface9* s : { gNormals.s, gAlbedo.s, gMaterial.s, gEmissive.s }) d->ColorFill(s, nullptr, 0);   // albedo a 0: no surface
				d->ColorFill(gDepth.s, nullptr, D3DCOLOR_ARGB(255, 255, 255, 255));                                            // 1 = the far plane
				d->SetRenderTarget(0, gNormals.s); d->SetRenderTarget(1, gAlbedo.s); d->SetRenderTarget(2, gMaterial.s); d->SetRenderTarget(3, gEmissive.s);
				d->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
				d->SetPixelShader(psGBuffer);
				d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE); d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
			}
			else {
				for (int i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
				d->SetRenderTarget(0, gDepth.s);
				d->SetPixelShader(psDepth);
				d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
			}
			d->SetPixelShaderConstantF(4, &camPs[0][0], 3);
			DrawObjects(false);
		}
		d->SetTexture(0, nullptr); d->SetTexture(1, nullptr);
		d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		d->SetDepthStencilSurface(nullptr);
	}
	// (2026-11-09, the owner's request) the object motion, as the game makes TESR_ObjectMotionBuffer (core\ObjectMotion.h): only the objects
	// whose transform changed since last frame are drawn again, each vertex placed twice (this frame's transform, last frame's), into an fp16
	// target: rgb = the world move since last frame (game units, world axes), a = 1, kept where the depth matches the G-buffer's (the surface
	// the screen shows there); every other pixel 0 = moved with the world. The denoisers subtract it before reprojecting (backbone.x 1) and
	// cap moving pixels at PTDenoiseMovingHistory (8), as RtPathTrace.cpp Show sets them with PTDenoiseObjectMotion. Key 5: off = as before.
	void MotionPass() {
		IDirect3DDevice9* d = host.dev;
		d->ColorFill(motion.s, nullptr, 0);
		motionThisFrame = false;
		const bool had = prevTransform.size() == scene.objects.size();
		if (!had) prevTransform.assign(scene.objects.size(), {});
		std::vector<size_t> moved;
		for (size_t i = 0; i < scene.objects.size(); i++) {
			const Object& o = scene.objects[i];
			float t[3][4]; o.transform(t);
			std::array<float, 12> now; std::memcpy(now.data(), t, sizeof(t));
			if (had && set.objectMotion && o.raster && o.visible && !o.glass() && now != prevTransform[i]) moved.push_back(i);
			else prevTransform[i] = now;
		}
		if (!set.objectMotion) return;
		motionThisFrame = true;   // (the buffer holds this frame's, also when nothing moved: all 0)
		if (moved.empty()) return;
		d->SetDepthStencilSurface(ds);
		d->SetRenderTarget(0, motion.s); for (int i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
		d->SetVertexDeclaration(declGeo); d->SetVertexShader(vsMotion); d->SetPixelShader(psMotion);
		SetCameraVs();
		d->SetRenderState(D3DRS_ZENABLE, TRUE); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		const float bias = -1.0e-5f; d->SetRenderState(D3DRS_DEPTHBIAS, *(const DWORD*)&bias);   // (the G-buffer's placement: a hair nearer)
		for (size_t i : moved) {
			const Object& o = scene.objects[i];
			const RasterMesh& m = meshes[o.mesh];
			float t[3][4]; o.transform(t);
			d->SetVertexShaderConstantF(0, &t[0][0], 3);
			d->SetVertexShaderConstantF(8, prevTransform[i].data(), 3);
			d->SetStreamSource(0, m.vb, 0, 32); d->SetIndices(m.ib);
			d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, m.vertices, 0, m.triangles);
			std::memcpy(prevTransform[i].data(), t, sizeof(t));
		}
		const float zero = 0.0f; d->SetRenderState(D3DRS_DEPTHBIAS, *(const DWORD*)&zero);
		d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		d->SetDepthStencilSurface(nullptr);
	}
	// the raster objects (glass only, or everything but glass) with the G-buffer pass' constants
	void DrawObjects(bool glassOnly) {
		IDirect3DDevice9* d = host.dev;
		for (const Object& o : scene.objects) {
			if (!o.raster || !o.visible || o.glass() != glassOnly) continue;
			const RasterMesh& m = meshes[o.mesh];
			float t[3][4]; o.transform(t);
			d->SetVertexShaderConstantF(0, &t[0][0], 3);
			const bool textured = o.diffuse && o.diffuse->d3d;
			const V3 emit = !o.traced ? o.emissive() : V3(set.ptGlow, set.ptGlow, set.ptGlow);   // traced glow: its glow map x PTGlow (as the path tracer)
			// raster only (the bulbs) and glass: the AlbedoBuffer has no colour there (albedo a 0: the game drew something it did not capture)
			const float pc[4][4] = { { o.albedo.x, o.albedo.y, o.albedo.z, textured ? 1.0f : 0.0f },
				{ o.rough, o.metal, o.alphaTested ? o.alphaRef / 255.0f : -1.0f, o.traced && !o.glass() ? 0.0f : 1.0f },
				{ o.glow > 0 ? emit.x : 0, o.glow > 0 ? emit.y : 0, o.glow > 0 ? emit.z : 0, o.glowTex && o.glowTex->d3d ? 1.0f : 0.0f },
				{ float(o.matClass), 0, 0, 0 } };
			d->SetPixelShaderConstantF(0, &pc[0][0], 4);
			d->SetTexture(0, textured ? (IDirect3DTexture9*)o.diffuse->d3d : white.t);
			d->SetTexture(1, o.glowTex && o.glowTex->d3d ? (IDirect3DTexture9*)o.glowTex->d3d : white.t);
			d->SetStreamSource(0, m.vb, 0, 32); d->SetIndices(m.ib);
			d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, m.vertices, 0, m.triangles);
		}
	}
	// glass, after the frame is shaded (as the game draws it after the world): into the G-buffer (normals / material / albedo a 0, the depth)
	// over what lies behind, and blended over the frame
	void RasterGlass() {
		IDirect3DDevice9* d = host.dev;
		bool any = false;
		for (const Object& o : scene.objects) if (o.glass() && o.raster && o.visible) any = true;
		if (!any) return;
		d->SetDepthStencilSurface(ds);
		d->SetVertexDeclaration(declGeo); d->SetVertexShader(vsGeo); SetCameraVs();
		d->SetRenderState(D3DRS_ZENABLE, TRUE); d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		const float camPs[3][4] = { { cam.r.x, cam.r.y, cam.r.z, 0 }, { cam.u.x, cam.u.y, cam.u.z, 0 }, { cam.f.x, cam.f.y, cam.f.z, cam.farZ } };
		d->SetPixelShaderConstantF(4, &camPs[0][0], 3);
		d->SetRenderTarget(0, gNormals.s); d->SetRenderTarget(1, gAlbedo.s); d->SetRenderTarget(2, gMaterial.s); d->SetRenderTarget(3, gEmissive.s);
		d->SetPixelShader(psGBuffer); DrawObjects(true);
		for (int i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
		d->SetRenderTarget(0, gDepth.s); d->SetPixelShader(psDepth); DrawObjects(true);
		d->SetRenderTarget(0, frame.s); d->SetPixelShader(psGlass);
		const float sky[4] = { SkyZenith().x * set.ptSky, SkyZenith().y * set.ptSky, SkyZenith().z * set.ptSky, 0 };
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		for (const Object& o : scene.objects) {
			if (!o.glass() || !o.raster || !o.visible) continue;
			const RasterMesh& m = meshes[o.mesh];
			float t[3][4]; o.transform(t);
			d->SetVertexShaderConstantF(0, &t[0][0], 3);
			const float pc[4] = { o.albedo.x, o.albedo.y, o.albedo.z, 0 };
			d->SetPixelShaderConstantF(0, pc, 1); d->SetPixelShaderConstantF(7, sky, 1);
			d->SetStreamSource(0, m.vb, 0, 32); d->SetIndices(m.ib);
			d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, m.vertices, 0, m.triangles);
		}
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		d->SetTexture(0, nullptr); d->SetTexture(1, nullptr);
		d->SetDepthStencilSurface(nullptr);
	}

	void FullScreen(IDirect3DSurface9* target, UINT tw, UINT th, IDirect3DPixelShader9* ps) {
		IDirect3DDevice9* d = host.dev;
		d->SetRenderTarget(0, target);
		d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		d->SetVertexDeclaration(declFull); d->SetVertexShader(vsFull); d->SetPixelShader(ps);
		const float off[4] = { -1.0f / tw, 1.0f / th, 0, 0 };
		d->SetVertexShaderConstantF(0, off, 1);
		d->SetStreamSource(0, fullQuad, 0, 8);
		d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
	}

	// 2. the "game's frame": the raster shading of the G-buffer (with the RT sun visibility when given)
	void Deferred(bool rtVisibility) {
		IDirect3DDevice9* d = host.dev;
		for (int i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
		IDirect3DTexture9* tex[5] = { gDepth.t, gNormals.t, gAlbedo.t, gEmissive.t, rtVisibility ? sunVis.t : white.t };
		for (int s = 0; s < 5; s++) {
			d->SetTexture(s, tex[s]);
			d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		}
		const V3 sun = SunDir(), sc = SunColor(), zen = SkyZenith() * set.ptSky, hor = SkyHorizon() * set.ptSky;
		float c[27][4] = {};
		put4(c[0], cam.f, cam.farZ); put4(c[1], cam.r * (1.0f / cam.p00), 0); put4(c[2], cam.u * (1.0f / cam.p11), 0);
		put4(c[3], cam.r, 0); put4(c[4], cam.u, 0);
		put4(c[5], sun, SunUp() ? 1.0f : 0.0f); put4(c[6], sc, std::cos(0.0175f));
		put4(c[7], zen, 1.0f); put4(c[8], hor, 0.0f);
		put4(c[9], 0.35f, 0, 0, rtVisibility ? 1.0f : 0.0f);
		const int n = std::min<int>(int(lampData.size()), 8);
		c[10][0] = float(n);
		for (int k = 0; k < n; k++) { std::memcpy(c[11 + k], lampData[k].pos, 16); std::memcpy(c[19 + k], lampData[k].colour, 16); }
		d->SetPixelShaderConstantF(0, &c[0][0], 27);
		FullScreen(frame.s, W, H, psDeferred);
		for (int s = 0; s < 5; s++) d->SetTexture(s, nullptr);
	}

	// the pass's camera block (RtPathTrace.cpp: forward + farZ, right / P00, up / P11; the eye at the scene's origin)
	void CameraBlock(float* size, float* forward, float* right, float* up, float* origin) {
		put4(size, float(W), float(H), 1.0f / W, 1.0f / H);
		put4(forward, cam.f, cam.farZ);
		put4(right, cam.r * (1.0f / cam.p00), 0.0f);
		put4(up, cam.u * (1.0f / cam.p11), 0.0f);
		put4(origin, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	// 3. the path tracer's pass, as RtPathTrace.cpp Record() under PTRealtime (mode 0 / 2) or the reference (mode 1). `inst`: this frame's
	// instance list (PTEmissive marks its glowing records' bits 8-15, as RtCore::MarkEmissive)
	uint32_t ptFlags = 0;
	bool TracePass(std::vector<PgInstance>& inst) {
		const bool realtime = set.ptMode != 1;
		const bool denoise = realtime;   // PTDenoise: each frame its own paths, demodulated (the denoiser or the raw view after it)
		const int bounces = realtime ? set.bounces : std::max(set.bounces, 3);
		const int bounceAlpha = realtime ? 3 : 1;
		const int lastSky = realtime && bounces == 1 ? 1 : 0, lastBounce = lastSky;
		const bool restir = realtime && set.restirDI;
		const bool cache = realtime && set.cache;
		const bool emissiveOn = realtime && set.emissive;   // PTEmissive -1: on under PTRealtime only
		RtMaterials::Keys materials = set.Materials();
		// (2026-11-09 pt-mirrors) PTDenoiseMirrorSurface rides in texel 26.y while the split specular light goes through the denoiser
		materials.mirrorSurface = denoise && set.denoiseSplit && set.mirrorSurface ? 0.1f : 0.0f;
		// the inputs: the G-buffer and the frame (flags 8, 256, 32), as RtCore::CopyIn
		uint32_t flags = 1u | 2u | 4u;   // the alpha test, the albedo atlas (the real textures), the glow textures
		if (host.CopyIn(imDepth, gDepth.t) && host.CopyIn(imNormals, gNormals.t) && host.CopyIn(imAlbedo, gAlbedo.t)) flags |= 8u;
		if ((flags & 8u) && set.specular && host.CopyIn(imMaterial, gMaterial.t)) flags |= 256u;
		if (set.specular) flags |= 128u;
		flags |= 2048u;                                  // PTGameLamps 4: the game's falloff
		flags |= 8192u;                                  // PTGlowFromFrame
		if (lastSky) flags |= 32768u;
		if (lastBounce) flags |= 65536u;
		if (set.lampFalloff == 1) flags |= 131072u;
		if (denoise) flags |= 512u;
		if ((flags & 512u) && set.denoiseSplit && imSpec >= 0) flags |= 4194304u;   // (2026-11-07) PTDenoiseSplit
		if (host.CopyIn(imFrame, frame.t)) flags |= 32u;
		// the reference: a running mean while nothing moves (RtPathTrace.cpp Moved)
		std::vector<float> key = { cam.eye.x, cam.eye.y, cam.eye.z, cam.yaw, cam.pitch, set.sunAz, set.sunEl, float(set.sun), float(set.lamps), float(bounces),
			float(set.specular), float(set.lampFalloff), float(set.debugView), float(set.glowLights), set.ptGlow, float(flags), float(set.emissive),
			materials.glass, materials.foliage, materials.skin, float(set.indirectClamp), float(set.contact) };
		for (const LampData& l : lampData) key.insert(key.end(), l.pos, l.pos + 3);
		for (const Object& o : scene.objects) if (o.animate) { key.push_back(o.yaw); key.push_back(o.pos.z); key.push_back(float(o.visible)); }
		if (key != lastKey) { samples = 0; lastKey = key; }
		if (denoise) samples = 0;
		else if (samples >= 4096) return true;   // PTMaxSamples: converged, the last picture stays
		PathConstants c = {};
		CameraBlock(c.size, c.forward, c.right, c.up, c.origin);
		c.origin[3] = float((uint32_t(frameNumber) * 7919u) & 0xFFFFFu);
		const V3 sun = SunDir(), sc = SunColor();
		put4(c.sunDir, sun, SunUp() ? 1.0f : 0.0f);
		put4(c.sunColor, sc, std::tan(0.5f * 0.53f * 0.0174532925f));
		put4(c.skyZenith, SkyZenith() * set.ptSky, 1.0f);   // outdoors: the sky on misses
		put4(c.skyHorizon, SkyHorizon() * set.ptSky, set.ptGlow);
		put4(c.ambient, 0, 0, 0, set.ptSky);
		const int first = std::min<int>(int(lampData.size()), 8);
		for (int i = 0; i < first; i++) { std::memcpy(c.lampPos[i], lampData[i].pos, 16); std::memcpy(c.lampColor[i], lampData[i].colour, 16); }
		c.misc[0] = float(first);
		// the glowing traced instances (RtCore::GlowLights(): a glow record, not alpha-tested): their index in the frame record, bound sphere
		// (camera relative), colour guess, geometry, triangles, transform
		struct Glow { uint32_t record; V3 centre; float radius; V3 rgb; uint32_t geometryId, triangles; float transform[3][4]; };
		std::vector<Glow> glowing;
		{
			uint32_t rec = 0;
			for (const Object& o : scene.objects) {
				if (!o.traced || !o.visible || meshes[o.mesh].rtId == 0) continue;
				const uint32_t index = rec++;
				if (!(o.glow > 0) || o.alphaTested) continue;
				const Mesh& m = scene.meshes[o.mesh];
				Glow g; g.record = index; g.rgb = o.albedo; g.geometryId = meshes[o.mesh].rtId; g.triangles = uint32_t(m.idx.size() / 3);
				o.transform(g.transform);
				V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
				for (const V3& p : m.pos) {
					const V3 w(g.transform[0][0] * p.x + g.transform[0][1] * p.y + g.transform[0][2] * p.z + g.transform[0][3], g.transform[1][0] * p.x + g.transform[1][1] * p.y + g.transform[1][2] * p.z + g.transform[1][3],
						g.transform[2][0] * p.x + g.transform[2][1] * p.y + g.transform[2][2] * p.z + g.transform[2][3]);
					lo = V3(std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)); hi = V3(std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z));
				}
				g.centre = (lo + hi) * 0.5f - cam.eye; g.radius = length(hi - lo) * 0.5f;
				g.transform[0][3] -= cam.eye.x; g.transform[1][3] -= cam.eye.y; g.transform[2][3] -= cam.eye.z;   // camera relative, as the instances
				glowing.push_back(g);
			}
		}
		// PTEmissive (needs the glow, flag 4, the lamp image, its build pass, the cache's table sized: RtPathTrace.cpp emissiveWanted)
		const bool emissiveWanted = emissiveOn && set.ptGlow > 0.0f && shEmissive >= 0 && (flags & 4u) && cacheLog2 > 0;
		// the glow lights (PTGlowLights; none with PTEmissive: it replaces them)
		std::vector<LampData> glows;
		if (set.glowLights && set.ptGlow > 0 && !emissiveWanted) for (const Glow& g : glowing) {
			if (!(g.radius > 0) || g.radius > 512.0f || int(glows.size()) >= kGlowMax) continue;
			LampData d; put4(d.pos, g.centre, g.radius * 1.01f + 1.0f);
			put4(d.colour, (g.rgb + V3(0.05f, 0.05f, 0.05f)) * (set.ptGlow * 0.25f), 0.0f);
			glows.push_back(d);
		}
		// the lamp image (flag 1024): the lamps, the glow lights, the frame row (ReSTIR DI / cache keys, the GGX energy table, PTContactShadows,
		// the materials' keys, PTEmissive's count), the emissive row
		const int lampCount = std::min<int>(int(lampData.size()), kLampsMax);
		emissiveCount = 0;
		D3DLOCKED_RECT r = {};
		if (SUCCEEDED(lampTexture->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) {
			for (int y = 0; y < kLampRows; y++) std::memset((char*)r.pBits + size_t(y) * r.Pitch, 0, sizeof(float) * 2 * kLightTexels);
			float* t = (float*)r.pBits;
			for (int i = 0; i < lampCount; i++) { const LampData& d = lampData[i]; const float v[8] = { d.pos[0], d.pos[1], d.pos[2], d.pos[3], d.colour[0], d.colour[1], d.colour[2], 0.0f }; std::memcpy(t + 8 * i, v, 32); }
			for (size_t i = 0; i < glows.size(); i++) { const LampData& d = glows[i]; const float v[8] = { d.pos[0], d.pos[1], d.pos[2], d.pos[3], d.colour[0], d.colour[1], d.colour[2], 0.0f }; std::memcpy(t + 8 * (lampCount + i), v, 32); }
			float* row = (float*)((char*)r.pBits + size_t(r.Pitch) * kFrameRow);
			RtRestirRow::Keys keys;   // the game's defaults (PTRestirDI*, PTCache*)
			keys.cacheLog2 = float(cacheLog2); keys.cacheBase4096 = float(CacheBaseWords(W, H) / 4096ull);
			keys.cacheCell = 8.0f; keys.cacheMaxSamples = 256.0f; keys.cacheMinSamples = 8.0f;
			const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
			const float eyeScene[3] = { 0, 0, 0 };
			RtRestirRow::Build(restirState, row, t, lampCount ? lampData[0].pos : nullptr, lampCount, 8, camWorld, eyeScene, c.forward, c.right, c.up, W, H, keys);
			row[30] = 0.0f; row[31] = 0.0f;   // texel 15: the flashlight's cone (none)
			RtTessContact::PackRow(row, RtTessContact::Slack(), RtTessContact::Make((flags & 8u) ? float(set.contact) : 0.0f, 24.0f));   // texels 16-21
			RtMaterials::PackRow(row, materials);   // texels 24-26
			row[54] = set.bounceRoughen ? 0.35f : 0.0f; row[55] = 0.0f;   // texel 27: PTBounceRoughen (2026-11-09)
			float* emRow = (float*)((char*)r.pBits + size_t(r.Pitch) * kEmissiveRow);
			if (emissiveWanted) {
				std::vector<RtEmissiveRow::Source> sources;
				for (const Glow& g : glowing) {
					RtEmissiveRow::Source s;
					s.record = g.record; s.geometryId = g.geometryId; s.triangles = g.triangles;
					std::memcpy(s.transform, g.transform, sizeof(s.transform));
					s.center[0] = g.centre.x; s.center[1] = g.centre.y; s.center[2] = g.centre.z; s.radius = g.radius;
					s.rgb[0] = g.rgb.x; s.rgb[1] = g.rgb.y; s.rgb[2] = g.rgb.z;
					sources.push_back(s);
				}
				std::vector<RtEmissiveRow::Mark> marks;
				emissiveCount = RtEmissiveRow::Build(emissiveState, sources.data(), int(sources.size()), 64, RtEmissiveRow::kTrianglesMax, camWorld, emRow, marks);
				for (const RtEmissiveRow::Mark& m : marks) if (m.record < inst.size()) inst[m.record].recordFlags = RtScenePick::WithEmissive(inst[m.record].recordFlags, m.number);
			}
			else emissiveState = RtEmissiveRow::State();
			row[2 * RtEmissiveRow::kFrameTexel] = float(emissiveCount);
			row[2 * RtEmissiveRow::kFrameTexel + 1] = float(EmissiveBaseWords(W, H, cacheLog2) / 4096ull);
			lampTexture->UnlockRect(0);
			if (host.CopyIn(imLamps, lampTexture)) {
				flags |= 1024u | (restir ? 262144u : 0u) | (cache ? 524288u : 0u) | 1048576u;
				c.misc[0] = float(lampCount); c.rect[2] = float(glows.size());
			}
			else { restirState.valid = false; emissiveCount = 0; }
		}
		if (!(flags & 262144u)) restirState.valid = false;
		ptFlags = flags;
		c.accum[0] = float(samples); c.accum[1] = float(bounces); c.accum[2] = 1.0f; c.accum[3] = float(flags);
		c.misc[1] = 0.0f; c.misc[2] = 60000.0f; c.misc[3] = float(kDebugViews[set.debugView]);
		c.rect[3] = float(bounceAlpha);
		c.grid[2][2] = 0.0f;                                  // PTBounceDistance 0 (60000)
		c.grid[2][3] = set.indirectClamp ? 4.0f : 0.0f;       // PTIndirectClamp
		// PTEmissive's list, before the trace (RtEmissiveBuild.spv: the lamp image + the glow atlas, one workgroup an instance)
		if (emissiveCount > 0 && (flags & 1024u)) {
			const float ec[4] = { float(emissiveCount), float(EmissiveBaseWords(W, H, cacheLog2) / 4096ull), 0.0f, 0.0f };
			host.AddPass(shEmissive, { imLamps, imAlbedoAtlas }, 0u, ec, sizeof(ec), uint32_t(emissiveCount), 1);
		}
		// images as RtPathTrace.cpp: the mean (2), the display, the atlases, the G-buffer, the sky (a stand-in: no panorama), the frame, the
		// normal atlas (a stand-in), the material, the first albedo, the lamp image; + (PTDenoiseSplit) the specular image
		const bool split = (flags & 4194304u) != 0u;
		std::vector<int> imgs = { imA, imB, imDisplay, imAlpha, imAlbedoAtlas, imAlbedoAtlas, imDepth, imNormals, imAlbedo, imFrame, imFrame,
			imAlbedoAtlas, imMaterial, imAlbedoOut, imLamps };
		if (split) imgs.push_back(imSpec);
		host.AddPass(shPath, imgs, 0x7u | (flags & 512u ? 1u << 13 : 0u) | (split ? 1u << 15 : 0u), &c, sizeof(c), (W + 7) / 8, (H + 7) / 8);
		if (flags & 524288u) {
			const float params[8] = { float(1u << cacheLog2), float(CacheBaseWords(W, H) / 4096ull), 256.0f, 120.0f, 8.0f, 0.0f, 0.0f, 0.0f };
			host.AddPass(shCache, {}, 0u, params, sizeof(params), ((1u << cacheLog2) + 63) / 64, 1);
		}
		if (!denoise) samples++;
		return true;
	}

	// the RT sun shadows for the raster view (RtSunShadows.cpp: hard, one ray a pixel at full size)
	void SunPass() {
		host.CopyIn(imDepth, gDepth.t); host.CopyIn(imNormals, gNormals.t);
		SunConstants k = {};
		CameraBlock(k.size, k.forward, k.right, k.up, k.origin);
		put4(k.sun, SunDir(), SunUp() ? 1.0f : 0.0f);
		put4(k.bias, 1.0f, 0.001f, 0.5f, 30000.0f);
		put4(k.range, 100000.0f, 1.0f, 255.0f, 0.0f);
		put4(k.alpha, 1.0f, float(frameNumber & 0xFFFF), 0.0f, 0.0f);
		put4(k.full, float(W), float(H), 0.0f, 0.0f);
		put4(k.camera, cam.eye, 0.0f);
		host.AddPass(shSun, { imDepth, imNormals, imSunVis, imAlpha, imNormals, imSunHit }, 0x4u, &k, sizeof(k), (W + 7) / 8, (H + 7) / 8);
	}

	// 4. RtDenoise::Run, RGB mode, with the path tracer's settings (RtPathTrace.cpp Show under PTRealtime): PTDenoiseHistory 32, 2 a-trous
	// passes, PTDenoiseSigma 1, AntiLag 3, Guide 0.35 (diffuse only), HistoryFix 3, MeanVariance 2, AntiFirefly (PTDenoiseFireflyK 2);
	// spec: the specular mode (RtDenoiseTemporalSpec: the roughness = the specular albedo's b / roughness image, history C, MirrorHistory 12,
	// the virtual reprojection). (2026-11-09) the object motion (MotionPass, key 5) as PTDenoiseObjectMotion; no DenoiseBackbone, no people (stand-ins as RtDenoise.cpp passes them).
	bool RunDenoiser(Denoiser& dn, IUnknown* noisy, IDirect3DSurface9* output, bool spec) {
		const V3 move = cam.eye - prevEye;
		const bool cut = !histValid || !dn.valid || length(move) > 512.0f;
		TemporalPush t = {};
		// last frame's camera-relative view-projection (clip = M p; column-major for GLSL: column = world axis): x right P00, y up P11, w forward
		for (int col = 0; col < 3; col++) { t.prevViewProj[col * 4 + 0] = prevBasis[0][col] * prevP[0]; t.prevViewProj[col * 4 + 1] = prevBasis[1][col] * prevP[1]; t.prevViewProj[col * 4 + 3] = prevBasis[2][col]; }
		put4(t.forward, cam.f, cam.farZ); put4(t.right, cam.r * (1.0f / cam.p00), 0); put4(t.up, cam.u * (1.0f / cam.p11), 0);
		put4(t.move, move, cut ? 1.0f : 0.0f);
		put4(t.params, set.denoiseHistory, 0.03f, 1.0f / W, 1.0f / H);
		put4(t.rect, 1, 1, 1, 1);
		const bool ff = set.antiFirefly && firefly.pipe;
		t.clampK = 0.0f; t.peopleCap = 0.0f; t.antiLag = set.denoiseAntiLag; t.fireflyK = ff ? set.fireflyK + 128.0f * (set.fireflySpread == 1 ? 4.0f : set.fireflySpread == 2 ? 1.0f : 0.0f) : 0.0f;	// (2026-11-09: + 128 x FireflySpread)
		const uint32_t last = dn.frame & 1, now = (dn.frame + 1) & 1;
		// AntiFirefly: the temporal pass writes A / B into chain[0] / chainB[0] and the clamped excess into scratchB, RtDenoiseFirefly spreads
		// the excess into the history; off: A / B straight into the history
		IDirect3DSurface9* nowA = ff ? dn.chain[0].s : dn.hist[now].s;
		IDirect3DSurface9* nowB = ff ? dn.chainB[0].s : dn.histB[now].s;
		// (2026-11-09) the object motion as RtDenoise::Run binds it (Settings.ObjectMotion + MovingHistory 8: PTDenoiseObjectMotion), key 5
		const bool om = set.objectMotion && motionThisFrame;
		t.backbone[0] = om ? 1.0f : 0.0f; t.backbone[2] = om ? 8.0f : 0.0f;
		IUnknown* motionImage = om ? (IUnknown*)motion.t : (IUnknown*)gDepth.t;
		IUnknown* temporalImages[7] = { noisy, dn.hist[last].t, gDepth.t, dn.histB[last].t, motionImage, gDepth.t, gDepth.t };
		if (!spec) { if (!DrawFrag(temporalRgb, nowA, nowB, temporalImages, &t, dn.scratchB.s)) return false; }
		else {
			SpecPush ts = {};
			std::memcpy(&ts, &t, offsetof(TemporalPush, samplerIndex));
			ts.spec[0] = set.mirrorHistory; ts.spec[1] = 1.0f;   // PTDenoiseMirrorHistory, PTDenoiseVirtual
			ts.spec[2] = set.mirrorSurface ? 1.0f : 0.0f;        // (2026-11-09 pt-mirrors) PTDenoiseMirrorSurface
			ts.clampK = t.clampK; ts.peopleCap = t.peopleCap; ts.antiLag = t.antiLag; ts.fireflyK = t.fireflyK;
			IUnknown* specImages[10] = { noisy, dn.hist[last].t, gDepth.t, dn.histB[last].t, motionImage, gDepth.t, gDepth.t, specAlbedoB.t, dn.histC[last].t, gNormals.t };
			if (!DrawFrag(temporalSpec, nowA, nowB, specImages, &ts, dn.histC[now].s, dn.scratchB.s)) return false;
		}
		if (ff) {
			FireflyPush f = {}; f.size[0] = 1.0f / W; f.size[1] = 1.0f / H;
			IUnknown* images[3] = { dn.chain[0].t, dn.scratchB.t, dn.chainB[0].t };
			if (!DrawFrag(firefly, dn.hist[now].s, dn.histB[now].s, images, &f)) return false;
		}
		AtrousPush a = {};
		put4(a.params, 0.0f, 1.0f, 64.0f, set.denoiseSigma);
		put4(a.size, 1.0f / W, 1.0f / H, set.denoiseMeanVariance > 1.0f ? set.denoiseMeanVariance : 0.0f, 0.0f);
		const bool guide = !spec && set.denoiseGuide > 0.0f;
		a.guideSigma = guide ? set.denoiseGuide : 0.0f;
		const bool mirror = spec && set.mirrorSurface;   // (2026-11-09 pt-mirrors) bit 2 + the raw image's hit distance in the guide albedo's slot
		a.specular = (spec ? 1u : 0u) | (ff ? 2u : 0u) | (mirror ? 4u : 0u);
		a.historyFix = set.denoiseHistoryFix;
		RtTex* srcA = &dn.hist[now]; RtTex* srcB = &dn.histB[now];
		const int iterations = std::max(0, std::min(set.denoiseIterations, 5));
		for (int i = 0; i < std::max(iterations, 1); i++) {
			const bool lastPass = i == std::max(iterations, 1) - 1;
			a.params[0] = iterations ? float(1 << i) : 0.0f;
			IUnknown* images[5] = { srcA->t, gNormals.t, srcB->t, spec ? (IUnknown*)specAlbedoB.t : guide ? (IUnknown*)frame.t : (IUnknown*)srcA->t,
				guide ? (IUnknown*)albedoOut.t : mirror ? noisy : (IUnknown*)srcA->t };
			if (!DrawFrag(atrousRgb, lastPass ? output : dn.chain[i & 1].s, lastPass ? dn.scratchB.s : dn.chainB[i & 1].s, images, &a)) return false;
			srcA = &dn.chain[i & 1]; srcB = &dn.chainB[i & 1];
		}
		dn.valid = true;
		dn.frame++;
		return true;
	}

	// 5. RtPathTrace.cpp Show(hdr = true): the traced picture into the HDR scene (PTReplace), PTDetail, the demodulation, (PTDenoiseSplit) +
	// the specular light x its albedo
	bool ShowPass(bool denoisedOk, bool specDenoisedOk) {
		const bool modulate = (ptFlags & 512u) != 0;
		const bool splitShow = modulate && splitThisFrame;
		const int view = kDebugViews[set.debugView];
		IDirect3DTexture9* shown = denoisedOk ? denoised.t : display.t;
		IDirect3DTexture9* specShown = specDenoisedOk ? specDenoised.t : specTex.t;
		struct { float exposure; uint32_t samplerIndex; float split, gamma; uint32_t modulate; float uvx, uvy; uint32_t keep; float detail, radius, texel[2]; } push =
			{ 1.0f, 0, set.split ? 0.5f : 0.0f, view == 1 || view == 2 || view == 4 ? 1.0f : 1.0f / 2.2f, (modulate ? 1u : 0u) | 2u | (splitShow ? 4u : 0u), 1.0f, 1.0f, 0u,
			  set.detail && modulate && view == 0 ? 1.0f : 0.0f, set.detailRadius, { 1.0f / W, 1.0f / H } };
		static_assert(sizeof(push) == 48, "RtPathTraceShow's push block");
		IUnknown* images[10] = { shown, modulate ? (IUnknown*)albedoOut.t : (IUnknown*)shown, display.t, display.t, frame.t, gDepth.t,
			splitShow ? (IUnknown*)specShown : display.t, splitShow ? (IUnknown*)specTex.t : display.t, splitShow ? (IUnknown*)specAlbedoA.t : display.t,
			splitShow ? (IUnknown*)specAlbedoB.t : display.t };
		return DrawFrag(show, scene_.s, nullptr, images, &push);
	}

	// ---------------------------------------------------------------- HUD
	struct HudVertex { float x, y, z, rhw; D3DCOLOR c; float u, v; };
	std::vector<HudVertex> hud;
	void Text(float x, float y, const std::string& s, D3DCOLOR col = D3DCOLOR_ARGB(255, 235, 235, 235)) {
		const float cw = 16.0f / (16 * 16), chh = 16.0f / (6 * 16);
		for (size_t i = 0; i < s.size(); i++) {
			int ch = (unsigned char)s[i]; if (ch < 32 || ch > 127) ch = '?';
			const int k = ch - 32; const float u0 = (k % 16) * cw, v0 = (k / 16) * chh;
			const float x0 = x + i * fontAdvance - 0.5f, y0 = y - 0.5f, x1 = x0 + 16, y1 = y0 + 16;
			const HudVertex q[6] = { { x0, y0, 0, 1, col, u0, v0 }, { x1, y0, 0, 1, col, u0 + cw, v0 }, { x1, y1, 0, 1, col, u0 + cw, v0 + chh },
				{ x0, y0, 0, 1, col, u0, v0 }, { x1, y1, 0, 1, col, u0 + cw, v0 + chh }, { x0, y1, 0, 1, col, u0, v0 + chh } };
			hud.insert(hud.end(), q, q + 6);
		}
	}
	void Panel(float x, float y, float w, float h, D3DCOLOR col) {
		const int k = 127 - 32; const float cw = 16.0f / 256, chh = 16.0f / 96, u = (k % 16) * cw + cw * 0.5f, v = (k / 16) * chh + chh * 0.5f;
		const HudVertex q[6] = { { x, y, 0, 1, col, u, v }, { x + w, y, 0, 1, col, u, v }, { x + w, y + h, 0, 1, col, u, v },
			{ x, y, 0, 1, col, u, v }, { x + w, y + h, 0, 1, col, u, v }, { x, y + h, 0, 1, col, u, v } };
		hud.insert(hud.end(), q, q + 6);
	}
	std::string StatusLine() {
		char s[512];
		const double gpu = host.shared ? host.shared->gpuTimeUsAvg / 1000.0 : 0, trace = host.shared ? host.shared->traceUsAvg / 1000.0 : 0;
		snprintf(s, sizeof(s), "%ux%u -> %ux%u | %.1f fps (%.1f ms) | RT helper GPU %.2f ms (passes %.2f) | %s", W, H, winW, winH, fps, frameMs, gpu, trace, host.deviceName);
		return s;
	}
	std::string ModeLine() {
		static const char* const modes[] = { "PTRealtime (denoised)", "reference (running mean)", "raw (1 path, no denoiser)" };
		char s[512];
		if (!set.pt) snprintf(s, sizeof(s), "PATH TRACER OFF: raster shading%s", set.rtSun ? " + RT sun shadows (RtSunShadow)" : " (no shadows)");
		else snprintf(s, sizeof(s), "%s | %d bounce%s | view: %s%s", modes[set.ptMode], set.ptMode == 1 ? std::max(set.bounces, 3) : set.bounces,
			set.bounces == 1 ? "" : "s", kDebugNames[set.debugView], set.ptMode == 1 ? (" | " + std::to_string(samples) + " samples").c_str() : "");
		return s;
	}
	std::vector<std::string> ToggleTokens() {
		std::vector<std::string> out;
		char e[160];
		snprintf(e, sizeof(e), "[B]bounces %d  ", set.bounces); out.push_back(e);
		for (Toggle& t : toggles()) {
			if (t.vk == 'P' || t.vk == 'N' || t.vk == 'V' || t.vk == VK_F1) continue;
			out.push_back(std::string("[") + t.key + "]" + t.name + (*t.v ? " on  " : " off  "));
		}
		snprintf(e, sizeof(e), "[+/-]exposure %.2f  [arrows]sun az %.0f el %.0f", set.exposure, set.sunAz * 57.2958f, set.sunEl * 57.2958f); out.push_back(e);
		return out;
	}
	void DrawHud() {
		hud.clear();
		const float lh = 17.0f;
		std::vector<std::pair<std::string, D3DCOLOR>> lines = { { StatusLine(), D3DCOLOR_ARGB(255, 255, 230, 140) },
			{ ModeLine() + (hudTag.empty() ? std::string() : "   | run: " + hudTag), D3DCOLOR_ARGB(255, 160, 220, 255) } };
		const size_t perLine = size_t(std::max(40, int(winW - 16) / fontAdvance));
		std::string cur;
		for (const std::string& tok : ToggleTokens()) {
			if (!cur.empty() && cur.size() + tok.size() > perLine) { lines.push_back({ cur, D3DCOLOR_ARGB(255, 225, 225, 225) }); cur.clear(); }
			cur += tok;
		}
		if (!cur.empty()) lines.push_back({ cur, D3DCOLOR_ARGB(255, 225, 225, 225) });
		if (set.hud == 1) lines.push_back({ "WASD move  Shift fast  Space/E up  C/Q down  hold RMB (or M) to look  1-4 viewpoints  P path tracer  N mode  V view  F12 shot  F1 HUD  Esc quit",
			D3DCOLOR_ARGB(255, 170, 170, 170) });
		if (!lastShot.empty()) lines.push_back({ "saved " + lastShot.substr(lastShot.find_last_of('\\') + 1), D3DCOLOR_ARGB(255, 140, 255, 140) });
		Panel(0, 0, float(winW), lh * lines.size() + 8, D3DCOLOR_ARGB(150, 0, 0, 0));
		hudBottom = int(lh * lines.size() + 8);
		for (size_t i = 0; i < lines.size(); i++) Text(8, 4 + lh * i, lines[i].first, lines[i].second);
		IDirect3DDevice9* d = host.dev;
		d->SetVertexShader(nullptr); d->SetPixelShader(nullptr); d->SetVertexDeclaration(nullptr);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		d->SetTexture(0, fontTex);
		d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_LIGHTING, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		if (!hud.empty()) d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(hud.size() / 3), hud.data(), sizeof(HudVertex));
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		d->SetTexture(0, nullptr);
	}

	// (2026-11-09 pt-mirrors) the specular denoiser's state on smooth pixels (roughness < 0.1) at a screenshot: the history length the
	// temporal pass kept (A.a), the virtual reprojection's share (C.z) and the hit distance, left and right half apart (segment 05: the
	// chrome sphere is on the left, the wall mirror on the right). One log line a half.
	static float Half(uint16_t h) {
		const uint32_t sgn = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
		float v = e == 0 ? m * (1.0f / 16777216.0f) : e == 31 ? 65504.0f : std::ldexp(1.0f + m / 1024.0f, int(e) - 15);
		return sgn ? -v : v;
	}
	bool ReadBack(RtTex& t, D3DFORMAT f, std::vector<float>& out, int comps) {
		IDirect3DSurface9* sys = nullptr;
		if (!t.s || FAILED(host.dev->CreateOffscreenPlainSurface(W, H, f, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return false;
		bool ok = SUCCEEDED(host.dev->GetRenderTargetData(t.s, sys));
		D3DLOCKED_RECT lr;
		if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			out.assign(size_t(W) * H * comps, 0.0f);
			for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) for (int c = 0; c < comps; c++) {
				const char* row = (const char*)lr.pBits + y * lr.Pitch;
				out[(size_t(y) * W + x) * comps + c] = f == D3DFMT_A16B16G16R16F ? Half(((const uint16_t*)row)[x * 4 + c]) : ((const float*)row)[x * comps + c];
			}
			sys->UnlockRect();
		}
		else ok = false;
		sys->Release();
		return ok;
	}
	void ProbeSpecular(const std::string& name) {
		if (!specularDn.valid || !splitThisFrame || set.ptMode != 0) return;
		const uint32_t now = specularDn.frame & 1;
		std::vector<float> a, c, rough, raw;
		if (!ReadBack(specularDn.hist[now], D3DFMT_A16B16G16R16F, a, 4) || !ReadBack(specularDn.histC[now], D3DFMT_A16B16G16R16F, c, 4) ||
			!ReadBack(specAlbedoB, D3DFMT_G32R32F, rough, 2) || !ReadBack(specTex, D3DFMT_A16B16G16R16F, raw, 4)) return;
		for (int half = 0; half < 2; half++) {
			double n = 0, len = 0, vs = 0, hit = 0, marked = 0; int full = 0, under8 = 0;
			for (UINT y = H / 6; y < H; y++) for (UINT x = half ? W / 2 : 0; x < (half ? W : W / 2); x++) {
				const size_t i = size_t(y) * W + x;
				if (!(rough[i * 2 + 1] < 0.1f) || !(raw[i * 4 + 3] != 0.0f)) continue;   // smooth, its ray the specular lobe's (< 0: marked, PTDenoiseMirrorSurface)
				n++; len += a[i * 4 + 3]; vs += c[i * 4 + 2]; hit += std::fabs(raw[i * 4 + 3]); marked += raw[i * 4 + 3] < 0.0f ? 1 : 0;
				if (a[i * 4 + 3] >= set.denoiseHistory - 0.5f) full++;
				if (a[i * 4 + 3] < 8.0f) under8++;
			}
			if (n > 0) pgLog("  probe %s %s half: %.0f smooth pixels, specular history %.1f frames avg (%.0f %% under 8, %.0f %% at the full %g), virtual share %.2f, hit %.0f, %.0f %% marked mirror surface\n",
				name.c_str(), half ? "right" : "left", n, len / n, 100.0 * under8 / n, 100.0 * full / n, set.denoiseHistory, vs / n, hit / n, 100.0 * marked / n);
		}
	}
	// UNOFFICIAL 2026-11-09 --frames <file>: every frame's finished picture (window size, RGB, 3 bytes a pixel, top row first) appended to
	// one raw file, for a film at a fixed rate (ffmpeg -f rawvideo): with --fixed-step 0.0333 the film plays what 30 frames a second look
	// like on a card that runs the path tracer in real time, however slowly this PC renders it.
	FILE* frameDump = nullptr;
	double framesFrom = -1.0e30, framesTo = 1.0e30;   // (2026-11-09 pt-mirrors) --frames-range a,b: only the demo's script seconds [a, b) (one segment's film)
	unsigned framesDumped = 0;
	void DumpFrame() {
		if (!frameDump || FAILED(host.dev->GetRenderTargetData(composite.s, shotSys))) return;
		D3DLOCKED_RECT lr;
		if (FAILED(shotSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
		std::vector<uint8_t> rgb(size_t(winW) * winH * 3);
		for (UINT y = 0; y < winH; y++) for (UINT x = 0; x < winW; x++) {
			const uint8_t* p = (const uint8_t*)lr.pBits + y * lr.Pitch + x * 4;
			uint8_t* q = &rgb[(size_t(y) * winW + x) * 3]; q[0] = p[2]; q[1] = p[1]; q[2] = p[0];
		}
		shotSys->UnlockRect();
		if (std::fwrite(rgb.data(), 1, rgb.size(), frameDump) == rgb.size()) framesDumped++;
	}
	bool SaveShot(const std::string& name) {
		CreateDirectoryA(outDir.c_str(), nullptr);
		if (FAILED(host.dev->GetRenderTargetData(composite.s, shotSys))) return false;
		D3DLOCKED_RECT lr;
		if (FAILED(shotSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
		std::vector<uint8_t> rgb(size_t(winW) * winH * 3);
		for (UINT y = 0; y < winH; y++) for (UINT x = 0; x < winW; x++) {
			const uint8_t* p = (const uint8_t*)lr.pBits + y * lr.Pitch + x * 4;
			uint8_t* q = &rgb[(size_t(y) * winW + x) * 3]; q[0] = p[2]; q[1] = p[1]; q[2] = p[0];
		}
		shotSys->UnlockRect();
		char file[512]; snprintf(file, sizeof(file), "%s\\%s.png", outDir.c_str(), name.c_str());
		const bool ok = WritePng(file, rgb, winW, winH);
		lastShot = file;
		const int ff = CountFireflies(rgb);
		pgLog("screenshot: %s (%s | %s) | fireflies %d\n", file, StatusLine().c_str(), ModeLine().c_str(), ff);
		FILE* csv = nullptr; fopen_s(&csv, (outDir + "\\fireflies.csv").c_str(), "a");
		if (csv) { std::fprintf(csv, "%s,%d,%u,%u\n", name.c_str(), ff, W, H); std::fclose(csv); }
		return ok;
	}
	// Bright outliers in a saved frame (below the HUD): pixels whose luma (as shown, 0-1) is over the median of their 5 x 5 neighbourhood by
	// more than 0.2 and more than 1.6 x it: the isolated sparkles a one-path denoiser leaves. Edges and highlights count too, alike in an A/B.
	int CountFireflies(const std::vector<uint8_t>& rgb) {
		const int w = int(winW), h = int(winH), top = std::max(hudBottom, 0) + 3;
		std::vector<float> l(size_t(w) * h);
		for (size_t i = 0; i < l.size(); i++) l[i] = (0.2126f * rgb[i * 3] + 0.7152f * rgb[i * 3 + 1] + 0.0722f * rgb[i * 3 + 2]) / 255.0f;
		int count = 0;
		float nb[24];
		for (int y = top; y < h - 2; y++) for (int x = 2; x < w - 2; x++) {
			const float c = l[size_t(y) * w + x];
			if (c < 0.2f) continue;
			int k = 0;
			for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) if (dx || dy) nb[k++] = l[size_t(y + dy) * w + x + dx];
			std::nth_element(nb, nb + 12, nb + 24);
			const float med = nb[12];
			if (c > med + 0.2f && c > 1.6f * med) count++;
		}
		return count;
	}

	bool Frame(double dt) {
		frameNumber++;
		cam.update(float(W) / float(H));
		if (demoTime >= 0.0) simTime = (freezeSim >= 0.0 ? freezeSim : demoTime) * 0.5;   // (the demo: the animation follows the script's clock, so A/B runs see the same frames)
		else if (set.lampAnim || set.spin) simTime += dt;
		Animate(simTime);
		std::vector<PgInstance> inst = Instances();
		const double eye[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
		IDirect3DDevice9* d = host.dev;
		d->BeginScene();
		RasterGBuffer();
		MotionPass();   // (2026-11-09) TESR_ObjectMotionBuffer's stand-in (key 5)
		bool denoisedOk = false, specOk = false;
		if (set.pt) {
			Deferred(false);                                  // the game's frame (before the trace: flag 32, PTDetail, the guide)
			RasterGlass();                                    // glass over it, and into the G-buffer (as the game draws glass)
			d->SetRenderTarget(0, park.s);
			if (!TracePass(inst)) return false;
			if (!host.Exchange(inst, eye)) return false;
			host.CopyOut(imDisplay, display.s);
			if (ptFlags & 512u) host.CopyOut(imAlbedoOut, albedoOut.s);
			// PTDenoiseSplit: the specular light and its albedo back (the mean images carry the albedo: r, g | b, roughness)
			splitThisFrame = (ptFlags & 4194304u) && host.CopyOut(imSpec, specTex.s) && host.CopyOut(imA, specAlbedoA.s) && host.CopyOut(imB, specAlbedoB.s);
			d->SetRenderTarget(0, park.s);                    // a fork pass may not draw into the device's current target
			if (set.ptMode == 0) {
				if (!RunDenoiser(diffuseDn, display.t, denoised.s, false)) return false;
				denoisedOk = true;
				if (splitThisFrame) { if (!RunDenoiser(specularDn, specTex.t, specDenoised.s, true)) return false; specOk = true; }
				else specularDn.valid = false;
				histValid = true;
			}
			else histValid = false;
			d->StretchRect(frame.s, nullptr, scene_.s, nullptr, D3DTEXF_POINT);   // PTSplit's left part keeps the game's frame
			d->SetRenderTarget(0, park.s);
			if (!ShowPass(denoisedOk, specOk)) return false;
		}
		else {
			const bool rtSun = set.rtSun && SunUp();
			if (rtSun) SunPass();
			if (!host.Exchange(inst, eye)) return false;      // (the scene is kept up to date either way)
			if (rtSun) host.CopyOut(imSunVis, sunVis.s);
			Deferred(rtSun);
			RasterGlass();
			d->StretchRect(frame.s, nullptr, scene_.s, nullptr, D3DTEXF_POINT);
			histValid = false;
		}
		prevEye = cam.eye; put4(prevBasis[0], cam.r, 0); put4(prevBasis[1], cam.u, 0); put4(prevBasis[2], cam.f, 0); prevP[0] = cam.p00; prevP[1] = cam.p11;
		// the tonemap (window size, linear upscale), the HUD, present
		d->SetTexture(0, scene_.t);
		d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		const float tm[4] = { set.exposure, kDebugViews[set.debugView] == 0 && set.tonemap ? 1.0f : 0.0f, 0, 0 };
		d->SetPixelShaderConstantF(0, tm, 1);
		FullScreen(composite.s, winW, winH, psTonemap);
		d->SetTexture(0, nullptr);
		if (set.hud) DrawHud();
		d->EndScene();
		if (!wantShot.empty()) {
			char name[128]; snprintf(name, sizeof(name), "%s_%03d", wantShot.c_str(), ++shotCount);
			if (wantShot != "shot") snprintf(name, sizeof(name), "%s", wantShot.c_str());
			SaveShot(name);
			ProbeSpecular(name);   // (2026-11-09 pt-mirrors)
			wantShot.clear();
		}
		if (demoTime < 0.0 || (demoTime >= framesFrom && demoTime < framesTo)) DumpFrame();	// (--frames; pt-mirrors: --frames-range, script seconds)
		d->StretchRect(composite.s, nullptr, backbuffer, nullptr, D3DTEXF_POINT);
		if (FAILED(d->Present(nullptr, nullptr, nullptr, nullptr))) { pgLog("  error: Present\n"); return false; }
		return true;
	}
};

// ------------------------------------------------------------------------------------------------------------------------------
// The demo: a scripted minute (--demo 60) that walks the room, flips the toggles and saves labelled screenshots; per segment the fps
// and the helper's GPU time go to the log. For proving a build without a person at the keys.
struct DemoStep { double from, to; const char* label; std::function<void(App&, double)> apply; double shotAt; };
static std::vector<DemoStep> DemoScript() {
	auto view = [](App& a, int v) { a.ApplyView(v); };
	return {
		{ 0, 10, "01_room_ptrealtime", [=](App& a, double u) { view(a, 0); a.cam.yaw = -0.62f + 0.35f * float(u); a.set = Settings(); }, 9.5 },
		{ 10, 20, "02_window_fence_sun", [=](App& a, double) { view(a, 1); a.set = Settings(); }, 19.5 },
		{ 20, 25, "03_raw_one_path", [=](App& a, double) { view(a, 1); a.set = Settings(); a.set.ptMode = 2; }, 24.5 },
		{ 25, 30, "04_pt_off_raster_rtsun", [=](App& a, double) { view(a, 1); a.set = Settings(); a.set.pt = 0; }, 29.5 },
		{ 30, 40, "05_mirror_spheres", [=](App& a, double u) { view(a, 2); a.cam.eye.y += 60.0f * float(u); a.set = Settings(); }, 39.5 },
		{ 40, 50, "06_reference_mean", [=](App& a, double) { view(a, 2); a.set = Settings(); a.set.ptMode = 1; a.set.lampAnim = 0; a.set.spin = 0; }, 49.5 },
		{ 50, 60, "07_night_lamps_glow", [=](App& a, double) { view(a, 0); a.set = Settings(); a.set.sun = 0; a.set.glowLights = 1; a.set.exposure = 1.6f; }, 59.5 },
		// (2026-11-07) the merged materials: the glass pane in the window, the back-lit leaf card, the skin head, in the sun
		{ 60, 70, "08_glass_leaf_skin", [=](App& a, double) { view(a, 1); a.cam.eye = a.cam.eye + V3(150, -40, -40); a.cam.yaw = 0.15f; a.set = Settings(); }, 69.5 },
		// (2026-11-09 pt-mirrors) the many-sample references of 05 and 08 at THEIR screenshot's camera and moment (the lamps and the cube
		// frozen there): the denoised shots are scored against these (tools/nvr-rt-playground/score_mirror.py)
		{ 70, 85, "09_reference_of_05", [=](App& a, double) { a.UsePose(4); a.set = Settings(); a.set.ptMode = 1; }, 84.5 },
		{ 85, 100, "10_reference_of_08", [=](App& a, double) { a.UsePose(7); a.set = Settings(); a.set.ptMode = 1; }, 99.5 },
	};
}

// Ctrl+C / Ctrl+Break / the console window closing: the same way out as the window's X (the main loop sees quit, runs App::Shutdown).
// For a closing console Windows ends the process when this returns: wait for the shutdown first (it has ~5 s).
static App* gApp = nullptr;
static HANDLE gShutdownDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
static BOOL WINAPI ConsoleHandler(DWORD type) {
	if (gApp) gApp->quit = true;
	if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) WaitForSingleObject(gShutdownDone, 4800);
	return TRUE;
}

int main(int argc, char** argv) {
	App app;
	app.outDir = "out";
	std::vector<std::pair<std::string, int>> sets;   // --set <toggle key>=<value> (demo A/B runs: e.g. R=0, B=1, Z=1)
	std::string tag;                                 // --tag: appended to the demo's screenshot names
	int maxFrames = 0;                               // --max-frames: stop after this many frames (soak tests)
	double fixedStep = 0.0;                          // --fixed-step: the script's seconds a frame (0: real time); A/B runs then see the same frames
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--helper") app.helperExe = next();
		else if (a == "--spv") app.spv = next();
		else if (a == "--compute") app.compute = next();
		else if (a == "--device") app.device = next();
		else if (a == "--out") app.outDir = next();
		else if (a == "--demo") app.demoSeconds = std::atof(next().c_str());
		else if (a == "--cache-log2") app.cacheLog2 = std::max(12, std::min(22, std::atoi(next().c_str())));
		else if (a == "--timeout") app.helperTimeout = DWORD(std::atof(next().c_str()) * 1000);
		else if (a == "--set") { const std::string kv = next(); const size_t eq = kv.find('='); if (eq != std::string::npos) sets.push_back({ kv.substr(0, eq), std::atoi(kv.c_str() + eq + 1) }); }
		else if (a == "--tag") tag = next();
		else if (a == "--fixed-step") fixedStep = std::atof(next().c_str());
		else if (a == "--frames-range") { double f0 = 0, f1 = 0; if (sscanf_s(next().c_str(), "%lf,%lf", &f0, &f1) == 2) { app.framesFrom = f0; app.framesTo = f1; } }
		else if (a == "--max-frames") maxFrames = std::atoi(next().c_str());
		else if (a == "--frames") { const std::string file = next(); fopen_s(&app.frameDump, file.c_str(), "wb"); if (!app.frameDump) pgLog("  error: --frames cannot write %s\n", file.c_str()); }
		else if (a == "--size" || a == "--window") {
			UINT w = 0, h = 0; if (sscanf_s(next().c_str(), "%ux%u", &w, &h) != 2 || w < 64 || h < 64) { std::printf("bad %s\n", a.c_str()); return 2; }
			if (a == "--size") { app.W = w; app.H = h; } else { app.winW = w; app.winH = h; }
		}
		else { std::printf("usage: NvrRtPlayground.exe --helper <NvrRtHelper.exe> --spv <trace .spv folder> --compute <fork passes .spv folder>\n"
			"       [--size 640x360] [--window 1280x720] [--device <name part>] [--out <folder>] [--demo <seconds>] [--cache-log2 12-22] [--timeout <s>]\n"); return 2; }
	}
	if (app.helperExe.empty() || app.spv.empty() || app.compute.empty()) { std::printf("--helper, --spv and --compute are required (run.ps1 passes them)\n"); return 2; }
	for (std::string* p : { &app.spv, &app.compute, &app.outDir, &app.helperExe }) { char full[MAX_PATH]; GetFullPathNameA(p->c_str(), MAX_PATH, full, nullptr); *p = full; }
	CreateDirectoryA(app.outDir.c_str(), nullptr);
	fopen_s(&gLog, (app.outDir + "\\playground.log").c_str(), "w");
	pgLog("Vegas Shaders RT playground: internal %ux%u, window %ux%u, helper %s\n", app.W, app.H, app.winW, app.winH, app.helperExe.c_str());
	gApp = &app;
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	if (!app.Init()) {
		pgLog("RESULT: FAIL (start)\n");
		MessageBoxA(app.hwnd, "The RT playground could not start: see playground.log (and NvrRtHelper.log / the DXVK log).", "RT playground", MB_ICONERROR);
		app.Shutdown();
		if (gShutdownDone) SetEvent(gShutdownDone);
		return 1;
	}
	LARGE_INTEGER freq, t0, last; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); last = t0;
	const std::vector<DemoStep> script = DemoScript();
	size_t step = 0; bool shotTaken = false;
	struct Seg { int frames = 0; double seconds = 0, gpuSum = 0, traceSum = 0; };
	std::vector<Seg> segs(script.size());
	bool ok = true;
	double titleAt = 0;
	// the demo's timeline is 100 s (2026-11-09: + the matched references 09, 10); --demo N runs it in N seconds (each segment scaled)
	const double scriptLength = script.back().to;
	const double timeScale = app.demoSeconds > 0 ? app.demoSeconds / scriptLength : 1.0;
	uint64_t demoFrames = 0;
	auto applySets = [&]() {
		for (auto& kv : sets) {
			if (kv.first == "B") { app.set.bounces = std::max(1, std::min(4, kv.second)); continue; }
			for (App::Toggle& tg : app.toggles()) if (kv.first == tg.key) *tg.v = std::max(0, std::min(tg.count - 1, kv.second));
		}
	};
	applySets();
	app.hudTag = tag;
	while (!app.quit) {
		MSG m; while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
		LARGE_INTEGER now; QueryPerformanceCounter(&now);
		const double frameDt = double(now.QuadPart - last.QuadPart) / freq.QuadPart, t = double(now.QuadPart - t0.QuadPart) / freq.QuadPart;
		const double dt = std::min(frameDt, 0.1);   // (the animation's step; the stats take the real one)
		last = now;
		if (app.demoSeconds > 0) {
			const double st = fixedStep > 0 ? double(demoFrames++) * fixedStep : t / timeScale;
			app.demoTime = st;
			while (step < script.size() && st >= script[step].to) { step++; shotTaken = false; }
			if (step >= script.size()) break;
			if (fixedStep > 0 && t > app.demoSeconds) { pgLog("stopped at the --demo limit of %.0f s (wall clock)\n", app.demoSeconds); break; }
			const DemoStep& s = script[step];
			const bool held = app.set.hud;   // (the HUD stays as the person set it: on)
			app.freezeSim = -1.0;   // (pt-mirrors: only a reference segment's apply freezes it)
			s.apply(app, (st - s.from) / (s.to - s.from));
			app.set.hud = held;
			applySets();
			if (!shotTaken && st >= s.shotAt) {
				app.wantShot = std::string(s.label) + tag; shotTaken = true;
				if (app.shotPoses.size() < script.size()) app.shotPoses.resize(script.size());
				App::ShotPose& pose = app.shotPoses[step];   // (pt-mirrors) for the matched reference segments
				pose.valid = true; pose.eye = app.cam.eye; pose.yaw = app.cam.yaw; pose.pitch = app.cam.pitch; pose.time = app.freezeSim >= 0.0 ? app.freezeSim : st;
			}
			if (st > s.from + 2.0) { segs[step].frames++; segs[step].seconds += frameDt; segs[step].gpuSum += app.host.shared->gpuTimeUsAvg / 1000.0; segs[step].traceSum += app.host.shared->traceUsAvg / 1000.0; }
		}
		else app.Input(float(dt));
		if (!app.Frame(dt)) { ok = false; break; }
		if (maxFrames > 0 && int(app.frameNumber) >= maxFrames) { pgLog("stopped after %d frames (--max-frames)\n", maxFrames); break; }
		app.statFrames++;
		if (t - app.statStart >= 0.5) { app.fps = app.statFrames / (t - app.statStart); app.frameMs = 1000.0 / std::max(app.fps, 1e-3); app.statFrames = 0; app.statStart = t; }
		if (t - titleAt > 0.5) { titleAt = t; SetWindowTextA(app.hwnd, ("Vegas Shaders RT playground | " + app.StatusLine()).c_str()); }
	}
	pgLog("helper: %u geometries alive, %u failed ops, shader failures 0x%x, GPU %.3f ms avg (builds %.3f, scene %.3f, passes %.3f), last: %s\n",
		app.host.shared->geometries, app.host.shared->failedOps, app.host.shared->shaderFailures, app.host.shared->gpuTimeUsAvg / 1000.0,
		app.host.shared->buildUsAvg / 1000.0, app.host.shared->sceneUsAvg / 1000.0, app.host.shared->traceUsAvg / 1000.0, app.host.shared->message);
	if (app.demoSeconds > 0) {
		pgLog("\nDEMO (%ux%u internal, window %ux%u, %s): per segment, after its first 2 s\n", app.W, app.H, app.winW, app.winH, app.host.deviceName);
		for (size_t i = 0; i < script.size(); i++) {
			const Seg& s = segs[i];
			if (!s.frames) continue;
			pgLog("  %-26s %6.2f fps  %7.2f ms a frame  helper GPU %7.2f ms (passes %7.2f)\n", script[i].label, s.frames / s.seconds, 1000.0 * s.seconds / s.frames,
				s.gpuSum / s.frames, s.traceSum / s.frames);
		}
	}
	if (app.host.shared->failedOps || app.host.shared->shaderFailures) ok = false;
	const bool clean = app.Shutdown();
	pgLog("RESULT: %s\n", ok ? (clean ? "PASS (ran without errors, clean shutdown)" : "PASS (ran without errors; the shutdown had to force something)") : "FAIL");
	if (gLog) fclose(gLog);
	if (gShutdownDone) SetEvent(gShutdownDone);
	return ok ? 0 : 1;
}
