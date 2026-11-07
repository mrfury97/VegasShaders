// 2026-11-03 the install self-check and the bug report (InstallCheck.h).
#include "InstallCheck.h"
#include "Branding.h"
#include "SessionLog.h"
#include "imgui.h"	// the screen size for the report
#include <algorithm>
#if __has_include("NvrBuildCommit.h")
#include "NvrBuildCommit.h"	// written by the project's pre-build step: #define NVR_BUILD_COMMIT "<git commit>[-dirty] <branch>"
#endif
#ifndef NVR_BUILD_COMMIT
#define NVR_BUILD_COMMIT "unknown"
#endif

namespace {

	std::vector<InstallCheck::Result> gResults;
	unsigned gNvse = 0, gGame = 0;
	std::string gGpuWindows, gGpuGame, gD3d9, gNvrDxvk, gLoadOrder;
	bool gDxvk = false, gDeviceSeen = false;

	// The textures the effects load from loose files (TextureManager reads them with D3DX: a BSA does not count). The release zip's
	// Textures folder (resource\Textures) has exactly these; missing ones mean the Textures folder was not copied into Data.
	const char* const kTextures[] = {
		"Effects\\BloodDrops.dds", "Effects\\bloodlens.dds", "Effects\\BloodRuns.dds", "Effects\\BloodSpatterSmall.dds", "Effects\\bluenoise256.dds",
		"Effects\\CloudDetail.dds", "Effects\\CloudShape.dds", "Effects\\CloudWeather.dds", "Effects\\noise.dds", "Effects\\NoiseVolume.dds",
		"Effects\\SMAA_AreaTex.dds", "Effects\\SMAA_SearchTex.dds", "Effects\\Snownoise256.dds", "Effects\\water_NRM_lens.dds",
		"Precipitations\\ripples.dds", "Precipitations\\snow_NRM.dds",	// (2026-11-05: puddles.dds and snow.dds, read by nothing, removed)
		"Precipitations\\splash.dds", "Precipitations\\splash_n.dds", "Precipitations\\water-streaks_n.dds",
		"Water\\caust_001.dds", "Water\\watercalm_NRM.dds", "Water\\water_d.dds", "Water\\water_NRM.dds", "Water\\water_NRM_LOD.dds" };

	// Plugin names that point at a weather or lighting mod (they change the sky and light colours the effects start from)
	const char* const kWeatherWords[] = { "weather", "skies", "climate", "hdr", "lighting", "enb" };

	bool Exists(const std::string& relative) { return GetFileAttributesA((Paths::GameRoot() + "\\" + relative).c_str()) != INVALID_FILE_ATTRIBUTES; }

	bool AnyFile(const std::string& relativeFolder, const char* pattern) {
		WIN32_FIND_DATAA entry;
		const HANDLE search = FindFirstFileA((Paths::GameRoot() + "\\" + relativeFolder + pattern).c_str(), &entry);
		if (search == INVALID_HANDLE_VALUE) return false;
		FindClose(search);
		return true;
	}

	std::string Lower(std::string s) {
		for (char& c : s) c = (char)tolower((unsigned char)c);
		return s;
	}

	void Clear(const char* group) {
		gResults.erase(std::remove_if(gResults.begin(), gResults.end(), [group](const InstallCheck::Result& r) { return r.group == group; }), gResults.end());
	}

	void Add(InstallCheck::Level level, const char* group, const std::string& title, const std::string& fix = std::string()) {
		gResults.push_back({ level, group, title, fix });
		std::stable_sort(gResults.begin(), gResults.end(), [](const InstallCheck::Result& a, const InstallCheck::Result& b) { return (int)a.level > (int)b.level; });
		Logger::Log("INSTALL %s: %s%s%s", InstallCheck::LevelName(level), title.c_str(), fix.empty() ? "" : " | ", fix.c_str());
	}
}

