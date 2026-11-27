#include <mutex>
#include <unordered_set>	// UNOFFICIAL 2026-10-30 SettingsCache
#include "InstallCheck.h"	// 2026-11-03: the user toml's unknown keys and version mark, for the install self-check
#include "GpuDebug.h"	// UNOFFICIAL 2026-10-26: GpuDebugLabels / RenderDoc capture key ([Main.Develop.Main])
#include "SettingForce.h"	// UNOFFICIAL 2026-11-14 full deferred renderer: its prerequisites read as forced
#include "StalePresetValues.h"	// UNOFFICIAL 2026-11-18: settings step 90 moves the old defaults presets and scripts saved
#include "ShelvedEffects.h"	// UNOFFICIAL 2026-11-16: a release build reads [Shaders.ShadowsInteriors.Capsules] Enabled as off
#include "UnsavedSettings.h"	// 2026-11-25 (unsaved marks): which settings the menu marks as not saved yet

// A settings file in <game>\Data\NVSE\Plugins (the game folder is read once, Paths::GameRoot: common-dialog calls can change the
// current directory later). 2026-11-02 Vegas Shaders: forWrite gives the new name (VegasShaders.toml...); a read takes the new
// name if it exists, else the old New Vegas Reloaded one (NewVegasReloaded.dll.toml...), so an old install keeps its settings
// and the first save writes them under the new name. The old file is never changed.
static void ConfigPathOf(char (&dest)[MAX_PATH], Paths::File file, bool forWrite = false) {
	const std::string folder = Paths::PluginsDir();
	const std::string path = forWrite ? Paths::WritePath(folder, file) : Paths::ReadPath(folder, file);
	strncpy_s(dest, path.c_str(), _TRUNCATE);
}

// A section's storage path split into its steps: the tables are kept under a leading "_" ("_Shaders.Bloom.Main").
static void StorageKeys(const char* section, StringList* steps) {
	// (2026-11-17: a string, was char[MAX_PATH] + strcat: a preset's or a script's section name of any length arrives here)
	const std::string path = std::string("_") + section;
	SettingManager::SplitString(path.c_str(), ".", steps);
}

// Bounded copy into a node's 80-byte text: cut at 79 characters, always terminated.
static void PutNodeText(char (&dest)[80], const char* text) {
	strncpy(dest, text, sizeof(dest) - 1);
	dest[sizeof(dest) - 1] = 0;
}

// Every node carries its value as int, float and bool at once, whatever its type, so a read through another accessor
// (GetSettingI on a bool, GetSettingF on an int) gets the native value without parsing the text.
typedef SettingManager::Configuration::ConfigNode SettingNode;
static void StoreInt(SettingNode* node, int number) {
	node->IntValue = number;
	node->FloatValue = (float)number;
	node->BoolValue = number != 0;
}
static void StoreFloat(SettingNode* node, float number) {
	node->FloatValue = number;
	node->IntValue = (int)number;
	node->BoolValue = number != 0.0f;
}
static void StoreBool(SettingNode* node, bool flag) {
	node->BoolValue = flag;
	node->IntValue = flag ? 1 : 0;
	node->FloatValue = flag ? 1.0f : 0.0f;
}

// bool and int print as "1"/"0" and as plain digits, never through the generic template's float route; they are declared
// before that template is first used below.
template <> std::string SettingManager::ToString<bool>(const bool flag) {
	return flag ? std::string("1") : std::string("0");
}

template <> std::string SettingManager::ToString<int>(const int number) {
	return std::to_string(number);
}

// The user config's [_Unofficial] SettingsVersion this build brings it up to (UpdateOldDefaults, below).
static const int UnofficialSettingsVersion = 109;  // (109, 2026-11-27 upstream 1.1.5 merge: CasterFadeMin 0.75 -> 0.25, ArenaMB 16 -> 32, ActorRebuildInterval 8 -> 16, which upstream changed without a step; 104-108 are upstream 1.1.5's 97-101: 108 DeferredFirstPerson on, 107 and 106 weapon metal and RT weapon reflection defaults, 105 PBR metal, env-map, cube-capture look and AmbientLight ExteriorBalance off, 104 NightMinDarkness 0.65 + UsePointShadowsNight on; 103, 2026-11-27: the removed forward Bend port's ten [ScreenSpace] keys deleted; 102, 2026-11-26 mirror puddles: [Shaders.Reflections.RT] SkyPanorama false -> true, [Shaders.Puddles.Main] Roughness 0.1 -> 0.02; 101: upstream 1.1.4's PTGlassSurface, GIStrengthInterior, GlowGIInterior; 2026-11-26 upstream 1.1.4 merge: 100 = upstream's 96 [Shaders.ShadowsExteriors.Deferred] MatchSunDisc true -> false, 99 = upstream's 95 [Main.Main.RayTracing] ActorFlushPerSpace false -> true; 98, 2026-11-26: Reflections ReplaceForward + RT FollowRenderScale on, upstream's defaults, owner's word; 2026-11-22 upstream 1.1.3 merge: 90-94 are upstream's steps, ours renumbered 90-92 -> 95-97. 97: HDR Gamma22 on (upstream 1.1.2); 96, 2026-11-21: WetWorld removed, Wetness Effects + Puddles on with the owner's look, the cover map's keys moved to SnowAccumulation; 95, 2026-11-18: Sharpening is CAS only, Strength 0.75 -> 0.5, the CAS switch removed; 94 (upstream): VanillaBridgeDielectricMax, parked; 93 (upstream): a New Vegas Reloaded settings file's fog and shadow forms; 92 (upstream): RT helper wait and RT reflection flips, parked; 91 (upstream): MetalFromMap, parked; 90 (upstream): old defaults left by presets; 89, 2026-11-17: DeferredTransparents on (glass; the silver-bottle fix is in); 88, 2026-11-16: DeferredSkin (+ SSS 2), DeferredHair on (DeferredTransparents held off on 11-17, on again in 89); 87, 2026-11-16: [Shaders.TAA.Cutouts] Enabled on (Smooth Cutouts with TAA); 86: SMAA and DitherBuster off; 85, 2026-11-15: DeferredRenderer 0 -> 2, the deferred renderer on by default; 84: FlashDetect, BeatDetect, HistoryWatch off; 83: RT sun shadows Mode 1 (replace) + RT grass on; 82: GameShaderPairRepair on; 81: CapturedGrass on, for path tracing; 80: the daylight look, only where it was never touched; 79: rain LightingScale; 78: RT defaults replaced since 1.0.5 now reach saved configs; 77: ptaaa-firstperson, FirstPersonBody on again) 53 was an enhanced-water step, since removed

// UNOFFICIAL 2026-10-30: a setting is read only when the defaults file lists it (FillNode), so a line in the user's toml that
// this build does not know (a key from a newer or an older build, a typo) silently did nothing. Each one is logged once per
// load; a whole unknown section is one line. [_Unofficial] is NVR's own bookkeeping, never in the defaults.
// 2026-11-03: also returns them (section.key), for the install check (InstallCheck.h)
// 2026-11-05: settings this build removed on purpose (the Oblivion-era leftovers: the old text menu's fonts, colours, layout and keys,
// SleepingMode, TorchesCastShadows). Older settings files still hold them; they are skipped quietly instead of counted as unknown.
static bool Retired(const std::string& Name) {
	static const char* const kSections[] = { "Main.SleepingMode", "Shaders.WetWorld", "Shaders.Blood", "Shaders.BloodFlow", "Shaders.BloodLens" };	// (2026-11-26: BloodFlow and BloodLens removed, as upstream 1.1.1 did, owner's word)	// (Shaders.Blood: upstream 1.1.1's dead stub; exact or dot: BloodFlow / BloodLens stay)	// (2026-11-21: WetWorld removed; settings step 91 drops it)
	static const char* const kKeys[] = { "Shaders.ShadowsInteriors.Main.TorchesCastShadows", "Shaders.ShadowsExteriors.ScreenSpace.LodOnly",	// (2026-11-06: always on now)
		"Shaders.Puddles.Main.Streaks", "Shaders.Puddles.Main.StreakWidth", "Shaders.Puddles.Main.StreakSpeed",	// (2026-11-18: the wall streaks removed)
		"Main.Menu.Keys.KeyAdd", "Main.Menu.Keys.KeyDown", "Main.Menu.Keys.KeyEditing", "Main.Menu.Keys.KeyLeft", "Main.Menu.Keys.KeyPageDown",
		"Main.Menu.Keys.KeyPageUp", "Main.Menu.Keys.KeyRight", "Main.Menu.Keys.KeySave", "Main.Menu.Keys.KeySubtract", "Main.Menu.Keys.KeyUp",
		"Main.Menu.Keys.EntryUseNumpad", "Main.Menu.Style.ItemColumnSize", "Main.Menu.Style.MainItemColumnSize", "Main.Menu.Style.PositionX",
		"Main.Menu.Style.PositionY", "Main.Menu.Style.RowSpace", "Main.Menu.Style.RowsPerPage", "Main.Menu.Style.TextColorEditing",
		"Main.Menu.Style.TextColorEnabled", "Main.Menu.Style.TextColorNormal", "Main.Menu.Style.TextColorSelected", "Main.Menu.Style.TextFont",
		"Main.Menu.Style.TextFontStatus", "Main.Menu.Style.TextShadowColorEditing", "Main.Menu.Style.TextShadowColorEnabled",
		"Main.Menu.Style.TextShadowColorNormal", "Main.Menu.Style.TextShadowColorSelected", "Main.Menu.Style.TextSizeStatus",
		"Main.Menu.Style.TitleColumnSize",
		// (upstream 1.1.1, 2026-11-19: Hi-Z trace and the FFX reflection denoiser's clip retired)
		"Shaders.AmbientLight.Main.HiZ", "Shaders.Reflections.Main.HiZ", "Shaders.Reflections.Main.HiZSafeCells", "Shaders.Reflections.Main.HiZCheck",
		"Shaders.Reflections.Main.HiZInteriorsTest", "Shaders.Reflections.RT.DenoiserClip",
		// (2026-11-27: the older forward Bend port removed; settings step 103 also deletes them from a saved file)
		"Shaders.ShadowsExteriors.ScreenSpace.Bend", "Shaders.ShadowsExteriors.ScreenSpace.InLighting", "Shaders.ShadowsExteriors.ScreenSpace.SampleMultiplier", "Shaders.ShadowsExteriors.ScreenSpace.SurfaceThickness", "Shaders.ShadowsExteriors.ScreenSpace.BilinearThreshold",
		"Shaders.ShadowsExteriors.ScreenSpace.ShadowContrast", "Shaders.ShadowsExteriors.ScreenSpace.DepthTolerance", "Shaders.ShadowsExteriors.ScreenSpace.StartDistance", "Shaders.ShadowsExteriors.ScreenSpace.FadeLength", "Shaders.ShadowsExteriors.ScreenSpace.BendDebug" };
	for (const char* k : kKeys) if (Name == k) return true;
	for (const char* sec : kSections) {	// the section itself or a key / table under it ("Main.SleepingMode.Main.Enabled"), not a longer name
		const size_t n = strlen(sec);
		if (Name.compare(0, n, sec) == 0 && (Name.size() == n || Name[n] == '.')) return true;
	}
	return false;
}

static std::vector<std::string> WarnUnknownKeys(const tomlValue& User, const tomlValue& Defaults) {
	std::vector<std::string> unknown;
	if (!User.is_table() || !Defaults.is_table()) return unknown;
	int logged = 0, more = 0;
	auto name = [](const std::string& path, const std::string& key) {
		std::string k = key[0] == '_' ? key.substr(1) : key;
		return path.empty() ? k : path + "." + k;
	};
	auto walk = [&](auto& self, const tomlValue& user, const tomlValue& defaults, const std::string& path) -> void {
		for (const auto& [key, value] : user.as_table()) {
			if (path.empty() && key == "_Unofficial") continue;
			const bool known = defaults.contains(key);
			if (known && value.is_table() && defaults.at(key).is_table()) { self(self, value, defaults.at(key), name(path, key)); continue; }
			if (known) continue;
			if (Retired(name(path, key))) continue;	// 2026-11-05: removed on purpose, not worth a warning
			unknown.push_back(name(path, key));
			if (logged >= 40) { more++; continue; }
			logged++;
			if (value.is_table())
				Logger::Log("UNOFFICIAL settings: section [%s] in your toml isn't known to this build: ignored", name(path, key).c_str());
			else
				Logger::Log("UNOFFICIAL settings: setting %s in your toml isn't known to this build: ignored (a newer or older build's setting, or a typo)", name(path, key).c_str());
		}
	};
	walk(walk, User, Defaults, "");
	if (more) Logger::Log("UNOFFICIAL settings: %i more settings in your toml aren't known to this build (ignored)", more);
	return unknown;
}

