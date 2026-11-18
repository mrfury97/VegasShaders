// UNOFFICIAL 2026-11-15 full deferred renderer, phase 6 (docs/next/FD-GI-RESULTS.md): the three passes of core/FdIndirect.h, checked on a real
// D3D9 device against a CPU reference. The pass text is core/FdIndirectShaders.h itself (included, compiled with the game's D3DX at ps_3_0 as the
// plugin compiles it). Inputs are G-buffer v2 texels made by the encoder the templates use (Includes/Shadow.hlsl ShadowEncodeNormal and the
// COLOR2 packing of Includes/CapturedNormals.hlsl), so the decode is checked against the true normal, not against itself.
//   1. shading normals: the output's normal = the true shading normal rotated into view space (row vector x the view matrix's rows, as
//      CapturedNormals::MergeIntoNormals), on exactly the pixels the rules take; the normals buffer's texel elsewhere.
//   2. the reflections' material: per model 1-10 x specular bit x metal x roughness codes, the copy's r g b (or clipped).
//   3. AmbientLight's albedo: COLOR1 x (1 - metal) x gain, alpha kept.
// Usage: fd_gi_passes.exe. Exit 0 = every pixel as the reference (normals within 2e-3, the fp32 decode's error; the rest within 1e-5).
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "../src/core/FdIndirectShaders.h"

static IDirect3DDevice9* gDevice = nullptr;
static const UINT W = 256, H = 256;

static bool MakeDevice() {
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) return false;
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "fdgipasses"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("fdgipasses", "fdgipasses", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = window;
	return SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &gDevice));
}

static IDirect3DPixelShader9* Compile(const char* src, const char* what) {
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL);
	if (errors) { printf("%s: %s\n", what, (const char*)errors->GetBufferPointer()); errors->Release(); }
	if (FAILED(hr) || !code) return nullptr;
	IDirect3DPixelShader9* ps = nullptr;
	gDevice->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
	printf("%-22s compiled: %u bytes\n", what, (unsigned)code->GetBufferSize());
	code->Release();
	return ps;
}

// An fp32 RGBA texture holding data (W x H x 4).
static IDirect3DTexture9* Upload(const std::vector<float>& data) {
	IDirect3DTexture9* sys = nullptr; IDirect3DTexture9* tex = nullptr;
	if (FAILED(gDevice->CreateTexture(W, H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, NULL))) return nullptr;
	D3DLOCKED_RECT lr;
	sys->LockRect(0, &lr, NULL, 0);
	for (UINT y = 0; y < H; y++) memcpy((char*)lr.pBits + y * lr.Pitch, &data[y * W * 4], W * 16);
	sys->UnlockRect(0);
	gDevice->CreateTexture(W, H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &tex, NULL);
	gDevice->UpdateTexture(sys, tex);
	sys->Release();
	return tex;
}

static bool Draw(IDirect3DPixelShader9* ps, IDirect3DTexture9* const* inputs, int count, const float* constants, int vectors, std::vector<float>& out) {
	IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr;
	if (FAILED(gDevice->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &tex, NULL))) return false;
	tex->GetSurfaceLevel(0, &rt);
	gDevice->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, NULL);
	gDevice->SetRenderTarget(0, rt);
	gDevice->SetDepthStencilSurface(nullptr);
	gDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
	gDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	gDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	// cleared to 0: a clipped pixel keeps all four channels 0 (every output the passes keep has a nonzero channel where the test checks clips)
	gDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
	for (int i = 0; i < count; i++) {
		gDevice->SetTexture(i, inputs[i]);
		gDevice->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_POINT); gDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		gDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		gDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); gDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	if (vectors) gDevice->SetPixelShaderConstantF(0, constants, vectors);
	gDevice->SetPixelShader(ps);
	gDevice->SetVertexShader(nullptr);
	gDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	// the plugin's quad (CapturedNormals::FullScreenPass): uv 0-1 over the whole target, pixel centres at uv (x + 0.5) / W
	const float x1 = W - 0.5f, y1 = H - 0.5f;
	const float q[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
	gDevice->BeginScene();
	gDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
	gDevice->EndScene();
	bool ok = SUCCEEDED(gDevice->GetRenderTargetData(rt, sys));
	D3DLOCKED_RECT lr;
	if (ok && SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
		out.resize(W * H * 4);
		for (UINT y = 0; y < H; y++) memcpy(&out[y * W * 4], (const char*)lr.pBits + y * lr.Pitch, W * 16);
		sys->UnlockRect();
	}
	else ok = false;
	for (int i = 0; i < count; i++) gDevice->SetTexture(i, nullptr);
	sys->Release(); rt->Release(); tex->Release();
	return ok;
}