const char* InstallCheck::LevelName(Level level) { return level == Level::Problem ? "PROBLEM" : level == Level::Warning ? "warning" : "info"; }
const std::vector<InstallCheck::Result>& InstallCheck::Results() { return gResults; }

int InstallCheck::Count(Level level) {
	int n = 0;
	for (const Result& r : gResults) if (r.level == level) n++;
	return n;
}

void InstallCheck::OnPluginLoad(unsigned nvseVersion, unsigned gameVersion) {
	gNvse = nvseVersion;
	gGame = gameVersion;
	Clear("files");
	const char* files = "files";

	// An old NewVegasReloaded.dll next to ours (Main.cpp found it): both load, both replace the game's shaders
	if (!Paths::TwinDll.empty()) {
		const char* name = strrchr(Paths::TwinDll.c_str(), '\\') ? strrchr(Paths::TwinDll.c_str(), '\\') + 1 : Paths::TwinDll.c_str();
		Add(Level::Problem, "twin", std::string("Old New Vegas Reloaded plugin still installed: Data\\NVSE\\Plugins\\") + name,
			std::string("Quit, delete or move ") + name + " out of Data\\NVSE\\Plugins (with Mod Organizer: untick the old New Vegas Reloaded mod), "
			"then start again. Both plugins load and both replace the game's shaders: white or wrong objects and ground.");
	}

	// The shader folders: Paths uses the new one when it is there, else the old one
	const bool newShaders = Exists(Paths::NewShaderRoot), oldShaders = Exists(Paths::OldShaderRoot);
	const std::string used = Paths::ShaderRoot();
	if (!newShaders && !oldShaders)
		Add(Level::Problem, files, std::string("Shader folder missing: Data\\") + Paths::ShaderRootShort() + " is not there",
			"Copy the Shaders folder from the Vegas Shaders zip into your game's Data folder (or reinstall the mod with your mod manager). "
			"Without it no effect can load.");
	else if (!newShaders)
		Add(Level::Problem, files, "Using the OLD shader folder Data\\Shaders\\NewVegasReloaded (Data\\Shaders\\VegasShaders is missing)",
			"Only the DLL was updated: those shaders are New Vegas Reloaded's, not this version's, and can turn things white or break effects. "
			"Copy the Shaders folder from the Vegas Shaders zip into Data (Data\\Shaders\\VegasShaders).");
	else if (oldShaders)
		Add(Paths::TwinDll.empty() ? Level::Info : Level::Warning, files,
			"Both shader folders are there: Data\\Shaders\\VegasShaders is used, Data\\Shaders\\NewVegasReloaded is not",
			Paths::TwinDll.empty() ? "" : "The old folder belongs to the old plugin above: remove both together.");
	if (newShaders || oldShaders) {
		const bool effects = AnyFile(Paths::EffectsDir(), "*.fx.hlsl"), shaders = AnyFile(Paths::ShadersDir(), "*.hlsl");
		if (!effects || !shaders)
			Add(Level::Problem, files, std::string("Shader folder incomplete: Data\\") + Paths::ShaderRootShort() + (!effects ? "Effects" : "Shaders") + " has no shader files",
				"Copy the whole Shaders folder from the Vegas Shaders zip into Data again (let it replace the files).");
	}

	// The defaults file decides which settings exist: without ours, every new setting is missing (and an old one is NVR's)
	const std::string plugins = Paths::PluginsDir();
	const std::string defaults = Paths::ReadPath(plugins, Paths::File::Defaults);
	if (GetFileAttributesA(defaults.c_str()) == INVALID_FILE_ATTRIBUTES)
		Add(Level::Problem, files, std::string("Settings defaults missing: Data\\NVSE\\Plugins\\") + Paths::NewName(Paths::File::Defaults) + " is not there",
			std::string("Copy ") + Paths::NewName(Paths::File::Defaults) + " from the zip's NVSE\\Plugins folder into Data\\NVSE\\Plugins. Without it no setting can be read.");
	else if (!Paths::OldPathIfRead(plugins, Paths::File::Defaults).empty())
		Add(Level::Problem, files, std::string("Old settings defaults in use: ") + Paths::OldName(Paths::File::Defaults) + " (New Vegas Reloaded's)",
			std::string("Copy ") + Paths::NewName(Paths::File::Defaults) + " from the zip's NVSE\\Plugins folder into Data\\NVSE\\Plugins.");

	// The textures the effects load themselves
	std::vector<std::string> missing;
	for (const char* t : kTextures) if (!Exists(std::string("Data\\Textures\\") + t)) missing.push_back(std::string("Textures\\") + t);
	if (!missing.empty()) {
		std::string list;
		for (size_t i = 0; i < missing.size() && i < 6; i++) list += (i ? ", " : "") + missing[i];
		if (missing.size() > 6) list += ", ...";
		char title[160];
		sprintf_s(title, "%zu of the mod's textures missing: ", missing.size());
		Add(Level::Problem, files, title + list,
			"Copy the Textures folder from the Vegas Shaders zip into your game's Data folder (or reinstall with your mod manager). "
			"Missing ones leave rain, snow, water, clouds or noise patterns blank or wrong.");
	}

	if (_stricmp(Paths::ModuleDir().c_str(), plugins.c_str()))
		Add(Level::Info, files, "The DLL is loaded from outside Data\\NVSE\\Plugins (a mod manager's virtual folder?)");
}

