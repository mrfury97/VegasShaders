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
#include "Branding.h"
#include "FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects: the status bar lists the parts switched off
#include "InstallCheck.h"	// 2026-11-03: the install self-check (main-menu notice, Help > Install check, Copy bug report)
#include "RtCore.h"			// 2026-11-05: the Ray tracing page's status lines
#include "RtSunShadows.h"
#include "RtLampShadows.h"
#include "RtPathTrace.h"
#include "RtAmbient.h"
#include "../effects/Reflections/Reflections.h"
#include "../effects/AmbientOcclusion/AmbientOcclusion.h"
#include <sstream>
#include <array>
#include <iomanip>
#include <ctime>
#include <unordered_set>
#include <algorithm>

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
		bool diskConfirm = false;		// File > Reload from disk was picked: open its confirmation next to the main window
		bool installCheckOpen = false;	// 2026-11-03: Help > Install check
		double reportCopiedAt = -1.0;	// 2026-11-03: when Copy bug report was last pressed (ImGui time), for the "Copied" line
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

// File > Revert: every value seen since the menu opened goes back to what it was then.
static void RevertSessionValues() {
	static const char* const kLutKeys[] = { "DayLUT", "NightLUT", "InteriorLUT" };	// LUT slots 0, 1, 2
	LUTEffect* lut = TheShaderManager->Effects.LUT;
	for (const auto& [section, values] : gMenu.sessionValues) {
		for (const auto& [key, text] : values) {
			// A LUT name has to load its file: LUTEffect::UpdateSettings never re-reads the names, so writing it is not enough
			int slot = -1;
			if (lut && section == "Shaders.LUT.Main")
				for (int i = 0; i < 3; i++) if (key == kLutKeys[i]) slot = i;
			if (slot >= 0) {
				lut->LoadLUT(slot, text.c_str());
				continue;
			}
			TheSettingManager->SetSettingS(section.c_str(), key.c_str(), text.c_str());
		}
	}
	TheSettingManager->LoadSettings();
	SyncShaderSwitches();
	gMenu.resetWheels = true;
	gMenu.page.clear();
}

// ---- Window procedure ------------------------------------------------------------------------------------------------------

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// A console command posted by Dev Tools runs here, outside any frame (see MenuTools.cpp)
	if (MenuTools::HandleWindowMessage(msg, lParam)) return 0;

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
	if (gOverlay.ready) return;
	IDirect3DDevice9* d3d = TheRenderManager->device;
	const HWND window = TheRenderManager->m_kWndFocus;
	if (!d3d || !window) {
		Logger::Log("ImGuiManager: deferring init");
		return;
	}
	gOverlay.window = window;

	NewPatchRebuild();	// UNOFFICIAL 2026-10-18: the "New in this patch" list, once (it logs its size)
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
}

void ImGuiManager::Shutdown() {
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

// A lost device drops ImGui's D3D objects (once); when it can be reset they are made again. Returns false while it is lost.
static bool FollowDeviceState() {
	const HRESULT state = TheRenderManager->device->TestCooperativeLevel();
	if (state == D3DERR_DEVICELOST) {
		if (!gOverlay.deviceLost) ImGui_ImplDX9_InvalidateDeviceObjects();
		gOverlay.deviceLost = true;
		return false;
	}
	if (gOverlay.deviceLost && state == D3DERR_DEVICENOTRESET) {
		ImGui_ImplDX9_CreateDeviceObjects();
		gOverlay.deviceLost = false;
		HookMouseOnce();
	}
	return true;
}

void ImGuiManager::NewFrame() {
	SessionLog::StageScope stage("the settings menu (input)");	// 2026-11-04 (CRASH lines)
	if (!gOverlay.ready) Initialize();
	if (!gOverlay.ready || !FollowDeviceState()) return;

	// A game menu (inventory, Pip-Boy, ...) takes over from ours
	if (gOverlay.open && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_None)) SetOverlayVisible(false);

	PollMenuKeys();
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

// ---- What the menu leaves out --------------------------------------------------------------------------------------------

// Sections never listed: the weather mode, the status pages, the New-in-patch data and the developer page (its one switch is
// View > Developer options).
static bool ShouldHideSection(const std::string& name) {
	return name == "WeatherMode" || name == "Status" || name == "NewInPatch" || name == "Developer";
}

// Keys never drawn as rows: the old text menu's colours (TextColor*, TextShadow*), the LUT names (the LUT page has pickers for
// them), the New-in-patch and developer lists, and the sidebar width (set by dragging the sidebar's edge).
static bool ShouldHideKey(const char* key) {
	if (!strncmp(key, "TextColor", 9) || !strncmp(key, "TextShadow", 10)) return true;
	static const std::unordered_set<std::string> kRowless = {
		"DayLUT", "NightLUT", "InteriorLUT",
		"NewSince", "NewInclude", "NewExclude", "NewDiagnostics",
		"DevOnly", "SidebarWidth",
	};
	return kRowless.count(key) != 0;
}

// Kept in the file for old setups but read by nothing (the code that read them is commented out): drawn greyed, not hidden.
static bool IsInertSetting(const char* section, const char* key) {
	const bool shadowMaps = !strcmp(section, "Shaders.ShadowsExteriors.ShadowMaps");
	return shadowMaps && (!strcmp(key, "Mipmaps") || !strcmp(key, "Anisotropy"));
}

// A tooltip of wrapped text, about 28 characters of the current font wide per em.
static void WrappedTooltip(const char* text) {
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

// ---- Menu honesty ----------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-19 (P81 playtest reports: "lotta effects u gotta restart", "shadow blurring does not work", "a lot of
// interior shadow settings don't do anything"). The menu now says which settings wait for a game restart, greys the ones the
// current shadow mode or switch does not read (the reason on hover; they stay editable), and keeps test switches and settings
// nothing reads out of sight unless Display > "Show test settings" is ticked.

// 1. Settings read only when the game starts: compiled into the shaders (ShaderRecord::BuildDefines, EffectRecord::LoadEffect),
// or used to install hooks or make textures at start-up. TurnOn: built in only when on at start, switching it off works at
// once; Changed: any change waits for the restart. MaxQuality has its own note (RenderMaxQuality).
enum class RestartRule { Changed, TurnOn };
struct RestartSetting { const char* Section; const char* Key; RestartRule Rule; };
static const RestartSetting kRestartSettings[] = {
	{ "Main.Main.Misc",                       "MoreLampsPerObject", RestartRule::Changed },	// engine patch + SPECULAR_LAMPS6
	{ "Main.Main.Misc",                       "LampReachMargin",    RestartRule::Changed },	// engine patch (Hooks.cpp)
	{ "Main.Main.Performance",                "ParallelShaderCompile", RestartRule::Changed },
	{ "Main.Main.Performance",                "ShaderWarmUp",       RestartRule::Changed },	// runs once, at the main menu
	{ "Main.Main.Performance",                "SlimDepthBuffer",    RestartRule::Changed },	// depth texture format (CombineDepth)
	{ "Shaders.ShadowsExteriors.Main",        "ForwardShadows",     RestartRule::TurnOn },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Bend",               RestartRule::TurnOn },	// UNOFFICIAL 2026-11-06 SUN_CONTACT_FORWARD + texture at start
	{ "Shaders.ShadowsExteriors.ScreenSpace", "InLighting",         RestartRule::TurnOn },
	{ "Main.Main.Performance",                "NvrDxvkDisplacement", RestartRule::TurnOn },	// land shaders registered as they load ([Shaders.Terrain.Displacement] Enabled is live)
	{ "Shaders.ShadowsExteriors.SunSmoothing","CrossFade",          RestartRule::Changed },
	{ "Shaders.ContactHardening.Status",      "Enabled",            RestartRule::TurnOn },
	{ "Shaders.ShadowsInteriors.Forward",     "Enabled",            RestartRule::TurnOn },	// hooks + shader overrides at start
	{ "Shaders.ShadowsInteriors.Forward",     "SmoothEdges",        RestartRule::Changed },
	{ "Shaders.ShadowsInteriors.Forward",     "EvenTaps",           RestartRule::Changed },	// POINT_SHADOW_EVEN (deep/lampedge)
	{ "Shaders.ShadowsInteriors.Forward",     "MagnifiedSoftness",  RestartRule::TurnOn },	// POINT_SHADOW_MAG when above 0 at start; 0 is live
	{ "Shaders.ShadowsInteriors.Forward",     "TexelFilter",        RestartRule::Changed },	// POINT_SHADOW_TEXEL (deep/lampedge)
	{ "Shaders.ShadowsInteriors.RT",          "Enabled",            RestartRule::Changed },	// POINT_SHADOW_RT (RtLampShadows.h); Live is live
	{ "Shaders.ShadowsInteriors.Main",        "ShadowCubeMapSize",  RestartRule::Changed },	// cube maps made at start (ShadowManager)
	{ "Shaders.AmbientLight.Main",           "LowScaleFullRes",    RestartRule::Changed },	// UNOFFICIAL 2026-11-01: buffers made at start (AmbientLight.cpp EnsureBuffers)
	{ "Shaders.ShadowsInteriors.Atlas",       "NearCubeSize",       RestartRule::Changed },	// UNOFFICIAL 2026-11-01: cube slots 0-7 made at start (ShadowsExterior.cpp)
	{ "Shaders.PBR.Main",                     "LampReachSkip",      RestartRule::Changed },
	{ "Shaders.PBR.Main",                     "UnlitShadowSkip",    RestartRule::Changed },
	{ "Shaders.PBR.Main",                     "SmithCorrelated",    RestartRule::Changed },
	{ "Shaders.PBR.Main",                     "SpecularFade",       RestartRule::Changed },	// SPEC_FADE_ON (deep/pbrpop)
	{ "Shaders.PBR.Main",                     "FoldHighlights",     RestartRule::TurnOn },	// FOLD_HIGHLIGHTS_ON (deep/pbrtiles); off is live
	{ "Shaders.PBR.Main",                     "LampSums",           RestartRule::TurnOn },	// LAMP_SUMS_ON (deep/lampsums); off is live
	{ "Shaders.PBR.Main",                     "HighlightSums",      RestartRule::TurnOn },	// HIGHLIGHT_SUMS_ON (2026-10-25); off is live
	{ "Shaders.PBR.Main",                     "SkylightingMode",    RestartRule::Changed },
	{ "Shaders.PBR.Metal",                    "CubeReflections",    RestartRule::TurnOn },	// LIVE_CUBE: either above 0 at start
	{ "Shaders.PBR.EnvMaps",                  "CubeReflections",    RestartRule::TurnOn },
	{ "Shaders.PBR.MaterialRules",            "Enabled",            RestartRule::TurnOn },
	{ "Shaders.PBR.Rmaos",                    "Enabled",            RestartRule::Changed },
	{ "Shaders.PBR.Rmaos",                    "SpecularFromMap",    RestartRule::Changed },	// RMAOS_SPEC_OFF (2026-11-02)
	{ "Shaders.Reflections.Main",             "MaterialAware",      RestartRule::TurnOn },	// REFL_MATERIAL_ON (core/ReflMaterial.h)
	{ "Shaders.Reflections.Main",             "ReflectAdd",         RestartRule::TurnOn },	// REFL_ADD_ON (Reflections.fx.hlsl)
	{ "Shaders.Reflections.Main",             "SpecularOcclusion",  RestartRule::TurnOn },	// REFL_SPECOCC_ON (Reflections.fx.hlsl)
	{ "Shaders.PBR.Linear",                   "Enabled",            RestartRule::TurnOn },	// off at runtime = Gamma 1
	{ "Shaders.PBR.Linear",                   "PreDecode",          RestartRule::Changed },
	{ "Shaders.Sky.Physical",                 "Enabled",            RestartRule::TurnOn },
	{ "Shaders.Terrain.Blending",             "Enabled",            RestartRule::TurnOn },
	{ "Shaders.Terrain.Variation",            "Enabled",            RestartRule::Changed },	// TERRAIN_VARIATION (deep/csports)
	{ "Shaders.Terrain.Variation",            "Layers",             RestartRule::Changed },	// TERRAIN_VARIATION_LAYERS
	{ "Shaders.Terrain.Macro",                "Enabled",            RestartRule::Changed },	// TERRAIN_MACRO (2026-11-04)
	{ "Shaders.PBR.Dust",                     "Enabled",            RestartRule::Changed },	// DUST_ON (2026-11-04)
	{ "Shaders.PBR.MicroShadow",              "Enabled",            RestartRule::Changed },	// MICRO_SHADOW_ON (2026-11-04)
	{ "Shaders.PBR.Main",                     "VanillaBridge",      RestartRule::Changed },	// VANILLA_BRIDGE_ON (2026-11-05)
	{ "Shaders.HandBlood.Status",             "Enabled",            RestartRule::Changed },
	{ "Shaders.Cloth.Main",                   "Enabled",            RestartRule::TurnOn },
	{ "Shaders.WetnessEffects.HudRaindrops",  "Enabled",            RestartRule::TurnOn },	// HUD_RAIN_ON: on at start
	{ "Shaders.VolumetricClouds.Main",        "Resolution",         RestartRule::Changed },	// textures made at start
	{ "Shaders.VolumetricClouds.Main",        "HalfResolution",     RestartRule::Changed },
	{ "Shaders.VolumetricClouds.Main",        "ShadowResolution",   RestartRule::Changed },
	{ "Shaders.Skin.Main",                    "OverrideVanillaPlusSkin", RestartRule::Changed },	// picks the SKIN shader files at start (with Vanilla Plus Skin installed)
};

static std::unordered_map<std::string, float> s_startupValues;	// "Section.Key" -> its value when the game started

static float NumericSetting(const char* section, const char* key) {
	SettingManager::Configuration::ConfigNode node;
	if (!TheSettingManager->Config.FillNode(&node, section, key)) return 0.0f;
	using Config = SettingManager::Configuration;
	if (node.Type == Config::Boolean) return node.BoolValue ? 1.0f : 0.0f;
	return node.Type == Config::Integer ? (float)node.IntValue : node.FloatValue;
}

// From ImGuiManager::Initialize (the first rendered frame, before any location loads and before the menu can change anything).
static void CaptureStartupValues() {
	for (const RestartSetting& r : kRestartSettings)
		s_startupValues[std::string(r.Section) + "." + r.Key] = NumericSetting(r.Section, r.Key);
}

// What the start-up reads make of the setting: the value itself, or the on/off the define is built from.
static float RestartState(const RestartSetting& r, bool atStart) {
	auto get = [atStart](const char* section, const char* key) {
		if (!atStart) return NumericSetting(section, key);
		auto it = s_startupValues.find(std::string(section) + "." + key);
		return it != s_startupValues.end() ? it->second : NumericSetting(section, key);
	};
	const std::string key = r.Key;
	if (key == "CubeReflections")
		return (get("Shaders.PBR.Metal", "CubeReflections") > 0.0f || get("Shaders.PBR.EnvMaps", "CubeReflections") > 0.0f) ? 1.0f : 0.0f;
	if (key == "SmithCorrelated") return get(r.Section, r.Key) == 1.0f ? 1.0f : 0.0f;
	return get(r.Section, r.Key);
}

static const RestartSetting* FindRestartSetting(const char* section, const char* key) {
	for (const RestartSetting& r : kRestartSettings)
		if (!strcmp(r.Section, section) && !strcmp(r.Key, key)) return &r;
	return nullptr;
}

// What a restart-only setting's row says: "(restart)" or "(on after a restart)" in grey, or "restart to apply" in orange once
// it differs from what the game started with. Nothing for settings that work at once (a TurnOn one that was on at start).
struct RestartNote {
	const char* text = nullptr;
	bool pending = false;
	RestartRule rule = RestartRule::Changed;
};

static bool FindRestartNote(const char* section, const char* key, RestartNote& note) {
	const RestartSetting* r = FindRestartSetting(section, key);
	if (!r || s_startupValues.empty()) return false;
	const float atStart = RestartState(*r, true);
	const float now = RestartState(*r, false);
	const bool turnOn = r->Rule == RestartRule::TurnOn;
	note.rule = r->Rule;
	note.pending = turnOn ? (atStart == 0.0f && now != 0.0f) : atStart != now;
	if (turnOn && !note.pending && atStart != 0.0f) return false;	// built in: on and off both work at once
	note.text = note.pending ? "restart to apply" : turnOn ? "(on after a restart)" : "(restart)";
	return true;
}

static float RestartNoteWidth(const RestartNote& note) {
	return note.text ? ImGui::CalcTextSize(note.text).x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
}

static const ImVec4 kRestartOrange(1.0f, 0.6f, 0.2f, 1.0f);

static void DrawRestartNote(const RestartNote& note) {
	if (note.pending) ImGui::TextColored(kRestartOrange, "%s", note.text);
	else ImGui::TextDisabled("%s", note.text);
	if (!ImGui::IsItemHovered()) return;
	const char* explain =
		note.pending ? "Changed since the game started. Press Save, then restart the game: until then the game keeps what it "
			"started with."
		: note.rule == RestartRule::TurnOn ? "This was off when the game started, so it is not built in yet: turn it on, press "
			"Save and restart the game. (When the game starts with it on, switching it off and on works at once.)"
		: "Read when the game starts (built into the shaders or the game's setup): change it, press Save, then restart the game.";
	WrappedTooltip(explain);
}

// The note on the same line as what was drawn last (the sidebar's and the page header's switches).
static void RestartBadge(const char* section, const char* key) {
	RestartNote note;
	if (!FindRestartNote(section, key, note)) return;
	ImGui::SameLine();
	DrawRestartNote(note);
}

// How many restart-only settings changed since the game started (the status bar). Counted again when a setting changes.
static int PendingRestartCount() {
	static UInt32 countedAt = 0xFFFFFFFFu;
	static int count = 0;
	if (countedAt == TheSettingManager->SettingsRevision) return count;
	countedAt = TheSettingManager->SettingsRevision;
	count = 0;
	for (const RestartSetting& r : kRestartSettings) {
		RestartNote note;
		if (FindRestartNote(r.Section, r.Key, note) && note.pending) count++;
	}
	return count;
}

// 2. Settings the current mode does not read: the reason, or nullptr when it is in use. Each rule follows the code that reads it.
static bool IsDeadSetting(const char* section, const char* key) {
	// Nothing in this build reads these (kept in the file for old settings files).
	static const char* const dead[][2] = {
		{ "Shaders.ShadowsExteriors.Main", "OrthoMapResolution" },	// the ortho map size is [Shaders.ShadowsExteriors.Ortho] Resolution
		{ "Shaders.ShadowsExteriors.Main", "OrthoRadius" },			// its reach is [Shaders.ShadowsExteriors.Ortho] Distance
		{ "Shaders.ShadowsInteriors.Main", "Quality" },				// "Not currently used": only Oblivion's ShadowCube.hlsl read it
		{ "Shaders.ShadowsInteriors.Main", "UseCastShadowFlag" },	// read into Settings.Interiors, never used
		{ "Shaders.ShadowsInteriors.Main", "PlayerShadowFirstPerson" },	// the same
		{ "Shaders.ShadowsInteriors.Main", "PlayerShadowThirdPerson" },	// the same
		{ "Shaders.Snow.Main", "Flakes" },
	};
	for (const auto& d : dead) if (!strcmp(d[0], section) && !strcmp(d[1], key)) return true;
	return false;
}

static const char* InactiveReason(const char* section, const char* key) {
	static char reason[256];
	const std::string sec = section;
	auto is = [key](std::initializer_list<const char*> keys) {
		for (const char* k : keys) if (!strcmp(k, key)) return true;
		return false;
	};

	if (IsDeadSetting(section, key)) return "Does nothing: nothing in the mod reads this setting (kept for old settings files).";

	// Sun shadows (ShadowsExteriorEffect::UpdateSettings): Quality 0-3 set these themselves; only 4 (Custom) reads them.
	const bool shadowMaps = sec == "Shaders.ShadowsExteriors.ShadowMaps";
	const bool forms = sec.rfind("Shaders.ShadowsExteriors.Forms", 0) == 0 && sec != "Shaders.ShadowsExteriors.FormsOrtho";
	if (shadowMaps || forms) {
		const int quality = (int)NumericSetting("Shaders.ShadowsExteriors.Main", "Quality");
		const bool custom = quality < 0 || quality > 3;
		if (!custom && (forms || is({ "Distance", "CascadeLambda", "SmoothFilter", "LimitFrequency", "CascadeResolution" }))) {
			sprintf_s(reason, "Not used now: shadow Quality (Shadows Exteriors > Main) is %d, which sets this itself. Only Quality 4 (Custom) reads it.", quality);
			return reason;
		}
	}
	if (sec == "Shaders.ShadowsExteriors.SunSmoothing" && is({ "CrossFade" }) && NumericSetting("Shaders.ShadowsExteriors.Main", "ForwardShadows") == 0.0f)
		return "Only used with ForwardShadows on (Shadows Exteriors > Main).";

	// Lamp shadows indoors (PointShadowForward, ShadowsExteriorEffect::UpdateSettings, ShaderManager's darkening pass).
	if (sec == "Shaders.ShadowsInteriors.Forward" && !is({ "Enabled", "ScopeFix" })) {
		if (NumericSetting(section, "Enabled") == 0.0f)
			return "Only used with Forward Enabled on (the first switch in this section); with it off the old screen-space lamp shadows are used.";
		if (is({ "Quality", "ContactHardening", "ContactHardeningStrength", "ContactHardeningMinSoftness", "LampSize", "MaxSoftness", "InvisibleLightSize" }) && NumericSetting(section, "Softness") <= 0.0f)
			return "Only used when Softness is above 0: sharp lamp shadows (Softness 0) take a single lookup.";
		if (is({ "ContactHardeningStrength", "ContactHardeningMinSoftness" }) && NumericSetting(section, "ContactHardening") == 0.0f)
			return "Only used with ContactHardening on.";
		if (is({ "EvenTaps", "MagnifiedSoftness", "SmoothEdges" }) && NumericSetting(section, "TexelFilter") != 0.0f)
			return "Not used with TexelFilter on (it filters every lamp shadow edge the way the sun's shadows are; restart after changing TexelFilter).";
		if (is({ "TexelFilter" }) && (NumericSetting(section, "Softness") <= 0.0f || NumericSetting(section, "Quality") < 1.0f))
			return "Only used with Softness above 0 and Quality 1 or more (sharp lamp shadows take a single lookup).";
		if (is({ "EvenTaps" }) && NumericSetting(section, "Quality") < 2.0f)
			return "Only changes Quality 2 (High) and 3 (Ultra): Low and Medium have no extra lookups.";
		if (is({ "MagnifiedSoftness" }) && (NumericSetting(section, "Softness") <= 0.0f || NumericSetting(section, "Quality") < 1.0f))
			return "Only used with Softness above 0 and Quality 1 or more (sharp lamp shadows take a single lookup).";
		if (is({ "ContactHardening", "ContactHardeningStrength", "ContactHardeningMinSoftness" }) && NumericSetting(section, "LampSize") > 0.0f)
			return "Not used while LampSize is above 0 (LampSize decides how soft each shadow is).";
		if (is({ "MaxSoftness" }) && NumericSetting(section, "LampSize") <= 0.0f && NumericSetting(section, "InvisibleLightSize") <= 0.0f)
			return "Only used when LampSize or InvisibleLightSize is above 0.";
		if (is({ "InvisibleLightSize" }) && NumericSetting(section, "InvisibleLightShadowStrength") <= 0.0f)
			return "Not used while InvisibleLightShadowStrength is 0 (invisible lights then cast no shadow at all).";
		if (is({ "InvisibleShadowFromLamp" }) && NumericSetting(section, "InvisibleLightShadowStrength") <= 0.0f)
			return "Not used while InvisibleLightShadowStrength is 0 (invisible lights then cast no shadow at all).";
		if (is({ "LampModelDistance", "LampModelOwnDistance" }) && NumericSetting(section, "LampModelShadows") != 0.0f)
			return "Only used with LampModelShadows off.";
		if (is({ "FixtureDistance" }) && NumericSetting(section, "InvisibleLightShadowStrength") >= 1.0f)
			return "Only used when InvisibleLightShadowStrength is below 1.";
		if (is({ "DarkeningBlur", "MaskHalfRes" }) && NumericSetting(section, "KeepDarkening") == 0.0f)
			return "Only used with KeepDarkening on (with it off the room darkening pass is not drawn).";
	}
	if (sec == "Shaders.ShadowsInteriors.Main" && is({ "ContactLength", "ContactThickness", "ContactDistance" }) && NumericSetting(section, "ContactShadows") <= 0.0f)
		return "Only used when ContactShadows is above 0.";
	// UNOFFICIAL 2026-10-19 Reflections GridFallback (ReflectionsEffect::GridStep): reads Ambient Light's light grid, made only while it runs with GridBounce.
	if (sec == "Shaders.Reflections.Main" && is({ "GridFallback", "GridBrightness" }) &&
		(NumericSetting("Shaders.AmbientLight.Status", "Enabled") == 0.0f || NumericSetting("Shaders.AmbientLight.Main", "GridBounce") <= 0.0f))
		return "Only used while Ambient Light is on with GridBounce above 0 (Ambient Light > Main): it reads the light grid that makes.";
	if (sec == "Shaders.Reflections.Main" && is({ "GridBrightness" }) && NumericSetting(section, "GridFallback") == 0.0f)
		return "Only used with GridFallback on.";
	// UNOFFICIAL 2026-10-20 MaterialAware (core/ReflMaterial.h): the material comes from the _rmaos maps.
	if (sec == "Shaders.Reflections.Main" && is({ "MaterialAware", "MaterialReflect", "MaterialMaxRoughness" }) && NumericSetting("Shaders.PBR.Rmaos", "Enabled") == 0.0f)
		return "Only used with the _rmaos maps on ([Shaders.PBR.Rmaos] Enabled): the materials come from them.";
	if (sec == "Shaders.Reflections.Main" && is({ "MaterialReflect", "MaterialMaxRoughness" }) && NumericSetting(section, "MaterialAware") != 1.0f)
		return "Only used with MaterialAware 1.";
	// UNOFFICIAL 2026-10-21 cloth indoors (core/ClothWind.h BeginFrame): the indoor set is read only while its switch is on; indoors
	// there is no weather, so the weather's wind is never used there.
	if (sec == "Shaders.Cloth.Interiors" && !is({ "Enabled" }) && NumericSetting(section, "Enabled") == 0.0f)
		return "Only used with Enabled on (the first switch in this section). With it off cloth stays still indoors "
			"(worn coats still follow you there: Cloth > Main MotionIndoors).";
	if (sec == "Shaders.Cloth.Main" && is({ "CalmFactor" }) && NumericSetting(section, "UseGameWind") == 0.0f)
		return "Only used with UseGameWind on.";
	if (sec == "Shaders.Cloth.Main" && is({ "WindDirX", "WindDirY", "WindDirZ" }) && NumericSetting(section, "UseGameWind") != 0.0f &&
		NumericSetting("Shaders.Cloth.Interiors", "Enabled") == 0.0f)
		return "Not used now: UseGameWind takes the direction from the weather. Used with UseGameWind off, and indoors "
			"with Cloth > Interiors on (no weather there).";
	return nullptr;
}

// 3. Test switches (the "New in this patch" tab's Diagnostics rule) and settings nothing reads: hidden unless ticked.
static bool NewIsDiagnostic(const std::string& key, const std::string& description, const std::vector<std::string>& extra, const std::string& section);

static bool IsDevOnlySetting(const std::string& section, const std::string& key);

// Shown only with Developer options (2026-10-25: also the [Main.Menu.Developer] DevOnly list).
static bool IsTestSetting(const SettingManager::Configuration::ConfigNode& node) {
	static const std::vector<std::string> none;
	return IsDeadSetting(node.Section, node.Key) || NewIsDiagnostic(node.Key, node.Description, none, node.Section) ||
		IsDevOnlySetting(node.Section, node.Key);
}

// UNOFFICIAL 2026-10-25 menu tidy: whole-number settings that are plain 0/1 switches are drawn as a checkbox (it writes 0 or 1, the
// type in the file stays). The ones listed here, and any whose description starts with "[0-1]"; a value other than 0 or 1 keeps the
// number box.
static bool IsOnOffInteger(const SettingManager::Configuration::ConfigNode& node) {
	static const char* const onOff[][2] = {
		{ "Main.Main.Performance", "ArrivalSampler" }, { "Main.Main.Performance", "IOWorker2" },
		{ "Main.Main.Performance", "FogReferenceTest" }, { "Main.Main.Performance", "LampMaskScissorCheck" },
		{ "Main.Main.Performance", "CubeFaceViewCullCheck" },
		{ "Shaders.Grass.Wind", "Log" }, { "Shaders.Plants.Wind", "Log" },
		{ "Shaders.DitherBuster.Main", "DebugView" }, { "Shaders.PBR.Metal", "DebugView" },
		{ "Shaders.ShadowsExteriors.SunSmoothing", "SmoothSun" }, { "Shaders.ShadowsExteriors.SunSmoothing", "QuantizeSun" },
		{ "Shaders.CloudShadows.Main", "HazardLog" }, { "Shaders.CloudShadows.Main", "FlickerWatch" },
		{ "Shaders.HandBlood.Main", "DebugView" }, { "Shaders.WetWorld.Main", "HudDebug" },
	};
	for (const auto& o : onOff) if (!strcmp(o[0], node.Section) && !strcmp(o[1], node.Key)) return true;
	return node.Description.compare(0, 5, "[0-1]") == 0;
}

// 2026-11-04 (user: "remove unofficial from all the different tooltips since we are no longer just a regular unofficial fork"):
// the text a description shows in the menu, without its "UNOFFICIAL" tags (and the date right after one). The settings files and
// the descriptions keep them: the New-in-patch list reads those dates.
static std::string ShownDescription(const std::string& text) {
	static const char kTag[] = "UNOFFICIAL";
	const size_t tagLength = sizeof(kTag) - 1;
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size()) {
		if (text.compare(i, tagLength, kTag) != 0) { out += text[i++]; continue; }
		i += tagLength;
		size_t j = i;
		while (j < text.size() && text[j] == ' ') j++;
		if (text.compare(j, 3, "(20") == 0) {	// its date: " (2026-10-31)"
			const size_t close = text.find(')', j);
			if (close != std::string::npos && close - j <= 24) i = close + 1;
		}
		while (i < text.size() && (text[i] == ',' || text[i] == ':' || text[i] == ' ')) i++;
		size_t end = out.size();
		while (end > 0 && out[end - 1] == ' ') end--;
		const bool sentenceStart = end == 0 || strchr(".!?\n]", out[end - 1]) != nullptr;
		if (sentenceStart && i < text.size()) out += (char)toupper((unsigned char)text[i++]);
	}
	return out;
}

// A setting's hover note: why it is greyed (if it is), then its description from the settings file. When the row's name was
// cut short, the full name comes first. normalText is the style's text colour (the row itself may be drawn red or grey).
static void SettingTooltip(const SettingManager::Configuration::ConfigNode& node, const char* why, const ImVec4& normalText, bool nameCut) {
	const bool described = !node.Description.empty();
	if (!described && !why && !nameCut) return;
	ImGui::BeginTooltip();
	ImGui::PushStyleColor(ImGuiCol_Text, normalText);
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	if (nameCut) ImGui::TextDisabled("%s", node.Key);
	if (why) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", why);
	if (why && described) ImGui::Separator();
	if (described) ImGui::TextUnformatted(ShownDescription(node.Description).c_str());
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::EndTooltip();
}

// ---- Key names ---------------------------------------------------------------------------------------------------------------
// DirectInput scan codes and the names the menu shows for them (the menu key's hint, the KeyEnable reference). Rows of keys
// that sit next to each other on the keyboard are spelled out once; the rest are listed one by one. Sorted by code.

struct KeyName { int code; std::string name; };

static const std::vector<KeyName>& KeyNames() {
	static std::vector<KeyName> names;
	if (!names.empty()) return names;
	auto run = [](int firstCode, const char* letters) {
		for (int i = 0; letters[i]; i++) names.push_back({ firstCode + i, std::string(1, letters[i]) });
	};
	run(0x02, "1234567890");
	run(0x10, "QWERTYUIOP");
	run(0x1E, "ASDFGHJKL");
	run(0x2C, "ZXCVBNM");
	for (int f = 1; f <= 10; f++) names.push_back({ 0x3A + f, "F" + std::to_string(f) });
	static const struct { int code; const char* name; } kSingles[] = {
		{ 0x01, "Escape" },
		{ 0x0C, "Minus (-)" },
		{ 0x0D, "Equals (=)" },
		{ 0x0E, "Backspace" },
		{ 0x0F, "Tab" },
		{ 0x1A, "[ Left Bracket" },
		{ 0x1B, "] Right Bracket" },
		{ 0x1C, "Enter" },
		{ 0x1D, "Left Ctrl" },
		{ 0x27, "Semicolon (;)" },
		{ 0x28, "Apostrophe (')" },
		{ 0x29, "Grave (`)" },
		{ 0x2A, "Left Shift" },
		{ 0x2B, "Backslash (\\)" },
		{ 0x33, "Comma (,)" },
		{ 0x34, "Period (.)" },
		{ 0x35, "Slash (/)" },
		{ 0x36, "Right Shift" },
		{ 0x37, "Numpad *" },
		{ 0x38, "Left Alt" },
		{ 0x39, "Space" },
		{ 0x3A, "Caps Lock" },
		{ 0x45, "Num Lock" },
		{ 0x46, "Scroll Lock" },
		{ 0x47, "Numpad 7" },
		{ 0x48, "Numpad 8" },
		{ 0x49, "Numpad 9" },
		{ 0x4A, "Numpad -" },
		{ 0x4B, "Numpad 4" },
		{ 0x4C, "Numpad 5" },
		{ 0x4D, "Numpad 6" },
		{ 0x4E, "Numpad +" },
		{ 0x4F, "Numpad 1" },
		{ 0x50, "Numpad 2" },
		{ 0x51, "Numpad 3" },
		{ 0x52, "Numpad 0" },
		{ 0x53, "Numpad ." },
		{ 0x57, "F11" },
		{ 0x58, "F12" },
		{ 0x9C, "Numpad Enter" },
		{ 0x9D, "Right Ctrl" },
		{ 0xB5, "Numpad /" },
		{ 0xB7, "Print Screen" },
		{ 0xB8, "Right Alt" },
		{ 0xC5, "Pause" },
		{ 0xC7, "Home" },
		{ 0xC8, "Up Arrow" },
		{ 0xC9, "Page Up" },
		{ 0xCB, "Left Arrow" },
		{ 0xCD, "Right Arrow" },
		{ 0xCF, "End" },
		{ 0xD0, "Down Arrow" },
		{ 0xD1, "Page Down" },
		{ 0xD2, "Insert" },
		{ 0xD3, "Delete" },
	};
	for (const auto& single : kSingles) names.push_back({ single.code, single.name });
	std::sort(names.begin(), names.end(), [](const KeyName& a, const KeyName& b) { return a.code < b.code; });
	return names;
}

static const char* DikToName(BYTE dik) {
	for (const KeyName& key : KeyNames())
		if (key.code == (int)dik) return key.name.c_str();
	return "Unknown";
}

// The KeyEnable "(?)" popup: every scan code with its key, in a scrolling two-column table.
static void ScanCodePopup() {
	if (!ImGui::BeginPopup("DIKReference")) return;
	ImGui::TextUnformatted("DirectInput Scancodes");
	ImGui::Separator();
	const ImGuiTableFlags look = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
	if (ImGui::BeginTable("diktbl", 2, look, ImVec2(220.0f, 320.0f))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		for (const char* heading : { "Code", "Key" }) ImGui::TableSetupColumn(heading);
		ImGui::TableHeadersRow();
		for (const KeyName& key : KeyNames()) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("0x%02X (%d)", key.code, key.code);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(key.name.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::EndPopup();
}

// ---- Colour triples ----------------------------------------------------------------------------------------------------------
// Three settings named <prefix>R, <prefix>G and <prefix>B are one colour: a hue wheel for the colour (largest channel 1) and an
// intensity that scales it. The wheel keeps its own copy while the menu is open: reading the three values back every frame would
// pin the wheel's dot to the edge and reset the intensity.

struct ColourWheel { float hue[3]; float intensity; };

static ColourWheel& WheelFor(const SettingManager::Configuration::ConfigNode* rgb[3], const std::string& prefix) {
	static std::unordered_map<std::string, ColourWheel> wheels;
	if (gMenu.resetWheels) {
		wheels.clear();
		gMenu.resetWheels = false;
	}
	const std::string id = std::string(rgb[0]->Section) + "." + prefix;
	const auto known = wheels.find(id);
	if (known != wheels.end()) return known->second;

	float value[3];
	for (int c = 0; c < 3; c++) value[c] = (float)atof(rgb[c]->Value);
	const float brightest = (std::max)({ value[0], value[1], value[2] });
	ColourWheel fresh = { { 0.0f, 0.0f, 0.0f }, 1.0f };
	if (brightest > 0.0f) {
		fresh.intensity = brightest;
		for (int c = 0; c < 3; c++) fresh.hue[c] = value[c] / brightest;
	}
	return wheels[id] = fresh;
}

// The value a setting had when it was first drawn since the menu opened (the ~ buttons go back to it).
static std::string& SessionValue(const char* section, const char* key, const char* now) {
	auto& values = gMenu.sessionValues[section];
	auto found = values.find(key);
	if (found == values.end()) found = values.emplace(key, now).first;
	return found->second;
}

// A drag box that also takes the +/- keys while the mouse is over it. Returns whether the value changed.
static bool DragWithStepKeys(const char* id, float* value, float speed, const char* format) {
	bool changed = ImGui::DragFloat(id, value, speed, 0.0f, 0.0f, format);
	if (ImGui::IsItemHovered() && (gMenu.plusKey || gMenu.minusKey)) {
		*value += gMenu.plusKey ? gMenu.step : -gMenu.step;
		changed = true;
	}
	return changed;
}

static void RenderColorTriple(SettingManager::Configuration::ConfigNode& nodeR, SettingManager::Configuration::ConfigNode& nodeG,
	SettingManager::Configuration::ConfigNode& nodeB, const std::string& prefix, const char* shownTitle = nullptr)	// shownTitle: a simple view's name
{
	SettingManager::Configuration::ConfigNode* rgb[3] = { &nodeR, &nodeG, &nodeB };
	ColourWheel& wheel = WheelFor(const_cast<const SettingManager::Configuration::ConfigNode**>(rgb), prefix);

	std::string title = shownTitle ? shownTitle : prefix;	// "FogColor_" reads "FogColor"
	while (!title.empty() && title.back() == '_') title.pop_back();

	std::string* atOpen[3];
	bool moved = false;
	for (int c = 0; c < 3; c++) {
		atOpen[c] = &SessionValue(rgb[c]->Section, rgb[c]->Key, rgb[c]->Value);
		moved |= *atOpen[c] != rgb[c]->Value;
	}

	ImGui::PushID(prefix.c_str());
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

	// Title line: the name in the skin's heading colour, then ~ (back to the values at menu open) while they differ
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(title.c_str(), 1.15f);
	ImGui::PopStyleColor();
	const bool titleHovered = ImGui::IsItemHovered();
	if (moved) {
		ImGui::SameLine();
		if (ImGui::SmallButton("~")) {
			for (int c = 0; c < 3; c++) TheSettingManager->SetSettingS(rgb[c]->Section, rgb[c]->Key, atOpen[c]->c_str());
			TheSettingManager->LoadSettings();
			gMenu.resetWheels = true;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Revert to value at session start");
	}

	bool edited = false;
	// UNOFFICIAL 2026-10-25: the wheel follows the text size instead of filling a wide page
	ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 22.0f));
	edited |= ImGui::ColorPicker3("##col", wheel.hue, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview);

	// The three channels on one row, each as its stored value (colour times intensity)
	const float rowW = MenuWidgets::RowBegin("Red / Green / Blue", 0.0f);
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	static const char* const kChannelIds[] = { "##r", "##g", "##b" };
	static const char* const kChannelFormats[] = { "R %.4f", "G %.4f", "B %.4f" };
	for (int c = 0; c < 3; c++) {
		if (c) ImGui::SameLine();
		ImGui::SetNextItemWidth((rowW - gap * 2.0f) / 3.0f);
		float stored = wheel.hue[c] * wheel.intensity;
		if (DragWithStepKeys(kChannelIds[c], &stored, 0.001f, kChannelFormats[c]) && wheel.intensity > 0.0f) {
			wheel.hue[c] = stored / wheel.intensity;
			edited = true;
		}
	}

	ImGui::SetNextItemWidth(MenuWidgets::RowBegin("Intensity", 0.0f));
	edited |= DragWithStepKeys("##intensity", &wheel.intensity, 0.01f, "%.4f");

	if (edited) {
		for (int c = 0; c < 3; c++) TheSettingManager->SetSetting(rgb[c]->Section, rgb[c]->Key, wheel.hue[c] * wheel.intensity);
		TheSettingManager->LoadSettings();
	}
	if (titleHovered && !nodeR.Description.empty()) WrappedTooltip(ShownDescription(nodeR.Description).c_str());
	ImGui::PopID();
}

// ---- Settings drawn as a list of choices -------------------------------------------------------------------------------------
// Whole numbers that pick one of a few modes get a dropdown of names. Keyed by "Section.Key": the key alone repeats ("Mode" is a
// different thing for Cinema and DepthOfField).
//
// Cinema and DepthOfField Mode: the C++ also takes 3 and 4 (Cinema.cpp, DepthOfField.cpp), once Oblivion's persuasion tests
// (2026-11-05: removed; New Vegas has no persuasion menu). 3 always switches the effect off ("Disabled"), 4 is Cinema's 2 and
// DepthOfField's 3: only 3 is offered.

static const char* const kToneMappers[] = {
	"0 - None (vanilla)",
	"1 - VTLottes",
	"2 - Lottes (tuned)",
	"3 - Reinhard",
	"4 - Reinhard Jodie",
	"5 - ACES Filmic",
	"6 - ACES Fitted",
	"7 - Uncharted 2",
	"8 - Uchimura (GT)",
	"9 - AGX",
	"10 - DICE",
	"11 - Perceptual (Polyphony)",
};
static const char* const kWhenShown[] = { "0 - Always", "1 - Not during dialog", "2 - Only during dialog", "3 - Disabled" };
static const char* const kExposureModes[] = { "0 - Classic (screen average)", "1 - Eye adaptation (whole view, white balance)" };	// UNOFFICIAL [Shaders.Exposure.Main] Mode
static const char* const kShadowQualities[] = { "0 - Low", "1 - Medium", "2 - High", "3 - Full", "4 - Custom" };
static const char* const kShadowFormats[] = { "0 - 16 bit", "1 - 32 bit" };
static const char* const kCascadeSizes[] = { "0 - 1024", "1 - 1536", "2 - 2048", "3 - 2560", "4 - 3072", "5 - 3584", "6 - 4096" };
static const char* const kOrthoSizes[] = { "0 - 128", "1 - 256", "2 - 512", "3 - 1024", "4 - 2048" };
static const char* const kSmaaEdges[] = { "0 - Luma", "1 - Color", "2 - Depth", "3 - Luma + Depth" };
static const char* const kOffOnRestart[] = { "0 - Off", "1 - On (needs a restart)" };	// fixes-a #11
static const char* const kFsrVersions[] = { "0 - FSR 2.2.1", "1 - FSR 3.1" };	// UNOFFICIAL 2026-10-28 (core/UpscalerFsr31.cpp)
static const char* const kSmokeTests[] = {
	"0 - Normal", "1 - Smoke in green", "2 - No muzzle flash light", "3 - Smoke not drawn", "4 - Nothing drawn at all",
};

struct Choices { const char* const* names; int count; };

static const Choices* ChoicesFor(const char* section, const char* key) {
	#define NAMES(list) Choices{ list, (int)(sizeof(list) / sizeof(*list)) }
	static const std::unordered_map<std::string, Choices> kBySetting = {
		{ "Shaders.Tonemapping.Main.TonemappingMode", NAMES(kToneMappers) },
		{ "Shaders.Tonemapping.Interiors.TonemappingMode", NAMES(kToneMappers) },
		{ "Shaders.Exposure.Main.Mode", NAMES(kExposureModes) },
		{ "Shaders.Cinema.Main.Mode", NAMES(kWhenShown) },
		{ "Shaders.DepthOfField.FirstPersonView.Mode", NAMES(kWhenShown) },
		{ "Shaders.DepthOfField.ThirdPersonView.Mode", NAMES(kWhenShown) },
		{ "Shaders.DepthOfField.VanityView.Mode", NAMES(kWhenShown) },
		{ "Shaders.ShadowsExteriors.Main.Quality", NAMES(kShadowQualities) },
		{ "Shaders.ShadowsExteriors.ShadowMaps.Format", NAMES(kShadowFormats) },
		{ "Shaders.ShadowsExteriors.ShadowMaps.CascadeResolution", NAMES(kCascadeSizes) },
		{ "Shaders.ShadowsExteriors.Ortho.Resolution", NAMES(kOrthoSizes) },
		{ "Shaders.SMAA.Main.EdgeDetection", NAMES(kSmaaEdges) },
		{ "Main.GunFX.Main.VolumetricSmokeTest", NAMES(kSmokeTests) },
		{ "Shaders.PBR.Main.SmithCorrelated", NAMES(kOffOnRestart) },
		{ "Shaders.TAA.FSR.Version", NAMES(kFsrVersions) },
	};
	#undef NAMES
	const auto found = kBySetting.find(std::string(section) + "." + key);
	return found == kBySetting.end() ? nullptr : &found->second;
}

// A dropdown over names (the current one ticked and focused when it opens). Returns the index picked this frame, or -1.
static int NamePicker(const char* id, const char* shown, const char* const* names, int count, int current) {
	if (!ImGui::BeginCombo(id, shown)) return -1;
	int picked = -1;
	for (int i = 0; i < count; i++) {
		if (ImGui::Selectable(names[i], i == current)) picked = i;
		if (i == current) ImGui::SetItemDefaultFocus();
	}
	ImGui::EndCombo();
	return picked;
}

// UNOFFICIAL 2026-10-27 (user: "oh it's called displacement instead of tessellation" / "ok do that"): a setting's menu name where
// users look for another word; the key in the settings files stays. The ## part keeps ImGui's ID the key.
// ---- Conflict warnings ------------------------------------------------------------------------------------------------------
// 2026-11-05 (user: "add some sort of warning if you're going to toggle an effect that silently disables / conflicts with another
// one"). Turning on a setting or an effect listed here, while the other side of the conflict is on, first opens a "Heads up" box that
// says in plain words what stops working; the change waits for "Turn it on anyway" (Cancel leaves everything as it was). The list is
// the conflicts found in the code on 2026-11-05 (AmbientLight's takeover, ComputeGI over RT GI, path tracing, ...), not every partial
// overlap. An effect's own switch is "Shaders.<Name>.Status" / "Enabled" (or its Main page's Enabled for the effects switched there).
struct ConflictRule {
	const char* Section;
	const char* Key;
	const char* Value;		// the value that triggers it: "true", or a whole number
	bool (*Applies)();		// the other side is on now (warn only then); nullptr: always
	const char* Text;
};
static bool SettingOn(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key) != 0; }
static bool EffectOn(const char* name) { return TheSettingManager->GetMenuShaderEnabled(name); }
static bool AnyRtOn() {
	return SettingOn("Shaders.ShadowsExteriors.RT", "Enabled") || SettingOn("Shaders.ShadowsInteriors.RT", "Enabled") ||
		SettingOn("Shaders.AmbientOcclusion.RT", "Enabled") || SettingOn("Shaders.Reflections.RT", "Enabled");
}
static bool CloudsCastShadows() { return EffectOn("VolumetricClouds") && TheSettingManager->GetSettingF("Shaders.VolumetricClouds.Main", "Shadows") > 0.0f; }

static const ConflictRule kConflictRules[] = {
	{ "Shaders.AmbientLight.Main", "ComputeGI", "true",
		[]() { return SettingOn("Shaders.AmbientOcclusion.RT", "GI") && SettingOn("Shaders.AmbientOcclusion.RT", "Enabled"); },
		"Screen-space bounce light replaces your ray-traced bounce light: the ray-traced one does nothing while this is on." },
	{ "Shaders.AmbientOcclusion.RT", "GI", "true",
		[]() { return EffectOn("AmbientLight") && SettingOn("Shaders.AmbientLight.Main", "ComputeGI"); },
		"Screen-space bounce light (on the Ambient light page) is on and replaces this: ray-traced bounce light does nothing until you turn that off." },
	{ "Shaders.AmbientOcclusion.Status", "Enabled", "true", []() { return EffectOn("AmbientLight"); },
		"Ambient light already does ambient occlusion and switches this effect off while it runs: this does nothing unless Ambient light is off." },
	{ "Shaders.BounceLight.Status", "Enabled", "true", []() { return EffectOn("AmbientLight"); },
		"Ambient light already does bounce light and switches this effect off while it runs: this does nothing unless Ambient light is off." },
	{ "Shaders.AmbientLight.Status", "Enabled", "true", []() { return EffectOn("AmbientOcclusion") || EffectOn("BounceLight"); },
		"Ambient light does ambient occlusion and bounce light itself: the separate Ambient occlusion and Bounce light effects switch off while it runs." },
	{ "Main.Main.RayTracing", "PTRealtime", "true", nullptr,
		"Path tracing lights the whole picture itself: Ambient light, ambient occlusion and specular highlights switch off, and the path "
		"tracer's own settings are overridden. Very heavy: for top graphics cards." },
	{ "Main.Main.RayTracing", "PTReplace", "true", nullptr,
		"The path-traced picture replaces the game's: Ambient light, ambient occlusion and specular highlights switch off while it runs." },
	{ "Shaders.ShadowsExteriors.RT", "Mode", "1", nullptr,
		"Replace: near you only ray-traced sun shadows are used. Leaves, fences and people may lose their sun shadows up close." },
	{ "Shaders.TAA.DLSS", "RayReconstruction", "true", AnyRtOn,
		"Ray Reconstruction takes over from Vegas Shaders' own ray-tracing smoothing: those smoothing settings do nothing while it runs." },
	{ "Shaders.CloudShadows.Main", "Enabled", "true", CloudsCastShadows,
		"Volumetric clouds cast their own shadows and replace these flat cloud shadows: this does nothing while they do." },
	{ "Shaders.VolumetricClouds.Status", "Enabled", "true",
		[]() { return SettingOn("Shaders.CloudShadows.Main", "Enabled") && TheSettingManager->GetSettingF("Shaders.VolumetricClouds.Main", "Shadows") > 0.0f; },
		"Volumetric clouds cast their own shadows and replace the flat cloud shadows while they are on." },
	{ "Shaders.VolumetricFog.Main", "LegacyFog", "true", nullptr,
		"Legacy fog (for old presets) ignores Classic look, the height layer and the current fog's settings." },
	{ "Shaders.Terrain.Blending", "Enabled", "true", []() { return SettingOn("Shaders.Grass.GPUGrass", "Enabled"); },
		"Terrain blending turns off GPU grass: the GPU grass will not show while this is on." },
	{ "Shaders.Grass.GPUGrass", "Enabled", "true", []() { return SettingOn("Shaders.Terrain.Blending", "Enabled"); },
		"GPU grass will not show while Terrain blending is on (blending turns grass-first drawing off)." },
	{ "Main.Main.Misc", "PresetManagerEnabled", "true", nullptr,
		"Location presets will change your settings by themselves as you move between places." },
	{ "Main.Main.ReducedQuality", "FXAA", "true", []() { return EffectOn("SMAA"); },
		"SMAA is on too: both smooth the edges, which costs more and softens the picture. Turn one of them off." },
	{ "Shaders.SMAA.Status", "Enabled", "true", []() { return SettingOn("Main.Main.ReducedQuality", "FXAA"); },
		"FXAA is on too: both smooth the edges, which costs more and softens the picture. Turn one of them off." },
};

static bool SameValue(const char* a, const std::string& b) {
	auto norm = [](const std::string& v) { return v == "true" ? std::string("1") : v == "false" ? std::string("0") : v; };
	return norm(a) == norm(b);
}

static const ConflictRule* FindConflict(const char* section, const char* key, const std::string& value) {
	for (const ConflictRule& r : kConflictRules)
		if (!strcmp(r.Section, section) && !strcmp(r.Key, key) && SameValue(r.Value, value) && (!r.Applies || r.Applies())) return &r;
	return nullptr;
}

// A change waiting for the Heads up box: the settings to write, or an effect to switch.
struct PendingChange {
	bool Request = false;	// open the box next frame (in the main window's ID scope, where it is drawn)
	std::string Text;
	std::vector<std::array<std::string, 3>> Sets;	// section, key, value
	std::string Shader;
};
static PendingChange s_pending;

// Whether a change may go ahead now; false: the Heads up box was asked for and the change waits for it.
static bool ConfirmChanges(const std::vector<std::array<std::string, 3>>& sets) {
	for (const auto& s : sets)
		if (const ConflictRule* rule = FindConflict(s[0].c_str(), s[1].c_str(), s[2])) {
			s_pending = { true, rule->Text, sets, std::string() };
			return false;
		}
	return true;
}
static bool ConfirmChange(const char* section, const char* key, const std::string& value) { return ConfirmChanges({ { section, key, value } }); }

// An effect's own switch being turned on ("Shaders.<Name>.Status" Enabled, or its Main page's Enabled).
static bool ConfirmEffectOn(const std::string& shader) {
	for (const char* page : { ".Status", ".Main" })
		if (const ConflictRule* rule = FindConflict(("Shaders." + shader + page).c_str(), "Enabled", "true")) {
			s_pending = { true, rule->Text, {}, shader };
			return false;
		}
	return true;
}

static void DrawConflictPopup() {
	if (s_pending.Request) {
		ImGui::OpenPopup("Heads up###conflict");
		s_pending.Request = false;
	}
	if (!ImGui::BeginPopupModal("Heads up###conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
	ImGui::TextUnformatted(s_pending.Text.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	if (ImGui::Button("Turn it on anyway")) {
		for (const auto& s : s_pending.Sets) TheSettingManager->SetSettingS(s[0].c_str(), s[1].c_str(), s[2].c_str());
		if (!s_pending.Shader.empty()) TheShaderManager->SwitchShaderStatus(s_pending.Shader.c_str());
		TheSettingManager->LoadSettings();
		s_pending = PendingChange();
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		s_pending = PendingChange();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ---- Simple views ---------------------------------------------------------------------------------------------------------
// 2026-11-05 (docs/next/SETTINGS-GAME-MENU-PLAN.md; user: "simplify the display names of the keys so that they are in a form of language
// the average gamer will understand and merge some together by proxy on the user side unless someone toggles advanced mode"). A page
// listed here shows, while the top row's Advanced switch is off, only its rows below: settings under a plain name, and choices that
// set several settings at once (shown as Custom when the settings match none of them). Advanced on (or Developer options): the page
// as before, every setting under its own name. The settings themselves, their sections and the saved file do not change. A page not
// listed is drawn as before in both modes, until it gets its simple view.
struct SimpleRow {
	const char* Section;
	const char* Key;		// a setting shown under Label, or nullptr: a choice
	const char* Label;
	const char* Options;	// a choice's options: "Name: Key=Value Key=Value | Name: ...", or nullptr
	const char* Tip;		// the tooltip while Advanced is off (plain words; Advanced shows the setting's own description)
};
static const SimpleRow kSimpleRows[] = {
	// Sun shadows
	{ "Shaders.ShadowsExteriors.RT", "Enabled", "Ray-traced sun shadows", nullptr, "Sun shadows traced with rays: exact contact shadows and no missing shadows far away. Needs an RTX-class GPU and the DXVK fork." },
	{ "Shaders.ShadowsExteriors.RT", nullptr, "Quality",
		"Performance: HalfResolution=true Penumbra=false | Balanced: HalfResolution=false Penumbra=false | High (soft, realistic edges): HalfResolution=false Penumbra=true",
		"Performance: traced at half resolution (cheaper, softer). Balanced: full resolution. High: edges soften with distance from what casts "
		"them, as real sun shadows do (the most expensive)." },
	{ "Shaders.ShadowsExteriors.RT", nullptr, "Near you",
		"Add to the shadow maps: Mode=0 | Replace the shadow maps: Mode=1",
		"Add: the traced shadows fill in what the game's shadow maps miss. Replace: near you only the traced shadows are used (sharper, exact)." },
	{ "Shaders.ShadowsExteriors.RT", "Strength", "Shadow strength", nullptr, "How dark the traced sun shadows are." },
	// Lamp shadows
	{ "Shaders.ShadowsInteriors.RT", "Enabled", "Ray-traced lamp shadows", nullptr, "Lamp shadows traced with rays instead of the old shadow maps: sharper and no leaks. Takes effect after a restart." },
	{ "Shaders.ShadowsInteriors.RT", "Live", "Use them now (off: the old shadows, to compare)", nullptr, "Switch between traced and old lamp shadows at once, to compare." },
	{ "Shaders.ShadowsInteriors.RT", nullptr, "Shadow edges",
		"Sharp: LampRadius=0.0 Rays=1 | Soft: LampRadius=4.0 Rays=1 | Soft, less grainy: LampRadius=4.0 Rays=4",
		"Sharp: hard edges, the cheapest. Soft: lamps have a size, so edges blur with distance (a little grain that the anti-aliasing smooths). "
		"Soft, less grainy: four rays per lamp (more expensive)." },
	{ "Shaders.ShadowsInteriors.RT", "Exteriors", "Also outdoors at night", nullptr, "Street lamps outdoors at night cast traced shadows too. Takes effect after a restart." },
	// Ambient occlusion and bounce light
	{ "Shaders.AmbientOcclusion.RT", "Enabled", "Ray-traced ambient occlusion", nullptr, "Soft shadows in corners and under objects, worked out with rays instead of from the screen: no halos, nothing vanishes at the screen edge." },
	{ "Shaders.AmbientOcclusion.RT", "GI", "Ray-traced bounce light", nullptr, "Light bounces off lit surfaces onto nearby ones, tinted by their colour. Turn off Screen-space bounce light, or it replaces this." },
	{ "Shaders.AmbientOcclusion.RT", nullptr, "Bounce light quality",
		"Low: GIReuse=8 GISpatial=0 RadianceCache=false CacheExteriors=false | Medium: GIReuse=8 GISpatial=2 RadianceCache=false CacheExteriors=false | "
		"High: GIReuse=8 GISpatial=2 RadianceCache=true CacheExteriors=false | Ultra: GIReuse=8 GISpatial=2 RadianceCache=true CacheExteriors=true",
		"Low: one bounce of light. Medium: less grain. High: light also bounces more than once indoors. Ultra: more than once outdoors too "
		"(expensive)." },
	{ "Shaders.AmbientOcclusion.RT", "GIStrength", "Bounce light strength", nullptr, "How much bounced light is added. Higher looks more vivid, less realistic." },
	{ "Shaders.AmbientOcclusion.RT", "GIAmbient", "Bounce light from the sky (shows on cloudy days)", nullptr, "Lets sky light bounce too, so bounce light still shows on cloudy days and in shade." },
	// Reflections
	{ "Shaders.Reflections.RT", "Enabled", "Ray-traced reflections", nullptr, "Rays fill in what normal reflections miss: things off screen or hidden behind something. Needs Reflections on." },
	{ "Shaders.Reflections.RT", nullptr, "Surfaces that get rays",
		"Shiny only: MaxRoughness=0.35 | Shiny and satin: MaxRoughness=0.5 | Most surfaces: MaxRoughness=0.75",
		"How rough a surface can be and still get ray-traced reflections; rougher ones keep the screen-space reflections. Wet ground counts "
		"by Reflections' WetRoughness: if it is rougher than this, puddles get no rays." },
	{ "Shaders.Reflections.RT", "Weapon", "Weapon reflections", nullptr, "Your gun reflects the world around it: lamps, signs, the sky. Strongest on metal parts." },
	{ "Shaders.Reflections.RT", "Water", "Ray-traced water reflections", nullptr, "Water reflects the world with rays instead of the old reflection map." },
	// Scene and path tracing
	{ "Main.Main.RayTracing", "Actors", "People in the traced scene", nullptr, "People and creatures cast traced shadows and show up in traced reflections. Costs some frame rate in crowds." },
	{ "Main.Main.RayTracing", "AlphaTested", "See-through meshes (fences, leaves, grates)", nullptr, "Fences, leaves and grates cast traced shadows with their holes, not as solid blocks." },
	{ "Main.Main.RayTracing", "Grass", "Grass", nullptr, "Grass casts traced shadows and shows in reflections. Needs See-through meshes on." },
	{ "Main.Main.RayTracing", "HitTextures", "Real textures in reflections and bounce light", nullptr, "Reflections and bounce light use the real colours of what they hit instead of flat ones. Uses more video memory." },
	{ "Main.Main.RayTracing", "FarObjectLOD", "Distant buildings and objects", nullptr, "Distant buildings and objects are in the traced scene, so far shadows and reflections are complete." },
	{ "Main.Main.RayTracing", "FarLandLOD", "Distant land", nullptr, "Distant hills are in the traced scene, so far shadows and reflections include them." },
	{ "Main.Main.RayTracing", "PTRealtime", "Path tracing (experimental, very heavy)", nullptr, "The whole picture lit by path tracing, like a film render. Experimental and very heavy: for top GPUs." },
	// GunFX (Main)
	{ "Main.GunFX.Main", "Puff", "Smoke puff at the muzzle", nullptr, "A small puff of smoke from the barrel on every shot. Needs GunFX.dll." },
	{ "Main.GunFX.Main", "HeatSmoke", "Smoke from a hot barrel", nullptr, "A thin strand of smoke rises from the barrel after sustained fire." },
	{ "Main.GunFX.Main", "AfterFireTrail", "Smoke trail after firing", nullptr, "A wispy trail rises from the barrel once you stop shooting." },
	{ "Main.GunFX.Main", "EjectionSmoke", "Smoke from the ejection port", nullptr, "A burst of smoke with every spent casing, bigger when the gun is hot." },
	{ "Main.GunFX.Main", "BarrelGlow", "Glowing hot barrel", nullptr, "The barrel glows after sustained fire and cools down slowly." },
	{ "Main.GunFX.Main", "HeatHaze", "Heat shimmer over a hot barrel", nullptr, "The air shimmers around a hot barrel." },
	{ "Main.GunFX.Main", "MuzzleBlast", "Heat ripple on every shot", nullptr, "A quick ripple of hot air at the muzzle each time you fire." },
	{ "Main.GunFX.Main", "VolumetricSmoke", "Real 3D smoke (experimental)", nullptr, "Gun smoke drawn as real 3D smoke that lamps and the sun light. Costs frame rate." },
	{ "Main.GunFX.Main", "SmokeHaze", "Smoke haze fills the room", nullptr, "Firing for a while fills the room with haze. Needs Real 3D smoke; the most expensive part." },
	{ "Main.GunFX.Main", "EnergyWeapons", "Also on energy weapons", nullptr, "Lasers, plasma and other energy weapons get the effects too." },
	// AmbientLight (Main)
	{ "Shaders.AmbientLight.Main", "OcclusionStrength", "Corner shading strength", nullptr, "How dark creases, corners and the ground under things get." },
	{ "Shaders.AmbientLight.Main", "Radius", "Corner shading reach", nullptr, "How far the corner shading and bounce light reach." },
	{ "Shaders.AmbientLight.Main", "GridBounce", "Sun bounce light strength", nullptr, "Soft bounce light from the sun in daylight, spread by a 3D light grid round you. Experimental." },
	{ "Shaders.AmbientLight.Main", "GridLamps", "Lamp bounce light (indoors, night)", nullptr, "Lamps and fires light the room softly, with light pooling and spreading round them. Indoors and at night." },
	{ "Shaders.AmbientLight.Main", "ComputeGI", "Screen-space bounce light", nullptr, "Light bounces off what is on screen onto nearby surfaces. Replaces ray-traced bounce light when on. Needs the DXVK fork." },
	{ "Shaders.AmbientLight.Main", "ComputeGIStrength", "Screen-space bounce light strength", nullptr, "How strong the screen-space bounce light is. Needs the DXVK fork." },
	{ "Shaders.AmbientLight.Main", nullptr, "Screen-space bounce light quality",
		"Low: ComputeGIQuality=0 | Medium: ComputeGIQuality=1 | High: ComputeGIQuality=2",
		"More detail in the shading and bounce light: smoother and steadier, but costs more." },
	// AmbientOcclusion (Exteriors)
	{ "Shaders.AmbientOcclusion.Exteriors", "Enabled", "Corner shading outdoors", nullptr, "Darkens corners and creases outdoors for more depth." },
	{ "Shaders.AmbientOcclusion.Exteriors", "StrengthMultiplier", "Corner shading strength", nullptr, "How dark corners and creases get." },
	{ "Shaders.AmbientOcclusion.Exteriors", "Range", "Shading reach (larger is softer)", nullptr, "Larger gives wider, softer shading; smaller gives sharper detail in creases." },
	{ "Shaders.AmbientOcclusion.Exteriors", "ClampStrength", "Darkest shading allowed", nullptr, "Limits how dark the shading may get. Lower values allow near black." },
	// AmbientOcclusion (Interiors)
	{ "Shaders.AmbientOcclusion.Interiors", "Enabled", "Corner shading indoors", nullptr, "Darkens corners and creases indoors for more depth." },
	{ "Shaders.AmbientOcclusion.Interiors", "StrengthMultiplier", "Corner shading strength", nullptr, "How dark corners and creases get." },
	{ "Shaders.AmbientOcclusion.Interiors", "Range", "Shading reach (larger is softer)", nullptr, "Larger gives wider, softer shading; smaller gives sharper detail in creases." },
	{ "Shaders.AmbientOcclusion.Interiors", "ClampStrength", "Darkest shading allowed", nullptr, "Limits how dark the shading may get. Lower values allow near black." },
	// AmbientOcclusion (GTAO)
	{ "Shaders.AmbientOcclusion.GTAO", "Enabled", "Better corner shading (GTAO)", nullptr, "A more accurate version of the corner shading: no grey flat ground and no halos round objects." },
	{ "Shaders.AmbientOcclusion.GTAO", "MaxRadius", "Longest reach (pixels)", nullptr, "The widest area on screen the shading looks at. Larger reaches further but costs more." },
	// AmbientOcclusion (Skylighting)
	{ "Shaders.AmbientOcclusion.Skylighting", "Enabled", "Darker under roofs and overhangs", nullptr, "Outdoors, removes sky light under roofs, porches, bridges and cliffs so they look properly shaded." },
	{ "Shaders.AmbientOcclusion.Skylighting", "Strength", "Strength", nullptr, "How much light is taken away under cover." },
	{ "Shaders.AmbientOcclusion.Skylighting", "Distance", "How far to look for cover", nullptr, "How far the light travels. Larger reaches further." },
	{ "Shaders.AmbientOcclusion.Skylighting", "Reflections", "Also darken sky reflections under cover", nullptr, "Wet floors and puddles under cover stop mirroring a bright sky." },
	// BounceLight (Main)
	{ "Shaders.BounceLight.Main", "Strength", "Bounce light strength", nullptr, "How much light bounces from lit surfaces onto nearby ones." },
	{ "Shaders.BounceLight.Main", "Radius", "Bounce light reach", nullptr, "How far the bounce light reaches." },
	{ "Shaders.BounceLight.Main", "BlurRadius", "Softness", nullptr, "Smooths out grain. Higher is softer." },
	{ "Shaders.BounceLight.Main", nullptr, "Quality",
		"Faster: QuarterResolution=true | Sharper: QuarterResolution=false",
		"Faster works the bounce out on a quarter of the pixels (no visible difference for most people). Sharper uses half of them and costs a little more." },
	{ "Shaders.BounceLight.Main", "Temporal", "Steadier light (less grain)", nullptr, "Keeps light from the last frame to reduce grain and flicker. Can trail slightly when turning." },
	// Specular (Exterior)
	{ "Shaders.Specular.Exterior", "SunStrength", "Sun highlight strength", nullptr, "How bright the sun glint is on surfaces. Strongest when looking towards the sun." },
	{ "Shaders.Specular.Exterior", "Roughness", "Highlight softness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.Specular.Exterior", "SkyReflections", "Sky reflection strength", nullptr, "How much of the sky shows in surfaces, mostly at glancing angles." },
	{ "Shaders.Specular.Exterior", "DistanceFade", "Highlight distance", nullptr, "How far away highlights stay visible before fading out." },
	// Specular (Rain)
	{ "Shaders.Specular.Rain", "SunStrength", "Sun highlight strength", nullptr, "The same, while it rains." },
	{ "Shaders.Specular.Rain", "Roughness", "Highlight softness", nullptr, "Wet surfaces are smoother: smaller, brighter highlights and clearer reflections." },
	{ "Shaders.Specular.Rain", "SkyReflections", "Sky reflection strength", nullptr, "The same, while it rains." },
	{ "Shaders.Specular.Rain", "DistanceFade", "Highlight distance", nullptr, "The same, while it rains." },
	// PBR (Main)
	{ "Shaders.PBR.Main", "ReflectivityExterior", "Shininess outdoors", nullptr, "How shiny and reflective surfaces look outdoors." },
	{ "Shaders.PBR.Main", "ReflectivityInterior", "Shininess indoors", nullptr, "How shiny surfaces look indoors. Lower it if floors look too glossy." },
	{ "Shaders.PBR.Main", "HairHighlight", "Hair shine", nullptr, "How bright the shine on hair is. 0 removes it." },
	{ "Shaders.PBR.Main", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.Main", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Main", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Main", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.Main", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	// PBR (Rain)
	{ "Shaders.PBR.Rain", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.Rain", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Rain", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Rain", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.Rain", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	// PBR (Night)
	{ "Shaders.PBR.Night", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.Night", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Night", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Night", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.Night", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	// PBR (NightRain)
	{ "Shaders.PBR.NightRain", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.NightRain", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.NightRain", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.NightRain", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	{ "Shaders.PBR.NightRain", "SkylightingScale", "Sky colour in shade", nullptr, "Adds the colour of the sky to shaded areas. 0 turns it off." },
	// PBR (Interiors)
	{ "Shaders.PBR.Interiors", "Roughness", "Surface roughness", nullptr, "Lower gives small bright highlights and clear reflections; higher gives a broad faint sheen." },
	{ "Shaders.PBR.Interiors", "AmbientScale", "Ambient light strength", nullptr, "How bright the general ambient light is on objects." },
	{ "Shaders.PBR.Interiors", "LightingScale", "Sun and lamp light strength", nullptr, "How strong the sun and lamp light is on objects." },
	{ "Shaders.PBR.Interiors", "Saturation", "Colour saturation", nullptr, "How vivid the colours of lit objects are." },
	// PBR (Metal)
	{ "Shaders.PBR.Metal", "MetalOn", "Metal look", nullptr, "Gives metal objects a proper metal look, with reflections instead of plastic shine." },
	{ "Shaders.PBR.Metal", "FirstPerson", "Metal on your weapon", nullptr, "How metal your weapon looks in first person." },
	{ "Shaders.PBR.Metal", "World", "Metal look on the world", nullptr, "How metal things in the world look. Grey concrete or cloth can be mistaken for metal, so keep it moderate." },
	{ "Shaders.PBR.Metal", "Roughness", "Metal finish (mirror to rough)", nullptr, "Finish of bare metal: low is a mirror, high is rough cast." },
	{ "Shaders.PBR.Metal", "Reflections", "Metal reflection strength", nullptr, "How strongly metal reflects the sky and its surroundings." },
	{ "Shaders.PBR.Metal", "CubeReflections", "Metal reflects its surroundings", nullptr, "Reflects what is really around it, such as the street, neon and lamps. Needs Reflections on." },
	// PBR (EnvMaps)
	{ "Shaders.PBR.EnvMaps", "Lighting", "Chrome reflections follow the light", nullptr, "Makes chrome shine follow the light around it, so it is dull in a dark cave and bright at noon." },
	{ "Shaders.PBR.EnvMaps", "Strength", "Chrome reflection brightness", nullptr, "How bright chrome and glass reflections are in the light." },
	{ "Shaders.PBR.EnvMaps", "CubeReflections", "Chrome shows its surroundings", nullptr, "Reflects what is really around it, such as the street, neon and lamps. Needs Reflections on." },
	// PBR (MaterialRules)
	{ "Shaders.PBR.MaterialRules", "Enabled", "Material types by texture", nullptr, "Sorts surfaces by texture (guns, glass, wood, rock) so only the right ones look metal or polished." },
	// PBR (Rmaos)
	{ "Shaders.PBR.Rmaos", "Enabled", "Material maps", nullptr, "Uses material maps for roughness, metal and shading where a texture has one." },
	{ "Shaders.PBR.Rmaos", "Occlusion", "Material map shading strength", nullptr, "How much the material maps darken the ambient light in crevices." },
	{ "Shaders.PBR.Rmaos", "MetalFromMap", "Metal from material maps", nullptr, "Takes metal from the material maps instead of guessing from the texture." },
	{ "Shaders.PBR.Rmaos", "SpecularFromMap", "Shine from material maps", nullptr, "Takes how much surfaces reflect from the material maps." },
	// PBR (Dust)
	{ "Shaders.PBR.Dust", "Enabled", "Dust on top surfaces", nullptr, "A thin sandy coat on upward-facing surfaces such as rock tops, car roofs and ledges." },
	{ "Shaders.PBR.Dust", "Amount", "Dust amount", nullptr, "How thick the dust coat is." },
	{ "Shaders.PBR.Dust", "ColorR", "Dust colour", nullptr, "The colour of the dust. Sand by default." },
	{ "Shaders.PBR.Dust", "Patchiness", "Patchiness", nullptr, "0 gives an even coat; higher gives patches with bare spots." },
	{ "Shaders.PBR.Dust", "Size", "Patch size", nullptr, "How big each patch of dust is." },
	{ "Shaders.PBR.Dust", "Interiors", "Also indoors", nullptr, "Also adds dust indoors, in ruins and dusty rooms." },
	// PBR (MicroShadow)
	{ "Shaders.PBR.MicroShadow", "Enabled", "Deeper cracks and crevices", nullptr, "Bumps in surfaces shadow themselves, so cracks look deep instead of painted on. Needs material maps." },
	{ "Shaders.PBR.MicroShadow", "Strength", "Strength", nullptr, "How deep cracks and crevices look." },
	// PBR (Linear)
	{ "Shaders.PBR.Linear", "Enabled", "Realistic light blending (linear lighting)", nullptr, "Blends light the way real light adds up: smoother falloff into shade and better lamp mixing." },
	{ "Shaders.PBR.Linear", "Gamma", "Light blending curve (gamma)", nullptr, "How light is blended. Leave at the standard value unless things look too dark or flat." },
	// PBR (LampFalloff)
	{ "Shaders.PBR.LampFalloff", "Exterior", "Realistic lamp falloff outdoors", nullptr, "Outdoor lamps and fires fade smoothly with distance instead of lighting an even disc." },
	{ "Shaders.PBR.LampFalloff", "Interior", "Realistic lamp falloff indoors", nullptr, "Indoor lamps make pools of light with darker areas between." },
	{ "Shaders.PBR.LampFalloff", "Falloff", "Lamp falloff speed", nullptr, "How fast a lamp dims with distance. Small gives a bright spot and a long dim tail." },
	{ "Shaders.PBR.LampFalloff", "Brightness", "Lamp brightness", nullptr, "How bright lamps are while this is on." },
	// Reflections (Main)
	// 2026-11-05: one choice that sets every setting the floor reflections' visibility depends on, so each look shows what it says
	// (separately, Specular occlusion hid sharp reflections on dark floors and physically faint floors showed only lamps).
	{ "Shaders.Reflections.Main", nullptr, "Look",
		"Realistic: Mirror=0.0 Roughness=0.4 InteriorFloors=0.0 GroundSheen=0.0 SpecularOcclusion=1.0 | "
		"Glossy floors: Mirror=0.3 Roughness=0.12 InteriorFloors=1.0 GroundSheen=1.0 SpecularOcclusion=0.0 | "
		"Mirror (showcase): Mirror=1.0 Roughness=0.02 InteriorFloors=2.0 GroundSheen=2.0 SpecularOcclusion=0.0",
		"Realistic: only shiny materials (tiles, glass, metal, wet ground) reflect, as in life: mostly lamps and bright things show. "
		"Glossy floors: every floor gets a clear sheen with people and objects in it. Mirror: every floor is a perfect mirror, to show "
		"off reflections (and ray-traced ones). Turning Specular occlusion back on (Realistic) takes a restart." },
	{ "Shaders.Reflections.Main", "GroundSheen", "Dry ground reflections", nullptr, "How much dry ground, such as roads and pavements, mirrors its surroundings. Streets pick up neon and lamps at night." },
	{ "Shaders.Reflections.Main", "RainWetness", "Wet ground reflections", nullptr, "How much rain makes the ground mirror neon, lamps and sky. Covered areas stay dry." },
	{ "Shaders.Reflections.Main", "InteriorFloors", "Floor reflections indoors", nullptr, "How much floors indoors mirror the room, wood and carpet too." },
	{ "Shaders.Reflections.Main", "Roughness", "Dry reflection blur", nullptr, "Lower gives mirror-like dry reflections; higher blurs them to a glow." },
	{ "Shaders.Reflections.Main", "Mirror", "Mirror floors", nullptr, "0 is real polished stone: you mostly see bright lamps in it unless you look along the floor. 1 turns reflecting floors into true mirrors that show people and objects at any angle. Use with Dry reflection blur near 0.02." },
	{ "Shaders.Reflections.Main", "WetRoughness", "Wet reflection blur", nullptr, "Lower gives sharp mirror-like reflections in puddles; higher blurs them." },
	{ "Shaders.Reflections.Main", "MaterialReflect", "Smooth surface reflections", nullptr, "How much smooth materials such as tiles, glass and polished counters reflect. Needs material maps on." },
	{ "Shaders.Reflections.Main", "DrawDistance", "Reflection distance", nullptr, "How far away surfaces still reflect." },
	{ "Shaders.Reflections.Main", nullptr, "Quality",
		"Low: Steps=12 | Medium: Steps=24 | High: Steps=40",
		"Higher gives steadier reflections with fewer gaps, and costs more." },
	// Sun shadows (Main)
	{ "Shaders.ShadowsExteriors.Main", "Enabled", "Sun shadows", nullptr, "Turns the shadows cast by the sun on or off outdoors." },
	{ "Shaders.ShadowsExteriors.Main", "Darkness", "Shadow darkness", nullptr, "How dark sun shadows are. Lower gives pale shadows, higher gives near-black ones." },
	{ "Shaders.ShadowsExteriors.Main", "NightMinDarkness", "Shadow darkness in moonlight", nullptr, "How dark shadows stay on nights with little moon. Set to zero for no shadows when the moon is dark." },
	{ "Shaders.ShadowsExteriors.Main", "SmoothCharacterShadows", "Smooth shadows on faces and skin", nullptr, "Shadows on faces and skin follow the smooth shape of the head instead of showing blocky facets." },
	{ "Shaders.ShadowsExteriors.Main", "UsePointShadowsNight", "Lamp shadows outdoors at night", nullptr, "Lamps and fires outdoors cast shadows at night. Darkens the whole picture slightly." },
	// Sun shadows (Sun movement)
	{ "Shaders.ShadowsExteriors.SunSmoothing", "SmoothSun", "Smooth shadow movement", nullptr, "Shadows glide as the sun moves instead of jumping a step at a time." },
	{ "Shaders.ShadowsExteriors.SunSmoothing", "GlideSeconds", "Shadow glide time (seconds)", nullptr, "How many real seconds each small shadow move takes. Longer is smoother but lags behind the sun." },
	// Sun shadows (Casters in the top-down map)
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Trees", "Trees and plants", nullptr, "Trees count as roofs, so rain and sky light stop under them." },
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Statics", "Buildings and rocks", nullptr, "Buildings and rocks count as roofs, so rain and sky light stop under them." },
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Misc", "Small props and junk", nullptr, "Small props such as awnings and junk count as roofs for rain and sky light." },
	{ "Shaders.ShadowsExteriors.FormsOrtho", "Terrain", "Ground", nullptr, "Hills and overhanging ground count as roofs for rain and sky light." },
	// Sun shadows (Contact shadows)
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Enabled", "Contact shadows (small detail shadows)", nullptr, "Adds small dark shadows where things touch, such as under rocks, plants and feet. Costs a little frame rate." },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Intensity", "Contact shadow strength", nullptr, "How dark the small contact shadows are." },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "BlurRadius", "Contact shadow softness", nullptr, "How soft contact shadows are. Keep it low, or they look blocky." },
	{ "Shaders.ShadowsExteriors.ScreenSpace", "Sharp", "Sharper, steadier contact shadows", nullptr, "Crisper contact shadows that shimmer less. Costs a little more frame rate." },
	// Sun shadows (Shadow maps)
	{ "Shaders.ShadowsExteriors.ShadowMaps", nullptr, "Shadow quality",
		"Low: CascadeResolution=0 Distance=4000 | Medium: CascadeResolution=1 Distance=6000 | High: CascadeResolution=2 Distance=8000 | Very high: CascadeResolution=4 Distance=10000",
		"Sharper sun shadows that reach further from you. Higher costs more video memory and frame rate, a lot more at Very high." },
	{ "Shaders.ShadowsExteriors.ShadowMaps", "SmoothFilter", "Smoother shadow edges", nullptr, "Rounds off the stair steps along shadow edges. Edges get slightly softer." },
	{ "Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap", "Sharper shadows on your character", nullptr, "Gives your own character much finer sun shadows, handy in third person. Needs a restart." },
	// Sun shadows (Top-down map)
	{ "Shaders.ShadowsExteriors.Ortho", nullptr, "Roof detection quality (rain, sky light)",
		"Low: Resolution=1 Distance=2000 | Medium: Resolution=2 Distance=3000 | High: Resolution=3 Distance=4000",
		"The overhead map that tells which ground is under a roof: rain and sky light stop there. Higher is more exact and costs a little more." },
	// Sun shadows (Mountain shadows)
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Enabled", "Mountain shadows", nullptr, "Hills and mountains shade the land far into the distance, most visible at sunrise and sunset." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Strength", "Mountain shadow strength", nullptr, "How much sunlight a mountain shadow takes away." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Softness", "Mountain shadow softness", nullptr, "How gradually mountain shadows fade at their edges." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "Contrast", "Mountain shadow sharpness", nullptr, "Higher gives a harder line between lit and shaded land." },
	{ "Shaders.ShadowsExteriors.TerrainShadows", nullptr, "Mountain shadow detail",
		"Low: MaxResolution=0 | Medium: MaxResolution=1 | High: MaxResolution=2",
		"How detailed mountain shadows are. Higher is finer and uses more video memory." },
	// Lamp shadows (Main)
	{ "Shaders.ShadowsInteriors.Main", "Darkness", "Shadow darkness", nullptr, "How dark lamp shadows are indoors." },
	{ "Shaders.ShadowsInteriors.Main", "LightPoints", "Lamps casting shadows at once", nullptr, "How many lamps can cast shadows together. More gives richer rooms and costs frame rate." },
	{ "Shaders.ShadowsInteriors.Main", nullptr, "Lamp shadow detail",
		"Low: ShadowCubeMapSize=256 | Medium: ShadowCubeMapSize=512 | High: ShadowCubeMapSize=1024",
		"How sharp each lamp shadow is. Higher costs more video memory and frame rate." },
	{ "Shaders.ShadowsInteriors.Main", "DrawDistance", "Lamp shadow distance", nullptr, "How far from you lamps still cast shadows." },
	{ "Shaders.ShadowsInteriors.Main", "ContactShadows", "Lamp contact shadows (where props meet floors)", nullptr, "Adds small dark lines where props meet the floor or tables meet a wall. Off at zero." },
	{ "Shaders.ShadowsInteriors.Main", "PlayerShadowFirstPerson", "Your own shadow in first person", nullptr, "Your body casts a lamp shadow even in first person." },
	// Lamp shadows (Per-object shadows)
	{ "Shaders.ShadowsInteriors.Forward", "Enabled", "Lamp shadows on each object", nullptr, "Each lamp shadows only its own light, so rooms keep their colours. Off gives the older flat darkening." },
	{ "Shaders.ShadowsInteriors.Forward", "Strength", "Lamp shadow strength", nullptr, "How dark a lamp shadow is. At full strength it blocks all of that lamp's light." },
	{ "Shaders.ShadowsInteriors.Forward", "Softness", "Lamp shadow softness", nullptr, "Zero gives hard edges and costs least. Higher gives softer edges and costs frame rate in busy rooms." },
	{ "Shaders.ShadowsInteriors.Forward", nullptr, "Lamp shadow edge quality",
		"Low: Quality=0 | Medium: Quality=1 | High: Quality=2 | Ultra: Quality=3",
		"How smooth lamp shadow edges are. Higher is smoother and costs more frame rate. Needs lamp shadow softness above zero." },
	{ "Shaders.ShadowsInteriors.Forward", "ContactHardening", "Sharp where objects touch the ground", nullptr, "Shadows are sharp where an object touches the floor and blur further away, as in real life." },
	{ "Shaders.ShadowsInteriors.Forward", "LampSize", "Lamp size (softer far shadows)", nullptr, "Bigger lamps make shadows blur more with distance. Zero keeps them sharp." },
	{ "Shaders.ShadowsInteriors.Forward", "FirstPerson", "Shadows on your weapon and arms", nullptr, "Lamp shadows fall on your weapon and arms in first person." },
	// Lamp shadows (Shadow atlas)
	{ "Shaders.ShadowsInteriors.Atlas", nullptr, "Lamp shadow map size",
		"Low: Size=2048 | Medium: Size=4096 | High: Size=8192",
		"Video memory set aside for lamp shadows. Larger is sharper and uses more memory: High takes 128 MB." },
	// Sun shadows (Soft contact edges)
	{ "Shaders.ContactHardening.Main", "SunSize", "Shadow softness with distance", nullptr, "Shadow edges soften further from what casts them. Higher is softer but can show jagged edges." },
	{ "Shaders.ContactHardening.Main", "MaxSoftness", "Softest shadow edge", nullptr, "The softest a shadow edge can get. Higher allows very soft shadows under tall objects and costs more." },
	{ "Shaders.ContactHardening.Main", "AtNight", "Also at night", nullptr, "Keeps soft shadow edges at night too. Moonlight shadows are faint, so you will barely see it." },
	// Cloud shadows (Main)
	{ "Shaders.CloudShadows.Main", "Enabled", "Cloud shadows", nullptr, "Clouds cast moving shadows on the land. Cheap." },
	{ "Shaders.CloudShadows.Main", "Strength", "Cloud shadow strength", nullptr, "How much sunlight a thick cloud takes away." },
	{ "Shaders.CloudShadows.Main", "Density", "Cloud thickness", nullptr, "How solid clouds count. Raise it if thin clouds cast no visible shadow." },
	{ "Shaders.CloudShadows.Main", "Speed", "Cloud shadow speed", nullptr, "How fast the shadows drift with the wind." },
	{ "Shaders.CloudShadows.Main", "TileSize", "Cloud shadow size", nullptr, "Smaller gives smaller, more frequent cloud shadows. Larger gives big, slow ones." },
	{ "Shaders.CloudShadows.Main", "WaterShadows", "Also on water", nullptr, "Cloud and mountain shadows also darken water. Shallow water darkens most." },
	// Skin (Main)
	{ "Shaders.Skin.Main", "SpecularStrength", "Skin shine", nullptr, "How strongly skin catches the light. Zero is flat and dry, high looks oily." },
	{ "Shaders.Skin.Main", "Roughness", "Skin roughness (lower is oilier)", nullptr, "Lower gives tight, bright highlights. Higher gives a matte look." },
	{ "Shaders.Skin.Main", "Scattering", "Skin softness (light under the skin)", nullptr, "Light spreads under the skin, softening the line between light and shadow, with a warm red edge." },
	{ "Shaders.Skin.Main", "Translucency", "Light through ears and fingers", nullptr, "Thin parts such as ears and fingers glow red when the light is behind them." },
	{ "Shaders.Skin.Main", "SkyReflection", "Sky reflection on skin", nullptr, "Skin reflects the sky at glancing angles. Outdoors only." },
	{ "Shaders.Skin.Main", "RainWetness", "Wet skin in rain", nullptr, "How wet and shiny skin gets in the rain." },
	{ "Shaders.Skin.Main", "VanillaMatchedHighlights", "Vanilla-strength skin highlights", nullptr, "One makes skin highlights as strong as the game's own; zero keeps the physically based strength." },
	// Skin (Interiors)
	{ "Shaders.Skin.Interiors", "SpecularStrength", "Skin shine", nullptr, "How strongly skin catches lamp light indoors. Zero is flat and dry, high looks oily." },
	{ "Shaders.Skin.Interiors", "Roughness", "Skin roughness (lower is oilier)", nullptr, "Lower gives tight, bright highlights indoors. Higher gives a matte look." },
	{ "Shaders.Skin.Interiors", "Scattering", "Skin softness (light under the skin)", nullptr, "Light spreads under the skin indoors, softening the edge of shadows." },
	{ "Shaders.Skin.Interiors", "Translucency", "Light through ears and fingers", nullptr, "Ears and fingers glow red when a lamp is behind them." },
	{ "Shaders.Skin.Interiors", "ScreenSpace", "Soft, waxy skin glow", nullptr, "Spreads light across the skin on screen for a softer, waxier look indoors." },
	{ "Shaders.Skin.Interiors", "Width", "Skin glow spread", nullptr, "How far that soft glow spreads. Higher is softer and waxier." },
	// Skin (Scattering)
	{ "Shaders.Skin.Scattering", "ScreenSpace", "Soft, waxy skin glow", nullptr, "Spreads light across the skin on screen for a softer, waxier look outdoors." },
	{ "Shaders.Skin.Scattering", "Width", "Skin glow spread", nullptr, "How far that soft glow spreads. Higher is softer and waxier." },
	{ "Shaders.Skin.Scattering", "AlbedoDetail", "Skin texture detail kept", nullptr, "Keeps freckles, pores and stubble sharp. Lower blurs them into a waxy look." },
	// Cloth (Main)
	{ "Shaders.Cloth.Main", "Enabled", "Moving cloth", nullptr, "Capes, coats, flags, banners and curtains sway. Needs a restart." },
	{ "Shaders.Cloth.Main", "UseGameWind", "Follow the wind of the weather", nullptr, "Cloth moves with the weather's wind direction and strength outdoors." },
	{ "Shaders.Cloth.Main", "WindIntensity", "Cloth movement amount", nullptr, "How far loose cloth moves. Higher looks windier." },
	{ "Shaders.Cloth.Main", "FlutterAmount", "Flutter", nullptr, "How much cloth quivers with small fast movements." },
	{ "Shaders.Cloth.Main", "RippleAmount", "Billowing", nullptr, "How much cloth billows in and out like a sail." },
	{ "Shaders.Cloth.Main", "WalkSim", "Coats swing as you walk", nullptr, "Worn coats hang from the hips and the hem trails behind you as you move." },
	{ "Shaders.Cloth.Main", "AutoDetect", "Move all loose clothing", nullptr, "Clothing not on the cloth list also moves if it hangs free of the legs, such as long coats and robes." },
	{ "Shaders.Cloth.Main", "MotionIndoors", "Coats swing indoors too", nullptr, "Worn coats follow your movement indoors as well." },
	// Cloth (Interiors)
	{ "Shaders.Cloth.Interiors", "Enabled", "Cloth moves indoors", nullptr, "Curtains, banners and flags sway gently indoors, in a draught." },
	{ "Shaders.Cloth.Interiors", "Strength", "Indoor cloth movement", nullptr, "How much indoor cloth moves. Zero is still, higher moves more." },
	// Pip-Boy sleeves (Main)
	{ "Shaders.PipboyFit.Main", "Enabled", "Sleeves fit under the Pip-Boy", nullptr, "In first person, armour sleeves are pulled in so they do not poke through the Pip-Boy. Needs a restart." },
	{ "Shaders.PipboyFit.Main", "Strength", "Fit strength", nullptr, "How much the sleeves are pulled in. Zero leaves them as they were." },
	{ "Shaders.PipboyFit.Main", "ThirdPerson", "Also in third person", nullptr, "Your own sleeves are fitted in third person too. Other characters are not changed." },
	// Blood (Main)
	{ "Shaders.BloodFlow.Main", "Splats", "Blood runs down walls", nullptr, "Blood splats grow streams that run down walls and slopes." },
	{ "Shaders.BloodFlow.Main", "Pools", "Blood pools under bodies", nullptr, "A pool of blood spreads under a body after death and slides downhill on slopes." },
	{ "Shaders.BloodFlow.Main", "BloodAmount", "Amount of blood", nullptr, "How much blood shows. Lower breaks it up into beads and thin streaks." },
	{ "Shaders.BloodFlow.Main", "DripLength", "Blood stream length", nullptr, "How long blood streams run. Zero means no streams." },
	{ "Shaders.BloodFlow.Main", "Gloss", "Blood shine", nullptr, "How much fresh blood glints in sun and lamp light. Dry blood stays dull." },
	{ "Shaders.BloodFlow.Main", "FreshColorR", "Fresh blood colour", nullptr, "The colour of fresh blood." },
	{ "Shaders.BloodFlow.Main", "DryColorR", "Dried blood colour", nullptr, "The colour blood dries to." },
	{ "Shaders.BloodFlow.Main", "MaxDistance", "Blood distance", nullptr, "How far from you blood streams and pools are drawn. They fade out over the last stretch." },
	// Blood (Fluid)
	{ "Shaders.BloodFlow.Fluid", "Enabled", "Fluid blood (simulated, heavy)", nullptr, "Fresh blood is simulated as a real liquid that runs, gathers and drips. Costs a lot of frame rate." },
	{ "Shaders.BloodFlow.Fluid", "Amount", "Amount of blood", nullptr, "How much liquid blood each splat and pool makes." },
	{ "Shaders.BloodFlow.Fluid", "Viscosity", "Blood thickness", nullptr, "Higher gives thicker, slower blood that moves as one blob." },
	{ "Shaders.BloodFlow.Fluid", "Drops", "Dripping drops", nullptr, "How much of the blood in the air shows as drops, such as dripping off ledges." },
	{ "Shaders.BloodFlow.Fluid", nullptr, "Fluid blood detail",
		"Low: MaxParticles=8192 | Medium: MaxParticles=16384 | High: MaxParticles=65536",
		"How much liquid blood can exist at once. More looks fuller and costs more frame rate and memory." },
	// Blood on weapon and hands (Main)
	{ "Shaders.HandBlood.Main", "PerHit", "Blood on weapon and hands", nullptr, "Fresh blood nearby splashes onto your weapon and gloves. Higher gets you bloodier faster." },
	{ "Shaders.HandBlood.Main", "Reach", "Distance blood reaches you", nullptr, "How close fresh blood must be to get on you. Small means point blank only." },
	{ "Shaders.HandBlood.Main", "ColorR", "Blood colour", nullptr, "The colour of blood on your hands and weapon, until a blood splat has been seen." },
	{ "Shaders.HandBlood.Main", "Brightness", "Blood brightness", nullptr, "Below one gives darker, thicker-looking blood on the gun." },
	{ "Shaders.HandBlood.Main", "CellSize", "Spatter droplet size", nullptr, "Bigger gives a coarser spatter." },
	{ "Shaders.HandBlood.Main", "Run", "Blood running", nullptr, "How far drops run down after a hit. Zero keeps them round." },
	{ "Shaders.HandBlood.Main", "DryTime", "Drying time", nullptr, "Seconds until the blood dries dark and matte." },
	{ "Shaders.HandBlood.Main", "FadeTime", "Time to wear off", nullptr, "Seconds until the blood wears off. Zero keeps it. Water washes it off." },
	// Sky (Main)
	{ "Shaders.Sky.Main", "TintDayR", "Sky colour by day", nullptr, "Tints the daytime sky. White leaves it as the weather made it." },
	{ "Shaders.Sky.Main", "TintNightR", "Sky colour at night", nullptr, "Tints the night sky, for example a little blue for moonlit nights." },
	{ "Shaders.Sky.Main", "SaturationDay", "Sky colourfulness by day", nullptr, "How colourful the daytime sky is. Lower is paler and greyer, higher is richer." },
	{ "Shaders.Sky.Main", "SaturationNight", "Sky colourfulness at night", nullptr, "How colourful the night sky is. Lower is paler and greyer, higher is richer." },
	{ "Shaders.Sky.Main", "SunsetR", "Sunset colour boost", nullptr, "Adds colour to the sun at sunrise and sunset. Red by default." },
	{ "Shaders.Sky.Main", "MoonBrightness", "Moon brightness", nullptr, "How bright the moon looks, and how much light it throws on the world at night." },
	{ "Shaders.Sky.Main", "StarStrength", "Star brightness", nullptr, "How bright the stars are at night." },
	{ "Shaders.Sky.Main", "StarTwinkle", "Star twinkle speed", nullptr, "How fast the stars twinkle." },
	// Sky (Physical)
	{ "Shaders.Sky.Physical", "Enabled", "Realistic sky", nullptr, "Paints the sky from simulated air: blue overhead, pale at the horizon, a glow round the sun, red and gold at sunset." },
	{ "Shaders.Sky.Physical", "Brightness", "Sky brightness", nullptr, "How bright the realistic sky is compared with the weather's own sky." },
	{ "Shaders.Sky.Physical", "WeatherTint", "Keep the weather's sky colours", nullptr, "How much of each weather's own sky colour is kept. Low gives pure air colours." },
	{ "Shaders.Sky.Physical", "Haze", "Dust haze in the air", nullptr, "Dust in the air: a wider, whiter glow round the sun and a paler horizon." },
	// Sky (Clouds)
	{ "Shaders.Sky.Clouds", "Transparency", "Flat cloud transparency", nullptr, "How see-through the game's flat clouds are." },
	{ "Shaders.Sky.Clouds", "Brightness", "Flat cloud brightness", nullptr, "How bright the game's flat clouds are." },
	// VolumetricClouds (Main)
	{ "Shaders.VolumetricClouds.Main", nullptr, "Cloud quality",
		"Low: Resolution=0 Steps=32 | Medium: Resolution=0 Steps=48 | High: Resolution=1 Steps=64 | Very high: Resolution=2 Steps=64",
		"More detail and steadier cloud edges. Very high costs a lot of frame rate." },
	{ "Shaders.VolumetricClouds.Main", "CoveragePleasant", "Cloud cover in fair weather", nullptr, "How much of the sky the clouds fill in fair weather." },
	{ "Shaders.VolumetricClouds.Main", "CoverageCloudy", "Cloud cover when cloudy", nullptr, "How much of the sky the clouds fill in cloudy weather." },
	{ "Shaders.VolumetricClouds.Main", "Thickness", "Cloud thickness", nullptr, "How deep the cloud layer is. Deeper gives taller, heaped clouds with darker undersides." },
	{ "Shaders.VolumetricClouds.Main", "Density", "Cloud density", nullptr, "How solid the clouds are. Higher is denser and darker underneath, lower is more see-through." },
	{ "Shaders.VolumetricClouds.Main", "Brightness", "Cloud brightness", nullptr, "Overall brightness of the clouds." },
	{ "Shaders.VolumetricClouds.Main", "WindSpeed", "Cloud drift speed", nullptr, "How fast the clouds drift across the sky." },
	{ "Shaders.VolumetricClouds.Main", "Shadows", "Cloud shadows on the ground", nullptr, "Clouds cast moving shadows over the land. Zero turns them off." },
	// VolumetricFog (Main)
	{ "Shaders.VolumetricFog.Main", nullptr, "Fog style",
		"Current: LegacyFog=false ClassicLook=0 | Classic: LegacyFog=false ClassicLook=1 | Legacy: LegacyFog=true ClassicLook=0",
		"Current: this version's fog. Classic: the original mod's look. Legacy: the older fog, for presets made for it." },
	{ "Shaders.VolumetricFog.Main", "Amount", "Fog strength", nullptr, "How strong the fog is outdoors." },
	{ "Shaders.VolumetricFog.Main", "BaseDensity", "Fog thickness", nullptr, "How thick the fog is, whatever the weather. Higher hides the distance sooner." },
	{ "Shaders.VolumetricFog.Main", "FogSaturation", "Fog colour richness", nullptr, "How rich the fog's colour is. Lower is greyer." },
	{ "Shaders.VolumetricFog.Main", "NoiseStrength", "Fog patchiness", nullptr, "How patchy the fog is. Zero is an even haze, higher gives drifting banks." },
	{ "Shaders.VolumetricFog.Main", "WindSpeed", "Fog drift speed", nullptr, "How fast the fog patches drift." },
	{ "Shaders.VolumetricFog.Main", "SunGlow", "Glow round the sun", nullptr, "The bright glow in the fog round the sun, strongest at sunrise and sunset." },
	{ "Shaders.VolumetricFog.Main", "DistantFogBlend", "Haze on distant hills", nullptr, "How strongly far hills and terrain fade into the sky near the horizon." },
	// VolumetricFog (Night)
	{ "Shaders.VolumetricFog.Night", "AmountScale", "Fog strength at night", nullptr, "Fog strength at night compared with the day. Below 1 is thinner, above 1 is stronger." },
	{ "Shaders.VolumetricFog.Night", "DensityScale", "Fog thickness at night", nullptr, "Fog thickness at night compared with the day." },
	{ "Shaders.VolumetricFog.Night", "InscatteringScale", "Fog brightness at night", nullptr, "How bright and glowing the fog looks at night. Lower is darker." },
	{ "Shaders.VolumetricFog.Night", "TintR", "Fog colour at night", nullptr, "Tints the fog at night. White leaves it as it is." },
	// VolumetricFog (Interiors)
	{ "Shaders.VolumetricFog.Interiors", "Amount", "Fog strength", nullptr, "How strong the fog is indoors." },
	{ "Shaders.VolumetricFog.Interiors", "BaseDensity", "Fog thickness", nullptr, "How thick the fog is indoors." },
	{ "Shaders.VolumetricFog.Interiors", "FogSaturation", "Fog colour richness", nullptr, "How rich the fog's colour is indoors. Lower is greyer." },
	{ "Shaders.VolumetricFog.Interiors", "NoiseStrength", "Fog patchiness", nullptr, "How patchy the indoor fog is. Zero is an even haze." },
	{ "Shaders.VolumetricFog.Interiors", "LampHazeEnabled", "Glow round lamps in the fog", nullptr, "Lamps glow softly in the fog indoors. Off while it is being tuned." },
	{ "Shaders.VolumetricFog.Interiors", "LampHaze", "Lamp glow strength", nullptr, "How strongly the fog glows round each lamp. Needs the lamp glow switched on." },
	// DustMotes (Main)
	{ "Shaders.DustMotes.Main", "Density", "Amount of dust", nullptr, "How many specks of dust float in the air." },
	{ "Shaders.DustMotes.Main", "Size", "Dust speck size", nullptr, "How big each speck of dust looks." },
	{ "Shaders.DustMotes.Main", "Brightness", "Dust brightness", nullptr, "How brightly the specks catch the light." },
	{ "Shaders.DustMotes.Main", "MaxDistance", "Dust distance", nullptr, "How far from you dust is drawn. Farther costs more frame rate." },
	{ "Shaders.DustMotes.Main", "Drift", "Dust drift speed", nullptr, "How fast the dust floats about." },
	{ "Shaders.DustMotes.Main", "Interiors", "Dust indoors", nullptr, "Show floating dust indoors, lit by the room's lamps." },
	{ "Shaders.DustMotes.Main", "Exteriors", "Dust outdoors", nullptr, "Show floating dust outdoors, lit by street lights and fires at night." },
	// VolumetricLight (Main)
	{ "Shaders.VolumetricLight.Main", "Strength", "Light shaft strength", nullptr, "How visible the sunbeams through trees and buildings are by day. Outdoors only; needs the exterior shadows on." },
	{ "Shaders.VolumetricLight.Main", "Density", "Air thickness", nullptr, "How thick the air is. Higher gives stronger, hazier beams over a shorter distance." },
	{ "Shaders.VolumetricLight.Main", "MaxDistance", "Light shaft distance", nullptr, "How far from you the beams are drawn." },
	{ "Shaders.VolumetricLight.Main", "Darken", "Darkening of shaded air", nullptr, "How much shaded air between the beams dims what is behind it." },
	{ "Shaders.VolumetricLight.Main", "RainStrength", "Strength in rain", nullptr, "How much of the beam strength is kept in rain." },
	{ "Shaders.VolumetricLight.Main", nullptr, "Quality",
		"Low: Steps=8 BlurRadius=6.0 | Medium: Steps=16 BlurRadius=4.0 | High: Steps=32 BlurRadius=3.0",
		"More samples and less smoothing give cleaner beams but cost more frame rate." },
	// VolumetricLight (Night)
	{ "Shaders.VolumetricLight.Night", "Strength", "Moonlight shaft strength", nullptr, "How visible the moonlight beams are at night." },
	{ "Shaders.VolumetricLight.Night", "Darken", "Darkening of shaded air", nullptr, "How much shaded air between the beams dims what is behind it at night." },
	{ "Shaders.VolumetricLight.Night", "Density", "Air thickness", nullptr, "How thick the air is at night. Higher gives stronger, hazier beams." },
	{ "Shaders.VolumetricLight.Night", "MaxDistance", "Light shaft distance", nullptr, "How far from you the moonlight beams are drawn." },
	// GodRays (Main)
	{ "Shaders.GodRays.Main", "DayMultiplier", "Light shafts by day", nullptr, "How strong the light shafts streaming from the sun are by day." },
	{ "Shaders.GodRays.Main", "NightMultiplier", "Light shafts at night", nullptr, "How strong the light shafts are at night." },
	{ "Shaders.GodRays.Main", "RayLength", "Light shaft length", nullptr, "How long the light shafts stretch across the screen." },
	{ "Shaders.GodRays.Main", "RayIntensity", "Light shaft brightness", nullptr, "How bright the light shafts are." },
	{ "Shaders.GodRays.Main", "RayVisibility", "Light shaft contrast", nullptr, "Contrast of the shafts. Higher makes them sharper and fewer." },
	{ "Shaders.GodRays.Main", "TimeEnabled", "Strongest at sunrise and sunset", nullptr, "Shafts are strongest at sunrise and sunset and fade away at noon." },
	// GodRays (Coloring)
	{ "Shaders.GodRays.Coloring", "RayR", "Light shaft colour", nullptr, "The colour used for the light shafts." },
	{ "Shaders.GodRays.Coloring", "Saturate", "Use this colour (0 = sun colour)", nullptr, "How much of your colour is used. Zero takes the sun's colour, full uses only yours." },
	// Sandstorm (Main)
	{ "Shaders.Sandstorm.Main", "Visibility", "Visibility in the storm", nullptr, "How far you can see in a full storm. Lower is a thicker storm." },
	{ "Shaders.Sandstorm.Main", "Billowing", "Cloudiness of the dust", nullptr, "Zero is an even haze, higher gives rolling dust clouds with clearer gaps." },
	{ "Shaders.Sandstorm.Main", "StreakStrength", "Blowing sand streaks", nullptr, "Thin streaks of sand blown past the camera. Zero turns them off." },
	{ "Shaders.Sandstorm.Main", "DustColorR", "Dust colour", nullptr, "The colour of the dust." },
	{ "Shaders.Sandstorm.Main", "Brightness", "Dust brightness", nullptr, "How brightly lit the dust is." },
	{ "Shaders.Sandstorm.Main", "WindSpeed", "Dust speed", nullptr, "How fast the dust blows in strong wind." },
	{ "Shaders.Sandstorm.Main", "GrassWind", "Grass sway in the storm", nullptr, "How hard grass sways and flutters in the storm." },
	{ "Shaders.Sandstorm.Main", "TreeWind", "Tree sway in the storm", nullptr, "How hard tree leaves swing in the storm." },
	// HeatHaze (Main)
	{ "Shaders.HeatHaze.Main", "Strength", "Haze strength", nullptr, "How much the distant ground wobbles in the heat. It builds through the day and is gone at night." },
	{ "Shaders.HeatHaze.Main", "StartDistance", "Nearest distance with haze", nullptr, "Nothing nearer than this shimmers, so close things never waver." },
	{ "Shaders.HeatHaze.Main", "NoiseScale", "Ripple fineness", nullptr, "Size of the ripples. Higher gives finer, busier ripples." },
	{ "Shaders.HeatHaze.Main", "Speed", "Ripple speed", nullptr, "How fast the ripples rise." },
	{ "Shaders.HeatHaze.Main", "PeakHour", "Hottest hour of the day", nullptr, "The hottest hour of the day, when the haze is strongest." },
	{ "Shaders.HeatHaze.Main", "CloudyScale", "Haze in cloudy weather", nullptr, "How much haze is left in cloudy weather compared with a clear day." },
	// Precipitations (Main)
	{ "Shaders.Precipitations.Main", "Opacity", "Rain visibility", nullptr, "How solid and visible the raindrops are." },
	{ "Shaders.Precipitations.Main", "Speed", "Rain speed", nullptr, "How fast the rain falls." },
	{ "Shaders.Precipitations.Main", "VerticalScale", "Rain streak length", nullptr, "Longer values draw rain as longer streaks." },
	{ "Shaders.Precipitations.Main", "Refraction", "Drop distortion", nullptr, "How much raindrops bend and distort the view behind them." },
	{ "Shaders.Precipitations.Main", "Coloring", "Rain brightness", nullptr, "Makes the rain brighter and more visible." },
	// Snow (Main)
	{ "Shaders.Snow.Main", "Flakes", "Snowfall amount", nullptr, "How many snowflakes fall." },
	{ "Shaders.Snow.Main", "Speed", "Snowfall speed", nullptr, "How fast the snow falls." },
	// SnowAccumulation (Main)
	{ "Shaders.SnowAccumulation.Main", "SnowColorR", "Snow colour", nullptr, "The colour of the snow lying on the ground." },
	{ "Shaders.SnowAccumulation.Main", "SunPower", "Snow brightness in sunlight", nullptr, "How brightly the sun lights the snow." },
	{ "Shaders.SnowAccumulation.Main", "Increase", "Hours for snow to build up", nullptr, "Game hours of snowfall before the ground is covered. Lower is faster." },
	{ "Shaders.SnowAccumulation.Main", "Decrease", "Hours for snow to melt", nullptr, "Game hours before the snow melts away. Higher keeps it longer." },
	{ "Shaders.SnowAccumulation.Main", "Persistent", "Snow stays where it fell (spot by spot)", nullptr, "Snow builds up and melts spot by spot, so sheltered ground stays clear. Needs snowfall." },
	{ "Shaders.SnowAccumulation.Main", "PersistentFootprints", "Footprints in snow", nullptr, "Your steps leave footprints in the snow." },
	{ "Shaders.SnowAccumulation.Main", "PersistentDrifts", "Snow drifts in hollows", nullptr, "More snow gathers in hollows and against walls, less on ridges." },
	// WetWorld (Main)
	{ "Shaders.WetWorld.Main", "Persistent", "Ground stays wet where rain fell", nullptr, "Rain wets only the ground it reaches, so covered spots stay dry and puddles form in the open." },
	{ "Shaders.WetWorld.Main", "PersistentWetTime", "Hours of rain to wet the ground", nullptr, "Game hours of rain before open ground is fully wet." },
	{ "Shaders.WetWorld.Main", "PersistentDryTime", "Hours for wet ground to dry", nullptr, "Game hours for wet ground to dry in full sun. Shade takes longer." },
	// Water (Default)
	{ "Shaders.Water.Default", "reflectivity", "Reflections", nullptr, "How strongly the water mirrors its surroundings." },
	{ "Shaders.Water.Default", "choppiness", "Wave sharpness", nullptr, "Higher gives sharper, more defined wave crests." },
	{ "Shaders.Water.Default", "waveSpeed", "Wave speed", nullptr, "How fast the waves move." },
	{ "Shaders.Water.Default", "waveWidth", "Wave size", nullptr, "Bigger values make larger waves; smaller values make fine ripples." },
	{ "Shaders.Water.Default", "turbidity", "Water murkiness", nullptr, "Higher makes the water murkier." },
	{ "Shaders.Water.Default", "refractionPower", "Refraction", nullptr, "How much the water bends the view of what lies beneath." },
	{ "Shaders.Water.Default", "causticsStrength", "Underwater light patterns", nullptr, "Rippling bright patterns of light seen when you are underwater." },
	{ "Shaders.Water.Default", "depthDarkness", "Darkness with depth", nullptr, "How dark the water gets as it gets deeper." },
	// Water (Interiors)
	{ "Shaders.Water.Interiors", "reflectivity", "Reflections", nullptr, "How strongly the water mirrors its surroundings." },
	{ "Shaders.Water.Interiors", "choppiness", "Wave sharpness", nullptr, "Higher gives sharper, more defined wave crests." },
	{ "Shaders.Water.Interiors", "waveSpeed", "Wave speed", nullptr, "How fast the waves move." },
	{ "Shaders.Water.Interiors", "waveWidth", "Wave size", nullptr, "Bigger values make larger waves; smaller values make fine ripples." },
	{ "Shaders.Water.Interiors", "turbidity", "Water murkiness", nullptr, "Higher makes the water murkier." },
	{ "Shaders.Water.Interiors", "refractionPower", "Refraction", nullptr, "How much the water bends the view of what lies beneath." },
	{ "Shaders.Water.Interiors", "depthDarkness", "Darkness with depth", nullptr, "How dark the water gets as it gets deeper." },
	{ "Shaders.Water.Interiors", "shoreFactor", "Shore fade width", nullptr, "How wide the soft fade is where the water meets a shallow shore." },
	// Water (Placed)
	{ "Shaders.Water.Placed", "reflectivity", "Reflections", nullptr, "How strongly the water mirrors its surroundings." },
	{ "Shaders.Water.Placed", "choppiness", "Wave sharpness", nullptr, "Higher gives sharper, more defined wave crests." },
	{ "Shaders.Water.Placed", "waveSpeed", "Wave speed", nullptr, "How fast the waves move." },
	{ "Shaders.Water.Placed", "waveWidth", "Wave size", nullptr, "Bigger values make larger waves; smaller values make fine ripples." },
	{ "Shaders.Water.Placed", "turbidity", "Water murkiness", nullptr, "Higher makes the water murkier." },
	{ "Shaders.Water.Placed", "refractionPower", "Refraction", nullptr, "How much the water bends the view of what lies beneath." },
	{ "Shaders.Water.Placed", "depthDarkness", "Darkness with depth", nullptr, "How dark the water gets as it gets deeper." },
	{ "Shaders.Water.Placed", "shoreFactor", "Shore fade width", nullptr, "How wide the soft fade is where the water meets a shallow shore." },
	// WaterLens (Main)
	{ "Shaders.WaterLens.Main", "Amount", "Water on the lens", nullptr, "How much water clings to the camera after you leave the water." },
	{ "Shaders.WaterLens.Main", "Viscosity", "Droplet thickness", nullptr, "Higher makes the droplets thicker and slower to run." },
	// Terrain (Main)
	{ "Shaders.Terrain.Main", "UsePBR", "Realistic ground lighting", nullptr, "Realistic lighting on the ground. Off gives the game's own ground lighting." },
	{ "Shaders.Terrain.Main", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.Main", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.Main", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.Main", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.Main", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.Main", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (Rain)
	{ "Shaders.Terrain.Rain", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.Rain", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.Rain", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.Rain", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.Rain", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.Rain", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (Night)
	{ "Shaders.Terrain.Night", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.Night", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.Night", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.Night", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.Night", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.Night", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (NightRain)
	{ "Shaders.Terrain.NightRain", "Roughness", "Ground roughness (less shine)", nullptr, "Higher makes the ground duller with less shine; lower makes it shinier." },
	{ "Shaders.Terrain.NightRain", "Metallicness", "Ground metallic sheen", nullptr, "Adds a metal-like sheen to the ground." },
	{ "Shaders.Terrain.NightRain", "AmbientScale", "Ambient light on the ground", nullptr, "Brightens or darkens the soft background light on the ground." },
	{ "Shaders.Terrain.NightRain", "SkylightingScale", "Sky colour on the ground", nullptr, "Adds sky colour to shaded ground. Zero turns it off." },
	{ "Shaders.Terrain.NightRain", "LightingScale", "Sun and lamp light on the ground", nullptr, "Brightens or darkens the ground where sun or lamps hit it." },
	{ "Shaders.Terrain.NightRain", "TerrainSaturation", "Ground colour strength", nullptr, "Higher gives richer ground colours; lower fades them toward grey." },
	// Terrain (Parallax)
	{ "Shaders.Terrain.Parallax", "Enabled", "Ground depth (parallax)", nullptr, "Gives the ground a feeling of depth, so stones and ruts look raised as you move." },
	{ "Shaders.Terrain.Parallax", "Shadows", "Self-shadowing in the ground", nullptr, "Raised bits of ground shade their own surroundings." },
	{ "Shaders.Terrain.Parallax", "ShadowsIntensity", "Ground self-shadow strength", nullptr, "How dark those ground shadows are." },
	{ "Shaders.Terrain.Parallax", nullptr, "Quality",
		"Low: HighQuality=false MaxDistance=1024 | Medium: HighQuality=false MaxDistance=2048 | High: HighQuality=true MaxDistance=2048 | Ultra: HighQuality=true MaxDistance=4096",
		"How finely and how far away the ground depth is drawn. Ultra reaches twice as far and costs the most frame rate." },
	// Terrain (Blending)
	{ "Shaders.Terrain.Blending", "Enabled", "Blend objects into the ground", nullptr, "Objects sink softly into the ground where they meet it. Needs a game restart to turn on." },
	{ "Shaders.Terrain.Blending", "BlendDistance", "Blend depth", nullptr, "How deep an object can sink into the ground before it fades away." },
	{ "Shaders.Terrain.Blending", "HeightBlend", "Follow ground texture detail", nullptr, "Makes the blend edge follow the ground texture, with sand reaching over the object." },
	{ "Shaders.Terrain.Blending", "MaxDistance", "Blend distance", nullptr, "How far away the blending still works." },
	// Terrain (Erosion)
	{ "Shaders.Terrain.Erosion", "Enabled", "Gullies on distant slopes", nullptr, "Adds gullies to the shading of distant slopes and mountains." },
	{ "Shaders.Terrain.Erosion", "Depth", "Gully depth", nullptr, "How deep the gullies look." },
	{ "Shaders.Terrain.Erosion", "GullyDarkening", "Gully darkness", nullptr, "How much darker the gully floors are than the crests." },
	{ "Shaders.Terrain.Erosion", "Spacing", "Gully spacing", nullptr, "Distance between the widest gullies. Smaller packs them tighter." },
	// Terrain (LOD)
	{ "Shaders.Terrain.LOD", "LODNoiseScale", "Distant land noise amount", nullptr, "How much noise is added to distant land to break up flat colour." },
	{ "Shaders.Terrain.LOD", "LODNoiseTile", "Distant land noise detail", nullptr, "Higher makes the noise finer; lower makes it broader." },
	// Terrain (Variation)
	{ "Shaders.Terrain.Variation", "Enabled", "Less repeating ground", nullptr, "Breaks up the repeating look of the ground so the desert stops looking tiled." },
	{ "Shaders.Terrain.Variation", "Layers", "Ground textures varied", nullptr, "How many ground textures per patch get the variation. More costs a little. Needs a restart." },
	{ "Shaders.Terrain.Variation", "Scale", "Variation patch density", nullptr, "Higher changes the pattern more often; lower makes bigger patches." },
	{ "Shaders.Terrain.Variation", "NormalMaps", "Vary ground bumps too", nullptr, "The ground bumps vary too. Off varies colour only and is cheaper." },
	{ "Shaders.Terrain.Variation", "StartDistance", "Variation starts this far away", nullptr, "How far from you the variation begins. Zero means everywhere." },
	// Terrain (Macro)
	{ "Shaders.Terrain.Macro", "Enabled", "Large-scale ground colour variation", nullptr, "Slow changes in ground brightness and tint over hundreds of metres, such as paler sand here and darker gravel there." },
	{ "Shaders.Terrain.Macro", "Strength", "Strength", nullptr, "How much of the variation shows." },
	{ "Shaders.Terrain.Macro", "Size", "Patch size", nullptr, "How big each patch is. Smaller is busier; larger is broad regions." },
	{ "Shaders.Terrain.Macro", "Brightness", "Brightness change", nullptr, "How much lighter or darker the patches get." },
	{ "Shaders.Terrain.Macro", "Tint", "Warm and cool tint change", nullptr, "How much warmer or cooler the patches get." },
	// Terrain (LODBlending)
	{ "Shaders.Terrain.LODBlending", "Enabled", "Match distant land to near land", nullptr, "Adjusts distant land so it matches the ground near you." },
	{ "Shaders.Terrain.LODBlending", "Brightness", "Distant land brightness", nullptr, "Brightens distant land if it looks darker than the ground near you." },
	{ "Shaders.Terrain.LODBlending", "Gamma", "Distant land gamma", nullptr, "Above one darkens the dark parts of distant land for more contrast." },
	// Terrain (Displacement)
	{ "Shaders.Terrain.Displacement", "Enabled", "Real 3D bumps on the ground", nullptr, "Real 3D bumps on the ground, not just painted shading. Needs the DXVK fork and costs frame rate." },
	{ "Shaders.Terrain.Displacement", "Strength", "Bump height", nullptr, "How tall the bumps are, from lowest to highest point." },
	{ "Shaders.Terrain.Displacement", "BumpSize", "Smallest bump size", nullptr, "The smallest bump the ground gets. Larger leaves out fine grain." },
	{ "Shaders.Terrain.Displacement", nullptr, "Quality",
		"Low: EdgeLength=24.0 MaxFactor=8 | Medium: EdgeLength=16.0 MaxFactor=16 | High: EdgeLength=12.0 MaxFactor=16 | Ultra: EdgeLength=8.0 MaxFactor=32",
		"How finely the ground is cut up for the bumps. Finer bumps look better and cost more frame rate." },
	{ "Shaders.Terrain.Displacement", nullptr, "Distance",
		"Near: FadeStart=512.0 FadeEnd=1024.0 | Medium: FadeStart=1024.0 FadeEnd=2048.0",
		"How far from you the bumps reach before they fade out. Medium costs more than Near." },
	{ "Shaders.Terrain.Displacement", "CastShadows", "Bumps cast shadows", nullptr, "The bumps throw small sun shadows, so stones and ruts look real." },
	{ "Shaders.Terrain.Displacement", "BendNormals", "Light bumps as real slopes", nullptr, "Bumps are lit as slopes: the side facing the sun is brighter, the other darker." },
	// POM (Main)
	{ "Shaders.POM.Main", "HeightMapScale", "Rock and wall depth (parallax)", nullptr, "How deep rocks and walls with depth maps look." },
	// POM (Displacement)
	{ "Shaders.POM.Displacement", "Enabled", "Real 3D bumps on rocks and cliffs", nullptr, "Real 3D bumps on rocks and cliffs, not just painted shading. Needs the DXVK fork and costs frame rate." },
	{ "Shaders.POM.Displacement", "Strength", "Bump height", nullptr, "How tall the bumps on rocks and cliffs are." },
	{ "Shaders.POM.Displacement", nullptr, "Quality",
		"Low: EdgePixels=24.0 MaxFactor=8 | Medium: EdgePixels=16.0 MaxFactor=16 | High: EdgePixels=12.0 MaxFactor=16 | Ultra: EdgePixels=8.0 MaxFactor=32",
		"How finely rocks and cliffs are cut up for the bumps. Finer bumps look better and cost more frame rate." },
	{ "Shaders.POM.Displacement", nullptr, "Distance",
		"Near: FadeStart=512.0 FadeEnd=1024.0 | Medium: FadeStart=1024.0 FadeEnd=2048.0",
		"How far from you the bumps reach before they fade out. Medium costs more than Near." },
	// Grass (Main)
	{ "Shaders.Grass.Main", "CastShadows", "Grass casts shadows", nullptr, "Grass throws sun shadows on the ground." },
	{ "Shaders.Grass.Main", "CastShadowsReach", "Grass shadow distance", nullptr, "How far from you the grass casts shadows." },
	// Grass (GPUGrass)
	{ "Shaders.Grass.GPUGrass", "Enabled", "Strand grass (a different grass renderer: needs the DXVK fork)", nullptr, "Draws grass as many thin strands instead of the game's flat cards. Looks very different. Needs the DXVK fork." },
	{ "Shaders.Grass.GPUGrass", "KeepCards", "Keep the game's flat grass as well (fuller)", nullptr, "Keeps the game's flat grass and adds strands around it for fuller clumps." },
	{ "Shaders.Grass.GPUGrass", "Density", "Grass density", nullptr, "More strand clumps beside each grass tuft. Denser fields cost more." },
	{ "Shaders.Grass.GPUGrass", nullptr, "Quality",
		"Low: Blades=24 BladesFar=4 | Medium: Blades=40 BladesFar=6 | High: Blades=64 BladesFar=8 | Ultra: Blades=96 BladesFar=12",
		"How many strands make up each clump of strand grass, near and far. More is fuller and costs more." },
	{ "Shaders.Grass.GPUGrass", "BladeDistance", "Strand grass distance", nullptr, "How far away strand grass is drawn. Beyond it the game's flat grass is used." },
	{ "Shaders.Grass.GPUGrass", "HeightScale", "Strand grass height", nullptr, "How tall the strands are compared to the plant." },
	// Grass (Wind)
	{ "Shaders.Grass.Wind", "Enabled", "Grass moves in waves of wind", nullptr, "Grass moves like a field in the wind, with gusts rolling across it." },
	{ "Shaders.Grass.Wind", "GustStrength", "Gust strength", nullptr, "How far a passing gust leans the grass." },
	{ "Shaders.Grass.Wind", "GustSize", "Gust size", nullptr, "Distance between gusts. Larger gives broad slow waves; smaller gives choppy ones." },
	{ "Shaders.Grass.Wind", "GustSpeed", "Gust speed", nullptr, "How fast gusts roll across the grass." },
	{ "Shaders.Grass.Wind", "Flutter", "Small quiver", nullptr, "Each tuft's own small quiver, so neighbours never move in step." },
	// Plants (Wind)
	{ "Shaders.Plants.Wind", "Enabled", "Plants move in the wind", nullptr, "Plants and bushes move in the same wind and gusts as the grass." },
	{ "Shaders.Plants.Wind", "Bend", "Plant sway", nullptr, "How far a gust leans the top of a plant." },
	{ "Shaders.Plants.Wind", "LeafGust", "Bush leaf sway in gusts", nullptr, "How much gusts strengthen the swing of bush leaves." },
	{ "Shaders.Plants.Wind", "LeafFlutter", "Bush leaf flutter", nullptr, "Extra leaf flutter on bushes in the wind." },
	// Foliage (Main)
	{ "Shaders.Foliage.Main", "Translucency", "Light shining through leaves and grass", nullptr, "Grass and leaves glow softly when the sun is behind them. Zero is off." },
	{ "Shaders.Foliage.Main", "Power", "Glow tightness (how close to the sun)", nullptr, "Higher shows the glow only when looking almost straight at the sun." },
	// Particles (Main)
	{ "Shaders.Particles.Main", "Strength", "Lighting on smoke, dust and blood", nullptr, "Smoke, dust and blood are lit by the scene instead of looking flat. Zero is the game's look." },
	{ "Shaders.Particles.Main", "SoftScale", "Soft smoke and dust edges", nullptr, "Smoke, dust, blood and sparks fade where they meet the ground and walls instead of cutting a hard line. Zero is off. Needs particle lighting on at startup." },
	{ "Shaders.Particles.Main", "MeshSoftScale", "Soft glow and flash edges", nullptr, "The same for glows, lamp cards, muzzle flashes and dust storm walls. Lower it if big dust storms look too faint near the ground." },
	{ "Shaders.Particles.Main", "SunShare", "Sunlight on particles", nullptr, "How much sunlight particles catch outdoors. Shaded ones get only ambient light and lamps." },
	// GPUParticles (Main)
	{ "Shaders.GPUParticles.Main", "Enabled", "Rain and snow drawn on the graphics card (needs the DXVK fork)", nullptr, "Draws rain and snow as many more drops and flakes outdoors. Needs the DXVK fork." },
	{ "Shaders.GPUParticles.Main", nullptr, "Quality",
		"Low: Count=16384 Radius=1000 | Medium: Count=32768 Radius=1200 | High: Count=65536 Radius=1600",
		"How many drops or flakes fall and how far around you. More is denser and costs more." },
	{ "Shaders.GPUParticles.Main", "Size", "Drop and flake size", nullptr, "How big the drops, splashes and flakes are." },
	{ "Shaders.GPUParticles.Main", "Speed", "Fall speed", nullptr, "How fast the drops and flakes fall." },
	{ "Shaders.GPUParticles.Main", "Opacity", "Visibility", nullptr, "How strongly the drops and flakes show." },
	{ "Shaders.GPUParticles.Main", "Splashes", "Splashes where drops land", nullptr, "Rain drops make a small splash where they land; snow settles briefly." },
	// Tonemapping (Main)
	{ "Shaders.Tonemapping.Main", nullptr, "Tone mapper",
		"None (the game's own): TonemappingMode=0 | VT Lottes: TonemappingMode=1 | Lottes (tuned): TonemappingMode=2 | Reinhard: TonemappingMode=3 | Reinhard Jodie: TonemappingMode=4 | ACES Filmic: TonemappingMode=5 | ACES Fitted: TonemappingMode=6 | Uncharted 2: TonemappingMode=7 | Uchimura (GT): TonemappingMode=8 | AGX: TonemappingMode=9 | DICE (default): TonemappingMode=10 | Perceptual (Gran Turismo style): TonemappingMode=11",
		"How bright light is fitted onto your screen. DICE is neutral; ACES and Uncharted 2 are punchier and more filmic; None keeps the game's look." },
	{ "Shaders.Tonemapping.Main", "Exposure", "Brightness", nullptr, "Makes the whole picture brighter or darker." },
	{ "Shaders.Tonemapping.Main", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer, higher is more vivid." },
	{ "Shaders.Tonemapping.Main", "SkyMultiplier", "Sky brightness", nullptr, "Makes the sky and sun brighter or dimmer. Needs the sky effect on." },
	{ "Shaders.Tonemapping.Main", "NightShiftEnabled", "Night colour shift (dim areas turn pale and blue)", nullptr, "In dark places the picture loses colour and turns pale and blue, as your eyes do in the dark." },
	{ "Shaders.Tonemapping.Main", "NightShift", "Night colour shift amount", nullptr, "How strongly dark areas lose colour and turn blue. Used when the night colour shift is on." },
	// Tonemapping (Interiors)
	{ "Shaders.Tonemapping.Interiors", nullptr, "Tone mapper",
		"None (the game's own): TonemappingMode=0 | VT Lottes: TonemappingMode=1 | Lottes (tuned): TonemappingMode=2 | Reinhard: TonemappingMode=3 | Reinhard Jodie: TonemappingMode=4 | ACES Filmic: TonemappingMode=5 | ACES Fitted: TonemappingMode=6 | Uncharted 2: TonemappingMode=7 | Uchimura (GT): TonemappingMode=8 | AGX: TonemappingMode=9 | DICE (default): TonemappingMode=10 | Perceptual (Gran Turismo style): TonemappingMode=11",
		"How bright light is fitted onto your screen. DICE is neutral; ACES and Uncharted 2 are punchier and more filmic; None keeps the game's look." },
	{ "Shaders.Tonemapping.Interiors", "Exposure", "Brightness", nullptr, "Makes the whole picture brighter or darker." },
	{ "Shaders.Tonemapping.Interiors", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer, higher is more vivid." },
	{ "Shaders.Tonemapping.Interiors", "NightShift", "Night colour shift amount", nullptr, "How strongly dark areas lose colour and turn blue. Used when the night colour shift is on." },
	// Tonemapping (Night)
	{ "Shaders.Tonemapping.Night", "Exposure", "Brightness", nullptr, "Makes the whole picture brighter or darker." },
	{ "Shaders.Tonemapping.Night", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer, higher is more vivid." },
	{ "Shaders.Tonemapping.Night", "SkyMultiplier", "Sky brightness", nullptr, "Makes the sky and sun brighter or dimmer. Needs the sky effect on." },
	{ "Shaders.Tonemapping.Night", "NightShift", "Night colour shift amount", nullptr, "How strongly dark areas lose colour and turn blue. Used when the night colour shift is on." },
	// Exposure (Main)
	{ "Shaders.Exposure.Main", nullptr, "Auto exposure",
		"Classic: Mode=0 | Eye adaptation: Mode=1",
		"Classic: the older brightness correction. Eye adaptation: brightness adjusts like an eye, with automatic white balance. The rows below only affect eye adaptation." },
	{ "Shaders.Exposure.Main", "AdaptExposure", "Eye adaptation strength", nullptr, "How much the brightness adjusts as you move between light and dark. Only with eye adaptation." },
	{ "Shaders.Exposure.Main", "AdaptWhiteBalance", "Automatic white balance", nullptr, "Automatically removes warm or blue tints from the light, as your eyes do. Only with eye adaptation." },
	{ "Shaders.Exposure.Main", "DarkAdaptSpeed", "Speed of adjusting to dark", nullptr, "How fast the picture brightens when you go somewhere darker. Higher is faster." },
	{ "Shaders.Exposure.Main", "LightAdaptSpeed", "Speed of adjusting to bright", nullptr, "How fast the picture darkens when you go somewhere brighter. Higher is faster." },
	// Exposure (Interiors)
	{ "Shaders.Exposure.Interiors", "AdaptExposure", "Eye adaptation strength", nullptr, "How much the brightness adjusts as you move between light and dark. Only with eye adaptation." },
	{ "Shaders.Exposure.Interiors", "AdaptWhiteBalance", "Automatic white balance", nullptr, "Automatically removes warm or blue tints from the light, as your eyes do. Only with eye adaptation." },
	{ "Shaders.Exposure.Interiors", "DarkAdaptSpeed", "Speed of adjusting to dark", nullptr, "How fast the picture brightens when you go somewhere darker. Higher is faster." },
	{ "Shaders.Exposure.Interiors", "LightAdaptSpeed", "Speed of adjusting to bright", nullptr, "How fast the picture darkens when you go somewhere brighter. Higher is faster." },
	// Exposure (Night)
	{ "Shaders.Exposure.Night", "AdaptExposure", "Eye adaptation strength", nullptr, "How much the brightness adjusts as you move between light and dark. Only with eye adaptation." },
	{ "Shaders.Exposure.Night", "AdaptWhiteBalance", "Automatic white balance", nullptr, "Automatically removes warm or blue tints from the light, as your eyes do. Only with eye adaptation." },
	{ "Shaders.Exposure.Night", "DarkAdaptSpeed", "Speed of adjusting to dark", nullptr, "How fast the picture brightens when you go somewhere darker. Higher is faster." },
	{ "Shaders.Exposure.Night", "LightAdaptSpeed", "Speed of adjusting to bright", nullptr, "How fast the picture darkens when you go somewhere brighter. Higher is faster." },
	// LocalExposure (Main)
	{ "Shaders.LocalExposure.Main", "Shadows", "Lift dark areas", nullptr, "Brightens very dark parts of the picture so shaded areas show more detail." },
	{ "Shaders.LocalExposure.Main", "Highlights", "Calm bright areas", nullptr, "Tones down very bright parts, such as bright sand or a doorway to the outside." },
	{ "Shaders.LocalExposure.Main", "Blur", "Area size", nullptr, "How large the areas are that get evened out. Small reaches finer detail but can leave halos." },
	{ "Shaders.LocalExposure.Main", "Interiors", "Also indoors", nullptr, "Also applies the effect indoors." },
	{ "Shaders.LocalExposure.Main", "Exteriors", "Also outdoors", nullptr, "Also applies the effect outdoors." },
	// HDR (Main)
	{ "Shaders.HDR.Main", "Enabled", "HDR output", nullptr, "Sends a true HDR picture to your display, with brighter highlights. Needs an HDR screen with HDR on in Windows and the DXVK fork." },
	{ "Shaders.HDR.Main", nullptr, "HDR format",
		"HDR10: Mode=1 | scRGB: Mode=2",
		"HDR10 is what most HDR games use. Try the other only if HDR10 looks wrong." },
	{ "Shaders.HDR.Main", "PaperWhiteNits", "Normal white brightness (nits)", nullptr, "How bright normal white, the HUD and menus are on an HDR screen. Match your Windows setting." },
	{ "Shaders.HDR.Main", "PeakNits", "Peak brightness (nits, 0 = automatic)", nullptr, "The brightest a highlight may get. Zero uses your display's own maximum." },
	{ "Shaders.HDR.Main", "HighlightStrength", "Highlight strength", nullptr, "How much brighter than normal white the highlights go. Zero gives a normal picture." },
	{ "Shaders.HDR.Main", "Gamma22", "Darker, deeper shadows (2.2 gamma)", nullptr, "Slightly darker, deeper shadows, like many standard monitors show." },
	// Bloom (Main)
	{ "Shaders.Bloom.Main", "Strength", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.Bloom.Main", nullptr, "Quality",
		"Low: Passes=4 | Medium: Passes=6 | High: Passes=8",
		"Higher gives a smoother glow and costs a little more." },
	{ "Shaders.Bloom.Main", "Spread", "Glow size", nullptr, "Makes glows wider and softer. Higher spreads light further around bright objects." },
	{ "Shaders.Bloom.Main", "GlowBoost", "Extra glow from bright lights", nullptr, "Makes bright lights such as neon signs, lamps and fire glow more than the rest of the picture." },
	// Bloom (Night)
	{ "Shaders.Bloom.Night", "Strength", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.Bloom.Night", nullptr, "Quality",
		"Low: Passes=4 | Medium: Passes=6 | High: Passes=8",
		"Higher gives a smoother glow and costs a little more." },
	{ "Shaders.Bloom.Night", "Spread", "Glow size", nullptr, "Makes glows wider and softer. Higher spreads light further around bright objects." },
	{ "Shaders.Bloom.Night", "GlowBoost", "Extra glow from bright lights", nullptr, "Makes bright lights such as neon signs, lamps and fire glow more than the rest of the picture." },
	// Bloom (Interiors)
	{ "Shaders.Bloom.Interiors", "Strength", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.Bloom.Interiors", nullptr, "Quality",
		"Low: Passes=4 | Medium: Passes=6 | High: Passes=8",
		"Higher gives a smoother glow and costs a little more." },
	{ "Shaders.Bloom.Interiors", "Spread", "Glow size", nullptr, "Makes glows wider and softer. Higher spreads light further around bright objects." },
	{ "Shaders.Bloom.Interiors", "GlowBoost", "Extra glow from bright lights", nullptr, "Makes bright lights such as neon signs, lamps and fire glow more than the rest of the picture." },
	// BloomLegacy (Exteriors)
	{ "Shaders.BloomLegacy.Exteriors", "BloomIntensity", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.BloomLegacy.Exteriors", "BloomSaturation", "Glow colour strength", nullptr, "How colourful the glow is." },
	{ "Shaders.BloomLegacy.Exteriors", "OriginalIntensity", "Picture brightness", nullptr, "Brightness of the picture itself under the glow." },
	{ "Shaders.BloomLegacy.Exteriors", "OriginalSaturation", "Picture colour strength", nullptr, "Colour strength of the picture itself under the glow." },
	// BloomLegacy (Interiors)
	{ "Shaders.BloomLegacy.Interiors", "BloomIntensity", "Glow strength", nullptr, "How strongly bright things glow." },
	{ "Shaders.BloomLegacy.Interiors", "BloomSaturation", "Glow colour strength", nullptr, "How colourful the glow is." },
	{ "Shaders.BloomLegacy.Interiors", "OriginalIntensity", "Picture brightness", nullptr, "Brightness of the picture itself under the glow." },
	{ "Shaders.BloomLegacy.Interiors", "OriginalSaturation", "Picture colour strength", nullptr, "Colour strength of the picture itself under the glow." },
	// Lens (Main)
	{ "Shaders.Lens.Main", "Strength", "Lens dirt strength", nullptr, "How visible the dirt on the camera lens is when you look at bright lights." },
	{ "Shaders.Lens.Main", "Smudginess", "Dirt coverage", nullptr, "How much of the screen the lens dirt covers." },
	// Lens (Night)
	{ "Shaders.Lens.Night", "Strength", "Lens dirt strength", nullptr, "How visible the dirt on the camera lens is when you look at bright lights." },
	{ "Shaders.Lens.Night", "Smudginess", "Dirt coverage", nullptr, "How much of the screen the lens dirt covers." },
	// Lens (Interiors)
	{ "Shaders.Lens.Interiors", "Strength", "Lens dirt strength", nullptr, "How visible the dirt on the camera lens is when you look at bright lights." },
	{ "Shaders.Lens.Interiors", "Smudginess", "Dirt coverage", nullptr, "How much of the screen the lens dirt covers." },
	// LUT (Main)
	{ "Shaders.LUT.Main", "Strength", "Colour grade strength", nullptr, "How strongly the chosen colour grade is applied. Zero is off." },
	// Coloring (Default)
	{ "Shaders.Coloring.Default", "Strength", "Strength", nullptr, "How strongly the whole colour look is applied." },
	{ "Shaders.Coloring.Default", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.Coloring.Default", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.Coloring.Default", "Bleach", "Bleached look", nullptr, "A washed-out, desaturated film look with harder contrast." },
	{ "Shaders.Coloring.Default", "Fade", "Faded look", nullptr, "Lifts the blacks for a faded, hazy look." },
	// ImageAdjust (Main)
	{ "Shaders.ImageAdjust.Main", "Strength", "Strength", nullptr, "How strongly these picture adjustments are applied." },
	{ "Shaders.ImageAdjust.Main", "Brightness", "Brightness", nullptr, "Makes the picture lighter or darker." },
	{ "Shaders.ImageAdjust.Main", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.ImageAdjust.Main", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.ImageAdjust.Main", "DarkColorR", "Shadow colour", nullptr, "Tints the dark parts of the picture." },
	{ "Shaders.ImageAdjust.Main", "LightColorR", "Highlight colour", nullptr, "Tints the bright parts of the picture." },
	// ImageAdjust (Night)
	{ "Shaders.ImageAdjust.Night", "Strength", "Strength", nullptr, "How strongly these picture adjustments are applied." },
	{ "Shaders.ImageAdjust.Night", "Brightness", "Brightness", nullptr, "Makes the picture lighter or darker." },
	{ "Shaders.ImageAdjust.Night", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.ImageAdjust.Night", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.ImageAdjust.Night", "DarkColorR", "Shadow colour", nullptr, "Tints the dark parts of the picture." },
	{ "Shaders.ImageAdjust.Night", "LightColorR", "Highlight colour", nullptr, "Tints the bright parts of the picture." },
	// ImageAdjust (Interiors)
	{ "Shaders.ImageAdjust.Interiors", "Strength", "Strength", nullptr, "How strongly these picture adjustments are applied." },
	{ "Shaders.ImageAdjust.Interiors", "Brightness", "Brightness", nullptr, "Makes the picture lighter or darker." },
	{ "Shaders.ImageAdjust.Interiors", "Contrast", "Contrast", nullptr, "Difference between dark and bright parts. Higher is punchier." },
	{ "Shaders.ImageAdjust.Interiors", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is greyer." },
	{ "Shaders.ImageAdjust.Interiors", "DarkColorR", "Shadow colour", nullptr, "Tints the dark parts of the picture." },
	{ "Shaders.ImageAdjust.Interiors", "LightColorR", "Highlight colour", nullptr, "Tints the bright parts of the picture." },
	// Cinema (Main)
	{ "Shaders.Cinema.Main", "AspectRatio", "Black bars (screen shape)", nullptr, "Adds black cinema bars at the top and bottom. Use 1 for no bars." },
	{ "Shaders.Cinema.Main", "FilmGrainAmount", "Film grain", nullptr, "Adds moving film grain, like a cinema picture." },
	{ "Shaders.Cinema.Main", "ChromaticAberration", "Colour fringes at the edges", nullptr, "Splits red and blue slightly towards the edges, like a real lens." },
	{ "Shaders.Cinema.Main", "VignetteDarkness", "Dark corners", nullptr, "Darkens the corners and edges of the screen." },
	{ "Shaders.Cinema.Main", "OverlayStrength", "Contrast and colour boost", nullptr, "Boosts colour and contrast for a richer, more filmic picture." },
	// Bodycam (Main)
	{ "Shaders.Bodycam.Main", "Fisheye", "Lens distortion", nullptr, "How much the wide lens bends straight lines outwards at the edges." },
	{ "Shaders.Bodycam.Main", "Vignette", "Dark corners", nullptr, "Darkens the corners of the picture." },
	{ "Shaders.Bodycam.Main", "Grain", "Camera noise", nullptr, "Adds camera noise, stronger in the dark." },
	{ "Shaders.Bodycam.Main", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is more washed out." },
	{ "Shaders.Bodycam.Main", "Text", "Timestamp", nullptr, "Shows a date and time stamp in the top right corner." },
	{ "Shaders.Bodycam.Main", "Bounce", "Camera bob", nullptr, "The picture bobs with each step and leans into turns, like a chest-mounted camera." },
	{ "Shaders.Bodycam.Main", "MotionBlur", "Motion smear", nullptr, "Smears the picture while you turn." },
	{ "Shaders.Bodycam.Main", "Mic", "Camera microphone sound", nullptr, "Makes all game sound like a cheap camera microphone. Costs no frame rate." },
	// Bodycam (Night)
	{ "Shaders.Bodycam.Night", "Fisheye", "Lens distortion", nullptr, "How much the wide lens bends straight lines outwards at the edges." },
	{ "Shaders.Bodycam.Night", "Vignette", "Dark corners", nullptr, "Darkens the corners of the picture." },
	{ "Shaders.Bodycam.Night", "Grain", "Camera noise", nullptr, "Adds camera noise, stronger in the dark." },
	{ "Shaders.Bodycam.Night", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is more washed out." },
	{ "Shaders.Bodycam.Night", "Text", "Timestamp", nullptr, "Shows a date and time stamp in the top right corner." },
	{ "Shaders.Bodycam.Night", "Bounce", "Camera bob", nullptr, "The picture bobs with each step and leans into turns, like a chest-mounted camera." },
	{ "Shaders.Bodycam.Night", "LowLight", "Low-light mode (black and white)", nullptr, "Near black and white night-vision look with lifted shadows and more grain." },
	{ "Shaders.Bodycam.Night", "MotionBlur", "Motion smear", nullptr, "Smears the picture while you turn." },
	// Bodycam (Interiors)
	{ "Shaders.Bodycam.Interiors", "Fisheye", "Lens distortion", nullptr, "How much the wide lens bends straight lines outwards at the edges." },
	{ "Shaders.Bodycam.Interiors", "Vignette", "Dark corners", nullptr, "Darkens the corners of the picture." },
	{ "Shaders.Bodycam.Interiors", "Grain", "Camera noise", nullptr, "Adds camera noise, stronger in the dark." },
	{ "Shaders.Bodycam.Interiors", "Saturation", "Colour strength", nullptr, "How rich the colours are. Lower is more washed out." },
	{ "Shaders.Bodycam.Interiors", "Text", "Timestamp", nullptr, "Shows a date and time stamp in the top right corner." },
	{ "Shaders.Bodycam.Interiors", "Bounce", "Camera bob", nullptr, "The picture bobs with each step and leans into turns, like a chest-mounted camera." },
	{ "Shaders.Bodycam.Interiors", "LowLight", "Low-light mode (black and white)", nullptr, "Near black and white night-vision look with lifted shadows and more grain." },
	{ "Shaders.Bodycam.Interiors", "MotionBlur", "Motion smear", nullptr, "Smears the picture while you turn." },
	// DepthOfField (FirstPersonView)
	{ "Shaders.DepthOfField.FirstPersonView", "Enabled", "Depth of field (background blur)", nullptr, "Blurs things in front of and behind what you look at." },
	{ "Shaders.DepthOfField.FirstPersonView", "BaseBlurRadius", "Blur strength", nullptr, "How strong the blur is." },
	{ "Shaders.DepthOfField.FirstPersonView", "Radius", "Blur size", nullptr, "How large the blur is." },
	{ "Shaders.DepthOfField.FirstPersonView", nullptr, "When it applies",
		"Always: Mode=0 | Not in dialogue: Mode=1 | Only in dialogue: Mode=2",
		"Whether the blur also applies while you talk to someone." },
	// DepthOfField (ThirdPersonView)
	{ "Shaders.DepthOfField.ThirdPersonView", "Enabled", "Depth of field (background blur)", nullptr, "Blurs things in front of and behind what you look at." },
	{ "Shaders.DepthOfField.ThirdPersonView", "BaseBlurRadius", "Blur strength", nullptr, "How strong the blur is." },
	{ "Shaders.DepthOfField.ThirdPersonView", "Radius", "Blur size", nullptr, "How large the blur is." },
	{ "Shaders.DepthOfField.ThirdPersonView", nullptr, "When it applies",
		"Always: Mode=0 | Not in dialogue: Mode=1 | Only in dialogue: Mode=2",
		"Whether the blur also applies while you talk to someone." },
	// DepthOfField (VanityView)
	{ "Shaders.DepthOfField.VanityView", "Enabled", "Depth of field (background blur)", nullptr, "Blurs things in front of and behind what you look at." },
	{ "Shaders.DepthOfField.VanityView", "BaseBlurRadius", "Blur strength", nullptr, "How strong the blur is." },
	{ "Shaders.DepthOfField.VanityView", "Radius", "Blur size", nullptr, "How large the blur is." },
	{ "Shaders.DepthOfField.VanityView", nullptr, "When it applies",
		"Always: Mode=0 | Not in dialogue: Mode=1 | Only in dialogue: Mode=2",
		"Whether the blur also applies while you talk to someone." },
	// DepthOfField (Bokeh)
	{ "Shaders.DepthOfField.Bokeh", "Bokeh", "Bokeh (bright lights blur into shapes)", nullptr, "Bright out-of-focus lights become soft shapes. Needs depth of field on and the DXVK fork." },
	{ "Shaders.DepthOfField.Bokeh", nullptr, "Bokeh shape",
		"Round: BokehShape=0 | Pentagon: BokehShape=5 | Hexagon: BokehShape=6 | Heptagon: BokehShape=7 | Octagon: BokehShape=8",
		"The shape of the blurred bright lights, like a camera lens." },
	{ "Shaders.DepthOfField.Bokeh", "BokehSize", "Bokeh size", nullptr, "How large the blurred light shapes are." },
	{ "Shaders.DepthOfField.Bokeh", "BokehIntensity", "Bokeh brightness", nullptr, "How bright the blurred light shapes are." },
	// MotionBlur (FirstPersonView)
	{ "Shaders.MotionBlur.FirstPersonView", "Enabled", "Motion blur", nullptr, "Blurs the picture while you turn or move quickly." },
	{ "Shaders.MotionBlur.FirstPersonView", "BlurScale", "Motion blur strength", nullptr, "How strong the motion blur is." },
	// MotionBlur (ThirdPersonView)
	{ "Shaders.MotionBlur.ThirdPersonView", "Enabled", "Motion blur", nullptr, "Blurs the picture while you turn or move quickly." },
	{ "Shaders.MotionBlur.ThirdPersonView", "BlurScale", "Motion blur strength", nullptr, "How strong the motion blur is." },
	// LowHF (Main)
	{ "Shaders.LowHF.Main", "BlurMultiplier", "Blur when hurt", nullptr, "How blurry the picture gets when you are badly hurt." },
	{ "Shaders.LowHF.Main", "DarknessMultiplier", "Darkening when hurt", nullptr, "How dark the picture gets when you are badly hurt." },
	{ "Shaders.LowHF.Main", "VignetteMultiplier", "Dark edges when hurt", nullptr, "How dark the screen edges get when you are badly hurt." },
	{ "Shaders.LowHF.Main", "HealthLimit", "Health level where it starts", nullptr, "The health level below which the hurt effect begins." },
	// TAA (Main)
	{ "Shaders.TAA.Main", "Sharpness", "Sharpness", nullptr, "Sharpens the picture to counter the softness anti-aliasing adds." },
	{ "Shaders.TAA.Main", "HistoryWeight", "Smoothness over time", nullptr, "Higher is smoother with less shimmer, but can leave slight ghosting on moving things." },
	{ "Shaders.TAA.Main", "WeaponTAA", "Smoothing on the weapon", nullptr, "How much anti-aliasing smoothing applies to your weapon and hands." },
	// TAA (Cutouts)
	{ "Shaders.TAA.Cutouts", "Enabled", "Smooth edges on leaves, grass and fences", nullptr, "Smooths the jagged edges of leaves, grass and fences. Needs a game restart." },
	// TAA (FSR)
	{ "Shaders.TAA.FSR", "Enabled", "FSR upscaler", nullptr, "AMD FSR upscaling and anti-aliasing. Needs the DXVK fork." },
	{ "Shaders.TAA.FSR", nullptr, "FSR version",
		"FSR 2.2: Version=0 | FSR 3.1: Version=1",
		"FSR 3.1 is the newer version and usually looks cleaner." },
	{ "Shaders.TAA.FSR", "RenderScale", "Upscaling quality", nullptr, "Draws the game at a lower resolution and scales it up. Lower runs faster but looks softer." },
	{ "Shaders.TAA.FSR", "Sharpness", "Sharpness", nullptr, "Sharpens the upscaled picture. Zero is off." },
	// TAA (DLSS)
	{ "Shaders.TAA.DLSS", "Enabled", "DLSS (NVIDIA RTX only)", nullptr, "NVIDIA DLSS upscaling and anti-aliasing. NVIDIA RTX cards only, needs the DXVK fork." },
	{ "Shaders.TAA.DLSS", "RenderScale", "Upscaling quality", nullptr, "Draws the game at a lower resolution and scales it up. Lower runs faster but looks softer." },
	{ "Shaders.TAA.DLSS", "RayReconstruction", "DLSS Ray Reconstruction", nullptr, "NVIDIA's cleaner denoising for ray tracing. NVIDIA RTX cards only, with DLSS on." },
	{ "Shaders.TAA.DLSS", "WeaponMask", "Weapon ghosting fix", nullptr, "Reduces ghosting and smearing on your weapon and hands with DLSS." },
	// SMAA (Main)
	{ "Shaders.SMAA.Main", nullptr, "Quality",
		"Low: MaxSearchSteps=16 | Medium: MaxSearchSteps=32 | High: MaxSearchSteps=64",
		"Higher smooths long, shallow edges better and costs a little more." },
	{ "Shaders.SMAA.Main", "Threshold", "Edge sensitivity", nullptr, "How easily edges are found. Lower smooths more of the picture but can soften detail." },
	// Sharpening (Main)
	{ "Shaders.Sharpening.Main", "Strength", "Sharpening strength", nullptr, "How much the picture is sharpened." },
	{ "Shaders.Sharpening.Main", "Offset", "Sharpening width", nullptr, "How wide the sharpening reaches. Wider looks coarser." },
	// Sharpening (Interiors)
	{ "Shaders.Sharpening.Interiors", "Strength", "Sharpening strength", nullptr, "How much the picture is sharpened." },
	{ "Shaders.Sharpening.Interiors", "Offset", "Sharpening width", nullptr, "How wide the sharpening reaches. Wider looks coarser." },
	// DitherBuster (Main)
	{ "Shaders.DitherBuster.Main", "Strength", "Strength", nullptr, "How strongly the dotted patterns on edges are smoothed out." },
	{ "Shaders.DitherBuster.Main", "Radius", "Smoothing reach", nullptr, "How far along edges the smoothing reaches." },
	// SkyDither (Main)
	{ "Shaders.SkyDither.Main", "Strength", "Banding removal strength", nullptr, "Hides visible bands in smooth skies with faint moving noise." },
	{ "Shaders.SkyDither.Main", "AtToneMap", "Also fix banding over the whole picture", nullptr, "Also removes banding in fog and dark gradients across the whole picture." },
	// Flashlight (Main)
	{ "Shaders.Flashlight.Main", "Dimmer", "Brightness", nullptr, "How bright the beam is. Its colour follows your Pip-Boy colour." },
	{ "Shaders.Flashlight.Main", "Distance", "Beam distance", nullptr, "How far the beam reaches. It fades out well before the end." },
	{ "Shaders.Flashlight.Main", "AttachToWeapon", "Follows your weapon", nullptr, "The beam points where your weapon points instead of where the camera looks." },
	// Flashlight (Deferred)
	{ "Shaders.Flashlight.Deferred", "Shadows", "Flashlight shadows", nullptr, "Things in front of the beam cast shadows. Costs some frame rate while the flashlight is on." },
	{ "Shaders.Flashlight.Deferred", nullptr, "Shadow quality",
		"Low: ShadowMapSize=1024 ShadowSoftness=3.0 | Medium: ShadowMapSize=2048 ShadowSoftness=5.0 | High: ShadowMapSize=4096 ShadowSoftness=8.0",
		"Higher gives sharper, softer shadow edges and uses more video memory. Needs a restart." },
	{ "Shaders.Flashlight.Deferred", "Falloff", "Beam fade with distance", nullptr, "Higher makes the beam fade out sooner and look shorter. Lower reaches further." },
	{ "Shaders.Flashlight.Deferred", "Cookie", "Lens pattern", nullptr, "Shows the flashlight lens pattern in the beam. Zero gives a clean, even beam." },
	{ "Shaders.Flashlight.Deferred", "GameLight", "Lights the world like a lamp", nullptr, "Leaves, your hands and shiny surfaces react to the flashlight the way they do to a lamp at night." },
	// Grass (Interaction)
	{ "Shaders.Grass.Interaction", "Enabled", "Grass bends as you walk", nullptr, "Grass pushes away from you as you walk through it and springs back afterwards." },
	{ "Shaders.Grass.Interaction", "Radius", "Bend distance", nullptr, "How far around your feet the grass bends." },
	{ "Shaders.Grass.Interaction", "StrengthStanding", "Bend strength (standing)", nullptr, "How much the grass is pushed aside while you stand still." },
	{ "Shaders.Grass.Interaction", "StrengthMax", "Bend strength (running)", nullptr, "How much the grass is pushed aside when you run." },
	{ "Shaders.Grass.Interaction", "SpringBackSeconds", "Recovery time", nullptr, "How slowly the grass stands back up after you pass. Higher leaves trails for longer." },
	// Grass (LOD)
	{ "Shaders.Grass.LOD", "Enabled", "Fade small grass by size", nullptr, "Grass fades out by how small it looks on screen, not just by distance. Saves frame rate in open fields." },
	{ "Shaders.Grass.LOD", "MinPixels", "Smallest grass kept", nullptr, "Grass smaller than this on screen disappears. Higher is faster, lower keeps distant grass longer." },
	{ "Shaders.Grass.LOD", "FadePixels", "Fade-out softness", nullptr, "How gradually small grass fades away. Higher avoids grass popping out." },
	// Grass (Lights)
	{ "Shaders.Grass.Lights", "Enabled", "Lamps light the grass", nullptr, "Lamps and your Pip-Boy light shine on nearby grass. Costs nothing where no lamp is near." },
	{ "Shaders.Grass.Lights", "MaxLights", "Lamps at once", nullptr, "The most lamps that can light the grass at the same time, nearest first." },
	{ "Shaders.Grass.Lights", "Strength", "Lamp light strength", nullptr, "How bright lamp light looks on the grass." },
	// Grass (NormalMaps)
	{ "Shaders.Grass.NormalMaps", "Enabled", "Grass surface detail", nullptr, "Grass textures that come with a detail map catch the sunlight with fine bumps." },
	{ "Shaders.Grass.NormalMaps", "Strength", "Detail strength", nullptr, "How strongly the fine bumps change the light on the grass." },
	{ "Shaders.Grass.NormalMaps", "EnableSpecular", "Sun shine on grass", nullptr, "Adds a sun highlight on grass and grass-placed rocks." },
	{ "Shaders.Grass.NormalMaps", "SpecularStrength", "Shine strength", nullptr, "How bright the sun highlight is." },
	{ "Shaders.Grass.NormalMaps", "Glossiness", "Shine sharpness", nullptr, "Low gives a broad soft sheen, high gives small sharp glints." },
	// Wetness (Main)
	{ "Shaders.WetnessEffects.Main", "MaxRainWetness", "Rain wetness", nullptr, "How wet and glossy surfaces under the open sky get in the rain." },
	{ "Shaders.WetnessEffects.Main", "MaxPuddleWetness", "Puddle amount", nullptr, "How much puddles fill up on flat ground." },
	{ "Shaders.WetnessEffects.Main", "PuddleRadius", "Puddle size", nullptr, "How big the puddles are." },
	{ "Shaders.WetnessEffects.Main", "MinRainWetness", "Wetness in sheltered spots", nullptr, "How wet walls and undersides get, where rain does not fall straight on." },
	{ "Shaders.WetnessEffects.Main", "WeatherTransitionSpeed", "Wet and dry speed", nullptr, "How fast things get wet when rain starts and dry when it stops." },
	{ "Shaders.WetnessEffects.Main", "CreviceFill", "Ground flooding", nullptr, "Terrain only. Higher lets standing water rise further in the cracks and flood more ground." },
	// Wetness (Raindrops)
	{ "Shaders.WetnessEffects.Raindrops", "EnableRaindropFx", "Raindrops on the ground", nullptr, "Drops land on open ground near you while it rains. Terrain only." },
	{ "Shaders.WetnessEffects.Raindrops", "EnableRipples", "Rain ripples", nullptr, "Rings spread out where drops land on puddles and wet flat ground." },
	{ "Shaders.WetnessEffects.Raindrops", "RippleStrength", "Ripple strength", nullptr, "How strongly the ripples distort the water." },
	{ "Shaders.WetnessEffects.Raindrops", "RaindropChance", "Raindrop amount", nullptr, "How many drops land in heavy rain. Lower looks lighter." },
	{ "Shaders.WetnessEffects.Raindrops", "RaindropFxRange", "Raindrop distance", nullptr, "How far from you the drops and ripples are drawn." },
	{ "Shaders.WetnessEffects.Raindrops", "RippleLifetime", "Ripple time", nullptr, "How long each ripple lasts, in seconds." },
	// Wetness (HudRaindrops)
	{ "Shaders.WetnessEffects.HudRaindrops", "Enabled", "Rain drops on arms and weapon", nullptr, "Water drops gather on your hands and weapon in the rain. They catch the light and bend what is under them." },
	{ "Shaders.WetnessEffects.HudRaindrops", "AllObjects", "Rain drops on everything", nullptr, "Drops also appear on buildings, props and people out in the rain. Sheltered things stay dry." },
	{ "Shaders.WetnessEffects.HudRaindrops", "Density", "Drop amount", nullptr, "How many drops gather once the surface is fully wet." },
	{ "Shaders.WetnessEffects.HudRaindrops", "DropSize", "Drop size", nullptr, "Bigger gives larger drops." },
	{ "Shaders.WetnessEffects.HudRaindrops", "Darkness", "Wet darkening", nullptr, "How much each drop darkens the surface under it, as if soaking in." },
	{ "Shaders.WetnessEffects.HudRaindrops", "WetTime", "Time to get wet or dry", nullptr, "Seconds of heavy rain for a surface to wet through, and to dry off again." },
	// Wetness (Reflections)
	{ "Shaders.WetnessEffects.Reflections", "Enabled", "Wet reflections", nullptr, "Wet ground and objects reflect what is on screen, not just the sky. Needs Wetness Effects on." },
	{ "Shaders.WetnessEffects.Reflections", "Strength", "Reflection strength", nullptr, "How much of the reflection shows the scene instead of the sky." },
	{ "Shaders.WetnessEffects.Reflections", nullptr, "Reflection quality",
		"Low: Steps=12 MaxDistance=800.0 | Medium: Steps=24 MaxDistance=1500.0 | High: Steps=40 MaxDistance=2500.0",
		"Higher finds more distant things and looks finer, but costs more frame rate." },
	{ "Shaders.WetnessEffects.Reflections", "MaxCameraDistance", "Reflection range", nullptr, "Wet surfaces farther away than this show no scene reflections." },
};

static const char* SimpleTip(const char* section, const char* key) {
	for (const SimpleRow& r : kSimpleRows) if (r.Key && !strcmp(section, r.Section) && !strcmp(key, r.Key)) return r.Tip;
	return nullptr;
}

static bool HasSimpleView(const std::string& section) {
	for (const SimpleRow& r : kSimpleRows) if (section == r.Section) return true;
	return false;
}

static const char* SimpleLabel(const char* section, const char* key) {
	for (const SimpleRow& r : kSimpleRows) if (r.Key && !strcmp(section, r.Section) && !strcmp(key, r.Key)) return r.Label;
	return nullptr;
}

struct SimpleOption { std::string Name; std::vector<std::pair<std::string, std::string>> Values; };

static std::vector<SimpleOption> ParseSimpleOptions(const char* text) {
	std::vector<SimpleOption> options;
	std::stringstream all(text);
	std::string part;
	while (std::getline(all, part, '|')) {
		const size_t colon = part.find(':');
		if (colon == std::string::npos) continue;
		SimpleOption o;
		o.Name = part.substr(0, colon);
		o.Name.erase(0, o.Name.find_first_not_of(' '));
		o.Name.erase(o.Name.find_last_not_of(' ') + 1);
		std::stringstream values(part.substr(colon + 1));
		std::string kv;
		while (values >> kv) {
			const size_t eq = kv.find('=');
			if (eq != std::string::npos) o.Values.push_back({ kv.substr(0, eq), kv.substr(eq + 1) });
		}
		options.push_back(o);
	}
	return options;
}

static bool SimpleValueIs(const char* section, const std::string& key, const std::string& value) {
	if (value == "true" || value == "false") return (TheSettingManager->GetSettingI(section, key.c_str()) != 0) == (value == "true");
	if (value.find('.') != std::string::npos) return fabsf(TheSettingManager->GetSettingF(section, key.c_str()) - (float)atof(value.c_str())) < 1e-3f;
	return TheSettingManager->GetSettingI(section, key.c_str()) == atoi(value.c_str());
}

// A choice row: picking an option writes all its settings; "Custom" while they match none of the options.
static void DrawSimpleChoice(const SimpleRow& r) {
	const std::vector<SimpleOption> options = ParseSimpleOptions(r.Options);
	int current = -1;
	for (int i = 0; i < (int)options.size() && current < 0; i++) {
		bool all = true;
		for (const auto& kv : options[i].Values) all = all && SimpleValueIs(r.Section, kv.first, kv.second);
		if (all) current = i;
	}
	ImGui::PushID(r.Label);
	MenuWidgets::RowBegin(r.Label, MenuWidgets::ResetSlotWidth());
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##choice", current >= 0 ? options[current].Name.c_str() : "Custom (set in Advanced)")) {
		for (int i = 0; i < (int)options.size(); i++) {
			if (!ImGui::Selectable(options[i].Name.c_str(), i == current)) continue;
			std::vector<std::array<std::string, 3>> sets;
			for (const auto& kv : options[i].Values) sets.push_back({ r.Section, kv.first, kv.second });
			if (!ConfirmChanges(sets)) continue;	// 2026-11-05: a conflict: the Heads up box first
			for (const auto& kv : options[i].Values) TheSettingManager->SetSettingS(r.Section, kv.first.c_str(), kv.second.c_str());
			TheSettingManager->LoadSettings();
		}
		ImGui::EndCombo();
	}
	if (r.Tip && (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())) WrappedTooltip(r.Tip);
	ImGui::PopID();
}

static const char* MenuKeyLabel(const char* section, const char* key) {
	if (!AdvancedShown())
		if (const char* plain = SimpleLabel(section, key)) return plain;	// 2026-11-05: the simple view's name
	if (!strcmp(section, "Main.Main.Performance") && !strcmp(key, "NvrDxvkDisplacement")) return "NvrDxvkDisplacement (tessellation)##NvrDxvkDisplacement";
	return key;
}

// UNOFFICIAL 2026-10-27 (user: "the render scale on dlss and fsr should mimic modern games and just have a few options to select"):
// [Shaders.TAA.FSR] / [Shaders.TAA.DLSS] RenderScale as the usual quality modes (AMD's and NVIDIA's ratios: 1.5x, 1.7x, 2x, 3x per
// axis). The file keeps the number: any other value shows as Custom and stays until a mode is picked.
static bool IsRenderScaleSetting(const char* section, const char* key) {
	return !strcmp(key, "RenderScale") && (!strcmp(section, "Shaders.TAA.FSR") || !strcmp(section, "Shaders.TAA.DLSS"));
}
static bool RenderScaleCombo(const char* section, float& val) {
	static const float scales[] = { 1.0f, 0.667f, 0.588f, 0.5f, 0.333f };
	const bool dlss = !strcmp(section, "Shaders.TAA.DLSS");
	const char* const names[] = { dlss ? "Native (DLAA)" : "Native (anti-aliasing only)", "Quality (67%)", "Balanced (59%)",
		"Performance (50%)", "Ultra Performance (33%)" };
	int current = -1;
	for (int i = 0; i < 5; i++) if (fabsf(val - scales[i]) < 0.005f) current = i;
	char custom[48];
	sprintf_s(custom, "Custom (%.0f%%)", val * 100.0f);
	const int picked = NamePicker("##v", current >= 0 ? names[current] : custom, names, 5, current);
	if (picked < 0) return false;
	val = scales[picked];
	return true;
}

// UNOFFICIAL: float settings too large for the drag step every other float uses (0.001 a pixel), such as distances in game
// units, render as a slider over a fixed range instead. Keyed by "Section.Key" like the choice lists.
struct FloatRange { float Min; float Max; const char* Format; };
static const std::unordered_map<std::string, FloatRange> kFloatSliders = {
	{ "Shaders.PBR.Metal.FadeDistance", { 0.0f, 4000.0f, "%.0f" } },
	{ "Shaders.ShadowsInteriors.Main.NearFade", { 0.0f, 128.0f, "%.0f" } },
	{ "Shaders.ShadowsInteriors.Main.PlayerLampMargin", { 0.0f, 64.0f, "%.0f" } },
	// UNOFFICIAL 2026-10-21 cloth indoors (core/ClothWind.h BeginFrame)
	{ "Shaders.Cloth.Interiors.Strength", { 0.0f, 2.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.WindIntensity", { 0.0f, 12.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.WindSpeed", { 0.0f, 400.0f, "%.0f" } },
	{ "Shaders.Cloth.Interiors.Wavelength", { 16.0f, 1000.0f, "%.0f" } },
	{ "Shaders.Cloth.Interiors.FlutterSpeed", { 0.0f, 6.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.FlutterAmount", { 0.0f, 1.0f, "%.2f" } },
	{ "Shaders.Cloth.Interiors.RippleAmount", { 0.0f, 1.0f, "%.2f" } },
	// UNOFFICIAL 2026-11-06 Bend contact shadows: where they start (0 = where the shadow maps end) and how long they fade in
	{ "Shaders.ShadowsExteriors.ScreenSpace.StartDistance", { 0.0f, 40000.0f, "%.0f" } },
	{ "Shaders.ShadowsExteriors.ScreenSpace.FadeLength", { 100.0f, 10000.0f, "%.0f" } },
};

// ---- Setting rows ------------------------------------------------------------------------------------------------------------
// One row per setting: its name on the left, the control filling the rest, a restart note if it needs one, and the ~ button at
// the end while the value differs from what it was when the menu opened. Max Quality's values are red (and editable); settings
// the current mode does not read are grey, with the reason on hover.

struct SettingRow {
	SettingManager::Configuration::ConfigNode& node;
	const std::string& atOpen;	// the value when the menu opened
	bool changed;				// differs from atOpen
	bool maxQuality;			// the value is Max Quality's
	bool inert;					// nothing reads it (greyed and disabled)
	const char* why;			// why it is greyed, or nullptr
	ImVec4 normalText;			// the style's text colour, for the tooltip
	RestartNote restart;		// restart.text is null when there is no note
	float controlW = 0.0f;		// the control's width (set by RenderSetting)
};

static void Store(const SettingManager::Configuration::ConfigNode& node, float value) {
	TheSettingManager->SetSetting(node.Section, node.Key, value);
	TheSettingManager->LoadSettings();
}

// The hover note of a greyed (inert) switch or number: Max Quality's file, or why nothing reads it.
static void InertSettingTooltip(const SettingRow& row) {
	if (row.maxQuality) {
		ImGui::SetTooltip("Max Quality value (VegasShaders.maxquality.toml). Changes here are saved to "
			"VegasShaders.maxquality.user.toml, not to your own settings.");
		return;
	}
	ImGui::SetTooltip("Disabled: has no effect. Mipmaps/anisotropy are commented out in ShadowsExterior.cpp (deferred shadows "
		"produce artifacted derivatives).");
}

// UNOFFICIAL 2026-10-27: FSR and DLSS are either/or (user: "toggling one disables the other"). 2026-10-29 (tester: DLSS -> FSR 3.1
// in the menu left the picture not upscaled): each has its own RenderScale, so the one switched on takes the RenderScale of the
// one switched off, and the picture keeps its render size.
static void SwitchOffOtherUpscaler(const SettingManager::Configuration::ConfigNode& node) {
	const char* other = !strcmp(node.Section, "Shaders.TAA.FSR") ? "Shaders.TAA.DLSS"
		: !strcmp(node.Section, "Shaders.TAA.DLSS") ? "Shaders.TAA.FSR" : nullptr;
	if (!other) return;
	if (TheSettingManager->GetSettingI(other, "Enabled")) {
		const float scale = TheSettingManager->GetSettingF(other, "RenderScale");
		if (scale > 0.0f) TheSettingManager->SetSetting(node.Section, "RenderScale", scale);
	}
	TheSettingManager->SetSetting(other, "Enabled", false);
}

// Returns whether the control is under the mouse (as the other Draw* below).
static bool DrawBoolSetting(SettingRow& row) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	bool on = !strcmp(node.Value, "1") || !_stricmp(node.Value, "true");
	if (MenuWidgets::Toggle("##v", &on) && (!on || ConfirmChange(node.Section, node.Key, "true"))) {	// (2026-11-05: conflicts ask first)
		TheSettingManager->SetSetting(node.Section, node.Key, on);
		if (on && !strcmp(node.Key, "Enabled")) SwitchOffOtherUpscaler(node);
		TheSettingManager->LoadSettings();
	}
	return ImGui::IsItemHovered();
}

// A dropdown of quality modes (render scale), a slider over a fixed range, or a drag box. The +/- keys step it while the mouse
// is over it; for shader pages, Developer options add -/+ buttons that do the same.
static bool DrawFloatSetting(SettingRow& row, bool isShader) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	float value = (float)atof(node.Value);
	const bool modes = IsRenderScaleSetting(node.Section, node.Key);	// UNOFFICIAL 2026-10-27
	const auto range = kFloatSliders.find(std::string(node.Section) + "." + node.Key);

	ImGui::SetNextItemWidth(row.controlW);
	bool edited;
	if (modes) edited = RenderScaleCombo(node.Section, value);
	else if (range != kFloatSliders.end()) edited = ImGui::SliderFloat("##v", &value, range->second.Min, range->second.Max, range->second.Format);
	else edited = ImGui::DragFloat("##v", &value, 0.001f, 0.0f, 0.0f, "%.4f");
	if (edited) Store(node, value);

	const bool hovered = ImGui::IsItemHovered();
	if (hovered && !modes && (gMenu.plusKey || gMenu.minusKey)) {
		value += gMenu.plusKey ? gMenu.step : -gMenu.step;
		Store(node, value);
	}
	if (isShader && DevOptionsShown() && !modes) {	// UNOFFICIAL 2026-10-25
		for (const char* button : { "-", "+" }) {
			ImGui::SameLine();
			if (!ImGui::SmallButton(button)) continue;
			value += button[0] == '+' ? gMenu.step : -gMenu.step;
			Store(node, value);
		}
	}
	return hovered;
}

// UNOFFICIAL 2026-10-25 menu tidy: whole numbers that are plain 0/1 switches get a switch (it writes 0 or 1; the type in the file
// stays). A choice list gets its dropdown; any other number a drag box.
static bool DrawIntSetting(SettingRow& row) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	const int stored = atoi(node.Value);
	int wanted = stored;
	const Choices* choices = ChoicesFor(node.Section, node.Key);
	if (!choices && (stored == 0 || stored == 1) && IsOnOffInteger(node)) {
		bool on = stored != 0;
		if (MenuWidgets::Toggle("##v", &on)) wanted = on ? 1 : 0;
	}
	else if (choices) {
		const int shown = ImClamp(stored, 0, choices->count - 1);
		ImGui::SetNextItemWidth(row.controlW);
		const int picked = NamePicker("##v", choices->names[shown], choices->names, choices->count, shown);
		if (picked >= 0) wanted = picked;
		if (ImGui::IsItemHovered() && (gMenu.plusKey || gMenu.minusKey))
			wanted = ImClamp(shown + (gMenu.plusKey ? 1 : -1), 0, choices->count - 1);
	}
	else {
		ImGui::SetNextItemWidth(row.controlW);
		ImGui::DragInt("##v", &wanted, 1.0f);
	}
	const bool hovered = ImGui::IsItemHovered();
	if (wanted != stored && ConfirmChange(node.Section, node.Key, std::to_string(wanted))) {	// (2026-11-05: conflicts ask first)
		TheSettingManager->SetSetting(node.Section, node.Key, wanted);
		TheSettingManager->LoadSettings();
	}
	return hovered;
}

// A text box, written back when editing ends. What is typed lives in its own buffer (per box) so the stored value does not
// overwrite it mid-edit. KeyEnable also gets the "(?)" scan code reference.
static bool DrawTextSetting(SettingRow& row) {
	SettingManager::Configuration::ConfigNode& node = row.node;
	static std::unordered_map<ImGuiID, std::string> typing;
	const ImGuiID box = ImGui::GetID("##v");
	std::string& text = typing[box];
	if (ImGui::GetActiveID() != box) text = node.Value;

	char edit[80];
	strncpy_s(edit, text.c_str(), _TRUNCATE);
	ImGui::SetNextItemWidth(row.controlW);
	if (ImGui::InputText("##v", edit, sizeof(edit))) text = edit;
	const bool hovered = ImGui::IsItemHovered();
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		TheSettingManager->SetSettingS(node.Section, node.Key, edit);
		TheSettingManager->LoadSettings();
	}
	if (!strcmp(node.Key, "KeyEnable")) {
		ImGui::SameLine();
		if (ImGui::SmallButton("(?)")) ImGui::OpenPopup("DIKReference");
		ScanCodePopup();
	}
	return hovered;
}

// Width kept after the control for what follows it on the row.
static float TrailingWidth(const SettingRow& row, bool isShader) {
	using NodeType = SettingManager::Configuration::NodeType;
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	float width = MenuWidgets::ResetSlotWidth() + RestartNoteWidth(row.restart);
	const SettingManager::Configuration::ConfigNode& node = row.node;
	if (node.Type == NodeType::Float && isShader && DevOptionsShown() && !IsRenderScaleSetting(node.Section, node.Key))
		width += MenuWidgets::SmallButtonWidth("-") + MenuWidgets::SmallButtonWidth("+") + gap * 2.0f;
	if (node.Type != NodeType::Boolean && node.Type != NodeType::Float && node.Type != NodeType::Integer && !strcmp(node.Key, "KeyEnable"))
		width += MenuWidgets::SmallButtonWidth("(?)") + gap;
	return width;
}

static void RenderSetting(SettingManager::Configuration::ConfigNode& node, bool isShader = false) {
	using NodeType = SettingManager::Configuration::NodeType;
	ImGui::PushID(node.Key);

	const std::string& atOpen = SessionValue(node.Section, node.Key, node.Value);
	const bool inert = IsInertSetting(node.Section, node.Key);
	SettingRow row{ node, atOpen, atOpen != node.Value, node.MaxQuality, inert,
		inert ? nullptr : InactiveReason(node.Section, node.Key), ImGui::GetStyleColorVec4(ImGuiCol_Text) };
	FindRestartNote(node.Section, node.Key, row.restart);

	if (row.inert) ImGui::BeginDisabled();
	int tints = 0;
	if (row.maxQuality) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
		tints++;
	}
	if (row.why) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		tints++;
	}

	row.controlW = MenuWidgets::RowBegin(MenuKeyLabel(node.Section, node.Key), TrailingWidth(row, isShader));
	bool overControl;
	switch (node.Type) {
	case NodeType::Boolean:
		overControl = DrawBoolSetting(row);
		if (row.inert && overControl) InertSettingTooltip(row);
		break;
	case NodeType::Float:
		overControl = DrawFloatSetting(row, isShader);
		break;
	case NodeType::Integer:
		overControl = DrawIntSetting(row);
		if (row.inert && overControl) InertSettingTooltip(row);
		break;
	default:
		// [Main.Menu.Style] Custom<colour>: a tick and a colour box instead of the text box (2026-11-02)
		if (!MenuSkin::DrawColourSetting(node.Section, node.Key, node.Value, row.controlW, overControl))
			overControl = DrawTextSetting(row);
		break;
	}
	if (tints) ImGui::PopStyleColor(tints);

	if (row.restart.text) {
		ImGui::SameLine();
		DrawRestartNote(row.restart);
	}
	if (MenuWidgets::ResetButton("~", row.changed, "Revert to value at session start")) {
		TheSettingManager->SetSettingS(node.Section, node.Key, row.atOpen.c_str());
		TheSettingManager->LoadSettings();
	}
	if (row.inert) ImGui::EndDisabled();

	if (overControl || MenuWidgets::RowLabelHovered()) {
		const char* plain = AdvancedShown() ? nullptr : SimpleTip(node.Section, node.Key);	// 2026-11-05: the simple view's own words
		if (plain && !row.why) WrappedTooltip(plain);
		else if (plain) WrappedTooltip((std::string(plain) + "\n\nNot active now: " + row.why).c_str());
		else SettingTooltip(node, row.why, row.normalText, MenuWidgets::RowLabelClipped());
	}
	ImGui::PopID();
}

// ---- "New in this patch" tab ---------------------------------------------------------------------
// UNOFFICIAL 2026-10-18 (user: "a tab at the top that has all new settings in it for that specific patch for easy testing. it
// shouldn't replace the settings that exist in the menus where they originate from"). A first entry in the sidebar that lists
// the settings new in the current patch, grouped by the section they live in. Every entry is the very same setting drawn by the
// same RenderSetting as in its own section (same storage, same Save, same tooltip, same revert button): the original menus are
// untouched. Which settings count is decided from the defaults file alone, never from the user's settings:
//   - the description (the comment next to the key) holds a date tag ("(2026-10-17)") on or after [Main.Menu.NewInPatch] NewSince
//     (the latest date in the text counts, so an old setting whose text gained a newer date is listed too), or
//   - the key is in NewInclude ("Section.Key" or "Section.*", comma separated, for keys whose text has no date), and
//   - not in NewExclude.
// Diagnostic keys (names ending in Check / Log / Probe / DebugView, descriptions that say diagnostic or test only, or NewDiagnostics)
// go in a collapsed Diagnostics group at the bottom. The list is built once (at start and after "Disk" reloads the defaults) and
// each entry's value is re-read only when a setting changed (SettingManager::SettingsRevision) or every 30 frames.
static const char* const kNewTabId = "@NewInPatch";

struct NewEntry {
	std::string Section, Key, Date;
	bool        Diagnostic = false;
	bool        Status     = false;	// Shaders.<Name>.Status Enabled: the shader's on/off switch (drawn like the sidebar's checkbox)
	std::string ShaderName;
	bool        Valid = false;
	SettingManager::Configuration::ConfigNode Node;
};
struct NewGroup { std::string Section, Display; size_t Begin, End; };

static std::vector<NewEntry> s_newMain, s_newDiag;	// sorted by section, then key (the same order as the section's own menu)
static std::vector<NewGroup> s_newMainGroups, s_newDiagGroups;
static std::string s_newSince;
static UInt32      s_newGeneration = 0xFFFFFFFFu;	// DefaultsGeneration the list was built from
static UInt32      s_newRevision   = 0xFFFFFFFFu;	// SettingsRevision the values were read at
static int         s_newFrames     = 0;

static const tomlValue* NewFind(const tomlValue* table, const char* name) {
	if (!table || !table->is_table()) return nullptr;
	const auto& map = table->as_table();
	auto it = map.find(name);
	return it == map.end() ? nullptr : &it->second;
}

// A hidden string from [Main.Menu.<page>], read from the defaults file only ("" if the section, the key or its text is missing).
static std::string MenuMeta(const char* page, const char* key) {
	try {
		const tomlValue* section = NewFind(NewFind(NewFind(&TheSettingManager->Config.DefaultConfig, "_Main"), "Menu"), page);
		const tomlValue* value = NewFind(section, key);
		if (value && value->is_string()) return value->as_string();
	} catch (...) {}
	return std::string();
}
static std::string NewMeta(const char* key) { return MenuMeta("NewInPatch", key); }

static void NewSplitList(const std::string& text, std::vector<std::string>& out) {
	std::string cur;
	for (size_t i = 0; i <= text.size(); i++) {
		const char c = i < text.size() ? text[i] : ',';
		if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			if (!cur.empty()) out.push_back(cur);
			cur.clear();
		} else cur += c;
	}
}

// "Section.Key" is that setting; "Section.*" is every setting of that section and of the sections below it.
static bool NewListMatches(const std::vector<std::string>& list, const std::string& section, const std::string& key) {
	for (const std::string& p : list) {
		if (p.size() > 2 && p.compare(p.size() - 2, 2, ".*") == 0) {
			const std::string base = p.substr(0, p.size() - 2);
			if (section == base || (section.size() > base.size() && section.compare(0, base.size(), base) == 0 && section[base.size()] == '.')) return true;
		} else {
			const size_t dot = p.rfind('.');
			if (dot != std::string::npos && dot > 0 && dot == section.size() && p.compare(0, dot, section) == 0 && p.compare(dot + 1, std::string::npos, key) == 0) return true;
		}
	}
	return false;
}

// UNOFFICIAL 2026-10-25 menu tidy: [Main.Menu.Developer] DevOnly (defaults file only, the same format as NewInclude): settings shown
// only with Developer options. An empty key asks about a whole section (true for a "Section.*" entry at or above it).
static bool IsDevOnlySetting(const std::string& section, const std::string& key) {
	static std::vector<std::string> list;
	static UInt32 generation = 0xFFFFFFFFu;
	if (generation != TheSettingManager->Config.DefaultsGeneration) {	// the defaults were read again ("Disk")
		generation = TheSettingManager->Config.DefaultsGeneration;
		list.clear();
		NewSplitList(MenuMeta("Developer", "DevOnly"), list);
	}
	return NewListMatches(list, section, key);
}

// The latest YYYY-MM-DD (20xx, month 1-12, day 1-31, not part of a longer number) in the text, or "" if none.
static std::string NewLatestDate(const std::string& s) {
	std::string best;
	auto dig = [](char c) { return c >= '0' && c <= '9'; };
	for (size_t i = 0; i + 10 <= s.size(); i++) {
		const char* p = s.c_str() + i;
		if (!(p[0] == '2' && p[1] == '0' && dig(p[2]) && dig(p[3]) && p[4] == '-' && dig(p[5]) && dig(p[6]) && p[7] == '-' && dig(p[8]) && dig(p[9]))) continue;
		if (i > 0 && dig(s[i - 1])) continue;
		if (i + 10 < s.size() && dig(s[i + 10])) continue;
		const int month = (p[5] - '0') * 10 + (p[6] - '0'), day = (p[8] - '0') * 10 + (p[9] - '0');
		if (month < 1 || month > 12 || day < 1 || day > 31) continue;
		const std::string date(p, 10);
		if (date > best) best = date;
	}
	return best;
}

static bool NewEndsWith(const std::string& s, const char* suffix) {
	const size_t n = strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static bool NewIsDiagnostic(const std::string& key, const std::string& description, const std::vector<std::string>& extra, const std::string& section) {
	if (NewListMatches(extra, section, key)) return true;
	static const char* const suffixes[] = { "Check", "Log", "DebugView", "Probe", "ProbeDump", "ProbeLog", "Dump", "TestSkip", "OrderLog", "Debug" };
	for (const char* suffix : suffixes) if (NewEndsWith(key, suffix)) return true;
	if (key.compare(0, 5, "Debug") == 0) return true;
	if (key.size() > 3 && key.compare(0, 3, "Log") == 0 && isupper((unsigned char)key[3])) return true;	// 2026-10-19: LogLamps, LogMatches, LogDraws
	std::string lower = description;
	for (char& c : lower) c = (char)tolower((unsigned char)c);
	static const char* const words[] = { "diagnostic", "test only", "testing", "debugging", "test views", "development builds only", "no effect:",
		"for finding faults", "checking only" };
	for (const char* word : words) if (lower.find(word) != std::string::npos) return true;
	return false;
}

struct NewWalkContext {
	std::string Since;
	std::vector<std::string> Include, Exclude, Diagnostics;
};

static void NewWalk(const tomlValue& table, const std::string& path, NewWalkContext& ctx) {
	for (const auto& [name, value] : table.as_table()) {
		if (value.is_table()) {
			const std::string child = path.empty() ? (name.compare(0, 1, "_") == 0 ? name.substr(1) : name) : path + "." + name;
			if (child == "Weathers" || child.compare(0, 9, "Weathers.") == 0) continue;
			NewWalk(value, child, ctx);
			continue;
		}
		if (path.empty()) continue;	// a setting outside any section
		if (path == "Main.Menu.NewInPatch") continue;	// this tab's own data
		if (!(value.is_boolean() || value.is_integer() || value.is_floating() || value.is_string())) continue;	// the menu cannot show it either
		if (("." + path + ".").find(".WeatherMode.") != std::string::npos) continue;	// hidden in the menu (ShouldHideSection)
		const bool status = NewEndsWith(path, ".Status");
		if (status && !(path.compare(0, 8, "Shaders.") == 0 && name == "Enabled")) continue;	// Status sections are hidden in the menu; only a shader's own switch has a place
		if (ShouldHideKey(name.c_str())) continue;

		std::string description;
		for (const std::string& c : value.comments()) { description += c; description += ' '; }
		const std::string date = NewLatestDate(description);
		bool isNew = !ctx.Since.empty() && !date.empty() && date >= ctx.Since;
		if (NewListMatches(ctx.Include, path, name)) isNew = true;
		if (NewListMatches(ctx.Exclude, path, name)) isNew = false;
		if (!isNew) continue;

		NewEntry entry;
		entry.Section = path;
		entry.Key = name;
		entry.Date = date;
		entry.Diagnostic = NewIsDiagnostic(name, description, ctx.Diagnostics, path);
		if (status) {
			entry.Status = true;
			const size_t dot = path.find('.', 8);
			entry.ShaderName = path.substr(8, dot == std::string::npos ? std::string::npos : dot - 8);
			if (entry.ShaderName.empty()) continue;
		}
		(entry.Diagnostic ? s_newDiag : s_newMain).push_back(std::move(entry));
	}
}

static void NewMakeGroups(const std::vector<NewEntry>& entries, std::vector<NewGroup>& groups) {
	groups.clear();
	for (size_t i = 0; i < entries.size(); i++) {
		if (groups.empty() || groups.back().Section != entries[i].Section) {
			std::string display;
			for (char c : entries[i].Section) display += (c == '.') ? std::string(" > ") : std::string(1, c);
			groups.push_back({ entries[i].Section, display, i, i + 1 });
		} else groups.back().End = i + 1;
	}
}

static void NewPatchRebuild() {
	s_newMain.clear();
	s_newDiag.clear();
	s_newMainGroups.clear();
	s_newDiagGroups.clear();
	s_newRevision = 0xFFFFFFFFu;
	s_newGeneration = TheSettingManager->Config.DefaultsGeneration;
	try {
		NewWalkContext ctx;
		ctx.Since = NewMeta("NewSince");
		s_newSince = ctx.Since;
		NewSplitList(NewMeta("NewInclude"), ctx.Include);
		NewSplitList(NewMeta("NewExclude"), ctx.Exclude);
		NewSplitList(NewMeta("NewDiagnostics"), ctx.Diagnostics);
		if (TheSettingManager->Config.DefaultConfig.is_table()) NewWalk(TheSettingManager->Config.DefaultConfig, std::string(), ctx);
	} catch (const std::exception& e) {
		Logger::Log("[ERROR] NEW IN THIS PATCH: the list could not be built (%s)", e.what());
		s_newMain.clear();
		s_newDiag.clear();
	} catch (...) {
		s_newMain.clear();
		s_newDiag.clear();
	}
	// A table that holds both settings and sub-sections can interleave them: keep every section's settings together.
	const auto bySection = [](const NewEntry& a, const NewEntry& b) { return a.Section < b.Section; };
	std::stable_sort(s_newMain.begin(), s_newMain.end(), bySection);
	std::stable_sort(s_newDiag.begin(), s_newDiag.end(), bySection);
	NewMakeGroups(s_newMain, s_newMainGroups);
	NewMakeGroups(s_newDiag, s_newDiagGroups);
	Logger::Log("NEW IN THIS PATCH: %u settings since %s (%u to test, %u diagnostics)", (UInt32)(s_newMain.size() + s_newDiag.size()),
		s_newSince.empty() ? "(no NewSince)" : s_newSince.c_str(), (UInt32)s_newMain.size(), (UInt32)s_newDiag.size());
}

static size_t NewPatchCount() { return s_newMain.size() + s_newDiag.size(); }
static void NewPatchEnsure() { if (s_newGeneration != TheSettingManager->Config.DefaultsGeneration) NewPatchRebuild(); }	// the defaults were read again ("Disk")

static void NewRefreshNodes(std::vector<NewEntry>& entries) {
	for (NewEntry& e : entries)
		e.Valid = TheSettingManager->Config.FillNode(&e.Node, e.Section.c_str(), e.Key.c_str());
}

// Same colour-picker grouping as the section menus (a prefix with R, G and B keys) when all three are in this group.
// A shader's own on/off switch, as a row ("<Name> (shader on / off)"): the same switch as the one beside it in the sidebar.
static void NewShaderSwitchRow(const NewEntry& e) {
	const char* shader = e.ShaderName.c_str();
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	ImGui::PushID(e.Key.c_str());
	ImGui::BeginDisabled(TheSettingManager->IsShaderForced(shader));
	MenuWidgets::RowBegin((e.ShaderName + " (shader on / off)").c_str(), MenuWidgets::ResetSlotWidth());
	const bool flipped = MenuWidgets::Toggle("##v", &on);
	ImGui::EndDisabled();
	if (flipped) {
		TheShaderManager->SwitchShaderStatus(shader);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered()) {
		std::string note = ShownDescription(e.Node.Description);
		if (!note.empty()) note += "\n\n";
		note += "The same switch as the one next to " + e.ShaderName + " in the sidebar.";
		WrappedTooltip(note.c_str());
	}
	ImGui::PopID();
}

static void NewRenderRun(std::vector<NewEntry>& entries, const NewGroup& group) {
	const bool isShader = group.Section.compare(0, 7, "Shaders") == 0;
	std::unordered_set<std::string> drawnKeys;	// the channels of a colour already drawn as one wheel
	for (size_t i = group.Begin; i < group.End; i++) {
		NewEntry& e = entries[i];
		if (!e.Valid || drawnKeys.count(e.Key)) continue;
		if (e.Status) {
			NewShaderSwitchRow(e);
			continue;
		}

		const std::string& key = e.Key;
		if (isShader && key.size() > 1 && (key.back() == 'R' || key.back() == 'G' || key.back() == 'B')) {
			const std::string pfx = key.substr(0, key.size() - 1);
			const bool isColorCurve = (pfx == "ColorCurve" && group.Section.find("Shaders.Coloring") != std::string::npos);
			NewEntry *r = nullptr, *g = nullptr, *b = nullptr;
			for (size_t j = group.Begin; j < group.End; j++) {
				if (entries[j].Key == pfx + "R") r = &entries[j];
				else if (entries[j].Key == pfx + "G") g = &entries[j];
				else if (entries[j].Key == pfx + "B") b = &entries[j];
			}
			if (!isColorCurve && r && g && b && r->Valid && g->Valid && b->Valid) {
				for (const NewEntry* channel : { r, g, b }) drawnKeys.insert(channel->Key);
				RenderColorTriple(r->Node, g->Node, b->Node, pfx);
				continue;
			}
		}
		RenderSetting(e.Node, isShader);
	}
}

// Each group under its section's name: a folding heading, or (the diagnostics) a plain grey line with the rows indented.
static void NewRenderGroups(std::vector<NewEntry>& entries, const std::vector<NewGroup>& groups, bool diagnostics) {
	for (const NewGroup& group : groups) {
		ImGui::PushID(group.Section.c_str());
		if (!diagnostics) {
			if (MenuWidgets::SubHeader((group.Display + "##newgroup").c_str(), true)) NewRenderRun(entries, group);
		}
		else {
			ImGui::TextDisabled("%s", group.Display.c_str());
			ImGui::Indent();
			NewRenderRun(entries, group);
			ImGui::Unindent();
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
		}
		ImGui::PopID();
	}
}

// A page's title area: the name large, then a dim line under it.
static void PageTitle(const char* title, const char* dimLine) {
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(title);
	ImGui::PopStyleColor();
	if (dimLine && dimLine[0]) ImGui::TextDisabled("%s", dimLine);
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

static void RenderNewInPatch() {
	NewPatchEnsure();

	char counted[96];
	sprintf_s(counted, "%u settings%s%s", (UInt32)NewPatchCount(), s_newSince.empty() ? "" : " since ", s_newSince.c_str());
	PageTitle("New in this patch", counted);
	if (NewPatchCount() == 0) {
		ImGui::TextDisabled("Nothing is marked as new in this patch.");
		return;
	}

	// Values are re-read when a setting changed or every 30 frames (a hotkey, a console command or a preset changes them too).
	if (s_newRevision != TheSettingManager->SettingsRevision || ++s_newFrames >= 30) {
		s_newFrames = 0;
		s_newRevision = TheSettingManager->SettingsRevision;
		NewRefreshNodes(s_newMain);
		NewRefreshNodes(s_newDiag);
	}

	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextDisabled("Each of these is the same setting as in the menu named above it: change it here or there, it is one value "
		"(saved by Save, undone by the ~ button, shown in red while Max Quality sets it). Hover for what it does.");
	ImGui::PopTextWrapPos();

	NewRenderGroups(s_newMain, s_newMainGroups, false);

	if (s_newDiag.empty()) return;
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	if (!DevOptionsShown()) {	// UNOFFICIAL 2026-10-19 menu honesty
		ImGui::TextDisabled("%u test settings hidden (View > Developer options)", (UInt32)s_newDiag.size());
	}
	else {
		const std::string header = "Diagnostics (" + std::to_string(s_newDiag.size()) + ")##newdiag";
		if (MenuWidgets::SubHeader(header.c_str(), false)) {
			ImGui::TextDisabled("Test switches for development builds; leave them at their defaults when playing.");
			NewRenderGroups(s_newDiag, s_newDiagGroups, true);
		}
	}
}

// UNOFFICIAL 2026-10-25: menu search. The box at the top of the sidebar filters every menu section and setting by name: the
// text is split into words and a setting matches when every word is in its "Section.Key" (case-insensitive), so "shadow
// bias" or "ambient" both work. A shader's name also finds its on/off switch. The results are the very same settings, drawn
// by the "New in this patch" renderer and grouped under their section; the sidebar lists those sections (a click opens one).
// Weathers (generated per weather) are left out. Clearing the box (the x, or Escape while typing) brings the normal menu back.
static char s_searchBuf[64] = "";
static char* SearchTestBuffer() { return s_searchBuf; }

struct SearchKey { std::string Section, Key, Haystack; bool Status; std::string ShaderName; };
static std::vector<SearchKey> s_searchIndex;
static std::vector<NewEntry>  s_searchResults;
static std::vector<NewGroup>  s_searchGroups;
static std::string s_searchQuery;	// the lower-case text the results were built for
static bool        s_searchTests   = false;	// DevOptionsShown() the results were built with
static size_t      s_searchTotal   = 0;	// matches before the cap
static UInt32      s_searchRevision = 0xFFFFFFFFu;
static int         s_searchFrames   = 0;
static const size_t kSearchMax = 400;	// more than this is not a search any more: ask for another word

static std::string SearchLower(const std::string& s) {
	std::string out(s);
	for (char& c : out) c = (char)tolower((unsigned char)c);
	return out;
}

static std::string SearchQueryText() {
	std::string q = SearchLower(s_searchBuf);
	const size_t b = q.find_first_not_of(' ');
	if (b == std::string::npos) return std::string();
	return q.substr(b, q.find_last_not_of(' ') - b + 1);
}

static bool SearchActive() { return !SearchQueryText().empty(); }

// The same tree as the sidebar (FillMenuSections / FillMenuSettings, the same hidden sections and keys), leaves only.
static void SearchIndexSection(const std::string& path, const std::string& name, bool parentIsShaders) {
	if (ShouldHideSection(name)) return;
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	if (children.empty()) {
		SettingManager::Configuration::SettingList page;
		TheSettingManager->FillMenuSettings(&page, path.c_str());
		for (const auto& setting : page)
			if (!ShouldHideKey(setting.Key))
				s_searchIndex.push_back({ path, setting.Key, SearchLower(path + "." + setting.Key), false, std::string() });
		return;
	}
	if (parentIsShaders && TheSettingManager->GetMenuShaderSwitch(name.c_str()) != SettingManager::MenuSwitch::None)
		s_searchIndex.push_back({ path + ".Status", "Enabled", SearchLower(path + ".Enabled on off"), true, name });	// the sidebar's checkbox
	for (const std::string& child : children)
		SearchIndexSection(path + "." + child, child, path == "Shaders");
}

static void SearchRebuild(const std::string& query) {
	s_searchQuery = query;
	s_searchTests = DevOptionsShown();
	s_searchResults.clear();
	s_searchGroups.clear();
	s_searchTotal = 0;
	s_searchRevision = TheSettingManager->SettingsRevision;
	s_searchFrames = 0;

	std::vector<std::string> words;
	for (size_t i = 0; i < query.size();) {
		const size_t j = query.find(' ', i);
		const size_t end = (j == std::string::npos) ? query.size() : j;
		if (end > i) words.push_back(query.substr(i, end - i));
		i = end + 1;
	}
	for (const SearchKey& k : s_searchIndex) {
		bool all = true;
		for (const std::string& w : words) if (k.Haystack.find(w) == std::string::npos) { all = false; break; }
		if (!all) continue;
		NewEntry e;
		e.Section = k.Section;
		e.Key = k.Key;
		e.Status = k.Status;
		e.ShaderName = k.ShaderName;
		e.Valid = TheSettingManager->Config.FillNode(&e.Node, e.Section.c_str(), e.Key.c_str());
		if (!e.Valid) continue;
		if (!DevOptionsShown() && IsTestSetting(e.Node)) continue;	// as hidden here as in its own section
		if (++s_searchTotal <= kSearchMax) s_searchResults.push_back(std::move(e));
	}
	NewMakeGroups(s_searchResults, s_searchGroups);
	for (NewGroup& g : s_searchGroups)	// a shader's switch: "Shaders > Name", not its hidden Status section
		if (NewEndsWith(g.Display, " > Status")) g.Display.resize(g.Display.size() - 9);
}

// Keeps the results in step with the box. The index is read again when a search starts (the sections can change with "Disk").
static void SearchEnsure() {
	const std::string query = SearchQueryText();
	if (query.empty()) {
		if (!s_searchQuery.empty()) { s_searchQuery.clear(); s_searchResults.clear(); s_searchGroups.clear(); s_searchIndex.clear(); }
		return;
	}
	if (s_searchIndex.empty()) {
		StringList roots;
		TheSettingManager->FillMenuSections(&roots, nullptr);
		for (const std::string& root : roots)
			if (root != "Weathers") SearchIndexSection(root, root, false);
	}
	if (query != s_searchQuery || s_searchTests != DevOptionsShown()) SearchRebuild(query);
	else if (s_searchRevision != TheSettingManager->SettingsRevision || ++s_searchFrames >= 30) {
		s_searchFrames = 0;
		s_searchRevision = TheSettingManager->SettingsRevision;
		NewRefreshNodes(s_searchResults);
	}
}

static void RenderSearchBox() {
	const char* clearLabel = "x##menusearchclear";
	const float clearW = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputTextWithHint("##menusearch", "Search settings...", s_searchBuf, sizeof(s_searchBuf), ImGuiInputTextFlags_EscapeClearsAll);
	if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
		ImGui::SetTooltip("Type part of a setting's or a section's name (for example \"shadow bias\").\nEvery word must match. Escape or x clears it.");
	ImGui::SameLine();
	const bool empty = s_searchBuf[0] == '\0';
	ImGui::BeginDisabled(empty);
	if (ImGui::Button(clearLabel, ImVec2(clearW, 0.0f))) s_searchBuf[0] = '\0';
	ImGui::EndDisabled();
	SearchEnsure();
}

// Sidebar while searching: the sections that hold a match. A click opens that section and clears the search.
static void RenderSearchSidebar() {
	char found[64];
	sprintf_s(found, "%u in %u sections", (UInt32)s_searchTotal, (UInt32)s_searchGroups.size());
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::TextDisabled("%s", found);
	ImGui::PushStyleColor(ImGuiCol_Text, MenuWidgets::MutedText(0.3f));
	for (const NewGroup& g : s_searchGroups) {
		const bool isSwitch = NewEndsWith(g.Section, ".Status");
		const std::string label = g.Display + (isSwitch ? std::string(" (on / off)") : " (" + std::to_string(g.End - g.Begin) + ")");
		ImGui::PushID(g.Section.c_str());
		const bool clicked = ImGui::Selectable(label.c_str(), false);
		if (clicked && !isSwitch) {
			gMenu.page = g.Section;
			s_searchBuf[0] = '\0';
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(isSwitch ? "The shader's on/off switch, shown on the right." : "Open this section (clears the search).");
		ImGui::PopID();
	}
	ImGui::PopStyleColor();
}

static void RenderSearchResults() {
	const std::string title = std::string("Search: ") + s_searchBuf;
	char counted[48];
	sprintf_s(counted, "%u setting%s", (UInt32)s_searchTotal, s_searchTotal == 1 ? "" : "s");
	PageTitle(title.c_str(), counted);
	if (!s_searchTotal) {
		ImGui::TextDisabled("No setting or section matches. Weather sections are not searched.");
		if (!DevOptionsShown()) ImGui::TextDisabled("Developer settings are hidden (View > Developer options).");
		return;
	}
	if (s_searchTotal > kSearchMax)
		ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Showing the first %u: add a word to narrow it down.", (UInt32)kSearchMax);
	ImGui::PushID("##searchresults");
	NewRenderGroups(s_searchResults, s_searchGroups, false);
	ImGui::PopID();
}

// ---- Menu groups -----------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-11-01 (user: the three fog types "separated with their settings under each collapsing menu", a toggle at the
// top of each that turns the others off; the DXVK settings under their own header in the performance page; "the enabled switch
// at the top"; "tessellation in particular would be pretty confusing to someone outside of our loop"). Only the menu changes:
// every key keeps its name and its section, so saved settings files read as before. On a page, the settings in no group come
// first, then each group under its own collapsing header with its keys in the page's order.

// The fog types: VolumetricFog draws one of them (VolumetricFog.cpp: LegacyFog wins over ClassicLook, ClassicLook 1-2 over the
// current fog). A group's switch picks its type and turns the shader on; unticking the type that is on turns the shader off.
enum class FogType { None = -1, Current, Classic, Legacy };
static const char* const kFogMain = "Shaders.VolumetricFog.Main";
static int s_classicMode = 1;	// the ClassicLook (1 or 2) the Classic switch brings back after another type was picked

static FogType ActiveFog() {
	if (!TheSettingManager->GetMenuShaderEnabled("VolumetricFog")) return FogType::None;
	if (TheSettingManager->GetSettingI(kFogMain, "LegacyFog")) return FogType::Legacy;
	if (TheSettingManager->GetSettingI(kFogMain, "ClassicLook") > 0) return FogType::Classic;
	return FogType::Current;
}

static void SetFogType(FogType type, bool on) {
	const int classic = TheSettingManager->GetSettingI(kFogMain, "ClassicLook");
	if (classic > 0) s_classicMode = classic > 2 ? 2 : classic;
	const bool shaderOn = TheSettingManager->GetMenuShaderEnabled("VolumetricFog");
	if (on) {
		TheSettingManager->SetSetting(kFogMain, "LegacyFog", type == FogType::Legacy);
		TheSettingManager->SetSetting(kFogMain, "ClassicLook", type == FogType::Classic ? s_classicMode : 0);
		if (!shaderOn) TheShaderManager->SwitchShaderStatus("VolumetricFog");
	}
	else if (shaderOn && ActiveFog() == type)
		TheShaderManager->SwitchShaderStatus("VolumetricFog");
	TheSettingManager->LoadSettings();	// the fog reads its type again
}

static void FogSwitch(FogType type) {
	static const char* const labels[] = { "Use the current fog", "Use the classic fog", "Use the legacy fog" };
	bool on = ActiveFog() == type;
	ImGui::BeginDisabled(TheSettingManager->IsShaderForced("VolumetricFog"));
	MenuWidgets::RowBegin(labels[(int)type], MenuWidgets::ResetSlotWidth());
	if (MenuWidgets::Toggle("##fogtype", &on)) SetFogType(type, on);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || MenuWidgets::RowLabelHovered())
		ImGui::SetTooltip("One fog type draws at a time: switching this one on turns the other two off.\n"
			"Switching it off turns the mod's fog off (the VolumetricFog switch in the sidebar).");
}

static void DxvkStatus() {
	if (DxvkCompute::Available())
		ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "The DXVK fork is loaded in this game: these settings work.");
	else
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "The DXVK fork is not loaded (native D3D9 or stock DXVK): these settings do nothing here.");
}

// 2026-11-04: a group's own on/off switch at the top of it (lamp haze, the GT7 modes), like a fog type's.
struct GroupSwitch {
	const char* Label;
	const char* Tip;
	bool (*Get)();
	void (*Set)(bool on);
};

static bool LampHazeGet() { return TheSettingManager->GetSettingI("Shaders.VolumetricFog.Interiors", "LampHazeEnabled") != 0; }
static void LampHazeSet(bool on) { TheSettingManager->SetSetting("Shaders.VolumetricFog.Interiors", "LampHazeEnabled", on); }
static const GroupSwitch kLampHazeSwitch = { "Lamp haze",
	"A soft glow of the room's lamps in the indoor fog's air (the settings below). Needs the current fog.", LampHazeGet, LampHazeSet };

static bool GT7ExposureGet() { return TheSettingManager->GetSettingI("Shaders.Exposure.Main", "Mode") == 1; }
static bool NightShiftGet() { return TheSettingManager->GetSettingI("Shaders.Tonemapping.Main", "NightShiftEnabled") != 0; }
static void NightShiftSet(bool on) { TheSettingManager->SetSetting("Shaders.Tonemapping.Main", "NightShiftEnabled", on); }
static const GroupSwitch kNightShiftSwitch = { "Night colour shift",
	"In the dark the eye loses colour and sees blues best: dim parts of the picture turn paler and cooler, lamps keep their colour. "
	"The same switch on the Main, Night and Interiors pages.", NightShiftGet, NightShiftSet };

static void GT7ExposureSet(bool on) { TheSettingManager->SetSetting("Shaders.Exposure.Main", "Mode", on ? 1 : 0); }
static const GroupSwitch kGT7ExposureSwitch = { "Use eye adaptation",
	"Exposure Mode 1: auto-exposure and white balance as the eye adapts, in place of the classic exposure (Mode 0). The same switch "
	"on the Main, Night and Interiors pages. Made to go with the Perceptual tone mapper (Tonemapping: TonemappingMode 11). After "
	"Polyphony Digital's SIGGRAPH 2025 paper for Gran Turismo 7.", GT7ExposureGet, GT7ExposureSet };

static void DrawGroupSwitch(const GroupSwitch& sw) {
	bool on = sw.Get();
	MenuWidgets::RowBegin(sw.Label, MenuWidgets::ResetSlotWidth());
	if (MenuWidgets::Toggle("##groupswitch", &on)) {
		sw.Set(on);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered()) ImGui::SetTooltip("%s", sw.Tip);
}

struct MenuGroup {
	const char* Section;
	const char* Header;
	const char* Keys;	// comma-separated
	FogType     Fog;	// a fog type's switch at the top of the group, or None
	bool        Open;	// open the first time the page is shown (a fog type's group also opens while it is the one on)
	const char* Note;	// a line under the header, or nullptr
	void      (*Status)();	// a live status line under the note, or nullptr
	const GroupSwitch* Switch = nullptr;	// 2026-11-04: the group's own on/off switch at its top, or nullptr
};

static const MenuGroup kMenuGroups[] = {
	{ kFogMain, "Shared by every fog type",
		"Amount, FogSaturation, WeatherImpact, Extinction, Inscattering, DistantFogRange, DistantFogBlend, DistantFogHeight",
		FogType::None, true, "How strong the fog is, its colour and the distant haze: the current, classic and legacy fog all use these.", nullptr },
	{ kFogMain, "Current fog",
		"FogVolume, BaseDensity, GameFogFloor, WeatherFogColor, MorningFogDip, SunriseSunsetBoost, HeightLayer, LayerBase, LayerThickness, LayerFollowsCamera, "
		"HeightFalloff, MaxHeight, HeightInfluence, "
		"WindAngle, WindSpeed, NoiseScale, NoiseStrength, PhaseAsymmetry, SunGlow, ShadowStrength, RainyDisablesSkyFilter, CloudyDisablesSkyFilter, "
		"AerialStrength, AerialRangeStart, AerialTintBlend, AerialDayFadeStart, AerialTintR, AerialTintG, AerialTintB, EdgeAA, MinDensityFloor",
		FogType::Current, false, "This build's fog: drifting patches that sit in the air, darker where the sun's shadow falls. The classic fog uses "
		"these settings too, except FogVolume, SunGlow, GameFogFloor, WeatherFogColor and the height layer.", nullptr },
	{ kFogMain, "Classic fog (the original look)", "ClassicLook",
		FogType::Classic, false, "The fog as the original mod drew it: patches painted on the surfaces and the game's own sky colour. "
		"ClassicLook 1 = at half resolution (faster), 2 = every pixel at full resolution, as the original did (slower). Its density, "
		"wind and height come from the current fog's settings.", nullptr },
	{ kFogMain, "Legacy fog (for older presets)",
		"LegacyFog, MinimumBaseFog, SunPower, HeightFogDensity, HeightFogFalloff, HeightFogDist, HeightFogSkyColor, HeightFogHeight, HeightFogBlend, "
		"HeightFogRolloff, HeightFogSaturation, HeightFogInscattering, FogNight, SimpleFogSkyColor, SimpleFogHeight, SimpleFogBlend",
		FogType::Legacy, false, "The fog from before this build's fog rewrite (a simple fog plus a height fog), for presets made for it "
		"(Uranium Haze and others). It also uses the shared settings above.", nullptr },

	{ "Shaders.VolumetricFog.Interiors", "Shared by every fog type", "Amount, FogSaturation, WeatherImpact, Extinction, Inscattering",
		FogType::None, true, "Indoors: how strong the fog is and its colour, for all three fog types.", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Current fog",
		"BaseDensity, HeightFalloff, MaxHeight, HeightInfluence, WindAngle, WindSpeed, NoiseScale, NoiseStrength, RainyDisablesSkyFilter, CloudyDisablesSkyFilter",
		FogType::Current, false, "Indoors, for the current fog (the classic fog uses them too).", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Classic fog (the original look)", "",
		FogType::Classic, false, "Indoors the classic fog uses the current fog's settings above; its mode (ClassicLook) is on the Main page.", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Legacy fog (for older presets)",
		"MinimumBaseFog, HeightFogDensity, HeightFogFalloff, HeightFogDist, HeightFogHeight, HeightFogBlend, HeightFogRolloff, HeightFogSaturation, "
		"HeightFogInscattering, SimpleFogHeight, SimpleFogBlend",
		FogType::Legacy, false, "Indoors, for the legacy fog (it also uses the shared settings above).", nullptr },
	{ "Shaders.VolumetricFog.Interiors", "Lamp haze", "LampHaze, LampHazeAnisotropy, LampHazeCore, LampHazeMaxLamps",
		FogType::None, false, "The room's lamps light the indoor fog's air: a soft glow round each lamp. Off by default.", nullptr, &kLampHazeSwitch },

	{ "Shaders.Tonemapping.Main", "Night colour shift", "NightShift, NightShiftLevel",
		FogType::None, false, "Dim parts of the picture lose colour and turn cooler, as the eye sees in the dark. Off by default.", nullptr, &kNightShiftSwitch },
	{ "Shaders.Tonemapping.Night", "Night colour shift", "NightShift, NightShiftLevel",
		FogType::None, false, "At night: the strength most people will want to change.", nullptr, &kNightShiftSwitch },
	{ "Shaders.Tonemapping.Interiors", "Night colour shift", "NightShift, NightShiftLevel",
		FogType::None, false, "Indoors (dark rooms).", nullptr, &kNightShiftSwitch },

	{ "Shaders.Exposure.Main", "Eye adaptation", "AdaptExposure, AdaptWhiteBalance, AdaptWeatherLight",
		FogType::None, false, "Auto-exposure and white balance that adapt as the eye does, metering the whole view. While it is on, MinBrightness, "
		"MaxBrightness and the adapt speeds above do nothing. Off by default.", nullptr, &kGT7ExposureSwitch },
	{ "Shaders.Exposure.Night", "Eye adaptation", "AdaptExposure, AdaptWhiteBalance, AdaptWeatherLight",
		FogType::None, false, "At night, with eye adaptation on.", nullptr, &kGT7ExposureSwitch },
	{ "Shaders.Exposure.Interiors", "Eye adaptation", "AdaptExposure, AdaptWhiteBalance, AdaptWeatherLight",
		FogType::None, false, "Indoors, with eye adaptation on.", nullptr, &kGT7ExposureSwitch },

	{ "Main.Main.Performance", "DXVK (Vulkan fork)",
		"NvrDxvkPasses, NvrDxvkVariants, NvrDxvkStencil, NvrDxvkAmbientLight, NvrDxvkClouds, NvrDxvkKeepD3D9, NvrDxvkTestSuffix, NvrDxvkMixCheck, "
		"NvrDxvkReplacements, NvrSm6Shaders, NvrDxvkDisplacement, AvgLumaCompute, AmbientLightSmoothCompute, AmbientLightBlurCompute",
		FogType::None, false, "Settings for the DXVK fork's d3d9.dll (the fork this build ships): the mod's effects as Vulkan passes, tessellation and the like. "
		"Native D3D9 and stock DXVK ignore them.", DxvkStatus },

	{ "Shaders.Terrain.Displacement", "Bump shape", "Strength, Center, Contrast, BumpSize, LodBias",
		FogType::None, true, "How tall the bumps are and how big the smallest one is.", nullptr },
	{ "Shaders.Terrain.Displacement", "Detail and reach", "EdgeLength, EdgeGrowth, MaxFactor, FadeStart, FadeEnd, BorderGrid, FarUntessellated, FarHysteresis",
		FogType::None, false, "How finely the ground near you is cut up (finer = more work for the graphics card) and how far from you the bumps reach.", nullptr },
	{ "Shaders.Terrain.Displacement", "Meeting objects", "ObjectBlend, ObjectBlendWidth, ObjectBlendKeep, ObjectBlendShadows",
		FogType::None, false, "Flatten the bumps where the ground meets rocks, buildings and feet, so those do not sink into it (experimental).", nullptr },
	{ "Shaders.Terrain.Displacement", "Shadows and lighting", "CastShadows, ShadowSlack, BendNormals, ContactShadowsOutside, IgnoreMipBias",
		FogType::None, false, "Whether the bumps cast sun shadows and catch the light as real slopes.", nullptr },
	{ "Shaders.Terrain.Displacement", "Test tools", "FixedFactor, DebugView, Winding",
		FogType::None, false, nullptr, nullptr },

	{ "Shaders.POM.Displacement", "Bump shape", "Strength, Center, Contrast, LodBias",
		FogType::None, true, "How far rock and cliff surfaces move in and out.", nullptr },
	{ "Shaders.POM.Displacement", "Detail and reach", "EdgePixels, EdgeFade, MaxFactor, FadeStart, FadeEnd",
		FogType::None, false, "How finely the objects near you are cut up (finer = more work for the graphics card) and how far from you the bumps reach.", nullptr },
	{ "Shaders.POM.Displacement", "Test tools", "SameHeight, FixedFactor, DebugView, Winding",
		FogType::None, false, nullptr, nullptr },

	// UNOFFICIAL 2026-11-01 (user: "group appropriate options together under collapsing menus" on the other pages)
	// Shaders.Cloth.Main
	{ "Shaders.Cloth.Main", "Which clothes move", "Keywords, Exclude, ExcludeParts, AutoDetect, AutoShare, ForceOff",
		FogType::None, false, "Which garments, flags and banners count as cloth, by texture name or by the shape of their mesh.", nullptr },
	{ "Shaders.Cloth.Main", "Which parts of a garment move",
		"MaskMode, SkinMask, PinFraction, SkinSpace, HeightTop, HeightBottom, LegCut, HeightFromFeet, UseSkeleton, LimbGap, LimbFade, "
		"UseWeights, StillWeights",
		FogType::None, false, "Where a coat is pinned (shoulders, chest) and where it hangs loose (the hem), and what stays still on tents and flags.", nullptr },
	{ "Shaders.Cloth.Main", "Wind",
		"UseGameWind, CalmFactor, WindDirX, WindDirY, WindDirZ, WindSpeed, WindIntensity, Wavelength, FlutterSpeed, FlutterAmount, "
		"RippleAmount, RippleScale",
		FogType::None, true, "How strongly and which way the wind blows the cloth, and the size of its billows and flutter.", nullptr },
	{ "Shaders.Cloth.Main", "Coats follow the wearer",
		"MotionDrag, MotionMax, MotionSpring, MotionDamping, StrideSwing, StrideLength, TurnLag, MotionIndoors",
		FogType::None, false, "The hem trails behind as you walk, swishes with your steps and flares out when you turn.", nullptr },
	{ "Shaders.Cloth.Main", "Walking coats",
		"WalkSim, WalkHang, WalkSpring, WalkDamping, WalkDrag, WalkLegMargin, WalkMaxSwing, WalkMaxAngle, WalkDragCap, WalkHemSmooth",
		FogType::None, false, "A small walking simulation: the coat hangs from the hips and swings instead of riding your legs.", nullptr },
	{ "Shaders.Cloth.Main", "Test tools", "LogAuto, LogMatches, TimeOffset",
		FogType::None, false, nullptr, nullptr },

	// Shaders.VolumetricClouds.Main
	{ "Shaders.VolumetricClouds.Main", "Cover and weather",
		"CoveragePleasant, CoverageCloudy, CoverageRainy, CoverageSnow, WeatherSize, FromWeatherTextures, WeatherTint",
		FogType::None, true, "How much of the sky the clouds fill in each kind of weather, and how closely they follow the weather's own clouds.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Cloud shape",
		"ShapeSize, DetailSize, Erosion, EdgeSharpness, ShapeSmoothing, Turbulence, DetailSmoothness, FineDetail, FineDetailSize, CumulusShape",
		FogType::None, false, "The size of the clouds, how wispy or crisp their edges are and how much fine detail they keep close up.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Storm clouds", "Towering, TowerHeight, TowerAnvil",
		FogType::None, false, "A few huge storm clouds towering over the desert, with flat anvil tops.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Cloud lighting", "SunStrength, AmbientStrength, ForwardScattering",
		FogType::None, false, "How brightly the sun and the sky light the clouds, and their silver linings toward the sun.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Movement", "WindSpeed, Evolution, GameTime",
		FogType::None, false, "How fast the clouds drift and change shape.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Cloud shadows",
		"Shadows, ShadowRange, ShadowResolution, ShadowPrefilter, ShadowSoftness, PhysicalShadows, ShadowDebug",
		FogType::None, false, "Drifting cloud shadows on the ground: how dark, how far round you and how soft.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Game clouds and water", "HideGameClouds, WaterReflections, ReflectionDebug",
		FogType::None, false, "Whether the game's own flat clouds stay, and these clouds in the water's reflection.", nullptr },
	{ "Shaders.VolumetricClouds.Main", "Quality and steadiness",
		"HalfResolution, Resolution, Steps, Sharpness, FixedMarchExit, History, Stable, JitterWrap",
		FogType::None, false, "The resolution and sample count the clouds are worked out at (the main cost), and how steady they stay as you move.", nullptr },

	// Shaders.ShadowsInteriors.Main
	{ "Shaders.ShadowsInteriors.Main", "Lamps",
		"LightPoints, ExteriorLightPoints, LightRadiusMult, DrawDistance, ShadowCubeMapSize, NearFade, EdgeFade",
		FogType::None, true, "How many lamps cast shadows at once, how far away and how detailed their shadows are.", nullptr },
	{ "Shaders.ShadowsInteriors.Main", "What casts shadows",
		"UseCastShadowFlag, Activators, Actors, AlphaEnabled, Apparatus, Books, Containers, Doors, Furniture, Misc, Statics, MinRadius, "
		"PlayerShadowThirdPerson, PlayerShadowFirstPerson, TwoSidedCasters, SkinnedAlphaTest",
		FogType::None, false, "Which kinds of objects are drawn into the lamp shadows.", nullptr },
	{ "Shaders.ShadowsInteriors.Main", "Contact shadows", "ContactShadows, ContactLength, ContactThickness, ContactDistance",
		FogType::None, false, "Small dark lines where a prop meets the floor or a wall.", nullptr },
	{ "Shaders.ShadowsInteriors.Main", "Fixes and speed-ups",
		"MaskSkipFar, LampReachMask, ForwardReachMask, PlayerInsideLamp, PlayerLampMargin, RedrawActorsOnly",
		FogType::None, false, "Bug fixes for lamp reach and lamps inside your body, and fewer shadow redraws when people move.", nullptr },

	// Shaders.ShadowsInteriors.Forward
	{ "Shaders.ShadowsInteriors.Forward", "Soft shadows",
		"ContactHardening, ContactHardeningStrength, ContactHardeningMinSoftness, LampSize, MaxSoftness, SmoothEdges, EvenTaps, "
		"MagnifiedSoftness, TexelFilter",
		FogType::None, true, "Shadows sharp where an object touches what it shadows and softer further away, as from a real lamp.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Which lamps get shadows",
		"LampSwitchMargin, LampRanking, KeepLampsInView, CameraIndependent, VisibleRanking, PoolLamps, PoolDrawsPerFrame",
		FogType::None, false, "How the lamps that cast shadows are picked, and extra lamps beyond the usual limit.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Shadow pop-in", "ShadowHold, SteadyLampShadows, FadeIn, FadeOut",
		FogType::None, false, "Lamp shadows fade in and out instead of popping when a lamp gains or loses its shadow.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Nearby lamps share a shadow",
		"ShadowClusterEnabled, ShadowClusterDistance, ShadowClusterInvisibleOnly, ShadowClusterMinLights",
		FogType::None, false, "Lamps close together share the brightest one's shadow (cheaper in rooms full of lights).", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Lamps' own models",
		"LampGlassShadows, OwnGlassDistance, LampModelShadows, LampModelDistance, LampModelOwnDistance, NoShadowLampModels, FixtureDistance",
		FogType::None, false, "Whether a lamp's glass, bulb and fixture shadow its own light.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Invisible and fill lights",
		"InvisibleLightShadowStrength, InvisibleShadowFromLamp, InvisibleLightSize, FillLightRadius, FillLightShadowStrength",
		FogType::None, false, "Shadows from the level designers' invisible lights and big room-filling lights.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Speckles", "Bias, NormalOffset, AutoNormalOffset",
		FogType::None, false, "Keeps lit walls from showing dark speckles where a surface shadows itself.", nullptr },
	{ "Shaders.ShadowsInteriors.Forward", "Room darkening", "KeepDarkening, DarkeningBlur, MaskHalfRes",
		FogType::None, false, "The old darkening of what no shadowed lamp reaches, so rooms keep their mood.", nullptr },

	// Shaders.ShadowsInteriors.Capsules
	{ "Shaders.ShadowsInteriors.Capsules", "People's shadows",
		"DropActors, MaxActors, MaxDistance, PlayerFirstPerson, RadiusScale, Strength, SelfMargin, LampSize, InvisibleLightSize, RankBy",
		FogType::None, true, "Soft lamp shadows of people from simple capsule shapes round their bones.", nullptr },
	{ "Shaders.ShadowsInteriors.Capsules", "Contact shadows",
		"ContactLength, ContactSteps, ContactThickness, ContactStrength, ContactBias, ContactJitter, StartOffset, StartOffsetPerUnit",
		FogType::None, false, "A short ray toward each lamp for the small shadows where things touch.", nullptr },

	// Shaders.ShadowsInteriors.RT
	{ "Shaders.ShadowsInteriors.RT", "Soft shadows", "Rays, LampRadius, InvisibleLightRadius, Jitter",
		FogType::None, true, "The size of the lamp the rays aim at, for soft shadow edges.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Smoothing the noise",
		"Denoise, DenoiseHistory, Filter, FilterStep, FilterDepthSigma, FilterNormalPower",
		FogType::None, false, "Smooths the grain of soft traced shadows over frames and neighbouring pixels.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Every lamp on screen",
		"ManyLights, ManyLightsMax, ManyLightsRays, ManyLightsSpatial, ManyLightsHistory, ManyLightsRestStride, ManyLightsOneRay",
		FogType::None, false, "Every lamp that lights something on screen gets a traced shadow, not only the ones with shadow maps.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Where rays start and stop",
		"StartOffset, StartOffsetPerUnit, LampStop, SkipLampModels, SkipLampGlass, DepthTolerance",
		FogType::None, false, "Keeps surfaces, bulbs and lamp fixtures from shadowing their own light.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Outdoors at night", "Exteriors, ExteriorForward, ExteriorLampStop, ExteriorDebug",
		FogType::None, false, "Traced shadows from street lamps and other lights outdoors at night.", nullptr },
	{ "Shaders.ShadowsInteriors.RT", "Test tools", "DebugView, OneSided, DebugLamp",
		FogType::None, false, nullptr, nullptr },

	// Shaders.Bodycam.Main
	{ "Shaders.Bodycam.Main", "Lens",
		"Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, LensDirt, Autofocus, WideView",
		FogType::None, true, "The cheap wide lens: bent edges, colour fringes, blur, dirt and a wide field of view.", nullptr },
	{ "Shaders.Bodycam.Main", "Colour", "Exposure, Contrast, Saturation, Lift, Cool, Tint",
		FogType::None, false, "Brightness, contrast and colour of the footage.", nullptr },
	{ "Shaders.Bodycam.Main", "Bright and low light", "Glow, AutoExposure, ExposureSpeed, Overload, LowLight, LightSmear",
		FogType::None, false, "How the camera copes with bright light, muzzle flashes and the dark.", nullptr },
	{ "Shaders.Bodycam.Main", "Video and compression", "Sharpen, Grain, Blocks, VideoFps, Breakup, Hiccups",
		FogType::None, false, "Sensor noise, compression blocks and the video's frame rate.", nullptr },
	{ "Shaders.Bodycam.Main", "Camera movement", "MotionBlur, RollingShutter, Bounce, GunJolt",
		FogType::None, false, "The chest-mounted camera bobs with your steps, smears and wobbles in fast turns and kicks with each shot.", nullptr },
	{ "Shaders.Bodycam.Main", "Microphone", "Mic, MicTone, MicCompression, MicGain, MicBits, MicSampleRate, MicHiss",
		FogType::None, false, "All the game's sound as if picked up by the camera's own cheap microphone.", nullptr },

	// Shaders.Bodycam.Night
	{ "Shaders.Bodycam.Night", "Lens",
		"Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, LensDirt, Autofocus",
		FogType::None, true, "At night outdoors: the cheap wide lens.", nullptr },
	{ "Shaders.Bodycam.Night", "Colour", "Exposure, Contrast, Saturation, Lift, Cool, Tint",
		FogType::None, false, "At night outdoors: brightness, contrast and colour.", nullptr },
	{ "Shaders.Bodycam.Night", "Bright and low light", "Glow, AutoExposure, ExposureSpeed, Overload, LowLight, LightSmear",
		FogType::None, false, "At night outdoors: bright lights, muzzle flashes and low-light mode.", nullptr },
	{ "Shaders.Bodycam.Night", "Video and compression", "Sharpen, Grain, Blocks, VideoFps, Breakup, Hiccups",
		FogType::None, false, "At night outdoors: noise, compression blocks and frame rate.", nullptr },
	{ "Shaders.Bodycam.Night", "Camera movement", "MotionBlur, RollingShutter, Bounce, GunJolt",
		FogType::None, false, "At night outdoors: bob, smear, wobble and shot kick.", nullptr },

	// Shaders.Bodycam.Interiors
	{ "Shaders.Bodycam.Interiors", "Lens",
		"Fisheye, FitEdges, Border, Fringe, EdgeBlur, LensBlur, Vignette, LensDirt, Autofocus",
		FogType::None, true, "Indoors: the cheap wide lens.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Colour", "Exposure, Contrast, Saturation, Lift, Cool, Tint",
		FogType::None, false, "Indoors: brightness, contrast and colour.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Bright and low light", "Glow, AutoExposure, ExposureSpeed, Overload, LowLight, LightSmear",
		FogType::None, false, "Indoors: bright lights, muzzle flashes and low-light mode.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Video and compression", "Sharpen, Grain, Blocks, VideoFps, Breakup, Hiccups",
		FogType::None, false, "Indoors: noise, compression blocks and frame rate.", nullptr },
	{ "Shaders.Bodycam.Interiors", "Camera movement", "MotionBlur, RollingShutter, Bounce, GunJolt",
		FogType::None, false, "Indoors: bob, smear, wobble and shot kick.", nullptr },

	// Shaders.BloodFlow.Main
	{ "Shaders.BloodFlow.Main", "Streams", "DripLength, FlowSpeed, Viscosity, MaxFall, StreamWidth, DryTime",
		FogType::None, true, "How far and how fast blood runs down walls and slopes from a splat, and how soon it dries.", nullptr },
	{ "Shaders.BloodFlow.Main", "Pools", "PoolSize, PoolSpreadTime, PoolDryTime, GibPools, GibHold",
		FogType::None, false, "The pools that spread under bodies and cut-off limbs.", nullptr },
	{ "Shaders.BloodFlow.Main", "Splat shape", "Splat, SplatReach, SplatSpray, SplatDrips",
		FogType::None, false, "Each splat gets its own impact shape, with arms, spray lines and drips.", nullptr },
	{ "Shaders.BloodFlow.Main", "Colour",
		"FreshColorR, FreshColorG, FreshColorB, DryColorR, DryColorG, DryColorB, MatchGameColor, MatchShade, MatchBrightness, MatchFloor",
		FogType::None, false, "The colour of fresh and dried blood, or the game's own splat colour.", nullptr },
	{ "Shaders.BloodFlow.Main", "Wet look", "Gloss, Sheen, SheenTint, Beads, BeadSize, OldLook",
		FogType::None, false, "How fresh blood shines and mirrors its surroundings, and its glossy drops.", nullptr },
	{ "Shaders.BloodFlow.Main", "Limits and speed",
		"CheapGather, SplatFast, SplatMask, Merge, MergeCount, MaxDistance, MaxSplats, MaxPools",
		FogType::None, false, "How many splats and pools are drawn and how far away, and cheaper ways to draw them.", nullptr },
	{ "Shaders.BloodFlow.Main", "Test tools", "GroundTestUnseen, TestSplats, DebugView",
		FogType::None, false, nullptr, nullptr },

	// Shaders.BloodFlow.Fluid
	{ "Shaders.BloodFlow.Fluid", "How the blood flows", "SettleTime, BleedTime, Viscosity, Cling, Friction, Lifetime",
		FogType::None, true, "How long the blood keeps moving, how thick it is and how it clings to walls.", nullptr },
	{ "Shaders.BloodFlow.Fluid", "Fluid look", "StainSize, Threshold, Bump, Drops, DropSize, TextureRelief, SteadyContact, SteadyFloor, RestSpeed, SteadyTiles, TileCap",
		FogType::None, false, "How the fluid's stains, edges and drops look.", nullptr },
	{ "Shaders.BloodFlow.Fluid", "Test tools", "OldThrow, OldColour, DebugView, TestThrow, TestPour, StatsEvery",
		FogType::None, false, nullptr, nullptr },

	// Shaders.AmbientOcclusion.RT
	{ "Shaders.AmbientOcclusion.RT", "Sky visibility", "Sky, SkyRays, SkyDistance",
		FogType::None, false, "How much of the sky each spot sees, from rays instead of a top-down map.", nullptr },
	{ "Shaders.AmbientOcclusion.RT", "Bounce light",
		"GI, GIDistance, GIStrength, GIOffscreen, GIScreenLight, GIGameLamps, GIClamp, GIAmbient, GISpatial, GIReuse, GIInterleave, GIMovingHistory, GlowGI, GlowDirect",
		FogType::None, true, "One bounce of light from walls and floors, and glowing signs and windows lighting what is near them.", nullptr },
	{ "Shaders.AmbientOcclusion.RT", "Light that bounces more than once",
		"RadianceCache, CacheStrength, CacheCellSize, CacheRays, CacheFrames, CacheExteriors, CacheSpread, CacheRaysExterior, CacheSpreadExterior",
		FogType::None, false, "A grid of light round you, for light that bounces more than once.", nullptr },

	// Shaders.Reflections.Main
	{ "Shaders.Reflections.Main", "Quality and reach", "MaxDistance, Steps, Thickness, Temporal, BlurRadius, DrawDistance",
		FogType::None, true, "How far the reflections reach and how steady and smooth they look (more steps cost more).", nullptr },
	{ "Shaders.Reflections.Main", "When rays find nothing",
		"SkyFallback, CubeMap, CubeCapture, CubeFade, GridFallback, GridBrightness, CloudSky",
		FogType::None, false, "What a reflection shows where the screen has nothing to mirror: the sky, a live cube of the surroundings or the light grid.", nullptr },
	{ "Shaders.Reflections.Main", "Materials",
		"MaterialAware, MaterialReflect, RoughnessOverrides, MaterialMaxRoughness, MaterialBufferRoughness, GlassReflect",
		FogType::None, false, "Reflections that follow each surface's own material (polished, rough, metal, glass).", nullptr },
	{ "Shaders.Reflections.Main", "Blending with the surface", "ReflectAdd, SpecularOcclusion, SpecularOcclusionLevel",
		FogType::None, false, "Whether the reflection replaces or adds to the surface, and darker reflections where the room is dark.", nullptr },
	{ "Shaders.Reflections.Main", "Faster ray walk (test)", "HiZ, HiZSafeCells, HiZCheck, HiZInteriorsTest",
		FogType::None, false, "Measured slower: leave off.", nullptr },

	// Shaders.Reflections.RT
	{ "Shaders.Reflections.RT", "What reflections show off screen",
		"ShadeOffscreen, ScreenTolerance, EstimateBrightness, SunShadows, HitLamps, HitAmbientGrid, HitTextures, GlowBrightness, SkyPanorama",
		FogType::None, true, "How things behind you or hidden from view are lit and coloured when they show in a reflection.", nullptr },
	{ "Shaders.Reflections.RT", "Glossy surfaces",
		"VisibleNormals, RayReuse, ReuseRoughness, ContactSharpness, ContactSpread, RoughShare, RoughReach",
		FogType::None, false, "Blurred reflections on worn tiles, satin metal and wet stone: how sharp they stay and how cheap they are.", nullptr },
	{ "Shaders.Reflections.RT", "Weapon", "Weapon, WeaponMetal, WeaponRoughness",
		FogType::None, false, "Your first-person weapon reflects the ray traced scene.", nullptr },
	{ "Shaders.Reflections.RT", "Water", "Water, WaterSkipPlanar, WaterPlanarRefresh, WaterRoughness",
		FogType::None, false, "Water reflects the ray traced scene instead of the game's own water reflection.", nullptr },

	// Shaders.Sandstorm.Main
	{ "Shaders.Sandstorm.Main", "Dust shape", "HeightFalloff, Billowing, NoiseScale, StreakStrength",
		FogType::None, true, "How the dust gathers into rolling clouds and streaks of blown sand.", nullptr },
	{ "Shaders.Sandstorm.Main", "Dust colour and light",
		"DustColorR, DustColorG, DustColorB, Brightness, SunScattering, LightScattering",
		FogType::None, false, "The dust's colour and how it glows toward the sun and round lamps and fires.", nullptr },
	{ "Shaders.Sandstorm.Main", "Wind", "WindSpeed, MinWindSpeed, GrassWind, TreeWind",
		FogType::None, false, "How fast the dust blows and how much harder grass and trees sway in the storm.", nullptr },
	{ "Shaders.Sandstorm.Main", "Fire and glass", "EmissiveThreshold, EmissiveDistance, ThroughBlended, CharactersKeepDepth",
		FogType::None, false, "Keeps fire, sparks and glass from showing through the dust as if it were not there.", nullptr },
	{ "Shaders.Sandstorm.Main", "Speed", "Steps, Octaves, SoftEdges, FineShelter, FastPath",
		FogType::None, false, "Cheaper or finer dust.", nullptr },

	// Shaders.Sky.Main
	{ "Shaders.Sky.Main", "Sky colour",
		"ColorLook, TintDayR, TintDayG, TintDayB, TintNightR, TintNightG, TintNightB, TintKeepsBrightness, SaturationDay, SaturationNight",
		FogType::None, true, "The colour and colourfulness of the sky by day and at night.", nullptr },
	{ "Shaders.Sky.Main", "Horizon colour",
		"TintHorizonDayR, TintHorizonDayG, TintHorizonDayB, TintHorizonNightR, TintHorizonNightG, TintHorizonNightB, TintGradientHeight",
		FogType::None, false, "An extra colour for the sky near the horizon, and how high it reaches.", nullptr },
	{ "Shaders.Sky.Main", "Sunrise and sunset", "UseSunDiskColor, SunsetR, SunsetG, SunsetB",
		FogType::None, false, "The colour the sun gives the sky at sunrise and sunset.", nullptr },
	{ "Shaders.Sky.Main", "Stars and moon", "StarStrength, StarTwinkle, MoonBrightness",
		FogType::None, false, nullptr, nullptr },

	// Shaders.AmbientLight.Main
	{ "Shaders.AmbientLight.Main", "Edges and outlines", "NoEdgeOutline, EdgeReject, LowScaleFullRes, TemporalJitterFix, MinReach, NormalSmoothing, NormalSmoothingWorld, HorizonBias",
		FogType::None, true, "Fixes for thin light outlines round things and dark facets on low-poly surfaces.", nullptr },
	{ "Shaders.AmbientLight.Main", "Light grid bounce",
		"GridBounce, GridCellSize, GridSteps, GridSpread, GridScreenStrength, GridBlocking, GridWallMemory, GridLamps, GridFine, GridColour, AlbedoBuffer",
		FogType::None, false, "Bounce light from a 3D grid of light round you, from the sun by day and the lamps indoors and at night.", nullptr },
	{ "Shaders.AmbientLight.Main", "Screen bounce light",
		"ComputeGI, ComputeGIStrength, ComputeGIThickness, ComputeGIThinOcclusion, ComputeGIQuality, ComputeGITest",
		FogType::None, false, "Light bounced from what is on screen (needs the DXVK fork).", nullptr },

	// Shaders.WetWorld.Main
	{ "Shaders.WetWorld.Main", "Wet ground that lasts",
		"Persistent, PersistentWetTime, PersistentDryTime, PersistentPuddleFill, PersistentPuddleDry, PersistentShade, PersistentMapSize, "
		"PersistentMapResolution, PersistentDebugView",
		FogType::None, false, "Wet ground and puddles are kept spot by spot round you and dry slowly, faster in the sun.", nullptr },
	{ "Shaders.WetWorld.Main", "Not used", "PuddleCoeff_R, PuddleCoeff_G, PuddleCoeff_B, PuddleSpecularMultiplier",
		FogType::None, false, "These settings do nothing.", nullptr },

	// Shaders.Grass.GPUGrass
	{ "Shaders.Grass.GPUGrass", "Distance and detail",
		"BladeDistance, Blades, BladesFar, LodNear, LodFar, DetailDistance, MinPixelWidth, MaxBlades",
		FogType::None, true, "How far the strands reach and how many each clump has near and far (more costs more).", nullptr },
	{ "Shaders.Grass.GPUGrass", "Strand shape", "LengthMin, TipReach, Spread, Bend, Kink, Droop, Flutter",
		FogType::None, false, "How long, bent and spread out the strands are, and their flutter in the wind.", nullptr },
	{ "Shaders.Grass.GPUGrass", "Colour", "Variation, TipLift, Saturation",
		FogType::None, false, nullptr, nullptr },

	// Shaders.PBR.Main
	{ "Shaders.PBR.Main", "Sky light", "SkylightingScale, SkylightingMode, SkylightingDirectionality, SkylightingSaturation",
		FogType::None, true, "How much the sky's colour lights objects from above and around.", nullptr },
	{ "Shaders.PBR.Main", "Hair shine", "HairHighlight, HairHighlightGameLook",
		FogType::None, true, "The hair has its own shader: Reflectivity and Roughness do not reach it. These set its shine.", nullptr },
	{ "Shaders.PBR.Main", "Material maps", "MetalnessScale, AOScale, RoughnessScale",
		FogType::None, false, "For materials with an _rmaos map: how much of their metal, occlusion and roughness applies.", nullptr },
	{ "Shaders.PBR.Main", "Highlights",
		"SmithCorrelated, SmithCorrelatedTest, EnergySplit, SpecularDistance, SpecularFade, FoldHighlights, HighlightSums",
		FogType::None, false, "How the shiny highlights on objects are worked out and how far they reach.", nullptr },
	{ "Shaders.PBR.Main", "Lamp light", "LampDistanceFix, LampSums, LampReachSkip, DxvkLampBranches",
		FogType::None, false, "Fixes and speed-ups for objects lit by several lamps.", nullptr },
	{ "Shaders.PBR.Main", "Shader marking (test)",
		"MarkShadersFrom, MarkShadersTo, MarkShadersVanilla, MarkShadersVsFrom, MarkShadersVsTo, MarkShadersFamily",
		FogType::None, false, nullptr, nullptr },

	// Main.Main.Performance (the DXVK group above stays as it is)
	{ "Main.Main.Performance", "Shader compiling and warm-up",
		"ParallelShaderCompile, NewEffectCompiler, ShaderWarmUp, ShaderWarmUp2, ShaderWarmUp2BudgetMs, ShaderWarmUp2MinMs",
		FogType::None, true, "How the mod's shaders are compiled at game start and preloaded against stutter.", nullptr },
	{ "Main.Main.Performance", "ReShade",
		"ReShadeDepthCompat, ReShadeNVRDepth, ReShadeBeforeHUD, ReShadeUnderNVRMenu, ReShadeDepthNearRemap, ReShadeMenuGuard, "
		"ReShadeMenuGuardEffects, ReShadeMenuGuardVATS, ReShadeGameValues, ReShadeDepthCheck",
		FogType::None, false, "For ReShade users: depth for ReShade's effects, ReShade under the HUD and menus, and pausing effects in menus.", nullptr },
	{ "Main.Main.Performance", "CPU savings", "SkipRedundantConstants, SettingsCache, PreDecodeCache, MenuHiddenLean",
		FogType::None, false, "Small savings on the processor: work skipped when nothing changed.", nullptr },
	{ "Main.Main.Performance", "Full-screen effect passes",
		"FrameChain, CompositeApply, ChainUsesGameTexture, MergedDepthNormals, SkipIdentityExposure, SlimDepthBuffer",
		FogType::None, false, "Fewer whole-frame copies and passes between the mod's effects.", nullptr },
	{ "Main.Main.Performance", "Lamp light passes",
		"LampScissor, LampScissorExact, TrimLampPasses, LampMaskScissor, LampMaskScissorCheck, LampPassTestSkip, LampPassOrderLog",
		FogType::None, false, "Indoors, each extra lamp's light is drawn only where the lamp reaches.", nullptr },
	{ "Main.Main.Performance", "Indoor lamp shadows",
		"SkinnedShadowFaceTest, FastCasterHash, CasterListCache, InteriorShadowFold, InteriorShadowFoldRound, InteriorShadowFoldCheck, "
		"CubeFaceCopySkip, CubeFaceCopySkipCheck, CubeFaceViewCull, CubeFaceViewCullCheck",
		FogType::None, false, "Less work redrawing the lamp shadows indoors.", nullptr },

	// Main.Main.RayTracing
	{ "Main.Main.RayTracing", "People", "Actors, ActorUpdateInterval, ActorRadius, ActorStillSkip, ActorStillThreshold, ActorRebuildInterval",
		FogType::None, true, "NPCs, creatures and your own body in the ray traced scene, and how often they are updated.", nullptr },
	{ "Main.Main.RayTracing", "Plants and grass", "LeafCards, LeafCardsOpaque, PlantSway, Grass, GrassRadius, GrassMaxTufts",
		FogType::None, false, "Tree leaves, bushes and the grass near you in the ray traced scene.", nullptr },
	{ "Main.Main.RayTracing", "Textures and see-through meshes", "AlphaTested, HitTextures, GlowTextures",
		FogType::None, false, "Fences and foliage cards with their holes, and each mesh's texture and glow where a ray hits it.", nullptr },
	{ "Main.Main.RayTracing", "Far distance", "FarObjectLOD, FarLandLOD, FarRadius",
		FogType::None, false, "Distant objects and terrain in the ray traced scene (for RT reflections).", nullptr },
	{ "Main.Main.RayTracing", "Building the scene", "BuildTrianglesPerFrame, BuildNearRadius, CullSmallAngle, Compact, ArenaMB",
		FogType::None, false, "How new objects are added to the ray traced scene without stutter, and its memory.", nullptr },
};

static bool KeyInList(const char* list, const char* key) {
	const size_t n = strlen(key);
	for (const char* p = list; *p; ) {
		while (*p == ' ' || *p == ',') p++;
		const char* end = p;
		while (*end && *end != ',') end++;
		const char* last = end;
		while (last > p && last[-1] == ' ') last--;
		if ((size_t)(last - p) == n && strncmp(p, key, n) == 0) return true;
		p = end;
	}
	return false;
}

static const MenuGroup* FindMenuGroup(const std::string& section, const char* key) {
	for (const MenuGroup& g : kMenuGroups)
		if (section == g.Section && KeyInList(g.Keys, key)) return &g;
	return nullptr;
}

// A key its group's switch draws instead (LegacyFog: the legacy fog's switch).
static bool IsGroupSwitchKey(const std::string& section, const char* key) {
	return (section == kFogMain && strcmp(key, "LegacyFog") == 0) ||
		(section == "Shaders.VolumetricFog.Interiors" && strcmp(key, "LampHazeEnabled") == 0) ||	// 2026-11-04: the Lamp haze group's switch
		(section == "Shaders.Exposure.Main" && strcmp(key, "Mode") == 0) ||	// 2026-11-04: the Eye adaptation group's switch
		(section == "Shaders.Tonemapping.Main" && strcmp(key, "NightShiftEnabled") == 0);	// 2026-11-04: the Night colour shift group's switch
}

// What a page is for, in plain words, under its title.
static void TessellationStatus() {
	const RestartSetting* r = FindRestartSetting("Main.Main.Performance", "NvrDxvkDisplacement");
	const bool atStart = r && RestartState(*r, true) != 0.0f;
	const ImVec4 orange(1.0f, 0.6f, 0.2f, 1.0f), green(0.45f, 0.9f, 0.45f, 1.0f);
	if (!DxvkCompute::Available())
		ImGui::TextColored(orange, "Not available in this game: the DXVK fork's d3d9.dll is not loaded (native D3D9 or stock DXVK).");
	else if (!atStart)
		ImGui::TextColored(orange, "Not set up: NvrDxvkDisplacement (Main > Performance > DXVK) was off when the game started. Tick it, press Save, restart the game.");
	else if (!DxvkCompute::DisplaceOn())
		ImGui::TextColored(orange, "NvrDxvkDisplacement is on, but no land shader took the tessellation: this DXVK fork may be older than 1.7-displace.");
	else
		ImGui::TextColored(green, "Set up: Enabled below turns the bumps on and off at once.");
}

struct SectionHelp { const char* Section; const char* Text; void (*Status)(); };
static const SectionHelp kSectionHelp[] = {
	{ "Shaders.Terrain.Displacement",
		"Tessellation gives the ground real 3D bumps (stones, ruts, mounds) instead of only painted-on depth: near you the land is cut "
		"into many small triangles and each point is raised or lowered by the ground texture's height map, so the bumps show against the "
		"sky, cast shadows and catch the light. Two switches: NvrDxvkDisplacement (Main > Performance > DXVK) sets it up when the game "
		"starts, and Enabled below turns the bumps on and off at once. Needs the DXVK fork's d3d9.dll. Off by default while it is being "
		"finished: the bumps can still flicker round objects.", TessellationStatus },
	{ "Shaders.POM.Displacement",
		"The same tessellation as the ground's (Terrain > Tessellation) for rocks, cliffs and the other objects with a parallax height "
		"map: their outlines get real bumps. NvrDxvkDisplacement (Main > Performance > DXVK) sets it up when the game starts; Enabled "
		"below turns the object bumps on and off at once, apart from the ground's. Needs the DXVK fork's d3d9.dll. Experimental.", TessellationStatus },
	{ kFogMain,
		"Three fog types, one at a time: the current fog (this build's), the classic fog (the original look) and the legacy fog (for "
		"older presets). Tick \"Use ...\" at the top of the one you want: the other two turn off. The shared settings apply to all three.", nullptr },
	{ "Shaders.VolumetricFog.Interiors",
		"The fog indoors. The fog type (current, classic or legacy) is the same indoors and out: the switches here and on the Main page "
		"are the same ones.", nullptr },
};

// A page's help text (kSectionHelp), wrapped at the page's edge, with its live status line under it.
static void RenderSectionHelp(const std::string& section) {
	for (const SectionHelp& help : kSectionHelp) {
		if (section != help.Section) continue;
		ImGui::PushTextWrapPos(0.0f);	// (TextColored, used by the status lines, does not wrap by itself)
		ImGui::TextUnformatted(help.Text);
		if (help.Status) help.Status();
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}
}

// What ImGui shows of a label: the part before "##".
static std::string ShownText(const std::string& label) {
	const size_t hidden = label.find("##");
	return hidden == std::string::npos ? label : label.substr(0, hidden);
}

static std::string MenuEntryLabel(const std::string& name);

// The shader a Shaders.<Name>.<Page> section belongs to, or "" for any other section.
static std::string ShaderOfPage(const std::string& section) {
	if (section.find("Shaders") != 0) return std::string();
	const size_t first = section.find('.');
	const size_t second = section.find('.', first + 1);
	if (first == std::string::npos || second == std::string::npos) return std::string();
	return section.substr(first + 1, second - first - 1);
}

// The shader's own switch at the right end of a shader page's title line: the whole shader (every section of it), not just
// the page shown. Insert does the same while the page is open.
static void RenderShaderHeaderSwitch(const std::string& shaderName) {
	const char* shader = shaderName.c_str();
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(shader);
	if (kind == SettingManager::MenuSwitch::None) return;	// UNOFFICIAL 2026-10-19: an entry without a switch gets none
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	const bool forced = TheSettingManager->IsShaderForced(shader);

	// Cloth and the like are read at start: the same note as on their Main page, left of the switch
	RestartNote note;
	const bool noted = kind == SettingManager::MenuSwitch::MainEnabled &&
		FindRestartNote(("Shaders." + shaderName + ".Main").c_str(), "Enabled", note);
	const float gap = ImGui::GetStyle().ItemSpacing.x;
	const float wanted = MenuWidgets::ToggleWidth() + (noted ? ImGui::CalcTextSize(note.text).x + gap : 0.0f);
	ImGui::SameLine();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - wanted;
	if (right > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right);
	if (noted) {
		DrawRestartNote(note);
		ImGui::SameLine();
	}
	ImGui::BeginDisabled(forced);
	if (MenuWidgets::Toggle("##headerSwitch", &on) && (!on || ConfirmEffectOn(shaderName))) {	// (2026-11-05: conflicts ask first)
		TheShaderManager->SwitchShaderStatus(shader);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Turns all of %s on or off (every %s section), not just this section (key: Insert).\n"
			"This section's own switches are in the list below.", shader, shader);
	ImGui::EndDisabled();
}

// A page's title area: the shader's (or the section's) name, large, the shader's switch at the right, and the section's full
// path as a dim line under it.
static void RenderPageHeader(const std::string& section) {
	const std::string shader = ShaderOfPage(section);
	const std::string title = shader.empty() ? section.substr(section.rfind('.') + 1) : ShownText(MenuEntryLabel(shader));
	std::string path;
	for (char c : section) path += c == '.' ? std::string(" > ") : std::string(1, c);

	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(title.c_str());
	ImGui::PopStyleColor();
	if (!shader.empty()) RenderShaderHeaderSwitch(shader);
	ImGui::TextDisabled("%s", path.c_str());
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

// ---- The LUT page --------------------------------------------------------------------------------------------------------
// DayLUT, NightLUT and InteriorLUT are not rows (ShouldHideKey): each is a picker that steps through the LUT files and loads
// the one shown. Their values at menu open are kept here, as the rows keep theirs.

static void RenderLutPicker(LUTEffect* lut, const char* label, const char* key, int& index, int slot) {
	const std::string& atOpen = SessionValue("Shaders.LUT.Main", key, lut->LUTFiles[index].c_str());
	const bool changed = lut->LUTFiles[index] != atOpen;
	ImGui::PushID(key);
	const float width = MenuWidgets::RowBegin(label, MenuWidgets::ResetSlotWidth());
	int step = 0;
	ImGui::BeginGroup();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) step = -1;
	ImGui::SameLine();
	const float arrowW = ImGui::GetFrameHeight();
	const float nameW = ImMax(width - arrowW * 2.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f, arrowW);
	const float nameX = ImGui::GetCursorPosX();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(lut->LUTFiles[index].c_str());
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::SetCursorPosX(nameX + nameW + ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::ArrowButton("##next", ImGuiDir_Right)) step = 1;
	ImGui::EndGroup();
	if (ImGui::IsItemHovered() && (gMenu.plusKey || gMenu.minusKey)) step = gMenu.plusKey ? 1 : -1;
	if (MenuWidgets::ResetButton("~", changed, "Revert to value at session start"))
		lut->LoadLUT(slot, atOpen.c_str());	// LoadLUT puts index and the texture in step (AssignLUTSlot)
	if (step) {
		const int count = (int)lut->LUTFiles.size();
		index = ((index + step) % count + count) % count;
		lut->LoadLUT(slot, lut->LUTFiles[index].c_str());
	}
	ImGui::PopID();
}

// Above the LUT page's settings: what the pre-tonemapping mode expects, then the three pickers.
static void RenderLutPage() {
	LUTEffect* lut = TheShaderManager->Effects.LUT;
	if (!lut) return;
	if (lut->Settings.PreTonemapping) {
		const bool compat = lut->Settings.HDRCompat;
		ImGui::TextColored(compat ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%s", compat
			? "HDR Compat: SDR LUTs normalized to HDR range (approximate)."
			: "Pre-tonemapping mode: use HDR LUTs (float16 DDS converted from .CUBE).");
	}
	if (lut->LUTFiles.empty()) {
		ImGui::TextDisabled("No LUTs found in %s", LUTEffect::LUTFolder());
	}
	else {
		RenderLutPicker(lut, "Day", "DayLUT", lut->DayIdx, 0);
		RenderLutPicker(lut, "Night", "NightLUT", lut->NightIdx, 1);
		RenderLutPicker(lut, "Interior", "InteriorLUT", lut->InteriorIdx, 2);
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

// Puts the pinned settings first, then the page's own Enabled switch (both orderings are stable).
static void PageOrder(SettingManager::Configuration::SettingList& settings) {
	// The settings players look for first go to the top of their section; the rest stay alphabetical.
	static const char* const kPinnedFirst[][2] = {
		{ "Shaders.PBR.Main", "ReflectivityExterior" },
		{ "Shaders.PBR.Main", "ReflectivityInterior" },
	};
	std::stable_partition(settings.begin(), settings.end(), [](const SettingManager::Configuration::ConfigNode& n) {
		for (const auto& pin : kPinnedFirst)
			if (gMenu.page == pin[0] && strcmp(n.Key, pin[1]) == 0) return true;
		return false;
	});
	// A page's own on/off switch comes before everything else.
	std::stable_partition(settings.begin(), settings.end(), [](const SettingManager::Configuration::ConfigNode& n) {
		return strcmp(n.Key, "Enabled") == 0;
	});
}

// What the page needs to know about one of its settings. Worked out once per frame (see RenderSettingsList).
struct PageEntry {
	const MenuGroup* group;	// the menu group it is drawn under, or nullptr for the ungrouped part
	bool             hidden;	// never drawn as a row (ShouldHideKey)
	bool             test;	// a test/developer setting (IsTestSetting)
};

// Draws every setting of the page: the ungrouped ones first, then each menu group under its own header.
// The group, hidden and test facts of each setting are computed once per frame, not in every pass over the list.
// The simple view of a page (kSimpleRows): its rows in the table's order, and how many more Advanced shows.
static void RenderSimpleList(SettingManager::Configuration::SettingList& settings, bool isShader) {
	int shown = 0, rows = 0;
	for (const auto& s : settings) if (!ShouldHideKey(s.Key) && !IsTestSetting(s)) rows++;
	for (const SimpleRow& r : kSimpleRows) {
		if (gMenu.page != r.Section) continue;
		if (!r.Key) {
			DrawSimpleChoice(r);
			continue;
		}
		for (size_t i = 0; i < settings.size(); i++) {
			if (strcmp(settings[i].Key, r.Key)) continue;
			// A colour (its R key listed): the page's colour wheel for R, G and B, under the plain name
			const std::string key(r.Key), prefix = key.substr(0, key.size() - 1);
			SettingManager::Configuration::ConfigNode *g = nullptr, *b = nullptr;
			if (isShader && key.size() > 1 && key.back() == 'R')
				for (auto& other : settings) {
					if (prefix + "G" == other.Key) g = &other;
					if (prefix + "B" == other.Key) b = &other;
				}
			if (g && b) {
				RenderColorTriple(settings[i], *g, *b, prefix, r.Label);
				shown += 3;
			}
			else {
				RenderSetting(settings[i], isShader);
				shown++;
			}
		}
	}
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	if (rows > shown)
		ImGui::TextDisabled("%d more setting%s here with Advanced on (top row)", rows - shown, rows - shown == 1 ? "" : "s");
}

static void RenderSettingsList(SettingManager::Configuration::SettingList& settings) {
	const bool isShader = (gMenu.page.find("Shaders") == 0);
	const bool devShown = DevOptionsShown();
	if (!AdvancedShown() && HasSimpleView(gMenu.page)) return RenderSimpleList(settings, isShader);	// 2026-11-05

	std::vector<PageEntry> facts;
	facts.reserve(settings.size());
	for (const auto& s : settings)
		facts.push_back({ FindMenuGroup(gMenu.page, s.Key), ShouldHideKey(s.Key), IsTestSetting(s) });

	// Where each key is in the list, to find the R/G/B siblings of a colour triple
	std::unordered_map<std::string, size_t> position;
	for (size_t i = 0; i < settings.size(); i++) position[settings[i].Key] = i;

	std::unordered_set<std::string> handled;
	int testHidden = 0, shown = 0;	// UNOFFICIAL 2026-10-19 menu honesty

	auto drawOne = [&](size_t i) {
		SettingManager::Configuration::ConfigNode& s = settings[i];
		const std::string key(s.Key);
		if (handled.count(key)) return;
		if (facts[i].hidden) return;
		if (IsGroupSwitchKey(gMenu.page, s.Key)) return;	// drawn as its group's switch
		if (!devShown && facts[i].test) { testHidden++; return; }	// UNOFFICIAL 2026-10-19: test switches, dead keys (2026-10-25: + DevOnly)
		shown++;

		// HDRCompat only means something with pre-tonemapping on
		const LUTEffect* lut = TheShaderManager->Effects.LUT;
		if (lut && !lut->Settings.PreTonemapping && gMenu.page == "Shaders.LUT.Main" && !strcmp(s.Key, "HDRCompat")) return;

		// <prefix>R/G/B on a shader page: one colour wheel. The list is alphabetical (B, G, R), so whichever channel comes
		// first starts it.
		const char last = key.empty() ? '\0' : key.back();
		const bool channel = key.size() > 1 && (last == 'R' || last == 'G' || last == 'B');
		if (isShader && channel) {
			const std::string prefix = key.substr(0, key.size() - 1);
			const std::string names[3] = { prefix + "R", prefix + "G", prefix + "B" };
			const bool curve = prefix == "ColorCurve" && strstr(s.Section, "Shaders.Coloring");	// ColorCurveR/G/B are three curves
			const bool complete = position.count(names[0]) && position.count(names[1]) && position.count(names[2]);
			if (!curve && complete && !handled.count(names[0])) {
				for (const std::string& n : names) handled.insert(n);
				RenderColorTriple(settings[position[names[0]]], settings[position[names[1]]], settings[position[names[2]]], prefix);
				return;
			}
		}
		RenderSetting(s, isShader);
	};

	for (size_t i = 0; i < settings.size(); i++)
		if (!facts[i].group) drawOne(i);

	for (const MenuGroup& g : kMenuGroups) {
		if (gMenu.page != g.Section) continue;
		// A group with nothing to show (all its keys developer-only) is left out, unless it carries a fog switch.
		bool any = g.Fog != FogType::None || g.Switch;
		for (size_t i = 0; i < settings.size(); i++)
			if (!any && facts[i].group == &g && !facts[i].hidden && (devShown || !facts[i].test)) any = true;
		if (!any) {
			for (size_t i = 0; i < settings.size(); i++)
				if (facts[i].group == &g && !facts[i].hidden) testHidden++;
			continue;
		}
		const bool active = (g.Fog != FogType::None && ActiveFog() == g.Fog) || (g.Switch && g.Switch->Get());
		const std::string header = std::string(g.Header) + (active ? "   (on)" : "") + "###group" + g.Header;
		if (!MenuWidgets::SubHeader(header.c_str(), g.Open || active)) {
			for (size_t i = 0; i < settings.size(); i++)
				if (facts[i].group == &g && !facts[i].hidden && !devShown && facts[i].test) testHidden++;
			continue;
		}
		ImGui::PushID(g.Header);
		if (g.Fog != FogType::None) FogSwitch(g.Fog);
		if (g.Switch) DrawGroupSwitch(*g.Switch);
		ImGui::PushTextWrapPos(0.0f);	// the note and the status line wrap at the page's edge
		if (g.Note) ImGui::TextDisabled("%s", g.Note);
		if (g.Status) g.Status();
		ImGui::PopTextWrapPos();
		for (size_t i = 0; i < settings.size(); i++)
			if (facts[i].group == &g) drawOne(i);
		ImGui::PopID();
	}

	if (!testHidden) return;
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
	if (shown) ImGui::TextDisabled("%d developer setting%s hidden (View > Developer options)", testHidden, testHidden == 1 ? "" : "s");
	else ImGui::TextDisabled("Only developer settings here: %d hidden (View > Developer options)", testHidden);
}

// UNOFFICIAL 2026-10-25: settings-only Shaders entries shown under the entry they belong with, so the list does not hold pages that
// read like effects of their own (a tester could not tell the flat clouds' shadows from the volumetric clouds' ones, and Foliage
// stood out as the one entry with no switch). Only the menu moves: their settings keep their own sections ([Shaders.<name>.Main]),
// so saved settings and the toml are unchanged; each page keeps its header (Cloud Shadows its on/off button).
struct MenuMove { const char* name; const char* under; const char* label; };
static const MenuMove kMenuMoves[] = {
	{ "CloudShadows", "ShadowsExteriors", "Cloud Shadows (flat clouds)" },
	{ "Foliage", "Plants", "Foliage glow" },
	{ "DustMotes", "VolumetricFog", "Dust motes" },	// 2026-11-04 (user: "make motes live under vol fog")
	{ "LocalExposure", "Exposure", "Local exposure" },	// 2026-11-04 (it works with the exposure)
};
// Test shots (gMenu.revealPage): whether this node leads to the page shown, so the shot's sidebar shows it.
static bool LeadsToSelected(const std::string& path) {
	if (gMenu.page.compare(0, path.size() + 1, path + ".") == 0) return true;
	for (const MenuMove& m : kMenuMoves)
		if (gMenu.page == std::string("Shaders.") + m.name + ".Main" && (path == "Shaders" || path == std::string("Shaders.") + m.under)) return true;
	return false;
}
// UNOFFICIAL 2026-10-27 (user: "why does sun far have its own ticker box in the list of shaders?" / "put sun far under shadows ext"):
// whole shader entries, their own on/off box included, shown inside another entry instead of at the top of the list. SunFar has no
// settings page (only [Shaders.SunFar.Status] Enabled): it is the pass that draws the mountain and flat-cloud shadows.
struct MenuNest { const char* name; const char* under; };
static const MenuNest kMenuNests[] = {
	{ "SunFar", "ShadowsExteriors" },
};
static bool IsMovedShaderEntry(const std::string& name) {
	for (const MenuMove& m : kMenuMoves) if (name == m.name) return true;
	for (const MenuNest& n : kMenuNests) if (name == n.name) return true;
	return false;
}

// Recursive sidebar tree — path is the full dotted section path, name is the display label.
// UNOFFICIAL 2026-10-27: the menu name of a Shaders entry where its internal name (its settings sections, kept for the users'
// settings files) says less than it does (user: "change the taa menu to say taa / fsr / dlss"). The ## part keeps ImGui's ID the name.
static std::string MenuEntryLabel(const std::string& name) {
	if (name == "TAA") return "TAA / FSR / DLSS##TAA";
	if (name == "SunFar") return "SunFar (mountain and flat-cloud shadow pass)##SunFar";
	return name;
}
// The same for a page in the sidebar (by its full path; the section keeps its name in the settings files).
static std::string MenuSectionLabel(const std::string& path, const std::string& name) {
	if (path == "Shaders.Terrain.Displacement") return "Tessellation (displacement)##" + name;	// UNOFFICIAL 2026-10-27 (user)
	if (path == "Shaders.POM.Displacement") return "Tessellation (displacement)##" + name;	// UNOFFICIAL 2026-10-28 rocks and cliffs
	return name;
}

// ---- The Ray tracing page ------------------------------------------------------------------------------------------------
// 2026-11-05 (docs/next/SETTINGS-GAME-MENU-PLAN.md phase 1; user: "the rt effects should live in their own menu all together for
// easier testing"): every ray-traced effect on one page: a status line each (running, or what stops it: testers' "RT does nothing"
// reports were nearly all another setting in the way), one switch for all of them (A/B), one picker for their debug views, then the
// five RT sections' own settings. Only the menu moves: the settings keep their sections in the toml, and those pages leave the sidebar.

static const char* const kRtTabId = "@RayTracing";

struct RtSection { const char* Section; const char* Title; const char* Note; };
static const RtSection kRtSections[] = {
	{ "Shaders.ShadowsExteriors.RT", "Sun shadows", "Traced sun shadows outdoors, combined with the shadow maps or replacing them near you (Mode)." },
	{ "Shaders.ShadowsInteriors.RT", "Lamp shadows", "Traced shadows from the lamps instead of cube maps. Enabled needs a restart; Live switches back and forth at once." },
	{ "Shaders.AmbientOcclusion.RT", "Ambient occlusion and bounce light",
		"Traced occlusion in place of the screen estimate, and with GI one bounce of light (more with RadianceCache)." },
	{ "Shaders.Reflections.RT", "Reflections", "Rays for what the screen-space reflections miss: things off screen or hidden. Needs Reflections on." },
	{ "Main.Main.RayTracing", "Scene and path tracing", "What goes into the traced scene (people, plants, see-through meshes, far distance), and the path tracer." },
};

static bool IsRtSection(const std::string& path) {
	for (const RtSection& s : kRtSections) if (path == s.Section) return true;
	return false;
}

static bool RtOn(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key) != 0; }

// One effect's line: running (green), off (dim), or on but something stops it or makes it weak (amber), with the reason.
enum class RtState { Off, Running, Blocked };
struct RtStatusLine { RtState State; std::string Text; std::string Detail; };

static RtStatusLine RtCoreLine() {
	const char* status = RtCore::StatusText();
	const std::string text = status && status[0] ? status : "not started (no ray-traced effect is on yet)";
	return { RtCore::Running() ? RtState::Running : RtState::Off, text, std::string() };
}

static RtStatusLine RtSunLine() {
	const char* s = "Shaders.ShadowsExteriors.RT";
	const std::string detail = RtSunShadows::StatusText();
	if (!RtOn(s, "Enabled")) return { RtState::Off, "off", detail };
	if (!RtCore::Running()) return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
	if (!TheShaderManager->GameState.isExterior) return { RtState::Off, "indoors: the sun is traced outdoors only", detail };
	return { RtState::Running, TheSettingManager->GetSettingI(s, "Mode") == 1 ? "running (replaces the shadow maps near you)" :
		"running (combined with the shadow maps)", detail };
}

static RtStatusLine RtLampLine() {
	const char* s = "Shaders.ShadowsInteriors.RT";
	const std::string detail = RtLampShadows::Status;
	const bool on = RtOn(s, "Enabled");
	if (on != RtLampShadows::CompiledIn)
		return { RtState::Blocked, on ? "switched on: needs a restart (the lamp shaders are built at start)" : "switched off: still traced until a restart", detail };
	if (!on) return { RtState::Off, "off (Enabled; needs a restart)", detail };
	if (!RtOn(s, "Live")) return { RtState::Off, "Live is off: the cube maps again (A/B)", detail };
	if (!RtCore::Running()) return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
	if (TheShaderManager->GameState.isExterior && !RtOn(s, "Exteriors")) return { RtState::Off, "outdoors: traced indoors only (Exteriors off; needs a restart)", detail };
	return { RtState::Running, "running", detail };
}

// Ambient light uses the RT occlusion and bounce light unless its own ComputeGI is on (then it keeps its screen search).
static bool RtComputeGIInWay() { return TheSettingManager->GetMenuShaderEnabled("AmbientLight") && RtOn("Shaders.AmbientLight.Main", "ComputeGI"); }

static RtStatusLine RtAOLine() {
	const char* s = "Shaders.AmbientOcclusion.RT";
	const std::string detail = AmbientOcclusionEffect::RtStatusText();
	if (!RtOn(s, "Enabled")) return { RtState::Off, "off", detail };
	if (!RtCore::Running()) return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
	if (!RtAmbient::PassesWanted()) return { RtState::Blocked, "on, but the Vulkan passes are off ([Main.Main.Performance] NvrDxvkPasses)", detail };
	const bool exterior = TheShaderManager->GameState.isExterior;
	const bool aoRuns = TheSettingManager->GetMenuShaderEnabled("AmbientOcclusion") &&
		RtOn(exterior ? "Shaders.AmbientOcclusion.Exteriors" : "Shaders.AmbientOcclusion.Interiors", "Enabled");
	const bool alUses = TheSettingManager->GetMenuShaderEnabled("AmbientLight") && !RtComputeGIInWay();
	if (!aoRuns && !alUses)
		return { RtState::Blocked, std::string("on, but nothing uses it here: Ambient occlusion is off ") + (exterior ? "outdoors (Exteriors" : "indoors (Interiors") +
			" Enabled) and Ambient light " + (RtComputeGIInWay() ? "has ComputeGI on" : "is off"), detail };
	if (RtComputeGIInWay()) return { RtState::Blocked, "running, but Ambient light ignores it while its ComputeGI is on (only Ambient occlusion uses it)", detail };
	return { RtState::Running, "running", detail };
}

static RtStatusLine RtGILine() {
	const char* s = "Shaders.AmbientOcclusion.RT";
	const std::string detail = AmbientOcclusionEffect::RtStatusText();
	if (!RtOn(s, "GI")) return { RtState::Off, "off (GI)", detail };
	if (!RtOn(s, "Enabled")) return { RtState::Blocked, "GI is on, but it needs the RT ambient occlusion on (Enabled)", detail };
	if (!RtCore::Running()) return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
	if (!TheSettingManager->GetMenuShaderEnabled("AmbientLight")) return { RtState::Blocked, "on, but Ambient light is off: the bounce light is added there", detail };
	if (RtComputeGIInWay()) return { RtState::Blocked, "on, but Ambient light's ComputeGI is on: it replaces the RT bounce light", detail };
	const ShaderConstants& sc = TheShaderManager->ShaderConst;
	const bool weakSun = TheShaderManager->GameState.isExterior && (sc.SunDir.z <= 0.0f || sc.SunAmount.x < 0.3f);
	if (weakSun && TheSettingManager->GetSettingF(s, "GIAmbient") <= 0.0f)
		return { RtState::Blocked, "running, but the sun is weak now (cloud, dusk or night) and GIAmbient is 0: only lamps give bounce light", detail };
	return { RtState::Running, RtOn(s, "RadianceCache") ? "running (more than one bounce: RadianceCache)" : "running (one bounce)", detail };
}

static RtStatusLine RtReflectionsLine() {
	const char* s = "Shaders.Reflections.RT";
	ReflectionsEffect* r = TheShaderManager->Effects.Reflections;
	const std::string detail = r ? r->RtStatusText() : std::string("no Reflections effect");
	if (!RtOn(s, "Enabled")) return { RtState::Off, "off", detail };
	if (!TheSettingManager->GetMenuShaderEnabled("Reflections"))
		return { RtState::Blocked, "on, but Reflections is off: the rays only fill in what its screen-space reflections miss", detail };
	if (!RtCore::Running()) return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
	const float wet = TheSettingManager->GetSettingF("Shaders.Reflections.Main", "WetRoughness");
	const float maxRough = TheSettingManager->GetSettingF(s, "MaxRoughness");
	char t[200];
	if (wet >= maxRough) {
		sprintf_s(t, "running, but wet ground (WetRoughness %.2f) is rougher than MaxRoughness %.2f: no rays on wet ground", wet, maxRough);
		return { RtState::Blocked, t, detail };
	}
	if (r && r->NothingReflects())
		return { RtState::Blocked, TheSettingManager->GetSettingF(s, "Weapon") > 0.0f ? "running for the weapon only: nothing else on screen reflects here (dry, GroundSheen 0)" :
			"on, but nothing on screen reflects here (dry, GroundSheen and InteriorFloors 0): nothing to trace", detail };
	return { RtState::Running, "running", detail };
}

static RtStatusLine RtPathTraceLine() {
	const char* s = "Main.Main.RayTracing";
	const std::string detail = RtPathTrace::StatusText();
	const bool realtime = RtOn(s, "PTRealtime");
	if (!realtime && !RtOn(s, "ReferencePT")) return { RtState::Off, "off", detail };
	if (!RtCore::Running()) return { RtState::Blocked, "on, but the ray tracing core is not running (first line)", detail };
	return { RtState::Running, realtime ? "running: the path-traced game (experimental)" : "running: the reference picture (a comparison, not for play)", detail };
}

static void DrawRtStatusLine(const char* name, const RtStatusLine& line) {
	static const ImVec4 kRunning(0.45f, 0.85f, 0.45f, 1.0f), kBlocked(1.0f, 0.75f, 0.2f, 1.0f);
	ImGui::TextUnformatted(name);
	ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
	ImGui::PushTextWrapPos(0.0f);
	if (line.State == RtState::Off) ImGui::TextDisabled("%s", line.Text.c_str());
	else ImGui::TextColored(line.State == RtState::Running ? kRunning : kBlocked, "%s", line.Text.c_str());
	ImGui::PopTextWrapPos();
	if (!line.Detail.empty() && ImGui::IsItemHovered()) WrappedTooltip(line.Detail.c_str());
}

// One switch for every RT effect, for an A/B: off remembers which were on, on brings them back (or, with nothing remembered, turns on
// the sun shadows, the ambient occlusion and bounce light, and the reflections). Lamp shadows go through Live (Enabled needs a restart).
struct RtMasterKey { const char* Section; const char* Key; bool Default; };
static const RtMasterKey kRtMasterKeys[] = {
	{ "Shaders.ShadowsExteriors.RT", "Enabled", true },
	{ "Shaders.ShadowsInteriors.RT", "Live", true },
	{ "Shaders.AmbientOcclusion.RT", "Enabled", true },
	{ "Shaders.Reflections.RT", "Enabled", true },
	{ "Main.Main.RayTracing", "PTRealtime", false },
	{ "Main.Main.RayTracing", "ReferencePT", false },
};
static bool s_rtRemembered = false;
static bool s_rtMemory[_countof(kRtMasterKeys)] = {};

static bool RtAnyOn() {
	for (const RtMasterKey& k : kRtMasterKeys) {
		if (!RtOn(k.Section, k.Key)) continue;
		if (!strcmp(k.Key, "Live") && !RtOn(k.Section, "Enabled")) continue;	// Live means nothing without the lamp shadows on
		return true;
	}
	return false;
}

static void DrawRtMasterSwitch() {
	bool on = RtAnyOn();
	MenuWidgets::RowBegin("All ray tracing", MenuWidgets::ResetSlotWidth());
	if (MenuWidgets::Toggle("##rtmaster", &on)) {
		if (!on) {
			for (size_t i = 0; i < _countof(kRtMasterKeys); i++) {
				s_rtMemory[i] = RtOn(kRtMasterKeys[i].Section, kRtMasterKeys[i].Key);
				TheSettingManager->SetSetting(kRtMasterKeys[i].Section, kRtMasterKeys[i].Key, false);
			}
			s_rtRemembered = true;
		}
		else
			for (size_t i = 0; i < _countof(kRtMasterKeys); i++)
				TheSettingManager->SetSetting(kRtMasterKeys[i].Section, kRtMasterKeys[i].Key, s_rtRemembered ? s_rtMemory[i] : kRtMasterKeys[i].Default);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Every ray-traced effect off, or back on as it was, to compare. With nothing to bring back it turns on the sun shadows, "
			"the ambient occlusion and the reflections. Lamp shadows switch through their Live setting (turning them on the first time needs "
			"a restart). The path tracer is only turned off, never on.");
}

// One picker for the RT debug views: picking one sets that DebugView and clears the other three, so only one shows.
struct RtDebugView { const char* Label; const char* Section; int Value; };
static const char* const kRtDebugSections[] = { "Shaders.ShadowsExteriors.RT", "Shaders.ShadowsInteriors.RT", "Shaders.AmbientOcclusion.RT", "Shaders.Reflections.RT" };
static const RtDebugView kRtDebugViews[] = {
	{ "Off (the normal picture)", nullptr, 0 },
	{ "Sun shadows: the traced shadow alone", "Shaders.ShadowsExteriors.RT", 1 },
	{ "Sun shadows: traced visibility (white lit, black blocked)", "Shaders.ShadowsExteriors.RT", 4 },
	{ "Lamp shadows: traced vs cube maps (red / green where they differ)", "Shaders.ShadowsInteriors.RT", 1 },
	{ "Lamp shadows: where the traced result is used", "Shaders.ShadowsInteriors.RT", 2 },
	{ "Ambient occlusion: the traced visibility", "Shaders.AmbientOcclusion.RT", 1 },
	{ "Bounce light alone (GI)", "Shaders.AmbientOcclusion.RT", 6 },
	{ "Reflections: where each reflection comes from", "Shaders.Reflections.RT", 1 },
	{ "Reflections: ray hit distance", "Shaders.Reflections.RT", 2 },
	{ "Reflections: ray tracing alone (screen trace off)", "Shaders.Reflections.RT", 3 },
};

static int RtCurrentDebugView() {	// the picker's entry, -1 when the DebugViews set match none (set by hand)
	int set = 0;
	for (const char* section : kRtDebugSections) if (TheSettingManager->GetSettingI(section, "DebugView") != 0) set++;
	if (set == 0) return 0;
	if (set > 1) return -1;
	for (int i = 1; i < (int)_countof(kRtDebugViews); i++)
		if (TheSettingManager->GetSettingI(kRtDebugViews[i].Section, "DebugView") == kRtDebugViews[i].Value) return i;
	return -1;
}

static void DrawRtDebugPicker() {
	const int current = RtCurrentDebugView();
	MenuWidgets::RowBegin("Debug view", MenuWidgets::ResetSlotWidth());
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo("##rtdebug", current >= 0 ? kRtDebugViews[current].Label : "Other (set in the sections below)")) {
		for (int i = 0; i < (int)_countof(kRtDebugViews); i++) {
			if (!ImGui::Selectable(kRtDebugViews[i].Label, i == current)) continue;
			for (const char* section : kRtDebugSections) TheSettingManager->SetSetting(section, "DebugView", 0);
			if (kRtDebugViews[i].Section) TheSettingManager->SetSetting(kRtDebugViews[i].Section, "DebugView", kRtDebugViews[i].Value);
			TheSettingManager->LoadSettings();
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered() || MenuWidgets::RowLabelHovered())
		WrappedTooltip("Shows one ray-traced effect's own picture, to see what it does. It stays on until you pick Off (the normal picture).");
}

static void RenderRayTracingPage() {
	PageTitle("Ray tracing", "Every ray-traced effect in one place. Needs the DXVK fork, NvrRtHelper.exe and a GPU with ray tracing.");

	if (MenuWidgets::SubHeader("Status###rtstatus", true)) {
		DrawRtStatusLine("Core", RtCoreLine());
		DrawRtStatusLine("Sun shadows", RtSunLine());
		DrawRtStatusLine("Lamp shadows", RtLampLine());
		DrawRtStatusLine("Occlusion", RtAOLine());
		DrawRtStatusLine("Bounce light", RtGILine());
		DrawRtStatusLine("Reflections", RtReflectionsLine());
		DrawRtStatusLine("Path tracing", RtPathTraceLine());
		ImGui::TextDisabled("Hover a line for the full state.");
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	}

	DrawRtMasterSwitch();
	DrawRtDebugPicker();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));

	// The five sections' own rows, each under its header, drawn as their pages draw them (the groups read the page shown)
	const std::string page = gMenu.page;
	for (const RtSection& section : kRtSections) {
		const std::string header = std::string(section.Title) + "###rtsection" + section.Section;
		if (!MenuWidgets::SubHeader(header.c_str(), false)) continue;
		SettingManager::Configuration::SettingList rows;
		TheSettingManager->FillMenuSettings(&rows, section.Section);
		ImGui::PushID(section.Section);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", section.Note);
		ImGui::PopTextWrapPos();
		gMenu.page = section.Section;
		PageOrder(rows);
		RenderSettingsList(rows);
		gMenu.page = page;
		ImGui::PopID();
	}
}

// 2026-11-05 (user: "cameramode, flycam can live under main -> misc"): small Main sections drawn inside another page under their own
// headers (as the Ray tracing page draws its sections), instead of as entries of their own in the sidebar. Only the menu moves.
struct PageAttach { const char* Host; const char* Section; const char* Title; };
static const PageAttach kPageAttaches[] = {
	{ "Main.Main.Misc", "Main.CameraMode.Main", "Camera mode" },
	{ "Main.Main.Misc", "Main.CameraMode.Positioning", "Camera mode position" },
	{ "Main.Main.Misc", "Main.FlyCam.Main", "Fly camera" },
	{ "Main.Main.Misc", "Main.Transitions.Main", "Day and night blending" },
};

static void RenderAttachedSections(const std::string& host) {
	const std::string page = gMenu.page;
	for (const PageAttach& a : kPageAttaches) {
		if (host != a.Host) continue;
		const std::string header = std::string(a.Title) + "###attached" + a.Section;
		if (!MenuWidgets::SubHeader(header.c_str(), false)) continue;
		SettingManager::Configuration::SettingList rows;
		TheSettingManager->FillMenuSettings(&rows, a.Section);
		ImGui::PushID(a.Section);
		gMenu.page = a.Section;
		PageOrder(rows);
		RenderSettingsList(rows);
		gMenu.page = page;
		ImGui::PopID();
	}
}

static bool HiddenFromSidebar(const std::string& path, const std::string& name);

// ---- Simple view: the whole effect on one page -------------------------------------------------------------------------------
// 2026-11-05 (user, of a simple page with four rows: "can we make it so we utilize all this dead space with simple mode on better so it
// doesn't look so barren?"). While Advanced is off, a page of an effect with a simple view shows every page of that effect at once, each
// as a card (its plain name, its simple rows, how many more Advanced has), two cards a row when the page is wide enough: one screen per
// effect, as a game's graphics menu has, instead of a few rows per page. The page picked in the sidebar comes first. Pages with no simple
// view and the ray tracing pages (on their own page) are left out of the cards.
static std::string SimplePageTitle(const std::string& name) {
	static const char* const kNames[][2] = { { "Main", "General" }, { "Exteriors", "Outdoors" }, { "Exterior", "Outdoors" },
		{ "Interiors", "Indoors" }, { "Night", "At night" }, { "Rain", "In the rain" }, { "NightRain", "At night in the rain" },
		{ "Default", "General" }, { "FirstPersonView", "First person" }, { "ThirdPersonView", "Third person" }, { "VanityView", "Idle camera" } };
	for (const auto& n : kNames) if (name == n[0]) return n[1];
	return name;
}

static void RenderSimpleCard(const std::string& section) {
	SettingManager::Configuration::SettingList rows;
	TheSettingManager->FillMenuSettings(&rows, section.c_str());
	ImGui::PushID(section.c_str());
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(0.25f));
	MenuWidgets::Title(SimplePageTitle(section.substr(section.rfind('.') + 1)).c_str(), 1.1f);
	ImGui::PopStyleColor();
	ImGui::Separator();
	const std::string page = gMenu.page;
	gMenu.page = section;
	PageOrder(rows);
	RenderSettingsList(rows);	// (Advanced off: RenderSimpleList)
	gMenu.page = page;
	ImGui::PopID();
}

// Whether this page is drawn as the whole effect's cards; draws them.
static bool RenderSimpleEffectPage() {
	if (AdvancedShown() || !HasSimpleView(gMenu.page) || IsRtSection(gMenu.page)) return false;
	const std::string effect = ShaderOfPage(gMenu.page);
	if (effect.empty()) return false;
	std::vector<std::string> cards = { gMenu.page };
	StringList children;
	TheSettingManager->FillMenuSections(&children, ("Shaders." + effect).c_str());
	for (const std::string& child : children) {
		const std::string section = "Shaders." + effect + "." + child;
		if (section != gMenu.page && HasSimpleView(section) && !IsRtSection(section) && !HiddenFromSidebar(section, child)) cards.push_back(section);
	}
	RenderPageHeader(gMenu.page);
	RenderSectionHelp(gMenu.page);
	const int columns = cards.size() > 1 && ImGui::GetContentRegionAvail().x > ImGui::GetFontSize() * 55.0f ? 2 : 1;
	if (!ImGui::BeginTable("##simplecards", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX)) return true;
	for (const std::string& section : cards) {
		ImGui::TableNextColumn();
		RenderSimpleCard(section);
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 2.0f));
	}
	ImGui::EndTable();
	return true;
}

// ---- What is hiding effects right now ---------------------------------------------------------------------------------------
// 2026-11-05 (user, after an hour of "the rays show in the debug view but I see no reflection": the master switch, a debug view left
// on, specular occlusion and physically faint floors each hid it in turn): "add a note for each setting that could override and make
// other effects virtually invisible if they're on ... with the dead space at the bottom". Every page ends with the notes that apply
// NOW, each with a button that undoes it. kHidingNotes holds the hand-written ones; the Heads up rules (kConflictRules) whose setting is
// at its value and whose other side is on are listed too. tools/check_menu_notes.py checks that every debug view in the defaults is
// in kDebugViews.
struct DebugViewKey { const char* Section; const char* Key; const char* Name; };
static const DebugViewKey kDebugViews[] = {
	{ "Main.Main.RayTracing", "PTDebugView", "Path tracing" },
	{ "Main.Main.ReducedQuality", "SunLayersDebug", "Sun layers" },
	{ "Shaders.AmbientOcclusion.Skylighting", "DebugView", "Skylighting" },
	{ "Shaders.AmbientOcclusion.RT", "DebugView", "Ray-traced ambient occlusion" },
	{ "Shaders.AmbientLight.Main", "DebugView", "Ambient light" },
	{ "Shaders.Flashlight.Deferred", "DebugView", "Flashlight" },
	{ "Shaders.Grass.GPUGrass", "DebugView", "GPU grass" },
	{ "Shaders.Grass.Interaction", "Debug", "Grass interaction" },
	{ "Shaders.DitherBuster.Main", "DebugView", "Dither buster" },
	{ "Shaders.TAA.Main", "DebugView", "TAA" },
	{ "Shaders.TAA.FSR", "ReactiveDebug", "FSR reactive mask" },
	{ "Shaders.TAA.FSR", "DebugView", "FSR" },
	{ "Shaders.PBR.Metal", "DebugView", "Metal" },
	{ "Shaders.PBR.MaterialRules", "DebugView", "Material rules" },
	{ "Shaders.PBR.Rmaos", "DebugView", "Material maps" },
	{ "Shaders.POM.Displacement", "DebugView", "Object displacement" },
	{ "Shaders.ShadowsExteriors.Main", "FitToDepthDebug", "Sun shadow fit" },
	{ "Shaders.ShadowsExteriors.RT", "DebugView", "Ray-traced sun shadows" },
	{ "Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapDebug", "Character shadow map" },
	{ "Shaders.ShadowsExteriors.TerrainShadows", "DebugView", "Terrain shadows" },
	{ "Shaders.CloudShadows.Main", "DebugView", "Cloud shadows" },
	{ "Shaders.ShadowsInteriors.Forward", "DebugView", "Lamp shadows (forward)" },
	{ "Shaders.ShadowsInteriors.Deferred", "DebugView", "Lamp shadows (deferred)" },
	{ "Shaders.ShadowsInteriors.RT", "DebugView", "Ray-traced lamp shadows" },
	{ "Shaders.ShadowsInteriors.RT", "ExteriorDebug", "Ray-traced lamp shadows outdoors" },
	{ "Shaders.ShadowsInteriors.RT", "DebugLamp", "Ray-traced lamp shadows (one lamp)" },
	{ "Shaders.DustMotes.Main", "DebugView", "Dust motes" },
	{ "Shaders.LocalExposure.Main", "DebugView", "Local exposure" },
	{ "Shaders.Reflections.Main", "DebugView", "Reflections" },
	{ "Shaders.Reflections.RT", "DebugView", "Ray-traced reflections" },
	{ "Shaders.VolumetricClouds.Main", "ShadowDebug", "Cloud shadow" },
	{ "Shaders.VolumetricClouds.Main", "ReflectionDebug", "Cloud reflections" },
	{ "Shaders.HandBlood.Main", "DebugView", "Hand blood" },
	{ "Shaders.BloodFlow.Main", "DebugView", "Blood flow" },
	{ "Shaders.BloodFlow.Fluid", "DebugView", "Blood fluid" },
	{ "Shaders.Skin.Debug", "DebugView", "Skin" },
	{ "Shaders.SnowAccumulation.Main", "CapturedNormalsDebugView", "Snow normals" },
	{ "Shaders.Terrain.Blending", "DebugView", "Terrain blending" },
	{ "Shaders.Terrain.Blending", "Debug", "Terrain blending (debug)" },
	{ "Shaders.Terrain.Displacement", "DebugView", "Terrain displacement" },
	{ "Shaders.WetWorld.Main", "PersistentDebugView", "Wet world" },
	{ "Shaders.WetnessEffects.HudRaindrops", "DebugView", "Raindrops on screen" },
	{ "Shaders.WetnessEffects.Reflections", "DebugView", "Wet reflections" },
};

static float SettingF(const char* section, const char* key) { return TheSettingManager->GetSettingF(section, key); }
static bool ReflectionsShown() { return TheSettingManager->SettingsMain.Main.RenderEffects && EffectOn("Reflections"); }
static bool RtReflectionsOn() { return ReflectionsShown() && SettingOn("Shaders.Reflections.RT", "Enabled"); }
// The user asked for clear reflections: a strong or sharp setting.
static bool WantsClearReflections() {
	return SettingF("Shaders.Reflections.Main", "Roughness") <= 0.15f || SettingF("Shaders.Reflections.Main", "InteriorFloors") >= 1.0f ||
		SettingF("Shaders.Reflections.Main", "GroundSheen") >= 1.0f || SettingF("Shaders.Reflections.Main", "Mirror") > 0.0f;
}

struct HidingNote {
	bool (*Active)();
	const char* Text;
	const char* Fix;		// the button
	const char* Sets;		// what the button writes: "Section/Key=Value Section/Key=Value"; "!effect:Name" switches that effect on
};
static const HidingNote kHidingNotes[] = {
	{ []() { return !TheSettingManager->SettingsMain.Main.RenderEffects; },
		"ALL effects are switched off (the master switch: the modifier key + the menu key, Alt+O by default). Changing settings shows "
		"nothing until they are on again.", "Turn effects on", "Main.Main.Misc/RenderEffects=true" },
	{ []() { return ReflectionsShown() && SettingF("Shaders.Reflections.Main", "SpecularOcclusion") > 0.0f &&
			SettingF("Shaders.Reflections.Main", "Mirror") < 1.0f && WantsClearReflections(); },
		"Specular occlusion dims reflections on floors darker than what they reflect: on dark polished floors people and objects "
		"vanish from the reflection and only lamps remain.", "Turn specular occlusion off", "Shaders.Reflections.Main/SpecularOcclusion=0.0" },
	{ []() { return ReflectionsShown() && SettingF("Shaders.Reflections.Main", "Mirror") <= 0.0f && WantsClearReflections(); },
		"Floors reflect like real stone and tile: about 4 % looking down (more toward the horizon), so mostly bright lamps show in "
		"them. For a clear mirror that shows people at any angle, raise Mirror floors.", "Make floors mirrors",
		"Shaders.Reflections.Main/Mirror=1.0 Shaders.Reflections.Main/Roughness=0.02" },
	{ []() { return ReflectionsShown() && !TheShaderManager->GameState.isExterior && SettingF("Shaders.Reflections.Main", "InteriorFloors") <= 0.0f &&
			SettingF("Shaders.Reflections.Main", "MaterialReflect") <= 0.0f; },
		"Indoors nothing reflects: Floor reflections indoors and Smooth surface reflections are both 0.", "Floors reflect indoors",
		"Shaders.Reflections.Main/InteriorFloors=1.0" },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && SettingOn("Shaders.Reflections.RT", "Enabled") && !EffectOn("Reflections"); },
		"Ray-traced reflections are on but the Reflections effect is off: they do nothing until it is on.", "Turn Reflections on", "!effect:Reflections" },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && AnyRtOn() && !SettingOn("Main.Main.RayTracing", "Actors"); },
		"People are left out of the ray-traced scene: no people in ray-traced reflections, and no ray-traced shadows from them.",
		"Put people in", "Main.Main.RayTracing/Actors=true" },
	{ []() { return RtReflectionsOn() && !SettingOn("Main.Main.RayTracing", "HitTextures"); },
		"Ray-traced reflections show each object as one flat colour (real textures are off).", "Use real textures",
		"Main.Main.RayTracing/HitTextures=true" },
	{ []() { return RtReflectionsOn() && !SettingOn("Main.Main.RayTracing", "AlphaTested"); },
		"Fences, leaves, grates and other see-through meshes are missing from ray-traced reflections and shadows.", "Put them in",
		"Main.Main.RayTracing/AlphaTested=true" },
	{ []() { return TheSettingManager->SettingsMain.Main.RenderEffects && AnyRtOn() && !RtCore::Running(); },
		"A ray-traced effect is on but ray tracing is not running (see the Ray tracing page's first line for why): it shows nothing.",
		nullptr, nullptr },
};

static void ApplySets(const char* sets) {
	if (!sets) return;
	const std::string all(sets);
	for (const bool on : { true, false }) {	// "!effect:Name" switches an effect on, "!effectoff:Name" off (its own switch, as the sidebar)
		const std::string tag = on ? "!effect:" : "!effectoff:";
		if (all.rfind(tag, 0) != 0) continue;
		const std::string name = all.substr(tag.size());
		if (TheSettingManager->GetMenuShaderEnabled(name.c_str()) != on) TheShaderManager->SwitchShaderStatus(name.c_str());
		return;
	}
	std::stringstream parts(all);
	std::string kv;
	while (parts >> kv) {
		const size_t slash = kv.find('/'), eq = kv.find('=');
		if (slash == std::string::npos || eq == std::string::npos || eq < slash) continue;
		TheSettingManager->SetSettingS(kv.substr(0, slash).c_str(), kv.substr(slash + 1, eq - slash - 1).c_str(), kv.substr(eq + 1).c_str());
	}
	TheSettingManager->LoadSettings();
}

static void RenderHidingNotes() {
	struct Line { std::string Text, Fix, Sets; };
	std::vector<Line> lines;
	for (const HidingNote& n : kHidingNotes)
		if (n.Active()) lines.push_back({ n.Text, n.Fix ? n.Fix : "", n.Sets ? n.Sets : "" });
	std::string views;
	std::string reset;
	for (const DebugViewKey& d : kDebugViews)
		if (TheSettingManager->GetSettingF(d.Section, d.Key) != 0.0f) {
			views += (views.empty() ? "" : ", ") + std::string(d.Name);
			reset += std::string(d.Section) + "/" + d.Key + "=0 ";
		}
	if (!views.empty())
		lines.push_back({ "A debug view is on (" + views + "): it replaces or tints the normal picture.", "Turn debug views off", reset });
	for (const ConflictRule& r : kConflictRules) {
		if (!r.Applies || !r.Applies()) continue;	// (the always-on ones are warnings for the moment of switching, not hiders)
		const bool boolean = !strcmp(r.Value, "true");
		const bool at = boolean ? SettingOn(r.Section, r.Key) : TheSettingManager->GetSettingI(r.Section, r.Key) == atoi(r.Value);
		if (!at) continue;
		const std::string section(r.Section);
		const bool status = boolean && !strcmp(r.Key, "Enabled") && section.size() > 15 && section.rfind("Shaders.", 0) == 0 &&
			section.compare(section.size() - 7, 7, ".Status") == 0;
		lines.push_back({ r.Text, boolean ? "Turn it off" : "",
			status ? "!effectoff:" + section.substr(8, section.size() - 15) : boolean ? section + "/" + r.Key + "=false" : std::string() });
	}
	if (lines.empty()) return;

	// In the page's empty space: pushed to the bottom when there is room, else after the rows.
	const float lineH = ImGui::GetTextLineHeightWithSpacing();
	float need = ImGui::GetFrameHeightWithSpacing() * 2.0f;
	for (const Line& l : lines) need += lineH * (1.0f + (float)l.Text.size() / 110.0f) + (l.Fix.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing());
	const float room = ImGui::GetContentRegionAvail().y;
	if (room > need) ImGui::Dummy(ImVec2(0.0f, room - need));
	ImGui::Separator();
	ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", lines.size() == 1 ? "1 thing may be hiding effects right now" :
		(std::to_string(lines.size()) + " things may be hiding effects right now").c_str());
	for (size_t i = 0; i < lines.size(); i++) {
		ImGui::PushID((int)i + 7100);
		ImGui::Bullet();
		ImGui::SameLine();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(lines[i].Text.c_str());
		ImGui::PopTextWrapPos();
		if (!lines[i].Fix.empty() && ImGui::SmallButton(lines[i].Fix.c_str())) ApplySets(lines[i].Sets.c_str());
		ImGui::PopID();
	}
}

// ---- The page -----------------------------------------------------------------------------------------------------------
// The search results, the New-in-patch page, or the selected section: title, help, the LUT pickers (LUT page) and the rows.
static void RenderContent() {
	if (SearchActive()) return RenderSearchResults();
	if (gMenu.page == kNewTabId) return RenderNewInPatch();
	if (gMenu.page == kRtTabId) return RenderRayTracingPage();
	if (gMenu.page.empty()) {
		ImGui::TextDisabled("Select a section from the left panel.");
		return;
	}
	SettingManager::Configuration::SettingList rows;
	TheSettingManager->FillMenuSettings(&rows, gMenu.page.c_str());
	if (rows.empty()) {
		ImGui::TextDisabled("No settings in this section.");
		return;
	}
	if (RenderSimpleEffectPage()) return;	// 2026-11-05: Advanced off: the effect's pages as cards on one page
	PageOrder(rows);
	RenderPageHeader(gMenu.page);
	RenderSectionHelp(gMenu.page);	// UNOFFICIAL 2026-11-01: what the page is for, in plain words (kSectionHelp)
	if (gMenu.page == "Shaders.LUT.Main") RenderLutPage();
	RenderSettingsList(rows);
	RenderAttachedSections(gMenu.page);	// 2026-11-05
}

// ---- The sidebar ---------------------------------------------------------------------------------------------------------
// Top-level sections (Main, Shaders, ...) are small group labels; under them each entry is a full-width row that folds open.
// A shader's row carries its on/off switch. Pages (sections with no sub-sections) are plain rows that open on the right.

static void RenderSectionNode(const std::string& path, const std::string& name, bool parentIsShaders);

// A page row: muted unless it is the page shown.
static void DrawLeafSection(const std::string& path, const std::string& name) {
	const bool shown = gMenu.page == path;
	ImGui::PushStyleColor(ImGuiCol_Text, shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
	if (ImGui::Selectable(MenuSectionLabel(path, name).c_str(), shown)) gMenu.page = path;
	if (shown && gMenu.revealPage) ImGui::SetScrollHereY(0.5f);	// test shots: the page's row in view
	ImGui::PopStyleColor();
}

// The folding part of a row: spans the sidebar's width (a click anywhere on it folds it) and lets the switch on top of it take
// its own clicks. Returns whether the row is open.
static bool FoldingRow(const std::string& path) {
	if (gMenu.revealPage && LeadsToSelected(path)) ImGui::SetNextItemOpen(true);
	const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
	ImGui::PushStyleColor(ImGuiCol_Text, MenuWidgets::MutedText(0.5f));	// the arrow
	const bool open = ImGui::TreeNodeEx("##node", flags);
	ImGui::PopStyleColor();
	ImGui::SameLine();
	return open;
}

// A shader's row: the fold arrow, its switch (or, with no switch of its own, just its name), "(forced)" when a preset locks it,
// and in debug mode the time it took last frame.
static bool DrawShaderEntryRow(const std::string& path, const std::string& name) {
	const char* shader = name.c_str();
	bool on = TheSettingManager->GetMenuShaderEnabled(shader);
	const bool forced = TheSettingManager->IsShaderForced(shader);
	const SettingManager::MenuSwitch kind = TheSettingManager->GetMenuShaderSwitch(shader);
	const std::string label = ShownText(MenuEntryLabel(name));

	const bool open = FoldingRow(path);
	if (kind == SettingManager::MenuSwitch::None) {
		// UNOFFICIAL 2026-10-19: no on/off setting (Foliage): a name only, not a switch that springs back
		ImGui::TextUnformatted(label.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s has no on/off switch of its own: the settings inside control it.", shader);
	}
	else {
		ImGui::BeginDisabled(forced);
		if (MenuWidgets::Toggle("##on", &on) && (!on || ConfirmEffectOn(name))) {	// (2026-11-05: conflicts ask first)
			TheShaderManager->SwitchShaderStatus(shader);
			TheSettingManager->LoadSettings();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.5f));
		ImGui::TextUnformatted(label.c_str());
		ImGui::PopStyleColor();
		if (kind == SettingManager::MenuSwitch::MainEnabled)	// Cloth and the like: read at start, the same note as their Main page
			RestartBadge(("Shaders." + name + ".Main").c_str(), "Enabled");
	}
	if (forced) {
		ImGui::SameLine();
		ImGui::TextDisabled("(forced)");
	}
	if (TheSettingManager->SettingsMain.Develop.DebugMode) {
		if (const EffectRecord* effect = TheShaderManager->GetEffectByName(shader)) {
			const float ms = TheSettingManager->SettingsMain.Main.RenderEffects ? ImMax(effect->renderTime + effect->constantUpdateTime, 0.0f) : 0.0f;
			ImGui::SameLine();
			ImGui::TextDisabled("%.3f ms", ms);
		}
	}
	return open;
}

// A section with sub-sections that is not a shader: the fold arrow and its name.
static bool DrawPlainSectionRow(const std::string& path, const std::string& name) {
	const bool open = FoldingRow(path);
	ImGui::TextUnformatted(name.c_str());
	return open;
}

// The pages shown under a shader entry that is not theirs (kMenuMoves): rows that open the page's Main section.
static void DrawMovedPages(const std::string& path) {
	for (const MenuMove& move : kMenuMoves) {
		if (path != std::string("Shaders.") + move.under) continue;
		const std::string page = std::string("Shaders.") + move.name + ".Main";
		ImGui::PushID(page.c_str());
		DrawLeafSection(page, std::string(move.label) + "##" + move.name);
		ImGui::PopID();
	}
}

// What sits under an open entry: its sub-sections (less the shader entries shown elsewhere), the moved pages, and the shader
// entries nested under it (kMenuNests) with their own switches.
static void DrawEffectGroups(const StringList& children);

static void DrawChildren(const std::string& path) {
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	const bool shaders = path == "Shaders";
	if (shaders) return DrawEffectGroups(children);	// 2026-11-05: the effects in groups of their kind
	for (const std::string& child : children)
		if (!(shaders && IsMovedShaderEntry(child)))	// UNOFFICIAL 2026-10-25: drawn under the entry it belongs with
			RenderSectionNode(path + "." + child, child, shaders);
	DrawMovedPages(path);
	for (const MenuNest& nest : kMenuNests)	// UNOFFICIAL 2026-10-27
		if (path == std::string("Shaders.") + nest.under) RenderSectionNode(std::string("Shaders.") + nest.name, nest.name, true);
}

static bool HiddenFromSidebar(const std::string& path, const std::string& name) {
	if (ShouldHideSection(name)) return true;
	if (IsRtSection(path)) return true;	// 2026-11-05: on the Ray tracing page instead
	// 2026-11-05 (user: "remove blood lens, sleeping mode, move gun fx ... under a relevant category"): their settings stay in the
	// file and keep working; GunFX's page is in the effects list's "Particles and gun effects" group
	if (path == "Shaders.BloodLens" || path == "Main.SleepingMode" || path == "Main.GunFX") return true;
	// (user: camera mode and fly camera under Main > Misc; frame rate options under Performance "if they do anything at all": they do
	// not: FrameRateManager::PerformSync is never called and the Stuttering pair is read by nothing)
	if (path == "Main.CameraMode" || path == "Main.FlyCam" || path == "Main.Transitions" || path == "Main.FrameRate") return true;
	if (path == "Main.Main.Precipitations") return true;	// (user: "remove the precipitations menu from main"; the setting stays)
	return !DevOptionsShown() && IsDevOnlySetting(path, std::string());	// UNOFFICIAL 2026-10-25: a "Section.*" DevOnly entry
}

static void RenderSectionNode(const std::string& path, const std::string& name, bool parentIsShaders) {
	if (HiddenFromSidebar(path, name)) return;
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	ImGui::PushID(path.c_str());	// every ID inside belongs to this path
	if (children.empty()) {
		DrawLeafSection(path, name);
	}
	else if (parentIsShaders ? DrawShaderEntryRow(path, name) : DrawPlainSectionRow(path, name)) {
		DrawChildren(path);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

// 2026-11-05 (user: "organize all of the effects into groups of similar effects with collapsible menus", and the Ray tracing page
// "in the list with the rest of the effects"; docs/next/SETTINGS-GAME-MENU-PLAN.md): the Shaders list as folding groups of effects of a
// kind, as a game's graphics menu groups them. Only the sidebar changes: each effect keeps its row, switch and pages. An effect no
// group names goes under "Other", so a new one never goes missing. Ray tracing is a row of its own here (the page above).
struct EffectGroup { const char* Name; const char* Entries; const char* Page = nullptr; const char* PageLabel = nullptr; };	// Page: a settings page that is no effect entry
static const EffectGroup kEffectGroups[] = {
	{ "Lighting", "AmbientLight, AmbientOcclusion, BounceLight, Specular, PBR, Reflections, Flashlight" },
	{ "Shadows", "ShadowsExteriors, ShadowsInteriors, ContactHardening" },
	{ "Ray tracing", nullptr },
	{ "Sky and atmosphere", "Sky, VolumetricClouds, VolumetricFog, VolumetricLight, GodRays, Sandstorm, HeatHaze" },
	{ "Weather and water", "Precipitations, Snow, SnowAccumulation, WetnessEffects, HudRaindrops, WetWorld, Water, Underwater, WaterLens" },
	{ "Ground and plants", "Terrain, POM, Grass, Plants" },
	{ "Characters", "Skin, Cloth, PipboyFit, BloodFlow, HandBlood, Blood" },
	{ "Camera and colour", "Tonemapping, Exposure, HDR, Bloom, BloomLegacy, Lens, LUT, Coloring, ImageAdjust, Cinema, Bodycam, DepthOfField, MotionBlur, LowHF" },
	{ "Anti-aliasing and sharpening", "TAA, SMAA, Sharpening, DitherBuster, SkyDither" },
	{ "Particles and gun effects", "Particles, GPUParticles", "Main.GunFX.Main", "Gun effects (smoke, barrel glow)##GunFX" },
	{ "Developer", "Debug" },
};

static bool InAnyEffectGroup(const std::string& name) {
	for (const EffectGroup& g : kEffectGroups) if (g.Entries && KeyInList(g.Entries, name.c_str())) return true;
	return false;
}

// One group's folding row: its name and how many of its effects are on; open while a test shot shows one of its pages.
static bool DrawEffectGroupRow(const char* name, const std::vector<std::string>& entries) {
	int on = 0, switches = 0;
	bool reveal = false;
	for (const std::string& e : entries) {
		if (TheSettingManager->GetMenuShaderSwitch(e.c_str()) != SettingManager::MenuSwitch::None) {
			switches++;
			if (TheSettingManager->GetMenuShaderEnabled(e.c_str())) on++;
		}
		reveal = reveal || (gMenu.revealPage && LeadsToSelected("Shaders." + e));
	}
	if (reveal) ImGui::SetNextItemOpen(true);
	const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
	const bool open = ImGui::TreeNodeEx((std::string(name) + "###effectgroup" + name).c_str(), flags);
	if (switches) {
		ImGui::SameLine();
		ImGui::TextDisabled("%d of %d on", on, switches);
	}
	return open;
}

// The Ray tracing group (2026-11-05, user: "its own little drop down like the others"): the overview page (status, one switch, debug
// views) and the five RT sections' own pages, which have no effect entry of their own in the list. "n of 5 on" as the other groups.
static void DrawRayTracingGroup() {
	static const char* const kOnKeys[][2] = { { "Shaders.ShadowsExteriors.RT", "Enabled" }, { "Shaders.ShadowsInteriors.RT", "Enabled" },
		{ "Shaders.AmbientOcclusion.RT", "Enabled" }, { "Shaders.Reflections.RT", "Enabled" }, { "Main.Main.RayTracing", "PTRealtime" } };
	int on = 0;
	for (const auto& k : kOnKeys) if (RtOn(k[0], k[1]) || (!strcmp(k[1], "PTRealtime") && RtOn(k[0], "ReferencePT"))) on++;
	if (gMenu.revealPage && (gMenu.page == kRtTabId || IsRtSection(gMenu.page))) ImGui::SetNextItemOpen(true);
	const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
	const bool open = ImGui::TreeNodeEx("Ray tracing###effectgroupRT", flags);
	ImGui::SameLine();
	ImGui::TextDisabled("%d of %d on", on, (int)_countof(kOnKeys));
	if (!open) return;
	const bool shown = gMenu.page == kRtTabId;
	ImGui::PushID(kRtTabId);
	ImGui::PushStyleColor(ImGuiCol_Text, shown ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : MenuWidgets::MutedText(0.35f));
	if (ImGui::Selectable("Overview (status, one switch, debug views)", shown)) gMenu.page = kRtTabId;
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every ray-traced effect in one place: what runs, what stops it, one switch for all and the debug views.");
	ImGui::PopID();
	for (const RtSection& section : kRtSections) {
		ImGui::PushID(section.Section);
		DrawLeafSection(section.Section, std::string(section.Title) + "##" + section.Section);
		ImGui::PopID();
	}
	ImGui::TreePop();
}

static void DrawEffectGroups(const StringList& children) {
	auto shown = [](const std::string& child) { return !IsMovedShaderEntry(child) && !HiddenFromSidebar("Shaders." + child, child); };
	std::vector<std::string> other;
	for (const std::string& child : children)
		if (shown(child) && !InAnyEffectGroup(child)) other.push_back(child);
	auto drawGroup = [&](const char* name, const std::vector<std::string>& entries, const EffectGroup* group) {
		if (entries.empty() && !(group && group->Page)) return;
		if (DrawEffectGroupRow(name, entries)) {
			for (const std::string& e : entries) RenderSectionNode("Shaders." + e, e, true);
			if (group && group->Page) {
				ImGui::PushID(group->Page);
				DrawLeafSection(group->Page, group->PageLabel);
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
	};
	for (const EffectGroup& g : kEffectGroups) {
		if (!g.Entries) {
			DrawRayTracingGroup();
			continue;
		}
		std::vector<std::string> entries;	// in the group's own order
		for (const char* p = g.Entries; *p; ) {
			while (*p == ' ' || *p == ',') p++;
			const char* end = p;
			while (*end && *end != ',') end++;
			const std::string name(p, end);
			if (!name.empty() && std::find(children.begin(), children.end(), name) != children.end() && shown(name)) entries.push_back(name);
			p = end;
		}
		drawGroup(g.Name, entries, &g);
	}
	drawGroup("Other", other, nullptr);
}

// "New in this patch (N)" at the top of the sidebar, in the heading colour; the page shown while nothing else is picked.
static void DrawNewInPatchEntry() {
	NewPatchEnsure();
	if (!NewPatchCount()) return;
	if (gMenu.page.empty()) gMenu.page = kNewTabId;
	const bool shown = gMenu.page == kNewTabId;
	const std::string label = "New in this patch (" + std::to_string(NewPatchCount()) + ")";
	ImGui::PushID(kNewTabId);
	ImGui::PushStyleColor(ImGuiCol_Text, MenuSkin::Heading(shown ? 0.25f : 0.0f));
	if (ImGui::Selectable(label.c_str(), shown)) gMenu.page = kNewTabId;
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Every setting that is new in this patch, in one place, to test. They are the same settings as in their own menus.");
	ImGui::PopID();
}

static void RenderSidebar() {
	RenderSearchBox();	// UNOFFICIAL 2026-10-25: menu search
	if (SearchActive()) return RenderSearchSidebar();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	DrawNewInPatchEntry();

	// The skin's IndentSpacing is 0 (the pages indent their own groups): the sidebar's folds get a small step per level
	ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetFontSize());
	StringList roots;
	TheSettingManager->FillMenuSections(&roots, nullptr);
	for (const std::string& root : roots) {
		if (HiddenFromSidebar(root, root)) continue;
		StringList children;
		TheSettingManager->FillMenuSections(&children, root.c_str());
		ImGui::PushID(root.c_str());
		if (children.empty()) DrawLeafSection(root, root);
		else if (MenuWidgets::GroupLabel(root.c_str(), gMenu.revealPage && LeadsToSelected(root))) DrawChildren(root);
		ImGui::PopID();
	}
	ImGui::PopStyleVar();
}

// ---- Drawn without the menu ----------------------------------------------------------------------------------------------

// At the game's main menu: the version and the menu key along the bottom of the screen, for the first five seconds.
static void RenderMainMenuToast() {
	static const ULONGLONG shownSince = GetTickCount64();
	// 2026-11-02: files taken over from (or left in) the old New Vegas Reloaded places are listed above the hint, longer
	const std::vector<std::string>& notices = Paths::OldFilesNotices();
	if (GetTickCount64() - shownSince > (notices.empty() ? 5000u : 20000u)) return;
	char text[256];
	sprintf_s(text, "%s %s loaded  |  press %s to open settings", Branding::ProductName, Branding::Version,
		DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));
	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImDrawList* front = ImGui::GetForegroundDrawList();
	float y = screen.y - 12.0f;
	auto line = [&](const char* s, ImU32 colour) {
		const ImVec2 size = ImGui::CalcTextSize(s);
		y -= size.y + 2.0f;
		const ImVec2 at((screen.x - size.x) * 0.5f, y);
		front->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), s);
		front->AddText(at, colour, s);
	};
	line(text, IM_COL32(220, 220, 220, 255));
	for (auto it = notices.rbegin(); it != notices.rend(); ++it) line(it->c_str(), IM_COL32(255, 200, 90, 255));
}

// 2026-11-03: an old NewVegasReloaded.dll left next to VegasShaders.dll by an upgrade (Paths::TwinDll, found in Main.cpp). Both load
// and both replace the game's shaders, which turns objects and ground white or wrong (three testers on 1.0.1; reproduced by putting
// P85 v4's DLL beside 1.0.1). Nothing in the picture says why, so a red panel says it: large in the middle of the main menu, a banner
// across the top in game, every frame, until the file is gone. Drawn on the foreground list (no window, no input).
static void RenderTwinDllWarning(bool mainMenu) {
	if (Paths::TwinDll.empty()) return;
	const char* name = strrchr(Paths::TwinDll.c_str(), '\\') ? strrchr(Paths::TwinDll.c_str(), '\\') + 1 : Paths::TwinDll.c_str();
	char title[160], body[640];
	sprintf_s(title, "Old New Vegas Reloaded is still installed: %s", name);
	sprintf_s(body,
		"Data\\NVSE\\Plugins\\%s loads next to %s. Both replace the game's shaders, so objects and ground can turn white or look wrong.\n"
		"Fix: quit the game, delete or move %s out of Data\\NVSE\\Plugins, and start again. "
		"With Mod Organizer, untick the old New Vegas Reloaded mod.", name, Paths::DllName, name);

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImFont* font = ImGui::GetFont();
	const float base = ImGui::GetFontSize();
	const float titleSize = base * (mainMenu ? 1.8f : 1.35f), bodySize = base * (mainMenu ? 1.25f : 1.0f);
	const float pad = base * (mainMenu ? 1.2f : 0.7f);
	const float width = ImMin(screen.x - 2.0f * base, mainMenu ? base * 52.0f : screen.x - 2.0f * base);
	const float wrap = width - 2.0f * pad;
	const ImVec2 titleDim = font->CalcTextSizeA(titleSize, FLT_MAX, wrap, title);
	const ImVec2 bodyDim = font->CalcTextSizeA(bodySize, FLT_MAX, wrap, body);
	const float height = pad * 2.0f + titleDim.y + base * 0.5f + bodyDim.y;
	const ImVec2 at((screen.x - width) * 0.5f, mainMenu ? (screen.y - height) * 0.4f : base);

	ImDrawList* front = ImGui::GetForegroundDrawList();
	const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.0f);	// the border breathes, so it is seen
	front->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(70, 8, 8, 235), base * 0.4f);
	front->AddRect(at, ImVec2(at.x + width, at.y + height), IM_COL32(255, 60 + (int)(80 * pulse), 60, 255), base * 0.4f, 0, 3.0f);
	ImVec2 text(at.x + pad, at.y + pad);
	front->AddText(font, titleSize, ImVec2(text.x + 2, text.y + 2), IM_COL32(0, 0, 0, 220), title, nullptr, wrap);
	front->AddText(font, titleSize, text, IM_COL32(255, 225, 90, 255), title, nullptr, wrap);
	text.y += titleDim.y + base * 0.5f;
	front->AddText(font, bodySize, ImVec2(text.x + 1, text.y + 1), IM_COL32(0, 0, 0, 220), body, nullptr, wrap);
	front->AddText(font, bodySize, text, IM_COL32(255, 255, 255, 255), body, nullptr, wrap);
}

// ---- Install check (2026-11-03, InstallCheck.h) -----------------------------------------------------------------------------
// What the start-up check found, on screen: a panel at the top of the game's main menu (first 45 s), the same at the top in game
// when the main menu was skipped (first 20 s), a status-bar item, and Help > Install check with every result and the bug report.
// The old-DLL result has its own large red panel (RenderTwinDllWarning), so the panels and counts here leave it out.

static int InstallIssueCount() {
	int n = 0;
	for (const InstallCheck::Result& r : InstallCheck::Results()) if (r.level != InstallCheck::Level::Info && r.group != "twin") n++;
	return n;
}

// Help > Copy bug report: the report on the clipboard, and in the log (so a report without the paste still has it)
static void CopyBugReport() {
	const std::string report = InstallCheck::BugReport();
	ImGui::SetClipboardText(report.c_str());
	gMenu.reportCopiedAt = ImGui::GetTime();
	size_t lines = 0, start = 0;
	for (size_t end; (end = report.find("\r\n", start)) != std::string::npos; start = end + 2, lines++)
		Logger::Log("INSTALL report | %s", report.substr(start, end - start).c_str());
	Logger::Log("INSTALL bug report copied to the clipboard (%zu lines)", lines);
}

static void RenderInstallNotice(bool mainMenu) {
	static ULONGLONG firstSeen = 0;
	static bool shownAtMainMenu = false;
	const int issues = InstallIssueCount();
	if (!issues) return;
	if (!firstSeen) firstSeen = GetTickCount64();
	if (mainMenu) shownAtMainMenu = true;
	else if (shownAtMainMenu) return;	// read at the main menu already
	if (GetTickCount64() - firstSeen > (mainMenu ? 45000u : 20000u)) return;
	static bool logged = false;
	if (!logged) {	// once: a report's log then shows the player was told
		logged = true;
		Logger::Log("INSTALL notice shown %s: %d thing%s to fix", mainMenu ? "on the main menu" : "in game", issues, issues == 1 ? "" : "s");
	}

	const bool problem = InstallCheck::Count(InstallCheck::Level::Problem) > (Paths::TwinDll.empty() ? 0 : 1);
	char title[160];
	sprintf_s(title, "%s install check: %d thing%s to fix", Branding::ProductName, issues, issues == 1 ? "" : "s");
	std::string body;
	for (const InstallCheck::Result& r : InstallCheck::Results()) {
		if (r.level == InstallCheck::Level::Info || r.group == "twin") continue;
		body += "- " + r.title + "\n";
		if (!r.fix.empty()) body += "   " + r.fix + "\n";
	}
	char tail[200];
	sprintf_s(tail, "Details and a ready bug report: press %s, then Help > Install check.", DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));
	body += tail;

	const ImVec2 screen = ImGui::GetIO().DisplaySize;
	ImFont* font = ImGui::GetFont();
	const float base = ImGui::GetFontSize();
	const float titleSize = base * 1.3f, bodySize = base;
	const float pad = base * 0.7f;
	const float width = ImMin(screen.x - 2.0f * base, base * 60.0f);
	const float wrap = width - 2.0f * pad;
	const ImVec2 titleDim = font->CalcTextSizeA(titleSize, FLT_MAX, wrap, title);
	const ImVec2 bodyDim = font->CalcTextSizeA(bodySize, FLT_MAX, wrap, body.c_str());
	const float height = pad * 2.0f + titleDim.y + base * 0.4f + bodyDim.y;
	// At the top; in game under the old-DLL banner when that one shows (at the main menu that one is in the middle)
	const ImVec2 at((screen.x - width) * 0.5f, base + (!mainMenu && !Paths::TwinDll.empty() ? base * 7.0f : 0.0f));

	ImDrawList* front = ImGui::GetForegroundDrawList();
	front->AddRectFilled(at, ImVec2(at.x + width, at.y + height), problem ? IM_COL32(60, 12, 8, 230) : IM_COL32(50, 36, 6, 230), base * 0.4f);
	front->AddRect(at, ImVec2(at.x + width, at.y + height), problem ? IM_COL32(255, 90, 60, 255) : IM_COL32(255, 190, 60, 255), base * 0.4f, 0, 2.0f);
	ImVec2 text(at.x + pad, at.y + pad);
	front->AddText(font, titleSize, ImVec2(text.x + 1, text.y + 1), IM_COL32(0, 0, 0, 220), title, nullptr, wrap);
	front->AddText(font, titleSize, text, IM_COL32(255, 220, 90, 255), title, nullptr, wrap);
	text.y += titleDim.y + base * 0.4f;
	front->AddText(font, bodySize, ImVec2(text.x + 1, text.y + 1), IM_COL32(0, 0, 0, 220), body.c_str(), nullptr, wrap);
	front->AddText(font, bodySize, text, IM_COL32(255, 255, 255, 255), body.c_str(), nullptr, wrap);
}

static void DrawInstallCheckWindow() {
	if (!gMenu.installCheckOpen) return;
	ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(Branding::InstallCheckTitle, &gMenu.installCheckOpen)) {
		static const ImVec4 kProblemRed(1.0f, 0.35f, 0.3f, 1.0f), kAmber(1.0f, 0.75f, 0.2f, 1.0f);
		ImGui::TextWrapped("What %s found when the game started: files left from New Vegas Reloaded, missing files, the settings "
			"file and the graphics setup. Problems and warnings come first, each with what to do.", Branding::ProductName);
		ImGui::Spacing();
		if (ImGui::Button("Copy bug report")) CopyBugReport();
		ImGui::SameLine();
		if (gMenu.reportCopiedAt >= 0.0 && ImGui::GetTime() - gMenu.reportCopiedAt < 4.0)
			ImGui::TextColored(MenuSkin::Heading(0.25f), "Copied: paste it into your report and attach the log file it names.");
		else
			ImGui::TextDisabled("version, GPU, DXVK, log file name, these results and your changed settings");
		ImGui::Separator();
		if (InstallCheck::Results().empty()) ImGui::TextUnformatted("Nothing found.");
		for (const InstallCheck::Result& r : InstallCheck::Results()) {
			const bool info = r.level == InstallCheck::Level::Info;
			const ImVec4 colour = r.level == InstallCheck::Level::Problem ? kProblemRed : info ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kAmber;
			ImGui::PushStyleColor(ImGuiCol_Text, colour);
			ImGui::TextWrapped("%s  %s", r.level == InstallCheck::Level::Problem ? "PROBLEM" : info ? "info" : "warning", r.title.c_str());
			ImGui::PopStyleColor();
			if (!r.fix.empty()) {
				ImGui::Indent();
				ImGui::TextWrapped("%s", r.fix.c_str());
				ImGui::Unindent();
			}
			ImGui::Spacing();
		}
	}
	ImGui::End();
}

// A small dim marker in the top-right corner while the F10 GPU/CPU profiler is running, so it is not
// left on by accident. Drawn on the foreground list, so it needs no window and shows with the menu closed.
static void RenderProfilerIndicator() {
	if (!GpuTimer::Enabled) return;

	const char* label = "PROF";
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const ImVec2 textSize = ImGui::CalcTextSize(label);
	const float radius = 3.0f, margin = 8.0f, gap = 5.0f;
	const ImVec2 textPos(display.x - margin - textSize.x, margin);
	const ImVec2 center(textPos.x - gap - radius, margin + textSize.y * 0.5f);

	ImDrawList* draw = ImGui::GetForegroundDrawList();
	draw->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 110), label);
	draw->AddText(textPos, IM_COL32(210, 210, 210, 120), label);
	draw->AddCircleFilled(center, radius, IM_COL32(230, 60, 60, 170));
}

// Escape (not while typing: there it only leaves the text box) or Alt closes the menu, once the key is let go, so the game never
// sees it held. Returns true when the menu closed.
static bool CloseMenuOnEscapeOrAlt() {
	struct CloseOnRelease {
		bool armed = false;
		bool Fire(bool pressed, bool held) {
			if (pressed) armed = true;
			if (!armed || held) return false;
			armed = false;
			return true;
		}
	};
	static CloseOnRelease escape, alt;
	const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput;
	const bool altPressed = ImGui::IsKeyPressed(ImGuiKey_LeftAlt) || ImGui::IsKeyPressed(ImGuiKey_RightAlt);
	if (escapePressed) escape.armed = true;
	if (altPressed) alt.armed = true;
	const bool closing = escape.Fire(false, ImGui::IsKeyDown(ImGuiKey_Escape)) ||
		alt.Fire(false, ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt));
	if (closing) SetOverlayVisible(false);
	return closing;
}

// ---- File menu actions ---------------------------------------------------------------------------------------------------

static void SaveSettingsNow() {
	TheSettingManager->SaveSettings();
	InterfaceManager->ShowMessage("Settings saved.");
}

// Letters and digits kept, anything else becomes '_' (for file names).
static std::string FileNamePart(const char* text) {
	std::string part;
	for (const char* c = text; c && *c; c++) part += isalnum((unsigned char)*c) ? *c : '_';
	return part;
}

// Where the player is, for the copy's file name: the worldspace and cell editor IDs (TTW-safe), and the grid for exteriors.
static std::string LocationForFileName() {
	if (!Player || !Player->parentCell) return std::string();
	TESObjectCELL* cell = Player->parentCell;
	TESWorldSpace* world = Player->GetWorldSpace();
	std::string location = world ? FileNamePart(world->GetEditorName()) : std::string();
	const std::string cellName = FileNamePart(cell->GetEditorName());
	if (!cellName.empty()) location += (location.empty() ? "" : "_") + cellName;
	if (TESObjectCELL::CellCoordinates* grid = cell->coords) {
		char coords[32] = {};
		_snprintf_s(coords, sizeof(coords), "_%d_%d", grid->x, grid->y);
		location += coords;
	}
	return location;
}

// File > Save Copy: the settings as VegasShaders_<yyyymmdd_hhmm>[_<location>].toml next to the DLL.
static void SaveSettingsCopy() {
	const std::string base = Paths::ModuleDir() + "VegasShaders";

	const time_t now = time(nullptr);
	tm local = {};
	localtime_s(&local, &now);
	char stamp[32] = {};
	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M", &local);

	const std::string location = LocationForFileName();
	std::string path = std::string(base) + "_" + stamp;
	if (!location.empty()) path += "_" + location;
	path += ".toml";

	TheSettingManager->SaveSettingsTo(path.c_str());
	InterfaceManager->ShowMessage("Settings copy saved.");
}

// File > Reload from Disk: every setting from the saved file again, after Yes in this box.
static void DrawDiskReloadPopup() {
	if (gMenu.diskConfirm) {	// asked from the menu bar: opened here, in the main window's ID scope, where it is drawn
		ImGui::OpenPopup("##ConfirmDisk");
		gMenu.diskConfirm = false;
	}
	if (!ImGui::BeginPopupModal("##ConfirmDisk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	ImGui::TextUnformatted("Reload from disk? All unsaved changes will be lost.");
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	const ImVec2 buttonSize(80.0f, 0.0f);
	if (ImGui::Button("Yes", buttonSize)) {
		TheSettingManager->RevertSettings();
		gMenu.sessionValues.clear();
		gMenu.fxModifier = -1;	// read again from the file
		gMenu.page.clear();
		gMenu.resetWheels = true;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", buttonSize)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// ---- Menu bar ------------------------------------------------------------------------------------------------------------

static const ImVec4 kWarnAmber(1.0f, 0.75f, 0.2f, 1.0f);

static void DrawFileMenu() {
	if (!ImGui::BeginMenu("File")) return;
	if (ImGui::MenuItem("Save Copy")) SaveSettingsCopy();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes a dated copy of the settings next to the DLL, named after where you are.");
	ImGui::Separator();
	if (ImGui::MenuItem("Revert")) RevertSessionValues();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Revert all settings to values at session start");
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
	if (ImGui::MenuItem("Developer options", nullptr, gMenu.devOptions)) {
		const bool on = !gMenu.devOptions;
		TheSettingManager->SetSetting("Main.Menu.Developer", "DeveloperOptions", on);
		TheSettingManager->LoadSettings();
		gMenu.devOptions = on;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Also show what players should not need to touch: test and fault-finding switches, the bug fixes and\n"
			"speed-ups that are on by default, A/B test paths, measuring tools, options that change almost nothing,\n"
			"and the step size and - / + buttons. Hidden settings keep working with their saved values.");
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
	if (ImGui::MenuItem("Copy bug report")) CopyBugReport();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Copies the version, GPU, DXVK, the log file's name, the install check and every setting you changed.\nPaste it into your report and attach that log file.");
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

// Effects (every effect on or off) and Max Quality (red while asked for: read once at start, so it needs Save and a restart).
static void DrawGlobalSwitches() {
	bool effects = TheSettingManager->SettingsMain.Main.RenderEffects;
	if (LabelledToggle("Effects", "##effects", &effects)) {
		TheSettingManager->SetSetting("Main.Main.Misc", "RenderEffects", effects);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Toggle all effects (hotkey: modifier + %s)", DikToName(TheSettingManager->SettingsMain.Menu.KeyEnable));

	const bool asked = TheSettingManager->GetSettingI("Main.Main.Quality", "MaxQuality") != 0;
	bool maxQuality = asked;
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
	if (asked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
	const bool flipped = LabelledToggle("Max Quality", "##maxquality", &maxQuality);
	if (asked) ImGui::PopStyleColor();
	if (flipped) {
		TheSettingManager->SetSetting("Main.Main.Quality", "MaxQuality", maxQuality);
		TheSettingManager->LoadSettings();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Max Quality: for screenshots, not for playing.\n"
			"Runs every reduced-resolution effect at full resolution, turns off every reduced-quality shortcut and\n"
			"pushes quality settings to their maximum. Expect a much lower frame rate (often half or less).\n"
			"Your own settings are kept: turn it off and restart to get them back exactly.\n"
			"Takes effect after Save and a game restart.%s",
			MaxQuality::Active ? "\nIt is ON now: red values in the menu are its own. You can change them: those changes are saved to VegasShaders.maxquality.user.toml, not to your own settings." : "");

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
	return ImGui::CalcTextSize("Effects").x + ImGui::CalcTextSize("Max Quality").x + ImGui::CalcTextSize("Advanced").x + MenuWidgets::ToggleWidth() * 3.0f +
		style.ItemInnerSpacing.x * 3.0f + style.ItemSpacing.x * 4.0f;
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
	if (!gOverlay.open) return;

	gMenu.devOptions = TheSettingManager->GetSettingI("Main.Menu.Developer", "DeveloperOptions") != 0;	// UNOFFICIAL 2026-10-25
	gMenu.advanced = TheSettingManager->GetSettingI("Main.Menu.Developer", "AdvancedSettings") != 0;	// 2026-11-05
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
	}
	ImGui::End();
	if (!keepOpen) {
		SetOverlayVisible(false);
		return;
	}
	if (!drawn) return;
	MenuTools::DrawWindows(gMenu.step);
	DrawInstallCheckWindow();
}
