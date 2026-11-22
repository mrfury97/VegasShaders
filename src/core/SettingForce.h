#pragma once
// UNOFFICIAL 2026-11-14 full deferred renderer, phase 2 (docs/next/FD-INTERIORS-RESULTS.md, FULL-DEFERRED-RENDERER-PLAN.md 4.8): the one switch
// [Main.Main.Quality] DeferredRenderer (restart; 0 off, 1 lamps deferred, 2 all direct light deferred) turns its prerequisites on by itself.
// It does so here, as a read-only overlay on SettingManager::GetSettingI / GetSettingF: while the switch is on at startup, the keys below read
// as the value the renderer needs, whatever the user's file says. Nothing is written into the settings (a menu save keeps the user's own
// values), and with the switch at 0 (off; the default is 2 since 2026-11-15) every key reads as before: one integer compare per read.
// Decided once, at the first GetSettingI / GetSettingF of the session (SettingManager.cpp ForcedValue): the mode is a startup value.

namespace SettingForce {
	inline int State = -1;			// -1 not decided yet, 0 nothing forced, 1 the table below applies
	inline int RendererMode = 0;	// [Main.Main.Quality] DeferredRenderer at startup, 0-2

	struct Entry { const char* section; const char* key; int value; int fromMode; };
	// What the deferred renderer needs (plan 4.8): the G-buffer (CapturedNormals, MaterialBuffer, AlbedoBuffer), linear lighting and
	// LinearScene (the passes add light in linear light), FoldFog (the fog identity of plan 4.1), the forward lamp shadows and the atlas
	// (the lamp pass's shadow tiles), the lamp table on every runtime (LampTableStandalone, LampTableFast: FD-LAMPTABLE-RESULTS.md), the
	// DeferredLamps pass itself with every lamp lit (UnshadowedLamps 1: the critic's item 3), and in mode 2 the lighting-free records and
	// DeferredDistant (the far land, far objects and distant trees drawn inside the G-buffer span: phase 3, docs/next/FD-LOD-RESULTS.md).
	inline const Entry Table[] = {
		{ "Main.Main.Quality", "CapturedNormals", 1, 1 },
		{ "Main.Main.Quality", "MaterialBuffer", 1, 1 },
		{ "Main.Main.Quality", "AlbedoBuffer", 1, 1 },
		{ "Main.Main.Quality", "GBufferV2", 1, 1 },	// (claude/fd-gbuffer's layout v2, once merged: needs AlbedoBuffer; no reader on this branch)
		{ "Shaders.PBR.Linear", "Enabled", 1, 1 },
		{ "Shaders.PBR.Linear", "LinearScene", 1, 1 },
		{ "Main.Main.Misc", "FoldFog", 1, 1 },
		{ "Main.Main.Misc", "LampTableStandalone", 1, 1 },
		{ "Main.Main.Misc", "LampTableFast", 1, 1 },
		{ "Shaders.ShadowsInteriors.Forward", "Enabled", 1, 1 },
		{ "Shaders.ShadowsInteriors.Atlas", "Enabled", 1, 1 },
		{ "Shaders.ShadowsInteriors.Deferred", "Enabled", 1, 1 },
		{ "Shaders.ShadowsInteriors.Deferred", "UnshadowedLamps", 1, 1 },
		{ "Shaders.ShadowsInteriors.Deferred", "LightFreeRecords", 1, 2 },
		{ "Shaders.ShadowsExteriors.Deferred", "Enabled", 1, 2 },
		{ "Main.Main.Quality", "DeferredFoliage", 1, 2 },	// UNOFFICIAL 2026-11-14 phase 3 foliage (core/DeferredFoliage.h)
		{ "Main.Main.Quality", "DeferredDistant", 1, 2 },	// UNOFFICIAL 2026-11-14 (core/DeferredDistant.h): the far world writes the G-buffer and takes the deferred sun
		// UNOFFICIAL 2026-11-14 (the orchestrator's call, FD-LOD-RESULTS.md "not decided"): mode 2 lights outdoors, where fp16 distance is too
		// coarse for sun shadows; the depth route (DeferredDepth 2: the INTZ copy -> fp32 view z) worked on both runtimes (FD-PHASE0-RESULTS.md Q2)
		{ "Main.Main.Quality", "DeferredDepth", 2, 2 },
		// UNOFFICIAL 2026-11-16 (owner: patches of land see-through with terrain blending on) a value forced OFF: terrain blending's Smooth fade
		// turns SRCALPHA / INVSRCALPHA on for every land draw (TerrainShaders::PrepareBlending), and D3D9 blends each render target by its OWN
		// alpha. In mode 2 the land's lighting-free record writes the code 1 + keep into COLOR1.a (TerrainTemplate.hlsl FD_LF_CODE), so its
		// albedo and that code were blended by a weight of 1..2 over whatever was drawn first (the buried rocks and land pieces), on the whole
		// land, not only the seam: DeferredSun then lit those pixels from a mixed record. Smooth 0 is the dither (clip), with alpha 1 and no
		// blend: each texel holds either the land's record or the object's. TAA / FSR / DLSS average the stipple (SharedNoise moves it while
		// they resolve). Mode 1 has no land record (the land writes its albedo alpha 1 there, a replace): mode 2 only.
		{ "Shaders.Terrain.Blending", "Smooth", 0, 2 },
	};

