#pragma once

// UNOFFICIAL 2026-10-29 [Main.Main.Performance] NewEffectCompiler (P84 v4 did not start for Conor: the main thread waited on the snow
// effect's compile; on a bench run no frame came within 180 s). D3DX9_43's effect compiler (D3DXCreateEffectCompiler + CompileEffect,
// the 2010 one) took minutes on the big effects: SnowAccumulation's macintroll look over 8 minutes, VolumetricClouds about 2.5 minutes,
// all on one core per effect at every start without a cache. Windows' own d3dcompiler_47 (in System32 / SysWOW64 on every Windows 10
// and 11, the compiler fxc uses) still builds fx_2_0 effects, the same binary format D3DXCreateEffectFromFile loads: the same snow
// effect in 6 s. Effects are compiled with it first; where it is missing or refuses an effect, D3DX compiles it as before (logged).
// Off = D3DX only, as before. The cache files keep their names; their check (the preprocessed source) also records which compiler the
// setting chose (CacheText), so switching it recompiles each effect once by itself and a cache made by the other never stays in use.
#include <windows.h>
#include <d3dx9.h>
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include "TestTools.h"

namespace EffectCompiler47 {

	// D3DCompile; its ID3DBlob results have ID3DXBuffer's interface (IUnknown, GetBufferPointer, GetBufferSize), so they go where
	// the D3DX buffers went. The macro and include arguments are left null: the source is preprocessed already (EffectRecord).
	typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DXBuffer**, ID3DXBuffer**);

	inline D3DCompileFn Function() {
		static const D3DCompileFn fn = []() -> D3DCompileFn {
			HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
			return module ? (D3DCompileFn)GetProcAddress(module, "D3DCompile") : nullptr;
		}();
		return fn;
	}

	// The setting, read on the main thread (EffectPrecompile::Start and LoadEffect) and kept for the workers.
	inline std::atomic<bool> Use{ false };
	// 2026-11-02 test only ([Main.Main.Performance] EffectNoPreshaders, absent from the defaults = off; ignored in release builds):
	// effects compiled with D3DCOMPILE_NO_PRESHADER, so the CPU-side expressions fx_2_0 folds out of a shader (preshaders) stay in it
	// and the shader reads the effect's own parameters. A frame recorded this way ('record', FrameRecorder) holds those raw parameters,
	// so frame_replay --no-preshader can swap in a rewritten effect and feed it by name; with preshaders the frame holds only the old
	// expressions' results. Costs some shader time: not for timing captures.
	inline std::atomic<bool> NoPreshaders{ false };
	inline UINT Flags() { return NoPreshaders.load(std::memory_order_relaxed) ? D3DXSHADER_NO_PRESHADER : 0; }	// same bit as D3DCOMPILE_NO_PRESHADER (1 << 8); d3dcompiler_47 only
	// A missing key (an older defaults toml next to the new DLL: macintroll, P84 v4, "stuck loading shaders") means on, the shipped
	// default, not 0 (the old compiler, minutes on the big effects).
	inline void ReadSetting() {
		SettingManager::Configuration::ConfigNode node;
		const bool wanted = TheSettingManager->Config.FillNode(&node, "Main.Main.Performance", "NewEffectCompiler") ? node.BoolValue : true;
		Use.store(wanted && Function() != nullptr, std::memory_order_release);
#if NVR_TEST_TOOLS
		NoPreshaders.store(TheSettingManager->Config.FillNode(&node, "Main.Main.Performance", "EffectNoPreshaders") && node.BoolValue, std::memory_order_release);
#endif
	}

	// What an effect's preprocessed cache file holds and is checked against: PortableSource, plus a line naming the compiler when the
	// new one is chosen (an older cache, made by D3DX, then counts as stale once).
	inline std::string CacheText(ID3DXBuffer* source) {
		std::string text = ShaderProgram::PortableSource(source);
		if (Use.load(std::memory_order_acquire)) text += "\n// NVR: effect compiled with d3dcompiler_47 ([Main.Main.Performance] NewEffectCompiler)\n";
		if (NoPreshaders.load(std::memory_order_acquire)) text += "// NVR: effect compiled without preshaders ([Main.Main.Performance] EffectNoPreshaders)\n";
		return text;
	}
	inline bool CacheFresh(const char* path, ID3DXBuffer* source) {
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file) return false;
		const std::string cached((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		return !cached.empty() && cached == CacheText(source);
	}

	// The effect binary from preprocessed source; false (nothing in *binary) when the compiler is missing or refuses it.
	inline bool Compile(ID3DXBuffer* source, const char* name, ID3DXBuffer** binary, ID3DXBuffer** errors) {
		*binary = nullptr;
		D3DCompileFn fn = Use.load(std::memory_order_acquire) ? Function() : nullptr;
		if (!fn || !source) return false;
		if (SUCCEEDED(fn(source->GetBufferPointer(), source->GetBufferSize(), name, nullptr, nullptr, nullptr, "fx_2_0", Flags(), 0, binary, errors)) && *binary)
			return true;
		if (*binary) { (*binary)->Release(); *binary = nullptr; }
		// Without preshaders some big effects no longer fit shader model 3 (VolumetricClouds, VolumetricFog, BounceLight, Sandstorm on
		// 2026-11-02); D3DX then took minutes on them (no frame within 180 s). Those keep their preshaders: compiled as usual, and a
		// recording cannot swap them.
		if (NoPreshaders.load(std::memory_order_acquire)) {
			if (*errors) { (*errors)->Release(); *errors = nullptr; }
			if (SUCCEEDED(fn(source->GetBufferPointer(), source->GetBufferSize(), name, nullptr, nullptr, nullptr, "fx_2_0", 0, 0, binary, errors)) && *binary) {
				Logger::Log("UNOFFICIAL EffectNoPreshaders: %s does not compile without preshaders, compiled with them", name);
				return true;
			}
			if (*binary) { (*binary)->Release(); *binary = nullptr; }
		}
		return false;
	}
}
