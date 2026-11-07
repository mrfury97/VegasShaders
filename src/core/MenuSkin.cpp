#include "MenuSkin.h"
#include "imgui.h"
#include "imgui_internal.h"	// ImClamp
#include <filesystem>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <commdlg.h>
#include <shellapi.h>
#include "ImGuiManager.h"
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// 2026-11-02 menu skins (MenuSkin.h). A skin file, Data\VegasShaders\Skins\<anything>.toml (Paths::DataRoot):
//
//   Name = "Desert Night"          # shown in the dropdown (the file name if missing)
//   Background = "22,23,26"        # the window, as R,G,B (0-255)
//   Panel = "27,29,33"             # the sidebar and the settings list
//   Field = "36,39,45"             # sliders, boxes, buttons
//   Accent = "217,164,65"          # ticks, slider handles, the selected entry
//   Text = "230,227,220"
//   Rounding = 6.0                 # corner radius in pixels (0 = square)
//   Opacity = 0.97                 # the window's opacity (0.5-1)
//   Image = "dunes.png"            # optional background picture, next to the file (png, jpg, dds, bmp)
//   ImageOpacity = 0.25            # how strongly it shows (0-1)
//   ImageFit = "cover"             # cover (fill, crop the overflow), contain (whole picture, bars) or stretch
//   Heading = "240,200,120"        # optional: titles and group headings (missing = the accent, a little towards the text)
//   Border = "60,60,60"            # optional: window and table borders, separators (missing = mixed from background and text)
//   Slider = "217,164,65"          # optional: slider handles (missing = the accent)
//   Selection = "60,50,30"         # optional: the selected or hovered entry's bar (missing = the panel tinted by the accent)
//
// Every other ImGui colour is mixed from those, so the menu stays readable whatever the colours.
//
// On top of the picked skin, [Main.Menu.Style] Custom<Part> replaces one part by hand: CustomBackground ... CustomSelection as
// "R,G,B" (empty = the skin's), CustomRounding (-1 = the skin's), CustomOpacity (0 = the skin's), CustomImage (a picture in the
// Skins folder or a full path, "none" = no picture, empty = the skin's), CustomImageOpacity (-1 = the skin's) and CustomImageFit
// (empty = the skin's). Export writes the result (skin plus own parts) as a new skin file.

namespace MenuSkin {
namespace {
	struct Skin {
		std::string name, file, image, fit = "cover";
		ImVec4 background, panel, field, accent, text;
		float rounding = 6.0f, opacity = 0.97f, imageOpacity = 0.25f;
		ImVec4 heading, border, slider, selection;	// optional: alpha 0 = mixed from the five above (HeadingOf etc.)
	};

	std::string SkinDir() { return std::string(Paths::DataRoot()) + "Skins\\"; }	// Data\VegasShaders\Skins\ (or the old Data\NVR\Skins\)
	constexpr const char* Section = "Main.Menu.Style";

	std::vector<Skin> Skins;
	int Current = 0;
	Skin Active;						// Skins[Current] with the Custom<Part> settings on top: what is drawn
	IDirect3DTexture9* Picture = nullptr;
	std::string PicturePath;			// the file Picture was loaded from
	ImVec2 PictureSize;
	UInt32 SeenRevision = 0xFFFFFFFFu;	// SettingManager::SettingsRevision the saved choice was last read at
	std::string SeenCustom;				// the Custom<Part> values Active was built with

	ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
	ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
	ImVec4 Alpha(ImVec4 c, float a) { c.w = a; return c; }

	bool Set(const ImVec4& c) { return c.w > 0.0f; }

