#pragma once
// UNOFFICIAL 2026-11-15 DeferredTransparents: forward+ for the blended (alpha) draws of the deferred renderer (docs/next/FULL-DEFERRED-RENDERER-
// PLAN.md phase 5; results and bench request: docs/next/FD-GLASS-RESULTS.md).
//
// [Main.Main.Quality] DeferredTransparents (restart to compile the records, then live; default 1 since 2026-11-17, settings migration 89 (the
// forward+ walk fix, docs/next/FD-GLASSGLOW.md); 0 before; needs DeferredRenderer >= 1, whose
// prerequisites make the lamp table with its froxel lists and tile rows and the shadow atlas on both runtimes):
//  - Why: glass, bottles, windows and light shades draw in the game's sorted blended list (RenderBlendedHook), after the main list's G-buffer
//    span, so the deferred lamp pass never lights them: they keep the game's own one-pass lamps (the shader's slots: 2 in the ADTS2 / ADTS
//    records, 6 + f with MoreLampsPerObject in the ADTS10 one-pass record on native; the fork's ManyLamps 24 / the froxel's 31 in SLS2034-2036
//    only), and the rest of the room's lamps go missing on them (FD-GLASS-RESULTS.md, the census).
//  - Records (ShaderManager::LoadShader, at load, next to DeferredRenderer::LoadInterior): every lit Object pixel shader with an Interior record
//    (not the added-light, light-only or hair passes) gets one more from the same Interior overrides + LAMPS_DEFERRED 4 (ObjectTemplate.hlsl
//    NVR_FORWARD_PLUS, Includes/ForwardPlus.hlsl): the game's lamps out, the sun / cell light slot kept, every lamp of the pixel's froxel from
//    the table added, each shadowed from its atlas tile. If 4 does not load, 5 (the same unshadowed). Own cache folder.
//  - Choice (NiD3DPixelShaderEx::SetupShader, Choose): indoors, inside the blended list (ListScope, RenderBlendedHook), in the world context
//    (BeforeBind: not reflections, first person, an extra camera or menus: their cameras are not the table's), while the table made this frame
//    holds this frame's froxel lists and is unlocked (LampTableThread: FinishTable at the list's start). Anything else: the record chosen as
//    before.
//  - Bind (OnBind, after the game's bind): the table on s3 (point), the atlas on s8 (linear), c180 / c181 the lamp shadows' parameters
//    (PointShadowForward's own registers and values; its cache is kept in step).
//  - Safety net (Skip / AfterDraw): an object whose base pass drew with a forward+ record skips its own lamp passes (and highlight passes after
//    a SPECULAR base) for the frame, as the deferred renderer does for its deferred records; the census found none in the blended list.
// Off (0): no record is made, nothing chosen, bound, written into the table or skipped.
// UNOFFICIAL 2026-11-16 fd-fwdgroups (docs/next/FD-FORWARD-GROUPS.md): the same froxel walk for every group the lamp pass cannot reach.
//  - Outdoors too (mode 2): every eligible shader gets a second record from its DEFAULT overrides (the sun and its shadows kept) +
//    LAMPS_DEFERRED 4, cache Deferred\GlassExterior\; chosen outdoors while the renderer makes the exterior table (the lamp pass lights the
//    streets at night from it). Before, outdoor glass kept the game's few lamps.
//  - The flashlight: ForwardPlusHeader's texels 5-7 (FlashlightDeferred::LightParams); the forward+ records add its cone. Before, a forward+
//    pane lost the flashlight entirely (the table leaves its game lamp out, the record compiles the draw's lamps out).
//  - The game's blended hair (namespace Hair below, [Main.Main.Quality] DeferredHair): SM3002 / SM3003.pso get a HAIR_FORWARD_PLUS record,
//    chosen in the same blended list (Includes/HairForwardPlus.hlsl).
//  - The deferred leaves' lamps (core/DeferredFoliage.h LampRecords, [Main.Main.Quality] DeferredFoliageLamps): bound here (OnBind).
// The hair and leaf records read the atlas on s12 (Includes/ForwardPlus.hlsl FP_OWN_SHADOW: s8 is the clouds' shadow in those shaders).
#include "DeferredRenderer.h"
#include "ForwardPlusHeader.h"
#include "FlashlightDeferred.h"
#include "DeferredFoliage.h"

