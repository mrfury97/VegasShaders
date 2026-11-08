# VolumetricFog - effect spec (rewritten 2026-11-02 for the wave-3 shader on branch claude/rw3-fog-vf)

Describes the shader in `src/hlsl/NewVegas/Effects/VolumetricFog.fx.hlsl` and its Vulkan passes `src/hlsl/NewVegas/Compute/VolumetricFog.glsl`,
`VolumetricFogEstimate.frag`, `VolumetricFogApply.frag` (written from this spec's first version and `src/effects/VolumetricFog.cpp/.h`; no
upstream shader code was read). Values are the shipped defaults from `resource/VegasShaders.defaults.toml` unless a code clamp
or fallback is stated. The defaults file writes its section headers with a leading underscore (`[_Shaders.VolumetricFog.Main]`); the
code asks for the same sections without it. Distances are game units (70 units = 1 m).

---

## 1. Purpose

The fog is the atmospheric haze over the whole picture. It blends each pixel toward a fog colour according to how far away it is, how
high it sits and how high the camera is, the weather, the time of day and a drifting density pattern, so distant terrain fades into the
sky and the air has patches of thicker fog that drift with the wind. Sunlight in the fog is shaded by the sun's shadow (the cascades, or
the volumetric light's froxel grid when it is on), so fog in shade is darker and glows toward a low sun. Near the horizon it also hides
the join between terrain and sky. It is the last large picture-wide effect before the volumetric light, sandstorm, bloom and the rest of
the post chain, and its pass can also apply the sun-shadow, interior darkening, AO and volumetric-light composites that come right
before or after it, saving their own full-screen passes.

## 2. When it runs

**Registration and enable.** Registered as `VolumetricFog`; the on/off switch is `[_Shaders.VolumetricFog.Status] Enabled` (default
true, read by the generic effect loader).

**Global gates (ShaderManager).** Nothing runs when the main render-effects switch is off, when the player has no parent cell, or while
the overlay (terminal / lockpicking) background is showing.

**Effect gate.** `ShouldRender` is false only when the camera is underwater. Render also returns at once when the effect is not enabled
or the effect file did not load.

**Exterior and interior.** `GameState.isExterior` picks the settings section (`Main` outdoors, `Interiors` indoors). Outdoor-only keys
are forced to 0 indoors by the C++ (see 4). The shader's `isExterior` (TESR_VolumetricFogWeather.y) is 0 indoors, so the sun, sky SH,
physical sky, aerial tint, horizon fog, EdgeAA, night terms, game fog floor, height layer and froxel-grid air all drop out; indoors the
fog's sky colours are the cell's fog colour (the C++ comment in UpdateSettings), so the shader uses TESR_FogSkyColor for all of them.

**Day and night.** `nightShare` = isExterior x (1 - smoothstep(0.4, 0.8, sun amount)): 0 by day and indoors, 1 at night. Every Night key
fades in with it. The sunlight terms use `dayFade` = smoothstep(0.1, 0.6, sun amount).

**Moon.** C++ derives the night ambient floor (TESR_VolumetricFogGlobal.z) from the days passed and the moon's phase length (the same
cosine as the exterior shadows) times 1 - `[Shaders.ShadowsExteriors.Main] NightMinDarkness`.

**Weather.** `WeatherFilterBlend` (TESR_VolumetricFogWeather.x, animated over 0.1 game hours) drops to 0 when it rains with
RainyDisablesSkyFilter on or is cloudy with CloudyDisablesSkyFilter on; at 0 the sky dome is fogged too (overcast look).

**Frame order (ShaderManager.cpp).** After the physical sky table and the volumetric clouds; after AmbientLight / AO and the sun-shadow
apply (which may be folded into the fog pass); after underwater and reflections; after the volumetric light's froxel grid when its
Froxels mode is on (technique 8). Before the volumetric light's composite (skipped when the fog did it: `gridApplied`), the sandstorm,
TAA, bloom and the later effects.

**Composite folding.** When nothing between them reads the picture, ShaderManager leaves the sun-shadow apply, the interior darkening
(InteriorShadowFold) and the deferred AO combine to the fog: technique CompositeFog / CompositeFogInterior applies them in the
reconstruct pass (shadows or interior darkening, then AO and the sky take, then the fog). If the fog fails, ShaderManager runs the
separate passes and the fog again. When effects between them keep the composite out, ApplyShadowAO does the shadow and AO in one pass.

## 3. Inputs

### 3a. Textures and samplers

TESR_ samplers bind by their declaration order (s0..s5); NVR_ ones are bound by the C++ to fixed stages. Every read is an explicit
level-0 read (tex2Dlod / tex3Dlod) so it is legal inside dynamic branches.

