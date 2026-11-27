#include "MenuWidgets.h"
#include "imgui_internal.h"	// ImLerp, RenderArrow, RenderTextEllipsis, FindRenderedTextEnd

namespace MenuWidgets {
namespace {
	// What the last RowBegin found out, for the calls that finish the row
	struct RowState {
		float endX = 0.0f;		// the row's right edge, in window coordinates
		bool nameHovered = false;
		bool nameClipped = false;
	};
	RowState Row;

	float TrackHeight() { return ImGui::GetFrameHeight() * 0.72f; }

	// ---- Unsaved marks (2026-11-25) ----
	const char* const kMark = " *";

	// How far the bold pass is drawn to the right: a pixel at the font's own size (13 px), more as the menu's text is scaled up
	float BoldShift() { return ImMax(1.0f, IM_ROUND(ImGui::GetFontSize() / 13.0f)); }

	// " *" in bold, its top left at "at", in the current font
	void DrawStar(ImDrawList* draw, const ImVec2& at, ImU32 col) {
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, col, kMark);
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + BoldShift(), at.y), col, kMark);
	}

	// The bold pass over a text a widget has drawn with its top left at "at", then the " *" right after it
	void DrawMark(ImDrawList* draw, const ImVec2& at, const char* text, const char* textEnd, ImU32 col) {
		draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + BoldShift(), at.y), col, text, textEnd);
		DrawStar(draw, ImVec2(at.x + ImGui::CalcTextSize(text, textEnd).x, at.y), col);
	}
}

float MarkWidth() { return ImGui::CalcTextSize(kMark).x + BoldShift(); }

void MarkLastItem(const char* label, bool marked) {
	if (!marked) return;
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems) return;
	// Selectable, TextUnformatted and an unframed TreeNodeEx all end their layout (ItemSize) where their text ends, and draw
	// it on the line's text baseline, so the text's place is known from the layout alone
	const char* shownEnd = ImGui::FindRenderedTextEnd(label);
	const float textW = ImGui::CalcTextSize(label, shownEnd).x;
	const ImVec2 at(window->DC.CursorPosPrevLine.x - textW, window->DC.CursorPosPrevLine.y + window->DC.PrevLineTextBaseOffset);
	if (ImGui::IsItemVisible()) {
		// kept inside the item when it is wider than its text (a Selectable or a tree node spans the sidebar: a long name does not
		// spill past it); a plain text item is exactly its text, and the mark goes after it
		const ImRect item(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const bool clip = item.Max.x > at.x + textW + 1.0f;
		if (clip) ImGui::PushClipRect(item.Min, item.Max, true);
		DrawMark(window->DrawList, at, label, shownEnd, ImGui::GetColorU32(ImGuiCol_Text));
		if (clip) ImGui::PopClipRect();
	}
	// what comes after the item on the same line (a count, a badge) starts after the mark
	window->DC.CursorPosPrevLine.x += MarkWidth();
	window->DC.CursorMaxPos.x = ImMax(window->DC.CursorMaxPos.x, window->DC.CursorPosPrevLine.x);
}

float ToggleWidth() { return TrackHeight() * 1.85f; }

bool Toggle(const char* id, bool* on) {
	const float trackH = TrackHeight();
	const float trackW = ToggleWidth();
	const float lineH = ImGui::GetFrameHeight();
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	const bool clicked = ImGui::InvisibleButton(id, ImVec2(trackW, lineH));
	if (clicked) *on = !*on;
	const bool hot = ImGui::IsItemHovered();

	// The knob slides over about a tenth of a second (the position is kept per switch in the window's state storage)
	float* slide = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *on ? 1.0f : 0.0f);
	const float goal = *on ? 1.0f : 0.0f;
	const float step = ImGui::GetIO().DeltaTime * 10.0f;
	*slide = goal > *slide ? ImMin(goal, *slide + step) : ImMax(goal, *slide - step);

	const ImGuiStyle& style = ImGui::GetStyle();
	const ImVec4 offTrack = style.Colors[hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg];
	const ImVec4 onTrack = style.Colors[ImGuiCol_CheckMark];
	const ImVec4 knob = ImLerp(style.Colors[ImGuiCol_TextDisabled], style.Colors[ImGuiCol_Text], *slide);

	const ImVec2 a(origin.x, origin.y + (lineH - trackH) * 0.5f);
	const ImVec2 b(a.x + trackW, a.y + trackH);
	const float radius = trackH * 0.5f;
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(a, b, ImGui::GetColorU32(ImLerp(offTrack, onTrack, *slide)), radius);
	if (style.FrameBorderSize > 0.0f || hot)
		draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), radius);
	const float knobX = a.x + radius + (trackW - trackH) * *slide;
	draw->AddCircleFilled(ImVec2(knobX, a.y + radius), radius - 2.5f, ImGui::GetColorU32(knob));
	return clicked;
}

