# ShadowsInteriors

REWRITTEN 2026-11-02 (branch claude/rw3-lights): the shaders are new, written from this spec (Effects/ShadowsInteriors.fx.hlsl; Compute/ShadowsInteriorsBlur.frag and ShadowsInteriorsApply.frag). The C++ and the settings are unchanged. Changes from the old file are marked below.

includes it uses), the C++ in effects/ShadowsInteriors.cpp and .h, the Vulkan passes in Compute/ShadowsInteriorsBlur.frag and
Compute/ShadowsInteriorsApply.frag, and the settings in resource/VegasShaders.defaults.toml.

## 1. Purpose

Indoors, this effect darkens the parts of the scene that the lamp shadow mask marks as shadowed, so that objects and walls
standing between a lamp and a surface show a soft shadow on it. It takes the shadow term that the point-light shadow passes
have written into the shadow buffer, softens it with a depth-aware blur, and multiplies the scene's brightness down where the
term says "shadow". Where the surface is far from the camera it fades back to fully lit, and the darkening keeps the colour
and saturation of the surface rather than turning it grey. Outdoors this effect does not run; the exterior sun shadows do that job.

## 2. When it runs

- Enabled by the effect's own status record: `[_Shaders.ShadowsInteriors.Status]`, key `Enabled`, default `true`. The
  defaults file's comment reads "Post process pointlights shadows in exteriors", but the effect is only applied indoors (see
  below); treat the comment as stale.
- Interiors only. The shadow-apply stage of the frame calls it only when the game is in an interior cell. In exteriors the
  shadow-apply stage calls the exterior shadows instead.
- Frame position: this is the shadow-apply stage of the frame, after the lamp shadow mask has been built. Its position
  relative to the ambient occlusion apply is set by the comments in the shadow-apply block of src/core/ShaderManager.cpp
  (the block that begins with the InteriorShadowFold comment); the exact order of the AO apply was not re-traced. It is skipped and its work is taken over by the fog composite
  only when the InteriorShadowFold setting is on and every fold condition holds (default: the setting is off, so this pass
  runs as its own pass). Several effects that read the scene (SnowAccumulation, WetWorld, BloodFlow, Flashlight, Specular,
  Underwater, BounceLight, Reflections, and AmbientLight when it runs) must see the scene with the darkening already applied,
  so when any of them is going to render this frame the fold is not used.
- Darkening mode depends on the per-object forward lamp shadows (`[Shaders.ShadowsInteriors.Forward]`):
  - Forward off: the darkening is drawn in this pass, blurred (the normal path).
  - Forward on with `KeepDarkening` on: the darkening is drawn in this pass, unblurred when `DarkeningBlur` is off
    ("skip blur" mode: the two blur passes are skipped and the apply reads the mask as it is).
  - Forward on with `KeepDarkening` off: this pass is not drawn at all.
- Menus: the effect has no menu rule of its own. The game-menu cost overlay counts it for interior cells. Whether it runs
  while a menu is open was not traced (see section 9).
- Render-scale rect frames: the D3D9 path restricts its drawing to the drawn part of the frame (see section 6). The Vulkan path
  is switched off whenever a render-scale rect is active.

## 3. Inputs

### Textures (D3D9 sampler slots, bound in declaration order)

| Slot | Name | Contents | Filter / address |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | The current scene image (HDR frame) that the darkening is applied to. In the dedicated path it is read untouched, because the blurs never write it. | LINEAR, CLAMP |
| s1 | TESR_SourceBuffer | A copy of the scene taken by the frame chain for the generic path. Declared, sampled by the generic blur passes only, not by the apply. | LINEAR, CLAMP |
| s2 | TESR_DepthBuffer | Depth of the scene, stored as depth divided by the far distance (0 to 1). Multiply by the far distance to get game units. | LINEAR, CLAMP |
| s3 | TESR_PointShadowBuffer | The lamp/sun shadow mask: a two-channel 16-bit-per-channel buffer (G16R16). Only the red channel is read. It is the same texture the exterior shadow effect owns as its shadow pass target. | LINEAR, CLAMP |
| s4 | NVR_InteriorShadowIn | Dedicated path only. The blurred shadow term, bound by the C++ (the mask after the two blurs, or the raw mask in skip-blur mode). Red channel read. | LINEAR, CLAMP, no mip |