/*
* Reads the defaults file and the user's settings file, then the Max Quality layer (UNOFFICIAL) and the old-default migrations.
*/
void SettingManager::Configuration::Init() {

	char userFile[MAX_PATH], defaultsFile[MAX_PATH];
	ConfigPathOf(userFile, Paths::File::UserSettings);
	ConfigPathOf(defaultsFile, Paths::File::Defaults);

	configLoaded = false;
	bool userFileRead = false;	// 2026-11-03 (InstallCheck): the user's file was there and parsed

	// the defaults keep their comments: a setting's first one is its description in the menu
	try {
		Logger::Log("Loading defaults from file %s", defaultsFile);
		DefaultConfig = toml::parse<toml::preserve_comments, std::map>(std::string_view(defaultsFile)).as_table();
	}
	catch (const std::exception& failure) {
		Logger::Log("error loading defaults toml: %s", failure.what());
	}

	// UNOFFICIAL 2026-11-19: check file existence before calling toml::parse so a fresh install with no settings file
	// does not throw a first-chance std::ios_base::failure exception into the vectored exception handler (LOAD: C++ exception).
	if (GetFileAttributesA(userFile) == INVALID_FILE_ATTRIBUTES) {
		Logger::Log("No settings file yet (%s): one will be made from the defaults.", userFile);
		TomlConfig = toml::table{};	// starts empty: every read fills in its default
	}
	else {
		try {
			Logger::Log("Loading settings from file %s", userFile);
			if (Paths::OldIsNewer(Paths::PluginsDir(), Paths::File::UserSettings)) {	// 2026-11-02: a New Vegas Reloaded preset copied in later
				Logger::Log("UNOFFICIAL settings: %s is newer than %s (a preset made for New Vegas Reloaded?): it is read; the next Save writes it as %s", Paths::OldName(Paths::File::UserSettings), Paths::NewName(Paths::File::UserSettings), Paths::NewName(Paths::File::UserSettings));
				Paths::AddOldFilesNotice("Settings read from %s (newer than %s: a New Vegas Reloaded preset?). Save keeps them as %s.", Paths::OldName(Paths::File::UserSettings), Paths::NewName(Paths::File::UserSettings), Paths::NewName(Paths::File::UserSettings));
			}
			TomlConfig = toml::parse<toml::discard_comments, std::map>(std::string_view(userFile)).as_table();
			userFileRead = true;
		}
		catch (const std::exception& failure) {
			Logger::Log("error loading toml: %s, new config will be created from defaults.", failure.what());
			TomlConfig = toml::table{};	// starts empty: every read fills in its default
		}
	}

	configLoaded = true;
	++DefaultsGeneration;	// UNOFFICIAL 2026-10-18: the defaults file was read again
	Logger::Log("Loading configs finished");

	// UNOFFICIAL 2026-10-15 Max Quality ([Main.Main.Quality] MaxQuality, read once here: it needs a restart). For screenshots:
	// every effect worked out at a reduced size runs at the full size (MaxQuality.h) and the settings listed in
	// VegasShaders.maxquality.toml are read in place of the user's, never written to their file: turned off, their own
	// settings are back as they were.
	// UNOFFICIAL 2026-10-15 (user: "we should still be able to change settings when max quality is on, but not affect our
	// original settings"): Max Quality's values stay editable in the menu. A change to one of them while it is on goes into
	// Max Quality's own layer (SetValue), and Save writes those changes to VegasShaders.maxquality.user.toml, read
	// back over the shipped list next time. Decided once per session (the effects' sizes were set at start): a reload from
	// disk keeps it as it was until a restart.
	{
		static int decided = -1;	// -1: not yet, else 0 / 1 for the whole session
		ConfigNode node;
		if (decided < 0) decided = FillNode(&node, "Main.Main.Quality", "MaxQuality") && node.BoolValue ? 1 : 0;
		MaxQualityActive = decided == 1;
		MaxQualityConfig = toml::table();
		MaxQualityUser = toml::table();
		MaxQualityUserChanged = false;
		if (MaxQualityActive) {
			char MaxQualityFilename[MAX_PATH], UserFilename[MAX_PATH];
			ConfigPathOf(MaxQualityFilename, Paths::File::MaxQuality);
			ConfigPathOf(UserFilename, Paths::File::MaxQualityUser);
			try {
				MaxQualityConfig = toml::parse<toml::discard_comments, std::map>((std::string_view)MaxQualityFilename).as_table();
				Logger::Log("UNOFFICIAL Max Quality is ON: reduced-size effects run at full size, the settings in %s are used in place of yours (your file is not changed)", MaxQualityFilename);
			}
			catch (const std::exception& failure) {
				Logger::Log("[ERROR] Max Quality is on but %s could not be read (%s): reduced-size effects run at full size, your settings stay as they are", MaxQualityFilename, failure.what());
			}
			if (GetFileAttributesA(UserFilename) != INVALID_FILE_ATTRIBUTES) {
				try {
					MaxQualityUser = toml::parse<toml::discard_comments, std::map>((std::string_view)UserFilename).as_table();
					MergeTables(MaxQualityConfig, MaxQualityUser);
					Logger::Log("UNOFFICIAL Max Quality: your own changes to it read from %s", UserFilename);
				}
				catch (const std::exception& failure) {
					Logger::Log("[ERROR] %s could not be read (%s): Max Quality's own values are used", UserFilename, failure.what());
					MaxQualityUser = toml::table();
				}
			}
		}
		MaxQuality::Active = MaxQualityActive;
	}

	// Before anything reads a setting: the effects read theirs while they register, which is before the first
	// LoadSettings, and a read of a section the user config lacks writes the defaults into it -- so a migration that
	// fills a new section from the user's old one (P63's Interiors) would find it already there and skip it.
	// 2026-11-03 (InstallCheck): the version mark the file was saved with, before UpdateOldDefaults sets this build's
	int savedVersion = 0;
	if (TomlConfig.is_table() && TomlConfig.contains("_Unofficial") && TomlConfig.at("_Unofficial").is_table() &&
		TomlConfig.at("_Unofficial").contains("SettingsVersion") && TomlConfig.at("_Unofficial").at("SettingsVersion").is_integer())
		savedVersion = (int)TomlConfig.at("_Unofficial").at("SettingsVersion").as_integer();
	UpdateOldDefaults();
	ApplyRainWetnessSwitch();	// UNOFFICIAL 2026-11-20 (before anything reads Puddles' switch)
	const std::vector<std::string> unknown = WarnUnknownKeys(TomlConfig, DefaultConfig);
	const char* slash = strrchr(userFile, '\\');
	InstallCheck::OnSettingsRead(userFileRead, slash ? slash + 1 : userFile, savedVersion, UnofficialSettingsVersion, unknown);
}


/*
* Fills a node with one setting: the user's value when their config has it, else the default (the defaults also give the type
* and the description). False when the defaults do not have the setting.
*/
bool SettingManager::Configuration::FillNode(ConfigNode* node, const char* section, const char* key) {

	node->MaxQuality = false;	// UNOFFICIAL: before the defaults write-back below (a reused node must not route it to Max Quality)

	StringList steps;	// FindSection uses them up: made again for every lookup
	StorageKeys(section, &steps);
	tomlValue* userTable = FindSection(&TomlConfig, &steps);
	StorageKeys(section, &steps);
	tomlValue* defaultTable = FindSection(&DefaultConfig, &steps);
	if (!defaultTable || !defaultTable->is_table()) return false;	// not a setting of this build (or, 2026-11-17, a path onto a value)
	if (userTable && !userTable->is_table()) userTable = nullptr;	// a user file with a value where the section should be: the defaults
	// 2026-11-17: the node keeps the names in 40 bytes each and takes the part between the first two dots (below); a name that
	// does not fit that is not one of this build's settings (a hand-made preset, script or defaults file).
	{
		const char* firstDot = strchr(section, '.');
		if (strlen(section) >= sizeof(node->Section) || strlen(key) >= sizeof(node->Key) || !firstDot || !strchr(firstDot + 1, '.')) return false;
	}

	bool usedDefault = true;
	std::string text;	// the value as text: toml's own format, or a String setting's whole text
	try {
		const tomlValue fallback = defaultTable->at(key);	// a key the defaults lack throws out_of_range: not a setting
		tomlValue setting = fallback;
		if (userTable) {
			try {
				setting = userTable->at(key);
				usedDefault = false;	// the user's own value, not the default standing in
			}
			catch (const std::exception&) {
				Logger::Log("Setting %s.%s not found in user config, getting from defaults", section, key);
			}
		}

		const auto& notes = fallback.comments();
		node->Description = notes.empty() ? std::string() : notes.front();

		// The defaults decide the type: a user value of another type gives way to the default.
		auto requireType = [&](bool matches, const char* typeName) {
			if (matches) return;
			Logger::Log("Setting %s.%s has wrong type in user config: Should be %s.", section, key, typeName);
			setting = fallback;
		};

		if (fallback.is_integer()) {
			requireType(setting.is_integer(), "an int");
			node->Type = NodeType::Integer;
			text = toml::format(setting);
			StoreInt(node, (int)setting.as_integer());	// from toml's number, not from the text
		}
		else if (fallback.is_floating()) {
			// UNOFFICIAL 2026-10-28 (user: LodBias "accept decimal numbers"): a whole number typed into a float setting ("LodBias = -1")
			// is that number, not a wrong type put back to the default
			if (setting.is_integer()) setting = toml::value((double)setting.as_integer());
			requireType(setting.is_floating(), "a float");
			node->Type = NodeType::Float;
			text = toml::format(setting, 0, 5);
			StoreFloat(node, (float)setting.as_floating());
		}
		else if (fallback.is_boolean()) {
			requireType(setting.is_boolean(), "a bool");
			node->Type = NodeType::Boolean;
			StoreBool(node, setting.as_boolean());
			text = ToString<bool>(node->BoolValue);
		}
		else if (fallback.is_string()) {
			requireType(setting.is_string(), "a string");
			node->Type = NodeType::String;
			text = setting.as_string();
			node->LongValue = text;
		}
		else
			return false;	// a table or an array, not one setting
	}
	catch (const std::out_of_range&) {
		return false;
	}

	strcpy(node->Section, section);
	strcpy(node->Key, key);
	// UNOFFICIAL 2026-10-17: bounded (Value is 80 bytes: a keyword list of 80+ characters in [Shaders.Cloth.Main] overflowed it and the
	// game died with a stack-buffer-overrun fast fail right after the load); String settings keep their whole text in LongValue.
	PutNodeText(node->Value, text.c_str());
	if (node->Type != NodeType::String) node->LongValue.clear();
	node->Reboot = false;

	// MidSection: what lies between the first and the second dot (usually the effect's name)
	const char* midFrom = strchr(section, '.') + 1;
	const size_t midLength = strchr(midFrom, '.') - midFrom;
	memcpy(node->MidSection, midFrom, midLength);
	node->MidSection[midLength] = 0;

	// a default standing in is written into the user's table (UNOFFICIAL: always theirs, never Max Quality's)
	if (usedDefault) SetValueIn(TomlConfig, node);

	// UNOFFICIAL 2026-10-15 Max Quality: its value in place of the user's, after the write above so it never reaches their file.
	if (MaxQualityActive) {
		StorageKeys(section, &steps);
		if (tomlValue* layer = FindSection(&MaxQualityConfig, &steps); layer && layer->is_table()) {
			auto found = layer->as_table().find(key);
			if (found != layer->as_table().end()) {
				const tomlValue& forced = found->second;
				bool applied = true;
				if (node->Type == NodeType::Integer && forced.is_integer()) {
					StoreInt(node, (int)forced.as_integer());
					PutNodeText(node->Value, toml::format(forced).c_str());
				}
				else if (node->Type == NodeType::Float && (forced.is_floating() || forced.is_integer())) {
					StoreFloat(node, forced.is_floating() ? (float)forced.as_floating() : (float)forced.as_integer());
					strcpy(node->Value, ToString<float>(node->FloatValue).c_str());
				}
				else if (node->Type == NodeType::Boolean && forced.is_boolean()) {
					StoreBool(node, forced.as_boolean());
					strcpy(node->Value, ToString<bool>(node->BoolValue).c_str());
				}
				else applied = false;
				node->MaxQuality = applied;
			}
		}
	}

	return true;
}

/*
* The sub-sections of a section (of the top level when parent is NULL), as the defaults list them.
*/
void SettingManager::Configuration::FillSections(StringList* names, const char* parent) {

	tomlValue* table = &DefaultConfig;
	if (parent != NULL) {
		StringList steps;
		StorageKeys(parent, &steps);
		table = FindSection(&DefaultConfig, &steps);
		if (!table) return;	// no such section: the list is left as it was
	}

	names->clear();
	for (const auto& entry : table->as_table()) {
		if (!entry.second.is_table()) continue;	// settings, not sections
		const char* sectionName = entry.first.c_str();
		names->push_back(sectionName[0] == '_' ? sectionName + 1 : sectionName);	// shown without the storage "_"
	}
}