namespace DeferredTransparents {
	inline const char* kSection = "Main.Main.Quality";
	inline const char* kKey = "DeferredTransparents";
	inline const char* kCache = "Deferred\\Glass\\";

	inline int Wanted = -1;
	inline bool WantedAtStartup() {
		if (Wanted < 0) Wanted = DeferredRenderer::On() && TheSettingManager->GetSettingI(kSection, kKey) != 0 ? 1 : 0;
		return Wanted != 0;
	}

	inline std::unordered_map<const void*, ShaderRecordPixel*> Records;	// NiD3DPixelShaderEx* -> its forward+ record
	inline std::unordered_map<const void*, ShaderRecordPixel*> RecordsExterior;	// (2026-11-16) -> its outdoor forward+ record (default overrides)
	inline std::unordered_set<IDirect3DPixelShader9*> Handles, SpecularHandles, ExteriorHandles;
	inline unsigned Loaded = 0, Unshadowed = 0, Refused = 0, LoadedExterior = 0, RefusedExterior = 0;
	inline bool GlassLive = false;		// this frame: the glass records may be chosen (Live: glass or the hair's)

	// (2026-11-16) the flashlight for ForwardPlusHeader's texels 5-7: FlashlightDeferred::LightParams (camera-relative, as the header's camera),
	// only while the light pass lights this frame's world (its game lamp is then out of the table)
	inline bool FlashlightRows(float out[3][4]) {
		D3DXVECTOR4 params[3];
		if (!FlashlightDeferred::Usable() || !FlashlightDeferred::LightParams(params)) return false;
		for (int k = 0; k < 3; k++) { out[k][0] = params[k].x; out[k][1] = params[k].y; out[k][2] = params[k].z; out[k][3] = params[k].w; }
		return true;
	}
	inline void EnableHeader() {
		ForwardPlusHeader::Enabled = true;
		ForwardPlusHeader::Flashlight = &FlashlightRows;
	}

	inline bool Live = false;			// this frame (BeginFrame): on, indoors, the table and its lists possible
	inline bool InList = false;			// the blended list is drawing (ListScope)
	inline bool ListReady = false;		// ... and this frame's table and lists are there and unlocked (checked at the list's start)
	inline bool BindWorld = false;		// the bind under way is in the world context (BeforeBind)
	inline bool BoundOurs = false, BoundSpecular = false;
	inline UInt32 FrameId = 1;

	// F10 line
	inline unsigned StatFrames = 0;
	inline unsigned long long StatBinds = 0, StatDraws = 0, StatLists = 0, StatListsRefused = 0, StatOtherContext = 0, StatSkipped = 0;

	// The Object template's lit base / one-pass records (DeferredRenderer::Eligible's families less the light-only AD passes: their light is
	// multiplied by the texture pass afterwards, and the census found none blended).
	inline bool Eligible(const char* name, const ShaderTemplate& tmpl) {
		if (!tmpl.Name || strcmp(tmpl.Name, "ObjectTemplate") || !DeferredRenderer::Eligible(name, tmpl, 1)) return false;
		return !DeferredRenderer::HasDefine(tmpl, "ONLY_LIGHT");
	}

