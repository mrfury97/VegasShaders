# WetWorld (rain wetness, puddles, splashes, wet streaks and puddle reflections)

## 1. Purpose
While it rains outdoors, the ground, roads and rock go wet and darker where the rain reaches them, with glossy highlights. Puddles form in hollows and on flat ground, and their surfaces ripple, reflect the sky and clouds, pick up the sun and nearby lamps, and show the scene above them (screen-space reflections). Rain splashes flicker on flat wet ground, and thin wet streaks run down walls. Surfaces under roofs or cover stay dry. The effect also feeds a small "cover test" (is something above the player?) that the first-person weapon's rain drops use. Water surfaces and distant ground are left alone.
Two looks ([Shaders.WetWorld.Main] Model, section 6 "Model 1"): 1 (default since 2026-11-05) a physically based, material-dependent wet surface: every open surface gets wet (walls less), porous materials darken, a thin water layer reflects the sky, sun and lamps through one GGX lobe; 0 the previous look exactly (wetness only on flat open ground, no darkening outside puddles, fixed Blinn highlights, a refraction wobble on all wet ground). A frame in which Model 1 cannot tell characters apart (no material buffer this frame, a frame upscaled before the effects, or MaterialBufferSkin off) draws the Model 0 look instead (section 5).

## 2. When it runs
- Enabled by [Shaders.WetWorld.Status] Enabled (default true).
- Per frame the wet pass draws when any of these holds: the rain amount above zero (TESR_WetWorldData x), the puddle amount above zero (TESR_WetWorldData z), or the persistent cover map still holds wet spots. It also needs the player to be outdoors and not underwater.
- Rain amount: the rain effect's own animator (Precipitations). It rises to 1 over 0.05 game hours when rain starts and falls to 0 over 0.07 game hours when it stops.
- Puddle amount: its own animator, in game hours, set by [Shaders.WetWorld.Main] Increase and Decrease. Both default to 0 (so puddles appear and vanish at once). When rain starts it rises to 1 over Increase; when rain stops it falls to 0 over Decrease.
- The orthographic shadow map is requested while the rain or puddle amount is above zero, and while the persistent map is active.
- Weather flag: the game's rainy weather, or [Shaders.Precipitations.Main] ForceRain (default false), which makes the game count as rainy outdoors.
- Frame order (src/core/ShaderManager.cpp): in the material-effects block, after the blood-flow pass and before the fog and beam pass. Just before the wet pass, the cover test (below) runs when [Shaders.WetWorld.Main] HudDrops is above zero, the rain amount is above zero, the player is outdoors, and not underwater. The persistent cover map updates earlier in the frame, before the snow accumulation pass.
- The wet pass does not test for menus; whether it draws in a menu is not confirmed here.

## 3. Inputs
### Textures (sampler slot = the register number in the D3D9 shader)
| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | TESR_RenderedBuffer | during the wet-map passes: the current image (the wet map, then its blurred copy). During the wet pass: the blurred wet map (red = puddle coverage, green = flat wet coverage on flat ground, blue = flat wet on very flat ground) | linear, clamp |
| s1 | TESR_DepthBuffer | combined linear depth (view distance over far distance) | linear, clamp |
| s2 | TESR_SourceBuffer | the frame before this effect (the scene); the wet pass reads it for the refracted colour and for reflections | linear, clamp |
| s3 | TESR_RippleSampler | rain ripple texture, file Precipitations\ripples.dds (per texel: a mask, two encoded direction values, and a phase offset) | linear mips, wrap |
| s4 | TESR_OrthoMapBuffer | the sun's orthographic shadow depth map | linear, clamp |
| s5 | TESR_NormalsBuffer | world normals of the frame (encoded) | point, clamp |
| s6 | TESR_PointShadowBuffer | point and sun shadow mask (red = sun shadow, green = point-light shadow) | linear, clamp |
| s7 | TESR_DepthBufferViewModel | depth of the first-person weapon and hands | linear, clamp |
| s8 | TESR_SplashSampler | rain splash sheet, file Precipitations\splash.dds (4 by 4 frames of 16) | linear mips, clamp |
| s9 | TESR_SplashNormalSampler | splash normal sheet, file Precipitations\splash_n.dds | linear mips, clamp |
| s10 | TESR_PuddleCloudSampler | cloud pattern on puddles, file water\water_d.dds | linear mips, wrap |
| s11 | TESR_PuddleNormalSampler | puddle normal map, file water\water_nrm.dds | linear mips, wrap |
| s12 | TESR_WaterStreakSampler | wall streak texture, file Precipitations\water-streaks_n.dds | linear mips, wrap |
| s13 | TESR_CoverMapBuffer | persistent cover map (wet, puddle water, snow, trodden snow per spot); persistent techniques only | wrap, no mips |
| s14 | TESR_MaterialBuffer | (Model 1 only, techniques 7 to 10) the material class + 0.49 x metal share (1 object, 2 worn gear, 3 cloth, 4 land, 5 plant, 6 skin), b roughness (-1 none), written by the mod's lit templates while the world draws (core/CapturedNormals.h) | point, clamp |
| s15 | TESR_AlbedoBuffer | (Model 1 only) the linear base colour, a 1 where written | point, clamp |

All 16 D3D9 sampler slots are now in use (the effect binds TESR_ samplers by declaration order). TESR_CapturedNormalsBuffer is not read: no slot is left, and it holds the smooth vertex normal (before the normal map), so it has no bump detail to give; actors are found by the material class instead (2 worn gear, 6 skin with [Main.Main.Quality] MaterialBufferSkin on, the default).

