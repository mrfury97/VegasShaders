#pragma once

// 2026-11-02: the project's name and the names the menu shows, in one place. The window IDs after "###" stay fixed, so renaming a
// title here keeps each window's saved position and docking (imgui.ini).
namespace Branding {
	constexpr const char* ProductName = "Vegas Shaders";
	constexpr const char* MenuTitle = "Vegas Shaders###MainMenu";
	constexpr const char* PresetsTitle = "Presets###PresetManager";
	constexpr const char* LightingTitle = "Lighting###LightingPanel";
	constexpr const char* DevToolsTitle = "Dev Tools###DevTools";
	constexpr const char* AboutTitle = "About Vegas Shaders###About";
	constexpr const char* InstallCheckTitle = "Install check###InstallCheck";	// 2026-11-03 Help > Install check
	constexpr const char* Version = "1.0.7-test2";	// keep in step with FILEVERSION / ProductVersion in NewVegasReloaded.rc
	constexpr const char* VersionLabel = "Vegas Shaders";	// the status bar's version line: "<VersionLabel> <Version>"
}
