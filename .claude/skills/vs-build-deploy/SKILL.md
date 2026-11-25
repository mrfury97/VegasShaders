---
name: vs-build-deploy
description: How to build VegasShaders.dll with msbuild, where branch worktrees and builds go, release vs test builds, the offline shader checks (effect_compile, template_compile, Vulkan .spv), installing into the MO2 mod with backups, and the exact file list for deploying to another PC or a tester. Use before any build, offline check, install, worktree set-up or deploy instructions.
---

# Build, check, install, deploy

## Build
Don't use `build.bat` (it wipes and mirrors a deploy folder). Build with msbuild into `F:\VS-build` (proven 2026-11-18, about
2 minutes, warnings only; PowerShell, `$r` = repo root):

```
& "F:\Tools\VS2022BT\MSBuild\Current\Bin\MSBuild.exe" "$r\NewVegasReloaded\NewVegasReloaded.vcxproj" /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=$r\\" /p:OutDir=F:\VS-build\ /p:IntDir=F:\VS-build\obj\ /m /nologo /v:minimal
```

End the quoted SolutionDir with `\\`: a single `\` escapes the quote (MSB4184).

- Release vs test build: a release tree carries the untracked marker `src\core\ReleaseBuild.h` (`TestTools.h`, `ShelvedEffects.h`
  check it with `__has_include`); without it the test tools (auto-test link used by the bench slots) are built in. Changing the
  marker needs `/t:Rebuild`: msbuild misses `__has_include` changes. Slot runs at `F:\NVR-bench` need a TEST build (no marker).
- Toolchain on this PC: VS 2022 Build Tools with v142 (`F:\Tools\VS2022BT`), Vulkan SDK 1.3.296.0, RenderDoc; Python 3.10 only
  (`py -3.10 -I`; `python3` is a hanging Store stub). Full list: `F:\Claude Brain\00_GLOBAL\TOOL_PATHS.md`.

## Worktrees and build folders
- Branch worktrees go in `F:\Claude Brain\VS-branches\<branch>`, builds in `F:\VS-build\<branch>` (main's own output stays in
  `F:\VS-build`). Remove a worktree once its branch is merged into the test build or main.
- A fresh worktree needs `git submodule update --init` (lib/imgui), `packages\` copied from the main checkout (NuGet D3DX), and
  `src\core\ReleaseBuild.h` copied only if it is to be a release build.
- Git refuses to move or remove a worktree that has submodules: `git submodule deinit --all`, delete
  `.git\worktrees\<name>\modules`, then `git worktree move/remove`, then `git submodule update --init` if the worktree is kept.

## Offline checks
Built under `build\` in the main checkout:
- `effect-test\effect_compile.exe <file.fx.hlsl>` (D3DX only; Exposure and VolumetricFog fail there but load in game).
- `template-compile\template_compile.exe <template> <profile> NAME[=VALUE]...` (also `--batch`).
- Vulkan pass shaders (`src/hlsl/NewVegas/Compute`, .frag/.comp -> .spv, plus `variants.txt`), from Git Bash:
  `PATH=/f/Tools/VulkanSDK/1.3.296.0/Bin:$PATH bash tools/nvr-dxvk/compile-compute.sh <names>` (reproduces the committed bytes).
- Settings defaults: `py -3.10 -I tools/defaults_tool.py check .` (see the vs-effects-modular skill).

## Install into the owner's game
Copy only the changed files into the MO2 mod folder `E:\Mod Organizer 2 (Archive)-6194-2-1-5 (1)\mods\VegasShaders-1.1.0\` (its
`NVSE\Plugins\` and `Shaders\VegasShaders\`), game closed, with the backup script from the shared rules (backs up every replaced
file, byte-compares, one-command restore). Install backups live in `F:\VS-scratch\install-<name>\` and chain back to
`F:\VS-scratch\install-original-2026-11-18` (the MO2 mod before any of our installs). The owner's settings file:
`E:\Mod Organizer 2 (Archive)-6194-2-1-5 (1)\overwrite\NVSE\Plugins\VegasShaders.toml`. Then tell the user in one line what changed.

## Deploy somewhere else (another PC, a tester)
List exactly which files go where:
- `VegasShaders.dll` (+ `.pdb` if useful for a crash) -> `<deploy>\nvse\Plugins\`. If that folder still has an old
  `NewVegasReloaded.dll`, tell them to delete or rename it (two copies of the plugin must not load).
- Any new/changed runtime `.hlsl` files under `src/hlsl/NewVegas/Shaders/` -> the same relative path under
  `<deploy>\Shaders\VegasShaders\Shaders\` (loaded and compiled at runtime, not built into the DLL). On an install that still
  has only the old `Shaders\NewVegasReloaded\` folder the DLL uses that folder, so copy there instead (or move the whole old
  folder to the new name first).
- Mention `Shaders\VegasShaders\Shaders\Cache\` as the place to clear if a shader change doesn't seem to take effect
  (`ShaderRecord::LoadShader` caches compiled/preprocessed shaders per name and normally recompiles on content change, but
  it's a reasonable troubleshooting step).
