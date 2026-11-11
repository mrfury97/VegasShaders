# RT playground

A walkable test room for Vegas Shaders' ray tracing, without the game. It opens a window. You move with WASD and the mouse and see the
path tracer the way the game runs it under `[Main.Main.RayTracing] PTRealtime`. It uses the same `.spv` shaders, the real
`NvrRtHelper.exe` and the NVR DXVK fork's `d3d9.dll`. Use it to see lighting in motion (denoiser lag, noise, moving lamps, the sun
through a window, mirrors) in seconds, without a game launch.

```
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1                 # build + open (640x360 traced, 1280x720 window)
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Size 960x540
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Demo 60   # scripted minute: screenshots + fps in out\
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Demo 60 -Set R=0 -Tag _restir_off   # an A/B
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Demo 20 -Size 320x180 -FixedStep 0.5 -Set "F2=0,F4=0" -Tag _off
```

`-Demo N` plays the scripted 100 s (10 segments: the room, the window + fence, raw, path tracer off, the mirror, the reference, night,
glass + leaf + skin, and the matched references of 05 and 08, below) in N seconds. With `-FixedStep S` the script advances S seconds a frame instead, so A/B runs see exactly the same
frames (the camera, the lamps, the random seeds); the run still stops at N seconds of wall clock. It saves one screenshot a segment (the HUD is the caption) and logs each segment's fps, frame ms and helper GPU ms to
`out\playground.log`. Its exit code is 0 only without helper errors. `-Set <key>=<value>` holds a toggle through it, by its key
(`R=0`, `B=1`, `Z=1`, `G=1`, `F2=0`, `F5=1`); `-Tag` suffixes the screenshot names and shows on the HUD. Each screenshot's line in the log
(and `out\fireflies.csv`) carries a count of bright outliers on the window-size frame (a sparkle detector; at a small `-Size` the frame
is scaled up and a sparkle becomes a block, so count at the internal size instead: the method is in docs/next/RT-PATH-TRACING.md).
`--max-frames N` stops after N frames.

**Films (2026-11-09).** `-Frames <file>` appends every frame's finished picture (window size, RGB, 3 bytes a pixel) to one raw file;
`-FramesRange a,b` keeps only the demo's script seconds [a, b) (segment 05 = 30,40: 300 frames at `-FixedStep 0.0333`, ~0.8 GB at
1280x720). With `-FixedStep 0.0333` played at 30 a second the film shows what a card that runs the path tracer in real time would show
(noise crawl and ghosting only show in motion): `ffmpeg -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i <file> -c:v libx264 -crf 18
-pix_fmt yuv420p out.mp4`, then delete the raw file.

**Object motion (2026-11-09).** The playground draws the game's object motion buffer for the things it animates (MotionPass: each moved
object drawn again with this frame's and last frame's transform; rgb = the world move since last frame, a = 1, where the depth matches),
binds it as the game does (`t_motion`, backbone.x 1, MovingHistory 8) for both denoisers. Key 5 / `-Set 5=0`: without it (before).

**Scoring against a reference (2026-11-09).** The script now runs 100 s: segments 09 and 10 are the many-sample references (~450
samples) of 05 (the mirror + spheres) and 08 (glass + leaf + skin) at the SAME camera and the SAME moment as those screenshots (the lamps
and the cube frozen there; `UsePose`). `py -3.11 -I tools\nvr-rt-playground\score_mirror.py _tagA _tagB` scores each run's 05 and 08
against them over fixed regions (the wall mirror, the chrome and gold spheres, the floor; the glass pane, the leaf, the sun patch): the
mean absolute error, a high-frequency noise measure, its ratio to the reference's, and the brightness bias. Judge the denoiser at
`-FixedStep 0.0333` (30 frames a second of motion; 0.2 starves every history), the same step for every A/B; at 320x180 the 100 s take
about 7 minutes. At each screenshot the log also gets two `probe` lines (left / right half): the specular denoiser's history length,
the virtual reprojection's share and how many smooth pixels are marked mirror surface (segment 05: the chrome sphere is on the left,
the wall mirror on the right).

