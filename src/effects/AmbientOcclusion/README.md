# AmbientOcclusion (with Skylighting)

Source of this spec: the rewritten shader (wave 3 buffers, branch claude/rw3-buffers): src/hlsl/NewVegas/Effects/AmbientOcclusion.fx.hlsl (D3D9: the legacy path, DedicatedAO, DedicatedAOFast, DedicatedGTAO, DedicatedGTAOFast, SkyVisibilityV2), and src/hlsl/NewVegas/Compute/AmbientOcclusion.glsl (the shared Vulkan header, now used by the kept RT passes). The C++ (src/effects/AmbientOcclusion.cpp) is unchanged. Written from the previous spec and the C++; the old shader was not read. The Vulkan AO passes are not rewritten yet (section 7).

Skylighting is part of this effect: the sky pass, its constants and its composite live in the same file. The RT ambient occlusion path is a separate branch (section 9).

## 1. Purpose

Ambient occlusion darkens the parts of the picture that a real surface would receive less skylight and bounce light on: inside corners and creases, the underside of ledges, the base of props. It is soft, computed in screen space from the depth and normals, and applied to the frame before the rest of the chain. Bright surfaces are darkened less, and distant pixels fade back to fully lit.

Skylighting (outdoors, off by default) takes away the sky's share of the light where something overhead blocks the sky (under roofs, porches, bridges, at the foot of cliffs). It removes only the sky's share, so sunlit surfaces keep their sun.

## 2. When it runs

- Enabled by [Shaders.AmbientOcclusion.Status] Enabled, and the location's section (Exteriors or Interiors) Enabled. ShouldRender is that test.
- Frame order: depth and normals first; then sky and the other effects; then either AmbientLight (which hosts the sky pass) or this effect (deferred into the fog composite when possible); then AvgLuma and Exposure.
- Skylighting v2 sky pass (SkyVisibilityV2): once per frame before the AO passes, outdoors only. Skylighting v1 has no pass: it multiplies the estimate.

## 3. Inputs

### Textures (stage numbers as declared; the C++ binds them)

| Stage | Name | Holds |
|---|---|---|
| s0 | TESR_RenderedBuffer | the frame (read by the legacy path only) |
| s1 | TESR_DepthBuffer | CombineDepth output (linear depth, LINEAR, CLAMP) |
| s2 | TESR_SourceBuffer | the scene the combine darkens |
| s3 | TESR_BlueNoiseSampler | 256 x 256 blue noise (POINT, WRAP) |
| s4 | TESR_NormalsBuffer | view-space normals (POINT, CLAMP) |
| s5 | NVR_AOBuffer | the AO ping-pong texture (R = AO, G = depth) (LINEAR, CLAMP) |
| s6 | NVR_OrthoMap | top-down height map (LINEAR, CLAMP). The sky tests use it with "outside the map = open" done in the shader (see section 6) |
| s7 | NVR_SkyVis | Skylighting factor (R) and visibility (B) (POINT, CLAMP) |
| s8 | NVR_SunMask | sun shadow mask, read in R (POINT, CLAMP) |
| s9 | NVR_SkyPrep | AmbientLight's smoothed normal, encoded as the normals are (POINT, CLAMP) |

### Constants (by name)

