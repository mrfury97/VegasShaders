// UNOFFICIAL 2026-11-17: offline test of src/core/DxvkLoader.h AdapterNameFromPrefs (docs/next/DXVK-LAUNCHER.md): which INI folder
// it picks (as FalloutNV 0x00876AD0), which sD3DDevice it reads, and what it adds to DXVK_CONFIG, scenario by scenario in scratch
// folders. No game, no DXVK. The "My Games" scenario only READS the current user's Documents\My Games\FalloutNV INIs.
// Build (x86 Developer prompt):  cl /nologo /EHsc /std:c++17 tests\dxvk_prefs_name.cpp /Fe:<out>\dxvk_prefs_name.exe version.lib shell32.lib
// Run:  <out>\dxvk_prefs_name.exe <empty scratch folder>      exit 0 = every check passed
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

// Stand-ins for what the header takes from Vegas Shaders.
struct Logger { static void Log(const char* format, ...) { va_list a; va_start(a, format); std::printf("    log: "); std::vprintf(format, a); va_end(a); std::puts(""); } };
namespace Paths { inline std::string PluginsDir() { return ".\\"; } inline std::string GameRoot() { return "."; } }
#include "../src/core/DxvkLoader.h"

static int failures = 0;
static std::string root;

static void Write(const std::string& name, const char* text) {
	FILE* f = nullptr;
	if (fopen_s(&f, name.c_str(), "wb") || !f) { std::printf("cannot write %s\n", name.c_str()); std::exit(2); }
	fputs(text, f); fclose(f);
}
static std::string Env(const char* name) { char v[4096] = {}; return GetEnvironmentVariableA(name, v, sizeof(v)) ? v : ""; }

// One scenario: a fresh folder as the current folder, the given files, DXVK_CONFIG preset (null = unset), then the call.
struct Scenario {
	const char* name;
	bool dxvk = true, wanted = true;
	const char* preset = nullptr;
	const char* files[4][2] = {};
	const char* expect;		// DXVK_CONFIG afterwards
};
static void Run(const Scenario& s, int index) {
	const std::string dir = root + "\\s" + std::to_string(index);
	CreateDirectoryA(dir.c_str(), nullptr);
	SetCurrentDirectoryA(dir.c_str());
	for (auto& f : s.files) if (f[0]) Write(f[0], f[1]);
	SetEnvironmentVariableA("DXVK_CONFIG", s.preset);
	SetEnvironmentVariableA("DXVK_CONFIG_FILE", nullptr);
	DxvkLoader::Loaded = s.dxvk ? (HMODULE)GetModuleHandleA(nullptr) : nullptr;
	std::printf("[%d] %s\n", index, s.name);
	DxvkLoader::AdapterNameFromPrefs(s.wanted);
	const std::string got = Env("DXVK_CONFIG");
	const bool ok = got == s.expect;
	if (!ok) ++failures;
	std::printf("    DXVK_CONFIG \"%s\" %s\n", got.c_str(), ok ? "ok" : ("FAIL, expected \"" + std::string(s.expect) + "\"").c_str());
}

