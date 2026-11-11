#include "PresetManager.h"
#include "StalePresetValues.h"	// UNOFFICIAL 2026-11-10
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;

using PresetData = PresetManager::PresetData;
using PresetValue = PresetManager::PresetValue;
using ValueKind = PresetManager::PresetValue::ValueType;
using ResolveResult = PresetManager::ResolveResult;
using ResolvedTier = PresetManager::ResolvedTier;

// ---- paths -----------------------------------------------------------------
// The game directory is captured on first use. Common-dialog calls from other
// modules can move the process CWD later, so every path below hangs off this
// snapshot instead (the same reason SettingManager keeps its own copy).
static const std::string& GameRootDir() {
	static std::string root;
	if (root.empty()) {
		char cwdBuf[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, cwdBuf);
		root = cwdBuf;
		root += '\\';
	}
	return root;
}

static std::string UnderGameRoot(const std::string& relative) {
	return GameRootDir() + relative;
}

namespace {

	// ---- module state ------------------------------------------------------

	struct KeywordTags {
		std::unordered_map<std::string, std::string>	keywordOfCell;		// cell EditorID -> keyword
		std::unordered_map<std::string, UInt32>			cellsPerKeyword;	// keyword -> how many cells carry it
	};

	// The place ShouldReResolve last answered true for.
	struct LastPlace {
		bool			known = false;
		bool			indoors = false;
		TESObjectCELL*	cell = nullptr;		// only while indoors
		TESWorldSpace*	world = nullptr;	// only while outdoors
	};

	// UNOFFICIAL 2026-10-19 (see ResolveAndApply): what is active now, and the player's own settings used where no preset exists.
	struct ActiveIdentity {
		std::string	key;			// preset name (or "<own settings>") plus the enabled Variants
		bool		isOwn = false;
		PresetData	ownSettings;
		bool		haveOwn = false;
	};

	KeywordTags					gTags;
	std::vector<std::string>	gVariantOrder;	// enabled Variants in file order, no repeats
	ResolveResult				gLastResult;
	UInt32						gGeneration = 0;
	PresetData					gBaseline;		// what the last apply pushed (CaptureVariantDiff compares against it)
	LastPlace					gPlace;
	ActiveIdentity				gActive;

	// ---- setting-scope blacklist ---------------------------------------------
	// docs/preset-manager-design.md, "Setting scope": machine and player choices, never stored in or applied from a preset.
	const std::unordered_set<std::string> kNeverInPreset = {
		"Main.CameraMode.Main", "Main.CameraMode.Positioning",
		"Main.Develop.Main", "Main.Develop.PixelInspector", "Main.Develop.ShaderCounters",	// UNOFFICIAL 2026-10-26: debugging tools
		"Main.FlyCam.Main", "Main.FrameRate.SmartControl", "Main.FrameRate.Stuttering",
		"Main.Main.Misc", "Main.Menu.Keys", "Main.Menu.Style",
		// UNOFFICIAL 2026-10-19: this build's own machine / player preferences, same reason as above (a location preset, or
		// the defaults applied where none exists, must never switch the player's speed, quality or gun-effect choices).
		"Main.GunFX.Main",
		"Main.Main.Performance", "Main.Main.Quality", "Main.Main.ReducedQuality",
		"Main.Menu.NewInPatch",
		"Main.Menu.Developer",	// UNOFFICIAL 2026-10-25: the menu's Developer options box
		"Shaders.AmbientOcclusion.FastPaths",
		"Shaders.Debug.Main",
		"Shaders.Skin.Debug",
	};

	// ---- small helpers -------------------------------------------------------

