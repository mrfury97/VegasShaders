// UNOFFICIAL 2026-10-27: the game's own MSAA and NVR. With iMultiSample on, the world is drawn into multisampled targets and every
// depth-based NVR effect reads one depth per pixel: RESZ (AMD, and DXVK on any GPU, which resolves it with VK_RESOLVE_MODE_SAMPLE_ZERO)
// copies the FIRST sample only, while the colour on screen is the average of all of them. Wherever a pixel holds two surfaces -- every
// edge, and all over grass, whose cut-outs become coverage masks with MSAA (transparency multisampling) -- colour and depth disagree:
// user and a tester, MSAA 4 on DXVK: crusty speckle in the cloud shadows on grass, a broken shadow ring round the player; gone with MSAA
// off. D3D9 shaders cannot read the samples, so NVR cannot fix that per effect. It says so once in the log and once on screen
// (NvrMsaaWarningTick). Reading iMultiSample as 0 here alone (tried the same day) left the game half multisampled: the water
// reflection has its own iWaterMultiSamples, and FalloutCustom.ini reaches the renderer's own setting past this hook. (The grass trouble
// that day was b30GrassVS, below.) 2026-11-04: [Main.Main.Misc] BlockGameMSAA (default true) switches it off for real: both settings
// read as 0 here, the renderer's copies set to 0 before it starts (BlockGameMSAABeforeRenderer), and neither written back to the INIs.
static int GameMSAAWarned = 0;	// the iMultiSample the INI asks for, when it is on (0: off)
D3DXVECTOR4 NvrGameMultisample(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-11-05 (Balebandro) TESR_GameMultisample (ShaderManager): x = iMultiSample as the game reads it
void NvrMsaaWarningTick() {
	static bool shown = false;
	if (!GameMSAAWarned || shown || !Player || !Player->parentCell || !InterfaceManager) return;
	shown = true;
	InterfaceManager->ShowMessage("Vegas Shaders: turn the game's anti-aliasing off (launcher options). With it on, Vegas Shaders' shadows and effects get speckled.");
}

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
		else if (nvr.BlockGameMSAA) {
			// UNOFFICIAL 2026-11-04 BlockGameMSAA: read as 0 (TESRenderer::Init then leaves eMode 0: no multisampled frame buffer, and
			// bTransparencyMultisampling needs iMultiSample > 1, so the grass takes GRASS23x00n, which NVR also replaces). The earlier
			// try (header) left the water reflection multisampled: that one is iWaterMultiSamples (below). Its grass problem was b30GrassVS.
			if (Setting->iValue > 0)
				Logger::Log("UNOFFICIAL BlockGameMSAA: the INI asks for the game's anti-aliasing (iMultiSample %d): read as 0 (MSAA off; the INI file is not changed)", Setting->iValue);
			Setting->iValue = 0;
		}
		else if (Setting->iValue > 0) {
			if (!GameMSAAWarned)
				Logger::Log("WARNING: the game's own anti-aliasing is on (iMultiSample %d). Vegas Shaders' effects read one depth sample per pixel, which with "
					"MSAA disagrees with the colour: speckled cloud shadows on grass, broken shadows round the player, patches on characters. Turn "
					"Antialiasing off in the game's launcher (Options); Vegas Shaders' TAA / FSR / DLSS smooth the edges instead.", Setting->iValue);
			GameMSAAWarned = Setting->iValue;
		}
		NvrGameMultisample.x = (float)(Setting->iValue > 0 ? Setting->iValue : 0);	// UNOFFICIAL 2026-11-05 (after BlockGameMSAA: 0 while it blocks)
	}
	// UNOFFICIAL 2026-11-04 BlockGameMSAA: the water reflection's own MSAA (BSShaderManager::iWaterReflectionMultisample, read once at start)
	else if (SameKey(key, "iWaterMultiSamples:Display") && nvr.BlockGameMSAA && !nvr.ForceMSAA) {
		if (Setting->iValue > 0)
			Logger::Log("UNOFFICIAL BlockGameMSAA: iWaterMultiSamples %d read as 0", Setting->iValue);
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
	// UNOFFICIAL 2026-11-04 BlockGameMSAA: the 0 read in above stays in memory; the INI keeps the player's value
	SettingsMainStruct::MainStruct& nvr = TheSettingManager->SettingsMain.Main;
	if (nvr.BlockGameMSAA && !nvr.ForceMSAA && (SameKey(Setting->Name, "iMultiSample:Display") || SameKey(Setting->Name, "iWaterMultiSamples:Display")))
		return true;
	return WriteSetting(This, Setting);

}

// UNOFFICIAL 2026-11-04 BlockGameMSAA, part 2: the renderer's own settings (RendererSettingCollection), set to 0 just before
// TESRenderer::Init reads them (called from InitializeRendererHook = TESMain::InitRenderer, after every INI file is read).
// The read hook above did not see every route: FalloutCustom.ini's iMultiSample 4 still reached the renderer (user test 16:09, the
// frame 4x). TESRenderer::Init: eMode (the frame buffer's MSAA) from gs_iMultiSample, BSShaderManager::iWaterReflectionMultisample
// from gs_iWaterMultiSamples, bTransparencyMultisampling needs gs_iMultiSample > 1.
void BlockGameMSAABeforeRenderer() {
	const SettingsMainStruct::MainStruct& nvr = TheSettingManager->SettingsMain.Main;
	if (!nvr.BlockGameMSAA || nvr.ForceMSAA) return;
	struct Entry { UInt32 address; const char* name; };
	const Entry entries[] = { { 0x011C73F4, "iMultiSample:Display" }, { 0x011C7284, "iWaterMultiSamples:Display" } };
	for (const Entry& e : entries) {
		GameSetting* s = (GameSetting*)e.address;
		if (!s->Name || strcmp(s->Name, e.name) != 0) {	// not this game build's layout: leave it
			Logger::Log("UNOFFICIAL BlockGameMSAA: the renderer's %s is not at %08X (found '%s'): not changed", e.name, e.address, s->Name ? s->Name : "(none)");
			continue;
		}
		if (s->iValue != 0)
			Logger::Log("UNOFFICIAL BlockGameMSAA: the renderer's %s was %d (the INIs, FalloutCustom.ini included): set to 0 before the renderer starts", e.name, s->iValue);
		s->iValue = 0;
	}
}

// Replaces the game's main-menu music path with NVR's track (called from two places, see Hooks.cpp).
void MainMenuMusicFix(char* DestinationBuffer, size_t Size, char* SourceBuffer) {

	char path[64] = {};
	sprintf(path, "Data\\Music\\Special\\%s.mp3", MainMenuMusic);
	strcpy(DestinationBuffer, path);

}
