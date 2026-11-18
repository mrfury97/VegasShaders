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

- Hair and eyes (`HairAndEyes`, 2026-11-12): `Shaders/Includes/FlashBeam.hlsl` gives the hair shaders (`SM3002.pso`, `SM3003.pso`, their
  vertex shader `SM3003.vso`) and the eye variant (`ObjectTemplate.hlsl` NVR_EYE) the same beam; see the section below.

Settings: `[Shaders.Flashlight.Main]` Dimmer, Distance, AttachToWeapon; `[Shaders.Flashlight.Deferred]` the light pass's options;
`[Shaders.Flashlight.Status]` Enabled.

## Hair and eyes (2026-11-12, `[Shaders.Flashlight.Deferred] HairAndEyes`, default on, live)
Why: the game's own hair, brows and beards are blended without an alpha test, so the game draws them in its sorted blended list,
after the main list and so after the light pass, through none of the per-object hooks. The pass never lights them, and the game lamp
reached them unshaped (full strength, no cone, no shadow, on every head in its reach) or not at all (GameLight off, the lamp not among
the hair's three lamps, path tracing). Eyeballs do write records, and the pass lit them like a smooth object: a flat veil in the
light's colour over iris and pupil.
What it does (`core/FlashlightDeferred.h` BeamBegin / BeamShow, constants `TESR_FlashBeam[5]`):
- the hair and eye shaders add the beam themselves: reach, Falloff, cone, NearFade, HotspotLimit, the shadow map (16 point taps) and
  for hair the lens pattern (Cookie); hair with its own lamp terms (diffuse + strand highlight), the eye with its wrapped diffuse;
- the hair shader leaves the flashlight's game lamp out of its lamps while the beam is on (the lamp in the beam's own direction);
- the eye marks its record's distance as "far" (60000), which the pass skips by its reach test; hair drawn in the main list
  (alpha-tested hair of a hair mod) writes the same mark over the record under it (`core/CapturedNormals.h` BeamMarkers);
- the beam is there from the main list's start to the end of the world's pass; never in reflections, the first-person pass, a
  scope's camera, with the light or the key off, without the G-buffer, or while path tracing replaces the picture.
Not done: the eye has no lens pattern; SoftEdges (the pass's wrap) is not applied to hair or eyes; under the game's MSAA (DXVK) a
marked pixel next to an unmarked one resolves to "far" as a whole (one pixel of rim, not checked in game); nothing changes under path
tracing. Log: `UNOFFICIAL FlashlightDeferred: HairAndEyes on ...` once, `CapturedNormals: HairAndEyes: ...` with the 600-frame lines
(hair shader binds before / after the pass, marks, eye draws).
Correct: hair in the beam is lit and shadowed like the head it sits on, dark outside the cone; an eye keeps its iris and pupil.
Wrong: hair twice as bright as the skin beside it (the game lamp was not left out), a black ring round an eye, unlit eyes.

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
