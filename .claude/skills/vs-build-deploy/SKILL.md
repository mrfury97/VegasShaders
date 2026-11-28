---
name: vs-build-deploy
description: "Workflow: build VegasShaders.dll with msbuild into F:\\VS-build, run the offline shader checks (effect_compile, template_compile, Vulkan .spv, defaults check), install changed files into the MO2 mod with backups, or write deploy instructions for another PC or a tester. Use before any build, offline check, install, worktree set-up or deploy instructions."
---

# Build, check, install, deploy

## Task
Turn a source change into a built DLL and checked shaders, put it in the owner's MO2 mod (or tell someone else which files go where).

## Prerequisites
- Toolchain (this PC): VS 2022 Build Tools v142 at `F:\Tools\VS2022BT`, Vulkan SDK 1.3.296.0, RenderDoc, Python 3.10 only
  (`py -3.10 -I`; `python3` is a hanging Store stub). Full list: `F:\Claude Brain\00_GLOBAL\TOOL_PATHS.md`.
- Game closed before any install. Install rights and the backup rule: shared `F:\Claude Brain\CLAUDE.md`.
- A fresh worktree needs `git submodule update --init` (lib/imgui) and `packages\` copied from the main checkout (NuGet D3DX).
  Copy `src\core\ReleaseBuild.h` only if it is to be a release build.

## Steps
1. **Worktree** (branch work): `F:\Claude Brain\VS-branches\<branch>`; builds in `F:\VS-build\<branch>` (main's output stays in
   `F:\VS-build`). Remove the worktree once its branch is merged. Git refuses to move or remove a worktree that has submodules:
   `git submodule deinit --all`, delete `.git\worktrees\<name>\modules`, then `git worktree move/remove`, then
   `git submodule update --init` if the worktree is kept.
2. **Build.** Don't use `build.bat` (it wipes and mirrors a deploy folder). PowerShell, `$r` = repo root (proven 2026-11-18, about
   2 minutes, warnings only):
   ```
   & "F:\Tools\VS2022BT\MSBuild\Current\Bin\MSBuild.exe" "$r\NewVegasReloaded\NewVegasReloaded.vcxproj" /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=$r\\" /p:OutDir=F:\VS-build\ /p:IntDir=F:\VS-build\obj\ /m /nologo /v:minimal
   ```
   End the quoted SolutionDir with `\\`: a single `\` escapes the quote (MSB4184).
3. **Release vs test build.** A release tree carries the untracked marker `src\core\ReleaseBuild.h` (`TestTools.h`,
   `ShelvedEffects.h` check it with `__has_include`); without it the test tools (the auto-test link used by the bench slots) are
   built in. Changing the marker needs `/t:Rebuild`: msbuild misses `__has_include` changes. Bench slot runs need a TEST build
   (no marker): see the `vs-bench-check` skill.
4. **Offline checks** (tools built under `build\` in the main checkout):
   - `build\effect-test\effect_compile.exe <file.fx.hlsl>` (D3DX only; Exposure and VolumetricFog fail there but load in game).
   - `build\template-compile\template_compile.exe <template> <profile> NAME[=VALUE]...` (also `--batch`).
   - Vulkan pass shaders (`src/hlsl/NewVegas/Compute`, .frag/.comp -> .spv, plus `variants.txt`), from Git Bash:
     `PATH=/f/Tools/VulkanSDK/1.3.296.0/Bin:$PATH bash tools/nvr-dxvk/compile-compute.sh <names>` (reproduces the committed bytes).
   - Settings defaults: `py -3.10 -I tools/defaults_tool.py check .` (see the `vs-effects-modular` skill).
   - **Runtime shader gate (required before any install, since 2026-11-28).** The offline compiles above only build the define sets typed
     by hand, and their newer compiler fits shaders the game's D3DX9 rejects; at runtime the DLL quietly drops a feature define (e.g.
     DL_LAND_SPLIT) and keeps drawing, so a failure is only a log warning (the 1.1.6 merge's DeferredLamps X4505 got past this way). The
     faithful compile is the game's own first start with the new build: one bench slot run (skill `vs-bench-check`) of a scene with lamps
     and of an outdoor scene, then `py -3.10 -I tools/log_gate.py <previous build's log> <new build's log> --new-cache <slot>\Data\Shaders\VegasShaders\Shaders\Cache`
     on each. Any NEW line (did not compile, X4505, made again without, did not load, compiled without it, deferred record only,
     [D3-FALLBACK]) or new `<binary>.failed.hlsl` in the cache fails the build: fix it, or explain each line as expected before installing.
     The failed record's exact text and defines are in the slot's cache as `<binary>.failed.hlsl` / `.failed.defines`.
5. **Install into the owner's game.** Copy only the changed files into
   `E:\Mod Organizer 2 (Archive)-6194-2-1-5 (1)\mods\VegasShaders-1.1.0\` (`NVSE\Plugins\` and `Shaders\VegasShaders\`) with the
   backup script (backs up every replaced file, byte-compares, one-command restore). Backups live in
   `F:\VS-scratch\install-<name>\` and chain back to `F:\VS-scratch\install-original-2026-11-18` (the mod before any of our
   installs). The owner's settings file: `E:\Mod Organizer 2 (Archive)-6194-2-1-5 (1)\overwrite\NVSE\Plugins\VegasShaders.toml`.
   Then tell the owner in one line what changed.
6. **Deploy somewhere else** (another PC, a tester): list exactly which files go where:
   - `VegasShaders.dll` (+ `.pdb` for a crash) -> `<deploy>\nvse\Plugins\`. If that folder still has an old
     `NewVegasReloaded.dll`, tell them to delete or rename it (two copies of the plugin must not load).
   - New or changed runtime `.hlsl` under `src/hlsl/NewVegas/Shaders/` -> the same relative path under
     `<deploy>\Shaders\VegasShaders\Shaders\` (compiled at runtime, not built into the DLL). On an install with only the old
     `Shaders\NewVegasReloaded\` folder the DLL uses that folder: copy there, or move the whole folder to the new name first.
   - RT helper changes: `NvrRtHelper.exe` (x64) must be replaced too; the plugin refuses a helper of another protocol.

## Validation
- msbuild: zero errors; re-read the diff before building.
- Offline checks above pass for every changed file; `defaults_tool.py check .` clean when settings changed.
- After an install: the byte-compare of the copies passed (the script does it); the log `VegasShaders-Logs\VegasShaders_<date>.log`
  in the game folder has no shader errors after the first start.

## Troubleshooting
- Shader cache (one wording, used in the repo `CLAUDE.md`, `docs/REFERENCE.md "Architecture"` and the memory note): the cache reuses an
  entry only when the whole preprocessed text matches, so changed shaders recompile by themselves and editing a shared include
  recompiles every shader using it (minutes on a slow PC). Do not tell the owner to delete the cache after an install; clear
  `Shaders\VegasShaders\Shaders\Cache\` only if a shader genuinely looks stale (for example a failed compile left an old binary).
- MSB4184 on SolutionDir: see the `\\` note in step 2.
- A test tool is missing from a build: the marker `ReleaseBuild.h` is present; remove it and `/t:Rebuild`.

## Links
- Runtime names: `docs/REFERENCE.md "Runtime paths"`. Code map: `docs/REFERENCE.md "Architecture"`.
- Bench run of a build: skill `vs-bench-check`. Porting upstream: skill `vs-upstream-hand-port`.
