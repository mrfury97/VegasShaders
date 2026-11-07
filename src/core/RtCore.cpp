// UNOFFICIAL 2026-10-28 (NVR DXVK): the ray tracing core (RtCore.h; docs\next\FORK-RT-CORE.md). The helper's side is
// tools\nvr-rt-helper (MIT, built on its own, 64-bit); the protocol is tools\nvr-rt-helper\NvrRtProtocol.h. The launch, the shared
// block, the watchdog and the stop follow UpscalerDlss.cpp (the DLSS helper), which proved them.

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "RtCore.h"
bool RtMaterialRuleOf(NiGeometry* geometry, float* roughness, float* metal);	// UNOFFICIAL 2026-11-03 (NewVegas/Hooks/Render.cpp)
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "PlantWind.h"	// (2026-10-30) PlantSway: a wind-bent plant's traced instance leans with it
#include "../../tools/nvr-rt-helper/NvrRtProtocol.h"
#include "RtScenePick.h"	// (2026-11-06) the scene picking rules (tested by tools/nvr-rt-host)

// UNOFFICIAL 2026-11-05 (rtrefl-only: the demo room's ceiling met by no reflection ray) the first 64 distinct meshes a cell that the
// traced scene leaves out, with why.
static void LogLeftOut(const NiGeometry* geo, const char* why, int shaderType) {
	static std::vector<std::string> told;
	static std::unordered_map<const void*, bool> seen;	// geometries met this cell (cheap: this runs for every left-out mesh every frame)
	static const void* toldCell = nullptr;
	if (Player && Player->parentCell != toldCell) { toldCell = Player->parentCell; told.clear(); seen.clear(); }
	if (told.size() >= 64 || seen.size() > 4096 || !seen.emplace(geo, true).second) return;
	char what[192];
	_snprintf_s(what, _TRUNCATE, "'%s' %s%s", geo && geo->m_pcName ? geo->m_pcName : "", why, shaderType >= 0 ? " (shader type " : "");
	std::string key(what);
	if (shaderType >= 0) key += std::to_string(shaderType) + ")";
	if (std::find(told.begin(), told.end(), key) != told.end()) return;
	told.push_back(key);
	Logger::Log("RT CORE: left out of the traced scene: %s", key.c_str());
}

// UNOFFICIAL 2026-11-06 (alpha lamp shadows: VS-AlphaLampTest's chain-link fence cast no traced shadow) the first 64 meshes (by name) a
// cell that the traced scene takes, alpha-tested ones with their atlas slot and alpha reference: with LogLeftOut, which meshes rays can meet.
static void LogAlphaTaken(const NiGeometry* geo, int slot, unsigned ref, bool blended) {
	static std::unordered_map<const void*, bool> seen;		// geometries met this cell (cheap: this runs for every taken mesh every frame)
	static std::unordered_map<std::string, bool> names;	// (2026-11-06 b) one line a mesh name + slot: a room's 40 copies of one wall filled the cap
	static const void* toldCell = nullptr;
	static int told = 0;
	if (Player && Player->parentCell != toldCell) { toldCell = Player->parentCell; seen.clear(); names.clear(); told = 0; }
	if (told >= 64 || seen.size() > 8192 || !seen.emplace(geo, true).second) return;
	if (!names.emplace(std::string(geo && geo->m_pcName ? geo->m_pcName : "") + "#" + std::to_string(slot), true).second) return;
	told++;
	// (2026-11-06 c, the demo room's missing office-kit ceiling) every taken mesh, not only alpha-tested ones (slot -1 / ref 999: opaque),
	// with its world bound's centre (camera relative) and radius: which part of a kit piece is the ceiling, and is it taken
	const NiBound* b = geo ? geo->m_kWorldBound : nullptr;
	const float cz = b ? b->Center.z - TheRenderManager->CameraPosition.z : 0.0f, r = b ? b->Radius : 0.0f;
	if (ref > 255u) Logger::Log("RT CORE: in the traced scene: '%s' opaque, bound centre z %+.0f (camera relative), r %.0f", geo && geo->m_pcName ? geo->m_pcName : "", cz, r);
	else {
		// (2026-11-06 d) the alpha property's test function (flags bits 10-12: 0 ALWAYS, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL,
		// 6 GEQUAL, 7 NEVER); the RT test assumes GREATER
		const NiAlphaProperty* ap = geo ? (const NiAlphaProperty*)const_cast<NiGeometry*>(geo)->GetProperty(NiProperty::PropertyType::kType_Alpha) : nullptr;
		static const char* const funcs[8] = { "ALWAYS", "LESS", "EQUAL", "LEQUAL", "GREATER", "NOTEQUAL", "GEQUAL", "NEVER" };
		Logger::Log("RT CORE: alpha-tested in the traced scene: '%s' atlas slot %d, alpha ref %u, test %s (flags %04X)%s, bound centre z %+.0f, r %.0f",
			geo && geo->m_pcName ? geo->m_pcName : "", slot, ref, ap ? funcs[(ap->flags >> 10) & 7] : "?", ap ? (unsigned)ap->flags : 0u, blended ? " (also blended)" : "", cz, r);
	}
}

// UNOFFICIAL 2026-11-06 (gs-hill: PTPrimary 0 showed a huge rounded mesh with huge texels just left of the camera in Goodsprings, drawn
// by no raster pass; the 64-name cap above never reached it) every LARGE taken mesh (world bound radius 300+), once a cell, up to 256:
// its bound centre (camera relative, x y z) and radius, its world scale and its parents' names (the NIF's nodes), so a mesh far bigger
// than it should be, or standing where the game draws nothing, can be named.
static void LogBigTaken(const NiGeometry* geo) {
	const NiBound* b = geo ? geo->m_kWorldBound : nullptr;
	if (!b || b->Radius < 300.0f) return;
	static std::unordered_map<const void*, bool> seen;
	static const void* toldCell = nullptr;
	static int told = 0;
	if (Player && Player->parentCell != toldCell) { toldCell = Player->parentCell; seen.clear(); told = 0; }
	if (told >= 256 || !seen.emplace(geo, true).second) return;
	told++;
	const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
	std::string parents;
	const NiNode* p = geo->m_parent;
	for (int k = 0; p && k < 3; k++, p = p->m_parent) { parents += " < '"; parents += p->m_pcName ? p->m_pcName : ""; parents += "'"; }
	Logger::Log("RT CORE: big mesh in the traced scene: '%s' bound centre %+.0f %+.0f %+.0f (camera relative), r %.0f, scale %.2f%s",
		geo->m_pcName ? geo->m_pcName : "", b->Center.x - cam.x, b->Center.y - cam.y, b->Center.z - cam.z, b->Radius, geo->m_worldTransform.scale, parents.c_str());
}

namespace {
	const UINT64 kReleaseMargin = 1ull << 20;	// the watchdog / Stop release every wait that could still be recorded
	const UINT kArenaWidth = 4096;
	const UINT kReleaseAfterFrames = 120;		// a geometry not met for this many frames is released in the helper
	// Shared images are made in sizes rounded up to this (the content in the top-left corner). Measured 2026-10-28 on AMD 25.10: the
	// game's 32-bit driver and the helper's 64-bit driver lay out an optimal-tiling image the same way only when both sides are
	// multiples of 256 texels (1280x720 R32F: 3686400 bytes in the game, 3957104 in the helper; 1280x768 and 1920x1080 -> 2048x1280
	// etc. agree, texel for texel: tools\nvr-rt-helper\test\rt_helper_test.cpp RT_TEST_SCREEN / RT_TEST_GAME).
	const UINT kImageAlign = 256;
	UINT AlignImage(UINT v) { return (v + kImageAlign - 1) / kImageAlign * kImageAlign; }

