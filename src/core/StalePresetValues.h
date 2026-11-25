#pragma once

// Old defaults that presets and other mods' scripts bring back.
//
// UNOFFICIAL 2026-11-10 (testers: "even with AlbedoBuffer on interiors are still discoloured"): the settings migration (SettingManager's
// UpdateOldDefaults) moves an old default saved in the player's own toml, but a preset or another mod's script made with an older build
// sets the old value again at every load. Seen in a tester's log: a 66-setting script batch four frames after the load with
// "[Shaders.AmbientLight.Main] AlbedoBuffer true->false", so the bounce took the guessed colour again (a room's tint applied twice).
//
// 2026-11-18 (owner: players switching from NVR keep their Cartographer / Atlas presets; "fixing these 2 points might make them work a
// lot better"): the one key became the whole list below. Cartographer and Atlas store every value they captured and replay it through
// NVRSetSetting at each cell change, and SetSetting also writes it into the player's settings file, which is already marked with this
// build's SettingsVersion: so the migration never sees those values again, and the old defaults stay for good. A preset file of our own
// PresetManager saved by an older build carries them the same way.
//
// What is on the list: every value UpdateOldDefaults moves that is a FIX (a bug guard, a correctness or speed switch, a re-scaled key,
// the anti-aliasing choice), with the old default it moves from. A preset or script that sets one of these keys to its old default is
// passed over: the setting stays as the player has it (their own file was migrated, so normally the new default). Any other value of
// the key goes through: that is a choice, not a leftover. Passing over keeps the live value, which is normally the player's own; when
// an earlier cell's preset had set a choice of its own for the same key, that one stays (rare: these keys are fixes, not looks).
// What is NOT on the list, on purpose: the LOOK a preset is made for (sky tint, exposure, LightingScale, grass brightness: steps 79 and
// 80's daylight look) and the effects it switches on or off ([Shaders.<Name>.Status] Enabled, e.g. AmbientLight), except the
// anti-aliasing effects, which belong to the player's PC rather than to a look.
// Keys that were renamed are passed on under their new name (an unknown key would be ignored).
//
// [Main.Main.Misc] PresetOldDefaults = true turns all of this off: presets and scripts set exactly what they say.
// Settings step 90 (SettingManager.cpp) uses the same table to clean a settings file that a menu Save wrote while such a batch was live.
// When UpdateOldDefaults gets a new step that moves a fix, add its key here too (tests/stale_preset_values.cpp checks the table).

#include <cmath>
#include <cstring>
#ifndef STALE_PRESET_VALUES_TABLE_ONLY
#include <string>
#include <unordered_set>
#endif

namespace StalePresetValues {

	struct OldDefault { const char* section; const char* key; float oldValue; int sinceVersion; bool playerChoice = false; };

