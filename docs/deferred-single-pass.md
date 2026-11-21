# Deferred lighting as a single pass on the Vulkan path: workshop (2026-11-21)

Owner's request (via Main brain): "workshop on how to make the deffered vulken a single pass", following the pipeline audit's option B4
(`docs/pipeline-audit.md`).

- **Code read:** main a7baa48, read only.
- **How the views were gathered:**
  - Two helpers gathered the facts: the DXVK fork bridge, and every deferred pass in frame order.
  - Four roles then proposed and attacked designs: lead-architect, renderer-engineer, shader-engineer, performance-engineer.
  - The shader engineer converted the installed deferred shaders to SPIR-V in a scratch folder, to test the toolchain. Nothing went into the repo.
- **Source of every measured number:** the owner's profile log `VegasShaders_2026-11-20_21-17-53.log`, the only one with GPU timers.
  It was taken on an RTX 4070 Ti SUPER at 1080p, DLAA, with the fork running.
- **Labels:**
  - PROVEN: code read, compile output or log line.
  - BELIEVED: inferred.
  - NOT MEASURED: no timing exists.

## The answer in plain words
- **What "single pass" can honestly mean here:** one Vulkan full-screen draw at today's spot, the end of the G-buffer span. That one draw
  lights every deferred pixel with the sun and every lamp, including all their shadows. It is compiled from **the same HLSL source** as
  today's D3D9 passes, and those passes stay as the fallback.
- **Rejected:**
  - a compute resolve, for now;
  - a visibility buffer;
  - DXVK subpass or render-pass merging;
  - a hand-written GLSL copy of the lighting.
- **The value is not frame time on this PC.** The frame is CPU-bound. The deferred light passes are about 3 % of GPU time outdoors by day
  and about 10 % indoors.
- **The value is removing the ps_3_0 limits that split the lamp pass into up to 9 draws and drop features:**
  - lamps past a pixel's 16th lose their traced shadow;
  - a fallback ladder drops parts;
  - one shader failing to compile has sent the whole renderer back to forward on a tester's 4090.
- **Bug found by the workshop:** in a game started outdoors, deferred ground and objects get **no lamp or fire light until you first go
  indoors**. This is PROVEN in code, and the log matches. It gets fixed first (step 0).
- **Effort:** about 12-17 sessions to the full single pass, in steps that each ship alone behind a switch.
- **The first build** is step 0 plus step 1. Step 1 is the toolchain proved on the sun pass, at the same look.

