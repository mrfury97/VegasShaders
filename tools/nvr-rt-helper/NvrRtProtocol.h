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
#include <stddef.h>   // offsetof (protocol 9's layout check)

#define NVR_RT_MAGIC            0x54525256u   // 'VRRT'
#define NVR_RT_PROTOCOL_VERSION 9u            // 9 (2026-11-07, ptaaa-omm): opacity micromaps (NvrRtShared::opacityMicromaps, NVR -> helper, was pad4; the helper's omm* counters after it: NvrRtOmm.inl); 8 (2026-11-07, one bump for two teams): rtgap-tessblas: displaced geometry (NVR_RT_OP_DISPLACE_PARAMS / _SOURCE / _DATA, instance flag bits 8-31: NvrRtDisplace below); rtgap-textures: real textures block-compressed on the GPU route (NvrRtShared::textureImage = up to 4 transfer images, a byte each, picked by the op's VkFormat; NVR_RT_TEX_SWIZZLE_GR in NvrRtTextureOp::levels); 7 (2026-11-07): real textures (set 1, NVR_RT_OP_TEXTURE / _SLOT_TEXTURES, the -tex staging block); 2 (2026-10-28): instance records + the geometry data (bindings 10, 11); 3 (2026-10-29): 32 shared images; 4 (2026-10-30): 32 shaders; 5 (2026-11-02): 48 shared images; 6 (2026-11-07): the scratch store (binding 20, NvrRtShared::scratchMB)
#define NVR_RT_FRAME_RING       4u            // frame records; NVR waits for consumedFrame before reusing one
#define NVR_RT_MAX_IMAGES       64u           // (protocol 7: 64, was 48: 46 in use with every RT effect, the path tracer was left out)           // shared images, registered at setup (index 0 is the geometry arena); 16 was too few with all four RT effects (~20), 32 with the reference path tracer (31: every user's Setup registers, on or off; 2026-11-02)
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
// UNOFFICIAL 2026-11-07 (protocol 7) real textures: the game's own textures (full size, every mip, BC-compressed as the game keeps them)
// in the helper, sampled with filtering and a mip by the ray's footprint, instead of the atlases' 64-256 texel copies (blocky up close,
// speckled far away). Descriptor SET 1, persistent (one per command slot): binding 0 a std430 buffer of uvec4 per atlas slot (x = the
// diffuse texture's index, y = the normal map's, z = the glow map's; 0 = none: the shaders keep the atlas), binding 1 a sampler (linear,
// mips, repeat, anisotropic), binding 2 texture2D[NVR_RT_MAX_TEXTURES] (partially bound). src/hlsl/NewVegas/Compute/Rt/RtTextures.glsl.
// The bytes travel in a second shared block "<base>-tex" of NvrRtShared::textureStagingMB, a region per frame ring slot (frame % RING):
// NVR writes a frame's texture ops' bytes there before latestFrame; the helper copies them out before consumedFrame (as the frame record).
#define NVR_RT_TEXTURE_SET          1u
#define NVR_RT_BINDING_TEX_TABLE    0u
#define NVR_RT_BINDING_TEX_SAMPLER  1u
#define NVR_RT_BINDING_TEXTURES     2u
#define NVR_RT_MAX_TEXTURES         4096u     // indices 1 .. 4095 (0 = none)
#define NVR_RT_TEX_TABLE_SLOTS      1024u     // = NVR_RT_ALPHA_ATLAS_SLOTS squared
#define NVR_RT_EVENT_TEX_SUFFIX     L"-tex"
// (protocol 8) NvrRtTextureOp::levels: the low byte is the level count; NVR_RT_TEX_SWIZZLE_GR = the helper's view of the texture is
// (G, R, 1, 1) (the game's ATI2 normal maps as BC5: DXVK samples ATI2 through that swizzle, so the RT copy reads what the game reads)
#define NVR_RT_TEX_LEVELS_MASK      0xFFu
#define NVR_RT_TEX_SWIZZLE_GR       0x100u
// (protocol 8) NvrRtShared::textureImage: up to NVR_RT_TEX_TRANSFERS transfer images, byte i = shared image index + 1 (0 = none); a GPU
// route op (bytes 0) reads the one whose VkFormat equals its own (RGBA8 = the fork's decoding blit; BC1 / BC3 / BC5 = the game's blocks
// copied as they are, fork 1.8-rt-bc). Byte 0 is the RGBA8 one, as protocol 7's single value.
#define NVR_RT_TEX_TRANSFERS        4u

