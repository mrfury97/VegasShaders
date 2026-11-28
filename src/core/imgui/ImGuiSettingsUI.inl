// ---- What the menu leaves out --------------------------------------------------------------------------------------------

// Sections never listed: the weather mode, the status pages, the New-in-patch data and the developer page (its one switch is
// View > Developer options).
static bool ShouldHideSection(const std::string& name) {
	return name == "WeatherMode" || name == "Status" || name == "NewInPatch" || name == "Developer";
}

// Keys never drawn as rows: the old text menu's colours (TextColor*, TextShadow*), the LUT names (the LUT page has pickers for
// them), the New-in-patch and developer lists, and the sidebar width (set by dragging the sidebar's edge).
static bool ShouldHideKey(const char* key) {
	// 2026-11-11: the path tracer's three internal comparison keys, and the key that shows them (RtPathTrace.h InternalTools)
	if (!strcmp(key, "InternalTools")) return true;
	if ((!strcmp(key, "ReferencePT") || !strcmp(key, "PTReplace") || !strcmp(key, "PTDenoise")) && !RtPathTrace::InternalTools()) return true;
	if (!strncmp(key, "TextColor", 9) || !strncmp(key, "TextShadow", 10)) return true;
	static const std::unordered_set<std::string> kRowless = {
		"DayLUT", "NightLUT", "InteriorLUT",
		"NewSince", "NewInclude", "NewExclude", "NewDiagnostics",
		"DevOnly", "SidebarWidth",
		"Internal", "ShowInternal",	// UNOFFICIAL 2026-11-12: the Internal list and the file-only key that shows its settings
		"RTInteriors", "RTExteriors",	// UNOFFICIAL 2026-11-15: the RT master switches, drawn at the top of the Ray tracing page (DrawRtSpaceSwitches)
	};
	return kRowless.count(key) != 0;
}

// ---- Unsaved marks (2026-11-25) -----------------------------------------------------------------------------------------
// A row, heading or sidebar entry that holds a setting changed since the last Save (or Revert) is drawn in bold with " *"
// (MenuWidgets' marked flags and MarkLastItem; UnsavedSettings.h keeps the list). A setting's row is marked by its own key. A
// control that shows or writes several keys (a mode picker, a master switch) is marked by its SCOPE, the keys behind the value it
// shows, written as a comma-separated list of:
//   "Section:Key"  that one key
//   "Section"      every key of the section and of the sections under it
//   "@Effect"      the effect's own on/off switch (the one beside it in the sidebar)
// The sidebar's entries are marked from the same facts further down ("Unsaved marks: the sidebar").

// The section of an effect's own on/off switch (its key is "Enabled"), as SettingManager::SetMenuShaderEnabled writes it; empty
// for an effect with no switch.
static std::string EffectSwitchSection(const std::string& effect) {
	if (effect == "HudRaindrops") return "Shaders.WetnessEffects.HudRaindrops";
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(effect.c_str());
	if (kind == SettingManager::MenuSwitch::None) return std::string();
	return "Shaders." + effect + (kind == SettingManager::MenuSwitch::MainEnabled ? ".Main" : ".Status");
}

static bool EffectSwitchMarked(const std::string& effect) {
	const std::string section = EffectSwitchSection(effect);
	return !section.empty() && UnsavedSettings::Key(section.c_str(), "Enabled");
}

// One of the section's rows is unsaved. A key that is no row (ShouldHideKey; the RT master switches, drawn at the top of the Ray
// tracing page) is marked where it is drawn instead.
static bool SectionRowsMarked(const char* section) {
	const UnsavedSettings::Table& all = UnsavedSettings::All();
	const auto sec = all.find(section);
	if (sec == all.end()) return false;
	for (const auto& key : sec->second) if (!ShouldHideKey(key.first.c_str())) return true;
	return false;
}

// Whether [section] key is one of the scope's keys.
static bool ScopeHas(const char* scope, const std::string& section, const std::string& key) {
	for (const char* p = scope; *p; ) {
		while (*p == ' ' || *p == ',') p++;
		const char* end = p;
		while (*end && *end != ',') end++;
		std::string item(p, end);
		item.erase(item.find_last_not_of(' ') + 1);
		p = end;
		if (item.empty()) continue;
		if (item[0] == '@') {
			if (key == "Enabled" && section == EffectSwitchSection(item.substr(1))) return true;
			continue;
		}
		const size_t colon = item.find(':');
		if (colon != std::string::npos) {
			if (section == item.substr(0, colon) && key == item.substr(colon + 1)) return true;
		}
		else if (section.compare(0, item.size(), item) == 0 && (section.size() == item.size() || section[item.size()] == '.')) return true;
	}
	return false;
}

// Whether one of the scope's keys is unsaved.
static bool ScopeMarked(const char* scope) {
	for (const auto& section : UnsavedSettings::All())
		for (const auto& key : section.second)
			if (ScopeHas(scope, section.first, key.first)) return true;
	return false;
}

// The scopes of the rows that pick a mode or switch several keys at once (the pickers are further down; the Quick settings,
// AO & bounce light and category pages share them, and the sidebar marks those pages' entries from them too).
// ---- Unsaved marks: controls that write more than they show (2026-11-26) ----------------------------------------------------
// A mode picker (AO / sky occlusion / bounce light, anti-aliasing, reflections, sun / cloud / lamp shadows, fog, wetness, exposure)
// or a switch that turns another one off (FSR / DLSS) writes more settings than the one it shows. The user's report: Ambient
// occlusion on turned Bounce light on with it, and with both off again Bounce light kept its "*" (ComputeGI and the two grid
// values were left changed underneath). So:
// - while such a control writes, PickerWrites names it the writer (UnsavedSettings::Owners keeps which control changed what);
// - when it is done and what it shows is what it showed when last saved (SavedMode: the same mode with the saved values), the
//   settings it changed go back to their saved values (Settle), and nothing is left unsaved underneath. A setting changed by hand
//   afterwards is no longer the control's and stays;
// - its row is marked when its mode differs from the saved one (ModeMarked), not by the settings underneath.
typedef std::function<int()> ModeOf;
static void SetShaderEnabled(const char* shader, bool wantOn);	// with the category pages

// What the mode was when last saved: worked out with the unsaved settings put back to their saved values for a moment.
static int SavedMode(const ModeOf& mode) {
	const auto live = TheSettingManager->PeekSavedBegin();
	const int saved = mode();
	TheSettingManager->PeekSavedEnd(live);
	return saved;
}

// The same, kept until the unsaved list changes (the rows ask every frame).
static int SavedModeCached(const char* id, const ModeOf& mode) {
	static std::map<std::string, std::pair<unsigned, int>> cache;	// id -> (UnsavedSettings::Revision() + 1, the saved mode)
	std::pair<unsigned, int>& c = cache[id];
	if (c.first != UnsavedSettings::Revision() + 1) c = std::make_pair(UnsavedSettings::Revision() + 1, SavedMode(mode));
	return c.second;
}

static bool ModeMarked(const char* id, const ModeOf& mode) { return UnsavedSettings::Any() && mode() != SavedModeCached(id, mode); }

// The control is back where it was saved: what it changed goes back to the saved values (an effect's switch through the effect).
static void Settle(const std::string& group, const ModeOf& mode) {
	if (!UnsavedSettings::Any() || mode() != SavedMode(mode)) return;
	std::vector<std::pair<std::string, std::string>> mine;
	for (const auto& owner : UnsavedSettings::Owners()) if (owner.second == group) mine.push_back(owner.first);
	if (mine.empty()) return;
	for (const auto& k : mine) {
		const auto sec = UnsavedSettings::All().find(k.first);
		if (sec == UnsavedSettings::All().end()) continue;
		const auto it = sec->second.find(k.second);
		if (it == sec->second.end() || it->second == UnsavedSettings::kNoValue) continue;
		const std::string saved = it->second;
		const size_t dot = k.first.rfind('.');
		const std::string effect = k.first.compare(0, 8, "Shaders.") == 0 && dot != std::string::npos && dot > 8 ? k.first.substr(8, dot - 8) : std::string();
		if (k.second == "Enabled" && !effect.empty() && EffectSwitchSection(effect) == k.first) SetShaderEnabled(effect.c_str(), saved == "true");
		else TheSettingManager->SetSettingS(k.first.c_str(), k.second.c_str(), saved.c_str());
	}
	TheSettingManager->LoadSettings();
}

// Put around a control's writes: { PickerWrites writes("group", Mode); ...SetSetting... }
struct PickerWrites {
	std::string Group;
	ModeOf Mode;
	PickerWrites(std::string group, ModeOf mode) : Group(std::move(group)), Mode(std::move(mode)) { UnsavedSettings::Writer() = Group; }
	~PickerWrites() { UnsavedSettings::Writer().clear(); Settle(Group, Mode); }
};

static const char* const kScopeAllEffects = "Main.Main.Misc:RenderEffects";
static const char* const kScopeMaxQuality = "Main.Main.Quality:MaxQuality";
static const char* const kScopeAoMode = "@AmbientLight, @AmbientOcclusion, Shaders.AmbientOcclusion.RT:Enabled, Shaders.AmbientOcclusion.RT:OcclusionOnD3D9, "
	"Shaders.AmbientOcclusion.GTAO:Enabled";
static const char* const kScopeSkyMode = "Shaders.AmbientOcclusion.Skylighting:Enabled, Shaders.AmbientOcclusion.RT:Sky";
static const char* const kScopeGiMode = "@AmbientLight, @BounceLight, Shaders.AmbientLight.Main:ComputeGI, Shaders.AmbientOcclusion.RT:GI, "
	"Shaders.AmbientLight.Main:GridBounce, Shaders.AmbientLight.Main:GridLamps";
static const char* const kScopeAaMode = "Main.Main.ReducedQuality:FXAA, @SMAA, @TAA, Shaders.TAA.FSR:Enabled, Shaders.TAA.DLSS:Enabled";

// Kept in the file for old setups but read by nothing (the code that read them is commented out): drawn greyed, not hidden.
static bool IsInertSetting(const char* section, const char* key) {
	const bool shadowMaps = !strcmp(section, "Shaders.ShadowsExteriors.ShadowMaps");
	return shadowMaps && (!strcmp(key, "Mipmaps") || !strcmp(key, "Anisotropy"));
}

