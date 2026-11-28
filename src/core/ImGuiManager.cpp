#include "ImGuiManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "GpuProfiler.h"
#include "TestTools.h"
#include "PixelInspector.h"	// UNOFFICIAL 2026-10-26 (debugging tool)
#include "MenuSkin.h"
#include "MenuTools.h"
#include "MenuWidgets.h"
#include "MenuLayout.h"	// 2026-11-10: the effects list's folders, the player's own arrangement
#include "UnsavedSettings.h"	// 2026-11-25 (unsaved marks): the settings not saved yet, marked in bold with " *"
#include "Branding.h"
#include "FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects: the status bar lists the parts switched off
#include "EffectStatus.h"
#include "InstallCheck.h"	// 2026-11-03: the install self-check (main-menu notice, Help > Install check, Copy bug report)
#include "DeepProbe.h"		// UNOFFICIAL 2026-11-21: Help > Performance probe... (full DLL/script/draw census)
#include "RtCore.h"			// 2026-11-05: the Ray tracing page's status lines
#include "RtSunShadows.h"
#include "RtLampShadows.h"
#include "RtPathTrace.h"
#include "RtAmbient.h"
#include "RtFogVisibility.h"	// UNOFFICIAL 2026-11-13: the Ray tracing page's "Light shafts" status line
#include "PtApplies.h"	// UNOFFICIAL 2026-11-12: what each effect does while the path traced picture replaces the scene (the notes)
#include "../effects/Reflections/Reflections.h"
#include "../effects/AmbientOcclusion/AmbientOcclusion.h"
#include "../effects/AmbientLight/AmbientLight.h"
#include <sstream>
#include <functional>	// 2026-11-26 (unsaved marks): ModeOf
#include <array>
#include <iomanip>
#include <ctime>
#include <unordered_set>
#include <algorithm>
#include <share.h>	// _SH_DENYNO (Copy bug report reads the open log)
#include "ShaderFallback.h"	// UNOFFICIAL 2026-11-12 ShaderFallback: its notices, shown with the install check's
#include "SunFadeDeferred.h"	// UNOFFICIAL 2026-11-15 fd-smoothsun (the SunSmoothing notes)
#include "ShelvedEffects.h"	// UNOFFICIAL 2026-11-16: shelved effects have no page, row or search entry in a release build
#include "CompileProgress.h"	// UNOFFICIAL 2026-11-20: close startup shader-compile overlay once the first frame arrives
#include "DistantLandProto.h"	// UNOFFICIAL 2026-11-25 device reset release
#include "DistantLandTerrain.h"	// UNOFFICIAL 2026-11-25 device reset release
#include "MaterialSSSProto.h"	// UNOFFICIAL 2026-11-25 device reset release
#include "DxvkLoader.h"

// The settings overlay (ImGui on the game's D3D9 device). Layout, top to bottom: a menu bar (File / View / Tools and the
// global switches), the sidebar of sections next to the page of the selected section, and a one-line status bar. Input
// does not come from window messages (DXVK swallows them, see CLAUDE.md): the keyboard is polled with GetAsyncKeyState
// and the wheel is read out of the DirectInput mouse buffer before the game sees it.

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
	// The overlay itself
	struct Overlay {
		bool ready = false;				// ImGui and its backends are set up
		bool open = false;				// the menu is shown
		bool deviceLost = false;		// the device is lost: nothing is drawn until it is reset
		HWND window = nullptr;			// the game window
		WNDPROC gameWndProc = nullptr;	// the game's window procedure, called after ours
	};
	Overlay gOverlay;

	// Everything the menu remembers between frames
	struct MenuState {
		std::string page;				// the section shown on the right ("Shaders.Bloom.Main", the New-in-patch id, or empty)
		float step = 0.1f;				// what the +/- keys and the Developer -/+ buttons add
		int fxModifier = -1;			// the master switch's modifier (0 off, 1 Ctrl, 2 Alt, 3 Shift); -1 = read it from the settings
		bool plusKey = false;			// + or - went down this frame (steps the setting under the mouse)
		bool minusKey = false;
		float wheel = 0.0f;				// wheel notches taken from the DirectInput buffer, given to ImGui next frame
		BYTE keyWas[256] = {};			// last polled key states (0x80 = down), for press/release edges
		bool resetWheels = false;		// colour wheels re-read their three values (after a revert or a reload)
		float sidebarWidth = 0.0f;		// UNOFFICIAL 2026-10-25: the dragged sidebar width (0 = read [Main.Menu.Style] SidebarWidth)
		bool revealPage = false;		// UNOFFICIAL 2026-10-25, test shots: unfold the sidebar down to the page shown
		bool testSettings = false;		// UNOFFICIAL 2026-10-19: the auto-test's 'menu' shots show the test settings (TestShowMenu)
		bool devOptions = false;		// UNOFFICIAL 2026-10-25: [Main.Menu.Developer] DeveloperOptions, read every drawn frame
		bool advanced = false;			// 2026-11-05: [Main.Menu.Developer] AdvancedSettings, read every drawn frame
		bool showInternal = false;		// UNOFFICIAL 2026-11-12: [Main.Menu.Developer] ShowInternal (typed into the settings file), read every drawn frame
		bool ptNotes = true;			// UNOFFICIAL 2026-11-12: [Main.Menu.Developer] PathTracingNotes (View > Path tracing notes), read every drawn frame
		bool devWarnAsk = false;		// UNOFFICIAL 2026-11-12: View > Developer options was switched on: open its warning next to the main window
		bool devWarned = false;			// that warning was answered with "Show them" this session (it is shown once)
		bool testResetAsk = false;		// UNOFFICIAL 2026-11-12: View > Reset test and internal settings was picked: open its box
		bool diskConfirm = false;		// File > Reload from disk was picked: open its confirmation next to the main window
		bool installCheckOpen = false;	// 2026-11-03: Help > Install check
		double reportCopiedAt = -1.0;	// 2026-11-03: when Copy bug report was last pressed (ImGui time), for the "Copied" line
		bool deepProbeOpen = false;		// UNOFFICIAL 2026-11-21: Help > Performance probe...
		int deepProbeDuration = 10;
		bool deepProbeAutoClose = true;
		double probeCopiedAt = -1.0;
		// The value each setting had when it was first drawn since the menu opened: section -> key -> text (the ~ buttons)
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sessionValues;
	};
	MenuState gMenu;

	// A key polled every frame: Rose() is true on the frame it goes down
	struct KeyEdge {
		bool down = false;
		bool Rose(bool now) { const bool rose = now && !down; down = now; return rose; }
	};

	bool KeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
}

static void NewPatchRebuild();	// UNOFFICIAL 2026-10-18: the "New in this patch" list (defined with that page)
static void CaptureStartupValues();	// UNOFFICIAL 2026-10-19: what the restart-only settings were at start (menu honesty)
static void SetOverlayVisible(bool visible);
static char* SearchTestBuffer();	// UNOFFICIAL 2026-10-25: the menu search box (defined with the search)
static void CopyBugReport();	// 2026-11-03: Help > Copy bug report (defined with the install check)

static bool DevOptionsShown() { return gMenu.devOptions || gMenu.testSettings; }
// 2026-11-05: every setting under its own name, no simple views (Developer options alone does not: it adds the test settings there)
static bool AdvancedShown() { return gMenu.advanced || gMenu.testSettings; }