// NvrRtInstanceRecord::flags (NVR sets them; bits 8.. are free for the effects)
#define NVR_RT_RECORD_LAND          0x01u
#define NVR_RT_RECORD_WATER         0x02u
#define NVR_RT_RECORD_GLOW          0x04u     // the draw has a glow / emissive map
#define NVR_RT_RECORD_METAL         0x08u
#define NVR_RT_RECORD_ALBEDO_KNOWN  0x10u     // albedo is the diffuse texture's average (else a neutral grey)
// UNOFFICIAL 2026-11-07 (ptaaa-materials, RT-PATH-TRACING.md "materials") the record's MATERIAL KIND for the path tracer, two bits that
// were defined but never set (WATER 0x02 = the low bit, METAL 0x08 = the high bit): 0 plain, 1 glass (a thin dielectric pane: Fresnel
// reflection + tinted transmission; the instance is non-opaque with mask NVR_RT_MASK_ALPHA and every shader but the path tracer passes it,
// RtAlpha.glsl), 2 foliage (thin: light through the leaf), 3 skin (softer, warmer diffuse, light through ears / fingers). Only
// RtPathTrace.comp reads them; a value in the records only: the protocol's layout is unchanged.
#define NVR_RT_RECORD_KIND_LO       0x02u
#define NVR_RT_RECORD_KIND_HI       0x08u
#define NVR_RT_KIND_PLAIN           0u
#define NVR_RT_KIND_GLASS           1u
#define NVR_RT_KIND_FOLIAGE         2u
#define NVR_RT_KIND_SKIN            3u
inline uint32_t NvrRtKindBits(uint32_t kind) { return ((kind & 1u) ? NVR_RT_RECORD_KIND_LO : 0u) | ((kind & 2u) ? NVR_RT_RECORD_KIND_HI : 0u); }
inline uint32_t NvrRtKindOf(uint32_t flags) { return ((flags >> 1) & 1u) | ((flags >> 2) & 2u); }
// UNOFFICIAL 2026-10-29 alpha-tested casters ([Main.Main.RayTracing] AlphaTested): the geometry is non-opaque (cull mask bit
// NVR_RT_MASK_ALPHA), its texcoords follow the triangle list in binding 11 at the word offset floatBitsToUint(albedo.w), its
// diffuse alpha is the alpha atlas' slot (flags >> 16) (RtCore::AlphaAtlasImage(): R16F 2048x2048, 64x64 texels a slot, 32 a row), and
// a candidate hit counts where alpha >= ((flags >> 8) & 0xFF) / 255. src/hlsl/NewVegas/Compute/Rt/RtAlpha.glsl does it.
// UNOFFICIAL 2026-11-07 (ptaaa-emissive; no version change: the helper never reads them) on a record that is NOT alpha-tested, bits 8-15
// are the path tracer's emissive number + 1 (PTEmissive, src/core/RtEmissiveRow.h; 0 = none; only with NVR_RT_RECORD_GLOW)
#define NVR_RT_RECORD_ALPHA_TESTED  0x20u
// UNOFFICIAL 2026-10-29 hit textures ([Main.Main.RayTracing] HitTextures): the geometry's texcoords are in binding 11 at
// floatBitsToUint(albedo.w) (as alpha-tested ones) and its diffuse colour is the albedo atlas' slot (flags >> 16) (RGBA8, the same
// slots as the alpha atlas). src/hlsl/NewVegas/Compute/Rt/RtHit.glsl reads it.
#define NVR_RT_RECORD_TEXTURED      0x40u
// UNOFFICIAL 2026-11-02 normal textures ([Main.Main.RayTracing] NormalTextures, with HitTextures): the draw's normal map (tangent space,
// as stored) is in the normal atlas at the same slot (RGBA8; a slot filled before the map was known reads 0: no map). RtHit.glsl rtHitNormalMapped.
// (2026-11-07) also with [Main.Main.RayTracing] RealNormalMaps (RealTextures, no atlas): the map is the slot's real normal texture
// (SLOT_TEXTURES id .y, protocol 7 unchanged); id 0 and no atlas = flat.
#define NVR_RT_RECORD_NORMAL        0x80u
// UNOFFICIAL 2026-11-03 the material rule (VegasShaders.materials.txt, the object shaders' table) in bits 26..31 of a textured /
// alpha-tested record: bits 26-28 roughness (0 unknown, k = 1..7: k / 7), bits 29-31 metal (0 unknown, k = 1..7: (k - 1) / 6)
#define NVR_RT_RECORD_ROUGH_SHIFT   26u
#define NVR_RT_RECORD_METAL_SHIFT   29u
// UNOFFICIAL 2026-11-07 (perf-reflfix) smooth vertex normals: the HELPER sets this bit in a record's geometryId (NVR's ids are 24 bits)
// when the geometry's packed vertex normals are ready, one word a vertex just BEFORE its positions in binding 11: at
// vertexOffset - (indexOffset - vertexOffset) / 3 (the positions are 3 words a vertex and the triangle list follows them), octahedral
// 16 + 16 bit in object space, 0 = none (tools/nvr-rt-helper/RtNormals.comp makes them; RtHit.glsl rtHitSmoothNormal reads them).
// A shader comparing geometryId masks it (RtEmissiveBuild.comp). No layout change (protocol 9): an older shader ignores the bit.
#define NVR_RT_RECORD_GEO_NORMALS   0x80000000u
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
// (2026-11-07, rtgap-decals) bit 2 = decal ([Main.Main.RayTracing] Decals: posters, cracks, blood; always alpha-tested, mask 0x10, lifted
// 0.5 unit off what they lie on): shadow, AO and visibility rays pass them (RtAlpha.glsl rtAlphaPassShadow), every other ray sees them.
// A value in the instances only: the protocol's layout is unchanged (an older helper passes the custom index through as it is).
#define NVR_RT_CUSTOM_DECAL         0x4u
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
	// (protocol 7) real textures, in NvrRtGeometryOp's fields: NvrRtTextureOp below
	NVR_RT_OP_TEXTURE = 3,           // texture `id` (1 .. NVR_RT_MAX_TEXTURES - 1): its mips from the frame's staging region (replaces an older one)
	NVR_RT_OP_TEXTURE_RELEASE = 4,   // texture `id` is gone (freed once no frame in flight uses it; no slot may name it any more)
	NVR_RT_OP_SLOT_TEXTURES = 5,     // atlas slot `id`: its diffuse / normal / glow textures (0 = none: the atlas)
	// (protocol 8) displaced geometry (NvrRtDisplace* below; docs/next/RT-DETAIL-GAPS.md 1c)
	NVR_RT_OP_DISPLACE_PARAMS = 6,   // NvrRtDisplaceParamsOp: one block of the displacement's settings (kept by the helper until sent again)
	NVR_RT_OP_DISPLACE_SOURCE = 7,   // NvrRtDisplaceSourceOp: how geometry `id` is displaced (its height textures, layer weights, normals)
	NVR_RT_OP_DATA = 8,              // vertex attributes only, as NvrRtGeometryOp: the 3 x vertexCount floats at arenaTexel (+ with
	                                 // NVR_RT_OP_FLAG_TEXCOORDS the 2 x vertexCount after the triangle list, which is skipped) kept under
	                                 // `id` (no structure built); NVR_RT_OP_RELEASE frees it. NVR stages some other element of the vertex
	                                 // (the land's layer weights, the normals) as the "position" to send it.
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