| Stage | Name | Holds | Declared filter / address | Read by |
|---|---|---|---|---|
| s0 | TESR_SourceBuffer | The scene (gamma-encoded HDR). The dedicated, composite and ShadowAO passes bind the current image here; the full-res and packed passes the chain's copy | LINEAR, CLAMP | all passes |
| s1 | TESR_RenderedBuffer | Packed path only: the packed coefficients in its top-left quarter | POINT, CLAMP | PackedFog apply |
| s2 | TESR_DepthBuffer | CombineDepth: R = view z / far (1 = sky) | LINEAR, CLAMP | all fog passes, composites |
| s3 | TESR_ShadowAtlas | Sun cascade atlas (VSM / EVSM2 / EVSM4 / PCF by TESR_ShadowFormatData.x) | LINEAR, CLAMP | fog sun visibility |
| s4 | TESR_NormalsBuffer | View-space normals (n x 0.5 + 0.5); turned to world with GetWorldNormalLod | LINEAR, CLAMP | cascade normal offset, water test |
| s5 | TESR_PointShadowBuffer | Shadow buffer: R sun term (indoors the lamp mask), G lamp term | LINEAR, CLAMP | sun composite, interior composite |
| s6 | NVR_FogBuffer | Half size: fog multiply (rgb) and the depth it was made at (a) | POINT, CLAMP | reconstruct |
| s7 | NVR_FogBufferAdd | Half size: fog add (rgb) | POINT, CLAMP | reconstruct |
| s8 | NVR_AOBuffer | AO result: R AO, G its depth in game units | LINEAR, CLAMP | AO composite |
| s9 | NVR_FogNoise | Data\Textures\Effects\NoiseVolume.dds, green = fractal noise | LINEAR (mips), WRAP u/v/w | density noise |
| s10 | NVR_FogSkyView | Physical sky table TESR_SkyViewLUT (128 x 64), bound only while SkyView draws it | LINEAR, CLAMP | PHYSICAL_SKY_ON builds |
| s11 | NVR_FogGrid | The volumetric light's froxel grid (8 x 8 atlas of 64 slices), bound only in RenderDedicated | LINEAR, CLAMP | estimate (FogVolume), reconstruct (VL composite) |
| s12 | NVR_SkyVis | Skylighting v2: R sky factor, B sky visibility (AO target size) | POINT, CLAMP | AO composite sky take |
| s13 | NVR_SunFar | SunFar's land / cloud shadow factor (rgb) | POINT, CLAMP | sun composite |
| s14 | NVR_FogLamps | LampHaze's lamp table, 16 x 2 A32B32G32R32F (row 0: lamp relative to the camera + reach, row 1: colour linear x dimmer), bound only while LampHaze is on this frame | POINT, CLAMP | estimate, full and combine passes (LampHazeLight) |

### 3b. Constants

**The fog's own (RegisterConstants; components as the C++ fills them):**

| Name | Components |
|---|---|
| TESR_VolumetricFogDensity | x BaseDensity, y WeatherImpact, z MorningFogDip (0 indoors), w SunriseSunsetBoost (0 indoors) |
| TESR_VolumetricFogShape | x HeightFalloff, or -1 / LayerThickness while HeightLayer is on; y MaxHeight, or the layer's base in world z (camera z added with LayerFollowsCamera, every frame); z Extinction; w Inscattering |
| TESR_VolumetricFogWind | xy cos / sin of WindAngle, z WindSpeed, w NoiseScale |
| TESR_VolumetricFogScatter | x PhaseAsymmetry (0 indoors), y ShadowStrength (0 indoors), z NoiseStrength, w HeightInfluence |
| TESR_VolumetricFogWeather | x WeatherFilterBlend, y isExterior, z the Sky effect is on (its SH is fresh), w FogSaturation |
| TESR_VolumetricFogAerial / AerialTint | AerialStrength, AerialRangeStart, AerialTintBlend, AerialDayFadeStart / manual tint rgb (all 0 indoors) |
| TESR_VolumetricFogDistant | x DistantFogRange, y DistantFogBlend, z DistantFogHeight, w EdgeAA (all 0 indoors) |
| TESR_VolumetricFogGlobal | x Amount, y Night DisableSkyMask, z moon night floor, w MinDensityFloor (0 indoors) |
| TESR_VolumetricFogNight | x DensityScale, y AmountScale, z HeightFalloffScale, w MaxHeightOffset |
| TESR_VolumetricFogNightScatter | x NoiseStrengthScale, y WindSpeedScale, z ExtinctionScale, w InscatteringScale |
| TESR_VolumetricFogNightTint | xyz night tint, w NightAmbientStrength |
| TESR_VolumetricFogLamps | x LampHaze (0 = off: outdoors, LampHaze 0, LampHazeMaxLamps 0, ClassicLook, LegacyFog, no lamp listed), y lamps in NVR_FogLamps, z LampHazeAnisotropy, w LampHazeCore |
| TESR_VolumetricFogSun | x SunGlow (0-2; 0 indoors), y WeatherFogColor (0 indoors), z GameFogFloor this frame (its Night scale faded in; 0 off / indoors), w the weather's raw fog near |
| TESR_VolumetricFogLow / High / Simple / Blend / Height / Data | Legacy keys (filled only while LegacyFog is on): Low x FogSaturation y WeatherImpact z DistantFogRange w SunPower; High x HeightFogDensity y HeightFogFalloff z HeightFogDist w HeightFogSkyColor; Simple x Extinction y Inscattering z FogNight (1 indoors) w SimpleFogSkyColor; Blend x DistantFogBlend y HeightFogBlend z HeightFogRolloff w SimpleFogBlend; Height x DistantFogHeight y HeightFogHeight z SimpleFogHeight w isExterior; Data x MinimumBaseFog y HeightFogSaturation z Amount w HeightFogInscattering |

