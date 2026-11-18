// UNOFFICIAL 2026-11-15 DeferredTransparents' maths check (docs/next/FD-GLASS-RESULTS.md). A synthetic interior (250 lamps round a camera far
// from the world origin) goes into a lamp table built by the game's own code (core/LampTableCore.h FillRow + BinFast, 16-texel froxel lists as
// with DeferredLamps; core/ForwardPlusHeader.h Put for the header texels), uploaded as the A32B32G32R32F texture the game makes (2048 x 63).
// tests/fd_glass_math.hlsl (compiled with the game's D3DX against the real includes) then lights 960 x 540 pseudo-random surface points:
//  1. with each lamp's term = colour x falloff: Includes/ForwardPlus.hlsl FPLampSum (the pixel's froxel list, the header's camera and grid)
//     against a CPU sum over EVERY lamp of the scene in double precision, at the points the GPU used (read back);
//  2. with the draw's PBR term (getPointLightLightingAtt): FPLampSum against the same shader summing every lamp of the table in index order.
// Usage: fd_glass_math.exe <repo root>. Exit 0 = every pixel within tolerance (1e-3 relative, at least 1e-4 absolute, for the CPU sum:
// the GPU subtracts float world positions; 1e-4 for the GPU-only pairs: only the order of the sum differs).
// 3. FPLampSum compiled with the real term (FG_REAL, no test wrapper) against a plain every-lamp loop calling it directly.
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include "../src/core/LampTableCore.h"
#include "../src/core/ForwardPlusHeader.h"

static IDirect3DDevice9* gDevice = nullptr;
static const UINT W = 960, H = 540;

static bool MakeDevice() {
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) return false;
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "fdglassmath"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("fdglassmath", "fdglassmath", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = window;
	return SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &gDevice));
}

static IDirect3DPixelShader9* Compile(const std::string& file, bool real = false) {
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	const D3DXMACRO m[2] = { { "FG_REAL", "1" }, { NULL, NULL } };
	HRESULT hr = D3DXCompileShaderFromFileA(file.c_str(), real ? m : NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL);
	if (errors) { printf("%s\n", (const char*)errors->GetBufferPointer()); errors->Release(); }
	if (FAILED(hr) || !code) return nullptr;
	IDirect3DPixelShader9* ps = nullptr;
	gDevice->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
	code->Release();
	return ps;
}

static bool Draw(IDirect3DPixelShader9* ps, float mode, float simple, float lamps, std::vector<float>& out, float probe = 0.0f) {
	IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr;
	if (FAILED(gDevice->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &tex, NULL))) return false;
	tex->GetSurfaceLevel(0, &rt);
	gDevice->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, NULL);
	gDevice->SetRenderTarget(0, rt);
	gDevice->SetDepthStencilSurface(nullptr);
	gDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
	gDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	gDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	gDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
	const float pbrData[4] = { 0.0f, 1.0f, 1.25f, 1.0f };		// c32 TESR_PBRData: z light scale (as tests/fd_sun_math.cpp)
	const float pbrExtra[4] = { 0.85f, 1.0f, 0.5f, 4000.0f };	// c33 TESR_PBRExtraData: x saturation
	const float pbrMath[4] = { 1.0f, 0.0f, 0.0f, 0.0f };		// c223 TESR_PBRMath: EnergySplit on
	const float linear[4] = { 2.2f, 1.0f / 2.2f, 0.0f, 0.0f };	// c191 TESR_LinearLighting
	const float m[4] = { mode, simple, probe, 0.0f }, v[4] = { (float)W, (float)H, lamps, 0.0f };
	gDevice->SetPixelShaderConstantF(32, pbrData, 1);
	gDevice->SetPixelShaderConstantF(33, pbrExtra, 1);
	gDevice->SetPixelShaderConstantF(223, pbrMath, 1);
	gDevice->SetPixelShaderConstantF(191, linear, 1);
	gDevice->SetPixelShaderConstantF(0, m, 1);
	gDevice->SetPixelShaderConstantF(1, v, 1);
	gDevice->SetPixelShader(ps);
	gDevice->SetVertexShader(nullptr);
	gDevice->SetFVF(D3DFVF_XYZRHW);
	const float q[4][4] = { { -0.5f, -0.5f, 0, 1 }, { W - 0.5f, -0.5f, 0, 1 }, { -0.5f, H - 0.5f, 0, 1 }, { W - 0.5f, H - 0.5f, 0, 1 } };
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
	sys->Release(); rt->Release(); tex->Release();
	return ok;
}

