# NvrRtHost: the RT framework's test bench without the game (2026-11-06)

A small 32-bit Windows program that plays the game's side of the ray tracing core (`src/core/RtCore.cpp`) and drives the real
`NvrRtHelper.exe` through the real protocol, so the trace shaders, the helper, its geometry path, the alpha test and the
temporal history can be tested in seconds. No Fallout New Vegas launch.

```
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-host\run.ps1
```
builds the helper (MSBuild x64, its own project) and the host (cl x86), checks the constant-block layouts, runs every scene, and
prints PASS / FAIL per scene (exit code 0 = all passed). 14 scenes, about 23 s in all on this PC (9800X3D iGPU) with the build;
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
| `restirgi` | RtPathTraceGI + RtRestirGI | floor + two coloured walls, sun + sky; reference = the plain tracer's 512-path mean | the reuse's picture (x the demodulated albedo) averaged over frames 8-39 within 3 % of the reference's mean (measured 2.6 %); one frame's RMSE with the history < 0.85 x without; a frame right after a slide within 5 % |
| `denoise` | RtDenoiseTemporal + RtDenoiseAtrous (fork graphics passes, D3D9 side) | a one-sample signal (each pixel lit with probability = a known clean value) over a floor with a darker disc and a box | after 16 frames the flat-area error < 0.3 x the noise's (0.028 vs 0.19), no bleeding next to the box; after a slide the error round the disc's edge with a stale camera > 1.5 x the right one's |
| `lamplookup` | PointShadowForward.hlsl `PointShadowTraced` (ps_3_0, D3DCompile) + RtLampTrace / RtLampTraceMany | frame A traced (48 lamps, or ManyLights after 24 frames); camera B slid sideways looks each pixel's lamp up | every answer from A's trace = the CPU's visibility at B's point; "not traced here" only where A's top three really lack the lamp; off-screen only off A's screen; ManyLights: a lamp outside the top two gets R |
| `scenepick` | RtScenePick.h (no GPU) | 21 picking cases, 15 mask / op / record cases, 73728 record packings (1024 slots x refs x material rules) | every fate, mask and flag as AddGeometry had them; every packed field reads back as the shaders read it (slot `& 0x3FF`), nothing spills |

**Proof that it catches the 11-06 bugs** (2026-11-06, scratch builds, not committed):
- `RtAlpha.glsl` before eacf46e1 (slot = flags >> 16, unmasked) + current RtLampTrace: `alphacard` PASS, `material` **FAIL**
  (1754 mismatches, 0 shadowed pixels: every ray passed the card).
- `RtLampTrace.comp` before e66e766b (`manyUnused[7]`): `alphacard` and `manylamps` **FAIL**; `rt_layout_check.py` on that file:
  "RtLampTrace.comp reads 'jitter' at byte 256 (vec4 16), RtLampShadows.h PassConstants writes it at byte 240 (vec4 15)".
- The lamp-slots-32-63 bug of 11-06 (43d5cf53) was in NVR's D3D9 lamp shaders, not the helper: out of this bench's reach;
  `manylamps` / `cameramove` prove the traced side handles slots past 32.

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