	bool FileExists(const std::wstring& path) {
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::wstring GameDir() {
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir(path);
		const size_t slash = dir.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
	}

	std::string HrText(HRESULT hr) {
		char text[96];
		if (HRESULT_FACILITY(hr) == NVR_EXT_FACILITY_VK) sprintf_s(text, "VkResult %d", -(int)(hr & 0xFFFF));
		else if (HRESULT_FACILITY(hr) == FACILITY_WIN32) sprintf_s(text, "Win32 error %lu", (unsigned long)HRESULT_CODE(hr));
		else if (hr == D3DERR_NOTAVAILABLE) sprintf_s(text, "D3DERR_NOTAVAILABLE");
		else sprintf_s(text, "HRESULT 0x%08lX", (unsigned long)hr);
		return text;
	}

	// A geometry the game draws: its D3D9 buffers and draw ranges. Two NiGeometry objects that share the buffer data (the same
	// model placed twice) are one geometry with two instances.
	struct GeometryKey {
		const void*	buffData;
		const void*	vb;
		const void*	ib;
		UInt32		vertCount, triCount, baseVertex;
		// (2026-11-06) skinned partitions: the actor's skin instance. Actors cloned from one body / armour mesh SHARE the partition data
		// (BuffData, VB, IB), so two NPCs in the same outfit got one id: both posed into one structure in the same frame (two builds of
		// one destination in one call: the NVIDIA device-lost crashes, found by the validation layer) and both drawn in the last pose
		const void*	owner = nullptr;
		bool operator==(const GeometryKey& o) const {
			return buffData == o.buffData && vb == o.vb && ib == o.ib && vertCount == o.vertCount && triCount == o.triCount && baseVertex == o.baseVertex &&
				owner == o.owner;
		}
	};
	struct GeometryKeyHash {
		size_t operator()(const GeometryKey& k) const {
			size_t h = (size_t)k.buffData * 2654435761u;
			h ^= (size_t)k.vb + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= (size_t)k.ib + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= (size_t)(k.vertCount * 31u + k.triCount * 17u + k.baseVertex) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= (size_t)k.owner + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};
	struct GeometryEntry {
		UInt32	id = 0;
		UInt32	lastSeen = 0;
		bool	sent = false;		// a BUILD op went to the helper
		bool	refused = false;	// the fork refused it (logged once): never again
		bool	textured = false;	// skinned: its last build carried texcoords (hit textures)
		bool	skinned = false;	// an actor's partition (dropped at once when the player changes cell)
		// UNOFFICIAL 2026-10-31 actors (docs\next\RT-SUN-SOFT-AND-BUILD.md): the camera its last pose was posed against (the posed
		// vertices are camera relative: the instance moves them by pose camera - camera), that pose's world-space bone palette
		// (ActorStillSkip) and the poses sent since its last full build (ActorRebuildInterval)
		float	poseCamera[3] = { 0.0f, 0.0f, 0.0f };
		std::vector<float> palette;
		UInt32	poses = 0;
	};

	// A diffuse texture's average colour (linear rgb), from its smallest mip, once per texture (protocol v2's instance records).
	// Managed textures only (they keep a system-memory copy: LockRect is a plain read); DXT1/3/5 take their first block's two end
	// colours, 32-bit formats their first texel. false: unknown (a neutral grey goes to the helper).
	struct Albedo { float rgb[3]; bool known; };
	std::unordered_map<const void*, Albedo> albedoCache;

	float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

	Albedo ReadAlbedo(IDirect3DBaseTexture9* base) {
		Albedo a = { { 0.5f, 0.5f, 0.5f }, false };
		IDirect3DTexture9* texture = nullptr;
		if (!base || base->GetType() != D3DRTYPE_TEXTURE || FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), (void**)&texture)) || !texture) return a;
		const DWORD level = texture->GetLevelCount() ? texture->GetLevelCount() - 1 : 0;
		D3DSURFACE_DESC desc = {};
		D3DLOCKED_RECT locked = {};
		if (SUCCEEDED(texture->GetLevelDesc(level, &desc)) && desc.Pool == D3DPOOL_MANAGED &&
			SUCCEEDED(texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE))) {
			const unsigned char* b = (const unsigned char*)locked.pBits;
			auto from565 = [](unsigned short c, float* out) { out[0] = ((c >> 11) & 31) / 31.0f; out[1] = ((c >> 5) & 63) / 63.0f; out[2] = (c & 31) / 31.0f; };
			float c0[3], c1[3];
			switch (desc.Format) {
			case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: {
				const unsigned char* block = b + (desc.Format == D3DFMT_DXT1 ? 0 : 8);
				from565(*(const unsigned short*)block, c0);
				from565(*(const unsigned short*)(block + 2), c1);
				for (int i = 0; i < 3; i++) a.rgb[i] = SrgbToLinear(0.5f * (c0[i] + c1[i]));
				a.known = true;
				break;
			}
			case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
				a.rgb[0] = SrgbToLinear(b[2] / 255.0f); a.rgb[1] = SrgbToLinear(b[1] / 255.0f); a.rgb[2] = SrgbToLinear(b[0] / 255.0f);
				a.known = true;
				break;
			default: break;
			}
			texture->UnlockRect(level);
		}
		texture->Release();
		return a;
	}

	const Albedo& AlbedoOfTexture(IDirect3DBaseTexture9* d3d) {
		auto found = albedoCache.find(d3d);
		if (found == albedoCache.end()) {
			if (albedoCache.size() > 65536) albedoCache.clear();	// textures come and go with the cells: keep it bounded
			found = albedoCache.emplace(d3d, ReadAlbedo(d3d)).first;
		}
		return found->second;
	}

	const Albedo& AlbedoOf(NiGeometry* geo, BSShaderProperty* shaderProperty) {
		static const Albedo unknown = { { 0.5f, 0.5f, 0.5f }, false };
		if (!shaderProperty->IsLightingProperty()) return unknown;
		NiSourceTexture** textures = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[0];
		NiTexture* texture = textures ? *textures : nullptr;
		if (!texture || !texture->rendererData || !texture->rendererData->dTexture) return unknown;
		return AlbedoOfTexture(texture->rendererData->dTexture);
	}

	float HalfToFloat(UInt16 h) {
		const UInt32 sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
		if (!exponent) return (sign ? -1.0f : 1.0f) * mantissa * (1.0f / 16777216.0f);	// zero and denormals
		const UInt32 bits = exponent == 31 ? sign | 0x7F800000u | (mantissa << 13) : sign | ((exponent + 112u) << 23) | (mantissa << 13);
		float f;
		memcpy(&f, &bits, 4);
		return f;
	}

	// A leaf mesh's vertex layout (cached per declaration): position, TEXCOORD0, BLENDINDICES (float4: x wind blend, y wind matrix,
	// z corner index + light scale, w corner scale); all in one stream or not used
	struct LeafLayout { bool ok = false; int stream = -1; UInt32 position = 0, uv = 0, blend = 0; bool position4 = false, uvHalf = false; BYTE types[3] = {}; };
	std::unordered_map<const void*, LeafLayout> leafLayouts;
	const LeafLayout& LeafLayoutOf(IDirect3DVertexDeclaration9* declaration) {
		auto found = leafLayouts.find(declaration);
		if (found != leafLayouts.end()) return found->second;
		LeafLayout l;
		D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
		UINT count = 0;
		int ps = -1, us = -1, bs = -1;
		if (declaration && SUCCEEDED(declaration->GetDeclaration(nullptr, &count)) && count <= MAXD3DDECLLENGTH + 1 && SUCCEEDED(declaration->GetDeclaration(elements, &count))) {
			for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++) {
				const D3DVERTEXELEMENT9& e = elements[i];
				if (e.UsageIndex != 0) continue;
				if (e.Usage == D3DDECLUSAGE_POSITION && ps < 0) { ps = e.Stream; l.position = e.Offset; l.types[0] = e.Type; l.position4 = e.Type == D3DDECLTYPE_FLOAT4; }
				else if (e.Usage == D3DDECLUSAGE_TEXCOORD && us < 0) { us = e.Stream; l.uv = e.Offset; l.types[1] = e.Type; l.uvHalf = e.Type == D3DDECLTYPE_FLOAT16_2; }
				else if (e.Usage == D3DDECLUSAGE_BLENDINDICES && bs < 0) { bs = e.Stream; l.blend = e.Offset; l.types[2] = e.Type; }
			}
			l.ok = ps >= 0 && ps == us && ps == bs && (l.types[0] == D3DDECLTYPE_FLOAT3 || l.types[0] == D3DDECLTYPE_FLOAT4) &&
				(l.types[1] == D3DDECLTYPE_FLOAT2 || l.types[1] == D3DDECLTYPE_FLOAT16_2) && l.types[2] == D3DDECLTYPE_FLOAT4;
			l.stream = ps;
		}
		if (leafLayouts.size() > 256) leafLayouts.clear();
		return leafLayouts.emplace(declaration, l).first->second;
	}

	// UNOFFICIAL 2026-10-30 the glow / emissive flag (NVR_RT_RECORD_GLOW; rt-reflections' GlowBrightness reads it): the draw's shader has
	// a glow map (BSShaderPPLightingProperty texture slot 2: neon, lit signs and windows, lamp globes).
	bool HasGlowMap(BSShaderProperty* shaderProperty) {
		const UInt32 type = static_cast<NiShadeProperty*>((NiProperty*)shaderProperty)->m_eShaderType;
		if (!shaderProperty->IsLightingProperty() && type != NiShadeProperty::kProp_PPLighting && type != NiShadeProperty::kProp_Lighting30) return false;
		NiSourceTexture** glow = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[2];
		return glow && *glow && (*glow)->rendererData && (*glow)->rendererData->dTexture;
	}
	IDirect3DBaseTexture9* GlowTextureOf(BSShaderProperty* shaderProperty) {
		return HasGlowMap(shaderProperty) ? (*((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[2])->rendererData->dTexture : nullptr;
	}
	// UNOFFICIAL 2026-11-02 the draw's normal map (texture slot 1, the _n.dds) for NormalTextures, or null
	IDirect3DBaseTexture9* NormalTextureOf(BSShaderProperty* shaderProperty) {
		const UInt32 type = static_cast<NiShadeProperty*>((NiProperty*)shaderProperty)->m_eShaderType;
		if (!shaderProperty->IsLightingProperty() && type != NiShadeProperty::kProp_PPLighting && type != NiShadeProperty::kProp_Lighting30) return nullptr;
		NiSourceTexture** normal = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[1];
		return normal && *normal && (*normal)->rendererData ? (*normal)->rendererData->dTexture : nullptr;
	}

	// Where a vertex declaration keeps the position (cached per declaration).
	struct PositionElement {
		int stream = -1; UInt32 offset = 0; NVR_EXT_RT_POSITION_FORMAT format = NVR_EXT_RT_POSITION_FLOAT3;
		int uvStream = -1; UInt32 uvOffset = 0; bool uvHalf = false;	// TEXCOORD0 (FLOAT2 / FLOAT16_2): alpha-tested casters
		// skinned actors: BLENDWEIGHT / BLENDINDICES (stream, offset, NVR_EXT_RT_SKIN_* format, weights stored)
		int weightStream = -1; UInt32 weightOffset = 0, weightFormat = 0, weightCount = 0;
		int indexStream = -1; UInt32 indexOffset = 0, indexFormat = 0;
	};

	struct PendingCopy { int image; IUnknown* target; UINT width, height; };	// 0, 0: the image's registered size

	struct Core {
		enum State { Idle, Starting, Running, Off };
		State		state = Idle;
		std::vector<RtCore::User> users;
		std::vector<const char*> leftOut;				// (2026-11-05) users whose images did not fit (NVR_RT_MAX_IMAGES) at this helper start
		bool		Out(const RtCore::User& user) const { for (const char* n : leftOut) if (n == user.Name) return true; return false; }
		ID3D9NvrExtShare*		share = nullptr;
		ID3D9NvrExtRt*			rt = nullptr;
		ID3D9NvrExtRtSkin*		rtSkin = nullptr;	// fork patch 0033 (skinned actors); null without it
		ID3D9NvrExtRtSeq*		rtSeq = nullptr;	// fork patch 0034 (the arena's sequence texel, 2026-10-30); null without it
		bool		actorsOn = false;					// [Main.Main.RayTracing] Actors (read at each helper start)
		bool		walkingPlayer = false;				// Gather's walk of the player's own node (NVR_RT_MASK_PLAYER on its rigid parts)
		bool		firstPersonBody = true;				// UNOFFICIAL 2026-11-05 [Main.Main.RayTracing] FirstPersonBody: the body is traced in first person too
		bool		walkingFar = false;					// UNOFFICIAL 2026-10-31: Gather's walk of the far object LOD (FarObjectLOD; NVR_RT_MASK_FAR)
		bool		farLodOn = false;					// [Main.Main.RayTracing] FarObjectLOD (live)
		bool		farLandOn = false;					// UNOFFICIAL 2026-10-31 [Main.Main.RayTracing] FarLandLOD (live): LOD land in the same ring
		UInt32		farLandInstances = 0;
		UInt32		farLandOverGrid = 0;				// UNOFFICIAL 2026-11-03: far LOD land quads left out over the loaded cells (Walk)				// far LOD land instances (the scene log)
		float		farRadius = 40000.0f;				// FarRadius: the far ring's outer edge (game units)
		UInt32		farInstances = 0;					// far LOD instances (the scene log)
		UInt32		farWalk[8] = {};					// the far walk's fates (the scene log): seen, culled, out of the ring, geometries, faded, nodes,
														// switch nodes, root missing frames
		float		actorRadius = 3000.0f;				// [Main.Main.RayTracing] ActorRadius (live)
		UInt32		actorInterval = 2;					// [Main.Main.RayTracing] ActorUpdateInterval (live)
		bool		actorStillSkip = false;				// [Main.Main.RayTracing] ActorStillSkip (live, 2026-10-31)
		float		actorStillThreshold = 0.05f;		// [Main.Main.RayTracing] ActorStillThreshold (live)
		UInt32		actorRebuild = 0;					// [Main.Main.RayTracing] ActorRebuildInterval (live; 0 never)
		UInt32		actorUpdates[4] = {};				// since the last line: posed, still (not posed), between poses (ActorUpdateInterval), rebuilds
		UInt32		actorPartitions = 0;				// skinned partitions staged (exchanges 1-60)
		UInt32		partitionFate[5] = {};
		UInt32		skinTex[6] = {};	// hit textures on actors: not PPLighting, no diffuse, texcoords elsewhere, no slot, staged without texcoords, textured				// skipped: no buffer data, too many bones, not described, streams / half positions, palette
		std::vector<ID3D9NvrExtSharedImage*> images;	// [0] = the arena
		std::vector<std::string> imageNames;
		std::vector<NVR_EXT_SHARED_IMAGE_DESC> imageDescs;	// the sizes made (rounded up: kImageAlign)
		std::vector<std::pair<UINT, UINT>> imageSizes;			// the sizes the users asked for (what CopyOut copies)
		bool		inSetup = false;
		ID3D9NvrExtSharedFence*	inputsReady = nullptr;
		ID3D9NvrExtSharedFence*	outputReady = nullptr;
		HANDLE		mapping = nullptr, frameEvent = nullptr, statusEvent = nullptr, process = nullptr, watchdog = nullptr;
		NvrRtShared* shared = nullptr;
		ULONGLONG	startTick = 0;
		UInt64		frame = 0;
		volatile LONG64	lastRecorded = 0;
		volatile LONG	watchStop = 0;
		volatile LONG	tripped = 0;
		char		tripReason[160] = {};
		UInt32		lastSerial = 0;
		UINT		width = 0, height = 0;		// the screen size the users' images were made for
		std::vector<std::string> shaders;		// names, in registration order (the helper's shader indices)
		char		status[200] = "not started";
		bool		loggedOff = false;

		// this frame
		NvrRtFrame*	record = nullptr;			// the ring slot being written (null outside an exchange)
		bool		recording = false;
		std::vector<PendingCopy> copiesOut;
		// UNOFFICIAL 2026-10-28 (RT reflections) the second exchange (RtCore::Exchange): the scene is gathered once per game frame; a
		// later exchange of the same frame sends this frame's instance list again (no geometry ops: those went with the first record).
		UInt32		gatheredFrame = 0;			// TheFrameRateManager->FrameNumber of the last Gather (+1, 0 = never)
		std::vector<NvrRtInstance> frameInstances;
		std::vector<NvrRtInstanceRecord> frameRecords;	// (protocol v2) the instances' records, resent with them
		std::vector<RtCore::GlowLight> glows, frameGlows;	// (2026-10-30) the glow-mapped instances, gathering / this frame's

		// the scene
		std::unordered_map<GeometryKey, GeometryEntry, GeometryKeyHash> geometries;
		std::unordered_map<const void*, PositionElement> declarations;
		UInt32		nextId = 1;
		UInt32		sceneFrame = 0;
		UInt32		arenaUsed = 0, arenaCapacity = 0;
		// Why geometry was left out ([0] objects, [1] land), counted from exchange 1 to 60 and logged with the scene at 60
		enum Skip { kSeen, kNoShader, kSkinned, kNotLighting, kFlags, kLeaf, kAlpha, kDescribe, kMeasure, kTaken, kSkipCount };
		UInt32		skips[3][kSkipCount] = {};	// [0] objects, [1] land, [2] LOD land
		UInt32		landCells[3] = {};		// cells walked, cells without a land node, land nodes culled (APP_CULLED)
		UInt32		landWalk[5] = {};
		// Alpha-tested casters ([Main.Main.RayTracing] AlphaTested, read at each helper start): the alpha atlas (a shared R16F image,
		// 64 x 64 a texture) and the textures already in it
		bool		alphaOn = false;
		// alpha-tested meshes by fate (exchanges 1-60): [0] blended only, [1] feature off, [2] not described, [3] texcoords not in the
		// position's stream, [4] no diffuse texture, [5] alpha unreadable (format / pool) or atlas full, [6] waited (upload budget), [7] taken
		UInt32		alphaFate[8] = {};
		int			atlasImage = -1;
		UInt32		atlasUsed = 0, atlasThisFrame = 0, glowSlots = 0;	// glowSlots: slots with a glow map (GlowTextures)
		std::unordered_map<UInt64, int> alphaSlots;			// diffuse texture | glow texture << 32 -> slot (-1: unreadable)
		IDirect3DTexture9* alphaUpload = nullptr;			// 64 x 64 R16F: the colour pass's alpha target (a scratch since 2026-11-06)
		IDirect3DTexture9* alphaDetailUpload = nullptr;	// (2026-11-06) AlphaDetail x AlphaDetail R16F: one slot's alpha on its way to the alpha atlas
		UInt32		alphaDetail = 256;				// (2026-11-06) [Main.Main.RayTracing] AlphaDetail (64 / 128 / 256, restart): the alpha atlas' texels a texture
		IDirect3DTexture9* albedoUpload = nullptr;			// 64 x 64 A8R8G8B8: one slot's colour on its way to the albedo atlas
		IDirect3DTexture9* albedoDetailUpload = nullptr;	// (2026-11-06) AlbedoDetail x AlbedoDetail A8R8G8B8: the colour at the albedo atlas' size
		IDirect3DTexture9* albedoDetailScratch = nullptr;	// (2026-11-06) its alpha target (R16F, same size; the alpha atlas has its own pass)
		UInt32		albedoDetail = 128;				// (2026-11-06) [Main.Main.RayTracing] AlbedoDetail (64 / 128 / 256, restart): the albedo atlas' texels a texture
		bool		hitTextures = false;				// [Main.Main.RayTracing] HitTextures (read at each helper start)
		bool		plantSway = true;					// [Main.Main.RayTracing] PlantSway (2026-10-30, read each frame)
		UInt32		swayed = 0;							// plant instances leaned (exchanges 1-60, the scene log)
		UInt32		playerInstances = 0;				// the player's body instances (the scene log, with Actors)
		int			albedoAtlasImage = -1;
		// UNOFFICIAL 2026-10-30 glow textures ([Main.Main.RayTracing] GlowTextures, with HitTextures; rt-ao-sky GlowGI / rt-reflections
		// GlowBrightness): a slot is a (diffuse, glow map) pair; the glow map goes into the glow atlas (RGBA8) at the same slot and texcoords
		IDirect3DTexture9* glowUpload = nullptr;
		int			glowAtlasImage = -1;
		// UNOFFICIAL 2026-11-02 normal textures ([Main.Main.RayTracing] NormalTextures, with HitTextures): the normal map at the same slot
		IDirect3DTexture9* normalUpload = nullptr;
		int			normalAtlasImage = -1;
		UInt32		normalSlots = 0;
		PositionElement lastElement;						// Describe's: where the texcoords are
		int			AlphaSlotOf(IDirect3DBaseTexture9* texture, IDirect3DBaseTexture9* glow = nullptr, IDirect3DBaseTexture9* normal = nullptr);		// land subtree: objects popped, APP_CULLED, nodes, faded fade nodes (alpha <= 0, walked anyway), switch nodes
		UInt32		instances = 0, waiting = 0, released = 0;
		float		radius = 8192.0f;
		// UNOFFICIAL 2026-10-30 scene cost (the main session's soak list, item 2), all [Main.Main.RayTracing], live:
		//   Compact: static geometry is compacted by the helper after its build (NVR_RT_OP_FLAG_COMPACT; about half the memory).
		//   BuildTrianglesPerFrame: new geometry farther than BuildNearRadius waits a frame once this many triangles were staged this
		//     frame (0: no limit, as before): a cell change builds its near geometry at once and the rest over the next frames.
		//   CullSmallAngle: objects (not land, not actors) whose bound radius / distance is below this (radians) stay out of the scene.
		bool		compactOn = false;
		UInt32		buildBudget = 0, trianglesThisFrame = 0, budgetWaits = 0, culledSmall = 0;
		float		buildNear = 2048.0f, cullAngle = 0.0f;
		std::vector<UInt32> pendingReleases;	// release ops that did not fit this frame's op list (sent first next frame)
		// UNOFFICIAL 2026-10-30 grass (RtCore::NoteGrass): this frame's grass draws and their tufts within GrassRadius (x y z, size step)
		struct GrassDraw { NiGeometry* geometry; IDirect3DBaseTexture9* texture; UInt32 vertsPerTuft, alphaRef, first, count; float scale[3]; float firstTuft[3]; };
		bool		grassOn = false;					// [Main.Main.RayTracing] Grass (read at each helper start; needs AlphaTested)
		float		grassRadius = 1500.0f;				// GrassRadius (live)
		UInt32		grassMax = 8192;					// GrassMaxTufts (live)
		UInt32		grassFrame = 0;						// TheFrameRateManager->FrameNumber + 1 of grassDraws (0: none)
		std::vector<GrassDraw> grassDraws;
		std::vector<float> grassTufts;
		// grass by fate (exchanges 1-60): [0] draws noted, [1] duplicate draws, [2] tufts met, [3] tufts in reach, [4] over GrassMaxTufts,
		// [5] draws not at the origin, [6] not described, [7] tuft layout not understood, [8] no alpha slot, [9] waited, [10] tufts taken,
		// [11] draws from another frame
		UInt32		grassFate[12] = {};
		double		grassNoteMs = 0.0, grassGatherMs = 0.0;	// CPU time (exchanges 1-60)
		void		AddGrass();
		// UNOFFICIAL 2026-10-30 SpeedTree leaf cards ([Main.Main.RayTracing] LeafCards: 0 off, 2 two crossed upright cards a leaf, 3 and a
		// level one; needs AlphaTested; live): a leaf mesh holds the card's centre at each corner and the leaf shader moves it out along
		// the camera's right / up by the tree model's corner table (SpeedTreeLeafShaderProperty::leafData->leafBase). Here each tree
		// model's leaves are expanded once on the CPU into fixed cards (the corner table along world-fixed axes, no wind), in buffers of
		// our own the fork stages like any mesh; one BLAS per model, an alpha-tested instance per tree (NVR_RT_MASK_ALPHA).
		struct LeafCards { IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr; UInt32 vertices = 0, triangles = 0, made = 0; bool failed = false; };
		std::unordered_map<GeometryKey, LeafCards, GeometryKeyHash> leafCards;
		UInt32		leafPlanes = 0;						// LeafCards (read each frame)
		bool		leafOpaque = false;					// LeafCardsOpaque (debug, live): the cards as opaque objects (mask 0x02), no alpha test
		// leaves by fate (exchanges 1-60 / 600): [0] met, [1] AlphaTested off, [2] no corner table / tree model / texture, [3] no alpha
		// slot, [4] waited, [5] not described / layout not understood, [6] buffers unreadable, [7] instances taken, [8] models built
		UInt32		leafFate[9] = {};
		void		AddLeaves(NiGeometry* geo, BSShaderProperty* shaderProperty, float distance);
		bool		BuildLeafCards(NiGeometry* geo, const float* leafBase, UInt32 planes, LeafCards& out);
		void		FreeLeafCards(bool all);
		// (2026-10-30) tree probe, one frame at the scene log (LeafCards on): every TREE ref of the loaded cells walked without culling, its
		// leaf meshes counted by what would stop the scene walk. [0] tree refs, [1] NotCastShadows, [2] no node, [3] in SceneRadius,
		// [4] leaf meshes, [5] under APP_CULLED, [6] under a faded node, [7] in a switch child not taken, [8] no buffer data, [9] reachable
		bool		treeProbe = false;
		UInt32		treeFate[10] = {};
		void		ProbeTree(TESObjectREFR* ref);
		void		Release(UInt32 id) { pendingReleases.push_back(id); released++; }
		std::vector<NiAVObject*> stack;

		void		TurnOff(const char* why);
		bool		Start();
		void		Stop(const char* why);
		void		Poll();
		void		Gather();
		void		WalkCell(TESObjectCELL* cell);
		void		Walk(NiAVObject* root, bool land, bool lod = false);
		bool		walkTree = false;	// (2026-10-30) Walk of a TREE ref: its root's FadeAlpha is not the game's (0 on most drawn trees, rtleaf-4)
		void		AddGeometry(NiGeometry* geo, bool land, bool lod = false);
		void		AddSkinned(NiGeometry* geo);
		bool		DescribeData(NiGeometryBufferData* data, const char* name, NVR_EXT_RT_GEOMETRY_DESC& desc, std::vector<NVR_EXT_RT_RANGE>& ranges, GeometryKey& key);
		bool		Describe(NiGeometry* geo, NVR_EXT_RT_GEOMETRY_DESC& desc, std::vector<NVR_EXT_RT_RANGE>& ranges, GeometryKey& key);
		void		LogHelperMessage();
		bool		Ready();					// Frame's checks before any exchange (start, poll, screen size): true = may exchange now
		bool		Exchange(IDirect3DDevice9* Device, const RtCore::User* only);	// one frame record: Point-0 users (only = null) or one user
		static DWORD WINAPI WatchdogMain(void* self);
	};

	Core TheCore;

	// A texture's slot in the alpha atlas: >= 0, -1 not possible (never again), -2 not this frame (8 a frame at most). On the GPU: the
	// fork pass RtAlphaToAtlas draws the texture's alpha into a 64 x 64 R16F target (the texture's mips downsample; any pool and format),
	// then that goes into the atlas slot. Both are recorded before this exchange's signal, so the helper sees them this frame.
	int Core::AlphaSlotOf(IDirect3DBaseTexture9* texture, IDirect3DBaseTexture9* glow, IDirect3DBaseTexture9* normal) {
		if (glowAtlasImage < 0) glow = nullptr;
		if (normalAtlasImage < 0) normal = nullptr;	// (the key leaves the normal map out: a diffuse texture comes with one normal map)
		const UInt64 key = (UInt64)(uintptr_t)texture | ((UInt64)(uintptr_t)glow << 32);	// a 32-bit process
		auto found = alphaSlots.find(key);
		if (found != alphaSlots.end()) return found->second;
		const UInt32 capacity = NVR_RT_ALPHA_ATLAS_SLOTS * NVR_RT_ALPHA_ATLAS_SLOTS;
		if (atlasImage < 0 || atlasUsed >= capacity) return -1;
		if (atlasThisFrame >= 8) return -2;
		atlasThisFrame++;
		const UInt32 S = NVR_RT_ALPHA_SLOT_SIZE;
		static ID3D9NvrExtPipeline* pipelines[2] = {};	// [0] alpha only (RtAlphaToAtlas), [1] alpha + colour (RtTextureToAtlas)
		static bool triedPipelines[2] = {};
		const int which = albedoAtlasImage >= 0 ? 1 : 0;
		ID3D9NvrExtPipeline*& pipeline = pipelines[which];
		// (2026-11-06) the alpha-only pass always too: it fills the alpha atlas at AlphaDetail texels (the colour pass stays at 64)
		for (int want = 0; want <= which; want += which ? 1 : 2) {
		ID3D9NvrExtPipeline*& made = pipelines[want];
		if (!triedPipelines[want]) {
			triedPipelines[want] = true;
			std::vector<char> spirv;
			ID3D9NvrExt* ext = DxvkCompute::Extension();
			if (ext && DxvkCompute::LoadSpirv(want ? "RtTextureToAtlas" : "RtAlphaToAtlas", spirv)) {
				NVR_EXT_BINDING_DESC bindings[2] = {
					{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
					{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 4 },
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
				desc.ColorTargetCount = want ? 2 : 1;
				desc.Blend = NVR_EXT_BLEND_NONE;
				desc.ColorWriteMask = 0xFu;
				desc.pDebugName = want ? "RtTextureToAtlas" : "RtAlphaToAtlas";
				if (FAILED(ext->CreateGraphicsPipeline(&desc, &made))) made = nullptr;
			}
			if (!made) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT alpha-tested casters: the alpha pass could not be made (Compute\\RtAlphaToAtlas.spv; the DXVK log says why)");
		}
		}
		int slot = -1;
		if (!alphaUpload) TheRenderManager->device->CreateTexture(S, S, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &alphaUpload, nullptr);
		const UInt32 D = alphaDetail;
		if (!alphaDetailUpload) TheRenderManager->device->CreateTexture(D, D, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &alphaDetailUpload, nullptr);
		if (which && !albedoUpload) TheRenderManager->device->CreateTexture(S, S, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &albedoUpload, nullptr);
		if (glow && !glowUpload) TheRenderManager->device->CreateTexture(S, S, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &glowUpload, nullptr);
		if (normal && !normalUpload) TheRenderManager->device->CreateTexture(S, S, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &normalUpload, nullptr);
		ID3D9NvrExtSampler* sampler = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP);
		IDirect3DSurface9* target = nullptr;
		IDirect3DSurface9* target2 = nullptr;
		if (which && (!albedoUpload || FAILED(albedoUpload->GetSurfaceLevel(0, &target2)))) target2 = nullptr;
		if (pipeline && sampler && alphaUpload && (!which || target2) && SUCCEEDED(alphaUpload->GetSurfaceLevel(0, &target))) {
			NVR_EXT_RESOURCE_BINDING bindings[2] = {};
			bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = texture;
			bindings[1].Slot = 0; bindings[1].Type = NVR_EXT_BIND_SAMPLER; bindings[1].pResource = sampler;
			const UInt32 push[4] = { 0, 0, 0, 0 };
			NVR_EXT_DRAW_DESC draw = {};
			draw.pPipeline = pipeline;
			draw.TargetCount = which ? 2 : 1;
			draw.Targets[0].pResource = target;
			draw.Targets[1].pResource = target2;
			draw.BindingCount = 2;
			draw.pBindings = bindings;
			draw.pPushData = push;
			draw.PushDataSize = sizeof(push);
			const RECT all = { 0, 0, (LONG)S, (LONG)S };
			const UInt32 at = atlasUsed;
			const UInt32 x = (at % NVR_RT_ALPHA_ATLAS_SLOTS) * S, y = (at / NVR_RT_ALPHA_ATLAS_SLOTS) * S;
			// (2026-11-06) the alpha atlas at AlphaDetail texels a texture: the alpha-only pass into its own target (the 64-texel colour pass
			// above keeps the albedo / glow / normal atlases; its alpha target is a scratch). Thin cut-outs (chain-link wires) survive at 256.
			auto detailAlpha = [&]() {
				IDirect3DSurface9* dt = nullptr;
				if (!pipelines[0] || !alphaDetailUpload || FAILED(alphaDetailUpload->GetSurfaceLevel(0, &dt))) return false;
				NVR_EXT_DRAW_DESC d2 = draw;
				d2.pPipeline = pipelines[0]; d2.TargetCount = 1; d2.Targets[0].pResource = dt; d2.Targets[1].pResource = nullptr;
				const RECT allD = { 0, 0, (LONG)D, (LONG)D };
				const bool okD = SUCCEEDED(DxvkCompute::Extension()->Draw(&d2)) &&
					SUCCEEDED(share->CopyToShared(images[atlasImage], (at % NVR_RT_ALPHA_ATLAS_SLOTS) * D, (at / NVR_RT_ALPHA_ATLAS_SLOTS) * D, alphaDetailUpload, &allD));
				dt->Release();
				return okD;
			};
			// (2026-11-06) the albedo atlas at AlbedoDetail texels a texture: the colour pass into targets of that size (glow / normal stay 64).
			// What mirrors show off screen (a person's front, their face) came from a 64 x 64 copy of the texture: smeared.
			auto detailAlbedo = [&]() {
				const UInt32 A = albedoDetail;
				if (A == S) return SUCCEEDED(DxvkCompute::Extension()->Draw(&draw)) && SUCCEEDED(share->CopyToShared(images[albedoAtlasImage], x, y, albedoUpload, &all));
				if (!albedoDetailUpload) TheRenderManager->device->CreateTexture(A, A, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &albedoDetailUpload, nullptr);
				if (!albedoDetailScratch) TheRenderManager->device->CreateTexture(A, A, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &albedoDetailScratch, nullptr);
				IDirect3DSurface9* ta = nullptr;
				IDirect3DSurface9* tc = nullptr;
				bool ok = albedoDetailUpload && albedoDetailScratch && SUCCEEDED(albedoDetailScratch->GetSurfaceLevel(0, &ta)) && SUCCEEDED(albedoDetailUpload->GetSurfaceLevel(0, &tc));
				if (ok) {
					NVR_EXT_DRAW_DESC dA = draw;
					dA.Targets[0].pResource = ta; dA.Targets[1].pResource = tc;
					const RECT allA = { 0, 0, (LONG)A, (LONG)A };
					ok = SUCCEEDED(DxvkCompute::Extension()->Draw(&dA)) &&
						SUCCEEDED(share->CopyToShared(images[albedoAtlasImage], (at % NVR_RT_ALPHA_ATLAS_SLOTS) * A, (at / NVR_RT_ALPHA_ATLAS_SLOTS) * A, albedoDetailUpload, &allA));
				}
				if (ta) ta->Release();
				if (tc) tc->Release();
				return ok;
			};
			if ((!which || detailAlbedo()) && detailAlpha()) {
				slot = (int)at;
				if (glow) {	// the same pass on the glow map: its alpha into the alpha upload (already copied, in stream order: a scratch now)
					IDirect3DSurface9* glowTarget = nullptr;
					bindings[0].pResource = glow;
					if (!glowUpload || FAILED(glowUpload->GetSurfaceLevel(0, &glowTarget))) slot = -1;
					else {
						draw.Targets[1].pResource = glowTarget;
						if (FAILED(DxvkCompute::Extension()->Draw(&draw)) || FAILED(share->CopyToShared(images[glowAtlasImage], x, y, glowUpload, &all))) slot = -1;
						glowTarget->Release();
					}
				}
				if (slot >= 0 && normal) {	// (2026-11-02) the normal map the same way: its rgb as stored (tangent space) into the normal atlas
					IDirect3DSurface9* normalTarget = nullptr;
					bindings[0].pResource = normal;
					if (normalUpload && SUCCEEDED(normalUpload->GetSurfaceLevel(0, &normalTarget))) {
						draw.Targets[1].pResource = normalTarget;
						if (SUCCEEDED(DxvkCompute::Extension()->Draw(&draw)) && SUCCEEDED(share->CopyToShared(images[normalAtlasImage], x, y, normalUpload, &all))) normalSlots++;
						normalTarget->Release();
					}	// (no map in the slot: it reads 0, the shaders keep the triangle's normal)
				}
				if (slot >= 0) { atlasUsed++; if (glow) glowSlots++; }
			}
			target->Release();
		}
		if (target2) target2->Release();
		alphaSlots.emplace(key, slot);
		return slot;
	}
	UInt32 (*MaskHook)(NiGeometry*, bool) = nullptr;

	void Core::TurnOff(const char* why) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: ray tracing core off for this session: %s. The RT effects keep their usual paths.", why);
		_snprintf_s(status, sizeof(status), _TRUNCATE, "off: %s", why);
		Stop(nullptr);
		state = Off;
	}

	void Core::LogHelperMessage() {
		if (!shared || shared->messageSerial == lastSerial) return;
		lastSerial = shared->messageSerial;
		char message[sizeof(shared->message)];
		memcpy(message, (const void*)shared->message, sizeof(message));
		message[sizeof(message) - 1] = 0;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper: %s", message);
	}

	DWORD WINAPI Core::WatchdogMain(void* self) {
		Core* c = (Core*)self;
		UINT64 lastValue = 0;
		ULONGLONG lastProgress = GetTickCount64();
		const char* why = nullptr;
		while (!InterlockedCompareExchange(&c->watchStop, 0, 0)) {
			if (WaitForSingleObject(c->process, 100) == WAIT_OBJECT_0) { why = "the helper process exited (NvrRtHelper.log says why)"; break; }
			UINT64 done = 0;
			if (FAILED(c->outputReady->GetCompletedValue(&done))) continue;
			const LONG64 recorded = InterlockedCompareExchange64(&c->lastRecorded, 0, 0);
			const ULONGLONG now = GetTickCount64();
			if (done != lastValue || (LONG64)done >= recorded) { lastValue = done; lastProgress = now; }
			else if (now - lastProgress > 10000) { why = "no frame came back from the helper for 10 seconds"; break; }
		}
		if (!why) return 0;
		strncpy_s(c->tripReason, why, _TRUNCATE);
		TerminateProcess(c->process, 1);
		WaitForSingleObject(c->process, 2000);
		UINT64 done = 0;
		c->outputReady->GetCompletedValue(&done);
		const UINT64 recorded = (UINT64)InterlockedCompareExchange64(&c->lastRecorded, 0, 0);
		c->outputReady->SignalFromHost((std::max)(done, recorded) + kReleaseMargin);
		InterlockedExchange(&c->tripped, 1);
		return 0;
	}

	bool Core::Start() {
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::Available()) { TurnOff("no Vegas Shaders DXVK (native D3D9 or stock DXVK)"); return false; }
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtShare), (void**)&share)) || !share) {
			share = nullptr;
			TurnOff("this Vegas Shaders DXVK d3d9.dll cannot share images with another process (fork patch 1.7-dlss / 0026 needed)");
			return false;
		}
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtRt), (void**)&rt)) || !rt) {
			rt = nullptr;
			TurnOff("this Vegas Shaders DXVK d3d9.dll has no ray tracing core (fork patch 1.8-rt / 0028 needed)");
			return false;
		}
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtRtSkin), (void**)&rtSkin))) rtSkin = nullptr;
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtRtSeq), (void**)&rtSeq))) rtSeq = nullptr;
		if (!rtSeq) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT: this d3d9.dll has no arena sequence check (fork patch 0034, 1.8-rt-seq); on some GPUs its "
			"geometry copy can run before the geometry is converted (RTX GPU fault, 2026-10-30): update the Vegas Shaders DXVK fork.");
		actorsOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "Actors") != 0;
		if (actorsOn && !rtSkin) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT actors off: this d3d9.dll cannot pose skinned geometry (fork patch 0033, 1.8-rt-skin).");
		NVR_EXT_SHARE_INFO info = {};
		share->GetShareInfo(&info);
		if (!info.Supported) { TurnOff("the Vulkan driver cannot share images and semaphores between processes"); return false; }

		const std::wstring game = GameDir();
		const std::wstring candidates[] = { game, game + L"\\Data\\NVSE\\Plugins\\NvrRt" };
		std::wstring exe, helperDir;
		for (const std::wstring& dir : candidates)
			if (FileExists(dir + L"\\NvrRtHelper.exe")) { helperDir = dir; exe = dir + L"\\NvrRtHelper.exe"; break; }
		if (exe.empty()) { TurnOff("NvrRtHelper.exe was not found (the game folder, or Data\\NVSE\\Plugins\\NvrRt)"); return false; }

		// The images: the arena first, then every user's (registered by their Setup now, at this screen size).
		width = TheRenderManager->width;
		height = TheRenderManager->height;
		imageNames.clear();
		imageDescs.clear();
		imageSizes.clear();
		const UINT arenaMB = (std::max)(4, (std::min)(256, TheSettingManager->GetSettingI("Main.Main.RayTracing", "ArenaMB")));
		imageNames.push_back("NVR RT arena");
		imageDescs.push_back({ kArenaWidth, AlignImage(arenaMB * 1024u * 1024u / 4u / kArenaWidth), NVR_EXT_SHARED_R32F, nullptr });
		imageSizes.push_back({ imageDescs.back().Width, imageDescs.back().Height });
		alphaOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "AlphaTested") != 0;
		hitTextures = TheSettingManager->GetSettingI("Main.Main.RayTracing", "HitTextures") != 0;
		grassOn = alphaOn && TheSettingManager->GetSettingI("Main.Main.RayTracing", "Grass") != 0;	// 2026-10-30
		if (!alphaOn && TheSettingManager->GetSettingI("Main.Main.RayTracing", "Grass") != 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT grass off: it needs [Main.Main.RayTracing] AlphaTested (the tufts are alpha-tested cards).");
		grassDraws.clear();
		grassTufts.clear();
		grassFrame = 0;
		atlasImage = -1;
		albedoAtlasImage = -1;
		glowAtlasImage = -1;
		normalAtlasImage = -1;
		normalSlots = 0;
		atlasUsed = 0;
		glowSlots = 0;
		alphaSlots.clear();
		if (alphaOn || hitTextures) {	// the slots are shared: hit textures keep the alpha atlas too
			atlasImage = (int)imageDescs.size();
			imageNames.push_back("NVR RT alpha atlas");
			// (2026-11-06) AlphaDetail texels a texture (64 / 128 / 256; the shaders read the slot size off the image: RtAlpha.glsl). 256 = an
			// 8192 x 8192 R16F atlas (128 MB): chain-link wires keep their pattern (test host scene chainlink: 81 % of the exact shadow vs 33 % at 64)
			{ const int ad = TheSettingManager->GetSettingI("Main.Main.RayTracing", "AlphaDetail"); alphaDetail = ad >= 256 ? 256u : ad >= 128 ? 128u : 64u; }
			const UINT side = alphaDetail * NVR_RT_ALPHA_ATLAS_SLOTS;
			imageDescs.push_back({ side, side, NVR_EXT_SHARED_R16F, nullptr });
			imageSizes.push_back({ side, side });
		}
		if (hitTextures) {
			albedoAtlasImage = (int)imageDescs.size();
			imageNames.push_back("NVR RT albedo atlas");
			// (2026-11-06) AlbedoDetail texels a texture (64 / 128 / 256; RtHit.glsl reads the slot size off the image): 128 = 4096 x 4096
			// RGBA8 (64 MB), 256 = 256 MB. The glow and normal atlases stay at 64.
			{ const int ad = TheSettingManager->GetSettingI("Main.Main.RayTracing", "AlbedoDetail"); albedoDetail = ad >= 256 ? 256u : ad >= 128 ? 128u : 64u; }
			const UINT albedoSide = albedoDetail * NVR_RT_ALPHA_ATLAS_SLOTS;
			imageDescs.push_back({ albedoSide, albedoSide, NVR_EXT_SHARED_RGBA8, nullptr });
			imageSizes.push_back({ albedoSide, albedoSide });
			const UINT side = NVR_RT_ALPHA_SLOT_SIZE * NVR_RT_ALPHA_ATLAS_SLOTS;
			if (TheSettingManager->GetSettingI("Main.Main.RayTracing", "GlowTextures") != 0) {
				glowAtlasImage = (int)imageDescs.size();
				imageNames.push_back("NVR RT glow atlas");
				imageDescs.push_back({ side, side, NVR_EXT_SHARED_RGBA8, nullptr });
				imageSizes.push_back({ side, side });
			}
			if (TheSettingManager->GetSettingI("Main.Main.RayTracing", "NormalTextures") != 0) {	// UNOFFICIAL 2026-11-02
				normalAtlasImage = (int)imageDescs.size();
				imageNames.push_back("NVR RT normal atlas");
				imageDescs.push_back({ side, side, NVR_EXT_SHARED_RGBA8, nullptr });
				imageSizes.push_back({ side, side });
			}
		}
		inSetup = true;
		// (2026-11-05) users in their Order; one whose images take the count past the protocol's NVR_RT_MAX_IMAGES is left out for the
		// session (its images rolled back) instead of the whole core turning off (rtext-land 11-05: lamps + sun + AO + reflections + the
		// path tracer's 20 asked for more than 48, so nothing traced)
		leftOut.clear();
		const size_t coreImages = imageDescs.size();
		std::vector<const RtCore::User*> order;
		for (const RtCore::User& user : users) order.push_back(&user);
		std::stable_sort(order.begin(), order.end(), [](const RtCore::User* a, const RtCore::User* b) { return a->Order < b->Order; });
		std::string counts;
		for (const RtCore::User* user : order) {
			if (!user->Setup) continue;
			const size_t before = imageDescs.size();
			user->Setup();
			char part[96];
			if (imageDescs.size() > NVR_RT_MAX_IMAGES) {
				_snprintf_s(part, _TRUNCATE, "%s%s %u LEFT OUT", counts.empty() ? "" : ", ", user->Name, (unsigned)(imageDescs.size() - before));
				imageNames.resize(before); imageDescs.resize(before); imageSizes.resize(before);
				leftOut.push_back(user->Name);
				Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: RT: %s is left out this session: its shared images do not fit the %u the protocol holds "
					"(turn off another RT effect and restart the game to get it back).", user->Name, (unsigned)NVR_RT_MAX_IMAGES);
			}
			else _snprintf_s(part, _TRUNCATE, "%s%s %u", counts.empty() ? "" : ", ", user->Name, (unsigned)(imageDescs.size() - before));
			counts += part;
		}
		inSetup = false;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT shared images %u of %u (the core's %u; %s)", (unsigned)imageDescs.size(), (unsigned)NVR_RT_MAX_IMAGES,
			(unsigned)coreImages, counts.c_str());
		if (imageDescs.size() > NVR_RT_MAX_IMAGES) { TurnOff("the RT effects asked for more shared images than the protocol holds"); return false; }
		char why[320];
		images.assign(imageDescs.size(), nullptr);
		for (size_t i = 0; i < imageDescs.size(); i++) {
			imageDescs[i].pDebugName = imageNames[i].c_str();
			const HRESULT hr = share->CreateSharedImage(&imageDescs[i], &images[i]);
			if (FAILED(hr)) {
				sprintf_s(why, "the shared image '%s' could not be made (fork CreateSharedImage: %s)", imageNames[i].c_str(), HrText(hr).c_str());
				TurnOff(why);
				return false;
			}
		}
		arenaCapacity = imageDescs[0].Width * imageDescs[0].Height;
		ID3D9NvrExtSharedFence** fences[2] = { &inputsReady, &outputReady };
		for (UInt32 i = 0; i < 2; i++) {
			const HRESULT hr = share->CreateSharedFence(0, fences[i]);
			if (FAILED(hr)) { sprintf_s(why, "a shared semaphore could not be made (fork CreateSharedFence: %s)", HrText(hr).c_str()); TurnOff(why); return false; }
		}

		wchar_t base[96];
		swprintf_s(base, L"Local\\NvrRt-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NvrRtShared), base);
		shared = mapping ? (NvrRtShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrRtShared)) : nullptr;
		frameEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_RT_EVENT_FRAME_SUFFIX).c_str());
		statusEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_RT_EVENT_STATUS_SUFFIX).c_str());
		if (!shared || !frameEvent || !statusEvent) { TurnOff("its shared-memory block could not be made"); return false; }
		memset(shared, 0, sizeof(NvrRtShared));
		shared->magic = NVR_RT_MAGIC;
		shared->version = NVR_RT_PROTOCOL_VERSION;
		shared->size = sizeof(NvrRtShared);
		shared->parentPid = GetCurrentProcessId();
		shared->vendorId = info.VendorId;
		memcpy(shared->deviceLuid, info.DeviceLUID, 8);
		shared->deviceLuidValid = info.DeviceLUIDValid ? 1u : 0u;
		memcpy(shared->deviceUuid, info.DeviceUUID, 16);
		memcpy(shared->driverUuid, info.DriverUUID, 16);
		// shaders registered so far (the helper compiles them before it says ready)
		const std::string shaderDir = std::string(Paths::ComputeDir()) + "Rt\\";
		char cwd[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, cwd);
		for (size_t i = 0; i < shaders.size() && i < NVR_RT_MAX_SHADERS; i++)
			_snprintf_s(shared->shaderPaths[i], sizeof(shared->shaderPaths[i]), _TRUNCATE, "%s\\%s%s.spv", cwd, shaderDir.c_str(), shaders[i].c_str());
		shared->shaderCount = (uint32_t)(std::min<size_t>)(shaders.size(), NVR_RT_MAX_SHADERS);

		std::wstring cmdLine = L"\"" + exe + L"\" --shm " + base;
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(exe.c_str(), &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS, nullptr, helperDir.c_str(), &si, &pi)) {
			sprintf_s(why, "NvrRtHelper.exe could not be started (CreateProcess: Win32 error %lu)", GetLastError());
			TurnOff(why);
			return false;
		}
		CloseHandle(pi.hThread);
		process = pi.hProcess;

		auto give = [&](const char* what, HRESULT created, HANDLE local, uint64_t& remote) {
			if (FAILED(created) || !local) { sprintf_s(why, "the %s's handle could not be made (fork CreateHandle: %s)", what, HrText(created).c_str()); return false; }
			HANDLE target = nullptr;
			if (!DuplicateHandle(GetCurrentProcess(), local, process, &target, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
				sprintf_s(why, "the %s's handle could not be given to the helper (Win32 error %lu)", what, GetLastError());
				return false;
			}
			remote = (uint64_t)(uintptr_t)target;
			return true;
		};
		shared->imageCount = (uint32_t)images.size();
		for (size_t i = 0; i < images.size(); i++) {
			NVR_EXT_SHARED_IMAGE_INFO image = {};
			images[i]->GetInfo(&image);
			NvrRtImageDesc& d = shared->images[i];
			d.allocationSize = image.AllocationSize;
			d.memoryTypeIndex = image.MemoryTypeIndex;
			d.vkFormat = image.VkFormat;
			d.width = image.Width;
			d.height = image.Height;
			d.usage = image.Usage;
			d.flags = image.Flags;
			d.tiling = image.Tiling;
			d.layout = image.Layout;
			d.handleType = image.HandleType;
			d.dedicated = image.Dedicated ? 1u : 0u;
			HANDLE handle = nullptr;
			const HRESULT hr = images[i]->CreateHandle(&handle);
			if (!give(imageNames[i].c_str(), hr, handle, d.handle)) { TurnOff(why); return false; }
		}
		HANDLE handle = nullptr;
		HRESULT hr = inputsReady->CreateHandle(&handle);
		if (!give("'inputs ready' semaphore", hr, handle, shared->semInputsReady)) { TurnOff(why); return false; }
		handle = nullptr;
		hr = outputReady->CreateHandle(&handle);
		if (!give("'output ready' semaphore", hr, handle, shared->semOutputReady)) { TurnOff(why); return false; }
		MemoryBarrier();
		shared->setupDone = 1;
		SetEvent(frameEvent);

		frame = 0;
		InterlockedExchange64(&lastRecorded, 0);
		InterlockedExchange(&watchStop, 0);
		InterlockedExchange(&tripped, 0);
		watchdog = CreateThread(nullptr, 64 * 1024, WatchdogMain, this, 0, nullptr);
		if (!watchdog) { TurnOff("its watchdog thread could not be started"); return false; }
		startTick = GetTickCount64();
		state = Starting;
		lastSerial = 0;
		geometries.clear();
		FreeLeafCards(true);
		pendingReleases.clear();
		nextId = 1;
		_snprintf_s(status, sizeof(status), _TRUNCATE, "starting on %s", info.DeviceName);
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper started (%ls): %u shared images (arena %ux%u), %u shaders, GPU %s.", exe.c_str(),
			(unsigned)images.size(), imageDescs[0].Width, imageDescs[0].Height, (unsigned)shaders.size(), info.DeviceName);
		return true;
	}

	void Core::Stop(const char* why) {
		if (why && state != Idle && state != Off) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper stopped: %s.", why);
		if (watchdog) {
			InterlockedExchange(&watchStop, 1);
			WaitForSingleObject(watchdog, 3000);
			CloseHandle(watchdog);
			watchdog = nullptr;
		}
		const UInt64 recorded = (UInt64)InterlockedCompareExchange64(&lastRecorded, 0, 0);
		if (process) {
			if (shared && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
				for (int i = 0; i < 200 && (UInt64)shared->submittedFrame < recorded; i++) Sleep(5);
				shared->quit = 1;
				SetEvent(frameEvent);
				if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) TerminateProcess(process, 1);
			}
			CloseHandle(process);
			process = nullptr;
		}
		if (outputReady) {
			UINT64 done = 0;
			outputReady->GetCompletedValue(&done);
			if (done < recorded) outputReady->SignalFromHost(recorded + kReleaseMargin);
		}
		for (ID3D9NvrExtSharedImage*& image : images) DxvkCompute::Drop(image);
		images.clear();
		DxvkCompute::Drop(inputsReady);
		DxvkCompute::Drop(outputReady);
		DxvkCompute::Drop(rt);
		DxvkCompute::Drop(rtSkin);
		DxvkCompute::Drop(rtSeq);
		DxvkCompute::Drop(share);
		if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
		HANDLE* handles[] = { &mapping, &frameEvent, &statusEvent };
		for (HANDLE* h : handles) if (*h) { CloseHandle(*h); *h = nullptr; }
		geometries.clear();
		FreeLeafCards(true);
		pendingReleases.clear();
		alphaSlots.clear();
		if (alphaUpload) { alphaUpload->Release(); alphaUpload = nullptr; }
		if (alphaDetailUpload) { alphaDetailUpload->Release(); alphaDetailUpload = nullptr; }
		if (albedoUpload) { albedoUpload->Release(); albedoUpload = nullptr; }
		if (albedoDetailUpload) { albedoDetailUpload->Release(); albedoDetailUpload = nullptr; }
		if (albedoDetailScratch) { albedoDetailScratch->Release(); albedoDetailScratch = nullptr; }
		if (glowUpload) { glowUpload->Release(); glowUpload = nullptr; }
	}

	void Core::Poll() {
		LogHelperMessage();
		if (state == Starting) {
			const UInt32 helperState = shared->state;
			if (helperState == NVR_RT_STATE_READY) {
				state = Running;
				_snprintf_s(status, sizeof(status), _TRUNCATE, "running");
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper ready after %llu ms; ray tracing from the next frame on.", (unsigned long long)(GetTickCount64() - startTick));
			}
			else if (helperState == NVR_RT_STATE_FAILED || helperState == NVR_RT_STATE_EXITED) TurnOff("the helper gave up (its message is above)");
			else if (InterlockedCompareExchange(&tripped, 0, 0)) TurnOff(tripReason);
			// (2026-11-03: 15 -> 60 s: the helper compiles every registered trace shader before it says ready, ~1 s each on the iGPU; with
			// 12 it was ready at 15.4 s and the core had turned itself off (warm slot, Lucky 38). The game never waits for it meanwhile.)
			else if (GetTickCount64() - startTick > 60000) TurnOff("the helper did not get ready within 60 seconds");
			return;
		}
		if (state == Running) {
			if (InterlockedCompareExchange(&tripped, 0, 0)) TurnOff(tripReason);
			else if (shared->state == NVR_RT_STATE_FAILED) TurnOff("the helper stopped (its message is above)");
		}
	}

	// --- The scene ----------------------------------------------------------------------------------------------------------

	bool PositionOf(IDirect3DVertexDeclaration9* declaration, PositionElement& out) {
		D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
		UINT count = 0;
		if (!declaration || FAILED(declaration->GetDeclaration(nullptr, &count)) || count > MAXD3DDECLLENGTH + 1) return false;
		if (FAILED(declaration->GetDeclaration(elements, &count))) return false;
		for (UINT i = 0; i < count && elements[i].Stream != 0xFF; i++) {
			const D3DVERTEXELEMENT9& e = elements[i];
			if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0 && (e.Type == D3DDECLTYPE_FLOAT2 || e.Type == D3DDECLTYPE_FLOAT16_2)) {
				out.uvStream = e.Stream; out.uvOffset = e.Offset; out.uvHalf = e.Type == D3DDECLTYPE_FLOAT16_2;
			}
			if (e.Usage == D3DDECLUSAGE_BLENDWEIGHT && e.UsageIndex == 0) {
				UInt32 format = 99, count = 0;
				switch (e.Type) {
				case D3DDECLTYPE_FLOAT1: format = NVR_EXT_RT_SKIN_WEIGHTS_FLOAT; count = 1; break;
				case D3DDECLTYPE_FLOAT2: format = NVR_EXT_RT_SKIN_WEIGHTS_FLOAT; count = 2; break;
				case D3DDECLTYPE_FLOAT3: format = NVR_EXT_RT_SKIN_WEIGHTS_FLOAT; count = 3; break;
				case D3DDECLTYPE_FLOAT4: format = NVR_EXT_RT_SKIN_WEIGHTS_FLOAT; count = 4; break;
				case D3DDECLTYPE_FLOAT16_2: format = NVR_EXT_RT_SKIN_WEIGHTS_HALF; count = 2; break;
				case D3DDECLTYPE_FLOAT16_4: format = NVR_EXT_RT_SKIN_WEIGHTS_HALF; count = 4; break;
				case D3DDECLTYPE_UBYTE4N: format = NVR_EXT_RT_SKIN_WEIGHTS_UBYTE4N; count = 4; break;
				default: break;
				}
				if (format != 99) { out.weightStream = e.Stream; out.weightOffset = e.Offset; out.weightFormat = format; out.weightCount = count; }
			}
			if (e.Usage == D3DDECLUSAGE_BLENDINDICES && e.UsageIndex == 0 && (e.Type == D3DDECLTYPE_UBYTE4 || e.Type == D3DDECLTYPE_D3DCOLOR)) {
				out.indexStream = e.Stream; out.indexOffset = e.Offset;
				out.indexFormat = e.Type == D3DDECLTYPE_D3DCOLOR ? NVR_EXT_RT_SKIN_INDICES_D3DCOLOR : NVR_EXT_RT_SKIN_INDICES_UBYTE4;
			}
			if (e.Usage != D3DDECLUSAGE_POSITION || e.UsageIndex != 0 || out.stream >= 0) continue;
			if (e.Type == D3DDECLTYPE_FLOAT3) out.format = NVR_EXT_RT_POSITION_FLOAT3;
			else if (e.Type == D3DDECLTYPE_FLOAT4) out.format = NVR_EXT_RT_POSITION_FLOAT4;
			else if (e.Type == D3DDECLTYPE_FLOAT16_4) out.format = NVR_EXT_RT_POSITION_HALF4;
			else return false;
			out.stream = e.Stream;
			out.offset = e.Offset;
		}
		return out.stream >= 0;
	}

	bool Core::Describe(NiGeometry* geo, NVR_EXT_RT_GEOMETRY_DESC& desc, std::vector<NVR_EXT_RT_RANGE>& ranges, GeometryKey& key) {
		return DescribeData(geo->geomData->m_pkBuffData, geo->m_pcName, desc, ranges, key);
	}

	bool Core::DescribeData(NiGeometryBufferData* data, const char* name, NVR_EXT_RT_GEOMETRY_DESC& desc, std::vector<NVR_EXT_RT_RANGE>& ranges, GeometryKey& key) {
		if (!data || !data->VertCount || !data->IB || !data->VBChip || !data->VertexStride || !data->StreamCount) return false;
		PositionElement position;
		if (data->FVF) {
			if ((data->FVF & D3DFVF_POSITION_MASK) != D3DFVF_XYZ) return false;
			position.stream = 0;
			position.offset = 0;
		}
		else {
			auto found = declarations.find(data->VertexDeclaration);
			if (found == declarations.end()) {
				PositionElement e;
				if (!PositionOf(data->VertexDeclaration, e)) e.stream = -1;
				found = declarations.emplace(data->VertexDeclaration, e).first;
			}
			position = found->second;
		}
		if (position.stream < 0 || (UInt32)position.stream >= data->StreamCount || !data->VBChip[position.stream] || !data->VBChip[position.stream]->VB) return false;

		lastElement = position;
		const UInt32 stride = data->VertexStride[position.stream];
		key = { data, data->VBChip[position.stream]->VB, data->IB, data->VertCount, data->TriCount, data->BaseVertexIndex };

		// The draws RenderPass::DrawGeometryBuffer makes (the game's RenderTriStripsAlt / RenderTriShapeAlt): one per strip, or the list.
		ranges.clear();
		if (data->PrimitiveType == D3DPT_TRIANGLESTRIP) {
			UInt32 start = 0;
			for (UInt32 i = 0; i < data->NumArrays; i++) {
				const UInt32 length = data->ArrayLengths ? data->ArrayLengths[i] : data->TriCount + 2;
				if (length >= 3) ranges.push_back({ NVR_EXT_RT_TRIANGLE_STRIP, start, length - 2, 0 });
				start += length;
			}
		}
		else if (data->PrimitiveType == D3DPT_TRIANGLELIST) ranges.push_back({ NVR_EXT_RT_TRIANGLE_LIST, 0, data->TriCount, 0 });
		if (ranges.empty() || ranges.size() > NVR_EXT_RT_MAX_RANGES) return false;

		desc = {};
		desc.pVertexBuffer = data->VBChip[position.stream]->VB;
		desc.PositionOffset = position.offset + data->BaseVertexIndex * stride;	// indices count from this geometry's first vertex
		desc.VertexStride = stride;
		desc.VertexCount = data->VertCount;
		desc.PositionFormat = position.format;
		desc.pIndexBuffer = data->IB;
		desc.RangeCount = (UINT)ranges.size();
		desc.pRanges = ranges.data();
		desc.pDebugName = name;
		return true;
	}

	// UNOFFICIAL 2026-10-29 skinned actors ([Main.Main.RayTracing] Actors; fork patch 0033 ID3D9NvrExtRtSkin): every partition of a
	// skinned geometry, posed every frame by the fork (palette = bone world x SkinToBone, Gamebryo's skinning, scene-relative), built
	// fast in the helper, placed with the identity. Mask NVR_RT_MASK_ACTOR.
	void Core::AddSkinned(NiGeometry* geo) {
		UInt32* skip = skips[0];
		NiSkinInstance* skin = geo->skinInstance;
		NiSkinPartition* parts = skin ? skin->SkinPartition : nullptr;
		if (!rtSkin || !parts || !parts->Partitions || !skin->SkinData || !skin->SkinData->BoneData || !skin->BoneObjects) { skip[kSkinned]++; return; }
		// Any shader: the bodies' skin shader is not a lighting property (rtsun-16: only the hidden dismemberment caps were in)
		BSShaderProperty* shaderProperty = (BSShaderProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Shade);
		if (!shaderProperty) { skip[kNotLighting]++; return; }
		// Dismemberment: a skin that does not render (the gore caps until a limb comes off) or a disabled partition stays out
		BSDismemberSkinInstance* dismember = *(void**)skin == Pointers::VirtualTables::BSDismemberSkinInstance ? (BSDismemberSkinInstance*)skin : nullptr;
		if (dismember && !dismember->IsRenderable) { skip[kSkinned]++; return; }
		NiAlphaProperty* alpha = (NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha);
		// Bodies are alpha-tested (the hair and fringe edges): they go in opaque (rtsun-13: with them left out, only heads and hands were
		// in the scene). Blended-only (glass, fx) stays out.
		if (alpha && (alpha->flags & NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK) && !(alpha->flags & NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK)) { skip[kAlpha]++; return; }
		// The skin shader is not a "lighting property" by its shader index, but its class is BSShaderPPLightingProperty (the textures)
		const UInt32 shaderType = static_cast<NiShadeProperty*>((NiProperty*)shaderProperty)->m_eShaderType;
		const bool ppLighting = shaderProperty->IsLightingProperty() || shaderType == NiShadeProperty::kProp_PPLighting || shaderType == NiShadeProperty::kProp_Lighting30;
		static const Albedo grey = { { 0.5f, 0.5f, 0.5f }, false };
		const Albedo& albedo = ppLighting ? AlbedoOf(geo, shaderProperty) : grey;
		IDirect3DBaseTexture9* diffuse = nullptr;	// hit textures: the body's diffuse texture
		if (hitTextures && !ppLighting) skinTex[0]++;
		if (hitTextures && ppLighting) {
			NiSourceTexture** textures = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[0];
			NiTexture* texture = textures ? *textures : nullptr;
			if (texture && texture->rendererData) diffuse = texture->rendererData->dTexture;
			if (!diffuse) skinTex[1]++;
		}
		const float cx = TheRenderManager->CameraPosition.x, cy = TheRenderManager->CameraPosition.y, cz = TheRenderManager->CameraPosition.z;
		static std::vector<NVR_EXT_RT_RANGE> ranges;
		static std::vector<float> palette;
		static int told = 0;
		if (told < 16) {	// the first skinned geometries, once: what their partitions look like
			told++;
			char line[512];
			int n = sprintf_s(line, "RT CORE: skinned '%s': %u partitions, own buffer data %p:", geo->m_pcName ? geo->m_pcName : "", parts->PartitionsCount,
				geo->geomData ? geo->geomData->m_pkBuffData : nullptr);
			for (UInt32 p = 0; p < parts->PartitionsCount && p < 8 && n > 0 && n < 440; p++) {
				const NiSkinPartition::Partition& part = parts->Partitions[p];
				n += sprintf_s(line + n, sizeof(line) - n, " [%u: buf %p, %u bones, %u a vertex, %u verts, %u tris]", p, part.BuffData, part.Bones,
					part.BonesPerVertex, part.Vertices, part.Triangles);
			}
			Logger::Log("%s", line);
		}
		for (UInt32 p = 0; p < parts->PartitionsCount; p++) {
			const NiSkinPartition::Partition& part = parts->Partitions[p];
			if (!part.BuffData) { partitionFate[0]++; continue; }
			if (dismember && dismember->partitions && p < dismember->partitionNumber && !dismember->partitions[p].Enabled) { partitionFate[1]++; continue; }
			if (!part.pBones || !part.Bones || part.Bones > NVR_EXT_RT_SKIN_MAX_BONES || !part.BonesPerVertex) { partitionFate[1]++; continue; }
			NVR_EXT_RT_GEOMETRY_DESC desc;
			GeometryKey key;
			if (!DescribeData(part.BuffData, geo->m_pcName, desc, ranges, key)) { skip[kDescribe]++; partitionFate[2]++; continue; }
			key.owner = skin;	// (2026-11-06) one entry per actor (clones share the partition data)
			const PositionElement& e = lastElement;
			if (e.weightStream != e.stream || e.indexStream != e.stream || e.format == NVR_EXT_RT_POSITION_HALF4) { skip[kDescribe]++; partitionFate[3]++; continue; }
			// the palette: partition bone k = the skin's bone part.pBones[k]
			palette.resize(part.Bones * 12u);
			bool ok = true;
			for (UInt32 k = 0; k < part.Bones && ok; k++) {
				const UInt32 g = part.pBones[k];
				NiAVObject* bone = g < skin->SkinData->Bones ? skin->BoneObjects[g] : nullptr;
				if (!bone) { ok = false; break; }
				const NiTransform& a = bone->m_worldTransform;
				const NiTransform& b = skin->SkinData->BoneData[g].SkinToBone;
				float* m = &palette[k * 12u];
				const float scale = a.scale * b.scale;
				for (int r = 0; r < 3; r++) {
					for (int c = 0; c < 3; c++)
						m[r * 4 + c] = scale * (a.rot.data[r][0] * b.rot.data[0][c] + a.rot.data[r][1] * b.rot.data[1][c] + a.rot.data[r][2] * b.rot.data[2][c]);
					const float t = a.rot.data[r][0] * b.pos.x + a.rot.data[r][1] * b.pos.y + a.rot.data[r][2] * b.pos.z;
					m[r * 4 + 3] = (r == 0 ? a.pos.x : r == 1 ? a.pos.y : a.pos.z) + a.scale * t - (r == 0 ? cx : r == 1 ? cy : cz);
				}
			}
			if (!ok) { skip[kSkinned]++; partitionFate[4]++; continue; }
			NVR_EXT_RT_SKIN_DESC s = {};
			s.WeightsDelta = (INT)e.weightOffset - (INT)e.offset;
			s.WeightsFormat = (NVR_EXT_RT_SKIN_WEIGHTS)e.weightFormat;
			s.WeightCount = (std::min)(e.weightCount, (UInt32)part.BonesPerVertex);
			s.BonesPerVertex = (std::min)((UInt32)part.BonesPerVertex, (UInt32)4);
			s.IndicesDelta = (INT)e.indexOffset - (INT)e.offset;
			// The game's skin shaders read the indices as blendIndices.zyxw * 765.01 (EnvTemplate.hlsl): a D3DCOLOR's swizzle undone, so
			// bone k is byte k in memory whatever the declared type (rtsun-14: un-swizzling it again posed the multi-bone torso wrong)
			s.IndicesFormat = NVR_EXT_RT_SKIN_INDICES_UBYTE4;
			s.PaletteCount = part.Bones;
			s.pPalette = palette.data();
			// Hit textures: the texcoords go with the posed positions (fork 0033 stages them for skinned geometry too), the colour in the atlas
			int slot = -1;
			if (diffuse && e.uvStream != e.stream) skinTex[2]++;
			if (diffuse && e.uvStream == e.stream) {
				slot = AlphaSlotOf(diffuse);
				if (slot >= 0) desc.Flags = NVR_EXT_RT_TEXCOORD_FLAGS((int)e.uvOffset - (int)e.offset, e.uvHalf);
				else skinTex[3]++;
			}

			GeometryEntry& entry = geometries[key];
			if (entry.refused) continue;
			entry.lastSeen = sceneFrame;
			entry.skinned = true;
			if (!entry.id) entry.id = nextId++ & 0xFFFFFF;
			// ActorUpdateInterval: a posed partition is re-posed only every Nth frame (staggered by id), the frames between keep the
			// last pose (a frame or two late: unseen in shadows; rt-lamp: actors were most of the RT cost indoors, ~60 partitions a frame)
			bool pose = !entry.sent || actorInterval <= 1 || ((sceneFrame + entry.id) % actorInterval) == 0;
			if (!pose) actorUpdates[2]++;
			// UNOFFICIAL 2026-10-31 ActorStillSkip: no bone moved (world space) since its last pose: the structure stays. A rotation term
			// moves a vertex by about its distance from the model's origin (a body: up to ~128 units), so it gets threshold / 128.
			if (pose && actorStillSkip && entry.sent && entry.palette.size() == palette.size()) {
				const float moveLimit = actorStillThreshold, turnLimit = actorStillThreshold / 128.0f;
				const float cam[3] = { cx, cy, cz };
				bool still = true;
				for (size_t i = 0; i < palette.size() && still; i++) {
					const int c = (int)(i % 4u), r = (int)((i % 12u) / 4u);
					const float now = c == 3 ? palette[i] + cam[r] : palette[i];
					still = fabsf(now - entry.palette[i]) <= (c == 3 ? moveLimit : turnLimit);
				}
				if (still) { pose = false; actorUpdates[1]++; }
			}
			NVR_EXT_RT_GEOMETRY_INFO info = {};
			if (pose && FAILED(rt->MeasureGeometry(&desc, &info))) { entry.refused = true; skip[kMeasure]++; continue; }
			if (record->instanceCount >= NVR_RT_MAX_INSTANCES) { waiting++; continue; }
			bool hasUv = entry.textured;	// what the helper has: decided by what the fork staged (an older 0033 drops skinned texcoords)
			if (pose) {
				if (record->opCount >= NVR_RT_MAX_OPS || arenaUsed + info.Texels + 1 > arenaCapacity) { waiting++; if (!entry.sent) continue; }
				else if (FAILED(rtSkin->StageSkinnedGeometry(&desc, &s, images[0], arenaUsed, &info))) { entry.refused = true; skip[kMeasure]++; continue; }
				else {
					NvrRtGeometryOp& op = record->ops[record->opCount++];
					op = NvrRtGeometryOp();
					op.op = NVR_RT_OP_BUILD;
					op.id = entry.id;
					op.vertexCount = info.VertexCount;
					op.triangleCount = info.TriangleCount;
					op.arenaTexel = arenaUsed;
					hasUv = slot >= 0 && info.Texels == 3u * (info.VertexCount + info.TriangleCount) + 2u * info.VertexCount;
					entry.textured = hasUv;
					if (slot >= 0) skinTex[hasUv ? 5 : 4]++;
					op.flags = NVR_RT_OP_FLAG_DYNAMIC | (hasUv ? NVR_RT_OP_FLAG_TEXCOORDS : 0u);
					// 2026-10-31 ActorRebuildInterval: every Nth pose built afresh (the helper refits the others)
					if (entry.sent && actorRebuild && (++entry.poses % actorRebuild) == 0) { op.flags |= NVR_RT_OP_FLAG_REBUILD; actorUpdates[3]++; }
					arenaUsed += info.Texels;
					entry.sent = true;
					actorPartitions++;
					actorUpdates[0]++;
					// 2026-10-31: the camera this pose is relative to, and its world-space palette (ActorStillSkip)
					entry.poseCamera[0] = cx; entry.poseCamera[1] = cy; entry.poseCamera[2] = cz;
					if (actorStillSkip) {
						entry.palette.resize(palette.size());
						for (size_t i = 0; i < palette.size(); i++) {
							const int c = (int)(i % 4u), r = (int)((i % 12u) / 4u);
							entry.palette[i] = c == 3 ? palette[i] + (r == 0 ? cx : r == 1 ? cy : cz) : palette[i];
						}
					}
					else entry.palette.clear();
				}
			}

			NvrRtInstanceRecord& extra = record->records[record->instanceCount];
			extra.albedo[0] = albedo.rgb[0]; extra.albedo[1] = albedo.rgb[1]; extra.albedo[2] = albedo.rgb[2]; extra.albedo[3] = 1.0f;
			extra.flags = (albedo.known ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | (hasUv && slot >= 0 ? NVR_RT_RECORD_TEXTURED | ((UInt32)slot << 16) : 0u);
			extra.geometryId = entry.id;
			extra.vertexOffset = extra.indexOffset = 0;
			NvrRtInstance& instance = record->instances[record->instanceCount++];
			memset(instance.transform, 0, sizeof(instance.transform));
			instance.transform[0][0] = instance.transform[1][1] = instance.transform[2][2] = 1.0f;	// posed in scene space already
			// ... relative to the camera of its last pose (2026-10-31: frames without a pose kept the old offset and slid with the camera)
			instance.transform[0][3] = entry.poseCamera[0] - cx;
			instance.transform[1][3] = entry.poseCamera[1] - cy;
			instance.transform[2][3] = entry.poseCamera[2] - cz;
			instance.geometryId = entry.id;
			instance.customIndex = 0;
			instance.mask = NVR_RT_MASK_ACTOR;
			instance.flags = 0x1u;
			skip[kTaken]++;
		}
	}

	// The leaf mesh's vertices and triangles, read back once (read-only locks of the game's buffers), each vertex moved out to its card
	// corner along fixed axes per plane: plane 0 along x and z, plane 1 along y and z, plane 2 along x and y. The corner is the leaf
	// shader's at rest (Speedtree.hlsl SpeedTreeTransform with no rustle, rock or wind): leafBase[int(z)] * w, its y along the card's
	// right and its z along its up.
	bool Core::BuildLeafCards(NiGeometry* geo, const float* leafBase, UInt32 planes, LeafCards& out) {
		NiGeometryBufferData* bd = geo->geomData->m_pkBuffData;
		const LeafLayout& l = LeafLayoutOf(bd->VertexDeclaration);
		static int told = 0;
		if (!l.ok || bd->FVF || (UInt32)l.stream >= bd->StreamCount || !bd->VBChip[l.stream] || !bd->VBChip[l.stream]->VB || !bd->IB) {
			if (told < 4) {
				told++;
				Logger::Log("RT CORE leaves: '%s' layout not understood: FVF %X, stream %d of %u, element types %u %u %u (position, texcoord, blend indices)",
					geo->m_pcName ? geo->m_pcName : "", bd->FVF, l.stream, bd->StreamCount, l.types[0], l.types[1], l.types[2]);
			}
			leafFate[5]++;
			return false;
		}
		static std::vector<NVR_EXT_RT_RANGE> ranges;
		NVR_EXT_RT_GEOMETRY_DESC desc;
		GeometryKey key;
		if (!Describe(geo, desc, ranges, key)) { leafFate[5]++; return false; }
		const UInt32 stride = bd->VertexStride[l.stream], V = bd->VertCount;
		IDirect3DVertexBuffer9* vb = bd->VBChip[l.stream]->VB;
		D3DVERTEXBUFFER_DESC vd = {};
		D3DINDEXBUFFER_DESC id = {};
		vb->GetDesc(&vd);
		bd->IB->GetDesc(&id);
		const UInt32 first = bd->BaseVertexIndex * stride;
		if (first + V * stride > vd.Size) { leafFate[6]++; return false; }

		struct Corner { float p[3]; float uv[2]; float corner[2]; };	// corner: the card offset's right and up amounts
		static std::vector<Corner> corners;
		corners.resize(V);
		void* bits = nullptr;
		if (FAILED(vb->Lock(first, V * stride, &bits, D3DLOCK_READONLY)) || !bits) { leafFate[6]++; return false; }
		const UInt8* base = (const UInt8*)bits;
		bool sane = true;
		const NiBound& bound = geo->geomData->m_kBound;
		const float reach = bound.Radius * 4.0f + 512.0f;
		for (UInt32 i = 0; i < V && sane; i++) {
			const UInt8* v = base + i * stride;
			Corner& c = corners[i];
			memcpy(c.p, v + l.position, 12);
			if (l.uvHalf) { c.uv[0] = HalfToFloat(*(const UInt16*)(v + l.uv)); c.uv[1] = HalfToFloat(*(const UInt16*)(v + l.uv + 2)); }
			else memcpy(c.uv, v + l.uv, 8);
			float b[4];
			memcpy(b, v + l.blend, 16);
			const int leaf = (int)floorf(b[2]);
			if (!(leaf >= 0 && leaf < 48) || !std::isfinite(b[3])) { sane = false; break; }
			c.corner[0] = leafBase[leaf * 4 + 1] * b[3];
			c.corner[1] = leafBase[leaf * 4 + 2] * b[3];
			const float dx = c.p[0] - bound.Center.x, dy = c.p[1] - bound.Center.y, dz = c.p[2] - bound.Center.z;
			if (!std::isfinite(dx + dy + dz + c.corner[0] + c.corner[1]) || dx * dx + dy * dy + dz * dz > reach * reach ||
				fabsf(c.corner[0]) > reach || fabsf(c.corner[1]) > reach) sane = false;
		}
		vb->Unlock();

		static std::vector<UInt32> triangles;
		triangles.clear();
		if (sane) {
			const bool wide = id.Format == D3DFMT_INDEX32;
			void* ibits = nullptr;
			if (FAILED(bd->IB->Lock(0, 0, &ibits, D3DLOCK_READONLY)) || !ibits) { leafFate[6]++; return false; }
			const UInt32 indexCount = id.Size / (wide ? 4u : 2u);
			auto index = [&](UInt32 k) { return wide ? ((const UInt32*)ibits)[k] : (UInt32)((const UInt16*)ibits)[k]; };
			for (const NVR_EXT_RT_RANGE& r : ranges) {
				const bool strip = r.Topology == NVR_EXT_RT_TRIANGLE_STRIP;
				const UInt32 needed = r.StartIndex + (strip ? r.PrimitiveCount + 2 : r.PrimitiveCount * 3);
				if (needed > indexCount) { sane = false; break; }
				for (UInt32 t = 0; t < r.PrimitiveCount; t++) {
					const UInt32 k = r.StartIndex + (strip ? t : t * 3);
					const UInt32 a = index(k), b = index(k + 1), c = index(k + 2);
					if (a >= V || b >= V || c >= V || a == b || b == c || a == c) continue;	// strip joins
					triangles.push_back(a); triangles.push_back(b); triangles.push_back(c);
				}
			}
			bd->IB->Unlock();
		}
		if (told < 3) {	// the first vertices as read: position, texcoord, corner
			for (UInt32 i = 0; i < V && i < 4; i++)
				Logger::Log("RT CORE leaves:   vertex %u: %.1f %.1f %.1f uv %.3f %.3f corner %.1f %.1f", i, corners[i].p[0], corners[i].p[1], corners[i].p[2],
					corners[i].uv[0], corners[i].uv[1], corners[i].corner[0], corners[i].corner[1]);
		}
		if (told < 3 || !sane) {
			if (told < 6) {
				told++;
				float size = 0.0f;
				for (UInt32 i = 0; i < V; i++) size += fabsf(corners[i].corner[0]) + fabsf(corners[i].corner[1]);
				Logger::Log("RT CORE leaves: '%s': %u vertices (stride %u, pool %u, usage %X), %u triangles from %u ranges, mean corner offset %.1f, "
					"bound r %.0f, %s", geo->m_pcName ? geo->m_pcName : "", V, stride, (unsigned)vd.Pool, (unsigned)vd.Usage, (unsigned)triangles.size() / 3,
					(unsigned)ranges.size(), V ? size / (2.0f * V) : 0.0f, bound.Radius, sane ? "built" : "NOT SANE (unreadable buffer?): left out");
			}
		}
		if (!sane || triangles.empty()) { leafFate[6]++; return false; }

		static const float right[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 1, 0, 0 } }, up[3][3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 } };
		const UInt32 T = (UInt32)triangles.size() / 3;
		IDirect3DDevice9* device = TheRenderManager->device;
		if (FAILED(device->CreateVertexBuffer(planes * V * 20u, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &out.vb, nullptr)) ||
			FAILED(device->CreateIndexBuffer(planes * T * 12u, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, &out.ib, nullptr))) {
			if (out.vb) { out.vb->Release(); out.vb = nullptr; }
			leafFate[6]++;
			return false;
		}
		float* vout = nullptr;
		UInt32* iout = nullptr;
		if (FAILED(out.vb->Lock(0, 0, (void**)&vout, 0)) || FAILED(out.ib->Lock(0, 0, (void**)&iout, 0))) {
			if (vout) out.vb->Unlock();
			out.vb->Release(); out.ib->Release(); out.vb = nullptr; out.ib = nullptr;
			leafFate[6]++;
			return false;
		}
		for (UInt32 p = 0; p < planes; p++) {
			for (UInt32 i = 0; i < V; i++) {
				const Corner& c = corners[i];
				for (int k = 0; k < 3; k++) *vout++ = c.p[k] + right[p][k] * c.corner[0] + up[p][k] * c.corner[1];
				*vout++ = c.uv[0];
				*vout++ = c.uv[1];
			}
			for (UInt32 k = 0; k < T * 3; k++) *iout++ = triangles[k] + p * V;
		}
		out.vb->Unlock();
		out.ib->Unlock();
		out.vertices = planes * V;
		out.triangles = planes * T;
		out.made = sceneFrame;
		leafFate[8]++;
		return true;
	}

	void Core::FreeLeafCards(bool all) {
		for (auto it = leafCards.begin(); it != leafCards.end(); ) {
			LeafCards& lc = it->second;
			// a built model's buffers are needed only until its BUILD was staged and flushed (a few frames); a failed model stays (not re-read)
			if (all || (!lc.failed && sceneFrame - lc.made > 8)) {
				if (lc.vb) lc.vb->Release();
				if (lc.ib) lc.ib->Release();
				it = leafCards.erase(it);
			}
			else ++it;
		}
	}

	void Core::AddLeaves(NiGeometry* geo, BSShaderProperty* shaderProperty, float distance) {
		UInt32* skip = skips[0];
		leafFate[0]++;
		if (!alphaOn) { leafFate[1]++; skip[kLeaf]++; return; }
		const SpeedTreeLeafShaderProperty* st = (const SpeedTreeLeafShaderProperty*)shaderProperty;
		const float* leafBase = st->leafData ? st->leafData->leafBase : nullptr;
		// the tree's node: geometry -> leaves node -> BSTreeNode (as SpeedTreeShadowRenderPass)
		const BSTreeNode* tree = geo->m_parent && geo->m_parent->m_parent ? (const BSTreeNode*)geo->m_parent->m_parent : nullptr;
		const NiSourceTexture* leaves = tree && tree->TreeModel ? tree->TreeModel->LeavesTexture : nullptr;
		IDirect3DBaseTexture9* texture = leaves && leaves->rendererData ? leaves->rendererData->dTexture : nullptr;
		if (!leafBase || !texture) { leafFate[2]++; skip[kLeaf]++; return; }
		const int slot = AlphaSlotOf(texture);
		if (slot == -2) { waiting++; leafFate[4]++; return; }
		if (slot < 0) { leafFate[3]++; skip[kLeaf]++; return; }
		static std::vector<NVR_EXT_RT_RANGE> ranges;
		NVR_EXT_RT_GEOMETRY_DESC desc;
		GeometryKey key;
		if (!Describe(geo, desc, ranges, key)) { leafFate[5]++; skip[kLeaf]++; return; }
		key.vertCount |= 0x40000000u | (leafPlanes << 28) | (leafOpaque ? 0x08000000u : 0u);	// the cards, not the mesh (and a new BLAS when the plane count changes)

		GeometryEntry& entry = geometries[key];
		if (entry.refused) { skip[kLeaf]++; return; }
		entry.lastSeen = sceneFrame;
		if (!entry.sent) {
			LeafCards& lc = leafCards[key];
			if (lc.failed) { skip[kLeaf]++; return; }
			if (!lc.vb && !BuildLeafCards(geo, leafBase, leafPlanes, lc)) { lc.failed = true; skip[kLeaf]++; return; }
			if (!entry.id) entry.id = nextId++ & 0xFFFFFF;
			static const NVR_EXT_RT_RANGE all = {};
			NVR_EXT_RT_RANGE range = all;
			range.Topology = NVR_EXT_RT_TRIANGLE_LIST;
			range.PrimitiveCount = lc.triangles;
			NVR_EXT_RT_GEOMETRY_DESC cards = {};
			cards.pVertexBuffer = lc.vb;
			cards.VertexStride = 20;
			cards.VertexCount = lc.vertices;
			cards.PositionFormat = NVR_EXT_RT_POSITION_FLOAT3;
			cards.pIndexBuffer = lc.ib;
			cards.RangeCount = 1;
			cards.pRanges = &range;
			cards.Flags = NVR_EXT_RT_TEXCOORD_FLAGS(12, false);
			cards.pDebugName = "NVR RT leaf cards";
			NVR_EXT_RT_GEOMETRY_INFO info = {};
			if (FAILED(rt->MeasureGeometry(&cards, &info))) { entry.refused = true; skip[kMeasure]++; return; }
			if (record->opCount >= NVR_RT_MAX_OPS || arenaUsed + info.Texels + 1 > arenaCapacity) { waiting++; leafFate[4]++; return; }
			if (buildBudget && distance > buildNear && trianglesThisFrame && trianglesThisFrame + info.TriangleCount > buildBudget) {
				waiting++; budgetWaits++; leafFate[4]++; return;
			}
			if (FAILED(rt->StageGeometry(&cards, images[0], arenaUsed, &info))) { entry.refused = true; return; }
			trianglesThisFrame += info.TriangleCount;
			NvrRtGeometryOp& op = record->ops[record->opCount++];
			op = NvrRtGeometryOp();
			op.op = NVR_RT_OP_BUILD;
			op.id = entry.id;
			op.vertexCount = info.VertexCount;
			op.triangleCount = info.TriangleCount;
			op.arenaTexel = arenaUsed;
			op.flags = NVR_RT_OP_FLAG_TEXCOORDS | (leafOpaque ? 0u : NVR_RT_OP_FLAG_ALPHA) | (compactOn ? NVR_RT_OP_FLAG_COMPACT : 0u);
			arenaUsed += info.Texels;
			entry.sent = true;
			lc.made = sceneFrame;
		}
		if (record->instanceCount >= NVR_RT_MAX_INSTANCES) return;
		skip[kTaken]++;
		leafFate[7]++;
		NiAlphaProperty* alpha = (NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha);
		const UInt32 alphaRef = alpha && (alpha->flags & NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK) ? alpha->alphaTestRef : 128u;
		const Albedo& albedo = AlbedoOfTexture(texture);
		NvrRtInstanceRecord& extra = record->records[record->instanceCount];
		extra.albedo[0] = albedo.rgb[0]; extra.albedo[1] = albedo.rgb[1]; extra.albedo[2] = albedo.rgb[2]; extra.albedo[3] = 1.0f;
		extra.flags = (albedo.known ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | (leafOpaque ? NVR_RT_RECORD_TEXTURED : NVR_RT_RECORD_ALPHA_TESTED | (alphaRef << 8)) | ((UInt32)slot << 16);
		extra.geometryId = entry.id;
		extra.vertexOffset = extra.indexOffset = 0;
		NvrRtInstance& instance = record->instances[record->instanceCount++];
		D3DMATRIX m;
		TheRenderManager->CreateD3DMatrix(&m, &geo->m_worldTransform);	// camera relative, row vectors (p' = p M)
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) instance.transform[r][c] = m.m[c][r];
			instance.transform[r][3] = m.m[3][r];
		}
		instance.geometryId = entry.id;
		instance.customIndex = 0;
		instance.mask = leafOpaque ? 0x02u : NVR_RT_MASK_ALPHA;	// only rays that test alpha see it (a ray mask without 0x10 skips the leaves)
		static int placed = 0;
		if (placed < 4) {	// where the first trees' cards go (camera relative) against the tree's world bound
			placed++;
			const NiBound* b = geo->m_kWorldBound;
			Logger::Log("RT CORE leaves: instance %u: translation %.0f %.0f %.0f, scale row %.2f %.2f %.2f; world bound centre (camera relative) %.0f %.0f %.0f r %.0f; "
				"alpha ref %u, slot %d, %s", entry.id, instance.transform[0][3], instance.transform[1][3], instance.transform[2][3], instance.transform[0][0],
				instance.transform[0][1], instance.transform[0][2], b ? b->Center.x - TheRenderManager->CameraPosition.x : 0.0f,
				b ? b->Center.y - TheRenderManager->CameraPosition.y : 0.0f, b ? b->Center.z - TheRenderManager->CameraPosition.z : 0.0f, b ? b->Radius : 0.0f,
				alphaRef, slot, leafOpaque ? "opaque (LeafCardsOpaque)" : "alpha-tested");
		}
		instance.flags = 0x1u;	// both faces
	}

	void Core::AddGeometry(NiGeometry* geo, bool land, bool lod) {
		UInt32* skip = skips[lod ? 2 : land ? 1 : 0];
		skip[kSeen]++;
		// (2026-11-06) The decisions are RtScenePick::Classify's (RtScenePick.h: no game types, so tools/nvr-rt-host tests every rule); this
		// reads their inputs off the geometry and keeps the counters and log lines per fate.
		float distance = 0.0f;	// the bound's centre from the camera (objects only: land's bounds are not usable)
		RtScenePick::Geometry pick;
		pick.land = land;
		pick.farLod = walkingFar;
		if (!land && geo->m_kWorldBound) {
			const NiBound* b = geo->m_kWorldBound;
			const float dx = b->Center.x - TheRenderManager->CameraPosition.x, dy = b->Center.y - TheRenderManager->CameraPosition.y,
				dz = b->Center.z - TheRenderManager->CameraPosition.z;
			distance = sqrtf(dx * dx + dy * dy + dz * dz);
			pick.hasBound = true; pick.distance = distance; pick.radius = b->Radius;
		}
		pick.hasShaderAndData = geo->shader && geo->geomData && geo->geomData->m_pkBuffData;
		pick.skinned = geo->skinInstance != nullptr;
		BSShaderProperty* shaderProperty = pick.hasShaderAndData && !pick.skinned ? (BSShaderProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Shade) : nullptr;
		const int shaderType = shaderProperty ? (int)static_cast<NiShadeProperty*>((NiProperty*)shaderProperty)->m_eShaderType : -1;
		pick.hasShaderProperty = shaderProperty != nullptr;
		pick.speedTreeLeaf = shaderType == NiShadeProperty::kProp_SpeedTreeLeaf;
		pick.lightingProperty = shaderProperty && shaderProperty->IsLightingProperty();
		pick.decalOrRefraction = shaderProperty && (shaderProperty->GetFlag(BSSP_REFRACTION) || shaderProperty->GetFlag(BSSP_FIRE_REFRACTION) ||
			shaderProperty->GetFlag(BSSP_DECAL) || shaderProperty->GetFlag(BSSP_DYNAMIC_DECAL));
		// Alpha-tested casters (AlphaTested on): traced with their diffuse alpha (RtAlpha.glsl); blended-only (glass, fx) stays out.
		// (FarObjectLOD, 2026-10-31 / 11-03: far object LOD's alpha property is its distance fade, it goes in opaque; far tree / billboard LOD
		// is cut out (TEST_ENABLE) and takes the alpha-tested path: RtScenePick::Classify)
		NiAlphaProperty* alpha = pick.hasShaderAndData && !pick.skinned ? (NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha) : nullptr;
		pick.hasAlphaProperty = alpha != nullptr;
		pick.alphaFlags = alpha ? (uint16_t)alpha->flags : 0;
		RtScenePick::Switches pickSwitches;
		pickSwitches.cullAngle = cullAngle; pickSwitches.alphaTested = alphaOn; pickSwitches.leafCards = leafPlanes != 0;
		bool alphaTested = false;
		switch (RtScenePick::Classify(pick, pickSwitches, alphaTested)) {
		case RtScenePick::Taken: break;
		case RtScenePick::Culled: culledSmall++; LogLeftOut(geo, "small at its distance (CullAngle)", -1); return;
		case RtScenePick::NoShader: skip[kNoShader]++; LogLeftOut(geo, "no shader / buffer data", -1); return;
		case RtScenePick::Skinned: skip[kSkinned]++; if (!land) LogLeftOut(geo, "skinned (not an actor)", -1); return;	// (2026-11-06: was silent)
		case RtScenePick::Leaves: AddLeaves(geo, shaderProperty, distance); return;	// (2026-10-30) LeafCards
		case RtScenePick::NotLighting: skip[kNotLighting]++; LogLeftOut(geo, shaderProperty ? "not a lighting shader" : "no shader property", shaderType); return;
		case RtScenePick::DecalOrRefraction: skip[kFlags]++; LogLeftOut(geo, "decal / refraction", -1); return;
		case RtScenePick::SpeedTreeLeaf: skip[kLeaf]++; return;
		case RtScenePick::BlendedOnly: skip[kAlpha]++; alphaFate[0]++; LogLeftOut(geo, "blended only (no alpha test)", -1); return;
		case RtScenePick::AlphaOff: skip[kAlpha]++; alphaFate[1]++; return;
		}

		NVR_EXT_RT_GEOMETRY_DESC desc;
		static std::vector<NVR_EXT_RT_RANGE> ranges;
		GeometryKey key;
		if (!Describe(geo, desc, ranges, key)) {
			if (alphaTested) { skip[kAlpha]++; alphaFate[2]++; LogLeftOut(geo, "alpha-tested, not described", -1); return; }
			skip[kDescribe]++;
			static int told = 0;
			NiGeometryBufferData* bd = geo->geomData->m_pkBuffData;
			if (land && told < 2) {
				told++;
				Logger::Log("RT CORE: land geometry '%s' not described: VertCount %u, IB %p, streams %u, FVF %X, declaration %p, primitive %d, arrays %u, tris %u",
					geo->m_pcName ? geo->m_pcName : "", bd->VertCount, bd->IB, bd->StreamCount, bd->FVF, bd->VertexDeclaration, (int)bd->PrimitiveType, bd->NumArrays, bd->TriCount);
			}
			if (!land) LogLeftOut(geo, "not described (vertex layout / primitive)", -1);	// (2026-11-06: objects were silent)
			return;
		}

		int alphaSlot = -1;
		if (alphaTested) {
			// the texcoords must sit in the position's stream; the diffuse alpha must be readable
			// (2026-11-06 alpha lamp shadows: these four fates were silent, so a fence missing from the scene left no trace in the log)
			if (lastElement.uvStream != lastElement.stream || !shaderProperty->IsLightingProperty()) {
				skip[kAlpha]++; alphaFate[3]++; LogLeftOut(geo, lastElement.uvStream != lastElement.stream ? "alpha-tested, texcoords not in the position's stream" : "alpha-tested, not a lighting property", -1); return; }
			NiSourceTexture** textures = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[0];
			NiTexture* texture = textures ? *textures : nullptr;
			if (!texture || !texture->rendererData || !texture->rendererData->dTexture) { skip[kAlpha]++; alphaFate[4]++; LogLeftOut(geo, "alpha-tested, no diffuse texture", -1); return; }
			alphaSlot = AlphaSlotOf(texture->rendererData->dTexture, GlowTextureOf(shaderProperty), NormalTextureOf(shaderProperty));
			if (alphaSlot == -2) { waiting++; alphaFate[6]++; return; }	// this frame's uploads are used up: next frame
			if (alphaSlot < 0) { skip[kAlpha]++; alphaFate[5]++; LogLeftOut(geo, "alpha-tested, alpha unreadable or atlas full", -1); return; }
			desc.Flags = NVR_EXT_RT_TEXCOORD_FLAGS((int)lastElement.uvOffset - (int)lastElement.offset, lastElement.uvHalf);
		}
		// Hit textures ([Main.Main.RayTracing] HitTextures): opaque geometry with a diffuse texture keeps its texcoords and gets an atlas
		// slot, so a ray's hit can read the texture's colour (RtHit.glsl). Without a slot this frame (upload budget) it waits a frame.
		bool textured = false;
		if (!alphaTested && hitTextures && lastElement.uvStream == lastElement.stream && shaderProperty->IsLightingProperty()) {
			NiSourceTexture** textures = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[0];
			NiTexture* texture = textures ? *textures : nullptr;
			if (texture && texture->rendererData && texture->rendererData->dTexture) {
				alphaSlot = AlphaSlotOf(texture->rendererData->dTexture, GlowTextureOf(shaderProperty), NormalTextureOf(shaderProperty));
				if (alphaSlot == -2) { waiting++; return; }
				if (alphaSlot >= 0) {
					textured = true;
					desc.Flags = NVR_EXT_RT_TEXCOORD_FLAGS((int)lastElement.uvOffset - (int)lastElement.offset, lastElement.uvHalf);
				}
			}
		}
		GeometryEntry& entry = geometries[key];
		if (entry.refused) { LogLeftOut(geo, "refused by the fork (measure / stage failed earlier)", -1); return; }	// (2026-11-06: was silent)
		entry.lastSeen = sceneFrame;
		if (!entry.sent) {
			if (!entry.id) entry.id = nextId++ & 0xFFFFFF;
			NVR_EXT_RT_GEOMETRY_INFO info = {};
			if (FAILED(rt->MeasureGeometry(&desc, &info))) { entry.refused = true; skip[kMeasure]++; LogLeftOut(geo, "refused by the fork (MeasureGeometry failed)", -1); return; }
			if ((alphaTested || textured) && info.Texels != 3u * (info.VertexCount + info.TriangleCount) + 2u * info.VertexCount) {
				hitTextures = false;
				alphaOn = false;	// a fork without 1.8-rt-uv (patch 0029): no texcoords staged, so no alpha-tested casters this session
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT alpha-tested casters off: this d3d9.dll does not stage texcoords (fork patch 0029, 1.8-rt-uv).");
				return;
			}
			if (record->opCount >= NVR_RT_MAX_OPS || arenaUsed + info.Texels + 1 > arenaCapacity) { waiting++; return; }	// next frame (+1: the sequence texel)
			if (buildBudget && !land && distance > buildNear && trianglesThisFrame && trianglesThisFrame + info.TriangleCount > buildBudget) {
				waiting++; budgetWaits++; return;	// far: a later frame (BuildTrianglesPerFrame)
			}
			if (FAILED(rt->StageGeometry(&desc, images[0], arenaUsed, &info))) { entry.refused = true; skip[kMeasure]++; LogLeftOut(geo, "refused by the fork (StageGeometry failed)", -1); return; }
			trianglesThisFrame += info.TriangleCount;
			NvrRtGeometryOp& op = record->ops[record->opCount++];
			op = NvrRtGeometryOp();
			op.op = NVR_RT_OP_BUILD;
			op.id = entry.id;
			op.vertexCount = info.VertexCount;
			op.triangleCount = info.TriangleCount;
			op.arenaTexel = arenaUsed;
			RtScenePick::Record po; po.alphaTested = alphaTested; po.textured = textured; po.compact = compactOn;
			op.flags = RtScenePick::OpFlags(po);	// (2026-11-06 RtScenePick.h)
			arenaUsed += info.Texels;
			entry.sent = true;
			static int probes = 0;
			if (probes < 3) {	// compared with the helper's readback of the same ids (NvrRtHelper.log "probe")
				probes++;
				NiGeometryData* gd = geo->geomData;
				NiGeometryBufferData* bd = gd->m_pkBuffData;
				const NiVBChip* chip = bd->VBChip[0];
				const NiPoint3* v = gd->m_pkVertex;
				D3DMATRIX pm;
				TheRenderManager->CreateD3DMatrix(&pm, &geo->m_worldTransform);
				Logger::Log("RT CORE probe: geometry %u '%s' land %d: %u vertices (VertCount %u, BaseVertexIndex %u, chip offset %u, stride %u, "
					"position offset %u, format %d, FVF %X), %u triangles (%s, %u ranges), model bound %.1f %.1f %.1f r %.1f, CPU vertex 0 %s%.1f %.1f %.1f, "
					"translation (camera relative) %.1f %.1f %.1f, arena texel %u", entry.id, geo->m_pcName ? geo->m_pcName : "", land ? 1 : 0, info.VertexCount,
					bd->VertCount, bd->BaseVertexIndex, chip ? chip->Offset : 0u, desc.VertexStride, desc.PositionOffset, (int)desc.PositionFormat, bd->FVF,
					info.TriangleCount, bd->PrimitiveType == D3DPT_TRIANGLESTRIP ? "strips" : "list", desc.RangeCount, gd->m_kBound.Center.x,
					gd->m_kBound.Center.y, gd->m_kBound.Center.z, gd->m_kBound.Radius, v ? "" : "(none) ", v ? v[0].x : 0.0f, v ? v[0].y : 0.0f,
					v ? v[0].z : 0.0f, pm.m[3][0], pm.m[3][1], pm.m[3][2], arenaUsed - info.Texels);
			}
		}
		if (record->instanceCount >= NVR_RT_MAX_INSTANCES) return;
		skip[kTaken]++;
		if (alphaTested) alphaFate[7]++;
		NvrRtInstanceRecord& extra = record->records[record->instanceCount];
		const Albedo& albedo = AlbedoOf(geo, shaderProperty);
		extra.albedo[0] = albedo.rgb[0]; extra.albedo[1] = albedo.rgb[1]; extra.albedo[2] = albedo.rgb[2]; extra.albedo[3] = 1.0f;
		// (2026-11-06) packed by RtScenePick::RecordFlags / InstanceMask / OpFlags (RtScenePick.h; tested bit for bit by tools/nvr-rt-host)
		RtScenePick::Record pr;
		pr.land = land; pr.albedoKnown = albedo.known; pr.glowMap = HasGlowMap(shaderProperty);
		pr.alphaTested = alphaTested; pr.alphaRef = alphaTested ? (UInt32)alpha->alphaTestRef : 0u;
		pr.textured = textured; pr.slot = alphaSlot;
		pr.normalMap = (alphaTested || textured) && normalAtlasImage >= 0 && NormalTextureOf(shaderProperty) != nullptr;	// UNOFFICIAL 2026-11-02
		if (alphaTested || textured) pr.materialRule = RtMaterialRuleOf(geo, &pr.rough, &pr.metal);	// UNOFFICIAL 2026-11-03 (bits 26-31)
		pr.hookMask = MaskHook ? (int)(MaskHook(geo, land) & 0xFFu) : -1;
		pr.player = walkingPlayer; pr.farLod = walkingFar;
		extra.flags = RtScenePick::RecordFlags(pr);
		LogAlphaTaken(geo, alphaTested ? alphaSlot : -1, alphaTested ? alpha->alphaTestRef : 999u, alphaTested && (alpha->flags & NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK) != 0);
		LogBigTaken(geo);
		if ((extra.flags & NVR_RT_RECORD_GLOW) && geo->m_kWorldBound && glows.size() < 4096) {	// (2026-10-30) GlowLights()
			const NiBound* b = geo->m_kWorldBound;
			glows.push_back({ { b->Center.x - TheRenderManager->CameraPosition.x, b->Center.y - TheRenderManager->CameraPosition.y,
				b->Center.z - TheRenderManager->CameraPosition.z }, b->Radius, { albedo.rgb[0], albedo.rgb[1], albedo.rgb[2] } });
		}
		extra.geometryId = entry.id;
		extra.vertexOffset = extra.indexOffset = 0;	// the helper's
		NvrRtInstance& instance = record->instances[record->instanceCount++];
		D3DMATRIX m;
		TheRenderManager->CreateD3DMatrix(&m, &geo->m_worldTransform);	// camera relative, row vectors (p' = p M)
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) instance.transform[r][c] = m.m[c][r];
			instance.transform[r][3] = m.m[3][r];
		}
		// (2026-10-30) a static plant bent by NVR's plant wind ([Shaders.Plants.Wind]): its drawn top leans with the gust, so the traced
		// instance gets the same lean as a shear rooted at its base (PlantWind::RtLean); the BLAS is untouched (no rebuild, no refit)
		float lean[2], base, H;
		if (plantSway && !land && PlantWind::RtLean(geo, lean, base, H)) {
			const float baseRel = base - TheRenderManager->CameraPosition.z;	// camera-relative, as the transform
			for (int r = 0; r < 2; r++) {
				const float k = lean[r] / H;
				for (int c = 0; c < 4; c++) instance.transform[r][c] += k * instance.transform[2][c];
				instance.transform[r][3] -= k * baseRel;
			}
			swayed++;
		}
		instance.geometryId = entry.id;
		instance.customIndex = land ? 1u : 0u;
		instance.mask = RtScenePick::InstanceMask(pr);	// (2026-11-06 RtScenePick.h) 0x01 land / 0x02 objects or the hook's; 0x10 alpha-tested, 0x40 the player's, 0x80 far LOD
		instance.flags = 0x1u;	// both faces (the game's two-sided and one-sided meshes alike: shadows need the back faces too)
	}

	// UNOFFICIAL 2026-10-30 grass (RtCore::NoteGrass): each grass model's first tuft built once (alpha-tested: its card texture's alpha in
	// the atlas, the render state's alpha reference), every noted tuft an instance of it: scale = 1 + size step x 0.01 x ScaleMask (the
	// grass VS's), placed at the tuft's world position (camera relative). Mask NVR_RT_MASK_ALPHA: only rays that test alpha see grass.
	void Core::AddGrass() {
		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);
		const UInt32 now = TheFrameRateManager->FrameNumber + 1;
		if (grassFrame != now) {
			if (grassFrame && !grassDraws.empty() && sceneFrame <= 60) grassFate[11] += (UInt32)grassDraws.size();
			grassDraws.clear(); grassTufts.clear(); grassFrame = 0;
			return;
		}
		const float camera[3] = { TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z };
		UInt32 fate[12] = {};
		for (const GrassDraw& d : grassDraws) {
			if (record->instanceCount >= NVR_RT_MAX_INSTANCES) break;
			NiGeometryData* gd = d.geometry->geomData;
			NiGeometryBufferData* bd = gd ? gd->m_pkBuffData : nullptr;
			NVR_EXT_RT_GEOMETRY_DESC desc;
			static std::vector<NVR_EXT_RT_RANGE> ranges;
			GeometryKey key;
			if (!bd || !DescribeData(bd, "NVR RT grass tuft", desc, ranges, key) || lastElement.uvStream != lastElement.stream) { fate[6]++; continue; }
			// One tuft: the buffer holds the model once per instance slot, its vertices and indices in that order. With the CPU copy of the
			// indices (IndexArray): the leading indices that address the first tuft's vertices only; else an even split by the slots.
			const UInt32 slots = d.vertsPerTuft ? bd->VertCount / d.vertsPerTuft : 0;
			const bool list = ranges[0].Topology == NVR_EXT_RT_TRIANGLE_LIST;
			UInt32 lead = 0;
			if (bd->IndexArray && bd->IndexCount && d.vertsPerTuft) while (lead < bd->IndexCount && bd->IndexArray[lead] < d.vertsPerTuft) lead++;
			bool understood = false;
			if (lead >= 3 && lead < bd->IndexCount) {
				if (list) { ranges.resize(1); ranges[0].PrimitiveCount = lead / 3; understood = true; }
				else if (bd->NumArrays <= 1 || !bd->ArrayLengths) { ranges.resize(1); ranges[0].StartIndex = 0; ranges[0].PrimitiveCount = lead - 2; understood = true; }
				else {	// several strips: the whole ones within the lead
					std::vector<NVR_EXT_RT_RANGE> first;
					for (const NVR_EXT_RT_RANGE& r : ranges) if (r.StartIndex + r.PrimitiveCount + 2 <= lead) first.push_back(r);
					if (!first.empty()) { ranges = first; understood = true; }
				}
			}
			else if (slots > 1 && slots * d.vertsPerTuft == bd->VertCount) {
				if (list && ranges.size() == 1 && bd->TriCount % slots == 0) { ranges[0].PrimitiveCount = bd->TriCount / slots; understood = true; }
				else if (!list && ranges.size() >= slots && ranges.size() % slots == 0) { ranges.resize(ranges.size() / slots); understood = true; }
			}
			if (!understood) {
				fate[7]++;
				static int told = 0;
				if (told < 4) {
					told++;
					Logger::Log("RT CORE grass: tuft layout not understood: %u vertices (max %u, model %u, %u a tuft), %s, %u triangles, %u arrays, %u indices "
						"(CPU copy %s, %u leading ones on the first tuft)", bd->VertCount, bd->MaxVertCount, (unsigned)gd->m_usVertices, d.vertsPerTuft,
						list ? "list" : "strips", bd->TriCount, bd->NumArrays, bd->IndexCount, bd->IndexArray ? "yes" : "no", lead);
				}
				continue;
			}
			desc.RangeCount = (UINT)ranges.size();
			desc.pRanges = ranges.data();
			UInt32 tris = 0;
			for (const NVR_EXT_RT_RANGE& r : ranges) tris += r.PrimitiveCount;
			key.vertCount = d.vertsPerTuft;
			key.triCount = tris | 0x80000000u;	// never the key of a whole-buffer object
			const int slot = AlphaSlotOf(d.texture);
			if (slot == -2) { fate[9]++; waiting++; continue; }
			if (slot < 0) { fate[8]++; continue; }
			desc.Flags = NVR_EXT_RT_TEXCOORD_FLAGS((int)lastElement.uvOffset - (int)lastElement.offset, lastElement.uvHalf);
			GeometryEntry& entry = geometries[key];
			if (entry.refused) { fate[6]++; continue; }
			entry.lastSeen = sceneFrame;
			if (!entry.sent) {
				if (!entry.id) entry.id = nextId++ & 0xFFFFFF;
				NVR_EXT_RT_GEOMETRY_INFO info = {};
				if (FAILED(rt->MeasureGeometry(&desc, &info))) { entry.refused = true; fate[6]++; continue; }
				if (record->opCount >= NVR_RT_MAX_OPS || arenaUsed + info.Texels + 1 > arenaCapacity) { fate[9]++; waiting++; continue; }
				if (FAILED(rt->StageGeometry(&desc, images[0], arenaUsed, &info))) { entry.refused = true; fate[6]++; continue; }
				trianglesThisFrame += info.TriangleCount;
				NvrRtGeometryOp& op = record->ops[record->opCount++];
				op = NvrRtGeometryOp();
				op.op = NVR_RT_OP_BUILD;
				op.id = entry.id;
				op.vertexCount = info.VertexCount;
				op.triangleCount = info.TriangleCount;
				op.arenaTexel = arenaUsed;
				op.flags = NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA | (compactOn ? NVR_RT_OP_FLAG_COMPACT : 0u);
				arenaUsed += info.Texels;
				entry.sent = true;
				static int told = 0;
				if (told < 3) {
					told++;
					Logger::Log("RT CORE grass: model %u: %u vertices a tuft (%u slots), %u triangles a tuft (%u ranges), alpha ref %u, scale mask %.2f %.2f %.2f, "
						"first tuft at %.0f %.0f %.0f (camera %.0f %.0f %.0f)", entry.id, d.vertsPerTuft, slots, tris, (unsigned)ranges.size(), d.alphaRef,
						d.scale[0], d.scale[1], d.scale[2], d.firstTuft[0], d.firstTuft[1], d.firstTuft[2], camera[0], camera[1], camera[2]);
				}
			}
			auto found = albedoCache.find(d.texture);
			if (found == albedoCache.end()) found = albedoCache.emplace(d.texture, ReadAlbedo(d.texture)).first;
			const Albedo& albedo = found->second;
			for (UInt32 i = 0; i < d.count && record->instanceCount < NVR_RT_MAX_INSTANCES; i++) {
				const float* t = &grassTufts[4 * (d.first + i)];
				NvrRtInstanceRecord& extra = record->records[record->instanceCount];
				extra.albedo[0] = albedo.rgb[0]; extra.albedo[1] = albedo.rgb[1]; extra.albedo[2] = albedo.rgb[2]; extra.albedo[3] = 1.0f;
				extra.flags = (albedo.known ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | NVR_RT_RECORD_ALPHA_TESTED | ((d.alphaRef & 0xFFu) << 8) | ((UInt32)slot << 16);
				extra.geometryId = entry.id;
				extra.vertexOffset = extra.indexOffset = 0;
				NvrRtInstance& instance = record->instances[record->instanceCount++];
				const float step = 0.01f * t[3];
				memset(instance.transform, 0, sizeof(instance.transform));
				for (int r = 0; r < 3; r++) {
					instance.transform[r][r] = 1.0f + step * d.scale[r];
					instance.transform[r][3] = t[r] - camera[r];
				}
				instance.geometryId = entry.id;
				instance.customIndex = 0;
				instance.mask = NVR_RT_MASK_ALPHA;
				instance.flags = 0x1u;	// both faces
				fate[10]++;
			}
		}
		grassDraws.clear();
		grassTufts.clear();
		grassFrame = 0;
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		if (sceneFrame <= 60) {
			for (int i = 6; i < 11; i++) grassFate[i] += fate[i];
			grassGatherMs += 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
		}
	}

	void Core::Walk(NiAVObject* root, bool land, bool lod) {
		if (!root) return;
		const NiPoint3 camera = { TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z };
		auto inReach = [&](NiAVObject* o) {
			const NiBound* b = o->m_kWorldBound;
			if (!b) return true;
			const float dx = b->Center.x - camera.x, dy = b->Center.y - camera.y, dz = b->Center.z - camera.z;
			if (walkingFar) {	// UNOFFICIAL 2026-10-31 FarObjectLOD: the ring past SceneRadius (inside it the full objects stand) out to FarRadius
				const float d = sqrtf(dx * dx + dy * dy + dz * dz);
				return d + b->Radius >= radius && d - b->Radius <= farRadius;
			}
			const float reach = radius + b->Radius;
			return dx * dx + dy * dy + dz * dz <= reach * reach;
		};
		stack.clear();
		stack.push_back(root);
		while (!stack.empty()) {
			NiAVObject* object = stack.back();
			stack.pop_back();
			// Land: no reach test (its nodes' world bounds are not usable: rtsun-6 met no land at all with it; the cascades' walk does not
			// test land either). The loaded grid limits it (a few quads a cell).
			if (land && !lod && object) { landWalk[0]++; if (object->m_flags & NiAVObject::NiFlags::APP_CULLED) landWalk[1]++; }
			// LOD land keeps the reach test (big quads with real bounds); its quads where the full land is loaded are APP_CULLED
			if (walkingFar && object) {	// UNOFFICIAL 2026-10-31: the far walk's fates (why FarObjectLOD finds what it finds)
				farWalk[0]++;
				if (object->m_flags & NiAVObject::NiFlags::APP_CULLED) farWalk[1]++;
				else if (!inReach(object)) farWalk[2]++;
				else if (object->IsGeometry()) farWalk[3]++;
				else if (NiNode* n = object->IsNiNode()) {
					farWalk[5]++;
					if (n->IsFadeNode() && static_cast<BSFadeNode*>(n)->FadeAlpha <= 0.0f) farWalk[4]++;
					if (n->IsKindOf<NiSwitchNode>()) farWalk[6]++;
				}
			}
			// UNOFFICIAL 2026-11-05 FirstPersonBody: in first person the game culls the root of the player's third-person body (its children
			// are not flagged): that one node is walked anyway, so the body shows in reflections, casts its sun shadow and darkens its feet
			// ("RT CORE: player: first person; body node CULLED; 0.0 body instances a frame" before, rtfull-lake / -int)
			const bool playerRoot = walkingPlayer && firstPersonBody && object == root;
			// (2026-11-06, the demo room's missing ceiling) the walk's own silent skips, named: culled objects (with LogLeftOut's per-cell limit)
			if (object && !land && !walkingFar && (object->m_flags & NiAVObject::NiFlags::APP_CULLED) && !playerRoot)
				LogLeftOut(static_cast<NiGeometry*>(object), object->IsGeometry() ? "APP_CULLED (the walk)" : "APP_CULLED node (the walk: its whole subtree)", -1);
			if (!object || ((object->m_flags & NiAVObject::NiFlags::APP_CULLED) && !playerRoot) || ((!land || lod) && !inReach(object))) continue;
			// UNOFFICIAL 2026-11-03 FarLandLOD: a far LOD land quad whose area reaches into the loaded cells stays out: the ring test above
			// lets in coarse quads (4-cell blocks and up, bound radius ~11600) that cover the loaded land, and where they stand above it they
			// block every sun / sky / bounce ray from the near ground (the reference path tracer's black ground, rtpt-2 / rtpt-4). The loaded
			// grid: 5 x 5 cells of 4096 round the camera's cell (uGridsToLoad 5), taken as a square of half-side 2.5 cells round the camera.
			if (walkingFar && land && object->IsGeometry()) {
				const NiBound* b = object->m_kWorldBound;
				const float half = 2.5f * 4096.0f;
				if (b && fabsf(b->Center.x - camera.x) < half + b->Radius && fabsf(b->Center.y - camera.y) < half + b->Radius) { farLandOverGrid++; continue; }
			}
			if (object->IsGeometry()) {
				NiGeometry* g = static_cast<NiGeometry*>(object);
				if (g->skinInstance && actorsOn && rtSkin) AddSkinned(g);
				else AddGeometry(g, land, lod);
				continue;
			}
			NiNode* node = object->IsNiNode();
			if (!node) continue;
			if (land && !lod) landWalk[2]++;
			// Faded out: not drawn. Land ignores it, as the cascades' walk does (rt-reflections' og-lake run met no land with it).
			// A tree's root fade node reads 0 on most trees the game draws (rtleaf-4: 630 of 668 SpeedTree leaf meshes under one); the cascades'
			// walk never tests a ref's root, so a tree's root is not tested here either (LeafCards)
			if (node->IsFadeNode() && static_cast<BSFadeNode*>(node)->FadeAlpha <= 0.0f && !(walkTree && object == root)) { if (land) { if (!lod) landWalk[3]++; } else { if (!walkingFar) LogLeftOut(static_cast<NiGeometry*>(object), "faded-out fade node (the walk: its whole subtree)", -1); continue; } }
			if (node->IsKindOf<NiSwitchNode>()) {
				if (land && !lod) landWalk[4]++;
				NiSwitchNode* sw = static_cast<NiSwitchNode*>(node);
				if (sw->m_iIndex >= 0 && sw->m_iIndex < node->m_children.end) stack.push_back(node->m_children.data[sw->m_iIndex]);
				continue;
			}
			for (int i = 0; i < node->m_children.end; i++) if (node->m_children.data[i]) stack.push_back(node->m_children.data[i]);
		}
	}

	void Core::ProbeTree(TESObjectREFR* ref) {
		treeFate[0]++;
		if (ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows) treeFate[1]++;
		NiNode* root = ref->GetNode();
		if (!root) { treeFate[2]++; return; }
		const float dx = ref->pos.x - TheRenderManager->CameraPosition.x, dy = ref->pos.y - TheRenderManager->CameraPosition.y;
		if (dx * dx + dy * dy <= radius * radius) treeFate[3]++;
		struct Item { NiAVObject* o; UInt32 why; };	// why: 1 culled, 2 faded, 4 switch child not taken
		static std::vector<Item> items;
		items.clear();
		items.push_back({ root, 0u });
		UInt32 leaves = 0, reachable = 0;
		while (!items.empty()) {
			Item it = items.back();
			items.pop_back();
			if (!it.o) continue;
			UInt32 why = it.why | ((it.o->m_flags & NiAVObject::NiFlags::APP_CULLED) ? 1u : 0u);
			if (it.o->IsGeometry()) {
				NiGeometry* g = static_cast<NiGeometry*>(it.o);
				BSShaderProperty* sp = (BSShaderProperty*)g->GetProperty(NiProperty::PropertyType::kType_Shade);
				if (!sp || static_cast<NiShadeProperty*>((NiProperty*)sp)->m_eShaderType != NiShadeProperty::kProp_SpeedTreeLeaf) continue;
				treeFate[4]++; leaves++;
				if (why & 1u) treeFate[5]++;
				if (why & 2u) treeFate[6]++;
				if (why & 4u) treeFate[7]++;
				if (!g->geomData || !g->geomData->m_pkBuffData) treeFate[8]++;
				else if (!why) { treeFate[9]++; reachable++; }
				continue;
			}
			NiNode* node = it.o->IsNiNode();
			if (!node) continue;
			if (node->IsFadeNode() && static_cast<BSFadeNode*>(node)->FadeAlpha <= 0.0f) why |= 2u;
			const bool sw = node->IsKindOf<NiSwitchNode>();
			for (int i = 0; i < node->m_children.end; i++)
				items.push_back({ node->m_children.data[i], why | (sw && i != static_cast<NiSwitchNode*>(node)->m_iIndex ? 4u : 0u) });
		}
		static int told = 0;
		if (told < 6) {
			told++;
			Logger::Log("RT CORE tree probe: ref %08X at %.0f units: root flags %X%s, %u leaf meshes, %u reachable", ref->refID, sqrtf(dx * dx + dy * dy),
				(unsigned)root->m_flags, root->IsFadeNode() ? " (fade node)" : "", leaves, reachable);
		}
	}

	void Core::WalkCell(TESObjectCELL* cell) {
		if (!cell) return;
		NiNode* landNode = cell->GetChildNode(TESObjectCELL::kCellNode_Land);
		landCells[0]++;
		if (!landNode) landCells[1]++;
		else if (landNode->m_flags & NiAVObject::NiFlags::APP_CULLED) landCells[2]++;
		Walk(landNode, true);
		for (TList<TESObjectREFR>::Entry* entry = &cell->objectList.First; entry; entry = entry->next) {
			TESObjectREFR* ref = entry->item;
			if (treeProbe && ref && ref->baseForm && ref->baseForm->formType == TESForm::FormType::kFormType_Tree) ProbeTree(ref);
			if (!ref || !ref->baseForm || (ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows)) continue;
			const UInt8 type = ref->baseForm->formType;
			if (type == TESForm::FormType::kFormType_Land) continue;
			const bool actor = type == TESForm::FormType::kFormType_NPC || type == TESForm::FormType::kFormType_Creature ||
				type == TESForm::FormType::kFormType_LeveledCreature;
			if (actor && !(actorsOn && rtSkin)) continue;	// actors: [Main.Main.RayTracing] Actors + fork patch 0033
			if (actor) {	// actors only near the camera ([Main.Main.RayTracing] ActorRadius): each is rebuilt every frame
				const float dx = ref->pos.x - TheRenderManager->CameraPosition.x, dy = ref->pos.y - TheRenderManager->CameraPosition.y;
				if (dx * dx + dy * dy > actorRadius * actorRadius) continue;
			}
			if ((TESObjectREFR*)Player == ref) continue;	// walked once, by Gather
			walkTree = leafPlanes && type == TESForm::FormType::kFormType_Tree;
			Walk(ref->GetNode(), false);
			walkTree = false;
		}
	}

	void Core::Gather() {
		sceneFrame++;
		// A cell change (a door, a jump, a load): every actor partition goes at once (its buffers and bones may be unloaded and the
		// addresses reused by new ones); the actors still there are posed again from scratch this frame
		static const void* lastCell = nullptr;
		const void* cell = Player ? (const void*)Player->parentCell : nullptr;
		if (cell != lastCell) {
			for (auto it = geometries.begin(); it != geometries.end(); ) {
				if (!it->second.skinned) { ++it; continue; }
				if (it->second.sent) Release(it->second.id);	// (2026-10-30: queued; a full op list used to drop it and the helper kept it)
				it = geometries.erase(it);
			}
			lastCell = cell;
		}
		// the releases that did not fit an earlier frame's op list go first
		size_t sent = 0;
		while (sent < pendingReleases.size() && record->opCount < NVR_RT_MAX_OPS) {
			NvrRtGeometryOp& op = record->ops[record->opCount++];
			op = NvrRtGeometryOp();
			op.op = NVR_RT_OP_RELEASE;
			op.id = pendingReleases[sent++];
		}
		pendingReleases.erase(pendingReleases.begin(), pendingReleases.begin() + sent);
		compactOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "Compact") != 0;
		buildBudget = (UInt32)(std::max)(0, TheSettingManager->GetSettingI("Main.Main.RayTracing", "BuildTrianglesPerFrame"));
		buildNear = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "BuildNearRadius"));
		cullAngle = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "CullSmallAngle"));
		plantSway = TheSettingManager->GetSettingI("Main.Main.RayTracing", "PlantSway") != 0;
		const int planes = TheSettingManager->GetSettingI("Main.Main.RayTracing", "LeafCards");
		leafPlanes = planes <= 0 ? 0u : planes >= 3 ? 3u : 2u;
		leafOpaque = TheSettingManager->GetSettingI("Main.Main.RayTracing", "LeafCardsOpaque") != 0;
		trianglesThisFrame = 0;
		radius = (std::max)(512.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "SceneRadius"));
		grassRadius = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "GrassRadius"));
		grassMax = (UInt32)(std::max)(0, TheSettingManager->GetSettingI("Main.Main.RayTracing", "GrassMaxTufts"));
		actorRadius = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "ActorRadius"));
		actorInterval = (UInt32)(std::max)(1, (std::min)(8, TheSettingManager->GetSettingI("Main.Main.RayTracing", "ActorUpdateInterval")));
		farLodOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FarObjectLOD") != 0;	// UNOFFICIAL 2026-10-31
		farLandOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FarLandLOD") != 0;
		farRadius = (std::max)(radius, TheSettingManager->GetSettingF("Main.Main.RayTracing", "FarRadius"));
		actorStillSkip = TheSettingManager->GetSettingI("Main.Main.RayTracing", "ActorStillSkip") != 0;
		actorStillThreshold = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "ActorStillThreshold"));
		firstPersonBody = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FirstPersonBody") != 0;
		actorRebuild = (UInt32)(std::max)(0, TheSettingManager->GetSettingI("Main.Main.RayTracing", "ActorRebuildInterval"));
		arenaUsed = 0;
		waiting = 0;
		atlasThisFrame = 0;
		if (Player && Player->GetWorldSpace() && Tes && Tes->gridCellArray) {
			GridCellArray* cells = Tes->gridCellArray;
			const UInt32 count = cells->size * cells->size;
			const UInt32 landBefore = skips[1][kTaken];
			for (UInt32 i = 0; i < count; i++) WalkCell(cells->GetCell(i));
			// LOD land only when the loaded cells gave no land geometry this frame (rtsun-8 at og-lake: every land node empty, the ground on
			// screen is LOD terrain). With the full land there, LOD land stays out: its coarse quads stand above the real ground and cast
			// false shadows on it (rtsun-9 at og-goodsprings: black slabs on the road and the house front).
			const bool lodLandFallback = skips[1][kTaken] == landBefore;
			if (lodLandFallback) Walk(BGSTerrainManager::GetRootLandLODNode(), true, true);
			// UNOFFICIAL 2026-10-31 FarLandLOD (rt-reflections round 15): the noon skyline is mountains, i.e. LOD land, which stays out while
			// the full land is loaded (its coarse quads stand above the real ground). Past SceneRadius there is no real land to stand above:
			// LOD land in the ring SceneRadius .. FarRadius only (mask NVR_RT_MASK_FAR), so reflections meet the mountains.
			else if (farLandOn) {
				walkingFar = true;
				const UInt32 before = record->instanceCount;
				Walk(BGSTerrainManager::GetRootLandLODNode(), true, true);
				farLandInstances += record->instanceCount - before;
				walkingFar = false;
			}
			// UNOFFICIAL 2026-10-31 FarObjectLOD (rt-reflections, FORK-RT-GAPS.md 2): the game's distant object LOD (buildings, rocks, the
			// skyline past the loaded grid), in the ring SceneRadius .. FarRadius only, mask NVR_RT_MASK_FAR: reflections reach the skyline.
			if (farLodOn) {
				walkingFar = true;
				const UInt32 before = record->instanceCount;
				if (!BGSTerrainManager::GetRootObjectLODNode()) farWalk[7]++;
				Walk(BGSTerrainManager::GetRootObjectLODNode(), false, true);
				farInstances += record->instanceCount - before;
				walkingFar = false;
			}
		}
		else if (Player) WalkCell(Player->parentCell);
		// the player's third-person body (not in the cells' lists). (2026-11-05: indoors too: it was walked only in the exterior branch above,
		// so indoors the player was never in the RT scene, first or third person: rtfull-body2 at Lucky 38, 0.0 body instances)
		if (actorsOn && rtSkin && Player && Player->GetNode()) {
			walkingPlayer = true;
			const UInt32 before = record->instanceCount;
			Walk(Player->GetNode(), false);
			playerInstances += record->instanceCount - before;	// (2026-10-30) the scene log: is the body traced in first person?
			walkingPlayer = false;
		}
		treeProbe = false;
		if (grassOn) AddGrass();	// UNOFFICIAL 2026-10-30: the grass drawn this frame (RtCore::NoteGrass)
		instances = record->instanceCount;

		// Geometries not met for a while (their cell unloaded, the object gone): released in the helper
		if ((sceneFrame % 30) == 0) {
			FreeLeafCards(false);
			for (auto it = geometries.begin(); it != geometries.end(); ) {
				if (sceneFrame - it->second.lastSeen > kReleaseAfterFrames) {
					if (it->second.sent) Release(it->second.id);
					it = geometries.erase(it);
				}
				else ++it;
			}
		}
		record->arenaTexels = arenaUsed;
	}
}

