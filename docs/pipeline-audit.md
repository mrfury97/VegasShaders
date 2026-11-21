# Pipeline audit: what overlaps, what can go, what can be combined (2026-11-21)

Owner's request (via Main brain): "check the whole rendering pipeline and see what is overlapping, what can be dropped, and what can be
combined", then "be unreasonably ambitious". Read-only study: nothing was built, installed or run for it.

- **Code read:** branch `test-p1a-bump` at e687e58, the code of install-test19/20. That is main plus the open branches deferred-phase1a,
  deferred-wetness-ground (Rain wetness), unified reflections, bumped normals, the menu clean-up and the PipboyFit/SMP fixes.
- **Owner's settings:** from the owner's last log, `VegasShaders_2026-11-21_12-41-27.log`.
- **Frame times:** the only measured numbers come from one dry F10 profile (`VegasShaders_2026-11-20_21-17-53.log`). No rain frame and no
  interior frame has been profiled. Every other number below is arithmetic (passes x pixels x taps) or a code comment, labelled so.
- **Labels:** PROVEN means the code path was read end to end, by a helper and, where marked "checked", by me. BELIEVED means inferred.
  NOT MEASURED means no timing exists.
- **Paths:** deferred (G-buffer and the deferred passes), forward (templates), post (screen pass, the same on both paths), Vulkan-only,
  D3D9-only.
- **Owner's word:** anything below that changes what players see, or changes frame cost noticeably, needs the owner's word before it is
  built.

## The short answer
1. **There is less waste than the number of effects suggests.** Many merges already exist:
   - post-tonemap fusions: Coloring + LUT, and Cinema + SkyDither;
   - the fog composite that applies shadow, AO and interior darkening;
   - AmbientLight taking over AO and BounceLight;
   - bounce into specular;
   - SMAA standing aside for TAA;
   - identity skips for exposure, ImageAdjust and LUT;
   - idle parking of off effects' targets.

   At default settings the post-tonemap chain is only TAA, the distant depth of field and one fused Cinema pass.
2. **The real per-frame waste is in the foundations,** for every player on every frame:
   - depth and normals are built twice when the upscaler is on, which is the default;
   - the normals buffer is written twice;
   - linear depth is made twice under the deferred renderer.

   Together this is about 0.3-0.6 ms at 1440p (NOT MEASURED, arithmetic), with no look change.
3. **The biggest tangle is wetness and reflections:**
   - 7 switches for one look;
   - 4 rain clocks;
   - 4 puddle fields;
   - 2 screen traces on wet ground when Unified reflections is off;
   - WetWorld still counted in rain after its pass stopped drawing.

   Most of it is already on the way out: Rain wetness step 4, Unified reflections, and the `retire-wetworld` session.
4. **One bug found on the way (PROVEN, checked):** in the owner's settings, wet objects (roads, slabs, walls) get no lamp or fire streaks
   at night (item Q1).
5. **The big wins are structural,** and they are in the "Big options" section, ranked:
   - one lighting resolve on the Vulkan path;
   - RT sun replacing the cascade maps inside its range;
   - one shadow / ambient / reflection quality ladder each;
   - a scratch-target pool with per-pass resolution.

## Ranked list, best value first
| Rank | Item | Kind | Saving | Players see | Effort | Needs owner's word |
|---|---|---|---|---|---|---|
| 1 | Q1 lamp streaks missing on wet objects (bug) | fix | - | streaks back | 0.5 | yes (look) |
| 2 | B2 depth and normals once (Q3 + M2 + M3) | combine | 0.3-0.6 ms every frame, everyone (NOT MEASURED) | no | 2-3 | yes (frame cost) |
| 3 | Quick-win batch Q4-Q7, Q9-Q12 | drop | small; load time; memory | no (Q12: two options shown) | 1 | Q12 only |
| 4 | Q2 WetWorld gate (with `retire-wetworld`) | drop | 0.1-0.5 ms in rain (NOT MEASURED) | no | 0.3 | no |
| 5 | M1 interior fold on by default | combine | 0.24-0.33 ms indoors (measured, comment) | practically no | 0.5 | yes |
| 6 | B1 finish one wet + reflection system (M5, M6, M8, M9) | combine/drop | one trace + one pass in rain; ~15 switches become 2 | one consistent wet look | 4-6 (in flight) | yes (step 4) |
| 7 | B3 RT sun skips near cascades; contact into DeferredSun; shadow ladder | combine | possibly ms for RT users (NOT MEASURED) | lighter ambient in contact shadows | 5-7 | yes |
| 8 | M4 / B6 exposure into the tonemap, one HDR finish pass | combine | 0.1-0.4 ms (NOT MEASURED) | no | 2-4 | yes |
| 9 | B4 one Vulkan lighting resolve | combine | 0.5-1.5 ms (NOT MEASURED; profile first) | no | 8-12 | yes |
| 10 | B5 scratch pool, lazy targets, resolution ladder | combine | 50-70 MB at 1080p; a new perf knob | no / new option | 3-5 | yes (new option) |
| 11 | B8 Phase 2 deferred switches made default (owner already runs four) | move to deferred | unknown | lamps light more | 0.5 each | yes |
| 12 | B7 ambient and lamp-shadow ladders | combine | menu | simpler menu | 2-3 each | yes |
| 13 | M7 cube cadence, M12 lazy records, M13 fog cascades, M14 lamp class stencil | combine | small each | no | 0.5-2 each | per item |
| 14 | B9 cut BloomLegacy, Specular, FXAA, BounceLight | drop | 0 at defaults | players who use them | 0.3-0.5 each | yes |

