# SunShadows: effect spec (describes the wave-3 shader of 2026-11-02, branch claude/rw3-fog-sun)

Files: `src/hlsl/NewVegas/Effects/SunShadows.fx.hlsl` (D3D9), `src/hlsl/NewVegas/Compute/SunShadows.glsl` with `SunShadowsMarch.frag`,
`SunShadowsBlur.frag`, `SunShadowsApply.frag` (+ `SunShadowsApply-c1.spv`), `SunShadowsSharp.frag`, `SunShadowsSharpFinish.frag` (Vulkan).
C++: `src/effects/SunShadows.cpp/.h` (passes, ping-pong, Vulkan), `src/effects/ShadowsExterior.cpp/.h` (constants, settings),
`src/core/ShadowManager.cpp` (cascade atlas and character map), `src/core/ShaderManager.cpp` (technique choice), `src/core/RtSunShadows.cpp`.
Reference for the cascade rules: `src/hlsl/NewVegas/Shaders/Includes/Shadow.hlsl` (`GetSunShadow`, `CharacterMapShadow`), not edited.
Settings sections in the toml carry a leading underscore (`[_Shaders.ShadowsExteriors.ScreenSpace]`); this spec drops it as the C++ does.

## 1. Purpose

SunShadows writes the outdoor sun's shadow term into the two-channel buffer `TESR_PointShadowBuffer` (G16R16, 0..1).
- Red: sun visibility, 1 = lit. It always holds the screen-space contact shadows (a short ray toward the sun from every pixel: dark
  where something stands right in front of the surface, under feet, props, along walls). In the deferred techniques it also holds
  the sun cascade maps and the player's character shadow map, combined so they agree with what the object shaders compute in the
  forward path (`Shadow.hlsl`).
- Green: the lamps' term written by the PointShadows passes before this effect. Carried through, blurred with red and raised to
  Intensity like red (as `ShadowsExteriorEffect::FadedContactUnread` describes the passes).
The exterior apply (`ShadowsExteriors.fx.hlsl`) later fades red by `TESR_ShadowFade.x` (dawn, dusk, moon) and darkens the frame by
Darkness. This effect applies neither.

## 2. When it runs

- Exterior frames only, inside the shadow block of `ShaderManager.cpp` (around line 2130), when `Effects.ShadowsExteriors->Enabled`
  holds, after the lamp passes. `SunShadowsEffect` has no `ShouldRender` override; `Enabled` comes from the effect loader (no
  `SunShadows` section in the defaults: on whenever the file loads).
- Skipped while `ShadowsExteriorEffect::FadedContactUnread()` is true: `[Main] SkipWhenFaded` on, the sun fade exactly 1, no lamp
  shadows outdoors, and none of blood flow, wet world, underwater or the debug view drawing. The buffer then keeps the lamp result.
- Technique (ShaderManager): 2 when `ForwardShadows` and `[ScreenSpace] Sharp` are on (and the file has `ForwardContactSharp` at
  index 2); else 1 when `ForwardShadows` is on, the file has `ForwardContactShadows` at index 1 and RT sun shadows are not replacing
  the cascades (`RtSunShadows::Replacing()`); else 0. `SunShadowsEffect::Render` then swaps 0 -> 3 and 1 -> 4 when
  `[ScreenSpace] FastPaths` is on and the file has `ContactFast` / `ForwardContactShadowsFast` at those indices.
- While SunFaded with ForwardShadows on, the C++ hands the passes `TESR_ShadowForwardData.x = 0` for the draw, so technique 0's
  apply leaves the cascades to the (faded) forward value and does not look them up.
- After the passes, `RtSunShadows::Apply` multiplies the traced sun visibility into red (nothing when RT is off).

## 3. Inputs

### 3.1 Samplers (EffectRecord binds stage N to the N-th declared sampler and parses `register ( sN )`: order = register)