	// Booleans are 1 / 0 (as scripts and preset files pass them). sinceVersion: the UpdateOldDefaults step that moved it. playerChoice:
	// anti-aliasing, the player's own pick for their PC: passed over in presets and scripts like the rest, but never moved by step 90.
	static const OldDefault kOldDefaults[] = {
		{ "Main.Main.ReducedQuality", "PointShadowInterval", 2.0f, 49 },
		{ "Shaders.Exposure.Main", "DarkAdaptSpeed", 50.0f, 50 }, { "Shaders.Exposure.Main", "LightAdaptSpeed", 50.0f, 50 },
		{ "Shaders.Exposure.Night", "DarkAdaptSpeed", 50.0f, 50 }, { "Shaders.Exposure.Night", "LightAdaptSpeed", 50.0f, 50 },
		{ "Shaders.Exposure.Interiors", "DarkAdaptSpeed", 50.0f, 50 }, { "Shaders.Exposure.Interiors", "LightAdaptSpeed", 50.0f, 50 },
		{ "Shaders.Skin.Scattering", "Width", 8.0f, 66 }, { "Shaders.Skin.Interiors", "Width", 8.0f, 66 },	// re-scaled: 8 then = 28.6 now
		{ "Shaders.ShadowsExteriors.Main", "OverheadSunFix", 0.0f, 68 },
		{ "Shaders.ShadowsInteriors.Forward", "EvenTaps", 0.0f, 68 },
		{ "Shaders.ShadowsInteriors.Forward", "MagnifiedSoftness", 0.0f, 68 },
		{ "Shaders.Reflections.Main", "MaterialAware", 0.0f, 68 },
		{ "Shaders.Reflections.Main", "SpecularOcclusion", 0.0f, 68 },
		{ "Shaders.PBR.Main", "LampSums", 0.0f, 69 },
		// (70's [Shaders.ShadowsExteriors.ShadowMaps] Mode is no longer a setting: the build ignores it by itself)
		{ "Shaders.PBR.Linear", "LinearScene", 0.0f, 71 },
		{ "Main.Main.Misc", "ManyLampsOnePass", 0.0f, 72 }, { "Main.Main.Misc", "LightCommonLamps", 0.0f, 72 },
		{ "Main.Main.Quality", "MaterialBuffer", 0.0f, 72 }, { "Main.Main.Quality", "AlbedoBuffer", 0.0f, 72 },
		{ "Shaders.ShadowsInteriors.Atlas", "Enabled", 0.0f, 72 }, { "Shaders.ShadowsInteriors.Atlas", "NearTiles", 0.0f, 72 },
		{ "Shaders.ShadowsInteriors.Deferred", "Enabled", 0.0f, 72 },
		{ "Main.Main.Quality", "ObjectMotion", 0.0f, 73 }, { "Main.Main.Quality", "ObjectMotionSkipCameraFollow", 0.0f, 73 },
		{ "Main.Main.Quality", "CapturedNormalsLeanHook", 0.0f, 73 }, { "Main.Main.Quality", "SharedDrawState", 0.0f, 73 },
		{ "Main.Main.Quality", "MaterialBufferSkin", 0.0f, 73 }, { "Main.Main.Performance", "SettingsCache", 0.0f, 73 },
		{ "Shaders.GodRays.Main", "SunsetGlareFix", 0.0f, 73 },
		{ "Shaders.CloudShadows.Main", "JitterFix", 0.0f, 73 }, { "Shaders.CloudShadows.Main", "SkipCascadeCheck", 0.0f, 73 },
		{ "Shaders.ShadowsInteriors.Forward", "FadeIn", 0.0f, 73 },
		{ "Shaders.ShadowsInteriors.Forward", "SteadyLampShadows", 0.0f, 74 },
		{ "Shaders.ShadowsInteriors.Deferred", "PrecisionBias", 1.0f, 75 },
		{ "Main.Main.RayTracing", "FirstPersonBody", 0.0f, 77 },
		{ "Main.Main.RayTracing", "Actors", 0.0f, 78 }, { "Main.Main.RayTracing", "AlphaTested", 0.0f, 78 },
		{ "Main.Main.RayTracing", "HitTextures", 0.0f, 78 }, { "Main.Main.RayTracing", "GlowTextures", 0.0f, 78 },
		{ "Main.Main.RayTracing", "ActorUpdateInterval", 2.0f, 78 }, { "Main.Main.RayTracing", "ActorRebuildInterval", 0.0f, 78 },
		{ "Shaders.AmbientOcclusion.RT", "GIOffscreen", 0.1f, 78 },
		{ "Shaders.AmbientLight.Main", "AlbedoBuffer", 0.0f, 78 },
		// 80, second part: anti-aliasing (the player's PC, not a look) and the less-repeating ground (a fix for visible tiling)
		{ "Shaders.TAA.Status", "Enabled", 0.0f, 80, true }, { "Shaders.TAA.FSR", "Enabled", 0.0f, 80, true },
		{ "Shaders.TAA.FSR", "Version", 0.0f, 80, true }, { "Shaders.TAA.FSR", "Sharpness", 0.0f, 80, true }, { "Shaders.TAA.FSR", "RenderScale", 1.0f, 80, true },
		{ "Shaders.Terrain.Variation", "Enabled", 0.0f, 80 },
		{ "Main.Main.Quality", "CapturedGrass", 0.0f, 81 },
		{ "Main.Main.Misc", "GameShaderPairRepair", 0.0f, 82 },
		{ "Shaders.ShadowsExteriors.RT", "Mode", 0.0f, 83 }, { "Main.Main.RayTracing", "Grass", 0.0f, 83 },
		{ "Main.Main.Misc", "FlashDetect", 1.0f, 84 }, { "Main.Main.Misc", "BeatDetect", 1.0f, 84 }, { "Main.Main.Misc", "HistoryWatch", 1.0f, 84 },
		{ "Main.Main.Quality", "DeferredRenderer", 0.0f, 85 },
		{ "Shaders.SMAA.Status", "Enabled", 1.0f, 86, true }, { "Shaders.DitherBuster.Status", "Enabled", 1.0f, 86, true },
		{ "Shaders.TAA.Cutouts", "Enabled", 0.0f, 87, true },
		{ "Main.Main.Quality", "DeferredSkinSSS", 0.0f, 88 }, { "Shaders.Skin.SSS", "Kernel", 0.0f, 88 },
		{ "Main.Main.Quality", "DeferredSkin", 0.0f, 88 }, { "Main.Main.Quality", "DeferredHair", 0.0f, 88 },
		{ "Main.Main.Quality", "DeferredTransparents", 0.0f, 89 },
		{ "Main.Main.RayTracing", "HelperWait", 0.0f, 92 },
		{ "Shaders.Reflections.RT", "RideMainExchange", 0.0f, 92 },
		{ "Shaders.Reflections.RT", "FollowRenderScale", 0.0f, 92 },
		{ "Shaders.Reflections.Main", "ReplaceForward", 0.0f, 92 },
		{ "Shaders.WetWorld.Main", "ReflectionsOwnPuddles", 0.0f, 92 },
		{ "Shaders.PBR.Main", "VanillaBridgeDielectricMax", 0.08f, 94 },
		{ "Main.Main.RayTracing", "ActorFlushPerSpace", 0.0f, 95 },
	};

