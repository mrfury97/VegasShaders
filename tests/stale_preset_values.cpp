// UNOFFICIAL 2026-11-18: offline test of src/core/StalePresetValues.h Decide (what a preset's or script's number does). No game.
// Build (x86 Developer prompt):  cl /nologo /EHsc /std:c++17 tests\stale_preset_values.cpp /Fe:<out>\stale_preset_values.exe
// Run:  <out>\stale_preset_values.exe      exit 0 = every case as expected
// tests/stale_preset_values_check.py checks the table against SettingManager's UpdateOldDefaults.
#define STALE_PRESET_VALUES_TABLE_ONLY
#include "../src/core/StalePresetValues.h"
#include <cstdio>

using namespace StalePresetValues;

static int failures = 0;
static void Expect(const char* section, const char* key, float value, Verdict want, const char* wantSection = nullptr, const char* wantKey = nullptr) {
	const Result r = Decide(section, key, value);
	bool ok = r.verdict == want;
	if (ok && wantSection) ok = !_stricmp(r.section, wantSection) && !_stricmp(r.key, wantKey);
	printf("%s  [%s] %s = %g\n", ok ? "ok  " : "FAIL", section, key, value);
	if (!ok) ++failures;
}

int main() {
	// an old default of a fix: passed over
	Expect("Shaders.PBR.Linear", "LinearScene", 0.0f, Verdict::PassOver);
	Expect("Shaders.AmbientLight.Main", "AlbedoBuffer", 0.0f, Verdict::PassOver);
	Expect("Shaders.Exposure.Main", "LightAdaptSpeed", 50.0f, Verdict::PassOver);
	Expect("Shaders.AmbientOcclusion.RT", "GIOffscreen", 0.1f, Verdict::PassOver);
	Expect("Shaders.AmbientOcclusion.RT", "GIOffscreen", (float)0.1, Verdict::PassOver);	// a double from a script, narrowed
	Expect("shaders.smaa.status", "enabled", 1.0f, Verdict::PassOver);	// case does not matter (as the settings lookup)
	Expect("Main.Main.Misc", "FlashDetect", 1.0f, Verdict::PassOver);
	Expect("Shaders.VolumetricLight.Main", "NightStrength", 0.45f, Verdict::PassOver);	// no single new key
	// the same keys with any other value: the preset's own choice
	Expect("Shaders.PBR.Linear", "LinearScene", 1.0f, Verdict::Set);
	Expect("Shaders.AmbientLight.Main", "AlbedoBuffer", 1.0f, Verdict::Set);
	Expect("Shaders.Exposure.Main", "LightAdaptSpeed", 0.5f, Verdict::Set);
	Expect("Shaders.SMAA.Status", "Enabled", 0.0f, Verdict::Set);
	Expect("Shaders.AmbientOcclusion.RT", "GIOffscreen", 0.12f, Verdict::Set);
	// the look and the effect switches: always the preset's
	Expect("Shaders.Tonemapping.Main", "Exposure", 1.5f, Verdict::Set);
	Expect("Shaders.PBR.Main", "LightingScale", 0.725f, Verdict::Set);
	Expect("Shaders.Sky.Main", "TintDayR", 1.0f, Verdict::Set);
	Expect("Shaders.AmbientLight.Status", "Enabled", 0.0f, Verdict::Set);
	Expect("Shaders.Bloom.Main", "Strength", 2.0f, Verdict::Set);
	// renamed keys
	Expect("Main.Main.ReducedQuality", "TerrainParallaxLite", 1.0f, Verdict::Rename, "Main.Main.ReducedQuality", "ParallaxLite");
	Expect("Shaders.Cloth.Main", "Interiors", 1.0f, Verdict::Rename, "Shaders.Cloth.Interiors", "Enabled");
	// the four old sun-shadow object lists (2026-11-18): the near one carries over key by key, the others are passed over
	Expect("Shaders.ShadowsExteriors.FormsNear", "Actors", 1.0f, Verdict::Rename, "Shaders.ShadowsExteriors.Forms", "Actors");
	Expect("Shaders.ShadowsExteriors.FormsLod", "Books", 1.0f, Verdict::PassOver);
	Expect("Shaders.ShadowsExteriors.Forms", "Actors", 0.0f, Verdict::Set);
	// no section / key
	Expect(nullptr, "Mode", 2.0f, Verdict::Set);
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
