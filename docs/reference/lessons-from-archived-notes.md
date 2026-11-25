# Lessons kept from the archived room-02 memory notes

Written 2026-11-25 when the old per-feature memory notes were archived to `F:\Claude Brain\archive\2026-11-25.zip\vs\memory\` (see its
`MANIFEST.md`). Only lessons that are still true and not already in the code docs are kept here. Each line names its source note and is
labelled **proven** (the note names the evidence or a fix that worked), **believed**, or **owner decision**. Paths in the old notes
(`VegasShaders-104`, `VEGAS_SHADERS.md`, `nvr-bench`, `RESUME_*.md`) no longer exist; the repo is `F:\Claude Brain\VS`.

## Shaders and registers (vs-puddles, vs-bend-forward-contact, vs-rain-system)
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

## RT and the DXVK fork (vs-bend-forward-contact, vs-rt-gi-review, vs-rt-sun-shadows-room, nvr-consider-vulkan-options)
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

## Benchmark and test traps (vs-rt-gi-review, vs-puddles, offline-renderer)
- **Proven.** Bench lists that freeze time (`sgtm 0.001`) never test wind-driven noise or ripple motion. The Zone save rolls a different weather per
  load: pin it (`fw`) and stop the day clock (`set timescale to 0`). The game's idle camera starts after about 2,500 frames: keep shot lists shorter
  (`setgs fVanityModeAutoDelay 99999` against it).
- **Proven.** A changed template means a cold shader cache, so the first run may time out (first-frame limit: the bench's `--first-frame-s`).
- **Proven.** Rain in a slim slot = an Honest Hearts weather (`fw 02010C78` then); Zone weather ids depend on the slot's load index. FalloutNV.esm has no rain weather.
- **Proven.** A sharper blur exposed grass grain the old loose blur hid: grass never writes the albedo buffer (see `gbuffer-dxvk-facts.md`).
- **Proven.** The path tracer cannot rank GI variants and its sky is its own tunable; SunGI forces RadianceCache on (test without the cache with
  CacheStrength 0).
- **Proven.** A pixel diff of shadows was unusable (TAA / view shift); compare by eye. Measure first: a measure-only census settled that Faster Objects was not worth porting.
- **Proven.** Check `.fx` files with the landc `fxc9.exe`: `shcheck` does not see `.fx` changes.

## Owner decisions that still hold (do not reverse unasked)
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

## Other projects' fixes worth knowing (balebandro-vs-fork, ranked; the clone is gone, source github.com/Balebandro/VegasShaders branch mo2-dxvk-loader)
**Believed (from a reading of its commits, based on 1.0.3, not tested here):** MSAA fixes (DeferredLamps additive and depth-tested; SkyCoverage / MsaaSkyEdge; FixBlendDepth),
a crash fix in `PipboyFit::ForPartition` after a load, ShadowMap.vso with one Bones array (skinned casters with more than 18 bones under DXVK), GPU grass with
terrain blending, sun shadow CasterCulling / SkipCoveredCasters. It removes Grass Collision (NPCs), FlashlightBeam, Flashlight MaterialLight/Interiors,
SleepingMode and per-cascade Forms sections. Authorship note: Balebandro = Alejandro Martinez, who also writes Physically-Based-Reloaded (GPL-3).
