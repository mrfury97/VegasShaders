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

#include <algorithm>
#include <cctype>
#include <cfgmgr32.h>	// CM_Get_DevNode_Status (SkipUnusedVulkanIcds)
#include <cstdint>
#include <cstring>
#include <setupapi.h>	// SetupDiGetClassDevsA (SkipUnusedVulkanIcds)
#include <shlobj.h>	// SHGetFolderPathA (AdapterNameFromPrefs)
#include <string>
#include <vector>

namespace DxvkLoader {
	inline HMODULE Loaded = nullptr;	// the mod's d3d9.dll, when it was loaded and the entry points redirected
	inline std::string BlockedRootDxvk;	// root <game>\d3d9.dll (another DXVK) that VSDxvk bypassed
	inline std::string OverridingWrapper;	// non-DXVK wrapper (ENB/ReShade/SpecialK) or unbypassable root d3d9.dll preventing VSDxvk
	inline std::string RootConfIgnored;	// leftover <game>\dxvk.conf ignored because a root DXVK was blocked and VSDxvk ships none

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

	inline bool IsInWindowsDir(const char* path) {
		if (!path || !*path) return false;
		char windows[MAX_PATH] = {};
		const DWORD n = GetSystemWindowsDirectoryA(windows, MAX_PATH);
		if (!n) return false;
		if (_strnicmp(path, windows, n) != 0) return false;
		return windows[n - 1] == '\\' || windows[n - 1] == '/' || path[n] == '\\' || path[n] == '/' || path[n] == '\0';
	}

	inline bool ContainsI(const char* haystack, const char* needle) {
		if (!haystack || !needle || !*needle) return false;
		const size_t nlen = strlen(needle);
		for (const char* p = haystack; *p; ++p) {
			size_t i = 0;
			while (i < nlen && p[i] && std::tolower((unsigned char)p[i]) == std::tolower((unsigned char)needle[i])) ++i;
			if (i == nlen) return true;
		}
		return false;
	}

	inline bool HasAsciiBytes(const std::vector<uint8_t>& buf, const char* needle) {
		if (!needle || !*needle) return false;
		const auto* n = reinterpret_cast<const uint8_t*>(needle);
		const size_t nlen = strlen(needle);
		return std::search(buf.begin(), buf.end(), n, n + nlen) != buf.end();
	}

	inline bool IsKnownNonDxvkWrapper(HMODULE mod) {
		if (!mod) return false;
		static const char* const kExports[] = {
			"ReShadeRegisterAddon", "ReShadeLogMessage",
			"ENBGetSDKVersion", "ENBGetVersion",
			"SK_GetModuleName", "SK_IsInjected"
		};
		for (const char* name : kExports) {
			if (GetProcAddress(mod, name)) return true;
		}
		return false;
	}