	void BuiltIns() {
		Skins.clear();
		const ImVec4 mixed;	// an optional part left to HeadingOf etc.
		// Default: charcoal with a muted amber, the desert at dusk rather than a neon sign.
		Skins.push_back({ "Default", "", "", "cover", Rgb(22, 23, 26), Rgb(27, 29, 33), Rgb(37, 40, 46), Rgb(214, 162, 72), Rgb(228, 225, 218), 6.0f, 0.97f });
		Skins.push_back({ "Pip-Boy", "", "", "cover", Rgb(6, 16, 9), Rgb(9, 22, 12), Rgb(14, 34, 19), Rgb(88, 230, 128), Rgb(170, 245, 190), 2.0f, 0.96f });
		Skins.push_back({ "Vault-Tec", "", "", "cover", Rgb(14, 26, 46), Rgb(18, 33, 58), Rgb(27, 46, 78), Rgb(242, 194, 48), Rgb(232, 238, 248), 4.0f, 0.97f });
		// Amber Terminal: an old amber phosphor screen, square corners.
		Skins.push_back({ "Amber Terminal", "", "", "cover", Rgb(14, 10, 4), Rgb(20, 14, 6), Rgb(33, 24, 10), Rgb(255, 176, 40), Rgb(255, 206, 128), 0.0f, 0.97f,
			0.25f, Rgb(255, 190, 70), Rgb(92, 64, 22) });
		// NCR: dusty tan and brown with the flag's red, gold headings.
		Skins.push_back({ "NCR", "", "", "cover", Rgb(38, 33, 25), Rgb(46, 40, 30), Rgb(61, 53, 40), Rgb(200, 78, 56), Rgb(238, 228, 206), 3.0f, 0.97f,
			0.25f, Rgb(222, 186, 118) });
		// Legion: near-black red with a strong red, bronze headings.
		Skins.push_back({ "Legion", "", "", "cover", Rgb(22, 9, 9), Rgb(30, 12, 12), Rgb(48, 20, 19), Rgb(204, 44, 38), Rgb(238, 222, 206), 1.0f, 0.97f,
			0.25f, Rgb(214, 168, 88), mixed, mixed, Rgb(78, 24, 22) });
		// Brotherhood: brushed steel, steel blue, gold headings.
		Skins.push_back({ "Brotherhood", "", "", "cover", Rgb(26, 28, 31), Rgb(32, 35, 39), Rgb(46, 50, 56), Rgb(124, 164, 212), Rgb(220, 224, 230), 2.0f, 0.97f,
			0.25f, Rgb(212, 180, 108) });
		// Strip Neon: a casino sign at night, pink with cyan headings and handles.
		Skins.push_back({ "Strip Neon", "", "", "cover", Rgb(16, 10, 24), Rgb(22, 13, 32), Rgb(35, 21, 50), Rgb(255, 72, 172), Rgb(238, 230, 255), 8.0f, 0.96f,
			0.25f, Rgb(90, 222, 255), mixed, Rgb(90, 222, 255) });
		// Midnight: plain dark blue-grey with a soft blue, no theme.
		Skins.push_back({ "Midnight", "", "", "cover", Rgb(16, 19, 26), Rgb(20, 24, 32), Rgb(31, 36, 47), Rgb(112, 152, 255), Rgb(222, 228, 240), 6.0f, 0.97f });
		// High Contrast: black, white text, yellow accent, solid borders, fully opaque.
		Skins.push_back({ "High Contrast", "", "", "cover", Rgb(0, 0, 0), Rgb(0, 0, 0), Rgb(30, 30, 30), Rgb(255, 222, 0), Rgb(255, 255, 255), 0.0f, 1.0f,
			0.25f, Rgb(255, 222, 0), Rgb(200, 200, 200), mixed, Rgb(96, 80, 0) });
		// Light: warm paper with dark text and a dark amber accent.
		Skins.push_back({ "Light", "", "", "cover", Rgb(234, 231, 224), Rgb(244, 242, 236), Rgb(218, 214, 204), Rgb(170, 94, 20), Rgb(32, 30, 26), 6.0f, 0.98f,
			0.25f, Rgb(140, 72, 12), Rgb(184, 178, 166), mixed, Rgb(232, 206, 168) });
	}

	// The optional parts as drawn: the skin's own, or mixed from the five base colours.
	ImVec4 HeadingOf(const Skin& s) { return Set(s.heading) ? s.heading : Mix(s.accent, s.text, 0.25f); }
	ImVec4 BorderOf(const Skin& s) { return Set(s.border) ? s.border : Mix(s.background, s.text, 0.12f); }
	ImVec4 SliderOf(const Skin& s) { return Set(s.slider) ? s.slider : Mix(s.accent, s.field, 0.15f); }
	ImVec4 SelectionOf(const Skin& s) { return Set(s.selection) ? s.selection : Mix(s.panel, s.accent, 0.14f); }
	ImVec4 BackgroundOf(const Skin& s) { return s.background; }
	ImVec4 PanelOf(const Skin& s) { return s.panel; }
	ImVec4 FieldOf(const Skin& s) { return s.field; }
	ImVec4 AccentOf(const Skin& s) { return s.accent; }
	ImVec4 TextOf(const Skin& s) { return s.text; }

	// The colour parts a [Main.Menu.Style] Custom<Part> setting can replace, in menu order.
	struct Part { const char* key; const char* label; ImVec4 Skin::* member; ImVec4 (*resolve)(const Skin&); const char* help; };
	const Part Parts[] = {
		{ "CustomBackground", "Background", &Skin::background, BackgroundOf, "The window behind everything." },
		{ "CustomPanel", "Panel", &Skin::panel, PanelOf, "The sidebar, the settings list and the menu bar." },
		{ "CustomField", "Field", &Skin::field, FieldOf, "Sliders, boxes and buttons." },
		{ "CustomAccent", "Accent", &Skin::accent, AccentOf, "Ticks, hovered buttons, tabs, the selected entry's tint." },
		{ "CustomText", "Text", &Skin::text, TextOf, "All text (greyed text is mixed from it and the background)." },
		{ "CustomHeading", "Heading", &Skin::heading, HeadingOf, "Titles, group headings and links." },
		{ "CustomBorder", "Border", &Skin::border, BorderOf, "Window and table borders, separators." },
		{ "CustomSlider", "Slider", &Skin::slider, SliderOf, "Slider handles." },
		{ "CustomSelection", "Selection", &Skin::selection, SelectionOf, "The bar behind the selected or hovered entry." },
	};
	const char* const OtherKeys[] = { "CustomRounding", "CustomOpacity", "CustomImage", "CustomImageOpacity", "CustomImageFit" };

	std::string ColourText(const ImVec4& v) {
		char b[32];
		sprintf_s(b, "%d,%d,%d", (int)(v.x * 255 + 0.5f), (int)(v.y * 255 + 0.5f), (int)(v.z * 255 + 0.5f));
		return b;
	}

	bool ParseColour(const std::string& s, ImVec4& out) {
		int r, g, b;
		const size_t start = s.find_first_not_of("\"' ");
		if (start == std::string::npos || sscanf_s(s.c_str() + start, "%d , %d , %d", &r, &g, &b) != 3) return false;
		out = Rgb(ImClamp(r, 0, 255), ImClamp(g, 0, 255), ImClamp(b, 0, 255));
		return true;
	}