The Vulkan wet pass (section 7) binds the same textures in a different order: binding 0 the blurred wet map, 1 the source picture, 2 depth, 3 view-model depth, 4 ortho map, 5 normals, 6 point shadow, 7 ripples, 8 splash, 9 splash normals, 10 cloud, 11 puddle normals, 12 streaks. The D3D9 slot numbers and the Vulkan binding numbers are not the same.

### Constants
| Name | Components and meaning | Notes |
|---|---|---|
| TESR_WetWorldData | x: rain amount, 0 to 1 (rain animator); y: rainy flag (1 when the game is rainy, else 0); z: puddle amount (puddle animator, 0 to 1); w: puddle darkness (the setting Amount, 0 to 1 in use) | registered by the effect; see section 6 for how y is used |
| TESR_WetWorldCoeffs | x, y, z: PuddleCoeff_R, G, B; w: PuddleSpecularMultiplier | registered and set from settings, but never read by the shader (their settings are marked not used) |
| TESR_WetWorldLook | x: KeepHighlights (1 on: the wet pass only clamps below zero; 0 off: it clamps to 0..1); y: 1 when the material and albedo buffers match the frame the effects see (0 in a frame upscaled before the effects: RenderScale with LowResEffects off), Model 1 only; z, w unused | registered by the effect |
| TESR_MaterialBufferData, TESR_AlbedoBufferData | x: 1 when that buffer holds this frame's data (core/CapturedNormals.h) | engine; Model 1 only |
| TESR_GameTime | z: animation clock; drives ripples, swell waves, splashes, the streak flow and the cloud drift | engine |
| TESR_SunDirection | direction toward the sun | engine |
| TESR_SunPosition | sun position vector; its dot product with up gives the sun's height | engine |
| TESR_SunColor, TESR_SunAmbient | sun colour and sun ambient colour | engine |
| TESR_SunAmount | declared, not read | |
| TESR_SkyColor, TESR_SkyLowColor, TESR_HorizonColor, TESR_SkyData | sky colours for the puddle reflection; the z component of TESR_SkyData is passed to the sky function as its sun strength | engine |
| TESR_OrthoData | x: the reach of the ortho shadow map (game units); the wet effect stops at 0.9 of it | engine (set by the exterior shadows effect) |
| TESR_WaterSettings | x: water level height | engine |
| TESR_ShadowCameraToLightTransformOrtho | 4x4, world to sun ortho clip | engine |
| TESR_ViewTransform, TESR_InvViewTransform, TESR_ProjectionTransform, TESR_InvProjectionTransform | camera matrices | engine |
| TESR_CameraData | x near, y far distance | engine |
| TESR_CameraPosition | camera world position | engine |
| TESR_ReciprocalResolution, TESR_RenderScale | texel size and the drawn part of the target | engine |
| TESR_DepthConstants | z: reversed-depth flag (0 normal depth, non-zero reversed); drives the first-person depth test | engine |
| TESR_LightPosition[12], TESR_ShadowLightPosition[12] | point lights (xyz position, w radius; w = 0 means an empty slot) | engine; the shadow-casting set is the second array |
| TESR_LightColor[24] | light colours: entries 0 to 11 match TESR_ShadowLightPosition, entries 12 to 23 match TESR_LightPosition | engine |
| TESR_CoverMapData, TESR_CoverMapWindow, TESR_CoverMapGlobal, TESR_CoverMapTrail | the cover map's placement, window, open-ground values and trail parameters (persistent only) | from the cover map |

