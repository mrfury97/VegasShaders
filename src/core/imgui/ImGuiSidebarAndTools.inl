// ---- The page -----------------------------------------------------------------------------------------------------------
// The search results, the New-in-patch page, or the selected section: title, help, the LUT pickers (LUT page) and the rows.
static void RenderContent() {
	if (SearchActive()) return RenderSearchResults();
	if (gMenu.page == kQuickTabId) return RenderQuickSettingsPage();
	if (gMenu.page == kUnderDevTabId) return RenderUnderDevelopmentPage();
	if (gMenu.page == kGearWeatherTabId) return RenderGearWeatherPage();
	if (gMenu.page == kAoTabId) return RenderIndirectLightingPage();
	if (gMenu.page == kRtTabId) return RenderRayTracingPage();
	if (gMenu.page == kDeferredTabId) return RenderDeferredRendererPage();
	if (gMenu.page == kDebugTabId) return RenderDebugSettingsPage();
	if (RenderSimpleCategoryPage()) return;
	if (gMenu.page.empty()) {
		ImGui::TextDisabled("Select a section from the left panel.");
		return;
	}
	SettingManager::Configuration::SettingList rows;
	TheSettingManager->FillMenuSettings(&rows, gMenu.page.c_str());
	if (rows.empty()) {
		ImGui::TextDisabled("No settings in this section.");
		return;
	}
	if (RenderGunPage()) return;	// 2026-11-16 (gunfx-builtin): one drop-down per gun effect
	if (RenderSimpleEffectPage()) return;	// 2026-11-05: Advanced off: the effect's pages as cards on one page
	PageOrder(rows);
	RenderPageHeader(gMenu.page);
	RenderSectionHelp(gMenu.page);	// UNOFFICIAL 2026-11-01: what the page is for, in plain words (kSectionHelp)
	if (gMenu.page == "Main.Main.Quality") {
		SettingManager::Configuration::SettingList filtered;
		filtered.reserve(rows.size());
		for (const auto& r : rows)
			if (!IsDeferredQualityKey(r.Key)) filtered.push_back(r);
		rows.swap(filtered);
		ImGui::TextDisabled("Deferred renderer settings have their own dedicated page.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Open Deferred Renderer...##qual_jump_def")) gMenu.page = kDeferredTabId;
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}
	if (gMenu.page == "Shaders.LUT.Main") RenderLutPage();
	RenderSettingsList(rows);
	RenderAttachedSections(gMenu.page);	// 2026-11-05
}

// ---- The sidebar ---------------------------------------------------------------------------------------------------------
// Top-level sections (Main, Shaders, ...) are small group labels; under them each entry is a full-width row that folds open.
// A shader's row carries its on/off switch. Pages (sections with no sub-sections) are plain rows that open on the right.

static void RenderSectionNode(const std::string& path, const std::string& name, bool parentIsShaders);

// 2026-11-10 (the effects list's folders): the row that was right-clicked this frame, a page's path or a folder's name. The menu that
// rearranges the list opens from them once the sidebar is drawn (LayoutMenu).
static std::string s_ctxPath, s_ctxFolder;
static void LayoutRowDragDrop(const std::string& path);	// (below, with the folders: a row can be dragged, and dropped on)
static void NoteRowRightClick(const std::string& path) {
	if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) s_ctxPath = path;
	LayoutRowDragDrop(path);
}

// A page row: muted unless it is the page shown.
static void DrawLeafSection(const std::string& path, const std::string& name) {
	const bool shown = gMenu.page == path;
	ImGui::PushStyleColor(ImGuiCol_Text, shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
	const std::string label = MenuSectionLabel(path, name);
	if (ImGui::Selectable(label.c_str(), shown)) gMenu.page = path;
	MenuWidgets::MarkLastItem(label.c_str(), SidebarMarked(path));	// 2026-11-25 (unsaved marks)
	NoteRowRightClick(path);
	if (shown && gMenu.revealPage) ImGui::SetScrollHereY(0.5f);	// test shots: the page's row in view
	ImGui::PopStyleColor();
}

// The folding part of a row: spans the sidebar's width (a click anywhere on it folds it) and lets the switch on top of it take
// its own clicks. Returns whether the row is open.
static bool FoldingRow(const std::string& path) {
	if (gMenu.revealPage && LeadsToSelected(path)) ImGui::SetNextItemOpen(true);
	const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
	ImGui::PushStyleColor(ImGuiCol_Text, MenuWidgets::MutedText(0.5f));	// the arrow
	const bool open = ImGui::TreeNodeEx("##node", flags);
	NoteRowRightClick(path);
	ImGui::PopStyleColor();
	ImGui::SameLine();
	return open;
}

// A shader's row: the fold arrow, its switch (or, with no switch of its own, just its name), "(forced)" when a preset locks it,
// and in debug mode the time it took last frame.
static bool DrawShaderEntryRow(const std::string& path, const std::string& name) {
	const char* shader = name.c_str();
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	const bool forced = TheSettingManager->IsShaderForced(shader);
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(shader);
	const std::string label = ShownText(MenuEntryLabel(name));

	const bool open = FoldingRow(path);
	if (kind == SettingManager::MenuSwitch::None) {
		// UNOFFICIAL 2026-10-19: no on/off setting (Foliage): a name only, not a switch that springs back
		ImGui::TextUnformatted(label.c_str());
		MenuWidgets::MarkLastItem(label.c_str(), SidebarMarked(path));	// 2026-11-25 (unsaved marks)
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s has no on/off switch of its own: the settings inside control it.", shader);
	}
	else {
		ImGui::BeginDisabled(forced);
		if (MenuWidgets::Toggle("##on", &on) && (!on || ConfirmEffectOn(name))) {	// (2026-11-05: conflicts ask first)
			TheShaderManager->SwitchShaderStatus(shader);
			TheSettingManager->LoadSettings();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.5f));
		ImGui::TextUnformatted(label.c_str());
		MenuWidgets::MarkLastItem(label.c_str(), SidebarMarked(path));	// 2026-11-25 (unsaved marks)
		ImGui::PopStyleColor();
		RestartBadge(("Shaders." + name + (kind == SettingManager::MenuSwitch::MainEnabled ? ".Main" : ".Status")).c_str(), "Enabled");
	}
	if (forced) {
		ImGui::SameLine();
		ImGui::TextDisabled("(forced)");
	}
	DrawPtTag(name);	// UNOFFICIAL 2026-11-12: "PT: replaced" and the like while path tracing replaces the scene (nothing otherwise)
	if (TheSettingManager->SettingsMain.Develop.DebugMode) {
		if (const EffectRecord* effect = TheShaderManager->GetEffectByName(shader)) {
			const float ms = TheSettingManager->SettingsMain.Main.RenderEffects ? ImMax(effect->renderTime + effect->constantUpdateTime, 0.0f) : 0.0f;
			ImGui::SameLine();
			ImGui::TextDisabled("%.3f ms", ms);
		}
	}
	return open;
}

// A section with sub-sections that is not a shader: the fold arrow and its name.
static bool DrawPlainSectionRow(const std::string& path, const std::string& name) {
	const bool open = FoldingRow(path);
	ImGui::TextUnformatted(name.c_str());
	MenuWidgets::MarkLastItem(name.c_str(), SidebarMarked(path));	// 2026-11-25 (unsaved marks)
	return open;
}

// The pages shown under a shader entry that is not theirs (kMenuMoves): rows that open the page's Main section.
static void DrawMovedPages(const std::string& path) {
	for (const MenuMove& move : kMenuMoves) {
		if (path != std::string("Shaders.") + move.under) continue;
		const std::string page = std::string("Shaders.") + move.name + ".Main";
		ImGui::PushID(page.c_str());
		DrawLeafSection(page, std::string(move.label) + "##" + move.name);
		ImGui::PopID();
	}
}

// What sits under an open entry: its sub-sections (less the shader entries shown elsewhere), the moved pages, and the shader
// entries nested under it (kMenuNests) with their own switches.
static void DrawEffectGroups(const StringList& children);

static void DrawChildren(const std::string& path) {
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	const bool shaders = path == "Shaders";
	if (shaders) return DrawEffectGroups(children);	// 2026-11-05: the effects in groups of their kind
	bool addedMainTabs = false;
	auto drawMainCustomTabs = [&]() {
		if (addedMainTabs) return;
		addedMainTabs = true;
		ImGui::PushID(kDeferredTabId);
		DrawLeafSection(kDeferredTabId, "Deferred renderer");
		ImGui::PopID();
		ImGui::PushID(kDebugTabId);
		DrawLeafSection(kDebugTabId, "Debug views");
		ImGui::PopID();
	};
	for (const std::string& child : children) {
		if (!(shaders && IsMovedShaderEntry(child)))	// UNOFFICIAL 2026-10-25: drawn under the entry it belongs with
			RenderSectionNode(path + "." + child, child, shaders);
		if (path == "Main.Main" && child == "Quality") drawMainCustomTabs();
	}
	if (path == "Main.Main") drawMainCustomTabs();
	DrawMovedPages(path);
	for (const MenuNest& nest : kMenuNests)	// UNOFFICIAL 2026-10-27
		if (path == std::string("Shaders.") + nest.under) RenderSectionNode(std::string("Shaders.") + nest.name, nest.name, true);
}

static bool HiddenFromSidebar(const std::string& path, const std::string& name) {
	if (ShouldHideSection(name)) return true;
	if (IsRtSection(path)) return true;	// 2026-11-05: on the Ray tracing page instead
	// 2026-11-05: SleepingMode hidden; GunFX lives in "Particles and gun effects"
	if (path == "Main.SleepingMode" || path == "Main.GunFX") return true;
	if (path == "Shaders.Debug" && !DevOptionsShown()) return true;
	// While Ambient Light is on in Simple mode, standalone AmbientOcclusion and BounceLight are superseded; switch modes on AO & Bounce Light or toggle Advanced
	if (!AdvancedShown() && EffectOn("AmbientLight") && (path == "Shaders.AmbientOcclusion" || path == "Shaders.BounceLight")) return true;
	// (user: camera mode and fly camera under Main > Misc; frame rate options under Performance "if they do anything at all": they do
	// not: FrameRateManager::PerformSync is never called and the Stuttering pair is read by nothing)
	if (path == "Main.CameraMode" || path == "Main.FlyCam" || path == "Main.Transitions" || path == "Main.FrameRate") return true;
	if (path == "Main.Main.Precipitations") return true;	// (user: "remove the precipitations menu from main"; the setting stays)
	if (InternalHidden(path, std::string())) return true;	// UNOFFICIAL 2026-11-12: a "Section.*" Internal entry (the page would be empty)
	return !DevOptionsShown() && IsDevOnlySetting(path, std::string());	// UNOFFICIAL 2026-10-25: a "Section.*" DevOnly entry
}