`build.ps1` fills `tools\nvr-rt-playground\bin\` with everything the program needs: `NvrRtPlayground.exe`, `NvrRtHelper.exe`,
`d3d9.dll` (the fork), `shaders\Rt\*.spv`, `shaders\Compute\*.spv` and `run-playground.cmd`. That folder runs on its own:
double-click `run-playground.cmd`, or copy the folder to another PC. The compiler's intermediates and TEMP go to
`$env:NVR_BUILD_SCRATCH\playground` when that variable is set, else `obj\` here (`-Scratch` changes it). Each build copies the shaders from the repo again
(`-Compile` compiles them from source instead), so merged shader changes show up on the next run.

`-Device` (default empty) picks the GPU through `DXVK_FILTER_DEVICE_NAME`. Empty: the first GPU the 64-bit helper's `--probe` finds with
ray queries (this PC: the Radeon iGPU, not the GTX 1070; an RTX PC: the RTX).

**Closing** (the window's X, Alt+F4, Esc, Ctrl+C or closing the console, a demo's end, an error) always runs the same shutdown
(`PgHost::Stop`):
1. no more frames; wait on the CPU until the helper has finished every frame submitted;
2. drain the D3D9 queue (SignalShared on our fence, polled: a D3D9 EVENT query never reported done under the fork);
3. ask the helper to quit and wait for it (TerminateProcess only when it is really hung, logged);
4. release every D3D9 / fork object and the device.

If the helper died, `SignalFromHost` releases the GPU waits on it first. Without this, a close left the D3D9 queue waiting on the
helper and reset the display driver (Windows System event 4101). The log's `shutdown:` line says "clean" or what had to be forced.

The first start after a shader change can take a few minutes on the iGPU while the helper compiles its pipelines. The title bar says
so while it waits.

## Controls

| Key | |
|---|---|
| W A S D, Shift | move, faster |
| Space / E, C / Q | up, down |
| right mouse button (held), M (toggle) | mouse look |
| 1-4 | viewpoints: the room, the window + fence, the mirror + spheres, outside |
| P | path tracer on / off (off: the raster shading + RT sun shadows, `RtSunShadow`) |
| N | path tracer mode: **PTRealtime** (denoised) / **reference** (`ReferencePT`: a running mean while nothing moves, up to 4096) / **raw** (the one path a pixel the denoiser gets) |
| B | bounces 1-4 (default 3, as PTRealtime). At 1, PTLastSky and PTLastBounce turn on, as in the game |
| U, arrow keys | sun on / off; turn the sun round (left / right), raise or lower it (up / down) |
| K, L | lamps on / off; the blue lamp moves / stops |
| J | the cube spins / stops (a moving object for the denoiser) |
| G | PTGlowLights (the glowing panel sampled as a light) |
| H | PTDetail |
| X | PTSpecular |
| R | PTRestirDI |
| Y | PTRadianceCache |
| F | PTLampFalloff (the game's / physical) |
| Z | PTIndirectClamp 0 / 4 (the game's firefly cut for light through a bounce) |
| F2 | PTEmissive (glowing triangles as lights: `RtEmissiveBuild` + the emissive row) |
| F3 | PTDenoiseSplit (diffuse and specular denoised apart: `RtDenoiseTemporalSpec`) |
| F4 | PTDenoiseAntiFirefly (`RtDenoiseFirefly`, PTDenoiseFireflyK 2) |
| F11 | PTDenoiseMirrorSurface 0.1 / off (2026-11-09: mirrors and chrome denoised as the thing they reflect; off = as before; `-Set F11=0`) |
| 5 | object motion on / off (2026-11-09: the game's TESR_ObjectMotionBuffer for the things that move here, the cube and the lamp globes; the denoisers subtract it and cap moving pixels at 8 frames, as PTDenoiseObjectMotion; off = no object motion, as before; `-Set 5=0`) |
| F5 | PTGlass 0 / 1 (the light) / 2 (the camera's rays too) |
| F6, F7 | PTFoliage 0 / 0.4; PTSkin 0 / 1 (+ PTClothSheen 0.5) |
| F8 | PTContactShadows |
| O | PTSplit 0.5 (left: the raster frame, right: the path tracer) |
| I | with the path tracer off: RT sun shadows on / off |
| V | PTDebugView: picture, albedo, normals, direct only, rough/metal, direct highlight, reflection only, radiance cache |
| T, + / - | tonemap (ACES fit) on / off; exposure |
| F1 | HUD: full / short / off |
| F12 | screenshot (`out\shot_NNN.png`, the window as shown, HUD included) |
| Esc | release the mouse, then quit |

The HUD and the title bar show the internal and window sizes, fps and frame ms, the helper's GPU time (`gpuTimeUsAvg`, and its
passes `traceUsAvg`), the GPU, the mode, and every toggle.

## What a frame does (the game's PTRealtime path)

1. **G-buffer by D3D9 raster** at the internal size. The outputs are the game's buffers in the game's encodings: `TESR_DepthBuffer`
   (view z / farZ, R32F), view-space normals x 0.5 + 0.5, linear albedo (a = 1 where a traced surface was drawn), the MaterialBuffer
   (r = class + 0.49 metal, b = roughness; class 1 plain, 5 plants, 6 skin), and emissive. Glass is drawn as the game draws it:
   after the frame is shaded, depth written, no AlbedoBuffer colour (the trace meets the pane itself), blended over the frame.
   **The game's frame** is a plain raster shading of that G-buffer, in the stored curve x^(1/2.2): the sun x N.L, a sky ambient, the
   lamps with the game's falloff, all unshadowed, plus the sky gradient of `RtPathTrace.comp`'s `Sky()`. The path tracer reads it for
   pixels it does not trace (flag 32), for `PTGlowFromFrame` and the raster-only bulbs, for `PTDetail`, and for the denoiser's guide.
2. **The trace** (`RtPathTrace.cpp` Record):
   - CopyIn of the G-buffer and the frame.
   - The lamp image: the lamps nearest first, the glow lights (none with PTEmissive), the frame row (`RtRestirRow::Build`: ReSTIR DI
     and radiance cache keys, the GGX energy table; `RtTessContact::PackRow`: PTContactShadows, texels 16-21; texel 22: PTEmissive's
     count and its scratch words; `RtMaterials::PackRow`: PTGlass / PTFoliage / PTSkin / PTClothSheen, texels 24-26), and the emissive
     row 50 (`RtEmissiveRow::Build`; the glowing records' bits 8-15 = their number, `RtScenePick::WithEmissive`).
   - `RtEmissiveBuild.spv` (PTEmissive's list), `RtPathTrace.spv` with the game's flag set (1, 2, 4, 8, 32, 128, 256, 512, 1024, 2048,
     8192, and 262144 / 524288 / 1048576 / 4194304 PTDenoiseSplit where on; pass image 15 = the specular image), then `RtPathTraceCache.spv`.
   - The scratch store is sized as `RtPathTrace.cpp` Setup sizes it (12 words a pixel of reservoirs, the cache's table, PTEmissive's
     blocks; `--cache-log2`, default 20).
3. **CopyOut** of the noisy picture and the first surface's colour; with PTDenoiseSplit also the specular light and its albedo (the
   mean images carry it: r, g | b, roughness).
4. **The denoisers** (`RtDenoise::Run`, RGB mode, as `RtPathTrace.cpp` Show sets them under PTRealtime):
   - diffuse: `RtDenoiseTemporalRgb` (3 targets: the history A / B and the clamped firefly excess), `RtDenoiseFirefly` (the excess
     spread back, PTDenoiseAntiFirefly), then `RtDenoiseAtrousRgb` x2 with steps 1 and 2;
   - specular (PTDenoiseSplit): `RtDenoiseTemporalSpec` (4 targets, the hit-distance / virtual reprojection, MirrorHistory 12), then the
     same firefly and a-trous passes in specular mode. With PTDenoiseMirrorSurface (F11, on by default since 2026-11-09) the trace marks
     smooth pure metals (negative hit distance, the reflected colour divided out); the temporal pass gets the normals as a 10th image
     (a curved mirror's virtual point) and keeps the full history there, the a-trous passes smooth them (bit 4, the raw specular image
     in the guide albedo's slot). docs/next/PT-MIRRORS-2026-11-09.md.

   The settings are PTDenoiseHistory 32, PTDenoiseSigma 1, AntiLag 3, Guide 0.35, HistoryFix 3, MeanVariance 2 and FireflyK 2. The
   push blocks are the game's; `rt_layout_check.py` checks the playground's copies against them.
5. **`RtPathTraceShow`** in PTReplace mode (HDR) onto the scene, with the demodulation, PTDetail, and (split) the specular light x its
   albedo added back. Then an ACES tonemap scales it up
   to the window, and the HUD is drawn over it.

## The room

The room is 1200 x 1000 x 400 units (z up, the game's units) and holds:
- a window in the east wall that the sun shines through, with a chain-link fence card inside it (a real 512x512 texture with alpha,
  so its shadow falls on the floor);
- a skylight gap in the ceiling;
- a tiled floor (a real texture);
- three spheres: diffuse red, gold (rough 0.3, metal), and chrome;
- a wall mirror (metal, smooth);
- a glowing panel (a glow map; PTEmissive samples its triangles);
- a glass pane in the window (record kind glass), a leaf card back-lit by the sun (kind foliage, class 5), a skin "head" (kind skin,
  class 6);
- a pillar, and a cube spinning on a plinth;
- three lamps (warm, a moving blue one, a small one by the mirror) with raster-only bulbs;
- sunlit ground outside with three blocks.

## Adding a scene object

Everything lives in `BuildRoom()` in `pg_scene.h`. Make a mesh and add it:

```cpp
{ Mesh m; m.box({ -50, -50, 0 }, { 50, 50, 120 });          // or m.quad(a, b, c, d, uTiles, vTiles) / m.sphere(radius)
  Object& o = s.add(m, "crate"); o.pos = { 300, -100, 0 };    // s.addInstance(meshIndex, name) reuses a mesh
  o.albedo = { 0.5f, 0.35f, 0.2f };                           // linear
  o.rough = 0.4f; o.metal = 0.0f;                             // optional material rule (record bits 26-31 + the MaterialBuffer)
  o.animate = [](Object& ob, double t) { ob.yaw = float(t); }; }   // optional: moves / turns each frame (J stops it)
