#pragma once

// UNOFFICIAL 2026-11-10 (testers: "even with AlbedoBuffer on interiors are still discoloured"): the settings migration (SettingManager's
// UpdateOldDefaults, version 78) moves an old default saved in the player's own toml, but a preset or another mod's script made with an
// older build sets the old value again at every load. Seen in a tester's log: a 66-setting script batch four frames after the load with
// "[Shaders.AmbientLight.Main] AlbedoBuffer true->false", so the bounce took the guessed colour again (a room's tint applied twice).
// AlbedoBuffer is a correctness switch, not a look: a preset or script that turns it off is passed over (logged once). The menu and the
// player's own toml still set it; [Main.Main.Misc] PresetOldDefaults = true lets presets and scripts set it too.
namespace StalePresetValues {
	inline bool Blocked(const char* section, const char* key, float value, const char* how) {
		if (value != 0.0f || !section || !key || _stricmp(section, "Shaders.AmbientLight.Main") || _stricmp(key, "AlbedoBuffer")) return false;
		static int allowed = -1;
		if (allowed < 0) {
			SettingManager::Configuration::ConfigNode node;
			allowed = TheSettingManager->Config.FillNode(&node, "Main.Main.Misc", "PresetOldDefaults") && node.BoolValue ? 1 : 0;
		}
		if (allowed) return false;
		static bool logged = false;
		if (!logged) {
			logged = true;
			Logger::Log("UNOFFICIAL settings: %s wants [Shaders.AmbientLight.Main] AlbedoBuffer off, the default of older builds (the bounce light then takes a guessed surface colour: tinted, "
				"over-bright rooms): passed over, it stays as you have it. [Main.Main.Misc] PresetOldDefaults = true lets presets and scripts set it (logged once).", how);
		}
		return true;
	}
}
