# Deferred vs forward in Vegas Shaders 1.1.0 (2026-11-18)

How upstream 1.1.0's deferred renderer works next to the old forward shaders, read from the code on `vs-1.1.0-plus-fork`
(= upstream 1.1.0 + small fork items; the renderer is upstream's, untouched). Paths are under `src\`. Nothing here was run in game.
The `docs/next/*.md` files the code cites (results, plans, measurements) are not in either repo: anything that rests only on them is
marked "claimed".

## In short
The world's opaque surfaces (objects, parallax, land, far land, skin, alpha-tested hair, near grass, leaves) no longer light themselves.
They draw once into a G-buffer and two full-screen passes add the sun and every lamp afterwards. Water, sky, particles, decals, the game's
own non-template shaders, first person and anything blended or fading still draw forward. Glass and blended hair get "forward+": still
forward, but lit by every lamp from the same lamp table. `DeferredRenderer 0` turns all of it off and gives the old forward drawing.

## Where things are
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

## Proven (from the code)

### What the switch does
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

### The G-buffer
Three extra `D3DFMT_A16B16G16R16F` targets at scene size, MRT slots 1-3, beside the scene target in slot 0 (linear light under LinearScene)
(`CapturedNormals.h:807,813,826,853,1306,1323`). Layout v2 (forced on):
- COLOR1 albedo: rgb linear base colour (texture x vertex colour); a = written mark, or in lighting-free records a code (1 + keep with sun,
  3 + keep no sun, 5 + keep "sun kept"), keep = 1 - fog.
- COLOR2 material: r = shading model (1 object, 2 gear, 3 cloth, 4 land, 5 plants, 6 skin, 7 grass, 8 hair, 9 eye, 10 unlit) + 16 x
  specular bit + 0.49 x metal; g and a = the normal-mapped shading normal (octahedral; foliage packs a fog keep into g); b = roughness.
- COLOR3 smooth normal: rg = octahedral vertex normal facing the camera; b = camera distance; a = 1 world / 2 actor (+ a sun-visibility
  and highlight-F0 code in the fraction, `DeferredPixel.hlsl` FDSunVisCode).
- Depth: an INTZ copy and, in mode 2, an R32F view-z texture (`DeferredDepth.h:1-30`). No motion vectors in the G-buffer (none found).

### Which pass fills it, and how lighting is skipped
- The main opaque list is wrapped by `GrassFirst::MainListHook` (`NewVegas/Hooks/Render.cpp:2894`, patched at :3157);
  `CapturedNormals::BeginSpan` binds the targets for its span. Only Vegas Shaders' own templates write them; other draws are masked off.
- The templates are compiled with `CAPTURED_NORMALS_ON` (4 under v2, `ShaderRecord.cpp:399-401`). Inside the span an eligible draw
  binds a deferred record instead: mode 1 `LAMPS_DEFERRED 3` (lamps compiled out, sun kept), mode 2 `LAMPS_DEFERRED 2` (sun, lamps and
  their shadows compiled out). Ambient, glow, emittance and fog stay in the record. Forward lighting is not computed and thrown away:
  it is compiled out, and the game's added lamp and highlight passes for that object are skipped (`DeferredRenderer.h:19-21,226-240`).
  The texture-multiply and fog pass of a light-only (AD) base stay forward.
- A blended or fading base pass takes the forward record and keeps its own lamp passes (`DeferredRenderer.h:250-277`).

### Where lighting is resolved
At the end of the main list, after the G-buffer is resolved and before LinearScene encodes the frame, in this order
(`Render.cpp:2913-2917`): depth copy, DeferredRenderer bookkeeping, skin prep, **DeferredSun** (one full-screen pass: the templates' own
sun term x their own shadow lookup: cascades, contact hardening, character map, cloud shadows, RT sun shadow where it applies; x (1 - fog);
added ONE/ONE in linear light), **DeferredLamps** (one full-screen pass over the lamp table's froxel lists, the objects' own lamp maths,
atlas shadows; a separate skin lamp pass), **FlashlightDeferred** (the beam on the G-buffer), skin scattering (`DeferredSkinSSS 2`, a
screen-space Burley-profile blur). Fog is rebuilt in the passes (`DeferredFog.h`); with VolumetricFog on (default) the game draws no fog
and no frame copy is needed.

### Family by family, with shipped defaults (mode 2)
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

### The forward shaders (still used by everything above marked forward, and by everything in mode 0)
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

### Screen effects in the two modes
- The G-buffer exists in mode 0 too: CapturedNormals, MaterialBuffer and AlbedoBuffer default on by their own keys (defaults :653-673),
  and the captured normals are merged into `TESR_NormalsBuffer` for every reader (`ShaderManager.cpp:2308-2310`). So AO, AmbientLight
  bounce, Reflections, Snow, WetWorld, BloodFlow, the deferred flashlight and DLSS RR guides read it in both modes.
- Opt-ins that use the v2 shading normal or G-buffer colour, all off: DeferredReflections, DeferredRRGuides, DeferredALAlbedo.
- Sun shadows: looked up in the lit shaders (forward) or in DeferredSun (mode 2); the screen-space contact shadows and the RT sun apply run
  in the effect chain in both modes (`EffectChain.cpp:462-495`).
- VolumetricLight, VolumetricFog and TAA have no DeferredRenderer branch in their C++ (grep).

### Gaps the code itself lists
- DeferredSun does not carry: metal's bridged tint, the wet film's sun highlight, the land's highlight with UsePBR off, parallax's
  vanilla sun with POM shadows off, and blended (fading) draws' terms (`Shaders/DeferredSun.hlsl:17-18`).
- Leaves get no room lamps under the deferred path (above). First person stays forward.
- Some comments in the shipped defaults are stale against the code: GBufferV2 "test only, nothing on screen changes" (:674), DeferredDepth
  "nothing reads them yet" (:647), `SettingForce.h:23` "no reader on this branch". The renderer now depends on all three.

## Believed (not proven)
- VolumetricLight / VolumetricFog / TAA behave the same in both modes (no branch found; not run).
- Bend's in-lighting contact shadows do not reach the deferred sun (DeferredSun.hlsl has no such opt-in); Bend is off by default.
- Tree trunks and branches are deferred as objects (inferred from the shader pairing, not traced).
- Menus and the Pip-Boy are forward (inferred from the span's size and context gate).

## Costs and visual differences (measured vs claimed)
- Measured (code comment, source file absent): the DeferredSun pass, GTX 1070, native D3D9 1080p, frame replay: 0.91 ms, 1.08 ms with
  the fade branch compiled in (`DeferredSun.h:200-202`). That is one pass, not deferred vs forward.
- Measured, forward only: ManyLamps one pass, Vault 21 lobby, GPU not named: frame 20.3-21.0 vs 22.9-23.1 ms, world draws 6,015 vs 8,677
  (`ManyLamps.h:12-15`). Lamp-shadow atlas on native, GTX 1070: Vault 21 world 16.0 vs 15.5 ms (`PointShadowForward.h:171`).
- Claimed: "the picture changes slightly; the first start takes longer (more shaders to compile)" (defaults :677); DeferredDistant
  "changes the image slightly" (:702); earlier bugs fixed per comments (deferred land 15-25 % darker by day; emissive objects with 3+
  lamps; silver glass bottles).
- Not measured anywhere in the repo: whole-frame DeferredRenderer 0 vs 2, any RTX or AMD number, 1.0.x vs 1.1.0.

## Open
- Measure DeferredRenderer 0 vs 2 on this PC (frame time alone, the same spots indoors and outdoors) and look at the differences by eye.
- Ask upstream for `docs/next/` (the FD-* results), or treat every number cited from it as unverified.
- Fix the stale defaults comments (GBufferV2, DeferredDepth) and the stale `DeferredRenderer.h` / `SettingForce.h:23` comments.
