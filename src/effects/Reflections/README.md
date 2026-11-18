# Reflections

Screen-space reflections on the ground (wet streets, dry sheen, interior floors, materials that reflect), with ray-traced reflections
on top where the RT core runs. Half-resolution trace, history and blur into the effect's own buffers, then a full-resolution composite
in the HDR chain (after Specular, before the fog). Started 2026-11-07 (this README did not exist before; the RT path's long history is
in docs/next/FORK-RT-REFLECTIONS.md).

## Frame (RenderBuffers)

1. The surroundings cube, the light grid's constants, the sky panorama.
2. **Trace** (technique 1, `Compute/ReflectionsTrace.frag` / the .fx) into Trace (Temporal on) or Temp. With RT
   (`[Shaders.Reflections.RT] Enabled`, the NVR DXVK fork, the RT core's helper): **RayGen** (`ReflectionsRayGen.frag`: the march,
   then Dir / Ray / March / MarchHit) -> the helper (`Compute/Rt/ReflectionsRT*.comp`) -> **Resolve** (`ReflectionsRTResolve.frag`:
   the hits shaded from the screen, the cube or the estimate; Spread = x the hit's distance share, y the hit distance).
   (2026-11-07) The hit normal the estimate is shaded with (sun N.L, lamps, grid / cube) is bent by the hit's own normal map in the
   textured variants (`ReflectionsRTTex*`) when the core's `[Main.Main.RayTracing] RealNormalMaps` is on (default, with RealTextures):
   bricks and panels off screen keep their bumps in mirrors. The shadow rays still leave from the flat triangle normal.
3. Smoothing, one of:
   - **the RT reflection denoiser** (`[Shaders.Reflections.RT] Denoiser`, default on with RT; core/RtReflDenoise.h,
     docs/next/RT-REFLECTION-DENOISER.md): 4 fork passes on frames the RT trace ran (native frames, not under DLSS RR), result copied
     into Buffer. Core 0 ours (ReBLUR-style: hit-point reprojection, long history, lobe-sized blur), core 1 AMD FidelityFX
     Reflections (ported, A/B). With it on, RayGen's glossy rays use decorrelated noise (the old pair sampled one line of each lobe).
   - **the old chain**: Temporal (technique 3: surface reprojection, 3 x 3 clamp, `Temporal` 0.85) -> Blur X / Y (2 / 4: depth-aware
     Gaussian, `BlurRadius` x roughness, narrower at RT contact). Always used without RT, in RenderScale rect frames, under DLSS RR
     (raw: no blur), and with `Denoiser false`.
4. The composite (technique 0) reads Buffer (water shaders read it next frame with RT Water).

## Settings
`[Shaders.Reflections.Main]` (the look and the SSR trace), `[Shaders.Reflections.RT]` (the RT trace, its fallbacks, and the denoiser:
`Denoiser`, `DenoiserCore`, `DenoiserHistory`, `DenoiserMirrorHistory`, `DenoiserBlur`, `DenoiserAntiLag`, `DenoiserClip`,
`DenoiserDebugView`; see defaults.toml for each).
- (2026-11-07 perf-history, docs/next/RT-PATH-TRACING.md "perf-history") `DenoiserHistory` and `DenoiserMirrorHistory` are frames AT 60
  FPS: with `[Main.Main.RayTracing] DenoiseTimeBased` (default on) the denoiser keeps the same seconds at the game's frame rate (32 -> ~11
  frames at 20 fps, at most 64 above 120 fps). At 60 fps nothing changes.
- (2026-11-07) The RT rays' reach is FarRadius when the core holds the far ring: FarObjectLOD, FarLandLOD, or PTRealtime (which turns
  FarLandLOD on by itself, docs/next/RT-DETAIL-GAPS.md 7).
- (2026-11-07) Decals (`[Main.Main.RayTracing] Decals`, with AlphaTested: the ReflectionsRTAlpha* variants): the reflection rays see
  posters, cracks and blood (lifted 0.5 unit off their surface); the sun / key-lamp shadow ray at a hit passes them
  (ReflectionsRTBody.glsl RtProceedShadow). RT host scene `decals`.
