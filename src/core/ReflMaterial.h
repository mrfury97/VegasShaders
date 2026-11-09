#pragma once
// UNOFFICIAL 2026-10-20 MaterialAware reflections ([Shaders.Reflections.Main] MaterialAware; deep/reflmat,
// bench\deep-roadmap\evidence\reflmat\RESULTS.md).
//
// The Reflections effect knew nothing of materials: it reflected surfaces that face up, with one roughness for all. The object
// shaders know each pixel's roughness and metal where an _rmaos map is bound (Includes/Object.hlsl RmaosValue). This carries it
// to the effect:
//  - D3D9 cannot draw into a second render target while the game's MSAA is on (SkinScattering.cpp measured it: nothing drawn at
//    all), so the scene's own alpha carries it. The world target is fp16; the object shaders' first passes write
//    2 + 0.25 x metal (4 steps) + 0.24 x roughness there (Object.hlsl ReflMaterialAlpha), always 2 or more, where the game writes 0-1.
//  - The game draws the world without alpha writes (COLORWRITEENABLE 7): a flagged pass gets alpha added to the mask on the device
//    once its shaders are bound, and the old mask back after the draw (OnShadersBound / AfterPass).
//  - Only opaque, untested objects' first passes in the world scene, and only objects with their map on, write it (c212.x = 1,
//    set per pass here; the shader then takes the map's values without testing for it): a blended pass's
//    alpha weighs its colour (SRCALPHA), and an alpha-tested one is cut by it. The lamp and highlight runs (PassSums::KindOf) are
//    blended, so never.
//  - While the world scene is drawn, every blended pass leaves the target's alpha alone (separate alpha blend: ZERO x source +
//    ONE x destination), so the lamps and highlights added on top, decals and glass do not change what the first pass wrote; the
//    colour blends exactly as before (the separate factors apply to alpha only).
//  - The Reflections effect copies the alpha out into its own small buffer before any effect draws (ReflectionsEffect::
//    CaptureMaterial), and reads it there.
// Safety net: if a pass drew blended although it got c212.x = 1 (a pass kind not known above), its pass number is put on a list
// that never gets the flag again, and the log says so once.
// UNOFFICIAL 2026-11-07 (perf-mirrorfix, core/ReflMaterialRule.h): every OTHER pass that replaces a pixel's colour writes its own alpha
// too (the game's 0..1: no material), so nothing drawn in front of a mirror keeps the mirror's material (chrome shirts and fence posts at
// the RT test room's mirror wall); blended first passes of skinned objects (hair) scale it by (1 - their alpha). NeutralBegin / NeutralEnd
// round every object pass (the two pass hooks AND BSBatchRenderer::RenderPassImmediately, which also draws grass), in the world scene
// and the first-person scene (BeginFirstPerson / EndFirstPerson: the same separate alpha blend round it).
// Off (MaterialAware 0 at startup, or the _rmaos maps off): nothing here runs and the shaders are compiled without it.
#include <d3d9.h>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>
#include "PassSums.h"
#include "ReflMaterialRule.h"

