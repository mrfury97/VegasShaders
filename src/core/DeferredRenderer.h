#pragma once
// UNOFFICIAL 2026-11-14 full deferred renderer, phase 2: interiors fully deferred on both runtimes (docs/next/FULL-DEFERRED-RENDERER-PLAN.md
// sections 4.1, 4.4, 4.5, 4.8 and phase 2; results and bench request: docs/next/FD-INTERIORS-RESULTS.md).
//
// [Main.Main.Quality] DeferredRenderer (restart): 0 off, 1 = the lamps deferred, 2 = all direct light deferred (the default since 2026-11-15).
//  - Prerequisites: turned on by the switch itself (core/SettingForce.h: CapturedNormals, MaterialBuffer, AlbedoBuffer, linear lighting +
//    LinearScene, FoldFog, forward lamp shadows + the atlas, LampTableStandalone + LampTableFast, DeferredLamps with every lamp lit, and in
//    mode 2 the lighting-free records). Refused (forward, one log line) under the game's MSAA on native D3D9 (no G-buffer there), and in
//    mode 2 while the DeferredSun pass cannot run (DeferredSun::Wanted).
//  - Records (ShaderManager::LoadShader, at load): mode 1 = LAMPS_DEFERRED 3 (ObjectTemplate.hlsl NVR_LAMPS_OUT: every lamp out, the sun /
//    cell light kept) for every lit Object and Parallax pixel shader, from the Interior overrides where the name has an Interior record;
//    mode 2 = the lighting-free records (LAMPS_DEFERRED 2, core/LightFreeRecords.h) of the Object, Parallax and Terrain shaders. Both write
//    the deferred mark (COLOR2.a 1.01..1.99) the lamp pass lights. (2026-11-18 DeferredStandalone) mode 2 also for a shader whose forward
//    records did not load (too long for ps_3_0): the game's own shader is then its forward partner (ForwardOf, the blended-draw swap).
//    Not: hair (HAIR), the game's added-light passes (DIFFUSE, ONLY_SPECULAR),
//    the unlit pass (NO_LIGHT); skin, eyes, first person, reflections and everything after the main list stay forward (other templates or
//    other contexts: the records are chosen only inside the main list's G-buffer span, CapturedNormals::InSpan).
//  - Selection (NiD3DPixelShaderEx::SetupShader, Choose): indoors (mode 2: outdoors too while DeferredSun runs) while the G-buffer is bound;
//    a blended base pass (fading in / out, alpha-blended; not the land's replace-fade) takes the forward record and keeps its own lamp passes
//    (BeforeDraw / Choose, 2026-11-14 fd-fixes).
//  - Pass skipping (Render.cpp's pass hooks, Skip): an object whose base pass drew with a deferred record skips the game's DIFFUSE lamp passes
//    and highlight passes (PassSums::Lamps / Highlights): the lamp pass adds that light. The TEXTURE multiply and the fog pass of a light-only
//    (AD) base stay: the AD pass's vertex shader carries no fog, so the record cannot fog itself (FD-INTERIORS-RESULTS.md "decided").
//  - Native D3D9: the lamp table and the atlas are made for the pass (ManyLamps::DeferredAtStartup, PointShadowForward::AtlasRuns), and the
//    game draws shiny objects with up to 23 + f lamps in one pass while the renderer is live indoors (LampsInOnePass, OnePassWanted).
// [Main.Main.Quality] DeferredRendererLive (live, test only): the A/B for ParityTest (`parity Main.Main.Quality DeferredRendererLive`): false =
// every draw forward as before (the prerequisites stay on).
// Off (0): nothing is loaded, chosen, skipped or patched; every template and record as before.
#include "DeferredLamps.h"
#include "LightFreeRecords.h"
#include "PassSums.h"
#include "OnePassLimit.h"	// 2026-11-14 fd-fixes: the per-object one-pass limit (Keep)
#include "SettingForce.h"
#include <unordered_set>
// UNOFFICIAL 2026-11-14 DeferredSun (merged from claude/fd-sun, docs/next/FD-SUN-RESULTS.md): mode 2 forces it (DeferredSun::SetForced) and reads
// whether it runs (DeferredSun::Wanted); its Apply is called in Render.cpp's DeferredSpan, after this file's AtSpanEnd.
#include "DeferredSun.h"
#include "DeferredFoliage.h"	// UNOFFICIAL 2026-11-14 phase 3 foliage: its live state follows mode 2's (BeginFrame)
#include "LateInteriors.h"	// UNOFFICIAL 2026-11-16 fast-restart (InteriorRecordCensus, log only)

namespace DeferredRenderer {
	inline const char* kSection = "Main.Main.Quality";
	inline const char* kCache = "Deferred\\All\\";	// mode 1's records (LAMPS_DEFERRED 3), their own cache folder

	inline int Mode() { return SettingForce::Mode(); }
	inline bool On() { return Mode() > 0; }