// (protocol 7) NVR_RT_OP_TEXTURE / _SLOT_TEXTURES: the same 32 bytes as NvrRtGeometryOp, named
struct NvrRtTextureOp {              // 32 bytes
	uint32_t	op;                  // NVR_RT_OP_TEXTURE: id = texture; NVR_RT_OP_SLOT_TEXTURES: id = atlas slot
	uint32_t	id;
	uint32_t	width;               // TEXTURE: the first (biggest) level sent; SLOT_TEXTURES: the diffuse texture
	uint32_t	height;              //                                           SLOT_TEXTURES: the normal map
	uint32_t	offset;              // TEXTURE: byte offset in this frame's staging region; SLOT_TEXTURES: the glow map
	uint32_t	vkFormat;            // TEXTURE: VK_FORMAT_BC1_RGBA_UNORM_BLOCK / BC2 / BC3 / BC5 / B8G8R8A8_UNORM / R8G8B8A8_UNORM
	uint32_t	bytes;               // TEXTURE: all its levels, packed (rows of blocks, no padding), biggest first; 0 = on the GPU: the levels
	                                 // are in the transfer image of the op's format (NvrRtShared::textureImage's bytes, protocol 8), level 0
	                                 // at offset (x = low 16 bits, y = high), level l >= 1 at (x + width, y + the heights of levels 1 .. l-1)
	                                 // (the game's textures are in D3D9's default pool: no CPU copy; the fork's CopyToShared copies each
	                                 // level there: BC blocks as they are, other formats decoded to RGBA8; BC offsets / sizes whole blocks)
	uint32_t	levels;              // low byte: levels (NVR_RT_TEX_LEVELS_MASK); NVR_RT_TEX_SWIZZLE_GR (protocol 8)
};

