// UNOFFICIAL 2026-10-28 (NVR DXVK): the ray tracing core (RtCore.h; docs\next\FORK-RT-CORE.md). The helper's side is
// tools\nvr-rt-helper (MIT, built on its own, 64-bit); the protocol is tools\nvr-rt-helper\NvrRtProtocol.h. The launch, the shared
// block, the watchdog and the stop follow UpscalerDlss.cpp (the DLSS helper), which proved them.

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "RtCore.h"
#include "RtPathTrace.h"	// UNOFFICIAL 2026-11-11: ReferenceOn (ReferencePT as honoured)
bool RtMaterialRuleOf(NiGeometry* geometry, float* roughness, float* metal);	// UNOFFICIAL 2026-11-03 (NewVegas/Hooks/Render.cpp)
const char* RtMaterialClassOf(NiGeometry* geometry);	// UNOFFICIAL 2026-11-07 ptaaa-materials (NewVegas/Hooks/Render.cpp)
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "PlantWind.h"	// (2026-10-30) PlantSway: a wind-bent plant's traced instance leans with it
#include "../../tools/nvr-rt-helper/NvrRtProtocol.h"
#include "RtScenePick.h"	// (2026-11-06) the scene picking rules (tested by tools/nvr-rt-host)
#include "RtTexturePack.h"	// (2026-11-07, protocol 8) the real textures' routes, packing, budget and release (tested by tools/nvr-rt-host)
#include "RtVideoMemory.h"	// (2026-11-07) the GPU's video memory for RealTextureMaxSize / RealTextureBudgetMB = 0 (auto)
#include "RtFaceMorph.h"	// (2026-11-07) faces and lip-sync: a head's change detection and feed (tested by tools/nvr-rt-host "facemorph")
#include "RtHairTint.h"	// (2026-11-07 perf-mirrorfix) the hair shaders' tint, noted per geometry by Render.cpp
#include "DisplaceShadows.h"	// (2026-11-07 rtgap-tessblas) the land draws' LandHeight (which layers have a height)

// UNOFFICIAL 2026-11-05 (rtrefl-only: the demo room's ceiling met by no reflection ray) the first 64 distinct meshes a cell that the
// traced scene leaves out, with why.
static const UInt32 kSceneLogCap = 3000;	// (rt-audit F9) lines a session for each of the three scene loggers below
static void LogLeftOut(const NiGeometry* geo, const char* why, int shaderType) {
	static UInt32 sessionLines = 0;	// UNOFFICIAL 2026-11-13 (rt-audit F9): a session cap (re-armed each cell, these wrote 60,000 flushed lines in 35 min outdoors)
	if (sessionLines >= kSceneLogCap) return;
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
	sessionLines++, Logger::Log("RT CORE: left out of the traced scene: %s", key.c_str());
}

// UNOFFICIAL 2026-11-06 (alpha lamp shadows: VS-AlphaLampTest's chain-link fence cast no traced shadow) the first 64 meshes (by name) a
// cell that the traced scene takes, alpha-tested ones with their atlas slot and alpha reference: with LogLeftOut, which meshes rays can meet.
static void LogAlphaTaken(const NiGeometry* geo, int slot, unsigned ref, bool blended) {
	static UInt32 sessionLines = 0;	// UNOFFICIAL 2026-11-13 (rt-audit F9): a session cap (re-armed each cell, these wrote 60,000 flushed lines in 35 min outdoors)
	if (sessionLines >= kSceneLogCap) return;
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
	if (ref > 255u) sessionLines++, Logger::Log("RT CORE: in the traced scene: '%s' opaque, bound centre z %+.0f (camera relative), r %.0f", geo && geo->m_pcName ? geo->m_pcName : "", cz, r);
	else {
		// (2026-11-06 d) the alpha property's test function (flags bits 10-12: 0 ALWAYS, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL,
		// 6 GEQUAL, 7 NEVER); the RT test assumes GREATER
		const NiAlphaProperty* ap = geo ? (const NiAlphaProperty*)const_cast<NiGeometry*>(geo)->GetProperty(NiProperty::PropertyType::kType_Alpha) : nullptr;
		static const char* const funcs[8] = { "ALWAYS", "LESS", "EQUAL", "LEQUAL", "GREATER", "NOTEQUAL", "GEQUAL", "NEVER" };
		sessionLines++, Logger::Log("RT CORE: alpha-tested in the traced scene: '%s' atlas slot %d, alpha ref %u, test %s (flags %04X)%s, bound centre z %+.0f, r %.0f",
			geo && geo->m_pcName ? geo->m_pcName : "", slot, ref, ap ? funcs[(ap->flags >> 10) & 7] : "?", ap ? (unsigned)ap->flags : 0u, blended ? " (also blended)" : "", cz, r);
	}
}

// UNOFFICIAL 2026-11-06 (gs-hill: PTPrimary 0 showed a huge rounded mesh with huge texels just left of the camera in Goodsprings, drawn
// by no raster pass; the 64-name cap above never reached it) every LARGE taken mesh (world bound radius 300+), once a cell, up to 256:
// its bound centre (camera relative, x y z) and radius, its world scale and its parents' names (the NIF's nodes), so a mesh far bigger
// than it should be, or standing where the game draws nothing, can be named.
static void LogBigTaken(const NiGeometry* geo) {
	static UInt32 sessionLines = 0;	// UNOFFICIAL 2026-11-13 (rt-audit F9): a session cap (re-armed each cell, these wrote 60,000 flushed lines in 35 min outdoors)
	if (sessionLines >= kSceneLogCap) return;
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
	sessionLines++, Logger::Log("RT CORE: big mesh in the traced scene: '%s' bound centre %+.0f %+.0f %+.0f (camera relative), r %.0f, scale %.2f%s",
		geo->m_pcName ? geo->m_pcName : "", b->Center.x - cam.x, b->Center.y - cam.y, b->Center.z - cam.z, b->Radius, geo->m_worldTransform.scale, parents.c_str());
}

namespace {
	const UINT64 kReleaseMargin = 1ull << 20;	// the watchdog / Stop release every wait that could still be recorded
	const UINT kArenaWidth = 4096;
	const UINT kReleaseAfterFrames = 120;		// a geometry not met for this many frames is released in the helper
	// UNOFFICIAL 2026-11-07 (rtgap-grass, RT-DETAIL-GAPS.md 6) PTRealtime turns the RT grass on by itself, its reach at most this (game units:
	// ~14 m; the default GrassRadius 1500 is for RT shadows, the path tracer pays for every tuft in every bounce)
	const float kPtGrassRadius = 1000.0f;
	// The decal's vertex signature (RtScenePick: a new dynamic decal at a dropped one's addresses): the vertex count and every position,
	// bit for bit, hashed (FNV-1a). 0: no CPU copy of the positions (the addresses alone tell it then).
	UInt32 DecalSignature(const NiGeometryData* gd) {
		if (!gd || !gd->m_pkVertex || !gd->m_usVertices) return 0;
		UInt32 h = 2166136261u ^ gd->m_usVertices;
		const UInt32* w = (const UInt32*)gd->m_pkVertex;
		for (UInt32 i = 0, n = 3u * gd->m_usVertices; i < n; i++) { h ^= w[i]; h *= 16777619u; }
		return h ? h : 1u;
	}
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
		bool	alphaBuilt = false;	// (2026-11-07 perf-mirrorfix) skinned: its last build was non-opaque (an alpha-tested part)
		bool	skinned = false;	// an actor's partition (dropped at once when the player changes cell)
		// UNOFFICIAL 2026-10-31 actors (docs\next\RT-SUN-SOFT-AND-BUILD.md): the camera its last pose was posed against (the posed
		// vertices are camera relative: the instance moves them by pose camera - camera), that pose's world-space bone palette
		// (ActorStillSkip) and the poses sent since its last full build (ActorRebuildInterval)
		float	poseCamera[3] = { 0.0f, 0.0f, 0.0f };
		std::vector<float> palette;
		UInt32	poses = 0;
		// UNOFFICIAL 2026-11-07 (rtgap-decals) decals: the model-space average normal the instance is lifted along (normalState 0 not read
		// yet, 1 known, -1 none: the decal stays in place), and its vertices' signature: the game makes and drops dynamic decals (blood) all
		// the time, and a new one can come back at a dropped one's addresses (buffer data, VB, IB): a signature that changed is a new decal,
		// released and built again (never the old shape)
		float	decalNormal[3] = { 0.0f, 0.0f, 1.0f };
		int		normalState = 0;
		UInt32	decalSignature = 0;
		// UNOFFICIAL 2026-11-07 (rtgap-tessblas, protocol 8) displaced geometry (Core::Displace): the DATA ids sent for it (land layer
		// weights A / B, normals; 0 none) and where it stands: 0 not looked at, 1 data sent (the source op waits for its textures),
		// 2 the source op sent (its instances are flagged), 3 never (no texcoords, no textures, a vertex layout the data cannot take)
		UInt32	dispData[3] = { 0, 0, 0 };
		UInt8	dispState = 0;
		UInt32	triangles = 0;		// (2026-11-07 ptaaa-emissive) its triangle count when built (PTEmissive's list; 0: unknown)
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
	// (2026-11-08 perf-reflfix4, the lead's in-game run: the reflected hair still white with the tint in its record) texture slot 2 is the
	// game's "glow/skin sst/hair layer texture" (decomp bsshaderpplightingproperty.cpp): on hair it is the hair LAYER map (the vanilla hair
	// NIFs: <name>_hl.dds; SM3002 / SM3003 LayerMap at s5), on FaceGen skin the skin map (_sk.dds, ApplyFaceGenTextures). Taken as a glow
	// map, the hair's record got the GLOW flag: the glow variants (ReflectionsRT*Glow, the path tracer's emissive lights) REPLACED the hair's
	// tinted colour with the grey-white layer map and added it as light. Not a glow map on a Hair- or FaceGen-flagged property, nor on the
	// skin shader (index 0xE).
	bool SlotTwoIsLayer(BSShaderProperty* shaderProperty) {
		return (shaderProperty->ulFlags[0] & (BSShaderProperty::Hair | BSShaderProperty::FaceGen)) != 0 || shaderProperty->uiShaderIndex == 0xE;
	}
	bool HasGlowMap(BSShaderProperty* shaderProperty) {
		const UInt32 type = static_cast<NiShadeProperty*>((NiProperty*)shaderProperty)->m_eShaderType;
		if (!shaderProperty->IsLightingProperty() && type != NiShadeProperty::kProp_PPLighting && type != NiShadeProperty::kProp_Lighting30) return false;
		if (SlotTwoIsLayer(shaderProperty)) return false;
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
		// UNOFFICIAL 2026-11-07 (rtgap-tessblas) for the displaced copies' DATA ops: NORMAL0 and the land's layer weights TEXCOORD1 / 2 (stream,
		// offset, as an NVR_EXT_RT_POSITION_FORMAT: -1 = a type the fork cannot stage as a position; type = the D3DDECLTYPE + 1, 0 = absent)
		int normalStream = -1; UInt32 normalOffset = 0; int normalFormat = -1; UInt32 normalType = 0;
		int layerStream[2] = { -1, -1 }; UInt32 layerOffset[2] = { 0, 0 }; int layerFormat[2] = { -1, -1 }; UInt32 layerType[2] = { 0, 0 };
	};

	struct PendingCopy { int image; IUnknown* target; UINT width, height; };	// 0, 0: the image's registered size

