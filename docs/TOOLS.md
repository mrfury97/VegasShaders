# Tools and asset folders

The tools, test benches and asset folders of the repo, one section per folder: the DXVK fork patches (`tools/nvr-dxvk`), the 64-bit DLSS helper
(`tools/nvr-dlss-helper`), the RT test host and the RT playground, the test kit builders, the generated and contributed textures
(`resource/Textures`) and the GunFX assets (`GunFX`). Each section keeps the text of the README it replaced. The folders' build scripts and
source comments name their section here. The file map and runtime names are in [REFERENCE.md](REFERENCE.md).

## Contents

- [nvr-dxvk](#nvr-dxvk)
- [nvr-dlss-helper](#nvr-dlss-helper)
- [nvr-rt-host](#nvr-rt-host)
- [nvr-rt-playground](#nvr-rt-playground)
- [testkit](#testkit)
- [Textures](#textures)
- [GunFX](#gunfx)

## nvr-dxvk

*Original title: NVR DXVK (tools/nvr-dxvk)*

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
                 0012       "1.5-hdr": ID3D9NvrExtHdr, HDR10 / scRGB output (docs/next/FORK-1026-HDR.md (not in this repo))
                 0013       "1.5-particles": ID3D9NvrExtGeometry, passes with their own vertex shader and instanced draws
                            (GPU particles, docs/next/FORK-1026-PARTICLES.md (not in this repo))
                 0014       "1.5-sm6": 64 slots per type / 96 bindings per pipeline for the SM6 builds of NVR's own passes
                            (`tools/nvr-sm6`, docs/next/FORK-1026-SM6.md (not in this repo); NVR reads MaxSlots from GetInfo)
                 0015       "1.5-debug": dxvk.conf `nvr.debugEnv = "NAME=value|..."` sets environment variables (DXVK_DEBUG,
                            Vulkan loader/layer settings) before DXVK and the loader read them
                 0016       "1.5-debug": ID3D9NvrExtReadback (readback that never waits), storage buffers in the pass stages
                 0017       (deep/fork-async) "1.5-warm": dxvk.conf `nvr.pipelineLog = True` logs every pipeline compiled or
                            linked at first use on the recording thread (a wait the frame cannot hide), with time and shader
                            names; measurement only, off by default; only `src/dxvk/dxvk_{options,graphics,compute}.*` + new
                            `dxvk_nvr_pipelog.h` change. docs/next/FORK-1027-ASYNC-WARMUP.md (not in this repo).
                 0021       (deep/fork2-displace) "1.7-displace": ID3D9NvrExtTess, tessellation displacement of the land
                            (new files `src/d3d9/d3d9_nvr_tess.{h,cpp}`; docs/next/FORK2-DISPLACE.md (not in this repo)). Its shaders:
                            `compile-tess.sh` (src/hlsl/NewVegas/Compute/Tess -> .spv, Windows Git Bash with the Vulkan SDK).
                 0024       (deep/fork2-grass) "1.7-grass": ID3D9NvrExtGeometryScene,
                            geometry passes drawn INTO the scene: the game's depth buffer with real depth, multisampled and
                            attachment-only targets, culling, indirect counts (NVR_EXT_BUFFER_INDIRECT); GPU grass,
                            docs/next/FORK2-GRASS.md (not in this repo). Only the three d3d9_nvr_ext* files change.
                 0026       (deep/fork2-dlss) "1.7-dlss": ID3D9NvrExtShare, images and timeline semaphores exported to another
                            process (NVR's 64-bit DLSS helper, `tools/nvr-dlss-helper`, docs/next/FORK2-DLSS.md (not in this repo)): plain Vulkan
                            external memory/semaphores, no NVIDIA code; the d3d9_nvr_ext* files, plus `src/dxvk/dxvk_memory.*`,
                            `dxvk_fence.*` and `dxvk_image.cpp`: an NT handle is exported ONCE per object and kept (NVIDIA returns
                            the same handle value on a second export), callers get duplicates.
                 0027       RESZ resolves a multisampled depth buffer to its nearest sample (nvr.depthResolve)
                 0028       (claude/rt-sun-core) "1.8-rt": ID3D9NvrExtRt, the ray tracing core. Geometry staging for the
                            64-bit ray tracing helper (`tools/nvr-rt-helper`; Windows' 32-bit drivers have no ray tracing)
                            on every GPU, plus opt-in in-process acceleration structures (dxvk.conf nvr.rayTracing = True,
                            default off = device created as before). docs/next/FORK-RT-CORE.md (not in this repo). Test: `test/nvr_rt_test.cpp`
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
                            (docs/next/RT-REAL-TEXTURES.md (not in this repo)). No new interface: an older fork refuses the new formats and NVR
                            falls back to RGBA8. Only the d3d9_nvr_ext* files change. Built on r13 (0001-0029 + 0033 + 0034 +
                            the HDR stack) in WSL ~/fork-rtgap-tex; RT-test forks: + 0035.
                 0036       (2026-11-17, security review) "1.5-debug": from dxvk.conf alone nvr.debugEnv sets only DXVK_DEBUG,
                            DXVK_LOG_LEVEL, DXVK_HUD, DXVK_FILTER_DEVICE_NAME (the bench's GPU pick), VK_KHRONOS_VALIDATION_VALIDATE_*,
                            VK_LAYER_ENABLES, VK_LOADER_LAYERS_DISABLE and ENABLE_VULKAN_RENDERDOC_CAPTURE; any other
                            name (VK_ADD_LAYER_PATH, PATH, ...) needs NVR_DXVK_DEV set in the game's environment. Applies
                            on the full stack (git am on v3.0.1). Built 2026-11-17 in WSL ~/fork-secfix (the fork-rtgap-tex tree + 0036), d3d9.dll
                            sha256 e9ad7814; RT test host PASS on it and its log shows the refusals. In no release yet.
                 0037       (upstream 1.1.2/1.1.3) HDR blitter: fallback descriptors and an HDR metadata guard (sets the
                            swap chain's HDR metadata only when the mode changed or none is set). Not RT. Merged 2026-11-26
                            with upstream 1.1.1-1.1.3; the installed fork was not rebuilt for it (RT needs neither patch).
               GetInfo reports 1.6. The "1.5-<feature>" parts are separate interfaces found with QueryInterface (or, for sm6,
               by MaxSlots), not by the minor version: NVR checks each one itself and keeps its D3D9 path without it.
- `debug/`     dxvk.conf templates for debugging (`dxvk.conf.pipelinelog`: the 0017 measurement): `dxvk.conf.markers` (RenderDoc captures with NVR's named groups),
               `dxvk.conf.renderdoc-slot` (RenderDoc loaded as a Vulkan layer in a slot game, for the 'capture' shot) and
               `dxvk.conf.validation` (Khronos validation + synchronization validation). docs/next/FORK-DEBUG-CAPTURE.md (not in this repo).
- `build.sh`   clone + patch + build the 32-bit d3d9.dll (Linux or WSL, MinGW-w64). The GitHub workflow
               `.github/workflows/nvr-dxvk.yml` runs the same and uploads the DLL as an artifact.
- `test/`      `nvr_ext_test.cpp`: an end-to-end test of the extension on dxvk-native (Linux, any Vulkan device, also
               lavapipe software Vulkan): the AvgLuma compute pass against the D3D9 pass's arithmetic, storage images,
               buffers, the graphics passes, every ported NVR .spv, a pixel shader replacement (`nvr_ext_test_replace.frag`
               for a hand-assembled ps_3_0), device Reset. Arguments: AvgLuma.spv, the three test .spv, the Compute
               folder, nvr_ext_test_replace.spv. See docs/next/NVR-DXVK-EXTENSION.md (not in this repo).

The public interface NVR compiles against is `src/d3d9/d3d9_nvr_ext_api.h` in the patch; NVR's copy is `src/core/NvrDxvkExt.h`.

## nvr-dlss-helper

*Original title: NvrDlssHelper (tools/nvr-dlss-helper)*

The 64-bit helper program behind New Vegas Reloaded's DLSS upscaler (`[Shaders.TAA.DLSS] Enabled = true`; before 2026-10-27 `[Shaders.TAA.FSR] Upscaler = 1`). Fallout New Vegas,
NVR and the NVR DXVK `d3d9.dll` are 32-bit; NVIDIA ships DLSS (NGX, `nvngx_dlss.dll`) for 64-bit Windows only, so DLSS runs
in this separate program, on the same GPU, with its own Vulkan device. NVR starts it, shares four GPU images (eight with Ray
Reconstruction) and two GPU timeline semaphores with it through the NVR DXVK fork (patch 1.7-dlss, `ID3D9NvrExtShare`) and a
small shared-memory block (`NvrDlssProtocol.h`, protocol 3), and it exits with the game. Design: `docs/next/FORK2-DLSS.md` (not in this repo);
Ray Reconstruction: `docs/next/FORK2-DLSS-RR.md` (not in this repo).

Licence: this folder is MIT (`LICENSE`). NVIDIA's SDK parts are NVIDIA's (`NVIDIA-SDK-NOTICE.txt`). Nothing from NVIDIA's SDK
is in this repository, and none of it is in NewVegasReloaded.dll.

### Getting NVIDIA's DLSS SDK

From https://github.com/NVIDIA/DLSS (the build was made with tag v310.9.16), read its `LICENSE.txt` and keep, in a folder
outside this repository:

    include\nvsdk_ngx*.h            (the API headers)
    lib\x64\nvsdk_ngx_s.lib         (Windows_x86_64\x64 in NVIDIA's tree: the static-CRT loader library)
    lib\rel\nvngx_dlss.dll          (the DLSS runtime that is shipped)
    lib\dev\nvngx_dlss.dll          (NVIDIA's development runtime with the on-screen overlay: testing only, never shipped)
    lib\rel\nvngx_dlssd.dll         (optional: the Ray Reconstruction runtime, "DLSS-D"; lib\Windows_x86_64\rel in NVIDIA's tree)
    lib\dev\nvngx_dlssd.dll         (optional: its development build, testing only)
    LICENSE.txt

`nvngx_dlssd.dll` comes from the same repository (https://github.com/NVIDIA/DLSS, `lib/Windows_x86_64/rel/nvngx_dlssd.dll`)
under the same licence terms as `nvngx_dlss.dll`. It is not in this repository either. The local SDK copy the builds use
(`tools\nvr-dlss-helper\dlss-sdk`, 2026-10-30) does not have it, nor NVIDIA's DLSS-D headers; the helper does not need those
headers (see "Ray Reconstruction" below).

### Building

Visual Studio 2019 (v142) x64 and the Vulkan SDK headers + `vulkan-1.lib` (`%VULKAN_SDK%`, else `C:\VulkanSDK\1.3.296.0`):

    msbuild tools\nvr-dlss-helper\NvrDlssHelper.vcxproj /p:Configuration=Release /p:Platform=x64 ^
      /p:DlssSdkDir=<SDK folder> /p:OutDir=<out>\ /p:IntDir=<obj>\

`DlssSdkDir` defaults to `%NVR_DLSS_SDK%`, then `tools\nvr-dlss-helper\dlss-sdk`. Without the SDK it still builds, for the
pass-through test only (`NVR_DLSS_NO_SDK`). `/p:DlssDevDll=true` copies NVIDIA's development `nvngx_dlss.dll` (its jitter and
motion-vector overlays help to check NVR's inputs). The release `nvngx_dlss.dll` is copied next to the exe, and
`nvngx_dlssd.dll` too when the SDK folder has it (the build says which).

### Installing (by hand)

Either next to the game's `FalloutNV.exe` / `d3d9.dll`, or (tidier) in its own folder:

    <game>\Data\NVSE\Plugins\NvrDlss\NvrDlssHelper.exe
    <game>\Data\NVSE\Plugins\NvrDlss\nvngx_dlss.dll        (DLSS only; the pass-through test does not need it)
    <game>\Data\NVSE\Plugins\NvrDlss\nvngx_dlssd.dll       (Ray Reconstruction only; optional)
    <game>\Data\NVSE\Plugins\NvrDlss\LICENSE.txt           (NVIDIA's, from the SDK, when shipping nvngx_dlss.dll / nvngx_dlssd.dll)

Not straight into `Data\NVSE\Plugins` (xNVSE tries to load every DLL there as a plugin). It needs the NVR DXVK `d3d9.dll`
built with patch 0026 (1.7-dlss) and an NVIDIA GPU. The helper writes `NvrDlssHelper.log` next to itself (or into %TEMP%).

### DLSS presets (`[Shaders.TAA.DLSS] Preset`)

NVR sends its own number every frame. The helper turns it into one NGX preset hint per quality mode, set before the DLSS
feature is made, so a change re-creates the feature (as a size or quality change does). An NVIDIA App or driver override wins
over the hint (NGX logs "Using DRS Overridden Preset").

| Preset | hints |
|---|---|
| 0 | none: the DLL's own choice (310.9.16: transformer K, M at Performance, L at Ultra Performance). Nothing is written to NGX until another preset was used once; after that, 0 writes "default" (0) to every mode. |
| 1 | CNN: E at Quality, Balanced, Performance and Ultra Quality; F at DLAA and Ultra Performance (deprecated in 310.x) |
| 2 | transformer, pinned: K at DLAA, Quality, Balanced and Ultra Quality; M at Performance; L at Ultra Performance |
| 3 | transformer J at every mode |

Any other number counts as 0. The log names the preset in its "NGX: DLSS feature made" line.

### The weapon's bias mask (protocol 3, `[Shaders.TAA.DLSS] WeaponMask`, 2026-11-01)

Player report: with DLSS the first-person arms and gun ghosted and smeared while they animated. The weapon is fixed to the camera,
so its motion vectors are 0 and its own sway, bob, recoil and reload were in DLSS's history only. NVR now makes one more shared
image (r16f, `NvrDlssShared::biasMask`, feature bit `NVR_DLSS_FEATURE_BIAS_MASK`): WeaponMask on the weapon, FSR's reactive value
elsewhere (`src/hlsl/NewVegas/Compute/DlssBiasMask.comp`). The helper gives it to DLSS SR as NGX's
`DLSS.Input.Bias.Current.Color.Mask` on frames flagged `NVR_DLSS_FRAME_BIAS_MASK` (WeaponMask above 0); with `--inputs exact` it
is copied at the render size like the other inputs. RR frames do not use it. A protocol 2 helper refuses the block (NVR runs
FSR 2, logged). Every self-test exports the mask too and checks `biasMaskFrames` (all 300 frames in the pass-through, none with
`rr`, every DLSS SR frame with `dlss`).

### Ray Reconstruction (protocol 2, `[Shaders.TAA.DLSS] RayReconstruction`)

With NVR's RR switch on, NVR makes four more shared images, the RR guides: diffuse albedo, specular albedo, normals and
roughness. It flags some frames as RR frames and sends the camera's world-to-view and view-to-clip matrices with them. The
helper evaluates those frames with NVIDIA's DLSS Ray Reconstruction (NGX feature `RayReconstruction`, "DLSS-D") instead of
DLSS SR. It needs:
- an RTX GPU and everything DLSS SR needs;
- `nvngx_dlssd.dll` next to the helper (NVIDIA's DLSS SDK, see above);
- NGX's capability `SuperSamplingDenoising.Available`, checked at start.

RR has its own NGX feature and its own parameter map, and both features stay alive while frames alternate. RR may not run: no
nvngx_dlssd.dll, an old driver, or a failed feature creation or evaluate. The helper then:
- says why in its log and its status line ("NGX: Ray Reconstruction not available (...); DLSS SR goes on");
- reports `rrAvailable = 0`;
- runs DLSS SR on that frame's picture.

It never gives up on DLSS SR because of RR. NVR stops sending RR frames once it sees `rrAvailable = 0`.

Without NVIDIA's DLSS-D headers, the helper sets RR through NGX's generic parameter API with string keys. The keys were
checked against the strings of `nvngx_dlss.dll` 310.9.16 and of the driver's NGX core. A few values are from memory and not
verified: denoise mode 1, `DLSS.Use.HW.Depth` 1 = hardware depth, the normals' space and the matrix layout. The list is in
`NvrDlssHelper.cpp`, above `RayReconstructionCapability`. RR presets (`RRPreset`): 0 none, 1 D, 2 E, the same at every
quality mode.

2026-11-15 (`docs/next/DLSS-RR-MODEL.md` (not in this repo)): `RRPreset` 3 = preset F, NVIDIA's second-generation RR transformer ("DLSS 4.5 Ray
Reconstruction"). It is already in `nvngx_dlssd.dll` 310.9.16 (its strings name Preset_A to Preset_F and the "RR2" kernels for
sm_75 to sm_120); it needs driver 580.00 or newer, so with an older driver the helper sends E instead and says so. `RRPreset` 4 =
auto: F on Ada and newer (RTX 40 / 50, L4), E on Turing and Ampere (RTX 20 / 30, where PCGH measured up to 25-29 % lower frame
rate), from the PCI device id (`GenerationOf` in `NvrDlssHelper.cpp`). Each RR feature creation logs "NGX: Ray Reconstruction
model: ... (why)", also in the game's log. `--probe` prints what 3 and 4 would send on the PC. A helper from before reads 3 and 4
as 0 (no hint); the protocol is unchanged.

In the pass-through test (`HelperTest = 1`), RR frames need no NGX. The helper stretches the colour as always and draws the
four guides as thumbnails along the bottom edge of the picture: each a sixth of its width, from the left diffuse albedo,
specular albedo, normals, roughness. Roughness shows as red, and the normals' negative parts as black. This shows on any NVIDIA
GPU, a GTX 1070 included, that NVR's RR side reaches the helper.

### By hand

    NvrDlssHelper.exe --probe       the GPU, Vulkan interop support, and whether DLSS is available on this PC; then
                                    "probe: Ray Reconstruction (SuperSamplingDenoising.Available) AVAILABLE" or
                                    "... not available: <reason>", and "nvngx_dlssd.dll next to the helper: yes / no"
    NvrDlssHelper.exe --selftest    the whole exchange without the game: this process exports images and semaphores like the
                                    fork, starts a second copy of itself as the helper (pass-through), runs 300 frames and checks
                                    the pixels, the marker (frame order) and the round trip; PASSED / FAILED at the end
    NvrDlssHelper.exe --selftest dlss   the same with the child in real DLSS mode: on an RTX card it must run the frames
                                    (each counted as a DLSS frame; DLSS's picture is not compared with the pattern); on
                                    a card without DLSS it must give up cleanly with the NGX reason (that is NVR's fallback)
    NvrDlssHelper.exe --selftest rr     the RR exchange (protocol 2): eight images, every frame an RR frame, the four guides
                                    cleared to known colours; the pass-through child must report rrAvailable, count all 300
                                    frames as RR frames and show each guide's colour in its thumbnail
    NvrDlssHelper.exe --selftest dlss rr   both: on an RTX card every frame must run as RR, or as DLSS SR where RR is not
                                    available (logged); without DLSS it must give up cleanly as above

The logs are NvrDlssHelper-selftest.log, -selftest-dlss.log, -selftest-rr.log and -selftest-dlss-rr.log.

2026-10-30 (tester report: with DLSS on, the render-scaled picture stayed small in the top-left corner): every self-test fills
the colour input outside the render rectangle green and FAILS if green reaches the output, the small picture in the corner.
`--selftest dlss` on an RTX card checks that for real DLSS too. Add `subrect` to run the child the old way (`--inputs subrect`).

2026-10-31 (RTX 4070 Ti SUPER tester: with `--inputs exact` DLSS 310.9.16 refused every frame below RenderScale 1, "RenderSubrect
(0x0) outside of Min ... and Max ... dynamic res"): the child checks the render subrect of every frame and must still be READY at
the end. In the pass-through that is the subrect a real DLSS evaluate would be given, so a GTX card catches what only NGX refused
before; with `--selftest dlss` on an RTX card the child also refuses a subrect outside NGX's dynamic range (`--subrect-range
strict`). Add `zerosubrect` to give the child that 0x0 again (`--fail-test zero-subrect`): it PASSES only if the check catches it.

### Start options (from NVR's settings)

- `--inputs exact` ([Shaders.TAA.DLSS] HelperExactInputs 1, the default): with RenderScale below 1, NVR's colour, depth and
  motion are screen-sized pictures with the frame in their top-left render rectangle. The helper copies that rectangle into
  images of exactly the render size each frame (about 0.1 ms at 1080p), and DLSS and the pass-through read those whole: the
  input size is the render size, the plainest way to use DLSS. DLSS is still given the render size as its render subrect, at
  0, 0 (2026-10-31: the first build, b0146f54, gave it 0x0, which DLSS 310.9.16 refuses). `--inputs subrect` (HelperExactInputs
  0, as before): DLSS is told the rectangle and reads it in place. If DLSS read the whole picture anyway (an nvngx_dlss.dll
  without dynamic resolution, a driver-side DLSS override), the frame showed small in the corner.
- Every frame's render subrect is checked before DLSS is called (2026-10-31): it must be NVR's render rectangle, not empty,
  inside each input and no larger than the output, or DLSS is not called and NVR goes to FSR 2 (the log says why). One outside
  NGX's dynamic range for the mode is logged once and NGX decides; `--subrect-range strict` (the self-test's child) refuses it
  too. The pass-through test runs the same check on every frame and reports a problem as a failed DLSS evaluation reports one.
- `--fail-test zero-subrect` (TEST ONLY, self-test `zerosubrect`; not reachable from NVR's settings): the 0x0 subrect of
  b0146f54 with `--inputs exact`, for the check to catch.
- `--fail-test report|hang` (HelperFailTest 2 / 3, TEST ONLY): from frame 120 the helper reports a failure as a failed DLSS
  evaluation does, or stops answering (NVR's watchdog ends it after 3 seconds). HelperFailTest 1 is NVR's own: it ends the
  helper process. Each must leave FSR 2 upscaling the whole screen.
- At each DLSS feature creation the log gives DLSS's own optimal render size and dynamic range for the mode, next to NVR's
  RenderScale size ("NGX: DLSS's optimal settings ..."). A size outside that range points at a DLSS override in the NVIDIA
  App or NVIDIA Profile Inspector.

On the GTX 1070 (2026-10-27): probe = "DLSS not available" (no tensor cores), self-test PASSED (helper GPU work 0.04 ms,
response 0.05-0.07 ms average), `--selftest dlss` PASSED (gives up in 0.3 s with "NGX: DLSS not available on this GPU").
Protocol 2 (2026-10-30): `--selftest rr` and the presets have not been run yet. RR itself can only be tried on an RTX card.

## nvr-rt-host

*Original title: NvrRtHost: the RT framework's test bench without the game (2026-11-06)*

A small 32-bit Windows program that plays the game's side of the ray tracing core (`src/core/RtCore.cpp`) and drives the real
`NvrRtHelper.exe` through the real protocol, so the trace shaders, the helper, its geometry path, the alpha test and the
temporal history can be tested in seconds. No Fallout New Vegas launch.

```
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-host\run.ps1
```
builds the helper (MSBuild x64, its own project) and the host (cl x86), checks the constant-block layouts, runs every scene, and
prints PASS / FAIL per scene (exit code 0 = all passed). Every scene of the merged branches (2026-11-07: + tessslack, contact, decals, normalmaps, facemorph, tessblas; ptaaa-materials: + materials; ptaaa-firstperson: + ptviewmodel, hiddennear, vmprep; ptaaa-omm: + omm, ommbreak; ptaaa-denoiser: + ptsplit, ptspecmove, ptobjmotion, ptsplitstill, ptdisocclusion, ptdenoisecost; perf-rr: + rrrect, DLSS RR's guide pass and composite at RenderScale < 1; perf-reflfix: + reflfix, smooth vertex normals at traced hits and MetalBounce, a chrome sphere in a mirror; perf-flashlight: + flashhead, a lamp in the hidden first-person head under SameFrame lamp shadows; and the fog / emissive scenes), a few minutes in all on this PC (9800X3D iGPU) with the build;
prints PASS / FAIL per scene (exit code 0 = all passed). Every scene of the merged branches (2026-11-07: + tessslack, contact, decals, normalmaps, facemorph, tessblas; ptaaa-materials: + materials; ptaaa-firstperson: + ptviewmodel, hiddennear, vmprep; ptaaa-omm: + omm, ommbreak; ptaaa-denoiser: + ptsplit, ptspecmove, ptobjmotion, ptsplitstill, ptdisocclusion, ptdenoisecost; perf-rr: + rrrect, DLSS RR's guide pass and composite at RenderScale < 1; perf-reflfix: + reflfix, smooth vertex normals at traced hits and MetalBounce, a chrome sphere in a mirror; perf-reflfix2: + reflorbs (RT reflections' lamps and the flashlight vs the game's own light, the key-lamp pick), reflbounce (MetalBounce's GGX lobe vs the CPU's, the roughness code), ptflashlight (the path tracer's flashlight cone in every variant); perf-reflfix3: + reflnear (a lamp by a ceiling, the flashlight at its origin), reflshimmer (the gold's shimmer in a mirror), hairtint (undrawn hair's tint); and the fog / emissive scenes), a few minutes in all on this PC (9800X3D iGPU) with the build;
the first run after a shader change takes ~45 s (the helper compiles the new pipelines, then they are cached).

| option | what |
|---|---|
| `-Compile` | compile the scenes' shaders from `src\hlsl\NewVegas\Compute\Rt\*.comp` now (into `bin\spv`) instead of running the committed `.spv` |
| `-Spv <folder>` | run the trace `.spv` files of that folder (all ten the scenes use: see `build.ps1 -Compile`'s list) |
| `-Scene furnace,mirror` | only these scenes |
| `-Fork <d3d9.dll>` | the NVR DXVK fork to test with (default: the bench copy `W\bench\deep-roadmap\rtmerge\fork-r13\d3d9.dll`, or `$env:NVR_RT_HOST_FORK`) |
| `-Device <name part>` | DXVK's and so the helper's GPU (default `Radeon`: the iGPU; the GTX 1070 has no ray queries) |
| `-NoBuild` | skip the build |

(2026-11-07 perf-ptcost) Environment knobs: `NVR_RT_HOST_PTCOST=r,b,c[,n]` writes the path tracer's ray budget keys (src/core/RtPtCost.h:
PTRestirDIRays r, PTBounceLights b, one camera query c) into every upload of the pt lamp image's frame row, so any pt scene runs the old
budget (`1,1,0`) or the new one (`0,0,1`; the camera's one query is only right without the hidden first-person body); `n` = 1 the count
mode: RtPathTrace writes 1 + its ray queries a pixel into the display's alpha and the host prints `PTCOST ray queries a pixel and frame`
after each scene. Unset, the scenes write 0 there (the new budget, the camera's two queries).

Output in `tools\nvr-rt-host\out\` (git-ignored): `report.txt` (the whole run), a `.pfm` of every failed scene's output image,
the DXVK log. The helper writes `NvrRtHelper.log` next to itself (`bin\`).

### Design: why the end-to-end host (option a), not synthetic captures (option b)
Two ways were open: (a) a host that makes a D3D9 device on the fork and goes through the same extension path as RtCore, or (b) a
generator of `.nvrcap` captures for `NvrRtHelper.exe --replay`. (a) was cheap because `tools/nvr-rt-helper/test/rt_helper_test.cpp`
(2026-10-28) already did the plumbing (device, shared images, timeline semaphores, StageGeometry, the helper as a child); the host
grew out of it. So (a) covers what (b) cannot:
- **shared images** made by the fork, rounded to 256 texels as `RtCore::AddImage`, imported by the helper by handle; inputs go in
  through `CopyToShared` from D3D9 textures (`RtCore::CopyIn`), outputs come back through `CopyFromShared` (`RtCore::CopyOut`);
- **geometry staging**: D3D9 vertex / index buffers converted by the fork into the arena (`StageGeometry`, texcoords through
  `NVR_EXT_RT_TEXCOORD_FLAGS(12)`, the arena sequence texel through `FlushStagedGeometrySeq` when the fork has patch 0034), BUILD
  and RELEASE ops, built by the helper;
- **instances + records** written as RtCore writes them: camera-relative transforms, cull masks (0x01 land, 0x02 objects, 0x10
  alpha-tested), record flags with the alpha reference (bits 8-15), the atlas slot (16-25) and the material rule (26-31);
- **passes** with the game's image lists (stand-ins included) and the game's constant structs (mirrored: the layout check below
  keeps the mirrors honest), the exchange (`SignalShared` / `WaitShared`) and the frame ring.
Since the second round (2026-11-06 b) it also covers:
- **RtCore's scene picking**: the rules `RtCore::AddGeometry` applies to every geometry (cull angle, no data, skinned, leaf cards,
  lighting shader, decals, blended-only vs alpha-tested, far LOD) and the record / mask / build-flag packing now live in
  `src/core/RtScenePick.h` (plain C++, no game types). RtCore calls it; the host tests every rule and packs its own records with it.
  The walk over the game's node trees (which NiGeometry reaches AddGeometry at all) still needs the game.
- **the game's D3D9 side**: RtDenoise's fork passes (`RtDenoiseTemporal.spv`, `RtDenoiseAtrous.spv`) run on the host's device with
  RtDenoise::Run's bindings and push blocks; the lamp shaders' lookup `PointShadowTraced` (`PointShadowForward.hlsl`) is compiled
  ps_3_0 as the game compiles it and reads the traced result through `NVR_RtLampReproject` from the next frame's camera.
- **the other passes**: sun shadows, AO / sky, the path tracer with the albedo atlas and the lamp image (all-lamp loop vs
  PTLightGrid, the grid built by `src/core/RtLightGrid.h`, moved out of RtPathTrace.cpp unchanged), ReSTIR GI and ManyLights' one
  ray a pixel.
Still not covered: the resolves / composites that turn these outputs into the picture (Reflections' resolve, the object shaders'
lighting maths around the lookup), DenoiseBackbone (RtDenoise's RGB and specular modes: since 2026-11-07, the `pt*` scenes), RT GI's own reuse pass (RtGITrace), the radiance cache,
glow lights, actors / skinned geometry (only `facemorph`'s one-bone head goes through StageSkinnedGeometry). Those still need the game (or the D3D9 frame replay).

### What is checked
**Layouts** (`rt_layout_check.py`, no GPU): each trace pass's C++ constant struct (`RtLampShadows.h` PassConstants,
`RtPathTrace.cpp` PassConstants + RestirConstants, `RtSunShadows.cpp` PassConstants (2026-11-07: also the host's SunConstants mirror), `Reflections.cpp` RtPassConstants) against
its shader's std140 block: every member named on both sides at the same byte offset, the block no longer than the struct, both
within 512 bytes; and the host's mirror structs equal to the game's. (RtAOTrace has three blocks by `#ifdef`, not checked.)

**Scenes** (128 x 128, every output checked against an exact CPU reference on the same triangles; pixels on a shadow / alpha /
geometry edge, where a 0.3-pixel shift changes the answer, are skipped and counted):

| scene | shader | world | pass condition |
|---|---|---|---|
| `furnace` | RtPathTrace | an open box of albedo 1 under a uniform sky L (48 bounces, 16 paths); then a floor of albedo 0.5 | box: image mean = L within 1.5 % per channel (measured 0.04 %); floor: every pixel 0.5 L within 0.2 %; no NaN |
| `alphacard` | RtLampTrace + RtAlpha.glsl | a lamp above an alpha-tested card (atlas slot 37, the second row: an 8 x 8 checker, ref 128) above a floor | every pixel's entry (slot, visibility 0 / 15, depth code) = the CPU's ray with the same alpha test; both shadowed and lit pixels present |
| `material` | the same | the same card whose record also has material bits 26-31 (rough 7, metal 5) | the result equals `alphacard`'s bit for bit (the 2026-11-06 unmasked-slot bug) and the reference |
| `manylamps` | RtLampTrace | 48 lamps on a grid over land, blocks beside lamps 3, 33, 40, 47; 3 lamps a pixel | every pixel's three entries (ranking + visibility) = the CPU's; > 1000 correct entries name slots 32-47 and some pixels are shadowed from them |
| `mirror` | ReflectionsRT | a mirror floor, red and green blocks, a plate shading the red block's front; RayGen's inputs made on the CPU | hit distance, flags, normal, albedo and the sun visibility at the hit = the CPU's; no-ray pixels -1 |
| `cameramove` | RtLampTraceMany | 40 lamps, 7 blocks; 40 frames at camera A, then the camera slides 260 units and traces once with the history (ping-pong pair, reprojection matrix as RtLampShadows builds it) | the rest estimate R stays near the exact sum(w V) / sum(w) (0.004 at A, 0.006 after the slide); a stale reprojection run alongside must be > 2x worse (measured 0.044: the test sees ghosting); the two exact entries per pixel all right |
| `sun` | RtSunShadow | alpha card + block under a low sun, traced at half resolution (64 x 64 of a 128 x 128 depth), Penumbra on | every traced pixel's visibility and blocker distance = the CPU's ray from the shader's own point (the traced uv with the full-size pixel's depth) |
| `ao` | RtAOTrace (plain pass) | floor, wall, roof, alpha card; 8 AO rays, 16 sky rays | AO, sky visibility and "covered" = the CPU repeating the shader's rays exactly (its packed normal decoded the same way); at most 0.3 % of texels off (rays grazing an edge) |
| `pathtex` | RtPathTrace + RtPathTraceGrid | a textured floor (atlas slot 45, material bits set); then 64 white lamps through the lamp image, with and without blocks | DebugView 1 = the atlas texel's colour, sRGB-decoded; DebugView 3 (direct light) within the exact sum's range over the pixel; PTLightGrid (built by the game's RtLightGrid::Build) bit for bit equal to the all-lamp loop |
| `restirlamps` | RtLampTraceRestir | the cameramove world, ManyLights' one ray a pixel (3 x 3 jobs, spatial radius 1, two history pairs) | after 60 frames the exact entries right (edges skipped) and R within 0.05 of the exact; after the slide still close, a stale reprojection worse |
| `restirdi` | RtPathTrace (PTRestirDI) + the game's `RtRestirRow.h` | floor, 7 blocks, a wall, 40 coloured lamps, the lamp list shuffled every frame (the reservoirs survive only through the game's remap); a narrow view (a pixel ~5 units, as the game's) | 40-frame mean within 2 % of the exact shadowed lamp sum (and the plain pick's within 2 %); frame noise < 0.6 x the plain pick's (measured 0.19 x); after a 150-unit slide < 0.8 x (0.33 x) and within 3 %; the run with the remap broken > 1.2 x worse (11.5 x) |
| `specfurnace` | RtPathTrace (PTSpecular) | the furnace box in white metal (metal 1, F0 1), roughness 3/7 and 6/7, uniform sky, 48 bounces x 16 frames x 16 paths; one bounce off a white metal floor | box mean = L within 2 % (3/7) / 3 % (6/7, heavy-tailed weights); floor mean = L within 1.5 % (printed by view cosine). Control `NVR_RT_HOST_NO_ENERGY=1` (no multiple-scattering compensation): 0.79 / 0.19 / 0.52 |
| `cache` | RtPathTrace (PTRadianceCache) + RtPathTraceCache | the furnace box in albedo 0.8 under a uniform sky; reference 32 bounces, 128 paths | 2 bounces without the cache < 0.97 (0.61: the scene needs multi-bounce light); 2 bounces + the cache within 2.5 % (0.983), every 16 x 16 tile within 6 %; the sky halved, 60 frames later within 3 % of half (1.002) |
| `emissive` | RtEmissiveBuild + RtPathTrace (PTEmissive, 2026-11-07 ptaaa-emissive) + the game's `RtEmissiveRow.h` | a dark room: a 600 x 300 sign of 8 x 8 quads whose glow map (atlas slot 200) lights 40 of its 128 triangles, a small glowing box (slot 201), a block shadowing the floor; all but the floor albedo 0, so the floor shows exactly albedo x its glow irradiance (CPU: 6 x 6 points a triangle, the shader's sRGB decode, shadow rays); 6 lamps for the ReSTIR runs | glow only (4 paths x 64 frames): PTEmissive within 2.5 % of the exact, bounce-only brute force within 4 %, their ratio within 3 % (no double counting), noise < 0.5 x brute force; lamps + glow (1 path): plain + PTEmissive and ReSTIR DI + PTEmissive within 2.5 %, ReSTIR's noise < 0.7 x ReSTIR with the glow by bounces only; glossy floor (GGX 2/7): on / off means within 4 %; both instances listed; pass GPU times printed |
| `ptcost` | RtEmissiveBuild + RtPathTrace as PTRealtime runs it (3 bounces, PTBounceAlpha 3, PTRestirDI, PTEmissive, PTSpecular; 2026-11-07 perf-ptcost, `src/core/RtPtCost.h`) | a yard under the sun + sky: the emissive scene's sign + glowing box, 6 lamps, a back wall, a glossy side wall, a block, 7 posts, every surface lit and bouncing; then the same closed into a room (walls, ceiling, the camera inside, no sun / sky) | per pass, 192 frames a run, the ray queries a pixel counted by the shader: A the old budget (PTRestirDIRays 1, PTBounceLights 1, two camera queries, PTRestirDISpatial 4), B PTBounceLights 0, C PTRestirDIRays 0 + 2 neighbours, D all new + one camera query, E (printed only) PTRestirDIRays 3; B, C, D's picture mean within max(1.5 %, 4 standard errors) of A's, D's per-pixel noise < 1.3 x A, D's rays < 0.8 x A's (measured: yard 10.01 -> 6.52 rays, room 11.51 -> 7.85; means within 0.1 %; noise 1.03 / 1.02 x) |
| `ptemcost` | RtEmissiveBuild + RtPathTrace as PTRealtime runs it, PTEmissive's cost keys (2026-11-11 pt-emissive-cost, `src/core/RtEmissiveRow.h` CostKeys / PackCost, frame row texels 289.y-291.x) | ptcost's yard with the game's ray budget, three lightings: noon (sun + sky, no lamps), night (a dim sky, the sign is the light), room (closed, 6 lamps + the sign); PTGlow 1 | per lighting, 128 frames a run (`NVR_RT_HOST_PTEMCOST_FRAMES`), the ray queries counted by the shader: A PTEmissive with the keys 0, PTEmissive off (printed only), then PTEmissiveCut 0.02 / 0.005, PTEmissiveFloor 0.002 / 0.01, PTEmissiveHits 1, PTEmissiveRoulette 0.1 and all of them; every keyed run's picture mean within max(1.5 %, 4 standard errors) of A's (the keys are unbiased), no NaN; at noon PTEmissiveCut 0.02's rays < 0.9 x A's and its noise < 1.1 x A's (measured at noon: A 7.26 rays, off 3.38, Cut 0.02 4.05 with the pass -25 % and noise 0.98 x; at night and in the room the cut changes nothing; RT-PATH-TRACING.md section 69 has the table) |
| `restirgi` | RtPathTraceGI + RtRestirGI | floor + two coloured walls, sun + sky; reference = the plain tracer's 512-path mean | the reuse's picture (x the demodulated albedo) averaged over frames 8-39 within 3 % of the reference's mean (measured 2.6 %); one frame's RMSE with the history < 0.85 x without; a frame right after a slide within 5 % |
| `cgnsdi` | RtPathTrace (PTRestirDI) + RtRestirRow.h, PTRestirCGNS (2026-11-15 rt-reuse, docs/next/RT-REUSE-CGNS.md (not in this repo)) | the restirdi lamps over a floor with 30 posts and 3 blocks, the game's PTRestirDISpatial 2 / radius 16 | off, off with other seeds, CGNS on (NVR_RT_HOST_CGNS_K candidates, default 8), 64 frames each against the exact shadowed lamp sum (4 x 4 points a pixel): SMAPE, RMSE, lag-1 / lag-4 temporal autocorrelation, all pixels and edge pixels; GPU time off / on / off / on; an FNV hash of the off frames (the committed shaders must give the same). Pass: finite, on's mean within 2 % of off's, its SMAPE <= 1.05 x off's |
| `cgnsgi` | RtPathTraceGI + RtRestirGI, PTRestirCGNS / PTRestirDupMap (2026-11-15 rt-reuse) | restirgi's floor and walls + 30 posts, sun + sky; reference the plain tracer's 1024-path mean | off, off with other seeds, CGNS, DupMap, both: the same metrics and pass rule as cgnsdi |
| `ptsplit` | RtPathTrace (PTDenoiseSplit, flag 4194304; the G-buffer's materials) (2026-11-07 ptaaa-denoiser) | a glossy half-metal floor, a gold block, a rough white wall under the sun + sky, 2 bounces; a mirror floor (roughness 0.05, metal 1) with a block and a wall | the same paths with and without the split: display x colour + specular x its albedo = the combined picture on every pixel within 0.4 % (measured: 0 of 98304 off, sums within 0.002 %); specular share gold > 80 % (100 %), rough wall < 15 % (2.8 %), floor between (24 %); PTSpecular off: no specular light; the specular hit distance of each specular-lobe pixel within the CPU's reflected-ray range over the pixel on >= 95 % (99.5 %), misses written 60000 (99.9 %) |
| `ptspecmove` | RtDenoiseTemporalSpec (fork pass, D3D9 side) | analytic: a mirror floor reflecting a striped wall + sky, the camera sliding 12 units a frame after 24 still frames; clean and noisy (x (1 + 0.35 n)) | mean error vs the exact reflection over the last 8 moving frames: the hit-distance (virtual) reprojection < 0.6 x the surface-only one at AntiLag 0 and 3 (measured 0.048 vs 0.098, 0.047 vs 0.098), noisy RMSE below it (0.087 vs 0.177); still: 0 error |
| `ptobjmotion` | RtDenoiseTemporalRgb (bilinear history) with Settings.ObjectMotion | analytic: a still camera over a floor, a box whose light pattern moves with it 1.5 px a frame, TESR_ObjectMotionBuffer on its pixels | error on the box (the ghost) with the object motion < 0.3 x without at AntiLag 0 (0.046 vs 0.209), < 0.6 x with AntiLag 3 + MovingHistory 8 (0.030 vs 0.178); no floor trail |
| `ptsplitstill` | RtPathTrace + the whole RtDenoise chain (RGB temporal + 2 a-trous; the specular temporal + 2 a-trous) | the `ptsplit` scene, still, 32 frames of one path a pixel | RMSE against the path tracer's own 512-path mean, frames 24-31: split <= 1.05 x today's combined denoise (measured 0.0129 vs 0.0129), mean within 3 % |
| `ptdisocclusion` | RtDenoiseAtrousRgb's history fix (PTDenoiseHistoryFix) and MeanVariance (PTDenoiseMeanVariance), through the RGB chain | analytic: a flat light 0.6 with a shadow band and a 1-pixel shadow line, one path a pixel (lit with chance 0.25) | newly seen pixels (one frame of history): RMSE with the history fix < 0.6 x without (measured 0.133 vs 0.980); 32 still frames: the output with = without (0 difference); the 1-pixel line after 32 frames keeps > 15 points more of its depth with MeanVariance 2 (83 % vs 61 %) for < 1.1 x the flat noise (1.00 x) |
| `ptdenoisecost` | the same chains at 1280 x 720 | D3D9 timestamp queries round the fork draws, the two chains interleaved | informational: prints today's chain and the specular chain's GPU time (the iGPU is shared with other runs: noisy) |
| `ptconfidence` (2026-11-15 rt-denoise-inputs) | RtDenoiseConfidence.frag + RtDenoiseTemporalRgb-conf (Settings.Confidence: PTDenoiseConfidence / GIDenoiseConfidence) through the RGB chain | analytic: a still floor, a lamp's pool switched off / on / sliding 1.5 px a frame, one path a pixel lit with chance 0.25 and 0.8 | k 3 or k 4 (both printed; 2026-11-15 only k 4 passed): the lamp-off ghost < 0.7 x without at chance 0.25; still RMSE <= 1.05 x and flicker <= 1.10 x at both; on / sliding, false alarms and the 720p cost printed (docs/next/RT-DENOISE-INPUTS.md (not in this repo)) |
| `rrmirror` (2026-11-15 rt-denoise-inputs) | DlssRRGuides-mirror (RRMirrorMotion) vs DlssRRGuides | analytic: a flat marked mirror floor reflecting a wall, a curved marked part, unmarked floor, sky; last frame's camera turned 1.5 degrees and moved | the motion at the flat mirror = the CPU's mirror image of the reflected point projected with last frame's camera (worst < 0.05 px); everything else untouched; the five guides bit for bit the plain pass' |
| `rrsky` (2026-11-15 rt-denoise-inputs) | FSRPrep.comp | all sky (depth 1 at farZ 353840), the camera turned 2 + 1 degrees and moved 10 / 100 units | the sky's motion within 0.1 px (1920 x 1080) of a direction at infinity on a walk: RR's sky needs no motion of its own |
| `denoise` | RtDenoiseTemporal + RtDenoiseAtrous (fork graphics passes, D3D9 side) | a one-sample signal (each pixel lit with probability = a known clean value) over a floor with a darker disc and a box | after 16 frames the flat-area error < 0.3 x the noise's (0.028 vs 0.19), no bleeding next to the box; after a slide the error round the disc's edge with a stale camera > 1.5 x the right one's |
| `sigmalamps` | RtLampTraceSigma + the helper's RtShadowDenoise module (2026-11-07, docs/next/RT-DENOISER.md (not in this repo)) | floor, two poles, a floating plate; two lamps of physical size (radius 14 and 7) | against the EXACT soft shadow (576 rays a pixel per lamp, the shader's own disc sampling): after 48 still frames the penumbra error < 0.35 x the raw ray's, below the old Filter's and 8 rays', bias < 0.03, contact < 0.5 x raw + 0.02; a camera sliding 6 units a frame: < 0.5 x the raw ray's, a frozen reprojection > 1.2 x worse |
| `sigmasun` | RtSunShadow (half resolution) + the module in sun mode | the same world under a low sun: a 10 degree test disc and the real 0.53 degree sun | the 10 deg disc as sigmalamps (still < 0.5 x raw, moving < 0.6 x raw); the real sun (penumbrae under a pixel: the ray is nearly the answer): better than the raw ray at rest, no worse than 1.1 x it in motion |
| `refldenoise` | the RT reflection denoiser (src/core/RtReflDenoise.h: ours = core 0, the AMD FidelityFX Reflections port = core 1; fork passes, D3D9 side; 2026-11-07, docs/next/RT-REFLECTION-DENOISER.md (not in this repo)) | analytic: a floor in roughness bands 0.02 / 0.08 / 0.2 / 0.4, coloured boxes, a bright panel, a sky; one VNDF ray a pixel as RayGen draws it (RoughShare borrowing); the old Temporal + Blur ported to the CPU line for line | against the EXACT reflection (32 x 32 stratified samples per pixel): ours below the old path at rest (all and contact), in motion (slide + turn, worst frame) and below 0.5 x the raw ray at rest; prints raw / old (today's noise) / old (fixed noise) / ours / FFX per band, contact and flicker; `NVR_REFL_CORES=0,1` picks the cores, `refl_sheet.py` makes pictures of the dumps |
| `lamplookup` | PointShadowForward.hlsl `PointShadowTraced` (ps_3_0, D3DCompile) + RtLampTrace / RtLampTraceMany | frame A traced (48 lamps, or ManyLights after 24 frames); camera B slid sideways looks each pixel's lamp up | every answer from A's trace = the CPU's visibility at B's point; "not traced here" only where A's top three really lack the lamp; off-screen only off A's screen; ManyLights: a lamp outside the top two gets R |
| `tessblas` | ReflectionsRT + the helper's displaced copies (`NvrRtDisplace.inl`, protocol 8; 2026-11-07) | a rolling land grid (3072 triangles) with two height layers (textures in alpha, real textures) blended by per-vertex weights sent as two DATA ops (the weight elements staged as positions, as RtCore does), a quadrant border across it and the fade band in view; a parallax box (height in red) scaled 1.25, edges pinned; rays start in the air and aim down over the bumps; three frames: camera A, camera B 300 units on (copies refit), displacement off | hit distance (0.75 + 0.002 t) and the sun ray at the hit = a CPU transcription of TerrainDisplace / ObjectDisplace .tesc/.tese on the same N x N split (land N 6, box N 8) on >= 99 % / 98.5 % of the stable rays (measured 100 % / 100 %); the flat geometry must agree on < 80 % (32 % / 24 %), on the box < 95 % (88 %); at B the un-refit copies of A must agree < 99 % (94.6 %); displacement off = the flat answers (100 %). Printed: the RT split vs a 16 x 16 split, 0.23 units mean |dt| |
| `sameframe` | RtLampPrimary + RtLampTrace + `PointShadowTraced` (2026-11-07) | a walker between a low lamp and a wall, restaged each frame as an actor (DYNAMIC, REBUILD every 3rd), a hidden first-person body (`NVR_RT_CUSTOM_HIDDEN`) in front of the camera, a new jitter each frame; 7 frames | SameFrame: camera rays + trace in one exchange, looked up with this frame's camera: 0 wrong answers, > 90 % answered; the camera rays' depth + normal = the CPU's; the hidden body passed over; the pipelined control (last frame's trace reprojected) > 40 wrong a frame (measured 217-278: the ghost) |
| `movingrest` | RtLampTraceMany + RtLampTraceRestir, MovingCasterHistory (2026-11-07) | 40 lamps, a walker still 30 frames then walking 8 | R vs the CPU's exact R where the walker's shadow is or was: the smear above a noise-floor run (cap 2, walker never moved) > 0.06 with no cap and under 0.35 x of that with cap 2 (measured 0.141 -> 0.025 Many, 0.110 -> 0.037 one ray); the rest of the floor < 0.06 / 0.1 |
| `texpack` | RtTexturePack.h + RtVideoMemory.h (no GPU; 2026-11-07, protocol 8) | the real textures' rules: auto caps tiers, routes / levels / footprints, 400 random BC footprints packed, least-recently-used release, id reuse; prints this PC's adapters' video memory as RtCore reads it (D3DKMT vs 32-bit DXGI) | every rule as RtCore applies it; D3DKMT and the DXGI fallback land each adapter in the same tier |
| `realtexbc` | RtLampTrace + RtTextures (CPU route) | the realtex chain-link as BC3 and BC1 blocks | realtex's bar (99 % agree, 90 % of the exact shadow, tiles < 0.025) |
| `realtexgpu` | RtLampTrace / RtPathTrace + RtTextures, the game's GPU route through the fork | D3D9 DEFAULT-pool DXT1 / DXT5 / ATI2 textures sent as RtCore sends them (RtTexturePack + CopyToShared into the transfer images; BC1 / BC3 / BC5 when the fork has 1.8-rt-bc, RGBA8 always): the chain-link shadow; a colour floor (path tracer first-hit colour) vs the same texels as BGRA8; a 2048 texture at cap 2048 | chain-link: realtex's bar on every route; colour: mean difference < 0.004 (measured 0.0000); 2048: routes equal, > 2 % of pixels differ from cap 1024 (35 %) |
| `texrelease` | RtLampTrace + RtTextures | an opaque real texture over a clear atlas slot, then TEXTURE_RELEASE, then the id reused for the chain-link | full card shadow; no shadow after the release; realtex's bar after the reuse |
| `tessslack` | RtPathTrace (PTPrimary, DebugView 3), RtSunShadow, RtLampTrace, RtAOTrace + the game's `RtTessContact.h` (2026-11-07, RT-DETAIL-GAPS.md 1a) | a flat RT floor; the G-buffer a +-10 bump field round it (Strength 20, Center 0.5: the tessellation's), sun / one lamp above, nothing that can shadow | without the bumps' slack the passes see the bug (dark pixels: path tracer 12.9 %, sun 22.1 %, lamp 37.0 %, AO covered / started inside 24.5 %); with it each <= 0.2 % (measured 0 / 0 / 0.01 / 0 %) and the path tracer's lit pixels within 3 % of the exact sun term (0.12 %) |
| `contact` | the same passes' contact march (RtContact.glsl; 1b) | the G-buffer holds a floor + a 60 x 60 x 10 step the TLAS lacks; then a flat floor alone, from above and at a grazing angle | contact on: >= 70 % of the floor in the step's near shadow (the CPU's ray meets the step within half the march) dark (measured 92.6 % sun, 98.4 % lamp, 91.5 / 98.0 % path tracer sun / lamp), off: none; free pixels darkened <= 1 % (0.07-0.22 %); the flat floor: on = off at every pixel |
| `normalmaps` | RtPathTrace (DebugView 2 / 3 / 5) + RtAOTrace (GI, one ray) + ReflectionsRTTex (2026-11-07, RealNormalMaps) | real normal-map textures (protocol 7 set 1, no normal atlas): a floor of 30-degree stripes, a uniformly tilted ceiling over a G-buffer-only floor, a striped wall; a sun 15 degrees over each | every pixel's light / normal = the CPU's bend (the shader's tangent frame from positions + texcoords) x the grazing sun; the PT's bounce keeps the flat normal where the bend faces the ray away, at the CPU's exact share (+-0.015); record flag off, or on with no map in the slot (the PT's normal-atlas binding a stand-in): flat within 2e-3 |
| `facemorph` | RtFaceMorph.h + the fork's StageSkinnedGeometry (patch 0033) + RtLampPrimary (2026-11-07, FaceMorphs, docs/next/RT-DETAIL-GAPS.md (not in this repo) 4) | no GPU: the hash (40-vertex morphs all seen; single vertices exactly when sampled) and the feed decisions (follows / never follows / a stray miss / own buffer only / unreadable frames), PatchPositions through a vertex map; GPU: a skinned 24 x 24 "head" (one still bone, the partition layout) with ActorStillSkip on, its mouth opened in the partition's buffer, then only in the CPU vertices (reversed vertex map) | the camera rays' depth = the CPU's exact rays on every stable pixel of every frame; no BUILD op on unchanged frames; the switch to the patched copy within 8 frames of CPU-only changes; control (FaceMorphs off) > 50 pixels keep the shut mouth (measured 705) |
| `omm` | NvrRtOmmMath.h (CPU) + the helper's opacity micromap builder RtOmm.comp and its check (which calls RtAlpha.glsl's rtAlphaPass) (2026-11-07, ptaaa-omm, protocol 9: NVR_RT_OMM_SELF_TEST, since this GPU has no VK_EXT_opacity_micromap) | CPU: the spec's curve at levels 0-10 (bijection; random points), the thresholds against every 8-bit and half alpha x 255 refs x ref within 2 ulp x 4 dithers, fused and not. GPU, one object a step: an atlas card at ref 128 and 30, a 288-triangle grid with tiled / negative / jittered uv (ref 100), a real 256 texture (BGRA8, mips) in lod-0 frames (RtLampTrace) and in "any mip" frames (RtLampTrace copied under an unknown name), the atlas card again in such frames, a decal | 0 curve / threshold failures; every key built once (the class-B atlas card and the decal: never); 0 violations over >= 10000 check samples on opaque micro-triangles a key (262144 points a key, a quarter on micro-triangle edges, 8 dithers, class B also at random cones); opaque shares: card 128 in 0.2-0.4 (measured 0.259), ref 30 above it (0.347), grid > 0.02 (0.200), real texture lod 0 > 0.15 (0.347), every mip below lod 0 (0.000) |
| `ommbreak` | the same, NVR_RT_OMM_BREAK_TEST (run LAST: it turns the micromaps off for the helper's session) | the builder marks every micro-triangle opaque | the check finds violations (measured 176746 of 262144) and the state goes to DISABLED with the helper's one-line reason |
| `scenepick` | RtScenePick.h (no GPU) | 21 picking cases, 15 mask / op / record cases, 73728 record packings (1024 slots x refs x material rules) | every fate, mask and flag as AddGeometry had them; every packed field reads back as the shaders read it (slot `& 0x3FF`), nothing spills |
| `decals` | RtLampTrace, ReflectionsRTAlpha, RtAOTrace, RtPathTrace + RtScenePick.h (2026-11-07, RT-DETAIL-GAPS.md 5; atlas slots 80-85) | decals as RtCore takes them (alpha-tested, ref by DecalAlphaRef, lifted 0.5 by AverageNormal + DecalLift, custom index NVR_RT_CUSTOM_DECAL): a floor decal and a floating checker card under a lamp; a mirror floor, a wall with a poster decal, a floating decal card between the wall and the sun; AO + sky with a decal card and a plain card overhead; the path tracer's direct light (DebugView 3, 64 lamps) on a floor under a floating decal card | lamp: 0 mismatches against the CPU (decals skipped by shadow rays, seen by camera rays), the decal card shadows NO pixel while the same card as a plain cut-out shadows > 500 (2084); mirror: 0 mismatches, > 200 reflection rays meet the poster (1110), > 50 sunlit hits behind the floating decal card (1253); AO with AOAlpha 2 and 3: within sceneAO's tolerance, > 100 texels open only because the decal card is passed (289 / 586); path tracer: 0 floor pixels change against the open floor (13970 checked, > 500 under the card: 1458), the plain card changes > 500 (2211). The shaders before the change: 2084 / 1253 / 2292 / 2559 mismatches (FAIL) |
| `fogvis` | RtFogVisibility + the game's `RtFogGrid.h` (2026-11-07, ptaaa-fog; docs/EFFECTS.md "VolumetricLight", "RT fog visibility") | a closed room with a window toward the sun, an alpha-tested checker card outside (atlas slot 97), a floor; the froxel grid 32 x 32 columns x 64 slices; the camera in the room; 24 still frames, then 16 frames walking 8 units + turning 0.5 degree a frame | fixed points (1 and 4 rays a cell): every cell whose answer is the same 0.5 unit round its points = the CPU's rays (0 mismatches; > 50 cells shadowed by the card, lit through its cut-outs, lit in the shaft); jittered + history: still RMS of the partly lit cells < 0.5 x one frame's, bias < 0.01 (measured 0.058 vs 0.248); walking < 0.65 x one frame's (0.0294 vs 0.0545), the control without the reprojection > 1.3 x worse (0.0615). `NVR_FOG_TEMPORAL`, `NVR_FOG_TEMPORAL_MOVING`, `NVR_FOG_RAYS` sweep the settings |
| `materials` | RtPathTrace (glass, foliage, skin, cloth; 2026-11-07 ptaaa-materials, RT-PATH-TRACING.md section "ptaaa-materials") + RtMaterials.h / RtScenePick.h | (a) an open white box under a uniform sky with a glass pane over it (the camera looks through it), a white leaf card and a white skin card inside, 48 bounces; (b) a closed room, a hole in the roof glazed with a tinted pane, the sun through it (DebugView 3); (c) a leaf card / skin card seen from below, the sun below then above; (d) a skin floor vs a plain one, sun at 90 / 45 / 15 / 5 degrees; (e) a G-buffer floor of MaterialBuffer class 3 vs 1, uniform sky and a low sun behind the camera; (f) a pane between the camera and a uniform sky; (g) no GPU: glass picking, the kind bits, masks, op flags, skin texture paths | (a) mean = L within 1.5 % (measured 0.27 % worst; materials off 0.01 %); (b) every stable lit floor pixel = albedo x sun x cos x the CPU's thin-slab T (Fresnel n 1.5, the tint) within 0.5 % (0 off; T 0.923 / 0.717 / 0.512), no pane = untinted, PTGlass 0 = passed clear; (c) back-lit = PTFoliage x front-lit (0.400), skin back = 0.25 x flesh red x the plain front, means within 0.3 % (0.04 %), setting 0: back = 0; (d) means = the wrap formula within 0.3 %, |skin - plain| <= 0.25 albedo x sun (0.231); (e) cloth never above L (or plain + 0.01), the rim > 1.1 x plain (1.50); (f) each incidence bin = L (R + T) within 1.5 %, PTGlass 1 = L; (g) 0 wrong |
| `hairmirror` | ReflectionsRTAlpha, ReflectionsRTAlphaTex + RtScenePick.h actor packing (2026-11-07 perf-mirrorfix; atlas slot 86) | an actor's hair card (alpha test + blend, reference 0 -> 128, striped strands, tinted 0.8 0.3 0.1) in front of a wall over a mirror floor, the sun behind; packed as RtCore::AddSkinned packs it now (ALPHA_TESTED + TEXTURED = tinted, mask 0x20, a DYNAMIC non-opaque build) and, as the control, as before (opaque, TEXTURED) | the new packing: 0 mismatches against the CPU (1851 rays), > 200 reach the wall through the gaps (604), > 200 meet the strands (719) in the tint's colour (the record's; with the atlas: texel x tint x 2^2.2), > 50 wall hits sunlit through the gaps (423); the old packing: 0 mismatches against a SOLID card, > 200 rays stopped where the gaps are (596) |
| `scenepick` | RtScenePick.h (no GPU) | 31 picking cases (2026-11-07: + decals / refraction), 29 mask / op / record / decal cases (custom index, DecalAlphaRef, AverageNormal, DecalLift), 73728 record packings (1024 slots x refs x material rules) | every fate, mask and flag as AddGeometry had them; every packed field reads back as the shaders read it (slot `& 0x3FF`), nothing spills |
| `ptviewmodel` | RtPathTrace (PTViewModel; 2026-11-07 ptaaa-firstperson, rt_host_firstperson.inl) | a floor and a wall; the hidden body round the eye (head + torso + legs 0x20, a third-person gun 0x40, all NVR_RT_CUSTOM_HIDDEN); a "gun" plate drawn into the G-buffer through the view model's narrower FOV (s 1.45), its normals' alpha 4 + s, part of it marked not captured (-1) | DebugView 3, a lamp in the head: every plate pixel within 2 % of the CPU's light at its TRUE (first-person) point (the world-FOV point is > 5 % off), floor + wall = the CPU without the body (> 200 pixels a visible body would darken: the reverse shadow); a lamp behind the player: the floor keeps the body's shadow (>= 95 %), the plate lit; DebugView 1: the plate's colour / the game's pixel where not captured, exact; DebugView 5 (sky): the plate's light body hidden = absent within 0.01, visible lower |
| `hiddennear` | RtSunShadow, RtLampTrace, RtAOTrace + Rt/RtHidden.glsl (2026-11-07 ptaaa-firstperson) | a counter in front of the player (its top within 80 units of the eye), the floor beyond, the hidden body with its third-person gun over the counter; three TLAS variants (body hidden / absent / visible) | bit for bit: within reach of the eye (every pixel for a lamp in the head) hidden = absent; beyond reach hidden = visible (the body's shadow and occlusion stay); the visible body changes > 20 pixels of each set |
| `flashhead` | RtLampPrimary + RtLampTrace + RtLampTraceSigma + RtLampEye (2026-11-07 perf-flashlight) | a floor, a back wall, a crate; the hidden first-person body; lamp 0 inside the head (the Pip-Boy light as a game lamp), lamp 1 low behind the crate; SameFrame exchanges, three TLAS variants (body hidden / absent / visible) | hidden = absent bit for bit (camera depth + normals, the lamp result, the Sigma signal, RtLampEye's 256 values); absent = the CPU on every stable pixel; the visible head blocks lamp 0 at >= 99 % of the pixels it lights (measured 100 %); RtLampEye: lamp 0 = 1 in every variant (people left out), lamp 1 = 0, empty entries 1 |
| `shutdown` | no trace shader: NvrRtHelper's `ShutdownDrain` + RtCore's `Core::Stop` order (= `Host::Stop`) (2026-11-07, claude/rt-shutdown; FORK-RT-CORE.md "Shutdown rules") | two SECOND hosts (own device, semaphores, helper with `--log NvrRtHelper-shutdown-quit.log` / `-kill.log`): (quit) 1 frame run, 4 published as the game does them without waking the helper, then quit; (kill) the helper's parent is a stand-in process (`NvrRtHost.exe --idle`), 1 frame submitted whose inputs ready never comes + 2 never run, then the stand-in killed | quit: the helper exits by itself (code 0) with `exit: output ready 5 of 5 ... GPU idle ... clean` and our queue drains; kill: exits by itself with output ready 4 of 4 and `GPU idle` (FORCED inputs is expected); every wait bounded. The main host's own shutdown (end of every run) uses the same order and FAILs the run when it had to force anything. Note: after the helper exits the host reads output ready as 2^64 - 1, so the helper's own count (status line) is checked |
| `screenown` | RtViewModel.h kPrepPS (the marks) + RtPathTrace (PTScreenOwn; 2026-11-11, rt_host_screenown.inl) | a wall and an armour plate (captured), a body (skin in the main list), a head (skin drawn after it), a blended hair cap over the head and the wall, an untraced thing; the G-buffer as the game leaves it | the prep pass marks = the CPU (also at RenderScale 0.5, one-sided, skin with / without the skin blur); the first surface per region before (no marks: the wall's colour and metal on hair and skin) and after (their own): PTDebugView 1 / 4 / 2 |
| `vmprep` | src/core/RtViewModel.h kPrepPS (ps_3_0 via D3DCompile on the fork's D3D9 device, drawn as RtPathTrace.cpp ViewModelPrep draws it; 2026-11-07 ptaaa-firstperson) | the ptviewmodel plate in a reversed view-model depth (near 4) and the captured normals (octahedral world normal, true distance), part left with the world's capture, a block of world pixels marked a person | every pixel's code and normal = the CPU's: the plate 4 + s and its view-space normal, the uncaptured part -1, people 2, the rest 1 with TESR_NormalsBuffer's normal |
| `farhandover` | src/core/RtFarHandOver.h kBlendPS / kGainPS / kShadowPS / kMapPS (ps_3_0 via D3DCompile on the fork's D3D9 device, drawn with the blend states of RtPathTrace.cpp FarDraw; 2026-11-12 PTGameFog / PTFarMatch / PTFarBlend) | a floor seen from a rise out to 40000 units, the loaded square's edge across it; a block of pixels not traced, a block the kept first-person model | A every pixel = the CPU's: traced x gain(ramp) x (1 - fog)(1 - band) + fog colour + the game's frame x band, others untouched (alpha too); B the shadow buffer: lit inside, the game's value by the band's share in the band, untouched past the edge; C the map pass + RtFarHandOver::Ratio return the ratio the game frame was built with (1.5 1.2 0.9), LoadedSquare's two cases |
| `denoisepeople` (+ 2026-11-07 ptaaa-firstperson) | RtDenoiseTemporalRgb | the camera 300 units aside, a column-gradient history, the left half marked the view model (4.45) / a person (2) | the view model's pixels take their own screen point's history (all), the control none |
| `historytime` | src/core/RtHistoryTime.h (no GPU) + RtDenoiseTemporalRgb (2026-11-07 perf-history, rt_host_historytime.inl) | the rules at 60 / 20 / 144 fps, the cap, the smoother; then a still floor through the real pass at dt 1/60 and 1/20 (time-based and frame-based): 40 noisy frames with a step (AntiLag 3, PeopleHistory 4 on half); a step 0.8 -> 0.1 under Bernoulli noise; a still Bernoulli 0.3 scene; a lit square moving 40 px/s | 60 fps: today's push and the time-based one give the same bits; 20 fps time-based: the step's ghost within 0.03 of 60 fps' at 0.1 / 0.2 / 0.35 / 0.5 / 1 s and cleared within a frame + 10 % of 60 fps' time (frame-based: not within 2 s), the moving light's trail profile in pixels within 0.03 (frame-based: its mean distance >= 1.2x), the still scene unbiased (< 0.005 without AntiLag) |

**Proof that it catches the 11-06 bugs** (2026-11-06, scratch builds, not committed):
- `RtAlpha.glsl` before eacf46e1 (slot = flags >> 16, unmasked) + current RtLampTrace: `alphacard` PASS, `material` **FAIL**
  (1754 mismatches, 0 shadowed pixels: every ray passed the card).
- `RtLampTrace.comp` before e66e766b (`manyUnused[7]`): `alphacard` and `manylamps` **FAIL**; `rt_layout_check.py` on that file:
  "RtLampTrace.comp reads 'jitter' at byte 256 (vec4 16), RtLampShadows.h PassConstants writes it at byte 240 (vec4 15)".
- The lamp-slots-32-63 bug of 11-06 (43d5cf53) was in NVR's D3D9 lamp shaders, not the helper: out of this bench's reach;
  `manylamps` / `cameramove` prove the traced side handles slots past 32.

**The RT shadow denoiser's cores** (2026-11-07): `NVR_SIGMA_CORE=1` (AMD FidelityFX Shadows) or `2` (FidelityFX with our penumbra-sized
steps) runs sigmalamps / sigmasun on that core instead of ours (0); their pass conditions are ours, so 1 and 2 FAIL (the comparison is
in docs/next/RT-DENOISER.md (not in this repo)). `sigma_sheet.py` turns the scenes' PFM dumps into pictures.

### Adding a scene
In `rt_host.cpp`: build a `World` (meshes + objects with masks / record flags / alpha slots; `Mesh::quad`, `Mesh::box`), stage it
(`stageWorld`), fill the pass's constants with the game's struct (mirror it next to `LampConstants` and add the pair to
`rt_layout_check.py` MIRRORS), `AddPass` with the game's image list, `Exchange`, `Read` the outputs, compare with `trace()` (the CPU
ray caster: same triangles, same alpha rule), `releaseWorld`. Register a new shader in `main` (`AddShader`) and a new image with
`AddImage` (at most 48 in all, index 0 the arena). Add the shader name to `build.ps1 -Compile`'s list.

### Rules kept
- Never launches the game. The host starts its own helper on its own shared-memory name (`Local\NvrRtHost-<pid>-<tick>`): it does not
  touch a running game's helper, and it is the only program writing to its helper's shared images.
- The helper is built with its own project command (`msbuild tools\nvr-rt-helper\NvrRtHelper.vcxproj /p:Configuration=Release /p:Platform=x64`).
- Needs: VS 2019/2022 C++ tools (x86 + x64), the Vulkan SDK only for `-Compile`, Python 3 for the layout check, a GPU with ray
  queries for 64-bit programs (here the AMD iGPU), and the fork's d3d9.dll.

## nvr-rt-playground

*Original title: RT playground*

A walkable test room for Vegas Shaders' ray tracing, without the game. It opens a window. You move with WASD and the mouse and see the
path tracer the way the game runs it under `[Main.Main.RayTracing] PTRealtime`. It uses the same `.spv` shaders, the real
`NvrRtHelper.exe` and the NVR DXVK fork's `d3d9.dll`. Use it to see lighting in motion (denoiser lag, noise, moving lamps, the sun
through a window, mirrors) in seconds, without a game launch.

```
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1                 # build + open (640x360 traced, 1280x720 window)
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Size 960x540
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Demo 60   # scripted minute: screenshots + fps in out\
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Demo 60 -Set R=0 -Tag _restir_off   # an A/B
powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 -NoBuild -Demo 20 -Size 320x180 -FixedStep 0.5 -Set "F2=0,F4=0" -Tag _off
```

`-Demo N` plays the scripted 100 s (10 segments: the room, the window + fence, raw, path tracer off, the mirror, the reference, night,
glass + leaf + skin, and the matched references of 05 and 08, below) in N seconds. With `-FixedStep S` the script advances S seconds a frame instead, so A/B runs see exactly the same
frames (the camera, the lamps, the random seeds); the run still stops at N seconds of wall clock. It saves one screenshot a segment (the HUD is the caption) and logs each segment's fps, frame ms and helper GPU ms to
`out\playground.log`. Its exit code is 0 only without helper errors. `-Set <key>=<value>` holds a toggle through it, by its key
(`R=0`, `B=1`, `Z=1`, `G=1`, `F2=0`, `F5=1`); `-Tag` suffixes the screenshot names and shows on the HUD. Each screenshot's line in the log
(and `out\fireflies.csv`) carries a count of bright outliers on the window-size frame (a sparkle detector; at a small `-Size` the frame
is scaled up and a sparkle becomes a block, so count at the internal size instead: the method is in docs/next/RT-PATH-TRACING.md (not in this repo)).
`--max-frames N` stops after N frames.

**Films (2026-11-09).** `-Frames <file>` appends every frame's finished picture (window size, RGB, 3 bytes a pixel) to one raw file;
`-FramesRange a,b` keeps only the demo's script seconds [a, b) (segment 05 = 30,40: 300 frames at `-FixedStep 0.0333`, ~0.8 GB at
1280x720). With `-FixedStep 0.0333` played at 30 a second the film shows what a card that runs the path tracer in real time would show
(noise crawl and ghosting only show in motion): `ffmpeg -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i <file> -c:v libx264 -crf 18
-pix_fmt yuv420p out.mp4`, then delete the raw file.

**Object motion (2026-11-09).** The playground draws the game's object motion buffer for the things it animates (MotionPass: each moved
object drawn again with this frame's and last frame's transform; rgb = the world move since last frame, a = 1, where the depth matches),
binds it as the game does (`t_motion`, backbone.x 1, MovingHistory 8) for both denoisers. Key 5 / `-Set 5=0`: without it (before).

**Scoring against a reference (2026-11-09).** The script now runs 100 s: segments 09 and 10 are the many-sample references (~450
samples) of 05 (the mirror + spheres) and 08 (glass + leaf + skin) at the SAME camera and the SAME moment as those screenshots (the lamps
and the cube frozen there; `UsePose`). `py -3.11 -I tools\nvr-rt-playground\score_mirror.py _tagA _tagB` scores each run's 05 and 08
against them over fixed regions (the wall mirror, the chrome and gold spheres, the floor; the glass pane, the leaf, the sun patch): the
mean absolute error, a high-frequency noise measure, its ratio to the reference's, and the brightness bias. Judge the denoiser at
`-FixedStep 0.0333` (30 frames a second of motion; 0.2 starves every history), the same step for every A/B; at 320x180 the 100 s take
about 7 minutes. At each screenshot the log also gets two `probe` lines (left / right half): the specular denoiser's history length,
the virtual reprojection's share and how many smooth pixels are marked mirror surface (segment 05: the chrome sphere is on the left,
the wall mirror on the right).

`build.ps1` fills `tools\nvr-rt-playground\bin\` with everything the program needs: `NvrRtPlayground.exe`, `NvrRtHelper.exe`,
`d3d9.dll` (the fork), `shaders\Rt\*.spv`, `shaders\Compute\*.spv` and `run-playground.cmd`. That folder runs on its own:
double-click `run-playground.cmd`, or copy the folder to another PC. The compiler's intermediates and TEMP go to
`$env:NVR_BUILD_SCRATCH\playground` when that variable is set, else `obj\` here (`-Scratch` changes it). Each build copies the shaders from the repo again
(`-Compile` compiles them from source instead), so merged shader changes show up on the next run.

`-Device` (default empty) picks the GPU through `DXVK_FILTER_DEVICE_NAME`. Empty: the first GPU the 64-bit helper's `--probe` finds with
ray queries (this PC: the Radeon iGPU, not the GTX 1070; an RTX PC: the RTX).

**Closing** (the window's X, Alt+F4, Esc, Ctrl+C or closing the console, a demo's end, an error) always runs the same shutdown
(`PgHost::Stop`):
1. no more frames; wait on the CPU until the helper has finished every frame submitted;
2. drain the D3D9 queue (SignalShared on our fence, polled: a D3D9 EVENT query never reported done under the fork);
3. ask the helper to quit and wait for it (TerminateProcess only when it is really hung, logged);
4. release every D3D9 / fork object and the device.

If the helper died, `SignalFromHost` releases the GPU waits on it first. Without this, a close left the D3D9 queue waiting on the
helper and reset the display driver (Windows System event 4101). The log's `shutdown:` line says "clean" or what had to be forced.

The first start after a shader change can take a few minutes on the iGPU while the helper compiles its pipelines. The title bar says
so while it waits.

### Controls

| Key | |
|---|---|
| W A S D, Shift | move, faster |
| Space / E, C / Q | up, down |
| right mouse button (held), M (toggle) | mouse look |
| 1-4 | viewpoints: the room, the window + fence, the mirror + spheres, outside |
| P | path tracer on / off (off: the raster shading + RT sun shadows, `RtSunShadow`) |
| N | path tracer mode: **PTRealtime** (denoised) / **reference** (`ReferencePT`: a running mean while nothing moves, up to 4096) / **raw** (the one path a pixel the denoiser gets) |
| B | bounces 1-4 (default 3, as PTRealtime). At 1, PTLastSky and PTLastBounce turn on, as in the game |
| U, arrow keys | sun on / off; turn the sun round (left / right), raise or lower it (up / down) |
| K, L | lamps on / off; the blue lamp moves / stops |
| J | the cube spins / stops (a moving object for the denoiser) |
| G | PTGlowLights (the glowing panel sampled as a light) |
| H | PTDetail |
| X | PTSpecular |
| R | PTRestirDI |
| Y | PTRadianceCache |
| F | PTLampFalloff (the game's / physical) |
| Z | PTIndirectClamp 0 / 4 (the game's firefly cut for light through a bounce) |
| F2 | PTEmissive (glowing triangles as lights: `RtEmissiveBuild` + the emissive row) |
| F3 | PTDenoiseSplit (diffuse and specular denoised apart: `RtDenoiseTemporalSpec`) |
| F4 | PTDenoiseAntiFirefly (`RtDenoiseFirefly`, PTDenoiseFireflyK 2) |
| F11 | PTDenoiseMirrorSurface 0.1 / off (2026-11-09: mirrors and chrome denoised as the thing they reflect; off = as before; `-Set F11=0`) |
| 5 | object motion on / off (2026-11-09: the game's TESR_ObjectMotionBuffer for the things that move here, the cube and the lamp globes; the denoisers subtract it and cap moving pixels at 8 frames, as PTDenoiseObjectMotion; off = no object motion, as before; `-Set 5=0`) |
| F5 | PTGlass 0 / 1 (the light) / 2 (the camera's rays too) |
| F6, F7 | PTFoliage 0 / 0.4; PTSkin 0 / 1 (+ PTClothSheen 0.5) |
| F8 | PTContactShadows |
| O | PTSplit 0.5 (left: the raster frame, right: the path tracer) |
| I | with the path tracer off: RT sun shadows on / off |
| V | PTDebugView: picture, albedo, normals, direct only, rough/metal, direct highlight, reflection only, radiance cache |
| T, + / - | tonemap (ACES fit) on / off; exposure |
| F1 | HUD: full / short / off |
| F12 | screenshot (`out\shot_NNN.png`, the window as shown, HUD included) |
| Esc | release the mouse, then quit |

The HUD and the title bar show the internal and window sizes, fps and frame ms, the helper's GPU time (`gpuTimeUsAvg`, and its
passes `traceUsAvg`), the GPU, the mode, and every toggle.

### What a frame does (the game's PTRealtime path)

1. **G-buffer by D3D9 raster** at the internal size. The outputs are the game's buffers in the game's encodings: `TESR_DepthBuffer`
   (view z / farZ, R32F), view-space normals x 0.5 + 0.5, linear albedo (a = 1 where a traced surface was drawn), the MaterialBuffer
   (r = class + 0.49 metal, b = roughness; class 1 plain, 5 plants, 6 skin), and emissive. Glass is drawn as the game draws it:
   after the frame is shaded, depth written, no AlbedoBuffer colour (the trace meets the pane itself), blended over the frame.
   **The game's frame** is a plain raster shading of that G-buffer, in the stored curve x^(1/2.2): the sun x N.L, a sky ambient, the
   lamps with the game's falloff, all unshadowed, plus the sky gradient of `RtPathTrace.comp`'s `Sky()`. The path tracer reads it for
   pixels it does not trace (flag 32), for `PTGlowFromFrame` and the raster-only bulbs, for `PTDetail`, and for the denoiser's guide.
2. **The trace** (`RtPathTrace.cpp` Record):
   - CopyIn of the G-buffer and the frame.
   - The lamp image: the lamps nearest first, the glow lights (none with PTEmissive), the frame row (`RtRestirRow::Build`: ReSTIR DI
     and radiance cache keys, the GGX energy table; `RtTessContact::PackRow`: PTContactShadows, texels 16-21; texel 22: PTEmissive's
     count and its scratch words; `RtMaterials::PackRow`: PTGlass / PTFoliage / PTSkin / PTClothSheen, texels 24-26), and the emissive
     row 50 (`RtEmissiveRow::Build`; the glowing records' bits 8-15 = their number, `RtScenePick::WithEmissive`).
   - `RtEmissiveBuild.spv` (PTEmissive's list), `RtPathTrace.spv` with the game's flag set (1, 2, 4, 8, 32, 128, 256, 512, 1024, 2048,
     8192, and 262144 / 524288 / 1048576 / 4194304 PTDenoiseSplit where on; pass image 15 = the specular image), then `RtPathTraceCache.spv`.
   - The scratch store is sized as `RtPathTrace.cpp` Setup sizes it (12 words a pixel of reservoirs, the cache's table, PTEmissive's
     blocks; `--cache-log2`, default 20).
3. **CopyOut** of the noisy picture and the first surface's colour; with PTDenoiseSplit also the specular light and its albedo (the
   mean images carry it: r, g | b, roughness).
4. **The denoisers** (`RtDenoise::Run`, RGB mode, as `RtPathTrace.cpp` Show sets them under PTRealtime):
   - diffuse: `RtDenoiseTemporalRgb` (3 targets: the history A / B and the clamped firefly excess), `RtDenoiseFirefly` (the excess
     spread back, PTDenoiseAntiFirefly), then `RtDenoiseAtrousRgb` x2 with steps 1 and 2;
   - specular (PTDenoiseSplit): `RtDenoiseTemporalSpec` (4 targets, the hit-distance / virtual reprojection, MirrorHistory 12), then the
     same firefly and a-trous passes in specular mode. With PTDenoiseMirrorSurface (F11, on by default since 2026-11-09) the trace marks
     smooth pure metals (negative hit distance, the reflected colour divided out); the temporal pass gets the normals as a 10th image
     (a curved mirror's virtual point) and keeps the full history there, the a-trous passes smooth them (bit 4, the raw specular image
     in the guide albedo's slot). docs/next/PT-MIRRORS-2026-11-09.md (not in this repo).

   The settings are PTDenoiseHistory 32, PTDenoiseSigma 1, AntiLag 3, Guide 0.35, HistoryFix 3, MeanVariance 2 and FireflyK 2. The
   push blocks are the game's; `rt_layout_check.py` checks the playground's copies against them.
5. **`RtPathTraceShow`** in PTReplace mode (HDR) onto the scene, with the demodulation, PTDetail, and (split) the specular light x its
   albedo added back. Then an ACES tonemap scales it up
   to the window, and the HUD is drawn over it.

### The room

The room is 1200 x 1000 x 400 units (z up, the game's units) and holds:
- a window in the east wall that the sun shines through, with a chain-link fence card inside it (a real 512x512 texture with alpha,
  so its shadow falls on the floor);
- a skylight gap in the ceiling;
- a tiled floor (a real texture);
- three spheres: diffuse red, gold (rough 0.3, metal), and chrome;
- a wall mirror (metal, smooth);
- a glowing panel (a glow map; PTEmissive samples its triangles);
- a glass pane in the window (record kind glass), a leaf card back-lit by the sun (kind foliage, class 5), a skin "head" (kind skin,
  class 6);
- a pillar, and a cube spinning on a plinth;
- three lamps (warm, a moving blue one, a small one by the mirror) with raster-only bulbs;
- sunlit ground outside with three blocks.

### Adding a scene object

Everything lives in `BuildRoom()` in `pg_scene.h`. Make a mesh and add it:

```cpp
{ Mesh m; m.box({ -50, -50, 0 }, { 50, 50, 120 });          // or m.quad(a, b, c, d, uTiles, vTiles) / m.sphere(radius)
  Object& o = s.add(m, "crate"); o.pos = { 300, -100, 0 };    // s.addInstance(meshIndex, name) reuses a mesh
  o.albedo = { 0.5f, 0.35f, 0.2f };                           // linear
  o.rough = 0.4f; o.metal = 0.0f;                             // optional material rule (record bits 26-31 + the MaterialBuffer)
  o.animate = [](Object& ob, double t) { ob.yaw = float(t); }; }   // optional: moves / turns each frame (J stops it)
```

- **Textures:** add a `MakeTexture(id, size, fn)` to `s.textures` (BGRA8 with sRGB colour, all mips, sent as a real texture), then
  set `o.slot` (a free atlas slot) and `o.diffuse` (+ `o.glowTex` and `o.glow > 0` for a glow map).
- **Cut-outs:** `o.alphaTested = true` takes the alpha from the diffuse texture. Make that texture with `MakeTexture(..., 0.5f)`
  (coverage-preserving mips); box-filtered mips fade thin wires under the test and the card vanishes at a distance.
- **Glow:** a traced object's glow is its glow map x PTGlow (the playground's default 4: the panel is its only traced glow), as the
  path tracer reads it. A raster-only object's glow is its colour x `glow`.
- **Material kinds:** `o.kind = NVR_RT_KIND_GLASS / _FOLIAGE / _SKIN` (the record's kind bits; glass also goes in non-opaque, mask
  0x10) and `o.matClass` for the first hit's MaterialBuffer class (5 plants, 6 skin, 3 cloth).
- **Raster only** (`o.traced = false`): the RT scene lacks it and the game's frame shows it, like the lamp bulbs.
- **Lamps:** the `lamp(...)` helper in `BuildRoom` (position, reach, linear colour, an optional `animate`).
- **Viewpoints:** `s.views`.

### Adding a feature toggle (for features other teams merge)

A new path tracer feature is usually a new flag bit, a frame-row texel, or a setting. Three small edits:

1. **A field in `Settings`** (`playground.cpp`), with the game's default: `int emissiveLights = 1;`.
2. **A line in `toggles()`:** `{ 'Z', "Z", "PTEmissiveLights", &set.emissiveLights, 2 },`. The key, its HUD entry and the screenshot
   labels then come by themselves (pick a free key; `count` > 2 cycles through values).
3. **Its use in `TracePass()`.** Mirror what the team added to `RtPathTrace.cpp` Record:
   - a flag: `if (set.emissiveLights) flags |= 4194304u;`
   - a frame-row value: write it into `row[...]` after `RtRestirRow::Build`
   - a constant: the `PathConstants` field
   - a new pass: `host.AddPass(shNew, images, writeMask, &consts, size, gx, gy)`, with the shader registered in `Init()` beside
     `shPath` (`host.AddShader(spv + "\\RtNew.spv")`) and its name added to `$traceShaders` in `build.ps1`.

For a feature on the D3D9 side (a new denoiser pass), do two things. Load it with
`MakeFrag("RtNewPass", images, pushSize, targets, samplerOffset, pass)` in `Init()` (the same arguments as the game's
`RtDenoise.cpp` Make), and draw it with `DrawFrag(...)` in `Denoise()` or `ShowPass()`. Then add the `.spv` name to `$forkShaders`
in `build.ps1`.

When the game's struct changes (the `PathConstants` block, push blocks), change the copies at the top of `playground.cpp` too.
`tools/nvr-rt-host/rt_layout_check.py` checks the host's copies, not these ones.

### Files

- `playground.cpp`: the app (window, input, raster, the PTRealtime chain, HUD, PNG screenshots, `--demo`).
- `pg_host.h`: the game's side of the RT core: the device, the helper, shared images, the exchange. It is a copy of
  `tools/nvr-rt-host`'s Host with a real window, persistent CopyIn / CopyOut and full instance transforms. `rt_host.cpp` is unchanged.
- `pg_scene.h`: meshes, objects, lamps, textures, `BuildRoom()`.
- `build.ps1`, `run.ps1`: build and run; output goes to `bin\` and `out\` (both git-ignored).

### Limits

- The raster "game's frame" is a stand-in for the game's look: no shadow maps, normal maps, AO or fog. PTDetail therefore has little
  fine shading to add here, and PT off means that raster plus RT sun shadows: no RT reflections and no RT lamp shadows.
- No sky panorama (flag 16), normal atlas, light grid (fewer than 40 lamps), PTRestirGI, first-person body, people or skinned
  meshes, and no displacement.
- The smoothest roughness a traced (bounce) hit can carry is the record's 1/7. The first hit takes the G-buffer's 0.05, as in the game.
- The internal size is fixed at start (`-Size`): the shared images are made for it.
- The mouse is polled (GetAsyncKeyState / GetCursorPos), like the game's overlay under DXVK.

## testkit

*Original title: Test kit builders*

Python 3 scripts (no packages) that write the tester kit in `resource/testkit/Data`: the test plugins and the few models and textures of
our own that they use. They read the game's `FalloutNV.esm` (set `FNV_ESM` to its full path; read only) to find vanilla base forms by
editor ID; no vanilla record is overridden and no vanilla file is copied.

| script | writes |
|---|---|
| `build_rtroom.py --out <dir>` | `<dir>\Data\VS-RTRoom.esp` (cell VSRTRoom) + `Meshes\VegasShaders\Test\vs_sphere_*01.nif, vs_glass01.nif, vs_glowpanel01.nif, vs_windowwall01.nif` + their textures under `Textures\VegasShaders\Test\`, and `<dir>\rtroom-views.json` (the camera marks) |
| `build_alphalamp.py --out <dir>` | `VS-AlphaLampTest.esp` (cell VSAlphaLampTest; uses the kit's `vs_mirror01.nif`, which is a patched vanilla model and is not made here) |
| `build_testcell.py --out <dir>` | `NVR-TestCell.esp` (the bench's test cells; the others reuse its writer) |
| `verify_kit.py <Data dir>` | checks a kit: each `.esp` with `verify_esp.py`, each `.nif` read back block by block (sizes, strings, geometry, triangle facing), each `.dds` size |

`vs_nif.py` writes the models (FNV static `.nif` 20.2.0.7 / user 11 / Bethesda 34: BSFadeNode, NiTriShape, BSShaderPPLightingProperty,
BSShaderTextureSet, NiMaterialProperty, NiAlphaProperty; no collision) and uncompressed DDS textures. `esm.py` reads the master,
`tes4.py` writes records.

To rebuild the RT room into the kit:

```
set FNV_ESM=<game>\Data\FalloutNV.esm
py -3 -I tools\testkit\build_rtroom.py --out %TEMP%\rtroom
py -3 -I tools\testkit\verify_kit.py %TEMP%\rtroom\Data
```

then copy `%TEMP%\rtroom\Data\*` over `resource\testkit\Data`. The output is the same bytes on every run.

## Textures

*Original title: Textures*

The runtime loads these from `Data\Textures\` (shader `ResourceName` annotations, path relative to that folder).

### Generated by `tools/gen_textures.py`

These used to be New Vegas Reloaded art and are now made procedurally from fixed seeds. Each one keeps the
original file's size, mip count, pixel format and channel layout, so the shaders and samplers read it unchanged.

| File | Format | What it is | Read by |
|---|---|---|---|
| `Effects/dirtlens.png` | 2048x1365 RGB | soft bokeh discs and faint smears on black (red used) | Lens |
| `Effects/bloodlens.dds` | 1024 DXT5, 11 mips | A = clustered blood splatter; RG = warp offset (x2-1), 0.5 = none, refraction round the drops; B unused | BloodLens |
| `Effects/Flashlight.png` | 2048 RGBA (grey) | flashlight cookie: hotspot, broad band, thin ring, dark corners (red used, CLAMP) | Flashlight, FlashlightBeam |
| `Effects/water_NRM_lens.dds` | 256 A8R8G8B8, no mips | RG = streak/droplet normal xy, B = soft patch mask (refraction z), A = smooth noise (all x2-1) | WaterLens |
| `Effects/noise.dds` | 255 R8G8B8, no mips | white gaussian noise per channel | none at present |
| `Effects/bluenoise256.dds` | 256 A8R8G8B8, no mips | four independent void-and-cluster blue-noise channels, each value 0..255 exactly 256 times | AO, AmbientLight, BounceLight, Cinema, SunShadows, Snow, Underwater, clouds |
| `Water/water_NRM.dds` | 1024 R8G8B8, 11 mips | ocean-spectrum wave normal map | water shaders, Underwater |
| `Water/water_NRM_LOD.dds` | 4096x128 R8G8B8, no mips | 32 looping 128x128 frames: RG wave normal xy, B crest height | none at present |
| `Water/watercalm_NRM.dds` | 4096 DXT5, 13 mips | calm-water normal map (long swells, fine ripples) | WATER001, WATER018 |
| `Water/caust_001.dds` | 256 DXT1, 9 mips | caustics: light refracted through a tiling wave surface onto a floor (red used) | Underwater |
| `Precipitations/ripples.dds` | 256 A8R8G8B8, 9 mips | rain drops: R = 1 at centre to 0 at rim, GB = outward direction (x right, y up) as 0.5+0.5*dir, A = phase | water shaders (ComputeRipple) |
| `Precipitations/snow_NRM.dds` | 512 R8G8B8, no mips | snow grain + soft drift normal map | SnowAccumulation |
| `VegasShaders/LUTs/neutral_lut.png` | 256x16 RGB | exact identity LUT strip (16 slices of 16x16) | LUT (skipped when it is the identity) |

All tile seamlessly except `dirtlens.png`, `Flashlight.png` and the LUT, which span the screen / cone.

Regenerate (all, or some by name) into this folder:

    py -3.11 tools/gen_textures.py
    py -3.11 tools/gen_textures.py --only ripples,caust_001 --texconv C:\path\texconv.exe

Needs Python 3 with numpy and Pillow, and Microsoft's `texconv` (DirectXTex) for the DXT files (`--texconv`,
the `TEXCONV` variable, or on PATH). The output is the same on every run.

### Not generated

- `Effects/SMAA_AreaTex.dds`, `Effects/SMAA_SearchTex.dds`: the SMAA project's lookup tables (MIT), kept exact.
- Our own: `Effects/BloodDrops`, `BloodRuns`, `BloodSpatterSmall`, `CloudDetail`, `CloudShape`, `CloudWeather`,
  `NoiseVolume`, `Snownoise256`; `Precipitations/splash`, `splash_n`, `water-streaks_n`; `Water/water_d`.

### Contributed (origin confirmed 2026-11-09: made by the contributor for this project; may ship, credit him)

| File | Format | What it is | Read by |
|---|---|---|---|
| `Precipitations/fx_weather_snow01.dds` | 512 DXT5, 10 mips | 4x4 sheet of snowflake pictures (alpha = shape). Sent by macintroll with his world-space snow on 2026-11-09. The file name looks like a stock game asset: confirm with him where it comes from before any release carries it | Snow (`[Shaders.Snow.Main] WorldSpace`) |

## GunFX

*Original title: GunFX assets*

GunFX used to be a plugin of its own (`GunFX.dll`); since 2026-11-16 it is built into Vegas Shaders: the source is
in [`src/gunfx/`](../src/gunfx/) and its settings are in Vegas Shaders' own settings (`[Main.GunFX.*]`). The player
guide is `docs/GUNFX.md` (not in this repo); the move is described in
`docs/next/GUNFX-BUILTIN.md` (not in this repo).

What is left here:

- `assets/Meshes/GunFX/`: the baked smoke effects, shipped as `Data\Meshes\GunFX\`. They are vanilla
  `effects\fxlightsmoke.nif` rebuilt with different numbers. GunFX only sets their emitter rate at run time
  (the puff and port bursts start when spawned and stop after `BurstSeconds`); it does not rewrite particle modifiers.

The old `GunFX.vcxproj` and `config/GunFX.ini.defaults` were removed (git history has them; `src/gunfx/GunFXKeys.h`
keeps every key with its shipped default).