Sampler behaviour: all sampled textures are read with linear filtering and clamp addressing. The one-level textures make
mip filtering irrelevant. The generic technique reads depth with mip level 0 explicitly in the blur.

### Constants (D3D9 names; registers are assigned by the compiler and are not fixed)

| Name | Meaning |
|---|---|
| TESR_WorldTransform | Declared; not read by this effect. |
| TESR_ShadowData | Four floats. Component y = the interior shadow darkness (`Darkness` from `[_Shaders.ShadowsInteriors.Main]`, passed through the shared shadow-constants block). Components x, z, w are not read by the apply. |
| TESR_ShadowFade | Four floats. y = 1 when interior shadows are enabled, 0 when disabled ("shadows are disabled" means the apply has no darkening at all). w = the draw distance of the shadows in game units (MAXDISTANCE); the blur writes black beyond it and the apply fades the shadow to fully lit between 300 units and this distance. x and z are not read here. |
| TESR_ReciprocalResolution | Texel size: 1 / width in x, 1 / height in y. The blur steps are multiples of it. |
| TESR_CameraData (read by the Vulkan path as the camera block) | y = the far distance of the camera, in game units. Depth in s2 is multiplied by it. |
| TESR_ViewTransform / TESR_ProjectionTransform | The view and projection matrices as the renderer stores them. Used only to turn a screen position into a world-space view direction for the distance fade. The D3D9 path reads the same values through the engine's own constant names. |
| farZ | Used by the D3D9 source as the far distance (same value as TESR_CameraData.y). |

### Dedicated-path inputs supplied by the C++

- The two scratch targets owned by the sun-shadow effect (two G16R16 textures, same size as the frame). The blurs ping-pong
  between them: mask into scratch A, then A into scratch B.
- Input for the apply pass: the blurred term in s4 (the scratch B result, or the raw mask in skip-blur mode).

## 4. Settings

Every key this effect reads, with where it is read:

| Section | Key | Type | Range | Default | Effect |
|---|---|---|---|---|---|
| `[_Shaders.ShadowsInteriors.Status]` | Enabled | bool | on/off | true | Turns the effect on or off. |
| `[_Shaders.ShadowsInteriors.Main]` | Darkness | float | not stated in the defaults file | 0.65 | Strength of the indoor shadow darkening (the "darkness of shadows"). Reaches the apply as the darkness constant (component y). The apply turns it into a darkening amount through a Reinhard-type curve with a speed of 5, then scales by the shadow-fade enable (section 6). |
| `[Main.Main.Performance]` | NvrDxvkPasses | bool | on/off | true in the defaults file (false in the C++ default) | When on and running on Vegas Shaders DXVK, the dedicated path runs as Vulkan passes. Off (or not on DXVK): the D3D9 dedicated path. |
| `[Main.Main.Performance]` | InteriorShadowFold | bool | on/off | false | When on, the apply moves into the fog composite and this pass is not drawn when the fold conditions hold. |
| `[Main.Main.Performance]` | InteriorShadowFoldRound | int | 0-3 | 0 | Rounding mode used only by the fold. Not this effect's look. |
| `[_Shaders.ShadowsInteriors.Forward]` | KeepDarkening | bool | on/off | not confirmed in the defaults file | Whether the room darkening is drawn at all when per-object lamp shadows are on. |
| `[_Shaders.ShadowsInteriors.Forward]` | DarkeningBlur | bool | on/off | not confirmed | On: the darkening is blurred (full blur path). Off: skip-blur mode. |
| `[_Shaders.ShadowsInteriors.Forward]` | MaskHalfRes | bool | on/off | not confirmed | Half-resolution mask for the room darkening (affects the mask it reads, not the apply). |

