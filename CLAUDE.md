# Room 02: New Vegas Reloaded (Vegas Shaders)

Claude Code reads this file at the start of every session. The shared rules (about me, what needs my word, how to talk, decide,
test and run sessions, safety, coding standards) live in `F:\Claude Brain\CLAUDE.md`, one folder up, which Claude Code loads
too; this file adds what is specific to Vegas Shaders. The second half ("Project notes") is the technical knowledge gathered so far.

Shared rules load automatically from `F:\Claude Brain\CLAUDE.md` (parent folder). Tool paths: `F:\Claude Brain\00_GLOBAL\TOOL_PATHS.md`.

## What this room does
Rendering: the Vegas Shaders plugin, DX9 and DXVK, the deferred renderer, shadows, post-processing and shaders, rendering
performance. Not here: other engine plugins (room 01), pure engine research (room 05), cross-project test runs (room 06).
Room index: `F:\Claude Brain\ROOMS.md`.

## This repo and install
- Repo: `F:\Claude Brain\VS`, cloned from my fork github.com/mrfury97/VegasShaders (remote `origin`, branch `main`); the original
  github.com/adeesababa/VegasShaders is remote `upstream` (read only for me). Build output: `F:\VS-build`.
  Scratch for big files: `F:\VS-scratch`.
- Vegas Shaders is installed as the MO2 mod `VegasShaders-1.1.0` (enabled). Other VS mods in that profile: `VS RT test room` and
  `VegasShaders-1.0.1-DLSS-helper` (enabled); `VegasShaders` and `VegasShaders-1.0.2-RT-lamp-shadows-test-2(1)` (disabled).
  Old `NewVegasReloaded.dll` mods exist but none of them is enabled; keep it that way (two plugins must not load).