bool GroupLabel(const char* text, bool forceOpen, bool marked) {
	ImGuiStorage* storage = ImGui::GetStateStorage();
	const ImGuiID key = ImGui::GetID(text);
	if (forceOpen) storage->SetInt(key, 1);
	bool open = storage->GetInt(key, 1) != 0;

	char upper[96];
	size_t n = 0;
	for (const char* c = text; *c && n + 1 < sizeof(upper); c++) upper[n++] = (char)toupper((unsigned char)*c);
	upper[n] = '\0';

	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	ImGui::SetWindowFontScale(0.85f);
	const ImVec2 textSize = ImGui::CalcTextSize(upper);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float width = ImMax(ImGui::GetContentRegionAvail().x, textSize.x);
	ImGui::PushID(key);
	if (ImGui::InvisibleButton("##fold", ImVec2(width, textSize.y + 2.0f))) {
		open = !open;
		storage->SetInt(key, open ? 1 : 0);
	}
	ImGui::PopID();
	const bool hot = ImGui::IsItemHovered();
	const ImU32 col = ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 2.0f, at.y + 1.0f), col, upper);
	if (marked) DrawMark(draw, ImVec2(at.x + 2.0f, at.y + 1.0f), upper, upper + n, col);	// 2026-11-25 (unsaved marks)
	if (!open || hot)	// a hint only where it helps: folded, or under the mouse
		ImGui::RenderArrow(draw, ImVec2(at.x + width - textSize.y, at.y + 1.0f), col, open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
	ImGui::SetWindowFontScale(1.0f);
	return open;
}

bool SubHeader(const char* label, bool defaultOpen, bool marked) {
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
	const bool open = ImGui::TreeNodeEx(label, flags);
	MarkLastItem(label, marked);	// 2026-11-25 (unsaved marks)
	ImGui::PopStyleColor();

	// The rule starts after the arrow and the text (the same offsets ImGui's tree node uses)
	const ImGuiStyle& style = ImGui::GetStyle();
	const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
	const float textW = ImGui::CalcTextSize(label, nullptr, true).x + (marked ? MarkWidth() : 0.0f);
	const float ruleX = lo.x + ImGui::GetFontSize() + style.FramePadding.x * 2.0f + textW + style.ItemInnerSpacing.x * 2.0f;
	if (ruleX < hi.x) {
		const float y = (float)(int)((lo.y + hi.y) * 0.5f) + 0.5f;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(ruleX, y), ImVec2(hi.x, y), ImGui::GetColorU32(ImGuiCol_Separator));
	}
	return open;
}

void Title(const char* text, float scale, bool marked) {
	ImGui::SetWindowFontScale(scale);
	ImGui::TextUnformatted(text);
	MarkLastItem(text, marked);	// 2026-11-25 (unsaved marks): at the title's size
	ImGui::SetWindowFontScale(1.0f);
}

float RowNameWidth() {
	return ImMin(ImGui::GetFontSize() * 14.0f, ImGui::GetContentRegionAvail().x * 0.45f);
}

float SmallButtonWidth(const char* text) {
	return ImGui::CalcTextSize(text, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float ResetSlotWidth() { return SmallButtonWidth("~") + ImGui::GetStyle().ItemSpacing.x; }

float RowBegin(const char* name, float trailing, bool marked) {
	const float startX = ImGui::GetCursorPosX();
	const float avail = ImGui::GetContentRegionAvail().x;
	const float nameW = RowNameWidth();
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	const float lineH = ImGui::GetFrameHeight();
	Row.endX = startX + avail;

	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(ImMax(nameW - gap, 1.0f), lineH));
	Row.nameHovered = ImGui::IsItemHovered();

	const char* shownEnd = ImGui::FindRenderedTextEnd(name);
	const ImVec2 textSize = ImGui::CalcTextSize(name, shownEnd);
	const float mark = marked ? MarkWidth() : 0.0f;	// 2026-11-25 (unsaved marks): the " *" keeps its room when the name is cut
	const float right = at.x + nameW - gap - mark;
	Row.nameClipped = textSize.x > nameW - gap - mark;
	const ImVec2 textAt(at.x, at.y + ImGui::GetStyle().FramePadding.y);
	ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), textAt, ImVec2(right, at.y + lineH), right, name, shownEnd, &textSize);
	if (marked) {
		// the bold pass is the same cut, a pixel over, so both passes end their "..." at the same letter
		const float bold = BoldShift();
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(textAt.x + bold, textAt.y), ImVec2(right + bold, at.y + lineH),
			right + bold, name, shownEnd, &textSize);
		DrawStar(ImGui::GetWindowDrawList(), ImVec2(Row.nameClipped ? right : textAt.x + textSize.x, textAt.y), ImGui::GetColorU32(ImGuiCol_Text));
	}

	ImGui::SameLine(0.0f, 0.0f);
	ImGui::SetCursorPosX(startX + nameW);
	return ImMax(ImGui::GetFontSize() * 4.0f, avail - nameW - trailing);
}

bool RowLabelHovered() { return Row.nameHovered; }
bool RowLabelClipped() { return Row.nameClipped; }

bool ResetButton(const char* id, bool show, const char* tooltip) {
	if (!show) return false;
	const float w = SmallButtonWidth(id);
	ImGui::SameLine();
	if (ImGui::GetCursorPosX() < Row.endX - w) ImGui::SetCursorPosX(Row.endX - w);
	const bool clicked = ImGui::SmallButton(id);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
	return clicked;
}

ImVec4 MutedText(float t) {
	const ImGuiStyle& style = ImGui::GetStyle();
	return ImLerp(style.Colors[ImGuiCol_Text], style.Colors[ImGuiCol_TextDisabled], t);
}

}
