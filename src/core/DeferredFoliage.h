#pragma once
// UNOFFICIAL 2026-11-14 full deferred renderer, phase 3: foliage (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.6 / 4.7 phase 3;
// docs/next/FD-FOLIAGE-RESULTS.md). The SpeedTree leaves (STLEAF2000 / 2000TMS / 2001 / 2001TMS.pso) and the grass (GRASS23x000 /
// 000TMS.pso) in the deferred path: their DEFERRED records (FOLIAGE_DEFERRED 1; the model in Shaders/Includes/FoliageDeferred.hlsl) draw the
// foliage without its sun and hand the sun's unshadowed light to DeferredSun, which adds it with the screen-space sun shadow.
//
// [Main.Main.Quality] DeferredFoliage (restart, test only, default off; DeferredRenderer 2 turns it on through core/SettingForce.h). Needs
// GBufferV2 (the records' code lives in its layout) and DeferredSun (the pass that adds the light) at startup; otherwise nothing is made.
//  - Records (ShaderManager::LoadShader, at load, never on the render thread): one per foliage pixel shader name, cache Deferred\Foliage\.
//    ShaderRecord::LoadShader notes them as grass / leaf writers by name, as their forward twins.
//  - Live (once a frame, from DeferredRenderer::BeginFrame): the renderer live in mode 2 and the path tracer not replacing the picture
//    (it reads the forward records' colour as the surface's: the foliage stays forward under it).
//  - Why the leaf records were "foliageidle" in the census (FD-PHASE0-RESULTS.md Q3): CapturedNormals wrote the foliage records only while the
//    path tracer replaced the picture (LeafNow) or, for the grass, while the RT GI read them (GrassNow). While this is live both are on
//    (CapturedNormals::FoliageDeferred), so the leaves write the G-buffer in the main list and the near grass opens the span before it.
//  - Selection (NiD3DPixelShaderEx::SetupShader, Choose): inside the G-buffer span with the records written this frame. The grass GrassFirst
//    draws before the main list opens the span at its bind (Render.cpp calls CapturedNormals::GrassBind before the pixel shader is set up
//    while this is live). The grass drawn AFTER the list (the fading tufts) is outside the span: forward, as before, and it writes no
//    record (CapturedNormals::GrassLateOk: only for its old readers, the path tracer and the RT GI).
//  - A foliage record still bound at the span's end goes back to its forward handle (DeferredLamps::ForwardOf, DeferredRenderer::AtSpanEnd).
// Off: no record is made, nothing is chosen; every shader and record as before.
// UNOFFICIAL 2026-11-16 fd-fwdgroups [Main.Main.Quality] DeferredFoliageLamps (restart to compile, then live; test only, default off;
// docs/next/FD-FORWARD-GROUPS.md): the leaves' lamps. The lamp pass never lights a leaf (its record writes a forward code: the bent normal and
// the leaf model are not in the G-buffer), so a deferred leaf kept the game's one point lamp (BSSM_LEAVES_ADPt; none in _AD) beside ground lit
// by every lamp. With the key the leaf's deferred record has a twin (LEAF_FORWARD_PLUS 1, Includes/Leaf.hlsl, cache Deferred\FoliageLamps\)
// that takes the vertex shader's own lamp out and adds every lamp of the pixel's froxel from the frame's lamp table (Includes/ForwardPlus.hlsl),
// atlas-shadowed, with the leaf vertex shader's lamp model on the bent normal (handed over by STLEAF001/003.vso with G-buffer v2). Chosen while
// the key is on and this frame's table holds this frame's froxel lists (else the plain record: never a leaf half-lit). The table and atlas are
// bound by DeferredTransparents::OnBind (s3, s12, c180 / c181).
#include "CapturedNormals.h"
#include "DeferredLamps.h"
#include "DeferredSun.h"
#include "SettingForce.h"
#include "ShaderRecord.h"
#include <unordered_map>
#include <unordered_set>

namespace RtPathTrace { bool Replacing(); }

namespace DeferredFoliage {
	inline const char* kSection = "Main.Main.Quality";
	inline const char* kCache = "Deferred\\Foliage\\";

