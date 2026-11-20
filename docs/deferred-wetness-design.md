# Deferred wetness: design and steps (road map Phase 1c, 2026-11-19, v2)

Owner's choice (2026-11-18/19): the deferred wetness pass is the end state; WetWorld is retired only once it covers everything WetWorld
does. Room rule: deferred first. Branch `deferred-wetness` (off `wet-lamp-film`), built on `deferred-wetness-ground`. Paths under `src\`.
State 2026-11-20: steps 1 and 2 run in game (owner: step 2 "Looks right"); step 3 built and installed (second build after the owner's first look), not run in game yet; step 4 open.

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
- Owner's look (2026-11-19/20, Goodsprings pond in rain): puddle reflections grainy. PROVEN by live toggles: the grain is the main Reflections effect, and only with `[Main.Main.Quality] DeferredBumpedNormals` on (its test prototype hands Reflections the bumped normal; `effects/Reflections/Reflections.cpp:32-39`). Reflections also decides standing water from its own noise field (`Compute/Reflections.glsl:217-234`), not from DeferredWetness's puddles, so it mirrors sharply (WetRoughness 0.069) off bumpy grass / dirt. With the switch off the reflection is clean. Not caused by DeferredWetness (believed: the forward land wrote the same flattened normal). Fix belongs in step 3 (one system): Reflections takes the film target's standing water and roughness on deferred land; the bumped-normal prototype needs roughness from normal variance (specular anti-aliasing) before it can be on.
- Installed for testing 2026-11-19 (test-p1a-bump 90fc395 = the earlier test build + this branch; restore
  `F:\VS-scratch\install-test6-2026-11-19