static void RenderSectionNode(const std::string& path, const std::string& name, bool parentIsShaders) {
	if (HiddenFromSidebar(path, name)) return;
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	ImGui::PushID(path.c_str());	// every ID inside belongs to this path
	if (children.empty()) {
		DrawLeafSection(path, name);
	}
	else if (parentIsShaders ? DrawShaderEntryRow(path, name) : DrawPlainSectionRow(path, name)) {
		DrawChildren(path);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

// 2026-11-05 (user: "organize all of the effects into groups of similar effects with collapsible menus", and the Ray tracing page
// "in the list with the rest of the effects"; docs/next/SETTINGS-GAME-MENU-PLAN.md): the Shaders list as folding groups of effects of a
// kind, as a game's graphics menu groups them. Only the sidebar changes: each effect keeps its row, switch and pages. An effect no
// group names goes under "Other", so a new one never goes missing. Ray tracing is a row of its own here (the page above).
// 2026-11-10: these are the BUILT-IN folders. The player can make, rename, reorder and delete folders and move effects between them
// (MenuLayout.h; right-click a row); with no arrangement of their own the list is exactly this table.
struct EffectGroup { const char* Name; const char* Entries; const char* Page = nullptr; const char* PageLabel = nullptr; };	// Page: a settings page that is no effect entry
static const EffectGroup kEffectGroups[] = {
	{ "Lighting", "@IndirectLighting, AmbientLight, AmbientOcclusion, BounceLight, Specular, PBR, Reflections, Flashlight" },
	{ "Shadows", "ShadowsExteriors, ShadowsInteriors" },
	{ "Ray tracing", nullptr },
	{ "Sky and atmosphere", "Sky, VolumetricClouds, VolumetricFog, VolumetricLight, GodRays, SunGlare, Sandstorm, HeatHaze" },
	{ "Weather and water", "Precipitations, Snow, SnowAccumulation, WetnessEffects, Puddles, WetWorld, Water, Underwater, WaterLens" },
	{ "Ground and plants", "Terrain, POM, Grass, Plants" },
	{ "Characters", "Skin, Cloth, PipboyFit, HandBlood" },
	{ "Camera and colour", "Tonemapping, Exposure, HDR, Bloom, BloomLegacy, Lens, LUT, Coloring, ImageAdjust, Cinema, Bodycam, DepthOfField, MotionBlur, LowHF" },
	{ "Anti-aliasing and sharpening", "TAA, SMAA, Sharpening, DitherBuster, SkyDither" },
	{ "Particles and gun effects", "Particles, GPUParticles", "Main.GunFX.Main", "Gun effects (smoke, barrel glow)##GunFX" },
	{ "Under Development", "WorldSurface, @GearWeather" },
	// "fun" looks that are no part of a normal setup, at the bottom where they stay out of the way (off by default)
	{ "Extras", "Dream" },
	{ "Developer", "Debug" },
};

// ---- The effects list's folders (2026-11-10) ----------------------------------------------------------------------------------
// User: "our folder system with the shaders is the default, but our users can make, rearrange, or delete folders if they want and their
// arrangement is saved". The folders above are the start; MenuLayout holds the list as it is now (the player's file, or the table).
// An entry is an effect's name or one of two fixed pages: the Ray tracing pages (one entry, drawn as their rows) and Gun effects.
// Right-click a folder or an effect's row: move to a folder, up / down, new folder, rename, delete, back to the built-in folders.
// Drag (user: "drag the folders and effects into each other for ease of access"): an effect onto a folder puts it in that folder, an
// effect onto an effect puts it at that place, a folder onto a folder moves it to that place. A drop only notes what to do: the list
// is changed once the sidebar is drawn (LayoutMenu), never while its rows are being walked.
static const char* const kLayoutRt = "@RayTracing";
static const char* const kLayoutGun = "@GunFX";
static const char* const kLayoutAo = "@IndirectLighting";
static const char* const kLayoutGearWeather = "@GearWeather";
static bool s_layoutDirty = true;		// entries may be missing from the folders (first use, or after an edit)
static size_t s_layoutChildren = 0;		// how many effects the list had when the folders were last checked
static std::string s_layoutEntry, s_layoutFolder;	// what the open right-click menu is about (one of them)
static char s_layoutNewName[48] = "", s_layoutRename[48] = "";
static std::string s_dropEntry, s_dropFolder;	// what was dropped this frame (an entry, or a folder) ...
static std::string s_dropOnEntry, s_dropOnFolder;	// ... and on what

static const EffectGroup* GunPageGroup() {
	for (const EffectGroup& g : kEffectGroups) if (g.Page) return &g;
	return nullptr;
}

// The built-in folder of an entry ("Other" for an effect no group names, so a new one never goes missing).
static const char* DefaultFolderOf(const std::string& entry) {
	for (const EffectGroup& g : kEffectGroups) {
		if (!g.Entries) { if (entry == kLayoutRt) return g.Name; }
		else if (KeyInList(g.Entries, entry.c_str())) return g.Name;
		if (g.Page && entry == kLayoutGun) return g.Name;
	}
	return "Other";
}

static void LayoutDefaults() {
	std::vector<MenuLayout::Folder>& folders = MenuLayout::S.folders;
	folders.clear();
	for (const EffectGroup& g : kEffectGroups) {
		MenuLayout::Folder f;
		f.name = g.Name;
		if (!g.Entries) f.entries.push_back(kLayoutRt);
		else for (const char* p = g.Entries; *p; ) {	// in the group's own order
			while (*p == ' ' || *p == ',') p++;
			const char* end = p;
			while (*end && *end != ',') end++;
			if (end > p) f.entries.push_back(std::string(p, end));
			p = end;
		}
		if (g.Page) f.entries.push_back(kLayoutGun);
		folders.push_back(f);
	}
}

// Puts an entry no folder holds into its built-in folder, or into "Other" when that folder is gone.
static bool LayoutPlace(const std::string& entry) {
	if (MenuLayout::FolderOf(entry)) return false;
	MenuLayout::Folder* f = MenuLayout::Find(DefaultFolderOf(entry));
	if (!f) f = MenuLayout::Find("Other");
	if (!f) {
		MenuLayout::S.folders.push_back({ "Other", {} });
		f = &MenuLayout::S.folders.back();
	}
	f->entries.push_back(entry);
	return true;
}

// Reads the player's file the first time (none: the built-in folders), and keeps every effect of the list in some folder.
static void LayoutEnsure(const StringList& children) {
	MenuLayout::State& layout = MenuLayout::S;
	if (!layout.loaded) {
		layout.loaded = true;
		layout.path = Paths::ModuleDir() + "VegasShaders.menu.toml";
		if (MenuLayout::Load())
			Logger::Log("UNOFFICIAL Vegas Shaders menu: the effects list uses your own folders (%d, from %s)", (int)layout.folders.size(), layout.path.c_str());
		else LayoutDefaults();
		s_layoutDirty = true;
	}
	if (!s_layoutDirty && s_layoutChildren == children.size()) return;
	s_layoutDirty = false;
	s_layoutChildren = children.size();
	bool placed = false;
	for (const std::string& child : children)
		if (!IsMovedShaderEntry(child)) placed = LayoutPlace(child) || placed;
	placed = LayoutPlace(kLayoutAo) || placed;
	placed = LayoutPlace(kLayoutRt) || placed;
	placed = LayoutPlace(kLayoutGun) || placed;
	placed = LayoutPlace(kLayoutGearWeather) || placed;
	if (placed && layout.custom) MenuLayout::Save();
}

// After any change the player makes: it is their own arrangement from now on, written at once.
static void LayoutEdited() {
	MenuLayout::S.custom = true;
	if (!MenuLayout::Save()) Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders menu: cannot write %s (the folders stay as arranged until the game closes)", MenuLayout::S.path.c_str());
	s_layoutDirty = true;
}

// Deleting a folder: its effects go back to their built-in folders (or "Other" when that is the folder deleted, or gone).
static void LayoutDeleteFolder(const std::string& name) {
	std::vector<MenuLayout::Folder>& folders = MenuLayout::S.folders;
	for (size_t i = 0; i < folders.size(); i++) {
		if (folders[i].name != name) continue;
		const std::vector<std::string> entries = folders[i].entries;
		folders.erase(folders.begin() + i);
		for (const std::string& e : entries) LayoutPlace(e);
		return;
	}
}

static void RtSwitchCount(int& on, int& switches) {
	static const char* const kOnKeys[][2] = { { "Shaders.ShadowsExteriors.RT", "Enabled" }, { "Shaders.ShadowsInteriors.RT", "Enabled" },
		{ "Shaders.AmbientOcclusion.RT", "Enabled" }, { "Shaders.AmbientOcclusion.RT", "GI" },
		{ "Shaders.Reflections.RT", "Enabled" }, { "Main.Main.RayTracing", "PTRealtime" } };
	for (const auto& k : kOnKeys) if (RtOn(k[0], k[1]) || (!strcmp(k[1], "PTRealtime") && RtPathTrace::ReferenceOn())) on++;
	switches += (int)_countof(kOnKeys);
	if (RtOn("Shaders.AmbientOcclusion.Skylighting", "Enabled") && RtOn("Shaders.AmbientOcclusion.RT", "Sky")) on++;
	switches++;
}

// One folder's row: its name and how many of its effects are on; open while a test shot shows one of its pages.
static bool DrawEffectGroupRow(const MenuLayout::Folder& folder, const std::vector<std::string>& effects, bool rayTracing) {
	int on = 0, switches = 0;
	bool reveal = false;
	for (const std::string& e : effects) {
		if (TheSettingManager->GetMenuShaderSwitch(e.c_str()) != SettingManager::MenuSwitch::None) {
			switches++;
			if (TheSettingManager->GetMenuShaderEnabled(e.c_str())) on++;
		}
		reveal = reveal || (gMenu.revealPage && LeadsToSelected("Shaders." + e));
	}
	if (rayTracing) {
		RtSwitchCount(on, switches);
		reveal = reveal || (gMenu.revealPage && (gMenu.page == kRtTabId || IsRtSection(gMenu.page)));
	}
	if (reveal) ImGui::SetNextItemOpen(true);
	bool marked = false;	// 2026-11-25 (unsaved marks): one of its entries leads to an unsaved setting
	for (const std::string& e : effects) marked = marked || SidebarMarked("Shaders." + e);
	const EffectGroup* gun = GunPageGroup();
	for (const std::string& e : folder.entries) {
		reveal = reveal || (gMenu.revealPage && e == kLayoutGearWeather && gMenu.page == kGearWeatherTabId);
		marked = marked || (e == kLayoutRt && SidebarMarked(kRtTabId)) || (e == kLayoutAo && SidebarMarked(kAoTabId)) ||
			(e == kLayoutGun && gun && SidebarMarked(gun->Page)) || (e == kLayoutGearWeather && SidebarMarked(kGearWeatherTabId));
	}
	const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
	const std::string label = folder.name + "###effectgroup" + folder.name;
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
	MenuWidgets::MarkLastItem(label.c_str(), marked);
	if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) s_ctxFolder = folder.name;
	if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
		ImGui::SetDragDropPayload("VSFOLDER", folder.name.c_str(), folder.name.size() + 1);
		ImGui::Text("Folder: %s", folder.name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("VSENTRY")) { s_dropEntry = (const char*)p->Data; s_dropOnFolder = folder.name; }
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("VSFOLDER")) { s_dropFolder = (const char*)p->Data; s_dropOnFolder = folder.name; }
		ImGui::EndDragDropTarget();
	}
	if (switches) {
		ImGui::SameLine();
		ImGui::TextDisabled("%d of %d on", on, switches);
	}
	else if (folder.entries.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("empty");
	}
	return open;
}