	// The line without leading/trailing spaces, tabs, CR and LF.
	std::string_view Trim(std::string_view text) {
		const size_t from = text.find_first_not_of(" \t\r\n");
		if (from == std::string_view::npos) return {};
		return text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1);
	}

	// Walks an INI-like file: blank lines and ';' comments are skipped, "[name]" lines go to onHeader (whole line, brackets
	// included), everything else to onLine (trimmed).
	void ScanListFile(std::istream& in, const std::function<void(std::string_view)>& onHeader,
		const std::function<void(std::string_view)>& onLine) {
		std::string text;
		while (std::getline(in, text)) {
			const std::string_view line = Trim(text);
			if (line.empty() || line[0] == ';') continue;
			if (line.front() == '[' && line.back() == ']')
				onHeader(line);
			else
				onLine(line);
		}
	}

	// Regular *.ini files directly inside an existing folder, in directory order.
	std::vector<fs::path> IniFilesIn(const fs::path& folder) {
		std::vector<fs::path> found;
		for (const auto& entry : fs::directory_iterator(folder)) {
			if (entry.is_regular_file() && entry.path().extension() == ".ini")
				found.push_back(entry.path());
		}
		return found;
	}

	bool IsFolder(const fs::path& folder) {
		return fs::exists(folder) && fs::is_directory(folder);
	}

	std::string PresetFile(const std::string& name) {
		return UnderGameRoot(PresetManager::kPresetsDir()) + name + ".ini";
	}

	std::string VariantFile(const std::string& name) {
		return UnderGameRoot(PresetManager::kVariantsDir()) + name + ".ini";
	}

	// ---- toml <-> PresetData -------------------------------------------------

	// Flattens a toml tree into PresetData. Section names are the dotted paths SetSettingF/SetSettingS take
	// ("Shaders.ImageAdjust.Main"). Blacklisted sections and leaves other than string/bool/integer/float are left out.
	void FlattenToml(const tomlValue& table, const std::string& dotted, PresetData& into) {
		if (!table.is_table()) return;

		for (const auto& [name, node] : table.as_table()) {
			if (node.is_table()) {
				// The defaults keep the '_' of their [_Main...] headers (SettingManager adds it back on lookup): drop it at
				// the root so the paths come out in the public form. Preset files never have it.
				const bool stripUnderscore = dotted.empty() && !name.empty() && name[0] == '_';
				const std::string part = stripUnderscore ? name.substr(1) : name;
				FlattenToml(node, dotted.empty() ? part : dotted + "." + part, into);
				continue;
			}
			if (PresetManager::IsBlacklistedSection(dotted)) continue;

			PresetValue stored{};
			stored.Type = ValueKind::Float;
			if (node.is_string()) {
				stored.Type = ValueKind::String;
				stored.StringValue = node.as_string();
			}
			else if (node.is_boolean())
				stored.FloatValue = node.as_boolean() ? 1.0f : 0.0f;
			else if (node.is_integer())
				stored.FloatValue = (float)node.as_integer();
			else if (node.is_floating())
				stored.FloatValue = (float)node.as_floating();
			else
				continue;	// arrays and other leaves are not preset values

			into[dotted][name] = stored;
		}
	}

	// Reads one preset or Variant file (same format).
	bool ReadPresetFile(const std::string& path, const std::string& label, PresetData& into) {
		into.clear();
		if (!fs::exists(path)) return false;
		try {
			const tomlValue parsed = toml::parse<toml::preserve_comments, std::map>(std::string_view(path));
			FlattenToml(parsed, "", into);
		}
		catch (const std::exception& failure) {
			Logger::Log("PresetManager: [Preset] Failed to parse '%s': %s", label.c_str(), failure.what());
			return false;
		}
		return true;
	}

	// Writes one preset or Variant file (same format); the folder is created when missing.
	bool WritePresetFile(const std::string& path, const std::string& folder, const std::string& label, const PresetData& contents) {
		tomlValue root = toml::table();

		for (const auto& [dotted, values] : contents) {
			if (PresetManager::IsBlacklistedSection(dotted)) continue;

			// One nested table per path segment, created when missing (as SettingManager's SetValue does).
			StringList parts;
			SettingManager::SplitString(dotted.c_str(), ".", &parts);
			tomlValue* table = &root;
			for (const auto& part : parts) {
				if (!table->contains(part)) (*table)[part] = toml::table();
				table = &table->at(part);
			}

			for (const auto& [key, value] : values) {
				if (value.Type == ValueKind::String)
					(*table)[key] = value.StringValue;
				else
					(*table)[key] = value.FloatValue;
			}
		}

		fs::create_directories(fs::path(folder));

		if (std::ofstream file(path, std::ios::trunc | std::ios::binary); file.is_open()) {
			file << root << std::endl;	// closed as it goes out of scope
			return true;
		}
		Logger::Log("PresetManager: [Preset] Failed to open '%s' for writing", label.c_str());
		return false;
	}

	// The shipped defaults (SettingManager's DefaultConfig) as PresetData: used where no default preset file exists.
	bool ReadShippedDefaults(PresetData& into) {
		into.clear();
		if (TheSettingManager == nullptr) return false;
		FlattenToml(TheSettingManager->Config.DefaultConfig, "", into);
		return true;
	}

	// ---- keyword tags and the Variant list -----------------------------------

	// docs "Keyword files": each [Section] names a keyword, each bare line under it a cell EditorID. Files in filename order,
	// sections in file order; the first keyword a cell gets is the one it keeps.
	void ReadKeywordFiles() {
		gTags.keywordOfCell.clear();
		gTags.cellsPerKeyword.clear();

		const std::string folderText = UnderGameRoot(PresetManager::kKeywordsDir());
		const fs::path folder(folderText);
		if (!IsFolder(folder)) {
			Logger::Log("PresetManager: [Preset] Keywords directory not found: %s", folderText.c_str());
			return;
		}

		std::vector<fs::path> files = IniFilesIn(folder);
		std::sort(files.begin(), files.end(), std::less<fs::path>());	// by file name (one folder)

		for (const fs::path& file : files) {
			std::ifstream in(file.string());
			if (!in.is_open()) {
				Logger::Log("PresetManager: [Preset] Failed to open keyword file: %s", file.string().c_str());
				continue;
			}

			std::string current;	// the keyword being read
			ScanListFile(in,
				[&](std::string_view header) { current.assign(header.substr(1, header.size() - 2)); },
				[&](std::string_view cellId) {
					if (current.empty()) return;
					const auto added = gTags.keywordOfCell.emplace(std::string(cellId), current);
					if (added.second) ++gTags.cellsPerKeyword[current];	// a cell already tagged keeps its first keyword
				});
		}

		Logger::Log("PresetManager: [Preset] Loaded %u cell keyword assignments across %u keyword(s)",
			UInt32(gTags.keywordOfCell.size()), UInt32(gTags.cellsPerKeyword.size()));
	}

	// docs "Persisting which Variants are enabled": the names under [EnabledVariants], in order, without repeats.
	void ReadVariantList() {
		gVariantOrder.clear();

		std::ifstream in(UnderGameRoot(PresetManager::kEnabledVariantsPath()));
		if (!in.is_open()) {
			Logger::Log("PresetManager: [Preset] EnabledVariants.ini not found (no Variants enabled yet)");
			return;
		}

		bool listing = false;
		ScanListFile(in,
			[&](std::string_view header) { listing = (header == "[EnabledVariants]"); },
			[&](std::string_view name) {
				if (!listing) return;
				const std::string entry(name);
				if (std::find(gVariantOrder.begin(), gVariantOrder.end(), entry) == gVariantOrder.end())
					gVariantOrder.push_back(entry);
			});

		Logger::Log("PresetManager: [Preset] Loaded %u enabled Variant(s)", UInt32(gVariantOrder.size()));
	}

	void WriteVariantList() {
		fs::create_directories(fs::path(UnderGameRoot(Paths::DataRoot())));

		std::ofstream file(UnderGameRoot(PresetManager::kEnabledVariantsPath()), std::ios::trunc | std::ios::binary);
		if (file.is_open()) {
			file << "[EnabledVariants]" << "\n";
			for (const std::string& entry : gVariantOrder) file << entry << "\n";
		}
		else
			Logger::Log("PresetManager: [Preset] Failed to open EnabledVariants.ini for writing");
	}

	// Lays the enabled Variants over a preset, lowest priority first, so the later one wins every key it has.
	void OverlayVariants(PresetData& preset) {
		for (const std::string& variant : gVariantOrder) {
			if (!PresetManager::VariantExists(variant)) continue;
			PresetData layer;
			if (!PresetManager::ReadVariant(variant, layer)) continue;
			for (const auto& [dotted, values] : layer) {
				PresetManager::PresetSection& target = preset[dotted];
				for (const auto& [key, value] : values) target[key] = value;
			}
		}
	}

	// ---- resolution ----------------------------------------------------------
	// docs "Resolution order"

	bool TakePreset(ResolvedTier tier, const std::string& name, PresetData& preset, ResolveResult& result) {
		result.Tier = tier;
		result.PresetName = name;
		return PresetManager::ReadPreset(result.PresetName, preset);
	}

	// The default tier: the authored DefaultInterior / DefaultExterior file, else the shipped defaults.
	bool TakeDefault(const char* defaultName, PresetData& preset, ResolveResult& result) {
		result.Tier = ResolvedTier::Default;
		if (PresetManager::PresetExists(defaultName))
			return TakePreset(ResolvedTier::Default, defaultName, preset, result);
		result.UsedRawDefaults = true;
		return ReadShippedDefaults(preset);
	}

	// Indoors: the cell's own preset, then its keyword's, then the interior default. Outdoors: the worldspace's own preset,
	// then the exterior default (no keyword tier outside).
	bool ChoosePreset(TESObjectCELL* cell, PresetData& preset, ResolveResult& result) {
		preset.clear();
		result = ResolveResult{};
		if (!cell) return false;

		const char* cellName = cell->GetEditorName();
		result.CellEditorID = cellName ? cellName : "";
		result.IsInterior = cell->IsInterior();

		if (!result.IsInterior) {
			TESWorldSpace* world = cell->worldSpace;
			const char* worldName = world ? world->GetEditorName() : nullptr;
			result.WorldspaceEditorID = worldName ? worldName : "";
			if (!result.WorldspaceEditorID.empty() && PresetManager::PresetExists(result.WorldspaceEditorID))
				return TakePreset(ResolvedTier::Override, result.WorldspaceEditorID, preset, result);
			return TakeDefault(PresetManager::kDefaultExteriorName, preset, result);
		}

		const std::string& cellId = result.CellEditorID;
		if (!cellId.empty()) {
			if (PresetManager::PresetExists(cellId))
				return TakePreset(ResolvedTier::Override, cellId, preset, result);
			if (const std::string* tag = PresetManager::GetKeywordForCell(cellId)) {
				result.Keyword = *tag;
				if (PresetManager::PresetExists(*tag))
					return TakePreset(ResolvedTier::Keyword, *tag, preset, result);
			}
		}
		return TakeDefault(PresetManager::kDefaultInteriorName, preset, result);
	}

	// ---- apply ---------------------------------------------------------------
	// docs "Application mechanism"

	// LUTEffect never re-reads its file names in UpdateSettings, so a LUT slot is reloaded directly (as ImGuiManager's
	// RevertToSnapshot does). -1: not a LUT slot.
	int LutSlot(const std::string& dotted, const std::string& key) {
		if (dotted != "Shaders.LUT.Main") return -1;
		if (key == "DayLUT") return 0;
		if (key == "NightLUT") return 1;
		if (key == "InteriorLUT") return 2;
		return -1;
	}

	// Pushes a preset into the live settings. Only values that differ are written (each SetSetting reloads all settings).
	void PushPreset(const PresetData& preset) {
		if (TheSettingManager == nullptr) return;

		gBaseline = preset;	// the reference for CaptureVariantDiff, previews included

		LUTEffect* lutEffect = TheShaderManager ? TheShaderManager->Effects.LUT : nullptr;
		UInt32 changed = 0; // UNOFFICIAL 2026-10-19: live values this call actually changed (logged below)

		for (const auto& [dotted, values] : preset) {
			const char* section = dotted.c_str();
			for (const auto& [key, value] : values) {
				if (value.Type != ValueKind::String) {
					if (StalePresetValues::Blocked(section, key.c_str(), value.FloatValue, "a preset")) continue;	// UNOFFICIAL 2026-11-10
					if (TheSettingManager->GetSettingF(section, key.c_str()) != value.FloatValue) {
						TheSettingManager->SetSettingF(section, key.c_str(), value.FloatValue);
						++changed;
					}
					continue;
				}

				char current[256] = {};
				TheSettingManager->GetSettingS(section, key.c_str(), current);
				const bool same = value.StringValue == current;
				const int slot = lutEffect ? LutSlot(dotted, key) : -1;
				if (slot >= 0 && !same)
					lutEffect->LoadLUT(slot, value.StringValue.c_str());
				else if (slot < 0 && !same) {
					TheSettingManager->SetSettingS(section, key.c_str(), value.StringValue.c_str());
					++changed;
				}
			}
		}

		// A preset can switch Shaders.*.Status.Enabled: copy the menu's on/off back onto each effect or collection.
		StringList effectNames;
		TheSettingManager->FillMenuSections(&effectNames, "Shaders");
		for (const std::string& name : effectNames) {
			const bool on = TheSettingManager->GetMenuShaderEnabled(name.c_str());
			if (EffectRecord* effect = TheShaderManager->GetEffectByName(name.c_str()))
				effect->Enabled = on;
			else if (ShaderCollection* collection = TheShaderManager->GetShaderCollectionByName(name.c_str()))
				collection->Enabled = on;
		}

		TheSettingManager->LoadSettings(); // one last full refresh, like RevertToSnapshot
		Logger::Log("PresetManager: [Preset] apply changed %u live value(s)", changed);
	}

	// True when `values` has `key` in `dotted`.
	bool HasKey(const PresetData& values, const std::string& dotted, const std::string& key) {
		const auto section = values.find(dotted);
		return section != values.end() && section->second.find(key) != section->second.end();
	}

	// True when the two entries hold the same value of the same kind.
	bool SameValue(const PresetValue& a, const PresetValue& b) {
		if (a.Type != b.Type) return false;
		return a.Type == ValueKind::String ? a.StringValue == b.StringValue : a.FloatValue == b.FloatValue;
	}
}

