# NVR DXVK (tools/nvr-dxvk)

DXVK v3.0.1 (zlib licence, github.com/doitsujin/dxvk) plus the NVR extension, as a patch series on the v3.0.1 tag.
Only d3d9.dll is built and shipped; NVR's own DLL, menu, settings and presets stay as they are.

- `patches/`   the fork: one patch series on top of v3.0.1, applied in file-name order with `git am` (build.sh does
               `git am --3way patches/*.patch`; re-apply onto a newer DXVK the same way). New files
               `src/d3d9/d3d9_nvr_ext*.{h,cpp}` and `d3d9_nvr_hdr.{h,cpp}`, plus small edits to `d3d9_device.{h,cpp}`,
               `d3d9_swapchain.{h,cpp}`, `d3d9_multithread.h`, `d3d9_shader.h`, `d3d9_shader_analysis.h`, `src/d3d9/meson.build`,
               `dxvk_context.h`, `dxvk_shader.cpp`, `dxvk_instance.cpp`, the swap chain blitter and its present shaders.
               Renumbered 2026-10-27 (deep/fork-combo-1027: the port line and the 10-26 feature round in one series):
                 0001-0006  extension 1.0-1.4: compute, full-screen passes, pixel shader replacements, the push-data pipeline
                            library rule, mipmapped/anisotropic samplers, DrawShifted
                 0007       RESZ depth resolve on every GPU vendor
                 0008       1.5: profiling (SetProfiling / GetProfile; NVR's "DXVK PROFILE" lines)
                 0009       injected graphics passes end suspended (no barrier after every pass)
                 0010       1.6: DrawDepthStencil (SMAA's weight pass through its stencil)
                 0011       managed textures (and pending AUTOGENMIPMAP mips) uploaded before a pass samples them
                 0012       "1.5-hdr": ID3D9NvrExtHdr, HDR10 / scRGB output (docs/next/FORK-1026-HDR.md)
                 0013       "1.5-particles": ID3D9NvrExtGeometry, passes with their own vertex shader and instanced draws
                            (GPU particles, docs/next/FORK-1026-PARTICLES.md)
                 0014       "1.5-sm6": 64 slots per type / 96 bindings per pipeline for the SM6 builds of NVR's own passes
                            (`tools/nvr-sm6`, docs/next/FORK-1026-SM6.md; NVR reads MaxSlots from GetInfo)
                 0015       "1.5-debug": dxvk.conf `nvr.debugEnv = "NAME=value|..."` sets environment variables (DXVK_DEBUG,
                            Vulkan loader/layer settings) before DXVK and the loader read them
                 0016       "1.5-debug": ID3D9NvrExtReadback (readback that never waits), storage buffers in the pass stages
                 0017       (deep/fork-async) "1.5-warm": dxvk.conf `nvr.pipelineLog = True` logs every pipeline compiled or
                            linked at first use on the recording thread (a wait the frame cannot hide), with time and shader
                            names; measurement only, off by default; only `src/dxvk/dxvk_{options,graphics,compute}.*` + new
                            `dxvk_nvr_pipelog.h` change. docs/next/FORK-1027-ASYNC-WARMUP.md.
                 0021       (deep/fork2-displace) "1.7-displace": ID3D9NvrExtTess, tessellation displacement of the land
                            (new files `src/d3d9/d3d9_nvr_tess.{h,cpp}`; docs/next/FORK2-DISPLACE.md). Its shaders:
                            `compile-tess.sh` (src/hlsl/NewVegas/Compute/Tess -> .spv, Windows Git Bash with the Vulkan SDK).
                 0024       (deep/fork2-grass) "1.7-grass": ID3D9NvrExtGeometryScene,
                            geometry passes drawn INTO the scene: the game's depth buffer with real depth, multisampled and
                            attachment-only targets, culling, indirect counts (NVR_EXT_BUFFER_INDIRECT); GPU grass,
                            docs/next/FORK2-GRASS.md. Only the three d3d9_nvr_ext* files change.
                 0026       (deep/fork2-dlss) "1.7-dlss": ID3D9NvrExtShare, images and timeline semaphores exported to another
                            process (NVR's 64-bit DLSS helper, `tools/nvr-dlss-helper`, docs/next/FORK2-DLSS.md): plain Vulkan
                            external memory/semaphores, no NVIDIA code; the d3d9_nvr_ext* files, plus `src/dxvk/dxvk_memory.*`,
                            `dxvk_fence.*` and `dxvk_image.cpp`: an NT handle is exported ONCE per object and kept (NVIDIA returns
                            the same handle value on a second export), callers get duplicates.
                 0027       RESZ resolves a multisampled depth buffer to its nearest sample (nvr.depthResolve)
                 0028       (claude/rt-sun-core) "1.8-rt": ID3D9NvrExtRt, the ray tracing core. Geometry staging for the
                            64-bit ray tracing helper (`tools/nvr-rt-helper`; Windows' 32-bit drivers have no ray tracing)
                            on every GPU, plus opt-in in-process acceleration structures (dxvk.conf nvr.rayTracing = True,
                            default off = device created as before). docs/next/FORK-RT-CORE.md. Test: `test/nvr_rt_test.cpp`
                            (in-process) and `tools/nvr-rt-helper/test/rt_helper_test.cpp` (the helper path, end to end).
                 0029       (claude/rt-sun-2) "1.8-rt-uv": NVR_EXT_RT_GEOMETRY_DESC::Flags = NVR_EXT_RT_TEXCOORD_FLAGS(...)
                            makes StageGeometry also write each vertex's texcoord (float2) after the triangle list, for
                            alpha-tested casters in the RT scene; a fork without it ignores the flag (MeasureGeometry's
                            Texels tells NVR which).
                 0033       (claude/rt-sun-2) "1.8-rt-skin": ID3D9NvrExtRtSkin (QueryInterface) StageSkinnedGeometry poses
                            skinned vertices by a bone palette (blend weights / indices from the vertex buffer) straight
                            into scene space, for actors in the RT scene ([Main.Main.RayTracing] Actors).
                            Numbered after the HDR stack's 0030-0032 (claude/hdr-fork-rt) but applies on 0029 alone too
                            (the RT-only forks the train ships build 0001-0029 + 0033).
                 0034       (claude/rt-sun-2, 2026-10-30) "1.8-rt-seq": FIXES a GPU fault on RTX (Conor): the arena copy
                            of FlushStagedGeometry could run in DXVK's init command buffer, BEFORE the conversions that
                            write its staging by address (DXVK saw neither resource used yet), so the helper built from the
                            previous flush's indices. The staging and the arena are now tracked as written before the copy
                            (it stays in the exec buffer, after the conversions). ID3D9NvrExtRtSeq (QueryInterface)
                            FlushStagedGeometrySeq also writes a sequence texel after the conversions; the helper checks it
                            and builds stale geometry empty (logged once). RT-only forks: 0001-0029 + 0033 + 0034.
                 0035       (claude/rtgap-textures, 2026-11-07) "1.8-rt-bc": block-compressed shared images
                            (NVR_EXT_SHARED_BC1 / BC3 / BC5: usage transfer + sampled only, sizes in whole blocks) and a
                            same-format block copy in CopyToShared / CopyFromShared (offsets on blocks, sizes whole blocks or
                            to the subresource's edge; another format into a BC image is refused). The RT real textures then
                            copy the game's DXT1 / DXT5 / ATI2 blocks as they are instead of decoding them to RGBA8
                            (docs/next/RT-REAL-TEXTURES.md). No new interface: an older fork refuses the new formats and NVR
                            falls back to RGBA8. Only the d3d9_nvr_ext* files change. Built on r13 (0001-0029 + 0033 + 0034 +
                            the HDR stack) in WSL ~/fork-rtgap-tex; RT-test forks: + 0035.
               GetInfo reports 1.6. The "1.5-<feature>" parts are separate interfaces found with QueryInterface (or, for sm6,
               by MaxSlots), not by the minor version: NVR checks each one itself and keeps its D3D9 path without it.
- `debug/`     dxvk.conf templates for debugging (`dxvk.conf.pipelinelog`: the 0017 measurement): `dxvk.conf.markers` (RenderDoc captures with NVR's named groups),
               `dxvk.conf.renderdoc-slot` (RenderDoc loaded as a Vulkan layer in a slot game, for the 'capture' shot) and
               `dxvk.conf.validation` (Khronos validation + synchronization validation). docs/next/FORK-DEBUG-CAPTURE.md.
- `build.sh`   clone + patch + build the 32-bit d3d9.dll (Linux or WSL, MinGW-w64). The GitHub workflow
               `.github/workflows/nvr-dxvk.yml` runs the same and uploads the DLL as an artifact.
- `test/`      `nvr_ext_test.cpp`: an end-to-end test of the extension on dxvk-native (Linux, any Vulkan device, also
               lavapipe software Vulkan): the AvgLuma compute pass against the D3D9 pass's arithmetic, storage images,
               buffers, the graphics passes, every ported NVR .spv, a pixel shader replacement (`nvr_ext_test_replace.frag`
               for a hand-assembled ps_3_0), device Reset. Arguments: AvgLuma.spv, the three test .spv, the Compute
               folder, nvr_ext_test_replace.spv. See docs/next/NVR-DXVK-EXTENSION.md.

The public interface NVR compiles against is `src/d3d9/d3d9_nvr_ext_api.h` in the patch; NVR's copy is `src/core/NvrDxvkExt.h`.
