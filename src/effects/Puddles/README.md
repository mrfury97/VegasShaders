# Puddles (rain puddles, wet ground, wall streaks)

> User rule (2026-11-12): "This codebase is yours: what it's for and how it's built are your call, and you'll keep building on it."
> For this effect: the puddle room decides its direction, design and architecture without asking for approval. Still ask first
> (F:\Claude Brain\CLAUDE.md): pushes, PRs or anything public, permanent deletion of user data, purchases, the user's own tuned
> settings, and reversing an earlier user decision.

## 1. Purpose
Puddles that behave like a modern engine's: they sit where water would gather (dips in the noise field, the land's crevices, flat open ground), grow while it rains and shrink back from their edges after it stops, at a pace set in game hours. Inside a puddle the surface is a flat, near-mirror water layer (IOR 1.33, F0 0.02) that hides the material's bumps, darkens what is under it with depth and ripples with the rain. Around each puddle a ring of soaked ground. On walls and other steep faces open to the rain, water runs down in streaks. Puddles reflect the sky (forward shaders), the scene on screen (Wetness Effects' reflections) and, with the Vulkan reflection passes, ray-traced reflections.

It is an upgrade of Wetness Effects (`src/effects/WetnessEffects`, `Shaders/Includes/Wetness.hlsl`): the same water layer, a new puddle model. It needs `[Shaders.WetnessEffects.Status] Enabled`.