void InstallCheck::OnSettingsRead(bool fileFound, const std::string& fileName, int settingsVersion, int currentVersion, const std::vector<std::string>& unknown) {
	Clear("settings");
	const char* group = "settings";
	const std::string where = "Data\\NVSE\\Plugins\\" + fileName;
	const std::string reset = "To test the defaults: quit, rename " + where + " to " + fileName + ".bak, and start the game (a new file is made when you Save).";
	if (!fileFound) {
		Add(Level::Info, group, std::string("No settings file yet: the defaults are used (") + Paths::NewName(Paths::File::UserSettings) + " is written when you Save)");
		return;
	}
	if (fileName != Paths::NewName(Paths::File::UserSettings))
		Add(Level::Info, group, "Settings read from " + fileName + " (New Vegas Reloaded's file name)");
	if (settingsVersion == 0)
		Add(Level::Warning, group, "Your settings file " + fileName + " was not saved by Vegas Shaders: a preset made for New Vegas Reloaded or by another tool",
			"Old presets can wash the picture out or turn it white (they were tuned for the old effects). " + reset);
	else if (settingsVersion > currentVersion)
		Add(Level::Warning, group, "Your settings were saved by a NEWER Vegas Shaders than this DLL",
			"An older VegasShaders.dll may be installed, or another mod overwrites it. Install the newest release and check Data\\NVSE\\Plugins.");
	if (!unknown.empty()) {
		std::string list;
		for (size_t i = 0; i < unknown.size() && i < 5; i++) list += (i ? ", " : "") + unknown[i];
		if (unknown.size() > 5) list += ", ...";
		char title[96];
		sprintf_s(title, "%zu setting%s in %s this version does not know (ignored): ", unknown.size(), unknown.size() == 1 ? "" : "s", fileName.c_str());
		const bool many = unknown.size() >= 10;	// a whole preset for another build, not one stray line
		Add(many ? Level::Warning : Level::Info, group, title + list,
			many ? "This looks like a preset for New Vegas Reloaded or another version. " + reset : std::string());
	}
}

void InstallCheck::OnDevice(const std::string& gpuWindows, const std::string& gpuGame, bool dxvk, const std::string& d3d9Version, const std::string& nvrDxvk) {
	gGpuWindows = gpuWindows;
	gGpuGame = gpuGame;
	gDxvk = dxvk;
	gD3d9.clear();	// ASCII only: Windows' own d3d9.dll says "Microsoft(R) Windows(R)" with ANSI symbols, unreadable once pasted
	for (const char c : d3d9Version) if ((unsigned char)c < 0x80) gD3d9 += c;
	gNvrDxvk = nvrDxvk;
	gDeviceSeen = true;
	Clear("device");
	std::string title = dxvk ? "Running under DXVK" : "Running on Windows' own Direct3D 9 (no DXVK)";
	if (dxvk) title += ": d3d9.dll " + (gD3d9.empty() ? std::string("(no version info)") : gD3d9);
	if (!nvrDxvk.empty()) title += "; Vegas Shaders DXVK: " + nvrDxvk;
	Add(Level::Info, "device", title);
}