// --- small maths -------------------------------------------------------------------------------------------------------------------
struct V3 { double x, y, z; };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
static double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Norm(V3 a) { double l = sqrt(Dot(a, a)); return a * (1.0 / l); }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static unsigned gSeed = 12345u;
static double Rand() { gSeed = gSeed * 1664525u + 1013904223u; return (gSeed >> 8) / 16777216.0; }
static V3 RandUnit() { for (;;) { V3 v = { Rand() * 2 - 1, Rand() * 2 - 1, Rand() * 2 - 1 }; double l = Dot(v, v); if (l > 0.01 && l < 1) return Norm(v); } }
// Includes/Shadow.hlsl ShadowEncodeNormal
static void Encode(V3 n, float& ex, float& ey) {
	double s = fabs(n.x) + fabs(n.y) + fabs(n.z); n = n * (1.0 / s);
	double x = n.x, y = n.y;
	if (n.z < 0) { x = (1 - fabs(n.y)) * (n.x >= 0 ? 1 : -1); y = (1 - fabs(n.x)) * (n.y >= 0 ? 1 : -1); }
	ex = (float)x; ey = (float)y;
}

int main() {
	if (!MakeDevice()) { printf("no D3D9 device\n"); return 2; }
	IDirect3DPixelShader9* psN = Compile(FdIndirectShaders::kShadingNormalsPS, "shading normals");
	IDirect3DPixelShader9* psR = Compile(FdIndirectShaders::kReflMaterialPS, "reflections material");
	IDirect3DPixelShader9* psA = Compile(FdIndirectShaders::kALAlbedoPS, "AL albedo");
	if (!psN || !psR || !psA) { printf("compile failed\n"); return 2; }
	bool allOk = true;

	// ---- 1. shading normals ----
	{
		// a camera: rows = the world axes' view-space images (D3D view matrix, row vectors: v_view = v_world x V)
		V3 f = Norm({ 0.3, 0.8, -0.2 }), up = { 0, 0, 1 };
		V3 r = Norm(Cross(up, f)), u = Cross(f, r);
		// view space x = right, y = up, z = forward: column j of V's 3x3 is camera axis j in world space
		double Vm[3][3] = { { r.x, u.x, f.x }, { r.y, u.y, f.y }, { r.z, u.z, f.z } };
		auto toWorld = [&](V3 v) { return V3{ v.x * Vm[0][0] + v.y * Vm[0][1] + v.z * Vm[0][2], v.x * Vm[1][0] + v.y * Vm[1][1] + v.z * Vm[1][2], v.x * Vm[2][0] + v.y * Vm[2][1] + v.z * Vm[2][2] }; };
		const double p11 = 1.0 / 1.3, p22 = 1.0 / 0.73, farZ = 100000.0, cosMin = 0.5, share = 0.004, units = 2.0;
		const double sx = 0.9, sy = 0.8;	// a RenderScale rect (TESR_RenderScale zw = 1 / xy)
		std::vector<float> nb(W * H * 4), c3(W * H * 4), m2(W * H * 4), dz(W * H * 4), want(W * H * 4);
		std::vector<int> expectTake(W * H);
		int cases[6] = {};
		for (UINT y = 0; y < H; y++) for (UINT x = 0; x < W; x++) {
			const size_t i = (y * W + x) * 4;
			const double s0 = (x + 0.5) / W / sx, t0 = (y + 0.5) / H / sy;
			V3 ray = { (s0 * 2 - 1) * p11, (1 - t0 * 2) * p22, 1.0 };
			const double z = 50.0 + Rand() * 5000.0;
			V3 vp = ray * z; const double dist = sqrt(Dot(vp, vp));
			V3 rayN = Norm(vp);
			// a smooth normal facing the camera (view space), and a shading normal bent from it
			V3 sv; do { sv = RandUnit(); } while (Dot(sv, rayN) > -0.2);
			V3 mv; int kind;
			const double pick = Rand();
			if (pick < 0.15) { do { mv = RandUnit(); } while (Dot(mv, rayN) < 0.1); kind = 1; }	// bent away: the smooth one stays
			else { do { mv = Norm(sv + RandUnit() * 0.4); } while (fabs(Dot(mv, rayN) + 0.02) < 0.01); kind = Dot(mv, rayN) < -0.02 ? 0 : 1; }
			const V3 sw = toWorld(sv), mw = toWorld(mv);
			// the normals buffer: the captured smooth normal (merged), or a rebuilt one far from it
			V3 bufferN = sv; bool merged = true;
			const double pick2 = Rand();
			if (pick2 < 0.1) { do { bufferN = RandUnit(); } while (Dot(bufferN, sv) > 0.3); merged = false; kind = 2; }
			float ex, ey;
			Encode(sw, ex, ey);
			c3[i] = ex; c3[i + 1] = ey; c3[i + 2] = (float)dist; c3[i + 3] = 1.0f;
			const double pick3 = Rand();
			if (pick3 < 0.08) { c3[i + 2] = (float)(dist * 1.2 + 10.0); kind = 3; }	// something else in front (water, glass): no distance match
			else if (pick3 < 0.14) { c3[i + 3] = 0.0f; kind = 4; }					// COLOR3 unwritten
			Encode(mw, ex, ey);
			const double pick4 = Rand();
			float r = 1.0f + (float)(int)(Rand() * 3) + (Rand() < 0.5 ? 16.0f : 0.0f) + 0.49f * (float)Rand();
			float g = ex, a = ey;
			if (pick4 < 0.07) { g = 4.0f; a = 1.0f; kind = 5; }			// no shading normal
			else if (pick4 < 0.12) { r = 0.0f; kind = 5; }				// material unwritten
			m2[i] = r; m2[i + 1] = g; m2[i + 2] = 0.4f; m2[i + 3] = a;
			nb[i] = (float)(bufferN.x * 0.5 + 0.5); nb[i + 1] = (float)(bufferN.y * 0.5 + 0.5); nb[i + 2] = (float)(bufferN.z * 0.5 + 0.5); nb[i + 3] = 0.75f;
			dz[i] = (float)(z / farZ);
			const bool take = kind == 0 && merged;
			expectTake[y * W + x] = take ? 1 : 0;
			cases[kind]++;
			if (take) { want[i] = (float)(mv.x * 0.5 + 0.5); want[i + 1] = (float)(mv.y * 0.5 + 0.5); want[i + 2] = (float)(mv.z * 0.5 + 0.5); want[i + 3] = 0.75f; }
			else { want[i] = nb[i]; want[i + 1] = nb[i + 1]; want[i + 2] = nb[i + 2]; want[i + 3] = nb[i + 3]; }
		}
		IDirect3DTexture9* in[4] = { Upload(nb), Upload(c3), Upload(m2), Upload(dz) };
		const float c[7][4] = {
			{ (float)p11, (float)p22, (float)cosMin, (float)farZ }, { (float)sx, (float)sy, (float)(1 / sx), (float)(1 / sy) },
			{ (float)Vm[0][0], (float)Vm[0][1], (float)Vm[0][2], 0 }, { (float)Vm[1][0], (float)Vm[1][1], (float)Vm[1][2], 0 },
			{ (float)Vm[2][0], (float)Vm[2][1], (float)Vm[2][2], 0 }, { 0, 0, 0, 1 }, { (float)share, (float)units, 0, 0 } };
		std::vector<float> out;
		if (!Draw(psN, in, 4, &c[0][0], 7, out)) { printf("draw failed\n"); return 2; }
		size_t wrongTake = 0, taken = 0; double maxErr = 0.0;
		for (size_t p = 0; p < (size_t)W * H; p++) {
			const float* o = &out[p * 4]; const float* w = &want[p * 4];
			const bool gotTake = fabs(o[0] - nb[p * 4]) > 1e-6 || fabs(o[1] - nb[p * 4 + 1]) > 1e-6 || fabs(o[2] - nb[p * 4 + 2]) > 1e-6;
			if (gotTake != (expectTake[p] != 0)) wrongTake++;
			if (gotTake) taken++;
			for (int k = 0; k < 4; k++) maxErr = (std::max)(maxErr, (double)fabs(o[k] - w[k]));
		}
		const bool ok = wrongTake == 0 && maxErr < 2e-3;
		printf("shading normals: %zu pixels (taken %zu; cases: bumped %d, bent away %d, buffer not the capture %d, no distance match %d, COLOR3 unwritten %d, no shading normal / unwritten %d)\n"
			"   decisions wrong %zu, max |output - reference| %.2e -> %s\n", (size_t)W * H, taken, cases[0], cases[1], cases[2], cases[3], cases[4], cases[5], wrongTake, maxErr, ok ? "PASS" : "FAIL");
		allOk = allOk && ok;
		for (auto* t : in) t->Release();
	}

	// ---- 2. the reflections' material ----
	{
		std::vector<float> m2(W * H * 4), want(W * H * 4);
		std::vector<int> clipped(W * H);
		const float bs[6] = { -1.0f, -2.5f, -3.0f, 0.0f, 0.35f, 1.0f };
		for (UINT p = 0; p < W * H; p++) {
			const int model = 1 + (int)(Rand() * 10), spec = Rand() < 0.5 ? 1 : 0;
			const double metal = Rand() < 0.3 ? 0.0 : Rand();
			const float b = Rand() < 0.5 ? bs[(int)(Rand() * 6)] : (float)Rand();
			float r = (float)(model + 16 * spec) + 0.49f * (float)metal;
			if (Rand() < 0.05) r = 0.25f;	// unwritten
			m2[p * 4] = r; m2[p * 4 + 1] = 0.3f; m2[p * 4 + 2] = b; m2[p * 4 + 3] = 0.1f;
			float* w = &want[p * 4];
			const double frac = r - floor(r);
			const double mDec = (std::min)(1.0, frac / 0.49);
			if (r < 0.5f) { clipped[p] = 1; continue; }
			if (model >= 5 && model <= 9) { w[0] = 1; w[1] = 0; w[2] = 0.5f; w[3] = 0; continue; }
			const float bb = b < -1.99f && b > -3.01f ? -2.0f - b : b;
			if (bb < 0) { clipped[p] = 1; continue; }
			w[0] = (std::min)(bb, 1.0f); w[1] = (float)mDec; w[2] = spec && model <= 3 ? 1.0f : 0.5f; w[3] = 0;
		}
		IDirect3DTexture9* in[1] = { Upload(m2) };
		std::vector<float> out;
		if (!Draw(psR, in, 1, nullptr, 0, out)) { printf("draw failed\n"); return 2; }
		size_t bad = 0, nClip = 0, aware = 0; double maxErr = 0.0;
		for (UINT p = 0; p < W * H; p++) {
			const float* o = &out[p * 4];
			const bool gotClip = o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0;	// the clear (every kept output has b 0.5 or 1)
			if (gotClip != (clipped[p] != 0)) { bad++; continue; }
			if (gotClip) { nClip++; continue; }
			if (want[p * 4 + 2] > 0.75f) aware++;
			for (int k = 0; k < 4; k++) maxErr = (std::max)(maxErr, (double)fabs(o[k] - want[p * 4 + k]));
		}
		// metal from frac(r) at r 17-26.49 carries fp32's error only here (fp16 storage is FD-GBUFFER's table); 1e-5 is the fp32 bound
		const bool ok = bad == 0 && maxErr < 1e-5;
		printf("reflections material: %u pixels, clipped %zu, reflect-any-way %zu; clip decisions wrong %zu, max |output - reference| %.2e -> %s\n",
			W * H, nClip, aware, bad, maxErr, ok ? "PASS" : "FAIL");
		allOk = allOk && ok;
		in[0]->Release();
	}

	// ---- 3. AmbientLight's albedo ----
	{
		std::vector<float> a1(W * H * 4), m2(W * H * 4), want(W * H * 4);
		const float gain = 1.7f;
		for (UINT p = 0; p < W * H; p++) {
			for (int k = 0; k < 3; k++) a1[p * 4 + k] = (float)(Rand() * 1.2 - 0.1);
			a1[p * 4 + 3] = Rand() < 0.2 ? 0.0f : (Rand() < 0.5 ? 1.0f : 1.0f + (float)Rand());
			const int model = 1 + (int)(Rand() * 10), spec = Rand() < 0.5 ? 1 : 0;
			const double metal = Rand() < 0.4 ? 0.0 : Rand();
			float r = (float)(model + 16 * spec) + 0.49f * (float)metal;
			if (Rand() < 0.1) r = 0.0f;
			m2[p * 4] = r; m2[p * 4 + 1] = 4.0f; m2[p * 4 + 2] = 0.5f; m2[p * 4 + 3] = 1.0f;
			const double frac = r - floor(r);
			const double mDec = r > 0.5f ? (std::min)(1.0, frac / 0.49) : 0.0;
			for (int k = 0; k < 3; k++) want[p * 4 + k] = (float)((std::max)(a1[p * 4 + k], 0.0f) * (1.0 - mDec) * gain);
			want[p * 4 + 3] = a1[p * 4 + 3];
		}
		IDirect3DTexture9* in[2] = { Upload(a1), Upload(m2) };
		const float c[4] = { gain, 0, 0, 0 };
		std::vector<float> out;
		if (!Draw(psA, in, 2, c, 1, out)) { printf("draw failed\n"); return 2; }
		double maxErr = 0.0;
		for (size_t k = 0; k < out.size(); k++) maxErr = (std::max)(maxErr, (double)fabs(out[k] - want[k]));
		const bool ok = maxErr < 1e-5;
		printf("AL albedo: %u pixels, max |output - reference| %.2e -> %s\n", W * H, maxErr, ok ? "PASS" : "FAIL");
		allOk = allOk && ok;
		for (auto* t : in) t->Release();
	}
	printf(allOk ? "FD GI PASSES: PASS\n" : "FD GI PASSES: FAIL\n");
	return allOk ? 0 : 1;
}
