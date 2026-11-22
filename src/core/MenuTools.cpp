#include "MenuTools.h"
#include "Branding.h"
#include "MenuSkin.h"
#include "../base/PluginVersion.h"
#include "imgui_internal.h"	// ImMax / ImClamp; brings imgui.h
#include <algorithm>
#include <fstream>
#include <iterator>

// Tool windows of the settings menu. Everything here is drawn from MenuTools::DrawWindows, after the main menu window, and only
// while the menu is open. Window titles come from Branding.h (their ### IDs keep each window's saved place in imgui.ini).

bool MenuTools::PresetsOpen = false;
bool MenuTools::LightingOpen = false;
bool MenuTools::DevToolsOpen = false;
bool MenuTools::EncabOpen = false;
bool MenuTools::ScreenshotMode = false;

namespace {

// ---- small drawing helpers ----------------------------------------------------------------------------------------------

// Greys out (and blocks) the widgets drawn while it lives, when asked to; does nothing otherwise.
struct GreyedOut {
	bool active;
	explicit GreyedOut(bool when) : active(when) { if (active) ImGui::BeginDisabled(); }
	~GreyedOut() { if (active) ImGui::EndDisabled(); }
};

// A breathing gap with a rule through it: space, line, space.
void RuledGap() {
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
}

// The next widget goes on the current line.
inline void Beside(float gap = -1.0f) { ImGui::SameLine(0.0f, gap); }

// A hover note for the widget just drawn.
void HoverNote(const char* fmt, ...) {
	if (!ImGui::IsItemHovered()) return;
	va_list args;
	va_start(args, fmt);
	ImGui::SetTooltipV(fmt, args);
	va_end(args);
}

// A tool window, sized and placed only the first time it ever shows. The body runs while it is showing; with closesAtOnce it is
// skipped on the frame the window's x is clicked. Returns true on that frame.
// A full-width bordered box of the given height that scrolls on its own, filled by the callback.
template <class Fill>
void Boxed(const char* id, float height, ImGuiWindowFlags flags, Fill&& fill) {
	ImGui::BeginChild(id, ImVec2(0.0f, height), true, flags);
	fill();
	ImGui::EndChild();
}

template <class Body>
bool ToolWindow(const char* title, bool& open, const ImVec2& size, const ImVec2& at, ImGuiWindowFlags flags, bool closesAtOnce, Body&& body) {
	if (!open) return false;
	ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(at, ImGuiCond_FirstUseEver);
	const bool showing = ImGui::Begin(title, &open, flags);
	if (showing && (open || !closesAtOnce)) body();
	ImGui::End();
	return !open;
}

// ---- Retro-Encabulator ----------------------------------------------------------------------------------------------------
// Three axes (X colour push, Y focus/dream, Z grime/cinema) that move a fixed set of post-process settings away from where they
// were when it was switched on. The starting values are kept and written back when it is switched off or the menu closes.

// Starting values of the settings the axes move, in the same order as kStartSlots below.
struct EncabStart {
	float saturation, curveR, curveG, curveB;
	float dofBlur, sharpening, bloom;
	float chroma, filmGrain, vignetteDark, vignetteRad;
	float lensStrength, lensSmudge;
	float tmSaturation, tmContrast, godRaysMult, fogAmount;
};
// Stand-ins for the readout before the first start.
const EncabStart kStartDefaults = { -0.1f, 0.9f, 1.0f, 1.0f, 1.0f, 0.75f, 1.0f, 1.0f, 0.3f, 1.2f, 0.6f, 0.4f, 0.0f, 1.0f, 1.0f, 1.0f, 0.4f };

// How far one full axis unit moves each setting (grouped X, Y, Z, as the Scales list shows them).
struct EncabGain {
	float saturation, curveR, curveG, curveB, tmSat, tmContrast;
	float dofBlur, sharpening, bloom, godRays;
	float chroma, filmGrain, vignetteDark, vignetteRad, lensSmudge, lensStrength, fog;
};

struct EncabMemo {	// a saved axis position
	char label[64];
	float at[3];
};

struct Encabulator {
	bool on = false;
	float axis[3] = {};								// X, Y, Z in -1..1
	bool drift = false;								// the LFO moves the axes
	float driftHz = 0.04f;
	float phase[3] = { 0.0f, 2.094f, 4.189f };		// one third of a turn apart
	EncabStart start = kStartDefaults;
	bool haveStart = false;
	EncabGain gain = { 1.00f, 0.20f, 0.05f, 0.15f, 0.05f, 0.03f, 6.0f, 1.5f, 4.0f, 0.5f, 5.0f, 1.3f, 2.5f, 0.5f, 0.7f, 0.5f, 0.3f };
	EncabMemo memos[8] = {};
	int memoCount = 0;
	char memoName[64] = "snapshot";
};
Encabulator gEncab;

// Where a starting value lives, as stored in the settings.
struct StartSlot {
	const char* section;
	const char* key;
	float EncabStart::*value;
};

// Read and written top to bottom (the restore writes in this order too).
const StartSlot kStartSlots[] = {
	{ "Shaders.Coloring.Default", "Saturation", &EncabStart::saturation },
	{ "Shaders.Coloring.Default", "ColorCurveR", &EncabStart::curveR },
	{ "Shaders.Coloring.Default", "ColorCurveG", &EncabStart::curveG },
	{ "Shaders.Coloring.Default", "ColorCurveB", &EncabStart::curveB },
	{ "Shaders.DepthOfField.FirstPersonView", "BaseBlurRadius", &EncabStart::dofBlur },
	{ "Shaders.Sharpening.Main", "Strength", &EncabStart::sharpening },
	{ "Shaders.Bloom.Main", "Strength", &EncabStart::bloom },
	{ "Shaders.Cinema.Main", "ChromaticAberration", &EncabStart::chroma },
	{ "Shaders.Cinema.Main", "FilmGrainAmount", &EncabStart::filmGrain },
	{ "Shaders.Cinema.Main", "VignetteDarkness", &EncabStart::vignetteDark },
	{ "Shaders.Cinema.Main", "VignetteRadius", &EncabStart::vignetteRad },
	{ "Shaders.Lens.Main", "Strength", &EncabStart::lensStrength },
	{ "Shaders.Lens.Main", "Smudginess", &EncabStart::lensSmudge },
	{ "Shaders.Tonemapping.Main", "Saturation", &EncabStart::tmSaturation },
	{ "Shaders.Tonemapping.Main", "TonemapContrast", &EncabStart::tmContrast },
	{ "Shaders.GodRays.Main", "DayMultiplier", &EncabStart::godRaysMult },
	{ "Shaders.VolumetricFog.Main", "Amount", &EncabStart::fogAmount },
};

// How an axis drives one setting: start + sign * axis * gain, then kept at or above zero, or inside 0..1, or left as is.
enum class Bound { None, NonNegative, Unit };
struct Drive {
	const char* section;
	const char* key;
	float EncabStart::*start;
	float EncabGain::*gain;
	int axis;
	float sign;
	Bound bound;
};

// Written in this order on every update.
const Drive kDrives[] = {
	// X: colour
	{ "Shaders.Coloring.Default", "Saturation", &EncabStart::saturation, &EncabGain::saturation, 0, 1.0f, Bound::None },
	{ "Shaders.Coloring.Default", "ColorCurveR", &EncabStart::curveR, &EncabGain::curveR, 0, 1.0f, Bound::None },
	{ "Shaders.Coloring.Default", "ColorCurveG", &EncabStart::curveG, &EncabGain::curveG, 0, 1.0f, Bound::None },
	{ "Shaders.Coloring.Default", "ColorCurveB", &EncabStart::curveB, &EncabGain::curveB, 0, -1.0f, Bound::None },
	{ "Shaders.Tonemapping.Main", "Saturation", &EncabStart::tmSaturation, &EncabGain::tmSat, 0, 1.0f, Bound::None },
	{ "Shaders.Tonemapping.Main", "TonemapContrast", &EncabStart::tmContrast, &EncabGain::tmContrast, 0, 1.0f, Bound::None },
	// Y: focus / dream
	{ "Shaders.DepthOfField.FirstPersonView", "BaseBlurRadius", &EncabStart::dofBlur, &EncabGain::dofBlur, 1, 1.0f, Bound::NonNegative },
	{ "Shaders.Sharpening.Main", "Strength", &EncabStart::sharpening, &EncabGain::sharpening, 1, -1.0f, Bound::Unit },	// (2026-11-18: CAS sharpness, 0-1)
	{ "Shaders.Bloom.Main", "Strength", &EncabStart::bloom, &EncabGain::bloom, 1, 1.0f, Bound::NonNegative },
	{ "Shaders.GodRays.Main", "DayMultiplier", &EncabStart::godRaysMult, &EncabGain::godRays, 1, 1.0f, Bound::NonNegative },
	// Z: grime / cinema
	{ "Shaders.Cinema.Main", "ChromaticAberration", &EncabStart::chroma, &EncabGain::chroma, 2, 1.0f, Bound::NonNegative },
	{ "Shaders.Cinema.Main", "FilmGrainAmount", &EncabStart::filmGrain, &EncabGain::filmGrain, 2, 1.0f, Bound::NonNegative },
	{ "Shaders.Cinema.Main", "VignetteDarkness", &EncabStart::vignetteDark, &EncabGain::vignetteDark, 2, 1.0f, Bound::NonNegative },
	{ "Shaders.Cinema.Main", "VignetteRadius", &EncabStart::vignetteRad, &EncabGain::vignetteRad, 2, -1.0f, Bound::Unit },
	{ "Shaders.Lens.Main", "Smudginess", &EncabStart::lensSmudge, &EncabGain::lensSmudge, 2, 1.0f, Bound::NonNegative },
	{ "Shaders.Lens.Main", "Strength", &EncabStart::lensStrength, &EncabGain::lensStrength, 2, 1.0f, Bound::NonNegative },
	{ "Shaders.VolumetricFog.Main", "Amount", &EncabStart::fogAmount, &EncabGain::fog, 2, 1.0f, Bound::NonNegative },
};
enum DriveIndex {	// positions in kDrives, for the readout
	kSat, kCurveR, kCurveG, kCurveB, kTmSat, kTmContrast,
	kDof, kSharpen, kBloom, kGodRays,
	kChroma, kGrain, kVigDark, kVigRad, kSmudge, kLens, kFog,
};

float DriveValue(const Drive& d, const EncabStart& from) {
	const float offset = gEncab.axis[d.axis] * (gEncab.gain.*d.gain);
	const float raw = (d.sign < 0.0f) ? (from.*d.start - offset) : (from.*d.start + offset);
	if (d.bound == Bound::NonNegative) return ImMax(0.0f, raw);
	if (d.bound == Bound::Unit) return ImClamp(raw, 0.0f, 1.0f);
	return raw;
}

void EncabTakeStart() {
	if (!TheSettingManager || gEncab.haveStart) return;
	for (const StartSlot& slot : kStartSlots)
		gEncab.start.*slot.value = TheSettingManager->GetSettingF(slot.section, slot.key);
	gEncab.haveStart = true;
}

void EncabPutBackStart() {
	if (!TheSettingManager || !gEncab.haveStart) return;
	for (const StartSlot& slot : kStartSlots)
		TheSettingManager->SetSetting(slot.section, slot.key, gEncab.start.*slot.value);
	TheSettingManager->LoadSettings();
	gEncab.haveStart = false;
}

void EncabSwitchOff() {
	if (!gEncab.on) return;
	gEncab.on = false;
	EncabPutBackStart();
}

void EncabPush() {
	if (!TheSettingManager || !gEncab.haveStart) return;
	for (const Drive& d : kDrives)
		TheSettingManager->SetSetting(d.section, d.key, DriveValue(d, gEncab.start));
	TheSettingManager->LoadSettings();
}

void EncabTick() {
	if (!gEncab.on) return;
	if (gEncab.drift) {
		const float advance = ImGui::GetIO().DeltaTime * gEncab.driftHz * 6.2832f;
		for (int a = 0; a < 3; a++) gEncab.phase[a] += advance;
		for (int a = 0; a < 3; a++) gEncab.axis[a] = sinf(gEncab.phase[a]);
	}
	EncabPush();
}

// The X/Y pad: dragging inside it sets colour (left-right) and focus (up-down).
void EncabPad(bool locked, float side) {
	const ImVec2 corner = ImGui::GetCursorScreenPos();
	const ImVec2 edge(corner.x + side, corner.y + side);
	{
		GreyedOut lock(locked);
		ImGui::InvisibleButton("##xypad", ImVec2(side, side));
		if (ImGui::IsItemActive()) {
			const ImVec2 m = ImGui::GetIO().MousePos;
			gEncab.axis[0] = ImClamp(((m.x - corner.x) / side) * 2.0f - 1.0f, -1.0f, 1.0f);
			gEncab.axis[1] = ImClamp(-((m.y - corner.y) / side) * 2.0f + 1.0f, -1.0f, 1.0f);
		}
	}

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilledMultiColor(corner, edge, IM_COL32(15, 20, 55, 255), IM_COL32(55, 25, 10, 255), IM_COL32(40, 18, 5, 255), IM_COL32(5, 15, 35, 255));
	dl->AddRect(corner, edge, IM_COL32(110, 110, 170, 220), 3.0f, 0, 1.5f);

	const float cx = corner.x + side * 0.5f, cy = corner.y + side * 0.5f;
	const ImU32 axisLine = IM_COL32(70, 70, 100, 160);
	dl->AddLine(ImVec2(corner.x, cy), ImVec2(edge.x, cy), axisLine);
	dl->AddLine(ImVec2(cx, corner.y), ImVec2(cx, edge.y), axisLine);

	const ImU32 dim = IM_COL32(150, 150, 200, 200);
	dl->AddText(ImVec2(corner.x + 5, corner.y + 4), dim, "dream");
	dl->AddText(ImVec2(corner.x + 5, edge.y - 17), dim, "crisp");
	dl->AddText(ImVec2(corner.x + 4, cy - 8), IM_COL32(110, 150, 220, 200), "< cool");
	dl->AddText(ImVec2(edge.x - 44, cy - 8), IM_COL32(220, 160, 90, 200), "warm >");

	const ImVec2 dot(corner.x + (gEncab.axis[0] + 1.0f) * 0.5f * side, corner.y + (1.0f - (gEncab.axis[1] + 1.0f) * 0.5f) * side);
	dl->AddCircleFilled(dot, 9.0f, IM_COL32(255, 200, 50, 240));
	dl->AddCircle(dot, 9.0f, IM_COL32(255, 255, 255, 180), 0, 1.5f);
	const ImU32 cross = IM_COL32(0, 0, 0, 180);
	dl->AddLine(ImVec2(dot.x - 5, dot.y), ImVec2(dot.x + 5, dot.y), cross);
	dl->AddLine(ImVec2(dot.x, dot.y - 5), ImVec2(dot.x, dot.y + 5), cross);
}

// The upright Z (grime) slider next to the pad.
void EncabGrime(bool locked, float side) {
	ImGui::BeginGroup();
	ImGui::TextDisabled("grime");
	{
		GreyedOut lock(locked);
		ImGui::VSliderFloat("##z", ImVec2(28.0f, side - 34.0f), &gEncab.axis[2], -1.0f, 1.0f, "");
		HoverNote("Z  %.2f\ngrime / cinema axis", gEncab.axis[2]);
	}
	ImGui::TextDisabled("clean");
	ImGui::EndGroup();
}

void EncabDriftRow() {
	ImGui::Checkbox("LFO Drift", &gEncab.drift);
	if (!gEncab.drift) return;
	Beside();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::SliderFloat("##rate", &gEncab.driftHz, 0.002f, 0.25f, "%.3f Hz");
	HoverNote("Drift speed. Axes are 120 deg out of phase with each other.");
	Beside();
	if (ImGui::SmallButton("Shuffle"))
		for (float& p : gEncab.phase) p = (float)(rand() % 628) / 100.0f;
	HoverNote("Randomize phase offsets so axes fall out of sync.");
}

void EncabMemos() {
	if (!ImGui::CollapsingHeader("Snapshots")) return;
	ImGui::SetNextItemWidth(160.0f);
	ImGui::InputText("##snapname", gEncab.memoName, sizeof(gEncab.memoName));
	Beside();
	{
		GreyedOut full(!(gEncab.memoCount < 8 && gEncab.memoName[0] != '\0'));
		if (ImGui::SmallButton("Save")) {
			EncabMemo& m = gEncab.memos[gEncab.memoCount++];
			strncpy_s(m.label, gEncab.memoName, sizeof(m.label) - 1);
			std::copy(gEncab.axis, gEncab.axis + 3, m.at);
		}
	}
	ImGui::Spacing();
	for (int n = 0; n < gEncab.memoCount; n++) {
		ImGui::PushID(n);
		const EncabMemo& m = gEncab.memos[n];
		ImGui::Text("%-18s  %+.2f %+.2f %+.2f", m.label, m.at[0], m.at[1], m.at[2]);
		Beside();
		if (ImGui::SmallButton("Recall")) {
			std::copy(m.at, m.at + 3, gEncab.axis);
			gEncab.drift = false;
		}
		Beside();
		if (ImGui::SmallButton("x")) {
			std::copy(gEncab.memos + n + 1, gEncab.memos + gEncab.memoCount, gEncab.memos + n);
			gEncab.memoCount--;
		}
		ImGui::PopID();
	}
	if (gEncab.memoCount == 0) ImGui::TextDisabled("No snapshots saved.");
	ImGui::Spacing();
}

// The gain multipliers, three groups (one per axis); each row has a drag box, -/+ by the menu's step, a reset, then its name.
struct GainRow { const char* name; float EncabGain::*value; float reset; };
struct GainGroup { const char* title; ImVec4 tint; const GainRow* rows; int count; };

const GainRow kColourGains[] = {
	{ "Saturation", &EncabGain::saturation, 1.00f }, { "CurveR", &EncabGain::curveR, 0.20f }, { "CurveG", &EncabGain::curveG, 0.05f },
	{ "CurveB", &EncabGain::curveB, 0.15f }, { "TM Saturation", &EncabGain::tmSat, 0.05f }, { "TM Contrast", &EncabGain::tmContrast, 0.03f },
};
const GainRow kFocusGains[] = {
	{ "DoF Blur", &EncabGain::dofBlur, 6.0f }, { "Sharpening", &EncabGain::sharpening, 1.5f }, { "Bloom", &EncabGain::bloom, 4.0f },
	{ "GodRays", &EncabGain::godRays, 0.5f },
};
const GainRow kGrimeGains[] = {
	{ "Chroma", &EncabGain::chroma, 5.0f }, { "FilmGrain", &EncabGain::filmGrain, 1.3f }, { "VignetteDark", &EncabGain::vignetteDark, 2.5f },
	{ "VignetteRad", &EncabGain::vignetteRad, 0.5f }, { "LensSmudge", &EncabGain::lensSmudge, 0.7f }, { "LensStrength", &EncabGain::lensStrength, 0.5f },
	{ "Fog", &EncabGain::fog, 0.3f },
};
const GainGroup kGainGroups[] = {
	{ "Color Push (X)", ImVec4(0.9f, 0.7f, 0.3f, 1.0f), kColourGains, IM_ARRAYSIZE(kColourGains) },
	{ "Focus/Dream (Y)", ImVec4(0.4f, 0.8f, 1.0f, 1.0f), kFocusGains, IM_ARRAYSIZE(kFocusGains) },
	{ "Grime/Cinema (Z)", ImVec4(0.7f, 0.4f, 0.9f, 1.0f), kGrimeGains, IM_ARRAYSIZE(kGrimeGains) },
};

void EncabGainRow(const GainRow& row, float step) {
	float& v = gEncab.gain.*row.value;
	ImGui::PushID(row.name);
	ImGui::SetNextItemWidth(70.0f);
	ImGui::DragFloat("##s", &v, 0.005f, 0.0f, 0.0f, "%.3f");
	Beside();
	if (ImGui::SmallButton("-")) v = ImMax(0.0f, v - step);
	Beside();
	if (ImGui::SmallButton("+")) v += step;
	Beside();
	if (ImGui::SmallButton("=")) v = row.reset;
	HoverNote("Reset to %.3f", row.reset);
	Beside();
	ImGui::TextUnformatted(row.name);
	ImGui::PopID();
}

void EncabGains(float step) {
	if (!ImGui::CollapsingHeader("Scales")) return;
	for (const GainGroup& group : kGainGroups) {
		ImGui::TextColored(group.tint, "%s", group.title);
		for (int r = 0; r < group.count; r++) EncabGainRow(group.rows[r], step);
		ImGui::Spacing();
	}
}

// What the settings are (or would be) right now: three columns, one per axis, from the start values (defaults before a start).
void EncabReadout() {
	const EncabStart& from = gEncab.haveStart ? gEncab.start : kStartDefaults;
	struct Cell { const char* label; int drive; };	// drive < 0: an empty cell
	static const Cell kGrid[5][3] = {
		{ { "Saturation", kSat }, { "DoF Blur", kDof }, { "Chroma", kChroma } },
		{ { "CurveR", kCurveR }, { "Sharpening", kSharpen }, { "FilmGrain", kGrain } },
		{ { "CurveB", kCurveB }, { "Bloom", kBloom }, { "Vignette", kVigDark } },
		{ { "TM Sat", kTmSat }, { "GodRays", kGodRays }, { "LensSmudge", kSmudge } },
		{ { "TM Contrast", kTmContrast }, { "", -1 }, { "Fog", kFog } },
	};
	if (!ImGui::BeginTable("##cfab", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame))
		return;
	for (const char* heading : { "Color Push  (X)", "Focus/Dream  (Y)", "Grime/Cinema  (Z)" })
		ImGui::TableSetupColumn(heading);
	ImGui::TableHeadersRow();
	for (const auto& line : kGrid) {
		ImGui::TableNextRow();
		for (int c = 0; c < 3; c++) {
			ImGui::TableSetColumnIndex(c);
			if (line[c].drive >= 0) ImGui::Text("%-10s %.3f", line[c].label, DriveValue(kDrives[line[c].drive], from));
		}
	}
	ImGui::EndTable();
}

void EncabBody(float step) {
	if (ImGui::Checkbox("Active", &gEncab.on)) {
		if (gEncab.on) EncabTakeStart();
		else EncabPutBackStart();
	}
	Beside();
	ImGui::TextDisabled("overrides Coloring / Tonemapping / DoF / Bloom / GodRays / Cinema / Fog");
	RuledGap();

	const float side = 220.0f;
	const bool locked = gEncab.drift && gEncab.on;	// the LFO owns the axes
	EncabPad(locked, side);
	Beside(8.0f);
	EncabGrime(locked, side);
	ImGui::Spacing();
	ImGui::Text("X %+.2f   Y %+.2f   Z %+.2f", gEncab.axis[0], gEncab.axis[1], gEncab.axis[2]);
	RuledGap();
	EncabDriftRow();
	RuledGap();
	EncabMemos();
	EncabGains(step);
	ImGui::Separator();
	ImGui::Spacing();
	EncabReadout();
}

void EncabWindow(float step) {
	const ImGuiWindowFlags still = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (ToolWindow("Salamandrastic Retro-Encabulator", MenuTools::EncabOpen, ImVec2(480.0f, 700.0f), ImVec2(980.0f, 60.0f), still, true, [step] { EncabBody(step); }))
		EncabSwitchOff();
}

// ---- Presets: confirmations ------------------------------------------------------------------------------------------------
// A preset button only queues its action here; the modal is opened and answered by PresetConfirmPopup, which runs at the top
// level every frame. OpenPopup and BeginPopupModal hash the popup name against the current window, so both must run in that
// same top-level context (opening it from inside the Presets window gave an ID the modal never matched). It also keeps a
// pending confirm answerable after the Presets window is closed.

enum class PresetAsk { None, Save, Reload, Load, RefreshAll, SaveVariant, Delete };

struct PresetQuestion {
	PresetAsk kind = PresetAsk::None;
	std::string target;		// the preset or Variant it is about
	std::string text;		// what the modal says
	bool openNow = false;	// the modal opens on the next PresetConfirmPopup
};
PresetQuestion gAsk;

struct PresetPreview {		// a Load shows a preset without saving it, until the location changes
	bool active = false;
	std::string name;
	UInt32 generation = 0;	// PresetManager::GetResolveGeneration() when it began
};
PresetPreview gPreview;
std::string gPickedPreset;	// the browser's selected row

void ResolveForPlayerCell() {
	if (Player && Player->parentCell) PresetManager::ResolveAndApply(Player->parentCell);
}

void DoPresetSave(const std::string& target) {
	PresetManager::PresetData captured;
	const char* failure = nullptr;
	if (!PresetManager::CaptureLiveState(captured)) failure = "couldn't capture live state";
	else if (!PresetManager::WritePreset(target, captured)) failure = "couldn't write file";
	if (failure) {
		Logger::Log("PresetManager: [Preset] Save to '%s' failed -- %s", target.c_str(), failure);
		return;
	}
	Logger::Log("PresetManager: [Preset] Saved current settings to '%s'", target.c_str());
	ResolveForPlayerCell();	// the status rows pick up the new file straight away
}

void DoPresetReload() {
	ResolveForPlayerCell();
	Logger::Log("PresetManager: [Preset] Reloaded current preset, discarding unsaved edits");
}

void DoPresetLoad(const std::string& name) {
	PresetManager::PresetData read;
	if (!PresetManager::ReadPreset(name, read)) {
		Logger::Log("PresetManager: [Preset] Load '%s' failed -- couldn't read file", name.c_str());
		return;
	}
	// The preview replaces the live values outright; nothing is written and the location keeps its assignment.
	PresetManager::ApplyPreviewPreset(read);
	gPreview.active = true;
	gPreview.name = name;
	gPreview.generation = PresetManager::GetResolveGeneration();
	Logger::Log("PresetManager: [Preset] Loaded '%s' as an unsaved preview", name.c_str());
}

void DoPresetDelete(const std::string& name) {
	if (!PresetManager::DeletePreset(name)) {
		Logger::Log("PresetManager: [Preset] Delete '%s' failed -- reserved name, missing file, or filesystem error", name.c_str());
		return;
	}
	Logger::Log("PresetManager: [Preset] Deleted '%s'", name.c_str());
	if (gPickedPreset == name) gPickedPreset.clear();
	if (gPreview.active && gPreview.name == name) gPreview.active = false;	// the values stay, the "previewing" label goes
	ResolveForPlayerCell();
}

void DoPresetRefreshAll() {
	const auto touched = PresetManager::RefreshAllPresets();
	for (const auto& file : touched) {
		Logger::Log("PresetManager: [Preset] Refresh backfilled %u key(s) into '%s':", (UInt32)file.AddedKeys.size(), file.PresetName.c_str());
		for (const auto& added : file.AddedKeys) Logger::Log("PresetManager: [Preset]   %s", added.c_str());
	}
	Logger::Log("PresetManager: [Preset] Refresh All Presets done -- %u file(s) touched", (UInt32)touched.size());
}

void DoVariantSave(const std::string& name) {
	PresetManager::PresetData changes;
	PresetManager::CaptureVariantDiff(changes);
	if (changes.empty()) {
		Logger::Log("PresetManager: [Preset] Save Variant '%s' skipped -- no live changes since the base preset loaded", name.c_str());
		return;
	}
	if (!PresetManager::WriteVariant(name, changes)) {
		Logger::Log("PresetManager: [Preset] Save Variant '%s' failed -- couldn't write file", name.c_str());
		return;
	}
	UInt32 keys = 0;
	for (const auto& section : changes) keys += (UInt32)section.second.size();
	Logger::Log("PresetManager: [Preset] Saved Variant '%s' (%u changed key(s))", name.c_str(), keys);
}

void AskFirst(PresetAsk kind, const std::string& target, const std::string& text) {
	gAsk.kind = kind;
	gAsk.target = target;
	gAsk.text = text;
	Logger::Log("PresetManager: [Preset] Confirmation requested for '%s' (kind=%d)", target.c_str(), (int)kind);
	gAsk.openNow = true;
}

void RunAnswered() {
	switch (gAsk.kind) {
	case PresetAsk::Reload: DoPresetReload(); break;
	case PresetAsk::Load: DoPresetLoad(gAsk.target); break;
	case PresetAsk::RefreshAll: DoPresetRefreshAll(); break;
	case PresetAsk::SaveVariant: DoVariantSave(gAsk.target); break;
	case PresetAsk::Delete: DoPresetDelete(gAsk.target); break;
	default: DoPresetSave(gAsk.target); break;	// Save (and anything unset)
	}
}

void PresetConfirmPopup() {
	const char* const id = "Confirm##presetaction";
	if (gAsk.openNow) {
		ImGui::OpenPopup(id);
		gAsk.openNow = false;
	}
	ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f));	// every time (no condition)
	if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::TextWrapped("%s", gAsk.text.c_str());
	ImGui::Spacing();
	const ImVec2 buttonSize(120.0f, 0.0f);
	auto settle = []() {
		gAsk.kind = PresetAsk::None;
		ImGui::CloseCurrentPopup();
	};
	if (ImGui::Button("OK", buttonSize)) {
		Logger::Log("PresetManager: [Preset] Confirm OK clicked for '%s' (kind=%d)", gAsk.target.c_str(), (int)gAsk.kind);
		RunAnswered();
		settle();
	}
	Beside();
	if (ImGui::Button("Cancel", buttonSize)) settle();
	ImGui::EndPopup();
}

