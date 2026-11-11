# Snow (falling snow, screen-space volume)

## 1. Purpose
While it snows outdoors, the picture gets drifting snowflakes laid over the whole view. Flakes are drawn in layers at increasing distance from the camera, so near snow is large and sparse and far snow is fine and dense. A flake only shows where the sun's shadow map says the point is open to the sky, so snow falls through open air and stops at roofs and under overhangs. Each lit flake lightens the scene toward a sun-coloured tint. The rest of the picture is left alone.

## 2. When it runs
- Enabled by the settings section Shaders.Snow.Status, key Enabled (default false). The effect is off out of the box.
- Per frame it renders only when all of these hold: the snow amount (TESR_SnowData x, section 3) is above zero, the player is outdoors (the cell is not an interior), and the camera is not underwater.
- The snow amount is animated, not a switch. When the game's weather becomes snow, the amount rises to 1 over 0.5 game hours. When snow stops it falls back to 0 over 0.2 game hours. The effect keeps drawing while the amount is above zero, so the fade-out tail is visible.
- On the frame snowfall starts, Snow tells the WetWorld effect to fade out its puddles over 0.3 game hours. WetWorld owns that animator; Snow only starts it.
- While the amount is above zero, Snow sets the frame's request for the sun's orthographic shadow map (the map Snow reads), so that map is computed on those frames.
- Weather state: the game's current weather is the snow type. Setting Shaders.Snow.Main ForceSnow (default false) makes the game count as snowing in any weather and clears the rain flag while it does so. It is applied in the per-frame game-state update, before the effects run.
- Frame order (src/core/ShaderManager.cpp): inside the weather block that runs after the TAA pass. Rain renders first, then Snow, then the GPU particle draw (off by default), then the legacy bloom. Snow therefore sees Rain's result. In the frame chain's final-effect order Snow is listed right after Rain.
- Snow's own ShouldRender does not test for menus; whether it ever draws in a menu is not confirmed here (see section 9).
- Final-pass rule: if Snow is the last enabled effect to draw this frame, its single pass writes straight to the game target. Otherwise it writes into the chain's spare target. Either way the output must cover the whole frame.

## 3. Inputs
### Textures (sampler slot = D3D9 declaration order; the C++ binds them in this order)
| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | the scene colour as the frame has it at this point (the rendered copy, not depth) | linear, clamp |
| s1 | TESR_DepthBuffer | the combined depth buffer: linear view distance divided by the far distance, single channel 32-bit float | linear, clamp |
| s2 | TESR_OrthoMapBuffer | the sun's orthographic shadow depth map for exteriors, single channel | linear, clamp |

The Vulkan pass binds the same three in descriptor set 1: binding 0 scene, 1 depth, 2 ortho map.

### Constants
| Name | Register / binding | Components and meaning | Units and notes |
|---|---|---|---|
| TESR_ShadowCameraToLightTransformOrtho | 4x4 matrix (registers 0 to 3) | maps world space into the sun's orthographic shadow clip space | world units in, clip out |
| TESR_ViewTransform | engine constant | camera basis; its first three columns build the world ray | |
| TESR_ProjectionTransform | engine constant | entries [0][0] and [1][1] (horizontal and vertical focal scale) | |
| TESR_CameraData | engine constant | y = far distance | game units |
| TESR_RenderScale | engine constant, via the shared depth include | render-rectangle scale (see section 6) | |
| TESR_GameTime | engine constant | z = animation clock; x, y, w not used by Snow | engine clock units |
| TESR_SnowData | registered by Snow | x = snow amount (0 to 1, animated); y not used; z = fall speed (from Speed); w not used | |
| TESR_SunColor | engine constant | sun colour, RGBA | display-referred |
| TESR_SunAmbient | engine constant | sun ambient colour, RGBA | display-referred |
| TESR_WorldViewProjectionTransform | declared | not read by the shader | |

## 4. Settings
| Section | Key | Type | Range / default | What it changes |
|---|---|---|---|---|
| Shaders.Snow.Status | Enabled | bool | default false | Turns the whole effect on or off. |
| Shaders.Snow.Main | Speed | float | default 1.4; no range in the defaults file | Multiplies how fast the flake layers move down the screen. Higher = faster falling snow. Read live into TESR_SnowData z. |
| Shaders.Snow.Main | ForceSnow | bool | default false | Not a look setting: makes the game treat the weather as snowing outdoors (section 2). |
| Shaders.Snow.Main | Flakes | float | default 0.5 | Dead. The shader does not read it, and the settings menu lists it as not used. Do not give it an effect. |