// The Ray tracing pages (2026-11-05): the overview (status, one switch, debug views) and the five RT sections' own pages, which have no
// effect entry of their own in the list. One entry of the folders: they move together.
static void DrawRayTracingRows() {
	const bool shown = gMenu.page == kRtTabId;
	ImGui::PushID(kRtTabId);
	ImGui::PushStyleColor(ImGuiCol_Text, shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
	const char* const label = "Ray tracing: overview (status, one switch, debug views)";
	if (ImGui::Selectable(label, shown)) gMenu.page = kRtTabId;
	MenuWidgets::MarkLastItem(label, SidebarMarked(kRtTabId));	// 2026-11-25 (unsaved marks)
	NoteRowRightClick(kRtTabId);
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every ray-traced effect in one place: what runs, what stops it, one switch for all and the debug views.");
	ImGui::PopID();
	for (const RtSection& section : kRtSections) {
		ImGui::PushID(section.Section);
		DrawLeafSection(section.Section, std::string(section.Title) + "##" + section.Section);
		ImGui::PopID();
	}
}

static void DrawEffectGroups(const StringList& children) {
	LayoutEnsure(children);
	auto shown = [](const std::string& child) { return !IsMovedShaderEntry(child) && !HiddenFromSidebar("Shaders." + child, child); };
	const EffectGroup* gun = GunPageGroup();
	for (const MenuLayout::Folder& folder : MenuLayout::S.folders) {
		std::vector<std::string> effects;	// the folder's effects this install has and shows
		bool rayTracing = false, pages = false;
		for (const std::string& e : folder.entries) {
			if (e == kLayoutRt) rayTracing = pages = true;
			else if (e == kLayoutAo) pages = true;
			else if (e == kLayoutGearWeather) pages = true;
			else if (e == kLayoutGun) pages = pages || gun;
			else if (std::find(children.begin(), children.end(), e) != children.end() && shown(e)) effects.push_back(e);
		}
		if (!folder.entries.empty() && effects.empty() && !pages) continue;	// (only hidden effects, as Developer > Debug; an EMPTY folder shows: something can be moved in)
		if (!DrawEffectGroupRow(folder, effects, rayTracing)) continue;
		for (const std::string& e : folder.entries) {
			if (e == kLayoutRt) DrawRayTracingRows();
			else if (e == kLayoutAo) {
				const bool isShown = gMenu.page == kAoTabId;
				ImGui::PushID(kAoTabId);
				ImGui::PushStyleColor(ImGuiCol_Text, isShown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
				if (ImGui::Selectable("Ambient occlusion & bounce light", isShown)) gMenu.page = kAoTabId;
				MenuWidgets::MarkLastItem("Ambient occlusion & bounce light", SidebarMarked(kAoTabId));	// 2026-11-25 (unsaved marks)
				NoteRowRightClick(kAoTabId);
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unified picker for corner shading (AO), sky occlusion, and bounce light (GI).");
				ImGui::PopID();
			}
			else if (e == kLayoutGearWeather) {
				const bool isShown = gMenu.page == kGearWeatherTabId;
				ImGui::PushID(kGearWeatherTabId);
				ImGui::PushStyleColor(ImGuiCol_Text, isShown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
				const char* const label = "Gear weather (1st-person)";
				if (ImGui::Selectable(label, isShown)) gMenu.page = kGearWeatherTabId;
				MenuWidgets::MarkLastItem(label, SidebarMarked(kGearWeatherTabId));
				NoteRowRightClick(kGearWeatherTabId);
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("First-person weapon and sleeve weathering (frost, snowflakes, sandstorm dust, rain wetness, mud).");
				ImGui::PopID();
			}
			else if (e == kLayoutGun) {
				if (!gun) continue;
				ImGui::PushID(gun->Page);
				DrawLeafSection(gun->Page, gun->PageLabel);
				ImGui::PopID();
			}
			else if (std::find(effects.begin(), effects.end(), e) != effects.end()) RenderSectionNode("Shaders." + e, e, true);
		}
		ImGui::TreePop();
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Drag a folder or an effect to move it. Right-click one for more (new folder, rename, delete, reset).");
	ImGui::PopTextWrapPos();
}

// The folders' entry a right-clicked row belongs to ("" = none: a page outside the effects list).
static std::string LayoutEntryOfPath(const std::string& path) {
	if (path == kRtTabId || IsRtSection(path)) return kLayoutRt;
	if (path == kAoTabId) return kLayoutAo;
	if (path == kGearWeatherTabId) return kLayoutGearWeather;
	const EffectGroup* gun = GunPageGroup();
	if (gun && path == gun->Page) return kLayoutGun;
	if (path.compare(0, 8, "Shaders.") != 0) return std::string();
	const std::string effect = path.substr(8, path.find('.', 8) == std::string::npos ? std::string::npos : path.find('.', 8) - 8);
	return MenuLayout::FolderOf(effect) ? effect : std::string();	// (an effect nested under another's entry is not in the folders)
}

static std::string LayoutEntryLabel(const std::string& entry) {
	if (entry == kLayoutRt) return "Ray tracing pages";
	if (entry == kLayoutAo) return "Ambient occlusion & bounce light";
	if (entry == kLayoutGearWeather) return "Gear weather (1st-person)";
	if (entry == kLayoutGun) return "Gun effects";
	return ShownText(MenuEntryLabel(entry));
}

// A row of the list as something to drag and to drop on (called for the row just drawn). An effect's own row, the Gun effects page and
// the Ray tracing overview can be dragged; any row of an effect (its pages too) takes a drop for that effect's place.
static void LayoutRowDragDrop(const std::string& path) {
	if (!MenuLayout::S.loaded) return;
	if (!ImGui::IsItemActive() && !(ImGui::IsDragDropActive() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) return;
	const std::string entry = LayoutEntryOfPath(path);
	if (entry.empty()) return;
	const EffectGroup* gun = GunPageGroup();
	const bool own = path == "Shaders." + entry || path == kRtTabId || path == kAoTabId || path == kGearWeatherTabId || (gun && path == gun->Page);
	if (own && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
		ImGui::SetDragDropPayload("VSENTRY", entry.c_str(), entry.size() + 1);
		ImGui::TextUnformatted(LayoutEntryLabel(entry).c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("VSENTRY")) { s_dropEntry = (const char*)p->Data; s_dropOnEntry = entry; }
		ImGui::EndDragDropTarget();
	}
}

// The right-click menu, and what was dropped this frame. Called once the sidebar is drawn, at the same place of the ID stack every frame.
static void LayoutMenu() {
	if (!s_dropEntry.empty() || !s_dropFolder.empty()) {
		bool moved = false;
		if (!s_dropFolder.empty()) moved = MenuLayout::DropFolderOn(s_dropFolder, s_dropOnFolder);
		else if (!s_dropOnEntry.empty()) moved = MenuLayout::DropEntryOn(s_dropEntry, s_dropOnEntry);
		else if (!s_dropOnFolder.empty()) {
			const MenuLayout::Folder* in = MenuLayout::FolderOf(s_dropEntry);
			moved = in && in->name != s_dropOnFolder && MenuLayout::MoveEntry(s_dropEntry, s_dropOnFolder);
		}
		s_dropEntry.clear(); s_dropFolder.clear(); s_dropOnEntry.clear(); s_dropOnFolder.clear();
		if (moved) LayoutEdited();
	}
	if (!s_ctxPath.empty() || !s_ctxFolder.empty()) {
		s_layoutEntry = s_ctxFolder.empty() ? LayoutEntryOfPath(s_ctxPath) : std::string();
		s_layoutFolder = s_ctxFolder;
		s_ctxPath.clear();
		s_ctxFolder.clear();
		if (MenuLayout::S.loaded && (!s_layoutEntry.empty() || !s_layoutFolder.empty())) {
			s_layoutNewName[0] = 0;
			strncpy_s(s_layoutRename, s_layoutFolder.c_str(), _TRUNCATE);
			ImGui::OpenPopup("##effectsLayout");
		}
	}
	if (!ImGui::BeginPopup("##effectsLayout")) return;
	bool edited = false;
	if (!s_layoutEntry.empty()) {
		const MenuLayout::Folder* in = MenuLayout::FolderOf(s_layoutEntry);
		ImGui::TextDisabled("%s", LayoutEntryLabel(s_layoutEntry).c_str());
		ImGui::Separator();
		if (ImGui::BeginMenu("Move to folder")) {
			for (size_t i = 0; i < MenuLayout::S.folders.size(); i++) {
				const std::string name = MenuLayout::S.folders[i].name;	// (a copy: the move below changes the list)
				if (ImGui::MenuItem((name + "##to" + std::to_string(i)).c_str(), nullptr, false, !in || in->name != name)) {
					edited = MenuLayout::MoveEntry(s_layoutEntry, name);
					break;
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Move up")) edited = MenuLayout::ShiftEntry(s_layoutEntry, -1) || edited;
		if (ImGui::MenuItem("Move down")) edited = MenuLayout::ShiftEntry(s_layoutEntry, +1) || edited;
	}
	else {
		ImGui::TextDisabled("Folder: %s", s_layoutFolder.c_str());
		ImGui::Separator();
		if (ImGui::MenuItem("Move up")) edited = MenuLayout::ShiftFolder(s_layoutFolder, -1) || edited;
		if (ImGui::MenuItem("Move down")) edited = MenuLayout::ShiftFolder(s_layoutFolder, +1) || edited;
		if (ImGui::BeginMenu("Rename")) {
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
			ImGui::InputText("##rename", s_layoutRename, sizeof(s_layoutRename));
			ImGui::SameLine();
			if (ImGui::Button("Rename")) {
				if (MenuLayout::RenameFolder(s_layoutFolder, s_layoutRename)) { edited = true; ImGui::CloseCurrentPopup(); }
			}
			ImGui::TextDisabled("A name no other folder has.");
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Delete folder")) {
			ImGui::TextDisabled("Its effects go back to their built-in folders (or \"Other\"). No setting changes.");
			if (ImGui::MenuItem("Delete")) { LayoutDeleteFolder(s_layoutFolder); edited = true; }
			ImGui::EndMenu();
		}
	}
	ImGui::Separator();
	if (ImGui::BeginMenu(s_layoutEntry.empty() ? "New folder" : "New folder with this in it")) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
		ImGui::InputText("##newfolder", s_layoutNewName, sizeof(s_layoutNewName));
		ImGui::SameLine();
		if (ImGui::Button("Create")) {
			if (MenuLayout::AddFolder(s_layoutNewName)) {
				if (!s_layoutEntry.empty()) MenuLayout::MoveEntry(s_layoutEntry, MenuLayout::Clean(s_layoutNewName));
				edited = true;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::TextDisabled("A name no other folder has.");
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Reset to the built-in folders", MenuLayout::S.custom)) {
		ImGui::TextDisabled("Removes your folders and their order (VegasShaders.menu.toml). No setting changes.");
		if (ImGui::MenuItem("Reset")) {
			MenuLayout::RemoveFile();
			MenuLayout::S.custom = false;
			LayoutDefaults();
			s_layoutDirty = true;
		}
		ImGui::EndMenu();
	}
	ImGui::EndPopup();
	if (edited) LayoutEdited();
}

// ---- Unsaved marks: the sidebar (2026-11-25) ------------------------------------------------------------------------------
// An entry is marked when what it opens holds an unsaved setting: a page's row by the settings its page shows, a fold (a group label,
// a section's or an effect's row, a folder) by the entries under it. Once a frame each unsaved key is followed to the entries that lead
// to it (its trail), by the sidebar's own rules: what HiddenFromSidebar leaves out, the moved and nested entries (kMenuMoves,
// kMenuNests), the sections drawn inside another page (kPageAttaches), and the pages that gather settings from many sections. Those
// pages' lists below follow what the pages draw (RenderQuickSettingsPage, RenderIndirectLightingPage, RenderDeferredRendererPage,
// RenderDebugSettingsPage, RenderCat*): a row added to one of those pages belongs in its list here too.
// (In Simple mode a card or a category can be marked for a setting only Advanced shows: one changed with Advanced on, not saved yet.)

// The Graphics categories of Simple mode (RenderCat*): their mode pickers and their cards (each card's effect switch and its sections).
struct CategoryScope { const char* Id; const char* Scope; };
static const CategoryScope kCategoryScopes[] = {
	{ "@Cat.Lighting", "@AmbientLight, @AmbientOcclusion, @BounceLight, @PBR, @Specular, @Reflections, @Flashlight, Shaders.AmbientLight.Main, "
		"Shaders.AmbientOcclusion.Exteriors, Shaders.AmbientOcclusion.Interiors, Shaders.AmbientOcclusion.GTAO, Shaders.AmbientOcclusion.RT, "
		"Shaders.AmbientOcclusion.Skylighting, Shaders.BounceLight.Main, Shaders.PBR.Main, Shaders.PBR.Metal, Shaders.PBR.Rmaos, Shaders.PBR.Linear, "
		"Shaders.Specular.Exterior, Shaders.Specular.Rain, Shaders.Reflections.Main, Shaders.Reflections.RT, Shaders.Flashlight.Main, "
		"Shaders.Flashlight.Deferred" },
	{ "@Cat.Shadows", "@ShadowsExteriors, @ContactHardening, @CloudShadows, @ShadowsInteriors, @VolumetricClouds, Shaders.VolumetricClouds.Main:Shadows, "
		"Shaders.ShadowsExteriors.Main, Shaders.ShadowsExteriors.ScreenSpace, Shaders.ShadowsExteriors.SSShadows, Shaders.ShadowsExteriors.TerrainShadows, Shaders.ShadowsExteriors.RT, "
		"Shaders.ContactHardening.Main, Shaders.CloudShadows.Main, Shaders.ShadowsInteriors.Main, Shaders.ShadowsInteriors.Forward, "
		"Shaders.ShadowsInteriors.Atlas, Shaders.ShadowsInteriors.RT" },
	{ "@Cat.Sky", "@Sky, @VolumetricClouds, @VolumetricFog, @VolumetricLight, @GodRays, @SunGlare, @Sandstorm, @HeatHaze, @DustMotes, "
		"Shaders.Sky.Main, Shaders.Sky.Physical, Shaders.Sky.Clouds, Shaders.VolumetricClouds.Main, Shaders.VolumetricFog.Main, Shaders.VolumetricFog.Night, "
		"Shaders.VolumetricFog.Interiors, Shaders.VolumetricLight.Main, Shaders.VolumetricLight.Night, Shaders.GodRays.Main, Shaders.GodRays.Coloring, "
		"Shaders.SunGlare.Main, Shaders.Sandstorm.Main, Shaders.HeatHaze.Main, Shaders.DustMotes.Main" },
	{ "@Cat.Weather", "@Precipitations, @Snow, @SnowAccumulation, @WetnessEffects, @Puddles, @WetWorld, @Water, @Underwater, @WaterLens, "
		"Shaders.Precipitations.Main, Shaders.Snow.Main, Shaders.SnowAccumulation.Main, Shaders.WetnessEffects.Main, Shaders.WetnessEffects.Raindrops, "
		"Shaders.WetnessEffects.HudRaindrops, Shaders.Puddles.Main, Shaders.WetWorld.Main, Shaders.Water.Default, Shaders.Water.Interiors, "
		"Shaders.Water.Placed, Shaders.WaterLens.Main, Main.Main.Quality:GearWeather, Main.Main.Quality:GearWeatherStrength, "
		"Main.Main.Quality:GearWeatherFrost, Main.Main.Quality:GearWeatherSand, Main.Main.Quality:GearWeatherWet, Main.Main.Quality:GearWeatherMud, "
		"Main.Main.Quality:GearWeatherSpeed, Main.Main.Quality:GearWeatherSkin, Main.Main.Quality:GearWeatherScale, Main.Main.Quality:GearWeatherForce" },	// test4: + the gear weather card
	{ "@Cat.Ground", "@Terrain, @WorldSurface, @POM, @Grass, @Plants, @Foliage, Shaders.WorldSurface.Main, Shaders.WorldSurface.Dust, "
		"Shaders.WorldSurface.Rust, Shaders.WorldSurface.Waterline, Shaders.WorldSurface.Moss, Shaders.Terrain.Blending, Shaders.Terrain.Variation, Shaders.Terrain.Macro, "
		"Shaders.Terrain.Parallax, Shaders.Terrain.Displacement, Shaders.POM.Main, Shaders.POM.Displacement, Shaders.Grass.Main, Shaders.Grass.GPUGrass, "
		"Shaders.Grass.Wind, Shaders.Grass.Interaction, Shaders.Plants.Wind, Shaders.Foliage.Main" },
	{ "@Cat.Characters", "@Skin, @Cloth, @PipboyFit, @HandBlood, Shaders.Skin.Main, Shaders.Skin.Scattering, Shaders.Skin.Interiors, Shaders.Cloth.Main, "
		"Shaders.Cloth.Interiors, Shaders.PipboyFit.Main, Shaders.HandBlood.Main" },
	{ "@Cat.Camera", "@Tonemapping, @Exposure, @LocalExposure, @HDR, @Bloom, @BloomLegacy, @LUT, @Coloring, @ImageAdjust, @Lens, @DepthOfField, "
		"@MotionBlur, @Cinema, @Bodycam, @LowHF, @Dream, Shaders.Tonemapping.Main, Shaders.Tonemapping.Interiors, Shaders.Tonemapping.Night, "
		"Shaders.Exposure.Main, Shaders.Exposure.Interiors, Shaders.Exposure.Night, Shaders.LocalExposure.Main, Shaders.HDR.Main, Shaders.Bloom.Main, "
		"Shaders.Bloom.Night, Shaders.Bloom.Interiors, Shaders.BloomLegacy.Exteriors, Shaders.BloomLegacy.Interiors, Shaders.LUT.Main, "
		"Shaders.Coloring.Default, Shaders.ImageAdjust.Main, Shaders.ImageAdjust.Night, Shaders.ImageAdjust.Interiors, Shaders.Lens.Main, "
		"Shaders.Lens.Night, Shaders.Lens.Interiors, Shaders.DepthOfField.FirstPersonView, Shaders.DepthOfField.ThirdPersonView, "
		"Shaders.MotionBlur.FirstPersonView, Shaders.MotionBlur.ThirdPersonView, Shaders.Cinema.Main, Shaders.Bodycam.Main, Shaders.Bodycam.Night, "
		"Shaders.Bodycam.Interiors, Shaders.LowHF.Main, Shaders.Dream.Main" },
	{ "@Cat.AA", "Main.Main.ReducedQuality:FXAA, @SMAA, @TAA, @Sharpening, @DitherBuster, @SkyDither, Shaders.TAA.FSR, Shaders.TAA.DLSS, "
		"Shaders.TAA.Main, Shaders.TAA.Cutouts, Shaders.SMAA.Main, Shaders.Sharpening.Main, Shaders.Sharpening.Interiors, Shaders.DitherBuster.Main, "
		"Shaders.SkyDither.Main" },
	{ "@Cat.Particles", "@Particles, @GPUParticles, Shaders.Particles.Main, Shaders.GPUParticles.Main, Main.GunFX" },
};	// (the Ray tracing category is the Ray tracing page: kRtTabId)

// Quick settings: its rows besides the AA and AO / sky / bounce light pickers (the ray tracing ones only with the DXVK fork).
static const char* const kScopeQuickPage = "Main.Main.Misc:RenderEffects, Main.Main.Quality:MaxQuality, "
	"Shaders.TAA.FSR:RenderScale, Shaders.TAA.DLSS:RenderScale";
static const char* const kScopeQuickPageRt = "Main.Main.RayTracing:RTInteriors, Main.Main.RayTracing:RTExteriors, Main.Main.RayTracing:PTRealtime, "
	"Main.Main.RayTracing:PTRealtimeLevel, Main.Main.RayTracing:PTStandDownRT";

// AO & bounce light: its tuning blocks, all modes together (each mode shows its own).
static const std::vector<VisualItem> kAoPageItems = {
	{ "Shaders.AmbientLight.Main", "OcclusionStrength, Radius, DrawDistance, DarkestOcclusion, BrightnessFade, ComputeGIStrength, ComputeGIQuality, "
		"ComputeGIThickness, ComputeGIThinOcclusion, SunGI, RoomGI, AmbientReplace, AmbientFloor, AmbientReplaceInterior, AmbientFloorInterior, "
		"GridBounce, GridLamps, GridSpread, GridColour", nullptr },
	{ "Shaders.AmbientOcclusion.Exteriors", "Enabled, StrengthMultiplier, Range, ClampStrength, LumThreshold", nullptr },
	{ "Shaders.AmbientOcclusion.Interiors", "Enabled, StrengthMultiplier, Range, ClampStrength, LumThreshold", nullptr },
	{ "Shaders.AmbientOcclusion.GTAO", "Enabled, MaxRadius", nullptr },
	{ "Shaders.AmbientOcclusion.RT", "RayCount, Distance, OcclusionOnD3D9, SkyRays, SkyDistance, GIStrength, GIDistance, GISky, SkyMissSH, GIAmbient, "
		"RadianceCache, CacheExteriors, GIStrengthNight, GIStrengthInterior, GlowGI, GlowDirect", nullptr },
	{ "Shaders.AmbientOcclusion.Skylighting", "Strength, Distance, Reflections", nullptr },
	{ "Shaders.BounceLight.Main", "Strength, Radius, BlurRadius, QuarterResolution, Temporal", nullptr },
};

// Deferred renderer: the deferred sun pass's keys it shows with Developer options (its Main.Main.Quality keys: IsDeferredQualityKey).
static const VisualItem kDeferredPageSunKeys = { "Shaders.ShadowsExteriors.Deferred",
	"MatchSunDisc, CanopyShadow, ParallaxShadow, LandCrackShadow, MicroShadow, AuthoredHighlight, HighlightFade, Shadows", nullptr };

// Debug views: every view of kDebugViews but one, and these.
static const std::vector<VisualItem> kDebugPageExtras = {
	{ "@effect", "Debug", nullptr },
	{ "Main.Develop.Main", "DebugMode, GpuDebugLabels, DebugVar1, DebugVar2, DebugVar3, DebugVar4", nullptr },
	{ "Main.Develop.PixelInspector", "Enabled, X, Y, LogEvery", nullptr },
	{ "Main.GunFX.Volume", "Debug", nullptr },
	{ "Shaders.AmbientLight.Main", "DebugGIExposure", nullptr },
	{ "Shaders.WetnessEffects.Debug", "WetnessOverride, PuddleOverride, RainOverride", nullptr },
};

static bool DebugPageHas(const std::string& section, const std::string& key) {
	for (const VisualItem& item : kDebugPageExtras) if (VisualItemHas(item, section, key)) return true;
	for (const DebugViewKey& v : kDebugViews)
		if (section == v.Section && key == v.Key) return !(section == "Shaders.Terrain.Blending" && key == "Debug");	// (the one the page leaves out)
	return false;
}

static bool NewPageHas(const std::string& section, const std::string& key) {
	for (const NewEntry& e : s_newMain) if (e.Section == section && e.Key == key) return true;
	if (DevOptionsShown()) for (const NewEntry& e : s_newDiag) if (e.Section == section && e.Key == key) return true;
	return false;
}

// The entries of the sidebar's tree (Main, and Shaders with Advanced on) that lead to where [section] key is drawn, top first; none
// when the tree does not show it.
static void TreeTrail(const std::string& section, const std::string& key, std::vector<std::string>& trail) {
	for (const PageAttach& a : kPageAttaches)	// drawn inside its host's page
		if (section == a.Section) return TreeTrail(a.Host, key, trail);
	if (IsRtSection(section)) { trail = { "Shaders", section }; return; }	// a row of the Ray tracing pages (their overview: below)
	const EffectGroup* gun = GunPageGroup();
	if (gun && section.compare(0, 11, "Main.GunFX.") == 0) { trail = { "Shaders", gun->Page }; return; }	// the Gun effects page
	if (section == "Main.Main.Quality" && IsDeferredQualityKey(key.c_str())) { trail = { "Main", "Main.Main", kDeferredTabId }; return; }
	if (section.compare(0, 8, "Shaders.") == 0) {
		const size_t dot = section.find('.', 8);
		const std::string effect = section.substr(8, dot == std::string::npos ? std::string::npos : dot - 8);
		for (const MenuMove& m : kMenuMoves)
			if (effect == m.name) { trail = { "Shaders", std::string("Shaders.") + m.under, "Shaders." + effect + ".Main" }; return; }
		for (const MenuNest& n : kMenuNests)
			if (effect == n.name) { trail = { "Shaders", std::string("Shaders.") + n.under, "Shaders." + effect }; return; }
	}
	// Every section above it, down to its own page. A switch's section (Status) is drawn on its effect's row; a section the sidebar
	// leaves out takes the whole trail with it.
	std::vector<std::string> rows;
	for (size_t from = 0; ; ) {
		const size_t dot = section.find('.', from);
		const std::string path = section.substr(0, dot);
		const std::string name = section.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
		if (name == "Status") break;
		if (HiddenFromSidebar(path, name)) return;
		rows.push_back(path);
		if (dot == std::string::npos) break;
		from = dot + 1;
	}
	trail.swap(rows);
}

static std::unordered_set<std::string> s_markedRows;	// this frame's marked entries: a page's or a fold's path, a special page's id
static int s_markedFrame = -1;

static void MarkSidebarRows() {
	if (s_markedFrame == ImGui::GetFrameCount()) return;
	s_markedFrame = ImGui::GetFrameCount();
	s_markedRows.clear();
	for (const auto& unsaved : UnsavedSettings::All()) {
		const std::string& section = unsaved.first;
		for (const auto& entry : unsaved.second) {
			const std::string& key = entry.first;
			// A key with no row is drawn nowhere, but for the LUT pickers and the RT master switches (on the Ray tracing page only)
			const bool rowless = ShouldHideKey(key.c_str());
			if (rowless && !KeyInList("DayLUT, NightLUT, InteriorLUT, RTInteriors, RTExteriors", key.c_str())) continue;
			std::vector<std::string> trail;
			if (!(rowless && section == "Main.Main.RayTracing")) TreeTrail(section, key, trail);
			bool rt = IsRtSection(section);
			for (const RtSharedBlock& b : kRtSharedBlocks)
				for (const VisualItem& item : b.Source->Items) rt = rt || VisualItemHas(item, section, key);
			if (rt) trail.insert(trail.end(), { "Shaders", kRtTabId });
			const bool aoPickers = ScopeHas(kScopeAoMode, section, key) || ScopeHas(kScopeSkyMode, section, key) || ScopeHas(kScopeGiMode, section, key);
			bool ao = aoPickers;
			for (const VisualItem& item : kAoPageItems) ao = ao || VisualItemHas(item, section, key);
			if (ao) trail.insert(trail.end(), { "Shaders", kAoTabId });
			if (aoPickers || ScopeHas(kScopeAaMode, section, key) || ScopeHas(kScopeQuickPage, section, key) ||
					(DxvkCompute::Available() && ScopeHas(kScopeQuickPageRt, section, key)))
				trail.push_back(kQuickTabId);
			if (DevOptionsShown() && VisualItemHas(kDeferredPageSunKeys, section, key)) trail.insert(trail.end(), { "Main", "Main.Main", kDeferredTabId });
			if (DebugPageHas(section, key)) trail.insert(trail.end(), { "Main", "Main.Main", kDebugTabId });
			if (ScopeHas(kScopeUnderDev, section, key)) trail.push_back(kUnderDevTabId);
			if (ScopeHas(kScopeGearWeather, section, key)) trail.insert(trail.end(), { "Shaders", kGearWeatherTabId });
			for (const CategoryScope& c : kCategoryScopes) if (ScopeHas(c.Scope, section, key)) trail.push_back(c.Id);
			for (const std::string& row : trail) s_markedRows.insert(row);
		}
	}
}

// Whether the sidebar entry (a page's or a fold's path, or a special page's id) leads to an unsaved setting.
static bool SidebarMarked(const std::string& row) {
	if (!UnsavedSettings::Any()) return false;
	MarkSidebarRows();
	return s_markedRows.count(row) != 0;
}

// "Quick settings" at the top of the sidebar: the default landing page when the menu opens.
static void DrawQuickSettingsEntry() {
	if (gMenu.page.empty()) gMenu.page = kQuickTabId;
	const bool shown = gMenu.page == kQuickTabId;
	ImGui::PushID(kQuickTabId);
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(shown ? 0.25f : 0.0f));
	if (ImGui::Selectable("Quick settings", shown)) gMenu.page = kQuickTabId;
	MenuWidgets::MarkLastItem("Quick settings", SidebarMarked(kQuickTabId));	// 2026-11-25 (unsaved marks)
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Runtime status and the main graphics switches in one place.");
	ImGui::PopID();
}

// "Under Development" right below Quick settings.
static void DrawUnderDevelopmentEntry() {
	const bool shown = gMenu.page == kUnderDevTabId;
	ImGui::PushID(kUnderDevTabId);
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(shown ? 0.25f : 0.0f));
	if (ImGui::Selectable("Under Development", shown)) gMenu.page = kUnderDevTabId;
	MenuWidgets::MarkLastItem("Under Development", SidebarMarked(kUnderDevTabId));
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Experimental features currently in active development: GearWeather, WorldSurface, MaterialSSS, and RealSunPath/SunSheen.");
	ImGui::PopID();
}

static void RenderSidebar() {
	RenderSearchBox();	// UNOFFICIAL 2026-10-25: menu search
	if (SearchActive()) return RenderSearchSidebar();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	DrawQuickSettingsEntry();
	DrawUnderDevelopmentEntry();

	// The skin's IndentSpacing is 0 (the pages indent their own groups): the sidebar's folds get a small step per level
	ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetFontSize());
	if (!AdvancedShown()) DrawSimpleCategoriesSidebar();
	StringList roots;
	TheSettingManager->FillMenuSections(&roots, nullptr);
	for (const std::string& root : roots) {
		if (!AdvancedShown() && root == "Shaders") continue;	// Simple mode uses the merged Graphics categories above
		if (HiddenFromSidebar(root, root)) continue;
		StringList children;
		TheSettingManager->FillMenuSections(&children, root.c_str());
		ImGui::PushID(root.c_str());
		if (children.empty()) DrawLeafSection(root, root);
		else if (MenuWidgets::GroupLabel(root.c_str(), gMenu.revealPage && LeadsToSelected(root), SidebarMarked(root))) DrawChildren(root);	// 2026-11-25 (unsaved marks)
		ImGui::PopID();
	}
	ImGui::PopStyleVar();
	LayoutMenu();	// 2026-11-10: the right-click menu that rearranges the effects list
}

// ---- Drawn without the menu ----------------------------------------------------------------------------------------------

// At the game's main menu: the version and the menu key along the bottom of the screen, for the first five seconds.
static void RenderMainMenuToast() {
	static const ULONGLONG shownSince = GetTickCount64();
	// 2026-11-02: files taken over from (or left in) the old New Vegas Reloaded places are listed above the hint, longer
	const std::vector<std::string>& notices = Paths::OldFilesNotices();
	if (GetTickCount64() - shownSince > (notices.empty() ? 5000u : 20000u)) return;
	char text[256];
	sprintf_s(text, "%s %s loaded  |  press %s to open settings", Branding::ProductName, Branding::Version,
		DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));
	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImDrawList* front = ImGui::GetForegroundDrawList();
	float y = screen.y - 12.0f;
	auto line = [&](const char* s, ImU32 colour) {
		const ImVec2 size = ImGui::CalcTextSize(s);
		y -= size.y + 2.0f;
		const ImVec2 at((screen.x - size.x) * 0.5f, y);
		front->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), s);
		front->AddText(at, colour, s);
	};
	line(text, IM_COL32(220, 220, 220, 255));
	for (auto it = notices.rbegin(); it != notices.rend(); ++it) line(it->c_str(), IM_COL32(255, 200, 90, 255));
}

// 2026-11-03: an old NewVegasReloaded.dll left next to VegasShaders.dll by an upgrade (Paths::TwinDll, found in Main.cpp). Both load
// and both replace the game's shaders, which turns objects and ground white or wrong (three testers on 1.0.1; reproduced by putting
// P85 v4's DLL beside 1.0.1). Nothing in the picture says why, so a red panel says it: large in the middle of the main menu, a banner
// across the top in game, every frame, until the file is gone. Drawn on the foreground list (no window, no input).
static void RenderTwinDllWarning(bool mainMenu) {
	if (Paths::TwinDll.empty()) return;
	const char* name = strrchr(Paths::TwinDll.c_str(), '\\') ? strrchr(Paths::TwinDll.c_str(), '\\') + 1 : Paths::TwinDll.c_str();
	char title[160], body[640];
	sprintf_s(title, "Old New Vegas Reloaded is still installed: %s", name);
	sprintf_s(body,
		"Data\\NVSE\\Plugins\\%s loads next to %s. Both replace the game's shaders, so objects and ground can turn white or look wrong.\n"
		"Fix: quit the game, delete or move %s out of Data\\NVSE\\Plugins, and start again. "
		"With Mod Organizer, untick the old New Vegas Reloaded mod.", name, Paths::DllName, name);

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImFont* font = ImGui::GetFont();
	const float base = ImGui::GetFontSize();
	const float titleSize = base * (mainMenu ? 1.8f : 1.35f), bodySize = base * (mainMenu ? 1.25f : 1.0f);
	const float pad = base * (mainMenu ? 1.2f : 0.7f);
	const float width = ImMin(screen.x - 2.0f * base, mainMenu ? base * 52.0f : screen.x - 2.0f * base);
	const float wrap = width - 2.0f * pad;
	const ImVec2 titleDim = font->CalcTextSizeA(titleSize, FLT_MAX, wrap, title);
	const ImVec2 bodyDim = font->CalcTextSizeA(bodySize, FLT_MAX, wrap, body);
	const float height = pad * 2.0f + titleDim.y + base * 0.5f + bodyDim.y;
	const ImVec2 at((screen.x - width) * 0.5f, mainMenu ? (screen.y - height) * 0.4f : base);

	ImDrawList* front = ImGui::GetForegroundDrawList();
	const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.0f);	// the border breathes, so it is seen
	front->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(70, 8, 8, 235), base * 0.4f);
	front->AddRect(at, ImVec2(at.x + width, at.y + height), IM_COL32(255, 60 + (int)(80 * pulse), 60, 255), base * 0.4f, 0, 3.0f);
	ImVec2 text(at.x + pad, at.y + pad);
	front->AddText(font, titleSize, ImVec2(text.x + 2, text.y + 2), IM_COL32(0, 0, 0, 220), title, nullptr, wrap);
	front->AddText(font, titleSize, text, IM_COL32(255, 225, 90, 255), title, nullptr, wrap);
	text.y += titleDim.y + base * 0.5f;
	front->AddText(font, bodySize, ImVec2(text.x + 1, text.y + 1), IM_COL32(0, 0, 0, 220), body, nullptr, wrap);
	front->AddText(font, bodySize, text, IM_COL32(255, 255, 255, 255), body, nullptr, wrap);
}

// ---- Install check (2026-11-03, InstallCheck.h) -----------------------------------------------------------------------------
// What the start-up check found, on screen: a panel at the top of the game's main menu (first 45 s), the same at the top in game
// when the main menu was skipped (first 20 s), a status-bar item, and Help > Install check with every result and the bug report.
// The old-DLL result has its own large red panel (RenderTwinDllWarning), so the panels and counts here leave it out.

static int InstallIssueCount() {
	int n = 0;
	for (const InstallCheck::Result& r : InstallCheck::Results()) if (r.level != InstallCheck::Level::Info && r.group != "twin") n++;
	// UNOFFICIAL 2026-11-12 ShaderFallback (core/ShaderFallback.h): shaders too long for the graphics driver, and what was left out of them
	for (const std::string& title : ShaderFallback::NoticeTitle) if (!title.empty()) n++;
	return n;
}

// 2026-11-05: this session's whole log, as written so far (already scrubbed line by line by Logger). User: Copy bug report "should copy the
// entire log from that session": players paste the report and forget the attachment. Over kLogCap the end is kept (the crash is at the end).
static std::string SessionLogText() {
	static const size_t kLogCap = 8u << 20;
	std::string path = Logger::FilePath;
	if (path.empty()) return "(no log file)\r\n";
	if (path.size() > 2 && path[1] != ':' && path[0] != '\\') path = Paths::GameRoot() + "\\" + path;	// relative to the game folder
	FILE* in = _fsopen(path.c_str(), "rb", _SH_DENYNO);	// the logger keeps it open for writing
	if (!in) return "(the log file could not be read)\r\n";
	std::string text;
	fseek(in, 0, SEEK_END);
	const long size = ftell(in);
	if (size > 0) {
		const size_t keep = (std::min)((size_t)size, kLogCap);
		fseek(in, (long)((size_t)size - keep), SEEK_SET);
		text.resize(keep);
		text.resize(fread(&text[0], 1, keep, in));
		if (keep < (size_t)size) text = "(the first " + std::to_string((size_t)size - keep) + " bytes left out)\r\n" + text.substr(text.find('\n') + 1);
	}
	fclose(in);
	return text;
}

// Help > Copy bug report: the report on the clipboard, and in the log (so a report without the paste still has it); then the whole session log
// on the clipboard after it (not into the log again)
static void CopyBugReport() {
	const std::string report = InstallCheck::BugReport();
	gMenu.reportCopiedAt = ImGui::GetTime();
	size_t lines = 0, start = 0;
	for (size_t end; (end = report.find("\r\n", start)) != std::string::npos; start = end + 2, lines++)
		Logger::Log("INSTALL report | %s", report.substr(start, end - start).c_str());
	Logger::Log("INSTALL bug report copied to the clipboard (%zu lines + this session's log)", lines);
	const std::string full = report + "\r\n---------- " + Branding::ProductName + " log of this session ----------\r\n" + SessionLogText();
	ImGui::SetClipboardText(full.c_str());
}

static void RenderInstallNotice(bool mainMenu) {
	static ULONGLONG firstSeen = 0;
	static bool shownAtMainMenu = false;
	const int issues = InstallIssueCount();
	if (!issues) return;
	if (!firstSeen) firstSeen = GetTickCount64();
	if (mainMenu) shownAtMainMenu = true;
	else if (shownAtMainMenu) return;	// read at the main menu already
	if (GetTickCount64() - firstSeen > (mainMenu ? 45000u : 20000u)) return;
	static bool logged = false;
	if (!logged) {	// once: a report's log then shows the player was told
		logged = true;
		Logger::Log("INSTALL notice shown %s: %d thing%s to fix", mainMenu ? "on the main menu" : "in game", issues, issues == 1 ? "" : "s");
	}

	const bool problem = InstallCheck::Count(InstallCheck::Level::Problem) > (Paths::TwinDll.empty() ? 0 : 1);
	char title[160];
	sprintf_s(title, "%s install check: %d thing%s to fix", Branding::ProductName, issues, issues == 1 ? "" : "s");
	std::string body;
	for (const InstallCheck::Result& r : InstallCheck::Results()) {
		if (r.level == InstallCheck::Level::Info || r.group == "twin") continue;
		body += "- " + r.title + "\n";
		if (!r.fix.empty()) body += "   " + r.fix + "\n";
	}
	for (unsigned family = 0; family < ShaderFallback::FamilyCount; family++) {	// UNOFFICIAL 2026-11-12 ShaderFallback
		if (ShaderFallback::NoticeTitle[family].empty()) continue;
		body += "- " + ShaderFallback::NoticeTitle[family] + "\n   " + ShaderFallback::NoticeFix[family] + "\n";
	}
	char tail[200];
	sprintf_s(tail, "Details and a ready bug report: press %s, then Help > Install check.", DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));
	body += tail;

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImFont* font = ImGui::GetFont();
	const float base = ImGui::GetFontSize();
	const float titleSize = base * 1.3f, bodySize = base;
	const float pad = base * 0.7f;
	const float width = ImMin(screen.x - 2.0f * base, base * 60.0f);
	const float wrap = width - 2.0f * pad;
	const ImVec2 titleDim = font->CalcTextSizeA(titleSize, FLT_MAX, wrap, title);
	const ImVec2 bodyDim = font->CalcTextSizeA(bodySize, FLT_MAX, wrap, body.c_str());
	const float height = pad * 2.0f + titleDim.y + base * 0.4f + bodyDim.y;
	// At the top; in game under the old-DLL banner when that one shows (at the main menu that one is in the middle)
	const ImVec2 at((screen.x - width) * 0.5f, base + (!mainMenu && !Paths::TwinDll.empty() ? base * 7.0f : 0.0f));

	ImDrawList* front = ImGui::GetForegroundDrawList();
	front->AddRectFilled(at, ImVec2(at.x + width, at.y + height), problem ? IM_COL32(60, 12, 8, 230) : IM_COL32(50, 36, 6, 230), base * 0.4f);
	front->AddRect(at, ImVec2(at.x + width, at.y + height), problem ? IM_COL32(255, 90, 60, 255) : IM_COL32(255, 190, 60, 255), base * 0.4f, 0, 2.0f);
	ImVec2 text(at.x + pad, at.y + pad);
	front->AddText(font, titleSize, ImVec2(text.x + 1, text.y + 1), IM_COL32(0, 0, 0, 220), title, nullptr, wrap);
	front->AddText(font, titleSize, text, IM_COL32(255, 220, 90, 255), title, nullptr, wrap);
	text.y += titleDim.y + base * 0.4f;
	front->AddText(font, bodySize, ImVec2(text.x + 1, text.y + 1), IM_COL32(0, 0, 0, 220), body.c_str(), nullptr, wrap);
	front->AddText(font, bodySize, text, IM_COL32(255, 255, 255, 255), body.c_str(), nullptr, wrap);
}

