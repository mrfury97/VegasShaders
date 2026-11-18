#pragma once

#include <algorithm>	// (std::max)

// UNOFFICIAL 2026-11-12 RainStateGuard ([Shaders.WetWorld.Main] RainStateGuard, on by default, live): the ONE rain state.
//
// The contract: "it rains" and "the ground is wet" are facts of the weather. They do not depend on whether the WetWorld effect (the
// puddle / wet-look pass) or the Precipitations effect (the falling drops) is switched on. Two amounts, both 0-1:
//   rain    - the rain falling now: up over 0.05 game hours when the weather turns rainy (GameState.isRainy: the game's weather type,
//             or [Shaders.Precipitations.Main] ForceRain), down over 0.07 when it stops;
//   puddles - the standing water: up over [Shaders.WetWorld.Main] Increase game hours, down over Decrease (0, the default: at once).
// Readers: TESR_WetWorldData x / y / z (the WetWorld pass, the water shaders' rain ripples, the persistent cover map), TESR_RainData.x
// (the drops), Factor() = max(rain, puddles) for the Rain setting groups of the land and the objects (Terrain.cpp, PBR.cpp), skin's
// RainWetness (Skin.cpp) and the heat haze (HeatHaze.cpp); and the rain animator itself for the Reflections effect's wetness,
// Specular's Rain look and the volumetric light.
//
// Why here: each amount was animated inside its effect's UpdateConstants, which ShaderManager::UpdateConstants skips for a switched-off
// effect. So it froze at the moment of the switch (in the rain: "wet" for the rest of the session), whichever way the switch was made
// (the menu and scripts go through EffectRecord::SwitchEffect; a reverted menu session, a preset and an effect reload assign Enabled
// directly). Three more ways to a wrong amount end here too:
//   - Animator::Start(0, target) never reaches its target: it reads the value after it has set the new times, GetValue then sees an
//     animation that is already over and returns the OLD target. Increase and Decrease are 0 by default, so the puddles read 0 all
//     through the rain and 1 from the moment it stopped, for good: the Rain groups stayed in after every rain (Move, below);
//   - a restart in the middle of a fade jumped back to the old start value (the same read-after-set; Move);
//   - an animator runs on the game clock: after loading a save from before its start it extrapolates (a 0.07 h fade read two game
//     days early gives about 690, not 0-1) until the clock catches up (SnapIfRewound).
// Tick() drives both animators and writes the amounts. It needs no hook of its own: every reader calls it, and
// PBRShaders::UpdateConstants runs every frame whether or not the object shaders are on (ShaderManager::UpdateConstants), so it runs
// every frame whatever is switched off. It is a few compares and may run several times a frame (the same answer each time).
// RainStateGuard off: Tick() changes nothing (it only notes switches for the log) and each effect animates its own amount as before.
namespace RainState {

	inline int		SeenGuard = -1, SeenWetWorld = -1, SeenPrecipitations = -1;	// the last call's switches (-1: no call yet)
	inline int		SeenLevel = -1;		// the last end the factor reached: 0 dry, 2 fully wet
	inline UInt32	LogLines = 0;		// at most MaxLogLines a session
	const UInt32	MaxLogLines = 60;

	inline bool MayLog() {
		if (LogLines >= MaxLogLines) return false;
		LogLines++;
		return true;
	}

	// 0-1 (a NaN gives 0)
	inline float Unit(float value) {
		return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
	}

	// The game clock is before the animation's start (an earlier save loaded while it ran): it ends at its target now.
	inline void SnapIfRewound(Animator& animator, const char* name) {
		if (!animator.running || !animator.time || !animator.time->GameDaysPassed) return;
		if (animator.time->GameDaysPassed->data * 24 >= animator.startTime) return;
		const float read = animator.GetValue();	// (the extrapolated value; nothing changes while the clock is before the end)
		animator.running = false;
		animator.startValue = animator.endValue;
		if (MayLog()) Logger::Log("UNOFFICIAL RainState: the game clock is before the start of the %s fade (an earlier save loaded while it ran): "
			"it read %.2f, set to its target %.2f.", name, read, animator.endValue);
	}

	// The animator sent to a new target from where it is now. Animator::Start alone reads the value after setting the new times: the
	// old start value in the middle of a fade, and with no duration the old target, where it then stays.
	inline void Move(Animator& animator, float hours, float target) {
		const float from = Unit(animator.GetValue());	// by the old times
		if (hours > 0.0f) {
			animator.Start(hours, target);
			animator.startValue = from;
		}
		else {	// at once
			animator.Initialize(target);
			animator.running = false;
		}
	}

	// At the target, or on its way there.
	inline bool Heading(const Animator& animator, float target) {
		return (animator.running ? animator.endValue : animator.startValue) == target;
	}

