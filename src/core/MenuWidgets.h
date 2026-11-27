#pragma once
#include "imgui.h"

// 2026-11-02: the settings menu's own small widgets (drawn with ImGui's draw list, coloured from the current style, so every
// MenuSkin applies to them). ImGuiManager lays the menu out with these; nothing here reads or writes a setting.
namespace MenuWidgets {
	// An on/off switch: a rounded track with a sliding knob. Returns true on the frame it is clicked (*on is flipped then),
	// like ImGui::Checkbox. The id is the ImGui label ("##..." to show nothing). Height follows the text size.
	bool Toggle(const char* id, bool* on);
	float ToggleWidth();

	// A small, dim, upper-case heading for a group of sidebar rows. Clicking it folds the group; returns whether it is open
	// (open the first time; forceOpen opens it, as SetNextItemOpen would). marked: see "Unsaved marks" below.
	bool GroupLabel(const char* text, bool forceOpen = false, bool marked = false);

	// A quiet collapsible heading inside a page: a small arrow, the text, and a thin rule to the right edge. label may carry
	// a "###id" part, as with ImGui::CollapsingHeader; defaultOpen applies the first time only. Returns whether it is open.
	bool SubHeader(const char* label, bool defaultOpen, bool marked = false);

	// A page title: the text at a larger size.
	void Title(const char* text, float scale = 1.3f, bool marked = false);

	// Settings rows: the name in a fixed-width column on the left, the control filling the rest of the row on the right, and
	// a slot at the far end for the reset button. RowBegin draws the name (cut with "..." if it does not fit) and leaves the
	// cursor at the control column; trailing is the width kept free after the control (reset slot included). The control's
	// width is returned. RowLabelHovered says whether the mouse was over the name. A marked name keeps its " *" in view
	// when it is cut.
	float RowBegin(const char* name, float trailing, bool marked = false);
	bool  RowLabelHovered();
	bool  RowLabelClipped();
	float RowNameWidth();
	float ResetSlotWidth();

	// The reset button at the row's end: drawn only when show is true (the slot stays empty otherwise, so rows line up).
	// Returns true when clicked. The tooltip is shown on hover.
	bool ResetButton(const char* id, bool show, const char* tooltip);

	// Width of a small button with this text (ImGui::SmallButton's own size).
	float SmallButtonWidth(const char* text);

	// A colour between the style's text and disabled-text colours (0 = text, 1 = disabled): rows that are not selected.
	ImVec4 MutedText(float t);

	// ---- Unsaved marks (2026-11-25) ----
	// A row or heading that holds a setting not saved yet is drawn in bold with " *" after its text (UnsavedSettings.h says
	// what counts). The menu font has no bold face: bold is the text drawn a second time, a pixel to the right. The widgets
	// above take a marked flag. For a plain ImGui item that shows its label as one line of text (Selectable, TreeNodeEx
	// without a frame, TextUnformatted), call MarkLastItem right after it, with the same label, while the colour the item's
	// text was drawn in is still pushed. The item keeps its ID, size and clicks; an item placed after it with SameLine moves
	// over to make room for the " *". Nothing is drawn or moved when marked is false.
	void  MarkLastItem(const char* label, bool marked);
	float MarkWidth();	// the room the mark takes after a text (" *" and the bold pixel)
}