void RtCore::AddUser(const User& user) { TheCore.users.push_back(user); }
D3DXVECTOR3 RtCore::Eye() { return D3DXVECTOR3(0.0f, 0.0f, 0.0f); }

void RtCore::SetMaskHook(UInt32 (*Hook)(NiGeometry*, bool)) { MaskHook = Hook; }

int RtCore::AddImage(const char* Name, UINT Width, UINT Height, NVR_EXT_SHARED_FORMAT Format) {
	if (!TheCore.inSetup || !Width || !Height) return -1;
	TheCore.imageNames.push_back(Name ? Name : "NVR RT image");
	TheCore.imageDescs.push_back({ AlignImage(Width), AlignImage(Height), Format, nullptr });
	TheCore.imageSizes.push_back({ Width, Height });
	return (int)TheCore.imageDescs.size() - 1;
}

int RtCore::AddShader(const char* Name) {
	for (size_t i = 0; i < TheCore.shaders.size(); i++) if (TheCore.shaders[i] == Name) return (int)i;
	if (TheCore.shaders.size() >= NVR_RT_MAX_SHADERS) return -1;
	TheCore.shaders.push_back(Name);
	const int index = (int)TheCore.shaders.size() - 1;
	if (TheCore.shared) {	// the helper is up: tell it (it compiles the shader while idle)
		char cwd[MAX_PATH] = {};
		GetCurrentDirectoryA(MAX_PATH, cwd);
		_snprintf_s(TheCore.shared->shaderPaths[index], sizeof(TheCore.shared->shaderPaths[index]), _TRUNCATE,
			"%s\\%sRt\\%s.spv", cwd, Paths::ComputeDir(), Name);
		MemoryBarrier();
		TheCore.shared->shaderCount = (uint32_t)TheCore.shaders.size();
		SetEvent(TheCore.frameEvent);
	}
	return index;
}