	// Keys that moved (UpdateOldDefaults 61 and 67): the value goes to the new key. [Shaders.VolumetricLight.Main] NightStrength (64) has
	// no single new key (Night has its own Strength and Darken): it is passed over. key nullptr = every key of the section, kept by name:
	// 2026-11-18 New Vegas Reloaded's four sun-shadow object lists became one, [Shaders.ShadowsExteriors.Forms]; FormsNear (the cascade near
	// the camera) carries over, the other three are passed over (settings step 92 does the same for a settings file).
	struct Renamed { const char* section; const char* key; const char* newSection; const char* newKey; };
	static const Renamed kRenamed[] = {
		{ "Main.Main.ReducedQuality", "TerrainParallaxLite", "Main.Main.ReducedQuality", "ParallaxLite" },
		{ "Shaders.Cloth.Main", "Interiors", "Shaders.Cloth.Interiors", "Enabled" },
		{ "Shaders.ShadowsExteriors.FormsNear", nullptr, "Shaders.ShadowsExteriors.Forms", nullptr },
	};
	static const OldDefault kDropped[] = {
		{ "Shaders.VolumetricLight.Main", "NightStrength", 0.0f, 64 },
		{ "Shaders.ShadowsExteriors.FormsMiddle", nullptr, 0.0f, 93 }, { "Shaders.ShadowsExteriors.FormsFar", nullptr, 0.0f, 93 },
		{ "Shaders.ShadowsExteriors.FormsLod", nullptr, 0.0f, 93 },
	};

	inline bool SameNumber(float a, float b) { return std::fabs(a - b) <= 1.0e-4f * (std::fabs(b) > 1.0f ? std::fabs(b) : 1.0f); }

	enum class Verdict { Set, PassOver, Rename };
	struct Result { Verdict verdict; const char* section; const char* key; int sinceVersion; bool playerChoice = false; bool gone = false; };

	// The pure decision (no game state; tests/stale_preset_values.cpp runs it): what to do with a preset's or script's number.
	inline Result Decide(const char* section, const char* key, float value) {
		Result result = { Verdict::Set, section, key, 0 };
		if (!section || !key) return result;
		for (const OldDefault& old : kOldDefaults)
			if (!_stricmp(section, old.section) && !_stricmp(key, old.key)) {
				if (SameNumber(value, old.oldValue)) { result.verdict = Verdict::PassOver; result.sinceVersion = old.sinceVersion; result.playerChoice = old.playerChoice; }
				return result;
			}
		for (const OldDefault& old : kDropped)
			if (!_stricmp(section, old.section) && (!old.key || !_stricmp(key, old.key))) {
				result.verdict = Verdict::PassOver; result.sinceVersion = old.sinceVersion; result.gone = true;
				return result;
			}
		for (const Renamed& moved : kRenamed)
			if (!_stricmp(section, moved.section) && (!moved.key || !_stricmp(key, moved.key))) {
				result.verdict = Verdict::Rename; result.section = moved.newSection; result.key = moved.newKey ? moved.newKey : key;
				return result;
			}
		return result;
	}

#ifndef STALE_PRESET_VALUES_TABLE_ONLY
	// In the game: the decision, unless [Main.Main.Misc] PresetOldDefaults = true, and one log line per key. how: "a preset" / "a script (...)".
	inline Result Check(const char* section, const char* key, float value, const char* how) {
		static int allowed = -1;
		if (allowed < 0) {
			SettingManager::Configuration::ConfigNode node;
			allowed = TheSettingManager->Config.FillNode(&node, "Main.Main.Misc", "PresetOldDefaults") && node.BoolValue ? 1 : 0;
		}
		if (allowed) return Result{ Verdict::Set, section, key, 0 };
		const Result result = Decide(section, key, value);
		if (result.verdict == Verdict::Set) return result;

		static std::unordered_set<std::string> logged;
		const std::string name = std::string(section) + "." + key;
		if (logged.size() < 256 && logged.insert(name).second) {
			if (result.gone)
				Logger::Log("UNOFFICIAL settings: %s set [%s] %s, a setting of older builds with no single place in this one (since settings step %d): "
					"passed over (logged once per key).", how, section, key, result.sinceVersion);
			else if (result.verdict == Verdict::Rename)
				Logger::Log("UNOFFICIAL settings: %s set [%s] %s, a key of older builds: passed on as [%s] %s (logged once per key).",
					how, section, key, result.section, result.key);
			else
				Logger::Log("UNOFFICIAL settings: %s set [%s] %s = %g, the default of builds before settings step %d (%s): passed over, "
					"it stays as you have it. [Main.Main.Misc] PresetOldDefaults = true lets presets and scripts set it (logged once per key).",
					how, section, key, value, result.sinceVersion, result.playerChoice ? "anti-aliasing: your own pick" : "a fix since");
		}
		return result;
	}
#endif
}