namespace ReflMaterial {
	inline int Startup = -1;					// MaterialAware > 0 and the _rmaos maps on, read once (the shaders are compiled with it)
	inline int AddStartup = -1;					// UNOFFICIAL 2026-10-21: [Shaders.Reflections.Main] ReflectAdd > 0 at startup (REFL_ADD_ON in the effect)
	inline bool Writing = false;				// this frame's world scene writes the material (MaterialAware > 0 now)
	inline float Current = -1.0f;				// c212.x as last sent (-1: send on the next pass)
	inline float CurrentRough = -9.0f;	// the override as last sent in c212.yz
	// UNOFFICIAL 2026-10-21 RoughnessOverrides ([Shaders.Reflections.Main], "keyword:roughness, ..."): the generated _rmaos maps rate some
	// polished floors too rough to mirror (Ultra-Luxe: centre tiles 0.48, black marble 0.38 against MaxRoughness 0.5). A draw whose _rmaos
	// file name contains a keyword gives the reflections that roughness instead of its map's (the object's own lighting is untouched).
	// (The generator's metal is a guess from grey colour, at most ~0.6: Object.hlsl counts only metal above 0.6, a fixed literal.)
	inline std::vector<std::pair<std::string, float>> Overrides;
	inline std::string OverrideText = "\x01";	// the setting as last parsed
	inline unsigned OverrideGeneration = 1;		// bumped whenever the list changes (RmaosDraw caches per map)
	inline float DrawRough = -1.0f;				// the override for the object being drawn (-1: its map's), set by RmaosDraw::ForObject
	inline bool StatesSet = false;
	inline DWORD SavedSeparate = FALSE, SavedSrcAlpha = D3DBLEND_ONE, SavedDstAlpha = D3DBLEND_ZERO, SavedOpAlpha = D3DBLEND_ONE;
	// Pass numbers that drew blended with the flag on (never flagged again). Seeded with the ones the first in-game run found (2026-10-21,
	// Ultra-Luxe: each drew blended once while loading): 14A 14D 14E and 248 252 254.
	// 2026-10-21 Strip run: also 249 and 24A. The 248-254 group (and 14A-14E) are taken whole.
	inline std::unordered_set<UInt16> Refused = { 0x14A, 0x14B, 0x14C, 0x14D, 0x14E, 0x248, 0x249, 0x24A, 0x24B, 0x24C, 0x24D, 0x24E,
		0x24F, 0x250, 0x251, 0x252, 0x253, 0x254 };
	inline UInt32 FlaggedPasses = 0, Frames = 0;
	inline UInt16 LastFlaggedPass = 0;
	inline bool LastFlagged = false;
	inline DWORD SavedWrite = 0xF;				// the pass's colour write mask before OnShadersBound added alpha
	inline bool WriteChanged = false;
	inline UInt32 DiagBound = 0, DiagChanged = 0, DiagFlagged = 0, DiagMask[16] = {};	// diagnostic counts for the log (per report)
	// UNOFFICIAL 2026-11-07 (perf-mirrorfix) the other passes' own alpha (ReflMaterialRule.h Own / Cover)
	inline bool FirstPersonLive = false;		// the first-person scene writes alpha (BeginFirstPerson)
	inline int NeutralDepth = 0;				// NeutralBegin calls open (the dispatcher's round the pass hook's: the outer one decides)
	inline ReflMaterialRule::Write NeutralWrite = ReflMaterialRule::Keep;
	inline bool NeutralAlphaAdded = false, NeutralBlendChanged = false;
	inline DWORD NeutralSavedDestAlpha = D3DBLEND_ONE;
	inline UInt32 DiagOwn = 0, DiagCover = 0;
	inline UInt32 DiagDrawOwn = 0, DiagDrawCover = 0, DiagDrawKeep = 0;	// (perf-mirrorfix2) DrawBegin's decisions

	// Whether the object shaders were compiled with it (asked by ShaderRecord::BuildDefines, at the first shader load).
	// UNOFFICIAL 2026-10-22: only while Reflections itself is on at startup, so MaterialAware can be on by default while the effect
	// is off without touching the object shaders or the world draw (turning Reflections on then needs a restart for the materials).
	inline bool CompiledIn() {
		if (Startup < 0)
			Startup = TheSettingManager->GetSettingI("Shaders.Reflections.Main", "MaterialAware") > 0 &&
				TheSettingManager->GetSettingI("Shaders.Reflections.Status", "Enabled") &&
				TheSettingManager->GetSettingI("Shaders.PBR.Rmaos", "Enabled") ? 1 : 0;
		return Startup > 0;
	}

	inline int OccStartup = -1;	// UNOFFICIAL 2026-10-21: [Shaders.Reflections.Main] SpecularOcclusion > 0 at startup (REFL_SPECOCC_ON)
	inline bool OccCompiledIn() {
		if (OccStartup < 0) OccStartup = TheSettingManager->GetSettingF("Shaders.Reflections.Main", "SpecularOcclusion") > 0.0f ? 1 : 0;
		return OccStartup > 0;
	}

