# Deferred wetness: design and steps (road map Phase 1c, 2026-11-19, v2)

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

## Design v2 (after the lead-architect review, 2026-11-19: "rethink"; v1's two-pass split is kept below only as what not to do)
**One wetness, worked out once.** A new core pass, DeferredWetness (`core/DeferredWetness.h`, `Shaders/DeferredWetness.hlsl`), right after
`DeferredDepth::AtSpanEnd` and before `DeferredSun::Apply` (the G-buffer is already resolved there: CapturedNormals::EndSpan runs first,
`NewVegas/Hooks/Render.cpp:2916/2970`). For each pixel a deferred record drew (albedo alpha code > 1) of the near land and objects (models 1-4,
NOT the far land: it carries FD_FAR_LAND_MARK, `Includes/DeferredPixel.hlsl:67`, and its forward shader has no wetness), it runs GetWetness
with the SAME defines the land record uses (`TerrainTemplate.hlsl:365-368`, WETNESS_EFFECTS from BuildDefines: map mode, raindrop ripples,
WETNESS_HEIGHT), not the screen effect's PUDDLES_RUNTIME set (which turns map mode, ripples, debris and NormalAwareness off,
`Wetness.hlsl:34,189,548`). It writes:
- COLOR1 rgb: darkened albedo (WetAlbedo, standing water 0.4, debris);
- COLOR2 g/a: the shading normal flattened under standing water;
- COLOR0: the record's ambient made wet: unfog with COLOR1.a's keep and the fog colour, scale by wet / dry albedo, fog again (the
  lighting-free record keeps its ambient in COLOR0 with the dry colour: `Includes/Terrain.hlsl:194/199`, `core/LightFreeRecords.h:5`; without
  this, overcast rain barely darkens the ground);