// ---- Presets window -------------------------------------------------------------------------------------------------------

const ImVec4 kLiveTint(0.4f, 1.0f, 0.7f, 1.0f);		// the tier in use
const ImVec4 kPreviewTint(1.0f, 0.8f, 0.2f, 1.0f);	// the unsaved-preview banner

void PresetSwitchRow() {
	bool on = TheSettingManager->SettingsMain.Main.PresetManagerEnabled;
	if (ImGui::Checkbox("Preset Manager enabled", &on)) {
		TheSettingManager->SetSetting("Main.Main.Misc", "PresetManagerEnabled", on);
		TheSettingManager->LoadSettings();
		Logger::Log("PresetManager: [Preset] Master toggle set to %s", on ? "enabled" : "disabled");
		if (on) {	// switched back on: resolve here and now, not at the next location change
			PresetManager::ForgetSession();	// UNOFFICIAL 2026-10-19: the settings right now are the player's own
			ResolveForPlayerCell();
		}
	}
	if (!on) ImGui::TextDisabled("Automatic switching is off -- your own settings are authoritative. Save/Load/Delete below still work.");
}

// One tier line: its name (tinted when it is the tier in use) and its Save button in the shared button column.
bool TierLine(const char* name, bool live, float buttonX, const char* button, bool usable) {
	if (live) ImGui::TextColored(kLiveTint, "%s", name);
	else ImGui::Text("%s", name);
	ImGui::SameLine(buttonX);
	GreyedOut off(!usable);
	return ImGui::SmallButton(button);
}