// ---- Game input while the menu is open -------------------------------------------------------------------------------
// Mouse movement: xNVSE cannot block it, so IDirectInputDevice8::GetDeviceState (vtable slot 9) of the mouse is wrapped and
// its DIMOUSESTATE2 is cleared while the menu is up. The wheel is read from that same buffer first: DXVK never delivers
// WM_MOUSEWHEEL. Keys and mouse buttons: xNVSE's DIHookControl.

typedef HRESULT(WINAPI* GetDeviceStateFn)(IDirectInputDevice8*, DWORD, LPVOID);
static GetDeviceStateFn RealGetDeviceState = nullptr;

static HRESULT WINAPI MouseStateHook(IDirectInputDevice8* device, DWORD size, LPVOID data) {
	const HRESULT result = RealGetDeviceState(device, size, data);
	const bool menuHasMouse = ImGuiManager::IsVisible() && !MenuTools::ScreenshotMode;
	if (SUCCEEDED(result) && size == sizeof(DIMOUSESTATE2) && menuHasMouse) {
		DIMOUSESTATE2* state = static_cast<DIMOUSESTATE2*>(data);
		gMenu.wheel += state->lZ / (float)WHEEL_DELTA;
		memset(data, 0, size);
	}
	return result;
}

// Wraps slot 9 once per session and never again. The slot belongs to the device class, so a reset or another open does not
// undo it; and another mod (CacheUI) may wrap the slot after us, chaining to our hook. Re-wrapping would then save that mod's
// hook as "the real one" and the two hooks would call each other forever (a stack overflow on opening the menu).
static void HookMouseOnce() {
	if (RealGetDeviceState) return;
	InputControl* input = Global->GetInputControl();
	if (!input || !input->mouseInterface) return;	// DirectInput not up yet: a later call tries again
	void** slots = *reinterpret_cast<void***>(input->mouseInterface);
	RealGetDeviceState = reinterpret_cast<GetDeviceStateFn>(slots[9]);
	DWORD protection;
	VirtualProtect(&slots[9], sizeof(void*), PAGE_READWRITE, &protection);
	slots[9] = reinterpret_cast<void*>(MouseStateHook);
	VirtualProtect(&slots[9], sizeof(void*), protection, &protection);
}

// Shift, Ctrl and Alt stay with the game: xNVSE tracks tap and hold on them, and disabling and re-enabling one breaks that for
// mods bound to it (sprint on Shift). Everything else, keyboard codes and the mouse codes above 255, is blocked.
static bool IsModifierCode(UInt32 code) {
	switch (code) {
	case 0x2A: case 0x36:	// DIK_LSHIFT, DIK_RSHIFT
	case 0x1D: case 0x9D:	// DIK_LCONTROL, DIK_RCONTROL
	case 0x38: case 0xB8:	// DIK_LMENU, DIK_RMENU
		return true;
	}
	return false;
}

static void SetGameInputBlocked(bool blocked) {
	if (!g_DIHookCtrl) return;
	for (UInt32 code = 0; code < kMaxMacros; code++)
		if (!IsModifierCode(code)) g_DIHookCtrl->SetKeyDisableState(code, blocked, DIHookControl::kDisable_User);
}

// ---- Opening and closing -------------------------------------------------------------------------------------------------

#if NVR_TEST_TOOLS
// UNOFFICIAL 2026-10-19: the auto-test's 'menu <section>' shots open the menu at that page (the picture is taken after it is
// drawn, SettingShots::CaptureMenu) and close it again (nullptr); ShowTests shows the test settings for that shot.
// 2026-10-25: 'menu search:<text>' fills the search box instead ('+' stands for a space, e.g. search:shadow+bias).
// 2026-11-03: 'menu @InstallCheck' opens Help > Install check and presses its Copy bug report (the report goes to the log too).
void ImGuiManager::TestShowMenu(const char* Section, bool ShowTests) {
	if (!gOverlay.ready) return;
	const bool opening = Section != nullptr;
	gMenu.testSettings = opening && ShowTests;
	gMenu.revealPage = opening;
	char* search = SearchTestBuffer();
	search[0] = '\0';
	if (opening && !strncmp(Section, "search:", 7)) {
		strncpy_s(search, 64, Section + 7, _TRUNCATE);
		std::replace(search, search + strlen(search), '+', ' ');
	}
	else if (opening && !strcmp(Section, "@InstallCheck")) {	// 2026-11-03: the Install check window, and Copy bug report pressed
		gMenu.installCheckOpen = true;
		CopyBugReport();
	}
	else if (opening) {
		gMenu.page = Section;
	}
	if (!opening) gMenu.installCheckOpen = false;
	SetOverlayVisible(opening);
}
#endif

// A cursor outside the window (left on another monitor before alt-tab) is moved to the window's centre, so the menu has a
// usable mouse position from its first frame.
static void BringCursorIntoWindow() {
	const HWND window = TheRenderManager->m_kWndFocus;
	RECT client;
	POINT screen;
	if (!::GetCursorPos(&screen) || !::GetClientRect(window, &client)) return;
	POINT local = screen;
	::ScreenToClient(window, &local);
	const bool inside = local.x >= client.left && local.x < client.right && local.y >= client.top && local.y < client.bottom;
	if (inside) return;
	POINT centre = { (client.left + client.right) / 2, (client.top + client.bottom) / 2 };
	::ClientToScreen(window, &centre);
	::SetCursorPos(centre.x, centre.y);
}

static void SetOverlayVisible(bool visible) {
	if (ImGuiManager::IsVisible() == visible) return;
	ImGuiManager::SetVisible(visible);
	ImGuiIO& io = ImGui::GetIO();
	if (!visible) {
		MenuTools::ScreenshotMode = false;
		MenuTools::OnMenuClosed();
		SetGameInputBlocked(false);
		io.MouseDrawCursor = false;
		return;
	}
	gMenu.sessionValues.clear();
	for (int vk = 0; vk < 256; vk++)	// keys already down when the menu opens are not presses
		gMenu.keyWas[vk] = KeyHeld(vk) ? 0x80 : 0;
	HookMouseOnce();
	ClipCursor(nullptr);
	SetGameInputBlocked(true);
	io.MouseDrawCursor = true;
	io.ClearInputKeys();
	BringCursorIntoWindow();
}

// Every list entry's on/off flag follows the settings again (as SettingManager::RevertSettings does).
static void SyncShaderSwitches() {
	StringList names;
	TheSettingManager->FillMenuSections(&names, "Shaders");
	for (const std::string& name : names) {
		const bool on = TheSettingManager->GetMenuShaderEnabled(name.c_str());
		if (EffectRecord* effect = TheShaderManager->GetEffectByName(name.c_str())) {
			effect->Enabled = on;
		}
		else if (ShaderCollection* shaders = TheShaderManager->GetShaderCollectionByName(name.c_str())) {
			shaders->Enabled = on;
		}
	}
}