	inline int Wanted = -1;
	// At startup: the key (forced on by DeferredRenderer 2), GBufferV2 compiled in, and the sun pass made (it is made with the lighting-free
	// records: LightFreeRecords, which DeferredRenderer 2 forces too).
	inline bool WantedAtStartup() {
		if (Wanted < 0) {
			const bool key = TheSettingManager->GetSettingI(kSection, "DeferredFoliage") != 0;
			Wanted = key && CapturedNormals::V2CompiledIn() && DeferredSun::WantedAtStartup();
			if (key && !Wanted) Logger::Log("[WARNING] UNOFFICIAL DeferredFoliage: needs GBufferV2 and the DeferredSun pass at startup (DeferredRenderer 2 turns both on): the foliage stays forward");
		}
		return Wanted != 0;
	}

	inline std::unordered_map<const void*, ShaderRecordPixel*> Records;	// NiD3DPixelShaderEx* -> its deferred record
	inline std::unordered_set<IDirect3DPixelShader9*> Handles;
	inline unsigned Loaded = 0, Refused = 0;
	inline bool Live = false;
	inline unsigned long long StatFrames = 0, StatBinds = 0, StatLeafBinds = 0, StatGrassBinds = 0, StatOutside = 0, StatLampBinds = 0;
	// (2026-11-16) DeferredFoliageLamps
	inline std::unordered_map<const void*, ShaderRecordPixel*> LampRecords;
	inline std::unordered_set<IDirect3DPixelShader9*> LampHandles;
	inline bool LampsLive = false;
	inline unsigned LampLoaded = 0;
	inline int LampsWanted = -1;
	inline bool LampsWantedAtStartup() {
		if (LampsWanted < 0) LampsWanted = TheSettingManager->GetSettingI(kSection, "DeferredFoliageLamps") != 0 ? 1 : 0;
		return LampsWanted != 0;
	}

	inline bool IsLeaf(const char* name) {
		return name && (!_stricmp(name, "STLEAF2000.pso") || !_stricmp(name, "STLEAF2000TMS.pso") || !_stricmp(name, "STLEAF2001.pso") || !_stricmp(name, "STLEAF2001TMS.pso"));
	}
	inline bool IsGrass(const char* name) { return name && (!_stricmp(name, "GRASS23x000.pso") || !_stricmp(name, "GRASS23x000TMS.pso")); }