void TierLines(const PresetManager::ResolveResult& here, bool hasKeyword, const char* keywordName, float buttonX) {
	using Tier = PresetManager::ResolvedTier;
	const bool showingTier = !gPreview.active;	// a preview hides which tier is live
	char text[192];

	// Default: always there; it is the floor for every interior or every exterior.
	if (TierLine("[Default]", showingTier && here.Tier == Tier::Default, buttonX, "Save to Default", true)) {
		const std::string target = here.IsInterior ? PresetManager::kDefaultInteriorName : PresetManager::kDefaultExteriorName;
		snprintf(text, sizeof(text), "This will rewrite the floor for %s in the game. Continue?", here.IsInterior ? "every interior" : "every exterior");
		Logger::Log("PresetManager: [Preset] Save to Default clicked, target='%s'", target.c_str());
		AskFirst(PresetAsk::Save, target, text);
	}
	ImGui::Spacing();

	// Keyword: interiors with a keyword tag only (tags are set offline, so an untagged cell has no row at all).
	if (hasKeyword && TierLine(keywordName, showingTier && here.Tier == Tier::Keyword, buttonX, "Save to Keyword", true)) {
		snprintf(text, sizeof(text), "This affects %u cell(s) using keyword '%s'. Continue?", PresetManager::CountCellsForKeyword(here.Keyword), here.Keyword.c_str());
		AskFirst(PresetAsk::Save, here.Keyword, text);
	}
	ImGui::Spacing();

	// Override: when one exists it is always the tier in use.
	const bool hasOverride = here.Tier == Tier::Override;
	const std::string place = here.IsInterior ? here.CellEditorID : here.WorldspaceEditorID;
	if (TierLine("[Override]", showingTier && hasOverride, buttonX, "Save to Override", !place.empty())) {
		if (!hasOverride) DoPresetSave(place);	// a new file: nothing to overwrite, nothing to confirm
		else AskFirst(PresetAsk::Save, place, "An override for this location already exists and will be overwritten. Continue?");
	}
}

