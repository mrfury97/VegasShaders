---
name: vs-bench-check
description: "Workflow: test a Vegas Shaders build in the unattended bench at F:\\NVR-bench, a general slot A, D or E (hidden copies of the game made from MO2; make another with make_slot.py create auto), and judge the pictures. Use after a build needs an in-game picture check and the owner is away or has said to run it."
---

# Testing a VS build on bench slot A

## Task
Run a TEST build of Vegas Shaders in bench slot A (hidden game copy, no save needed), take pictures of fixed scenes and compare them
with an earlier run. Source of every fact below: `F:\NVR-bench\MO2-SLOTS.md` (read only for this skill; read it before the first run).

## Prerequisites
- **Permission:** slot runs are unattended and start only while the owner is away (away mode: 10+ min without input, never beside
  their own game, killed on any input), or when they say so. Never in the real game folder or through MO2 without their word.
- A TEST build (no `src\core\ReleaseBuild.h`, so the auto-test link is in): skill `vs-build-deploy`. The restore point's own DLL has no
  link. Make a tree under `F:\NVR-bench\work\` (each with `TREE-INFO.txt`; examples `VS-installed`, `VS-main`) holding the build and
  its shader files.
- Slot A = `E:\_NVR-slots\slotA` (mo2 profile, restore point `E:\_NVR-snapshots\snap-mo2-20261120-2257`). Needed slot mods are already
  in `mo2-slim.json` (VS refuses to start without `LOD Flicker Fix - NVSE`, `Vanilla Plus Terrain`, `Shader Loader - NVSE`).
- The slot's exe copy is renamed (mutex, window class); the real exe is never written. Slot B and C belong to the Stalker ports.
- **Slots D and E are built like A (2026-11-26)**: `SLOT=auto` takes A, D, E (whichever is free); name one only when you must. All busy
  10+ min: `make_slot.py create auto --tag <LOCKTAG> --why "..."` makes another. A new DLL that needs a slot of its own:
  `make_slot.py create auto --tag <LOCKTAG> --pin "room 02: <why>"`, run with `SLOT=<X>`, then `make_slot.py remove <X>`. No asking.

## Steps (Git Bash)
1. Pick a scene (`v-saloon`, `v-strip-night`, ... in `work/bench/foundations/scenes/`) and a shot list (`.../lists/`). Start a tour
   at `v-strip-night` (`v-goodsprings-overcast` fails as the first scene); keep a tour under 480 s (the plugin stops the list then).
2. Run:
   ```
   cd /f/NVR-bench/work && LOCKTAG=<unique> SLOT=A SLOT_NO_RETRY=1 SNAPSHOT=snap-mo2-20261120-2257 FORK_D3D9=F:/NVR-bench/fork-d3d9 \
     RUN_TOML=F:/NVR-bench/tomls/owner-mo2-20261120.toml sh bench/deep-roadmap/multirun/run_slot.sh <scene> <list> <label> <tree>
   ```
   (`SLOT=auto` instead of `SLOT=A` to take any free general slot.) Away mode instead: the same variables with
   `away_runner.py jobs/<file> --slots A --parallel 1 --any-profile`. The shader store `F:\NVR-bench\shader-cache` is per tree since
   2026-11-26: a tree's next run reuses its own compiled shaders even after other trees ran (al-int: 650 s before, 181 s after). Only
   shaders whose source changed recompile; the NoCloud/PuddlesLean fallbacks still cost ~55 s a start until the plugin remembers failures.
   List `wait` lines count frames: use 100-300 for pictures, 1500 only for HEARTBEAT averages. `MO2-SLOTS.md` "Where a slot run's minutes go".
3. Last lines: `WALL: <s> s` and `RESULT: <run folder> <pictures folder>`; pictures in `F:\NVR-bench\shots\<run>`.
4. Compare: `work/bench/autotest/tools/abcmp.py <runA> <runB>` (mean luma and mean |diff| per shot); `compare_runs.py` / `flipcmp.py`
   for FLIP. Judge the pictures by eye as if the owner had sent them; title and caption each panel.
5. Stop only processes you started, by id. Never leave the slot unreset (the run script resets and checks links).

## Validation
- `VERDICT PASS` in `launcher.log`; the player placed at the spot (x/y off about 0); pictures exist for every shot; `VegasShaders-Logs`
  in the slot has no shader errors.
- Forced wetness for reflection pictures: `set Shaders.WetnessEffects.Debug WetnessOverride/PuddleOverride/RainOverride` (FalloutNV.esm has
  no rain weather). Do not take pictures with `landflicker` active (its magenta mask frame stays in the Reflections history).

## Troubleshooting
- Exit 31: spot check failed (an NPC moved the player, or the first scene was a bad one): hide the reference in the scene or start
  with another scene. Exit 32: shot list over 480 s. Exit 30: the coc never arrived.
- A start-up box stopped it: read `DIALOG-BOX.txt` in the run folder (usually a missing slot mod).
- Slot game runs about 24 fps at the owner's settings: frame-time numbers from it are not release numbers; and frame-time runs go alone.
- `NVRAutoTest.log` records window titles of whatever is in the foreground: scrub before sharing (2026-11-21 logs were scrubbed).

## Links
- `F:\NVR-bench\MO2-SLOTS.md` (full record, proven / believed / not run), `F:\NVR-bench\CLAUDE.md`, `F:\NVR-bench\SETUP.md`.
- The owner-level skill `nvr-bench-run` covers the whole bench (slots B and C too). Build: skill `vs-build-deploy`. Reflection pictures: `docs/DESIGN.md "Reflection speckle"`.
