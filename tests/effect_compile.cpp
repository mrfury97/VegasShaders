// Compiles NVR effect files the way EffectRecord::LoadEffect does in the game (D3DXPreprocessShaderFromFileA with the
// effects' defines, then D3DXCreateEffectCompiler + CompileEffect with no flags, D3DX 9.43), without the game. A shader
// that fails there fails silently in game: the effect is switched off and only the log says why.
// Usage: build\effect-test\effect_compile.exe <effect .fx.hlsl file> [more files...]   (exit code 1 on any failure)
#include <windows.h>
#include <d3dx9.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static bool CompileOne(const char* path, const char* forwardShadows) {
	// UNOFFICIAL 2026-11-14 EFFECT_DEFINE=NAME (test only): one more define, value 1 (GBUFFER_V2: core/CapturedNormals.h, EffectRecord::BuildMacros)
	const char* extra = getenv("EFFECT_DEFINE");
	D3DXMACRO macros[] = { { "FORWARD_SHADOWS", forwardShadows }, { extra && *extra ? extra : NULL, "1" }, { NULL, NULL } };
	ID3DXBuffer *source = nullptr, *errors = nullptr, *effect = nullptr;
	ID3DXEffectCompiler* compiler = nullptr;
	HRESULT hr = D3DXPreprocessShaderFromFileA(path, macros, NULL, &source, &errors);
	bool ok = SUCCEEDED(hr);
	if (ok) {
		if (errors) { errors->Release(); errors = nullptr; }
		hr = D3DXCreateEffectCompiler((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, 0, &compiler, &errors);
		ok = SUCCEEDED(hr);
		if (ok) {
			if (errors) { errors->Release(); errors = nullptr; }
			hr = compiler->CompileEffect(getenv("EFFECT_FLAGS") ? (DWORD)atoi(getenv("EFFECT_FLAGS")) : 0, &effect, &errors);	// (EFFECT_FLAGS: test only, 2026-10-16)
			ok = SUCCEEDED(hr);
		}
	}
	// EFFECT_DISASM (test only, 2026-10-16): the compiled effect disassembled (preshaders and register use), on a NULLREF device.
	if (ok && effect && getenv("EFFECT_DISASM")) {
		IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
		D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = GetDesktopWindow();
		IDirect3DDevice9* dev = nullptr; ID3DXEffect* fx = nullptr; ID3DXBuffer* text = nullptr;
		if (d3d && SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, GetDesktopWindow(), D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) &&
			SUCCEEDED(D3DXCreateEffect(dev, effect->GetBufferPointer(), effect->GetBufferSize(), NULL, NULL, 0, NULL, &fx, NULL)) &&
			SUCCEEDED(D3DXDisassembleEffect(fx, FALSE, &text))) std::printf("%s\n", (const char*)text->GetBufferPointer());
		else std::printf("(no disassembly)\n");
		if (text) text->Release(); if (fx) fx->Release(); if (dev) dev->Release(); if (d3d) d3d->Release();
	}
	std::printf("%s %s (FORWARD_SHADOWS %s)%s\n", ok ? "PASS:" : "FAIL:", path, forwardShadows, ok && effect ? "" : "");
	if (!ok && errors) std::printf("%s\n", (const char*)errors->GetBufferPointer());
	if (effect) effect->Release();
	if (compiler) compiler->Release();
	if (errors) errors->Release();
	if (source) source->Release();
	return ok;
}

// UNOFFICIAL 2026-10-16: --ps <file> <entry>: one ps_3_0 function compiled with the same D3DX (flags 0) and disassembled, to see
// where this compiler puts constants (it ran out of registers on PointShadows.fx where a newer fxc did not).
static int Disassemble(const char* path, const char* entry) {
	ID3DXBuffer *code = nullptr, *errors = nullptr, *text = nullptr;
	ID3DXConstantTable* table = nullptr;
	HRESULT hr = D3DXCompileShaderFromFileA(path, NULL, NULL, entry, "ps_3_0", 0, &code, &errors, &table);
	if (FAILED(hr)) { std::printf("FAIL %s\n", errors ? (const char*)errors->GetBufferPointer() : "?"); return 1; }
	D3DXDisassembleShader((const DWORD*)code->GetBufferPointer(), FALSE, NULL, &text);
	if (text) std::printf("%s\n", (const char*)text->GetBufferPointer());
	return 0;
}

int main(int argc, char** argv) {
	if (argc == 4 && !strcmp(argv[1], "--ps")) return Disassemble(argv[2], argv[3]);
	if (argc < 2) { std::puts("usage: effect_compile <effect .fx.hlsl file> [more files...]"); return 2; }
	int failures = 0;
	for (int i = 1; i < argc; i++) {
		if (!CompileOne(argv[i], "0")) failures++;
		if (!CompileOne(argv[i], "1")) failures++;
	}
	std::printf(failures ? "%d effect compile(s) FAILED\n" : "All effects compiled\n", failures);
	return failures ? 1 : 0;
}