bool RtCore::CopyIn(int Image, IUnknown* Source, const RECT* Rect) {
	if (!TheCore.recording || Image <= 0 || Image >= (int)TheCore.images.size() || !Source) return false;
	return SUCCEEDED(TheCore.share->CopyToShared(TheCore.images[Image], 0, 0, Source, Rect));
}

bool RtCore::AddPass(int Shader, const int* Images, UINT ImageCount, UINT WriteMask, const void* Constants, UINT ConstantsSize, UINT GroupsX, UINT GroupsY) {
	NvrRtFrame* f = TheCore.record;
	if (!TheCore.recording || !f || f->passCount >= NVR_RT_MAX_PASSES || Shader < 0 || ImageCount > NVR_RT_PASS_IMAGES_MAX || ConstantsSize > NVR_RT_PASS_CONSTANTS)
		return false;
	NvrRtPass& p = f->passes[f->passCount];
	memset(&p, 0, offsetof(NvrRtPass, constants));
	p.shader = (uint32_t)Shader;
	p.groupsX = GroupsX;
	p.groupsY = GroupsY;
	p.groupsZ = 1;
	for (UINT i = 0; i < ImageCount; i++) {
		if (Images[i] <= 0 || Images[i] >= (int)TheCore.images.size()) return false;
		if (i < NVR_RT_PASS_IMAGES) p.images[i] = (uint32_t)Images[i];
		else if (i < NVR_RT_PASS_IMAGES + 4) p.extraImages |= (uint32_t)Images[i] << (8u * (i - NVR_RT_PASS_IMAGES));	// images 8..11: bindings 12..15 (2026-10-30)
		else p.extraImages2 |= (uint32_t)Images[i] << (8u * (i - NVR_RT_PASS_IMAGES - 4));	// images 12..15: bindings 16..19 (2026-11-02, protocol 5)
	}
	p.imageCount = ImageCount;
	p.writeMask = WriteMask;
	p.constantsSize = ConstantsSize;
	if (ConstantsSize) memcpy(p.constants, Constants, ConstantsSize);
	f->passCount++;
	return true;
}

