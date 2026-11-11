# VolumetricLight - notes

The shader's own header (`src/hlsl/NewVegas/Effects/VolumetricLight.fx.hlsl`) describes the passes. This file holds what it does not.

## Where the light is applied

With `Froxels` on (the default) and the volumetric fog running, the fog applies the light grid in its own composite
(`VolumetricFog.fx.hlsl` FogGridLight, the Vulkan `VolumetricFogApply` under the DXVK fork); this effect's composites (technique 0 the
march, 8 CompositeGrid) draw only when the fog did not (`EffectChain.cpp` PreStep_VolumetricLight, `gridApplied`). A fix to how the light
is put on the picture has to go into both.

## TAA jitter and the history (2026-11-09, roadmap track 1 step 2)

`[Shaders.VolumetricLight.Main] TemporalJitterFix` (on; only the half-resolution march's Temporal pass, so only with `Froxels` off): the
march is drawn from this frame's jittered depth and the history from last frame's, so the history is read where the jitter's change puts
it (as AmbientLight's TemporalJitterFix). C++ only: `TemporalBackbone::ShiftReprojection` shifts `TESR_VolumetricLightReproject`, which
the .fx and `Compute/VolumetricLightTemporal.frag` both read; `lastJitter` is kept with `lastView`. Off: as before; 0 without TAA's Jitter.

## The game's MSAA and see-through cutouts

Each pixel's air is read at the one depth the effects get (`TESR_DepthBuffer`). With the game's MSAA and transparency multisampling, a
cutout far off (a pylon's lattice, a bush) covers only part of each pixel's samples (alpha to coverage), but that depth is one sample's:
the nearest under the NVR DXVK fork, sample 0 under stock DXVK and native D3D9. The see-through part then took the air only up to the
cutout, so the cutout read as a solid shape in the light.

- 2026-11-04 (PR #5): this effect's D3D9 composites blended a surface pixel's air toward the whole view's by its exact sky share
  (`TESR_SkyCoverage`, `Includes/MsaaSkyEdge.hlsl` SkyShare). The fog's grid composite and the Vulkan composites did not, and with the
  defaults it is the fog that applies the light.
- 2026-11-05 `[Main.Main.Quality] MsaaEdgeFix` (on by default, live; off = as before): the composites here (`MsaaAirMix`): a surface pixel's
  sky share takes the whole view's air; a pixel that is sky by depth but only partly sky (sample-0 resolves) takes, for its surface share,
  the air up to the nearest surface 1-2 px round it. Ported to the Vulkan composites (`Compute/VolumetricLightComposite.frag`,
  `VolumetricLightCompositeGrid.frag`, `Compute/MsaaSkyEdge.glsl`; binding 3 `TESR_SkyCoverage`, `TESR_GameMultisample` at the end of
  the uniform block). The fog's FogGridLight blends the grid depth instead (see `src/effects/VolumetricFog/README.md`, section 7).
- `[Main.Main.Quality] MsaaEdgeDebugView 1` (test only): these composites show red where the depth says sky, green the exact sky share.

Bench 2026-11-05 (og-desert, `bench/foundations/lists/alphacut-vl.txt`, MaxDistance 15000 so the pylons lie inside the march): with MSAA 4
MsaaEdgeFix on vs off moved the pylon heads by +1.2 (fork) / +1.5 (native) luma, the rest of the frame |diff| 0.07 (noise); without MSAA on
vs off = noise. At the default MaxDistance 5000 the pylons there lie beyond the march and their air is the sky's already.

Left: a cutout against nearer ground (a bush in front of the land a few metres behind) is not helped: the shares only know sky samples, not
the depth of the land behind. The air between them differs little at that range.

## RT fog visibility (2026-11-07, branch claude/ptaaa-fog, `[Shaders.VolumetricLight.RT]`)

**What.** With ray tracing running, the sun's visibility in each froxel grid cell is traced against the RT core's scene instead of read
from the cascades (`MarchShadow`). The cascades end at the shadow distance (6000 units by default, far less sharp past ~1750), hold
alpha-tested cards and the far LOD differently from the RT scene, and disagree with the RT sun shadows / the path tracer near the camera;
the traced air uses the same casters (land, objects, alpha-tested cards with their cut-outs, actors with `Actors`, the far LOD with
FarLandLOD / FarObjectLOD; decals never block). The fog reads the grid (FogVolume), so its sun shading follows too.

**Pieces.** `core/RtFogVisibility.{h,cpp}` (an RT core user, Order 2, registered last), `core/RtFogGrid.h` (the pass constants and the
history rules, plain C++: the RT test host fills the block with the same code), `Compute/Rt/RtFogVisibility.comp` (runs in the helper).
Each frame at the RT point (before the shadow passes) the pass writes one R16F texel per cell in the grid's own layout (8 x 8 tiles of
the 64 slices) into the helper's ping-pong pair; the core copies it into `TESR_RtFogVisibility` (this effect's
`Textures.RtVisibilityTexture`, made in RegisterTextures with the grid). `RenderBuffers` sets `TESR_VolumetricLightRt` from
`RtFogVisibility::Constant`: x 0 = not traced this frame (MarchShadow alone, exactly as before), 1 = traced on this frame's grid,
2 = traced with another MaxDistance (looked up by position); y the RT range, z FadeLength, w the traced MaxDistance.
(2026-11-07 ptaaa-hangfix) A sun ray that is not finite is not traced (the cell counts it lit), and a ray stops after 256 candidates
(cut-outs, decals): guards against GPU hangs, no change with sane data (RT-PATH-TRACING.md section 66).

**Use.** GridLight (Froxels): the cell's own texel replaces its two cascade reads inside the range; over the last FadeLength of the range
(distance from the eye) the cascades blend back in, past it the cascades alone. March (Froxels off, or a RenderScale rect frame):
`SunVisibility(position)` projects each step into the grid (bilinear between columns, linear between slice centres). The Vulkan passes
(`VolumetricLightGridLight.frag`, `VolumetricLightMarch.frag`, `VolumetricLight.glsl` VL_RT) bind the texture at binding 2 and take the
constant at the end of the uniform block.

**The trace.** RaysPerCell (default 2) rays toward the sun per cell (a rank-1 lattice across the column, stratified in depth; one ray =
the cell's centre), each from a point in the air (a point inside a solid meets the back face: shadowed, as the cascades). Every frame the
points move (a per-cell, per-frame shift) and last frame's value is blended in, reprojected through last frame's camera at the cell's
centre (trilinear in the old grid): an even average over the frames since a restart, held at Temporal (0.9) for a still cell and at
TemporalMoving (0.7) for one that moved a whole cell (the resampling blurs; a moving edge lags). The history restarts on a camera jump
over 500 units, a sun turn over ~2.5 degrees, a frame not traced, or a grid change. Range (8000, capped by SceneRadius) bounds the traced
cells; RayLength 20000. (2026-11-07 perf-history) Temporal and TemporalMoving are the weights AT 60 FPS: with `[Main.Main.RayTracing]
DenoiseTimeBased` (default on) the grid uses k^(60 dt), the same fade a second (0.9 -> 0.73, 0.7 -> 0.34 at 20 fps).

**When.** On by default but it only rides along: `Wants` = Enabled, the core Running, outdoors, not underwater, this effect on, and
`RtCore::OthersWant` (another RT effect traces this frame). Without the fork, a ray tracing GPU, the helper or another RT effect: nothing
changes. If the frame's RT record has no pass left (NVR_RT_MAX_PASSES 8) the frame uses the cascades (logged every 600). DebugView 1:
traced, not used (an A/B at the same cost). The REPORT block has its line ("RT fog visibility on ...").

**Cost.** One pass in the helper, columns x rows x 64 x RaysPerCell short rays (TerminateOnFirstHit): 1.8 M at 1440p with 2 rays, plus 8
history reads per cell; two R16F shared images (1280 x 768 at 1440p) and one copy out. Not measured on an RTX GPU yet. On the host's iGPU
the 32 x 32 x 64 grid (131 k rays) is under the noise of the run.

**Proof (RT test host, scene `fogvis`, no game).** A closed room with a window toward the sun, an alpha-tested checker card outside, a
32 x 32 x 64 grid: one fixed ray per cell, 65516 of 65536 cells checked against the CPU's ray (20 on an edge skipped), 0 mismatches
(63 shadowed by the card, 60 lit through its cut-outs, 150 lit in the window's shaft); four fixed rays, 0 mismatches in 65318. Two
jittered rays + the history, still camera: RMS against each cell's sunlit share (4 x 4 x 4 points) 0.0128 vs one frame's 0.0535 (partly
lit cells 0.058 vs 0.248), bias -0.0001. Then 16 frames walking 8 units and turning 0.5 degree a frame: 0.0294 vs one frame there 0.0545;
the control without the reprojection 0.0615. (Temporal 0.95 / no moving cap: 0.0379 moving; 0.75: still 0.0189.)

**Not done / unproven.** Not run in game (the bench iGPU; a tester checks): the look of the shafts and fog, the cost on RTX, the copy's
timing under real frames. Lamp light in the fog (VolumetricFog LampHaze) still has no shadows (a traced visibility for the few brightest
lamps would be the next step). The fog's surface lookup past MaxDistance (FogSunVisibility) and ClassicLook stay the cascades.