	// Mode 2's sun (DeferredSun): live only while its pass can run; mode 1 never forces it (its records keep the sun).
	inline bool SunRuns() { return DeferredSun::Wanted(); }
	inline void SunForce(bool on) { DeferredSun::SetForced(on); }
	// G-buffer v2 (forced on by the switch, SettingForce.h): the lamp pass reads each pixel's specular bit (DeferredLamps.hlsl DL_LAMP_LOOP, PBR or PBRDiffuse per pixel).
	inline bool GBufferV2() { return CapturedNormals::V2CompiledIn(); }

	inline std::unordered_map<const void*, ShaderRecordPixel*> Records;	// NiD3DPixelShaderEx* -> its deferred record
	inline std::unordered_set<IDirect3DPixelShader9*> Handles;			// every deferred record's handle
	inline std::unordered_set<IDirect3DPixelShader9*> PlainHandles;		// ... of a record without SPECULAR (the light-only AD bases, plain objects)
	inline bool BoundPlain = false;
	inline std::unordered_set<const void*> Missing;						// eligible shaders whose deferred record did not load
	inline unsigned Loaded = 0, Refused = 0;
	// 2026-11-14 fd-fixes (blended draws): every deferred handle -> its record and its forward record (the per-draw swap below), and the
	// land's (TerrainTemplate, mode 2) handles: the land's fade over objects is a SRCALPHA blend with alpha 1, a plain replace, so it stays deferred
	// UNOFFICIAL 2026-11-19: outdoor partner (Exterior, else Default — FORWARD_SHADOWS = 1). Swapping an alpha-blended outdoor object
	// (cracked asphalt road slabs, rubble skirts, decals) to the Interior record (FORWARD_SHADOWS = 0) stripped all sun shadows from it.
	struct Pair { ShaderRecordPixel* deferred; ShaderRecordPixel* forward; ShaderRecordPixel* outdoor = nullptr; };
	inline ShaderRecordPixel* OutdoorRecord(const NiD3DPixelShaderEx* shader) {
		ShaderRecordPixel* r = shader ? (ShaderRecordPixel*)shader->ShaderProg[ShaderRecordType::Exterior] : nullptr;
		if (!r && shader) r = (ShaderRecordPixel*)shader->ShaderProg[ShaderRecordType::Default];
		return r && r->ShaderHandle ? r : nullptr;
	}
	inline ShaderRecordPixel* PartnerOf(const Pair& pair) {
		return pair.outdoor && TheShaderManager && TheShaderManager->GameState.isExterior ? pair.outdoor : pair.forward;
	}
	inline std::unordered_map<IDirect3DPixelShader9*, Pair> Pairs;
	inline std::unordered_set<IDirect3DPixelShader9*> LandHandles;

	inline bool Live = false;			// this frame
	inline bool OnePass = false;		// this frame: the one-pass compare patch wanted (LampsInOnePass::BeginFrame)
	inline int OnePassMode = 1;			// [Main.Main.Quality] DeferredRendererOnePass (live, test only): 0 none, 1 all but emissive objects (OnePassLimit.h), 2 all
	inline bool BoundDeferred = false;	// the pixel shader bound now is a deferred record (OnBind)
	inline bool SpanSeen = false;		// a world G-buffer span has run in this session (the one-pass patch waits for it: MSAA known by then)
	inline bool Native = false;
	inline UInt32 FrameId = 1;

	// F10 line
	inline unsigned StatFrames = 0;
	inline unsigned long long StatBinds = 0, StatFallbacks = 0, StatSwaps = 0, StatSkippedLamps = 0, StatSkippedHighlights = 0, StatKeptHighlights = 0, StatMarked = 0,
		StatForwardDraws = 0, StatMissedLampDraws = 0, StatMissedLamps = 0, StatLiveFrames = 0, StatBlended = 0, StatBlendedSwaps = 0,
		StatSlotDrawsIn = 0, StatSlotLampsIn = 0, StatSlotDrawsOut = 0, StatSlotLampsOut = 0;
	inline bool BlendedNow = false;							// this draw: a blended base pass in the span (BeforeDraw .. AfterDraw)
	// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/DeferredFirstPerson.h): while its view-model span is open, the player's first-person node;
	// a draw of that render whose geometry is not under it (the HUD's 3D objects drawn with another camera) takes its forward record, as a
	// blended draw does (BeforeDraw). Null otherwise (always without the switch).
	inline NiNode* ViewModelRoot = nullptr;
	inline bool OutsideViewModel(const NiAVObject* geometry) {
		if (!ViewModelRoot) return false;
		for (int depth = 0; geometry && depth < 64; depth++, geometry = geometry->m_parent) if (geometry == ViewModelRoot) return false;
		return true;
	}
	inline IDirect3DPixelShader9* SwappedFrom = nullptr;	// this draw: the deferred handle BeforeDraw swapped for its forward one

