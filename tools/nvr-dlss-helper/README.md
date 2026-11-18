# NvrDlssHelper (tools/nvr-dlss-helper)

The 64-bit helper program behind New Vegas Reloaded's DLSS upscaler (`[Shaders.TAA.DLSS] Enabled = true`; before 2026-10-27 `[Shaders.TAA.FSR] Upscaler = 1`). Fallout New Vegas,
NVR and the NVR DXVK `d3d9.dll` are 32-bit; NVIDIA ships DLSS (NGX, `nvngx_dlss.dll`) for 64-bit Windows only, so DLSS runs
in this separate program, on the same GPU, with its own Vulkan device. NVR starts it, shares four GPU images (eight with Ray
Reconstruction) and two GPU timeline semaphores with it through the NVR DXVK fork (patch 1.7-dlss, `ID3D9NvrExtShare`) and a
small shared-memory block (`NvrDlssProtocol.h`, protocol 3), and it exits with the game. Design: `docs/next/FORK2-DLSS.md`;
Ray Reconstruction: `docs/next/FORK2-DLSS-RR.md`.

Licence: this folder is MIT (`LICENSE`). NVIDIA's SDK parts are NVIDIA's (`NVIDIA-SDK-NOTICE.txt`). Nothing from NVIDIA's SDK
is in this repository, and none of it is in NewVegasReloaded.dll.

## Getting NVIDIA's DLSS SDK

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

## Building

Visual Studio 2019 (v142) x64 and the Vulkan SDK headers + `vulkan-1.lib` (`%VULKAN_SDK%`, else `C:\VulkanSDK\1.3.296.0`):

    msbuild tools\nvr-dlss-helper\NvrDlssHelper.vcxproj /p:Configuration=Release /p:Platform=x64 ^
      /p:DlssSdkDir=<SDK folder> /p:OutDir=<out>\ /p:IntDir=<obj>\

`DlssSdkDir` defaults to `%NVR_DLSS_SDK%`, then `tools\nvr-dlss-helper\dlss-sdk`. Without the SDK it still builds, for the
pass-through test only (`NVR_DLSS_NO_SDK`). `/p:DlssDevDll=true` copies NVIDIA's development `nvngx_dlss.dll` (its jitter and
motion-vector overlays help to check NVR's inputs). The release `nvngx_dlss.dll` is copied next to the exe, and
`nvngx_dlssd.dll` too when the SDK folder has it (the build says which).

## Installing (by hand)

Either next to the game's `FalloutNV.exe` / `d3d9.dll`, or (tidier) in its own folder:

    <game>\Data\NVSE\Plugins\NvrDlss\NvrDlssHelper.exe
    <game>\Data\NVSE\Plugins\NvrDlss\nvngx_dlss.dll        (DLSS only; the pass-through test does not need it)
    <game>\Data\NVSE\Plugins\NvrDlss\nvngx_dlssd.dll       (Ray Reconstruction only; optional)
    <game>\Data\NVSE\Plugins\NvrDlss\LICENSE.txt           (NVIDIA's, from the SDK, when shipping nvngx_dlss.dll / nvngx_dlssd.dll)

Not straight into `Data\NVSE\Plugins` (xNVSE tries to load every DLL there as a plugin). It needs the NVR DXVK `d3d9.dll`
built with patch 0026 (1.7-dlss) and an NVIDIA GPU. The helper writes `NvrDlssHelper.log` next to itself (or into %TEMP%).

## DLSS presets (`[Shaders.TAA.DLSS] Preset`)

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

## The weapon's bias mask (protocol 3, `[Shaders.TAA.DLSS] WeaponMask`, 2026-11-01)

Player report: with DLSS the first-person arms and gun ghosted and smeared while they animated. The weapon is fixed to the camera,
so its motion vectors are 0 and its own sway, bob, recoil and reload were in DLSS's history only. NVR now makes one more shared
image (r16f, `NvrDlssShared::biasMask`, feature bit `NVR_DLSS_FEATURE_BIAS_MASK`): WeaponMask on the weapon, FSR's reactive value
elsewhere (`src/hlsl/NewVegas/Compute/DlssBiasMask.comp`). The helper gives it to DLSS SR as NGX's
`DLSS.Input.Bias.Current.Color.Mask` on frames flagged `NVR_DLSS_FRAME_BIAS_MASK` (WeaponMask above 0); with `--inputs exact` it
is copied at the render size like the other inputs. RR frames do not use it. A protocol 2 helper refuses the block (NVR runs
FSR 2, logged). Every self-test exports the mask too and checks `biasMaskFrames` (all 300 frames in the pass-through, none with
`rr`, every DLSS SR frame with `dlss`).

## Ray Reconstruction (protocol 2, `[Shaders.TAA.DLSS] RayReconstruction`)

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

2026-11-15 (`docs/next/DLSS-RR-MODEL.md`): `RRPreset` 3 = preset F, NVIDIA's second-generation RR transformer ("DLSS 4.5 Ray
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

## By hand

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

## Start options (from NVR's settings)

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
