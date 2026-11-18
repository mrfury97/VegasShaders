#pragma once
#include <string>
#include <vector>

// 2026-11-03 the install self-check and the bug report (user: white roads and objects reported on 1.0.1 that never reproduce on the
// bench, in the user's own game or for a tester; the causes found were install mistakes: the old NewVegasReloaded.dll left next to
// VegasShaders.dll, the old shader folder, an old or third-party preset toml, missing textures). The mod looks for those itself at
// start and says what it found on screen, in plain words with what to do:
//   - the game's main menu: a panel at the top for the first 30 s when something needs fixing (ImGuiManager RenderInstallNotice);
//   - the settings menu: a status-bar item, and Help > Install check (every result, Info lines too, and the report button);
//   - the log: one INSTALL line per result.
// Help > Copy bug report puts on the clipboard what a report needs: version + build commit, game + xNVSE, GPU, D3D9 / DXVK, the log
// file's name, the results below, weather mods in the load order, and every setting that differs from the defaults. The text goes
// through the log's privacy scrubber (base/LogScrub.h): the game folder reads <GAME>, no user or computer name, no other folder.
namespace InstallCheck {
	enum class Level { Info, Warning, Problem };	// Problem: likely to break the picture; Warning: may; Info: for reports only
	struct Result {
		Level level;
		std::string group;	// what found it (results of a group are replaced when it runs again: settings are re-read on reload)
		std::string title;	// one short line
		std::string fix;	// what to do, in plain words (empty for Info)
	};

	// NVSEPlugin_Load, after Paths::LogChoice and the twin-DLL check: the plugin folder, the shader folders, the required textures
	void OnPluginLoad(unsigned nvseVersion, unsigned gameVersion);
	// SettingManager::Configuration::Init, after the user's toml is read and BEFORE UpdateOldDefaults marks it with this build's
	// version: fileFound = the file was there and read; unknown = settings this build does not know (section.key)
	void OnSettingsRead(bool fileFound, const std::string& fileName, int settingsVersion, int currentVersion, const std::vector<std::string>& unknown);
	// SessionLog::OnDevice: the GPU(s) and the Direct3D 9 runtime as the log line has them
	void OnDevice(const std::string& gpuWindows, const std::string& gpuGame, bool dxvk, const std::string& d3d9Version, const std::string& nvrDxvk);
	// SessionLog::OnDeferredInit: the load order (plugin names), for weather / lighting mods
	void OnLoadOrder(const std::vector<std::string>& plugins, unsigned hash);

	const std::vector<Result>& Results();
	int Count(Level level);
	// UNOFFICIAL 2026-11-13 a result found later, while the game runs (core/GameShaderPairs.h: shader files of two mods that do not fit)
	void Note(Level level, const char* group, const std::string& title, const std::string& fix);
	const char* LevelName(Level level);

	// The whole report, scrubbed, ready for the clipboard
	std::string BugReport();
}
