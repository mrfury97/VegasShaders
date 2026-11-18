#pragma once
// UNOFFICIAL 2026-11-14 full deferred renderer, phase 0 (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.4, docs/next/FD-RECORDS-RESULTS.md):
// the lighting-free records. LAMPS_DEFERRED 2 (ObjectTemplate.hlsl / ParallaxTemplate.hlsl / TerrainTemplate.hlsl NVR_LIGHT_DEFERRED) compiles
// every direct light out of a lit template shader: the sun with its cascades, cloud and character shadows, every lamp with its cube / atlas /
// RT shadows, the flashlight and all their highlights. The ambient, glow, fog and every G-buffer write stay; COLOR1.a = 1 - fog. One record a
// shader name, indoors and out (without lights the Interior / Exterior split has nothing left to split). A value of the existing define, so
// the define list grows by one entry as the DeferredLamps records' does (the 62 cap: docs/next/FD-RECORDS-RESULTS.md).
// [Shaders.ShadowsInteriors.Deferred] LightFreeRecords (test only, restart, off): the records are made at shader load and their load or the
// device's refusal logged. Nothing binds them yet (selection is a later phase: the ExteriorRecords / DeferredLamps::Records side-map pattern).
// Off: no record is made, the define lists and every shader as before.
#include "ShaderRecord.h"
#include "DeferredDistant.h"	// UNOFFICIAL 2026-11-14 DeferredDistant: the far land gets one too (TerrainLODTemplate)

namespace LightFreeRecords {
	inline const char* kSection = "Shaders.ShadowsInteriors.Deferred";
	inline const char* kCache = "LightFree\\";	// own cache folder: records of one name and folder overwrite each other
	inline int Wanted = -1;
	inline bool WantedAtStartup() {
		if (Wanted < 0) Wanted = TheSettingManager->GetSettingI(kSection, "LightFreeRecords") != 0;
		return Wanted != 0;
	}

	// UNOFFICIAL 2026-11-15 DeferredHair ([Main.Main.Quality] DeferredHair, restart, test only, off; docs/next/FD-SKIN-RESULTS.md): the object
	// template's HAIR records (a hair mod's alpha-tested hair, drawn in the main list; the game's own hair is blended and drawn after it) get
	// the deferred renderer's records like any object (DeferredRenderer::Eligible), write model 8 (GBufferV2) and are lit by the object model,
	// which is what their forward records light them with (ObjectTemplate.hlsl: HAIR only tints the albedo); DeferredSun takes model 8 (FD_HAIR).
	// A blended hair draw stays forward (DeferredRenderer::BeforeDraw). Needs GBufferV2. Off: hair forward as before.
	inline int HairWanted = -1;
	inline bool HairDeferred() {
		if (HairWanted < 0) HairWanted = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredHair") != 0 && CapturedNormals::V2CompiledIn();
		return HairWanted != 0;
	}

	inline std::unordered_map<const void*, ShaderRecordPixel*> Records;	// NiD3DPixelShaderEx* -> its lighting-free record
	inline unsigned Loaded = 0, Refused = 0;
	// UNOFFICIAL 2026-11-15 (DeferredSun, batch 5) the far land's records (TerrainLODTemplate): DeferredSun asks core/LandLight.h for the far
	// land's own light at their binds (its colour in c3, its direction in the VERTEX constant c25: TerrainLODTemplate.hlsl LightData)
	inline std::unordered_set<const void*> FarLand;	// NiD3DPixelShaderEx* of the far land's shaders that have a record
	inline std::unordered_set<const void*> NearLand;	// (2026-11-15 batch 6) ... of the near land's (TerrainTemplate: c18 / c3)
	// (2026-11-15 batch 6) ... of the object and parallax shaders whose record had a sun term (light 0 = the sun: not OPT / POINT), whose c3 is
	// the sun's colour as the game hands the objects (DeferredSun reads it back at their draw)
	inline std::unordered_set<const void*> SunObjects;
	inline std::unordered_set<const void*> FadeBand;	// (2026-11-15 batch 6) ... of the hand-over band's (TerrainFadeTemplate), for its once-a-session line

	// The overrides that make the record (BuildDefines puts them first): LAMPS_DEFERRED 2 alone.
	inline const D3DXMACRO* Overrides() {
		static const D3DXMACRO overrides[2] = { { "LAMPS_DEFERRED", "2" }, { NULL, NULL } };
		return overrides;
	}

	// Which pixel shaders get one: the lit records of the three G-buffer-writing templates, not the game's added-light passes (the DIFFUSE lamp
	// and ONLY_SPECULAR highlight passes a deferred frame skips: plan 4.5) and not the unlit texture pass (NO_LIGHT).
	inline bool Eligible(const char* name, const ShaderTemplate& tmpl) {
		if (!name || !tmpl.Name) return false;
		const size_t length = strlen(name);
		if (length < 4 || strcmp(name + length - 4, ".pso")) return false;
		// (2026-11-14 DeferredDistant: the far land too, with its G-buffer compiled in: TerrainLODTemplate.hlsl DISTANT_GBUFFER)
		const bool farLand = (!strcmp(tmpl.Name, "TerrainLODTemplate") || !strcmp(tmpl.Name, "TerrainFadeTemplate")) && DeferredDistant::CompiledIn() && CapturedNormals::AlbedoCompiledIn();	// (the code needs the albedo target; 2026-11-15 + the near / far hand-over band: TerrainFadeTemplate.hlsl)
		if (strcmp(tmpl.Name, "ObjectTemplate") && strcmp(tmpl.Name, "ParallaxTemplate") && strcmp(tmpl.Name, "TerrainTemplate") && !farLand) return false;
		for (int d = 0; d < 63 && tmpl.Defines[d].Name; d++) {
			const char* define = tmpl.Defines[d].Name;
			if (!strcmp(define, "DIFFUSE") || !strcmp(define, "ONLY_SPECULAR") || !strcmp(define, "NO_LIGHT")) return false;
		}
		return true;
	}

	// ShaderManager::LoadShader (pixel), after the shader's own records: its lighting-free record, compiled now (at load, never on the render
	// thread). The template as the family gives it (before BuildDefines fills it: LoadShader takes it by value).
	inline void Load(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl) {
		if (!WantedAtStartup() || !shader->ShaderProg[ShaderRecordType::Default] || !Eligible(name, tmpl)) return;
		ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, kCache, Overrides());
		if (!record) {
			Refused++;
			Logger::Log("[WARNING] LIGHT-FREE RECORD %s: did not load (see the device's line above); %u loaded, %u not so far", name, Loaded, Refused);
			return;
		}
		Records[shader] = record;
		if (!strcmp(tmpl.Name, "TerrainLODTemplate")) FarLand.insert(shader);
		else if (!strcmp(tmpl.Name, "TerrainTemplate")) NearLand.insert(shader);
		else if (!strcmp(tmpl.Name, "TerrainFadeTemplate")) FadeBand.insert(shader);
		else if (!strcmp(tmpl.Name, "ObjectTemplate") || !strcmp(tmpl.Name, "ParallaxTemplate")) {
			bool lampFirst = false;
			for (int d = 0; d < 63 && tmpl.Defines[d].Name; d++)
				if (!strcmp(tmpl.Defines[d].Name, "OPT") || !strcmp(tmpl.Defines[d].Name, "POINT")) lampFirst = true;
			if (!lampFirst) SunObjects.insert(shader);
		}
		Loaded++;
		Logger::Log("LIGHT-FREE RECORD %s: loaded (%u loaded, %u not so far)", name, Loaded, Refused);
	}
}
