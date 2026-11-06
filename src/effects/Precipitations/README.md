# Precipitations (rain: screen-space falling drops and streaks)

## 1. Purpose
While it rains outdoors, the picture gets falling rain streaks laid over the view. Drops are drawn in layers at increasing distance from the camera, so near streaks are larger and far ones finer. Each drop bends the picture behind it (a small refraction) and adds the scene's bloom glow on top. Rain only shows where the sun's shadow map says the point is open to the sky, so it stops under roofs and overhangs. Pixels with no drops are left as they are.

## 2. When it runs
- Enabled by the settings section Shaders.Precipitations.Status, key Enabled (default true).
- Per frame it renders only when the rain amount (TESR_RainData x, section 3) is above zero, the player is outdoors, and the camera is not underwater.
- The rain amount is animated, not a switch. When the weather becomes rainy it rises to 1 over 0.05 game hours. When rain stops it falls to 0 over 0.07 game hours. The effect keeps drawing while the amount is above zero, so the fade-out tail is visible.
- Weather state: the game's current weather is the rainy type. Setting Shaders.Precipitations.Main ForceRain (default false) makes the game count as raining outdoors in any weather and clears the snow flag while it does so. It is applied in the per-frame game-state update, before the effects run.
- Frame order (src/core/ShaderManager.cpp): inside the weather block that runs after the TAA pass. Rain renders first of the weather effects, then Snow, then the GPU particle draw (off by default), then the legacy bloom. In the frame chain's final-effect order Rain is the first entry.
- Final-pass rule: if Rain is the last enabled effect to draw this frame, its single pass writes straight to the game target. Otherwise it writes into the chain's spare target. Either way the output must cover the whole frame.
- Rain's own ShouldRender does not test for menus; whether it draws in a menu is not confirmed here.

## 3. Inputs
### Textures (sampler slot = D3D9 declaration order; the C++ binds by name)
| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | TESR_SourceBuffer | the picture as it stands before this effect (a copy the frame keeps). The drops' background and refracted colour come from here | linear, clamp |
| s1 | TESR_RenderedBuffer | declared but never read by the shader; not bound in the Vulkan pass | linear, clamp |
| s2 | TESR_DepthBuffer | the combined depth buffer: linear view distance divided by the far distance | linear, clamp |
| s3 | TESR_OrthoMapBuffer | the sun's orthographic shadow depth map for exteriors | linear, clamp; read at level 0 only |
| s4 | TESR_BloomBuffer | the bloom texture at its first level (from the bloom effect) | linear, clamp |

The Vulkan pass binds, in descriptor set 1: binding 0 source picture, 1 depth, 2 ortho map, 3 bloom. The unused rendered-buffer slot has no Vulkan binding.

### Constants
| Name | Components and meaning | Notes |
|---|---|---|
| TESR_RainData | x = rain amount (0 to 1, animated); y = streak vertical scale (VerticalScale); z = fall speed multiplier (Speed); w = drop opacity (Opacity) | registered by the effect |
| TESR_RainAspect | x = refraction strength (Refraction); y = base colouring (Coloring); z = the Bloom setting (written by the C++, read by no shader, see section 9); w = SkipHiddenLayers switch (1 on, 0 off) | registered by the effect |
| TESR_GameTime | z = animation clock; others not used | engine constant |
| TESR_ShadowCameraToLightTransformOrtho | 4x4 world-to-sun-ortho-clip transform | engine constant; a push value in the Vulkan pass |
| TESR_ViewTransform, TESR_ProjectionTransform, TESR_CameraData | camera basis; projection entries [0][0] and [1][1]; camera data y = far distance | engine constants, used through the shared depth include |
| TESR_SunDirection | direction toward the sun | engine constant |
| TESR_SunColor | sun colour | engine constant |
| TESR_ReciprocalResolution, TESR_FogColor, TESR_WorldViewProjectionTransform | declared, not read | |

