# DepthOfField

Rewritten 2026-11-02 (wave 3, branch `claude/rw3-screen`, commit 05647249) from the spec of the same name. The C++ (`src/effects/DepthOfField/DepthOfField.cpp`,
`.h`) is unchanged. The new shaders: `src/hlsl/NewVegas/Effects/DepthOfField.fx.hlsl` (D3D9), `src/hlsl/NewVegas/Compute/DepthOfField.glsl`
(shared push block and helpers), `DepthOfField.frag` (distant pass), `DepthOfFieldFocus.frag`, `DepthOfFieldMosaic.frag`,
`DepthOfFieldBokeh.frag`, `DepthOfFieldCombine.frag`, `DepthOfFieldCombineBokeh.frag` (Vulkan). The bokeh scatter and resolve
(`DepthOfFieldBokehScatter.comp`, `DepthOfFieldBokehResolve.frag`) are unchanged.
Fixed bugs are marked **FIXED**; behaviour kept on purpose (or not yet changed) is marked **KEPT** or **OPEN**.

## 1. Purpose

The effect blurs parts of the picture that are out of focus, the way a camera lens does. Near objects that are closer than the focus range
blur into the foreground, and far objects beyond the focus range blur into the background. The focus follows what is at the screen centre
(autofocus), so the blur moves as the player looks around. A second, optional blur softens the far distance (terrain LOD and draw
distance), so that distant geometry does not pop. With the optional bokeh mode on, bright lights that are out of focus are drawn as
hard-edged shapes of the lens aperture (a disc or a polygon) instead of soft smudges.

## 2. When it runs

**Master switch.** `[Shaders.DepthOfField.Status] Enabled`. The effect is skipped when its compiled shader is not loaded.

**Which view.** Per frame: vanity camera uses `VanityView`; third person (camera not first person) uses `ThirdPersonView`; otherwise
`FirstPersonView`.

**Per-view switch and mode.** Each view has `Enabled` (autofocus on) and `Mode`: 0 always; 1 off in dialog; 2 only in dialog; 3 always off;
4 (file only) always off. (3 and 4 were Oblivion's persuasion tests; New Vegas has no persuasion menu, and they were removed
2026-11-05 with the same result.)

**Techniques.** Technique 0 (unnamed, autofocus, six passes) when the view's autofocus is on. Technique 1 (`DistantBlurOnly`, one pass) when
autofocus is off and `DistantBlur` is on. Otherwise nothing is drawn.

**Distant-only skip.** When autofocus is off and the camera far plane is at or below `DistantBlurStartRange`, the pass is skipped (interiors).
The C++ does this; **KEPT**.

**Frame order** (ShaderManager): TAA and the particle / haze effects, Coloring, LUT, then DepthOfField, MotionBlur, lens effects, low-HF,
dither buster, SMAA, FXAA, Sharpening, Cinema, ImageAdjust, Bodycam, SkyDither, Debug.

## 3. Inputs

### 3.1 Textures (declaration order = slot order; all linear, clamp, one mip)

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | The chain's current image at each pass (the picture for the distant pass; the blur amounts after pass 1; the packed quadrants after pass 2; the blurred packed image after passes 3 to 5). |
| s1 | TESR_DepthBuffer | Combined linear depth / farZ (one channel). Multiply by TESR_CameraData.y for game units. |
| s2 | TESR_SourceBuffer | The image before this effect (a copy the chain makes). Used by the autofocus technique only; the declaration still makes the copy each frame (see OPEN in section 9). |
| s3 | TESR_AvgLumaBuffer | 1 x 1. Blue = focus distance as a fraction of the hyperfocal distance, read at (0.5, 0.5). |

### 3.2 Constants

| Name | Source | Use |
|---|---|---|
| TESR_ReciprocalResolution | shared | xy: 1 / full target size |
| TESR_DepthOfFieldBlur | effect | x distant switch (zero or not), y start, z end (game units), w base blur radius (texels) |
| TESR_DepthOfFieldData | effect | x focus scale (BlurFallOff x enabled), y radius (texels, plain blur), z diameter range (bokeh threshold), w near cut-off (game units) |
| TESR_DepthOfFieldJitter | effect (JitterFix) | xy depth jitter in UV, subtracted from every depth read paired with the picture's pixel |
| TESR_CameraPosition, TESR_CameraData, TESR_ViewTransform, TESR_ProjectionTransform, TESR_RenderScale | shared (Includes/Depth.hlsl, RenderRect.hlsl) | camera position and height, far Z, view ray and rect helpers |

### 3.3 Vulkan push block (224 bytes, `DofPush` in DepthOfField.cpp, `Compute/DepthOfField.glsl`)

view matrix (0), projection (64), reciprocal resolution (128), blur (144), data (160), camera position (176), camera data (192),
sampler heap index (208), useBokeh (212), JitterFix offset as two floats (216). The bokeh Combine writes half-resolution size at 216; the
shader does not read it.