The snow amount is not a setting. It is driven only by weather and the animator.

## 5. Passes and outputs
- One unnamed technique with one pass: a full-screen vertex pass (position and texture coordinate passthrough) and a pixel pass. The C++ asks for technique index 0; any other index draws nothing.
- Output: one colour per pixel at full frame size, in the frame target's format. Nothing is pre-cleared, so every pixel must be written.
- Pixels with no snow return the input picture, after the linearise and delinearise round trip (identity on values in 0 to 1, up to rounding).
- The Vulkan pass (section 7) writes its colour to the same target.

## 6. Behaviour details
- Colour space: the input is display (gamma-encoded) colour. It is converted to linear with a power of 2.2 (sign-preserving, so negative values do not give NaN), mixed in linear space, and converted back with a power of 1/2.2. Keep the same space, or the snow looks too bright or too dark.
- Ray and layers: the world ray through the pixel comes from the camera basis and projection. The effect runs 7 layers (indices 1 to 7; index 0 is never drawn). Layer i is at 50 x i game units along the ray. A layer only contributes to a pixel whose surface is further away than that distance, so closer geometry hides the layer.
- Sky: a sky pixel must count as further than every layer, so the sky shows snow. The value the depth buffer holds for sky is not confirmed here (section 9).
- Cylinder mapping: the flake grid of each layer lies on a cylinder around the camera, not on the screen. The horizontal coordinate is the ray's azimuth (negated, divided by pi, then mapped into 0 to 1). The vertical coordinate is the height over horizontal distance times 0.1 (the snow's own scale, not the rain's).
- Flake grid: each layer is a grid of cells, each with a pseudo-random brightness (a fixed 3 by 3 mixing constant) and a round drop shape. Each layer is moved horizontally by a sway that depends on the layer and on the vertical position, and vertically by Speed times the animation clock. The sway is a sine of half the clock, with unit amplitude, offset per layer (period in clock units about 4 pi). Different layers therefore drift differently.
- Flake size: the size edge grows with the layer index and with the snow amount (the smaller of the layer index and 4 times the amount, scaled by 0.05, plus 0.005). Far layers have larger flakes, and a small amount gives smaller flakes. The edge must never be zero, because the smooth step breaks at zero.
- Shadow test: for each layer, the sample point along the ray is looked up in the sun's ortho map. The flake counts if the map's stored depth is not closer than the point (bias 0.0001 in map depth), or if the point lies outside the map (outside counts as open). Points under roofs or in a sun shadow get no snow.
- Total: the flake values of all layers are summed, then clamped to 0 to 1. The shadow test multiplies each layer's flake.
- Snow tint: the snow colour is the component-wise maximum of the linearised sun ambient colour and the linearised sun colour, with alpha maximised the same way. The picture is lerped toward this colour by the total. It is a light, sun-coloured tint, not a grey overlay.
- Division guard: the cell's brightness divides by a fractional product that can be zero on some cells. The current shader has no guard there (the rain shader does, with a floor of 1e-4). A new version must guard it. A zero gives infinite or NaN values, shown as black or bright speckles.
- Modulo: the old code uses HLSL fmod meaning (x minus y times the truncated quotient), which keeps the sign of x. A floor-based modulo gives different results for negative coordinates, and the sway can make them negative. Keep the HLSL behaviour.
- Smooth step with reversed edges (the first edge is larger than the second): the result is the standard smooth step of the clamped ratio computed with the reversed edges. GLSL leaves this case undefined, so the ratio and cubic must be written out.
- Render-rectangle frames: when TESR_RenderScale is below 1 in x or y, the texture coordinate is mapped into the drawn part of the target before the ray and depth are worked out (the shared depth include does this). The Vulkan pass is off in those frames (section 7).
- Reversed depth: the depth buffer holds linear view distance over far distance, so reversed depth does not arise. Do not rebuild a projected depth.
- First-person weapon: the shader has no special handling for the weapon; it is treated like any other pixel in the depth buffer.
- Jitter (TAA): the shader has no jitter term; it uses the view as the frame has it.
- HDR vs LDR: input is the display-referred frame after tonemapping; output is the same kind of value. No HDR values are expected.
- NaN/inf guards: the division guard above is the only one needed inside the layer loop. An ortho coordinate outside the map is treated as open sky.
- Things that look wrong if missed: flakes under roofs (shadow test missing); the whole screen snowing at amount zero (ShouldRender gate missing); snow in interiors (interior gate missing); a jump in intensity at the start of snowfall (the animator must ramp); a grey tint (sun colours missing); frozen flakes (animation clock not used).

