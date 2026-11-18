#pragma once

// UNOFFICIAL 2026-11-04 DxvkLoader: the DXVK fork's d3d9.dll from a mod manager's mod. Mod Organizer 2 maps only Data, so a
// d3d9.dll in a mod never reaches the game folder, where Windows looks for it. Instead Data\NVSE\Plugins\VSDxvk\ holds it (as
// the DLSS / RT helpers' NvrDlss / NvrRt folders do), with an optional dxvk.conf. At plugin load, before the game makes its
// Direct3D:
// - DXVK_CONFIG_FILE points DXVK at that dxvk.conf (unless the environment already names one);
// - the d3d9.dll there is loaded by full path, as a second module beside Windows' d3d9.dll (which the game already imports);
// - Windows' Direct3DCreate9 (the game's import, and what other plugins' hooks on it chain to) and Direct3DCreate9Ex jump to
//   DXVK's. The originals are never called again. (2026-11-15: the jump sits behind the functions' first instructions, so hooks
//   other plugins put on them before this plugin loaded keep running: RedirectBehindPrologue.)
// A DXVK d3d9.dll in the game folder (ProductName "DXVK", already loaded as the game's import) is blocked the same way: its
// entry points jump to the mod's, so it never makes a device. Any other wrapper there (ENB, ReShade's d3d9.dll, ...) is left as it
// is and the mod's file is not loaded. Nothing is done in the editor or when the file is missing. GetModuleHandle("d3d9.dll")
// still returns the game folder's or Windows' module afterwards: code that asks which d3d9.dll the game runs on uses Runtime().