**Registered by other systems:** TESR_FogData (x near, y far, z sun glare, w fog power), TESR_FogWeatherColor, TESR_FogHorizonColor,
TESR_FogSkyLowColor, TESR_FogSkyColor (the fog's copies with the sky look; Sky.cpp UpdateFogSky), TESR_ReciprocalResolution,
TESR_GameTime (x ms), TESR_SmoothedSunDir, TESR_SunPosition, TESR_SunColor, TESR_SunsetColor, TESR_SunAmbient, TESR_SkyData (x
atmosphere), TESR_SunAmount (x), TESR_PBRData (z direct scale, w ambient scale), TESR_SkyIrradiance[9] / TESR_SkyIrradianceGame[9]
(order-2 SH with band weights and 1/pi folded in: evaluating them gives radiance), the four cascade matrices and centres,
TESR_ShadowFormatData, TESR_ShadowBlur, TESR_ShadowFade (x sun-shadow fade, y maps on, z lamp shadows on, w interior distance),
TESR_ShadowData (y Darkness), TESR_WaterSettings, TESR_AmbientOcclusionAOData (z floor), TESR_AmbientOcclusionData (y LumThreshold,
z BlurDropThreshold), TESR_VolumetricLightData (x strength, y darkening, z air density, w march distance), TESR_VolumetricLightData2
(y g, z fade), TESR_VolumetricLightGridLayout (xy cells, zw atlas texel), TESR_VolumetricLightGridData (x FogVolume sums written, y lit
scale), TESR_PhysSky / TESR_PhysSkySun (declared only with PHYSICAL_SKY_ON: the C++ uses that parameter's presence to know the build),
TESR_CameraPosition, TESR_CameraData, TESR_ViewTransform, TESR_ProjectionTransform, TESR_RenderScale, and Depth.hlsl's other names.

**Set per draw by the C++:** NVR_FogLayout (xy half / full extent, zw 1 / half size), NVR_CompositeFlags (x sun shadow, y AO, z sky
visibility, w the interior fold's 16-bit rounding: 0 nearest even, 1 none, 2 toward zero), NVR_CompositeAOTexel (xy 1 / AO size, zw
SkyTake cap and no-limit), NVR_FogGridFlags (x grid bound, y the volumetric light's composite is done here; reset to 0 after the
dedicated draw), NVR_SunFarFlags (x SunFar bound).

Units: linear light with the piecewise sRGB curve for the fog; the shadow / interior / AO composites use the 2.2 power of the passes
they replace.

## 4. Settings

"Ext" = read outdoors only (forced to 0 indoors by the C++); "Int" = read from both sections. Clamps are the code's.

### 4a. `[Shaders.VolumetricFog.Main]` (and `Interiors` for the Int keys)

| Key | Range | Default | What it does in this shader |
|---|---|---|---|
| ClassicLook | 0-2 | 0 | 1: the *Classic techniques: noise sampled at the surface point (3 octaves), sun shadow at the surface, the game's SH sky, SunGlow fixed at 1, no physical sky, no WeatherFogColor, no grid air, no game fog floor. 2: VolumetricFogClassic at full resolution (C++). LegacyFog wins. Int. |
| FogVolume | bool | true | Read by the volumetric light (TESR_VolumetricLightGridData.x). With the grid bound, outdoors, modern look: the noise and the sun visibility come from the grid's height-weighted air sums along the view (the part inside the march distance), the rest from the four-point march / the surface. |
| Amount | max 0 | 0.4 (Int 0.25) | Final strength: mul = lerp(1, mul, a), add x a, a = saturate(Amount x lerp(1, AmountScale, night)). Int. |
| FogSaturation | max 0 | 1.7 (Int 0.5) | lerp(luma, colour, FogSaturation / (1 + density)). Int. |
| BaseDensity | max 0 | 0.2 (Int 0.3) | Vegas Shaders' own density. Int. |
| WeatherImpact | max 0 | 0.5 (Int 0) | Adds the weather's density (from its fog near / far), pulls the fog power toward the weather's (lerp(1, FogData.w, WI), as the froxel grid), and blends the colour toward the weather side. Int. |
| GameFogFloor | 0-2 | 0 | Ext, modern only. After Amount, tops coverage up to the game's own fog curve x this (raw near, far, power), never on the sky, with the fog colour without glow. |
| WeatherFogColor | 0-1 (missing 1) | 1 | Ext, modern only. Share of the weather's fog colour (instead of the sky toward the horizon) in the weather side; the sky dome keeps the sky colour. |
| MorningFogDip | 0-1 | 0.5 | Ext. Base density x (1 - dip x sun height x dayFade). |
| SunriseSunsetBoost | max 0 | 0.3 | Ext. Density + boost x 4 a (1 - a), a = sun amount. |
| HeightFalloff | min 1e-4 | 50 | Scale of the height curve exp(-k (z - base)), k = 1.5e-4 / (fogPower x HeightFalloff x lerp(1, HeightFalloffScale, night)) (the grid's formula). Higher = thins more slowly. Not used while HeightLayer is on. |
| MaxHeight | - | 3 (Int 0) | Height curve base = -10000 + 1000 x (MaxHeight + MaxHeightOffset x night). |
| HeightLayer, LayerBase, LayerThickness, LayerFollowsCamera | | off, 0, 2000, off | Ext, modern (the C++ writes Shape.xy only then). Density 1 below the base, exp(-(z - base) / thickness) above, averaged along the view in closed form. |
| HeightInfluence | 0-1 | 1 (Int 0) | Blend between flat (0) and the height path mean (1); also weighs the noise taps. Int. |
| Extinction | max 0 | 3 (Int 0.4) | Optical depth per unit = density x Extinction x 0.1 x 1e-4. Int. |
| Inscattering | max 0 | 1.6 (Int 0.35) | Albedo = saturate(Inscattering / Extinction). Int. |
| WindAngle, WindSpeed | | 45, 0.3 (Int 0, 0.1) | Noise drift = (cos, sin) x speed x lerp(1, WindSpeedScale, night) x game ms x 0.002 units. Int. |
| NoiseScale | min 1e-4 | 1 | Noise tile = 6000 x NoiseScale units (4 cells of 1500, as the grid). Int. |
| NoiseStrength | 0-1 | 0.7 (Int 0.5) | Density x max(1 + k (2n - 1), 0), n the contrast-stretched noise: mean-preserving. Int. |
| PhaseAsymmetry | 0-1 | 0.6 | Ext. Henyey-Greenstein g of the glow lobe (0 = no glow). |
| SunGlow | 0-2 | 0.25 | Ext. Glow = SunGlow x 2 x (HG(g) - 1)+ (HG capped at 8) x (1 - sun height)^2 x (1 + glare x depth) x dayFade. |
| ShadowStrength | 0-1 | 1 | Ext. Blend of the sun visibility; 0 skips every shadow read. |
| RainyDisablesSkyFilter, CloudyDisablesSkyFilter | bool | true | Int. Drive WeatherFilterBlend (C++). |
| AerialStrength, AerialRangeStart, AerialTintBlend, AerialDayFadeStart, AerialTintR/G/B | | 0.5, 0.3, 0, 0.85, 0.6/0.7/1 | Ext, not legacy. mul x lerp(1, tint, s), tint = lerp(sky colour normalised to max 1, manual, blend), s = strength x smoothstep(start, 1, depth) x smoothstep(0.1, fadeStart, sun amount), not on the sky. |
| DistantFogRange, DistantFogBlend, DistantFogHeight | | 0.9, 0.3, 0.3 | Ext. Horizon fog toward the horizon colour: sqrt(smoothstep(exp(-4 range), 1, depth)) x blend x heightFade, heightFade = min(1, exp(-z / (80000 x height))), or 1 - sky with height 0. |
| EdgeAA | max 0 | 0.4 | Ext. Land pixels touching the sky blend toward the horizon colour by EdgeAA / 2 x the same distance ramp. |
| MinDensityFloor | 0-1 | 0 | Ext. Optical depth never below this share of the flat (height-free) one. |
| LegacyFog | bool | false | The *Legacy techniques (see 6, legacy). Int. |
| Legacy keys | | | MinimumBaseFog, SunPower, HeightFogDensity, HeightFogFalloff, HeightFogDist, HeightFogSkyColor, HeightFogHeight, HeightFogBlend, HeightFogRolloff, HeightFogSaturation, HeightFogInscattering, FogNight, SimpleFogSkyColor, SimpleFogHeight, SimpleFogBlend: used as described in 6 (legacy). |

### 4b. `[Shaders.VolumetricFog.Night]` (all fade in with nightShare; outdoors only)

GameFogFloorScale (C++), DensityScale (base density only), AmountScale, HeightFalloffScale (multiplies HeightFalloff), MaxHeightOffset,
NoiseStrengthScale, WindSpeedScale, ExtinctionScale, InscatteringScale, TintR/G/B (multiply the fog colour), NightAmbientStrength (x the
moon floor on the ambient), DisableSkyMask (the sky dome takes fog at night).

### 4c. `[Shaders.VolumetricFog.Interiors]`

The Int keys with indoor values, and the legacy keys. The Ext keys are forced to 0 (C++). Indoor-only keys (UNOFFICIAL 2026-11-03 LampHaze,
read by UpdateSettings, live; a missing key takes the default):

| Key | Range | Default | What it does in this shader |
|---|---|---|---|
| LampHaze | 0-4 | 0.5 | Strength of the lamps' light in the fog's air (6, LampHaze). 0 = the term is skipped by a uniform branch: the fog exactly as before. |
| LampHazeAnisotropy | 0-0.9 | 0.3 | Henyey-Greenstein g of the lamps' light in the haze (x 4 pi, capped at 8): brighter looking toward a lamp. |
| LampHazeCore | 0.05-1 | 0.25 | The glow's bright core e as a share of the lamp's reach (as [Shaders.PBR.LampFalloff] Falloff). |
| LampHazeMaxLamps | 0-16 | 8 | At most this many lamps, nearest sphere first. 0 = off. |

### 4d. Other keys

`[Shaders.ShadowsExteriors.Main] NightMinDarkness` (moon floor depth), `[Main.Main.Performance] NvrDxvkPasses / NvrDxvkVariants /
NvrDxvkKeepD3D9 / NvrDxvkTestSuffix` (Vulkan passes, see 7), `NvrSm6Shaders` (the SM6 build of the dedicated passes), `FogReferenceTest`
(development builds; its technique is not in this file, so the C++ draws the unnamed one), `DisableCompositeApply`, `InteriorShadowFold`
and `InteriorShadowFoldRound` (ShaderManager), `[Shaders.Sky.Physical] Enabled` (compiles PHYSICAL_SKY_ON), `[Main.Main.Quality]
MaxQuality` (the fog targets at full size: NVR_FogLayout = 1, handled).

## 5. Passes and outputs

| Technique | Passes | Pixel shaders |
|---|---|---|
| (unnamed, first) | Fog | FullPS(modern) |
| DedicatedFog | Estimate; Reconstruct | EstimatePS(modern, outdoor); ReconstructPS(modern, none) |
| CompositeFog | Estimate; Reconstruct | EstimatePS(modern, outdoor); ReconstructPS(modern, exterior composite) |
| CompositeFogInterior | Estimate; Reconstruct | EstimatePS(modern, inside); ReconstructPS(modern, interior composite) |
| ApplyShadowAO | Apply | ShadowAOPS |
| PackedFog | Estimate; Apply | PackEstimatePS(modern); PackApplyPS(modern) |
| HalfStoreProbe | Probe | ProbeVS (XYZW quad) + HalfProbePS |
| VolumetricFogLegacy, DedicatedFogLegacy, CompositeFogLegacy, CompositeFogInteriorLegacy, PackedFogLegacy | as above | legacy look |
| VolumetricFogClassic, DedicatedFogClassic, CompositeFogClassic, CompositeFogInteriorClassic | as above | classic look |

Not in the file: FogReference (diagnostic of the upstream shader), PackedFogClassic (the C++ does not ask for it).

Targets: NVR_FogBuffer0 / 1 (C++, half size rounded up, or full with MaxQuality, A16B16G16R16F): the Estimate writes (mul.rgb,
normalised depth) to COLOR0 and (add.rgb, 1) to COLOR1. Reconstruct, full, packed apply and ShadowAO write the frame's next image
(rgb; alpha see 6). PackedFog pass 1 writes float4(add.rgb, luma(mul)) into the top-left quarter of the target, which the C++ copies
to TESR_RenderedBuffer. HalfStoreProbe writes 64 x 16 texels of (value, value rounded to nearest even, value rounded toward zero, 1).

The looks, the interior estimate and the composite kind are compile-time arguments (one shader per pass); every runtime switch is a
dynamic branch on a float constant. Compiled sizes (plain ps_3_0, static slots / highest constant register): estimate 989 / c95
(1115 / c104 with the physical sky), composite reconstruct 1396 / c105 (1518 / c114), interior reconstruct 756, packed apply 1156,
classic full 784, legacy estimate 427, probe 48. The effect compiler's preshader adds registers on top; the effect compiles in all five
checked builds.

## 6. Behaviour details

**Geometry.** Camera-relative position = toWorld(uv) x view z (Depth.hlsl; the projection's TAA jitter terms are not used, so the fog
does not move with the jitter); distance = its length (true distance, so the fog does not change as the view turns); absolute height =
camera z + its z. Sky: normalised depth >= 0.9961.

**Density (modern and classic).** fogPower = max(lerp(1, FogData.w, saturate(WeatherImpact)), 0.05). anchor = (mean of saturate(1 -
FogData.x / far) and saturate(1 - FogData.y / far))^2 / (fogPower + 1). S = max(BaseDensity x timeScale x lerp(1, DensityScale,
night) + WeatherImpact x anchor + SunriseSunsetBoost x transition, 0).

**Optical depth.** flat = S x ext x ref x (dist / ref)^fogPower x 1e-4, ref = max(fog far, 1000) (the power bends the curve about the
weather's far distance; at power 1 it is the plain distance), ext = Extinction x 0.1 x lerp(1, ExtinctionScale, night). Height: the
mean of the profile along the straight path between camera height and pixel height in closed form (exponential:
rho(zc) (1 - e^-x) / x, x = k dz; layer: the antiderivative difference over dz; |dz| < 1 uses the midpoint), clamped to [0, 1e4].
plainOD = max(flat x lerp(1, mean, HeightInfluence), MinDensityFloor x flat). Then the noise factor, od = max(plainOD x max(1 + k(2n - 1),
0), floor), T = exp(-od).

**Noise.** Skipped (neutral) on the sky or when NoiseStrength x scale is 0. Modern: four points at (i + 0.5) / 4 of min(dist, 12000)
along the view, each weighed by the height profile (so patches sit in the layer); with the grid, its height-weighted noise mean
(b / g) for the part inside the march distance, the share by optical depth: (1 - e^(-od c)) / (1 - e^-od), c = march / view z.
Classic: 0.5, 0.3, 0.2 of the noise at the surface point x 1, 2.03, 4.01. The mean is contrast-stretched (x1.8 about 0.5).

**Fog colour.** skyHaze = lerp(horizon, sky low, smoothstep(0, 0.35, view up)) (outdoors; indoors the sky colour). With the physical
sky (PHYSICAL_SKY_ON, modern, outdoors, PhysSky.x > 0): skyHaze and the horizon colour are blended with the table's radiance in the view
direction / along the horizon (PhysAtmosphere::Blend: weather tint z, share x), and the sunlight with the sun's transmittance through the
air (AtmSunDepth, as the clouds). Ambient = the SH sky straight up (C0 + C2 + 2 C6; the game's SH in classic) when the Sky effect is on
and outdoors, else the sky colour; x PBRData.w; at least the weather ambient x moon floor x NightAmbientStrength x night. Colour =
lerp(ambient, lerp(skyHaze, weather colour, WeatherFogColor x (1 - sky)), WeatherImpact), then toward the weather colour (the sky colour
on the sky) by 1 - exp(-6 S), saturation, night tint.

**Sun.** sunLight = GetSunColor (Sky.hlsl: (1 + h) sun + sunset x (1 - h)^8 x day x atmosphere) x PBRData.z. Visibility (ShadowStrength
> 0, outdoors, maps on, faded by ShadowFade.x, by day): modern with the grid: lerp(1, a / g, grid share); otherwise one cascade lookup
at the surface (not for the sky) with the include's normal offset and bias (FogSunVisibility: the first cascade holding the point, lit
past the last one with a 10 % fade); lerp(1, vis, ShadowStrength). 35 % x dayFade of the fog colour counts as direct sun and is shaded;
the glow (4a SunGlow) is added on top and shaded. inscatter = (shaded colour + sunLight x glow x vis) x albedo.
(2026-11-07, ptaaa-fog) While the ray tracing core runs for another RT effect, the grid's a channel holds the TRACED sun visibility
(`[Shaders.VolumetricLight.RT]`, src/effects/VolumetricLight/README.md "RT fog visibility"): with FogVolume + Froxels the fog's sun
shading inside MaxDistance follows the RT sun shadows' casters with no change here. The surface lookup past the grid (and the ClassicLook /
no-grid paths) stays the cascades.

**Compose.** skyMask = lerp(1 - sky x WeatherFilterBlend, 1, night x DisableSkyMask); mul = lerp(1, T, skyMask), add = inscatter x (1 -
T) x skyMask; aerial tint on mul; horizon fog and EdgeAA (one blend k = hf + edge - hf x edge toward the horizon colour); Amount; game
fog floor (modern); mul saturated, add >= 0. Final colour = delinearize(max(linearize(scene) x mul + add, 0)); alpha = lerp(scene alpha,
1, night) (1 where a composite ran first, as the passes it replaces write 1).

**Legacy (LegacyFog; an approximation of the pre-rewrite Simple + Height fog from its keys).** Distances x lerp(1, FogNight, night).
Simple fog: coverage 1 - exp(-MinimumBaseFog x ext x dist x 1e-4 x heightMean(SimpleFogHeight x 1e-3, relative to the camera)) x (1 -
weather curve x WeatherImpact), x SimpleFogBlend. Height fog: HeightFogDensity x ext x far x (dist x HeightFogDist / far)^HeightFogRolloff
x 1e-4 x heightMean(HeightFogFalloff curve from HeightFogHeight), coverage x HeightFogBlend. Colours: sky toward weather colour by
coverage x 10 / SimpleFogSkyColor or / HeightFogSkyColor, saturation FogSaturation / HeightFogSaturation, half ambient, night tint;
albedos Inscattering / Extinction and HeightFogInscattering / Extinction; SunPower x sunlight x ((1 + cos) / 2)^4. Horizon fog from the
legacy distant keys, Amount from LegacyData.z. No aerial, EdgeAA, layer, floor, grid or physical sky.

**Estimate.** Each half texel takes the fog of the nearest (smallest depth) full pixel it covers (2 x 2, or the one pixel with
MaxQuality), at that pixel's centre: thin near objects keep their own fog.

**Reconstruct.** The composites first (below), then: the four nearest half texels, bilinear x saturate(2 - |depth - pixel depth| / tol),
tol = max(0.02 x depth, 0.0005). Where the weights sum below 0.25 the pixel's own fog is computed (dynamic branch) and blended in by
weight x 4. Then, with NVR_FogGridFlags.y, the volumetric light's composite on the linear result (VolumetricLight's CompositeAir: lit
air from the grid's r sums / grid scale, air 1 - exp(-density x |ray| x planar), HG phase capped at 8, x strength x fade).

**Composites.** Sun (CompositeFlags.x): ShadowsExteriors' apply: SunFar factor (if bound), f = lerp(floor, 1, saturate(lerp(R, 1,
ShadowFade.x) + G x ShadowFade.z)), floor = max(0, 1 - Darkness), shade tinted toward the sky (blend by saturate(f + 0.5)), gamma 2.2,
the underwater water-plane exception. Interior (CompositeFogInterior): InteriorShadowApply.hlsl with the raw lamp mask (R), then
rounded to 16-bit float as NVR_CompositeFlags.w says (RoundToHalf: nearest even, toward zero, or none), alpha 1. AO (flags.y):
AmbientOcclusion's dedicated combine: 2 x 2 AO taps, bilinear / (1 + |depth diff| / BlurDrop), lerp(floor, 1, AO), lighter where
luma > LumThreshold, frame x AO^(1/2.2); with flags.z the sky take (SkyTake with the same weights over NVR_SkyVis rb); nothing past 8000.