// File > Revert: every unsaved change goes back to its saved value.
// 2026-11-26 (user: "the Revert button does nothing": AA SMAA -> FXAA, Revert, still FXAA): it went back only through the
// values the number rows had drawn since the menu opened (SessionValue), so a mode picker, a switch or a list never went back. Now it
// takes every setting on the unsaved list (UnsavedSettings: the value each had at the last Save) back to that value, so the bold
// " *" marks, the "Unsaved" and the amber Save go off with it. An effect's switch goes through the effect (it loads what it needs),
// a LUT name loads its file. A key this build does not know has no saved value and stays.
static void SetShaderEnabled(const char* shader, bool wantOn);	// with the category pages
static std::string EffectSwitchSection(const std::string& effect);
static void RevertSessionValues() {
	static const char* const kLutKeys[] = { "DayLUT", "NightLUT", "InteriorLUT" };	// LUT slots 0, 1, 2
	LUTEffect* lut = TheShaderManager->Effects.LUT;
	const UnsavedSettings::Table unsaved = UnsavedSettings::All();	// a copy: every write below takes its setting off the list
	for (const auto& [section, values] : unsaved) {
		for (const auto& [key, text] : values) {
			if (text == UnsavedSettings::kNoValue) continue;
			// A LUT name has to load its file: LUTEffect::UpdateSettings never re-reads the names, so writing it is not enough
			int slot = -1;
			if (lut && section == "Shaders.LUT.Main")
				for (int i = 0; i < 3; i++) if (key == kLutKeys[i]) slot = i;
			if (slot >= 0) {
				lut->LoadLUT(slot, text.c_str());
				continue;
			}
			const size_t dot = section.rfind('.');
			const std::string effect = section.compare(0, 8, "Shaders.") == 0 && dot != std::string::npos && dot > 8 ? section.substr(8, dot - 8) : std::string();
			if (key == "Enabled" && !effect.empty() && EffectSwitchSection(effect) == section) SetShaderEnabled(effect.c_str(), text == "true");
			else TheSettingManager->SetSettingS(section.c_str(), key.c_str(), text.c_str());
		}
	}
	TheSettingManager->LoadSettings();
	SyncShaderSwitches();
	gMenu.sessionValues.clear();	// the ~ buttons start again from these values
	gMenu.resetWheels = true;
	gMenu.page.clear();
}

// ---- Window procedure ------------------------------------------------------------------------------------------------------

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// A console command posted by Dev Tools runs here, outside any frame (see MenuTools.cpp)
	if (MenuTools::HandleWindowMessage(msg, lParam)) return 0;

	// UNOFFICIAL 2026-11-07 (rt-shutdown): the window closing (Alt+F4, the X, Windows logging off) may end the game without xNVSE's
	// ExitGame messages: the RT helper's shutdown first (RtCore::Shutdown; nothing when RT never ran, ~5 s at most). These arrive
	// through the game's message pump on its main thread, outside a frame. (A WM_CLOSE the game then ignored leaves RT off.)
	if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_ENDSESSION && wParam)) {
		RtCore::Shutdown(msg == WM_CLOSE ? "the game window is closing (WM_CLOSE)" : msg == WM_DESTROY ? "the game window is closing (WM_DESTROY)" : "Windows is ending the session");
		DeepProbe::OnGameExit();	// UNOFFICIAL 2026-11-28: live mode's final report (the writer gets at most 1 s)
	}

	if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) SetOverlayVisible(false);

	if (gOverlay.open) {
		if (msg == WM_SYSKEYDOWN && wParam == VK_MENU) return TRUE;	// Alt must not open the window's system menu
		// UNOFFICIAL 2026-11-04 (tester: "double inputs", every letter typed twice in the search box and setting fields): keys and
		// the characters they type reach ImGui only through FeedKeyboard (GetAsyncKeyState + ToUnicode, every frame), because under
		// DXVK these messages usually never arrive. Where they DO arrive (native D3D9, or another plugin translating the game's
		// key messages), the Win32 backend added each WM_CHAR a second time; it no longer sees keyboard messages at all.
		const bool keyboardMessage = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_UNICHAR;
		if (!keyboardMessage && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
		if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) return 0;	// the wheel scrolls the menu, not the game
	}
	return CallWindowProc(gOverlay.gameWndProc, hwnd, msg, wParam, lParam);
}

// ---- Start and shutdown --------------------------------------------------------------------------------------------------

// The window layout file: next to the DLL, VegasShaders.imgui.ini (always written under that name). Kept in a static string, as
// ImGui keeps the pointer.
static const char* LayoutFilePath() {
	static const std::string path = Paths::WritePath(Paths::ModuleDir(), Paths::File::ImGuiLayout);
	return path.c_str();
}

// [Main.Menu.Style] TextSize is a percentage. Values under 50 are the old pixel sizes (8-24): those mean 100 %.
static void ApplySavedTextSize(ImGuiIO& io) {
	if (!TheSettingManager) return;
	const int percent = (int)TheSettingManager->SettingsMain.Menu.TextSize;
	io.FontGlobalScale = percent < 50 ? 1.0f : percent / 100.0f;
}

void ImGuiManager::Initialize() {
	if (gOverlay.ready || !TheRenderManager) return;
	IDirect3DDevice9* d3d = TheRenderManager->device;
	const HWND window = TheRenderManager->m_kWndFocus;
	if (!d3d || !window) {
		Logger::Log("ImGuiManager: deferring init");
		return;
	}
	gOverlay.window = window;

	CaptureStartupValues();	// UNOFFICIAL 2026-10-19: the restart-only settings as the game started

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = LayoutFilePath();
	// 2026-11-02 Vegas Shaders: an older install's layout (NewVegasReloaded.imgui.ini) is read once while the new file is missing;
	// ImGui then saves it under the new name. The old file is not changed.
	const std::string oldLayout = Paths::OldPathIfRead(Paths::ModuleDir(), Paths::File::ImGuiLayout);
	if (!oldLayout.empty()) {
		ImGui::LoadIniSettingsFromDisk(oldLayout.c_str());
		Logger::Log("ImGuiManager: window layout read from %s (saved as %s from now on)", oldLayout.c_str(), io.IniFilename);
	}
	ApplySavedTextSize(io);
	MenuSkin::Apply();	// 2026-11-02: the menu's look, [Main.Menu.Style] Skin

	ImGui_ImplWin32_Init(window);
	ImGui_ImplDX9_Init(d3d);
	gOverlay.gameWndProc = (WNDPROC)SetWindowLong(window, GWL_WNDPROC, (LONG)(LONG_PTR)OverlayWndProc);
	HookMouseOnce();

	gOverlay.ready = true;
	Logger::Log("ImGuiManager: Initialized");
	CompileProgress::Finish();	// UNOFFICIAL 2026-11-20: startup shader compilation finished; close progress overlay if shown
	ShaderRecord::LogCompileTimes();	// UNOFFICIAL 2026-11-11: the start's slowest shader compiles (log only; docs/next/SHADER-COMPILE-TIMES-2026-11-11.md)
}

void ImGuiManager::Shutdown() {
	CompileProgress::Finish();
	if (!gOverlay.ready) return;
	SetOverlayVisible(false);
	MenuSkin::Release();
	SetWindowLong(gOverlay.window, GWL_WNDPROC, (LONG)(LONG_PTR)gOverlay.gameWndProc);
	ImGui_ImplDX9_Shutdown();	// the backends go in the reverse order of their start
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	gOverlay.ready = false;
	Logger::Log("ImGuiManager: Shutdown");
}

bool ImGuiManager::IsVisible() { return gOverlay.open; }
void ImGuiManager::SetVisible(bool visible) { gOverlay.open = visible; }
HWND ImGuiManager::GetWindow() { return gOverlay.window; }
bool ImGuiManager::TextInputActive() { return gOverlay.ready && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }

// ---- Keyboard ------------------------------------------------------------------------------------------------------------