## 4. Settings
| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| Shaders.Precipitations.Status | Enabled | bool | true | Turns the effect on or off. |
| Shaders.Precipitations.Main | VerticalScale | float | 1.6 | Length of the streaks: higher = longer, more stretched drops. |
| Shaders.Precipitations.Main | Speed | float | 3.0 | Fall speed multiplier. |
| Shaders.Precipitations.Main | Opacity | float | 1.0 | How strongly the drop colour replaces the picture (scales the drop mix). |
| Shaders.Precipitations.Main | Refraction | float | 1.0 | How far the drops shift the picture behind them. |
| Shaders.Precipitations.Main | Coloring | float | 1.1 | Base brightness added to the refracted colour inside each drop (multiplied by 0.02). |
| Shaders.Precipitations.Main | Bloom | float | 0.5 | Meant to weight the bloom glow in the drops. No shader reads it (section 9); the bloom always enters at full weight. |
| Shaders.Precipitations.Main | SkipHiddenLayers | bool | true | Skips layers that lie behind the pixel (they add nothing). Same picture, less work. |
| Shaders.Precipitations.Main | ForceRain | bool | false | Not a look setting: makes the game count as raining outdoors (section 2). |

## 5. Passes and outputs
- One unnamed technique with one pass: a full-screen vertex pass and a pixel pass. The C++ asks for technique index 0; any other index draws nothing.
- Output: one colour per pixel at full frame size, in the frame target's format. Nothing is pre-cleared, so every pixel must be written. Pixels with no drops return the source picture after the linearise and delinearise round trip.
- The Vulkan pass (section 7) writes to the same target.

## 6. Behaviour details
- Colour space: source colours are converted from display (sRGB) to linear with the standard sRGB curve, combined there, and converted back with the inverse curve. The refracted source colour is converted the same way. The bloom sample is added as read, without conversion (the shader does not convert it).
- Ray and layers: the world ray through the pixel comes from the camera basis and projection. The effect runs 14 layers (indices 1 to 14; index 0 is never drawn). Layer i is at 100 x i game units along the ray. A layer contributes only where the pixel's surface is further away than that distance; closer geometry hides it.
- Cylinder mapping: each layer's drop grid lies on a cylinder around the camera. The horizontal coordinate is the ray's azimuth, mapped into 0 to 1 with a half-turn offset. The vertical coordinate is the height over horizontal distance times 0.004 times VerticalScale.
- Drop grid: each layer is a grid of cells, each with a pseudo-random brightness and a streak-shaped drop that is elongated along the vertical. Each layer is offset horizontally by a term that depends on the layer and on the grid position, and vertically by Speed times the animation clock. A cell's brightness fades with the layer index (divided by 1 plus 0.2 times the index).
- Drop size: the size edge grows with the layer index and with the rain amount (the smaller of the layer index and 4 times the amount, scaled by 0.05, plus 0.005). The edge must never be zero, because the smooth step breaks at zero.
- Shadow test: each layer's drop is multiplied by the sun ortho-map test at the layer's point along the ray. The point counts as visible only if the map's stored depth is not closer than the point (bias 0.0001) and the point lies inside the map. Points outside the map do not show rain. This is the reverse of Snow, which counts outside points as open; keep the rain rule as it is (section 9).
- Division guard: a cell's brightness divides by a fractional product that can be zero. The divisor has a floor of 1e-4. Keep it.
- Refraction: the total drop amount shifts the sampling position diagonally (right by the total times Refraction, and up on screen by the same amount). The shifted position samples the source picture and the bloom texture.
- Drop colour: a tint that moves from mid-grey (0.5) toward twice the linearised sun colour, weighted by the square of the saturated dot product between the ray direction and the sun direction. Drops look brighter toward the sun.
- Final mix: the result is the linear mix between the original colour and (refracted source + bloom + tint times Coloring times 0.02), weighted by the total drop amount times Opacity, then converted back to display space.
- SkipHiddenLayers: a layer is evaluated only when the pixel's depth is greater than the layer's distance, or when the switch is off. Both settings give the same picture; the switch only changes cost.
- Render-rectangle frames: the shared depth include maps coordinates into the drawn part of the target before the ray and depth are worked out. The Vulkan pass is off in those frames (section 7).
- Reversed depth: the depth buffer holds linear view distance over far distance, so reversed depth does not arise.
- Jitter (TAA): the shader has no jitter term; it uses the picture as the frame has it.
- HDR vs LDR: input is the display-referred frame after tonemapping; output is the same kind of value.
- NaN/inf guards: the division floor above, and the explicit level-0 read of the ortho map (so the read is safe inside the layer branch). Both must stay.
- Things that look wrong if missed: a grey film, or no streaks (amount or ShouldRender wrong); rain under roofs (shadow test missing); the picture shifting by blocks (refraction scaled wrong); black or white speckles (division floor missing); rain indoors (interior gate missing); a grey tint instead of a sun-coloured one (sun colour missing).