static bool Compare(const char* label, const std::vector<double>& ref, const std::vector<float>& got, double tolerance, double floor = 0.0) {
	double maxAbs = 0.0, maxRel = 0.0, mean = 0.0; size_t bad = 0, lit = 0, worst = 0;
	for (size_t i = 0; i < ref.size(); i++) {
		if (i % 4 == 3) continue;
		const double d = fabs(ref[i] - got[i]), m = (std::max)(fabs(ref[i]), (std::max)(tolerance, floor));
		if (d > maxAbs) worst = i;
		maxAbs = (std::max)(maxAbs, d); maxRel = (std::max)(maxRel, d / m); mean += ref[i];
		if (ref[i] > 0.0) lit++;
		if (d / m > tolerance) bad++;
	}
	printf("%-58s max |diff| %.3g, max relative %.3g, channels over %.0e: %zu of %zu (lit %zu, mean %.4f)\n", label, maxAbs, maxRel, tolerance, bad,
		ref.size() / 4 * 3, lit, mean / (ref.size() / 4 * 3));
	printf("    worst at pixel %zu (x %zu, y %zu): reference %.6g, got %.6g\n", worst / 4, (worst / 4) % W, (worst / 4) / W, ref[worst], got[worst]);
	return bad == 0;
}

static unsigned Rng = 12345u;
static float Rand() { Rng = Rng * 1664525u + 1013904223u; return (float)(Rng >> 8) / 16777216.0f; }

