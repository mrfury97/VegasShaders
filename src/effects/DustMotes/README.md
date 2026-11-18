# DustMotes - effect spec

NEW 2026-11-03 (branch claude/dust-motes). FIXED the same day after the first bench (saloon16, int23: nothing visible): see section 10. Seen in game since (bench crops, saloon16); the cost below is still worked out, not measured.

## 1. Purpose

Sparse, tiny specks of dust drifting in the air, visible only where light falls on them: round lamps, and (optionally) in the sun's shafts. A speck between you and a light glints brighter. Specks in unlit air are invisible. The effect is procedural: no particles are placed in the world, so it works in every cell, modded ones included, with no setup.

It only adds light (additive, in linear light, before the tonemap, so the specks are exposed and bloomed with the scene). It never darkens anything.

## 2. When it runs

All of these must hold:
- `[Shaders.DustMotes.Status] Enabled` (default true).
- Indoors `Interiors`, outdoors `Exteriors` (both default true); never under water.
- `Density` and `Brightness` above 0.
- Some light can reach a speck: at least one of the frame's tracked lamps has its reach within `MaxDistance` of the eye, or the sun counts this frame (outdoors, `SunShare` above 0, and the volumetric light's froxel grid was built this frame: `[Shaders.VolumetricLight.Main] Froxels` on and the shafts drawn, which needs the sun's shadows; scaled by the shafts' own fade, so none in rain or with sun shadows off).
- The copy-free effect chain is active (always, in practice). Without it the effect logs once and draws nothing.

Frame order (src/core/EffectChain.cpp, pre-tonemap table): step `DustMotes`, right after `VolumetricLight` (so after the fog and the sun shafts: the specks sit in front of the lit air) and before `Sandstorm`, `GodRays` and the exposure / bloom step.

## 3. How it works

The air is cut into cubic cells of 40 game units (57 cm) that drift slowly with the "air" (about 5 units a second at Drift 1, sideways and rising a little). A cell holds one speck or none: a hash of the cell's number, against `Density`. The speck sits at a hashed spot in the cell (0.2 to 0.8 of the way across on each axis) and wobbles round it (0.13 of the cell each way, at 1, 2 or 3 times a slow clock). So the specks are world-anchored: they do not swim with the camera. The pattern repeats every 64 cells (2560 units) on each axis (the hash is of the cell number modulo 64); the C++ side gives the shader the camera's place in that repeat (in doubles), so every position the shader works with is small and the drift can wrap without any speck jumping.