- (2026-11-09, roadmap track 1 step 2) `[Shaders.Reflections.Main] TemporalJitterFix` (on): the old chain's Temporal pass reads the
  history where the change of TAA / FSR / DLSS's jitter since last frame puts it (as AmbientLight's). Done in C++ only:
  `TemporalBackbone::ShiftReprojection` shifts `TESR_ReflectionsReproject` (the matrix the .fx and `Compute/ReflectionsTemporal.frag` both
  read), `lastJitter` is kept with `lastView`. The RT denoiser's own reprojection is not shifted (not measured). Off: as before. Without
  TAA's Jitter the shift is 0. Wrong would look like the reflection drifting by a pixel each frame on a still camera with TAA on.

## One helper round trip a frame: RideMainExchange, FollowRenderScale (2026-11-11; both default off, NOT yet run in a game)
Cloud bench 029 (L4, 720p): RT reflections' helper pass costs 0.01-0.08 ms, but the effect made its own second exchange with the helper
every frame ("Reflections RT (exchange)" 0.7-1.1 ms, beside the main "RT exchange: helper round trip" 1.1-2.6 ms).
- **Why a second exchange**: the helper's inputs are RayGen's images (Rt.Dir, Rt.Ray), and RayGen ran in the Reflections step of the
  chain, after RtCore::Frame, because its screen march reads the lit picture. With `RaysOnly` (default) the march is skipped; the rest
  of RayGen reads the depth, normals, first-person depth / weapon mask, glass mask, TESR_MaterialBuffer, the ortho map and MaterialAware's
  material buffer: all there before RtCore::Frame, except the material buffer, which CaptureMaterial fills at the chain's start.