	inline bool AddCompiledIn() {
		if (AddStartup < 0) AddStartup = TheSettingManager->GetSettingF("Shaders.Reflections.Main", "ReflectAdd") > 0.0f ? 1 : 0;
		return AddStartup > 0;
	}

	// c212: x the flag, yz the reflections' roughness = the map's x y + z (1, 0: the map's; 0, r: the override r).
	inline void Send(IDirect3DDevice9* device, float v) {
		const float rough = v > 0.0f ? DrawRough : -1.0f;
		if (v == Current && rough == CurrentRough) return;
		Current = v; CurrentRough = rough;
		const float c[4] = { v, rough >= 0.0f ? 0.0f : 1.0f, rough >= 0.0f ? rough : 0.0f, 0.0f };
		device->SetPixelShaderConstantF(212, c, 1);
	}

	// Reads RoughnessOverrides (once a frame, at the world scene's start; parsed only when the text changed).
	inline void ReadSettings() {
		SettingManager::Configuration::ConfigNode node;	// a String setting's whole text is in LongValue (Value is cut at 79 characters)
		std::string text = TheSettingManager->Config.FillNode(&node, "Shaders.Reflections.Main", "RoughnessOverrides") ?
			(node.LongValue.empty() ? std::string(node.Value) : node.LongValue) : std::string();
		if (OverrideText == text) return;
		OverrideText = text;
		Overrides.clear();
		OverrideGeneration++;
		const std::string& s = text;
		size_t start = 0;
		while (start < s.size()) {
			size_t end = s.find(',', start);
			if (end == std::string::npos) end = s.size();
			std::string item = s.substr(start, end - start);
			start = end + 1;
			const size_t colon = item.rfind(':');
			if (colon == std::string::npos) continue;
			std::string key = item.substr(0, colon);
			key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\t' || c == '"'; }), key.end());
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			const float r = (float)atof(item.c_str() + colon + 1);
			if (!key.empty()) Overrides.push_back({ key, std::clamp(r, 0.02f, 1.0f) });
		}
		if (!Overrides.empty()) Logger::Log("MaterialAware reflections: %u roughness override(s) from RoughnessOverrides", (unsigned)Overrides.size());
	}

	// The override for an _rmaos file (lower case compare; first keyword found wins), -1 for none.
	inline float RoughFor(const std::string& file) {
		if (Overrides.empty()) return -1.0f;
		std::string f(file);
		std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		for (const auto& o : Overrides) if (f.find(o.first) != std::string::npos) {
			Logger::Log("MaterialAware reflections: roughness %.2f for %s (RoughnessOverrides '%s')", o.second, file.c_str(), o.first.c_str());
			return o.second;
		}
		return -1.0f;
	}

