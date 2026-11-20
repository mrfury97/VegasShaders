# Wetness Effects (rain wetness)

## 1. What it does
Ported from Community Shaders' Wetness Effects (through Physically-Based-Reloaded): in the rain surfaces open to the sky darken and gain a
glossy water film with its own highlight and reflection, drops ripple on flat wet ground, nothing gets wet under cover (the top-down ortho
map). Puddles (`src/effects/Puddles`) builds on it for the standing water. Since 2026-11-21 it is THE rain wetness, on by default:
WetWorld, the old screen-space wet pass, is removed (owner's word; `docs/deferred-wetness-design.md` step 4). It is a shader collection, not
a pass: the water layer is `Shaders/Includes/Wetness.hlsl` inside the object, parallax and land pixel shaders (and, with
`[Main.Main.Quality] DeferredWetness`, the deferred wetness pass, `core/DeferredWetness.h`).

This folder also holds HudRaindrops (drops on the first-person arms, weapon and objects) and WetnessReflections (its screen-space
reflections on wet surfaces; off by default since 2026-11-21, the owner's setting).

## 2. The one rain clock (2026-11-21)
`WetnessEffectsShaders::TickWeather` follows the weather as Community Shaders does (the current rainy weather fading in over the
transition, the last one fading out) and eases three amounts at WeatherTransitionSpeed / 60 a second: Wetness (the film; about 20 s at
3), PuddleWetness (three times slower to dry), Raining (four times faster). Interiors hold the state they were entered with.
ForceRain counts as full rain, ForceSnow as none. The Debug overrides force the amounts.

It runs every rendered frame whether or not the collection is on (`core/RainState.h` calls it once a frame, by FrameNumber), and the
rest of the mod reads it there: `RainState::Rain()` (Raining: the drops, the water shaders' rain rings through TESR_WetnessData.z,
Reflections' wetness, Specular's rain look, the volumetric light) and `RainState::Factor()` (max(Raining, Wetness): the Rain setting
groups of the land and the objects, skin, the heat haze). So the ground, skin, heat haze and ripples get wet and dry together.
Puddles' water level keeps its own game-hour clock (FillHours / DryHours): the water left after the film has dried.

## 3. Inputs and constants
TESR_WetnessData (x Wetness, y PuddleWetness, z Raining, w time for the drop hashes), TESR_WetnessParams / Params2 (the Main keys),
TESR_WetnessDrops / Ripple (the Raindrops keys), TESR_WetnessCamera, TESR_WetnessFlags (x on, z ripples, w ortho texel size),
TESR_WetnessSun (w: the ShowCover view), TESR_WetnessHeight. The ortho map (rain cover) is asked for while anything is wet.

## 4. Settings
`[Shaders.WetnessEffects.Status] Enabled` (restart; default true since 2026-11-21, settings step 91). `.Main`, `.Raindrops`, `.Debug`,
`.HudRaindrops`, `.Reflections`: see `defaults.toml` (the shipped values are the owner's: MaxRainWetness 0.5, RaindropChance 0.5,
Reflections off with EdgeFade 0.279, RoughnessCutoff 0.434, Thickness 50).

## 5. Edge cases
- ShaderFallback (native Direct3D 9, land shaders over the 4096-slot limit): Wetness Effects is left out of every family and switched off
  for the session. Since WetWorld's removal nothing draws wet ground in its place; a [WARNING] in the log says to use DXVK. The clock still
  runs, so drops, water rings and the Rain groups still follow the rain.
- The collection off (player's choice): no wet surfaces; the clock still runs for the rest.

## 6. Vulkan path
None of its own: the film is in the D3D9 surface shaders; the Vulkan reflection passes read Puddles' and the deferred film's data.

## 7. What correct and wrong look like
Correct: when rain starts, the ground, roads, rocks, skin and gear darken and gloss together, ripples start on puddles and water, the heat
haze goes; when it stops, all of them dry within about 20 s except the puddles, which shrink over game hours. Wrong: skin or the land's
Rain look staying wet while the ground is dry (two clocks), or wet ground under roofs.
