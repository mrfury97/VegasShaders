// NvrRtProtocol.h -- the shared-memory block between New Vegas Reloaded (32-bit, inside the game) and NvrRtHelper.exe
// (64-bit), the process that owns NVR's ray tracing scene. MIT licence (tools/nvr-rt-helper/LICENSE), so both sides may
// include it. docs/next/FORK-RT-CORE.md has the whole design and the rules for writing a trace pass.
//
// Why a helper: Fallout New Vegas, NVR and the NVR DXVK d3d9.dll are 32-bit, and Windows' 32-bit Vulkan drivers offer no
// ray tracing (measured 2026-10-28: AMD 25.10 and NVIDIA 582 list no VK_KHR_acceleration_structure / ray_query to a 32-bit
// process; their 64-bit drivers do). The helper makes its own Vulkan device on the same GPU, imports images and two
// timeline semaphores the fork exports (patch 0026, ID3D9NvrExtShare), and per frame:
//
//   NVR (render thread)                                      helper (its own process, same GPU)
//   StageGeometry ... FlushStagedGeometry (fork, GPU)
//   CopyToShared the passes' inputs (fork, GPU)
//   frames[N % RING] = ops, instances, passes; latestFrame = N, SetEvent  ----->  wakes up, copies frames[N % RING] out,
//   SignalShared(inputsReady, N)  (fork: flush)                              sets consumedFrame = N, records and submits:
//   WaitShared(outputReady, N)    (GPU wait only)                            GPU waits inputsReady >= N; arena -> its own
//   CopyFromShared the passes' outputs (fork, GPU)                           buffer; new geometry built; the scene (TLAS)
//                                                                            rebuilt from the instances; the passes run;
//                                                                            signals outputReady = N
// Nobody waits on the CPU per frame. NVR only waits (briefly, with a timeout) before it would overwrite a ring slot the
// helper has not read yet (consumedFrame), since geometry operations must never be lost. Fixed-size fields only (the two
// sides differ in pointer size); 8-byte alignment kept by hand (static_asserts below).

#pragma once

#include <stdint.h>

#define NVR_RT_MAGIC            0x54525256u   // 'VRRT'
#define NVR_RT_PROTOCOL_VERSION 6u            // 2 (2026-10-28): instance records + the geometry data (bindings 10, 11); 3 (2026-10-29): 32 shared images; 4 (2026-10-30): 32 shaders; 5 (2026-11-02): 48 shared images; 6 (2026-11-07): the scratch store (binding 20, NvrRtShared::scratchMB)
#define NVR_RT_FRAME_RING       4u            // frame records; NVR waits for consumedFrame before reusing one
#define NVR_RT_MAX_IMAGES       48u           // shared images, registered at setup (index 0 is the geometry arena); 16 was too few with all four RT effects (~20), 32 with the reference path tracer (31: every user's Setup registers, on or off; 2026-11-02)
#define NVR_RT_MAX_SHADERS      32u           // trace shaders (.spv paths), registered any time (16 until 2026-10-30: the four RT strands used 15)
#define NVR_RT_MAX_OPS          2048u         // geometry operations per frame
#define NVR_RT_MAX_INSTANCES    32768u        // scene instances per frame
#define NVR_RT_MAX_PASSES       8u            // trace passes per frame
#define NVR_RT_PASS_IMAGES      8u            // images bound to one pass (bindings 2..9) in NvrRtPass::images
// UNOFFICIAL 2026-10-30 (rt-ao-sky: GI merged into the AO pass): up to 4 more images (8..11) at bindings 12..15, their indices as
// bytes in NvrRtPass::extraImages (the old pad word: same layout, protocol 3). A helper older than this skips a pass with more than 8.
// UNOFFICIAL 2026-11-02 (protocol 5, the reference path tracer): 16 images, 12..15 at bindings 16..19 (NvrRtPass::extraImages2).
#define NVR_RT_PASS_IMAGES_MAX  16u
#define NVR_RT_PASS_EXTRA_BINDING 12u          // binding of image 8 (9..15 follow: 13..19)
#define NVR_RT_PASS_CONSTANTS   512u          // bytes of a pass's uniform block (binding 1)
#define NVR_RT_ARENA_IMAGE      0u
#define NVR_RT_BINDING_RECORDS  10u           // readonly std430 SSBO: NvrRtInstanceRecord per instance, by rayQueryGetIntersectionInstanceIdEXT
#define NVR_RT_BINDING_GEOMETRY 11u           // readonly std430 SSBO: uint words; a record's vertexOffset / indexOffset point into it
#define NVR_RT_NO_DATA          0xFFFFFFFFu   // vertexOffset / indexOffset when the helper had no room for the geometry's data
// UNOFFICIAL 2026-11-07 (protocol 6) the scratch store: a std430 buffer of uint words the HELPER owns (never shared with the game), bound to
// every pass at binding 20, zero when the helper starts and kept from frame to frame: state a pass keeps for itself (the path tracer's
// ReSTIR DI reservoirs and radiance cache, RtPathTrace.comp). NvrRtShared::scratchMB megabytes (at least 1). Who uses which words is the
// passes' business (today only RtPathTrace.comp: docs/next/RT-PATH-TRACING.md section 47 has the layout).
#define NVR_RT_BINDING_SCRATCH  20u

