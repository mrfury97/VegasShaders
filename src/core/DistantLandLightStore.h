#pragma once

// UNOFFICIAL Phase 3 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Decoupled lock-free/main-thread handoff for NIF-calibrated & ESM-extracted distant night lights
// between DistantLandTerrain (worker upload) and DistantLandProto (pre-tonemap billboard render).

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "../effects/Terrain/LandHeights.h"
#include "SettingManager.h"
#include "ShelvedEffects.h"

namespace DistantLandLightStore {
	inline int EffectiveDistantLandMode() {
		if constexpr (ShelvedEffects::Active) return 0;
		if (!TheSettingManager) return 0;
		const float dbg = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar1");
		if (dbg >= 0.5f) return (int)(dbg + 0.5f);
		return std::clamp(TheSettingManager->GetSettingI("Main.Main.Quality", "DistantLand"), 0, 2);
	}

	inline bool ForceNightLights() {
		if constexpr (ShelvedEffects::Active) return false;
		return TheSettingManager && TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar1") >= 3.5f;
	}

	inline int EffectiveSuppressLevel() {
		if constexpr (ShelvedEffects::Active) return 0;
		if (!TheSettingManager) return 0;
		return std::clamp(TheSettingManager->GetSettingI("Main.Main.Quality", "DistantLandSuppressVanilla"), 0, 2);
	}

	inline std::vector<LandHeights::NightLightRef> Lights;
	inline void*                                   WorldSpace = nullptr;
	inline uint32_t                                Generation = 0;

	inline void Set(void* worldPtr, std::vector<LandHeights::NightLightRef>&& lights) {
		Lights = std::move(lights);
		WorldSpace = worldPtr;
		++Generation;
	}

	inline void Clear() {
		std::vector<LandHeights::NightLightRef>().swap(Lights);
		WorldSpace = nullptr;
		++Generation;
	}
}
