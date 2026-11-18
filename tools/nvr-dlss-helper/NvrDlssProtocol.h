// NvrDlssProtocol.h -- the shared-memory block between New Vegas Reloaded (32-bit, inside the game) and NvrDlssHelper.exe
// (64-bit). MIT licence (tools/nvr-dlss-helper/LICENSE), so both sides may include it. No NVIDIA code or types in here.
//
// How a frame goes (docs/next/FORK2-DLSS.md has the long version):
//   NVR (render thread)                                      helper (its own process, same GPU, own Vulkan device)
//   CopyToShared colour/depth/motion (fork, GPU)
//   frames[N % NVR_DLSS_FRAME_RING] = this frame's values
//   latestFrame = N, SetEvent(frame event)          ----->   wakes up, reads frames[N % ring]
//   SignalShared(inputsReady, N)  (fork: flush)               records DLSS (or the pass-through copy) and submits it:
//   WaitShared(outputReady, N)    (GPU wait only)              GPU waits inputsReady >= N, signals outputReady = N
//   CopyFromShared output -> NVR's texture (GPU)
// Nobody waits on the CPU per frame. Fixed-size fields only (the two sides differ in pointer size); 8-byte alignment kept
// by hand so both compilers lay the block out the same way (static_asserts below).
//
// UNOFFICIAL 2026-10-30 protocol 2 (DLSS Ray Reconstruction, docs/next/FORK2-DLSS-RR.md): four more images (the RR guides), made only
// when NVR wants RR (features bit 0; imageCount 8, else 4); a frame with NVR_DLSS_FRAME_RR is evaluated by DLSS-D with the guides and
// the frame's matrices instead of DLSS SR; DLSS SR's preset per frame; the helper reports rrAvailable separately (no RR never ends SR).
//
// UNOFFICIAL 2026-11-01 protocol 3 (player report: with DLSS the first-person arms and gun ghost and smear while they animate): one more
// image, DLSS SR's "bias current colour" mask (NGX DLSS.Input.Bias.Current.Color.Mask), r16f at the inputs' size, render size in the
// top-left: 1 on the first-person weapon ([Shaders.TAA.DLSS] WeaponMask), FSR's reactive value elsewhere. It has its own descriptor
// (biasMask, after the image table), exists with NVR_DLSS_FEATURE_BIAS_MASK, and DLSS SR gets it on frames flagged NVR_DLSS_FRAME_BIAS_MASK.
// Protocol 2 helpers refuse a protocol 3 block (version and size): NVR then runs FSR 2, logged; the two ship together.

#pragma once

#include <stdint.h>

#define NVR_DLSS_MAGIC            0x53534C44u   // 'DLSS'
#define NVR_DLSS_PROTOCOL_VERSION 3u   // 2026-10-30 (2): RR guides, presets, matrices; 2026-11-01 (3): the bias current colour mask
#define NVR_DLSS_FRAME_RING       16u           // per-frame values kept; NVR never runs more than a few frames ahead

// File mapping and event names: <base> is passed to the helper as "--shm <base>"; the events are <base>-frame (auto-reset,
// NVR -> helper: a new frame) and <base>-status (auto-reset, helper -> NVR: state or message changed).
#define NVR_DLSS_EVENT_FRAME_SUFFIX  L"-frame"
#define NVR_DLSS_EVENT_STATUS_SUFFIX L"-status"

enum NvrDlssMode : uint32_t {
	NVR_DLSS_MODE_DLSS        = 0,   // NVIDIA DLSS (needs an RTX GPU and nvngx_dlss.dll next to the helper)
	NVR_DLSS_MODE_PASSTHROUGH = 1,   // test: the colour input stretched to the output plus a frame marker (any NVIDIA GPU)
};

enum NvrDlssState : uint32_t {
	NVR_DLSS_STATE_STARTING = 0,
	NVR_DLSS_STATE_READY    = 1,     // images and semaphores imported; frames are taken
	NVR_DLSS_STATE_FAILED   = 2,     // see message; the helper exits
	NVR_DLSS_STATE_EXITED   = 3,
};

enum NvrDlssImage : uint32_t {
	NVR_DLSS_IMAGE_COLOR  = 0,       // render size in the top-left of an input-size image (RR frames: linear HDR)
	NVR_DLSS_IMAGE_DEPTH  = 1,       // r: device depth 0 near .. 1 far (not inverted)
	NVR_DLSS_IMAGE_MOTION = 2,       // rg: previous uv - current uv, render size, unjittered
	NVR_DLSS_IMAGE_OUTPUT = 3,       // display size, written whole by the helper every frame
	// UNOFFICIAL 2026-10-30 protocol 2: the RR guides (input size, render size in the top-left), only with NVR_DLSS_FEATURE_RR
	NVR_DLSS_IMAGE_DIFFUSE_ALBEDO  = 4,   // rgba16f: rgb diffuse albedo
	NVR_DLSS_IMAGE_SPECULAR_ALBEDO = 5,   // rgba16f: rgb specular albedo (integrated reflectance)
	NVR_DLSS_IMAGE_NORMALS         = 6,   // rgba16f: xyz unit normal (camera-relative world space, view space with NVR_DLSS_RR_NORMALS_VIEW)
	NVR_DLSS_IMAGE_ROUGHNESS       = 7,   // r16f: linear roughness
	NVR_DLSS_IMAGE_COUNT_SR = 4,     // the images of a helper start without RR (imageCount)
	NVR_DLSS_IMAGE_COUNT  = 8,       // the table's size; imageCount says how many were made (4 or 8). Inputs: every index but OUTPUT
	// UNOFFICIAL 2026-11-01 protocol 3: not in the table (NvrDlssShared::biasMask), only with NVR_DLSS_FEATURE_BIAS_MASK
	NVR_DLSS_IMAGE_BIAS_MASK = 8,    // r16f: DLSS SR's bias current colour mask [0, 1] (1 = trust this frame's colour: the weapon)
};

