# PBR

Physically based lighting for objects and the ground: Lambert diffuse plus GGX highlights for the sun and lamps, metal, and
optional per-texture material maps. Settings: `[Shaders.PBR.*]` (`defaults.toml` here). Shader code: `Shaders/Includes/PBR.hlsl`
(the lighting maths), `Shaders/Includes/Object.hlsl` (material inputs), `Shaders/ObjectTemplate.hlsl` / `ParallaxTemplate.hlsl`
(the object shaders that use them).

This doc is still partial: it covers the vanilla bridge and the material maps. The rest of the effect is described in the settings'
own descriptions.

## Which data decides a highlight

| Where | Roughness and highlight strength from | Code |
|---|---|---|
| texture with an `_rmaos` map (`[Shaders.PBR.Rmaos]` On) | the map (R roughness, A specular level, G metal with `MetalFromMap`); with `VanillaBridgeOverMaps` the bridge instead (the map keeps B occlusion and G metal) | `RmaosRead`, `getRoughness`, `BRIDGE_OVER_MAP` |
| everything else, where the normal map's alpha is above ~0.04 | the game's own Blinn-Phong data through the **vanilla bridge** (below) | `BridgeSetup`, `BridgeSpecF0` |
| where that alpha is ~0 (the game drew no highlight) | the old values: `1 - alpha` (or the material class's `rough=`), F0 0.04 | `getRoughness`, `getMetalRoughness` |
| a material class with `authored=replace` | the class's `rough=` everywhere, as before the bridge | `VegasShaders.materials.txt`, `MaterialRules.h` |

## Vanilla bridge (`[Shaders.PBR.Main] VanillaBridge`, 2026-11-05)

Diagnosis from the PBR-Reloaded fork's author: authored shine (power armour, polished metal) lost its sun glints because every
highlight went through GGX with a generic roughness. The game's highlight is `mask x (N.H)^n x light`: the normal map's alpha is a
highlight **strength** (not a gloss map), `n` is the mesh's glossiness (`NiMaterialProperty::m_fShine`, sent as `Toggles.z`, c27; the
4+ lamp OPT shaders read it from `PSLightColor[1].w`), and the game's highlight is not normalised and has no Fresnel. Before the
bridge NVR used the mask as `1 - roughness`, dropped `n`, and fixed the strength at F0 0.04: a mask of 0.5 with n = 30 gave a
highlight peak of 0.16 x the light at roughness 0.5 where the game drew 0.5 x the light at about a fifth of that width.

The bridge (`Includes/Object.hlsl`) maps the authored lobe onto the same GGX BRDF:

- **Width**: `(N.H)^n ~ exp(-n t^2 / 2)` is the Beckmann lobe of `alpha^2 = 2 / (n + 2)` (Walter et al. 2007);
  roughness `= alpha^(1/2) = (2 / (n + 2))^(1/4)` (NVR's `GGX()` takes roughness^4 = alpha^2).
- **Peak**: GGX lit and seen head-on returns `D(1) F Vis N.L pi = F / (4 alpha^2)` of the light (Vis = 1/4 for both Smith terms);
  the game's returns `mask`. So the highlight's F0 is `4 alpha^2 mask = 8 mask / (n + 2)` (at most 1). This is the inverse of the
  Blinn-Phong normalisation `(n + 8) / (8 pi)`: the game's lobe is unnormalised, so a sharp lobe of the same peak holds less energy.
- **Why alpha^2 = 2/(n+2) and not a GGX-specific fit** (numbers: `tests/furnace/bridge_lobe_fit.py`): with the peak matched,
  GGX at that alpha has 0.77-0.91 of the Blinn-Phong half width and 1.04-1.20 x its energy for n = 2-200. An exact energy match
  needs `(1.7-1.9) / (n + 2)` and narrows the core to 0.73 x; the least-squares shape fit is `(1.9-2.6) / (n + 2)`. 2 is between.
- **Mask not decoded** under linear lighting: the game sums `(albedo + mask) x light` in gamma space. Over albedo 0.05-0.6 and mask
  0.05-1 the mask used as it is matches that on average (mean error +0.004 of full scale, mean |error| 0.06); decoding it
  (mask^2.2) loses 0.14 on average, which is exactly the "glints gone" look.
- **Only the highlight** takes the bridged F0 (`PBR.hlsl PBR_SPEC_FRESNEL`); the diffuse keeps the dielectric's `(1 - F)` share,
  so a surface's diffuse light does not change. Metal keeps the bridged strength, tinted by its colour's hue (normalised to the
  brightest channel). Fresnel still rises to 1 at grazing angles and the GGX highlight still brightens as the view grazes
  (`1 / N.V`): those are the microfacet model's, not the game's.
- **Gap fill**: the bridge's weight ramps over mask 0-0.04 (`BridgeT`): where the game drew no highlight, the old roughness and
  F0 0.04 (and a material class's `rough=`) stay, so matte surfaces are unchanged. `Roughness` (per weather) scales the bridged
  roughness as it did the old one; `Reflectivity` scales its F0 as before.

Where it applies: the object and parallax pixel shaders that draw a highlight and know `n` (`NVR_VANILLA_BRIDGE` in
`ObjectTemplate.hlsl` / `ParallaxTemplate.hlsl`): every SPECULAR variant with Toggles, the highlight-only passes, and the 4+ lamp
OPT ones, except the 6-lamp OPT shader at the driver's slot limit (SLS2035 with `SPECULAR_LAMPS6`, `NVR_SLOT_TIGHT`; the bridge
costs about 40-46 instruction slots). Not hair, not the skin shader, not terrain.

Switches: `VanillaBridge` (restart; off = the shaders exactly as before, `VANILLA_BRIDGE_ON` is not defined) and
`VanillaBridgeAmount` (live, TESR_PBRData.x, the lane the unused Metallicness setting had: 0 = the old values exactly, a lerp by 0).

`VanillaBridgeOverMaps` (live, off by default; + 2 in the same lane): the bridge also decides where an `_rmaos` map is bound. Vegas
Shaders' generated maps guess roughness from the colour and carry no glossiness: on an install with them (power armour has one),
the bridge changes nothing on those objects unless this is on.

`VanillaBridgeDielectricMax` (restart, default 0.08; 1 = no cap, the 1.0.5 bridge exactly; `VANILLA_BRIDGE_DIELECTRIC_MAX`, defined
only below 1). 2026-11-06 report (Bale, 1.0.5): cliffs very shiny. The peak match gives F0 = 8 mask / (n + 2), so a wide authored lobe
(glossiness about 10, common on world meshes) with a bright mask, or a normal map with no alpha (reads as 1), came out at F0 0.3-0.7
on rock, concrete, wood and plaster: a near-metal sheen. The non-metal share of the bridged F0 is capped (real non-metals: 0.02-0.08);
the metal share keeps the full authored strength. Unseen on our own install: there the generated `_rmaos` maps cover the rock and
the bridge stays out (VanillaBridgeOverMaps off), so the case only shows on installs without the maps.

Measured (2026-11-05, bench slot, Goodsprings 13:00, the player in T-45d power armour in third person, 1920x1080; region = the
armour; "glint px" = pixels standing more than 40 levels above their 31x31 neighbourhood mean; lists `pbrbridge-s1b/s2/s3`):

| picture | armour p99 / p99.9 luma | glint px | glint excess sum |
|---|---|---|---|
| before the bridge, as installed (maps, rules, metal on) | 116 / 133 | 2735 | 438k |
| before, `_rmaos` off / + metal off / rules off (restart) | 114 / 155, 112 / 132, 117 / 134 | 2174, 1897, 2814 | 389k, 445k, 451k |
| bridge, maps on (default: the armour's generated map decides) | 115 / 133 | 2742 | 444k |
| bridge, maps off | 144 / 187 | 3851 | 547k |
| bridge, maps on + `VanillaBridgeOverMaps` | 145 / 188 | 4037 | 566k |
| same launch, `VanillaBridgeAmount` 0 | 112-114 / 134-159 | 2221-2603 | 393-422k |
| the game's own shaders (PBR off) | 201 / 244 | 8600 | 1.19M |

So neither the maps, the material rules nor the metal look killed the glints (each moves them by noise); the generic GGX mapping
did. With the bridge the armour's highlight pixels grow by 45-73 % and the brightest 1 % by 30 luma levels; the rest of the
picture moves 0.87-1.4 levels on average (the noise between two launches of one build is 1.95). The game's own picture stays
brighter because its diffuse light is also brighter on the armour (region mean 69 vs 43-45: gamma-space lighting and no metal look,
which leaves the metal parts no diffuse light), not because of the highlight alone.

Known gaps: the FoldHighlights light-only passes (objects reached by 3+ lamps indoors) and the DeferredLamps pass have no `n`
and keep the old highlight (DeferredLamps reads the bridged roughness from the material buffer, but with F0 0.04).

## Material maps (`_rmaos`)

`[Shaders.PBR.Rmaos] Enabled` (restart). Where a texture has `<name>_rmaos.dds` beside it or under `textures\pbr\` at the same
path, the object shaders read it on sampler s2 (`Render.cpp` RmaosDraw binds it per draw, sets `NVR_Rmaos` c220). Layout is the
Community Shaders "True PBR" one:

| Channel | Meaning | Used as |
|---|---|---|
| R | roughness | replaces the guess from the normal map's alpha (`getRoughness`) |
| G | metal | only with `MetalFromMap` (default off: Vegas Shaders' generated maps carry a guessed metal) |
| B | ambient occlusion, white = none | darkens the ambient light (`RmaosAmbient`, times `Occlusion` and the weather's `AOScale`) |
| A | specular level | non-metal reflectance F0 = 0.04 x A (`SpecularFromMap`, 2026-11-02) |

Specular level: white (1) is the usual 0.04, so maps without a painted level (and Vegas Shaders' generated maps, which write A = 1)
look exactly as before; darker values reflect less. It is set through `PBR_DIELECTRIC_F0`, which `Object.hlsl` defines before it
includes `PBR.hlsl`; every other includer of `PBR.hlsl` (skin, terrain, water) gets the plain 0.04. Not applied in the 6-lamp
interior shader (SLS2035, `NVR_SLOT_TIGHT`), which is at the instruction count the driver refuses. `SpecularFromMap = false`
compiles it out (`RMAOS_SPEC_OFF`, `ShaderRecord.cpp`).

Edge cases: a map that has just loaded fades in over `FadeIn` seconds (the specular level too, from 0.04); `DebugView` 1-4 shows
roughness, metal, occlusion, or which objects have a map. MaterialAware reflections read the map's roughness and metal from the
scene's alpha (`core/ReflMaterial.h`).

How to check it: an object with a hand-made map whose alpha is dark should show weaker sun and lamp highlights on its non-metal
parts than before; with Vegas Shaders' generated maps nothing changes.