	// ShaderManager::LoadShader, where a lit shader's level-0 Interior record loaded (overrides: that record's Interior overrides).
	inline void Load(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl, const D3DXMACRO* interiorOverrides) {
		if (!WantedAtStartup() || !Eligible(name, tmpl) || Records.count(shader)) return;
		static D3DXMACRO overrides[14] = {};
		int n = 0;
		for (const D3DXMACRO* o = interiorOverrides; o && o->Name && n < 12; o++) overrides[n++] = *o;
		// (2026-11-16) outdoors: the same from the default record's overrides (none: the sun and its shadows kept), mode 2 only (mode 1 is
		// interiors only)
		if (DeferredRenderer::Mode() >= 2 && !RecordsExterior.count(shader)) {
			ShaderRecordPixel* outdoor = nullptr;
			for (int level = 4; level <= 5 && !outdoor; level++) {
				static D3DXMACRO o[2] = {};
				o[0] = { "LAMPS_DEFERRED", level == 4 ? "4" : "5" };
				o[1] = { NULL, NULL };
				ShaderTemplate copy = tmpl;
				outdoor = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, copy, level == 4 ? "Deferred\\GlassExterior\\" : "Deferred\\GlassExteriorUnshadowed\\", o);
			}
			if (outdoor) {
				RecordsExterior[shader] = outdoor;
				Handles.insert(outdoor->ShaderHandle);
				ExteriorHandles.insert(outdoor->ShaderHandle);
				if (DeferredRenderer::HasDefine(tmpl, "SPECULAR")) SpecularHandles.insert(outdoor->ShaderHandle);
				LoadedExterior++;
			}
			else { RefusedExterior++; Logger::Log("[WARNING] DEFERRED TRANSPARENTS outdoor record %s: did not load: its blended draws outdoors keep the game's lamps", name); }
		}
		ShaderRecordPixel* record = nullptr;
		for (int level = 4; level <= 5 && !record; level++) {
			overrides[n] = { "LAMPS_DEFERRED", level == 4 ? "4" : "5" };
			overrides[n + 1] = { NULL, NULL };
			ShaderTemplate copy = tmpl;	// (LoadShader appends the global defines to the template it is given)
			record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, copy, level == 4 ? kCache : "Deferred\\GlassUnshadowed\\", overrides);
			if (!record && level == 4) Logger::Log("[WARNING] DEFERRED TRANSPARENTS record %s: the shadowed version did not load; trying it without lamp shadows", name);
			if (record && level == 5) Unshadowed++;
		}
		if (!record) { Refused++; Logger::Log("[WARNING] DEFERRED TRANSPARENTS record %s: did not load: its blended draws keep the game's lamps (%u loaded, %u not)", name, Loaded, Refused); return; }
		Records[shader] = record;
		Handles.insert(record->ShaderHandle);
		if (DeferredRenderer::HasDefine(tmpl, "SPECULAR")) SpecularHandles.insert(record->ShaderHandle);
		EnableHeader();
		Loaded++;
		Logger::Log("DEFERRED TRANSPARENTS record %s: loaded (%u loaded, %u of them unshadowed, %u not so far)", name, Loaded, Unshadowed, Refused);
	}

	// --- (2026-11-16 fd-fwdgroups) the game's blended hair, brows and beards: SM3002 / SM3003.pso's forward+ record (Includes/HairForwardPlus.hlsl)
	// [Main.Main.Quality] DeferredHair (restart; with the renderer and G-buffer v2: SM3003.vso then hands over the world frame) makes it;
	// DeferredHairLive (live, test only, default true) is its one-launch A/B. Chosen in the sorted blended list, in the world context, with
	// this frame's table, indoors and (mode 2) outdoors. The atlas on s12.
	namespace Hair {
		inline std::unordered_map<const void*, ShaderRecordPixel*> Records;
		inline std::unordered_set<IDirect3DPixelShader9*> Handles;
		inline unsigned Loaded = 0, Refused = 0;
		inline bool Live = false;
		inline unsigned long long StatBinds = 0;
		inline int Wanted = -1;
		inline bool WantedAtStartup() {
			if (Wanted < 0) {
				const bool key = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredHair") != 0;
				Wanted = key && DeferredRenderer::On() && CapturedNormals::V2CompiledIn() ? 1 : 0;
			}
			return Wanted != 0;
		}
		inline bool IsHair(const char* name) { return name && (!_stricmp(name, "SM3002.pso") || !_stricmp(name, "SM3003.pso")); }
		// ShaderManager::LoadShader (pixel), after the shader's own records
		inline void Load(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl) {
			if (!shader || !IsHair(name) || !shader->ShaderProg[ShaderRecordType::Default] || !WantedAtStartup() || Records.count(shader)) return;
			static const D3DXMACRO overrides[2] = { { "HAIR_FORWARD_PLUS", "1" }, { NULL, NULL } };
			ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, "Deferred\\Hair\\", overrides);
			if (!record || !record->ShaderHandle) {
				Refused++;
				Logger::Log("[WARNING] DEFERRED HAIR forward+ record %s: did not load (see the device's line above): the game's hair keeps its own lamps", name);
				return;
			}
			Records[shader] = record;
			Handles.insert(record->ShaderHandle);
			EnableHeader();
			Loaded++;
			Logger::Log("DEFERRED HAIR forward+ record %s: loaded (the game's blended hair lit by every lamp of its froxel; %u loaded, %u not)", name, Loaded, Refused);
		}
	}

	// Once a frame (Render.cpp, after DeferredRenderer::BeginFrame).
	inline void BeginFrame() {
		FrameId++;
		{	// (2026-11-16) the hair's forward+: with the renderer live (indoors; outdoors in mode 2) and the table's lists and tile rows made
			const bool exterior = TheShaderManager->GameState.isExterior;
			Hair::Live = Hair::WantedAtStartup() && !Hair::Records.empty() && DeferredRenderer::Live && (!exterior || DeferredRenderer::Mode() >= 2) &&
				TheSettingManager->GetSettingI(kSection, "DeferredHairLive") != 0 && ManyLamps::FroxelRows() && ManyLamps::TileRowsWanted() && !ManyLamps::TableFailed;
		}
		if (!WantedAtStartup() || Records.empty()) { GlassLive = false; Live = Hair::Live; return; }
		static bool told = false;
		if (!told) {
			told = true;
			Logger::Log("UNOFFICIAL DeferredTransparents: %u forward+ records (%u unshadowed), %u did not load; the lamp table %s, its tile rows %s, the atlas %s",
				Loaded, Unshadowed, Refused, ManyLamps::FroxelRows() ? "with froxel lists" : "WITHOUT froxel lists (nothing to read: off)",
				ManyLamps::TileRowsWanted() ? "made" : "NOT made (no header: off)", PointShadowForward::AtlasRuns() ? "made" : "not made (unshadowed)");
		}
		// (2026-11-16) outdoors too, in mode 2 with the outdoor records (the renderer live: the lamp pass makes the exterior table)
		const bool exterior = TheShaderManager->GameState.isExterior;
		GlassLive = TheSettingManager->GetSettingI(kSection, kKey) != 0 && (!exterior || (DeferredRenderer::Mode() >= 2 && DeferredRenderer::Live && !RecordsExterior.empty())) &&
			ManyLamps::FroxelRows() && ManyLamps::TileRowsWanted() && !ManyLamps::TableFailed;
		Live = GlassLive || Hair::Live;
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED TRANSPARENTS %u frames, glass %s, hair %s: blended lists %.2f a frame (%.2f without this frame's table: the game's lamps), forward+ binds %.1f (hair %.1f), draws %.1f, "
			"lamp / highlight passes skipped %.2f, binds left alone outside the world context %.1f; header writes %u (%u with lists); records %u indoor, %u outdoor, %u hair",
			StatFrames, GlassLive ? "live" : "off", Hair::Live ? "live" : "off", StatLists / f, StatListsRefused / f, StatBinds / f, Hair::StatBinds / f, StatDraws / f, StatSkipped / f, StatOtherContext / f,
			ForwardPlusHeader::Writes, ForwardPlusHeader::ValidWrites, Loaded, LoadedExterior, Hair::Loaded);
		StatFrames = 0; StatBinds = StatDraws = StatLists = StatListsRefused = StatOtherContext = StatSkipped = 0; Hair::StatBinds = 0;
		ForwardPlusHeader::Writes = ForwardPlusHeader::ValidWrites = 0;
	}

	// RenderBlendedHook: the sorted blended list. At its start the table made at this frame's first world draw is finished (LampTableThread's
	// binding waited for and the table unlocked: the blended draws read it) and checked; a table of another frame, or one without this frame's
	// froxel lists (no world draw made them), is not read: every draw keeps its forward record.
	struct ListScope {
		bool was;
		ListScope() : was(InList) {
			InList = true;
			ListReady = false;
			if (!Live) return;
			ManyLamps::FinishTable();
			ListReady = ManyLamps::Table && !ManyLamps::TableFailed && ManyLamps::TableFrame == ManyLamps::FrameId &&
				ManyLamps::ListsFrame == ManyLamps::FrameId && !ManyLamps::TableJob.pending;
			StatLists++;
			if (!ListReady) StatListsRefused++;
		}
		~ListScope() { InList = was; if (!was) ListReady = false; }
	};

	// SetShadersHook, before the game's bind (SetupShader runs inside it): the context of this bind.
	inline void BeforeBind(bool world) { BindWorld = world; }

	// NiD3DPixelShaderEx::SetupShader.
	inline ShaderRecordPixel* Choose(const void* shader) {
		if (!Live || !InList || !ListReady) return nullptr;
		if (Hair::Live && !Hair::Records.empty()) {	// (2026-11-16) the game's blended hair
			auto hair = Hair::Records.find(shader);
			if (hair != Hair::Records.end()) {
				if (!BindWorld) { StatOtherContext++; return nullptr; }
				Hair::StatBinds++;
				return hair->second;
			}
		}
		if (!GlassLive) return nullptr;
		const bool exterior = TheShaderManager->GameState.isExterior;
		const auto& map = exterior ? RecordsExterior : Records;
		auto found = map.find(shader);
		if (found == map.end()) return nullptr;
		if (!BindWorld) { StatOtherContext++; return nullptr; }
		StatBinds++;
		return found->second;
	}

	// SetShadersHook, after the game's bind: a forward+ record bound gets the table, the atlas and the lamp shadows' parameters.
	inline void OnBind(IDirect3DPixelShader9* handle) {
		BoundOurs = Live && handle && Handles.count(handle) != 0;
		BoundSpecular = BoundOurs && SpecularHandles.count(handle) != 0;
		// (2026-11-16) the hair's and the deferred leaves' forward+ records: the atlas on s12 (their s8 is the clouds' shadow)
		const bool own = handle && ((Hair::Live && Hair::Handles.count(handle) != 0) || (DeferredFoliage::LampsLive && DeferredFoliage::LampHandles.count(handle) != 0));
		if (!BoundOurs && !own) return;
		PointShadowForward::Bind(3, ManyLamps::Table, true, true);
		// Outdoor forward+ records (RecordsExterior) compile SHADOW_CLOUD_SAMPLE with TESR_CloudShadow on s8 and have no lamp shadow
		// atlas (TABLE_LAMP_SHADOWS_ON is unset without INTERIOR_SHADOWS); overwriting s8 with ManyLamps::Table or atlas blew out
		// CloudSunAt() on outdoor alpha-blended meshes (Issue 2, ForwardShadows ON).
		if (!(BoundOurs && ExteriorHandles.count(handle) != 0)) {
			PointShadowForward::Bind(own ? 12 : 8, PointShadowForward::AtlasTexture ? (IDirect3DBaseTexture9*)PointShadowForward::AtlasTexture : (IDirect3DBaseTexture9*)ManyLamps::Table, true, false);
			float k[2][4] = {};
			if (PointShadowForward::Active && PointShadowForward::AtlasTexture) memcpy(k[0], PointShadowForward::Params, sizeof(k[0]));	// x 0: TableLampShadow lights every lamp
			k[1][1] = PointShadowForward::NearFade;
			k[1][3] = PointShadowForward::Quality;
			TheRenderManager->device->SetPixelShaderConstantF(180, &k[0][0], 2);
			memcpy(PointShadowForward::Uploaded[6], k, sizeof(k));	// (its cache of c174-c189 in step: c180, c181)
		}
	}

	// Objects whose base pass drew with a forward+ record this frame (open addressing tagged with the frame, as DeferredRenderer's).
	constexpr unsigned kSlots = 4096;
	inline const void* Key[kSlots] = {};
	inline UInt32 Frame[kSlots] = {};
	inline bool Spec[kSlots] = {};
	inline int Find(const void* geometry, bool insert) {
		unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kSlots - 1);
		for (unsigned probe = 0; probe < 32; probe++, h = (h + 1) & (kSlots - 1)) {
			if (Frame[h] == FrameId && Key[h] == geometry) return (int)h;
			if (Frame[h] != FrameId) {
				if (!insert) return -1;
				Key[h] = geometry; Frame[h] = FrameId; Spec[h] = false;
				return (int)h;
			}
		}
		return -1;
	}

	// Render.cpp's pass hooks, early: the lamp passes (and after a SPECULAR base the highlight passes) of an object lit by forward+ this frame.
	inline bool Skip(const void* passPtr, bool world) {
		if (!Live || !InList || !world || !passPtr) return false;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
		const PassSums::Kind kind = PassSums::KindOf(pass->passEnum);
		if (kind == PassSums::None) return false;
		const int slot = Find(pass->geometry, false);
		if (slot < 0 || (kind == PassSums::Highlights && !Spec[slot])) return false;
		StatSkipped++;
		return true;
	}

	// Render.cpp's pass hooks, after the game drew the pass.
	inline void AfterDraw(const void* passPtr, bool world) {
		if (!BoundOurs || !InList || !world || !passPtr) return;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
		StatDraws++;
		if (PassSums::KindOf(pass->passEnum) != PassSums::None || !pass->geometry) return;
		const int slot = Find(pass->geometry, true);
		if (slot >= 0) Spec[slot] = BoundSpecular;
	}
}