## 4. Settings
| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| Shaders.WetWorld.Status | Enabled | bool | true | Turns the effect on or off. |
| Shaders.WetWorld.Main | FastPaths | bool | true | Same picture with real early exits (less work): the wet pass skips work on sky, distance, water, dry ground and first-person pixels. Off = the original pass (technique 0). |
| Shaders.WetWorld.Main | FixedReflectionTrace | bool | false | Uses the corrected puddle reflection trace (section 6). It changes the look (softer, no border patches). Off = the trace as the source describes it (see section 9). |
| Shaders.WetWorld.Main | Model | int 0 or 1 | 1 | 1: the physically based, material-dependent wet surface (section 6, "Model 1"; techniques 7 to 10, always the FastPaths structure; D3D9 only). 0: the previous look exactly (techniques 0 to 6, unchanged programs). Applies immediately. Model 1 falls back to Model 0 for any frame without this frame's material buffer (section 5). |
| Shaders.WetWorld.Main | KeepHighlights | bool | true | On: highlights above 1 pass through to the tonemapper (only negative values are cut). Off: the wet pass clamps to 0..1 before the tonemapper. |
| Shaders.WetWorld.Main | Increase | float (game hours) | 0.0 | Time for puddles to appear after rain starts. |
| Shaders.WetWorld.Main | Decrease | float (game hours) | 0.0 | Time for puddles to dissipate after rain stops. |
| Shaders.WetWorld.Main | Amount | float 0 to 1 (darkness) | 0.0 | How dark puddles are: at 1 the puddle base colour is multiplied by 0.55; at 0 not at all. |
| Shaders.WetWorld.Main | PuddleCoeff_R, PuddleCoeff_G, PuddleCoeff_B | float | 0.72, 0.62, 0.48 | Not used by the shader. |
| Shaders.WetWorld.Main | PuddleSpecularMultiplier | float | 0.2 | Not used by the shader. |
| Shaders.WetWorld.Main | HudDrops | float 0 to 1 | 1.0 | Rain drops on the first-person weapon while the player stands in rain, outside cover. 0 = off (restart to switch; compiled into the object shaders). Enables the cover test (section 5). |
| Shaders.WetWorld.Main | HudDropSize, HudDropDensity, HudRefraction, HudReflection, HudGloss, HudWetGloss, HudWetTime, HudDryTime, HudDebug | float / int | 0.6, 0.6, 1.0, 1.0, 0.8, 0.3, 20, 45, 0 | The first-person drops (not in this effect's shader; they read the cover test result). Size, density, magnification, tilt, gloss, wet gloss, seconds to wet through and to dry under cover, and a debug switch. |
| Shaders.WetWorld.Main | Persistent | bool | false | Wetness and puddles kept spot by spot in the cover map (section 6). Costs about 0.05 ms and 32 MB of video memory. |
| Shaders.WetWorld.Main | PersistentWetTime | float (game hours) | 0.25 | Rain needed for open ground to be fully wet (cover map). |
| Shaders.WetWorld.Main | PersistentDryTime | float (game hours) | 1.5 | Time for wet ground in full sun to dry (cover map). |
| Shaders.WetWorld.Main | PersistentPuddleFill | float (game hours) | 0.5 | Rain needed for puddles to fill (cover map). |
| Shaders.WetWorld.Main | PersistentPuddleDry | float (game hours) | 4.0 | Time for puddles in full sun to dry up (cover map). |
| Shaders.WetWorld.Main | PersistentShade | float 0.02 to 1 | 0.35 | How much slower shaded and night-time spots dry compared with sunlit ones (cover map). |
| Shaders.WetWorld.Main | PersistentMapSize | float (game units) | 8192 | Width of the cover map round the player (shared with the snow's persistent mode). |
| Shaders.WetWorld.Main | PersistentMapResolution | int | 1024 | Texels across the map. |
| Shaders.WetWorld.Main | PersistentDebugView | int | 0 | Test view of the cover map (1 top-down map, 2 painted over the world). |
| Shaders.Precipitations.Main | ForceRain | bool | false | Not this effect's setting: makes the game count as rainy outdoors (section 2). |

## 5. Passes and outputs
| Technique (index) | Passes | Used when |
|---|---|---|
| unnamed (0) | wet map, blur across, blur down, wet pass (no fast path, original trace) | FastPaths off, no fixed trace |
| CoverTestTechnique (1) | cover test (one pass) | the first-person cover test (HudDrops) |
| FastPathsTechnique (2) | wet map, blur across, blur down, wet pass (fast) | FastPaths on, no fixed trace |
| FixedTraceTechnique (3) | the same, wet pass with the fixed trace (not fast) | FixedReflectionTrace on, FastPaths off |
| FixedTraceFastPathsTechnique (4) | the same, wet pass with the fixed trace and fast | FixedReflectionTrace on, FastPaths on |
| PersistentTechnique (5) | the same, wet pass fast and reading the cover map | Persistent on, FixedReflectionTrace off |
| PersistentFixedTraceTechnique (6) | the same, fixed trace, fast, reading the cover map | Persistent on, FixedReflectionTrace on |
| PbrTechnique (7) | wet map, blurs, wet pass Model 1 (WetPBR) | Model 1, no fixed trace (stands in for 0 and 2) |
| PbrFixedTraceTechnique (8) | the same with the fixed trace | Model 1, FixedReflectionTrace on (for 3 and 4) |
| PbrPersistentTechnique (9) | the same reading the cover map | Model 1, Persistent on (for 5) |
| PbrPersistentFixedTraceTechnique (10) | the same, fixed trace, cover map | Model 1, Persistent and FixedReflectionTrace on (for 6) |

The C++ picks the technique (MainTechnique and the persistent variant); with Model 1, WetWorldEffect::Render then swaps it for its Model 1 twin (0 and 2 -> 7, 3 and 4 -> 8, 5 -> 9, 6 -> 10; the cover test 1 and a shader file without the twins keep the index). Each technique's passes are drawn into the frame chain.

Model 1 fallback (2026-11-05, WetWorldEffect::Model1Unavailable): Model 1 tells characters (worn gear, skin) and materials apart only by TESR_MaterialBuffer; without it every pixel is class 0 and characters would get the full darkening and film. So Render keeps the Model 0 technique (the old look, which may then take the Vulkan path) in any frame where: (1) the frame is upscaled before the effects (RenderScale with LowResEffects off: the buffer has another size), (2) the material buffer holds nothing this frame (CapturedNormals::Data.x or MaterialData.x is 0, or no target: [Main.Main.Quality] CapturedNormals or MaterialBuffer off, the game's own MSAA on native D3D9, a capture that failed), or (3) [Main.Main.Quality] MaterialBufferSkin is off (skin would be class 0). Each reason is logged once a session: "WetWorld: Model 1 needs this frame's material buffer, but ...: the Model 0 technique N runs instead". A missing albedo buffer alone does not fall back (porosity is then guessed from the picture's chroma, and there is no albedo bump).

Pass outputs:
- Wet-map pass: writes (red) puddle coverage in hollows and on near-flat ground, (green) flat wetness: the ground that is open to the sun and tilted slightly up, and (blue) flat wetness on very flat ground. Alpha 1. Pixels beyond 0.9 of the ortho reach write zero in all channels.
- Blur passes (across, then down): a depth-aware five-tap blur of the wet map (the centre and the taps at one and two texels, with weights 0.375, 0.25 and 0.0625, and a depth tolerance that keeps the blur on one surface). Writes the blurred values, alpha 1. Both passes must cover every pixel.
- Wet pass: writes the final picture at full frame size (alpha 1, except the first-person early exit, which keeps the base colour's alpha). It may write the game target directly. The picture is the scene with wetness, puddles, splashes and streaks applied, and clamped as KeepHighlights says.
- Cover test (technique 1): draws one white pixel only if it is kept by the test (below); the C++ counts the kept pixels with an occlusion query, and a non-zero count means "covered".

## 6. Behaviour details
### Distance, water and the far cut-off
- The effect stops at 0.9 of the ortho map's reach (TESR_OrthoData x). Beyond that the wet pass returns the scene (plus any splash addition) and nothing else. Between the near range and that limit the wetness fades with a smooth curve (full near, zero at the limit).
- Water exclusion: a very flat, upward-facing surface (its normal within about 0.99 of straight up) whose height lies within a band of the water level (the band is the pixel's depth over far distance times 500 game units, each side) is returned unchanged.

### Roofs and open ground
- Roof test: a pixel is "open" when the ortho map shows nothing between it and the sun. A point outside the ortho map counts as covered (it gets no rain wetness), which differs from the snow effects, which count outside as open.
- Flat wetness (green channel) requires open ground and a slope tilted up between cosine 0.65 and 0.92. The very flat channel (blue) requires cosine 0.90 to 0.97.
- A first-person pixel (view-model test) gets no ground wetness or puddles.

### Puddles
- Puddle coverage comes from hollows: the ortho depth is compared with eight neighbours at a radius of 100 game units (scaled to the map). A pixel is concave when it lies well below its ring (curvature test, smooth step 0.25 to 2.0 on the depth difference scaled by the depth scale). The concave test is kept only on nearly flat ground (tilt filter: cosine between 0.707 and 0.906 of the height normal), where the neighbours do not jump in depth by more than about 40 to 80 units (edge filter, fading between those), and where the pixel lies on the surface the map saw (tolerance 0.0005 in ortho depth).
- The coverage is then blurred by the wet-map blur (radius 12) and multiplied by a tilt filter on the surface's up component (cosine between 0.707 and 0.906), by the view-model exclusion (puddles do not form under the weapon), and by the puddle fill.
- Puddle fill: the puddle amount (TESR_WetWorldData z, or the persistent puddle water) is mapped through a smooth step from 0.08 to 1.0.
- Puddle rim: a thin darkened band at the edge of each puddle (rim darkening 0.15, reflection boost 0.08) from a quadratic of the puddle mask.
- Puddle depth: a value derived from the puddle mask (squared, after a 1.15 scale) tints the puddle base slightly toward blue (factor 0.25) and darkens it by up to 0.18, so the middle of a puddle reads deeper than its edge.

### Rain ripples and swell
- Four ripple layers sample the ripple texture at scales 120 game units (times 1.0, 1.1, 1.3 and 1.5), each moving on the clock at its own rate (about 0.96 to 0.99 times the clock). Their strength is the raw rain value times 1, 0.75, 0.5 and 0.25 (saturated, times 4), so the rain's ripples build up as it intensifies.
- The ripples bend the surface normal in XY. Ripples apply where the surface is flat (the flat ripple mask, cosine 0.65 to 0.92) and where rain can reach (rain visibility: rain fade times the roof test).
- Puddle swell: two sine waves across the world's X and Y directions (wavelengths about 24 to 31 game units) add a small slope to puddle normals, with amplitude 0.003 scaled by the puddle mask to the power 1.35, fading out between 400 and 1800 game units.
- Puddle texture normals: two samples of the puddle normal map at 256 game units per repeat (the second at 1.37 times the scale, drifting the other way), blended 65:35, add slope scaled by the puddle strength (0.25 for puddles, a smaller flat value for wet ground) and by the near-distance fade.

### Splashes
- Splashes: each cell of 8 world units (8 units is the splash cell size) may hold one splash, chosen by a hash and a density equal to the rain amount times the roof test times 0.395. Each splash plays a 16-frame sheet, one cycle every 1/1.35 clock units, with a fade in and out, at a random position within the cell with a small jitter. The first-person weapon uses the same sheet at a scale of 0.28 of the world scale, with its own size variation.
- Splash strength: multiplied by the splash alpha, the flat-ground mask, the rain visibility and a distance fade (full to 1200 game units, zero at 2000). Splash brightness is contrast-stretched and mixed 35 percent toward white, then scaled by 0.17 and by 0.6.
- Splashes also bend the normal (strength 2.2) and refract the picture slightly (0.35 of the refraction scale).

### Vertical wet streaks
- Streaks run down near-vertical surfaces (strongest where the absolute up component is below about 0.55, fading out by 0.8), only under the roof test and in rain. They fade with distance between 800 and 1000 game units.
- The streak texture is projected on the wall in world units of 64 by 90, in two flow groups per lane (eight lanes per texture unit) with random speeds between 0.45 and 1.65 times 12 units per clock unit. The two projections are weighted by the surface's facing.
- Streaks darken the base colour by 0.45 times the streak mask, add a normal offset (strength 0.25), and add a specular term (exponent 80 for the sun and 30 for lamps, strength 1.2), limited to 0.25 times the mask.

### Puddle colour and reflection
- Refraction: the puddle shows the scene shifted by its normal (scale 50 game units, reduced on puddles to 0.35 of that, limited to 0.75 of the scale), using the source picture; a view factor reduces it at steep angles.
- Sky reflection: the sky colour is taken from the sky functions at the reflection direction; Fresnel-style weight with a minimum of 0.12 (power 5), strength 0.55, maximum blend 0.28, brightness 0.55, and the luma is limited to the base's luma (floor 0.015, scale 0.65).
- Cloud reflection: the cloud texture (256 game units per repeat, drifting slowly, contrast 15) is added over the sky colour with brightness 1.65 and strength 0.38.
- Sun highlight: specular with the sun direction, exponent 80 to 200 on wet ground and 700 on puddles, scaled by the sun height (0.25 to 0.70), the shadow mask and the sun colour; limit 1.95 (1.9 on puddles); the puddle strength multiplies it by 2.25.
- Lamp highlights: 12 shadow-casting and 12 other lamps, each with attenuation (one minus the distance over radius, squared), specular exponent 30, limited to 0.9 (1.8 on puddles) after the puddle strength of 2.5.

### Screen-space reflections (puddles)
- A short ray march in view space: 8 steps, the far travel is 1800 game units with a squared step spacing, with a surface bias of 3 units along the normal. The normal used for reflections is partly the puddle normal (influence 0.25).
- Original trace: the source notes say the original march does not stop on the GPU (its loop exits do not work), so as written it produces no reflection. Fixed trace (FixedReflectionTrace on): the first depth crossing counts as a hit; a hit is taken only where the ray is behind the surface by no more than 24 game units (plus 30 percent of the step), it is not the first-person weapon, it is at least four pixels from the source, and it lies on screen; the hit's visibility fades with distance (full to 0.65 of 1800, floor 0.25) and near the screen edge (over the outer 5 percent).
- Reflection blend: 0.55 of the traced colour, weighted by the hit's visibility. The reflection is strongest at grazing angles, is removed as the camera looks steeply down onto the surface (the angle fade runs from cosine 0.2 to 0.4 of the view against up), and is absent in the top third of the screen, full from two thirds of the way down.
- Puddle reflections are not used on the first-person weapon or the view model.

### Rain amount and the rain flag (a quirk to keep or fix deliberately)
- The wet pass computes its rain amount as the rain value divided by the rainy flag (floored at 0.0001), then clamps to 0..1. When it is raining the flag is 1, so the amount is simply the rain value. When it is not raining the flag is 0, so the amount becomes 1 as soon as the rain value exceeds about 0.0001. In practice, after rain stops the wetness and ripples stay at full strength for the whole rain fade-out (about 0.07 game hours) and drop out when the value reaches zero. Puddles, which use the puddle amount, fade on their own schedule (Decrease).
- The wetness itself uses a smooth step from 0.05 to 0.35 on the amount (rain fade), then multiplies the roof test.

### Persistent wetness and puddles (Persistent on, techniques 5 and 6)
- The flat wetness comes from the cover map's wet value per spot (instead of the rain fade), and puddle fill from the cover map's puddle water (instead of the puddle amount).
- The ripples, splashes and wall streaks still follow the rain falling now.
- Persistent techniques always use the fast pass (with or without the fixed trace).

### First-person weapon and view-model
- The view-model test marks first-person pixels: a pixel is first-person when the view-model depth is closer than the normal depth test (normal depth: view-model depth below 0.9; reversed depth: view-model depth above 0.01). It uses the reversed-depth flag (TESR_DepthConstants z).
- First-person pixels get a separate composition: the base picture refracted slightly by the ripple normals, blended by the ripple mask, with no puddle, sky, streak or reflection work. Water exclusion and the far cut-off still apply to them first.
- The cover test (technique 1) keeps a single pixel: it is drawn at a point 48 game units above the player, in the sun's ortho map. It is kept only where that point is inside the map and lies below the map's top surface by more than 0.003 (so something lies above the player's head). Outside the map it is not kept, so the count stays zero and the player counts as not covered. The result is read a frame or two later (through an occlusion query), and the rain drops on the weapon use it.

### Cross-cutting behaviour
- Render-rectangle frames: texture coordinates for the depth, the ortho map and the reflection are mapped into the drawn part of the target (RenderScale). The refraction offset is scaled to native frames.
- Jitter: the wet pass takes no jitter offset (it uses the picture as the frame has it).
- Reversed depth: the combined depth buffer is linear and unaffected, but the first-person test and the view-model depth depend on the reversed-depth flag (TESR_DepthConstants z).
- HDR vs LDR: the pass runs before the tonemapper (KeepHighlights). Its output can exceed 1 when KeepHighlights is on; the picture is display-referred otherwise. Colour changes are done in linear space (sRGB curve), and the output is converted back.
- NaN / inf guards: divisions use a floor (1e-4 or 1e-6, or a max with a small value); exponents apply to saturated inputs; the half vectors and normals normalise; the streak colour divides by a floored alpha.
- Things that look wrong if missed: wet ground under roofs (roof test); wet sky or water (far and water tests); puddles on steep slopes (tilt filter); puddles that reflect the wrong place (SSR thickness and refinement); hard-edged reflection patches at the screen edge (edge fade); rain wetness that stays after the rain (rain amount); streaks on flat ground (up-component range); highlights clipped on puddles (highlight limits and KeepHighlights).

### Model 1: physically based wet surfaces (techniques 7 to 10, WetPBR)
Everything above (wet map, puddle detection, ripples, swell, splashes, streaks, SSR, the first-person composition, the far and water exits) is kept as it is. What differs:
- Wetness w = rain history (the rain fade, or the cover map's wetness with Persistent) x open sky x exposure x the distance fade (LODfade) x the wall fade. Open sky: the roof test taken at the surface moved out along its normal by lerp(4, 36, 1 - N.z) game units (so a wall is tested in front of itself, about three ortho texels). Exposure = saturate(0.35 + 0.65 N.z): 1 facing up, 0.35 on a vertical wall, 0 facing down past about 33 degrees. Wall fade (performance, QA P8): where N.z is below 0.4 (fading in from 0.5) the wetness goes from full at 1200 to 0 at 1600 game units of view depth, so far walls take the early exit (their 0.35-exposure sheen does not show that far); surfaces with N.z >= 0.5 are not touched. Actors (material class 2 worn gear and 6 skin) and the first-person view get w = 0, no puddles, no splashes and no wall streaks (2026-11-05: Model 0 drew both on characters); the skin shader wets skin itself (Skin.hlsl).
- Material: class, metal share (frac(r) / 0.49) and roughness from TESR_MaterialBuffer, albedo from TESR_AlbedoBuffer, each only where written this frame (Data.x, the a / r test, TESR_WetWorldLook.y). A frame without the material buffer does not reach WetPBR (the Model 0 fallback, section 5); within a frame, pixels nothing wrote (the game's own shaders, far LOD, grass, see-through things) are class "unknown", and without the albedo buffer the picture's own chroma stands in for the albedo.
- Porosity p: grey-ness g = 1 - saturate(1.5 x chroma), chroma = (max - min) / max of the albedo. Land (4) lerp(0.6, 0.9, g x smoothstep(0.06, 0.30, albedo luma)) (bright grey sand and dirt 0.9, dark asphalt and rock 0.6); objects (1) lerp(0.3, 0.6, g), times lerp(0.35, 1, saturate((roughness - 0.2) / 0.5)) where a roughness is written (smooth = sealed); cloth (3) 0.85; plants (5) 0.15 (waxy); unknown lerp(0.4, 0.7, g). Times (1 - metal).
- Darkening (Lagarde 2013, "Water drop 3b", floor 0.3 instead of his 0.2 because it scales the lit picture, ambient included; with a saturation term after Lekner and Dorf 1988: light that bounces inside the water layer meets the albedo again): darken_c = min(lerp(1, 0.3, p w) x (albedo_c / luma(albedo))^(0.35 p w), 1). Applied to the lit colour's diffuse share: colour x lerp(1, darken, diffuseShare), diffuseShare = (1 - metal) x lerp(0.8, 1, roughness) (0.95 with no roughness). Puddle bottoms take it at w = 1, then Amount, the depth tint and the rim as before.
- Water layer: F0 0.02; roughness lerp(dry, 0.08, w (1 - p / 2)) (dry = the buffer's roughness, else 0.7; cloth at least 0.6), lerp to 0.05 in puddles. Coverage film = max(w, puddle x LODfade).
- Sky and SSR: environment = the sky functions (GetSkyColor with the direction's height, the horizon and the sun's side) in Karis's dominant direction lerp(N, R, (1 - a)(sqrt(1 - a) + a)), a = roughness^2, dimmed to 0.2 below the horizon; the cloud texture modulates it over puddles (x 0.35 to 1.25); then the sky visibility (below) mixes in the facades the reflection meets; the SSR hit (fixed trace) replaces the result by its coverage (no 0.55 cap). Weight F = EnvBRDFApprox(0.02, roughness, N.V) x film (Karis 2014); the light through the layer keeps (1 - F): out = ground x (1 - F) + environment x F + highlights. No 28 % cap, no luma limiter.
- Sky visibility (2026-11-05, QA finding 16: with FixedReflectionTrace off, the default, there is no SSR, and grazing street views showed a full sky sheen where buildings would be reflected): the ortho map is read as a height field along the reflection ray (WetSkyVisibility). From the surface moved 4 units out along its normal, the ray in the dominant direction Rd is probed at 128 and 768 game units; a probe below the map's top surface is inside a facade or under a roof or awning: open = saturate(1 + (top - z) / s), s = 48 game units in ortho depth (GetOrthoDepthScale); outside the map it counts as open. vis = 0.5 (open(128) + open(768)) x lerp(0.7, 1, smoothstep(0, 0.25, Rd.z)) (the horizon term: a ray along the horizon mostly meets far terrain and buildings past the probes). environment = lerp(occluder, sky, vis), the occluder stood in for by the pixel's own dry lit colour (lit by the same light as the facades around it). Two point reads of the ortho map per wet pixel, no loop.
- Sun and lamps: one GGX / Schlick-Smith / Schlick (F0 0.02) lobe x N.L x PI (the game's lights give albedo x colour x N.L on a diffuse surface), with the roughness at least 0.1 (the sun and lamps are not points) and the normal's variance across the pixel added to alpha^2 (Tokuyoshi and Kaplanyan 2019, at most 0.18), so ripples sparkle instead of flickering. Sun: x the sun colour x the shadow term as before; lamps: x (1 - d / r)^2 x the lamp colour. The sum x film is limited to 4.0 (linear) per channel. The 24-lamp loop (unrolled) runs only where a lamp slot is in use and the film is above 0.01 or a wall streak is present (QA P8): below that the highlights cannot show. The lamp attenuation's radius floor depends on the pixel (max(r, 1e-6 d + 1e-6), an empty slot then gives 0), so the unrolled loop makes no per-slot preshader values: with them the Persistent + fixed trace variant went past ps_3_0's 224 float constants.
- Normal: the depth normal with a bump from the albedo's brightness (surface gradient, Mikkelsen 2010: height = luma x 1.5 game units, tilt at most 0.35) where the albedo exists, flattened to up in puddles, with the ripples and puddle normals where flat ground is wet (as before, weight max(puddle, flat wetness)).
- Refraction: only in puddles (x 0.35 of the old offset, as the old puddles had); plain wet ground is no longer shifted.
- Streaks: unchanged (their darkening, normal offset and Blinn lobes).
- Kept as is: KeepHighlights (WetOut), the far and water exits, the first-person composition, splashes, Persistent's cover map reads (the wetness is read at the moved-out point), the rain-amount quirk (section 6).

## 7. Vulkan path
- Shaders: src/hlsl/NewVegas/Compute/WetWorldMap.frag (the wet map and its two blur passes, passIndex 0, 1, 2), src/hlsl/NewVegas/Compute/WetWorld.frag (the wet pass, with mode bit 0 = fast, bit 1 = fixed trace) and src/hlsl/NewVegas/Compute/WetWorldCommon.glsl (the shared constants block, included by both). Compiled forms: WetWorld.spv and WetWorldMap.spv in the same folder. Pass names requested: "WetWorldMap" (the three map passes) and "WetWorld" (the wet pass).
- Selection (src/core/EffectRecord.cpp, EffectRecord::Render, and this effect's ExtRender): the Vulkan path runs only for techniques 2, 3 and 4, and only when [Main.Main.Performance] NvrDxvkPasses is on, the frame is not a render-rectangle frame, the Vulkan extension is available, the effect name is not in NvrDxvkKeepD3D9, and the source buffer is the frame's source surface with the needed textures present. Technique 0 (the original pass), the cover test (1), and the persistent techniques (5 and 6) always run as D3D9 passes. With FastPaths on and NvrDxvkPasses on, the normal frame therefore runs the Vulkan path (technique 2, or 4 with the fixed trace).
- The C++ side is src/effects/WetWorld.cpp, ExtRender. It creates the two pipelines once, copies the constants as the D3D9 constant table holds them (matrices are transposed into the block so that the shader's matrix products match the D3D9 meaning), and draws: the map pass, two blur passes (each reads the image the previous pass committed), then the wet pass (which may write the game target directly). Failure before the first pass returns false (D3D9 runs); after the first pass has drawn, a later failure keeps what is on the target.
- Textures, wet map passes: set 1, bindings 0 to 4 (rendered, depth, view-model depth, ortho map, normals). Wet pass: set 1, bindings 0 to 12 as listed in section 3.
- Samplers: four samplers in order, each addressed by a push-block index at offsets 0, 4, 8 and 12: linear clamp (0), point clamp (1, the normals), linear mips wrap (2, the ripple, streak and puddle normal and cloud textures), linear mips clamp (3, the splash sheets). The wet map passes use the first two.
- Uniform block: set 2, binding 0, std140, 1376 bytes (checked by the source), with these fields in order: shadowToLight, projection, invProjection, view, invView (mat4 each); cameraData; depthConstants; cameraPosition; reciprocalResolution; gameTime; sunDirection; sunPosition; sunAmount; sunColor; sunAmbient; skyData; skyColor; skyLowColor; horizonColor; orthoData; waterSettings; wetWorldData; wetWorldLook (each vec4); lightPosition[12]; shadowLightPosition[12]; lightColor[24].
- Push block (32 bytes):
  - wet map passes: four sampler indices (0, 4, 8, 12 bytes: linear clamp, point clamp, unused, unused) then passIndex (0 wet map, 1 blur across, 2 blur down).
  - wet pass: sampler indices (linear clamp, point clamp, mip wrap, mip clamp) then mode (bit 0 fast, bit 1 fixed trace).
- The Vulkan wet pass has no original reflection trace (technique 0 is not on the Vulkan path). Its fast variant traces only when the fixed trace is on, as the D3D9 fast path does.
- Model 1 (techniques 7 to 10) has no Vulkan pass yet: ExtRender declines them and the D3D9 pass draws, under the DXVK fork too. A port needs two more bindings (13 material, 14 albedo), TESR_MaterialBufferData / TESR_AlbedoBufferData in the uniform block (its size check changes), and WetPBR in WetWorld.frag; under SPIR-V the sampler limit also goes, so TESR_CapturedNormalsBuffer could be bound for its distance test (the material read then known to be the visible surface's).

## 8. Look and cost
- Correct look: rain darkens and glosses the wet ground in the open and leaves roofed and dry ground alone; puddles form in hollows and on flat ground, reflecting the sky and clouds, with sun glints and lamp highlights; ripples move with the rain and build up as it intensifies; splashes flicker on flat wet ground at the rain's density; thin streaks run down walls; the picture is not clipped at white around bright highlights (KeepHighlights on); the first-person weapon's surface ripples in the rain; puddles dry after the rain stops (Decrease).
- Typical wrong results: wet ground under roofs (roof test broken); puddles on slopes (tilt filter broken); puddles reflecting the wrong places or hard reflection patches at the screen edge (trace or edge fade broken); wetness that does not dry off when rain ends (rain amount quirk, see section 6); bright highlights clipped flat (KeepHighlights off or highlight limits wrong); splashes or streaks on the wrong surfaces (up-component ranges); a grey film on the whole screen in rain (base colour or the wet blend broken); reflections on water (water band broken).
- Model 1 correct look: asphalt clearly darker with a sharp sky band at grazing angles; sand and dirt much darker and slightly more saturated but matte up close; painted metal and car bodies barely darker but glossy; concrete darker and blotchy (the albedo bump); grass and leaves only slightly glossy; cloth darker and matte; walls wet but less than the ground; roofs, overhangs and characters dry; puddles see-through looking down and mirror-like near the horizon. Grazing views down a street: the film shows the sky only where the reflection can reach it (above the facades); towards the buildings it reflects a darker, building-coloured sheen. Wrong: wet walls under a roof or porch (the moved-out roof test failing), dark wet characters or streaks and splashes on them (class not written; with the buffer missing or MaterialBufferSkin off the frame should be in the Model 0 look and the log says why), bright flickering dots on ripples (the variance term), a sky-bright sheen on a street between tall buildings (the sky visibility failing: ortho map missing or out of reach), walls whose wet sheen ends at a visible line around 1200 to 1600 units (the wall fade too short), a wet look everywhere identical (the albedo buffer missing: check TESR_AlbedoBufferData).
- Model 1 cost: the same structure as FastPaths plus two point reads, one ortho read (the moved-out roof test), the GGX lobes (sun, up to 24 lamps, unrolled, only where the film is above 0.01 or a streak shows), two more ortho reads for the sky visibility and the sky function per wet pixel; dry, far, water and sky pixels exit as before, and so do walls past 1600 units (wall fade). fxc ps_3_0 sizes (2026-11-05, after the QA fixes): techniques 7 / 8 / 9 / 10 about 2601 / 2841 / 2625 / 2866 instruction slots (2520 / 2759 / 2545 / 2782 before them: the sky visibility adds about 80 static slots; the lamp gate and wall exit cut the work per frame). Not measured in game.
- Cost class: multi-pass with a per-pixel loop. Map pass plus two blur passes (cheap), then a per-pixel wet pass with about 14 to 20 reads of the ripple, splash, streak and cloud textures, the 24 lamp terms (in the fast path, only where a lamp reaches the pixel, skipped for the whole screen when no lamp is in use), and the reflection march (8 steps with refinement, only on pixels that can reflect). The source notes say FastPaths saves about 1 ms at 2560 by 1440 in rain, and the fixed trace costs about 0.1 to 0.2 ms more in rain.

## 9. Open questions
- The source notes say the original reflection trace (technique 0 and the fast path without the fixed trace) produces no reflection on the GPU, because its loop exits do not work. The Vulkan path follows that behaviour (no trace unless the fixed trace is on). Decide whether the new version should match this, or implement the fixed trace as the intended behaviour.
- The rain-amount quirk (section 6): after rain stops the wetness stays at full for the whole rain fade-out. Confirm whether that is intended before a new shader copies it.
- TESR_WetWorldCoeffs is registered and set from settings (PuddleCoeff_R, G, B and PuddleSpecularMultiplier), but no shader reads it. Those settings are shown as not used. Decide whether to retire them.
- TESR_SunAmount is declared but not read.
- The first-person drops (HudDrops, the HudDrop keys) are compiled into the object shaders (ShaderRecord and the HudRain include), not into this effect; this spec describes only the cover test they depend on. Their own spec is not part of this wave.
- The persistent map (CoverMap.cpp, CoverMap.h, CoverMap.fx.hlsl) is a separate effect that this one reads through the cover map inputs; this spec describes only what the wet pass reads from it.
- The sky's depth value in the combined depth buffer is not confirmed here; the wet pass treats the sky as beyond its far limit (0.9 of the ortho reach).
- Whether the wet pass draws in menus is not confirmed: the effect's own gate has no menu test.

## 10. Status notes (corrected 2026-11-05)
- This section used to describe a "wave 3 rewrite" (branch claude/rw3-weather: the rain-amount quirk fixed, the ground darkened by 0.3, no Vulkan path). That code is not in this branch (rain-system, from upstream 1.0.3): WetWorld.fx.hlsl here is the upstream pass with FastPaths, the fixed trace and Persistent (techniques 0 to 6, the quirk of section 6 still in them), plus Model 1 (techniques 7 to 10, 2026-11-05). The Vulkan path of section 7 exists (Compute\WetWorld*.frag, .spv) for techniques 2 to 4.
- Model 1 is compile-checked only (tests effect_compile, D3DX 9.43, both FORWARD_SHADOWS values); not yet seen in game. 2026-11-05 QA fixes (docs/rain/research/06-07-qa-performance-review.md findings 15, 16, P8): the Model 0 fallback without the material buffer, no streaks or splashes on characters, the wall fade, the lamp gate and the sky visibility (sections 5 and 6). Research and the formulas' sources: docs/rain/research/04-wet-surfaces.md and 04b-wet-surfaces-implementation.md.
