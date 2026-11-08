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
