# Cinema

Rewritten 2026-11-02 (wave 3, branch claude/rw3-screen, commit 9f541fcc). New shaders: src/hlsl/NewVegas/Effects/Cinema.fx.hlsl (D3D9,
both techniques) and src/hlsl/NewVegas/Compute/Cinema.frag (Vulkan, both techniques via push dither). The C++ is unchanged.
Changes against the old shader: the aspect of zero no longer divides by zero (section 9, item 3, FIXED); the dirt-lens term is removed
because the C++ never sets it, so it could never act (item 2, REMOVED); the sky dither uses the same pixel grid in both paths (item 8,
FIXED). Everything else is as described below.


## 1. Purpose

Cinema gives the picture a film look. It adds black letterbox bars at the top and bottom of the screen for a wide aspect ratio, a faint lens colour split (red and blue pulled slightly apart towards the edges), animated film grain at 24 frames a second, a slight overlay boost of colour and contrast, and a darkening vignette round the edges. The letterbox bars are optional by context: always, only outside conversations, only during conversations, or never. With the default settings (Mode 3) the player sees the grain, colour split, contrast and vignette all the time, and never the bars (Mode 3 was Oblivion's "only during persuasion"; New Vegas has no persuasion menu).

## 2. When it runs

- **Switch:** `[_Shaders.Cinema.Status] Enabled`, default **true**.
- **Frame position:** in the post chain after Sharpening and before ImageAdjust (`ShaderManager.cpp`, the Cinema block). It therefore takes Sharpening's output. It is in the chain's "last effect" candidate list.
- **Two techniques:** technique 0 (`Cinema`, the normal pass) and technique 1 (`CinemaSkyDither`, the same look with the sky dither added in the same pass). `ShaderManager` uses technique 1 only when all of these hold: Cinema and SkyDither both render this frame, ImageAdjust does not render, Bodycam does not render, and the loaded file has the `CinemaSkyDither` technique. Otherwise the SkyDither effect runs as its own pass, and Cinema uses technique 0.
- **Letterbox context (`Mode`):** the bars are shown only in the chosen game situations (see section 4). When the situation is not met the aspect is treated as 1 (no bars) for that frame, but the grain, colour split, overlay and vignette still apply.
- **Interior / exterior / night:** no separate rule. The same values apply everywhere. Letterbox bars follow `Mode`, not the cell type.
- **Menus:** no special rule; it runs whenever the post chain runs and the switch is on.

## 3. Inputs

### Textures (slot order; Vegas Shaders binds samplers by declaration order)

| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | `TESR_RenderedBuffer` | The picture arriving at Cinema (after Sharpening) | Linear, clamp |
| s1 | `TESR_DepthBuffer` | CombineDepth's combined depth: first channel = linear depth divided by far plane distance | Linear, clamp |
| s2 | `TESR_BlueNoiseSampler` | The game file `Effects\bluenoise256.dds`, a 256 x 256 blue-noise texture (from `ResourceName`) | **Point, wrap** (no filtering, repeats) |

On the Vulkan path, if the blue-noise file is not loaded the pass does not run and the D3D9 technique is used instead.

### Constants

| Name | Components and meaning |
|---|---|
| `TESR_GameTime` | z = game time in seconds (used for the grain animation; multiplied by 25 to get the grain's frame count) |
| `TESR_ReciprocalResolution` | xy = 1 divided by picture width and height; z = width / height (the aspect of the picture, used for the letterbox size) |
| `TESR_CinemaData` | x = aspect ratio of the visible picture (1 = no bars); y = vignette radius (darkening starts at distance 1 minus this, see section 6); z = vignette darkness (how much the edge darkens); w = overlay strength (blend of the self-overlay, 0 = none) |
| `TESR_CinemaSettings` | x = dirt-lens opacity (**never set by the C++; always 0 in practice**, see section 9); y = film grain amount (scaled by 0.01 in the shader); z = chromatic aberration strength (in pixels at the edge); w = letterbox depth threshold (game units) |
| `TESR_SkyDitherData` | Used only by technique 1. x = dither step size (one 8-bit step, 1/255, typical); y = 1 for "sky only", 0 for all; z = frame offset for the dither pattern. Supplied from SkyDither's own constant, or zeros if SkyDither is absent. |
| `TESR_CameraData` | y = far plane distance in game units (used by the depth helper for the letterbox depth test) |

The D3D9 side registers `TESR_CinemaData` and `TESR_CinemaSettings` by name. The C++ fills `TESR_CinemaData` from `Constants.Data` and `TESR_CinemaSettings` from `Constants.Settings`.

## 4. Settings

All keys are read in `Cinema.cpp` (`UpdateSettings`).

| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| `[_Shaders.Cinema.Main]` | `AspectRatio` | float | 2.35 | Width/height of the visible picture. The bars are sized so the picture has this ratio. 1 = no bars. Only applies when the context (Mode) allows bars. |
| `[_Shaders.Cinema.Main]` | `Mode` | int | 3 | When bars show. See the table below. |
| `[_Shaders.Cinema.Main]` | `VignetteRadius` | float | 0.6 | Sent as `TESR_CinemaData.y`. Darkening starts at distance (1 minus this value) from the centre (section 6). Larger = darkening starts nearer the centre. This is the reverse of how the toml comment reads (section 9). |
| `[_Shaders.Cinema.Main]` | `VignetteDarkness` | float | 1.2 | Strength of the edge darkening. Sent as `.z`. 0 = none. Above 1 darkens the edges beyond black at the corners. |
| `[_Shaders.Cinema.Main]` | `OverlayStrength` | float | 0.1 | Blend amount of the self-overlay (picture overlaid on itself to raise contrast and colour). Sent as `TESR_CinemaData.w`. 0 = none. |
| `[_Shaders.Cinema.Main]` | `FilmGrainAmount` | float | 0.3 | Grain strength. Sent as `TESR_CinemaSettings.y`; the shader scales it by 0.01. 0 = none. |
| `[_Shaders.Cinema.Main]` | `ChromaticAberration` | float | 1.0 | Colour split at the edges, in pixels. Sent as `TESR_CinemaSettings.z`. 0 = none. |
| `[_Shaders.Cinema.Main]` | `LetterBoxDepth` | float | 0.0 | Depth threshold for the bars: only pixels farther than this (game units) are blacked out. Sent as `TESR_CinemaSettings.w`. 0 = every pixel with any depth is blacked out, so near objects such as a first-person weapon are covered too. Raising it keeps near objects (in the letterbox band) visible. |
| `[_Shaders.Cinema.Status]` | `Enabled` | bool | true | Turns the effect on or off. |

**`Mode` values (as the code reads them):**

| Mode | Bars are shown when |
|---|---|
| 0 | always (when AspectRatio is not 1) |
| 1 | not in a dialogue |
| 2 | only in a dialogue |
| 3 | never (the **default**; it was Oblivion's "only in persuasion", and New Vegas has no persuasion menu) |
| 4 | only in a dialogue (it was "in a dialogue or in persuasion") |

The toml comment lists only 0, 1 and 2. Modes 3 and 4 exist in the code (see section 9). Any other value falls into "always" for the bars in the code's chain of tests, but the spec should state only the five values above.

## 5. Passes and outputs

- **Technique 0 (`Cinema`, unnamed in the file):** one pass, full-screen, writes the picture with letterbox, colour split, grain, overlay and vignette applied. Output alpha is 1. Writes the chain's output target at the picture's resolution, 8-bit LDR.
- **Technique 1 (`CinemaSkyDither`):** the same pass, then the sky dither is added to the result (section 6). One pass.
- **Vulkan path:** the C++ asks for the Vulkan pass named `Cinema` for technique 0 and 1 (the push block selects the dither). Same output target.

## 6. Behaviour details

**Letterbox.** When the aspect is not 1, the bar height is calculated from the picture's aspect (from `TESR_ReciprocalResolution.z`, the render target's own width / height) and the wanted aspect: the bar takes the part of the height that the wanted ratio does not use, split evenly top and bottom. Pixels in the bar band are blacked out (alpha 1) only if their linear depth is greater than `LetterBoxDepth`. The depth read happens only when the aspect is not 1 (a branch), so normal gameplay with bars off pays nothing for it.

- The bars use the render target's aspect, not a render rectangle's. In a RenderScale frame this may be wrong (section 9).

**Colour split (chromatic aberration).** The picture is sampled three times. Red is taken from a position moved slightly towards the screen centre, blue from a position moved slightly away from it; green is not moved. The offset grows with distance from the centre and is `ChromaticAberration` pixels at the edge (the offset is the reciprocal resolution times the strength, scaled by the distance from centre). Nothing happens at the exact centre.

**Film grain.** Two samples of the blue-noise texture are taken at positions moved by the animated time (game time x 25, so the grain moves at 24 frames a second when the game time runs in seconds), and the product of their two channels is turned into a value near 0.5. That value (scaled by grain amount) drives a standard overlay blend of the grain onto the picture, so the grain brightens and darkens without changing the average brightness much. The grain is stronger where the picture is dark (the overlay blend does this). Point filtering and wrap addressing on the noise texture keep the grain sharp and repeating.

**Overlay.** The picture is blended with itself by an overlay blend and mixed back in by the overlay strength. This raises contrast and colour saturation a little.

**Vignette.** The distance from the centre is measured on a scale where the middle of each screen edge is 0.5 and the corners are about 0.71. A smooth falloff (smoothstep) gives a value of 1 up to the distance `1 - VignetteRadius` and falls to 0 at distance 1. The colour is multiplied by `1 - darkness x (1 - falloff)`: no change where the falloff is 1, darkening by up to the darkness setting where it is 0. With darkness above 1 the multiplier goes below zero at the far edge and the colour clips to black there. With the defaults (radius 0.6, darkness 1.2) darkening starts at distance 0.4, so the middle of the screen edges is already partly darkened.

**Sky dither (technique 1 only).** After the colour is computed, pixels whose depth is "sky" are dithered: if the sky-only flag is set and the raw depth (not linear) is below 0.996 (a non-sky pixel), the pixel is left alone. Otherwise the pixel gets a noise value in the range of about one 8-bit step (the x value of the dither constant), built from two interleaved gradient noise samples at the pixel position plus the frame offset, minus 1. The noise removes banding in the sky gradient.

**First-person weapon.** No special handling. The weapon is near, so it falls outside the bar band only if its depth is at or below `LetterBoxDepth`, and it is affected by grain, colour split and vignette like everything else. This is the current behaviour.

**Sky and depth.** Only the letterbox and the sky dither read depth. The sky is treated as depth at the far end (raw value near 1), which is how the dither decides "sky".

**RenderScale / RenderRect frames.** The C++ supplies the full-target size. The Vulkan path is off in render-rectangle frames (the same as Sharpening), so the D3D9 path runs. The letterbox and vignette use the full picture.

**Jitter (TAA).** No handling; Cinema runs on the displayed picture.

**Reversed depth.** Not relevant: depth is Vegas Shaders' linear combined depth, and sky is recognised by the stored value near far.

**HDR vs LDR.** LDR only, 8-bit chain picture. Overlay and grain are applied in display space.

**NaN and inf.** The grain and overlay are bounded. Vignette and letterbox use clamped values. The letterbox's division by the wanted aspect is safe only while the aspect is not 0; the C++ passes the setting through, so an aspect of 0 gives a division by zero (see section 9).

**Things that would look wrong if missed:** bars that cover the first-person weapon when `LetterBoxDepth` is 0 (may be intended); grain that does not animate at 24 fps (the time factor); a colour split that goes the wrong way (red inward, blue outward); sky banding if technique 1's dither is left out; the bars appearing during normal play when `Mode` is 0 (intended only for "always").

## 7. Vulkan path

- **Files:** `src/hlsl/NewVegas/Compute/Cinema.frag` (SPIR-V `Cinema.spv` in the same folder).
- **Selection:** `CinemaEffect::ExtRender` is used only when Vegas Shaders DXVK is loaded, `[Main.Main.Performance] NvrDxvkPasses` is on, and the frame is not a render-rectangle frame. Technique index 0 or 1 is handled; any other index returns "not drawn". If the pass fails, the D3D9 technique runs for the session.
- **Bindings (descriptor set 1):** binding 0 = picture (`t_rendered`), binding 1 = combined depth (`t_depth`), binding 2 = blue-noise texture (`t_noise`).
- **Samplers:** the picture and depth use the linear clamp sampler, taken from the push block at offset 96. The blue-noise texture uses the point wrap sampler, taken from the push block at offset 100.
- **Push constants (112 bytes):**

| Offset | Type | Field |
|---|---|---|
| 0 | vec4 | `TESR_CinemaData` |
| 16 | vec4 | `TESR_CinemaSettings` |
| 32 | vec4 | `TESR_GameTime` |
| 48 | vec4 | `TESR_ReciprocalResolution` |
| 64 | vec4 | `TESR_CameraData` (y far plane distance) |
| 80 | vec4 | `TESR_SkyDitherData` |
| 96 | uint | linear clamp sampler index |
| 100 | uint | point wrap sampler index (noise) |
| 104 | uint | dither: 0 = technique 0, 1 = technique 1 |
| 108 to 111 | uint | padding |

- The Vulkan shader matches the D3D9 shader except for the sky dither pixel position, which uses the framebuffer pixel coordinate rather than the screen position register.

## 8. Look and cost

- **Correct:** a clear film feel. Bars at the top and bottom at the wanted ratio when the chosen context is on, otherwise the full picture. Grain fine and moving slowly at a film rate, not crawling or flickering per frame. A small colour fringe at the edges, red and blue apart, green in the middle. Corners a little darker. No bars over conversation menus unless the chosen Mode says so. The sky shows smooth gradients with no banding.
- **Typical wrong result:** bars at the wrong ratio or on the wrong axis; bars showing in normal play under Mode 3 (should not); a colour fringe that is strong in the centre; grain much too coarse or moving at the frame rate instead of 24 fps; banding in the sky when the sky dither is missing; black corners when the darkness setting is above 1 (may be wanted).
- **Cost class:** single full-screen pass. Three colour reads, two noise reads, one overlay, cheap arithmetic. The depth read happens only when the letterbox is on. Technique 1 adds one depth read. Cheap.

## 9. Open questions

1. **Mode 3 and 4 are missing from the toml.** Since the persuasion tests were removed (2026-11-05), Mode 3 (the default) means "never" and Mode 4 the same as 2; New Vegas never had a persuasion menu, so neither changed on screen. The toml describes only 0, 1 and 2. Decide whether the default should stay 3 and update the comment.
2. **Dirt lens (REMOVED 2026-11-02).** The shader term read TESR_CinemaSettings.x, which the C++ never sets, so it was always zero. The new shader does not read it; the constant stays registered with x unused.
3. **Aspect of zero (FIXED 2026-11-02).** An aspect of zero (or below) now means no bars, instead of dividing by zero. The valid range (the toml gives none) is still not stated.
4. **Letterbox in RenderScale frames.** The bars use the render target's aspect (`TESR_ReciprocalResolution.z`). Confirm that is right when a render rectangle is active.
5. **Ranges not stated** for `VignetteRadius`, `VignetteDarkness`, `OverlayStrength`, `FilmGrainAmount`, `ChromaticAberration`, `LetterBoxDepth`. Only `AspectRatio` and `Mode` have a description. Ranges should be written in the spec once known.
5a. **VignetteRadius direction.** The toml comment says it is "distance to the centre for the start of the Vignette effect", which reads as: larger = darkening starts farther out. The code does the reverse (larger = darkening starts nearer the centre). Decide which is wanted, then fix the code or the comment.
6. **Grain timing.** The shader multiplies game time by 25 to get the grain's frame count. Confirm the intended grain rate (the comment says 24 fps) and whether game time is in seconds.
7. **Sky dither threshold.** The raw depth threshold of 0.996 is a hard number in the source. Confirm it is the intended sky test for the sky dither, especially at the far plane settings the user runs.
8. **Vulkan dither pixel position (FIXED 2026-11-02).** Both paths take the pixel as floor(uv x size), so the grid is the same.
