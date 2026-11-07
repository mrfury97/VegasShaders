# NvrRtHost: the RT framework's test bench without the game (2026-11-06)

A small 32-bit Windows program that plays the game's side of the ray tracing core (`src/core/RtCore.cpp`) and drives the real
`NvrRtHelper.exe` through the real protocol, so the trace shaders, the helper, its geometry path, the alpha test and the
temporal history can be tested in seconds. No Fallout New Vegas launch.

```
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-host\run.ps1
```
builds the helper (MSBuild x64, its own project) and the host (cl x86), checks the constant-block layouts, runs every scene, and
prints PASS / FAIL per scene (exit code 0 = all passed). 23 scenes (2026-11-07: same-frame, denoiser and PT-quality scenes merged), about 30 s in all on this PC (9800X3D iGPU) with the build;
the first run after a shader change takes ~45 s (the helper compiles the new pipelines, then they are cached).

| option | what |
|---|---|
| `-Compile` | compile the scenes' shaders from `src\hlsl\NewVegas\Compute\Rt\*.comp` now (into `bin\spv`) instead of running the committed `.spv` |
| `-Spv <folder>` | run the trace `.spv` files of that folder (all ten the scenes use: see `build.ps1 -Compile`'s list) |
| `-Scene furnace,mirror` | only these scenes |
| `-Fork <d3d9.dll>` | the NVR DXVK fork to test with (default: the bench copy `W\bench\deep-roadmap\rtmerge\fork-r13\d3d9.dll`, or `$env:NVR_RT_HOST_FORK`) |
| `-Device <name part>` | DXVK's and so the helper's GPU (default `Radeon`: the iGPU; the GTX 1070 has no ray queries) |
| `-NoBuild` | skip the build |

Output in `tools\nvr-rt-host\out\` (git-ignored): `report.txt` (the whole run), a `.pfm` of every failed scene's output image,
the DXVK log. The helper writes `NvrRtHelper.log` next to itself (`bin\`).

## Design: why the end-to-end host (option a), not synthetic captures (option b)
Two ways were open: (a) a host that makes a D3D9 device on the fork and goes through the same extension path as RtCore, or (b) a
generator of `.nvrcap` captures for `NvrRtHelper.exe --replay`. (a) was cheap because `tools/nvr-rt-helper/test/rt_helper_test.cpp`
(2026-10-28) already did the plumbing (device, shared images, timeline semaphores, StageGeometry, the helper as a child); the host
grew out of it. So (a) covers what (b) cannot:
- **shared images** made by the fork, rounded to 256 texels as `RtCore::AddImage`, imported by the helper by handle; inputs go in
  through `CopyToShared` from D3D9 textures (`RtCore::CopyIn`), outputs come back through `CopyFromShared` (`RtCore::CopyOut`);
- **geometry staging**: D3D9 vertex / index buffers converted by the fork into the arena (`StageGeometry`, texcoords through
  `NVR_EXT_RT_TEXCOORD_FLAGS(12)`, the arena sequence texel through `FlushStagedGeometrySeq` when the fork has patch 0034), BUILD
  and RELEASE ops, built by the helper;
- **instances + records** written as RtCore writes them: camera-relative transforms, cull masks (0x01 land, 0x02 objects, 0x10
  alpha-tested), record flags with the alpha reference (bits 8-15), the atlas slot (16-25) and the material rule (26-31);
- **passes** with the game's image lists (stand-ins included) and the game's constant structs (mirrored: the layout check below
  keeps the mirrors honest), the exchange (`SignalShared` / `WaitShared`) and the frame ring.
Since the second round (2026-11-06 b) it also covers:
- **RtCore's scene picking**: the rules `RtCore::AddGeometry` applies to every geometry (cull angle, no data, skinned, leaf cards,
  lighting shader, decals, blended-only vs alpha-tested, far LOD) and the record / mask / build-flag packing now live in
  `src/core/RtScenePick.h` (plain C++, no game types). RtCore calls it; the host tests every rule and packs its own records with it.
  The walk over the game's node trees (which NiGeometry reaches AddGeometry at all) still needs the game.
- **the game's D3D9 side**: RtDenoise's fork passes (`RtDenoiseTemporal.spv`, `RtDenoiseAtrous.spv`) run on the host's device with
  RtDenoise::Run's bindings and push blocks; the lamp shaders' lookup `PointShadowTraced` (`PointShadowForward.hlsl`) is compiled
  ps_3_0 as the game compiles it and reads the traced result through `NVR_RtLampReproject` from the next frame's camera.
- **the other passes**: sun shadows, AO / sky, the path tracer with the albedo atlas and the lamp image (all-lamp loop vs
  PTLightGrid, the grid built by `src/core/RtLightGrid.h`, moved out of RtPathTrace.cpp unchanged), ReSTIR GI and ManyLights' one
  ray a pixel.
Still not covered: the resolves / composites that turn these outputs into the picture (Reflections' resolve, the object shaders'
lighting maths around the lookup), RtDenoise's RGB mode and DenoiseBackbone, RT GI's own reuse pass (RtGITrace), the radiance cache,
glow lights, actors / skinned geometry. Those still need the game (or the D3D9 frame replay).

## What is checked
**Layouts** (`rt_layout_check.py`, no GPU): each trace pass's C++ constant struct (`RtLampShadows.h` PassConstants,
`RtPathTrace.cpp` PassConstants + RestirConstants, `RtSunShadows.cpp` PassConstants, `Reflections.cpp` RtPassConstants) against
its shader's std140 block: every member named on both sides at the same byte offset, the block no longer than the struct, both
within 512 bytes; and the host's mirror structs equal to the game's. (RtAOTrace has three blocks by `#ifdef`, not checked.)

**Scenes** (128 x 128, every output checked against an exact CPU reference on the same triangles; pixels on a shadow / alpha /
geometry edge, where a 0.3-pixel shift changes the answer, are skipped and counted):

| scene | shader | world | pass condition |
|---|---|---|---|
| `furnace` | RtPathTrace | an open box of albedo 1 under a uniform sky L (48 bounces, 16 paths); then a floor of albedo 0.5 | box: image mean = L within 1.5 % per channel (measured 0.04 %); floor: every pixel 0.5 L within 0.2 %; no NaN |
| `alphacard` | RtLampTrace + RtAlpha.glsl | a lamp above an alpha-tested card (atlas slot 37, the second row: an 8 x 8 checker, ref 128) above a floor | every pixel's entry (slot, visibility 0 / 15, depth code) = the CPU's ray with the same alpha test; both shadowed and lit pixels present |
| `material` | the same | the same card whose record also has material bits 26-31 (rough 7, metal 5) | the result equals `alphacard`'s bit for bit (the 2026-11-06 unmasked-slot bug) and the reference |
| `manylamps` | RtLampTrace | 48 lamps on a grid over land, blocks beside lamps 3, 33, 40, 47; 3 lamps a pixel | every pixel's three entries (ranking + visibility) = the CPU's; > 1000 correct entries name slots 32-47 and some pixels are shadowed from them |
| `mirror` | ReflectionsRT | a mirror floor, red and green blocks, a plate shading the red block's front; RayGen's inputs made on the CPU | hit distance, flags, normal, albedo and the sun visibility at the hit = the CPU's; no-ray pixels -1 |
| `cameramove` | RtLampTraceMany | 40 lamps, 7 blocks; 40 frames at camera A, then the camera slides 260 units and traces once with the history (ping-pong pair, reprojection matrix as RtLampShadows builds it) | the rest estimate R stays near the exact sum(w V) / sum(w) (0.004 at A, 0.006 after the slide); a stale reprojection run alongside must be > 2x worse (measured 0.044: the test sees ghosting); the two exact entries per pixel all right |
| `sun` | RtSunShadow | alpha card + block under a low sun, traced at half resolution (64 x 64 of a 128 x 128 depth), Penumbra on | every traced pixel's visibility and blocker distance = the CPU's ray from the shader's own point (the traced uv with the full-size pixel's depth) |
| `ao` | RtAOTrace (plain pass) | floor, wall, roof, alpha card; 8 AO rays, 16 sky rays | AO, sky visibility and "covered" = the CPU repeating the shader's rays exactly (its packed normal decoded the same way); at most 0.3 % of texels off (rays grazing an edge) |
| `pathtex` | RtPathTrace + RtPathTraceGrid | a textured floor (atlas slot 45, material bits set); then 64 white lamps through the lamp image, with and without blocks | DebugView 1 = the atlas texel's colour, sRGB-decoded; DebugView 3 (direct light) within the exact sum's range over the pixel; PTLightGrid (built by the game's RtLightGrid::Build) bit for bit equal to the all-lamp loop |
| `restirlamps` | RtLampTraceRestir | the cameramove world, ManyLights' one ray a pixel (3 x 3 jobs, spatial radius 1, two history pairs) | after 60 frames the exact entries right (edges skipped) and R within 0.05 of the exact; after the slide still close, a stale reprojection worse |
| `restirdi` | RtPathTrace (PTRestirDI) + the game's `RtRestirRow.h` | floor, 7 blocks, a wall, 40 coloured lamps, the lamp list shuffled every frame (the reservoirs survive only through the game's remap); a narrow view (a pixel ~5 units, as the game's) | 40-frame mean within 2 % of the exact shadowed lamp sum (and the plain pick's within 2 %); frame noise < 0.6 x the plain pick's (measured 0.19 x); after a 150-unit slide < 0.8 x (0.33 x) and within 3 %; the run with the remap broken > 1.2 x worse (11.5 x) |
| `specfurnace` | RtPathTrace (PTSpecular) | the furnace box in white metal (metal 1, F0 1), roughness 3/7 and 6/7, uniform sky, 48 bounces x 16 frames x 16 paths; one bounce off a white metal floor | box mean = L within 2 % (3/7) / 3 % (6/7, heavy-tailed weights); floor mean = L within 1.5 % (printed by view cosine). Control `NVR_RT_HOST_NO_ENERGY=1` (no multiple-scattering compensation): 0.79 / 0.19 / 0.52 |
| `cache` | RtPathTrace (PTRadianceCache) + RtPathTraceCache | the furnace box in albedo 0.8 under a uniform sky; reference 32 bounces, 128 paths | 2 bounces without the cache < 0.97 (0.61: the scene needs multi-bounce light); 2 bounces + the cache within 2.5 % (0.983), every 16 x 16 tile within 6 %; the sky halved, 60 frames later within 3 % of half (1.002) |
| `restirgi` | RtPathTraceGI + RtRestirGI | floor + two coloured walls, sun + sky; reference = the plain tracer's 512-path mean | the reuse's picture (x the demodulated albedo) averaged over frames 8-39 within 3 % of the reference's mean (measured 2.6 %); one frame's RMSE with the history < 0.85 x without; a frame right after a slide within 5 % |
| `denoise` | RtDenoiseTemporal + RtDenoiseAtrous (fork graphics passes, D3D9 side) | a one-sample signal (each pixel lit with probability = a known clean value) over a floor with a darker disc and a box | after 16 frames the flat-area error < 0.3 x the noise's (0.028 vs 0.19), no bleeding next to the box; after a slide the error round the disc's edge with a stale camera > 1.5 x the right one's |
| `sigmalamps` | RtLampTraceSigma + the helper's RtShadowDenoise module (2026-11-07, docs/next/RT-DENOISER.md) | floor, two poles, a floating plate; two lamps of physical size (radius 14 and 7) | against the EXACT soft shadow (576 rays a pixel per lamp, the shader's own disc sampling): after 48 still frames the penumbra error < 0.35 x the raw ray's, below the old Filter's and 8 rays', bias < 0.03, contact < 0.5 x raw + 0.02; a camera sliding 6 units a frame: < 0.5 x the raw ray's, a frozen reprojection > 1.2 x worse |
| `sigmasun` | RtSunShadow (half resolution) + the module in sun mode | the same world under a low sun: a 10 degree test disc and the real 0.53 degree sun | the 10 deg disc as sigmalamps (still < 0.5 x raw, moving < 0.6 x raw); the real sun (penumbrae under a pixel: the ray is nearly the answer): better than the raw ray at rest, no worse than 1.1 x it in motion |
| `refldenoise` | the RT reflection denoiser (src/core/RtReflDenoise.h: ours = core 0, the AMD FidelityFX Reflections port = core 1; fork passes, D3D9 side; 2026-11-07, docs/next/RT-REFLECTION-DENOISER.md) | analytic: a floor in roughness bands 0.02 / 0.08 / 0.2 / 0.4, coloured boxes, a bright panel, a sky; one VNDF ray a pixel as RayGen draws it (RoughShare borrowing); the old Temporal + Blur ported to the CPU line for line | against the EXACT reflection (32 x 32 stratified samples per pixel): ours below the old path at rest (all and contact), in motion (slide + turn, worst frame) and below 0.5 x the raw ray at rest; prints raw / old (today's noise) / old (fixed noise) / ours / FFX per band, contact and flicker; `NVR_REFL_CORES=0,1` picks the cores, `refl_sheet.py` makes pictures of the dumps |
| `lamplookup` | PointShadowForward.hlsl `PointShadowTraced` (ps_3_0, D3DCompile) + RtLampTrace / RtLampTraceMany | frame A traced (48 lamps, or ManyLights after 24 frames); camera B slid sideways looks each pixel's lamp up | every answer from A's trace = the CPU's visibility at B's point; "not traced here" only where A's top three really lack the lamp; off-screen only off A's screen; ManyLights: a lamp outside the top two gets R |
| `sameframe` | RtLampPrimary + RtLampTrace + `PointShadowTraced` (2026-11-07) | a walker between a low lamp and a wall, restaged each frame as an actor (DYNAMIC, REBUILD every 3rd), a hidden first-person body (`NVR_RT_CUSTOM_HIDDEN`) in front of the camera, a new jitter each frame; 7 frames | SameFrame: camera rays + trace in one exchange, looked up with this frame's camera: 0 wrong answers, > 90 % answered; the camera rays' depth + normal = the CPU's; the hidden body passed over; the pipelined control (last frame's trace reprojected) > 40 wrong a frame (measured 217-278: the ghost) |
| `movingrest` | RtLampTraceMany + RtLampTraceRestir, MovingCasterHistory (2026-11-07) | 40 lamps, a walker still 30 frames then walking 8 | R vs the CPU's exact R where the walker's shadow is or was: the smear above a noise-floor run (cap 2, walker never moved) > 0.06 with no cap and under 0.35 x of that with cap 2 (measured 0.141 -> 0.025 Many, 0.110 -> 0.037 one ray); the rest of the floor < 0.06 / 0.1 |
| `scenepick` | RtScenePick.h (no GPU) | 21 picking cases, 15 mask / op / record cases, 73728 record packings (1024 slots x refs x material rules) | every fate, mask and flag as AddGeometry had them; every packed field reads back as the shaders read it (slot `& 0x3FF`), nothing spills |

