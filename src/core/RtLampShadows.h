#pragma once

// UNOFFICIAL 2026-10-28 RT lamp shadows ([Shaders.ShadowsInteriors.RT], docs/next/FORK-RT-LAMPS.md). Indoors, each lamp's forward
// shadow (PointShadowForward.hlsl) is read from a traced result instead of its cube map. The tracing is an RT core user (RtCore.h,
// FORK-RT-CORE.md): at the core's point of the frame (after the depth and normals) the helper runs Compute/Rt/RtLampTrace.spv over a
// table of every shadowed lamp of this frame (up to 63, by cube slot); each pixel traces its LampsPerPixel strongest (2026-10-29 v4)
// and the result lands in TESR_RtLampShadow (G32R32F: three cube-slot + visibility entries and a depth code). The object shaders draw
// earlier in the frame, so they use the PREVIOUS frame's result: they reproject into the frame it was traced for and use it where that
// frame saw the same surface; elsewhere the cube map, as before. The lamps are still picked, ranked and given cube slots as before.
//
// POINT_SHADOW_RT is defined at startup only with the setting and the per-object lamp shadows on and a d3d9.dll with the RT core's
// interface (NVR DXVK patch 0028). Without it every shader is exactly what it was. With it, but no ray tracing for the helper (the
// GTX 1070), the core stays off, Mode stays 0 and every lamp keeps its cube map (same picture, the RT shaders' extra branch only).
//
// Per draw (PointShadowForward::SetForPass): c206-c209 the reprojection, c215 mode / tolerance / depth unit / people-from-cube, c216-c217
// the draw's lights' cube slots (when in the table), the result on s15 (the sixth lamp's cube stage: see the shader).

#include <algorithm>
#include <unordered_map>
#include "PointShadowSchedule.h"	// PointShadowSlotState (KeepCube)
#include "LampFixtures.h"
#include "DxvkCompute.h"
#include "RtCore.h"
#include "RenderRect.h"	// UNOFFICIAL 2026-11-03 RenderScale < 1 (docs/next/RT-RENDERSCALE.md)
#include "RtDenoise.h"
#include "RtSunShadows.h"	// UNOFFICIAL 2026-11-04 the shared depth / normals images

namespace RtLampShadows {

	inline constexpr int TableSlots = 63;		// RtLampTrace.comp v4: cube slots 0-62 in the lamp table (63 = "none" in the result)
	inline constexpr int TableWidth = 1024;		// 4 RG32F texels a lamp (2026-10-31: 256 lamps, ManyLights' ids 0-254)
	inline constexpr int ManyIds = 255;			// ManyLights: lamp ids 0-254 (0-62 the cube slots, 64+ the other lamps the draws used)
	inline constexpr const char* kSection = "Shaders.ShadowsInteriors.RT";

	// Instance mask bits the RT core gives the scene (FORK-RT-CORE.md: 0x01 land, 0x02 objects); ours for the lamps' own parts,
	// through the core's mask hook (InstanceMask below): 0x04 lamp glass and glow meshes, 0x08 the rest of a lamp model. The core puts
	// alpha-tested casters under 0x10, non-opaque (agreed scheme 2026-10-29). (2026-11-03 rtalpha) With the core's AlphaTested on the lamp
	// rays test them too (RtAlpha.glsl, the alpha atlas at binding 12 / 13: a tester, "RT exterior shadows don't have alpha textures");
	// without it they are not in the scene's tested set and let lamp light through, as before.
	inline constexpr UINT MaskGlass = 0x04, MaskLampModel = 0x08, MaskAlphaTested = 0x10;

	// ActorsFromCubeMaps applies only while the traced scene has no people: with the RT core's posed actors in it (RtCore::Actors(),
	// mask 0x20, which the lamp rays' masks keep) people cast traced shadows and their cube-map layer is neither read nor redrawn.
	inline bool ActorsFromCubeMaps() {
		return !RtCore::Actors() && TheSettingManager->GetSettingI(kSection, "ActorsFromCubeMaps");
	}

	inline bool CompiledIn = false;			// POINT_SHADOW_RT in the object shaders (fixed at startup)
	// UNOFFICIAL 2026-10-31 CapsuleShadows (core/CapsuleShadows.h): a second writer of the same buffer, a D3D9 pass (people's capsules and
	// contact shadows), no RT needed. RtEnabled: the RT section on at startup with the RT core's interface (the RT path may trace);
	// ScreenCompiledIn: [Shaders.ShadowsInteriors.Capsules] Enabled at startup. ScreenSourceFrame: this frame's buffer is the pass's
	// (set in Submit: the pass is wanted and the RT path is not running).
	inline constexpr const char* kScreenSection = "Shaders.ShadowsInteriors.Capsules";
	inline bool RtEnabled = false, ScreenCompiledIn = false, ScreenSourceFrame = false;
	inline bool ScreenWants() {
		return ScreenCompiledIn && !TheShaderManager->GameState.isExterior &&
			(TheSettingManager->GetSettingI(kScreenSection, "Capsules") != 0 || TheSettingManager->GetSettingI(kScreenSection, "Contact") != 0);
	}
	inline char Status[200] = "not checked yet";

	// The trace pass's uniform block (RtLampTrace.comp, std140, 128 bytes); the lamps are in the lamp table image
	struct PassConstants {
		float size[4], forward[4], right[4], up[4], origin[4], params[4], params2[4];
		float flags[4];							// x: OneSided (diagnostic), y: lamps a pixel traces (LampsPerPixel 1-3), z: Interleave, w: its phase
		float mask[4];							// Exteriors: x EdgeFade, y LampReachMask (TESR_PointShadowNear.y, .z)
		// UNOFFICIAL 2026-10-31 ManyLights only (RtLampTraceMany.comp; the other variants' blocks end above)
		float many[4];							// x reservoirs, y history frames, z spatial radius, w 1 = history valid
		float reproject[4][4];					// scene-space point -> the previous trace's clip space (Reproject)
		float many2[4];							// x frame number, y history depth tolerance
		float jitter[4];						// (2026-11-03) xy the depth's jitter (TAAEffect::DepthJitterUV, screen uv), every variant
		// UNOFFICIAL 2026-11-04 RenderScale < 1 (docs/next/RT-RENDERSCALE.md), ManyLights' two variants: xy the result / history images'
		// full size. Reproject's uv is pre-scaled into the previous trace's rect of the full-size images (Submit), so the history texel
		// is uv x this (not x size, the rect's); the history is only used at the same scale, so that rect is this frame's (size.xy).
		float rect[4];
	};
	static_assert(sizeof(PassConstants) <= 512, "RtCore: 512 bytes of constants a pass");

	// This frame's lamps (Submit, from PointShadowForward::Publish) for Record
	// stop: past the lamp's own glowing part (0 none); intensity: luminance x dimmer (the per-pixel ranking); radius: the game's (Spec.r)
	// light: the ShadowSceneLight (ManyLights' id map; never dereferenced from here)
	struct SlotLamp { int slot; D3DXVECTOR3 position; float reach; float fade; float stop; float intensity; float radius; const void* light = nullptr;
		bool invisible = false; };	// 2026-11-04 invisible: one of the game's invisible lights (LampFixtures::Invisible), traced with InvisibleLightRadius
	inline SlotLamp Lamps[ManyIds] = {};
	inline int LampCount = 0;
	inline bool LampsThisFrame = false;

	// Which lamps are in the trace (v4, 2026-10-29): every published shadow lamp in cube slots 0-62 (MaxLamps caps it); each pixel then
	// traces the ones that light it most. (v3 traced 9 lamps everywhere, picked by how many draws used them: Vault 21's 64 shadow lamps
	// needed more.) TracedMap: which cube slots the newest result's table held, so the draws only look up lamps it can have.
	inline constexpr int MapSlots = 64;
	inline int PendingMap[MapSlots];		// this frame's table: cube slot -> the slot (in the table) or -1
	inline int TracedMap[MapSlots];		// the newest result's

	// The camera of the frame the newest result was traced for (Record) and whether it exists
	struct TracedCamera { D3DXMATRIX view; D3DXVECTOR3 location;	// location: the traced frame's eye in world space
		 float p11, p22, farZ; bool valid; float sx = 1.0f, sy = 1.0f, jx = 0.0f, jy = 0.0f; };	// sx, sy (2026-11-03): the RenderRect scale it was
		// traced at; jx, jy: the depth's jitter then (screen uv)
	inline TracedCamera Traced = {};
	inline TracedCamera Pending = {};

	// Per draw
	inline float Mode = 0.0f;				// c215.x: 0 cube maps, 1 traced, 2 compare view, 3 coverage view, 4 one lamp (DebugView 3)
	inline float DebugLamp = 0.0f;			// c217.z: DebugView 3's lamp (cube slot)
	inline int MaxLamps = TableSlots;		// lamps in the table
	inline float Reproject[4][4] = {};		// c206-c209
	inline float RtConstant[4] = {};		// c215
	inline float Uploaded[7][4] = {};
	inline bool UploadedValid = false;