- TESR_AmbientOcclusionAOData: x samples (unused: the kernel is five per axis), y StrengthMultiplier, z ClampStrength, w Range.
- TESR_AmbientOcclusionData: x AngleBias, y LumThreshold, z BlurDropThreshold, w BlurRadiusMultiplier.
- TESR_AmbientOcclusionGTAO: x GTAO on, w MaxRadius (pixels).
- TESR_AmbientOcclusionSky (v1): x on, y Strength, z Distance, w slack (unused).
- TESR_AmbientOcclusionSky2 (v2): x on, y Strength, z A (sky light, linear luminance x AmbientScale), w S (sun light).
- TESR_AmbientOcclusionSky3: x DepthNormals, y 1 / OpenThreshold, z ShareApply, w debug view (0 off, 1 visibility, 2 share, 3 factor, 4 1 - covered).
- TESR_AmbientOcclusionSky4: x ray reach (Distance), y SoftWidth, z sun mask bound, w sky factor bound (combine).
- TESR_AmbientOcclusionSky5: x LightCap take limit, y OpenSides, z unused, w no-limit value (1e6 when the limit is off).
- TESR_SmoothedSunDir (xyz), TESR_ShadowFade (x), TESR_ReciprocalResolution (xy), TESR_FogData (x start, y end), TESR_FogColor (rgb), TESR_CameraData (x near, y far).
- TESR_ProjectionTransform, TESR_InvProjectionTransform, TESR_InvViewTransform, TESR_ShadowCameraToLightTransformOrtho (matrices).
- TESR_RenderScale (RenderRect include), NVR_AOLayout (zw = 1 / the AO target's size), NVR_SkySmooth (x = 1 when NVR_SkyPrep holds a smoothed normal).

## 4. Settings

Unchanged (all read by AmbientOcclusion.cpp): the Exteriors and Interiors sections (Enabled, Samples, StrengthMultiplier, ClampStrength, Range, AngleBias, LumThreshold, BlurDropThreshold, BlurRadiusMultiplier); Status Enabled; GTAO (Enabled, MaxRadius); FastPaths (Enabled); Skylighting (Enabled, Version, Strength, Distance, ShareApply, OpenThreshold, DepthNormals, AmbientScale, SoftWidth, DebugView, Reflections, MinReflection, LightCap, OpenSides); AOLowRes; Max Quality; NvrDxvkPasses and NvrDxvkVariants; RT (separate, section 9).

(2026-11-07 perf-history, docs/next/RT-PATH-TRACING.md "perf-history") With `[Main.Main.RayTracing] DenoiseTimeBased` (default on) the RT
estimate's time behaviour is in seconds, as tuned at 60 fps: the RT AO temporal pass keeps 0.9 of last frame at 60 fps and 0.9^(60 dt) at
other frame rates (0.73 at 20 fps); `[Shaders.AmbientOcclusion.RT] CacheFrames` and `GIMovingHistory` and the RT GI / AO denoisers'
histories (RtDenoise, 16 frames) are frames at 60 fps, kept as the same seconds. At 60 fps nothing changes.

## 5. Passes and outputs

### Techniques

| Requested when | Technique | Passes |
|---|---|---|
| default | DedicatedAO | Estimate, BlurX, BlurY, Combine |
| FastPaths on, GTAO off | DedicatedAOFast | the same four |
| GTAO on, FastPaths off | DedicatedGTAO | the same four |
| GTAO on, FastPaths on | DedicatedGTAOFast | the same four |
| Skylighting v2 | SkyVisibilityV2 | Sky |
| the legacy path (unnamed technique 0) | (unnamed) | one pass (new: was five) |

### Dedicated passes

Targets are G16R16F at the AO size (half the frame, a quarter with AOLowRes, full with Max Quality). Ping-pong AO 0 and AO 1.
- Estimate -> AO 0: R = AO, G = linear depth (game units) capped at 8000. Pixels beyond 8000 give R = 1 and G = 8000.
- BlurX: AO 0 -> AO 1. BlurY: AO 1 -> AO 0. Each keeps G (depth).
- Combine -> the frame (or the chain's spare texture): the scene darkened in linear light (section 6).
- Deferred and combine-only modes use the same passes (the C++ picks them).

### Legacy path (unnamed technique)

One pass from TESR_RenderedBuffer: the random-sample estimate, then the same combine without Skylighting. The old five-pass legacy (SSAO, multiply, two blurs, combine) is not kept.

### Sky pass (SkyVisibilityV2)

Target: the sky target, the AO size. G16R16F, or A16B16G16R16F when Reflections or LightCap is on.
- R = the sky factor (1 = no change). G = linear depth capped at 8000. B = the sky visibility V (1 open, 0 blocked). A = 1.

## 6. Behaviour details

### Estimate

- The pixel's view position from the depth; the normal from the normals buffer.
- Random-sample estimate (AngleBias, Range, StrengthMultiplier): five samples per axis from the blue noise. Per sample i, the blue-noise vector r (in -1..1) gives a horizontal direction from its (x, z) components and a vertical direction from its (y, z) components, both turned by the pixel's quarter-turn angle (blue-noise value x pi/2) and flipped to the normal's side. The sample point is origin + direction x scale x Range, scale = lerp(AngleBias, 1, ((i+1)/5)^2). A sample occludes when the stored depth there is nearer than the sample and within Range; its weight falls off linearly for the first two samples (the first quarter) and as the square for the rest. Each axis gives 1 - StrengthMultiplier x (sum / 5); the AO is the product of the two axes.
- GTAO (when on): two slices across the half turn (slice angle from the blue noise), four horizon steps each way out to min(Range in pixels, MaxRadius). Horizons are limited to the normal's hemisphere; the cosine-weighted visibility between them is the AO, with samples past 0.385 of the radius counting less. Result = 1 - StrengthMultiplier x (1 - visibility).
- Skylighting v1 (Sky.x on, outdoors): multiplies the estimate by lerp(1, open / total, Strength), where the rays are one straight up and six on two rings (60 and 30 degrees, turned by the blue noise), each probed at a third and at the full Distance, weighted by the cosine to the normal.
- Fog: AO = max(AO, lerp(AO, luma(fog colour), fog amount)); the fog amount is linear from FogData.x to FogData.y, saturated. Fog only lightens.
- Fade: AO = lerp(AO, 1, saturate((viewZ - 2000) / 6000)). Beyond 8000, AO = 1.

### Blur (BlurX, BlurY)

- Seven taps per side at the Gaussian weights exp(-k^2 / 18), k = 0..6 (a centre tap and six pairs), along the pass's axis.
- Step in pixels = min(BlurRadiusMultiplier x Range / viewZ, 8). Offsets are in screen texels.
- A tap counts when its depth is within BlurDropThreshold x (1.5 + n_z) of the centre's, n_z being the centre normal's z.
- Beyond 8000 the input is returned unchanged.

### Combine

- Edge-aware upsample: the 2 x 2 AO grid neighbours, each weighted by its bilinear weight over (1 + |its depth - the full-res depth| / max(BlurDropThreshold, 0.001)); the same weights read the sky factor and visibility.
- AO = lerp(ClampStrength, 1, AO). Brightness: AO = lerp(AO, 1, saturate((Y - LumThreshold) x 3)), Y = BT.709 luminance of the linear scene.
- Skylighting composite (sky factor bound): take = saturate(min(1 - factor, ((1 - visibility) x LightCap-limit + no-limit) / Y)); the linear colour is multiplied by AO and by (1 - take); delinearised. Without the sky factor, take = 0.
- Beyond 8000 the scene is returned unchanged.

### Skylighting v2: the sky pass

- Beyond 8000: factor 1, visibility 1.
- Normal: the normals buffer; or from the depth (DepthNormals, each axis from the side with the smaller depth step, oriented toward the camera); or the smoothed normal from NVR_SkyPrep (NVR_SkySmooth.x).
- Start point: the view position moved to world space, pushed along its world normal by 24 units.
- Map test (Openness): the point's ortho-map coordinates (the transform's xy in -1..1 mapped to 0..1); outside the map: open. Open where the map's top (R of NVR_OrthoMap) is below the probe's depth in map units; soft over SoftWidth game units times the transform's z scale: saturate((probe - top) / (SoftWidth x zScale) + 0.5).
- Rays: one straight up, weight 0.034 x max(normal.z, 0). Eight azimuths at 22.5 + 45k degrees, each with a 60-degree ray (weight 0.0324 x cosine) and a 30-degree ray (weight 0.0884 x cosine). A ray is open when its probes at 40% and 100% of Distance are both open (the minimum).
- Covered: 1 - openness of a probe 32 units above the point. For a covered point, the 30-degree rays are replaced by a horizontal probe at full reach, by OpenSides x covered.
- Visibility = weighted average of open rays. Edge fade: outside the map's middle 80% (|x| or |y| above 0.8) the visibility fades to 1 over the outer 20%. Then multiplied by 1 / OpenThreshold and saturated.
- Sun share (ShareApply): sunE = S x saturate(normal . sun direction) x sunMask, sunMask = lerp(ShadowFade.x, 1, the sun mask) when bound, else 1; share = A / (A + sunE). Off: share 1.
- Factor = saturate(1 - Strength x (1 - visibility) x share). Debug views replace it: 1 visibility, 2 share, 4 1 - covered.

### Fixed in this rewrite (from the old spec's open items and the brief)

- Blur divisor, reversed depth (old open question 2): the old spec divided by one minus the post-projection depth, so pixels near the camera got huge offsets. The step is now BlurRadiusMultiplier x Range / viewZ, capped at 8 pixels per step (BlurRadiusMultiplier still scales it linearly). This changes the blur radius on near surfaces; check in the game.
- Divide-by-zero guards: the blur weight sum (1e-6), the upsample depth term (BlurDropThreshold floored at 0.001), the combine weight sum (1e-6), the sky share sum (1e-6), the GTAO and visibility sums, and the view positions (viewZ floored).
- Sky map border: done in the shader (outside the map is open) instead of a separate border sampler, so the stage 6 sampler is plain clamp.
- Openness sign: the old spec's formula, read literally, gives closed for open points. The rewrite uses the meaning the text states (open where the map's top lies below the probe).
- Legacy path: reduced to one pass (old open question on its fallback).