	// Binary PE marker fallback for stripped or resource-less DXVK builds (DXVK loads vulkan-1.dll
	// dynamically, so it is absent from static IATs, while DXVK binaries carry "vulkan-1.dll" and
	// "DXVK_CONFIG" or "dxvk.conf").
	inline bool FileLooksLikeDxvk(const char* path) {
		if (!path || !*path) return false;
		HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER li = {};
		if (!GetFileSizeEx(f, &li) || li.QuadPart < (LONGLONG)sizeof(IMAGE_DOS_HEADER) || li.QuadPart > (64LL << 20)) {
			CloseHandle(f);
			return false;
		}
		std::vector<uint8_t> buf((size_t)li.QuadPart);
		size_t total = 0;
		while (total < buf.size()) {
			const size_t rem = buf.size() - total;
			const DWORD chunk = rem > (1u << 20) ? (1u << 20) : (DWORD)rem;
			DWORD got = 0;
			if (!ReadFile(f, buf.data() + total, chunk, &got, nullptr) || got == 0) break;
			total += got;
		}
		CloseHandle(f);
		if (total != buf.size()) return false;
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf.data());
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
		const size_t peOff = (size_t)dos->e_lfanew;
		if (peOff > buf.size() - sizeof(DWORD)) return false;
		if (*reinterpret_cast<const DWORD*>(buf.data() + peOff) != IMAGE_NT_SIGNATURE) return false;
		return HasAsciiBytes(buf, "vulkan-1.dll") && (HasAsciiBytes(buf, "DXVK_CONFIG") || HasAsciiBytes(buf, "dxvk.conf"));
	}

	// Rewrites FalloutNV.exe's d3d9.dll IAT entries when bypassing a non-system root d3d9.dll (another DXVK),
	// so game calls reach VSDxvk even if the root DLL's prologue cannot be inline-patched. Not used for
	// Windows' own d3d9.dll so pre-loaded plugins keeping behind-prologue hooks stay intact.
	inline int PatchGameIat(void* orig9, void* new9, void* orig9Ex, void* new9Ex) {
		if ((!orig9 || !new9) && (!orig9Ex || !new9Ex)) return 0;
		HMODULE exe = GetModuleHandleA(nullptr);
		if (!exe) return 0;
		const uint8_t* base = reinterpret_cast<const uint8_t*>(exe);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
		const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
		if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return 0;
		const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= imageSize) return 0;

		int patched = 0;
		const auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
		while (reinterpret_cast<const uint8_t*>(imp + 1) <= base + imageSize && imp->Name) {
			if (imp->Name < imageSize && imp->FirstThunk && imp->FirstThunk + sizeof(IMAGE_THUNK_DATA32) <= imageSize) {
				const char* dllName = reinterpret_cast<const char*>(base + imp->Name);
				if (_stricmp(dllName, "d3d9.dll") == 0) {
					auto* iat = reinterpret_cast<IMAGE_THUNK_DATA32*>(const_cast<uint8_t*>(base) + imp->FirstThunk);
					while (reinterpret_cast<const uint8_t*>(iat + 1) <= base + imageSize && iat->u1.Function) {
						void* cur = reinterpret_cast<void*>(static_cast<uintptr_t>(iat->u1.Function));
						void* target = nullptr;
						if (orig9 && new9 && cur == orig9) target = new9;
						else if (orig9Ex && new9Ex && cur == orig9Ex) target = new9Ex;
						if (target && target != cur) {
							DWORD oldProt = 0;
							if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &oldProt)) {
								iat->u1.Function = static_cast<DWORD>(reinterpret_cast<uintptr_t>(target));
								DWORD dummy = 0;
								VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), oldProt, &dummy);
								++patched;
							}
						}
						++iat;
					}
				}
			}
			++imp;
		}
		return patched;
	}

	// True when the file is DXVK (version info ProductName/FileDescription contains "DXVK", or stripped PE fallback).
	inline bool IsDxvk(const char* path, HMODULE mod = nullptr) {
		if (!path || !*path || IsInWindowsDir(path)) return false;
		if (IsKnownNonDxvkWrapper(mod)) return false;
		DWORD ignored = 0;
		const DWORD size = GetFileVersionInfoSizeA(path, &ignored);
		if (size) {
			std::vector<char> info(size);
			if (GetFileVersionInfoA(path, 0, size, info.data())) {
				struct LangCodepage { WORD language, codePage; }* table = nullptr;
				UINT bytes = 0;
				if (VerQueryValueA(info.data(), "\\VarFileInfo\\Translation", (LPVOID*)&table, &bytes)) {
					for (UINT i = 0; i < bytes / sizeof(LangCodepage); i++) {
						for (const char* field : { "ProductName", "FileDescription" }) {
							char key[64];
							sprintf_s(key, "\\StringFileInfo\\%04x%04x\\%s", table[i].language, table[i].codePage, field);
							char* val = nullptr;
							UINT length = 0;
							if (VerQueryValueA(info.data(), key, (LPVOID*)&val, &length) && val) {
								if (ContainsI(val, "ReShade") || ContainsI(val, "ENBSeries") || ContainsI(val, "Special K")) return false;
								if (ContainsI(val, "DXVK")) return true;
							}
						}
					}
				}
			}
		}
		return FileLooksLikeDxvk(path);
	}

	inline void Load() {
		BlockedRootDxvk.clear();
		OverridingWrapper.clear();
		RootConfIgnored.clear();

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
		char systemPath[MAX_PATH] = {};
		if (system) GetModuleFileNameA(system, systemPath, MAX_PATH);
		const bool windowsOwn = system && IsInWindowsDir(systemPath);
		if (!system || (!windowsOwn && !IsDxvk(systemPath, system))) {
			if (system) OverridingWrapper = systemPath;
			Logger::Log("DxvkLoader: %s is there, but the game's d3d9.dll is %s, another wrapper: that one is used (remove it from the game folder to use the mod's)",
				dll.c_str(), system ? systemPath : "not loaded");
			return;
		}
		if (!windowsOwn) {
			BlockedRootDxvk = systemPath;
			Logger::Log("DxvkLoader: the game folder's DXVK %s is blocked: the mod's is used", systemPath);
		}

		const bool hasConfig = GetFileAttributesA(config.c_str()) != INVALID_FILE_ATTRIBUTES;
		const bool hasEnvConfig = GetEnvironmentVariableA("DXVK_CONFIG_FILE", nullptr, 0) != 0;
		if (hasConfig && !hasEnvConfig) SetEnvironmentVariableA("DXVK_CONFIG_FILE", config.c_str());
		else if (!hasConfig && !windowsOwn && !hasEnvConfig) {
			const std::string rootConf = Paths::GameRoot() + "\\dxvk.conf";
			if (GetFileAttributesA(rootConf.c_str()) != INVALID_FILE_ATTRIBUTES) {
				RootConfIgnored = rootConf;
				SetEnvironmentVariableA("DXVK_CONFIG_FILE", config.c_str());	// point away from CWD <game>\dxvk.conf (missing path -> empty config)
				Logger::Log("DxvkLoader: ignoring stale %s left next to blocked root d3d9.dll", rootConf.c_str());
			}
		}

		HMODULE dxvk = LoadLibraryA(dll.c_str());
		if (!dxvk) {
			BlockedRootDxvk.clear();
			Logger::Log("[ERROR] DxvkLoader: could not load %s (Win32 error %lu): Windows' Direct3D 9", dll.c_str(), GetLastError());
			return;
		}

		void* origCreate9 = (void*)GetProcAddress(system, "Direct3DCreate9");
		void* origCreate9Ex = (void*)GetProcAddress(system, "Direct3DCreate9Ex");
		void* newCreate9 = (void*)GetProcAddress(dxvk, "Direct3DCreate9");
		void* newCreate9Ex = (void*)GetProcAddress(dxvk, "Direct3DCreate9Ex");

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
		int iatPatched = 0;
		if (!windowsOwn) iatPatched = PatchGameIat(origCreate9, newCreate9, origCreate9Ex, newCreate9Ex);
		if (redirected || iatPatched) Loaded = dxvk;
		else if (!windowsOwn) {
			OverridingWrapper = systemPath;
			BlockedRootDxvk.clear();
		}
		char loadedConfig[MAX_PATH] = {};
		GetEnvironmentVariableA("DXVK_CONFIG_FILE", loadedConfig, MAX_PATH);
		const std::string confDesc = !RootConfIgnored.empty() ? ("none (ignored stale " + RootConfIgnored + ")") : (loadedConfig[0] ? loadedConfig : "none");
		Logger::Log("DxvkLoader: %s loaded (%d of 2 entry points redirected, %d behind the function's first instructions, %d earlier hook(s) kept, %d game IAT slot(s) patched; dxvk.conf %s)",
			dll.c_str(), redirected, behind, kept, iatPatched, confDesc.c_str());
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

	// UNOFFICIAL 2026-11-23 DxvkSkipUnusedIcd ([Main.Main.Misc], default on): on desktop systems with a discrete GPU and a CPU
	// integrated GPU (e.g. Ryzen 7000/9000 + NVIDIA), vulkan-1.dll loads every registered 32-bit ICD into FalloutNV.exe during
	// device enumeration and leaves unused drivers (and their memory-mapped shader caches, e.g. amdvlk32.dll + 256 MB .parc) in
	// the 4 GB 32-bit address space. Hide unused integrated Vulkan ICDs via VK_LOADER_DRIVERS_DISABLE (and AMD's implicit
	// switchable-graphics layer via DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1) unless the user already configured Vulkan/DXVK
	// adapter selection.
	inline void SkipUnusedVulkanIcds(bool wanted) {
		char runtime[MAX_PATH] = {};
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		if (!Loaded && !(d3d9 && GetModuleFileNameA(d3d9, runtime, MAX_PATH) && IsDxvk(runtime))) return;
		if (!wanted) { Logger::Log("DxvkLoader: DxvkSkipUnusedIcd is off: every Vulkan driver stays enabled"); return; }

		for (const char* envKey : { "VK_LOADER_DRIVERS_DISABLE", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "DXVK_FILTER_DEVICE_NAME" }) {
			if (GetEnvironmentVariableA(envKey, nullptr, 0) != 0) {
				Logger::Log("DxvkLoader: %s is already set: Vulkan ICD selection left as configured", envKey);
				return;
			}
		}

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
		const std::string dxvkEnv = have < sizeof(line) ? line : "";
		for (const char* key : { "dxvk.deviceFilter", "dxvk.hideIntegratedGraphics" }) {
			if (ConfigSetsKey(conf, key) || ConfigSetsKey(dxvkEnv, key)) {
				Logger::Log("DxvkLoader: %s is set in %s: Vulkan ICD selection left as configured", key, ConfigSetsKey(conf, key) ? file : "DXVK_CONFIG");
				return;
			}
		}

		char sysDir[MAX_PATH] = {};
		const UINT sysLen = GetSystemDirectoryA(sysDir, MAX_PATH);
		HMODULE setupapi = sysLen ? LoadLibraryA((std::string(sysDir) + "\\setupapi.dll").c_str()) : nullptr;
		HMODULE cfgmgr32 = sysLen ? LoadLibraryA((std::string(sysDir) + "\\cfgmgr32.dll").c_str()) : nullptr;
		const auto getClassDevs = setupapi ? reinterpret_cast<decltype(&::SetupDiGetClassDevsA)>(GetProcAddress(setupapi, "SetupDiGetClassDevsA")) : nullptr;
		const auto enumDeviceInfo = setupapi ? reinterpret_cast<decltype(&::SetupDiEnumDeviceInfo)>(GetProcAddress(setupapi, "SetupDiEnumDeviceInfo")) : nullptr;
		const auto getDevProp = setupapi ? reinterpret_cast<decltype(&::SetupDiGetDeviceRegistryPropertyA)>(GetProcAddress(setupapi, "SetupDiGetDeviceRegistryPropertyA")) : nullptr;
		const auto destroyDevs = setupapi ? reinterpret_cast<decltype(&::SetupDiDestroyDeviceInfoList)>(GetProcAddress(setupapi, "SetupDiDestroyDeviceInfoList")) : nullptr;
		const auto cmGetStatus = cfgmgr32 ? reinterpret_cast<decltype(&::CM_Get_DevNode_Status)>(GetProcAddress(cfgmgr32, "CM_Get_DevNode_Status")) : nullptr;

		static const GUID kDisplayClass = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
		HDEVINFO devs = (getClassDevs && enumDeviceInfo && getDevProp && destroyDevs && cmGetStatus)
			? getClassDevs(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT)
			: INVALID_HANDLE_VALUE;
		if (devs == INVALID_HANDLE_VALUE) {
			const DWORD err = GetLastError();
			if (cfgmgr32) FreeLibrary(cfgmgr32);
			if (setupapi) FreeLibrary(setupapi);
			Logger::Log("DxvkLoader: SkipUnusedVulkanIcds skipped (SetupAPI unavailable, err=%lu)", err);
			return;
		}

		bool hasDiscreteNvidia = false, hasDiscreteAmd = false, hasIgpuAmd = false, hasDiscreteIntel = false, hasIgpuIntel = false;
		constexpr unsigned long long kDiscreteFloor = 1536ULL << 20;
		UINT activeCount = 0;

		SP_DEVINFO_DATA dev = {};
		dev.cbSize = sizeof(dev);
		for (DWORD i = 0; enumDeviceInfo(devs, i, &dev); i++) {
			ULONG status = 0, problem = 0;
			if (cmGetStatus(&status, &problem, dev.DevInst, 0) != CR_SUCCESS || !(status & DN_STARTED) || problem != 0)
				continue;
			activeCount++;

			char hwid[1024] = {};
			if (!getDevProp(devs, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(hwid), sizeof(hwid) - 2, nullptr))
				continue;
			bool isNvidia = false, isAmd = false, isIntel = false;
			for (const char* s = hwid; *s; s += strlen(s) + 1) {
				if (ContainsI(s, "VEN_10DE")) isNvidia = true;
				if (ContainsI(s, "VEN_1002")) isAmd = true;
				if (ContainsI(s, "VEN_8086")) isIntel = true;
			}
			if (!isNvidia && !isAmd && !isIntel) continue;

			char drv[256] = {};
			if (!getDevProp(devs, &dev, SPDRP_DRIVER, nullptr, reinterpret_cast<PBYTE>(drv), sizeof(drv) - 1, nullptr) || !drv[0])
				continue;
			char regPath[384];
			sprintf_s(regPath, "SYSTEM\\CurrentControlSet\\Control\\Class\\%s", drv);

			unsigned long long vram = 0;
			DWORD size = sizeof(vram);
			if (RegGetValueA(HKEY_LOCAL_MACHINE, regPath, "HardwareInformation.qwMemorySize", RRF_RT_QWORD, nullptr, &vram, &size) != ERROR_SUCCESS) {
				DWORD vram32 = 0;
				size = sizeof(vram32);
				if (RegGetValueA(HKEY_LOCAL_MACHINE, regPath, "HardwareInformation.MemorySize", RRF_RT_DWORD | RRF_RT_REG_BINARY, nullptr, &vram32, &size) == ERROR_SUCCESS)
					vram = vram32;
			}

			if (isNvidia && vram >= kDiscreteFloor) hasDiscreteNvidia = true;
			else if (isAmd) {
				if (vram >= kDiscreteFloor) hasDiscreteAmd = true;
				else hasIgpuAmd = true;
			}
			else if (isIntel) {
				if (vram >= kDiscreteFloor) hasDiscreteIntel = true;
				else hasIgpuIntel = true;
			}
		}

		destroyDevs(devs);
		FreeLibrary(cfgmgr32);
		FreeLibrary(setupapi);

		const bool hideAmd = (hasDiscreteNvidia || hasDiscreteIntel) && !hasDiscreteAmd && hasIgpuAmd;
		const bool hideIntel = (hasDiscreteNvidia || hasDiscreteAmd) && !hasDiscreteIntel && hasIgpuIntel;

		std::string disable;
		auto append = [&](const char* pat) {
			if (!disable.empty()) disable += ",";
			disable += pat;
		};
		bool amdLayerDisabled = false;
		if (hideAmd) {
			append("*amd-vulkan*,*amd_icd*");
			if (GetEnvironmentVariableA("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", nullptr, 0) == 0) {
				if (SetEnvironmentVariableA("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", "1")) amdLayerDisabled = true;
			}
		}
		if (hideIntel) append("*igvk*");
		if (disable.empty()) {
			Logger::Log("DxvkLoader: SkipUnusedVulkanIcds: no unused integrated Vulkan ICD to hide (%u active display adapter(s))", activeCount);
			return;
		}

		if (SetEnvironmentVariableA("VK_LOADER_DRIVERS_DISABLE", disable.c_str())) {
			Logger::Log("DxvkLoader: hiding unused integrated Vulkan ICD(s) to save 32-bit address space (VK_LOADER_DRIVERS_DISABLE=\"%s\"%s; [Main.Main.Misc] DxvkSkipUnusedIcd)",
				disable.c_str(), amdLayerDisabled ? ", DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=\"1\"" : "");
		}
	}

	// UNOFFICIAL 2026-11-23 DxvkCompilerThreads ([Main.Main.Misc], default 3): DXVK defaults to spawning ~3/4 of CPU hardware
	// threads for background dxvk-shader compilation (8-12+ threads on modern CPUs). Running those simultaneously with Vegas
	// Shaders' D3D shader compiler threads inside a 32-bit WOW64 process saturates memory bandwidth/VA and can hang LoadingMenu.
	inline void CapCompilerThreads(int threads) {
		static const char* kKey = "dxvk.numCompilerThreads";
		char runtime[MAX_PATH] = {};
		HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
		if (!Loaded && !(d3d9 && GetModuleFileNameA(d3d9, runtime, MAX_PATH) && IsDxvk(runtime))) return;
		if (threads <= 0) {
			Logger::Log("DxvkLoader: DxvkCompilerThreads is 0: DXVK chooses its shader compiler thread count itself");
			return;
		}
		if (threads > 16) threads = 16;

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
		if (have >= sizeof(line)) {
			Logger::Log("[WARNING] DxvkLoader: DXVK_CONFIG is too long to add to: %s left to DXVK", kKey);
			return;
		}
		if (ConfigSetsKey(conf, kKey) || ConfigSetsKey(line, kKey)) {
			Logger::Log("DxvkLoader: %s is set in %s: left as the player set it", kKey, ConfigSetsKey(conf, kKey) ? file : "DXVK_CONFIG");
			return;
		}

		char entry[64];
		sprintf_s(entry, "%s = %d", kKey, threads);
		std::string value = line;
		if (!value.empty()) value += ";";
		value += entry;
		SetEnvironmentVariableA("DXVK_CONFIG", value.c_str());
		Logger::Log("DxvkLoader: DXVK shader compiler threads capped to %d to prevent 32-bit startup saturation (DXVK_CONFIG \"%s\"; [Main.Main.Misc] DxvkCompilerThreads)",
			threads, value.c_str());
	}
}
