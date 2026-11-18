# Bumped normals for the screen effects: study and plan (2026-11-18)

Branch `bumped-normals-study` (off `vs-1.1.0-plus-fork`), worktree `F:\Claude Brain\VS-bump`. A study and a small prototype behind a test
switch that is off by default; not a ship, nothing installed. Background: `docs/deferred-vs-forward.md`, section "Surface normals".
Paths are under `src\` unless noted.

## In short
- The deferred sun and lamps already light with the normal-mapped ("bumped") normal. Every screen effect (AO, bounce light, shadows' normal
  offsets, SSR, wetness, snow) and the RT AO / GI input see only the smooth vertex normal. So does the templates' own sky-ambient term, so
  shaded areas show no bump detail anywhere.
- Worth giving the bumped normal: **reflections** (SSR, RT reflections, wet reflections, WetWorld's rain sheen) first; the **bounce light / RT
  GI receiver** term later. Not worth it: AO, contact / sun / lamp shadow offsets, snow placement, RT ray origins.
- Recommended design (option B): one view-space copy of the normals buffer with the bumped normal swapped in, made once a frame, which each
  effect opts into. It already exists (`core/FdIndirect.h` ShadingNormals, used by the off-by-default DeferredReflections / DeferredRRGuides).
- Prototype: `[Main.Main.Quality] DeferredBumpedNormals` (live, test only, off): SSR / RT reflections and Wetness Effects' wet reflections
  take the bumped normal. Builds; the changed shader compiles offline; not run in game.

## Proven (from the code)

### Where the normals are
- G-buffer: COLOR3 smooth normal, COLOR2 g / a bumped normal (GBufferV2, forced on by DeferredRenderer 1 / 2), camera-relative world axes,
  octahedral, fp16 (`hlsl/NewVegas/Shaders/Includes/CapturedNormals.hlsl:1-124`).
- `TESR_NormalsBuffer` (view space, n x 0.5 + 0.5, fp16): rebuilt from depth (`Effects/Normals.fx.hlsl`), then the smooth normal merged in where
  it agrees with the depth (`core/CapturedNormals.h:645-683`, `core/ShaderManager.cpp:2308-2310`). Every screen effect reads this.
- The shading-normal copy (`core/FdIndirect.h` ShadingNormals, shader `core/FdIndirectShaders.h:23-37`): per pixel, the bumped normal turned to
  view space where the normals buffer shows the captured surface (written, distance match, within the merge's angle of the smooth normal,
  a bumped normal stored, facing the camera); everywhere else the normals buffer as it is. So leaves, grass, far land without a stored
  bumped normal, the game's own shaders, water and pixels that failed the merge keep exactly what they read today. Same size and format as
  the normals buffer (internal resolution), made at most once a frame, GPU timer "FD shading normals" (F10 profile).
- The templates' sky-ambient term uses the smooth / geometric normal (`Shaders/Includes/Object.hlsl:484-495`, `ObjectTemplate.hlsl:2685,2697`),
  and the game's ambient is flat. So in shade nothing shows the normal map.
- RT AO / GI rays start from `NVR_RTAOInput` (G32R32F: x the distance, y the 12-bit octahedral world normal), written by `Compute/RtAOInput.frag`
  from `TESR_NormalsBuffer` (`core/RtAmbient.h:13`, `effects/AmbientOcclusion/AmbientOcclusion.cpp:778-800`). No spare channel. The path tracer
  already gets `TESR_MaterialBuffer` across the bridge as a shared image (`core/RtPathTrace.cpp:913,1075-1077`) and lights its first hit with
  the bumped normal while the smooth one places the rays (PTBumpNormals, defaults :480).
- Wetness: `GetWetness(position, vertexNormal, normal)` already takes two normals; without `WETNESS_HEIGHT` (the screen pass has none) the second
  only lands in `w.normal`, so wetness placement follows the first (`Shaders/Includes/Wetness.hlsl:432-456`). WetWorld uses its one normal for
  both puddle placement (`dot(normal, up)`, `Effects/WetWorld.fx.hlsl:802-826`) and the surface it shades (`:927,1014`).

### Defaults that decide what a player sees
AO on, AmbientLight on, WetWorld on; Reflections, Wetness Effects, Snow, Specular, BounceLight, RT AO off (`resource/VegasShaders.defaults.toml`,
each effect's Status). DeferredRenderer 2 (on).

## Per effect: help or hurt
| Effect | Bumped normal? | Why |
|---|---|---|
| SSR / RT reflections (Reflections) | yes | the reflection direction is where bump detail shows most (tiles, cobbles, dented metal); every engine's SSR reads the G-buffer normal (believed) |
| Wet reflections (Wetness Effects) | yes, for the reflection only | placement and the ray start stay smooth (prototype does this) |
| WetWorld rain sheen | yes, outside puddles | puddles already flatten toward up (`WetWorld.fx.hlsl:1014`), matching real puddles; placement must stay smooth or puddle edges speckle |
| AmbientLight bounce, RT GI | receiver cosine only | detail in shade; the rays / samples must start from the smooth normal or light leaks through surfaces (the path tracer's own rule) |
| AO (SSAO / GTAO) | no | the horizon search on a normal map self-occludes and shimmers; GTAO-style AO uses the depth-derived or smooth normal (believed); the code's own experience: AO's sky term has a switch to take the depth normal instead, added after wrong road normals put its test start under the ground (`Effects/AmbientOcclusion.fx.hlsl:545-555`, DepthNormals, off) |
| Sun / lamp shadow normal offsets, contact shadows | no | the offset must follow the geometry; bumps give speckled self-shadowing |
| Snow (placement) | no | snow covers bumps; placement on a normal map speckles |
| Specular (screen sun highlight) | no need | off by default, and the deferred sun already lights highlights with the bumped normal |

What others do (believed, from general knowledge, not checked against sources this session): deferred engines (Unreal, Frostbite) feed the
G-buffer's normal-mapped normal to SSR and to the GI / irradiance lookup, and use depth-derived or geometric normals for AO horizon searches
and shadow biasing; wet-surface models (Lagarde, "Water drop" 2013) keep the normal map on damp surfaces and flatten it toward the geometric
normal as puddles fill; "bent normals" carry AO's unoccluded direction into the ambient lookup, a separate feature from bump detail.

## Design options
| Option | What | Frame cost (not measured) | Shaders recompiled | Verdict |
|---|---|---|---|---|
| A | merge the bumped normal into `TESR_NormalsBuffer` at the existing merge | about +8 bytes / pixel in a pass that already runs: ~0.03 ms | none (the merge shader is a C++ string) | rejected: every reader at once, AO / shadows / snow get worse |
| **B** | one shared copy (ShadingNormals), each effect opts in | one full-screen pass, ~36 bytes / pixel: about 60 MB at 1440p x 67 % render scale, 0.1-0.25 ms (GTX 1070 to a current RTX card); only on frames where an opted-in effect draws; the readers pay nothing extra (a texture swap) or one extra read | only effects that need two normals (wet reflections now: one .fx; WetWorld later: .fx + .frag / .spv); no game shader | **recommended** |
| C | each effect decodes COLOR2 / COLOR3 itself | no extra pass, but every reader repeats ~3 reads + the agreement test | every reader, D3D9 and Vulkan (.spv rebuilds) | rejected: most code, parity risk, cost grows with readers |
| D | the templates' sky term takes the bumped normal | about 0 | every object / land / skin template: one full recompile (minutes on the owner's PC) | separate question: the biggest change in shade, player-visible |

Leaves, far land, grass and pixels that fail the merge: under B they keep today's normal (above), so nothing gets worse there. Leaves have
no bumped normal to give; far land has one (its LOD normal map) and gets it.

## RT GI and the helper
- Recommended: no helper change. The bump belongs in the receiver term of the in-process GI resolve / composite (AmbientLight's passes), which
  can read the shading-normal copy like any effect. The rays keep starting from the smooth normal.
- If the rays themselves ever had to use it: `NVR_RTAOInput` has no spare channel, so a second shared image (RtCore::AddImage, as the path
  tracer's material image) plus a `Compute/Rt/RtAOTrace.comp` change and `.spv` rebuild. The bridge already moves `TESR_MaterialBuffer` to the
  helper, so no DXVK-fork change is believed needed (the fork's own code was not read).

## The prototype (this branch)
- `core/FdIndirect.h`: `BumpedNormalsOn()` = renderer on + material buffer this frame + `[Main.Main.Quality] DeferredBumpedNormals`.
- `effects/Reflections/Reflections.cpp` FdReflNormals: the shading normals also under the new key, without DeferredReflections' material copy.
  This swaps the whole normals texture for every pass of the effect (D3D9, Vulkan, the RT RayGen and denoiser), so the ray start, the trace and
  the temporal pass take the bumped normal too, exactly as DeferredReflections already does. A smooth ray start would need a second normal in
  every one of those passes (D3D9, Vulkan, RT): left for later.
- `hlsl/NewVegas/Effects/WetnessReflections.fx.hlsl`: a second normal `NVR_WetShadingNormals` (s4). The smooth one still decides what is wet,
  where the ray starts and the horizon test. While on (`TESR_WetnessReflectionBump.x` 1), the reflection takes the film's own normal as the
  forward shaders do (`wet.normal`: the bumped normal flattened toward the smooth one under standing water and streaks, plus ripples), so a
  puddle does not mirror the ground's bumps; the sky taken out uses that same normal; a ray tilted into the surface is mirrored about the
  smooth plane. Off (0): the old path, untouched. `effects/WetnessEffects/WetnessReflections.cpp` BindShadingNormals sets s4 and the constant
  before each Render (`core/EffectChain.cpp`), the stage is cleared after it.
- Defaults: the key in `resource/defaults/Core.toml` and the shipped `resource/VegasShaders.defaults.toml`, false.
- Checks: DLL build (Release Win32, v142) into `F:\VS-build-bump`, exit 0, no errors (without the release marker: a development build, not
  for players). Offline compile of the changed effect: see "Checks" below.

### What the owner would see with the switch on
Only with Reflections or Wetness Effects on (both off by default): floors, cobbles, tiles, metal and wet roads reflect with their bumps and
grooves breaking the reflection up instead of a smooth mirror. Nothing else changes. Off: nothing changes at all.

## Checks
- DLL: built, see above.
- WetnessReflections.fx.hlsl, both runtime compile routes: `build\effect-test\effect_compile.exe` (D3DX) PASS with FORWARD_SHADOWS 0 and 1;
  `fxc /T fx_2_0 /I <Effects>` (d3dcompiler_47, the default route) succeeded, only the usual X4717 "effects deprecated" warning.
- No game shader, `.frag` or `.spv` changed, so nothing else recompiles; in game this one effect recompiles once (seconds).
- All three re-run after the review fixes: DLL exit 0 (`F:\VS-build-bump\build2.log`), D3DX PASS both variants, fxc succeeded.
- Defaults: `tools/defaults_tool.py` shows the same 58-line fragments-vs-shipped mismatch as the base branch (upstream's own), line for
  line; the new key adds none.

## Review (lead-architect, 2026-11-18): approve with fixes
- Fixed: puddles reflected the ground's bumps (now `wet.normal`); the sky taken out did not follow the bumped ray (now the same normal); rays
  tilted into the surface (now mirrored); docs (this file, the Reflections README, FdIndirect's comments).
- Proven by reading: key off gives the old output (s4 = the same texture, point / clamp, the constant 0 keeps the old code path); the
  shading-normal pass restores every state Render relies on; clearing the stage after Render is the Reflections pattern.
- Taken into the plan: Wetness Effects is off by default, so WetWorld (on by default) is the path players see: step 2 first. Distance
  sparkle wants a fade to the smooth normal or a Toksvig-style roughness widening. With DLSS Ray Reconstruction on and DeferredRRGuides off,
  the guides keep the smooth normal while the reflections are bumped, which may smear the detail: tie the guides to the same switch.

## Believed (not proven)
- Reflection gain is the largest visible win; distant normal-mapped surfaces may sparkle in SSR (normal-map aliasing: no mip filtering of the
  normal); TAA softens it. A fade from bumped to smooth with distance or roughness would be the fix if it shows.
- The 0.1-0.25 ms estimate (bandwidth arithmetic only).
- No DXVK-fork change for any phase.

## Not run
- Anything in game. No frame-time measurement.

## Plan and effort (working sessions)
1. In-game look at the prototype (owner, with Reflections on): 0.5 session including an install and the before / after shots.
2. WetWorld's rain sheen on the bumped normal, D3D9 + Vulkan, with a distance fade, and the DLSS RR guides on the same switch: 1 session.
   Risk: D3D9 / Vulkan mismatch, puddle edges. The one players would see by default once shipped.
3. Bounce light / RT GI receiver term: 1-2 sessions with look tuning. Risk: brightness shifts in shade, noise in the GI denoiser.
4. Option D (sky term in the templates): 1 session plus a full shader recompile. Risk: the look in shade changes everywhere.
Fold DeferredReflections and DeferredBumpedNormals into one switch before anything ships (product rule: one switch).

## Open questions for the owner
1. Look at the prototype in game? (It needs an install into the MO2 mod; Main brain asked for none this round.)
2. Go on with WetWorld's rain sheen (step 2)? WetWorld is on by default, so shipping it on later is a visible change.
3. Want bump detail in shade (step 3, and option D with its one full recompile)?

## Where things are
- Branch `bumped-normals-study`, worktree `F:\Claude Brain\VS-bump` (needs `packages\` copied, as every worktree). Not pushed.
- Build `F:\VS-build-bump\VegasShaders.dll`, log `F:\VS-build-bump\build.log`.