// Windows key -> ImGui key, for the keys the menu uses (editing, text, the keypad). Letters and digits are worked out below.
static ImGuiKey ImGuiKeyFor(int vk) {
	if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
	if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
	static const struct { int vk; ImGuiKey key; } kOthers[] = {
		{ VK_TAB, ImGuiKey_Tab }, { VK_BACK, ImGuiKey_Backspace }, { VK_RETURN, ImGuiKey_Enter },
		{ VK_ESCAPE, ImGuiKey_Escape }, { VK_SPACE, ImGuiKey_Space },
		{ VK_LEFT, ImGuiKey_LeftArrow }, { VK_RIGHT, ImGuiKey_RightArrow },
		{ VK_UP, ImGuiKey_UpArrow }, { VK_DOWN, ImGuiKey_DownArrow },
		{ VK_PRIOR, ImGuiKey_PageUp }, { VK_NEXT, ImGuiKey_PageDown },
		{ VK_HOME, ImGuiKey_Home }, { VK_END, ImGuiKey_End },
		{ VK_INSERT, ImGuiKey_Insert }, { VK_DELETE, ImGuiKey_Delete },
		{ VK_LSHIFT, ImGuiKey_LeftShift }, { VK_RSHIFT, ImGuiKey_RightShift },
		{ VK_LCONTROL, ImGuiKey_LeftCtrl }, { VK_RCONTROL, ImGuiKey_RightCtrl },
		{ VK_LMENU, ImGuiKey_LeftAlt }, { VK_RMENU, ImGuiKey_RightAlt },
		{ VK_ADD, ImGuiKey_KeypadAdd }, { VK_SUBTRACT, ImGuiKey_KeypadSubtract },
		{ VK_MULTIPLY, ImGuiKey_KeypadMultiply }, { VK_DIVIDE, ImGuiKey_KeypadDivide },
	};
	for (const auto& entry : kOthers)
		if (entry.vk == vk) return entry.key;
	return ImGuiKey_None;
}

// DirectInput scan code -> Windows key. MapVirtualKey gets the E0-prefixed keys wrong (End, Home, the arrows: 0 or another
// key), so those come from this list.
static int VkForScanCode(BYTE dik) {
	static const BYTE kExtended[][2] = {
		{ 0x9C, VK_RETURN }, { 0x9D, VK_RCONTROL }, { 0xB5, VK_DIVIDE }, { 0xB8, VK_RMENU }, { 0xC5, VK_PAUSE },
		{ 0xC7, VK_HOME }, { 0xC8, VK_UP }, { 0xC9, VK_PRIOR }, { 0xCB, VK_LEFT }, { 0xCD, VK_RIGHT },
		{ 0xCF, VK_END }, { 0xD0, VK_DOWN }, { 0xD1, VK_NEXT }, { 0xD2, VK_INSERT }, { 0xD3, VK_DELETE },
	};
	for (const auto& pair : kExtended)
		if (pair[0] == dik) return pair[1];
	return (int)MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX);
}

// The menu key opens and closes the menu (alone: with the FX modifier held it is the master switch instead). In screenshot
// mode it brings the menu back. While a text box has the keyboard, a menu key that types a character is typing.
static void PollMenuKeys() {
	if (!TheSettingManager) return;
	const int vk = VkForScanCode((BYTE)TheSettingManager->SettingsMain.Menu.KeyEnable);
	if (!vk) return;
	if (gMenu.fxModifier < 0) gMenu.fxModifier = (int)TheSettingManager->SettingsMain.Menu.MasterSwitchModifier;

	const bool keyDown = KeyHeld(vk);
	static const int kModifierKeys[] = { 0, VK_CONTROL, VK_MENU, VK_SHIFT };	// by fxModifier
	const int modifier = gMenu.fxModifier >= 1 && gMenu.fxModifier <= 3 ? kModifierKeys[gMenu.fxModifier] : 0;
	const bool modifierDown = modifier && KeyHeld(modifier);
	const bool typing = ImGuiManager::IsVisible() && ImGui::GetIO().WantTextInput && MapVirtualKey(vk, MAPVK_VK_TO_CHAR) != 0;

	static KeyEdge menuKey;
	if (menuKey.Rose(keyDown) && !modifierDown && !typing) {
		if (MenuTools::ScreenshotMode) MenuTools::ScreenshotMode = false;
		else SetOverlayVisible(!ImGuiManager::IsVisible());
	}

	// RenderEffects master switch: modifier + menu key
	if (gMenu.fxModifier > 0) {
		static KeyEdge masterKey;
		if (masterKey.Rose(modifierDown && keyDown) && !typing) {
			const bool effectsOn = TheSettingManager->SettingsMain.Main.RenderEffects;
			TheSettingManager->SetSetting("Main.Main.Misc", "RenderEffects", !effectsOn);
			TheSettingManager->LoadSettings();
		}
	}
}

// UNOFFICIAL 2026-11-28 (deepprobe-live): the Performance probe's live-mode key ([Main.Develop.PerformanceProbe] LiveKey, Shift by
// default) starts live mode or stops any running capture. Polled like the menu key (DXVK swallows key messages), only while the game
// window is in front and no text box has the keyboard. The settings are read again every ~2 s, not every frame.
static void PollProbeKey() {
	if (!TheSettingManager) return;
	static int vk = 0, frames = 0;
	static bool needShift = true;
	if (frames-- <= 0) {
		frames = 120;
		vk = VkForScanCode((BYTE)TheSettingManager->GetSettingI("Main.Develop.PerformanceProbe", "LiveKey"));
		needShift = TheSettingManager->GetSettingI("Main.Develop.PerformanceProbe", "LiveKeyShift") != 0;
		// (2026-11-28) LiveRequest 1 starts live mode, 2 stops it, read with the key every 120 frames and put back to 0: a shot list
		// (the unattended bench) cannot press the key.
		const int request = TheSettingManager->GetSettingI("Main.Develop.PerformanceProbe", "LiveRequest");
		if (request != 0) {
			TheSettingManager->SetSetting("Main.Develop.PerformanceProbe", "LiveRequest", 0);
			if (request == 1 && !DeepProbe::LiveActive()) DeepProbe::RequestStartLive();
			else if (request == 2 && DeepProbe::Active()) DeepProbe::RequestStop();
			else if (request == 3 && !DeepProbe::Active()) DeepProbe::RequestStart(20.0, false);	// a timed 20 s capture (the bench)
		}
	}
	HWND front = GetForegroundWindow();
	DWORD pid = 0;
	if (front) GetWindowThreadProcessId(front, &pid);
	DeepProbe::SetGameInFront(pid == GetCurrentProcessId());	// the probe's watchdog reads this instead of calling user32
	static KeyEdge probeKey;
	if (!vk) { probeKey.Rose(false); return; }
	const bool typing = ImGuiManager::IsVisible() && ImGui::GetIO().WantTextInput;
	const bool down = pid == GetCurrentProcessId() && !typing && KeyHeld(vk) && (!needShift || KeyHeld(VK_SHIFT));
	if (probeKey.Rose(down)) DeepProbe::ToggleLive();
}

// Screenshot mode hands input back to the game (and hides the cursor) until it ends.
static void FollowScreenshotMode() {
	static bool wasOn = false;
	if (MenuTools::ScreenshotMode == wasOn) return;
	wasOn = MenuTools::ScreenshotMode;
	SetGameInputBlocked(!wasOn);
	ImGui::GetIO().MouseDrawCursor = !wasOn;
}

// Insert on a shader's page switches that shader (not while typing).
static void PollInsertKey() {
	if (gMenu.page.compare(0, 8, "Shaders.") != 0) return;
	static KeyEdge insert;
	if (!insert.Rose(KeyHeld(VK_INSERT)) || ImGui::GetIO().WantTextInput) return;
	const size_t nameStart = gMenu.page.find('.') + 1;
	const size_t nameEnd = gMenu.page.find('.', nameStart);
	if (nameEnd == std::string::npos) return;
	const std::string shader = gMenu.page.substr(nameStart, nameEnd - nameStart);
	TheShaderManager->SwitchShaderStatus(shader.c_str());
	TheSettingManager->LoadSettings();
}

