#pragma once

// UNOFFICIAL 2026-11-04 DxvkLoader: the DXVK fork's d3d9.dll from a mod manager's mod. Mod Organizer 2 maps only Data, so a
// d3d9.dll in a mod never reaches the game folder, where Windows looks for it. Instead Data\NVSE\Plugins\VSDxvk\ holds it (as
// the DLSS / RT helpers' NvrDlss / NvrRt folders do), with an optional dxvk.conf. At plugin load, before the game makes its
// Direct3D:
// - DXVK_CONFIG_FILE points DXVK at that dxvk.conf (unless the environment already names one);
// - the d3d9.dll there is loaded by full path, as a second module beside Windows' d3d9.dll (which the game already imports);
// - Windows' Direct3DCreate9 (the game's import, and what other plugins' hooks on it chain to) and Direct3DCreate9Ex jump to
//   DXVK's. The originals are never called again.
// A DXVK d3d9.dll in the game folder (ProductName "DXVK", already loaded as the game's import) is blocked the same way: its
// entry points jump to the mod's, so it never makes a device. Any other wrapper there (ENB, ReShade's d3d9.dll, ...) is left as it
// is and the mod's file is not loaded. Nothing is done in the editor or when the file is missing. GetModuleHandle("d3d9.dll")
// still returns the game folder's or Windows' module afterwards: code that asks which d3d9.dll the game runs on uses Runtime().

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace DxvkLoader {
	inline HMODULE Loaded = nullptr;	// the mod's d3d9.dll, when it was loaded and the entry points redirected

	// The d3d9.dll the game's Direct3D comes from.
	inline HMODULE Runtime() { return Loaded ? Loaded : GetModuleHandleA("d3d9.dll"); }

	inline bool JumpTo(void* from, void* to) {
		DWORD old;
		if (!VirtualProtect(from, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
		uint8_t* p = (uint8_t*)from;
		p[0] = 0xE9;
		*(int32_t*)(p + 1) = (int32_t)((uintptr_t)to - (uintptr_t)from - 5);
		VirtualProtect(from, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), from, 5);
		return true;
	}

	// True when the file's ProductName starts with "DXVK" (what RenderManager's DXVK check reads too).
	inline bool IsDxvk(const char* path) {
		DWORD ignored = 0;
		const DWORD size = GetFileVersionInfoSizeA(path, &ignored);
		if (!size) return false;
		std::vector<char> info(size);
		if (!GetFileVersionInfoA(path, 0, size, info.data())) return false;
		struct LangCodepage { WORD language, codePage; }* table = nullptr;
		UINT bytes = 0;
		if (!VerQueryValueA(info.data(), "\\VarFileInfo\\Translation", (LPVOID*)&table, &bytes)) return false;
		for (UINT i = 0; i < bytes / sizeof(LangCodepage); i++) {
			char key[64];
			sprintf_s(key, "\\StringFileInfo\\%04x%04x\\ProductName", table[i].language, table[i].codePage);
			char* product = nullptr;
			UINT length = 0;
			if (VerQueryValueA(info.data(), key, (LPVOID*)&product, &length) && product && !strncmp(product, "DXVK", 4)) return true;
		}
		return false;
	}

	inline void Load() {
		const std::string dir = Paths::PluginsDir() + "VSDxvk\\";
		const std::string dll = dir + "d3d9.dll", config = dir + "dxvk.conf";
		if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return;	// no mod: nothing to say

		HMODULE system = GetModuleHandleA("d3d9.dll");
		char systemPath[MAX_PATH] = {}, windows[MAX_PATH] = {};
		if (system) GetModuleFileNameA(system, systemPath, MAX_PATH);
		GetSystemWindowsDirectoryA(windows, MAX_PATH);
		const bool windowsOwn = system && !_strnicmp(systemPath, windows, strlen(windows));
		if (!system || (!windowsOwn && !IsDxvk(systemPath))) {
			Logger::Log("DxvkLoader: %s is there, but the game's d3d9.dll is %s, another wrapper: that one is used (remove it from the game folder to use the mod's)",
				dll.c_str(), system ? systemPath : "not loaded");
			return;
		}
		if (!windowsOwn) Logger::Log("DxvkLoader: the game folder's DXVK %s is blocked: the mod's is used", systemPath);

		const bool hasConfig = GetFileAttributesA(config.c_str()) != INVALID_FILE_ATTRIBUTES;
		if (hasConfig && !GetEnvironmentVariableA("DXVK_CONFIG_FILE", nullptr, 0)) SetEnvironmentVariableA("DXVK_CONFIG_FILE", config.c_str());
		HMODULE dxvk = LoadLibraryA(dll.c_str());
		if (!dxvk) { Logger::Log("[ERROR] DxvkLoader: could not load %s (Win32 error %lu): Windows' Direct3D 9", dll.c_str(), GetLastError()); return; }

		int redirected = 0;
		for (const char* name : { "Direct3DCreate9", "Direct3DCreate9Ex" }) {
			void* from = (void*)GetProcAddress(system, name);
			void* to = (void*)GetProcAddress(dxvk, name);
			if (from && to && JumpTo(from, to)) ++redirected;
			else Logger::Log("[ERROR] DxvkLoader: %s not redirected", name);
		}
		if (redirected) Loaded = dxvk;
		char loadedConfig[MAX_PATH] = {};
		GetEnvironmentVariableA("DXVK_CONFIG_FILE", loadedConfig, MAX_PATH);
		Logger::Log("DxvkLoader: %s loaded (%d of 2 entry points redirected; dxvk.conf %s)", dll.c_str(), redirected, loadedConfig[0] ? loadedConfig : "none");
	}

	// UNOFFICIAL 2026-11-09 DxvkDescriptorHeapOff ([Main.Main.Misc], default on; docs/next/RT-PT-REVIEW-2026-11-09.md section 11): DXVK 3.0
	// binds textures and samplers through VK_EXT_descriptor_heap wherever the driver is known good (NVIDIA from 595.84, AMD's and
	// Mesa's drivers that have it). Vegas Shaders' DXVK extension was written and tested without it, and in that mode a pass's
	// second sampler read blank on a tester's RTX 5090 (the ray tracing input pass: one bogus normal for every ray, a screen-fixed
	// triangle in the bounce light); 34 of the mod's Vulkan shaders read through a second sampler. Until the extension is right
	// there, DXVK is told not to use it: "dxvk.enableDescriptorHeap = False" is added to the DXVK_CONFIG environment variable of
	// this process (what DXVK reads after its dxvk.conf), unless the player's dxvk.conf or DXVK_CONFIG already sets that key
	// either way. Called at plugin load after the settings are read, before the game makes its Direct3D. No file is changed.
	inline bool ConfigSetsKey(const std::string& text, const char* key) {
		size_t at = 0;
		while ((at = text.find(key, at)) != std::string::npos) {
			size_t line = text.find_last_of("\n;", at);	// (';' separates DXVK_CONFIG's entries)
			line = line == std::string::npos ? 0 : line + 1;
			size_t first = text.find_first_not_of(" \t\r", line);
			if (first == at) return true;	// the key starts its line or entry: not a comment
			at += strlen(key);
		}
		return false;
	}
	inline void DescriptorHeapOff(bool wanted) {
		static const char* kKey = "dxvk.enableDescriptorHeap";
		if (!wanted) { Logger::Log("DxvkLoader: DxvkDescriptorHeapOff is off: DXVK chooses its binding model itself"); return; }
		char file[MAX_PATH] = {};
		if (!GetEnvironmentVariableA("DXVK_CONFIG_FILE", file, MAX_PATH)) strcpy_s(file, "dxvk.conf");
		std::string conf;
		if (FILE* f = nullptr; !fopen_s(&f, file, "rb") && f) {
			char buffer[4096]; size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) conf.append(buffer, n);
			fclose(f);
		}
		char line[2048] = {};
		const DWORD have = GetEnvironmentVariableA("DXVK_CONFIG", line, sizeof(line));
		if (have >= sizeof(line)) { Logger::Log("DxvkLoader: DXVK_CONFIG is too long to add to: left as it is"); return; }
		if (ConfigSetsKey(conf, kKey) || ConfigSetsKey(line, kKey)) {
			Logger::Log("DxvkLoader: %s is set in %s: left as the player set it", kKey, ConfigSetsKey(conf, kKey) ? file : "DXVK_CONFIG");
			return;
		}
		std::string value = line;
		if (!value.empty()) value += ";";
		value += std::string(kKey) + " = False";
		SetEnvironmentVariableA("DXVK_CONFIG", value.c_str());
		Logger::Log("DxvkLoader: DXVK told not to use its descriptor-heap binding model (DXVK_CONFIG \"%s\"; [Main.Main.Misc] DxvkDescriptorHeapOff)", value.c_str());
	}
}
