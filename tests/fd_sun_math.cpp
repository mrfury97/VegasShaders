// UNOFFICIAL 2026-11-14 DeferredSun's maths check (docs/next/FD-SUN-RESULTS.md). Compiles tests/fd_sun_math.hlsl with the game's D3DX against
// the real includes (src/hlsl/NewVegas/Shaders/Includes) four ways -- the template's getSunLighting with SPECULAR and without, the pass's
// getSunLightingModel choosing PBRSun and PBRDiffuse at run time -- draws each over 512 x 512 pixels of pseudo-random inputs (normal, light,
// view, albedo, roughness, metal, sun colour) into an fp32 target on a real D3D9 device, and compares the template's numbers with the pass's.
// The object shaders' frame constants are set to typical values (saturation 0.85, EnergySplit on, Reflectivity 0, linear lighting, gamma
// 2.2, light scale 1.25). Usage: fd_sun_math.exe <repo root>. Exit 0 = every pixel within 1e-5 relative (the two must be the same maths).
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static IDirect3DDevice9* gDevice = nullptr;

static bool MakeDevice() {
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) return false;
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "fdsunmath"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("fdsunmath", "fdsunmath", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = window;
	return SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &gDevice));
}

static IDirect3DPixelShader9* Compile(const std::string& file, bool pass, bool specular, bool fade = false) {
	std::vector<D3DXMACRO> m;
	m.push_back({ "FD_PASS", pass ? "1" : "0" });
	if (specular) m.push_back({ "SPECULAR", "" });
	if (fade) m.push_back({ "FD_FADE", "1" });	// (2026-11-16 fd-sunvis) SpecularFade's form, template vs pass
	m.push_back({ "CAPTURED_NORMALS_ON", "3" });
	m.push_back({ NULL, NULL });
	ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
	HRESULT hr = D3DXCompileShaderFromFileA(file.c_str(), m.data(), NULL, "main", "ps_3_0", 0, &code, &errors, NULL);
	if (errors) { printf("%s\n", (const char*)errors->GetBufferPointer()); errors->Release(); }
	if (FAILED(hr) || !code) return nullptr;
	IDirect3DPixelShader9* ps = nullptr;
	gDevice->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
	code->Release();
	return ps;
}

static const UINT W = 512, H = 512;

static bool Draw(IDirect3DPixelShader9* ps, float spec, std::vector<float>& out, float fade = 1.0f) {
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
	// the object shaders' frame constants (Object.hlsl, PBR.hlsl, LinearLight.hlsl registers)
	const float pbrData[4] = { 0.0f, 1.0f, 1.25f, 1.0f };		// c32 TESR_PBRData: z light scale
	const float pbrExtra[4] = { 0.85f, 1.0f, 0.5f, 4000.0f };	// c33 TESR_PBRExtraData: x saturation
	const float pbrMath[4] = { 1.0f, 0.0f, 0.0f, 0.0f };		// c223 TESR_PBRMath: EnergySplit on, Reflectivity 1
	const float linear[4] = { 2.2f, 1.0f / 2.2f, 0.0f, 0.0f };	// c191 TESR_LinearLighting
	const float sp[4] = { spec, fade, 0.0f, 0.0f };
	const float specFade[4] = { fade, 0.0f, 0.0f, 0.0f };	// c211 NVR_SpecFade: x the template's highlight fade (PBR_SPEC_FADE)
	gDevice->SetPixelShaderConstantF(211, specFade, 1);
	gDevice->SetPixelShaderConstantF(32, pbrData, 1);
	gDevice->SetPixelShaderConstantF(33, pbrExtra, 1);
	gDevice->SetPixelShaderConstantF(223, pbrMath, 1);
	gDevice->SetPixelShaderConstantF(191, linear, 1);
	gDevice->SetPixelShaderConstantF(0, sp, 1);
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

static bool Compare(const char* label, const std::vector<float>& a, const std::vector<float>& b) {
	double maxAbs = 0.0, maxRel = 0.0, sum = 0.0; size_t bad = 0, lit = 0;
	for (size_t i = 0; i < a.size(); i++) {
		if (i % 4 == 3) continue;
		const double d = fabs((double)a[i] - b[i]), m = (std::max)(fabs((double)a[i]), 1e-3);
		maxAbs = (std::max)(maxAbs, d); maxRel = (std::max)(maxRel, d / m); sum += a[i];
		if (a[i] > 0.0f) lit++;
		if (d / m > 1e-5) bad++;
	}
	printf("%-34s max |diff| %.3g, max relative %.3g, channels over 1e-5: %zu of %zu (lit %zu, mean %.4f)\n", label, maxAbs, maxRel, bad, a.size() / 4 * 3, lit, sum / (a.size() / 4 * 3));
	return bad == 0;
}

int main(int argc, char** argv) {
	const std::string root = argc > 1 ? argv[1] : ".";
	const std::string file = root + "\\tests\\fd_sun_math.hlsl";
	// the includes resolve from the file's folder: copy it next to them for the compile
	const std::string shaders = root + "\\src\\hlsl\\NewVegas\\Shaders\\";
	const std::string placed = shaders + "fd_sun_math_test.hlsl";
	if (!CopyFileA(file.c_str(), placed.c_str(), FALSE)) { printf("could not place %s\n", placed.c_str()); return 2; }
	if (!MakeDevice()) { printf("no D3D9 device\n"); DeleteFileA(placed.c_str()); return 2; }
	IDirect3DPixelShader9* tSpec = Compile(placed, false, true), *tDiff = Compile(placed, false, false), *pass = Compile(placed, true, false);
	IDirect3DPixelShader9* tFade = Compile(placed, false, true, true), *passFade = Compile(placed, true, false, true);	// (2026-11-16 fd-sunvis)
	DeleteFileA(placed.c_str());
	if (!tSpec || !tDiff || !pass || !tFade || !passFade) { printf("compile failed\n"); return 2; }
	std::vector<float> a, b, c, d;
	if (!Draw(tSpec, 0.0f, a) || !Draw(pass, 1.0f, b) || !Draw(tDiff, 0.0f, c) || !Draw(pass, 0.0f, d)) { printf("draw failed\n"); return 2; }
	const bool ok1 = Compare("SPECULAR template vs pass (PBRSun)", a, b);
	const bool ok2 = Compare("plain template vs pass (PBRDiffuse)", c, d);
	Compare("(control) SPECULAR vs plain template", a, c);
	// (2026-11-16 fd-sunvis) SpecularFade at 0.37 and at 0: the template's PBR_SPEC_FADE against the pass's lerp form
	std::vector<float> e, f, g, h;
	if (!Draw(tFade, 0.0f, e, 0.37f) || !Draw(passFade, 1.0f, f, 0.37f) || !Draw(tFade, 0.0f, g, 0.0f) || !Draw(passFade, 1.0f, h, 0.0f)) { printf("draw failed\n"); return 2; }
	const bool ok3 = Compare("fade 0.37: template vs pass", e, f);
	const bool ok4 = Compare("fade 0: template vs pass", g, h);
	Compare("(control) fade 0.37 vs no fade", a, e);
	const bool ok = ok1 && ok2 && ok3 && ok4;
	printf(ok ? "FD SUN MATH: PASS\n" : "FD SUN MATH: FAIL\n");
	return ok ? 0 : 1;
}