// Key presses and releases for ImGui, and the characters they type (with the Shift / Ctrl / Alt / Caps Lock state).
static void FeedKeyboard() {
	ImGuiIO& io = ImGui::GetIO();
	for (int vk = 1; vk < 256; vk++) {
		const bool now = KeyHeld(vk);
		if (now == ((gMenu.keyWas[vk] & 0x80) != 0)) continue;
		gMenu.keyWas[vk] = now ? 0x80 : 0;
		const ImGuiKey key = ImGuiKeyFor(vk);
		if (key != ImGuiKey_None) io.AddKeyEvent(key, now);
		if (!now) continue;
		BYTE keyboard[256] = {};
		keyboard[VK_SHIFT] = KeyHeld(VK_SHIFT) ? 0x80 : 0;
		keyboard[VK_CONTROL] = KeyHeld(VK_CONTROL) ? 0x80 : 0;
		keyboard[VK_MENU] = KeyHeld(VK_MENU) ? 0x80 : 0;
		keyboard[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 0x01 : 0;
		keyboard[vk] = 0x80;
		WCHAR typed[4] = {};
		const int count = ToUnicode(vk, MapVirtualKey(vk, MAPVK_VK_TO_VSC), keyboard, typed, 4, 0);
		for (int i = 0; i < count; i++) io.AddInputCharacterUTF16((ImWchar16)typed[i]);
	}
}

// UNOFFICIAL 2026-10-30 MenuHiddenLean ([Main.Main.Performance], live; ROOTPLAN round 7 step 18): with the menu closed the
// Win32 backend still asked Windows for the foreground window and the cursor every frame (~0.7-0.9 % of the main thread).
// Closed, only what a hidden frame needs is set here (the display size and the time step); open, the backend runs as usual.
static void BackendNewFrame() {
	static bool backendRan = true;
	const bool lean = ImGuiManager::IsVisible() == false && TheSettingManager &&
		TheSettingManager->GetSettingI("Main.Main.Performance", "MenuHiddenLean");
	if (!lean) {
		ImGui_ImplWin32_NewFrame();	// (the first frame back: its time step spans the closed frames)
		backendRan = true;
		return;
	}
	static LARGE_INTEGER ticksPerSecond = {};
	static INT64 lastTicks = 0;
	if (!ticksPerSecond.QuadPart) ::QueryPerformanceFrequency(&ticksPerSecond);
	LARGE_INTEGER ticks;
	::QueryPerformanceCounter(&ticks);
	ImGuiIO& io = ImGui::GetIO();
	RECT client = { 0, 0, 0, 0 };
	::GetClientRect(TheRenderManager->m_kWndFocus, &client);
	io.DisplaySize = ImVec2((float)(client.right - client.left), (float)(client.bottom - client.top));
	const bool firstLean = backendRan || !lastTicks;
	io.DeltaTime = firstLean ? 1.0f / 60.0f : (float)((double)(ticks.QuadPart - lastTicks) / (double)ticksPerSecond.QuadPart);
	if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0e-5f;
	lastTicks = ticks.QuadPart;
	backendRan = false;
}

// The Win32 backend stops reading the cursor once it has seen a WM_MOUSEMOVE (it then waits for more, which the game's raw
// input, RIDEV_NOLEGACY, never lets through): after a click back into the window the cursor would freeze. The position is
// given again here, after the backend; the last one queued wins.
static void FeedCursorPosition() {
	const HWND window = TheRenderManager->m_kWndFocus;
	POINT cursor;
	RECT client;
	if (!::GetCursorPos(&cursor) || !::GetClientRect(window, &client)) return;
	::ScreenToClient(window, &cursor);
	if (cursor.x < client.left || cursor.x >= client.right || cursor.y < client.top || cursor.y >= client.bottom) return;
	ImGui::GetIO().AddMousePosEvent((float)cursor.x, (float)cursor.y);
}

// Mouse buttons, the wheel, and the +/- keys (numpad or main row) that step the setting under the mouse.
static void FeedMouseAndStepKeys() {
	ImGuiIO& io = ImGui::GetIO();
	static const int kButtons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
	for (int i = 0; i < 3; i++) io.AddMouseButtonEvent(i, KeyHeld(kButtons[i]));
	if (gMenu.wheel != 0.0f) {
		io.AddMouseWheelEvent(0.0f, gMenu.wheel);
		gMenu.wheel = 0.0f;
	}
	static KeyEdge plus, minus;
	const bool plusRose = plus.Rose(KeyHeld(VK_ADD) || KeyHeld(VK_OEM_PLUS));
	const bool minusRose = minus.Rose(KeyHeld(VK_SUBTRACT) || KeyHeld(VK_OEM_MINUS));
	// UNOFFICIAL 2026-10-25: a '-' typed into a text box (the search) does not step anything
	gMenu.plusKey = plusRose && !io.WantTextInput;
	gMenu.minusKey = minusRose && !io.WantTextInput;
}

// ---- Frame ------------------------------------------------------------------------------------------------------------------

// A lost device drops ImGui's D3D objects (once); when reset succeeds they are made again. Returns false while lost or un-reset.
static bool FollowDeviceState() {
	const HRESULT state = TheRenderManager->device->TestCooperativeLevel();
	if (state == D3DERR_DEVICELOST || state == D3DERR_DEVICENOTRESET) {
		if (!gOverlay.deviceLost) {
			ImGui_ImplDX9_InvalidateDeviceObjects();
			DistantLandTerrain::OnDeviceLost();
			DistantLandProto::ReleaseDefaultPool();
			MaterialSSSProto::ReleaseDefaultPool();
		}
		gOverlay.deviceLost = true;
		return false;
	}
	if (gOverlay.deviceLost && state == D3D_OK) {
		ImGui_ImplDX9_CreateDeviceObjects();
		gOverlay.deviceLost = false;
		HookMouseOnce();
	}
	return state == D3D_OK;
}

void ImGuiManager::NewFrame() {
	SessionLog::StageScope stage("the settings menu (input)");	// 2026-11-04 (CRASH lines)
	if (!gOverlay.ready) Initialize();
	if (!gOverlay.ready || !FollowDeviceState()) return;

	// A game menu (inventory, Pip-Boy, ...) takes over from ours
	if (gOverlay.open && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_None)) SetOverlayVisible(false);

	PollMenuKeys();
	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_None) || gOverlay.open) PollProbeKey();	// in game, not in the game's menus
	if (gOverlay.open) {
		FollowScreenshotMode();
		PollInsertKey();
		FeedKeyboard();
	}

	ImGui_ImplDX9_NewFrame();
	BackendNewFrame();
	if (gOverlay.open) FeedCursorPosition();

	ImGui::NewFrame();
	if (gOverlay.open) FeedMouseAndStepKeys();
}

static void DrawMenuFrame();