void PresetBrowser() {
	ImGui::TextUnformatted("Preset browser");
	const auto onDisk = PresetManager::ListAllPresets();

	Boxed("##presetBrowser", 130.0f, 0, [&] {
		for (const auto& p : onDisk) {
			const char* tag = "[Override] ";
			if (p.Kind == PresetManager::PresetKind::Keyword) tag = "[Keyword] ";
			else if (p.Kind == PresetManager::PresetKind::Default) tag = "[Default] ";
			char row[256];
			snprintf(row, sizeof(row), "%s%s", tag, p.Name.c_str());
			if (ImGui::Selectable(row, gPickedPreset == p.Name)) gPickedPreset = p.Name;
		}
		if (onDisk.empty()) ImGui::TextDisabled("(no presets on disk yet)");
	});

	const bool picked = !gPickedPreset.empty();
	{
		GreyedOut off(!picked);
		if (ImGui::Button("Load")) {
			Logger::Log("PresetManager: [Preset] Load clicked, target='%s'", gPickedPreset.c_str());
			AskFirst(PresetAsk::Load, gPickedPreset, "This will override your current unsaved settings. Continue?");
		}
	}
	Beside();
	{
		GreyedOut off(!picked || PresetManager::IsReservedName(gPickedPreset));	// the built-in presets cannot be deleted
		if (ImGui::Button("Delete")) {
			Logger::Log("PresetManager: [Preset] Delete clicked, target='%s'", gPickedPreset.c_str());
			char text[288];
			snprintf(text, sizeof(text), "Permanently delete '%s'? This cannot be undone.", gPickedPreset.c_str());
			AskFirst(PresetAsk::Delete, gPickedPreset, text);
		}
	}
}

