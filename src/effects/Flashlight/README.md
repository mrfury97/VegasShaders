# Flashlight

Restructured 2026-11-04. The old post-process flashlight (Flashlight.fx and its Vulkan passes), the FlashlightBeam shaft, the
MaterialLight relight (NewVegas/Hooks/MaterialPass.cpp) and the separate Interiors look are gone; their code and spec are in
VegasShaders-backups/before_flashlight_restructure_20261104.

What it is now:
- `Flashlight.cpp` (FlashlightEffect) draws nothing. Each frame, while the Pip-Boy light's magic effect is on, it places the light (camera,
  head in third person, or the weapon: `AttachToWeapon`, always while aiming), colours it with the game's "Pipboy" system colour (the
  Pip-Boy HUD colour of the display settings) and publishes it as the spot list ShadowManager draws the shadow map for. Its
  `Flashlight.fx` is a stub so the effect loads and keeps its menu switch.
- `core/FlashlightDeferred.h` + `Shaders/FlashlightDeferred.hlsl`: the spot light on the G-buffer (cone, `Falloff`, PCF shadow map,
  cookie), `Dimmer` its brightness and `Distance` its reach.
- `effects/Grass`: the grass's own beam (card and GPU grass) with its shadow; grass also casts into the flashlight's shadow map.
- `core/FlashlightLamp.h` (`GameLight`): the Pip-Boy light moved onto the beam, so the game lights what the G-buffer does not hold, with a
  per-pixel cone in the object shaders; `core/EnvLightFade.h` shapes the env-map shine of light-fade assets per pixel.

Settings: `[Shaders.Flashlight.Main]` Dimmer, Distance, AttachToWeapon; `[Shaders.Flashlight.Deferred]` the light pass's options;
`[Shaders.Flashlight.Status]` Enabled.

## Ray tracing without the path tracer (2026-11-07, perf-flashlight)
No RT lamp-shadow pass traces the flashlight: its game lamp is left out of every NVR lamp list (GetNearbyLights, so no cube slot and no
place in the RT lamp table; ManyLamps' table; the RT reflections' hit lamps; and since perf-flashlight ManyLights' / ExteriorManyLights'
table, PointShadowForward::NoteManyLamp), and the light pass's shadow is its own spot map, which leaves the player out. So
FirstPersonBody, SameFrame and the RT shadow denoiser change nothing on the beam (host scene `flashhead`: a lamp in the hidden head is
traced as if the body were absent anyway). What RT does change: RT reflections replace the Fresnel share of a shiny surface's lit colour,
the beam's light included, with a reflection whose off-screen hits are lit without the flashlight (mirrors, chrome, gold: the spot fades
there). Checks: DebugView 1 / 2 here, `[Shaders.Reflections.RT] Enabled = false` for the reflections. docs/next/RT-PATH-TRACING.md
"Team perf-flashlight".

## Path tracing (2026-11-07)
Under PTRealtime / PTReplace the path tracer lights the picture itself; the flashlight is passed to it as lamp 0 with its cone
(core/RtPathTrace.cpp Lamps, Compute/Rt/RtPathTrace.comp LampSpot; docs/next/RT-PATH-TRACING.md section 53). Before, it showed only on
the few pixels where the path-traced picture falls back to the game's frame.
