# Reference

Technical reference, one section per topic: the key files and how the plugin is built (Architecture), menu input under DXVK, the runtime file and
folder names, facts about the G-buffer and DXVK limits, and lessons kept from the archived room notes. Each fact says proven (with the evidence) or
believed. Design docs and studies are in [DESIGN.md](DESIGN.md); per-effect notes are in [EFFECTS.md](EFFECTS.md).

## Contents

- [Architecture](#architecture)
- [DXVK input](#dxvk-input)
- [Runtime paths](#runtime-paths)
- [G-buffer and DXVK facts](#g-buffer-and-dxvk-facts)
- [Lessons from archived notes](#lessons-from-archived-notes)

## Architecture

*Original title: Vegas Shaders architecture*

> Technical reference. Moved from the `vs-architecture` skill on 2026-11-25 (content unchanged apart from links).

### The plugin
- Vegas Shaders (formerly New Vegas Reloaded) is a 32-bit xNVSE plugin for Fallout: New Vegas: `VegasShaders.dll`, built from
  `NewVegasReloaded\NewVegasReloaded.vcxproj` (Release, Win32, MSVC toolset v142). Build: the `vs-build-deploy` skill.
- Shaders under `src\hlsl\NewVegas\` are compiled at runtime, not built into the DLL: a shader change ships as files under
  `<game>\Data\Shaders\VegasShaders\`. The shader cache reuses an entry only when the whole preprocessed text matches, so changed
  shaders recompile by themselves and editing a shared include recompiles every shader that uses it (minutes on a slow PC).
  Do not tell the owner to delete the cache after an install; clear it only if a shader looks stale.
- Ray-traced effects run in a separate 64-bit helper through the project's DXVK fork, because 32-bit Vulkan has no ray tracing.

### Key files
- `src/core/ShaderManager.cpp`: main render loop; calls `ImGuiManager::NewFrame()` and `Render()`.
- `src/core/EffectChain.cpp`: frame order of the effects (see the `vs-effects-modular` skill).
- `src/core/ImGuiManager.cpp`: ImGui settings overlay (DX9 + Win32 backend); input under DXVK: [DXVK input](#dxvk-input).
- `src/core/SettingManager.cpp`: TOML-backed settings; the ImGui menu is auto-populated from it.
- `src/core/Paths.h/.cpp`: every runtime file and folder name (see [Runtime paths](#runtime-paths)).
- `src/core/SkinBones.h`: every reader of skin `BoneObjects` goes through it (bones matched by pointer; NV-SMP-Physics safe).
- `lib/imgui`: git submodule, docking branch (`git submodule update --init --recursive` after a fresh clone).
- xNVSE headers: `src/NewVegas/nvse/`.

### Docs
- Docs map: the repo `CLAUDE.md`. Current design docs live in `docs\`, technical reference in `docs
eference\`, history in
  `docs\archive\` (see its `MANIFEST.md`). When a doc goes stale, rewrite it or move it to the archive; do not leave it in place.
- Deferred vs forward background: [Deferred vs forward](DESIGN.md#deferred-vs-forward).

## DXVK input

*Original title: DXVK input, cursor and input blocking*

> Technical reference. Moved from the `vs-dxvk-input` skill on 2026-11-25 (content unchanged). Read it before touching `ImGuiManager` input,
> the toggle key, input blocking, or any hook on input, UI or game objects.

### DXVK input: critical finding
FNV is commonly run under **DXVK** (Vulkan translation layer). DXVK suppresses `WM_KEYDOWN` and `WM_MOUSEWHEEL` before they reach
any subclassed WndProc. **Do not rely on WM messages for keyboard or scroll wheel input.**

`WM_INPUT` (Raw Input) is also unreliable: DXVK's message pump may not dispatch `WM_INPUT` to the subclassed WndProc at all.
**Do not use WM_INPUT for any keyboard input.**

#### What works instead
| Input | Solution |
|---|---|
| Keyboard (all: text, nav, characters) | `GetAsyncKeyState` polled for VK 1-255 in `NewFrame`, edge-detect against `s_prevKeyState`, inject via `io.AddKeyEvent` + `ToUnicode` -> `io.AddInputCharacterUTF16` |
| Scroll wheel | Read `DIMOUSESTATE2::lZ` from the DI mouse buffer *before* zeroing it in `HookedGetDeviceState`, accumulate in `s_pendingWheelDelta`, inject via `io.AddMouseWheelEvent` in `NewFrame` |
| Mouse buttons | `GetAsyncKeyState(VK_LBUTTON/RBUTTON/MBUTTON)` -> `io.AddMouseButtonEvent` each frame |
| Toggle key open/close | `GetAsyncKeyState(DikToVk(KeyEnable))` polled in `NewFrame` every render frame. `DikToVk` has a hardcoded table for extended keys that `MapVirtualKey` gets wrong (END, HOME, arrows, etc.) |

**Rule**: everything goes through `GetAsyncKeyState` polled on the render thread. No WM messages, no cross-thread queues.

#### DIK -> VK conversion
`MapVirtualKey(dik, MAPVK_VSC_TO_VK_EX)` **fails silently** for E0-prefixed extended keys (END=0xCF, HOME=0xC7, arrow keys,
Insert, Delete, etc.): it returns 0 or the wrong VK. Use the hardcoded `DikToVk()` table in `ImGuiManager.cpp` instead.
Non-extended keys fall through to `MapVirtualKey`, which handles them correctly.

#### ImGui keyboard navigation
`ImGuiConfigFlags_NavEnableKeyboard` is intentionally **not set**. With it enabled, END/HOME/arrow keys leak into ImGui widget
navigation (jumping to the last item, etc.) even while the overlay is trying to close. Text field editing is unaffected: it uses
a separate code path that does not need nav.

#### What does NOT work under DXVK
- `ImGui_ImplWin32_WndProcHandler` for keyboard/scroll (WM messages never arrive)
- `WM_KEYDOWN` + `ToUnicode` in WndProc
- Any `WM_MOUSEWHEEL` handler in WndProc
- `WM_INPUT` for keyboard input (may never be dispatched to WndProc under DXVK)
- `MapVirtualKey` for extended DIK scancodes (returns 0 or wrong VK)

`WM_ACTIVATE` and other non-input system messages still arrive normally.

### Mouse cursor position: click-to-refocus quirk
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

### Input blocking while the overlay is open
- **Mouse movement**: vtable hook on `IDirectInputDevice8::GetDeviceState` (slot 9), zeros the `DIMOUSESTATE2` buffer. Mouse
  device only: the keyboard device has a separate vtable under DXVK.
- **Keyboard actions**: `DIHookControl::SetKeyDisableState` via xNVSE for codes 0-265 (`BlockGameInput`).
- **Mouse buttons / wheel macros**: the same `DIHookControl` call covers codes 256+.

### xNVSE first
Always check xNVSE (`src/NewVegas/nvse/`) before writing custom hooks for anything that interacts directly with the game (input,
UI, game objects). Prefer xNVSE APIs over vtable patches where available.

## Runtime paths

*Original title: Runtime names (2026-11-02 rename)*

> Technical reference. Moved from the `vs-runtime-paths` skill on 2026-11-25 (content unchanged). Read it before touching any code that reads
> or writes a runtime file or folder, or when a tester's install has old names.

`src/core/Paths.h/.cpp` owns every runtime name.

| New | Old (still read when the new one is missing) |
|---|---|
| `Data\NVSE\Plugins\VegasShaders.dll` (+ `.pdb`) | `NewVegasReloaded.dll` (an old copy left beside the new one is logged as an error: two plugins) |
| `VegasShaders.toml` (user settings; always written under this name) | `NewVegasReloaded.dll.toml` (read only; never changed or deleted) |
| `VegasShaders.defaults.toml`, `.maxquality.toml`, `.maxquality.user.toml`, `.materials.txt`, `.imgui.ini` | `NewVegasReloaded.dll.<same>` / `NewVegasReloaded.imgui.ini` |
| `Data\Shaders\VegasShaders\` (Shaders, Effects, Compute, caches) | `Data\Shaders\NewVegasReloaded\` |
| `Data\Textures\VegasShaders\` (LUTs) | `Data\Textures\NewVegasReloaded\` |
| `Data\VegasShaders\` (Presets, Variants, Keywords, Skins, EnabledVariants.ini) | `Data\NVR\` |
| `VegasShaders-Logs\VegasShaders_<date>.log` (game folder) | none: logs are only written under the new names |

- Folders are chosen once at startup (new if it exists, else old if it exists, else new) and logged in one `PATHS folders:` line.
- Kept unchanged on purpose: the xNVSE plugin name `NewVegasReloaded` (Main.cpp; mods check it), the console prefix `NVR`, every
  settings section/key, `TESR_*` names, preset formats, log line tags, and the legacy `NewVegasReloaded.dll.config` (only read).
- New code that touches a runtime file or folder goes through `Paths::` (or `VS_LOGS_FOLDER` for the logs), never a literal path.

## G-buffer and DXVK facts

*Original title: G-buffer and DXVK limits (facts from the room's work, 2026-11-18 to 2026-11-22)*

> Technical reference. Moved from the `vs-gbuffer-dxvk-facts` skill on 2026-11-25; proven/believed labels kept. Read it before adding push
> constants or samplers to a DXVK-fork pass, growing a land or lighting-free shader, or reading COLOR2 / albedo for grass or land.

Each fact says proven (with the evidence) or believed. Layout of the G-buffer itself: [Deferred vs forward](DESIGN.md#deferred-vs-forward); wetness
design: [Deferred wetness design](DESIGN.md#deferred-wetness-design); reasons behind the X4505 work: [Deferred roadmap](DESIGN.md#deferred-roadmap).

### DXVK push blocks: two different limits (checked against this repo 2026-11-25)
- **Proven in this repo (`src/core/NvrDxvkExt.h:55`):** the NVR extension's own passes (compute and full-screen graphics passes made through
  the extension) declare `NVR_EXT_MAX_PUSH_BYTES 240`; their push block starts at offset 0 (`NvrDxvkExt.h:27`, `:187-190`) and the
  host copies the data over the whole block. Examples in code: the RtDenoise blocks of 200 / 216 / 48 bytes (`RtDenoise.cpp:167`).
  The agent `lead-architect` checks against this 240.
- **Proven from the repo's own patch text (`tools/nvr-dxvk/patches/0004-NVR-extension-no-pipeline-library-for-a-stage-whose-.patch`,
  commit message and comment):** in DXVK's independent (graphics pipeline library) layout the fragment stage gets a fixed 48-byte slot at
  offset 192 of the context's push data; the extension's full-screen passes declare up to 240 bytes, so a stage whose block is larger
  than its slot is never fast-linked: it always gets a complete pipeline. That is why 240 is safe for the extension's own passes and
  why a replaced D3D9 game shader's fork-side fragment block must stay within 48 bytes (from offset 192; 192 + 48 = 240 of the 256).
- **Not re-read from DXVK source:** the claim of one shared 256-byte `constantData` block for all D3D9 stages (below) comes from the
  2026-11-22 debugging; the DXVK source is not in this repo (`tools/nvr-dxvk` holds only patches), so the 256 total is believed. The
  repo agrees on 192 and 48.
- Summary: extension passes (complete pipeline): up to 240 bytes from offset 0. Replaced D3D9 pixel shaders in the pipeline-library
  layout: 48 bytes from offset 192. Open: nothing about the numbers; only the 256 total is unchecked here.

#### DXVK push block: 256 bytes, fragment block at 192 (D3D9 draws)
- **Proven (2026-11-22, branch `deferred-single-pass`, tip 5d4e0b1).** DXVK keeps one 256-byte `constantData` push block for all
  stages; the fragment stage's part starts at byte 192. The fork's 96-byte FRAGMENT push block ran past the end, and DXVK's uint16
  sampler heap indices overwrote the low halves of `uvmap` / `ztest`, so G-buffer reads landed up to 7x3 pixels off.
- Fix that works: move non-sampler data (uvmap, ztest) into the UBO and keep the push block to compacted sampler dwords, at most
  48 bytes (layout format 2). Same-frame parity probe (`tests/parity_probe.py`, value 14): mean and max |A-B| 0 at DebugView 5.
- **Believed, open:** VolumetricFogApply / VolumetricFogEstimate push 112 bytes with samplers at 0/4/8 and likely corrupt the low bits
  of `compositeAOTexel` the same way.
- Rule: before adding to a fragment push block, add up its bytes from offset 192; anything past 256 is silently clobbered.

### ps_3_0 temp-register wall on land (X4505)
- **Proven (2026-11-18 game log and offline compiles).** With Terrain Blending, the 5-7-layer land shaders fail to compile with X4505
  (out of temp registers) and fall back to the game's shader: that is the "pale ground" seen in game, not a prototype bug.
- **Proven (offline, 2026-11-18):** forward land records use temp r31 (all 32) at 3,842-4,600 instruction slots; the lighting-free
  (deferred) records peak at r29-r30 with 2,738-3,178 slots, before Terrain Blending adds its part. So a lighting-free record alone
  may not rescue the Terrain Blending case; shortening the shader (DeferredStandalone rescued 12 shaders in the 2026-11-18 log; puddle streaks out) or moving work
  to a deferred pass makes the room.
- Removing the puddle wall streaks saved about 175 instruction slots on the 6-layer land, with the temp peak unchanged: slots and
  temps are separate limits; check both (`template_compile`, see the `vs-build-deploy` skill).
- ps_3_0 limits (32 temps, instruction slots, no dynamic indexing) do not exist in compute or SPIR-V replacements; the D3D9 path keeps
  the split passes as the fallback.

### Grass writes no G-buffer record
- **Proven (fix 40acc53, deferred wetness; RT GI review).** Grass draws no lighting-free record, so it writes neither the material
  (COLOR2) nor the albedo buffer for its pixels: whatever the ground wrote stays under the blades. The deferred wetness film made
  grass mirror-like until 40acc53 fixed it. The RT GI review hit the same fact: a sharper blur exposed grass grain, because grass
  never writes the albedo buffer.
- Use: "no albedo written" is how to find grass pixels in a deferred pass; never treat COLOR2 / albedo under grass as the grass's own.

### Land carries its height in COLOR2.r, only between record and pass A
- **Proven (deferred wetness step 1, `TerrainTemplate.hlsl` near line 896, design doc "Height").** Every fraction of COLOR2.r is
  already read (metal as `/ 0.49` by DeferredSun, DeferredLamps, the flashlight, the path tracer, the DLSS guides; the far land's
  1/64 mark by DeferredSun), so there is no spare channel. The near land's lighting-free record writes r = 4 + 0.99 x height (the
  specular bit 16 taken out, so fp16 keeps 1/256 steps; at 20 it would be 1/64), and DeferredWetness pass A writes r = 20 (= 4 + 16)
  back for every such pixel, wet or dry (a restore-only record in dry frames, and a fallback restore on every DeferredSun return that
  skipped A).
- Consequence: any pass that reads COLOR2.r between the land records and pass A sees height, not metal. Ruled out: a separate
  water-layer height channel in the G-buffer.

## Lessons from archived notes

*Original title: Lessons kept from the archived room-02 memory notes*

Written 2026-11-25 when the old per-feature memory notes were archived to `F:\Claude Brain\archive\2026-11-25.zip\vs\memory\` (see its
`MANIFEST.md`). Only lessons that are still true and not already in the code docs are kept here. Each line names its source note and is
labelled **proven** (the note names the evidence or a fix that worked), **believed**, or **owner decision**. Paths in the old notes
(`VegasShaders-104`, `VEGAS_SHADERS.md`, `nvr-bench`, `RESUME_*.md`) no longer exist; the repo is `F:\Claude Brain\VS`.

### Shaders and registers (vs-puddles, vs-bend-forward-contact, vs-rain-system)
- **Proven.** No new compile define is possible: at most 62 per shader. Puddles use `[Shaders.Puddles.Status] Enabled` at startup, which makes
  `ShaderRecord` send `WETNESS_EFFECTS=2` (3 = map mode); `Includes/Wetness.hlsl` then defines `PUDDLES_COMPILED`; the D3D9
  `WetnessReflections.fx` defines `PUDDLES_RUNTIME`. (WetWorld is retired since 2026-11-21; the rule about the define cap stands.)
- **Proven.** A register that is free in every constant table can still be overwritten by RAW per-draw `SetPixelShaderConstantF` writes (c192+,
  c188/c189 failed once): grep `src` before picking registers. Check registers with the owner's toml (`--toml`), not the repo defaults
  (c218 was HandBlood's in the owner's toml).
- **Proven.** A per-draw bool register (b13) is only usable as a plain `if` (X4509 otherwise).
- **Proven.** After an upstream merge re-check every register and the `tests/template_defines.py` mirrors (upstream's HUD raindrops took
  s12-s14 and c153-c156 in object shaders once). `template_defines.py` does not model WETNESS_EFFECTS: force it, or register clashes stay hidden.
- **Proven.** Feature-off byte identity: guard struct fields and conditions with the preprocessor; an unused field or a `false &&` inside a
  `[branch]` changed codegen.
- **Proven.** The game compiles with D3DX9: judge "fits" with the landc tool, not `template_compile`; failed records leave `<name>.failed.hlsl`.
  `template_compile.exe` does not create output folders: mkdir first, or comparisons silently use missing files.
- **Proven.** The D3DX 9.43 compiler takes minutes over float-counted dynamic loops: use int counters with a fixed bound plus `[branch] break`.
- **Proven.** `far` is a Windows macro: not a C++ identifier. `= {}` does not zero `D3DXVECTOR4` / `D3DXMATRIX` members (empty user
  constructor): constant structs held stack garbage (RT GI review R21).
- **Proven.** The game binds the 7th land normal map to s13 itself (the puddle map sits on s15 there).
- **Proven.** Cache rule for fallback copies: they count only while newer than the sources and are stamped when used
  (`FallbackNewerThanSources` / `TouchCacheCopy`).
- **Proven.** Puddle CPU side: the level integrates over `GameDaysPassed*24` (Calendar::Update 0x867A40); rain follows Sky::GetIsRaining (0x59E950:
  rainy flag 4, begin/end precip bytes 6/7 = Game.h padE6[0..1]); held indoors.
- **Proven (frame cost, one launch).** In the old rain system the three DebugView early-outs in `RainSystem()` made the D3D9/DXVK path about 5x
  faster (1.65 to 0.32 ms); the cause is unknown ("believed": keep them if the rain system is ever restored).
- **Proven.** Upstream's `ORDER.txt` can be stale after a merge: rebuild it from the merged defaults order.

### RT and the DXVK fork (vs-bend-forward-contact, vs-rt-gi-review, vs-rt-sun-shadows-room, nvr-consider-vulkan-options)
- **Proven.** Every RT shared image counts against the helper protocol limit: past it the RT core refuses to start and ALL RT effects go off
  (all RT effects together once did this; the path tracer now skips its 20 images when ReferencePT / PTRealtime is off at core start, so turning
  PT on needs a restart).
- **Proven.** Never kill the RT helper by image name (`taskkill /IM NvrRtHelper.exe` kills the owner's helper too); stop only the process you started.
- **Proven.** The no-limits replacement set is matched by ps_3_0 bytecode hash: after any settings or shader change that recompiles shaders,
  rebuild it with `rt_sun_set.py` (it existed only on the old combo-106 build; it is NOT in this repo's `tools/nvr-dxvk/replace/`, which has `nolimits.py`, `psasm2glsl.py`, `sm4asm2glsl.py`), or those shaders silently fall back to the cascades. (The set was 67 replacements for
  the forward RT sun in 2026-11-14; whether main still ships it: **open**, upstream 1.1.0 keeps `NvrDxvkReplacements` off.)
- **Proven.** The forward RT sun's traced visibility rides in the blue channel of Bend's `TESR_ContactForward` (rgba16f); the 15-degree-quantised
  sun direction (F2) caused "dark streaks on a sunlit slope with a backlit sun".
- **Rule from the owner (2026-10-29/30).** When a feature touches the DXVK fork, also say what the extension (SPIR-V replacement, compute, storage
  images, tessellation) could add; SPIR-V replacement is the real fix for ps_3_0 limits (X4505, instruction slots, no dynamic indexing), with the
  ps_3_0 version kept as the D3D9 fallback. Hair plan of 2026-10-29 (not done in VS): Scheuermann in ps_3_0 first, then SPIR-V, then a reactive mask for FSR/TAA.
- Bend (Community Shaders screen-space shadows, applied "forward" since 2026-11-06): `Compute/BendContact.comp` runs at effect-chain step 11 and
  writes `TESR_ContactForward`; `core/BendContact.h` builds `TESR_ContactReproject` next frame; Shadow.hlsl `SunContactForward` multiplies it into
  `GetSunShadow` when the depth matches. Under upstream 1.1.0's deferred mode Bend has no lookup. Debug: `BendDebug 3` dumps to
  `<game>\VegasShaders-Logs\BendContact.raw`.

### Benchmark and test traps (vs-rt-gi-review, vs-puddles, offline-renderer)
- **Proven.** Bench lists that freeze time (`sgtm 0.001`) never test wind-driven noise or ripple motion. The Zone save rolls a different weather per
  load: pin it (`fw`) and stop the day clock (`set timescale to 0`). The game's idle camera starts after about 2,500 frames: keep shot lists shorter
  (`setgs fVanityModeAutoDelay 99999` against it).
- **Proven.** A changed template means a cold shader cache, so the first run may time out (first-frame limit: the bench's `--first-frame-s`).
- **Proven.** Rain in a slim slot = an Honest Hearts weather (`fw 02010C78` then); Zone weather ids depend on the slot's load index. FalloutNV.esm has no rain weather.
- **Proven.** A sharper blur exposed grass grain the old loose blur hid: grass never writes the albedo buffer (see [G-buffer and DXVK facts](#g-buffer-and-dxvk-facts)).
- **Proven.** The path tracer cannot rank GI variants and its sky is its own tunable; SunGI forces RadianceCache on (test without the cache with
  CacheStrength 0).
- **Proven.** A pixel diff of shadows was unusable (TAA / view shift); compare by eye. Measure first: a measure-only census settled that Faster Objects was not worth porting.
- **Proven.** Check `.fx` files with the landc `fxc9.exe`: `shcheck` does not see `.fx` changes.

### Owner decisions that still hold (do not reverse unasked)
- **Owner decision** (vs-puddles, 2026-11-11/12): walls under an overhang stay DRY (40-unit cover look-out); first-person arms never get puddles;
  terrain only: `ObjectPuddles=false`, actors always dry. The owner tunes `[Shaders.Puddles.Main]` in game: never overwrite those values or flip the Puddles /
  WetnessEffects Status or Cavity unasked. Wall streaks were REMOVED on 2026-11-18 (keys retired).
- **Owner decision** (vs-sun-gi): the toml keeps `SunGI true`, GIStrength 5 (20 looks right in the Zone, 5 in the Mojave; a per-worldspace or per-weather
  strength was offered, not built). A path-tracer match would be about 1-1.6; do not push the owner back to it. `sungi-vpl` was never merged.
- **Owner decision** (vs-rt-gi-review, 2026-11-13): proven fixes are always on with NO key; upstream's legacy keys are only hidden through DevOnly,
  never deleted (deleting makes merge conflicts). Recommendation not yet decided: GISpatial 0.
- **Owner decision** (vs-bend-forward-contact): Bend is LOD-only and hardcoded (TerrainLOD / TerrainFade always, ObjectTemplate only for draws under
  `BGSTerrainManager::GetRootObjectLODNode`); skin, hair, grass, leaves, parallax and full land do not compile it. StartDistance / FadeLength
  sliders show only with Advanced on. A side-on-sun guard was removed at the owner's request.
- **Owner decision** (modern-lighting-room, 2026-11-13): leaf lighting on the object shader is "for another day" (Zone trees are alpha-tested NIFs,
  so Foliage translucency never reaches them): do not start it unasked. `TonemapContrast` stays 1.0 (the STALKER-reference scores worse with more).
- **Owner decision** (faster-objects-fnv-study, 2026-11-06): build nothing from Faster Objects (Hi-Z overlaps the owner's occlusion-culling plugin;
  draw merging ceiling 0.05-1.0 ms; static shadow-caster merging not worth it, VS's `SunLayers` + `SunCacheByChange` roughly halved sun-cascade CPU).
  Do not re-propose without new measurements. Five performance switches the owner tried were turned off again: `SunLayers`, `SunCacheByChange`,
  `CasterListCache`, `HiddenObjectCulling`, `InteriorShadowFold`: do not flip them again unasked.
- **Owner decision** (2026-11-18): the deferred wetness pass (Phase 1c) was chosen over Vulkan no-limits replacements as the way around ps_3_0 limits.
- **Owner decision** (vs-rain-system, 2026-11-06): the world-space rain system was dropped in the 1.0.5 rebase ("rebase but drop our RT changes and Rain shader").
  Open if the owner wants it back: the saved patch is in `F:\Claude Brain\saved-patches\2026-11-17-tree-cleanup\VegasShaders-rain\` (not re-checked 2026-11-25).

### Other projects' fixes worth knowing (balebandro-vs-fork, ranked; the clone is gone, source github.com/Balebandro/VegasShaders branch mo2-dxvk-loader)
**Believed (from a reading of its commits, based on 1.0.3, not tested here):** MSAA fixes (DeferredLamps additive and depth-tested; SkyCoverage / MsaaSkyEdge; FixBlendDepth),
a crash fix in `PipboyFit::ForPartition` after a load, ShadowMap.vso with one Bones array (skinned casters with more than 18 bones under DXVK), GPU grass with
terrain blending, sun shadow CasterCulling / SkipCoveredCasters. It removes Grass Collision (NPCs), FlashlightBeam, Flashlight MaterialLight/Interiors,
SleepingMode and per-cascade Forms sections. Authorship note: Balebandro = Alejandro Martinez, who also writes Physically-Based-Reloaded (GPL-3).