// Walks down from table along the steps and returns the table reached, or NULL when a step is missing. Every step taken is
// removed from the list, so callers make it again before the next walk.
tomlValue* SettingManager::Configuration::FindSection(tomlValue* table, StringList* keys) {
	for (tomlValue* level = table;; keys->erase(keys->begin())) {
		try {
			level = &level->at(keys->front().c_str());
		}
		catch (const std::exception&) {	// no such step, or (2026-11-17) a step that is a value, not a table: toml's type error went uncaught
			return nullptr;
		}
		if (keys->size() == 1) return level;
	}
}

/*
* The setting nodes of one section, as the defaults list them (what the menu shows).
*/
void SettingManager::Configuration::FillSettings(SettingList* nodes, const char* section) {

	StringList steps;
	StorageKeys(section, &steps);
	const tomlValue* table = FindSection(&DefaultConfig, &steps);
	if (!table) return;	// no such section

	nodes->clear();
	for (const auto& entry : table->as_table()) {
		if (entry.second.is_table()) continue;	// sub-sections are listed elsewhere
		ConfigNode node;
		if (FillNode(&node, section, entry.first.c_str())) nodes->push_back(node);
	}
}

// The user config's own table for a section such as "Main.Main.ReducedQuality" (nullptr if it has none).
tomlValue* SettingManager::Configuration::UserSection(const char* section) {
	StringList steps;
	StorageKeys(section, &steps);
	// 2026-11-17 (security review 2): only a table. A user file with a value where the section should be ([_Shaders] Bloom = 1)
	// came back as that value, and the migration's contains() on it threw toml's type error out of the plugin's load.
	tomlValue* found = FindSection(&TomlConfig, &steps);
	return found && found->is_table() ? found : nullptr;
}

// A saved value that equals a replaced default is moved to the new default once. The menu's Save writes every
// setting, so most saved configs hold the defaults of the build they were saved with, not the player's
// choice. The user config records how far it has been updated in [_Unofficial] SettingsVersion; that key is
// not in the defaults file, so the menu does not list it, and Save writes it with the rest. Configs without
// it (anything saved before P50) count as version 0. Called at the start of every LoadSettings; after the
// first call the version is current and it returns at once.
// (UnofficialSettingsVersion is declared above Configuration::Init, which hands it to the install check)
extern bool ParallaxLiteDistanceMigrationPending;  // effects/Terrain.cpp (version 65)
extern D3DXVECTOR4 NvrGameMultisample;	// UNOFFICIAL 2026-11-05 Hooks/Settings.cpp (TESR_GameMultisample; .z / .w set below)

// UNOFFICIAL 2026-11-20 deferred wetness step 3 (docs/DESIGN.md "Deferred wetness design" "Step 3 as built"): with [Main.Main.Quality] DeferredWetness on,
// rain wetness has one switch, Wetness Effects' (SettingManager::RainWetnessOneSwitch): Puddles' saved Enabled is set to it here, when the files
// are read and before anything reads it (the shaders' defines, the lighting-free records and the puddle map read the setting itself); the
// menu's Save keeps it. No default changes, so no settings version. (review 2026-11-20) Saved, Puddles keeps that value when DeferredWetness is
// turned off again; and it keys on the saved DeferredWetness alone, so with Wetness Effects off Puddles goes off too (it needs Wetness Effects anyway).
void SettingManager::Configuration::ApplyRainWetnessSwitch() {
	ConfigNode node;
	if (!FillNode(&node, "Main.Main.Quality", "DeferredWetness") || !node.BoolValue) return;
	const bool wet = FillNode(&node, "Shaders.WetnessEffects.Status", "Enabled") && node.BoolValue;
	if (FillNode(&node, "Shaders.Puddles.Status", "Enabled") && node.BoolValue == wet) return;
	tomlValue* saved = UserSection("Shaders.Puddles.Status");
	if (!saved || !saved->is_table()) {
		Logger::Log("[WARNING] UNOFFICIAL settings: DeferredWetness: [Shaders.Puddles.Status] is not in your settings file, so Puddles could not be switched %s "
			"with Wetness Effects this session (save the settings once from the menu)", wet ? "on" : "off");
		return;
	}
	(*saved)["Enabled"] = wet;
	Logger::Log("UNOFFICIAL settings: DeferredWetness: Puddles switched %s with Wetness Effects (one Rain wetness switch)", wet ? "on" : "off");
}

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
	constexpr int kUpstreamNewest = 101;	// the newest settings number upstream's builds write (1.1.5): bump it on the next upstream merge
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
	if (version < 109) {
		replace("Shaders.ShadowsExteriors.ShadowMaps", "CasterFadeMin", 0.75, 0.25, false);
		replace("Main.Main.RayTracing", "ArenaMB", 16.0, 32.0, true);
		replace("Main.Main.RayTracing", "ActorRebuildInterval", 8.0, 16.0, true);
	}
	if (!changed.empty())
		Logger::Log("UNOFFICIAL settings: old defaults saved by an earlier build moved to the new defaults: %s. "
			"Change them in the menu and press Save if you want the old values back.", changed.c_str());

	if (!TomlConfig.contains("_Unofficial") || !TomlConfig.at("_Unofficial").is_table()) TomlConfig["_Unofficial"] = toml::table();
	TomlConfig["_Unofficial"]["SettingsVersion"] = UnofficialSettingsVersion;
}

void SettingManager::Configuration::SetValue(ConfigNode* node) {
	// UNOFFICIAL 2026-10-15: a value Max Quality sets goes into its own layer (read at once, saved to the user's Max Quality
	// file), never into the user's settings.
	if (MaxQualityActive && (node->MaxQuality || IsMaxQualityKey(node->Section, node->Key))) {
		SetValueIn(MaxQualityConfig, node);
		SetValueIn(MaxQualityUser, node);
		MaxQualityUserChanged = true;
		return;
	}
	SetValueIn(TomlConfig, node);
}

// UNOFFICIAL 2026-10-15: whether Max Quality's list (shipped or the user's changes) holds this setting. By name, since the
// menu's checkbox, number and list writes make their node with CreateNode (no FillNode, so no MaxQuality flag).
bool SettingManager::Configuration::IsMaxQualityKey(const char* Section, const char* Key) {
	if (!MaxQualityActive) return false;
	StringList steps;
	StorageKeys(Section, &steps);
	tomlValue* section = FindSection(&MaxQualityConfig, &steps);
	return section && section->is_table() && section->as_table().count(Key) > 0;
}

// UNOFFICIAL 2026-10-15: From's values over Into's; a table in both is merged key by key.
void SettingManager::Configuration::MergeTables(tomlValue& Into, const tomlValue& From) {
	if (!Into.is_table() || !From.is_table()) return;
	for (const auto& [key, value] : From.as_table()) {
		if (value.is_table() && Into.contains(key) && Into.at(key).is_table()) MergeTables(Into.at(key), value);
		else Into[key] = value;
	}
}

void SettingManager::Configuration::SetValueIn(tomlValue& Target, ConfigNode* node) {

	StringList steps;
	StorageKeys(node->Section, &steps);
	tomlValue* table = FindSection(&Target, &steps);
	StorageKeys(node->Section, &steps);
	if (!FindSection(&DefaultConfig, &steps)) return;	// not a setting of this build: nothing is written

	// a section the target lacks is built level by level
	// 2026-11-17 (security review 2): a value where a table should be (a hand-made or downloaded file) is replaced by a table;
	// contains() and [] on a value threw toml's type error.
	if (!table || !table->is_table()) {
		StorageKeys(node->Section, &steps);
		table = &Target;
		if (!table->is_table()) return;
		for (const std::string& step : steps) {
			if (!table->contains(step) || !table->at(step).is_table()) (*table)[step] = toml::table();
			table = &table->at(step);
		}
	}

	// the node's typed fields hold the value (FillNode, CreateNode and SetSetting keep them current): the text is not parsed
	tomlValue& owner = *table;
	switch (node->Type) {
	case NodeType::Integer: owner[node->Key] = node->IntValue; break;
	case NodeType::Float: owner[node->Key] = node->FloatValue; break;
	case NodeType::Boolean: owner[node->Key] = node->BoolValue; break;
	case NodeType::String:
		// fixes-a #22: the whole text (Value is cut at 79 characters), not the cut one
		owner[node->Key] = node->LongValue.empty() ? std::string(node->Value) : node->LongValue;
		break;
	default:
		break;
	}
}


// Weather sections are not written to the toml any more: the weathers' values come from the game's data and from the defaults.
// The old text writer is gone; the function stays so that its callers keep working.
void SettingManager::Configuration::CreateWeatherSection(const char*, TESWeather*) {}

void SettingManager::Initialize() {

	Logger::Log("Starting the settings manager...");
	SettingManager* manager = new SettingManager();
	manager->GameLoading = false;
	manager->SettingsChanged = true;	// the first frame reads every effect's settings
	manager->hasUnsavedChanges = false;
	TheSettingManager = manager;
}

// A table of float settings of one struct: the toml key and the struct member it fills.
template <typename S>
struct FloatField {
	const char* key;
	float S::* field;
};