## Part 1: quick wins
Each is no look change unless stated, and costs 0.5 session or less. All could go in one build, one test run.

| # | What, where | Path | Evidence | Saving | Risk | Players see | Effort |
|---|---|---|---|---|---|---|---|
| Q1 | **Bug: no lamp streaks on wet objects in the owner's settings.** The objects' (and the far land's) lamp film is drawn only inside the Wetness Effects reflection screen pass. `WetnessReflectionsEffect::ShouldRender` returns false when `[Shaders.WetnessEffects.Reflections] Enabled` is false (`WetnessReflections.cpp:43`), and the owner has it false (log line 19). Land streaks are unaffected (DeferredLamps `DL_LAND_FILM_ONLY`). Fix: run the pass for the film alone when Reflections is off, or move the object film into DeferredLamps' class passes. | deferred | PROVEN, checked (code + log) | none (adds work) | low | yes: streaks come back on wet roads and walls | 0.5 (film-only run) |
| Q2 | **WetWorld still counted in rain after its pass stopped drawing.** `effectsBetween` (`EffectChain.cpp:635`) and `bounceIntoSpecular` (`:838`) test `wouldRender(WetWorld)`. `WetWorld::ShouldRender` (`WetWorld.cpp:133`) ignores Wetness Effects, while the draw is skipped separately (`EffectChain.cpp:946`). So in rain with Wetness Effects on, the fog-composite fold (two full-res passes, per its comment) and the bounce + specular merge are lost. The same applies to `FadedContactUnread` (`ShadowsExterior.cpp:789`). Fix: one "WetWorld draws" test. Not in the `retire-wetworld` diff; give it to that session. | post | PROVEN, checked | 0.1-0.5 ms in rain (NOT MEASURED, from the dry 0.06-0.25 ms passes) | low | no | 0.3 |
| Q3 | **Normals redrawn at full size after the upscale.** `TAAUpscale.cpp:753` and `:801` call `RenderDepthAndNormals` again at display size every upscaled frame (RenderScale 0.67 + FSR is the default). After that point only Snow, Debug and DLSS RR read the normals, and all are off by default. Keep the depth; skip the normals unless one of those is on. | post | PROVEN for the second run (checked); BELIEVED for "no other reader" (grep, not every C++ reader traced) | 0.1-0.3 ms at 1440p, every frame (NOT MEASURED) | a missed reader gets stale normals | no | 0.5 |
| Q4 | **PointShadows2 compiled and bound but never drawn.** It is drawn only when `!mergedPointShadows` (`EffectChain.cpp:447`); the shipped PointShadows.fx has `MergedPointShadows`. Drop the enrolment (`ShaderManager.cpp:126`) or load it lazily. | both | PROVEN (checked) | load time only | custom fx without the merged technique | no | 0.3 |
| Q5 | **FXAA does not stand aside for TAA or SMAA** (`EffectChain.cpp:1463`, no gate), unlike SMAA (`:1450`). | post | PROVEN | ~0.15 ms when a player turns it on with TAA; 0 at defaults | low | no (double AA only) | 0.2 |
| Q6 | **WetnessReflections works out wetness for every pixel, then overwrites it where the film exists** (`WetnessReflections.fx.hlsl:173` vs `:180-206`). Fix: `[branch]` on the film flag. | deferred | PROVEN (order); BELIEVED (fxc does not sink it) | 0.1-0.4 ms in rain with that pass on (NOT MEASURED) | low | no | 0.3 |
| Q7 | **Deferred wetness: sky reflection computed, then multiplied by `DWSkyKeep` = 0 on floors** (`DeferredWetness.hlsl:206`, `:320-321`). Fix: branch first. | deferred | PROVEN | tiny | none | no | 0.2 |
| Q8 | **WetWorld's unused keys:** `PuddleCoeff_R/G/B` and `PuddleSpecularMultiplier` feed `TESR_WetWorldCoeffs`, which no shader reads (`WetWorld.fx.hlsl:20`; no Vulkan reader). Goes with WetWorld's retirement. | both | PROVEN | none | none | no | with retire-wetworld |
| Q10 | **Dead keys and files out:** the 12 keys read by nothing and the orphan `ObjectRefractionTemplate.hlsl` (see "Dead settings"). Not MotionBlur's per-view Enabled: wiring that is a behaviour change. | both | PROVEN | none | none (a key missing from the defaults is simply not read) | no | 0.5 |
| Q11 | **AmbientOcclusion's targets parked while AmbientLight holds it off** (the default), and the same for VolumetricClouds and the other off effects without `TargetParkable`. | both | PROVEN no override; BELIEVED sizes | memory only (AO half + quarter G16R16F pairs; clouds) | low | no | 0.5-1 |
| Q12 | **Menu hides two live keys:** `PlayerShadowFirstPerson` / `ThirdPerson` are on the dead list (`ImGuiManager.cpp:803-804`) but are read (`ShadowManager.cpp:1684`). | menu | PROVEN | none | none | yes: two working options become visible again | 0.1 |
| Q9 | **Stale docs:** CombineDepth README section 7 says DepthNormals.frag cannot compile for lack of `viewNormalFromTaps`, but `Normals.glsl:54` now defines it. DepthOfField README section 9 item 6 says the distant technique's source copy is open, but `EffectChain.cpp:1406` already skips it. | docs | PROVEN | none | none | no | 0.1 |

## Part 2: medium items
Each is 1-2 sessions and worth doing on its own. Ranked by value: saving x how often it runs / risk.

| # | What, where | Path | Evidence | Saving | Risk | Players see | Effort | Default |
|---|---|---|---|---|---|---|---|---|
| M1 | **Interior fold on by default.** `InteriorShadowFold` (`[Main.Main.Performance]`, off, defaults :427) folds the KeepDarkening apply into the fog composite (`CompositePlan`, `EffectChain.cpp:647`). | deferred (indoors) | PROVEN code; saving measured in two interiors (code comment) | 0.24-0.33 ms indoors (comment, measured) | about 50 pixels a frame differ by a few 16-bit steps at ceiling gaps | practically no | 0.5 (a look, then owner's word) | off |
| M2 | **Normals written once.** The Normals pass writes every pixel, then `CapturedNormals::MergeIntoNormals` (`CapturedNormals.h:651-700`, called `ShaderManager.cpp:2325`) rewrites most pixels from COLOR3, rebuilding its own facet normal from ddx/ddy. One technique that reads COLOR3 and falls back to the depth facet would do both. | both | PROVEN (two helpers independently) | ~0.1 ms at 1440p, every frame (NOT MEASURED) | edge pixels may flip accept/reject (2x2 derivative vs better-of-two-neighbours); Vulkan `Normals.frag` twin | maybe at silhouettes (A/B picture needed) | 1 | on |
| M3 | **Linear depth made twice under the deferred renderer.** `DeferredDepth` linearizes into `TESR_DeferredDepthLinear` (R32F, `DeferredDepth.h:213`); then `ResolveDepthBuffer` + CombineDepth write another R32F (`/far`, plus the view-model merge). CombineDepth could start from the deferred one for the world part. | deferred | PROVEN both exist; BELIEVED same world values (not checked whether anything draws depth between the span end and the chain) | 0.05-0.15 ms + one R32F (~15 MB at 1440p) (NOT MEASURED) | view-model and sky edges; units differ | no | 1-2 | on (mode 2) |
| M4 | **Exposure folded into the tonemap.** Exposure is a full-res HDR pass that multiplies by one scalar from the 1x1 AvgLuma texel (`EffectChain.cpp:1213-1221`). The tonemapper already takes an exposure scalar (`Tonemapping.hlsl:538`). Bloom's first downsample and LocalExposure would apply the scalar on their taps. | post | PROVEN | 0.1-0.2 ms at 1440p by day and indoors (NOT MEASURED) | Exposure Mode 1 (GT7) adaptation; the tonemapper's sampler budget; Lens and pre-tonemap LUT read the exposed frame | no if exact | 2 | on |
| M5 | **Wetness and reflection switches made one.** DeferredWetness, UnifiedReflections, DeferredBumpedNormals, DeferredReflections, DeferredWetLampFilm, MaterialAware and MaterialBufferRoughness all serve one wet-reflection look. The owner runs them all on. `FdIndirect.h:37` keeps BumpedNormalsOn separate because Wetness Effects and WetWorld read it. Map them to Rain wetness + one Reflections quality level, with a migration in `UpdateOldDefaults`. | deferred | PROVEN | none (menu) | saved toml values | no | 1 (part of Rain wetness step 4 / Unified reflections step 4) | mixed |
| M6 | **Four rain clocks:** RainState + the Rain animator (read by Reflections' rain guess `Reflections.cpp:749`, GearWeather `GearWeather.h:143`, HeatHaze, Skin, Terrain/PBR "Rain" groups); Wetness Effects' weather percent (~20 s ease, `WetnessEffects.cpp:83-120`); Puddles' own game-hours fill (`Puddles.cpp:75-86`); CoverMap (off). The ground dries on one clock, skin and gear on another. | both | PROVEN | none | timing changes | yes, small: wet skin, gear and ground in step | 1-2 (in progress: `retire-wetworld` moves the clock to Wetness Effects) | on |
| M7 | **Reflection cube redrawn every frame:** 48 draws (6 faces + 42 filter), `Reflections.cpp:1249-1298`, blending only 2 % a frame (`CubeCapture 0.02`). Update every 2-4 frames at a higher blend. | both | PROVEN | 0.05-0.15 ms (NOT MEASURED; whole Reflections step 0.50 ms p50 dry, measured) | lag when turning fast | maybe | 0.5 | Reflections off by default; owner on |
| M8 | **Two screen traces on wet ground when Unified reflections is off.** WetnessReflections ignores Reflections (`WetnessReflections.cpp:42-52`). With Unified on, `reflKeep` returns early but the pixel shader still runs for every wet pixel. With Reflections on, the wet pass should keep only its film. | deferred | PROVEN gating | 0.4-1 ms at 1440p in rain (design-doc estimate, NOT MEASURED) | wet walls' sheen | maybe | 0.5 | not default |
| M9 | **Deferred wetness fallback + Wetness screen pass:** two full-screen passes back to back, each with its own frame copy (`DeferredWetness.h:266-377`, copy at `:294`; then `EffectChain.cpp:934`). | deferred | PROVEN passes; BELIEVED merge feasible (the march reads the lit frame the fallback changes) | 0.1-0.3 ms in rain (NOT MEASURED) | medium-high | no | 1.5 | not default |
| M10 | **Dry-frame restore for the land height trick:** a material copy every frame, dry ones too, plus a 0.028 ms draw (`DeferredWetness.h:137-154`; the copy is outside the timer). | deferred | PROVEN; draw measured | ~0.03 ms + one copy | records would have to write 20 directly when dry | no | 0.5 | not default |
| M11 | **Cinema + SkyDither fusion drops out when ImageAdjust runs** (`EffectChain.cpp:1293`), giving three passes. Fold ImageAdjust into the fused technique. | post | PROVEN | two LDR full-res passes (~0.1-0.2 ms) for tuned installs only | order of ops; Vulkan twin | no | 1 | identity by default |
| M12 | **Shader records on first use.** In mode 2 each lit object shader loads up to five records: forward Default, forward Interior, lighting-free and two glass forward+ records (`ShaderManager.cpp:1129-1196`, `:1348`; `DeferredTransparents.h:89-117`). Load the forward+ and Interior ones lazily. Related open item: failed forward compiles are retried every start. | both | PROVEN record count; BELIEVED usage share | start-up compile and load time, driver memory (NOT MEASURED) | a first-use hitch | maybe a stutter | 1-2 | on |
| M13 | **Fog samples the four sun cascades again** at the surface point DeferredSun already resolved (`VolumetricFog.fx.hlsl:630-660`). The froxel path may avoid it (not checked). | post | PROVEN for the classic path | NOT MEASURED | DeferredSun would export visibility; fog also covers forward pixels | no | 1-2 | on |
| M14 | **Deferred lamp class passes:** the main, land, skin and land-film lamp passes are separate full-screen draws, each clipping per pixel by class (`DeferredLamps.hlsl:58-71`, `:359-372`). `DeferredTiledLight` (off by default, ON for the owner) limits them to lamp tiles; a stencil mark per class would avoid re-reading the G-buffer. | deferred | PROVEN | NOT MEASURED | stencil shared with the first-person mark (`DeferredLamps.h:110`) | no | 1-2 (DeferredTiledLight default: 0.5 + owner's word) | on |

Low value, recorded so nobody re-checks:
- **Water's planar reflection pass** (`ForceReflections`, default true, `Render.cpp:5238-5306`): 0.33 ms GPU + 0.24-0.29 ms CPU in the dry
  profile (measured). It also fills the water's depth maps; skipping it once gave streaky water. High risk; keep unless RT holds the water
  (`WaterSkipPlanar`, ~1.7 ms in RT scenes per its toml text, untested).
- **VisibleRanking** (on): up to 128 small draws and occlusion queries a frame; the froxel lists carry no occlusion, so they are no
  substitute. Weak.
- **The facet normal is rebuilt from depth in five shaders** (AO sky term, `Reflections.glsl:340`, `RtAOInput.frag`, `RtSunShadow.comp`,
  Normals). Deliberate (`docs/deferred-vs-forward.md:149-150`): a shared include is code tidiness only, no time.
- **Normals smoothing in two places** (Normals `SmoothNormals`, default 0, vs AmbientLight's own smooth): never compared (Normals README
  open question 4). BELIEVED overlap.
- **Private fp16 copies per deferred pass** (DeferredLamps, Flashlight, DeferredFog, DeferredSkin, DeferredWetness, LinearScene): mostly
  never allocated under linear light. LinearScene's "a few conversions a frame" were not counted.

## Part 3: big options (the ambitious list)
Ranked by value: how much frame time or complexity it removes for how many players, against risk. Every one changes frame cost and
most change the look a little, so each needs the owner's word before it is built.

### B1. Finish the one wet + reflection system (already in flight)
- **What:** at the end there is one Rain wetness switch (DeferredWetness default on) and one Reflections quality level (Unified
  reflections). WetWorld, the Wetness Effects screen trace, three of the four puddle fields, three of the four rain clocks, Reflections'
  GLSL copy of the puddle noise and the old switches all go: M5, M6, M8, M9, Q2, Q6, Q8, plus the lighting-free records' wetness code.
- **Evidence:**
  - PROVEN that every piece listed exists and runs; the reflections/wetness helper and this session checked them.
  - The owner already runs the end state's switches: the design docs `deferred-wetness-design.md` and `unified-reflections-design.md`,
    plus the 2026-11-21 log.
- **Saving:** NOT MEASURED. It removes one screen trace (0.4-1 ms, design estimate) and one full-screen pass + copy in rain. Its biggest
  gain is complexity: about 15 switches become 2.
- **Risk:** medium. D3D9 Reflections has no film for every case. The RT reflection passes are unaware of the film. Partly damp pixels
  still march.
- **Players see:** one consistent wet look. Rain wetness on by default is a visible change: owner's word, already planned as step 4.
- **Effort:** 4-6 sessions left (Rain wetness step 4 + Unified reflections steps 3-5).

### B2. Depth and normals once (every player, every frame)
- **What:** one "foundations" pass family, built once per frame. Q3 (no normals redraw after the upscale), M2 (normals written once),
  M3 (one linear depth) and a shared facet-normal include.
- **Evidence:** PROVEN per item (see Q3, M2, M3).
- **Saving:** about 0.3-0.6 ms at 1440p on every frame for everyone (NOT MEASURED, arithmetic). The best saving-per-session in this audit.
- **Risk:** low-medium. Silhouette edges in the normals; the view-model depth merge.
- **Players see:** nothing (an A/B picture to prove it).
- **Effort:** 2-3 sessions.

### B3. RT sun shadows replace the cascades inside their range, and one shadow quality ladder
- **What:** with RT sun shadows on (Mode 1), the cascade shadow maps are still drawn in full. There is no RT test anywhere in
  `ShadowManager.cpp` (checked: no `RtSunShadows::` reference); Mode 1 only switches off the fused forward-contact technique. Skip the
  near cascades the RT range covers, and keep the far ones for the fade.
- **Contact shadows into DeferredSun:** today the exterior screen-shadow chain multiplies the whole lit pixel, ambient and lamps
  included. It then adds the lamp term back (`Shadow.r += Shadow.g`), which is why the PointShadows lamp mask runs outdoors
  (`EffectChain.cpp:465-511`, `715-742`). Contact shadows on the sun term only would drop the lamp compensation and the apply.
- **Ladder:** one Shadows quality setting: cascades -> + contact -> + RT sun (cascades skipped near) -> path tracing. It replaces
  RT.Enabled, Mode, ForwardLookup, HalfResolution, ScreenSpace.*, Bend and DeferredRtSunFirst.
- **Evidence:** PROVEN that the cascades are drawn under RT (frame-graph helper; checked) and for the contact chain (lighting helper).
- **Saving:**
  - RT users: the near cascades' scene draws, the biggest single CPU + GPU item in shadows. It could be milliseconds in dense scenes
    (NOT MEASURED; the cascade draw cost was never profiled).
  - Everyone: the outdoor lamp-compensation mask + apply (NOT MEASURED).
- **Risk:**
  - Cascade fade seam at the RT range edge.
  - RT's frame of delay with `ForwardLookup`.
  - Contact shadows stop darkening ambient: a visible look change.
  - WetWorld, Underwater and Debug read the shadow buffer.
- **Players see:**
  - RT: nothing if the seam is clean.
  - Contact: slightly lighter ambient inside contact shadows. That is deferred-first and more correct, but visible.
- **Effort:**
  - 2-3 sessions for the cascade skip;
  - 2-3 for contact into DeferredSun;
  - 1 for the ladder.

### B4. One lighting resolve for everything that reads the G-buffer (Vulkan path)
- **Superseded 2026-11-21 by the workshop, `docs/deferred-single-pass.md`:** the scope is narrowed to sun and lamps (wetness, AO,
  ambient and reflections stay outside); the pass is a fragment Draw compiled from the same HLSL; 12-17 sessions in switchable steps;
  the value is removing the ps_3_0 limits more than frame time.
- **What:** today a deferred pixel is read by many separate full-screen passes:
  - DeferredSun;
  - up to four DeferredLamps class passes;
  - DeferredWetness;
  - the shadow-apply / AO / ambient composite (AmbientLight apply 0.25 ms, measured dry);
  - the fog composite (0.06 ms, measured);
  - the Reflections composite;
  - the skin pass.

  Each re-reads 3-4 fp16 targets.
- **The proposal:** one tiled compute resolve on the Vulkan path. It reads the G-buffer once per pixel, then does:
  - the sun with shadow;
  - the froxel lamps, already in a table (`ManyLamps.h:132`);
  - ambient and AO;
  - the reflection term from Unified reflections;
  - the wet film;
  - and writes the lit pixel once.

  The ps_3_0 limits that forced the split (32 temps, X4505) do not exist in compute. D3D9 keeps today's passes as the fallback.
- **Evidence:**
  - PROVEN that the passes and their G-buffer reads exist (lighting helper, items 8-9).
  - BELIEVED that bandwidth dominates their cost.
- **Saving:** NOT MEASURED. Each full-res G-buffer read + write at 1440p is roughly 0.1-0.25 ms (dry profile scale), so merging 5-7
  passes could save about 0.5-1.5 ms. A rain + interior profile must come first.
- **Risk:** high.
  - Two lighting implementations to keep equal (Vulkan resolve vs D3D9 passes).
  - The DXVK fork interface.
  - Ordering with fog and reflections.
  - Native D3D9 players never get it.
- **Players see:** nothing if matched, and the way to lamp-heavy scenes without the 4-lamps-per-target shadow batching.
- **Effort:** 8-12 sessions. The largest item here; only worth starting after B2 and a profile.

### B5. A frame graph light: scratch pool, lazy targets, per-pass resolution
- **What:**
  - **Scratch pool.** About 185 bytes per screen pixel is held in screen-sized targets: ~385 MB at 1080p, ~680 MB at 1440p
    (BELIEVED, hand sum). Nothing aliases. Half-res fp16 scratch targets with disjoint lifetimes could share a pool of 3-4 textures,
    replacing about 14 allocations:
    - GodRays Buffer0/1 (step 37);
    - Fog Buffer0/1 (34);
    - VolumetricLight March/Temp (35);
    - Reflections Trace/Temp (32);
    - DustMotes (35b).

    `FrameChain::Pair` is the precedent. Histories cannot alias.
  - **Lazy targets.** Allocate off effects' targets on first use: LocalExposure and VolumetricClouds are off by default but always
    allocated (not parkable).
  - **Resolution ladder.** One "effects resolution" setting replacing the hard-coded `MaxQuality::Reduced(w, 2/4)` divisors. There is
    no per-pass scale setting today, and none for reflections, fog or volumetric light.
- **Evidence:** PROVEN for "no aliasing" and "no per-pass scale"; the memory figures are BELIEVED.
- **Saving:**
  - Memory: about 50-70 MB at 1080p from the pool alone.
  - Frame time: only through the resolution ladder, which is a player knob, not a free saving.
- **Risk:** medium. A lifetime mistake shows as garbage in one effect; half-res needs an edge-aware upsample.
- **Players see:** the pool, nothing. The ladder is a new option.
- **Effort:** 3-5 sessions.

### B6. One HDR "finish" pass
- **What:** Exposure (M4), bloom composite, tonemap, pre-tonemap LUT, Coloring, Cinema, ImageAdjust and SkyDither as one or two passes.
- **Evidence:** PROVEN that the post chain is already lean at defaults (TAA, distant DoF, one fused Cinema pass). The extra wins are
  Exposure and the ImageAdjust case.
- **Saving:** 0.2-0.4 ms (NOT MEASURED).
- **Risk:** medium. Order of operations; constant and sampler budget in the tonemapper.
- **Players see:** nothing.
- **Effort:** 3-4 sessions. Modest value: most of the post merging is already done.

### B7. Ambient and lamp-shadow quality ladders
- **Ambient:** SSAO -> GTAO -> AmbientLight -> RT AO/GI.
  - It replaces AO.Enabled, GTAO, Skylighting, RT.Enabled, SunGI, RoomGI, BounceLight.Enabled, AOLowRes and NvrDxvkAmbientLight.
  - The takeover already makes the passes exclusive (`EffectChain.cpp:599-608`).
  - One gap: with AmbientLight off, BounceLight and RT GI can both run (BELIEVED, no guard found).
- **Lamp shadows:** capsules -> cube maps -> RT. It replaces Enabled, Live, SkipTracedCubes, ActorsFromCubeMaps, ManyLights,
  ShadowDenoiser and DenoiseLamps.
- **Saving:** none in itself; menu clarity, and the double-run gap closed.
- **Risk:** saved settings need a migration.
- **Players see:** a simpler menu.
- **Effort:** 2-3 sessions each.

### B8. Move more forward groups to deferred (road map Phases 2-4)
- **Phase 2 (cheap):** the owner already runs DeferredFirstPerson, DeferredTiledLight, DeferredRtSunFirst and DeferredSkinBlur on.
  Making the ones that look right the default is 0.5 session each plus a look and the owner's word. This is the best value inside B8.
- **Phases 3-4:**
  - water forward+;
  - fading objects;
  - late grass;
  - deferred decals;
  - distant trees;
  - folding EnvTemplate's cube layer into Unified reflections;
  - DeferredSun's missing terms.

  10-14 sessions. Mostly correctness (every lamp lights them); the frame-time effect is unknown until Phase 0 measures the forward
  share.
- **Evidence:** `docs/deferred-roadmap.md`, `docs/deferred-vs-forward.md`; owner's log for the switches.
- **Risk:** per group, low-medium.
- **Players see:** lamps lighting what they don't today.

### B9. Cut whole effects whose job another effect now does
Each removal is a product decision: players who turned it on lose it. Each costs 0 at defaults today and only adds menu and code weight.

| Effect | Replaced by | Evidence | Effort |
|---|---|---|---|
| WetWorld | Rain wetness | in progress (`retire-wetworld`) | - |
| PointShadows2 | PointShadows' merged technique | PROVEN never drawn (Q4) | 0.3 |
| BloomLegacy | Bloom | README: "an older, alternative bloom"; off | 0.5 |
| Specular | DeferredSun's GGX highlight + Unified reflections' sky | PROVEN it re-lights the sun highlight from depth-rebuilt normals; off; ~0.4 ms when on (its comment) | 0.5 |
| FXAA | TAA / SMAA | off; no gate (Q5) | 0.3 |
| MaterialCapture / ReflMaterial alpha path | the G-buffer COLOR2 under Unified reflections | PROVEN; Unified already skips its copy (`Reflections.cpp:788`); the `_rmaos` roughness overrides must be in the G-buffer first (not checked) | 2 (inside B1) |
| BounceLight | AmbientLight (already takes it over) | PROVEN takeover; off | 0.5 |
| Screen SkinScattering | DeferredSkin's own blur | gated for deferred skin (`SkinScattering.cpp:285`); still needed for forward skin and mode 0 | keep until mode 0 is retired |

## What I would start first
1. **A rain and an interior F10 profile** (owner or room 06, 1 run). Every rain and indoor saving above is NOT MEASURED; this decides B4.
2. **Quick wins Q1, Q3-Q7 and Q9-Q12 in one build:** 1-1.5 sessions, no look change except Q1's streaks coming back and Q12's two
   options showing. Q2 and Q8 go to the `retire-wetworld` session instead.
3. **B2, depth and normals once:** 2-3 sessions. The only item that saves time on every frame for every player with no look change.
4. **B1, keep going** (it is in flight), and fold M5 into its step 4.
5. Then, with the profile in hand: **B3's cascade skip** if the cascades cost what I expect, **B4** if the G-buffer passes add up to
   over 1 ms.

## Already merged or fused (don't re-propose)
- FusedPassPlan: Coloring + LUT, Cinema + SkyDither (`EffectChain.cpp:1287`).
- The fog composite applies sun shadow, AO and interior darkening; plus pairApply and shadowIntoAmbientLight (CompositePlan).
- AmbientLight takes over AO and BounceLight (`EffectChain.cpp:599-608`).
- Bounce combine folded into Specular's (SpecularBouncePlan).
- SMAA yields to TAA. ImageAdjust, LUT and Exposure skip themselves at identity.
- The distant DoF technique skips the source copy.
- Idle target parking (`TargetReleaseSeconds`).
- FrameChain removes the per-effect RenderedBuffer copies; the game's HDR target is used as the chain texture.

These were checked and found not to overlap:
- one lamp table and froxel list a frame, shared by forward+, glass and DeferredLamps;
- one reflection cube writer;
- one shading-normals copy shared by WetnessReflections and WetWorld;
- the sky tables are gated;
- AvgLuma vs bloom mips, and DoF focus (shares AvgLuma);
- Flashlight vs the lamp table (exclusive);
- deferred pixels sample the sun shadow once (DeferredSun only; the screen buffer holds contact and lamp terms);
- RT AO replaces the SSAO estimate (no double);
- RT exterior lamp shadows replace the cubes outdoors.

## Dead settings, shaders and targets (mechanical sweep)
**Method.**
- All 3019 shipped keys were parsed from `resource/VegasShaders.defaults.toml`. Each one was matched as a quoted string in the C++ (menu
  files and the migration block excluded), together with its section name.
- Anything that failed was read by hand.
- `Shaders.*.Status Enabled` keys are read generically (`GetMenuShaderEnabled`, `SettingManager.cpp:1915`). GunFX keys are read
  through the `GunFXKeys.h` table.
- Raw lists are in this session's scratchpad (`audit\dead3.txt`, `hidden_internal.txt`, `hidden_devonly.txt`, `lists.txt`,
  `inittex.txt`), not kept.

**Read by nothing (PROVEN; 14 keys). Drop them from the defaults:**
- `Shaders.Blood.Status Enabled`: no Blood effect exists; the toml says "Not currently used".
- `Shaders.Tonemapping.Main/.Interiors/.Night PointLightMultiplier`: "disabled for now", `Tonemapping.cpp:33`.
- `Shaders.Tonemapping.Interiors SkyMultiplier`: `Sky.cpp:603` reads only Main and Night.
- `Shaders.ShadowsExteriors.Main OrthoMapResolution, OrthoRadius`: already in the menu's dead list.
- `Shaders.ShadowsExteriors.ShadowMaps Mipmaps, Anisotropy`: the field they would set is never assigned.
- `Shaders.Sky.Clouds SphericalNormals`: the read is commented out (`Sky.cpp:613`); `SKYTEX.pso.hlsl:34` hard-codes 1.
- `Shaders.Snow.Main Flakes`.
- `Shaders.WaterLens.Main Time`: `WaterLens.cpp:23-27` reads the other four keys only.
- `Shaders.MotionBlur.FirstPersonView/ThirdPersonView Enabled`: `ReadBlurView` (`MotionBlur.cpp:52-58`, checked) reads four keys and
  not Enabled. So the shipped `ThirdPersonView Enabled = false` does nothing; motion blur still runs in third person.
  - That last one is a small behaviour bug, not just a dead key. Wiring it up changes what players see: owner's word.

**Read, but the shader never uses the value (PROVEN):**
- AO `Samples` (Main, Interiors);
- GodRays `LightShaftPasses`;
- Sharpening `Clamp` (Main, Interiors);
- WetWorld `PuddleCoeff_R/G/B`, `PuddleSpecularMultiplier` (Q8);
- ShadowsInteriors `UseCastShadowFlag`.

Unsure: ShadowsInteriors `Quality` (sent as a constant, toml says unused; shader not checked).

**Stale menu "dead" list:** `PlayerShadowFirstPerson` / `PlayerShadowThirdPerson` are marked dead (`ImGuiManager.cpp:803-804`) but
ARE read (`ShadowManager.cpp:1684`). That is a menu bug: the keys are hidden while they still work.

**Hidden, not dead:**
- 433 keys on the Internal list and 189 on the DevOnly list;
- the Rain wetness hides (12 WetWorld + 5 Wetness Effects keys);
- the Unified reflections hides (3);
- the Reflections HiZ* and DampTrace keys.

No key on the `Retired()` list is still in the defaults.

**Shader files:**
- `Shaders/ObjectRefractionTemplate.hlsl` is an orphan: no C++, shader, tool or doc names it or its shader names (PROVEN, checked).
- Includes no shader includes:
  - `Compute/ObjectMotion.glsl`;
  - `FSR31/.../ffx_fsr3upscaler_debug_view.h`;
  - `Rt/ffx/ffx_denoiser_shadows_prepare.h`;
  - the AMD original `*_callbacks_glsl.h` files kept beside our `_nvr.h` replacements (BELIEVED kept on purpose).
- `Compute/Sm6/` holds two hash generations of the VolumetricFog variants (`b85a1398`, `b95a152b`); one set is BELIEVED stale.
- `ReflectionsTraceHiZCount.spv` is a debug build still loaded by a counter path (`Reflections.cpp:1420`).
- 139 .spv and 42 game-named .pso/.vso files have no literal name in C++. That is expected: they are variants, helper-loaded or
  picked by the game.

**Effects:**
- Every effect folder is enrolled and stepped.
- Flashlight's .fx is loaded and never drawn (the flashlight is deferred now; its comment says so). Kept for the menu switch; it
  could become a settings-only record.
- Off effects cost one early return (`EffectRecord.cpp:471`).
- BELIEVED: every effect's .fx is compiled at start whether on or off (no switch gate in LoadEffect). That costs load time.
- Debug and self-test steps do nothing when off. The exception is one `GetSettingI` a frame in the G-buffer view
  (`CapturedNormals.h:1880`), and only while it is live.
- Stale comment: `EffectChain.cpp:107` says TAA is off by default; it is on.

**Render targets allocated while their effect is off (not parkable; BELIEVED, sizes not logged):**
- **AmbientOcclusion's buffers.** AO is forced off whenever AmbientLight is on, which is the default, so these are held for nothing
  by default.
- **VolumetricClouds** (off by default).
- **LocalExposure, Dream, Bodycam, SunGlare, VolumetricSmoke, HudRaindrops.**

Making them `TargetParkable` is the cheap half of B5: 0.5-1 session, memory only, no look change. No truly dead target was found (the
two 1x1 query targets and the cloud probe are read on purpose).

**Duplicates by purpose:**
- Bloom + BloomLegacy, and TAA + FXAA, can all run together with no warning. The menu warns only about SMAA + FXAA.
- Snow and Precipitations are exclusive by weather.
- Specular and PBR are different layers.
- PointShadows2 is PointShadows' continuation pass (never drawn with the shipped fx, Q4).
- ShadowsExteriors and SunShadows are complementary.

## Not checked / open
- GPU cost of any pass in rain or indoors; the cascade shadow-map draw cost; the LinearScene conversion count.
- Native D3D9 (non-DXVK) behaviour of each item; DeferredRenderer 0 behaviour item by item.
- The RT helper's internals beyond the reflection exchange (`RideMainExchange`, off, ~0.7-1.1 ms per its toml text from a cloud bench).
- Whether the froxel fog path also re-samples the cascades (M13).