### Interpretations (not verified in game)

- The ortho map's R value and the transform's z are taken to share units; the z scale is the length of the transform's z column. The old spec did not say.
- The random-sample kernel's exact form (which vector components form each one-dimensional estimate, and the rotation) is a reading of the spec's description.
- The blue-noise sample offsets (37 and 71 texels per sample index) are chosen here.

## 7. Vulkan path

- Not rewritten in this pass. The AmbientOcclusionEstimate, Blur, Combine and Sky frag files and their variant lines in Compute/variants.txt are not present, so the Vulkan AO path is not active and the D3D9 passes run. Compute/variants.txt still lists AmbientOcclusionEstimate g0 and g1, which makes a full compile-compute.sh run fail until those frags exist (it stops at the first missing file).
- AmbientOcclusion.glsl (new): the shared header for all AO passes. Uniform block cb (set 2, binding 0, std140) with the members in the order the C++ fills them (projection, inverse projection, inverse view, ortho transform, aoData, data, gtao, sky, sky2 to sky5, smoothedSunDir, shadowFade, reciprocalResolution, fogData, fogColor, depthConstants, cameraData, aoLayout, skyLayout, rtReproject, rtTemporal). Push block aoPush (24 bytes: axis at 0, gtaoEstimate at 8, linear clamp index at 12, point clamp index at 16, point wrap index at 20). Macros LINEAR_LOD and POINT_LOD, the camera plane macros (nearZ, farZ), AOstrength, startFade and endFade, and the helpers invlerp, SkyFactor and projectedDepthFromLinear. These are the names the kept RT passes use.
- The four kept RT AO passes (AmbientOcclusionEstimateRT, AmbientOcclusionSkyRT, AmbientOcclusionTemporalRT, RtAOInput) compile against this header.
- (2026-11-07 rtgap-tess, docs/next/RT-DETAIL-GAPS.md section 1a) RtAOTrace.comp's AO and sky rays start the tessellated bumps' Strength
  (faded by view depth as the bumps fade) further off the surface while the bumps are drawn (`DxvkCompute::DisplaceRtSlack`,
  `[Main.Main.RayTracing] TessStartSlack`): a point in a hollow of the depth buffer lay under the flat RT surface (covered sky, rays
  started inside the floor). The 512-byte block is full: the slack rides in viewToWorld's 4th row (_41 Strength, _42 FadeStart, _43
  FadeEnd; the view's directions have w 0, so nothing else reads it). The GI bounce pass (RtGITrace) is unchanged. RT host `tessslack`.