void ImGuiManager::Render() {
	SessionLog::StageScope stage("the settings menu (drawing)");	// 2026-11-04 (CRASH lines)
	if (!gOverlay.ready || gOverlay.deviceLost) return;
	DrawMenuFrame();
	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

#include "imgui/ImGuiSettingsUI.inl"
#include "imgui/ImGuiPages.inl"
#include "imgui/ImGuiSidebarAndTools.inl"
// ---- Menu bar ------------------------------------------------------------------------------------------------------------

static const ImVec4 kWarnAmber(1.0f, 0.75f, 0.2f, 1.0f);

static void DrawFileMenu() {
	if (!ImGui::BeginMenu("File")) return;
	if (ImGui::MenuItem("Save")) SaveSettingsNow();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save your changed settings to VegasShaders.toml");
	if (ImGui::MenuItem("Save Copy")) SaveSettingsCopy();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes a dated copy of the settings next to the DLL, named after where you are.");
	ImGui::Separator();
	if (ImGui::MenuItem("Revert")) RevertSessionValues();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo every unsaved change (back to the last Save)");	// 2026-11-26
	if (ImGui::MenuItem("Reload from Disk...")) gMenu.diskConfirm = true;
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload all settings from saved TOML file");
	ImGui::EndMenu();
}

// Text size: A- and A+ change it by 10 % (50-200 %) and save it as [Main.Menu.Style] TextSize.
static void DrawTextSizeControls() {
	ImGuiIO& io = ImGui::GetIO();
	const auto resize = [&io](float scale) {
		io.FontGlobalScale = scale;
		TheSettingManager->SetSetting("Main.Menu.Style", "TextSize", (int)(scale * 100.0f + 0.5f));
		TheSettingManager->LoadSettings();
	};
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Text size");
	ImGui::SameLine();
	if (ImGui::SmallButton("A-")) resize(ImMax(0.5f, io.FontGlobalScale - 0.1f));
	ImGui::SameLine();
	ImGui::Text("%.0f%%", io.FontGlobalScale * 100.0f);
	ImGui::SameLine();
	if (ImGui::SmallButton("A+")) resize(ImMin(2.0f, io.FontGlobalScale + 0.1f));
}

static void DrawViewMenu() {
	if (!ImGui::BeginMenu("View")) return;
	DrawTextSizeControls();
	if (ImGui::BeginMenu("Skin")) {	// 2026-11-02
		MenuSkin::DrawMenu();
		ImGui::EndMenu();
	}
	ImGui::Separator();
	// UNOFFICIAL 2026-10-25 menu tidy: one switch (off by default) for what a player does not need: test and fault-finding
	// switches, the fixes and same-picture speed-ups that are on by default, A/B paths, measuring hooks ([Main.Menu.Developer]
	// DevOnly), the step size and -/+ buttons and Retro-Encab. Hidden only: hidden settings keep their saved values.
	// UNOFFICIAL 2026-11-12: the fixes, speed-ups, test keys and debug views moved on to [Main.Menu.Developer] Internal, which this
	// switch does not show (IsInternalSetting); it now shows experimental features and the options a tester may be asked to flip.
	if (ImGui::MenuItem("Developer options", nullptr, gMenu.devOptions)) {
		const bool on = !gMenu.devOptions;
		if (on && !gMenu.devWarned) gMenu.devWarnAsk = true;	// UNOFFICIAL 2026-11-12: its warning first, once a session (DrawDevWarningPopup)
		else SetDeveloperOptions(on);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Also show test switches a player should not need to touch: experimental features that are off by\n"
			"default, options to try while a fault is looked into, tuning that changes almost nothing, and the step\n"
			"size and - / + buttons. Hidden settings keep working with their saved values.");
	// UNOFFICIAL 2026-11-12: always here, Developer options on or off: it also resets the Internal settings, which are never shown
	if (ImGui::MenuItem("Reset test and internal settings to defaults...")) gMenu.testResetAsk = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Puts every Developer options setting, and every internal setting the menu never shows, back to its\n"
			"default. Your other settings stay. It asks first and says how many it reset.");
	// UNOFFICIAL 2026-11-12 (core/PtApplies.h): the notes that say what each effect does while path tracing replaces the scene
	if (ImGui::MenuItem("Path tracing notes", nullptr, gMenu.ptNotes)) {
		gMenu.ptNotes = !gMenu.ptNotes;
		TheSettingManager->SetSetting("Main.Menu.Developer", "PathTracingNotes", gMenu.ptNotes);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("While path tracing replaces the scene, say what each effect still does: a line at the top of its\n"
			"page, a tag beside its name in the effects list, and the whole list on the Ray tracing page.\n"
			"Nothing shows while path tracing is off.");
	if (DevOptionsShown() && ImGui::BeginMenu("Step size")) {	// what +/- and the -/+ buttons add
		static const float kSteps[] = { 0.001f, 0.01f, 0.1f, 1.0f };
		static const char* const kStepNames[] = { "0.001", "0.01", "0.1", "1.0" };
		for (int i = 0; i < 4; i++)
			if (ImGui::MenuItem(kStepNames[i], nullptr, fabsf(gMenu.step - kSteps[i]) < 1e-6f)) gMenu.step = kSteps[i];
		ImGui::EndMenu();
	}
	ImGui::EndMenu();
}

// Help: the install check and the bug report (2026-11-03), the About window (credits)
static void DrawHelpMenu() {
	if (!ImGui::BeginMenu("Help")) return;
	ImGui::MenuItem("Install check", nullptr, &gMenu.installCheckOpen);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the start-up check found: old or missing files, the settings file, DXVK.");
	ImGui::MenuItem("Performance probe...", nullptr, &gMenu.deepProbeOpen);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Run a timed full-game performance probe: breaks down CPU stages, every loaded DLL / NVSE plugin,\nevery running script & ESP/ESM mod, Vegas Shaders GPU passes, and a draw-call / light / actor census.");
	if (ImGui::MenuItem("Copy bug report")) CopyBugReport();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Copies the version, GPU, DXVK, the install check, every setting you changed and this session's whole log.\nPaste it into your report.");
	ImGui::Separator();
	ImGui::MenuItem("About Vegas Shaders", nullptr, &MenuTools::AboutOpen);
	ImGui::EndMenu();
}

static void DrawToolsMenu() {
	if (!ImGui::BeginMenu("Tools")) return;
	ImGui::MenuItem("Presets", nullptr, &MenuTools::PresetsOpen);
	ImGui::MenuItem("Lighting", nullptr, &MenuTools::LightingOpen);
	if (ImGui::MenuItem("Dev Tools", nullptr, &MenuTools::DevToolsOpen) && !MenuTools::DevToolsOpen) MenuTools::OnDevToolsClosed();
	if (ImGui::BeginMenu("FX key")) {	// the master switch's modifier: the file gives the start value, the menu changes it
		static const char* const kModifiers[] = { "Off", "Ctrl", "Alt", "Shift" };
		for (int i = 0; i < 4; i++) {
			if (!ImGui::MenuItem(kModifiers[i], nullptr, gMenu.fxModifier == i)) continue;
			gMenu.fxModifier = i;
			TheSettingManager->SetSetting("Main.Menu.Keys", "MasterSwitchModifier", i);
			TheSettingManager->LoadSettings();
		}
		ImGui::EndMenu();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Held with %s, switches all effects on or off.", DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));
	if (DevOptionsShown()) {	// UNOFFICIAL 2026-10-25: Developer options only
		ImGui::Separator();
		ImGui::MenuItem("Retro-Encab.", nullptr, &MenuTools::EncabOpen);
	}
	ImGui::EndMenu();
}

// A label and a switch, for the right end of the menu bar. Returns whether the switch was clicked.
static bool LabelledToggle(const char* label, const char* id, bool* on) {
	ImGui::TextUnformatted(label);
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	return MenuWidgets::Toggle(id, on);
}

// Effects (every effect on or off) and Advanced (simple views vs all raw settings). Max Quality lives in Quick settings & Main > Quality.
static void DrawGlobalSwitches() {
	bool effects = TheSettingManager->SettingsMain.Main.RenderEffects;
	if (LabelledToggle("Effects", "##effects", &effects)) {
		TheSettingManager->SetSetting("Main.Main.Misc", "RenderEffects", effects);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Toggle all effects (hotkey: modifier + %s)", DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));

	// 2026-11-05: Advanced (simple views off: every setting under its own name)
	bool advanced = gMenu.advanced;
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
	if (LabelledToggle("Advanced", "##advanced", &advanced)) {
		TheSettingManager->SetSetting("Main.Menu.Developer", "AdvancedSettings", advanced);
		TheSettingManager->LoadSettings();
		gMenu.advanced = advanced;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Off: pages with a simple view show the main settings with plain names, and choices that set several at once.\n"
			"On: every setting under its own name, as in guides and logs. Nothing changes in your saved settings either way.");
}

static float GlobalSwitchesWidth() {
	const ImGuiStyle& style = ImGui::GetStyle();
	return ImGui::CalcTextSize("Effects").x + ImGui::CalcTextSize("Advanced").x + MenuWidgets::ToggleWidth() * 2.0f +
		style.ItemInnerSpacing.x * 2.0f + style.ItemSpacing.x * 2.0f;
}

static void DrawUnsavedMarker() {
	const float radius = ImGui::GetFontSize() * 0.22f;
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const float lineH = ImGui::GetFrameHeight();
	ImGui::Dummy(ImVec2(radius * 2.0f, lineH));
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + radius, at.y + lineH * 0.5f), radius, ImGui::GetColorU32(kWarnAmber));
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextColored(kWarnAmber, "Unsaved");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some settings changed since the last Save (File > Save).");
}

static float UnsavedMarkerWidth() {
	return ImGui::GetFontSize() * 0.44f + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Unsaved").x;
}

// 2026-11-03 (the user: "the save button always visible again", as before the wave 4 menu rewrite): a Save button on the menu
// bar, which never scrolls away; amber while there are unsaved changes. File > Save does the same.
static float SaveButtonWidth() {
	return ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

static void DrawSaveButton(bool unsaved) {
	if (unsaved) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kWarnAmber.x * 0.55f, kWarnAmber.y * 0.55f, kWarnAmber.z * 0.55f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kWarnAmber.x * 0.75f, kWarnAmber.y * 0.75f, kWarnAmber.z * 0.75f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, kWarnAmber);
	}
	if (ImGui::Button("Save##menubar")) SaveSettingsNow();
	if (unsaved) ImGui::PopStyleColor(3);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(unsaved ? "Save the changed settings to VegasShaders.toml" : "Nothing changed since the last save");
}