Pass 0, Specks (half resolution, into `TESR_DustMotesBuffer`, RGBA16F), per pixel:
1. The view from 24 units (closer specks would be big, fast blobs, and the weapon is there) to the first surface or `MaxDistance`, whichever is nearer.
2. Cut to the stretch some light reaches: the union of the up-to-8 lamps' spheres along the view; all of it when the sun counts. Nothing lit on this view: the pixel ends here (most pixels of a room far from lamps).
3. The cells that stretch crosses are visited in order (3D DDA, at most 48). In each cell with a speck: the speck's distance from the view line gives its cover of this pixel, a Gaussian disc whose width is the speck's size on screen held between 0.8 and 1.25 buffer pixels (about 1.6 to 2.5 screen pixels). A speck smaller than that is drawn that wide but dimmer by the ratio of the sizes (not of the areas: that was physically fair but left nearly every speck invisible), so it fades out with distance and does not flicker as it moves across pixels. A speck must be in front of the surface (depth test) and past the near limit. The speck covering the pixel most is kept.
4. That speck is lit: each lamp's colour (linear, times its dimmer) with a falloff to zero at the lamp's reach, `1 - x^2` with x = distance / reach (close to the game's own lamp falloff), through a Henyey-Greenstein phase with g = `Glint` (times 4 pi, so 1 = scattering evenly; capped at 8); plus, when the sun counts, the sun's colour times its lit share of the air at the speck's depth (read from the froxel grid, section 5) times `SunShare` times the same phase towards the sun.
5. Times `Brightness`, the cover, a slow twinkle (0.55 most of the time, a short flash up to 1.75 now and then, per speck), and a fade in past the near limit and out over the last 30 % of `MaxDistance`.
6. Written as the difference it makes to the gamma-encoded HDR frame at this spot: `delinearize(linearize(frame) + light) - frame`. So the light is added in linear light, and the next pass does not need to read the frame.

Pass 1, Composite (full resolution, into the chain's current image in place): the buffer read bilinearly (that is the upsample), pixels with nothing to add discarded (`clip`), the rest added with additive blending (One, One). Colour channels only: the frame's alpha (the material capture's) is kept. No frame copy, no ping-pong: only the specks' own pixels are written.

## 4. Inputs

Textures (D3D9, slot order = declaration order):

| Slot | Name | Holds | Used in |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | the frame (gamma-encoded HDR) | Specks (to work out the difference); unbound in Composite (it is the target) |
| s1 | TESR_DepthBuffer | linear depth | Specks |
| s2 | TESR_VolumetricLightGrid | the volumetric light's froxel grid (running sum of lit air, front to back) | Specks, only while the sun counts |
| s3 | TESR_DustMotesBuffer | this effect's half-resolution result | Composite; unbound in Specks (it is the target) |

Constants of its own (DustMotes.cpp):

| Name | Components |
|---|---|
| TESR_DustMotesData | x Brightness (0 = off this frame), y Size (radius, units), z MaxDistance, w Density |
| TESR_DustMotesLook | x Glint, y cell size (40), z wobble clock (cycles, wraps at 1), w lamps in use |
| TESR_DustMotesOrigin | xyz the camera in the drifting cells' frame (wrapped at 2560), w radians per buffer pixel (from the projection and the drawn rows; RenderScale-aware) |
| TESR_DustMotesSun | x SunShare times the shafts' fade (0 = no sun this frame) |
| TESR_DustMotesLampPosition[8] | xyz camera-relative, w radius (0 = empty) |
| TESR_DustMotesLampColor[8] | rgb linear colour times dimmer |

Read from others: TESR_CameraData, TESR_ViewTransform, TESR_ProjectionTransform, TESR_RenderScale (Depth.hlsl), TESR_SunColor, TESR_SunDirection, and while the sun counts TESR_VolumetricLightData (z density, w grid depth), TESR_VolumetricLightGridLayout, TESR_VolumetricLightGridData.

Lamps: picked on the CPU each frame in the effect's step (after the shadow pass has run GetNearbyLights) from the same list LampHaze and AmbientLight's grid read: every shadow slot's lamp (`ShaderManager::BatchLightPosition / BatchLightColor`, all 64 slots) and the 12 tracked unshadowed ones (`LightPosition`, `LightColor[12..23]`). Their w is the game's reach times `[Shaders.ShadowsInteriors.Main] LightRadiusMult`; the specks divide that back out, so they are lit as far as the lamp lights the walls. A lamp in both lists counts once. A lamp counts if its reach overlaps the drawn distance round the eye and its colour is positive; the 8 that look brightest (linear luminance times how much of the drawn distance they cover) are used. Negative ("darkness") lamps light nothing.

## 5. The sun

Only outdoors, only with `SunShare` above 0 (default 0), and only on frames the volumetric light built its froxel grid. The grid holds, per column (16 x 16 pixels) and slice (64, spaced by the square of the slice number out to the shafts' MaxDistance), the running sum of each slice's lit air weighted by the air's transmittance. The lit share at the speck's depth is that sum over the speck's slice and one either side, divided by what the same slices would hold fully lit. So the specks are sun-lit exactly where the shafts are, at the grid's resolution (soft edges). With the march path instead of the grid (Froxels off), or at a RenderScale below 1 (the grid is not built in rect frames), the sun does not light specks.

Why the sun is off by default: by day nearly all the outdoor air is sunlit, so every pixel's view would be walked the whole `MaxDistance` (the effect's worst cost), and the specks mostly show as glitter when looking towards the sun, against bright surroundings where they add little. Lamps outdoors (street lights, fires, at night) light specks with Exteriors on.

## 6. Settings (`[Shaders.DustMotes.Main]`, all live)

| Key | Default | Range | What it does |
|---|---|---|---|
| Density | 0.5 | 0-1 | chance a 40-unit cell holds a speck (about 2.7 specks per cubic metre at 0.5, 5.4 at 1) |
| Size | 0.15 | 0.01-0.5 | speck radius, game units; mostly sets how far specks stay visible (drawn 1.6 to 2.5 screen pixels wide) |
| Brightness | 1.0 | 0-10 | light scattered, times the lamps' light where the speck is (1 = about a white surface lit by the same lamp, before the phase and twinkle) |
| MaxDistance | 250 | 100-1000 | how far out specks are drawn (fade over the last 30 %); the main cost |
| Drift | 1.0 | 0-5 | drift, wobble and twinkle speed; 0 = frozen |
| Glint | 0.45 | 0-0.9 | forward scattering g |
| SunShare | 0.0 | 0-2 | sun's light on specks outdoors (section 5) |
| Interiors | true | | specks indoors |
| Exteriors | true | | specks outdoors |
| DebugView | 0 | 0-2 | test only: 1 = the half-resolution buffer drawn over the picture (blue: lamp reaches the view crosses, faint blue wherever specks could be, so a blue screen proves the pass reaches the frame; green: share of MaxDistance walked; red: the speck found); 2 = every speck white, lit or not (the walk covers the whole view) |

Fixed in the code (DustMotes.cpp, DustMotes.fx.hlsl): the cell size (40), the near limit (24), the drift direction and speed at Drift 1 (5 units/s), the wobble size (0.13 of a cell) and clock (0.08 cycles/s at Drift 1), the twinkle shape, at most 8 lamps.

No setting needs a restart (the buffer is made at start, at half the screen size, full with Max Quality).

## 7. Cost (estimated, not measured)

Worked out from the compiled shader (D3DX, ps_3_0: Specks about 600 instruction slots in all; the cell loop about 28 slots per cell plus about 50 in cells that hold a speck; the lamp lighting about 200 slots, run only where a speck covers the pixel) and the walk's length. The GPU timer is "Dust motes" in the profiler.

- Specks pass, 1440p (1280 x 720 buffer): a view walked the full 250 units crosses about 1.6 x 250 / 40 + 1, about 11 cells: about 800 slots, about 1500 scalar operations a pixel; over 0.92 M pixels that is about 1.4 G operations, about 0.4 ms on a GTX 1070 (about 3.2 G scalar operations per ms) if EVERY pixel were walked the full distance. Indoors most views end at a wall within a few metres and only the stretch inside a lamp's reach is walked, and pixels with no lamp on their view stop after the lamp test (about 140 slots): expected about 0.1 to 0.25 ms in a lamp-lit room. With SunShare above 0 outdoors by day the worst case applies.
- Composite pass: 9 slots a pixel, one bilinear read of a half-resolution buffer, writes only where specks are: about 0.03 to 0.05 ms at 1440p. No frame copy.
- Nothing when no lamp reaches the drawn distance and the sun does not count (the step returns before any pass); the CPU side is a sort of at most 24 lamps once a frame.

## 8. Edge cases and limits

- Lamps' shadows are not read: a speck inside a lamp's reach is lit even if a wall stands between them. The reach is short and the depth test hides specks behind surfaces, so this shows only as a few specks in a lamp's shaded corner.
- Indoors the sun never lights specks (the froxel grid is an exterior pass); a sunlit interior gets lamp-lit specks only.
- One speck per pixel is lit (the one covering it most); two specks overlapping in one buffer pixel show as one.
- The speck's difference is worked out against the frame at the buffer pixel's centre: on a sharp edge (a bright bulb against a dark wall) a speck's added light can be a little off. It never darkens.
- MaxDistance near 1000 at 1080p: the farthest specks' discs can reach about 2 units, close to the 2.8-unit margin the wobble leaves inside a cell, so a far speck may rarely show a clipped edge.
- First-person weapon: specks nearer than 24 units are not drawn, and the depth test keeps specks behind the weapon hidden.
- RenderScale (FSR / DLSS LowResEffects): the passes draw into the rect like every pre-tonemap effect; the pixel size follows the drawn rows. The sun term is off in rect frames (no grid).
- Under DXVK (with or without the Vegas Shaders extension) the effect draws through D3D9 like any effect with no Vulkan port (no ExtRender), which DXVK translates; nothing extra is needed.

## 9. What correct / wrong looks like

Correct: in a lamp-lit room, a few dozen faint specks hang in the air near the lamps, drifting slowly sideways and up, each wobbling; looking towards a lamp they glint noticeably, with the lamp behind you they are faint. Walking does not drag them along (they are fixed in the air). Away from lamps there are none. Off (Enabled false) the frame is unchanged.

Wrong: specks moving with the camera (the origin or drift is broken); specks in dark air; specks that pop or jump every few seconds (a wrap not hidden); blocky or flickering specks; a visible boundary at MaxDistance; specks drawn over the weapon or through walls; any darkening; anything at all with no lamp near.

Log: every 5 seconds while the effect is on for the cell type, one line `UNOFFICIAL DustMotes: drawn | not drawn (...)`: lamps seen and picked, MaxDistance, Brightness, Density, the sun share, DebugView, the buffer pixel's angle, whether the chain is on, and each picked lamp's distance, reach and linear colour.

## 10. The first bench (2026-11-03): nothing visible, and the fix

Bench slot C, 1920 x 1080, saloon16 and int23, shots off / defaults / strong (Density 1, Brightness 3): the strong shot pixel-identical to off. Debug run (DebugView 1 and 2, the log line above):
- The passes were fine: DebugView 1 showed the lamp reaches over the whole room (the half-resolution pass runs and the full-resolution pass reaches the frame), DebugView 2 showed white specks.
- The specks were almost black. Three factors multiplied: a speck smaller than its minimum width lost light by the square of its size ratio (at 1 to 2 m, 0.06-unit specks kept 5 to 20 %); the lamp falloff `(1 - x^2)^2 / (1 + 4 x^2)` gave a few % in most of a lamp's reach (0.04 at 80 %); and the bilinear upsample spreads a one-buffer-pixel speck over four screen pixels.
- Fix: the light lost goes by the size ratio (not the area ratio), the minimum width is 0.8 buffer pixels, the falloff is `1 - x^2` (about the game's own), Size defaults to 0.15. Also: the lamps now come from the full GetNearbyLights list (all 64 shadow slots + the tracked ones, as LampHaze), with the reach the walls get, instead of the 12-entry TESR_ShadowLightPosition (the debug run logged 30 lamps seen, 8 picked, in saloon16).
- After it (same scenes): saloon16 shows a few subtle warm specks at the defaults and clear glinting ones at strong; int23 (camera looking down at a bright casino floor, short views) shows only a few faint ones even at strong: low contrast against bright surfaces, as expected.

## Idle targets (2026-11-12)
With `[Main.Main.Performance] TargetReleaseSeconds` above 0 (default 20) the texture manager keeps 1 x 1 stand-ins in place of this effect's render targets while it is off (not made at start-up when it starts off; released that many seconds after it is switched off; made again, zero filled, before the frame draws when it is switched on). Which targets and why that is safe here: its buffer is drawn and read only in RenderFrame, behind the switch. A new reader or writer of these targets outside the effect's own switch, or a copy of their pointers kept past a frame, breaks this: then answer false in `TargetParkable` (the header). See `core/TextureManager.h`.