## 4. Settings

Unchanged, all in `resource/VegasShaders.defaults.toml`. Keys: `Enabled`, `Mode`, `BlurFallOff`, `BaseBlurRadius`, `Radius`,
`DiameterRange`, `NearBlurCutOff`, `DistantBlur`, `DistantBlurStartRange`, `DistantBlurEndRange` per view; `JitterFix` in Status;
Bokeh block: `Bokeh`, `BokehShape`, `BokehThreshold`, `BokehIntensity`, `BokehSize`, `BokehRotation`, `BokehMaxSprites` (clamps in the C++).
The settings meanings are kept exactly as before (see section 9 for the two that read differently from their file comments).

## 5. Passes and outputs

### 5.1 DistantBlurOnly (technique 1, one pass)
Reads the picture (s0) and depth (s1). Depth at the picture's pixel minus the JitterFix offset. Distant amount = a ramp from the start to the
end range (linear, complete at the end), times a height factor (full at or below 5000 game units of world height, none at or above
100000), so high points and the sky are left alone. Where the amount is zero the picture is returned unchanged. Otherwise four diagonal
taps at radius `BaseBlurRadius x (1 + amount)` texels, each weighted by how close its depth is to the centre's (full within 7 %, none
beyond 22 %); the blur is mixed with the centre by the amount.

### 5.2 Autofocus (technique 0, six passes, each reads the chain's current image)
1. **Focus.** Depth at the picture's pixel (JitterFix). H = max(BlurFallOff x 1000, 0.0001); f = AvgLuma.b x H; near limit and far limit
   from the standard hyperfocal form (guarded, section 6.2). Output: red = near amount, green = far amount (the distant amount is added to
   green), blue = alpha = 1.
2. **Mosaic.** Quadrants of the full target, each holding the whole picture at half resolution (a 2 x 2 box of full-resolution pixels):
   top left and top right = the sharp image (SourceBuffer); bottom left and bottom right = the blur amounts (red near, green far).
3. **Bokeh blur (bokeh select).** Top quadrants blurred with their amount's radius (top left: near amount from bottom left, top right: far
   amount from bottom right); bottom quadrants unchanged. 12 disc taps (30 degree steps, alternating outer and inner ring), radius
   `BaseBlurRadius x amount` texels, taps outside the pixel's quadrant ignored. Output: the brightest valid tap when its luma is above
   `DiameterRange`, else the average of the valid taps.
4. **Bokeh blur (bokeh select)**, the same as pass 3 on its output.
5. **Plain blur.** The same taps with the plain average and radius `Radius x amount`.
6. **Combine.** Per pixel: the sharp image, mixed toward the top-left (near) blur by the near amount, then toward the top-right (far) blur
   by the far amount. Writes the frame (directly into the game target when this is the last effect).

### 5.3 Bokeh sprites (Vulkan only; fork-only)
Unchanged scatter and resolve (`DepthOfFieldBokehScatter.comp`, `DepthOfFieldBokehResolve.frag`). The bokeh Combine adds the resolved sprites
in linear light: `1 - (1 - colour) x exp(-sprites)`, clamped to white, then back to display space.

## 6. Behaviour details

### 6.1 Depth and sky
Depth is linear (fraction of far Z). Sky pixels are at the far plane (1.0). **FIXED (2026-11-02 rewrite):** the autofocus far weight is
zero on sky pixels (depth at or above 0.9999), so the sky is not blurred by the focus; the distance blur still follows the height rule.
The height rule is kept: world height is camera Z plus the view ray's Z times the depth.

### 6.2 Focus geometry (guarded)
- near limit = f (H - 0.01) / max(H + f - 0.02, 0.0001), itself at least 0.0001 (**FIXED**: no divide by zero).
- far limit = f (H - 0.01) / (H - f) when H - f exceeds 0.0001; otherwise 10^6 (focus at or beyond the hyperfocal distance has no far limit;
  **FIXED**: the old form gave a negative or infinite limit there). At least 0.0001.
- near weight = saturate(1 - depth / near limit) x near cut-off attenuation; far weight = saturate((depth - far limit) / far limit) x 0.8,
  then plus the distance amount, saturated.

### 6.3 Near cut-off attenuation (first and third person)
Factor = 0.5 x (1 - smoothstep(0, c/2, d)) + smoothstep(c/2, c, d), with c = NearBlurCutOff. It is 0.5 at the camera, 0 at c/2, 1 from c on.
**FIXED**: c <= 0 (vanity default) returns 1 instead of dividing by zero. The dip near the camera is **KEPT** (see section 9).

### 6.4 Blur radii
Bokeh passes use `BaseBlurRadius` (TESR_DepthOfFieldBlur.w); the plain pass uses `Radius` (TESR_DepthOfFieldData.y). Both are scaled by the
amount of the pixel's quadrant. **KEPT**: this is what the code did before; the settings file comments say the reverse (section 9).