	std::string Str(const tomlValue& doc, const char* key, const std::string& fallback) {
		return doc.contains(key) && doc.at(key).is_string() ? doc.at(key).as_string().str : fallback;
	}
	float Num(const tomlValue& doc, const char* key, float fallback) {
		if (!doc.contains(key)) return fallback;
		const tomlValue& v = doc.at(key);
		return v.is_floating() ? (float)v.as_floating() : v.is_integer() ? (float)v.as_integer() : fallback;
	}

	// The built-ins, then every readable file in the skin folder (a file whose Name matches a built-in replaces it).
	void Scan() {
		BuiltIns();
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(SkinDir(), ec)) {
			if (entry.path().extension() != ".toml") continue;
			try {
				tomlValue doc = toml::parse<toml::preserve_comments, std::map>(entry.path().string());
				Skin s = Skins[0];
				s.file = entry.path().string();
				s.name = Str(doc, "Name", entry.path().stem().string());
				ParseColour(Str(doc, "Background", ""), s.background);
				ParseColour(Str(doc, "Panel", ""), s.panel);
				ParseColour(Str(doc, "Field", ""), s.field);
				ParseColour(Str(doc, "Accent", ""), s.accent);
				ParseColour(Str(doc, "Text", ""), s.text);
				s.heading = s.border = s.slider = s.selection = ImVec4();	// a file's optional parts are its own, never Default's
				ParseColour(Str(doc, "Heading", ""), s.heading);
				ParseColour(Str(doc, "Border", ""), s.border);
				ParseColour(Str(doc, "Slider", ""), s.slider);
				ParseColour(Str(doc, "Selection", ""), s.selection);
				s.rounding = ImClamp(Num(doc, "Rounding", s.rounding), 0.0f, 16.0f);
				s.opacity = ImClamp(Num(doc, "Opacity", s.opacity), 0.5f, 1.0f);
				s.image = Str(doc, "Image", "");
				s.imageOpacity = ImClamp(Num(doc, "ImageOpacity", s.imageOpacity), 0.0f, 1.0f);
				s.fit = Str(doc, "ImageFit", "cover");
				bool replaced = false;
				for (auto& existing : Skins) if (_stricmp(existing.name.c_str(), s.name.c_str()) == 0) { existing = s; replaced = true; }
				if (!replaced) Skins.push_back(s);
			}
			catch (const std::exception& err) {
				Logger::Log("MenuSkin: cannot read %s: %s", entry.path().string().c_str(), err.what());
			}
		}
	}

	void LoadPicture(const Skin& s) {
		const std::string path = s.image.empty() || s.file.empty() ? std::string()
			: (std::filesystem::path(s.file).parent_path() / s.image).string();	// a full path in Image replaces the folder
		if (path == PicturePath && (Picture || path.empty())) return;	// already loaded, or none either way
		if (Picture) { Picture->Release(); Picture = nullptr; }
		PicturePath = path;
		if (path.empty()) return;
		D3DXIMAGE_INFO info = {};
		if (FAILED(D3DXCreateTextureFromFileExA(TheRenderManager->device, path.c_str(), D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, 0,
				D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, &info, nullptr, &Picture))) {
			Logger::Log("MenuSkin: cannot load the background picture %s", path.c_str());
			Picture = nullptr;
			return;
		}
		PictureSize = ImVec2((float)info.Width, (float)info.Height);
	}

