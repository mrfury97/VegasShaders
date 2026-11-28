# Design docs

Design docs and studies, one section per topic, in reading order: how the renderer is split into deferred and forward, the road map for
moving the rest to deferred, the wetness and reflection designs built on it, the open audit, and two compatibility notes. Each section keeps
the text of the file it replaced (its original title, with its date, is the italic line under the heading). Technical reference (file map, DXVK input, G-buffer
facts) is in [REFERENCE.md](REFERENCE.md); per-effect notes are in [EFFECTS.md](EFFECTS.md).

## Contents

- [Deferred vs forward](#deferred-vs-forward)
- [Deferred roadmap](#deferred-roadmap)
- [Deferred wetness design](#deferred-wetness-design)
- [Unified reflections design](#unified-reflections-design)
- [Reflection speckle](#reflection-speckle)
- [Old fork reflections (NVR-Unofficial-Optimized)](#old-fork-reflections-nvr-unofficial-optimized)
- [SSFX rain comparison](#ssfx-rain-comparison)
- [Pipeline audit](#pipeline-audit)
- [SMP compatibility](#smp-compatibility)
- [Community Shaders ports](#community-shaders-ports)
- [Upstream 1.1.1-1.1.3 merge](#upstream-111-113-merge)
- [Upstream 1.1.5 merge](#upstream-115-merge)
- [Deferred shadow quality](#deferred-shadow-quality)
- [The Astral Shaders study](#the-astral-shaders-study)
- [The FNVGodot-Bridge study](#the-fnvgodot-bridge-study)

## Deferred vs forward

*Original title: Deferred vs forward in Vegas Shaders 1.1.0 (2026-11-18)*

How upstream 1.1.0's deferred renderer works next to the old forward shaders, read from the code on `vs-1.1.0-plus-fork`
(= upstream 1.1.0 + small fork items; the renderer is upstream's, untouched). Paths are under `src\`. Nothing here was run in game.
The `docs/next/*.md` files the code cites (results, plans, measurements) are not in either repo: anything that rests only on them is
marked "claimed".

### In short
The world's opaque surfaces (objects, parallax, land, far land, skin, alpha-tested hair, near grass, leaves) no longer light themselves.
They draw once into a G-buffer and two full-screen passes add the sun and every lamp afterwards. Water, sky, particles, decals, the game's
own non-template shaders, first person and anything blended or fading still draw forward. Glass and blended hair get "forward+": still
forward, but lit by every lamp from the same lamp table. `DeferredRenderer 0` turns all of it off and gives the old forward drawing.

### Where things are
- Switch: `[Main.Main.Quality] DeferredRenderer` (0 off, 1 lamps deferred indoors, 2 sun + lamps deferred everywhere; default 2;
  restart). Defaults text: `resource/VegasShaders.defaults.toml:677`.
- Forcing: `core/SettingForce.h:19-46`, a read-only overlay on setting reads; nothing is written to the player's file.
- Record choice: `core/DeferredRenderer.h` (Eligible :116-130, Choose :190-202, pass skipping :226-240, blended swap :250-277, refusals
  in BeginFrame ~:341-376); per-draw order `core/ShaderRecord.cpp:1330-1360`.
- G-buffer: `core/CapturedNormals.h`, `hlsl/NewVegas/Shaders/Includes/CapturedNormals.hlsl`, `Includes/DeferredPixel.hlsl`;
  depth `core/DeferredDepth.h`.
- Light passes: `core/DeferredSun.h` + `Shaders/DeferredSun.hlsl`, `core/DeferredLamps.h` + `Shaders/DeferredLamps.hlsl`,
  `core/DeferredSkin.h` (+ `Shaders/DeferredSkinSSS.hlsl`), `core/FlashlightDeferred.h`, `core/DeferredFog.h`.
- Families: `core/DeferredSkin.h`, `DeferredFoliage.h`, `DeferredDistant.h`, `DeferredTransparents.h`, `DeferredFirstPerson.h`,
  `LightFreeRecords.h`; forward+ `ForwardPlusHeader.h`, `Includes/ForwardPlus.hlsl`, `HairForwardPlus.hlsl`.

### Proven (from the code)

#### What the switch does
- Read once at the first settings read, clamped 0-2 (`core/SettingManager.cpp:1437-1449`).
- Mode 1 and 2 force on (`SettingForce.h:20-32`): CapturedNormals, MaterialBuffer, AlbedoBuffer, GBufferV2, linear lighting and
  LinearScene, FoldFog, the lamp table (LampTableStandalone, LampTableFast), forward lamp shadows + the shadow atlas, the deferred lamp
  pass with every lamp. Mode 2 adds (:33-46) LightFreeRecords, DeferredFoliage, DeferredDistant, DeferredDepth 2, and forces
  `[Shaders.Terrain.Blending] Smooth` to 0 (the dither instead of an alpha blend on land). So the defaults file's `false` for GBufferV2,
  DeferredFoliage and DeferredDistant does not decide anything while the renderer is on.
- Falls back to forward for the frame, with a log line: the game's MSAA on native D3D9, a missing lamp pass or G-buffer target, the
  lockpick menu, and in mode 2 a sun pass that cannot run (`DeferredRenderer.h` BeginFrame). The game's MSAA is turned off while the mode
  is above 0 unless `[Main.Main.Misc] ForceMSAA` is true (then the old drawing is used) (`NewVegas/Hooks/Settings.cpp:67-68`).
- `DeferredRendererLive` (live, test) is an A/B: false = every draw forward with the prerequisites still on (`DeferredRenderer.h:24-25`).

#### The G-buffer
Three extra `D3DFMT_A16B16G16R16F` targets at scene size, MRT slots 1-3, beside the scene target in slot 0 (linear light under LinearScene)
(`CapturedNormals.h:807,813,826,853,1306,1323`). Layout v2 (forced on):
- COLOR1 albedo: rgb linear base colour (texture x vertex colour); a = written mark, or in lighting-free records a code (1 + keep with sun,
  3 + keep no sun, 5 + keep "sun kept"), keep = 1 - fog.
- COLOR2 material: r = shading model (1 object, 2 gear, 3 cloth, 4 land, 5 plants, 6 skin, 7 grass, 8 hair, 9 eye, 10 unlit) + 16 x
  specular bit + 0.49 x metal; g and a = the normal-mapped shading normal (octahedral; foliage packs a fog keep into g); b = roughness.
- COLOR3 smooth normal: rg = octahedral vertex normal facing the camera; b = camera distance; a = 1 world / 2 actor (+ a sun-visibility
  and highlight-F0 code in the fraction, `DeferredPixel.hlsl` FDSunVisCode).
- Depth: an INTZ copy and, in mode 2, an R32F view-z texture (`DeferredDepth.h:1-30`). No motion vectors in the G-buffer (none found).

#### Which pass fills it, and how lighting is skipped
- The main opaque list is wrapped by `GrassFirst::MainListHook` (`NewVegas/Hooks/Render.cpp:2894`, patched at :3157);
  `CapturedNormals::BeginSpan` binds the targets for its span. Only Vegas Shaders' own templates write them; other draws are masked off.
- The templates are compiled with `CAPTURED_NORMALS_ON` (4 under v2, `ShaderRecord.cpp:399-401`). Inside the span an eligible draw
  binds a deferred record instead: mode 1 `LAMPS_DEFERRED 3` (lamps compiled out, sun kept), mode 2 `LAMPS_DEFERRED 2` (sun, lamps and
  their shadows compiled out). Ambient, glow, emittance and fog stay in the record. Forward lighting is not computed and thrown away:
  it is compiled out, and the game's added lamp and highlight passes for that object are skipped (`DeferredRenderer.h:19-21,226-240`).
  The texture-multiply and fog pass of a light-only (AD) base stay forward.
- A blended or fading base pass takes the forward record and keeps its own lamp passes (`DeferredRenderer.h:250-277`).

#### Where lighting is resolved
At the end of the main list, after the G-buffer is resolved and before LinearScene encodes the frame, in this order
(`Render.cpp:2913-2917`): depth copy, DeferredRenderer bookkeeping, skin prep, **DeferredSun** (one full-screen pass: the templates' own
sun term x their own shadow lookup: cascades, contact hardening, character map, cloud shadows, RT sun shadow where it applies; x (1 - fog);
added ONE/ONE in linear light), **DeferredLamps** (one full-screen pass over the lamp table's froxel lists, the objects' own lamp maths,
atlas shadows; a separate skin lamp pass), **FlashlightDeferred** (the beam on the G-buffer), skin scattering (`DeferredSkinSSS 2`, a
screen-space Burley-profile blur). Fog is rebuilt in the passes (`DeferredFog.h`); with VolumetricFog on (default) the game draws no fog
and no frame copy is needed.

#### Family by family, with shipped defaults (mode 2)
| Family | Path | Switch | Notes |
|---|---|---|---|
| Statics / objects, parallax (ObjectTemplate, ParallaxTemplate) | deferred | DeferredRenderer | not the DIFFUSE / ONLY_SPECULAR / NO_LIGHT passes (`DeferredRenderer.h:116-130`) |
| Armour, clothes (SKIN variants of ObjectTemplate) | deferred | DeferredRenderer | as objects |
| Shiny objects' cube-map layer (EnvTemplate) | forward | - | drawn over the deferred base, scaled by ambient and sky |
| Near land (TerrainTemplate) | deferred | mode 2 only | mode 1: forward |
| Far land, LOD objects | deferred | DeferredDistant (forced) | drawn inside the span (`DeferredDistant.h:5-24`) |
| Distant trees (game's DISTTREE shader) | forward | - | masked off, drawn at their depth (`DeferredDistant.h:24-25`) |
| Skin (SkinTemplate base passes) | deferred | DeferredSkin (on) | own chooser `ShaderRecord.cpp:1336`; fading faces, light-only and added-lamp skin passes stay forward (`DeferredSkin.h:10-24`). The `DeferredRenderer.h` header still says skin stays forward: stale, it predates DeferredSkin |
| Hair, alpha-tested | deferred | DeferredHair (on) | |
| Hair, blended (SM3002/3003) | forward+ | DeferredHair + DeferredHairLive | every lamp of the froxel (`DeferredTransparents.h:135-160`) |
| Near grass | deferred | DeferredFoliage (forced) | grass drawn after the list stays forward (`DeferredFoliage.h:20-22`) |
| SpeedTree leaves | deferred, sun only | DeferredFoliage (forced) | the lamp pass does not light them; they keep the game's one point lamp unless DeferredFoliageLamps (off) (`DeferredFoliage.h:24-31`) |
| Glass and other blended lit objects | forward+ | DeferredTransparents 1 | every lamp of the froxel from the lamp table + atlas shadows + flashlight; needs the table made that frame, else the game's few lamps (`DeferredTransparents.h:1-45,169-231`) |
| First-person arms and weapon | forward | DeferredFirstPerson (off) | |
| Water, sky and clouds, particles | forward | - | "drawn the old way in every mode" (defaults :677) |
| Decals (bullet holes SM300x, GDECAL blood), unlit NO_LIGHT objects | forward | - | game shaders / no record |
| Menus, Pip-Boy screen, other cameras | forward | - | the span opens only at main-target size in the world context |

#### The forward shaders (still used by everything above marked forward, and by everything in mode 0)
- Built from `Shaders/ObjectTemplate.hlsl` variants (ADTS full pass, AD light-only, DIFFUSE extra lamps, ONLY_SPECULAR highlights;
  LIGHTS 2/3/4/9), plus Terrain, Skin, Parallax, TerrainLOD/Fade and Env templates. The game's pass list decides the lamps per draw: a
  shiny object gets one pass only up to 2+f lamps, else 5-7 draws (light-only pass with 2 lamps, extra passes of 2-3 lamps, texture
  multiply, highlight passes, fog pass) (`core/LampsInOnePass.h:8-20`).
- What Vegas Shaders adds to them: PBR (GGX; the VanillaBridge maps the game's Blinn-Phong onto it), linear lighting and LinearScene, a
  sky-ambient term and metal, the sun-shadow cascade lookup inside every lit shader (`FORWARD_SHADOWS`, `Includes/Shadow.hlsl`), per-lamp
  cube or atlas shadows indoors (`PointShadowForward.h`), fixed multi-pass sums, and more lamps per object: MoreLampsPerObject (on, the
  game's cap 3+f -> 6+f), ManyLampsOnePass (on, DXVK, 13-24 lamps in one pass), LandAllLamps (off, 24 lamps). Fog is the game's
  per-vertex fog, except that VolumetricFog (on) makes the game draw none.
- Main constants: `TESR_PBRData` c32, `TESR_PBRExtraData` c33, metal c34/c36, `TESR_LinearLighting` c191, `TESR_ShadowData` c128 and
  the cascade matrices c108-c127, lamp shadows c174-c190, plus the frame-wide `TESR_Sun*`, `TESR_Fog*`, `TESR_Camera*` and transforms
  (`effects/PBR/PBR.cpp:8-26`, `effects/ShadowsExteriors/ShadowsExterior.cpp:833-889`, `core/ShaderManager.cpp:183-245`).

#### Screen effects in the two modes
- The G-buffer exists in mode 0 too: CapturedNormals, MaterialBuffer and AlbedoBuffer default on by their own keys (defaults :653-673),
  and the captured normals are merged into `TESR_NormalsBuffer` for every reader (`ShaderManager.cpp:2308-2310`). So AO, AmbientLight
  bounce, Reflections, Snow, WetWorld, BloodFlow, the deferred flashlight and DLSS RR guides read it in both modes.
- Opt-ins that use the v2 shading normal or G-buffer colour, all off: DeferredReflections, DeferredRRGuides, DeferredALAlbedo.
- Sun shadows: looked up in the lit shaders (forward) or in DeferredSun (mode 2); the screen-space contact shadows and the RT sun apply run
  in the effect chain in both modes (`EffectChain.cpp:462-495`).
- VolumetricLight, VolumetricFog and TAA have no DeferredRenderer branch in their C++ (grep).

#### Surface normals (added 2026-11-18)
Two normals per pixel, both in camera-relative world axes, octahedral (`Includes/Shadow.hlsl:959-970`), at full fp16:
- **Smooth normal**, COLOR3 = `TESR_CapturedNormalsBuffer` rg: the interpolated vertex normal (after skinning), before the normal map,
  turned to the side the camera sees (`Includes/CapturedNormals.hlsl:1-31`); objects and parallax turn it by the triangle's side instead
  (`:33-59`, the roof-ridge fix). b = camera distance (the light passes rebuild the position from it), a = 1 world / 2 actor + codes.
- **Shading normal** (normal-mapped), COLOR2 = `TESR_MaterialBuffer` g and a under GBufferV2 (forced on by the renderer); g = 4 means
  "none here, use the smooth one" (`CapturedNormals.hlsl:88-124`).
- Both are written while the main opaque list draws, by Vegas Shaders' own templates and the grass / leaf shaders only (other draws are
  masked off).

Who writes what (mode 2):
| Geometry | Smooth normal (COLOR3) | Shading normal (COLOR2) |
|---|---|---|
| Objects, armour, clothes, hair: LIGHTS 2/3 family | vertex normal (`ObjectTemplate.hlsl:2258-2270`) | the normal map through a tangent frame rebuilt from the screen derivatives of position and uv (Schuler 2013); two-sided cards seen from behind store the front's normal (`:1697-1701`, `CapturedNormals.hlsl:126-165`) |
| Objects: many-lamp table family (LIGHTS 4+) | vertex normal (`:2849-2863`) | exact, from the vertex's own tangent rows (`:2641-2645`); that family's other records use the derivative frame (`:2647`) |
| Parallax | vertex normal, triangle-faced (`ParallaxTemplate.hlsl:1016`) | derivative frame on the parallax normal map (`:840`) |
| Near land | vertex normal (`TerrainTemplate.hlsl:884`) | the blended layers' normal map through the land's TBN, after standing water flattens it, as the forward lighting uses it (`:847-850`; until 2026-11-18 the wet-world normal from before the flatten: bumps showed through puddles in the deferred light) |
| Far land (TerrainLODTemplate) | geometric normal (`TerrainLODTemplate.hlsl:259`) | the LOD normal map (`:234`) |
| Skin | vertex normal after skinning (`SkinTemplate.hlsl:608`) | the skin normal map, into the same COLOR2 (`:633-634`) |
| SpeedTree leaves | the card's flat face normal (`Includes/Leaf.hlsl:57`, `Shadow.hlsl:916-920`) | none |
| Near grass | the grass vertex's lighting normal, bent toward the camera (`GRASS23x000TMS.pso.hlsl:267-269,306`) | none (g holds the grass mark) |
| Slot-tight six-lamp shader (SLS2035) | the flat face normal (`ObjectTemplate.hlsl:2850-2855`) | as its family |
| A draw whose vertex shader is the game's own (no world position) | as above | none: lit with the smooth normal (`ObjectTemplate.hlsl:1699-1701`) |
| Water, sky, particles, decals, distant trees, first person, glass, game shaders | nothing | nothing |

Who reads which:
- **DeferredSun, DeferredLamps, FlashlightDeferred:** the shading normal where one is stored, else the smooth one (`DeferredSun.hlsl:151-159,231`,
  `DeferredLamps.hlsl:133-144`). Their shadow lookups use the smooth normal, view-faced, for the normal offset (`DeferredSun.hlsl:238-240,311-313`,
  `DeferredLamps.hlsl:214,267`); grass takes world up there.
- **Every screen effect reads `TESR_NormalsBuffer`**, which is not the G-buffer: the Normals effect rebuilds a view-space normal from depth
  (one flat normal per triangle, `Effects/Normals.fx.hlsl:1-4`), then the merge copies in the **smooth** normal wherever the pixel's
  surface wrote one, its distance matches the depth, and it is within 60 degrees of the depth facet (`ShaderManager.cpp:2308-2310`,
  `CapturedNormals.h:645-683`, least cosine 0.5 at `:54`). So AO, AmbientLight / bounce, the sun and point contact shadows, Specular,
  Reflections (SSR), WetWorld and wetness reflections, Snow, and the RT inputs (RT AO / GI via `RtAOInput.frag`, RT sun and lamp shadows)
  see the vertex normal, never the normal map.
- Rebuilt from depth on purpose: the AO sky term with `[Shaders.AmbientOcclusion.Skylighting] DepthNormals` (default false,
  `AmbientOcclusion.fx.hlsl:545-555`); the RT AO / GI input on a texel that holds no unit normal (`RtAOInput.frag:39-63`).
- Normal-mapped readers besides the light passes, all off by default: DeferredReflections and DeferredRRGuides (a view-space copy of the
  shading normal, `core/FdIndirect.h:10-32,60-77`); the path tracer's first hit (PTBumpNormals; path tracing is off by default).

Gaps:
- Proven: screen effects and RT AO / GI get no normal-map detail (above). Pixels where the merge test fails keep the flat depth normal.
- Proven: leaves have no normal map in the deferred passes, only the card's flat normal; the slot-tight shader and far land store a flat
  smooth normal; draws through the game's own vertex shader have no shading normal.
- Proven: with `EffectsAfterUpscale` (developer test, off) the merge is skipped and every screen effect is back to the depth normal
  (`ShaderManager.cpp:2309`, `TAA.cpp:111`).
- Believed: the derivative-built tangent frame (most objects and parallax) differs a little from the vertex tangent frame the forward
  shaders light with, mostly on small or distant triangles where the 2x2 pixel derivatives cross a triangle edge. Not measured.
- Believed: the SunShadows effect (screen-space cascade lookup) samples the normals buffer with linear filtering (`SunShadows.fx.hlsl:39`), so normals blend
  across silhouettes there. Not checked in game.
- Precision: fp16 octahedral is about 0.03 degrees at worst; not a problem (arithmetic, not measured).

#### Gaps the code itself lists
- DeferredSun does not carry: metal's bridged tint, the wet film's sun highlight, the land's highlight with UsePBR off, parallax's
  vanilla sun with POM shadows off, and blended (fading) draws' terms (`Shaders/DeferredSun.hlsl:17-18`).
- Leaves get no room lamps under the deferred path (above). First person stays forward.
- (Found 2026-11-19) The lamps' streak on the wet film (Wetness Effects' `WetLampFilm`, the wet-road highlight of a fire or a street lamp) is
  drawn only in the forward records' lamp loops; the deferred records compile every lamp out and DeferredLamps has no wet film, so it was
  gone under the deferred renderer. Restored by `[Main.Main.Quality] DeferredWetLampFilm` (branch `wet-lamp-film`): the wet reflection pass
  (`Effects/WetnessReflections.fx.hlsl` WetLampFilmSum) adds it for the pixels a deferred record drew, from the frame's lamp table. It needs
  Wetness Effects' reflections on (the pass runs only then). The sun's wet highlight is still missing (DeferredSun, above).
- Some comments in the shipped defaults are stale against the code: GBufferV2 "test only, nothing on screen changes" (:674), DeferredDepth
  "nothing reads them yet" (:647), `SettingForce.h:23` "no reader on this branch". The renderer now depends on all three.

### Believed (not proven)
- VolumetricLight / VolumetricFog / TAA behave the same in both modes (no branch found; not run).
- Bend's in-lighting contact shadows do not reach the deferred sun (DeferredSun.hlsl has no such opt-in); Bend is off by default. (2026-11-27: that
  forward port is removed; the deferred sun takes the Bottled Shaders port, EFFECTS.md "ShadowsExteriors" section 12.)
- Tree trunks and branches are deferred as objects (inferred from the shader pairing, not traced).
- Menus and the Pip-Boy are forward (inferred from the span's size and context gate).

### Costs and visual differences (measured vs claimed)
- Measured (code comment, source file absent): the DeferredSun pass, GTX 1070, native D3D9 1080p, frame replay: 0.91 ms, 1.08 ms with
  the fade branch compiled in (`DeferredSun.h:200-202`). That is one pass, not deferred vs forward.
- Measured, forward only: ManyLamps one pass, Vault 21 lobby, GPU not named: frame 20.3-21.0 vs 22.9-23.1 ms, world draws 6,015 vs 8,677
  (`ManyLamps.h:12-15`). Lamp-shadow atlas on native, GTX 1070: Vault 21 world 16.0 vs 15.5 ms (`PointShadowForward.h:171`).
- Claimed: "the picture changes slightly; the first start takes longer (more shaders to compile)" (defaults :677); DeferredDistant
  "changes the image slightly" (:702); earlier bugs fixed per comments (deferred land 15-25 % darker by day; emissive objects with 3+
  lamps; silver glass bottles).
- Not measured anywhere in the repo: whole-frame DeferredRenderer 0 vs 2, any RTX or AMD number, 1.0.x vs 1.1.0.

### Open
- Measure DeferredRenderer 0 vs 2 on this PC (frame time alone, the same spots indoors and outdoors) and look at the differences by eye.
- Ask upstream for `docs/next/` (the FD-* results), or treat every number cited from it as unverified.
- Fix the stale defaults comments (GBufferV2, DeferredDepth) and the stale `DeferredRenderer.h` / `SettingForce.h:23` comments.
- Road map for moving the rest to deferred: `docs/DESIGN.md "Deferred roadmap"`.

## Deferred roadmap

*Original title: Deferred renderer: what is and isn't deferred, and the road map (2026-11-18)*

Read from the code on `vs-1.1.0-plus-fork` (upstream 1.1.0's renderer). Background and file:line detail: `docs/DESIGN.md "Deferred vs forward"`.
Effort is in working sessions and is an estimate. Nothing here was run in game. Upstream's own result docs (`docs/next/*`) are not in
either repo, so anything resting on them is "claimed".

### Where it stands (mode 2, the default)
| Deferred (sun and lamps in the full-screen passes) | Forward+ (forward, but every lamp from the lamp table) | Forward (the game's few lamps, or unlit) |
|---|---|---|
| Objects, armour, clothes, parallax | Glass and other blended lit objects (DeferredTransparents 1) | Water |
| Near land, far land, far (LOD) objects | Blended hair (DeferredHair) | Particles, smoke |
| Skin (DeferredSkin), alpha-tested hair | | Decals: blood (GDECAL), bullet holes (SM300x) |
| Near grass, SpeedTree leaves (sun only; their lamps: DeferredFoliageLamps, off) | | Distant trees (the game's DISTTREE shader) |
| | | First person: arms and gun (DeferredFirstPerson, off) |
| | | Fading-in / fading-out objects and faces, late grass |
| | | Shiny objects' cube-map layer (EnvTemplate), drawn over the deferred base |
| | | Anything whose forward shader failed to compile (the game's shader; see Phase 1) |
| | | Sky, clouds, menus, Pip-Boy screen, unlit objects: nothing to light |

### The order, and why
1. Measure before moving anything: the frame cost of deferred vs forward was never measured in the repo.
2. Fix what silently drops pieces back to the game's shader: it looks worst (today's pale ground) and costs least.
3. Turn on what upstream already built but left off: the code exists; it needs an in-game check and your word (each changes the picture).
4. Then new work, cheapest visible win first: forward+ for what still gets the game's few lamps, then true deferred for the last opaque bits.

### Phase 0: measure (room 06's kind of work)
- DeferredRenderer 0 vs 2, frame time alone, the same spots: a lamp-heavy interior (Vault 21), the Strip at night, open desert by day.
- `[Main.Main.Misc] GBufferCensus 2` on the same spots: how much of the screen each group covers (deferred, forward+, forward), so later
  phases are ranked by real screen share, not by guess (`core/GBufferCensus.h`).
- Effort 1 session. Risk: none to the game (test keys).

### Phase 1: stop pieces falling back to the game's shader
- **1a. Build the deferred record even when the forward one fails.** Today the lighting-free (deferred) record is only tried after the
  forward record loaded (`core/LightFreeRecords.h:70`, `core/DeferredRenderer.h:174`). At 19:13 the forward land shaders with 5-7 layers
  failed (X4505, out of temp registers, with Terrain Blending + Wetness Effects + Puddles), so the deferred ones were never tried and that
  ground was the game's shader. The deferred record leaves out the sun shadows and lamps, so it was believed to fit (see 1a status: it is close).
  Frames that fall back to forward (MSAA, lockpick) would then show the game's shader there.
- **1a status (2026-11-18, branch `deferred-phase1a`):** built behind `[Main.Main.Quality] DeferredStandalone` (bug fix, default on,
  restart, in the hidden Internal list). Without a forward record the lighting-free record is still made (with the lean puddle path as a
  second try), paired with the game's own shader, which takes the forward record's place outside the G-buffer span and for blended draws.
  A failed compile now also leaves `<binary>.failed.defines` next to `.failed.hlsl`, so the deferred variant can be compiled offline.
  Headroom measured on the cached 17:53 builds (before Terrain Blending), `fxc /dumpbin`: forward land SLS2124 / 2136 / 2146 peak at
  temp r31 (all 32) with 3,842-4,600 slots; their lighting-free records peak at r29-r30 with 2,738-3,178 slots. So the deferred version
  is much shorter but only 2-3 temps from the same wall: with Terrain Blending it may still fail. Not run in game; the next start's log
  says ("DeferredStandalone: ... loaded" or a LIGHT-FREE RECORD warning). If it fails, 1b / 1c are what makes room.
  Lead-architect review: approve with fixes, all applied (the lean copy goes first once the full one failed, as the forward record's;
  any forward slot counts as a forward record; the game's handle is AddRef'd for the session; header notes). Proven by reading: the
  lighting-free record binds only inside the G-buffer span (DeferredRenderer::Choose needs CapturedNormals::InSpan); everything else falls
  through to the game's shader. Believed: the game-shader swap for blended draws is no worse than today's whole-object fallback.
  Default on is a player-visible change: it needs the owner's word before it is installed or merged.
- **1b. The fallback ladder reacts to compile failures too.** ShaderFallback drops one optional feature at a time only when a shader is
  over native D3D9's instruction cap (`core/ShaderRecord.cpp:271` tests `over` only); a shader that does not compile at all (X4505, also
  under DXVK) goes straight to the game's. Count `failed` as a reason to drop the next feature.
- What you'd see: the pale patches gone with Terrain Blending on. Effort 1 session + one full shader recompile to check. Risk: the ladder
  may drop Wetness Effects first (its order), which turns wetness off everywhere for the session; the order may need a land-only rung.

### Phase 1c: deferred wetness, the way around ps_3_0's limits (owner's choice 2026-11-18)
Chosen over the Vulkan "no-limits" replacements (`NvrDxvkReplacements`, `tools/nvr-dxvk/replace/nolimits.py`): those work only under the
DXVK fork, are built offline per exact shader variant (dump, build, copy; any setting that changes a define means a rebuild) and need a
ps_3_0 build that loads to key on. The deferred pass works on native D3D9 too and needs no per-variant builds.

**Why.** Wetness Effects is the biggest optional block in the land shaders: about 1,400 of the 6-layer land shader's 4,000-5,000
instruction slots by the code's own count (`core/ShaderFallback.h`, THE ORDER), and the land's temp-register overflow (X4505) at 19:13
came with Terrain Blending added on top of it. Taken out of the G-buffer-writing records, it leaves room for Terrain Blending and the rest.

**Proof that it can be done on screen.** `Effects/WetnessReflections.fx.hlsl` already works out "where it is wet and how glossy" for every
pixel from the depth and the normals with the surface shaders' own function (`Shaders/Includes/Wetness.hlsl` GetWetness, the same weather,
puddle field and puddle map), and GearWeather already writes weather layers into the G-buffer for first person (`core/GearWeather.h`).

**Design.**
- The lighting-free (deferred) records are compiled without the water layer: `WETNESS_EFFECTS 0` added to their overrides
  (`core/LightFreeRecords.h:46-49`; the define is set at `core/ShaderRecord.cpp:585`). Forward records keep it, so fading objects, water
  reflections and other cameras stay wet as today.
- One full-screen pass, `DeferredWetness`, between the G-buffer's end and the deferred sun (the span's end in `NewVegas/Hooks/Render.cpp`,
  after `DeferredDepth::AtSpanEnd`, before `DeferredSun`). Per pixel with a deferred record: GetWetness from the position (G-buffer
  distance or DeferredDepth), the smooth normal (COLOR3) and the bumped normal (COLOR2), then:
  - albedo (COLOR1 rgb) darkened by the porous-wet term;
  - roughness (COLOR2 b) lowered to the film's, and the specular bit set, so DeferredSun and DeferredLamps give the wet highlight with
    their own GGX (this also closes the "wet film's sun highlight" gap in DeferredSun);
  - the shading normal (COLOR2 g / a) flattened toward the smooth normal under standing water, with the ripples on top (`wet.normal`).
- The pass reads and writes the same targets, so it works from copies of COLOR1 and COLOR2 (or writes through blend states where the
  maths allows); the codes in COLOR1's alpha and COLOR2's model / metal bits are passed through untouched.
- Skin, hair, eyes, grass and leaves keep their own models: the pass skips their shading models (COLOR2 r), as the forward shaders give
  them no water film either (to be checked per family).
- Under its own switch first (`[Main.Main.Quality] DeferredWetness`, test only, off), with an A/B against the forward wetness.

**Cost (not measured).** One full-screen pass reading about four fp16 targets and depth, writing two: about 50-60 bytes a pixel, roughly
0.15-0.3 ms at 1080p, only while something is wet (Wetness Effects' own flag). The deferred land and object shaders get shorter, so the
G-buffer fill should get cheaper in the rain, believed to cover most of it.

**What you'd see.** The same wet ground, puddles and ripples, now also on the land pieces that fell back to the game's shader, with
Terrain Blending on. A one-time full shader recompile when the switch turns on.

**Effort and risk.** 3-4 sessions plus a recompile and a rain test. Risks: matching today's wet look (darkening curve, puddle edges);
puddle height on objects (`OBJ_PUDDLE_HEIGHT` reads the object's own texture, which the screen pass does not have: puddles on objects
may need the object's height carried in or dropped); wet surfaces seen in water reflections stay on the forward path (unchanged).
Needs Phase 1a first so the deferred record loads even if a forward one still fails.

### Phase 2: built upstream, off by default (each: an in-game look, then your word)
| Switch | What it moves | Look check | Effort |
|---|---|---|---|
| DeferredFoliageLamps | leaves lit by every lamp of the froxel, not the game's one | trees near street lamps at night | 0.5 |
| DeferredFirstPerson | arms, hands and gun through the deferred passes (aiming, scopes, Pip-Boy, VATS stay forward) | gun in a lamp-lit room, then aim | 0.5 |
| DeferredReflections, DeferredRRGuides | reflections / DLSS RR guides read the normal-mapped normal and the G-buffer material | wet roads, tiled floors | 0.5 |
| DeferredALAlbedo | bounce light takes the G-buffer colour minus metal | coloured walls near light | 0.5 |
| DeferredRtSunFirst | RT sun shadow as the deferred sun's own visibility | RT sun shadows on, outdoors | 0.5 |
| DeferredTiledLight | lamp pass only over the tiles that have lamps (speed) | frame time in Vault 21 | 0.5 |
| DeferredADFog | a fog fix for the light-only base passes | foggy interiors | 0.5 |
Risk: low per switch (the code exists and has its own A/B key), but none has been looked at on this PC. Several are claimed in upstream's
missing result docs only.

### Phase 3: forward+ for what still gets the game's few lamps
- **Water** (1-2 sessions): every lamp of the froxel and the flashlight on the water surface from the lamp table, as glass does
  (`Includes/ForwardPlus.hlsl`). True deferred does not fit water (blended, refracts, its own reflection). See: lamps reflected in water at night.
- **Fading objects and faces** (1 session): the blended base pass of an object fading in or out takes the forward+ record instead of the
  forward one, so it is lit like its neighbours while it fades (today it keeps the game's lamps, `DeferredRenderer.h:250-277`).
- **Late grass** (1 session): the fading tufts drawn after the main list get the leaves' forward+ lamp model.
- **Particles and smoke** (2 sessions, believed): the game's particle shaders are not this mod's templates, so they need replacement shaders
  first; whether the vanilla ones light at all was not checked.

### Phase 4: true deferred for the last opaque pieces
- **Decals** (2-3 sessions): blood and bullet holes write their colour, normal and roughness into the G-buffer inside the span, so the
  deferred passes light them (the standard "deferred decals"). Needs them drawn early inside the span, as DeferredDistant already does for
  the far world (`core/DeferredDistant.h`). Risk: draw order, blended writes into fp16 targets.
- **Distant trees** (1-2 sessions): a replacement for the game's DISTTREE shader that writes the G-buffer. Small screen share (Phase 0 will say).
- **Cube-map layer** (1-2 sessions): fold the EnvTemplate pass into the deferred reflections (cube map as the fallback when a ray misses).
  Risk: double reflections during the change.
  (2026-11-20) Part of the one-reflection-pass plan: `docs/DESIGN.md "Unified reflections design"` (steps 1-5, 6-9 sessions).
- **DeferredSun's missing terms** (1 session): metal's tint, the wet film's sun highlight, the land's highlight with UsePBR off, parallax's
  sun with POM shadows off (`Shaders/DeferredSun.hlsl:17-18`).

### Stays forward on purpose
Sky and clouds, menus and the Pip-Boy screen, other cameras, unlit objects, first person while aiming, and glass (forward+ is the right end
state for see-through things). The path tracer replaces the picture and keeps foliage forward under it.

### Effort in total
Phase 0: 1. Phase 1: 1. Phase 1c (deferred wetness): 3-4. Phase 2: 3-4 (looks). Phase 3: 5-6. Phase 4: 5-8. About 18-24 sessions for
everything; Phases 0-1c (5-6 sessions) fix what breaks today and lift the shader-size limit.

### Gaps today and "can it be fully unhooked from forward?" (2026-11-19)
Asked by the owner. Read from the code on the installed test build (`test-p1a-bump` 90fc395 + deferred wetness step 1) and the docs
`docs/DESIGN.md "Deferred vs forward"`, `archive/2026-11-25.zip vs/repo-docs-archive/bumped-normals-study.md`, `docs/DESIGN.md "Deferred wetness design"`. Nothing measured; ranks are by what a player sees (believed).

#### The gaps, most visible first
| # | Gap | What the player sees | Fixed by |
|---|---|---|---|
| 1 | Screen effects read the smooth normal (AO, bounce light, SSR, wetness, snow, RT AO / GI input; `core/CapturedNormals.h:645-683`) | no normal-map detail in shade or in reflections: flat-looking walls and ground out of the sun | DeferredBumpedNormals (reflections; prototype, the owner has it on), then a bounce-light receiver opt-in (bumped-normals-study) |
| 2 | Shaders too big for ps_3_0 (X4505) fall back to the game's | pale, unlit patches; since DeferredStandalone (proven in the 2026-11-18 log) only outside the main view: reflections, fading draws, forward frames | 1b (fallback ladder on compile failure), deferred wetness steps 2-4 (shorter records) |
| 3 | Water: the game's few lamps, no flashlight | lamps and torches don't light or glint on water at night | Phase 3 water forward+ |
| 4 | Wetness on objects, parallax and far land still inside their records (step 1 did the near land) | wet rocks, walls and road objects get no sun glint under the deferred renderer; heavier shaders | deferred wetness steps 2-3 |
| 5 | Decals (blood GDECAL, bullet holes SM300x) forward | holes and blood lit by the game's few lamps, flat at night beside correctly lit walls | Phase 4 deferred decals |
| 6 | Particles and smoke: the game's shaders | smoke, fire and dust ignore Vegas Shaders' lamps and sun shadows | Phase 3 (needs replacement shaders first; vanilla lighting not checked) |
| 7 | Reflections and other cameras use the forward records or the game's shader | water and scope reflections lit differently from the main view; the game's shader where the forward record failed | "Lighting" below |
| 8 | Fading objects and faces, late grass forward | a short lighting pop while things fade in | Phase 3 fading forward+ |
| 9 | First person forward (DeferredFirstPerson off) | the gun and arms lit by the game's few lamps, not the room's | Phase 2 switch (exists) |
| 10 | The lamp streak on wet ground has no lamp shadows (screen pass) | a streak through a wall or under an object at night | deferred wetness: WetLampFilm into DeferredLamps' loop (1 session) |
| 11 | DeferredSun's missing terms (metal tint, land highlight with UsePBR off, parallax sun with POM shadows off) | slightly different highlights than forward | Phase 4 |
| 12 | Distant trees (DISTTREE), cube-map layer (EnvTemplate) | small; far trees keep the game's light | Phase 4 |
| 13 | The game's MSAA is off while the renderer is on (`NewVegas/Hooks/Settings.cpp:16,67`) | jagged edges only without TAA / FSR / DLSS (the owner uses DLSS: none) | not needed (deferred + MSAA would need per-sample passes) |

#### Can the deferred path be fully unhooked from forward?
**Partly.** Proven by reading: the deferred records are chosen only inside the main list's G-buffer span (`core/DeferredRenderer.h:204`,
`CapturedNormals::InSpan`), so every other draw of an opaque object still needs a lit forward record (or the game's shader):
- the water's reflection camera, scopes and other cameras, the cube maps the game renders (shadow cameras are not lit: they draw with
  depth-only shaders, so they are not part of this);
- blended / fading draws of opaque objects (`core/DeferredRenderer.h:250-277`);
- whole frames that fall back (lockpick, the game's MSAA with ForceMSAA, a refused sun pass);
- `DeferredRenderer 0`.

**Lighting (believed doable, big).** Taking the forward lighting code off opaque geometry means giving the other cameras their own path:
- a "reflection record": the lighting-free record + the sun only, no lamps, no shadow filter (water reflections are blurred and small). Still
  a forward shader, but a far shorter one that never hits the size wall. 2-3 sessions. Recommended.
- a second, small G-buffer for the reflection camera with the sun and lamp passes run on it: the "real" answer, but the game draws
  reflections through its own pass lists (believed at awkward points), every pass needs a second set of targets, and the cost about doubles
  in water scenes. 4-6 sessions, high risk.
Blended / fading opaque draws go forward+ (Phase 3): still a forward shader, but with the deferred lamp model and lamp table.

**Drawing (stays forward by nature).** Transparency, particles, water (it blends and refracts), sky, UI. Every deferred engine keeps these
forward; the standard answer is forward+ / clustered lighting: the same lamp table and shadow atlas the deferred passes use, read per pixel
by the blended shader. Vegas Shaders already does this for glass and blended hair (`core/DeferredTransparents.h`,
`Includes/ForwardPlus.hlsl`). Order-independent transparency (weighted blended OIT) fixes sorting, not lighting; FNV sorts its alpha list
itself, so OIT is not needed first (believed).

**`DeferredRenderer 0`.** Keep it for now. The cost of two paths: every shading feature exists twice (the template's forward code and the
pass's), each change needs a check in both modes, and the forward records keep the ps_3_0 size wall alive. The forward records are needed
anyway for the items above, so dropping mode 0 saves little until the reflection record and forward+ exist. After that mode 0 can be frozen
("deferred first" is already the room rule) and later removed: 1 session.

**Realistic end state for FNV on D3D9 / DXVK.** All opaque world geometry in the main view deferred (today's plus decals, distant trees, first
person, wetness); everything blended (glass, hair, water, fading objects, particles) forward+ from the same lamp table; other cameras on a
short reflection record; sky and UI unlit. No opaque forward lighting code left except that reflection record. Not "100 % deferred", but
one lighting model everywhere, and the ps_3_0 size wall gone from the heavy shaders.

#### Alternatives for what looked forward-only (2026-11-19, owner: "think about alt methods for stuff that seems like it can't")
All believed (engine comparisons, efforts); nothing built or run. With these, everything opaque goes deferred and only glass, particles
(lit from a shared light volume) and sky / UI stay forward.
| Item | Alternative that keeps it deferred | Done this way in | Effort, risk |
|---|---|---|---|
| Fading objects | dithered (screen-door) fade, the stipple averaged by TAA / DLSS; no forward pass while fading | Unreal (Dithered LOD Transition), Unity (LOD cross-fade) | ~1, low |
| Blended hair | dithered hair into the G-buffer (alpha-tested hair is already deferred) | Unreal | 1, medium (strands may shimmer) |
| Water reflection camera | drop it: SSR + the RT reflections + a sky / cube-map fallback; no second lit scene draw | most modern engines | 3-4, medium (off-screen, untraced things show the sky) |
| (if the camera stays) | a half-size G-buffer for it with the sun pass run there | planar reflections in deferred engines | 4-6, high |
| Water surface | "deferred water": the surface written opaque into the G-buffer (normal, gloss, F0 0.02) so the sun and lamp passes light it; one pass after does refraction / absorption / murk from the scene copy and the depth difference | Frostbite, Unreal | 3-5, medium-high (shore edges, seen from below) |
| Particles, smoke, fire | a low-res light volume (sun with shadow + lamps) made once a frame from the froxel lamp grid; the particles sample it | Unreal (translucency lighting volume), Frostbite | 2-3 + replacement particle shaders (2+) |
| Glass | stays forward+ (built); a one-layer transparent G-buffer brings more artefacts than it fixes | - | 0 |
| Scopes, other cameras | the deferred span opened for any world camera at main-view size; small pictures on a short sun-only "reflection record" | - | 2-3, medium |
| Fallback frames (lockpick) | open the span for that scene too | - | ~1, low (believed) |
| MSAA | not needed: TAA / DLSS | - | 0 |
| Cube-map layer (EnvTemplate) | the reflection pass's "ray missed" fallback | standard | 1-2 |
| `DeferredRenderer 0` | frozen once the rows above exist, then removed with most forward lighting code | - | 1 |
Revised order: finish wetness; Phase 0 measure; dithered fade + hair; decals + distant trees; water reflections from SSR / RT (alone first,
the biggest visible change); deferred water surface; particle light volume; freeze and remove mode 0. About 25-35 sessions. Risks: water
(look match), the dither stipple without TAA / DLSS, particles need their shaders replaced first.

#### Order and effort (sessions, estimates)
1. Finish deferred wetness: check the smear fix, lamp streak into DeferredLamps (1), objects + parallax (1-2), one system (1-2), retire
   WetWorld and the records' water layer (0.5).
2. Phase 0 measure (1), so the rest is ranked by real screen share.
3. Bumped normals for reflections on by default (0.5, your look), bounce-light receiver (1-2).
4. Phase 2 switches as one batch look (1), first person included.
5. Water forward+ (1-2), fading objects forward+ (1).
6. Reflection record for other cameras (2-3).
7. Decals (2-3), distant trees (1-2), cube-map layer (1-2), DeferredSun's missing terms (1).
8. Particles (2+, needs their shaders first).
9. Mode 0 frozen, then removed (1).
About 20-28 sessions in all. Main risks: matching the forward look per group (each needs a side-by-side), the game's draw order for decals
and reflections, and the frame cost of extra passes in water scenes (not measured).
Existing mods: none of xNVSE, JIP LN, NVTF, Engine Optimizations or johnnyguitar does this kind of rendering; Vegas Shaders is the only
deferred renderer for FNV. Community Shaders (Skyrim) is the reference for clustered lighting (Light Limit Fix), wetness and terrain
blending, and several of its ideas are already ported here.

### Questions for you
1. Start with Phase 0 (measure) or Phase 1 (the fallback fixes) first?
2. Phase 2 switches change the picture once on by default: look at them one at a time, or as a batch in one session?
3. Is water (Phase 3) the next big one, or decals / first person?

### Proven / believed / not checked
- Proven from the code: the table above, the Phase 1 causes (file:line given), the existing switches and their defaults.
- Believed: the deferred land record fits where the forward one did not; the effort numbers; screen shares.
- Not checked: vanilla particle lighting, DISTTREE's screen share, any frame time, anything in game.

## Deferred wetness design

*Original title: Deferred wetness: design and steps (road map Phase 1c, 2026-11-19, v2)*

Owner's choice (2026-11-18/19): the deferred wetness pass is the end state; WetWorld is retired only once it covers everything WetWorld
does. Room rule: deferred first. Branch `deferred-wetness` (off `wet-lamp-film`), built on `deferred-wetness-ground`. Paths under `src\`.
State 2026-11-21: steps 1-3 run in game; step 4 (retire WetWorld, one rain clock, Wetness Effects + Puddles on by default) built on branch
`retire-wetworld`, see "Step 4 as built". DeferredWetness itself stays a test switch (off by default): its frame cost is not measured.

### Why
- Wetness Effects + Puddles live inside the land and object shaders (`hlsl/NewVegas/Shaders/Includes/Wetness.hlsl`, compiled into
  TerrainTemplate / ObjectTemplate). They are the biggest optional block there (about 1,400 slots on the 6-layer land, `core/ShaderFallback.h`)
  and keep the land shaders at the ps_3_0 temp-register wall (X4505 with Terrain Blending; measured 2026-11-18: lighting-free land at r29-r31).
- Under the deferred renderer the wet look is only half there: the lighting-free records drop the film's sun highlight (DeferredSun gap) and
  its lamp streaks (fixed by DeferredWetLampFilm, branch `wet-lamp-film`); the sky reflection of the film is still drawn by the record.
- Two wetness systems (WetWorld screen pass, Wetness Effects in the shaders) and two reflection passes on wet ground confuse the menu.

### What the forward land does today (TerrainTemplate.hlsl, proven by reading)
1. `GetWetness(camPos, geoN, wetWorldNormal, surfaceHeight)` (:703): darkening, gloss, roughness, the film normal (flattened under standing
   water, ripples on top), flatten, murk, debris.
2. Surface changes, before lighting: `combinedNormal` flattened by `wet.flatten` (:704-705); `baseColor = WetAlbedo(baseColor, darkening)`,
   `*= lerp(1, 0.4, flatten)`, debris (:707-712).
3. Lighting with the DRY gloss (the layers' glossV / specPow) on the flattened normal and the darkened colour.
4. The film on top, after lighting: the sun's highlight `WetSpecular(wet.normal, ...)` (:730), the sky reflection
   `WetSkyReflection(reflect) * WetReflectance(...) * horizon^2 * PuddleMurkTint` (:731-736), each lamp's streak `WetLampFilm` (:779).
Objects (ObjectTemplate.hlsl ObjectWetFilm, :565; OBJ_PUDDLE_HEIGHT for object puddles, off by default) follow the same split.

### Design v2 (after the lead-architect review, 2026-11-19: "rethink"; v1's two-pass split is kept below only as what not to do)
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

### Step 1 as built (2026-11-19, branch `deferred-wetness-ground`)
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
  `F:\VS-scratch\install-test6-2026-11-19\restore.ps1` (folder merged into `install-original-2026-11-18` on 2026-11-20) -> test5). Not run in game yet. What to check: the log lines "DeferredWetness:
  the pass loaded", "wet: the land's water layer drawn by the pass", no "[ERROR] ShaderRecord: more than 62", no LIGHT-FREE RECORD
  warnings; in rain, the switch on vs off side by side (same spot, same weather).

### Step 2 as built (2026-11-20, same branch)
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

### Step 3 as built (2026-11-20, same branch): one system
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
  forward records got it: land (model 4) the real level, worn gear, hair and plants 0, other objects and unwritten pixels only with ObjectPuddles
  (on for the owner). No film at all on skin, grass, eyes and unlit (2026-11-21, slot run: grass with the ObjectPuddles level became a sharp
  sky mirror in the Reflections effect). Film cleared (not skipped) on a stale record: COLOR3's camera distance not this pixel's (0.4 % + 2
  units), i.e. something drew over the records later. The game's grass writes no record outside path tracing, so its pixels kept the
  ground's film from the sun-pass pass and Reflections mirrored the blades on depth-rebuilt normals (believed: the owner's grainy,
  boiling wet grass in motion). No fog round trip on unwritten pixels (their fog is unknown): believed darker blobs far away in dense rain fog. Skipped: sky, water plane (Reflections' test), first-person
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
After step 3 (2026-11-21): the Reflections effect reads the film's damp ground from its cube, not the trace (only standing water is
traced; `docs/EFFECTS.md "Reflections"` last section, `docs/DESIGN.md "Reflection speckle"`). The film's roughness is what tells them apart, so
the film must keep the Puddles Roughness under standing water and the rougher damp value elsewhere.

### Step 4 as built (2026-11-21, branch `retire-wetworld` off `deferred-wetness-ground`)
Owner's words: "move the 'is it raining / is the ground wet' clock to Wetness Effects and remove WetWorld"; on the defaults, "make the default
mine" (the shipped wetness defaults = the owner's settings); on players whose shaders cannot carry Wetness Effects, the recommended option
(no replacement) was taken without an answer.

Proven by reading the code before the change
- Two clocks: WetWorld hosted RainStateGuard (`core/RainState.h`: WetWorld's PuddlesAnimator by `[Shaders.WetWorld.Main] Increase / Decrease`
  game hours, Precipitations' RainAnimator 0.05 / 0.07 h), read by the Rain setting groups (Terrain.cpp, PBR.cpp), skin, heat haze, the gear,
  the drops, the water's rain rings (WATER000/001/016/017/018 `TESR_WetWorldData.x`), Reflections, Specular and the volumetric light; Wetness
  Effects' own clock (Wetness / PuddleWetness / Raining by WeatherTransitionSpeed) drew the ground. So the Rain groups, skin and heat haze
  dried on WetWorld's schedule while the ground dried on Wetness Effects'.
- Wetness Effects' clock ran only while the collection was on (ShaderManager::UpdateConstants calls a family's UpdateConstants only when
  Enabled) and twice a frame (UpdateConstants runs twice per rendered frame; it eased by the frame's ElapsedTime each time).
- The cover map's wet half had one reader, WetWorld.fx techniques 5 / 6 (grep of src/hlsl); snow reads its own half.

What changed (C++ only, no path: a CPU clock; the shaders are unchanged except the water's register name)
- `RainState::Tick` runs `WetnessEffectsShaders::TickWeather` once a rendered frame (FrameNumber) from ShaderManager::UpdateConstants and
  from every reader, on or off. `Rain()` = Raining (drops, water rings, Reflections, Specular, volumetric light, the gear's wet), `Factor()`
  = max(Raining, Wetness) (the Rain groups, skin, heat haze, the gear's mud). TESR_WetnessData is filled every frame. RainState also asks for
  the ortho map while it rains outdoors (WetWorld used to). ForceRain / ForceSnow act on this clock and on Puddles' level.
- WetWorld is gone: its screen pass (EffectChain), effect, settings, menu rows, shaders (WetWorld.fx, Compute WetWorld*.frag/.spv/.glsl).
  The cover map's wet half is always off (dormant code); its size, resolution and debug view are SnowAccumulation's keys now.
- Defaults: Wetness Effects and Puddles on, with the owner's values; settings step 91 (UpdateOldDefaults) carries the map keys over, turns a
  saved Wetness Effects / Puddles off on when WetWorld had been on, drops the [Shaders.WetWorld] section, moves the other old defaults once.
- ShaderFallback (native Direct3D 9, land shaders over 4096 slots): Wetness Effects is still dropped first; nothing draws wet ground in its
  place now (a [WARNING] says to use DXVK). The clock still drives the drops, the water's rings and the Rain groups.

Believed, not run in game
- Ground, skin, heat haze, the Rain groups and the drops start and stop together; the film dries in about 10 s at WeatherTransitionSpeed 3,
  as before (the clock now runs once a frame at TunedClockRate 2, the speed it had running twice a frame; review fix). Puddles' standing
  water keeps its game-hour DryHours.
- The drops now fade in and out with Raining (about 2.5 s) and the weather's transition instead of 0.05 / 0.07 game hours.
- Review fix: the wet passes of the material step (Wetness Effects' reflections, the deferred wetness fallback) now hold off the fog fold
  (effectsBetween) and the bounce-into-specular merge, as WetWorld's wet frames did.

Not done in step 4
- The records' water layer (forward Wetness.hlsl in the land and object records) is not retired: the forward path and DeferredWetness off
  still need it. DeferredWetness stays off by default (frame cost not measured).
- Frame cost: not measured.

### Steps (v2)
1. **Land:** A (albedo, normal, ambient, film target) + DeferredSun's film highlight + DeferredLamps' streaks + the screen pass's sky for land,
   behind `[Main.Main.Quality] DeferredWetness` (restart, test only, off); the land's lighting-free records with WETNESS_OFF; the height via
   COLOR2.r's fraction if the reader audit allows. Offline: compile A, DeferredSun, DeferredLamps, the land records (size, temps); DLL;
   review; install; rain look against the forward land (switch off). 2-3 sessions.
2. **Objects and parallax** (their film, their sky moved to the screen pass). 1-2 sessions.
3. **One system:** the film also for pixels no deferred record drew (game shaders, LOD, forward frames: WetWorld-style on the lit colour), one
   "Rain wetness" switch, old puddle keys hidden; WetWorld only as the fallback where there is no G-buffer. 1-2 sessions.
4. **Retire** WetWorld and the records' water layer; on by default (owner's word: player-visible, one full recompile). 0.5 session.

### Risks (v2)
- Matching the forward look: the A wetness must equal what the forward record computes beside it (same defines, same inputs).
- ps_3_0 room in DeferredSun / DeferredLamps for the film terms and one more sampler.
- The height channel (see above).
- Fog round trip in COLOR0 (keep codes 1 / 3 / 5 + keep).

### v1, rejected (for the record)
A wrote only albedo and normal, B (the screen pass) added sky, sun and lamps. Gaps found by the review: dry ambient in COLOR0; WETNESS_EFFECTS 0
does not remove the layer; PUDDLES_RUNTIME's different wetness; B reading A's flattened normal; B's run conditions hiding the film; the screen
sun-shadow mask has no cascade shadows (a highlight inside cast shadows); a double sky on objects; far land made wet; Cavity lost.

### The film's sky reflection under unified reflections (2026-11-20)
With `[Main.Main.Quality] UnifiedReflections` used, outdoors, the pass and the fallback leave out the film's sky reflection on the floors the
Reflections effect mirrors (DWSkyKeep, `docs/DESIGN.md "Unified reflections design"` "Step 2 as built"), and Wetness Effects' screen pass skips its trace
(its lamp streaks stay). Off: as before.

### Not decided (owner)
- (Answered by the build, not by the owner: the height rides in COLOR2.r only between the record and pass A, so no channel is spent.)
- When to make DeferredWetness the default (WetWorld is retired: step 4 as built). Needs its frame cost first.

## Unified reflections design

*Original title: One deferred reflection pass: design (2026-11-20)*

Read from the code of the current test build (`test-p1a-bump` e31a5ed: deferred wetness steps 1-3, bumped normals, Reflections with the
film). Paths are under `src\` unless they start with `docs\` or `resource\`. Nothing here was built or run. Costs are estimates, not
measured. Background: `docs/DESIGN.md "Deferred roadmap"`, `docs/DESIGN.md "Deferred vs forward"`, `archive/2026-11-25.zip vs/repo-docs-archive/bumped-normals-study.md`,
`docs/DESIGN.md "Deferred wetness design"`, `docs/EFFECTS.md "Reflections"`.

### Short answer
- **Doable, and most of it exists already.** The Reflections effect's Vulkan path is already the source ladder the request describes:
  screen ray (or RT hit) -> live cube -> light grid -> sky, mixed by split-sum Fresnel and blurred by roughness
  (`hlsl/NewVegas/Compute/ReflectionsTrace.frag:75-99`, `ReflectionsRTResolve.frag:247-264`). What makes reflections "many systems" today
  is everything around it: four other screen passes trace or fake their own reflections, it takes its material from a side channel
  (MaterialAware) instead of the G-buffer, it only covers floors, and the forward shaders still add their own cube / sky / metal sheen.
- **The plan:** grow the Reflections effect into the one pass rather than write a new one. It reads normal, roughness, metal and the wet
  film from the G-buffer, covers every surface by roughness, and the other reflection passes and forward sheens retire one by one.
- **In game:** one reflection look everywhere (wet roads, tiles, metal, puddles agree with each other); glossy walls and props reflect
  too, not only floors; no doubled sheen on env-mapped props; one "Reflections" switch with a quality level.
- **Effort:** about 6-9 working sessions in five steps. Biggest risk: double-counted or missing reflections while forward sheens are
  retired (the energy has to move, not be added).
- **Recommended first step:** measure today's reflection cost in rain and dry (0.5 session, needs one in-game run with the profiler),
  then step 1: Reflections reads its material from the G-buffer (1-2 sessions, test switch, off).

### 1. Every reflection path today

| Path | Traces / samples | Normal | Roughness / F0 | Miss fallback | Covers | API |
|---|---|---|---|---|---|---|
| **Reflections effect** (`effects/Reflections/`, chain step Reflections `core/EffectChain.cpp:990-1011`; off by default, `[Shaders.Reflections.Status]`) | half-res march over depth, sampling the lit frame (`Compute/ReflectionsTrace.frag:42-73`); GGX spread, exact ray under roughness 0.03 (:20-31); optional Hi-Z | `FdReflNormals(TESR_NormalsBuffer)` (`Reflections.cpp:36-41,1397`): smooth, or the G-buffer shading normal with DeferredReflections / DeferredBumpedNormals; wet pixels lean back to smooth (`Compute/Reflections.glsl:196-204`) | constants Dry/WetRoughness (glsl:104-107); MaterialAware copy (glsl:322,390-414); film when `FilmWhole` (glsl:273-277); specular AA on bumped normals (glsl:421-458); metal F0 `ReflMetalF0.glsl` | live cube -> light grid -> sky, hand-over `smoothstep(0.15,0.35,roughness)` (Trace.frag:82) | upward floors `N.z > 0.75` (glsl:389), or MaterialAware b > 0.75 any facing (:392), glass by GlassMask, not water | Vulkan (fork) or D3D9 `Effects/Reflections.fx.hlsl` (film, puddles, AA, sky pano are Vulkan only) |
| **RT reflections** (same effect, `[Shaders.Reflections.RT] Enabled` off) | 64-bit helper, compute ray queries on the scene TLAS (`Compute/Rt/ReflectionsRTBody.glsl`), sun / key-lamp shadow ray, MetalBounce | as above | as above, MaxRoughness 0.5 | Resolve: screen -> cube -> hit estimate -> grid -> sky (`ReflectionsRTResolve.frag:35,59,247-264`) | as above | Vulkan + helper |
| **Path tracer** (`ReferencePT`, internal) | its own GGX bounce (`RtPathTrace.comp:1967-1979`, roughness from COLOR2.b) | G-buffer | COLOR2.b | sky | whole picture; Reflections skipped (`Reflections.cpp:819`) | Vulkan |
| **WetnessReflections** (`effects/WetnessEffects/WetnessReflections.cpp`, step after `DeferredWetness::Fallback`, `EffectChain.cpp:934-939`) | full-res march over depth, 24 steps (`Effects/WetnessReflections.fx.hlsl:252-310`); replaces the forward wet sky term with the hit | TESR_NormalsBuffer; film / COLOR2 oct normal (fx:186-203) | GetWetness, or the film (r gloss, g roughness) | keeps the forward sky (fx:289) | wet pixels, outdoors, Wetness Effects on | D3D9 only |
| **WetWorld** (`effects/WetWorld/`, on by default, drawn only when Wetness Effects is off, `EffectChain.cpp:946-951`) | its own puddle SSR (`WetWorld.fx.hlsl:494,603`) | shading normals s15 | its own puddle mask, MaterialBuffer r/a | analytic sky (:1127-1135) | wet ground | D3D9 |
| **DeferredWetness sky** (`Shaders/DeferredWetness.hlsl:177,291`; fallback :164-180) | sky along the wet normal (`Includes/Wetness.hlsl:681-695` WetSkyReflection) | film normal | wet gloss, F0 0.02 | - | wet pixels | D3D9 full-screen |
| **Forward wet film** (`ObjectTemplate.hlsl:571-581`, Terrain :737-747, Parallax :893-899) | sky SH along the wet normal + sun highlight | GetWetness | F0 0.02 x gloss | - | forward records (and lighting-free ones without DeferredWetness) | D3D9 game shaders |
| **DeferredReflections** (`[Main.Main.Quality]`, off; `core/FdIndirect.h:30,72`) | not a pass: swaps Reflections' normals to the shading normals and its material copy to the G-buffer one (`core/FdIndirectShaders.h:48-56`) | G-buffer shading normal | spec-bit models 1-3 reflect, 5-9 rough | - | - | both |
| **Env-map pass** ("chrome", `Shaders/EnvTemplate.hlsl`, replaces SLS2057/2058, `effects/PBR/PBR.h:112-117`) | the game's painted cube (s1) x mask (normal alpha or s3); LiveCube replaces it when ready (:189-194) | normal map, bumps flattened x 0.1 (:77-81,167-169) | none (no roughness, no Fresnel) | - | env-mapped meshes, glass windows; drawn ADDED over the deferred base, never in the G-buffer (`core/LightFreeRecords.h:85-97`, `core/CapturedNormals.h:12-15`) | D3D9 forward |
| **Metal sheen** (`Includes/Object.hlsl:643-659` getMetalAmbient) | ambient + sky along the reflected elevation, or the live cube | geometric | EnvBRDFApprox(F0 = albedo, roughness) | - | metal in lit ObjectTemplate passes; stays in COLOR0 of the lighting-free record (`ObjectTemplate.hlsl:1541-1547`) | D3D9 forward, baked into the deferred base |
| **Skin sky** (`Includes/Skin.hlsl:248-262`) | sky SH | skin | skin F0 | - | skin (deferred skin keeps it in COLOR0) | forward / in the record |
| **Specular effect** (off; `EffectChain.cpp:954-979`) | analytic GGX sun + sky, quarter res | depth normals | constants | - | everything | D3D9 |
| **Water** (`NewVegas/Hooks/Render.cpp:5238-5306`, ForceReflections on) | the game's planar reflection camera (`WATER000:85`, `WATER017:84`, `WATER033:63`); placed / interior water: horizon or fog colour; RT Water reads last frame's Reflections buffer, reprojected (`Includes/Water.hlsl:112-128`) | wave normal map + rain ripples | Schlick, F0 0.08, no roughness (`Water.hlsl:167-183`) | planar -> horizon | water | D3D9 forward, its own camera |
| **Glass** | only the window env-map pass above; GlassMask lets Reflections reflect on glass (GlassReflect 0) | - | - | - | blended, forward+ (`core/DeferredTransparents.h`) | D3D9 forward |

So a wet, metal-trimmed street in the rain can get up to five reflections at once: the forward / DeferredWetness sky term, WetnessReflections'
trace over it, Reflections' trace, the env-map pass, and the metal sheen in the base. They are mixed or added by different rules.

### 2. The unified pass

#### Where it sits and what it is
The Reflections effect, kept at its place in the chain (after the lit frame exists, before the fog; `effects/Reflections/Reflections.h:4-6`).
A true "right after the G-buffer" pass is not possible for the screen trace: it samples the lit colour. Modern deferred engines put SSR
after lighting and before transparents; moving it before the blended list here is a later option (step 5), not needed for one system.

Per pixel, one ladder (as today's Vulkan trace, extended):
1. Inputs from the G-buffer: shading normal (COLOR2 g/a, smooth COLOR3 for the ray start), roughness (COLOR2 b), metal (COLOR2 r fraction /
   0.49), specular bit and model (COLOR2 r), albedo (COLOR1 rgb: metal F0), the wet film (DeferredWetness target: gloss, roughness, normal).
2. Effective surface: wet share w from the film; roughness = lerp(dry, film roughness, w), widened by specular AA where the normal is bumped;
   normal leans to the smooth / film normal by w; F0 = lerp(0.04, albedo, metal), toward 0.02 by w on dielectrics.
3. Source, by roughness: under MaxRoughness the trace (RT hit when RT is on, else SSR; Hi-Z at high quality). Miss or too rough: live
   cube -> light grid -> sky (today's hand-over). Above the cut-off no ray at all: probe only (cheap; this is the "ambient specular" every
   surface needs and what the forward sheens fake).
4. Mix: today's composite (`ReflectionsComposite.frag`: `lin = mix(lin, reflected, share)`, share = split-sum Fresnel x coverage),
   denoised by roughness (RT denoiser, or temporal + blur).

#### What the G-buffer has and lacks (mode 2)
- **Has:** shading normal, smooth normal, roughness, metal, model, spec bit, albedo, distance, the film. Nothing new is needed for steps 1-3.
- **Lacks:**
  - *"The artist made this shiny":* the env-map flag and mask never reach the G-buffer (the env pass is masked off). Two ways, no new
    channel: (a) `[Shaders.PBR.EnvMaps] FromGameData` (exists, off; `Includes/Object.hlsl:119-126,517-557`) already turns the env data
    into metal in the object records, so it lands in COLOR2 r; (b) the record lowers its written roughness by the env mask. (a) first.
  - *Specular occlusion:* none stored; AO and Skylighting's sky visibility are screen buffers the pass can read (already does for the sky).
  - *Pixels the G-buffer does not hold:* game shaders (fallbacks, decals, distant trees), water, glass, particles, first person. They get
    what they get today (water / glass: section "Forward surfaces"; the rest: no reflection), never a wrong material: COLOR1 a = 0 means skip.
- **Channel reuse:** none proposed, so no reader changes. If wet roughness were ever written into COLOR2 b instead of the film target (so
  DeferredSun's own GGX gives the wet highlight), every reader of COLOR2 b must be checked: `DeferredSun.hlsl:205,259`,
  `DeferredLamps.hlsl:419`, `FlashlightDeferred.hlsl:168`, `GearWeather.hlsl:85`, `Compute/Reflections.glsl:379-380,412-413`,
  `DlssRRGuides.comp:113-117`, `RtPathTrace.comp:1967-1982`, `core/FdIndirectShaders.h:51-55`, `core/CapturedNormals.h:471`. Not proposed now.
- **Mode 0** (`DeferredRenderer = 0`) has no lighting-free records but still has CapturedNormals / MaterialBuffer by default. MaterialAware
  (the scene alpha trick, `core/ReflMaterial.h`, `core/ReflMaterialRule.h`) stays as the mode-0 material source; mode 2 reads the G-buffer.

#### What it replaces, and what stays
| Today | Unified |
|---|---|
| MaterialAware / `kReflMaterialPS` material copy (mode 2) | G-buffer read in the trace (no copy pass, no scene-alpha rule) |
| DeferredReflections + DeferredBumpedNormals (two test keys) | always the G-buffer normals, with specular AA (one behaviour, no key) |
| "floors only" (`Reflections.glsl:389`) | every surface by roughness; floors-only stays as the Low quality |
| WetnessReflections screen pass (full-res D3D9 trace) | removed while Reflections is on: the film feeds the one trace (already done for the Vulkan path, step 3) |
| WetWorld's puddle SSR | retired with the wetness step 4 (WetWorld off when Wetness Effects is the one system) |
| DeferredWetness's own sky add (`DeferredWetness.hlsl:177,291`) | the pass's sky / probe fallback; DeferredWetness keeps albedo, normal and film only |
| Env-map pass on top of the deferred base | skipped for G-buffer pixels when the pass answers (ReplaceForward exists, `EnvTemplate.hlsl:203`); env data becomes metal / roughness (FromGameData) |
| Metal sheen in the base (getMetalAmbient) | dropped from the lighting-free records (shorter shaders: helps the X4505 wall), added by the probe step |
| Specular effect's sky term (off by default) | retired: the probe step is the same thing, done right |
| **Stays separate** | water (planar camera, forward), glass (blended, forward+), skin's sky term (skin's own model), the path tracer (replaces the picture) |

**Forward surfaces.** Water already reads last frame's reflection buffer for RT Water (`Includes/Water.hlsl:112-128`); the same read can take
the SSR result for water near the camera, with the planar map as the fallback (the planar camera stays: SSR misses most of what water
mirrors). Glass can read the same buffer at its pixel (the env pass's ReplaceForward read already does exactly this).

#### Wetness
The film target already says where and how wet (step 3). In the pass it is only a material change: lower roughness, F0 toward 0.02, the
normal toward smooth. That is what step 3 built in `Reflections.glsl` FilmWetness for the Vulkan path. Missing: the D3D9 technique has none
of it (film, puddles, specular AA are Vulkan only, `Effects/Reflections.fx.hlsl`): to be ported for one system on native D3D9.

#### D3D9 and Vulkan / RT
- One effect, two back ends as today (`core/EffectRecord.cpp:504` picks Vulkan when `DxvkCompute::PassesOn()`). The rule becomes: both
  back ends read the same inputs and give the same answer without RT; Vulkan adds RT and Hi-Z.
- RT is not a separate effect now either: RayGen -> helper -> Resolve replaces the trace inside the Reflections step and shares the
  fallbacks and the composite. In the unified version RT is simply the top quality level's first source.

#### Settings
One switch, `[Shaders.Reflections.Status] Enabled`, and one `Quality`:
| Quality | What |
|---|---|
| 0 Probes | no trace: cube / grid / sky by roughness on every surface (replaces the forward sheens cheaply) |
| 1 Low | half-res SSR on floors and wet ground, probes elsewhere |
| 2 Medium (default when on) | half-res SSR on every surface under MaxRoughness |
| 3 High | + Hi-Z trace, temporal + blur at full settings |
| 4 RT | RT hits first (fork + helper), denoiser |
Retired keys (kept readable for old tomls, then removed with a migration): WetnessEffects.Reflections Enabled, DeferredReflections,
DeferredBumpedNormals, MaterialAware, Specular's sky. The tuning keys stay under `[Shaders.Reflections.Main]`. Turning reflections on by
default, or folding keys players have saved, changes the picture: owner's word, with a step in `Configuration::UpdateOldDefaults()`.

#### Frame cost (estimate, NOT measured)
- Today in rain with both on: Reflections (cube 6 + 42 filter draws, half-res trace, temporal, two blurs, full-res composite) plus
  WetnessReflections' full-res 24-step march (believed the larger of the two traces: 4x the pixels of a half-res trace) plus the
  DeferredWetness fallback.
- Unified: the full-res wet march goes (believed about 0.4-1 ms at 1440p on a mid card); the trace covers more pixels (walls, props) but at
  half resolution and only under MaxRoughness; the probe-only pixels cost one cube / grid read; the lighting-free records lose the metal
  sheen and the env pass is skipped on answered pixels.
- Net believed: lower than today in rain with both on; up to about +0.3-0.6 ms in dry interiors full of glossy walls at Medium (more
  pixels traced). Quality 0 / 1 cost less than today's effect. Phase 0 measuring decides.

### 3. Risks, effort, order

**Does a mod already do this?** Not for New Vegas that we know of. Community Shaders (Skyrim) is the reference: believed (not checked this
session) it keeps the engine's own deferred SSR for water and wet surfaces and adds Dynamic Cubemaps (a cube made from the screen, used by
env-mapped materials and its wetness) and Skylighting (sky visibility for specular, which this build already ported). That is the same split
this design ends: screen trace first, a live cube as the probe, sky visibility on top. No single CS pass covers everything either.

**Risks**
- Double or missing reflection while forward sheens move (env pass, metal sheen, wet sky): each retirement must remove one term and add it
  in the pass in the same change. Check by A/B on the same spot (proven method: the owner's toggles found the grain cause).
- Wider coverage shows SSR's limits on walls (holes at screen edges, disocclusion streaks): the probe fallback hides most; Quality 1 keeps
  floors only.
- Noise on bumped surfaces: specular AA exists (step 3) but is only proven offline; the owner's look is pending.
- D3D9 parity: the .fx path lacks the film and the AA; porting them meets ps_3_0 limits in a full-screen pass (no 62-define issue, temps
  believed fine).
- Frame cost on walls at Medium (not measured).
- Mode 0 keeps MaterialAware: two material sources to maintain, by design (deferred first).

**Steps** (each under a test switch, off, until the owner's look; then one migration for the defaults)
| # | Step | What you'd see | Sessions |
|---|---|---|---|
| 0 | Measure: Reflections and WetnessReflections cost, rain and dry, F10 profiler, alone | nothing | 0.5 (one in-game run by you) |
| 1 | Reflections reads normal, roughness, metal, albedo F0 from the G-buffer in mode 2 (replaces MaterialAware and DeferredReflections' copy; bumped normals + specular AA always) | same floors; metal and smooth props reflect by their material | 1-2 |
| 2 | Wet into it on both back ends: D3D9 technique gets the film; WetnessReflections pass and DeferredWetness's sky add skipped while Reflections is on | one wet reflection, no double sky on wet ground | 1-2 |
| 3 | Every surface by roughness + probe-only above the cut-off; lighting-free records drop getMetalAmbient; env pass skipped where answered (ReplaceForward, FromGameData) | glossy walls / props reflect; no painted sheen over real reflections | 2-3 |
| 4 | One switch + Quality; retire the old keys with a migration (owner's word) | one menu entry | 0.5-1 |
| 5 | Optional: water and glass read the result; pass moved before the blended list | near water shows SSR detail; glass reflects the room | 1-2 |
Total about 6-9 sessions (6.5-10.5 with step 5).


### Step 1 as built (2026-11-20, branch `claude/unified-refl-step1-009a94`; NOT yet run in a game)
- Switch: `[Main.Main.Quality] UnifiedReflections` (live, test only, off, in the hidden Internal list). To be folded into one Reflections
  Quality setting in step 4 (owner's word).
- In mode 2 with this frame's COLOR1-3, the Reflections passes decode the material per pixel from the G-buffer (`Compute/Reflections.glsl`
  GBufferMaterial; `Effects/Reflections.fx.hlsl` the same under `REFL_GBUFFER_ON`) into the old copy's layout, so MaterialReflectivity and the
  floor rule are unchanged: specular-bit objects / gear / cloth (models 1-3) reflect any way they face by MaterialReflect, up to
  MaterialMaxRoughness (chosen over RT MaxRoughness: it is the existing "which materials reflect" knob, MaterialAware's; RT MaxRoughness only
  decides which reflecting pixels get rays; both default 0.5); other written materials give the floor rule their roughness and metal; models 5-9
  rough. Metal F0 = COLOR1 albedo through ReflMetalF0; dielectric 0.04.
- A record counts only where COLOR3's distance matches the depth (the CapturedNormals readers' rule): anything drawn over the G-buffer keeps
  today's rules. The first-person weapon keeps MaterialAware's lookup (the copy is still made while the RT weapon rays run).
- Normals: the shading normals in every frame it is used (as DeferredBumpedNormals), so the Vulkan specular AA and the film's lean run.
- Review (lead-architect, approve with changes, applied): EffectPrecompile's Job::Macros was 8 while BuildMacros fills 10 (latent, now
  reachable with the ninth define: fixed to 10); the weapon's copy; the normals gated on the frame's decision; the per-technique D3D9
  fallback documented as a known gap. Metal F0 from COLOR1's raw base colour makes rough coloured metal darker than the old normalised hue
  (by 1 / its brightest channel): intended, to be judged by eye.
- MaterialAware's copy (technique 7) and DeferredReflections' / MaterialBufferRoughness's merge are not drawn while it is on. Mode 0 / 1:
  untouched.
- D3D9: compiled in only when on at startup (one define more: nine at most, the macro arrays hold ten with the terminator); COLOR3 / COLOR1
  ride on the capture cubes' stages 8 / 9 (only Capture / Filter read those). Offline: off, the disassembly is identical to before
  (plain and MaterialAware builds); all four on/off x MaterialAware variants compile. Instruction slots with it on: Composite 555 -> 719,
  Trace 816 -> 889, Blur 535 -> 609 (MaterialAware builds).
- Vulkan: bindings 30 / 31, `cb.gbuffer`; all twelve Reflections*.spv rebuilt and validated.
- Owner's first look (2026-11-20, install-test13): "reflections dont seem to be using the normal bump at all". Cause (from the code, believed):
  step 3's specular AA measured the bumped normal, so every bump became roughness past the trace's hand-over. Fix (install-test14): AA on the
  smooth normal's curvature (kappa 0.015), bumps kept whole near the camera and faded to the smooth normal by pixel footprint (0.5 -> 2.0
  units / pixel), damp ground keeps 20 % of its bump (puddles are flat in the G-buffer; 2026-11-20 owner: 100 %, DampBumpLean 0, install-test16); DebugView 12; DeferredReflections, MaterialAware and
  MaterialBufferRoughness hidden in the menu while UnifiedReflections is on in mode 2 (DeferredBumpedNormals stays: Wetness Effects and
  WetWorld still use it). Reviewed (approve with changes, applied). docs/EFFECTS.md "Reflections" "The wet floors from DeferredWetness's film" has the numbers.
- Not done here: the D3D9 film and specular AA (step 2), coverage of every surface / probe-only (step 3), the env-map pass and metal sheen
  (step 3), the one switch (step 4).

### Step 2 as built (2026-11-20, same branch; NOT yet run in a game)
Under the same switch (`UnifiedReflections`; no new key). One wet reflection on the floors:
- `ReflectionsEffect::WetReach` decides once a frame, first inside the sun pass (DeferredWetness::Apply runs after the G-buffer's flags are set):
  UnifiedReflections used (mode 2, this frame's G-buffer, a back end that reads it), outdoors, above water, Reflections on with its targets,
  `RainWetness` above 0 and SkyFallback on, DeferredWetness's fallback loaded and drawn LAST frame (review: a frame it cannot draw, e.g.
  upscaled before the effects, would leave Reflections without the film and drying floors without any reflection), all four of its targets,
  not the path tracer with PTReflectionsOff. Then it returns DrawDistance, else 0. Log: a line when it changes. (The first wet frame keeps
  the old three reflections; from the second, one.)
- **DeferredWetness** (`Shaders/DeferredWetness.hlsl` DWSkyKeep; c6.z in the pass, c15.w in the fallback): the film's sky reflection is taken
  out by Reflections' own floor share (smooth normal 0.75 .. 0.95 up) and its fade to DrawDistance. Walls, steep slopes and far floors keep it
  (Reflections gives them no wet reflection). The darker albedo, flat puddles, the film and its sun highlight stay. Offline: the pass and the
  fallback compile with the game's cached defines (963 -> 977 and 762 -> 774 slots).
- **Wetness Effects' screen pass** (`TESR_WetnessReflectionBump.w` = the reach): its trace is weighted by the same keep as DWSkyKeep (review:
  skipping it everywhere left wet walls and steep slopes with only the sky), so it reflects nothing on the floors Reflections mirrors and as
  before elsewhere; the lamps' streaks on wet ground still drawn.
- **The D3D9 technique** (`REFL_GBUFFER_ON`) now has the Vulkan passes' wet and normal rules: the film (stage 7, the history's: only Temporal
  reads it, RenderBuffers takes it off for that pass; `TESR_ReflectionsFilm`), FilmWetness on floors and in MaterialReflectivity, the bump's
  footprint fade and the damp lean (the smooth normal decoded from COLOR3 on s8: no new stage), the curvature AA. No DebugView 12 there.
- Step 1's GBufferFrame is now off in a frame upscaled before the effects (RenderScale with LowResEffects off: COLOR1-3 at the drawn size).
- Known small gap (review, believed): DeferredWetness's main pass has no water-height test, so a thin shoreline strip of land at the water level
  loses the sky sheen that Reflections treats as water and does not give back.
- Still doubled, by design for later steps: the forward records' own wet film (the records that stay forward draw their sky term into their
  colour), the env-map pass and the metal sheen (step 3).
- What correct looks like: wet ground in rain shows one reflection (screen hits, else the cube / grid / sky by roughness), no brighter than
  before at a grazing view and a little darker looking straight down (only the Fresnel share now, no added sky on top); the lamps' streaks on
  wet roads as before; walls and steep wet slopes keep the sky sheen. Wrong: wet ground going dull or black where Reflections finds nothing
  (the sky fallback missing: SkyFallback), a hard line where floors meet steeper ground (the share's edge), or no lamp streaks.

### Proven / believed / not run
- **Proven from the code** (file:line above): every path in section 1, the G-buffer layout and COLOR2 b readers, that the Vulkan trace already
  has the source ladder and Fresnel mix, that WetnessReflections and the env pass add their own reflection on top.
- **Believed:** the cost estimates, the relative weight of the two traces, Community Shaders' split, that wider coverage looks better than
  floors-only, that the D3D9 port fits.
- **Not run:** all of it; nothing built, nothing measured.

## Reflection speckle

*Original title: Speckled reflections on bumpy wet ground*

Owner's report 2026-11-21 (save `reflection01`, Goodsprings, sunny after rain, install-test18): the reflection on the damp dirt breaks into
white speckles at mid distance, most toward the sun. Owner asked for an unreasonably ambitious fix.

### Proven (read in the code, `src/hlsl/NewVegas/Compute/`, test-p1a-bump aca9929)
- Damp ground: full bump (`DampBumpLean 0`, test16), blur roughness clamped to at most `DampMaxRoughness 0.3` (7231832, test18).
- `BumpKeep` keeps the whole bump until a pixel covers 0.5 world units (about 12 m at 1440p): the speckled band is inside that.
- `NormalVarianceRoughness` measures the SMOOTH normal only (by design since the bump fix), so the bump adds no roughness anywhere.
- Trace (`ReflectionsTrace.frag`) is half resolution: one ray per 2x2 block, its direction from ONE pixel's bumped normal (GGX-spread);
  a ray that leaves below the bumped surface falls back to the plain mirror; nothing checks it against the real (smooth) surface.
- Composite (`ReflectionsComposite.frag:154-157`): the Fresnel share uses each full-res pixel's own bumped normal.
- Temporal (`ReflectionsTemporal.frag`): history found by the SURFACE's motion, clamped to the current 3x3 min/max; mirrors keep none.

### Believed (not run): why it speckles
1. A ray bent by a bump at a low view angle swings between the bright sky and the dark ground: neighbouring blocks get opposite answers
   (the sky is many times brighter than the dirt), and the 0.3 blur (a few half-res pixels) cannot average a contrast that large.
2. Each full-res pixel takes a reflection traced for another pixel's normal (2x2 block, upsampled) but its own Fresnel: at a low angle
   Fresnel swings from ~0.05 to ~0.6 across a bump, so the same reflection is drawn at very different strengths pixel to pixel.
3. Rays bent below the real ground surface are counted (they should be shadowed by the ground itself: horizon occlusion).
4. The history cannot clean it: it follows the ground, not the reflected thing, and the min/max clamp keeps the noise.
5. Possibly not the reflection at all: the sun's own highlight on the wet film (DeferredSun) also glints on every bump toward the sun.
   The owner's toggle below tells 5 from 1-4.

### Ruled out
- Sub-pixel aliasing as the main cause: at 5-10 m a dirt bump spans several pixels (footprint about 0.4-0.7 units), so it is drawn, not aliased.

### How Community Shaders (Skyrim) does wet ground (read 2026-11-21, github.com/community-shaders/skyrim-community-shaders 2b05afa)
GPL-3.0 with a modding exception (`COPYING.txt`, `EXCEPTIONS.md`); ours is GPL-3.0 too, so adapting is allowed with credit; we still write
our own. Clone: `F:\VS-scratch\ext\cs` (sparse) and `F:\VS-scratch\ext\cs-files`.
- Wet ground has NO screen-space trace. `Lighting.hlsl` adds `wetnessReflectance x GetDynamicCubemapSpecularIrradiance(wetNormal, V,
  wetRoughness)`: one lookup in a cube made each frame from the screen (`Dynamic Cubemaps`: screen pixels splatted into a cube round the
  camera, holes filled from coarser mips, then pre-filtered by roughness), mip = roughness x 8, its brightness normalised to the ambient
  light's and scaled by Skylighting's sky visibility (smooth spherical harmonics).
- Damp ground keeps its bumped normal but is rough: roughness = 1 - wetness gloss (`WetnessEffects.hlsli` GetSurfaceWetnessRoughness).
  Puddles get a flat ripple normal and roughness down to 0.05. Fresnel: split-sum EnvBRDF, F0 0.02.
- Skyrim's own SSR (`ISReflectionsRayTracing.hlsl`) is the WATER's, with one fixed normal; it never runs on ground.
- Why theirs cannot speckle: the cube is pre-blurred, so pixels with different bump normals read neighbouring parts of one smooth
  picture (no sky-or-ground flips), and its brightness is tied to the ambient (no bright sky specks over dark dirt). The price: no exact
  reflections on damp ground (the cube is seen from the camera, not the surface), a soft sheen rather than a mirror.
- For us: the same split costs little. Damp ground (film, no standing water) hands over to our live cube / light grid at its own
  roughness; only standing water (flat in the G-buffer) is traced. Catch: the owner's test10 look ("dont seem to see any reflections")
  was damp ground handed to the grid, so the cube's sheen must be strong enough to read as wet.

### Plan (each tier stands alone)
**Tier 1, about 1 session, deferred Vulkan path only:**
- Fresnel from the smooth normal (or the 2x2 average), the bump kept in the ray direction only.
- Horizon fade: a ray below the smooth surface fades to the surface's own ambient, not sky or ground.
- Bump roughness: the bumped normal's change across the 2x2 block adds to the BLUR roughness only (not to the trace-to-grid hand-over),
  so bumpy damp ground blurs instead of speckling and still bends.
- Clamp a block's sky brightness to its neighbours' (firefly clamp) where the bump is busy.

**Tier 2, 2-3 sessions:**
- The G-buffer write measures its own normal map's variance (ddx/ddy of the bumped normal, or Toksvig from the mip-filtered length)
  and stores it in the roughness: one fix for the sun, the lamps and the reflections (sun glints too).
- Full-resolution trace on wet pixels only (the band where it shows).
- History by the reflected point's motion (hit distance: "virtual" motion), so the rough reflection can average over 8-16 frames.

**Tier 3, the ambitious one, 6-10 sessions, needs NVIDIA (RTX) and the 64-bit helper:**
- Stochastic reflections done properly: several GGX rays per pixel shared with neighbours (ratio estimator, Stachowiak 2015), and a
  real specular denoiser (NVIDIA NRD ReBLUR, open source; check its licence) or DLSS Ray Reconstruction through the DLSS helper, which
  is built for exactly this: noisy ray-traced reflections on rough, bumpy surfaces, guided by normals, roughness and hit distance.
- Sun glitter as a glint model (a counted number of sparkles per pixel footprint, stable and filtered, as in Deliot & Belcour 2023),
  so the wet-road sparkle toward the sun is a deliberate look, not noise.
- Risk: the 64-bit helper and DXVK fork already carry RT; adding NRD or RR is a large new dependency; AMD cards would keep tier 2.

### Open
- (2026-11-21, state at install-test22) Owner's look at `reflection01` with the smooth-normal ray: are the specks gone? The slot never
  reproduced them, and the owner's test19 look (rainy field) showed none. If they remain: Reflections off -> do they stay (then cause 5, the
  sun's film highlight)? DebugView 12 screenshot (red = bend, green = roughness).
- Tier 2 only if specks remain; tier 3 needs the owner's word (new dependency, player-visible, frame cost).

### What was tried
- 2026-11-21 test18 (7231832): damp ground's blur raised to the film's roughness, at most 0.3. Owner at `reflection01`: still speckled at
  mid distance.
- 2026-11-21 test19 (deferred-wetness-ground 197d49f, 9a89f67, 8573bb0; owner: "Yes, try it"): the Community Shaders split. Damp ground (the
  film rougher than the standing water's Puddles Roughness, soft blend over 0.1) is no longer traced: no march, no random spread, the cube /
  light grid / sky along the mirror direction at the film's own roughness (0.61 for the owner). Standing water keeps the trace. The
  composite takes the damp share's Fresnel at most at 0.3 rough (at 0.61 it is a quarter of 0.3's at 5 degrees: the test10 "no
  reflections" look). `[Shaders.Reflections.Main] DampTrace = true` (hidden) brings the trace back for an A/B. Vulkan passes only; RT
  passes unaware (known gap). Details: `docs/EFFECTS.md "Reflections"`, last section. Not yet looked at in game; frame cost not measured
  (believed cheaper). What to look at: `reflection01`: speckles gone? damp ground still reads wet? puddles still sharp?
- 2026-11-21 test20 (144544a): lamps and fires on damp ground came back (bright screen hits kept over the cube).
- 2026-11-21 slot A/B (owner's word; sheets `F:\NVR-bench\shots\refl-ab-sheets`, `refl-ab2-sheets`): PROVEN the cube cost the puddle edges
  (fixed: ff12370) and the Strip's crisp wet-road mirror (robots, benches, lamp posts); PROVEN an "only bumpy damp ground" rule (378b306)
  does nothing useful: DebugView 12 shows the Strip's asphalt bends more than Goodsprings dirt, so almost nothing reached the cube. NOT
  REPRODUCED: reflection01's white specks, with either version (the slot's weather and sun differ). No flicker in any case (pairs 2 frames
  apart: about 0.2 of 255).
- 2026-11-21 test22, owner's choice "Smooth-normal ray": damp ground traced again (DampTrace defaults to true), and at the film's full wet
  share the reflection's normal is the smooth one (DampBumpLean 1, unified branch e86485b): the ray, the blur and the Fresnel follow the
  smooth ground, so the sky/ground flips (cause 1), the per-pixel Fresnel swings (cause 2) and rays under the bump (cause 3) are gone on wet
  ground (believed, from the code). Price: bumps no longer bend the wet reflection. The cube path stays for DampTrace = false.
- Not tried: the bump's variance in the roughness (tier 2), tier 3.

## Old fork reflections (NVR-Unofficial-Optimized)

Read 2026-11-27 (owner: "they seemed better back then"): github.com/mrfury97/NVR-Unofficial-Optimized, main 4acb2504 (P79, 2026-10-13),
`src/hlsl/NewVegas/Effects/Reflections.fx.hlsl` (411 lines) and `src/effects/Reflections.cpp`; clone `F:\VS-scratch\ext\nvr-unofficial`.
Separate git history from this repo, but our `Reflections.fx.hlsl` grew from the same design (`Tame`, `BrightestReflection`, the floor rule
are still in it). Builds P80-P84 sit disabled in MO2 (`NVR-Unofficial-Optimized-P8x...`). No pictures compared yet.

What the old effect did (proven from the code):
- **Floors only:** a pixel reflects only when its smooth normal faces up (`world z > 0.75`, full at 0.95); walls, vehicles, benches, guns
  never ran through it. Amount = GroundSheen (dry) + RainWetness, with Roughness 0.4 dry / WetRoughness 0.08 wet.
- **Dielectric Fresnel only:** split-sum with F0 0.04 (`EnvironmentFresnel`); nothing was treated as metal here.
- **Smooth normals** (the normals buffer, no normal-map bumps): no speckle, no DXT tilt.
- **One GGX-sampled ray per half-res pixel**, 24 quadratic steps, 5 binary refine steps; hit rejected on the weapon and back faces;
  confidence fades at the screen edge and the ray's end.
- **Misses:** a screen-filled cube (16 taps per texel, brightness clamped at 32, CubeCapture 0.02 / CubeFade 0.005; no world render, as
  Community Shaders' Dynamic Cubemaps), then the sky gradient for rays pointing up.
- **Noise control:** `Tame`/`Untame` (luma-compressed accumulation), brightest reflection 16 (mirror) down to 3 (rough); temporal 0.85
  with a 3x3 neighbourhood clamp; depth-aware separable blur up to 12 half-res pixels, wider when rough.
- Its metal look on objects was the PBR pass alone (`[Shaders.PBR.Metal] Reflections 0.6`, cube from the screen); `FromGameData` did not
  exist yet (2026-11-09).

What ours adds that the old one never had (each a believed source of today's harsher look): objects and metal reflect by material
(MaterialAware / UnifiedReflections, metal F0 = colour), the 3D world cube (CubeMode 2) and panorama, the light-grid fallback, RT rays,
weapon reflections, normal-mapped normals on some paths, and the upstream 1.1.x layers on top. Not run: an A/B of P84 against the current
build at the same spots.

## SSFX rain comparison

*Original title: Screen Space Shaders (STALKER Anomaly) rain and wet reflections vs ours*

Owner's question (2026-11-21): can the outdoor rain reflection code of github.com/dsh2dsh/screen_space_shaders be ported?
Read from a shallow clone at `F:\VS-scratch\ext\screen_space_shaders` (commit a9fe1aa, "Update 18.1", 2023-10-19). It is Ascii1457's
Screen Space Shaders for S.T.A.L.K.E.R. Anomaly (X-Ray engine, D3D11 `r3` shaders), mirrored by dsh2dsh.

### Proven (read in the code)
- **No licence.** No licence file and no licence or permission text in any file; headers name the author and the ModDB page only.
  So: ideas may be re-written in our own words, no code is copied, and their textures (ripple and water-fall normal maps, the per-map
  puddle masks) never go into our repo or install.
- What its rain wetness and rain reflections are (files under `96 - IMPROVED GLOSS & WET SURFACES`, `05 - Rain - Puddles`, `03 - SSR`):
  - `rain_patch_normal.ps`: one deferred pass over the G-buffer. A rain cover map seen from above (the engine's own rain shadow
    map) decides what is open; open surfaces get rain ripples from a texture (up-facing), a scrolling "water fall" normal texture in
    50 columns on X- and Z-facing walls, and a gloss raise by rain wetness. Flora is skipped, undersides get no gloss, gloss fades out
    200-250 m, the cover counts as open past the rain map's far edge.
  - `deffer_impl_flat.ps` (terrain): puddles = a Perlin texture threshold x a hand-painted puddle mask per STALKER map x a flatness
    mask; inside, the normal is set straight up plus two scrolling ripple samples and rain ripples; the ground's detail texture is read
    with its UV pushed by the ripple normal (a fake refraction); a tint; the puddle writes its gloss into the G-buffer.
  - `screenspace_reflections.h`: a plain linear screen-space ray march (8-64 steps, one binary step at high quality), the sky or
    an environment colour where it misses, Fresnel = pow(cos, 3), strength = gloss. Terrain keeps the sky reflection near the screen
    edge; the first-person weapon gets extra reflection in rain.

### Compared with ours (Puddles, DeferredWetness, Reflections on `test-p1a-bump`)
| Theirs | Ours |
|---|---|
| Rain cover from a top-down map | Have it (ortho map cover test, 40-unit look-out on walls; `includes/Wetness.hlsl`) |
| Rain ripples (texture) | Have it (procedural rings `WetRainDrops`, stronger with rain) |
| Puddles: noise x painted mask x flatness | Have more: world noise + the land's height crevices + slope + cover, fill and dry over game hours, soaked ring, murk |
| Wet gloss raise, undersides dry | Have it; undersides keep `MinRainWetness` instead of 0 (a look choice) |
| Wall water streaks | We had them; removed 2026-11-18 on the owner's request |
| Linear SSR + sky | Ours is well past it: SSR or RT, live cube, light grid, sky ladder, rough blur, film-aware (Reflections + DeferredWetness) |
| Fake refraction inside puddles (ground wobbles under ripples) | **Missing in ours** |
| Extra first-person weapon reflection in rain | Not in ours (the first-person model is forward and dry; DeferredFirstPerson off) |

### Believed (not run)
- Worth taking, as an idea written fresh: **puddle refraction**. In `DeferredWetness.hlsl`, inside puddles, read the albedo a few
  pixels off along the ripple normal (scaled by water depth), so the ground under the water wobbles with the rain. Deferred path,
  one extra G-buffer read per puddle pixel; about one session including the offline compile check. Player-visible but only adds
  to puddles; frame cost believed small, not measured.
- Not worth porting: their SSR (weaker than ours), the water-fall streaks (removed by owner's choice), the painted masks (per STALKER
  map, their asset), the ripple texture (ours is procedural, no free sampler in the object shaders).

### Ruled out
- A direct port of the code: no licence, different engine (X-Ray D3D11 `r3`, Lua-script driven settings), and every piece but the two
  rows above already exists in ours in a stronger form.

### Open
- Owner's call: add puddle refraction (above)?

## Pipeline audit

*Original title: Pipeline audit: what overlaps, what can go, what can be combined (2026-11-21)*

Owner's request (via Main brain): "check the whole rendering pipeline and see what is overlapping, what can be dropped, and what can be
combined", then "be unreasonably ambitious". Read-only study: nothing was built, installed or run for it.

- **Code read:** branch `test-p1a-bump` at e687e58, the code of install-test19/20. That is main plus the open branches deferred-phase1a,
  deferred-wetness-ground (Rain wetness), unified reflections, bumped normals, the menu clean-up and the PipboyFit/SMP fixes.
- **Owner's settings:** from the owner's last log, `VegasShaders_2026-11-21_12-41-27.log`.
- **Frame times:** the only measured numbers come from one dry F10 profile (`VegasShaders_2026-11-20_21-17-53.log`). No rain frame and no
  interior frame has been profiled. Every other number below is arithmetic (passes x pixels x taps) or a code comment, labelled so.
- **Labels:** PROVEN means the code path was read end to end, by a helper and, where marked "checked", by me. BELIEVED means inferred.
  NOT MEASURED means no timing exists.
- **Paths:** deferred (G-buffer and the deferred passes), forward (templates), post (screen pass, the same on both paths), Vulkan-only,
  D3D9-only.
- **Owner's word:** anything below that changes what players see, or changes frame cost noticeably, needs the owner's word before it is
  built.

### The short answer
1. **There is less waste than the number of effects suggests.** Many merges already exist:
   - post-tonemap fusions: Coloring + LUT, and Cinema + SkyDither;
   - the fog composite that applies shadow, AO and interior darkening;
   - AmbientLight taking over AO and BounceLight;
   - bounce into specular;
   - SMAA standing aside for TAA;
   - identity skips for exposure, ImageAdjust and LUT;
   - idle parking of off effects' targets.

   At default settings the post-tonemap chain is only TAA, the distant depth of field and one fused Cinema pass.
2. **The real per-frame waste is in the foundations,** for every player on every frame:
   - depth and normals are built twice when the upscaler is on, which is the default;
   - the normals buffer is written twice;
   - linear depth is made twice under the deferred renderer.

   Together this is about 0.3-0.6 ms at 1440p (NOT MEASURED, arithmetic), with no look change.
3. **The biggest tangle is wetness and reflections:**
   - 7 switches for one look;
   - 4 rain clocks;
   - 4 puddle fields;
   - 2 screen traces on wet ground when Unified reflections is off;
   - WetWorld still counted in rain after its pass stopped drawing.

   Most of it is already on the way out: Rain wetness step 4, Unified reflections, and the `retire-wetworld` session.
4. **One bug found on the way (PROVEN, checked):** in the owner's settings, wet objects (roads, slabs, walls) get no lamp or fire streaks
   at night (item Q1).
5. **The big wins are structural,** and they are in the "Big options" section, ranked:
   - one lighting resolve on the Vulkan path;
   - RT sun replacing the cascade maps inside its range;
   - one shadow / ambient / reflection quality ladder each;
   - a scratch-target pool with per-pass resolution.

### Ranked list, best value first
| Rank | Item | Kind | Saving | Players see | Effort | Needs owner's word |
|---|---|---|---|---|---|---|
| 1 | Q1 lamp streaks missing on wet objects (bug) | fix | - | streaks back | 0.5 | yes (look) |
| 2 | B2 depth and normals once (Q3 + M2 + M3) | combine | 0.3-0.6 ms every frame, everyone (NOT MEASURED) | no | 2-3 | yes (frame cost) |
| 3 | Quick-win batch Q4-Q7, Q9-Q12 | drop | small; load time; memory | no (Q12: two options shown) | 1 | Q12 only |
| 4 | Q2 WetWorld gate (with `retire-wetworld`) | drop | 0.1-0.5 ms in rain (NOT MEASURED) | no | 0.3 | no |
| 5 | M1 interior fold on by default | combine | 0.24-0.33 ms indoors (measured, comment) | practically no | 0.5 | yes |
| 6 | B1 finish one wet + reflection system (M5, M6, M8, M9) | combine/drop | one trace + one pass in rain; ~15 switches become 2 | one consistent wet look | 4-6 (in flight) | yes (step 4) |
| 7 | B3 RT sun skips near cascades; contact into DeferredSun; shadow ladder | combine | possibly ms for RT users (NOT MEASURED) | lighter ambient in contact shadows | 5-7 | yes |
| 8 | M4 / B6 exposure into the tonemap, one HDR finish pass | combine | 0.1-0.4 ms (NOT MEASURED) | no | 2-4 | yes |
| 9 | B4 one Vulkan lighting resolve | combine | 0.5-1.5 ms (NOT MEASURED; profile first) | no | 8-12 | yes |
| 10 | B5 scratch pool, lazy targets, resolution ladder | combine | 50-70 MB at 1080p; a new perf knob | no / new option | 3-5 | yes (new option) |
| 11 | B8 Phase 2 deferred switches made default (owner already runs four) | move to deferred | unknown | lamps light more | 0.5 each | yes |
| 12 | B7 ambient and lamp-shadow ladders | combine | menu | simpler menu | 2-3 each | yes |
| 13 | M7 cube cadence, M12 lazy records, M13 fog cascades, M14 lamp class stencil | combine | small each | no | 0.5-2 each | per item |
| 14 | B9 cut BloomLegacy, Specular, FXAA, BounceLight | drop | 0 at defaults | players who use them | 0.3-0.5 each | yes |

### Part 1: quick wins
Each is no look change unless stated, and costs 0.5 session or less. All could go in one build, one test run.

| # | What, where | Path | Evidence | Saving | Risk | Players see | Effort |
|---|---|---|---|---|---|---|---|
| Q1 | **Bug: no lamp streaks on wet objects in the owner's settings.** The objects' (and the far land's) lamp film is drawn only inside the Wetness Effects reflection screen pass. `WetnessReflectionsEffect::ShouldRender` returns false when `[Shaders.WetnessEffects.Reflections] Enabled` is false (`WetnessReflections.cpp:43`), and the owner has it false (log line 19). Land streaks are unaffected (DeferredLamps `DL_LAND_FILM_ONLY`). Fix: run the pass for the film alone when Reflections is off, or move the object film into DeferredLamps' class passes. | deferred | PROVEN, checked (code + log) | none (adds work) | low | yes: streaks come back on wet roads and walls | 0.5 (film-only run) |
| Q2 | **WetWorld still counted in rain after its pass stopped drawing.** `effectsBetween` (`EffectChain.cpp:635`) and `bounceIntoSpecular` (`:838`) test `wouldRender(WetWorld)`. `WetWorld::ShouldRender` (`WetWorld.cpp:133`) ignores Wetness Effects, while the draw is skipped separately (`EffectChain.cpp:946`). So in rain with Wetness Effects on, the fog-composite fold (two full-res passes, per its comment) and the bounce + specular merge are lost. The same applies to `FadedContactUnread` (`ShadowsExterior.cpp:789`). Fix: one "WetWorld draws" test. Not in the `retire-wetworld` diff; give it to that session. | post | PROVEN, checked | 0.1-0.5 ms in rain (NOT MEASURED, from the dry 0.06-0.25 ms passes) | low | no | 0.3 |
| Q3 | **Normals redrawn at full size after the upscale.** `TAAUpscale.cpp:753` and `:801` call `RenderDepthAndNormals` again at display size every upscaled frame (RenderScale 0.67 + FSR is the default). After that point only Snow, Debug and DLSS RR read the normals, and all are off by default. Keep the depth; skip the normals unless one of those is on. | post | PROVEN for the second run (checked); BELIEVED for "no other reader" (grep, not every C++ reader traced) | 0.1-0.3 ms at 1440p, every frame (NOT MEASURED) | a missed reader gets stale normals | no | 0.5 |
| Q4 | **PointShadows2 compiled and bound but never drawn.** It is drawn only when `!mergedPointShadows` (`EffectChain.cpp:447`); the shipped PointShadows.fx has `MergedPointShadows`. Drop the enrolment (`ShaderManager.cpp:126`) or load it lazily. | both | PROVEN (checked) | load time only | custom fx without the merged technique | no | 0.3 |
| Q5 | **FXAA does not stand aside for TAA or SMAA** (`EffectChain.cpp:1463`, no gate), unlike SMAA (`:1450`). | post | PROVEN | ~0.15 ms when a player turns it on with TAA; 0 at defaults | low | no (double AA only) | 0.2 |
| Q6 | **WetnessReflections works out wetness for every pixel, then overwrites it where the film exists** (`WetnessReflections.fx.hlsl:173` vs `:180-206`). Fix: `[branch]` on the film flag. | deferred | PROVEN (order); BELIEVED (fxc does not sink it) | 0.1-0.4 ms in rain with that pass on (NOT MEASURED) | low | no | 0.3 |
| Q7 | **Deferred wetness: sky reflection computed, then multiplied by `DWSkyKeep` = 0 on floors** (`DeferredWetness.hlsl:206`, `:320-321`). Fix: branch first. | deferred | PROVEN | tiny | none | no | 0.2 |
| Q8 | **WetWorld's unused keys:** `PuddleCoeff_R/G/B` and `PuddleSpecularMultiplier` feed `TESR_WetWorldCoeffs`, which no shader reads (`WetWorld.fx.hlsl:20`; no Vulkan reader). Goes with WetWorld's retirement. | both | PROVEN | none | none | no | with retire-wetworld |
| Q10 | **Dead keys and files out:** the 12 keys read by nothing and the orphan `ObjectRefractionTemplate.hlsl` (see "Dead settings"). Not MotionBlur's per-view Enabled: wiring that is a behaviour change. | both | PROVEN | none | none (a key missing from the defaults is simply not read) | no | 0.5 |
| Q11 | **AmbientOcclusion's targets parked while AmbientLight holds it off** (the default), and the same for VolumetricClouds and the other off effects without `TargetParkable`. | both | PROVEN no override; BELIEVED sizes | memory only (AO half + quarter G16R16F pairs; clouds) | low | no | 0.5-1 |
| Q12 | **Menu hides two live keys:** `PlayerShadowFirstPerson` / `ThirdPerson` are on the dead list (`ImGuiManager.cpp:803-804`) but are read (`ShadowManager.cpp:1684`). | menu | PROVEN | none | none | yes: two working options become visible again | 0.1 |
| Q9 | **Stale docs:** CombineDepth section 7 (docs/EFFECTS.md) says DepthNormals.frag cannot compile for lack of `viewNormalFromTaps`, but `Normals.glsl:54` now defines it. DepthOfField section 9 (docs/EFFECTS.md) item 6 says the distant technique's source copy is open, but `EffectChain.cpp:1406` already skips it. | docs | PROVEN | none | none | no | 0.1 |

### Part 2: medium items
Each is 1-2 sessions and worth doing on its own. Ranked by value: saving x how often it runs / risk.

| # | What, where | Path | Evidence | Saving | Risk | Players see | Effort | Default |
|---|---|---|---|---|---|---|---|---|
| M1 | **Interior fold on by default.** `InteriorShadowFold` (`[Main.Main.Performance]`, off, defaults :427) folds the KeepDarkening apply into the fog composite (`CompositePlan`, `EffectChain.cpp:647`). | deferred (indoors) | PROVEN code; saving measured in two interiors (code comment) | 0.24-0.33 ms indoors (comment, measured) | about 50 pixels a frame differ by a few 16-bit steps at ceiling gaps | practically no | 0.5 (a look, then owner's word) | off |
| M2 | **Normals written once.** The Normals pass writes every pixel, then `CapturedNormals::MergeIntoNormals` (`CapturedNormals.h:651-700`, called `ShaderManager.cpp:2325`) rewrites most pixels from COLOR3, rebuilding its own facet normal from ddx/ddy. One technique that reads COLOR3 and falls back to the depth facet would do both. | both | PROVEN (two helpers independently) | ~0.1 ms at 1440p, every frame (NOT MEASURED) | edge pixels may flip accept/reject (2x2 derivative vs better-of-two-neighbours); Vulkan `Normals.frag` twin | maybe at silhouettes (A/B picture needed) | 1 | on |
| M3 | **Linear depth made twice under the deferred renderer.** `DeferredDepth` linearizes into `TESR_DeferredDepthLinear` (R32F, `DeferredDepth.h:213`); then `ResolveDepthBuffer` + CombineDepth write another R32F (`/far`, plus the view-model merge). CombineDepth could start from the deferred one for the world part. | deferred | PROVEN both exist; BELIEVED same world values (not checked whether anything draws depth between the span end and the chain) | 0.05-0.15 ms + one R32F (~15 MB at 1440p) (NOT MEASURED) | view-model and sky edges; units differ | no | 1-2 | on (mode 2) |
| M4 | **Exposure folded into the tonemap.** Exposure is a full-res HDR pass that multiplies by one scalar from the 1x1 AvgLuma texel (`EffectChain.cpp:1213-1221`). The tonemapper already takes an exposure scalar (`Tonemapping.hlsl:538`). Bloom's first downsample and LocalExposure would apply the scalar on their taps. | post | PROVEN | 0.1-0.2 ms at 1440p by day and indoors (NOT MEASURED) | Exposure Mode 1 (GT7) adaptation; the tonemapper's sampler budget; Lens and pre-tonemap LUT read the exposed frame | no if exact | 2 | on |
| M5 | **Wetness and reflection switches made one.** DeferredWetness, UnifiedReflections, DeferredBumpedNormals, DeferredReflections, DeferredWetLampFilm, MaterialAware and MaterialBufferRoughness all serve one wet-reflection look. The owner runs them all on. `FdIndirect.h:37` keeps BumpedNormalsOn separate because Wetness Effects and WetWorld read it. Map them to Rain wetness + one Reflections quality level, with a migration in `UpdateOldDefaults`. | deferred | PROVEN | none (menu) | saved toml values | no | 1 (part of Rain wetness step 4 / Unified reflections step 4) | mixed |
| M6 | **Four rain clocks:** RainState + the Rain animator (read by Reflections' rain guess `Reflections.cpp:749`, GearWeather `GearWeather.h:143`, HeatHaze, Skin, Terrain/PBR "Rain" groups); Wetness Effects' weather percent (~20 s ease, `WetnessEffects.cpp:83-120`); Puddles' own game-hours fill (`Puddles.cpp:75-86`); CoverMap (off). The ground dries on one clock, skin and gear on another. | both | PROVEN | none | timing changes | yes, small: wet skin, gear and ground in step | 1-2 (in progress: `retire-wetworld` moves the clock to Wetness Effects) | on |
| M7 | **Reflection cube redrawn every frame:** 48 draws (6 faces + 42 filter), `Reflections.cpp:1249-1298`, blending only 2 % a frame (`CubeCapture 0.02`). Update every 2-4 frames at a higher blend. | both | PROVEN | 0.05-0.15 ms (NOT MEASURED; whole Reflections step 0.50 ms p50 dry, measured) | lag when turning fast | maybe | 0.5 | Reflections off by default; owner on |
| M8 | **Two screen traces on wet ground when Unified reflections is off.** WetnessReflections ignores Reflections (`WetnessReflections.cpp:42-52`). With Unified on, `reflKeep` returns early but the pixel shader still runs for every wet pixel. With Reflections on, the wet pass should keep only its film. | deferred | PROVEN gating | 0.4-1 ms at 1440p in rain (design-doc estimate, NOT MEASURED) | wet walls' sheen | maybe | 0.5 | not default |
| M9 | **Deferred wetness fallback + Wetness screen pass:** two full-screen passes back to back, each with its own frame copy (`DeferredWetness.h:266-377`, copy at `:294`; then `EffectChain.cpp:934`). | deferred | PROVEN passes; BELIEVED merge feasible (the march reads the lit frame the fallback changes) | 0.1-0.3 ms in rain (NOT MEASURED) | medium-high | no | 1.5 | not default |
| M10 | **Dry-frame restore for the land height trick:** a material copy every frame, dry ones too, plus a 0.028 ms draw (`DeferredWetness.h:137-154`; the copy is outside the timer). | deferred | PROVEN; draw measured | ~0.03 ms + one copy | records would have to write 20 directly when dry | no | 0.5 | not default |
| M11 | **Cinema + SkyDither fusion drops out when ImageAdjust runs** (`EffectChain.cpp:1293`), giving three passes. Fold ImageAdjust into the fused technique. | post | PROVEN | two LDR full-res passes (~0.1-0.2 ms) for tuned installs only | order of ops; Vulkan twin | no | 1 | identity by default |
| M12 | **Shader records on first use.** In mode 2 each lit object shader loads up to five records: forward Default, forward Interior, lighting-free and two glass forward+ records (`ShaderManager.cpp:1129-1196`, `:1348`; `DeferredTransparents.h:89-117`). Load the forward+ and Interior ones lazily. Related open item: failed forward compiles are retried every start. | both | PROVEN record count; BELIEVED usage share | start-up compile and load time, driver memory (NOT MEASURED) | a first-use hitch | maybe a stutter | 1-2 | on |
| M13 | **Fog samples the four sun cascades again** at the surface point DeferredSun already resolved (`VolumetricFog.fx.hlsl:630-660`). The froxel path may avoid it (not checked). | post | PROVEN for the classic path | NOT MEASURED | DeferredSun would export visibility; fog also covers forward pixels | no | 1-2 | on |
| M14 | **Deferred lamp class passes:** the main, land, skin and land-film lamp passes are separate full-screen draws, each clipping per pixel by class (`DeferredLamps.hlsl:58-71`, `:359-372`). `DeferredTiledLight` (off by default, ON for the owner) limits them to lamp tiles; a stencil mark per class would avoid re-reading the G-buffer. | deferred | PROVEN | NOT MEASURED | stencil shared with the first-person mark (`DeferredLamps.h:110`) | no | 1-2 (DeferredTiledLight default: 0.5 + owner's word) | on |

Low value, recorded so nobody re-checks:
- **Water's planar reflection pass** (`ForceReflections`, default true, `Render.cpp:5238-5306`): 0.33 ms GPU + 0.24-0.29 ms CPU in the dry
  profile (measured). It also fills the water's depth maps; skipping it once gave streaky water. High risk; keep unless RT holds the water
  (`WaterSkipPlanar`, ~1.7 ms in RT scenes per its toml text, untested).
- **VisibleRanking** (on): up to 128 small draws and occlusion queries a frame; the froxel lists carry no occlusion, so they are no
  substitute. Weak.
- **The facet normal is rebuilt from depth in five shaders** (AO sky term, `Reflections.glsl:340`, `RtAOInput.frag`, `RtSunShadow.comp`,
  Normals). Deliberate (`docs/DESIGN.md "Deferred vs forward":149-150`): a shared include is code tidiness only, no time.
- **Normals smoothing in two places** (Normals `SmoothNormals`, default 0, vs AmbientLight's own smooth): never compared (docs/EFFECTS.md "Normals"
  open question 4). BELIEVED overlap.
- **Private fp16 copies per deferred pass** (DeferredLamps, Flashlight, DeferredFog, DeferredSkin, DeferredWetness, LinearScene): mostly
  never allocated under linear light. LinearScene's "a few conversions a frame" were not counted.

### Part 3: big options (the ambitious list)
Ranked by value: how much frame time or complexity it removes for how many players, against risk. Every one changes frame cost and
most change the look a little, so each needs the owner's word before it is built.

#### B1. Finish the one wet + reflection system (already in flight)
- **What:** at the end there is one Rain wetness switch (DeferredWetness default on) and one Reflections quality level (Unified
  reflections). WetWorld, the Wetness Effects screen trace, three of the four puddle fields, three of the four rain clocks, Reflections'
  GLSL copy of the puddle noise and the old switches all go: M5, M6, M8, M9, Q2, Q6, Q8, plus the lighting-free records' wetness code.
- **Evidence:**
  - PROVEN that every piece listed exists and runs; the reflections/wetness helper and this session checked them.
  - The owner already runs the end state's switches: the design docs `docs/DESIGN.md "Deferred wetness design"` and `docs/DESIGN.md "Unified reflections design"`,
    plus the 2026-11-21 log.
- **Saving:** NOT MEASURED. It removes one screen trace (0.4-1 ms, design estimate) and one full-screen pass + copy in rain. Its biggest
  gain is complexity: about 15 switches become 2.
- **Risk:** medium. D3D9 Reflections has no film for every case. The RT reflection passes are unaware of the film. Partly damp pixels
  still march.
- **Players see:** one consistent wet look. Rain wetness on by default is a visible change: owner's word, already planned as step 4.
- **Effort:** 4-6 sessions left (Rain wetness step 4 + Unified reflections steps 3-5).

#### B2. Depth and normals once (every player, every frame)
- **What:** one "foundations" pass family, built once per frame. Q3 (no normals redraw after the upscale), M2 (normals written once),
  M3 (one linear depth) and a shared facet-normal include.
- **Evidence:** PROVEN per item (see Q3, M2, M3).
- **Saving:** about 0.3-0.6 ms at 1440p on every frame for everyone (NOT MEASURED, arithmetic). The best saving-per-session in this audit.
- **Risk:** low-medium. Silhouette edges in the normals; the view-model depth merge.
- **Players see:** nothing (an A/B picture to prove it).
- **Effort:** 2-3 sessions.

#### B3. RT sun shadows replace the cascades inside their range, and one shadow quality ladder
- **What:** with RT sun shadows on (Mode 1), the cascade shadow maps are still drawn in full. There is no RT test anywhere in
  `ShadowManager.cpp` (checked: no `RtSunShadows::` reference); Mode 1 only switches off the fused forward-contact technique. Skip the
  near cascades the RT range covers, and keep the far ones for the fade.
- **Contact shadows into DeferredSun:** today the exterior screen-shadow chain multiplies the whole lit pixel, ambient and lamps
  included. It then adds the lamp term back (`Shadow.r += Shadow.g`), which is why the PointShadows lamp mask runs outdoors
  (`EffectChain.cpp:465-511`, `715-742`). Contact shadows on the sun term only would drop the lamp compensation and the apply.
- **Ladder:** one Shadows quality setting: cascades -> + contact -> + RT sun (cascades skipped near) -> path tracing. It replaces
  RT.Enabled, Mode, ForwardLookup, HalfResolution, ScreenSpace.*, Bend and DeferredRtSunFirst.
- **Evidence:** PROVEN that the cascades are drawn under RT (frame-graph helper; checked) and for the contact chain (lighting helper).
- **Saving:**
  - RT users: the near cascades' scene draws, the biggest single CPU + GPU item in shadows. It could be milliseconds in dense scenes
    (NOT MEASURED; the cascade draw cost was never profiled).
  - Everyone: the outdoor lamp-compensation mask + apply (NOT MEASURED).
- **Risk:**
  - Cascade fade seam at the RT range edge.
  - RT's frame of delay with `ForwardLookup`.
  - Contact shadows stop darkening ambient: a visible look change.
  - WetWorld, Underwater and Debug read the shadow buffer.
- **Players see:**
  - RT: nothing if the seam is clean.
  - Contact: slightly lighter ambient inside contact shadows. That is deferred-first and more correct, but visible.
- **Effort:**
  - 2-3 sessions for the cascade skip;
  - 2-3 for contact into DeferredSun;
  - 1 for the ladder.

#### B4. One lighting resolve for everything that reads the G-buffer (Vulkan path)
- **What:** today a deferred pixel is read by many separate full-screen passes:
  - DeferredSun;
  - up to four DeferredLamps class passes;
  - DeferredWetness;
  - the shadow-apply / AO / ambient composite (AmbientLight apply 0.25 ms, measured dry);
  - the fog composite (0.06 ms, measured);
  - the Reflections composite;
  - the skin pass.

  Each re-reads 3-4 fp16 targets.
- **The proposal:** one tiled compute resolve on the Vulkan path. It reads the G-buffer once per pixel, then does:
  - the sun with shadow;
  - the froxel lamps, already in a table (`ManyLamps.h:132`);
  - ambient and AO;
  - the reflection term from Unified reflections;
  - the wet film;
  - and writes the lit pixel once.

  The ps_3_0 limits that forced the split (32 temps, X4505) do not exist in compute. D3D9 keeps today's passes as the fallback.
- **Evidence:**
  - PROVEN that the passes and their G-buffer reads exist (lighting helper, items 8-9).
  - BELIEVED that bandwidth dominates their cost.
- **Saving:** NOT MEASURED. Each full-res G-buffer read + write at 1440p is roughly 0.1-0.25 ms (dry profile scale), so merging 5-7
  passes could save about 0.5-1.5 ms. A rain + interior profile must come first.
- **Risk:** high.
  - Two lighting implementations to keep equal (Vulkan resolve vs D3D9 passes).
  - The DXVK fork interface.
  - Ordering with fog and reflections.
  - Native D3D9 players never get it.
- **Players see:** nothing if matched, and the way to lamp-heavy scenes without the 4-lamps-per-target shadow batching.
- **Effort:** 8-12 sessions. The largest item here; only worth starting after B2 and a profile.

#### B5. A frame graph light: scratch pool, lazy targets, per-pass resolution
- **What:**
  - **Scratch pool.** About 185 bytes per screen pixel is held in screen-sized targets: ~385 MB at 1080p, ~680 MB at 1440p
    (BELIEVED, hand sum). Nothing aliases. Half-res fp16 scratch targets with disjoint lifetimes could share a pool of 3-4 textures,
    replacing about 14 allocations:
    - GodRays Buffer0/1 (step 37);
    - Fog Buffer0/1 (34);
    - VolumetricLight March/Temp (35);
    - Reflections Trace/Temp (32);
    - DustMotes (35b).

    `FrameChain::Pair` is the precedent. Histories cannot alias.
  - **Lazy targets.** Allocate off effects' targets on first use: LocalExposure and VolumetricClouds are off by default but always
    allocated (not parkable).
  - **Resolution ladder.** One "effects resolution" setting replacing the hard-coded `MaxQuality::Reduced(w, 2/4)` divisors. There is
    no per-pass scale setting today, and none for reflections, fog or volumetric light.
- **Evidence:** PROVEN for "no aliasing" and "no per-pass scale"; the memory figures are BELIEVED.
- **Saving:**
  - Memory: about 50-70 MB at 1080p from the pool alone.
  - Frame time: only through the resolution ladder, which is a player knob, not a free saving.
- **Risk:** medium. A lifetime mistake shows as garbage in one effect; half-res needs an edge-aware upsample.
- **Players see:** the pool, nothing. The ladder is a new option.
- **Effort:** 3-5 sessions.

#### B6. One HDR "finish" pass
- **What:** Exposure (M4), bloom composite, tonemap, pre-tonemap LUT, Coloring, Cinema, ImageAdjust and SkyDither as one or two passes.
- **Evidence:** PROVEN that the post chain is already lean at defaults (TAA, distant DoF, one fused Cinema pass). The extra wins are
  Exposure and the ImageAdjust case.
- **Saving:** 0.2-0.4 ms (NOT MEASURED).
- **Risk:** medium. Order of operations; constant and sampler budget in the tonemapper.
- **Players see:** nothing.
- **Effort:** 3-4 sessions. Modest value: most of the post merging is already done.

#### B7. Ambient and lamp-shadow quality ladders
- **Ambient:** SSAO -> GTAO -> AmbientLight -> RT AO/GI.
  - It replaces AO.Enabled, GTAO, Skylighting, RT.Enabled, SunGI, RoomGI, BounceLight.Enabled, AOLowRes and NvrDxvkAmbientLight.
  - The takeover already makes the passes exclusive (`EffectChain.cpp:599-608`).
  - One gap: with AmbientLight off, BounceLight and RT GI can both run (BELIEVED, no guard found).
- **Lamp shadows:** capsules (removed 2026-11-27) -> cube maps -> RT. It replaces Enabled, Live, SkipTracedCubes, ActorsFromCubeMaps, ManyLights,
  ShadowDenoiser and DenoiseLamps.
- **Saving:** none in itself; menu clarity, and the double-run gap closed.
- **Risk:** saved settings need a migration.
- **Players see:** a simpler menu.
- **Effort:** 2-3 sessions each.

#### B8. Move more forward groups to deferred (road map Phases 2-4)
- **Phase 2 (cheap):** the owner already runs DeferredFirstPerson, DeferredTiledLight, DeferredRtSunFirst and DeferredSkinBlur on.
  Making the ones that look right the default is 0.5 session each plus a look and the owner's word. This is the best value inside B8.
- **Phases 3-4:**
  - water forward+;
  - fading objects;
  - late grass;
  - deferred decals;
  - distant trees;
  - folding EnvTemplate's cube layer into Unified reflections;
  - DeferredSun's missing terms.

  10-14 sessions. Mostly correctness (every lamp lights them); the frame-time effect is unknown until Phase 0 measures the forward
  share.
- **Evidence:** `docs/DESIGN.md "Deferred roadmap"`, `docs/DESIGN.md "Deferred vs forward"`; owner's log for the switches.
- **Risk:** per group, low-medium.
- **Players see:** lamps lighting what they don't today.

#### B9. Cut whole effects whose job another effect now does
Each removal is a product decision: players who turned it on lose it. Each costs 0 at defaults today and only adds menu and code weight.

| Effect | Replaced by | Evidence | Effort |
|---|---|---|---|
| WetWorld | Rain wetness | in progress (`retire-wetworld`) | - |
| PointShadows2 | PointShadows' merged technique | PROVEN never drawn (Q4) | 0.3 |
| BloomLegacy | Bloom | BloomLegacy section in docs/EFFECTS.md: "an older, alternative bloom"; off | 0.5 |
| Specular | DeferredSun's GGX highlight + Unified reflections' sky | PROVEN it re-lights the sun highlight from depth-rebuilt normals; off; ~0.4 ms when on (its comment) | 0.5 |
| FXAA | TAA / SMAA | off; no gate (Q5) | 0.3 |
| MaterialCapture / ReflMaterial alpha path | the G-buffer COLOR2 under Unified reflections | PROVEN; Unified already skips its copy (`Reflections.cpp:788`); the `_rmaos` roughness overrides must be in the G-buffer first (not checked) | 2 (inside B1) |
| BounceLight | AmbientLight (already takes it over) | PROVEN takeover; off | 0.5 |
| Screen SkinScattering | DeferredSkin's own blur | gated for deferred skin (`SkinScattering.cpp:285`); still needed for forward skin and mode 0 | keep until mode 0 is retired |

### What I would start first
1. **A rain and an interior F10 profile** (owner or room 06, 1 run). Every rain and indoor saving above is NOT MEASURED; this decides B4.
2. **Quick wins Q1, Q3-Q7 and Q9-Q12 in one build:** 1-1.5 sessions, no look change except Q1's streaks coming back and Q12's two
   options showing. Q2 and Q8 go to the `retire-wetworld` session instead.
3. **B2, depth and normals once:** 2-3 sessions. The only item that saves time on every frame for every player with no look change.
4. **B1, keep going** (it is in flight), and fold M5 into its step 4.
5. Then, with the profile in hand: **B3's cascade skip** if the cascades cost what I expect, **B4** if the G-buffer passes add up to
   over 1 ms.

### Already merged or fused (don't re-propose)
- FusedPassPlan: Coloring + LUT, Cinema + SkyDither (`EffectChain.cpp:1287`).
- The fog composite applies sun shadow, AO and interior darkening; plus pairApply and shadowIntoAmbientLight (CompositePlan).
- AmbientLight takes over AO and BounceLight (`EffectChain.cpp:599-608`).
- Bounce combine folded into Specular's (SpecularBouncePlan).
- SMAA yields to TAA. ImageAdjust, LUT and Exposure skip themselves at identity.
- The distant DoF technique skips the source copy.
- Idle target parking (`TargetReleaseSeconds`).
- FrameChain removes the per-effect RenderedBuffer copies; the game's HDR target is used as the chain texture.

These were checked and found not to overlap:
- one lamp table and froxel list a frame, shared by forward+, glass and DeferredLamps;
- one reflection cube writer;
- one shading-normals copy shared by WetnessReflections and WetWorld;
- the sky tables are gated;
- AvgLuma vs bloom mips, and DoF focus (shares AvgLuma);
- Flashlight vs the lamp table (exclusive);
- deferred pixels sample the sun shadow once (DeferredSun only; the screen buffer holds contact and lamp terms);
- RT AO replaces the SSAO estimate (no double);
- RT exterior lamp shadows replace the cubes outdoors.

### Dead settings, shaders and targets (mechanical sweep)
**Method.**
- All 3019 shipped keys were parsed from `resource/VegasShaders.defaults.toml`. Each one was matched as a quoted string in the C++ (menu
  files and the migration block excluded), together with its section name.
- Anything that failed was read by hand.
- `Shaders.*.Status Enabled` keys are read generically (`GetMenuShaderEnabled`, `SettingManager.cpp:1915`). GunFX keys are read
  through the `GunFXKeys.h` table.
- Raw lists are in this session's scratchpad (`audit\dead3.txt`, `hidden_internal.txt`, `hidden_devonly.txt`, `lists.txt`,
  `inittex.txt`), not kept.

**Read by nothing (PROVEN; 14 keys). Drop them from the defaults:**
- `Shaders.Blood.Status Enabled`: no Blood effect exists; the toml says "Not currently used".
- `Shaders.Tonemapping.Main/.Interiors/.Night PointLightMultiplier`: "disabled for now", `Tonemapping.cpp:33`.
- `Shaders.Tonemapping.Interiors SkyMultiplier`: `Sky.cpp:603` reads only Main and Night.
- `Shaders.ShadowsExteriors.Main OrthoMapResolution, OrthoRadius`: already in the menu's dead list.
- `Shaders.ShadowsExteriors.ShadowMaps Mipmaps, Anisotropy`: the field they would set is never assigned.
- `Shaders.Sky.Clouds SphericalNormals`: the read is commented out (`Sky.cpp:613`); `SKYTEX.pso.hlsl:34` hard-codes 1.
- `Shaders.Snow.Main Flakes`.
- `Shaders.WaterLens.Main Time`: `WaterLens.cpp:23-27` reads the other four keys only.
- `Shaders.MotionBlur.FirstPersonView/ThirdPersonView Enabled`: `ReadBlurView` (`MotionBlur.cpp:52-58`, checked) reads four keys and
  not Enabled. So the shipped `ThirdPersonView Enabled = false` does nothing; motion blur still runs in third person.
  - That last one is a small behaviour bug, not just a dead key. Wiring it up changes what players see: owner's word.

**Read, but the shader never uses the value (PROVEN):**
- AO `Samples` (Main, Interiors);
- GodRays `LightShaftPasses`;
- Sharpening `Clamp` (Main, Interiors);
- WetWorld `PuddleCoeff_R/G/B`, `PuddleSpecularMultiplier` (Q8);
- ShadowsInteriors `UseCastShadowFlag`.

Unsure: ShadowsInteriors `Quality` (sent as a constant, toml says unused; shader not checked).

**Stale menu "dead" list:** `PlayerShadowFirstPerson` / `PlayerShadowThirdPerson` are marked dead (`ImGuiManager.cpp:803-804`) but
ARE read (`ShadowManager.cpp:1684`). That is a menu bug: the keys are hidden while they still work.

**Hidden, not dead:**
- 433 keys on the Internal list and 189 on the DevOnly list;
- the Rain wetness hides (12 WetWorld + 5 Wetness Effects keys);
- the Unified reflections hides (3);
- the Reflections HiZ* and DampTrace keys.

No key on the `Retired()` list is still in the defaults.

**Shader files:**
- `Shaders/ObjectRefractionTemplate.hlsl` is an orphan: no C++, shader, tool or doc names it or its shader names (PROVEN, checked).
- Includes no shader includes:
  - `Compute/ObjectMotion.glsl`;
  - `FSR31/.../ffx_fsr3upscaler_debug_view.h`;
  - `Rt/ffx/ffx_denoiser_shadows_prepare.h`;
  - the AMD original `*_callbacks_glsl.h` files kept beside our `_nvr.h` replacements (BELIEVED kept on purpose).
- `Compute/Sm6/` holds two hash generations of the VolumetricFog variants (`b85a1398`, `b95a152b`); one set is BELIEVED stale.
- `ReflectionsTraceHiZCount.spv` is a debug build still loaded by a counter path (`Reflections.cpp:1420`).
- 139 .spv and 42 game-named .pso/.vso files have no literal name in C++. That is expected: they are variants, helper-loaded or
  picked by the game.

**Effects:**
- Every effect folder is enrolled and stepped.
- Flashlight's .fx is loaded and never drawn (the flashlight is deferred now; its comment says so). Kept for the menu switch; it
  could become a settings-only record.
- Off effects cost one early return (`EffectRecord.cpp:471`).
- BELIEVED: every effect's .fx is compiled at start whether on or off (no switch gate in LoadEffect). That costs load time.
- Debug and self-test steps do nothing when off. The exception is one `GetSettingI` a frame in the G-buffer view
  (`CapturedNormals.h:1880`), and only while it is live.
- Stale comment: `EffectChain.cpp:107` says TAA is off by default; it is on.

**Render targets allocated while their effect is off (not parkable; BELIEVED, sizes not logged):**
- **AmbientOcclusion's buffers.** AO is forced off whenever AmbientLight is on, which is the default, so these are held for nothing
  by default.
- **VolumetricClouds** (off by default).
- **LocalExposure, Dream, Bodycam, SunGlare, VolumetricSmoke, HudRaindrops.**

Making them `TargetParkable` is the cheap half of B5: 0.5-1 session, memory only, no look change. No truly dead target was found (the
two 1x1 query targets and the cloud probe are read on purpose).

**Duplicates by purpose:**
- Bloom + BloomLegacy, and TAA + FXAA, can all run together with no warning. The menu warns only about SMAA + FXAA.
- Snow and Precipitations are exclusive by weather.
- Specular and PBR are different layers.
- PointShadows2 is PointShadows' continuation pass (never drawn with the shipped fx, Q4).
- ShadowsExteriors and SunShadows are complementary.

### Not checked / open
- GPU cost of any pass in rain or indoors; the cascade shadow-map draw cost; the LinearScene conversion count.
- Native D3D9 (non-DXVK) behaviour of each item; DeferredRenderer 0 behaviour item by item.
- The RT helper's internals beyond the reflection exchange (`RideMainExchange`, off, ~0.7-1.1 ms per its toml text from a cloud bench).
- Whether the froxel fog path also re-samples the cascades (M13).

## SMP compatibility

*Original title: Vegas Shaders with NV-SMP-Physics (2026-11-18)*

NV-SMP-Physics (github.com/Balebandro/NV-SMP-Physics, commit fd9ff44, read as a reference only at `F:\Claude-refs\refs\NV-SMP-Physics`)
simulates SMP cloth, hair and physics bones in New Vegas. A tester crashed with it (CrashLogger 2026-11-18-09-20-54). This is what was
found and changed on branch `pipboyfit-crash`.

### The crash
- `PipboyFit::ForGeometry` (`src/core/PipboyFit.h:122` before the fix) ran `_stricmp` on the name of every bone a skinned mesh pointed at.
  On a mod's first-person shape "VirtualLegs" (`BSDismemberSkinInstance`) one bone slot pointed at float data: the name pointer was
  0xC1880000 (-17.0f) and the read faulted (main thread; `RenderPassSkinnedHook`, `Render.cpp:4408`).
- Why a bone slot can point at garbage (believed for this crash, documented by NV-SMP-Physics `src/nv/ActorManager.cpp:33-48`): when the
  game attaches an item it drops the item's own bone nodes from the scene, but the skin keeps pointing at them until the game binds the skin
  to the skeleton, seconds later, after the Pip-Boy closes. Those nodes may be freed or their memory reused. The crash stack held the Pip-Boy
  manager and its screen, which fits. This is the game's own behaviour; NV-SMP-Physics works around it for its items (reads slots under SEH,
  puts its own never-freed nodes in stale slots: `ActorManager.cpp:49-135`), and shapes it does not handle keep the stale slots.

### What NV-SMP-Physics does (proven from its source)
- Hooks: `TESMain::DrawWorld_WaitForParallelUpdate` 0x00872AD0 (it runs the physics after it, on the main thread, before the draw) and
  `Animation::FindSkinnedNode` at the call 0x004AF0FC (SMP items are attached as rigid add-ons, keeping their own bone nodes)
  (`src/nv/main.cpp:309-335,554-555`, `src/nv/Game.h:18-28`). No render hooks: nothing overlaps `RenderPassSkinnedHook`.
- Writes into skins: stale slots of SMP items swapped for its own nodes (`ActorManager.cpp:133`); hair and race-menu heads re-bound with
  bone arrays it allocates with the game's allocator (`ActorManager.cpp:1000-1030,1110-1130`, freed with the skin by the game); the hair's
  head slot moved to the skeleton's head (`ActorManager.cpp:820-831`). The bones it simulates get their world transforms written before
  the draw, so everything that reads bones at draw time sees the simulated pose.

### What Vegas Shaders does now
- `src/core/SkinBones.h`: `Live(bone)` (the slot's object readable under SEH and its vtable in the game's .rdata, where every game vtable
  is) and `Name(bone)` (the name copied under SEH). Every reader of `NiSkinInstance::BoneObjects` goes through them.
- PipboyFit finds the forearm bones by name in the player's own live skeletons once a frame and compares slots by pointer only.
- ClothWind `SkeletonOf` and CharacterObjectReach `Classify` (bone names, any actor): names through `Name`; a non-live slot makes the
  result "unsure", not cached (ClothWind retries at most once a frame; its cloth auto-detect stores no verdict then).
- Transform readers (ClothWind `BoneXf`, PipboyFit `ForPartition`, GearWeather, DeferredFirstPerson, ShadowManager's skinned bounds and
  caster boxes, RtCore's RT skinning palette) skip a non-live slot.

### Still wrong with SMP (not crashes)
- (Fixed 2026-11-18) ClothWind's procedural wind moved garments SMP already simulates a second time. Now a skinned shape carrying SMP's
  "SMP Bones" string extra data (the plugin's own marker, one per skinned shape: its README, ActorManager.cpp:110) gets no cloth wind:
  `NiObjectNET::ExtraDataNamed` (GameNi.h; the extra-data list at +0x10, its count at +0x14, each entry's name at +0x08, as JIP LN's
  headers have it; LayoutCheck.h asserts the offsets). SMP blows them in the weather's wind itself.
- `Live` shows a slot looks like a node, not that it is still in the scene: a dropped node not yet freed passes, and for one frame its
  transform can be garbage in RT skinning, shadow bounds or cloth points during the bind wait.

### Not run
Nothing here was run in game. The VirtualLegs shape's own layout was not seen.

## Community Shaders ports

*Original title: Community Shaders ports: what came from where, and under which licence*

Written 2026-10-19 with the five features below; LOD Blending (the sixth) added the same day. Kept so that anyone can check, file by file, that nothing was taken from a
closed or unlicensed source.

### The short version

- Every piece of outside code used here is **GPL-3.0** (Community Shaders) or **GPL-3.0-or-later** (Conor's NVR fork).
  This project is GPL-3.0-or-later with the same Modding and Linking exceptions, so the combination is allowed. The
  combined work is distributed as GPL-3.0 with its source public (this repository).
- Each ported file and function says at its top where it came from (repository, path, commit) and keeps the original
  credit. Nothing was relicensed or stripped of a notice.
- Parts written for this build (the land-height reader and its inflate, the DirectX 9 cloud shadow pass, the grass
  push texture and its CPU side, the sky-visibility channel for reflections, the composite plumbing) are this project's
  own code, under its licence.
- Not used, on purpose: the Community Shaders logo and icons (no licence for redistribution), their glints shader (origin
  unclear), any vendor SDK (DLSS, FSR, Streamline), and their shipped heightmap textures (we read the player's own game
  files at run time instead, so no game-derived data is distributed).

### Sources checked (2026-10-19)

| Source | Commit read | Licence, as checked |
|---|---|---|
| github.com/community-shaders/skyrim-community-shaders (branch `dev`) | 3f9f72d | `COPYING.txt` GPL-3.0; README "License: GPL-3.0 WITH Modding Exception AND GPL-3.0 Linking Exception"; `EXCEPTIONS.md`. Per their README, files without their own header are GPL-3.0 with the exceptions; `package/SKSE/Plugins/CommunityShaders/Licenses/THIRD_PARTY.md` lists the exceptions to that (none of the files below are on it, except SebH's spherical harmonics, MIT, which is not used here). |
| github.com/northaxosky/fallout4-community-shaders | e499056 | GPL-3.0-or-later + the same exceptions (read for the heightmap workflow only; no code taken). |
| github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, branches `testbed` and `claude/terrain-variation` | 9000473d, 6043062a, bae95aef; 66653eea, d07ff4b9 | A fork of macrimmon12-tech/TESReloaded10. Its `COPYING.txt` is the GPL-3.0 text and its `License.md` is byte-for-byte this project's (GPL-3.0-or-later with the same section 7 terms). GitHub shows "Other" only because of those extra terms. |

Files of the Community Shaders features that were read (none carries its own licence header, none is in THIRD_PARTY.md,
so all are GPL-3.0 with the exceptions):
`features/Terrain Shadows/Shaders/TerrainShadows/{TerrainShadows.hlsli,ShadowUpdate.cs.hlsl}`, `features/Terrain Shadows/textures/heightmaps/readme.txt`,
`features/Cloud Shadows/Shaders/CloudShadows/CloudShadows.hlsli`, `src/Features/CloudShadows.cpp`,
`features/Grass Collision/Shaders/GrassCollision/{GrassCollision.hlsli,CollisionUpdateCS.hlsl}`,
`features/Terrain Variation/Shaders/TerrainVariation/TerrainVariation.hlsli`,
`features/Skylighting/Shaders/Skylighting/Skylighting.hlsli`, `src/Features/Skylighting.{h,cpp}` (the MinSpecularVisibility default),
`features/LOD Blending/Shaders/Features/LODBlending.ini`, `src/Features/LODBlending.{h,cpp}`, and the `LOD_BLENDING` lines of
`package/Shaders/Lighting.hlsl` (the brightness and gamma on the LOD land, DisableTerrainVertexColors).

### Feature by feature

#### Mountain shadows ([Shaders.ShadowsExteriors.TerrainShadows])
- `src/effects/TerrainShadows.{h,cpp}`: **Conor's code** (his `TerrainShadows.{h,cpp}` at 9000473d), itself after Community
  Shaders' Terrain Shadows. Kept: the xLODGen height-map reader, the worker-thread sweep toward the sun, the strip upload.
  Changed: a fallback to the land read from the plugins; the load request built on the main thread.
- `src/effects/LandHeights.{h,cpp}`: **this build's own code.** Reads the LAND records of a worldspace from the player's own
  plugins in load order (format from UESP / xEdit documentation) and a small zlib inflate written from RFC 1950/1951.
- `Effects/SunFar.fx.hlsl` `TerrainShadow()`, `TerrainShadowCoverage()`, `SkyIrradiance()` and the share formula: **Conor's**
  (his `ShadowsExteriors.fx.hlsl` at 6043062a). The separate pass, the composite hooks and the single-lookup cascade test are
  this build's.
- `ShadowsExteriorEffect::UpdateLightColors`: **Conor's** (6043062a).

#### Cloud shadows ([Shaders.CloudShadows.Main])
- **This build's own code**, after the idea of Community Shaders' Cloud Shadows (the cover of the game's cloud layers where
  the light from a point meets the cloud layer, dimming only the sun). CS captures the layers into a cube map during the
  sky's reflection pass (D3D11); here the sky's four cloud textures are read directly on a flat layer above the camera.
  The 2 km height default is CS's `CloudHeight`.

#### Grass collision ([Shaders.Grass.Collision])
- **This build's own code**, after the idea of Community Shaders' Grass Collision (actors flatten the grass round them, the
  grass vertex shader bends away, a spring brings it back). CS uses a compute shader and a constant buffer; here the CPU
  keeps a world-locked push grid and the grass vertex shaders read it as a vertex texture.

#### Less repeating ground ([Shaders.Terrain.Variation])
- `Shaders/Includes/TerrainVariation.hlsl`: **Conor's port** (66653eea, d07ff4b9) of Community Shaders' Terrain Variation:
  the skewed lattice, the hash, the corner ranking and fades, the height-weighted blend (constants identical to CS's
  `TerrainVariation.hlsli`). After Deliot and Heitz, "Procedural Stochastic Textures by Tiling and Blending" (2019).
  Changed: the taps use this build's explicit gradients and empty-layer skip; StartDistance; register c95.

#### Sky reflections under cover ([Shaders.AmbientOcclusion.Skylighting] Reflections)
- **This build's own code**: Skylighting v2's visibility kept in a spare channel and applied to the Specular effect's sky
  reflection and to the sky fallback of the wet-ground reflections. The blend `lerp(MinReflection, 1, visibility)` and its
  0.1 default follow Community Shaders' Skylighting `MixSpecular` / `MinSpecularVisibility`.

#### Far land matching ([Shaders.Terrain.LODBlending])
- **This build's own code**, after Community Shaders' LOD Blending: the far land's colour taken to a gamma and times a
  brightness (`pow(abs(color), gamma) * brightness`, as their LOD land does), here in `TerrainLODTemplate.hlsl` and the
  near/far fade (`TerrainFadeTemplate.hlsl`), and the near land without its vertex colours (their
  `DisableTerrainVertexColors`). Their LOD-object and snow knobs are not ported (New Vegas draws far objects with the
  ordinary object shaders, so there is no LOD-object shader to adjust).

### Optimisation pass (2026-10-19, same picture)
Done after the ports, none of it changing what is drawn (checked in game against the first build: the same pixels change by
the same amount): Skylighting's target keeps two channels unless the sky reflections are on (3.5 MB of video memory at
1280x720); the mountain shadows keep the ground height in a texture of its own, written once a map, so each sweep packs and
uploads half the bytes (about 11 MB less memory in the game's process at the default size); the SunFar pass reads the normals
only past its early-out; grass beyond the push texture or where no one pushes skips the read and the second transform; the
push texture is only sampled near the pushed cells. None of these moves a frame time measurably; they save memory, uploads
and a little CPU. Tried and dropped: skipping the composites' new texture reads with a dynamic branch while the features are
off. It measured slower (the fog composite 0.47 -> 0.52-0.53 ms, two launches each), and the plain read costs nothing
measurable with them off (0.47, the same as P81's shader without it).

### Credit lines for release notes

- "Community Shaders team and contributors (github.com/community-shaders/skyrim-community-shaders, GPL-3.0): Terrain Shadows,
  Cloud Shadows, Grass Collision, Terrain Variation, LOD Blending and Skylighting, adapted for DirectX 9."
- "Conor (mrfury97, github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, GPL-3.0-or-later): the terrain shadows' height-map
  sweep and share composite, and the terrain variation port."
- "Terrain variation after Deliot and Heitz 2019."
- "Bottled Shaders (github.com/InTheBottle/Bottled-Shaders, GPL-3.0): Screen-Space Shadows, ported one to one; the raymarch is
  Bend Studio's screen-space shadows, Copyright 2023 Sony Interactive Entertainment (Apache-2.0)."

## Upstream 1.1.1-1.1.3 merge

*Merged 2026-11-22 to 2026-11-26 on branch `upstream-rt-merge` (merge commit 68cbb9c; upstream 5559923, base d3477de = 1.1.0).*

Owner's word, relayed by Main brain: "yea tell it to start that" (2026-11-22: take upstream's RT as is, keep our deferred wetness,
unified reflections and speckle fixes, hand-merge where they meet), then "do it all jfc" (2026-11-26: take upstream's defaults for the
four open choices, bench, merge into main, push). Upstream 1.1.4 (5538f4b) came out during the merge and is NOT in it.

**From upstream, unchanged:** ray tracing (path tracer 3-pass split, ReSTIR GI, pure direct-lit GI, RT AO / Sky / GI as separate switches,
RT fog lamps, RT shadow denoise, RT reflections at RenderScale with their own denoiser, `ViewModelMotion.h`), RT helper protocol 9 -> 10
(`tools/nvr-rt-helper` builds `NvrRtHelper.exe`, which must replace the installed one), the 1.1.1-test2 menu overhaul, Vulkan
`DeferredLamps` compute (off here, below), RenderRect post-FX, matte materials, first-person / ADS / AA fixes, 3D menu PBR isolation,
Sandstorm and shadow ghosting fixes, Hi-Z removed, BloodFlow and BloodLens removed (owner's word 2026-11-26; their saved settings are
retired quietly). Fork patches 0036 (security) and 0037 (HDR blitter) touch no RT: no fork rebuild (`docs/TOOLS.md` "nvr-dxvk").

**Ours, kept:** deferred wetness (G-buffer film, `core/RainState.h`, WetWorld removed: its files, settings and the menu's "Legacy WetWorld"
choice stay out), unified reflections (`GBufferFrame`, `WetReach`, DebugView 12), the speckle fixes (`DampTrace`, `DampBumpLean`, the
`DampMaxRoughness` Fresnel cap), our own `WetnessReflections`, the retired old text-menu settings.

**Hand-merged:** Reflections (Vulkan and D3D9: one wetness source per frame; `docs/EFFECTS.md` "Reflections", "Upstream 1.1.1-1.1.3
merge"); settings migrations (upstream's 90-94, ours renumbered 95-97, step 98 for the RT reflection defaults; a file from our own builds
at 91 / 92 is recognised by its missing `[Shaders.WetWorld]`); files that only held our hand-port of 1.1.2 are upstream's now.

**Defaults:**

| Key | Shipped | Upstream | Why |
|---|---|---|---|
| `[Shaders.Reflections.Main] ReplaceForward` | true | true | owner: upstream's (step 98) |
| `[Shaders.Reflections.RT] FollowRenderScale` | true | true | owner: upstream's (step 98) |
| `[Shaders.Reflections.RT] RideMainExchange` | false | true | RayGen would run before the deferred wetness film: RT rays and the composite would disagree on wet ground |
| `[Shaders.Reflections.Main] ForwardWetFilm` (ours) | false | (always on) | with MaxRainWetness 0.5 wet open ground loses its mirror (review) |
| `[Main.Main.RayTracing] HelperWait` | 0 | 1 | owner's question |
| `[Shaders.PBR.Rmaos] MetalFromMap` | false | true | owner's question |
| `[Shaders.PBR.Main] VanillaBridgeDielectricMax` | 0.08 | 0.04 | owner's question |
| `[Main.Main.Quality] DeferredBumpedNormals` | false | true | owner's question |
| `[Main.Main.Performance] NvrDxvkDeferred` | true (since 2026-11-27, step 111) | true | was parked (frame-cost change); owner 2026-11-27: the deferred sun runs in Vulkan (EFFECTS.md "ShadowsExteriors" 14) |
| `[Shaders.AmbientOcclusion.RT] Sky` | false | false | independent of RT AO now; true would start sky rays nobody asked for |

**Mistakes caught:** the merged defaults had `DeferredStandalone` twice (both sides added it) and the game refused the whole file (slot
log "toml::insert_value ... already exists"): every defaults file is now parsed with a TOML parser before an install.

**Results (2026-11-26):**
- Proven: the DLL (release and test), `NvrRtHelper.exe` (x64, protocol 10) and the 12 rebuilt Vulkan shaders build (the committed .spv
  match their sources); `Reflections.fx` compiles in its default, MaterialAware and UnifiedReflections variants; every defaults file
  parses (TOML parser) and matches its fragments; lead-architect review: approve with changes, all applied. In a bench slot the merged
  DLL loaded and migrated the owner's settings file (v91) exactly as designed (only Gamma22 moved). Installed as install-test28, then
  install-test29 (`F:\VS-scratch\install-test29-2026-11-26estore.ps1` -> test28 -> test27).
- Not run: the in-game pictures (owner: "dont worry about bench pics, ill do it myself"), ray tracing with the new helper, frame time.
- Not in it: upstream 1.1.4 (merged next, below).

**Upstream 1.1.4 (2026-11-26, branch `upstream-114-merge`, upstream 2bf5110 = 1.1.4 5538f4b + WORKS-CITED):** terrain bump and sunrise
continuity, shadow resize hardening, PT / RT and denoiser fixes, a `DeferredSun` compute pass, DoF at RenderScale, DXVK precedence. The owner
said to take upstream's choices, so its five default changes come in: `[Main.Main.RayTracing] ActorFlushPerSpace` true and
`[Shaders.ShadowsExteriors.Deferred] MatchSunDisc` false (upstream's steps 95 / 96, here 99 / 100, version 100), and, without a step as
upstream ships them, `PTGlassSurface` 1, `[Shaders.AmbientOcclusion.RT] GIStrengthInterior` 2 and `GlowGIInterior` 1.5. Settings history:
a file at 95 / 96 WITH `[Shaders.WetWorld]` was saved by upstream 1.1.4 (it ran their 95 / 96, none of ours); without it, by ours.
Hand-merged: `ShaderRecord.cpp` (our failed-compile dump + upstream's null-safe constant table), `ShadowsExterior.cpp` (upstream's null
checks, no WetWorld), `EnvTemplate.hlsl` (upstream's two-register `EnvLightFadeExtra`), `ReflectionsComposite.frag` (the metal F0 branch also
for the G-buffer material). RT helper protocol stays 10 (rebuilt: `NvrRtTextures.inl` changed). Upstream's committed `Compute/Rt/RtPathTrace*.spv`
do not rebuild byte for byte with our Vulkan SDK 1.3.296 (believed: another glslang); they are kept as upstream built them; every other
.spv checked reproduces. `RtReflDenoisePrefilter.frag` (1.0.6, its FFX include removed upstream) no longer compiles and is not loaded.
`docs/WORKS-CITED.md` is upstream's credits file, kept as theirs (not one of our four docs).

**Bench, slot A, 2026-11-26 (the owner's settings of 11-26; sheets in `F:\VS-scratchtm-sheets`):**
- Proven, test29 (1.1.1-1.1.3 merge): loads, migrates (step 98 moved ReplaceForward and FollowRenderScale); RT reflections and RT GI run
  with the new helper (ready in 0.5 s once its shaders are cached); rain, RT, FSR 0.67 and night pictures show no specks or smears. Found:
  REGISTER CLASH X4500 in `DeferredSun.pso`'s SunFade record (upstream's `TESR_ShadowDynamic` on s12 = our `DS_WetFilmMap`): that record
  lost the film's sun highlight. Fixed in 4f242a4 (the atlas to s14); `template_compile` with the game's defines: main FAILED, fix OK.
- Proven, test30 (this branch): no errors in the log; steps 98-101 moved ReplaceForward, FollowRenderScale, ActorFlushPerSpace,
  MatchSunDisc (true -> false), PTGlassSurface, GIStrengthInterior, GlowGIInterior; the RT helper (rebuilt) ready in 0.6 s.
- Control (test27 with the same settings): with MatchSunDisc on (the owner's saved 1.1.3 default) test29 lit the ground from the true sun
  disc (another shadow direction, +14 luma); with 1.1.4's step 100 the direction matches test27 again. Rain darkens the ground by the same
  amount in both builds (-7 / -8 luma). Believed: the remaining softer, lighter daylight (+12 luma) is upstream's lighting changes.
- Not run: HDR (no HDR display on the bench), hair (the Character Kit Remake hair the fix targets is not in the slot), grass under lamps
  (no lamp at these spots at night), frame time.

## Upstream 1.1.5 merge

*Merged 2026-11-27 on branch `upstream-115-port` (merge commit 8179e3f; upstream ae74446 = 1.1.5, base 2bf5110 = 1.1.4; fixes e429835 and e9780e5).*

Mode: full merge (`git merge upstream/main`, conflicts resolved by hand in three groups: shadows and deferred core, reflections and
shaders, settings and compile). Upstream's code and defaults are taken wherever they do not fight ours.

**From upstream, unchanged (by area):**
- Deferred: the pipeline completion (DeferredSun compute reads the screen-space shadow mask, DeferredLamps compute refactor), night lamp
  shadows, first-person deferred (`DeferredFirstPerson`), Material SSS (off), RealSunPath (off), e401def (specular foliage halos, Sun Shadows
  off with PBR, grass and tree shadow popping), the `c211.w` class stride 8 -> 16.
- Surfaces: WorldSurface (new effect, `docs/EFFECTS.md` "WorldSurface"), clearcoat and pearlescence (0), the dynamic world cubemap,
  weapon reflections without RT, metal fixes, the ReplaceForward reprojection fix, EnvLightFade, GearWeather's dry layers.
- Performance and robustness: BoneMatrixBatch, ConstantMapSort, VA map, lazy effect load, the generic compile-failure memo, DxvkLoader,
  InstallCheck, Diagnostics, unsaved-setting marks, the RenderScale guard, BlendedLayerFix, Distant Land (off; shelved upstream too).
- Engine layout fixes: fog, weather colours, land texture count, drawn camera, DoF depth sites.

**Ours, kept:** SunSheen (wet gates), Bottled SSS (upstream took our port: one copy) and lamp SSS, unified reflections, puddle mirror,
mirror puddles, the tilted-mirror floor fix, `DeferredLandRoughF90`, GearWeather's wet block, the AmbientLight menu fix, our CloudOff and
LightFreeWet compile jobs, the compute DeferredSun standing aside while the wet film is live, the SunFade s12 fix, `Lamps::MadeThisSpan`,
README. WetWorld stays removed.

**Seams (where both sides wanted the same slot):**

| Seam | Ours / upstream | Resolution |
|---|---|---|
| `DeferredSun.hlsl` c17 | our wet film `DS_WetFilm` / upstream's Material SSS | film stays c17; Material SSS is `DS_MatSSS` c20 |
| c18 `DS_Sheen` | our puddle gate in .z / upstream's clearcoat in .z | c18 = x strength, y rough, z clearcoat, w pearl; the puddle gate is `DS_SheenGate` c21.x |
| c19 | our `DS_SSS` (Bottled SSS) / upstream's `DS_BendSSS` | one, ours, c19 |
| compute flags | upstream bit 22 = SSS mask | bit 22 SSS mask, bit 23 puddle gate (`kFlagSheenPuddleGate`) |
| world cube binding | upstream set 1 binding 30 / slot 29 | binding 32 / slot 31 (30 and 31 carry our G-buffer COLOR1 / COLOR3 and film); slot 31 is below `NVR_EXT_MAX_SLOTS` |
| `c168.w` (`TESR_LinearScales`) | our `DeferredLandRoughF90` / upstream's `GlowScale` | ours stays; `GlowScale` is on `TESR_SharpSky.w` (c136), declared in every forward object shader via `SkyAmbient.hlsl` |
| `c211.w` | upstream stride 16 | encoder (`Render.cpp MaterialWFor`) and decoder (`CapturedNormals.hlsl`) both 16 |
| forward metal | upstream clearcoat / pearl always compiled | behind `METAL_CLEARCOAT_ON` / `METAL_PEARL_ON` (restart defines, below) |
| WorldSurface | reads the land's material fraction as metal | leaves the land out while `DEFERRED_WETNESS` is compiled in (`WS_Flags` c21.x = 1) |

c20 and c21 are free in every include (grep by group S) and inside the saved c0-c223 range.

**Restart defines (e429835).** Upstream's forward metal code (pearlescence in `getMetalAmbient`, the clearcoat's candy absorption and coat
lobe, the BaseMap look-through with its ddx / ddy) cost the 2-lamp object records their room for the water layer: SLS2023 went from wet to
X4505. `BuildDefines` now sets `METAL_PEARL_ON` / `METAL_CLEARCOAT_ON` only when `[Shaders.PBR.Metal] Pearlescence` / `Clearcoat` is above 0
at startup (menu: restart to turn on from 0, then live). The deferred sun and lamp passes read both live and are unchanged.
`template_defines.py` mirrors the two defines.

**WorldSurface and the wet land (e9780e5).** Under `DEFERRED_WETNESS` the land records keep their height for the water layer in the
material target's fraction. WorldSurface read it as metal and wrote it back capped before DeferredSun read it (lead-architect review). It
now leaves the land out in that case. It matters for every player: `core/SettingForce.h` forces GBufferV2 on (the owner's toml says false,
the log says layout v2), and Wetness Effects is on by default.

**Settings.** Upstream's steps 97-101 run here as 104-108 (all applied, none parked); step 109 is ours.

| Step | What | Runs for a saved file |
|---|---|---|
| 104-108 | upstream 97-101 (metal / env / cube / CloudSky / ExteriorBalance, weapon and DeferredFirstPerson defaults) | each skipped when an upstream-built file already ran its number (`kUpstreamNewest` 96 -> 101) |
| 109 | `CasterFadeMin` 0.75 -> 0.25, `[Main.Main.RayTracing] ArenaMB` 16 -> 32, `ActorRebuildInterval` 8 -> 16 (upstream changed these without a step; only a value still at the old default moves) | all |

Our 101-103 also run for upstream-built files. `UnofficialSettingsVersion` is 109. Walk-through (proven by reading the code, not run in a
game): ours at 103 runs 104-109; ours at 98 runs 99-103 then 104-109; upstream at 96 runs 101-103, 104-108, 109; upstream at 101 runs 102,
103, 109 (104-108 skipped); a fresh file runs nothing and is written at 109.

**Defaults that differ from upstream:**
- `[Shaders.ShadowsExteriors.Deferred] SunSheen` 0.9 (upstream 0.0).
- `[Main.Main.RayTracing] HelperWait` 0 (upstream 1).
- Parked as before: the kParkUpstream91 / 92 / 94 choices from the 1.1.1-1.1.3 merge.

**Upstream defaults that now show in game (taken, by the owner's standing rule to keep upstream's defaults):** WorldSurface (on; GBufferV2 is forced), `SunColorFromLight`, `DeferredDecals`, `DeferredEnvFade`, `PortalVisibilityCulling`, `DeferredFirstPerson`,
night lamp shadows with `NightMinDarkness` 0.65, `CubeMode` 2 (the dynamic world cubemap), weapon reflections, the metal defaults,
`ExteriorBalance` off, `CasterFadeMin` 0.25. Frame cost: see Bench below.

**Skipped:** upstream's version string (no release naming here); `ForwardLightKill` (test-only); WetWorld (stays removed).

**Forward record compile (`template_compile`, owner toml, DXVK, default record; ps_3_0 slots):**
- Main 9948b06: SLS2023 wet OK (3140); SLS2024 / 2026 / 2027 X4505, so they load dry (forward dry fallback, ebf1156), about 2400 slots.
- The merge before e429835 (proven): all four X4505 at full and lean (`Object.hlsl`, the pearl block in `getMetalAmbient`); all four OK dry
  (2417 / 2445 / 2425 / 2450), so SLS2023 had newly become dry. Forcing `NVR_PEARL_AMBIENT_LEAN` was still X4505 (`SkyAmbient.hlsl:170`).
- After e429835 (proven, `template_compile` with `--flow`, WETNESS_EFFECTS 1): at the default 0 all four compile wet (2996 / 3020 / 2997 / 3021)
  and dry (2260 / 2285 / 2268 / 2286), below main. At WETNESS_EFFECTS 3 (what the game builds with Puddles on) SLS2023 fails wet on main too, so
  in game it already loaded dry: the branch matches main. Both defines on: the four fail wet again (upstream's code). 211 Object / Parallax /
  Env records: no pass/fail regression against main; SLS2034 / 2036 now pass at 3 where main failed (upstream's restructure).
- The other 154 Object / Parallax / Env records: same OK / FAILED as main (proven before e429835).
- GearWeather.hlsl: 1918 slots (main 980, upstream 1737).
- Sun records (proven, group S): sun record 2246-3150, fade records 3217-3953, flow 0. `fork-v2-rt` / `fork-v2-rtfirst` fade with FD_SKIN is
  X4505 in main 9948b06 and in upstream ae74446 alike (test-only combos).

**Other checks (proven by the groups):** `compile-compute` for DeferredSun, DeferredLamps, ScreenSpaceShadows and 23 reflection shaders
(spirv-val OK, `refl-hashes --check` OK); `effect_compile Reflections.fx` in four define sets; the defaults file regenerated with
`defaults_tool.py merge`, `check` OK, no duplicate key. Not run: any game run; the DLL build at the final commit is room 02's.

**Known gaps:**
- A LateInteriors (ReleaseAfterCache) parent that hits a known failure releases its source, so its CloudOff job is never made and that
  record compiles on the main thread, as before the 11-26 compile speed-up.
- Commit 8179e3f alone does not compile (a brace); e429835 fixes it. Do not bisect through 8179e3f.
- `tests/stale_preset_values_check.py` still reports the 19 problems main has.
- `TextureManager.cpp` includes `ScreenSpaceShadows.h` twice (harmless).

Bench (slot A, test build of the branch vs main 9948b06, owner's toml of 2026-11-27, picture waits 10; sheets in
`F:\NVR-bench\shots\u115\`):
- SunGI2 / SunGI4 (runs 20261127-130129 / -130356 before, -132142 / -132848 after): same scene, mean luma +0.7 to +3.7; shadows a little less
  purple (upstream's SunColorFromLight / ExteriorBalance off), more grass and shrubs drawn at distance. Nothing broken.
- puddle1 held wet (`owner-b-wet-r01.toml`; -114900 before, -135909 after with the WorldSurface fix): mirrors cleaner (fewer white specks), sky
  reflection intact, luma within 1.
- v-tilted01 (-114243 before, -133751 after): the floor mirror stays level (our tilt fix kept); in the test-only sharp-mirror shots the
  reflected far wall is darker and grainier (missed rays now fall back to upstream's live world cubemap); the default view -1.5 luma.
- Saloon (-125637 before, -133029 after): lamp globes and the jukebox neon are clearly dimmer and the neon's green floor reflection is gone.
  Proven upstream's own look: upstream ae74446 built as is shows the same (-142843-slotE). Not caused by DecoupleGlowMaps (off in the toml from
  start: unchanged, -141928), GlowScale 2, PortalVisibilityCulling, DeferredEnvFade, DeferredDecals, BlendedLayerFix, CubeMode 1/0, Reflections
  off, FastConstantMapSort, BatchBoneUploads, RenderScaleGuard or the old EnvMaps values (live toggles, -141320 / -141522). Cause not found.
- Logs: no shader falls back to the game's own; fewer X4505 retries than main (572 vs 859 lines at puddle1); settings steps 102-109 ran on the
  owner's file (version 103).
- GPU frame time (F10 timer "Game frame total", SunGI2, 1080p, RTX 4070 Ti SUPER, uncapped while profiling; runs -141109 before, -140437
  after): before 10.1 ms; after 12.2 ms with CubeMode 2 (default), 11.2 ms with CubeMode 1, 10.8 ms with CubeMode 0. So the merge costs about
  +2.1 ms here, of which the live world cubemap is about 1.0 ms. Per pass in the after run: world cubemap 1.18 ms, WorldSurface 0.29 ms, a new
  "Light grid sun map" and game water reflections higher. The slot's frame rate itself is capped (~23 fps), so HEARTBEAT averages show no change.
- Not run: CubeMode 2 indoors cost, night lamp shadows outdoors, HDR, cold-start time.

## Deferred shadow quality

*Study 2026-11-26 (room 02, branch `shadow-quality-study`, main 04d9515 = test36). Owner's question: "what can we do to increase the
quality of deferred shadows". Trigger: a thin pipe's lamp shadow on a white interior wall showed as a hard staircase of big texels with
a pale fringe round a dark core.*

### In short
The blocky interior shadows are the **lamp** shadows, and two things make them look like that: the source is a **512-texel cube map**
per lamp (seen close, one texel covers about 20 screen pixels), and the deferred lookup filters it with **four taps** that MagnifiedSoftness
spreads two texels apart, so a shadow edge shows as up to four stacked, shifted copies of the texel staircase (the pale fringe). The sun is
not the problem: it already has four stabilised 1536-texel cascades (about 4 mm a texel near the camera) with a B-spline PCF and
contact hardening. Best look per ms, in order: (1) replace the four lamp taps with a noise-rotated disc of bilinear-PCF taps, (2) draw the
nearest 8 lamps' cube maps at 1024 (`NearCubeSize`, already built), (3) contact hardening on the lamps (already built) once (1) is in.

### Which shadow the screenshot shows (proven from code and the owner's toml unless marked)
- Interior, deferred renderer 2: the wall is lit by the deferred lamp pass. `DeferredLamps.hlsl` `DLShadow` (shadow passes, a pixel's
  first 16 lamps, full filter) calls `PointShadowCube` in `includes/PointShadowForward.hlsl`, reading the lamp's tile of the **shadow
  atlas** (`POINT_SHADOW_ATLAS`, `core/ShadowAtlas.h`). RT lamp shadows are off (`[_Shaders.ShadowsInteriors.RT] Enabled = false`,
  `RTInteriors = false`), capsules off, lamp contact shadows off (`[_Shaders.ShadowsInteriors.Main] ContactShadows = 0`). The old
  screen-space lamp mask is not laid over it (`[Deferred] ReplaceScreenShadows = true`), so the fringe is not a doubled shadow.
- **Resolution.** Each lamp's shadow is first drawn as a cube map, `ShadowCubeMapSize = 512` per face (`NearCubeSize = 0`), then copied
  with POINT sampling into an octahedral tile of the 8192 R16F atlas: the first 8 live lamps get 2048-texel tiles (`NearTiles = 8`),
  the other live lamps (up to `LightPoints = 24`) 1024, pool lamps 512 (`ShadowAtlas.h:80-120`). At the centre of a cube face a texel is
  2/512 rad, so its world size is about lamp distance / 256: a lamp 256 units (3.7 m) away gives 1 unit (1.4 cm) a texel. With the camera
  about 40 units from the wall that is roughly 20-25 screen pixels a texel, the step size in the screenshot (believed: camera and lamp
  distance estimated from the picture). An octahedral 2048 tile is about 4x finer than the cube near a face centre (2/2048 vs 2/512 rad;
  the octahedral stretch varies about 2x across the tile, believed), so for the near 8 lamps **the 512 cube is the limit, not the atlas**;
  1024 tiles are about as fine as the cube; pool lamps' 512 tiles are coarser than their cube.
- **Filter** (owner: `[Forward] Quality = 1`, `Softness = 0.4`, `MagnifiedSoftness = 2.0`, `SmoothEdges`, `EvenTaps`, `TruePCF` on,
  `ContactHardening` off, `LampSize = 0`, `TexelFilter` off): Medium = 4 taps on a rotated 2x2 pattern at +-spread on each face axis
  (`PointShadowCube`). Spread = Softness texels (0.4), raised by MagnifiedSoftness to at least 2 texels where one texel covers 2-8+ screen
  pixels (`POINT_SHADOW_MAG`, `PointShadowForward.h:480-491`). Each tap is a bilinear PCF of the 4 atlas texels round it (TruePCF,
  `PointShadowAtlasBlocked`): the soft edge it gives is one **atlas** texel, about a third to a quarter of a cube texel for the near tiles,
  so the cube's stair-steps stay hard. `TexelFilter` (the sun-style B-spline) has no atlas form: with the atlas on it falls back to the
  taps (comment at `PointShadowForward.hlsl:94-95`).
- **Why the pale fringe** (believed, from the filter's geometry; the A/B below checks it): with 4 taps 4 texels apart per axis, the result
  can only be 0, 25, 50, 75 or 100 % blocked. Inside a wide-enough caster all four taps are blocked (the black core); within 2 texels of the
  edge only one or two are (a 50-75 % lit band), and each band is a copy of the texel staircase shifted by the tap offset. A thin caster
  (2-4 texels wide) can never block all four. It is not a bias artefact.
- **Bias:** threshold = d - 0.018 d (`[Forward] Bias`, proportional to the lamp distance), plus a normal offset of max(NormalOffset,
  1.5 x Softness, 1.5 x MagnifiedSoftness) = 3 texels (`AutoNormalOffset`, `PointShadowForward.h:444-458`), plus the deferred
  `PrecisionBias = 4` (0.4 % of the camera distance, along the face normal, `DeferredLamps.hlsl:258-266`). The 3-texel push is tied to
  MagnifiedSoftness: a smaller filter footprint would allow a smaller push (less light leaking under contact).
- **Sun** (for comparison, helper read of `Shadow.hlsl`, `ShadowsExterior.cpp`): 4 cascades (Near, Middle, Far, Lod) of 1536 texels in
  one 3072 atlas (`CascadeResolution = 1`), lambda 0.9 over 6000 units, about 0.3 / 0.75 / 2.1 / 7.4 units a texel (believed: FOV
  assumed). Sphere-fitted and texel-snapped (no shimmer on turning), sun direction quantised and faded. 3x3 B-spline PCF of manual compares
  (no hardware compare), ContactHardening on (5-tap blocker search, then a 1+8 golden-angle disc of bilinear compares, hash-rotated with
  Smooth), normal offset 1.5 texels x (1 - N.L), receiver-plane slope slack. Cascade hand-over is a dithered pick over a 70-unit band.
  `SmoothFilter` (4x4 cubic) and `CascadeResolution` 2-6 exist and are off / at 1.

### What upstream and Community Shaders do (helper read, 2026-11-26)
- Upstream 1.1.5-test4 (`4b29278`, one squashed commit): the same lamp scheme (512 cubes, octahedral atlas with NearTiles, Quality 0-3 =
  1/4/10/20 taps, ContactHardening and LampSize off, Bias 0.018); its shadow changes are resource work (atlas released outdoors, targets
  skipped when unread) and night lamp shadows outdoors (`UsePointShadowsNight` true). Nothing new for filter quality.
- Community Shaders (Skyrim): no point-light filter of its own (it reuses the game's shadow-mask channels, a few lamps). Sun: a 2-moment
  variance map with an 11-tap Gaussian blur and light-bleed reduction, two cascades blended with a smoothstep, and Bend Studio's
  screen-space shadows. Ruled out for us: variance maps need 2-4 float channels (the 8192 atlas would grow 2-8x) and leak light; this fork
  already removed VSM/EVSM for the sun (2026-10-27).

### Options for the lamp shadows (deferred first: all of these change the deferred shadow passes; forward objects use the same include)

| Option | Gain | Frame cost | Effort | Risk |
|---|---|---|---|---|
| **A. Disc filter**: 8-12 bilinear-PCF taps on a golden-angle (Vogel) disc, rotated per pixel by interleaved-gradient noise that TAA averages, radius in cube texels (at least 1, MagnifiedSoftness where magnified); the sun's own disc (`Shadow.hlsl`, contact hardening) is the model. D3D9 shadow passes and `Compute/DeferredLamps.comp` | Removes the 4 stacked copies: a smooth penumbra with many levels, the staircase blurred into it. Biggest visible change per ms | +0.2-0.8 ms in busy rooms (believed: 9 bilinear taps = 36 reads vs 16 now per lamp, 16 lamps a pixel at most). Not measured | 1-2 sessions | ps_3_0 temp registers (X4505) in the D3D9 shadow pass; noise must be stable under TAA (no crawl) |
| B. `Quality = 3` (Ultra, EvenTaps 16 taps), built | 17 levels instead of 5: the fringe becomes finer steps, still a box filter | "Ultra with twenty cost 0.85 ms more than the grid" (EvenTaps comment); defaults text: soft edges 1-2 ms in busy rooms. Not measured here | 0 (a default) | Cost in lamp-heavy rooms |
| **C. `NearCubeSize = 1024`**, built (cube slots 0-7, the NearTiles lamps) | Texels half as wide for the 8 nearest lamps: steps 10-12 px instead of 20-25 at the screenshot's distance | Memory about 200 MB (400 with RedrawActorsOnly); 4x the fill when those cubes redraw (static lamps draw once). Not measured | 0.5 (default + migration step + bench) | VRAM on 4 GB cards; restart needed; untested in game before this study |
| C2. Screen-size allocation: give the big cube/tile to the lamps whose shadows cover the most screen (lamp distance to camera / lamp radius), not the first 8 slots | The resolution goes where the texels are magnified | Same memory as C | 2-3 | Lamps swapping sizes visibly (needs hysteresis, as `LampSwitchMargin`) |
| **D. Contact hardening** (`ContactHardening` on or `LampSize` 4-6 units), built | Sharp where a caster touches, softer further away: hides the big texels exactly where they are biggest (far from the caster). Looks like real lamps | 5 extra reads a lamp (blocker search) | 0.5 to tune (after A, so the wide penumbra is smooth) | With today's 4 taps a wide penumbra shows the stacked copies more (why it stays off); fine after A |
| E. Lamp screen-space contact shadows (`[Main] ContactShadows`, built, 0) | Pixel-exact shadows of thin detail touching surfaces (pipes on walls, cables, table legs) the map is too coarse for | A short depth march per lamp; not measured | 0.5 to test and tune | Halos on thin depth edges; only what is on screen |
| F. RT lamp shadows (built: `[ShadowsInteriors.RT]`, ShadowDenoiser, PhysicalLampSize; DXVK fork + RT GPU) | Exact silhouettes and physical penumbrae, no texels at all; the deferred shadow passes already read it for a pixel's first 16 lamps | Several ms (trace + denoiser); RT hardware only | 0 to switch on; tuning was done in Nov | Not for every player; the owner has it off |
| G. Normal offset tied to the filter (receiver-plane bias as the sun has, push = A's radius) | Less light leaking under contact, shadows meet casters | Nil | 0.5 (with A) | Acne if too small |
| H. Temporal accumulation of the shadow term per lamp | Smoother than A alone | Memory per lamp, history logic | 3+ | Ghosting on moving casters; TAA already does most of it for A |
| I. ESM/EVSM/MSM filterable maps | Prefiltered soft shadows | 2-8x atlas memory | 3+ | Light bleeding; removed before for the sun. Not recommended |

### Would "fully deferred" interior shadows help? (owner's question, 2026-11-26)
They already are deferred for nearly every pixel: the shadow passes (`DL_SHADOW_PASS`, `DeferredLamps.h:48-53`) compute the visibility of a
pixel's first 16 lamps in screen space, four lamps a pass into four A8R8G8B8 targets, and the lighting pass reads them. What stays
non-deferred: the shadow maps themselves (each lamp's cube map is drawn from the lamp; only RT avoids that), lamps past a pixel's 16th (a
cheaper lookup in the lighting pass, or lit), and forward-only surfaces (glass, blended hair, particles, water: the per-draw lookup, 6
lamps a draw). Moving the rest to deferred would not change the blocky look: that comes from the 512 cube and the 4-tap filter, not from
where the lookup runs. What the deferred split does buy:
- Room for a real filter: the shadow passes have the full ps_3_0 register budget, so option A goes there first.
- On the Vulkan lamp pass (`Compute/DeferredLamps.comp`) no sampler or register limit: it could read the lamps' cube maps directly
  (no lossy point-sampled copy into the octahedral atlas, which exists because a D3D9 pass has 16 samplers) with as many taps as A needs.
  Believed 2-3 sessions; needs the cube maps shared with the Vulkan side.
- Per-lamp screen-space work on the visibility targets: a contact-shadow march per lamp (option E) and a temporal pass on the 16 lamp
  channels (option H) both fit there. A screen-space blur of the targets alone is not a fix: it is what the old blurred room mask did, and
  a blurred staircase is still a wavy edge.

### Micro Shadow (2026-11-27)
Removed from the forward object shaders (owner: "Yes, forward only"); kept in the deferred sun (the lighting-free records record its share,
`FDSunVisMicro`). The deferred lamp passes never had it: to be redone in the deferred lamp path later (store the `_rmaos` occlusion in the
G-buffer, or read it there). `docs/EFFECTS.md` "PBR", "Micro Shadow".

### Sun options (lower priority: the sun is already good)
- `CascadeResolution = 2` (2048 per cascade): 1.8x the texels for about 1.8x the cascade memory and draw fill; `SmoothFilter` (4x4 cubic,
  16 compares instead of 9); `FitToDepth` (cascades fitted to what is on screen, about 0.05 ms). All built and off; each 0-0.5 session.
- Blend the cascade hand-over band (both lookups lerped, as the NVR_NO_SM3_LIMITS build does) instead of the dither, on the Vulkan path.
- RT sun shadows (`[ShadowsExteriors.RT]`, built, off): exact, for RT GPUs only.

### Recommendation (top 3, best look per ms)
1. **A, the disc filter** for lamp shadows, D3D9 shadow passes and the Vulkan DeferredLamps pass. Fixes the fringe and most of the
   staircase. 1-2 sessions, about +0.2-0.8 ms in busy rooms (believed). Interim with no code: `Quality = 3`.
2. **C, `NearCubeSize = 1024` by default** (needs a settings-version migration, as it changes a saved default). Halves the steps of the
   8 nearest lamps. 0.5 session, about 200-400 MB of video memory, little frame time for static lamps (believed).
3. **D, contact hardening on** once A is in: sharp at contact, soft away from it, which is where the texels are largest. 0.5 session.
Then E (lamp contact shadows) for thin detail touching surfaces. Sun: no change needed for this complaint.

### Bench A/B (queued 2026-11-26)
Job `F:\NVR-bench\work\bench\deep-roadmap\multirun\jobs\lampq.txt`, list `lists/lampq-1.txt`, tree VS-sheen (= test36), scene v-saloon
(Prospector Saloon, 17:13), four views x {Quality 1 (owner), Quality 3, MagnifiedSoftness 0}, at the owner's toml and with NearCubeSize
1024. Ran 2026-11-26 19:02-19:05 (slot A, runs 20261126-190327-slotA and -190438-slotA, VERDICT PASS both). Result: inconclusive for the blockiness question. In the Saloon the lamp shadows (table, stools) are soft and not magnified (a texel covers only a few pixels), so Quality 1 / 3, MagnifiedSoftness 0 and NearCubeSize 1024 look the same; the two runs' cameras also differ slightly (whole-frame |diff| about 9 from placement and TAA). Crops: `F:\VS-scratch\lampq-pics\lampq-floor.png` (not in the repo: game pictures). Settling it needs a close-up of a thin caster on a wall under a lamp (the owner's screenshot type): a scene not yet in the bench.

### Proven / believed / not run
- Proven (code + toml): which pass draws the shadow, cube 512 -> atlas copy, tile sizes, the 4-tap + MagnifiedSoftness filter, the
  bias terms, that TexelFilter has no atlas form, that the screen-space mask is not doubled over it; the sun's cascade setup.
- Believed: the texel-to-pixel ratio in the screenshot; the fringe = the 4-tap quantisation; the cost estimates for A-E.
- Not run: any frame-time measurement; any prototype of A; the A/B at a magnified thin-caster spot (the Saloon run did not show the problem).

## The Astral Shaders study

Written 2026-11-27 (room 02, read only). Source: github.com/Alaxouche/The-Astral-Shaders, cloned at `F:\Claude-refs\refs\The-Astral-Shaders`,
HEAD 00ae9471f (2026-11-22). Owner's ask: what in this Bottled Shaders fork is useful to us.

### What it is
- A **Skyrim SE/AE** plugin (SKSE, Direct3D 11, compute shaders), not FNV: Community Shaders -> Bottled Shaders (InTheBottle's
  `Bottle-Compendium` branch) -> Astral. Nothing in it runs in FNV; anything we take is a rewrite for D3D9 / ps_3_0 (or a Vulkan
  pass through our DXVK fork).
- Not a GitHub fork: its history contains Bottled's own commits and it merges Bottled every few days (last: Bottled 5c2f2d3d9,
  2026-11-21). Astral's own work is 146 commits by Alaxouche since 2026-09-26 ("minimal fork on Bottle f1ec4a225"), many
  co-authored with Claude; some are ports of Open Shaders (`OS #nnn`) and Community Shaders pull requests.
- Everything Astral adds over Bottled = `git diff 5c2f2d3d9 HEAD`: 253 files, +22.9k lines, most of it in Upscaling
  (Neural Rendering, DLSS-G), Living Windows and Caustic Light.
- Licence: GPL-3.0 (`COPYING.txt`) with the same Modding and Linking exceptions as Community Shaders (`EXCEPTIONS.md`), so
  compatible with ours as in [Community Shaders ports](#community-shaders-ports). Caustic Light's model and atlases are from
  qtuna's CausticLight, **GPL-3.0-only** (fine: the combined work is then GPL-3.0); its atlas generator came from ScottieFox's
  caustic-volume (MIT). Living Windows' atlases are PIXL Renderer's (GPL-3.0, image-model generated, medieval Nordic art).

### What Astral adds over Bottled, and whether Vegas Shaders has it

| Astral feature | What it does | In Vegas Shaders? |
| --- | --- | --- |
| Caustic Light | Light reflected off rippling water onto walls, ceilings, bridge undersides; refracted streaks on pool floors. Sun, moon and every clustered lamp, with shadows. Deferred compute pass, G-buffer albedo and normal. | **No.** Ours is underwater only (`[Shaders.Water]` causticsStrength: "Caustics seen from above water not currently supported"). |
| Living Windows | Parallax room interiors behind window panes (rooms, curtains, occupants on depth layers), lit on per-window night schedules, old-glass grime and reflections, rain beading, frost; sun shafts through windows into interiors. | **No** interior mapping. Lit windows exist only as glow maps (RT reads them, `GlowTextures`). |
| Volumetric Mists | Wind-driven mist pockets near the camera inside the froxel height fog (noise shape, warp, detail). | **Mostly yes**: `[Shaders.VolumetricFog]` has froxel fog patches (`FogVolume`), wind drift (`WindAngle`, `WindSpeed`), a height layer. Astral's pockets are a look refinement. |
| Interior eye adaptation | Own exposure values indoors (for ENB presets in Effects11). | **Yes**: `[Shaders.Exposure.Interiors]`, also tonemapping and fog interiors. |
| DLSS Frame Generation (DLSS-G) | Frame generation on RTX 40+ via a D3D11-to-D3D12 proxy swap chain (Bottled's code, Open Shaders design); FSR3 FG on the same chain. | **No.** We upscale only (DLSS helper, FSR 2.2/3.1 without FG); players use driver Smooth Motion or Lossless Scaling. |
| DLSS Neural Rendering | NVIDIA NGX feature 18 (`nvngx_dlssnr.dll` 310.8.x, not shipped) run before upscaling. | No. Vendor preview DLL, D3D12 interop, hardware-gated, untested even by its author. |
| Fur Shells additions | Fur on skin and khajiit bodies, touch interactors from body capsules, inertia spring. | No Fur Shells at all (that is a Bottled feature). |
| Grass Wet Darkening | Caps rain darkening of grass (default 0.3 porosity) so grass is not near black in a downpour. Plus a simple tree wetness permutation. | Not checked in game. Our deferred wetness has no grass-specific cap that I found. |
| Snow trails, snow print shading | Smoother snow deformation trails. | Not relevant (no snow in FNV). |
| Seam Probe | Debug tool for render-rectangle seams with upscalers. | We have the pixel inspector; not needed. |
| Skyrim-only pieces | Unified Water incremental caches, Tree LOD Wind (Windswept), ENB helper loading, Vel'dun theme and logo, Swedish strings, RaceMenu pass skip, light list search vectorised, Hitch Hunter sun poll. | Not applicable. |

### Ranked for us

1. **Caustic Light (worth a port).**
   - Player sees: moving light ripples on rock and concrete beside water in sun or lamplight (Hoover Dam, Lake Mead shore,
     Goodsprings pond, Vault pools, sewers, Lake Las Vegas), and bright streaks on shallow pool floors.
   - Path: deferred. A full-screen pixel pass after the deferred sun and lamp passes, reading the COLOR2 normal, the distance and
     the albedo, adding light x albedo. No compute in D3D9: a pixel shader (or a Vulkan pass through the DXVK fork).
   - Needs: where the water is. Astral gathers the engine's water objects and the land height on the CPU into a small top-down
     mask. We have the current water height (`TESR_WaterSettings`); cells with several water planes need a gather like theirs.
   - Cost: believed 0.3-0.8 ms at 1440p (one pass, lamps x pattern taps, only pixels within Reach of water). Not measured.
   - Effort: 3-4 sessions (water mask, pass, lamp list, tuning); 1 more for the pool-floor refraction.
   - Licence: GPL-3.0-only model and atlases; port by writing fresh from the description, credit qtuna, ship the six atlases
     with their notice.
   - Risk: ripples on walls far from water if the water mask is wrong; the ps_3_0 instruction limit with many lamps (cap lamps
     per pixel as they do, 12); interiors with water at several heights.
2. **Living Windows interiors (good idea, art-bound).**
   - Player sees: Strip and Freeside windows that look into lit rooms with curtains and figures at night, rooms that shift with
     the viewing angle, dirty old glass, rain running down the panes.
   - Path: deferred for the opaque static window meshes (the room colour goes into the pane pixels' albedo and glow in the
     G-buffer); forward only for glass that stays forward. Windows found by texture path rules plus a rule file for mods.
   - Cost: believed small (only window pixels, a few atlas taps). Their window sun shafts add a half-res march; FNV interiors
     have no sun, so skip the shafts.
   - Effort: 4-6 sessions of code, plus new atlases: theirs are medieval Nordic rooms and would look wrong in FNV. Room 04
     would need to make FNV rooms (diners, casino rooms, apartments, ruins).
   - Licence: code GPL-3.0; atlases GPL-3.0 but the wrong look.
   - Risk: false windows (signs, glass cases, boarded panes); many FNV windows are broken or dark by design.
3. **Wet grass darkening cap (tiny check).** Look at grass in heavy rain with our wetness on; if it goes near black, a cap like
   theirs is one setting. Under 1 session. Not run.
4. **Volumetric mist pockets (look polish).** We already have froxel fog with wind; their near-camera pockets would add drifting
   mist for mornings at the lake or in Zion. Fog pass, not tied to deferred. 1-2 sessions, cost believed under 0.2 ms on top
   of the froxels. Low priority.
5. **Frame generation (big, park).** Useful for a CPU-bound game, but it needs a Vulkan present path for DLSS-G or FSR3 FG
   through our DXVK fork, HUD and menu handling, and vendor SDK terms. 6+ sessions, high risk. Driver Smooth Motion already
   covers part of it.

### Skip
Interior eye adaptation (we have it), DLSS Neural Rendering (vendor preview, no way to test), Fur Shells (needs Bottled's whole
Fur Shells first; skinned, forward; few furry FNV creatures), snow work, Seam Probe, and every Skyrim-only piece listed above.

### Not covered here
Bottled's own features that Astral inherits (Physical Sky, Rainbows, Procedural Lightning, Rain Refraction, Heat Haze, Night Sky,
Footstep Particles, HiZ Occlusion, Terrain Shadows, ...). They are not Astral's work; a separate Bottled feature study would
compare them with Vegas Shaders.

### Proven / believed / not run
- Proven (from the source): the target game and API, the licence files, the list of Astral-only features (the diff against
  Bottled 5c2f2d3d9), what Caustic Light and Living Windows read and how; our gaps by grep of `VegasShaders.defaults.toml` and
  `docs\EFFECTS.md` (underwater-only caustics, interior exposure present, froxel fog with wind present, no frame generation).
- Believed: the frame costs and session counts above; that our G-buffer carries what Caustic Light needs.
- Not run: nothing of Astral was built or run (it cannot run in FNV); no in-game look at grass in rain.

## The FNVGodot-Bridge study

Written 2026-11-28 (room 02, read only). Source: github.com/gidHog/FNVGodot-Bridge, cloned at `F:\VS-scratch\fnvgodot-bridge`, HEAD c36a5b5
(5 commits, first release 2026-11-28). Owner's ask: "can we study this and see if it's superior in anyway to VS in implementation".
Paths below are in that clone; nothing was built or run.

### What it is
- A **replacement renderer**, not a shader mod. A small xNVSE plugin (`GodotBridge.dll`, `plugin\main.cpp` 2.7k lines) mirrors the live
  world every frame over TCP 127.0.0.1:47800 to a **separate 64-bit Godot 4.7.2 process** (Vulkan Forward+, `godot\project.godot:14`,
  ~13k lines of GDScript, ~1.1k lines of shaders), whose window is kept on top of the game window (`main.cpp:2249-2278`).
- The game keeps running gameplay, scripts, Havok, audio and its own D3D9 device. In replace mode the plugin marks the TES world, LOD,
  water-LOD and sky roots as culled and turns grass off (`main.cpp:249-274`), so the game draws only HUD, menus and first person; its
  Present is cleared to black and paced to 60 FPS (`plugin\UiCapture.h:126-140`, pacer `:84-124`).
- Godot loads every NIF and texture itself from the BSAs and loose files in load order (`godot\scripts\fnv_vfs.gd`, `nif_loader.gd`),
  caches them (`model_cache.gd`), and re-implements each game shader family in Godot shaders (`godot\shaders\fnv_lit.gdshaderinc`).
  Offline steps: model/texture cache, LOD/HLOD bake, interior VoxelGI bake (~25 min), and an optional AI upscale with generated PBR maps
  (SeedVR2, CUDA).
- Maturity: early. `docs\STATUS.md` (2026-11-17) marks most effects "built, not seen in game"; missing: distant trees, decals (blood,
  bullet holes), beam/Tesla effects, actor effect shaders, fog volumes, DOF, motion blur, image-space modifiers (hit flash, drugs,
  radiation, VATS), god rays, FaceGen morphs, intro videos, light flicker. README known issues: computers broken, wrong glows, stutter
  after cell changes. Exit crashes in xNVSE are hidden by `TerminateProcess` (`main.cpp:2597-2605`). About 100 hard-coded 1.4.0.525
  offsets, read under `__try` (faults are swallowed, not fixed).
- Licence: GPL-3.0 (`LICENSE`). Vegas Shaders is not GPL (`License.md`), so **no code from it may go into our repo**; ideas only,
  written fresh.

### Area by area

| Area | FNVGodot-Bridge | Vegas Shaders | Verdict |
| --- | --- | --- | --- |
| Hooking | One D3D9 vtable patch (Present, `UiCapture.h:143-155`), the xNVSE main-loop message (`main.cpp:2578-2608`), cull flags on TES roots, three LOD fade floats. | Many hooks inside the game's render passes plus the DXVK fork bridge (`src\NewVegas\Hooks\Render.cpp`). | Simpler, but only because it throws the game's renderer away. **Not comparable.** |
| Scene capture | Walks every loaded cell's refs each frame on the game's main thread (`main.cpp:481-502`, `:1550-1581`), sends only changed transforms (`:1727-1762`), bones, lights (`:624`), sun and environment, land, UI tiles. Re-implements every material. | Sees exactly what the game draws as it draws it; every game feature (decals, effect shaders, VATS, image-space modifiers, distant trees) keeps working. | **Worse** for coverage: each game feature must be rebuilt in Godot (STATUS.md missing list). |
| Deferred / G-buffer | None: Godot clustered Forward+. | Deferred G-buffer, sun and lamp passes ("Deferred vs forward"). | **Not comparable**; both shade many lamps. |
| GI | SDFGI outdoors (6 cascades), baked VoxelGI per interior cell (`godot\scripts\bridge.gd:505-516`, `:546-563`). | AmbientLight (screen space); RT GI unofficial and off (`GI = false`). | **Better on paper** (whole-scene bounce); cost unknown, 25 min bake. |
| Shadows | Sun atlas 8192, 250 m; up to 16 shadowed lamps (4 cubes, the rest dual paraboloid), atlas 8192 (`graphics_options.gd:54-58`, `bridge.gd:1079-1088`). Contact shadows: 16-step, sun-only compute march (`post_effects.gd:12-140`). | 24 shadowed lamps by default, up to 64 with the per-object lamp shadows (`src\effects\ShadowsInteriors\defaults.toml:12` LightPoints), 512 per cube face ("Deferred shadow quality"); Bottled SSS for the sun and for 2-4 lamps. | Lamp shadow count **worse** (16 vs our 24); per-lamp resolution believed **better** (shared 8192 atlas vs our 512 faces; its 12 dual-paraboloid lamps are cheaper but seam); contact shadows **equal or worse**. No cost numbers. |
| Reflections | Godot SSR outdoors only, off indoors on purpose; no reflection probes; water with Fresnel SSR (`bridge.gd:562-566`). | Unified reflections: Hi-Z trace, world cube, sky panorama, puddle mirrors. | **Worse.** |
| Wetness, rain | One global wetness float, used by grass only (`weather_fx.gd:52-68`); GPU particle rain. | Deferred wetness, puddles, GearWeather, rain on surfaces. | **Worse.** |
| Post effects | Godot glow, SSAO, SSIL, fog, AgX, FSR 2.2, auto-exposure; a custom desert heat shimmer (`post_effects.gd:219` on). | Full set (EFFECTS.md index), DLSS and FSR, AgX among 11 tonemappers. | **Worse** (many missing), except the desert heat shimmer we lack. |
| Settings, UI | F10 window with Low-Extreme presets saved as JSON (`graphics_options.gd:311-341`); F3 FPS with 1 % / 0.1 % lows (`perf_overlay.gd`); F6 issue reporter (`issue_reporter.gd`). | toml plus in-game menu, many separate switches. | Presets and the reporter **better**; closer to our rule "one preset beats several switches". |
| CPU per frame | Game: full simulation, the ref walk and serialisation on the main thread, a UI-only D3D9 frame. World draw submission moves to another process (Godot, its own threads) with instanced far objects and grass (`lod_manager.gd:487-546`). The plugin logs its own cost (`main.cpp:2510-2523`) but publishes no number. | Owner's probe: 6.6 ms CPU a frame on the Strip, +2.6 ms inside DXVK, all on the game's thread. | **Believed better** for the game thread, by not drawing the world in D3D9 at all. Not measured; Godot's own CPU cost unknown. |
| GPU per frame | Unknown; defaults are heavy (SDFGI, two 8192 atlases, MSAA 4x `project.godot:20`). | Measured per effect. | **Not comparable** (no numbers). |
| DXVK | Not used or needed; the D3D9 vtable patch is untested under DXVK. | Built on our DXVK fork. | **Not comparable.** |
| Making shaders | Godot shader language; F8 in-game editor saves overrides, delete the file to revert (`shader_editor.gd:1-12`, `live_overrides.gd:1-12`). | ps_3_0 HLSL limits, or GLSL to SPIR-V; rebuild and restart. | **Better.** |
| Stability | 5 commits, many items unseen in game, focus and Alt-Tab gaps, image believed at least a frame behind the game (no sync, `main.cpp:2541-2548`), 60 FPS pacing. | Years of use, bench-tested. | **Worse.** |

### Verdict
Not superior as a whole: it is a different kind of project (a second renderer fed by the game), and early. Where it is ahead: shader
editing, whole-scene GI and lamp shadow resolution (both on paper, no cost numbers; we shadow more lamps, 24 vs 16), and, by design, the game thread no longer
submits the world's draws, which is the kind of CPU cost the 6.6 ms probe shows. It pays for that by losing everything the game draws
itself, with a frame of latency and a second process. Nothing in it can replace the deferred path.

### Worth borrowing (ideas, written fresh)
1. **Cull-the-world CPU probe: not needed.** Our Performance probe (`src\core\DeepProbe.h`: CPU update / render / Present split,
   CpuSplit per engine stage, per-DLL sampling, draw-call census) already splits the 6.6 ms; read it there before item 2.
2. **Instanced far objects and grass.** One instanced draw per model for distant objects (the bridge's MultiMesh batching): D3D9
   stream-frequency instancing in the G-buffer pass. Engine draw paths, needs room 05 facts. 4-6 sessions, high risk. Only if the Performance probe shows world draw submission is the big share.
3. **Light from glowing meshes.** Unshadowed deferred lamps placed on neon signs, marquees and vending machines (bridge: up to 4 per
   model, 2.6 m clusters, `emissive_lights.gd:9-30`). Pick them by glow map, not by name keywords. Deferred path. 2-3 sessions, medium
   risk (lamp cost on the Strip, false lights).
4. **Desert heat shimmer.** Day only, beyond ~40 m, off in rain and interiors (ours exists only around a hot gun barrel). Post pass,
   1 session, low risk, cost believed under 0.2 ms.
5. **Issue reporter key.** Writes camera, cell, the object under the crosshair, nearby lights and the live settings to a JSON the bench
   can replay. 1 session, low risk.
6. **Quality presets.** One Low-Ultra switch over our many keys (product rule). 1-2 sessions; risk: presets overriding the owner's
   tuned values (needs a migration).
7. **Live shader edit.** Recompile a changed shader in game without a restart (check first what our runtime compile already allows).
   1-2 sessions, low risk.

### Skip
The two-process design itself, SDFGI and VoxelGI (Godot engine features; our GI route is RT and AmbientLight), AI texture upscaling and
PN-triangle remeshing (asset work for room 04, not renderer work), the 60 FPS Present pacer, and the sky-dome fill shadow (our AO covers it).

### Proven / believed / not run
- Proven (read in the source, key lines re-checked by hand): the architecture, the hooks, the world cull, the Present clear and pacer,
  the per-frame ref walk on the main thread, Forward+, SSR off indoors, the GI and shadow defaults, the licence.
- Believed: a frame or more of latency; that the game thread gets cheaper; the costs and session counts above. STATUS.md lags the
  README, so some "missing" items may work by now.
- Not run: nothing of it was built or run; no CPU or GPU numbers exist for it anywhere in its repo.