// ---- public: names, init, keywords --------------------------------------------

bool PresetManager::IsBlacklistedSection(const std::string& dottedSection) {
	return kNeverInPreset.find(dottedSection) != kNeverInPreset.end();
}

bool PresetManager::IsReservedName(const std::string& name) {
	return name == kDefaultExteriorName || name == kDefaultInteriorName;
}

// 2026-11-02: with Data\VegasShaders\ in use, presets made for New Vegas Reloaded (Data\NVR\, the same file format: settings
// sections and keys kept their names) were never seen. At boot every file there that ours lacks, or that is newer than our copy
// (a preset pack copied in later), is copied over; EnabledVariants.ini only when ours is missing (it is the user's own list). The
// old folder is never changed. The count goes to the menu's notice (OldFilesNotice).
static void ImportOldData()
{
	if (std::string(Paths::DataRoot()) != Paths::NewDataRoot) return;	// the old folder is the one in use, or neither exists
	const fs::path oldRoot(UnderGameRoot(Paths::OldDataRoot)), newRoot(UnderGameRoot(Paths::NewDataRoot));
	std::error_code ec;
	if (!fs::is_directory(oldRoot, ec)) return;
	int copied = 0, failed = 0;
	for (const char* sub : { "Presets", "Variants", "Keywords" }) {
		const fs::path from = oldRoot / sub;
		if (!fs::is_directory(from, ec)) { ec.clear(); continue; }
		for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
			if (!it->is_regular_file(ec)) continue;
			const fs::path relative = fs::path(sub) / fs::relative(it->path(), from, ec);
			const fs::path to = newRoot / relative;
			if (fs::exists(to, ec) && fs::last_write_time(to, ec) >= fs::last_write_time(it->path(), ec)) continue;
			fs::create_directories(to.parent_path(), ec);
			if (fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec)) {
				Logger::Log("PresetManager: [Preset] %s taken over from %s", relative.string().c_str(), Paths::OldDataRoot);
				copied++;
			}
			else { Logger::Log("PresetManager: [Preset] could not copy %s: %s", it->path().string().c_str(), ec.message().c_str()); failed++; ec.clear(); }
		}
		ec.clear();
	}
	const fs::path oldList = oldRoot / "EnabledVariants.ini", newList = newRoot / "EnabledVariants.ini";
	if (fs::exists(oldList, ec) && !fs::exists(newList, ec) && fs::copy_file(oldList, newList, ec)) copied++;
	if (copied || failed) {
		Logger::Log("PresetManager: [Preset] %d file(s) taken over from %s, %d failed", copied, Paths::OldDataRoot, failed);
		Paths::AddOldFilesNotice("%d preset file(s) taken over from %s", copied, Paths::OldDataRoot);
	}
}

