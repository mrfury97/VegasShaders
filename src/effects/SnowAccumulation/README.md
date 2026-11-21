# SnowAccumulation (snow cover on the ground and surfaces during snowfall)

## 1. Purpose
While it snows outdoors, snow builds up on the ground, on the tops of rocks, walls and cars, and stays for a while after the flakes stop, then melts over game hours. It is laid over the existing picture as a white layer with its own lighting: sun, sky ambient, sun-coloured specular and rim, a faint sparkle, and light from nearby lamps. Snow appears only where the surface faces upward, where the sun's shadow map says the sky reaches it, and away from water. Actors (skin and skinned clothes) get no snow. The snow is broken up by noise, a snow normal map and a blue-noise sparkle, so it is not a flat sheet. Two looks exist: Look 0 (the original) and Look 1 (a tester's rewrite with a broader, softer cover, footprints, and impacts on the first-person weapon). An optional Persistent mode keeps the snow spot by spot in a map round the player, instead of one amount for the whole world.

## 2. When it runs
- Enabled by [Shaders.SnowAccumulation.Status] Enabled (default false).
- Per frame it renders when the snow amount is above zero, or when the persistent cover map still holds snow (its own check), and the player is outdoors and not underwater.
- The snow amount is animated in game hours. It rises from 0 to 1 over the setting Increase (default 0.3 game hours) when the game's weather becomes snow. It falls back to 0 over Decrease (default 12 game hours) when snow stops. Both are read when the effect updates its constants.
- While the amount is above zero, the frame is asked for the sun's orthographic shadow map. The map is also requested while the persistent cover map holds snow.
- Weather state: the game's snow weather flag. [Shaders.Snow.Main] ForceSnow (default false) also makes the game count as snowing, so this effect follows it too.
- Frame order (src/core/ShaderManager.cpp): this effect runs earlier than the weather effects. After the cover map's own update (if Persistent is on for wet or snow) and before the ambient-occlusion pass and the composite. It is not part of the weather block (Rain, Snow, particles) and is not part of the final-effect chain order.
- The effect's own gate does not test for menus; whether it draws in a menu is not confirmed here (section 9).
- Sun and light values are read from the frame's constants as the D3D9 constant table holds them that frame.
- Render-rectangle frames: the coverage pass covers only the drawn part of the target (RenderScale below 1). See section 6.

## 3. Inputs
### Textures (sampler slot = the register number in the D3D9 shader; the declaration order is the same)
| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | the current image of the chain: the coverage map while the passes run, the blurred coverage during the snow pass | linear, clamp |
| s1 | TESR_SourceBuffer | the frame before this effect (the scene) | linear, clamp |
| s2 | TESR_DepthBuffer | combined linear depth (view distance over far distance) | linear, clamp |
| s3 | TESR_OrthoMapBuffer | the sun's orthographic shadow depth map | linear, clamp |
| s4 | TESR_NormalsBuffer | the world normals of the frame (encoded) | linear, clamp (the D3D9 sampler: the Vulkan pass samples it linearly) |
| s5 | TESR_PointShadowBuffer | point and sun shadow mask: red = sun shadow (1 lit), green = point-light shadow | linear, clamp |
| s6 | TESR_SnowNormSampler | snow normal map, file Precipitations\snow_NRM.dds | linear mips, wrap |
| s7 | TESR_BlueNoiseSampler | blue-noise texture, file Effects\bluenoise256.dds (used by Look 0 sparkle) | linear mips, wrap |
| s8 | TESR_DepthBufferViewModel | depth of the first-person weapon and hands (their own depth) | linear, clamp |
| s9 | TESR_CoverMapBuffer | the persistent cover map (wet, puddle, snow, trodden snow per spot); used only in Persistent mode | wrap, no mips |
| s10 | TESR_SnowNoiseSampler | snow noise texture, file Effects\Snownoise256.dds (used by Look 1) | linear mips, wrap |
| s11 | TESR_CapturedNormalsBuffer | captured smooth normals (see section 6); point sampled | point, clamp |

The Vulkan pass (section 7) binds the Look 0 textures in set 1 at binding 0 (rendered), 1 (source), 2 (depth), 3 (ortho), 4 (normals), 5 (point shadow), 6 (snow normal), 7 (blue noise), 8 (captured normals).