### 6.5 Tap guards
Every tap that is read with a rect (RenderScale) frame is clamped to the drawn part (`clampToRectIfOn`), and the autofocus taps stay inside
their quadrant, half a texel from the edge. Added in the rewrite: the rect clamp keeps reads inside the drawn part in render-rectangle frames, and the
quadrant bounds stop one quadrant's taps reading another. Distant-pass weight division uses a floor of 0.0001.

### 6.6 Bokeh sprite detection (unchanged, compute)
Half-resolution candidates above `BokehThreshold` that stand out from their surroundings; radius below one pixel dropped; weight and energy as
in the scatter file. Depth stored with a sprite is the half-resolution depth sample.

### 6.7 Bokeh splat and combine (unchanged, compute and Combine)
Shape coverage with an anti-aliased edge, energy kept over the shape's area, fixed-point adds capped at 4.0 per pixel. Far-field sprites hidden
by a nearer in-focus pixel. Combine: section 5.3.

### 6.8 JitterFix
Every depth read paired with the picture's pixel subtracts `TESR_DepthOfFieldJitter` (focus pass, distant pass and its taps, Vulkan copies).
Zero when JitterFix is off. The bokeh scatter does not (see section 9).

## 7. Vulkan path

Files in `src/hlsl/NewVegas/Compute`, all compiled with `tools/nvr-dxvk/compile-compute.sh`:

| Pass | Source | Pipeline |
|---|---|---|
| DepthOfField (technique 1) | DepthOfField.frag (+ DepthOfField.glsl) | 2 textures, sampler |
| DepthOfFieldFocus | DepthOfFieldFocus.frag | depth, AvgLuma, sampler |
| DepthOfFieldMosaic | DepthOfFieldMosaic.frag | blur amounts / chain, source, sampler |
| DepthOfFieldBokeh (passes 3 to 5, push useBokeh) | DepthOfFieldBokeh.frag | chain, sampler |
| DepthOfFieldCombine | DepthOfFieldCombine.frag | chain, source, sampler |
| DepthOfFieldCombineBokeh | DepthOfFieldCombineBokeh.frag | chain, source, sprites, sampler |
| DepthOfFieldBokehScatter / Resolve | unchanged | compute / graphics |

The Vulkan passes are native-frame only (no render-rectangle maths). Each file is the D3D9 pass line for line. Compile check: all six
`.spv` build and pass spirv-val.

## 8. Look and cost

**Correct look.** Distant geometry soft and even with no flicker along roofs and skylines (JitterFix on). The first-person gun and body stay
sharp (NearBlurCutOff). Focus follows the centre without pumping. Sky not blurred by the focus. Bokeh on: out-of-focus lights become
aperture shapes.

**Wrong results to watch for.** Blocks along roofs with TAA, FSR or DLSS at native size and JitterFix off. A dark halo around thin objects over
the sky (the distant weight prevents it). Quadrant seams in the autofocus look (the taps now stay inside their quadrant).

**Cost.** Distant pass: one sample on most pixels. Autofocus: six full-screen passes at 12 taps per blur pass, plus the bokeh dispatches when on.
The D3D9 chain also copies the source image each frame while the effect declares TESR_SourceBuffer (OPEN, section 9).

## 9. Open questions

1. **Radius and BaseBlurRadius (KEPT).** The settings file says `Radius` is the bokeh radius and `BaseBlurRadius` the overall strength. The
   code (and this rewrite) uses `BaseBlurRadius` for the bokeh passes and `Radius` for the plain pass. Which one is meant? Changing it changes
   what the setting does, so it was not changed.
2. **Near cut-off dip (KEPT).** The attenuation is 0.5 at the camera, the file comment says it is meant to reveal the gun. The dip near the
   camera is kept as written; confirm it is intended.
3. **Bokeh scatter ignores JitterFix (OPEN).** The sprite detection reads the sharp image at the picture's pixel, but the depth for the sprite
   and the far occlusion test is read at the half-resolution pixel without the offset. Fixing it needs one more field in the scatter's push
   block (C++ change), which this rewrite did not make.
4. **Bokeh passes select even when Bokeh is off (KEPT).** The C++ always runs passes 3 and 4 with the bokeh select (push 1, 1, 0), in both
   paths. "Bokeh off = exactly the old DoF" is therefore not literally true for the brightest-tap choice; left as the C++ and the Vulkan path
   already do.
5. **Mode 3 and 4 (KEPT).** Always off in New Vegas, as before.
6. **Source copy (OPEN).** The effect declares TESR_SourceBuffer for the whole file, so the chain copies the picture before the distant pass
   too, although that pass does not read it. Removing it needs a separate effect file or a C++ change.
7. **Slider ranges.** Not checked; the C++ does not clamp the view values.
8. **Sprite drop order.** Atomic counter, so which sprites are dropped at the budget is not deterministic (accepted by design).
