# Precipitations (rain: screen-space falling drops and streaks)

## 1. Purpose
While it rains outdoors, the picture gets falling rain streaks laid over the view. Drops are drawn in layers at increasing distance from the camera, so near streaks are larger and far ones finer. Each drop bends the picture behind it (a small refraction) and adds the scene's bloom glow on top. Rain only shows where the sun's shadow map says the point is open to the sky, so it stops under roofs and overhangs. Pixels with no drops are left as they are.

## 2. When it runs
- Enabled by the settings section Shaders.Precipitations.Status, key Enabled (default true).
- Per frame it renders only when the rain amount (TESR_RainData x, section 3) is above zero, the player is outdoors, and the camera is not underwater.
- The rain amount is animated, not a switch. When the weather becomes rainy it rises to 1 over 0.05 game hours. When rain stops it falls to 0 over 0.07 game hours. With [Shaders.WetWorld.Main] RainStateGuard on (default) the animator is driven by the shared rain state (src/core/RainState.h, WetWorld README section 11) every frame, also while this effect is off; other effects read it. The effect keeps drawing while the amount is above zero, so the fade-out tail is visible.
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
- New D3D9 effect: src/hlsl/NewVegas/Effects/Precipitations.fx.hlsl. One technique, the same sampler order (source, rendered, depth, ortho, bloom), the same constant names. Compiles (fx_2_0, all four defines).
- Settings are read as before. SkipHiddenLayers now only saves work: the picture is the same with it on or off (the layers behind the pixel contribute nothing).
- Chosen here, not in the spec: the refraction scale (0.004 of the total times Refraction in screen units), the drop cell grid (64 by 32 per layer), the drop hash and its 0.55 presence threshold, the streak shape, and the drop size as the spec's formula. The spec's division guard does not apply, because this shader has no division by a fractional product.
- Vulkan path (Compute/Precipitations.frag, .spv): NOT written yet. Without it the C++ falls back to D3D9.