void SettingManager::LoadSettings() {

	SettingsRevision++;
	TimeLogger timer;
	Config.UpdateOldDefaults();

	StringList sectionNames;	// sub-section lists: the weathers, the coloring and water sets
	StringList colorTriples;	// one weather colour's "r,g,b" groups, one per time of day
	SettingsWeatherStruct SE{};	// one weather's values; kept across the weathers as before (every field is set each time)
	char text[80];

	if (Config.configLoaded == false) Config.Init();

	{
		const char* sec = "Main.Main.Water";
		SettingsMain.Main.RemoveUnderwater = GetSettingI(sec, "RemoveUnderwater");
	}
	{
		const char* sec = "Main.Main.Precipitations";
		SettingsMain.Main.RemovePrecipitations = GetSettingI(sec, "RemovePrecipitations");
	}
	{
		const char* sec = "Main.Main.Water";
		SettingsMain.Main.ForceReflections = GetSettingI(sec, "ForceReflections");
		SettingsMain.Main.GameShadersInReflections = GetSettingI(sec, "GameShadersInReflections") != 0;
		SettingsMain.Main.ReflectionClipFix = GetSettingI(sec, "ReflectionClipFix") != 0;
	}
	{
		const char* sec = "Main.Main.Memory";
		SettingsMain.Main.MemoryHeapManagement = GetSettingI(sec, "HeapManagement");
		SettingsMain.Main.MemoryTextureManagement = GetSettingI(sec, "TextureManagement");
	}
	{
		const char* sec = "Main.Main.Misc";
		SettingsMain.Main.AnisotropicFilter = GetSettingI(sec, "AnisotropicFilter");
		SettingsMain.Main.FarPlaneDistance = GetSettingF(sec, "FarPlaneDistance");
		SettingsMain.Main.ScreenshotKey = GetSettingI(sec, "ScreenshotKey");
		SettingsMain.Main.HDRScreenshot = GetSettingI(sec, "HDRScreenshot");
		SettingsMain.Main.ReplaceIntro = GetSettingI(sec, "ReplaceIntro");
		SettingsMain.Main.ForceMSAA = GetSettingI(sec, "ForceMSAA");
		SettingsMain.Main.SkipFog = GetSettingI(sec, "SkipFog");
		SettingsMain.Main.RenderEffects = GetSettingI(sec, "RenderEffects");
		SettingsMain.Main.RenderPreTonemapping = GetSettingI(sec, "RenderPreTonemapping");
	}
	// Settings only resolve when they exist in the defaults file (Configuration::FillNode), so these
	// live in [_Main.Main.Performance] / [_Main.Main.ReducedQuality] there and show up as switches in
	// the in-game menu. A key missing from an older defaults file reads as its fallback, so a
	// DLL-only update keeps the lossless ones on and the image-changing ones off.
	auto boolSetting = [this](const char* section, const char* key, bool fallback) {
		Configuration::ConfigNode node;
		return Config.FillNode(&node, section, key) ? node.BoolValue : fallback;
	};
	const char* performance = "Main.Main.Performance";
	SettingsMain.Main.DisableFrameChain = !boolSetting(performance, "FrameChain", true);
	SettingsMain.Main.DisableWorldSceneGuard = !boolSetting(performance, "WorldSceneGuard", true);
	SettingsMain.Main.MenuSceneGuard = boolSetting(performance, "MenuSceneGuard", true);
	SettingsMain.Main.DisableCompositeApply = !boolSetting(performance, "CompositeApply", true);
	SettingsMain.Main.DisableChainGameTexture = !boolSetting(performance, "ChainUsesGameTexture", true);
	// Off unless asked for: on native D3D9 (GTX 1070) the driver drops the two-target draw without
	// an error, leaving depth and normals stale.
	SettingsMain.Main.DisableMergedNormals = !boolSetting(performance, "MergedDepthNormals", false);
	SettingsMain.Main.SkipIdentityExposure = boolSetting(performance, "SkipIdentityExposure", true);
	// UNOFFICIAL 2026-10-23 (NVR DXVK, core\DxvkCompute.h): off by default; does nothing without the NVR DXVK extension.
	SettingsMain.Main.AvgLumaCompute = boolSetting(performance, "AvgLumaCompute", false);
	SettingsMain.Main.NvrDxvkPasses = boolSetting(performance, "NvrDxvkPasses", false);
	SettingsMain.Main.NvrDxvkVariants = boolSetting(performance, "NvrDxvkVariants", true);	// UNOFFICIAL 2026-10-26 (DxvkCompute::VariantsOn)
	SettingsMain.Main.NvrDxvkAmbientLight = boolSetting(performance, "NvrDxvkAmbientLight", false);	// UNOFFICIAL 2026-11-01 (AmbientLightEffect::ExtOn)
	SettingsMain.Main.NvrDxvkClouds = boolSetting(performance, "NvrDxvkClouds", false);	// UNOFFICIAL 2026-11-01 (VolumetricCloudsEffect::ExtRender)
	SettingsMain.Main.NvrDxvkStencil = boolSetting(performance, "NvrDxvkStencil", true);	// UNOFFICIAL 2026-10-26 (DxvkCompute::StencilOn)
	{	// UNOFFICIAL 2026-10-26 (DxvkCompute::KeptOnD3D9; a String setting keeps its whole text in LongValue)
		Configuration::ConfigNode node;
		const std::string keep = Config.FillNode(&node, performance, "NvrDxvkKeepD3D9") ? node.LongValue : std::string();
		strncpy_s(SettingsMain.Main.NvrDxvkKeepD3D9, keep.c_str(), _TRUNCATE);
		const std::string test = Config.FillNode(&node, performance, "NvrDxvkTestSuffix") ? node.LongValue : std::string();
		strncpy_s(SettingsMain.Main.NvrDxvkTestSuffix, test.c_str(), _TRUNCATE);
	}
	SettingsMain.Main.AmbientLightSmoothCompute = boolSetting(performance, "AmbientLightSmoothCompute", false);	// UNOFFICIAL 2026-10-25 (AmbientLight.cpp SmoothCompute)
	SettingsMain.Main.NvrDxvkReplacements = boolSetting(performance, "NvrDxvkReplacements", false);
	SettingsMain.Main.AmbientLightBlurCompute = boolSetting(performance, "AmbientLightBlurCompute", false);	// UNOFFICIAL 2026-10-25
	SettingsMain.Main.NvrSm6Shaders = boolSetting(performance, "NvrSm6Shaders", false);	// UNOFFICIAL 2026-10-26 (core\Sm6Pass.h)
	{
		Configuration::ConfigNode node;
		const int every = Config.FillNode(&node, performance, "AvgLumaComputeCheck") ? node.IntValue : 0;
		SettingsMain.Main.AvgLumaComputeCheck = max(0, min(every, 100000));
	}
	{
		// UNOFFICIAL 2026-10-23 (sharednormals): off by default (the image changes: smoother shading on low-poly ground and rocks).
		Configuration::ConfigNode node;
		const int reach = Config.FillNode(&node, "Main.Main.Quality", "SmoothNormals") ? node.IntValue : 0;
		SettingsMain.Main.SmoothNormals = max(0, min(reach, 256));
	}
	{
		// UNOFFICIAL 2026-11-05 alpha cutouts under the game's MSAA (Effects/Includes/MsaaSkyEdge.hlsl): TESR_GameMultisample.z = the fix on,
		// .w = the debug view (Hooks/Settings.cpp owns .x, core/SkyCoverage.h .y).
		Configuration::ConfigNode node;
		SettingsMain.Main.MsaaEdgeFix = boolSetting("Main.Main.Quality", "MsaaEdgeFix", true);
		const int view = Config.FillNode(&node, "Main.Main.Quality", "MsaaEdgeDebugView") ? node.IntValue : 0;
		SettingsMain.Main.MsaaEdgeDebugView = max(0, min(view, 1));
		NvrGameMultisample.z = SettingsMain.Main.MsaaEdgeFix ? 1.0f : 0.0f;
		NvrGameMultisample.w = (float)SettingsMain.Main.MsaaEdgeDebugView;
	}
	SettingsMain.Main.LampMaskScissor = boolSetting(performance, "LampMaskScissor", true);
	SettingsMain.Main.InteriorShadowFold = boolSetting(performance, "InteriorShadowFold", false);	// off: not bit-exact next to the void (intshadow-LOG.md)
	{
		Configuration::ConfigNode node;
		const int mode = Config.FillNode(&node, performance, "InteriorShadowFoldRound") ? node.IntValue : 0;
		SettingsMain.Main.InteriorShadowFoldRound = max(0, min(mode, 3));
	}
	SettingsMain.Main.CubeFaceCopySkip = boolSetting(performance, "CubeFaceCopySkip", false);
	SettingsMain.Main.CubeFaceViewCull = boolSetting(performance, "CubeFaceViewCull", true);
	SettingsMain.Main.SlimDepthBuffer = boolSetting(performance, "SlimDepthBuffer", true);
	SettingsMain.Main.SkipRedundantConstants = boolSetting(performance, "SkipRedundantConstants", true);
	SettingsMain.Main.BatchBoneUploads = boolSetting(performance, "BatchBoneUploads", true);
	SettingsMain.Main.FastConstantMapSort = boolSetting(performance, "FastConstantMapSort", true);
	SettingsMain.Main.SkinnedShadowFaceTest = boolSetting(performance, "SkinnedShadowFaceTest", true);
	SettingsMain.Main.ShaderWarmUp = boolSetting(performance, "ShaderWarmUp", false);
	SettingsMain.Main.HiddenObjectCulling = boolSetting(performance, "HiddenObjectCulling", false);
	SettingsMain.Main.PreDecodeCache = boolSetting(performance, "PreDecodeCache", false);	// UNOFFICIAL 2026-10-21 cpulimits
	SettingsMain.Main.FastCasterHash = boolSetting(performance, "FastCasterHash", false);
	SettingsMain.Main.SkyOnWorker = boolSetting(performance, "SkyOnWorker", false);	// UNOFFICIAL 2026-11-03 cpu-workers
	SettingsMain.Main.CasterListCache = boolSetting(performance, "CasterListCache", false);	// UNOFFICIAL 2026-10-30 ROOTPLAN round 7 step 18
	SettingsMain.Main.TrimLampPasses = boolSetting(performance, "TrimLampPasses", true);
	{
		Configuration::ConfigNode node;
		const int mode = Config.FillNode(&node, performance, "LampPassTestSkip") ? node.IntValue : 0;
		SettingsMain.Main.LampPassTestSkip = max(0, min(mode, 7));	// 4 (2026-10-16): also the passes after the highlights (fog)
	}
	{
		Configuration::ConfigNode node;
		SettingsMain.Main.NeonSpillSelfTest = Config.FillNode(&node, "Shaders.BounceLight.Main", "NeonSpillSelfTest") ? node.BoolValue : false;
	}
	SettingsMain.Main.LampScissor = boolSetting(performance, "LampScissor", true);
	SettingsMain.Main.LampScissorExact = boolSetting(performance, "LampScissorExact", true);
	SettingsMain.Main.ReShadeDepthCompat = boolSetting(performance, "ReShadeDepthCompat", false);
	SettingsMain.Main.ReShadeNVRDepth = boolSetting(performance, "ReShadeNVRDepth", true);
	SettingsMain.Main.ReShadeBeforeHUD = boolSetting(performance, "ReShadeBeforeHUD", true);
	SettingsMain.Main.ReShadeUnderNVRMenu = boolSetting(performance, "ReShadeUnderNVRMenu", true);
	SettingsMain.Main.ReShadeDepthNearRemap = boolSetting(performance, "ReShadeDepthNearRemap", true);
	SettingsMain.Main.ReShadeMenuGuard = boolSetting(performance, "ReShadeMenuGuard", false);
	SettingsMain.Main.ReShadeMenuGuardVATS = boolSetting(performance, "ReShadeMenuGuardVATS", false);
	SettingsMain.Main.ReShadeGameValues = boolSetting(performance, "ReShadeGameValues", true);
	{
		Configuration::ConfigNode node;	// a String setting: the whole text is in LongValue (text is cut at 79 characters)
		ReShadeIntegration::SetMenuGuardEffects(Config.FillNode(&node, performance, "ReShadeMenuGuardEffects") ? (node.LongValue.empty() ? node.Value : node.LongValue.c_str()) : "");
	}
	{
		Configuration::ConfigNode node;
		const int age = Config.FillNode(&node, performance, "HiddenObjectCullingMaxAge") ? node.IntValue : 4;
		SettingsMain.Main.HiddenObjectCullingMaxAge = max(1, min(age, 7));
	}
	const char* reducedQuality = "Main.Main.ReducedQuality";
	SettingsMain.Main.GodRaysLowRes = boolSetting(reducedQuality, "GodRaysLowRes", false);
	SettingsMain.Main.AOLowRes = boolSetting(reducedQuality, "AOLowRes", false);
	SettingsMain.Main.FXAA = boolSetting(reducedQuality, "FXAA", false);
	{
		Configuration::ConfigNode node;
		const int interval = Config.FillNode(&node, reducedQuality, "PointShadowInterval") ? node.IntValue : 1;
		SettingsMain.Main.PointShadowInterval = max(1, min(interval, 4));
		const float tolerance = Config.FillNode(&node, reducedQuality, "LampShadowMoveTolerance") ? node.FloatValue : 0.0f;
		SettingsMain.Main.LampShadowMoveTolerance = tolerance > 64.0f ? 64.0f : tolerance > 0.0f ? tolerance : 0.0f;
	}
	SettingsMain.Main.CheapReflections = boolSetting(reducedQuality, "CheapReflections", false);
	SettingsMain.Main.CheapUnderwaterTerrain = boolSetting(reducedQuality, "CheapUnderwaterTerrain", false);
	SettingsMain.Main.ParallaxLite = boolSetting(reducedQuality, "ParallaxLite", false);
	SettingsMain.Main.StaggeredSunShadows = boolSetting(reducedQuality, "StaggeredSunShadows", false);
	SettingsMain.Main.CachedDistantShadows = boolSetting(reducedQuality, "CachedDistantShadows", false);
	{
		Configuration::ConfigNode node;
		const int interval = Config.FillNode(&node, reducedQuality, "NearCascadeInterval") ? node.IntValue : 1;
		SettingsMain.Main.NearCascadeInterval = max(1, min(interval, 2));
	}
	{
		const char* sec = "Main.Main.Misc";
		SettingsMain.Main.PresetManagerEnabled = GetSettingI(sec, "PresetManagerEnabled");
	}

	{
		const char* sec = "Main.FrameRate.SmartControl";
		SettingsMain.FrameRate.SmartControl = GetSettingI(sec, "SmartControl");
		SettingsMain.FrameRate.SmartControlFPS = GetSettingI(sec, "SmartControlFPS");
		SettingsMain.FrameRate.FlowControl = GetSettingF(sec, "FlowControl");
	}
	{
		const char* sec = "Main.FrameRate.Stuttering";
		SettingsMain.FrameRate.SmartBackgroundProcess = GetSettingI(sec, "SmartBackgroundProcess");
		SettingsMain.FrameRate.BackgroundThreadPriority = GetSettingI(sec, "BackgroundThreadPriority");
	}

	{
		const char* sec = "Main.CullingProcess.Main";
		SettingsMain.CullingProcess.EnableCulling = GetSettingI(sec, "EnableCulling");
		SettingsMain.CullingProcess.EnableReflectionCulling = GetSettingI(sec, "EnableReflectionCulling");
		SettingsMain.CullingProcess.CullMinSize = GetSettingF(sec, "CullMinSize");
		SettingsMain.CullingProcess.CullMinSizeReflection = GetSettingF(sec, "CullMinSizeReflection");
	}

	{
		const char* sec = "Main.CameraMode.Main";
		SettingsMain.CameraMode.Enabled = GetSettingI(sec, "Enabled");
		SettingsMain.CameraMode.Crosshair = GetSettingI(sec, "Crosshair");
		SettingsMain.CameraMode.ChasingFirst = GetSettingI(sec, "ChasingFirst");
		SettingsMain.CameraMode.ChasingThird = GetSettingI(sec, "ChasingThird");
		SettingsMain.CameraMode.FoV = GetSettingF(sec, "FoV");
	}
	{
		const char* sec = "Main.CameraMode.Positioning";
		SettingsMain.CameraMode.Offset.x = GetSettingF(sec, "OffsetX");
		SettingsMain.CameraMode.Offset.y = GetSettingF(sec, "OffsetY");
		SettingsMain.CameraMode.Offset.z = GetSettingF(sec, "OffsetZ");
		SettingsMain.CameraMode.DialogOffset.x = GetSettingF(sec, "DialogOffsetX");
		SettingsMain.CameraMode.DialogOffset.y = GetSettingF(sec, "DialogOffsetY");
		SettingsMain.CameraMode.DialogOffset.z = GetSettingF(sec, "DialogOffsetZ");
		SettingsMain.CameraMode.AimingOffset.x = GetSettingF(sec, "AimingOffsetX");
		SettingsMain.CameraMode.AimingOffset.y = GetSettingF(sec, "AimingOffsetY");
		SettingsMain.CameraMode.AimingOffset.z = GetSettingF(sec, "AimingOffsetZ");
	}

	{
		const char* sec = "Main.WeatherMode.Main";
		SettingsMain.WeatherMode.Enabled = GetSettingI(sec, "Enabled");
	}
	{
		const char* sec = "Main.WeatherMode.General";
		SettingsMain.WeatherMode.CoeffNight.x = GetSettingF(sec, "CoeffNightR");
		SettingsMain.WeatherMode.CoeffNight.y = GetSettingF(sec, "CoeffNightG");
		SettingsMain.WeatherMode.CoeffNight.z = GetSettingF(sec, "CoeffNightB");
		SettingsMain.WeatherMode.CoeffFog.x = GetSettingF(sec, "CoeffFogR");
		SettingsMain.WeatherMode.CoeffFog.y = GetSettingF(sec, "CoeffFogG");
		SettingsMain.WeatherMode.CoeffFog.z = GetSettingF(sec, "CoeffFogB");
		SettingsMain.WeatherMode.CoeffSun.x = GetSettingF(sec, "CoeffSunR");
		SettingsMain.WeatherMode.CoeffSun.y = GetSettingF(sec, "CoeffSunG");
		SettingsMain.WeatherMode.CoeffSun.z = GetSettingF(sec, "CoeffSunB");
	}

	{
		// 2026-11-05: only what the ImGui menu reads (the old text menu's fonts, colours, layout and keys are gone)
		SettingsMain.Menu.TextSize = GetSettingI("Main.Menu.Style", "TextSize");
		SettingsMain.Menu.KeyEnable = GetSettingI("Main.Menu.Keys", "KeyEnable");
		SettingsMain.Menu.MasterSwitchModifier = GetSettingI("Main.Menu.Keys", "MasterSwitchModifier");
	}

	{
		const char* sec = "Main.FlyCam.Main";
		SettingsMain.FlyCam.Enabled = GetSettingI(sec, "Enabled");
		SettingsMain.FlyCam.ScrollMultiplier = GetSettingF(sec, "ScrollMultiplier");
		SettingsMain.FlyCam.KeyAdd = GetSettingI(sec, "KeyAdd");
		SettingsMain.FlyCam.KeySubtract = GetSettingI(sec, "KeySubtract");
		SettingsMain.FlyCam.StepValue = GetSettingF(sec, "StepValue");
	}

	{
		const char* sec = "Main.Develop.Main";
		SettingsMain.Develop.DebugMode = GetSettingI(sec, "DebugMode");
		SettingsMain.Develop.TraceShaders = GetSettingI(sec, "TraceShaders");
	}
	// UNOFFICIAL 2026-10-26 (GpuDebug.h, docs/next/FORK-DEBUG-CAPTURE.md): capture/debug tools, all off by default.
	GpuDebug::Labels = GetSettingI("Main.Develop.Main", "GpuDebugLabels") != 0;
	GpuDebug::CaptureKey = GetSettingI("Main.Develop.Main", "RenderDocCaptureKey");
	GpuDebug::CaptureFrames = GetSettingI("Main.Develop.Main", "RenderDocCaptureFrames");
	if (GpuDebug::CaptureFrames < 1) GpuDebug::CaptureFrames = 1;
	if (GpuDebug::CaptureFrames > 10) GpuDebug::CaptureFrames = 10;

	{
		const char* sec = "Main.Transitions.Main";
		SettingsMain.Transitions.TransitionCurvePower = GetSettingF(sec, "TransitionCurvePower");
	}


	Config.FillSections(&sectionNames, "Weathers"); // the weathers: their colours, HDR list and Main values
	for (const std::string& weatherName : sectionNames) {
		const std::string weatherPath = "Weathers." + weatherName;
		const std::string colorsPath = weatherPath + ".Colors";
		UInt8 rgb[3];
		for (int colorType = 0; colorType < TESWeather::kNumColorTypes; ++colorType) {
			char colorKey[12];
			sprintf(colorKey, "Color%i", colorType);
			SplitString(GetSettingS(colorsPath.c_str(), colorKey, text), " ", &colorTriples);
			const int loadedTimes = (int)(std::min)(colorTriples.size(), (size_t)TESWeather::kNumTimeOfDay);
			SE.ColorTimeCount[colorType] = (UInt8)loadedTimes;
			for (int when = 0; when < loadedTimes; ++when) {
				rgb[0] = rgb[1] = rgb[2] = 0;
				FillFromString(colorTriples[when].c_str(), ",", rgb);
				auto& color = SE.Colors[colorType].colors[when];
				color.r = rgb[0];
				color.g = rgb[1];
				color.b = rgb[2];
			}
			for (int when = loadedTimes; when < TESWeather::kNumTimeOfDay; ++when) {
				SE.Colors[colorType].colors[when] = RGBA{};
			}
		}
		FillFromString(GetSettingS((weatherPath + ".HDR").c_str(), "HDR", text), " ", SE.HDR);

		const std::string mainPath = weatherPath + ".Main";
		const char* mainSection = mainPath.c_str();
		SE.FogNearDay = GetSettingF(mainSection, "FogNearDay");
		SE.FogNearNight = GetSettingF(mainSection, "FogNearNight");
		SE.FogFarDay = GetSettingF(mainSection, "FogFarDay");
		SE.FogFarNight = GetSettingF(mainSection, "FogFarNight");
		SE.WindSpeed = GetSettingI(mainSection, "WindSpeed");
		SE.CloudSpeedLower = GetSettingI(mainSection, "CloudSpeedLower");
		SE.CloudSpeedUpper = GetSettingI(mainSection, "CloudSpeedUpper");
		SE.TransDelta = GetSettingI(mainSection, "TransDelta");
		SE.SunGlare = GetSettingI(mainSection, "SunGlare");
		SE.SunDamage = GetSettingI(mainSection, "SunDamage");
		strcpy(SE.LowerLayer, GetSettingS(mainSection, "LowerLayer", text));
		strcpy(SE.UpperLayer, GetSettingS(mainSection, "UpperLayer", text));
		SettingsWeather[weatherName] = SE;
	}

	// the coloring and the water sections: each float setting is read by its key, in the order of the defaults
	static const FloatField<SettingsColoringStruct> coloringFields[] = {
		{ "Strength", &SettingsColoringStruct::Strength },
		{ "BaseGamma", &SettingsColoringStruct::BaseGamma },
		{ "Fade", &SettingsColoringStruct::Fade },
		{ "Contrast", &SettingsColoringStruct::Contrast },
		{ "Saturation", &SettingsColoringStruct::Saturation },
		{ "Bleach", &SettingsColoringStruct::Bleach },
		{ "BleachLuma", &SettingsColoringStruct::BleachLuma },
		{ "ColorCurve", &SettingsColoringStruct::ColorCurve },
		{ "ColorCurveR", &SettingsColoringStruct::ColorCurveR },
		{ "ColorCurveG", &SettingsColoringStruct::ColorCurveG },
		{ "ColorCurveB", &SettingsColoringStruct::ColorCurveB },
		{ "EffectGamma", &SettingsColoringStruct::EffectGamma },
		{ "EffectGammaR", &SettingsColoringStruct::EffectGammaR },
		{ "EffectGammaG", &SettingsColoringStruct::EffectGammaG },
		{ "EffectGammaB", &SettingsColoringStruct::EffectGammaB },
		{ "Linearization", &SettingsColoringStruct::Linearization },
	};
	static const FloatField<SettingsWaterStruct> waterFields[] = {
		{ "choppiness", &SettingsWaterStruct::choppiness },
		{ "waveWidth", &SettingsWaterStruct::waveWidth },
		{ "waveSpeed", &SettingsWaterStruct::waveSpeed },
		{ "reflectivity", &SettingsWaterStruct::reflectivity },
		{ "causticsStrength", &SettingsWaterStruct::causticsStrength },
		{ "causticsStrengthS", &SettingsWaterStruct::causticsStrengthS },
		{ "shoreFactor", &SettingsWaterStruct::shoreFactor },
		{ "shoreMovement", &SettingsWaterStruct::shoreMovement },
		{ "turbidity", &SettingsWaterStruct::turbidity },
		{ "inScattCoeff", &SettingsWaterStruct::inScattCoeff },
		{ "inExtCoeff_R", &SettingsWaterStruct::inExtCoeff_R },
		{ "inExtCoeff_G", &SettingsWaterStruct::inExtCoeff_G },
		{ "inExtCoeff_B", &SettingsWaterStruct::inExtCoeff_B },
		{ "depthDarkness", &SettingsWaterStruct::depthDarkness },
	};

	Config.FillSections(&sectionNames, "Shaders.Coloring");
	for (const std::string& name : sectionNames) {
		if (name == "Status") continue;
		const std::string section = "Shaders.Coloring." + name;
		SettingsColoringStruct coloring{};
		for (const auto& f : coloringFields) coloring.*(f.field) = GetSettingF(section.c_str(), f.key);
		SettingsColoring[name] = coloring;
	}

	Config.FillSections(&sectionNames, "Shaders.Water");
	for (const std::string& name : sectionNames) {
		if (name == "Status") continue;
		const std::string section = "Shaders.Water." + name;
		SettingsWaterStruct water{};
		for (const auto& f : waterFields) water.*(f.field) = GetSettingF(section.c_str(), f.key);
		SettingsWater[name] = water;
	}

	SettingsChanged = true;
	timer.LogTime("SettingsManager::LoadSettings");

	if (GameMenuManager* menu = TheGameMenuManager) menu->ValidateSelection();	// the selected row may be gone now
}


