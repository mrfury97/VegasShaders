# Underwater effect spec

Status 2026-11-02 (branch claude/rw3-lens): the D3D9 effect `src/hlsl/NewVegas/Effects/Underwater.fx.hlsl` and the four Vulkan passes (`Compute/UnderwaterGodrays.frag`, `UnderwaterBlur.frag`, `UnderwaterWater.frag`, `UnderwaterDistortion.frag`, shared `Underwater.glsl`) were written fresh from this spec. The text below describes the new shader. Fixed from the old one: caustic projection plane (now the water surface height, TESR_WaterSettings x, not the underwater flag z: open question 1); sun vertical component guarded (open question 3); no horizontal-view division (open question 4). TESR_FogColor is no longer declared (it was never used; open question 7). Not changed: the settings, the pass order, the wobble clock (open question 2).

Source read: `the source tree` (read only). Covers the D3D9 effect, its Vulkan (Vegas Shaders DXVK) port, and the settings it reads.

## 1. Purpose

When the camera is under water, or the player is in water, the picture is turned into what water looks like from inside. The colour drains toward the water's deep and shallow tints, distant scenery fades into that murk, and surfaces below the waterline shimmer and drift. Sunlit floors show moving caustic light (only where the point-light shadow map says the light reaches), and faint shafts of light fall through the water. The whole picture gets a very small wobble that reads as ripples. Anything above the surface is left exactly as it was before this effect ran.

## 2. When it runs

