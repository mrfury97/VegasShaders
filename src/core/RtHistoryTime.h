#pragma once

// UNOFFICIAL 2026-11-07 (perf-history, docs/next/RT-PATH-TRACING.md "perf-history") the RT / PT denoisers' history measured in TIME.
// Plain C++ (no game types): the game (RtDenoise.cpp's HistoryClock, the effects) and the RT test host (tools/nvr-rt-host) use the same
// rules.
//
// Every denoiser history setting is a number of FRAMES that was tuned at about 60 fps (PTDenoiseHistory 32 = 0.53 s there). At 20 fps the
// same 32 frames last 1.6 s: a moving shadow or a walked-off NPC trails three times as long (a 4090 tester at 2560 x 1440 PTRealtime).
// AAA denoisers keep ~0.3-0.5 s whatever the frame rate. So the settings now mean "frames at 60 fps" and the frame count a denoiser uses
// is the same SECONDS at this frame rate:
//     frames = L x (1/60) / dt       (L the setting, dt the smoothed frame time)
// at most 2 x L (a 144 fps frame keeps 64 of PTDenoiseHistory's 32: the half-float histories stay precise), at least 1 frame (or the
// caller's floor), and at most DenoiseMaxHistorySeconds / dt when that cap is set. A keep share k a frame (last frame's weight) becomes
// k^(60 dt): the same decay a second. 0 (off) stays 0. At dt = 1/60 every number is returned unchanged, bit for bit.

#include <cmath>

namespace RtHistoryTime {

	constexpr float kReferenceDt = 1.0f / 60.0f;	// the frame time today's settings were tuned at
	constexpr float kMinDt = 1.0f / 240.0f;			// the smoothed frame time is held within these
	constexpr float kMaxDt = 1.0f / 10.0f;
	constexpr float kMaxGrow = 2.0f;				// a fast frame rate keeps at most this many times the setting's frames
	constexpr float kHitch = 0.25f;					// a frame longer than this (a load, a pause, a hitch) is not a frame rate: the average restarts
	constexpr float kSmoothSeconds = 0.5f;			// the average's time constant

	// RtDenoise's anti-lag fast history (RtDenoiseTemporalRgb / Spec.frag: push fastFrames; it was a hard-coded 4): 4 frames at 60 fps,
	// at least 2 (a restart at one frame would take a single noisy path as the whole history)
	constexpr float kFastFrames = 4.0f, kFastFramesMin = 2.0f;

	// This frame's conversion: the smoothed frame time, the switch (DenoiseTimeBased) and the cap (DenoiseMaxHistorySeconds, 0 off)
	struct Clock {
		float	dt = kReferenceDt;
		bool	on = true;
		float	maxSeconds = 0.0f;
	};

	inline float ClampDt(float dt) { return !(dt > kMinDt) ? kMinDt : dt > kMaxDt ? kMaxDt : dt; }

	// How many 60 fps frames one frame of this clock is worth, held to the grow limit (1 at 60 fps or when off)
	inline float FramesPerReference(const Clock& c) {
		if (!c.on) return 1.0f;
		const float r = kReferenceDt / ClampDt(c.dt);
		return r > kMaxGrow ? kMaxGrow : r;
	}

	// A history length in frames at 60 fps -> frames at this clock. minFrames: the floor (never above the setting itself).
	inline float Frames(float framesAt60, const Clock& c, float minFrames = 1.0f) {
		if (!(framesAt60 > 0.0f) || !c.on) return framesAt60;
		const float dt = ClampDt(c.dt);
		if (dt == kReferenceDt && !(c.maxSeconds > 0.0f)) return framesAt60;	// (bit for bit at 60 fps)
		float f = framesAt60 * (kReferenceDt / dt);
		if (f > framesAt60 * kMaxGrow) f = framesAt60 * kMaxGrow;
		if (c.maxSeconds > 0.0f && f > c.maxSeconds / dt) f = c.maxSeconds / dt;
		const float floor = minFrames < framesAt60 ? minFrames : framesAt60;
		return f < floor ? floor : f;
	}

	// Last frame's weight k (0..1) a frame at 60 fps -> the weight with the same decay a second at this clock: k^(60 dt) (held to the
	// grow limit), and no longer-lived than DenoiseMaxHistorySeconds when that is set (an average of N frames keeps 1 - 1/N)
	inline float Keep(float keepAt60, const Clock& c) {
		if (!(keepAt60 > 0.0f) || !c.on) return keepAt60;
		if (keepAt60 >= 1.0f) return keepAt60;
		const float dt = ClampDt(c.dt);
		if (dt == kReferenceDt && !(c.maxSeconds > 0.0f)) return keepAt60;
		float k = std::pow(keepAt60, 1.0f / FramesPerReference(c));
		if (c.maxSeconds > 0.0f) {
			const float n = c.maxSeconds / dt;
			const float most = n > 1.0f ? 1.0f - 1.0f / n : 0.0f;
			if (k > most) k = most;
		}
		return k;
	}

	// The smoothed frame time: an exponential average of the frames' times over ~kSmoothSeconds, each held to kMinDt..kMaxDt. A hitch
	// (> kHitch: a load, a pause) is left out and restarts the average, as does a camera cut: the next frame's time is the new average.
	struct Smoother {
		float	average = kReferenceDt;
		bool	has = false;
		bool	restart = false;
		void Reset() { has = false; restart = false; average = kReferenceDt; }
		void Feed(float elapsed, bool cut) {
			if (cut) restart = true;
			if (!(elapsed > 0.0f)) return;
			if (elapsed > kHitch) { restart = true; return; }
			const float e = ClampDt(elapsed);
			if (!has || restart) { average = e; has = true; restart = false; return; }
			// every frame the same weight (one frame of ~kSmoothSeconds' worth): the mean frame time, not a time-weighted one (a 30 / 90 fps
			// alternation is 45 frames a second: 22.2 ms here; weighting each frame by its own length would say 27.8 ms)
			const float w = 1.0f - std::exp(-average / kSmoothSeconds);
			average += (e - average) * w;
		}
	};
}