**Proof that it catches the 11-06 bugs** (2026-11-06, scratch builds, not committed):
- `RtAlpha.glsl` before eacf46e1 (slot = flags >> 16, unmasked) + current RtLampTrace: `alphacard` PASS, `material` **FAIL**
  (1754 mismatches, 0 shadowed pixels: every ray passed the card).
- `RtLampTrace.comp` before e66e766b (`manyUnused[7]`): `alphacard` and `manylamps` **FAIL**; `rt_layout_check.py` on that file:
  "RtLampTrace.comp reads 'jitter' at byte 256 (vec4 16), RtLampShadows.h PassConstants writes it at byte 240 (vec4 15)".
- The lamp-slots-32-63 bug of 11-06 (43d5cf53) was in NVR's D3D9 lamp shaders, not the helper: out of this bench's reach;
  `manylamps` / `cameramove` prove the traced side handles slots past 32.

**The RT shadow denoiser's cores** (2026-11-07): `NVR_SIGMA_CORE=1` (AMD FidelityFX Shadows) or `2` (FidelityFX with our penumbra-sized
steps) runs sigmalamps / sigmasun on that core instead of ours (0); their pass conditions are ours, so 1 and 2 FAIL (the comparison is
in docs/next/RT-DENOISER.md). `sigma_sheet.py` turns the scenes' PFM dumps into pictures.