## 7. Vulkan path
- Shader: src/hlsl/NewVegas/Compute/Snow.frag, a line-for-line port of the D3D9 pixel pass; the header comment records the matrix conventions. Compiled form: Snow.spv in the same folder. The pass is requested under the name "Snow".
- Selection (src/core/EffectRecord.cpp, EffectRecord::Render): the Vulkan pass runs only when all of these hold:
  - the setting [Main.Main.Performance] NvrDxvkPasses is on (src/core/SettingManager.cpp);
  - the frame is not a render-rectangle frame (DxvkCompute::PassesOn);
  - the Vulkan extension is available;
  - the name "Snow" is not in the NvrDxvkKeepD3D9 list (DxvkCompute::KeptOnD3D9);
  - no SM6 build covers this technique (not the case for Snow).
  Otherwise the D3D9 pass runs as in section 5.
- C++ side: src/effects/Snow.cpp, ExtRender. It returns false (so D3D9 runs) if the technique index is not 0, if any of the seven engine constants it reads is missing from the registered constant table, or if the depth or ortho texture is missing.
- Textures: descriptor set 1, binding 0 scene, 1 combined depth, 2 ortho map. The sampler is the linear clamp sampler at binding 3; its index is read from the push block at offset 192.
- Push block (208 bytes; offsets in bytes):
  - 0: the shadow-to-light matrix, four columns (copied from the registered TESR_ShadowCameraToLightTransformOrtho).
  - 64, 80, 96: the view matrix's first three columns, in the order the shader's ray reconstruction uses. The C++ builds them from the registered view transform. The fourth column is not sent.
  - 112: game time (copied from TESR_GameTime).
  - 128: snow data (registered TESR_SnowData: x amount, z speed).
  - 144: sun colour.
  - 160: sun ambient colour.
  - 176: x = projection [0][0], y = projection [1][1], z = far distance (camera data y), w = 0.
  - 192: sampler index (32-bit unsigned), then 12 bytes of padding.
- The Vulkan pass runs the same algorithm: 7 layers, the same shadow test, the same written-out smooth step, the same HLSL-style modulo, and the same colour round trip. Like the D3D9 pass it has no division guard; add one in a new version.
- The Vulkan pass does not run in render-rectangle frames, so it has no rectangle mapping.

## 8. Look and cost
- A correct result: fine, slow-looking flakes falling over open ground and sky; near flakes big and sparse, far flakes small and dense; no snow under roofs or in shadows; a light, sun-coloured tint, never a grey film. Layers drift at slightly different sways and do not all move in step.
- Typical wrong results: a grey or white wash over the screen (tint or shadow test broken); flakes under roofs (shadow test broken); a pattern that does not move (animation clock wrong); black or sparkling speckles (division guard missing); snow indoors (gate broken); flakes that pop on and off with the weather instead of fading (animator not used).
- Cost class: per-pixel loop. One full-screen pass per frame, 7 layers, each with one ortho map lookup and a few arithmetic steps on the cell. The pass is not skipped by pixel content; only the amount and the gates skip the draw.

## 9. Open questions
- The value the combined depth buffer holds for sky pixels is not confirmed in this read. The effect relies on sky counting as further than every layer; confirm against the depth-combine pass.
- Speed has no range in the defaults file, and the shader applies no clamp.
- Whether Snow ever draws in a menu is not confirmed: ShouldRender has no menu test. The frame-level gating in ShaderManager was not traced in full.
- The engine's exact unit for TESR_GameTime z (seconds or game time) comes from the engine constants code, which was not read in this pass.