void PresetManager::Initialize()
{
	Logger::Log("PresetManager: [Preset] Initializing...");
	ImportOldData();
	// 2026-11-02: Cartographer (its own .esp) swaps settings per cell through NVRSetSetting; with ours on as well, the two fight
	// over the same settings on every cell change. Its switch is General:bNVR in Data\Config\Cartographer\Settings.ini.
	const std::string cartographer = UnderGameRoot("Data\\Config\\Cartographer\\Settings.ini");
	const bool theirsOn = GetPrivateProfileIntA("General", "bNVR", 0, cartographer.c_str()) != 0;
	if (theirsOn && TheSettingManager->GetSettingI("Main.Main.Misc", "PresetManagerEnabled")) {
		Logger::Log("PresetManager: [Preset] Cartographer's NVR preset swapping (bNVR=1) and this mod's Preset Manager are both on: they change the same settings on every cell change");
		Paths::AddOldFilesNotice("Cartographer's preset swapping and the Preset Manager are both on: turn one off (Cartographer: bNVR in its Settings.ini)");
	}
	ReadKeywordFiles();
	ReadVariantList();
}

const std::string* PresetManager::GetKeywordForCell(const std::string& editorId) {
	const auto hit = gTags.keywordOfCell.find(editorId);
	return hit != gTags.keywordOfCell.end() ? &hit->second : nullptr;
}