**LampHaze (UNOFFICIAL 2026-11-03, indoors).** Before, indoors the fog's air was lit by the ambient (and the cell's fog colour) only. Now
ModernFogTerms adds the light of the room's lamps scattered toward the camera to the add term, after the sky mask and before Amount:
add += LampHaze x S x Inscattering x skyMask x sum over lamps of colour x J x P, S the density with its noise (as the rest of the fog).
Lamps (C++ SetLampHaze, each frame in Render): the shadow-slot lamps (all slots, BatchLightPosition) and the tracked unshadowed ones
(LightPosition) that ShaderManager::GetNearbyLights listed this frame (the list AmbientLight's grid and the path tracer use), each once,
spheres wholly behind the camera left out, nearest sphere first, at most LampHazeMaxLamps; reach = the list's radius / [Shaders.
ShadowsInteriors.Main] LightRadiusMult (the game's Spec.r, where the lamp's light on the walls ends); colour = (game colour)^2.2 x dimmer.
Light in the air: f(d) = (e^2 / (d^2 + e^2) - e^2 / (r^2 + e^2)) / (1 - e^2 / (r^2 + e^2)), e = LampHazeCore x r: inverse square with a core,
1 at the lamp, 0 at its reach. J = the integral of f along the visible view inside the sphere (clipped to the camera and the surface), in
core lengths (about 2.3 through a lamp's centre at the default core, whatever its size), closed form: with s along the view from the point nearest the lamp, h that point's distance to it, h' = sqrt(h^2 + e^2),
phi = atan(s / h'): (e^2 / h' (phi_b - phi_a) - e^2 / (r^2 + e^2) (s_b - s_a)) / ((1 - e^2 / (r^2 + e^2)) e).
P = the mean of HG(g) (x 4 pi, capped at 8) x exp(-extinction x distance) at the two Gauss-Legendre points of
[phi_a, phi_b] (the remaining factors are smooth in phi: no march, no banding however close the view passes to a lamp). Rough size with the
defaults (S 0.3, Inscattering 0.35, Amount 0.25): a view through a lamp's centre gains about 0.03 x its linear colour, half its reach off
the centre about a fifth of that. Passes: the estimates (DedicatedFog, CompositeFog, CompositeFogInterior), the full-resolution and packed
techniques, and DedicatedFogCombine's per-pixel fallback. NOT in the per-pixel fallback of the CompositeFog and CompositeFogInterior
reconstructs (both went over the 224 constant registers with it): those rare edge pixels keep the fog without the lamps' light (a lamp's
own silhouette may show a one-pixel rim with less glow). ClassicLook and LegacyFog never have it (their techniques compile to exactly the
same bytecode as before). No shadows: a lamp's sphere that reaches through a wall glows on the other side too; the lamp cube maps would need
a per-point cube read per lamp (two free samplers, s15 left) and are a follow-up. Big invisible fill lights (FillLightRadius) glow like any
lamp. Not taken: TESR_ExtraLights (no colour), ManyLamps' game lamp table, spot lights (the flashlight).

