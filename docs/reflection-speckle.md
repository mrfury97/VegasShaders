# Speckled reflections on bumpy wet ground

Owner's report 2026-11-21 (save `reflection01`, Goodsprings, sunny after rain, install-test18): the reflection on the damp dirt breaks into
white speckles at mid distance, most toward the sun. Owner asked for an unreasonably ambitious fix.

## Proven (read in the code, `src/hlsl/NewVegas/Compute/`, test-p1a-bump aca9929)
- Damp ground: full bump (`DampBumpLean 0`, test16), blur roughness clamped to at most `DampMaxRoughness 0.3` (7231832, test18).
- `BumpKeep` keeps the whole bump until a pixel covers 0.5 world units (about 12 m at 1440p): the speckled band is inside that.
- `NormalVarianceRoughness` measures the SMOOTH normal only (by design since the bump fix), so the bump adds no roughness anywhere.
- Trace (`ReflectionsTrace.frag`) is half resolution: one ray per 2x2 block, its direction from ONE pixel's bumped normal (GGX-spread);
  a ray that leaves below the bumped surface falls back to the plain mirror; nothing checks it against the real (smooth) surface.
- Composite (`ReflectionsComposite.frag:154-157`): the Fresnel share uses each full-res pixel's own bumped normal.
- Temporal (`ReflectionsTemporal.frag`): history found by the SURFACE's motion, clamped to the current 3x3 min/max; mirrors keep none.

## Believed (not run): why it speckles
1. A ray bent by a bump at a low view angle swings between the bright sky and the dark ground: neighbouring blocks get opposite answers
   (the sky is many times brighter than the dirt), and the 0.3 blur (a few half-res pixels) cannot average a contrast that large.
2. Each full-res pixel takes a reflection traced for another pixel's normal (2x2 block, upsampled) but its own Fresnel: at a low angle
   Fresnel swings from ~0.05 to ~0.6 across a bump, so the same reflection is drawn at very different strengths pixel to pixel.
3. Rays bent below the real ground surface are counted (they should be shadowed by the ground itself: horizon occlusion).
4. The history cannot clean it: it follows the ground, not the reflected thing, and the min/max clamp keeps the noise.
5. Possibly not the reflection at all: the sun's own highlight on the wet film (DeferredSun) also glints on every bump toward the sun.
   The owner's toggle below tells 5 from 1-4.

## Ruled out
- Sub-pixel aliasing as the main cause: at 5-10 m a dirt bump spans several pixels (footprint about 0.4-0.7 units), so it is drawn, not aliased.

## How Community Shaders (Skyrim) does wet ground (read 2026-11-21, github.com/community-shaders/skyrim-community-shaders 2b05afa)
GPL-3.0 with a modding exception (`COPYING.txt`, `EXCEPTIONS.md`); ours is GPL-3.0 too, so adapting is allowed with credit; we still write
our own. Clone: `F:\VS-scratch\ext\cs` (sparse) and `F:\VS-scratch\ext\cs-files`.
- Wet ground has NO screen-space trace. `Lighting.hlsl` adds `wetnessReflectance x GetDynamicCubemapSpecularIrradiance(wetNormal, V,
  wetRoughness)`: one lookup in a cube made each frame from the screen (`Dynamic Cubemaps`: screen pixels splatted into a cube round the
  camera, holes filled from coarser mips, then pre-filtered by roughness), mip = roughness x 8, its brightness normalised to the ambient
  light's and scaled by Skylighting's sky visibility (smooth spherical harmonics).
- Damp ground keeps its bumped normal but is rough: roughness = 1 - wetness gloss (`WetnessEffects.hlsli` GetSurfaceWetnessRoughness).
  Puddles get a flat ripple normal and roughness down to 0.05. Fresnel: split-sum EnvBRDF, F0 0.02.
- Skyrim's own SSR (`ISReflectionsRayTracing.hlsl`) is the WATER's, with one fixed normal; it never runs on ground.
- Why theirs cannot speckle: the cube is pre-blurred, so pixels with different bump normals read neighbouring parts of one smooth
  picture (no sky-or-ground flips), and its brightness is tied to the ambient (no bright sky specks over dark dirt). The price: no exact
  reflections on damp ground (the cube is seen from the camera, not the surface), a soft sheen rather than a mirror.
- For us: the same split costs little. Damp ground (film, no standing water) hands over to our live cube / light grid at its own
  roughness; only standing water (flat in the G-buffer) is traced. Catch: the owner's test10 look ("dont seem to see any reflections")
  was damp ground handed to the grid, so the cube's sheen must be strong enough to read as wet.

## Plan (each tier stands alone)
**Tier 1, about 1 session, deferred Vulkan path only:**
- Fresnel from the smooth normal (or the 2x2 average), the bump kept in the ray direction only.
- Horizon fade: a ray below the smooth surface fades to the surface's own ambient, not sky or ground.
- Bump roughness: the bumped normal's change across the 2x2 block adds to the BLUR roughness only (not to the trace-to-grid hand-over),
  so bumpy damp ground blurs instead of speckling and still bends.
- Clamp a block's sky brightness to its neighbours' (firefly clamp) where the bump is busy.

**Tier 2, 2-3 sessions:**
- The G-buffer write measures its own normal map's variance (ddx/ddy of the bumped normal, or Toksvig from the mip-filtered length)
  and stores it in the roughness: one fix for the sun, the lamps and the reflections (sun glints too).
- Full-resolution trace on wet pixels only (the band where it shows).
- History by the reflected point's motion (hit distance: "virtual" motion), so the rough reflection can average over 8-16 frames.

**Tier 3, the ambitious one, 6-10 sessions, needs NVIDIA (RTX) and the 64-bit helper:**
- Stochastic reflections done properly: several GGX rays per pixel shared with neighbours (ratio estimator, Stachowiak 2015), and a
  real specular denoiser (NVIDIA NRD ReBLUR, open source; check its licence) or DLSS Ray Reconstruction through the DLSS helper, which
  is built for exactly this: noisy ray-traced reflections on rough, bumpy surfaces, guided by normals, roughness and hit distance.
- Sun glitter as a glint model (a counted number of sparkles per pixel footprint, stable and filtered, as in Deliot & Belcour 2023),
  so the wet-road sparkle toward the sun is a deliberate look, not noise.
- Risk: the 64-bit helper and DXVK fork already carry RT; adding NRD or RR is a large new dependency; AMD cards would keep tier 2.

## Open
- Owner's toggle at `reflection01`: Reflections off -> do the speckles stay (then 5)? DebugView 12 screenshot (red = bend, green =
  roughness).
- Owner's word for tier 3 (new dependency, player-visible, frame cost).