bool RtCore::CopyOut(int Image, IUnknown* Target) {
	if (!TheCore.recording || Image <= 0 || Image >= (int)TheCore.images.size() || !Target) return false;
	TheCore.copiesOut.push_back({ Image, Target, 0, 0 });
	return true;
}

bool RtCore::CopyOut(int Image, IUnknown* Target, UINT Width, UINT Height) {
	if (!TheCore.recording || Image <= 0 || Image >= (int)TheCore.images.size() || !Target || !Width || !Height) return false;
	TheCore.copiesOut.push_back({ Image, Target, (std::min)(Width, TheCore.imageSizes[Image].first), (std::min)(Height, TheCore.imageSizes[Image].second) });
	return true;
}

namespace {
	bool Core::Ready() {
		if (state == Core::Off) return false;
		bool wanted = false;
		for (const RtCore::User& user : users) wanted = wanted || (!Out(user) && user.Wants && user.Wants());
		if (!wanted) return false;	// a running helper stays up (idle); it exits with the game
		if (state == Core::Idle) { Start(); return false; }
		Poll();
		if (state != Core::Running) return false;
		if (TheRenderManager->width != width || TheRenderManager->height != height) {
			Stop("the screen size changed (the shared images are made again)");
			state = Core::Idle;
			return false;
		}
		return true;
	}