/*
* The FormIDs listed as sub-sections (hex names) of a section, sorted.
*/
void SettingManager::GetFormList(const char* sectionName, FormsList* forms) {
	StringList names;
	Config.FillSections(&names, sectionName);
	if (names.empty()) return;
	forms->reserve(names.size());
	for (const std::string& hex : names) forms->push_back(strtol(hex.c_str(), NULL, 16));
	std::sort(forms->begin(), forms->end());
}

// Writes a toml document over a file (the settings, Save As and Max Quality's own changes), after an optional header.
static void WriteTomlFile(const char* path, const tomlValue& document, const char* header = nullptr) {
	std::ofstream out(path, std::ios::trunc | std::ios::binary);
	if (header) out << header;
	out << document << std::endl;
}

/*
* Saves the settings to the settings file.
*/
void SettingManager::SaveSettings() {

	char settingsFile[MAX_PATH];
	ConfigPathOf(settingsFile, Paths::File::UserSettings, true);
	WriteTomlFile(settingsFile, Config.TomlConfig);

	// UNOFFICIAL 2026-10-15: the user's own changes to Max Quality's values, in their own file
	if (Config.MaxQualityUserChanged) {
		char changesFile[MAX_PATH];
		ConfigPathOf(changesFile, Paths::File::MaxQualityUser, true);
		WriteTomlFile(changesFile, Config.MaxQualityUser,
			"# Vegas Shaders: your own changes to Max Quality's values, made in the menu while it was on.\n"
			"# Read over VegasShaders.maxquality.toml. Delete this file to go back to Max Quality's own values.\n");
		Config.MaxQualityUserChanged = false;
	}

	hasUnsavedChanges = false;
	UnsavedSettings::Clear();	// 2026-11-25 (unsaved marks)
	SessionLog::OnEvent("Vegas Shaders settings saved to the toml");	// UNOFFICIAL 2026-11-01
}

void SettingManager::RevertSettings() {
	SessionLog::OnEvent("Vegas Shaders settings reverted to the saved toml");	// UNOFFICIAL 2026-11-01
	Config.Init();	// both files read again
	LoadSettings();

	// The shaders' runtime Enabled flags are only changed by SwitchShaderStatus, not by UpdateSettings(), so after a reload
	// they are brought back in line with the settings here.
	StringList names;
	FillMenuSections(&names, "Shaders");
	for (const auto& name : names) {
		const bool want = GetMenuShaderEnabled(name.c_str());
		if (EffectRecord* effect = TheShaderManager->GetEffectByName(name.c_str())) {
			effect->Enabled = want;
			continue;
		}
		if (ShaderCollection* collection = TheShaderManager->GetShaderCollectionByName(name.c_str())) collection->Enabled = want;
	}

	hasUnsavedChanges = false;	// what is live is what is on disk again
	UnsavedSettings::Clear();	// 2026-11-25 (unsaved marks)
}

void SettingManager::SaveSettingsTo(const char* path) {
	WriteTomlFile(path, Config.TomlConfig);
}

