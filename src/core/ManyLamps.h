#pragma once
#include "LinearPreDecode.h"
#include "GpuProfiler.h"
#include "PointShadowForward.h"
#include "FlashlightLamp.h"	// UNOFFICIAL 2026-11-04 (the table leaves the flashlight's game lamp out)
#include <unordered_map>

// Ported 2026-10-28 from Conor (mrfury97)'s fork onto P84 v3 (github.com/mrfury97/NVR-Unofficial-Optimized pull request 1, branch
// manylamps-one-pass, commits 82ef371b 7127ac3d 3e73bf06, GPL-3.0-or-later; user: "seems like a good fit"); notes docs/next/MANYLAMPS-PORT.md.
//
// UNOFFICIAL 2026-10-27 ManyLampsOnePass ([Main.Main.Misc] ManyLampsOnePass, DXVK only; with MoreLampsPerObject; it switches LampsInOnePass's
// one-pass limit on by itself since the port):
// shiny objects lit by 6 to 13 lamps are drawn in ONE pass with all their lamps, instead of the game's 5-7 passes (light-only pass,
// extra lamp passes, texture pass, highlight passes, fog pass). It needs a restart to come on (its shaders); after that its lamp limit
// is live: off goes back to LampsInOnePass's 5 + f with the same shaders, for an F10 comparison at one spot.
// Measured 2026-10-27, Vault 21 lobby, same spot (one launch, 12 -> 5 -> 12): world draws 6,015 vs 8,677 a frame, frame time 20.3-21.0
// vs 22.9-23.1 ms, GPU world scene 12.2-12.4 vs 14.5-14.6 ms.
//
// Why the 6-lamp limit was there (read in the decompile):
//  - BSShaderPPLightingProperty::GetRenderPasses_2x (0x00BDF790) sends a shiny object to the one many-lamp pass (AddPass_Opt
//    0x00BDE9B0) only while its lamps are at most 2 + f (LampsInOnePass: 5 + f; ADD EAX,imm8 at 0x00BE00E0). Past that: several passes.
//  - The game's own light arrays hold 10 lights: ShadowLightShader::kLightData (0x011FD9A8, 10 x float4, followed by spPixelShaders) and
//    the pixel shader's PSLightPosition (c19-c26, then Toggles c27). AddPass_Opt caps the pass at 6 + f entries (MoreLampsPerObject).
//  - The 6-lamp shaders carry each lamp's direction in its own interpolator: at 6 every one of ps_3_0's 10 inputs is taken.
// So past 6 lamps nothing of the game's can be stretched. Instead:
//  - the compare is raised to 12 + f (LampsInOnePass::BeginFrame writes CompareImm), the game's cap stays 6 + f (its arrays stay safe);
//  - before every draw (Render.cpp's RenderPassStandard/SkinnedHook, and at a shader bind) NVR writes the object's lamps itself into
//    pixel registers c37-c63 (free in every ObjectTemplate pixel shader): c37.x the count, then per lamp its position and reach in the
//    object's own space, exactly as the game makes them for its shaders (c38 + 2k; ShadowLightShader::SetupGeometryConstants_Lights
//    0x00B78A90: the lamp's world position plus ShadowSceneNode::kLightingOffset through BSUtilities::GetInverseWorldMatrix 0x00C4C2D0,
//    the reach divided by the object's scale unless skinned) and its colour as the game computes it (c39 + 2k;
//    SetupGeometryOpt_LightsSpecular 0x00B7CB00: diffuse x dimmer x the light's LOD dimmer), decoded to linear on the CPU when linear
//    lighting is compiled in. The lamps are the pass's own (light 1 on; light 0 is the cell's directional light) and, for a many-lamp
//    pass the game capped, the object's other active lamps (BSShaderLightingProperty light list +0x60, read as
//    GetNumberOfActiveNonShadowLights 0x00B707D0 does), brightest first as the game sorts them;
//  - the game's own constant upload for these passes (NiD3DShaderConstantMap::SetShaderConstants 0x00E826D0, the call at 0x00E8283B)
//    writes zeros into c39-c86 while it sets up each draw, after NVR's upload: ConstantFilter's pixel hook calls AfterPixelWrite, which
//    puts the block back at once (the game's own constants for these shaders are all below c32);
//  - the 4-lamp-and-up shiny shaders (ObjectTemplate.hlsl MANY_LAMPS: SLS2025-2027.vso, SLS2034-2036.pso) carry the surface's tangent
//    frame (in the vertex's own space, as the game's lamp directions are made) instead of six lamp directions, and light every lamp per
//    pixel from these registers.
// The game's kLightingOffset is 0 in the world pass and set only while the first person is drawn (TESMain::DrawWorld_MTRender1stPerson ->
// ShadowSceneNode::SetLightingOffset 0x008750E0); for first-person draws it is learned from the game's own value for lamp 1 after each
// such draw (AfterDraw) and used for the next.
// Lamps 7-24 get no forward cube-map shadow (only six cube slots); the screen-space lamp shadows cover them as before.
// 2026-10-27 (PassFamilyView, Vault 21: the floor and wall pieces lit by 13+ lamps, 15-16 there, were still drawn in several passes
// and looked different from their one-pass neighbours): 24 lamps, the block in three ranges of pixel registers no NVR pixel shader
// uses: c37-c63 (the count and lamps 1-13), c76-c87 (lamps 14-19, between LinearPreDecode's c64-c75 and terrain's c88-c97) and
// c98-c107 (lamps 20-24, below the sun shadows' c108). The game's zeros (c39-c86) reach the second range too: AfterPixelWrite
// restores any range a write overlaps.
namespace ManyLamps {

	enum { MaxLamps = 24, FirstRegister = 37, Registers = 1 + 2 * MaxLamps };	// rows of the block (see Ranges)
	enum : UInt8 { CompareImm = MaxLamps - 1 };		// lamps <= 23 + f in one pass (f = 1 when the directional light is dark: 24)
	struct Range { UINT reg, row, rows; };
	constexpr Range Ranges[3] = { { 37, 0, 27 }, { 76, 27, 12 }, { 98, 39, 10 } };	// ObjectTemplate.hlsl NVR_ManyLamps / B / C
	static_assert(27 + 12 + 10 == Registers, "ManyLamps ranges");

	inline int Wanted = -1;			// the setting at startup (-1 not read yet)
	inline int State = -1;			// -1 not decided yet, 0 off, 1 on
	inline bool LinearCompiled = false;	// [Shaders.PBR.Linear] Enabled at startup: the shaders decode PSLightColor (LINEAR_LIGHTING_ON)
	inline float Block[Registers][4] = {};
	inline UINT BlockRegisters = 0;		// rows of the block on the device now (1 + 2 x count)
	inline const void* LastPass = nullptr;	// the pass whose lamps are in the block now
	inline UInt32 LastFrame = 0;
	inline UInt32 FrameId = 1;
	inline bool Bound = false;			// one of our shaders is bound (OnBind)
	inline bool OwnUpload = false;		// NVR's own upload is under way (AfterPixelWrite ignores it)
	inline bool FirstPerson = false;	// the pass being set up is drawn in the first-person pass
	inline D3DXVECTOR3 FirstPersonOffset(0.0f, 0.0f, 0.0f);	// the game's kLightingOffset for first-person draws, as learned
	inline bool HookWarned = false;

	// F10 statistics (one line per window)
	inline unsigned StatFrames = 0;
	inline unsigned long long StatDraws = 0, StatExtraDraws = 0, StatExtras = 0, StatOver = 0, StatByCount[MaxLamps + 1] = {};

