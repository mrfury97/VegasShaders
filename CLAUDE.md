# Room 02: New Vegas Reloaded (Vegas Shaders)

The shared rules load from `F:\Claude Brain\CLAUDE.md` (parent folder); this file adds only what is specific to this room.
Technical knowledge is in the skills below; state and the owner's decisions are in this room's memory. Tool paths:
`F:\Claude Brain\00_GLOBAL\TOOL_PATHS.md`. Room index: `F:\Claude Brain\ROOMS.md`.

## What this room does
Rendering: the Vegas Shaders plugin (32-bit xNVSE plugin `VegasShaders.dll`, formerly New Vegas Reloaded), DX9 and DXVK, the
deferred renderer, shadows, post-processing and shaders, rendering performance. Not here: other engine plugins (room 01), pure
engine research (room 05), cross-project test runs (room 06).

## Where things are
- Repo `F:\Claude Brain\VS`, from my fork github.com/mrfury97/VegasShaders (remote `origin`, branch `main`); the original
  github.com/adeesababa/VegasShaders is remote `upstream` (read only for me).
- Branch worktrees `F:\Claude Brain\VS-branches\<branch>`; build output `F:\VS-build` (branches `F:\VS-build\<branch>`);
  scratch for big files and install backups `F:\VS-scratch`.
- MO2 mod `VegasShaders-1.1.0` (enabled) is the install. Also enabled: `VS RT test room`, `VegasShaders-1.0.1-DLSS-helper`;
  disabled: `VegasShaders`, `VegasShaders-1.0.2-RT-lamp-shadows-test-2(1)`. Old `NewVegasReloaded.dll` mods exist but none is
  enabled; keep it that way (two plugins must not load).
- Without asking you may install into `VegasShaders-1.1.0` (game closed, backup script, see the shared rules).
- `NV source code\` (decompiled game) and `IDA9\` (never run) sit in this folder, excluded from git locally
  (`.git\info\exclude`); never commit them. Address lookup: `F:\Claude Brain\fnv_decomp\build\units.json`.
- Design docs: `docs\` (rewrite or delete a stale one). Effect docs: `src\effects\<Name>\README.md`.

## Product rules
- A feature that works ships on by default.
- Settings: `resource\VegasShaders.defaults.toml` is what ships; a key missing from it is not read. Changing a default players may
  have saved needs a migration step in `Configuration::UpdateOldDefaults()` (`src\core\SettingManager.cpp`) and a higher
  settings version, because the menu's Save writes every key.
- One switch, preset or quality level beats several separate switches.
- **Deferred first (set by me 2026-11-18).** Shading and lighting work targets the deferred path first: the G-buffer (COLOR2
  bumped normal and material, COLOR3 smooth normal and distance), the deferred sun and lamp passes, and effects that read them.
  Change the forward templates only for geometry that stays forward (water, sky, particles, decals, first person, distant trees,
  glass, menus), and so that `DeferredRenderer = 0` still renders correctly; it need not gain the feature. Every plan says which
  path each change uses, and why when it's forward. Background: `docs\deferred-vs-forward.md`.
- Read an effect's README before changing it and update it in the same change.
- Many players run under DXVK: never rely on window messages for input (vs-dxvk-input). Check xNVSE before writing a hook.

## Agents (`.claude\agents\`)
`lead-architect` (reviews plans and diffs, read only), `shader-engineer` (HLSL and GLSL), `renderer-engineer` (C++ render
pipeline), `performance-engineer` (frame time and profiling), `qa-engineer` (builds, offline checks, in-game tests),
`research-agent` (engine, Gamebryo, DX9 and DXVK facts, read only). Risky changes get a `lead-architect` review before they are
committed or installed.

## Skills (`.claude\skills\`, load the one that fits before starting)
- `vs-build-deploy`: before a build, offline shader check, worktree set-up, MO2 install, or deploy list for a tester.
- `vs-effects-modular`: before changing or adding an effect, a settings default or a migration, or EffectChain.
- `vs-dxvk-input`: before touching menu input, the toggle key, cursor, input blocking, or any input/UI/game-object hook.
- `vs-runtime-paths`: before code that reads or writes a runtime file or folder, or when an install has old NVR names.
- `vs-architecture`: map of the key source files, runtime shader compile and cache, the 64-bit RT helper.