	// True while RainStateGuard is on: the two animators are driven and TESR_WetWorldData x / y / z and TESR_RainData.x are this frame's.
	inline bool Tick() {
		ShaderManager* const sm = TheShaderManager;
		WetWorldEffect* const wet = sm ? sm->Effects.WetWorld : nullptr;
		RainEffect* const precipitations = sm ? sm->Effects.Rain : nullptr;
		if (!wet || !precipitations) return false;

		D3DXVECTOR4& data = wet->Constants.Data;
		const bool guard = wet->rainStateGuard;
		const bool raining = sm->GameState.isRainy;
		const float rainBefore = data.x, puddlesBefore = data.z;
		const float before = (std::max)(rainBefore, puddlesBefore);	// what the readers had until now

		if (guard) {
			Animator& fall = precipitations->Constants.RainAnimator;
			Animator& puddles = wet->Constants.PuddlesAnimator;
			SnapIfRewound(fall, "rain");
			SnapIfRewound(puddles, "puddle");
			// By where each animator is or is going, not by remembered edges: whatever state it was left in (this key just switched
			// on, an effect that was off for a while, a save loaded), it is sent to the weather's target.
			const float target = raining ? 1.0f : 0.0f;
			const bool edge = !Heading(fall, target);
			if (edge) Move(fall, raining ? 0.05f : 0.07f, target);	// the fades Precipitations always had
			if (!Heading(puddles, target)) Move(puddles, raining ? wet->increaseRate : wet->decreaseRate, target);	// they fill in the rain and drain after it
			fall.switched = puddles.switched = raining;	// (what the code without the guard compares, should the key go off)
			data.x = Unit(fall.GetValue());
			data.y = raining ? 1.0f : 0.0f;
			data.z = Unit(puddles.GetValue());
			precipitations->Constants.Data.x = data.x;
			if (edge && MayLog()) Logger::Log("UNOFFICIAL RainState: the rain %s (the weather, or ForceRain): rain %.2f to %d over %.2f game hours, puddles %.2f to %d over %.2f; "
				"factor %.2f -> %.2f.", raining ? "started" : "stopped", rainBefore, raining ? 1 : 0, raining ? 0.05f : 0.07f, puddlesBefore, raining ? 1 : 0,
				(std::max)(raining ? wet->increaseRate : wet->decreaseRate, 0.0f), before, (std::max)(data.x, data.z));
		}

		// The log: the mode (at the first call and when the key changes), every switch of the two effects however it was made (the
		// Enabled flags themselves are compared), and each time the factor arrives at dry or fully wet.
		const float after = (std::max)(data.x, data.z);
		const int guardNow = guard ? 1 : 0, wetOn = wet->Enabled ? 1 : 0, precipitationsOn = precipitations->Enabled ? 1 : 0;
		if (SeenGuard != guardNow) {
			if (MayLog()) Logger::Log("UNOFFICIAL RainState: RainStateGuard %s%s. Raining %s; rain %.2f, puddles %.2f; factor %.2f -> %.2f. WetWorld %s, Precipitations %s.",
				SeenGuard < 0 ? "" : "now ", guard ? "on: one rain state, updated every frame whether WetWorld and Precipitations are on or off" :
				"off: each effect animates its own amount, which stays as it is while that effect is off", raining ? "yes" : "no", data.x, data.z, before, after,
				wetOn ? "on" : "off", precipitationsOn ? "on" : "off");
		}
		else {
			const char* const note = guard ? "The rain state follows the weather, not the switch." : "RainStateGuard off: its amount stays as it is while it is off.";
			if (SeenWetWorld != wetOn && MayLog())
				Logger::Log("UNOFFICIAL RainState: WetWorld switched %s%s: factor %.2f -> %.2f (rain %.2f, puddles %.2f, raining %s). %s",
					wetOn ? "on" : "off", before > 0.0f ? " while wet" : "", before, after, data.x, data.z, raining ? "yes" : "no", note);
			if (SeenPrecipitations != precipitationsOn && MayLog())
				Logger::Log("UNOFFICIAL RainState: Precipitations switched %s%s: factor %.2f -> %.2f (rain %.2f, puddles %.2f, raining %s). %s",
					precipitationsOn ? "on" : "off", before > 0.0f ? " while wet" : "", before, after, data.x, data.z, raining ? "yes" : "no", note);
		}
		SeenGuard = guardNow;
		SeenWetWorld = wetOn;
		SeenPrecipitations = precipitationsOn;

		const int level = after <= 0.0f ? 0 : after >= 1.0f ? 2 : 1;
		if (level != 1 && level != SeenLevel) {
			if (SeenLevel >= 0 && MayLog()) Logger::Log("UNOFFICIAL RainState: %s (factor %.2f; rain %.2f, puddles %.2f; raining %s; RainStateGuard %s).",
				level == 2 ? "fully wet" : "dry again", after, data.x, data.z, raining ? "yes" : "no", guard ? "on" : "off");
			SeenLevel = level;
		}
		return guard;
	}

	// The dry-to-wet blend: the stronger of the rain and the puddles. RainStateGuard on: 0-1 and this frame's. Off: the amounts as
	// WetWorld last left them (the old expression of every reader).
	inline float Factor() {
		Tick();
		const WetWorldEffect* const wet = TheShaderManager ? TheShaderManager->Effects.WetWorld : nullptr;
		return wet ? (std::max)(wet->Constants.Data.x, wet->Constants.Data.z) : 0.0f;
	}
}
