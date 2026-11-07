# Sharpening

UNOFFICIAL 2026-11-04 (mrfury97 fork, branch amd-cas): the shader is now **AMD FidelityFX CAS** (Contrast Adaptive Sharpening), ported from
`ffx_cas.h` (MIT, Copyright (c) 2017-2019 Advanced Micro Devices, Inc.; the notice is in both shader files). The non-scaling filter is used,
with `CAS_BETTER_DIAGONALS` and AMD's default one weight for all channels (green's). The mod's depth falloff is kept. Shaders:
src/hlsl/NewVegas/Effects/Sharpening.fx.hlsl (D3D9) and src/hlsl/NewVegas/Compute/Sharpening.frag (Vulkan); keep the two equal.
The C++ is unchanged. The old shader (a weighted 3x3 average, pushed away by a luma-weighted contrast) is gone.

## 1. Purpose

Sharpening makes near surfaces look crisper. CAS adapts per pixel: it measures how much headroom the pixel has before its 3x3 neighbourhood
would clip, and sharpens low-contrast detail (textures, foliage, fabric) more than edges that are already strong. Strong edges get little
or no extra contrast, so CAS does not draw halos or ringing, and the result never leaves the neighbourhood's range. The effect fades out
with distance: nearby surfaces get the full CAS, surfaces past a set distance get none, so the sky and far land are left alone.

## 2. When it runs

- **Switch:** `[_Shaders.Sharpening.Status] Enabled`, default `false`. Off: the effect does not run.
- **Frame position:** in the post chain (`EffectChain.cpp`): ... SMAA, FXAA, **Sharpening**, Cinema, ImageAdjust, Bodycam, SkyDither,
  Debug. It sees the anti-aliased picture and feeds everything after it. It can be the final writer to the game target.
- **Interior and exterior:** the same pass; the values come from `Main` outdoors and `Interiors` indoors (`GameState.isExterior`), re-chosen
  every frame.
- **Night, menus:** no special rule.
- **Single technique:** index 0 only.

## 3. Inputs

### Textures (slot order; Vegas Shaders binds samplers by declaration order)

| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | `TESR_RenderedBuffer` | The current post-chain picture (alpha carried through) | **Point**, clamp (CAS reads exact pixels; the UVs are texel centres) |
| s1 | `TESR_DepthBuffer` | CombineDepth's combined depth: linear depth / far plane | Linear, clamp |

### Constants

| Name | Components |
|---|---|
| `TESR_SharpeningData` | x = Strength (CAS sharpness, saturated to 0..1); y = Clamp (unused); z = Offset (falloff distance / 1000); w = 0 |
| `TESR_ReciprocalResolution` | xy = 1 / picture size in pixels (the tap step) |
| `TESR_CameraData` | y = far plane distance (through the depth helper) |

## 4. Settings

| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| `[_Shaders.Sharpening.Main]` | `Strength` | float [0-1] | 0.5 | Outdoors CAS sharpness: the cross neighbours' peak weight is -1 / lerp(8, 5, Strength). 0 = CAS's lightest (still sharpens; turn the effect off with Enabled), 1 = the strongest. Was 0.75 with the old shader. |
| `[_Shaders.Sharpening.Main]` | `Offset` | float [0-50] | 2.0 | Outdoors fade distance in thousands of units (2.0 = 2000 units). 0 or less = no sharpening. |
| `[_Shaders.Sharpening.Main]` | `Clamp` | float | 0.1 | Unused (kept so older tomls stay valid). CAS cannot overshoot. |
| `[_Shaders.Sharpening.Interiors]` | `Strength`, `Offset`, `Clamp` | | 0.5, 2.0, 0.1 | The same, indoors. |
| `[_Shaders.Sharpening.Status]` | `Enabled` | bool | false | The whole effect on or off. |

## 5. Passes and outputs

- **D3D9:** one pass, technique index 0, full-screen quad into the chain's output (8-bit LDR, same size). Alpha = input alpha.
- **Vulkan:** the pass `Sharpening` (Sharpening.spv), same result. Writes only the colour target; never the depth.

## 6. Behaviour details

**Depth falloff.** maxDepth = Offset x 1000; falloff = saturate((maxDepth - distance) / maxDepth)^4: 1 at the camera, 0 at maxDepth and
beyond (the sky is at the far end, so it gets 0). The CAS result is blended with the original by this factor. maxDepth <= 0 returns the
original picture (no divide by zero). Pixels with falloff 0 skip the 8 neighbour reads.

**CAS (per pixel).** Taps a..i of the 3x3 neighbourhood (e = the pixel; b, d, f, h the cross):
1. Soft minimum and maximum per channel: mn = min(cross) + min(all 9), mx = max(cross) + max(all 9) (both 0..2; CAS_BETTER_DIAGONALS).
2. amp = sqrt(saturate(min(mn, 2 - mx) / mx)) on the green channel (mx guarded by 1e-5): the headroom to black or white over the max.
3. w = amp x peak, with peak = -1 / lerp(8, 5, saturate(Strength)) (so w is in -0.2..0).
4. out = saturate(((b + d + f + h) x w + e) / (1 + 4w)) per channel. 1 + 4w is at least 0.2, never 0.

The picture is 8-bit gamma-encoded; CAS runs on it as is (AMD's filter works in perceptual space as well as linear; it is not converted).

**First-person weapon:** near, so the full CAS, as before. **RenderScale rect frames:** the Vulkan pass is off while a rect is active
(`DxvkCompute::PassesOn`), so the D3D9 pass runs on the full-size picture after the upscale. **TAA jitter:** none reaches this pass (it
runs after TAA). **NaN:** none possible (guards above).

**Would look wrong if missed:** halos on strong edges (CAS weight or soft min/max wrong); a sharpened sky or horizon (falloff not reaching 0);
colour fringes (per-channel weights instead of green's); a blurred picture (the sign of peak lost).

## 7. Vulkan path

- **Files:** `Compute/Sharpening.frag` -> `Sharpening.spv` (glslang, Vulkan 1.1).
- **Selection:** `SharpeningEffect::ExtRender`, when Vegas Shaders DXVK is loaded, `[Main.Main.Performance] NvrDxvkPasses` is on and the frame is not
  a rect frame; otherwise D3D9. A failed build or draw falls back to D3D9 for the session.
- **Bindings (set 1):** 0 = the rendered picture, 1 = the combined depth. One sampler (linear clamp, from the push block); the colour
  taps land on texel centres, so they read exact pixels.
- **Push constants (64 bytes):** 0 `TESR_SharpeningData`, 16 `TESR_ReciprocalResolution`, 32 `TESR_CameraData`, 48 sampler index, 52-63 padding.

## 8. Look and cost

- **Correct:** near textures and foliage crisper, flat areas and strong edges almost unchanged, no bright or dark outlines; far land and
  the sky unchanged; Enabled false gives exactly the picture before.
- **Cost:** one pass, 9 colour reads + 1 depth read and a few ALU ops per near pixel; 1 colour + 1 depth read past the falloff distance.

## 9. Open questions

1. `Clamp` is unused; it could be removed from the settings once older tomls no longer matter.
2. CAS on the gamma-encoded picture vs linear: AMD notes either works; a linear option could be tried if the look is too strong in darks.
3. RenderScale below 1: the pass runs after the upscale on the full-size picture, but the depth falloff reads the combined depth at the
   same uv; whether that depth is still in the rect's coordinates then was not checked (the old shader read it the same way).