static void DrawInstallCheckWindow() {
	if (!gMenu.installCheckOpen) return;
	ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(Branding::InstallCheckTitle, &gMenu.installCheckOpen)) {
		static const ImVec4 kProblemRed(1.0f, 0.35f, 0.3f, 1.0f), kAmber(1.0f, 0.75f, 0.2f, 1.0f);
		ImGui::TextWrapped("What %s found when the game started: files left from New Vegas Reloaded, missing files, the settings "
			"file and the graphics setup. Problems and warnings come first, each with what to do.", Branding::ProductName);
		ImGui::Spacing();
		if (ImGui::Button("Copy bug report")) CopyBugReport();
		ImGui::SameLine();
		if (gMenu.reportCopiedAt >= 0.0 && ImGui::GetTime() - gMenu.reportCopiedAt < 4.0)
			ImGui::TextColored(MenuSkin::Heading(0.25f), "Copied: paste it into your report (this session's log is included).");
		else
			ImGui::TextDisabled("version, GPU, DXVK, these results, your changed settings and this session's log");
		ImGui::Separator();
		if (InstallCheck::Results().empty()) ImGui::TextUnformatted("Nothing found.");
		for (unsigned family = 0; family < ShaderFallback::FamilyCount; family++) {	// UNOFFICIAL 2026-11-12 ShaderFallback (core/ShaderFallback.h)
			if (ShaderFallback::NoticeTitle[family].empty()) continue;
			const bool lost = ShaderFallback::Lost[family] != 0;	// some are drawn by the game's own shader
			ImGui::PushStyleColor(ImGuiCol_Text, lost ? kProblemRed : kAmber);
			ImGui::TextWrapped("%s  %s", lost ? "PROBLEM" : "warning", ShaderFallback::NoticeTitle[family].c_str());
			ImGui::PopStyleColor();
			ImGui::Indent();
			ImGui::TextWrapped("%s", ShaderFallback::NoticeFix[family].c_str());
			ImGui::Unindent();
			ImGui::Spacing();
		}
		for (const InstallCheck::Result& r : InstallCheck::Results()) {
			const bool info = r.level == InstallCheck::Level::Info;
			const ImVec4 colour = r.level == InstallCheck::Level::Problem ? kProblemRed : info ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kAmber;
			ImGui::PushStyleColor(ImGuiCol_Text, colour);
			ImGui::TextWrapped("%s  %s", r.level == InstallCheck::Level::Problem ? "PROBLEM" : info ? "info" : "warning", r.title.c_str());
			ImGui::PopStyleColor();
			if (!r.fix.empty()) {
				ImGui::Indent();
				ImGui::TextWrapped("%s", r.fix.c_str());
				ImGui::Unindent();
			}
			ImGui::Spacing();
		}
	}
	ImGui::End();
}

