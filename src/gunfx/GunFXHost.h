#pragma once
// The Vegas Shaders side of the built-in GunFX (2026-11-16, gunfx-builtin; src/gunfx/GunFX.h is GunFX itself). This file is compiled
// with Vegas Shaders' headers; GunFX.cpp is not (it has types of its own), so everything GunFX needs from Vegas Shaders goes through
// here. No game or Vegas Shaders types in this header: GunFX.cpp includes it too.
namespace GunFXHost {
	// Once a frame (the world pass): the menu switches to GunFX when a setting changed (SettingsRevision), and at the first frame an
	// old GunFX.dll (any module that still exports GunFX_SetSwitches) switched off, once.
	void FrameStart();
	void SyncSwitches();	// the menu switches to GunFX now (also from NVSEPlugin_Load, before any frame)

	// NVSEPlugin_Load, before GunFX::Load: an old GunFX.ini next to the DLL read into the settings once (only the values that differ
	// from GunFX.ini.defaults; an effect the INI switched off turns its menu switch off). [Main.GunFX.General] IniImported marks it done.
	void ImportIniOnce();

	// For GunFX.cpp: one setting by its GunFX.ini section and key (GunFXKeys.h maps them to Main.GunFX.<section>.<key>), as INI
	// text ("1"/"0" for a switch). False when this build has no such setting.
	bool GetValue(const char* iniSection, const char* iniKey, char* out, unsigned size);
	unsigned Revision();				// SettingsRevision: changes with every settings change (the menu, a preset, the test shots)
	void Log(const char* line);			// one line into the Vegas Shaders log, "GunFX: " in front
}
