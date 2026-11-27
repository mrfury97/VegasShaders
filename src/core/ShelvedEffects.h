#pragma once

// UNOFFICIAL 2026-11-16 (owner: "shelve bloodflow and dust motes before the nexus release, we still need to work on them. they
// can live in the offline dev build"): effects left out of RELEASE builds while they are unfinished. A release tree carries the
// marker file ReleaseBuild.h next to this one (as TestTools.h); in it the effects named below are never created (no object, no
// shader compiled, no buffer), their menu pages and search entries do not show, and one log line says so. Their settings stay in
// the defaults, so a player's saved keys are kept as they are (no unknown-key line, nothing rewritten on Save), and HandBlood
// still reads [Shaders.BloodFlow.Main] MatchShade. Development builds (no marker): nothing changes.
// (2026-11-27: capsule shadows, shelved here as a setting since 11-16, were removed from the code: owner, "just remove the capsule stuff")
// To bring one back into releases: take its name out of kNames and its guard lines (docs/next/SHELVED-EFFECTS.md lists them).
#include <cstring>
#include <string>

#if __has_include("ReleaseBuild.h")
	#define NVR_SHELVED_OUT 1	// a release tree: the shelved effects are left out
#else
	#define NVR_SHELVED_OUT 0
#endif

namespace ShelvedEffects {

	// The shelved effects, by their effect / settings name ([Shaders.<Name>.*]).
	inline const char* const kNames[] = { "DustMotes" };
	inline constexpr bool Active = NVR_SHELVED_OUT != 0;

	// True (release builds only) for a shelved effect's name.
	inline bool Name(const char* name) {
		if (!Active || !name) return false;
		for (const char* shelved : kNames) if (!_stricmp(name, shelved)) return true;
		return false;
	}

	// True (release builds only) for a settings path under a shelved effect: "Shaders.BloodFlow", "Shaders.BloodFlow.Main", ...
	inline bool Section(const std::string& path) {
		if (!Active || path.compare(0, 8, "Shaders.") != 0) return false;
		for (const char* shelved : kNames) {
			const size_t n = strlen(shelved);
			if (path.compare(8, n, shelved) == 0 && (path.size() == 8 + n || path[8 + n] == '.')) return true;
		}
		return false;
	}

	// True (release builds only) for shelved individual keys (such as DistantLand).
	inline bool Key(const std::string& section, const std::string& key) {
		if (Section(section)) return true;
		if (!Active) return false;
		if (section == "Main.Main.Quality" && (key == "DistantLand" || key == "DistantLandSuppressVanilla")) return true;
		return false;
	}

	// Shelved SETTINGS, not effects: in a release build their switch reads as off whatever the settings file says (a read-only overlay in
	// SettingManager::GetSettingI / GetSettingF, as SettingForce.h: nothing is written, Save keeps the player's value).
	inline bool DistantLandSavedOn = false;
	// True when section / key is a shelved switch (release builds only); value is then what it reads as (0). Under the settings lock.
	inline bool ForcedOff(const char* section, const char* key, int savedValue) {
		if (!Active || !section || !key) return false;
		if (!strcmp(section, "Main.Main.Quality") && (!strcmp(key, "DistantLand") || !strcmp(key, "DistantLandSuppressVanilla"))) {
			if (savedValue) DistantLandSavedOn = true;
			return true;
		}
		return false;
	}

	// The one log line of a release build (ShaderManager::Initialize).
	inline const char* LogLine() {
		return "Dust motes (DustMotes) are not part of this release (work in progress): not loaded, whatever the settings say; their settings are kept as they are.";
	}
}
