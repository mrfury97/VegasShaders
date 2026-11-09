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
