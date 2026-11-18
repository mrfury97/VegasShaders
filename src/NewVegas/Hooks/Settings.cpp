// UNOFFICIAL 2026-10-27: the game's own MSAA and NVR. With iMultiSample on, the world is drawn into multisampled targets and every
// depth-based NVR effect reads one depth per pixel: RESZ (AMD, and DXVK on any GPU, which resolves it with VK_RESOLVE_MODE_SAMPLE_ZERO)
// copies the FIRST sample only, while the colour on screen is the average of all of them. Wherever a pixel holds two surfaces -- every
// edge, and all over grass, whose cut-outs become coverage masks with MSAA (transparency multisampling) -- colour and depth disagree:
// user and a tester, MSAA 4 on DXVK: crusty speckle in the cloud shadows on grass, a broken shadow ring round the player; gone with MSAA
// off. D3D9 shaders cannot read the samples, so NVR cannot fix that per effect. It says so once in the log and once on screen
// (NvrMsaaWarningTick). Reading iMultiSample as 0 here alone (tried the same day) left the game half multisampled: the water
// reflection has its own iWaterMultiSamples, and FalloutCustom.ini reaches the renderer's own setting past this hook. (The grass trouble
// that day was b30GrassVS, below.) 2026-11-04: [Main.Main.Misc] BlockGameMSAA switched it off for real (default on). 2026-11-05 (user,
// after merging Balebandro's PR #5: SkyCoverage, ForwardCloudShadows, deferred passes and skin under MSAA): BlockGameMSAA is removed, the
// INIs' MSAA is used again; the log still says once that it is on, and the on-screen warning is gone.
// 2026-11-15 (owner decision, docs/next/FULL-DEFERRED-RENDERER-PLAN.md section 7; docs/next/FD-RELEASE-PREP.md): the deferred renderer
// ships on by default and cannot work with the game's MSAA (no G-buffer on native D3D9; LinearScene, and with it DeferredSun, off under
// MSAA on DXVK), and FSR / DLSS upscaling skips every multisampled frame ("a frame stays at native resolution: the game's MSAA is on").
// The game's launcher rewrites FalloutPrefs.ini with iMultiSample 4 after its "graphics card changed" check (the owner, 11-14 and
// 11-15), which silently turned both off. So, unless [Main.Main.Misc] ForceMSAA is on, the game's MSAA is turned off while either is
// on, by BlockGameMSAA's tested route (commit 9e45720aa, 2026-11-04, removed 5f4eb941e): iMultiSample and the water reflection's own
// iWaterMultiSamples read as 0 here, the renderer's own copies set to 0 before the renderer starts (GameMsaaBeforeRenderer:
// FalloutCustom.ini reaches them past this hook), and neither written back, so the INIs keep the player's values. (The water one as
// well: the state 9e45720aa tested had both at 0, and a multisampled screen-sized reflection target could trip CapturedNormals'
// session-long MSAA refusal on native D3D9.)
#include "../../core/SettingForce.h"
static int GameMSAAWarned = 0;	// the iMultiSample the INI asks for, when it is on (0: off)
static int GameMSAAOff = 0;		// UNOFFICIAL 2026-11-15: the iMultiSample the INIs asked for and that was turned off (0: none)
D3DXVECTOR4 NvrGameMultisample(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-11-04 TESR_GameMultisample (ShaderManager): x = iMultiSample as the game reads it
void NvrMsaaWarningTick() {}	// (2026-11-05: the on-screen warning is gone with BlockGameMSAA; ShaderManager still calls it)

// The game's INI read/write calls, hooked (Hooks.cpp attaches them). A read lets the game fill the value first and then adjusts the
// few entries NVR depends on; a write is skipped for the entries NVR overrides at run time, so they never land in the user's INI.
typedef bool (__thiscall* IniSettingCall)(INISettingCollection*, GameSetting*);
IniSettingCall ReadSetting = reinterpret_cast<IniSettingCall>(Hooks::ReadSetting);
IniSettingCall WriteSetting = reinterpret_cast<IniSettingCall>(Hooks::WriteSetting);

namespace {
	struct ForcedIni { const char* key; int value; };

	// INI entries that always read as a fixed value, whatever the file says.
	const ForcedIni kForcedOnRead[] = {
		{ "iLocation X:Display", 0 }, { "iLocation Y:Display", 0 },					// window placed at the corner
		{ "bDoCanopyShadowPass:Display", 0 }, { "bDoActorShadows:Display", 0 },		// the game's own shadows: NVR draws them
		{ "iActorShadowCountExt:Display", 0 }, { "iActorShadowCountInt:Display", 0 },
		{ "bDoHighDynamicRange:BlurShaderHDR", 1 }, { "bUseBlurShader:BlurShader", 0 },
		{ "bUseWaterHiRes:Water", 1 }, { "bForceHighDetailReflections:Water", 0 },
	};

	// INI entries the game must not write back.
	const char* const kKeptFromWrite[] = {
		"fNearDistance:Display", "fNear1stPersonDistance:Display",
		"fDefaultFOV:Display", "fDefaultWorldFOV:Display", "fDefault1stPersonFOV:Display", "fPipboy1stPersonFOV:Display",
		"fLODFadeOutMultActors:LOD", "fLODFadeOutMultItems:LOD", "fLODFadeOutMultObjects:LOD",
		"SIntroSequence:General", "SMainMenuMovie:General", "SMainMenuMusic:General", "STitleMusic:Loading",
	};

	inline bool SameKey(const char* a, const char* b) { return strcmp(a, b) == 0; }

	// UNOFFICIAL 2026-11-15 (header): why the game's MSAA is turned off this session, or null when it is not. Decided at the first
	// call and kept (the renderer reads its MSAA once, at start): the settings are loaded before the hooks are attached (Main.cpp
	// NVSEPlugin_Load). The upscaler's condition is the one TAAEffect::UpdateConstants and TAAEffect::UpscaleBegin apply (TAA on;
	// DLSS wins over FSR; each its own RenderScale, a missing 0 = 1; upscaled below 0.999). Whether the DXVK fork is there (FSR and
	// DLSS do nothing without it) is not known this early: the settings decide.
	const char* GameMsaaOffReason() {
		static int decided = -1;
		static const char* reason = nullptr;
		if (decided >= 0) return reason;
		decided = 1;
		SettingManager* sm = TheSettingManager;
		if (!sm || sm->SettingsMain.Main.ForceMSAA) return reason = nullptr;
		if (SettingForce::Mode() > 0) return reason = "the deferred renderer is on ([Main.Main.Quality] DeferredRenderer)";
		if (sm->GetSettingI("Shaders.TAA.Status", "Enabled") != 0) {
			const bool dlss = sm->GetSettingI("Shaders.TAA.DLSS", "Enabled") != 0;
			if (dlss || sm->GetSettingI("Shaders.TAA.FSR", "Enabled") != 0) {
				const float scale = sm->GetSettingF(dlss ? "Shaders.TAA.DLSS" : "Shaders.TAA.FSR", "RenderScale");
				if (scale > 0.0f && scale < 0.999f)
					reason = dlss ? "DLSS upscaling is on ([Shaders.TAA.DLSS] RenderScale below 1)" : "FSR upscaling is on ([Shaders.TAA.FSR] RenderScale below 1)";
			}
		}
		return reason;
	}
}

bool __fastcall ReadSettingHook(INISettingCollection* This, UInt32 edx, GameSetting* Setting) {

	const bool found = ReadSetting(This, Setting);
	const char* key = Setting->Name;
	SettingsMainStruct::MainStruct& nvr = TheSettingManager->SettingsMain.Main;

	bool handled = false;
	for (const ForcedIni& forced : kForcedOnRead) {
		if (SameKey(key, forced.key)) {
			Setting->iValue = forced.value;
			handled = true;
			break;
		}
	}

	if (handled) {}
	else if (SameKey(key, "bFull Screen:Display")) {
		TheSettingManager->SetWindowedMode(Setting->iValue);
	}
	else if (SameKey(key, "SIntroSequence:General")) {
		if (nvr.ReplaceIntro) Setting->pValue = (char*)IntroMovie;
	}
	else if (SameKey(key, "SMainMenuMovie:General")) {
		if (nvr.ReplaceIntro) Setting->pValue = (char*)MainMenuMovie;
	}
	else if (SameKey(key, "SMainMenuMusic:General") || SameKey(key, "STitleMusic:Loading")) {
		if (nvr.ReplaceIntro) Setting->pValue = (char*)MainMenuMusic;
	}
	else if (SameKey(key, "iMultiSample:Display")) {
		if (nvr.ForceMSAA) {
			if (Setting->iValue < 2) Setting->iValue = 2;
		}
		else if (const char* why = GameMsaaOffReason()) {	// UNOFFICIAL 2026-11-15 (header)
			if (Setting->iValue > 0 && !GameMSAAOff) {
				GameMSAAOff = Setting->iValue;
				Logger::Log("UNOFFICIAL the game's own anti-aliasing (iMultiSample %d in the game's INI) is turned OFF for this session, because %s: "
					"with the game's MSAA on, the deferred renderer draws the old way and FSR / DLSS do not upscale. TAA, FSR or DLSS smooth the edges "
					"instead. The INI file is not changed. To keep the game's MSAA (and lose those): [Main.Main.Misc] ForceMSAA = true, then restart.",
					Setting->iValue, why);
			}
			Setting->iValue = 0;
		}
		else if (Setting->iValue > 0) {
			if (!GameMSAAWarned)
				Logger::Log("UNOFFICIAL the game's own anti-aliasing is on (iMultiSample %d): the clouds, volumetric light and terrain blending take "
					"each edge pixel's sky share (SkyCoverage); for cloud shadows without speckled edges turn on [Shaders.CloudShadows.Main] "
					"ForwardShadows (restart). Effects that read one depth per pixel can still show thin edge seams.", Setting->iValue);
			GameMSAAWarned = Setting->iValue;
		}
		NvrGameMultisample.x = (float)(Setting->iValue > 0 ? Setting->iValue : 0);
	}
	// UNOFFICIAL 2026-11-15 (header): the water reflection's own MSAA (BSShaderManager::iWaterReflectionMultisample, read once at start)
	else if (SameKey(key, "iWaterMultiSamples:Display") && !nvr.ForceMSAA && GameMsaaOffReason()) {
		if (Setting->iValue > 0) Logger::Log("UNOFFICIAL the water reflection's own anti-aliasing (iWaterMultiSamples %d) is turned off too (the INI file is not changed)", Setting->iValue);
		Setting->iValue = 0;
	}
	// UNOFFICIAL 2026-10-27 (user: "still no grass shadows"): NVR's grass shaders replace the game's shader model 3 grass (GRASS23x000-007);
	// with b30GrassVS 0 the game takes its older GRASS2000-2023 set, which NVR leaves alone: no NVR lighting, shadow or wind on grass at all.
	// The launcher writes 0 whenever it sees DXVK's adapter name (it rewrote the user's FalloutPrefs.ini that way the same day), so it is
	// read as 1 here. (Read once, at start, for the grass shader set; unlike iMultiSample nothing else reads it.)
	else if (SameKey(key, "b30GrassVS:Grass") && !Setting->iValue) {
		Logger::Log("UNOFFICIAL b30GrassVS was 0 in the game's INI (the launcher writes 0 under DXVK): read as 1, so Vegas Shaders' grass shaders are used");
		Setting->iValue = 1;
	}
	SessionLog::NoteIni(Setting);	// UNOFFICIAL 2026-11-01: a few values for the session header (SessionLog.h)
	return found;

}

bool __fastcall WriteSettingHook(INISettingCollection* This, UInt32 edx, GameSetting* Setting) {

	for (const char* kept : kKeptFromWrite) {
		if (SameKey(Setting->Name, kept)) return true;	// reported as written, file untouched
	}
	// UNOFFICIAL 2026-11-15 (header): the 0 read in above stays in memory; the INI keeps the player's value
	if ((SameKey(Setting->Name, "iMultiSample:Display") || SameKey(Setting->Name, "iWaterMultiSamples:Display")) && GameMsaaOffReason()) return true;
	return WriteSetting(This, Setting);

}

// UNOFFICIAL 2026-11-15 (header), part 2, as BlockGameMSAA's (commit 9e45720aa, tested in game 2026-11-04: FalloutCustom.ini
// iMultiSample 4 gave a back buffer and scene targets at msaa 0): the renderer's own settings (RendererSettingCollection gs_iMultiSample
// 0x011C73F4, gs_iWaterMultiSamples 0x011C7284), set to 0 just before TESRenderer::Init reads them (InitializeRendererHook, after every
// INI file is read). FalloutCustom.ini reaches them past the read hook. Each one's name is checked before writing.
void GameMsaaBeforeRenderer() {
	const char* why = GameMsaaOffReason();
	if (!why) return;
	struct Entry { UInt32 address; const char* name; };
	const Entry entries[] = { { 0x011C73F4, "iMultiSample:Display" }, { 0x011C7284, "iWaterMultiSamples:Display" } };
	for (const Entry& e : entries) {
		GameSetting* s = (GameSetting*)e.address;
		if (!s->Name || strcmp(s->Name, e.name) != 0) {	// not this game build's layout: leave it
			Logger::Log("[WARNING] UNOFFICIAL the renderer's %s is not at %08X (found '%s'): not changed, the game's MSAA may stay on", e.name, e.address, s->Name ? s->Name : "(none)");
			continue;
		}
		if (s->iValue == 0) continue;
		const bool main = e.address == 0x011C73F4;
		if (main && !GameMSAAOff) {
			GameMSAAOff = s->iValue;
			Logger::Log("UNOFFICIAL the game's own anti-aliasing (iMultiSample %d, from the game's INIs, FalloutCustom.ini included) is turned OFF for this "
				"session, because %s. The INI files are not changed. To keep it: [Main.Main.Misc] ForceMSAA = true, then restart.", s->iValue, why);
		}
		else Logger::Log("UNOFFICIAL the renderer's %s was %d (FalloutCustom.ini?): set to 0 as well", e.name, s->iValue);
		s->iValue = 0;
	}
	NvrGameMultisample.x = 0.0f;
}

// Replaces the game's main-menu music path with NVR's track (called from two places, see Hooks.cpp).
void MainMenuMusicFix(char* DestinationBuffer, size_t Size, char* SourceBuffer) {

	char path[64] = {};
	sprintf(path, "Data\\Music\\Special\\%s.mp3", MainMenuMusic);
	strcpy(DestinationBuffer, path);

}
