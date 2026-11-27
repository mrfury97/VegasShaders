// Checks the menu's list of unsaved settings (src/core/UnsavedSettings.h, 2026-11-25 unsaved marks): a change puts the setting on
// the list with the value it had before, a later change keeps that first value, changing it back to that value takes it off (by hand,
// a reset or a preset: anything that goes through SetSetting), a change to the same value adds nothing, and Save / Revert clear it all.
#include <cstdio>
#include "../src/core/UnsavedSettings.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace UnsavedSettings;

int main() {
	// 1. Nothing changed yet.
	CHECK(!Any() && !Key("Shaders.Bloom.Main", "Strength") && !Section("Shaders.Bloom.Main"), "a fresh list is empty");

	// 2. A slider moved: on the list, its section too; the other keys and sections are not.
	Changed("Shaders.Bloom.Main", "Strength", "1", "1.5");
	CHECK(Any() && Key("Shaders.Bloom.Main", "Strength"), "a changed setting is unsaved");
	CHECK(Section("Shaders.Bloom.Main") && !Section("Shaders.Bloom") && !Section("Shaders.Bloom.Night"), "only its own section is marked (not the parent, not a sibling)");
	CHECK(!Key("Shaders.Bloom.Main", "Threshold") && !Key("Shaders.Bloom.Night", "Strength"), "other keys stay saved");

	// 3. Moved again, then back to where it was saved: off the list, and the section with it.
	Changed("Shaders.Bloom.Main", "Strength", "1.5", "2");
	CHECK(Key("Shaders.Bloom.Main", "Strength") && All()["Shaders.Bloom.Main"]["Strength"] == "1", "a second change keeps the saved value (1)");
	Changed("Shaders.Bloom.Main", "Strength", "2", "1");
	CHECK(!Key("Shaders.Bloom.Main", "Strength") && !Section("Shaders.Bloom.Main") && !Any(), "back to the saved value: nothing unsaved");

	// 4. A write of the value it already has (a preset that does not change it) adds nothing.
	Changed("Shaders.Bloom.Main", "Strength", "1", "1");
	CHECK(!Any(), "a write of the same value adds nothing");

	// 5. Two keys in one section: the section stays marked until both are back.
	Changed("Shaders.TAA.Main", "Sharpness", "0.5", "0.8");
	Changed("Shaders.TAA.Main", "Enabled", "false", "true");
	Changed("Shaders.TAA.Main", "Sharpness", "0.8", "0.5");
	CHECK(Section("Shaders.TAA.Main") && Key("Shaders.TAA.Main", "Enabled") && !Key("Shaders.TAA.Main", "Sharpness"), "one key back: the other keeps its section marked");

	// 6. Save (or Revert): everything is what is on disk again.
	Changed("Main.Main.Misc", "RenderEffects", "true", "false");
	Clear();
	CHECK(!Any() && !Section("Shaders.TAA.Main") && !Key("Main.Main.Misc", "RenderEffects"), "Save / Revert clear the list");

	// 7. After a save the value just saved is the one to come back to.
	Changed("Main.Main.Misc", "RenderEffects", "false", "true");
	Changed("Main.Main.Misc", "RenderEffects", "true", "false");
	CHECK(!Any(), "changed after a save and back: nothing unsaved");

	// 8. A key this build does not know (SetSetting passes kNoValue): it stays unsaved whatever it is set to, until Save.
	Changed("Main.Hand.Made", "Key", kNoValue, "1");
	Changed("Main.Hand.Made", "Key", "1", "");
	CHECK(Any() && Key("Main.Hand.Made", "Key"), "an unknown key stays unsaved (even set to an empty text)");
	Clear();
	CHECK(!Any(), "Save clears it");

	// 9. 2026-11-26: a control that writes more than it shows owns what it changed; a change by hand takes the setting back.
	Writer() = "il";
	Changed("Shaders.AmbientLight.Main", "ComputeGI", "true", "false");
	Changed("Shaders.AmbientLight.Main", "GridBounce", "0.7", "0");
	Writer().clear();
	CHECK(Owner("Shaders.AmbientLight.Main", "ComputeGI") == "il" && Owner("Shaders.AmbientLight.Main", "GridBounce") == "il", "the picker owns what it changed");
	Changed("Shaders.AmbientLight.Main", "GridBounce", "0", "0.3");
	CHECK(Owner("Shaders.AmbientLight.Main", "GridBounce").empty() && Key("Shaders.AmbientLight.Main", "GridBounce"), "changed by hand afterwards: no longer the picker's, still unsaved");
	const unsigned revision = Revision();
	Changed("Shaders.AmbientLight.Main", "ComputeGI", "false", "true");
	CHECK(Owner("Shaders.AmbientLight.Main", "ComputeGI").empty() && !Key("Shaders.AmbientLight.Main", "ComputeGI") && Revision() != revision, "back to saved: no owner, the list changed");
	const unsigned same = Revision();
	Changed("Shaders.AmbientLight.Main", "GridBounce", "0.3", "0.3");
	CHECK(Revision() == same, "a write of the same value does not change the list");
	Clear();
	CHECK(Owners().empty(), "Save clears the owners too");

	std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