## 10. Status of the wave 3 rewrite (2026-11-02, branch claude/rw3-weather)
- New D3D9 effect: src/hlsl/NewVegas/Effects/Snow.fx.hlsl. One technique, the same sampler order (rendered, depth, ortho), the same constants. Compiles (fx_2_0, all four defines).
- Fixed: the division by the fractional product (section 6) is now guarded: the divisor is at least 1, so no zero can reach it.
- Chosen here, not in the spec: the flake cell grid (40 by 24 per layer), the flake hash and its 0.5 presence threshold, the round flake shape (smooth step to half its edge), the sway amplitude (0.6 cells), and the fall rate per layer (1 + 0.15 x index).
- The snow-start call that fades WetWorld's puddles is C++ (Snow.cpp) and is unchanged.
- Vulkan path (Compute/Snow.frag, .spv): NOT written yet. Without it the C++ falls back to D3D9.

## 11. World-space snow (2026-11-09, branch claude/snow-worldspace-73a9a9), contributed by macintroll
- Switch: Shaders.Snow.Main WorldSpace (default false). False = everything above, unchanged (technique 0; its compiled pixel shader is byte-identical to before). True = techniques 1 and 2 of the same Snow.fx.hlsl (the file's second part). Snow.cpp `Technique()` picks; EffectChain passes it to Render. Live.
- Technique 1 `SnowWorldSpace`: one pass, `SnowWorld`. The view ray walks a grid of 64-unit cells that scrolls down with time (70 units x Speed a second) and sideways with the volumetric clouds' wind offset (TESR_CloudsWind), up to 40 cells and WorldDistance units. Each occupied cell holds one billboard from a 4x4 flake sheet, facing the camera, with its own size, spin and wobble. A flake shows only where the ortho map says the cell is open to the sky, fades near the camera (12 to 52 units), at the far end (last fifth) and just in front of surfaces. Colour: a blue-white tint, brighter by day, tinted a quarter by the sun + ambient colour.
- Technique 2 `SnowWorldSpaceMarks`: pass `SnowSurfaceHits` first, then the same `SnowWorld`. The first pass draws short-lived flake marks (4 s on, 2 s off, per 12-unit world cell) on surfaces that face up (normal z 0.70 to 0.92), are open to the sky and lie within 1000 units; not on the first-person model, not below the water level. Used when WorldSurfaceMarks is on and the Normals effect is enabled; else technique 1.
- Extra samplers (declaration order = stage): s3 TESR_SnowFlakeBuffer = Textures\Precipitations\fx_weather_snow01.dds (linear, clamp, mips), s4 TESR_NormalsBuffer (point), s5 TESR_DepthBufferViewModel (point). They are bound for technique 0 too (it does not read them). A missing flake sheet = no flakes and no marks, nothing else breaks.
- Extra constants: TESR_SnowWorldData (x flake radius, y density, z max distance, w opacity), TESR_SnowWorldWind (x cloud wind strength, y grid repeat in cells = max(1, floor(8192 x strength + 0.5)), z plant wind strength), both registered by Snow; TESR_CloudsWind (VolumetricClouds), TESR_PlantLeafWind / 2 / 3 (PlantWind), TESR_WaterSettings, TESR_CameraPosition.
- Settings (all Shaders.Snow.Main, live): WorldFlakeSize 1.8 [0.5-3], WorldDensity 1 [0-1], WorldDistance 1200 [200-1300], WorldOpacity 0.8 [0-1], WorldSurfaceMarks true, WorldCloudWind 0.7 [0-2], WorldPlantWind 0 [0-3]. The other look values stay compile-time constants in the shader (cell size, fall speed, fades, tints, the marks' size / lifetime / density, the debug views).
- Limits kept from the shader: 40 cells cover (40 - 3) x 64 / 1.733 = 1366 units, hence WorldDistance's top of 1300. A flake's radius is capped at 2.88 units (4.5 % of the cell) so a billboard never crosses its cell.
- Vulkan path: none. `ExtRender` returns false for any technique but 0, so with the DXVK fork WorldSpace runs as D3D9 passes. A twin would need two fragment shaders, six textures and more constants than the 240-byte push block holds (a uniform buffer).
- The flake sheet was made by the contributor for this project (confirmed 2026-11-09): it may ship, with credit.
- Bench results and cost: docs/next/SNOW-WORLDSPACE-2026-11-09.md.
- Cost cut added here (2026-11-09): a cell is skipped as soon as the ray is further from the flake's resting place than the wobble plus the radius can reach (the reach test in SnowWorld's loop). Same picture; 2.2 ms -> 1.2 ms at 1080p on a GTX 1070.