- Switched by `[_Shaders.Underwater.Status]` `Enabled` (bool, default true).
- Draws when the sky reports the camera as underwater (the game's sky flag) or the player is in water (the player-in-water flag). No other condition: the effect has no night, interior, exterior, menu or dialogue gate of its own.
- Interiors: it runs too. The water settings come from the interior section (section 4). The point-shadow buffer is cleared to "no shadow" indoors for this effect (ShaderManager, interior forward-shadow block).
- Night: no special rule. The sun vector and sun colour are used as they are at that time.
- Frame order (`src/core/ShaderManager.cpp`, scene-effects block): it runs after the lighting and shading effects (sun shadows, skin scattering, clouds, interior and exterior shadows, snow accumulation, ambient occlusion, ambient light, bounce light, blood flow, wet world, flashlight, specular) and after the combined bounce/specular step. It runs before reflections, volumetric fog, volumetric light, sandstorm, god rays, exposure, lens and LUT. The volumetric fog therefore hazes its output afterwards.
- It is listed in the effects that stop the interior forward-shadow fold from being used when it draws (`effectsBetween`).
- The render path checks the enabled state and ShouldRender. If either is false, the effect draws nothing and the frame is unchanged.

## 3. Inputs

### Textures (D3D9 file; sampler registers are fixed)

| Slot | Name | Holds | Filter / address |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | The current frame (the chain's image). Later passes read the god-ray image from it. | linear, clamp |
| s1 | TESR_SourceBuffer | The frame as it was when this effect started. The render path refreshes it for this effect because the effect samples it. | linear, clamp |
| s2 | TESR_DepthBuffer | Linear depth from CombineDepth (view distance divided by the far distance; the effect multiplies by the far distance). | linear, clamp |
| s3 | TESR_WavesSampler | Water normal texture `Water\water_NRM.dds`. Declared and loaded, but not read by any pass (kept so the C++ binding still fits). | anisotropic, wrap |
| s4 | TESR_NormalsBuffer | Per-pixel normals from the Normals effect (view space, packed to 0..1). | point, clamp |
| s5 | TESR_BlueNoiseSampler | `Effects\bluenoise256.dds` (noise for ray jitter). | point, wrap |
| s6 | TESR_CausticsSampler | `Water\caust_001.dds` (caustic pattern; red channel used). | anisotropic, wrap |
| s7 | TESR_PointShadowBuffer | Point-light shadow mask: red = the light reaches the point (caustics); green = a second shadow term for surface absorption. | anisotropic, wrap |

The depth, normals and point-shadow textures are the same objects other effects use: CombineDepth's depth, the Normals effect's normals, and the point-shadow buffer.

### Constants (D3D9 names; registers come from the constant table, not fixed)

Units: game units for distances and heights. Colours are the game's own values; the effect linearises the colour inputs itself.

- TESR_ReciprocalResolution: xy = 1 / frame width and height (texel size).
- TESR_SunDirection: xyz = unit vector to the sun (z up).
- TESR_SunColor: rgb = sun colour; w = sun glare (not read here).
- TESR_GameTime: x = game time (drives caustic drift); z = the frame value used for the wobble (see section 9).
- TESR_WaterCoefficients: xyz = light extinction per channel (from `inExtCoeff_R/G/B`); w = scattering amount (from `inScattCoeff`).
- TESR_WaterFog: x = near distance of the underwater fog; y = far distance (both from the water form's underwater fog values). z and w are not read.
- TESR_WaterVolume: x = caustic strength (from `causticsStrength`, multiplied by the weather's sun glare); y = shore factor (not read here); z = turbidity (from `turbidity`); w = god-ray strength (from `causticsStrengthS`).
- TESR_WaveParams: x = choppiness (not read by the active passes); y = wave width (not read by the active passes); z = wave speed (from `waveSpeed`; sets caustic drift and wobble rate); w = reflectivity (not read here).
- TESR_WaterSettings: x = water surface height (from the water height at the player); y = depth darkness (from `depthDarkness`); z = the underwater flag (0 or 1), read as the height the caustic projection is taken to (see section 9); w = refraction power (not read here).
- TESR_WaterShallowColor, TESR_WaterDeepColor: rgb = the water form's shallow and deep tints.
- TESR_HorizonColor, TESR_SkyColor: rgb = the game's horizon and zenith sky colours.
- TESR_FogColor: no longer declared in the new shader (it had no effect on the picture).
- TESR_CameraPosition: xyz = camera world position.
- TESR_CameraData: x = near distance (not read here); y = far distance (the depth scale).
- TESR_ProjectionTransform, TESR_ViewTransform: 4x4 matrices as the engine stores them.
- TESR_RenderScale (rect frames only): xy = the drawn part's share of the frame. Used by the coordinate helpers and the clips (section 6).
- Not read by this effect: TESR_WaterLODColor, the TESR_Placed* water constants, TESR_DebugVar.

The water constants are registered by the Water shader (`src/effects/Water/Water.cpp`). This effect has no constants of its own; its own constant block and settings update are empty. The effect's constants are refreshed from the Water shader's values even when the Water shader itself is disabled (ShaderManager, underwater check).

## 4. Settings

Settings the effect reads, by TOML section. The defaults file does not state ranges, so none are given.

| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| `[_Shaders.Underwater.Status]` | Enabled | bool | true | Switches the effect. |
| `[_Shaders.Water.Default]` | causticsStrength | float | 3.0 | Brightness of caustic light on sunlit floors (also multiplied by the weather's sun glare). High values wash the floor out. |
| `[_Shaders.Water.Default]` | causticsStrengthS | float | 0.2 | Strength of the god-ray shafts added to the water. |
| `[_Shaders.Water.Default]` | inExtCoeff_R, inExtCoeff_G, inExtCoeff_B | float | 1.0 each | Per-channel tint: how fast each colour is absorbed. |
| `[_Shaders.Water.Default]` | inScattCoeff | float | 1.0 | Light scattered into the murk. Higher = darker, less bright water. |
| `[_Shaders.Water.Default]` | depthDarkness | float | 1.0 | How much light is absorbed with depth (darkening of the bottom and far water). |
| `[_Shaders.Water.Default]` | turbidity | float | 3.0 | Murkiness. Higher = thicker fog and shorter sight distance. |
| `[_Shaders.Water.Default]` | waveSpeed | float | 0.7 | Speed of caustic drift and wobble. |
| `[_Shaders.Water.Interiors]` | depthDarkness | float | 10.0 | Depth darkness indoors. |
| `[_Shaders.Water.Interiors]` | turbidity | float | 1.0 | Murkiness indoors. |
| `[_Shaders.Water.Interiors]` | waveSpeed | float | 0.7 | Wave speed indoors. |
| `[Main.Main.Performance]` | NvrDxvkPasses | bool | false | Runs the Vulkan port instead of the D3D9 passes (section 7). |
| `[Main.Main.Performance]` | NvrDxvkKeepD3D9 | text | empty | Names effects that keep their D3D9 passes while NvrDxvkPasses is on. Listing this effect forces D3D9. |
| `[Main.Main.Water]` | RemoveUnderwater | bool | false | Removes the game's own underwater fog. The defaults file says it is not compatible with this effect. |

Keys in the Water sections that this effect does not read: choppiness, waveWidth, reflectivity, shoreFactor, shoreMovement, refractionPower.

Section choice: the water constants come from `[_Shaders.Water.Default]` outdoors and `[_Shaders.Water.Interiors]` indoors. Blood and lava water types use their own sections. Those sections are not in the defaults file (see section 9). The Interiors section does not define causticsStrength, causticsStrengthS, inExtCoeff_R/G/B or inScattCoeff, so indoors those values come from whatever the settings reader returns for a missing key (see section 9).

## 5. Passes and outputs

The C++ asks for technique index 0 only. The D3D9 technique is unnamed and has five passes (index 0 to 4). The Vulkan port has the same five passes in the same order.

| Pass | D3D9 pass index | Vulkan pass | Reads | Writes | Region |
|---|---|---|---|---|---|
| 1, god rays | 0 | UnderwaterGodrays | current image (outside the region), depth, blue noise, caustics | the chain's next image | upper-left quarter (the ray image) |
| 2, blur vertical | 1 | UnderwaterBlur, mask (0, 1) | current image | the chain's next image | upper-left quarter; the rest keeps the current image |
| 3, blur horizontal | 2 | UnderwaterBlur, mask (1, 0) | current image | the chain's next image | same as pass 2 |
| 4, water | 3 | UnderwaterWater | current image (blurred rays), source frame, depth, normals, caustics, point shadows | the chain's next image | whole frame |
| 5, wobble | 4 | UnderwaterDistortion | current image (the water result), source frame, depth | the game's render target (last pass, written directly) | whole frame |

Format: the chain's images and the target use the frame chain's format (not checked here). Alpha is 1 on every pass output.

Pass 1 detail: the ray image is computed at half resolution per axis and stored in the upper-left quarter of the frame. Its own 0 to 1 coordinates cover that quarter. Outside the quarter the pass returns the current image unchanged.

Passes 2 and 3 detail: a 12-tap blur along one axis, taps at 1 to 6 steps either side of the centre with the step set to two full-resolution texels (so the reach is up to 12 texels). The centre weight is about 0.115, and the twelve tap weights are fixed in the shader file. Taps that land outside the quarter are dropped and the weights renormalised. The blur only changes the quarter.

Pass 5 returns the source frame unchanged where the pixel is above water.

The source frame copy: the render path copies the current frame into the source buffer before this effect's passes (the Vulkan port does the same copy in its first pass).

## 6. Behaviour details

- Above-water test, per pixel. Let E = camera height, W = water surface height (TESR_WaterSettings x), P = the pixel's world height. If E is below W - 4, every pixel is treated as underwater. Otherwise, if E is above W + 4, the pixel is above water. Otherwise (camera near the surface), the pixel is above water only if P is above W - 0.6. Above-water pixels return the source frame in passes 4 and 5, so the effect is a no-op there. Passes 1 to 3 still compute (cost) but are not seen above water.
- Sky: there is no sky mask. Sky pixels read the far-plane depth, so the far fog treatment (fade to the fog colour past 5000 units) turns them into fog colour when the camera is under water.
- Ray reading: each full-frame pixel reads the god-ray image at half its own coordinates. The ray image covers the whole picture once, computed at half resolution per axis. The shafts follow the whole screen and are never seen as a quarter block.
- Water maths in words: the fog colour is the deep tint for the lower view and the shallow tint (plus a little sky) toward the horizon. It is scaled by the sun's brightness, a sun-direction factor (base 0.4, rising with alignment to the sun), and the extinction tint. A height fog that thins with height (density 0.08, falloff 0.00225, fixed in the shader) darkens water below the camera. The sky colour for the fog tint is built from the horizon and zenith colours with a sun-glow lobe (power 12).
- Caustics: a point is projected toward the sun onto the water surface plane (TESR_WaterSettings x; was the underwater flag, see section 9). The sun's vertical component is floored at 0.05 for the projection, and caustics and god rays fade out as the sun goes below about 0.17 (saturate(sun z x 6)), and the caustic pattern is sampled twice (two scales, the second 1.2 times the first, drifting in opposite directions at a speed from game time and wave speed). The lower of the two red values is raised to 1.3 and multiplied by 4. Caustic light is added only where the floor faces up and faces the sun (smooth steps on the normal), where it is close below the surface (fades to nothing 800 units down), within 6000 units of the camera, and where the point-shadow red channel is lit.
- Surface absorption uses the point-shadow green channel: lit water keeps its colour, shadowed water darkens toward the deep tint. The strength fades out by 6000 units.
- Scattering: the scattering amount (one minus the fog colour, times the scattering coefficient) mixes into the fog colour by a linear distance factor between TESR_WaterFog x and y. A thickness exponent from turbidity sets how sharp the falloff is, and a turbidity weight saturates as turbidity grows.
- Far fog: past about 5000 units a steep curve pushes the colour fully to the fog colour, so far geometry loses its detail.
- God-ray add: the ray value (red channel of the ray image) times the sun colour times the god-ray strength multiplies the colour. It is applied after scattering and before the encode step.
- Encode and dither: the colour is linearised at the start and encoded back at the end. A fixed 4x4 ordered-dither threshold pattern (values about 1/17 to 16/17) is added as a fraction of one 8-bit step, to hide banding. The pattern index follows the pixel position modulo 4.
- Wobble (pass 5): inside the water, the source sample is shifted by a small sine offset in x and a cosine offset in y, amplitude 0.001 of the frame. The phase is the wobble value times 3, plus the screen position on the other axis times 60, so the ripple pattern moves across the picture. In a rect frame the amplitude is multiplied by RenderScale's x and y (D3D9 only), so its on-screen size stays the same.
- Rect frames (RenderScale below 1): the depth and view-ray helpers map texture coordinates to the drawn part before reading. The god-ray pass and the blur are clipped to the drawn part. The Vulkan port is not used in rect frames (its pass check needs RenderScale at 1), so this is D3D9 only.
- Jitter (TAA, FSR, DLSS): the effect does no jitter correction. It reads depth and the current frame at the uv it is given. Not verified visually.
- Reversed depth: none. The depth input is CombineDepth's linear value; the effect never reads raw projected depth.
- HDR vs LDR: the effect works on sRGB-encoded values with linearisation at the boundaries. There is no HDR path. Values above 1 are not clamped after the god-ray add or caustic add, so bright results may clip at the encode step.
- NaN and infinity: the new shader has no division by the view ray's vertical component (the fog uses dot products only), and the sun's vertical component is floored at 0.05 in the caustic projection. The Vulkan side clamps the fog amount; the caustic power keeps the sign of its input.
- First-person weapon: no mask. Whatever is in the frame at this point is tinted and wobbled. Not verified.
- Interior underwater: the water values come from the interior section. Sun direction and colour indoors come from whatever the sky constants hold. Not verified.

## 7. Vulkan path

Files (`src/hlsl/NewVegas/Compute/`):
- `Underwater.glsl`: shared part (the std140 constant block, texel helpers, depth and normal helpers, caustic function, sky-colour function, above-water test). Each pass includes it after the common shader include.
- `UnderwaterGodrays.frag`: pass 1.
- `UnderwaterBlur.frag`: passes 2 and 3 (one file; the mask is pushed per pass).
- `UnderwaterWater.frag`: pass 4.
- `UnderwaterDistortion.frag`: pass 5.
- Compiled `.spv` files exist next to the four `.frag` passes. The shared `.glsl` is not compiled on its own.

Selection (the frame-chain effect draw path in `src/core/EffectRecord.cpp`): the Vulkan port runs only when all of these hold: `[Main.Main.Performance] NvrDxvkPasses` is on; the Vegas Shaders DXVK extension is available; no rect frame is active (RenderScale at 1); the effect is not named in `NvrDxvkKeepD3D9`; the technique is not covered by the SM6 switch; the effect has not failed earlier in the session; the target belongs to the frame chain; and the technique index is 0 with no target clear. If any check fails, the effect's Vulkan routine returns false and the D3D9 technique runs. Pipelines are created once, on first use, with the pass names above. If a pipeline, sampler or required texture is missing, the frame is left as it was and the D3D9 technique runs. Required textures for the port: the depth, normals and point-shadow textures, and the caustic and blue-noise file textures (the caustic texture is sampled with mips; the blue noise at level 0).

Constants: one uniform buffer (set 2, binding 0, std140), filled every frame from the same registered constants the D3D9 technique uploads, in this order: reciprocal resolution, sun direction, game time, water coefficients, sun colour, water fog, water volume, wave params, water settings, shallow colour, deep colour, horizon colour, sky colour, camera position, camera data (15 vec4s, 240 bytes), then projection (4x4, 64 bytes at offset 240) and view (4x4, 64 bytes at offset 304). Total 368 bytes. The matrices are copied as the engine stores them.

Bindings per pass:
- Godrays: set 1 bindings 0 to 3 = current image, depth, blue noise, caustics (sampled images). Set 2 binding 0 = constants. Samplers come from the sampler heap by index in push data: offset 0 = linear clamp, offset 4 = point wrap, offset 8 = anisotropic wrap. Push block 16 bytes: linearIndex (0), noiseIndex (4), causticsIndex (8), pad (12).
- Blur: set 1 binding 0 = current image; set 2 binding 0 = constants; sampler index at push offset 8 = linear clamp. Push block 16 bytes: offset mask as two floats (0 and 4), samplerIndex (8), pad (12).
- Water: set 1 bindings 0 to 5 = current image, source frame, depth, normals, caustics, point shadows. Set 2 binding 0 = constants. Samplers: offset 0 = linear clamp, offset 4 = point clamp, offset 8 = anisotropic wrap. Push block 16 bytes: linearIndex (0), pointIndex (4), anisoIndex (8), pad (12).
- Distortion: set 1 bindings 0 to 2 = current image, source frame, depth. Set 2 binding 0 = constants. Sampler: offset 0 = linear clamp. Push block 16 bytes: linearIndex (0), pad (4 to 15).

Differences from D3D9 that the port makes: the ray pass copies the current image outside the quarter with a direct read instead of the D3D9 pre-fill; the wobble is not scaled by RenderScale (the port is not used in rect frames); the ray region test is done directly, not with a clip.

## 8. Look and cost

Correct result:
- Murky water with a clear blue-green (or the water form's) cast, darker with depth and distance. Far scenery fades into the fog colour and loses detail.
- Sunlit floor shows soft, moving caustic net where the light reaches. In shade it is absent.
- Faint, moving light shafts over the whole picture, not confined to a corner.
- Subtle ripple wobble, no tearing or streaks.
- Above the surface: identical to the frame without the effect. Near the surface there is a short transition band (about 4 units either side of the water height), not a hard seam.
- No visible banding in the murk gradients.

Typical wrong results:
- Rays confined to one quarter of the screen, or a hard edge at the middle of the screen.
- Caustics far too bright, especially with a high causticsStrength on a bright (high sun glare) day.
- Sky visible under water as sky colour (it should go to fog colour).
- Surface pixels tinted from above the water.
- Tearing or streaks from the wobble.
- Murk too clean (scattering or turbidity missing), or too dark (depth darkness too high), especially at interior and exterior changes.
- Black or noisy pixels from divisions by zero at horizontal views or with the sun below the horizon.

Cost class: multi-pass (five passes), per-pixel loop. Pass 1 is a 49-step loop over the quarter of the frame, with two caustic reads and one noise read per step. Passes 2 and 3 are 12-tap blurs over the quarter. Pass 4 is a full-frame pass with about 10 texture reads (the caustic adds two more). Pass 5 is full-frame with about 3 reads. Passes 1 and 4 dominate. The passes run whenever the effect is enabled and its condition holds, even if most of the view is above water.

## 9. Open questions

1. FIXED 2026-11-02: caustic projection now uses the water surface height (TESR_WaterSettings x). The old use of z (the underwater flag) placed caustics wrongly at every water height.
2. TESR_GameTime z: the wobble phase multiplies this value by the wave speed and 1.5. That is a smooth clock only if z is an accumulating time. The Vulkan file calls it "frame time". Which is it?
3. GUARDED 2026-11-02: the projection divides by max(sun z, 0.05), and caustics and shafts fade out with saturate(sun z x 6). The sun vector at night and indoors is still not confirmed.
4. FIXED 2026-11-02: no division by the view ray's vertical component in the new shader.
5. Interior and fallback values: `[_Shaders.Water.Interiors]` does not define causticsStrength, causticsStrengthS, inExtCoeff_R/G/B or inScattCoeff. What does the settings reader return for a missing key? Same for the blood and lava sections, which are not in the defaults file.
6. Ranges for the water keys are not stated in the defaults file, and no clamping was confirmed in the reader.
7. Unused values (TESR_FogColor is now dropped from the shader; the others are unchanged): WaterVolume y, WaveParams x, y and w, WaterSettings w, and the water shader's shore and refraction values are not used by the active passes. Confirm no other pass depends on them before a new shader drops them.
8. The wave normal texture (sampler register 3) is still declared and loaded in the new D3D9 file but no pass reads it. Vulkan does not bind it. Whether the C++ needs it at all is still open.
9. Vulkan vertex stage: the Compute folder has no vertex shader; the full-screen stage is assumed to come from the Vegas Shaders DXVK extension. Confirm its name and coordinate convention.
10. First-person weapon and held items: whether they are drawn before this effect (and so get tinted and wobbled). The effect has no mask. Not checked.
11. The scene-effects block that calls this effect may have an enclosing condition beyond this effect's own. Not traced.
12. TAA, FSR and DLSS jitter: no jitter correction on the depth read. Whether this shows as shimmer on the wobble or caustics has not been checked in the picture.
13. Partial failure in the Vulkan port: if passes 2, 3 or 4 fail after pass 1 has drawn, the frame keeps a partial result, and the D3D9 technique would then run the god rays again on top. Whether this happens in practice is not known.