int main(int argc, char** argv) {
	if (argc < 2) { std::puts("usage: dxvk_prefs_name <empty scratch folder>"); return 2; }
	root = argv[1];
	CreateDirectoryA(root.c_str(), nullptr);
	const char* gameIni = "[General]\r\nbUseMyGamesDirectory=0\r\n[Display]\r\nsD3DDevice=\"From Fallout Ini\"\r\n";
	const char* prefsGtx = "[Display]\r\niSize W=1280\r\nsD3DDevice=\"NVIDIA GeForce GTX 1070\"\r\n";
	const Scenario scenarios[] = {
		{ "case 1: game-folder INIs (bUseMyGamesDirectory=0 in FALLOUT.INI), prefs name the real card",
			true, true, nullptr, { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", prefsGtx } }, "d3d9.customDeviceDesc = \"NVIDIA GeForce GTX 1070\"" },
		{ "case 2: prefs hold DXVK's name; DescriptorHeapOff's entry already there",
			true, true, "dxvk.enableDescriptorHeap = False", { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", "[Display]\r\nsD3DDevice=\"AMD Radeon RX 6700 XT\"\r\n" } },
			"dxvk.enableDescriptorHeap = False;d3d9.customDeviceDesc = \"AMD Radeon RX 6700 XT\"" },
		{ "Custom.ini switches the folder; no prefs: FALLOUT.INI's name",
			true, true, nullptr, { { "Custom.ini", "[General]\r\nbUseMyGamesDirectory=0\r\n" }, { "FALLOUT.INI", "[Display]\r\nsD3DDevice=\"Only In Fallout Ini\"\r\n" } },
			"d3d9.customDeviceDesc = \"Only In Fallout Ini\"" },
		{ "unquoted name in the prefs", true, true, nullptr, { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", "[Display]\r\nsD3DDevice=NVIDIA GeForce GTX 1070\r\n" } },
			"d3d9.customDeviceDesc = \"NVIDIA GeForce GTX 1070\"" },
		{ "no sD3DDevice anywhere (first start): left alone", true, true, nullptr, { { "FALLOUT.INI", "[General]\r\nbUseMyGamesDirectory=0\r\n" }, { "FalloutPrefs.ini", "[Display]\r\niSize W=1280\r\n" } }, "" },
		{ "empty sD3DDevice: left alone", true, true, nullptr, { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", "[Display]\r\nsD3DDevice=\"\"\r\n" } }, "" },
		{ "case 5: the player's dxvk.conf names the card", true, true, nullptr,
			{ { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", prefsGtx }, { "dxvk.conf", "d3d9.hideNvidiaGpu = True\r\n  d3d9.customDeviceDesc = \"Mine\"\r\n" } }, "" },
		{ "a commented-out customDeviceDesc in dxvk.conf (DXVK's sample file) does not count", true, true, nullptr,
			{ { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", prefsGtx }, { "dxvk.conf", "# d3d9.customDeviceDesc = \"\"\r\nd3d9.hideNvidiaGpu = True\r\n" } },
			"d3d9.customDeviceDesc = \"NVIDIA GeForce GTX 1070\"" },
		{ "case 5: DXVK_CONFIG names the card", true, true, "d3d9.customDeviceDesc = \"Env\"", { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", prefsGtx } }, "d3d9.customDeviceDesc = \"Env\"" },
		{ "a name DXVK_CONFIG cannot carry", true, true, nullptr, { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", "[Display]\r\nsD3DDevice=\"Odd;Name\"\r\n" } }, "" },
		{ "case 4: not DXVK: nothing", false, true, nullptr, { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", prefsGtx } }, "" },
		{ "switch off", true, false, nullptr, { { "FALLOUT.INI", gameIni }, { "FalloutPrefs.ini", prefsGtx } }, "" },
	};
	int i = 0;
	for (const Scenario& s : scenarios) Run(s, ++i);

	// My Games: no INI in the current folder says bUseMyGamesDirectory=0 (read only: the current user's own INIs)
	const std::string dir = root + "\\mygames";
	CreateDirectoryA(dir.c_str(), nullptr);
	SetCurrentDirectoryA(dir.c_str());
	char docs[MAX_PATH] = {};
	SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
	const std::string expected = std::string(docs) + "\\My Games\\FalloutNV\\", folder = DxvkLoader::GameIniFolder();
	const bool ok = folder == expected;
	if (!ok) ++failures;
	std::string name, from;
	const bool found = DxvkLoader::GameD3DDevice(folder, name, from);
	std::printf("[My Games] folder %s %s; sD3DDevice %s%s%s\n", folder.c_str(), ok ? "ok" : "FAIL", found ? "\"" : "(none)", found ? name.c_str() : "", found ? ("\" from " + from).c_str() : "");
	std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