void VariantList() {
	ImGui::TextUnformatted("Variants");
	const auto all = PresetManager::ListAllVariants();
	const auto& chosen = PresetManager::GetEnabledVariants();

	if (all.empty()) ImGui::TextDisabled("(no Variants on disk yet)");
	for (const auto& name : all) {
		bool on = std::find(chosen.begin(), chosen.end(), name) != chosen.end();
		if (ImGui::Checkbox(name.c_str(), &on)) PresetManager::SetVariantEnabled(name, on);
	}

	if (chosen.empty()) return;
	std::string order;
	const char* joint = "";
	for (const auto& name : chosen) {
		order += joint;
		order += name;
		joint = " -> ";
	}
	ImGui::TextDisabled("Priority: %s (left lowest, right highest)", order.c_str());
}

void VariantSaveRow() {
	static char typed[64] = "";
	ImGui::SetNextItemWidth(180.0f);
	ImGui::InputText("##variantname", typed, sizeof(typed));
	Beside();
	if (!ImGui::Button("Save Variant")) return;

	const std::string name(typed);
	if (name.empty())
		Logger::Log("PresetManager: [Preset] Save Variant clicked with an empty name -- ignored");
	else if (PresetManager::IsReservedName(name))
		Logger::Log("PresetManager: [Preset] Save Variant '%s' rejected -- reserved name", name.c_str());
	else if (!PresetManager::VariantExists(name))
		DoVariantSave(name);
	else
		AskFirst(PresetAsk::SaveVariant, name, "A Variant with this name already exists and will be overwritten. Continue?");
}

// Where the player is, and the unsaved-preview banner. A new resolve generation means the location changed, which ends a preview.
void PresetWhereLines() {
	if (gPreview.active && PresetManager::GetResolveGeneration() != gPreview.generation) {
		Logger::Log("PresetManager: [Preset] Unsaved preview of '%s' discarded -- location changed", gPreview.name.c_str());
		gPreview.active = false;
	}
	const auto& here = PresetManager::GetLastResolveResult();
	auto orNone = [](const std::string& id) { return id.empty() ? "(none)" : id.c_str(); };
	ImGui::Text("Cell EditorID:       %s", orNone(here.CellEditorID));
	ImGui::Text("Worldspace EditorID: %s", orNone(here.WorldspaceEditorID));
	ImGui::Text("IsInterior:          %s", here.IsInterior ? "true" : "false");
	if (gPreview.active) ImGui::TextColored(kPreviewTint, "Unsaved -- previewing '%s'", gPreview.name.c_str());
}

// The tier lines, their Save buttons lined up just right of the widest tier name drawn this frame.
void PresetTierBlock() {
	const auto& here = PresetManager::GetLastResolveResult();
	const bool hasKeyword = here.IsInterior && !here.Keyword.empty();
	char keywordName[160];
	snprintf(keywordName, sizeof(keywordName), "[Keyword: %s]", here.Keyword.c_str());
	float widest = (std::max)(ImGui::CalcTextSize("[Default]").x, ImGui::CalcTextSize("[Override]").x);	// (std::max): windows.h defines max
	if (hasKeyword) widest = (std::max)(widest, ImGui::CalcTextSize(keywordName).x);
	TierLines(here, hasKeyword, keywordName, widest + 24.0f);
}

void PresetReloadButton() {
	if (ImGui::Button("Reload current preset"))
		AskFirst(PresetAsk::Reload, "", "This will discard any unsaved live edits and reload what's actually assigned to this location. Continue?");
}

void PresetsWindow() {
	ToolWindow(Branding::PresetsTitle, MenuTools::PresetsOpen, ImVec2(460.0f, 640.0f), ImVec2(540.0f, 380.0f), 0, false, [] {
		ImGui::SetWindowFontScale(1.1f);	// a little larger; set first, so the text widths measured below include it
		void (*const blocks[])() = { PresetSwitchRow, PresetWhereLines, PresetTierBlock, PresetReloadButton, PresetBrowser, VariantList };
		bool first = true;
		for (auto block : blocks) {
			if (!first) ImGui::Separator();	// a rule between blocks
			first = false;
			block();
		}
		ImGui::Spacing();
		VariantSaveRow();
	});
}

// ---- Console commands -----------------------------------------------------------------------------------------------------

void Console(const char* line) {
	if (g_ConsoleInterface) g_ConsoleInterface->RunScriptLine(line, nullptr);
}

// A registered message (unique per name system-wide) rather than a WM_APP number another component might also use.
UINT DeferredConsoleMessage() {
	static const UINT id = RegisterWindowMessageA("NVR_DeferredConsoleCommand");
	return id;
}

// For commands with a loading screen (coc): menu buttons run inside the game's render call for this frame, and a command that
// must present more frames would hang there. Posting it runs it from the game's message loop instead, outside any render call.
void ConsoleLater(const char* line) {
	const HWND target = ImGuiManager::GetWindow();
	if (target) PostMessage(target, DeferredConsoleMessage(), 0, (LPARAM)_strdup(line));
	else Console(line);	// no window yet: run it now, as best we can
}

// ---- Lighting window -------------------------------------------------------------------------------------------------------
// Light, ambient and sky intensity of PBR and Terrain, grouped by condition. Each condition can link its sliders so that moving
// one scales the others by the same ratio: none, per effect, or all six.

enum class Link { Unlinked, ByEffect, AllLinked };

struct Condition {
	const char* name;
	const char* pbr;		// the sections the effects read
	const char* terrain;	// nullptr: interiors (no terrain)
	Link link;
};
Condition gConditions[] = {
	{ "Day", "Shaders.PBR.Main", "Shaders.Terrain.Main", Link::Unlinked },
	{ "Night", "Shaders.PBR.Night", "Shaders.Terrain.Night", Link::Unlinked },
	{ "Rain", "Shaders.PBR.Rain", "Shaders.Terrain.Rain", Link::Unlinked },
	{ "Night + Rain", "Shaders.PBR.NightRain", "Shaders.Terrain.NightRain", Link::Unlinked },
	{ "Interiors", "Shaders.PBR.Interiors", nullptr, Link::Unlinked },
};
// Collapsible groups of the window: Rain holds both rain conditions.
struct ConditionGroup { const char* title; int first, count; };
const ConditionGroup kConditionGroups[] = { { "Day", 0, 1 }, { "Night", 1, 1 }, { "Rain", 2, 2 }, { "Interiors", 4, 1 } };

const char* const kScaleKeys[3] = { "LightingScale", "AmbientScale", "SkylightingScale" };

// Draws the sliders (3 PBR, then 3 Terrain when there are 6). Returns true when one moved.
bool ScaleSliders(Link link, float* values, int count) {
	static const char* const kNames[6] = { "PBR Light", "PBR Ambient", "PBR Sky", "Terrain Light", "Terrain Ambient", "Terrain Sky" };
	bool moved = false;
	for (int i = 0; i < count; i++) {
		const float was = values[i];
		ImGui::PushID(i);
		const bool edited = ImGui::DragFloat(kNames[i], &values[i], 0.005f, 0.0f, 0.0f, "%.3f");
		ImGui::PopID();
		if (!edited) continue;
		moved = true;
		if (was == 0.0f || link == Link::Unlinked) continue;	// from zero there is no ratio to follow

		const float ratio = values[i] / was;
		int lo = 0, hi = count;
		if (link == Link::ByEffect) {
			const bool pbrSide = i < 3;
			lo = pbrSide ? 0 : 3;
			hi = pbrSide ? 3 : count;
		}
		for (int j = lo; j < hi; j++)
			if (j != i) values[j] *= ratio;
	}
	return moved;
}