// NvrRtInstanceRecord::flags (NVR sets them; bits 8.. are free for the effects)
#define NVR_RT_RECORD_LAND          0x01u
#define NVR_RT_RECORD_WATER         0x02u
#define NVR_RT_RECORD_GLOW          0x04u     // the draw has a glow / emissive map
#define NVR_RT_RECORD_METAL         0x08u
#define NVR_RT_RECORD_ALBEDO_KNOWN  0x10u     // albedo is the diffuse texture's average (else a neutral grey)
// UNOFFICIAL 2026-10-29 alpha-tested casters ([Main.Main.RayTracing] AlphaTested): the geometry is non-opaque (cull mask bit
// NVR_RT_MASK_ALPHA), its texcoords follow the triangle list in binding 11 at the word offset floatBitsToUint(albedo.w), its
// diffuse alpha is the alpha atlas' slot (flags >> 16) (RtCore::AlphaAtlasImage(): R16F 2048x2048, 64x64 texels a slot, 32 a row), and
// a candidate hit counts where alpha >= ((flags >> 8) & 0xFF) / 255. src/hlsl/NewVegas/Compute/Rt/RtAlpha.glsl does it.
#define NVR_RT_RECORD_ALPHA_TESTED  0x20u
// UNOFFICIAL 2026-10-29 hit textures ([Main.Main.RayTracing] HitTextures): the geometry's texcoords are in binding 11 at
// floatBitsToUint(albedo.w) (as alpha-tested ones) and its diffuse colour is the albedo atlas' slot (flags >> 16) (RGBA8, the same
// slots as the alpha atlas). src/hlsl/NewVegas/Compute/Rt/RtHit.glsl reads it.
#define NVR_RT_RECORD_TEXTURED      0x40u
// UNOFFICIAL 2026-11-02 normal textures ([Main.Main.RayTracing] NormalTextures, with HitTextures): the draw's normal map (tangent space,
// as stored) is in the normal atlas at the same slot (RGBA8; a slot filled before the map was known reads 0: no map). RtHit.glsl rtHitNormalMapped.
#define NVR_RT_RECORD_NORMAL        0x80u
// UNOFFICIAL 2026-11-03 the material rule (VegasShaders.materials.txt, the object shaders' table) in bits 26..31 of a textured /
// alpha-tested record: bits 26-28 roughness (0 unknown, k = 1..7: k / 7), bits 29-31 metal (0 unknown, k = 1..7: (k - 1) / 6)
#define NVR_RT_RECORD_ROUGH_SHIFT   26u
#define NVR_RT_RECORD_METAL_SHIFT   29u
#define NVR_RT_MASK_ACTOR           0x20u     // UNOFFICIAL 2026-10-29: skinned actors ([Main.Main.RayTracing] Actors), posed every frame
#define NVR_RT_MASK_ALPHA           0x10u     // the instance mask of alpha-tested geometry (rays that cannot test alpha leave it out)
#define NVR_RT_MASK_PLAYER          0x40u     // UNOFFICIAL 2026-10-30: the player's own rigid parts with Actors on (the third-person body's
                                              // held and holstered weapons, rigid armour): rays leaving from the player's gun leave it out
