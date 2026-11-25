#pragma once

#include <algorithm>	// (std::max)

// UNOFFICIAL 2026-11-21 the ONE rain clock (owner: "move the 'is it raining / is the ground wet' clock to Wetness Effects and remove
// WetWorld"; docs/DESIGN.md "Deferred wetness design" step 4). It was RainStateGuard (2026-11-12), hosted on WetWorld with its own timing
// (Increase / Decrease game hours, Precipitations' 0.05 / 0.07 h fades), so the Rain setting groups, skin and the heat haze could dry on
// another schedule than the ground Wetness Effects and Puddles draw. Now Wetness Effects' weather clock is the only one
// (WetnessEffectsShaders::TickWeather: Raining, Wetness and PuddleWetness, eased by [Shaders.WetnessEffects.Main] WeatherTransitionSpeed),
// and everything reads it through here:
//   Rain()   - the rain falling now, 0-1 (Raining): the drops (TESR_RainData.x), the water's rain rings (TESR_WetnessData.z), the
//              Reflections effect's wetness, Specular's rain look, the volumetric light, the gear's "wet";
//   Factor() - how wet the ground is, 0-1 (max(Raining, Wetness): the same film the land and objects draw): the Rain setting groups of the
//              land and the objects (Terrain.cpp, PBR.cpp), skin (Skin.cpp), the heat haze (HeatHaze.cpp), the gear's mud.
// Puddles' standing water keeps its own, slower level (Puddles.cpp, FillHours / DryHours game hours): it is the water left after the
// ground's film has dried, read only by the shaders that draw it.
//
// The guarantee RainStateGuard gave stays: the clock runs every frame whatever is switched on or off (Wetness Effects itself, Puddles,
// Precipitations, or Wetness Effects left out of the shaders by ShaderFallback), so no amount can freeze at "wet" when an effect goes off
// in the rain. Tick() needs no hook of its own: ShaderManager::UpdateConstants calls it before the families' constants each frame, and
// every reader calls it too (it does the work once a rendered frame, by FrameNumber; UpdateConstants runs twice a frame).
namespace RainState {

	inline UInt32	TickedFrame = ~0u;
	inline float	RainNow = 0.0f, WetNow = 0.0f;
	inline int		SeenLevel = -1;		// the last end the factor reached: 0 dry, 2 fully wet
	inline int		SeenRaining = -1;
	inline UInt32	LogLines = 0;		// at most MaxLogLines a session
	const UInt32	MaxLogLines = 60;

	inline bool MayLog() {
		if (LogLines >= MaxLogLines) return false;
		LogLines++;
		return true;
	}

	// The weather the sky is moving to (its current one).
	inline TESWeather* CurrentWeather() {
		Sky* const sky = Tes ? Tes->sky : nullptr;
		return sky ? sky->firstWeather : nullptr;
	}

	// [Shaders.Precipitations.Main] ForceRain made this frame rainy (ShaderManager::UpdateConstants; FalloutNV.esm has no rainy weather):
	// the game state is rainy, the weather itself is not.
	inline bool ForcedRain() {
		const ShaderManager* const sm = TheShaderManager;
		if (!sm || !sm->GameState.isRainy) return false;
		TESWeather* const weather = CurrentWeather();
		return !(weather && weather->GetWeatherType() == TESWeather::WeatherType::kType_Rainy);
	}

	// [Shaders.Snow.Main] ForceSnow: the snow effects act as if it snowed (no rain then, whatever the weather).
	inline bool ForcedSnow() {
		const ShaderManager* const sm = TheShaderManager;
		if (!sm || !sm->GameState.isSnow) return false;
		TESWeather* const weather = CurrentWeather();
		return !(weather && weather->GetWeatherType() == TESWeather::WeatherType::kType_Snow);
	}

	inline float Unit(float value) {
		return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;	// (a NaN gives 0)
	}

	// This frame's amounts (once a frame; later calls in the same frame change nothing).
	inline void Tick() {
		ShaderManager* const sm = TheShaderManager;
		WetnessEffectsShaders* const wetness = sm ? sm->Shaders.Wetness : nullptr;
		if (!wetness || !TheFrameRateManager) return;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame == TickedFrame) return;
		TickedFrame = frame;

		const float before = (std::max)(RainNow, WetNow);
		wetness->TickWeather();
		RainNow = Unit(wetness->Raining);
		WetNow = Unit(wetness->Wetness);
		const float after = (std::max)(RainNow, WetNow);

		// The drops (Precipitations.fx, TESR_RainData.x), whether that effect is on or off.
		if (RainEffect* const precipitations = sm->Effects.Rain) precipitations->Constants.Data.x = RainNow;
		// The top-down ortho map tells open ground from covered ground for the drops and the water film: kept while it rains outdoors,
		// whichever of them is on (WetWorld asked for it before; Wetness Effects and the HUD drops still ask for their own reasons).
		if (RainNow > 0.0f && sm->GameState.isExterior) sm->orthoRequired = true;

		// The log: rain starting and stopping, and the factor arriving at fully wet or dry.
		const int raining = sm->GameState.isRainy ? 1 : 0;
		if (raining != SeenRaining) {
			if (SeenRaining >= 0 && MayLog()) Logger::Log("UNOFFICIAL RainState: the rain %s (%s): rain %.2f, wet %.2f; Wetness Effects' clock (WeatherTransitionSpeed %.1f)%s.",
				raining ? "started" : "stopped", ForcedRain() ? "ForceRain" : "the weather", RainNow, WetNow, wetness->Settings.WeatherTransitionSpeed,
				wetness->Enabled ? "" : "; Wetness Effects is off, its clock runs for the rest");
			SeenRaining = raining;
		}
		const int level = after <= 0.0f ? 0 : after >= 1.0f ? 2 : 1;
		if (level != 1 && level != SeenLevel) {
			if (SeenLevel >= 0 && MayLog()) Logger::Log("UNOFFICIAL RainState: %s (factor %.2f -> %.2f; rain %.2f, wet %.2f).",
				level == 2 ? "fully wet" : "dry again", before, after, RainNow, WetNow);
			SeenLevel = level;
		}
	}

	// The rain falling now, 0-1, this frame's.
	inline float Rain() {
		Tick();
		return RainNow;
	}

	// The dry-to-wet blend, 0-1, this frame's: the rain, or the wetness it left on the ground.
	inline float Factor() {
		Tick();
		return (std::max)(RainNow, WetNow);
	}
}