- Decompiled game code sits in `NV source code\` in this folder and `IDA9\` holds the IDA copy that is never run; both are
  excluded from git locally (`.git\info\exclude`). Address lookup: `F:\Claude Brain\fnv_decomp\build\units.json`.
- Without asking you may also install into the `VegasShaders-1.1.0` MO2 mod (game closed, backup script, see the shared rules).

## Agents in this room (`.claude\agents\`)
`lead-architect` (reviews plans and diffs, read only), `shader-engineer` (HLSL and GLSL), `renderer-engineer` (C++ render
pipeline), `performance-engineer` (frame time and profiling), `qa-engineer` (builds, offline checks, in-game tests),
`research-agent` (engine, Gamebryo, DX9 and DXVK facts, read only). Risky changes get a `lead-architect` review before they are
committed or installed.

## The project in brief
- Vegas Shaders (formerly New Vegas Reloaded) is a 32-bit xNVSE plugin for Fallout: New Vegas: `VegasShaders.dll`, built from
  `NewVegasReloaded\NewVegasReloaded.vcxproj` (Release, Win32, MSVC toolset v142). Build command: "Build/deploy workflow" below.
- Shaders under `src\hlsl\NewVegas\` are compiled at runtime, not built into the DLL: a shader change ships as files under
  `<game>\Data\Shaders\VegasShaders\`. The shader cache reuses an entry only when the whole preprocessed text matches, so editing a
  shared include recompiles every shader that uses it (minutes on a slow PC).
- One folder per effect in `src\effects\<Name>\` with its C++, `README.md` and `defaults.toml`. Read the effect's README before
  changing it and update it in the same change (details under "Effects are modular" below).
- Settings: `resource\VegasShaders.defaults.toml` is what ships; a key missing from it is not read. Changing a default that players
  may have saved needs a migration step in `Configuration::UpdateOldDefaults()` (`src\core\SettingManager.cpp`) and a higher
  settings version, because the menu's Save writes every key.
- Many players run the game under DXVK: window messages for keys, mouse wheel and raw input never reach the plugin, so menu input
  is polled with `GetAsyncKeyState`. Read "DXVK input" below before touching input.
- Check xNVSE (`src\NewVegas\nvse\`) before writing your own hook for input, UI or game objects.
- Ray-traced effects run in a separate 64-bit helper through the project's DXVK fork, because 32-bit Vulkan has no ray tracing.
- Design docs live in `docs\`. There is no doc index yet; when a doc goes stale, rewrite or delete it rather than leaving it.

## Product rules
- A feature that works ships on by default. Flipping a default that players may have saved needs a migration.
- One switch, preset or quality level beats several separate switches.
- **Deferred first (set by me 2026-11-18).** Shading and lighting work targets the deferred path first: the G-buffer
  (COLOR2 bumped normal and material, COLOR3 smooth normal and distance), the deferred sun and lamp passes, and effects
  that read them. Change the forward templates only for geometry that stays forward (water, sky, particles, decals,
  first person, distant trees, glass, menus), and so that `DeferredRenderer = 0` still renders correctly; it need not gain
  the feature. Every plan says which path each change uses, and why when it's forward. Background:
  `docs\deferred-vs-forward.md` (on `vs-1.1.0-plus-fork`).

---

# Project notes

## Runtime names (2026-11-02 rename) — `src/core/Paths.h/.cpp` owns every one
| New | Old (still read when the new one is missing) |
|---|---|
| `Data\NVSE\Plugins\VegasShaders.dll` (+ `.pdb`) | `NewVegasReloaded.dll` (an old copy left beside the new one is logged as an error: two plugins) |
| `VegasShaders.toml` (user settings; always written under this name) | `NewVegasReloaded.dll.toml` (read only; never changed or deleted) |
| `VegasShaders.defaults.toml`, `.maxquality.toml`, `.maxquality.user.toml`, `.materials.txt`, `.imgui.ini` | `NewVegasReloaded.dll.<same>` / `NewVegasReloaded.imgui.ini` |
| `Data\Shaders\VegasShaders\` (Shaders, Effects, Compute, caches) | `Data\Shaders\NewVegasReloaded\` |
| `Data\Textures\VegasShaders\` (LUTs) | `Data\Textures\NewVegasReloaded\` |
| `Data\VegasShaders\` (Presets, Variants, Keywords, Skins, EnabledVariants.ini) | `Data\NVR\` |
| `VegasShaders-Logs\VegasShaders_<date>.log` (game folder) | none: logs are only written under the new names |

Folders are chosen once at startup (new if it exists, else old if it exists, else new) and logged in one `PATHS folders:` line.
Kept unchanged on purpose: the xNVSE plugin name `NewVegasReloaded` (Main.cpp; mods check it), the console prefix `NVR`, every
settings section/key, `TESR_*` names, preset formats, log line tags, and the legacy `NewVegasReloaded.dll.config` (only read).
New code that touches a runtime file or folder goes through `Paths::` (or `VS_LOGS_FOLDER` for the logs), never a literal path.

## Build/deploy workflow
Don't use `build.bat` (it wipes and mirrors a deploy folder). Build with msbuild into `F:\VS-build` (proven 2026-11-18, about 2 minutes, warnings only; PowerShell, `$r` = repo root):
`& "F:\Tools\VS2022BT\MSBuild\Current\Bin\MSBuild.exe" "$r\NewVegasReloaded\NewVegasReloaded.vcxproj" /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=$r\\" /p:OutDir=F:\VS-build\ /p:IntDir=F:\VS-build\obj\ /m /nologo /v:minimal`
(end the quoted SolutionDir with `\\`: a single `\` escapes the quote, MSB4184). Branch worktrees go in
`F:\Claude Brain\VS-branches\<branch>`, builds in `F:\VS-build\<branch>` (main's own output stays in `F:\VS-build`); remove a
worktree once its branch is merged into the test build or main. Offline checks, built under `build\`:
`effect-test\effect_compile.exe <file.fx.hlsl>` (D3DX only; Exposure and VolumetricFog fail there but load in game) and
`template-compile\template_compile.exe <template> <profile> NAME[=VALUE]...`. Then install by copying only
the changed files into the MO2 mod folder `E:\Mod Organizer 2 (Archive)-6194-2-1-5 (1)\mods\VegasShaders-1.1.0\` (its
`NVSE\Plugins\` and `Shaders\VegasShaders\`), game closed, with the backup script from "How to test". Then tell the user in one
line what changed. When installing somewhere else (another PC, a tester), list exactly which files go where:
- `VegasShaders.dll` (+ `.pdb` if useful for a crash) -> `<deploy>\nvse\Plugins\`. If that folder still has an old
  `NewVegasReloaded.dll`, tell them to delete or rename it (two copies of the plugin must not load).
- Any new/changed runtime `.hlsl` files under `src/hlsl/NewVegas/Shaders/` -> the same relative
  path under `<deploy>\Shaders\VegasShaders\Shaders\` (these are loaded/compiled at runtime,
  not built into the DLL). On an install that still has only the old `Shaders\NewVegasReloaded\` folder the DLL
  uses that folder, so copy there instead (or move the whole old folder to the new name first).
- Mention `Shaders\VegasShaders\Shaders\Cache\` as the place to clear if a shader change
  doesn't seem to take effect (`ShaderRecord::LoadShader` caches compiled/preprocessed shaders
  per name and normally auto-recompiles on content change, but it's a reasonable troubleshooting
  step)

## Key architecture
- `src/core/ImGuiManager.cpp` — ImGui settings overlay (DX9 + Win32 backend)
- `src/core/ShaderManager.cpp` — main render loop; calls `ImGuiManager::NewFrame()` and `Render()`
- `src/core/SettingManager.cpp` — TOML-backed settings; ImGui menu is auto-populated from this
- `lib/imgui` — git submodule, docking branch (`git submodule update --init --recursive` required after fresh clone)
- xNVSE headers live under `src/NewVegas/nvse/`

## DXVK input — critical finding
FNV is commonly run under **DXVK** (Vulkan translation layer). DXVK suppresses
`WM_KEYDOWN` and `WM_MOUSEWHEEL` before they reach any subclassed WndProc.
**Do not rely on WM messages for keyboard or scroll wheel input.**

`WM_INPUT` (Raw Input) is also unreliable — DXVK's message pump may not dispatch
`WM_INPUT` to the subclassed WndProc at all. **Do not use WM_INPUT for any keyboard input.**

### What works instead
| Input | Solution |
|---|---|
| Keyboard (all — text, nav, characters) | `GetAsyncKeyState` polled for VK 1–255 in `NewFrame`, edge-detect against `s_prevKeyState`, inject via `io.AddKeyEvent` + `ToUnicode` → `io.AddInputCharacterUTF16` |
| Scroll wheel | Read `DIMOUSESTATE2::lZ` from the DI mouse buffer *before* zeroing it in `HookedGetDeviceState`, accumulate in `s_pendingWheelDelta`, inject via `io.AddMouseWheelEvent` in `NewFrame` |
| Mouse buttons | `GetAsyncKeyState(VK_LBUTTON/RBUTTON/MBUTTON)` → `io.AddMouseButtonEvent` each frame |
| Toggle key open/close | `GetAsyncKeyState(DikToVk(KeyEnable))` polled in `NewFrame` every render frame. `DikToVk` has a hardcoded table for extended keys that `MapVirtualKey` gets wrong (END, HOME, arrows, etc.) |

**Rule**: everything goes through `GetAsyncKeyState` polled on the render thread. No WM messages, no cross-thread queues.

### DIK → VK conversion
`MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX)` **fails silently** for E0-prefixed extended keys
(END=0xCF, HOME=0xC7, arrow keys, Insert, Delete, etc.) — returns 0 or wrong VK.
Use the hardcoded `DikToVk()` table in `ImGuiManager.cpp` instead. Non-extended keys
fall through to `MapVirtualKey` which handles them correctly.

### ImGui keyboard navigation
`ImGuiConfigFlags_NavEnableKeyboard` is intentionally **not set**. With it enabled,
END/HOME/arrow keys leak into ImGui widget navigation (jumping to last item, etc.)
even while the overlay is trying to close. Text field editing is unaffected — it uses
a separate code path that does not require nav to be enabled.

### What does NOT work under DXVK
- `ImGui_ImplWin32_WndProcHandler` for keyboard/scroll (WM messages never arrive)
- `WM_KEYDOWN` + `ToUnicode` in WndProc
- Any `WM_MOUSEWHEEL` handler in WndProc
- `WM_INPUT` for keyboard input (may never be dispatched to WndProc under DXVK)
- `MapVirtualKey` for extended DIK scancodes (returns 0 or wrong VK)

WM_ACTIVATE and other non-input system messages still arrive normally.

## Mouse cursor position — click-to-refocus quirk
When the user **clicks** the game window to refocus (rather than Alt+Tab), the OS
generates a `WM_MOUSEMOVE`. The ImGui Win32 backend handles this by calling
`TrackMouseEvent`, which sets its internal `bd->MouseTrackedArea = 1`. Once that
flag is set, `ImGui_ImplWin32_UpdateMouseData` **suppresses** its `GetCursorPos`
fallback path. No further `WM_MOUSEMOVE` arrives because the game registers raw
input with `RIDEV_NOLEGACY`, which eats legacy mouse messages. Result: cursor
position freezes wherever it was when the click landed.

**Fix** (`NewFrame`, after `ImGui_ImplWin32_NewFrame`): inject our own
`io.AddMousePosEvent` from `GetCursorPos` + `ScreenToClient`. Last write wins in
ImGui's event queue, so it overrides whatever stale position `UpdateMouseData` left.
A bounds-check ensures we only inject when the cursor is inside the client rect.

Multi-monitor alt-tab fix is complementary: `SetOverlayVisible(true)` warps the OS
cursor to the window center via `SetCursorPos` if it is outside the client rect.
This runs once on open; the per-frame injection above keeps it correct thereafter.

## Input blocking when overlay is open
- **Mouse movement**: vtable hook on `IDirectInputDevice8::GetDeviceState` (slot 9), zeros `DIMOUSESTATE2` buffer. Mouse device only — keyboard device has a separate vtable under DXVK.
- **Keyboard actions**: `DIHookControl::SetKeyDisableState` via xNVSE for codes 0–265 (`BlockGameInput`).
- **Mouse buttons / wheel macros**: same `DIHookControl` call covers codes 256+.

## xNVSE
Always check xNVSE (`src/NewVegas/nvse/`) before writing custom hooks for anything that interacts directly with the game (input, UI, game objects). Prefer xNVSE APIs over vtable patches where available.

## Effects are modular (2026-11-02)
- One folder per effect: `src/effects/<Name>/` holds its C++, `README.md` (what it does, inputs, settings, passes, edge cases, Vulkan path,
  what correct/wrong looks like), `defaults.toml` (its settings defaults) and `SHADERS.txt` (its shader files; the shaders themselves stay in
  `src/hlsl/NewVegas/`, mirroring the game's `Shaders\VegasShaders\` layout). Index: `src/effects/INDEX.md` (`tools/effects_index.py`).
- **Read the effect's README.md before changing it, and update it in the same change.** A stale doc is worse than none.
- Settings defaults: edit the fragment (`src/effects/<Name>/defaults.toml`, or `resource/defaults/<Name>.toml` / `Core.toml` for sections
  without an effect folder), then `py -3.10 -I tools/defaults_tool.py merge .` (this PC has only Python 3.10); `check` verifies the committed
  `resource/VegasShaders.defaults.toml` matches. A new section also needs its line in `resource/defaults/ORDER.txt`.
- Frame order: `src/core/EffectChain.cpp` (two ordered step tables, pre- and post-tonemap). Adding an effect = a step there, a member in
  `ShaderManager` (Effects struct + registration), its folder, its README.
