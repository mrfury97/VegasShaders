# DXVK input, cursor and input blocking

> Technical reference. Moved from the `vs-dxvk-input` skill on 2026-11-25 (content unchanged). Read it before touching `ImGuiManager` input,
> the toggle key, input blocking, or any hook on input, UI or game objects.

## DXVK input: critical finding
FNV is commonly run under **DXVK** (Vulkan translation layer). DXVK suppresses `WM_KEYDOWN` and `WM_MOUSEWHEEL` before they reach
any subclassed WndProc. **Do not rely on WM messages for keyboard or scroll wheel input.**

`WM_INPUT` (Raw Input) is also unreliable: DXVK's message pump may not dispatch `WM_INPUT` to the subclassed WndProc at all.
**Do not use WM_INPUT for any keyboard input.**

### What works instead
| Input | Solution |
|---|---|
| Keyboard (all: text, nav, characters) | `GetAsyncKeyState` polled for VK 1-255 in `NewFrame`, edge-detect against `s_prevKeyState`, inject via `io.AddKeyEvent` + `ToUnicode` -> `io.AddInputCharacterUTF16` |
| Scroll wheel | Read `DIMOUSESTATE2::lZ` from the DI mouse buffer *before* zeroing it in `HookedGetDeviceState`, accumulate in `s_pendingWheelDelta`, inject via `io.AddMouseWheelEvent` in `NewFrame` |
| Mouse buttons | `GetAsyncKeyState(VK_LBUTTON/RBUTTON/MBUTTON)` -> `io.AddMouseButtonEvent` each frame |
| Toggle key open/close | `GetAsyncKeyState(DikToVk(KeyEnable))` polled in `NewFrame` every render frame. `DikToVk` has a hardcoded table for extended keys that `MapVirtualKey` gets wrong (END, HOME, arrows, etc.) |

**Rule**: everything goes through `GetAsyncKeyState` polled on the render thread. No WM messages, no cross-thread queues.

### DIK -> VK conversion
`MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX)` **fails silently** for E0-prefixed extended keys (END=0xCF, HOME=0xC7, arrow keys,
Insert, Delete, etc.): it returns 0 or the wrong VK. Use the hardcoded `DikToVk()` table in `ImGuiManager.cpp` instead.
Non-extended keys fall through to `MapVirtualKey`, which handles them correctly.

### ImGui keyboard navigation
`ImGuiConfigFlags_NavEnableKeyboard` is intentionally **not set**. With it enabled, END/HOME/arrow keys leak into ImGui widget
navigation (jumping to the last item, etc.) even while the overlay is trying to close. Text field editing is unaffected: it uses
a separate code path that does not need nav.

### What does NOT work under DXVK
- `ImGui_ImplWin32_WndProcHandler` for keyboard/scroll (WM messages never arrive)
- `WM_KEYDOWN` + `ToUnicode` in WndProc
- Any `WM_MOUSEWHEEL` handler in WndProc
- `WM_INPUT` for keyboard input (may never be dispatched to WndProc under DXVK)
- `MapVirtualKey` for extended DIK scancodes (returns 0 or wrong VK)

`WM_ACTIVATE` and other non-input system messages still arrive normally.

## Mouse cursor position: click-to-refocus quirk
When the user **clicks** the game window to refocus (rather than Alt+Tab), the OS generates a `WM_MOUSEMOVE`. The ImGui Win32
backend handles it by calling `TrackMouseEvent`, which sets its internal `bd->MouseTrackedArea = 1`. Once that flag is set,
`ImGui_ImplWin32_UpdateMouseData` **suppresses** its `GetCursorPos` fallback. No further `WM_MOUSEMOVE` arrives because the game
registers raw input with `RIDEV_NOLEGACY`, which eats legacy mouse messages. Result: the cursor position freezes where the
click landed.

**Fix** (`NewFrame`, after `ImGui_ImplWin32_NewFrame`): inject our own `io.AddMousePosEvent` from `GetCursorPos` +
`ScreenToClient`. Last write wins in ImGui's event queue, so it overrides the stale position `UpdateMouseData` left. A bounds
check makes it inject only when the cursor is inside the client rect.

Multi-monitor alt-tab fix (complementary): `SetOverlayVisible(true)` warps the OS cursor to the window centre via `SetCursorPos`
if it is outside the client rect. This runs once on open; the per-frame injection above keeps it correct afterwards.

## Input blocking while the overlay is open
- **Mouse movement**: vtable hook on `IDirectInputDevice8::GetDeviceState` (slot 9), zeros the `DIMOUSESTATE2` buffer. Mouse
  device only: the keyboard device has a separate vtable under DXVK.
- **Keyboard actions**: `DIHookControl::SetKeyDisableState` via xNVSE for codes 0-265 (`BlockGameInput`).
- **Mouse buttons / wheel macros**: the same `DIHookControl` call covers codes 256+.

## xNVSE first
Always check xNVSE (`src/NewVegas/nvse/`) before writing custom hooks for anything that interacts directly with the game (input,
UI, game objects). Prefer xNVSE APIs over vtable patches where available.