	inline int imageDepth = -1, imageNormals = -1, imageResult = -1, imageTable = -1, shader = -1;
	inline int imageAlphaStandIn = -1;	// (2026-11-03 rtalpha) 1 x 1 R16F: the alpha atlas binding when the core has no atlas (jitter.z 0)
	// the alpha atlas when the core tests alpha (RtCore::AlphaTested), else the stand-in; Record sets jitter.z to match
	inline int AlphaImage() {
		const int atlas = RtCore::AlphaAtlasImage();
		if (RtCore::AlphaTested() && atlas >= 0) return atlas;
		return imageAlphaStandIn >= 0 ? imageAlphaStandIn : imageDepth;	// (no stand-in: any image; jitter.z 0, never read)
	}
	inline bool AlphaOn() { const int atlas = RtCore::AlphaAtlasImage(); return RtCore::AlphaTested() && atlas >= 0; }
	inline int imageRaw = -1, filterShader = -1;	// Filter: the trace writes imageRaw, RtLampFilter.comp imageResult
	// UNOFFICIAL 2026-10-29 Denoise: RtLampTraceSplit.spv also writes ranks 0 and 1 apart (visibility, cube slot) for the shared
	// RtDenoise (run per rank with the slot as its id); RtLampRepack.frag writes the result again with them (RepackTexture -> s15)
	inline int imageSplit[4] = { -1, -1, -1, -1 }, splitShader = -1;	// vis0, vis1, id0, id1 (R32F)
	inline IDirect3DTexture9* SplitTexture[4] = {};
	inline IDirect3DSurface9* SplitSurface[4] = {};
	inline IDirect3DTexture9* DenoisedTexture[2] = {};
	inline IDirect3DSurface9* DenoisedSurface[2] = {};
	inline IDirect3DTexture9* RepackTexture = nullptr;
	inline IDirect3DSurface9* RepackSurface = nullptr;
	inline bool SplitThisFrame = false, UseRepacked = false;
	// UNOFFICIAL 2026-10-29 Exteriors ([Shaders.ShadowsInteriors.RT] Exteriors, off): outdoors at night the lamps of the screen-space
	// lamp mask (ShadowManager's shadow lamps + tracked lamps) go into the table and RtLampTraceExterior.spv writes the mask itself
	// (traced visibility instead of cube maps); ApplyExteriorMask puts it in the shadow buffer's green (PointShadows.fx's output).
	inline int imageMask = -1, exteriorShader = -1;
	inline IDirect3DTexture9* MaskTexture = nullptr;
	inline IDirect3DSurface9* MaskSurface = nullptr;
	inline bool ExteriorFrame = false, MaskThisFrame = false, ExteriorLive = false;
	// UNOFFICIAL 2026-10-30 ExteriorForward ([Shaders.ShadowsInteriors.RT] ExteriorForward, off, needs a restart): outdoors the lamps'
	// shadows inside each object's own lighting, as indoors: the lit pixel shaders get an outdoor record with INTERIOR_SHADOWS +
	// POINT_SHADOW_RT_ONLY (ShaderManager::LoadShader, PointShadowForward::ExteriorRecords), the table holds ShadowManager's shadow
	// lamps by their index (PointShadowForward::PublishExterior gives the draws the same ids) and the per-pixel trace runs as indoors.
	inline bool ExteriorForwardFrame = false;
	// UNOFFICIAL 2026-10-30 the land (TerrainTemplate TERRAIN_LAMP_RT): each slot's world position in the traced frame (w 1 = traced),
	// matched there by position (outdoor slots are ShadowLights indices, which may change order from frame to frame)
	inline D3DXVECTOR4 TracedLampWorld[TableSlots] = {};
	inline bool ExteriorForwardOn() { return TheSettingManager->GetSettingI(kSection, "ExteriorForward") != 0; }
	// UNOFFICIAL 2026-10-30 Flashlight ([Shaders.ShadowsInteriors.RT] Flashlight, off): the flashlight's (Pip-Boy light's) shadow traced
	// (RtFlashTrace.comp, 1 lit / 0 blocked, screen space); Flashlight's shadow pass reads it instead of its shadow map
	// (FlashlightShadowsRT.frag), and the shadow map is not drawn meanwhile.
	inline int imageFlash = -1, flashShader = -1;
	inline IDirect3DTexture9* FlashTexture = nullptr;
	inline IDirect3DSurface9* FlashSurface = nullptr;
	inline bool FlashThisFrame = false, FlashLive = false;
	inline bool FlashWanted() {
		FlashlightEffect* flash = TheShaderManager->Effects.Flashlight;
		return CompiledIn && imageFlash >= 0 && flashShader >= 0 && flash && flash->Enabled && flash->spotLightActive &&
			TheShaderManager->SpotLightPosition[0].w > 0.0f && TheSettingManager->GetSettingI(kSection, "Flashlight") &&
			TheSettingManager->GetSettingI(kSection, "Live");
	}
	// Flashlight.cpp ExtRender: this frame's traced flashlight shadow, or null (then its shadow map, as before)
	inline IDirect3DBaseTexture9* FlashlightShadow() { return FlashThisFrame && FlashTexture ? FlashTexture : nullptr; }
	// ShadowManager: the flashlight's shadow map is not drawn while the trace stands in for it
	inline bool FlashlightReplacing() { return FlashLive && RtCore::Running(); }
	// UNOFFICIAL 2026-10-31 ManyLights ([Shaders.ShadowsInteriors.RT] ManyLights, off, needs a restart; docs/next/RT-MANY-LIGHTS.md):
	// indoors every lamp a draw used goes into the table (the cube-slot lamps at their slot, the others at ids 64+, noted by
	// PointShadowForward::SetForPass), and RtLampTraceMany.spv traces each pixel's two strongest exactly and all its other lamps as one
	// reservoir-sampled ratio R (reused over neighbours and frames); a draw's lamp that is not one of the two takes R. The result's
	// packing differs (unit 4096: ids 0-254): the draws get the unit in c217.w.
	inline int imageHistory[2] = { -1, -1 }, manyShader = -1;
	// UNOFFICIAL 2026-10-31 ManyLightsOneRay (RtLampTraceRestir.comp): one shadow ray a pixel; its lamp ids + counts history pair and
	// ObjectMotion's buffer (made at the helper's start with ManyLights and ManyLightsOneRay on)
	inline int imageHistoryIds[2] = { -1, -1 }, imageMotion = -1, restirShader = -1;
	inline bool TracedOneRay = false;		// the history pair holds the one-ray variant's (V0 V1 R) rather than RtLampTraceMany's R
	inline bool OneRayReady() { return restirShader >= 0 && imageHistoryIds[0] >= 0 && imageHistoryIds[1] >= 0 && imageMotion >= 0; }
	inline UInt32 MotionFrames = 0;			// one-ray traces that had ObjectMotion's buffer (the log)
	inline int HistoryParity = 0;
	inline bool TracedMany = false;			// the newest result is in the ManyLights packing (and the history pair holds its R)
	struct ManyId { int id; float reach; };
	inline std::unordered_map<const void*, ManyId> PendingIds, TracedIds;	// light -> id: this frame's table / the newest result's
	inline int ExtraCount = 0;				// lamps noted at ids 64+ this frame
	inline int RefusedCount = 0;			// lamps the draws used past ManyLightsMax this frame (no id: cube map or unshadowed)
	// UNOFFICIAL 2026-11-04 stable ids: a lamp keeps its id 64+ from frame to frame (the one-ray trace matches its history by id; ids
	// handed out first-come each frame changed whenever the draw order did, and the lamp's history was lost). An id stays its lamp's
	// for IdHold frames after it was last noted; a new lamp takes the lowest free id, else the one unused longest.
	inline constexpr UInt32 IdHold = 120;
	inline const void* IdOwner[ManyIds] = {};
	inline UInt32 IdLastFrame[ManyIds] = {};
	inline UInt32 NoteFrame = 1;			// Submit's count
	inline int TableTop = 64;				// this frame's highest noted id + 1: the trace's table length
	inline std::unordered_map<const void*, int> StableIds;
	inline void ForgetIds() {
		StableIds.clear();
		for (int i = 0; i < ManyIds; i++) { IdOwner[i] = nullptr; IdLastFrame[i] = 0; }
	}
	inline UInt32 ManyTraces = 0, ManyLogCell = 0, ManyCellTraces = 0;	// the collected-lamps log (Record)
	inline bool ManyNoting = false;			// this frame's draws note their lamps (Submit: ManyLights on, indoors)
	inline bool ManyReady() { return CompiledIn && manyShader >= 0 && imageHistory[0] >= 0 && imageHistory[1] >= 0; }
	// UNOFFICIAL 2026-11-03 (rtext) ExteriorManyLights: outdoors with ExteriorForward the same packing (2 exact lamps a pixel + R, the traced
	// share of all the others): a lamp that is not one of a pixel's 2 strongest takes R instead of staying lit (Novac: the walkway lamps were
	// never a pixel's 2 strongest, rtlamp-31..35). ExteriorSubmit: SubmitExterior's Submit is running.
	inline bool ExteriorSubmit = false;
	inline bool ExteriorManyOn() { return TheSettingManager->GetSettingI(kSection, "ExteriorManyLights") != 0; }
	inline bool ManyOn() {
		if (!ManyReady()) return false;
		if (TheShaderManager->GameState.isExterior) return ExteriorSubmit && ExteriorManyOn();
		return TheSettingManager->GetSettingI(kSection, "ManyLights") != 0;
	}
	// c217.w: 1 / the packing unit of the result the draws read
	inline float PackingInverse() { return TracedMany ? 1.0f / 4096.0f : 1.0f / 1024.0f; }
	// SetForPass, ManyLights: the newest result's id for this light (-1 none) and its reach
	inline int ManyIdOf(const void* light, float* reach = nullptr) {
		if (!TracedMany || Mode <= 0.0f) return -1;
		auto found = TracedIds.find(light);
		if (found == TracedIds.end()) return -1;
		if (reach) *reach = found->second.reach;
		return found->second.id;
	}
	// SetForPass, ManyLights: a draw's point light into the next trace's table (once a frame each; its data copied now)
	inline bool Noting(const void* light) { return ManyNoting && !PendingIds.count(light); }
	inline void NoteLamp(const void* light, const SlotLamp& data) {
		if (!Noting(light)) return;
		const int limit = (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "ManyLightsMax"), 64), ManyIds);
		int id = -1;
		if (LampCount < ManyIds) {
			auto kept = StableIds.find(light);
			if (kept != StableIds.end() && kept->second < limit && IdOwner[kept->second] == light) id = kept->second;
			else {
				if (kept != StableIds.end()) {	// its old id is past ManyLightsMax now (or was taken): let it go
					if (kept->second >= 0 && kept->second < ManyIds && IdOwner[kept->second] == light) IdOwner[kept->second] = nullptr;
					StableIds.erase(kept);
				}
				int oldest = -1;
				for (int i = 64; i < limit; i++) {
					if (!IdOwner[i] || NoteFrame - IdLastFrame[i] > IdHold) { id = i; break; }
					if (IdLastFrame[i] != NoteFrame && (oldest < 0 || NoteFrame - IdLastFrame[i] > NoteFrame - IdLastFrame[oldest])) oldest = i;
				}
				if (id < 0) id = oldest;
				if (id >= 0) {
					if (IdOwner[id]) {
						auto owner = StableIds.find(IdOwner[id]);
						if (owner != StableIds.end() && owner->second == id) StableIds.erase(owner);
					}
					IdOwner[id] = light;
					StableIds[light] = id;
				}
			}
		}
		if (id < 0) {
			PendingIds.emplace(light, ManyId{ -1, data.reach });	// counted once (ManyIdOf: -1, no id)
			RefusedCount++;
			return;
		}
		IdLastFrame[id] = NoteFrame;
		ExtraCount++;
		TableTop = (std::max)(TableTop, id + 1);
		Lamps[LampCount] = data;
		Lamps[LampCount].slot = id;
		Lamps[LampCount].light = light;
		LampCount++;
		PendingIds.emplace(light, ManyId{ id, data.reach });
	}
	inline IDirect3DTexture9* TableTexture = nullptr;		// the lamp table as the GPU sees it (copied into imageTable each frame)
	inline IDirect3DSurface9* TableSurface = nullptr;
	inline IDirect3DTexture9* TableStaging = nullptr;		// written by the CPU (system memory), UpdateSurface into TableTexture
	inline IDirect3DTexture9* ResultTexture = nullptr;
	inline IDirect3DSurface9* ResultSurface = nullptr;
	inline UINT TextureWidth = 0, TextureHeight = 0;
	inline UInt32 FramesTraced = 0;

	// Our part of the instance mask (MaskHook below): lamp glass and glow meshes (LampFixtures::LampPart), the rest of a
	// lamp model listed in the cell (fixtures, shades, chandeliers, fans); 0 = the core's own (land / object).
	inline UInt32 InstanceMask(NiGeometry* geometry, bool actor) {
		if (actor || !geometry || TheShaderManager->GameState.isExterior) return 0;
		if (LampFixtures::LampPart(geometry)) return MaskGlass;
		if (LampFixtures::ModelIndexOf(geometry) >= 0) return MaskLampModel;
		return 0;
	}

	// RenderManager, right after DxvkCompute::Initialize and before any shader is loaded: decides POINT_SHADOW_RT for the session.
	inline void Initialize() {
		CompiledIn = false;
		RtEnabled = false;
		for (int i = 0; i < MapSlots; i++) PendingMap[i] = TracedMap[i] = -1;
		ID3D9NvrExtRt* rt = nullptr;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!TheSettingManager->GetSettingI(kSection, "Enabled"))
			strcpy_s(Status, "off ([Shaders.ShadowsInteriors.RT] Enabled false at startup)");
		else if (!TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled"))
			strcpy_s(Status, "off: needs the per-object lamp shadows ([Shaders.ShadowsInteriors.Forward] Enabled)");
		else if (!ext || FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtRt), (void**)&rt)) || !rt)
			strcpy_s(Status, "off: needs Vegas Shaders DXVK with the RT core (patch 0028)");
		else {
			rt->Release();
			CompiledIn = true;
			RtEnabled = true;
			strcpy_s(Status, "lamp shaders compiled to read traced lamp shadows (used once the RT helper runs)");
		}
		if (TheSettingManager->GetSettingI(kSection, "Enabled")) Logger::Log("UNOFFICIAL RT lamp shadows: %s", Status);
		// UNOFFICIAL 2026-10-31 CapsuleShadows: the same shader reader (POINT_SHADOW_RT), its buffer from a D3D9 pass (any GPU)
		ScreenCompiledIn = TheSettingManager->GetSettingI(kScreenSection, "Enabled") != 0 &&
			TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled") != 0;
		if (ScreenCompiledIn) CompiledIn = true;
		if (TheSettingManager->GetSettingI(kScreenSection, "Enabled"))
			Logger::Log("UNOFFICIAL CapsuleShadows: %s", ScreenCompiledIn ? "lamp shaders compiled to read the capsule and contact shadows (POINT_SHADOW_RT)" :
				"off: needs the per-object lamp shadows ([Shaders.ShadowsInteriors.Forward] Enabled)");
	}

	inline const D3DXVECTOR4* Constant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
	inline IDirect3DBaseTexture9* Texture(const char* name) {
		auto found = TheTextureManager->TextureNames.find(name);
		return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
	}

	// UNOFFICIAL 2026-11-04: forget the newest result (cell change, helper start): the draws use the cube maps until the next trace
	inline UInt32 SubmitCell = 0;
	inline void ForgetTraced() {
		Traced.valid = false;
		for (int i = 0; i < MapSlots; i++) TracedMap[i] = -1;
		TracedIds.clear();
		TracedMany = false;
		ForgetIds();
	}

	// PointShadowForward::Publish, once a frame indoors after the slots are known (before the world draws): this frame's lamps for
	// the trace, and the reprojection into the newest result for this frame's draws.
	inline void Submit(const SlotLamp* lamps, int count) {
		Mode = 0.0f;
		LampCount = 0;
		LampsThisFrame = false;
		PendingIds.clear();
		RefusedCount = 0;
		ExtraCount = 0;
		TableTop = 64;
		NoteFrame++;
		ManyNoting = false;
		ScreenSourceFrame = false;
		if (!CompiledIn) return;
		// UNOFFICIAL 2026-11-04: a new interior cell (a load door) drops the newest result and the ids: the lamps are other lamps there
		{
			const UInt32 cell = TheShaderManager->GameState.isExterior ? 0xFFFFFFFF : (Player && Player->parentCell ? Player->parentCell->refID : 0);
			if (cell != SubmitCell) { SubmitCell = cell; ForgetTraced(); }
		}
		MaxLamps = (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "MaxLamps"), 0), TableSlots);
		const bool many = ManyOn() && TheSettingManager->GetSettingI(kSection, "Live") != 0;
		// The table: every published lamp in cube slots 0-62, in NVR's order (its ranking), up to MaxLamps
		for (int i = 0; i < MapSlots; i++) PendingMap[i] = -1;
		for (int i = 0; i < count && LampCount < MaxLamps; i++)
			if (lamps[i].slot >= 0 && lamps[i].slot < TableSlots) {
				PendingMap[lamps[i].slot] = lamps[i].slot;
				Lamps[LampCount++] = lamps[i];
				if (many && lamps[i].light) PendingIds.emplace(lamps[i].light, ManyId{ lamps[i].slot, lamps[i].reach });
			}
		ManyNoting = many;
		LampsThisFrame = RtEnabled && TheSettingManager->GetSettingI(kSection, "Live") != 0;
		// UNOFFICIAL 2026-10-31 CapsuleShadows: the pass writes the buffer when the RT path is not running (no RT GPU, or RT off)
		const bool rtSource = RtEnabled && RtCore::Running();
		if (!rtSource && ScreenWants() && count > 0) { ScreenSourceFrame = true; LampsThisFrame = true; }

		// The newest result: traced last frame (Record moves Pending to Traced once its copy-out is recorded)
		if (!Traced.valid || !ResultTexture || !LampsThisFrame || !(rtSource || ScreenSourceFrame)) return;
		const D3DXMATRIX& v = Traced.view;
		const D3DXVECTOR3 right(v._11, v._21, v._31), up(v._12, v._22, v._32), forward(v._13, v._23, v._33);
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		// A camera-relative point of this frame, seen from the traced frame's eye (that frame's CameraPosition: the eye is the scene's
		// origin, RtCore::Eye; 2026-10-29: Pointers::Generic::CameraLocation reads ~0 in game, which put the eye at the world origin)
		const D3DXVECTOR3 d(camera.x - Traced.location.x, camera.y - Traced.location.y, camera.z - Traced.location.z);
		const float rows[3][3] = { { right.x, up.x, forward.x }, { right.y, up.y, forward.y }, { right.z, up.z, forward.z } };
		for (int i = 0; i < 3; i++) {
			Reproject[i][0] = rows[i][0] * Traced.p11;
			Reproject[i][1] = rows[i][1] * Traced.p22;
			Reproject[i][2] = 0.0f;
			Reproject[i][3] = rows[i][2];
		}
		Reproject[3][0] = D3DXVec3Dot(&d, &right) * Traced.p11;
		Reproject[3][1] = D3DXVec3Dot(&d, &up) * Traced.p22;
		Reproject[3][2] = 0.0f;
		Reproject[3][3] = D3DXVec3Dot(&d, &forward);
		// (2026-11-03) RenderScale < 1: the traced result is in the top-left sx x sy part of its texture: the forward shaders' uv
		// (clip.xy / w x (0.5, -0.5) + 0.5) is scaled into it by the matrix itself (x' = sx x + (sx - 1) w, y' = sy y - (sy - 1) w gives
		// uv' = s uv), no shader change; points past the part read stale texels whose depth code does not match: the cube map.
		// (2026-11-03) and the traced frame's jitter off (its texel at uv shows the ray through uv + j): uv' = s (uv - j), i.e.
		// x' = s x + (s - 1 - 2 s jx) w, y' = s y - (s - 1 - 2 s jy) w. Without it the depth codes missed across whole floors with TAA /
		// FSR / DLSS on (rtpt-rs5 coverage: ~75 % red at RenderScale 1 and 0.67) and the cube maps were used there.
		if (Traced.sx != 1.0f || Traced.sy != 1.0f || Traced.jx != 0.0f || Traced.jy != 0.0f)
			for (int i = 0; i < 4; i++) {
				Reproject[i][0] = Traced.sx * Reproject[i][0] + (Traced.sx - 1.0f - 2.0f * Traced.sx * Traced.jx) * Reproject[i][3];
				Reproject[i][1] = Traced.sy * Reproject[i][1] - (Traced.sy - 1.0f - 2.0f * Traced.sy * Traced.jy) * Reproject[i][3];
			}
		RtConstant[1] = (std::max)(TheSettingManager->GetSettingF(kSection, "DepthTolerance"), 0.004f);	// at least the depth code's step
		RtConstant[2] = log2f((std::max)(Traced.farZ, 2.0f)) / 4095.0f;
		RtConstant[3] = ScreenSourceFrame || ActorsFromCubeMaps() ? 1.0f : 0.0f;	// the capsule / contact buffer: always the darker of it and the lamp's map
		const int debugView = TheSettingManager->GetSettingI(kSection, "DebugView");
		// DebugView 4 (2026-10-31 ManyLights): R, the other lamps' traced share (x 5)
		Mode = debugView == 1 ? 2.0f : debugView == 2 ? 3.0f : debugView == 3 ? 4.0f : debugView == 4 ? 5.0f : 1.0f;
		DebugLamp = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "DebugLamp"), 0), TableSlots - 1);
		RtConstant[0] = Mode;
	}

	// What a draw's light looks up in the traced result (its cube slot, when the newest result's table had it), -1 for its cube map.
	inline float TracedSlot(int cubeSlot) {
		return Mode > 0.0f && cubeSlot >= 0 && cubeSlot < MapSlots && TracedMap[cubeSlot] >= 0 ? (float)cubeSlot : -1.0f;
	}

	// UNOFFICIAL 2026-10-29 SkipTracedCubes ([Shaders.ShadowsInteriors.RT], on): the cost win. A lamp the trace covered last frame
	// keeps the cube map it has (the fallback where the traced frame did not see a point) instead of redrawing it: no redraw for the
	// schedule, moved casters or a moved light. Drawn as before when it has no cube map of its own yet (new lamp, new cell, another
	// light in the slot, another radius, a partial one), and, with ActorsFromCubeMaps on, while people move near it (their shadows
	// come from the cube map). ShadowManager's cube loop asks before drawing slot i.
	inline UInt32 SkippedCubes = 0, SkippedCubesFrames = 0;
	inline bool KeepCube(int slot, const PointShadowSlotState& last, const PointShadowSlotState& now) {
		if (!CompiledIn || ScreenSourceFrame || Mode <= 0.0f || slot < 0 || slot >= MapSlots || TracedMap[slot] < 0) return false;	// (the capsule buffer replaces no map)
		if (!TheSettingManager->GetSettingI(kSection, "SkipTracedCubes")) return false;
		if (!last.valid || last.light != now.light || last.cell != now.cell || last.texture != now.texture ||
			last.radius != now.radius) return false;
		// A partial cube map (CubeFaceViewCull left the out-of-view faces of a people refresh as they were) is only the fallback here;
		// redrawing it for that made a loop: each people refresh is partial again (rtlamp-10: Vault 21 7 refreshes a frame, 8.1 ms)
		if (last.partial && ActorsFromCubeMaps()) return false;
		if (!now.staticCasters && ActorsFromCubeMaps()) return false;
		SkippedCubes++;
		return true;
	}

	// PointShadowForward::SetForPass: slots = the draw's lights' TracedSlot (-1 none).
	inline void SetForPass(const float slots[6], bool force) {
		if (!CompiledIn) return;
		float c[7][4] = {};
		memcpy(c, Reproject, sizeof(Reproject));
		memcpy(c[4], RtConstant, sizeof(RtConstant));
		c[4][0] = Mode;
		memcpy(c[5], slots, 4 * sizeof(float));
		c[6][0] = slots[4]; c[6][1] = slots[5]; c[6][2] = DebugLamp; c[6][3] = PackingInverse();	// z: DebugView 3's lamp; w: the result's packing
		if (force || !UploadedValid || memcmp(c, Uploaded, sizeof(c))) {
			// (outdoors the forward records read it at c200: POINT_SHADOW_RT_ONLY; c206-c209 are the character shadow map's there)
			TheRenderManager->device->SetPixelShaderConstantF(TheShaderManager->GameState.isExterior ? 200 : 206, &c[0][0], 4);
			TheRenderManager->device->SetPixelShaderConstantF(215, &c[4][0], 3);
			memcpy(Uploaded, c, sizeof(c));
			UploadedValid = true;
		}
	}

	// s15 in POINT_SHADOW_RT shaders: the result, or a 1x1 "nothing traced" (depth code 0 never matches) before there is one
	inline IDirect3DBaseTexture9* Stage15Texture() {
		if (Mode > 0.0f && UseRepacked && RepackTexture) return RepackTexture;
		if (Mode > 0.0f && ResultTexture) return ResultTexture;
		static IDirect3DTexture9* neutral = nullptr;
		if (!neutral && SUCCEEDED(TheRenderManager->device->CreateTexture(1, 1, 1, 0, D3DFMT_G32R32F, D3DPOOL_MANAGED, &neutral, NULL))) {
			D3DLOCKED_RECT lock;
			if (SUCCEEDED(neutral->LockRect(0, &lock, NULL, 0))) {
				float* texel = (float*)lock.pBits;
				texel[0] = 1023.0f + 1024.0f * 1023.0f; texel[1] = 1023.0f;	// no entries, depth code 0 (never matches)
				neutral->UnlockRect(0);
			}
		}
		return neutral;
	}

	// --- The RT core user -----------------------------------------------------------------------------------------------------------
	inline bool Wants() {
		return (RtEnabled && !ScreenSourceFrame && LampsThisFrame && LampCount > 0 && (!TheShaderManager->GameState.isExterior || ExteriorFrame)) || FlashWanted();
	}

	// Exteriors: ShadowManager, every frame outdoors right after GetNearbyLights: the mask's lamps (the shadow lamps, then the tracked
	// ones) into the table, with the mask's own radius (Spec.r x LightRadiusMult) and weight (luma x dimmer)
	inline void SubmitExterior(ShadowSceneLight* const* shadowLights, int shadowCount, NiPointLight* const* lights, int lightCount) {
		ExteriorFrame = false;
		ExteriorForwardFrame = false;
		const bool forward = CompiledIn && ExteriorForwardOn();
		const bool maskMode = !forward && imageMask >= 0 && exteriorShader >= 0 && TheSettingManager->GetSettingI(kSection, "Exteriors");
		if (!CompiledIn || (!forward && !maskMode)) { ExteriorLive = false; return; }
		const float mult = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.LightRadiusMult;
		// outdoors the lamps' own heads are not told apart from other objects (LampFixtures is the interior cell's list): the rays stop
		// this far short of each light, past a street lamp's or a lantern's own housing
		const float stop = (std::max)(TheSettingManager->GetSettingF(kSection, "ExteriorLampStop"), 0.0f);
		SlotLamp list[TableSlots];
		int n = 0;
		auto add = [&](NiPointLight* light) {
			if (!light || n >= TableSlots) return;
			const D3DXVECTOR3 at(light->m_worldTransform.pos.x, light->m_worldTransform.pos.y, light->m_worldTransform.pos.z);
			for (int i = 0; i < n; i++) if (list[i].position == at) return;
			const float reach = light->Spec.r * mult;
			const float weight = (0.2126f * light->Diff.r + 0.7152f * light->Diff.g + 0.0722f * light->Diff.b) * light->Dimmer;
			if (!(reach > 1.0f) || !(weight > 0.0f)) return;
			list[n] = { n, at, reach, 1.0f, stop, weight, reach };
			n++;
		};
		if (forward) {
			// ExteriorForward: the shadow lamps only (the draws match them by pointer), each by its index in ShadowLights = its slot
			for (int i = 0; i < shadowCount && i < TableSlots; i++) {
				const NiPointLight* light = shadowLights[i] ? shadowLights[i]->sourceLight : nullptr;
				if (!light) continue;
				const float reach = light->Spec.r * mult;
				const float intensity = (0.2126f * light->Diff.r + 0.7152f * light->Diff.g + 0.0722f * light->Diff.b) * light->Dimmer;
				if (!(reach > 1.0f)) continue;
				list[n++] = { i, D3DXVECTOR3(light->m_worldTransform.pos.x, light->m_worldTransform.pos.y, light->m_worldTransform.pos.z),
					reach, 1.0f, stop, (std::max)(intensity, 0.001f), light->Spec.r, shadowLights[i] };	// (rtext: the light, ManyLights' id map)
			}
			ExteriorSubmit = true;
			Submit(list, n);	// keeps the reprojection and Mode: the lamp shaders read the trace as indoors
			ExteriorSubmit = false;
			ExteriorForwardFrame = ExteriorFrame = LampsThisFrame && LampCount > 0;
			ExteriorLive = false;
			return;
		}
		for (int i = 0; i < shadowCount; i++) if (shadowLights[i]) add(shadowLights[i]->sourceLight);
		for (int i = 0; i < lightCount; i++) add(lights[i]);
		Submit(list, n);
		Mode = 0.0f;	// outdoors nothing reads the reprojected result
		ExteriorFrame = LampsThisFrame && LampCount > 0;
		if (!LampsThisFrame) ExteriorLive = false;
	}
	// ShadowManager: skip the outdoor cube maps while the traced mask stands in for them
	inline bool ExteriorReplacing() { return (ExteriorLive || ExteriorForwardFrame) && TheShaderManager->GameState.isExterior && RtCore::Running(); }

	inline void Setup() {
		ForgetTraced();	// 2026-11-04: a (re)started helper has none of the old images' contents
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		// UNOFFICIAL 2026-11-04: the depth and normals the RT sun shadows registered (the same screen-size copies of the same buffers,
		// R32F and RGBA16F; its Setup runs first), so the two effects take 2 of the core's 32 shared images, not 4 (with the sun's
		// blocker history the set was 33: the core refused to start and no RT effect ran)
		const int sunDepth = RtSunShadows::DepthImage(), sunNormals = RtSunShadows::NormalsImage();
		imageDepth = sunDepth >= 0 ? sunDepth : RtCore::AddImage("NVR RT lamps depth", w, h, NVR_EXT_SHARED_R32F);
		imageNormals = sunNormals >= 0 ? sunNormals : RtCore::AddImage("NVR RT lamps normals", w, h, NVR_EXT_SHARED_RGBA16F);
		imageResult = RtCore::AddImage("NVR RT lamps", w, h, NVR_EXT_SHARED_RG32F);
		imageTable = RtCore::AddImage("NVR RT lamp table", TableWidth, 1, NVR_EXT_SHARED_RG32F);
		imageAlphaStandIn = RtCore::AddImage("NVR RT lamps alpha stand-in", 1, 1, NVR_EXT_SHARED_R16F);	// (2026-11-03 rtalpha)
		// optional images only when asked for at the helper's start (the core holds 32 shared images for every RT effect)
		imageRaw = TheSettingManager->GetSettingI(kSection, "Filter") ? RtCore::AddImage("NVR RT lamps unfiltered", w, h, NVR_EXT_SHARED_RG32F) : -1;
		static const char* splitNames[4] = { "NVR RT lamps vis 0", "NVR RT lamps vis 1", "NVR RT lamps id 0", "NVR RT lamps id 1" };
		const bool denoise = TheSettingManager->GetSettingI(kSection, "Denoise") != 0;
		for (int i = 0; i < 4; i++) imageSplit[i] = denoise ? RtCore::AddImage(splitNames[i], w, h, NVR_EXT_SHARED_R32F) : -1;
		imageMask = TheSettingManager->GetSettingI(kSection, "Exteriors") ? RtCore::AddImage("NVR RT lamps mask", w, h, NVR_EXT_SHARED_R32F) : -1;
		imageFlash = TheSettingManager->GetSettingI(kSection, "Flashlight") ? RtCore::AddImage("NVR RT flashlight", w, h, NVR_EXT_SHARED_R32F) : -1;
		// (2026-11-03 rtext) also for ExteriorManyLights (outdoors with ExteriorForward)
		const bool many = TheSettingManager->GetSettingI(kSection, "ManyLights") != 0 ||
			(TheSettingManager->GetSettingI(kSection, "ExteriorForward") != 0 && TheSettingManager->GetSettingI(kSection, "ExteriorManyLights") != 0);
		imageHistory[0] = many ? RtCore::AddImage("NVR RT lamps rest history 0", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		imageHistory[1] = many ? RtCore::AddImage("NVR RT lamps rest history 1", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		const bool oneRay = many && TheSettingManager->GetSettingI(kSection, "ManyLightsOneRay") != 0;
		imageHistoryIds[0] = oneRay ? RtCore::AddImage("NVR RT lamps ids history 0", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		imageHistoryIds[1] = oneRay ? RtCore::AddImage("NVR RT lamps ids history 1", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		imageMotion = oneRay ? RtCore::AddImage("NVR RT lamps motion", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		TracedMany = false;
	}

	// The lamp table into its GPU texture (scene space = world minus CameraPosition), for the copy into imageTable
	inline bool WriteTable(const D3DXVECTOR4& camera, float discRadius, float invisibleRadius, float stopCap = 0.0f) {
		IDirect3DDevice9* device = TheRenderManager->device;
		if (!TableTexture) {
			TheTextureManager->InitTexture("TESR_RtLampTable", &TableTexture, &TableSurface, TableWidth, 1, D3DFMT_G32R32F);
			if (!TableTexture) return false;
		}
		if (!TableStaging && FAILED(device->CreateTexture(TableWidth, 1, 1, 0, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &TableStaging, NULL))) {
			TableStaging = nullptr;
			return false;
		}
		D3DLOCKED_RECT lock;
		if (FAILED(TableStaging->LockRect(0, &lock, NULL, 0))) return false;
		float* t = (float*)lock.pBits;
		memset(t, 0, TableWidth * 2 * sizeof(float));		// reach 0 = no lamp
		for (int i = 0; i < LampCount; i++) {
			const SlotLamp& l = Lamps[i];
			if (l.slot < 0 || l.slot >= ManyIds) continue;
			float* e = t + 8 * l.slot;
			e[0] = l.position.x - camera.x; e[1] = l.position.y - camera.y;
			e[2] = l.position.z - camera.z; e[3] = l.reach;
			e[4] = (std::max)(l.intensity, 0.001f); e[5] = (std::max)(l.radius, 1.0f);
			// 2026-11-04: GlowStopMax caps the stop from the lamp's glow (it can reach 40-80 units: shelves and ceilings by the lamp
			// never blocked it); the invisible flag, not a negative fade (that only came with [Forward] InvisibleLightSize > 0)
			e[6] = stopCap > 0.0f ? (std::min)(l.stop, stopCap) : l.stop;
			e[7] = l.invisible && invisibleRadius >= 0.0f ? invisibleRadius : discRadius;
		}
		TableStaging->UnlockRect(0);
		IDirect3DSurface9* staging = nullptr;
		if (FAILED(TableStaging->GetSurfaceLevel(0, &staging))) return false;
		const HRESULT hr = device->UpdateSurface(staging, NULL, TableSurface, NULL);
		staging->Release();
		return SUCCEEDED(hr);
	}

	inline void Record(IDirect3DDevice9* Device) {
		if (imageDepth < 0 || imageNormals < 0 || imageResult < 0 || imageTable < 0 || shader < 0) return;
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		// (2026-11-03) RenderScale < 1: trace only the frame's top-left RenderRect part (uv = pixel / rect size is the screen's; the
		// depth is read in the rect); the forward shaders read it through Reproject's scaled uv. Off: the whole screen, as before.
		const UINT tw = RenderRect::Width(w), th = RenderRect::Height(h);
		if (!ResultTexture || TextureWidth != w || TextureHeight != h) {
			if (ResultSurface) { ResultSurface->Release(); ResultSurface = nullptr; }
			if (ResultTexture) { ResultTexture->Release(); ResultTexture = nullptr; }
			Traced.valid = false;
			for (int i = 0; i < MapSlots; i++) TracedMap[i] = -1;
			TheTextureManager->InitTexture("TESR_RtLampShadow", &ResultTexture, &ResultSurface, w, h, D3DFMT_G32R32F);
			TextureWidth = w;
			TextureHeight = h;
			if (!ResultTexture) return;
		}
		IDirect3DBaseTexture9* depth = Texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* normals = Texture("TESR_NormalsBuffer");
		const D3DXVECTOR4* cameraData = Constant("TESR_CameraData");
		FlashThisFrame = false;
		if (!depth || !normals || !cameraData) { FlashLive = false; return; }
		// Flashlight: its own pass, before (and without) the lamps
		bool inputsIn = false;
		if (FlashWanted()) {
			static UINT flashWidth = 0, flashHeight = 0;
			if (!FlashTexture || flashWidth != w || flashHeight != h) {
				flashWidth = w; flashHeight = h;
				if (FlashSurface) { FlashSurface->Release(); FlashSurface = nullptr; }
				if (FlashTexture) { FlashTexture->Release(); FlashTexture = nullptr; }
				Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &FlashTexture, nullptr);
				if (FlashTexture) FlashTexture->GetSurfaceLevel(0, &FlashSurface);
			}
			if (FlashSurface && RtCore::CopyIn(imageDepth, depth) && RtCore::CopyIn(imageNormals, normals)) {
				inputsIn = true;
				const D3DXMATRIX& fv = TheRenderManager->viewMatrix;
				const D3DXMATRIX& fp = TheRenderManager->projMatrix;
				const D3DXVECTOR3 feye = RtCore::Eye();
				const D3DXVECTOR4& fcam = TheRenderManager->CameraPosition;
				const D3DXVECTOR4& spot = TheShaderManager->SpotLightPosition[0];
				struct { float size[4], forward[4], right[4], up[4], origin[4], light[4], params[4]; } f = {};
				f.size[0] = (float)tw; f.size[1] = (float)th;
				// UNOFFICIAL 2026-11-04 the depth's jitter on the ray uv (as the lamp traces, PassConstants.jitter); the result is in the rect
				// layout in rect frames, which Flashlight's D3D9 shadow pass reads there (TESR_RtFlashShadow, FlashlightEffect::TechniqueThisFrame)
				if (TAAEffect* taa = TheShaderManager->Effects.TAA) { const D3DXVECTOR2 fj = taa->DepthJitterUV(); f.size[2] = fj.x; f.size[3] = fj.y; }
				f.forward[0] = fv._13; f.forward[1] = fv._23; f.forward[2] = fv._33; f.forward[3] = cameraData->y;
				f.right[0] = fv._11 / fp._11; f.right[1] = fv._21 / fp._11; f.right[2] = fv._31 / fp._11;
				f.up[0] = fv._12 / fp._22; f.up[1] = fv._22 / fp._22; f.up[2] = fv._32 / fp._22;
				f.origin[0] = feye.x; f.origin[1] = feye.y; f.origin[2] = feye.z;
				f.light[0] = spot.x - fcam.x; f.light[1] = spot.y - fcam.y; f.light[2] = spot.z - fcam.z; f.light[3] = spot.w;
				f.params[0] = (std::max)(TheSettingManager->GetSettingF(kSection, "StartOffset"), 0.0f);
				f.params[1] = (std::max)(TheSettingManager->GetSettingF(kSection, "FlashlightStop"), 0.0f);
				f.params[2] = (float)(0xFF & ~MaskAlphaTested & ~MaskGlass);	// people (0x20) block it; lamp glass never does
				f.params[3] = (std::max)(TheSettingManager->GetSettingF(kSection, "StartOffsetPerUnit"), 0.0f);	// (2026-11-04) as the lamp rays
				const int flashImages[3] = { imageDepth, imageNormals, imageFlash };
				if (RtCore::AddPass(flashShader, flashImages, 3, 0x4, &f, sizeof(f), (tw + 7) / 8, (th + 7) / 8))
					FlashThisFrame = RtCore::CopyOut(imageFlash, FlashSurface);
			}
		}
		FlashLive = FlashThisFrame;
		if (!(CompiledIn && LampsThisFrame && LampCount > 0)) return;

		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		PassConstants c = {};
		c.size[0] = (float)tw; c.size[1] = (float)th; c.size[2] = 1.0f / tw; c.size[3] = 1.0f / th;
		D3DXVECTOR2 jitter(0.0f, 0.0f);	// (2026-11-03) the depth's jitter, for the trace and (Traced) the lookup
		if (TAAEffect* taa = TheShaderManager->Effects.TAA) jitter = taa->DepthJitterUV();
		c.jitter[0] = jitter.x; c.jitter[1] = jitter.y;
		c.jitter[2] = AlphaOn() && TheSettingManager->GetSettingI(kSection, "AlphaTest") != 0 ? 1.0f : 0.0f;	// (2026-11-03 rtalpha) test alpha-tested casters (the shaders add 0x10 to the masks)
		c.forward[0] = view._13; c.forward[1] = view._23; c.forward[2] = view._33; c.forward[3] = cameraData->y;
		c.right[0] = view._11 / proj._11; c.right[1] = view._21 / proj._11; c.right[2] = view._31 / proj._11;
		c.up[0] = view._12 / proj._22; c.up[1] = view._22 / proj._22; c.up[2] = view._32 / proj._22;
		const D3DXVECTOR3 eye = RtCore::Eye();	// the eye in scene space (0, 0, 0: the scene is camera-relative)
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		c.origin[0] = eye.x; c.origin[1] = eye.y; c.origin[2] = eye.z;
		c.origin[3] = 4095.0f / log2f((std::max)(cameraData->y, 2.0f));
		static UInt32 frame = 0;
		frame++;
		c.params[0] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "Rays"), 1), 8);
		c.params[1] = (std::max)(TheSettingManager->GetSettingF(kSection, "StartOffset"), 0.0f);
		c.mask[3] = (std::max)(TheSettingManager->GetSettingF(kSection, "StartOffsetPerUnit"), 0.0f);	// 2026-10-30: x the view depth
		c.mask[2] = (float)TheSettingManager->GetSettingI(kSection, "RankBy");	// 2026-10-30 (the mask variant sets its ExteriorDebug there)
		c.params[2] = (std::max)(TheSettingManager->GetSettingF(kSection, "LampStop"), 0.0f);
		c.params[3] = TheSettingManager->GetSettingI(kSection, "Jitter") ? fmodf((float)frame * 0.61803399f, 1.0f) : 0.0f;
		// Lamp models skip their own light over the last LampModelDistance of each ray, as LampModelShadows false does for the cube maps
		const bool skipModels = TheSettingManager->GetSettingI(kSection, "SkipLampModels") != 0;
		c.params2[0] = skipModels ? (std::max)(TheSettingManager->GetSettingF("Shaders.ShadowsInteriors.Forward", "LampModelDistance"), 0.0f) : 0.0f;
		UINT farMask = 0xFF & ~MaskAlphaTested;	// 0xEF: the alpha-tested casters (0x10) only with jitter.z (the shaders add it), else skipped
		if (TheSettingManager->GetSettingI(kSection, "SkipLampGlass")) farMask &= ~MaskGlass;
		c.params2[1] = (float)farMask;
		c.params2[2] = (float)(skipModels ? (farMask & ~MaskLampModel) : farMask);
		c.params2[3] = (float)TableSlots;
		c.flags[0] = TheSettingManager->GetSettingI(kSection, "OneSided") ? 1.0f : 0.0f;
		c.flags[1] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "LampsPerPixel"), 1), 3);
		// UNOFFICIAL 2026-10-29 Interleave: 1/2 (checkerboard) or 1/4 (2 x 2 cells) of the pixels traced a frame, in turn
		const int interleaveSetting = TheSettingManager->GetSettingI(kSection, "Interleave");
		const int interleave = interleaveSetting >= 4 ? 4 : interleaveSetting >= 2 ? 2 : 1;
		c.flags[2] = (float)interleave;
		c.flags[3] = (float)(frame % (UInt32)interleave);
		const float radius = (std::max)(TheSettingManager->GetSettingF(kSection, "LampRadius"), 0.0f);
		const float invisible = TheSettingManager->GetSettingF(kSection, "InvisibleLightRadius");
		if (!WriteTable(camera, radius, invisible, (std::max)(TheSettingManager->GetSettingF(kSection, "GlowStopMax"), 0.0f))) {
			static bool logged = false;
			if (!logged) { logged = true; Logger::Log("UNOFFICIAL RT lamp shadows: the lamp table could not be written; not traced"); }
			return;
		}

		if ((!inputsIn && (!RtCore::CopyIn(imageDepth, depth) || !RtCore::CopyIn(imageNormals, normals))) || !RtCore::CopyIn(imageTable, TableTexture)) return;
		// Exteriors: the mask variant, its result copied out for ApplyExteriorMask this frame; nothing is reprojected
		if (TheShaderManager->GameState.isExterior && !ExteriorForwardFrame) {
			static UINT maskWidth = 0, maskHeight = 0;
			if (!MaskTexture || maskWidth != w || maskHeight != h) {
				maskWidth = w; maskHeight = h;
				if (MaskSurface) { MaskSurface->Release(); MaskSurface = nullptr; }
				if (MaskTexture) { MaskTexture->Release(); MaskTexture = nullptr; }
				Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &MaskTexture, nullptr);
				if (MaskTexture) MaskTexture->GetSurfaceLevel(0, &MaskSurface);
			}
			if (!MaskSurface || imageMask < 0 || exteriorShader < 0) return;
			const D3DXVECTOR4& pointNear = TheShaderManager->Effects.ShadowsExteriors->Constants.PointShadowNear;
			c.mask[0] = pointNear.y; c.mask[1] = pointNear.z;
			c.mask[2] = (float)TheSettingManager->GetSettingI(kSection, "ExteriorDebug");	// diagnostic: 1 = mask 0, 2 = mask 1
			// [8]: binding 12, the alpha atlas (rtalpha); [5]-[7] unused by this variant
			const int exteriorImages[9] = { imageDepth, imageNormals, imageResult, imageTable, imageMask, imageDepth, imageDepth, imageDepth, AlphaImage() };
			if (!RtCore::AddPass(exteriorShader, exteriorImages, 9, 0x14, &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
			MaskThisFrame = RtCore::CopyOut(imageMask, MaskSurface);
			return;
		}
		// UNOFFICIAL 2026-10-31 ManyLights: the many-light variant (its own packing, its R history pair); Filter and Denoise do not apply
		if (ManyNoting) {
			c.params2[3] = (float)TableTop;	// ids 0 .. 63 + the other lamps up to the highest noted id (empty slots have reach 0)
			// ManyLightsOneRay: one ray a pixel (RtLampTraceRestir.spv), else RtLampTraceMany.spv; the history only from the same variant
			const bool oneRay = OneRayReady() && TheSettingManager->GetSettingI(kSection, "ManyLightsOneRay") != 0;
			// (2026-11-04) and only at the scale it was traced at: a RenderScale change or a rect <-> native frame restarts the history
			// (its texels are another layout); RenderScale 1: always 1 = 1, as before
			const bool sameScale = Traced.sx == (float)tw / (float)w && Traced.sy == (float)th / (float)h;
			const bool history = TracedMany && TracedOneRay == oneRay && Traced.valid && Mode > 0.0f && sameScale;	// Reproject (Submit) maps into the previous (ManyLights) trace
			static int restartLogs = 0;
			if (TracedMany && Traced.valid && !sameScale && restartLogs < 4) {
				restartLogs++;
				Logger::Log("UNOFFICIAL RT lamp shadows: ManyLights history restarted: traced scale %.3f x %.3f -> %.3f x %.3f (RenderScale change or rect <-> native)",
					Traced.sx, Traced.sy, (float)tw / (float)w, (float)th / (float)h);
			}
			c.rect[0] = (float)w; c.rect[1] = (float)h;
			static int rectLogs = 0;
			if (tw < w && rectLogs < 2) {
				rectLogs++;
				Logger::Log("UNOFFICIAL RT lamp shadows: ManyLights (%s) traced in the RenderScale rect: %ux%u of %ux%u, history %d.", oneRay ? "one ray" : "many", tw, th, w, h, history ? 1 : 0);
			}
			c.many[0] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "ManyLightsRays"), 1), 4);
			c.many[1] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "ManyLightsHistory"), 1), 64);
			c.many[2] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "ManyLightsSpatial"), 0), 2);
			c.many[3] = history ? 1.0f : 0.0f;
			memcpy(c.reproject, Reproject, sizeof(Reproject));
			c.many2[0] = (float)(frame & 0xFFFFFF);
			c.many2[1] = 0.05f;
			c.many2[2] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "ManyLightsRestStride"), 1), 2);
			if (oneRay) {
				// ObjectMotion's world move a pixel (B1), when it was made this frame; else the camera's reprojection alone
				IDirect3DBaseTexture9* motion = Texture("TESR_ObjectMotionBuffer");
				const D3DXVECTOR4* motionData = Constant("TESR_ObjectMotionData");
				const bool moved = motion && motionData && motionData->x > 0.0f && RtCore::CopyIn(imageMotion, motion);
				c.many2[3] = moved ? 1.0f : 0.0f;
				if (moved) MotionFrames++;
				// moving tiles: the 8 x 8 groups shifted by 0..7 pixels each way, another shift each frame (many.x = -1 - shift), one more
				// group each way to cover the edge
				c.many[0] = -1.0f - (float)((frame * 37u) & 63u);
				const int images[10] = { imageDepth, imageNormals, imageResult, imageTable, imageHistory[HistoryParity], imageHistory[1 - HistoryParity],
					imageHistoryIds[HistoryParity], imageHistoryIds[1 - HistoryParity], moved ? imageMotion : imageNormals, AlphaImage() };	// [9]: binding 13 (rtalpha)
				if (!RtCore::AddPass(restirShader, images, 10, 0xA4, &c, sizeof(c), (tw + 7) / 8 + 1, (th + 7) / 8 + 1)) return;
			} else {
				// [8]: binding 12, the alpha atlas (rtalpha); [6], [7] unused
				const int manyImages[9] = { imageDepth, imageNormals, imageResult, imageTable, imageHistory[HistoryParity], imageHistory[1 - HistoryParity], imageDepth, imageDepth, AlphaImage() };
				if (!RtCore::AddPass(manyShader, manyImages, 9, 0x24, &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
			}
			TracedOneRay = oneRay;
			HistoryParity = 1 - HistoryParity;
			SplitThisFrame = false;
			if (!RtCore::CopyOut(imageResult, ResultSurface)) { TracedMany = false; return; }
			Traced = { view, D3DXVECTOR3(camera.x + eye.x, camera.y + eye.y, camera.z + eye.z), proj._11, proj._22, cameraData->y, true, (float)tw / (float)w, (float)th / (float)h, jitter.x, jitter.y };
			memcpy(TracedMap, PendingMap, sizeof(TracedMap));
			memset(TracedLampWorld, 0, sizeof(TracedLampWorld));
			for (int i = 0; i < LampCount; i++)	// (2026-11-03 rtext) the land's slots 0-19 by position (BindTerrain), as the other variants
				if (Lamps[i].slot >= 0 && Lamps[i].slot < TableSlots)
					TracedLampWorld[Lamps[i].slot] = D3DXVECTOR4(Lamps[i].position.x, Lamps[i].position.y, Lamps[i].position.z, 1.0f);
			TracedIds.swap(PendingIds);
			TracedMany = true;
			// The collected lamps: on the 3rd ManyLights trace in each cell (the draws have noted their lamps by then) and every
			// 1200th; the others listed (up to 16) with where they are, so a run's log says which lamps the trace took
			const UInt32 cell = Player && Player->parentCell ? Player->parentCell->refID : 0;
			if (cell != ManyLogCell) { ManyLogCell = cell; ManyCellTraces = 0; }
			ManyCellTraces++;
			if ((ManyCellTraces == 3 || (++ManyTraces % 1200) == 0) && ManyTraces <= 24000) {
				char list[1024] = "";
				int listed = 0;
				for (int i = 0; i < LampCount && listed < 16; i++) {
					const SlotLamp& l = Lamps[i];
					if (l.slot < 64) continue;
					char one[96];
					snprintf(one, sizeof(one), "%s %d (%.0f %.0f %.0f) reach %.0f int %.2f fade %.2f", listed ? ";" : "", l.slot, l.position.x, l.position.y, l.position.z, l.reach, l.intensity, l.fade);
					strncat_s(list, one, _TRUNCATE);
					listed++;
				}
				Logger::Log("UNOFFICIAL RT lamp shadows: ManyLights trace %u (cell %08X, %s, ObjectMotion on %u traces): %d lamps in the table (%d with a cube slot, %d others, %d more refused past ManyLightsMax), cube redraws skipped %u since the last line; others:%s%s",
					FramesTraced, cell, oneRay ? "one ray a pixel" : "2 exact + rest rays", MotionFrames, LampCount, LampCount - ExtraCount, ExtraCount, RefusedCount, SkippedCubes, listed ? list : " none", ExtraCount > listed ? " ..." : "");
				SkippedCubes = 0;
			}
			if (FramesTraced++ == 0)
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT lamp shadows traced, ManyLights (%ux%u, %d lamps, %s).", w, h, LampCount, oneRay ? "one ray a pixel" : "2 exact + rest rays");
			return;
		}
		// UNOFFICIAL 2026-10-29 Filter: the soft edges' noise (LampRadius, 1 ray) smoothed per lamp (RtLampFilter.comp) before the copy out
		const bool filter =imageRaw >= 0 && filterShader >= 0 && TheSettingManager->GetSettingI(kSection, "Filter");
		// Denoise: the split variant also writes ranks 0 and 1 apart (images 6-9), copied out for RtDenoise (Denoise() below)
		bool split = !filter && splitShader >= 0 && imageSplit[3] >= 0 && TheSettingManager->GetSettingI(kSection, "Denoise");
		static UINT splitWidth = 0, splitHeight = 0;
		const bool resized = split && (splitWidth != w || splitHeight != h);
		if (split) { splitWidth = w; splitHeight = h; }
		for (int i = 0; split && i < 4; i++) {
			if (!SplitTexture[i] || resized) {
				if (SplitSurface[i]) { SplitSurface[i]->Release(); SplitSurface[i] = nullptr; }
				if (SplitTexture[i]) { SplitTexture[i]->Release(); SplitTexture[i] = nullptr; }
				TheRenderManager->device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &SplitTexture[i], nullptr);
				if (SplitTexture[i]) SplitTexture[i]->GetSurfaceLevel(0, &SplitSurface[i]);
			}
			split = SplitSurface[i] != nullptr;
		}
		// [8]: binding 12, the alpha atlas (rtalpha); [4]-[7] the split targets (unused without Denoise)
		const int images[9] = { imageDepth, imageNormals, filter ? imageRaw : imageResult, imageTable, split ? imageSplit[0] : imageDepth, split ? imageSplit[1] : imageDepth,
			split ? imageSplit[2] : imageDepth, split ? imageSplit[3] : imageDepth, AlphaImage() };
		if (!RtCore::AddPass(split ? splitShader : shader, images, 9, split ? 0xF4 : 0x4, &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
		for (int i = 0; split && i < 4; i++) split = RtCore::CopyOut(imageSplit[i], SplitSurface[i]);
		SplitThisFrame = split;
		if (filter) {
			struct { float size[4], params[4]; } f = {};
			f.size[0] = (float)tw; f.size[1] = (float)th;
			f.size[2] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "FilterStep"), 1), 4);
			f.size[3] = (std::max)(TheSettingManager->GetSettingF(kSection, "FilterDepthSigma"), 0.01f);
			f.params[0] = (std::max)(TheSettingManager->GetSettingF(kSection, "FilterNormalPower"), 0.0f);
			const int filterImages[4] = { imageDepth, imageNormals, imageRaw, imageResult };
			if (!RtCore::AddPass(filterShader, filterImages, 4, 0x8, &f, sizeof(f), (tw + 7) / 8, (th + 7) / 8)) return;
		}
		if (!RtCore::CopyOut(imageResult, ResultSurface)) return;
		TracedMany = false;
		// The next frame's draws reproject into this frame
		Traced = { view, D3DXVECTOR3(camera.x + eye.x, camera.y + eye.y, camera.z + eye.z), proj._11, proj._22, cameraData->y, true, (float)tw / (float)w, (float)th / (float)h, jitter.x, jitter.y };
		memcpy(TracedMap, PendingMap, sizeof(TracedMap));
		memset(TracedLampWorld, 0, sizeof(TracedLampWorld));
		for (int i = 0; i < LampCount; i++)
			if (Lamps[i].slot >= 0 && Lamps[i].slot < TableSlots)
				TracedLampWorld[Lamps[i].slot] = D3DXVECTOR4(Lamps[i].position.x, Lamps[i].position.y, Lamps[i].position.z, 1.0f);
		// Which lamps were traced (index -> cube slot -> where): matches the LAMPSLOT lines of LogLamps; now and then
		if ((FramesTraced % 600) == 1 && FramesTraced <= 6001) {
			char text[900]; int used = 0;
			for (int i = 0; i < LampCount && used < 800; i++)
				used += sprintf_s(text + used, sizeof(text) - used, " | slot %d at %.0f %.0f %.0f reach %.0f I %.2f", Lamps[i].slot,
					Lamps[i].position.x, Lamps[i].position.y, Lamps[i].position.z, Lamps[i].reach, Lamps[i].intensity);
			Logger::Log("UNOFFICIAL RT lamp shadows: trace %u, %d lamps, cube redraws skipped %u since the last line (SkipTracedCubes)%s",
				FramesTraced, LampCount, SkippedCubes, LampCount ? text : "");
			SkippedCubes = 0;
		}
		if (FramesTraced++ == 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT lamp shadows traced (%ux%u, %d lamps, %d rays).", w, h, LampCount, (int)c.params[0]);
	}

	// RtCore's mask hook (the FULL mask, replacing the core's land 0x01 / objects 0x02): a lamp's glass or model gets ONLY its own bit,
	// so the lamp rays' cull masks (no 0x04, near the lamp no 0x08) skip it while every other ray (the sun's 0xFF) still hits it.
	inline UInt32 MaskCounts[4] = {};		// land, objects, glass, lamp models (logged now and then: is the hook used, does it find lamps?)
	inline UInt32 MaskHook(NiGeometry* geometry, bool land) {
		static UInt32 calls = 0;
		if ((++calls % 20000) == 0 && calls <= 200000)
			Logger::Log("UNOFFICIAL RT lamp shadows: mask hook %u calls: land %u, objects %u, lamp glass %u, lamp models %u (cell glow list %u)",
				calls, MaskCounts[0], MaskCounts[1], MaskCounts[2], MaskCounts[3], (UInt32)LampFixtures::Glows.size());
		if (land) { MaskCounts[0]++; return 0x01; }
		const UInt32 own = InstanceMask(geometry, false);
		MaskCounts[own == MaskGlass ? 2 : own == MaskLampModel ? 3 : 1]++;
		return own ? own : 0x02;
	}

	// UNOFFICIAL 2026-10-29 Denoise: ShaderManager, right after RtCore::Frame (the traced images are copied out by then): ranks 0 and 1
	// through the shared denoiser with the cube slot as the id (taps of another lamp are not mixed), then RtLampRepack.frag writes the
	// result again with the denoised visibility; the lamp shaders of the next frame read that (Stage15Texture).
	inline void Denoise(IDirect3DDevice9* Device) {
		const bool split = SplitThisFrame;
		SplitThisFrame = false;
		if (!split) { UseRepacked = false; return; }
		const UINT w = TextureWidth, h = TextureHeight;
		static UINT denoisedWidth = 0, denoisedHeight = 0;
		if (!RepackTexture || denoisedWidth != w || denoisedHeight != h) {
			denoisedWidth = w; denoisedHeight = h;
			for (int k = 0; k < 2; k++) {
				if (DenoisedSurface[k]) { DenoisedSurface[k]->Release(); DenoisedSurface[k] = nullptr; }
				if (DenoisedTexture[k]) { DenoisedTexture[k]->Release(); DenoisedTexture[k] = nullptr; }
				Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &DenoisedTexture[k], nullptr);
				if (DenoisedTexture[k]) DenoisedTexture[k]->GetSurfaceLevel(0, &DenoisedSurface[k]);
			}
			if (RepackSurface) { RepackSurface->Release(); RepackSurface = nullptr; }
			if (RepackTexture) { RepackTexture->Release(); RepackTexture = nullptr; }
			Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &RepackTexture, nullptr);
			if (RepackTexture) RepackTexture->GetSurfaceLevel(0, &RepackSurface);
		}
		static ID3D9NvrExtPipeline* repack = nullptr;
		static bool repackTried = false;
		if (!repackTried) {
			repackTried = true;
			std::vector<char> spirv;
			ID3D9NvrExt* ext = DxvkCompute::Extension();
			if (ext && DxvkCompute::LoadSpirv("RtLampRepack", spirv)) {
				NVR_EXT_BINDING_DESC bindings[4] = {
					{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
					{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
					{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },
					{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
				};
				NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
				desc.pFragmentSpirv = spirv.data();
				desc.FragmentSpirvSize = (UINT)spirv.size();
				desc.BindingCount = 4;
				desc.pBindings = bindings;
				desc.PushConstantSize = 16;
				desc.UsesSamplerHeap = TRUE;
				desc.SamplerHeapSet = 0;
				desc.SamplerHeapBinding = 0;
				desc.ColorTargetCount = 1;
				desc.Blend = NVR_EXT_BLEND_NONE;
				desc.ColorWriteMask = 0x3u;
				desc.pDebugName = "RtLampRepack";
				if (FAILED(ext->CreateGraphicsPipeline(&desc, &repack))) repack = nullptr;
			}
			if (!repack) Logger::Log("UNOFFICIAL RT lamp shadows: Denoise off: the repack pass could not be made (RtLampRepack.spv / Vegas Shaders DXVK)");
		}
		ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
		UseRepacked = false;
		if (!repack || !point || !RepackSurface || !DenoisedSurface[0] || !DenoisedSurface[1]) return;
		static RtDenoise::Denoiser* denoiser[2] = {};
		RtDenoise::Settings settings;
		settings.MaxHistory = (std::max)(TheSettingManager->GetSettingF(kSection, "DenoiseHistory"), 1.0f);
		settings.IgnoreRr = true;	// UNOFFICIAL 2026-10-30 (DLSS RR review): next frame's object shaders read it; it stays denoised under RR
		for (int k = 0; k < 2; k++) {
			if (!denoiser[k]) denoiser[k] = RtDenoise::Create(k ? "RT lamp shadows rank 1" : "RT lamp shadows rank 0");
			settings.Ids = SplitTexture[2 + k];
			if (!RtDenoise::Run(denoiser[k], Device, SplitTexture[k], DenoisedSurface[k], settings)) return;
		}
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);	// a pass may not draw into the device's current target (RepackSurface never is)
		struct { UInt32 samplerIndex, pad[3]; } push = {};
		NVR_EXT_RESOURCE_BINDING bindings[4] = {};
		IUnknown* inputs[3] = { ResultTexture, DenoisedTexture[0], DenoisedTexture[1] };
		for (int i = 0; i < 3; i++) { bindings[i].Slot = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].pResource = inputs[i]; }
		bindings[3].Slot = 0; bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = repack;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = RepackSurface;
		draw.BindingCount = 4;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		UseRepacked = SUCCEEDED(DxvkCompute::Extension()->Draw(&draw));
		if (current) current->Release();
	}

	// Exteriors: ShaderManager, after the screen-space lamp mask passes outdoors (with point shadows on: TESR_ShadowFade.z): the traced
	// mask into the shadow buffer's green (the lamps' channel; the sun's red untouched), over what PointShadows.fx wrote there
	inline void ApplyExteriorMask(IDirect3DDevice9* Device, IDirect3DSurface9* target) {
		const bool mask = MaskThisFrame;
		MaskThisFrame = false;
		ExteriorLive = mask && TheSettingManager->GetSettingI(kSection, "Live") != 0;
		if (!mask || !target || !MaskTexture) return;
		static ID3D9NvrExtPipeline* combine = nullptr;
		static bool tried = false;
		if (!tried) {
			tried = true;
			std::vector<char> spirv;
			ID3D9NvrExt* ext = DxvkCompute::Extension();
			if (ext && DxvkCompute::LoadSpirv("RtLampMaskCombine", spirv)) {
				NVR_EXT_BINDING_DESC bindings[2] = {
					{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
					{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
				};
				NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
				desc.pFragmentSpirv = spirv.data();
				desc.FragmentSpirvSize = (UINT)spirv.size();
				desc.BindingCount = 2;
				desc.pBindings = bindings;
				desc.PushConstantSize = 16;
				desc.UsesSamplerHeap = TRUE;
				desc.SamplerHeapSet = 0;
				desc.SamplerHeapBinding = 0;
				desc.ColorTargetCount = 1;
				desc.Blend = NVR_EXT_BLEND_NONE;
				desc.ColorWriteMask = 0x2u;	// green only: the lamps
				desc.pDebugName = "RtLampMaskCombine";
				if (FAILED(ext->CreateGraphicsPipeline(&desc, &combine))) combine = nullptr;
			}
			if (!combine) Logger::Log("UNOFFICIAL RT lamp shadows: Exteriors off: the mask pass could not be made (RtLampMaskCombine.spv / Vegas Shaders DXVK)");
		}
		ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
		if (!combine || !point) { ExteriorLive = false; return; }
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);	// a pass may not draw into the device's current target: park it meanwhile
		IDirect3DSurface9* parking = TheShaderManager->Effects.SunShadows ? TheShaderManager->Effects.SunShadows->scratchSurface[0] : nullptr;
		const bool park = current == target && parking;
		if (park) Device->SetRenderTarget(0, parking);
		struct { UInt32 samplerIndex, pad[3]; } push = {};
		NVR_EXT_RESOURCE_BINDING bindings[2] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = MaskTexture;
		bindings[1].Slot = 0; bindings[1].Type = NVR_EXT_BIND_SAMPLER; bindings[1].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = combine;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = target;
		draw.BindingCount = 2;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		if (FAILED(DxvkCompute::Extension()->Draw(&draw))) ExteriorLive = false;
		if (park) Device->SetRenderTarget(0, current);
		if (current) current->Release();
		static UInt32 applied = 0;
		if (applied++ == 0) Logger::Log("UNOFFICIAL RT lamp shadows: Exteriors: the traced lamp mask is applied outdoors (%d lamps in the table)", LampCount);
	}

	// ShaderManager, once before the first RtCore::Frame (next to RtSunShadows::Register)
	inline void Register() {
		if (!CompiledIn) return;
		RtCore::SetMaskHook(MaskHook);
		shader = RtCore::AddShader("RtLampTrace");
		splitShader = RtCore::AddShader("RtLampTraceSplit");
		exteriorShader = RtCore::AddShader("RtLampTraceExterior");
		flashShader = RtCore::AddShader("RtFlashTrace");
		filterShader = RtCore::AddShader("RtLampFilter");
		manyShader = RtCore::AddShader("RtLampTraceMany");
		restirShader = RtCore::AddShader("RtLampTraceRestir");
		RtCore::AddUser({ "RT lamp shadows", Wants, Setup, Record });
	}

}
