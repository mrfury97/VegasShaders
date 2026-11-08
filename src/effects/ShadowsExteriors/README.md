# ShadowsExteriors - effect spec


Scope: this spec covers the final apply pass only, the one that multiplies the scene by the sun-shadow term. The sun cascade maps, the contact-shadow march and the point-light passes are producers that write into the buffer this pass reads. They are described only as far as this pass depends on them. Their own specs are separate.

## 1. Purpose

Outdoors, the sun's shadow term is stored in a screen-sized shadow buffer, one value per pixel. This pass reads the finished frame and darkens each pixel by that term, so that surfaces in the sun's shadow lose light and take on the sky colour. Where the player is lit by the sun, the pass leaves the picture as it was. The visible result is the ordinary sunlit-versus-shadowed contrast on land, buildings and actors, with shadowed areas tinted toward the sky colour rather than going flat grey. The strength of the darkening is set by the Darkness setting.

## 2. When it runs

- Frame position: in the post-process chain, after the lighting and before the fog and the later post-process effects. In `ShaderManager.cpp` the call is made in the block that handles the shadow pass, and only when `GameState.isExterior` is true. The call is `Effects.ShadowsExteriors->Render(...)` with technique index 0, into the chain's rendered surface, with the source surface passed through.
- It is skipped when the composite path already applied the shadow (the `composite && shadowApplies` case), when a pair-apply or a shadow-into-AmbientLight path is in use. In those cases the shadow is applied elsewhere and this pass does not run.
- Enable conditions, all of which must hold for the effect to draw:
  - `[_Shaders.ShadowsExteriors.Status] Enabled` is true (the effect's own enable flag, loaded through the generic effect loader). Note: `[_Shaders.ShadowsExteriors.Main] Enabled` is a separate key that controls whether the sun shadow maps are rendered at all. The apply reads it indirectly through the constants (see section 3).
  - The game is in an exterior cell. Interiors use the separate ShadowsInteriors path.
  - `ShouldRender()` passes. This requires either that the SunShadows effect is enabled and loaded, or that the frame is exterior. When SunShadows is not running, the pass is skipped and a one-time log line is written. The reason: without SunShadows the red channel of the shadow buffer holds only the point-light term (about zero outdoors), and the apply would darken the whole image by Darkness.
- Night: no separate on/off switch. At night the sun factor fades (see section 6), and the darkness floor used at night is the Main setting `NightMinDarkness`, which the sun-shadow producer folds into the darkness constant.
- Menus: not specially handled in this pass. The Render call is driven by the frame, and the menu case was not traced further (see section 9).
- Vulkan: when the Vulkan pass path is chosen the effect's `ExtRender` is used instead of the D3D9 technique (see section 7). The Vulkan path is not used while a render rect is active (`DxvkCompute::PassesOn` returns false in rect frames), so in those frames the D3D9 technique runs.

## 3. Inputs

### Textures (D3D9 sampler registers, declaration order)

The fx file declares five samplers. The register numbers below are the ones written in the source and are the slot order Vegas Shaders binds by declaration.

| Slot | Name | Holds | Filter / address |
|---|---|---|---|
| s0 | `TESR_RenderedBuffer` | the frame as the chain has it now (linear light, gamma encoded as the game stores it) | LINEAR, CLAMP |
| s1 | `TESR_DepthBuffer` | linear depth of the frame, normalised by the far plane (multiply by TESR_CameraData.y to get view-space distance) | LINEAR minification, ANISOTROPIC min filter (as declared), CLAMP; the pass only uses explicit level 0 reads |
| s2 | `TESR_PointShadowBuffer` | the shadow buffer. Red: sun shadow factor (0 = in shadow, 1 = lit) from the sun producer; green: point-light term. Two channels in G16R16 (declared format of the buffer, see below) | LINEAR, CLAMP |
| s3 | `TESR_NormalsBuffer` | world-space-agnostic normals in view space, encoded to 0..1 (decode as value times 2 minus 1) | LINEAR, CLAMP |
| s4 | `TESR_SunFarBuffer` | a per-pixel sun factor colour from the SunFar pass (land and cloud shadows far past the cascades). Read only when SunFarApply.x is 1 | POINT, CLAMP, no mips |

The buffer the sampler s2 names is created by the shadow-buffer setup in `ShadowsExterior.cpp` at the frame's width and height, format G16R16 (the `TESR_PointShadowBuffer` init). It is cleared to red 1 / green 0 (opaque white in red) before the producers write into it.

Note on the Vulkan binding: the Vulkan pass uses a different numeric order (rendered = binding 0, sun-far = 1, point shadow = 2, depth = 3, normals = 4, all in set 1). Only the names and meanings matter for a new implementation. The numbers do not have to match the D3D9 registers.

### Constants (names, registers and meanings)

The C++ fills these by name each draw from the shared constant table. The fx file declares them as float4 unless noted. Registers are not fixed in the source for these; the D3D9 path binds them by name.

- `TESR_ReciprocalResolution` (declared, not used by this pass).
- `TESR_WaterSettings` (float4): x = water height in the current cell (world units), y = water depth darkness (not used here), z = 1 when the camera is underwater, else 0. Only z and x are read by this pass, and only for the water-plane exception.
- `TESR_ShadowData` (float4): x = Quality (0 to 4), y = Darkness (the Main `Darkness` setting, after the night floor is applied by the producer), z = near map resolution, w = far map resolution. This pass reads only y. The darkness used in the shader is `max(0, 1 - y)`.
- `TESR_ShadowFade` (float4): x = fade of the sun shadows toward light (0 = full shadow, 1 = fully faded out; at sunrise and sunset the producer raises it; at night with the moon up it is raised toward the moon's visibility), y = 1 when the exterior shadow pass is enabled and on, z = 1 when point-light shadows are on (outdoors this follows UsePointShadowsDay / UsePointShadowsNight), w = the shadow map's far distance (not read here). This pass reads x and z.
- `TESR_SkyColor` (float4): rgb = sky colour (gamma-encoded), w = its alpha, passed through.
- `TESR_SunAmbient` (float4): rgb = the sun ambient colour; only its luminance is read.
- `TESR_SunFarApply` (float4): x = 1 when the SunFar pass wrote its factor this frame, else 0 (read as saturate(x)).
- `TESR_CameraPosition` (float4): xyz = camera position in world units (the fields this pass reads are only in the underwater branch).
- `TESR_CameraData` (float4): y = the far plane distance, used to convert normalised depth to view distance.
- `TESR_ViewTransform` (matrix): the view matrix. Used only in the underwater branch, to turn the normal from view to world space in a way that the test expects.
- `TESR_ProjectionTransform` (matrix): entries [0][0] and [1][1] are used to rebuild a view ray from a pixel coordinate in the underwater branch.

Constants declared in the fx file but not used by this pass's arithmetic: `TESR_ReciprocalResolution`, `TESR_ShadowScreenSpaceData` (declared in the file, not referenced in the shader body). A new implementation does not need them.

### Helper behaviour that the pass uses

- Linearisation of the frame and the sky colour uses a gamma of 2.2 (plain power, not sRGB).
- `luma` is the Rec.709 luminance of an RGB triple (weights 0.2126, 0.7152, 0.0722).

## 4. Settings

Every key read by this pass or by its producer that changes what this pass draws. The C++ reads them under the sections shown.

- `[_Shaders.ShadowsExteriors.Status] Enabled` (bool, default true): whether the effect runs at all.
- `[_Shaders.ShadowsExteriors.Main] Enabled` (bool, default true): sun shadows rendered from shadow maps. When false, the producer does not write the sun term, and the pass is skipped by `ShouldRender` (the SunShadows check above). Visible change: no outdoor sun shadows.
- `[_Shaders.ShadowsExteriors.Main] Darkness` (float, default 0.75): how dark a fully shadowed area becomes. Passed as the `y` of `TESR_ShadowData`. Visible change: deeper or lighter shadows (0 is no darkening; 1 is full black at the shadow floor).
- `[_Shaders.ShadowsExteriors.Main] NightMinDarkness` (float, default 0.35): darkness floor used under the dark moon. Set to 0 to make shadows vanish when there is no moon. Read by the producer into the sun fade and darkness; visible change at night only.
- `[_Shaders.ShadowsExteriors.Main] UsePointShadowsDay` (bool, default false) and `UsePointShadowsNight` (bool, default false): whether point-light (lamp) shadows are part of the shadow buffer's green channel outdoors by day or by night. These set `TESR_ShadowFade.z`. Visible change: lamp shadows outdoors, and a slight whole-screen darkening that the setting's comment mentions.
- `[_Shaders.ShadowsExteriors.Main] Quality` (int 0 to 4, default 4): preset that sets the sun shadow map distance and resolution. Passed as the `x` of `TESR_ShadowData`. This pass does not branch on it.
- `[_Shaders.ShadowsExteriors.Main] SkipWhenFaded` (bool, default true): when on, near sunrise and sunset the producers skip work that nothing reads. This pass still runs, because the fade is 1 and the result is the same.
- `[_Shaders.ShadowsExteriors.Main] ForwardShadows` (bool, default true): whether the sun shadow is also evaluated in the object lighting shaders. Changes which path writes the sun term, not this pass's arithmetic. Compile-time: starting with it false removes the forward path entirely.
- `[_Shaders.ShadowsExteriors.Main] Enabled`, `TwoSidedCasters`, `FitToDepth`, `SunCascade*`, `OverheadSunFix`, `SmoothCharacterShadows`, `CharacterShadowSoftness`, `CharacterShadowReach` and the `CharacterShadowMap*` keys under `[_Shaders.ShadowsExteriors.ShadowMaps]`: these control the sun producers, not this pass. They change the shadow buffer's content only.
- `[_Shaders.ShadowsExteriors.ScreenSpace] Enabled`, `Intensity`, `NearFade`, `Sharp`, `BlurRadius`, `RenderDistance`: contact-shadow producer settings. They change the content of the sun buffer's red channel, not this pass's arithmetic.
- `[_Shaders.ShadowsExteriors.RT] Enabled` (bool, default false) and its keys (`SunAngle`, `Denoise`, `Mode` and others): an optional ray-traced sun term that is combined into the sun buffer by the producer. Not read by this pass.
  Since 2026-11-07 `ShadowDenoiser` (bool, default true, live) denoises that term in the RT helper (one ray over the sun's disc, SunAngle 0 =
  the real 0.53 degrees; a penumbra-sized blur: sharp at contact, softer away) and `Denoise` / `Penumbra` stand aside while it runs; its
  `Denoiser*` keys tune it. docs/next/RT-DENOISER.md. Still not read by this pass.
  Since 2026-11-07 (perf-history, docs/next/RT-PATH-TRACING.md "perf-history") `DenoiserHistory` and `DenoiserStabilize` are frames AT 60
  FPS: with `[Main.Main.RayTracing] DenoiseTimeBased` (default on) the helper gets the same seconds at the game's frame rate
  (RtShadowDenoise::TimeScale; 32 -> ~11 frames at 20 fps). At 60 fps nothing changes.
  Since 2026-11-07 (rtgap-tess, docs/next/RT-DETAIL-GAPS.md section 1) `ContactShadows` (0-1, default 1, live) and `ContactLength`
  (pixels, default 24): a screen-space contact march in the traced sun term for what the depth buffer has and the RT scene lacks
  (tessellated bumps, grass, small props). Mode 1 only; where this pass's own contact shadows (`[_Shaders.ShadowsExteriors.ScreenSpace]`)
  draw, the traced march fades out over their fade-in band (TESR_ShadowScreenSpaceFade, NearFade pushed to the bumps' FadeEnd by
  `DisplaceContactFade`), so a pixel never gets both. With the bumps on, the traced rays also start the bumps' Strength further off
  the surface (`[Main.Main.RayTracing] TessStartSlack`). Not read by this pass.
- `[_Shaders.ShadowsExteriors.Ortho]` and `[_Shaders.ShadowsExteriors.TerrainShadows]`: producer settings only.
- `[_Shaders.ShadowsExteriors.SunSmoothing]`: producer settings only (quantised sun direction and cross-fade).
- Interior settings under `[_Shaders.ShadowsInteriors.*]` do not affect this pass.

## 5. Passes and outputs

- Technique: one technique with no name in the D3D9 fx file, index 0. Requested by the C++ with technique index 0 (any other index is refused). One pass: a vertex shader that passes the full-screen quad through (version 3.0), and a pixel shader named `Shadow` (version 3.0).
- Output: one colour target, the chain's spare or current target in the frame chain. Output format is the chain's format (the frame's render target, not a dedicated buffer). The result is written as RGB with alpha 1.
- The pass writes every pixel of the target. No pre-clear is needed.
- Vulkan: one graphics pass (see section 7), same output target and format.
- No other render target is produced by this effect. It does not write the shadow buffer, the depth buffer or the normals buffer.

## 6. Behaviour details

Each rule below is needed to match the current look.

1. Sun factor. The red channel of the shadow buffer is first faded toward 1 by `TESR_ShadowFade.x`: at full fade the sun term has no effect. Then it is pushed toward the ambient level (the sun term is scaled to the luminance of the sun ambient colour, multiplied by the darkness and by `ShadowFade.z`), so that the brightest areas sit at ambient and the point-light attenuation can add light on top.
2. Point light combination. The green channel (point-light term) is added to the red channel after step 1. The sum is then mapped into the range from the darkness floor to 1, so that a pixel with no sun and no lamp lands exactly on the darkness floor, and a pixel in full sun with no lamp lands on 1.
3. Clamp. The result is saturated to 0..1 before use.
4. Shadow colour. The frame is linearised with gamma 2.2. The sky colour is linearised the same way. Two shadowed candidates are computed: the frame's luminance times the shadow factor times the linear sky colour (a tint), and the linear frame times the shadow factor (a plain darkening). They are blended with a weight of saturate(shadow factor plus 0.5), so that as the shadow factor falls the result moves toward the tint. The blend is then delinearised with gamma 1/2.2. Alpha is set to 1.
5. Water exception. When the camera is underwater (`TESR_WaterSettings.z` equals 1), the pass reads the depth and normal at the pixel and, if the point's world height is within 2 units of the water height (`TESR_WaterSettings.x`) and its world normal points along the negative Z axis with a dot product above 0.999, the frame is returned unchanged. This keeps the water plane from being darkened by shadows.
6. SunFar. Before anything else, the frame is multiplied per channel by the SunFar factor (read through POINT sampling, no mip). The multiply is applied as a lerp from 1, weighted by saturate of `TESR_SunFarApply.x`. When the SunFar pass did not run this frame (x is 0) the factor is 1 and nothing changes.
7. Sky and depth. The pass does not test for sky pixels. Sky pixels get the sun term the producer wrote for them (the producer writes the buffer's sky value; not traced here). A new implementation should treat sky as "no sun shadow" unless the producer says otherwise, and must check the sky behaviour against the producer's spec.
8. First-person weapon. The pass does not special-case the first-person weapon. The weapon is in the rendered frame like any other object, so it is darkened by the same term as the scene at its position. Any exemption lives in the producers.
9. Render-scale and rect frames. The D3D9 technique works on whatever target it is given, so it follows the frame's size. The Vulkan pass is turned off in render-rect frames, so the D3D9 path handles them.
10. Jitter (TAA). None in this pass. The sampling uses the pixel's own coordinate, with no sub-pixel offset.
11. Reversed depth. Not used. The depth is read as linear depth normalised by the far plane, and the pass only uses it in the underwater branch.
12. HDR and LDR. The pass works on the frame as the chain holds it, in gamma-encoded values, and applies gamma 2.2 internally. It makes no HDR-specific decision.
13. NaN and inf guards. The delinearisation takes max(0, x) before the power, so negative values do not become NaN. The shadow factor is saturated before use. No other guard is present in the shader.
14. Frame content that must not darken. The underwater water-plane exception (rule 5), and the case where the sun term is not written (rule "ShouldRender" in section 2). Anything else that the sun term does not cover is darkened by the factor.
15. Look difference from a wrong result. If the sun term is missing, the whole image darkens by Darkness. If the sun term is read as 0 everywhere, the scene looks uniformly dim. If the point-light green term is not added, lamp-lit outdoor areas look dark. If the sky tint is dropped, shadows look flat grey instead of blue-ish.

## 7. Vulkan path

- Vulkan file: `Compute/ShadowsExteriors.frag` (GLSL 4.5, with the shared `Common.glsl` include). The compiled `ShadowsExteriors.spv` sits beside it. There is no variant file for this pass in `Compute/variants.txt`.
- The shader's body mirrors the D3D9 technique line for line. Its helpers reproduce the depth read, the view-ray rebuild and the normal transform.
- Selection: `ShadowsExteriorEffect::ExtRender` builds the pass and runs it through the shared full-screen path. It is used when `[Main.Main.Performance] NvrDxvkPasses` is true, the DXVK extension is available (`DxvkCompute::Available`), and no render rect is active (`DxvkCompute::PassesOn`). `[Main.Main.Performance] NvrDxvkKeepD3D9` naming "ShadowsExteriors" forces the D3D9 technique for the frame. Otherwise the D3D9 technique runs.
- Fallback: if any required constant or texture is missing, `ExtRender` returns false and the D3D9 path runs for that frame. The pipeline is created once (the first call sets a "tried" flag); if creation fails, the pass stays on the D3D9 path.
- Texture bindings (set 1): binding 0 = rendered buffer, 1 = sun-far buffer, 2 = point-shadow buffer, 3 = depth buffer, 4 = normals buffer. All are sampled images with 2D views, level 0.
- Sampler bindings: sampler slot 0 is LINEAR CLAMP (its heap index is written into the push block at byte offset 192), and sampler slot 1 is POINT CLAMP (byte offset 196). Both are used through the `NVR_SAMPLER` indirection in the shader.
- Push block layout (bytes; total 208, the shader reads the first 200):
  - 0: view transform, HLSL column 0 (four floats)
  - 16: view transform column 1
  - 32: view transform column 2
  - 48: view transform column 3
  - 64: camera position
  - 80: x = projection [0][0], y = projection [1][1], z = far plane (CameraData.y), w unused
  - 96: shadow fade (TESR_ShadowFade)
  - 112: shadow data (TESR_ShadowData)
  - 128: water settings (TESR_WaterSettings)
  - 144: sun-far apply (TESR_SunFarApply)
  - 160: sun ambient (TESR_SunAmbient)
  - 176: sky colour (TESR_SkyColor)
  - 192: linear sampler index (uint)
  - 196: point sampler index (uint)
  - 200 to 207: padding (zero)
- Matrix convention: the view transform is stored in the push block by its HLSL columns, taken from the logical element (row i, column j) of the D3D9 matrix. A new Vulkan version must rebuild the same HLSL-style product from those four columns (a matrix times vector in the shader reads as the HLSL `mul(M, v)`).

## 8. Look and cost

- Correct look: outdoor shadows that are clearly darker than the lit ground, with a bluish or sky-coloured tint in the shadow and no hard black, no whole-screen darkening, no change at night beyond the moon floor, and no darkening of the water plane when the camera is underwater.
- Typical wrong results:
  - The whole frame darkens by Darkness with no shadow shape: the sun term is missing (see ShouldRender).
  - Shadows look flat grey: the sky tint is missing or the luminance term is wrong.
  - Lamp-lit outdoor areas stay dark: the green term is not added.
  - The water surface is darkened when underwater: the exception is missing.
  - Sky becomes dark: the producer's sky value is wrong, not this pass.
- Cost class: a single full-screen per-pixel pass with four to six texture reads (rendered, sun-far, point shadow, and in the underwater case depth and normals) and a few multiplies. Cheap. The underwater branch is coherent and costs extra only in that case.

## 9. Open questions

1. The menu case is not traced beyond the frame call. Whether the pass runs under menus is determined by the frame loop, not by this file.
2. Sky handling: the pass has no sky test. Whether sky pixels hold a value that leaves them unchanged was not traced in the sun producer. Needs a check against the producer's spec.
3. The `TESR_ShadowFade.x` night formula (moon visibility) and the sunrise/sunset smooth-step are computed in the producer. The exact curve was read but not re-derived here.
4. The Vulkan-only switch `NvrDxvkKeepD3D9` is checked by the shared full-screen path. Whether the ExtRender function's own early return on technique index is the only gate was not fully traced.
5. The depth sampler is declared ANISOTROPIC for minification but the pass reads only explicit level 0. Whether the anisotropic filter ever matters was not checked.
6. The HLSL's declared constant `TESR_ReciprocalResolution` is not used by this pass; whether the D3D9 binding writes it anyway was not verified.
7. The exact D3D9 register numbers for the constants (as opposed to the samplers) were not determined, because the C++ binds them by name.

## 10. Rewrite status (2026-11-02, commit: rw3 ShadowsExteriors)

The new shader is src/hlsl/NewVegas/Effects/ShadowsExteriors.fx.hlsl: one unnamed technique, pass Shadow, as before. Samplers in
declaration order: RenderedBuffer s0, DepthBuffer s1, PointShadowBuffer s2, NormalsBuffer s3, SunFarBuffer s4 (POINT).

Changes from the old behaviour described above:
- Rule 1 (push toward the ambient level by the sun ambient luminance) is NOT implemented: the spec is vague about the formula. The
  sun term is the sun factor faded by ShadowFade.x, plus the lamp term times ShadowFade.z, mapped from the darkness floor to 1.
- Water exception (rule 5): implemented as the spec states it (world normal along -Z, dot above 0.999). A surface facing up would be
  along +Z, so this may need checking in game; left as written.
- Depth and world position for the water test use reconstructWorldPosition (needs TESR_InvProjectionTransform and the inverse view,
  which the core registers; the spec's list did not include them).

Open: the Vulkan frag (Compute/ShadowsExteriors.frag) is not written; the file is not in this worktree.