| Slot | Name | States | Holds / use |
|---|---|---|---|
| s0 | `TESR_DepthBuffer` | LINEAR, CLAMP, no mip | View depth / far (R). Sky = 1. Read at texel centres (march samples are snapped). |
| s1 | `TESR_ShadowAtlas` | LINEAR, CLAMP, no mip | 2 x 2 cascade atlas, plain light-space depth (PCF). Near top-left, Middle top-right, Far bottom-left, Lod bottom-right. Taps at texel centres. `[ShadowMaps] Anisotropy` may set ANISOTROPIC here (SetCT; harmless at LOD 0). |
| s2 | `TESR_NormalsBuffer` | LINEAR, CLAMP, no mip | VIEW-space normal encoded 0..1 (Normals.fx). Decoded, normalised; facing the camera if empty. World normal = view normal through the transposed view rotation. |
| s3 | `TESR_PointShadowBuffer` | LINEAR, CLAMP, no mip | The pass's source: the buffer for pass 1, then the previous scratch target (the C++ rebinds stage 3 per pass). |
| s4 | `TESR_NoiseSampler` | LINEAR, WRAP, LINEAR mips | `Effects\bluenoise256.dds` (ResourceName annotation; also what `ExtFileTexture` hands the Vulkan pass). One noise texel per pixel. |
| s5 | `TESR_CharShadowMap` | POINT, CLAMP, no mip | The character sun map (R32F depth, cleared to 1). Declared always (so s6 stays stage 6); read only with `CHARACTER_SHADOW_MAP` and `TESR_CharShadowData.w > 0`. |
| s6 | `TESR_BackDepthBuffer` | POINT, CLAMP, no mip | Nearest back faces (BackDepth pass). Read only when `TESR_BackDepthData.x > 0.5` (BackDepth.hlsl). |

### 3.2 Constants (bound by name)

- `TESR_ProjectionTransform`: `_11, _22, _31, _32` rebuild view positions from linear depth and project them back (independent of
  REVERSED_DEPTH). `TESR_ViewTransform`: view -> world direction of normals. `TESR_InvViewTransform`: view -> camera-relative world
  position (w = 1, as `GetShadowWorldPos`). `TESR_InvProjectionTransform`, `TESR_DepthConstants`, `TESR_CameraPosition`: declared by
  Depth.hlsl, unused.
- `TESR_CameraData`: x near (ray points nearer are ignored, sharp ray end clamp), y far (depth scale, sky test, ray lengths).
- `TESR_ReciprocalResolution.xy`: one texel; blur stride, noise scale, texel snap, finish taps, rect clamp.
- `TESR_RenderScale` (RenderRect.hlsl): rect-frame coordinate mapping and read clamps.
- `TESR_ViewSpaceLightDir.xyz`: march direction and contact facing (normalised here).
- `TESR_SmoothedSunDir.xyz`: N.L for the cascade and character normal offset and slope slack (the sun the maps were drawn for).
- `TESR_ShadowScreenSpaceData`: x contact on, y BlurRadius (texels between blur taps), z RenderDistance, w Intensity.
- `TESR_ShadowScreenSpaceFade`: x NearFade start, y NearFade end (y = 0: no fade).
- `TESR_ShadowForwardData`: x 1 = object shaders' cascade lookups off (with FORWARD_SHADOWS compiled, 0 here means the apply skips
  the maps); w HandoverBlend.
- `TESR_ShadowFade.y`: maps drawn (0: no map term). (x, z: the apply's / the lamps'; not read.)
- `TESR_ShadowFormatData`: x must be 3 (PCF) and z 1 (outdoors), else no map term (as `GetSunShadow`).
- `TESR_ShadowBlur`: x 1 / atlas size (texel, floored at 1/16384), z SmoothFilter (cubic 4 x 4 instead of quadratic 3 x 3).
- `TESR_ShadowCameraToLightTransform{Near,Middle,Far,Lod}`, `TESR_Shadow{Near,Middle,Far,Lod}Center` (xyz centre, w radius).
- `TESR_RtSunReplace.x`: RT range R in Mode 1 (replace), else 0.
- With `CHARACTER_SHADOW_MAP`: `TESR_CharShadowCameraToLight`, `TESR_CharShadowCenter` (w half-extent), `TESR_CharShadowData`
  (x 1/res, y band share, z depth bias, w = 1 + CharacterShadowMapDebug, 0 = not drawn this frame).