	struct Core {
		enum State { Idle, Starting, Running, Off };
		State		state = Idle;
		std::vector<RtCore::User> users;
		std::vector<const char*> leftOut;				// (2026-11-05) users whose images did not fit (NVR_RT_MAX_IMAGES) at this helper start
		UINT		scratchMB = 0;					// (2026-11-07) AskScratch's largest ask this helper start
		bool		Out(const RtCore::User& user) const { for (const char* n : leftOut) if (n == user.Name) return true; return false; }
		ID3D9NvrExtShare*		share = nullptr;
		ID3D9NvrExtRt*			rt = nullptr;
		ID3D9NvrExtRtSkin*		rtSkin = nullptr;	// fork patch 0033 (skinned actors); null without it
		ID3D9NvrExtRtSeq*		rtSeq = nullptr;	// fork patch 0034 (the arena's sequence texel, 2026-10-30); null without it
		bool		actorsOn = false;					// [Main.Main.RayTracing] Actors (read at each helper start)
		bool		walkingPlayer = false;				// Gather's walk of the player's own node (NVR_RT_MASK_PLAYER on its rigid parts)
		bool		playerHidden = false;				// (2026-11-07) ... in first person: its instances get NVR_RT_CUSTOM_HIDDEN (not drawn)
		bool		firstPersonBody = false;			// UNOFFICIAL 2026-11-05 [Main.Main.RayTracing] FirstPersonBody: the body is traced in first person too (off 2026-11-07, on again with ptaaa-firstperson: RtHidden.glsl)
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
		UInt32		actorInterval = 1;					// [Main.Main.RayTracing] ActorUpdateInterval (live)
		bool		actorStillSkip = false;				// [Main.Main.RayTracing] ActorStillSkip (live, 2026-10-31)
		float		actorStillThreshold = 0.05f;		// [Main.Main.RayTracing] ActorStillThreshold (live)
		UInt32		actorRebuild = 8;					// [Main.Main.RayTracing] ActorRebuildInterval (live; 0 never)
		UInt32		actorUpdates[4] = {};				// since the last line: posed, still (not posed), between poses (ActorUpdateInterval), rebuilds
		UInt32		actorPartitions = 0;				// skinned partitions staged (exchanges 1-60)
		UInt32		actorAlphaParts = 0, actorTintedParts = 0;	// (2026-11-07 perf-mirrorfix) poses built alpha-tested, of them tinted (hair)
		UInt32		faceGains[2] = { 0, 0 };				// (perf-mirrorfix2) FaceGen face records: [0] gain from the maps, [1] the x 2 fallback
		int			faceGainLogs = 0;
		UInt32		hairParts[2][4] = {};				// (2026-11-08 perf-reflfix4) hair parts met (a part a frame): [rigid, skinned][RtHairTint::Source]
		const void*	hairTold[8] = {};					// ... the first 8 logged (HairTintOf)
		int			hairLogs = 0;
		int			HairTintOf(NiGeometry* geo, BSShaderProperty* shaderProperty, bool skinned, bool alphaPart, float rgb[3]);
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
		UInt32		startNotes = 0;						// (2026-11-08 perf-reflfix4) "still getting ready" lines written (one every 30 s)
		static constexpr ULONGLONG kHelperReadyMs = 300000;	// (2026-11-08 perf-reflfix4) how long the helper may take to get ready (was 60 s)
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
		// UNOFFICIAL 2026-11-10 (PT early kick, env NVR_RT_EARLY_PT; docs/next/PT-EARLY-KICK-PLAN-2026-11-10.md) RtCore::SubmitEarly's open
		// frame: its copies out wait here for CollectEarly (copiesOut / record are reused by the frame's later exchange). One frame in flight.
		bool		submitOnly = false;			// Exchange: signal and return; the copies go to pendingOut (set by SubmitEarly only)
		std::vector<PendingCopy> pendingOut;
		UInt64		pendingN = 0;				// the pending exchange's record number (0: nothing pending)
		const char*	earlyName = nullptr;		// the user submitted early, in game frame earlyFrame (FrameNumber + 1)
		UInt32		earlyFrame = 0;
		LONGLONG	earlySignalQpc = 0;
		UInt32		earlySubmitted = 0, earlyRefused = 0, earlyCollected = 0;	// since the last log line
		double		earlyOverlapMs = 0.0;
		const char*	earlyRefusal = "none";
		void		CollectEarly(IDirect3DDevice9* Device);
		bool		EarlyThisFrame(const char* name) const;	// that user's Record went with this game frame's SubmitEarly
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
		bool		blendedHairOn = true;	// (2026-11-10) [Main.Main.RayTracing] BlendedHair: the game's own blend-only hair as a cut-out (AddGeometry)
		// alpha-tested meshes by fate (exchanges 1-60): [0] blended only, [1] feature off, [2] not described, [3] texcoords not in the
		// position's stream, [4] no diffuse texture, [5] alpha unreadable (format / pool) or atlas full, [6] waited (upload budget), [7] taken
		UInt32		alphaFate[8] = {};
		// UNOFFICIAL 2026-11-07 (rtgap-decals) [Main.Main.RayTracing] Decals (live, with AlphaTested): decals by fate (exchanges 1-60, and
		// every 600): [0] met, [1] taken (instances), [2] lifted, [3] taken without a normal (not lifted), [4] rebuilt (a new decal at a
		// dropped one's addresses), [5] left out (Decals or AlphaTested off). The rest of [0] took an alpha-tested fate (alphaFate, LogLeftOut).
		bool		decalsOn = false;
		bool		glassOn = false;	// (2026-11-07 ptaaa-materials) PTGlass with the path tracer on (ReferencePT / PTRealtime), AlphaTested + the atlas
		UInt32		glassTaken = 0, skinParts = 0, foliageCards = 0;	// (ptaaa-materials) this frame's glass panes, skin partitions, foliage cards (logged)
		UInt32		decalFate[6] = {};
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
		// UNOFFICIAL 2026-11-07 (protocol 7; protocol 8 rtgap-textures) real textures ([Main.Main.RayTracing] RealTextures, restart): every
		// texture an atlas slot is made for also goes to the helper whole (its levels from RealTextureMaxSize down) on the GPU route: the fork's
		// CopyToShared puts each level into a transfer image (BC1 / BC3 / BC5 blocks as the game keeps them with fork 1.8-rt-bc, else decoded
		// to RGBA8; texMapping / texStaging, the CPU "-tex" block, are never made: nothing to lock in the DEFAULT pool). The helper samples it
		// filtered with mips (RtTextures.glsl). The atlas stays: the fallback (and what older shaders read). docs/next/RT-REAL-TEXTURES.md.
		bool		realTextures = false;
		HANDLE		texMapping = nullptr;
		UInt8*		texStaging = nullptr;
		UInt32		texMaxSize = 1024;						// RealTextureMaxSize (0 = auto by video memory): the biggest level sent (bigger textures start at a smaller mip)
		UInt64		texBudget = 0;							// RealTextureBudgetMB (0 = auto) in bytes, against texRes.alive (the helper's copies, the route's format)
		// UNOFFICIAL 2026-11-13 (rt-audit F4) auto budget: its cap at start (min(tier, 20 % of the process's local budget)) and the live watch
		bool		texAuto = false, texLuidValid = false;
		BYTE		texLuid[8] = {};
		UInt64		texBudgetCap = 0;
		UInt32		vramShrinks = 0;
		void		VramWatch(NvrRtFrame& f, UInt64 n);
		// (2026-11-07, protocol 8, rtgap-textures) the routes (RtTexturePack.h): RGBA8 (the fork's decoding blit) and, with fork 1.8-rt-bc,
		// BC1 / BC3 / BC5 (the game's blocks as they are); a transfer image each (two halves: exchange parity), its own shelf packing
		uint32_t	texCapBc = 1024, texCapRgba = 1024;		// the biggest level a route sends
		bool		texRoute[RtTexturePack::RouteCount] = {};	// the route's transfer image exists
		int			texImages[RtTexturePack::RouteCount] = { -1, -1, -1, -1 };
		uint32_t	texW[RtTexturePack::RouteCount] = {}, texHalfH[RtTexturePack::RouteCount] = {};
		RtTexturePack::Shelf texShelf[RtTexturePack::RouteCount];
		RtTexturePack::Residency texRes;					// ids alive in the helper: bytes, last use, release (least recently used first)
		std::vector<IDirect3DBaseTexture9*> texKeyOf;		// by id: its texIds key (erased when the id is released)
		struct TexEntry { UInt32 id = 0; bool tried = false; UInt64 retryAt = 0; };	// retryAt: a budget refusal, tried again from that exchange
		std::unordered_map<IDirect3DBaseTexture9*, TexEntry> texIds;	// 0 = not sendable (format, pool, budget): the slot keeps the atlas
		struct SlotWant { int slot; IDirect3DBaseTexture9* tex[3]; };	// diffuse, normal, glow
		std::vector<SlotWant> slotQueue;
		// UNOFFICIAL 2026-11-11 (sungi, RT use-after-free): a queued want may wait several exchanges (op list or budget full), and the
		// game can free its textures meanwhile (cell unload, LOD swap): SendTexture then called a freed texture's GetType (EIP 0, slot C
		// 2026-11-09). Each texture in slotQueue / looseTextures holds one reference from the push to the erase / clear.
		void QueueSlot(int slot, IDirect3DBaseTexture9* t, IDirect3DBaseTexture9* nm, IDirect3DBaseTexture9* g) {
			SlotWant w = { slot, { t, nm, g } };
			for (IDirect3DBaseTexture9* x : w.tex) if (x) x->AddRef();
			slotQueue.push_back(w);
		}
		void DropSlots(size_t count) {
			for (size_t i = 0; i < count && i < slotQueue.size(); i++) for (IDirect3DBaseTexture9* x : slotQueue[i].tex) if (x) x->Release();
			slotQueue.erase(slotQueue.begin(), slotQueue.begin() + (std::min)(count, slotQueue.size()));
		}
		void DropLoose(size_t count) {
			for (size_t i = 0; i < count && i < looseTextures.size(); i++) if (looseTextures[i]) looseTextures[i]->Release();
			looseTextures.erase(looseTextures.begin(), looseTextures.begin() + (std::min)(count, looseTextures.size()));
		}
		void ClearTextureQueues() { DropSlots(slotQueue.size()); DropLoose(looseTextures.size()); }
		// UNOFFICIAL 2026-11-11 (sungi, ABA guard F2): texIds / texKeyOf / alphaSlots are keyed by the texture's address and outlive the
		// game's texture: a new texture allocated at a freed one's address inherited its RT id / atlas slot (wrong colour, wrong alpha at
		// hits). Now one COM reference is held per texture with a live texKeyOf id (taken when the id is assigned, given back when the id is
		// released or at the full clears); alphaSlots keys are not pinned (see AlphaSlotOf; alphaHeld stays empty). The VRAM tradeoff: a texture the
		// game dropped stays alive until its id is released (RealTextureBudgetMB LRU, kTexIdle exchanges unused).
		std::vector<IDirect3DBaseTexture9*> alphaHeld;
		// UNOFFICIAL 2026-11-13 (rt-audit F3/F5, RTAudit\AUDIT.md) atlas slot recycling. The 1024 alpha / albedo atlas slots were never given back and
		// were keyed by the texture's address: a cell reloaded at new addresses took new slots, and the user's 35-min session filled the atlas at
		// 09:27 ("1023 slots pointed"); from then on every new alpha-tested mesh was left out of the RT scene ("atlas full", 2,258 lines in 7 min)
		// and no real texture was sent again. Now each slot holds one COM reference to its diffuse texture (slotTex) and a sweep (SweepSlots,
		// kSlotSweep slots an exchange) frees a slot whose texture only we still hold (the game dropped it): its alphaSlots key erased (so the
		// address can never be reused while the key lives: the ABA of SunGI F2 / AUDIT F5), the slot reused from kSlotReuse exchanges on.
		struct SlotTex { IDirect3DBaseTexture9* diffuse = nullptr; UInt64 key = 0; };
		std::vector<SlotTex> slotTex;
		struct FreeSlot { UInt32 slot; UInt64 at; };
		std::vector<FreeSlot> freeSlots;
		std::unordered_map<IDirect3DBaseTexture9*, UInt32> slotHolds;	// slots holding a texture (one diffuse can key two slots: with / without a glow map)
		std::vector<UInt8> slotTableDirty;	// a freed slot's helper table row still names the old ids: the reuse's SLOT_TEXTURES op always goes
		UInt32		slotSweepAt = 0, slotsFreed = 0, slotsReused = 0;
		static const UInt32 kSlotSweep = 64;				// slots checked an exchange (the 1024 in 16 exchanges)
		static const UInt64 kSlotReuse = 64;				// exchanges before a freed slot is drawn into again (frames in flight read it)
		void		SweepSlots();
		void ReleaseTexRefs() {
			for (IDirect3DBaseTexture9*& x : texKeyOf) if (x) { x->Release(); x = nullptr; }
			for (IDirect3DBaseTexture9* x : alphaHeld) if (x) x->Release();
			alphaHeld.clear();
			for (SlotTex& s : slotTex) if (s.diffuse) { s.diffuse->Release(); s.diffuse = nullptr; }	// (rt-audit F3)
			slotTex.clear();
			slotHolds.clear();
			slotTableDirty.clear();
			freeSlots.clear();
			slotSweepAt = 0;
		}
		// per atlas slot: the ids its SLOT_TEXTURES op named; state 0 none, 1 queued, 2 sent, 3 short (a texture refused by the budget or
		// released): queued again when the slot is next used from retryAt on
		struct SlotReal { UInt32 ids[3] = {}; UInt8 state = 0; UInt64 retryAt = 0; };
		std::vector<SlotReal> slotReal;
		static const UInt32 kTexSweep = 256;					// UNOFFICIAL 2026-11-13 (rt-tex-release) ids checked for a game-dropped texture an exchange
		UInt32		texSweepAt = 1, texDropped = 0;				// the sweep's next id; ids released because the game dropped the texture
		static const UInt64 kTexIdle = 600;					// exchanges unused before a texture may be released for a new one
		static const UInt64 kTexReuse = 64;					// exchanges before a released id is given out again (frames in flight)
		static const UInt64 kTexRetry = 300;				// exchanges before a texture the budget refused is tried again
		UInt32		texSent = 0, texFailed = 0, texSlotsSent = 0, texReleased = 0, texRouteSent[RtTexturePack::RouteCount] = {};
		double		texCpuMs = 0.0;							// time spent recording the copies (the scene log)
		// UNOFFICIAL 2026-11-07 (RT-DETAIL-GAPS.md section 2) real normal maps ([Main.Main.RayTracing] RealNormalMaps, restart, with
		// RealTextures): every slot's normal map goes to the helper as a real texture whatever the normal atlas (NormalTextures), and the
		// records whose slot has one get NVR_RT_RECORD_NORMAL, so the path tracer's bounces, RT reflections and RT GI bend their hits'
		// normals (RtHit.glsl rtHitNormalMapped). slotNormals: the normal map each slot was sent with (a slot first made without one, by
		// an actor or a leaf card, is pointed at it later by another SLOT_TEXTURES op). Its id is the slot's SlotReal::ids[1]: released and
		// sent again like the colour; past 3/4 of RealTextureBudgetMB (the helper's real bytes, texRes.alive) a normal map waits (tried
		// again kTexRetry exchanges later, as a budget refusal) so the colour keeps the rest.
		bool		realNormals = false;
		std::vector<IDirect3DBaseTexture9*> slotNormals;
		UInt32		normalsHeld = 0;						// normal maps held back at 3/4 of the budget (times)
		void		PumpTextures(NvrRtFrame& f, UInt64 n);
		int			SendTexture(IDirect3DBaseTexture9* texture, NvrRtFrame& f, UInt64 n, bool& budgetRefused);
		void		ReleaseTexture(UInt32 id, NvrRtFrame& f, UInt64 n);
		void		NoteSlotUse(int slot, IDirect3DBaseTexture9* texture, IDirect3DBaseTexture9* normal, IDirect3DBaseTexture9* glow);
		PositionElement lastElement;						// Describe's: where the texcoords are
		int			AlphaSlotOf(IDirect3DBaseTexture9* texture, IDirect3DBaseTexture9* glow = nullptr, IDirect3DBaseTexture9* normal = nullptr);		// land subtree: objects popped, APP_CULLED, nodes, faded fade nodes (alpha <= 0, walked anyway), switch nodes
		// UNOFFICIAL 2026-11-07 (rtgap-tessblas, protocol 8) displaced geometry ([Main.Main.RayTracing] Displacement; docs/next/RT-DETAIL-GAPS.md
		// 1c, tools/nvr-rt-helper/NvrRtDisplace.inl): with the game's tessellation on, the land and the parallax objects within FadeEnd are
		// flagged NVR_RT_INSTANCE_DISPLACE and the helper traces them through displaced copies. dispLand / dispObjects: on this frame (the
		// raster's displacement on, and the RT setting); dispParams: the five NvrRtDisplaceParamsOp blocks; dispSentOn: they went out
		// enabled last frame (one more, disabled, when both go off).
		bool		dispLand = false, dispObjects = false, dispSentOn = false;
		float		dispParams[NVR_RT_DISPLACE_BLOCKS][6] = {};
		float		dispLandReach = 0.0f, dispObjectReach = 0.0f;	// FadeEnd of each
		UInt32		dispInstances = 0, dispSources = 0, dispData = 0, dispNever = 0;	// (the scene log)
		std::vector<IDirect3DBaseTexture9*> looseTextures;		// textures the displacement wants an id for (no atlas slot of their own)
		int			TextureIdFor(IDirect3DBaseTexture9* texture);	// > 0 its id, 0 never, -1 not sent yet (queued: PumpTextures)
		void		DisplaceSettings();
		void		Displace(NiGeometry* geo, GeometryEntry& entry, NvrRtInstance& instance, bool land, BSShaderProperty* shaderProperty,
						const NVR_EXT_RT_GEOMETRY_DESC& desc, bool textured);
		UInt32		instances = 0, waiting = 0, released = 0;
		UInt32		hiddenInstances = 0;	// (2026-11-07 perf-ptcost) this exchange's NVR_RT_CUSTOM_HIDDEN instances (RtCore::HiddenInstances)
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
		bool		grassOn = false;					// [Main.Main.RayTracing] Grass (read at each helper start; needs AlphaTested) or PTRealtime (live)
		bool		grassSetting = false;				// (2026-11-07) Grass as read at the helper start (grassOn = alphaOn && (it || PTRealtime))
		float		grassRadius = 1500.0f;				// GrassRadius (live; at most kPtGrassRadius under PTRealtime)
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
		// UNOFFICIAL 2026-11-07 faces and lip-sync ([Main.Main.RayTracing] FaceMorphs, FaceMorphRadius; RtFaceMorph.h; docs/next/RT-DETAIL-GAPS.md 4):
		// each skinned partition of a FaceGen head near the camera has its vertices hashed every frame (the partition's buffer, the geometry
		// data's CPU vertices, its own buffer); a change poses it even when ActorStillSkip / ActorUpdateInterval would not. When only the CPU
		// vertices (own buffer) follow the morphs, its poses read a copy of the partition's vertices patched from them (`copy`, ours).
		struct FaceState {
			RtFaceMorph::State s;
			UInt32		lastSeen = 0, firstChange = 0;
			IDirect3DVertexBuffer9* copy = nullptr;
			UInt32		copyBytes = 0, copyRefreshes = 0;
			bool		reported = false, copyFailed = false, pending = false;	// pending: a change not yet staged
			UInt32		partition = 0, partitions = 0, vertices = 0, dataVertices = 0, keepFlags = 0;
			const void*	skin = nullptr;
			const char*	how = "";
			const char*	own = "";
			bool		cpuThere = false, mapThere = false;
			char		name[64] = {};
		};
		std::unordered_map<GeometryKey, FaceState, GeometryKeyHash> faces;
		bool		faceMorphs = true;					// FaceMorphs (live)
		float		faceRadius = 800.0f;				// FaceMorphRadius (live)
		UInt32		faceThisFrame = 0;					// head partitions hashed this frame (at most kFacesAFrame)
		static const UInt32 kFacesAFrame = 24;
		// since the last actor line: [0] head partitions met within FaceMorphRadius, [1] hashed, [2] partition buffer unreadable this frame,
		// [3] poses forced by a change (the bones still or between poses), [4] copy refreshes, [5] over kFacesAFrame, [6] changes seen
		UInt32		faceStats[7] = {};
		double		faceMs = 0.0;						// CPU time of the above (hashing, copies)
		UInt32		faceReports = 0, faceFound = 0, rigidFaceGen = 0;
		RtFaceMorph::Decision FaceObserve(NiGeometry* geo, UInt32 p, const GeometryKey& key, NVR_EXT_RT_GEOMETRY_DESC& desc, const PositionElement& e, const char* how, FaceState*& out);
		int			FaceCopy(FaceState& f, NiGeometry* geo, const NiSkinPartition::Partition& part, const NVR_EXT_RT_GEOMETRY_DESC& desc, const PositionElement& e, bool refresh);
		void		FaceReport(FaceState& f, const char* why);
		void		FreeFaces(bool all);
		bool		PositionIn(NiGeometryBufferData* data, PositionElement& position);	// DescribeData's position lookup (lastElement untouched)
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
		if (normalAtlasImage < 0 && !realNormals) normal = nullptr;	// (the key leaves the normal map out: a diffuse texture comes with one normal map)
		const UInt64 key = (UInt64)(uintptr_t)texture | ((UInt64)(uintptr_t)glow << 32);	// a 32-bit process
		auto found = alphaSlots.find(key);
		if (found != alphaSlots.end()) {
			const int s = found->second;
			// (2026-11-07) RealNormalMaps: a slot first made without its normal map (an actor's or a leaf card's ask) is pointed at it now
			if (s >= 0 && normal && realNormals && (size_t)s < slotNormals.size() && !slotNormals[s]) {
				slotNormals[s] = normal;
				if ((size_t)s < slotReal.size() && slotReal[s].state != 1) { slotReal[s].state = 1; QueueSlot(s, texture, normal, glow); }
			}
			if (s >= 0 && realTextures) NoteSlotUse(s, texture, normal, glow);	// (2026-11-07, protocol 8) last use / resend
			return s;
		}
		const UInt32 capacity = NVR_RT_ALPHA_ATLAS_SLOTS * NVR_RT_ALPHA_ATLAS_SLOTS;
		// (rt-audit F3) a freed slot whose reuse delay has passed comes first; the never-used ones after
		int reuseAt = -1;
		for (size_t i = 0; i < freeSlots.size(); i++) if (freeSlots[i].at <= frame) { reuseAt = (int)i; break; }
		if (atlasImage < 0 || (atlasUsed >= capacity && reuseAt < 0)) return -1;
		if (atlasThisFrame >= 8) return -2;
		atlasThisFrame++;
		const UInt32 S = NVR_RT_ALPHA_SLOT_SIZE;
		static ID3D9NvrExtPipeline* pipelines[2] = {};	// [0] alpha only (RtAlphaToAtlas), [1] alpha + colour (RtTextureToAtlas)
		static UInt64 pipelineRetryAt[2] = {};	// UNOFFICIAL 2026-11-13 (rt-audit F12): a failed make is tried again 600 exchanges later (was: once a session)
		const int which = albedoAtlasImage >= 0 ? 1 : 0;
		ID3D9NvrExtPipeline*& pipeline = pipelines[which];
		// (2026-11-06) the alpha-only pass always too: it fills the alpha atlas at AlphaDetail texels (the colour pass stays at 64)
		for (int want = 0; want <= which; want += which ? 1 : 2) {
		ID3D9NvrExtPipeline*& made = pipelines[want];
		if (!made && frame >= pipelineRetryAt[want]) {
			pipelineRetryAt[want] = frame + 600;
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
		if (normal && normalAtlasImage >= 0 && !normalUpload) TheRenderManager->device->CreateTexture(S, S, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &normalUpload, nullptr);
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
			const UInt32 at = reuseAt >= 0 ? freeSlots[reuseAt].slot : atlasUsed;	// (rt-audit F3)
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
				if (slot >= 0 && normal && normalAtlasImage >= 0) {	// (2026-11-02) the normal map the same way: its rgb as stored (tangent space) into the normal atlas
					IDirect3DSurface9* normalTarget = nullptr;
					bindings[0].pResource = normal;
					if (normalUpload && SUCCEEDED(normalUpload->GetSurfaceLevel(0, &normalTarget))) {
						draw.Targets[1].pResource = normalTarget;
						if (SUCCEEDED(DxvkCompute::Extension()->Draw(&draw)) && SUCCEEDED(share->CopyToShared(images[normalAtlasImage], x, y, normalUpload, &all))) normalSlots++;
						normalTarget->Release();
					}	// (no map in the slot: it reads 0, the shaders keep the triangle's normal)
				}
				if (slot >= 0) {
					if (reuseAt >= 0) { freeSlots.erase(freeSlots.begin() + reuseAt); slotsReused++; }	// (rt-audit F3)
					else atlasUsed++;
					if (glow) glowSlots++;
					// (rt-audit F3/F5) the slot holds its diffuse texture until the game drops it (SweepSlots): the key's address stays this texture's
					if (slotTex.size() < capacity) { slotTex.resize(capacity); slotTableDirty.resize(capacity, 0); }
					texture->AddRef();
					slotTex[slot].diffuse = texture;
					slotTex[slot].key = key;
					slotHolds[texture]++;
				}
				if (slot >= 0 && realTextures && (UInt32)slot < slotReal.size()) {	// (2026-11-07, protocol 7)
					QueueSlot(slot, texture, normal, glow);
					slotReal[slot] = SlotReal();
					slotReal[slot].state = 1;
				}
				if (slot >= 0 && realNormals && (size_t)slot < slotNormals.size()) slotNormals[slot] = normal;	// (2026-11-07, RealNormalMaps)
			}
			target->Release();
		}
		if (target2) target2->Release();
		// UNOFFICIAL 2026-11-11 (sungi) / 2026-11-13 (rt-audit F3): a made slot's key is pinned through slotTex (its diffuse texture held until the
		// game drops it, then the slot is freed and the key erased: SweepSlots). A -1 (the texture could not be drawn) stays unpinned: those
		// entries are dropped when the map grows (a new texture at such an address gets its chance again).
		if (slot < 0 && alphaSlots.size() > 8192)
			for (auto it = alphaSlots.begin(); it != alphaSlots.end(); ) { if (it->second < 0) it = alphaSlots.erase(it); else ++it; }
		alphaSlots.emplace(key, slot);
		return slot;
	}