// NvrDlssShared::features (NVR, setup)
#define NVR_DLSS_FEATURE_RR       1u     // the RR images exist and NVR may send RR frames: the helper checks RR at start
#define NVR_DLSS_FEATURE_BIAS_MASK 2u    // UNOFFICIAL 2026-11-01 protocol 3: NvrDlssShared::biasMask exists
// NvrDlssFrame::flags (NVR, per frame)
#define NVR_DLSS_FRAME_RR         1u     // evaluate DLSS-D (Ray Reconstruction) with the guides and the matrices, not DLSS SR
#define NVR_DLSS_FRAME_BIAS_MASK  2u     // UNOFFICIAL 2026-11-01 protocol 3: this frame's bias mask is written: DLSS SR gets it (else no mask)
// NvrDlssFrame::rrOptions (NVR, per frame; unverified NGX conventions a tester can flip, docs/next/FORK2-DLSS-RR.md)
#define NVR_DLSS_RR_NORMALS_VIEW  1u     // the normals are in view space (else camera-relative world space)
#define NVR_DLSS_RR_DEPTH_LINEAR  2u     // tell DLSS-D the depth is not hardware depth (DLSS.Use.HW.Depth 0; else 1)

// NvrDlssFrame::preset: DLSS SR's model, NVR's own numbers ([Shaders.TAA.DLSS] Preset); the helper maps them to NGX's preset
// letters per quality mode and re-creates the feature when it changes.
enum NvrDlssPreset : uint32_t {
	NVR_DLSS_PRESET_DEFAULT       = 0,   // no hint: the DLL's own choice (310.9.16: transformer K / M / L)
	NVR_DLSS_PRESET_CNN           = 1,   // E; F at DLAA and Ultra Performance (NVIDIA's last CNN defaults)
	NVR_DLSS_PRESET_TRANSFORMER   = 2,   // K at DLAA / Quality / Balanced, M at Performance, L at Ultra Performance
	NVR_DLSS_PRESET_TRANSFORMER_J = 3,   // J everywhere
	NVR_DLSS_PRESET_COUNT         = 4,
};
// NvrDlssFrame::rrPreset ([Shaders.TAA.DLSS] RRPreset)
// UNOFFICIAL 2026-11-15 (docs/next/DLSS-RR-MODEL.md): F (NVIDIA's second-generation RR transformer, "DLSS 4.5 Ray Reconstruction",
// already inside the nvngx_dlssd.dll 310.9.16 that ships; needs driver 580.00 or newer) and AUTO (the helper picks F on RTX 40 / 50
// class GPUs and E on RTX 20 / 30). No protocol change: a helper from before reads 3 and 4 as out of range, i.e. as DEFAULT (no hint).
enum NvrDlssRrPreset : uint32_t {
	NVR_DLSS_RR_PRESET_DEFAULT = 0,      // no hint
	NVR_DLSS_RR_PRESET_D       = 1,      // NGX's RR preset D (4)
	NVR_DLSS_RR_PRESET_E       = 2,      // NGX's RR preset E (5): the first-generation transformer, "the current model"
	NVR_DLSS_RR_PRESET_F       = 3,      // NGX's RR preset F (6): the second-generation transformer, "the newer model"
	NVR_DLSS_RR_PRESET_AUTO    = 4,      // the helper decides by GPU generation and driver: F or E (its log says which and why)
	NVR_DLSS_RR_PRESET_COUNT   = 5,
};

// What the fork's ID3D9NvrExtSharedImage::GetInfo reported, plus the NT handle duplicated into the helper's process.
struct NvrDlssImageDesc {
	uint64_t	handle;              // HANDLE value valid in the helper's process (the helper closes it after importing)
	uint64_t	allocationSize;
	uint32_t	memoryTypeIndex;
	uint32_t	vkFormat;
	uint32_t	width, height;
	uint32_t	usage;
	uint32_t	flags;
	uint32_t	tiling;
	uint32_t	layout;              // the layout the image is in between uses (GENERAL)
	uint32_t	handleType;
	uint32_t	dedicated;
};

