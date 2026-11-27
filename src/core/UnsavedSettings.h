#pragma once
// 2026-11-25 (unsaved marks): which settings were changed since the last Save or Revert, so the menu can draw the sidebar rows
// and the page rows that hold them in bold with a " *" (MenuWidgets' marked flags and MarkLastItem, ImGuiManager's "Unsaved marks").
//   SettingManager::SetSetting (every change goes through it) reports each change here with the value before and after it;
//   SaveSettings and RevertSettings clear the list. The value kept is the one the setting had before its first change, so a
//   setting changed back to it (by hand, a reset, a preset) leaves the list again. SetSetting sets hasUnsavedChanges (the menu
//   bar's "Unsaved", the amber Save, the status bar's "Unsaved changes") to Any() after each change, so they go off when every
//   change is undone. A key this build does not know (no saved value to come back to) goes on with kNoValue and stays until Save.
// 2026-11-26: a menu control that writes more settings than the one it shows (a mode picker) names itself in Writer() while it
//   writes; Owners() then says which unsaved settings it changed, so the menu can put them back when the control is back where it
//   was saved (ImGuiManager's "controls that write more than they show"). A change made with no writer named drops the owner.
//   Pure (no game or ImGui types), so tests/unsaved_settings.cpp can check it without the game.
#include <map>
#include <string>
#include <utility>

namespace UnsavedSettings {
	// section -> key -> the setting's value when it was last saved (the text SettingManager compares)
	typedef std::map<std::string, std::map<std::string, std::string>> Table;
	// (section, key) -> the control that changed it last (only for unsaved settings)
	typedef std::map<std::pair<std::string, std::string>, std::string> OwnerTable;

	inline Table& All() { static Table table; return table; }
	inline OwnerTable& Owners() { static OwnerTable owners; return owners; }

	// The control writing now ("" when none: a change by hand).
	inline std::string& Writer() { static std::string writer; return writer; }

	// Goes up whenever the list changes (the menu keeps what it worked out from the list until then).
	inline unsigned& Revision() { static unsigned revision = 0; return revision; }

	// The "value before" of a key that had none (not a setting of this build): no value text equals it.
	const char* const kNoValue = "\x01(no value)";

	// A setting went from before to after (their whole values as text).
	inline void Changed(const char* section, const char* key, const std::string& before, const std::string& after) {
		Table& all = All();
		const std::pair<std::string, std::string> id(section, key);
		const auto own = [&]() {	// the setting is (still) unsaved: who changed it last
			if (Writer().empty()) Owners().erase(id);
			else Owners()[id] = Writer();
		};
		const auto sec = all.find(section);
		if (sec == all.end()) {
			if (before == after) return;
			all[section][key] = before;
			own();
			Revision()++;
			return;
		}
		const auto it = sec->second.find(key);
		if (it == sec->second.end()) {
			if (before == after) return;
			sec->second[key] = before;
			own();
			Revision()++;
			return;
		}
		if (before != after) Revision()++;
		if (it->second != after) {	// still not the saved value
			if (before != after) own();
			return;
		}
		sec->second.erase(it);
		if (sec->second.empty()) all.erase(sec);
		Owners().erase(id);
	}

	// Everything is what is on disk again (Save wrote it, Revert read it back).
	inline void Clear() {
		All().clear();
		Owners().clear();
		Revision()++;
	}

	inline bool Any() { return !All().empty(); }

	// This one setting is unsaved.
	inline bool Key(const char* section, const char* key) {
		const Table& all = All();
		const auto sec = all.find(section);
		return sec != all.end() && sec->second.count(key) != 0;
	}

	// Something in this very section is unsaved (not counting the sections under it).
	inline bool Section(const char* section) { return All().count(section) != 0; }

	// The control that changed this unsaved setting last ("" when it was changed by hand, or is saved).
	inline std::string Owner(const char* section, const char* key) {
		const auto it = Owners().find(std::make_pair(std::string(section), std::string(key)));
		return it == Owners().end() ? std::string() : it->second;
	}
}
