#include <mutex>
#include <unordered_set>	// UNOFFICIAL 2026-10-30 SettingsCache
#include "InstallCheck.h"	// 2026-11-03: the user toml's unknown keys and version mark, for the install self-check
#include "GpuDebug.h"	// UNOFFICIAL 2026-10-26: GpuDebugLabels / RenderDoc capture key ([Main.Develop.Main])

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
	char path[MAX_PATH] = { '_' };
	strcat(path, section);
	SettingManager::SplitString(path, ".", steps);
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
static const int UnofficialSettingsVersion = 78;  // (78: RT defaults replaced since 1.0.5 now reach saved configs; 77: ptaaa-firstperson, FirstPersonBody on again) 53 was an enhanced-water step, since removed

// UNOFFICIAL 2026-10-30: a setting is read only when the defaults file lists it (FillNode), so a line in the user's toml that
// this build does not know (a key from a newer or an older build, a typo) silently did nothing. Each one is logged once per
// load; a whole unknown section is one line. [_Unofficial] is NVR's own bookkeeping, never in the defaults.
// 2026-11-03: also returns them (section.key), for the install check (InstallCheck.h)
// 2026-11-05: settings this build removed on purpose (the Oblivion-era leftovers: the old text menu's fonts, colours, layout and keys,
// SleepingMode, TorchesCastShadows). Older settings files still hold them; they are skipped quietly instead of counted as unknown.
static bool Retired(const std::string& Name) {
	static const char* const kSections[] = { "Main.SleepingMode" };
	static const char* const kKeys[] = { "Shaders.ShadowsInteriors.Main.TorchesCastShadows", "Shaders.ShadowsExteriors.ScreenSpace.LodOnly",	// (2026-11-06: always on now)
		"Main.Menu.Keys.KeyAdd", "Main.Menu.Keys.KeyDown", "Main.Menu.Keys.KeyEditing", "Main.Menu.Keys.KeyLeft", "Main.Menu.Keys.KeyPageDown",
		"Main.Menu.Keys.KeyPageUp", "Main.Menu.Keys.KeyRight", "Main.Menu.Keys.KeySave", "Main.Menu.Keys.KeySubtract", "Main.Menu.Keys.KeyUp",
		"Main.Menu.Keys.EntryUseNumpad", "Main.Menu.Style.ItemColumnSize", "Main.Menu.Style.MainItemColumnSize", "Main.Menu.Style.PositionX",
		"Main.Menu.Style.PositionY", "Main.Menu.Style.RowSpace", "Main.Menu.Style.RowsPerPage", "Main.Menu.Style.TextColorEditing",
		"Main.Menu.Style.TextColorEnabled", "Main.Menu.Style.TextColorNormal", "Main.Menu.Style.TextColorSelected", "Main.Menu.Style.TextFont",
		"Main.Menu.Style.TextFontStatus", "Main.Menu.Style.TextShadowColorEditing", "Main.Menu.Style.TextShadowColorEnabled",
		"Main.Menu.Style.TextShadowColorNormal", "Main.Menu.Style.TextShadowColorSelected", "Main.Menu.Style.TextSizeStatus",
		"Main.Menu.Style.TitleColumnSize" };
	for (const char* k : kKeys) if (Name == k) return true;
	for (const char* sec : kSections) if (Name.compare(0, strlen(sec), sec) == 0) return true;
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
		// UNOFFICIAL 2026-10-19: no settings file yet (a new install, or a test run that moved it aside) is no error
		const bool noFile = GetFileAttributesA(userFile) == INVALID_FILE_ATTRIBUTES;
		if (noFile)
			Logger::Log("No settings file yet (%s): one will be made from the defaults.", userFile);
		else
			Logger::Log("error loading toml: %s, new config will be created from defaults.", failure.what());
		TomlConfig = toml::table{};	// starts empty: every read fills in its default
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
	if (!defaultTable) return false;	// not a setting of this build

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
		if (tomlValue* layer = FindSection(&MaxQualityConfig, &steps)) {
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
		catch (const std::out_of_range&) {
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
	return FindSection(&TomlConfig, &steps);
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
	if (!table) {
		StorageKeys(node->Section, &steps);
		table = &Target;
		for (const std::string& step : steps) {
			if (!table->contains(step)) (*table)[step] = toml::table();
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
			for (int when = 0; when < TESWeather::kNumTimeOfDay; ++when) {
				FillFromString(colorTriples[when].c_str(), ",", rgb);
				auto& color = SE.Colors[colorType].colors[when];
				color.r = rgb[0];
				color.g = rgb[1];
				color.b = rgb[2];
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

int SettingManager::GetSettingI(const char* section, const char* key) {
	std::lock_guard<std::mutex> lock(SettingCacheLock);
	CachedSetting scratch;
	return CachedNode(this, section, key, scratch)->i;	// the typed value directly, no string re-parse (0 when the setting does not exist)
}

float SettingManager::GetSettingF(const char* section, const char* key) {
	std::lock_guard<std::mutex> lock(SettingCacheLock);
	CachedSetting scratch;
	return CachedNode(this, section, key, scratch)->f;
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
		if (statusSwitch && node.BoolValue != on && (on || !IsShaderForced(node.MidSection)))
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

/*
* Stores a node's value in the settings (the user's table, or Max Quality's layer).
*/
void SettingManager::SetSetting(Configuration::ConfigNode* node) {
	hasUnsavedChanges = true;
	SettingsRevision++;

	// UNOFFICIAL 2026-11-01: every change goes to the log as old -> new (SessionLog: coalesced while a slider is dragged, rate-limited)
	SettingNode before;
	const bool known = Config.FillNode(&before, node->Section, node->Key);
	Config.SetValue(node);
	if (known) SessionLog::OnSettingChanged(node->Section, node->Key, LogValue(before).c_str(), LogValue(*node).c_str());
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
			for (UInt32 when = 0; when < TESWeather::kNumTimeOfDay; ++when) {
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
				if (when < TESWeather::kNumTimeOfDay - 1) append(" ");
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


bool SettingManager::GetMenuShaderEnabled(const char* name) {
	// UNOFFICIAL 2026-11-04 HudRaindrops has no section of its own: it is switched under Wetness Effects (HudRaindropsEffect::UpdateSettings).
	if (!strcmp(name, "HudRaindrops")) return GetSettingI("Shaders.WetnessEffects.HudRaindrops", "Enabled") != 0;
	// UNOFFICIAL 2026-11-12 (sungi p4) SunGI ([Shaders.AmbientLight.Main] SunGI, the one switch for the exterior sun bounce) implies AmbientLight's
	// Status Enabled: it is on at load and after a revert (the runtime flag; AmbientLight::UpdateSettings follows a live change). The toml is not written;
	// AmbientLight::ShouldRender keeps it off indoors while only SunGI has it on, so interiors stay as the user set them.
	if (!strcmp(name, "AmbientLight")) {
		Configuration::ConfigNode sunGi;
		if (Config.FillNode(&sunGi, "Shaders.AmbientLight.Main", "SunGI") && sunGi.BoolValue) return true;
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
		memcpy(ex->colors, saved->Colors, TESWeather::kNumColorTypes * TESWeather::kNumTimeOfDay * 4);
		memcpy(ex->colorsb, saved->Colors, TESWeather::kNumColorTypes * TESWeather::kNumTimeOfDay * 4);
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

// The pieces of text between the delimiter characters (empty pieces dropped); text is at most 255 characters.
void SettingManager::SplitString(const char* text, const char* delimiters, StringList* parts) {
	char buffer[256];
	strcpy(buffer, text);
	parts->clear();
	for (char* piece = strtok(buffer, delimiters); piece != NULL; piece = strtok(NULL, delimiters))
		parts->push_back(piece);
}

// Parses the pieces of text into consecutive slots of values (the caller sizes values for them).
template <class T>
void SettingManager::FillFromString(const char* text, const char* delimiters, T* values) {
	char buffer[256];
	strcpy(buffer, text);
	T* slot = values;
	for (char* piece = strtok(buffer, delimiters); piece != NULL; piece = strtok(NULL, delimiters))
		*slot++ = FromString<T>(piece);
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

