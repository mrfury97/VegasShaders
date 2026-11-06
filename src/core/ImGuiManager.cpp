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
#include <sstream>
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
	{ "Shaders.HandBlood.Status",             "Enabled",            RestartRule::Changed },
	{ "Shaders.Cloth.Main",                   "Enabled",            RestartRule::TurnOn },
	{ "Shaders.WetWorld.Main",                "HudDrops",           RestartRule::TurnOn },	// HUD_RAIN_ON: above 0 at start
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
	if (key == "HudDrops") return get(r.Section, r.Key) > 0.0f ? 1.0f : 0.0f;
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
	SettingManager::Configuration::ConfigNode& nodeB, const std::string& prefix)
{
	SettingManager::Configuration::ConfigNode* rgb[3] = { &nodeR, &nodeG, &nodeB };
	ColourWheel& wheel = WheelFor(const_cast<const SettingManager::Configuration::ConfigNode**>(rgb), prefix);

	std::string title = prefix;	// "FogColor_" reads "FogColor"
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
static const char* MenuKeyLabel(const char* section, const char* key) {
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
	if (MenuWidgets::Toggle("##v", &on)) {
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
	if (wanted != stored) {
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

	if (overControl || MenuWidgets::RowLabelHovered())
		SettingTooltip(node, row.why, row.normalText, MenuWidgets::RowLabelClipped());
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
		"ScreenTolerance, EstimateBrightness, SunShadows, HitLamps, HitAmbientGrid, HitTextures, GlowBrightness, SkyPanorama",
		FogType::None, true, "How things behind you or hidden from view are lit and coloured when they show in a reflection.", nullptr },
	{ "Shaders.Reflections.RT", "Glossy surfaces",
		"VisibleNormals, RayReuse, ReuseRoughness, ContactSharpness, ContactSpread, RoughShare, RoughReach",
		FogType::None, false, "Blurred reflections on worn tiles, satin metal and wet stone: how sharp they stay and how cheap they are.", nullptr },
	{ "Shaders.Reflections.RT", "Weapon", "Weapon, WeaponMetal, WeaponRoughness",
		FogType::None, false, "Your first-person weapon reflects the ray traced scene.", nullptr },
	{ "Shaders.Reflections.RT", "Water", "Water, WaterSkipPlanar, WaterRoughness",
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
	{ "Shaders.WetWorld.Main", "Rain on your weapon",
		"HudDrops, HudDropSize, HudDropDensity, HudRefraction, HudReflection, HudGloss, HudWetGloss, HudWetTime, HudDryTime, HudDebug",
		FogType::None, true, "Rain drops gather on your first-person weapon while you stand in the rain, and dry off under cover.", nullptr },
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
	if (MenuWidgets::Toggle("##headerSwitch", &on)) {
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
static void RenderSettingsList(SettingManager::Configuration::SettingList& settings) {
	const bool isShader = (gMenu.page.find("Shaders") == 0);
	const bool devShown = DevOptionsShown();

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

// ---- The page -----------------------------------------------------------------------------------------------------------
// The search results, the New-in-patch page, or the selected section: title, help, the LUT pickers (LUT page) and the rows.
static void RenderContent() {
	if (SearchActive()) return RenderSearchResults();
	if (gMenu.page == kNewTabId) return RenderNewInPatch();
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
	PageOrder(rows);
	RenderPageHeader(gMenu.page);
	RenderSectionHelp(gMenu.page);	// UNOFFICIAL 2026-11-01: what the page is for, in plain words (kSectionHelp)
	if (gMenu.page == "Shaders.LUT.Main") RenderLutPage();
	RenderSettingsList(rows);
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
		if (MenuWidgets::Toggle("##on", &on)) {
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
static void DrawChildren(const std::string& path) {
	StringList children;
	TheSettingManager->FillMenuSections(&children, path.c_str());
	const bool shaders = path == "Shaders";
	for (const std::string& child : children)
		if (!(shaders && IsMovedShaderEntry(child)))	// UNOFFICIAL 2026-10-25: drawn under the entry it belongs with
			RenderSectionNode(path + "." + child, child, shaders);
	DrawMovedPages(path);
	for (const MenuNest& nest : kMenuNests)	// UNOFFICIAL 2026-10-27
		if (path == std::string("Shaders.") + nest.under) RenderSectionNode(std::string("Shaders.") + nest.name, nest.name, true);
}

static bool HiddenFromSidebar(const std::string& path, const std::string& name) {
	if (ShouldHideSection(name)) return true;
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
	if (ImGui::MenuItem("Save")) SaveSettingsNow();
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
}

static float GlobalSwitchesWidth() {
	const ImGuiStyle& style = ImGui::GetStyle();
	return ImGui::CalcTextSize("Effects").x + ImGui::CalcTextSize("Max Quality").x + MenuWidgets::ToggleWidth() * 2.0f +
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