// UNOFFICIAL 2026-10-30 SettingsCache ([Main.Main.Performance], live; ROOTPLAN round 7 step 18): effects read their settings with these
// every frame, and each read went through Configuration::FillNode: the section path split twice, two toml trees walked, the values
// (comments and all) copied and formatted -- about 2 % of the main thread in Goodsprings (CPU sampler, FillNode / SplitString / the toml
// map). The result of each (section, key) is kept until any setting changes (SettingsRevision: every SetSetting and LoadSettings, so
// also the menu, Max Quality and the test shots); a read then returns exactly what FillNode would. Off: every read as before. A lock,
// for the loader threads that read settings too.
namespace {
	struct CachedSetting { bool ok; int i; float f; std::string s; };
	std::unordered_map<std::string, CachedSetting> SettingCache;
	std::mutex SettingCacheLock;
	UInt32 SettingCacheRevision = 0xFFFFFFFFu;
	bool SettingCacheOn = false;
}

static const CachedSetting* CachedNode(SettingManager* manager, const char* section, const char* key, CachedSetting& scratch) {
	auto fill = [&](CachedSetting& c) {
		SettingNode node;
		c.ok = manager->Config.FillNode(&node, section, key);
		c.i = c.ok ? node.IntValue : 0; c.f = c.ok ? node.FloatValue : 0.0f; c.s = c.ok ? node.Value : "";
	};
	if (SettingCacheRevision != manager->SettingsRevision) {	// a setting changed (or the first read): the switch again, the cache emptied
		SettingNode node;
		SettingCacheOn = !manager->Config.FillNode(&node, "Main.Main.Performance", "SettingsCache") || node.BoolValue;
		SettingCache.clear();
		SettingCacheRevision = manager->SettingsRevision;
	}
	if (!SettingCacheOn) { fill(scratch); return &scratch; }
	static std::string cacheKey;	// one buffer for every read: the lock is held, so a read allocates nothing
	cacheKey.assign(section); cacheKey += '\x1f'; cacheKey += key;
	auto slot = SettingCache.find(cacheKey);
	if (slot == SettingCache.end()) { CachedSetting c; fill(c); slot = SettingCache.emplace(cacheKey, std::move(c)).first; }
	return &slot->second;
}

// UNOFFICIAL 2026-11-14 full deferred renderer (core/SettingForce.h): the prerequisites [Main.Main.Quality] DeferredRenderer turns on read as
// forced while it is on at startup (decided at the session's first read; nothing is written into the settings). Off: State 0, nothing more.
static bool ForcedValue(SettingManager* manager, const char* section, const char* key, int& value) {
	if (SettingForce::State < 0) {
		CachedSetting scratch;
		const CachedSetting* mode = CachedNode(manager, "Main.Main.Quality", "DeferredRenderer", scratch);
		if (!mode->ok) {	// a key every defaults file has: missing too = the settings are not loaded yet (decide at a later read)
			CachedSetting other;
			if (!CachedNode(manager, "Main.Main.Quality", "CapturedNormals", other)->ok) return false;
		}
		const int m = mode->ok ? mode->i : 0;
		SettingForce::RendererMode = m < 0 ? 0 : m > 2 ? 2 : m;
		SettingForce::State = SettingForce::RendererMode > 0 ? 1 : 0;
		// (logged by DeferredRenderer::BeginFrame, not here: under the cache lock)
	}
	return SettingForce::State == 1 && SettingForce::Find(section, key, value);
}

int SettingManager::GetSettingI(const char* section, const char* key) {
	std::lock_guard<std::mutex> lock(SettingCacheLock);
	CachedSetting scratch;
	if (SettingForce::DxvkRequiredOff(section, key)) return 0;	// UNOFFICIAL 2026-11-20: FSR / DLSS / HDR off when DXVK fork is absent
	if (SettingForce::State) { int forced; if (ForcedValue(this, section, key, forced)) return forced; }
	const CachedSetting* node = CachedNode(this, section, key, scratch);
	if constexpr (ShelvedEffects::Active) { if (ShelvedEffects::ForcedOff(section, key, node->i)) return 0; }	// UNOFFICIAL 2026-11-16 (release builds only)
	return node->i;	// the typed value directly, no string re-parse (0 when the setting does not exist)
}

float SettingManager::GetSettingF(const char* section, const char* key) {
	std::lock_guard<std::mutex> lock(SettingCacheLock);
	CachedSetting scratch;
	if (SettingForce::DxvkRequiredOff(section, key)) return 0.0f;	// UNOFFICIAL 2026-11-20: FSR / DLSS / HDR off when DXVK fork is absent
	if (SettingForce::State) { int forced; if (ForcedValue(this, section, key, forced)) return (float)forced; }	// UNOFFICIAL 2026-11-14 (above)
	float value = CachedNode(this, section, key, scratch)->f;
	if constexpr (ShelvedEffects::Active) { if (ShelvedEffects::ForcedOff(section, key, value != 0.0f)) return 0.0f; }	// UNOFFICIAL 2026-11-16 (release builds only)
	if (SettingForce::State == 1) SettingForce::FloorF(section, key, value);	// UNOFFICIAL 2026-11-15 linear gamma floor (SettingForce.h)
	return value;
}

char* SettingManager::GetSettingS(const char* section, const char* key, char* out) {
	std::lock_guard<std::mutex> lock(SettingCacheLock);
	CachedSetting scratch;
	const CachedSetting* found = CachedNode(this, section, key, scratch);
	strcpy(out, found->ok ? found->s.c_str() : "\"");	// a setting that does not exist reads as a lone quote
	return out;
}


// Interiors read <section>.Interiors; exteriors blend <section>.Night into <section>.Main by the day factor.
float SettingManager::GetSettingTransition(const char* section, const char* key, bool isExterior, float dayFactor) {
	auto sub = [section](char (&dest)[50], const char* suffix) -> const char* {
		strcpy(dest, section);
		strcat(dest, suffix);
		return dest;
	};
	char day[50], night[50];
	if (!isExterior) return GetSettingF(sub(day, ".Interiors"), key);
	sub(day, ".Main");
	sub(night, ".Night");
	return std::lerp(GetSettingF(night, key), GetSettingF(day, key), dayFactor);
}


// The typed SetSetting overloads: a node of that type, then the common path.
template <typename T>
static void SetTypedSetting(SettingManager* manager, const char* section, const char* key, T value) {
	SettingNode node;
	manager->CreateNode(&node, section, key, value, false);
	manager->SetSetting(&node);
}

void SettingManager::SetSetting(const char* section, const char* key, int value) {
	SetTypedSetting(this, section, key, value);
}

void SettingManager::SetSetting(const char* section, const char* key, bool value) {
	SetTypedSetting(this, section, key, value);
}

void SettingManager::SetSetting(const char* section, const char* key, float value) {
	SetTypedSetting(this, section, key, value);
}

/*
* Sets a setting from text, whatever its type.
*/
// 2026-11-02: a setting this build does not have, set by a script (NVRSetSetting / NVRSetSettingS from another mod such as
// Cartographer, or a preset made for New Vegas Reloaded) or the console. The node was used unfilled before (its Section and Key
// never written: garbage text into fixed buffers, a possible crash), as in the original mod. Now ignored, logged once per key.
static void IgnoreUnknownSetting(const char* section, const char* key, const char* how) {
	static std::unordered_set<std::string> logged;
	if (logged.size() < 500 && logged.insert(std::string(section) + "." + key).second)
		Logger::Log("UNOFFICIAL settings: %s %s.%s: not a setting of this build, ignored (a script or preset made for another version?)", how, section, key);
}

void SettingManager::SetSettingS(const char* section, const char* key, const char* value) {

	using Kind = Configuration::NodeType;
	SettingNode node;
	if (!Config.FillNode(&node, section, key)) { IgnoreUnknownSetting(section, key, "SetSettingS"); return; }
	// bounded copy (Value is 80 bytes; a preset or console string of 80+ characters overflowed it). The whole text of a String
	// setting goes to LongValue, which is what the config file and the String readers use.
	PutNodeText(node.Value, value);
	if (node.Type == Kind::String) node.LongValue = value;

	// The one place that parses text into the node's own type (a snapshotted value to revert to, or a real String setting): the
	// typed field must match the text, since Config.SetValue stores the typed field.
	if (node.Type == Kind::Integer)
		node.IntValue = atoi(value);
	else if (node.Type == Kind::Float)
		node.FloatValue = (float)atof(value);
	else if (node.Type == Kind::Boolean)	// 2026-11-05: "true" / "false" too (atoi("true") is 0: the menu's simple choices wrote
		node.BoolValue = !_stricmp(value, "true") || (_stricmp(value, "false") && atoi(value) != 0);	// "...=true" and got false)

	SetSetting(&node);
}

/*
* Sets a setting from a number, whatever its type.
*/
void SettingManager::SetSettingF(const char* section, const char* key, float value) {

	using Kind = Configuration::NodeType;
	SettingNode node;
	if (!Config.FillNode(&node, section, key)) { IgnoreUnknownSetting(section, key, "SetSetting"); return; }	// the type comes from the defaults

	// the number goes into the node's own type: its typed field and its text together
	if (node.Type == Kind::Boolean) {
		const bool on = value != 0.0f;
		// [Shaders.<Name>.Status] Enabled also switches the effect itself
		const bool statusSwitch = !memcmp(section, "Shaders", 7) && !memcmp(section + strlen(section) - 6, "Status", 6) && !memcmp(key, "Enabled", 7);
		// (2026-11-26 al-menu) AmbientLight while SunGI or RoomGI holds it on: the key alone is written, the effect is not switched (presets and
		// NVRSetSetting scripts keep the designed "SunGI on, switch off" state, outdoors only: AmbientLightEffect::UpdateSettings keeps it held on).
		// Switching would have flipped the running effect (1 turned it off) and, through SetMenuShaderEnabled, cleared SunGI and RoomGI: that is
		// the menu's click only.
		bool held = false;
		if (statusSwitch && !strcmp(node.MidSection, "AmbientLight")) {
			SettingNode gi;
			held = (Config.FillNode(&gi, "Shaders.AmbientLight.Main", "SunGI") && gi.BoolValue) || (Config.FillNode(&gi, "Shaders.AmbientLight.Main", "RoomGI") && gi.BoolValue);
		}
		if (statusSwitch && !held && node.BoolValue != on && (on || !IsShaderForced(node.MidSection)))
			TheShaderManager->SwitchShaderStatus(node.MidSection);
		node.BoolValue = on;
		strcpy(node.Value, ToString<bool>(on).c_str());
	}
	else if (node.Type == Kind::Integer) {
		node.IntValue = (int)value;
		strcpy(node.Value, ToString<int>(node.IntValue).c_str());
	}
	else if (node.Type == Kind::Float) {
		node.FloatValue = value;
		strcpy(node.Value, ToString<float>(value).c_str());
	}

	SetSetting(&node);
	LoadSettings(); // the settings structs read again
}



// UNOFFICIAL 2026-11-01: a node's value as the SETTING log lines show it
static std::string LogValue(const SettingNode& node) {
	typedef SettingManager::Configuration::NodeType Type;
	char text[64];
	switch (node.Type) {
	case Type::Boolean: return node.BoolValue ? "true" : "false";
	case Type::Integer: sprintf_s(text, "%d", node.IntValue); return text;
	case Type::Float: sprintf_s(text, "%g", node.FloatValue); return text;
	default: {
		std::string s = node.LongValue.empty() ? std::string(node.Value) : node.LongValue;
		if (s.size() > 60) s = s.substr(0, 57) + "...";
		return "\"" + s + "\"";
	}
	}
}

// 2026-11-25 (unsaved marks): a node's whole value as text (LogValue cuts long strings and rounds floats), so UnsavedSettings can
// tell a setting that is back at its saved value from one that is not
static tomlValue s_peekMaxQuality, s_peekMaxQualityUser;	// 2026-11-26 (unsaved marks): PeekSavedBegin's copies
static bool s_peekMaxQualityChanged = false;

static std::string WholeValue(const SettingNode& node) {
	typedef SettingManager::Configuration::NodeType Type;
	char text[32];
	switch (node.Type) {
	case Type::Boolean: return node.BoolValue ? "true" : "false";
	case Type::Integer: sprintf_s(text, "%d", node.IntValue); return text;
	case Type::Float: sprintf_s(text, "%.9g", node.FloatValue); return text;
	default: return node.LongValue.empty() ? std::string(node.Value) : node.LongValue;
	}
}

/*
* Stores a node's value in the settings (the user's table, or Max Quality's layer).
*/
void SettingManager::SetSetting(Configuration::ConfigNode* node) {
	SettingsRevision++;

	// UNOFFICIAL 2026-11-01: every change goes to the log as old -> new (SessionLog: coalesced while a slider is dragged, rate-limited)
	SettingNode before;
	const bool known = Config.FillNode(&before, node->Section, node->Key);
	Config.SetValue(node);
	if (known) SessionLog::OnSettingChanged(node->Section, node->Key, LogValue(before).c_str(), LogValue(*node).c_str());
	if (known) UnsavedSettings::Changed(node->Section, node->Key, WholeValue(before), WholeValue(*node));	// 2026-11-25 (unsaved marks)
	else UnsavedSettings::Changed(node->Section, node->Key, UnsavedSettings::kNoValue, WholeValue(*node));	// 2026-11-25: not a setting of this build: unsaved until Save
	// 2026-11-25: was "true" on every change, so a setting changed and changed back (nothing to save) kept the menu bar's "Unsaved",
	// the amber Save and the status bar's "Unsaved changes" until Save. Now they say what the unsaved list says.
	hasUnsavedChanges = UnsavedSettings::Any();
}