- RT AO / sky / GI (the helper's RtAOTrace.comp, `[Shaders.AmbientOcclusion.RT]`), 2026-11-07 decals (docs/next/RT-DETAIL-GAPS.md 5):
  the core's decals (`[Main.Main.RayTracing] Decals`, custom index NVR_RT_CUSTOM_DECAL) never occlude: the AO and sky rays (Trace) and
  the GI hits' shadow rays (Visible) pass them (ProceedShadow); the GI bounce rays see them. AOAlpha 1 (cut-outs solid, traced with the
  opaque flag, which would make a decal a solid quad) is sent to the shader as 3 while `RtCore::DecalsTraced()`: the cut-outs come as
  candidates and are confirmed whole, decals passed (ProceedSolid; a little dearer than 1 where many cut-outs are hit). RT host
  scene `decals` (AOAlpha 2 and 3 against the CPU, the decal card never covers).

## 8. Look and cost

Correct: soft contact darkening in corners and under ledges; no halo at silhouettes (the upsample and blur are depth-aware); sunlit faces barely touched; distant land untouched beyond 8000 with a soft fade from 2000; under a porch the sky's light goes, the sun on the same surface stays; a lamp-lit wall under a wide cover keeps its lamp light.

Typical wrong: flat open ground greyed (random samples under the surface: GTAO avoids it); haloes at depth edges (the tolerance); speckle (too few samples surviving the blur); lamp-lit walls going black under a canopy (LightCap too low); a visible band where the fade begins.