void LinkChoice(Link& link, bool hasTerrain) {
	int pick = (int)link;
	ImGui::RadioButton("Unlinked", &pick, (int)Link::Unlinked);
	Beside();
	ImGui::RadioButton("By Effect", &pick, (int)Link::ByEffect);
	if (hasTerrain) {	// interiors have only PBR, so "all" would be the same as "by effect"
		Beside();
		ImGui::RadioButton("All Linked", &pick, (int)Link::AllLinked);
	}
	link = (Link)pick;
}

// One condition's block. Writes the settings when a slider moved (the caller reloads once for the whole window).
bool ConditionBlock(Condition& c) {
	ImGui::PushID(c.name);
	ImGui::TextUnformatted(c.name);
	ImGui::Separator();
	LinkChoice(c.link, c.terrain != nullptr);

	float values[6] = {};
	for (int k = 0; k < 3; k++) values[k] = TheSettingManager->GetSettingF(c.pbr, kScaleKeys[k]);
	if (c.terrain)
		for (int k = 0; k < 3; k++) values[3 + k] = TheSettingManager->GetSettingF(c.terrain, kScaleKeys[k]);

	const bool moved = ScaleSliders(c.link, values, c.terrain ? 6 : 3);
	if (moved) {
		for (int k = 0; k < 3; k++) TheSettingManager->SetSettingF(c.pbr, kScaleKeys[k], values[k]);
		if (c.terrain)
			for (int k = 0; k < 3; k++) TheSettingManager->SetSettingF(c.terrain, kScaleKeys[k], values[3 + k]);
	}
	ImGui::PopID();
	return moved;
}

void LightingWindow() {
	ToolWindow(Branding::LightingTitle, MenuTools::LightingOpen, ImVec2(440.0f, 560.0f), ImVec2(620.0f, 200.0f), 0, false, [] {
		ImGui::TextWrapped("Intensity-only view over the same PBR/Terrain settings the main menu already edits, grouped by condition, with an optional proportional link per group.");
		bool written = false;
		for (const ConditionGroup& g : kConditionGroups) {
			if (!ImGui::CollapsingHeader(g.title, ImGuiTreeNodeFlags_DefaultOpen)) continue;
			for (int n = 0; n < g.count; n++) written |= ConditionBlock(gConditions[g.first + n]);
		}
		if (written) TheSettingManager->LoadSettings();	// once, for every block that wrote
	});
}

// ---- Dev Tools window ------------------------------------------------------------------------------------------------------

struct DevState {
	float sessionTimeScale = -1.0f;	// TimeScale when Dev Tools first opened this session (-1: not yet)
	bool freecam = false;			// tfc is on
	bool hudHidden = false;			// tm hid the HUD
};
DevState gDev;

// Interior cells for the jump list, most recent first. The game has no list of every interior (its cell array only holds cells
// loaded so far), so this is the keyword-file cells plus every interior visited this session, and it grows as you play.
std::vector<std::string> gVisited;
std::string gLastVisited;

void RememberInterior() {
	TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
	if (!cell || !cell->IsInterior()) return;
	const char* id = cell->GetEditorName();
	if (!id || !id[0] || gLastVisited == id) return;
	gLastVisited = id;
	gVisited.erase(std::remove(gVisited.begin(), gVisited.end(), gLastVisited), gVisited.end());
	gVisited.insert(gVisited.begin(), gLastVisited);
	if (gVisited.size() > 50) gVisited.resize(50);
}

// Turns back off what Dev Tools switched on: the hidden HUD, then the free camera.
void DevUndo() {
	if (gDev.hudHidden) {
		Console("tm");
		gDev.hudHidden = false;
	}
	if (gDev.freecam) {
		Console("tfc");
		gDev.freecam = false;
	}
}

// Time of day, TimeScale (with back-to-session and back-to-vanilla), game speed and the free camera.
void DevTime(TimeGlobals* time) {
	if (!time || !time->GameHour || !time->TimeScale) {
		ImGui::TextDisabled("Time globals unavailable.");
		return;
	}
	float hour = fmodf(time->GameHour->data, 24.0f);
	if (ImGui::SliderFloat("Time of Day", &hour, 0.0f, 23.99f, "%.2f h")) time->GameHour->data = hour;
	float scale = time->TimeScale->data;
	if (ImGui::SliderFloat("TimeScale", &scale, 1.0f, 200.0f, "%.1f")) time->TimeScale->data = scale;
	ImGui::Spacing();

	const bool known = gDev.sessionTimeScale >= 0.0f;
	{
		GreyedOut same(known && fabsf(time->TimeScale->data - gDev.sessionTimeScale) < 0.05f);
		char label[64];
		snprintf(label, sizeof(label), "Restore: %.1f###RestoreSession", known ? gDev.sessionTimeScale : 30.0f);
		if (ImGui::Button(label) && known) time->TimeScale->data = gDev.sessionTimeScale;
	}
	Beside();
	{
		GreyedOut same(fabsf(time->TimeScale->data - 30.0f) < 0.05f);
		if (ImGui::Button("Vanilla (30)")) time->TimeScale->data = 30.0f;
	}
	RuledGap();

	// Game speed: read straight from the game, set with sgtm
	const float speed = *(float*)0x11AC3A0;
	float wanted = speed;
	ImGui::SetNextItemWidth(200.0f);
	if (ImGui::SliderFloat("Game Speed", &wanted, 0.05f, 4.0f, "%.3fx")) {
		char line[64];
		snprintf(line, sizeof(line), "sgtm %.4f", wanted);
		Console(line);
	}
	Beside();
	{
		GreyedOut same(fabsf(speed - 1.0f) < 0.005f);
		if (ImGui::SmallButton("Reset (1x)")) Console("sgtm 1.0");
	}
	ImGui::Spacing();

	if (ImGui::Checkbox("FreeCam + Timestop  (tfc 1)", &gDev.freecam)) Console(gDev.freecam ? "tfc 1" : "tfc");
}

void DevDisplay() {
	if (!ImGui::CollapsingHeader("Display")) return;
	if (ImGui::Checkbox("Hide Game HUD  (tm)", &gDev.hudHidden)) Console("tm");
	ImGui::Spacing();
	if (ImGui::Button("Hide UI for Screenshot")) MenuTools::ScreenshotMode = true;
	Beside();
	ImGui::TextDisabled("(Press the menu key to restore)");
	// (UNOFFICIAL 2026-10-25: "Developer options" is only on the menu's top row now; it was here too, and as a setting under
	// Main > Menu > Developer: three places for one switch, user.)
}

// Type an interior EditorID, or pick one, and jump there with coc.
void DevJump() {
	if (!ImGui::CollapsingHeader("Location (coc)")) return;
	static char destination[64] = "";
	static ImGuiTextFilter narrow;

	ImGui::SetNextItemWidth(220.0f);
	ImGui::InputText("Interior cell EditorID", destination, sizeof(destination));
	ImGui::TextDisabled("(type freely, or pick from a list below)");
	ImGui::Spacing();

	ImGui::TextUnformatted("Interior cells (keyword-tagged + visited this session)");
	narrow.Draw("##cellfilter", 200.0f);
	auto listCells = [&](const std::vector<std::string>& cells, const char* tag) {
		for (const auto& cell : cells) {
			if (!narrow.PassFilter(cell.c_str())) continue;
			char row[96];
			snprintf(row, sizeof(row), "%s %s", tag, cell.c_str());
			if (ImGui::Selectable(row)) strncpy_s(destination, cell.c_str(), _TRUNCATE);
		}
	};
	Boxed("##celllist", 90.0f, 0, [&] {
		listCells(gVisited, "[Visited]");
		listCells(PresetManager::GetKnownKeywordCells(), "[Keyword]");
	});
	ImGui::Spacing();

	GreyedOut empty(destination[0] == '\0');
	if (ImGui::Button("COC")) {
		char line[96];
		snprintf(line, sizeof(line), "coc %s", destination);
		ConsoleLater(line);	// coc loads over several frames: it cannot run inside this frame
	}
}