// 2026-11-26 (unsaved marks): the unsaved settings at their saved values, written straight into the tables (Config.SetValue, as
// SetSetting does, but with no log, no revision and no unsaved list), so the menu can ask a mode picker what it showed when last
// saved. Max Quality's two tables and its changed flag are kept as they were (SetValue writes into both while it is on).
std::vector<SettingManager::Configuration::ConfigNode> SettingManager::PeekSavedBegin() {
	using Kind = Configuration::NodeType;
	std::vector<SettingNode> live;
	for (const auto& section : UnsavedSettings::All())
		for (const auto& key : section.second) {
			if (key.second == UnsavedSettings::kNoValue) continue;
			SettingNode now;
			if (!Config.FillNode(&now, section.first.c_str(), key.first.c_str())) continue;
			live.push_back(now);
		}
	if (live.empty()) return live;
	if (Config.MaxQualityActive) {	// put back by PeekSavedEnd
		s_peekMaxQuality = Config.MaxQualityConfig;
		s_peekMaxQualityUser = Config.MaxQualityUser;
		s_peekMaxQualityChanged = Config.MaxQualityUserChanged;
	}
	SettingsRevision++;	// 2026-11-26: GetSetting* keep what they read until this changes (SettingsCache): read the saved values
	for (const SettingNode& now : live) {
		SettingNode saved = now;
		const std::string& text = UnsavedSettings::All()[now.Section][now.Key];
		PutNodeText(saved.Value, text.c_str());
		if (saved.Type == Kind::String) saved.LongValue = text;
		else if (saved.Type == Kind::Integer) saved.IntValue = atoi(text.c_str());
		else if (saved.Type == Kind::Float) saved.FloatValue = (float)atof(text.c_str());
		else if (saved.Type == Kind::Boolean) saved.BoolValue = text == "true";
		Config.SetValue(&saved);
	}
	return live;
}

void SettingManager::PeekSavedEnd(const std::vector<Configuration::ConfigNode>& live) {
	if (live.empty()) return;
	for (const SettingNode& now : live) {
		SettingNode node = now;
		Config.SetValue(&node);
	}
	if (Config.MaxQualityActive) {
		Config.MaxQualityConfig = s_peekMaxQuality;
		Config.MaxQualityUser = s_peekMaxQualityUser;
		Config.MaxQualityUserChanged = s_peekMaxQualityChanged;
	}
	SettingsRevision++;	// and the live ones again
}

// Increment and Decrement share one step: +1 (Increment) or -1 (Decrement) for the setting's type. A boolean goes only to the side
// the step points at.
static void StepSetting(SettingManager* manager, const char* section, const char* key, int step) {
	using Kind = SettingManager::Configuration::NodeType;
	SettingNode node;
	manager->Config.FillNode(&node, section, key);
	if (node.Type == Kind::Integer) {
		manager->SetSetting(section, key, manager->GetSettingI(section, key) + step);
	}
	else if (node.Type == Kind::Float) {
		// steps of 0.1 on a value cut to four decimals, so float error does not pile up
		const float scaled = (int)(manager->GetSettingF(section, key) * 10000) + step * 1000;
		manager->SetSetting(section, key, scaled / 10000);
	}
	else if (node.Type == Kind::Boolean) {
		const bool wanted = step > 0;
		if (node.BoolValue != wanted) manager->SetSetting(section, key, wanted);
	}
	else if (step > 0)
		Logger::Log("Node %s is of a type that can't be incremented", key);
	else
		Logger::Log("Node %s is of a type that can't be decremented", key);	// UNOFFICIAL 2026-11-01: the name was missing (read a random argument)
}

void SettingManager::Increment(const char* section, const char* key) {
	StepSetting(this, section, key, 1);
}

void SettingManager::Decrement(const char* section, const char* key) {
	StepSetting(this, section, key, -1);
}


// A weather's setting changed in the menu: the weather's toml entry (Main value, HDR list or one colour row) and the game's weather.
void SettingManager::SetSettingWeather(const char* section, const char* key, float value) {

	using Kind = Configuration::NodeType;
	Configuration::ConfigNode node;
	char attribute[80] = { NULL };
	char colorSection[40];
	char colorNode[8];

	StringList pieces;	// "Weathers", the weather's name, the sub-section
	SplitString(section, ".", &pieces);
	const char* weatherName = pieces[1].c_str();
	TESWeather* gameWeather = (TESWeather*)DataHandler->GetFormByName(weatherName, TESForm::FormType::kFormType_Weather);
	SettingsWeatherStruct* weather = GetSettingsWeather(weatherName);
	if (!weather) {
		Config.CreateWeatherSection(weatherName, gameWeather);
		weather = GetSettingsWeather(weatherName);
	}

	// the attribute text is built piece by piece; numbers are printed the way the generic ToString prints them
	auto append = [&](const char* piece) { strcat(attribute, piece); };
	auto appendValue = [&](auto number) { strcat(attribute, ToString(number).c_str()); };
	const std::string& part = pieces[2];

	if (part == "Main") {
		Config.FillNode(&node, section, key);
		if (node.Type == Kind::Boolean) {
			node.BoolValue = value != 0.0f;
			strcpy(node.Value, ToString<bool>(node.BoolValue).c_str());
		}
		else if (node.Type == Kind::Integer) {
			node.IntValue = (int)value;
			strcpy(node.Value, ToString<int>(node.IntValue).c_str());
		}
		else if (node.Type == Kind::Float) {
			node.FloatValue = value;
			strcpy(node.Value, ToString<float>(value).c_str());
		}
	}
	else if (part == "HDR") {
		attribute[0] = NULL;
		for (UInt32 slot = 0; slot < 14; ++slot) {
			appendValue(strcmp(WeatherHDRTypes[slot], key) ? weather->HDR[slot] : value);	// the edited entry takes the new value
			if (slot < 13) append(" ");
		}
		CreateNodeS(&node, section, "HDR", attribute, 0);
	}
	else {
		attribute[0] = NULL;
		for (UInt32 type = 0; type < TESWeather::kNumColorTypes; ++type) {
			if (part != WeatherColorTypes[type]) continue;
			const UInt32 count = weather->ColorTimeCount[type] > 4 ? TESWeather::kNumTimeOfDay : 4;
			for (UInt32 when = 0; when < count; ++when) {
				const auto& rgb = weather->Colors[type].colors[when];
				if (!strstr(key, WeatherTimesOfDay[when])) {
					appendValue(rgb.r); append(","); appendValue(rgb.g); append(","); appendValue(rgb.b);
				}
				else {
					// the edited channel is the one the key ends with; the other two stay as they are
					const char channel = key[strlen(key) - 1];
					if (channel == 'R' || channel == 'G' || channel == 'B') {
						appendValue(channel == 'R' ? value : (float)rgb.r); append(",");
						appendValue(channel == 'G' ? value : (float)rgb.g); append(",");
						appendValue(channel == 'B' ? value : (float)rgb.b);
					}
				}
				if (when < count - 1) append(" ");
			}
			sprintf(colorSection, "%s.%s.Colors", pieces[0].c_str(), pieces[1].c_str());
			sprintf(colorNode, "Color%i", type);
			CreateNodeS(&node, colorSection, colorNode, attribute, 0);
			break;
		}
	}

	SetSetting(&node);
	SetSettingsWeather(gameWeather);
}

/*
* The children of a section, as the menu lists them.
*/
void SettingManager::FillMenuSections(StringList* names, const char* parent) {

	const bool weathers = parent != NULL && memcmp(parent, "Weathers", 8) == 0;
	if (!weathers) {
		Config.FillSections(names, parent);
		FilterMenuSections(names, parent);
		return;
	}

	if (parent[8] == '\0') {
		// "Weathers" itself: the weather names, from the game's data
		DataHandler->FillNames(names, TESForm::FormType::kFormType_Weather);
		std::sort(names->begin(), names->end());
		return;
	}

	// inside one weather: its Main, one list per colour type and HDR
	*names = { "Main" };
	for (UInt32 type = 0; type < TESWeather::kNumColorTypes; ++type) names->push_back(WeatherColorTypes[type]);
	names->emplace_back("HDR");
}

/*
* The settings of one menu section, with their values, for the menu to show.
*/
void SettingManager::FillMenuSettings(Configuration::SettingList* list, const char* section) {

	Configuration::ConfigNode node;
	StringList pieces;

	list->clear();
	if (memcmp(section, "Weathers", 8) != 0) {
		Config.FillSettings(list, section);	// an ordinary section: its own settings
		return;
	}

	// the weather sections show values generated from the game's data
	SplitString(section, ".", &pieces);
	SettingsWeatherStruct* weather = GetSettingsWeather(pieces[1].c_str());

	auto pushValue = [&](const char* key, auto value) {
		CreateNode(&node, section, key, value, 0);
		list->push_back(node);
	};
	// the day, night, sunrise and sunset values of one colour type: red, green, blue for each time of day
	auto pushColors = [&](const auto& colorType) {
		static const char* times[] = { "Day", "Night", "Sunrise", "Sunset" };
		static const int timeOfDay[] = { TESWeather::eTime_Day, TESWeather::eTime_Night, TESWeather::eTime_Sunrise, TESWeather::eTime_Sunset };
		for (int t = 0; t < 4; t++) {
			const auto& rgb = colorType.colors[timeOfDay[t]];
			pushValue((std::string(times[t]) + "R").c_str(), rgb.r);
			pushValue((std::string(times[t]) + "G").c_str(), rgb.g);
			pushValue((std::string(times[t]) + "B").c_str(), rgb.b);
		}
	};
	const std::string& part = pieces[2];

	if (weather) {
		if (part == "Main") {
			Config.FillSettings(list, section);
		}
		else if (part == "HDR") {
			for (UInt32 slot = 0; slot < 14; ++slot) pushValue(WeatherHDRTypes[slot], weather->HDR[slot]);
		}
		else {
			for (UInt32 type = 0; type < TESWeather::kNumColorTypes; ++type)
				if (part == WeatherColorTypes[type]) pushColors(weather->Colors[type]);
		}
		return;
	}

	TESWeather* gameWeather = (TESWeather*)DataHandler->GetFormByName(pieces[1].c_str(), TESForm::FormType::kFormType_Weather);
	if (part == "Main") {
		pushValue("CloudSpeedLower", gameWeather->GetCloudSpeedLower());
		pushValue("CloudSpeedUpper", gameWeather->GetCloudSpeedUpper());
		pushValue("FogFarDay", gameWeather->GetFogDayFar());
		pushValue("FogFarNight", gameWeather->GetFogNightFar());
		pushValue("FogNearDay", gameWeather->GetFogDayNear());
		pushValue("FogNearNight", gameWeather->GetFogNightNear());
		pushValue("SunDamage", gameWeather->GetSunDamage());
		pushValue("SunGlare", gameWeather->GetSunGlare());
		pushValue("TransDelta", gameWeather->GetTransDelta());
		pushValue("WindSpeed", gameWeather->GetWindSpeed());
	}
	else if (part == "HDR") {
		for (UInt32 slot = 0; slot < 14; ++slot) pushValue(WeatherHDRTypes[slot], gameWeather->GetHDR(slot));
	}
	else {
		for (UInt32 type = 0; type < TESWeather::kNumColorTypes; ++type)
			if (part == WeatherColorTypes[type]) pushColors(gameWeather->colors[type]);
	}
}


// The parts every hand-made node shares: its name, the reboot flag and its type.
static void NameNode(SettingNode* node, const char* section, const char* key, bool reboot, UInt32 type) {
	strcpy(node->Section, section);
	strcpy(node->Key, key);
	node->Reboot = reboot;
	node->Type = type;
}

/*
* Hand-made nodes (menu values, weather entries, SetSetting), one per value type. The text form is filled too, for the
* readers that still use it (the UI, the weather strings).
*/
void SettingManager::CreateNode(Configuration::ConfigNode* node, const char* section, const char* key, float value, bool reboot) {
	NameNode(node, section, key, reboot, Configuration::NodeType::Float);
	node->FloatValue = value;
	strcpy(node->Value, ToString<float>(value).c_str());
}

void SettingManager::CreateNode(Configuration::ConfigNode* node, const char* section, const char* key, int value, bool reboot) {
	NameNode(node, section, key, reboot, Configuration::NodeType::Integer);
	node->IntValue = value;
	strcpy(node->Value, ToString<int>(value).c_str());
}