// A small dim marker in the top-right corner while the F10 GPU/CPU profiler or Deep Probe is running.
// Drawn on the foreground list, so it needs no window and shows with the menu closed.
static void RenderProfilerIndicator() {
	const bool prof = GpuTimer::Enabled;
	const bool probe = DeepProbe::Active();
	if (!prof && !probe) return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const float radius = 3.0f, margin = 8.0f, gap = 5.0f;
	ImDrawList* draw = ImGui::GetForegroundDrawList();
	float y = margin;

	auto drawBadge = [&](const char* label, ImU32 dotCol) {
		const ImVec2 textSize = ImGui::CalcTextSize(label);
		const ImVec2 textPos(display.x - margin - textSize.x, y);
		const ImVec2 center(textPos.x - gap - radius, y + textSize.y * 0.5f);
		draw->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 140), label);
		draw->AddText(textPos, IM_COL32(225, 225, 225, 190), label);
		draw->AddCircleFilled(center, radius, dotCol);
		y += textSize.y + 3.0f;
	};

	if (prof) drawBadge("PROF", IM_COL32(230, 60, 60, 190));
	if (probe && !DeepProbe::LiveActive()) {
		char buf[32];
		sprintf_s(buf, "PROBE %.1fs", DeepProbe::RemainingSeconds());
		drawBadge(buf, IM_COL32(245, 175, 45, 220));
	}
	if (DeepProbe::LiveActive()) {
		// UNOFFICIAL 2026-11-28 (deepprobe-live): the badge, and with LiveHud a few lines of the 10 s window while the menu is closed
		const LARGE_INTEGER t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();
		const int secs = (int)DeepProbe::LiveSeconds();
		char buf[64];
		sprintf_s(buf, "LIVE %d:%02d", secs / 60, secs % 60);
		drawBadge(buf, IM_COL32(80, 200, 120, 220));
		static int hudCheck = 0;
		static bool hud = true;
		if (hudCheck-- <= 0) { hudCheck = 120; hud = TheSettingManager && TheSettingManager->GetSettingI("Main.Develop.PerformanceProbe", "LiveHud") != 0; }
		if (hud && !gOverlay.open) {
			const DeepProbe::LiveView& v = DeepProbe::Live(10);
			auto line = [&](const char* text) {
				const ImVec2 size = ImGui::CalcTextSize(text);
				const ImVec2 pos(display.x - margin - size.x, y);
				draw->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 160), text);
				draw->AddText(pos, IM_COL32(225, 225, 225, 200), text);
				y += size.y + 1.0f;
			};
			char text[160];
			sprintf_s(text, "%.1f ms (10 s) | 1 s %.1f | p99 %.1f | worst %.1f", v.avg10Ms, v.avg1Ms, v.p99SessionMs, v.worst10Ms);
			line(text);
			sprintf_s(text, "spikes %u | probe %.2f ms/f%s", v.spikes, v.probeMs, v.samplerOff ? " | SAMPLER OFF" : "");
			line(text);
			for (size_t i = 0, shown = 0; i < v.dlls.size() && shown < 3; i++) {
				if (v.dlls[i].role == std::string("Game") || v.dlls[i].role == std::string("OS / CRT") || v.dlls[i].role == std::string("Windows")) continue;
				sprintf_s(text, "%.40s %.2f ms", v.dlls[i].name.c_str(), v.dlls[i].nearMs);
				line(text);
				shown++;
			}
		}
		LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
		DeepProbe::AddUiCost(t1.QuadPart - t0.QuadPart);
	}
}