	// The forced value of section / key, when the table applies. Called with State already decided.
	inline bool Find(const char* section, const char* key, int& value) {
		if (State != 1 || !section || !key) return false;
		for (const Entry& e : Table) {
			if (RendererMode < e.fromMode || e.key[0] != key[0] || strcmp(e.key, key) || strcmp(e.section, section)) continue;
			value = e.value;
			return true;
		}
		return false;
	}

	// UNOFFICIAL 2026-11-15 (FD-RELEASE-PREP.md) a floor, not an exact value: linear lighting at a gamma of 1.0 is the same as off (LinearScene
	// is not live, so DeferredSun never runs and mode 2 draws forward everywhere), so while the renderer is on [Shaders.PBR.Linear] Gamma
	// <= 1.0 reads as 2.2 (its default) and NightGamma above 0 and <= 1.0 reads as 0 (follow Gamma). Values above 1.0 are the player's look
	// and stay. Applied at every GetSettingF (SettingManager.cpp), so a live menu change to 1.0 reads as 2.2 too. Every reader of the two
	// keys goes through GetSettingF (PBRShaders::UpdateSettings -> Constants.Linear -> LinearScene::Gamma). The override is noted here (the
	// read holds the settings cache lock) and logged by DeferredRenderer::BeginFrame when the overridden value changes.
	inline float GammaWas = 0.0f, NightGammaWas = 0.0f;	// the player's value that was overridden (0: none)
	inline bool FloorF(const char* section, const char* key, float& value) {
		if (State != 1 || !section || !key || (key[0] != 'G' && key[0] != 'N') || strcmp(section, "Shaders.PBR.Linear")) return false;
		if (!strcmp(key, "Gamma")) {
			if (value > 1.0f) { GammaWas = 0.0f; return false; }
			GammaWas = value > 0.0f ? value : 1.0f;	// (0 = a missing key: shown as 1)
			value = 2.2f;
			return true;
		}
		if (!strcmp(key, "NightGamma")) {
			if (value <= 0.0f || value > 1.0f) { NightGammaWas = 0.0f; return false; }
			NightGammaWas = value;
			value = 0.0f;
			return true;
		}
		return false;
	}

	// The mode at startup (decides it on the first call: SettingManager.cpp).
	inline int Mode() {
		if (State < 0 && TheSettingManager) TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRenderer");
		return State == 1 ? RendererMode : 0;
	}

	// UNOFFICIAL 2026-11-20: features that require the Vegas Shaders DXVK fork (ID3D9NvrExt). Set by DxvkCompute::Initialize
	// when the fork is absent (native D3D9 or stock DXVK) so FSR/DLSS (default on) cannot trigger RenderScale / ScopeOverlay /
	// AmbientLight / ReactiveMask bugs, and HDR output cannot pretend to toggle on DX9. Nothing is written to VegasShaders.toml.
	inline bool NoDxvkFork = false;
	inline bool DxvkRequiredOff(const char* section, const char* key) {
		if (!NoDxvkFork || !section || !key) return false;
		if (key[0] == 'E' && !strcmp(key, "Enabled"))
			return !strcmp(section, "Shaders.TAA.FSR") || !strcmp(section, "Shaders.TAA.DLSS") || !strcmp(section, "Shaders.HDR.Main");
		if (key[0] == 'R' && !strcmp(key, "RayReconstruction") && !strcmp(section, "Shaders.TAA.DLSS"))
			return true;
		return false;
	}
}