	// UNOFFICIAL 2026-11-13 (rt-audit F3, RTAudit\AUDIT.md) kSlotSweep slots an exchange: a slot whose diffuse texture only we still hold (our slot
	// reference + our real-texture id's reference, texKeyOf) is freed: its key erased, its reference given back (the texture's real id then goes
	// by the real-texture sweep: only texKeyOf holds it), reusable from kSlotReuse exchanges on. A queued want holds its own reference (QueueSlot),
	// so a slot waiting to send is never taken for dropped. Textures the game still holds (anything in a loaded cell) are never touched.
	// UNOFFICIAL 2026-11-13 (rt-audit F4) every 120 exchanges with an auto budget: this process's local video memory use (the game's DXVK) + the
	// helper's real textures and structures against 90 % of the process's budget. Over it: the cap drops to what is alive minus the excess
	// (at least 256 MB) and idle textures (unused kTexIdle exchanges, least recent first) are released now, up to 64 a call; textures in use
	// stay (new ones then wait: the slots keep the atlas). Under it with 512 MB to spare: the cap climbs back 256 MB a call to its start value.
	void Core::VramWatch(NvrRtFrame& f, UInt64 n) {
		if (!texAuto || !realTextures || (n % 120) != 0) return;
		unsigned long long budget = 0, usage = 0;
		if (!RtVideoMemory::Local(texLuid, texLuidValid, budget, usage)) return;
		const UInt64 helper = texRes.alive + (shared ? (UInt64)shared->geometryBytes : 0);
		const UInt64 total = usage + helper, limit = budget / 10 * 9, floorBytes = 256ull << 20;
		if (total > limit) {
			const UInt64 over = total - limit;
			const UInt64 cap = (std::max)(floorBytes, texRes.alive > over ? texRes.alive - over : 0ull);
			if (cap < texBudget) {
				texBudget = cap;
				if (vramShrinks++ < 20 || (vramShrinks % 100) == 0)
					Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: video memory at %llu of %llu MB (game %llu + helper %llu): auto budget down to %llu MB (rt-audit F4)",
						(unsigned long long)(total >> 20), budget >> 20, usage >> 20, (unsigned long long)(helper >> 20), (unsigned long long)(texBudget >> 20));
			}
			if (texRes.alive > texBudget) {
				std::vector<uint32_t> victims;
				texRes.PickVictims(0, texBudget, n, kTexIdle, victims);	// (false when even all idle ones would not do: then none; try the idle ones anyway)
				if (victims.empty()) {
					std::vector<uint32_t> some;
					if (texRes.alive > 0) texRes.PickVictims(0, texRes.alive - (std::min)(texRes.alive, (UInt64)(64ull << 20)), n, kTexIdle, some);
					victims.swap(some);
				}
				for (size_t k = 0; k < victims.size() && k < 64 && f.opCount < NVR_RT_MAX_OPS - 64; k++) ReleaseTexture(victims[k], f, n);
			}
		}
		else if (total + (512ull << 20) < limit && texBudget < texBudgetCap) texBudget = (std::min)(texBudgetCap, texBudget + (256ull << 20));
	}

	void Core::SweepSlots() {
		if (slotTex.empty()) return;
		const UInt32 total = (UInt32)slotTex.size();
		for (UInt32 k = 0; k < kSlotSweep && k < total; k++) {
			if (slotSweepAt >= total) slotSweepAt = 0;
			const UInt32 s = slotSweepAt++;
			SlotTex& st = slotTex[s];
			if (!st.diffuse) continue;
			auto holds = slotHolds.find(st.diffuse);
			UInt32 ours = holds != slotHolds.end() ? holds->second : 1u;
			auto id = texIds.find(st.diffuse);
			if (id != texIds.end() && id->second.id && id->second.id < texKeyOf.size() && texKeyOf[id->second.id] == st.diffuse) ours++;
			st.diffuse->AddRef();
			if (st.diffuse->Release() != ours) continue;
			alphaSlots.erase(st.key);	// first: the Release below may free the texture and its address
			if (holds != slotHolds.end() && --holds->second == 0) slotHolds.erase(holds);
			st.diffuse->Release();
			st = SlotTex();
			if (s < slotNormals.size()) slotNormals[s] = nullptr;
			if (s < slotReal.size()) slotReal[s] = SlotReal();	// (its ids stay with their textures: the real-texture sweep releases them)
			if (s < slotTableDirty.size()) slotTableDirty[s] = 1;
			freeSlots.push_back({ s, frame + kSlotReuse });
			slotsFreed++;
		}
		static UInt32 logged = 0;
		if (slotsFreed >= logged + 100) {
			logged = slotsFreed;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT atlas: %u slots freed so far (the game dropped their textures), %u reused, %zu free; %u of %u ever drawn (rt-audit F3)",
				slotsFreed, slotsReused, freeSlots.size(), atlasUsed, total);
		}
	}
	// (2026-11-07, protocol 7; protocol 8 rtgap-textures) a texture's levels into this exchange's half of its route's transfer image + a
	// TEXTURE op (bytes 0: on the GPU). Returns its id, 0 = not now (the slot keeps the atlas: not a 2D texture, a format left out, or the
	// budget full of textures in use: budgetRefused, tried again later), -1 = no room this exchange (the half or the op list: next one).
	// The game's textures are in D3D9's default pool (measured 2026-11-07: every one, DXT1 / DXT5), so there is no CPU copy to lock: the
	// fork's CopyToShared copies each level's surface into the transfer image of the texture's route (RtTexturePack::Choose): with fork
	// 1.8-rt-bc DXT1 / DXT5 / ATI2 go as their BC1 / BC3 / BC5 blocks (no decode: the helper's copy is BC too, a quarter to an eighth of
	// RGBA8), anything else (and everything with an older fork) is decoded by the fork's blit into the RGBA8 one. The helper copies the
	// rectangles into its own texture before this exchange's passes. Layout (NvrRtTextureOp::bytes): level 0 at (x, y), the smaller levels
	// in a column right of it. A transfer image's two halves alternate by exchange, so a half is only written again after the game's GPU
	// waited for the helper's frame that read it. Over RealTextureBudgetMB, textures unused for kTexIdle exchanges are released (least
	// recently used first) to make room; their slots fall back to the atlas and send them again when next used.
	int Core::SendTexture(IDirect3DBaseTexture9* base, NvrRtFrame& f, UInt64 n, bool& budgetRefused) {
		static UInt32 told = 0;
		budgetRefused = false;
		auto no = [&](const char* why, UInt32 a, UInt32 b) {
			if (told < 12) { told++; Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: a texture keeps the atlas: %s (%u, %u)", why, a, b); }
			return 0;
		};
		if (!base || base->GetType() != D3DRTYPE_TEXTURE) return no("not a 2D texture", base ? (UInt32)base->GetType() : 0u, 0u);
		IDirect3DTexture9* t = (IDirect3DTexture9*)base;
		D3DSURFACE_DESC d = {};
		if (FAILED(t->GetLevelDesc(0, &d))) return no("no level description", 0u, 0u);
		RtTexturePack::Plan p;
		const char* why = nullptr;
		if (!RtTexturePack::Choose((UInt32)d.Format, d.Width, d.Height, t->GetLevelCount(), texRoute, texCapBc, texCapRgba, p, why)) return no(why, (UInt32)d.Format, d.Width);
		const int image = texImages[p.route];
		if (image < 0) return no("no transfer image for its route", (UInt32)p.route, (UInt32)d.Format);
		// room in the budget: textures unused for a while released (their RELEASE ops first in the list), or refused for now
		std::vector<uint32_t> victims;
		if (!texRes.PickVictims(p.bytes, texBudget, n, kTexIdle, victims)) {
			budgetRefused = true;
			static UInt32 toldBudget = 0;
			if (toldBudget < 4) { toldBudget++; Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: RealTextureBudgetMB full of textures in use (%llu MB, %u textures): a %u KB one keeps the atlas for now",
				(unsigned long long)(texRes.alive >> 20), texRes.count, (UInt32)(p.bytes >> 10)); }
			return 0;
		}
		if (f.opCount + victims.size() + 2 > NVR_RT_MAX_OPS) return -1;
		uint32_t x = 0, y = 0;
		if (!texShelf[p.route].Place(p.fw, p.fh, texW[p.route], texHalfH[p.route], x, y)) return -1;
		y += (UInt32)(n & 1) * texHalfH[p.route];
		for (UInt32 l = 0; l < p.levels; l++) {
			IDirect3DSurface9* level = nullptr;
			if (FAILED(t->GetSurfaceLevel(p.first + l, &level)) || !level) return no("no surface for level", p.first + l, (UInt32)d.Format);
			uint32_t lx = 0, ly = 0;
			RtTexturePack::LevelAt(p, l, lx, ly);
			const HRESULT hr = share->CopyToShared(images[image], x + lx, y + ly, level, nullptr);
			level->Release();
			if (FAILED(hr)) return no("CopyToShared failed, format / level", (UInt32)d.Format, p.first + l);
		}
		for (uint32_t victim : victims) ReleaseTexture((UInt32)victim, f, n);
		const UInt32 id = texRes.Alloc(n);
		if (!id) { budgetRefused = true; return no("no free texture id (4095 alive)", texRes.count, 0u); }
		NvrRtTextureOp op = { NVR_RT_OP_TEXTURE, id, p.w0, p.h0, x | (y << 16), RtTexturePack::VkFormatOf(p.route), 0u,
			p.levels | (p.swapRG ? NVR_RT_TEX_SWIZZLE_GR : 0u) };
		memcpy(&f.ops[f.opCount++], &op, sizeof(op));
		texRes.Add(id, p.bytes, n);
		// UNOFFICIAL 2026-11-11 (sungi, ABA guard F2): one reference while the id lives (given back in ReleaseTexture / ReleaseTexRefs)
		if (id < texKeyOf.size()) {
			base->AddRef();
			if (texKeyOf[id]) texKeyOf[id]->Release();	// (not expected: the id was freed before; keeps the count balanced)
			texKeyOf[id] = base;
		}
		texSent++;
		texRouteSent[p.route]++;
		return (int)id;
	}

	// (2026-11-07, protocol 8) a texture leaves the helper (RealTextureBudgetMB full, it was unused for kTexIdle exchanges): its RELEASE op,
	// its id free again after kTexReuse exchanges, the slots that named it short (they fall back to the atlas until used and sent again)
	void Core::ReleaseTexture(UInt32 id, NvrRtFrame& f, UInt64 n) {
		if (!texRes.Live(id) || f.opCount >= NVR_RT_MAX_OPS) return;
		NvrRtTextureOp op = { NVR_RT_OP_TEXTURE_RELEASE, id, 0u, 0u, 0u, 0u, 0u, 0u };
		memcpy(&f.ops[f.opCount++], &op, sizeof(op));
		texRes.Free(id, n, kTexReuse);
		if (id < texKeyOf.size()) {
			auto it = texIds.find(texKeyOf[id]);
			if (it != texIds.end() && it->second.id == id) texIds.erase(it);
			// UNOFFICIAL 2026-11-11 (sungi, ABA guard F2): the key is erased first (the Release may free the texture and its address)
			if (texKeyOf[id]) texKeyOf[id]->Release();
			texKeyOf[id] = nullptr;
		}
		for (SlotReal& s : slotReal)
			for (UInt32 k = 0; k < 3; k++)
				if (s.ids[k] == id) { s.ids[k] = 0; s.state = 3; s.retryAt = n; }
		texReleased++;
	}

	// (2026-11-07, protocol 8) an atlas slot met again (AlphaSlotOf's cache): its textures' last use, and a short slot queued again
	void Core::NoteSlotUse(int slot, IDirect3DBaseTexture9* texture, IDirect3DBaseTexture9* normal, IDirect3DBaseTexture9* glow) {
		if ((UInt32)slot >= slotReal.size()) return;
		SlotReal& s = slotReal[slot];
		for (UInt32 k = 0; k < 3; k++) if (s.ids[k]) texRes.Use(s.ids[k], frame);
		if (s.state == 3 && frame >= s.retryAt) {
			s.state = 1;
			QueueSlot(slot, texture, normal, glow);
		}
	}

	// (2026-11-07, protocol 7) the slots made (or short) so far: each one's textures sent (as many as fit this exchange), then its SLOT_TEXTURES op
	void Core::PumpTextures(NvrRtFrame& f, UInt64 n) {
		VramWatch(f, n);	// UNOFFICIAL 2026-11-13 (rt-audit F4) the auto budget against the process's video memory
		// UNOFFICIAL 2026-11-13 (rt-tex-release; user log 2026-11-13 08:58: alive 1398 -> 3688 MB of 4096 after Mojave -> Cordon -> Mojave, "0 released",
		// VRAM 95 %, 1 % lows 1-9 fps) the textures the GAME has dropped leave the helper too. Each live id holds one COM reference (texKeyOf, the
		// ABA guard): it kept the game's own copy alive after the game unloaded it, and below RealTextureBudgetMB nothing was ever released, so both
		// sides filled VRAM. Now that reference is the sensor: a sweep of kTexSweep ids an exchange (all 4095 in ~16 exchanges) asks each texture's
		// count (AddRef / Release); 1 = only ours: the game no longer uses it (no cell, no draw holds it), so its id is released now (RELEASE op;
		// the id is reused only kTexReuse exchanges later, the frames in flight; our Release frees the game's texture, DXVK keeps the image
		// until the GPU is done with it). Textures the game still holds (everything in view) are never touched: no pops. Pinned (displacement)
		// ids stay. A queued want holds its own reference (QueueSlot), so a texture waiting to be sent is not taken for dropped.
		if (realTextures && texKeyOf.size() > 1) {
			const UInt32 total = (UInt32)texKeyOf.size();
			for (UInt32 k = 0; k < kTexSweep && f.opCount < NVR_RT_MAX_OPS - 64; k++) {
				if (texSweepAt == 0 || texSweepAt >= total) texSweepAt = 1;
				const UInt32 id = texSweepAt++;
				IDirect3DBaseTexture9* t = texKeyOf[id];
				if (!t || !texRes.Live(id) || texRes.Pinned(id)) continue;
				t->AddRef();
				if (t->Release() == 1) { ReleaseTexture(id, f, n); texDropped++; }
			}
			static UInt32 droppedLogged = 0;
			if (texDropped >= droppedLogged + 100) {
				droppedLogged = texDropped;
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: %u released so far (%u dropped by the game: unloaded cells); %u alive = %llu MB of %llu",
					texReleased, texDropped, texRes.count, (unsigned long long)(texRes.alive >> 20), (unsigned long long)(texBudget >> 20));
			}
		}
		if (!realTextures || (slotQueue.empty() && looseTextures.empty())) return;
		LARGE_INTEGER t0, t1, fq;
		QueryPerformanceCounter(&t0);
		for (RtTexturePack::Shelf& s : texShelf) s = RtTexturePack::Shelf();
		size_t done = 0;
		for (; done < slotQueue.size(); done++) {
			SlotWant& want = slotQueue[done];
			bool ready = true, shortOf = false;
			UInt32 ids[3] = {};
			for (int k = 0; k < 3 && ready; k++) {
				if (!want.tex[k]) continue;
				TexEntry& e = texIds[want.tex[k]];
				const bool due = !e.tried || (!e.id && e.retryAt && n >= e.retryAt);
				// (2026-11-07) RealNormalMaps: a normal map only while the textures alive take under 3/4 of the budget (the colour matters
				// more); held like a budget refusal: tried again kTexRetry exchanges later (releases may have made room)
				if (due && k == 1 && normalAtlasImage < 0 && texRes.alive >= texBudget / 4 * 3) {
					e.tried = true; e.id = 0; e.retryAt = n + kTexRetry;
					if (++normalsHeld == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real normal maps: the textures alive reached 3/4 of RealTextureBudgetMB (%llu MB): "
						"normal maps wait (those surfaces shade flat in RT hits until room is made; colour textures keep the rest)", (unsigned long long)(texRes.alive >> 20));
				}
				else if (due) {
					bool budget = false;
					const int r = SendTexture(want.tex[k], f, n, budget);
					if (r < 0) { ready = false; break; }
					e.tried = true;
					e.id = (UInt32)r;
					e.retryAt = !r && budget ? n + kTexRetry : 0;
					if (!r) texFailed++;
				}
				ids[k] = e.id;
				if (e.id) texRes.Use(e.id, n);	// (in use from now: never a victim of this slot's next texture)
				shortOf = shortOf || (!e.id && e.retryAt);
			}
			if (!ready || f.opCount >= NVR_RT_MAX_OPS) break;
			SlotReal* s = (UInt32)want.slot < slotReal.size() ? &slotReal[want.slot] : nullptr;
			const bool dirty = (UInt32)want.slot < slotTableDirty.size() && slotTableDirty[want.slot];	// (rt-audit F3) a reused slot: its old row goes
			const bool named = s && (s->ids[0] || s->ids[1] || s->ids[2] || dirty);
			if (ids[0] || ids[1] || ids[2] || named) {	// (also when it named textures before: the table entry follows)
				NvrRtTextureOp op = { NVR_RT_OP_SLOT_TEXTURES, (UInt32)want.slot, ids[0], ids[1], ids[2], 0, 0, 0 };
				memcpy(&f.ops[f.opCount++], &op, sizeof(op));
				texSlotsSent++;
				if (dirty) slotTableDirty[want.slot] = 0;
			}
			if (s) {
				for (UInt32 k = 0; k < 3; k++) { s->ids[k] = ids[k]; if (ids[k]) texRes.Use(ids[k], n); }
				s->state = shortOf ? 3 : 2;
				s->retryAt = shortOf ? n + kTexRetry : 0;
			}
		}
		DropSlots(done);
		// (2026-11-07 rtgap-tessblas) the textures the displacement asked for (land layers 1-6, parallax height maps): ids only, no slot
		// (merged with rtgap-textures) a loose texture's id is pinned (RtTexturePack::Residency::Pin): never released while the session's
		// displaced copies may read it (the helper keeps a DISPLACE_SOURCE's ids); a budget refusal is asked again by TextureIdFor later
		if (slotQueue.empty()) {	// (every slot went: room left this frame)
			size_t loose = 0;
			for (; loose < looseTextures.size(); loose++) {
				TexEntry& e = texIds[looseTextures[loose]];
				if (e.tried && !(!e.id && e.retryAt && n >= e.retryAt)) continue;
				bool budget = false;
				const int r = SendTexture(looseTextures[loose], f, n, budget);
				if (r < 0) break;
				e.tried = true;
				e.id = (UInt32)r;
				e.retryAt = !r && budget ? n + kTexRetry : 0;
				if (r > 0) texRes.Pin((uint32_t)r);
				if (!r) texFailed++;
			}
			DropLoose(loose);
		}
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&fq);
		texCpuMs += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(fq.QuadPart);
		static UInt32 lastLogged = 0;
		if (texSent + texFailed >= lastLogged + 50 || (lastLogged == 0 && texSent + texFailed > 0)) {
			lastLogged = texSent + texFailed;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: %u sent (BC1 %u, BC3 %u, BC5 %u, RGBA8 %u), %u alive = %llu MB of %llu, %u released (%u dropped by the game), "
				"%u kept the atlas, %u slots pointed, %zu slots waiting, %.1f ms CPU so far", texSent, texRouteSent[RtTexturePack::Bc1], texRouteSent[RtTexturePack::Bc3],
				texRouteSent[RtTexturePack::Bc5], texRouteSent[RtTexturePack::Rgba8], texRes.count, (unsigned long long)(texRes.alive >> 20),
				(unsigned long long)(texBudget >> 20), texReleased, texDropped, texFailed, texSlotsSent, slotQueue.size(), texCpuMs);
		}
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
		// (2026-11-07 rt-shutdown) once the helper process is gone the shared semaphore can read 2^64 - 1 (RT host shutdown scene, AMD iGPU):
		// every wait is released then, and max(done, recorded) + margin would wrap to a value below the current one
		if (done < recorded + kReleaseMargin) c->outputReady->SignalFromHost((std::max)(done, recorded) + kReleaseMargin);
		InterlockedExchange(&c->tripped, 1);
		return 0;
	}

	// UNOFFICIAL 2026-11-07 (perf-ptcost) the STALL lines' RT state (SessionLog.h StallWatch::SetExtra; runs on the watchdog thread, its reads
	// guarded there: plain counters and the shared block's two frame numbers)
	void StallText(char* out, unsigned size) {
		const Core& c = TheCore;
		const NvrRtShared* s = c.shared;
		sprintf_s(out, size, "RT core: state %d, exchanges recorded %llu, the helper has read up to %lld (latest published %lld)%s", (int)c.state,
			(unsigned long long)c.frame, s ? (long long)s->consumedFrame : -1ll, s ? (long long)s->latestFrame : -1ll, c.recording ? ", recording an exchange now" : "");
	}

	bool Core::Start() {
		SessionLog::StageScope stage("RT core: starting the RT helper");	// (2026-11-07 perf-ptcost: STALL lines)
		SessionLog::StallWatch::SetExtra(StallText);
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
		{	// UNOFFICIAL 2026-11-10 BlendedHair (a missing key, an older toml: on)
			SettingManager::Configuration::ConfigNode node;
			blendedHairOn = !TheSettingManager->Config.FillNode(&node, "Main.Main.RayTracing", "BlendedHair") || node.BoolValue;
		}
		hitTextures = TheSettingManager->GetSettingI("Main.Main.RayTracing", "HitTextures") != 0;
		// 2026-10-30; (2026-11-07, RT-DETAIL-GAPS.md 6) PTRealtime turns it on too: read here and again each frame (Gather), so the menu's
		// PTRealtime switch brings the grass without a restart
		grassSetting = TheSettingManager->GetSettingI("Main.Main.RayTracing", "Grass") != 0;
		grassOn = alphaOn && (grassSetting || TheSettingManager->GetSettingI("Main.Main.RayTracing", "PTRealtime") != 0);
		if (!alphaOn && grassSetting)
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
		ReleaseTexRefs();	// UNOFFICIAL 2026-11-11 (sungi, ABA guard F2): the references of the keys cleared here and below
		alphaSlots.clear();
		// (2026-11-07, protocol 7) real textures: read at each helper start (restart), with the atlases they follow
		realTextures = (alphaOn || hitTextures) && TheSettingManager->GetSettingI("Main.Main.RayTracing", "RealTextures") != 0;
		texIds.clear(); ClearTextureQueues(); dispSentOn = false; texSent = texFailed = texSlotsSent = texReleased = 0; texCpuMs = 0.0;
		// (2026-11-07, RT-DETAIL-GAPS.md section 2) the real normal maps, on by default with RealTextures (restart)
		realNormals = realTextures && TheSettingManager->GetSettingI("Main.Main.RayTracing", "RealNormalMaps") != 0;
		slotNormals.assign(realNormals ? NVR_RT_ALPHA_ATLAS_SLOTS * NVR_RT_ALPHA_ATLAS_SLOTS : 0, nullptr);
		normalsHeld = 0;
		if (realTextures) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: normal maps at ray hits %s (RealNormalMaps)", realNormals ? "on" : "off");
		for (UInt32 r = 0; r < RtTexturePack::RouteCount; r++) { texRoute[r] = false; texImages[r] = -1; texW[r] = texHalfH[r] = 0; texRouteSent[r] = 0; }
		texRes.Reset(NVR_RT_MAX_TEXTURES);
		texKeyOf.assign(NVR_RT_MAX_TEXTURES, nullptr);
		slotReal.assign(NVR_RT_ALPHA_ATLAS_SLOTS * NVR_RT_ALPHA_ATLAS_SLOTS, SlotReal());
		if (realTextures) {
			// (2026-11-07, rtgap-textures) the caps: 0 (the default) = auto by the GPU's video memory, logged once; explicit values as before
			const int maxSetting = TheSettingManager->GetSettingI("Main.Main.RayTracing", "RealTextureMaxSize");
			const int budgetSetting = TheSettingManager->GetSettingI("Main.Main.RayTracing", "RealTextureBudgetMB");
			char memText[256] = "not asked";
			RtTexturePack::Caps caps = { 1024u, 1536u, "" };
			if (maxSetting <= 0 || budgetSetting <= 0)
				caps = RtTexturePack::AutoCaps(RtVideoMemory::Dedicated(info.DeviceLUID, info.DeviceLUIDValid != FALSE, info.VendorId, info.DeviceId, memText, sizeof(memText)));
			texMaxSize = maxSetting > 0 ? RtTexturePack::ExplicitMaxSize(maxSetting) : caps.maxSize;
			texBudget = (UInt64)(budgetSetting > 0 ? (std::max)(64, budgetSetting) : (int)caps.budgetMB) << 20;
			// UNOFFICIAL 2026-11-13 (rt-audit F4) auto: also at most 20 % of THIS process's WDDM local budget (the user's 16 GB card gave the game
			// 12.1 GB; the tier's 4096 MB of copies plus a modded game's own textures went to 95 % and the helper's builds slowed 50-100x), and
			// watched live (VramWatch): over 90 % of the budget the cap drops to what is alive minus the excess, back up when there is room again
			texAuto = budgetSetting <= 0;
			memcpy(texLuid, info.DeviceLUID, 8); texLuidValid = info.DeviceLUIDValid != FALSE;
			texBudgetCap = texBudget;
			{
				unsigned long long vb = 0, vu = 0;
				if (texAuto && RtVideoMemory::Local(texLuid, texLuidValid, vb, vu)) {
					texBudgetCap = (std::min)(texBudgetCap, (UInt64)(vb / 5) & ~((1ull << 20) - 1));
					texBudget = texBudgetCap;
					Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: auto budget %llu MB = min(tier, 20 %% of the process's local video memory budget %llu MB; %llu MB in use now) (rt-audit F4)",
						(unsigned long long)(texBudget >> 20), vb >> 20, vu >> 20);
				}
			}
			// (protocol 8) the BC routes when the fork makes BC shared images (1.8-rt-bc; an older one refuses the format) and
			// RealTextureCompressed is on (off: everything through RGBA8, the 1.0.6-test7 route, for an A/B)
			bool bc = false;
			if (TheSettingManager->GetSettingI("Main.Main.RayTracing", "RealTextureCompressed") != 0) {
				NVR_EXT_SHARED_IMAGE_DESC probe = { 256, 256, NVR_EXT_SHARED_BC1, "NVR RT BC probe" };
				ID3D9NvrExtSharedImage* probeImage = nullptr;
				if (SUCCEEDED(share->CreateSharedImage(&probe, &probeImage)) && probeImage) { bc = true; probeImage->Release(); }
			}
			texCapBc = texMaxSize;
			texCapRgba = (std::min)((uint32_t)texMaxSize, RtTexturePack::RgbaCapLimit(bc));
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: RealTextureMaxSize %u%s, RealTextureBudgetMB %llu%s (video memory: %s%s%s); routes: %s",
				texMaxSize, maxSetting > 0 ? "" : " (auto)", (unsigned long long)(texBudget >> 20), budgetSetting > 0 ? "" : " (auto)", memText,
				caps.tier[0] ? ", tier " : "", caps.tier, bc ? "BC1 / BC3 / BC5 as the game keeps them (fork 1.8-rt-bc), other formats decoded to RGBA8" :
				"everything decoded to RGBA8 (this d3d9.dll has no BC shared images: fork patch 0035, 1.8-rt-bc; or RealTextureCompressed off)");
			if (!bc && texMaxSize > texCapRgba) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT real textures: RealTextureMaxSize %u needs the BC routes (fork 1.8-rt-bc): %u sent",
				texMaxSize, texCapRgba);
			for (UInt32 r = 0; r < RtTexturePack::RouteCount; r++) texRoute[r] = r == RtTexturePack::Rgba8 || bc;
		}
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
		scratchMB = 0;
		// (2026-11-07, protocol 7; protocol 8) the real textures' transfer images, one a route (RtTexturePack::TransferSize: at the default
		// 1024 RGBA8 4096 x 2048 = 32 MB, BC1 4 MB, BC3 / BC5 8 MB each)
		for (UInt32 r = 0; r < RtTexturePack::RouteCount && realTextures; r++) {
			if (!texRoute[r]) continue;
			static const char* names[RtTexturePack::RouteCount] = { "NVR RT texture transfer", "NVR RT texture transfer BC1", "NVR RT texture transfer BC3", "NVR RT texture transfer BC5" };
			static const NVR_EXT_SHARED_FORMAT formats[RtTexturePack::RouteCount] = { NVR_EXT_SHARED_RGBA8, NVR_EXT_SHARED_BC1, NVR_EXT_SHARED_BC3, NVR_EXT_SHARED_BC5 };
			RtTexturePack::TransferSize(r == RtTexturePack::Rgba8 ? texCapRgba : texCapBc, texW[r], texHalfH[r]);
			texImages[r] = (int)imageDescs.size();
			imageNames.push_back(names[r]);
			imageDescs.push_back({ texW[r], 2 * texHalfH[r], formats[r], nullptr });
			imageSizes.push_back({ texW[r], 2 * texHalfH[r] });
		}
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
		shared->scratchMB = scratchMB;	// (2026-11-07, protocol 6)
		shared->textureImage = 0;	// (2026-11-07, protocol 7; protocol 8: a byte a route) the real textures' transfer images
		for (UInt32 r = 0; r < RtTexturePack::RouteCount; r++)
			if (texImages[r] >= 0) shared->textureImage |= ((uint32_t)texImages[r] + 1u) << (8u * r);
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
		{	// UNOFFICIAL 2026-11-10 [Main.Main.RayTracing] HelperWait (read at each helper start; a missing key: 0): 1 = the helper waits for the
			// game's inputs on the CPU and submits with no GPU wait (NvrRtHelper.cpp --wait cpu), 0 = its submission waits on the GPU (as before)
			SettingManager::Configuration::ConfigNode node;
			bool cpuWait = TheSettingManager->Config.FillNode(&node, "Main.Main.RayTracing", "HelperWait") && node.IntValue != 0;
			if (const char* e = getenv("NVR_RT_HELPER_WAIT")) cpuWait = !strcmp(e, "cpu");	// (bench A/B without touching the settings file: cpu / gpu)
			cmdLine += cpuWait ? L" --wait cpu" : L" --wait gpu";
			if (const char* e = getenv("NVR_RT_HELPER_PRIORITY")) if (!strcmp(e, "high")) cmdLine += L" --gpu-priority high";	// (bench only, 2026-11-10)
			if (getenv("NVR_RT_HELPER_MEASURE")) cmdLine += L" --measure";
			if (const char* e = getenv("NVR_RT_HELPER_SIGNAL")) if (!strcmp(e, "cpu")) cmdLine += L" --signal cpu";	// (bench only, 2026-11-10; with --wait cpu)
			if (getenv("NVR_RT_HELPER_TIMER")) cmdLine += L" --timer-1ms";	// (bench only, 2026-11-10: the helper process's timer resolution 1 ms)
			if (getenv("NVR_GAME_TIMER")) {	// (bench only: the game process's too; the DXVK submission thread's and the driver's waits tick on it)
				typedef UINT(WINAPI* TimeBegin)(UINT);
				const HMODULE winmm = LoadLibraryW(L"winmm.dll");
				const TimeBegin begin = winmm ? (TimeBegin)GetProcAddress(winmm, "timeBeginPeriod") : nullptr;
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: NVR_GAME_TIMER: the game process's timer resolution 1 ms: %s", begin && begin(1) == 0 ? "set" : "not set");
			}	// (bench only: the helper's hand-over timing lines, needs --wait cpu)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper start option HelperWait %d (%s)", cpuWait ? 1 : 0, cpuWait ? "the helper waits on the CPU" : "the GPU waits");
		}
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
		startNotes = 0;
		state = Starting;
		lastSerial = 0;
		geometries.clear();
		FreeLeafCards(true);
		FreeFaces(true);
		pendingReleases.clear();
		nextId = 1;
		_snprintf_s(status, sizeof(status), _TRUNCATE, "starting on %s", info.DeviceName);
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper started (%ls): %u shared images (arena %ux%u), %u shaders, GPU %s.", exe.c_str(),
			(unsigned)images.size(), imageDescs[0].Width, imageDescs[0].Height, (unsigned)shaders.size(), info.DeviceName);
		return true;
	}

	void Core::Stop(const char* why) {
		SessionLog::StageScope stage("RT core: stopping the RT helper");	// (2026-11-07 perf-ptcost: STALL lines)
		if (why && state != Idle && state != Off) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper stopped: %s.", why);
		// UNOFFICIAL 2026-11-10 (PT early kick) an open SubmitEarly is dropped, not collected: step 1 below waits for every recorded frame
		// (lastRecorded includes it) and the shared images go, so its copies out would read nothing. The user keeps last frame's picture.
		pendingOut.clear();
		pendingN = 0;
		earlyName = nullptr;
		submitOnly = false;
		if (watchdog) {
			InterlockedExchange(&watchStop, 1);
			WaitForSingleObject(watchdog, 3000);
			CloseHandle(watchdog);
			watchdog = nullptr;
		}
		// (2026-11-07 rt-shutdown) THE SHUTDOWN, in order (as the RT playground's PgHost::Stop). The old one only waited for the helper to
		// SUBMIT the last frame, asked it to quit and dropped everything: DXVK's queue could still hold a wait for an "output ready" value
		// the helper never signalled (it left on quit before running the last frames), and on the AMD iGPU the driver then reset the display
		// (System event 4101). Now, with nothing new recorded (the caller stops exchanging):
		//   1. the helper, still running, finishes every recorded frame: a CPU wait until outputReady >= recorded. A helper gone or hung
		//      (no progress within the time) is terminated (logged), then SignalFromHost releases our queue's waits;
		//   2. DXVK drained: SignalShared(inputsReady, recorded + 1) submits everything recorded so far, polled on the CPU until done (the
		//      helper never waits for that value; a D3D9 EVENT query never reported done under the fork, playground 2026-11-07);
		//   3. quit: the helper releases anything left (nothing, normally), waits for its GPU and exits; TerminateProcess only when hung;
		//   4. the shared objects dropped (below).
		// Bounded: ~5 s in all (the game's exit calls this: RtCore::Shutdown), each step at least 250 ms.
		const UInt64 recorded = (UInt64)InterlockedCompareExchange64(&lastRecorded, 0, 0);
		const ULONGLONG t0 = GetTickCount64();
		const DWORD budget = 5000;
		auto within = [&](DWORD cap) -> DWORD {
			const ULONGLONG used = GetTickCount64() - t0;
			const DWORD rest = used >= budget ? 0 : DWORD(budget - used);
			return (std::min)(cap, (std::max)(rest, (DWORD)250));
		};
		auto alive = [&]() { return process && WaitForSingleObject(process, 0) != WAIT_OBJECT_0; };
		bool forced = false;
		const bool hadHelper = process != nullptr;
		UINT64 done = 0;
		if (outputReady && recorded) {	// 1.
			const DWORD cap = within(3000);
			const ULONGLONG s = GetTickCount64();
			while (SUCCEEDED(outputReady->GetCompletedValue(&done)) && done < recorded && GetTickCount64() - s < cap && alive()) Sleep(1);
			outputReady->GetCompletedValue(&done);
			if (done < recorded) {
				if (alive()) {
					Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: RT helper shutdown: the helper did not finish frame %llu within %lu ms (output ready at %llu): "
						"hung, terminated.", (unsigned long long)recorded, cap, (unsigned long long)done);
					TerminateProcess(process, 1);
					WaitForSingleObject(process, 1000);
				}
				else Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper shutdown: the helper is gone with output ready at %llu of %llu: released from here.",
					(unsigned long long)done, (unsigned long long)recorded);
				outputReady->SignalFromHost(recorded + kReleaseMargin);
				forced = true;
			}
		}
		if (share && inputsReady && recorded) {	// 2.
			const UInt64 target = recorded + 1;
			const DWORD cap = within(2000);
			UINT64 v = 0;
			if (FAILED(share->SignalShared(inputsReady, target))) { Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper shutdown: SignalShared for the drain failed."); forced = true; }
			else {
				const ULONGLONG s = GetTickCount64();
				while (SUCCEEDED(inputsReady->GetCompletedValue(&v)) && v < target && GetTickCount64() - s < cap) Sleep(1);
				inputsReady->GetCompletedValue(&v);
				if (v < target) {
					Logger::Log("[ERROR] UNOFFICIAL Vegas Shaders DXVK: RT helper shutdown: the game's GPU queue did not drain within %lu ms (fence at %llu of %llu).",
						cap, (unsigned long long)v, (unsigned long long)target);
					forced = true;
				}
			}
		}
		if (process) {	// 3.
			if (shared && alive()) {
				const bool starting = state == Starting;
				shared->quit = 1;
				MemoryBarrier();
				SetEvent(frameEvent);
				const DWORD cap = within(3000);
				if (WaitForSingleObject(process, cap) != WAIT_OBJECT_0) {
					Logger::Log("%sUNOFFICIAL Vegas Shaders DXVK: RT helper shutdown: the helper did not exit within %lu ms%s: terminated.", starting ? "" : "[ERROR] ", cap,
						starting ? " (it was still starting: it reads quit only once ready)" : " (hung)");
					TerminateProcess(process, 1);
					WaitForSingleObject(process, 1000);
					if (!starting) forced = true;
				}
			}
			CloseHandle(process);
			process = nullptr;
		}
		LogHelperMessage();	// the helper's last status line ("exit: output ready ... clean")
		if (outputReady) {	// (the helper is gone now: nothing else signals it)
			outputReady->GetCompletedValue(&done);
			if (done < recorded) { outputReady->SignalFromHost(recorded + kReleaseMargin); forced = true; }
		}
		if (hadHelper)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper shutdown %s in %llu ms (%llu frames recorded, output ready at %llu).", forced ? "FORCED (see above)" : "clean",
				(unsigned long long)(GetTickCount64() - t0), (unsigned long long)recorded, (unsigned long long)done);
		for (ID3D9NvrExtSharedImage*& image : images) DxvkCompute::Drop(image);
		images.clear();
		DxvkCompute::Drop(inputsReady);
		DxvkCompute::Drop(outputReady);
		DxvkCompute::Drop(rt);
		DxvkCompute::Drop(rtSkin);
		DxvkCompute::Drop(rtSeq);
		DxvkCompute::Drop(share);
		if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
		if (texStaging) { UnmapViewOfFile(texStaging); texStaging = nullptr; }	// (2026-11-07, protocol 7)
		if (texMapping) { CloseHandle(texMapping); texMapping = nullptr; }
		texIds.clear(); ClearTextureQueues(); dispSentOn = false;
		HANDLE* handles[] = { &mapping, &frameEvent, &statusEvent };
		for (HANDLE* h : handles) if (*h) { CloseHandle(*h); *h = nullptr; }
		geometries.clear();
		FreeLeafCards(true);
		FreeFaces(true);
		pendingReleases.clear();
		ReleaseTexRefs();	// UNOFFICIAL 2026-11-11 (sungi, ABA guard F2): Stop may run at shutdown; releasing a held reference is valid there
		alphaSlots.clear();
		if (alphaUpload) { alphaUpload->Release(); alphaUpload = nullptr; }
		if (alphaDetailUpload) { alphaDetailUpload->Release(); alphaDetailUpload = nullptr; }
		if (albedoUpload) { albedoUpload->Release(); albedoUpload = nullptr; }
		if (albedoDetailUpload) { albedoDetailUpload->Release(); albedoDetailUpload = nullptr; }
		if (albedoDetailScratch) { albedoDetailScratch->Release(); albedoDetailScratch = nullptr; }
		if (glowUpload) { glowUpload->Release(); glowUpload = nullptr; }
		if (normalUpload) { normalUpload->Release(); normalUpload = nullptr; }	// UNOFFICIAL 2026-11-13 (rt-audit F11): leaked at every helper restart
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
			// (2026-11-08 perf-reflfix4, the lead's bench run of test 12 on the iGPU: ~22 trace shaders now, RtAOTrace alone 29 s; ready
			// after 55.7 s, and the run before had lost RT for the whole session at 60 s) 60 -> 300 s, and a line every 30 s while it waits
			else {
				const ULONGLONG waited = GetTickCount64() - startTick;
				if (waited > kHelperReadyMs) TurnOff("the helper did not get ready within 300 seconds (it compiles its trace shaders first)");
				else if (waited / 30000 > startNotes) {
					startNotes = (UInt32)(waited / 30000);
					Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT helper still getting ready after %llu s (it compiles its %u trace shaders before the first "
						"frame; a first launch on a slow GPU takes a minute or more; the game runs without RT meanwhile; up to %u s) (perf-reflfix4)",
						(unsigned long long)(waited / 1000), (unsigned)shared->shaderCount, (unsigned)(kHelperReadyMs / 1000));
				}
			}
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
			{	// (2026-11-07 rtgap-tessblas) the normal and the land's layer weights, for the displaced copies
				const int fmt = e.Type == D3DDECLTYPE_FLOAT3 ? (int)NVR_EXT_RT_POSITION_FLOAT3 : e.Type == D3DDECLTYPE_FLOAT4 ? (int)NVR_EXT_RT_POSITION_FLOAT4 :
					e.Type == D3DDECLTYPE_FLOAT16_4 ? (int)NVR_EXT_RT_POSITION_HALF4 : -1;
				if (e.Usage == D3DDECLUSAGE_NORMAL && e.UsageIndex == 0) { out.normalStream = e.Stream; out.normalOffset = e.Offset; out.normalFormat = fmt; out.normalType = e.Type + 1u; }
				if (e.Usage == D3DDECLUSAGE_TEXCOORD && (e.UsageIndex == 1 || e.UsageIndex == 2)) {
					const int k = e.UsageIndex - 1;
					out.layerStream[k] = e.Stream; out.layerOffset[k] = e.Offset; out.layerType[k] = e.Type + 1u;
					out.layerFormat[k] = fmt == (int)NVR_EXT_RT_POSITION_FLOAT3 ? -1 : fmt;	// four weights each: FLOAT4 / HALF4 only
				}
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

	// (2026-11-07) where a buffer data's positions are (FVF or declaration, cached), its stream's VB there; lastElement untouched
	bool Core::PositionIn(NiGeometryBufferData* data, PositionElement& position) {
		if (!data || !data->VertCount || !data->VBChip || !data->VertexStride || !data->StreamCount) return false;
		position = PositionElement();
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
		return position.stream >= 0 && (UInt32)position.stream < data->StreamCount && data->VBChip[position.stream] && data->VBChip[position.stream]->VB;
	}

	bool Core::DescribeData(NiGeometryBufferData* data, const char* name, NVR_EXT_RT_GEOMETRY_DESC& desc, std::vector<NVR_EXT_RT_RANGE>& ranges, GeometryKey& key) {
		if (!data || !data->VertCount || !data->IB || !data->VBChip || !data->VertexStride || !data->StreamCount) return false;
		PositionElement position;
		if (!PositionIn(data, position)) return false;

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

	// UNOFFICIAL 2026-11-08 (perf-reflfix4, test 12: the reflected hair white in first person) a part's hair tint: what the game puts in the
	// hair shader's c2 is the part's own shader property at +0x84 (RtHairTint.h: Lighting30Shader's constant 2 for the hair pass, written
	// by ApplyFaceGenTextures from TESNPC::iHairColor when the head is built), for the player and NPCs, first and third person, drawn or not.
	// rgb: the tint (0..1, the stored curve); the answer RtHairTint::Source (None: not hair, or no colour known). skinned: which path met it
	// (the vanilla hair, beards and brows are rigid: AddGeometry). alphaPart: it goes in as a cut-out (a TINTED record needs that). The first
	// 8 hair parts met are logged with everything read, so a tester's log settles it.
	int Core::HairTintOf(NiGeometry* geo, BSShaderProperty* shaderProperty, bool skinned, bool alphaPart, float rgb[3]) {
		if (!geo || !shaderProperty) return RtHairTint::None;
		const bool hairFlag = (shaderProperty->ulFlags[0] & BSShaderProperty::Hair) != 0;
		const bool named = RtHairTint::HairName(geo->m_pcName);
		if (!hairFlag && !named) return RtHairTint::None;
		const UInt32 shaderType = static_cast<NiShadeProperty*>((NiProperty*)shaderProperty)->m_eShaderType;
		const float* at84 = RtHairTint::TintClass(shaderType) ? (const float*)((const UInt8*)shaderProperty + RtHairTint::kPropertyTintOffset) : nullptr;
		const bool playerHair = walkingPlayer && Player && Player->baseForm && Player->baseForm->formType == TESForm::kFormType_NPC;
		const UInt32 packed = playerHair ? *(const UInt32*)((const UInt8*)Player->baseForm + 0x1D8) : 0u;	// TESNPC::iHairColor (decomp: offsetof asserts)
		const int how = alphaPart ? RtHairTint::For(geo, at84, true, playerHair, packed, rgb) : RtHairTint::None;
		hairParts[skinned ? 1 : 0][how]++;
		bool told = false;
		for (int k = 0; k < hairLogs && !told; k++) told = hairTold[k] == geo;
		if (!told && hairLogs < 8) {
			hairTold[hairLogs++] = geo;
			float noted[3] = { -1.0f, -1.0f, -1.0f };
			const bool hasNote = RtHairTint::Lookup(geo, noted);
			const NiAlphaProperty* ap = (const NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha);
			Logger::Log("RT CORE hair (perf-reflfix4): '%s' %s, %s (alpha %04X); its shader property vtable %08X type %u index %u flags %08X: Hair flag %d, "
				"FaceGen hair name %d, the player's %d (TESNPC colour %08X); at +0x84 %s%.3f %.3f %.3f %.3f; a draw's c2 %s%.3f %.3f %.3f; tint from %s = "
				"%.3f %.3f %.3f -> record albedo (linear) %.4f %.4f %.4f; hair-shader draws seen so far: %u still, %u skinned",
				geo->m_pcName ? geo->m_pcName : "", skinned ? "skinned (AddSkinned)" : "rigid (AddGeometry)", alphaPart ? "a cut-out" : "NOT a cut-out (no TINTED record)",
				ap ? (unsigned)ap->flags : 0u, *(const UInt32*)shaderProperty, shaderType, shaderProperty->uiShaderIndex, shaderProperty->ulFlags[0], hairFlag ? 1 : 0,
				named ? 1 : 0, playerHair ? 1 : 0, packed, at84 ? "" : "(not read: not shader type 8 / 12) ", at84 ? at84[0] : 0.0f, at84 ? at84[1] : 0.0f,
				at84 ? at84[2] : 0.0f, at84 ? at84[3] : 0.0f, hasNote ? "" : "(none) ", noted[0], noted[1], noted[2], RtHairTint::SourceName(how),
				how ? rgb[0] : 0.0f, how ? rgb[1] : 0.0f, how ? rgb[2] : 0.0f, how ? RtScenePick::TintLinear(rgb[0]) : 0.0f, how ? RtScenePick::TintLinear(rgb[1]) : 0.0f,
				how ? RtScenePick::TintLinear(rgb[2]) : 0.0f, RtHairTint::HairDraws[0], RtHairTint::HairDraws[1]);
		}
		return how;
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
		// Bodies are alpha-tested (the hair and fringe edges): they go in (rtsun-13: with them left out, only heads and hands were in the
		// scene); (2026-11-07 perf-mirrorfix) the alpha-tested parts that are not skin as cut-outs (RtScenePick::ActorAlphaTested, below).
		// Blended-only (glass, fx) stays out.
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
		// (2026-11-07 ptaaa-materials) skin for the path tracer (its scattering, PTSkin): the part's diffuse texture under textures\characters (RtScenePick::SkinTexturePath)
		bool skinKind = false;
		if (ppLighting) {
			NiSourceTexture** textures = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[0];
			const NiSourceTexture* source = textures ? *textures : nullptr;
			skinKind = source && RtScenePick::SkinTexturePath(source->ddsPath1);
		}
		// (2026-11-07 perf-mirrorfix) hair, brows, lashes, beards, eye cards, fringed clothes: cut out by every ray that tests alpha (they
		// were solid sheets: the hair's card round the head in mirrors, its pale transparent texels a white fringe, the face inside it
		// shadowed dark); the hair's tint from its draw (RtHairTint, Render.cpp)
		const bool alphaPart = hitTextures && ppLighting && RtScenePick::ActorAlphaTested(alpha != nullptr, alpha ? alpha->flags : 0, skinKind, alphaOn);
		const UInt32 actorAlphaRef = alphaPart ? RtScenePick::ActorAlphaRef(alpha->flags, alpha->alphaTestRef) : 0u;
		float hairTint[3] = { 1.0f, 1.0f, 1.0f };
		// (2026-11-08 perf-reflfix4) the tint the game gives the hair shader (the part's own shader property, +0x84), else a noted draw, else
		// the player's TESNPC colour (HairTintOf, RtHairTint.h). The vanilla hair, beards and brows are rigid (AddGeometry); a mod's skinned
		// hair comes through here.
		const bool tinted = HairTintOf(geo, shaderProperty, true, alphaPart, hairTint) != RtHairTint::None;
		// (2026-11-07 perf-mirrorfix2) a FaceGen face: the game's face colour is 2((2(fg0 - 0.5) + base) 2 fg1), not the base texture (the
		// face dark in mirrors): its gain from the three textures' averages (RtScenePick::FaceGenGain; 2^2.2 where the maps cannot be read)
		float faceGain = 0.0f;
		if (ppLighting && skinKind && hitTextures && (shaderProperty->ulFlags[0] & BSShaderProperty::FaceGen)) {
			BSShaderPPLightingProperty* pp = (BSShaderPPLightingProperty*)shaderProperty;
			auto averageOf = [&](int set, int index) -> const Albedo* {
				NiSourceTexture** t = pp->ppTextures[set];
				NiTexture* texture = t ? t[index] : nullptr;
				if (!texture || !texture->rendererData || !texture->rendererData->dTexture) return nullptr;
				const Albedo& a = AlbedoOfTexture(texture->rendererData->dTexture);
				return a.known ? &a : nullptr;
			};
			const Albedo* base = averageOf(0, 0);
			const Albedo* fg0 = pp->ppTextures[0] && pp->ppTextures[0][0] ? averageOf(0, 1) : nullptr;
			const Albedo* fg1 = pp->ppTextures[1] && pp->ppTextures[1][0] ? averageOf(1, 1) : nullptr;
			faceGain = RtScenePick::FaceGenGain(base ? base->rgb : nullptr, fg0 ? fg0->rgb : nullptr, fg1 ? fg1->rgb : nullptr);
			faceGains[base && fg0 && fg1 ? 0 : 1]++;
			if (faceGainLogs < 8) {
				faceGainLogs++;
				Logger::Log("RT CORE face colour (perf-mirrorfix2): FaceGen head '%s': gain %.2f (linear; %s)", geo->m_pcName ? geo->m_pcName : "",
					faceGain, base && fg0 && fg1 ? "from its base texture and FaceGen maps" : "a FaceGen map could not be read: x 2 in the stored curve");
			}
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
		// (2026-11-07) FaceMorphs: a FaceGen head (its shader's FaceGen flag, or under a BSFaceGenNiNode: the game's head nodes
		// "BSFaceGenNiNodeBiped" / "BSFaceGenNiNodeSkinned" hold the head, eyes, mouth, teeth, hair) within FaceMorphRadius
		const char* faceHow = nullptr;
		if (faceMorphs && geo->geomData && geo->m_kWorldBound) {
			const NiBound* b = geo->m_kWorldBound;
			const float dx = b->Center.x - cx, dy = b->Center.y - cy, dz = b->Center.z - cz;
			if (dx * dx + dy * dy + dz * dz <= faceRadius * faceRadius) {
				if (shaderProperty->ulFlags[0] & BSShaderProperty::FaceGen) faceHow = "FaceGen shader flag";
				else {
					int depth = 0;
					for (const NiNode* n = geo->m_parent; n && depth < 4 && !faceHow; n = n->m_parent, depth++)
						if (n->m_pcName && !strncmp(n->m_pcName, "BSFaceGenNiNode", 15)) faceHow = "under a BSFaceGenNiNode";
				}
			}
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
			// (2026-11-07) RealNormalMaps: the body's normal map with it (skin, clothes, armour; nullptr without the setting)
			IDirect3DBaseTexture9* skinNormal = realNormals && ppLighting ? NormalTextureOf(shaderProperty) : nullptr;
			if (diffuse && e.uvStream == e.stream) {
				slot = AlphaSlotOf(diffuse, nullptr, skinNormal);
				if (slot >= 0) desc.Flags = NVR_EXT_RT_TEXCOORD_FLAGS((int)e.uvOffset - (int)e.offset, e.uvHalf);
				else skinTex[3]++;
			}

			GeometryEntry& entry = geometries[key];
			if (entry.refused) continue;
			entry.lastSeen = sceneFrame;
			entry.skinned = true;
			if (!entry.id) entry.id = nextId++ & 0xFFFFFF;
			// (2026-11-07) FaceMorphs: the head's vertices changed (an expression, lip-sync): posed now, whatever the bones did; desc may now
			// read our patched copy (the morphs reach the CPU vertices, not the partition's buffer)
			bool faceForce = false;
			FaceState* face = nullptr;
			if (faceHow) {
				faceStats[0]++;
				if (faceThisFrame < kFacesAFrame) { faceThisFrame++; faceForce = FaceObserve(geo, p, key, desc, e, faceHow, face).force || (face && face->pending); }
				else faceStats[5]++;
			}
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
			if (!pose && faceForce && entry.sent) { pose = true; faceStats[3]++; }
			if (face && faceForce) face->pending = true;	// until a pose of it is staged (the arena or the op list may be full this frame)
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
					// (2026-11-07 perf-mirrorfix) non-opaque when alpha-tested (RtScenePick::ActorOpFlags)
					op.flags = RtScenePick::ActorOpFlags(hasUv, alphaPart);
					const bool builtAlpha = (op.flags & NVR_RT_OP_FLAG_ALPHA) != 0;
					// 2026-10-31 ActorRebuildInterval: every Nth pose built afresh (the helper refits the others)
					if (entry.sent && actorRebuild && (++entry.poses % actorRebuild) == 0) { op.flags |= NVR_RT_OP_FLAG_REBUILD; actorUpdates[3]++; }
					// (2026-11-07 perf-mirrorfix) a refit must keep the structure's opacity (Vulkan: the geometry flags of its build); the helper
					// refits on the same counts and texcoords only, so a change of the alpha decision alone (an alpha property that changed) is
					// built afresh here
					if (entry.sent && entry.alphaBuilt != builtAlpha) op.flags |= NVR_RT_OP_FLAG_REBUILD;
					entry.alphaBuilt = builtAlpha;
					if (builtAlpha) actorAlphaParts++;
					if (builtAlpha && tinted) actorTintedParts++;
					arenaUsed += info.Texels;
					entry.sent = true;
					if (face) face->pending = false;
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
			// (2026-11-07 perf-mirrorfix) RtScenePick::ActorRecordFlags: textured, or alpha-tested (+ tinted) where the structure was built
			// non-opaque (entry.alphaBuilt: frames without a pose keep what the helper has)
			RtScenePick::ActorPart actorPart;
			actorPart.albedoKnown = albedo.known;
			actorPart.textured = hasUv && slot >= 0;
			actorPart.slot = slot;
			actorPart.normalMap = skinNormal != nullptr;	// (2026-11-07) RealNormalMaps
			actorPart.skin = skinKind;	// (2026-11-07 ptaaa-materials)
			actorPart.alphaTested = actorPart.textured && entry.alphaBuilt;
			actorPart.alphaRef = actorAlphaRef;
			actorPart.tinted = actorPart.alphaTested && tinted;
			if (actorPart.tinted) {	// the record's albedo: the tint's linear colour (RtHit.glsl multiplies it into the texture; alone: the hair's colour)
				for (int k = 0; k < 3; k++) extra.albedo[k] = RtScenePick::TintLinear(hairTint[k]);
			}
			else { extra.albedo[0] = albedo.rgb[0]; extra.albedo[1] = albedo.rgb[1]; extra.albedo[2] = albedo.rgb[2]; }
			// (perf-mirrorfix2) a FaceGen face: its gain on the texture (RtHit.glsl), and on the average (the colour without the texture)
			actorPart.skinGain = actorPart.textured && !actorPart.alphaTested ? RtScenePick::SkinGainCode(faceGain) : 0u;
			if (faceGain > 0.0f && !actorPart.tinted)
				for (int k = 0; k < 3; k++) extra.albedo[k] = (std::min)(1.0f, extra.albedo[k] * faceGain);
			extra.albedo[3] = 1.0f;
			extra.flags = RtScenePick::ActorRecordFlags(actorPart);
			if (skinKind) skinParts++;
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
			instance.customIndex = walkingPlayer && playerHidden ? NVR_RT_CUSTOM_HIDDEN : 0u;	// (2026-11-07) the first-person body: not drawn
			instance.mask = NVR_RT_MASK_ACTOR;
			instance.flags = 0x1u;
			skip[kTaken]++;
		}
	}

	// --- (2026-11-07) FaceMorphs: faces and lip-sync (RtFaceMorph.h; docs/next/RT-DETAIL-GAPS.md 4) ----------------------------------

	// Reads of memory the game owns (the CPU vertices may be freed under us by a model change): a fault is a failed read, not a crash
	static bool HashGuarded(const UInt8* base, UInt32 count, UInt32 stride, UInt32 offset, uint32_t& out) {
		__try { out = RtFaceMorph::Hash(base, count, stride, offset); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool PatchGuarded(UInt8* dst, UInt32 count, UInt32 stride, UInt32 offset, const UInt8* src, UInt32 srcCount, UInt32 srcStride, UInt32 srcOffset,
		const UInt16* map) {
		__try { return RtFaceMorph::PatchPositions(dst, count, stride, offset, src, srcCount, srcStride, srcOffset, map); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool CopyGuarded(void* dst, const void* src, UInt32 bytes) {
		__try { memcpy(dst, src, bytes); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool AgreementGuarded(const UInt8* part, UInt32 count, UInt32 stride, UInt32 offset, const UInt8* src, UInt32 srcCount, UInt32 srcStride,
		UInt32 srcOffset, const UInt16* map, uint32_t& agree, uint32_t& tested) {
		__try { agree = RtFaceMorph::MapAgreement(part, count, stride, offset, src, srcCount, srcStride, srcOffset, map, tested); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	// a read-only look at a vertex range of one of the game's buffers (as BuildLeafCards); a buffer the GPU is still writing is skipped
	// this frame (D3DLOCK_DONOTWAIT) rather than waited for
	static const UInt8* LockRead(IDirect3DVertexBuffer9* vb, UInt32 first, UInt32 bytes) {
		D3DVERTEXBUFFER_DESC d = {};
		void* bits = nullptr;
		if (!vb || !bytes || FAILED(vb->GetDesc(&d)) || first + bytes > d.Size) return nullptr;
		if (FAILED(vb->Lock(first, bytes, &bits, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT)) || !bits) return nullptr;
		return (const UInt8*)bits;
	}

	RtFaceMorph::Decision Core::FaceObserve(NiGeometry* geo, UInt32 p, const GeometryKey& key, NVR_EXT_RT_GEOMETRY_DESC& desc, const PositionElement& e,
		const char* how, FaceState*& out) {
		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);
		const NiSkinPartition* parts = geo->skinInstance->SkinPartition;
		const NiSkinPartition::Partition& part = parts->Partitions[p];
		NiGeometryData* gd = geo->geomData;
		NiGeometryBufferData* own = gd->m_pkBuffData && gd->m_pkBuffData != part.BuffData ? gd->m_pkBuffData : nullptr;
		FaceState& f = faces[key];
		out = &f;
		if (!f.s.frames) {	// met for the first time: what it is (the first few in the log)
			strncpy_s(f.name, geo->m_pcName ? geo->m_pcName : "", _TRUNCATE);
			f.how = how; f.partition = p; f.partitions = parts->PartitionsCount; f.vertices = desc.VertexCount; f.dataVertices = gd->m_usVertices;
			f.keepFlags = gd->m_ucKeepFlags; f.skin = key.owner; f.cpuThere = gd->m_pkVertex != nullptr; f.mapThere = part.VertexMap != nullptr;
			f.own = !gd->m_pkBuffData ? "none" : own ? "a buffer of its own" : "the partition's";
			if (faceFound++ < 8)
				Logger::Log("RT CORE face morphs: head part '%s' (%s), partition %u of %u: %u vertices (the data %u, vertex map %s), CPU vertices %s (keep flags %X, "
					"data flags %X, dirty flags %X), the data's buffer: %s; hashed every frame within FaceMorphRadius %.0f", f.name, how, p, f.partitions, f.vertices,
					f.dataVertices, f.mapThere ? "yes" : "none", f.cpuThere ? "there" : "none", f.keepFlags, (unsigned)gd->m_usDataFlags, (unsigned)gd->m_usDirtyFlags,
					f.own, faceRadius);
		}
		f.lastSeen = sceneFrame;
		// this frame's hashes: the partition's buffer (what the poses read), the CPU vertices, the data's own buffer
		RtFaceMorph::Sample in;
		const UInt32 stride = desc.VertexStride, first = desc.PositionOffset - e.offset;
		if (const UInt8* bits = LockRead(desc.pVertexBuffer, first, desc.VertexCount * stride)) {
			in.part = HashGuarded(bits, desc.VertexCount, stride, e.offset, in.partHash);
			desc.pVertexBuffer->Unlock();
		}
		if (!in.part) faceStats[2]++;
		if (gd->m_pkVertex) in.cpu = HashGuarded((const UInt8*)gd->m_pkVertex, gd->m_usVertices, 12, 0, in.cpuHash);
		PositionElement pe;
		if (own && PositionIn(own, pe) && pe.format != NVR_EXT_RT_POSITION_HALF4) {
			const UInt32 s = own->VertexStride[pe.stream];
			IDirect3DVertexBuffer9* vb = own->VBChip[pe.stream]->VB;
			if (const UInt8* bits = LockRead(vb, own->BaseVertexIndex * s, own->VertCount * s)) {
				in.own = HashGuarded(bits, own->VertCount, s, pe.offset, in.ownHash);
				vb->Unlock();
			}
		}
		const UInt8 feedBefore = f.s.feed;
		RtFaceMorph::Decision d = RtFaceMorph::Observe(f.s, in);
		faceStats[1]++;
		if (d.changed) faceStats[6]++;
		if (!f.firstChange && (f.s.part.changes || f.s.cpu.changes || f.s.own.changes)) f.firstChange = f.s.frames;
		if (d.switched && feedBefore == RtFaceMorph::FeedPartition)
			Logger::Log("RT CORE face morphs: head part '%s': the partition's buffer did not follow its vertices' changes (%u missed, %u followed): its poses read %s "
				"from now on", f.name, f.s.misses, f.s.followed, RtFaceMorph::FeedName(f.s.feed));
		// fed from a copy: refresh it on a change (or make it), and pose from it
		if (f.s.feed != RtFaceMorph::FeedPartition && !f.copyFailed) {
			const int copied = FaceCopy(f, geo, part, desc, e, d.force || f.pending);
			if (copied > 0) {
				desc.pVertexBuffer = f.copy;
				desc.PositionOffset = e.offset;	// the copy starts at the geometry's first vertex (the indices count from there)
			}
			else if (copied < 0) {
				f.copyFailed = true;
				Logger::Log("RT CORE face morphs: head part '%s': its vertices could not be copied from %s (vertex map %s, %u partition vertices, %u in the data): "
					"it stays posed from the partition's buffer", f.name, RtFaceMorph::FeedName(f.s.feed), f.mapThere ? "there" : "none", f.vertices, f.dataVertices);
				f.s.feed = RtFaceMorph::FeedPartition;
				f.s.followed = 0x40000000;	// never again
				d.feed = RtFaceMorph::FeedPartition;
			}
		}
		if (!f.reported && faceReports < 6 && ((f.firstChange && f.s.frames - f.firstChange >= 120) || f.s.frames >= 1800)) FaceReport(f, "");
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		faceMs += 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
		return d;
	}

	// the copy of the partition's vertices our poses read when the morphs reach only the CPU vertices (own buffer): made from the partition's
	// buffer once (weights, indices, texcoords), its positions taken from the feed through the partition's vertex map (on each change)
	// 1: pose from the copy, 0: not this frame (a busy buffer: from the partition's), -1: cannot be made
	int Core::FaceCopy(FaceState& f, NiGeometry* geo, const NiSkinPartition::Partition& part, const NVR_EXT_RT_GEOMETRY_DESC& desc, const PositionElement& e, bool refresh) {
		const UInt32 stride = desc.VertexStride, count = desc.VertexCount, bytes = count * stride;
		NiGeometryData* gd = geo->geomData;
		const UInt8* src = nullptr;
		UInt32 srcCount = 0, srcStride = 12, srcOffset = 0;
		IDirect3DVertexBuffer9* srcVb = nullptr;
		if (f.s.feed == RtFaceMorph::FeedCpu) {
			if (!gd->m_pkVertex) return -1;
			src = (const UInt8*)gd->m_pkVertex;
			srcCount = gd->m_usVertices;
		}
		else {
			NiGeometryBufferData* own = gd->m_pkBuffData;
			PositionElement pe;
			if (!own || own == part.BuffData || !PositionIn(own, pe) || pe.format == NVR_EXT_RT_POSITION_HALF4) return -1;
			srcStride = own->VertexStride[pe.stream];
			srcVb = own->VBChip[pe.stream]->VB;
			srcCount = own->VertCount;
			srcOffset = pe.offset;
			if (!(refresh || !f.copy)) srcVb = nullptr;	// nothing to read this frame
			else if (!(src = LockRead(srcVb, own->BaseVertexIndex * srcStride, srcCount * srcStride))) { f.pending = true; return f.copy ? 1 : 0; }	// busy: the old copy this frame
		}
		bool ok = part.VertexMap || count == srcCount;
		bool fresh = false;
		if (ok && (!f.copy || f.copyBytes != bytes)) {
			if (f.copy) { f.copy->Release(); f.copy = nullptr; }
			ok = SUCCEEDED(TheRenderManager->device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &f.copy, nullptr)) && f.copy;
			f.copyBytes = ok ? bytes : 0;
			fresh = ok;
		}
		if (ok && (fresh || refresh)) {
			void* dst = nullptr;
			ok = SUCCEEDED(f.copy->Lock(0, bytes, &dst, 0)) && dst;
			if (ok) {
				if (fresh) {	// the partition's own vertices first (weights, indices, texcoords)
					const UInt8* pb = LockRead(part.BuffData->VBChip[e.stream]->VB, desc.PositionOffset - e.offset, bytes);
					ok = pb && CopyGuarded(dst, pb, bytes);
					// the vertex map and layouts checked: most partition vertices (all but the morphed ones) must sit at their mapped
					// feed vertex, or the copy would scramble the head
					uint32_t tested = 0, agree = 0;
					if (ok && !AgreementGuarded(pb, count, stride, e.offset, src, srcCount, srcStride, srcOffset, part.VertexMap, agree, tested)) ok = false;
					if (ok && agree * 2 < tested) {
						ok = false;
						Logger::Log("RT CORE face morphs: head part '%s': the vertex map does not match (%u of %u sampled partition vertices at their mapped "
							"vertex): no copy", f.name, agree, tested);
					}
					if (pb) part.BuffData->VBChip[e.stream]->VB->Unlock();
				}
				ok = ok && PatchGuarded((UInt8*)dst, count, stride, e.offset, src, srcCount, srcStride, srcOffset, part.VertexMap);
				f.copy->Unlock();
			}
			if (ok) { f.copyRefreshes++; faceStats[4]++; }
			else if (fresh && f.copy) { f.copy->Release(); f.copy = nullptr; f.copyBytes = 0; }
		}
		if (srcVb && src) srcVb->Unlock();
		return ok && f.copy ? 1 : -1;
	}

	void Core::FaceReport(FaceState& f, const char* why) {
		f.reported = true;
		faceReports++;
		const RtFaceMorph::State& s = f.s;
		const char* answer = s.part.changes ? "the morphs reach the partition's buffer (the game's): re-posing from it shows them" :
			(s.cpu.changes || s.own.changes) ? "the morphs reach only the geometry data's vertices: the poses read our copy" :
			"no change seen (no expression or lip-sync on this head while it was near)";
		Logger::Log("RT CORE face morphs: head part '%s' (%s; skin %p, partition %u of %u, %u vertices, data %u%s)%s: over %u frames the partition's buffer changed in %u "
			"(read in %u), the CPU vertices (%s, keep flags %X) in %u (read in %u), the data's own buffer (%s) in %u (read in %u); the partition followed %u of "
			"their changes and missed %u; %u poses asked for by a change, %u copy refreshes; poses read %s. Answer: %s.", f.name, f.how, f.skin, f.partition,
			f.partitions, f.vertices, f.dataVertices, f.mapThere ? ", vertex map" : "", why, s.frames, s.part.changes, s.part.reads, f.cpuThere ? "there" : "none",
			f.keepFlags, s.cpu.changes, s.cpu.reads, f.own, s.own.changes, s.own.reads, s.followed > 0x3FFFFFFF ? 0u : s.followed, s.misses, s.forced,
			f.copyRefreshes, RtFaceMorph::FeedName(s.feed), answer);
	}

	void Core::FreeFaces(bool all) {
		for (auto it = faces.begin(); it != faces.end(); ) {
			FaceState& f = it->second;
			if (all || sceneFrame - f.lastSeen > kReleaseAfterFrames) {
				if (!f.reported && faceReports < 6 && f.s.frames >= 60) FaceReport(f, " (gone)");
				if (f.copy) f.copy->Release();
				it = faces.erase(it);
			}
			else ++it;
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
		extra.flags = (albedo.known ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | (leafOpaque ? NVR_RT_RECORD_TEXTURED : NVR_RT_RECORD_ALPHA_TESTED | (alphaRef << 8)) | ((UInt32)slot << 16) |
			NvrRtKindBits(NVR_RT_KIND_FOLIAGE);	// (2026-11-07 ptaaa-materials) leaves: light goes through them (the path tracer's PTFoliage)
		foliageCards++;
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

	// ---- UNOFFICIAL 2026-11-07 (rtgap-tessblas, protocol 8) displaced geometry (docs/next/RT-DETAIL-GAPS.md 1c) --------------------------

	// (merged with rtgap-textures, protocol 8) an id the displacement takes is pinned: the helper's displaced copies keep reading it (their
	// DISPLACE_SOURCE names it), so the budget's least-recently-used release never takes it (it still counts in RealTextureBudgetMB). A
	// texture the budget refused is pending (-1), asked again from its retry exchange on, not "never".
	int Core::TextureIdFor(IDirect3DBaseTexture9* texture) {
		if (!realTextures || !texture) return 0;
		auto it = texIds.find(texture);
		if (it != texIds.end() && it->second.tried) {
			const TexEntry& e = it->second;
			if (e.id) { texRes.Pin(e.id); texRes.Use(e.id, frame); return (int)e.id; }
			if (!e.retryAt) return 0;
			if (frame < e.retryAt) return -1;
		}
		if (std::find(looseTextures.begin(), looseTextures.end(), texture) == looseTextures.end()) { texture->AddRef(); looseTextures.push_back(texture); }
		return -1;
	}

	// Once a gather: is the game's displacement on (and the RT setting), its settings as the helper's parameter blocks (the same clamps as
	// DxvkCompute::DisplaceTick / DisplaceObjectSettings), sent every frame while either kind is on (the camera block changes every frame)
	void Core::DisplaceSettings() {
		const bool want = TheSettingManager->GetSettingI("Main.Main.RayTracing", "Displacement") != 0 && realTextures && hitTextures;
		dispLand = want && DxvkCompute::DisplaceLandActive();
		dispObjects = want && DxvkCompute::DisplaceObjectsActive();
		dispInstances = 0;
		if (!dispLand && !dispObjects && !dispSentOn) return;
		auto F = [](const char* section, const char* key, float missing) {
			const float v = TheSettingManager->GetSettingF(section, key);
			return v == v ? v : missing;
		};
		const char* l = "Shaders.Terrain.Displacement";
		const char* o = "Shaders.POM.Displacement";
		const char* r = "Main.Main.RayTracing";
		const float spacing = std::clamp(F(r, "DisplaceSpacing", 16.0f), 4.0f, 256.0f);
		const float split = (float)std::clamp(TheSettingManager->GetSettingI(r, "DisplaceMaxSplit"), 1, 16);
		const float refresh = std::clamp(F(r, "DisplaceRefresh", 32.0f), 1.0f, 1024.0f);
		const float landStart = (std::max)(F(l, "FadeStart", 1024.0f), 0.0f), objStart = (std::max)(F(o, "FadeStart", 1024.0f), 0.0f);
		dispLandReach = (std::max)(F(l, "FadeEnd", 2048.0f), landStart + 1.0f);
		dispObjectReach = (std::max)(F(o, "FadeEnd", 2048.0f), objStart + 1.0f);
		const float blocks[NVR_RT_DISPLACE_BLOCKS][6] = {
			{ std::clamp(F(l, "Strength", 10.0f), 0.0f, 64.0f), std::clamp(F(l, "Center", 0.5f), 0.0f, 1.0f), std::clamp(F(l, "Contrast", 4.0f), 0.0f, 32.0f),
			  floorf(std::clamp(F(l, "BumpSize", 48.0f), 0.0f, 255.0f) + 0.5f), std::clamp(F(l, "LodBias", 0.0f), -2.0f, 4.0f), (std::max)(F(l, "BorderGrid", 2048.0f), 0.0f) },
			{ landStart, dispLandReach, spacing, split, refresh, dispLand ? 1.0f : 0.0f },
			{ std::clamp(F(o, "Strength", 8.0f), 0.0f, 64.0f), std::clamp(F(o, "Center", 0.5f), 0.0f, 1.0f), std::clamp(F(o, "Contrast", 2.0f), 0.0f, 32.0f),
			  std::clamp(F(o, "EdgeFade", 1.5f), 0.0f, 8.0f), std::clamp(F(o, "LodBias", 0.0f), -2.0f, 4.0f), 0.0f },
			{ objStart, dispObjectReach, spacing, split, refresh, dispObjects ? 1.0f : 0.0f },
			{ TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z,
			  (float)(std::max)(0, TheSettingManager->GetSettingI(r, "DisplaceTrianglesPerFrame")), (float)(std::max)(0, TheSettingManager->GetSettingI(r, "DisplaceMaxTriangles")), 0.0f },
		};
		if (record->opCount + NVR_RT_DISPLACE_BLOCKS > NVR_RT_MAX_OPS) return;	// (next frame)
		memcpy(dispParams, blocks, sizeof(blocks));
		for (UInt32 b = 0; b < NVR_RT_DISPLACE_BLOCKS; b++) {
			NvrRtDisplaceParamsOp op = { NVR_RT_OP_DISPLACE_PARAMS, b, {} };
			memcpy(op.v, blocks[b], sizeof(op.v));
			memcpy(&record->ops[record->opCount++], &op, sizeof(op));
		}
		dispSentOn = dispLand || dispObjects;
	}

	// AddGeometry, for an instance just written: a land piece / parallax object within its FadeEnd is flagged for the helper's displaced copy,
	// once its geometry has been described (DATA ops for the land's layer weights and the normals, then the source op with the textures'
	// ids). Until then, and for anything else, it stays flat.
	void Core::Displace(NiGeometry* geo, GeometryEntry& entry, NvrRtInstance& instance, bool land, BSShaderProperty* shaderProperty,
		const NVR_EXT_RT_GEOMETRY_DESC& desc, bool textured) {
		if (!textured || entry.dispState == 3 || !shaderProperty || !shaderProperty->IsLightingProperty() || !geo->m_kWorldBound) return;
		BSShaderPPLightingProperty* lighting = (BSShaderPPLightingProperty*)shaderProperty;
		if (land ? !dispLand : !dispObjects) return;
		// which objects the game displaces: its parallax shader (ParallaxTemplate, the HeightMap at s3 = the texture set's height map)
		NiSourceTexture* heightMap = nullptr;
		if (!land) {
			const bool parallax = shaderProperty->uiShaderIndex == 0xF || (shaderProperty->ulFlags[0] & (BSShaderProperty::Parallax_Shader | BSShaderProperty::Parallax_Occlusion));
			heightMap = parallax && lighting->ppTextures[3] ? lighting->ppTextures[3][0] : nullptr;
			if (!heightMap || !heightMap->rendererData || !heightMap->rendererData->dTexture) return;
		}
		// within FadeEnd (the bound's nearest point): past it the game draws it flat too
		const NiBound* b = geo->m_kWorldBound;
		const float dx = b->Center.x - TheRenderManager->CameraPosition.x, dy = b->Center.y - TheRenderManager->CameraPosition.y, dz = b->Center.z - TheRenderManager->CameraPosition.z;
		const float reach = (land ? dispLandReach : dispObjectReach) + b->Radius + 64.0f;
		if (dx * dx + dy * dy + dz * dz > reach * reach) return;
		if (entry.dispState == 0) {
			// the land's layer weights (TEXCOORD1 / 2, staged as the "position": A with its zw as texcoords for the fourth) and the normals
			const PositionElement& e = lastElement;
			NiGeometryBufferData* data = geo->geomData->m_pkBuffData;
			struct Want { int stream; UInt32 offset; int format; UInt32 flags; } wants[3] = { { -1, 0, -1, 0 }, { -1, 0, -1, 0 }, { -1, 0, -1, 0 } };
			if (land && e.layerFormat[0] >= 0) wants[0] = { e.layerStream[0], e.layerOffset[0], e.layerFormat[0],
				NVR_EXT_RT_TEXCOORD_FLAGS(e.layerFormat[0] == (int)NVR_EXT_RT_POSITION_HALF4 ? 4 : 8, e.layerFormat[0] == (int)NVR_EXT_RT_POSITION_HALF4) };
			if (land && e.layerFormat[0] >= 0 && e.layerFormat[1] >= 0) wants[1] = { e.layerStream[1], e.layerOffset[1], e.layerFormat[1], 0u };
			if (e.normalFormat >= 0) wants[2] = { e.normalStream, e.normalOffset, e.normalFormat, 0u };
			static int told = 0;
			if (told < 4) {
				told++;
				Logger::Log("RT CORE displacement: '%s' (%s): vertex layout: NORMAL type %d, TEXCOORD1 type %d, TEXCOORD2 type %d (D3DDECLTYPE; -1 absent): sends %s%s%s",
					geo->m_pcName ? geo->m_pcName : "", land ? "land" : "object", (int)e.normalType - 1, (int)e.layerType[0] - 1, (int)e.layerType[1] - 1,
					wants[0].stream >= 0 ? "weights A " : "", wants[1].stream >= 0 ? "weights B " : "", wants[2].stream >= 0 ? "normals" : "(smooth normals made by the helper)");
			}
			UInt32 texels = 0;
			NVR_EXT_RT_GEOMETRY_INFO infos[3] = {};
			NVR_EXT_RT_GEOMETRY_DESC descs[3];
			for (int k = 0; k < 3; k++) {
				if (wants[k].stream < 0) continue;
				if ((UInt32)wants[k].stream >= data->StreamCount || !data->VBChip[wants[k].stream] || !data->VBChip[wants[k].stream]->VB) { wants[k].stream = -1; continue; }
				descs[k] = desc;
				const UInt32 stride = data->VertexStride[wants[k].stream];
				descs[k].pVertexBuffer = data->VBChip[wants[k].stream]->VB;
				descs[k].VertexStride = stride;
				descs[k].PositionOffset = wants[k].offset + data->BaseVertexIndex * stride;
				descs[k].PositionFormat = (NVR_EXT_RT_POSITION_FORMAT)wants[k].format;
				descs[k].Flags = wants[k].flags;
				descs[k].pDebugName = "NVR RT displacement data";
				if (FAILED(rt->MeasureGeometry(&descs[k], &infos[k]))) { wants[k].stream = -1; continue; }
				texels += infos[k].Texels;
			}
			if (wants[0].stream < 0) wants[1].stream = -1;	// (B means nothing without A)
			if (record->opCount + 3 >= NVR_RT_MAX_OPS || arenaUsed + texels + 1 > arenaCapacity) { waiting++; return; }	// next frame
			for (int k = 0; k < 3; k++) {
				if (wants[k].stream < 0) continue;
				if (FAILED(rt->StageGeometry(&descs[k], images[0], arenaUsed, &infos[k]))) continue;
				const UInt32 id = nextId++ & 0xFFFFFF;
				NvrRtGeometryOp& op = record->ops[record->opCount++];
				op = NvrRtGeometryOp();
				op.op = NVR_RT_OP_DATA;
				op.id = id;
				op.vertexCount = infos[k].VertexCount;
				op.triangleCount = infos[k].TriangleCount;
				op.arenaTexel = arenaUsed;
				op.flags = wants[k].flags ? NVR_RT_OP_FLAG_TEXCOORDS : 0u;
				arenaUsed += infos[k].Texels;
				entry.dispData[k] = id;
				dispData++;
			}
			entry.dispState = 1;
		}
		if (entry.dispState == 1) {
			// the textures' ids (layer i: its diffuse, height in alpha; an object: its height map, height in red)
			uint32_t ids[7] = {}, layers = 1, heightBits = 1;	// (uint32_t: NvrRtDisplacePack's; UInt32 is unsigned long here)
			bool pending = false, anyTexture = false;
			if (land) {
				NiSourceTexture** textures = lighting->ppTextures[0];
				layers = (std::min)((uint32_t)lighting->usLandscapeTextures, 7u);
				if (!textures || !layers) { entry.dispState = 3; dispNever++; return; }
				for (uint32_t i = 0; i < layers; i++) {
					NiSourceTexture* t = textures[i];
					const int id = TextureIdFor(t && t->rendererData ? t->rendererData->dTexture : nullptr);
					if (id < 0) pending = true; else ids[i] = (uint32_t)id;
					anyTexture = anyTexture || id != 0;
				}
				// which layers have a height (the land pixel shader's LandHeight, c34-c35, as the world's land draws had it: DisplaceShadows'
				// capture of them, when CastShadows took it); unknown: all
				heightBits = (1u << layers) - 1u;
				NiGeometryBufferData* data = geo->geomData->m_pkBuffData;
				if (data && data->VBChip && data->VBChip[0] && data->VBChip[0]->VB) {
					auto found = DisplaceShadows::Captures.find(DisplaceShadows::Key{ data->VBChip[0]->VB, (INT)data->BaseVertexIndex });
					if (found != DisplaceShadows::Captures.end() && found->second.layers == (int)layers) {
						heightBits = 0;
						for (uint32_t i = 0; i < layers; i++) if (found->second.height[i] > 0.0f) heightBits |= 1u << i;
					}
				}
			}
			else {
				const int id = TextureIdFor(heightMap->rendererData->dTexture);
				if (id < 0) pending = true; else ids[0] = (uint32_t)id;
				anyTexture = id != 0;
			}
			if (pending) return;	// (sent at this frame's end: next frame)
			if (!anyTexture) { entry.dispState = 3; dispNever++; return; }
			if (record->opCount >= NVR_RT_MAX_OPS) return;
			NvrRtDisplaceSourceOp op = {};
			op.op = NVR_RT_OP_DISPLACE_SOURCE;
			op.id = entry.id;
			op.weightsA = entry.dispData[0]; op.weightsB = entry.dispData[1]; op.normals = entry.dispData[2];
			NvrRtDisplacePack(op.packed, ids, layers, heightBits, land ? NVR_RT_DISPLACE_KIND_LAND : NVR_RT_DISPLACE_KIND_OBJECT);
			memcpy(&record->ops[record->opCount++], &op, sizeof(op));
			entry.dispState = 2;
			dispSources++;
			static int toldSource = 0;
			if (toldSource < 4) {
				toldSource++;
				Logger::Log("RT CORE displacement: geometry %u '%s' described (%s, %u layers, height bits 0x%02X, textures %u %u %u, data %u %u %u)", entry.id,
					geo->m_pcName ? geo->m_pcName : "", land ? "land" : "parallax object", layers, heightBits, ids[0], ids[1], ids[2], entry.dispData[0],
					entry.dispData[1], entry.dispData[2]);
			}
		}
		if (entry.dispState == 2) {
			const UInt32 key = (UInt32)(((uintptr_t)geo >> 4) * 2654435761u) >> NVR_RT_INSTANCE_KEY_SHIFT;
			instance.flags |= NVR_RT_INSTANCE_DISPLACE | (key << NVR_RT_INSTANCE_KEY_SHIFT);
			dispInstances++;
		}
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
		if (shaderProperty && (shaderProperty->ulFlags[0] & BSShaderProperty::FaceGen) && rigidFaceGen++ < 4)	// (2026-11-07) FaceMorphs covers skinned heads only
			Logger::Log("RT CORE face morphs: rigid (not skinned) FaceGen geometry '%s': traced as a static shape, a morph of it is not followed", geo->m_pcName ? geo->m_pcName : "");
		pick.refraction = shaderProperty && (shaderProperty->GetFlag(BSSP_REFRACTION) || shaderProperty->GetFlag(BSSP_FIRE_REFRACTION));
		// (2026-11-07, RT-DETAIL-GAPS.md 5) decals: alpha-tested, lifted, passed by shadow / AO / visibility rays (RtScenePick::Classify)
		pick.decal = shaderProperty && (shaderProperty->GetFlag(BSSP_DECAL) || shaderProperty->GetFlag(BSSP_DYNAMIC_DECAL));
		if (pick.decal && !pick.refraction) decalFate[0]++;
		// Alpha-tested casters (AlphaTested on): traced with their diffuse alpha (RtAlpha.glsl); blended-only (glass, fx) stays out.
		// (FarObjectLOD, 2026-10-31 / 11-03: far object LOD's alpha property is its distance fade, it goes in opaque; far tree / billboard LOD
		// is cut out (TEST_ENABLE) and takes the alpha-tested path: RtScenePick::Classify)
		NiAlphaProperty* alpha = pick.hasShaderAndData && !pick.skinned ? (NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha) : nullptr;
		pick.hasAlphaProperty = alpha != nullptr;
		pick.alphaFlags = alpha ? (uint16_t)alpha->flags : 0;
		// UNOFFICIAL 2026-11-10 BlendedHair (seen on an RTX-class card with the UNMODDED game: people bald under the path tracer, a ghost of
		// the hair's outline; the log: "'FaceGenHairNoHat' blended only (no alpha test)", hair parts in the traced scene 0 of 22683 hair
		// draws). The game's own hair, brows and beards are alpha-BLENDED with no alpha test, and blended-only geometry stays out of the
		// scene (glass, effects). A hair mod with alpha-tested hair went in, which is why this never showed on the bench. Hair (the Hair
		// flag or a FaceGen hair name) that only blends is taken as a cut-out at half alpha, as the skinned path does (ActorAlphaRef).
		bool hairBlendedOnly = false;
		if (blendedHairOn && alphaOn && alpha && shaderProperty && (pick.alphaFlags & RtScenePick::kAlphaBlend) && !(pick.alphaFlags & RtScenePick::kAlphaTest) &&
			((shaderProperty->ulFlags[0] & BSShaderProperty::Hair) || RtHairTint::HairName(geo->m_pcName))) {
			pick.alphaFlags |= RtScenePick::kAlphaTest;
			hairBlendedOnly = true;
			static int told = 0;
			if (told < 4) { told++; Logger::Log("RT CORE hair: '%s' only blends (alpha flags %04X, no alpha test): traced as a cut-out at alpha 128 (BlendedHair)", geo->m_pcName ? geo->m_pcName : "", (unsigned)alpha->flags); }
		}
		RtScenePick::Switches pickSwitches;
		pickSwitches.cullAngle = cullAngle; pickSwitches.alphaTested = alphaOn; pickSwitches.leafCards = leafPlanes != 0;
		pickSwitches.decals = decalsOn;
		pickSwitches.glass = glassOn;
		// (2026-11-07 ptaaa-materials) the glass rule is looked up only for what would otherwise stay out (refraction, blended only)
		if (glassOn && shaderProperty && !land && (pick.refraction || (alpha && (pick.alphaFlags & RtScenePick::kAlphaBlend) && !(pick.alphaFlags & RtScenePick::kAlphaTest))))
			pick.glassRule = !strcmp(RtMaterialClassOf(geo), "glass");
		bool alphaTested = false, glass = false;
		switch (RtScenePick::Classify(pick, pickSwitches, alphaTested, glass)) {
		case RtScenePick::Taken: break;
		case RtScenePick::Culled: culledSmall++; LogLeftOut(geo, "small at its distance (CullAngle)", -1); return;
		case RtScenePick::NoShader: skip[kNoShader]++; LogLeftOut(geo, "no shader / buffer data", -1); return;
		case RtScenePick::Skinned: skip[kSkinned]++; if (!land) LogLeftOut(geo, "skinned (not an actor)", -1); return;	// (2026-11-06: was silent)
		case RtScenePick::Leaves: AddLeaves(geo, shaderProperty, distance); return;	// (2026-10-30) LeafCards
		case RtScenePick::NotLighting: skip[kNotLighting]++; LogLeftOut(geo, shaderProperty ? "not a lighting shader" : "no shader property", shaderType); return;
		case RtScenePick::DecalOrRefraction: skip[kFlags]++; if (pick.decal && !pick.refraction) decalFate[5]++;
			LogLeftOut(geo, pick.refraction ? "refraction" : "decal (Decals or AlphaTested off)", -1); return;
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
		// UNOFFICIAL 2026-11-07 (RT-DETAIL-GAPS.md 5) decals: the game makes and drops dynamic decals (blood) all the time; a new one at a
		// dropped one's addresses has other vertices: the old structure is released (Release: next frame's op list) and this one built
		// under a new id. Dropped decals that do not come back go as every geometry does (kReleaseAfterFrames unseen).
		if (pick.decal) {
			const UInt32 signature = DecalSignature(geo->geomData);
			if (entry.sent && signature && entry.decalSignature && signature != entry.decalSignature) {
				Release(entry.id);
				entry.sent = false; entry.id = 0; entry.normalState = 0;
				decalFate[4]++;
			}
			entry.decalSignature = signature;
			if (entry.normalState == 0) {	// the average normal, once (the vertex normals' CPU copy; without one the decal is not lifted)
				const NiGeometryData* gd = geo->geomData;
				entry.normalState = gd && gd->m_pkNormal && RtScenePick::AverageNormal(&gd->m_pkNormal[0].x, gd->m_usVertices, entry.decalNormal) ? 1 : -1;
			}
		}
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
			entry.triangles = info.TriangleCount;	// (2026-11-07 ptaaa-emissive)
			RtScenePick::Record po; po.alphaTested = alphaTested; po.textured = textured; po.compact = compactOn;
			po.kind = glass ? NVR_RT_KIND_GLASS : NVR_RT_KIND_PLAIN;	// (2026-11-07 ptaaa-materials) glass: built non-opaque
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
		pr.land = land && !pick.decal; pr.albedoKnown = albedo.known; pr.glowMap = HasGlowMap(shaderProperty);
		pr.alphaTested = alphaTested; pr.alphaRef = alphaTested && alpha ? (hairBlendedOnly ? 128u : (UInt32)alpha->alphaTestRef) : 0u;	// (hairBlendedOnly: 2026-11-10 BlendedHair)
		// (2026-11-07) a decal: alpha-tested whatever its alpha property (blended ones at 128, opaque ones at 0: RtScenePick::DecalAlphaRef)
		pr.decal = pick.decal; pr.hidden = walkingPlayer && playerHidden;
		if (pick.decal) pr.alphaRef = RtScenePick::DecalAlphaRef(pick.hasAlphaProperty, pick.alphaFlags, alpha ? (UInt32)alpha->alphaTestRef : 0u);
		pr.textured = textured; pr.slot = alphaSlot;
		pr.normalMap = (alphaTested || textured) && (normalAtlasImage >= 0 || realNormals) && NormalTextureOf(shaderProperty) != nullptr;	// UNOFFICIAL 2026-11-02 (2026-11-07: or RealNormalMaps)
		if (alphaTested || textured) pr.materialRule = RtMaterialRuleOf(geo, &pr.rough, &pr.metal);	// UNOFFICIAL 2026-11-03 (bits 26-31)
		pr.hookMask = MaskHook ? (int)(MaskHook(geo, land) & 0xFFu) : -1;
		pr.player = walkingPlayer; pr.farLod = walkingFar;
		// (2026-11-07 ptaaa-materials) the path tracer's material kind: glass panes; cut-out cards whose texture's rule is "plant" (leaves,
		// bushes: thin, light goes through them; trunks are not cut out and stay plain)
		if (glass) { pr.kind = NVR_RT_KIND_GLASS; glassTaken++; }
		else if (alphaTested && !pick.decal && !land && !strcmp(RtMaterialClassOf(geo), "plant")) { pr.kind = NVR_RT_KIND_FOLIAGE; foliageCards++; }
		// (2026-11-08 perf-reflfix4, test 12: the reflected hair still white) the hair, beards and brows are RIGID shapes (the vanilla hair
		// NIFs hold no skin instance; the tester's log: 'FaceGenHairNoHat' taken here): their tint, as the game gives the hair shader, goes in
		// a TINTED record (RtScenePick::RecordFlags: alpha-tested + the textured bit; its albedo the tint's linear colour, RtHit.glsl)
		if (!land && !walkingFar && !pick.decal && !glass) {
			float hairTint[3];
			if (HairTintOf(geo, shaderProperty, false, alphaTested && alphaSlot >= 0, hairTint) != RtHairTint::None) {
				pr.tinted = true;
				for (int k = 0; k < 3; k++) extra.albedo[k] = RtScenePick::TintLinear(hairTint[k]);
			}
		}
		extra.flags = RtScenePick::RecordFlags(pr);
		if (pr.tinted) {	// (2026-11-08 perf-reflfix4) the hair's record as sent: TINTED (0x60 both), no GLOW (0x04: its slot 2 is the layer map)
			static int toldRecord = 0;
			if (toldRecord < 4) {
				toldRecord++;
				NiSourceTexture** layer = ((BSShaderPPLightingProperty*)shaderProperty)->ppTextures[2];
				Logger::Log("RT CORE hair record (perf-reflfix4): '%s' flags %08X: tinted %d, glow %d, alpha ref %u, slot %u, albedo %.4f %.4f %.4f; its slot-2 "
					"texture '%s' (%s)", geo->m_pcName ? geo->m_pcName : "", extra.flags, RtScenePick::IsTinted(extra.flags) ? 1 : 0,
					(extra.flags & NVR_RT_RECORD_GLOW) ? 1 : 0, (extra.flags >> 8) & 0xFFu, (extra.flags >> 16) & 0x3FFu, extra.albedo[0], extra.albedo[1], extra.albedo[2],
					layer && *layer && (*layer)->ddsPath1 ? (*layer)->ddsPath1 : "(none)", SlotTwoIsLayer(shaderProperty) ? "the hair layer map: not a glow map" : "a glow map");
			}
		}
		LogAlphaTaken(geo, alphaTested ? alphaSlot : -1, alphaTested ? pr.alphaRef : 999u, alphaTested && alpha && (alpha->flags & NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK) != 0);
		LogBigTaken(geo);
		if ((extra.flags & NVR_RT_RECORD_GLOW) && geo->m_kWorldBound && glows.size() < 4096) {	// (2026-10-30) GlowLights()
			const NiBound* b = geo->m_kWorldBound;
			glows.push_back({ { b->Center.x - TheRenderManager->CameraPosition.x, b->Center.y - TheRenderManager->CameraPosition.y,
				b->Center.z - TheRenderManager->CameraPosition.z }, b->Radius, { albedo.rgb[0], albedo.rgb[1], albedo.rgb[2] },
				record->instanceCount, entry.id, entry.triangles });	// (2026-11-07 ptaaa-emissive) its record, geometry, triangles
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
		if (plantSway && !land && !pick.decal && PlantWind::RtLean(geo, lean, base, H)) {
			const float baseRel = base - TheRenderManager->CameraPosition.z;	// camera-relative, as the transform
			for (int r = 0; r < 2; r++) {
				const float k = lean[r] / H;
				for (int c = 0; c < 4; c++) instance.transform[r][c] += k * instance.transform[2][c];
				instance.transform[r][3] -= k * baseRel;
			}
			swayed++;
		}
		// (2026-11-07, RT-DETAIL-GAPS.md 5) a decal floats kDecalLift above what it lies on, along its average normal (RtScenePick::DecalLift)
		if (pick.decal) {
			decalFate[1]++;
			if (entry.normalState > 0 && RtScenePick::DecalLift(instance.transform, entry.decalNormal, RtScenePick::kDecalLift)) decalFate[2]++;
			else decalFate[3]++;
		}
		instance.geometryId = entry.id;
		instance.customIndex = RtScenePick::CustomIndex(pr);	// (2026-11-07) land, hidden (the first-person body), decal
		instance.mask = RtScenePick::InstanceMask(pr);	// (2026-11-06 RtScenePick.h) 0x01 land / 0x02 objects or the hook's; 0x10 alpha-tested, 0x40 the player's, 0x80 far LOD
		instance.flags = 0x1u;	// both faces (the game's two-sided and one-sided meshes alike: shadows need the back faces too)
		if ((dispLand || dispObjects) && !walkingFar && !alphaTested && !glass) Displace(geo, entry, instance, land, shaderProperty, desc, textured);	// (2026-11-07 rtgap-tessblas)
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
				extra.flags = (albedo.known ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | NVR_RT_RECORD_ALPHA_TESTED | ((d.alphaRef & 0xFFu) << 8) | ((UInt32)slot << 16) |
					NvrRtKindBits(NVR_RT_KIND_FOLIAGE);	// (2026-11-07 ptaaa-materials) grass blades: thin, light goes through (PTFoliage)
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
		RtHairTint::Frame = sceneFrame;	// (2026-11-07 perf-mirrorfix) the hair draws note their tint while actors with textures are traced
		RtHairTint::Wanted = alphaOn || (actorsOn && rtSkin && hitTextures);	// (2026-11-08 perf-reflfix4) the rigid hair: an alpha-tested caster
		SweepSlots();	// UNOFFICIAL 2026-11-13 (rt-audit F3) atlas slots whose textures the game dropped
		// A cell change (a door, a jump, a load): every actor partition goes at once (its buffers and bones may be unloaded and the
		// addresses reused by new ones); the actors still there are posed again from scratch this frame
		static const void* lastCell = nullptr;
		// UNOFFICIAL 2026-11-13 (rt-audit F6) the SPACE, not the exterior cell: outdoors parentCell changes at every cell border, which released and
		// rebuilt every actor partition (33-230 a frame in the user's log) at each crossing. Indoors the cell, outdoors the worldspace; and a camera
		// jump past 8192 units in one frame (a save load or fast travel in the same worldspace: every actor made again at new addresses). Actors that
		// leave the loaded grid age out as any geometry (kReleaseAfterFrames); a partition met again at a dropped one's addresses is rebuilt by the
		// helper when its counts differ (NvrRtHelper.cpp refit rule) and every ActorRebuildInterval poses anyway.
		const void* space = Player ? (const void*)Player->GetWorldSpace() : nullptr;
		const void* cell = space ? space : Player ? (const void*)Player->parentCell : nullptr;
		static D3DXVECTOR3 lastEye(0.0f, 0.0f, 0.0f);
		const D3DXVECTOR3 eye(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
		const D3DXVECTOR3 eyeMove = eye - lastEye;
		const bool jumped = D3DXVec3LengthSq(&eyeMove) > 8192.0f * 8192.0f;
		lastEye = eye;
		if (cell != lastCell || jumped) {
			for (auto it = geometries.begin(); it != geometries.end(); ) {
				if (!it->second.skinned) { ++it; continue; }
				if (it->second.sent) Release(it->second.id);	// (2026-10-30: queued; a full op list used to drop it and the helper kept it)
				it = geometries.erase(it);
			}
			FreeFaces(true);	// (2026-11-07) the heads' states are keyed as their partitions
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
		DisplaceSettings();	// (2026-11-07 rtgap-tessblas) and the parameter blocks for the helper
		compactOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "Compact") != 0;
		buildBudget = (UInt32)(std::max)(0, TheSettingManager->GetSettingI("Main.Main.RayTracing", "BuildTrianglesPerFrame"));
		buildNear = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "BuildNearRadius"));
		cullAngle = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "CullSmallAngle"));
		plantSway = TheSettingManager->GetSettingI("Main.Main.RayTracing", "PlantSway") != 0;
		const int planes = TheSettingManager->GetSettingI("Main.Main.RayTracing", "LeafCards");
		leafPlanes = planes <= 0 ? 0u : planes >= 3 ? 3u : 2u;
		leafOpaque = TheSettingManager->GetSettingI("Main.Main.RayTracing", "LeafCardsOpaque") != 0;
		// UNOFFICIAL 2026-11-07 (ptaaa-omm, protocol 9) [Main.Main.RayTracing] OpacityMicromaps (live): the helper gives alpha-tested geometry
		// opacity micromaps (known-solid micro-triangles resolved by the GPU, the rest by the shaders' alpha test as before) where the GPU has
		// VK_EXT_opacity_micromap; it needs the alpha atlas' image (tools/nvr-rt-helper/NvrRtOmm.inl). No such GPU: nothing changes.
		if (shared) {
			const bool omm = atlasImage >= 0 && TheSettingManager->GetSettingI("Main.Main.RayTracing", "OpacityMicromaps") != 0;
			shared->opacityMicromaps = omm ? (NVR_RT_OMM_ON | ((uint32_t)(atlasImage + 1) & NVR_RT_OMM_ATLAS_MASK)) : 0u;
			static UInt32 toldOmm = 0xFFFFFFFFu;
			const UInt32 ommState = shared->ommState;
			if (ommState != toldOmm && frame > 2) {
				toldOmm = ommState;
				static const char* names[] = { "off", "asked for, but this GPU has no VK_EXT_opacity_micromap (the shaders' alpha test as before)", "on",
					"OFF for this session after a failed check (NvrRtHelper.log says why)", "self-test" };
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT opacity micromaps: %s", ommState < 5 ? names[ommState] : "?");
			}
		}
		trianglesThisFrame = 0;
		radius = (std::max)(512.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "SceneRadius"));
		// UNOFFICIAL 2026-11-07 (rtgap, RT-DETAIL-GAPS.md 6 + 7) PTRealtime overrides, as it overrides the path tracer's own settings
		// (RtPathTrace.cpp): the grass on (with AlphaTested) within at most kPtGrassRadius, and FarLandLOD on (the distant hills block the
		// low sun and the sky, and show in water and mirrors; FarObjectLOD stays the user's: its cost did not pass, RT-TEST-CHANGELOG.md)
		const bool ptRealtime = TheSettingManager->GetSettingI("Main.Main.RayTracing", "PTRealtime") != 0;
		grassOn = alphaOn && (grassSetting || ptRealtime);
		grassRadius = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "GrassRadius"));
		if (ptRealtime) grassRadius = (std::min)(grassRadius, kPtGrassRadius);
		grassMax = (UInt32)(std::max)(0, TheSettingManager->GetSettingI("Main.Main.RayTracing", "GrassMaxTufts"));
		// (2026-11-07, RT-DETAIL-GAPS.md 5) decals: live, with the alpha atlas (they are alpha-tested)
		decalsOn = alphaOn && atlasImage >= 0 && TheSettingManager->GetSettingI("Main.Main.RayTracing", "Decals") != 0;
		// UNOFFICIAL 2026-11-07 (ptaaa-materials, RT-PATH-TRACING.md "materials") glass panes in the scene: only for the path tracer (every other
		// shader passes them, RtAlpha.glsl), live; non-opaque instances, so they need the alpha test (AlphaTested + the atlas)
		glassOn = alphaOn && atlasImage >= 0 && TheSettingManager->GetSettingI("Main.Main.RayTracing", "PTGlass") > 0 &&
			(ptRealtime || RtPathTrace::ReferenceOn());	// (2026-11-11: ReferencePT as honoured)
		if (glassTaken || skinParts || foliageCards) {
			static UInt32 told = 0;
			if (told < 6 && (TheFrameRateManager->FrameNumber % 600u) == 0u) {
				told++;
				Logger::Log("RT CORE materials (path tracer): glass panes %u, skin partitions %u, foliage cards %u this frame (PTGlass %s)", glassTaken, skinParts,
					foliageCards, glassOn ? "on" : "off");
			}
		}
		glassTaken = skinParts = foliageCards = 0;
		actorRadius = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "ActorRadius"));
		actorInterval = (UInt32)(std::max)(1, (std::min)(8, TheSettingManager->GetSettingI("Main.Main.RayTracing", "ActorUpdateInterval")));
		farLodOn = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FarObjectLOD") != 0;	// UNOFFICIAL 2026-10-31
		farLandOn = ptRealtime || TheSettingManager->GetSettingI("Main.Main.RayTracing", "FarLandLOD") != 0;
		farRadius = (std::max)(radius, TheSettingManager->GetSettingF("Main.Main.RayTracing", "FarRadius"));
		actorStillSkip = TheSettingManager->GetSettingI("Main.Main.RayTracing", "ActorStillSkip") != 0;
		actorStillThreshold = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "ActorStillThreshold"));
		firstPersonBody = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FirstPersonBody") != 0;
		actorRebuild = (UInt32)(std::max)(0, TheSettingManager->GetSettingI("Main.Main.RayTracing", "ActorRebuildInterval"));
		faceMorphs = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FaceMorphs") != 0;	// UNOFFICIAL 2026-11-07
		faceRadius = (std::max)(0.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "FaceMorphRadius"));
		faceThisFrame = 0;
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
			playerHidden = !Player->isThirdPerson;	// (2026-11-07) first person: the body is traced (FirstPersonBody) but not drawn
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
			FreeFaces(false);
			for (auto it = geometries.begin(); it != geometries.end(); ) {
				if (sceneFrame - it->second.lastSeen > kReleaseAfterFrames) {
					if (it->second.sent) Release(it->second.id);
					for (UInt32 d : it->second.dispData) if (d) Release(d);	// (2026-11-07 rtgap-tessblas) its DATA ids
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

void RtCore::AskScratch(UINT Megabytes) { if (TheCore.inSetup && Megabytes > TheCore.scratchMB) TheCore.scratchMB = Megabytes; }

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

UINT RtCore::PassesLeft() {	// UNOFFICIAL 2026-11-07
	const NvrRtFrame* f = TheCore.record;
	return TheCore.recording && f && f->passCount < NVR_RT_MAX_PASSES ? NVR_RT_MAX_PASSES - f->passCount : 0;
}

bool RtCore::AddPass(int Shader, const int* Images, UINT ImageCount, UINT WriteMask, const void* Constants, UINT ConstantsSize, UINT GroupsX, UINT GroupsY) {
	NvrRtFrame* f = TheCore.record;
	if (!TheCore.recording || !f || f->passCount >= NVR_RT_MAX_PASSES || Shader < 0 || ImageCount > NVR_RT_PASS_IMAGES_MAX || ConstantsSize > NVR_RT_PASS_CONSTANTS)
		return false;
	// UNOFFICIAL 2026-11-09 (rt-review; docs/next/RT-PT-REVIEW-2026-11-09.md R3) a shader the helper could not load or build (its
	// shaderFailures bit: a missing .spv, a driver that refuses the pipeline). The helper skips such a pass without a word to the game,
	// which went on to copy out and use an image nothing had written (all shadow, or last frame's). Refused here instead, so the effect
	// takes the path it takes for any failed AddPass (its own passes); logged once per shader.
	if (TheCore.shared && Shader < (int)NVR_RT_MAX_SHADERS && (TheCore.shared->shaderFailures & (1u << Shader))) {
		static uint32_t logged = 0;
		if (!(logged & (1u << Shader))) {
			logged |= 1u << Shader;
			Logger::Log("[WARNING] UNOFFICIAL Vegas Shaders DXVK: RT helper could not load the trace shader %s (Compute\\Rt\\%s.spv; the helper's log says why): its passes are not run, the effect that asked falls back.",
				Shader < (int)TheCore.shaders.size() ? TheCore.shaders[Shader].c_str() : "?", Shader < (int)TheCore.shaders.size() ? TheCore.shaders[Shader].c_str() : "?");
		}
		return false;
	}
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
		if (!wanted) return false;	// a running helper stays up (idle); the game's exit stops it (RtCore::Shutdown)
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
		// UNOFFICIAL 2026-11-10 (PT early kick) an open SubmitEarly first: its wait and copies before this record, so the helper never has an
		// unpaired frame and this exchange's geometry staging / copies in come after its WaitShared (the arena rule above). Only with
		// NVR_RT_EARLY_PT set is anything ever pending.
		if (c.pendingN) c.CollectEarly(Device);
		// The ring slot: the helper must have copied out the record that used it last (normally long ago; a few ms at worst)
		const UInt64 n = c.frame + 1;
		{
			SessionLog::StageScope wait("RT core: waiting for the helper's ring slot (100 ms at most)");	// (2026-11-07 perf-ptcost: STALL lines)
			for (int i = 0; i < 100 && c.shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n; i++) Sleep(1);
		}
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

		// UNOFFICIAL 2026-11-10, measurement only (F10 profile): the exchange's three parts on the game's GPU timeline. 'in' = what the game
		// queues before the signal (scene data, the users' copies in), 'round trip' = from the signal to the first command after the wait
		// (GPU idle + the helper's whole frame + the cross-process hand-over: minus the log's 'RT CORE: frame' GPU total = the hand-over),
		// 'out' = the results copied back. Only the frame's own exchange (Point 0); the timers do nothing with the profile off.
		static GpuTimer timerIn("  RT exchange: in (scene + copies)"), timerTrip("  RT exchange: helper round trip"), timerOut("  RT exchange: out (copies)");
		const bool timedIn = !only && timerIn.Begin(Device);
		static GpuTimer timerSubmit("  RT early: submit (scene + copies)");	// UNOFFICIAL 2026-11-10 (PT early kick): SubmitEarly's 'in'
		const bool timedSubmit = c.submitOnly && timerSubmit.Begin(Device);
		const UInt32 now = TheFrameRateManager->FrameNumber + 1;
		if (c.gatheredFrame != now) {
			c.glows.clear();
			c.Gather();
			c.PumpTextures(f, n);	// (2026-11-07, protocol 7) the new slots' textures, before the ops list closes
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
		{	// (2026-11-07 perf-ptcost) RtCore::HiddenInstances for the users' records
			UInt32 hidden = 0;
			for (uint32_t i = 0; i < f.instanceCount; i++) if (f.instances[i].customIndex & NVR_RT_CUSTOM_HIDDEN) hidden++;
			c.hiddenInstances = hidden;
		}
		if (only) { if (only->Record) only->Record(Device); }
		// (2026-11-10 PT early kick) a user whose Record went with this frame's SubmitEarly is not recorded again (its results are pending)
		else for (const RtCore::User& user : c.users) if (user.Point == 0 && !c.Out(user) && user.Wants && user.Wants() && user.Record && !c.EarlyThisFrame(user.Name)) user.Record(Device);
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
		if (timedIn) timerIn.End();
		if (timedSubmit) timerSubmit.End();
		const bool timedTrip = !only && timerTrip.Begin(Device);
		c.share->SignalShared(c.inputsReady, n);
		if (c.submitOnly) {
			// UNOFFICIAL 2026-11-10 (PT early kick) SubmitEarly: no wait here; the copies out wait for CollectEarly (pendingOut), the helper
			// traces while the game draws the world
			c.pendingOut.swap(c.copiesOut);
			c.pendingN = n;
			LARGE_INTEGER signalled;
			QueryPerformanceCounter(&signalled);
			c.earlySignalQpc = signalled.QuadPart;
		}
		else {
			// UNOFFICIAL 2026-11-10, BENCH ONLY (env NVR_RT_WAIT_PREV): the game waits for the PREVIOUS frame's results instead of this frame's, to
			// measure what the two cross-process hand-overs cost a frame (the driver reported the helper's work done ~3 ms late on an L4). The
			// copies below then read images the helper may still be writing: the picture is wrong, only the frame time counts.
			static const bool waitPrev = getenv("NVR_RT_WAIT_PREV") != nullptr;
			if (waitPrev) { static bool told = false; if (!told) { told = true; Logger::Log("UNOFFICIAL Vegas Shaders DXVK: NVR_RT_WAIT_PREV: the game waits for the PREVIOUS frame's ray tracing results (bench timing only: the pictures are wrong)"); } }
			if (!waitPrev) c.share->WaitShared(c.outputReady, n);
			else if (n > 1) c.share->WaitShared(c.outputReady, n - 1);
			if (timedTrip) timerTrip.End();
			const bool timedOut = !only && timerOut.Begin(Device);
			for (const PendingCopy& copy : c.copiesOut) {
				const RECT used = { 0, 0, (LONG)(copy.width ? copy.width : c.imageSizes[copy.image].first),
					(LONG)(copy.height ? copy.height : c.imageSizes[copy.image].second) };
				c.share->CopyFromShared(copy.target, 0, 0, c.images[copy.image], &used);
			}
			if (timedOut) timerOut.End();
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
			if (c.faceStats[0])	// (2026-11-07) FaceMorphs: the heads near the camera
				Logger::Log("RT CORE face morphs: %.2f head partitions a frame within FaceMorphRadius %.0f (%.2f over the %u a frame hashed), %.2f hashed, %.2f with the "
					"partition's buffer unreadable that frame, %.2f changed; %.2f poses forced by a change (bones still or between poses), %.2f copy refreshes; "
					"%.3f ms a frame; %u heads' parts known, %u fed from a copy", c.faceStats[0] / 150.0f, c.faceRadius, c.faceStats[5] / 150.0f, Core::kFacesAFrame,
					c.faceStats[1] / 150.0f, c.faceStats[2] / 150.0f, c.faceStats[6] / 150.0f, c.faceStats[3] / 150.0f, c.faceStats[4] / 150.0f, c.faceMs / 150.0,
					(unsigned)c.faces.size(), (unsigned)std::count_if(c.faces.begin(), c.faces.end(), [](const auto& f) { return f.second.s.feed != RtFaceMorph::FeedPartition; }));
			memset(c.faceStats, 0, sizeof(c.faceStats));
			c.faceMs = 0.0;
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
			if (c.dispLand || c.dispObjects || c.dispSources)	// (2026-11-07 rtgap-tessblas)
				Logger::Log("RT CORE: displacement (land %s, objects %s): %u instances flagged this frame; %u geometries described to the helper, %u data sends, "
					"%u left flat (no texcoords / textures / layout)", c.dispLand ? "on" : "off", c.dispObjects ? "on" : "off", c.dispInstances, c.dispSources,
					c.dispData, c.dispNever);
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
			{	// UNOFFICIAL 2026-11-07 (rtgap-decals) [Main.Main.RayTracing] Decals
				const float frames = (float)(n == 60 ? 60 : n == 600 ? 540 : 600);
				const UInt32* d = c.decalFate;
				if (d[0]) Logger::Log("RT CORE: decals %s: %.1f met a frame, %.1f traced (%.1f lifted %.1f units, %.1f without vertex normals: not lifted), "
					"%.1f left out (Decals / AlphaTested off; the rest: the alpha-tested fates above); %u rebuilt (a new decal at a dropped one's buffers)",
					c.decalsOn ? "on" : "off", d[0] / frames, d[1] / frames, d[2] / frames, RtScenePick::kDecalLift, d[3] / frames, d[5] / frames, d[4]);
				for (UInt32& v : c.decalFate) v = 0;
			}
			if (n == 60 && c.actorsOn)
				Logger::Log("RT CORE: actors: %u skinned partitions posed in exchanges 1-60 (%s); skipped: no buffer data %u, bones %u, not described %u, "
					"streams / half positions %u, palette %u; hit textures: not PPLighting %u, no diffuse %u, texcoords in another stream %u, "
					"no slot %u, staged without texcoords %u, textured %u", c.actorPartitions, c.rtSkin ? "fork 1.8-rt-skin" : "this d3d9.dll has no skinning: off",
					c.partitionFate[0], c.partitionFate[1], c.partitionFate[2], c.partitionFate[3], c.partitionFate[4],
					c.skinTex[0], c.skinTex[1], c.skinTex[2], c.skinTex[3], c.skinTex[4], c.skinTex[5]);
			if (n == 60 && c.actorsOn)	// (2026-11-07 perf-mirrorfix)
				Logger::Log("RT CORE: actors: %u poses built as cut-outs (alpha-tested hair, brows, eye cards, fringes: AlphaTested %s), %u of them with "
					"the hair's tint (%u tints noted from the hair draws, %u hair draws seen at draw time); FaceGen face records: %u with the gain from "
					"their maps, %u with the x 2 fallback (perf-mirrorfix2)", c.actorAlphaParts, c.alphaOn ? "on" : "off", c.actorTintedParts,
					(unsigned)RtHairTint::Tints.size(), RtHairTint::DrawNotes, c.faceGains[0], c.faceGains[1]);
			if ((n == 60 || n == 600 || n == 6000) && RtHairTint::Wanted) {	// (2026-11-08 perf-reflfix4) where the hair's tint came from
				char shaders[128] = "";
				for (int k = 0; k < RtHairTint::SkinnedShaderCount; k++) {
					strcat_s(shaders, k ? ", " : ""); strcat_s(shaders, RtHairTint::SkinnedShaders[k]);
				}
				const UInt32 (&hp)[2][4] = c.hairParts;
				Logger::Log("RT CORE hair (perf-reflfix4, exchanges 1-%u): hair-shader draws of the world seen at draw time: %u still objects (rigid), %u skinned "
					"partitions (%u tints noted); pixel shaders of the world's skinned partitions: %s; hair parts in the traced scene (a part a frame): rigid "
					"%u (tint from the shader property %u, a draw %u, the player's colour %u, none %u), skinned %u (%u, %u, %u, %u)", (unsigned)n, RtHairTint::HairDraws[0],
					RtHairTint::HairDraws[1], (unsigned)RtHairTint::Tints.size(), shaders[0] ? shaders : "(none seen)", hp[0][0] + hp[0][1] + hp[0][2] + hp[0][3],
					hp[0][1], hp[0][2], hp[0][3], hp[0][0], hp[1][0] + hp[1][1] + hp[1][2] + hp[1][3], hp[1][1], hp[1][2], hp[1][3], hp[1][0]);
			}
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
	// UNOFFICIAL 2026-11-10 (PT early kick) an open SubmitEarly is collected here at the latest, even when no user exchanges below (the
	// path tracer switched off after its submit: its copy targets may go soon). Normally RtPathTrace::CollectBeforeShow already did.
	if (c.pendingN) c.CollectEarly(Device);
	if (!c.Ready()) return;
	// UNOFFICIAL 2026-10-28: only the Point-0 users exchange here; with none on, a Point-1 user's Exchange gathers the scene itself.
	// (2026-11-10 PT early kick) a user submitted early this frame does not count: alone, it needs no second round trip here
	bool pointZero = false;
	for (const User& user : c.users) pointZero = pointZero || (user.Point == 0 && !c.Out(user) && user.Wants && user.Wants() && !c.EarlyThisFrame(user.Name));
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

bool RtCore::ExchangeEarly(IDirect3DDevice9* Device, const User& user) {	// UNOFFICIAL 2026-11-07 (RtCore.h)
	Core& c = TheCore;
	if (c.grassOn && TheShaderManager->GameState.isExterior) return false;	// this frame's grass is noted by the world's draws
	if (!c.Ready()) return false;
	return Exchange(Device, user);
}

// UNOFFICIAL 2026-11-10 (PT early kick, docs/next/PT-EARLY-KICK-PLAN-2026-11-10.md; RtCore.h). Bench only: unset, nothing below runs.
bool RtCore::EarlyPtWanted() {
	static const bool wanted = getenv("NVR_RT_EARLY_PT") != nullptr;
	static bool told = false;
	if (wanted && !told) {
		told = true;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: NVR_RT_EARLY_PT: the path tracer's exchange is signalled at the world's start and collected before its "
			"show pass (no G-buffer inputs; grass left out of those frames' scene; last frame's lamp list)");
	}
	return wanted;
}

namespace {
	bool Core::EarlyThisFrame(const char* name) const {
		return earlyName && earlyFrame == TheFrameRateManager->FrameNumber + 1 && (earlyName == name || (name && !strcmp(earlyName, name)));
	}

	void Core::CollectEarly(IDirect3DDevice9* Device) {
		if (!pendingN) return;
		const UInt64 n = pendingN;
		pendingN = 0;
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		earlyOverlapMs += 1000.0 * (double)(now.QuadPart - earlySignalQpc) / (double)freq.QuadPart;	// signal -> collect: the overlap window
		earlyCollected++;
		if (state != Running || !share || !outputReady) { pendingOut.clear(); return; }	// (Stop clears it first; belt and braces)
		static GpuTimer timerCollect("  RT early: collect wait");
		const bool timed = timerCollect.Begin(Device);
		share->WaitShared(outputReady, n);
		if (timed) timerCollect.End();
		for (const PendingCopy& copy : pendingOut) {
			const RECT used = { 0, 0, (LONG)(copy.width ? copy.width : imageSizes[copy.image].first),
				(LONG)(copy.height ? copy.height : imageSizes[copy.image].second) };
			share->CopyFromShared(copy.target, 0, 0, images[copy.image], &used);
		}
		pendingOut.clear();
	}
}

bool RtCore::SubmitEarly(IDirect3DDevice9* Device, const User* user) {
	Core& c = TheCore;
	if (!EarlyPtWanted()) return false;
	auto refuse = [&](const char* why) { c.earlyRefused++; c.earlyRefusal = why; return false; };
	auto logEvery300 = [&]() {
		if ((c.earlySubmitted + c.earlyRefused) < 300) return;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT early (NVR_RT_EARLY_PT): %u frames submitted early, %u refused (last: %s); signal -> collect %.3f ms avg CPU "
			"(%u collected)", c.earlySubmitted, c.earlyRefused, c.earlyRefusal, c.earlyCollected ? c.earlyOverlapMs / c.earlyCollected : 0.0, c.earlyCollected);
		c.earlySubmitted = c.earlyRefused = c.earlyCollected = 0;
		c.earlyOverlapMs = 0.0;
	};
	if (c.pendingN) c.CollectEarly(Device);	// one frame in flight (a frame without the show pass left it open)
	bool ok = false;
	if (!user) refuse("no user");
	// (unlike ExchangeEarly, no refusal outdoors with Grass on: Core::AddGrass takes only THIS frame's notes, which the world's draws have not
	// made yet, so an early frame's scene has no grass (last frame's notes are cleared, not used: their geometry may be gone by now))
	else if (!c.Ready()) refuse("helper not ready (starting, off, or the screen size changed)");
	else if (c.Out(*user) || !user->Wants || !user->Wants()) refuse("the user does not trace this frame");
	else if (c.EarlyThisFrame(user->Name)) refuse("already submitted this frame");
	else {
		// The user's Record runs now, before the world's first draw: whatever it reads from the frame's draws is last frame's (the ManyLamps
		// lamp list, built at the first world draw), and it must take no G-buffer input (the path tracer: as with PTPrimary false).
		c.submitOnly = true;
		const UInt64 before = c.frame;
		const bool recorded = c.Exchange(Device, user);
		c.submitOnly = false;
		if (c.frame == before) refuse("helper behind (ring slot not free)");	// Exchange returned before signalling: nothing pending
		else if (!recorded) {
			// signalled with no passes (the user's AddPass refused): paired now, as Exchange would, so nothing is left pending (not an
			// overlap sample)
			const UInt32 collected = c.earlyCollected;
			const double overlap = c.earlyOverlapMs;
			c.CollectEarly(Device);
			c.earlyCollected = collected;
			c.earlyOverlapMs = overlap;
			refuse("the user recorded no pass");
		}
		else {
			c.earlyName = user->Name;
			c.earlyFrame = TheFrameRateManager->FrameNumber + 1;
			c.earlySubmitted++;
			ok = true;
		}
	}
	logEvery300();
	return ok;
}

bool RtCore::EarlyPending() { return TheCore.pendingN != 0; }
void RtCore::CollectEarly(IDirect3DDevice9* Device) { if (TheCore.pendingN) TheCore.CollectEarly(Device); }
bool RtCore::SubmittedEarlyThisFrame(const User* user) { return user && TheCore.EarlyThisFrame(user->Name); }

bool RtCore::Running() { return TheCore.state == Core::Running; }
bool RtCore::OthersWant(const char* Name) {	// UNOFFICIAL 2026-11-07 (ptaaa-fog; RtCore.h)
	for (const User& user : TheCore.users)
		if (user.Name != Name && !(user.Name && Name && !strcmp(user.Name, Name)) && !TheCore.Out(user) && user.Wants && user.Wants()) return true;
	return false;
}
bool RtCore::LeftOut(const char* Name) { for (const char* n : TheCore.leftOut) if (n == Name || (n && Name && !strcmp(n, Name))) return true; return false; }
void RtCore::Restart(const char* Why) {	// UNOFFICIAL 2026-11-02: as a screen-size change (Core::Ready)
	Core& c = TheCore;
	if (c.state == Core::Off || c.state == Core::Idle) return;
	c.Stop(Why);
	c.state = Core::Idle;
}
void RtCore::Shutdown(const char* Why) {	// UNOFFICIAL 2026-11-07 (rt-shutdown; RtCore.h)
	Core& c = TheCore;
	if (c.state == Core::Off && !c.process && !c.shared) return;	// already off (TurnOff stopped it)
	if (c.state != Core::Idle || c.process || c.shared) c.Stop(Why);
	c.state = Core::Off;
	_snprintf_s(c.status, sizeof(c.status), _TRUNCATE, "off: %s", Why);
}
const char* RtCore::StatusText() { return TheCore.status; }
const NvrRtShared* RtCore::Helper() { return TheCore.state == Core::Running ? TheCore.shared : nullptr; }
UInt32 RtCore::Instances() { return TheCore.instances; }
const std::vector<RtCore::GlowLight>& RtCore::GlowLights() { return TheCore.frameGlows; }

// UNOFFICIAL 2026-11-07 (ptaaa-emissive) PTEmissive: a glow instance of the frame being recorded (users' Record) as an emissive light
bool RtCore::EmissiveTransform(UInt32 Record, float Transform[3][4]) {
	const NvrRtFrame* f = TheCore.record;
	if (!TheCore.recording || !f || Record >= f->instanceCount) return false;
	if (!RtScenePick::CanBeEmissive(f->records[Record].flags)) return false;	// alpha-tested: bits 8-15 are taken
	if (f->instances[Record].flags & NVR_RT_INSTANCE_DISPLACE) return false;	// traced through a displaced copy (other triangles)
	memcpy(Transform, f->instances[Record].transform, sizeof(float) * 12);
	return true;
}
bool RtCore::MarkEmissive(UInt32 Record, UInt32 Number) {
	NvrRtFrame* f = TheCore.record;
	if (!TheCore.recording || !f || Record >= f->instanceCount || !RtScenePick::CanBeEmissive(f->records[Record].flags)) return false;
	f->records[Record].flags = RtScenePick::WithEmissive(f->records[Record].flags, Number);
	return true;
}
UInt32 RtCore::Geometries() { return (UInt32)TheCore.geometries.size(); }
UInt32 RtCore::Waiting() { return TheCore.waiting; }
UInt32 RtCore::Released() { return TheCore.released; }
bool RtCore::AlphaTested() { return TheCore.state == Core::Running && TheCore.alphaOn && TheCore.atlasImage >= 0; }
int RtCore::AlphaAtlasImage() { return TheCore.alphaOn || TheCore.hitTextures ? TheCore.atlasImage : -1; }	// shared slots
int RtCore::AlbedoAtlasImage() { return TheCore.state == Core::Running && TheCore.hitTextures ? TheCore.albedoAtlasImage : -1; }
int RtCore::GlowAtlasImage() { return TheCore.state == Core::Running && TheCore.hitTextures ? TheCore.glowAtlasImage : -1; }
int RtCore::NormalAtlasImage() { return TheCore.state == Core::Running && TheCore.hitTextures ? TheCore.normalAtlasImage : -1; }	// UNOFFICIAL 2026-11-02
UInt32 RtCore::HiddenInstances() { return TheCore.hiddenInstances; }	// UNOFFICIAL 2026-11-07 (perf-ptcost; RtCore.h)
bool RtCore::GrassWanted() { return TheCore.state == Core::Running && TheCore.grassOn; }
void RtCore::DisplacedCopies(bool& Land, bool& Objects) {	// UNOFFICIAL 2026-11-07
	const bool on = TheCore.state == Core::Running;
	Land = on && TheCore.dispLand; Objects = on && TheCore.dispObjects;
}
bool RtCore::DecalsTraced() { return TheCore.state == Core::Running && TheCore.decalsOn && TheCore.alphaOn && TheCore.atlasImage >= 0; }	// UNOFFICIAL 2026-11-07

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
