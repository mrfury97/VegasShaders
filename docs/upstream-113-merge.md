# Upstream 1.1.1-1.1.3 merged into this fork (2026-11-22)

Branch `upstream-rt-merge` (worktree `F:\Claude Brain\VS-branches\upstream-rt-merge`), merge commit 68cbb9c of our main f4e43e3 and
upstream/main 5559923 (merge base d3477de = 1.1.0). Owner's word (relayed by Main brain, 2026-11-22): "yea tell it to start that" for
the recommended option of `docs/upstream-reflections-rt-compare.md` (branch `upstream-refl-rt-compare`): take upstream's RT as is, keep our
deferred wetness, unified reflections and speckle fixes, hand-merge where they meet. Not pushed, not in main.

## What came in from upstream unchanged
- Ray tracing: path tracer 3-pass split, ReSTIR GI, pure direct-lit GI, RT AO / Sky / GI as separate switches, RT fog lamps, RT shadow
  denoise, RT reflections at RenderScale with their own denoiser and weapon reprojection (`ViewModelMotion.h`). RT helper protocol 9 -> 10:
  `tools/nvr-rt-helper` builds `NvrRtHelper.exe` (x64); it must replace the installed one (the plugin refuses a helper of another protocol).
- Everything else of 1.1.1-1.1.3 that did not touch our work: menu overhaul (1.1.1-test2), Vulkan `DeferredLamps` compute, RenderRect
  post-FX, matte materials, 1st-person viewmodel / ADS / AA fixes, 3D menu PBR isolation, Sandstorm and shadow ghosting fixes, Hi-Z removed.
- DXVK fork: upstream added patches 0036 (security: `nvr.debugEnv` from dxvk.conf sets only harmless names) and 0037 (HDR blitter fallback
  descriptors and metadata guard). Neither touches RT (read in the patches): no fork rebuild is needed for the merge.

## What stays ours
- Deferred wetness: G-buffer film (`core/DeferredWetness.h`), one rain clock (`core/RainState.h`), WetWorld removed (upstream still has it;
  its files, settings and menu choice "Legacy WetWorld" are left out), Wetness Effects + Puddles on by default.
- Unified reflections: `GBufferFrame`, `WetReach`, `TESR_ReflectionsGBuffer` / `TESR_ReflectionsFilm`, DebugView 12.
- Speckle fixes: `DampTrace` true, `DampBumpLean` 1, lamps on damp ground, the Fresnel cap (`DampMaxRoughness`).
- BloodFlow and BloodLens (upstream deleted both): restored with their menu rows, chain steps and project entries.
- Our own `WetnessReflections` (no stand-down when Reflections runs, no `SkylightingMode = 1` sky there, no first-person exclusion).
- The old text-menu settings stay retired (upstream keeps them, hidden).

## Hand-merged seams
- **Reflections, Vulkan** (`Reflections.cpp`, `Compute/Reflections.glsl`, `ReflectionsComposite.frag`): one wetness source per frame. Our
  G-buffer film when the frame has a whole one (`FilmWhole`; then the C++ zeroes `wetFx.x`), else upstream's forward film
  (`ComputeWetFxFilm`: rain, puddles, open sky, wet walls). Upstream's composite F0 (albedo metal F0, G-buffer dielectric F0, wet F0,
  weapon tint) with our damp-ground Fresnel cap. Upstream's composite-only bindings 28 / 29 collided with our film / smooth normals:
  the composite reads the same two textures through our bindings 30 / 31.
- **Reflections, D3D9** (`Effects/Reflections.fx.hlsl`): the `REFL_GBUFFER_ON` variant (UnifiedReflections on at startup) keeps ours;
  the other variant takes upstream's forward film.
- **Settings migrations** (`SettingManager.cpp`, version 97): upstream's steps 90-94 first, ours renumbered 90-92 -> 95-97. A file saved by
  our builds at 91 / 92 (no `[Shaders.WetWorld]` left) skips ours where it already ran them. Upstream's 91 (MetalFromMap), 92 (HelperWait +
  RT reflection flips) and 94 (VanillaBridgeDielectricMax) are parked behind one constant each (`kParkUpstream91/92/94`).
- **1.1.2 files**: files that only held our hand-port of 1.1.2 are upstream's version now (the port and upstream's own 1.1.2 duplicated
  code): HDR.toml, CapturedNormals.h (+ BloodFlow's albedo request), CompileProgress.h (upstream's: the window shows after 1.5 s),
  DeferredSkin.h, HdrOutput.h, ShaderPrecompile.h, ShadowManager.cpp, Grass.cpp, GRASS23x000TMS.pso.hlsl, Leaf.hlsl.

## Defaults: kept ours, the owner decides (one key each)
| Key | Ours (shipped here) | Upstream |
|---|---|---|
| `[Shaders.Reflections.Main] ReplaceForward` | false | true |
| `[Shaders.Reflections.RT] RideMainExchange` | false | true |
| `[Shaders.Reflections.RT] FollowRenderScale` | false | true |
| `[Main.Main.RayTracing] HelperWait` | 0 | 1 (removes a 5-8 ms queue stall, upstream says) |
| `[Shaders.PBR.Rmaos] MetalFromMap` | false | true |
| `[Shaders.PBR.Main] VanillaBridgeDielectricMax` | 0.08 | 0.04 |
| `[Main.Main.Quality] DeferredBumpedNormals` | false | true |
| `[Main.Main.Performance] NvrDxvkDeferred` (new) | false | true (Vulkan compute DeferredLamps, said to be 6-7x faster) |

Taken as upstream's: `[Shaders.AmbientOcclusion.RT] Sky` false (it is independent of RT AO now; true would start sky rays for players who
never turned RT AO on). New upstream keys on by default (fixes): `TAA.Main WeaponAA`, `TAA.FSR PreTonemapHDR`,
`ShadowsExteriors.Deferred MatchSunDisc`, `RayTracing PTDenoiseSplitSun`.

## Proven / believed / not run
(filled in after the review, install and bench below)
