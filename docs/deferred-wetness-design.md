# Deferred wetness: design and steps (road map Phase 1c, 2026-11-19)

Owner's choice (2026-11-18/19): the deferred wetness pass is the end state; WetWorld is retired only once it covers everything WetWorld
does. Room rule: deferred first. Branch `deferred-wetness` (off `wet-lamp-film`). Paths under `src\`. Nothing here is run yet.

## Why
- Wetness Effects + Puddles live inside the land and object shaders (`hlsl/NewVegas/Shaders/Includes/Wetness.hlsl`, compiled into
  TerrainTemplate / ObjectTemplate). They are the biggest optional block there (about 1,400 slots on the 6-layer land, `core/ShaderFallback.h`)
  and keep the land shaders at the ps_3_0 temp-register wall (X4505 with Terrain Blending; measured 2026-11-18: lighting-free land at r29-r31).
- Under the deferred renderer the wet look is only half there: the lighting-free records drop the film's sun highlight (DeferredSun gap) and
  its lamp streaks (fixed by DeferredWetLampFilm, branch `wet-lamp-film`); the sky reflection of the film is still drawn by the record.
- Two wetness systems (WetWorld screen pass, Wetness Effects in the shaders) and two reflection passes on wet ground confuse the menu.

## What the forward land does today (TerrainTemplate.hlsl, proven by reading)
1. `GetWetness(camPos, geoN, wetWorldNormal, surfaceHeight)` (:703): darkening, gloss, roughness, the film normal (flattened under standing
   water, ripples on top), flatten, murk, debris.
2. Surface changes, before lighting: `combinedNormal` flattened by `wet.flatten` (:704-705); `baseColor = WetAlbedo(baseColor, darkening)`,
   `*= lerp(1, 0.4, flatten)`, debris (:707-712).
3. Lighting with the DRY gloss (the layers' glossV / specPow) on the flattened normal and the darkened colour.
4. The film on top, after lighting: the sun's highlight `WetSpecular(wet.normal, ...)` (:730), the sky reflection
   `WetSkyReflection(reflect) * WetReflectance(...) * horizon^2 * PuddleMurkTint` (:731-736), each lamp's streak `WetLampFilm` (:779).
Objects (ObjectTemplate.hlsl ObjectWetFilm, :565; OBJ_PUDDLE_HEIGHT for object puddles, off by default) follow the same split.

## The design: two passes, split as the forward shader splits it
**A. DeferredWetness (new, before the lights: `core/DeferredWetness.h`, `Shaders/DeferredWetness.hlsl`)** at the G-buffer span's end, after
DeferredDepth's copy and before DeferredSun (`NewVegas/Hooks/Render.cpp` DeferredSpan). For each pixel a deferred record drew (albedo alpha
code > 1) of the land and objects (models 1-4): `GetWetness` from the position (G-buffer distance / DeferredDepth), the smooth normal (COLOR3),
the bumped normal (COLOR2 g/a) and the puddle map, as Effects/WetnessReflections.fx.hlsl already does on screen; then writes:
- COLOR1 rgb: the darkened colour (WetAlbedo, the standing water's 0.4, debris), alpha untouched (its codes);
- COLOR2 g/a: the shading normal flattened by `wet.flatten` (what the forward lighting uses); r and b untouched (model, metal, DRY roughness).
Reads copies of COLOR1 / COLOR2 (two StretchRects at scene size, fp16) and writes them back with colour-write masks.
**B. The film, after the lights: Effects/WetnessReflections.fx.hlsl** (the Wetness Effects screen pass), for the same deferred pixels: the
sky reflection of the film where the screen-space ray finds nothing, the hit where it does (today it swaps the forward record's sky for the
hit: for deferred pixels it adds instead), the sun's film highlight (WetSpecular, shadowed by the screen's sun-shadow mask), and the lamp
streaks (DeferredWetLampFilm, done). All fogged with the pixel's keep (DeferredPixel.hlsl FDAddedLightKeep), murk-tinted.
**Records:** with the switch on at startup, the lighting-free land and object records are compiled without the water layer
(`WETNESS_EFFECTS 0` in `core/LightFreeRecords.h` Overrides). The forward records keep it (reflections, other cameras, fading draws).

Cost (estimate, not measured): A one pass reading ~5 fp16 targets + depth + the puddle map, writing two, plus two copies: ~0.2-0.3 ms at
1080p, only while something is wet (Wetness Effects' flag). B adds the sky / sun terms to a pass that already runs. The lighting-free land
and object shaders get shorter (believed: ~1,000+ slots and several temps on the land).

## Steps
1. **A for the land + B's sky and sun terms**, behind `[Main.Main.Quality] DeferredWetness` (restart, test only, off). Offline: compile the
   pass and the WETNESS_EFFECTS 0 land records (size and temps before / after); DLL build; review; install; look in the rain with the
   switch on and off (the forward look is the reference). 2 sessions.
2. **Objects** (wet film on roads, slabs, walls; object puddles if ObjectPuddles). 1 session.
3. **Unify with WetWorld:** B also runs for pixels no deferred record drew (the game's shaders, LOD, forward frames), WetWorld-style
   (darken the lit colour, film on top); then one "Rain wetness" switch, the old puddle keys hidden, WetWorld kept only as the fallback where
   the G-buffer is missing (native MSAA). 1-2 sessions.
4. **Retire** WetWorld and the wetness code in the lighting-free records; turn it on by default. Needs the owner's word (player-visible,
   one full recompile). 0.5 session.

## Risks
- Matching the forward look exactly: darkening curve, puddle rims, ripples; the A pass and the forward record must give the same wetness
  where both draw next to each other (a deferred pixel beside a forward one).
- GetWetness's land height (`surfaceHeight`: the layers' height maps) is not in the G-buffer: A recomputes standing water without it, so
  crevices fill less sharply than in the forward land (CreviceFill, HeightAwareness, Cavity). Options: store the height in a spare channel
  (COLOR2.b holds the land's roughness; COLOR3's fraction codes are full) or accept the softer shape.
- Register room in a core pass that includes Wetness.hlsl (its constants sit at fixed registers in the templates; PUDDLES_RUNTIME's layout
  for effects).
- Order: DeferredSun reads COLOR2's normal; A must finish before it, and the skin / foliage passes must not see changed codes.

## Not decided (owner)
- Whether the softer crevice shape (no land height in A) is acceptable, or worth a G-buffer channel.
- When to make it the default and retire WetWorld (step 4).
