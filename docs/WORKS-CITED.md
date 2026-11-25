# Vegas Shaders: works cited

Everything Vegas Shaders (VS) uses from other people, from the start of the unofficial New Vegas Reloaded work
(NVR-Unofficial-Optimized, P-packages from late September 2026) through the 2026-11-02 rename to Vegas Shaders, up to
the deferred-renderer branches of 2026-11-15. It is the source for the README credits, the in-game Help > About list
(`src/core/MenuTools.cpp`) and release notes.

Compiled 2026-11-15 from the source tree (branch `claude/fd-release-prep`, the trunk `claude/rt-review-fixes`, and the
newer branches `claude/fd-batch8` and `claude/fd-furyrtgi`), the licence files, the release notes 1.0.1 to 1.0.9, the
git history and the project's notes. Section 6 checks (2026-11-15): every DOI was resolved at doi.org and its
Crossref title compared with the cited title; every other link was loaded. Titles, authors and venues were confirmed
by web search for most entries; for some well-known papers they come from standard references and agree with the
DOI's landing title. Links that a site refused to serve to a script (Unreal Engine blog, Utah library, Extreme
Learning, Shadertoy) are marked *(link not loaded)*. Anything not confirmed is marked **(unverified)**.

How to read the markers:
- **in VS**: the code is in the shipping tree (on or off by default).
- **opt-in**: in the tree but off by default, experimental, or needing the DXVK fork / RT helper.
- **branch only**: on `fd-batch8` / `fd-furyrtgi`, not yet in a release.
- Testers and other private people are not named here. Names appear only as they already appear in public credits,
  licence files or public repositories.

---

## 1. Base project and its authors

**New Vegas Reloaded / TESReloaded** (Oblivion Reloaded lineage). Vegas Shaders is a fork of it and remains under its
licence.
- Author: Alessio Tamburini (Alenet), 2019-2022. Maintainer: Llde (github.com/llde/TESReloaded10).
- Contributors listed in `License.md`: Timeslip, scanti, ShadeMe, Ethatron, GBR, mcfurston, noonemusteverknow,
  Mathieu U, "and all others that helped fixing bugs, betatesting or improving shaders".
- Original shader authors still named in source: Boomstick and Mac (WetWorld, `WetWorld.fx.hlsl`).
- Several shaders still carry "for Oblivion Reloaded" headers (AvgLuma, BloomLegacy, Precipitations, Snow,
  ShadowsExteriors); `Sharpening.fx.hlsl` is "Contrast Adaptive Sharpening Enhanced Shader for TESReloaded" (author
  not named).
- Licence: GPL-3.0-or-later (`COPYING.txt`) plus the section 7 terms in `License.md`: binaries must say "UNOFFICIAL"
  or carry their own name, must state that the official channels can't be used for them, and may not be sold; plus the
  Modding and Linking exceptions.