- `TESR_BackDepthData` (BackDepth.hlsl): x drawn, y/z depth -> view distance.
- Not declared (the apply's, or unused by this effect): `TESR_ShadowData`, `TESR_SunAmbient`, `TESR_WorldViewProjectionTransform`,
  `TESR_ShadowCrossFade` and the `Old` cascades (CrossFade needs ForwardShadows), `TESR_ContactHardeningData` (needs ForwardShadows).

### 3.3 Macros

`FORWARD_SHADOWS` 0/1 (`[Main] ForwardShadows` at load), `CHARACTER_SHADOW_MAP` (defined 1 when `[ShadowMaps] CharacterShadowMap`
was on at load), `REVERSED_DEPTH` and `PHYSICAL_SKY_ON` (no effect here; compiles with all of them).

### 3.4 Includes

`Includes/Depth.hlsl` (with RenderRect.hlsl; for the declarations), `Includes/BackDepth.hlsl`. The PCF and character lookups are
written in the effect, following `Shaders/Includes/Shadow.hlsl`.

## 4. Settings

| Section | Key | Default | Reaches | Effect here |
|---|---|---|---|---|
| ScreenSpace | Enabled | true | ScreenSpaceData.x (and the effect's enable) | Off: red = 1 from the march; blurs and apply still run. |
| ScreenSpace | BlurRadius | 1.0 | ScreenSpaceData.y | Texels between the 12 blur taps. 0 = no blur. Not used by Sharp. |
| ScreenSpace | RenderDistance | 180000 | ScreenSpaceData.z | No contact past it; fades to none over its last 20 %. Blur leaves pixels past it untouched. |
| ScreenSpace | Intensity | 0.315 | ScreenSpaceData.w | Exponent on red and green in every technique's last pass (below 1 lighter). |
| ScreenSpace | NearFade | 400 | ScreenSpaceFade (x = half, y = full; pushed out by the land displacement fade) | Contacts none nearer than x, full from y. 0 = everywhere. |
| ScreenSpace | Sharp | false | technique 2 | One 16-step march + small finish instead of march + two blurs. |
| ScreenSpace | FastPaths | true | techniques 3 / 4 | The march's skips as real branches. Same picture. |
| Main | ForwardShadows | true | macro + ForwardData.x + technique choice | On: object shaders do the cascades (techniques 1, 2, 4). Off: technique 0/3 apply does them. |
| Main | Enabled | true | ShadowFade.y | Off: no map term. |
| Main | SkipWhenFaded | true | C++ skip (section 2) | |
| ShadowMaps | HandoverBlend | 70 | ForwardData.w | Width of the cascade cross-fade band (at least a tenth of the radius, at most the radius). |
| ShadowMaps | SmoothFilter | false | ShadowBlur.z | Cubic 4 x 4 PCF instead of quadratic 3 x 3, as the object shaders. |
| ShadowMaps | CharacterShadowMap | true | macro (restart) | Adds the character map lookup to the deferred apply. |
| ShadowMaps | CharacterShadowMapResolution / Bias / Blend / Margin / Fit / Target / Others | | CharShadowData, box | Map texel, bias, hand-over band, fit (producer side). |
| ShadowMaps | CharacterShadowMapDebug | 0 | CharShadowData.w - 1 | 1: map verdict, 2: map depth, 3: point depth, inside the box, replacing red. |
| ShadowMaps | Anisotropy | 0 | SetCT stage 1 | Dead (the assignment in ShadowsExterior.cpp is commented out); harmless. |
| ShadowMaps | Distance, CascadeResolution, CascadeLambda, PlayerInNear, FitToDepth | | cascade centres, radii, matrices | Producer side; read through the constants. |
| RT | Enabled, Mode, Range | false, 0, 6000 | RtSunReplace.x (Mode 1) | Mode 1: maps = 1 out to 0.85 R, back linearly to full at R. |
| Main.Main.Performance | NvrDxvkPasses | true | DxvkCompute::PassesOn | Vulkan passes (section 7). |
| Main | Darkness, NightMinDarkness, UsePointShadowsDay/Night | | the apply / lamps | Not read here. |

## 5. Passes and outputs

All passes draw the frame quad (`FrameVS` passes position and uv through) into a G16R16 target of the screen size, viewport set to the
drawn part in rect frames. Ping-pong (SunShadows.cpp): pass 1 reads the buffer and writes scratch 0; each next pass reads the
previous target and writes the other scratch; the last writes the buffer. No pass reads its own target. Output: (red, green, 0, 1).

| Index | Name | Passes (pixel function) |
|---|---|---|
| 0 | (none) | ContactMarchPS(false), BlurAcrossPS, BlurDownPS, ApplySunMapsPS |
| 1 | ForwardContactShadows | ContactMarchPS(false), BlurAcrossPS, BlurDownFinalPS |
| 2 | ForwardContactSharp | SharpMarchPS, SharpFinishPS |
| 3 | ContactFast | ContactMarchPS(true), BlurAcrossPS, BlurDownPS, ApplySunMapsPS |
| 4 | ForwardContactShadowsFast | ContactMarchPS(true), BlurAcrossPS, BlurDownFinalPS |

Instruction slots (fxc ps_3_0): march ~376, blur ~365, apply ~770 (~925 with the character map), sharp ~184 (loop), finish ~76.

## 6. Behaviour

### 6.1 Positions
View position from linear depth z: `x = (ndc.x - P31) z / P11`, `y = (ndc.y - P32) z / P22`, ndc from the screen position of the
texture coordinate (rect-aware). Projection back is the inverse. Camera-relative world position = view position through
`TESR_InvViewTransform` (w = 1). Sky: stored depth >= 0.9999.

### 6.2 Contact march (ContactMarchPS)
- No contact (red 1) when contacts are off, z >= RenderDistance, sky, or facing <= 0, where facing = saturate(8 N.L) with the view
  normal and the view-space sun. FastPaths: real branch; otherwise computed and selected (same result).
- Four samples at `i * step`, i = 1..4, along the view-space sun from the pixel. step = 400 (= 2000 / 5) x (z / far)^0.6 x
  lerp(min(0.8, z / RenderDistance), 1, blue noise). Weight 1 / i.
- A sample counts (RayBlocked) by how far the ray point lies behind the depth seen at its texel centre: 0 below the bias
  (max(0.01, 0.002 z)), ramping to 1 over one more bias, back to 0 over the last quarter of the thickness. Thickness: 20 units, or
  BackDepth's back face minus the front when it drew one there (never over 20). Points off the drawn part or nearer than the near
  plane count 0. The ramps (instead of steps) keep sub-pixel motion and TAA jitter from flipping samples.
- Contact = 1 - occ^0.3 x facing x NearFade, faded to 1 by smoothstep(0.8 RD, RD, z). Green copied from the source.

### 6.3 Sharp march (SharpMarchPS) and finish (SharpFinishPS)
- Ray end = pixel + sun x 4000 (z / far)^0.6, pulled back along the ray to the near plane if it would cross it. 16 even screen-space
  steps (t = k / 16) between the pixel and the end's projection, ray depth = 1 / lerp(1/z0, 1/zEnd, t) (perspective-correct),
  weight 1 / (1 + 4t), same RayBlocked and contact formula, no noise.
- Finish: centre red counted twice plus the four direct neighbours whose depth is within max(2 % z, 1); then red AND green raised to
  Intensity.

### 6.4 Blur (BlurAcrossPS, BlurDownPS, BlurDownFinalPS)
12 taps at k = +-1..+-6 texels x BlurRadius along x or y (rect-clamped), weight exp(-k^2 / 18) x saturate(1 - |plane gap| / tol),
plane gap = distance of the tap's view position from the centre's plane (centre normal), tol = max(1, 0.012 z). The centre counts 1.
Blurs red and green. Sky and pixels at or past RenderDistance are kept. BlurDownFinalPS raises the result to Intensity.

### 6.5 Apply (ApplySunMapsPS)
`contact = pow(max(src.rg, 1e-6), Intensity)`; `red = min(contact.r, maps)`; a character-map debug view replaces red. Green = contact.g.

### 6.6 Map term (SunMapsVisibility), = Shadow.hlsl GetSunShadow (PCF) except the fade
- 1 when FORWARD_SHADOWS is compiled and ForwardData.x < 0.5, ShadowFade.y <= 0.5, FormatData.z <= 0.5, FormatData.x != 3, or sky.
- N.L with the world normal and SmoothedSunDir; offsetScale = saturate(1 - N.L).
- Per cascade: texel (world) = 4 R x atlas texel; normal offset = 1.5 texels x offsetScale; slope slack = 2 x atlas texel x
  min(tan, 5) (tan from N.L floored at 0.05); bias = 0.0002 (1 + offsetScale). Compare depth = light z - bias - 1.5 slack.
- Cascade = innermost whose sphere (centre, radius) holds the point; band starts at R - max(0.1 R, min(HandoverBlend, R)),
  fade = smoothstep over the band. Inside the band both this cascade and the next are read at the SAME point (normal offset lerped
  from this cascade's to the next's) and blended by fade; the Middle read inside Near's band keeps Near's slack in world units
  (slack x lerp(R_near / R_middle, 1, fade)). The Lod cascade fades to 1. Past it: 1.
- PCF: quadratic B-spline over the 3 x 3 texels round the point (cubic B-spline 4 x 4, half a texel more slack, with SmoothFilter);
  taps clamped half a texel inside the quadrant.
- RT Mode 1 (RtSunReplace.x = R > 0): maps = lerp(1, maps, saturate((z - 0.85 R) / (0.15 R))). The traced visibility fades to 1
  linearly over the same last 15 % (Rt/RtSunShadow.comp), so cascades and rays hand over without a double shadow or a gap.
- Character map (CHARACTER_SHADOW_MAP, CharShadowData.w > 0), after the RT fade (the rays do not trace actors): point offset by
  1.5 x 2 x halfExtent x map texel x offsetScale; inside the box (edge = max |x|, |y| of its clip position) weight = 1 -
  smoothstep(1 - max(band, 0.01), 1, edge); quadratic 3 x 3 PCF on the map with depth = min(z, 0.9999) - bias - 1.5 x texel x slope;
  result lerp(maps, min(maps, lit), weight). Debug views (w > 1.5) inside the box: 2 verdict, 3 map depth, 4 point depth.
- No sun fade (TESR_ShadowFade.x) here: the exterior apply fades red; applying it here too would fade twice.

### 6.7 Edge cases
- Sky: contact 1, maps 1, blur keeps it.
- RenderScale rect frames: positions use rectToScreen / screenToRect; every off-centre read is rect-clamped or (march) dropped
  when outside the drawn part.
- TAA jitter: the effects see the unjittered projection while the depth was drawn jittered (TAA.cpp BeginJitter); no jitter
  constant reaches this effect, so nothing is undone. Stability comes from: static (screen-fixed) blue noise, ramps instead of steps
  in the ray test, texel-snapped depth reads, plane-weighted blur, texel-snapped cascades with B-spline PCF and band blending.
- Reversed depth: not relevant (linear depth and projection terms only).
- NaN guards: texel floors (1/16384), radius / band floors, max(..., 1e-6) before pow, non-zero blur weight sum (centre 1),
  1 / max(z, 1e-3) in the projection, ray end clamp at the near plane, normal fallback.
- First-person weapon: whatever CombineDepth gives its pixels; treated as any surface.

## 7. Vulkan path

- `SunShadowsEffect::ExtPingPong` (when `DxvkCompute::PassesOn()`: NvrDxvkPasses on, Vegas Shaders DXVK present, not a rect frame) runs the
  same techniques through the same targets: March (techniques 0, 1, 3, 4 pass 1), Blur (push pass 0 across, 1 down, 2 down +
  Intensity), Apply (`SunShadowsApply-c1` when CHARACTER_SHADOW_MAP was compiled; variants.txt line
  `SunShadowsApply c1 -DCHARACTER_SHADOW_MAP=1`), Sharp, SharpFinish.
- Bindings: set 1 binding 0 depth, 1 atlas, 2 normals, 3 source, 4 noise, 5 character map (depth as stand-in when off); set 2
  binding 0 the 832-byte std140 block (4 cascades, projection, invProjection, view, invView, reciprocalResolution, smoothedSunDir,
  viewSpaceLightDir, formatData, screenSpaceData, shadowFade, shadowBlur, forwardData, center[4], cameraData, flags (x FORWARD_SHADOWS
  compiled, y/z NearFade, w RT range), charToLight, charCenter, charData). The C++ comment "eight matrices: 736 bytes" counts only the
  first block; 832 is right. Push (16 bytes): linear sampler heap index, noise sampler index, blur pass, pad.
- Matrices as stored: GLSL `M[i][j]` = HLSL `M[i][j]`, HLSL `mul(v, M)` = `M * v`, `mul(M, v)` = `v * M`.
- The GLSL is the D3D9 arithmetic; the March is the FastPaths form for every technique (same picture). Differences: no back-face
  thickness (not bound by the C++: always 20 units, as D3D9 with BackDepth off, its default); the rect clamp is the plain half-texel
  clamp to the texture (no TESR_RenderScale in the block, and PassesOn excludes rect frames, so the drawn part is the whole texture).
  The character map is read with the LINEAR sampler at texel centres (= POINT).
- Build: `bash tools/nvr-dxvk/compile-compute.sh SunShadowsMarch SunShadowsBlur SunShadowsApply SunShadowsSharp SunShadowsSharpFinish`.

## 8. Look and cost

Correct look: soft contact darkening where things meet the ground and walls, fading in past NearFade and out toward
RenderDistance, none on faces turned from the sun; cascade shadow edges matching the object shaders' (same filter, bias, bands)
with no pop at cascade borders; the player's face and hair self-shadow sharp in third person with the character map in the
deferred path; no crawling under TAA or camera motion.
Wrong-look signs: bands on lit ground (bias), shadows on the far side of thin walls (thickness), halos across silhouettes (blur
weights), double or missing shadow in RT Mode 1 near R (fade), pops at cascade borders (bands), acne (normal offset / slack).
Cost: 3-4 full-screen passes; march 4 samples, sharp 16; blur 12 depth reads + 12 source reads per direction; apply 9 atlas reads
(18 in a band, 16/32 with SmoothFilter) + 9 for the character map inside its box.

## 9. Fixed and open items

Fixed in this rewrite:
1. Character shadow map was not sampled: now read in the deferred apply (D3D9 and the Vulkan c1 variant), as `CharacterMapShadow`.
2. RT replace range: the two old statements disagreed; decided from RtSunShadows.h ("the deferred apply fades the cascades in only
   where the rays fade out"), the toml (Mode 1: "the cascades fade in only over the last 15 % of Range") and Rt/RtSunShadow.comp
   (rays fade linearly over the last 15 %): maps are 1 inside 0.85 R and come back linearly to full at R.
3. The cascade lookup now matches the object shaders (3 x 3 B-spline, SmoothFilter, slope slack, 1.5-texel offset, bands, slack
   scale) instead of a 2 x 2 compare with a different bias.
4. Normals: the buffer holds VIEW-space normals; the facing test uses them directly and the map lookups the world normal (the old
   spec had them the other way round).
5. `TESR_NoiseSampler` declares its ResourceName, so the blue noise really loads (D3D9 and `ExtFileTexture`).
6. `TESR_CharShadowMap` is declared unconditionally, so `TESR_BackDepthBuffer` is the 7th sampler and binds to stage 6 whatever
   the macro says (stage = declaration index).
7. The apply's map term is gated like `GetSunShadow` (FormatData.x = 3, FormatData.z outdoors, ShadowFade.y).
8. Divide-by-zero guards (texels, radii, bands, projection), texel-snapped depth reads, ramps in the ray test, plane-weighted blur.
9. Vulkan: rewritten for every pass; the rect clamp is present (full-texture clamp, see 7).

Open / decided, for review:
1. Sharp finish raises green to Intensity too (the old file raised only red). Chosen for one behaviour across techniques, per the
   C++ comment that the passes raise the lamp term to Intensity; with Sharp on, lamp shadows outdoors look as with Sharp off.
2. Vulkan back-face thickness: the C++ binds no TESR_BackDepthBuffer / TESR_BackDepthData for the Vulkan passes; with
   `[Main.Main.Quality] BackDepth` on, D3D9 and Vulkan differ on thin occluders. Needs a C++ binding (slot 6 + one vec4) to close.
3. Ray lengths read "distance" in the old formulas as the depth's share of the far distance (400 x (z/far)^0.6 per step, 4000 x
   (z/far)^0.6 for Sharp), giving contact rays of tens of units at mid range; with raw view distance they would be kilometres.
   They therefore change with the far distance.
4. No sun fade here (the apply fades); if a reader of red other than the apply needs the faded value, it must fade itself (as the
   apply and its copies do).
5. TAA jitter is not undone (no constant); fine under TAA, a sub-pixel offset of the contact term otherwise.
6. `NvrDxvkKeepD3D9` is not checked by `ExtPingPong` (C++, unchanged).
7. `[ShadowMaps] Anisotropy` is dead in the C++ (unchanged).
8. Not measured in game (no game runs in this wave).