	void ApplySkin(const Skin& s) {
		ImGuiStyle& style = ImGui::GetStyle();
		style = ImGuiStyle();	// the stock sizes, then ours

		style.WindowPadding = ImVec2(14.0f, 12.0f);
		style.FramePadding = ImVec2(10.0f, 4.0f);
		style.CellPadding = ImVec2(8.0f, 5.0f);
		style.ItemSpacing = ImVec2(8.0f, 7.0f);
		style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
		style.IndentSpacing = 0.0f;			// the settings list indents its own groups
		style.ScrollbarSize = 11.0f;
		style.GrabMinSize = 9.0f;
		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 0.0f;
		style.TabBorderSize = 0.0f;
		style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.ColorButtonPosition = ImGuiDir_Right;
		style.DisabledAlpha = 0.5f;

		const float r = s.rounding;
		style.WindowRounding = r;
		style.ChildRounding = r;
		style.PopupRounding = r;
		style.FrameRounding = r * 0.6f;
		style.GrabRounding = r * 0.6f;
		style.TabRounding = r * 0.6f;
		style.ScrollbarRounding = r;

		const ImVec4 bg = s.background, panel = s.panel, field = s.field, acc = s.accent, text = s.text;
		const ImVec4 border = BorderOf(s), slider = SliderOf(s), sel = SelectionOf(s);
		const ImVec4 black(0.0f, 0.0f, 0.0f, 1.0f), white(1.0f, 1.0f, 1.0f, 1.0f);
		const float childAlpha = Picture ? 0.55f : 1.0f;	// a background picture shows through the panels
		ImVec4* c = style.Colors;
		c[ImGuiCol_Text] = text;
		c[ImGuiCol_TextDisabled] = Mix(text, bg, 0.5f);
		c[ImGuiCol_WindowBg] = Alpha(bg, s.opacity);
		c[ImGuiCol_ChildBg] = Alpha(panel, childAlpha);
		c[ImGuiCol_PopupBg] = Alpha(Mix(bg, white, 0.03f), 0.98f);
		c[ImGuiCol_Border] = Alpha(border, 1.0f);
		c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		c[ImGuiCol_FrameBg] = field;
		c[ImGuiCol_FrameBgHovered] = Mix(field, acc, 0.15f);
		c[ImGuiCol_FrameBgActive] = Mix(field, acc, 0.25f);
		c[ImGuiCol_TitleBg] = Mix(bg, black, 0.25f);
		c[ImGuiCol_TitleBgActive] = Mix(bg, acc, 0.12f);
		c[ImGuiCol_TitleBgCollapsed] = Alpha(Mix(bg, black, 0.25f), 0.75f);
		c[ImGuiCol_MenuBarBg] = panel;
		c[ImGuiCol_ScrollbarBg] = Alpha(bg, 0.0f);
		c[ImGuiCol_ScrollbarGrab] = Mix(field, text, 0.12f);
		c[ImGuiCol_ScrollbarGrabHovered] = Mix(field, text, 0.25f);
		c[ImGuiCol_ScrollbarGrabActive] = Mix(field, acc, 0.6f);
		c[ImGuiCol_CheckMark] = acc;
		c[ImGuiCol_SliderGrab] = slider;
		c[ImGuiCol_SliderGrabActive] = Mix(slider, white, 0.2f);
		c[ImGuiCol_Button] = Mix(field, text, 0.04f);
		c[ImGuiCol_ButtonHovered] = Mix(field, acc, 0.3f);
		c[ImGuiCol_ButtonActive] = Mix(field, acc, 0.5f);
		c[ImGuiCol_Header] = sel;
		c[ImGuiCol_HeaderHovered] = Mix(sel, acc, 0.12f);
		c[ImGuiCol_HeaderActive] = Mix(sel, acc, 0.24f);
		c[ImGuiCol_Separator] = border;
		c[ImGuiCol_SeparatorHovered] = Mix(bg, acc, 0.6f);
		c[ImGuiCol_SeparatorActive] = acc;
		c[ImGuiCol_ResizeGrip] = Alpha(acc, 0.15f);
		c[ImGuiCol_ResizeGripHovered] = Alpha(acc, 0.5f);
		c[ImGuiCol_ResizeGripActive] = Alpha(acc, 0.8f);
		c[ImGuiCol_Tab] = Mix(bg, field, 0.5f);
		c[ImGuiCol_TabHovered] = Mix(field, acc, 0.3f);
		c[ImGuiCol_TabActive] = Mix(field, acc, 0.18f);
		c[ImGuiCol_TabUnfocused] = Mix(bg, field, 0.4f);
		c[ImGuiCol_TabUnfocusedActive] = Mix(bg, field, 0.8f);
		c[ImGuiCol_DockingPreview] = Alpha(acc, 0.5f);
		c[ImGuiCol_DockingEmptyBg] = bg;
		c[ImGuiCol_PlotLines] = Mix(text, bg, 0.3f);
		c[ImGuiCol_PlotLinesHovered] = acc;
		c[ImGuiCol_PlotHistogram] = acc;
		c[ImGuiCol_PlotHistogramHovered] = Mix(acc, white, 0.2f);
		c[ImGuiCol_TableHeaderBg] = Mix(panel, field, 0.5f);
		c[ImGuiCol_TableBorderStrong] = Mix(border, text, 0.04f);
		c[ImGuiCol_TableBorderLight] = Mix(border, bg, 0.35f);
		c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		c[ImGuiCol_TableRowBgAlt] = Alpha(text, 0.03f);
		c[ImGuiCol_TextSelectedBg] = Alpha(acc, 0.35f);
		c[ImGuiCol_DragDropTarget] = acc;
		c[ImGuiCol_NavHighlight] = acc;
		c[ImGuiCol_NavWindowingHighlight] = Alpha(acc, 0.7f);
		c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.4f);
		c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
	}

	// Every Custom<Part> value as one string: a change in any of them rebuilds Active.
	std::string CustomValues() {
		std::string all;
		char v[80];
		for (const Part& p : Parts) { all += TheSettingManager->GetSettingS(Section, p.key, v); all += '|'; }
		char big[1024];
		for (const char* k : OtherKeys) { all += TheSettingManager->GetSettingS(Section, k, big); all += '|'; }
		return all;
	}

	// Active = the picked skin with the Custom<Part> settings on top, then the picture and the ImGui style from it.
	void Rebuild() {
		Active = Skins[Current];
		char v[80];
		for (const Part& p : Parts) {
			ImVec4 colour;
			if (ParseColour(TheSettingManager->GetSettingS(Section, p.key, v), colour)) Active.*p.member = colour;
		}
		const float rounding = TheSettingManager->GetSettingF(Section, "CustomRounding");
		if (rounding >= 0.0f) Active.rounding = ImClamp(rounding, 0.0f, 16.0f);
		const float opacity = TheSettingManager->GetSettingF(Section, "CustomOpacity");
		if (opacity > 0.0f) Active.opacity = ImClamp(opacity, 0.5f, 1.0f);
		char imageText[1024];	// may be a full path
		const std::string image = TheSettingManager->GetSettingS(Section, "CustomImage", imageText);
		if (_stricmp(image.c_str(), "none") == 0) Active.image.clear();
		else if (!image.empty()) { Active.image = image; Active.file = SkinDir() + "(own picture)"; }	// relative to the Skins folder
		const float imageOpacity = TheSettingManager->GetSettingF(Section, "CustomImageOpacity");
		if (imageOpacity >= 0.0f) Active.imageOpacity = ImClamp(imageOpacity, 0.0f, 1.0f);
		const std::string fit = TheSettingManager->GetSettingS(Section, "CustomImageFit", v);
		if (!fit.empty()) Active.fit = fit;
		SeenCustom = CustomValues();
		LoadPicture(Active);
		ApplySkin(Active);
	}

	void Select(int index) {
		Current = ImClamp(index, 0, (int)Skins.size() - 1);
		Rebuild();
	}

	void Store(const char* key, const std::string& value) {
		TheSettingManager->SetSettingS(Section, key, value.c_str());
		TheSettingManager->LoadSettings();
		Rebuild();
	}
	void StoreNumber(const char* key, float value) {
		TheSettingManager->SetSetting(Section, key, value);
		TheSettingManager->LoadSettings();
		Rebuild();
	}

	// One colour part: a tick (own colour on/off) and a colour box. While off, the box shows the skin's colour greyed and
	// changing it turns the own colour on. label follows the box ("" on the settings page, whose row has its own name).
	bool PartControl(const Part& p, float width, const char* label) {
		char v[80];
		ImVec4 own;
		const bool isOwn = ParseColour(TheSettingManager->GetSettingS(Section, p.key, v), own);
		const ImVec4 shown = isOwn ? own : p.resolve(Active);
		ImGui::PushID(p.key);
		bool tick = isOwn, hovered = false;
		if (ImGui::Checkbox("##own", &tick)) Store(p.key, tick ? ColourText(shown) : std::string());
		if (ImGui::IsItemHovered()) {
			hovered = true;
			ImGui::SetTooltip(isOwn ? "Your own colour: untick to use the skin's again." : "Tick for your own colour instead of the skin's.");
		}
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		float rgb[3] = { shown.x, shown.y, shown.z };
		ImGui::SetNextItemWidth(ImMax(width - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x, 1.0f));
		const std::string id = std::string(label) + "##colour";
		if (!isOwn) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
		if (ImGui::ColorEdit3(id.c_str(), rgb, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB))
			Store(p.key, ColourText(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f)));
		if (!isOwn) ImGui::PopStyleVar();
		if (ImGui::IsItemHovered()) {
			hovered = true;
			if (*label) ImGui::SetTooltip("%s", p.help);
		}
		ImGui::PopID();
		return hovered;
	}

	// A number part: a tick (own value on/off) and a slider. Off = the skin's value (shown greyed); off is the value saved for
	// "the skin's" (-1, or 0 for the opacity).
	void NumberControl(const char* key, const char* label, float skinValue, float off, float lo, float hi, const char* format, const char* help) {
		const float saved = TheSettingManager->GetSettingF(Section, key);
		const bool isOwn = off < 0.0f ? saved >= 0.0f : saved > off;
		ImGui::PushID(key);
		bool tick = isOwn;
		if (ImGui::Checkbox("##own", &tick)) StoreNumber(key, tick ? skinValue : off);
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		float value = isOwn ? ImClamp(saved, lo, hi) : skinValue;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
		if (!isOwn) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
		if (ImGui::SliderFloat(label, &value, lo, hi, format)) StoreNumber(key, value);
		if (!isOwn) ImGui::PopStyleVar();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
		ImGui::PopID();
	}

	// 2026-11-07 background picture chooser (View > Skin > Own colours). Pictures live in Data\VegasShaders\Skins\Backgrounds\;
	// CustomImage stores them relative to the Skins folder ("Backgrounds\<file>"). Browse... copies the picked file there, so the
	// setting stays short and the install keeps its own copy. The file dialog runs on its own thread: the game keeps drawing.
	std::string BackgroundDir() { return SkinDir() + "Backgrounds\\"; }
	bool IsPictureFile(const std::filesystem::path& f) {
		std::string ext = f.extension().string();
		for (char& c : ext) c = (char)tolower((unsigned char)c);
		return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || ext == ".bmp" || ext == ".tga";
	}
	std::vector<std::string> BackgroundFiles;	// "Backgrounds\x.png" (and pictures straight in the Skins folder: "x.png")
	void ScanBackgrounds() {
		BackgroundFiles.clear();
		std::error_code ec;
		std::filesystem::create_directories(BackgroundDir(), ec);
		for (const auto& e : std::filesystem::directory_iterator(BackgroundDir(), ec))
			if (e.is_regular_file(ec) && IsPictureFile(e.path())) BackgroundFiles.push_back("Backgrounds\\" + e.path().filename().string());
		for (const auto& e : std::filesystem::directory_iterator(SkinDir(), ec))
			if (e.is_regular_file(ec) && IsPictureFile(e.path())) BackgroundFiles.push_back(e.path().filename().string());
		std::sort(BackgroundFiles.begin(), BackgroundFiles.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
	}
	std::atomic<int> BrowseState{ 0 };	// 0 idle, 1 dialog open, 2 a file was picked (BrowsedFile)
	std::mutex BrowseLock;
	std::string BrowsedFile, BrowseMessage;
	void StartBrowse() {
		int idle = 0;
		if (!BrowseState.compare_exchange_strong(idle, 1)) return;
		const HWND owner = ImGuiManager::GetWindow();
		std::thread([owner]() {
			char file[MAX_PATH] = {};
			OPENFILENAMEA ofn = {};
			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = owner;
			ofn.lpstrFilter = "Pictures (png, jpg, dds, bmp, tga)\0*.png;*.jpg;*.jpeg;*.dds;*.bmp;*.tga\0All files\0*.*\0";
			ofn.lpstrFile = file;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrTitle = "Vegas Shaders: menu background picture (best 1920x1080 or your screen size)";
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;	// NOCHANGEDIR: the game's folder stays the current one
			if (GetOpenFileNameA(&ofn)) {
				std::lock_guard<std::mutex> lock(BrowseLock);
				BrowsedFile = file;
				BrowseState = 2;
			}
			else BrowseState = 0;
		}).detach();
	}
	// On the render thread: copy a picked file into the Backgrounds folder and use it.
	void FinishBrowse() {
		if (BrowseState != 2) return;
		std::string picked;
		{ std::lock_guard<std::mutex> lock(BrowseLock); picked = BrowsedFile; }
		BrowseState = 0;
		std::error_code ec;
		const std::filesystem::path from(picked);
		std::filesystem::create_directories(BackgroundDir(), ec);
		const std::filesystem::path to = std::filesystem::path(BackgroundDir()) / from.filename();
		ec.clear();
		const bool same = std::filesystem::equivalent(from, to, ec);
		ec.clear();
		if (!same) std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) {
			BrowseMessage = "Could not copy " + picked + " (" + ec.message() + ")";
			Logger::Log("MenuSkin: %s", BrowseMessage.c_str());
			return;
		}
		BrowseMessage.clear();
		ScanBackgrounds();
		Store("CustomImage", "Backgrounds\\" + from.filename().string());
		if (!Picture) BrowseMessage = "Could not load " + from.filename().string() + " (png, jpg, dds, bmp or tga only)";
	}

	void DrawBackgroundPicker() {
		FinishBrowse();
		static bool scanned = false;
		if (!scanned) { ScanBackgrounds(); scanned = true; }
		char text[1024];
		const std::string image = TheSettingManager->GetSettingS(Section, "CustomImage", text);
		const bool none = _stricmp(image.c_str(), "none") == 0;
		const std::string shown = image.empty() ? "the skin's" : none ? "no picture" : std::filesystem::path(image).filename().string();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
		if (ImGui::BeginCombo("Background picture", shown.c_str())) {
			if (ImGui::Selectable("the skin's", image.empty())) Store("CustomImage", "");
			if (ImGui::Selectable("no picture", none)) Store("CustomImage", "none");
			for (const std::string& f : BackgroundFiles) {
				const std::string name = std::filesystem::path(f).filename().string();
				if (ImGui::Selectable((name + "##" + f).c_str(), _stricmp(f.c_str(), image.c_str()) == 0)) Store("CustomImage", f);
			}
			if (BackgroundFiles.empty()) ImGui::TextDisabled("No pictures yet: put some in %s", BackgroundDir().c_str());
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pictures in %s\n(png, jpg, dds, bmp or tga). Best size: your screen size, e.g. 1920x1080 (16:9);\n"
			"with Picture fit = cover any size works, the overflow is cropped. Above 2560x1440 only costs memory.", BackgroundDir().c_str());
		const bool open = BrowseState == 1;
		if (open) ImGui::BeginDisabled();
		if (ImGui::Button(open ? "Choosing..." : "Browse...")) StartBrowse();
		if (open) ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Pick a picture anywhere on your PC: it is copied into the Backgrounds folder and used.\n"
				"In exclusive fullscreen the window may open behind the game: Alt+Tab to it, or use the folder instead.");
		ImGui::SameLine();
		if (ImGui::Button("Open folder")) {
			std::error_code ec;
			std::filesystem::create_directories(BackgroundDir(), ec);
			ShellExecuteA(nullptr, "open", BackgroundDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens %s: drop pictures there, then Refresh.", BackgroundDir().c_str());
		ImGui::SameLine();
		if (ImGui::Button("Refresh")) ScanBackgrounds();
		ImGui::TextDisabled("Best size: your screen size, e.g. 1920x1080 (16:9).");
		if (Picture) ImGui::TextDisabled("Now showing: %d x %d", (int)PictureSize.x, (int)PictureSize.y);
		if (!BrowseMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", BrowseMessage.c_str());
	}

	// View > Skin > Own colours: every part of the look, each on top of whichever skin is picked.
	void DrawOwnColours() {
		ImGui::TextDisabled("Ticked parts replace the skin's own, whichever skin is picked.");
		const float width = ImGui::GetFontSize() * 12.0f;
		for (const Part& p : Parts) PartControl(p, width, p.label);
		ImGui::Separator();
		const Skin& base = Skins[Current];
		NumberControl("CustomRounding", "Corner rounding", base.rounding, -1.0f, 0.0f, 16.0f, "%.1f px", "Corner radius in pixels, 0 = square.");
		NumberControl("CustomOpacity", "Window opacity", base.opacity, 0.0f, 0.5f, 1.0f, "%.2f", "How solid the window is (0.5-1).");
		DrawBackgroundPicker();
		NumberControl("CustomImageOpacity", "Picture strength", base.imageOpacity, -1.0f, 0.0f, 1.0f, "%.2f",
			"How strongly the background picture shows (a skin file's Image, or CustomImage on the Main.Menu.Style page).");
		static const char* const fits[] = { "cover", "contain", "stretch" };
		char v[80];
		const std::string fit = TheSettingManager->GetSettingS(Section, "CustomImageFit", v);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
		if (ImGui::BeginCombo("Picture fit", fit.empty() ? "the skin's" : fit.c_str())) {
			if (ImGui::Selectable("the skin's", fit.empty())) Store("CustomImageFit", "");
			for (const char* f : fits) if (ImGui::Selectable(f, fit == f)) Store("CustomImageFit", f);
			ImGui::EndCombo();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Use the skin's own for everything")) {
			for (const Part& p : Parts) TheSettingManager->SetSettingS(Section, p.key, "");
			TheSettingManager->SetSetting(Section, "CustomRounding", -1.0f);
			TheSettingManager->SetSetting(Section, "CustomOpacity", 0.0f);
			TheSettingManager->SetSettingS(Section, "CustomImage", "");
			TheSettingManager->SetSetting(Section, "CustomImageOpacity", -1.0f);
			TheSettingManager->SetSettingS(Section, "CustomImageFit", "");
			TheSettingManager->LoadSettings();
			Rebuild();
		}
	}

	// The current skin as a file in the skin folder, to edit by hand. Returns the path ("" on failure).
	std::string Export() {
		const Skin& s = Active;	// the picked skin with the own parts on top
		std::error_code ec;
		std::filesystem::create_directories(SkinDir(), ec);
		std::string base = s.name;
		for (char& ch : base) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
		std::string path = SkinDir() + base + " (copy).toml";
		for (int n = 2; std::filesystem::exists(path); n++) path = SkinDir() + base + " (copy " + std::to_string(n) + ").toml";
		FILE* f = nullptr;
		if (fopen_s(&f, path.c_str(), "w") || !f) return "";
		auto col = [](const ImVec4& v) { return "\"" + ColourText(v) + "\""; };
		// The picture: its name when it is in the Skins folder, else its full path (forward slashes: TOML strings take no backslashes)
		std::string image = s.image;
		if (!image.empty() && !s.file.empty()) {
			const std::filesystem::path full = std::filesystem::path(s.file).parent_path() / image;
			const bool here = _stricmp(full.parent_path().string().c_str(), std::filesystem::path(SkinDir()).parent_path().string().c_str()) == 0;
			image = here ? full.filename().string() : full.string();
			std::replace(image.begin(), image.end(), '\\', '/');
		}
		fprintf(f, "# Menu skin: edit the values, then pick it from the Skin dropdown (it re-reads this folder when opened).\n");
		fprintf(f, "Name = \"%s (copy)\"\n", s.name.c_str());
		fprintf(f, "Background = %s     # the window, R,G,B 0-255\n", col(s.background).c_str());
		fprintf(f, "Panel = %s          # the sidebar and the settings list\n", col(s.panel).c_str());
		fprintf(f, "Field = %s          # sliders, boxes, buttons\n", col(s.field).c_str());
		fprintf(f, "Accent = %s         # ticks, slider handles, the selected entry\n", col(s.accent).c_str());
		fprintf(f, "Text = %s\n", col(s.text).c_str());
		// The optional parts: commented out (with the mixed colour shown) when the look leaves them to the mix
		fprintf(f, "%sHeading = %s        # titles and group headings (optional)\n", Set(s.heading) ? "" : "# ", col(HeadingOf(s)).c_str());
		fprintf(f, "%sBorder = %s         # borders and separators (optional)\n", Set(s.border) ? "" : "# ", col(BorderOf(s)).c_str());
		fprintf(f, "%sSlider = %s         # slider handles (optional)\n", Set(s.slider) ? "" : "# ", col(SliderOf(s)).c_str());
		fprintf(f, "%sSelection = %s      # the selected entry's bar (optional)\n", Set(s.selection) ? "" : "# ", col(SelectionOf(s)).c_str());
		fprintf(f, "Rounding = %.1f              # corner radius in pixels, 0 = square\n", s.rounding);
		fprintf(f, "Opacity = %.2f              # the window's opacity, 0.5-1\n", s.opacity);
		fprintf(f, "Image = \"%s\"                 # optional background picture in this folder (png, jpg, dds, bmp)\n", image.c_str());
		fprintf(f, "ImageOpacity = %.2f         # 0-1\n", s.imageOpacity);
		fprintf(f, "ImageFit = \"%s\"           # cover, contain or stretch\n", s.fit.c_str());
		fclose(f);
		return path;
	}
}

void Apply() {
	Scan();
	char saved[80] = {};
	TheSettingManager->GetSettingS(Section, "Skin", saved);
	int index = 0;
	for (int i = 0; i < (int)Skins.size(); i++) if (_stricmp(Skins[i].name.c_str(), saved) == 0) index = i;
	Select(index);
}

void DrawBackground() {
	// The saved choice can change without the dropdown (a preset, the console's SetSettingS, a test shot list): follow it.
	// Read again only when a setting changed (SettingsRevision); the own parts (Custom<Part>) are followed the same way.
	if (SeenRevision != TheSettingManager->SettingsRevision && !Skins.empty()) {
		SeenRevision = TheSettingManager->SettingsRevision;
		char saved[80] = {};
		TheSettingManager->GetSettingS(Section, "Skin", saved);
		bool selected = false;
		if (saved[0] != '"' && _stricmp(saved, Skins[Current].name.c_str()) != 0) {
			if (std::none_of(Skins.begin(), Skins.end(), [&](const Skin& k) { return _stricmp(k.name.c_str(), saved) == 0; })) Scan();
			for (int i = 0; i < (int)Skins.size(); i++) if (_stricmp(Skins[i].name.c_str(), saved) == 0) { Select(i); selected = true; break; }
		}
		if (!selected && CustomValues() != SeenCustom) Rebuild();
	}
	if (!Picture) return;
	const Skin& s = Active;
	const ImVec2 wpos = ImGui::GetWindowPos(), wsize = ImGui::GetWindowSize();
	ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f), pmin = wpos, pmax(wpos.x + wsize.x, wpos.y + wsize.y);
	const float winAspect = wsize.x / ImMax(wsize.y, 1.0f), picAspect = PictureSize.x / ImMax(PictureSize.y, 1.0f);
	if (s.fit == "contain") {
		if (picAspect > winAspect) { const float h = wsize.x / picAspect; pmin.y += (wsize.y - h) * 0.5f; pmax.y = pmin.y + h; }
		else { const float w = wsize.y * picAspect; pmin.x += (wsize.x - w) * 0.5f; pmax.x = pmin.x + w; }
	}
	else if (s.fit != "stretch") {	// cover: crop the overflow
		if (picAspect > winAspect) { const float u = winAspect / picAspect; uv0.x = (1.0f - u) * 0.5f; uv1.x = uv0.x + u; }
		else { const float v = picAspect / winAspect; uv0.y = (1.0f - v) * 0.5f; uv1.y = uv0.y + v; }
	}
	ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)Picture, pmin, pmax, uv0, uv1,
		ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, s.imageOpacity)), ImGui::GetStyle().WindowRounding);
}