estore.ps1` -> test5). Not run in game yet. What to check: the log lines "DeferredWetness:
  the pass loaded", "wet: the land's water layer drawn by the pass", no "[ERROR] ShaderRecord: more than 62", no LIGHT-FREE RECORD
  warnings; in rain, the switch on vs off side by side (same spot, same weather).

## Step 2 as built (2026-11-20, same branch)
Under the same switch. The object and parallax lighting-free records (ObjectTemplate, ParallaxTemplate) get `DEFERRED_WETNESS` too
(`ObjectTemplate.hlsl` OBJ_WET_DEFERRED, `ParallaxTemplate.hlsl`): no water layer of their own. Pass A takes every lighting-free pixel with a
sun (albedo alpha 1 + keep) of model 1 object, 2 gear, 3 cloth, 5 plants, 8 hair beside the near land, with the objects' own inputs: the face
normal for both GetWetness normals (as forward), no height (forward's came from the colour texture's detail, ObjectPuddles only, off for the
owner: not carried), porosity 1 - sqrt(metal) (the METAL path's), the standing-water darkening, no debris, no murk, the ambient ratio without
the land's grading (Object.hlsl getAmbientLighting takes the albedo itself), the sky reflection. DeferredSun adds the film's sun highlight for
every film pixel (objects with the sun x LIN_SCALE_LIGHT, as ObjectWetFilm); the screen pass reads the film for every film pixel (the target's
g is never 0 where A wrote). Objects' lamp streaks stay in the screen pass (unshadowed): the objects' lamp loop is DeferredLamps' main pass,
which has no temp register left.
Differences from the forward objects (believed small): A is compiled with the land's defines, so wet upward faces of objects get rain rings in
rain (forward: only standing water at Quality 3); the forward parallax had no standing-water darkening (x 0.4); objects lose the colour-texture
height of ObjectPuddles; light-only (AD) records and terms that do not scale with albedo (the env-map pass, barrel heat, metal's EnvBRDF bias) get the ambient ratio too (review: believed small).
Lead-architect review: approve with fixes, applied: objects get the water LEVEL only as forward gave it (0 unless ObjectPuddles, never worn gear or hair: `effects/Puddles/Puddles.cpp` SetDrawConstants; Wetness.hlsl's level now goes through a PUDDLE_LEVEL macro, the same bytecode); the forward skin drawn over a record (COLOR3.a 2.02-3.5) is not made wet; the screen pass's ray takes the smooth normal unless DeferredBumpedNormals is on. Known gap: with DeferredFirstPerson (test, off for the owner) the gun and arms lose their rain dampness (their records have no layer and the pass skips the view model), and the screen pass may read the world's film under them (believed).
Offline (approximate defines): pass A 963 slots, device ok; all 66 eligible object / parallax lighting-free records compile with the switch (594-1,137 slots); sampled records without it and the forward land / object / parallax records byte-identical.

Owner in game 2026-11-20 (install-test9): step 2 "Looks right".

## Step 3 as built (2026-11-20, same branch): one system
Under the same switch (test only, off). Three parts:
- **The Reflections effect reads the film** (Vulkan passes only: `Compute/Reflections.glsl` FilmWetness, `effects/Reflections/Reflections.cpp`
  binding 28 / slot 27 and `cb.film`). In a frame the film covers the whole screen (`DeferredWetness::FilmWhole()`: the fallback below drew, after
  pass A if it ran; review: with the fallback off the film holds pass A's pixels only and the rest would turn dry), the film decides WHERE the
  floors are wet and how much: wet share = saturate(gloss / MaxRainWetness) x RainWetness outdoors (open ground in full rain counts 1, as the
  rain guess did), for the floor guess and MaterialAware's floors alike. The rain guess (Data.y) and the puddle field (PuddleStanding, the
  material buffer's mask) are not used then; a pixel without a film (sky, skin, grass, first person, far land) is dry there. No whole film this
  frame (dry, indoors, the fallback off): as before. The mirror's blur is WetRoughness, at most the film's roughness (a smoother puddle keeps
  its own). First build (install-test10) took the film's roughness itself: owner, 2026-11-20, "dont seem to see any reflections" (PROVEN
  cause from the code and the owner's settings: damp ground lerp(1, 0.22, rain) = 0.61 at MaxRainWetness 0.5, puddles 0.36 = their Puddles
  Roughness, both past the 0.35 where ReflectionsTrace.frag hands over to the light grid; the gloss 0.5 also halved the strength). Fixed in
  the second build (install-test11). Owner on install-test11: "reflections are back to being extremely noisy" (DeferredBumpedNormals on;
  believed: a 0.069 mirror on the bumped normal, whose direction the half-resolution trace cannot average; the variance roughness only
  blurs it away). Third build: on wet pixels the mirror normal leans to the smooth normal (TESR_NormalsBuffer, binding 29 / slot 28) by
  the film's wet share (`Reflections.glsl` ViewNormal): water levels small bumps; dry reflections keep them (install-test12).
  The D3D9 Reflections technique (no NVR DXVK) is unchanged: it keeps its rain guess.
- **Specular anti-aliasing** (`Reflections.glsl` NormalVarianceRoughness; cb.film.z = the normals are FdIndirect's normal-mapped shading
  normals, i.e. DeferredBumpedNormals or DeferredReflections on): alpha^2 + min(2 x 1/(2 pi) x |dn|^2, 0.18) from the right and lower full-
  resolution neighbours (a depth jump over 2 % is not counted), applied in Reflectivity after Mirror, not to water, the weapon or glass-free
  paths that return 0. Under standing water the normal is already flat (pass A flattened COLOR2), so puddles keep their roughness. This is
  tied to the bumped normals, not to DeferredWetness (both are test keys). Believed to remove the grain the owner proved 2026-11-20; not run.
- **The fallback** (`Shaders/DeferredWetness.hlsl` DW_FALLBACK, `core/DeferredWetness.h` Fallback, called first in
  `EffectChain.cpp` PreStep_WetFlashlightSpecularUnderwater, before the Wetness Effects screen pass): one full-screen draw, outdoors in wet
  frames, two targets (the chain's current image in place, read from its own copy; the film target, cleared first when pass A did not run).
  By COLOR1.a: pass A's pixels and the lighting-free pixels it leaves out (1 < a < 4.5) are skipped; forward records (a 1) and "sun kept"
  records (5 + keep) get the film only (their colour has their own layer); unwritten pixels (a 0: the game's own shaders, LOD, trees without
  a record) get the film and the layer on the lit colour: x WetAlbedo(colour) / colour (colour at most 1 stands for the albedo), x 0.4 under
  standing water, + the film's sky reflection, faded out from 0.7 to 1 x 10,240 units (the film's gloss with it). The water level as the
  forward records got it: land (model 4) the real level, worn gear and hair 0, other objects and unwritten pixels only with ObjectPuddles
  (off for the owner). No fog round trip on unwritten pixels (their fog is unknown): believed darker blobs far away in dense rain fog. Skipped: sky, water plane (Reflections' test), first-person
  view model, past 10,240 units. GetWetness with the records' defines, from TESR_DepthBuffer and TESR_NormalsBuffer (view -> world through the
  DeferredDepth frame block's camera axes), no height. No sun highlight and no lamp streaks on unwritten pixels (believed minor in rain).
  Needs the frame and the film at one size and format, no multisampling (one MRT draw): otherwise off for the session, logged. Runs only with
  this frame's G-buffer (AlbedoData / MaterialData / CapturedNormals::Data.x, the last 0 when the effects run after the upscale) and
  DeferredDepth's frame block; under RenderScale it draws the render rect only. The Wetness Effects screen pass
  now takes the film for every pixel with one (was: lighting-free pixels only), so it no longer runs its own lighter wetness (PUDDLES_RUNTIME)
  there; its ray's bumped normal is read only where a record wrote the G-buffer (a 0 decodes as straight up).
- **One switch** (`SettingManager` RainWetnessOneSwitch = the saved DeferredWetness): Wetness Effects' row is called "Rain wetness"; Puddles'
  switch follows it (`Configuration::ApplyRainWetnessSwitch` sets Puddles' saved Enabled when the files are read, SetMenuShaderEnabled sets both,
  SwitchShaderStatus refuses Puddles alone; saved, Puddles keeps that value if DeferredWetness is turned off again); Puddles' row shows no
  switch, WetWorld's none while Rain wetness is on (its screen pass already draws only when Wetness Effects is not live: EffectChain.cpp); `[Shaders.Puddles.Main] ReflectionPuddles` is hidden
  (no effect in film frames; WetRoughness was hidden in the first build, shown again: it is the wet mirror's blur). No default changed: no settings version. Off: the menu as before.
  Settings no pass reads under Rain wetness are hidden as well (2026-11-20, owner's request, `ImGuiManager.cpp` RainWetnessDeadKey):
  WetWorld's look keys (its pass does not draw; the rain tracking's keys stay) and Wetness Effects' five noise-puddle keys while Puddles
  Quality is above 0. The Reflections Hi-Z keys (measured slower, test only) are hidden for everyone unless ShowInternal.
Offline (2026-11-20, the owner's toml defines): pass A 963 slots (unchanged), fallback 762, restore 18, all device ok; WetnessReflections.fx both
variants PASS; the eleven Reflections*.spv rebuilt (an unchanged source rebuilt byte-identical first) and spirv-val clean; DLL built.
Lead-architect review: approve with fixes, applied: (1) the fallback's G-buffer / depth / upscale guards; (2) the water level per model and
ObjectPuddles; (3) Reflections only with the whole film (FilmWhole); (4) the film's gloss faded with the colour; (6) WetWorld's switch hidden
only while Rain wetness is on; (8) no multisampled target; (9) the render rect only. Not changed: (5) fog on unwritten pixels (believed
minor), (10) ReflectionsTraceHiZCount.spv (debug counting build) not rebuilt and the shader-file log's old hash table (says OLD), (11) a
fully fogged near-land pixel (albedo alpha exactly 1) gets the fallback's film over pass A's (invisible under full fog).
Installed for testing 2026-11-20 (install-test10: test-p1a-bump a1307d6 = install-test9 + this step; 16 files: DLL, pdb, defaults,
DeferredWetness.hlsl, WetnessReflections.fx.hlsl, eleven Reflections*.spv; restore `F:\VS-scratch\install-test10-2026-11-20
estore.ps1` -> test9).
Owner ran it 2026-11-20 12:37 (log: "wet", "the fallback drew"): no reflections (above). Second build installed as install-test11
(test-p1a-bump eb572af; DLL, pdb, six Reflections*.spv; restore `F:\VS-scratch\install-test11-2026-11-20
estore.ps1` -> test10), not run yet.
Log lines to look for: "DeferredWetness: the fallback loaded", "the fallback drew", no "the fallback is off this session".
Not done in step 3: the D3D9 Reflections technique; the two screen-space reflections on wet ground (Wetness Effects' SSR, then Reflections'
mix over it, as before step 3) are still two passes; the fallback's and the reflections' frame cost (not measured).

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
