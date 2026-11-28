// Old-default settings migrations (SettingManager::Configuration::UpdateOldDefaults).
// Extracted from SettingManager.cpp; version constant is SettingManager::Configuration::UnofficialSettingsVersion.
#include "StalePresetValues.h"

// (UnofficialSettingsVersion is declared above Configuration::Init, which hands it to the install check)
extern bool ParallaxLiteDistanceMigrationPending;  // effects/Terrain.cpp (version 65)

void SettingManager::Configuration::UpdateOldDefaults() {
	if (!TomlConfig.is_table()) return;
	int version = 0;
	if (TomlConfig.contains("_Unofficial")) {
		const tomlValue& unofficial = TomlConfig.at("_Unofficial");
		if (unofficial.is_table() && unofficial.contains("SettingsVersion") && unofficial.at("SettingsVersion").is_integer())
			version = (int)unofficial.at("SettingsVersion").as_integer();
	}
	if (version >= UnofficialSettingsVersion) return;

	std::string changed;
	auto replace = [&](const char* section, const char* key, double oldDefault, double newDefault, bool integer) {
		tomlValue* saved = UserSection(section);
		if (!saved || !saved->contains(key)) return;
		const tomlValue& value = saved->at(key);
		const bool isOld = value.is_integer() ? (double)value.as_integer() == oldDefault :
			value.is_floating() && value.as_floating() == oldDefault;
		if (!isOld) return;
		if (integer) (*saved)[key] = (int)newDefault;
		else (*saved)[key] = newDefault;
		char entry[160];
		sprintf_s(entry, "%s%s.%s %g -> %g", changed.empty() ? "" : ", ", section, key, oldDefault, newDefault);
		changed += entry;
	};
	// P49: PointShadowInterval 2 made the shadows of people moving near lamps stutter.
	if (version < 49) replace("Main.Main.ReducedQuality", "PointShadowInterval", 2, 1, true);
	// P50: adapt speed 50 let the exposure follow every blinking sign within a few frames (flicker on the Strip).
	if (version < 50) {
		for (const char* section : { "Shaders.Exposure.Main", "Shaders.Exposure.Night", "Shaders.Exposure.Interiors" }) {
			replace(section, "DarkAdaptSpeed", 50.0, 0.2, false);
			replace(section, "LightAdaptSpeed", 50.0, 0.2, false);
		}
	}
	// P61: P60's TerrainParallaxLite became ParallaxLite (terrain and parallax objects); a saved value carries over.
	if (version < 61) {
		tomlValue* saved = UserSection("Main.Main.ReducedQuality");
		if (saved && saved->is_table() && saved->contains("TerrainParallaxLite")) {
			const tomlValue old = saved->at("TerrainParallaxLite");
			saved->as_table().erase("TerrainParallaxLite");
			if (!saved->contains("ParallaxLite") && old.is_boolean()) {
				(*saved)["ParallaxLite"] = old.as_boolean();
				Logger::Log("UNOFFICIAL settings: TerrainParallaxLite = %s carried over to ParallaxLite (it now covers "
					"parallax objects too).", old.as_boolean() ? "true" : "false");
			}
		}
	}
	// P63: Sharpening and the flashlight's look got separate interior settings. Someone who tuned the old single set
	// keeps it indoors too: their saved values are copied once into the new Interiors section.
	if (version < 63) {
		auto copyToInteriors = [&](const char* parent, const char* from, std::initializer_list<const char*> keys) {
			std::string fromPath = std::string(parent) + "." + from;
			tomlValue* saved = UserSection(fromPath.c_str());
			tomlValue* parentTable = UserSection(parent);
			if (!saved || !saved->is_table() || !parentTable || !parentTable->is_table() || parentTable->contains("Interiors")) return;
			tomlValue::table_type interiors;
			for (const char* key : keys)
				if (saved->contains(key)) interiors[key] = saved->at(key);
			if (interiors.empty()) return;
			(*parentTable)["Interiors"] = interiors;
			Logger::Log("UNOFFICIAL settings: %s now has separate interior settings; your saved %s values were copied to %s.Interiors.",
				parent, fromPath.c_str(), parent);
		};
		copyToInteriors("Shaders.Sharpening", "Main", { "Strength", "Clamp", "Offset" });
		// (2026-11-04: the flashlight's copy is gone with its Interiors section)
	}
	// 64: volumetric light got its own night look ([Shaders.VolumetricLight.Night]) in place of Main's NightStrength,
	// which scaled the strength and the darkening by moonlight. A saved Main carries over to Night as it looked.
	if (version < 64) {
		tomlValue* saved = UserSection("Shaders.VolumetricLight.Main");
		tomlValue* parentTable = UserSection("Shaders.VolumetricLight");
		if (saved && saved->is_table() && parentTable && parentTable->is_table() && !parentTable->contains("Night")) {
			auto number = [&](const char* key, double fallback) {
				if (!saved->contains(key)) return fallback;
				const tomlValue& value = saved->at(key);
				return value.is_integer() ? (double)value.as_integer() : value.is_floating() ? value.as_floating() : fallback;
			};
			const double nightStrength = number("NightStrength", 0.3);
			tomlValue::table_type night;
			night["Strength"] = number("Strength", 0.6) * nightStrength;
			night["Darken"] = number("Darken", 0.4) * nightStrength;
			for (const char* key : { "Density", "MaxDistance", "Anisotropy" })
				if (saved->contains(key)) night[key] = number(key, 0.0);
			(*parentTable)["Night"] = night;
			Logger::Log("UNOFFICIAL settings: volumetric light now has separate night settings; your saved values (NightStrength %g) "
				"were copied to Shaders.VolumetricLight.Night.", nightStrength);
		}
		if (saved && saved->is_table() && saved->contains("NightStrength")) saved->as_table().erase("NightStrength");
	}
	// 65: ParallaxLite no longer forces its shorter terrain parallax reach every frame; it writes it into
	// Shaders.Terrain.Parallax MaxDistance once, when switched on (TerrainShaders::UpdateSettings, which knows the screen
	// height). A config that already has it on gets that once too, so its look stays as it was until the player changes
	// MaxDistance.
	if (version < 65) {
		tomlValue* saved = UserSection("Main.Main.ReducedQuality");
		if (saved && saved->is_table() && saved->contains("ParallaxLite") && saved->at("ParallaxLite").is_boolean() &&
			saved->at("ParallaxLite").as_boolean())
			ParallaxLiteDistanceMigrationPending = true;
	}
	// 66 (2026-10-19): NewSkin's screen-space blur re-synced with Conor's version: Community Shaders' 21-sample kernel over
	// +-3 and its width, Width 28.6 (= CS Blur Radius 1.0). The old 8 was sized for the 17-sample kernel over +-2.
	if (version < 66) {
		replace("Shaders.Skin.Scattering", "Width", 8.0, 28.6, false);
		replace("Shaders.Skin.Interiors", "Width", 8.0, 28.6, false);
	}
	// 67 (2026-10-21): cloth got its own indoor settings, [Shaders.Cloth.Interiors] (testers: still cloth indoors moved far too
	// much with the outdoor values). [Shaders.Cloth.Main] Interiors became its Enabled: a saved 'true' carries over (cloth keeps
	// moving indoors, now with the gentle indoor values); the old key goes.
	if (version < 67) {
		tomlValue* saved = UserSection("Shaders.Cloth.Main");
		if (saved && saved->is_table() && saved->contains("Interiors")) {
			const tomlValue old = saved->at("Interiors");
			saved->as_table().erase("Interiors");
			tomlValue* parentTable = UserSection("Shaders.Cloth");
			if (old.is_boolean() && old.as_boolean() && parentTable && parentTable->is_table()) {
				if (!parentTable->contains("Interiors") || !parentTable->at("Interiors").is_table()) (*parentTable)["Interiors"] = toml::table();
				(*parentTable)["Interiors"]["Enabled"] = true;
				Logger::Log("UNOFFICIAL settings: cloth now has separate indoor settings; your Shaders.Cloth.Main Interiors = true "
					"was carried over to Shaders.Cloth.Interiors Enabled (indoor cloth now moves with the gentler indoor values).");
			}
		}
	}
	// 68 (2026-10-22): AmbientLight on by default with its fixes (NoEdgeOutline, NormalSmoothing), the shadow fixes on (OverheadSunFix:
	// midday sun shadows; interior lamp shadows' EvenTaps + MagnifiedSoftness 2) and the reflections' material settings on
	// (MaterialAware 1, SpecularOcclusion 1, RoughnessOverrides); Reflections itself stays off.
	if (version < 68) {
		auto replaceBool = [&](const char* section, const char* key, bool oldDefault, bool newDefault) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean() != oldDefault) return;
			(*saved)[key] = newDefault;
			char entry[160];
			sprintf_s(entry, "%s%s.%s %s -> %s", changed.empty() ? "" : ", ", section, key, oldDefault ? "true" : "false", newDefault ? "true" : "false");
			changed += entry;
		};
		replaceBool("Shaders.AmbientLight.Status", "Enabled", false, true);
		replaceBool("Shaders.ShadowsExteriors.Main", "OverheadSunFix", false, true);
		replaceBool("Shaders.ShadowsInteriors.Forward", "EvenTaps", false, true);
		replace("Shaders.ShadowsInteriors.Forward", "MagnifiedSoftness", 0.0, 2.0, false);
		replace("Shaders.Reflections.Main", "MaterialAware", 0, 1, true);
		replace("Shaders.Reflections.Main", "SpecularOcclusion", 0.0, 1.0, false);
		tomlValue* saved = UserSection("Shaders.Reflections.Main");
		if (saved && saved->contains("RoughnessOverrides") && saved->at("RoughnessOverrides").is_string() &&
			std::string(saved->at("RoughnessOverrides").as_string()).empty()) {
			(*saved)["RoughnessOverrides"] = "nv_ultralux_centertiles:0.1, ul_marbleblack:0.12";
			changed += changed.empty() ? "Shaders.Reflections.Main.RoughnessOverrides" : ", Shaders.Reflections.Main.RoughnessOverrides";
		}
	}
	// 69 (2026-10-25): LampSums on by default (owner: the hard steps in indoor lamp light at tile and object edges; exact lamp sums,
	// no whitening, about 0.3-0.4 ms in lamp-rich rooms). The other PBR fixes of that day are new settings (default on by themselves).
	if (version < 69) {
		tomlValue* saved = UserSection("Shaders.PBR.Main");
		if (saved && saved->contains("LampSums") && saved->at("LampSums").is_boolean() && !saved->at("LampSums").as_boolean()) {
			(*saved)["LampSums"] = true;
			changed += changed.empty() ? "Shaders.PBR.Main.LampSums false -> true" : ", Shaders.PBR.Main.LampSums false -> true";
		}
	}
	// 70 (2026-10-27): the sun shadow maps' Mode 2 (EVSM4, the old default) -> 3 (PCF, the default since 2026-10-14). A tester's config
	// (Doc, saved by an earlier build) still held 2: every character-shadow fix since (the smooth lookup normals' offset, the receiver-plane
	// slack, CharacterShadowReach's cap) is compiled for PCF only, and EVSM has no normal offset at all, so faces and clothing shaded their
	// own triangles: the facets that tester still saw with MSAA off. A saved 2 is the old default, moved once like the others.
	if (version < 70) replace("Shaders.ShadowsExteriors.ShadowMaps", "Mode", 2, 3, true);
	// P84 (2026-10-28, user: LinearScene "seems to have completely resolved the pop in issue. that should be the default"): a saved
	// LinearScene = false (the old default) goes on.
	if (version < 71) {
		tomlValue* saved = UserSection("Shaders.PBR.Linear");
		if (saved && saved->contains("LinearScene") && saved->at("LinearScene").is_boolean() && !saved->at("LinearScene").as_boolean()) {
			(*saved)["LinearScene"] = true;
			changed += changed.empty() ? "Shaders.PBR.Linear.LinearScene false -> true" : ", Shaders.PBR.Linear.LinearScene false -> true";
		}
	}
	// P85 (2026-10-31, user: the deferred lamps stack "as a default"): ManyLampsOnePass, LightCommonLamps, MaterialBuffer, AlbedoBuffer, the shadow
	// atlas with NearTiles 8, DeferredLamps: a saved false / 0 (the old default) goes on. They run on DXVK only (native D3D9: as before).
	if (version < 72) {
		auto toTrue = [&](const char* section, const char* key) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean()) return;
			(*saved)[key] = true;
			changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " false -> true";
		};
		toTrue("Main.Main.Misc", "ManyLampsOnePass");
		toTrue("Main.Main.Misc", "LightCommonLamps");
		toTrue("Main.Main.Quality", "MaterialBuffer");
		toTrue("Main.Main.Quality", "AlbedoBuffer");
		toTrue("Shaders.ShadowsInteriors.Atlas", "Enabled");
		toTrue("Shaders.ShadowsInteriors.Deferred", "Enabled");
		replace("Shaders.ShadowsInteriors.Atlas", "NearTiles", 0, 8, true);
	}
	// P85 v2 (2026-11-01, user: "are our defaults just not getting applied"; a tester's DLSS ghosting with ObjectMotion still off): switches that
	// went on by default after a test build had saved them off. A saved false from those builds kept them off (only a migration moves a saved
	// value). Once: a saved false goes on, a saved FadeIn 0 goes to 0.25.
	if (version < 73) {
		auto toTrue = [&](const char* section, const char* key) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean()) return;
			(*saved)[key] = true;
			changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " false -> true";
		};
		toTrue("Main.Main.Quality", "ObjectMotion");
		toTrue("Main.Main.Quality", "ObjectMotionSkipCameraFollow");
		toTrue("Main.Main.Quality", "CapturedNormalsLeanHook");
		toTrue("Main.Main.Quality", "SharedDrawState");
		toTrue("Main.Main.Quality", "MaterialBufferSkin");
		toTrue("Main.Main.Performance", "SettingsCache");
		toTrue("Shaders.GodRays.Main", "SunsetGlareFix");
		toTrue("Shaders.CloudShadows.Main", "JitterFix");
		toTrue("Shaders.CloudShadows.Main", "SkipCascadeCheck");
		replace("Shaders.ShadowsInteriors.Forward", "FadeIn", 0.0, 0.25, false);
	}
	// 2026-11-01 flicker handovers (docs/next/FLICKER-PROGRAM.md part 3): SteadyLampShadows on by default; a saved false (the old default)
	// goes on once. (FadeOut and the deferred StableOrder are new keys: the defaults apply.)
	if (version < 74) {
		tomlValue* saved = UserSection("Shaders.ShadowsInteriors.Forward");
		if (saved && saved->contains("SteadyLampShadows") && saved->at("SteadyLampShadows").is_boolean() && !saved->at("SteadyLampShadows").as_boolean()) {
			(*saved)["SteadyLampShadows"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsInteriors.Forward.SteadyLampShadows false -> true";
		}
	}
	// 2026-11-01 flicker bench: deferred lamp shadows flickered with TAA's jitter where lamps sit on a surface; PrecisionBias 1 -> 4 (a saved 1, the old default, moves once).
	if (version < 75) replace("Shaders.ShadowsInteriors.Deferred", "PrecisionBias", 1.0, 4.0, false);
	// 2026-11-07 (test 5 testers): FirstPersonBody off by default (its shadow is the third-person pose: a "reverse shadow" cut-out on the floor;
	// the path tracer drew the inside of the head over the sky). A saved true, the old default, goes off once.
	if (version < 76) {
		tomlValue* saved = UserSection("Main.Main.RayTracing");
		if (saved && saved->contains("FirstPersonBody") && saved->at("FirstPersonBody").is_boolean() && saved->at("FirstPersonBody").as_boolean()) {
			(*saved)["FirstPersonBody"] = false;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.RayTracing.FirstPersonBody true -> false";
		}
	}
	// 2026-11-07 (ptaaa-firstperson, RT-PATH-TRACING.md "ptaaa-firstperson"): FirstPersonBody on again: the camera rays pass over it, and every
	// ray that starts or ends within arm's reach of the eye passes over it too (Compute/Rt/RtHidden.glsl: the reverse shadow, the gun shadowed
	// by the head). A saved false (version 76's, or the default of the hours between) goes on once.
	if (version < 77) {
		tomlValue* saved = UserSection("Main.Main.RayTracing");
		if (saved && saved->contains("FirstPersonBody") && saved->at("FirstPersonBody").is_boolean() && !saved->at("FirstPersonBody").as_boolean()) {
			(*saved)["FirstPersonBody"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.RayTracing.FirstPersonBody false -> true";
		}
	}
	// 78 (2026-11-09, user: "make any obvious default RT settings default by default so this doesn't happen again"; a tester's "deep fried" picture): the
	// menu's Save writes every setting, and RT defaults replaced since 1.0.5 had no migration, so a saved config (the tester's: version 75) still held
	// Actors / AlphaTested / HitTextures / GlowTextures false (the RT scene without people, cut-outs, the real hit textures and the glow maps: the bounce
	// took each mesh's average colour), GIOffscreen 0.1 (1 since 1.0.4: 0.1 made most of the bounce 10 times too dark), ActorUpdateInterval 2 /
	// ActorRebuildInterval 0 (1 / 8 since test 4) and AmbientLight AlbedoBuffer false (the bounce took a colour guessed from the lit picture: a
	// room's tint applied twice). A saved old default goes to the new one once; anything else the player chose stays.
	if (version < 78) {
		auto toTrue = [&](const char* section, const char* key) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean()) return;
			(*saved)[key] = true;
			changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " false -> true";
		};
		toTrue("Main.Main.RayTracing", "Actors");
		toTrue("Main.Main.RayTracing", "AlphaTested");
		toTrue("Main.Main.RayTracing", "HitTextures");
		toTrue("Main.Main.RayTracing", "GlowTextures");
		replace("Main.Main.RayTracing", "ActorUpdateInterval", 2, 1, true);
		replace("Main.Main.RayTracing", "ActorRebuildInterval", 0, 8, true);
		replace("Shaders.AmbientOcclusion.RT", "GIOffscreen", 0.1, 1.0, false);
		toTrue("Shaders.AmbientLight.Main", "AlbedoBuffer");
	}
	// 79 (2026-11-12, a rain run on an RTX card: the land's mean brightness 94.5 dry, 121.7 in rain): rain lit the land and the objects BRIGHTER than
	// dry weather. On 2026-09-19 the dry groups' LightingScale went from 1.0 to 0.725 ([Shaders.Terrain.Main], [Shaders.PBR.Main]) and their Rain
	// siblings stayed at 1.0. The Rain groups' default is 0.725 now, the same as dry. A saved 1.0 (the old default) moves once; it stays where the
	// player's own dry value is 1.0 or more (rain is then not brighter than dry, and 1.0 may be their choice).
	if (version < 79) {
		for (const char* family : { "Shaders.Terrain", "Shaders.PBR" }) {
			const std::string dry = std::string(family) + ".Main", rain = std::string(family) + ".Rain";
			const tomlValue* saved = UserSection(dry.c_str());
			double dryScale = 0.725;	// (no saved value: the default)
			if (saved && saved->contains("LightingScale")) {
				const tomlValue& value = saved->at("LightingScale");
				if (value.is_floating()) dryScale = value.as_floating();
				else if (value.is_integer()) dryScale = (double)value.as_integer();
			}
			if (dryScale < 1.0) replace(rain.c_str(), "LightingScale", 1.0, 0.725, false);
		}
	}
	// 80 (2026-11-12, the owner: "make a nice preset ... and ship that to users as the real default and they can go from there"): the daylight
	// look tuned on the bench against photos of the real Mojave (a blue sky in place of the lavender one, sunlit ground about 1.7x as bright and
	// warmer; night and interiors have their own groups and do not change). A look is a choice, so a saved configuration moves ONLY when every
	// one of its values is still the old default (or was never saved): one changed value means the player tuned the look, and nothing moves.
	if (version < 80) {
		struct LookKey { const char* section; const char* key; double oldDefault, newDefault; };
		static const LookKey look[] = {
			{ "Shaders.Sky.Main", "TintDayR", 1.0, 0.55 }, { "Shaders.Sky.Main", "TintDayG", 1.0, 0.78 }, { "Shaders.Sky.Main", "TintDayB", 1.0, 1.0 },
			{ "Shaders.Sky.Main", "SaturationDay", 1.0, 1.6 },
			{ "Shaders.Tonemapping.Main", "Exposure", 1.5, 2.5 }, { "Shaders.Tonemapping.Main", "SkyMultiplier", 1.0, 0.6 },
			{ "Shaders.PBR.Main", "LightingScale", 0.725, 1.15 }, { "Shaders.Terrain.Main", "LightingScale", 0.725, 1.15 },
			{ "Shaders.PBR.Rain", "LightingScale", 0.725, 1.15 }, { "Shaders.Terrain.Rain", "LightingScale", 0.725, 1.15 }, { "Shaders.Grass.Main", "Brightness", 1.0, 1.3 },
		};
		const LookKey* touched = nullptr;
		bool anySaved = false;
		for (const LookKey& k : look) {
			const tomlValue* saved = UserSection(k.section);
			if (!saved || !saved->contains(k.key)) continue;	// never saved: the new default applies by itself
			anySaved = true;
			const tomlValue& value = saved->at(k.key);
			const double v = value.is_integer() ? (double)value.as_integer() : value.is_floating() ? value.as_floating() : -1.0e9;
			if (v != k.oldDefault) { touched = &k; break; }
		}
		if (touched) {
			Logger::Log("UNOFFICIAL settings: this build's new daylight look was NOT applied, because your saved %s.%s is your own value. To try it: "
				"[Shaders.Sky.Main] TintDay 0.55 0.78 1.0, SaturationDay 1.6; [Shaders.Tonemapping.Main] Exposure 2.5, SkyMultiplier 0.6; LightingScale 1.15 in "
				"[Shaders.PBR.Main], [Shaders.Terrain.Main] and their Rain groups; [Shaders.Grass.Main] Brightness 1.3.", touched->section, touched->key);
			// 2026-11-18 (old NVR presets on VS, bench: memory vs-old-preset-look-caffeine): a look of the player's own keeps its OLD values for
			// the look keys it never saved. A preset or settings file made for New Vegas Reloaded has no [Shaders.Sky.Main] TintDay* /
			// SaturationDay at all (they came later), so the new look's blue sky tint and saturation 1.6 filled them in and Caffeine's warm
			// sky came out blue. Only reached by files from before this step (a menu Save writes every key, so a newer file has none unsaved).
			std::string kept;
			for (const LookKey& k : look) {
				StringList steps;
				StorageKeys(k.section, &steps);
				tomlValue* table = &TomlConfig;
				for (const std::string& step : steps) {
					if (!table->contains(step) || !table->at(step).is_table()) (*table)[step] = toml::table();
					table = &table->at(step);
				}
				if (table->contains(k.key)) continue;
				(*table)[k.key] = k.oldDefault;
				kept += std::string(kept.empty() ? "" : ", ") + k.section + "." + k.key;
			}
			if (!kept.empty())
				Logger::Log("UNOFFICIAL settings: your look's settings that your file did not have keep the old look's values (not the new look's): %s.",
					kept.c_str());
		}
		else if (anySaved) {
			for (const LookKey& k : look) if (k.oldDefault != k.newDefault) replace(k.section, k.key, k.oldDefault, k.newDefault, false);
			Logger::Log("UNOFFICIAL settings: the new daylight look was applied (none of its values had been changed). The old look: [Shaders.Sky.Main] TintDay "
				"1 1 1, SaturationDay 1.0; [Shaders.Tonemapping.Main] Exposure 1.5, SkyMultiplier 1.0; LightingScale 0.725 in [Shaders.PBR.Main], "
				"[Shaders.Terrain.Main] and their Rain groups; [Shaders.Grass.Main] Brightness 1.0.");
		}
	}
	// 80, second part (2026-11-12, the owner: "have the default ... use taa fsr 3.1 native render scale with sharpening. use less repeating
	// ground too"). Anti-aliasing is a choice too: the saved file moves only when all five of its keys are still the old defaults (TAA
	// off, FSR off, FSR version 0, FSR sharpness 0, RenderScale 1): someone who switched TAA on, or FSR on with TAA, keeps exactly what they chose.
	// RenderScale goes to 0.67 with it (FSR 3.1 Quality). Less-repeating ground is a plain switch: a saved false moves to true.
	if (version < 80) {
		auto savedIs = [&](const char* section, const char* key, bool wantBool, bool boolValue, double number) {
			const tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key)) return true;	// never saved: the new default applies by itself
			const tomlValue& value = saved->at(key);
			if (wantBool) return value.is_boolean() && value.as_boolean() == boolValue;
			return (value.is_integer() && (double)value.as_integer() == number) || (value.is_floating() && value.as_floating() == number);
		};
		auto setBool = [&](const char* section, const char* key) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean()) return;
			(*saved)[key] = true;
			changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " false -> true";
		};
		if (savedIs("Shaders.TAA.Status", "Enabled", true, false, 0) && savedIs("Shaders.TAA.FSR", "Enabled", true, false, 0) &&
			savedIs("Shaders.TAA.FSR", "Version", false, false, 0.0) && savedIs("Shaders.TAA.FSR", "Sharpness", false, false, 0.0) &&
			savedIs("Shaders.TAA.FSR", "RenderScale", false, false, 1.0)) {
			setBool("Shaders.TAA.Status", "Enabled");
			setBool("Shaders.TAA.FSR", "Enabled");
			replace("Shaders.TAA.FSR", "Version", 0, 1, true);
			replace("Shaders.TAA.FSR", "Sharpness", 0.0, 0.4, false);
			replace("Shaders.TAA.FSR", "RenderScale", 1.0, 0.67, false);	// FSR's Quality mode (native cost 5.6 ms on a GTX 1070 at 1080p)
		}
		setBool("Shaders.Terrain.Variation", "Enabled");
	}
	// 81 (2026-11-13): [Main.Main.Quality] CapturedGrass false -> true. The key was added on 2026-11-12 and switched off the same day (bright
	// speckles on an RTX card, since fixed: the grass's own normal, shade and vertex colour are recorded now), so every file saved by 1.0.8
	// holds false. Without its records, grass under path tracing took the land's colour or the game's own pixel by view angle (a tester:
	// "grass disappears / reappears depending on viewing angle"). It only does anything while path tracing or ray traced bounce light runs.
	if (version < 81) {
		tomlValue* saved = UserSection("Main.Main.Quality");
		if (saved && saved->contains("CapturedGrass") && saved->at("CapturedGrass").is_boolean() && !saved->at("CapturedGrass").as_boolean()) {
			(*saved)["CapturedGrass"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Quality.CapturedGrass false -> true";
		}
	}
	// UNOFFICIAL 2026-11-13 b (82): [Main.Main.Misc] GameShaderPairRepair false -> true. The key is one day old and was never released with
	// false, so only files saved by the development builds of 2026-11-13 hold it.
	if (version < 82) {
		tomlValue* saved = UserSection("Main.Main.Misc");
		if (saved && saved->contains("GameShaderPairRepair") && saved->at("GameShaderPairRepair").is_boolean() && !saved->at("GameShaderPairRepair").as_boolean()) {
			(*saved)["GameShaderPairRepair"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Misc.GameShaderPairRepair false -> true";
		}
	}
	// UNOFFICIAL 2026-11-13 c (83): [Shaders.ShadowsExteriors.RT] Mode 0 -> 1 and [Main.Main.RayTracing] Grass false -> true. The owner's
	// rule: an enabled ray traced effect replaces its raster twin where the trace has an answer. Mode 1 was wrong before this build (the
	// deferred apply could not stand in for the shaders' own lookup: people in shade came out sunlit); ForwardLookup fixes it. Without
	// grass in the traced scene the grass casts no sun shadow inside the RT range, so the two move together. The menu's Save writes every
	// key, so every saved file holds the old values. Neither does anything unless a ray traced effect is on.
	if (version < 83) {
		tomlValue* saved = UserSection("Shaders.ShadowsExteriors.RT");
		if (saved && saved->contains("Mode") && saved->at("Mode").is_integer() && saved->at("Mode").as_integer() == 0) {
			(*saved)["Mode"] = 1;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsExteriors.RT.Mode 0 -> 1";
		}
		saved = UserSection("Main.Main.RayTracing");
		if (saved && saved->contains("Grass") && saved->at("Grass").is_boolean() && !saved->at("Grass").as_boolean()) {
			(*saved)["Grass"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.RayTracing.Grass false -> true";
		}
	}
	// 84 (2026-11-14): the flash detector, beat detector and history watch are off by default (their per-frame read-backs measured
	// 4-7 ms a frame on native D3D9 (a CPU wait in the HUD step) and 0.3-0.6 ms on DXVK at 1440p, GTX 1070, every other setting the same). Menu saves hold the old true.
	if (version < 84) {
		tomlValue* saved = UserSection("Main.Main.Misc");
		for (const char* key : { "FlashDetect", "BeatDetect", "HistoryWatch" }) {
			if (saved && saved->contains(key) && saved->at(key).is_boolean() && saved->at(key).as_boolean()) {
				(*saved)[key] = false;
				changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Misc." + key + " true -> false";
			}
		}
	}
	// 85 (2026-11-15, owner decision, docs/next/FULL-DEFERRED-RENDERER-PLAN.md section 7): the deferred renderer ships on by default,
	// [Main.Main.Quality] DeferredRenderer 0 -> 2. The key never shipped in a public release (added 2026-11-14, default 0), so only files saved
	// by the development and playtest builds hold the old 0. A saved 1 or 2 is a choice and stays.
	if (version < 85) replace("Main.Main.Quality", "DeferredRenderer", 0, 2, true);
	// 86 (2026-11-16, owner decision): SMAA and DitherBuster are off by default ([Shaders.SMAA.Status] / [Shaders.DitherBuster.Status]
	// Enabled true -> false). A menu save holds the old true, which moves to false once; a saved false stays.
	if (version < 86) {
		for (const char* section : { "Shaders.SMAA.Status", "Shaders.DitherBuster.Status" }) {
			tomlValue* saved = UserSection(section);
			if (saved && saved->contains("Enabled") && saved->at("Enabled").is_boolean() && saved->at("Enabled").as_boolean()) {
				(*saved)["Enabled"] = false;
				changed += std::string(changed.empty() ? "" : ", ") + section + ".Enabled true -> false";
			}
		}
	}
	// 87 (2026-11-16, owner decision: "smooth cutouts should be part of the default if we're going to ship it with taa on"): [Shaders.TAA.Cutouts]
	// Enabled false -> true (Mode 2, like the game's MSAA). A menu save holds the old false, which moves to true once. It is a restart key, read
	// when the shaders are first compiled, which is after this step: it applies from this start. It does nothing without TAA / FSR / DLSS.
	if (version < 87) {
		tomlValue* saved = UserSection("Shaders.TAA.Cutouts");
		if (saved && saved->contains("Enabled") && saved->at("Enabled").is_boolean() && !saved->at("Enabled").as_boolean()) {
			(*saved)["Enabled"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.TAA.Cutouts.Enabled false -> true (Smooth Cutouts; a restart key: compiled in from this start)";
		}
	}
	// 88 (2026-11-16, owner decision after the fd-fwdgroups pictures, docs/next/FD-FORWARD-GROUPS.md section 6): skin and hair join the
	// deferred renderer by default (glass too at first, held back on 2026-11-17, on again in 89: below): [Main.Main.Quality] DeferredSkin false -> true,
	// DeferredSkinSSS 0 -> 2 (with [Shaders.Skin.SSS] Kernel 1, the separable kernel) and DeferredHair false -> true. The keys never shipped in
	// a public release (added 2026-11-15, test builds only); a menu save holds the old values, which move once. A saved non-default value stays.
	// 2026-11-17 (owner: "glass off for now", claude/test-1117): [Main.Main.Quality] DeferredTransparents 1 turned pale glass bottles silver
	// (docs/next/BOTTLE-GLOW.md), so its default went back to 0 and 88 no longer moves a saved 0 to 1. Editing 88 itself was safe then: no
	// settings file outside the bench had been saved at 88. Glass is turned on again by migration 89 (below), not here: test-1117's first zip
	// (rev1) ran this 88, so a file it saved is at 88 with DeferredTransparents 0, and only a newer step reaches it.
	if (version < 88) {
		replace("Main.Main.Quality", "DeferredSkinSSS", 0, 2, true);
		replace("Shaders.Skin.SSS", "Kernel", 0, 1, true);	// the old default 0 (Burley) leaves a dark patch under a moustache
		tomlValue* saved = UserSection("Main.Main.Quality");
		for (const char* key : { "DeferredSkin", "DeferredHair" }) {
			if (saved && saved->contains(key) && saved->at(key).is_boolean() && !saved->at(key).as_boolean()) {
				(*saved)[key] = true;
				changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Quality." + key + " false -> true";
			}
		}
	}
	// 89 (2026-11-17, owner: "Turn glass on", claude/test-1117 rev2): [Main.Main.Quality] DeferredTransparents 0 -> 1, glass under the deferred
	// renderer on by default again. The silver bottles that held it back came from the forward+ lamp walk (Includes/ForwardPlus.hlsl FPLampSum):
	// D3DX compiled its `if (k >= count) break` as a break that never fires in 20 records, so the walk ran past the froxel's lamp list and added
	// lamps several times over; the list length is now the loop's own bound (claude/fd-glassglow 0e065a3a + 3479ea34, here 9046dea41 + debca731f;
	// docs/next/FD-GLASSGLOW.md). A menu save holds the old 0 (the default from 2026-11-15 and again from 11-17 until this step), which moves to 1
	// once, whether the file was saved at 85 (before 88) or at 88 (test-1117 rev1). A saved 1 stays. It is a restart key read when the shaders
	// are first compiled, which is after this step: the glass records are compiled in from this start.
	if (version < 89) {
		tomlValue* saved = UserSection("Main.Main.Quality");
		if (saved && saved->contains("DeferredTransparents")) {
			const tomlValue& value = saved->at("DeferredTransparents");
			if ((value.is_integer() && value.as_integer() == 0) || (value.is_floating() && value.as_floating() == 0.0)) {
				(*saved)["DeferredTransparents"] = 1;
				changed += std::string(changed.empty() ? "" : ", ") +
					"Main.Main.Quality.DeferredTransparents 0 -> 1 (glass under the new renderer, settings step 89: the silver-bottle fix is in; a restart key: compiled in from this start)";
			}
		}
	}
	// 2026-11-22 upstream 1.1.3 merge (branch upstream-rt-merge): steps 90-94 below are upstream's (1.1.1-1.1.3); this fork's own 90-92 (CAS,
	// WetWorld removed, Gamma22) run after them as 95-97. A file saved by this fork's builds at 91 or 92 already ran ours (it has no
	// [Shaders.WetWorld] left: step 91 of ours removed it, and a menu save of upstream's builds always writes it), so ours count from its
	// own number there; any other file below 95 (upstream's 90-94, or ours at 90) runs all three. Upstream's 91, 92 and 94 change defaults
	// this fork keeps as they were until the owner decides (each one switch below).
	constexpr bool kParkUpstream91 = true, kParkUpstream92 = true, kParkUpstream94 = true;
	// (2026-11-26, upstream 1.1.4 merge) upstream's builds now count to 96: a file at 95 or 96 WITH [Shaders.WetWorld] is upstream's (it ran
	// upstream's 95 / 96, now our 99 / 100, and none of ours); one without it is ours (our 95-98).
	// (2026-11-27 upstream 1.1.5 merge) upstream now counts to 101: its 97-101 run here as 104-108, each skipped for an upstream-built file that already ran it.
	const bool upstreamFile = UserSection("Shaders.WetWorld") != nullptr;
	const bool ourHistory = (version == 91 || version == 92) && !upstreamFile;
	const int ours = version >= 95 && !upstreamFile ? version - 5 : ourHistory ? version : 89;
	constexpr int kUpstreamNewest = 103;	// the newest settings number upstream's builds write (1.1.6-test2): bump it on the next upstream merge
	auto upstreamRan = [&](int step) { return upstreamFile && version >= step && version <= kUpstreamNewest; };
	// 90 (2026-11-18, owner: players switching from NVR keep their Cartographer / Atlas presets): those mods replay every value they
	// captured through NVRSetSetting, which went into this table, and the menu's Save wrote it to the file after the steps above had run,
	// so the old defaults of the fixes stayed. Since this build a preset or script can no longer set them (StalePresetValues.h); this
	// step cleans a file saved before. A captured batch leaves many of them at once; one or two may be the player's own pick (a fix turned
	// off on purpose), so they move only when 5 or more are found, all together, to this build's default. Anti-aliasing never moves here.
	if (version < 90 || ourHistory) {
		struct Hit { tomlValue* saved; const StalePresetValues::OldDefault* old; const tomlValue* fresh; };
		std::vector<Hit> hits;
		for (const StalePresetValues::OldDefault& old : StalePresetValues::kOldDefaults) {
			if (old.playerChoice || old.sinceVersion >= 90) continue;	// (2026-11-26: keys of later steps are moved by those steps)
			tomlValue* saved = UserSection(old.section);
			if (!saved || !saved->contains(old.key)) continue;
			const tomlValue& value = saved->at(old.key);
			const double number = value.is_boolean() ? (value.as_boolean() ? 1.0 : 0.0) : value.is_integer() ? (double)value.as_integer() :
				value.is_floating() ? value.as_floating() : -1.0e9;
			if (!StalePresetValues::SameNumber((float)number, old.oldValue)) continue;
			StringList steps;
			StorageKeys(old.section, &steps);
			const tomlValue* defaults = FindSection(&DefaultConfig, &steps);
			if (!defaults || !defaults->is_table() || !defaults->contains(old.key)) continue;
			hits.push_back({ saved, &old, &defaults->at(old.key) });
		}
		if (hits.size() >= 5) {
			for (const Hit& hit : hits) {
				const tomlValue& fresh = *hit.fresh;
				if (fresh.is_boolean()) (*hit.saved)[hit.old->key] = fresh.as_boolean();
				else if (fresh.is_integer()) (*hit.saved)[hit.old->key] = fresh.as_integer();
				else if (fresh.is_floating()) (*hit.saved)[hit.old->key] = fresh.as_floating();
				else continue;
				changed += std::string(changed.empty() ? "" : ", ") + hit.old->section + "." + hit.old->key + " (old default left by a preset or script)";
			}
		}
		else if (!hits.empty())
			Logger::Log("UNOFFICIAL settings: %u setting(s) in your file still hold the default of an older build (first: [%s] %s); fewer than 5, so "
				"taken as your own choice and left as they are.", (unsigned)hits.size(), hits[0].old->section, hits[0].old->key);
	}
	// 91 (2026-11-18, owner: "maps decide"): [Shaders.PBR.Rmaos] MetalFromMap false -> true. A hand-made _rmaos map's metal (its green) was
	// ignored since 2026-10-28 and a colour guess used instead, which turned first-person guns with a map into one uniform metal sheen
	// (effects/PBR/defaults.toml). A menu save holds the old false, which moves once; a false saved after this step stays.
	// (2026-11-22 merge: PARKED, owner's question. This fork keeps MetalFromMap false by default, so the step is off: kParkUpstream91.)
	if (version < 91 && !kParkUpstream91) {
		tomlValue* saved = UserSection("Shaders.PBR.Rmaos");
		if (saved && saved->contains("MetalFromMap") && saved->at("MetalFromMap").is_boolean() && !saved->at("MetalFromMap").as_boolean()) {
			(*saved)["MetalFromMap"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.PBR.Rmaos.MetalFromMap false -> true (_rmaos maps decide metal)";
		}
	}
	// 92 (2026-11-19): RT helper handover wait (HelperWait 0 -> 1, eliminates 5-8 ms GPU queue handover stall), Reflections RT
	// RideMainExchange + FollowRenderScale on (single exchange + RenderScale sub-rect copies), ReplaceForward + ReflectionsOwnPuddles on.
	// (2026-11-22 merge: PARKED, owner's question. This fork keeps HelperWait 0 and the three RT reflection keys off by default, and has no
	// WetWorld: the step is off, kParkUpstream92.)
	if (version < 92 && !kParkUpstream92) {
		replace("Main.Main.RayTracing", "HelperWait", 0, 1, true);
		auto toTrue = [&](const char* section, const char* key) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean()) return;
			(*saved)[key] = true;
			changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " false -> true";
		};
		toTrue("Shaders.Reflections.RT", "RideMainExchange");
		toTrue("Shaders.Reflections.RT", "FollowRenderScale");
		toTrue("Shaders.Reflections.Main", "ReplaceForward");
	}
	// 93 (2026-11-18/19, merged from claude/preset-old-values, renumbered 92 -> 93 after RT overhaul took 92): a settings file from New Vegas
	// Reloaded itself (no [_Unofficial] SettingsVersion: never saved by this project's builds), as preset packs ship it (Caffeine, Riven).
	// (a) Its fog keys (SimpleFog*, HeightFog*, MinimumBaseFog...) are this build's LEGACY fog keys, read only with LegacyFog: without it
	//     the current fog drew its own look and the preset's fog was lost. LegacyAs441 with it: 4.4.1's weaker height fog (VolumetricFog.cpp).
	// (b) [Shaders.ShadowsExteriors.FormsNear/Middle/Far/Lod] became the one [Shaders.ShadowsExteriors.Forms]: FormsNear (the cascade near
	//     the camera, the one people see) carries over when Forms itself was never saved.
	// A file with LegacyFog or ClassicLook saved chose its fog already; one saved by this project's builds (version > 0) is left as it is.
	if (version == 0) {
		tomlValue* fog = UserSection("Shaders.VolumetricFog.Main");
		if (fog && fog->is_table() && !fog->contains("LegacyFog") && !fog->contains("ClassicLook")) {
			bool legacyKeys = false;
			for (const char* key : { "SimpleFogBlend", "SimpleFogHeight", "HeightFogDensity", "HeightFogDist", "HeightFogBlend", "MinimumBaseFog" })
				legacyKeys = legacyKeys || fog->contains(key);
			if (legacyKeys) {
				(*fog)["LegacyFog"] = true;
				(*fog)["LegacyAs441"] = true;
				changed += std::string(changed.empty() ? "" : ", ") + "Shaders.VolumetricFog.Main.LegacyFog and LegacyAs441 on (a New Vegas Reloaded settings file: "
					"its fog as 4.4.1 drew it; LegacyFog false = this build's fog)";
			}
		}
		const tomlValue* nearForms = UserSection("Shaders.ShadowsExteriors.FormsNear");
		const tomlValue* forms = UserSection("Shaders.ShadowsExteriors.Forms");
		tomlValue* exteriors = UserSection("Shaders.ShadowsExteriors");
		StringList steps;
		StorageKeys("Shaders.ShadowsExteriors.Forms", &steps);
		const tomlValue* formsDefaults = FindSection(&DefaultConfig, &steps);
		if (nearForms && exteriors && formsDefaults && formsDefaults->is_table() && (!forms || forms->as_table().empty())) {
			tomlValue::table_type carried;
			for (const auto& [key, value] : nearForms->as_table())
				if (formsDefaults->contains(key) && !value.is_table()) carried[key] = value;
			if (!carried.empty()) {
				(*exteriors)["Forms"] = carried;
				changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsExteriors.FormsNear copied to Shaders.ShadowsExteriors.Forms (one list for every cascade now)";
			}
		}
		// (c) 2026-11-18: the ground lit as 4.4.1 lit it, without this build's sky fill on the land (Terrain SkylightingScale) and the light
		//     given back into the ground's cracks (Parallax ShadowBounce). Only keys the file never had.
		if (UserSection("Shaders.Terrain")) {
			auto zeroIfUnsaved = [&](const char* section, const char* key) {
				StringList steps;
				StorageKeys(section, &steps);
				tomlValue* table = &TomlConfig;
				for (const std::string& step : steps) {
					if (!table->contains(step) || !table->at(step).is_table()) (*table)[step] = toml::table();
					table = &table->at(step);
				}
				if (table->contains(key)) return;
				(*table)[key] = 0.0;
				changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " 0 (as New Vegas Reloaded 4.4.1 lit the ground)";
			};
			for (const char* group : { "Shaders.Terrain.Main", "Shaders.Terrain.Rain", "Shaders.Terrain.Night", "Shaders.Terrain.NightRain" })
				zeroIfUnsaved(group, "SkylightingScale");
			zeroIfUnsaved("Shaders.Terrain.Parallax", "ShadowBounce");
		}
	}
	// 94 (2026-11-21): [Shaders.PBR.Main] VanillaBridgeDielectricMax 0.08 -> 0.04 (caps unmapped non-metal surfaces at physical
	// dielectric F0 so clothes, hair and rocks without _rmaos maps do not look glossy/plastic; hidden under Internal, so existing
	// saved configs move once).
	// (2026-11-22 merge: PARKED, owner's question. This fork keeps 0.08 by default: kParkUpstream94.)
	if (version < 94 && !kParkUpstream94) {
		replace("Shaders.PBR.Main", "VanillaBridgeDielectricMax", 0.08, 0.04, false);
	}
	// 95, was 90 (2026-11-18, owner decision: "include CAS"): Sharpening is AMD FidelityFX CAS only (the fork's filter; 1.1.0 had it behind
	// [Shaders.Sharpening.Main] CAS = false, the old box-average filter by default). Strength is now CAS's sharpness, 0-1, default 0.5: a
	// menu save holds the old filter's 0.75 (outdoors and indoors), which moves to 0.5 once; another saved value is a choice and stays (it
	// reads as a CAS sharpness, clamped to 0-1). A player who had switched CAS on already saw CAS at their Strength: theirs stays. The CAS
	// switch is gone: a saved one is removed here, so it is not reported as unknown. Enabled is untouched (off by default, as before).
	if (ours < 90) {
		tomlValue* saved = UserSection("Shaders.Sharpening.Main");
		const bool casWasOn = saved && saved->is_table() && saved->contains("CAS") && saved->at("CAS").is_boolean() && saved->at("CAS").as_boolean();
		if (!casWasOn) {
			replace("Shaders.Sharpening.Main", "Strength", 0.75, 0.5, false);
			replace("Shaders.Sharpening.Interiors", "Strength", 0.75, 0.5, false);
		}
		if (saved && saved->is_table() && saved->contains("CAS")) {
			saved->as_table().erase("CAS");
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.Sharpening.Main.CAS removed (CAS is now the only sharpening filter)";
		}
	}
	// 96, was 91 (2026-11-21, owner: "move the 'is it raining / is the ground wet' clock to Wetness Effects and remove WetWorld", then "make the
	// default mine"): WetWorld is gone; Wetness Effects and Puddles are the rain wetness, on by default with the owner's look
	// (docs/DESIGN.md "Deferred wetness design" step 4).
	//  - The cover map's shared keys (PersistentMapSize, PersistentMapResolution, PersistentDebugView) move from [Shaders.WetWorld.Main] to
	//    [Shaders.SnowAccumulation.Main], a saved value carrying over; then the whole WetWorld section leaves the file.
	//  - A saved Wetness Effects / Puddles Enabled false moves to true when WetWorld was on (the player had rain wetness, which WetWorld
	//    alone gave them); one who had switched WetWorld off keeps them as saved. Both are restart keys, read after this step: from this start.
	//  - The other defaults the owner's settings changed move from the old default once; a value the player chose stays.
	if (ours < 91) {
		tomlValue* wetWorld = UserSection("Shaders.WetWorld.Main");
		tomlValue* snow = UserSection("Shaders.SnowAccumulation.Main");
		if (wetWorld && wetWorld->is_table() && !snow) {	// (review) no snow section saved: made, so the keys are not lost
			tomlValue* parentTable = UserSection("Shaders.SnowAccumulation");
			if (!parentTable) {
				tomlValue* shadersTable = UserSection("Shaders");
				if (shadersTable && shadersTable->is_table()) { (*shadersTable)["SnowAccumulation"] = toml::table(); parentTable = UserSection("Shaders.SnowAccumulation"); }
			}
			if (parentTable && parentTable->is_table()) { (*parentTable)["Main"] = toml::table(); snow = UserSection("Shaders.SnowAccumulation.Main"); }
		}
		if (wetWorld && wetWorld->is_table() && snow && snow->is_table()) {
			for (const char* key : { "PersistentMapSize", "PersistentMapResolution", "PersistentDebugView" }) {
				if (!wetWorld->contains(key) || snow->contains(key)) continue;
				(*snow)[key] = wetWorld->at(key);
				changed += std::string(changed.empty() ? "" : ", ") + "Shaders.WetWorld.Main." + key + " -> Shaders.SnowAccumulation.Main." + key;
			}
		}
		tomlValue* wetWorldStatus = UserSection("Shaders.WetWorld.Status");
		const bool wetWorldOff = wetWorldStatus && wetWorldStatus->is_table() && wetWorldStatus->contains("Enabled") &&
			wetWorldStatus->at("Enabled").is_boolean() && !wetWorldStatus->at("Enabled").as_boolean();
		auto setBool = [&](const char* section, const char* key, bool from, bool to) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->is_table() || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean() != from) return;
			(*saved)[key] = to;
			changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + (to ? " false -> true" : " true -> false");
		};
		if (!wetWorldOff) {
			setBool("Shaders.WetnessEffects.Status", "Enabled", false, true);
			setBool("Shaders.Puddles.Status", "Enabled", false, true);
		}
		tomlValue* shaders = UserSection("Shaders");
		if (shaders && shaders->is_table() && shaders->contains("WetWorld")) {
			shaders->as_table().erase("WetWorld");
			changed += std::string(changed.empty() ? "" : ", ") + "the [Shaders.WetWorld] settings removed (WetWorld is gone: Wetness Effects and Puddles are the rain wetness)";
		}
		replace("Shaders.WetnessEffects.Main", "MaxRainWetness", 1.0, 0.5, false);
		replace("Shaders.WetnessEffects.Raindrops", "RaindropChance", 1.0, 0.5, false);
		replace("Shaders.WetnessEffects.Reflections", "EdgeFade", 0.1, 0.279, false);
		replace("Shaders.WetnessEffects.Reflections", "RoughnessCutoff", 0.5, 0.434, false);
		replace("Shaders.WetnessEffects.Reflections", "Thickness", 30.0, 50.0, false);
		setBool("Shaders.WetnessEffects.Reflections", "Enabled", true, false);
		replace("Shaders.Puddles.Main", "Cavity", 1.0, 1.993, false);
		replace("Shaders.Puddles.Main", "Coverage", 0.25, 0.262, false);
		replace("Shaders.Puddles.Main", "Darkening", 0.6, 1.0, false);
		replace("Shaders.Puddles.Main", "EdgeSoftness", 0.05, 0.108, false);
		replace("Shaders.Puddles.Main", "Ripples", 1.0, 2.0, false);
		replace("Shaders.Puddles.Main", "Roughness", 0.03, 0.1, false);
		replace("Shaders.Puddles.Main", "Size", 420.0, 250.0, false);
		setBool("Shaders.Puddles.Main", "ObjectPuddles", false, true);
	}
	// 97, was 92 (2026-11-22, upstream 1.1.2, owner: "Take upstream's"): [Shaders.HDR.Main] Gamma22 false -> true, HDR output decodes the SDR picture
	// with a pure 2.2 gamma by default (HdrOutput.h). A menu save holds the old false, which moves to true once; a saved true stays. It only
	// changes the picture while HDR output is on (Enabled, off by default, with the DXVK fork and Windows HDR) and applies immediately.
	if (ours < 92) {
		tomlValue* saved = UserSection("Shaders.HDR.Main");
		if (saved && saved->is_table() && saved->contains("Gamma22") && saved->at("Gamma22").is_boolean() && !saved->at("Gamma22").as_boolean()) {
			(*saved)["Gamma22"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.HDR.Main.Gamma22 false -> true (HDR output: pure 2.2 gamma decode)";
		}
	}
	// 98 (2026-11-26, owner via Main brain: "do it all", upstream's defaults): [Shaders.Reflections.Main] ReplaceForward and
	// [Shaders.Reflections.RT] FollowRenderScale false -> true, as upstream's step 92 moves them (its RideMainExchange stays off here:
	// RayGen would run before the deferred wetness film). A file saved by upstream's builds at 92-94 ran that step already: a false it
	// holds is the player's own choice and stays.
	{
		const bool upstreamRan92 = upstreamRan(92);
		if (version < 98 && !upstreamRan92) {
			for (const auto& [section, key] : { std::pair<const char*, const char*>{ "Shaders.Reflections.Main", "ReplaceForward" },
					std::pair<const char*, const char*>{ "Shaders.Reflections.RT", "FollowRenderScale" } }) {
				tomlValue* saved = UserSection(section);
				if (!saved || !saved->contains(key) || !saved->at(key).is_boolean() || saved->at(key).as_boolean()) continue;
				(*saved)[key] = true;
				changed += std::string(changed.empty() ? "" : ", ") + section + "." + key + " false -> true";
			}
		}
	}
	// 99, upstream 1.1.4's 95 (2026-11-21): [Main.Main.RayTracing] ActorFlushPerSpace false -> true (avoids flushing and rebuilding all actors on every
	// exterior cell crossing; flushes only on worldspace/interior change or >8192 unit camera jumps).
	if (version < 99 && !upstreamRan(95)) {
		tomlValue* saved = UserSection("Main.Main.RayTracing");
		if (saved && saved->contains("ActorFlushPerSpace") && saved->at("ActorFlushPerSpace").is_boolean() && !saved->at("ActorFlushPerSpace").as_boolean()) {
			(*saved)["ActorFlushPerSpace"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.RayTracing.ActorFlushPerSpace false -> true";
		}
	}
	// 100, upstream 1.1.4's 96 (2026-11-22): [Shaders.ShadowsExteriors.Deferred] MatchSunDisc true -> false (matching vanilla FNV sun direction avoids
	// flattening terrain normal/parallax relief when the sky sun sits high around midday/afternoon).
	if (version < 100 && !upstreamRan(96)) {
		tomlValue* saved = UserSection("Shaders.ShadowsExteriors.Deferred");
		if (saved && saved->contains("MatchSunDisc") && saved->at("MatchSunDisc").is_boolean() && saved->at("MatchSunDisc").as_boolean()) {
			(*saved)["MatchSunDisc"] = false;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsExteriors.Deferred.MatchSunDisc true -> false";
		}
	}
	// (2026-11-27 upstream 1.1.5 merge: 101-103 are this fork's own steps; an upstream-built file at 101 never ran them, and each only moves an exact
	// old value or deletes retired keys, so they also run for any upstream file.)
	// 101 (2026-11-26, upstream 1.1.4's defaults, owner: take upstream's): [Main.Main.RayTracing] PTGlassSurface 0 -> 1,
	// [Shaders.AmbientOcclusion.RT] GIStrengthInterior -1 -> 2 and GlowGIInterior -1 -> 1.5. Upstream changed them without a step, so a
	// menu save of any earlier build (theirs or ours) holds the old values: each moves once from exactly its old default.
	if (version < 101 || upstreamFile) {
		replace("Main.Main.RayTracing", "PTGlassSurface", 0.0, 1.0, false);
		replace("Shaders.AmbientOcclusion.RT", "GIStrengthInterior", -1.0, 2.0, false);
		replace("Shaders.AmbientOcclusion.RT", "GlowGIInterior", -1.0, 1.5, false);
	}
	// 102 (2026-11-26, mirror puddles; docs/EFFECTS.md "Reflections" "Mirror puddles"): [Shaders.Reflections.RT] SkyPanorama false -> true (mirror
	// puddles show the sky at full strength, and the plain gradient a missed ray took was paler than the drawn sky) and [Shaders.Puddles.Main]
	// Roughness 0.1 -> 0.02 (the mirror's blur; under 0.03 the reflection is the exact mirror ray). A menu save holds the old values.
	if (version < 102 || upstreamFile) {
		replace("Shaders.Puddles.Main", "Roughness", 0.1, 0.02, false);
		tomlValue* saved = UserSection("Shaders.Reflections.RT");
		if (saved && saved->contains("SkyPanorama") && saved->at("SkyPanorama").is_boolean() && !saved->at("SkyPanorama").as_boolean()) {
			(*saved)["SkyPanorama"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.Reflections.RT.SkyPanorama false -> true";
		}
	}
	// 103 (2026-11-27): the older forward Bend port (Community Shaders' screen-space shadows in the forward lit shaders) was removed (main
	// 2480e33; the Bottled Shaders port in the deferred sun replaces it): its ten [Shaders.ShadowsExteriors.ScreenSpace] keys are deleted
	// from a saved file, so the install check does not call them unknown (they are also on Retired's list, for a file saved by a newer build).
	if (version < 103 || upstreamFile) {
		tomlValue* saved = UserSection("Shaders.ShadowsExteriors.ScreenSpace");
		if (saved && saved->is_table()) {
			int removed = 0;
			for (const char* key : { "Bend", "InLighting", "SampleMultiplier", "SurfaceThickness", "BilinearThreshold", "ShadowContrast", "DepthTolerance", "StartDistance", "FadeLength", "BendDebug" })
				if (saved->contains(key)) { saved->as_table().erase(key); removed++; }
			if (removed) changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsExteriors.ScreenSpace: " + std::to_string(removed) + " keys of the removed Bend port deleted";
		}
	}
	// 104, upstream 1.1.5's 97 (2026-11-23): [Shaders.ShadowsExteriors.Main] NightMinDarkness 0.35 -> 0.65 and UsePointShadowsNight false -> true
	// (strengthens moonlight shadows and enables deferred outdoor night lamp shadows on existing configs).
	if (version < 104 && !upstreamRan(97)) {
		replace("Shaders.ShadowsExteriors.Main", "NightMinDarkness", 0.35, 0.65, false);
		tomlValue* saved = UserSection("Shaders.ShadowsExteriors.Main");
		if (saved && saved->contains("UsePointShadowsNight") && saved->at("UsePointShadowsNight").is_boolean() && !saved->at("UsePointShadowsNight").as_boolean()) {
			(*saved)["UsePointShadowsNight"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsExteriors.Main.UsePointShadowsNight false -> true";
		}
	}
	// 105, upstream 1.1.5's 98 (2026-11-24, the dynamic-reflections upgrade & AmbientLight defaults; review: the menu's Save writes every key, so without this step a
	// saved file kept the old values while a fresh install got the new ones). [Shaders.PBR.Metal] PolishedRoughness 0.65 -> 0.18 is a FIX
	// (the painted shininess mask made metal rougher than bare metal: inverted), also in StalePresetValues.h; the others are the look:
	// [Shaders.PBR.Metal] Roughness 0.3 -> 0.35, [Shaders.PBR.EnvMaps] Lighting 0.5 -> 0.45, Strength 1.5 -> 1.8, WithMetal 0.6 -> 0.85,
	// [Shaders.Reflections.Main] CubeCapture 0.02 -> 0.25, CubeFade 0.005 -> 0.001, CloudSky false -> true,
	// [Shaders.AmbientLight.Main] ExteriorBalance true -> false.
	if (version < 105 && !upstreamRan(98)) {
		replace("Shaders.PBR.Metal", "PolishedRoughness", 0.65, 0.18, false);
		replace("Shaders.PBR.Metal", "Roughness", 0.3, 0.35, false);
		replace("Shaders.PBR.EnvMaps", "Lighting", 0.5, 0.45, false);
		replace("Shaders.PBR.EnvMaps", "Strength", 1.5, 1.8, false);
		replace("Shaders.PBR.EnvMaps", "WithMetal", 0.6, 0.85, false);
		replace("Shaders.Reflections.Main", "CubeCapture", 0.02, 0.25, false);
		replace("Shaders.Reflections.Main", "CubeFade", 0.005, 0.001, false);
		tomlValue* saved = UserSection("Shaders.Reflections.Main");
		if (saved && saved->contains("CloudSky") && saved->at("CloudSky").is_boolean() && !saved->at("CloudSky").as_boolean()) {
			(*saved)["CloudSky"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.Reflections.Main.CloudSky false -> true";
		}
		tomlValue* al = UserSection("Shaders.AmbientLight.Main");
		if (al && al->contains("ExteriorBalance") && al->at("ExteriorBalance").is_boolean() && al->at("ExteriorBalance").as_boolean()) {
			(*al)["ExteriorBalance"] = false;
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.AmbientLight.Main.ExteriorBalance true -> false";
		}
	}
	// 106, upstream 1.1.5's 99 (2026-11-24, weapon reflections for all users): tuned PBR metal weapon defaults and enabled non-RT + RT weapon reflections.
	if (version < 106 && !upstreamRan(99)) {
		replace("Shaders.PBR.Metal", "GreyMetal", 0.5, 0.75, false);
		replace("Shaders.PBR.Metal", "Roughness", 0.35, 0.28, false);
		replace("Shaders.PBR.Metal", "Reflections", 0.6, 1.0, false);
		tomlValue* metal = UserSection("Shaders.PBR.Metal");
		if (metal && metal->contains("PolishedRoughness")) {
			const tomlValue& pr = metal->at("PolishedRoughness");
			if (pr.is_floating() && pr.as_floating() == 0.18) {
				(*metal)["PolishedRoughness"] = 0.14;
				changed += std::string(changed.empty() ? "" : ", ") + "Shaders.PBR.Metal.PolishedRoughness 0.18 -> 0.14";
			}
		}
		replace("Shaders.Reflections.RT", "Weapon", 0.0, 0.85, false);
		replace("Shaders.Reflections.RT", "WeaponRoughness", 0.3, 0.22, false);
	}
	// 107, upstream 1.1.5's 100 (2026-11-24, weapon reflection refinement): move step 99's over-reflective defaults onto balanced gunmetal vs nickel values.
	if (version < 107 && !upstreamRan(100)) {
		auto moveFloat = [&](const char* section, const char* key, double oldVal, double newVal) {
			tomlValue* saved = UserSection(section);
			if (!saved || !saved->contains(key)) return;
			const tomlValue& cur = saved->at(key);
			const double v = cur.is_floating() ? cur.as_floating() : cur.is_integer() ? (double)cur.as_integer() : -1.0e9;
			if (fabs(v - oldVal) < 1e-4) {
				(*saved)[key] = newVal;
				changed += std::string(changed.empty() ? "" : ", ") + section + "." + key;
			}
		};
		moveFloat("Shaders.PBR.Metal", "GreyMetal", 0.75, 0.45);
		moveFloat("Shaders.PBR.Metal", "Roughness", 0.28, 0.36);
		moveFloat("Shaders.PBR.Metal", "PolishedRoughness", 0.14, 0.16);
		moveFloat("Shaders.PBR.Metal", "Reflections", 1.0, 0.75);
		moveFloat("Shaders.Reflections.RT", "Weapon", 0.85, 0.65);
		moveFloat("Shaders.Reflections.RT", "WeaponRoughness", 0.22, 0.28);
	}
	// 108, upstream 1.1.5's 101 (2026-11-24, claude/fd-complete batch 2, docs/next/FD-COMPLETE.md; the owner's and Conor's definition of a complete deferred renderer):
	// [Main.Main.Quality] DeferredFirstPerson false -> true: the gun and hands are lit by the deferred passes (sun, every lamp, the flashlight's
	// beam) instead of the game's forward lights. A saved false (the default until now; a test key, so only a menu save holds it) moves once;
	// a restart key, read when the shaders are first compiled, which is after this step.
	if (version < 108 && !upstreamRan(101)) {
		tomlValue* saved = UserSection("Main.Main.Quality");
		if (saved && saved->contains("DeferredFirstPerson") && saved->at("DeferredFirstPerson").is_boolean() && !saved->at("DeferredFirstPerson").as_boolean()) {
			(*saved)["DeferredFirstPerson"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Quality.DeferredFirstPerson false -> true (the gun and hands through the deferred renderer, settings step 108)";
		}
	}
	// 109 (2026-11-27 upstream 1.1.5 merge): upstream changed three defaults without a step, so a menu save of any earlier build (theirs or ours)
	// holds the old value: [Shaders.ShadowsExteriors.ShadowMaps] CasterFadeMin 0.75 -> 0.25 (visible: shadow casters fade out further away),
	// [Main.Main.RayTracing] ArenaMB 16 -> 32 and ActorRebuildInterval 8 -> 16. Each moves once, only from exactly its old default.
	// 110 (2026-11-27, owner: "just remove the capsule stuff from the code"): capsule shadows are gone; their [Shaders.ShadowsInteriors.Capsules]
	// section is deleted from a saved file (it is also on Retired's list, for a file saved by a newer build).
	if (version < 110) {
		tomlValue* saved = UserSection("Shaders.ShadowsInteriors");
		if (saved && saved->is_table() && saved->contains("Capsules")) {
			saved->as_table().erase("Capsules");
			changed += std::string(changed.empty() ? "" : ", ") + "Shaders.ShadowsInteriors.Capsules: the removed capsule shadows' settings deleted";
		}
	}
	if (version < 109) {
		replace("Shaders.ShadowsExteriors.ShadowMaps", "CasterFadeMin", 0.75, 0.25, false);
		replace("Main.Main.RayTracing", "ArenaMB", 16.0, 32.0, true);
		replace("Main.Main.RayTracing", "ActorRebuildInterval", 8.0, 16.0, true);
	}
	// 111 (2026-11-27 sunsheen-fade, owner: "i dont give a fuck about staying with d3d9"): [Main.Main.Performance] NvrDxvkDeferred false -> true,
	// the deferred sun (Compute\DeferredSun.spv, now with the sun steps' fade and the wet film's highlight) and the deferred lamps
	// (Compute\DeferredLamps.spv) as Vulkan compute passes under the DXVK fork; the D3D9 passes stay the fallback without it. This fork shipped
	// false since the 1.1.3 merge, so a menu save of ours holds it and moves once. Not in a file upstream's builds saved: their default was
	// always true, so a false there is the player's own. A live key: it applies at once.
	if (version < 111 && !upstreamFile) {
		tomlValue* saved = UserSection("Main.Main.Performance");
		if (saved && saved->contains("NvrDxvkDeferred") && saved->at("NvrDxvkDeferred").is_boolean() && !saved->at("NvrDxvkDeferred").as_boolean()) {
			(*saved)["NvrDxvkDeferred"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Performance.NvrDxvkDeferred false -> true (the deferred sun and lamps in Vulkan, settings step 111)";
		}
	}
	// 112 (2026-11-28 upstream 1.1.6-test2 merge; upstream's step 102): [Main.Main.Performance] NvrDxvkPasses false -> true (the Vulkan
	// passes on). Both defaults have been true since 1.0.0, so a saved false is the player's own: moved anyway because the DXVK fork is now
	// required (owner 2026-11-28: fully deferred) and the renderer needs its Vulkan passes. Not again in a file upstream's builds saved after
	// their own 102.
	if (version < 112 && !upstreamRan(102)) {
		tomlValue* saved = UserSection("Main.Main.Performance");
		if (saved && saved->contains("NvrDxvkPasses") && saved->at("NvrDxvkPasses").is_boolean() && !saved->at("NvrDxvkPasses").as_boolean()) {
			(*saved)["NvrDxvkPasses"] = true;
			changed += std::string(changed.empty() ? "" : ", ") + "Main.Main.Performance.NvrDxvkPasses false -> true (Vulkan passes on by default, settings step 112)";
		}
	}
	// Upstream's step 103 (NvrDxvkAmbientLight, AvgLumaCompute, NvrDxvkClouds on; TargetReleaseSeconds 20 -> 0; PBR.EnvMaps Strength
	// 1.8 -> 1.2 and WithMetal 0.85 -> 0.25; retired keys reset) is not taken: those are defaults the owner has not chosen (docs/DESIGN.md
	// "Upstream 1.1.6-test2 study"). Taking them later = their defaults in the toml fragments plus a NEW step numbered above the version
	// this build stamps (113 while this build stamps 112), guarded !upstreamRan(103).
	if (!changed.empty())
		Logger::Log("UNOFFICIAL settings: old defaults saved by an earlier build moved to the new defaults: %s. "
			"Change them in the menu and press Save if you want the old values back.", changed.c_str());

	if (!TomlConfig.contains("_Unofficial") || !TomlConfig.at("_Unofficial").is_table()) TomlConfig["_Unofficial"] = toml::table();
	TomlConfig["_Unofficial"]["SettingsVersion"] = UnofficialSettingsVersion;
}