#define NVR_RT_MASK_FAR             0x80u     // UNOFFICIAL 2026-10-31: far object LOD past SceneRadius ([Main.Main.RayTracing] FarObjectLOD,
                                              // rt-reflections: the skyline in reflections); a ray that should not see it drops 0x80
// UNOFFICIAL 2026-11-07 NvrRtInstance::customIndex bits (no trace shader read the custom index before): bit 0 = land (since 2026-10-28),
// bit 1 = hidden: the player's own body and gear in first person, traced (shadows, reflections) but not drawn; camera rays pass over it
// (RtLampPrimary.comp, SameFrame lamp shadows)
#define NVR_RT_CUSTOM_LAND          0x1u
#define NVR_RT_CUSTOM_HIDDEN        0x2u
#define NVR_RT_ALPHA_SLOT_SIZE      64u
#define NVR_RT_ALPHA_ATLAS_SLOTS    32u       // a row (and rows): 2048 x 2048 texels, 1024 slots

// <base> is passed as "--shm <base>"; events <base>-frame (auto-reset, NVR -> helper) and <base>-status (helper -> NVR).
#define NVR_RT_EVENT_FRAME_SUFFIX  L"-frame"
#define NVR_RT_EVENT_STATUS_SUFFIX L"-status"

enum NvrRtState : uint32_t {
	NVR_RT_STATE_STARTING = 0,
	NVR_RT_STATE_READY    = 1,       // images and semaphores imported, ray tracing available: frames are taken
	NVR_RT_STATE_FAILED   = 2,       // see message (no RT on this GPU, import failed, ...); the helper exits
	NVR_RT_STATE_EXITED   = 3,
};

enum NvrRtOp : uint32_t {
	NVR_RT_OP_BUILD   = 1,           // geometry `id` from the arena: 3 x vertexCount floats at arenaTexel, then 3 x triangleCount uints
	NVR_RT_OP_RELEASE = 2,           // geometry `id` is gone (the helper frees it once no frame in flight uses it)
};

// What the fork's ID3D9NvrExtSharedImage::GetInfo reported, plus the NT handle duplicated into the helper's process.
// (The same layout as NvrDlssImageDesc.)
struct NvrRtImageDesc {
	uint64_t	handle;              // HANDLE value valid in the helper's process (the helper closes it after importing)
	uint64_t	allocationSize;
	uint32_t	memoryTypeIndex;
	uint32_t	vkFormat;
	uint32_t	width, height;
	uint32_t	usage;
	uint32_t	flags;
	uint32_t	tiling;
	uint32_t	layout;              // GENERAL between uses
	uint32_t	handleType;
	uint32_t	dedicated;
};

#define NVR_RT_OP_FLAG_TEXCOORDS  0x1u   // 2 x vertexCount more texels: the texcoords after the triangle list (fork 1.8-rt-uv)
#define NVR_RT_OP_FLAG_ALPHA      0x2u   // built non-opaque (alpha-tested: the rays test the candidates)
#define NVR_RT_OP_FLAG_DYNAMIC    0x4u   // rebuilt every frame (skinned actors): fast build instead of fast trace
// UNOFFICIAL 2026-10-30 ([Main.Main.RayTracing] Compact): static geometry built with ALLOW_COMPACTION; the helper reads the compacted
// size once the build's frame is done and copies it into a structure of that size a frame later (typically half the memory). An older
// helper ignores the flag (no compaction, nothing else changes).
#define NVR_RT_OP_FLAG_COMPACT    0x8u
// UNOFFICIAL 2026-10-31 ([Main.Main.RayTracing] ActorRebuildInterval): a DYNAMIC build the helper must build afresh instead of refitting
// (a refit's tree degrades as the pose moves away from the one it was built in). An older helper ignores it and refits (protocol stays 4).
#define NVR_RT_OP_FLAG_REBUILD    0x10u
#define NVR_RT_ARENA_SEQUENCE     0x80000000u   // NvrRtFrame::arenaTexels: a sequence texel follows the geometry (2026-10-30)
inline uint32_t NvrRtSequenceOf(uint64_t frame) { return (uint32_t)frame ? (uint32_t)frame : 1u; }

struct NvrRtGeometryOp {             // 32 bytes
	uint32_t	op;                  // NvrRtOp
	uint32_t	id;                  // NVR's geometry id, 1 .. 2^24 - 1 (0 = none)
	uint32_t	vertexCount;
	uint32_t	triangleCount;
	uint32_t	arenaTexel;          // first 32-bit texel in the arena (row-major)
	uint32_t	flags;               // NVR_RT_OP_FLAG_*
	uint32_t	pad[2];
};