	// ShaderManager::LoadShader (pixel), after the shader's own records: its deferred record. tmpl: the family's (no template: its own file).
	inline void Load(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl) {
		if (!shader || !shader->ShaderProg[ShaderRecordType::Default] || !(IsLeaf(name) || IsGrass(name)) || !WantedAtStartup()) return;
		static const D3DXMACRO overrides[2] = { { "FOLIAGE_DEFERRED", "1" }, { NULL, NULL } };
		ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, kCache, overrides);
		if (!record || !record->ShaderHandle) {
			Refused++;
			Logger::Log("[WARNING] UNOFFICIAL DeferredFoliage record %s: did not load (see the device's line above): it stays forward; %u loaded, %u not so far", name, Loaded, Refused);
			return;
		}
		Records[shader] = record;
		Handles.insert(record->ShaderHandle);
		CapturedNormals::FoliageDeferredHandles.insert(record->ShaderHandle);
		if (IsGrass(name)) CapturedNormals::FoliageGrassRange = true;	// (2026-11-14 fog: the grass-mark readers take its COLOR2.g range)
		DeferredSun::FoliageHandles.insert(record->ShaderHandle);
		ShaderRecordPixel* forward = (ShaderRecordPixel*)shader->ShaderProg[ShaderRecordType::Default];
		if (forward && forward->ShaderHandle) DeferredLamps::ForwardOf[record->ShaderHandle] = forward->ShaderHandle;	// the span-end swap (DeferredRenderer::AtSpanEnd)
		Loaded++;
		Logger::Log("UNOFFICIAL DeferredFoliage record %s: loaded (%u loaded, %u not so far)", name, Loaded, Refused);
		if (IsLeaf(name) && LampsWantedAtStartup()) {	// (2026-11-16) the twin with the froxel's lamps
			static const D3DXMACRO lampOverrides[3] = { { "FOLIAGE_DEFERRED", "1" }, { "LEAF_FORWARD_PLUS", "1" }, { NULL, NULL } };
			ShaderRecordPixel* lamps = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, "Deferred\\FoliageLamps\\", lampOverrides);
			if (lamps && lamps->ShaderHandle) {
				LampRecords[shader] = lamps;
				LampHandles.insert(lamps->ShaderHandle);
				Handles.insert(lamps->ShaderHandle);
				CapturedNormals::FoliageDeferredHandles.insert(lamps->ShaderHandle);
				DeferredSun::FoliageHandles.insert(lamps->ShaderHandle);
				if (forward && forward->ShaderHandle) DeferredLamps::ForwardOf[lamps->ShaderHandle] = forward->ShaderHandle;
				ForwardPlusHeader::Enabled = true;
				LampLoaded++;
				Logger::Log("UNOFFICIAL DeferredFoliageLamps record %s: loaded (the leaf lit by every lamp of its froxel)", name);
			}
			else Logger::Log("[WARNING] UNOFFICIAL DeferredFoliageLamps record %s: did not load: the leaves keep the game's lamp", name);
		}
	}

	// DeferredRenderer::BeginFrame, once a frame (before CapturedNormals::BeginFrame reads CapturedNormals::FoliageDeferred).
	// rendererLive: the renderer is live this frame in mode 2 (which needs DeferredSun outdoors).
	inline void BeginFrame(bool rendererLive) {
		Live = rendererLive && !Records.empty() && WantedAtStartup() && !RtPathTrace::Replacing();
		LampsLive = Live && !LampRecords.empty() && TheSettingManager->GetSettingI(kSection, "DeferredFoliageLamps") != 0 && ManyLamps::FroxelRows() && ManyLamps::TileRowsWanted();
		CapturedNormals::FoliageDeferred = Live;
		{
			static int last = -1;
			const int state = Live ? 1 : 0;
			if (WantedAtStartup() && state != last) {
				last = state;
				Logger::Log("UNOFFICIAL DeferredFoliage: %s (%u records)", Live ? "live: the leaves and the near grass draw their deferred records; DeferredSun adds their sun" :
					"not live (the renderer forward this frame, or the path tracer replaces the picture): the foliage draws forward", Loaded);
			}
		}
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED FOLIAGE %u frames: deferred binds %.1f a frame (leaves %.1f, of them with the froxel's lamps %.1f, grass %.1f), foliage binds outside the span or without records (forward) %.1f",
			(unsigned)StatFrames, StatBinds / f, StatLeafBinds / f, StatLampBinds / f, StatGrassBinds / f, StatOutside / f);
		StatFrames = StatBinds = StatLeafBinds = StatGrassBinds = StatOutside = StatLampBinds = 0;
	}

	// NiD3DPixelShaderEx::SetupShader: the deferred record while the G-buffer span is open and this frame's foliage records are written.
	inline ShaderRecordPixel* Choose(const void* shader, const char* name) {
		auto found = Records.find(shader);
		if (found == Records.end()) return nullptr;
		const bool leaf = IsLeaf(name);
		if (!CapturedNormals::InSpan || !(leaf ? CapturedNormals::LeafNow : CapturedNormals::GrassNow)) { StatOutside++; return nullptr; }
		StatBinds++;
		if (leaf) StatLeafBinds++; else StatGrassBinds++;
		// (2026-11-16) the twin with the froxel's lamps, only with this frame's table and lists finished (else the plain record)
		if (leaf && LampsLive && ManyLamps::Table && !ManyLamps::TableFailed && ManyLamps::TableFrame == ManyLamps::FrameId && ManyLamps::ListsFrame == ManyLamps::FrameId &&
			!ManyLamps::TableJob.pending) {
			auto lamps = LampRecords.find(shader);
			if (lamps != LampRecords.end()) { StatLampBinds++; return lamps->second; }
		}
		return found->second;
	}
}