UInt32 PresetManager::CountCellsForKeyword(const std::string& keyword) {
	const auto hit = gTags.cellsPerKeyword.find(keyword);
	return hit != gTags.cellsPerKeyword.end() ? hit->second : 0;
}

std::vector<std::string> PresetManager::GetKnownKeywordCells()
{
	std::vector<std::string> cells;
	cells.reserve(gTags.keywordOfCell.size());
	for (const auto& tag : gTags.keywordOfCell) cells.push_back(tag.first);
	std::sort(cells.begin(), cells.end());
	return cells;
}

// ---- public: files --------------------------------------------------------------
// docs "Preset files": a toml11 document with a full snapshot; nothing is cached.

bool PresetManager::PresetExists(const std::string& name) {
	return fs::exists(PresetFile(name));
}

bool PresetManager::VariantExists(const std::string& name) {
	return fs::exists(VariantFile(name));
}

bool PresetManager::DeletePreset(const std::string& name) {
	if (IsReservedName(name)) return false; // the two defaults cannot be deleted
	std::error_code failure;
	const bool gone = fs::remove(PresetFile(name), failure);
	return gone && !failure;
}

bool PresetManager::ReadPreset(const std::string& name, PresetData& into) {
	return ReadPresetFile(PresetFile(name), name, into);
}