### Constants (names as the D3D9 effect declares them)
| Name | Components and meaning | Units / notes |
|---|---|---|
| TESR_SnowAccumulationParams | x: ortho sampling radius for coverage (the setting BlurNormDropThreshhold, default 120); y: blur radius multiplier (BlurRadiusMultiplier, default 2.0); z: sun power (SunPower, default 1.0, scales every sun-driven term); w: snow amount 0 to 1 (animated) | x in game units; registered by the effect |
| TESR_SnowAccumulationColor | rgb: snow colour (SnowColorR/G/B, default 0.9, 0.9, 0.8); w: normal smoothing radius in game units (NormalSmoothing, clamped to 0..256, default 32) | registered by the effect |
| TESR_SnowAccumulationExtra | x: 1 when captured normals are compiled in and on, else 0; y: captured-normals debug view (0, 1, 2); z, w: this frame's jitter offset in screen units (zero except with FSR / DLSS low-res effects and FollowJitter on) | registered by the effect; set per frame |
| TESR_SnowData | x: active snowfall, 0 (not snowing) to 1 (full snow), as the Snow effect's animator gives it | from the Snow effect; yz unused by this effect |
| TESR_GameTime | z: the animation clock (used for the impact animation on the first-person weapon) | engine |
| TESR_ShadowCameraToLightTransformOrtho | 4x4, world to sun ortho clip | engine |
| TESR_ShadowData | y: shadow darkness (1 minus y is the darkness of shadowed snow in Look 1) | engine |
| TESR_ShadowFade | x: fade of the sun's shadows toward fully lit when the sun is low; y: with TESR_ShadowScreenSpaceData x, the shadows-on test (Look 0: when both are zero the shadow mask is ignored) | engine |
| TESR_ShadowScreenSpaceData | x: with TESR_ShadowFade y, the shadows-on test (see above) | engine |
| TESR_OrthoData | x: the reach of the ortho shadow map in game units (beyond it no coverage is computed) | engine (set by the exterior shadows effect) |
| TESR_WaterSettings | x: water level height | engine |
| TESR_SunDirection, TESR_SunColor, TESR_SunAmbient | sun direction (toward the sun), sun colour, sun ambient colour | engine |
| TESR_ViewTransform, TESR_InvViewTransform, TESR_ProjectionTransform, TESR_InvProjectionTransform | camera matrices | engine |
| TESR_CameraData | x near, y far distance | engine |
| TESR_CameraPosition | camera world position | engine |
| TESR_ReciprocalResolution, TESR_RenderScale | texel size and the drawn part of the target | engine |
| TESR_DepthConstants | z: the reversed-depth flag (0 normal, non-zero reversed); drives the first-person depth test | engine |
| TESR_ShadowLightPosition[12] | shadow-casting point lights: xyz position, w radius (0 = empty slot) | engine |
| TESR_LightPosition[12] | other point lights: xyz position, w radius (0 = empty) | engine |
| TESR_LightColor[24] | light colours: the first 12 match the shadow-casting lights, the next 12 the others; w is an intensity (rgb * a) | engine |
| TESR_CapturedNormalsData | x 1 when this frame's captured normals exist; y distance tolerance share; z distance tolerance in units; w least cosine for the agreement test | engine (captured normals) |
| TESR_CoverMapFeet[24], TESR_CoverMapFeetData | footprints: 24 print records (centre relative to camera, side and heading) and a bounding circle with the print half-length | from the cover map (Persistent) |
| TESR_CoverMapData, TESR_CoverMapWindow, TESR_CoverMapGlobal, TESR_CoverMapTrail | the cover map's placement, window, open-ground values (wet, puddle, snow), and trail parameters (Persistent) | from the cover map |
| TESR_WorldViewProjectionTransform | declared, not read | |