void InstallCheck::OnLoadOrder(const std::vector<std::string>& plugins, unsigned hash) {
	char head[64];
	sprintf_s(head, "%zu plugins, hash %08X", plugins.size(), hash);
	gLoadOrder = head;
	Clear("loadorder");
	std::string found;
	for (const std::string& p : plugins) {
		const std::string low = Lower(p);
		for (const char* w : kWeatherWords) if (low.find(w) != std::string::npos) { found += (found.empty() ? "" : ", ") + p; break; }
	}
	if (!found.empty())
		Add(Level::Info, "loadorder", "Weather / lighting mods loaded: " + found + " (they change the sky and light the effects start from: name them in a report)");
}

std::string InstallCheck::BugReport() {
	std::string r;
	char line[512];
	sprintf_s(line, "%s %s bug report\r\nBuild: commit %s, compiled %s (%s)\r\n", Branding::ProductName, Branding::Version, NVR_BUILD_COMMIT, __DATE__,
#if NVR_TEST_TOOLS
		"test tools in"
#else
		"release"
#endif
	);
	r += line;
	sprintf_s(line, "Game 1.%u.%u.%u, xNVSE %u.%u.%u\r\n", gGame >> 24, (gGame >> 16) & 0xFF, (gGame >> 4) & 0xFFF, gNvse >> 24, (gNvse >> 16) & 0xFF, (gNvse >> 4) & 0xFFF);
	r += line;
	if (gDeviceSeen) {
		r += "GPU (as the game sees it): " + gGpuGame + "\r\n";
		r += "GPU (Windows): " + (gGpuWindows.empty() ? std::string("?") : gGpuWindows) + "\r\n";
		r += std::string("Direct3D 9: ") + (gDxvk ? "DXVK" : "Windows' own (no DXVK)") + ", d3d9.dll " + (gD3d9.empty() ? "(no version info)" : gD3d9);
		if (!gNvrDxvk.empty()) r += " | Vegas Shaders DXVK: " + gNvrDxvk;
		r += "\r\n";
	}
	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	sprintf_s(line, "Screen: %.0fx%.0f\r\n", screen.x, screen.y);
	r += line;
	if (!gLoadOrder.empty()) r += "Load order: " + gLoadOrder + "\r\n";
	std::string log = Logger::FilePath;
	if (log.size() > 2 && log[1] != ':' && log[0] != '\\') log = Paths::GameRoot() + "\\" + log;	// relative to the game folder
	r += "Log file (attach it): " + (log.empty() ? std::string("(none)") : log) + "\r\n";

	r += "\r\nInstall check:\r\n";
	if (gResults.empty()) r += "  nothing found\r\n";
	for (const Result& c : gResults) r += std::string("  [") + LevelName(c.level) + "] " + c.title + "\r\n";

	unsigned differ = 0, total = 0;
	const std::vector<std::string> diff = SessionLog::SettingsDiff(differ, total);
	sprintf_s(line, "\r\nSettings that differ from the defaults: %u of %u%s (key=yours (default))\r\n", differ, total,
		TheSettingManager && TheSettingManager->Config.MaxQualityActive ? "; Max Quality is ON" : "");
	r += line;
	for (const std::string& d : diff) r += "  " + d + "\r\n";
	r += "\r\nWhat happened (where, what you see, what you expected):\r\n\r\n";
	return Logger::Scrub(r);	// <GAME> for the game folder, no user / computer names (LogScrub.h)
}