bool PresetManager::WritePreset(const std::string& name, const PresetData& contents) {
	return WritePresetFile(PresetFile(name), UnderGameRoot(kPresetsDir()), name, contents);
}

bool PresetManager::ReadVariant(const std::string& name, PresetData& into) {
	return ReadPresetFile(VariantFile(name), name, into);
}

bool PresetManager::WriteVariant(const std::string& name, const PresetData& contents) {
	return WritePresetFile(VariantFile(name), UnderGameRoot(kVariantsDir()), name, contents);
}

// ---- public: resolve and apply ----------------------------------------------------

bool PresetManager::CaptureLiveState(PresetData& into) {
	into.clear();
	if (TheSettingManager == nullptr) return false;

	// Keys and kinds from the defaults (complete); values from the live getters, which fall back to the default for anything
	// the user's file leaves out.
	PresetData layout;
	if (!ReadShippedDefaults(layout)) return false;

	for (const auto& [dotted, values] : layout) {
		PresetSection& out = into[dotted];
		for (const auto& [key, shipped] : values) {
			PresetValue live{ shipped.Type, 0.0f, std::string() };
			if (live.Type != ValueKind::String) {
				live.FloatValue = TheSettingManager->GetSettingF(dotted.c_str(), key.c_str());
			}
			else {
				char text[256] = {};
				TheSettingManager->GetSettingS(dotted.c_str(), key.c_str(), text);
				live.StringValue = text;
			}
			out[key] = live;
		}
	}
	return true;
}