Notes: the C++ also reads `[Shaders.ShadowsInteriors.Main] Enabled` into the interior settings, but no such key is in the
defaults file (see section 9). `[_Shaders.ShadowsInteriors.RT]` (traced lamp shadows) lives in this folder's defaults fragment but is
read by `src/core/RtLampShadows.h`, not by this effect: docs/next/FORK-RT-LAMPS.md (2026-11-07: `SameFrame`, on = this frame's trace
before the world draw, with the helper pass `Compute/Rt/RtLampPrimary.comp`; `MovingCasterHistory` 2 = ManyLights' histories follow
people's moving shadows). The `Darkness` value is shared with the lamp-shadow code that writes the mask.

The ray-traced lamp shadows (`[_Shaders.ShadowsInteriors.RT]`, core/RtLampShadows.h) feed the per-object lamp shadows, not this pass.
Since 2026-11-07 their `ShadowDenoiser` (default true; a restart to come on, off is live) traces the `DenoiseLamps` lamps that matter
most with one ray at their physical size (`PhysicalLampSize`, `PhysicalLampScale/Min/Max`) and denoises them in the RT helper
(`Denoiser*` keys): docs/next/RT-DENOISER.md. Nothing here changes.
Since 2026-11-07 (perf-history, docs/next/RT-PATH-TRACING.md "perf-history") `[_Shaders.ShadowsInteriors.RT] DenoiserHistory`,
`DenoiserStabilize` and `DenoiseHistory` (the RtDenoise path) are frames AT 60 FPS: with `[Main.Main.RayTracing] DenoiseTimeBased`
(default on) they are kept as the same seconds at the game's frame rate. At 60 fps nothing changes.
Since 2026-11-07 (rtgap-tess, docs/next/RT-DETAIL-GAPS.md section 1) `[_Shaders.ShadowsInteriors.RT] ContactShadows` (0-1, default 1) and
`ContactLength` (pixels, 24) add a screen-space contact march toward each traced lamp (RtLampTrace's variants) for what the depth buffer
has and the RT scene lacks; it stays off while this effect's own lamp contact shadows (`[_Shaders.ShadowsInteriors.Main]
ContactShadows` > 0) draw. With the tessellated bumps on the traced rays start the bumps' Strength further off the surface
(`[Main.Main.RayTracing] TessStartSlack`). Nothing here changes.

## 5. Passes and outputs

### Generic technique (default name: the unnamed technique, index 0, three passes), D3D9

1. Horizontal depth-aware blur of the shadow term (s3) into a full-resolution HDR-format target. Output: the blurred term
   in red. Offset direction: (1, 0) in texels.
2. Vertical depth-aware blur of the output of pass 1 (s0 is bound to it), into the same kind of target. Offset direction: (0, 1).
3. Apply: reads the scene from TESR_SourceBuffer (s1: the copy the chain makes at the start of the effect), the blurred term from
   TESR_RenderedBuffer (s0: the output of pass 2, which the chain binds to s0 for this pass), and depth (s2). Writes the darkened
   scene to the frame. Output: opaque colour, alpha 1. (Wave 3: the old text had the scene in s0 and no read of s1, which cannot
   hold for a copy-free chain; the scene now comes from s1. Same result.)

The frame chain supplies the full-resolution copy of the scene and clears each target before its pass. Blurred pixels beyond
the draw distance are discarded by the blur, which leaves the target black there (see section 6).

### Dedicated technique `DedicatedInteriorShadows`, three passes, D3D9

1. Horizontal blur: reads s3 (the raw mask) and depth (s2). Writes scratch A (G16R16). Depth passes through the
   same blur rules as in the generic path.
2. Vertical blur: reads s4 (bound to scratch A) and depth. Writes scratch B (G16R16).
3. Apply: reads the scene (s0, untouched), the blurred term (s4, bound to scratch B), and depth. Writes the frame.

Before each blur pass the C++ clears its target to opaque black. In skip-blur mode the first two passes are skipped and the
apply runs alone, reading the raw mask bound to s4. The apply pass must be the last pass in both modes.

Every pass writes its full target except where the blur deliberately leaves black (beyond the draw distance). The apply
writes every pixel of the frame.

### Vulkan passes (only the dedicated path has a Vulkan version)

The generic technique has no Vulkan version; it always runs as D3D9.

- Blur pass, file Compute/ShadowsInteriorsBlur.frag: two runs (horizontal, then vertical). Each reads the mask and depth and
  writes one scratch target. Textures: set 1, binding 0 = mask (2D, linear, clamp), binding 1 = depth (2D, linear, clamp).
  Sampler: set-1 sampler slot 0 (linear, clamp), heap index taken from the push block.
- Apply pass, file Compute/ShadowsInteriorsApply.frag: one run, writes the frame through the full-screen path.
  Textures: set 1, binding 0 = scene, binding 1 = blurred mask, binding 2 = depth. Sampler as above.

Output formats: both blur targets are G16R16 scratch targets owned by the sun-shadow effect; the apply writes the frame's
HDR target.

## 6. Behaviour details

- Depth scale: depth samples are read as stored and multiplied by the far distance to get game units. The blur's depth
  threshold scales with the centre depth: the threshold is 3500 game units times (centre depth / far distance), so it grows
  with distance. A tap contributes only if its depth differs from the centre by less than the threshold.
- Blur kernel: 12 taps, symmetric about the centre, sampled at offsets of -6..-1 and +1..+6 texels in the blur direction
  (no tap at zero offset, the centre is added separately). Tap weights, in order from the first tap to the twelfth:
  0.057424882, 0.058107773, 0.061460144, 0.071020611, 0.088092873, 0.106530916, 0.106530916, 0.088092873, 0.071020611,
  0.061460144, 0.058107773, 0.057424882. The centre sample has weight 0.114725602. The result is the weighted average of
  the accepted taps divided by the sum of the accepted weights (the centre is always accepted).
- Blur early exits, which must match the look (they do not change the result):
  - If the centre depth is beyond the draw distance (MAXDISTANCE), the blur returns black with alpha 1. This is what
    the older passes left in their cleared targets.
  - If every tap equals the centre (the largest difference in red or green is below 0.0001), the blur returns the centre
    value without reading depth again. The result is the same as the full blur would give.
- Blur edges with render-scale rects: the D3D9 blur clamps every tap to the drawn part of the frame when a render-scale rect
  is active, so the widest taps at the right and bottom edges stay inside the drawn part. The Vulkan blur does not clamp.
  It is safe only because the Vulkan path is switched off while a rect is active.
- Apply, step by step (the look must match this):
  1. Convert the scene colour from gamma 2.2 to linear.
  2. Compute the world-space view direction for the pixel, and the uniform distance = depth (game units) times the
     length of that direction.
  3. Clamp the shadow sample to 0..1. Fade it toward fully lit with distance: the fade runs from 0 at 300 game units to 1 at
     the draw distance (MAXDISTANCE), linearly and clamped.
  4. Darkness amount: let d = Darkness constant. The curve is 5d / (1 + 5d). Then darkness = clamp(1 - curve * ShadowFade.y, 0, 1).
     With shadows disabled (ShadowFade.y = 0), darkness = 1 and the result is unchanged.
  5. Shadow = clamp(lerp(darkness, 1, faded sample), 0, 1). 0 is full shadow, 1 is full light.
  6. shadowPower = 1 - Shadow.
  7. Colour: Shadow times the colour raised to the power (1 - shadowPower * 0.2), clamped to 0..1. This boosts the gamma of
     the darker parts.
  8. Saturation: mix the colour with its luma by the factor (1 + shadowPower * 0.3). Darker parts keep some colour. (Wave 3 reading: the mix is lerp(luma, colour, 1 + shadowPower * 0.3), which raises saturation in the shadow. The other order would give a factor above 1 that over-desaturates and can go negative, which is the grey look the spec warns about.)
  9. Add back any part of the original colour above 1 (max(original - 1, 0)), so highlights above 1 are not lost.
  10. Convert back to gamma 2.2 encoding. Output alpha 1.
- Sky: depth for sky pixels is at or beyond the far distance, so the blur writes black there and the apply finds no shadow
  beyond the draw distance. The fade is to fully lit, so the sky is not darkened. (This was inferred, not checked, see section 9.)
- First-person weapon and view model: not handled by this effect; the view model is drawn before the frame reaches it.
  Not checked in the source.
- Jitter (TAA): the effect does not apply sub-pixel jitter to its own taps. It samples at the pixel centre of the frame
  it is given.
- Reversed depth: the depth buffer is not reversed here; depth 0 is the camera and 1 is the far distance.
- HDR vs LDR: the scene the apply reads is HDR (linear-ish, values above 1 possible). The apply keeps values above 1
  (step 9), so highlights are not clipped by it.
- NaN and inf guards: none in the apply or blur. Depth is clamped by the fade and by the early exits only.
- Things that look wrong if missed:
  - Dropping the step-9 add-back makes bright lamp highlights go dim where they overlap a shadow.
  - Dropping the saturation step makes shadows grey and flat.
  - Not clearing the blur targets to black before the blur passes leaves stale colour beyond the draw distance.
  - Not using the far-distance fade (300 to MAXDISTANCE) makes shadows linger over the whole room.
  - The skip-blur mode must not run the blur passes (it would blur a mask that was supposed to be sharp).

## 7. Vulkan path

Selection (in the C++):

- The dedicated path is used when its technique and textures are available and the blur and apply targets match the frame
  size and the render target.
- The Vulkan path is tried first only when the Vulkan passes are on: `[Main.Main.Performance] NvrDxvkPasses` is on, the
  game runs on Vegas Shaders DXVK (the extension is available), and no render-scale rect is active. In that case the C++ runs the
  two blur passes into the two scratch targets and the apply into the frame.
- If any Vulkan pass cannot be created or fails before the apply is committed, the effect falls back to the D3D9 dedicated
  path for that frame. If it fails in the middle of the D3D9 path, that path falls back to the generic technique. A failure
  of the Vulkan path sets its failed flag, so the session stays on D3D9.
- The generic technique (three passes through the frame chain) has no Vulkan version and always runs as D3D9.
- Wave 3 (2026-11-02): ShadowsInteriorsBlur.frag and ShadowsInteriorsApply.frag are written from the D3D9 dedicated passes with the push offsets above, and both compile with tools/nvr-dxvk/compile-compute.sh (spirv-val passes). Their .spv files are new builds. Not run in the game.

Blur pass push block (Compute/ShadowsInteriorsBlur.frag):

| Offset | Name | Type | Meaning |
|---|---|---|---|
| 0 | cameraData | vec4 | y = far distance |
| 16 | reciprocalResolution | vec4 | texel size (x, y) |
| 32 | offsetMask | vec2 | (1, 0) for the horizontal pass, (0, 1) for the vertical pass |
| 40 | maxDistance | float | draw distance (MAXDISTANCE) |
| 44 | samplerIndex | uint | heap index of the LINEAR, CLAMP sampler |

Apply pass push block (Compute/ShadowsInteriorsApply.frag):

| Offset | Name | Type | Meaning |
|---|---|---|---|
| 0 | cameraData | vec4 | y = far distance |
| 16 | shadowData | vec4 | y = darkness |
| 32 | shadowFade | vec4 | y = enable (0 or 1), w = draw distance |
| 48 | viewTransform | mat4 | view matrix, stored as the renderer stores it |
| 112 | projectionTransform | mat4 | projection matrix, stored as the renderer stores it |
| 176 | samplerIndex | uint | heap index of the LINEAR, CLAMP sampler |

The C++ push structs also have padding fields after the apply's sampler index. The offsets above are the ones the shader
reads; the padding is not read. The C++ records the sampler's heap index into the blur push at offset 44 and into the apply
push at offset 176, so the sampler is filled in by the extension at those offsets.

Descriptor binding: textures are bound in set 1, binding i, in the order listed in section 5. The sampler is set-1 slot 0
(linear, clamp). Views are 2D.

## 8. Look and cost

- Correct look: indoors, objects and walls that block a lamp leave a soft, darker, slightly desaturated shadow on the surfaces
  behind them. Shadows fade out over the room toward the draw distance. Surfaces right at the camera are darkened fully by
  the shadow term, not made grey. Highlights from lamps are not dimmed by the shadow. No halo or dark edge appears where the
  blur crosses depth discontinuities (the depth threshold keeps the blur from smearing across a wall edge).
- Typical wrong results:
  - Dark halos around objects where the blur averages across a depth edge (the depth threshold is too large or missing).
  - Grey, washed-out shadows (saturation step missing or wrong sign).
  - Shadows that never fade (the 300 to draw-distance fade missing).
  - A visible hard line where the draw distance cuts off (the black beyond the draw distance is not faded by the apply).
  - Shadows darker when the render-scale rect is active (taps not clamped on the D3D9 path).
- Cost class: multi-pass, per-pixel loop. Generic path: two 12-tap blurs (each tap reads the mask and, for the depth
  test, depth) plus the apply, all at full frame resolution. Dedicated path: the same work on 16-bit scratch targets (less
  bandwidth), no copy of the frame. Skip-blur mode: the apply alone, one pass.

## 9. Open questions

- `[Shaders.ShadowsInteriors.Main] Enabled`: read by the shadow-exterior code into the interior settings, but the defaults
  file has no such key. Whether it has any effect on this pass was not established.
- Draw-distance and fade values: the value of TESR_ShadowFade.w and TESR_ShadowFade.y indoors depends on the shared shadow
  constants code. The interior branch sets y from the interior enable; w was taken as the exterior MAXDISTANCE value and was
  not confirmed for interiors.
- Darkness range: the defaults file gives 0.65 with no range; the menu may clamp it (not checked).
- Defaults not confirmed in the defaults file: KeepDarkening, DarkeningBlur, MaskHalfRes in `[_Shaders.ShadowsInteriors.Forward]`.
  Which of skip-blur and blurred mode is the shipped default was not established.
- Menus: whether the effect runs while a menu is open, and how its apply interacts with menu-drawn scenes, was not traced.
- Sky handling: the claim that sky pixels hold a depth at or beyond the far distance was inferred from the blur's early
  exit, not checked against the sky pass.
- Blur sampling filter on the depth read: the D3D9 depth reads use mip level 0 explicitly; the filter the shader used for the
  shadow mask in the generic path was not confirmed beyond the sampler declarations.
- Non-forward indoor branch: when forward lamp shadows are off, the pass runs as the normal blurred path; the frame branch
  that handles it (the separate pass and the interior branch of the apply stage) was read but not every call site
  (src/core/ShaderManager.cpp around the interior apply and the shadow-apply branch) was traced to its end.
- Vulkan blur clamping: the Vulkan blur does not clamp taps to a render-scale rect. Safe only while the Vulkan path is gated
  off during rects (as the C++ does now); a change to that gate would need the clamp added.
- Generic path binding: RESOLVED in wave 3 by the shader's design. The chain binds the current image to s0 for each pass
  (EffectRecord::RebindSlotTextures), and the scene copy into s1 happens once at the start of the effect (EffectRecord::Render,
  usesSourceBuffer). So pass 2 reads the blurred term from s0 and the scene from s1.