// ---- (protocol 8, 2026-11-07, rtgap-tessblas) displaced geometry: the land's and the parallax objects' tessellation bumps in the RT scene
// ([Shaders.Terrain.Displacement] / [Shaders.POM.Displacement] with [Main.Main.RayTracing] Displacement; docs/next/RT-DETAIL-GAPS.md 1c,
// tools/nvr-rt-helper/NvrRtDisplace.inl). An instance with NVR_RT_INSTANCE_DISPLACE in NvrRtInstance::flags is traced through a copy of
// its geometry the helper splits (every triangle into N x N) and moves along the normal by the same height maps and formula as the game's
// tessellation (Compute/Tess/TerrainDisplace.tese, ObjectDisplace.tese), with the instance's own distance fade: one copy per instance
// (NVR_RT_INSTANCE_KEY: a stable 22-bit key, the geometry's address hashed), redone as the camera moves. The copy's triangles, texcoords
// and positions are what the record's vertexOffset / indexOffset / texcoord offset point at. Without a copy yet: the flat geometry.
#define NVR_RT_INSTANCE_DISPLACE    0x100u    // NvrRtInstance::flags bit 8 (bits 0-7 stay VkGeometryInstanceFlags)
#define NVR_RT_INSTANCE_KEY_SHIFT   10u       // bits 10-31: the instance's key
inline uint32_t NvrRtInstanceKey(uint32_t flags) { return flags >> NVR_RT_INSTANCE_KEY_SHIFT; }