void PresetManager::ResolveAndApply(TESObjectCELL* Cell, bool Automatic) {
	PresetData chosen;
	ResolveResult outcome;
	if (!ChoosePreset(Cell, chosen, outcome)) return;

	// UNOFFICIAL 2026-10-19 (see the header): no authored preset = the player's own settings, one identity indoors and out.
	const bool ownSettings = outcome.UsedRawDefaults;
	std::string identity = ownSettings ? std::string("<own settings>") : outcome.PresetName;
	for (const std::string& variant : gVariantOrder) identity.append("+").append(variant);

	if (!gActive.haveOwn)
		gActive.haveOwn = CaptureLiveState(gActive.ownSettings); // the player's settings before anything is applied

	const char* place = (outcome.IsInterior ? outcome.CellEditorID : outcome.WorldspaceEditorID).c_str();

	if (identity == gActive.key && (Automatic || ownSettings)) {
		// Already active: live values (unsaved menu edits too) stay. An explicit Save, Reload or Delete still re-reads the file.
		gLastResult = outcome;
		Logger::Log("PresetManager: [Preset] Resolved %s -> '%s' again: already active, nothing re-applied (live settings kept)",
			place, identity.c_str());
		return;
	}

	if (Automatic && gActive.isOwn)
		CaptureLiveState(gActive.ownSettings); // leaving a place without a preset: remember the player's settings as they are now
	if (ownSettings)
		chosen = gActive.ownSettings; // instead of the shipped defaults ChoosePreset read

	OverlayVariants(chosen);
	PushPreset(chosen);
	gActive.key = identity;
	gActive.isOwn = ownSettings;
	gLastResult = outcome;
	++gGeneration;

	Logger::Log("PresetManager: [Preset] Resolved %s -> tier=%d name='%s' rawDefaults=%d keyword='%s' variants=%u%s",
		place, int(outcome.Tier), outcome.PresetName.c_str(), outcome.UsedRawDefaults, outcome.Keyword.c_str(),
		UInt32(gVariantOrder.size()), ownSettings ? " (the player's own settings)" : "");
}

void PresetManager::ForgetSession() {
	gActive.key.clear();
	gActive.isOwn = false;
	gActive.ownSettings.clear();
	gActive.haveOwn = false;
}

const ResolveResult& PresetManager::GetLastResolveResult() {
	return gLastResult;
}

bool PresetManager::ShouldReResolve(TESObjectCELL* cell) {
	if (cell == nullptr) return false;

	const bool indoors = cell->IsInterior();
	// Indoors presets are per cell, outdoors per worldspace; crossing between the two always counts.
	const bool changed = !gPlace.known || indoors != gPlace.indoors ||
		(indoors ? cell != gPlace.cell : cell->worldSpace != gPlace.world);
	if (!changed) return false;

	gPlace.known = true;
	gPlace.indoors = indoors;
	gPlace.cell = indoors ? cell : nullptr;
	gPlace.world = indoors ? nullptr : cell->worldSpace;
	return true;
}

// ---- public: browser, preview ---------------------------------------------------
// docs "In-game UI -- Preset browser / Load", "Unsaved/previewing state"