	// One frame record = one exchange with the helper. The record counter (frame) counts exchanges, so each exchange has its own
	// inputsReady / outputReady values. The geometry staging of an exchange is recorded after the previous exchange's WaitShared
	// (exchanges are recorded one after the other on the render thread), so the arena rule holds with two exchanges a frame.
	bool Core::Exchange(IDirect3DDevice9* Device, const RtCore::User* only) {
		Core& c = *this;
		// The ring slot: the helper must have copied out the record that used it last (normally long ago; a few ms at worst)
		const UInt64 n = c.frame + 1;
		for (int i = 0; i < 100 && c.shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n; i++) Sleep(1);
		if (c.shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n) {
			static UInt32 skipped = 0;
			if ((skipped++ % 300) == 0) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper behind (frame %llu, read up to %lld): this frame not traced (%u so far).",
				(unsigned long long)n, (long long)c.shared->consumedFrame, skipped);
			return false;
		}
		c.frame = n;
		NvrRtFrame& f = c.shared->frames[n % NVR_RT_FRAME_RING];
		f.frame = 0;
		MemoryBarrier();
		f.opCount = f.instanceCount = f.passCount = f.arenaTexels = 0;
		c.record = &f;
		c.recording = true;
		c.copiesOut.clear();

		const UInt32 now = TheFrameRateManager->FrameNumber + 1;
		if (c.gatheredFrame != now) {
			c.glows.clear();
			c.Gather();
			c.frameGlows = c.glows;
			// 2026-10-30: with fork 0034 the texel after this frame's geometry gets the frame's sequence number, written after the
			// conversions; the helper checks it before building (a reordered copy = an earlier flush's data: built empty, logged once)
			if (c.rtSeq && f.arenaTexels && f.arenaTexels < c.arenaCapacity &&
				SUCCEEDED(c.rtSeq->FlushStagedGeometrySeq(c.images[0], NvrRtSequenceOf(n), f.arenaTexels)))
				f.arenaTexels |= NVR_RT_ARENA_SEQUENCE;
			else c.rt->FlushStagedGeometry(c.images[0]);
			c.frameInstances.assign(f.instances, f.instances + f.instanceCount);
			c.frameRecords.assign(f.records, f.records + f.instanceCount);
			c.gatheredFrame = now;
		}
		else {
			// The frame's second exchange: the same scene, no geometry ops (they went with the first record), no arena rows.
			memcpy(f.instances, c.frameInstances.data(), c.frameInstances.size() * sizeof(NvrRtInstance));
			memcpy(f.records, c.frameRecords.data(), c.frameRecords.size() * sizeof(NvrRtInstanceRecord));
			f.instanceCount = (uint32_t)c.frameInstances.size();
		}
		if (only) { if (only->Record) only->Record(Device); }
		else for (const RtCore::User& user : c.users) if (user.Point == 0 && !c.Out(user) && user.Wants && user.Wants() && user.Record) user.Record(Device);
		const bool recorded = f.passCount > 0;