// NVR_RT_OP_DISPLACE_PARAMS: id = the block; v[] its values. Kept by the helper until the block comes again (NVR sends all five every
// frame it gathers the scene while either kind is on, and the reach blocks with enable 0 once when both go off).
enum NvrRtDisplaceBlock : uint32_t {
	NVR_RT_DISPLACE_LAND_SHAPE = 0,   // Strength, Center, Contrast, BumpSize, LodBias, BorderGrid      ([Shaders.Terrain.Displacement])
	NVR_RT_DISPLACE_LAND_REACH = 1,   // FadeStart, FadeEnd, spacing, max split, refresh distance, enable (1 / 0)
	NVR_RT_DISPLACE_OBJ_SHAPE  = 2,   // Strength, Center, Contrast, EdgeFade, LodBias, 0               ([Shaders.POM.Displacement])
	NVR_RT_DISPLACE_OBJ_REACH  = 3,   // FadeStart, FadeEnd, spacing, max split, refresh distance, enable
	NVR_RT_DISPLACE_CAMERA     = 4,   // the camera's forward (scene space) xyz, triangles a frame (budget), triangles a copy (cap), 0
	NVR_RT_DISPLACE_BLOCKS     = 5,
};
struct NvrRtDisplaceParamsOp {       // 32 bytes, in NvrRtFrame::ops
	uint32_t	op;                  // NVR_RT_OP_DISPLACE_PARAMS
	uint32_t	id;                  // NvrRtDisplaceBlock
	float		v[6];
};
// NVR_RT_OP_DISPLACE_SOURCE: geometry `id` (a BUILD with texcoords) is displaced like this. weightsA / weightsB / normals: NVR_RT_OP_DATA
// ids (0 = none): A = the land's layer weights 0-2 as xyz (+ texcoords = weights 2-3: .y is layer 3), B = layers 4-6 as xyz; normals = the
// object-space vertex normals as xyz (none: the helper makes smooth ones from the triangles). packed: 96 bits, little endian over
// packed[0..2]: bits 12 i .. 12 i + 11 the texture id of layer i (0-6; real textures, NVR_RT_OP_TEXTURE), 84-86 the layer count (1-7),
// 87-93 the layers that have a height (bit per layer: the land shader's LandHeight > 0), 94 the kind (0 land: height in alpha, 1 a
// parallax object: height in red, layer 0 only).
struct NvrRtDisplaceSourceOp {       // 32 bytes
	uint32_t	op;                  // NVR_RT_OP_DISPLACE_SOURCE
	uint32_t	id;
	uint32_t	weightsA, weightsB, normals;
	uint32_t	packed[3];
};
inline void NvrRtDisplacePack(uint32_t packed[3], const uint32_t textures[7], uint32_t layers, uint32_t heightBits, uint32_t kind) {
	packed[0] = packed[1] = packed[2] = 0;
	auto put = [&](uint32_t bit, uint32_t width, uint32_t value) {
		for (uint32_t b = 0; b < width; b++) if ((value >> b) & 1u) packed[(bit + b) >> 5] |= 1u << ((bit + b) & 31u);
	};
	for (uint32_t i = 0; i < 7; i++) put(12 * i, 12, textures[i] & 0xFFFu);
	put(84, 3, layers & 7u); put(87, 7, heightBits & 0x7Fu); put(94, 1, kind & 1u);
}
inline uint32_t NvrRtDisplaceField(const uint32_t packed[3], uint32_t bit, uint32_t width) {
	uint32_t v = 0;
	for (uint32_t b = 0; b < width; b++) if ((packed[(bit + b) >> 5] >> ((bit + b) & 31u)) & 1u) v |= 1u << b;
	return v;
}
#define NVR_RT_DISPLACE_KIND_LAND   0u
#define NVR_RT_DISPLACE_KIND_OBJECT 1u

struct NvrRtInstance {               // 64 bytes
	float		transform[3][4];     // object -> scene (camera-relative world space): row-major 3x4, as VkTransformMatrixKHR
	uint32_t	geometryId;          // a built geometry (0 or unknown: skipped)
	uint32_t	customIndex;         // low 24 bits: rayQueryGetIntersectionInstanceCustomIndexEXT
	uint32_t	mask;                // low 8 bits: ANDed with the ray's cull mask
	uint32_t	flags;               // VkGeometryInstanceFlagBitsKHR (1 = cull disable, 2 = flip facing, 4 = force opaque) in bits 0-7;
	                                 // (protocol 8) NVR_RT_INSTANCE_DISPLACE, NVR_RT_INSTANCE_KEY
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
	uint32_t	textureStagingMB;    // (protocol 7, was pad0) the "<base>-tex" block's size (0: no real textures), a quarter a frame ring slot
	NvrRtImageDesc images[NVR_RT_MAX_IMAGES];
	uint64_t	semInputsReady;      // HANDLE values valid in the helper's process (timeline semaphores, OPAQUE_WIN32)
	uint64_t	semOutputReady;
	uint32_t	setupDone;           // 1 once everything above is filled in
	uint32_t	scratchMB;           // (protocol 6, was pad1) the scratch store's size the passes need, megabytes (NVR_RT_BINDING_SCRATCH; 0 = 1)

	// ---- written by NVR any time (shaderCount last, with a release barrier); paths are full, ANSI
	volatile uint32_t	shaderCount;
	uint32_t			textureImage;        // (protocol 7, was pad2) the real textures' transfer images: byte i = image index + 1 (0 = none), the
	                                         // helper takes the one whose format is the op's (protocol 8; protocol 7: one RGBA8 image + 1)
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
	volatile uint32_t	opacityMicromaps;    // (protocol 9, was pad4; any time) NVR_RT_OMM_* below: [Main.Main.RayTracing] OpacityMicromaps and
	                                         // the alpha atlas' image index (0 = no micromaps: what an older NVR leaves there)

