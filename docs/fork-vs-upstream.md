# Fork vs upstream: rebased onto 1.1.0 (2026-11-18)

The fork (mrfury97/VegasShaders `origin/main`, 46e575e) was compared with upstream 1.1.0 (adeesababa/VegasShaders tag v1.1.0,
d3477de). Upstream had already absorbed almost all of the fork, so the fork's own items were brought over onto upstream 1.1.0 on the
side branch `vs-1.1.0-plus-fork`, one commit each, and installed into the MO2 mod for an in-game check. `main` and GitHub are unchanged:
replacing `main` waits for the user's word after the in-game check.

## Where things are
- Branch `vs-1.1.0-plus-fork` (worktree `F:\Claude Brain\VS-110plus`), on top of d3477de. Not pushed.
- Backup of the old fork main: local branch `backup/fork-main-2026-11-18` = 46e575e.
- Upstream 1.1.0, read only: `F:\Claude Brain\VS-upstream` (detached at d3477de).
- Build: `F:\VS-build\VegasShaders.dll` (release build: untracked marker `src\core\ReleaseBuild.h` in the worktree, as upstream's
  packages; it leaves out BloodFlow, dust motes, capsule shadows and the test tools). Detailed logs `F:\VS-build\build-110plus*.log`.
- Install: `F:\VS-scratch\install-110plus-2026-11-18\` (manifest, backup, `install.ps1`, `restore.ps1`). Restore:
  `powershell -ExecutionPolicy Bypass -File "F:\VS-scratch\install-110plus-2026-11-18\restore.ps1"`.

## The branch
| Commit | What |
|---|---|
| 69826bc, 332b1f1 | Oblivion-era settings and code removed (old text menu keys, OcclusionCulling/IKFoot/... reads, AddCastShadowFlag + TorchesCastShadows, persuasion tests). Old saved keys skipped quietly (`Retired()`). No value changes, no migration. |
| 02f494e, efc6f8d | A game shader that fails to compile leaves `<binary>.failed.hlsl` (the exact preprocessed text) in the cache, from every compile route; a binary removes it. |
| 3905346, 379f0f4, e4ef8e9 | RT path tracer: its 11-20 screen-sized shared images and scratch store are made only when it is on. Switched on mid-game, it asks the new `RtCore::SetupAgain` once: the helper stops at the next safe point and starts again with its images. |
| 4f822a0 | Unused `puddles.dds` and `snow.dds` deleted (5.9 MB). |

## Choices per fork-only item
- RT texture table ("atlas full") fix: upstream's kept (idle-longest slot reuse + `RealTextureKeepFrames` release + the ported VRAM
  budget). The fork's COM-count sweep was measured only against the old code, not against upstream's design, and would redo 31 conflict
  hunks in `RtCore.cpp`.
- CAS sharpening (owner decision 2026-11-18: "include CAS"): CAS is the only filter again, Strength 0.5, upstream's `CAS` switch folded in
  (one switch). Sharpening stays off by default. Settings step 90 moves a saved Strength 0.75 to 0.5 (unless the saved `CAS` was true) and
  removes the `CAS` key. Maths identical to upstream's CAS path; the fork's `.spv` rebuilds byte for byte.
- `GpuProfile` test key: dropped (a bench aid only).
- The other four: ported (table above).

## Proven (with evidence)
- Build: `MSBuild ... /p:Configuration=Release /p:Platform=Win32` exit 0, no warnings in the log. Compiler
  `F:\Tools\VS2022BT\VC\Tools\MSVC\14.29.30133\bin\HostX86\x86\CL.exe` (cl 19.29.30159, toolset v142 as the project asks:
  `NewVegasReloaded.vcxproj:23,29`), Windows SDK 10.0.19041.0 (`WindowsTargetPlatformVersion 10.0` = latest installed), `/MACHINE:X86`,
  PE machine 0x14c, `/O2 /D NDEBUG`. Release marker in effect: the DLL holds the release-only "not part of this release" log line.
- SPIR-V: the Vulkan SDK 1.3.296.0 glslangValidator (glslang 15.0.0) and spirv-opt (SPIRV-Tools v2024.4) with upstream's flags
  (`tools/nvr-dxvk/compile-compute.sh`: vulkan1.1, vulkan1.2 for ray query and `Rt/*.comp`, spirv-opt -O for variants) rebuild
  `Sharpening.spv`, `Rt/RtSunShadow.spv` and `VolumetricFogApply-m0l0c0p0.spv` byte for byte. No `.spv` is touched by the branch.
- Runtime shader compilers: effects use d3dcompiler_47 first with a D3DX fallback (`EffectRecord.cpp:125-139`, `NewEffectCompiler = true`);
  game shaders use D3DX only (`ShaderRecord.cpp` D3DXCompileShader). `effect_compile.exe` is D3DX only, so it matches the fallback path,
  not the default one. The branch changes no `.fx` or `.hlsl`, so no offline shader compile was needed.
- Defaults: upstream 1.1.0's own shipped `VegasShaders.defaults.toml` fails `defaults_tool.py check` (58 lines differ from its fragments:
  order of some RT/AO keys, two comments). The branch edits the fragment and the shipped file identically: the mismatch is the same 58
  lines before and after.
- Two `lead-architect` reviews of the branch diff: approve (findings fixed in 332b1f1, efc6f8d, 379f0f4, e4ef8e9).
- Install: 102 files into `mods\VegasShaders-1.1.0` (84 replaced and backed up, 18 new), every copy byte-compared.

## What the install changed besides the branch
The installed mod was upstream's **1.1.0 preview** (6d340b5, 2026-11-16), not the final 1.1.0. Its shaders differed from d3477de in 53
files and missed 11, so the install brought every shader file up to the branch (= final 1.1.0 + fork items), plus the DLL, PDB and
defaults. Kept as they were: the RT helper `NvrRt\NvrRtHelper.exe` (2026-11-11 build, protocol 9 = what 1.1.0 speaks; the final 1.1.0
helper only adds a free-memory cap on start-up compile workers), the old `GunFX.dll` (1.1.0 switches it off itself), and the unused
`puddles.dds` / `snow.dds` (nothing loads them).

## Believed (not proven)
- Cinema and DepthOfField look the same as before (persuasion was never true in New Vegas).
- Switching path tracing on mid-game blanks the other RT effects for the few seconds the helper takes to start again, then path tracing
  shows. Not run in game.
- The final 1.1.0 renderer changes (deferred renderer, migrations 88/89) behave on this PC as upstream describes. Upstream's notes say
  RT and PT were not checked on an RTX card for 1.0.9 or 1.1.0.

## Not run
- Anything in game. First start recompiles the changed shaders (minutes on a slow PC).

## Open
- In-game check (see the report): deferred renderer, RT on the RTX card, TAA + FSR at 67 % render scale, contact shadows now off by default,
  CAS sharpening (switch Sharpening on in the menu: near surfaces crisper, no halos on strong edges, far land and sky untouched).
- Settings version 90 is this branch's own: if upstream's next release also uses 90 for another step, a player who ran this build skips
  it. Renumber ours above upstream's on the next upstream merge.
- User's word: replace `main` (and GitHub) with this branch, or keep the fork as it is.
- Upstream code points to `docs/next/*.md` from 209 places, but neither repo has `docs/next/`.
- Upstream's fragments vs shipped defaults mismatch (58 lines): fix the fragments or ask upstream.