## Facts the designs rest on
**The bridge** (PROVEN, `src/core/NvrDxvkExt.h`):
- The fork's `ID3D9NvrExt` 1.6 gives the plugin:
  - compute `Dispatch`;
  - full-screen fragment `Draw` with up to 4 colour targets and an ADD blend (`:97`, `:101`; used by PointShadows' AddShadowedLamps);
  - `DrawDepthStencil` (1.6, `:215-233`);
  - storage images and buffers;
  - 64 slots per type, 96 bindings per pipeline, 240 B of push constants (`:53-59`).
- There are no specialization constants: variants are separate `.spv` files.
- A pipeline compiles on first use (a stall).
- Every plugin Draw or Dispatch ends the current D3D9 render pass.
- No patch gives subpasses, input attachments, tile memory or render-pass control.

**Nothing deferred is on Vulkan yet** (PROVEN):
- DeferredSun, DeferredLamps, DeferredWetness, DeferredSkinSSS and FlashlightDeferred are D3D9 ps_3_0 `DrawPrimitiveUP` passes on every
  runtime (`DeferredSun.h:525`, `DeferredLamps.h:665-666`).
- The only deferred piece with a Vulkan twin is the linear view z (`DeferredDepth.h:85-229`).

**Hand-written GLSL twins have a poor record** (PROVEN): AmbientLight and Clouds are off by default after tester bugs that were never
reproduced (`VegasShaders.defaults.toml:416-417`).

**Frame order at the span end** (PROVEN, `Render.cpp:2913-2924`, destructor order):
1. G-buffer resolve.
2. Depth copy and linear z (0.030 + 0.013 ms).
3. Skin highlight clear.
4. Inside DeferredSun::Apply:
   - the LinearScene to-linear convert;
   - DeferredWetness A (0.04 ms; it rewrites COLOR1, COLOR2 and the film);
   - the sun draw (p50 0.20-0.27 ms outdoors).
5. DeferredLamps, up to 9 draws (interior p50 0.63 ms, including 0.34 ms of lamp shadows):
   - a frame copy;
   - 4 shadow passes writing 4 lamps each into A8R8G8B8 targets;
   - lighting;
   - land (outdoors);
   - land film;
   - skin;
   - fog resolve.
6. FlashlightDeferred.
7. Skin SSS blur.
8. LinearScene encode.

**What runs after the light passes:**
- The blended list: glass forward+, hair, particles, decals and late grass.
- Water (BELIEVED).
- First person.
- The pre-tonemap chain: contact shadows, ShadowApply, AmbientLight, SSR, fog and VL. These work on the final frame including forward
  pixels.

**Why the lamp pass is split** (PROVEN, `DeferredLamps.hlsl:25,63,77,105,203,270,309,341,428`):
- Every split is X4505, ps_3_0's 32 temp registers.
- The shader engineer reproduced the failure: the in-loop `POINT_SHADOW_FIT=0` fails ps_3_0 with X4505 at `PointShadowForward.hlsl:321`,
  and the same source passes glslang and spirv-val.

**Measured costs** (PROVEN, p50):

| Scene | Deferred span total | Share of GPU busy | Frame / GPU busy |
|---|---|---|---|
| Goodsprings day | 0.31-0.36 ms | ~3 % | 17.89 / 10.65 ms, CPU-bound |
| Strip noon | 0.27-0.32 ms | ~3.5 % | 24.91 / 8.88 ms |
| Prospector interior | ~0.95 ms | ~10 % | 15.42 / 9.19 ms |
| Gomorrah interior | 0.75-0.90 ms | ~9 % | - |
| Night exterior with lamps | NOT MEASURED | - | the lamp pass never ran outdoors: the step-0 bug |

**For scale:** point shadow cube maps cost 3.5-4.6 ms indoors, 6-8 times the whole lamp lighting pass. That, not the light resolve, is the
biggest frame-time lever in lighting.

## The candidates
| Candidate | What it is | Verdict | Why |
|---|---|---|---|
| **A** One resolve: sun + lamps + shadows + skin highlight + fog keep, Vulkan fragment Draw, same HLSL | the single pass | **Adopted**, built in steps (E then A) | fits the bridge (2 targets, ~17 of 96 bindings, constants in one uniform buffer); one source keeps parity |
| A wide (plus AO, ambient, reflections, wetness) | audit B4 as first written | Rejected in that scope | AmbientLight, SSR and the shadow apply work on the final frame including forward pixels. Wetness A rewrites the G-buffer, and one ADD blend can't also overwrite it. AO scales direct light today (BELIEVED), so folding it in changes the picture |
| A as compute (storage write) | in-place read-modify-write | Rejected for now; step 5 option | nobody has written these fp16 targets from compute; mipmapped storage views came back zero (`HiZ.h:6-8`); storage usage may switch off framebuffer compression frame-wide (BELIEVED); the hardware blend, depth test and stencil would have to be rebuilt |
| **B** Tiled / clustered lamps | one lamp dispatch over binned tiles | Not a separate step | the lists are already clustered (64-px tiles x 8 slices, CPU-built in `ManyLamps.h`), and `DeferredTiledLight` exists. The 9 draws come from X4505, not from batching. GPU binning is not needed at 24 active / 63 per list. The CPU tiling covered 100 % of tiles indoors (log), so it saved nothing there |
| C Visibility buffer | IDs, material resolve | Rejected | all 4 MRT slots are taken; ps_3_0 has no primitive ID; the game's draws can't be resolved bindless |
| D Subpasses / render-pass merging | tile memory | Rejected | DXVK uses dynamic rendering; there is no fork hook; desktop GPUs gain nothing; every plugin draw ends the render pass anyway |
| **E** SPIR-V builds of today's passes, X4505 splits removed | same passes, each one draw | **Adopted as the mechanism** for A | PROVEN feasible: all 9 installed deferred variants converted with a patched `hlsl2spv` and pass spirv-val |
| GLSL hand copy | second source | Rejected | 1,100-1,420 preprocessed lines per pass, 12 includes; AmbientLight and Clouds show the drift risk |

## The design
**Hook.**
- No new call site. Inside today's `DeferredSun::Apply` and `DeferredLamps::Apply` every check and every constant setup runs as now.
- Where they would call `DrawPrimitiveUP`, they instead:
  - read back the device's pixel constants (`GetPixelShaderConstantF(0, 224)`; the sun already reads them at `DeferredSun.h:404`);
  - read the bound textures;
  - hand both to one Vulkan Draw.
- So the C++ decisions are shared by both paths and cannot drift apart (renderer engineer).

**Constants.**
- One uniform buffer mirrors the D3D9 register file: `register(cN)` at offset N x 16, 7,168 B for the sun and lamps together.
  `UpdateBuffer` takes up to 64 KiB.
- **This is a converter fix still to make (the 7th, below).** Today `hlsl2spv.py:417-421` writes the cbuffer in declaration order with no
  offsets.
- The converter must emit `packoffset(cN)` from each register:
  - collapse the D3D9 aliases (SunFade c146, LandFilm c174, the shared sampler s10 at `Shadow.hlsl:1131`);
  - refuse b and i registers.
- An offline check proves that every reflected offset equals 16 x register, row-major matrices included.

**Bindings.**
- 14 distinct images: frame copy, 3 G-buffer targets, linear z, cloud map, cascade atlas, old atlas, character map, film, RT sun, lamp
  table, point atlas, RT lamp. VisTex goes.
- Plus 2 samplers and 1 uniform buffer: 17 of 96 bindings.
- Targets: scene and skin highlight, 2 of 4.
- Push data about 32 B (viewport, flags).

**Depth test.**
- Done in the shader: discard where the linear-z copy is nearer than the G-buffer surface, the same rule as today's GREATEREQUAL at
  oDepth (`DeferredSun.hlsl:228-240`, `DeferredLamps.h:704-706`).
- Fallback if parity fails: `DrawDepthStencil` with `FragDepth`, which nobody has tested yet.

**Target.**
- A Draw's target must not be the device's current render target (`NvrDxvkExt.h:364-366`).
- So RT0 is swapped to a scratch surface around the Draw and restored after (BELIEVED free). Put RT0 back **before** the viewport restore
  at `DeferredSun.h:544`: setting a render target resets the viewport.
- The sun writes two targets: the scene, and the skin highlight on MRT1.
  - The fork's pipelines use one blend and write all channels on every target (`DxvkCompute.cpp:186-188`); D3D9 writes only RGB on RT0
    (`DeferredSun.h:411`).
  - That is equivalent only because the sun's `main` returns alpha 0 (`DeferredSun.hlsl:398,400`). Debug views return alpha 1 (`:392`), so
    they stay on D3D9.
  - Without a skin-highlight target (`DeferredSun.h:524`), use a one-target pipeline.

**Inputs in record mode.**
- Stage 0 holds the fog copy only on exact-fog frames (`DeferredSun.h:501-512`). Otherwise it holds whatever was bound last, possibly the
  scene target. A target must not also be an input (`NvrDxvkExt.h:365`).
- So bind a 1x1 dummy for every stage the pass did not set itself, and for any stage that equals a target.

**Pixel position (a step-1 fix, not a risk).**
- `DeferredDepth.hlsl:34-45` and the passes assume D3D9's whole-number pixel centres for VPOS. Being half a pixel off is the proven
  "fd-sun-fsr" bug: black triangles on the land at RenderScale 0.67.
- glslang maps VPOS to `FragCoord`, whose centres sit at x.5. The converter must emit `FragCoord.xy - 0.5`.
- The built-in triangle's uv must also match the D3D9 quad's target-relative uv (`DeferredSun.h:519`). If it spans the viewport, remap it,
  or G-buffer reads stretch at 0.67.

**Viewport.**
- At the default FSR RenderScale 0.67 the span runs before `RenderRect::Begin` (`TAAUpscale.cpp:700-705`), so `PassesOn()` is still
  true. But the world fills only the render rect (`TAAUpscale.cpp:229-233`).
- The Draw must take the device viewport, or it lights the whole target.
- High priority: most fork players run this way (BELIEVED, part-traced).

**Fog.**
- The exact-fog path (`DL_FOG_EXACT`) folds in: sum sun and lamp light, then apply `FDAddFogged` once.
- `FDAddFogged` is not linear in the added light (a `pow` at `DeferredPixel.hlsl:122`), but it composes. Each call recovers the encoded
  value exactly, so F(F(x, D1), D2) = F(x, D1 + D2).
- That holds given the clamps at `:119-122`:
  - light added per pass is >= 0;
  - keep >= 1e-4;
  - frame >= f·F.

  BELIEVED exact under these; it removes one fp16 rounding and the separate fog-resolve pass.
- Before step 3, confirm the sun's gamma (`TESR_LinearLighting.x`, `DeferredSun.hlsl:398`) equals the lamps' (`LinearScene::Gamma` or 2.2,
  `DeferredLamps.h:587`).

**Lamp table.**
- Keep the CPU-built table: it is shared with the forward records and built on the FrameWorker thread.
- Open test: the extension reading a DYNAMIC texture locked with DISCARD.

**Shadows inline.**
- Each lamp's full filter (FIT 0) plus RT runs inside the loop.
- VisTex's 1/255 quantization goes away, a small parity point.

**Variants.**
- About 54 settings-driven defines exist.
- Key each `.spv` by a hash of the game's own preprocessed cache text (`Shaders\Cache\Deferred\...`), and build only what the shipped
  settings produce.
- A missing key falls back to D3D9, as Sm6Pass already does.
- Keep heavy optional parts as separate variants, not runtime branches: the SunFade twin's never-taken branch cost 0.91 -> 1.08 ms on a
  GTX 1070 (`DeferredSun.h:207-210`).
- Prebuild pipelines at load.

**Failure.**
- The pass uses its own latch, like `Sm6Pass::Fail` / `EffectRecord.cpp:645-650`.
- It never reaches `DxvkCompute::Fail`, which would turn off every Vulkan pass for the session (`DxvkCompute.cpp:48`). Three existing
  helpers call it:
  - use `CreateGraphicsPipeline(..., FailAll=false)` (`:195`);
  - use `CreateBufferNoFail` (`:125`);
  - reuse existing samplers, because `CreateSampler` calls `Fail` at `:155`.
- A missing `.spv` is logged on every call (`:172-173`). Cache the miss, so it is logged once.
- A failed Draw re-runs the D3D9 draws in the same frame from the captured state.

**Gate** (otherwise today's D3D9 path runs unchanged):
- the switch is on;
- `DxvkCompute::PassesOn()`;
- `!KeptOnD3D9("DeferredResolve")`;
- the variant exists;
- not the first-person re-run (`Vm.on`);
- the debug views are 0;
- `DeferredDepth::LinearReady()`.

The A/B names are separate: `NvrDxvkKeepD3D9` takes `DeferredSun` and `DeferredLamps`, so steps 1 and 2 can be compared apart.

In DeferredRenderer mode 1, DeferredDepth is not forced (`SettingForce.h:38` forces it only from mode 2), so the Vulkan path never runs
there. That is fine: mode 1 keeps D3D9.

**What stays outside** (each for a stated reason above):
- shadow-map rendering (cascades, cubes, atlas convert);
- RT traces;
- DeferredWetness A;
- the G-buffer fix-ups;
- the skin SSS blur, which needs neighbouring lit pixels;
- the chain's screen-space work;
- TAA, upscale and post.

**Paths.**
- Native D3D9, stock DXVK and DeferredRenderer = 0 render exactly as today: the new code only runs behind the fork gate, and Apply
  returns early in mode 0.
- RT timing is unchanged: the RT sun is a frame late (`TESR_RtSunForward`), and RT lamps trace earlier in the same frame.

## Toolchain status (step 1's shader half built 2026-11-21)
- **Built** (branch `deferred-single-pass`): `tools/nvr-sm6/tpl2spv.py` (template mode), `build_deferred.py`, the check
  `tests/sm6_record_check.py`; contract and `.layout` format in `tools/nvr-sm6/README.md`. glslang's HLSL front end (as the effect mode;
  dxc 1.8 `-HV 2018` also runs but would be a second back end). Windows: `py -3.10 -I`, the Vulkan SDK's tools (`hlsl2spv.tool`).
- **What template mode does** (the scratch run's six fixes, the 7th and the VPOS fix):
  - input = the cache text; the trailing NUL dropped; `main` renamed (else "Load must appear in a block"); `out DEPTH` / `out COLORn`
    -> `SV_Depth` / `SV_Targetn`; prototypes taking samplers rewritten;
  - constants by their `register(cN)` at `packoffset(cN)`: the buffer IS the register file (16 x N). Names sharing registers read the same
    bytes: same register and type -> one member plus `#define`; otherwise one float4 array over their registers with accessors (SunFade
    c146-c157: the old cascades' matrices under the wetness rows; LandFilm c174-c179: `NVR_PointShadowLight[6]` under the puddle rows).
    Unpinned constants or samplers, `b` / `i` registers and int / bool in `c` registers are refused;
  - stage sN -> set 1 binding N slot N; sampler heap indices in push dwords 0-15 (the states come from the device at runtime);
  - `VPOS` = `FragCoord.xy - 0.5`; `TEXCOORD0` = `FragCoord.xy * uvmap.xy + uvmap.zw` (push): the D3D9 quad's uv whatever the Draw's
    viewport, so the built-in triangle's uv is not read and RenderScale 0.67 cannot stretch it;
  - the depth test in the shader (push `ztest`, texture `zref`: view z or device depth, GREATEREQUAL / LESSEQUAL), or `SV_Depth` for a
    `DrawDepthStencil` attachment: the C++ chooses per Draw.
- **Variant key:** FNV-1a 64 of the cache bytes without the trailing NUL = `PortableSource(source)` in `ShaderRecord::LoadShader`
  (rule in the README). File `Compute/Deferred/<Name>-<key>.spv` + `.layout`.
- **PROVEN** (`tests/sm6_record_check.py --lamps`, PASS): 13 texts convert and pass spirv-val; spirv-cross reflection puts every member at
  16 x the register the cache text pins (matrices 16-byte stride, HLSL row_major = SPIR-V column-major), push offsets, bindings and
  outputs as the layout says; a tampered layout fails it (8 failures). In the repo: `DeferredSun` x 4 (live Sun d7b765f21cb2b456 and
  SunFade 28b60d3e24263566, 2 targets; shipped-cache Sun b147790faf0a1aa5 and SunFade 72eb3aeff1077824, 1 target). Other settings have
  no file and stay on D3D9.
- **Lamps dry run (step 2's input):** every lamp text in both caches converts and validates: DeferredLamps (Fit3/Land; Fit3 shipped),
  LampFog, LampShadows, LampSkin (2 targets), LampLand, LampLandFilm. The base `Deferred\DeferredLamps` text is in neither cache on this
  PC: NOT RUN. Wetness, WetnessFallback and WetnessRestore (no constants read: `ubo ... unused`) convert too.
- **Sampler states** come from the device, not the layout (POINT/CLAMP at `DeferredSun.h:505-507`).
- **No change needed:** `tex2Dlod`, `clip`, `[loop]` and 4 targets.
- **ps_3_0 slot counts for scale** (D3DX):
  - sun 2,344;
  - SunFade 3,291;
  - lamps 916;
  - lamp shadows 3,026;
  - wetness 977;
  - LampSkin 660 and LampLand 593, which compile only with the flow-control retry.

## What disappears on the Vulkan path (PROVEN that each exists only for ps_3_0)
Each item is in `DeferredLamps.hlsl`:
- RT removed outside the shadow passes (`:25-31`). Also widen the `DL_SHADOW_PASS` gate at `:266`.
- The skin pass and `DL_PAST16_LIT`: lamps past the 16th unshadowed (`:61-67`).
- The fog-resolve pass and its LightTex (`:74-81`).
- The land-film pass (`:100-115`).
- The split skin lamp loop (`:199-213`).
- The 4 shadow passes into VisTex (`:307-326`).
- The land split (`DeferredLamps.h:294-296`).
- The pass-level ladder (`DeferredLamps.h:282-298`) and `Past16Unshadowed` (`:72`, `:327`). The SPIR-V path builds only the full level;
  D3D9 keeps the ladder.

**Not ps_3_0 limits** (they stay or need other work):
- the SunFade twin (a performance choice);
- 24 lamps (the forward constant block, `ManyLamps.h:56`);
- 63 per list and 1,024 table lamps (the list texture's layout, `ManyLamps.h:214-217`);
- the frame copy (D3D9 can't sample its own target);
- the 4-target extension limit.

## Frame time (NOT MEASURED; performance engineer's model)
**Model.** Today's passes run near the bandwidth floor:
- Sun: ~76 B/px predicts 0.235 ms; 0.20-0.27 ms measured.
- Lamps: ~110-118 B/px predicts 0.34-0.36 ms; 0.46-0.63 ms measured. The extra is the shadow PCF work.

**Expected savings:**

| | RTX 4070 Ti S, 1080p | 1440p | GTX 1070, 1080p (floor) |
|---|---|---|---|
| Full single pass (A), interior | 0.20-0.35 ms | 0.35-0.6 ms | ~1.1 ms |
| Full single pass (A), exterior day | 0.03-0.08 ms | 0.05-0.14 ms | ~0.28 ms |
| Lamps only in one draw, interior | 0.08-0.15 ms | 0.15-0.27 ms | 0.45-0.56 ms |
| Sun alone as one SPIR-V draw | ~0 | ~0 | ~0 |

**Costs a merge adds:**
- **Render passes:** about 8-12 fewer per frame indoors. BELIEVED worth 0.05-0.2 ms.
- **Occupancy:** one shader holding the worst path, skin + FIT 0 + RT. Likely over 128 registers, which means 25-33 % occupancy on Ada and
  poorer hiding of PCF latency. **The real risk is that the merged pass comes out slower on this PC.**
- **SPIR-V size:** the lamp variant grows from 55.6 KB to 126.6 KB. Size is not registers, but it is a hint.
- **First use:** a pipeline stall of 100-500 ms per variant (guess), avoided by prebuilding.

**On this PC the frame rate will not move:** it is CPU-bound (game thread 12-15 ms). A weak GPU or 1440p is where it shows. We have no weak
GPU on the bench; that needs a tester.

## Risks (ranked)
1. **The merged pass is slower** from occupancy. Mitigations: a step-by-step A/B; separate variants for heavy parts; compute only if
   needed.
2. **Parity between D3D9 and Vulkan.** One source removes drift in the maths. What remains (BELIEVED):
   - fxc vs glslang order and FMA;
   - DXVK's D3D9 float rules (0 x inf);
   - `pow` of negative bases (X3571 warnings at `DeferredPixel.hlsl:123`, `DeferredLamps.hlsl:477`, `DeferredWetness.hlsl:236`);
   - the half-pixel convention of the extension's triangle vs the D3D9 quad, for point-sampled G-buffer reads.
3. **The RenderScale viewport:** the Draw must take the device viewport.
4. **Variant explosion:** the cache-hash key, with D3D9 for misses.
5. **Fork-only features** (lamps past the 16th shadowed, more lamps). Native D3D9 and stock DXVK players would see a different picture.
   That is the owner's call.

## Step plan
Each step ships on its own behind a switch and can be switched off.

One live switch: `[Main.Main.Performance] NvrDxvkDeferredResolve`.
- 0: off (default).
- 1: sun on Vulkan.
- 2: sun and lamps on Vulkan, as separate draws.
- 3: one merged draw.

It goes in `resource/defaults/Core.toml`. It is off by default, so no migration is needed.

| Step | What | Path | Players see | Frame time | Effort | Needs owner's word |
|---|---|---|---|---|---|---|
| **0** | **Lamp light outdoors from the first frame.** The atlas is only ensured in the interior branch (`ShadowManager.cpp:3465`), and the lamp pass skips without it (`DeferredLamps.h:467-468`). Fix: when there is no atlas, bind a local 1x1 R16F stand-in holding 1.0 (lit) on s8. Not the real 128 MB atlas, and never published to `PointShadowForward::AtlasTexture`, which other code reads (`ManyLamps.h:427`, `PointShadowForward.h:489,887`, `DeferredTransparents.h:250-252`). Outdoors no tile is written, so lamps are unshadowed or RT-traced, as designed (`DeferredLamps.hlsl:286-292`). Skip the 4 shadow passes while `!PointShadowForward::Active`. Also fixes the lamp pass dying indoors if the atlas fails to make (`ShadowAtlas.h:54`). Live switch `[Shaders.ShadowsInteriors.Deferred] AtlasStandIn`, shipped off, on in the test toml. Plus timers: "Deferred span (all)" (resolve end to `LinearScene::EndSpan`), the LinearScene converts, and the span's render-pass count. **As built (2026-11-21, built, not run in game):** `DeferredLamps.h` `StandIn()` (:73-97, a MANAGED 1x1 R16F = 1.0, so no device-reset handling) and the skip at :498-502; s8 gets the atlas or the stand-in (:686); `noShadowPasses` (:706-712) = stand-in and `!PointShadowForward::Active` and not DebugView 8 (which reads s9 whatever c10.y says); then c10.y = 0 and the lighting pass reads no `DL_Vis` stage (`DeferredLamps.hlsl:408`; every pass's loop is `DL_LAMP_LOOP`), and with no tile row written a lamp takes `DL_Grid.w` = UnshadowedLamps (`DLShadow`, :265-292). `ShadowedThisFrame` is not set on the stand-in (:756), so the screen-space lamp shadows run as without the pass. One file beyond the plan: `ManyLamps.h:423-431` `TilesWithoutAtlas` (set from the switch in BeginFrame) lets `PutTiles` write the tile rows without the atlas, so with the RT lamp trace outdoors the traced lamps get their trace ids as they do outdoors after an interior (slots have span 0 until `ShadowAtlas::Update` gives a tile, so the stand-in is never sampled). Timers: "Deferred span (all)" (`Render.cpp:2914,2921-2922`: started first in DeferredSpan's end, ended after LinearSpan's), "LinearScene to linear" / "LinearScene encode" per conversion (`LinearScene.h:102-105`). The render-pass count is NOT built (left for step 1). Key in the Internal list; the test toml is not changed here | deferred, D3D9, every player | street lamps and fires light the ground and objects outdoors in a game started outdoors | the lamp pass runs on every outdoor frame with lamps in the table, day too: est. +0.2-0.5 ms (NOT MEASURED) | 0.5-1 | yes, to ship on (a bug fix that adds cost). Built now as a test switch |
| **1** | **Toolchain and plumbing, proved on the sun.** `hlsl2spv` template mode (the six fixes, Windows path), an Sm6Pass "record mode" (constants from the device registers, textures from the stages), RT0 swap, viewport, in-shader depth test, own failure latch, prebuilt pipelines, `NvrDxvkKeepD3D9` A/B. DeferredSun as one Vulkan Draw | deferred, Vulkan fork only | nothing (same picture) | ~0 (proves the path) | 3-4 | no (test switch, off) |
| **2** | **Lamps as one Vulkan draw:** 9 draws become 1. Shadows inline (FIT 0 + RT for every lamp), fog folded, land, film and skin in the loop. D3D9 keeps its ladder | deferred, Vulkan | same picture; lamps past a pixel's 16th get their traced shadow; the X4505 fallbacks can no longer happen on the fork | -0.08 to -0.15 ms indoors here (NOT MEASURED) | 3-4 | no (off). Default on later: yes |
| **3** | **Sun and lamps in one draw**, plus FlashlightDeferred as a lamp. Folding in LinearScene's to-linear convert is OPEN: the convert rewrites every pixel, forward ones too, while the light pass clips to lighting-free pixels (`DeferredSun.hlsl:206-218`). An ADD blend can't do a non-linear rewrite without the frame copy | deferred, Vulkan | same picture | -0.2 to -0.35 ms indoors here, ~-1 ms on a GTX 1070 (NOT MEASURED) | 2-3 | no (off). Default on later: yes |
| **4** | **Fork-only light:** all lamps shadowed, more than 63 lamps per list (a buffer list), better lamp filtering | deferred, Vulkan | more and better lamp shadows on the fork only | + (more work) | 2-4 each | yes (fork vs native look) |
| **5** | **First-person re-run on the same pipeline** (with its stencil); compute in-place only if measurements ask for it | deferred, Vulkan | first person matches | small | 1-3 | no (off) |

**Total:** about 12-17 sessions to step 3 with first person. The audit's 8-12 assumed a wider scope with less proof.

**Default on.** Turning steps 2-3 on for everyone needs:
- bench parity in every scene;
- no timer regression;
- the owner's word.

## First step proposal (to build now)
**Step 0 + step 1, on branch `deferred-single-pass`.** Install as the next install-testN, then bench (owner's go-ahead via Main brain,
2026-11-21).

**Bench plan:**
- **Setup:** slot bench `F:\NVR-bench` (start with `MO2-SLOTS.md`), a test build, game copy only.
- **Scenes, each with the switch on and off from the same camera:**
  - Goodsprings;
  - the Strip at night;
  - desert noon;
  - Lake Mead;
  - a metal interior;
  - Vault 21;
  - a dark interior with many lamps;
  - forced rain.
- **Step 0 check:** night exterior before and after, started outdoors with no interior visited.
- **Step 1 parity:**
  - same frame, D3D9 vs Vulkan;
  - a diff image amplified x16;
  - DeferredSun debug views 2 (shadow), 3 (cascade) and 8 (sunVis).
  - Pass: no whole-object differences, and below about 1/255 off edges.
  - At FSR 0.67: no light outside the render rect.
- **Fault test:** remove the `.spv`. It must be logged once, and the picture must match the switch off.
- **Timing:**
  - span timer p50, A/B interleaved, 240-frame windows, the first window after each toggle dropped;
  - only from a run alone (no other game);
  - pass: span p50 no worse than +0.02 ms in any scene.

## Who said what (where the roles disagreed)
**Step 1: sun or lamps?**
- The architect and the renderer engineer wanted the lamps first: that is where the splits and the cost are.
- The shader engineer wanted the sun first: it is already one draw, so it proves the toolchain, constants, depth and parity without
  changing any lighting maths.
- **Chosen: the sun.** It separates plumbing bugs from lamp-merge bugs, and the lamp step follows straight after on the proven plumbing.

**Depth test.**
- Shader engineer: `DrawDepthStencil` + `FragDepth`.
- Renderer engineer: an in-shader discard against the linear z copy.
- **Chosen: in-shader first.** It uses proven pieces; the attachment route is the fallback.

**Is this worth it?**
- The performance engineer argued the frame-time value is small on this PC. The bigger lever is the point shadow cube maps (3.5-4.6 ms
  indoors).
- All agreed the value is robustness (no X4505 fallbacks), the dropped features coming back, and weak GPUs. The cube maps are a separate
  study.

**Cheaper D3D9 idea** (performance engineer, BELIEVED ~0.05 ms here, ~0.13 ms on a GTX 1070): fold LinearScene's to-linear convert into
the sun pass. The architect's review found it does not work as stated: the sun clips to lighting-free pixels, while the convert must
rewrite forward pixels too. Open.

**Architect's review of this doc:** approve with changes, all applied:
- the step-0 stand-in and its switch;
- the converter's constant offsets;
- the VPOS fix;
- dummy inputs;
- the failure helpers;
- the two-target notes;
- the fog wording;
- the step-3 convert marked open.

## Not checked / open tests
1. An RT0-swapped Draw into the scene target.
2. The extension reading the DYNAMIC lamp table.
3. In-shader depth-test parity.
4. D3D9 vs Vulkan colour parity.
5. One Draw's cost vs today's draws.
6. Register counts of the merged shader (no vendor compiler offline).
7. Whether `RenderRect::Active()` stays false through the whole span.
8. Reading constants back under DXVK without a stall (BELIEVED fine; the sun does it today).