## 7. Vulkan path
- Shader: src/hlsl/NewVegas/Compute/Precipitations.frag, a line-for-line port of the D3D9 pixel pass; the header records the matrix conventions and that the smooth step is written out. Compiled form: Precipitations.spv in the same folder. The pass is requested under the name "Precipitations".
- Selection (src/core/EffectRecord.cpp, EffectRecord::Render): the same rule as Snow. The Vulkan pass runs only with NvrDxvkPasses on, not in a render-rectangle frame, with the Vulkan extension available, when "Precipitations" is not in NvrDxvkKeepD3D9, and when no SM6 build covers the technique. Otherwise the D3D9 pass runs.
- C++ side: src/effects/Rain.cpp (class RainEffect, whose effect name is "Precipitations"), ExtRender. It returns false (D3D9 runs) if the technique index is not 0, if the shadow effect is missing, or if any of the textures is missing. The bloom texture is required: with no bloom texture the Vulkan pass does not run.
- Textures: descriptor set 1, binding 0 source picture, 1 combined depth, 2 ortho map, 3 bloom (first level). The sampler is the linear clamp sampler at binding 4; its index is read from the push block at offset 208.
- Push block (224 bytes; offsets in bytes):
  - 0: shadow-to-light matrix (four columns).
  - 64: view transform (four columns, as the engine stores the view matrix).
  - 128: view scale: x projection [0][0], y projection [1][1], z far distance (camera data y), w game clock z.
  - 144: rain data (TESR_RainData as registered).
  - 160: rain aspect (TESR_RainAspect as registered; its z component is sent but not read).
  - 176: sun direction.
  - 192: sun colour.
  - 208: sampler index (32-bit unsigned), then 12 bytes of padding.
- The Vulkan pass runs the same 14-layer loop, the same shadow test, the same colour mix, and the same guards: the division floor, the explicit level-0 ortho read, and the SkipHiddenLayers skip. The tint's saturated dot product is the same operation as the D3D9 shading term.
- The Vulkan pass does not run in render-rectangle frames, so it has no rectangle mapping.

## 8. Look and cost
- A correct result: thin, slightly bent streaks falling over open ground and sky, longer when VerticalScale is high, with a faint glow on them. Rain disappears under roofs and in shadows. Drops are brighter toward the sun. Only the drops change the picture.
- Typical wrong results: a grey wash (opacity or tint broken); streaks indoors or under roofs (gate or shadow test broken); the picture shifting by blocks (refraction broken); black or white sparkles (division floor missing); no refraction at all (refraction term missing); bloom too strong or absent (section 9).
- Cost class: per-pixel loop. One full-screen pass per frame, 14 layers, each with at most one ortho map lookup (skipped when SkipHiddenLayers skips the layer), plus three or four texture samples after the loop.