```

- **Textures:** add a `MakeTexture(id, size, fn)` to `s.textures` (BGRA8 with sRGB colour, all mips, sent as a real texture), then
  set `o.slot` (a free atlas slot) and `o.diffuse` (+ `o.glowTex` and `o.glow > 0` for a glow map).
- **Cut-outs:** `o.alphaTested = true` takes the alpha from the diffuse texture. Make that texture with `MakeTexture(..., 0.5f)`
  (coverage-preserving mips); box-filtered mips fade thin wires under the test and the card vanishes at a distance.
- **Glow:** a traced object's glow is its glow map x PTGlow (the playground's default 4: the panel is its only traced glow), as the
  path tracer reads it. A raster-only object's glow is its colour x `glow`.
- **Material kinds:** `o.kind = NVR_RT_KIND_GLASS / _FOLIAGE / _SKIN` (the record's kind bits; glass also goes in non-opaque, mask
  0x10) and `o.matClass` for the first hit's MaterialBuffer class (5 plants, 6 skin, 3 cloth).
- **Raster only** (`o.traced = false`): the RT scene lacks it and the game's frame shows it, like the lamp bulbs.
- **Lamps:** the `lamp(...)` helper in `BuildRoom` (position, reach, linear colour, an optional `animate`).
- **Viewpoints:** `s.views`.

## Adding a feature toggle (for features other teams merge)

A new path tracer feature is usually a new flag bit, a frame-row texel, or a setting. Three small edits:

1. **A field in `Settings`** (`playground.cpp`), with the game's default: `int emissiveLights = 1;`.
2. **A line in `toggles()`:** `{ 'Z', "Z", "PTEmissiveLights", &set.emissiveLights, 2 },`. The key, its HUD entry and the screenshot
   labels then come by themselves (pick a free key; `count` > 2 cycles through values).
3. **Its use in `TracePass()`.** Mirror what the team added to `RtPathTrace.cpp` Record:
   - a flag: `if (set.emissiveLights) flags |= 4194304u;`
   - a frame-row value: write it into `row[...]` after `RtRestirRow::Build`
   - a constant: the `PathConstants` field
   - a new pass: `host.AddPass(shNew, images, writeMask, &consts, size, gx, gy)`, with the shader registered in `Init()` beside
     `shPath` (`host.AddShader(spv + "\\RtNew.spv")`) and its name added to `$traceShaders` in `build.ps1`.

For a feature on the D3D9 side (a new denoiser pass), do two things. Load it with
`MakeFrag("RtNewPass", images, pushSize, targets, samplerOffset, pass)` in `Init()` (the same arguments as the game's
`RtDenoise.cpp` Make), and draw it with `DrawFrag(...)` in `Denoise()` or `ShowPass()`. Then add the `.spv` name to `$forkShaders`
in `build.ps1`.

When the game's struct changes (the `PathConstants` block, push blocks), change the copies at the top of `playground.cpp` too.
`tools/nvr-rt-host/rt_layout_check.py` checks the host's copies, not these ones.

## Files

- `playground.cpp`: the app (window, input, raster, the PTRealtime chain, HUD, PNG screenshots, `--demo`).
- `pg_host.h`: the game's side of the RT core: the device, the helper, shared images, the exchange. It is a copy of
  `tools/nvr-rt-host`'s Host with a real window, persistent CopyIn / CopyOut and full instance transforms. `rt_host.cpp` is unchanged.
- `pg_scene.h`: meshes, objects, lamps, textures, `BuildRoom()`.
- `build.ps1`, `run.ps1`: build and run; output goes to `bin\` and `out\` (both git-ignored).

## Limits

- The raster "game's frame" is a stand-in for the game's look: no shadow maps, normal maps, AO or fog. PTDetail therefore has little
  fine shading to add here, and PT off means that raster plus RT sun shadows: no RT reflections and no RT lamp shadows.
- No sky panorama (flag 16), normal atlas, light grid (fewer than 40 lamps), PTRestirGI, first-person body, people or skinned
  meshes, and no displacement.
- The smoothest roughness a traced (bounce) hit can carry is the record's 1/7. The first hit takes the G-buffer's 0.05, as in the game.
- The internal size is fixed at start (`-Size`): the shared images are made for it.
- The mouse is polled (GetAsyncKeyState / GetCursorPos), like the game's overlay under DXVK.