## 4. Settings
| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| Shaders.SnowAccumulation.Status | Enabled | bool | false | Turns the effect on or off. |
| Shaders.SnowAccumulation.Main | Increase | float (game hours) | 0.3 | How long the snow takes to build up to full cover after snow starts. |
| Shaders.SnowAccumulation.Main | Decrease | float (game hours) | 12.0 | How long the cover takes to melt after snow stops. |
| Shaders.SnowAccumulation.Main | BlurNormDropThreshhold | float (game units) | 120 | Distance around each point over which the sun's shadow map is averaged for coverage. Larger = softer, wider cover edges. Read into the ortho sampling radius. |
| Shaders.SnowAccumulation.Main | BlurRadiusMultiplier | float | 2.0 | Scales the screen-space blur of the coverage map. Larger = softer edges. |
| Shaders.SnowAccumulation.Main | SunPower | float | 1.0 | Multiplies all sun-driven lighting on the snow (0 = flat ambient only). |
| Shaders.SnowAccumulation.Main | SnowColorR, SnowColorG, SnowColorB | float | 0.9, 0.9, 0.8 | Colour of the snow (the blue is lower, so it reads as warm white). |
| Shaders.SnowAccumulation.Main | NormalSmoothing | float (game units, 0 to 256) | 32 | Radius over which the ground's slope is averaged, so tessellated (bumpy) ground does not make snow come and go triangle by triangle. 0 = per triangle. 70 units = 1 metre. Live. |
| Shaders.SnowAccumulation.Main | CapturedNormals | bool | false | Snow takes each surface's own smooth normal, written while the surface draws, instead of the normal rebuilt from depth. Needs a restart (the lit shaders are built with it). |
| Shaders.SnowAccumulation.Main | CapturedNormalsDebugView | int | 0 (0 to 2) | Diagnostic (needs CapturedNormals): 1 shows the normal the snow takes, 2 how much it differs from the depth normal. 0 = the real picture. |
| Shaders.SnowAccumulation.Main | Look | int | 1 (0 or 1) | 0 = the original look; 1 = the rewritten look (broader, softer cover; delayed build-up; footprints; impacts on the first-person weapon). Look 1 always runs as D3D9. |
| Shaders.SnowAccumulation.Main | FollowJitter | bool | true | With FSR / DLSS low-resolution effects, places snow along each jittered pixel's own ray so it does not shimmer. Off = as before (for comparison). |
| Shaders.SnowAccumulation.Main | Persistent | bool | false | Snow is kept spot by spot in the cover map (built up where it reaches, gathers in hollows, melts faster in sun than in shade, footprints tread into it). Off = one amount for the whole world. Read by the cover map (CoverMap.cpp, section Shaders.SnowAccumulation.Main). |
| Shaders.SnowAccumulation.Main | PersistentShade | float | 0.35 (0.02 to 1) | Melt rate in shade (and at night) relative to full sun. Used by the cover map. |
| Shaders.SnowAccumulation.Main | PersistentDrifts | float | 0.5 (0 to 1) | How much more snow gathers in hollows and against walls, and how much less on ridges and edges. Used by the cover map. |
| Shaders.SnowAccumulation.Main | PersistentFootprints | float | 0.7 (0 to 1) | How hard steps tread the snow (compacted, greyer, flatter prints). Used by the cover map. |
| Shaders.SnowAccumulation.Main | PersistentFootprintShape | int | 0 (0 or 1) | 0 = one soft oval per print, 1 = a boot print (sole and heel). Used by the cover map. |
| Shaders.SnowAccumulation.Main | PersistentFootprintSize | float | 10 (2 to 200) | Half the length of one print, in game units. Used by the cover map. |
| Shaders.Snow.Main | ForceSnow | bool | false | Not this effect's setting: makes the game count as snowing (section 2). |
| Shaders.SnowAccumulation.Main | PersistentMapSize | float | 8192 (1024 to 65536) | Width of the map round the player, game units. Used by the cover map. |
| Shaders.SnowAccumulation.Main | PersistentMapResolution | int | 1024 (128 to 4096) | Texels across the map. Used by the cover map. |
| Shaders.SnowAccumulation.Main | PersistentDebugView | int | 0 (0 to 2) | The cover map's test view (1 top-down inset, 2 painted over the world). |