## 9. Open questions
- The Bloom setting ([Shaders.Precipitations.Main] Bloom, default 0.5) is written into the rain's aspect constant (its z component) but no shader reads that component. The drops' bloom is always at full weight, so the setting does nothing in either path. Decide whether the new version should honour it (the current picture is the reference for a like-for-like result) or retire the setting.
- The value the combined depth buffer holds for sky pixels is not confirmed here. The effect treats sky as further than every layer; confirm against the depth-combine pass.
- Whether Rain ever draws in a menu is not confirmed: ShouldRender has no menu test. The frame-level gating in ShaderManager was not traced in full.
- The shadow-test sense differs between Rain (points outside the ortho map show no rain) and Snow (points outside count as open). Both are the current behaviour; it was not checked whether the difference is intended.

## 10. Status of the wave 3 rewrite (2026-11-02, branch claude/rw3-weather)
Note (2026-11-05): this section is historical. The shipped `Precipitations.fx.hlsl` technique 0 is still the original 14-layer shader described in sections 2-8, and the Vulkan pass (`Compute/Precipitations.frag`, `.spv`, `RainEffect::ExtRender`) does exist and covers technique 0.
- New D3D9 effect: src/hlsl/NewVegas/Effects/Precipitations.fx.hlsl. One technique, the same sampler order (source, rendered, depth, ortho, bloom), the same constant names. Compiles (fx_2_0, all four defines).
- Settings are read as before. SkipHiddenLayers now only saves work: the picture is the same with it on or off (the layers behind the pixel contribute nothing).
- Chosen here, not in the spec: the refraction scale (0.004 of the total times Refraction in screen units), the drop cell grid (64 by 32 per layer), the drop hash and its 0.55 presence threshold, the streak shape, and the drop size as the spec's formula. The spec's division guard does not apply, because this shader has no division by a fractional product.
- Vulkan path (Compute/Precipitations.frag, .spv): NOT written yet. Without it the C++ falls back to D3D9.

## 11. Rain system (technique 1, UNOFFICIAL 2026-11-05, branch rain-system)
Design: `docs/rain/DESIGN.md`. Research: `docs/rain/research/`. Shader: `Effects/Includes/RainSystem.hlsl`, included by `Precipitations.fx.hlsl` as techniques `RainSystemTechnique` (index 1, the main pass) and `RainFarTechnique` (index 2, the far bands at half resolution). QA review: `docs/rain/research/06-07-qa-performance-review.md`.

### When it runs
- `[Shaders.Precipitations.System] Mode = 1` (the default) picks technique 1. `Mode = 0` gives the original layers (sections 2-8), and every `[Shaders.Precipitations.Main]` look key applies only to Mode 0.
- It is gated like Mode 0 (enabled, outdoors, not underwater, rain amount above 0). It also keeps drawing while drops are still drying on the lens.
- The passes sit in the same post-tonemap place as Mode 0 (after TAA). `EffectChain.cpp` calls `RainEffect::RenderFrame`. In Mode 1 with Quality 1-2 that first draws technique 2 into `TESR_RainFarBuffer` (half resolution, G16R16F, x = coverage of the far bands; `RenderEffectToRT`), then the main pass through the frame chain. Mode 0 draws technique 0 only.
- The rain now requests the ortho map itself while it rains (`orthoRequired`). Before, it relied on WetWorld to request it.

### What it draws
1. **Streak volume.** Drops sit in world-anchored vertical columns, sheared by the wind. Each pixel walks the columns along its view in up to three bands: cells of 32/96/288 units, reaching NearDistance / 30 % of the rest / FarDistance. The near band runs at full resolution in the main pass; the far bands run at half resolution in technique 2 (Quality 1-2) or in the main pass (Quality 3). The steps per pixel are the cells the view really crosses (at most 96 a band). A column's two drop slots are worked out side by side (float2), from one hash, with a cheap reject before the streak maths, and the walk stops once the coverage is opaque. Per drop, the shader works out analytically where the view passes it:
   - The streak is the drop's path during Exposure, so it is perspective-correct: near drops are longer and wider.
   - Streaks tilt with the wind and never stick to the screen.
   - Drops seen end-on, looking up or down, become dots.
   - Drop sizes 1, 2 and 3.4 mm fall at 4.0, 6.5 and 8.5 m/s.
   - A drop thinner than a pixel is drawn a pixel wide and dimmer.
   - Drops behind the scene are cut softly (12 units). Drops under cover are cut by the ortho map, one read per hit. Points outside the ortho map count as open, which differs from Mode 0.