struct NvrRtInstance {               // 64 bytes
	float		transform[3][4];     // object -> scene (camera-relative world space): row-major 3x4, as VkTransformMatrixKHR
	uint32_t	geometryId;          // a built geometry (0 or unknown: skipped)
	uint32_t	customIndex;         // low 24 bits: rayQueryGetIntersectionInstanceCustomIndexEXT
	uint32_t	mask;                // low 8 bits: ANDed with the ray's cull mask
	uint32_t	flags;               // VkGeometryInstanceFlagBitsKHR (1 = cull disable, 2 = flip facing, 4 = force opaque)
};

// One per instance, in the instance list's order (the helper keeps the pairs together when it skips an instance). In the shader:
//   struct Record { vec4 albedo; uint flags; uint geometryId; uint vertexOffset; uint indexOffset; };
//   layout(set = 0, binding = 10, std430) readonly buffer Records { Record records[]; };
//   layout(set = 0, binding = 11, std430) readonly buffer Geometry { uint words[]; };
//   Record r = records[rayQueryGetIntersectionInstanceIdEXT(q, true)];
//   uint p = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
//   uvec3 i = uvec3(words[r.indexOffset + 3 * p], words[r.indexOffset + 3 * p + 1], words[r.indexOffset + 3 * p + 2]);
//   vec3 v0 = vec3(uintBitsToFloat(words[r.vertexOffset + 3 * i.x]), ...);   // object space: rayQueryGetIntersectionObjectToWorldEXT
struct NvrRtInstanceRecord {         // 32 bytes
	float		albedo[4];           // linear rgb of the draw's diffuse texture (its smallest mip), a = 1
	uint32_t	flags;               // NVR_RT_RECORD_*
	uint32_t	geometryId;          // = the instance's
	uint32_t	vertexOffset;        // filled by the helper: word offset of the geometry's float3 positions in binding 11
	uint32_t	indexOffset;         // filled by the helper: word offset of its uint triangle list (indices from its vertex 0)
};

struct NvrRtPass {                   // 580 bytes (protocol 5)
	uint32_t	shader;              // index into shaderPaths
	uint32_t	groupsX, groupsY, groupsZ;
	uint32_t	imageCount;          // images[0 .. imageCount-1] bound at bindings 2 .. (storage images, GENERAL)
	uint32_t	images[NVR_RT_PASS_IMAGES];   // indices into images[] of the shared block
	uint32_t	writeMask;           // bit i: the pass writes images[i] (a barrier after the pass for those)
	uint32_t	constantsSize;
	uint32_t	extraImages;         // images 8..11: byte i - 8 (imageCount > 8 only; was pad)
	uint32_t	extraImages2;        // images 12..15: byte i - 12 (protocol 5)
	uint8_t		constants[NVR_RT_PASS_CONSTANTS];
};

inline uint32_t NvrRtPassImage(const NvrRtPass& p, uint32_t i) { return i < NVR_RT_PASS_IMAGES ? p.images[i] : i < NVR_RT_PASS_IMAGES + 4u ?
	(p.extraImages >> (8u * (i - NVR_RT_PASS_IMAGES))) & 0xFFu : (p.extraImages2 >> (8u * (i - NVR_RT_PASS_IMAGES - 4u))) & 0xFFu; }
inline uint32_t NvrRtPassBinding(uint32_t i) { return i < NVR_RT_PASS_IMAGES ? 2u + i : NVR_RT_PASS_EXTRA_BINDING + (i - NVR_RT_PASS_IMAGES); }

struct NvrRtFrame {
	uint64_t	frame;               // N
	int64_t		writtenQpc;          // QueryPerformanceCounter when NVR wrote this (the same clock in both processes)
	uint32_t	opCount;
	uint32_t	instanceCount;
	uint32_t	passCount;
	uint32_t	arenaTexels;         // texels of the arena this frame's builds read (0 = none); bit 31 (NVR_RT_ARENA_SEQUENCE): the texel
	                                 // right after them holds (uint32_t)frame (or 1 for 0), written by the fork after its conversions
	                                 // (1.8-rt-seq): another value = the arena holds an earlier flush's data (UNOFFICIAL 2026-10-30)
	NvrRtGeometryOp	ops[NVR_RT_MAX_OPS];
	NvrRtInstance	instances[NVR_RT_MAX_INSTANCES];
	NvrRtInstanceRecord records[NVR_RT_MAX_INSTANCES];	// v2: records[i] belongs to instances[i]
	NvrRtPass		passes[NVR_RT_MAX_PASSES];
};