- a new film target (one A16B16G16R16F at scene size): gloss, roughness, the film normal (octahedral, ripples on), murk.
**The film is lit where the light is:** DeferredSun adds the film's sun highlight (WetSpecular) with its own full shadow; DeferredLamps adds
each lamp's streak (WetLampFilm) in its lamp loop (replacing the screen-pass DeferredWetLampFilm); the Wetness Effects screen pass keeps only
the sky / screen-space reflection of the film, reading the film target (not COLOR2, which A has flattened), for the land only in step 1
(objects still draw their own sky). Its run conditions must not hide the film: the sky term runs even with its reflection Strength 0 or
Reflections off.
**Records:** with the switch on at startup the lighting-free land (later object, parallax) records get `WETNESS_OFF` (the real switch:
`core/ShaderManager.cpp:1278/1323`; `WETNESS_EFFECTS 0` would not remove it: the templates test `defined(WETNESS_EFFECTS)`), through a second
Overrides list in `core/LightFreeRecords.h`.
**The land's height:** GetWetness's `surfaceHeight` (the layers' height maps) drives Cavity and CreviceFill. A has no height unless a record
stores it: candidate, COLOR2.r's metal fraction on land pixels (believed unused for land; every reader of that fraction to be checked).
Without it, bumpy ground loses its small pools.

Cost (estimate, not measured): A ~0.2-0.4 ms at 1080p while wet (reads ~5 targets + depth + puddle map, writes 4); one more fp16 target
(~16 MB at 1080p); DeferredSun / DeferredLamps a few instructions more per lit pixel (ps_3_0 size to be checked).

## Step 1 as built (2026-11-19, branch `deferred-wetness-ground`)
Behind `[Main.Main.Quality] DeferredWetness` (restart, test only, off, in the Internal list). Needs DeferredRenderer 2 (lighting-free records),
GBufferV2 and Wetness Effects at startup. Files: `core/DeferredWetness.h`, `Shaders/DeferredWetness.hlsl`; changes in `TerrainTemplate.hlsl`,
`core/LightFreeRecords.h`, `core/DeferredSun.h` + `Shaders/DeferredSun.hlsl`, `effects/WetnessEffects/WetnessReflections.*` + its `.fx.hlsl`.
- **Records:** the near land's lighting-free records get `DEFERRED_WETNESS` (own cache `LightFreeWet\`; no lean puddle retry): the water layer
  is compiled out (TERRAIN_WET_DEFERRED; `WETNESS_OFF` was not used because the record still needs the height code below). Forward records
  and every record without the define: byte-identical (checked offline, SLS2092 / 2124 / 2146).
- **Height (the open item):** reader audit of COLOR2.r's fraction: it is read as metal (`/ 0.49`) by DeferredSun, DeferredLamps, the
  flashlight, the path tracer and the DLSS guides, and as the far land's mark (1 / 64) by DeferredSun: no free fraction for land. So the
  height is stored **only between the record and pass A**: the record writes r = 4 + 0.99 height (the spec bit 16 taken out, so fp16
  keeps 1 / 256 steps; at 20 it would be 1 / 64), and A writes r = 20 back for every such pixel, wet or dry (a restore-only record in dry
  frames, and a fallback restore on every DeferredSun return that skipped A). The relative-height detail (puddles' water level > 0) is
  computed by the record exactly as the forward layer does.
- **Pass A:** inside DeferredSun::Apply after the linear conversion, before the sun. Reads copies of albedo / material / frame, writes by MRT:
  frame (ambient x wet graded / dry graded albedo, + the film's sky reflection, through the inverse of the stored-curve fog), albedo
  (WetAlbedo, x 0.4 under standing water, debris), material (normal flattened toward COLOR3's smooth normal, r = 20), film target (fp16:
  r round(8 murk) + 0.99 gloss, g roughness, b / a film normal). No depth test; where the depth copy is nearer than the G-buffer
  (hair etc.) the frame is kept. Deviation from v2: the sky reflection is drawn by A, not by the screen pass (A knows the film exactly
  and has no run conditions; the screen pass's SSR swap then takes that sky out where it finds a hit, as for forward pixels).
- **Sun:** DeferredSun `DS_WET_FILM` (a test-only part: dropped with DeferredSkin / Hair / RtSunFirst if the pass does not fit): WetSpecular
  on near land from the film target, x the pass's shadow and the crack share (forward's sunMul). +71 slots (2,375 with skin + hair; 3,323
  with the sun fade).
- **Lamps (2026-11-20):** each lamp's streak on the near land's film is drawn by a new DeferredLamps record, `DL_LAND_FILM_ONLY`
  (`Shaders/DeferredLamps.hlsl`, `core/DeferredLamps.h`), right after the land lamp pass with the same state: the forward land's
  WetLampFilm from A's film target, inside the lamp loop, so each lamp's own shadow covers its streak. In the land pass's own loop it ran
  out of temp registers (X4505), hence its own record (577 slots offline). It draws only where the add is exact (linear frame or the exact
  fog's light target), outdoors, not in debug views or first person, with Puddles on and under `DeferredWetLampFilm`; the screen pass
  then leaves those pixels out (`TESR_WetnessReflectionBump.z`), else it draws the streak as before (unshadowed). Change in behaviour: with
  DeferredWetness on, the land's streak no longer needs Wetness Effects' reflections switched on (the forward land never did).
  Lead-architect review: approve with fixes (the exact-add gate, the DeferredWetLampFilm gate, Puddles-only load, these docs).
  Frame cost of the extra lamp loop over wet land: not measured.
- **Screen pass:** reads A's film (gloss, roughness, normal, murk) for near-land deferred pixels for the streaks and the SSR.
- Offline (2026-11-19, user's toml + DeferredRenderer 2's forced keys, tests/template_defines.py; not the game's exact list): pass A 901
  slots (restore 18), device ok; the 28 land lighting-free records 1,267-2,663 slots with the switch (1,984-3,651 without), all device ok.
- Lead-architect review (2026-11-19): approve with fixes. Applied: (1) the full pass only with DeferredDepth 2's exact view z (mode 2
  forces it); without it restore only, since covered pixels could not be told apart; (2) a failed target or copy latches `Broken()`,
  DeferredSun::Wanted() then refuses and the renderer draws forward (the forward land keeps its water layer); (3) a fully fogged pixel
  (albedo alpha 1.0) is restored too; (4) the wetness runs only inside `[branch] if (near)` (clip does not end a shader); the restore pass
  reads the albedo target itself (one copy less in dry frames); (5) frame write mask 0x7; (9) the restore guard is built before
  DeferredSun's `!Live || !binds` return. Not changed (believed low): blended content over near land (seams, decals) takes the land's
  ratio and sky; with the switch off every land record and DeferredSun still recompile once (the cache compares the preprocessed text,
  line numbers included); under the game's MSAA the late-grass re-resolve can copy the unrestored material back (mode 2 does not run there).
- Not done in step 1: objects / parallax / far land (steps 2-3); the passes' frame cost (not measured).
- Installed for testing 2026-11-19 (test-p1a-bump 90fc395 = the earlier test build + this branch; restore
  `F:\VS-scratch\install-test6-2026-11-19estore.ps1` -> test5). Not run in game yet. What to check: the log lines "DeferredWetness:
  the pass loaded", "wet: the land's water layer drawn by the pass", no "[ERROR] ShaderRecord: more than 62", no LIGHT-FREE RECORD
  warnings; in rain, the switch on vs off side by side (same spot, same weather).

## Steps (v2)
1. **Land:** A (albedo, normal, ambient, film target) + DeferredSun's film highlight + DeferredLamps' streaks + the screen pass's sky for land,
   behind `[Main.Main.Quality] DeferredWetness` (restart, test only, off); the land's lighting-free records with WETNESS_OFF; the height via
   COLOR2.r's fraction if the reader audit allows. Offline: compile A, DeferredSun, DeferredLamps, the land records (size, temps); DLL;
   review; install; rain look against the forward land (switch off). 2-3 sessions.
2. **Objects and parallax** (their film, their sky moved to the screen pass). 1-2 sessions.
3. **One system:** the film also for pixels no deferred record drew (game shaders, LOD, forward frames: WetWorld-style on the lit colour), one
   "Rain wetness" switch, old puddle keys hidden; WetWorld only as the fallback where there is no G-buffer. 1-2 sessions.
4. **Retire** WetWorld and the records' water layer; on by default (owner's word: player-visible, one full recompile). 0.5 session.

## Risks (v2)
- Matching the forward look: the A wetness must equal what the forward record computes beside it (same defines, same inputs).
- ps_3_0 room in DeferredSun / DeferredLamps for the film terms and one more sampler.
- The height channel (see above).
- Fog round trip in COLOR0 (keep codes 1 / 3 / 5 + keep).

## v1, rejected (for the record)
A wrote only albedo and normal, B (the screen pass) added sky, sun and lamps. Gaps found by the review: dry ambient in COLOR0; WETNESS_EFFECTS 0
does not remove the layer; PUDDLES_RUNTIME's different wetness; B reading A's flattened normal; B's run conditions hiding the film; the screen
sun-shadow mask has no cascade shadows (a highlight inside cast shadows); a double sky on objects; far land made wet; Cavity lost.

## Not decided (owner)
- (Answered by the build, not by the owner: the height rides in COLOR2.r only between the record and pass A, so no channel is spent.)
- When to make it the default and retire WetWorld (step 4).
