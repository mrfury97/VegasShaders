#pragma once

// UNOFFICIAL 2026-10-27 FoldFog ([Main.Main.Misc] FoldFog, live, default on). Ported 2026-10-28 from Conor (mrfury97)'s fork onto P84 v3 (github.com/mrfury97/NVR-Unofficial-Optimized pull request 1, branch
// manylamps-one-pass, commits 82ef371b 7127ac3d 3e73bf06, GPL-3.0-or-later; user: "seems like a good fit"); notes docs/next/MANYLAMPS-PORT.md.
// With LinearScene the lit pass fogs in the stored curve before LinearSceneOut decodes its colour: the same lerp as the fog pass.
//
// F10 census (Vault 21, a casino) after ManyLampsOnePass: the separate fog passes were the largest group of extra draws left
// (Vault 21 ~212 of 2,283 world draws a frame, the casino up to 285 of 1,538). BSShaderPPLightingProperty::GetRenderPasses_2x
// (0x00BDF790) gives an object fog in its last pass's shader (RenderPass::bNoFog, +0x08: "this pass fogs"; without it
// SetupGeometryConstants_Toggles 0x00B795B0 clears Toggles.y, the shaders' useFog) -- except an environment-mapped object: base
// pass, then the env-map pass (BSSM_2x_ENVMAP*, 0x248-0x24B, blended ONE + ONE, PresetEnvMapPasses_2x), then a fog pass
// (BSSM_FOG / FOG_S, 0x252 / 0x254: SRCALPHA / INVSRCALPHA with the fog colour, i.e. lerp(scene, fog colour, fog)).
// On: for an opaque object whose pass list is <one-pass lit pass (BSSM_ADT*)> <env pass> <fog pass>, the lit pass gets bNoFog (its
// shader fogs: lerp(lit, fog colour, fog)) and the fog pass is not drawn. The env pass already weighs its reflection by 1 - fog
// (EnvTemplate.hlsl, as the game's own shader), so the sum is the fog pass's result, the reflection fogged once instead of twice.
// Alpha-blended objects (FOG_A / FOG_SA, or any alpha) keep their fog pass: it fogged what is behind them too. Multi-pass objects
// (light-only + lamp passes) keep it as well. Off: the passes are rebuilt (iEnabledPasses bit 0x20000000, read nowhere else), as before.
namespace FoldFog {

	constexpr UInt32 kEnabledPasses = 0x011F91D8;	// BSShaderManager::iEnabledPasses
	constexpr UInt32 kRebuildBit = 0x20000000;		// (LampsInOnePass uses 0x10000000)
	inline bool On = false, Applied = false;

	// Objects whose lit pass fogged this frame (open addressing, tagged with the frame number: no clearing).
	constexpr unsigned kSlots = 8192;
	inline const void* Key[kSlots] = {};
	inline unsigned Frame[kSlots] = {};
	inline unsigned FrameId = 1;
	inline unsigned long long Folded = 0, Skipped = 0, Frames = 0;

	struct PassArrayView {	// BSShaderProperty::RenderPassArray
		void* vtbl;
		PointShadowForward::RenderPassView** base;
		UInt16 maxSize, size, eSize, growBy;
		UInt32 count;	// +0x10 uiSize
	};
	static_assert(offsetof(PassArrayView, count) == 0x10, "RenderPassArray layout");

	inline bool IsEnv(UInt16 e) { return e >= 0x248 && e <= 0x24B; }
	inline bool IsFog(UInt16 e) { return e == 0x252 || e == 0x254; }	// BSSM_FOG, BSSM_FOG_S (not the alpha ones)

	inline bool Find(const void* geometry, bool insert) {
		unsigned h = (unsigned)(((uintptr_t)geometry >> 4) * 2654435761u) & (kSlots - 1);
		for (unsigned probe = 0; probe < 64; probe++, h = (h + 1) & (kSlots - 1)) {
			if (Frame[h] == FrameId && Key[h] == geometry) return true;
			if (Frame[h] != FrameId) {
				if (insert) { Key[h] = geometry; Frame[h] = FrameId; }
				return insert;
			}
		}
		return false;
	}

	inline void BeginFrame() {
		if (!Applied) {
			Applied = true;
			Logger::Log("UNOFFICIAL FoldFog: on (opaque env-mapped objects fog in their lit pass; their fog pass is not drawn)");
		}
		On = true;
		FrameId++;
		if (!GpuTimer::Enabled) { Folded = Skipped = Frames = 0; return; }
		if (On && ++Frames >= 600) {
			Logger::Log("UNOFFICIAL FoldFog (F10): per frame %.1f lit passes fogged in their shader, %.1f fog passes not drawn", Folded / (double)Frames, Skipped / (double)Frames);
			Folded = Skipped = Frames = 0;
		}
	}

	// Early in the world pass hooks: true = do not draw this pass (a fog pass whose object's lit pass fogged).
	inline bool Skip(void* passPtr, bool world) {
		if (!On || !world || !passPtr) return false;
		PointShadowForward::RenderPassView* pass = (PointShadowForward::RenderPassView*)passPtr;
		if (IsFog(pass->passEnum)) {
			if (!Find(pass->geometry, false)) return false;
			Skipped++;
			return true;
		}
		const char* name = Pointers::Functions::GetPassDescription(pass->passEnum);
		if (!name || strncmp(name, "BSSM_ADT", 8) || !pass->geometry) return false;	// the one-pass lit passes (ADT, ADTS, ADT10, ADTS10, ...)
		const BSShaderProperty* property = (const BSShaderProperty*)pass->geometry->propertyState.m_spShadeProperty;
		if (!property || property->fAlpha < 1.0f || property->fFadeAlpha < 1.0f) return false;
		const NiAlphaProperty* alpha = pass->geometry->propertyState.m_spAlphaProperty;
		if (alpha && (alpha->flags & 1)) return false;	// alpha-blended
		const PassArrayView* passes = (const PassArrayView*)property->pRenderPassArray;
		if (!passes || !passes->base) return false;
		for (UInt32 i = 0; i + 2 < passes->count; i++) {
			if (passes->base[i] != pass) continue;
			const PointShadowForward::RenderPassView* env = passes->base[i + 1];
			const PointShadowForward::RenderPassView* fog = passes->base[i + 2];
			if (!env || !fog || !IsEnv(env->passEnum) || !IsFog(fog->passEnum)) return false;
			pass->unk08 = 1;	// RenderPass::bNoFog: this pass fogs (Toggles.y kept)
			Find(pass->geometry, true);
			Folded++;
			return false;
		}
		return false;
	}
}