// 2026-11-05 (user: "weather manipulation in the dev menu too with common weathers as an option"): force a weather with fw,
// back to the game's own with rw. The quick buttons take the first loaded weather whose editor name holds one of their words (any
// weather mod's names count too); greyed when the load order has none. The list below holds every weather loaded.
struct QuickWeather { const char* Label; const char* Words; };	// Words: comma separated, matched case-insensitively
const QuickWeather kQuickWeathers[] = {
	{ "Clear", "clear,pleasant,sunny" }, { "Cloudy", "cloud,overcast" }, { "Rain", "rain" }, { "Storm", "storm,thunder" },
	{ "Sandstorm", "sand,dust" }, { "Fog", "fog,mist" }, { "Snow", "snow" }, { "Radstorm", "rad" },
};

std::string Lower(const char* text) {
	std::string s(text ? text : "");
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

TESWeather* FindQuickWeather(const QuickWeather& q) {
	if (!DataHandler) return nullptr;
	const std::string words = q.Words;
	for (auto* entry = &DataHandler->weathers.First; entry && entry->item; entry = entry->next) {
		const std::string name = Lower(entry->item->GetEditorName());
		size_t start = 0;
		while (start < words.size()) {
			const size_t end = words.find(',', start);
			const std::string word = words.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!word.empty() && name.find(word) != std::string::npos) return entry->item;
			if (end == std::string::npos) break;
			start = end + 1;
		}
	}
	return nullptr;
}

void ForceWeatherNow(TESWeather* weather) {
	char line[32];
	snprintf(line, sizeof(line), "fw %08X", weather->refID);
	Console(line);
}

// 2026-11-05 (user: "also allow for you to turn on and off weathers so it saves to your hdrweatherandcolor.ini"): DN Weathers' own
// list of which of its weathers may come up ([Weathers] in Data\config\HDRWeatherAndColor\HDRWeatherAndColor.ini: AADN<name> = 1 / 0,
// under "; ------- WEATHER TOGGLES <region> -------" and "; --- <kind> ---" comment lines). A switch rewrites only its own line (a line
// the file has commented out, "; AADN... = 1", counts as off and is written back uncommented); every other line, comment and the line
// ends stay as they were. The first change keeps a copy of the file as .backup-vegasshaders. DN Weathers reads the file itself.
const char* const kDnwPath = "Data\\config\\HDRWeatherAndColor\\HDRWeatherAndColor.ini";

struct DnwToggle { size_t Line; std::string Key, Group; bool On; };
struct DnwFile {
	bool Loaded = false, Missing = false;
	std::string Eol = "\r\n", Error;
	bool FinalEol = true;
	std::vector<std::string> Lines;
	std::vector<DnwToggle> Toggles;
};
DnwFile gDnw;