	// Objects whose base pass drew with a deferred record this frame (open addressing, tagged with the frame: no clearing; FoldHighlights' pattern)
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 15): 32,768 slots (8,192 before) and the insert failures counted (StatMarkFailed,
	// on the F10 line): an object that could not be marked keeps its own lamp passes AND is lit by the lamp pass (lit twice). With 64 probes
	// a failure needs a run of 64 taken slots, which at 8,192 slots began to be likely at a few thousand objects a frame; now ~4x that.
	constexpr unsigned kSlots = 32768;	// (a power of two: the hash masks with kSlots - 1)
	static_assert((kSlots & (kSlots - 1)) == 0, "kSlots must be a power of two");
	inline unsigned long long StatMarkFailed = 0;
	inline const void* Key[kSlots] = {};
	inline UInt32 Frame[kSlots] = {};
	inline bool Plain[kSlots] = {};		// the object's base record has no SPECULAR (see Skip)
	inline bool* PlainOf = nullptr;		// (Marked: the slot found)
	inline bool Marked(const void* geometry, bool insert) {
		unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kSlots - 1);
		for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kSlots - 1)) {
			if (Frame[h] == FrameId && Key[h] == geometry) { PlainOf = &Plain[h]; return true; }
			if (Frame[h] != FrameId) {
				if (!insert) return false;
				Key[h] = geometry; Frame[h] = FrameId; Plain[h] = false;
				PlainOf = &Plain[h];
				return true;
			}
		}
		if (insert) StatMarkFailed++;	// (2026-11-16) a mark that failed (not a lookup that found nothing)
		return false;	// table full: the object keeps its passes (drawn twice is wrong, so: not marked = its lamp passes drawn AND lit by the pass;
						// counted in StatMarkFailed)
	}

	// Which pixel shaders get a deferred record (the lit records of the G-buffer templates; mode 2 adds the land).
	inline bool Eligible(const char* name, const ShaderTemplate& tmpl, int mode) {
		if (!name || !tmpl.Name) return false;
		const size_t length = strlen(name);
		if (length < 4 || strcmp(name + length - 4, ".pso")) return false;
		const bool object = !strcmp(tmpl.Name, "ObjectTemplate"), parallax = !strcmp(tmpl.Name, "ParallaxTemplate"), land = !strcmp(tmpl.Name, "TerrainTemplate");
		// UNOFFICIAL 2026-11-14 DeferredDistant (core/DeferredDistant.h): the far land too in mode 2, with its G-buffer compiled in (its lighting-free
		// record: LightFreeRecords::Eligible); it binds inside the main list's span, where DeferredDistant draws the game's geometry group 0
		const bool farLand = (!strcmp(tmpl.Name, "TerrainLODTemplate") || !strcmp(tmpl.Name, "TerrainFadeTemplate")) && DeferredDistant::CompiledIn();	// (2026-11-15 DeferredSun: + the hand-over band, its sun taken out with the land's under it)
		if (!object && !parallax && !(land && mode >= 2) && !(farLand && mode >= 2)) return false;
		for (int d = 0; d < ShaderTemplate::kMaxDefines - 1 && tmpl.Defines[d].Name; d++) {
			const char* define = tmpl.Defines[d].Name;
			if (!strcmp(define, "DIFFUSE") || !strcmp(define, "ONLY_SPECULAR") || !strcmp(define, "NO_LIGHT") || (!strcmp(define, "HAIR") && !LightFreeRecords::HairDeferred())) return false;	// (2026-11-15 DeferredHair)
		}
		return true;
	}

	inline bool HasDefine(const ShaderTemplate& tmpl, const char* define) {
		for (int d = 0; d < ShaderTemplate::kMaxDefines - 1 && tmpl.Defines[d].Name; d++) if (!strcmp(tmpl.Defines[d].Name, define)) return true;
		return false;
	}
	// gameHandle (2026-11-18 DeferredStandalone): with no forward record, the game's own shader takes the forward record's place in the span-end
	// swap and for blended draws (BeforeDraw), so the lighting-free record never draws outside the G-buffer span
	inline void Add(const void* shader, ShaderRecordPixel* record, ShaderRecordPixel* forward, const char* name, const ShaderTemplate& tmpl, IDirect3DPixelShader9* gameHandle = nullptr) {
		Records[shader] = record;
		Handles.insert(record->ShaderHandle);
		if (!HasDefine(tmpl, "SPECULAR")) PlainHandles.insert(record->ShaderHandle);
		if (forward && forward->ShaderHandle) DeferredLamps::ForwardOf[record->ShaderHandle] = forward->ShaderHandle;	// the span-end swap (critic item 4)
		else if (gameHandle) {
			gameHandle->AddRef();	// held for the session: the swaps bind it after load, and the game may let go of its own reference
			DeferredLamps::ForwardOf[record->ShaderHandle] = gameHandle;
		}
		ShaderRecordPixel* outdoor = OutdoorRecord((const NiD3DPixelShaderEx*)shader);
		if (outdoor && outdoor->ShaderHandle) DeferredLamps::ForwardOfOutdoor[record->ShaderHandle] = outdoor->ShaderHandle;
		Pairs[record->ShaderHandle] = { record, forward && forward->ShaderHandle ? forward : nullptr, outdoor };
		if (tmpl.Name && !strcmp(tmpl.Name, "TerrainTemplate")) LandHandles.insert(record->ShaderHandle);
		Missing.erase(shader);
		Loaded++;
		Logger::Log("DEFERRED RENDERER record %s: loaded (%u loaded, %u not so far)", name, Loaded, Refused);
	}
	inline void NoteRefused(const void* shader, const char* name) {
		Missing.insert(shader);
		Refused++;
		Logger::Log("[WARNING] DEFERRED RENDERER record %s: did not load (see the device's line above): it stays forward; %u loaded, %u not so far", name, Loaded, Refused);
	}

	// ShaderManager::LoadShader, where a lit shader's level-0 Interior record loaded (overrides: that record's Interior overrides): mode 1's
	// record from the same overrides + LAMPS_DEFERRED 3. lampFalloff: the Interior records' LAMP_FALLOFF_ON override, for the lamp pass.
	inline void LoadInterior(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl, const D3DXMACRO* interiorOverrides, ShaderRecordPixel* forward) {
		if (Mode() != 1 || !Eligible(name, tmpl, 1) || Records.count(shader)) return;
		static D3DXMACRO overrides[12] = {};
		int n = 0;
		for (const D3DXMACRO* o = interiorOverrides; o && o->Name && n < 10; o++) {
			overrides[n++] = *o;
			if (!strcmp(o->Name, "LAMP_FALLOFF_ON")) DeferredLamps::LampFalloffOverride = o->Definition;
		}
		overrides[n++] = { "LAMPS_DEFERRED", "3" };
		overrides[n] = { NULL, NULL };
		ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, kCache, overrides);
		if (record) Add(shader, record, forward, name, tmpl); else NoteRefused(shader, name);
	}

	// ShaderManager::LoadShader, after the shader's own records and LightFreeRecords::Load: mode 2 takes the lighting-free record; mode 1 makes
	// LAMPS_DEFERRED 3 alone for an eligible name that got no Interior record above. Then both light passes are compiled (at load, never on the
	// render thread).
	inline void Load(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl) {
		const int mode = Mode();
		if (!mode || !Eligible(name, tmpl, mode)) return;
		// (2026-11-18 DeferredStandalone, LightFreeRecords.h) mode 2 with no forward record: the lighting-free record alone, if one was made
		ShaderRecordPixel* forward = (ShaderRecordPixel*)(shader->ShaderProg[ShaderRecordType::Interior] ? shader->ShaderProg[ShaderRecordType::Interior] :
			shader->ShaderProg[ShaderRecordType::Default] ? shader->ShaderProg[ShaderRecordType::Default] : shader->ShaderProg[ShaderRecordType::Exterior]);
		const bool forwardLoaded = forward != nullptr;
		if (!forwardLoaded && !(mode >= 2 && LightFreeRecords::Records.count(shader) && shader->ShaderHandleBackup)) return;
		if (mode >= 2) {
			auto found = LightFreeRecords::Records.find(shader);
			if (found != LightFreeRecords::Records.end()) Add(shader, found->second, forward, name, tmpl, forwardLoaded ? nullptr : (IDirect3DPixelShader9*)shader->ShaderHandleBackup);
			else NoteRefused(shader, name);
		}
		else if (!Records.count(shader)) {
			static const D3DXMACRO overrides[2] = { { "LAMPS_DEFERRED", "3" }, { NULL, NULL } };
			ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, kCache, overrides);
			if (record) Add(shader, record, forward, name, tmpl); else NoteRefused(shader, name);
		}
		if (Records.count(shader)) { DeferredLamps::EnsurePass(); DeferredLamps::EnsureShadowShader(); }
	}

	// NiD3DPixelShaderEx::SetupShader: the deferred record while the main list's G-buffer is bound (mode 1 indoors; mode 2 also outdoors once
	// DeferredSun is ready). Counts the binds and the forward fallbacks (an eligible shader without a record).
	inline ShaderRecordPixel* Choose(const void* shader, bool exterior) {
		if (!Live || !CapturedNormals::InSpan || (CapturedNormals::VmSpan && !CapturedNormals::VmDeferred)) return nullptr;
		if (exterior && (Mode() < 2 || !SunRuns())) return nullptr;
		SpanSeen = true;
		auto found = Records.find(shader);
		if (found == Records.end()) {
			if (Missing.count(shader)) StatFallbacks++;
			return nullptr;
		}
		if (BlendedNow && !LandHandles.count(found->second->ShaderHandle)) return nullptr;	// a blended draw: forward (BeforeDraw; counted in AfterDraw)
		StatBinds++;
		return found->second;
	}

	// 2026-11-14 fd-fixes (item 5): the light slots of the many-lamp object shaders (main light + lamps, ObjectTemplate.hlsl MAX_LIGHTS), as
	// LampsInOnePass's census reads them (Census::ShaderSlots: SLS2029/2030 6, SLS2031-2033 4, SLS2034-2036 ManyLamps' MaxLamps or 6 / 3 by
	// MoreLampsPerObject's byte at 0x00BDEA3F). 0 = not one of them (not counted).
	inline unsigned BoundSlots = 0;
	inline unsigned SlotsOf(const char* ps) {
		if (!ps || strncmp(ps, "SLS", 3)) return 0;
		const int n = atoi(ps + 3);
		if (n == 2029 || n == 2030) return 6;
		if (n >= 2031 && n <= 2033) return 4;
		if (n >= 2034 && n <= 2036) return ManyLamps::Active() ? (unsigned)ManyLamps::MaxLamps : *(const UInt8*)0x00BDEA3F == 0x05 ? 6 : 3;
		return 0;
	}

	// Render.cpp's bind hook (SetShadersHook, after the game bound a pass's shaders): is the pixel shader a deferred record?
	inline void OnBind(IDirect3DPixelShader9* handle, const char* name) {
		BoundDeferred = Live && handle && Handles.count(handle) != 0;
		BoundPlain = BoundDeferred && PlainHandles.count(handle) != 0;
		BoundSlots = Live ? SlotsOf(name) : 0;
	}

	// Render.cpp's pass hooks, early (next to FoldHighlights::Skip): true = do not draw this pass (a lamp or highlight pass of an object whose
	// base pass drew with a deferred record: the lamp pass adds its light).
	inline bool Skip(const void* passPtr, bool world) {
		if (!Live || !world || !passPtr) return false;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
		const PassSums::Kind kind = PassSums::KindOf(pass->passEnum);
		if (kind == PassSums::None || !Marked(pass->geometry, false)) return false;
		// The specular choice (coordinator, 11-14: on native release defaults ~65 % of shiny texels came from a non-SPECULAR base record + the
		// game's highlight passes). Decided: the highlight comes from where the material says it is. Without G-buffer v2 the lamp pass lights every
		// deferred pixel with the highlight, so every highlight pass is skipped. With v2 the pass reads the record's specular bit: a base record
		// WITHOUT SPECULAR (the light-only AD records, the plain ones) writes "no specular", so its object's highlight passes are KEPT (drawn
		// forward, exactly as today) while its lamps' diffuse comes from the pass; a SPECULAR base's highlight passes are skipped (the pass adds
		// the highlight). Counted apart.
		if (kind == PassSums::Highlights && GBufferV2() && *PlainOf) { StatKeptHighlights++; return false; }
		if (kind == PassSums::Lamps) StatSkippedLamps++; else StatSkippedHighlights++;
		return true;
	}

	// Render.cpp's pass hooks, after the game drew the pass: mark the object when its pass drew with a deferred record (its later lamp and
	// highlight passes are then skipped); count forward world draws inside the span, and on native those whose lamps the game capped (lamps
	// past the shader's slots: lit by nobody, since the lamp pass lights only deferred pixels).
	// 2026-11-14 fd-fixes (blended draws): a blended object (fading in or out, or alpha-blended: FoldFog's test, the shader property's alpha or
	// fade alpha below 1, or the NiAlphaProperty blend bit) cannot be lit by the lamp pass: CapturedCoverage keeps its G-buffer texel only at
	// alpha >= 0.5, so with a deferred record and its lamp passes skipped it got no lamp light below 0.5 and hid the surface behind from the
	// pass above it. Its base pass draws with the forward record instead and keeps its own lamp passes (not marked). The land (mode 2) is
	// not swapped: its fade over objects is a replace (alpha 1).
	inline bool Blended(const NiGeometry* geometry) {
		if (!geometry) return false;
		const BSShaderProperty* property = (const BSShaderProperty*)geometry->propertyState.m_spShadeProperty;
		if (property && (property->fAlpha < 1.0f || property->fFadeAlpha < 1.0f)) return true;
		const NiAlphaProperty* alpha = geometry->propertyState.m_spAlphaProperty;
		return alpha && (alpha->flags & 1);	// NiAlphaProperty::ALPHA_BLEND_MASK
	}
	// Render.cpp's pass hooks, right before the game's pass call. The game binds once per run of passes (not per draw), so a blended draw
	// inside a run whose bind chose the deferred record swaps to the forward record here (its constants set, on the device only: the game's
	// state cache keeps the deferred handle, and AfterDraw puts the device back when no bind came in between); a bind inside the draw sees
	// BlendedNow in Choose and takes the forward record itself.
	inline void BeforeDraw(const void* passPtr, bool world, IDirect3DDevice9* d) {
		BlendedNow = false; SwappedFrom = nullptr;
		if (!Live || !world || !passPtr || !CapturedNormals::InSpan) return;
		const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
		if (PassSums::KindOf(pass->passEnum) != PassSums::None || (!Blended(pass->geometry) && !OutsideViewModel(pass->geometry))) return;	// (2026-11-15 + DeferredFirstPerson)
		IDirect3DPixelShader9* bound = BoundDeferred ? TheRenderManager->renderState->GetPixelShader() : nullptr;
		if (bound && LandHandles.count(bound)) return;	// the land's fade: deferred
		BlendedNow = true;
		if (!bound) return;
		auto p = Pairs.find(bound);
		if (p == Pairs.end()) return;
		ShaderRecordPixel* partner = PartnerOf(p->second);
		if (partner) {
			partner->SetCT();
			d->SetPixelShader(partner->ShaderHandle);
			if (GpuTimer::Enabled && partner->PointShadowForward) InteriorRecordCensus::Swap();	// (2026-11-16 fast-restart, log only)
		}
		else {	// (2026-11-18 DeferredStandalone) no forward record: the game's own shader, which takes the game's constants as they are
			auto g = DeferredLamps::ForwardOf.find(bound);
			if (g == DeferredLamps::ForwardOf.end()) return;	// (none known: drawn deferred, as before)
			d->SetPixelShader(g->second);
		}
		SwappedFrom = bound;
		BoundDeferred = BoundPlain = false;
		StatBlendedSwaps++;
	}

	inline void AfterDraw(const void* passPtr, bool world) {
		const bool blended = BlendedNow;
		BlendedNow = false;
		if (Live && world && passPtr) {
			const PointShadowForward::RenderPassView* pass = (const PointShadowForward::RenderPassView*)passPtr;
			if (BoundDeferred) {
				if (PassSums::KindOf(pass->passEnum) == PassSums::None && pass->geometry && Marked(pass->geometry, true)) { *PlainOf = BoundPlain; StatMarked++; }
			}
			else {
				// (item 5) the one-pass compare (LampsInOnePass, OnePass) is read when the game builds an object's pass list, at registration
				// (BSShaderAccumulator::RegisterObject_Standard 0x00B64020 -> the property's GetRenderPasses 0x00B67BE0 -> GetRenderPasses_2x
				// 0x00BDF790, the compare at 0x00BE00E0), before the main list draws, and the list is kept until the key changes: it cannot follow
				// the span. So a forward draw of an OPT pass in a frame with the patch on lights only its shader's slots: counted, in the span
				// (fallbacks, blended draws) and outside it (everything drawn after the main list).
				// (bench batch 2) Such an object is learned (OnePassLimit::Keep, native only: OnePassLimit::On) and its next pass
				// list keeps the forward limit, so it is drawn in several passes that light every lamp; the counts here are what is left.
				const UInt16 e = pass->passEnum;
				if (OnePass && e >= 0xC3 && e <= 0xD6) {
					const unsigned used = (e == 0xCA || e == 0xD1) ? (pass->numLights ? pass->numLights - 1u : 0u) : pass->numLights;	// (as the census)
					const unsigned lost = (pass->maxLights > pass->numLights ? (unsigned)(pass->maxLights - pass->numLights) : 0u) + (BoundSlots && used > BoundSlots ? used - BoundSlots : 0u);
					if (lost) {
						if (CapturedNormals::InSpan) { StatSlotDrawsIn++; StatSlotLampsIn += lost; }
						else { StatSlotDrawsOut++; StatSlotLampsOut += lost; }
						OnePassLimit::Keep(pass->geometry);
					}
				}
			}
			if (!BoundDeferred && CapturedNormals::InSpan) {
				StatForwardDraws++;
				if (blended) StatBlended++;
				// (2026-11-15) OPT passes only: the game's several-pass set always holds fewer lamps per pass than the object has (AD 2-3, the
				// lamp passes the rest, all drawn for an unmarked object), so counting those (batch 3's "capped 25 / unlit 25") was no lost light
				if (Native && pass->passEnum >= 0xC3 && pass->passEnum <= 0xD6 && pass->maxLights > pass->numLights) { StatMissedLampDraws++; StatMissedLamps += (unsigned)(pass->maxLights - pass->numLights); }
			}
		}
		if (SwappedFrom) {	// the deferred record back on the device for the rest of the run, unless the game bound something else meanwhile
			if (TheRenderManager->renderState->GetPixelShader() == SwappedFrom) {
				auto p = Pairs.find(SwappedFrom);
				if (p != Pairs.end()) p->second.deferred->SetCT();
				TheRenderManager->device->SetPixelShader(SwappedFrom);
				BoundDeferred = true;
				BoundPlain = PlainHandles.count(SwappedFrom) != 0;
			}
			SwappedFrom = nullptr;
		}
	}

	// Render.cpp's DeferredSpan, after the G-buffer is resolved and before DeferredLamps::Apply: a deferred handle still bound goes back to its
	// forward one (draws after the list light their own lamps: critic item 4), counted; then DeferredSun through the hook (mode 2).
	inline void AtSpanEnd(IDirect3DDevice9* d) {
		if (!On()) return;
		IDirect3DPixelShader9* bound = nullptr;
		d->GetPixelShader(&bound);
		if (bound) {
			if (Handles.count(bound)) {
				auto p = Pairs.find(bound);
				ShaderRecordPixel* partner = p != Pairs.end() ? PartnerOf(p->second) : nullptr;
				if (partner && partner->ShaderHandle) {
					partner->SetCT();
					d->SetPixelShader(partner->ShaderHandle);
					StatSwaps++;
				}
				else {
					auto f = DeferredLamps::ForwardOf.find(bound);
					if (f != DeferredLamps::ForwardOf.end()) { d->SetPixelShader(f->second); StatSwaps++; }
				}
			}
			bound->Release();
		}
		BoundDeferred = false;
	}

	// Once a frame (Render.cpp, before LampsInOnePass::BeginFrame and DeferredLamps::BeginFrame).
	inline void BeginFrame() {
		FrameId++;
		const int mode = Mode();
		if (!mode) { Live = OnePass = false; DeferredLamps::RendererLive = false; return; }
		static bool told = false;
		Native = TheRenderManager && strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4) != 0;
		if (!told) {
			told = true;
			Logger::Log("UNOFFICIAL DeferredRenderer %d (%s): its prerequisites read as on for this session (CapturedNormals, MaterialBuffer, AlbedoBuffer, linear lighting + "
				"LinearScene, FoldFog, forward lamp shadows + the atlas, LampTableStandalone + LampTableFast, DeferredLamps with UnshadowedLamps 1%s); %u deferred records, "
				"%u did not load; the lamp pass %s", mode, Native ? "native D3D9" : "DXVK", mode >= 2 ? ", the lighting-free records, DeferredDistant" : "", Loaded, Refused,
				DeferredLamps::Pass ? "loaded" : "NOT loaded (forward)");
		}
		const bool exterior = TheShaderManager->GameState.isExterior;
		if (mode >= 2) SunForce(true);	// fd-sun: its pass on whatever its own Enabled says (it lights only lighting-free pixels: nothing while forward)
		bool live = TheSettingManager->GetSettingI(kSection, "DeferredRendererLive") != 0 && !Records.empty() && DeferredLamps::WantedAtStartup() &&
			!DeferredLamps::PassFailed && !CapturedNormals::Failed && CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn();
		const char* why = nullptr;
		if (live && Native && CapturedNormals::MsaaWarned) { live = false; why = "the game's MSAA on native D3D9 (no G-buffer there): forward"; }
		if (live && exterior && (mode < 2 || !SunRuns())) { live = false; why = mode < 2 ? nullptr : "outdoors: mode 2 needs the DeferredSun pass (claude/fd-sun): forward"; }
		// Fallback when Menu3DGuard could not be installed: keep overlay 3D menus / lockpick forward.
		if (live && !Menu3DGuardInstalled && (TheShaderManager->GameState.OverlayIsOn || (LockPickMenu::GetSingleton() && InterfaceManager->IsOpen(Menu::kMenuType_LockPick)))) live = false;
		if (why) { static const char* lastWhy = nullptr; if (why != lastWhy) { lastWhy = why; Logger::Log("UNOFFICIAL DeferredRenderer: refused, %s (logged once a reason)", why); } }
		if (mode >= 2 && !SunRuns() && DeferredSun::Failure()) { static bool sunTold = false; if (!sunTold) { sunTold = true; Logger::Log("[WARNING] UNOFFICIAL DeferredRenderer 2: the DeferredSun pass cannot run (%s): mode 2 draws forward", DeferredSun::Failure()); } }	// (2026-11-15: not while it is only not ready yet, frame 1)
		{	// UNOFFICIAL 2026-11-15 (FD-RELEASE-PREP.md) the linear gamma floor (SettingForce::FloorF): one line whenever the overridden value changes
			static float gammaLogged = 0.0f, nightLogged = 0.0f;
			if (SettingForce::GammaWas != gammaLogged || SettingForce::NightGammaWas != nightLogged) {
				gammaLogged = SettingForce::GammaWas; nightLogged = SettingForce::NightGammaWas;
				if (gammaLogged > 0.0f) Logger::Log("UNOFFICIAL DeferredRenderer: [Shaders.PBR.Linear] Gamma %.2f reads as 2.2 while the deferred renderer is on (a gamma of 1.0 is the same as linear lighting off, which the renderer needs)", gammaLogged);
				if (nightLogged > 0.0f) Logger::Log("UNOFFICIAL DeferredRenderer: [Shaders.PBR.Linear] NightGamma %.2f reads as 0 (follow Gamma) while the deferred renderer is on (a gamma of 1.0 is the same as linear lighting off, which the renderer needs)", nightLogged);
			}
		}
		if (mode >= 2 && !SunRuns()) live = false;
		if (!DeferredLamps::WantedAtStartup()) { static bool wTold = false; if (!wTold) { wTold = true; Logger::Log("[WARNING] UNOFFICIAL DeferredRenderer: the DeferredLamps pass is not possible (ManyLamps::DeferredAtStartup: the atlas, forward lamp shadows or the lamp table missing): forward"); } }
		Live = live;
		DeferredLamps::RendererLive = live;
		DeferredFoliage::BeginFrame(live && mode >= 2);	// UNOFFICIAL 2026-11-14 (before CapturedNormals::BeginFrame, which reads it)
		// the one-pass compare (LampsInOnePass): only indoors while live, after a G-buffer span has run (MSAA known by then); a frame late after a
		// switch, as LampsInOnePass's own limit
		{	// UNOFFICIAL 2026-11-15 (bench batch 3) DeferredRendererOnePass: a change rebuilds the held set's pass lists (the cave's rebuild bit)
			int m = TheSettingManager->GetSettingI(kSection, "DeferredRendererOnePass");
			m = m < 0 ? 0 : m > 2 ? 2 : m;
			if (m != OnePassMode) { OnePassMode = m; OnePassLimit::RebuildWanted = true; Logger::Log("UNOFFICIAL DeferredRenderer: DeferredRendererOnePass %d (%s)", m,
				m == 0 ? "no one-pass compare: the game's own pass lists" : m == 1 ? "one pass except emissive objects with more lamps than the game's own one pass takes" : "every shiny object in one pass"); }
		}
		OnePass = live && !exterior && SpanSeen && OnePassMode > 0;
		if (live) StatLiveFrames++;
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED RENDERER %u frames (%u live), mode %d, %s, per frame: deferred binds %.1f, forward fallbacks %.2f, deferred handles swapped back at the span's end %.2f | "
			"objects marked deferred %.1f (marks failed, table full: %llu in these frames), lamp passes skipped %.1f, highlight passes skipped %.1f (kept for a base without SPECULAR %.1f) | forward world draws in the span %.1f, of them blended (fading / alpha-blended: forward record, own lamp passes) %.2f (swapped inside a run %.2f), with lamps capped by the game %.2f (%.2f lamps unlit%s) | one-pass patch %s: forward OPT draws with lamps lost (the game's cap or the shader's slots) in the span %.2f (%.2f lamps), after it %.2f (%.2f lamps); per-object limit %s: objects learned %u, held at the forward limit %.1f, rebuilds %u; DeferredRendererOnePass %d (emittance per object %s), emissive objects held at the game's own one-pass limit %.1f, one-pass draws without the game's emittance %.1f (objects noted %u)",
			StatFrames, (unsigned)StatLiveFrames, mode, Native ? "native" : "DXVK", StatBinds / f, StatFallbacks / f, StatSwaps / f, StatMarked / f, StatMarkFailed, StatSkippedLamps / f, StatSkippedHighlights / f, StatKeptHighlights / f,
			StatForwardDraws / f, StatBlended / f, StatBlendedSwaps / f, StatMissedLampDraws / f, StatMissedLamps / f, Native ? "" : ": counted on native only", OnePass ? "on" : "off",
			StatSlotDrawsIn / f, StatSlotLampsIn / f, StatSlotDrawsOut / f, StatSlotLampsOut / f,
			OnePassLimit::On ? (OnePassLimit::CaveIn ? "on" : "NOT installed") : "off (DXVK: ManyLamps feeds the forward shaders)",
			OnePassLimit::Learned, OnePassLimit::Held / f, OnePassLimit::Rebuilds, OnePassMode, OnePassLimit::EmitExact ? "on" : "off", OnePassLimit::HeldEmissive / f, OnePassLimit::EmitDraws / f, OnePassLimit::MultiNoted);
		OnePassLimit::Held = 0; OnePassLimit::HeldEmissive = 0; OnePassLimit::EmitDraws = 0;
		if (StatMarkFailed) Logger::Log("[WARNING] UNOFFICIAL DeferredRenderer: %llu objects could not be marked deferred in the last %u frames (the table of %u slots was full there): they were lit twice (their own lamp passes and the lamp pass)", StatMarkFailed, StatFrames, kSlots);
		StatMarkFailed = 0;
		StatFrames = 0;
		StatBinds = StatFallbacks = StatSwaps = StatSkippedLamps = StatSkippedHighlights = StatKeptHighlights = StatMarked = StatForwardDraws = StatMissedLampDraws = StatMissedLamps = StatLiveFrames = StatBlended = StatBlendedSwaps = 0;
		StatSlotDrawsIn = StatSlotLampsIn = StatSlotDrawsOut = StatSlotLampsOut = 0;
	}
}
