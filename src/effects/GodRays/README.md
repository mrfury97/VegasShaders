# Effect spec: GodRays

light-shaft effect after the sandstorm and before exposure and bloom.

## 1. Purpose

When the sun is up in an exterior and bright sky (or the sun's own disc and glare) sits near or beside the view, soft shafts of light
fan out from the sun's screen position across the picture. The shafts only come from bright sky pixels, never from geometry, and they
are added to the lit scene in linear light, tinted by the sun colour (warmer near the horizon). The player sees a haze of light
beams that is strongest when the sun is low and bright, and gone when the sun is out of view under a dim sky.

## 2. When it runs

Frame order (src/core/ShaderManager.cpp, the frame function around the god rays block at about line 2617):
- It runs after the volumetric light composite (volumetric fog and the sun-lit volumetric light block) and after the sandstorm block.
- It runs before the DLSS Ray Reconstruction point (TAA RayReconstruct), and before the exposure, bloom, lens, LUT and tonemap steps.
- It is called unconditionally from the frame function with: target = the frame's current render target, scene copy = the game's
  RenderedSurface (TheTextureManager->RenderedSurface), SourceBuffer = the source surface, technique index 0, clear = true. The effect
  decides for itself whether to draw. Its GPU timer is "God rays" and its pass timers are "GR sky mask (half)", "GR light mask
  (half)", "GR blur 1 (half)", "GR blur 2 (half)", "GR blur 3 (half)", "GR combine (full)"; a reduction timer is "GR max reduce".

Enable and skip rules (all in the effect's own render entry):
- The effect must be loaded (its .fx compiled) and enabled. Enabled comes from the menu switch of the effect: the key
  [Shaders.GodRays.Status] Enabled (default true), the menu's "Status" kind.
- The per-frame gate (exterior and day only): exterior cell (not interior) AND not underwater AND day light above 0.5. Nothing is drawn
  in interiors, underwater, or when the day light is 0.5 or less (night, and the dark part of dawn and dusk).
- No weather or menu rule is in the GodRays code itself. The menu-open case was not traced beyond the frame call (see section 9).
- Rect frames (RenderScale, TESR_RenderScale / RenderRect active): the dedicated D3D9 path still runs, with its passes limited to the
  rect; the packed (legacy) path draws nothing in a rect frame; the Vulkan path is off in rect frames (section 7).
- Once a skip decision is made the frame is left as it is (input = output), and no frame-chain commit happens for that frame.
- SkipIdle (section 5 / 6): with the sun well outside the view and the sky known to be empty, the effect draws nothing for that frame.

Day light: the game-driven day/night value (0 to 1, a linear ramp across each sunrise and sunset window, plus a smoothstep curve used
for blending, transition curve power [Main.Transitions.Main] TransitionCurvePower, default 1.0).

## 3. Inputs

### 3a. Textures (D3D9 effect, explicit registers; the register is the slot)

| Name | Register | What it holds | Filter / address |
|---|---|---|---|
| TESR_RenderedBuffer | s0 | the current scene at render-target size (the frame's RenderedSurface copy). Read by the sky mask, the light mask and the combine. | linear, clamp |
| TESR_DepthBuffer | s1 | the combined linear depth (CombineDepth): first channel holds view depth divided by the far distance | linear, clamp |
| TESR_SourceBuffer | s2 | the scene copy passed in as the source surface. Read only by the legacy (packed) technique. | linear, clamp |
| TESR_AvgLumaBuffer | s3 | declared, not read by any active code (only in a comment) | linear, clamp |
| NVR_GodRaysBuffer | s5 | the ray buffers (half, or quarter with GodRaysLowRes). Written by the sky mask, light mask, blurs; read by the next pass and the combine. Bound by the C++ per pass. | linear, clamp |
| NVR_GodRaysMax | s6 | the light-mask maximum reduced to 1 x 1 (SkipEmpty), or a reduction step; also the sky-mask maximum for SkipIdle. | point, clamp |

The register numbers are explicit in the D3D9 source, so declaration order is not what binds these. s4 is unused.
On the Vulkan side the same textures are set 1, binding index = slot index (section 7).

### 3b. Constants (registered by the C++ with the shader constant table; the shader reads these names)

| Name | Components (meaning, units) |
|---|---|
| TESR_ReciprocalResolution | x, y: 1 / scene width and 1 / scene height (texels). z: used as an aspect factor for the blur direction (see open questions). w: unused. |
| TESR_GameTime | declared, not read by GodRays. |
| TESR_SunColor | rgb: the sun's colour (linear-light conversion applied by the shader). |
| TESR_GodRaysRay | x: intensity (RayIntensity). y: ray length multiplier (RayLength). z: RayDensity, copied in but read by no shader (dead). w: SunsetGlareFix flag, 1 = on, 0 = off (section 4). |
| TESR_GodRaysRayColor | rgb: custom ray colour (Coloring RayR, RayG, RayB). w: Saturate, 0 to 1, how much of the custom colour replaces the sun colour. |
| TESR_GodRaysData | x: LightShaftPasses, copied in, read by no shader (dead; the sample count is fixed at 10). y: Luminance threshold. z: the per-frame day/night strength (computed each frame, see 3c). w: TimeEnabled flag, 1 = ray attenuation depends on the sun's height. |
| TESR_ViewSpaceLightDir | xyz: the sun's direction in view space. Not necessarily unit length (the code notes 0.707 at 45 degrees). Used unnormalised in the glare and the ray attenuation, and times the far distance for the sun's projected position. w: unused. |
| TESR_SunDirection | xyz: the sun direction used for the sun colour (the sun's disc position while exterior and day, else the light direction). |
| TESR_SunPosition | xyz: the sun disc's unit position in world space. z is negated when not (exterior and day light above 0.5); the effect's own maths uses z as the sun's height above the horizon. w: 1. |
| TESR_SunAmount | x: day time factor (0 at night, 1 by day) used by the sun colour. |
| TESR_SunsetColor | rgb: the sunset colour boost ([Shaders.Sky.Main] SunsetR, SunsetG, SunsetB; red by default). |
| TESR_CameraData | x: near distance, y: far distance (world units). |
| TESR_ProjectionTransform, TESR_InvProjectionTransform | 4 x 4 matrices (the depth and view-space helpers use them). |
| TESR_DebugVar, TESR_ShadowFade | declared, not read by GodRays. |

Shader-local constants the C++ also sets (NVR_*, registered only by the GodRays C++):
- NVR_GodRaysSkip: x = 1 when SkipEmpty is on and the light-mask reduction ran this frame (else 0); y = 1 when SkipIdle is on (so the
  combine returns the input exactly where the rays are zero); z = SkipIdle's limit (0.8 times the light-mask threshold, see 6).
- NVR_GodRaysReduce: xy = 1 / source size of the current reduction step.
- NVR_GodRaysLayout: xy = half-resolution extent / full extent (ratio of the ray buffer to the full target), zw = 1 / ray buffer
  dimensions. Lets one technique serve both the half and the quarter buffers.

### 3c. Per-frame values the C++ computes

- TESR_GodRaysData.z = lerp(NightMultiplier, DayMultiplier, transitionCurve). transitionCurve is the day/night curve from the
  game's day light (section 2). So the strength moves from the night value to the day value across the sunrise and sunset windows.
- Sun height in the effect's maths: 1 minus the sun disc's elevation, clamped at zero from below (so 1 at the horizon, 0 at the
  zenith). "Sunset factor" = that height to the power 8: 1 at the horizon, near 0 with the sun high.

## 4. Settings

All keys read from resource/VegasShaders.defaults.toml (the defaults file writes these sections with a leading underscore
in the headers; the code uses the name without it). Defaults are the toml values, which are what a fresh install gets.

| Section | Key | Type | Default (toml) | What it changes |
|---|---|---|---|---|
| Shaders.GodRays.Status | Enabled | bool | true | the effect's on/off in the menu |
| Shaders.GodRays.Main | DayMultiplier | float | 1.0 | strength by day (multiplies the combined rays). The ImGui look overlay also writes this key (it scales the GodRays value). |
| Shaders.GodRays.Main | NightMultiplier | float | 0.5 | strength at night; applied through the day/night curve (see 3c, section 6) |
| Shaders.GodRays.Main | RayIntensity | float | 1.0 | brightness of the bright sky pixels that cast rays (light-mask gain) |
| Shaders.GodRays.Main | RayLength | float | 1.0 | how far each blur reaches (scales the blur step) |
| Shaders.GodRays.Main | RayDensity | float | 1.0 | DEAD: copied to TESR_GodRaysRay.z, read by no shader |
| Shaders.GodRays.Main | RayVisibility | float | 50.0 | DEAD: read into a C++ member, used nowhere |
| Shaders.GodRays.Main | LightShaftPasses | float | 8 | DEAD: copied to TESR_GodRaysData.x, read by no shader ("Not used" in the defaults file) |
| Shaders.GodRays.Main | Luminance | float | 0.8 | threshold for sky brightness that casts rays; lower = more sky casts rays. Sets the light-mask ramp width (about 15 times this value in luma). |
| Shaders.GodRays.Main | SunGlareEnabled | bool | true | DEAD: read into a C++ member, used nowhere (the defaults file says it scales with the weather sun glare; no code does that) |
| Shaders.GodRays.Main | TimeEnabled | bool | false | when on, ray attenuation grows with the sun's height (0.2 at the horizon to 4 high; blend by the sun's height to the power 4) instead of a constant 1 |
| Shaders.GodRays.Main | SunsetGlareFix | bool | true | when on, the sunset colour boost is applied to the glare near the horizon only (see 6). Off: the boost is applied with the sun high and not at sunset (red tint all day). A missing key counts as on. |
| Shaders.GodRays.Main | SkipEmpty | bool | false | exact speed-up: the light mask's maximum is reduced to 1 x 1; if zero, the blur passes return black at once |
| Shaders.GodRays.Main | SkipIdle | bool | true | speed-up that is not exact in one case, using a check that is one or more frames old: draw nothing while the sun is well out of view and the sky is dim (section 6). Its own comments say release default off; the toml value is true. |
| Shaders.GodRays.Main | ProbeLog | int | 0 | diagnostic, development builds only; any value above 0 forces the D3D9 path (no Vulkan) |
| Shaders.GodRays.Coloring | RayR, RayG, RayB | float | 1.0 each | custom ray colour (linear-light conversion applied) |
| Shaders.GodRays.Coloring | Saturate | float | 0.0 | 0 = use the sun colour; 1 = use only the custom colour |
| Main.Main.ReducedQuality | GodRaysLowRes | bool | false | rays computed at quarter instead of half resolution (4 times fewer pixels; softer rays, shimmer in thin foliage) |
| Main.Main.Performance | NvrDxvkPasses | bool | true | the Vegas Shaders DXVK Vulkan route for the passes (section 7); also used by other effects |
| Main.Main.Quality | MaxQuality | bool | false | Max Quality: the ray buffers are made at full resolution (divisor 1) instead of half (or quarter). Needs restart. |
| Main.Transitions.Main | TransitionCurvePower | float | 1.0 | shape of the day/night curve used for the ray strength |
| Shaders.Sky.Main | SunsetR, SunsetG, SunsetB | float | (sky values) | the sunset colour boost (through TESR_SunsetColor) |

Keys hidden from the menu (2026-11-12): SunGlareEnabled and NvrDxvkPasses are shown only with Developer options (the DevOnly list
in the defaults file). SkipEmpty, SkipIdle, SunsetGlareFix, ProbeLog and LightShaftPasses are Internal (the Internal list there):
never shown, unless `[Main.Menu.Developer] ShowInternal = true` is typed into the settings file. Neither list is an on/off switch
by itself: hidden keys keep their saved values and work as before.

## 5. Passes and outputs

Render targets are created by the effect's texture setup (all A16B16G16R16F, i.e. 16-bit float RGBA):
- NVR_GodRaysBuffer0 and NVR_GodRaysBuffer1: half size of the scene (each side divided by 2, rounded up; 1 with Max Quality).
- NVR_GodRaysBufferLow0 and NVR_GodRaysBufferLow1: quarter size (divided by 4); used when GodRaysLowRes is on (and all four exist).
- Reduction chain NVR_GodRaysMax0 to NVR_GodRaysMax3 (half) and NVR_GodRaysMaxLow0 to NVR_GodRaysMaxLow3 (quarter): each step's
  side is the previous side divided by 8 (rounded up), until 1 x 1. At most 4 steps. A screen too large for 4 steps has no reduction
  (SkipEmpty and SkipIdle then do nothing).

Technique DedicatedGodRays (the main path, 6 passes, in this order; pass 0 is "SkyMask" etc.):

| Pass | Name | Reads | Writes | Size | Job |
|---|---|---|---|---|---|
| 0 | SkyMask | s0 scene, s1 depth | ray buffer 0 | half | the scene in sky pixels plus the sun glare, zero everywhere else (section 6) |
| 1 | LightMask | ray buffer 0 | ray buffer 1 | half | keeps only bright sky (threshold) and scales it up (section 6) |
| (SkipEmpty only) | technique GodRaysMaxReduce, pass Reduce | the light mask (ray buffer 1) | NVR_GodRaysMax chain | 8:1 steps | maximum of the light mask down to 1 x 1 |
| 2 | Blur1 | ray buffer 1 (and NVR_GodRaysMax as a skip) | ray buffer 0 | half | radial blur toward the sun, step 1/10 |
| 3 | Blur2 | ray buffer 0 | ray buffer 1 | half | radial blur, step 1/100 |
| 4 | Blur3 | ray buffer 1 | ray buffer 0 | half | radial blur, step 1/1000; its alpha = ray attenuation (section 6) |
| 5 | Combine | s0 scene, ray buffer 0 | the frame's render target | full | adds the rays to the scene in linear light, re-encodes (section 6) |

Each blur pass takes 10 taps along the line from the pixel to the sun's projected screen position (section 6 for the exact tap rule).

Technique GodRaysMaxReduce, pass "Reduce": 8 x 8 texels per output texel, output = largest of the three colour channels. Run after the
light mask when SkipEmpty is on (and, once, for the SkipIdle check).

Technique GodRaysIdleTest, pass "Test": one pixel inside an occlusion query (see section 6). Discards the pixel unless the sky
maximum is at least the limit (or is NaN); the query counts survivors.

Legacy technique, unnamed (technique index 0; used when the dedicated path cannot run): 6 passes in the same order (sky mask, light
mask, three blurs with the same steps and storeAttenuation 0, 0, 1, combine). It works in the top-left half of the target (packed)
with a copy into the scene copy for each pass. It requires exactly 6 passes. Used only as a fallback.

## 6. Behaviour details

Sky and depth:
- Sky test: a pixel counts as sky when its linear depth (combined depth divided by the far distance) is above 0.9. Only sky pixels
  pass the sky mask; every world pixel is exactly black there, so geometry never casts or receives rays from itself.
- The dedicated sky mask reads the depth first and returns black for world pixels, or when the horizon factor is 0, without reading
  the scene. Must match the legacy result on sky pixels.
- Glare: the sun glare is the dot product of the view-space sun direction and the normalised view direction of the pixel, raised to
  the 180th power, times a glare power (0.1 by day, 8 at the horizon, interpolated by the sunset factor), times the sun colour, added to
  the scene colour in linear light before the sky test.
- Sun colour in the glare: the sun colour (linear) plus the sunset colour boost weighted by the sunset factor. With SunsetGlareFix on
  the boost is weighted by the sunset factor (strong only near the horizon). With it off the boost is weighted by (1 minus the sunset
  factor): strong with the sun high, none at sunset. That is the old red tint.
- Horizon factor: the sky mask and the blur attenuation both use the sun's height. See open question 2: the arithmetic gives the
  horizon factor 1 when the sun is below the horizon, while the comments say black; the attenuation (below) is what stops the rays.

Light mask:
- Five texel-centred taps (the pixel and its four diagonal neighbours, one half-resolution texel away), averaged.
- Threshold: 2 with the sun at the zenith, falling to 0 at the horizon (the sun's height to the power 8 weights the fall). Rays only
  come from pixels whose luma passes the threshold, with a soft ramp of width 15 times Luminance above it.
- The result is multiplied by 100 and the intensity, clamped to 0 to 1 (so the light mask is 0 or close to 1 for bright sky).
- Consequence: at a high sun a dim sky (luma well under 2) produces an exactly black light mask. The sun disc (luma 5 and up)
  saturates it. This is what SkipIdle relies on.

Blur (each of the three passes):
- Centre: the sun's projected screen position (the view-space sun direction times the far distance, projected). It can be outside
  the screen.
- Direction: from the pixel to that centre, with the vertical part scaled by the aspect factor (section 7 and open question 3).
- 10 taps at distances i times the step (step times RayLength), i = 0 to 9. A tap is used only if its distance is at most the distance to
  the centre AND it lies inside the screen. Taps outside the screen are skipped, not clamped.
- The colour result is the sum of the used taps divided by one more than their count (an extra black sample): the rays are slightly
  dimmer near the edges of the shaft where few taps are in range.
- Alpha (only Blur3 keeps it): the ray attenuation, otherwise 1.
- Ray attenuation: the view-space sun direction dotted with the normalised view direction of the pixel, clamped at 0 from below,
  mapped to 0.5 to 1 (0.5 plus half of it), raised to the power 2.5. So it runs from about 0.18 (sun behind the camera) to 1 (sun
  straight ahead). Times the time factor when TimeEnabled is on (0.2 at the horizon to 4 high, blend by the sun's height to the power
  4). Forced to 0 when the sun is at or below the horizon (its height not less than 1 in the shader's units).

Combine:
- Input = scene (linear light) plus rays (ray buffer 0 after Blur3). Where the rays are exactly zero and SkipIdle is on, the combine
  returns the scene as it is (without the linear round trip, which changes about half the texels by one half-float step otherwise).
- Ray colour: the sun colour from the sun direction's elevation (brightened with elevation; sunset colour boost near the horizon
  times the day time factor), or the custom colour; mixed by Saturate. Multiplied by the strength (TESR_GodRaysData.z), then by the
  Blur3 attenuation (the alpha).
- Result: linear scene times (1 + 5 times the rays) plus 0.2 times the rays (rays clamped at 0), re-encoded. Ray values are not
  clamped at 1 here.

SkipEmpty (exact):
- The light mask's maximum at 1 x 1: if it is 0 (or not above 0), every blur pixel is black, so the blurs return black at once.
- Maximum, not average, so a single bright texel is never lost. Ray values after the skip are identical to the full run.

SkipIdle (approximate, guarded):
- Guard (checked each frame): the sun is at least 20 degrees above the horizon (sun height at least 0.342), AND the sun is at least
  12 degrees outside the view frustum (the test uses the true angle to the frustum's side planes), AND the projection is valid.
- Check: every 4th frame, while the guard holds, the sky mask is drawn, reduced to 1 x 1 and tested by an occlusion query around the
  one-pixel GodRaysIdleTest draw (the query is read the next frame or later, never waited on). The answer is "empty" when the count is 0
  and the answer is not more than 8 frames old.
- Limit: 0.8 times the light-mask threshold for the same sun height. A sky maximum below the limit proves the light mask is 0.
- While the guard holds and the sky is known empty, the effect draws nothing for that frame (the input is the output, no chain
  commit). On a check frame that finds the sky empty, the sky mask and the check run but the blurs and combine are left out.
- Any change (guard fails, query not ready, or the device has no occlusion queries) makes the sky unknown and the effect draws normally.
- Not exact in one case: something else brightens the sky within the few frames between two checks (a lightning flash). Rays then start
  up to about 8 frames late.

Frame-chain and buffers:
- The combine writes through the frame chain when the target is owned by it (the chain's output is then committed); otherwise the
  result is copied back into the scene copy. Full target size and format must match the scene copy (checked; mismatch = no rays).
- The D3D9 path clears the depth-stencil binding while it draws (leaves the game's depth-stencil bound when the ReShade depth compatibility
  option asks) and restores it after.
- Scissor: the dedicated D3D9 passes turn scissor off and set their own viewports. The combine restores the caller's scissor state.
- MSAA targets: not supported (no rays).

Errors and fallbacks:
- A failed dedicated pass sets dedicatedRenderFailed (the effect uses the legacy path for the session) and restores the scene copy.
- A failed Vulkan pass sets the Vulkan failure flag for the session (logged once) and the frame is redone with D3D9 (section 7).
- Output with NaN or infinite values: the sky mask's clip step does not discard NaN, so NaN can reach the light mask and the combine. The
  SkipIdle query treats NaN as "bright" (not empty) so a NaN sky never causes a skip. No other NaN guard exists in these passes.

Diagnostics (development builds only, release builds leave them out): ProbeLog 1 reads the maxima back and logs them; 2 also compares the
combine's input and output texel by texel; 3 validates the SkipIdle query against a read-back. Any ProbeLog above 0 forces the D3D9 path.

## 7. Vulkan path

Selection (C++, effects/GodRays.cpp, the effect's render entry):
- The Vulkan route is tried first when: the effect has not failed on Vulkan this session, the dedicated path has not failed, and
  DxvkCompute::PassesOn() is true. PassesOn() = [Main.Main.Performance] NvrDxvkPasses (default true) AND no RenderRect active AND the
  Vegas Shaders DXVK extension is present ("Available"). Test builds with ProbeLog above 0 skip it.
- The Vulkan dedicated path (RenderDedicated with the Vulkan flag) also needs: no scissor on the device, the five pipelines made, the
  two samplers, the depth texture (CombineDepth's combined depth), the scene texture, the constants present, and the target sizes
  as the D3D9 path requires. If any fails the same frame goes to the D3D9 dedicated path, then to the legacy path.
- The NvrDxvkKeepD3D9 setting (a comma list of effect names, default empty) can keep this effect on D3D9 (test tool).
- Full-screen Vulkan passes (the combine) need Vegas Shaders DXVK extension 1.1 or later (the log says so). Without it the combine fails and
  the frame goes to D3D9.
- A failed Vulkan pass: logged as "pass GodRays failed to draw: its D3D9 technique is used from now on". The Vulkan path is then off
  for the session. Nothing of the frame has been committed at that point, so D3D9 draws the whole frame.
- The SkipIdle test (the 1 x 1 occlusion-query draw) stays on D3D9. Its reduction runs on Vulkan.

Pipelines and files (C++ names = SPIR-V file names in src\hlsl\NewVegas\Compute, built into .spv beside them):

| Pipeline name (C++) | Shader file (.frag) | .spv | Samplers | Textures in set 1 binding order |
|---|---|---|---|---|
| GodRaysSkyMask | GodRaysSkyMask.frag | GodRaysSkyMask.spv | linear | 0 scene, 1 combined depth |
| GodRaysLightMask | GodRaysLightMask.frag | GodRaysLightMask.spv | linear | 0 ray buffer 0 (sky mask) |
| GodRaysBlur | GodRaysBlur.frag | GodRaysBlur.spv | linear and point | 0 ray buffer read, 1 maximum (point), 2 combined depth |
| GodRaysCombine | GodRaysCombine.frag | GodRaysCombine.spv | linear | 0 scene, 1 ray buffer 0 (Blur3's output) |
| GodRaysMaxReduce | GodRaysMaxReduce.frag | GodRaysMaxReduce.spv | linear | 0 the step's source |

- GodRays.glsl is the shared include (no .spv of its own): the uniform block, the push block, the sampler macros, the projection and
  sun-height helpers, the smoothstep, and the standard colour helpers used by the shaders.
- Blur pass uses one shader for all three blurs; the step and the storeAttenuation come from the push block (values 1/10, 1/100,
  1/1000; storeAttenuation 0, 0, 1).
- Reduction: one shader, one 8 x 8 step per draw. The point sampler is the one for the maximum read in the blur.
- The skip, ray-size and reduction vectors are the same as the D3D9 technique's.
- UnderwaterGodrays.frag / .spv, and the Underwater effect's own pipelines (effects/Underwater.cpp), are NOT used by GodRays. The
  volumetric light, volumetric fog and volumetric smoke files are NOT used by GodRays.

Uniform block (set 2, binding 0, std140 "Constants"), 320 bytes, filled once per frame by the C++ (one uniform buffer per frame via the
extension's uniform-buffer call), in this order:
1. projection matrix (TESR_ProjectionTransform) and inverse projection matrix (TESR_InvProjectionTransform), each 4 x 4, stored as
   D3DX holds them. The GLSL side multiplies with the matrix in the order that gives the same result as the D3D9 row-vector product
   (the transpose of the stored matrix). A new Vulkan version must do the same.
2. vec4 reciprocal resolution (TESR_ReciprocalResolution)
3. vec4 sun colour (TESR_SunColor)
4. vec4 ray (TESR_GodRaysRay)
5. vec4 ray colour (TESR_GodRaysRayColor)
6. vec4 data (TESR_GodRaysData)
7. vec4 view-space sun direction (TESR_ViewSpaceLightDir)
8. vec4 sun direction (TESR_SunDirection)
9. vec4 sun position (TESR_SunPosition)
10. vec4 sun amount (TESR_SunAmount)
11. vec4 sunset colour (TESR_SunsetColor)
12. vec4 camera data (TESR_CameraData: x near, y far)
13. vec4 layout (NVR_GodRaysLayout)

Push block (48 bytes, push constants, filled per draw by the C++):
- offset 0: vec4 skip (NVR_GodRaysSkip: x SkipEmpty reduction ran, y SkipIdle on, z SkipIdle limit)
- offset 16: vec4 reduce (NVR_GodRaysReduce: xy = 1 / source size of the step)
- offset 32: float blurStep (the blur's step argument)
- offset 36: float storeAttenuation (0 or 1)
- offset 40: uint linear sampler index (LINEAR, CLAMP; the extension fills it in from the heap)
- offset 44: uint point sampler index (POINT, CLAMP); used only by the blur pipeline

Sampler binding: a sampler binding with slot 0 is the linear sampler (push offset 40) and slot 1 the point sampler (push offset 44).
Sampled-image bindings are set 1, binding = slot = the order in the table above. The uniform buffer is set 2, binding 0.

Outputs: the sky mask, light mask, blurs and reduction write each whole target through the extension's draw-to call (one colour
target each, viewport = the whole target). The combine writes the frame through the full-screen call (last pass), so it goes into
the frame chain or the render target like the D3D9 combine.

## 8. Look and cost

Correct:
- Soft shafts that fan out from the sun's screen position across bright sky, fading with distance along each ray, with a visible
  tint from the sun colour (warm near the horizon, with SunsetGlareFix on).
- Nothing in the interior, underwater, at night, or where the sky is dim while the sun is out of view (SkipIdle on, the picture is the
  same as with the effect off).
- No rays across geometry: the edge of an object in front of the sky is sharp, not lit by rays from behind it.
- Day strength near DayMultiplier 1.0; the ramps across sunrise and sunset are smooth.

Typical wrong results:
- Rays over buildings, trees or the ground (sky test failed or the depth texture read in the wrong place).
- Rays at night, indoors, or under water.
- Red haze all day (SunsetGlareFix off).
- Shafts that flicker or shimmer when moving, most with GodRaysLowRes or in thin foliage.
- A hard line where the screen edge crosses a shaft (the screen-bound tap rule).
- A visible pop when SkipIdle switches (a frame that draws rays where the picture was clean), or rays arriving late after a lightning flash.
- A darkening of the whole frame (the combine must add, not multiply), or banding in the rays.
- Rays from a sun below the horizon (the attenuation must be zero there).

Cost class: multi-pass with per-pixel loops. Half-resolution work: sky mask (one scene read, one depth read), light mask (five reads),
three blurs (up to 10 reads each), plus one full-resolution combine. Plus, with SkipEmpty, a 4-step reduction (64 reads per output texel
at the first step). Quarter resolution (GodRaysLowRes) is about 4 times cheaper. The code comments measure the whole effect at about
0.45 ms per frame at game hours 8 to 16, and about 0.04 ms on frames the SkipIdle guard skips.

## 9. Open questions

1. Four keys are read but used by no shader: RayDensity (TESR_GodRaysRay.z), RayVisibility (C++ member only), SunGlareEnabled
   (C++ member only) and LightShaftPasses (TESR_GodRaysData.x). Confirm they are dead, or say what they should do. The defaults file
   says SunGlareEnabled scales with the weather sun glare and LightShaftPasses is "Not used", but the RayDensity text is not clear.
2. The sun-height horizon factor: the code comments say the sky mask is exactly black while the sun is below the horizon, but the
   arithmetic (1 minus the sun's height clamped at zero) gives the factor 1 below the horizon. The rays are then removed only by the
   attenuation (forced to 0 at or below the horizon). Which one is intended? The two give the same rays but not the same sky-mask
   values (the glare still shows in the sky mask below the horizon).
3. The meaning of the z component of TESR_ReciprocalResolution as the blur's aspect factor (the shader divides 1 by it). The register
   code that fills it was not read in this pass, so the exact layout (height over width, or width over height) is not confirmed.
4. TESR_ViewSpaceLightDir is not always unit length (the SkipIdle comment says 0.707 at 45 degrees). The glare power (raised to 180) and
   the ray attenuation use it unnormalised. Check how it is filled before a rewrite assumes unit length.
5. Default of SkipIdle: the defaults file has true, with a comment saying "PLAYTEST ON ... release default off", and the C++ default is
   false (overwritten by the file's value). Which default ships?
6. NightMultiplier: the effect is gated on day light above 0.5, and the strength curve is at least 0.5 to the curve power there. So the
   night value only applies in the upper half of the sunrise and sunset windows, and the strength is near DayMultiplier everywhere the
   effect runs. Confirm that is intended, or whether the gate should be lower.
7. Menu: the frame call is unconditional, and the effect has no menu rule of its own. What the menu and the loading screens show was not
   traced (the frame function's menu branch was not read).
8. D3D9 texture binding for s0 to s3: the effect's shared texture slots are bound by the base effect's constant-table call from the
   registered texture list; that list for GodRays was not traced.
9. The Vegas Shaders DXVK extension version shipped now: the combine needs 1.1; the version in the current extension was not checked.
10. The legacy (packed) path: reachable only after a dedicated-path failure, and the rect-frame case draws nothing there. Its real
    frequency in use was not measured.

## 10. Rewrite status (2026-11-02, commit bcb04003)

The new shader is src/hlsl/NewVegas/Effects/GodRays.fx.hlsl. Techniques: DedicatedGodRays (SkyMask, LightMask, Blur1, Blur2, Blur3,
Combine), GodRaysMaxReduce (Reduce), GodRaysIdleTest (Test), and the unnamed technique (the same six passes, see below). Explicit
slots: TESR_RenderedBuffer s0, TESR_DepthBuffer s1, NVR_GodRaysBuffer s5, NVR_GodRaysMax s6.

Bug and spec items:
- Blur and combine share the same maths as the spec (10 taps, step x RayLength, tap inside the screen and within the centre distance,
  the sum over count plus one; the attenuation in Blur3 alpha; the combine adds linear light with the strength and the attenuation).
- The combine returns the stored scene where the rays are exactly zero and SkipIdle is on (no linear round trip there).
- Ray colour from the sun direction's elevation (TESR_SunDirection.z), by the sunset boost and the day factor, or the custom colour.

Not done, or changed:
- The packed (legacy) path is not reproduced: the unnamed fallback uses the dedicated maths instead, so the top-left packing is gone.
- The aspect factor of the blur direction (TESR_ReciprocalResolution.z) is not applied (the direction is taken in uv units).
- The horizon factor of the sky mask (open question 2) is not applied; the attenuation still removes the rays below the horizon.
- The light mask threshold is 2 x (elevation to the power 8) and the output keeps the averaged colour, scaled by the ramp; the spec does not say whether
  the mask is colour or scalar, so colour was kept.
- Vulkan frags (GodRaysSkyMask, GodRaysLightMask, GodRaysBlur, GodRaysCombine, GodRaysMaxReduce) are not written.
- Rect frames: not handled here (the C++ does not draw the Vulkan path in rect frames anyway).

## Idle targets (2026-11-12)
With `[Main.Main.Performance] TargetReleaseSeconds` above 0 (default 20) the texture manager keeps 1 x 1 stand-ins in place of this effect's render targets while it is off (not made at start-up when it starts off; released that many seconds after it is switched off; made again, zero filled, before the frame draws when it is switched on). Which targets and why that is safe here: the ray buffers and the reduction chains are used only from Render, behind the switch. A new reader or writer of these targets outside the effect's own switch, or a copy of their pointers kept past a frame, breaks this: then answer false in `TargetParkable` (the header). See `core/TextureManager.h`.