// UNOFFICIAL 2026-11-28 (deepprobe-live): the probe window's Live tab: a 1 s / 10 s / session window over live mode's counters.
static void DrawDeepProbeLive() {
	static int window = 10;
	ImGui::TextUnformatted("Window:");
	ImGui::SameLine();
	if (ImGui::RadioButton("1 s", window == 1)) window = 1;
	ImGui::SameLine();
	if (ImGui::RadioButton("10 s", window == 10)) window = 10;
	ImGui::SameLine();
	if (ImGui::RadioButton("Session", window == 0)) window = 0;
	const DeepProbe::LiveView& v = DeepProbe::Live(window);
	if (v.nextAutosaveSec >= 0.0) {
		ImGui::SameLine();
		ImGui::TextDisabled("| report folder rewritten in %.0f s: %s", v.nextAutosaveSec, v.folder.c_str());
	}

	ImGui::Text("Frame: %.2f ms in this window (%.1f fps) | 1 s %.2f | 10 s %.2f | session %.2f, p99 %.1f | worst (10 s) %.1f ms | %u frames, %u spikes",
		v.windowAvgMs, v.windowAvgMs > 0.0 ? 1000.0 / v.windowAvgMs : 0.0, v.avg1Ms, v.avg10Ms, v.avgSessionMs, v.p99SessionMs, v.worst10Ms, v.frames, v.spikes);
	ImGui::Text("Split: Game update %.2f ms | Game render CPU %.2f ms | Present / GPU wait %.2f ms", v.updateMs, v.renderMs, v.presentMs);
	const ImVec4 costColour = v.samplerOff ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : ImVec4(0.6f, 0.85f, 1.0f, 1.0f);
	ImGui::TextColored(costColour, "Probe cost: ~%.3f ms/frame (sampler %.3f: %.1f us x %.0f samples/s; script timing %.3f; bookkeeping + drawing %.3f)%s",
		v.probeMs, v.probeSamplerMs, v.stallUs, v.samplesPerSec, v.probeScriptMs, v.probeOwnMs,
		v.samplerOff ? (v.watchdogTrips ? " | SAMPLER OFF: the watchdog stopped it (see the log)" : " | SAMPLER OFF") : "");

	// The last 300 frames
	float recent[DeepProbe::kRecent];
	for (unsigned i = 0; i < DeepProbe::kRecent; i++) recent[i] = v.recent[(v.recentHead + i) % DeepProbe::kRecent];
	ImGui::PlotLines("##probe_recent", recent, (int)DeepProbe::kRecent, 0, "last 300 frames (ms)", 0.0f, (std::max)(34.0f, (float)v.worst10Ms * 1.1f), ImVec2(-1.0f, 60.0f));

	const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##live_cols", 2, ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextColored(MenuSkin::Heading(0.25f), "DLLs (main thread, %u samples)", v.windowSamples);
		if (ImGui::BeginTable("##live_dlls", 4, flags)) {
			ImGui::TableSetupColumn("Module"); ImGui::TableSetupColumn("Role"); ImGui::TableSetupColumn("Near ms"); ImGui::TableSetupColumn("Own ms");
			ImGui::TableHeadersRow();
			for (const auto& d : v.dlls) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(d.name.c_str());
				ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
				ImGui::TableNextColumn(); ImGui::Text("%.3f", d.nearMs);
				ImGui::TableNextColumn(); ImGui::Text("%.3f", d.ownMs);
			}
			ImGui::EndTable();
		}
		ImGui::TableNextColumn();
		ImGui::TextColored(MenuSkin::Heading(0.25f), "Engine stages (CpuSplit, ms/frame)");
		if (ImGui::BeginTable("##live_stages", 2, flags)) {
			for (const auto& st : v.stages) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(st.name);
				ImGui::TableNextColumn(); ImGui::Text("%.2f", st.ms);
			}
			ImGui::EndTable();
		}
		ImGui::TextColored(MenuSkin::Heading(0.25f), "Scripts by plugin (exact, ms/frame)");
		if (ImGui::BeginTable("##live_scripts", 2, flags)) {
			for (const auto& m : v.scriptMods) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(m.name.c_str());
				ImGui::TableNextColumn(); ImGui::Text("%.3f", m.ms);
			}
			ImGui::EndTable();
		}
		ImGui::EndTable();
	}

	ImGui::TextColored(MenuSkin::Heading(0.25f), "Latest spikes (newest first)");
	if (v.spikesList.empty()) ImGui::TextDisabled("None yet.");
	else if (ImGui::BeginTable("##live_spikes", 5, flags)) {
		ImGui::TableSetupColumn("Ago"); ImGui::TableSetupColumn("ms"); ImGui::TableSetupColumn("Top near DLLs (sampled)");
		ImGui::TableSetupColumn("Top stage"); ImGui::TableSetupColumn("Top script plugin");
		ImGui::TableHeadersRow();
		for (const auto& sp : v.spikesList) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("%.0f s", sp.agoSec);
			ImGui::TableNextColumn(); ImGui::Text("%.1f", sp.ms);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(sp.dlls.empty() ? "(pending / no sample)" : sp.dlls.c_str());
			ImGui::TableNextColumn(); ImGui::TextUnformatted(sp.stage.empty() ? "-" : sp.stage.c_str());
			ImGui::TableNextColumn(); ImGui::TextUnformatted(sp.script.empty() ? "-" : sp.script.c_str());
		}
		ImGui::EndTable();
	}
}

static void DrawDeepProbeWindowBody();
static void DrawDeepProbeWindow() {
	if (!gMenu.deepProbeOpen) return;
	LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
	DrawDeepProbeWindowBody();
	LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
	DeepProbe::AddUiCost(t1.QuadPart - t0.QuadPart);	// counted in the probe's own cost while a capture runs
}