struct NvrRtShared {
	// ---- written by NVR before setupDone, read-only afterwards
	uint32_t	magic;
	uint32_t	version;
	uint32_t	size;                // sizeof(NvrRtShared)
	uint32_t	parentPid;
	uint32_t	vendorId;            // of DXVK's Vulkan device
	uint32_t	deviceLuidValid;
	uint8_t		deviceLuid[8];
	uint8_t		deviceUuid[16];
	uint8_t		driverUuid[16];
	uint32_t	imageCount;
	uint32_t	pad0;
	NvrRtImageDesc images[NVR_RT_MAX_IMAGES];
	uint64_t	semInputsReady;      // HANDLE values valid in the helper's process (timeline semaphores, OPAQUE_WIN32)
	uint64_t	semOutputReady;
	uint32_t	setupDone;           // 1 once everything above is filled in
	uint32_t	scratchMB;           // (protocol 6, was pad1) the scratch store's size the passes need, megabytes (NVR_RT_BINDING_SCRATCH; 0 = 1)

	// ---- written by NVR any time (shaderCount last, with a release barrier); paths are full, ANSI
	volatile uint32_t	shaderCount;
	uint32_t			pad2;
	char				shaderPaths[NVR_RT_MAX_SHADERS][260];

	// ---- per frame, written by NVR (latestFrame last, with a release barrier)
	volatile int64_t	latestFrame;
	NvrRtFrame			frames[NVR_RT_FRAME_RING];

	// ---- written by the helper
	volatile uint32_t	state;               // NvrRtState
	volatile uint32_t	messageSerial;       // incremented with every new message
	volatile int64_t	consumedFrame;       // the last frame record the helper has copied out (NVR may reuse slots up to it)
	volatile int64_t	submittedFrame;      // the last frame the helper submitted
	volatile uint64_t	framesSubmitted;
	volatile uint32_t	gpuTimeUsLast;       // the helper's GPU work of the last measured frame, microseconds
	volatile uint32_t	gpuTimeUsAvg;        // average of the last 64
	volatile uint32_t	buildUsAvg;          // of which: copying the arena + new geometry builds (frames that had any)
	volatile uint32_t	sceneUsAvg;          // of which: the scene (TLAS) rebuild
	volatile uint32_t	traceUsAvg;          // of which: the passes
	volatile uint32_t	responseUsAvg;       // NVR's write -> the helper's submit, CPU, average of the last 64
	volatile uint32_t	responseUsMax;
	volatile uint32_t	geometries;          // alive in the helper
	volatile uint64_t	triangles;           // in them
	volatile uint64_t	geometryBytes;       // their structures
	volatile uint32_t	lastInstances;       // in the last scene build
	volatile uint32_t	failedOps;           // build ops refused (bad sizes, no memory) since start
	volatile uint32_t	shaderFailures;      // bitmask: shaders that did not load / compile
	volatile uint32_t	pad3;
	char				message[512];        // the last status line, NUL-terminated (the helper's log has them all)

	// ---- written by NVR
	volatile uint32_t	quit;                // 1: exit
	uint32_t			pad4;
};

#ifdef __cplusplus
static_assert(sizeof(NvrRtImageDesc) == 56, "NvrRtImageDesc layout");
static_assert(sizeof(NvrRtGeometryOp) == 32, "NvrRtGeometryOp layout");
static_assert(sizeof(NvrRtInstance) == 64, "NvrRtInstance layout");
static_assert(sizeof(NvrRtPass) == 580, "NvrRtPass layout");	// (protocol 5: + extraImages2)
static_assert(sizeof(NvrRtInstanceRecord) == 32, "NvrRtInstanceRecord layout");
static_assert(sizeof(NvrRtFrame) % 8 == 0, "NvrRtFrame layout");
static_assert(sizeof(NvrRtShared) % 8 == 0, "NvrRtShared layout");
#endif