## 2. Design: what the mainstream engines do, and what this effect takes
| Source | Technique | Here |
|---|---|---|
| Lagarde, "Water drop" 2a/2b/3a/3b (Remember Me, DONTNOD) | Two wet states: a porous surface darkens (albedo raised to a power by porosity) and gets glossier; standing water is a separate layer that hides the normal map and gives F0 0.02. Puddles fill the height map's cracks first, then the puddle areas, by a water level compared with the height. Ripples: a ring texture animated by time per drop. Streaks: a scrolling mask on vertical surfaces. | All four. The water level becomes a threshold on a world-space field (noise + the land's height), so puddles fill from the deepest points and dry from their edges. Wet ring = the porous state, puddle = the water layer. Ripples are procedural rings (no texture slot free in the object shaders). Streaks are procedural lanes. |
| UE5 (Landscape / material wetness, Substrate water slab) | Height-blended puddle masks; a water layer with IOR 1.33 over the base; puddle normal flattened toward the vertex normal; ripples as a normal layer. | Height-blend on the land (`HeightAwareness`), IOR 1.33 Fresnel (Wetness.hlsl's F0 0.02 film), normal flattening (`Flatten`). |
| Frostbite (Battlefield, Need for Speed) | Wet roads by roughness and albedo, not a texture swap; screen-space reflections on the wet road; stretched lamp highlights. | Roughness/albedo response; SSR via Wetness Effects' reflections. Stretched highlights: not done (section 9). |
| Far Cry 5 | Top-down depth map of the world for rain occlusion; wetness accumulated over time. | The ortho map (s12 objects, s13 land) was already there (Wetness.hlsl `WetnessOcclusion`); the accumulation is the CPU driver below. |
| RDR2, The Last of Us Part II, Cyberpunk 2077 | Puddle reflections from SSR, falling back to probes; Cyberpunk adds RT reflections on wet roads. | Forward sky reflection, then Wetness Effects' SSR, then the Vulkan reflection pass (SSR + RT): the puddle mask is in `Compute/Reflections.glsl`, so RT reflections see the puddles at mirror roughness. |
| Decima (Horizon) | Procedural rain ripples on puddles, more and faster with heavier rain. | Wetness.hlsl's raindrop rings (`WetRainDrops`), on the land and (Quality 3) on objects, at full strength only inside puddles. |
| Unity HDRP | Wetness as a coat layer: a smooth clear coat over the base, the base darkened by porosity. | The same split: Wetness.hlsl's film is the coat. |

Two things go to the Vulkan path, both because of where they live, not because ps_3_0 ran out:
- The reflection pass (`Compute/Reflections.glsl`) is Vulkan only (DXVK fork). Its puddle mask runs there; the D3D9 reflection effect is unchanged.
- The ray-traced reflections take the puddles' roughness from that mask.

The object, parallax and land pixel shaders keep ps_3_0: the new constants fit in four free registers (section 3) and the code fits the instruction limits (checked offline, section 8).

## 3. Inputs
### Constants (pixel shaders; set by name from `PuddlesShaders`)
| Register | Name | Components |
|---|---|---|
| c174 | TESR_PuddleData | x water level 0-1 (CPU, section 6), y Coverage, z 1 / Size (game units), w Quality (0 = off: Wetness Effects' old puddles) |
| c175 | TESR_PuddleLook | x puddle roughness, y depth darkening, z edge softness (field units), w ripple strength in puddles |
| c165 | TESR_PuddleShape | x slope start (world up of the face), y slope full, z cavity (land height) strength, w wet-ring width |
| c166 | TESR_PuddleStreak | x streak strength x raining, y 1 / streak lane width, z streak speed, w debug view |

c165-c166 are the sun cross-fade's (Shadow.hlsl, SUN_CROSSFADE), which never compiles with Wetness Effects. c174-c175: free in all object pixel shaders (VEGAS_SHADERS.md section 3) and in the land's (c172-c173 lamp RT, c176-c179 blending and macro). Also read by `Effects/WetnessReflections.fx.hlsl`.

Plus Wetness Effects' own: TESR_WetnessData (c146: wetness, puddle wetness, raining, time), the ortho map, the camera.

### Vulkan (Reflections.glsl, uniform block `cb`)
Two vec4 appended after `renderScale`: `puddle` (= TESR_PuddleData) and `puddle2` (x slope start, y slope full, z puddle roughness, w edge softness). Set by `ReflectionsEffect` from `TheShaderManager->Shaders.Puddles`.

## 4. Settings
`[Shaders.Puddles.Status] Enabled` (restart: it picks `WETNESS_EFFECTS=2` for the surface shaders). Everything in `[Shaders.Puddles.Main]` and `.Debug` is live.

| Key | Default | Meaning |
|---|---|---|
| Quality | 2 | 0 off (Wetness Effects' puddles), 1 low (mask, water layer, flattening), 2 medium (+ land height cavities, wet ring, wall streaks, land ripples), 3 high (+ ripples on objects, a third noise octave) |
| Coverage | 0.25 | Share of flat open ground under water at a full water level |
| Size | 420 | Puddle pattern size, game units |
| FillHours | 0.5 | Game hours of steady rain to fill the puddles |
| DryHours | 3.0 | Game hours of daylight to dry them (night and overcast dry at NightDrying of that speed) |
| NightDrying | 0.35 | Drying speed at night, share of the day's |
| Roughness | 0.03 | Water surface roughness |
| Darkening | 0.6 | How much deeper water darkens what is under it |
| EdgeSoftness | 0.05 | Width of the shore, field units |
| SlopeStart / SlopeFull | 0.92 / 0.97 | World up of a face where puddles start / reach full |
| Cavity | 0.6 | Land only: how much the height maps' crevices pull water in |
| WetRing | 0.12 | Width of the soaked ring round each puddle, field units |
| Flatten | 1.0 | How much the water hides the material's normal map |
| Ripples | 1.0 | Rain ripple strength inside puddles |
| Streaks | 1.0 | Wall streak strength while it rains |
| StreakWidth | 6.0 | Streak lane width, game units |
| StreakSpeed | 1.0 | How fast streaks run down |
| MapMode | true | Restart: the large-scale terms from the puddle map on the NVR DXVK extension (section 6b) |
| ReflectionPuddles | true | The Vulkan reflection pass (SSR + RT) mirrors the puddles too |
| `.Debug` LevelOverride | -1 | Force the water level 0-1 (-1 off), for testing in dry weather |
| `.Debug` DebugView | 0 | 1 mask (red puddle, green wet ring, blue streaks), 2 ripples (normal xy), 3 levels (red rain wetness, green water level, blue raining), 4 the streaks' inputs (red open to the sky, green how steep the face is, blue the raw lanes), 5 the puddle map's terms (map mode) |

## 5. Passes and outputs
No pass of its own. The puddle model runs inside `GetWetness` (Includes/Wetness.hlsl) in the object, parallax and land pixel shaders, and in the Vulkan reflection passes' `Reflectivity`. The effect's C++ (`Puddles.cpp`) only works out the water level and fills the constants.

## 6. Behaviour
### Water level (CPU, `PuddlesShaders::UpdateConstants`)
- Rain, as the engine decides it (IDA `Sky::GetIsRaining` 0x59E950): the current weather is rainy (`TESWeather::IsRainy`: data byte 11 flag 4) and the weather percent is past its begin-precipitation byte (6) / 255, or the last weather is rainy and the percent is below its end-precipitation byte (7) / 255. Here as an amount: it ramps from the begin threshold to full, and from full down to the end threshold.
- Game time from `GameDaysPassed * 24 + GameHour` (so waiting and sleeping count; paused menus do not). Steps over 48 hours are taken as a load and ignored.
- Rising: level += rain x hours / FillHours. Falling (no rain): level -= hours / DryHours x lerp(NightDrying, 1, daylight x (1 - cloudiness)). Indoors the level is held (no weather there).
- `.Debug` LevelOverride wins.

### The puddle field (Wetness.hlsl `PuddleField`)
- f = value noise of the world xy at Size (two octaves; three at Quality 3), stretched to 0-1.
- + Cavity x (0.5 - height) (2026-11-12: x1 of a RELATIVE height, was x0.5 of an absolute one), so hollows, cracks and ruts fill first and bumps poke out as islands (Lagarde's flood level). The height: the land's height maps; a land layer without one gives its colour's brightness detail instead (0.6 x (luminance - its last mip's)); objects (no height maps in FNV) the colour texture's detail the same way (`ObjectPuddleHeight`, a mip chosen by distance). The material sorts itself: a flat texture (asphalt, concrete) barely moves the field, so the large noise shapes big irregular puddles; a bumpy one (gravel, mud, rock) gets many small pools and islands. x1.5 broke big puddles on gravel into islands everywhere in the bench, so x1. Limit: brightness is not height (a dark stain fills like a dip).
  2026-11-11: while there is water, each layer's height counts against its own average (its last mip, TerrainTemplate.hlsl), so the
  level a height map sits at overall no longer adds or removes water by texture, and a layer without a height map adds nothing (flat).
  Cost: one cached 1x1-mip read per layer, only while wet. (Seams at the land's quad edges seen by the user at Size 64 come from the
  per-pixel position rebuild of each quad draw, magnified by a small Size; not fixed: keep Size at about 250 or more.)
- The level's threshold t = 1 - level x Coverage. Puddle where f > t, softened over EdgeSoftness. As the level falls, t rises and the shallow edges go first: puddles dry from the edge in.
- x the slope mask on the face's own (geometric) up, x the ortho map's open-sky test, x a distance fade.
- Three zones (2026-11-12, the user's reference pictures): damp ground (the rain: darkening rain^2 x 0.7, roughness lerp(1, 0.22, rain), the normal map KEPT: wet mud glints on its bumps); the soaked rim (f between t - w and t, w = WetRing x (0.35..1.65) by a second noise so the band wanders; squared; darkening 0.85, roughness 0.12, normal kept); standing water (below). The shore softness wanders too (EdgeSoftness x 0.5..1.5).
- Floating bits (land, Quality 2+, standing > 0.3, within 1500 units): one hashed cell per ~6 units, about 1 in 80 holds a speck (dull green) or a leaf (brown), 0.6-1.8 units, laid over the albedo; the water there is not flat (`w.debris`, TerrainTemplate).

### The water layer
- Inside: darkening = 1 + Darkening x depth (the porous darkening of Wetness.hlsl taken further: water absorbs), gloss 1, roughness lerp to Roughness, the film normal = the face normal (no bumps) plus ripples. Murk (2026-11-12): `w.murk` = depth x standing tints what the water reflects toward brown-grey (`PuddleMurkTint`, land sky reflection and lamp highlights). The templates also flatten their own tangent-space normal by `wet.flatten`, so the sun, lamps and ambient see a flat surface too.
- Ripples: Wetness.hlsl's `WetRainDrops` rings (land: Quality 2, objects: Quality 3), at Ripples strength inside puddles.
- Streaks (Quality 2+): faces steeper than about 55 degrees, open to the sky, while it rains: lanes StreakWidth wide, one in three dry, each with drops running down at its own speed (the bright head at the bottom, the tail fading above it). A streak adds to the wall's own rain wetness (darkening and gloss), hides the material's bumps, and its surface bulges (rounded across the lane, the drop's tail sloping up) so it catches the sky as moving glints; a flat film on a wall reflects almost nothing. A wall under an overhang stays dry (user 2026-11-11): the 40-unit look-out of the cover test is kept.

### Lamps on wet ground (2026-11-12, the land)
- Each lamp adds its highlight on the water film (`WetSpecularStretched`: GGX with the half vector's part along the view, projected on the surface, shrunk to 0.3 at full gloss), so the highlight runs long toward the eye: the wet-road streak. x the lamp's RT shadow, x murk.
- The land's outdoor RT lamp-shadow record (TERRAIN_LAMP_RT) has no room for it (32 temps: 20 records no longer fit): `PUDDLES_LEAN` there raises the lamps' own gloss by the wetness instead (PBR: roughness = 1 - gloss; the game's look: power 200), and leaves out debris and the brightness height (height maps only). The objects' outdoor lamp record has its own lean path (no rim noise, no texture height).

### Reflections
- Forward: the film's sky reflection (Wetness.hlsl `WetSkyReflection`, Fresnel F0 0.02).
- `[Shaders.WetnessEffects.Reflections]` (D3D9 SSR): recomputes `GetWetness` from the depth, so it sees the same puddles (noise part; no height map there).
- `[Shaders.Reflections]` Vulkan passes: `ReflectivityOwn` adds the puddle share to the rain wetness on floors and pulls the roughness to the puddle's. With RT reflections on, the rays come from there.
- 2026-11-12 the surfaces' own mask: the land and object templates add the standing water they drew to the material buffer's alpha (`TESR_MaterialBuffer` a = 1 + standing, `PuddleStandingOut`; needs `[Main.Main.Quality] MaterialBuffer`), and `PuddleStanding` (Reflections.glsl) reads it where a template wrote (r > 0.5), so SSR/RT mirror exactly the drawn water (height pooling, cover, debris). Elsewhere (grass, LOD, no buffer) the old noise rebuild. Reflections.cpp binds the buffer for this even with MaterialBufferRoughness off: material2.x bit 1 = roughness override, bit 2 = puddle mask.

## 6b. Puddle map (map mode, WETNESS_EFFECTS 3; 2026-11-12)
Why: the in-shader puddles cost the ps_3_0 surface shaders their temps (the rain-cover test: 16 ortho taps + a 3D Perlin wander;
the field: 2-3 value-noise octaves; the rim wander). That forced lean paths (the land and object outdoor lamp-shadow records) and a
dry lamp record, and 7-layer land had no sampler for the cover map.
- **Mode:** `[Shaders.Puddles.Main] MapMode` (default true, restart) with the NVR DXVK extension: ShaderRecord sends WETNESS_EFFECTS 3
  (`PuddleMap::ModeAtStart`, logged once). Without the extension, or MapMode off: WETNESS_EFFECTS 2, the in-shader path as before.
- **The map** (`PuddleMap.h`, `Compute/PuddleMap.comp`): TESR_PuddleMap, rgba16f 2048 x 1024, two 1024^2 cascades side by side
  (near 4 units a texel = +-2048 round the camera, far 20 units = +-10240), each snapped to its texel grid; built on the extension right
  after the ortho map's draw (ShadowManager), only when a cascade should move (the camera left the middle quarter of its window), the
  camera moved 64 units up or down, Size or Quality changed, or every 8th ortho draw. Per texel: r the field's large-scale part (the
  same value-noise octaves), g the top surface's height (the ortho depth through the inverse ortho transform, one refinement for its
  0.05 tilt, averaged over a 3 x 3 tent of ortho texels at a position jittered by world noise: the old tent + wander), b the rim wander.
- **The surfaces** read it ONCE a pixel (`PuddleMapRead`: the near cascade inside it, else the far one, no blend), at the cover's
  look-out point (steep faces 40 units out): the cover = saturate((its height - the top's) / 16 + 0.5); the field = r + Cavity x relief
  (the per-pixel height maps / colour detail); the wander = b. Everything else stays per pixel (zones, shore, rim, ripples, streaks,
  debris, murk, darker water, lamp highlights). Constants: TESR_PuddleMapNear / Far on the old ortho matrix's registers (c153-c154; land
  lamp record c11-c12). Sampler: the old cover map's (objects s12, land s13; 7-layer land s15, see below).
- **7-layer land:** the GAME binds its seventh normal map to s13 on every land draw whatever the shader declares (measured: the map's
  alpha read 1 there), so 7-layer land takes the map on s15 (free without terrain blending, outside the lamp-shadow record). With
  blending on, or in the 7-layer lamp-shadow record, no stage is left: PUDDLES_NOMAP, the in-shader path WITHOUT the cover (as before
  the map). Not done: a vertex-shader fetch of the map (spare interpolators exist: vcol.w, tanO/binO/nrmO/eyeO.w; vertex samplers s0, s2,
  s3) or the no-limits SPIR-V surface shaders.
- **Lamp records:** the land and object outdoor lamp-shadow records now fit the full puddles (no PUDDLES_LEAN in map mode); the land's
  lamp loop takes the lamp highlight from the lamps' own specular with the wet gloss (no temps left there for a second lobe per lamp,
  even `WetLampFilm`); elsewhere the stretched `WetLampFilm` (land: every lamp; objects: lamps 2-3 of the full pass, OBJ_LAMP2/3).
- **Fallbacks left:** SLS2027 with [PBR] VanillaBridge + MicroShadow: its main record is past the D3DX9 compiler's limit in map mode
  (a register-allocation quirk: it fails even with the map read replaced by a constant) and loads through the lean retry
  (`PUDDLES_LEAN` + `PUDDLES_NOMAP`: the in-shader lean path for that record only); its outdoor lamp-shadow record stays dry (as since
  2026-11-09). Fix: the no-limits SPIR-V route (NvrDxvkReplacements).
- **Checking a record the way the game compiles it:** `tools\landc\landcf.exe` (D3DX9 + the PREFER_FLOW_CONTROL retry); the offline
  template tool's newer compiler fits shaders the game's does not. A record that fails in game leaves `<cache>\<name>.failed.hlsl` (the
  exact source) for it.
- **Debug:** `.Debug` DebugView 5 (map mode): red the map's field, green this point against the map's top surface (0.5 = on it), blue
  the map holds data. The log has the first build's transform and one readback of the texels round the camera.

## 7. Vulkan path
- `Compute/Reflections.glsl`: `PuddleShare(uv, depth, worldNormal)`, the same field without the land's height term (the post pass has no height map), the slope mask and the ortho map's open test (`OpenAbove`). Used in `ReflectivityOwn` (floor guess) and `MaterialReflectivity`.
- Rebuilt: every `Reflections*.frag` that includes Reflections.glsl (glslang 16.6.0).
- No new images: the RT shared-image count is unchanged.

## 8. Look and cost
- Correct: in rain, flat open ground grows irregular puddles from the low spots of the pattern and the land's crevices; puddles are flat mirrors with ripples, darker inside, with a soaked ring; none under roofs or on slopes; walls show running streaks; after the rain the puddles shrink from their edges over DryHours.
- Wrong: puddles on walls or slopes (slope mask); puddles under cover (ortho map); the whole ground under water (Coverage or the level too high); a hard staircase edge (EdgeSoftness); puddles that pop in or out (level not eased).
- Cost: inside the surface shaders, only where wet (branches on the level and the wetness). Measured numbers: section 10.

## 9. Not done / open
- Stretched lamp highlights: done on the land (2026-11-12, above). Objects (roads and slabs that are objects) keep the round GGX of their lamp passes: their lamp loops are spread over many template paths and passes; not done.
- (Fixed 2026-11-09) The outdoor RT lamp-shadow records had no water layer at all, so ground and objects near any light (towns, campfires) stayed dry. They have it now: objects with the puddles' two on c160/c161, the land (WETNESS_ALT_REGS) on c2, c4-c17, c98-c99. The first try (c192+ / c188-c189) sat on registers the DLL writes raw per draw (material rules c197-c199, lamp binding c174-c189) and drew nothing. A record that doesn't fit with the water layer is rebuilt with WETNESS_OFF and keeps its lamp shadows (SLS2027 in game).
- Wall streaks show on open steep faces (NVRAutoTest2's sign post, the puddle2 metal building) and not on the puddle2 shack walls: confirmed 2026-11-11 with DebugView 4, their eave plus the cover lookup's spread (about +-37 units) reaches past the 40-unit look-out (open at 120). Kept on purpose (user: walls under an overhang stay dry). Slanted rain under short eaves (a dry band under the eave, wet below) is not modelled.
- `[Shaders.WetnessEffects.Debug] ShowCover` blue ("no map") on the land = 7-layer land quads (2026-11-12, the puddle4 canopy): s13, the cover map's sampler, holds the seventh layer's normal map there, so that land has no cover test and gets puddles under roofs (the gas-station canopy). 6-layer land is fine (puddle2: green open, red under the car). Fix options: a vertex-shader lookup of the cover map passed down (coarse: one value per land vertex, ~128 units), dropping the 7th layer's normal map for the cover map, or the Vulkan side (a wetness pre-pass writing the cover into the material buffer). Not done.
- Object surfaces have no height map in the shader, so their puddles are noise-shaped only.
- Splashes (WetWorld had them) are not in the forward path.

## 10. Bench (2026-11-08, slim slot C, RTX 4070 Ti SUPER, full RT, rain = Zone weather 0B300012)
- Runs: `nvr-bench/runs/20261108-220055-capture-NVRAutoTest1` (list `lists\puddles-ab.txt`: dry, rain fill at timescale 240, Quality 2/0/3/1, debug views 1-3, drying at timescale 1200) and `20261108-220936-capture-NVRAutoTest2` (`lists\puddles-perf.txt`).
- Log: 0 `REGISTER CLASH`; 2 compile failures, both SKIN2010/2011 in the exterior-lamp records (Skin.hlsl temp limit; the skin template does not include Wetness.hlsl: not this effect's).
- Look: puddles fill the flat open ground from the low spots of the field, are flat sky- and scene-reflecting water (the sign is mirrored by the SSR/RT pass), soaked rings show in the mask, the sign's post gets streaks; after the rain they shrink from the edges and are gone after about 3 game hours. At Coverage 0.35 full puddles flooded most flat ground: default lowered to 0.25.
- Cost: not measurable here. Three slot games shared the GPU, the world scene swung 2.2-7.3 ms in blocks unrelated to the setting; neighbouring Quality 3 / 0 blocks: world scene median 2.7 / 2.8 ms. Retime on a quiet machine.

### 2026-11-09 (slim slot C, the user's own saves puddle1-4: a road near a wreck, a town of shacks, Freeside, a gas-station canopy)
- Lists: `lists/puddles-spot.txt` (dry, rain fill, off, Quality 3, mask, ripples, night, drying), `lists/puddles-lampcheck.txt` (mask with the outdoor lamp records on / off, ShowCover). Rain: Honest Hearts `NVDLC02ZVRainWeather` (02010C78 in the 12-plugin slot); clear `NVWastelandClear` 000FFC88. `setgs fVanityModeAutoDelay 99999` (the idle third-person camera moved the view in long runs).
- Found and fixed: ground and objects near lights got no water layer (above); the lamp-record registers' first try overwritten by raw per-draw writes; SLS2027's lamp record no longer fits (rebuilt dry, lamp shadows kept).
- Final runs (20261109-103940 lampcheck, -104815, -104953, -110013, -110223): 0 REGISTER CLASH, 0 crashes, 0 lamp-shadow records lost; the lampcheck mask is the same with the lamp records on and off. puddle2 ran out of the plugin's 600 s at 5 of 12 pictures (about 5 fps with other slots on the GPU).
- The user's own tuning (toml, 2026-11-08 23:14): Coverage 0.664, WetRing 0.5, Cavity 2, Darkening 2, Ripples 2, StreakSpeed 8; the bench pictures use those.

### 2026-11-11 (slim slot C, puddle2): streaks
- "Streaks not working": the streak mask was 0 on the shack walls. DebugView 4 (new) showed them steep and with lanes but covered; ExteriorForward off changed nothing; a 120-unit look-out made them open. Kept at 40 (overhangs stay dry).
- Where streaks ran they hardly showed: max() against the wall's rain wetness (MinRainWetness 0.65: darkening 0.42, film 0.65) hid most of them, and the drop's bright head sat at the top of its tail. Now additive, bulging (glints) and head-down.
- The user's StreakSpeed 8 (drops at 2-13 m/s, flicker) set to 1.5 with the user's OK.

## 2026-11-12 Who gets standing water
Only the land, by default (user): `ObjectPuddles = false` sends object draws TESR_PuddleData with level 0 (Puddles::SetDrawConstants, per pass from Render.cpp SetShadersHook; ShaderRecordPixel::IsLand = TerrainTemplate). Skinned meshes (actors) and the first-person model are always dry. Objects keep the rain dampness and wall streaks.