#include <cstdint>
#include <cstring>
#include <shlobj.h>	// SHGetFolderPathA (AdapterNameFromPrefs)
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

	// UNOFFICIAL 2026-11-15: the redirect goes BEHIND the function's first instructions, so a hook another plugin already put on
	// Direct3DCreate9 / Ex keeps working (before, JumpTo overwrote it: that plugin never saw the device, e.g. the bench's auto-test
	// plugin, which hooks Direct3DCreate9 at its preload and then ended the game: "the device was created before the plugin could
	// force windowed"). Windows' 32-bit Direct3DCreate9 and Direct3DCreate9Ex begin with the hot-patch prologue
	// "mov edi,edi; push ebp; mov ebp,esp" (8B FF 55 8B EC). A hook (Detours, MinHook, a hot patch) replaces those 5 bytes, and its
	// trampoline runs them and continues at +5. At +5 this writes "pop ebp; jmp <DXVK's function>": the caller's ebp back, the return
	// address on top of the stack again, so DXVK's function sees the call as the game (or the hook) made it. Only when the file's own
	// first 5 bytes are that prologue and the 6 bytes at +5 are still the file's own (read from the d3d9.dll file on disk: FileBytes),
	// and the export resolved inside d3d9.dll; otherwise Load falls back to JumpTo at the first byte, as before, and says so.
	inline const uint8_t kPrologue[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
	// The bytes at an RVA of a 32-bit PE file, read from the file on disk (a 32-bit process reading System32 gets SysWOW64's copy,
	// the one it loaded). Not a second mapping: LoadLibraryEx as an image resource returns the module already loaded, so the
	// first version of this check compared the live, possibly hooked bytes with themselves (bench 11-15: the auto-test plugin's
	// Detours hook made it fall back). Proven with Detours in a 32-bit test program.
	inline bool FileBytes(const char* path, uint32_t rva, uint8_t* out, uint32_t n) {
		HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE) return false;
		std::vector<uint8_t> d;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20);
		if (ok) { d.resize((size_t)size.QuadPart); DWORD got = 0; ok = ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr) && got == d.size(); }
		CloseHandle(f);
		if (!ok || d.size() < sizeof(IMAGE_DOS_HEADER)) return false;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)d.data();
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > d.size()) return false;
		const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(d.data() + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;
		const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
			if ((const uint8_t*)(s + 1) > d.data() + d.size()) return false;
			if (rva < s->VirtualAddress || rva + n > s->VirtualAddress + s->SizeOfRawData) continue;
			const size_t at = (size_t)rva - s->VirtualAddress + s->PointerToRawData;
			if (at + n > d.size()) return false;
			memcpy(out, d.data() + at, n);
			return true;
		}
		return false;
	}
	inline bool RedirectBehindPrologue(HMODULE system, const char* systemPath, void* from, void* to) {
		if (!system || !systemPath || !systemPath[0]) return false;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)system;	// (the loaded module's own headers: its size in memory)
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
		const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const uint8_t*)system + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
		const uintptr_t rva = (uintptr_t)from - (uintptr_t)system;
		if ((uintptr_t)from < (uintptr_t)system || rva + 11 > nt->OptionalHeader.SizeOfImage) return false;	// (the export resolved outside d3d9.dll: someone redirected it)
		uint8_t file[11] = {};
		if (!FileBytes(systemPath, (uint32_t)rva, file, sizeof(file))) return false;
		if (memcmp(file, kPrologue, sizeof(kPrologue)) != 0 || memcmp((const uint8_t*)from + 5, file + 5, 6) != 0) return false;
		uint8_t* at = (uint8_t*)from + 5;
		DWORD old;
		if (!VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
		at[0] = 0x5D;	// pop ebp
		at[1] = 0xE9;	// jmp rel32
		*(int32_t*)(at + 2) = (int32_t)((uintptr_t)to - (uintptr_t)(at + 6));
		VirtualProtect(at, 6, old, &old);
		FlushInstructionCache(GetCurrentProcess(), at, 6);
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
		std::string dir = Paths::PluginsDir() + "VSDxvk\\";
		// UNOFFICIAL 2026-11-15 (testers: "the DXVK build did not work"; their logs had no DxvkLoader line at all, so the file was never
		// found and nothing said so). The zip's folders start at NVSE\: extracted into the GAME folder instead of Data (where a d3d9.dll
		// usually goes) it lands in <game>\NVSE\Plugins\VSDxvk; a second Data level comes from extracting "Data" into Data. Those two
		// places are taken too, with a line saying where the file belongs; when it is nowhere, one line says where it was looked for.
		if (GetFileAttributesA((dir + "d3d9.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
			const std::string misplaced[] = { Paths::GameRoot() + "\\NVSE\\Plugins\\VSDxvk\\", Paths::GameRoot() + "\\Data\\Data\\NVSE\\Plugins\\VSDxvk\\" };
			std::string found;
			for (const std::string& m : misplaced) if (GetFileAttributesA((m + "d3d9.dll").c_str()) != INVALID_FILE_ATTRIBUTES) { found = m; break; }
			if (found.empty()) {
				Logger::Log("DxvkLoader: no Vegas Shaders DXVK build at %sd3d9.dll (optional; the game's own d3d9.dll decides the runtime)", dir.c_str());
				return;
			}
			Logger::Log("[WARNING] DxvkLoader: the DXVK build is at %sd3d9.dll, not where it belongs (%sd3d9.dll: its zip goes into the game's Data "
				"folder, or install it as its own mod): used from there this time", found.c_str(), dir.c_str());
			dir = found;
		}
		const std::string dll = dir + "d3d9.dll", config = dir + "dxvk.conf";

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

		int redirected = 0, behind = 0, kept = 0;
		for (const char* name : { "Direct3DCreate9", "Direct3DCreate9Ex" }) {
			void* from = (void*)GetProcAddress(system, name);
			void* to = (void*)GetProcAddress(dxvk, name);
			if (!from || !to) { Logger::Log("[ERROR] DxvkLoader: %s not redirected (not exported)", name); continue; }
			uint8_t start[5];	// the function's first bytes as found (another plugin's hook, if one was put there before this plugin loaded)
			memcpy(start, from, sizeof(start));
			const bool hookedBefore = memcmp(start, kPrologue, sizeof(kPrologue)) != 0;
			if (RedirectBehindPrologue(system, systemPath, from, to)) {
				++redirected; ++behind;
				if (hookedBefore) {
					++kept;
					Logger::Log("DxvkLoader: %s was already hooked by another plugin (%02X %02X %02X %02X %02X): that hook is kept and still runs before DXVK's",
						name, start[0], start[1], start[2], start[3], start[4]);
				}
			}
			else if (JumpTo(from, to)) {
				++redirected;
				Logger::Log("DxvkLoader: %s redirected at its first byte (the bytes after its start are not Windows' own; start %02X %02X %02X %02X %02X): a hook "
					"another plugin put there before Vegas Shaders loaded no longer runs", name, start[0], start[1], start[2], start[3], start[4]);
			}
			else Logger::Log("[ERROR] DxvkLoader: %s not redirected", name);
		}
		if (redirected) Loaded = dxvk;
		char loadedConfig[MAX_PATH] = {};
		GetEnvironmentVariableA("DXVK_CONFIG_FILE", loadedConfig, MAX_PATH);
		Logger::Log("DxvkLoader: %s loaded (%d of 2 entry points redirected, %d behind the function's first instructions, %d earlier hook(s) kept; dxvk.conf %s)",
			dll.c_str(), redirected, behind, kept, loadedConfig[0] ? loadedConfig : "none");
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

	// UNOFFICIAL 2026-11-17 DxvkAdapterNameFromPrefs ([Main.Main.Misc], default on; docs/next/DXVK-LAUNCHER.md): the launcher loop.
	// At start the game compares the name of the first graphics adapter its Direct3D reports with [Display] sD3DDevice from its
	// INIs, which the launcher wrote; when they differ it starts FalloutNVLauncher.exe (from the registry's install path) and quits
	// (FalloutNV 0x004DA570, called from WinMain after the INIs are read). The launcher then resets many graphics settings
	// (iTexMipMapSkip=1 among them, which crashes with NVTF). DXVK reports another name than Windows: on NVIDIA its built-in profile
	// for this game (d3d9.hideNvidiaGpu) and on Intel its default (d3d9.hideIntelGpu) say "AMD Radeon RX 6700 XT". With DXVK in the
	// game folder the launcher runs on it too and writes that name; from Data\NVSE\Plugins\VSDxvk (a mod manager install) only the
	// game runs on DXVK, the launcher writes Windows' name, the game sees DXVK's, starts the launcher, and so on for ever
	// (a co-developer, 2026-11-17). So whenever the game's Direct3D is DXVK (the mod's, or a DXVK d3d9.dll in the game folder), DXVK is
	// told to report the name the INIs already hold: d3d9.customDeviceDesc = "<that name>" added to DXVK_CONFIG for this process.
	// Only the name: DXVK keeps its vendor and device ids (on NVIDIA still AMD's 1002 / 73DF, as the game and Vegas Shaders have
	// always seen them under DXVK), so DXVK and the game treat the card exactly as before. Nothing on disk is changed. Left alone:
	// INIs that name no card yet (a first start: the launcher runs once, as it does without DXVK), a name DXVK_CONFIG cannot carry
	// (" or ;), a dxvk.conf or DXVK_CONFIG that sets d3d9.customDeviceDesc itself (the player's setting wins), any other d3d9.dll.
	// Cost: a real graphics card change no longer starts the launcher under DXVK (the old settings stay). Called at plugin load
	// after the settings are read, before the game reads its INIs or makes its Direct3D (DXVK reads DXVK_CONFIG when the first
	// Direct3D object is made).

	// The game's INI folder, chosen as FalloutNV 0x00876AD0 does: the game folder when .\Custom.ini or .\FALLOUT.INI (relative to the
	// current folder, which is the game's) has [General] bUseMyGamesDirectory=0, else Documents\My Games\FalloutNV\.
	inline std::string GameIniFolder() {
		if (!GetPrivateProfileIntA("General", "bUseMyGamesDirectory", 1, ".\\Custom.ini") || !GetPrivateProfileIntA("General", "bUseMyGamesDirectory", 1, ".\\FALLOUT.INI"))
			return ".\\";
		char documents[MAX_PATH] = {};
		if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents))) return "";
		return std::string(documents) + "\\My Games\\FalloutNV\\";
	}
	// sD3DDevice as the game reads it: FALLOUT.INI, then FalloutPrefs.ini over it, through GetPrivateProfileString as the game reads
	// its INIs (surrounding quotes stripped; a mod manager's profile INIs are seen the same way). False when neither file has it.
	inline bool GameD3DDevice(const std::string& folder, std::string& name, std::string& from) {
		static const char* kMissing = "\x01";
		bool found = false;
		for (const char* file : { "FALLOUT.INI", "FalloutPrefs.ini" }) {
			char value[512] = {};
			const std::string path = folder + file;
			GetPrivateProfileStringA("Display", "sD3DDevice", kMissing, value, sizeof(value), path.c_str());
			if (strcmp(value, kMissing)) { name = value; from = path; found = true; }
		}
		return found;
	}

	inline void AdapterNameFromPrefs(bool wanted) {
		static const char* kKey = "d3d9.customDeviceDesc";
		char runtime[MAX_PATH] = {};
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		if (!Loaded && !(d3d9 && GetModuleFileNameA(d3d9, runtime, MAX_PATH) && IsDxvk(runtime))) return;	// not DXVK: nothing to do
		if (!wanted) { Logger::Log("DxvkLoader: DxvkAdapterNameFromPrefs is off: DXVK names the graphics card itself"); return; }
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
		if (ConfigSetsKey(conf, kKey) || (have < sizeof(line) && ConfigSetsKey(line, kKey))) {
			Logger::Log("DxvkLoader: %s is set in %s: the graphics card's name is left as the player set it", kKey, ConfigSetsKey(conf, kKey) ? file : "DXVK_CONFIG");
			return;
		}
		const std::string folder = GameIniFolder();
		std::string name, from;
		if (folder.empty() || !GameD3DDevice(folder, name, from) || name.empty()) {
			Logger::Log("DxvkLoader: the game's INIs (%s) name no graphics card yet (sD3DDevice): DXVK's own name is used, and the game starts its "
				"launcher once, as it does without DXVK", folder.empty() ? "Documents folder not found" : folder.c_str());
			return;
		}
		if (name.find_first_of("\";") != std::string::npos) {
			Logger::Log("[WARNING] DxvkLoader: sD3DDevice \"%s\" (%s) cannot be passed to DXVK: DXVK's own name is used", name.c_str(), from.c_str());
			return;
		}
		if (have >= sizeof(line)) { Logger::Log("[WARNING] DxvkLoader: DXVK_CONFIG is too long to add to: DXVK's own graphics card name is used"); return; }
		std::string value = line;
		if (!value.empty()) value += ";";
		value += std::string(kKey) + " = \"" + name + "\"";
		SetEnvironmentVariableA("DXVK_CONFIG", value.c_str());
		Logger::Log("DxvkLoader: DXVK told to name the graphics card \"%s\", as %s does, so the game's graphics-card check does not start the "
			"launcher (vendor and device ids unchanged; DXVK_CONFIG \"%s\"; [Main.Main.Misc] DxvkAdapterNameFromPrefs)", name.c_str(), from.c_str(), value.c_str());
	}
}