static void DrawMenuBar() {
	if (gMenu.fxModifier < 0) gMenu.fxModifier = (int)TheSettingManager->SettingsMain.Menu.MasterSwitchModifier;
	if (!ImGui::BeginMenuBar()) return;
	DrawFileMenu();
	DrawViewMenu();
	DrawToolsMenu();
	DrawHelpMenu();

	// Right end: the unsaved marker (when there is room), the Save button and the two global switches
	const bool unsaved = TheSettingManager->hasUnsavedChanges;
	const float gap = ImGui::GetStyle().ItemSpacing.x * 2.0f;
	const float switchesW = GlobalSwitchesWidth();
	const float saveW = SaveButtonWidth() + gap;
	const float markerW = unsaved ? UnsavedMarkerWidth() + gap : 0.0f;
	ImGui::SameLine();
	const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	const float menusEnd = ImGui::GetCursorPosX() + gap;
	const bool roomForMarker = unsaved && rightEdge - switchesW - saveW - markerW >= menusEnd;
	const float start = rightEdge - switchesW - saveW - (roomForMarker ? markerW : 0.0f);
	if (start > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(start);
	if (roomForMarker) {
		DrawUnsavedMarker();
		ImGui::SameLine(0.0f, gap);
	}
	DrawSaveButton(unsaved);
	ImGui::SameLine(0.0f, gap);
	DrawGlobalSwitches();
	ImGui::EndMenuBar();
}

// ---- Status bar --------------------------------------------------------------------------------------------------------------
// One line under the panels: the version, what waits for a restart, the tools that are running, and unsaved changes at the right.

static void StatusItem(const ImVec4* colour, const char* text) {
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 3.0f);
	if (colour) ImGui::TextColored(*colour, "%s", text);
	else ImGui::TextDisabled("%s", text);
}

static void DrawStatusBar() {
	const ImVec2 lineStart = ImGui::GetCursorScreenPos();
	const float width = ImGui::GetContentRegionAvail().x;
	const float y = lineStart.y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
	ImGui::GetWindowDrawList()->AddLine(ImVec2(lineStart.x, y), ImVec2(lineStart.x + width, y), ImGui::GetColorU32(ImGuiCol_Separator));

	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s %s", Branding::VersionLabel, Branding::Version);

	char text[96];
	const int waiting = PendingRestartCount();
	if (waiting) {
		sprintf_s(text, "%d change%s wait%s for a restart", waiting, waiting == 1 ? "" : "s", waiting == 1 ? "s" : "");
		StatusItem(&kRestartOrange, text);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Marked \"restart to apply\" on their rows: press Save, then restart the game.");
	}
	const bool maxAsked = TheSettingManager->GetSettingI("Main.Main.Quality", "MaxQuality") != 0;
	if (maxAsked != MaxQuality::Active) {
		sprintf_s(text, "Save + restart to turn Max Quality %s", maxAsked ? "on" : "off");
		StatusItem(&kWarnAmber, text);
	}
	else if (MaxQuality::Active) {
		static const ImVec4 kMaxRed(1.0f, 0.45f, 0.45f, 1.0f);
		StatusItem(&kMaxRed, "[Max Quality active]");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Max Quality screenshot mode is active (Quick settings > Screenshot mode or Main > Quality to turn off).");
	}
	if (!Paths::TwinDll.empty()) {	// 2026-11-03: an old NewVegasReloaded.dll next to this one (RenderTwinDllWarning)
		static const ImVec4 kTwinRed(1.0f, 0.3f, 0.25f, 1.0f);
		StatusItem(&kTwinRed, "Old NewVegasReloaded.dll installed: delete it");
		if (ImGui::IsItemHovered()) WrappedTooltip(("Both plugins load and both replace the game's shaders (white or broken objects). Delete or move " + Paths::TwinDll + ", then restart.").c_str());
	}
	if (const int issues = InstallIssueCount()) {	// 2026-11-03: the install self-check (InstallCheck.h)
		const bool problem = InstallCheck::Count(InstallCheck::Level::Problem) > (Paths::TwinDll.empty() ? 0 : 1);
		static const ImVec4 kProblemRed(1.0f, 0.35f, 0.3f, 1.0f);
		sprintf_s(text, "Install check: %d to fix", issues);
		StatusItem(problem ? &kProblemRed : &kWarnAmber, text);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click for the details (Help > Install check).");
		if (ImGui::IsItemClicked()) gMenu.installCheckOpen = true;
	}
	if (!Paths::OldFilesNotices().empty()) {	// 2026-11-02: files from the old New Vegas Reloaded places (Paths.h)
		const size_t count = Paths::OldFilesNotices().size();
		sprintf_s(text, "%zu note%s about New Vegas Reloaded files", count, count == 1 ? "" : "s");
		StatusItem(&kWarnAmber, text);
		if (ImGui::IsItemHovered()) {
			std::string all;
			for (const std::string& n : Paths::OldFilesNotices()) all += (all.empty() ? "" : "\n\n") + n;
			WrappedTooltip(all.c_str());
		}
	}
	if (!FailOnce::Notices.empty()) {	// UNOFFICIAL 2026-11-02 RetryFailedEffects (core/FailOnce.h)
		const size_t count = FailOnce::Notices.size();
		sprintf_s(text, "%zu effect part%s switched off after an error", count, count == 1 ? "" : "s");
		StatusItem(&kWarnAmber, text);
		if (ImGui::IsItemHovered()) {
			std::string all;
			for (const std::string& n : FailOnce::Notices) all += (all.empty() ? "" : "\n\n") + n;
			WrappedTooltip(all.c_str());
		}
	}
	if (const long compileFails = EffectStatus::CompileFailureCount(); compileFails > 0) {
		static const ImVec4 kErrRed(1.0f, 0.35f, 0.35f, 1.0f);
		sprintf_s(text, "%ld shader%s failed to compile", compileFails, compileFails == 1 ? "" : "s");
		StatusItem(&kErrRed, text);
		if (ImGui::IsItemHovered()) {
			std::string tip;
			const std::vector<std::string> items = EffectStatus::CompileFailureItems();
			const size_t limit = (std::min)(items.size(), (size_t)16);
			for (size_t i = 0; i < limit; i++) tip += (tip.empty() ? "" : "\n") + items[i];
			if (items.size() > limit) tip += "\n... (" + std::to_string(items.size() - limit) + " more)";
			if (!tip.empty()) tip += "\n\n";
			tip += "Check VegasShaders-Logs for full details.";
			WrappedTooltip(tip.c_str());
		}
	}
	if (MenuTools::EncabActive()) {
		static const ImVec4 mint(0.4f, 1.0f, 0.7f, 1.0f);
		StatusItem(&mint, "[Retro-Encab. active]");
	}
	if (MenuTools::FreecamOn()) {
		static const ImVec4 orange(1.0f, 0.5f, 0.1f, 1.0f);
		StatusItem(&orange, "[freecam]");
	}
	if (TheSettingManager->hasUnsavedChanges) {
		const char* unsaved = "/!\\ Unsaved changes";
		ImGui::SameLine();
		const float right = lineStart.x + width - ImGui::CalcTextSize(unsaved).x;
		if (right > ImGui::GetCursorScreenPos().x) ImGui::SetCursorScreenPos(ImVec2(right, ImGui::GetCursorScreenPos().y));
		ImGui::TextColored(kWarnAmber, "%s", unsaved);
	}
}