	// The world scene starts: the blended passes keep off the alpha (through the game's render state cache, so its own later
	// settings stay right), and c212 is sent again on the first pass (an effect may have used the register since).
	// The scene's separate alpha blend (blended passes keep off the alpha), through the game's state cache; and back
	inline void SetSceneStates(NiDX9RenderState* state, IDirect3DDevice9* device) {
		device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &SavedSeparate);
		device->GetRenderState(D3DRS_SRCBLENDALPHA, &SavedSrcAlpha);
		device->GetRenderState(D3DRS_DESTBLENDALPHA, &SavedDstAlpha);
		device->GetRenderState(D3DRS_BLENDOPALPHA, &SavedOpAlpha);
		state->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO, RenderStateArgs);
		state->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE, RenderStateArgs);
		state->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD, RenderStateArgs);
		state->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE, RenderStateArgs);
		StatesSet = true;
	}
	inline void RestoreSceneStates(NiDX9RenderState* state, IDirect3DDevice9* device, const char* scene) {
		if (StatesSet && state && device) {
			// Still on? (the game never sets it, as far as known; if it did, the log says so once)
			DWORD now = FALSE;
			device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &now);
			static bool warned = false;
			if (!now && !warned) { warned = true; Logger::Log("[WARNING] MaterialAware reflections: separate alpha blending was switched off during the %s scene; blended draws after that point change the material buffer", scene); }
			state->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, SavedSeparate, RenderStateArgs);
			state->SetRenderState(D3DRS_SRCBLENDALPHA, SavedSrcAlpha, RenderStateArgs);
			state->SetRenderState(D3DRS_DESTBLENDALPHA, SavedDstAlpha, RenderStateArgs);
			state->SetRenderState(D3DRS_BLENDOPALPHA, SavedOpAlpha, RenderStateArgs);
		}
		StatesSet = false;
	}

	inline void BeginWorld(NiDX9RenderState* state, IDirect3DDevice9* device, bool live) {
		Writing = CompiledIn() && live;
		Current = -1.0f;
		NeutralDepth = 0;
		if (!Writing || !state || !device) { Writing = false; return; }
		ReadSettings();
		SetSceneStates(state, device);
		Frames++;
	}

	// UNOFFICIAL 2026-11-07 (perf-mirrorfix) the first-person scene (Render.cpp RenderFirstPersonHook): drawn into the same target before
	// the effects copy the material out (the arms and gun over a mirror kept its material: a chrome gun). No material passes here; every
	// pass that replaces the colour writes its own alpha (Own), under the same separate alpha blend as the world scene.
	inline void BeginFirstPerson(NiDX9RenderState* state, IDirect3DDevice9* device, bool live) {
		FirstPersonLive = CompiledIn() && live && state && device && !StatesSet;
		NeutralDepth = 0;
		if (FirstPersonLive) SetSceneStates(state, device);
	}
	inline void EndFirstPerson(NiDX9RenderState* state, IDirect3DDevice9* device) {
		if (FirstPersonLive) RestoreSceneStates(state, device, "first-person");
		FirstPersonLive = false;
	}

	inline void EndWorld(NiDX9RenderState* state, IDirect3DDevice9* device) {
		if (device && Current != 0.0f && Current != -1.0f) Send(device, 0.0f);
		RestoreSceneStates(state, device, "world");
		Writing = false;
		if (Frames >= 1800) {
			Logger::Log("MaterialAware reflections: %u object passes per frame wrote their material (average of %u frames); %u pass kinds refused",
				FlaggedPasses / Frames, Frames, (unsigned)Refused.size());
			Logger::Log("MaterialAware reflections: per frame %u other passes wrote their own alpha (no material), %u blended skinned passes covered it (perf-mirrorfix)",
				DiagOwn / Frames, DiagCover / Frames);
			Logger::Log("MaterialAware reflections: per frame at draw time %u draws wrote their own alpha, %u covered by their alpha, %u kept the alpha under them (perf-mirrorfix2)",
				DiagDrawOwn / Frames, DiagDrawCover / Frames, DiagDrawKeep / Frames);
			DiagOwn = DiagCover = 0;
			DiagDrawOwn = DiagDrawCover = DiagDrawKeep = 0;
			char masks[160] = ""; size_t used = 0;
			for (int m = 0; m < 16; m++) if (DiagMask[m]) used += snprintf(masks + used, sizeof(masks) - used, " %X:%u", m, DiagMask[m]);
			Logger::Log("MaterialAware reflections diagnostic: %u flagged passes drew, %u shader binds inside them, %u got alpha added; write mask at the draw's end (mask:passes)%s",
				DiagFlagged, DiagBound, DiagChanged, masks);
			DiagBound = DiagChanged = DiagFlagged = 0; memset(DiagMask, 0, sizeof(DiagMask));
			FlaggedPasses = 0; Frames = 0;
		}
	}

	// A flagged pass is about to draw (ForPass; the game binds the pass's shaders before the pass hook, run 2026-10-21 00:29: no
	// SetShaders call inside 151800 flagged passes): the game draws the world with COLORWRITEENABLE 7 (no alpha: run 00:22, the world
	// target's alpha stayed 0 everywhere), so alpha is added to the mask on the device for this draw only. The game's state cache
	// never sees it; AfterPass puts the old value back. (Also called from SetShadersHook, in case a pass binds inside.)
	inline void NeutralRebind(IDirect3DDevice9* device);
	inline void OnShadersBound(IDirect3DDevice9* device) {
		NeutralRebind(device);	// (2026-11-07 perf-mirrorfix) a pass that set its own write mask after NeutralBegin
		if (!LastFlagged || WriteChanged || !device) return;
		DiagBound++;
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &SavedWrite);
		if (SavedWrite & D3DCOLORWRITEENABLE_ALPHA) return;
		device->SetRenderState(D3DRS_COLORWRITEENABLE, SavedWrite | D3DCOLORWRITEENABLE_ALPHA);
		WriteChanged = true;
		DiagChanged++;
	}

	// UNOFFICIAL 2026-11-07 (perf-mirrorfix) the pass's facts for ReflMaterialRule::Decide (device: read only for what Decide needs then)
	inline ReflMaterialRule::Pass PassFacts(const void* pass, NiGeometry* geometry, bool world, bool hasMap) {
		ReflMaterialRule::Pass p;
		p.live = world ? Writing : FirstPersonLive;
		p.world = world;
		p.hasGeometry = pass && geometry;
		if (!p.live || !p.hasGeometry) return p;
		p.hasMap = hasMap;
		const UInt16 e = *(const UInt16*)((const UInt8*)pass + 0x04);
		p.firstKind = PassSums::KindOf(e) == PassSums::None;
		p.refused = Refused.count(e) != 0;
		const NiAlphaProperty* alpha = geometry->propertyState.m_spAlphaProperty;
		p.blendProperty = alpha && (alpha->flags & NiAlphaProperty::ALPHA_BLEND_MASK);
		p.testProperty = alpha && (alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK);
		p.skinned = geometry->skinInstance != nullptr;
		return p;
	}

	// Round every object pass of the world / first-person scene (the outermost call decides; nested calls only count): world / firstPerson
	// = ShaderSplit's context; hasMap: RmaosDraw::ThisDraw where known (the pass hooks), false in the dispatcher (a material pass is then
	// given alpha writes here, and its c212 flag by ForPass inside: the same result).
	inline void NeutralBegin(const void* pass, NiGeometry* geometry, bool world, bool firstPerson, bool hasMap, IDirect3DDevice9* device) {
		if (NeutralDepth++ > 0) return;
		NeutralWrite = ReflMaterialRule::Keep;
		NeutralAlphaAdded = NeutralBlendChanged = false;
		if (!device || !(world || firstPerson) || !(world ? Writing : FirstPersonLive)) return;
		ReflMaterialRule::Pass p = PassFacts(pass, geometry, world, hasMap);
		if (!p.live || !p.hasGeometry) return;
		DWORD separate = FALSE, write = 0;
		device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separate);
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &write);
		p.separateAlpha = separate != FALSE;
		p.colorWrite = write;
		ReflMaterialRule::Write w = ReflMaterialRule::Decide(p);
		if (w == ReflMaterialRule::Material) w = ReflMaterialRule::Own;	// (the dispatcher's view of a material pass: ForPass sets its flag)
		NeutralWrite = w;
		if (w == ReflMaterialRule::Keep) return;
		if (w == ReflMaterialRule::Cover) {
			device->GetRenderState(D3DRS_DESTBLENDALPHA, &NeutralSavedDestAlpha);
			device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
			NeutralBlendChanged = true;
			DiagCover++;
		}
		else DiagOwn++;
		if (!(write & D3DCOLORWRITEENABLE_ALPHA)) {
			device->SetRenderState(D3DRS_COLORWRITEENABLE, write | D3DCOLORWRITEENABLE_ALPHA);
			NeutralAlphaAdded = true;
		}
	}

	// The game set a write mask inside the pass (through its state cache, after NeutralBegin): alpha again, if it writes colour
	inline void NeutralRebind(IDirect3DDevice9* device) {
		if (NeutralDepth <= 0 || NeutralWrite == ReflMaterialRule::Keep || !device) return;
		DWORD write = 0;
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &write);
		if (!(write & 7u) || (write & D3DCOLORWRITEENABLE_ALPHA)) return;
		device->SetRenderState(D3DRS_COLORWRITEENABLE, write | D3DCOLORWRITEENABLE_ALPHA);
		NeutralAlphaAdded = true;
	}

	inline void NeutralEnd(IDirect3DDevice9* device) {
		if (NeutralDepth <= 0) return;
		if (--NeutralDepth > 0) return;
		if (device && NeutralAlphaAdded) {	// the mask the game believes in (its cache never saw the alpha bit)
			DWORD write = 0;
			device->GetRenderState(D3DRS_COLORWRITEENABLE, &write);
			if (write & D3DCOLORWRITEENABLE_ALPHA) device->SetRenderState(D3DRS_COLORWRITEENABLE, write & ~(DWORD)D3DCOLORWRITEENABLE_ALPHA);
		}
		if (device && NeutralBlendChanged) device->SetRenderState(D3DRS_DESTBLENDALPHA, NeutralSavedDestAlpha);
		NeutralWrite = ReflMaterialRule::Keep;
		NeutralAlphaAdded = NeutralBlendChanged = false;
	}

	// --- UNOFFICIAL 2026-11-07 (perf-mirrorfix2) the same at DRAW time (ReflMaterialRule::DecideDraw), round every game draw of an object:
	// Render.cpp MaterialDraw wraps the skinned partitions' two draw calls and the renderer's still-object draws. Test 10 showed her head,
	// hair and shoulder in front of the mirror still taking its material: actors with a blend property and hair are drawn by the sorted
	// blended list, which never comes through the pass hooks (and the pass hooks are only redirected with some settings on).
	inline bool InBlendedList = false;			// RenderBlendedHook: the sorted blended list is drawing
	inline std::unordered_set<UInt64> DiagDrawSeen;	// the once-per-session list (distinct pass / shader / state)
	struct DrawState { bool alphaAdded = false, blendChanged = false; DWORD savedDest = D3DBLEND_ONE; };

	// world / firstPerson: ShaderSplit's context; passEnum / pixelShader: the game's current pass (for the session list only)
	inline ReflMaterialRule::Write DrawBegin(IDirect3DDevice9* device, bool skinned, bool world, bool firstPerson, UInt16 passEnum,
		const char* pixelShader, DrawState& s) {
		s = DrawState();
		ReflMaterialRule::Draw d;
		d.live = device && ((world && Writing) || (firstPerson && FirstPersonLive));
		if (!d.live) return ReflMaterialRule::Keep;
		DWORD write = 0, blend = FALSE, separate = FALSE, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
		device->GetRenderState(D3DRS_COLORWRITEENABLE, &write);
		d.colorWrite = write;
		if ((write & 7u) && !(write & D3DCOLORWRITEENABLE_ALPHA)) {
			device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			if (blend) { device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separate); device->GetRenderState(D3DRS_SRCBLEND, &src); }
		}
		d.blend = blend != FALSE; d.separateAlpha = separate != FALSE; d.srcBlend = src; d.skinned = skinned;
		const ReflMaterialRule::Write w = ReflMaterialRule::DecideDraw(d);
		if (w == ReflMaterialRule::Own || w == ReflMaterialRule::Cover) {
			if (w == ReflMaterialRule::Cover) {
				device->GetRenderState(D3DRS_DESTBLENDALPHA, &s.savedDest);
				device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
				s.blendChanged = true;
				DiagDrawCover++;
			}
			else DiagDrawOwn++;
			device->SetRenderState(D3DRS_COLORWRITEENABLE, write | D3DCOLORWRITEENABLE_ALPHA);
			s.alphaAdded = true;
		}
		else if (w == ReflMaterialRule::Keep && (write & 7u)) DiagDrawKeep++;
		// The once-per-session list (the lead's ask, test 10): every distinct kind of draw that writes colour, for skinned geometry and for
		// whatever leaves the alpha under it (Keep: a material there would stay), with what was done; at most 64 lines a session
		if ((write & 7u) && (skinned || w == ReflMaterialRule::Keep) && DiagDrawSeen.size() < 64) {
			if (blend) device->GetRenderState(D3DRS_DESTBLEND, &dst);
			const UInt64 key = ((UInt64)passEnum << 48) ^ ((UInt64)(uintptr_t)pixelShader << 8) ^ ((UInt64)w << 4) ^ ((UInt64)src << 20) ^ ((UInt64)dst << 26) ^
				((UInt64)blend << 1) ^ (skinned ? 1u : 0u) ^ (InBlendedList ? 4u : 0u) ^ (firstPerson ? 8u : 0u);
			if (DiagDrawSeen.insert(key).second) {
				static const char* const kNames[] = { "KEEPS the alpha under it", "material pass", "writes its own alpha", "covers by its alpha" };
				Logger::Log("MaterialAware reflections draw (session list %u): %s pass %03X (%s), PS %s, %s, blend %s%u->%u, %s%s",
					(unsigned)DiagDrawSeen.size(), skinned ? "skinned" : "still", (unsigned)passEnum, Pointers::Functions::GetPassDescription(passEnum),
					pixelShader ? pixelShader : "?", InBlendedList ? "sorted blended list" : "main lists", blend ? "on " : "off ", (unsigned)src, (unsigned)dst,
					kNames[(int)w], firstPerson ? " (first person)" : "");
			}
		}
		return w;
	}
	inline void DrawEnd(IDirect3DDevice9* device, const DrawState& s) {
		if (!device) return;
		if (s.alphaAdded) {
			DWORD write = 0;
			device->GetRenderState(D3DRS_COLORWRITEENABLE, &write);
			device->SetRenderState(D3DRS_COLORWRITEENABLE, write & ~(DWORD)D3DCOLORWRITEENABLE_ALPHA);
		}
		if (s.blendChanged) device->SetRenderState(D3DRS_DESTBLENDALPHA, s.savedDest);
	}

	// Scoped NeutralBegin / NeutralEnd (the dispatcher's calls)
	struct NeutralScope {
		IDirect3DDevice9* device;
		NeutralScope(const void* pass, NiGeometry* geometry, bool world, bool firstPerson, IDirect3DDevice9* d) : device(d) {
			NeutralBegin(pass, geometry, world, firstPerson, false, d);
		}
		~NeutralScope() { NeutralEnd(device); }
	};

	// Before each object pass (Render.cpp RenderPassStandardHook / RenderPassSkinnedHook). world: in the world scene; hasMap: with an
	// _rmaos map. (2026-11-07 perf-mirrorfix: the decision is ReflMaterialRule::Decide's Material.)
	inline void ForPass(const void* pass, NiGeometry* geometry, bool world, bool hasMap, IDirect3DDevice9* device) {
		LastFlagged = false;
		if (!Writing || !device) return;
		UInt16 e = pass ? *(const UInt16*)((const UInt8*)pass + 0x04) : 0;
		const bool flag = world && ReflMaterialRule::Decide(PassFacts(pass, geometry, true, hasMap)) == ReflMaterialRule::Material;
		Send(device, flag ? 1.0f : 0.0f);
		if (flag) { FlaggedPasses++; LastFlagged = true; LastFlaggedPass = e; OnShadersBound(device); }
	}

	// After the pass drew: the flag goes back to 0 at once (draws that do not come through the pass hooks, such as the sorted
	// blended ones, must never see it), and the state the pass drew with is still set: a flagged pass that blended goes on the
	// refused list.
	inline void AfterPass(IDirect3DDevice9* device) {
		if (!LastFlagged || !device) return;
		LastFlagged = false;
		DiagFlagged++;
		{ DWORD mask = 0; device->GetRenderState(D3DRS_COLORWRITEENABLE, &mask); DiagMask[mask & 15]++; }
		Send(device, 0.0f);
		if (WriteChanged) { device->SetRenderState(D3DRS_COLORWRITEENABLE, SavedWrite); WriteChanged = false; }
		DWORD blend = FALSE;
		device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		if (!blend) return;
		Refused.insert(LastFlaggedPass);
		Logger::Log("[WARNING] MaterialAware reflections: pass %03X drew blended with the material flag on (its colour may have been wrong for one draw); it is left out from now on", (unsigned)LastFlaggedPass);
	}
}