**PackedFog.** Pass 1 at half size into the target's top-left quarter: (add, luma(mul)). Pass 2: the four half texels' coefficients, their
depths found again (nearest of their 2 x 2), the same weights and own-fog fallback; the multiply is grey in this path.

**HalfStoreProbe.** r = (1024 + i + q) / 1024 x 2^(i mod 7 - 3), q in {0.25, 0.5, 0.75} (ties included), i = texel index; g, b =
RoundToHalf(r, 0 / 2). The C++ compares the stored r with g and b.

**Stability.** No per-frame noise anywhere (positions ignore jitter, noise is world-space and moves only with the wind); the half-size
estimate takes whole pixels (no shimmer from sub-pixel resampling); every division is guarded; smoothstep edges are kept apart.

**Bugs of the old spec and status.**
- Solid white wall near the camera (height term divided by the distance): fixed, closed-form path mean.
- Fog going to infinity below z = 0 (uncapped horizon height fade): fixed, capped at 1.
- Thin objects taking the sky's fog: fixed, nearest-pixel estimate.
- Edge pixels with no matching half texel: now get their own fog (the earlier wave-3 shader took the nearest texel's).
- smoothstep with equal edges (AerialRangeStart 1, DistantFogRange 0 indoors): guarded.
- The froxel grid, the physical sky table, the interior rounding and HalfStoreProbe, PackedFog: implemented (were missing).

## 7. Vulkan path

Files: `Compute/VolumetricFog.glsl` (bindings, uniform block, push block, the whole FogTerms and composites, ported from the .fx),
`VolumetricFogEstimate.frag` (two outputs), `VolumetricFogApply.frag` (push.mode 0-4). `bash tools/nvr-dxvk/compile-compute.sh` builds
`VolumetricFogEstimate.spv`, `VolumetricFogApply.spv` and the variants of `Compute/variants.txt` (Estimate l c i p, Apply m l c p, the
test suffixes n / s / k / f).

- Selected by `NvrDxvkPasses` on Vegas Shaders DXVK (not in rect frames, not for the diagnostic overrides or SM6); ExtDedicated (both passes),
  ExtShadowAO (mode 3), ExtRender (mode 4, full-resolution techniques). PackedFog and HalfStoreProbe stay D3D9.
- Set 1 bindings: 0 scene, 1 depth, 2 atlas, 3 normals, 4 point shadow, 5 noise (3D), 6 sky table, 7 grid; Apply also 8 fog multiply, 9
  fog add, 10 AO, 11 sky visibility, 12 SunFar, 13 the sky shares (TESR_SkyCoverage, UNOFFICIAL 2026-11-05 MsaaEdgeFix: read while
  NVR_FogGridFlags.z is 1). Stand-ins (the depth buffer) are never read while their flags are 0.
- UNOFFICIAL 2026-11-05 MsaaEdgeFix ([Main.Main.Quality], on by default; only with the game's MSAA): where the volumetric light's froxel
  grid is applied here (FogGridLight), a pixel's grid depth is blended from the surface's toward the march distance by the pixel's exact sky
  share (core/SkyCoverage.h; D3D9: NVR_FogSkyCoverage on stage 15, bound by hand, nothing bound = 0; Vulkan: binding 13 and
  NVR_FogGridFlags.z). With transparency multisampling a see-through cutout far off (a pylon's lattice, a bush) covers only part of each
  pixel's samples, but the depth read here is one sample's (the nearest under the DXVK fork), so the see-through part took the air only
  up to the cutout. Blending the depth instead of the two airs: a second grid lookup took the D3D9 composites past ps_3_0's 224 constants
  (the air builds up almost evenly along a view, so the two differ by a few percent). Off, or without MSAA: the lookup is the one before
  (lerp by 0). Bench 2026-11-05 og-desert, MsaaEdgeFix on vs off, VL MaxDistance 15000: pylon head +1.2 (fork, Vulkan) / +1.5 (native D3D9)
  luma, the rest of the frame |diff| 0.07 (noise); without MSAA on vs off |diff| 0.10 = noise.
- UNOFFICIAL 2026-11-03 LampHaze: not ported. While its term is on this frame (TESR_VolumetricFogLamps.x > 0: indoors, lamps listed), the
  Vulkan passes (ExtDedicated, ExtRender for the modern full-resolution technique) and the SM6 passes are not used and the D3D9 technique
  draws (logged once: "UNOFFICIAL LampHaze: ..."); outdoors, with LampHaze 0, ClassicLook or LegacyFog they run as before. Porting means
  the lamp table as a set 1 binding (14; 13 is the sky shares since 2026-11-05), TESR_VolumetricFogLamps in FogExtConstants, LampHazeLight in VolumetricFog.glsl and the .spv
  rebuilt with tools/nvr-dxvk/compile-compute.sh.
- Set 2 binding 0: FogExtConstants (6 matrices, 54 vectors in FogExtVectorNames order, 9 + 9 SH, 9 colours linearized on the CPU).
- Push (112 bytes): sampler heap indices (point, linear, wrap), mode, legacy, classic, interior, physicalSky, then NVR_FogLayout,
  NVR_CompositeFlags, NVR_CompositeAOTexel, NVR_FogGridFlags, NVR_SunFarFlags.
- The C++ asks for the variant (FOG_VARIANT: the switches become constants) and falls back to the general shader.

## 8. Look and cost

Expected look (not yet checked in game): haze growing with distance and toward the weather's fog colour, a clean horizon, a soft glow
toward a low sun (bright at SunGlow 1), fog darker in sun shadow (and shaded through the air with Froxels + FogVolume), patches that drift
with the wind and sit in the height layer, the sky untouched unless the weather or the night lets the fog in, dark-but-not-black fog at
night. Cost: the half-size estimate (four depth reads, up to four noise reads or two grid reads, one cascade lookup, the closed-form
height terms; branches skip what is off) and the full-size reconstruct (four half texels x 2, the composites when folded, the own-fog
fallback only on edge pixels, two grid reads for the folded volumetric light).

LampHaze (indoors, lamps listed): estimate +165-175 static slots (DedicatedFog estimate 1132 -> 1301, CompositeFogInterior estimate 1254 ->
1423, full 1124 -> 1293, with the physical sky; highest constant c184 -> c195), DedicatedFog reconstruct 1309 -> 1478 (fallback only). Per
half-size pixel a loop of up to LampHazeMaxLamps lamps: about 12 instructions and one point read for a lamp whose sphere the view misses,
about 120 and two reads for one it passes through (two atan, two sincos, two exp, the phase). Not measured in game.

## 9. Open questions

- LampHaze (2026-11-03): not seen in game. Its scale (the 0.03 x lamp colour through a lamp with the defaults), the core size, fill lights
  glowing, glow through walls (no shadows), the rim on edge pixels in the composite passes, and the cost with 8 lamps are all to check.
  Follow-ups: lamp shadows in the air for the strongest 1-2 shadowed lamps (TESR_ShadowCubeMapBuffer, s15); the interior's directional
  light (the cell's or its lighting template's directional colour, read by AmbientLight.cpp ~940; it has no position or window, so it would
  be an even tint, not light through windows, and was left out); the Vulkan port.

- Not checked in game: every magnitude (the glow x 2 scale, the 35 % sun share of the fog colour, the noise contrast, the legacy fog's
  look, the physical-sky blend in the fog colour).
- Settings whose meaning was kept as the live code has it although a description says otherwise: HeightFalloffScale multiplies
  HeightFalloff (the toml says raising it hugs the ground; the froxel grid also multiplies); WeatherImpact pulls the fog power from 1
  toward the weather's (the earlier spec said the reverse; the grid does this); the height scale is HeightFalloff x fog power / 1.5e-4
  (the toml says "divided by the weather's fog power"; the grid multiplies).
- The sun composite's shade tint uses TESR_FogSkyColor (the fog's copy with the sky look) where ShadowsExteriors.fx uses TESR_SkyColor:
  the Vulkan constant block has only the fog's copy, and both paths must match. A small difference between folded and separate shadows.
- FogSunVisibility takes one cascade (no cross-cascade blend; the include's blended lookup does not fit the composite pass's
  constants once the effect compiler adds its preshader).
- LegacyFog and ClassicLook are rebuilt from their key descriptions, not from the old shaders (which were not read): presets made for
  them will look similar, not identical.
- FogReference (development diagnostic) is not provided.
- compile-compute.sh (full run) stops at `SunShadowsApply c1` (variants.txt names `SunShadowsApply.frag`, which is not in this tree since
  the wave-3 SunShadows rewrite); every other shader and variant, the fog's included, compiles and validates.
- The loader for `[_Shaders.VolumetricFog.Status] Enabled` was not traced (unchanged).