auto PresetManager::ListAllPresets() -> std::vector<PresetListEntry> {
	std::vector<PresetListEntry> listing;
	const fs::path folder(UnderGameRoot(kPresetsDir()));
	if (!IsFolder(folder)) return listing;

	for (const fs::path& file : IniFilesIn(folder)) {
		std::string name = file.stem().string();
		const PresetKind kind = IsReservedName(name) ? PresetKind::Default
			: gTags.cellsPerKeyword.count(name) ? PresetKind::Keyword
			: PresetKind::Override;
		listing.push_back({ std::move(name), kind });
	}

	// Keyword presets, then the defaults, then overrides; by name within each group.
	std::sort(listing.begin(), listing.end(), [](const PresetListEntry& first, const PresetListEntry& second) {
		return first.Kind != second.Kind ? int(first.Kind) < int(second.Kind) : first.Name < second.Name;
	});
	return listing;
}

void PresetManager::ApplyPreviewPreset(const PresetData& contents) {
	PushPreset(contents);
}

UInt32 PresetManager::GetResolveGeneration()
{
	return gGeneration;
}

// ---- public: variants -------------------------------------------------------------
// docs "Variants", "In-game UI -- Variants"

const std::vector<std::string>& PresetManager::GetEnabledVariants()
{
	return gVariantOrder;
}

void PresetManager::SetVariantEnabled(const std::string& name, bool enable) {
	const auto where = std::find(gVariantOrder.begin(), gVariantOrder.end(), name);
	const bool listed = where != gVariantOrder.end();
	if (enable && !listed)
		gVariantOrder.push_back(name); // at the end: highest priority
	else if (!enable && listed)
		gVariantOrder.erase(where); // removed, so enabling again puts it at the end
	WriteVariantList();
}

std::vector<std::string> PresetManager::ListAllVariants()
{
	std::vector<std::string> stems;
	const fs::path folder(UnderGameRoot(kVariantsDir()));
	if (!IsFolder(folder)) return stems;

	for (const fs::path& file : IniFilesIn(folder)) stems.push_back(file.stem().string());
	std::sort(stems.begin(), stems.end());
	return stems;
}

void PresetManager::CaptureVariantDiff(PresetData& into) {
	into.clear();

	PresetData now;
	if (!CaptureLiveState(now)) return;

	for (const auto& [dotted, values] : now) {
		const auto baseSection = gBaseline.find(dotted);
		for (const auto& [key, value] : values) {
			bool keep = true;	// nothing to compare with: counts as changed
			if (baseSection != gBaseline.end()) {
				const auto baseValue = baseSection->second.find(key);
				if (baseValue != baseSection->second.end() && baseValue->second.Type == value.Type)
					keep = !SameValue(baseValue->second, value);
			}
			if (keep) into[dotted][key] = value;
		}
	}
}

// docs "Refresh all presets"
auto PresetManager::RefreshAllPresets() -> std::vector<RefreshSummary> {
	std::vector<RefreshSummary> changedFiles;

	PresetData eligible; // the current defaults without the blacklisted sections
	if (!ReadShippedDefaults(eligible)) return changedFiles;

	for (const PresetListEntry& listed : ListAllPresets()) {
		PresetData contents;
		if (!ReadPreset(listed.Name, contents)) continue;

		RefreshSummary summary{ listed.Name, {} };

		for (const auto& [dotted, values] : eligible) {
			for (const auto& [key, shipped] : values) {
				if (HasKey(contents, dotted, key)) continue; // keys the file has are never touched
				contents[dotted][key] = shipped;

				char added[320];
				if (shipped.Type == ValueKind::String)
					snprintf(added, sizeof(added), "%s.%s = %s", dotted.c_str(), key.c_str(), shipped.StringValue.c_str());
				else
					snprintf(added, sizeof(added), "%s.%s = %g", dotted.c_str(), key.c_str(), shipped.FloatValue);
				summary.AddedKeys.emplace_back(added);
			}
		}

		// Stale keys are kept too: a refresh only ever adds.
		if (!summary.AddedKeys.empty() && WritePreset(listed.Name, contents))
			changedFiles.push_back(std::move(summary));
	}
	return changedFiles;
}