void SettingManager::CreateNode(Configuration::ConfigNode* node, const char* section, const char* key, bool value, bool reboot) {
	NameNode(node, section, key, reboot, Configuration::NodeType::Boolean);
	node->BoolValue = value;
	strcpy(node->Value, ToString<bool>(value).c_str());
}

void SettingManager::CreateNodeS(Configuration::ConfigNode* node, const char* section, const char* key, const char* value, bool reboot) {
	NameNode(node, section, key, reboot, Configuration::NodeType::String);
	PutNodeText(node->Value, value);	// cut; the whole text goes to LongValue
	node->LongValue = value;
}


// UNOFFICIAL 2026-10-19 ('cloth toggle turns on then off immediately'): which setting is a Shaders list entry's on/off switch.
// Effects and shaders have [Shaders.<Name>.Status] Enabled; a few settings-only entries keep theirs in [Shaders.<Name>.Main]
// Enabled (Cloth, CloudShadows) and some have none (Foliage: the settings inside decide). The list's checkbox, the page's
// button and Insert wrote a Status that does not exist for those, so the switch sprang straight back.
SettingManager::MenuSwitch SettingManager::GetMenuShaderSwitch(const char* Name) {
	static std::unordered_map<std::string, MenuSwitch> fromDefaults;	// the defaults do not change while the game runs
	MenuSwitch kind;
	auto it = fromDefaults.find(Name);
	if (it != fromDefaults.end()) kind = it->second;
	else {
		auto hasEnabled = [this, Name](const char* sub) {
			char path[256];
			snprintf(path, sizeof(path), "_Shaders.%s.%s", Name, sub);
			StringList keys;
			SplitString(path, ".", &keys);
			tomlValue* section = Config.FindSection(&Config.DefaultConfig, &keys);
			if (!section) return false;
			try { section->at("Enabled"); return true; }
			catch (const std::exception&) { return false; }
		};
		kind = hasEnabled("Status") ? MenuSwitch::Status : hasEnabled("Main") ? MenuSwitch::MainEnabled : MenuSwitch::None;
		fromDefaults.emplace(Name, kind);
	}
	// an effect or shader without a Status in the defaults keeps the old behaviour (SwitchShaderStatus switches its record)
	if (kind == MenuSwitch::None && (TheShaderManager->GetEffectByName(Name) || TheShaderManager->GetShaderCollectionByName(Name)))
		return MenuSwitch::Status;
	return kind;
}


// UNOFFICIAL 2026-11-20 deferred wetness step 3 (docs/DESIGN.md "Deferred wetness design" "Step 3 as built"; SettingManager.h)
bool SettingManager::RainWetnessOneSwitch() {
	return GetSettingI("Main.Main.Quality", "DeferredWetness") != 0;
}

bool SettingManager::GetMenuShaderEnabled(const char* name) {
	// UNOFFICIAL 2026-11-04 HudRaindrops has no section of its own: it is switched under Wetness Effects (HudRaindropsEffect::UpdateSettings).
	if (!strcmp(name, "HudRaindrops")) return GetSettingI("Shaders.WetnessEffects.HudRaindrops", "Enabled") != 0;
	// UNOFFICIAL 2026-11-20 one Rain wetness switch: Puddles is on exactly when Wetness Effects is (ApplyRainWetnessSwitch keeps the setting so)
	if (!strcmp(name, "Puddles") && RainWetnessOneSwitch()) return GetMenuShaderEnabled("WetnessEffects");
	// UNOFFICIAL 2026-11-12 (sungi p4) SunGI ([Shaders.AmbientLight.Main] SunGI, the one switch for the exterior sun bounce) implies AmbientLight's
	// Status Enabled: it is on at load and after a revert (the runtime flag; AmbientLight::UpdateSettings follows a live change). The toml is not written;
	// AmbientLight::ShouldRender keeps it off indoors while only SunGI has it on, so interiors stay as the user set them. RoomGI the same, indoors.
	// (2026-11-26 al-menu) Switching the effect off clears both keys (SetMenuShaderEnabled below), so the row then reads the Status key alone.
	if (!strcmp(name, "AmbientLight")) {
		Configuration::ConfigNode sunGi;
		if (Config.FillNode(&sunGi, "Shaders.AmbientLight.Main", "SunGI") && sunGi.BoolValue) return true;
		if (Config.FillNode(&sunGi, "Shaders.AmbientLight.Main", "RoomGI") && sunGi.BoolValue) return true;
	}

	char section[256];

	switch (GetMenuShaderSwitch(name)) {
	case MenuSwitch::MainEnabled:
		snprintf(section, sizeof(section), "Shaders.%s.Main", name);
		return GetSettingI(section, "Enabled") != 0;
	case MenuSwitch::None:
		return true;	// no switch (the menu shows none): its settings decide
	default:
		break;
	}

	snprintf(section, sizeof(section), "Shaders.%s", name);

	// a shader with no section in the user's config nor in the defaults has nothing to switch: it is always on
	StringList steps;
	StorageKeys(section, &steps);
	const bool inUser = Config.FindSection(&Config.TomlConfig, &steps) != nullptr;
	StorageKeys(section, &steps);
	const bool inDefaults = Config.FindSection(&Config.DefaultConfig, &steps) != nullptr;
	if (!inUser && !inDefaults) {
		Logger::Log("No Shader setting for %s, defaults to true", section);
		return true;
	}

	strcat(section, ".Status");
	if (GetSettingI(section, "Enabled") != 0) return true;
	if (!IsShaderForced(name)) return false;
	SetMenuShaderEnabled(name, true);	// a forced shader is switched back on
	return true;
}


void SettingManager::SetMenuShaderEnabled(const char* name, bool enabled) {
	if (!strcmp(name, "HudRaindrops")) { SetSetting("Shaders.WetnessEffects.HudRaindrops", "Enabled", enabled); return; }	// UNOFFICIAL 2026-11-04 (above)
	// UNOFFICIAL 2026-11-20 one Rain wetness switch (RainWetnessOneSwitch): Wetness Effects' switch sets Puddles' too; Puddles has none of its own
	if (RainWetnessOneSwitch()) {
		if (!strcmp(name, "Puddles")) return;
		if (!strcmp(name, "WetnessEffects")) SetSetting("Shaders.Puddles.Status", "Enabled", enabled);
	}
	// UNOFFICIAL 2026-11-26 (al-menu) Ambient light off also turns SunGI and RoomGI off. Either one holds the effect on in memory (GetMenuShaderEnabled
	// above, AmbientLightEffect::UpdateSettings), so with one left on the switch was put straight back: the row stayed on, and its Status key, written
	// false, could never be written true again from the menu (the row only offered "off"). One place for the effect row, the effects list, the AO page's
	// pickers and the hiding notes' buttons: all of them reach here through ShaderManager::SwitchShaderStatus. Switching on writes Status only.
	if (!enabled && !strcmp(name, "AmbientLight")) {
		for (const char* key : { "SunGI", "RoomGI" }) {
			Configuration::ConfigNode node;
			if (Config.FillNode(&node, "Shaders.AmbientLight.Main", key) && node.BoolValue) SetSetting("Shaders.AmbientLight.Main", key, false);
		}
	}
	const MenuSwitch kind = GetMenuShaderSwitch(name);
	if (kind == MenuSwitch::None) return;
	char switchSection[256];
	snprintf(switchSection, sizeof(switchSection), kind == MenuSwitch::MainEnabled ? "Shaders.%s.Main" : "Shaders.%s.Status", name);
	SetSetting(switchSection, "Enabled", enabled);
}

bool SettingManager::GetMenuMiscEnabled(const char* key) {
	return GetSettingI("Main.Main.Misc", key) != 0;
}


void SettingManager::SetMenuMiscEnabled(const char* key, bool enabled) {
	SetSetting("Main.Main.Misc", key, enabled);
}


// the entry of a map keyed by name, or NULL when there is none
template <typename Map>
static typename Map::mapped_type* FindEntry(Map& entries, const char* name) {
	auto it = entries.find(std::string(name));
	return it == entries.end() ? nullptr : &it->second;
}

SettingsWaterStruct* SettingManager::GetSettingsWater(const char* location) {
	return FindEntry(SettingsWater, location);
}

SettingsColoringStruct* SettingManager::GetSettingsColoring(const char* location) {
	return FindEntry(SettingsColoring, location);
}

SettingsWeatherStruct* SettingManager::GetSettingsWeather(const char* weatherName) {
	return FindEntry(SettingsWeather, weatherName);
}

void SettingManager::SetSettingsWeather(TESWeather* weather) {

	TESWeatherEx* ex = (TESWeatherEx*)weather;
	SettingsWeatherStruct* saved = GetSettingsWeather(ex->GetEditorName());

	if (saved) {
		SetTextureAndHDR(ex, saved->UpperLayer, saved->LowerLayer, saved->HDR);
		ex->SetWindSpeed(saved->WindSpeed);
		ex->SetCloudSpeedLower(saved->CloudSpeedLower);
		ex->SetCloudSpeedUpper(saved->CloudSpeedUpper);
		ex->SetTransDelta(saved->TransDelta);
		ex->SetSunGlare(saved->SunGlare);
		ex->SetSunDamage(saved->SunDamage);
		ex->SetFogDayNear(saved->FogNearDay);
		ex->SetFogDayFar(saved->FogFarDay);
		ex->SetFogNightNear(saved->FogNearNight);
		ex->SetFogNightFar(saved->FogFarNight);
		for (int c = 0; c < TESWeather::kNumColorTypes; ++c) {
			const int count = (std::min)((int)saved->ColorTimeCount[c], (int)TESWeather::kNumTimeOfDay);
			for (int t = 0; t < count; ++t) {
				ex->colors[c].colors[t] = saved->Colors[c].colors[t];
				ex->colorsb[c].colors[t] = saved->Colors[c].colors[t];
			}
		}
	}

	// the lighting, fog and sun colours are the source copy (colorsb) times the Main coefficients; the source is never written
	auto scale = [&](int color, int time, const NiPoint3& k) {
		auto& dst = ex->colors[color].colors[time];
		const auto& src = ex->colorsb[color].colors[time];
		dst.r = src.r * k.x;
		dst.g = src.g * k.y;
		dst.b = src.b * k.z;
	};
	const auto& mode = SettingsMain.WeatherMode;
	for (int c = 0; c < TESWeatherEx::eColor_Lighting; c++) scale(c, TESWeatherEx::eTime_Night, mode.CoeffNight);
	for (int when = 0; when < TESWeatherEx::eTime_Night; ++when) {
		scale(TESWeatherEx::eColor_Fog, when, mode.CoeffFog);
		scale(TESWeatherEx::eColor_Sunlight, when, mode.CoeffSun);
		scale(TESWeatherEx::eColor_Sun, when, mode.CoeffSun);
	}
}

// Numbers print through float: "0.5", "3", "1e+06" (the format the weather strings and the menu's text use).
template <class T>
std::string SettingManager::ToString(const T number) {
	std::ostringstream text;
	text << (float)number;
	return text.str();
}

template <class T>
T SettingManager::FromString(const char* text) {
	std::istringstream in(text);
	float parsed = 0.0f;
	in >> parsed;
	return parsed;
}

// The pieces of text between the delimiter characters (empty pieces dropped). Any length (2026-11-17: was a 256-byte buffer).
void SettingManager::SplitString(const char* text, const char* delimiters, StringList* parts) {
	std::string buffer(text);
	parts->clear();
	for (char* piece = strtok(buffer.data(), delimiters); piece != NULL; piece = strtok(NULL, delimiters))
		parts->push_back(piece);
}

// Parses the pieces of text into consecutive slots of values, at most as many as the array has (2026-11-17: was as many as
// the text had, written past the array's end).
template <class T, size_t N>
void SettingManager::FillFromString(const char* text, const char* delimiters, T (&values)[N]) {
	std::string buffer(text);
	size_t slot = 0;
	for (char* piece = strtok(buffer.data(), delimiters); piece != NULL && slot < N; piece = strtok(NULL, delimiters))
		values[slot++] = FromString<T>(piece);
}

bool SettingManager::IsShaderForced(const char* /*name*/) {
	return false;	// nothing is forced on in this build
}

void SettingManager::FilterMenuSections(StringList* sections, const char* parent) {
	if (!parent) return;
	if (memcmp(parent, "Shaders.ShadowsExteriors", 24)) return;

	// the quality level: below Custom (4) the advanced shadow sections are not listed
	const int quality = GetSettingI("Shaders.ShadowsExteriors.Main", "Quality");
	if (quality < 0 || quality >= 4) return;

	static const char* const advanced[] = { "Forms", "ShadowMaps" };	// UNOFFICIAL 2026-11-04: one Forms for the four cascades (was FormsNear, FormsMiddle, FormsFar, FormsLod)
	StringList shown;
	for (const std::string& entry : *sections) {
		bool hide = false;
		for (const char* name : advanced)
			if (entry == name) hide = true;
		if (!hide) shown.push_back(entry);
	}
	*sections = shown;
}

