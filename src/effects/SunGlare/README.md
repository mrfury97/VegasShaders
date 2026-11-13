# Effect: SunGlare

UNOFFICIAL, 2026-11-13 (modern lighting). Source of truth: `SunGlare.h/.cpp`, `src/hlsl/NewVegas/Effects/SunGlare.fx.hlsl`. No Vulkan twin yet (the D3D9 technique draws under DXVK too).

## 1. Purpose

The sun's lens glare: what a camera lens (or the eye, through its lashes) makes of a small, very bright source. Round the sun's place on screen it adds
- a tight **halo**,
- a **starburst**: thin lines through the sun, each a pair of points, of differing length,
- a long horizontal **streak**.

It only adds light, and only as much as the sun's disc itself shows. Behind a tree, a wall, the first-person weapon or thick cloud there is no glare.

## 2. When it runs

- `[Shaders.SunGlare.Status] Enabled` (default false: a look change).
- Outdoors, above water, by day (`dayLight > 0.5`, as the god rays), the sun in front of the camera (`TESR_ViewSpaceLightDir.z > 0.05`), Strength above 0.
- Frame order: the HDR chain before the tone mapper, right after the god rays (`EffectChain.cpp` step 37b, `PreStep_SunGlare`). The tone mapper rolls its core off to white.

## 3. Passes

1. **Cover** (technique `SunGlareCover`, `SunGlareEffect::RunCover`): one draw into a 1 x 1 `A16B16G16R16F` target, `TESR_SunGlareCover`.
   - 16 taps on a spiral over a patch of radius SunSize round the sun's centre: the share that is sky in the depth buffer (`> 0.9`), and their mean brightness.
   - 8 taps on a ring at 7 x SunSize: the sky's brightness there. Patch over ring is the cloud contrast, through `smoothstep(CloudContrastLow, CloudContrastHigh)`. With no open sky on the ring (the sun in a gap between leaves) the patch is compared with a plain 0.25.
   - A sun off the screen cannot be measured: the result fades to 0 over the last 4 % past the edge.
   - The pass blends its value over what the texel held (SrcAlpha / InvSrcAlpha, alpha = 1 / EaseFrames), so the glare eases in and out. After a frame without the pass the texel is cleared first.
2. **Picture** (unnamed technique, through `EffectRecord::Render`): one full-screen pass. A pixel the glare cannot reach (halo + longest spike + streak under 0.004) returns the stored picture before anything else. Else it reads the cover texel and adds the glare in linear light.

Positions are in screen heights from the sun's projected position, so the shape is the same at any resolution and aspect.

## 4. Inputs

| Slot | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | the current HDR picture |
| s1 | TESR_DepthBuffer | linear depth (CombineDepth) |
| s2 | TESR_SunGlareCover | the eased cover texel (x the cover used, y sky share, z cloud contrast) |

Constants (all `SunGlareEffect::UpdateConstants`): `TESR_SunGlareData` (x strength x day x horizon fade x SunsetStrength, y halo size, z halo strength, w debug view), `TESR_SunGlareSpikes` (lines, length, width, strength), `TESR_SunGlareStreak` (length, width, strength, chromatic), `TESR_SunGlareSun` (patch radius, cloud ramp low / high, 1 / EaseFrames), `TESR_SunGlareTurn` (cos / sin of the first line's angle and of the step between lines). Shared: `TESR_ViewSpaceLightDir`, `TESR_SunColor`, `TESR_SunsetColor`, `TESR_SunDirection`, `TESR_SunAmount`, `TESR_ReciprocalResolution`.

## 5. Settings (`[Shaders.SunGlare.Main]`, all live)

Strength, HaloSize, HaloStrength, Spikes, SpikeLength, SpikeWidth, SpikeStrength, SpikeRotation, StreakLength, StreakWidth, StreakStrength, Chromatic, SunSize, CloudContrastLow, CloudContrastHigh, EaseFrames, SunsetStrength, DebugView. See `defaults.toml` for ranges.

Works best with the game's own glare sprite turned down (`[Shaders.Sky.Main] GlareStrength` about 0.2): at 1.0 that sprite is a white area a quarter of the screen wide and the starburst drowns in it.

## 6. Edge cases

- **Sun behind moving leaves, or a branch crossing it during a pan:** the cover changes from frame to frame; EaseFrames spreads it. At 1 the whole glare switches on and off (flicker sweep: 19 spikes up to 24 % in 2,400 frames; at 8: 8 spikes of 5-6 %, the bench's noise level is 4-5 %).
- **TAA / DLSS jitter** moves the depth edge by a fraction of a pixel each frame: same remedy.
- **Thick cloud over the sun:** the patch is no brighter than the ring, no glare. Thin cloud: partial.
- **RenderScale:** positions go through `rectToScreen` / `screenToRect`.
- **Night, interiors, underwater:** not drawn. The moon has no glare.
- The 1 x 1 target is the pass's own render target and sampler s2 of the picture pass: `RunCover` clears stage 2 before it draws.

## 7. Cost

0.06 ms with the sun in view at 1176 x 664 (RTX 4070 Ti SUPER), 0 when the sun is behind the camera (ShouldRender).

## 8. Looks right / wrong

- Right: a compact white core, 16 fine points of unequal length, a thin streak left and right, all gone the moment the sun passes behind a trunk.
- Wrong: glare over a tree or the weapon that covers the sun (the cover pass did not run: check the `SunGlareCover` technique loaded); a glare that blinks every frame (EaseFrames 1); a square-edged halo (HaloSize too large against the early-out threshold).
- DebugView 1 tints the picture green (sun counted visible) to red (covered); 2 shows the glare alone.