std::string Trim(const std::string& s) {
	const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "AADNName = 1" (or with a leading ';'): the key and its value; false for any other line
bool DnwParseToggle(std::string text, std::string& key, bool& on) {
	text = Trim(text);
	if (!text.empty() && text[0] == ';') text = Trim(text.substr(1));
	if (text.compare(0, 4, "AADN") != 0) return false;
	const size_t eq = text.find('=');
	if (eq == std::string::npos) return false;
	key = Trim(text.substr(0, eq));
	const std::string value = Trim(text.substr(eq + 1));
	if (key.find_first_of(" \t;") != std::string::npos || (value != "0" && value != "1")) return false;
	on = value == "1";
	return true;
}

void DnwLoad() {
	gDnw = DnwFile();
	gDnw.Loaded = true;
	std::ifstream in(kDnwPath, std::ios::binary);
	if (!in) {
		gDnw.Missing = true;
		return;
	}
	const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	gDnw.Eol = all.find("\r\n") != std::string::npos ? "\r\n" : "\n";
	gDnw.FinalEol = !all.empty() && all.back() == '\n';
	size_t start = 0;
	while (start < all.size()) {
		size_t end = all.find('\n', start);
		if (end == std::string::npos) end = all.size();
		std::string line = all.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		gDnw.Lines.push_back(line);
		start = end + 1;
	}
	bool weathers = false;
	std::string region, kind;
	for (size_t i = 0; i < gDnw.Lines.size(); i++) {
		const std::string text = Trim(gDnw.Lines[i]);
		if (!text.empty() && text[0] == '[') {
			weathers = text == "[Weathers]";
			continue;
		}
		if (!weathers) continue;
		std::string key;
		bool on = false;
		const bool commented = !text.empty() && text[0] == ';';
		if (DnwParseToggle(text, key, on)) {
			gDnw.Toggles.push_back({ i, key, region + (kind.empty() ? "" : ": " + kind), on && !commented });
			continue;
		}
		if (!commented) continue;
		std::string body = Trim(text.substr(1));
		const size_t toggles = body.find("WEATHER TOGGLES");
		if (toggles != std::string::npos) {
			region = Trim(body.substr(toggles + 15));
			while (!region.empty() && region.back() == '-') region.pop_back();
			region = Trim(region);
			kind.clear();
		}
		else if (body.compare(0, 3, "---") == 0) {	// "; --- HEAVY OVERCAST ---"
			while (!body.empty() && body.front() == '-') body.erase(body.begin());
			while (!body.empty() && body.back() == '-') body.pop_back();
			kind = Trim(body);
		}
	}
}

bool DnwSave() {
	std::ifstream exists(std::string(kDnwPath) + ".backup-vegasshaders");
	if (!exists) CopyFileA(kDnwPath, (std::string(kDnwPath) + ".backup-vegasshaders").c_str(), TRUE);
	std::ofstream out(kDnwPath, std::ios::binary | std::ios::trunc);
	if (!out) return false;
	for (size_t i = 0; i < gDnw.Lines.size(); i++) {
		out << gDnw.Lines[i];
		if (i + 1 < gDnw.Lines.size() || gDnw.FinalEol) out << gDnw.Eol;
	}
	return (bool)out;
}

void DnwSet(DnwToggle& t, bool on) {
	std::string& line = gDnw.Lines[t.Line];
	const size_t first = line.find_first_not_of(" \t;");
	std::string kept;	// the line's indent, without a comment mark
	for (size_t i = 0; i < (first == std::string::npos ? 0 : first); i++) if (line[i] != ';') kept += line[i];
	line = kept + t.Key + " = " + (on ? "1" : "0");
	t.On = on;
	gDnw.Error = DnwSave() ? std::string() : std::string("Could not write ") + kDnwPath + " (read-only, or open in another program?)";
	Logger::Log("UNOFFICIAL Dev Tools: DN Weathers %s set to %d in HDRWeatherAndColor.ini%s", t.Key.c_str(), on ? 1 : 0,
		gDnw.Error.empty() ? "" : " (FAILED to write)");
}

// The loaded weather a DN Weathers key names: its editor name is the key, or the key without its "AADN" prefix.
TESWeather* DnwWeather(const std::string& key) {
	if (!DataHandler) return nullptr;
	const std::string a = Lower(key.c_str()), b = Lower(key.substr(4).c_str());
	for (auto* entry = &DataHandler->weathers.First; entry && entry->item; entry = entry->next) {
		const std::string name = Lower(entry->item->GetEditorName());
		if (name == a || name == b) return entry->item;
	}
	return nullptr;
}

void DevDnwToggles() {
	if (!ImGui::TreeNode("Which weathers can come up (DN Weathers)")) return;
	if (!gDnw.Loaded) DnwLoad();
	if (gDnw.Missing) {
		ImGui::TextDisabled("DN Weathers' HDRWeatherAndColor.ini was not found (Data\\config\\HDRWeatherAndColor).");
		if (ImGui::SmallButton("Look again")) DnwLoad();
		ImGui::TreePop();
		return;
	}
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
	ImGui::TextDisabled("Saved to HDRWeatherAndColor.ini at once (a copy of the original is kept as .backup-vegasshaders). DN Weathers "
		"reads its file itself: if a change does not show, load a save. DLC weathers cannot be switched off.");
	ImGui::PopTextWrapPos();
	if (!gDnw.Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", gDnw.Error.c_str());
	if (ImGui::SmallButton("Read the file again")) DnwLoad();
	std::string group;
	for (DnwToggle& t : gDnw.Toggles) {
		if (t.Group != group) {
			group = t.Group;
			ImGui::Spacing();
			ImGui::SeparatorText(group.empty() ? "Weathers" : group.c_str());
		}
		ImGui::PushID((int)t.Line);
		bool on = t.On;
		if (ImGui::Checkbox(t.Key.c_str() + 4, &on)) DnwSet(t, on);	// (shown without the "AADN" prefix)
		if (TESWeather* weather = DnwWeather(t.Key)) {
			Beside();
			if (ImGui::SmallButton("Show")) ForceWeatherNow(weather);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Force this weather now (fw), to see it");
		}
		ImGui::PopID();
	}
	ImGui::TreePop();
}

void DevWeather() {
	if (!ImGui::CollapsingHeader("Weather")) return;
	if (!DataHandler) {
		ImGui::TextDisabled("Weathers not loaded yet.");
		return;
	}
	int inRow = 0;
	for (const QuickWeather& q : kQuickWeathers) {
		TESWeather* weather = FindQuickWeather(q);
		if (inRow++ % 4) Beside();
		GreyedOut none(!weather);
		if (ImGui::Button(q.Label, ImVec2(90.0f, 0.0f)) && weather) ForceWeatherNow(weather);
		if (weather && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", weather->GetEditorName());
	}
	ImGui::Spacing();
	if (ImGui::Button("Back to the game's weather  (rw)")) Console("rw");
	ImGui::Spacing();

	static ImGuiTextFilter narrow;
	narrow.Draw("##weatherfilter", 200.0f);
	Beside();
	ImGui::TextDisabled("every weather loaded");
	Boxed("##weatherlist", 120.0f, 0, [&] {
		for (auto* entry = &DataHandler->weathers.First; entry && entry->item; entry = entry->next) {
			const char* name = entry->item->GetEditorName();
			if (!name || !name[0] || !narrow.PassFilter(name)) continue;
			ImGui::PushID(entry->item);
			if (ImGui::Selectable(name)) ForceWeatherNow(entry->item);
			ImGui::PopID();
		}
	});
	ImGui::Spacing();
	DevDnwToggles();
}

void DevPresets() {
	if (!ImGui::CollapsingHeader("Presets")) return;
	if (ImGui::Button("Refresh All Presets")) {
		char text[192];
		snprintf(text, sizeof(text), "This will check %u preset file(s) and backfill any settings missing from current defaults. Continue?",
			(UInt32)PresetManager::ListAllPresets().size());
		AskFirst(PresetAsk::RefreshAll, "", text);
	}
	ImGui::TextDisabled("Backfills missing settings only -- never overwrites an existing value, never prunes.");
}

// Recent log lines, filterable, following the end while the view is scrolled to the bottom.
void DevLog() {
	if (!ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) return;
	static ImGuiTextFilter narrow;
	static bool presetsOnly = false;
	narrow.Draw("Filter", 180.0f);
	Beside();
	ImGui::Checkbox("Preset only", &presetsOnly);

	std::deque<std::string> recent;
	Logger::GetRecentLines(recent);
	Boxed("##logscroll", 180.0f, ImGuiWindowFlags_HorizontalScrollbar, [&] {
		for (const auto& line : recent) {
			const bool hidden = (presetsOnly && line.find("[Preset]") == std::string::npos) || !narrow.PassFilter(line.c_str());
			if (!hidden) ImGui::TextUnformatted(line.c_str());
		}
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);	// only when already at the end
	});
}

void DevBody(TimeGlobals* time) {
	const ImVec4 caution(1.0f, 0.55f, 0.1f, 1.0f);
	for (const char* warning : { "WARNING: For testing saves only.", "TimeScale and freecam changes can break active scripts and quests." })
		ImGui::TextColored(caution, "%s", warning);
	ImGui::Separator();
	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Time & World", ImGuiTreeNodeFlags_DefaultOpen)) DevTime(time);
	void (*const groups[])() = { DevWeather, DevDisplay, DevJump, DevPresets, DevLog };
	for (auto group : groups) {
		ImGui::Spacing();
		group();
	}
}

void DevWindow() {
	if (!MenuTools::DevToolsOpen) return;
	TimeGlobals* time = TimeGlobals::Get();
	if (gDev.sessionTimeScale < 0.0f && time && time->TimeScale) gDev.sessionTimeScale = time->TimeScale->data;	// once per session
	if (ToolWindow(Branding::DevToolsTitle, MenuTools::DevToolsOpen, ImVec2(420.0f, 460.0f), ImVec2(100.0f, 200.0f), 0, true, [time] { DevBody(time); }))
		DevUndo();
}

}	// namespace

// ---- MenuTools ------------------------------------------------------------------------------------------------------------

bool MenuTools::EncabActive() { return gEncab.on; }
bool MenuTools::FreecamOn() { return gDev.freecam; }
void MenuTools::FrameUpdate() { EncabTick(); }
void MenuTools::TrackCell() { RememberInterior(); }

// ---- About ---------------------------------------------------------------------------------------------------------------
// Help > About: what this is, its version, and who it builds on. The same credits as the README.
bool MenuTools::AboutOpen = false;

static void AboutCredit(const char* who, const char* what) {
	ImGui::TextUnformatted(who);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", what);
}

static void AboutWindow() {
	ToolWindow(Branding::AboutTitle, MenuTools::AboutOpen, ImVec2(560.0f, 420.0f), ImVec2(400.0f, 160.0f), 0, false, [] {
		ImGui::SetWindowFontScale(1.3f);
		ImGui::TextColored(MenuSkin::Heading(0.25f), "%s", Branding::ProductName);
		ImGui::SetWindowFontScale(1.0f);
		ImGui::TextDisabled("Version %s", Branding::Version);
		ImGui::Spacing();
		ImGui::TextWrapped("A graphics overhaul for Fallout: New Vegas: lighting, shadows, materials, sky, weather and post-processing. "
			"Free software under the GPL-3.0 (see License.md); it may not be sold.");
		ImGui::Spacing();
		ImGui::SeparatorText("Based on");
		ImGui::TextWrapped("New Vegas Reloaded / TESReloaded by Alessio Tamburini (Alenet), maintained by Llde, with the New Vegas "
			"Reloaded team and contributors (Timeslip, scanti, ShadeMe, Ethatron, GBR, mcfurston, noonemusteverknow, Mathieu U and others). "
			"Vegas Shaders is an independent project: please ask for help with it in its own community, not in the official channels.");
		ImGui::Spacing();
		ImGui::SeparatorText("With work from");
		AboutCredit("macrimmon12-tech", "the fork this grew from: volumetric fog v2, skin and lighting fixes, the first ImGui menu");
		AboutCredit("Conor (mrfury97)", "terrain shadow sweep, terrain variation, many lamps, skin work, Pip-Boy fit, the game MSAA block");
		AboutCredit("PixelsSquared", "the Perceptual tone mapper, eye adaptation, cloud moonlight and sky light");
		AboutCredit("macintroll", "snow accumulation, the snow look, captured normals, lens and underwater shaders");
		AboutCredit("Balebandro", "Physically-Based-Reloaded: PBR weather scales, grass size fade");
		AboutCredit("VHeyes (mirjomi)", "fixes: SMAA edges and tuning, the material pass depth, and more");
		AboutCredit("Community Shaders team", "terrain/cloud shadows, grass collision, skylighting, LOD blending, True PBR layout (GPL-3.0)");
		AboutCredit("Polyphony Digital", "the Perceptual tone mapper (their MIT sample code) and the eye adaptation design, from their SIGGRAPH 2025 paper for Gran Turismo 7");
		AboutCredit("WallSoGB", "the Fallout: New Vegas engine database behind the engine fixes");
		AboutCredit("NVSE / xNVSE", "the plugin interface and the community's reverse engineering of the game");
		ImGui::Spacing();
		ImGui::TextDisabled("And everyone who tested builds and sent reports.");
	});
}

void MenuTools::DrawWindows(float stepSize) {
	EncabWindow(stepSize);
	DevWindow();
	PresetsWindow();
	LightingWindow();
	AboutWindow();
	PresetConfirmPopup();	// always: a pending confirm stays answerable with the Presets window closed
}

void MenuTools::OnMenuClosed() {
	EncabSwitchOff();
	DevUndo();
}

void MenuTools::OnDevToolsClosed() { DevUndo(); }

bool MenuTools::HandleWindowMessage(UINT msg, LPARAM lParam) {
	if (msg != DeferredConsoleMessage()) return false;
	if (char* line = (char*)lParam) {
		Console(line);
		free(line);
	}
	return true;
}