		LARGE_INTEGER written;
		QueryPerformanceCounter(&written);
		f.writtenQpc = written.QuadPart;
		MemoryBarrier();
		f.frame = n;
		MemoryBarrier();
		InterlockedExchange64((volatile LONG64*)&c.shared->latestFrame, (LONG64)n);
		SetEvent(c.frameEvent);
		InterlockedExchange64(&c.lastRecorded, (LONG64)n);
		c.share->SignalShared(c.inputsReady, n);
		c.share->WaitShared(c.outputReady, n);
		for (const PendingCopy& copy : c.copiesOut) {
			const RECT used = { 0, 0, (LONG)(copy.width ? copy.width : c.imageSizes[copy.image].first),
				(LONG)(copy.height ? copy.height : c.imageSizes[copy.image].second) };
			c.share->CopyFromShared(copy.target, 0, 0, c.images[copy.image], &used);
		}
		c.copiesOut.clear();
		c.recording = false;
		c.record = nullptr;

		if (c.actorsOn && (n % 150) == 0) {	// 2026-10-31: actor structure updates over the last 150 frames (ActorStillSkip, ActorRebuildInterval)
			Logger::Log("RT CORE: actor updates a frame: %.2f posed (%.2f of them rebuilt afresh), %.2f still (not posed: ActorStillSkip %s, threshold %.3f), "
				"%.2f between poses (ActorUpdateInterval %u, rebuild interval %u); helper builds %.3f ms avg", c.actorUpdates[0] / 150.0f, c.actorUpdates[3] / 150.0f,
				c.actorUpdates[1] / 150.0f, c.actorStillSkip ? "on" : "off", c.actorStillThreshold, c.actorUpdates[2] / 150.0f, c.actorInterval, c.actorRebuild,
				c.shared->buildUsAvg / 1000.0);
			memset(c.actorUpdates, 0, sizeof(c.actorUpdates));
		}
		if (n == 60 || (n % 600) == 0) {
			const NvrRtShared* s = c.shared;
			Logger::Log("RT CORE: frame %llu, %u instances, %u geometries known (%u waiting for arena space, %u released); helper: %u geometries, %llu triangles, "
				"%llu MB; GPU %.3f ms avg (builds %.3f, scene %.3f, passes %.3f), response %.3f ms avg (max %.3f)",
				(unsigned long long)n, c.instances, (unsigned)c.geometries.size(), c.waiting, c.released, s->geometries, (unsigned long long)s->triangles,
				(unsigned long long)(s->geometryBytes >> 20), s->gpuTimeUsAvg / 1000.0, s->buildUsAvg / 1000.0, s->sceneUsAvg / 1000.0, s->traceUsAvg / 1000.0,
				s->responseUsAvg / 1000.0, s->responseUsMax / 1000.0);
			c.shared->responseUsMax = 0;
			if (c.buildBudget || c.cullAngle > 0.0f || c.compactOn || !c.pendingReleases.empty())
				Logger::Log("RT CORE: scene cost: compact %s, %u far builds deferred by BuildTrianglesPerFrame %u, %u small objects culled (CullSmallAngle %.4f), "
					"%u releases queued", c.compactOn ? "on" : "off", c.budgetWaits, c.buildBudget, c.culledSmall, c.cullAngle, (unsigned)c.pendingReleases.size());
			c.budgetWaits = c.culledSmall = 0;
			if (PlantWind::StaticsOn || c.swayed)	// (2026-10-30) PlantSway
				Logger::Log("RT CORE: plant sway %s: %.1f plant instances leaned a frame (plant wind statics %s)", c.plantSway ? "on" : "off",
					c.swayed / (float)(n == 60 ? 60 : n == 600 ? 540 : 600), PlantWind::StaticsOn ? "on" : "off");
			c.swayed = 0;
			if (c.treeFate[0]) {
				const UInt32* t = c.treeFate;
				Logger::Log("RT CORE: tree probe (one frame): %u tree refs (NotCastShadows %u, no node %u, in SceneRadius %u); %u leaf meshes: under APP_CULLED %u, "
					"under a faded node %u, in a switch child not taken %u, no buffer data %u, reachable %u", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9]);
				for (UInt32& v : c.treeFate) v = 0;
			}
			c.treeProbe = c.leafPlanes != 0;	// the next frame's gather walks the tree refs once (above, at the next log)
			if (c.leafPlanes || c.leafFate[0]) {	// (2026-10-30) LeafCards
				const UInt32* f = c.leafFate;
				const float frames = (float)(n == 60 ? 60 : n == 600 ? 540 : 600);
				Logger::Log("RT CORE: leaf cards %u planes: %.1f leaf meshes met a frame, %.1f traced; models built %u (buffers held %u); left out: AlphaTested off %u, "
					"no corner table / texture %u, no alpha slot %u, layout not understood %u, buffers unreadable %u; waited %u", c.leafPlanes, f[0] / frames,
					f[7] / frames, f[8], (unsigned)c.leafCards.size(), f[1], f[2], f[3], f[5], f[6], f[4]);
				for (int k = 0; k < 9; k++) if (k != 8) c.leafFate[k] = 0;
			}
			if (c.farLandOn) {	// UNOFFICIAL 2026-10-31 FarLandLOD
				Logger::Log("RT CORE: far LOD land on: %.1f instances a frame in the ring %.0f .. %.0f (mask 0x80), %.1f quads a frame left out over the loaded cells",
					c.farLandInstances / (float)(n == 60 ? 60 : n == 600 ? 540 : 600), c.radius, c.farRadius, c.farLandOverGrid / (float)(n == 60 ? 60 : n == 600 ? 540 : 600));
			}
			c.farLandInstances = 0;
			c.farLandOverGrid = 0;
			if (c.farLodOn)	// UNOFFICIAL 2026-10-31 FarObjectLOD
			{
				const float frames = (float)(n == 60 ? 60 : n == 600 ? 540 : 600);
				Logger::Log("RT CORE: far object LOD on: %.1f instances a frame in the ring %.0f .. %.0f (mask 0x80); walk a frame: %.1f objects seen, "
					"%.1f culled, %.1f out of the ring, %.1f geometries, %.1f nodes (%.1f faded, %.1f switch); root missing %u frames",
					c.farInstances / frames, c.radius, c.farRadius, c.farWalk[0] / frames, c.farWalk[1] / frames, c.farWalk[2] / frames,
					c.farWalk[3] / frames, c.farWalk[5] / frames, c.farWalk[4] / frames, c.farWalk[6] / frames, c.farWalk[7]);
			}
			c.farInstances = 0;
			for (UInt32& v : c.farWalk) v = 0;
			if (c.actorsOn && Player) {	// (2026-10-30) the player in the traced scene: the third-person body (walked always), the view model (never)
				const NiNode* body = Player->GetNode();
				Logger::Log("RT CORE: player: %s person; body node %s; %.1f body instances a frame; first-person node %s (not traced)",
					Player->isThirdPerson ? "third" : "first", !body ? "none" : (body->m_flags & NiAVObject::NiFlags::APP_CULLED) ? "CULLED" : "drawn",
					c.playerInstances / (float)(n == 60 ? 60 : n == 600 ? 540 : 600), !Player->firstPersonNiNode ? "none" :
					(Player->firstPersonNiNode->m_flags & NiAVObject::NiFlags::APP_CULLED) ? "culled" : "drawn");
			}
			c.playerInstances = 0;
			if (n == 60 && c.actorsOn)
				Logger::Log("RT CORE: actors: %u skinned partitions posed in exchanges 1-60 (%s); skipped: no buffer data %u, bones %u, not described %u, "
					"streams / half positions %u, palette %u; hit textures: not PPLighting %u, no diffuse %u, texcoords in another stream %u, "
					"no slot %u, staged without texcoords %u, textured %u", c.actorPartitions, c.rtSkin ? "fork 1.8-rt-skin" : "this d3d9.dll has no skinning: off",
					c.partitionFate[0], c.partitionFate[1], c.partitionFate[2], c.partitionFate[3], c.partitionFate[4],
					c.skinTex[0], c.skinTex[1], c.skinTex[2], c.skinTex[3], c.skinTex[4], c.skinTex[5]);
			if (n == 60) {
				const UInt32* a = c.alphaFate;
				Logger::Log("RT CORE: alpha-tested meshes met (exchanges 1-60): taken %u, atlas %u slots; left out: blended only %u, AlphaTested off %u, "
					"not described %u, texcoords not in the position's stream %u, no diffuse texture %u, alpha unreadable or atlas full %u; "
					"waited for an upload %u; glow maps in the atlas %u (%s); normal maps %u (%s)", a[7], c.atlasUsed, a[0], a[1], a[2], a[3], a[4], a[5], a[6], c.glowSlots, c.glowAtlasImage >= 0 ? "GlowTextures on" : "GlowTextures off",
					c.normalSlots, c.normalAtlasImage >= 0 ? "NormalTextures on" : "NormalTextures off");
			}
			if (n == 60 && c.grassOn) {
				const UInt32* g = c.grassFate;
				Logger::Log("RT CORE: grass (exchanges 1-60, GrassRadius %.0f, GrassMaxTufts %u): %u draws noted (%u duplicates, %u not at the origin, %u from "
					"another frame), %u tufts met, %u in reach, %u over GrassMaxTufts, %u taken as instances; draws left out: not described %u, tuft layout %u, "
					"no alpha slot %u, waited %u; CPU %.3f ms a frame noting + %.3f ms gathering", c.grassRadius, c.grassMax, g[0], g[1], g[5], g[11], g[2], g[3],
					g[4], g[10], g[6], g[7], g[8], g[9], c.grassNoteMs / 60.0, c.grassGatherMs / 60.0);
			}
			// (2026-11-05) also every 600 exchanges, counted since the last line (the demo room's scene after a cell change was not logged)
			if (n == 60 || (n % 600) == 0)
				for (int k = 0; k < 3; k++) {
					const UInt32* m = c.skips[k];
					char cells[256] = "";
					if (k == 1) sprintf_s(cells, "; cells walked %u, without a land node %u, land node culled %u; land subtree: objects %u, culled %u, nodes %u, "
						"faded %u, switch %u", c.landCells[0], c.landCells[1], c.landCells[2], c.landWalk[0], c.landWalk[1], c.landWalk[2], c.landWalk[3], c.landWalk[4]);
					Logger::Log("RT CORE: %s geometry met (exchanges %u-%u): %u, taken %u; left out: no shader/data %u, skinned %u, not a lighting shader %u, "
						"decal/refraction %u, tree leaves %u, alpha %u, not described %u, refused by the fork %u%s", k == 2 ? "LOD land" : k ? "land" : "object", n == 60 ? 1u : (unsigned)(n - 599), (unsigned)n, m[kSeen], m[kTaken],
						m[kNoShader], m[kSkinned], m[kNotLighting], m[kFlags], m[kLeaf], m[kAlpha], m[kDescribe], m[kMeasure],
						cells);
					if (k == 2) { memset(c.skips, 0, sizeof(c.skips)); memset(c.landCells, 0, sizeof(c.landCells)); memset(c.landWalk, 0, sizeof(c.landWalk)); }
				}
		}
		return recorded;
	}
}

