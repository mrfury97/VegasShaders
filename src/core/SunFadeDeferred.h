#pragma once
// UNOFFICIAL 2026-11-15 fd-smoothsun (docs/next/FD-SMOOTHSUN-RESULTS.md): sun shadows that move with the sun under the full deferred
// renderer, without sliding once an hour and without crawling. The sun the shadow maps are drawn for stands still and steps by StepDegrees
// whenever the real sun has moved that far; at each step the screen pass that draws the sun (DeferredSun today, any deferred light pass that
// draws it later) fades from the maps of before the step to the new ones over FadeSeconds (Includes/Shadow.hlsl SUN_CROSSFADE_PASS,
// GetOldSunShadowPass; ShadowsExteriorEffect::CalculateDeferredSunDir keeps the old atlas and its cascades). Bigger moves (waiting,
// sleeping, fast travel, loading, the sun / moon swap) still jump at once.
// Compiled in when the deferred sun pass is made at startup ([Main.Main.Quality] DeferredRenderer 2: LightFreeRecords) with forward sun
// shadows (the default) and [Shaders.ShadowsExteriors.SunSmoothing] DeferredFade (test only, default on; off at once in game: today's
// 15-degree glide, for the bench's A/B). Then the forward records never get the old forward CrossFade (SUN_CROSSFADE 0 whatever its key
// says), so Terrain Blending, Wetness Effects and RT sun shadows' ForwardLookup keep their room; forward leftovers (glass, the frames the
// deferred renderer draws forward) take the new maps at each step without a fade. The copy of the atlas (~72 MB at the default
// CascadeResolution) is made at the first step that fades, never while this is off.
// A pass that draws the sun under the deferred renderer makes a second record of itself with AppendDefines on its override list, draws that
// record while Fading, binds every TESR_ constant and sampler by name with the record's SetCT (the kept atlas TESR_ShadowAtlasOld on s10
// unless the pass defines SHADOW_OLD_ATLAS_SAMPLER_REG; the kept cascades c146-c166; RT sun shadows' rows then move to c170-c173), and sets
// PassLive each frame (DeferredSun.h does all four).

namespace SunFadeDeferred {
	inline const char* kSection = "Shaders.ShadowsExteriors.SunSmoothing";
	// The step and the fade (docs/next/FD-SMOOTHSUN-RESULTS.md "Why these values"). Half a degree is about the sun's own disc (0.53):
	// a step moves a shadow edge by (caster distance x 0.5 degrees), about the width of the penumbra contact hardening gives that same
	// edge at SunSize 1 (caster distance x 0.53 degrees), so the old and the new edge overlap and the fade reads as the edge moving, not as
	// a second edge. Two seconds: at the default timescale (30) the sun takes about 4 s of real time to move half a degree on average,
	// so one fade ends before the next step is due; a step never starts while a fade runs (it waits, and is then a little bigger).
	inline const float StepDegrees = 0.5f;
	inline const float FadeSeconds = 2.0f;
	// After a step, the fade's clock waits until every cascade has been redrawn for the new sun (the far cascades are redrawn every 4 / 8
	// frames, not at once), at most this many frames: the old maps alone until then, so no cascade is seen switching during the fade.
	inline const int MaxHoldFrames = 12;

	inline int Compiled = -1;
	// Decided once, at the first call (ShaderRecord's define list, the effect's textures, DeferredSun::LoadShaders: all at startup).
	inline bool CompiledIn() {
		if (Compiled < 0 && TheSettingManager)
			Compiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") &&
				TheSettingManager->GetSettingI(kSection, "DeferredFade") ? 1 : 0;
		return Compiled == 1;
	}
	// The forward records' SUN_CROSSFADE (ShaderRecord::BuildDefines; RtSunShadows' ForwardLookup and the effect's atlas copy read the same):
	// the old forward CrossFade key, unless this replaces it.
	inline bool ForwardCrossFade() {
		return TheSettingManager->GetSettingI(kSection, "CrossFade") && TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") &&
			!CompiledIn();
	}
	// A sun pass's override list: the fade compiled in. n = the next free entry; returns the new count. Room for two entries is the caller's.
	inline int AppendDefines(D3DXMACRO* overrides, int n) {
		if (!CompiledIn()) return n;
		overrides[n++] = { "SUN_CROSSFADE", "1" };
		overrides[n++] = { "SUN_CROSSFADE_PASS", "1" };
		return n;
	}

	// Set with the shadow maps' sun, before the frame's world (ShadowsExteriorEffect::CalculateDeferredSunDir): a fade runs this frame, so the
	// pass draws its record with the fade (a second record: compiled in, the fade costs even while it does not run).
	inline bool Fading = false;
	inline bool PassLive = false;	// set each frame by the pass that draws the sun (DeferredSun::BeginFrame): it draws the sun and has its fade record
	// This frame: the deferred stepping (ShadowsExteriorEffect::CalculateSmoothedSunDir). DeferredFade off in game: today's glide.
	inline bool Live() {
		return CompiledIn() && PassLive && TheSettingManager->GetSettingI(kSection, "DeferredFade") != 0;
	}
}