Cost: estimate at half size (five sample pairs), two blurs of twelve taps, a combine of four taps (plus four sky taps); the sky pass once a frame outdoors (about 40 map reads).

## 9. Open questions

1. RT ambient occlusion: [Shaders.AmbientOcclusion.RT] is not in the D3D9 effect; the RT passes are the kept Vulkan files. Not specified here. Decide whether the new D3D9 file needs its own RT techniques. At RenderScale < 1 (FSR / DLSS LowResEffects) the RT AO, sky and GI traces cover the frame's top-left rect (RtRecord: the trace grid = RenderRect::Width/Height of the half grid; the GI reuse history restarts on a scale or rect <-> native change), the RT estimate runs inside the rect-aware D3D9 passes and the GI bounce reaches the picture through AmbientLight's D3D9 GI path; details and measurements in docs/next/RT-RENDERSCALE.md (phase 1 AO 2026-11-03, phase 2 GI 2026-11-04). (2026-11-07) RT GI's bounce hits (Compute/Rt/RtAOTrace.comp Shade, also its RtGITrace / RtRadianceCache variants) are lit with the hit's normal bent by its own normal map (ShadingNormal: the core's [Main.Main.RayTracing] RealNormalMaps, default on with RealTextures; the flat triangle normal stays for the shadow rays' lift); RT host scene `normalmaps`.
2. Skylighting v1 and v2: both are kept (the C++ selects by Version). v1 darkens the whole pixel. Whether v1 should stay is open.
3. Vulkan AO passes: not written (section 7). Needed for the Vegas Shaders DXVK path and for the batch compile to succeed again.
4. The ortho map's height units and the transform's z scale (section 6) need checking against ShadowsExteriors' map.
5. Samples is read but unused (the kernel is fixed at five).
6. The sun mask is read in its R channel only.
7. bNVAOLoaded is declared in the C++ and never set; the check never blocks.
8. The blur step cap (8 pixels) and the constant of the blur radius are chosen here; tune in game if the blur looks too strong or too weak near the camera.
