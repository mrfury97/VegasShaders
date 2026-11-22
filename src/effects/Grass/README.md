# Grass

The game's grass, re-shaded: the sun's shadow on the tufts, brightness and night brightness, normal maps with the sun's
highlight, the grass wind and the player's bend, grass shadows, the placed lamps and the flashlight on the grass, and the
optional GPU strand grass. Settings: the `[Shaders.Grass.*]` sections in `defaults.toml` (each key's comment says what it does).

This README is new (2026-11-22) and so far covers only the lamps in full; the other parts are described in the code comments
of `Grass.cpp`, `GpuGrass.cpp` and the shaders below until their sections are written.

Files:
- `Grass.cpp` / `Grass.h` (GrassShaders): constants, settings, the lamp list (`UpdateLights`), the grass's own flashlight beam.
- `GpuGrass.cpp` / `GpuGrass.h`: GPU strand grass (needs the DXVK fork 1.7-grass; off by default).
- Shaders (`src/hlsl/NewVegas/Shaders/`): `GRASS23x000TMS.pso.hlsl` (the grass PS; `GRASS23x000.pso.hlsl` includes it with the
  game's hard alpha cut), `GRASS23x000/001/002/003.vso.hlsl`, `GrassShadowCaster.pso.hlsl`; GPU grass: `Compute/GpuGrass.glsl`.

## Lamps on the grass (`[Shaders.Grass.Lights]`: Enabled, MaxLights 0-8, Strength)

Grass is never in the G-buffer's lit set: the deferred lamp pass does not light it, in the forward grass or in its deferred
record (`DeferredFoliage`). So the grass PS lights itself by up to 8 lamps, in both paths (`GrassLamps`).

What `UpdateLights` hands the shader each frame (`TESR_GrassLightData` c82: x lamps live, y Strength;
`TESR_GrassLightPosition[8]` c83: camera-relative position, w reach; `TESR_GrassLightColor[8]` c91: rgb colour x dimmer):
- every slot cleared first (a frame with fewer lamps leaves none behind; GPU grass copies the slots too);
- the active point lights of the world ShadowSceneNode, taken as ManyLamps' table takes them: not switched off for the view,
  not hidden, not flagged; the deferred flashlight's game lamp left out (the grass has its own beam, `TESR_GrassSpot`);
- reach = the lamp's own `Spec.r`; colour = `Diff` x `Dimmer` (capped at 1 without HDR) x the LOD dimmer;
- nearest first (distance minus reach), at most MaxLights.

In the shader, per lamp: the game's falloff `1 - d^2/r^2` through `LampAtt` (as the objects; with
`[Shaders.PBR.LampFalloff] Exterior/Interior` on, the physical shape), the colour decoded on its own (`PBRLight` per lamp, so
overlapping lamps add in linear light), and a wrapped diffuse `saturate((N.L + 0.35) / 1.35)`. The sum times `LinScale(Strength)`
and the tuft's light scale (`f_amount.x`, 0.25-1, as the game scales every light on a tuft). NightBrightness does not touch lamps.

History: upstream 1.1.2 (commit 6e85afb) "grass lamp blowout", hand-ported 2026-11-22. Before it the lamps came from
`ShadowLightPosition` / `LightPosition` (the shadow radius `Spec.r x LightRadiusMult`, 1.5 by default, and the raw Dimmer: no LOD
dimmer, no HDR cap, lamps the game had switched off included), the sum of all lamps was decoded at once (pow of a sum: blown-out
white where two lamps met), and `abs(N.L)` let a lamp below the ground light the blades from underneath.

What correct looks like: at night by a street light the grass is lit over the same pool as the ground under it, fading out at
the same edge, no brighter where two lamps overlap than the ground there, and dark on the side away from the lamp.
Wrong: a lit disc on the grass half again as wide as the ground's, white patches where lamps overlap, grass lit by a switched-off
or culled lamp, or glowing above a lamp under the terrain.

GPU grass (`Compute/GpuGrass.glsl`, `GG_LAMPS`) still lights its strands with the old per-lamp formula (gamma-space falloff,
`abs` wrap); it gets the new lamp list (true reach, LOD dimmer). Not ported: the strands light in the stored curve, not linear light.

Leaves are not part of this effect, but the same upstream fix touched them: `Includes/Leaf.hlsl` (the leaves' deferred record
with `DeferredFoliageLamps`, `LEAF_FORWARD_PLUS`) now takes each lamp's falloff through `LampAtt` as well.