int main(int argc, char** argv) {
	const std::string root = argc > 1 ? argv[1] : ".";
	const std::string shaders = root + "\\src\\hlsl\\NewVegas\\Shaders\\";
	const std::string placed = shaders + "fd_glass_math_test.hlsl";
	if (!CopyFileA((root + "\\tests\\fd_glass_math.hlsl").c_str(), placed.c_str(), FALSE)) { printf("could not place %s\n", placed.c_str()); return 2; }
	if (!MakeDevice()) { printf("no D3D9 device\n"); DeleteFileA(placed.c_str()); return 2; }
	IDirect3DPixelShader9* ps = Compile(placed);
	IDirect3DPixelShader9* psReal = Compile(placed, true);
	DeleteFileA(placed.c_str());
	if (!ps || !psReal) { printf("compile failed\n"); return 2; }

	// The view: a camera far from the origin (float positions as the game's), yaw / pitch, 90 degrees across, 960 x 540.
	LampTableCore::View view;
	view.width = W; view.height = H;
	view.tilePx = LampTableCore::TileSizeFor(W, H);	// (2026-11-15, as ManyLamps::BinFroxels)
	const float yaw = 0.7f, tilt = -0.15f;
	view.fwd[0] = cosf(yaw) * cosf(tilt); view.fwd[1] = sinf(yaw) * cosf(tilt); view.fwd[2] = sinf(tilt);
	view.right[0] = sinf(yaw); view.right[1] = -cosf(yaw); view.right[2] = 0.0f;
	view.up[0] = view.right[1] * view.fwd[2] - view.right[2] * view.fwd[1];	// right x forward
	view.up[1] = view.right[2] * view.fwd[0] - view.right[0] * view.fwd[2];
	view.up[2] = view.right[0] * view.fwd[1] - view.right[1] * view.fwd[0];
	view.cam[0] = -21234.5f; view.cam[1] = 35678.25f; view.cam[2] = 1024.0f;
	view.tanX = 1.0f; view.tanY = (float)H / (float)W;
	view.listTexels = 16;
	view.stableOrder = true;
	// The lamps: 250 in a room-sized volume in front of and round the camera (some behind it, some touching it).
	const int n = 250;	// (under 255: a ps_3_0 loop runs at most 255 times, so the reference loop over every lamp must stay below that)
	std::vector<LampTableCore::SceneLamp> lamps(LampTableCore::TableLamps);
	std::vector<int> active;
	for (int k = 0; k < n; k++) {
		LampTableCore::SceneLamp& l = lamps[k];
		l.id = &lamps[k];
		const float a = Rand() * 6000.0f - 1000.0f, b = Rand() * 6000.0f - 3000.0f, c = Rand() * 800.0f - 300.0f;
		for (int i = 0; i < 3; i++) l.pos[i] = view.cam[i] + view.fwd[i] * a + view.right[i] * b + view.up[i] * c;
		l.reach = 40.0f + Rand() * Rand() * 900.0f;
		l.diff[0] = Rand() * 1.5f; l.diff[1] = Rand() * 1.5f; l.diff[2] = Rand() * 1.5f;
		l.dimmer = 0.5f + Rand(); l.lod = 1.0f;
		l.active = true; l.hidden = false;
		active.push_back(k);
	}
	// The table as ManyLamps::BuildTable makes it with DeferredLamps' 16-texel lists and the tile rows.
	const int listRows = (int)((LampTableCore::MaxFroxels * 16 + LampTableCore::TableTexels - 1) / LampTableCore::TableTexels);	// 60
	const int tileRow = 1 + listRows, rows = tileRow + 2;
	const int pitch = LampTableCore::TableTexels * 4;
	std::vector<float> table((size_t)pitch * rows, 0.0f);
	auto decode = [](float* out, const float* in, float) { memcpy(out, in, 4 * sizeof(float)); };
	LampTableCore::FillRow(table.data(), lamps.data(), n, true, false, 2.2f, decode, nullptr);
	LampTableCore::FastState* fast = new LampTableCore::FastState();
	LampTableCore::BinStats stats;
	LampTableCore::BinFast(view, lamps.data(), active.data(), (int)active.size(), table.data() + pitch, pitch, 1, *fast, stats);
	const int tilesX = LampTableCore::TilesX(view), tilesY = LampTableCore::TilesY(view);
	ForwardPlusHeader::Enabled = true;
	ForwardPlusHeader::Put(table.data() + (size_t)pitch * (tileRow + 1), view.cam, view.right, view.up, view.fwd, view.tanX, view.tanY,
		(float)W / (float)view.tilePx, (float)H / (float)view.tilePx, tilesX, tilesY, true);
	printf("table: %d lamps, %d x %d tiles x 8 slices, %llu lamps binned, %llu list entries, %llu dropped over a list's 63 (must be 0), %d rows\n",
		n, tilesX, tilesY, (unsigned long long)stats.lamps, (unsigned long long)stats.entries, (unsigned long long)stats.over, rows);
	IDirect3DTexture9* tex = nullptr;
	if (FAILED(gDevice->CreateTexture(LampTableCore::TableTexels, rows, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &tex, NULL))) { printf("no table texture\n"); return 2; }
	D3DLOCKED_RECT lr;
	tex->LockRect(0, &lr, NULL, 0);
	for (int r = 0; r < rows; r++) memcpy((char*)lr.pBits + r * lr.Pitch, &table[(size_t)r * pitch], pitch * sizeof(float));
	tex->UnlockRect(0);
	gDevice->SetTexture(3, tex);
	const DWORD states[][2] = { { D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP }, { D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP }, { D3DSAMP_MAGFILTER, D3DTEXF_POINT },
		{ D3DSAMP_MINFILTER, D3DTEXF_POINT }, { D3DSAMP_MIPFILTER, D3DTEXF_NONE } };
	for (const auto& s : states) gDevice->SetSamplerState(3, (D3DSAMPLERSTATETYPE)s[0], s[1]);

	std::vector<float> points, fpSimple, fpPbr, allPbr;
	if (!Draw(ps, 2.0f, 0.0f, (float)n, points) || !Draw(ps, 0.0f, 1.0f, (float)n, fpSimple) || !Draw(ps, 0.0f, 0.0f, (float)n, fpPbr) ||
		!Draw(ps, 1.0f, 0.0f, (float)n, allPbr)) { printf("draw failed\n"); return 2; }
	// 1. the CPU's sum of colour x falloff over every lamp, at the GPU's points (double precision, world positions as the table holds them)
	std::vector<double> ref(points.size(), 0.0), allPbrD(allPbr.begin(), allPbr.end());
	size_t reached = 0, maxLamps = 0, entries = 0; double attSum = 0.0;
	for (size_t px = 0; px < (size_t)W * H; px++) {
		const double p[3] = { (double)view.cam[0] + points[px * 4], (double)view.cam[1] + points[px * 4 + 1], (double)view.cam[2] + points[px * 4 + 2] };
		size_t here = 0;
		for (int k = 0; k < n; k++) {
			const float* row = &table[(size_t)k * 8];
			const double d[3] = { row[0] - p[0], row[1] - p[1], row[2] - p[2] };
			const double r = (std::max)((double)row[3], 1.0e-3);
			const double att = 1.0 - (std::min)(1.0, (std::max)(0.0, (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / (r * r)));
			if (att <= 0.0) continue;
			here++; entries++; attSum += att;
			for (int c = 0; c < 3; c++) ref[px * 4 + c] += row[4 + c] * att;
		}
		if (here) reached++;
		maxLamps = (std::max)(maxLamps, here);
	}
	printf("points: %zu, lit by at least one lamp %zu, most lamps on one point %zu; CPU: lamp-point pairs %zu, falloff sum %.1f\n", (size_t)W * H, reached, maxLamps, entries, attSum);
	const bool ok1 = Compare("FPLampSum (froxel list) vs the CPU's sum of every lamp", ref, fpSimple, 1.0e-3, 0.1);
	const bool ok2 = Compare("FPLampSum vs the shader's every-lamp loop, PBR term", allPbrD, fpPbr, 1.0e-4);
	// FPLampSum as the templates compile it (the real getPointLightLightingAtt, no test wrapper) vs the plain every-lamp loop
	bool ok4 = false;
	{
		std::vector<float> fr, dr;
		if (Draw(psReal, 0.0f, 0.0f, (float)n, fr) && Draw(psReal, 1.0f, 0.0f, (float)n, dr, 1.0f)) { std::vector<double> dd(dr.begin(), dr.end()); ok4 = Compare("FPLampSum (real term) vs the every-lamp loop, PBR", dd, fr, 1.0e-4); }
	}
	const bool ok3 = stats.over == 0;
	printf(ok1 && ok2 && ok3 && ok4 ? "FD GLASS MATH: PASS\n" : "FD GLASS MATH: FAIL\n");
	tex->Release();
	delete fast;
	return ok1 && ok2 && ok3 && ok4 ? 0 : 1;
}