- **`[Shaders.Reflections.RT] RideMainExchange`**: `RtRideBeforeFrame` (EffectChain PreStep_RtCore, right before `RtCore::Frame`) runs
  CaptureMaterial early (TESR_RenderedBuffer pointed at the game target's own texture for that pass; the chain's MaterialCapture step
  then returns at once), draws RayGen (`RtRayGen`, RtTrace's first half) and arms a second core user, "RT reflections (main exchange)"
  (Point 0, no Setup, added last so it never takes a pass from another effect). `RtCore::Frame` records it with the others; `RtTrace`
  sees the frame rode and goes straight to Resolve. Same frame's data: no latency. Falls back to the old exchange (per frame) when:
  RaysOnly off / an RT DebugView / SplitTest, the path tracer replacing the scene, the scene target not a single-level texture with
  MaterialAware on, the main exchange's 8 passes full, the main exchange not run. Log: `Reflections: RideMainExchange: RayGen is drawn
  before the frame's main RT exchange ...` or `... is on but not used now: <why>` (a line per change); with the F10 profile a count
  line every 600 armed frames. Correct: the same picture as off, the "Reflections RT (exchange)" profiler line gone (no samples),
  the helper log's "runs" per pass equal for ReflectionsRT* and the other passes. Wrong would look like: reflections on surfaces that
  should not reflect / missing on mirrors (the early material capture read something else), or reflections lagging a frame.
- **`FollowRenderScale`**: the trace, RayGen, Resolve, Temporal, the blurs and the composite have kept to the RenderScale rect since
  2026-11-03 (the bench's own log: "RT traced in the RenderScale rect: 428x240 of 640x360"); what did not shrink is the round trip.
  The key only makes the five image copies (Dir, Ray in; Hit, Shade, Bounce out) take the rect too. Log: `Reflections:
  FollowRenderScale: the RT images are copied ... in the RenderScale rect only`. Still not in rect frames: the RT reflection denoiser
  (its passes have no rect rules: reprojection, taps and history would need rectToScreen / screenToRect and a rect clamp in all seven
  RtReflDenoise*.frag), MirrorJitter, weapon rays.

## MaterialAware: what owns the scene's alpha (2026-11-07, perf-mirrorfix)
The per-pixel material (MaterialAware, core/ReflMaterial.h) lives in the world target's alpha: 2 + 0.25 x metal + 0.24 x roughness,
written by the first pass of an object with an `_rmaos` map; [2, 3) reads as a material (CaptureMaterial, technique 7), anything else
as none. The game draws the world without alpha writes, so before this fix every OTHER draw left the alpha it found: whatever stood
in front of a mirror panel (a person's shirt, a fence post, hair, the first-person gun) kept the panel's material and turned into a
mirror (tester, VSRTRoom mirror wall: a chrome shirt with a noisy reflection painted on her back; a fence post showing the room).
Now (core/ReflMaterialRule.h, applied round every object pass: the two pass hooks, BSBatchRenderer::RenderPassImmediately, the grass
and depth-first replays, and the first-person scene):
- material pass (opaque, untested, a map, world scene): its material, as before;
- every other pass whose colour is not blended by its alpha property: alpha writes on, its own alpha (the game's 0..1: no material);
  the lamp / highlight passes the device blends keep the alpha under them (the scene's separate alpha blend ZERO / ONE);
- a blended first pass of a skinned geometry (hair): the material under it x (1 - its alpha) (separate alpha INVSRCALPHA for that
  draw): solid hair clears it, its see-through texels keep it;
- blended static draws (glass, decals, particles), depth-only passes: alpha left alone (a mirror seen through a window still reflects).
Log (every 1800 frames): `MaterialAware reflections: per frame N other passes wrote their own alpha (no material), M blended skinned
passes covered it`. Correct: `DebugView 7` (the material buffer) shows the mirror panels only, with every person / post / gun in front
of them black (no material); wrong: a person's outline filled with the mirror's colour in DebugView 7.
(2026-11-07 perf-mirrorfix2, test 10: her head and hair still took the mirror) the same rule also runs at DRAW time from the device's
state (Render.cpp MaterialDraw round the skinned partition and still-object draws: the sorted blended list, which holds actors with a
blend property and hair, never came through the pass hooks; ReflMaterialRule::DecideDraw). The log lists each distinct kind of draw
once a session (`MaterialAware reflections draw (session list N)`): a `KEEPS the alpha under it` line for an opaque or skinned draw is
the one to look at if something still shows the mirror. RT-PATH-TRACING.md section 68.
Known gap: the path tracer's G-buffer (CapturedNormals MaterialBuffer / AlbedoBuffer) keeps the same leak for draws that write no
G-buffer (the game's own shaders, hair, blended draws): docs/next/RT-PATH-TRACING.md section 67.

## What correct / wrong looks like (the denoiser)
- At rest, a glossy floor's reflection settles within a second to a clean blur that is sharp where an object stands on the floor.
- Turning or walking: the reflection stays attached to the reflected object (no trail dragged along the floor); after a door opens or
  a light changes the reflection catches up within a few frames (anti-lag).
- Wrong: a reflection that smears along the floor when you move (the hit-point reprojection failing: `DenoiserDebugView 5` shows
  where it is used, green), a stuck ghost after a change (anti-lag), blotches that crawl (history too short: `DenoiserDebugView 3`).

## Tests
- RT test host scene `refldenoise` (`tools/nvr-rt-host/run.ps1 -Scene refldenoise`): the denoiser vs the old chain vs the exact glossy
  reflection, still and moving (no game).
- RT test host scene `normalmaps`: the RT trace's hit normal against the CPU's bend of a striped normal map (and flat with it off).
- RT test host scene `hairmirror` (perf-mirrorfix): an actor's alpha-tested, tinted hair card in a mirror: reflection and sun rays
  through its gaps, its tint, the old opaque packing as the control.
- `tests/refl_material_rule.cpp` (cl, no game): the per-pass alpha rule on a simulated mirror pixel (shirt, post, hair, lamp passes,
  glass, first person, MSAA edge), with the old rule's leak as controls.
- Bench lists `rrdn-still`, `rrdn-turn` (bench/foundations/lists; run script bench/deep-roadmap/rtrefldn/run_rdn.sh).

## Mirrors: smooth hits, metal that reflects, rays only (2026-11-07, perf-reflfix)
Tester (RTX 4090, test 9, the RT test room's mirror wall): spheres in the mirror were FACETED, the chrome sphere a plain white ball and
the gold one banded orange, neither reflecting anything. docs/next/RT-PATH-TRACING.md "perf-reflfix" has the whole change.
- **Smooth hits**: the RT helper now keeps a vertex normal per vertex (tools/nvr-rt-helper/RtNormals.comp, made from each mesh's own
  triangles); the helper's pass (`ReflectionsRTBody.glsl` ShadeHit) shades a hit with them interpolated (RtHit.glsl
  `rtHitSmoothNormal`), then the normal map; the shadow rays still leave along the flat triangle normal.
- **MetalBounce** (`[Shaders.Reflections.RT] MetalBounce`, default 0.5 = the roughest metal that bounces; 0 off; live): a ray that meets
  smooth metal (the material rules' metal >= 0.75 at roughness <= MetalBounce: chrome, gold / brass / copper) sends ONE more ray along
  the mirror direction about the metal's normal. The pixel is then the metal's Fresnel tint (Schlick, F0 = its colour) x what that ray
  meets: the screen there when the screen shows it (Resolve `RtBounceColour`), else that hit's estimate, or the sky / panorama / cube
  along it when it meets nothing. One bounce only: mirror -> chrome -> the room is shown; a second metal behind that is lit, not
  reflected again (cost: one ray + one shadow ray, only on pixels whose ray meets such metal). The hit image's y then carries 256 (+ 512:
  the bounce met nothing), its zw the bounce's direction, the shade image the bounced hit's colour, and the new **bounce** image
  (RGBA16F, the helper pass's last image; Resolve set 1 binding 26, slot 25) the tint + the bounce's distance.
- **Screen reuse at a hit**: Resolve took the screen's colour wherever the hit's depth matched within ScreenTolerance; on a sphere seen
  in a mirror the hit is the far (mirror-facing) side, and near the outline it matched the front the camera sees. Now the hit's normal
  must face the camera (`RtScreenShows`, -0.15 for normal-map tilt).
- **RaysOnly** (default) skips RayGen's march altogether (its answer was dropped in Resolve on every pixel anyway): the same picture,
  the march's cost saved. Mirrors take the rays alone.
- What correct looks like: spheres in the mirror round, without bands; the chrome sphere shows the room and the red sphere in it, the
  gold one the room tinted gold. Wrong: chrome still plain white (no material rule for its texture: the bounce needs the rule's metal
  bits; `[Shaders.PBR.MaterialRules] DebugView` shows the class), bands (an old helper: the new one logs "smooth normals: on" at start).
- Not done (follow-up): the REFLECTING surface's own normal map. RayGen reflects about the screen's normal (TESR_NormalsBuffer /
  CapturedNormals), which is the smooth vertex normal BEFORE the normal map (CapturedNormals.h); no buffer holds the mapped normal.
  Bumpy floors reflect as if flat until the material pass also writes the mapped normal (CapturedNormals target 3 or the material
  buffer's free g / a, DEFERRED-RENDERING-PLAN.md); then RayGen can lerp toward it by (1 - roughness) for mirrors.
- RT test host scene `reflfix` (`tools/nvr-rt-host/run.ps1 -Scene reflfix`).

## Lamps as the game lights them, glossy metal bounces, the flashlight (2026-11-07, perf-reflfix2)
Tester (RTX 4090, test 10, the RT test room) and the flashlight team. docs/next/RT-PATH-TRACING.md "Team perf-reflfix2" has the numbers.
- **Lamps at hits the screen cannot show** (Resolve `RtShadeHit`, the helper's key-lamp pick): `RtGatherLamps` packs each lamp through
  `RtReflLamps.h` as the game lights it: diffuse x Dimmer at most 1 without HDR x the LOD dimmer x LightingScale, decoded on the CPU
  (linear), the game's falloff `(1 - (d/r)^2)^2.2` or LampFalloff's shape; `Rt/RtLampLight.glsl` evaluates it in both passes. Before:
  the dimmer unclamped and decoded with the colour (the room's lamps have fade 1.8-2: 4.4-8.9x the game's light): white discs on the
  ceiling near each lamp in the mirror ("orbs"), discs on the chrome and gold spheres.
- **MetalBounce's direction**: a GGX sample of the metal's roughness about its normal (visible normals), a new one each frame while the
  RT reflection denoiser (or DLSS RR) averages the frames (Pass `frame.y`); without one the exact mirror direction. Gold (0.33) is now
  as blurred in the mirror as seen directly; chrome (0.08) stays sharp. The RT records' roughness code is `(k / 7)^2` (finer near 0).
- **HitFlashlight** (`[Shaders.Reflections.RT]`, default true, live): the flashlight (FlashlightDeferred::LightParams) is lamp 0 of the
  hits' lamps, with its cone and its pass's falloff, never the key lamp (no shadow ray). Its beam's spot shows in mirrors and on metal.
- **KeepFlashlight** (`[Shaders.Reflections.Main]`, default true, live; the Vulkan composite only): the composite adds back the share of
  the flashlight's GGX highlight that mixing the reflection over the lit colour took away (no reflection ray sees the light itself).
  Lamps' highlights are not kept (the composite has no lamp list).
- What correct looks like: in the RT test room's mirror the ceiling over a lamp as bright as on screen, no white dots; gold in the mirror
  blurred like gold seen directly (a little grain that settles in a few frames with the denoiser on), chrome sharp; the flashlight's spot
  on the walls seen in the mirror, and its small highlight on a mirror / chrome / gold surface it shines on. Wrong: a speckled gold
  sphere (the denoiser off: the bounce then stays sharp, report it), the beam's spot on screen but not in the mirror (HitFlashlight off,
  or HitLamps off).
- RT test host scenes `reflorbs`, `reflbounce` (`tools/nvr-rt-host/run.ps1 -Scene reflorbs`).

## Steady gold in mirrors, the flashlight near the player, undrawn hair (2026-11-08, perf-reflfix3)
docs/next/RT-PATH-TRACING.md "Team perf-reflfix3" has the numbers.
- **MetalBounce on rough metal** (roughness 0.1 and up: gold, brass): the bounce's GGX sample is fixed per pixel (one of 9 lobe points by
  its place in a 3 x 3 block) and Resolve averages each bouncing pixel with its neighbours on the same metal: the lobe's 9-ray mean, the
  same every frame (test 11 drew a new sample each frame and a mirror keeps no history: the gold shimmered). Chrome keeps one exact ray.
- **HitFlashlight**: no light within 24 units of the flashlight, full from 48 (the reflected head was a white orb). KeepFlashlight's glint
  is capped at 2 linear (was 8).
- **Hair**: RT hair keeps its tint while the game is not drawing it (first person: the player's hair from the player's hair colour).
  (perf-reflfix4: this never ran for the hair, which is rigid; see below.)
- What correct looks like: the gold in the mirror smooth and still (a fine blur of the room, no sparkle); no orb round the reflected head
  (the flashlight's spot on the walls stays); the player's hair its own colour in chrome and mirrors in first person. Small bright dots at
  the lamps in mirrors and chrome are the surface's own highlight of each lamp (as on the gold on screen), not a fault.
- **MirrorLampHighlight** (`[Shaders.Reflections.Main]`, default 0, live; the Vulkan composite): the point lamps' own highlight on mirror-smooth / chrome surfaces the reflection covers (the blooming dots) is taken out before the mix (above 1.0 linear and above its 12-pixel surroundings); the sun's and the flashlight's glints stay, the gold's soft highlights (roughness 0.15 and up) are not touched. 1 = as before. Host scene `mirrorlamp`.

## Silver mirrors, smooth gold, hair in its colour (2026-11-08, perf-reflfix4)
docs/next/RT-PATH-TRACING.md "Team perf-reflfix4" has the facts (each checked against the game's code / files) and the numbers.
- **Metal's F0 (MaterialAware)**: the composite took a metal's reflection colour from the hue of its own lit colour; on a mirror (a
  near-black diffuse) that hue is the lamps', and the whole reflected room went orange under warm lamps. Now (`Compute/ReflMetalF0.glsl`,
  the D3D9 composite's `ReflMetalF0`, MetalBounce's F0 in the RT pass) a metal up to roughness 0.06 reflects in neutral silver (0.95),
  blending into its colour from 0.06 to 0.15; rough coloured metal (gold 0.2-0.5) keeps its colour.
- **Rough metal's bounce average** (Resolve): a 5 x 5 tent (weights (3 - |dx|)(3 - |dy|)) in place of test 12's 3 x 3 box, which made 3-texel
  blocks and staircase edges in the gold (each of the 9 lobe directions held on a 3-texel lattice); first hits within 25 % + 32 units.
  Still steady (the fixed 9-point lobe). Host scene `reflsmooth` (pictures in out\reflsmooth_*.pfm), `mirrorf0`.
- **Hair** (core/RtHairTint.h, RtCore HairTintOf): the vanilla hair, beards and brows are rigid shapes (no skin instance), so the tint
  look-ups on the skinned path never met them. The tint is now read from the hair's own shader property (+0x84: what the game's hair
  shader is given as HairTint, written from the NPC's hair colour when the head is built), for the player and NPCs, first and third
  person; the rigid hair goes in as a TINTED record like the actors'. Log: `RT CORE hair (perf-reflfix4): ...` per hair part (first 8)
  and a summary at exchanges 60 / 600 / 6000. The hair's texture slot 2 is its layer map (_hl.dds), not a glow map: taken as one, the glow
  variants replaced the tinted colour with it (white hair). `RT CORE hair record (perf-reflfix4)` shows the flags as sent (tinted 1, glow
  0); `UNOFFICIAL RT reflections (perf-reflfix4): trace variant ...` the variant. Host scene `hairglow`.
- **Blocky mirrors: MirrorJitter** (`[Shaders.Reflections.RT] MirrorJitter`, default 1, + `MirrorJitterHistory` 8): a mirror pixel's ray
  leaves from one of 8 points of its half-resolution texel frame by frame (Compute/RtMirrorJitter.glsl: RayGen, the helper, Resolve) and the
  RT reflection denoiser keeps a short history on mirrors (virtual reprojection, anti-lag, a 1.25-sigma variance clip): outlines and hard
  shadow edges smooth instead of stepping every 2 output pixels. Only where the denoiser runs (native resolution; 2 = also under DLSS
  RR); rect frames keep the fixed centre ray. Log `UNOFFICIAL RT reflections MirrorJitter (perf-reflfix4): ...`. Host scene `mirroraa`.
  What wrong looks like: a soft trail behind a fast-moving person in a mirror (lower MirrorJitterHistory), shimmer on mirror edges
  (MirrorJitter 0 to compare).
- **Shader files check**: `UNOFFICIAL Reflections shader files (perf-reflfix4: ...)` once at start: each of the changed .spv (and the
  D3D9 Reflections.fx.hlsl) "this build's" or "OLD (copy the new one)".
- What correct looks like: the mirror's reflection the room's own colours under any lamp (white walls stay white under the warm lamps);
  the gold in the mirror and in the chrome sphere smooth (no blocks, no staircase on the red sphere's reflection) and still; the player's
  and NPCs' hair in the colour the game draws it. Wrong: the room orange in the mirror again (an old ReflectionsComposite.spv: the log
  line says so); blocks in the gold (an old ReflectionsRTResolve.spv); white hair with `tint from none` in the hair lines (the property
  held no written colour: the line shows what it held).


## Replace, not add: ReplaceForward (2026-11-12; default off, NOT yet run in a game)
What the frame already holds when the composite runs, and what the composite does with it (`lin = mix(lin, reflected, share)`, share =
saturate(amount x the split-sum Fresnel of the material's F0 and roughness) x the trace's coverage; ReflectionsComposite.frag / the .fx):
- **The game's env-map pass** (SLS2057 / SLS2058 = `Shaders/EnvTemplate.hlsl`, an additive pass after the object's lit pass: its cube map x
  the mask x the artist's strength, by the light with `[Shaders.PBR.EnvMaps]`). DOUBLE-COUNTED: the mix only takes `share` of it away,
  and on stone, tile or glass seen straight on that is about 4 %, so the painted cube map stayed almost whole under the real reflection.
- **Metal's surroundings** (`Includes/Object.hlsl` getMetalAmbient, added in ObjectTemplate.hlsl's lit pass: sky / ambient / the live cube x
  EnvBRDFApprox). Mostly replaced already: on metal the share is its F0 (0.5-0.95), so 5-50 % of it is left under the reflection. Not
  changed here (the object shaders are at their instruction limits and have no per-pixel knowledge of the reflection).
- **The Specular effect's sky reflection** (`Effects/Specular.fx.hlsl` SkyStrength, outdoors, the chain step before this one). Same as
  metal: (1 - share) of it is left; at glancing angles, where it is strongest, the share is near 1. Not changed here (another effect).
- Lamp and sun highlights are lights, not the environment: the mix dims them by the share too (KeepFlashlight puts the flashlight's back).
- A ray that finds nothing: the surroundings cube, the light grid, then the sky fill the missing share (the trace / Resolve); what they
  cannot fill stays out of the alpha, so the surface keeps its own colour there (never black).

**`[Shaders.Reflections.Main] ReplaceForward`** (live): the env-map pass reads LAST frame's Buffer at its own pixel (the pass is drawn
before this frame's trace, as the water with RT Water) and leaves out `alpha` of its cube map: where the effect answered the reflection
(screen, rays, its fallbacks) the surface shows that one reflection; where it did not (the surface does not reflect for this effect: no
material, too rough, past DrawDistance; or nothing was found) the game's cube map stays as the probe. The composite is unchanged.
- C++: `UpdateWaterConstants` makes the frame's value (TESR_ReflReplace: x 1, zw one scene pixel in UV) when last frame's composite drew
  from that Buffer (the effect on, drawn last frame, no camera jump, no DebugView, not under water, not while the path tracer replaces the
  scene); `core/ReflMaterial.h` hands it to the shaders only inside the frame's first world scene and the first-person scene (the planar
  water reflection and any other scene get 0). Log, a line per change: `UNOFFICIAL Reflections ReplaceForward: on ...` / `... not used now: <why>`.
- Off: the constant is 0, the shader's branch is not taken, the picture is as before. The pass itself is a little larger than before
  (one constant, one sampler on s11, a skipped branch); `REFL_REPLACE_FORWARD 0` compiles it out byte for byte.
- What correct looks like: the Ultra-Luxe's marble, polished counters, glass and chrome props show the room once (the real reflection),
  not the room over a painted sheen; the same things at a distance / in a rough spot / with the Reflections effect off keep the game's
  sheen. With RT Weapon the gun shows the traced room without its painted chrome under it.
- What wrong would look like: an env-mapped surface gone matte where the effect reflects weakly (by design where the material is nearly
  too rough: MaterialMaxRoughness's fade; and where SpecularOcclusion dims the reflection: lower it with this on); a thin bright or dark
  outline on shiny props for one frame while turning (the mask is a frame old, half resolution, not reprojected); the sheen flickering
  (the log line then alternates between `on` and `not used now`).
- Energy: the composite's weight already follows the material's Fresnel and roughness and is a mix (never more than 1); only ReflectAdd
  (default 0) adds on purpose. Nothing was changed there.
- What a reflection shows (RT hits out of view): the sun through a shadow ray (`[Shaders.Reflections.RT] SunShadows`, default on), or
  indoors / at night the lamp that lights the hit most through one (`HitLamps`, reflecting surfaces under roughness 0.25); the other 11
  lamps and the flashlight unshadowed, the ambient from the light grid at the hit. `Rt/ReflectionsRTBody.glsl` ShadeHit.

## Idle targets (2026-11-12)
With `[Main.Main.Performance] TargetReleaseSeconds` above 0 (default 20) the texture manager keeps 1 x 1 stand-ins in place of this effect's render targets while it is off (not made at start-up when it starts off; released that many seconds after it is switched off; made again, zero filled, before the frame draws when it is switched on). Which targets and why that is safe here: the four screen buffers. The water and env-map game shaders declare TESR_ReflectionsBuffer and read it only behind TESR_RTWaterData.x / TESR_ReflReplace.x (0 while off); their cached pointer is cleared at the swap. Not the cube maps, the RT images or the material buffer (made by hand). A new reader or writer of these targets outside the effect's own switch, or a copy of their pointers kept past a frame, breaks this: then answer false in `TargetParkable` (the header). See `core/TextureManager.h`.