	// UNOFFICIAL 2026-10-31 SteadyLampShadows ([Shaders.ShadowsInteriors.Forward], live, off by default; user: shadow pop-in while walking,
	// save NVRAutoTest31, Rivet City market). A many-lamp pass gives its first six lamps (the game's own entries of the pass) the
	// cube-map shadows and lights the rest (7-24) without one. Which lamps the game puts first is the order they joined the object's
	// light list, so it depends on the way you walked there: the same spot reached forward and backward showed a wall lit with a
	// beam's hole shadows one way and without them the other (8 % of the picture after 400 still frames; 0.1 % with ManyLampsOnePass
	// off). On: the lamps of a pass the game capped are ordered by how much light they give the object (colour x reach x how far into
	// that reach its bound is; lamps with a cube map first, ties by position), with a quarter's hysteresis per object, so the same six
	// get the shadows wherever you came from; c37.w 1 tells the shader its slots' lamps are re-sorted (the game's PSLightPosition reach
	// no longer gates their cube lookups; the cube's own radius does). The pass handed on is a
	// copy (the game's own list is not touched); callers use it only while one of the many-lamp shaders is bound (IsOurs), so the
	// shader's lamps and their shadow slots come from the same list (PointShadowForward::SetForDraw / OnRenderPass, SetForPass). Off: as before.
	inline bool SteadyShadows = false;		// the setting, read once a frame (BeginFrame)
	// The six each object gave the shadows last time (hysteresis: a lamp takes a place only when it beats the weakest of them by a
	// quarter), and the frame of that, so objects not drawn for a while forget them (pruned in BeginFrame).
	struct SteadyKept { const void* lamps[6] = {}; UInt32 frame = 0; };
	inline std::unordered_map<const void*, SteadyKept> SteadyMemory;
	inline PointShadowForward::RenderPassView SteadyPass = {};
	inline ShadowSceneLight* SteadyLights[MaxLamps + 2] = {};
	// The setting as the shaders were compiled with it (ShaderRecord::BuildDefines reads this too): startup value, DXVK and
	// MoreLampsPerObject (the shaders' SPECULAR_LAMPS6) required.
	inline bool WantedAtStartup() {
		if (Wanted < 0) Wanted = TheSettingManager->GetSettingI("Main.Main.Misc", "ManyLampsOnePass") != 0 &&
			TheSettingManager->GetSettingI("Main.Main.Misc", "MoreLampsPerObject") != 0;
		return Wanted != 0;
	}
	inline bool ShadersWant() {
		return WantedAtStartup() && TheRenderManager && !strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4);
	}

	// UNOFFICIAL 2026-10-29 LightCommonLamps ([Main.Main.Misc], restart; docs/next/ROOT-ASSUMPTIONS-PLAN.md C, Includes/LightCommon.hlsl): the
	// same lamps, but from ONE lamp table a frame (a float texture on s3) instead of each lamp's position and colour in the draw's constants:
	// a draw gets its list of table indices and its own world -> object rows (c37-c44), and the shader lights every lamp past the sixth in a
	// real loop (constant arrays can only be unrolled in ps_3_0). MANY_LAMPS_ON is then 2 (ShaderRecord::BuildDefines). Off: as before.
	inline int TableWanted = -1;
	inline bool TableAtStartup() {
		if (TableWanted < 0) TableWanted = TheSettingManager->GetSettingI("Main.Main.Misc", "LightCommonLamps") != 0;
		return TableWanted != 0;
	}
	// UNOFFICIAL 2026-10-30 ClusteredLamps ([Main.Main.Misc], restart to compile, then live; needs LightCommonLamps; Conor: "some form of
	// deferred rendering"): lighting decoupled from the game's per-object lamp lists. Once a frame the table's lamps are binned on the CPU into
	// screen tiles (64 px) x 8 depth slices (froxels; slice 0 nearer than 64 units, slice k covers 2^(k+5)..2^(k+6), slice 7 the rest), up to
	// 31 lamps a froxel, most important first; the lists go into the lamp table's rows 1+. The many-lamp shader keeps the draw's first six
	// lamps (their cube-map shadow slots) and then lights, per pixel, every other lamp of ITS froxel instead of the draw's list (no 24-lamp
	// cap; only the lamps that reach that part of the screen and depth). 0 live = the LightCommonLamps loop over the draw's list, as before.
	inline int ClusterWanted = -1;
	inline bool ClusterAtStartup() {
		if (ClusterWanted < 0) ClusterWanted = TableAtStartup() && TheSettingManager->GetSettingI("Main.Main.Misc", "ClusteredLamps") != 0;
		return ClusterWanted != 0;
	}
	inline const char* ShaderDefine() { return ShadersWant() ? (TableAtStartup() ? (ClusterAtStartup() ? "3" : "2") : "1") : "0"; }

	// UNOFFICIAL 2026-10-31 LampTableStandalone ([Main.Main.Misc], restart; docs/next/DEFERRED-RENDERING-PLAN.md stage 2a): the lamp table and
	// its froxel rows made at the frame's first world draw on ANY runtime (native D3D9 too), without ManyLampsOnePass, for the passes after it
	// (DeferredLamps, stage 3). Nothing reads it yet on its own: the picture is unchanged, the F10 line gives its CPU cost. Off: as before.
	inline int StandaloneWanted = -1;
	inline bool StandaloneAtStartup() {
		if (StandaloneWanted < 0) StandaloneWanted = TheSettingManager->GetSettingI("Main.Main.Misc", "LampTableStandalone") != 0;
		return StandaloneWanted != 0;
	}
	inline bool StandaloneLive = false;	// the same setting, read once a frame (BeginFrame): 0 live skips the standalone make (the CPU A/B)
	// UNOFFICIAL 2026-10-31 TableLamps ([Shaders.ShadowsInteriors.Atlas], restart; plan stage 2b-c, rung B): two more table rows hold each
	// lamp's shadow-atlas tile, joined from PointShadowForward::Slots after ShadowAtlas::Update, and the many-lamp loop (lamps 7-24; not
	// ClusteredLamps' froxel lamps: X4505) multiplies a lamp that holds a tile by a 4-tap atlas lookup (LightCommon.hlsl TableLampShadow). Needs the atlas,
	// LightCommonLamps and ManyLampsOnePass (NVR DXVK). Off: the shaders and the table as before.
	inline int TileWanted = -1;
	inline bool TileRowsAtStartup() {
		if (TileWanted < 0) TileWanted = ShadersWant() && TableAtStartup() &&
			PointShadowForward::AtlasWanted() &&
			TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "TableLamps") != 0;
		return TileWanted != 0;
	}
	// UNOFFICIAL 2026-10-31 DeferredLamps (core/DeferredLamps.h, [Shaders.ShadowsInteriors.Deferred] Enabled, restart): the lamp pass reads the
	// froxel lists and the tile rows every frame, so they are made as for LampTableStandalone + TableLamps (without TABLE_LAMP_SHADOWS).
	inline int DeferredWanted = -1;
	inline bool DeferredAtStartup() {
		if (DeferredWanted < 0) DeferredWanted = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Deferred", "Enabled") != 0 && ShadersWant() &&
			TableAtStartup() && PointShadowForward::AtlasWanted() &&
			TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled") != 0;
		return DeferredWanted != 0;
	}
	inline bool TileRowsWanted() { return TileRowsAtStartup() || DeferredAtStartup(); }
	// Rows of the table: the lamps, the froxel lists (ClusteredLamps, standalone or deferred), the two tile rows (TableLamps / deferred) last.
	inline bool FroxelRows() { return ClusterAtStartup() || StandaloneAtStartup() || DeferredAtStartup(); }
	inline UINT TableRowsNow();	// (below: TableRows)
	inline UINT TileRow();		// first tile row
	// ShaderRecord::BuildDefines: the rows the shaders must assume when they differ from what MANY_LAMPS_ON implies (only then defined).
	inline bool RowsOverride() { return ShadersWant() && TableAtStartup() && TableRowsNow() != (ClusterAtStartup() ? 31u : 1u); }

	inline bool Active() {
		if (State < 0 && TheRenderManager && TheRenderManager->device) {
			State = ShadersWant() ? 1 : 0;
			LinearCompiled = TheSettingManager->GetSettingI("Shaders.PBR.Linear", "Enabled") != 0;
			if (WantedAtStartup())
				Logger::Log(State ? "UNOFFICIAL ManyLampsOnePass: on (shiny objects with up to %d lamps in one pass; lamps in c37-c63, c76-c87, c98-c107)"
					: "UNOFFICIAL ManyLampsOnePass: off (needs DXVK; the shaders keep 6 lamp slots)", (int)MaxLamps);
		}
		return State == 1;
	}

	inline bool IsOptPass(UInt16 e) { return e >= 0xC3 && e <= 0xD6; }

	// The game's world -> object-space matrix for its lamp constants (identity rotation minus NiRenderer::kPosAdjust for skinned objects).
	typedef void(__cdecl* InverseWorldFn)(const NiTransform*, const NiSkinInstance*, D3DXMATRIX*);
	inline const InverseWorldFn GetInverseWorldMatrix = (InverseWorldFn)0x00C4C2D0;
	inline D3DXMATRIX ToObject;		// this pass's
	inline float ReachScale = 1.0f;	// 1 / the object's scale (1 when skinned), as SetupGeometryConstants_Lights
	inline D3DXVECTOR3 LightingOffset(0.0f, 0.0f, 0.0f);	// this pass's kLightingOffset as NVR applies it

	// One lamp into the block; false when it is not a usable point light.
	inline bool Put(int k, const ShadowSceneLight* light, bool linear, float gamma) {
		const NiPointLight* source = light->sourceLight;
		if (!source || !light->bPointLight) return false;
		const D3DXVECTOR3 world(source->m_worldTransform.pos.x + LightingOffset.x, source->m_worldTransform.pos.y + LightingOffset.y,
			source->m_worldTransform.pos.z + LightingOffset.z);
		D3DXVECTOR3 local;
		D3DXVec3TransformCoord(&local, &world, &ToObject);
		float* p = Block[1 + 2 * k];
		p[0] = local.x;
		p[1] = local.y;
		p[2] = local.z;
		p[3] = source->Spec.r * ReachScale;
		float dimmer = source->Dimmer;
		if (!*(const bool*)0x011F941E && dimmer > 1.0f) dimmer = 1.0f;	// BSShaderManager::bHDR
		const float lod = light->fLODDimmer;
		const float raw[4] = { source->Diff.r * dimmer * lod, source->Diff.g * dimmer * lod, source->Diff.b * dimmer * lod, 0.0f };
		float* c = Block[2 + 2 * k];
		if (linear) LinearPreDecode::DecodeFast(c, raw, gamma);
		else memcpy(c, raw, sizeof(raw));
		return true;
	}

	// --- LightCommonLamps: the frame's lamp table ---------------------------------------------------------------------------------
	enum { TableLamps = 1024, TableTexels = 2 * TableLamps, NoLamp = 4095 };	// LightCommon.hlsl LIGHT_COMMON_TEXELS / LIGHT_COMMON_NONE
	// UNOFFICIAL 2026-10-30 ClusteredLamps: the froxel grid (LightCommon.hlsl LIGHT_COMMON_ROWS, ManyLightsDirect.hlsl MANY_LAMPS_ON 3)
	enum { TilePx = 64, MaxTilesX = 40, MaxTilesY = 24, Slices = 8, ListTexels = 8, ListLamps = 31,
		MaxFroxels = MaxTilesX * MaxTilesY * Slices, TableRows = 1 + (MaxFroxels * ListTexels + TableTexels - 1) / TableTexels };
	static_assert(TableRows == 31, "LIGHT_COMMON_ROWS 31.0f in ObjectTemplate.hlsl");
	enum { TileRows = 2 };	// UNOFFICIAL 2026-10-31 TableLamps: row T 2 texels a lamp (tile u, v, span, fade | camera-relative position, 1 / radius),
							// row T + 1 one texel a lamp (cube slot, RT id, fixture size (0: not per lamp yet), 1) (LightCommon.hlsl LightCommonTile)
	inline int TilesX = 0, TilesY = 0;
	inline float BinWidth = 0.0f, BinHeight = 0.0f;	// UNOFFICIAL 2026-10-31 DeferredLamps: the viewport the lists were made for (its pass maps its pixels by screen share)
	inline bool ClusterLive = false;	// this frame: compiled in, the setting on, the lists made
	inline bool WorldDraw = false;		// the pass being set up is drawn in the world scene (the only camera / viewport the lists are made for)
	inline UInt32 ListsFrame = 0;		// FrameId the froxel lists were made for (at the frame's first world draw)
	inline double StatClusterMs = 0;
	inline unsigned long long StatClusterLamps = 0, StatClusterEntries = 0, StatClusterOver = 0, StatClusterFrames = 0;
	inline IDirect3DTexture9* Table = nullptr;
	inline bool TableFailed = false;
	inline UInt32 TableFrame = 0;		// FrameId the table was filled for
	inline std::unordered_map<const ShadowSceneLight*, int> TableIndex;
	// UNOFFICIAL 2026-11-03 (rtspec) the table's ACTIVE lamps (the ones the deferred pass bins) for the reference path tracer (RtPathTrace.cpp
	// PTGameLamps 4): world position, reach (Spec.r), colour as in the table (linear when linear lighting is compiled in); TableFrame says when
	struct ActiveLamp { float pos[3], reach, colour[3]; };
	inline std::vector<ActiveLamp> ActiveLamps;
	inline unsigned StatTableMissing = 0, StatTableLamps = 0;
	// UNOFFICIAL 2026-10-31 LampTableStandalone / TableLamps (F10 line, BeginFrame)
	inline double StatBuildMs = 0;
	inline unsigned long long StatBuilds = 0, StatTiled = 0, StatTiledPool = 0;

	// UNOFFICIAL 2026-10-31 TableLampsParity ([Shaders.ShadowsInteriors.Atlas], live, a test): the tile row as written (8 floats + cube slot a
	// lamp), and the counts of ParityCheck (F10 line).
	inline std::vector<float> TileCopy;
	inline unsigned long long ParityDraws = 0, ParityLamps = 0, ParitySame = 0, ParityTile = 0, ParityPosition = 0, ParityRadius = 0, ParityFade = 0,
		ParityDrawOnly = 0, ParityTableOnly = 0, ParityNotInTable = 0;
	inline double ParityMaxDiff = 0.0;

	// UNOFFICIAL 2026-10-31 DeferredLamps: 16 texels a froxel list (its count + 63 lamps) instead of 8 (+ 31): the deferred pass is a pixel's only
	// lighter, and Vault 21 dropped 378 entries a frame over 31 (round 3d). The forward ClusteredLamps loop reads the first 31 of them
	// (most important first) through LIGHT_COMMON_LIST_TEXELS. Without DeferredLamps: 8, as before.
	inline int ListTexelsNow() { return DeferredAtStartup() ? 16 : (int)ListTexels; }
	inline int ListLampsNow() { return ListTexelsNow() * 4 - 1; }
	inline UINT FroxelTableRows() { return 1u + (UINT)((MaxFroxels * ListTexelsNow() + TableTexels - 1) / TableTexels); }
	inline UINT TableRowsNow() { return (FroxelRows() ? FroxelTableRows() : 1u) + (TileRowsWanted() ? (UINT)TileRows : 0u); }
	inline UINT TileRow() { return FroxelRows() ? FroxelTableRows() : 1u; }

	inline bool TableMode() { return Active() && TableAtStartup() && !TableFailed; }
	// The table made at the frame's first world draw (not only at the first draw of the frame): the froxel lists need the world camera,
	// and the tiles this frame's ShadowAtlas::Update (an earlier draw may be a reflection or a cube map).
	inline bool WorldBuild() { return FroxelRows() || TileRowsWanted(); }
	inline void Upload();	// (below)

	// Every lamp of the scene (ShadowSceneNode's list) into the table, once a frame, at the first draw that wants it (so the lamps' LOD
	// dimmers are this frame's: the scene walk sets them). Position in the world (the draw adds its lighting offset), reach, colour as Put.
	// UNOFFICIAL 2026-10-30 ClusteredLamps: the active lamps into the froxel lists (rows 1+ of the locked table; pitch in floats).
	// standalone (LampTableStandalone): the lists made whatever ClusteredLamps says live; ClusterLive (the shaders' c46.w) still follows it.
	inline void BinFroxels(float* t, int pitch, const std::vector<const ShadowSceneLight*>& active, bool standalone = false) {
		const bool live = ClusterAtStartup() && TheSettingManager->GetSettingI("Main.Main.Misc", "ClusteredLamps") != 0;
		if (!live && !standalone) return;
		LARGE_INTEGER freq, c0, c1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&c0);
		D3DVIEWPORT9 vp = {};
		TheRenderManager->device->GetViewport(&vp);
		TilesX = (std::min)((int)MaxTilesX, (int)(vp.Width + TilePx - 1) / TilePx);
		TilesY = (std::min)((int)MaxTilesY, (int)(vp.Height + TilePx - 1) / TilePx);
		BinWidth = (float)vp.Width; BinHeight = (float)vp.Height;
		if (TilesX <= 0 || TilesY <= 0) return;
		const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
		const D3DXVECTOR3 right(inv._11, inv._12, inv._13), up(inv._21, inv._22, inv._23), fwd(inv._31, inv._32, inv._33);
		const float tanX = fabsf(TheRenderManager->InvProjMatrix._11), tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		auto sliceOf = [](float z) { return z < 64.0f ? 0 : (std::min)((int)Slices - 1, (int)floorf(log2f(z)) - 5); };
		struct L { int index; float key; float x, y, z, r; const ShadowSceneLight* light; };
		std::vector<L> lamps; lamps.reserve(active.size());
		// UNOFFICIAL 2026-11-01 StableOrder ([Shaders.ShadowsInteriors.Deferred] StableOrder, live; docs/next/FLICKER-PROGRAM.md part 3): the
		// lists were sorted by brightness x reach / distance from the camera, so walking re-ordered them, and the deferred pass gives the first
		// 16 lamps of a pixel's list the full shadow filter and the rest the inline one (DeferredLamps.hlsl): lamps near 16th place switched
		// filter as you moved and their shadow edges shimmered. On: brightness x reach only (no distance: the same order wherever you stand),
		// ties by the lamp (its address: steady while it lives), and a lamp's key kept from the frame before while it changes by under 10 %
		// (flickering lamps, LOD dimmer steps). A full list still drops the least important. Also the forward ClusteredLamps' first 31.
		// Off: as before.
		const bool stableOrder = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Deferred", "StableOrder") != 0;
		static std::unordered_map<const ShadowSceneLight*, std::pair<float, UInt32>> stableKeys;	// lamp -> key used, frame last seen
		static UInt32 stableFrame = 0;
		stableFrame++;
		for (const ShadowSceneLight* light : active) {
			const NiPointLight* s = light->sourceLight;
			const D3DXVECTOR3 v(s->m_worldTransform.pos.x - cam.x, s->m_worldTransform.pos.y - cam.y, s->m_worldTransform.pos.z - cam.z);
			const float x = D3DXVec3Dot(&v, &right), y = D3DXVec3Dot(&v, &up), z = D3DXVec3Dot(&v, &fwd), r = s->Spec.r;
			if (r <= 0.0f || z + r <= 1.0f) continue;	// wholly behind the camera
			const float brightness = (s->Diff.r + s->Diff.g + s->Diff.b) * s->Dimmer * light->fLODDimmer;
			float key;
			if (stableOrder) {
				key = brightness * r;
				auto& kept = stableKeys[light];
				if (kept.second && kept.second + 1 >= stableFrame && fabsf(key - kept.first) < 0.1f * kept.first) key = kept.first;
				kept = { key, stableFrame };
			}
			else {
				const float dist = (std::max)(sqrtf(x * x + y * y + z * z), 0.25f * r + 1.0f);
				key = brightness * r / dist;
			}
			lamps.push_back({ TableIndex[light], key, x, y, z, r, light });
		}
		if (stableOrder) {
			if ((stableFrame & 1023) == 0) for (auto it = stableKeys.begin(); it != stableKeys.end();) it = stableFrame - it->second.second > 1024 ? stableKeys.erase(it) : std::next(it);
			std::sort(lamps.begin(), lamps.end(), [](const L& a, const L& b) { return a.key != b.key ? a.key > b.key : (uintptr_t)a.light < (uintptr_t)b.light; });
		}
		else {
			if (!stableKeys.empty()) stableKeys.clear();
			std::sort(lamps.begin(), lamps.end(), [](const L& a, const L& b) { return a.key > b.key; });	// a full list drops the least important
		}
		static std::vector<UInt8> counts;
		counts.assign((size_t)TilesX * TilesY * Slices, 0);
		float* lists = t + pitch;	// row 1
		auto put = [&](int froxel, int k, float value) {	// entry k of the froxel's list (-1 = its count)
			const int texel = froxel * ListTexelsNow() + (k + 1) / 4, comp = (k + 1) % 4;
			lists[(texel / TableTexels) * pitch + (texel % TableTexels) * 4 + comp] = value;
		};
		for (const L& l : lamps) {
			const float zmin = l.z - l.r, zmax = l.z + l.r;
			int x0 = 0, x1 = TilesX - 1, y0 = 0, y1 = TilesY - 1;
			if (zmin > 8.0f) {	// a conservative screen rect of the sphere (each side taken at the depth that makes it widest)
				const float lx = l.x - l.r, hx = l.x + l.r, ly = l.y - l.r, hy = l.y + l.r;
				const float nx0 = lx / ((lx < 0.0f ? zmin : zmax) * tanX), nx1 = hx / ((hx > 0.0f ? zmin : zmax) * tanX);
				const float ny0 = ly / ((ly < 0.0f ? zmin : zmax) * tanY), ny1 = hy / ((hy > 0.0f ? zmin : zmax) * tanY);
				if (nx0 > 1.0f || nx1 < -1.0f || ny0 > 1.0f || ny1 < -1.0f) continue;	// off screen
				x0 = (std::max)(0, (int)floorf((nx0 * 0.5f + 0.5f) * vp.Width / TilePx));
				x1 = (std::min)(TilesX - 1, (int)floorf((nx1 * 0.5f + 0.5f) * vp.Width / TilePx));
				y0 = (std::max)(0, (int)floorf((0.5f - ny1 * 0.5f) * vp.Height / TilePx));
				y1 = (std::min)(TilesY - 1, (int)floorf((0.5f - ny0 * 0.5f) * vp.Height / TilePx));
			}
			const int s0 = sliceOf((std::max)(zmin, 0.0f)), s1 = sliceOf(zmax);
			StatClusterLamps++;
			for (int sl = s0; sl <= s1; sl++)
				for (int ty = y0; ty <= y1; ty++)
					for (int tx = x0; tx <= x1; tx++) {
						const int froxel = (sl * TilesY + ty) * TilesX + tx;
						UInt8& c = counts[froxel];
						if (c >= ListLampsNow()) { StatClusterOver++; continue; }
						put(froxel, c, (float)l.index);
						c++;
						StatClusterEntries++;
					}
		}
		for (size_t f = 0; f < counts.size(); f++) put((int)f, -1, (float)counts[f]);
		ClusterLive = live;
		QueryPerformanceCounter(&c1);
		StatClusterMs += 1000.0 * (c1.QuadPart - c0.QuadPart) / freq.QuadPart;
		StatClusterFrames++;
	}

	// UNOFFICIAL 2026-10-31 TableLamps: each table lamp's shadow-atlas tile (rows TileRow(), TileRow() + 1; zeros = no tile, unshadowed), from
	// the slots ShadowAtlas::Update filled this frame (live slots and pool lamps alike), as PointShadowForward::SetForPass reads them for a
	// draw's first six lamps: position and reach of the shadow map, camera-relative (GetShadowWorldPos's origin), its fade, its RT id.
	inline void PutTiles(float* t, int pitch, const std::vector<const ShadowSceneLight*>& order) {
		float* a = t + pitch * TileRow();
		float* b = a + pitch;
		TileCopy.assign(order.size() * 9, 0.0f);	// (TableLampsParity)
		if (!PointShadowForward::Active || !PointShadowForward::AtlasTexture) return;
		if (!DeferredAtStartup() && TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "TableLampsLive") == 0) return;	// live A/B: no tiles, unshadowed
		static std::unordered_map<const ShadowSceneLight*, int> slotOf;
		slotOf.clear();
		for (int s = 0; s < PointShadowForward::SlotCount; s++)
			if (PointShadowForward::Slots[s].light && (PointShadowForward::Slots[s].tile[2] > 0.0f ||	// UNOFFICIAL 2026-11-06: or traced (no tile: slots 32-63)
				(!RtLampShadows::TracedMany && RtLampShadows::TracedSlot(PointShadowForward::Slots[s].slot) >= 0.0f))) slotOf.emplace(PointShadowForward::Slots[s].light, s);
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		for (size_t n = 0; n < order.size(); n++) {
			auto found = slotOf.find(order[n]);
			if (found == slotOf.end()) continue;
			const PointShadowForward::Slot& s = PointShadowForward::Slots[found->second];
			float* p = a + n * 8;
			p[0] = s.tile[0]; p[1] = s.tile[1]; p[2] = s.tile[2]; p[3] = s.fade;
			p[4] = s.position.x - camera.x; p[5] = s.position.y - camera.y; p[6] = s.position.z - camera.z; p[7] = 1.0f / s.radius;
			const int manyId = RtLampShadows::ManyIdOf(order[n]);
			float* c = b + n * 4;
			c[0] = (float)s.slot;
			c[1] = manyId >= 0 ? (float)manyId : RtLampShadows::TracedMany ? -1.0f : RtLampShadows::TracedSlot(s.slot);
			c[2] = 0.0f; c[3] = 1.0f;
			memcpy(&TileCopy[n * 9], p, 8 * sizeof(float));
			TileCopy[n * 9 + 8] = (float)s.slot;
			StatTiled++;
			if (s.slot >= ShadowCubeMapsMax) StatTiledPool++;
		}
	}

	inline void BuildTable() {
		LARGE_INTEGER freq, c0, c1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&c0);
		TableFrame = FrameId;
		TableIndex.clear();
		IDirect3DDevice9* d = TheRenderManager->device;
		const UINT rows = TableRowsNow();	// UNOFFICIAL 2026-10-30 ClusteredLamps: the froxel lists below the lamps (2026-10-31: + the tile rows)
		if (!Table && FAILED(d->CreateTexture(TableTexels, rows, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &Table, NULL))) {
			Logger::Log("[ERROR] UNOFFICIAL LightCommonLamps: the lamp table could not be made: the lamps of many-lamp objects stay dark (set it off)");
			TableFailed = true;
			return;
		}
		D3DLOCKED_RECT r = {};
		if (FAILED(Table->LockRect(0, &r, NULL, D3DLOCK_DISCARD))) return;
		float* t = (float*)r.pBits;
		memset(t, 0, TableTexels * 4 * sizeof(float));
		if (TileRowsWanted()) for (UINT row = TileRow(); row < rows; row++) memset((UInt8*)t + row * r.Pitch, 0, TableTexels * 4 * sizeof(float));
		std::vector<const ShadowSceneLight*> active;	// UNOFFICIAL 2026-10-30 ClusteredLamps: the lamps the game has on
		static std::vector<const ShadowSceneLight*> order;	// UNOFFICIAL 2026-10-31 TableLamps: lamp n of the table
		order.clear();
		ActiveLamps.clear();
		const bool linear = LinearCompiled && LinearPreDecode::Gamma > 1.0f;
		const float gamma = LinearPreDecode::Gamma;
		int n = 0;
		for (NiTList<ShadowSceneLight>::Entry* e = SceneNode ? SceneNode->lights.start : nullptr; e && n < TableLamps; e = e->next) {
			const ShadowSceneLight* light = e->data;
			const NiPointLight* source = light ? light->sourceLight : nullptr;
			if (!source || !light->bPointLight || TableIndex.count(light)) continue;
			if (FlashlightLamp::Is(source)) continue;	// UNOFFICIAL 2026-11-04: the flashlight's game lamp (the light pass is the flashlight here)
			float* p = t + n * 8;
			p[0] = source->m_worldTransform.pos.x; p[1] = source->m_worldTransform.pos.y; p[2] = source->m_worldTransform.pos.z;
			p[3] = source->Spec.r;
			float dimmer = source->Dimmer;
			if (!*(const bool*)0x011F941E && dimmer > 1.0f) dimmer = 1.0f;	// BSShaderManager::bHDR (as Put)
			const float lod = light->fLODDimmer;
			const float raw[4] = { source->Diff.r * dimmer * lod, source->Diff.g * dimmer * lod, source->Diff.b * dimmer * lod, 0.0f };
			if (linear) LinearPreDecode::DecodeFast(p + 4, raw, gamma);
			else memcpy(p + 4, raw, sizeof(raw));
			TableIndex[light] = n++;
			order.push_back(light);
			// as GetNumberOfActiveNonShadowLights: not switched off for the view (+0x110), not culled, not flagged (+0xEC)
			const UInt8* raw8 = (const UInt8*)light;
			if (*(const UInt16*)(raw8 + 0x110) != 0xFF && !(((const UInt8*)source)[0x30] & 1) && !raw8[0xEC]) {
				active.push_back(light);
				ActiveLamps.push_back({ { p[0], p[1], p[2] }, p[3], { p[4], p[5], p[6] } });
			}
		}
		ClusterLive = false;
		// (the lists only at a world-scene draw: an earlier draw of the frame may be a reflection or a cube map, with its own camera and size)
		if (WorldBuild() && WorldDraw) ListsFrame = FrameId;
		if (FroxelRows() && WorldDraw) BinFroxels(t, r.Pitch / (int)sizeof(float), active, StandaloneAtStartup() || DeferredAtStartup());
		if (TileRowsWanted()) PutTiles(t, r.Pitch / (int)sizeof(float), order);
		Table->UnlockRect(0);
		StatTableLamps = n;
		QueryPerformanceCounter(&c1);
		StatBuildMs += 1000.0 * (c1.QuadPart - c0.QuadPart) / freq.QuadPart;
		StatBuilds++;
	}

	inline int TableSlot(const ShadowSceneLight* light) {
		if (!light) return NoLamp;
		auto found = TableIndex.find(light);
		if (found != TableIndex.end()) return found->second;
		if (GpuTimer::Enabled) StatTableMissing++;
		return NoLamp;	// a lamp the scene's list did not hold at the frame's first draw: left out this frame
	}

	// The object's lamps for this pass as table indices (the same choice and order as SetForPass below).
	inline void SetForPassTable(const PointShadowForward::RenderPassView* pass) {
		// UNOFFICIAL 2026-10-30 ClusteredLamps: made again at the frame's first world draw (a discarding lock: earlier draws keep theirs)
		if (TableFrame != FrameId || (WorldBuild() && WorldDraw && ListsFrame != FrameId)) BuildTable();
		if (TableFailed) return;
		int idx[MaxLamps];
		int count = 0;
		if (pass && pass->lights && pass->geometry) {
			NiGeometry* geometry = pass->geometry;
			GetInverseWorldMatrix(&geometry->m_worldTransform, geometry->skinInstance, &ToObject);
			ReachScale = !geometry->skinInstance && geometry->m_worldTransform.scale > 0.0f ? 1.0f / geometry->m_worldTransform.scale : 1.0f;
			LightingOffset = FirstPerson ? FirstPersonOffset : D3DXVECTOR3(0.0f, 0.0f, 0.0f);
			for (int i = 1; i < pass->numLights && count < MaxLamps; i++) {
				const ShadowSceneLight* light = pass->lights[i];	// (lamp k stays light k + 1 of the pass: its shadow slot)
				idx[count++] = light && light->sourceLight && light->bPointLight ? TableSlot(light) : NoLamp;
			}
			if (pass->maxLights > pass->numLights && IsOptPass(pass->passEnum)) {	// as SetForPass: a capped many-lamp pass's other lamps
				const UInt8* property = (const UInt8*)geometry->propertyState.m_spShadeProperty;
				int extras = 0;
				for (const UInt32* node = property ? *(const UInt32* const*)(property + 0x60) : nullptr; node; node = (const UInt32*)node[0]) {
					const UInt8* light = (const UInt8*)node[2];
					if (!light || *(const UInt16*)(light + 0x110) == 0xFF) continue;
					const UInt8* niLight = *(const UInt8* const*)(light + 0xF8);
					if (!niLight || (niLight[0x30] & 1) || light[0xEC]) continue;
					bool known = false;
					for (int i = 1; i < pass->numLights && !known; i++) known = (const UInt8*)pass->lights[i] == light;
					if (known) continue;
					if (count == MaxLamps) { StatOver++; continue; }
					const ShadowSceneLight* l = (const ShadowSceneLight*)light;
					if (!l->sourceLight || !l->bPointLight) continue;
					const int slot = TableSlot(l);
					if (slot == NoLamp) continue;
					idx[count++] = slot; extras++;
				}
				if (GpuTimer::Enabled && extras) { StatExtraDraws++; StatExtras += extras; }
			}
		}
		for (int k = count; k < MaxLamps; k++) idx[k] = NoLamp;
		Block[0][0] = (float)count; Block[0][1] = ReachScale; Block[0][2] = 0.0f;
		Block[0][3] = pass == &SteadyPass ? 1.0f : 0.0f;	// UNOFFICIAL 2026-10-31 SteadyLampShadows: the slots' lamps are re-sorted (ObjectTemplate.hlsl)
		Block[1][0] = LightingOffset.x; Block[1][1] = LightingOffset.y; Block[1][2] = LightingOffset.z; Block[1][3] = 0.0f;
		const D3DXMATRIX& m = ToObject;	// row vectors: local = (world, 1) x m, so row r of the shader's transform is m's column r
		const float rows[3][4] = { { m._11, m._21, m._31, m._41 }, { m._12, m._22, m._32, m._42 }, { m._13, m._23, m._33, m._43 } };
		memcpy(Block[2], rows, sizeof(rows));
		for (int f = 0; f < 12; f++)	// two 12-bit indices a float (exact below 2^24), eight a float4 (LightCommonIndex)
			Block[5 + f / 4][f % 4] = (float)(idx[f * 2] + 4096 * idx[f * 2 + 1]);
		BlockRegisters = 8;
		if (ClusterAtStartup()) {	// UNOFFICIAL 2026-10-30 ClusteredLamps: c45 view depth from the object's space, c46-c47 the froxel grid
			D3DXMATRIX toWorld;
			const bool on = ClusterLive && ListsFrame == FrameId && WorldDraw && !FirstPerson && D3DXMatrixInverse(&toWorld, nullptr, &ToObject);
			if (on) {	// view depth = (object point, 1) x toWorld - camera, along the camera's forward axis
				const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
				const float f[3] = { iv._31, iv._32, iv._33 };
				const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
				for (int j = 0; j < 3; j++) Block[8][j] = toWorld.m[j][0] * f[0] + toWorld.m[j][1] * f[1] + toWorld.m[j][2] * f[2];
				Block[8][3] = (toWorld.m[3][0] - cam.x) * f[0] + (toWorld.m[3][1] - cam.y) * f[1] + (toWorld.m[3][2] - cam.z) * f[2];
			}
			else memset(Block[8], 0, sizeof(Block[8]));
			Block[9][0] = 1.0f / TilePx; Block[9][1] = (float)TilesX; Block[9][2] = (float)(TilesX * TilesY); Block[9][3] = on ? 1.0f : 0.0f;
			Block[10][0] = (float)Slices - 1.0f; Block[10][1] = (float)FroxelTableRows(); Block[10][2] = Block[10][3] = 0.0f;
			BlockRegisters = 11;
		}
		Upload();
		TheRenderManager->renderState->SetTexture(3, Table);	// through the game's state cache, so its own binds there still happen
		for (DWORD s : { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER }) TheRenderManager->renderState->SetSamplerState(3, (D3DSAMPLERSTATETYPE)s, D3DTEXF_POINT, false);
		TheRenderManager->renderState->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE, false);
		TheRenderManager->renderState->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP, false);
		TheRenderManager->renderState->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP, false);
		LastPass = pass;
		LastFrame = FrameId;
		if (GpuTimer::Enabled) { StatDraws++; StatByCount[count]++; }
	}

	// The block's rows in use onto the device (NVR's own upload: AfterPixelWrite ignores it).
	inline void Upload() {
		OwnUpload = true;
		for (const Range& r : Ranges) {
			if (BlockRegisters <= r.row) break;
			TheRenderManager->device->SetPixelShaderConstantF(r.reg, &Block[r.row][0], min(r.rows, BlockRegisters - r.row));
		}
		OwnUpload = false;
	}

	// The object's lamps for this pass into the block.
	inline void SetForPass(const PointShadowForward::RenderPassView* pass) {
		if (TableMode()) { SetForPassTable(pass); return; }	// UNOFFICIAL 2026-10-29 LightCommonLamps
		int count = 0;
		const bool linear = LinearCompiled && LinearPreDecode::Gamma > 1.0f;	// as LinColor: decoded only above gamma 1
		const float gamma = LinearPreDecode::Gamma;
		if (pass && pass->lights && pass->geometry) {
			NiGeometry* geometry = pass->geometry;
			GetInverseWorldMatrix(&geometry->m_worldTransform, geometry->skinInstance, &ToObject);
			ReachScale = !geometry->skinInstance && geometry->m_worldTransform.scale > 0.0f ? 1.0f / geometry->m_worldTransform.scale : 1.0f;
			LightingOffset = FirstPerson ? FirstPersonOffset : D3DXVECTOR3(0.0f, 0.0f, 0.0f);
			for (int i = 1; i < pass->numLights && count < MaxLamps; i++) {
				const ShadowSceneLight* light = pass->lights[i];
				// Lamp k must stay light k + 1 of the pass (the shader gives it that light's cube-map shadow slot): one that is not a usable
				// point light keeps its place with reach 0, which the shader skips.
				if (!light || !Put(count, light, linear, gamma)) memset(Block[1 + 2 * count], 0, 2 * sizeof(Block[0]));
				count++;
			}
			// A many-lamp pass the game capped (AddPass_Opt keeps 6 + f entries of maxLights): its other lamps from the object's list.
			if (pass->maxLights > pass->numLights && IsOptPass(pass->passEnum)) {
				const UInt8* property = (const UInt8*)geometry->propertyState.m_spShadeProperty;
				int extras = 0;
				for (const UInt32* node = property ? *(const UInt32* const*)(property + 0x60) : nullptr; node; node = (const UInt32*)node[0]) {
					const UInt8* light = (const UInt8*)node[2];
					if (!light || *(const UInt16*)(light + 0x110) == 0xFF) continue;	// as GetNumberOfActiveNonShadowLights
					const UInt8* niLight = *(const UInt8* const*)(light + 0xF8);
					if (!niLight || (niLight[0x30] & 1) || light[0xEC]) continue;
					bool known = false;
					for (int i = 1; i < pass->numLights && !known; i++) known = (const UInt8*)pass->lights[i] == light;
					if (known) continue;
					if (count == MaxLamps) { StatOver++; continue; }
					if (Put(count, (const ShadowSceneLight*)light, linear, gamma)) { count++; extras++; }
				}
				if (GpuTimer::Enabled && extras) { StatExtraDraws++; StatExtras += extras; }
			}
		}
		Block[0][0] = (float)count;
		Block[0][1] = Block[0][2] = 0.0f;
		Block[0][3] = pass == &SteadyPass ? 1.0f : 0.0f;	// UNOFFICIAL 2026-10-31 SteadyLampShadows: the slots' lamps are re-sorted (ObjectTemplate.hlsl)
		BlockRegisters = 1 + 2 * count;
		Upload();
		LastPass = pass;
		LastFrame = FrameId;
		if (GpuTimer::Enabled) { StatDraws++; StatByCount[count]++; }
	}


	// (SteadyLampShadows, see above SteadyShadows.) The ranking uses only what stays put: the lamp's colour x its reach x how far into that
	// reach the object's bound is (not the dimmer or the LOD fade, which change from frame to frame). Lamps with a cube map this frame come
	// first, ties by position; nulls and lamps that are not point lights keep their key -1 (last).
	inline bool HasShadowMap(const ShadowSceneLight* light) {
		for (int s = 0; s < PointShadowForward::SlotCount; s++) if (PointShadowForward::Slots[s].light == light) return true;
		return false;
	}
	inline const PointShadowForward::RenderPassView* Steady(const PointShadowForward::RenderPassView* pass) {
		if (!SteadyShadows || !Active() || !pass || !pass->lights || !pass->geometry || !IsOptPass(pass->passEnum) ||
			pass->maxLights <= pass->numLights || pass->numLights < 2 || pass->numLights > MaxLamps + 1) return pass;
		NiGeometry* geometry = pass->geometry;
		const NiBound* bound = geometry->m_kWorldBound;
		if (!bound) return pass;
		const UInt8* property = (const UInt8*)geometry->propertyState.m_spShadeProperty;
		struct Lamp { ShadowSceneLight* light; bool map; float key, x, y, z; };
		Lamp lamps[MaxLamps + 2];
		int n = 0;
		auto add = [&](ShadowSceneLight* light) {
			if (n >= MaxLamps + 2) return;
			const NiPointLight* source = light ? light->sourceLight : nullptr;
			Lamp l = { light, false, -1.0f, 0.0f, 0.0f, 0.0f };
			if (source && light->bPointLight) {
				l.x = source->m_worldTransform.pos.x; l.y = source->m_worldTransform.pos.y; l.z = source->m_worldTransform.pos.z;
				const float dx = l.x - bound->Center.x, dy = l.y - bound->Center.y, dz = l.z - bound->Center.z;
				const float reach = (std::max)(source->Spec.r, 1.0f);
				const float gap = (std::max)(0.0f, sqrtf(dx * dx + dy * dy + dz * dz) - bound->Radius);
				const float falloff = (std::max)(0.0f, 1.0f - gap / reach);
				l.key = (source->Diff.r + source->Diff.g + source->Diff.b) * reach * falloff * falloff;
				l.map = HasShadowMap(light);
			}
			lamps[n++] = l;
		};
		for (int i = 1; i < pass->numLights; i++) add(pass->lights[i]);	// (nulls too: they keep their place, key -1)
		const int own = n;
		for (const UInt32* node = property ? *(const UInt32* const*)(property + 0x60) : nullptr; node && n < MaxLamps + 2; node = (const UInt32*)node[0]) {
			UInt8* light = (UInt8*)node[2];
			if (!light || *(const UInt16*)(light + 0x110) == 0xFF) continue;	// as SetForPass's extras
			const UInt8* niLight = *(const UInt8* const*)(light + 0xF8);
			if (!niLight || (niLight[0x30] & 1) || light[0xEC]) continue;
			bool known = false;
			for (int i = 1; i < pass->numLights && !known; i++) known = (UInt8*)pass->lights[i] == light;
			if (!known) add((ShadowSceneLight*)light);
		}
		if (n == own) return pass;	// nothing past the cap: the game's own order (every lamp in it has its shadow slot)
		auto better = [](const Lamp& a, const Lamp& b) {
			if (a.map != b.map) return a.map;
			if (a.key != b.key) return a.key > b.key;
			if (a.x != b.x) return a.x < b.x;
			if (a.y != b.y) return a.y < b.y;
			return a.z < b.z;
		};
		std::stable_sort(lamps, lamps + n, better);
		// Hysteresis: last time's holders keep their places unless a lamp outside them is a quarter stronger than the weakest holder.
		const int places = (std::min)(own, 6);
		SteadyKept& kept = SteadyMemory[geometry];
		if (kept.frame && FrameId - kept.frame < 8) {
			auto wasKept = [&](const void* light) { for (int k = 0; k < 6; k++) if (light && kept.lamps[k] == light) return true; return false; };
			for (int pass2 = 0; pass2 < 6; pass2++) {
				int challenger = -1;
				for (int i = places; i < n && challenger < 0; i++) if (wasKept(lamps[i].light) && lamps[i].map) challenger = i;
				if (challenger < 0) break;	// every holder that is still here is in a place
				// a holder fell out of the places: it comes back unless the lamp in the last non-holder place beats it by a quarter
				int victim = -1;
				for (int i = places - 1; i >= 0 && victim < 0; i--) if (!wasKept(lamps[i].light)) victim = i;
				if (victim < 0) break;
				if (lamps[victim].map == lamps[challenger].map && lamps[victim].key > 1.25f * lamps[challenger].key) break;
				std::swap(lamps[victim], lamps[challenger]);
				std::stable_sort(lamps, lamps + places, better);
			}
		}
		for (int k = 0; k < 6; k++) kept.lamps[k] = k < places ? lamps[k].light : nullptr;
		kept.frame = FrameId;
		SteadyPass = *pass;
		SteadyLights[0] = pass->lights[0];
		for (int i = 0; i < own; i++) SteadyLights[1 + i] = lamps[i].light;	// the best ones take the pass's own places (and the shadows)
		SteadyPass.lights = SteadyLights;
		return &SteadyPass;
	}

	// Only while one of NVR's many-lamp shiny shaders is bound (SLS2034-2036.pso, NVR's record in place): other pixel shaders, the
	// game's or NVR's, may hold their own values in c37-c63 for a whole run of draws (set once at their bind), and a write per object
	// would overwrite them.
	inline bool IsOurs(const char* name) {
		return name && !strncmp(name, "SLS203", 6) && name[6] >= '4' && name[6] <= '6' && !strcmp(name + 7, ".pso");
	}

	// Before every object's draw (Render.cpp), and when the game binds a pass's shaders (SetShadersHook, BSBatchRenderer::BeginPass: the run's
	// first object, from BSShaderManager::pCurrentRenderPass).
	inline void ForPass(const PointShadowForward::RenderPassView* pass, bool firstPerson, bool world = false) {
		// UNOFFICIAL 2026-10-31 LampTableStandalone: the frame's first world draw makes the table, whatever is bound (any runtime)
		if (world && ListsFrame != FrameId && !TableFailed && StandaloneLive && !firstPerson) { WorldDraw = true; BuildTable(); }
		if (!Bound || !Active()) return;
		FirstPerson = firstPerson;
		WorldDraw = world;
		SetForPass(pass);
	}
	inline void OnBind(const char* pixelShaderName, bool nvrRecord, bool firstPerson, bool world = false) {
		Bound = nvrRecord && IsOurs(pixelShaderName) && Active();
		if (!Bound) return;
		FirstPerson = firstPerson;
		WorldDraw = world;
		const PointShadowForward::RenderPassView* pass = Steady(*(const PointShadowForward::RenderPassView* const*)0x011F91E0);	// (2026-10-31 SteadyLampShadows)
		if (pass && (pass != LastPass || LastFrame != FrameId || pass == &SteadyPass)) SetForPass(pass);
	}

	// ConstantFilter's pixel hook, after every upload: one that covered part of the block while one of our shaders is bound (the game's
	// zeros in c39-c86, see above) gets the block put back at once.
	inline void AfterPixelWrite(UINT start, UINT count) {
		if (OwnUpload || !Bound || !BlockRegisters || LastFrame != FrameId) return;
		bool overlaps = false;
		for (const Range& r : Ranges) {
			if (BlockRegisters <= r.row) break;
			const UINT used = min(r.rows, BlockRegisters - r.row);
			if (start < r.reg + used && start + count > r.reg) { overlaps = true; break; }
		}
		if (overlaps) Upload();
	}

	// UNOFFICIAL 2026-10-31 TableLampsParity (Render.cpp, after PointShadowForward::OnRenderPass; a test, live, off): for the first six lamps of
	// a many-lamp draw, the lamp's row in the table's tile rows against the shadow constants the draw itself got (c174-c179: camera-relative
	// position, 1 / radius; c182-c187: tile u, v, span, fade). Equal = lamps 7-24 read exactly the inputs the first six read (any difference
	// in the picture is the filter); not equal = a join fault, by field. Reads the device's constants back: only while the test is on.
	inline void ParityCheck(const PointShadowForward::RenderPassView* pass, bool world) {
		if (!world || !Bound || !TileRowsAtStartup() || !pass || !pass->lights || TileCopy.empty()) return;
		if (TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "TableLampsParity") == 0 ||
			TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "TableLampsLive") == 0) return;
		float light[6][4], tint[6][4];
		IDirect3DDevice9* d = TheRenderManager->device;
		if (FAILED(d->GetPixelShaderConstantF(174, &light[0][0], 6)) || FAILED(d->GetPixelShaderConstantF(182, &tint[0][0], 6))) return;
		ParityDraws++;
		const int first = PointShadowForward::FirstShaderLight(pass);
		const int count = (std::min)((int)pass->numLights - first, 6);
		for (int k = 0; k < count; k++) {
			const ShadowSceneLight* l = pass->lights[first + k];
			if (!l || !l->bPointLight) continue;
			auto found = TableIndex.find(l);
			const bool drawHas = light[k][3] > 0.0f && tint[k][2] > 0.0f;	// a tile (the atlas path; traced-only lamps carry no tile)
			if (found == TableIndex.end() || (size_t)found->second * 9 + 9 > TileCopy.size()) { if (drawHas) ParityNotInTable++; continue; }
			const float* row = &TileCopy[found->second * 9];
			const bool tableHas = row[2] > 0.0f;
			if (!drawHas && !tableHas) continue;
			ParityLamps++;
			if (drawHas != tableHas) { (drawHas ? ParityDrawOnly : ParityTableOnly)++; continue; }
			auto off = [](float x, float y) { return fabsf(x - y) > 1.0e-3f * (std::max)(1.0f, fabsf(y)); };
			const bool tile = off(row[0], tint[k][0]) || off(row[1], tint[k][1]) || off(row[2], tint[k][2]);
			const bool position = off(row[4], light[k][0]) || off(row[5], light[k][1]) || off(row[6], light[k][2]);
			const bool radius = off(row[7], light[k][3]);
			const bool fade = off(row[3], tint[k][3]);
			for (int j = 0; j < 3; j++) ParityMaxDiff = (std::max)(ParityMaxDiff, (double)fabsf(row[4 + j] - light[k][j]));
			if (tile) ParityTile++;
			if (position) ParityPosition++;
			if (radius) ParityRadius++;
			if (fade) ParityFade++;
			if (!tile && !position && !radius && !fade) ParitySame++;
		}
	}

	// After a draw with one of our shaders (Render.cpp): for first-person draws, the game's own lamp 1 (c19, object space with its
	// kLightingOffset) back to world space, minus the lamp's world position, is the offset for the next first-person draw.
	inline void AfterDraw(const PointShadowForward::RenderPassView* pass) {
		if (!FirstPerson || !Bound || !Active() || !pass) return;
		if (pass != LastPass && !(LastPass == &SteadyPass && SteadyPass.geometry == pass->geometry)) return;	// (SteadyLampShadows: c19 is still the game's own lamp 1)
		if (pass->numLights < 2 || !pass->lights || !pass->lights[1] || !pass->lights[1]->sourceLight) return;
		float game[4];
		D3DXMATRIX toWorld;
		if (FAILED(TheRenderManager->device->GetPixelShaderConstantF(19, game, 1)) || !D3DXMatrixInverse(&toWorld, nullptr, &ToObject)) return;
		D3DXVECTOR3 world;
		const D3DXVECTOR3 local(game[0], game[1], game[2]);
		D3DXVec3TransformCoord(&world, &local, &toWorld);
		const NiPoint3& lamp = pass->lights[1]->sourceLight->m_worldTransform.pos;
		const D3DXVECTOR3 offset(world.x - lamp.x, world.y - lamp.y, world.z - lamp.z);
		if (_finite(offset.x) && _finite(offset.y) && _finite(offset.z)) FirstPersonOffset = offset;
	}

	// Once a frame (Render.cpp). restoreHooked: ConstantFilter's pixel hook is in (without it the game's zeros stay and the lamps are lost).
	inline void BeginFrame(bool restoreHooked) {
		FrameId++;
		SteadyShadows = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "SteadyLampShadows") != 0;	// UNOFFICIAL 2026-10-31
		if ((FrameId & 255) == 0 || !SteadyShadows) {	// objects not drawn for a while (or the setting off): their kept lamps forgotten
			if (!SteadyShadows) SteadyMemory.clear();
			else for (auto it = SteadyMemory.begin(); it != SteadyMemory.end();) it = FrameId - it->second.frame > 64 ? SteadyMemory.erase(it) : std::next(it);
		}
		StandaloneLive = StandaloneAtStartup() && TheSettingManager->GetSettingI("Main.Main.Misc", "LampTableStandalone") != 0;
		if (!Active()) {
			// UNOFFICIAL 2026-10-31 LampTableStandalone: the table's cost where nothing else here runs (native D3D9, ManyLampsOnePass off)
			if (!StandaloneAtStartup() || !GpuTimer::Enabled) { StatFrames = 0; return; }
			if (++StatFrames < 240) return;
			const double f = StatFrames;
			Logger::Log("LAMP TABLE standalone %u frames: made %.2f a frame, %.3f ms CPU a frame (%.3f a make), %u lamps, froxel binning %.3f ms a frame, %.1f lamps binned",
				StatFrames, StatBuilds / f, StatBuildMs / f, StatBuilds ? StatBuildMs / StatBuilds : 0.0, StatTableLamps, StatClusterMs / f, StatClusterLamps / f);
			StatFrames = 0; StatBuildMs = 0; StatBuilds = 0; StatClusterMs = 0; StatClusterLamps = StatClusterEntries = StatClusterOver = StatClusterFrames = 0;
			return;
		}
		if (!restoreHooked && !HookWarned) {
			HookWarned = true;
			Logger::Log("[ERROR] UNOFFICIAL ManyLampsOnePass: the shader-constant hooks are not in (see SkipRedundantConstants above); objects lit by many lamps will be too dark. Set [Main.Main.Misc] ManyLampsOnePass = false.");
		}
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = StatFrames;
		char line[1024]; int n = 0;
		n += sprintf_s(line + n, sizeof(line) - n, "MANY LAMPS %u frames, per frame: draws %.0f, lamps per draw", StatFrames, StatDraws / f);
		for (int k = 0; k <= MaxLamps; k++) if (StatByCount[k]) n += sprintf_s(line + n, sizeof(line) - n, " %d:%.1f", k, StatByCount[k] / f);
		n += sprintf_s(line + n, sizeof(line) - n, " | capped many-lamp passes given their other lamps %.1f (%.1f lamps), lamps past %d dropped %.2f",
			StatExtraDraws / f, StatExtras / f, (int)MaxLamps, StatOver / f);
		if (TableMode()) n += sprintf_s(line + n, sizeof(line) - n, " | lamp table %u lamps, lamps not in it %.2f, made %.2f a frame %.3f ms CPU",
			StatTableLamps, StatTableMissing / f, StatBuilds / f, StatBuildMs / f);
		if (TileRowsWanted()) n += sprintf_s(line + n, sizeof(line) - n, " | TABLE LAMPS tiled %.1f a make (%.1f pool)",	// UNOFFICIAL 2026-10-31 TableLamps
			StatBuilds ? (double)StatTiled / StatBuilds : 0.0, StatBuilds ? (double)StatTiledPool / StatBuilds : 0.0);
		StatBuildMs = 0; StatBuilds = StatTiled = StatTiledPool = 0;
		if (ParityDraws) {	// UNOFFICIAL 2026-10-31 TableLampsParity
			Logger::Log("TABLE LAMPS PARITY %u frames: %.1f draws a frame checked, %.1f lamps compared, %.1f the same, differing: tile %.2f position %.2f radius %.2f fade %.2f, a tile in the draw only %.2f, in the table only %.2f, not in the table %.2f; largest position difference %.4f units",
				StatFrames, ParityDraws / f, ParityLamps / f, ParitySame / f, ParityTile / f, ParityPosition / f, ParityRadius / f, ParityFade / f,
				ParityDrawOnly / f, ParityTableOnly / f, ParityNotInTable / f, ParityMaxDiff);
			ParityDraws = ParityLamps = ParitySame = ParityTile = ParityPosition = ParityRadius = ParityFade = ParityDrawOnly = ParityTableOnly = ParityNotInTable = 0;
			ParityMaxDiff = 0.0;
		}
		if (StatClusterFrames) {	// UNOFFICIAL 2026-10-30 ClusteredLamps
			const double cf = (double)StatClusterFrames;
			n += sprintf_s(line + n, sizeof(line) - n, " | CLUSTERED %dx%dx%d froxels: binning %.3f ms CPU, %.1f lamps binned, %.0f list entries (%.2f a froxel), over 31 dropped %.0f",
				TilesX, TilesY, (int)Slices, StatClusterMs / cf, StatClusterLamps / cf, StatClusterEntries / cf, StatClusterEntries / cf / (std::max)(1, TilesX * TilesY * (int)Slices), StatClusterOver / cf);
			StatClusterMs = 0; StatClusterLamps = StatClusterEntries = StatClusterOver = StatClusterFrames = 0;
		}
		Logger::Log("%s", line);
		StatFrames = 0; StatDraws = StatExtraDraws = StatExtras = StatOver = 0; StatTableMissing = 0;
		memset(StatByCount, 0, sizeof(StatByCount));
	}
}