	// ---- (protocol 9) written by the helper: opacity micromaps (tools/nvr-rt-helper/NvrRtOmm.inl), for NVR's log and the RT test host
	volatile uint32_t	ommState;            // NVR_RT_OMM_STATE_*
	volatile uint32_t	ommVariants;         // alpha-tested geometries with a micromap structure ready (traced through it when eligible)
	volatile uint64_t	ommBuilt;            // micromaps built (self-test: state sets made and checked) since start
	volatile uint64_t	ommMicro;            // their micro-triangles
	volatile uint64_t	ommOpaque;           // of which known opaque (resolved in hardware, no shader call)
	volatile uint64_t	ommChecked;          // validation samples that fell on a known-opaque micro-triangle (each tested with rtAlphaPass)
	volatile uint64_t	ommViolations;       // of which rtAlphaPass said "not solid" (any: micromaps off for the session)
	volatile uint32_t	ommInstances;        // instances of the last scene traced through a micromap structure
	volatile uint32_t	ommFlags;            // NVR_RT_OMM_FLAG_*
};

// (protocol 9) NvrRtShared::opacityMicromaps (NVR -> helper)
#define NVR_RT_OMM_ATLAS_MASK       0xFFu         // the alpha atlas' image index + 1 (0: none: nothing is built)
#define NVR_RT_OMM_ON               0x80000000u   // [Main.Main.RayTracing] OpacityMicromaps (used only where the GPU has VK_EXT_opacity_micromap)
#define NVR_RT_OMM_SELF_TEST        0x40000000u   // the RT test host: build and check the states on any GPU (no micromaps without the extension)
#define NVR_RT_OMM_BREAK_TEST       0x20000000u   // the RT test host's negative control: every micro-triangle marked opaque (the checks must catch it)
// NvrRtShared::ommState (helper -> NVR)
#define NVR_RT_OMM_STATE_OFF        0u            // not asked for, or no alpha atlas
#define NVR_RT_OMM_STATE_NO_EXT     1u            // asked for, the GPU has no VK_EXT_opacity_micromap: the shaders' alpha test as before
#define NVR_RT_OMM_STATE_ON         2u            // micromaps built and used
#define NVR_RT_OMM_STATE_DISABLED   3u            // a check failed (the hardware self-test or a state set): off for the session, logged
#define NVR_RT_OMM_STATE_SELF_TEST  4u            // NVR_RT_OMM_SELF_TEST without the extension: states built and checked, nothing traced
// NvrRtShared::ommFlags
#define NVR_RT_OMM_FLAG_EXTENSION   0x1u          // the device has VK_EXT_opacity_micromap (feature micromap) and it is enabled
#define NVR_RT_OMM_FLAG_HW_TESTED   0x2u          // the startup hardware self-test passed (the micro-triangle order is the one written here)
#define NVR_RT_OMM_FLAG_FINEST_MIP  0x4u          // the last frame's passes all test alpha at the finest mip (class A: RtAlpha.glsl's lod 0)

#ifdef __cplusplus
static_assert(sizeof(NvrRtImageDesc) == 56, "NvrRtImageDesc layout");
static_assert(sizeof(NvrRtGeometryOp) == 32, "NvrRtGeometryOp layout");
static_assert(sizeof(NvrRtTextureOp) == 32, "NvrRtTextureOp layout");
static_assert(sizeof(NvrRtDisplaceParamsOp) == 32 && sizeof(NvrRtDisplaceSourceOp) == 32, "displacement ops layout");
static_assert(sizeof(NvrRtInstance) == 64, "NvrRtInstance layout");
static_assert(sizeof(NvrRtPass) == 580, "NvrRtPass layout");	// (protocol 5: + extraImages2)
static_assert(sizeof(NvrRtInstanceRecord) == 32, "NvrRtInstanceRecord layout");
static_assert(sizeof(NvrRtFrame) % 8 == 0, "NvrRtFrame layout");
static_assert(sizeof(NvrRtShared) % 8 == 0, "NvrRtShared layout");
static_assert(offsetof(NvrRtShared, ommBuilt) % 8 == 0, "NvrRtShared layout (protocol 9: the same offsets in the 32-bit game and the 64-bit helper)");
#endif