struct NvrDlssFrame {
	uint64_t	frame;               // N: the helper waits for inputsReady >= N and signals outputReady = N
	int64_t		writtenQpc;          // QueryPerformanceCounter when NVR wrote this (the same clock in both processes)
	uint32_t	renderWidth, renderHeight;
	uint32_t	reset;               // no usable history
	uint32_t	hdr;                 // colour is linear HDR (before tonemapping)
	float		jitterX, jitterY;    // sub-pixel offset in render pixels, DLSS's convention (NVR converts)
	float		mvScaleX, mvScaleY;  // motion texel * scale = render pixels
	float		preExposure;         // 1
	float		frameTimeMs;
	uint32_t	quality;             // NVSDK_NGX_PerfQuality_Value number (NVR picks it from render / display size)
	uint32_t	flags;               // NVR_DLSS_FRAME_* (protocol 2)
	// ---- protocol 2 (2026-10-30)
	uint32_t	preset;              // NvrDlssPreset (DLSS SR)
	uint32_t	rrPreset;            // NvrDlssRrPreset (RR frames)
	uint32_t	rrOptions;           // NVR_DLSS_RR_* (RR frames)
	uint32_t	pad;
	float		worldToView[16];     // RR frames: camera-relative view (rotation only), D3DX row-vector layout (row-major, v * M)
	float		viewToClip[16];      // RR frames: the unjittered projection, the same layout
};

struct NvrDlssShared {
	// ---- written by NVR before it starts the helper, read-only afterwards
	uint32_t	magic;
	uint32_t	version;
	uint32_t	size;                // sizeof(NvrDlssShared)
	uint32_t	mode;                // NvrDlssMode
	uint32_t	parentPid;
	uint32_t	vendorId;            // of DXVK's Vulkan device
	uint8_t		deviceLuid[8];
	uint32_t	deviceLuidValid;
	uint32_t	imageCount;          // images made: NVR_DLSS_IMAGE_COUNT_SR, or NVR_DLSS_IMAGE_COUNT with NVR_DLSS_FEATURE_RR (protocol 2)
	uint8_t		deviceUuid[16];
	uint8_t		driverUuid[16];
	NvrDlssImageDesc images[NVR_DLSS_IMAGE_COUNT];
	NvrDlssImageDesc biasMask;       // UNOFFICIAL 2026-11-01 protocol 3: NVR_DLSS_IMAGE_BIAS_MASK, with NVR_DLSS_FEATURE_BIAS_MASK
	uint64_t	semInputsReady;      // HANDLE values valid in the helper's process (timeline semaphores, OPAQUE_WIN32)
	uint64_t	semOutputReady;
	uint32_t	displayWidth, displayHeight;
	uint32_t	inputWidth, inputHeight;
	uint32_t	setupDone;           // 1 once everything above is filled in
	uint32_t	features;            // NVR_DLSS_FEATURE_* (protocol 2)

	// ---- per frame, written by NVR (latestFrame last, with a release barrier)
	volatile int64_t	latestFrame;
	NvrDlssFrame		frames[NVR_DLSS_FRAME_RING];

	// ---- written by the helper
	volatile uint32_t	state;               // NvrDlssState
	volatile uint32_t	ngxAvailable;        // 1: DLSS can run on this GPU (0 also when built without NVIDIA's SDK)
	volatile int32_t	ngxResult;           // the last NGX result code (0 = none)
	volatile uint32_t	messageSerial;       // incremented with every new message
	volatile int64_t	submittedFrame;      // the last frame the helper submitted
	volatile uint64_t	framesSubmitted;
	volatile uint32_t	gpuTimeUsLast;       // the helper's own GPU work per frame (timestamps), microseconds
	volatile uint32_t	gpuTimeUsAvg;        // average of the last 64
	volatile uint32_t	responseUsAvg;       // NVR's write -> the helper's submit, CPU, average of the last 64
	volatile uint32_t	responseUsMax;       // worst since the last report
	volatile uint32_t	paramMismatches;     // frames whose values were overwritten before the helper read them
	volatile uint32_t	dlssEvaluated;       // frames DLSS itself ran (0 in pass-through)
	// ---- protocol 2 (2026-10-30), written by the helper
	volatile uint32_t	rrAvailable;         // 1: RR frames are evaluated (DLSS-D, or the pass-through test); 0: NVR sends none (SR goes on)
	volatile uint32_t	rrEvaluated;         // RR frames the helper evaluated (pass-through ones included)
	volatile int32_t	rrResult;            // the last DLSS-D NGX result code (0 = none)
	volatile uint32_t	biasMaskFrames;      // UNOFFICIAL 2026-11-01 protocol 3: frames whose bias mask the helper used (pass-through: imported and copied)
	char				message[512];        // the last status line, NUL-terminated (the helper's log has them all)

	// ---- written by NVR
	volatile uint32_t	quit;                // 1: exit
	uint32_t			pad2;
};

#ifdef __cplusplus
static_assert(sizeof(NvrDlssImageDesc) == 56, "NvrDlssImageDesc layout");
static_assert(sizeof(NvrDlssFrame) == 208, "NvrDlssFrame layout");
static_assert(sizeof(NvrDlssShared) == 4544, "NvrDlssShared layout (the same in the 32-bit and the 64-bit build)");
#endif