// ---- Panels ------------------------------------------------------------------------------------------------------------------

// The gap between the sidebar and the page: dragging it resizes the sidebar, a double-click resets it. The width is saved to
// [Main.Menu.Style] SidebarWidth when the drag ends.
static void DrawSidebarSplitter(float height, float defaultW, float minW, float maxW, float gapW) {
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::InvisibleButton("##sidebarSplit", ImVec2(gapW, height));
	const bool hovered = ImGui::IsItemHovered(), dragging = ImGui::IsItemActive();
	if (hovered || dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	if (dragging) gMenu.sidebarWidth = ImClamp(gMenu.sidebarWidth + ImGui::GetIO().MouseDelta.x, minW, maxW);
	const bool reset = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
	if (reset) gMenu.sidebarWidth = defaultW;
	if (ImGui::IsItemDeactivated() || reset) {
		TheSettingManager->SetSetting("Main.Menu.Style", "SidebarWidth", (int)(gMenu.sidebarWidth + 0.5f));
		TheSettingManager->LoadSettings();
	}
	if (hovered || dragging) {
		const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
		const float x = (lo.x + hi.x) * 0.5f;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(x, lo.y), ImVec2(x, hi.y),
			ImGui::GetColorU32(dragging ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered), 2.0f);
	}
	ImGui::SameLine(0.0f, 0.0f);
}

// The sidebar and the page side by side, leaving one line at the bottom for the status bar.
static void DrawTwoPanels() {
	const float defaultW = 260.0f, minW = 120.0f, gapW = 8.0f;
	if (gMenu.sidebarWidth <= 0.0f) {
		const int saved = TheSettingManager->GetSettingI("Main.Menu.Style", "SidebarWidth");
		gMenu.sidebarWidth = saved > 0 ? (float)saved : defaultW;
	}
	const ImVec2 space = ImGui::GetContentRegionAvail();
	const float maxW = ImMax(minW, space.x - gapW - 200.0f);
	gMenu.sidebarWidth = ImClamp(gMenu.sidebarWidth, minW, maxW);
	const float height = ImMax(space.y - ImGui::GetFrameHeightWithSpacing(), ImGui::GetFrameHeight());

	ImGui::BeginChild("##sidebar", ImVec2(gMenu.sidebarWidth, height), ImGuiChildFlags_Borders);
	RenderSidebar();
	ImGui::EndChild();
	DrawSidebarSplitter(height, defaultW, minW, maxW, gapW);
	ImGui::BeginChild("##content", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
	RenderContent();
	RenderHidingNotes();	// 2026-11-05: what is hiding effects right now, in the page's empty space
	ImGui::EndChild();
}

// ---- Every frame -------------------------------------------------------------------------------------------------------------

static void DrawMenuFrame() {
	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) {	// the game's main menu: only the hint
		RenderMainMenuToast();
		RenderTwinDllWarning(true);
		RenderInstallNotice(true);
		return;
	}
	RenderProfilerIndicator();
	RenderTwinDllWarning(false);
	RenderInstallNotice(false);
	bool reopenProbe = false;
	if (!InterfaceManager->IsActive(Menu::MenuType::kMenuType_Loading) && DeepProbe::ConsumeJustFinished(reopenProbe)) {
		gMenu.deepProbeOpen = true;
		if (reopenProbe) SetOverlayVisible(true);
	}
	if (!gOverlay.open) return;

	gMenu.devOptions = TheSettingManager->GetSettingI("Main.Menu.Developer", "DeveloperOptions") != 0;	// UNOFFICIAL 2026-10-25
	gMenu.advanced = TheSettingManager->GetSettingI("Main.Menu.Developer", "AdvancedSettings") != 0;	// 2026-11-05
	gMenu.showInternal = TheSettingManager->GetSettingI("Main.Menu.Developer", "ShowInternal") != 0;	// UNOFFICIAL 2026-11-12 (the file's key)
	gMenu.ptNotes = TheSettingManager->GetSettingI("Main.Menu.Developer", "PathTracingNotes") != 0;	// UNOFFICIAL 2026-11-12 (View > Path tracing notes)
	MenuTools::FrameUpdate();
	if (MenuTools::ScreenshotMode) return;
	PixelInspector::DrawUI();	// UNOFFICIAL 2026-10-26: [Main.Develop.PixelInspector] / shader counters window (only while one is on)
	MenuTools::TrackCell();
	if (CloseMenuOnEscapeOrAlt()) return;

	ImGui::SetNextWindowSize(ImVec2(960.0f, 620.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));
	const ImGuiWindowFlags look = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar;
	bool keepOpen = true;
	const bool drawn = ImGui::Begin(Branding::MenuTitle, &keepOpen, look);
	if (drawn && keepOpen) {
		MenuSkin::DrawBackground();	// 2026-11-02: the skin's picture, if it has one
		DrawMenuBar();
		DrawTwoPanels();
		DrawStatusBar();
		DrawDiskReloadPopup();
		DrawConflictPopup();	// 2026-11-05
		DrawDevWarningPopup();	// UNOFFICIAL 2026-11-12
		DrawTestResetPopup();	// UNOFFICIAL 2026-11-12
	}
	ImGui::End();
	if (!keepOpen) {
		SetOverlayVisible(false);
		return;
	}
	if (!drawn) return;
	MenuTools::DrawWindows(gMenu.step);
	DrawInstallCheckWindow();
	DrawDeepProbeWindow();
}