The persistent map's size, resolution and debug view were in the wet-world section ([Shaders.WetWorld.Main]) until 2026-11-21; WetWorld is removed, the keys moved here and settings step 91 carries a saved value over. The cover map's wet half (WetWorld's Persistent) went with it: this effect is the map's only reader.

## 5. Passes and outputs
Each technique below is requested by index. The C++ chooses it (section 7).

| Technique (index) | Passes | Used when |
|---|---|---|
| unnamed (0) | coverage, upscale, blur across, blur down, snow | Look 0, Persistent off |
| PersistentTechnique (1) | the same five, with the persistent snow pass | Look 0, Persistent on |
| LookTechnique (2) | coverage, upscale, blur across (depth tolerance 8500), blur down (depth tolerance 8500), Look 1 snow pass | Look 1, Persistent off |
| LookPersistentTechnique (3) | the same as 2 with the persistent Look 1 snow pass | Look 1, Persistent on |

Pass outputs:
- Coverage pass: writes a value in all three colour channels (the same grey) into the chain's image. The value is 1 where snow may lie (open to the sky, or outside the reach), 0 where the ground is in shade or under cover. It is computed for one pixel in every four in each direction (see section 6): the rest of the target is set to 1.
- Upscale pass (the scale factor is 4): samples the coverage at a quarter of each pixel's position, so the coverage fills the whole target. Pixels beyond the drawn part of a render-rectangle frame are discarded.
- Blur passes (horizontal, then vertical): a depth-aware 12-tap blur of the coverage. A tap counts only where its depth is close to the centre's: the depth tolerance is 3500 game units for techniques 0 and 1 and 8500 for techniques 2 and 3, scaled by the centre's distance as a fraction of the far distance. The blur radius is the radius multiplier times the base offsets. Pixels beyond the ortho reach (TESR_OrthoData x, all techniques) are not blurred: their input is passed through.
- Snow pass: writes the final colour at full frame size, linearised then converted back to display space, with alpha 1. It may write directly to the game target. Pixels with no snow return the scene unchanged.
- All passes must write every pixel of their target (nothing is pre-cleared).

## 6. Behaviour details
### Coverage (where snow may lie)
- Sky and distant pixels: any pixel beyond the ortho reach (TESR_OrthoData x) gets coverage 1 (no coverage check). Look 1 also stops at 3500 game units and fades to fully open between 2500 and 3500.
- Points are placed in the world from the pixel's depth along its own ray (with the FollowJitter offset when set). The point's sun-ortho visibility is averaged over 13 taps (Look 0) or 9 taps (Look 1) spread round it at the radius from Look 0's ortho radius setting. Each tap counts 1 when the point is open to the sun (outside the map counts as open) and 0 when a roof or other occluder is above it.
- The average is sharpened by a smooth step from 0.1 to 0.9 (removes outliers), and faded toward open (1) with distance from 60 percent of the reach to the reach.
- Coverage is computed only at one pixel in four in each direction (the pixel stands for a 4 by 4 block): the upscale fills in between.
- Under a render rectangle (RenderScale below 1 in x or y) the coverage is computed over the drawn part only; beyond it the value is 1.

### Snow surface (Look 0 and the non-persistent snow pass)
- Snow requires the surface to face upward enough: the surface's normal (from depth, or the captured normal, then optionally smoothed) must be above a threshold, and the snow is multiplied by a vertical gate that is 0 below a cosine of about 0.5 and 1 above about 0.8 (on the surface's upward component), and by the coverage. Undersides and walls get none.
- Water: an upward-facing surface whose height lies within a band around the water level (the band is the pixel's depth over far distance times 200 game units, each side) is skipped. Look 1 uses 150 units for this band. Look 1 also skips any surface whose depth-rebuilt normal points clearly downward (its up component at or below about -0.10).
- Actors: pixels the captured normals mark as actors (skin, skinned armour and clothes) keep the scene colour, so snow does not appear on characters standing near a snowy rock.
- Normal smoothing (NormalSmoothing, radius in game units): the normal is averaged with 12 taps on a disc of that world radius around the pixel. Each tap's weight falls to zero as its world position moves away from this pixel's (zero at twice the radius), ramps up as its normal comes within about 60 degrees of this pixel's normal, and is zero off screen (off-screen taps are clamped and do not count). Look 1 uses a fixed 48-unit radius and 8 taps with the same kind of weights.
- Captured normals: where the surface wrote its smooth vertex normal and the distance matches the depth buffer (within the distance tolerance), that normal replaces the depth-rebuilt normal. Where it disagrees by more than about 60 degrees with the rebuilt normal, the rebuilt one is kept. Surfaces the game's own shaders draw have no captured normal. The captured-normal read uses the texel the pixel lies in (point sampling).
- Light on the snow (Look 0): the snow colour is linearised. Ambient is the snow colour times the sun ambient colour raised to the power 2.2 (sign-preserving). Diffuse is the snow colour times the sun-normal dot product, sun colour, a diffuse factor of 0.56 times SunPower, and the sun shadow. Specular is a Blinn-style half-vector term with exponent 20, multiplied by a Fresnel term (power 5 of one minus the eye dot, clamped), and scaled by the specular factor 1.0 times SunPower and the shadow. Fresnel adds a rim of sun colour (factor 0.65 times SunPower). Sparkle is a very sharp term (exponent 1000) from the blue-noise texture, weighted 0.2 and by the shadow.
- Point lights: 12 shadow-casting and 12 other lights. Each adds its colour (times its intensity) times an attenuation of (1 minus s) over (1 plus s), where s is the squared ratio of distance to radius, times a diffuse-plus-specular term (exponent 20), at a power of 0.5 (Look 0). Empty slots (radius 0) add nothing.
- Coverage pattern (Look 0): the coverage noise is three world-anchored noise octaves with feature sizes of 100, 600 and 200 game units, multiplied together and raised to the power 2 minus 2 times the snow amount, then pushed through a smooth step from 0.2 to 0.4. The final mask is coverage times the min of the vertical gate and the ortho value.
- Composite (Look 0): the scene is lerped toward the snow colour by the mask.
- Snow normal map: the snow's own normal map is sampled in world XY at 200 game units per repeat (wrapping), expanded from its 0 to 1 range, and blended into the surface normal (xy added, z multiplied).

### Look 1 (Look = 1)
- Build-up is delayed. Visible cover begins only when the active snowfall (TESR_SnowData x) is essentially full (smooth step from 0.94 to 0.995). The accumulation amount is remapped with a smooth step from 0.25 to 0.82, so visible coverage starts later and rises. Once the amount is high (0.72 to 0.88), the persistence gate keeps snow visible even after the snowfall stops, so accumulated snow lingers and melts separately.
- Shape: the lighting normal is pulled toward an upward-biased snow normal (roundness 1). A broad detail layer samples the same snow normal map at 700 game units per repeat (strength 0.12, rotated), the slope gate is softened (width 0.26), lifted by 0.35 toward up, and given world noise of 0.12. Downward faces keep a gentle underside guard.
- Coverage (Look 1) is two noise octaves (scales 100 and 200 game units) raised to the power 2.2 minus 2 times the delayed accumulation; a smooth step from 0.05 to 0.6; multiplied by a slow world breakup (value noise at 0.003 per unit, between 0.8 and 1.0).
- Eligibility: the slope value (up-facing amount) is pushed by a macro patch noise and mid noise, then gated by a smooth step around a threshold between 0.92 (no accumulation) and 0.40 (full accumulation).
- Edges: the mask is broken up at its edges (a band 0.12 wide), where a keep-probability and fine noise remove snow in patches, so the edge is ragged, not a clean line.
- Shoreline: within 5 game units of the water level the snow is removed with a noise threshold (the keep probability follows the distance) and what survives is semi-transparent (opacity 0.05 to 0.15, noisy). The shoreline's fine noise fades out with distance (1000 to 1800 game units).
- Shadows: the sun shadow factor is lerped between the darkness (1 minus ShadowData y) and 1 by the shadow mask. The snow's own contribution is darkened by that factor, not the whole pixel, so shadows are not darkened twice.
- Sparkle (Look 1): a very sharp term from the snow-noise texture, with a distance fade (800 to 1600 units).
- Light: the same sun terms as Look 0 (diffuse 0.56, specular 1.0, Fresnel 0.65, all times SunPower) and the same point-light sum (power 0.5 per light), but the point-light attenuation is computed without early exits (equivalent result).
- Footprints (Persistent only, Look 1): the cover map's trodden value darkens and flattens the snow inside prints (inner shadow up to 0.32, outer glow 0.12); the trodden snow is greyer and loses some of its surface detail; in the middle of a hard-trodden trail the ground shows through (removal 0.9).
- First-person impacts (Look 1): for the camera's own weapon and hands (the first-person test below), upward surfaces within 120 game units get impact flecks: small cells that spawn, spread and fade on a timer (about 0.35 of a cycle each), lighten the picture toward a near-white colour by up to 0.45, and are gated by the active snowfall (smooth step from 0 to 0.15). They do not use the snow surface.
- First-person test: a pixel is first-person when the view-model depth is closer than the normal depth test (normal depth: view-model depth below 0.9; reversed depth: view-model depth above 0.01). It uses the reversed-depth flag from TESR_DepthConstants z.

### Debug and diagnostics (both looks)
- Captured-normals debug views (section 4): 1 shows the normal the snow uses, 2 the difference from the depth normal; both replace the picture.

### Cross-cutting behaviour
- Render-rectangle frames: the coverage and blur passes use the drawn part of the target (RenderScale). Texture coordinates are mapped into the drawn part before the depth is read; reads are clamped to the drawn part.
- Jitter (FSR / DLSS low-resolution frames, FollowJitter on): the world position of each pixel is taken along the jittered pixel's own ray, using the jitter offset (TESR_SnowAccumulationExtra z, w, in screen units) scaled by the render scale. With FollowJitter off the snow is placed on the unjittered ray (the old look).
- Reversed depth: the combined depth buffer is linear and does not change with reversed depth. The first-person test (Look 1) and the view-model depth do depend on the reversed-depth flag (section 3, TESR_DepthConstants z).
- HDR vs LDR: all inputs and the output are display-referred (the scene after tonemapping). Linear conversion uses the standard sRGB curve for the scene and the snow colour, and a power of 2.2 for the sun ambient in Look 0 (sign-preserving). The output is converted back.
- NaN / inf guards: the normal smoothing and the captured normal decode normalise with a fallback; power terms use saturated or sign-preserving inputs; the sun half-vector normalises. A shadow-map lookup outside the map counts as open (see section 9).
- Things that look wrong if missed: snow on walls or undersides (vertical gate); snow under roofs (ortho test); snow on actors (actor test); snow inside water (water band); triangle-by-triangle snow on bumpy ground (smoothing or captured normals); snow not melting (Decrease); a grey film (tint or sun colour); snow shimmering with FSR (jitter offset); hard unbroken edges (edge breakup); double-darkened shadows (shadow factor applied to the whole pixel).

## 7. Vulkan path
- Shader: src/hlsl/NewVegas/Compute/SnowAccumulation.frag (400 lines). It ports the Look 0, non-persistent passes line for line (coverage, upscale, depth-aware blur, snow pass), with the same constants block, plus the captured-normal and smoothing helpers. It has no Look 1 and no persistent path. The compiled form is SnowAccumulation.spv in the same folder. Pass name requested: "SnowAccumulation".
- Selection (src/core/EffectRecord.cpp, EffectRecord::Render, and this effect's ExtRender): the Vulkan path runs only for technique index 0 (Look 0, Persistent off), and only when:
  - [Main.Main.Performance] NvrDxvkPasses is on;
  - the frame is not a render-rectangle frame;
  - the Vulkan extension is available and the effect's name is not in NvrDxvkKeepD3D9;
  - ClearRenderTarget is false and the source buffer is the frame's source surface;
  - the depth, ortho, normals, point shadow, snow normal and blue noise textures are all present (the captured-normal texture falls back to the normals texture when no captured texture exists).
  Look 1 (techniques 2 and 3) and Persistent (technique 1 and 3) always run as D3D9 passes.
- The C++ side is src/effects/SnowAccumulation.cpp, ExtRender. It builds the constants block by copying the registered constants (as stored), and the textures by name. Failure before the first pass (missing texture, pipeline, constant or block) returns false and D3D9 runs. After the first pass has drawn, a later failure keeps what is on the target.
- Four modes in the Vulkan shader, selected by the push block's mode field, drawn as five passes: mode 0 coverage (the frame's source is copied into the chain's spare image first, as the D3D9 chain does), mode 1 upscale, mode 2 blur (run twice; the offset direction is given by the push block's offset field: (1, 0) across, then (0, 1) down), mode 3 snow (may write the game target directly).
- Textures: set 1, bindings 0 to 8 as listed in section 3 (rendered, source, depth, ortho, normals, point shadow, snow normal, blue noise, captured). Before each pass the rendered texture is rebound (the chain's current image).
- Samplers: linear clamp (index in the push block at offset 8) and linear wrap with mips (offset 12). The sampler indices come from the extension.
- Uniform block: set 2, binding 0, std140, fields in this order: shadowToLight (mat4), projection, invProjection, view, invView (mat4 each), cameraData, cameraPosition, reciprocalResolution, sunDirection, sunColor, sunAmbient, waterSettings, shadowFade, shadowScreenSpaceData, orthoData, snowParams (= TESR_SnowAccumulationParams), snowAccColor (= TESR_SnowAccumulationColor), shadowLightPosition[12], lightPosition[12], lightColor[24], capturedData (= TESR_CapturedNormalsData), snowExtra (= TESR_SnowAccumulationExtra, with x set to 0 when the captured-normal texture is missing). Matrices are stored as the D3D9 matrix (the shader's matrix reads are written to match).
- Push block (24 bytes): offset 0 offsetMask (vec2: the blur direction), 8 sampler (clamp) index, 12 sampler (wrap) index, 16 mode, 20 padding.
- Differences from D3D9 that the Vulkan pass keeps on purpose: the blur clips become direct returns of the input; the captured-normal read is a texel fetch (point), matching the D3D9 point sampler; the smooth step with reversed edges is written out (GLSL leaves it undefined).

## 8. Look and cost
- Correct look (Look 0): a thin, even white cover on flat ground and on the tops of rocks, walls and cars left clear; cover thinner on slopes and under roofs; no snow on water, on actors, or under cover; edges broken by noise, not a clean line; the sun gives a warm highlight on the top surfaces; shadows darken the snow with the ground.
- Correct look (Look 1): cover builds slowly at first and lingers after the snow stops; footprints show as greyed prints with a shadowed inside; a soft, rounded look on bumpy ground; impacts flicker on the first-person weapon while it snows.
- Typical wrong results: snow on walls or on undersides (vertical gate broken); a hard line where coverage ends (blur or edge breakup broken); snow appearing triangle by triangle on tessellated ground (smoothing or captured normals off or broken); snow inside water (water band broken); snow on the player's own body or on actors (actor test broken); snow that keeps shimmering under FSR (jitter offset missing); snow darker than the ground in the sun (sun-driven terms wrong); shadows darkened twice (shadow factor applied to the whole pixel); no melt (Decrease not applied).
- Cost class: multi-pass. Coverage (13 or 9 ortho samples at one pixel in four, so cheap), upscale, two 12-tap depth-aware blur passes at full resolution (skipped beyond reach), and a per-pixel snow pass. The snow pass is the expensive one: Look 0 has 3 noise octaves and 24 point-light evaluations; Look 1 has the same 24-light loop and a second noise set, and the Look 1 pass is much heavier to compile (the authors note minutes of compile time when the loop is unrolled badly). Normal smoothing adds 12 (Look 0) or 8 (Look 1) normal reads per pixel where it is on.

## 9. Open questions
- Sky: the depth value sky pixels hold in the combined depth buffer is not confirmed here. Coverage treats them as beyond reach (coverage 1), but the snow pass's treatment of sky normals (which are not real surfaces) is not confirmed.
- Whether the snow pass draws in menus is not confirmed: the effect's own gate has no menu test.
- Outside the ortho map: both looks count a point outside the shadow map as open (coverage 1), so snow may lie there. Confirm that is intended at the edge of the shadow map.
- The reach of the ortho map (TESR_OrthoData x) is set by the exterior shadows effect and was not traced here.
- The persistent cover map and its footprints are implemented by the cover map code (CoverMap.cpp / CoverMap.h, CoverMap.fx.hlsl), which is not one of the effects in this wave; this spec describes only the inputs the snow reads from it.
- TESR_ShadowScreenSpaceData x is treated as a shadows-enabled flag from its use; its exact meaning in the shadows effect was not read.
- The Look 1 point-light attenuation is written without early exits; the spec treats the result as identical to the Look 0 form.