2. **Lighting.** A drop shows its environment: 4 picture taps above the pixel, mixed with the fog colour by SkyShare, times 1 + EnvironmentLift. On top of that come forward-scattered sun or moon light (Henyey-Greenstein, g 0.75) and the light of the nearest 8 lamps (g 0.7, lit at the streaks' mean position).
3. **Rain haze.** Heavy rain greys out the distance toward the same environment colour, in drifting curtains.
4. **Lens drops** (LensDrops, off by default; third person unless LensDropsFirstPerson). Drops bead up on the lens while the camera is out in the rain, more when looking up. They linger, some slide, and they dry in about 20 s under cover (WetWorld's cover test) or after the rain. Each drop refracts the picture as a small inverted lens.

### Inputs
- Samplers: as Mode 0 (s0 source, s2 depth, s3 ortho map; the bloom sampler is not read in techniques 1-2), plus s5 `TESR_RainFarBuffer` (technique 1).
- Constants (registered in `Rain.cpp`):

  | Name | Contents |
  |---|---|
  | `TESR_RainBand[3]` | cell, period, start, end |
  | `TESR_RainOrigin[3]` | camera in the band lattice, wrapped; repeat |
  | `TESR_RainFall[3]` | fall offsets of the 3 sizes; max steps |
  | `TESR_RainStreak` | streak lengths; width scale |
  | `TESR_RainShape` | shear xy; radians per pixel; drop chance |
  | `TESR_RainLight` | opacity, environment lift, sun glint, lamp glint |
  | `TESR_RainHaze` | amount, extinction, curtains, curtain size |
  | `TESR_RainHazeDrift` | the eye plus the curtains' drift in curtain cells, wrapped at 256 (the noise repeats there); sky share; DebugView |
  | `TESR_RainLens` | wetness, drop size, clock, refraction |
  | `TESR_RainMisc` | lamps used, soft range, far buffer in use (1/0), aspect |
  | `TESR_RainPass` | pixel size scale (2 in the far pass), first band, bands end; set per pass by RenderFrame |
  | `TESR_RainLampPosition[8]`, `TESR_RainLampColor[8]` | lamps |

  Engine constants: `TESR_FogColor`, `TESR_SunDirection`, `TESR_SunColor`, the camera matrices (Depth.hlsl) and `TESR_ShadowCameraToLightTransformOrtho`.
- Wind: `Sky::windSpeed` (0..1) and `Sky::windDirection`, the same pair grass and cloth use. The drift is (sin a, cos a) × speed × Wind units/s, plus Gusts. Rain space is x' = x + shear·z + offset; when the shear changes, the offset takes the change at the eye's height, so the drops round the eye never slide (QA finding 4).

### Settings ([Shaders.Precipitations.System]; all live)
| Key | Default | What it changes |
|---|---|---|
| Mode | 1 | 1 rain system, 0 original layers |
| Quality | 2 | 0: near band only. 1: plus one far band to FarDistance (half resolution). 2: plus two far bands (half resolution). 3: as 2, to 1.5 × FarDistance, far bands at full resolution |
| Intensity | 1.0 | multiplies the weather's rain amount |
| Density | 0.55 | chance per drop slot at full rain |
| Exposure | 0.025 s | streak length = fall speed × this |
| StreakLength | 1.0 | multiplier on the length |
| Width | 1.5 | multiplier on the drops' real width |
| Opacity | 0.45 | strength of a streak over the picture |
| EnvironmentLift | 0.6 | drop brightness above its surroundings |
| SunGlint | 1.0 | sun or moon light scattered forward |
| LampGlint | 1.0 | lamp light scattered (0 skips the lamp loop) |
| SkyShare | 0.35 | fog colour share of the environment |
| FallSpeed | 1.0 | multiplier on the physical fall speeds |
| Wind | 400 units/s | drift at the weather's full wind |
| Gusts | 0.5 | gust strength |
| Haze | 0.35 | maximum haze in heavy rain |
| HazeExtinction | 0.00012 /unit | how fast haze builds |
| Curtains | 0.6 | how much the haze gathers in drifting curtains |
| NearDistance | 600 | the near band's depth, the main cost |
| FarDistance | 5000 | where streaks end |
| LensDrops | 0.0 | lens drops on/strength |
| LensDropsFirstPerson | false | lens drops in first person too |
| LensDropSize | 0.06 | lens drop cell size, as a share of the screen height |
| DebugView | 0 | 1: red near-band coverage, green far-band coverage, blue haze + lens wetness |

The game's own rain particles are still drawn unless `[Main.Main.Precipitations] RemovePrecipitations = true` (restart). With Mode 1 that setting is recommended, or the game's sprites fall on top.

### Passes, cost, guards
- Quality 1-2: a half-resolution far pass, then the full-resolution main pass. Quality 0 and 3: the main pass only. One render target of its own, `TESR_RainFarBuffer` (half size, G16R16F, about 3.7 MB at 2560x1440), made in RegisterTextures like DustMotes' buffer.
- Static size per fxc 10.0.19041: main pass 815 ps_3_0 slots (17 texture), far pass 290 (4 texture). The column step's common path is about 77 slots (it was about 212 before the QA rework). The band loop, the column walk and the lens 3×3 are all `[loop]` with int counters and fixed bounds (the game's D3DX 9 compiler takes minutes over a float-counted loop), and every read inside them is `tex2Dlod`.
- The game compiles the effect once (about 40 s per variant with D3DX 9.43 on this PC, measured with tests/effect_compile) and caches it.
- Per pixel cost:
  - the column walk: up to about 90 steps on sky pixels at Quality 2, fewer where geometry stops the view;
  - one ortho read per drop hit;
  - 4 environment taps where rain or haze shows;
  - the lamp loop once.
- Guards: divisions floored (1e-8 horizontal ray, 1e-4 phase), zero-direction DDA steps clamped to 1e-6, unused lamps get radius 1.
- Vulkan: none for techniques 1-2. ExtRender returns false and the D3D9 passes run, under DXVK too.
- Lens drops: the lens clock wraps at 100 s and every drop runs a whole number of cycles in it (no pop); a sliding drop moves less than a cell (the 3×3 search always finds it). Their cover test is WetWorld's: EffectChain runs it while `LensWantsCover()` even without HudDrops (it still needs WetWorld on).
- Known limits (QA risks 6, 9): the sun glint has no sun-shadow test (rain in the shade of a building still glints toward the sun, bounded by 1 − exp); lamps light a pixel's near drops at their coverage-weighted mean position.
- Not measured in game yet. Static estimate after the rework, Quality 2 at 2560x1440 on an RTX 4070 Ti class GPU: roughly 1–1.5 ms (the QA review estimated about 5.7 ms before it); Mode 0 about 0.2 ms. The timer is "Weather + legacy bloom" in the F10 GPU profile; compare Mode 0 and 1, and Quality 0–3.

### Look: right vs wrong
- **Right:**
  - fine streaks, denser and fainter with distance;
  - they tilt with the wind and keep their place in the world when you turn or walk;
  - none under roofs, none in front of near walls;
  - bright toward the sun and round lamps at night, faint against a bright sky;
  - grey curtains in heavy rain.
- **Wrong:**
  - streaks that move with the camera (lattice origin wrong);
  - rain under roofs (ortho map not requested or test inverted);
  - sparkle or flicker at distance (minimum pixel width broken);
  - horizontal jumps in the rain (shear pivot or origin wrap wrong);
  - a grey film over everything (haze or opacity too high).