void RtCore::Frame(IDirect3DDevice9* Device) {
	Core& c = TheCore;
	if (!c.Ready()) return;
	// UNOFFICIAL 2026-10-28: only the Point-0 users exchange here; with none on, a Point-1 user's Exchange gathers the scene itself.
	bool pointZero = false;
	for (const User& user : c.users) pointZero = pointZero || (user.Point == 0 && !c.Out(user) && user.Wants && user.Wants());
	if (!pointZero) return;
	static GpuTimer timer("Ray tracing (copies + helper + waits)");
	GpuProfileScope gpu(timer, Device);
	c.Exchange(Device, nullptr);
}

// UNOFFICIAL 2026-10-28 (RT reflections): RtCore.h. Frame (earlier in the frame) has already started the helper and checked the screen.
bool RtCore::Exchange(IDirect3DDevice9* Device, const User& user) {
	Core& c = TheCore;
	if (c.state != Core::Running || !c.shared || TheRenderManager->width != c.width || TheRenderManager->height != c.height) return false;
	if (c.Out(user) || !user.Wants || !user.Wants()) return false;
	return c.Exchange(Device, &user);
}

bool RtCore::Running() { return TheCore.state == Core::Running; }
bool RtCore::LeftOut(const char* Name) { for (const char* n : TheCore.leftOut) if (n == Name || (n && Name && !strcmp(n, Name))) return true; return false; }
void RtCore::Restart(const char* Why) {	// UNOFFICIAL 2026-11-02: as a screen-size change (Core::Ready)
	Core& c = TheCore;
	if (c.state == Core::Off || c.state == Core::Idle) return;
	c.Stop(Why);
	c.state = Core::Idle;
}
const char* RtCore::StatusText() { return TheCore.status; }
const NvrRtShared* RtCore::Helper() { return TheCore.state == Core::Running ? TheCore.shared : nullptr; }
UInt32 RtCore::Instances() { return TheCore.instances; }
const std::vector<RtCore::GlowLight>& RtCore::GlowLights() { return TheCore.frameGlows; }
UInt32 RtCore::Geometries() { return (UInt32)TheCore.geometries.size(); }
UInt32 RtCore::Waiting() { return TheCore.waiting; }
UInt32 RtCore::Released() { return TheCore.released; }
bool RtCore::AlphaTested() { return TheCore.state == Core::Running && TheCore.alphaOn && TheCore.atlasImage >= 0; }
int RtCore::AlphaAtlasImage() { return TheCore.alphaOn || TheCore.hitTextures ? TheCore.atlasImage : -1; }	// shared slots
int RtCore::AlbedoAtlasImage() { return TheCore.state == Core::Running && TheCore.hitTextures ? TheCore.albedoAtlasImage : -1; }
int RtCore::GlowAtlasImage() { return TheCore.state == Core::Running && TheCore.hitTextures ? TheCore.glowAtlasImage : -1; }
int RtCore::NormalAtlasImage() { return TheCore.state == Core::Running && TheCore.hitTextures ? TheCore.normalAtlasImage : -1; }	// UNOFFICIAL 2026-11-02
bool RtCore::GrassWanted() { return TheCore.state == Core::Running && TheCore.grassOn; }

void RtCore::NoteGrass(NiGeometry* Geometry, IDirect3DBaseTexture9* Texture, const float* Instances, UINT Count, UINT VertsPerTuft, const float* ScaleMask, UInt32 AlphaRef) {
	Core& c = TheCore;
	if (c.state != Core::Running || !c.grassOn || !Geometry || !Texture || !Instances || !Count || !VertsPerTuft) return;
	LARGE_INTEGER t0, t1, freq;
	QueryPerformanceCounter(&t0);
	const bool count60 = c.sceneFrame < 60;
	const UInt32 now = TheFrameRateManager->FrameNumber + 1;
	if (c.grassFrame != now) { c.grassDraws.clear(); c.grassTufts.clear(); c.grassFrame = now; }
	if (count60) c.grassFate[0]++;
	// the same pass again this frame (its fading tufts after the land, or the grass shadow redraw): already noted
	for (const Core::GrassDraw& d : c.grassDraws)
		if (d.geometry == Geometry && d.firstTuft[0] == Instances[0] && d.firstTuft[1] == Instances[1] && d.firstTuft[2] == Instances[2]) { if (count60) c.grassFate[1]++; return; }
	const NiTransform& w = Geometry->m_worldTransform;	// the tufts are world positions with the model at the origin (GpuGrass's AtOrigin)
	if (fabsf(w.pos.x) + fabsf(w.pos.y) + fabsf(w.pos.z) > 0.5f || fabsf(w.scale - 1.0f) > 1e-3f) { if (count60) c.grassFate[5]++; return; }
	Core::GrassDraw d = { Geometry, Texture, VertsPerTuft, AlphaRef ? AlphaRef : 128u, (UInt32)(c.grassTufts.size() / 4), 0,
		{ ScaleMask[0], ScaleMask[1], ScaleMask[2] }, { Instances[0], Instances[1], Instances[2] } };
	const float cx = TheRenderManager->CameraPosition.x, cy = TheRenderManager->CameraPosition.y, cz = TheRenderManager->CameraPosition.z;
	const float r2 = c.grassRadius * c.grassRadius;
	UInt32 inReach = 0, over = 0;
	for (UINT i = 0; i < Count; i++) {
		const float* t = Instances + 4 * i;
		const float dx = t[0] - cx, dy = t[1] - cy, dz = t[2] - cz;
		if (dx * dx + dy * dy + dz * dz > r2) continue;
		inReach++;
		if (c.grassTufts.size() / 4 >= c.grassMax) { over++; continue; }
		c.grassTufts.insert(c.grassTufts.end(), t, t + 4);
		d.count++;
	}
	if (d.count) c.grassDraws.push_back(d);
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&freq);
	if (count60) {
		c.grassFate[2] += Count; c.grassFate[3] += inReach; c.grassFate[4] += over;
		c.grassNoteMs += 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
	}
}

bool RtCore::Actors() { return TheCore.state == Core::Running && TheCore.actorsOn && TheCore.rtSkin; }