// A tooltip of wrapped text, about 28 characters of the current font wide per em.
static void WrappedTooltip(const char* text) {
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

// ---- Menu honesty ----------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-19 (P81 playtest reports: "lotta effects u gotta restart", "shadow blurring does not work", "a lot of
// interior shadow settings don't do anything"). The menu now says which settings wait for a game restart, greys the ones the
// current shadow mode or switch does not read (the reason on hover; they stay editable), and keeps test switches and settings
// nothing reads out of sight unless Display > "Show test settings" is ticked.

// 1. Settings read only when the game starts: compiled into the shaders (ShaderRecord::BuildDefines, EffectRecord::LoadEffect),
// or used to install hooks or make textures at start-up. TurnOn: built in only when on at start, switching it off works at
// once; Changed: any change waits for the restart. MaxQuality has its own note (RenderMaxQuality).
enum class RestartRule { Changed, TurnOn };
struct RestartSetting { const char* Section; const char* Key; RestartRule Rule; };
static const RestartSetting kRestartSettings[] = {
	{ "Main.Main.Misc",                       "MoreLampsPerObject", RestartRule::Changed },	// engine patch + SPECULAR_LAMPS6
	{ "Main.Main.Misc",                       "LampReachMargin",    RestartRule::Changed },	// engine patch (Hooks.cpp)
	{ "Main.Main.Performance",                "ParallelShaderCompile", RestartRule::Changed },
	{ "Main.Main.Performance",                "SlimDepthBuffer",    RestartRule::Changed },	// depth texture format (CombineDepth)
	{ "Shaders.ShadowsExteriors.Main",        "ForwardShadows",     RestartRule::TurnOn },
	{ "Shaders.ShadowsExteriors.SSShadows",   "Enabled",            RestartRule::TurnOn },	// UNOFFICIAL 2026-11-26 Bottled Shaders / Bend SSS texture at start
	{ "Main.Main.Performance",                "NvrDxvkDisplacement", RestartRule::TurnOn },	// land shaders registered as they load ([Shaders.Terrain.Displacement] Enabled is live)
	{ "Shaders.ShadowsExteriors.SunSmoothing","CrossFade",          RestartRule::Changed },
	{ "Shaders.ShadowsExteriors.SunSmoothing","DeferredFade",       RestartRule::TurnOn },	// UNOFFICIAL 2026-11-15 fd-smoothsun (compiled into the deferred sun pass at startup)
	{ "Shaders.ContactHardening.Status",      "Enabled",            RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.Forward",     "Enabled",            RestartRule::TurnOn },	// hooks + shader overrides at start
	{ "Shaders.ShadowsInteriors.Forward",     "SmoothEdges",        RestartRule::Changed },
	{ "Shaders.ShadowsInteriors.Forward",     "EvenTaps",           RestartRule::Changed },	// POINT_SHADOW_EVEN (deep/lampedge)
	{ "Shaders.ShadowsInteriors.Forward",     "MagnifiedSoftness",  RestartRule::TurnOn },	// POINT_SHADOW_MAG when above 0 at start; 0 is live
	{ "Shaders.ShadowsInteriors.Forward",     "TexelFilter",        RestartRule::Changed },	// POINT_SHADOW_TEXEL (deep/lampedge)
	{ "Shaders.ShadowsInteriors.RT",          "Enabled",            RestartRule::Changed },	// POINT_SHADOW_RT (RtLampShadows.h); Live is live
	{ "Shaders.ShadowsInteriors.Main",        "ShadowCubeMapSize",  RestartRule::Changed },	// cube maps made at start (ShadowManager)
	{ "Shaders.AmbientLight.Main",           "LowScaleFullRes",    RestartRule::Changed },	// UNOFFICIAL 2026-11-01: buffers made at start (AmbientLight.cpp EnsureBuffers)
	{ "Shaders.ShadowsInteriors.Atlas",       "NearCubeSize",       RestartRule::Changed },	// UNOFFICIAL 2026-11-01: cube slots 0-7 made at start (ShadowsExterior.cpp)
	{ "Shaders.PBR.Main",                     "LampReachSkip",      RestartRule::Changed },
	{ "Shaders.PBR.Main",                     "UnlitShadowSkip",    RestartRule::Changed },
	{ "Shaders.PBR.Main",                     "SmithCorrelated",    RestartRule::Changed },
	{ "Shaders.PBR.Main",                     "SpecularFade",       RestartRule::Changed },	// SPEC_FADE_ON (deep/pbrpop)
	{ "Shaders.PBR.Main",                     "FoldHighlights",     RestartRule::TurnOn },	// FOLD_HIGHLIGHTS_ON (deep/pbrtiles); off is live
	{ "Shaders.PBR.Main",                     "LampSums",           RestartRule::TurnOn },	// LAMP_SUMS_ON (deep/lampsums); off is live
	{ "Shaders.PBR.Main",                     "HighlightSums",      RestartRule::TurnOn },	// HIGHLIGHT_SUMS_ON (2026-10-25); off is live
	{ "Shaders.PBR.Main",                     "SkylightingMode",    RestartRule::Changed },
	{ "Shaders.PBR.Metal",                    "CubeReflections",    RestartRule::TurnOn },	// LIVE_CUBE: either above 0 at start
	{ "Shaders.PBR.EnvMaps",                  "CubeReflections",    RestartRule::TurnOn },
	{ "Shaders.PBR.MaterialRules",            "Enabled",            RestartRule::TurnOn },
	{ "Shaders.PBR.Rmaos",                    "Enabled",            RestartRule::Changed },
	{ "Shaders.PBR.Rmaos",                    "SpecularFromMap",    RestartRule::Changed },	// RMAOS_SPEC_OFF (2026-11-02)
	{ "Shaders.Reflections.Main",             "MaterialAware",      RestartRule::TurnOn },	// REFL_MATERIAL_ON (core/ReflMaterial.h)
	{ "Shaders.Reflections.Main",             "CubeResolution",     RestartRule::Changed },	// live cube textures made at start (Reflections.cpp)
	{ "Shaders.Reflections.Main",             "ReflectAdd",         RestartRule::TurnOn },	// REFL_ADD_ON (Reflections.fx.hlsl)
	{ "Shaders.Reflections.Main",             "SpecularOcclusion",  RestartRule::TurnOn },	// REFL_SPECOCC_ON (Reflections.fx.hlsl)
	{ "Shaders.PBR.Linear",                   "Enabled",            RestartRule::TurnOn },	// off at runtime = Gamma 1
	{ "Shaders.PBR.Linear",                   "PreDecode",          RestartRule::Changed },
	{ "Shaders.Sky.Physical",                 "Enabled",            RestartRule::TurnOn },
	{ "Shaders.Terrain.Blending",             "Enabled",            RestartRule::TurnOn },
	{ "Shaders.Terrain.Variation",            "Enabled",            RestartRule::Changed },	// TERRAIN_VARIATION (deep/csports)
	{ "Shaders.Terrain.Variation",            "Layers",             RestartRule::Changed },	// TERRAIN_VARIATION_LAYERS
	{ "Shaders.Terrain.Macro",                "Enabled",            RestartRule::Changed },	// TERRAIN_MACRO (2026-11-04)
	{ "Shaders.PBR.Dust",                     "Enabled",            RestartRule::Changed },	// DUST_ON (2026-11-04)
	{ "Shaders.PBR.MicroShadow",              "Enabled",            RestartRule::Changed },	// MICRO_SHADOW_ON (2026-11-04)
	{ "Shaders.PBR.Main",                     "VanillaBridge",      RestartRule::Changed },	// VANILLA_BRIDGE_ON (2026-11-05)
	{ "Shaders.PBR.Main",                     "VanillaBridgeDielectricMax", RestartRule::Changed },	// VANILLA_BRIDGE_DIELECTRIC_MAX (2026-11-06)
	{ "Shaders.HandBlood.Status",             "Enabled",            RestartRule::Changed },
	{ "Shaders.Cloth.Main",                   "Enabled",            RestartRule::TurnOn },
	{ "Shaders.Puddles.Status",               "Enabled",            RestartRule::Changed },	// WETNESS_EFFECTS 2 (2026-11-08)
	{ "Shaders.Puddles.Main",                 "MapMode",            RestartRule::Changed },	// WETNESS_EFFECTS 3 (2026-11-12)
	{ "Shaders.WetnessEffects.HudRaindrops",  "Enabled",            RestartRule::TurnOn },	// HUD_RAIN_ON: on at start
	{ "Shaders.VolumetricClouds.Main",        "Resolution",         RestartRule::Changed },	// textures made at start
	{ "Shaders.VolumetricClouds.Main",        "HalfResolution",     RestartRule::Changed },
	{ "Shaders.VolumetricClouds.Main",        "ShadowResolution",   RestartRule::Changed },
	{ "Shaders.Skin.Main",                    "OverrideVanillaPlusSkin", RestartRule::Changed },	// picks the SKIN shader files at start (with Vanilla Plus Skin installed)
	// UNOFFICIAL 2026-11-07 (perf-ptcost) the path tracer's images made at the RT helper's start (RtPathTrace.cpp Setup): turned on later they
	// wait for the next helper start (a game restart, or reloadshaders); RtPathTrace keeps its other features running meanwhile
	{ "Main.Main.RayTracing",                 "PTRestirGI",         RestartRule::TurnOn },
	{ "Main.Main.RayTracing",                 "PTDenoiseSplit",     RestartRule::TurnOn },
	{ "Main.Main.RayTracing",                 "PTCacheEntriesLog2", RestartRule::Changed },
	// Renderer, pipelines, hooks and scene setup read at start
	{ "Main.Main.Quality",                    "DeferredSkin",       RestartRule::Changed },
	{ "Main.Main.Quality",                    "DeferredHair",       RestartRule::Changed },
	{ "Main.Main.Quality",                    "DeferredTransparents", RestartRule::Changed },
	{ "Main.Main.Quality",                    "DeferredFirstPerson", RestartRule::Changed },
	{ "Main.Main.Quality",                    "DeferredFoliageLamps", RestartRule::Changed },
	{ "Main.Main.Quality",                    "DeferredRtSunFirst", RestartRule::Changed },
	{ "Main.Main.Quality",                    "DeferredADFog",      RestartRule::Changed },
	{ "Main.Main.Quality",                    "CapturedBumpNormals", RestartRule::Changed },
	{ "Main.Main.Misc",                       "PresetOldDefaults",  RestartRule::Changed },
	{ "Main.Main.Misc",                       "ReplaceIntro",       RestartRule::Changed },
	{ "Shaders.WetnessEffects.Status",        "Enabled",            RestartRule::Changed },
	{ "Shaders.TAA.Cutouts",                  "Enabled",            RestartRule::TurnOn },
	{ "Shaders.TAA.Cutouts",                  "FarCoverage",        RestartRule::Changed },
	{ "Shaders.Flashlight.Deferred",          "ShadowMapSize",      RestartRule::Changed },
	{ "Shaders.PipboyFit.Main",               "Enabled",            RestartRule::Changed },
	{ "Shaders.ShadowsExteriors.ShadowMaps",  "CharacterShadowMap", RestartRule::Changed },
	{ "Shaders.ShadowsExteriors.Main",        "CharacterObjectReach", RestartRule::Changed },
	{ "Shaders.ShadowsInteriors.RT",          "Exteriors",          RestartRule::Changed },
	{ "Shaders.ShadowsInteriors.RT",          "ManyLights",         RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.RT",          "ShadowDenoiser",     RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.RT",          "DenoiseLamps",       RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.RT",          "Denoise",            RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.RT",          "ExteriorForward",    RestartRule::Changed },
	{ "Shaders.ShadowsInteriors.RT",          "ExteriorManyLights", RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.RT",          "Flashlight",         RestartRule::Changed },
	{ "Shaders.SnowAccumulation.Main",        "CapturedNormals",    RestartRule::Changed },
	{ "Shaders.PBR.EnvMaps",                  "FromGameData",       RestartRule::Changed },
	{ "Shaders.VolumetricFog.Interiors",      "LampHazeShadowCell", RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "Actors",             RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "AlphaTested",        RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "HitTextures",        RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "AlphaDetail",        RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "AlbedoDetail",       RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "RealNormalMaps",     RestartRule::Changed },
	{ "Main.Main.RayTracing",                 "Grass",              RestartRule::Changed },
};

static std::unordered_map<std::string, float> s_startupValues;	// "Section.Key" -> its value when the game started

static float NumericSetting(const char* section, const char* key) {
	SettingManager::Configuration::ConfigNode node;
	if (!TheSettingManager->Config.FillNode(&node, section, key)) return 0.0f;
	using Config = SettingManager::Configuration;
	if (node.Type == Config::Boolean) return node.BoolValue ? 1.0f : 0.0f;
	return node.Type == Config::Integer ? (float)node.IntValue : node.FloatValue;
}

// From ImGuiManager::Initialize (the first rendered frame, before any location loads and before the menu can change anything).
static void CaptureStartupValues() {
	for (const RestartSetting& r : kRestartSettings)
		s_startupValues[std::string(r.Section) + "." + r.Key] = NumericSetting(r.Section, r.Key);
}

// What the start-up reads make of the setting: the value itself, or the on/off the define is built from.
static float RestartState(const RestartSetting& r, bool atStart) {
	auto get = [atStart](const char* section, const char* key) {
		if (!atStart) return NumericSetting(section, key);
		auto it = s_startupValues.find(std::string(section) + "." + key);
		return it != s_startupValues.end() ? it->second : NumericSetting(section, key);
	};
	const std::string key = r.Key;
	if (key == "CubeReflections")
		return (get("Shaders.PBR.Metal", "CubeReflections") > 0.0f || get("Shaders.PBR.EnvMaps", "CubeReflections") > 0.0f) ? 1.0f : 0.0f;
	if (key == "SmithCorrelated") return get(r.Section, r.Key) == 1.0f ? 1.0f : 0.0f;
	return get(r.Section, r.Key);
}

static const RestartSetting* FindRestartSetting(const char* section, const char* key) {
	for (const RestartSetting& r : kRestartSettings)
		if (!strcmp(r.Section, section) && !strcmp(r.Key, key)) return &r;
	return nullptr;
}

// What a restart-only setting's row says: "(restart)" or "(on after a restart)" in grey, or "restart to apply" in orange once
// it differs from what the game started with. Nothing for settings that work at once (a TurnOn one that was on at start).
struct RestartNote {
	const char* text = nullptr;
	bool pending = false;
	RestartRule rule = RestartRule::Changed;
};

static bool FindRestartNote(const char* section, const char* key, RestartNote& note) {
	const RestartSetting* r = FindRestartSetting(section, key);
	if (!r || s_startupValues.empty()) return false;
	const float atStart = RestartState(*r, true);
	const float now = RestartState(*r, false);
	const bool turnOn = r->Rule == RestartRule::TurnOn;
	note.rule = r->Rule;
	note.pending = turnOn ? (atStart == 0.0f && now != 0.0f) : atStart != now;
	if (turnOn && !note.pending && atStart != 0.0f) return false;	// built in: on and off both work at once
	note.text = note.pending ? "restart to apply" : turnOn ? "(on after a restart)" : "(restart)";
	return true;
}

static float RestartNoteWidth(const RestartNote& note) {
	return note.text ? ImGui::CalcTextSize(note.text).x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
}

static const ImVec4 kRestartOrange(1.0f, 0.6f, 0.2f, 1.0f);

static void DrawRestartNote(const RestartNote& note) {
	if (note.pending) ImGui::TextColored(kRestartOrange, "%s", note.text);
	else ImGui::TextDisabled("%s", note.text);
	if (!ImGui::IsItemHovered()) return;
	const char* explain =
		note.pending ? "Changed since the game started. Press Save, then restart the game: until then the game keeps what it "
			"started with."
		: note.rule == RestartRule::TurnOn ? "This was off when the game started, so it is not built in yet: turn it on, press "
			"Save and restart the game. (When the game starts with it on, switching it off and on works at once.)"
		: "Read when the game starts (built into the shaders or the game's setup): change it, press Save, then restart the game.";
	WrappedTooltip(explain);
}

// The note on the same line as what was drawn last (the sidebar's and the page header's switches).
static void RestartBadge(const char* section, const char* key) {
	RestartNote note;
	if (!FindRestartNote(section, key, note)) return;
	ImGui::SameLine();
	DrawRestartNote(note);
}

// How many restart-only settings changed since the game started (the status bar). Counted again when a setting changes.
static int PendingRestartCount() {
	static UInt32 countedAt = 0xFFFFFFFFu;
	static int count = 0;
	if (countedAt == TheSettingManager->SettingsRevision) return count;
	countedAt = TheSettingManager->SettingsRevision;
	count = 0;
	for (const RestartSetting& r : kRestartSettings) {
		RestartNote note;
		if (FindRestartNote(r.Section, r.Key, note) && note.pending) count++;
	}
	return count;
}

// 2. Settings the current mode does not read: the reason, or nullptr when it is in use. Each rule follows the code that reads it.
static bool IsDeadSetting(const char* section, const char* key) {
	// Nothing in this build reads these (kept in the file for old settings files).
	static const char* const dead[][2] = {
		{ "Shaders.ShadowsExteriors.Main", "OrthoMapResolution" },	// the ortho map size is [Shaders.ShadowsExteriors.Ortho] Resolution
		{ "Shaders.ShadowsExteriors.Main", "OrthoRadius" },			// its reach is [Shaders.ShadowsExteriors.Ortho] Distance
		{ "Shaders.ShadowsInteriors.Main", "Quality" },				// "Not currently used": only Oblivion's ShadowCube.hlsl read it
		{ "Shaders.ShadowsInteriors.Main", "TorchesCastShadows" },	// its hook (AddCastShadowFlag) is not installed in New Vegas
		{ "Shaders.ShadowsInteriors.Main", "UseCastShadowFlag" },	// read into Settings.Interiors, never used
		{ "Shaders.ShadowsInteriors.Main", "PlayerShadowFirstPerson" },	// the same
		{ "Shaders.ShadowsInteriors.Main", "PlayerShadowThirdPerson" },	// the same
		{ "Shaders.Snow.Main", "Flakes" },
	};
	for (const auto& d : dead) if (!strcmp(d[0], section) && !strcmp(d[1], key)) return true;
	return false;
}

static const char* InactiveReason(const char* section, const char* key) {
	static char reason[256];
	const std::string sec = section;
	auto is = [key](std::initializer_list<const char*> keys) {
		for (const char* k : keys) if (!strcmp(k, key)) return true;
		return false;
	};

	if (IsDeadSetting(section, key)) return "Does nothing: nothing in the mod reads this setting (kept for old settings files).";

	// Sun shadows (ShadowsExteriorEffect::UpdateSettings): Quality 0-3 set these themselves; only 4 (Custom) reads them.
	const bool shadowMaps = sec == "Shaders.ShadowsExteriors.ShadowMaps";
	const bool forms = sec.rfind("Shaders.ShadowsExteriors.Forms", 0) == 0 && sec != "Shaders.ShadowsExteriors.FormsOrtho";
	if (shadowMaps || forms) {
		const int quality = (int)NumericSetting("Shaders.ShadowsExteriors.Main", "Quality");
		const bool custom = quality < 0 || quality > 3;
		if (!custom && (forms || is({ "Distance", "CascadeLambda", "SmoothFilter", "LimitFrequency", "CascadeResolution" }))) {
			sprintf_s(reason, "Not used now: shadow Quality (Shadows Exteriors > Main) is %d, which sets this itself. Only Quality 4 (Custom) reads it.", quality);
			return reason;
		}
	}
	// UNOFFICIAL 2026-11-15 fd-smoothsun (core/SunFadeDeferred.h): the deferred renderer's half-degree steps replace these
	if (sec == "Shaders.ShadowsExteriors.SunSmoothing" && SunFadeDeferred::CompiledIn() && is({ "CrossFade", "YawStepSize", "PitchStepSize", "GlideSeconds" }))
		return "Not used while the deferred renderer draws the sun outdoors: DeferredFade (in this section) moves the shadows in half-degree steps, each faded in. Used indoors and in frames drawn without the deferred renderer.";
	if (sec == "Shaders.ShadowsExteriors.SunSmoothing" && is({ "CrossFade" }) && NumericSetting("Shaders.ShadowsExteriors.Main", "ForwardShadows") == 0.0f)
		return "Only used with ForwardShadows on (Shadows Exteriors > Main).";

	// Lamp shadows indoors (PointShadowForward, ShadowsExteriorEffect::UpdateSettings, ShaderManager's darkening pass).
	if (sec == "Shaders.ShadowsInteriors.Forward" && !is({ "Enabled", "ScopeFix" })) {
		if (NumericSetting(section, "Enabled") == 0.0f)
			return "Only used with Forward Enabled on (the first switch in this section); with it off the old screen-space lamp shadows are used.";
		if (is({ "Quality", "ContactHardening", "ContactHardeningStrength", "ContactHardeningMinSoftness", "LampSize", "MaxSoftness", "InvisibleLightSize" }) && NumericSetting(section, "Softness") <= 0.0f)
			return "Only used when Softness is above 0: sharp lamp shadows (Softness 0) take a single lookup.";
		if (is({ "ContactHardeningStrength", "ContactHardeningMinSoftness" }) && NumericSetting(section, "ContactHardening") == 0.0f)
			return "Only used with ContactHardening on.";
		if (is({ "EvenTaps", "MagnifiedSoftness", "SmoothEdges" }) && NumericSetting(section, "TexelFilter") != 0.0f)
			return "Not used with TexelFilter on (it filters every lamp shadow edge the way the sun's shadows are; restart after changing TexelFilter).";
		if (is({ "TexelFilter" }) && (NumericSetting(section, "Softness") <= 0.0f || NumericSetting(section, "Quality") < 1.0f))
			return "Only used with Softness above 0 and Quality 1 or more (sharp lamp shadows take a single lookup).";
		if (is({ "EvenTaps" }) && NumericSetting(section, "Quality") < 2.0f)
			return "Only changes Quality 2 (High) and 3 (Ultra): Low and Medium have no extra lookups.";
		if (is({ "MagnifiedSoftness" }) && (NumericSetting(section, "Softness") <= 0.0f || NumericSetting(section, "Quality") < 1.0f))
			return "Only used with Softness above 0 and Quality 1 or more (sharp lamp shadows take a single lookup).";
		if (is({ "ContactHardening", "ContactHardeningStrength", "ContactHardeningMinSoftness" }) && NumericSetting(section, "LampSize") > 0.0f)
			return "Not used while LampSize is above 0 (LampSize decides how soft each shadow is).";
		if (is({ "MaxSoftness" }) && NumericSetting(section, "LampSize") <= 0.0f && NumericSetting(section, "InvisibleLightSize") <= 0.0f)
			return "Only used when LampSize or InvisibleLightSize is above 0.";
		if (is({ "InvisibleLightSize" }) && NumericSetting(section, "InvisibleLightShadowStrength") <= 0.0f)
			return "Not used while InvisibleLightShadowStrength is 0 (invisible lights then cast no shadow at all).";
		if (is({ "InvisibleShadowFromLamp" }) && NumericSetting(section, "InvisibleLightShadowStrength") <= 0.0f)
			return "Not used while InvisibleLightShadowStrength is 0 (invisible lights then cast no shadow at all).";
		if (is({ "LampModelDistance", "LampModelOwnDistance" }) && NumericSetting(section, "LampModelShadows") != 0.0f)
			return "Only used with LampModelShadows off.";
		if (is({ "FixtureDistance" }) && NumericSetting(section, "InvisibleLightShadowStrength") >= 1.0f)
			return "Only used when InvisibleLightShadowStrength is below 1.";
		if (is({ "DarkeningBlur", "MaskHalfRes" }) && NumericSetting(section, "KeepDarkening") == 0.0f)
			return "Only used with KeepDarkening on (with it off the room darkening pass is not drawn).";
	}
	if (sec == "Shaders.ShadowsInteriors.Main" && is({ "ContactLength", "ContactThickness", "ContactDistance" }) && NumericSetting(section, "ContactShadows") <= 0.0f)
		return "Only used when ContactShadows is above 0.";
	// UNOFFICIAL 2026-10-19 Reflections GridFallback (ReflectionsEffect::GridStep): reads Ambient Light's light grid, made only while it runs with GridBounce.
	if (sec == "Shaders.Reflections.Main" && is({ "GridFallback", "GridBrightness" }) &&
		(NumericSetting("Shaders.AmbientLight.Status", "Enabled") == 0.0f || NumericSetting("Shaders.AmbientLight.Main", "GridBounce") <= 0.0f))
		return "Only used while Ambient Light is on with GridBounce above 0 (Ambient Light > Main): it reads the light grid that makes.";
	if (sec == "Shaders.Reflections.Main" && is({ "GridBrightness" }) && NumericSetting(section, "GridFallback") == 0.0f)
		return "Only used with GridFallback on.";
	// UNOFFICIAL 2026-10-20 MaterialAware (core/ReflMaterial.h): the material comes from the _rmaos maps.
	if (sec == "Shaders.Reflections.Main" && is({ "MaterialAware", "MaterialReflect", "MaterialMaxRoughness" }) && NumericSetting("Shaders.PBR.Rmaos", "Enabled") == 0.0f)
		return "Only used with the _rmaos maps on ([Shaders.PBR.Rmaos] Enabled): the materials come from them.";
	if (sec == "Shaders.Reflections.Main" && is({ "MaterialReflect", "MaterialMaxRoughness" }) && NumericSetting(section, "MaterialAware") != 1.0f)
		return "Only used with MaterialAware 1.";
	// UNOFFICIAL 2026-10-21 cloth indoors (core/ClothWind.h BeginFrame): the indoor set is read only while its switch is on; indoors
	// there is no weather, so the weather's wind is never used there.
	if (sec == "Shaders.Cloth.Interiors" && !is({ "Enabled" }) && NumericSetting(section, "Enabled") == 0.0f)
		return "Only used with Enabled on (the first switch in this section). With it off cloth stays still indoors "
			"(worn coats still follow you there: Cloth > Main MotionIndoors).";
	if (sec == "Shaders.Cloth.Main" && is({ "CalmFactor" }) && NumericSetting(section, "UseGameWind") == 0.0f)
		return "Only used with UseGameWind on.";
	if (sec == "Shaders.Cloth.Main" && is({ "WindDirX", "WindDirY", "WindDirZ" }) && NumericSetting(section, "UseGameWind") != 0.0f &&
		NumericSetting("Shaders.Cloth.Interiors", "Enabled") == 0.0f)
		return "Not used now: UseGameWind takes the direction from the weather. Used with UseGameWind off, and indoors "
			"with Cloth > Interiors on (no weather there).";

	// DXVK fork required (native D3D9 / stock DXVK ignores these)
	if (!DxvkCompute::Available()) {
		if ((sec == "Shaders.AmbientLight.Main" && is({ "ComputeGI", "ComputeGIStrength", "ComputeGIThickness", "ComputeGIThinOcclusion", "ComputeGIQuality" })) ||
			(sec == "Main.Main.Performance" && is({ "NvrDxvkPasses", "NvrDxvkClouds", "NvrDxvkDisplacement", "NvrDxvkAmbientLight" })) ||
			sec == "Shaders.Terrain.Displacement" || sec == "Shaders.POM.Displacement")
			return "Needs the DXVK fork's d3d9.dll: not active on standard D3D9 or stock DXVK.";
	}

	// Deferred renderer sub-features & GearWeather
	if (sec == "Main.Main.Quality") {
		if (is({ "DeferredSkinSSS", "DeferredSkinBlur" }) && NumericSetting("Main.Main.Quality", "DeferredSkin") == 0.0f)
			return "Only used while DeferredSkin is on.";
		if (is({ "DeferredALAlbedoGain" }) && NumericSetting("Main.Main.Quality", "DeferredALAlbedo") == 0.0f)
			return "Only used while DeferredALAlbedo is on.";
		if (!strncmp(key, "GearWeather", 11)) {
			if (NumericSetting("Main.Main.Quality", "DeferredFirstPerson") == 0.0f)
				return "Requires DeferredFirstPerson to be enabled (restart).";
			if (strcmp(key, "GearWeather") != 0 && NumericSetting("Main.Main.Quality", "GearWeather") == 0.0f)
				return "Only used while GearWeather is on.";
		}
	}
	if (sec.rfind("Shaders.WorldSurface.", 0) == 0) {
		if (sec != "Shaders.WorldSurface.Status" && NumericSetting("Shaders.WorldSurface.Status", "Enabled") == 0.0f)
			return "Only used while WorldSurface is enabled.";
		if (sec != "Shaders.WorldSurface.Main" && sec != "Shaders.WorldSurface.Status" &&
			strcmp(key, "Enabled") != 0 && NumericSetting(sec.c_str(), "Enabled") == 0.0f)
			return "Only used while this layer is enabled.";
	}

	// Skin: ScreenSpace blur takes over from per-pixel Scattering; Main/Scattering apply outdoors, Interiors applies indoors
	const bool isExt = TheShaderManager && TheShaderManager->GameState.isExterior;
	if (sec == "Shaders.Skin.Main" && is({ "Scattering" }) && NumericSetting("Shaders.Skin.Scattering", "ScreenSpace") != 0.0f)
		return "Overridden while ScreenSpace (Skin > Scattering) is on: the screen-space blur replaces per-pixel Scattering.";
	if (sec == "Shaders.Skin.Interiors" && is({ "Scattering" }) && NumericSetting("Shaders.Skin.Interiors", "ScreenSpace") != 0.0f)
		return "Overridden while ScreenSpace (Skin > Interiors) is on: the screen-space blur replaces per-pixel Scattering indoors.";
	if ((sec == "Shaders.Skin.Main" || sec == "Shaders.Skin.Scattering") && !is({ "OverrideVanillaPlusSkin", "MuteGameHighlights", "RainWetness" }) && !isExt)
		return "Applies outdoors only (you are indoors right now - adjust Skin > Interiors to see live changes here).";
	if (sec == "Shaders.Skin.Interiors" && isExt)
		return "Applies indoors only (you are outdoors right now - adjust Skin > Main / Scattering to see live changes here).";
	if (sec == "Shaders.Skin.SSS" && !is({ "Kernel" }) && NumericSetting(section, "Kernel") == 0.0f)
		return "Only used when Skin > SSS Kernel is non-zero (experimental SSS kernel).";

	// AmbientLight replaces standalone AmbientOcclusion and BounceLight (Skylighting still runs!)
	if (TheSettingManager->GetMenuShaderEnabled("AmbientLight")) {
		if (sec == "Shaders.AmbientOcclusion.Exteriors" || sec == "Shaders.AmbientOcclusion.Interiors" || sec == "Shaders.AmbientOcclusion.GTAO")
			return "Replaced while Ambient Light is on: Ambient Light does corner shading itself. Switch modes on Lighting > Ambient occlusion & bounce light.";
		if (sec == "Shaders.BounceLight.Main")
			return "Replaced while Ambient Light is on: Ambient Light handles bounce light. Switch modes on Lighting > Ambient occlusion & bounce light.";
	}

	// Shadow Darkness / Contrast rules
	if (sec == "Shaders.ShadowsExteriors.Main" && is({ "Darkness" }) && NumericSetting(section, "ForwardShadows") != 0.0f)
		return "Not used while ForwardShadows is on: shadows block the sun's light directly instead of darkening the screen.";
	if (sec == "Shaders.ShadowsInteriors.Main" && is({ "Darkness" }) &&
		NumericSetting("Shaders.ShadowsInteriors.Forward", "Enabled") != 0.0f && NumericSetting("Shaders.ShadowsInteriors.Forward", "KeepDarkening") == 0.0f)
		return "Not used while Forward lamp shadows are on with KeepDarkening off: lamps shadow their own light directly.";
	if (sec == "Shaders.ShadowsInteriors.Forward" && is({ "ShadowContrast" }))
		return "Not used while the deferred renderer is active (deferred lighting computes physical lamp shadows directly).";

	// PBR SmithCorrelated is not read by DeferredRenderer mode 2
	if (sec == "Shaders.PBR.Main" && is({ "SmithCorrelated" }))
		return "Not used in DeferredRenderer mode 2 (the deferred pass uses its own specular term).";

	// Exposure Mode 0 vs Mode 1 (Eye adaptation)
	if (sec == "Shaders.Exposure.Main" || sec == "Shaders.Exposure.Interiors" || sec == "Shaders.Exposure.Night") {
		const bool eyeAdapt = NumericSetting("Shaders.Exposure.Main", "Mode") == 1.0f;
		if (!eyeAdapt && is({ "AdaptExposure", "AdaptWhiteBalance", "AdaptWeatherLight" }))
			return "Only used with Eye adaptation on (Exposure Mode 1).";
		if (eyeAdapt && is({ "MinBrightness", "MaxBrightness", "DarkAdaptSpeed", "LightAdaptSpeed", "Histogram", "HistogramLowPercent", "HistogramHighPercent" }))
			return "Not used while Eye adaptation (Mode 1) is on: Mode 1 uses AdaptExposure / AdaptWhiteBalance instead.";
	}

	// VolumetricLight Froxels replaces raymarch Steps / BlurRadius / Temporal
	if (sec == "Shaders.VolumetricLight.Main" && is({ "Steps", "BlurRadius", "Temporal" }) && NumericSetting(section, "Froxels") != 0.0f)
		return "Not used while Froxels is on (the froxel grid replaces the screen-space raymarch).";

	// Reflections CloudSky requires NvrDxvkClouds
	if (sec == "Shaders.Reflections.Main" && is({ "CloudSky" }) &&
		(!DxvkCompute::Available() || NumericSetting("Main.Main.Performance", "NvrDxvkClouds") == 0.0f))
		return "Only used when the DXVK fork is loaded and NvrDxvkClouds (Main > Performance > DXVK) is on.";

	// WetWorld vs WetnessEffects & Puddles
	if (sec == "Shaders.WetWorld.Main" && TheSettingManager->GetMenuShaderEnabled("WetnessEffects"))
		return "Replaced while WetnessEffects is on (turn WetnessEffects off to use the legacy WetWorld effect).";
	if (sec == "Shaders.Puddles.Main" && !TheSettingManager->GetMenuShaderEnabled("WetnessEffects"))
		return "Requires WetnessEffects to be enabled (Weather and water > WetnessEffects).";

	// CloudShadows replaced when VolumetricClouds casts shadows
	if (sec == "Shaders.CloudShadows.Main" && TheSettingManager->GetMenuShaderEnabled("VolumetricClouds") &&
		NumericSetting("Shaders.VolumetricClouds.Main", "Shadows") > 0.0f)
		return "Replaced while VolumetricClouds is on with Shadows above 0: volumetric clouds cast their own shadows.";

	// VolumetricClouds PhysicalShadows overrides Shadows darkness/strength (thick clouds block all direct sunlight)
	if (sec == "Shaders.VolumetricClouds.Main" && is({ "Shadows" }) && NumericSetting(section, "PhysicalShadows") != 0.0f)
		return "Not used while PhysicalShadows is on: thick clouds block all direct sunlight physically instead of using a fixed shadow darkness.";

	return nullptr;
}

// 3. Test switches (the "New in this patch" tab's Diagnostics rule) and settings nothing reads: hidden unless ticked.
static bool NewIsDiagnostic(const std::string& key, const std::string& description, const std::vector<std::string>& extra, const std::string& section);

static bool IsDevOnlySetting(const std::string& section, const std::string& key);
static bool IsInternalSetting(const std::string& section, const std::string& key);	// UNOFFICIAL 2026-11-12 (defined with IsDevOnlySetting)

// UNOFFICIAL 2026-11-12: an Internal setting ([Main.Menu.Developer] Internal) while the settings file does not ask for them
// (ShowInternal): never a row, on its page, in the search or in the simple views, and not counted as a hidden developer setting.
static bool InternalHidden(const std::string& section, const std::string& key) {
	if (ShelvedEffects::Key(section, key)) return true;	// UNOFFICIAL 2026-11-16: a shelved effect or key (release builds only), whatever ShowInternal says
	return !gMenu.showInternal && IsInternalSetting(section, key);
}

// Shown only with Developer options (2026-10-25: also the [Main.Menu.Developer] DevOnly list; 2026-11-12: and, with ShowInternal
// typed into the settings file, the Internal list).
static bool IsTestSetting(const SettingManager::Configuration::ConfigNode& node) {
	static const std::vector<std::string> none;
	return IsDeadSetting(node.Section, node.Key) || NewIsDiagnostic(node.Key, node.Description, none, node.Section) ||
		IsDevOnlySetting(node.Section, node.Key) || IsInternalSetting(node.Section, node.Key);
}

// UNOFFICIAL 2026-10-25 menu tidy: whole-number settings that are plain 0/1 switches are drawn as a checkbox (it writes 0 or 1, the
// type in the file stays). The ones listed here, and any whose description starts with "[0-1]"; a value other than 0 or 1 keeps the
// number box.
static bool IsOnOffInteger(const SettingManager::Configuration::ConfigNode& node) {
	static const char* const onOff[][2] = {
		{ "Main.Main.Performance", "ArrivalSampler" }, { "Main.Main.Performance", "IOWorker2" },
		{ "Main.Main.Performance", "FogReferenceTest" }, { "Main.Main.Performance", "LampMaskScissorCheck" },
		{ "Main.Main.Performance", "CubeFaceViewCullCheck" },
		{ "Shaders.Grass.Wind", "Log" }, { "Shaders.Plants.Wind", "Log" },
		{ "Shaders.DitherBuster.Main", "DebugView" }, { "Shaders.PBR.Metal", "DebugView" },
		{ "Shaders.ShadowsExteriors.SunSmoothing", "SmoothSun" }, { "Shaders.ShadowsExteriors.SunSmoothing", "QuantizeSun" },
		{ "Shaders.CloudShadows.Main", "HazardLog" }, { "Shaders.CloudShadows.Main", "FlickerWatch" },
		{ "Shaders.HandBlood.Main", "DebugView" }, { "Shaders.WetWorld.Main", "HudDebug" },
	};
	for (const auto& o : onOff) if (!strcmp(o[0], node.Section) && !strcmp(o[1], node.Key)) return true;
	return node.Description.compare(0, 5, "[0-1]") == 0;
}

// 2026-11-04 (user: "remove unofficial from all the different tooltips since we are no longer just a regular unofficial fork"):
// the text a description shows in the menu, without its "UNOFFICIAL" tags (and the date right after one). The settings files and
// the descriptions keep them: the New-in-patch list reads those dates.
static std::string ShownDescription(const std::string& text) {
	static const char kTag[] = "UNOFFICIAL";
	const size_t tagLength = sizeof(kTag) - 1;
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size()) {
		if (text.compare(i, tagLength, kTag) != 0) { out += text[i++]; continue; }
		i += tagLength;
		size_t j = i;
		while (j < text.size() && text[j] == ' ') j++;
		if (text.compare(j, 3, "(20") == 0) {	// its date: " (2026-10-31)"
			const size_t close = text.find(')', j);
			if (close != std::string::npos && close - j <= 24) i = close + 1;
		}
		while (i < text.size() && (text[i] == ',' || text[i] == ':' || text[i] == ' ')) i++;
		size_t end = out.size();
		while (end > 0 && out[end - 1] == ' ') end--;
		const bool sentenceStart = end == 0 || strchr(".!?\n]", out[end - 1]) != nullptr;
		if (sentenceStart && i < text.size()) out += (char)toupper((unsigned char)text[i++]);
	}
	return out;
}

// A setting's hover note: why it is greyed (if it is), then its description from the settings file. When the row's name was
// cut short, the full name comes first. normalText is the style's text colour (the row itself may be drawn red or grey).
static void SettingTooltip(const SettingManager::Configuration::ConfigNode& node, const char* why, const ImVec4& normalText, bool nameCut) {
	const bool described = !node.Description.empty();
	if (!described && !why && !nameCut) return;
	ImGui::BeginTooltip();
	ImGui::PushStyleColor(ImGuiCol_Text, normalText);
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	if (nameCut) ImGui::TextDisabled("%s", node.Key);
	if (why) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", why);
	if (why && described) ImGui::Separator();
	if (described) ImGui::TextUnformatted(ShownDescription(node.Description).c_str());
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::EndTooltip();
}

// ---- Key names ---------------------------------------------------------------------------------------------------------------
// DirectInput scan codes and the names the menu shows for them (the menu key's hint, the KeyEnable reference). Rows of keys
// that sit next to each other on the keyboard are spelled out once; the rest are listed one by one. Sorted by code.

struct KeyName { int code; std::string name; };

static const std::vector<KeyName>& KeyNames() {
	static std::vector<KeyName> names;
	if (!names.empty()) return names;
	auto run = [](int firstCode, const char* letters) {
		for (int i = 0; letters[i]; i++) names.push_back({ firstCode + i, std::string(1, letters[i]) });
	};
	run(0x02, "1234567890");
	run(0x10, "QWERTYUIOP");
	run(0x1E, "ASDFGHJKL");
	run(0x2C, "ZXCVBNM");
	for (int f = 1; f <= 10; f++) names.push_back({ 0x3A + f, "F" + std::to_string(f) });
	static const struct { int code; const char* name; } kSingles[] = {
		{ 0x01, "Escape" },
		{ 0x0C, "Minus (-)" },
		{ 0x0D, "Equals (=)" },
		{ 0x0E, "Backspace" },
		{ 0x0F, "Tab" },
		{ 0x1A, "[ Left Bracket" },
		{ 0x1B, "] Right Bracket" },
		{ 0x1C, "Enter" },
		{ 0x1D, "Left Ctrl" },
		{ 0x27, "Semicolon (;)" },
		{ 0x28, "Apostrophe (')" },
		{ 0x29, "Grave (`)" },
		{ 0x2A, "Left Shift" },
		{ 0x2B, "Backslash (\\)" },
		{ 0x33, "Comma (,)" },
		{ 0x34, "Period (.)" },
		{ 0x35, "Slash (/)" },
		{ 0x36, "Right Shift" },
		{ 0x37, "Numpad *" },
		{ 0x38, "Left Alt" },
		{ 0x39, "Space" },
		{ 0x3A, "Caps Lock" },
		{ 0x45, "Num Lock" },
		{ 0x46, "Scroll Lock" },
		{ 0x47, "Numpad 7" },
		{ 0x48, "Numpad 8" },
		{ 0x49, "Numpad 9" },
		{ 0x4A, "Numpad -" },
		{ 0x4B, "Numpad 4" },
		{ 0x4C, "Numpad 5" },
		{ 0x4D, "Numpad 6" },
		{ 0x4E, "Numpad +" },
		{ 0x4F, "Numpad 1" },
		{ 0x50, "Numpad 2" },
		{ 0x51, "Numpad 3" },
		{ 0x52, "Numpad 0" },
		{ 0x53, "Numpad ." },
		{ 0x57, "F11" },
		{ 0x58, "F12" },
		{ 0x9C, "Numpad Enter" },
		{ 0x9D, "Right Ctrl" },
		{ 0xB5, "Numpad /" },
		{ 0xB7, "Print Screen" },
		{ 0xB8, "Right Alt" },
		{ 0xC5, "Pause" },
		{ 0xC7, "Home" },
		{ 0xC8, "Up Arrow" },
		{ 0xC9, "Page Up" },
		{ 0xCB, "Left Arrow" },
		{ 0xCD, "Right Arrow" },
		{ 0xCF, "End" },
		{ 0xD0, "Down Arrow" },
		{ 0xD1, "Page Down" },
		{ 0xD2, "Insert" },
		{ 0xD3, "Delete" },
	};
	for (const auto& single : kSingles) names.push_back({ single.code, single.name });
	std::sort(names.begin(), names.end(), [](const KeyName& a, const KeyName& b) { return a.code < b.code; });
	return names;
}

static const char* DikToName(BYTE dik) {
	for (const KeyName& key : KeyNames())
		if (key.code == (int)dik) return key.name.c_str();
	return "Unknown";
}

// The KeyEnable "(?)" popup: every scan code with its key, in a scrolling two-column table.
static void ScanCodePopup() {
	if (!ImGui::BeginPopup("DIKReference")) return;
	ImGui::TextUnformatted("DirectInput Scancodes");
	ImGui::Separator();
	const ImGuiTableFlags look = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
	if (ImGui::BeginTable("diktbl", 2, look, ImVec2(220.0f, 320.0f))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		for (const char* heading : { "Code", "Key" }) ImGui::TableSetupColumn(heading);
		ImGui::TableHeadersRow();
		for (const KeyName& key : KeyNames()) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("0x%02X (%d)", key.code, key.code);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(key.name.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::EndPopup();
}

// ---- Colour triples ----------------------------------------------------------------------------------------------------------
// Three settings named <prefix>R, <prefix>G and <prefix>B are one colour: a hue wheel for the colour (largest channel 1) and an
// intensity that scales it. The wheel keeps its own copy while the menu is open: reading the three values back every frame would
// pin the wheel's dot to the edge and reset the intensity.

struct ColourWheel { float hue[3]; float intensity; };

static ColourWheel& WheelFor(const SettingManager::Configuration::ConfigNode* rgb[3], const std::string& prefix) {
	static std::unordered_map<std::string, ColourWheel> wheels;
	if (gMenu.resetWheels) {
		wheels.clear();
		gMenu.resetWheels = false;
	}
	const std::string id = std::string(rgb[0]->Section) + "." + prefix;
	const auto known = wheels.find(id);
	if (known != wheels.end()) return known->second;

	float value[3];
	for (int c = 0; c < 3; c++) value[c] = (float)atof(rgb[c]->Value);
	const float brightest = (std::max)({ value[0], value[1], value[2] });
	ColourWheel fresh = { { 0.0f, 0.0f, 0.0f }, 1.0f };
	if (brightest > 0.0f) {
		fresh.intensity = brightest;
		for (int c = 0; c < 3; c++) fresh.hue[c] = value[c] / brightest;
	}
	return wheels[id] = fresh;
}

// The value a setting had when it was first drawn since the menu opened (the ~ buttons go back to it).
static std::string& SessionValue(const char* section, const char* key, const char* now) {
	auto& values = gMenu.sessionValues[section];
	auto found = values.find(key);
	if (found == values.end()) found = values.emplace(key, now).first;
	return found->second;
}

// A drag box that also takes the +/- keys while the mouse is over it. Returns whether the value changed.
static bool DragWithStepKeys(const char* id, float* value, float speed, const char* format) {
	bool changed = ImGui::DragFloat(id, value, speed, 0.0f, 0.0f, format);
	if (ImGui::IsItemHovered() && (gMenu.plusKey || gMenu.minusKey)) {
		*value += gMenu.plusKey ? gMenu.step : -gMenu.step;
		changed = true;
	}
	return changed;
}

static void RenderColorTriple(SettingManager::Configuration::ConfigNode& nodeR, SettingManager::Configuration::ConfigNode& nodeG,
	SettingManager::Configuration::ConfigNode& nodeB, const std::string& prefix, const char* shownTitle = nullptr)	// shownTitle: a simple view's name
{
	SettingManager::Configuration::ConfigNode* rgb[3] = { &nodeR, &nodeG, &nodeB };
	ColourWheel& wheel = WheelFor(const_cast<const SettingManager::Configuration::ConfigNode**>(rgb), prefix);

	std::string title = shownTitle ? shownTitle : prefix;	// "FogColor_" reads "FogColor"
	while (!title.empty() && title.back() == '_') title.pop_back();

	std::string* atOpen[3];
	bool moved = false;
	for (int c = 0; c < 3; c++) {
		atOpen[c] = &SessionValue(rgb[c]->Section, rgb[c]->Key, rgb[c]->Value);
		moved |= *atOpen[c] != rgb[c]->Value;
	}
	bool unsaved = false;	// 2026-11-25 (unsaved marks): the three keys are one item, marked on its title
	for (int c = 0; c < 3; c++) unsaved = unsaved || UnsavedSettings::Key(rgb[c]->Section, rgb[c]->Key);

	ImGui::PushID(prefix.c_str());
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

	// Title line: the name in the skin's heading colour, then ~ (back to the values at menu open) while they differ
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(title.c_str(), 1.15f, unsaved);
	ImGui::PopStyleColor();
	const bool titleHovered = ImGui::IsItemHovered();
	if (moved) {
		ImGui::SameLine();
		if (ImGui::SmallButton("~")) {
			for (int c = 0; c < 3; c++) TheSettingManager->SetSettingS(rgb[c]->Section, rgb[c]->Key, atOpen[c]->c_str());
			TheSettingManager->LoadSettings();
			gMenu.resetWheels = true;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Revert to value at session start");
	}

	bool edited = false;
	// UNOFFICIAL 2026-10-25: the wheel follows the text size instead of filling a wide page
	ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22.0f));
	edited |= ImGui::ColorPicker3("##col", wheel.hue, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview);

	// The three channels on one row, each as its stored value (colour times intensity)
	const float rowW = MenuWidgets::RowBegin("Red / Green / Blue", 0.0f);
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	static const char* const kChannelIds[] = { "##r", "##g", "##b" };
	static const char* const kChannelFormats[] = { "R %.4f", "G %.4f", "B %.4f" };
	for (int c = 0; c < 3; c++) {
		if (c) ImGui::SameLine();
		ImGui::SetNextItemWidth((rowW - gap * 2.0f) / 3.0f);
		float stored = wheel.hue[c] * wheel.intensity;
		if (DragWithStepKeys(kChannelIds[c], &stored, 0.001f, kChannelFormats[c]) && wheel.intensity > 0.0f) {
			wheel.hue[c] = stored / wheel.intensity;
			edited = true;
		}
	}

	ImGui::SetNextItemWidth(MenuWidgets::RowBegin("Intensity", 0.0f));
	edited |= DragWithStepKeys("##intensity", &wheel.intensity, 0.01f, "%.4f");

	if (edited) {
		for (int c = 0; c < 3; c++) TheSettingManager->SetSetting(rgb[c]->Section, rgb[c]->Key, wheel.hue[c] * wheel.intensity);
		TheSettingManager->LoadSettings();
	}
	if (titleHovered && !nodeR.Description.empty()) WrappedTooltip(ShownDescription(nodeR.Description).c_str());
	ImGui::PopID();
}

// ---- Settings drawn as a list of choices -------------------------------------------------------------------------------------
// Whole numbers that pick one of a few modes get a dropdown of names. Keyed by "Section.Key": the key alone repeats ("Mode" is a
// different thing for Cinema and DepthOfField).
//
// Cinema and DepthOfField Mode: the C++ also takes 3 and 4 (Cinema.cpp, DepthOfField.cpp), tests left from the Oblivion code that
// check for persuasion. New Vegas has no persuasion, so 3 always switches the effect off ("Disabled") and 4 is a copy of 1 or 2 or
// always off: only 3 is offered.

static const char* const kToneMappers[] = {
	"0 - None (vanilla)",
	"1 - VTLottes",
	"2 - Lottes (tuned)",
	"3 - Reinhard",
	"4 - Reinhard Jodie",
	"5 - ACES Filmic",
	"6 - ACES Fitted",
	"7 - Uncharted 2",
	"8 - Uchimura (GT)",
	"9 - AGX",
	"10 - DICE",
	"11 - Perceptual (Polyphony)",
};
static const char* const kWhenShown[] = { "0 - Always", "1 - Not during dialog", "2 - Only during dialog", "3 - Disabled" };
static const char* const kExposureModes[] = { "0 - Classic (screen average)", "1 - Eye adaptation (whole view, white balance)" };	// UNOFFICIAL [Shaders.Exposure.Main] Mode
static const char* const kShadowQualities[] = { "0 - Low", "1 - Medium", "2 - High", "3 - Full", "4 - Custom" };
static const char* const kShadowFormats[] = { "0 - 16 bit", "1 - 32 bit" };
static const char* const kCascadeSizes[] = { "0 - 1024", "1 - 1536", "2 - 2048", "3 - 2560", "4 - 3072", "5 - 3584", "6 - 4096" };
static const char* const kOrthoSizes[] = { "0 - 128", "1 - 256", "2 - 512", "3 - 1024", "4 - 2048" };
static const char* const kSmaaEdges[] = { "0 - Luma", "1 - Color", "2 - Depth", "3 - Luma + Depth" };
static const char* const kOffOnRestart[] = { "0 - Off", "1 - On (needs a restart)" };	// fixes-a #11
static const char* const kPtLevels[] = { "0 - Quality", "1 - Balanced", "2 - Performance" };	// UNOFFICIAL 2026-11-11 PTRealtimeLevel
static const char* const kDeferredSkinSSSModes[] = { "0 - Off", "1 - Classic blur", "2 - Burley SSS (sharp highlights)" };
static const char* const kMaterialSSSModes[] = { "0 - Off", "1 - In-pass scatter & transmission", "2 - In-pass + Burley screen scatter" };
static const char* const kRealSunPathModes[] = { "0 - Vanilla overhead arc", "1 - Physical tilted arc", "2 - Seasonal calendar tilt" };
static const char* const kSmokeTests[] = {
	"0 - Normal", "1 - Smoke in green", "2 - No muzzle flash light", "3 - Smoke not drawn", "4 - Nothing drawn at all",
};

struct Choices { const char* const* names; int count; };

static const Choices* ChoicesFor(const char* section, const char* key) {
	#define NAMES(list) Choices{ list, (int)(sizeof(list) / sizeof(*list)) }
	static const std::unordered_map<std::string, Choices> kBySetting = {
		{ "Shaders.Tonemapping.Main.TonemappingMode", NAMES(kToneMappers) },
		{ "Shaders.Tonemapping.Interiors.TonemappingMode", NAMES(kToneMappers) },
		{ "Shaders.Exposure.Main.Mode", NAMES(kExposureModes) },
		{ "Shaders.Cinema.Main.Mode", NAMES(kWhenShown) },
		{ "Shaders.DepthOfField.FirstPersonView.Mode", NAMES(kWhenShown) },
		{ "Shaders.DepthOfField.ThirdPersonView.Mode", NAMES(kWhenShown) },
		{ "Shaders.DepthOfField.VanityView.Mode", NAMES(kWhenShown) },
		{ "Shaders.ShadowsExteriors.Main.Quality", NAMES(kShadowQualities) },
		{ "Shaders.ShadowsExteriors.ShadowMaps.Format", NAMES(kShadowFormats) },
		{ "Shaders.ShadowsExteriors.ShadowMaps.CascadeResolution", NAMES(kCascadeSizes) },
		{ "Shaders.ShadowsExteriors.Ortho.Resolution", NAMES(kOrthoSizes) },
		{ "Shaders.SMAA.Main.EdgeDetection", NAMES(kSmaaEdges) },
		{ "Main.GunFX.Main.VolumetricSmokeTest", NAMES(kSmokeTests) },
		{ "Shaders.PBR.Main.SmithCorrelated", NAMES(kOffOnRestart) },
		{ "Main.Main.RayTracing.PTRealtimeLevel", NAMES(kPtLevels) },	// UNOFFICIAL 2026-11-11 (the Advanced view's row; the simple view: "Path tracing preset")
		{ "Main.Main.Quality.DeferredSkinSSS", NAMES(kDeferredSkinSSSModes) },
		{ "Main.Main.Quality.MaterialSSS", NAMES(kMaterialSSSModes) },
		{ "Shaders.ShadowsExteriors.Deferred.RealSunPath", NAMES(kRealSunPathModes) },
	};
	#undef NAMES
	const auto found = kBySetting.find(std::string(section) + "." + key);
	return found == kBySetting.end() ? nullptr : &found->second;
}

// A dropdown over names (the current one ticked and focused when it opens). Returns the index picked this frame, or -1.
static int NamePicker(const char* id, const char* shown, const char* const* names, int count, int current) {
	if (!ImGui::BeginCombo(id, shown)) return -1;
	int picked = -1;
	for (int i = 0; i < count; i++) {
		if (ImGui::Selectable(names[i], i == current)) picked = i;
		if (i == current) ImGui::SetItemDefaultFocus();
	}
	ImGui::EndCombo();
	return picked;
}

// UNOFFICIAL 2026-10-27 (user: "oh it's called displacement instead of tessellation" / "ok do that"): a setting's menu name where
// users look for another word; the key in the settings files stays. The ## part keeps ImGui's ID the key.
// ---- Conflict warnings ------------------------------------------------------------------------------------------------------
// 2026-11-05 (user: "add some sort of warning if you're going to toggle an effect that silently disables / conflicts with another
// one"). Turning on a setting or an effect listed here, while the other side of the conflict is on, first opens a "Heads up" box that
// says in plain words what stops working; the change waits for "Turn it on anyway" (Cancel leaves everything as it was). The list is
// the conflicts found in the code on 2026-11-05 (AmbientLight's takeover, ComputeGI over RT GI, path tracing, ...), not every partial
// overlap. An effect's own switch is "Shaders.<Name>.Status" / "Enabled" (or its Main page's Enabled for the effects switched there).
struct ConflictRule {
	const char* Section;
	const char* Key;
	const char* Value;		// the value that triggers it: "true", or a whole number
	bool (*Applies)();		// the other side is on now (warn only then); nullptr: always
	const char* Text;
};
static bool SettingOn(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key) != 0; }
static bool EffectOn(const char* name) { return TheSettingManager->GetMenuShaderEnabled(name); }
static bool AnyRtOn() {
	return SettingOn("Shaders.ShadowsExteriors.RT", "Enabled") || SettingOn("Shaders.ShadowsInteriors.RT", "Enabled") ||
		SettingOn("Shaders.AmbientOcclusion.RT", "Enabled") || SettingOn("Shaders.AmbientOcclusion.RT", "GI") ||
		(SettingOn("Shaders.AmbientOcclusion.Skylighting", "Enabled") && SettingOn("Shaders.AmbientOcclusion.RT", "Sky")) ||
		SettingOn("Shaders.Reflections.RT", "Enabled");
}
static bool CloudsCastShadows() { return EffectOn("VolumetricClouds") && TheSettingManager->GetSettingF("Shaders.VolumetricClouds.Main", "Shadows") > 0.0f; }

static const ConflictRule kConflictRules[] = {
	{ "Shaders.AmbientLight.Main", "ComputeGI", "true",
		[]() { return SettingOn("Shaders.AmbientOcclusion.RT", "GI"); },
		"Screen-space bounce light replaces your ray-traced bounce light: the ray-traced one does nothing while this is on." },
	{ "Shaders.AmbientOcclusion.RT", "GI", "true",
		[]() { return EffectOn("AmbientLight") && SettingOn("Shaders.AmbientLight.Main", "ComputeGI"); },
		"Screen-space bounce light (on the Ambient light page) is on and replaces this: ray-traced bounce light does nothing until you turn that off." },
	{ "Shaders.AmbientOcclusion.Status", "Enabled", "true", []() { return EffectOn("AmbientLight"); },
		"Ambient light already does ambient occlusion and switches this effect off while it runs: this does nothing unless Ambient light is off." },
	{ "Shaders.BounceLight.Status", "Enabled", "true", []() { return EffectOn("AmbientLight"); },
		"Ambient light already does bounce light and switches this effect off while it runs: this does nothing unless Ambient light is off." },
	{ "Shaders.AmbientLight.Status", "Enabled", "true", []() { return EffectOn("AmbientOcclusion") || EffectOn("BounceLight"); },
		"Ambient light does ambient occlusion and bounce light itself: the separate Ambient occlusion and Bounce light effects switch off while it runs." },
	{ "Main.Main.RayTracing", "PTRealtime", "true", []() { return !SettingOn("Main.Main.RayTracing", "PTRealtime"); },	// (2026-11-11: only when it turns ON: the preset choice writes it with every pick)
		"Path tracing lights the whole picture itself: Ambient light, ambient occlusion and specular highlights switch off, and the path "
		"tracer's own settings are overridden. It also turns on Grass (near you, up to 1000 units, with See-through meshes on) and "
		"Distant land in the traced scene. Very heavy: for top graphics cards." },
	{ "Main.Main.RayTracing", "PTReplace", "true", nullptr,
		"The path-traced picture replaces the game's: Ambient light, ambient occlusion and specular highlights switch off while it runs." },
	{ "Shaders.ShadowsExteriors.RT", "Mode", "1", nullptr,
		"Replace (the default): near you only ray-traced sun shadows are used; the shadow maps take over beyond their range." },
	{ "Shaders.TAA.DLSS", "RayReconstruction", "true", AnyRtOn,
		"Ray Reconstruction takes over from Vegas Shaders' own ray-tracing smoothing: those smoothing settings do nothing while it runs." },
	{ "Shaders.CloudShadows.Main", "Enabled", "true", CloudsCastShadows,
		"Volumetric clouds cast their own shadows and replace these flat cloud shadows: this does nothing while they do." },
	{ "Shaders.VolumetricClouds.Status", "Enabled", "true",
		[]() { return SettingOn("Shaders.CloudShadows.Main", "Enabled") && TheSettingManager->GetSettingF("Shaders.VolumetricClouds.Main", "Shadows") > 0.0f; },
		"Volumetric clouds cast their own shadows and replace the flat cloud shadows while they are on." },
	{ "Shaders.VolumetricFog.Main", "LegacyFog", "true", nullptr,
		"Legacy fog (for old presets) ignores Classic look, the height layer and the current fog's settings." },
	{ "Shaders.Terrain.Blending", "Enabled", "true", []() { return SettingOn("Shaders.Grass.GPUGrass", "Enabled"); },
		"Terrain blending turns off GPU grass: the GPU grass will not show while this is on." },
	{ "Shaders.Grass.GPUGrass", "Enabled", "true", []() { return SettingOn("Shaders.Terrain.Blending", "Enabled"); },
		"GPU grass will not show while Terrain blending is on (blending turns grass-first drawing off)." },
	// UNOFFICIAL 2026-11-16 grass-cull (docs/next/GRASS-CULL.md): the strands are made from the grass drawn before the ground
	{ "Shaders.Grass.GPUGrass", "Enabled", "true", []() { return !SettingOn("Shaders.Grass.Main", "DrawFirst"); },
		"Strand grass is made from the grass drawn before the ground: with Grass > Draw first off, no strands are drawn and the game's flat grass stays." },
	{ "Shaders.Grass.Main", "DrawFirst", "false", []() { return SettingOn("Shaders.Grass.GPUGrass", "Enabled"); },
		"Strand grass (GPU grass) is on and needs this: with it off, no strands are drawn and the game's flat grass is used instead." },
	{ "Main.Main.Misc", "PresetManagerEnabled", "true", nullptr,
		"Location presets will change your settings by themselves as you move between places." },
	{ "Main.Main.ReducedQuality", "FXAA", "true", []() { return EffectOn("SMAA"); },
		"SMAA is on too: both smooth the edges, which costs more and softens the picture. Turn one of them off." },
	{ "Shaders.SMAA.Status", "Enabled", "true", []() { return SettingOn("Main.Main.ReducedQuality", "FXAA"); },
		"FXAA is on too: both smooth the edges, which costs more and softens the picture. Turn one of them off." },
	{ "Shaders.HDR.Main", "Enabled", "true", nullptr,
		"HDR output currently causes freezes for some players. A fix is being researched." },
};

static bool SameValue(const char* a, const std::string& b) {
	auto norm = [](const std::string& v) { return v == "true" ? std::string("1") : v == "false" ? std::string("0") : v; };
	return norm(a) == norm(b);
}

static const ConflictRule* FindConflict(const char* section, const char* key, const std::string& value) {
	for (const ConflictRule& r : kConflictRules)
		if (!strcmp(r.Section, section) && !strcmp(r.Key, key) && SameValue(r.Value, value) && (!r.Applies || r.Applies())) return &r;
	return nullptr;
}

// A change waiting for the Heads up box: the settings to write, or an effect to switch.
struct PendingChange {
	bool Request = false;	// open the box next frame (in the main window's ID scope, where it is drawn)
	std::string Text;
	std::vector<std::array<std::string, 3>> Sets;	// section, key, value
	std::string Shader;
};
static PendingChange s_pending;

// Whether a change may go ahead now; false: the Heads up box was asked for and the change waits for it.
static bool ConfirmChanges(const std::vector<std::array<std::string, 3>>& sets) {
	for (const auto& s : sets)
		if (const ConflictRule* rule = FindConflict(s[0].c_str(), s[1].c_str(), s[2])) {
			s_pending = { true, rule->Text, sets, std::string() };
			return false;
		}
	return true;
}
static bool ConfirmChange(const char* section, const char* key, const std::string& value) { return ConfirmChanges({ { section, key, value } }); }

// An effect's own switch being turned on ("Shaders.<Name>.Status" Enabled, or its Main page's Enabled).
static bool ConfirmEffectOn(const std::string& shader) {
	for (const char* page : { ".Status", ".Main" })
		if (const ConflictRule* rule = FindConflict(("Shaders." + shader + page).c_str(), "Enabled", "true")) {
			s_pending = { true, rule->Text, {}, shader };
			return false;
		}
	return true;
}

static void DrawConflictPopup() {
	if (s_pending.Request) {
		ImGui::OpenPopup("Heads up###conflict");
		s_pending.Request = false;
	}
	if (!ImGui::BeginPopupModal("Heads up###conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	ImGui::TextUnformatted(s_pending.Text.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	if (ImGui::Button("Turn it on anyway")) {
		for (const auto& s : s_pending.Sets) TheSettingManager->SetSettingS(s[0].c_str(), s[1].c_str(), s[2].c_str());
		if (!s_pending.Shader.empty()) TheShaderManager->SwitchShaderStatus(s_pending.Shader.c_str());
		TheSettingManager->LoadSettings();
		s_pending = PendingChange();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		s_pending = PendingChange();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ---- Simple views ---------------------------------------------------------------------------------------------------------
// 2026-11-05 (docs/next/SETTINGS-GAME-MENU-PLAN.md; user: "simplify the display names of the keys so that they are in a form of language
// the average gamer will understand and merge some together by proxy on the user side unless someone toggles advanced mode"). A page
// listed here shows, while the top row's Advanced switch is off, only its rows below: settings under a plain name, and choices that
// set several settings at once (shown as Custom when the settings match none of them). Advanced on (or Developer options): the page
// as before, every setting under its own name. The settings themselves, their sections and the saved file do not change. A page not
// listed is drawn as before in both modes, until it gets its simple view.
struct SimpleRow {
	const char* Section;
	const char* Key;		// a setting shown under Label, or nullptr: a choice
	const char* Label;
	const char* Options;	// a choice's options: "Name: Key=Value Key=Value | Name: ...", or nullptr
	const char* Tip;		// the tooltip while Advanced is off (plain words; Advanced shows the setting's own description)
};
static const SimpleRow kSimpleRows[] = {
	// Sun shadows
	{ "Shaders.ShadowsExteriors.RT", "Enabled", "Ray-traced sun shadows", nullptr, "Sun shadows traced with rays: exact contact shadows and no missing shadows far away. Needs an RTX-class GPU and the DXVK fork." },
	{ "Shaders.ShadowsExteriors.RT", nullptr, "Quality",
		"Performance: HalfResolution=true Penumbra=false | Balanced: HalfResolution=false Penumbra=false | High (soft, realistic edges): HalfResolution=false Penumbra=true",
		"Performance: traced at half resolution (cheaper, softer). Balanced: full resolution. High: edges soften with distance from what casts "
		"them, as real sun shadows do (the most expensive)." },
	{ "Shaders.ShadowsExteriors.RT", nullptr, "Near you",
		"Add to the shadow maps: Mode=0 | Replace the shadow maps: Mode=1",
		"Add: the traced shadows fill in what the game's shadow maps miss. Replace: near you only the traced shadows are used (sharper, exact)." },
	{ "Shaders.ShadowsExteriors.RT", "Strength", "Shadow strength", nullptr, "How dark the traced sun shadows are." },
	// Lamp shadows
	{ "Shaders.ShadowsInteriors.RT", "Enabled", "Ray-traced lamp shadows", nullptr, "Lamp shadows traced with rays instead of the old shadow maps: sharper and no leaks. Takes effect after a restart." },
	{ "Shaders.ShadowsInteriors.RT", nullptr, "Shadow edges",
		"Sharp: LampRadius=0.0 Rays=1 | Soft: LampRadius=4.0 Rays=1 | Soft, less grainy: LampRadius=4.0 Rays=4",
		"Sharp: hard edges, the cheapest. Soft: lamps have a size, so edges blur with distance (a little grain that the anti-aliasing smooths). "
		"Soft, less grainy: four rays per lamp (more expensive)." },
	{ "Shaders.ShadowsInteriors.RT", "Exteriors", "Also outdoors at night", nullptr, "Street lamps outdoors at night cast traced shadows too. Takes effect after a restart." },
	// Ambient occlusion and bounce light
	{ "Shaders.AmbientOcclusion.RT", "Enabled", "Ray-traced ambient occlusion", nullptr, "Soft shadows in corners and under objects, worked out with rays instead of from the screen: no halos, nothing vanishes at the screen edge." },
	{ "Shaders.AmbientOcclusion.RT", "GI", "Ray-traced bounce light", nullptr, "Light bounces off lit surfaces onto nearby ones, tinted by their colour. Turn off Screen-space bounce light, or it replaces this." },
	{ "Shaders.AmbientOcclusion.RT", nullptr, "Bounce light quality",
		"Low: GIReuse=8 GISpatial=0 RadianceCache=false CacheExteriors=false | Medium: GIReuse=8 GISpatial=2 RadianceCache=false CacheExteriors=false | "
		"High: GIReuse=8 GISpatial=2 RadianceCache=true CacheExteriors=false | Ultra: GIReuse=8 GISpatial=2 RadianceCache=true CacheExteriors=true",
		"Low: one bounce of light. Medium: less grain. High: light also bounces more than once indoors. Ultra: more than once outdoors too "
		"(expensive)." },
	{ "Shaders.AmbientOcclusion.RT", "GIStrength", "Bounce light strength", nullptr, "How much bounced light is added. Higher looks more vivid, less realistic." },
	{ "Shaders.AmbientOcclusion.RT", "GIStrengthNight", "Bounce light strength at night", nullptr, "Outdoors at night, this replaces Bounce light strength. It blends in over dusk and dawn. -1 uses the daytime value." },
	{ "Shaders.AmbientOcclusion.RT", "GIStrengthInterior", "Bounce light strength indoors", nullptr, "Indoors, this replaces the setting of the same name without Interior. -1 uses that setting indoors too." },
	{ "Shaders.AmbientOcclusion.RT", "GIClampInterior", "Brightest single bounce indoors", nullptr, "Indoors, this replaces the setting of the same name without Interior. -1 uses that setting indoors too." },
	{ "Shaders.AmbientOcclusion.RT", "GIDistanceInterior", "Bounce light reach indoors", nullptr, "Indoors, this replaces the setting of the same name without Interior. -1 uses that setting indoors too." },
	{ "Shaders.AmbientOcclusion.RT", "GlowGIInterior", "Light from glowing surfaces indoors", nullptr, "Indoors, this replaces the setting of the same name without Interior. -1 uses that setting indoors too." },
	{ "Shaders.AmbientOcclusion.RT", "GlowDirectInterior", "Direct light from glowing objects indoors", nullptr, "Indoors, this replaces the setting of the same name without Interior. -1 uses that setting indoors too." },
	// Reflections
	{ "Shaders.Reflections.RT", "Enabled", "Ray-traced reflections", nullptr, "Rays fill in what normal reflections miss: things off screen or hidden behind something. Needs Reflections on." },
	{ "Shaders.Reflections.RT", nullptr, "Surfaces that get rays",
		"Shiny only: MaxRoughness=0.35 | Shiny and satin: MaxRoughness=0.5 | Most surfaces: MaxRoughness=0.75",
		"How rough a surface can be and still get ray-traced reflections; rougher ones keep the screen-space reflections. Wet ground counts "
		"by Reflections' WetRoughness: if it is rougher than this, puddles get no rays." },
	{ "Shaders.Reflections.RT", "Weapon", "Weapon reflections", nullptr, "Your first-person weapon reflects its surroundings (the live 360-degree cube on all DXVK GPUs, or ray-traced hits when RT reflections are on). Strongest on metal parts." },
	{ "Shaders.Reflections.RT", "WeaponHands", "Hands and arms reflect too", nullptr, "Off: only the gun reflects. On: your hands, gloves and arms reflect like the gun (the whole first-person model)." },
	{ "Shaders.Reflections.RT", "Water", "Ray-traced water reflections", nullptr, "Water reflects the world with rays instead of the old reflection map." },
	// Light shafts and fog (UNOFFICIAL 2026-11-07 ptaaa-fog)
	// (2026-11-13: said "Sunbeams through windows": it is outdoors only, and it adds no shafts of its own. A tester: "no worky".)
	{ "Shaders.VolumetricLight.RT", "Enabled", "Ray-traced light shafts and fog", nullptr, "Outdoors only. The sunbeams past trees, fences and buildings, and the "
		"fog's sunlight, take their shadows from the traced scene instead of the shadow maps: a small change (shaft edges, people, far buildings). "
		"It adds no shafts of its own: how visible they are is Volumetric light's Strength and Density. Works only while another ray-traced effect "
		"is on. The Status block above says what it does right now." },
	{ "Shaders.VolumetricLight.RT", nullptr, "Shaft quality",
		"Fast: RaysPerCell=1 | Balanced: RaysPerCell=2 | High: RaysPerCell=4",
		"Rays per piece of air each frame: more gives cleaner shaft edges sooner after the view moves, at more cost." },
	// UNOFFICIAL 2026-11-13 (rtvol2) the expanded look
	{ "Shaders.VolumetricLight.RT", "Expanded", "Modern light shafts", nullptr, "Makes the traced shafts look like a current game's: the lit air "
		"reaches much farther and all of it is traced, so hills, buildings, trees and people cast long shafts of shade into the dusty air; the "
		"dust hugs the ground, the glow gathers round the sun, and cloud shadows drift through the air. Off: the traced shadow only replaces the "
		"shadow maps' (a small change)." },
	{ "Shaders.VolumetricLight.RT", "ExpandedDistance", "How far the shafts reach", nullptr, "How far from you the lit air goes, in game units "
		"(8000 is about 115 metres). Farther shows the long shadows of hills and far buildings; it costs a little more." },
	{ "Shaders.VolumetricLight.RT", "ExpandedDensity", "Dust in the air", nullptr, "How thick the air is near the ground. Higher gives stronger "
		"shafts and more haze; too high washes the picture out." },
	{ "Shaders.VolumetricLight.RT", "ExpandedHeight", "Dust layer height", nullptr, "How quickly the dust thins above your head. Lower keeps the "
		"haze close to the ground and the sky clear; 0 makes the air the same everywhere." },
	{ "Shaders.VolumetricLight.RT", "ExpandedSunGlow", "Glow round the sun", nullptr, "How much of the light gathers in a tight glow round the "
		"sun, where the shafts meet. 0: the glow of Volumetric light's Anisotropy alone." },
	{ "Shaders.VolumetricLight.RT", "ExpandedGlowSharpness", "Glow size", nullptr, "Higher makes the glow round the sun smaller and brighter." },
	{ "Shaders.VolumetricLight.RT", "ExpandedClouds", "Cloud shadows in the air", nullptr, "The volumetric clouds' shadows also darken the lit "
		"air, so rays of light fall between the clouds. Needs Volumetric clouds with their shadows on before ray tracing starts." },
	// Scene and path tracing
	{ "Main.Main.RayTracing", "Actors", "People in the traced scene", nullptr, "People and creatures cast traced shadows and show up in traced reflections. Costs some frame rate in crowds." },
	{ "Main.Main.RayTracing", "AlphaTested", "See-through meshes (fences, leaves, grates)", nullptr, "Fences, leaves and grates cast traced shadows with their holes, not as solid blocks." },
	{ "Main.Main.RayTracing", "Grass", "Grass", nullptr, "Grass casts traced shadows and shows in reflections. Needs See-through meshes on. Path tracing turns it on by itself." },
	{ "Main.Main.RayTracing", "Decals", "Decals and blood", nullptr, "Posters, cracks and blood splats show in traced reflections and bounce light. They cast no shadows. Needs See-through meshes on." },
	{ "Main.Main.RayTracing", "HitTextures", "Real textures in reflections and bounce light", nullptr, "Reflections and bounce light use the real colours of what they hit instead of flat ones. Uses more video memory." },
	{ "Main.Main.RayTracing", "FarObjectLOD", "Distant buildings and objects", nullptr, "Distant buildings and objects are in the traced scene, so far shadows and reflections are complete." },
	{ "Main.Main.RayTracing", "FarLandLOD", "Distant land", nullptr, "Distant hills are in the traced scene, so far shadows and reflections include them." },
	{ "Main.Main.RayTracing", "PTRealtime", "Path tracing (experimental, very heavy)", nullptr, "The whole picture lit by path tracing, like a film render. Experimental and very heavy: for top GPUs." },
	// UNOFFICIAL 2026-11-11 the three path tracing set-ups measured on an RTX-class card (NVIDIA L4, 1280 x 720, three scenes: Quality 20.5 /
	// 26.2 / 23.1 fps, Performance 57.7 / 39.5 / 76.7). Picked by the player only; a choice writes these keys and nothing else (the
	// anti-aliasing / upscaler stays the player's: the tip only recommends one). PTRealtimeLevel's steps: core/RtPathTrace.cpp.
	{ "Main.Main.RayTracing", nullptr, "Path tracing preset",
		"Off: PTRealtime=false | Quality: PTRealtime=true PTRealtimeLevel=0 PTStandDownRT=true | "
		"Balanced: PTRealtime=true PTRealtimeLevel=1 PTStandDownRT=true | Performance: PTRealtime=true PTRealtimeLevel=2 PTStandDownRT=true",
		"Three ready-made path tracing set-ups. Frame rates are from an RTX-class test card at 1280x720 in three test scenes; yours will differ.\n\n"
		"Quality: light bounces three times, glowing signs and windows light what is around them, the best lamp sampling. About 20-26 fps.\n\n"
		"Balanced: two bounces, most light after the first bounce comes from a coarser stored estimate, and glowing surfaces are no longer "
		"sampled as lights (their glow still arrives through the bounces). Faster; light that has bounced "
		"twice or more is a little blurrier.\n\n"
		"Performance: one bounce (the rest is estimated; more grain next to neon, corners a "
		"little darker) and lamps are sampled the simple way (more grain in rooms with many lamps). About 40-77 fps; no loss was seen in the "
		"day, night, fire and neon test pictures.\n\n"
		"All three also pause ray-traced lamp shadows and reflections while path tracing runs, because it draws its own (a few fps more). "
		"Your first-person weapon and see-through things may lose those traced shadows and reflections meanwhile.\n\n"
		"For much more speed, use FSR or DLSS at Quality (67%) on the anti-aliasing page: path tracing follows the render scale, and that "
		"roughly doubles the frame rate. A preset does not change your anti-aliasing or any other setting. "
		"Custom means your settings match none of the three (for example path tracing switched on by its own switch above)." },
	// GunFX (2026-11-16, gunfx-builtin: built into Vegas Shaders; its settings moved from GunFX.ini into [Main.GunFX.*]). The Gun effects
	// page draws one drop-down per effect (RenderGunPage): its switch and these few keys; the rest with Advanced on.
	{ "Main.GunFX.Main", "Puff", "Smoke puff at the muzzle", nullptr, "A small puff of smoke from the barrel on every shot." },
	{ "Main.GunFX.Main", "HeatSmoke", "Smoke from a hot barrel", nullptr, "A thin strand of smoke rises from the barrel after sustained fire." },
	{ "Main.GunFX.Main", "AfterFireTrail", "Smoke trail after firing", nullptr, "A wispy trail rises from the barrel once you stop shooting." },
	{ "Main.GunFX.Main", "EjectionSmoke", "Smoke from the ejection port", nullptr, "A burst of smoke with every spent casing, bigger when the gun is hot." },
	{ "Main.GunFX.Main", "BarrelGlow", "Glowing hot barrel", nullptr, "The barrel glows after sustained fire and cools down slowly." },
	{ "Main.GunFX.Main", "HeatHaze", "Heat shimmer over a hot barrel", nullptr, "The air shimmers around a hot barrel." },
	{ "Main.GunFX.Main", "MuzzleBlast", "Heat ripple on every shot", nullptr, "A quick ripple of hot air at the muzzle each time you fire." },
	{ "Main.GunFX.Main", "VolumetricSmoke", "Real 3D smoke (experimental)", nullptr, "Gun smoke drawn as real 3D smoke that lamps and the sun light. Costs frame rate." },
	{ "Main.GunFX.Main", "VolumetricSmokeHalfRes", "Draw the 3D smoke at half resolution", nullptr, "Much cheaper, and the smoke is soft anyway. Off = full resolution." },
	{ "Main.GunFX.Main", "VolumetricSmokeLowRes", "Big close smoke at low resolution", nullptr, "When you walk into thick smoke, it is drawn at an eighth of the resolution. Much cheaper; looks the same." },
	{ "Main.GunFX.Main", "BulletTunnels", "Bullets cut through the smoke", nullptr, "Bullets punch tunnels through the 3D smoke and the haze, which then fill back in." },
	{ "Main.GunFX.Main", "SmokeHaze", "Smoke haze fills the room", nullptr, "Firing for a while fills the room with haze. Needs Real 3D smoke; the most expensive part." },
	{ "Main.GunFX.Main", "EnergyWeapons", "Also on energy weapons", nullptr, "Lasers, plasma and other energy weapons get the effects too." },
	{ "Main.GunFX.Puff", "Scale", "Puff size", nullptr, "How big each puff is." },
	{ "Main.GunFX.Puff", "Opacity", "Puff thickness", nullptr, "How thick the smoke is (0 = invisible). Small numbers: the pieces of smoke overlap and add up." },
	{ "Main.GunFX.Puff", "SmokeLife", "How long a puff lasts", nullptr, "Seconds each bit of smoke stays before it fades." },
	{ "Main.GunFX.Puff", "MinInterval", "Gap between puffs (automatic guns)", nullptr, "Seconds between two puffs on automatic guns. Higher = a lighter haze of puffs." },
	{ "Main.GunFX.Puff", "PlayerOnly", "Only your own shots", nullptr, "Off: everyone's guns puff smoke." },
	{ "Main.GunFX.Wisp", "Scale", "Strand size", nullptr, "How big the heat smoke strand is." },
	{ "Main.GunFX.Wisp", "Opacity", "Strand thickness", nullptr, "How thick the heat smoke is (0 = invisible)." },
	{ "Main.GunFX.Wisp", "SmokeLife", "How long the smoke lasts", nullptr, "Seconds each bit of the strand stays before it fades." },
	{ "Main.GunFX.Wisp", "HeatPerShot", "Heat per shot", nullptr, "How much each shot heats the barrel. Also how fast the glow and the shimmer build up." },
	{ "Main.GunFX.Wisp", "CoolPerSecond", "Cooling speed", nullptr, "How fast the barrel cools once you stop firing." },
	{ "Main.GunFX.Trail", "Scale", "Trail size", nullptr, "How big the after-fire trail is." },
	{ "Main.GunFX.Trail", "Opacity", "Trail thickness", nullptr, "How thick the trail is (0 = invisible)." },
	{ "Main.GunFX.Trail", "SmokeLife", "How long the trail lingers", nullptr, "Seconds each bit of the trail stays before it fades." },
	{ "Main.GunFX.Trail", "StartDelay", "Starts after you stop firing", nullptr, "Seconds after your last shot before the trail starts." },
	{ "Main.GunFX.Ejection", "Scale", "Burst size", nullptr, "How big the burst from the ejection port is." },
	{ "Main.GunFX.Ejection", "HotScale", "Bigger when hot", nullptr, "How much bigger the burst gets on a hot gun (1 = the same)." },
	{ "Main.GunFX.Ejection", "Opacity", "Burst thickness", nullptr, "How thick the smoke is (0 = invisible)." },
	{ "Main.GunFX.Ejection", "SmokeLife", "How long it lasts", nullptr, "Seconds each bit of smoke stays before it fades." },
	{ "Main.GunFX.Glow", "Radius", "Glow thickness", nullptr, "How far round the barrel the glow reaches." },
	{ "Main.GunFX.Glow", "Length", "Glow length", nullptr, "How far back along the barrel it glows when fully hot." },
	{ "Main.GunFX.Glow", "StartHeat", "Heat before it glows", nullptr, "How hot the barrel must be before it starts to glow (a few shots' worth)." },
	{ "Main.GunFX.Glow", "RiseSeconds", "Time to full glow", nullptr, "Seconds the glow takes to come up." },
	{ "Main.GunFX.Haze", "StrengthPixels", "Shimmer strength", nullptr, "How much the air above the barrel ripples." },
	{ "Main.GunFX.Haze", "HeightPixels", "Shimmer height", nullptr, "How tall the shimmer is on screen." },
	{ "Main.GunFX.Haze", "StartHeat", "Heat before it shimmers", nullptr, "How hot the barrel must be before the air shimmers." },
	{ "Main.GunFX.Blast", "StrengthPixels", "Ripple strength", nullptr, "How strong the ripple at the muzzle is on each shot." },
	{ "Main.GunFX.Blast", "RadiusPixels", "Ripple size", nullptr, "How wide the ripple is on screen." },
	{ "Main.GunFX.Blast", "Seconds", "Ripple length", nullptr, "How long each ripple lasts." },
	{ "Main.GunFX.Volume", "DensityScale", "3D smoke thickness", nullptr, "How thick the 3D smoke is." },
	{ "Main.GunFX.Volume", "SizeScale", "3D smoke size", nullptr, "How big the 3D smoke is." },
	{ "Main.GunFX.Volume", "Brightness", "3D smoke brightness", nullptr, "How bright the light makes the 3D smoke." },
	{ "Main.GunFX.Volume", "Wind", "Drift with the wind", nullptr, "How far the weather's wind carries the smoke outdoors." },
	{ "Main.GunFX.Volume", "Haze", "Haze per shot", nullptr, "How much haze each shot adds to the room." },
	{ "Main.GunFX.Volume", "HazeLife", "How long the haze lingers", nullptr, "Seconds the haze takes to clear." },
	{ "Main.GunFX.Volume", "HazeSize", "Haze size", nullptr, "How big each cloud of haze is." },
	// AmbientLight (Main)
	{ "Shaders.AmbientLight.Main", "OcclusionStrength", "Corner shading strength", nullptr, "How dark creases, corners and the ground under things get." },
	{ "Shaders.AmbientLight.Main", "Radius", "Corner shading reach", nullptr, "How far the corner shading and bounce light reach." },
	{ "Shaders.AmbientLight.Main", "GridBounce", "Sun bounce light strength", nullptr, "Soft bounce light from the sun in daylight, spread by a 3D light grid round you. Experimental." },
	{ "Shaders.AmbientLight.Main", "GridLamps", "Lamp bounce light (indoors, night)", nullptr, "Lamps and fires light the room softly, with light pooling and spreading round them. Indoors and at night." },
	{ "Shaders.AmbientLight.Main", "ComputeGI", "Screen-space bounce light", nullptr, "Light bounces off what is on screen onto nearby surfaces. Replaces ray-traced bounce light when on. Needs the DXVK fork." },
	{ "Shaders.AmbientLight.Main", "ComputeGIStrength", "Screen-space bounce light strength", nullptr, "How strong the screen-space bounce light is. Needs the DXVK fork." },
	{ "Shaders.AmbientLight.Main", nullptr, "Screen-space bounce light quality",
		"Low: ComputeGIQuality=0 | Medium: ComputeGIQuality=1 | High: ComputeGIQuality=2",
		"More detail in the shading and bounce light: smoother and steadier, but costs more." },
	// AmbientOcclusion (Exteriors)
	{ "Shaders.AmbientOcclusion.Exteriors", "Enabled", "Corner shading outdoors", nullptr, "Darkens corners and creases outdoors for more depth." },
	{ "Shaders.AmbientOcclusion.Exteriors", "StrengthMultiplier", "Corner shading strength", nullptr, "How dark corners and creases get." },
	{ "Shaders.AmbientOcclusion.Exteriors", "Range", "Shading reach (larger is softer)", nullptr, "Larger gives wider, softer shading; smaller gives sharper detail in creases." },
	{ "Shaders.AmbientOcclusion.Exteriors", "ClampStrength", "Darkest shading allowed", nullptr, "Limits how dark the shading may get. Lower values allow near black." },
	// AmbientOcclusion (Interiors)
	{ "Shaders.AmbientOcclusion.Interiors", "Enabled", "Corner shading indoors", nullptr, "Darkens corners and creases indoors for more depth." },
	{ "Shaders.AmbientOcclusion.Interiors", "StrengthMultiplier", "Corner shading strength", nullptr, "How dark corners and creases get." },
	{ "Shaders.AmbientOcclusion.Interiors", "Range", "Shading reach (larger is softer)", nullptr, "Larger gives wider, softer shading; smaller gives sharper detail in creases." },
	{ "Shaders.AmbientOcclusion.Interiors", "ClampStrength", "Darkest shading allowed", nullptr, "Limits how dark the shading may get. Lower values allow near black." },
	// AmbientOcclusion (GTAO)
	{ "Shaders.AmbientOcclusion.GTAO", "Enabled", "Better corner shading (GTAO)", nullptr, "A more accurate version of the corner shading: no grey flat ground and no halos round objects." },
	{ "Shaders.AmbientOcclusion.GTAO", "MaxRadius", "Longest reach (pixels)", nullptr, "The widest area on screen the shading looks at. Larger reaches further but costs more." },
	// AmbientOcclusion (Skylighting)
	{ "Shaders.AmbientOcclusion.Skylighting", "Enabled", "Darker under roofs and overhangs", nullptr, "Outdoors, removes sky light under roofs, porches, bridges and cliffs so they look properly shaded." },
	{ "Shaders.AmbientOcclusion.Skylighting", "Strength", "Strength", nullptr, "How much light is taken away under cover." },
	{ "Shaders.AmbientOcclusion.Skylighting", "Distance", "How far to look for cover", nullptr, "How far the light travels. Larger reaches further." },
	{ "Shaders.AmbientOcclusion.Skylighting", "Reflections", "Also darken sky reflections under cover", nullptr, "Wet floors and puddles under cover stop mirroring a bright sky." },
	// BounceLight (Main)
	{ "Shaders.BounceLight.Main", "Strength", "Bounce light strength", nullptr, "How much light bounces from lit surfaces onto nearby ones." },
	{ "Shaders.BounceLight.Main", "Radius", "Bounce light reach", nullptr, "How far the bounce light reaches." },
	{ "Shaders.BounceLight.Main", "BlurRadius", "Softness", nullptr, "Smooths out grain. Higher is softer." },
	{ "Shaders.BounceLight.Main", nullptr, "Quality",
		"Faster: QuarterResolution=true | Sharper: QuarterResolution=false",
		"Faster works the bounce out on a quarter of the pixels (no visible difference for most people). Sharper uses half of them and costs a little more." },
	{ "Shaders.BounceLight.Main", "Temporal", "Steadier light (less grain)", nullptr, "Keeps light from the last frame to reduce grain and flicker. Can trail slightly when turning." },
	// Specular (Exterior)
	{ "Shaders.Specular.Exterior", "SunStrength", "Sun highlight strength", nullptr, "How bright the sun glint is on surfaces. Strongest when looking towards the sun." },
	{ "Shaders.Specular.Exterior", "Roughness", "Highlight softness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.Specular.Exterior", "SkyReflections", "Sky reflection strength", nullptr, "How much of the sky shows in surfaces, mostly at glancing angles." },
	{ "Shaders.Specular.Exterior", "DistanceFade", "Highlight distance", nullptr, "How far away highlights stay visible before fading out." },
	// Specular (Rain)
	{ "Shaders.Specular.Rain", "SunStrength", "Sun highlight strength", nullptr, "The same, while it rains." },
	{ "Shaders.Specular.Rain", "Roughness", "Highlight softness", nullptr, "Wet surfaces are smoother: smaller, brighter highlights and clearer reflections." },
	{ "Shaders.Specular.Rain", "SkyReflections", "Sky reflection strength", nullptr, "The same, while it rains." },
	{ "Shaders.Specular.Rain", "DistanceFade", "Highlight distance", nullptr, "The same, while it rains." },
	// PBR (Main)
	{ "Shaders.PBR.Main", "ReflectivityExterior", "Shininess outdoors", nullptr, "How shiny and reflective surfaces look outdoors." },
	{ "Shaders.PBR.Main", "ReflectivityInterior", "Shininess indoors", nullptr, "How shiny surfaces look indoors. Lower it if floors look too glossy." },
	{ "Shaders.PBR.Main", "HairHighlight", "Hair shine", nullptr, "How bright the shine on hair is. 0 removes it." },
	{ "Shaders.PBR.Main", "Roughness", "Surface roughness (non-RMAOS, day)", nullptr, "Scales roughness on non-_rmaos objects outdoors by day. _rmaos maps use RoughnessScale instead." },
	{ "Shaders.PBR.Main", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Main", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Main", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.Main", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	{ "Shaders.PBR.Main", "SkylightingSaturation", "Sky colour strength in shade", nullptr, "How vivid the sky colour in shaded areas is. 0 is grey, 1 matches the sky." },
	{ "Shaders.PBR.Main", "DecoupleGlowMaps", "Decouple glow maps from lighting", nullptr, "Prevents sun and ambient scales from washing out emissive glow maps like weapon sights." },
	{ "Shaders.PBR.Main", "GlowScale", "Glow map brightness", nullptr, "Adjusts brightness of self-illuminated glow maps when decoupled from lighting." },
	// PBR (Rain)
	{ "Shaders.PBR.Rain", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.Rain", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Rain", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Rain", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.Rain", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	{ "Shaders.PBR.Rain", "SkylightingSaturation", "Sky colour strength in shade", nullptr, "How vivid the sky colour in shaded areas is. 0 is grey, 1 matches the sky." },
	// PBR (Night)
	{ "Shaders.PBR.Night", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.Night", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Night", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Night", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.Night", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	{ "Shaders.PBR.Night", "SkylightingSaturation", "Sky colour strength in shade", nullptr, "How vivid the sky colour in shaded areas is. 0 is grey, 1 matches the sky." },
	// PBR (NightRain)
	{ "Shaders.PBR.NightRain", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.NightRain", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.NightRain", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.NightRain", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.NightRain", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	{ "Shaders.PBR.NightRain", "SkylightingSaturation", "Sky colour strength in shade", nullptr, "How vivid the sky colour in shaded areas is. 0 is grey, 1 matches the sky." },
	// PBR (Interiors)
	{ "Shaders.PBR.Interiors", "Roughness", "Surface roughness (non-RMAOS, indoors)", nullptr, "Scales roughness on non-_rmaos objects indoors. _rmaos maps use RoughnessScale instead." },
	{ "Shaders.PBR.Interiors", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Interiors", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Interiors", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	// PBR (Metal)
	{ "Shaders.PBR.Metal", "MetalOn", "Metal look", nullptr, "Enables metallic PBR shading (both _rmaos maps and non-_rmaos fallback heuristics)." },
	{ "Shaders.PBR.Metal", "FirstPerson", "Metal on your weapon (fallback / gate)", nullptr, "Heuristic metal strength on non-_rmaos weapons. On _rmaos weapons with MetalFromMap on, acts as an on/off gate (>0)." },
	{ "Shaders.PBR.Metal", "World", "Metal look on the world (fallback / gate)", nullptr, "Heuristic metal strength on non-_rmaos world objects. On _rmaos world objects with MetalFromMap on, acts as an on/off gate (>0)." },
	{ "Shaders.PBR.Metal", "Roughness", "Fallback metal finish (non-RMAOS)", nullptr, "Finish of bare metal on objects without an _rmaos map (low = mirror, high = rough cast). Bypassed by _rmaos maps." },
	{ "Shaders.PBR.Metal", "Reflections", "Metal reflection strength", nullptr, "How strongly metal reflects the sky and its surroundings." },
	{ "Shaders.PBR.Metal", "CubeReflections", "Metal reflects its surroundings", nullptr, "Reflects what is really around it, such as the street, neon and lamps. Needs Reflections on." },
	// PBR (EnvMaps)
	{ "Shaders.PBR.EnvMaps", "Lighting", "Chrome reflections follow the light", nullptr, "Makes chrome shine follow the light around it, so it is dull in a dark cave and bright at noon." },
	{ "Shaders.PBR.EnvMaps", "Strength", "Chrome reflection brightness", nullptr, "How bright chrome and glass reflections are in the light." },
	{ "Shaders.PBR.EnvMaps", "CubeReflections", "Chrome shows its surroundings", nullptr, "Reflects what is really around it, such as the street, neon and lamps. Needs Reflections on." },
	// PBR (MaterialRules)
	{ "Shaders.PBR.MaterialRules", "Enabled", "Material types by texture", nullptr, "Sorts surfaces by texture (guns, glass, wood, rock) so only the right ones look metal or polished." },
	// PBR (Rmaos)
	{ "Shaders.PBR.Rmaos", "Enabled", "Material maps", nullptr, "Uses material maps for roughness, metal and shading where a texture has one." },
	{ "Shaders.PBR.Rmaos", "Occlusion", "Material map shading strength", nullptr, "How much the material maps darken the ambient light in crevices." },
	{ "Shaders.PBR.Rmaos", "MetalFromMap", "Metal from material maps", nullptr, "Takes metal from the material maps instead of guessing from the texture." },
	{ "Shaders.PBR.Rmaos", "SpecularFromMap", "Shine from material maps", nullptr, "Takes how much surfaces reflect from the material maps." },
	// PBR (Dust)
	{ "Shaders.PBR.Dust", "Enabled", "Dust on top surfaces", nullptr, "A thin sandy coat on upward-facing surfaces such as rock tops, car roofs and ledges." },
	{ "Shaders.PBR.Dust", "Amount", "Dust amount", nullptr, "How thick the dust coat is." },
	{ "Shaders.PBR.Dust", "ColorR", "Dust colour", nullptr, "The colour of the dust. Sand by default." },
	{ "Shaders.PBR.Dust", "Patchiness", "Patchiness", nullptr, "0 gives an even coat; higher gives patches with bare spots." },
	{ "Shaders.PBR.Dust", "Size", "Patch size", nullptr, "How big each patch of dust is." },
	{ "Shaders.PBR.Dust", "Interiors", "Also indoors", nullptr, "Also adds dust indoors, in ruins and dusty rooms." },
	// PBR (MicroShadow)
	{ "Shaders.PBR.MicroShadow", "Enabled", "Deeper cracks and crevices", nullptr, "Bumps in surfaces shadow themselves, so cracks look deep instead of painted on. Needs material maps." },
	{ "Shaders.PBR.MicroShadow", "Strength", "Strength", nullptr, "How deep cracks and crevices look." },
	// PBR (Linear)
	{ "Shaders.PBR.Linear", "Enabled", "Realistic light blending (linear lighting)", nullptr, "Blends light the way real light adds up: smoother falloff into shade and better lamp mixing." },
	{ "Shaders.PBR.Linear", "Gamma", "Light blending curve (gamma)", nullptr, "How light is blended. Leave at the standard value unless things look too dark or flat." },
	// PBR (LampFalloff)
	{ "Shaders.PBR.LampFalloff", "Exterior", "Realistic lamp falloff outdoors", nullptr, "Outdoor lamps and fires fade smoothly with distance instead of lighting an even disc." },
	{ "Shaders.PBR.LampFalloff", "Interior", "Realistic lamp falloff indoors", nullptr, "Indoor lamps make pools of light with darker areas between." },
	{ "Shaders.PBR.LampFalloff", "Falloff", "Lamp falloff speed", nullptr, "How fast a lamp dims with distance. Small gives a bright spot and a long dim tail." },
	{ "Shaders.PBR.LampFalloff", "Brightness", "Lamp brightness", nullptr, "How bright lamps are while this is on." },
	// Reflections (Main)
	// 2026-11-05: one choice that sets every setting the floor reflections' visibility depends on, so each look shows what it says
	// (separately, Specular occlusion hid sharp reflections on dark floors and physically faint floors showed only lamps).
	{ "Shaders.Reflections.Main", nullptr, "Look",
		"Realistic: Mirror=0.0 Roughness=0.4 InteriorFloors=0.0 GroundSheen=0.0 SpecularOcclusion=1.0 | "
		"Glossy floors: Mirror=0.3 Roughness=0.12 InteriorFloors=1.0 GroundSheen=1.0 SpecularOcclusion=0.0 | "
		"Mirror (showcase): Mirror=1.0 Roughness=0.02 InteriorFloors=2.0 GroundSheen=2.0 SpecularOcclusion=0.0",
		"Realistic: only shiny materials (tiles, glass, metal, wet ground) reflect, as in life: mostly lamps and bright things show. "
		"Glossy floors: every floor gets a clear sheen with people and objects in it. Mirror: every floor is a perfect mirror, to show "
		"off reflections (and ray-traced ones). Turning Specular occlusion back on (Realistic) takes a restart." },
	{ "Shaders.Reflections.Main", "GroundSheen", "Dry ground reflections", nullptr, "How much dry ground, such as roads and pavements, mirrors its surroundings. Streets pick up neon and lamps at night." },
	{ "Shaders.Reflections.Main", "RainWetness", "Wet ground reflections", nullptr, "How much rain makes the ground mirror neon, lamps and sky. Covered areas stay dry." },
	{ "Shaders.Reflections.Main", "InteriorFloors", "Floor reflections indoors", nullptr, "How much floors indoors mirror the room, wood and carpet too." },
	{ "Shaders.Reflections.Main", "Roughness", "Dry reflection blur", nullptr, "Lower gives mirror-like dry reflections; higher blurs them to a glow." },
	{ "Shaders.Reflections.Main", "Mirror", "Mirror floors", nullptr, "0 is real polished stone: you mostly see bright lamps in it unless you look along the floor. 1 turns reflecting floors into true mirrors that show people and objects at any angle. Use with Dry reflection blur near 0.02." },
	{ "Shaders.Reflections.Main", "WetRoughness", "Wet reflection blur", nullptr, "Lower gives sharp mirror-like reflections in puddles; higher blurs them." },
	{ "Shaders.Reflections.Main", "MaterialReflect", "Smooth surface reflections", nullptr, "How much smooth materials such as tiles, glass and polished counters reflect. Needs material maps on." },
	{ "Shaders.Reflections.Main", "DrawDistance", "Reflection distance", nullptr, "How far away surfaces still reflect." },
	{ "Shaders.Reflections.Main", nullptr, "Quality",
		"Low: Steps=12 | Medium: Steps=24 | High: Steps=40",
		"Higher gives steadier reflections with fewer gaps, and costs more." },
	{ "Shaders.Reflections.Main", "CubeMode", "Environment reflections mode", nullptr, "How the live surroundings cube fills: 0 captures screen view only while Reflections is on, 1 keeps it updated for PBR metal/chrome too and fills unseen directions with sky and light grid, 2 (default) also renders a staggered 3D world cubemap so weapons, metal, and glass reflect overhead and behind you." },
	// UNOFFICIAL 2026-11-27 cube-visible
	{ "Shaders.Reflections.Main", "CubeStrength", "Environment reflections strength", nullptr, "How clearly the live surroundings cube shows in reflections: on your weapon, metal, chrome and glass, wet ground and puddles. 0 none, 1 as blurred as each surface's roughness, up to 2 sharper on smooth metal and mirrors. Never brighter than the surface's own reflectance, and dark where the surroundings are dark." },
	{ "Shaders.Reflections.Main", "CubeResolution", "Environment cube resolution", nullptr, "Resolution per face for the live surroundings cubemap (64, 128, or 256). Higher gives sharper reflections on chrome and glossy surfaces. Needs a restart." },
	{ "Shaders.Reflections.Main", "CubeDistance", "Environment cube distance", nullptr, "Maximum capture distance for 3D objects and point lamps in world-cubemap mode (mode 2)." },
	// UNOFFICIAL 2026-11-27 cheaper world cube
	{ "Shaders.Reflections.Main", "CubeFaceInterval", "Environment cube update interval", nullptr, "Frames between redraws of one face of the world cube (mode 2) once it is complete. Higher is cheaper while you move; reflections follow a little later." },
	{ "Shaders.Reflections.Main", "CubeMoveThreshold", "Environment cube move distance", nullptr, "How far you move before the world cube (mode 2) is redrawn around you. Standing still in steady light it costs nothing. 0 redraws all the time." },
	{ "Shaders.Reflections.Main", "CubeRefreshSeconds", "Environment cube refresh", nullptr, "Seconds after which a world-cube face is redrawn anyway, for doors and moving lamps. 0 never." },
	{ "Shaders.Reflections.Main", "CubeLean", "Environment cube: big things only", nullptr, "Leaves people, creatures, clutter and small objects out of the world cube (mode 2). Cheaper; hardly visible in blurred reflections." },
	// Sun shadows (Main)
	{ "Shaders.ShadowsExteriors.Main", "Enabled", "Sun shadows", nullptr, "Turns the shadows cast by the sun on or off outdoors." },
	{ "Shaders.ShadowsExteriors.Main", "Darkness", "Shadow darkness", nullptr, "How dark sun shadows are. Lower gives pale shadows, higher gives near-black ones." },
	{ "Shaders.ShadowsExteriors.Main", "NightMinDarkness", "Shadow darkness in moonlight", nullptr, "How dark shadows stay on nights with little moon. Set to zero for no shadows when the moon is dark." },
	{ "Shaders.ShadowsExteriors.Main", "SmoothCharacterShadows", "Smooth shadows on faces and skin", nullptr, "Shadows on faces and skin follow the smooth shape of the head instead of showing blocky facets." },
	{ "Shaders.ShadowsExteriors.Main", "UsePointShadowsNight", "Lamp shadows outdoors at night", nullptr, "Lamps and fires outdoors cast shadows at night. Darkens the whole picture slightly." },
	// Sun shadows (Sun movement)
	{ "Shaders.ShadowsExteriors.SunSmoothing", "SmoothSun", "Smooth shadow movement", nullptr, "Shadows glide as the sun moves instead of jumping a step at a time." },
	{ "Shaders.ShadowsExteriors.SunSmoothing", "GlideSeconds", "Shadow glide time (seconds)", nullptr, "How many real seconds each small shadow move takes. Longer is smoother but lags behind the sun." },
	// Sun shadows (Casters in the top-down map)
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Trees", "Trees and plants", nullptr, "Trees count as roofs, so rain and sky light stop under them." },
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Statics", "Buildings and rocks", nullptr, "Buildings and rocks count as roofs, so rain and sky light stop under them." },
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Misc", "Small props and junk", nullptr, "Small props such as awnings and junk count as roofs for rain and sky light." },
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Terrain", "Ground", nullptr, "Hills and overhanging ground count as roofs for rain and sky light." },
	// Sun shadows (Contact shadows)
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Enabled", "Contact shadows (small detail shadows)", nullptr, "Adds small dark shadows where things touch, such as under rocks, plants and feet. Costs a little frame rate." },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Intensity", "Contact shadow strength", nullptr, "How dark the small contact shadows are." },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "BlurRadius", "Contact shadow softness", nullptr, "How soft contact shadows are. Keep it low, or they look blocky." },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Sharp", "Sharper, steadier contact shadows", nullptr, "Crisper contact shadows that shimmer less. Costs a little more frame rate." },
	// Sun shadows (Screen-Space Shadows, Bottled Shaders / Bend Studio port)
	{ "Shaders.ShadowsExteriors.SSShadows", "Enabled", "Bend screen-space shadows (deferred)", nullptr, "Fine screen-space contact shadows of the sun (Bottled Shaders / Bend Studio port) applied to sun light only in deferred mode. Needs a restart to turn on from off." },
	{ "Shaders.ShadowsExteriors.SSShadows", "SampleCount", "Bend shadow reach (samples)", nullptr, "How far each screen-space sun shadow marches across the screen (1-4 x 60 samples at 1080p). Higher costs more GPU." },
	{ "Shaders.ShadowsExteriors.SSShadows", "SurfaceThickness", "Bend surface thickness", nullptr, "How thick surfaces are assumed to be behind the depth buffer. Lower gives sharper, more precise contact shadows." },
	{ "Shaders.ShadowsExteriors.SSShadows", "BilinearThreshold", "Bend edge sharpness threshold", nullptr, "Depth steps above this relative threshold read sharp instead of bilinear, preventing foreground edges from bleeding." },
	{ "Shaders.ShadowsExteriors.SSShadows", "ShadowContrast", "Bend shadow contrast", nullptr, "Contrast of the screen-space sun shadows (1.0 is default; higher is darker/harder)." },
	// Sun shadows (Shadow maps)
	{ "Shaders.ShadowsExteriors.ShadowMaps", nullptr, "Shadow quality",
		"Low: CascadeResolution=0 Distance=4000 | Medium: CascadeResolution=1 Distance=6000 | High: CascadeResolution=2 Distance=8000 | Very high: CascadeResolution=4 Distance=10000",
		"Sharper sun shadows that reach further from you. Higher costs more video memory and frame rate, a lot more at Very high." },
	{ "Shaders.ShadowsExteriors.ShadowMaps", "SmoothFilter", "Smoother shadow edges", nullptr, "Rounds off the stair steps along shadow edges. Edges get slightly softer." },
	{ "Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap", "Sharper shadows on your character", nullptr, "Gives your own character much finer sun shadows, handy in third person. Needs a restart." },
	// Sun shadows (Top-down map)
	{ "Shaders.ShadowsExteriors.Ortho", nullptr, "Roof detection quality (rain, sky light)",
		"Low: Resolution=1 Distance=2000 | Medium: Resolution=2 Distance=3000 | High: Resolution=3 Distance=4000",
		"The overhead map that tells which ground is under a roof: rain and sky light stop there. Higher is more exact and costs a little more." },
	// Sun shadows (Mountain shadows)
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Enabled", "Mountain shadows", nullptr, "Hills and mountains shade the land far into the distance, most visible at sunrise and sunset." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Strength", "Mountain shadow strength", nullptr, "How much sunlight a mountain shadow takes away." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Softness", "Mountain shadow softness", nullptr, "How gradually mountain shadows fade at their edges." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Contrast", "Mountain shadow sharpness", nullptr, "Higher gives a harder line between lit and shaded land." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", nullptr, "Mountain shadow detail",
		"Low: MaxResolution=0 | Medium: MaxResolution=1 | High: MaxResolution=2",
		"How detailed mountain shadows are. Higher is finer and uses more video memory." },
	// Lamp shadows (Main)
	{ "Shaders.ShadowsInteriors.Main", "Darkness", "Shadow darkness", nullptr, "How dark lamp shadows are indoors." },
	{ "Shaders.ShadowsInteriors.Main", "LightPoints", "Lamps casting shadows at once", nullptr, "How many lamps can cast shadows together. More gives richer rooms and costs frame rate." },
	{ "Shaders.ShadowsInteriors.Main", nullptr, "Lamp shadow detail",
		"Low: ShadowCubeMapSize=256 | Medium: ShadowCubeMapSize=512 | High: ShadowCubeMapSize=1024",
		"How sharp each lamp shadow is. Higher costs more video memory and frame rate." },
	{ "Shaders.ShadowsInteriors.Main", "DrawDistance", "Lamp shadow distance", nullptr, "How far from you lamps still cast shadows." },
	{ "Shaders.ShadowsInteriors.Main", "ContactShadows", "Lamp contact shadows (where props meet floors)", nullptr, "Adds small dark lines where props meet the floor or tables meet a wall. Off at zero." },
	// Lamp shadows (Per-object shadows)
	{ "Shaders.ShadowsInteriors.Forward", "Enabled", "Lamp shadows on each object", nullptr, "Each lamp shadows only its own light, so rooms keep their colours. Off gives the older flat darkening." },
	{ "Shaders.ShadowsInteriors.Forward", "Strength", "Lamp shadow strength", nullptr, "How dark a lamp shadow is. At full strength it blocks all of that lamp's light." },
	{ "Shaders.ShadowsInteriors.Forward", "Softness", "Lamp shadow softness", nullptr, "Zero gives hard edges and costs least. Higher gives softer edges and costs frame rate in busy rooms." },
	{ "Shaders.ShadowsInteriors.Forward", nullptr, "Lamp shadow edge quality",
		"Low: Quality=0 | Medium: Quality=1 | High: Quality=2 | Ultra: Quality=3",
		"How smooth lamp shadow edges are. Higher is smoother and costs more frame rate. Needs lamp shadow softness above zero." },
	{ "Shaders.ShadowsInteriors.Forward", "ContactHardening", "Sharp where objects touch the ground", nullptr, "Shadows are sharp where an object touches the floor and blur further away, as in real life." },
	{ "Shaders.ShadowsInteriors.Forward", "LampSize", "Lamp size (softer far shadows)", nullptr, "Bigger lamps make shadows blur more with distance. Zero keeps them sharp." },
	{ "Shaders.ShadowsInteriors.Forward", "FirstPerson", "Shadows on your weapon and arms", nullptr, "Lamp shadows fall on your weapon and arms in first person." },
	// Lamp shadows (Shadow atlas)
	{ "Shaders.ShadowsInteriors.Atlas", nullptr, "Lamp shadow map size",
		"Low: Size=2048 | Medium: Size=4096 | High: Size=8192",
		"Video memory set aside for lamp shadows. Larger is sharper and uses more memory: High takes 128 MB." },
	// Sun shadows (Soft contact edges)
	{ "Shaders.ContactHardening.Main", "SunSize", "Shadow softness with distance", nullptr, "Shadow edges soften further from what casts them. Higher is softer but can show jagged edges." },
	{ "Shaders.ContactHardening.Main", "MaxSoftness", "Softest shadow edge", nullptr, "The softest a shadow edge can get. Higher allows very soft shadows under tall objects and costs more." },
	{ "Shaders.ContactHardening.Main", "AtNight", "Also at night", nullptr, "Keeps soft shadow edges at night too. Moonlight shadows are faint, so you will barely see it." },
	// Cloud shadows (Main)
	{ "Shaders.CloudShadows.Main", "Enabled", "Cloud shadows", nullptr, "Clouds cast moving shadows on the land. Cheap." },
	{ "Shaders.CloudShadows.Main", "Strength", "Cloud shadow strength", nullptr, "How much sunlight a thick cloud takes away." },
	{ "Shaders.CloudShadows.Main", "Density", "Cloud thickness", nullptr, "How solid clouds count. Raise it if thin clouds cast no visible shadow." },
	{ "Shaders.CloudShadows.Main", "Speed", "Cloud shadow speed", nullptr, "How fast the shadows drift with the wind." },
	{ "Shaders.CloudShadows.Main", "TileSize", "Cloud shadow size", nullptr, "Smaller gives smaller, more frequent cloud shadows. Larger gives big, slow ones." },
	{ "Shaders.CloudShadows.Main", "WaterShadows", "Also on water", nullptr, "Cloud and mountain shadows also darken water. Shallow water darkens most." },
	// Skin (Main - outdoors)
	{ "Shaders.Skin.Main", "SpecularStrength", "Skin shine (outdoors)", nullptr, "How strongly skin catches sunlight outdoors. Zero is flat and dry, high looks oily." },
	{ "Shaders.Skin.Main", "Roughness", "Skin roughness (outdoors)", nullptr, "Outdoors: lower gives tight, oily highlights; higher gives a matte look." },
	{ "Shaders.Skin.Main", "Scattering", "Skin softness (outdoors, per-pixel)", nullptr, "Outdoors: per-pixel skin softness. Replaced while Soft skin glow (ScreenSpace) is on." },
	{ "Shaders.Skin.Main", "Translucency", "Backlit glow on ears/fingers (outdoors)", nullptr, "Outdoors: thin parts such as ears and fingers glow warm red when backlit." },
	{ "Shaders.Skin.Main", "SkyReflection", "Sky reflection on skin (outdoors)", nullptr, "Skin reflects the sky at glancing angles. Outdoors only." },
	{ "Shaders.Skin.Main", "RainWetness", "Wet skin in rain", nullptr, "How wet and shiny skin gets in the rain." },
	{ "Shaders.Skin.Main", "VanillaMatchedHighlights", "Vanilla-strength skin highlights", nullptr, "One makes skin highlights as strong as the game's own; zero keeps the physically based strength." },
	// Skin (Interiors - indoors)
	{ "Shaders.Skin.Interiors", "SpecularStrength", "Skin shine (indoors)", nullptr, "How strongly skin catches lamp light indoors. Zero is flat and dry, high looks oily." },
	{ "Shaders.Skin.Interiors", "Roughness", "Skin roughness (indoors)", nullptr, "Indoors: lower gives tight, oily highlights; higher gives a matte look." },
	{ "Shaders.Skin.Interiors", "Scattering", "Skin softness (indoors, per-pixel)", nullptr, "Indoors: per-pixel skin softness. Replaced while Soft skin glow (ScreenSpace) is on." },
	{ "Shaders.Skin.Interiors", "Translucency", "Backlit glow on ears/fingers (indoors)", nullptr, "Indoors: ears and fingers glow warm red when a lamp is behind them." },
	{ "Shaders.Skin.Interiors", "ScreenSpace", "Soft skin glow (indoors)", nullptr, "Spreads light across skin on screen for a softer look indoors." },
	{ "Shaders.Skin.Interiors", "Width", "Skin glow spread (indoors)", nullptr, "Indoors: how far the soft skin glow spreads. Higher is softer and waxier." },
	{ "Shaders.Skin.Interiors", "AlbedoDetail", "Skin texture detail kept (indoors)", nullptr, "Indoors: keeps freckles, pores and stubble sharp. Lower blurs them into a waxy look." },
	// Skin (Scattering - outdoors)
	{ "Shaders.Skin.Scattering", "ScreenSpace", "Soft skin glow (outdoors)", nullptr, "Spreads light across skin on screen for a softer look outdoors." },
	{ "Shaders.Skin.Scattering", "Width", "Skin glow spread (outdoors)", nullptr, "Outdoors: how far the soft skin glow spreads. Higher is softer and waxier." },
	{ "Shaders.Skin.Scattering", "AlbedoDetail", "Skin texture detail kept (outdoors)", nullptr, "Outdoors: keeps freckles, pores and stubble sharp. Lower blurs them into a waxy look." },
	// Cloth (Main)
	{ "Shaders.Cloth.Main", "Enabled", "Moving cloth", nullptr, "Capes, coats, flags, banners and curtains sway. Needs a restart." },
	{ "Shaders.Cloth.Main", "UseGameWind", "Follow the wind of the weather", nullptr, "Cloth moves with the weather's wind direction and strength outdoors." },
	{ "Shaders.Cloth.Main", "WindIntensity", "Cloth movement amount", nullptr, "How far loose cloth moves. Higher looks windier." },
	{ "Shaders.Cloth.Main", "FlutterAmount", "Flutter", nullptr, "How much cloth quivers with small fast movements." },
	{ "Shaders.Cloth.Main", "RippleAmount", "Billowing", nullptr, "How much cloth billows in and out like a sail." },
	{ "Shaders.Cloth.Main", "WalkSim", "Coats swing as you walk", nullptr, "Worn coats hang from the hips and the hem trails behind you as you move." },
	{ "Shaders.Cloth.Main", "AutoDetect", "Move all loose clothing", nullptr, "Clothing not on the cloth list also moves if it hangs free of the legs, such as long coats and robes." },
	{ "Shaders.Cloth.Main", "MotionIndoors", "Coats swing indoors too", nullptr, "Worn coats follow your movement indoors as well." },
	// Cloth (Interiors)
	{ "Shaders.Cloth.Interiors", "Enabled", "Cloth moves indoors", nullptr, "Curtains, banners and flags sway gently indoors, in a draught." },
	{ "Shaders.Cloth.Interiors", "Strength", "Indoor cloth movement", nullptr, "How much indoor cloth moves. Zero is still, higher moves more." },
	// Pip-Boy sleeves (Main)
	{ "Shaders.PipboyFit.Main", "Enabled", "Sleeves fit under the Pip-Boy", nullptr, "In first person, armour sleeves are pulled in so they do not poke through the Pip-Boy. Needs a restart." },
	{ "Shaders.PipboyFit.Main", "Strength", "Fit strength", nullptr, "How much the sleeves are pulled in. Zero leaves them as they were." },
	{ "Shaders.PipboyFit.Main", "ThirdPerson", "Also in third person", nullptr, "Your own sleeves are fitted in third person too. Other characters are not changed." },
	// Blood on weapon and hands (Main)
	{ "Shaders.HandBlood.Main", "PerHit", "Blood on weapon and hands", nullptr, "Fresh blood nearby splashes onto your weapon and gloves. Higher gets you bloodier faster." },
	{ "Shaders.HandBlood.Main", "Reach", "Distance blood reaches you", nullptr, "How close fresh blood must be to get on you. Small means point blank only." },
	{ "Shaders.HandBlood.Main", "ColorR", "Blood colour", nullptr, "The colour of blood on your hands and weapon, until a blood splat has been seen." },
	{ "Shaders.HandBlood.Main", "Brightness", "Blood brightness", nullptr, "Below one gives darker, thicker-looking blood on the gun." },
	{ "Shaders.HandBlood.Main", "CellSize", "Spatter droplet size", nullptr, "Bigger gives a coarser spatter." },
	{ "Shaders.HandBlood.Main", "Run", "Blood running", nullptr, "How far drops run down after a hit. Zero keeps them round." },
	{ "Shaders.HandBlood.Main", "DryTime", "Drying time", nullptr, "Seconds until the blood dries dark and matte." },
	{ "Shaders.HandBlood.Main", "FadeTime", "Time to wear off", nullptr, "Seconds until the blood wears off. Zero keeps it. Water washes it off." },
	// Sky (Main)
	{ "Shaders.Sky.Main", "TintDayR", "Sky colour by day", nullptr, "Tints the daytime sky. White leaves it as the weather made it." },
	{ "Shaders.Sky.Main", "TintNightR", "Sky colour at night", nullptr, "Tints the night sky, for example a little blue for moonlit nights." },
	{ "Shaders.Sky.Main", "SaturationDay", "Sky colourfulness by day", nullptr, "How colourful the daytime sky is. Lower is paler and greyer, higher is richer." },
	{ "Shaders.Sky.Main", "SaturationNight", "Sky colourfulness at night", nullptr, "How colourful the night sky is. Lower is paler and greyer, higher is richer." },
	{ "Shaders.Sky.Main", "SunsetR", "Sunset colour boost", nullptr, "Adds colour to the sun at sunrise and sunset. Red by default." },
	{ "Shaders.Sky.Main", "MoonBrightness", "Moon brightness", nullptr, "How bright the moon looks, and how much light it throws on the world at night." },
	{ "Shaders.Sky.Main", "StarStrength", "Star brightness", nullptr, "How bright the stars are at night." },
	{ "Shaders.Sky.Main", "StarTwinkle", "Star twinkle speed", nullptr, "How fast the stars twinkle." },
	// Sky (Physical)
	{ "Shaders.Sky.Physical", "Enabled", "Realistic sky", nullptr, "Paints the sky from simulated air: blue overhead, pale at the horizon, a glow round the sun, red and gold at sunset." },
	{ "Shaders.Sky.Physical", "Brightness", "Sky brightness", nullptr, "How bright the realistic sky is compared with the weather's own sky." },
	{ "Shaders.Sky.Physical", "WeatherTint", "Keep the weather's sky colours", nullptr, "How much of each weather's own sky colour is kept. Low gives pure air colours." },
	{ "Shaders.Sky.Physical", "Haze", "Dust haze in the air", nullptr, "Dust in the air: a wider, whiter glow round the sun and a paler horizon." },
	// Sky (Clouds)
	{ "Shaders.Sky.Clouds", "Transparency", "Flat cloud transparency", nullptr, "How see-through the game's flat clouds are." },
	{ "Shaders.Sky.Clouds", "Brightness", "Flat cloud brightness", nullptr, "How bright the game's flat clouds are." },
	// VolumetricClouds (Main)
	{ "Shaders.VolumetricClouds.Main", nullptr, "Cloud quality",
		"Low: Resolution=0 Steps=32 | Medium: Resolution=0 Steps=48 | High: Resolution=1 Steps=64 | Very high: Resolution=2 Steps=64",
		"More detail and steadier cloud edges. Very high costs a lot of frame rate." },
	{ "Shaders.VolumetricClouds.Main", "CoveragePleasant", "Cloud cover in fair weather", nullptr, "How much of the sky the clouds fill in fair weather." },
	{ "Shaders.VolumetricClouds.Main", "CoverageCloudy", "Cloud cover when cloudy", nullptr, "How much of the sky the clouds fill in cloudy weather." },
	{ "Shaders.VolumetricClouds.Main", "Thickness", "Cloud thickness", nullptr, "How deep the cloud layer is. Deeper gives taller, heaped clouds with darker undersides." },
	{ "Shaders.VolumetricClouds.Main", "Density", "Cloud density", nullptr, "How solid the clouds are. Higher is denser and darker underneath, lower is more see-through." },
	{ "Shaders.VolumetricClouds.Main", "Brightness", "Cloud brightness", nullptr, "Overall brightness of the clouds." },
	{ "Shaders.VolumetricClouds.Main", "WindSpeed", "Cloud drift speed", nullptr, "How fast the clouds drift across the sky." },
	{ "Shaders.VolumetricClouds.Main", "Shadows", "Cloud shadows on the ground", nullptr, "Clouds cast moving shadows over the land. Zero turns them off." },
	// VolumetricFog (Main)
	{ "Shaders.VolumetricFog.Main", nullptr, "Fog style",
		"Current: LegacyFog=false ClassicLook=0 | Classic: LegacyFog=false ClassicLook=1 | Legacy: LegacyFog=true ClassicLook=0",
		"Current: this version's fog. Classic: the original mod's look. Legacy: the older fog, for presets made for it." },
	{ "Shaders.VolumetricFog.Main", "Amount", "Fog strength", nullptr, "How strong the fog is outdoors." },
	{ "Shaders.VolumetricFog.Main", "StartDistance", "Nearest distance with fog", nullptr, "Distance from you (game units, 70 = 1 m) where fog begins outdoors. Zero starts right at you." },
	{ "Shaders.VolumetricFog.Main", "BaseDensity", "Fog thickness", nullptr, "How thick the fog is, whatever the weather. Higher hides the distance sooner." },
	{ "Shaders.VolumetricFog.Main", "FogSaturation", "Fog colour richness", nullptr, "How rich the fog's colour is. Lower is greyer." },
	{ "Shaders.VolumetricFog.Main", "NoiseStrength", "Fog patchiness", nullptr, "How patchy the fog is. Zero is an even haze, higher gives drifting banks." },
	{ "Shaders.VolumetricFog.Main", "WindSpeed", "Fog drift speed", nullptr, "How fast the fog patches drift." },
	{ "Shaders.VolumetricFog.Main", "SunGlow", "Glow round the sun", nullptr, "The bright glow in the fog round the sun, strongest at sunrise and sunset." },
	{ "Shaders.VolumetricFog.Main", "DistantFogStart", "Distant haze start distance", nullptr, "Distance from you (game units) where distant horizon haze starts. -1 uses DistantFogRange." },
	{ "Shaders.VolumetricFog.Main", "DistantFogBlend", "Haze on distant hills", nullptr, "How strongly far hills and terrain fade into the sky near the horizon." },
	// VolumetricFog (Night)
	{ "Shaders.VolumetricFog.Night", "AmountScale", "Fog strength at night", nullptr, "Fog strength at night compared with the day. Below 1 is thinner, above 1 is stronger." },
	{ "Shaders.VolumetricFog.Night", "StartDistance", "Nearest distance with fog at night", nullptr, "Distance from you (game units) where fog begins at night. -1 uses the daytime distance." },
	{ "Shaders.VolumetricFog.Night", "DensityScale", "Fog thickness at night", nullptr, "Fog thickness at night compared with the day." },
	{ "Shaders.VolumetricFog.Night", "InscatteringScale", "Fog brightness at night", nullptr, "How bright and glowing the fog looks at night. Lower is darker." },
	{ "Shaders.VolumetricFog.Night", "TintR", "Fog colour at night", nullptr, "Tints the fog at night. White leaves it as it is." },
	// VolumetricFog (Interiors)
	{ "Shaders.VolumetricFog.Interiors", "Amount", "Fog strength", nullptr, "How strong the fog is indoors." },
	{ "Shaders.VolumetricFog.Interiors", "StartDistance", "Nearest distance with fog", nullptr, "Distance from you (game units, 70 = 1 m) where fog begins indoors. Zero starts right at you." },
	{ "Shaders.VolumetricFog.Interiors", "BaseDensity", "Fog thickness", nullptr, "How thick the fog is indoors." },
	{ "Shaders.VolumetricFog.Interiors", "FogSaturation", "Fog colour richness", nullptr, "How rich the fog's colour is indoors. Lower is greyer." },
	{ "Shaders.VolumetricFog.Interiors", "NoiseStrength", "Fog patchiness", nullptr, "How patchy the indoor fog is. Zero is an even haze." },
	{ "Shaders.VolumetricFog.Interiors", "LampHazeEnabled", "Glow round lamps in the fog", nullptr, "Lamps glow softly in the fog indoors. Off while it is being tuned." },
	{ "Shaders.VolumetricFog.Interiors", "LampHaze", "Lamp glow strength", nullptr, "How strongly the fog glows round each lamp. Needs the lamp glow switched on." },
	// DustMotes (Main)
	{ "Shaders.DustMotes.Main", "Density", "Amount of dust", nullptr, "How many specks of dust float in the air." },
	{ "Shaders.DustMotes.Main", "Size", "Dust speck size", nullptr, "How big each speck of dust looks." },
	{ "Shaders.DustMotes.Main", "Brightness", "Dust brightness", nullptr, "How brightly the specks catch the light." },
	{ "Shaders.DustMotes.Main", "MaxDistance", "Dust distance", nullptr, "How far from you dust is drawn. Farther costs more frame rate." },
	{ "Shaders.DustMotes.Main", "Drift", "Dust drift speed", nullptr, "How fast the dust floats about." },
	{ "Shaders.DustMotes.Main", "Interiors", "Dust indoors", nullptr, "Show floating dust indoors, lit by the room's lamps." },
	{ "Shaders.DustMotes.Main", "Exteriors", "Dust outdoors", nullptr, "Show floating dust outdoors, lit by street lights and fires at night." },
	// VolumetricLight (Main)
	{ "Shaders.VolumetricLight.Main", "Strength", "Light shaft strength", nullptr, "How visible the sunbeams through trees and buildings are by day. Outdoors only; needs the exterior shadows on." },
	{ "Shaders.VolumetricLight.Main", "Density", "Air thickness", nullptr, "How thick the air is. Higher gives stronger, hazier beams over a shorter distance." },
	{ "Shaders.VolumetricLight.Main", "MaxDistance", "Light shaft distance", nullptr, "How far from you the beams are drawn." },
	{ "Shaders.VolumetricLight.Main", "Darken", "Darkening of shaded air", nullptr, "How much shaded air between the beams dims what is behind it." },
	{ "Shaders.VolumetricLight.Main", "RainStrength", "Strength in rain", nullptr, "How much of the beam strength is kept in rain." },
	{ "Shaders.VolumetricLight.Main", nullptr, "Quality",
		"Low: Steps=8 BlurRadius=6.0 | Medium: Steps=16 BlurRadius=4.0 | High: Steps=32 BlurRadius=3.0",
		"More samples and less smoothing give cleaner beams but cost more frame rate." },
	// VolumetricLight (Night)
	{ "Shaders.VolumetricLight.Night", "Strength", "Moonlight shaft strength", nullptr, "How visible the moonlight beams are at night." },
	{ "Shaders.VolumetricLight.Night", "Darken", "Darkening of shaded air", nullptr, "How much shaded air between the beams dims what is behind it at night." },
	{ "Shaders.VolumetricLight.Night", "Density", "Air thickness", nullptr, "How thick the air is at night. Higher gives stronger, hazier beams." },
	{ "Shaders.VolumetricLight.Night", "MaxDistance", "Light shaft distance", nullptr, "How far from you the moonlight beams are drawn." },
	// GodRays (Main)
	{ "Shaders.GodRays.Main", "DayMultiplier", "Light shafts by day", nullptr, "How strong the light shafts streaming from the sun are by day." },
	{ "Shaders.GodRays.Main", "NightMultiplier", "Light shafts at night", nullptr, "How strong the light shafts are at night." },
	{ "Shaders.GodRays.Main", "RayLength", "Light shaft length", nullptr, "How long the light shafts stretch across the screen." },
	{ "Shaders.GodRays.Main", "RayIntensity", "Light shaft brightness", nullptr, "How bright the light shafts are." },
	{ "Shaders.GodRays.Main", "RayVisibility", "Light shaft contrast", nullptr, "Contrast of the shafts. Higher makes them sharper and fewer." },
	{ "Shaders.GodRays.Main", "TimeEnabled", "Strongest at sunrise and sunset", nullptr, "Shafts are strongest at sunrise and sunset and fade away at noon." },
	// GodRays (Coloring)
	{ "Shaders.GodRays.Coloring", "RayR", "Light shaft colour", nullptr, "The colour used for the light shafts." },
	{ "Shaders.GodRays.Coloring", "Saturate", "Use this colour (0 = sun colour)", nullptr, "How much of your colour is used. Zero takes the sun's colour, full uses only yours." },
	// Sandstorm (Main)
	{ "Shaders.Sandstorm.Main", "Visibility", "Visibility in the storm", nullptr, "How far you can see in a full storm. Lower is a thicker storm." },
	{ "Shaders.Sandstorm.Main", "Billowing", "Cloudiness of the dust", nullptr, "Zero is an even haze, higher gives rolling dust clouds with clearer gaps." },
	{ "Shaders.Sandstorm.Main", "StreakStrength", "Blowing sand streaks", nullptr, "Thin streaks of sand blown past the camera. Zero turns them off." },
	{ "Shaders.Sandstorm.Main", "DustColorR", "Dust colour", nullptr, "The colour of the dust." },
	{ "Shaders.Sandstorm.Main", "Brightness", "Dust brightness", nullptr, "How brightly lit the dust is." },
	{ "Shaders.Sandstorm.Main", "WindSpeed", "Dust speed", nullptr, "How fast the dust blows in strong wind." },
	{ "Shaders.Sandstorm.Main", "GrassWind", "Grass sway in the storm", nullptr, "How hard grass sways and flutters in the storm." },
	{ "Shaders.Sandstorm.Main", "TreeWind", "Tree sway in the storm", nullptr, "How hard tree leaves swing in the storm." },
	// HeatHaze (Main)
	{ "Shaders.HeatHaze.Main", "Strength", "Haze strength", nullptr, "How much the distant ground wobbles in the heat. It builds through the day and is gone at night." },
	{ "Shaders.HeatHaze.Main", "StartDistance", "Nearest distance with haze", nullptr, "Nothing nearer than this shimmers, so close things never waver." },
	{ "Shaders.HeatHaze.Main", "NoiseScale", "Ripple fineness", nullptr, "Size of the ripples. Higher gives finer, busier ripples." },
	{ "Shaders.HeatHaze.Main", "Speed", "Ripple speed", nullptr, "How fast the ripples rise." },
	{ "Shaders.HeatHaze.Main", "PeakHour", "Hottest hour of the day", nullptr, "The hottest hour of the day, when the haze is strongest." },
	{ "Shaders.HeatHaze.Main", "CloudyScale", "Haze in cloudy weather", nullptr, "How much haze is left in cloudy weather compared with a clear day." },
	// Precipitations (Main)
	{ "Shaders.Precipitations.Main", "Opacity", "Rain visibility", nullptr, "How solid and visible the raindrops are." },
	{ "Shaders.Precipitations.Main", "Speed", "Rain speed", nullptr, "How fast the rain falls." },
	{ "Shaders.Precipitations.Main", "VerticalScale", "Rain streak length", nullptr, "Longer values draw rain as longer streaks." },
	{ "Shaders.Precipitations.Main", "Refraction", "Drop distortion", nullptr, "How much raindrops bend and distort the view behind them." },
	{ "Shaders.Precipitations.Main", "Coloring", "Rain brightness", nullptr, "Makes the rain brighter and more visible." },
	// Snow (Main)
	{ "Shaders.Snow.Main", "Speed", "Snowfall speed", nullptr, "How fast the snow falls." },
	{ "Shaders.Snow.Main", "WorldSpace", "World-space snow", nullptr, "Flakes have a place in the world: the snow stays put when you turn. Off is the old snow." },
	{ "Shaders.Snow.Main", "WorldFlakeSize", "Flake size", nullptr, "How big each flake is." },
	{ "Shaders.Snow.Main", "WorldDensity", "Flake density", nullptr, "How thick the snowfall is." },
	{ "Shaders.Snow.Main", "WorldDistance", "Snow distance", nullptr, "How far away flakes are drawn. Shorter is cheaper." },
	{ "Shaders.Snow.Main", "WorldOpacity", "Flake opacity", nullptr, "How solid each flake is." },
	{ "Shaders.Snow.Main", "WorldSurfaceMarks", "Flakes landing on surfaces", nullptr, "Short-lived marks where flakes land on open ground and roofs." },
	{ "Shaders.Snow.Main", "WorldCloudWind", "Drift with the cloud wind", nullptr, "How strongly the snow drifts sideways with the clouds." },
	{ "Shaders.Snow.Main", "WorldPlantWind", "Sway with the plant wind", nullptr, "How much flakes sway with the gusts that move grass and leaves." },
	// SnowAccumulation (Main)
	{ "Shaders.SnowAccumulation.Main", "SnowColorR", "Snow colour", nullptr, "The colour of the snow lying on the ground." },
	{ "Shaders.SnowAccumulation.Main", "SunPower", "Snow brightness in sunlight", nullptr, "How brightly the sun lights the snow." },
	{ "Shaders.SnowAccumulation.Main", "Increase", "Hours for snow to build up", nullptr, "Game hours of snowfall before the ground is covered. Lower is faster." },
	{ "Shaders.SnowAccumulation.Main", "Decrease", "Hours for snow to melt", nullptr, "Game hours before the snow melts away. Higher keeps it longer." },
	{ "Shaders.SnowAccumulation.Main", "Persistent", "Snow stays where it fell (spot by spot)", nullptr, "Snow builds up and melts spot by spot, so sheltered ground stays clear. Needs snowfall." },
	{ "Shaders.SnowAccumulation.Main", "PersistentFootprints", "Footprints in snow", nullptr, "Your steps leave footprints in the snow." },
	{ "Shaders.SnowAccumulation.Main", "PersistentDrifts", "Snow drifts in hollows", nullptr, "More snow gathers in hollows and against walls, less on ridges." },
	// WetWorld (Main)
	{ "Shaders.WetWorld.Main", "Persistent", "Ground stays wet where rain fell", nullptr, "Rain wets only the ground it reaches, so covered spots stay dry and puddles form in the open." },
	{ "Shaders.WetWorld.Main", "PersistentWetTime", "Hours of rain to wet the ground", nullptr, "Game hours of rain before open ground is fully wet." },
	{ "Shaders.WetWorld.Main", "PersistentDryTime", "Hours for wet ground to dry", nullptr, "Game hours for wet ground to dry in full sun. Shade takes longer." },
	// Water (Default)
	{ "Shaders.Water.Default", "reflectivity", "Reflections", nullptr, "How strongly the water mirrors its surroundings." },
	{ "Shaders.Water.Default", "choppiness", "Wave sharpness", nullptr, "Higher gives sharper, more defined wave crests." },
	{ "Shaders.Water.Default", "waveSpeed", "Wave speed", nullptr, "How fast the waves move." },
	{ "Shaders.Water.Default", "waveWidth", "Wave size", nullptr, "Bigger values make larger waves; smaller values make fine ripples." },
	{ "Shaders.Water.Default", "turbidity", "Water murkiness", nullptr, "Higher makes the water murkier." },
	{ "Shaders.Water.Default", "refractionPower", "Refraction", nullptr, "How much the water bends the view of what lies beneath." },
	{ "Shaders.Water.Default", "causticsStrength", "Underwater light patterns", nullptr, "Rippling bright patterns of light seen when you are underwater." },
	{ "Shaders.Water.Default", "depthDarkness", "Darkness with depth", nullptr, "How dark the water gets as it gets deeper." },
	// Water (Interiors)
	{ "Shaders.Water.Interiors", "reflectivity", "Reflections", nullptr, "How strongly the water mirrors its surroundings." },
	{ "Shaders.Water.Interiors", "choppiness", "Wave sharpness", nullptr, "Higher gives sharper, more defined wave crests." },
	{ "Shaders.Water.Interiors", "waveSpeed", "Wave speed", nullptr, "How fast the waves move." },
	{ "Shaders.Water.Interiors", "waveWidth", "Wave size", nullptr, "Bigger values make larger waves; smaller values make fine ripples." },
	{ "Shaders.Water.Interiors", "turbidity", "Water murkiness", nullptr, "Higher makes the water murkier." },
	{ "Shaders.Water.Interiors", "refractionPower", "Refraction", nullptr, "How much the water bends the view of what lies beneath." },
	{ "Shaders.Water.Interiors", "depthDarkness", "Darkness with depth", nullptr, "How dark the water gets as it gets deeper." },
	{ "Shaders.Water.Interiors", "shoreFactor", "Shore fade width", nullptr, "How wide the soft fade is where the water meets a shallow shore." },
	// Water (Placed)
	{ "Shaders.Water.Placed", "reflectivity", "Reflections", nullptr, "How strongly the water mirrors its surroundings." },
	{ "Shaders.Water.Placed", "choppiness", "Wave sharpness", nullptr, "Higher gives sharper, more defined wave crests." },
	{ "Shaders.Water.Placed", "waveSpeed", "Wave speed", nullptr, "How fast the waves move." },
	{ "Shaders.Water.Placed", "waveWidth", "Wave size", nullptr, "Bigger values make larger waves; smaller values make fine ripples." },
	{ "Shaders.Water.Placed", "turbidity", "Water murkiness", nullptr, "Higher makes the water murkier." },
	{ "Shaders.Water.Placed", "refractionPower", "Refraction", nullptr, "How much the water bends the view of what lies beneath." },
	{ "Shaders.Water.Placed", "depthDarkness", "Darkness with depth", nullptr, "How dark the water gets as it gets deeper." },
	{ "Shaders.Water.Placed", "shoreFactor", "Shore fade width", nullptr, "How wide the soft fade is where the water meets a shallow shore." },
	// WaterLens (Main)
	{ "Shaders.WaterLens.Main", "Amount", "Water on the lens", nullptr, "How much water clings to the camera after you leave the water." },
	{ "Shaders.WaterLens.Main", "Viscosity", "Droplet thickness", nullptr, "Higher makes the droplets thicker and slower to run." },
	// Terrain (Main)
	{ "Shaders.Terrain.Main", "UsePBR", "Realistic ground lighting", nullptr, "Realistic lighting on the ground. Off gives the game's own ground lighting." },
	{ "Shaders.Terrain.Main", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.Main", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.Main", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.Main", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.Main", "SkylightingSaturation", "Sky colour strength on ground", nullptr, "How vivid the sky colour on shaded ground is. 0 is grey, 1 matches the sky." },
	{ "Shaders.Terrain.Main", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.Main", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (Rain)
	{ "Shaders.Terrain.Rain", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.Rain", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.Rain", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.Rain", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.Rain", "SkylightingSaturation", "Sky colour strength on ground", nullptr, "How vivid the sky colour on shaded ground is. 0 is grey, 1 matches the sky." },
	{ "Shaders.Terrain.Rain", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.Rain", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (Night)
	{ "Shaders.Terrain.Night", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.Night", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.Night", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.Night", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.Night", "SkylightingSaturation", "Sky colour strength on ground", nullptr, "How vivid the sky colour on shaded ground is. 0 is grey, 1 matches the sky." },
	{ "Shaders.Terrain.Night", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.Night", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (NightRain)
	{ "Shaders.Terrain.NightRain", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.NightRain", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.NightRain", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.NightRain", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.NightRain", "SkylightingSaturation", "Sky colour strength on ground", nullptr, "How vivid the sky colour on shaded ground is. 0 is grey, 1 matches the sky." },
	{ "Shaders.Terrain.NightRain", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.NightRain", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (Parallax)
	{ "Shaders.Terrain.Parallax", "Enabled", "Ground depth (parallax)", nullptr, "Gives the ground a feeling of depth, so stones and ruts look raised as you move." },
	{ "Shaders.Terrain.Parallax", "Shadows", "Self-shadowing in the ground", nullptr, "Raised bits of ground shade their own surroundings." },
	{ "Shaders.Terrain.Parallax", "ShadowsIntensity", "Ground self-shadow strength", nullptr, "How dark those ground shadows are." },
	{ "Shaders.Terrain.Parallax", nullptr, "Quality",
		"Low: HighQuality=false MaxDistance=1024 | Medium: HighQuality=false MaxDistance=2048 | High: HighQuality=true MaxDistance=2048 | Ultra: HighQuality=true MaxDistance=4096",
		"How finely and how far away the ground depth is drawn. Ultra reaches twice as far and costs the most frame rate." },
	// Terrain (Blending)
	{ "Shaders.Terrain.Blending", "Enabled", "Blend objects into the ground", nullptr, "Objects sink softly into the ground where they meet it. Needs a game restart to turn on." },
	{ "Shaders.Terrain.Blending", "BlendDistance", "Blend depth", nullptr, "How deep an object can sink into the ground before it fades away." },
	{ "Shaders.Terrain.Blending", "HeightBlend", "Follow ground texture detail", nullptr, "Makes the blend edge follow the ground texture, with sand reaching over the object." },
	{ "Shaders.Terrain.Blending", "MaxDistance", "Blend distance", nullptr, "How far away the blending still works." },
	// Terrain (Erosion)
	{ "Shaders.Terrain.Erosion", "Enabled", "Gullies on distant slopes", nullptr, "Adds gullies to the shading of distant slopes and mountains." },
	{ "Shaders.Terrain.Erosion", "Depth", "Gully depth", nullptr, "How deep the gullies look." },
	{ "Shaders.Terrain.Erosion", "GullyDarkening", "Gully darkness", nullptr, "How much darker the gully floors are than the crests." },
	{ "Shaders.Terrain.Erosion", "Spacing", "Gully spacing", nullptr, "Distance between the widest gullies. Smaller packs them tighter." },
	// Terrain (LOD)
	{ "Shaders.Terrain.LOD", "LODNoiseScale", "Distant land noise amount", nullptr, "How much noise is added to distant land to break up flat colour." },
	{ "Shaders.Terrain.LOD", "LODNoiseTile", "Distant land noise detail", nullptr, "Higher makes the noise finer; lower makes it broader." },
	// Terrain (Variation)
	{ "Shaders.Terrain.Variation", "Enabled", "Less repeating ground", nullptr, "Breaks up the repeating look of the ground so the desert stops looking tiled." },
	{ "Shaders.Terrain.Variation", "Layers", "Ground textures varied", nullptr, "How many ground textures per patch get the variation. More costs a little. Needs a restart." },
	{ "Shaders.Terrain.Variation", "Scale", "Variation patch density", nullptr, "Higher changes the pattern more often; lower makes bigger patches." },
	{ "Shaders.Terrain.Variation", "NormalMaps", "Vary ground bumps too", nullptr, "The ground bumps vary too. Off varies colour only and is cheaper." },
	{ "Shaders.Terrain.Variation", "StartDistance", "Variation starts this far away", nullptr, "How far from you the variation begins. Zero means everywhere." },
	// Terrain (Macro)
	{ "Shaders.Terrain.Macro", "Enabled", "Large-scale ground colour variation", nullptr, "Slow changes in ground brightness and tint over hundreds of metres, such as paler sand here and darker gravel there." },
	{ "Shaders.Terrain.Macro", "Strength", "Strength", nullptr, "How much of the variation shows." },
	{ "Shaders.Terrain.Macro", "Size", "Patch size", nullptr, "How big each patch is. Smaller is busier; larger is broad regions." },
	{ "Shaders.Terrain.Macro", "Brightness", "Brightness change", nullptr, "How much lighter or darker the patches get." },
	{ "Shaders.Terrain.Macro", "Tint", "Warm and cool tint change", nullptr, "How much warmer or cooler the patches get." },
	// Terrain (LODBlending)
	{ "Shaders.Terrain.LODBlending", "Enabled", "Match distant land to near land", nullptr, "Adjusts distant land so it matches the ground near you." },
	{ "Shaders.Terrain.LODBlending", "Brightness", "Distant land brightness", nullptr, "Brightens distant land if it looks darker than the ground near you." },
	{ "Shaders.Terrain.LODBlending", "Gamma", "Distant land gamma", nullptr, "Above one darkens the dark parts of distant land for more contrast." },
	// Terrain (Displacement)
	{ "Shaders.Terrain.Displacement", "Enabled", "Real 3D bumps on the ground", nullptr, "Real 3D bumps on the ground, not just painted shading. Needs the DXVK fork and costs frame rate." },
	{ "Shaders.Terrain.Displacement", "Strength", "Bump height", nullptr, "How tall the bumps are, from lowest to highest point." },
	{ "Shaders.Terrain.Displacement", "BumpSize", "Smallest bump size", nullptr, "The smallest bump the ground gets. Larger leaves out fine grain." },
	{ "Shaders.Terrain.Displacement", nullptr, "Quality",
		"Low: EdgeLength=24.0 MaxFactor=8 | Medium: EdgeLength=16.0 MaxFactor=16 | High: EdgeLength=12.0 MaxFactor=16 | Ultra: EdgeLength=8.0 MaxFactor=32",
		"How finely the ground is cut up for the bumps. Finer bumps look better and cost more frame rate." },
	{ "Shaders.Terrain.Displacement", nullptr, "Distance",
		"Near: FadeStart=512.0 FadeEnd=1024.0 | Medium: FadeStart=1024.0 FadeEnd=2048.0",
		"How far from you the bumps reach before they fade out. Medium costs more than Near." },
	{ "Shaders.Terrain.Displacement", "CastShadows", "Bumps cast shadows", nullptr, "The bumps throw small sun shadows, so stones and ruts look real." },
	{ "Shaders.Terrain.Displacement", "BendNormals", "Light bumps as real slopes", nullptr, "Bumps are lit as slopes: the side facing the sun is brighter, the other darker." },
	// POM (Main)
	{ "Shaders.POM.Main", "HeightMapScale", "Rock and wall depth (parallax)", nullptr, "How deep rocks and walls with depth maps look." },
	// POM (Displacement)
	{ "Shaders.POM.Displacement", "Enabled", "Real 3D bumps on rocks and cliffs", nullptr, "Real 3D bumps on rocks and cliffs, not just painted shading. Needs the DXVK fork and costs frame rate." },
	{ "Shaders.POM.Displacement", "Strength", "Bump height", nullptr, "How tall the bumps on rocks and cliffs are." },
	{ "Shaders.POM.Displacement", nullptr, "Quality",
		"Low: EdgePixels=24.0 MaxFactor=8 | Medium: EdgePixels=16.0 MaxFactor=16 | High: EdgePixels=12.0 MaxFactor=16 | Ultra: EdgePixels=8.0 MaxFactor=32",
		"How finely rocks and cliffs are cut up for the bumps. Finer bumps look better and cost more frame rate." },
	{ "Shaders.POM.Displacement", nullptr, "Distance",
		"Near: FadeStart=512.0 FadeEnd=1024.0 | Medium: FadeStart=1024.0 FadeEnd=2048.0",
		"How far from you the bumps reach before they fade out. Medium costs more than Near." },
	// Grass (Main)
	{ "Shaders.Grass.Main", "CastShadows", "Grass casts shadows", nullptr, "Grass throws sun shadows on the ground." },
	{ "Shaders.Grass.Main", "CastShadowsReach", "Grass shadow distance", nullptr, "How far from you the grass casts shadows." },
	// Grass (GPUGrass)
	{ "Shaders.Grass.GPUGrass", "Enabled", "Strand grass (a different grass renderer: needs the DXVK fork)", nullptr, "Draws grass as many thin strands instead of the game's flat cards. Looks very different. Needs the DXVK fork." },
	{ "Shaders.Grass.GPUGrass", "KeepCards", "Keep the game's flat grass as well (fuller)", nullptr, "Keeps the game's flat grass and adds strands around it for fuller clumps." },
	{ "Shaders.Grass.GPUGrass", "Density", "Grass density", nullptr, "More strand clumps beside each grass tuft. Denser fields cost more." },
	{ "Shaders.Grass.GPUGrass", nullptr, "Quality",
		"Low: Blades=24 BladesFar=4 | Medium: Blades=40 BladesFar=6 | High: Blades=64 BladesFar=8 | Ultra: Blades=96 BladesFar=12",
		"How many strands make up each clump of strand grass, near and far. More is fuller and costs more." },
	{ "Shaders.Grass.GPUGrass", "BladeDistance", "Strand grass distance", nullptr, "How far away strand grass is drawn. Beyond it the game's flat grass is used." },
	{ "Shaders.Grass.GPUGrass", "HeightScale", "Strand grass height", nullptr, "How tall the strands are compared to the plant." },
	// Grass (Wind)
	{ "Shaders.Grass.Wind", "Enabled", "Grass moves in waves of wind", nullptr, "Grass moves like a field in the wind, with gusts rolling across it." },
	{ "Shaders.Grass.Wind", "GustStrength", "Gust strength", nullptr, "How far a passing gust leans the grass." },
	{ "Shaders.Grass.Wind", "GustSize", "Gust size", nullptr, "Distance between gusts. Larger gives broad slow waves; smaller gives choppy ones." },
	{ "Shaders.Grass.Wind", "GustSpeed", "Gust speed", nullptr, "How fast gusts roll across the grass." },
	{ "Shaders.Grass.Wind", "Flutter", "Small quiver", nullptr, "Each tuft's own small quiver, so neighbours never move in step." },
	// Plants (Wind)
	{ "Shaders.Plants.Wind", "Enabled", "Plants move in the wind", nullptr, "Plants and bushes move in the same wind and gusts as the grass." },
	{ "Shaders.Plants.Wind", "Bend", "Plant sway", nullptr, "How far a gust leans the top of a plant." },
	{ "Shaders.Plants.Wind", "LeafGust", "Bush leaf sway in gusts", nullptr, "How much gusts strengthen the swing of bush leaves." },
	{ "Shaders.Plants.Wind", "LeafFlutter", "Bush leaf flutter", nullptr, "Extra leaf flutter on bushes in the wind." },
	// Foliage (Main)
	{ "Shaders.Foliage.Main", "Translucency", "Light shining through leaves and grass", nullptr, "Grass and leaves glow softly when the sun is behind them. Zero is off." },
	{ "Shaders.Foliage.Main", "Power", "Glow tightness (how close to the sun)", nullptr, "Higher shows the glow only when looking almost straight at the sun." },
	// Particles (Main)
	{ "Shaders.Particles.Main", "Strength", "Lighting on smoke, dust and blood", nullptr, "Smoke, dust and blood are lit by the scene instead of looking flat. Zero is the game's look." },
	{ "Shaders.Particles.Main", "SoftScale", "Soft smoke and dust edges", nullptr, "Smoke, dust, blood and sparks fade where they meet the ground and walls instead of cutting a hard line. Zero is off. Needs particle lighting on at startup." },
	{ "Shaders.Particles.Main", "MeshSoftScale", "Soft glow and flash edges", nullptr, "The same for glows, lamp cards, muzzle flashes and dust storm walls. Lower it if big dust storms look too faint near the ground." },
	{ "Shaders.Particles.Main", "SunShare", "Sunlight on particles", nullptr, "How much sunlight particles catch outdoors. Shaded ones get only ambient light and lamps." },
	{ "Shaders.Particles.Main", "SunPhase", "Smoke glows with the sun behind it", nullptr, "Smoke and dust glow when the sun is behind them and look darker from the sun's side, at the same average brightness. Zero is the old even look." },
	// GPUParticles (Main)
	{ "Shaders.GPUParticles.Main", "Enabled", "Rain and snow drawn on the graphics card (needs the DXVK fork)", nullptr, "Draws rain and snow as many more drops and flakes outdoors. Needs the DXVK fork." },
	{ "Shaders.GPUParticles.Main", nullptr, "Quality",
		"Low: Count=16384 Radius=1000 | Medium: Count=32768 Radius=1200 | High: Count=65536 Radius=1600",
		"How many drops or flakes fall and how far around you. More is denser and costs more." },
	{ "Shaders.GPUParticles.Main", "Size", "Drop and flake size", nullptr, "How big the drops, splashes and flakes are." },
	{ "Shaders.GPUParticles.Main", "Speed", "Fall speed", nullptr, "How fast the drops and flakes fall." },
	{ "Shaders.GPUParticles.Main", "Opacity", "Visibility", nullptr, "How strongly the drops and flakes show." },
	{ "Shaders.GPUParticles.Main", "Splashes", "Splashes where drops land", nullptr, "Rain drops make a small splash where they land; snow settles briefly." },
	// Tonemapping (Main)
	{ "Shaders.Tonemapping.Main", nullptr, "Tone mapper",
		"None (the game's own): TonemappingMode=0 | VT Lottes: TonemappingMode=1 | Lottes (tuned): TonemappingMode=2 | Reinhard: TonemappingMode=3 | Reinhard Jodie: TonemappingMode=4 | ACES Filmic: TonemappingMode=5 | ACES Fitted: TonemappingMode=6 | Uncharted 2: TonemappingMode=7 | Uchimura (GT): TonemappingMode=8 | AGX: TonemappingMode=9 | DICE (default): TonemappingMode=10 | Perceptual (Gran Turismo style): TonemappingMode=11",
		"How bright light is fitted onto your screen. DICE is neutral; ACES and Uncharted 2 are punchier and more filmic; None keeps the game's look." },
	{ "Shaders.Tonemapping.Main", "Exposure", "Brightness", nullptr, "Makes the whole picture brighter or darker." },
	{ "Shaders.Tonemapping.Main", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer, higher is more vivid." },
	{ "Shaders.Tonemapping.Main", "SkyMultiplier", "Sky brightness", nullptr, "Makes the sky and sun brighter or dimmer. Needs the sky effect on." },
	{ "Shaders.Tonemapping.Main", "NightShiftEnabled", "Night colour shift (dim areas turn pale and blue)", nullptr, "In dark places the picture loses colour and turns pale and blue, as your eyes do in the dark." },
	{ "Shaders.Tonemapping.Main", "NightShift", "Night colour shift amount", nullptr, "How strongly dark areas lose colour and turn blue. Used when the night colour shift is on." },
	// Tonemapping (Interiors)
	{ "Shaders.Tonemapping.Interiors", nullptr, "Tone mapper",
		"None (the game's own): TonemappingMode=0 | VT Lottes: TonemappingMode=1 | Lottes (tuned): TonemappingMode=2 | Reinhard: TonemappingMode=3 | Reinhard Jodie: TonemappingMode=4 | ACES Filmic: TonemappingMode=5 | ACES Fitted: TonemappingMode=6 | Uncharted 2: TonemappingMode=7 | Uchimura (GT): TonemappingMode=8 | AGX: TonemappingMode=9 | DICE (default): TonemappingMode=10 | Perceptual (Gran Turismo style): TonemappingMode=11",
		"How bright light is fitted onto your screen. DICE is neutral; ACES and Uncharted 2 are punchier and more filmic; None keeps the game's look." },
	{ "Shaders.Tonemapping.Interiors", "Exposure", "Brightness", nullptr, "Makes the whole picture brighter or darker." },
	{ "Shaders.Tonemapping.Interiors", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer, higher is more vivid." },
	{ "Shaders.Tonemapping.Interiors", "NightShift", "Night colour shift amount", nullptr, "How strongly dark areas lose colour and turn blue. Used when the night colour shift is on." },
	// Tonemapping (Night)
	{ "Shaders.Tonemapping.Night", "Exposure", "Brightness", nullptr, "Makes the whole picture brighter or darker." },
	{ "Shaders.Tonemapping.Night", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer, higher is more vivid." },
	{ "Shaders.Tonemapping.Night", "SkyMultiplier", "Sky brightness", nullptr, "Makes the sky and sun brighter or dimmer. Needs the sky effect on." },
	{ "Shaders.Tonemapping.Night", "NightShift", "Night colour shift amount", nullptr, "How strongly dark areas lose colour and turn blue. Used when the night colour shift is on." },
	// Exposure (Main)
	{ "Shaders.Exposure.Main", nullptr, "Auto exposure",
		"Classic: Mode=0 | Eye adaptation: Mode=1",
		"Classic: the older brightness correction. Eye adaptation: brightness adjusts like an eye, with automatic white balance. The rows below only affect eye adaptation." },
	{ "Shaders.Exposure.Main", "AdaptExposure", "Eye adaptation strength", nullptr, "How much the brightness adjusts as you move between light and dark. Only with eye adaptation." },
	{ "Shaders.Exposure.Main", "AdaptWhiteBalance", "Automatic white balance", nullptr, "Automatically removes warm or blue tints from the light, as your eyes do. Only with eye adaptation." },
	{ "Shaders.Exposure.Main", "DarkAdaptSpeed", "Speed of adjusting to dark", nullptr, "How fast the picture brightens when you go somewhere darker. Higher is faster." },
	{ "Shaders.Exposure.Main", "LightAdaptSpeed", "Speed of adjusting to bright", nullptr, "How fast the picture darkens when you go somewhere brighter. Higher is faster." },
	// Exposure (Interiors)
	{ "Shaders.Exposure.Interiors", "AdaptExposure", "Eye adaptation strength", nullptr, "How much the brightness adjusts as you move between light and dark. Only with eye adaptation." },
	{ "Shaders.Exposure.Interiors", "AdaptWhiteBalance", "Automatic white balance", nullptr, "Automatically removes warm or blue tints from the light, as your eyes do. Only with eye adaptation." },
	{ "Shaders.Exposure.Interiors", "DarkAdaptSpeed", "Speed of adjusting to dark", nullptr, "How fast the picture brightens when you go somewhere darker. Higher is faster." },
	{ "Shaders.Exposure.Interiors", "LightAdaptSpeed", "Speed of adjusting to bright", nullptr, "How fast the picture darkens when you go somewhere brighter. Higher is faster." },
	// Exposure (Night)
	{ "Shaders.Exposure.Night", "AdaptExposure", "Eye adaptation strength", nullptr, "How much the brightness adjusts as you move between light and dark. Only with eye adaptation." },
	{ "Shaders.Exposure.Night", "AdaptWhiteBalance", "Automatic white balance", nullptr, "Automatically removes warm or blue tints from the light, as your eyes do. Only with eye adaptation." },
	{ "Shaders.Exposure.Night", "DarkAdaptSpeed", "Speed of adjusting to dark", nullptr, "How fast the picture brightens when you go somewhere darker. Higher is faster." },
	{ "Shaders.Exposure.Night", "LightAdaptSpeed", "Speed of adjusting to bright", nullptr, "How fast the picture darkens when you go somewhere brighter. Higher is faster." },
	// LocalExposure (Main)
	{ "Shaders.LocalExposure.Main", "Shadows", "Lift dark areas", nullptr, "Brightens very dark parts of the picture so shaded areas show more detail." },
	{ "Shaders.LocalExposure.Main", "Highlights", "Calm bright areas", nullptr, "Tones down very bright parts, such as bright sand or a doorway to the outside." },
	{ "Shaders.LocalExposure.Main", "Blur", "Area size", nullptr, "How large the areas are that get evened out. Small reaches finer detail but can leave halos." },
	{ "Shaders.LocalExposure.Main", "Interiors", "Also indoors", nullptr, "Also applies the effect indoors." },
	{ "Shaders.LocalExposure.Main", "Exteriors", "Also outdoors", nullptr, "Also applies the effect outdoors." },
	// HDR (Main)
	{ "Shaders.HDR.Main", "Enabled", "HDR output", nullptr, "Sends a true HDR picture to your display, with brighter highlights. Needs an HDR screen with HDR on in Windows and the DXVK fork. Warning: currently causes freezes for some players; a fix is being researched." },
	{ "Shaders.HDR.Main", nullptr, "HDR format",
		"HDR10: Mode=1 | scRGB: Mode=2",
		"HDR10 is what most HDR games use. Try the other only if HDR10 looks wrong." },
	{ "Shaders.HDR.Main", "PaperWhiteNits", "Normal white brightness (nits)", nullptr, "How bright normal white, the HUD and menus are on an HDR screen. Match your Windows setting." },
	{ "Shaders.HDR.Main", "PeakNits", "Peak brightness (nits, 0 = automatic)", nullptr, "The brightest a highlight may get. Zero uses your display's own maximum." },
	{ "Shaders.HDR.Main", "HighlightStrength", "Highlight strength", nullptr, "How much brighter than normal white the highlights go. Zero gives a normal picture." },
	{ "Shaders.HDR.Main", "Gamma22", "Darker, deeper shadows (2.2 gamma)", nullptr, "Slightly darker, deeper shadows, like many standard monitors show." },
	// Bloom (Main)
	{ "Shaders.Bloom.Main", "Strength", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.Bloom.Main", nullptr, "Quality",
		"Low: Passes=4 | Medium: Passes=6 | High: Passes=8",
		"Higher gives a smoother glow and costs a little more." },
	{ "Shaders.Bloom.Main", "Spread", "Glow size", nullptr, "Makes glows wider and softer. Higher spreads light further around bright objects." },
	{ "Shaders.Bloom.Main", "GlowBoost", "Extra glow from bright lights", nullptr, "Makes bright lights such as neon signs, lamps and fire glow more than the rest of the picture." },
	// Bloom (Night)
	{ "Shaders.Bloom.Night", "Strength", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.Bloom.Night", nullptr, "Quality",
		"Low: Passes=4 | Medium: Passes=6 | High: Passes=8",
		"Higher gives a smoother glow and costs a little more." },
	{ "Shaders.Bloom.Night", "Spread", "Glow size", nullptr, "Makes glows wider and softer. Higher spreads light further around bright objects." },
	{ "Shaders.Bloom.Night", "GlowBoost", "Extra glow from bright lights", nullptr, "Makes bright lights such as neon signs, lamps and fire glow more than the rest of the picture." },
	// Bloom (Interiors)
	{ "Shaders.Bloom.Interiors", "Strength", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.Bloom.Interiors", nullptr, "Quality",
		"Low: Passes=4 | Medium: Passes=6 | High: Passes=8",
		"Higher gives a smoother glow and costs a little more." },
	{ "Shaders.Bloom.Interiors", "Spread", "Glow size", nullptr, "Makes glows wider and softer. Higher spreads light further around bright objects." },
	{ "Shaders.Bloom.Interiors", "GlowBoost", "Extra glow from bright lights", nullptr, "Makes bright lights such as neon signs, lamps and fire glow more than the rest of the picture." },
	// BloomLegacy (Exteriors)
	{ "Shaders.BloomLegacy.Exteriors", "BloomIntensity", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.BloomLegacy.Exteriors", "BloomSaturation", "Glow colour strength", nullptr, "How colourful the glow is." },
	{ "Shaders.BloomLegacy.Exteriors", "OriginalIntensity", "Picture brightness", nullptr, "Brightness of the picture itself under the glow." },
	{ "Shaders.BloomLegacy.Exteriors", "OriginalSaturation", "Picture colour strength", nullptr, "Colour strength of the picture itself under the glow." },
	// BloomLegacy (Interiors)
	{ "Shaders.BloomLegacy.Interiors", "BloomIntensity", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.BloomLegacy.Interiors", "BloomSaturation", "Glow colour strength", nullptr, "How colourful the glow is." },
	{ "Shaders.BloomLegacy.Interiors", "OriginalIntensity", "Picture brightness", nullptr, "Brightness of the picture itself under the glow." },
	{ "Shaders.BloomLegacy.Interiors", "OriginalSaturation", "Picture colour strength", nullptr, "Colour strength of the picture itself under the glow." },
	// Lens (Main)
	{ "Shaders.Lens.Main", "Strength", "Lens dirt strength", nullptr, "How visible the dirt on the camera lens is when you look at bright lights." },
	{ "Shaders.Lens.Main", "Smudginess", "Dirt coverage", nullptr, "How much of the screen the lens dirt covers." },
	// Lens (Night)
	{ "Shaders.Lens.Night", "Strength", "Lens dirt strength", nullptr, "How visible the dirt on the camera lens is when you look at bright lights." },
	{ "Shaders.Lens.Night", "Smudginess", "Dirt coverage", nullptr, "How much of the screen the lens dirt covers." },
	// Lens (Interiors)
	{ "Shaders.Lens.Interiors", "Strength", "Lens dirt strength", nullptr, "How visible the dirt on the camera lens is when you look at bright lights." },
	{ "Shaders.Lens.Interiors", "Smudginess", "Dirt coverage", nullptr, "How much of the screen the lens dirt covers." },
	// LUT (Main)
	{ "Shaders.LUT.Main", "Strength", "Colour grade strength", nullptr, "How strongly the chosen colour grade is applied. Zero is off." },
	// Coloring (Default)
	{ "Shaders.Coloring.Default", "Strength", "Strength", nullptr, "How strongly the whole colour look is applied." },
	{ "Shaders.Coloring.Default", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.Coloring.Default", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.Coloring.Default", "Bleach", "Bleached look", nullptr, "A washed-out, desaturated film look with harder contrast." },
	{ "Shaders.Coloring.Default", "Fade", "Faded look", nullptr, "Lifts the blacks for a faded, hazy look." },
	// ImageAdjust (Main)
	{ "Shaders.ImageAdjust.Main", "Strength", "Strength", nullptr, "How strongly these picture adjustments are applied." },
	{ "Shaders.ImageAdjust.Main", "Brightness", "Brightness", nullptr, "Makes the picture lighter or darker." },
	{ "Shaders.ImageAdjust.Main", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.ImageAdjust.Main", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.ImageAdjust.Main", "DarkColorR", "Shadow colour", nullptr, "Tints the dark parts of the picture." },
	{ "Shaders.ImageAdjust.Main", "LightColorR", "Highlight colour", nullptr, "Tints the bright parts of the picture." },
	// ImageAdjust (Night)
	{ "Shaders.ImageAdjust.Night", "Strength", "Strength", nullptr, "How strongly these picture adjustments are applied." },
	{ "Shaders.ImageAdjust.Night", "Brightness", "Brightness", nullptr, "Makes the picture lighter or darker." },
	{ "Shaders.ImageAdjust.Night", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.ImageAdjust.Night", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.ImageAdjust.Night", "DarkColorR", "Shadow colour", nullptr, "Tints the dark parts of the picture." },
	{ "Shaders.ImageAdjust.Night", "LightColorR", "Highlight colour", nullptr, "Tints the bright parts of the picture." },
	// ImageAdjust (Interiors)
	{ "Shaders.ImageAdjust.Interiors", "Strength", "Strength", nullptr, "How strongly these picture adjustments are applied." },
	{ "Shaders.ImageAdjust.Interiors", "Brightness", "Brightness", nullptr, "Makes the picture lighter or darker." },
	{ "Shaders.ImageAdjust.Interiors", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.ImageAdjust.Interiors", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.ImageAdjust.Interiors", "DarkColorR", "Shadow colour", nullptr, "Tints the dark parts of the picture." },
	{ "Shaders.ImageAdjust.Interiors", "LightColorR", "Highlight colour", nullptr, "Tints the bright parts of the picture." },
	// Cinema (Main)
	{ "Shaders.Cinema.Main", "AspectRatio", "Black bars (screen shape)", nullptr, "Adds black cinema bars at the top and bottom. Use 1 for no bars." },
	{ "Shaders.Cinema.Main", "FilmGrainAmount", "Film grain", nullptr, "Adds moving film grain, like a cinema picture." },
	{ "Shaders.Cinema.Main", "ChromaticAberration", "Colour fringes at the edges", nullptr, "Splits red and blue slightly towards the edges, like a real lens." },
	{ "Shaders.Cinema.Main", "VignetteDarkness", "Dark corners", nullptr, "Darkens the corners and edges of the screen." },
	{ "Shaders.Cinema.Main", "OverlayStrength", "Contrast and colour boost", nullptr, "Boosts colour and contrast for a richer, more filmic picture." },
	// Bodycam (Main)
	{ "Shaders.Bodycam.Main", "Fisheye", "Lens distortion", nullptr, "How much the wide lens bends straight lines outwards at the edges." },
	{ "Shaders.Bodycam.Main", "Vignette", "Dark corners", nullptr, "Darkens the corners of the picture." },
	{ "Shaders.Bodycam.Main", "Grain", "Camera noise", nullptr, "Adds camera noise, stronger in the dark." },
	{ "Shaders.Bodycam.Main", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is more washed out." },
	{ "Shaders.Bodycam.Main", "Text", "Timestamp", nullptr, "Shows a date and time stamp in the top right corner." },
	{ "Shaders.Bodycam.Main", "Bounce", "Camera bob", nullptr, "The picture bobs with each step and leans into turns, like a chest-mounted camera." },
	{ "Shaders.Bodycam.Main", "MotionBlur", "Motion smear", nullptr, "Smears the picture while you turn." },
	{ "Shaders.Bodycam.Main", "Mic", "Camera microphone sound", nullptr, "Makes all game sound like a cheap camera microphone. Costs no frame rate." },
	// Dream (Main): the "fun" trip look (Extras group)
	{ "Shaders.Dream.Main", "Level", "Dream level", nullptr, "How deep the dream is. Everything below grows with it." },
	{ "Shaders.Dream.Main", "Breathing", "Breathing", nullptr, "The dream level rises and falls slowly by itself." },
	{ "Shaders.Dream.Main", "Tracers", "Tracers", nullptr, "Anything that moves leaves trails behind it." },
	{ "Shaders.Dream.Main", "Tunnel", "Stare tunnel", nullptr, "Hold the view still for a few seconds and it starts falling into itself. Move to leave it." },
	{ "Shaders.Dream.Main", "Melt", "Fractal melts", nullptr, "Now and then the whole scene slides into a fractal for a few seconds." },
	{ "Shaders.Dream.Main", "LowHealth", "Deeper when hurt", nullptr, "Low health pushes the dream deeper." },
	// Bodycam (Night)
	{ "Shaders.Bodycam.Night", "Fisheye", "Lens distortion", nullptr, "How much the wide lens bends straight lines outwards at the edges." },
	{ "Shaders.Bodycam.Night", "Vignette", "Dark corners", nullptr, "Darkens the corners of the picture." },
	{ "Shaders.Bodycam.Night", "Grain", "Camera noise", nullptr, "Adds camera noise, stronger in the dark." },
	{ "Shaders.Bodycam.Night", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is more washed out." },
	{ "Shaders.Bodycam.Night", "Text", "Timestamp", nullptr, "Shows a date and time stamp in the top right corner." },
	{ "Shaders.Bodycam.Night", "Bounce", "Camera bob", nullptr, "The picture bobs with each step and leans into turns, like a chest-mounted camera." },
	{ "Shaders.Bodycam.Night", "LowLight", "Low-light mode (black and white)", nullptr, "Near black and white night-vision look with lifted shadows and more grain." },
	{ "Shaders.Bodycam.Night", "MotionBlur", "Motion smear", nullptr, "Smears the picture while you turn." },
	// Bodycam (Interiors)
	{ "Shaders.Bodycam.Interiors", "Fisheye", "Lens distortion", nullptr, "How much the wide lens bends straight lines outwards at the edges." },
	{ "Shaders.Bodycam.Interiors", "Vignette", "Dark corners", nullptr, "Darkens the corners of the picture." },
	{ "Shaders.Bodycam.Interiors", "Grain", "Camera noise", nullptr, "Adds camera noise, stronger in the dark." },
	{ "Shaders.Bodycam.Interiors", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is more washed out." },
	{ "Shaders.Bodycam.Interiors", "Text", "Timestamp", nullptr, "Shows a date and time stamp in the top right corner." },
	{ "Shaders.Bodycam.Interiors", "Bounce", "Camera bob", nullptr, "The picture bobs with each step and leans into turns, like a chest-mounted camera." },
	{ "Shaders.Bodycam.Interiors", "LowLight", "Low-light mode (black and white)", nullptr, "Near black and white night-vision look with lifted shadows and more grain." },
	{ "Shaders.Bodycam.Interiors", "MotionBlur", "Motion smear", nullptr, "Smears the picture while you turn." },
	// DepthOfField (FirstPersonView)
	{ "Shaders.DepthOfField.FirstPersonView", "Enabled", "Depth of field (background blur)", nullptr, "Blurs things in front of and behind what you look at." },
	{ "Shaders.DepthOfField.FirstPersonView", "BaseBlurRadius", "Blur strength", nullptr, "How strong the blur is." },
	{ "Shaders.DepthOfField.FirstPersonView", "Radius", "Blur size", nullptr, "How large the blur is." },
	{ "Shaders.DepthOfField.FirstPersonView", "DistantBlur", "Distant blur", nullptr, "Softens the far distance and mountain horizons to hide LOD edges." },
	{ "Shaders.DepthOfField.FirstPersonView", "DistantBlurStrength", "Distant blur strength", nullptr, "How strong the distant blur is (0 = off, 1 = default)." },
	{ "Shaders.DepthOfField.FirstPersonView", nullptr, "When it applies",
		"Always: Mode=0 | Not in dialogue: Mode=1 | Only in dialogue: Mode=2",
		"Whether the blur also applies while you talk to someone." },
	// DepthOfField (ThirdPersonView)
	{ "Shaders.DepthOfField.ThirdPersonView", "Enabled", "Depth of field (background blur)", nullptr, "Blurs things in front of and behind what you look at." },
	{ "Shaders.DepthOfField.ThirdPersonView", "BaseBlurRadius", "Blur strength", nullptr, "How strong the blur is." },
	{ "Shaders.DepthOfField.ThirdPersonView", "Radius", "Blur size", nullptr, "How large the blur is." },
	{ "Shaders.DepthOfField.ThirdPersonView", "DistantBlur", "Distant blur", nullptr, "Softens the far distance and mountain horizons to hide LOD edges." },
	{ "Shaders.DepthOfField.ThirdPersonView", "DistantBlurStrength", "Distant blur strength", nullptr, "How strong the distant blur is (0 = off, 1 = default)." },
	{ "Shaders.DepthOfField.ThirdPersonView", nullptr, "When it applies",
		"Always: Mode=0 | Not in dialogue: Mode=1 | Only in dialogue: Mode=2",
		"Whether the blur also applies while you talk to someone." },
	// DepthOfField (VanityView)
	{ "Shaders.DepthOfField.VanityView", "Enabled", "Depth of field (background blur)", nullptr, "Blurs things in front of and behind what you look at." },
	{ "Shaders.DepthOfField.VanityView", "BaseBlurRadius", "Blur strength", nullptr, "How strong the blur is." },
	{ "Shaders.DepthOfField.VanityView", "Radius", "Blur size", nullptr, "How large the blur is." },
	{ "Shaders.DepthOfField.VanityView", "DistantBlur", "Distant blur", nullptr, "Softens the far distance and mountain horizons to hide LOD edges." },
	{ "Shaders.DepthOfField.VanityView", "DistantBlurStrength", "Distant blur strength", nullptr, "How strong the distant blur is (0 = off, 1 = default)." },
	{ "Shaders.DepthOfField.VanityView", nullptr, "When it applies",
		"Always: Mode=0 | Not in dialogue: Mode=1 | Only in dialogue: Mode=2",
		"Whether the blur also applies while you talk to someone." },
	// DepthOfField (Bokeh)
	{ "Shaders.DepthOfField.Bokeh", "Bokeh", "Bokeh (bright lights blur into shapes)", nullptr, "Bright out-of-focus lights become soft shapes. Needs depth of field on and the DXVK fork." },
	{ "Shaders.DepthOfField.Bokeh", nullptr, "Bokeh shape",
		"Round: BokehShape=0 | Pentagon: BokehShape=5 | Hexagon: BokehShape=6 | Heptagon: BokehShape=7 | Octagon: BokehShape=8",
		"The shape of the blurred bright lights, like a camera lens." },
	{ "Shaders.DepthOfField.Bokeh", "BokehSize", "Bokeh size", nullptr, "How large the blurred light shapes are." },
	{ "Shaders.DepthOfField.Bokeh", "BokehIntensity", "Bokeh brightness", nullptr, "How bright the blurred light shapes are." },
	// MotionBlur (FirstPersonView)
	{ "Shaders.MotionBlur.FirstPersonView", "Enabled", "Motion blur", nullptr, "Blurs the picture while you turn or move quickly." },
	{ "Shaders.MotionBlur.FirstPersonView", "BlurScale", "Motion blur strength", nullptr, "How strong the motion blur is." },
	// MotionBlur (ThirdPersonView)
	{ "Shaders.MotionBlur.ThirdPersonView", "Enabled", "Motion blur", nullptr, "Blurs the picture while you turn or move quickly." },
	{ "Shaders.MotionBlur.ThirdPersonView", "BlurScale", "Motion blur strength", nullptr, "How strong the motion blur is." },
	// LowHF (Main)
	{ "Shaders.LowHF.Main", "BlurMultiplier", "Blur when hurt", nullptr, "How blurry the picture gets when you are badly hurt." },
	{ "Shaders.LowHF.Main", "DarknessMultiplier", "Darkening when hurt", nullptr, "How dark the picture gets when you are badly hurt." },
	{ "Shaders.LowHF.Main", "VignetteMultiplier", "Dark edges when hurt", nullptr, "How dark the screen edges get when you are badly hurt." },
	{ "Shaders.LowHF.Main", "HealthLimit", "Health level where it starts", nullptr, "The health level below which the hurt effect begins." },
	// TAA (Main)
	{ "Shaders.TAA.Main", "Sharpness", "Sharpness", nullptr, "Sharpens the picture to counter the softness anti-aliasing adds." },
	{ "Shaders.TAA.Main", "HistoryWeight", "Smoothness over time", nullptr, "Higher is smoother with less shimmer, but can leave slight ghosting on moving things." },
	{ "Shaders.TAA.Main", "WeaponTAA", "Smoothing on the weapon", nullptr, "How much anti-aliasing smoothing applies to your weapon and hands." },
	// TAA (Cutouts)
	{ "Shaders.TAA.Cutouts", "Enabled", "Smooth edges on leaves, grass and fences", nullptr, "Smooths the jagged edges of leaves, grass and fences. Needs a game restart." },
	// TAA (FSR)
	{ "Shaders.TAA.FSR", "Enabled", "FSR upscaler", nullptr, "AMD FSR upscaling and anti-aliasing. Needs the DXVK fork." },
	{ "Shaders.TAA.FSR", "RenderScale", "Upscaling quality", nullptr, "Draws the game at a lower resolution and scales it up. Lower runs faster but looks softer." },
	{ "Shaders.TAA.FSR", "Sharpness", "Sharpness", nullptr, "Sharpens the upscaled picture. Zero is off." },
	// TAA (DLSS)
	{ "Shaders.TAA.DLSS", "Enabled", "DLSS (NVIDIA RTX only)", nullptr, "NVIDIA DLSS upscaling and anti-aliasing. NVIDIA RTX cards only, needs the DXVK fork." },
	{ "Shaders.TAA.DLSS", "RenderScale", "Upscaling quality", nullptr, "Draws the game at a lower resolution and scales it up. Lower runs faster but looks softer." },
	{ "Shaders.TAA.DLSS", "RayReconstruction", "DLSS Ray Reconstruction", nullptr, "NVIDIA's cleaner denoising for ray tracing. NVIDIA RTX cards only, with DLSS on." },
	// UNOFFICIAL 2026-11-15 (docs/next/DLSS-RR-MODEL.md) RRPreset 3 / 4: NVIDIA's newer Ray Reconstruction model (preset F)
	{ "Shaders.TAA.DLSS", nullptr, "Ray Reconstruction model",
		"NVIDIA's choice (as before): RRPreset=0 | Current model: RRPreset=2 | Newer model (DLSS 4.5): RRPreset=3 | Pick by graphics card: RRPreset=4",
		"Newer model: more detail and reacts faster, but can shimmer more. About the same speed on RTX 40 and 50 cards; on RTX 20 and 30 it "
		"can cost up to a quarter of the frame rate. Pick by graphics card uses the newer model on RTX 40 / 50 and the current one on "
		"RTX 20 / 30. Needs an NVIDIA driver from version 580. NVIDIA's choice is already the newer model on drivers from 580." },
	{ "Shaders.TAA.DLSS", "WeaponMask", "Weapon ghosting fix", nullptr, "Reduces ghosting and smearing on your weapon and hands with DLSS." },
	// SMAA (Main)
	{ "Shaders.SMAA.Main", nullptr, "Quality",
		"Low: MaxSearchSteps=16 | Medium: MaxSearchSteps=32 | High: MaxSearchSteps=64",
		"Higher smooths long, shallow edges better and costs a little more." },
	{ "Shaders.SMAA.Main", "Threshold", "Edge sensitivity", nullptr, "How easily edges are found. Lower smooths more of the picture but can soften detail." },
	// Sharpening (Main)
	{ "Shaders.Sharpening.Main", "Strength", "Sharpening strength", nullptr, "How much the picture is sharpened." },
	{ "Shaders.Sharpening.Main", "Offset", "Sharpening width", nullptr, "How wide the sharpening reaches. Wider looks coarser." },
	// Sharpening (Interiors)
	{ "Shaders.Sharpening.Interiors", "Strength", "Sharpening strength", nullptr, "How much the picture is sharpened." },
	{ "Shaders.Sharpening.Interiors", "Offset", "Sharpening width", nullptr, "How wide the sharpening reaches. Wider looks coarser." },
	// DitherBuster (Main)
	{ "Shaders.DitherBuster.Main", "Strength", "Strength", nullptr, "How strongly the dotted patterns on edges are smoothed out." },
	{ "Shaders.DitherBuster.Main", "Radius", "Smoothing reach", nullptr, "How far along edges the smoothing reaches." },
	// SkyDither (Main)
	{ "Shaders.SkyDither.Main", "Strength", "Banding removal strength", nullptr, "Hides visible bands in smooth skies with faint moving noise." },
	{ "Shaders.SkyDither.Main", "AtToneMap", "Also fix banding over the whole picture", nullptr, "Also removes banding in fog and dark gradients across the whole picture." },
	// Flashlight (Main)
	{ "Shaders.Flashlight.Main", "Dimmer", "Brightness", nullptr, "How bright the beam is. Its colour follows your Pip-Boy colour." },
	{ "Shaders.Flashlight.Main", "Distance", "Beam distance", nullptr, "How far the beam reaches. It fades out well before the end." },
	{ "Shaders.Flashlight.Main", "AttachToWeapon", "Follows your weapon", nullptr, "The beam points where your weapon points instead of where the camera looks." },
	// Flashlight (Deferred)
	{ "Shaders.Flashlight.Deferred", "Shadows", "Flashlight shadows", nullptr, "Things in front of the beam cast shadows. Costs some frame rate while the flashlight is on." },
	{ "Shaders.Flashlight.Deferred", nullptr, "Shadow quality",
		"Low: ShadowMapSize=1024 ShadowSoftness=3.0 | Medium: ShadowMapSize=2048 ShadowSoftness=5.0 | High: ShadowMapSize=4096 ShadowSoftness=8.0",
		"Higher gives sharper, softer shadow edges and uses more video memory. Needs a restart." },
	{ "Shaders.Flashlight.Deferred", "Falloff", "Beam fade with distance", nullptr, "Higher makes the beam fade out sooner and look shorter. Lower reaches further." },
	{ "Shaders.Flashlight.Deferred", "Cookie", "Lens pattern", nullptr, "Shows the flashlight lens pattern in the beam. Zero gives a clean, even beam." },
	{ "Shaders.Flashlight.Deferred", "GameLight", "Lights the world like a lamp", nullptr, "Leaves, your hands and shiny surfaces react to the flashlight the way they do to a lamp at night." },
	// Grass (Interaction)
	{ "Shaders.Grass.Interaction", "Enabled", "Grass bends as you walk", nullptr, "Grass pushes away from you as you walk through it and springs back afterwards." },
	{ "Shaders.Grass.Interaction", "Radius", "Bend distance", nullptr, "How far around your feet the grass bends." },
	{ "Shaders.Grass.Interaction", "StrengthStanding", "Bend strength (standing)", nullptr, "How much the grass is pushed aside while you stand still." },
	{ "Shaders.Grass.Interaction", "StrengthMax", "Bend strength (running)", nullptr, "How much the grass is pushed aside when you run." },
	{ "Shaders.Grass.Interaction", "SpringBackSeconds", "Recovery time", nullptr, "How slowly the grass stands back up after you pass. Higher leaves trails for longer." },
	// Grass (LOD)
	{ "Shaders.Grass.LOD", "Enabled", "Fade small grass by size", nullptr, "Grass fades out by how small it looks on screen, not just by distance. Saves frame rate in open fields." },
	{ "Shaders.Grass.LOD", "MinPixels", "Smallest grass kept", nullptr, "Grass smaller than this on screen disappears. Higher is faster, lower keeps distant grass longer." },
	{ "Shaders.Grass.LOD", "FadePixels", "Fade-out softness", nullptr, "How gradually small grass fades away. Higher avoids grass popping out." },
	// Grass (Lights)
	{ "Shaders.Grass.Lights", "Enabled", "Lamps light the grass", nullptr, "Lamps and your Pip-Boy light shine on nearby grass. Costs nothing where no lamp is near." },
	{ "Shaders.Grass.Lights", "MaxLights", "Lamps at once", nullptr, "The most lamps that can light the grass at the same time, nearest first." },
	{ "Shaders.Grass.Lights", "Strength", "Lamp light strength", nullptr, "How bright lamp light looks on the grass." },
	// Grass (NormalMaps)
	{ "Shaders.Grass.NormalMaps", "Enabled", "Grass surface detail", nullptr, "Grass textures that come with a detail map catch the sunlight with fine bumps." },
	{ "Shaders.Grass.NormalMaps", "Strength", "Detail strength", nullptr, "How strongly the fine bumps change the light on the grass." },
	{ "Shaders.Grass.NormalMaps", "EnableSpecular", "Sun shine on grass", nullptr, "Adds a sun highlight on grass and grass-placed rocks." },
	{ "Shaders.Grass.NormalMaps", "SpecularStrength", "Shine strength", nullptr, "How bright the sun highlight is." },
	{ "Shaders.Grass.NormalMaps", "Glossiness", "Shine sharpness", nullptr, "Low gives a broad soft sheen, high gives small sharp glints." },
	// Wetness (Main)
	{ "Shaders.WetnessEffects.Main", "MaxRainWetness", "Rain wetness", nullptr, "How wet and glossy surfaces under the open sky get in the rain." },
	{ "Shaders.WetnessEffects.Main", "MaxPuddleWetness", "Puddle amount", nullptr, "How much puddles fill up on flat ground." },
	{ "Shaders.WetnessEffects.Main", "PuddleRadius", "Puddle size", nullptr, "How big the puddles are." },
	{ "Shaders.WetnessEffects.Main", "MinRainWetness", "Wetness in sheltered spots", nullptr, "How wet walls and undersides get, where rain does not fall straight on." },
	{ "Shaders.WetnessEffects.Main", "WeatherTransitionSpeed", "Wet and dry speed", nullptr, "How fast things get wet when rain starts and dry when it stops." },
	{ "Shaders.WetnessEffects.Main", "CreviceFill", "Ground flooding", nullptr, "Terrain only. Higher lets standing water rise further in the cracks and flood more ground." },
	// Wetness (Raindrops)
	{ "Shaders.WetnessEffects.Raindrops", "EnableRaindropFx", "Raindrops on the ground", nullptr, "Drops land on open ground near you while it rains. Terrain only." },
	{ "Shaders.WetnessEffects.Raindrops", "EnableRipples", "Rain ripples", nullptr, "Rings spread out where drops land on puddles and wet flat ground." },
	{ "Shaders.WetnessEffects.Raindrops", "RippleStrength", "Ripple strength", nullptr, "How strongly the ripples distort the water." },
	{ "Shaders.WetnessEffects.Raindrops", "RaindropChance", "Raindrop amount", nullptr, "How many drops land in heavy rain. Lower looks lighter." },
	{ "Shaders.WetnessEffects.Raindrops", "RaindropFxRange", "Raindrop distance", nullptr, "How far from you the drops and ripples are drawn." },
	{ "Shaders.WetnessEffects.Raindrops", "RippleLifetime", "Ripple time", nullptr, "How long each ripple lasts, in seconds." },
	// Wetness (HudRaindrops)
	{ "Shaders.WetnessEffects.HudRaindrops", "Enabled", "Rain drops on arms and weapon", nullptr, "Water drops gather on your hands and weapon in the rain. They catch the light and bend what is under them." },
	{ "Shaders.WetnessEffects.HudRaindrops", "AllObjects", "Rain drops on everything", nullptr, "Drops also appear on buildings, props and people out in the rain. Sheltered things stay dry." },
	{ "Shaders.WetnessEffects.HudRaindrops", "Density", "Drop amount", nullptr, "How many drops gather once the surface is fully wet." },
	{ "Shaders.WetnessEffects.HudRaindrops", "DropSize", "Drop size", nullptr, "Bigger gives larger drops." },
	{ "Shaders.WetnessEffects.HudRaindrops", "Darkness", "Wet darkening", nullptr, "How much each drop darkens the surface under it, as if soaking in." },
	{ "Shaders.WetnessEffects.HudRaindrops", "WetTime", "Time to get wet or dry", nullptr, "Seconds of heavy rain for a surface to wet through, and to dry off again." },
	// Wetness (Reflections)
	{ "Shaders.WetnessEffects.Reflections", "Enabled", "Wet reflections", nullptr, "Wet ground and objects reflect what is on screen, not just the sky. Needs Wetness Effects on." },
	{ "Shaders.WetnessEffects.Reflections", "Strength", "Reflection strength", nullptr, "How much of the reflection shows the scene instead of the sky." },
	{ "Shaders.WetnessEffects.Reflections", nullptr, "Reflection quality",
		"Low: Steps=12 MaxDistance=800.0 | Medium: Steps=24 MaxDistance=1500.0 | High: Steps=40 MaxDistance=2500.0",
		"Higher finds more distant things and looks finer, but costs more frame rate." },
	{ "Shaders.WetnessEffects.Reflections", "MaxCameraDistance", "Reflection range", nullptr, "Wet surfaces farther away than this show no scene reflections." },
	// First-person gear weather (Main.Main.Quality)
	{ "Main.Main.Quality", "DeferredFirstPerson", "Deferred 1st-person viewmodel", nullptr, "Routes 1st-person hands and weapons through the deferred renderer (required for First-person gear weather; restart)." },
	{ "Main.Main.Quality", "GearWeather", "Gear weather enabled", nullptr, "Accumulates snow/frost, animated melting snowflakes, sandstorm dust/grit, rain wetness, and mud on your 1st-person weapon, hands, and arms." },
	{ "Main.Main.Quality", "GearWeatherStrength", "Strength", nullptr, "Overall strength of all first-person gear weather effects." },
	{ "Main.Main.Quality", "GearWeatherFrost", "Frost & snowflakes", nullptr, "Strength of cold metal frost rime and animated melting snowflakes in snow." },
	{ "Main.Main.Quality", "GearWeatherSand", "Sandstorm dust & grit", nullptr, "Strength of sandstorm dust and fine sand particles across weapon, hands, and sleeves." },
	{ "Main.Main.Quality", "GearWeatherWet", "Rain wetness", nullptr, "Strength of the rain wetness film on arms and weapons." },
	{ "Main.Main.Quality", "GearWeatherMud", "Mud splash", nullptr, "Strength of mud splashed while moving over wet ground." },
	{ "Main.Main.Quality", "GearWeatherSpeed", "Buildup / fade speed", nullptr, "How fast weather effects build up outdoors and melt, dry, or wear off." },
	{ "Main.Main.Quality", "GearWeatherSkin", "Bare skin share", nullptr, "How strongly frost, snowflakes, sand dust, and mud cling to bare skin." },
	{ "Main.Main.Quality", "GearWeatherScale", "Pattern scale", nullptr, "Blotch size in game units for broad frost, sand, and mud layers." },
	// World surface detail & weathering (Shaders.WorldSurface.*)
	{ "Shaders.WorldSurface.Main", "Strength", "Overall strength", nullptr, "Overall multiplier for all World surface detail & weathering layers." },
	{ "Shaders.WorldSurface.Main", "Interiors", "Enabled in interiors", nullptr, "Also apply World surface detail and weathering inside interior cells (off by default to save cost indoors)." },
	{ "Shaders.WorldSurface.Main", "Detail", "Micro-detail grain", nullptr, "High-frequency surface bump and roughness variation on close-up concrete, stone, metal, and terrain." },
	{ "Shaders.WorldSurface.Main", "Distance", "Fade distance", nullptr, "Distance from the camera (game units) at which world surface overlays fade out." },
	{ "Shaders.WorldSurface.Dust", "Enabled", "Enabled", nullptr, "Enables settled Mojave silt outdoors and aged crevice dust indoors." },
	{ "Shaders.WorldSurface.Dust", "Strength", "Dust strength", nullptr, "Settled Mojave silt outdoors and aged crevice dust indoors in seams and on upward ledges." },
	{ "Shaders.WorldSurface.Rust", "Enabled", "Enabled", nullptr, "Enables exposed metal tarnish patina and patchy iron-oxide rust." },
	{ "Shaders.WorldSurface.Rust", "Strength", "Rust strength", nullptr, "Patchy oxidation and matte iron-oxide rust along crevices and exposed faces of metallic objects." },
	{ "Shaders.WorldSurface.Waterline", "Enabled", "Enabled", nullptr, "Enables capillary water darkening, shoreline mud, and damp algae just above water level." },
	{ "Shaders.WorldSurface.Waterline", "Strength", "Waterline strength", nullptr, "Dark capillary water wicking, glossy wet rims, mud, and algae just above water level." },
	{ "Shaders.WorldSurface.Moss", "Enabled", "Enabled", nullptr, "Enables patchy green moss and lichen on shaded rocks, ruins, and damp stone outdoors." },
	{ "Shaders.WorldSurface.Moss", "Strength", "Moss strength", nullptr, "Patchy green moss and lichen on shaded/upward rocks, ruins, and damp stone outdoors." },
};

static const char* SimpleTip(const char* section, const char* key) {
	for (const SimpleRow& r : kSimpleRows) if (r.Key && !strcmp(section, r.Section) && !strcmp(key, r.Key)) return r.Tip;
	return nullptr;
}

static bool HasSimpleView(const std::string& section) {
	for (const SimpleRow& r : kSimpleRows) if (section == r.Section) return true;
	return false;
}

static const char* SimpleLabel(const char* section, const char* key) {
	for (const SimpleRow& r : kSimpleRows) if (r.Key && !strcmp(section, r.Section) && !strcmp(key, r.Key)) return r.Label;
	return nullptr;
}

struct SimpleOption { std::string Name; std::vector<std::pair<std::string, std::string>> Values; };

static std::vector<SimpleOption> ParseSimpleOptions(const char* text) {
	std::vector<SimpleOption> options;
	std::stringstream all(text);
	std::string part;
	while (std::getline(all, part, '|')) {
		const size_t colon = part.find(':');
		if (colon == std::string::npos) continue;
		SimpleOption o;
		o.Name = part.substr(0, colon);
		o.Name.erase(0, o.Name.find_first_not_of(' '));
		o.Name.erase(o.Name.find_last_not_of(' ') + 1);
		std::stringstream values(part.substr(colon + 1));
		std::string kv;
		while (values >> kv) {
			const size_t eq = kv.find('=');
			if (eq != std::string::npos) o.Values.push_back({ kv.substr(0, eq), kv.substr(eq + 1) });
		}
		options.push_back(o);
	}
	return options;
}

static bool SimpleValueIs(const char* section, const std::string& key, const std::string& value) {
	if (value == "true" || value == "false") return (TheSettingManager->GetSettingI(section, key.c_str()) != 0) == (value == "true");
	if (value.find('.') != std::string::npos) return fabsf(TheSettingManager->GetSettingF(section, key.c_str()) - (float)atof(value.c_str())) < 1e-3f;
	return TheSettingManager->GetSettingI(section, key.c_str()) == atoi(value.c_str());
}

// A choice row: picking an option writes all its settings; "Custom" while they match none of the options.
static void DrawSimpleChoice(const SimpleRow& r) {
	const std::vector<SimpleOption> options = ParseSimpleOptions(r.Options);
	int current = -1;
	for (int i = 0; i < (int)options.size() && current < 0; i++) {
		bool all = true;
		for (const auto& kv : options[i].Values) all = all && SimpleValueIs(r.Section, kv.first, kv.second);
		if (all) current = i;
	}
	// Check member keys for any restart requirement or inactive state
	RestartNote restart;
	const char* why = nullptr;
	if (!options.empty()) {
		for (const auto& kv : options[0].Values) {
			RestartNote n;
			if (FindRestartNote(r.Section, kv.first.c_str(), n) && (!restart.text || n.pending)) restart = n;
			if (!why) why = InactiveReason(r.Section, kv.first.c_str());
		}
	}
	bool unsaved = false;	// 2026-11-25 (unsaved marks): one of the keys the options write
	if (!options.empty())
		for (const auto& kv : options[0].Values) unsaved = unsaved || UnsavedSettings::Key(r.Section, kv.first.c_str());
	ImGui::PushID(r.Label);
	if (why) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	const float ctrlW = MenuWidgets::RowBegin(r.Label, MenuWidgets::ResetSlotWidth() + RestartNoteWidth(restart), unsaved);
	ImGui::SetNextItemWidth(ctrlW);
	if (ImGui::BeginCombo("##choice", current >= 0 ? options[current].Name.c_str() : "Custom (set in Advanced)")) {
		for (int i = 0; i < (int)options.size(); i++) {
			if (!ImGui::Selectable(options[i].Name.c_str(), i == current)) continue;
			std::vector<std::array<std::string, 3>> sets;
			for (const auto& kv : options[i].Values) sets.push_back({ r.Section, kv.first, kv.second });
			if (!ConfirmChanges(sets)) continue;	// 2026-11-05: a conflict: the Heads up box first
			for (const auto& kv : options[i].Values) TheSettingManager->SetSettingS(r.Section, kv.first.c_str(), kv.second.c_str());
			TheSettingManager->LoadSettings();
		}
		ImGui::EndCombo();
	}
	const bool hovered = ImGui::IsItemHovered();
	if (why) ImGui::PopStyleColor();
	if (restart.text) {
		ImGui::SameLine();
		DrawRestartNote(restart);
	}
	if (hovered || MenuWidgets::RowLabelHovered()) {
		std::string tip = r.Tip ? r.Tip : "";
		if (why) {
			if (!tip.empty()) tip += "\n\nNot active now: ";
			tip += why;
		}
		if (!tip.empty()) WrappedTooltip(tip.c_str());
	}
	ImGui::PopID();
}

static const char* MenuKeyLabel(const char* section, const char* key) {
	if (!AdvancedShown())
		if (const char* plain = SimpleLabel(section, key)) return plain;	// 2026-11-05: the simple view's name
	if (!strcmp(section, "Main.Main.Performance") && !strcmp(key, "NvrDxvkDisplacement")) return "NvrDxvkDisplacement (tessellation)##NvrDxvkDisplacement";
	return key;
}

// UNOFFICIAL 2026-10-27 (user: "the render scale on dlss and fsr should mimic modern games and just have a few options to select"):
// [Shaders.TAA.FSR] / [Shaders.TAA.DLSS] RenderScale as the usual quality modes (AMD's and NVIDIA's ratios: 1.5x, 1.7x, 2x, 3x per
// axis). The file keeps the number: any other value shows as Custom and stays until a mode is picked.
static bool IsRenderScaleSetting(const char* section, const char* key) {
	return !strcmp(key, "RenderScale") && (!strcmp(section, "Shaders.TAA.FSR") || !strcmp(section, "Shaders.TAA.DLSS"));
}
static bool RenderScaleCombo(const char* section, float& val) {
	static const float scales[] = { 1.0f, 0.667f, 0.588f, 0.5f, 0.333f };
	const bool dlss = !strcmp(section, "Shaders.TAA.DLSS");
	const char* const names[] = { dlss ? "Native (DLAA)" : "Native (anti-aliasing only)", "Quality (67%)", "Balanced (59%)",
		"Performance (50%)", "Ultra Performance (33%)" };
	int current = -1;
	for (int i = 0; i < 5; i++) if (fabsf(val - scales[i]) < 0.005f) current = i;
	char custom[48];
	sprintf_s(custom, "Custom (%.0f%%)", val * 100.0f);
	const int picked = NamePicker("##v", current >= 0 ? names[current] : custom, names, 5, current);
	if (picked < 0) return false;
	val = scales[picked];
	return true;
}

// UNOFFICIAL 2026-11-09 (user: "clamp settings in the menu if a user sets it higher than the limit so there's no confusion"): a number
// whose description opens with its limits, "[0-4] How much ...", is held to them in the menu. It cannot be dragged, typed (Ctrl+click)
// or stepped past them, and one saved past a limit (a hand-edited file) shows the limit the effect reads it at. The limits are read from
// the defaults' own comment, so the menu and the settings file say the same; the effects clamp what they read to the same numbers (the
// ranges and the code's clamps were compared once: 0 differ). Only "[a-b]" counts; "[1, 2, 4, 8]" lists and plain text are left alone.
static bool DocumentedRange(const std::string& description, float& lo, float& hi) {
	if (description.size() < 5 || description[0] != '[') return false;
	const char* p = description.c_str() + 1;
	char* end = nullptr;
	const float a = strtof(p, &end);
	if (end == p) return false;
	while (*end == ' ') end++;
	if (*end != '-') return false;
	p = ++end;
	while (*p == ' ') p++;
	const float b = strtof(p, &end);
	if (end == p) return false;
	while (*end == ' ') end++;
	if (*end != ']' || !(b > a)) return false;
	lo = a;
	hi = b;
	return true;
}

// UNOFFICIAL: float settings too large for the drag step every other float uses (0.001 a pixel), such as distances in game
// units, render as a slider over a fixed range instead. Keyed by "Section.Key" like the choice lists.
struct FloatRange { float Min; float Max; const char* Format; };
static const std::unordered_map<std::string, FloatRange> kFloatSliders = {
	{ "Shaders.PBR.Metal.FadeDistance", { 0.0f, 4000.0f, "%.0f" } },
	{ "Shaders.ShadowsInteriors.Main.NearFade", { 0.0f, 128.0f, "%.0f" } },
	{ "Shaders.ShadowsInteriors.Main.PlayerLampMargin", { 0.0f, 64.0f, "%.0f" } },
	// UNOFFICIAL 2026-10-21 cloth indoors (core/ClothWind.h BeginFrame)
	// 2026-11-16 GunFX's menu keys (gunfx-builtin; the Gun effects page)
	{ "Main.GunFX.Puff.Scale", { 0.2f, 5.0f, "%.2f" } }, { "Main.GunFX.Puff.Opacity", { 0.0f, 0.3f, "%.3f" } },
	{ "Main.GunFX.Puff.SmokeLife", { 0.2f, 5.0f, "%.2f" } }, { "Main.GunFX.Puff.MinInterval", { 0.0f, 1.0f, "%.2f" } },
	{ "Main.GunFX.Wisp.Scale", { 0.5f, 6.0f, "%.2f" } }, { "Main.GunFX.Wisp.Opacity", { 0.0f, 0.5f, "%.3f" } },
	{ "Main.GunFX.Wisp.SmokeLife", { 0.5f, 8.0f, "%.2f" } }, { "Main.GunFX.Wisp.HeatPerShot", { 0.05f, 3.0f, "%.2f" } },
	{ "Main.GunFX.Wisp.CoolPerSecond", { 0.2f, 15.0f, "%.1f" } },
	{ "Main.GunFX.Trail.Scale", { 0.5f, 6.0f, "%.2f" } }, { "Main.GunFX.Trail.Opacity", { 0.0f, 0.6f, "%.3f" } },
	{ "Main.GunFX.Trail.SmokeLife", { 0.5f, 10.0f, "%.2f" } }, { "Main.GunFX.Trail.StartDelay", { 0.0f, 3.0f, "%.2f" } },
	{ "Main.GunFX.Ejection.Scale", { 0.01f, 3.0f, "%.2f" } }, { "Main.GunFX.Ejection.HotScale", { 1.0f, 5.0f, "%.2f" } },
	{ "Main.GunFX.Ejection.Opacity", { 0.0f, 0.3f, "%.3f" } }, { "Main.GunFX.Ejection.SmokeLife", { 0.2f, 5.0f, "%.2f" } },
	{ "Main.GunFX.Glow.Radius", { 1.0f, 20.0f, "%.1f" } }, { "Main.GunFX.Glow.Length", { 0.0f, 40.0f, "%.1f" } },
	{ "Main.GunFX.Glow.StartHeat", { 0.0f, 15.0f, "%.1f" } }, { "Main.GunFX.Glow.RiseSeconds", { 0.1f, 10.0f, "%.1f" } },
	{ "Main.GunFX.Haze.StrengthPixels", { 0.0f, 8.0f, "%.1f" } }, { "Main.GunFX.Haze.HeightPixels", { 1.0f, 200.0f, "%.0f" } },
	{ "Main.GunFX.Haze.StartHeat", { 0.0f, 15.0f, "%.1f" } },
	{ "Main.GunFX.Blast.StrengthPixels", { 0.0f, 30.0f, "%.1f" } }, { "Main.GunFX.Blast.RadiusPixels", { 5.0f, 400.0f, "%.0f" } },
	{ "Main.GunFX.Blast.Seconds", { 0.01f, 1.0f, "%.2f" } },
	{ "Main.GunFX.Volume.DensityScale", { 0.5f, 12.0f, "%.2f" } }, { "Main.GunFX.Volume.SizeScale", { 0.05f, 1.0f, "%.2f" } },
	{ "Main.GunFX.Volume.Brightness", { 0.2f, 3.0f, "%.2f" } }, { "Main.GunFX.Volume.Wind", { 0.0f, 120.0f, "%.0f" } },
	{ "Main.GunFX.Volume.Haze", { 0.0f, 10.0f, "%.1f" } }, { "Main.GunFX.Volume.HazeLife", { 5.0f, 180.0f, "%.0f" } },
	{ "Main.GunFX.Volume.HazeSize", { 20.0f, 300.0f, "%.0f" } },
	{ "Shaders.Cloth.Interiors.Strength", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.WindIntensity", { 0.0f, 12.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.WindSpeed", { 0.0f, 400.0f, "%.0f" } },
	{ "Shaders.Cloth.Interiors.Wavelength", { 16.0f, 1000.0f, "%.0f" } },
	{ "Shaders.Cloth.Interiors.FlutterSpeed", { 0.0f, 6.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.FlutterAmount", { 0.0f, 1.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.RippleAmount", { 0.0f, 1.0f, "%.2f" } },
	// UNOFFICIAL 2026-11-26 Bottled Shaders / Bend Screen-Space Shadows sliders
	{ "Shaders.ShadowsExteriors.SSShadows.SurfaceThickness", { 0.005f, 0.05f, "%.3f" } },
	{ "Shaders.ShadowsExteriors.SSShadows.BilinearThreshold", { 0.02f, 1.0f, "%.2f" } },
	{ "Shaders.ShadowsExteriors.SSShadows.ShadowContrast", { 0.0f, 4.0f, "%.2f" } },
	// VolumetricFog hard start-distance sliders (game units, 70 = 1 m; -1 = use existing range/weather start)
	{ "Shaders.VolumetricFog.Main.StartDistance", { 0.0f, 100000.0f, "%.0f" } },
	{ "Shaders.VolumetricFog.Main.DistantFogStart", { -1.0f, 150000.0f, "%.0f" } },
	{ "Shaders.VolumetricFog.Main.AerialStartDistance", { -1.0f, 150000.0f, "%.0f" } },
	{ "Shaders.VolumetricFog.Main.GameFogStart", { -1.0f, 100000.0f, "%.0f" } },
	{ "Shaders.VolumetricFog.Night.StartDistance", { -1.0f, 100000.0f, "%.0f" } },
	{ "Shaders.VolumetricFog.Interiors.StartDistance", { 0.0f, 15000.0f, "%.0f" } },
	{ "Shaders.VolumetricFog.Interiors.GameFogStart", { -1.0f, 15000.0f, "%.0f" } },
	// Skin sliders (Width = 28.6 default was nearly immobile at 0.001 drag speed)
	{ "Shaders.Skin.Main.SpecularStrength", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Skin.Main.Roughness", { 0.1f, 1.0f, "%.2f" } },
	{ "Shaders.Skin.Main.Scattering", { 0.0f, 4.0f, "%.2f" } },
	{ "Shaders.Skin.Main.Translucency", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Skin.Main.SkyReflection", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Skin.Main.RainWetness", { 0.0f, 1.0f, "%.2f" } },
	{ "Shaders.Skin.Main.VanillaMatchedHighlights", { 0.0f, 1.0f, "%.2f" } },
	{ "Shaders.Skin.Interiors.SpecularStrength", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Skin.Interiors.Roughness", { 0.1f, 1.0f, "%.2f" } },
	{ "Shaders.Skin.Interiors.Scattering", { 0.0f, 4.0f, "%.2f" } },
	{ "Shaders.Skin.Interiors.Translucency", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Skin.Interiors.Width", { 0.0f, 80.0f, "%.1f" } },
	{ "Shaders.Skin.Interiors.AlbedoDetail", { 0.0f, 1.0f, "%.2f" } },
	{ "Shaders.Skin.Scattering.Width", { 0.0f, 80.0f, "%.1f" } },
	{ "Shaders.Skin.Scattering.AlbedoDetail", { 0.0f, 1.0f, "%.2f" } },
	{ "Shaders.ShadowsExteriors.Deferred.SunNoonElevation", { 20.0f, 75.0f, "%.1f" } },
	{ "Shaders.ShadowsExteriors.Deferred.SunAzimuthOffset", { -180.0f, 180.0f, "%.0f" } },
	{ "Shaders.ShadowsExteriors.Deferred.SunSheen", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.ShadowsExteriors.Deferred.SunSheenRoughness", { 0.45f, 0.95f, "%.2f" } },
};

// ---- Setting rows ------------------------------------------------------------------------------------------------------------
// One row per setting: its name on the left, the control filling the rest, a restart note if it needs one, and the ~ button at
// the end while the value differs from what it was when the menu opened. Max Quality's values are red (and editable); settings
// the current mode does not read are grey, with the reason on hover.

struct SettingRow {
	SettingManager::Configuration::ConfigNode& node;
	const std::string& atOpen;	// the value when the menu opened
	bool changed;				// differs from atOpen
	bool maxQuality;			// the value is Max Quality's
	bool inert;					// nothing reads it (greyed and disabled)
	const char* why;			// why it is greyed, or nullptr
	ImVec4 normalText;			// the style's text colour, for the tooltip
	RestartNote restart;		// restart.text is null when there is no note
	float controlW = 0.0f;		// the control's width (set by RenderSetting)
	const char* note = nullptr;	// a hover note that does not grey the row (the linear gamma floor)
};

static bool LinearGammaFloored(const char* section, const char* key, bool& night) {
	if (strcmp(section, "Shaders.PBR.Linear")) return false;
	night = !strcmp(key, "NightGamma");
	return night || !strcmp(key, "Gamma");
}
static const char* LinearGammaNote(bool night) {
	return night ? "With the deferred renderer on, 1.0 or below reads as 0 (follow Gamma)."
		: "With the deferred renderer on, 1.0 or below reads as 2.2.";
}

static void Store(const SettingManager::Configuration::ConfigNode& node, float value) {
	TheSettingManager->SetSetting(node.Section, node.Key, value);
	TheSettingManager->LoadSettings();
}

// The hover note of a greyed (inert) switch or number: Max Quality's file, or why nothing reads it.
static void InertSettingTooltip(const SettingRow& row) {
	if (row.maxQuality) {
		ImGui::SetTooltip("Max Quality value (VegasShaders.maxquality.toml). Changes here are saved to "
			"VegasShaders.maxquality.user.toml, not to your own settings.");
		return;
	}
	ImGui::SetTooltip("Disabled: has no effect. Mipmaps/anisotropy are commented out in ShadowsExterior.cpp (deferred shadows "
		"produce artifacted derivatives).");
}

// UNOFFICIAL 2026-10-27: FSR and DLSS are either/or (user: "toggling one disables the other"). 2026-10-29 (tester: DLSS -> FSR 3.1
// in the menu left the picture not upscaled): each has its own RenderScale, so the one switched on takes the RenderScale of the
// one switched off, and the picture keeps its render size.
static void SwitchOffOtherUpscaler(const SettingManager::Configuration::ConfigNode& node) {
	const char* other = !strcmp(node.Section, "Shaders.TAA.FSR") ? "Shaders.TAA.DLSS"
		: !strcmp(node.Section, "Shaders.TAA.DLSS") ? "Shaders.TAA.FSR" : nullptr;
	if (!other) return;
	if (TheSettingManager->GetSettingI(other, "Enabled")) {
		const float scale = TheSettingManager->GetSettingF(other, "RenderScale");
		if (scale > 0.0f) TheSettingManager->SetSetting(node.Section, "RenderScale", scale);
	}
	TheSettingManager->SetSetting(other, "Enabled", false);
}

// Returns whether the control is under the mouse (as the other Draw* below).
static bool DrawBoolSetting(SettingRow& row) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	bool on = !strcmp(node.Value, "1") || !_stricmp(node.Value, "true");
	if (MenuWidgets::Toggle("##v", &on) && (!on || ConfirmChange(node.Section, node.Key, "true"))) {	// (2026-11-05: conflicts ask first)
		const std::string section = node.Section, key = node.Key;	// 2026-11-26 (unsaved marks): it may write other settings too
		PickerWrites writes("row " + section + ":" + key, [section, key]() { return TheSettingManager->GetSettingI(section.c_str(), key.c_str()); });
		TheSettingManager->SetSetting(node.Section, node.Key, on);
		if (on && !strcmp(node.Key, "Enabled")) SwitchOffOtherUpscaler(node);
		if (on && !strcmp(node.Section, "Shaders.VolumetricClouds.Main") && !strcmp(node.Key, "PhysicalShadows") &&
			TheSettingManager->GetSettingF("Shaders.VolumetricClouds.Main", "Shadows") <= 0.0f)
			TheSettingManager->SetSetting("Shaders.VolumetricClouds.Main", "Shadows", 0.6f);
		TheSettingManager->LoadSettings();
	}
	return ImGui::IsItemHovered();
}

// A dropdown of quality modes (render scale), a slider over a fixed range, or a drag box. The +/- keys step it while the mouse
// is over it; for shader pages, Developer options add -/+ buttons that do the same.
static bool DrawFloatSetting(SettingRow& row, bool isShader) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	float value = (float)atof(node.Value);
	const bool modes = IsRenderScaleSetting(node.Section, node.Key);	// UNOFFICIAL 2026-10-27
	const auto range = kFloatSliders.find(std::string(node.Section) + "." + node.Key);
	// UNOFFICIAL 2026-11-09: held to its limits (a slider's own, else the "[a-b]" its description opens with): dragging, typing and the
	// step keys / buttons all stop there, and a value saved past one shows the limit it is read at (the file is left alone until edited)
	float lo = 0.0f, hi = 0.0f;
	bool limited = false;
	if (!modes) {
		if (range != kFloatSliders.end()) { lo = range->second.Min; hi = range->second.Max; limited = true; }
		else limited = DocumentedRange(node.Description, lo, hi);
	}
	// UNOFFICIAL 2026-11-16: with the deferred renderer on, Gamma <= 1.0 reads as 2.2 (SettingForce::FloorF): show the value in
	// effect, and the lower end starts above 1.0. Only an edit writes. NightGamma keeps its stored value (0 must stay reachable, and
	// a drag up from 0 passes through (0, 1.0]); its row's note tells the floor.
	bool night = false;
	if (LinearGammaFloored(node.Section, node.Key, night) && !night) {
		if (value <= 1.0f) value = 2.2f;
		if (limited) lo = ImMax(lo, 1.01f);
	}
	if (limited) value = ImClamp(value, lo, hi);

	ImGui::SetNextItemWidth(row.controlW);
	bool edited;
	if (modes) edited = RenderScaleCombo(node.Section, value);
	else if (range != kFloatSliders.end()) edited = ImGui::SliderFloat("##v", &value, lo, hi, range->second.Format, ImGuiSliderFlags_AlwaysClamp);
	else if (limited) {
		const float span = hi - lo;
		const float speed = ImClamp(span * 0.002f, 0.001f, 5.0f);
		const char* fmt = span >= 100.0f ? "%.1f" : span >= 10.0f ? "%.2f" : span >= 1.0f ? "%.3f" : "%.4f";
		edited = ImGui::DragFloat("##v", &value, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
	}
	else {
		const float mag = fabsf(value);
		const float speed = ImClamp(mag * 0.005f, 0.002f, 2.0f);
		const char* fmt = mag >= 100.0f ? "%.1f" : mag >= 10.0f ? "%.2f" : "%.3f";
		edited = ImGui::DragFloat("##v", &value, speed, 0.0f, 0.0f, fmt);
	}
	if (edited) Store(node, value);

	const bool hovered = ImGui::IsItemHovered();
	if (hovered && !modes && (gMenu.plusKey || gMenu.minusKey)) {
		value += gMenu.plusKey ? gMenu.step : -gMenu.step;
		if (limited) value = ImClamp(value, lo, hi);
		Store(node, value);
	}
	if (isShader && DevOptionsShown() && !modes) {	// UNOFFICIAL 2026-10-25
		for (const char* button : { "-", "+" }) {
			ImGui::SameLine();
			if (!ImGui::SmallButton(button)) continue;
			value += button[0] == '+' ? gMenu.step : -gMenu.step;
			if (limited) value = ImClamp(value, lo, hi);
			Store(node, value);
		}
	}
	return hovered;
}

// UNOFFICIAL 2026-10-25 menu tidy: whole numbers that are plain 0/1 switches get a switch (it writes 0 or 1; the type in the file
// stays). A choice list gets its dropdown; any other number a drag box.
static bool DrawIntSetting(SettingRow& row) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	const int stored = atoi(node.Value);
	int wanted = stored;
	const Choices* choices = ChoicesFor(node.Section, node.Key);
	if (!choices && (stored == 0 || stored == 1) && IsOnOffInteger(node)) {
		bool on = stored != 0;
		if (MenuWidgets::Toggle("##v", &on)) wanted = on ? 1 : 0;
	}
	else if (choices) {
		const int shown = ImClamp(stored, 0, choices->count - 1);
		ImGui::SetNextItemWidth(row.controlW);
		const int picked = NamePicker("##v", choices->names[shown], choices->names, choices->count, shown);
		if (picked >= 0) wanted = picked;
		if (ImGui::IsItemHovered() && (gMenu.plusKey || gMenu.minusKey))
			wanted = ImClamp(shown + (gMenu.plusKey ? 1 : -1), 0, choices->count - 1);
	}
	else {
		ImGui::SetNextItemWidth(row.controlW);
		float lo = 0.0f, hi = 0.0f;
		if (DocumentedRange(node.Description, lo, hi)) {	// UNOFFICIAL 2026-11-09: held to its documented limits, as the float rows
			const int low = (int)lo, high = (int)hi;
			int shown = ImClamp(stored, low, high);	// a value saved past a limit shows the limit it is read at; only an edit writes
			ImGui::DragInt("##v", &shown, 1.0f, low, high, "%d", ImGuiSliderFlags_AlwaysClamp);
			if (shown != ImClamp(stored, low, high)) wanted = shown;
		}
		else ImGui::DragInt("##v", &wanted, 1.0f);
	}
	const bool hovered = ImGui::IsItemHovered();
	if (wanted != stored && ConfirmChange(node.Section, node.Key, std::to_string(wanted))) {	// (2026-11-05: conflicts ask first)
		TheSettingManager->SetSetting(node.Section, node.Key, wanted);
		TheSettingManager->LoadSettings();
	}
	return hovered;
}

// A text box, written back when editing ends. What is typed lives in its own buffer (per box) so the stored value does not
// overwrite it mid-edit. KeyEnable also gets the "(?)" scan code reference.
static bool DrawTextSetting(SettingRow& row) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	static std::unordered_map<ImGuiID, std::string> typing;
	const ImGuiID box = ImGui::GetID("##v");
	std::string& text = typing[box];
	if (ImGui::GetActiveID() != box) text = node.Value;

	char edit[80];
	strncpy_s(edit, text.c_str(), _TRUNCATE);
	ImGui::SetNextItemWidth(row.controlW);
	if (ImGui::InputText("##v", edit, sizeof(edit))) text = edit;
	const bool hovered = ImGui::IsItemHovered();
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		TheSettingManager->SetSettingS(node.Section, node.Key, edit);
		TheSettingManager->LoadSettings();
	}
	if (!strcmp(node.Key, "KeyEnable")) {
		ImGui::SameLine();
		if (ImGui::SmallButton("(?)")) ImGui::OpenPopup("DIKReference");
		ScanCodePopup();
	}
	return hovered;
}

// Width kept after the control for what follows it on the row.
static float TrailingWidth(const SettingRow& row, bool isShader) {
	using NodeType = SettingManager::Configuration::NodeType;
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	float width = MenuWidgets::ResetSlotWidth() + RestartNoteWidth(row.restart);
	const SettingManager::Configuration::ConfigNode& node = row.node;
	if (node.Type == NodeType::Float && isShader && DevOptionsShown() && !IsRenderScaleSetting(node.Section, node.Key))
		width += MenuWidgets::SmallButtonWidth("-") + MenuWidgets::SmallButtonWidth("+") + gap * 2.0f;
	if (node.Type != NodeType::Boolean && node.Type != NodeType::Float && node.Type != NodeType::Integer && !strcmp(node.Key, "KeyEnable"))
		width += MenuWidgets::SmallButtonWidth("(?)") + gap;
	return width;
}

static void RenderSetting(SettingManager::Configuration::ConfigNode& stored, bool isShader = false, const char* labelOverride = nullptr) {
	using NodeType = SettingManager::Configuration::NodeType;
	ImGui::PushID(stored.Key);

	// The session's first value and "changed" come from the player's own value (stored), never from a forced one.
	const std::string& atOpen = SessionValue(stored.Section, stored.Key, stored.Value);
	const bool changed = atOpen != stored.Value;
	SettingManager::Configuration::ConfigNode& node = stored;

	const bool inert = IsInertSetting(node.Section, node.Key);
	SettingRow row{ node, atOpen, changed, node.MaxQuality, inert,
		inert ? nullptr : InactiveReason(node.Section, node.Key), ImGui::GetStyleColorVec4(ImGuiCol_Text) };
	FindRestartNote(node.Section, node.Key, row.restart);
	bool night = false;
	if (LinearGammaFloored(node.Section, node.Key, night)) row.note = LinearGammaNote(night);

	if (row.inert) ImGui::BeginDisabled();
	int tints = 0;
	if (row.maxQuality) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
		tints++;
	}
	if (row.why) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		tints++;
	}

	row.controlW = MenuWidgets::RowBegin(labelOverride ? labelOverride : MenuKeyLabel(node.Section, node.Key), TrailingWidth(row, isShader),
		UnsavedSettings::Key(stored.Section, stored.Key));	// 2026-11-25 (unsaved marks)
	const bool controlDisabled = !strcmp(node.Section, "Shaders.VolumetricClouds.Main") && !strcmp(node.Key, "Shadows") && row.why != nullptr;
	// A forced / overridden row is disabled after its name, so hovering the name still shows the note (ImGui does not hover disabled items)
	if (controlDisabled) ImGui::BeginDisabled();
	bool overControl;
	switch (node.Type) {
	case NodeType::Boolean:
		overControl = DrawBoolSetting(row);
		if (row.inert && overControl) InertSettingTooltip(row);
		break;
	case NodeType::Float:
		overControl = DrawFloatSetting(row, isShader);
		break;
	case NodeType::Integer:
		overControl = DrawIntSetting(row);
		if (row.inert && overControl) InertSettingTooltip(row);
		break;
	default:
		// [Main.Menu.Style] Custom<colour>: a tick and a colour box instead of the text box (2026-11-02)
		if (!MenuSkin::DrawColourSetting(node.Section, node.Key, node.Value, row.controlW, overControl))
			overControl = DrawTextSetting(row);
		break;
	}
	if (controlDisabled) overControl = overControl || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	if (tints) ImGui::PopStyleColor(tints);

	if (row.restart.text) {
		ImGui::SameLine();
		DrawRestartNote(row.restart);
	}
	if (MenuWidgets::ResetButton("~", row.changed, "Revert to value at session start")) {
		TheSettingManager->SetSettingS(node.Section, node.Key, row.atOpen.c_str());
		TheSettingManager->LoadSettings();
	}
	if (controlDisabled) ImGui::EndDisabled();
	if (row.inert) ImGui::EndDisabled();

	if (overControl || MenuWidgets::RowLabelHovered()) {
		const char* plain = AdvancedShown() ? nullptr : SimpleTip(node.Section, node.Key);	// 2026-11-05: the simple view's own words
		if (plain) {
			std::string tip = plain;
			if (row.why) tip += std::string("\n\nNot active now: ") + row.why;
			if (row.note) tip += std::string("\n\n") + row.note;
			WrappedTooltip(tip.c_str());
		}
		else SettingTooltip(node, row.why ? row.why : row.note, row.normalText, MenuWidgets::RowLabelClipped());
	}
	ImGui::PopID();
}

// ---- "New in this patch" tab ---------------------------------------------------------------------
// UNOFFICIAL 2026-10-18 (user: "a tab at the top that has all new settings in it for that specific patch for easy testing. it
// shouldn't replace the settings that exist in the menus where they originate from"). A first entry in the sidebar that lists
// the settings new in the current patch, grouped by the section they live in. Every entry is the very same setting drawn by the
// same RenderSetting as in its own section (same storage, same Save, same tooltip, same revert button): the original menus are
// untouched. Which settings count is decided from the defaults file alone, never from the user's settings:
//   - the description (the comment next to the key) holds a date tag ("(2026-10-17)") on or after [Main.Menu.NewInPatch] NewSince
//     (the latest date in the text counts, so an old setting whose text gained a newer date is listed too), or
//   - the key is in NewInclude ("Section.Key" or "Section.*", comma separated, for keys whose text has no date), and
//   - not in NewExclude.
// Diagnostic keys (names ending in Check / Log / Probe / DebugView, descriptions that say diagnostic or test only, or NewDiagnostics)
// go in a collapsed Diagnostics group at the bottom. The list is built once (at start and after "Disk" reloads the defaults) and
// each entry's value is re-read only when a setting changed (SettingManager::SettingsRevision) or every 30 frames.
static const char* const kNewTabId = "@NewInPatch";

struct NewEntry {
	std::string Section, Key, Date;
	bool        Diagnostic = false;
	bool        Status     = false;	// Shaders.<Name>.Status Enabled: the shader's on/off switch (drawn like the sidebar's checkbox)
	std::string ShaderName;
	bool        Valid = false;
	SettingManager::Configuration::ConfigNode Node;
};
struct NewGroup { std::string Section, Display; size_t Begin, End; };

static std::vector<NewEntry> s_newMain, s_newDiag;	// sorted by section, then key (the same order as the section's own menu)
static std::vector<NewGroup> s_newMainGroups, s_newDiagGroups;
static std::string s_newSince;
static UInt32      s_newGeneration = 0xFFFFFFFFu;	// DefaultsGeneration the list was built from
static UInt32      s_newRevision   = 0xFFFFFFFFu;	// SettingsRevision the values were read at
static int         s_newFrames     = 0;

static const tomlValue* NewFind(const tomlValue* table, const char* name) {
	if (!table || !table->is_table()) return nullptr;
	const auto& map = table->as_table();
	auto it = map.find(name);
	return it == map.end() ? nullptr : &it->second;
}

// A hidden string from [Main.Menu.<page>], read from the defaults file only ("" if the section, the key or its text is missing).
static std::string MenuMeta(const char* page, const char* key) {
	try {
		const tomlValue* section = NewFind(NewFind(NewFind(&TheSettingManager->Config.DefaultConfig, "_Main"), "Menu"), page);
		const tomlValue* value = NewFind(section, key);
		if (value && value->is_string()) return value->as_string();
	} catch (...) {}
	return std::string();
}
static std::string NewMeta(const char* key) { return MenuMeta("NewInPatch", key); }

static void NewSplitList(const std::string& text, std::vector<std::string>& out) {
	std::string cur;
	for (size_t i = 0; i <= text.size(); i++) {
		const char c = i < text.size() ? text[i] : ',';
		if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (!cur.empty()) out.push_back(cur);
			cur.clear();
		} else cur += c;
	}
}

// "Section.Key" is that setting; "Section.*" is every setting of that section and of the sections below it.
static bool NewListMatches(const std::vector<std::string>& list, const std::string& section, const std::string& key) {
	for (const std::string& p : list) {
		if (p.size() > 2 && p.compare(p.size() - 2, 2, ".*") == 0) {
			const std::string base = p.substr(0, p.size() - 2);
			if (section == base || (section.size() > base.size() && section.compare(0, base.size(), base) == 0 && section[base.size()] == '.')) return true;
		} else {
			const size_t dot = p.rfind('.');
			if (dot != std::string::npos && dot > 0 && dot == section.size() && p.compare(0, dot, section) == 0 && p.compare(dot + 1, std::string::npos, key) == 0) return true;
		}
	}
	return false;
}

// UNOFFICIAL 2026-10-25 menu tidy: [Main.Menu.Developer] DevOnly (defaults file only, the same format as NewInclude): settings shown
// only with Developer options. An empty key asks about a whole section (true for a "Section.*" entry at or above it).
static bool IsDevOnlySetting(const std::string& section, const std::string& key) {
	static std::vector<std::string> list;
	static UInt32 generation = 0xFFFFFFFFu;
	if (generation != TheSettingManager->Config.DefaultsGeneration) {	// the defaults were read again ("Disk")
		generation = TheSettingManager->Config.DefaultsGeneration;
		list.clear();
		NewSplitList(MenuMeta("Developer", "DevOnly"), list);
	}
	return NewListMatches(list, section, key);
}

// UNOFFICIAL 2026-11-12 menu levels: [Main.Menu.Developer] Internal (defaults file only, the DevOnly format): settings the menu never
// shows, not even with Developer options: the on/off switches of bug fixes, test and Old* comparison keys, debug views and
// self-checks, capture / compile / cache plumbing. With Developer options on, people flipped them at random and the results came
// back as bug reports. They keep their saved values and work from the settings file. [Main.Menu.Developer] ShowInternal = true,
// typed into the user's own file (never a row), shows them again under Developer options. An empty key asks about a whole section.
static bool IsInternalSetting(const std::string& section, const std::string& key) {
	static std::vector<std::string> list;
	static UInt32 generation = 0xFFFFFFFFu;
	if (generation != TheSettingManager->Config.DefaultsGeneration) {	// the defaults were read again ("Disk")
		generation = TheSettingManager->Config.DefaultsGeneration;
		list.clear();
		NewSplitList(MenuMeta("Developer", "Internal"), list);
	}
	return NewListMatches(list, section, key);
}

// UNOFFICIAL 2026-11-12: the level the menu gives a setting, for the session log and the reset below (description: the default's
// first comment, as ConfigNode::Description). 0 = a player's setting, 1 = Developer options (IsTestSetting's rules), 2 = Internal.
int ImGuiManager::SettingLevel(const char* section, const char* key, const char* description) {
	if (!TheSettingManager || !section || !key) return 0;
	if (IsInternalSetting(section, key)) return 2;
	if (SimpleLabel(section, key)) return 0;	// a row of a simple view: every player sees it, whatever the test rules make of its comment (PTRealtime)
	static const std::vector<std::string> none;
	if (IsDeadSetting(section, key) || NewIsDiagnostic(key, description ? description : "", none, section) || IsDevOnlySetting(section, key)) return 1;
	return 0;
}

// The latest YYYY-MM-DD (20xx, month 1-12, day 1-31, not part of a longer number) in the text, or "" if none.
static std::string NewLatestDate(const std::string& s) {
	std::string best;
	auto dig = [](char c) { return c >= '0' && c <= '9'; };
	for (size_t i = 0; i + 10 <= s.size(); i++) {
		const char* p = s.c_str() + i;
		if (!(p[0] == '2' && p[1] == '0' && dig(p[2]) && dig(p[3]) && p[4] == '-' && dig(p[5]) && dig(p[6]) && p[7] == '-' && dig(p[8]) && dig(p[9]))) continue;
		if (i > 0 && dig(s[i - 1])) continue;
		if (i + 10 < s.size() && dig(s[i + 10])) continue;
		const int month = (p[5] - '0') * 10 + (p[6] - '0'), day = (p[8] - '0') * 10 + (p[9] - '0');
		if (month < 1 || month > 12 || day < 1 || day > 31) continue;
		const std::string date(p, 10);
		if (date > best) best = date;
	}
	return best;
}

static bool NewEndsWith(const std::string& s, const char* suffix) {
	const size_t n = strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static bool NewIsDiagnostic(const std::string& key, const std::string& description, const std::vector<std::string>& extra, const std::string& section) {
	if (NewListMatches(extra, section, key)) return true;
	static const char* const suffixes[] = { "Check", "Log", "DebugView", "Probe", "ProbeDump", "ProbeLog", "Dump", "TestSkip", "OrderLog", "Debug" };
	for (const char* suffix : suffixes) if (NewEndsWith(key, suffix)) return true;
	if (key.compare(0, 5, "Debug") == 0) return true;
	if (key.size() > 3 && key.compare(0, 3, "Log") == 0 && isupper((unsigned char)key[3])) return true;	// 2026-10-19: LogLamps, LogMatches, LogDraws
	std::string lower = description;
	for (char& c : lower) c = (char)tolower((unsigned char)c);
	static const char* const words[] = { "diagnostic", "test only", "testing", "debugging", "test views", "development builds only", "no effect:",
		"for finding faults", "checking only" };
	for (const char* word : words) if (lower.find(word) != std::string::npos) return true;
	return false;
}

struct NewWalkContext {
	std::string Since;
	std::vector<std::string> Include, Exclude, Diagnostics;
};

static void NewWalk(const tomlValue& table, const std::string& path, NewWalkContext& ctx) {
	for (const auto& [name, value] : table.as_table()) {
		if (value.is_table()) {
			const std::string child = path.empty() ? (name.compare(0, 1, "_") == 0 ? name.substr(1) : name) : path + "." + name;
			if (child == "Weathers" || child.compare(0, 9, "Weathers.") == 0) continue;
			NewWalk(value, child, ctx);
			continue;
		}
		if (path.empty()) continue;	// a setting outside any section
		if (path == "Main.Menu.NewInPatch") continue;	// this tab's own data
		if (!(value.is_boolean() || value.is_integer() || value.is_floating() || value.is_string())) continue;	// the menu cannot show it either
		if (("." + path + ".").find(".WeatherMode.") != std::string::npos) continue;	// hidden in the menu (ShouldHideSection)
		const bool status = NewEndsWith(path, ".Status");
		if (status && !(path.compare(0, 8, "Shaders.") == 0 && name == "Enabled")) continue;	// Status sections are hidden in the menu; only a shader's own switch has a place
		if (ShouldHideKey(name.c_str())) continue;
		if (IsInternalSetting(path, name)) continue;	// UNOFFICIAL 2026-11-12: never in this tab (with ShowInternal: on its own page and in the search)
		if (ShelvedEffects::Key(path, name)) continue;	// UNOFFICIAL 2026-11-16: a shelved effect (release builds only; not in IsInternalSetting, so "Reset test and internal settings" leaves its saved keys alone)

		std::string description;
		for (const std::string& c : value.comments()) { description += c; description += ' '; }
		const std::string date = NewLatestDate(description);
		bool isNew = !ctx.Since.empty() && !date.empty() && date >= ctx.Since;
		if (NewListMatches(ctx.Include, path, name)) isNew = true;
		if (NewListMatches(ctx.Exclude, path, name)) isNew = false;
		if (!isNew) continue;

		NewEntry entry;
		entry.Section = path;
		entry.Key = name;
		entry.Date = date;
		const bool isSimple = SimpleLabel(path.c_str(), name.c_str()) != nullptr;
		entry.Diagnostic = !isSimple && (IsDeadSetting(path.c_str(), name.c_str()) ||
			IsDevOnlySetting(path, name) || NewIsDiagnostic(name, description, ctx.Diagnostics, path));
		if (status) {
			entry.Status = true;
			const size_t dot = path.find('.', 8);
			entry.ShaderName = path.substr(8, dot == std::string::npos ? std::string::npos : dot - 8);
			if (entry.ShaderName.empty()) continue;
		}
		(entry.Diagnostic ? s_newDiag : s_newMain).push_back(std::move(entry));
	}
}

static void NewMakeGroups(const std::vector<NewEntry>& entries, std::vector<NewGroup>& groups) {
	groups.clear();
	for (size_t i = 0; i < entries.size(); i++) {
		if (groups.empty() || groups.back().Section != entries[i].Section) {
			std::string display;
			for (char c : entries[i].Section) display += (c == '.') ? std::string(" > ") : std::string(1, c);
			groups.push_back({ entries[i].Section, display, i, i + 1 });
		} else groups.back().End = i + 1;
	}
}

static void NewPatchRebuild() {
	s_newMain.clear();
	s_newDiag.clear();
	s_newMainGroups.clear();
	s_newDiagGroups.clear();
	s_newRevision = 0xFFFFFFFFu;
	s_newGeneration = TheSettingManager->Config.DefaultsGeneration;
	try {
		NewWalkContext ctx;
		ctx.Since = NewMeta("NewSince");
		s_newSince = ctx.Since;
		NewSplitList(NewMeta("NewInclude"), ctx.Include);
		NewSplitList(NewMeta("NewExclude"), ctx.Exclude);
		NewSplitList(NewMeta("NewDiagnostics"), ctx.Diagnostics);
		if (TheSettingManager->Config.DefaultConfig.is_table()) NewWalk(TheSettingManager->Config.DefaultConfig, std::string(), ctx);
	} catch (const std::exception& e) {
		Logger::Log("[ERROR] NEW IN THIS PATCH: the list could not be built (%s)", e.what());
		s_newMain.clear();
		s_newDiag.clear();
	} catch (...) {
		s_newMain.clear();
		s_newDiag.clear();
	}
	// A table that holds both settings and sub-sections can interleave them: keep every section's settings together.
	const auto bySection = [](const NewEntry& a, const NewEntry& b) { return a.Section < b.Section; };
	std::stable_sort(s_newMain.begin(), s_newMain.end(), bySection);
	std::stable_sort(s_newDiag.begin(), s_newDiag.end(), bySection);
	NewMakeGroups(s_newMain, s_newMainGroups);
	NewMakeGroups(s_newDiag, s_newDiagGroups);
	Logger::Log("NEW IN THIS PATCH: %u settings since %s (%u to test, %u diagnostics)", (UInt32)(s_newMain.size() + s_newDiag.size()),
		s_newSince.empty() ? "(no NewSince)" : s_newSince.c_str(), (UInt32)s_newMain.size(), (UInt32)s_newDiag.size());
}

static size_t NewPatchCount() { return s_newMain.size() + s_newDiag.size(); }
static void NewPatchEnsure() { if (s_newGeneration != TheSettingManager->Config.DefaultsGeneration) NewPatchRebuild(); }	// the defaults were read again ("Disk")

static void NewRefreshNodes(std::vector<NewEntry>& entries) {
	for (NewEntry& e : entries)
		e.Valid = TheSettingManager->Config.FillNode(&e.Node, e.Section.c_str(), e.Key.c_str());
}

// Same colour-picker grouping as the section menus (a prefix with R, G and B keys) when all three are in this group.
// A shader's own on/off switch, as a row ("<Name> (shader on / off)"): the same switch as the one beside it in the sidebar.
static void NewShaderSwitchRow(const NewEntry& e) {
	const char* shader = e.ShaderName.c_str();
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	ImGui::PushID(e.Key.c_str());
	ImGui::BeginDisabled(TheSettingManager->IsShaderForced(shader));
	MenuWidgets::RowBegin((e.ShaderName + " (shader on / off)").c_str(), MenuWidgets::ResetSlotWidth(),
		UnsavedSettings::Key(e.Section.c_str(), e.Key.c_str()));	// 2026-11-25 (unsaved marks)
	const bool flipped = MenuWidgets::Toggle("##v", &on);
	ImGui::EndDisabled();
	if (flipped) {
		TheShaderManager->SwitchShaderStatus(shader);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered()) {
		std::string note = ShownDescription(e.Node.Description);
		if (!note.empty()) note += "\n\n";
		note += "The same switch as the one next to " + e.ShaderName + " in the sidebar.";
		WrappedTooltip(note.c_str());
	}
	ImGui::PopID();
}

static void NewRenderRun(std::vector<NewEntry>& entries, const NewGroup& group) {
	const bool isShader = group.Section.compare(0, 7, "Shaders") == 0;
	std::unordered_set<std::string> drawnKeys;	// the channels of a colour already drawn as one wheel
	for (size_t i = group.Begin; i < group.End; i++) {
		NewEntry& e = entries[i];
		if (!e.Valid || drawnKeys.count(e.Key)) continue;
		if (e.Status) {
			NewShaderSwitchRow(e);
			continue;
		}

		const std::string& key = e.Key;
		if (isShader && key.size() > 1 && (key.back() == 'R' || key.back() == 'G' || key.back() == 'B')) {
			const std::string pfx = key.substr(0, key.size() - 1);
			const bool isColorCurve = (pfx == "ColorCurve" && group.Section.find("Shaders.Coloring") != std::string::npos);
			NewEntry *r = nullptr, *g = nullptr, *b = nullptr;
			for (size_t j = group.Begin; j < group.End; j++) {
				if (entries[j].Key == pfx + "R") r = &entries[j];
				else if (entries[j].Key == pfx + "G") g = &entries[j];
				else if (entries[j].Key == pfx + "B") b = &entries[j];
			}
			if (!isColorCurve && r && g && b && r->Valid && g->Valid && b->Valid) {
				for (const NewEntry* channel : { r, g, b }) drawnKeys.insert(channel->Key);
				RenderColorTriple(r->Node, g->Node, b->Node, pfx);
				continue;
			}
		}
		RenderSetting(e.Node, isShader);
	}
}

// 2026-11-25 (unsaved marks): whether one of the group's settings is unsaved
static bool NewGroupMarked(const std::vector<NewEntry>& entries, const NewGroup& group) {
	for (size_t i = group.Begin; i < group.End && i < entries.size(); i++)
		if (UnsavedSettings::Key(entries[i].Section.c_str(), entries[i].Key.c_str())) return true;
	return false;
}

// Each group under its section's name: a folding heading, or (the diagnostics) a plain grey line with the rows indented.
static void NewRenderGroups(std::vector<NewEntry>& entries, const std::vector<NewGroup>& groups, bool diagnostics) {
	for (const NewGroup& group : groups) {
		ImGui::PushID(group.Section.c_str());
		if (!diagnostics) {
			if (MenuWidgets::SubHeader((group.Display + "##newgroup").c_str(), true, NewGroupMarked(entries, group))) NewRenderRun(entries, group);
		}
		else {
			ImGui::TextDisabled("%s", group.Display.c_str());
			ImGui::Indent();
			NewRenderRun(entries, group);
			ImGui::Unindent();
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
		}
		ImGui::PopID();
	}
}

// A page's title area: the name large, then a dim line under it.
static void PageTitle(const char* title, const char* dimLine) {
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(title);
	ImGui::PopStyleColor();
	if (dimLine && dimLine[0]) ImGui::TextDisabled("%s", dimLine);
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

static void RenderNewInPatch() {
	NewPatchEnsure();

	char counted[96];
	sprintf_s(counted, "%u settings%s%s", (UInt32)NewPatchCount(), s_newSince.empty() ? "" : " since ", s_newSince.c_str());
	PageTitle("New in this patch", counted);
	if (NewPatchCount() == 0) {
		ImGui::TextDisabled("Nothing is marked as new in this patch.");
		return;
	}

	// Values are re-read when a setting changed or every 30 frames (a hotkey, a console command or a preset changes them too).
	if (s_newRevision != TheSettingManager->SettingsRevision || ++s_newFrames >= 30) {
		s_newFrames = 0;
		s_newRevision = TheSettingManager->SettingsRevision;
		NewRefreshNodes(s_newMain);
		NewRefreshNodes(s_newDiag);
	}

	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Each of these is the same setting as in the menu named above it: change it here or there, it is one value "
		"(saved by Save, undone by the ~ button, shown in red while Max Quality sets it). Hover for what it does.");
	ImGui::PopTextWrapPos();

	NewRenderGroups(s_newMain, s_newMainGroups, false);

	if (s_newDiag.empty()) return;
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	if (!DevOptionsShown()) {	// UNOFFICIAL 2026-10-19 menu honesty
		ImGui::TextDisabled("%u test settings hidden (View > Developer options)", (UInt32)s_newDiag.size());
	}
	else {
		const std::string header = "Diagnostics (" + std::to_string(s_newDiag.size()) + ")##newdiag";
		if (MenuWidgets::SubHeader(header.c_str(), false, NewGroupMarked(s_newDiag, { std::string(), std::string(), 0, s_newDiag.size() }))) {
			ImGui::TextDisabled("Test switches for development builds; leave them at their defaults when playing.");
			NewRenderGroups(s_newDiag, s_newDiagGroups, true);
		}
	}
}

// UNOFFICIAL 2026-10-25: menu search. The box at the top of the sidebar filters every menu section and setting by name: the
// text is split into words and a setting matches when every word is in its "Section.Key" (case-insensitive), so "shadow
// bias" or "ambient" both work. A shader's name also finds its on/off switch. The results are the very same settings, drawn
// by the "New in this patch" renderer and grouped under their section; the sidebar lists those sections (a click opens one).
// Weathers (generated per weather) are left out. Clearing the box (the x, or Escape while typing) brings the normal menu back.
static char s_searchBuf[64] = "";
static char* SearchTestBuffer() { return s_searchBuf; }

struct SearchKey { std::string Section, Key, Haystack; bool Status; std::string ShaderName; };
static std::vector<SearchKey> s_searchIndex;
static std::vector<NewEntry>  s_searchResults;
static std::vector<NewGroup>  s_searchGroups;
static std::string s_searchQuery;	// the lower-case text the results were built for
static bool        s_searchTests   = false;	// DevOptionsShown() the results were built with
static bool        s_searchInternal = false;	// UNOFFICIAL 2026-11-12: gMenu.showInternal the results were built with
static size_t      s_searchTotal   = 0;	// matches before the cap
static UInt32      s_searchRevision = 0xFFFFFFFFu;
static int         s_searchFrames   = 0;
static const size_t kSearchMax = 400;	// more than this is not a search any more: ask for another word

static std::string SearchLower(const std::string& s) {
	std::string out(s);
	for (char& c : out) c = (char)tolower((unsigned char)c);
	return out;
}

static std::string SearchQueryText() {
	std::string q = SearchLower(s_searchBuf);
	const size_t b = q.find_first_not_of(' ');
	if (b == std::string::npos) return std::string();
	return q.substr(b, q.find_last_not_of(' ') - b + 1);
}

static bool SearchActive() { return !SearchQueryText().empty(); }

// The same tree as the sidebar (FillMenuSections / FillMenuSettings, the same hidden sections and keys), leaves only.
static std::string MenuEntryLabel(const std::string& name);
static std::string MenuSectionLabel(const std::string& path, const std::string& name);
static std::string ShownText(const std::string& label);

static void SearchIndexSection(const std::string& path, const std::string& name, bool parentIsShaders) {
	if (ShouldHideSection(name)) return;
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	if (children.empty()) {
		std::string secExtra = ShownText(MenuSectionLabel(path, name));
		if (path.compare(0, 8, "Shaders.") == 0) {
			const size_t dot = path.find('.', 8);
			const std::string shader = path.substr(8, dot == std::string::npos ? std::string::npos : dot - 8);
			secExtra += " " + ShownText(MenuEntryLabel(shader));
		}
		SettingManager::Configuration::SettingList page;
		TheSettingManager->FillMenuSettings(&page, path.c_str());
		for (const auto& setting : page) {
			if (ShouldHideKey(setting.Key)) continue;
			std::string hay = path + "." + setting.Key + " " + secExtra;
			if (const char* plain = SimpleLabel(path.c_str(), setting.Key)) { hay += ' '; hay += plain; }
			s_searchIndex.push_back({ path, setting.Key, SearchLower(hay), false, std::string() });
		}
		return;
	}
	if (parentIsShaders && TheSettingManager->GetMenuShaderSwitch(name.c_str()) != SettingManager::MenuSwitch::None)
		s_searchIndex.push_back({ path + ".Status", "Enabled", SearchLower(path + ".Enabled on off " + ShownText(MenuEntryLabel(name))), true, name });	// the sidebar's checkbox
	for (const std::string& child : children)
		SearchIndexSection(path + "." + child, child, path == "Shaders");
}

static void SearchRebuild(const std::string& query) {
	s_searchQuery = query;
	s_searchTests = DevOptionsShown();
	s_searchInternal = gMenu.showInternal;
	s_searchResults.clear();
	s_searchGroups.clear();
	s_searchTotal = 0;
	s_searchRevision = TheSettingManager->SettingsRevision;
	s_searchFrames = 0;

	std::vector<std::string> words;
	for (size_t i = 0; i < query.size();) {
		const size_t j = query.find(' ', i);
		const size_t end = (j == std::string::npos) ? query.size() : j;
		if (end > i) words.push_back(query.substr(i, end - i));
		i = end + 1;
	}
	for (const SearchKey& k : s_searchIndex) {
		bool all = true;
		for (const std::string& w : words) if (k.Haystack.find(w) == std::string::npos) { all = false; break; }
		if (!all) continue;
		if (InternalHidden(k.Section, k.Key)) continue;	// UNOFFICIAL 2026-11-12: not found by the search either
		NewEntry e;
		e.Section = k.Section;
		e.Key = k.Key;
		e.Status = k.Status;
		e.ShaderName = k.ShaderName;
		e.Valid = TheSettingManager->Config.FillNode(&e.Node, e.Section.c_str(), e.Key.c_str());
		if (!e.Valid) continue;
		if (!DevOptionsShown() && IsTestSetting(e.Node)) continue;	// as hidden here as in its own section
		if (++s_searchTotal <= kSearchMax) s_searchResults.push_back(std::move(e));
	}
	NewMakeGroups(s_searchResults, s_searchGroups);
	for (NewGroup& g : s_searchGroups)	// a shader's switch: "Shaders > Name", not its hidden Status section
		if (NewEndsWith(g.Display, " > Status")) g.Display.resize(g.Display.size() - 9);
}

// Keeps the results in step with the box. The index is read again when a search starts (the sections can change with "Disk").
static void SearchEnsure() {
	const std::string query = SearchQueryText();
	if (query.empty()) {
		if (!s_searchQuery.empty()) { s_searchQuery.clear(); s_searchResults.clear(); s_searchGroups.clear(); s_searchIndex.clear(); }
		return;
	}
	if (s_searchIndex.empty()) {
		StringList roots;
		TheSettingManager->FillMenuSections(&roots, nullptr);
		for (const std::string& root : roots)
			if (root != "Weathers") SearchIndexSection(root, root, false);
	}
	if (query != s_searchQuery || s_searchTests != DevOptionsShown() || s_searchInternal != gMenu.showInternal) SearchRebuild(query);
	else if (s_searchRevision != TheSettingManager->SettingsRevision || ++s_searchFrames >= 30) {
		s_searchFrames = 0;
		s_searchRevision = TheSettingManager->SettingsRevision;
		NewRefreshNodes(s_searchResults);
	}
}

static void RenderSearchBox() {
	const char* clearLabel = "x##menusearchclear";
	const float clearW = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputTextWithHint("##menusearch", "Search settings...", s_searchBuf, sizeof(s_searchBuf), ImGuiInputTextFlags_EscapeClearsAll);
	if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
		ImGui::SetTooltip("Type part of a setting's or a section's name (for example \"shadow bias\").\nEvery word must match. Escape or x clears it.");
	ImGui::SameLine();
	const bool empty = s_searchBuf[0] == '\0';
	ImGui::BeginDisabled(empty);
	if (ImGui::Button(clearLabel, ImVec2(clearW, 0.0f))) s_searchBuf[0] = '\0';
	ImGui::EndDisabled();
	SearchEnsure();
}

// Sidebar while searching: the sections that hold a match. A click opens that section and clears the search.
static void RenderSearchSidebar() {
	char found[64];
	sprintf_s(found, "%u in %u sections", (UInt32)s_searchTotal, (UInt32)s_searchGroups.size());
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::TextDisabled("%s", found);
	ImGui::PushStyleColor(ImGuiCol_Text, MenuWidgets::MutedText(0.3f));
	for (const NewGroup& g : s_searchGroups) {
		const bool isSwitch = NewEndsWith(g.Section, ".Status");
		const std::string label = g.Display + (isSwitch ? std::string(" (on / off)") : " (" + std::to_string(g.End - g.Begin) + ")");
		ImGui::PushID(g.Section.c_str());
		const bool clicked = ImGui::Selectable(label.c_str(), false);
		MenuWidgets::MarkLastItem(label.c_str(), NewGroupMarked(s_searchResults, g));	// 2026-11-25 (unsaved marks)
		if (clicked && !isSwitch) {
			gMenu.page = g.Section;
			s_searchBuf[0] = '\0';
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(isSwitch ? "The shader's on/off switch, shown on the right." : "Open this section (clears the search).");
		ImGui::PopID();
	}
	ImGui::PopStyleColor();
}

static void RenderSearchResults() {
	const std::string title = std::string("Search: ") + s_searchBuf;
	char counted[48];
	sprintf_s(counted, "%u setting%s", (UInt32)s_searchTotal, s_searchTotal == 1 ? "" : "s");
	PageTitle(title.c_str(), counted);
	if (!s_searchTotal) {
		ImGui::TextDisabled("No setting or section matches. Weather sections are not searched.");
		if (!DevOptionsShown()) ImGui::TextDisabled("Developer settings are hidden (View > Developer options).");
		return;
	}
	if (s_searchTotal > kSearchMax)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Showing the first %u: add a word to narrow it down.", (UInt32)kSearchMax);
	ImGui::PushID("##searchresults");
	NewRenderGroups(s_searchResults, s_searchGroups, false);
	ImGui::PopID();
}

