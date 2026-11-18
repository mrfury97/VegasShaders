#pragma once

// The settings menu: an ImGui overlay on the game's own D3D9 device and window. Set up on the first frame both exist, then
// fed and drawn once per frame from the render hooks (Render.cpp). Its state lives in ImGuiManager.cpp.
class ImGuiManager {
public:
	static void		Initialize();		// NewFrame calls it until the device and the window are there
	static void		NewFrame();			// input (polled: see CLAUDE.md, DXVK), the menu key, then ImGui's new frame
	static void		Render();			// the menu while it is open, the main-menu hint and markers always; then to the screen
	static void		Shutdown();			// the window hook removed and ImGui freed

	static bool		IsVisible();		// the menu is open
	static void		SetVisible(bool visible);	// the flag alone (opening and closing properly is the menu key's job)
	static HWND		GetWindow();		// the game window the overlay hooks
	static bool		TextInputActive();	// UNOFFICIAL 2026-11-01: an NVR menu text box has the keyboard (the report key waits)
	static void		TestShowMenu(const char* Section, bool ShowTests);	// UNOFFICIAL 2026-10-19, test builds only: SettingShots 'menu' (nullptr closes)
	// UNOFFICIAL 2026-11-12: the menu level of a setting (description: the default's first comment): 0 a player's setting, 1 shown only
	// with Developer options (test rules + [Main.Menu.Developer] DevOnly; not a row of a simple view, which every player sees), 2 Internal
	// (its Internal list: never shown). Needs only the settings to be loaded, not the menu.
	static int		SettingLevel(const char* section, const char* key, const char* description);
};