static void DrawDeepProbeWindowBody() {
	ImGui::SetNextWindowSize(ImVec2(820.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Performance probe###DeepProbe", &gMenu.deepProbeOpen)) { ImGui::End(); return; }

	const bool running = DeepProbe::Active();
	ImGui::TextWrapped("Profiles everything happening in a slow area: engine stages, every loaded DLL / xNVSE plugin, "
		"every running script and ESP/ESM mod, Vegas Shaders GPU passes, and a 1-frame draw-call / light / actor census.");
	ImGui::Spacing();

	const bool live = DeepProbe::LiveActive();
	if (running && live) {
		if (ImGui::Button("Stop Live")) DeepProbe::RequestStop();
		ImGui::SameLine();
		const int secs = (int)DeepProbe::LiveSeconds();
		ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.5f, 1.0f), "Live for %d:%02d (play normally; the key or this button stops it)", secs / 60, secs % 60);
	}
	else if (running) {
		if (ImGui::Button("Stop Now")) DeepProbe::RequestStop();
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Capturing... %.1f s left (play normally in the slow spot)", DeepProbe::RemainingSeconds());
	}
	else {
		if (ImGui::Button("Start Probe")) {
			DeepProbe::RequestStart((double)gMenu.deepProbeDuration, gMenu.deepProbeAutoClose);
			if (gMenu.deepProbeAutoClose) SetOverlayVisible(false);
		}
		ImGui::SameLine();
		if (ImGui::RadioButton("5s", gMenu.deepProbeDuration == 5)) gMenu.deepProbeDuration = 5;
		ImGui::SameLine();
		if (ImGui::RadioButton("10s", gMenu.deepProbeDuration == 10)) gMenu.deepProbeDuration = 10;
		ImGui::SameLine();
		if (ImGui::RadioButton("20s", gMenu.deepProbeDuration == 20)) gMenu.deepProbeDuration = 20;
		ImGui::SameLine();
		ImGui::Checkbox("Auto-close menu", &gMenu.deepProbeAutoClose);
		// UNOFFICIAL 2026-11-28 (deepprobe-live): live mode runs until stopped
		if (ImGui::Button("Start Live")) DeepProbe::RequestStartLive();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Records until stopped (this button or the LiveKey, Shift+F12 by default): 1 s / 10 s / session windows here,\n"
				"spike frames with their top DLLs, a small HUD while the menu is closed, and a report folder\n%s\\DeepProbe_Live_<time>\\ rewritten every minute.",
				Paths::LogsFolder);
		ImGui::SameLine();
		ImGui::TextDisabled("%s", DeepProbe::LastStatus().c_str());
	}

	const DeepProbe::Summary& s = DeepProbe::LastSummary();
	if (s.valid || live) {
		if (s.valid) {
			ImGui::SameLine();
			if (ImGui::Button("Copy Full Report")) {
				ImGui::SetClipboardText(DeepProbe::LastReportText().c_str());
				gMenu.probeCopiedAt = ImGui::GetTime();
			}
			if (gMenu.probeCopiedAt >= 0.0 && ImGui::GetTime() - gMenu.probeCopiedAt < 4.0) {
				ImGui::SameLine();
				ImGui::TextColored(MenuSkin::Heading(0.25f), "Copied!");
			}
			ImGui::TextDisabled("Saved: %s%s", DeepProbe::LastReportPath().c_str(), s.live && !s.final ? " (autosave)" : "");
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
			ImGui::TextWrapped("Verdict: %s", s.verdict.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Separator();

		const ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
		if (ImGui::BeginTabBar("##deep_probe_tabs")) {
			if (live && ImGui::BeginTabItem("Live")) {
				DrawDeepProbeLive();
				ImGui::EndTabItem();
			}
			if (s.valid && ImGui::BeginTabItem("Overview")) {
				ImGui::Text("FPS: avg %.1f (%.2f ms) | p50 %.2f | p95 %.2f | p99 %.2f | 1%% low %.1f fps | 0.1%% low %.1f fps | %u frames (%.1fs)",
					s.avgFps, s.avgMs, s.p50Ms, s.p95Ms, s.p99Ms, s.low1Fps, s.low01Fps, s.frames, s.durationSec);
				ImGui::Text("CPU Split: Game Update %.2f ms/f | Game Render CPU %.2f ms/f | Present/Wait %.2f ms/f | AI1 %.2f ms | AI2 %.2f ms",
					s.updateMs, s.renderCpuMs, s.presentMs, s.ai1BusyMs, s.ai2BusyMs);
				ImGui::Text("Vegas Shaders GPU ~%.2f ms/f | Scripts: %.2f ms/f (%llu calls) | Draws (1f): %u (%u skinned, %u shader binds)",
					s.gpuVegasMs, s.totalScriptSelfMs, s.totalScriptCalls, s.censusTotalDraws, s.censusSkinnedDraws, s.censusShaderBinds);
				ImGui::Text("Scene: %s | Lights: %u active / %u | Actors H/MH/ML/L: %u/%u/%u/%u | VAS: %.0f/%.0f MB | ESPs %u, DLLs %u",
					s.cellDesc.c_str(), s.activeLights, s.totalLights, s.actorsHigh, s.actorsMidHigh, s.actorsMidLow, s.actorsLow,
					s.vasUsedMB, s.vasTotalMB, s.loadedPlugins, s.loadedDlls);
				ImGui::Spacing();
				ImGui::TextColored(MenuSkin::Heading(0.25f), "Top DLLs (Main Thread Exclusive)");
				if (ImGui::BeginTable("##ov_dlls", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
					ImGui::TableSetupColumn("Module"); ImGui::TableSetupColumn("Role");
					ImGui::TableSetupColumn("Excl ms/f"); ImGui::TableSetupColumn("Incl ms/f"); ImGui::TableSetupColumn("Top Stage");
					ImGui::TableHeadersRow();
					for (size_t i = 0; i < s.dlls.size() && i < 8; i++) {
						const auto& d = s.dlls[i];
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(d.name.c_str());
						ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.2f (%.1f%%)", d.exclMs, d.exclPct);
						ImGui::TableNextColumn(); ImGui::Text("%.2f (%.1f%%)", d.inclMs, d.inclPct);
						ImGui::TableNextColumn(); ImGui::TextUnformatted(d.topStage.c_str());
					}
					ImGui::EndTable();
				}
				ImGui::Spacing();
				ImGui::TextColored(MenuSkin::Heading(0.25f), "Top Script Mods (.esp/.esm)");
				if (ImGui::BeginTable("##ov_smods", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
					ImGui::TableSetupColumn("Mod"); ImGui::TableSetupColumn("Self ms/f");
					ImGui::TableSetupColumn("Calls/f"); ImGui::TableSetupColumn("Max Call");
					ImGui::TableHeadersRow();
					for (size_t i = 0; i < s.scriptMods.size() && i < 8; i++) {
						const auto& m = s.scriptMods[i];
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(m.modName.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.3f ms", m.selfMs);
						ImGui::TableNextColumn(); ImGui::Text("%.1f", m.callsPerFrame);
						ImGui::TableNextColumn(); ImGui::Text("%.2f ms", m.maxCallMs);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (s.valid && ImGui::BeginTabItem("DLLs / Plugins")) {
				ImGui::Text("Main thread samples: %u (AI1: %u, AI2: %u) | probe cost ~%.3f ms/f (sampler %.3f, %.1f us/sample; scripts %.3f; bookkeeping %.3f)%s",
					s.samplerMainSamples, s.samplerAi1Samples, s.samplerAi2Samples, s.probeMs, s.probeSamplerMs, s.stallUsPerSample, s.probeScriptMs, s.probeOwnMs,
					s.watchdogTrips ? " | WATCHDOG turned the sampler off" : "");
				ImGui::TextDisabled("Excl = time in the module itself; Near = the first plugin DLL among the next return addresses (engine work a plugin's hook calls).");
				if (ImGui::BeginTable("##dlls_tbl", 8, tblFlags, ImVec2(0.0f, 260.0f))) {
					ImGui::TableSetupColumn("Module"); ImGui::TableSetupColumn("Role");
					ImGui::TableSetupColumn("Excl ms/f"); ImGui::TableSetupColumn("Excl %");
					ImGui::TableSetupColumn("Near ms/f");
					ImGui::TableSetupColumn("Incl ms/f"); ImGui::TableSetupColumn("Wait %");
					ImGui::TableSetupColumn("Top Stage");
					ImGui::TableHeadersRow();
					for (const auto& d : s.dlls) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(d.name.c_str());
						if (!d.mo2Mod.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("MO2 mod: %s", d.mo2Mod.c_str());
						ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.2f", d.exclMs);
						ImGui::TableNextColumn(); ImGui::Text("%.1f%%", d.exclPct);
						ImGui::TableNextColumn(); ImGui::Text("%.2f", d.nearMs);
						ImGui::TableNextColumn(); ImGui::Text("%.2f (%.1f%%)", d.inclMs, d.inclPct);
						ImGui::TableNextColumn(); ImGui::Text("%.0f%%", d.waitPct);
						ImGui::TableNextColumn(); ImGui::TextUnformatted(d.topStage.c_str());
					}
					ImGui::EndTable();
				}
				if (!s.driverCallers.empty()) {
					ImGui::Spacing();
					ImGui::TextColored(MenuSkin::Heading(0.25f), "Driver / DXVK CPU Time by Calling Module");
					if (ImGui::BeginTable("##drv_tbl", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
						ImGui::TableSetupColumn("Caller"); ImGui::TableSetupColumn("Role");
						ImGui::TableSetupColumn("ms/frame"); ImGui::TableSetupColumn("% Main CPU");
						ImGui::TableHeadersRow();
						for (const auto& dc : s.driverCallers) {
							ImGui::TableNextRow();
							ImGui::TableNextColumn(); ImGui::TextUnformatted(dc.callerName.c_str());
							ImGui::TableNextColumn(); ImGui::TextUnformatted(dc.callerRole.c_str());
							ImGui::TableNextColumn(); ImGui::Text("%.2f", dc.ms);
							ImGui::TableNextColumn(); ImGui::Text("%.1f%%", dc.pct);
						}
						ImGui::EndTable();
					}
				}
				ImGui::EndTabItem();
			}
			if (s.valid && ImGui::BeginTabItem("Scripts & Mods")) {
				ImGui::Text("Total Script Self Time: %.3f ms/frame across %llu calls (%s)",
					s.totalScriptSelfMs, s.totalScriptCalls, s.scriptHookOk ? "ScriptRunner hook active" : "hook unavailable");
				if (ImGui::BeginTable("##smods_tbl", 5, tblFlags, ImVec2(0.0f, 165.0f))) {
					ImGui::TableSetupColumn("Mod (.esp/.esm)"); ImGui::TableSetupColumn("Self ms/f");
					ImGui::TableSetupColumn("Calls/f"); ImGui::TableSetupColumn("Scripts"); ImGui::TableSetupColumn("Max Call ms");
					ImGui::TableHeadersRow();
					for (const auto& m : s.scriptMods) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(m.modName.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.3f", m.selfMs);
						ImGui::TableNextColumn(); ImGui::Text("%.1f", m.callsPerFrame);
						ImGui::TableNextColumn(); ImGui::Text("%u", m.uniqueScripts);
						ImGui::TableNextColumn(); ImGui::Text("%.2f", m.maxCallMs);
					}
					ImGui::EndTable();
				}
				ImGui::Spacing();
				ImGui::TextColored(MenuSkin::Heading(0.25f), "Top Individual Scripts");
				if (ImGui::BeginTable("##scripts_tbl", 7, tblFlags, ImVec2(0.0f, 0.0f))) {
					ImGui::TableSetupColumn("Script"); ImGui::TableSetupColumn("Mod");
					ImGui::TableSetupColumn("Self ms/f"); ImGui::TableSetupColumn("Incl ms/f");
					ImGui::TableSetupColumn("Calls/f"); ImGui::TableSetupColumn("Max ms"); ImGui::TableSetupColumn("FormID");
					ImGui::TableHeadersRow();
					for (const auto& sc : s.scripts) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(sc.label.c_str());
						ImGui::TableNextColumn(); ImGui::TextUnformatted(sc.modName.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.3f", sc.selfMs);
						ImGui::TableNextColumn(); ImGui::Text("%.3f", sc.inclMs);
						ImGui::TableNextColumn(); ImGui::Text("%.1f", sc.callsPerFrame);
						ImGui::TableNextColumn(); ImGui::Text("%.2f", sc.maxCallMs);
						ImGui::TableNextColumn(); ImGui::Text("%08X", sc.formID);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (s.valid && ImGui::BeginTabItem("Stages & GPU")) {
				ImGui::TextColored(MenuSkin::Heading(0.25f), "Engine CPU Stages (CpuSplit)");
				if (ImGui::BeginTable("##stages_tbl", 6, tblFlags, ImVec2(0.0f, 200.0f))) {
					ImGui::TableSetupColumn("Stage"); ImGui::TableSetupColumn("Avg ms");
					ImGui::TableSetupColumn("p95 ms"); ImGui::TableSetupColumn("Max ms");
					ImGui::TableSetupColumn("Frame %"); ImGui::TableSetupColumn("CPU Busy");
					ImGui::TableHeadersRow();
					for (const auto& stg : s.stages) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(stg.name.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.2f", stg.avgMs);
						ImGui::TableNextColumn(); ImGui::Text("%.2f", stg.p95Ms);
						ImGui::TableNextColumn(); ImGui::Text("%.2f", stg.maxMs);
						ImGui::TableNextColumn(); ImGui::Text("%.1f%%", stg.pct);
						ImGui::TableNextColumn();
						if (stg.busyPct >= 0.0) ImGui::Text("%.1f%%", stg.busyPct); else ImGui::TextUnformatted("-");
					}
					ImGui::EndTable();
				}
				ImGui::Spacing();
				ImGui::TextColored(MenuSkin::Heading(0.25f), "GPU Timers (Vegas Shaders & Scene Passes)");
				if (ImGui::BeginTable("##gpu_tbl", 4, tblFlags, ImVec2(0.0f, 0.0f))) {
					ImGui::TableSetupColumn("Pass"); ImGui::TableSetupColumn("Avg ms");
					ImGui::TableSetupColumn("Max ms"); ImGui::TableSetupColumn("Samples");
					ImGui::TableHeadersRow();
					for (const auto& gp : s.gpuPasses) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(gp.name.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%.3f", gp.avgMs);
						ImGui::TableNextColumn(); ImGui::Text("%.3f", gp.maxMs);
						ImGui::TableNextColumn(); ImGui::Text("%u", gp.samples);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (s.valid && ImGui::BeginTabItem("Scene Census")) {
				ImGui::Text("Draws (1 frame): %u total (World %u | 1st-P %u | Refl %u | Other %u) | Skinned: %u | Binds: %u",
					s.censusTotalDraws, s.censusWorldDraws, s.censusFirstPersonDraws, s.censusReflDraws, s.censusOtherDraws, s.censusSkinnedDraws, s.censusShaderBinds);
				ImGui::Text("Active Point Lights: %u / %u | Actors (H/MH/ML/L): %u / %u / %u / %u",
					s.activeLights, s.totalLights, s.actorsHigh, s.actorsMidHigh, s.actorsMidLow, s.actorsLow);
				if (s.censusTotalDraws == 0 && s.censusShaderBinds > 0)
					ImGui::TextDisabled("Note: per-draw mesh hook inactive (enable MaterialRules / LampScissor / HiddenObjectCulling to capture per-mesh counts).");
				if (ImGui::BeginTable("##dmods_tbl", 3, tblFlags, ImVec2(0.0f, 165.0f))) {
					ImGui::TableSetupColumn("Mod (.esp/.esm)"); ImGui::TableSetupColumn("Draws"); ImGui::TableSetupColumn("Skinned");
					ImGui::TableHeadersRow();
					for (const auto& dm : s.drawMods) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(dm.modName.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%u", dm.draws);
						ImGui::TableNextColumn(); ImGui::Text("%u", dm.skinned);
					}
					ImGui::EndTable();
				}
				ImGui::Spacing();
				if (ImGui::BeginTable("##dmesh_tbl", 3, tblFlags, ImVec2(0.0f, 0.0f))) {
					ImGui::TableSetupColumn("Mesh / Node"); ImGui::TableSetupColumn("Owner Mod"); ImGui::TableSetupColumn("Draws");
					ImGui::TableHeadersRow();
					for (const auto& msh : s.drawMeshes) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(msh.meshName.c_str());
						ImGui::TableNextColumn(); ImGui::TextUnformatted(msh.modName.c_str());
						ImGui::TableNextColumn(); ImGui::Text("%u", msh.draws);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (s.valid && !s.spikes.empty() && ImGui::BeginTabItem("Spikes")) {
				ImGui::Text("%u spike frames (over SpikeMs or 2.5x the 10 s mean; loading screens left out); the newest %u:", s.spikesTotal, (unsigned)s.spikes.size());
				if (ImGui::BeginTable("##spikes_tbl", 5, tblFlags, ImVec2(0.0f, 0.0f))) {
					ImGui::TableSetupColumn("At s"); ImGui::TableSetupColumn("ms"); ImGui::TableSetupColumn("Top near DLLs (sampled)");
					ImGui::TableSetupColumn("Top stage"); ImGui::TableSetupColumn("Top script plugin");
					ImGui::TableHeadersRow();
					for (const auto& sp : s.spikes) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::Text("%.1f", sp.atSec);
						ImGui::TableNextColumn(); ImGui::Text("%.1f", sp.ms);
						ImGui::TableNextColumn(); ImGui::TextUnformatted(sp.dlls.empty() ? "(no sample)" : sp.dlls.c_str());
						ImGui::TableNextColumn(); if (sp.stage.empty()) ImGui::TextUnformatted("-"); else ImGui::Text("%s %.1f ms", sp.stage.c_str(), sp.stageMs);
						ImGui::TableNextColumn(); if (sp.script.empty()) ImGui::TextUnformatted("-"); else ImGui::Text("%s %.2f ms", sp.script.c_str(), sp.scriptMs);
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (s.valid && ImGui::BeginTabItem("Full Report")) {
				ImGui::BeginChild("##probe_full_txt", ImVec2(0.0f, 0.0f), true);
				const std::string& full = DeepProbe::LastReportText();
				ImGui::TextUnformatted(full.c_str(), full.c_str() + full.size());
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	else if (!running) {
		ImGui::Separator();
		ImGui::TextDisabled("Click 'Start Probe' while standing in a laggy spot. Results open automatically when finished and save to %s\\DeepProbe_<time>.txt.", Paths::LogsFolder);
		ImGui::TextDisabled("'Start Live' (or Shift+F12) records until stopped, with live windows, spike breakdowns and a report folder rewritten every minute.");
	}
	ImGui::End();
}

// Escape (not while typing: there it only leaves the text box) or Alt closes the menu, once the key is let go, so the game never
// sees it held. Returns true when the menu closed.
static bool CloseMenuOnEscapeOrAlt() {
	struct CloseOnRelease {
		bool armed = false;
		bool Fire(bool pressed, bool held) {
			if (pressed) armed = true;
			if (!armed || held) return false;
			armed = false;
			return true;
		}
	};
	static CloseOnRelease escape, alt;
	const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput;
	const bool altPressed = ImGui::IsKeyPressed(ImGuiKey_LeftAlt) || ImGui::IsKeyPressed(ImGuiKey_RightAlt);
	if (escapePressed) escape.armed = true;
	if (altPressed) alt.armed = true;
	const bool closing = escape.Fire(false, ImGui::IsKeyDown(ImGuiKey_Escape)) ||
		alt.Fire(false, ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt));
	if (closing) SetOverlayVisible(false);
	return closing;
}

// ---- File menu actions ---------------------------------------------------------------------------------------------------

static void SaveSettingsNow() {
	TheSettingManager->SaveSettings();
	InterfaceManager->ShowMessage("Settings saved.");
}

// Letters and digits kept, anything else becomes '_' (for file names).
static std::string FileNamePart(const char* text) {
	std::string part;
	for (const char* c = text; c && *c; c++) part += isalnum((unsigned char)*c) ? *c : '_';
	return part;
}

// Where the player is, for the copy's file name: the worldspace and cell editor IDs (TTW-safe), and the grid for exteriors.
static std::string LocationForFileName() {
	if (!Player || !Player->parentCell) return std::string();
	TESObjectCELL* cell = Player->parentCell;
	TESWorldSpace* world = Player->GetWorldSpace();
	std::string location = world ? FileNamePart(world->GetEditorName()) : std::string();
	const std::string cellName = FileNamePart(cell->GetEditorName());
	if (!cellName.empty()) location += (location.empty() ? "" : "_") + cellName;
	if (TESObjectCELL::CellCoordinates* grid = cell->coords) {
		char coords[32] = {};
		_snprintf_s(coords, sizeof(coords), "_%d_%d", grid->x, grid->y);
		location += coords;
	}
	return location;
}

// File > Save Copy: the settings as VegasShaders_<yyyymmdd_hhmm>[_<location>].toml next to the DLL.
static void SaveSettingsCopy() {
	const std::string base = Paths::ModuleDir() + "VegasShaders";

	const time_t now = time(nullptr);
	tm local = {};
	localtime_s(&local, &now);
	char stamp[32] = {};
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M", &local);

	const std::string location = LocationForFileName();
	std::string path = std::string(base) + "_" + stamp;
	if (!location.empty()) path += "_" + location;
	path += ".toml";

	TheSettingManager->SaveSettingsTo(path.c_str());
	InterfaceManager->ShowMessage("Settings copy saved.");
}

// File > Reload from Disk: every setting from the saved file again, after Yes in this box.
static void DrawDiskReloadPopup() {
	if (gMenu.diskConfirm) {	// asked from the menu bar: opened here, in the main window's ID scope, where it is drawn
		ImGui::OpenPopup("##ConfirmDisk");
		gMenu.diskConfirm = false;
	}
	if (!ImGui::BeginPopupModal("##ConfirmDisk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::TextUnformatted("Reload from disk? All unsaved changes will be lost.");
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	const ImVec2 buttonSize(80.0f, 0.0f);
	if (ImGui::Button("Yes", buttonSize)) {
		TheSettingManager->RevertSettings();
		gMenu.sessionValues.clear();
		gMenu.fxModifier = -1;	// read again from the file
		gMenu.page.clear();
		gMenu.resetWheels = true;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", buttonSize)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

static void SetDeveloperOptions(bool on) {
	TheSettingManager->SetSetting("Main.Menu.Developer", "DeveloperOptions", on);
	TheSettingManager->LoadSettings();
	gMenu.devOptions = on;
}

// UNOFFICIAL 2026-11-12: View > Developer options, switched on: a plain warning first, once a session (people switched it on,
// flipped test switches at random and reported the result as bugs). Cancel leaves it off. Advanced has no such box.
static void DrawDevWarningPopup() {
	if (gMenu.devWarnAsk) {	// asked from the menu bar: opened here, in the main window's ID scope, where it is drawn
		ImGui::OpenPopup("Developer options###devwarn");
		gMenu.devWarnAsk = false;
	}
	if (!ImGui::BeginPopupModal("Developer options###devwarn", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	ImGui::TextUnformatted("These are test switches. Changing them can break the picture; reports made with them changed will first be "
		"asked to reset them.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	if (ImGui::Button("Show them")) {
		gMenu.devWarned = true;
		SetDeveloperOptions(true);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// UNOFFICIAL 2026-11-12: View > Reset test and internal settings to defaults. Only the Developer options settings and the Internal
// ones (ImGuiManager::SettingLevel; the list the session log's "test / internal settings changed" line names) go back to their
// defaults, after one question; the box then says how many. Like any change it is kept by File > Save.
static void DrawTestResetPopup() {
	static std::vector<SessionLog::ChangedSetting> pending;	// what differs, read when the box opens
	static int done = -1, left = 0;	// how many were reset and how many still differ (the box then says so); -1 while it asks
	if (gMenu.testResetAsk) {
		pending = SessionLog::TestSettingsChanged();
		done = -1;
		if (!pending.empty()) SessionLog::LogTestSettingsDiff("MENU test / internal settings changed");	// the box names eight; the log all
		ImGui::OpenPopup("Reset test and internal settings###testreset");
		gMenu.testResetAsk = false;
	}
	if (!ImGui::BeginPopupModal("Reset test and internal settings###testreset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
	if (done >= 0) {
		ImGui::Text("%d setting%s reset to the default. File > Save keeps it; some take effect after a game restart.", done, done == 1 ? " was" : "s were");
		if (left > 0) ImGui::Text("%d could not be reset (Max Quality holds them, or their type did not match): see the log.", left);
	}
	else if (pending.empty())
		ImGui::TextUnformatted("Every test and internal setting is at its default already: nothing to reset.");
	else {
		ImGui::Text("%u test and internal setting%s not at the default. Reset %s? Every other setting stays as it is.", (UInt32)pending.size(),
			pending.size() == 1 ? " is" : "s are", pending.size() == 1 ? "it" : "them");
		const size_t named = 8;
		for (size_t i = 0; i < pending.size() && i < named; i++)
			ImGui::TextDisabled("%s.%s = %s (default %s)", pending[i].Section.c_str(), pending[i].Key.c_str(), pending[i].Mine.c_str(), pending[i].Default.c_str());
		if (pending.size() > named) ImGui::TextDisabled("and %u more (the log names them)", (UInt32)(pending.size() - named));
	}
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	if (done < 0 && !pending.empty()) {
		if (ImGui::Button("Reset them")) {
			for (const SessionLog::ChangedSetting& c : pending)
				TheSettingManager->SetSettingS(c.Section.c_str(), c.Key.c_str(), c.DefaultValue.c_str());
			TheSettingManager->LoadSettings();
			SyncShaderSwitches();
			gMenu.resetWheels = true;
			left = (int)SessionLog::TestSettingsChanged().size();
			done = (int)pending.size() - left;
			Logger::Log("MENU reset test and internal settings: %d put back to their defaults, %d still differ", done, left);
			if (left > 0) SessionLog::LogTestSettingsDiff("MENU test / internal settings still changed");
			pending.clear();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	}
	else if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