## Adding a scene
In `rt_host.cpp`: build a `World` (meshes + objects with masks / record flags / alpha slots; `Mesh::quad`, `Mesh::box`), stage it
(`stageWorld`), fill the pass's constants with the game's struct (mirror it next to `LampConstants` and add the pair to
`rt_layout_check.py` MIRRORS), `AddPass` with the game's image list, `Exchange`, `Read` the outputs, compare with `trace()` (the CPU
ray caster: same triangles, same alpha rule), `releaseWorld`. Register a new shader in `main` (`AddShader`) and a new image with
`AddImage` (at most 48 in all, index 0 the arena). Add the shader name to `build.ps1 -Compile`'s list.

## Rules kept
- Never launches the game. The host starts its own helper on its own shared-memory name (`Local\NvrRtHost-<pid>-<tick>`): it does not
  touch a running game's helper, and it is the only program writing to its helper's shared images.
- The helper is built with its own project command (`msbuild tools\nvr-rt-helper\NvrRtHelper.vcxproj /p:Configuration=Release /p:Platform=x64`).
- Needs: VS 2019/2022 C++ tools (x86 + x64), the Vulkan SDK only for `-Compile`, Python 3 for the layout check, a GPU with ray
  queries for 64-bit programs (here the AMD iGPU), and the fork's d3d9.dll.