void DrawPicker() {
	static std::string exported;
	ImGui::Text("Skin:");
	ImGui::SameLine();
	float longest = 0.0f;
	for (const Skin& k : Skins) longest = ImMax(longest, ImGui::CalcTextSize(k.name.c_str()).x);
	ImGui::SetNextItemWidth(longest + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 4.0f);
	if (ImGui::BeginCombo("##skin", Skins.empty() ? "" : Skins[Current].name.c_str())) {
		if (ImGui::IsWindowAppearing()) {	// pick up files added while the game runs
			const std::string name = Skins[Current].name;
			Scan();
			Current = 0;
			for (int i = 0; i < (int)Skins.size(); i++) if (Skins[i].name == name) Current = i;
		}
		for (int i = 0; i < (int)Skins.size(); i++) {
			if (ImGui::Selectable(Skins[i].name.c_str(), i == Current)) {
				Select(i);
				TheSettingManager->SetSettingS(Section, "Skin", Skins[i].name.substr(0, 79).c_str());
				TheSettingManager->LoadSettings();
			}
			if (ImGui::IsItemHovered() && !Skins[i].file.empty()) ImGui::SetTooltip("%s", Skins[i].file.c_str());
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Export")) exported = Export();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(exported.empty() ? "Writes this skin to %s as a file to edit: your own colours and a background picture."
			: "Written to %s", exported.empty() ? SkinDir().c_str() : exported.c_str());
}

void DrawMenu() {
	static std::string exported;
	if (ImGui::IsWindowAppearing()) {	// the submenu just opened: pick up files added while the game runs
		const std::string name = Skins.empty() ? std::string() : Skins[Current].name;
		Scan();
		Current = 0;
		for (int i = 0; i < (int)Skins.size(); i++) if (Skins[i].name == name) Current = i;
	}
	for (int i = 0; i < (int)Skins.size(); i++) {
		if (ImGui::MenuItem(Skins[i].name.c_str(), nullptr, i == Current)) {
			Select(i);
			TheSettingManager->SetSettingS(Section, "Skin", Skins[i].name.substr(0, 79).c_str());
			TheSettingManager->LoadSettings();
		}
		if (ImGui::IsItemHovered() && !Skins[i].file.empty()) ImGui::SetTooltip("%s", Skins[i].file.c_str());
	}
	ImGui::Separator();
	if (ImGui::BeginMenu("Own colours")) {
		DrawOwnColours();
		ImGui::EndMenu();
	}
	if (ImGui::MenuItem("Export this skin...")) exported = Export();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(exported.empty() ? "Writes this look (the skin and your own colours) to %s as a file to edit: colours and a background picture."
			: "Written to %s", exported.empty() ? SkinDir().c_str() : exported.c_str());
}

ImVec4 Heading(float towardText) {
	if (Skins.empty()) return ImVec4(1.0f, 0.85f, 0.45f, 1.0f);
	if (!Set(Active.heading)) return Mix(Active.accent, Active.text, towardText);	// no heading colour: the accent, as before
	return Mix(Active.heading, Active.text, towardText * 0.5f);
}

bool DrawColourSetting(const char* section, const char* key, const char* value, float width, bool& hovered) {
	(void)value;	// PartControl reads the saved value itself
	if (_stricmp(section, Section) != 0 || Skins.empty()) return false;
	for (const Part& p : Parts) {
		if (_stricmp(key, p.key) != 0) continue;
		hovered = PartControl(p, width, "");
		return true;
	}
	return false;
}

void Release() {
	if (Picture) { Picture->Release(); Picture = nullptr; }
}

}