**macrimmon12-tech/TESReloaded10** (github.com/macrimmon12-tech/TESReloaded10), GPL-3.0: the fork this work branched
from. Volumetric fog v2, skin and lighting fixes, the first ImGui menu. History kept (merges of its PRs #54, #57, #58,
#78, Sept 2026).

---

## 2. People and projects whose work is in Vegas Shaders

| Who | What is in VS | How it came in | Licence | Link |
|---|---|---|---|---|
| **Conor (mrfury97)** | ManyLamps and ManyLampsOnePass, terrain-shadow sweep (TerrainShadows, SunFar), terrain variation and the terrain-blending update, screen-space skin scattering (SkinScattering), FoldFog, Pip-Boy fit, BlockGameMSAA, effect precompile idea, RT sun GI phases, AmbientReplace, ExteriorBalance, Puddles, RT audit fixes; the TAA ("TAA for New Vegas Reloaded by Conor"); about 95 commits merged from his forks | His forks (Conor-sClaudeHell-NVR-SAL-Fork; PR #1 to NVR-Unofficial-Optimized; PR #1 to VegasShaders; github.com/mrfury97/VegasShaders) | GPL-3.0-or-later | github.com/mrfury97 |
| Conor (mrfury97), **branch only** | RT GI review series (GIGrassBlur, GIHitMips, GISamplePoints reservoir layout, GIScreenAmbientOut), 15 commits ported | `claude/fd-furyrtgi`, `docs/next/FURY-RTGI-PORT.md` | GPL-3.0-or-later | github.com/mrfury97/VegasShaders |
| **PixelsSquared** | Perceptual tone mapper (TonemappingMode 11, a port of Polyphony Digital's MIT sample code), eye adaptation (Exposure Mode 1), cloud PhysicalMoonlight / PhysicalSkyLight | PR #4 (physical-clouds), merged 11-04 | GPL-3.0 (contribution); MIT for the Polyphony part | github.com/PixelsSquared |
| **macintroll** | SnowAccumulation "Look 1" (default), world-space snow (`[Shaders.Snow.Main] WorldSpace`, off by default), the flake sheet `fx_weather_snow01.dds`, captured normals, lens and underwater shaders, WetWorld2 (with Boomstick), terrain-blending HeightEdge 1/2 | Made for this project and sent directly; owner confirmed 11-09 it may ship with credit | Permission with credit | (no public link) |
| **Balebandro**, Physically-Based-Reloaded | Sandstorm, HeatHaze, terrain erosion gullies (TerrainErosion), terrain blending parts, HUD raindrops port, WetnessReflections, Skin.hlsl parts, Grass size fade; ideas only (our own code): per-weather PBR scales, SmithCorrelated, CharactersKeepDepth. His PRs #5 (MO2 DXVK loader, deferred flashlight, grass and wetness ports, shadow and MSAA fixes) and #6 (skin, soft particles, flashlight under terrain blending, first-person shadow dropout) | Ports from his fork; PRs #5 and #6 merged | GPL-3.0 | github.com/Balebandro/Physically-Based-Reloaded |
| **VHeyes (mirjomi)** | MaterialPass depth pull (PR #46), SMAA fixes and tuning (#48), DitherBuster (#49), SpeedTree forward shadow, mip repair | Cherry-picked from PRs to macrimmon12-tech (`docs/upstream-cherry-picks.md`) | GPL-3.0 | github.com/mirjomi |
| **Community Shaders team** (Skyrim) | Terrain Shadows, Cloud Shadows, Grass Collision, Terrain Variation, LOD Blending, Skylighting sky-reflection mix, Wetness Effects, the Separable SSS kernel (via Conor), Terrain Blending method, Complex Parallax Materials method, the True PBR `_rmaos` layout, Grass Optimizations size fade; **branch only**: deferred-skin kernel generator and pre/post scatter (`DeferredSkin.h`, `DeferredSkinSSS.hlsl`) | Ported / adapted, checked at commit 3f9f72d (`docs/COMMUNITY-SHADERS-PORTS.md`). Logo/icons and the glints shader not used | GPL-3.0 with the same exceptions | github.com/community-shaders/skyrim-community-shaders |
| **pr0bability**, Vanilla Plus Particles | Particle size, NoLighting and SoftParticle includes (`ParticleSize.h`, `NoLighting.hlsl`, `SoftParticle.hlsl`) | Ported | GPL-3.0 | github.com/pr0bability/fnv-vanilla-plus-particles |
| **Zenteon**, ZN_FX | The method behind NeonSpill in `BounceLight.fx.hlsl` | Method adapted | GPL-3.0 | github.com/Zenteon/ZN_FX |
| **NVR Depth Bridge contributors** | The near-depth remap and the MenuGuard design in `ReShadeIntegration.cpp` | Adapted, notice kept in the file | MIT | (link not recorded) |
| **Terrain Blending - NVSE** (mod) | The terrain-blending method descends from it (`TerrainTemplate.hlsl`) | Technique | — | (link not recorded) |
| **Ascii1457**, Screen Space Shaders (S.T.A.L.K.E.R. Anomaly) | HUD raindrops technique (`HudRain.hlsl`), rewritten after Conor's version; reached VS through the PB-Reloaded port | Technique only, no licence found for the original | — | github.com/Ascii1457 (handle page) |
| **acidpointer**, psycho | IOManager layout and thread-hazard facts behind the IO worker and ArrivalSampler | Learned from, no code taken (no licence) | — | github.com/acidpointer/psycho |
| **DreamSim** | The "Dream" effect (`src/effects/Dream`) | The owner's own project, one-way port | owner's | — |
| **xNVSE** | Plugin interface; headers vendored in `src/NewVegas/nvse` | Vendored headers | (xNVSE's licence) | github.com/xNVSE/NVSE |
| **WallSoGB** | His Fallout: New Vegas IDA database / symbol map, behind the engine fixes since P75 (credited in `LampsInOnePass.h`, `PbrPop.h`, `Render.cpp`); his Fallout-Dynamic-Reflections headers consulted for struct layouts | Reference (see section 7) | — | github.com/WallSoGB |

Testers: many reports (RT, path tracing, DLAA, DXVK black screen, flicker, Discord bug reports) drove fixes in every
release. They are thanked as "testers" without names.

---

## 3. Code adapted from other projects

| Feature / file | Origin | Licence | Link |
|---|---|---|---|
| SMAA (`Effects/SMAA.fx.hlsl`, `Effects/Includes/SMAA.hlsl`, `SMAA_AreaTex.dds`, `SMAA_SearchTex.dds`) | Jorge Jimenez, Jose I. Echevarria, Tiago Sousa, Belen Masia, Fernando Navarro, Diego Gutierrez | MIT (2013 header); the 2011 header asks binaries to reproduce its copyright line (section 8) | github.com/iryoku/smaa |
| Perceptual tone mapper (`Shaders/Includes/Tonemapping.hlsl`) | Polyphony Digital, "gt7_tone_mapping.cpp" sample code, (c) 2025 Polyphony Digital Inc. | MIT | blog.selfshadow.com/publications/s2025-shading-course/ |
| ACES fitted tone mapper (`Tonemapping.hlsl`) | Stephen Hill (self_shadow), header kept | not stated in our copy (BakingLab, where it is published, is MIT) | github.com/TheRealMJP/BakingLab |
| Lottes 2016 tone curve (`Tonemapping.hlsl`) | via KelSolaar's and vtastek's gists | not stated **(unverified)** | (gist URLs in the file) |
| AgX (`Tonemapping.hlsl`) | after "Minimal AgX implementation", iolite engine blog | — | iolite-engine.com/blog_posts/minimal_agx_implementation |
| TAA history filter (`TAA.fx.hlsl`) | Catmull-Rom filter from MJP's MSAAFilter | MIT | github.com/TheRealMJP/MSAAFilter |
| TAA lineage | Oblivion Reloaded E3 (arafuse/tes-reloaded) via Conor | GPL-3.0 | github.com/arafuse/tes-reloaded |
| RT reflection denoiser, alternate core 1 (`Compute/RtReflDenoise*.glsl/.frag`) | AMD FidelityFX Reflections Denoiser, (c) 2021 AMD; includes AMD's copy of Playdead's ClipAABB, (c) 2015 Playdead | MIT (both) | github.com/GPUOpen-Effects/FidelityFX-Denoiser |
| RT shadow denoiser (`Compute/Rt/ffx/*.h`, `RtShadowDenoiseFfx.hlsl`) | AMD FidelityFX Shadows Denoiser, (c) 2021 AMD | MIT | github.com/GPUOpen-Effects/FidelityFX-Denoiser |
| FSR 2 upscaler (`Compute/FSR2/`, `Upscaler.cpp`, `TAAFSR.cpp`) | AMD FidelityFX Super Resolution 2.2.1, (c) 2022-2023 AMD | MIT | github.com/GPUOpen-Effects/FidelityFX-FSR2 |
| FSR 3.1 upscaler (`Compute/FSR31/`, `UpscalerFsr31.cpp`) | AMD FidelityFX SDK 1.1.4, (c) 2024 AMD | MIT | github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| Separable SSS (`SkinScattering.fx.hlsl`, `SkinScattering.cpp/.h`) | Jorge Jimenez and Diego Gutierrez (c) 2012, via Community Shaders and Conor | BSD-style, notice kept | github.com/iryoku/separable-sss |
| "Hash without sine" (`DustMotes.fx.hlsl`, `ObjectDust.hlsl`, `TerrainErosion.hlsl`, `TerrainMacro.hlsl`, `Wetness.hlsl`) | Dave Hoskins | MIT | shadertoy.com/view/4djSRW (not named in our source; from recall, *link not loaded*) |
| 3D value noise (`SnowAccumulation.fx.hlsl`, `VolumetricFog.fx.hlsl` fbm3, `SKYSHORIZFADE.pso.hlsl`; Vulkan ports `Compute/SnowAccumulation.frag`, `Compute/VolumetricFog.glsl`) | Our own (2026-11-15, `claude/fd-noiselicence` b51de1e24), lattice hash = Dave Hoskins' hash13 (named in the source). Replaces upstream's copy of Inigo Quilez's 2013 Shadertoy XslGRr value noise (its header said CC BY-NC-SA 3.0; the old row called it simplex) | ours; hash MIT | shadertoy.com/view/4djSRW |
| Exponential fog | after Inigo Quilez's fog article | technique | iquilezles.org/articles/fog/ |
| ReShade game values (`ReShadeIntegration.cpp`) | Same uniform names as FNV ReShade Helper (doodlum, WallSoGB); written from scratch, that repo has no licence | — | github.com/doodlum/fnv-reshade-helper |
| DXVK `d3d9_interfaces.h` subset (`ReShadeIntegration.cpp`) | DXVK, Philip Rebohle (doitsujin) | zlib | github.com/doitsujin/dxvk |
| RenderDoc in-app API subset (`GpuDebug.h`) | `renderdoc_app.h` 1.1.2, Baldur Karlsson | MIT | github.com/baldurk/renderdoc |
| zlib inflate (`LandHeights.cpp`) | Our own, written from RFC 1950/1951 | ours | — |

---

## 4. Libraries and SDKs

| Name | Version | Licence | Where |
|---|---|---|---|
| Dear ImGui (Omar Cornut), docking branch | 1.92.9 WIP (submodule commit 5a76f2adf) | MIT | `lib/imgui` |
| toml11 (Toru Niina) | 4.x master snapshot, commit d47fe788 (exact release not determined) | MIT | `lib/toml11` |
| Microsoft Detours | 4.0.1 | MIT | `lib/Detours` |
| ReShade add-on API headers (Patrick Mours / crosire) | API 20, commit 7bf9de8b | BSD-3-Clause | `lib/reshade` |
| NVIDIA NVAPI | release not recorded | NVIDIA SDK licence (inherited from upstream) | `lib/Nvidia` |
| RAD Game Tools Bink SDK | version not recorded | proprietary (inherited from upstream) | `lib/Bink` |
| Microsoft DirectX SDK D3DX9 (NuGet Microsoft.DXSDK.D3DX) | 9.29.952.8 | Microsoft DirectX SDK licence terms (not re-read) | `NewVegasReloaded/packages.config` |
| AMD FidelityFX FSR 2 | 2.2.1 | MIT | `src/hlsl/NewVegas/Compute/FSR2/` |
| AMD FidelityFX SDK (FSR 3.1 upscaler) | 1.1.4 | MIT | `src/hlsl/NewVegas/Compute/FSR31/` |
| AMD FidelityFX Denoiser (Shadows, Reflections) | 1.x (2021) | MIT | `Compute/Rt/ffx/`, `Compute/ffx/` |
| NVIDIA DLSS SDK (`nvsdk_ngx_s.lib`, NGX helper headers, `nvngx_dlss.dll`, `nvngx_dlssd.dll`) | tag v310.9.16 | NVIDIA RTX SDKs LICENSE | Optional DLSS-helper zip only, `tools/nvr-dlss-helper` (see section 8 for the required credit) |
| DXVK (Philip Rebohle / doitsujin), the project's fork | v3.0.1 plus our patches 0001-0035 | zlib | `tools/nvr-dxvk`; optional `d3d9.dll` ships with LICENSE-DXVK |
| Vulkan SDK (LunarG / Khronos) | 1.3.296.0 | Khronos/LunarG SDK terms (headers Apache-2.0; not re-read) | `tools/nvr-rt-helper`, `tools/nvr-dlss-helper` |
| glslang (Khronos) | build tool only, not shipped | BSD-style (not re-read) | `tools/nvr-sm6`, `tools/nvr-rt-host`, `tools/nvr-dxvk` |
| numpy, Pillow; Microsoft texconv (DirectXTex) | build tools only, not shipped | their own open licences (not re-read) | `tools/gen_textures.py`, `texgen_dds.py` |

Our own code under its own licence: `tools/nvr-rt-helper` and `tools/nvr-dlss-helper` are MIT,
"Copyright (c) 2026 adeesababa and the New Vegas Reloaded contributors".

---

## 5. Assets and data

| Asset | Origin | Status |
|---|---|---|
| Most textures in `resource/Textures` (noise, blue noise, LUTs, former NVR art) | Ours, generated by `tools/gen_textures.py` (blue noise by Ulichney's void-and-cluster) | ours |
| `SMAA_AreaTex.dds`, `SMAA_SearchTex.dds` | SMAA project | MIT |
| `Precipitations/fx_weather_snow01.dds` | macintroll, made for VS | may ship with credit: the owner settled the origin on 2026-11-09 ("created and provided by macintroll to be used in this project"). `resource/Textures/README.md` still carries an older "confirm before release" sentence; drop it |
| `Effects/hud_raindrops.dds` | Came in with Balebandro's PR #5; likely Physically-Based-Reloaded or Screen Space Shaders | **origin unconfirmed** |
| GunFX meshes (`GunFX/assets/Meshes/GunFX`) | Vanilla `effects\fxlightsmoke.nif` (Bethesda) rebuilt with other values; GunFX itself is the owner's plugin | derived from a game asset, see section 8 |
| RT test kit `vs_mirror01.nif` | Patched vanilla `nv_ul_wallscreen01.nif` (Bethesda); other kit meshes made by our scripts | derived from a game asset |
| `rmaos` maps (6506, user-side) | Ours, `gen_rmaos.py` | ours, not shipped |
| Cloud noise volumes | Ours, `tools/cloud_noise.cpp` | ours |

---

## 6. Research papers, talks and articles

Only work that code in the tree implements is listed. Ideas from plan-only research reports (CGNS, ReSTIR PT,
Conditional ReSTIR, neural radiance caching, MegaLights and others) are not cited: none is merged.

### Tone mapping and exposure
1. K. Yasutomi, K. Suzuki, H. Uchimura (Polyphony Digital), "Driving Toward Reality: Physically Based Tone Mapping and
   Perceptual Fidelity in Gran Turismo 7", SIGGRAPH 2025 course *Physically Based Shading in Theory and Practice*.
   https://blog.selfshadow.com/publications/s2025-shading-course/ . Used by: Perceptual tone mapper, eye adaptation
   (opt-in).
2. M. Safdar, G. Cui, Y. J. Kim, M. R. Luo, "Perceptually uniform color space for image signals including high dynamic
   range and wide gamut", Optics Express 25(13), 2017. https://doi.org/10.1364/OE.25.015131 . Used by: Jzazbz in the GT7
   path.
3. M. Krystek, "An algorithm to calculate correlated colour temperature", Color Research & Application 10(1), 1985.
   https://doi.org/10.1002/col.5080100109 . Used by: Planckian locus in GT7 adaptation.
4. K. Narkowicz, "ACES Filmic Tone Mapping Curve", blog, 2016.
   https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/ . Used by: ACES (Narkowicz) mode.
5. T. Lottes, "Advanced Techniques and Optimization of HDR Color Pipelines", GDC 2016.
   https://gpuopen.com/download/GdcVdrLottes.pdf . Used by: Lottes tone mapper.
6. J. Hable, "Uncharted 2: HDR Lighting", GDC 2010. https://gdcvault.com/play/1012351/Uncharted-2-HDR . Used by: Hable
   filmic mode.
7. H. Uchimura, "HDR Theory and Practice", slides, 2017 **(venue unverified)**.
   https://www.slideshare.net/slideshow/hdr-theory-and-practicce-jp/79290599 . Used by: Uchimura GT mode.
8. E. Reinhard, M. Stark, P. Shirley, J. Ferwerda, "Photographic Tone Reproduction for Digital Images", ACM TOG 21(3)
   (SIGGRAPH 2002). https://doi.org/10.1145/566654.566575 . Used by: Reinhard modes.

### Bloom, anti-aliasing, upscaling
9. J. Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 *Advances in
   Real-Time Rendering in Games*. https://advances.realtimerendering.com/s2014/index.html . Used by: Bloom; interleaved
   gradient noise (many effects: AlphaHash, FrameNoise, VolumetricLight, shadows, reflections).
10. B. Karis, "Tone mapping", Graphic Rants blog, 2013. https://graphicrants.blogspot.com/2013/12/tone-mapping.html .
    Used by: the "Karis average" in the bloom downsample.
11. Bloom also follows two articles: "Physically Based Bloom", LearnOpenGL (2022),
    https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom , and Froyok, "Custom Bloom" (UE4), 2021,
    https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/ **(titles as in the URLs, not re-checked)**.
12. M. Salvi, "An Excursion in Temporal Supersampling", GDC 2016.
    https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf . Used by: TAA.
13. L. J. F. Pedersen, "Temporal Reprojection Anti-Aliasing in INSIDE", GDC 2016.
    https://www.gdcvault.com/play/1022970/Temporal-Reprojection-Anti-Aliasing-in . Used by: TAA.
14. J. Jimenez, J. I. Echevarria, T. Sousa, D. Gutierrez, "SMAA: Enhanced Subpixel Morphological Antialiasing",
    Computer Graphics Forum 31(2) (Eurographics 2012). https://doi.org/10.1111/j.1467-8659.2012.03014.x . Used by: SMAA
    (incl. predication).
15. T. Lottes, "FXAA", NVIDIA whitepaper, 2009.
    https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf . Used by: FXAA (our own lean
    version after FXAA 3.11).

### Ambient occlusion and screen-space indirect light
16. J. Jimenez, X.-C. Wu, A. Pesce, A. Jarabo, "Practical Realtime Strategies for Accurate Indirect Occlusion",
    Activision ATVI-TR-16-01 / SIGGRAPH 2016 PBS course.
    https://research.activision.com/publications/archives/atvi-tr-16-01practical-realtime-strategies-for-accurate-indirect-occlusion .
    Used by: GTAO (AmbientOcclusion), AmbientLight. Intel's XeGTAO (MIT) was read as a reference, not copied.
17. B. Mayaux, "Horizon-Based Indirect Lighting (HBIL)", self-published paper and code, 2018.
    https://github.com/Patapom/GodComplex/tree/master/Tests/TestHBIL . Used by: AmbientLight bounce.
18. O. Therrien, Y. Levesque, G. Gilet, "Screen Space Indirect Lighting with Visibility Bitmask", The Visual Computer
    39(11), 2023. https://doi.org/10.1007/s00371-022-02703-y . Used by: ComputeGI (opt-in).
19. M. McGuire, M. Mara, D. Luebke, "Scalable Ambient Obscurance", HPG 2012.
    https://doi.org/10.2312/EGGH/HPG12/097-103 . Used by: ComputeGI HiZ option (opt-in).
20. A. Kaplanyan, "Light Propagation Volumes in CryEngine 3", SIGGRAPH 2009 Advances course.
    https://advances.realtimerendering.com/s2009/Light_Propagation_Volumes.pdf . Used by: AmbientLight GridBounce
    (opt-in, experimental).
21. N. Max, "Horizon mapping: shadows for bump-mapped surfaces", The Visual Computer 4(2), 1988.
    https://doi.org/10.1007/BF01905562 . Used by: Skylighting v2 (opt-in).
22. P.-P. Sloan, M. F. Cohen, "Interactive Horizon Mapping", Eurographics Workshop on Rendering 2000.
    https://doi.org/10.1007/978-3-7091-6303-0_25 . Used by: Skylighting v2 (opt-in).

### Ray tracing, path tracing, denoising (all opt-in: RT helper / DXVK fork)
23. B. Bitterli, C. Wyman, M. Pharr, P. Shirley, A. Lefohn, W. Jarosz, "Spatiotemporal Reservoir Resampling for
    Real-Time Ray Tracing with Dynamic Direct Lighting", ACM TOG 39(4) (SIGGRAPH 2020).
    https://doi.org/10.1145/3386569.3392481 . Used by: PT ReSTIR DI (`RtPathTrace.comp`), RT GI reuse.
24. Y. Ouyang, S. Liu, M. Kettunen, M. Pharr, J. Pantaleoni, "ReSTIR GI: Path Resampling for Real-Time Path Tracing",
    Computer Graphics Forum 40(8) (HPG 2021). https://doi.org/10.1111/cgf.14378 . Used by: PTRestirGI
    (`RtRestirGI.comp`), RT GI temporal reuse (`RtAOTrace.comp`); branch only: GISamplePoints.
25. D. Lin, M. Kettunen, B. Bitterli, J. Pantaleoni, C. Yuksel, C. Wyman, "Generalized Resampled Importance Sampling:
    Foundations of ReSTIR", ACM TOG 41(4) (SIGGRAPH 2022). https://doi.org/10.1145/3528223.3530158 . Used by: pairwise
    MIS in the PT spatial reuse.
26. E. Heitz, S. Hill, M. McGuire, "Combining Analytic Direct Illumination and Stochastic Shadows", I3D 2018.
    https://doi.org/10.1145/3190834.3190852 . Used by: RT ManyLights lamp shadows.
27. E. Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018.
    https://jcgt.org/published/0007/04/01/ . Used by: PT, RT reflections (VisibleNormals).
28. E. Turquin, "Practical multiple scattering compensation for microfacet models", ILM technical report, 2019.
    https://blog.selfshadow.com/publications/turquin/ms_comp_final.pdf . Used by: PT energy compensation,
    `ggx_energy_table.py`.
29. C. J. Fdez-Agüera, "A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting", JCGT 8(1), 2019.
    https://jcgt.org/published/0008/01/03/ . Used by: same as 28.
30. M. E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number
    Generation", Harvey Mudd College HMC-CS-2014-0905, 2014. https://www.pcg-random.org/paper.html . Used by: PT random
    numbers.
31. T. Akenine-Möller, C. Crassin, J. Boksansky, L. Belcour, A. Panteleev, O. Wright, "Improved Shader and Texture Level
    of Detail Using Ray Cones", JCGT 10(1), 2021. https://jcgt.org/published/0010/01/01/ . Used by: RT texture LOD
    (`RtTextures.glsl`).
32. Z. Majercik, J.-P. Guertin, D. Nowrouzezahrai, M. McGuire, "Dynamic Diffuse Global Illumination with Ray-Traced
    Irradiance Fields", JCGT 8(2), 2019. https://jcgt.org/published/0008/02/01/ . Used by: Chebyshev visibility in RT
    ambient.
33. T. J. V. Malley, "A Shading Method for Computer Generated Images", M.S. thesis, University of Utah, 1988.
    https://collections.lib.utah.edu/ark:/87278/s6qv5nvm *(link not loaded)*. Used by: cosine-weighted sampling in RT GI/AO.
34. NVIDIA, "SHaRC: Spatially Hashed Radiance Cache" (RTXGI SDK). https://github.com/NVIDIA-RTX/SHARC . Used by: the
    PT radiance cache follows SHaRC's use of its cache (idea only, no SHaRC code).
35. C. Schied, A. Kaplanyan, C. Wyman, et al., "Spatiotemporal Variance-Guided Filtering: Real-Time Reconstruction for
    Path-Traced Global Illumination", HPG 2017. https://doi.org/10.1145/3105762.3105770 . Used by: the shared RT
    denoiser's a-trous filter (`RtDenoiseAtrous*.frag`).
36. D. Zhdan, "ReBLUR: A Hierarchical Recurrent Denoiser", Ray Tracing Gems II, ch. 49, Apress, 2021.
    https://doi.org/10.1007/978-1-4842-7185-8_49 ; and D. Zhdan, "Fast Denoising with Self-Stabilizing Recurrent
    Blurs", GTC 2020. Used by: anti-lag and anti-firefly ideas in our denoiser.
37. NVIDIA Real-Time Denoisers (NRD): ReBLUR, ReLAX, SIGMA designs, as documented. https://github.com/NVIDIA-RTX/NRD .
    Used by: our reflection denoiser core 0 and RT denoiser lobe weights / history fix are written **like** ReLAX and
    ReBLUR from their published descriptions; SIGMA-style sun penumbra. **No NRD code is used.**
38. AMD, "FidelityFX Shadow Denoiser" documentation. https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/ .
    Used by: RT sun penumbra design; the AMD code itself is listed in section 3.
39. Halton (2,3) and Vogel / golden-angle spiral sampling are textbook methods, named in TAA/FSR jitter and PCF; no
    single paper is cited for them.

### Physically based shading
40. B. Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 PBS course.
    https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf . Used by:
    Schlick-Smith, lamp falloff (opt-in), skin sun disc.
41. B. Karis, "Physically Based Shading on Mobile", Unreal Engine blog, 2014.
    https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile *(link not loaded)*. Used by: EnvBRDFApprox (PBR,
    Specular, DLSS RR guides).
42. D. Lazarov, "Getting More Physical in Call of Duty: Black Ops II", SIGGRAPH 2013 PBS course.
    https://blog.selfshadow.com/publications/s2013-shading-course/ . Used by: same as 41.
43. S. Lagarde, C. de Rousiers, "Moving Frostbite to Physically Based Rendering 3.0", SIGGRAPH 2014 PBS course.
    https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf . Used by: rough Fresnel
    in PT.
44. E. Heitz, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs", JCGT 3(2), 2014.
    https://jcgt.org/published/0003/02/03/ . Used by: SmithCorrelated (opt-in).
45. E. Hammon Jr., "PBR Diffuse Lighting for GGX+Smith Microsurfaces", GDC 2017.
    https://www.gdcvault.com/play/1024478/PBR-Diffuse-Lighting-for-GGX . Used by: SmithCorrelated (opt-in).
46. B. Burley, "Physically-Based Shading at Disney", SIGGRAPH 2012 PBS course.
    https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf . Used by:
    Disney diffuse in `PBR.hlsl`.
47. B. Walter, S. R. Marschner, H. Li, K. E. Torrance, "Microfacet Models for Refraction through Rough Surfaces", EGSR
    2007. https://doi.org/10.2312/EGWR/EGSR07/195-206 . Used by: the Blinn-Phong to GGX bridge in `Object.hlsl`.
48. T. S. Trowbridge, K. P. Reitz, "Average irregularity representation of a rough surface for ray reflection", JOSA
    65(5), 1975. https://doi.org/10.1364/JOSA.65.000531 . Used by: GGX throughout.
49. C. Schlick, "An Inexpensive BRDF Model for Physically-based Rendering", Computer Graphics Forum 13(3), 1994.
    https://doi.org/10.1111/1467-8659.1330233 . Used by: Fresnel throughout.
50. Y. Tokuyoshi, A. S. Kaplanyan, "Improved Geometric Specular Antialiasing", I3D 2019.
    https://doi.org/10.1145/3306131.3317026 ; and "Stable Geometric Specular Antialiasing with Projected-Space NDF
    Filtering", JCGT 10(2), 2021. https://jcgt.org/published/0010/02/02/ . Used by: specular AA in `PBR.hlsl`.
51. A. Conty Estevez, C. Kulla, "Production Friendly Microfacet Sheen BRDF", Sony Imageworks / SIGGRAPH 2017 PBS course.
    https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf . Used by:
    PT cloth sheen ("Charlie").
52. D. Neubelt, M. Pettineo, "Crafting a Next-Gen Material Pipeline for The Order: 1886", SIGGRAPH 2013 PBS course.
    https://blog.selfshadow.com/publications/s2013-shading-course/rad/s2013_pbs_rad_notes.pdf . Used by: cloth
    visibility term in PT. (Our skin comment also credits it for spherical-Gaussian skin diffusion; that attribution is
    **unverified**, see 59.)
53. Naughty Dog micro-shadowing (Uncharted 4), SIGGRAPH 2016 **(exact talk not recorded in our files; unverified)**.
    Used by: PBR micro-shadowing (`Object.hlsl`).
54. C. Schüler, "Followup: Normal Mapping Without Precomputed Tangents", blog, 2013.
    http://www.thetenthplanet.de/archives/1180 . Used by: captured normals.
55. R. Ramamoorthi, P. Hanrahan, "An Efficient Representation for Irradiance Environment Maps", SIGGRAPH 2001.
    https://doi.org/10.1145/383259.383317 . Used by: SH sky irradiance (Sky, SkyAmbient).
56. lisyarus, "A better point light attenuation function", blog, 2022.
    https://lisyarus.github.io/blog/posts/point-light-attenuation.html . Used by: point light falloff (Pointlights,
    PointShadows, SnowAccumulation).

### Skin
57. J. Jimenez, K. Zsolnai, A. Jarabo, et al., "Separable Subsurface Scattering", Computer Graphics Forum 34(6), 2015.
    https://doi.org/10.1111/cgf.12529 . Used by: SkinScattering (code in section 3).
58. E. d'Eon, D. Luebke, "Advanced Techniques for Realistic Real-Time Skin Rendering", GPU Gems 3, ch. 14, 2007.
    https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-14-advanced-techniques-realistic-real-time-skin .
    Used by: the skin diffusion profile (SkinScattering kernel, forward skin widths) and the dual-lobe specular origin.
59. M. Pettineo, "SG Series Part 3: Diffuse Lighting From an SG Light Source", blog, 2016 (with S. Hill's fitted SG
    irradiance). https://therealmjp.github.io/posts/sg-series-part-3-diffuse-lighting-from-an-sg-light-source/ . Used
    by: `SkinSGIrradiance` in forward skin.
60. E. Penner, G. Borshukov, "Pre-Integrated Skin Shading", GPU Pro 2, 2011.
    https://www.gameenginegems.com/gemsdb/article.php?id=1103 . Used by: forward skin lobe widths.
61. C. Barré-Brisebois, M. Bouchard, "Approximating Translucency for a Fast, Cheap and Convincing Subsurface-Scattering
    Look", GDC 2011. https://www.gdcvault.com/play/1014538/Approximating-Translucency-for-a-Fast . Used by: skin
    transmission.
62. J. Jimenez, J. von der Pahlen, "Next-Generation Character Rendering", GDC 2013 **(no stable link found;
    unverified)**. Used by: dual-lobe skin specular (origin: d'Eon & Luebke, 58). The deferred skin's sun
    transmission term (branch only) is named "Jimenez's" in code; its exact source is not recorded.
63. P. H. Christensen, B. Burley, "Approximate Reflectance Profiles for Efficient Subsurface Scattering", Pixar Technical
    Memo 15-04, 2015. https://graphics.pixar.com/library/ApproxBSSRDF/ . Used by: deferred skin SSS v2 kernel
    (**branch only**, `fd-batch8`, test setting).

### Shadows
64. R. Fernando, "Percentage-Closer Soft Shadows", SIGGRAPH 2005 Sketches. https://doi.org/10.1145/1187112.1187153 .
    Used by: ContactHardening (opt-in), RT sun penumbra.
65. B. Yang, Z. Dong, J. Feng, H.-P. Seidel, J. Kautz, "Variance Soft Shadow Mapping", Computer Graphics Forum 29(7)
    (Pacific Graphics 2010). https://doi.org/10.1111/j.1467-8659.2010.01800.x . Used by: ContactHardening (opt-in).
66. Z. H. Cigolle, S. Donow, D. Evangelakos, M. Mara, M. McGuire, Q. Meyer, "A Survey of Efficient Representations for
    Independent Unit Vectors", JCGT 3(2), 2014. https://jcgt.org/published/0003/02/01/ . Used by: octahedral encoding
    (shadow atlas, normal packing).
67. M. Pettineo, "A Sampling of Shadow Techniques", blog, 2013. https://therealmjp.github.io/posts/shadow-maps/ . Used
    by: exterior shadows.
68. D. Rákos, "Efficient Gaussian blur with linear sampling", RasterGrid blog, 2010.
    https://www.rastergrid.com/blog/2010/09/efficient-gaussian-blur-with-linear-sampling/ . Used by: shadow map blur.
69. Bend Studio screen-space shadows (Days Gone) **(exact source not recorded; unverified)**. Used by: lamp contact
    shadows written "after" it (`PointShadows.fx.hlsl`).

### Sky, atmosphere, fog, clouds
70. E. Bruneton, F. Neyret, "Precomputed Atmospheric Scattering", Computer Graphics Forum 27(4) (EGSR 2008).
    https://doi.org/10.1111/j.1467-8659.2008.01245.x . Used by: physical sky.
71. S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", Computer Graphics Forum 39(4)
    (EGSR 2020). https://doi.org/10.1111/cgf.14050 . Used by: physical sky (single scattering on; multiple scattering
    opt-in).
72. C. Schüler, "An Approximation to the Chapman Grazing-Incidence Function for Atmospheric Scattering", GPU Pro 3,
    2012. https://doi.org/10.1201/b11642-9 . Used by: `Atmosphere.hlsl`.
73. B. Wronski, "Volumetric Fog: Unified compute shader based solution to atmospheric scattering", SIGGRAPH 2014
    Advances course.
    https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf . Used by:
    Sandstorm (opt-in).
74. S. Hillaire, "Physically Based and Unified Volumetric Rendering in Frostbite", SIGGRAPH 2015 Advances course.
    https://advances.realtimerendering.com/s2015/index.html . Used by: Sandstorm.
75. T. Porter, T. Duff, "Compositing Digital Images", SIGGRAPH 1984. https://doi.org/10.1145/800031.808606 . Used by:
    Sandstorm compositing.
76. B. Sun, R. Ramamoorthi, S. G. Narasimhan, S. K. Nayar, "A Practical Analytic Single Scattering Model for Real Time
    Rendering", ACM TOG 24(3) (SIGGRAPH 2005). https://doi.org/10.1145/1073204.1073309 . Used by: Sandstorm airlight.
77. A. Schneider, N. Vos, "The Real-Time Volumetric Cloudscapes of Horizon: Zero Dawn", SIGGRAPH 2015 Advances course.
    https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn . Used by:
    VolumetricClouds (opt-in), `cloud_noise.cpp`.
78. S. Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016 PBS course.
    https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf . Used by:
    VolumetricClouds.
79. I. Quilez, "Better fog", article. https://iquilezles.org/articles/fog/ . Used by: exponential fog.

### Reflections and wetness
80. M. McGuire, M. Mara, "Efficient GPU Screen-Space Ray Tracing", JCGT 3(4), 2014.
    https://jcgt.org/published/0003/04/04/ . Used by: WetWorld reflections.
81. T. Stachowiak, Y. Uludag, "Stochastic Screen-Space Reflections", SIGGRAPH 2015 Advances course.
    https://advances.realtimerendering.com/s2015/index.html . Used by: WetWorld; RT reflections resolve.

### Terrain, vegetation, fluids
82. T. Deliot, E. Heitz, "Procedural Stochastic Textures by Tiling and Blending", GPU Zen 2, 2019.
    https://unity.com/blog/engine-platform/procedural-stochastic-texturing-in-unity . Used by: Terrain Variation.
83. B. Wronski, "Assassin's Creed IV: Black Flag - Road to Next-Gen Graphics", GDC 2014.
    https://bartwronski.com/wp-content/uploads/2014/03/ac4_gdc.pdf . Used by: terrain, parallax. (Parallax also links
    Andrea Riccardi's ArtStation post on contact-refinement parallax.)
84. T. Sousa, "Vegetation Procedural Animation and Shading in Crysis", GPU Gems 3, ch. 16, 2007.
    https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis .
    Used by: grass, plant and cloth wind.
85. E. Wohllaib, "Procedural Grass in 'Ghost of Tsushima'", GDC 2021. https://gdcvault.com/play/1027214 . Used by: GPU
    grass (opt-in).
86. K. Jahrmann, M. Wimmer, "Responsive Real-Time Grass Rendering for General 3D Scenes", I3D 2017.
    https://doi.org/10.1145/3023368.3023380 . Used by: GPU grass culling (opt-in).
87. W. J. van der Laan, S. Green, M. Sainz, "Screen Space Fluid Rendering with Curvature Flow", I3D 2009.
    https://doi.org/10.1145/1507149.1507164 . Used by: BloodFluid (opt-in); ideas also from gwater2 (meetric, GPL-3.0),
    no code.

### Noise and sampling
88. M. Roberts, "The Unreasonable Effectiveness of Quasirandom Sequences", blog, 2018.
    https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/ *(link not loaded)*. Used by: R2 sequence
    (TemporalBackbone, FrameNoise, RT AO).
89. R. Ulichney, "Void-and-cluster method for dither array generation", Proc. SPIE 1913, 1993.
    https://doi.org/10.1117/12.152707 . Used by: the blue-noise texture made by `gen_textures.py`.

### Other ideas named in code
- Unreal Engine 5 "Local Exposure" (idea for LocalExposure); Intel XeGTAO (read as reference); HBAO's angle bias.
- The deferred renderer branch (`fd-batch8`: full deferred plan, tiled lights, forward+ transparents, smooth sun)
  cites no paper; only the Burley skin kernel (63) is paper-based there.

---

## 7. References used but not copied from

| Reference | Role | Notes |
|---|---|---|
| WallSoGB's FNV IDA database / symbol map | Engine addresses and structures behind the engine fixes since P75 | Credit WallSoGB wherever his map or headers are used |
| WallSoGB's Fallout-Dynamic-Reflections headers | Struct layouts consulted | — |
| xNVSE headers (`src/NewVegas/nvse`) | The plugin API VS is built on | Vendored; JIP LN NVSE recommended, not bundled |
| momusdigital/fnv_decomp | Read-only reference | No licence granted: **no code copied** |
| Intel XeGTAO | Read while writing GTAO from the paper | MIT; not copied |
| FO4 Community Shaders (northaxosky/fallout4-community-shaders) | Read only | No code taken |
| psycho (acidpointer) | IO worker facts | No licence; learned from, no code |
| FNV ReShade Helper (doodlum, WallSoGB) | Uniform names matched | No licence; written from scratch |
| New Vegas RTX (Kim2091) | DLLs studied for research only | Not used; NVIDIA Remix DLLs off limits |
| gwater2 / NVIDIA FleX | Fluid ideas | FleX rejected (licence); gwater2 ideas only |
| **B42 Optics - NVR Bridge** (Nexus 100461) | **Not used.** Its permissions forbid reuse; none of its files or code was read or shipped. VS's scope work is our own | The 1.0.8 notes thank its author for the idea |
| mrfury97/FNV-Occlusion-Culling | Separate plugin; VS stays compatible | Not shipped in VS |
| DreamSim | The owner's own project; one-way source of the Dream effect | VS code never goes back into it |
| LegacyRT (github.com/adeesababa/LegacyRT, MIT), SpawnMenu, ShaderSwap | Side projects | Not shipped in VS |

---




