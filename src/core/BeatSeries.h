#pragma once

// UNOFFICIAL 2026-11-11 BeatSeries: what the world draw did each frame, as a handful of 32-bit numbers, kept next to the flash / beat
// detector's 64-frame history (Diagnostics.cpp). A tester's interior flicker beats every 3rd frame inside the world draw ("scene before
// Vegas Shaders") and nothing in the log says which input beats with it. The sources below note their state as they run (plain
// increments and a few hashes; nothing drawn, nothing read back, no behaviour changed); when a BEAT or a "period N" FLASH line is
// written, Diagnostics tests every series over the same frames for the same period and phase and adds a "matches:" line.
// Engine-free: no game or D3D types, so every source can include it.
//
// On: set once a frame by Diagnostics::OnFinalPicture (the flash or the beat detector is on). The counters are always counted (an
// increment); the hashes that walk a list are made only while On.
namespace BeatSeries {

	enum Id {
		LampStateHash,		// every lamp GetNearbyLights sees: pointer + its view mask (bIsEnabled) + culled + lit
		LampsOffForView,	// ... of them, switched off for the main view (mask 0xFF: the game's view test or another plugin's)
		LampsCulledOrDark,	// ... of them, culled (APP_CULLED) or too dim to count
		ShadowTableHash,	// PointShadowForward's published table: lamp, cube map, fade in 1/64 steps
		ShadowsPublished,	// its entries
		ShadowsFading,		// ... with |fade| below 1 (FadeIn / FadeOut / fill / invisible-light strengths)
		ClusterHash,		// ShadowCluster: member -> the lamp whose shadow it shares
		ClusterMembers,		// lamps sharing another lamp's shadow
		ClusterFlips,		// members whose group lamp is not last frame's (joined, left or changed group)
		WorldPasses,		// object passes drawn in the world scene (the pass hooks, after every skip)
		LampPasses,			// ... of them lamp-only passes (SLS2045/46, SLS2051-56)
		LampPassesHidden,	// lamp-only passes HiddenObjectCulling skipped
		PassesFolded,		// light-only + extra-lamp passes FoldHighlights folded
		HighlightsSkipped,	// highlight passes FoldHighlights skipped
		LiveCubeDraws,		// live lamp cube maps redrawn
		PoolCubeDraws,		// pool (static) lamp cube maps redrawn
		Count
	};
	inline const char* const Name[Count] = {
		"lamp on/off states (hash)", "lamps switched off for the view", "lamps culled or dark", "published lamp shadow table (hash)",
		"lamp shadows published", "lamp shadows with fade below 1", "ShadowCluster grouping (hash)", "ShadowCluster members",
		"ShadowCluster group changes", "world passes drawn", "lamp passes drawn", "lamp passes skipped by HiddenObjectCulling",
		"passes folded by FoldHighlights", "highlight passes skipped by FoldHighlights", "live lamp cube maps redrawn", "pool lamp cube maps redrawn" };
	inline const bool IsHash[Count] = { true, false, false, true, false, false, true, false, false, false, false, false, false, false, false, false };

	inline bool On = false;
	inline unsigned Cur[Count] = {};

	inline void Add(Id id, unsigned n = 1) { Cur[id] += n; }
	inline void Set(Id id, unsigned v) { Cur[id] = v; }
	// Order-independent (the game's lists come in no fixed order): each item is scrambled on its own, the items summed.
	inline unsigned Item(unsigned long long a, unsigned b = 0, unsigned c = 0) {
		unsigned long long h = (a ^ ((unsigned long long)b << 32) ^ ((unsigned long long)c << 17)) * 0x9E3779B97F4A7C15ull;
		h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
		return (unsigned)h;
	}
	inline void Mix(Id id, unsigned item) { Cur[id] += item | 1u; }

	// Diagnostics, once a frame after the world and the lamp shadows are done: this frame's values out, the counters back to 0.
	inline void Take(unsigned* out) {
		for (int i = 0; i < Count; i++) { out[i] = Cur[i]; Cur[i] = 0; }
	}

	// One series over a beat's window. values / valid: per window frame; beatFrame[i]: frame i is one of the beat's frames (frame % period
	// == phase). A match: (a) the value on every beat frame differs from the value on every other frame -- for counts: the two ranges do
	// not overlap; for hashes: the non-beat frames all hold one value and no beat frame holds it -- or (b) the value changes exactly on
	// the beat frames or exactly on the frames after them (a state that flips with the beat).
	struct Match { bool match = false; int kind = 0; unsigned onLo = 0, onHi = 0, offLo = 0, offHi = 0; int onFrames = 0, offFrames = 0; };
	inline Match Test(const unsigned* values, const bool* valid, const bool* beatFrame, int count, bool hash) {
		Match m;
		bool first[2] = { true, true };
		unsigned lo[2] = {}, hi[2] = {};
		int n[2] = {};
		for (int i = 0; i < count; i++) {
			if (!valid[i]) continue;
			const int k = beatFrame[i] ? 0 : 1;
			if (first[k]) { lo[k] = hi[k] = values[i]; first[k] = false; }
			else { if (values[i] < lo[k]) lo[k] = values[i]; if (values[i] > hi[k]) hi[k] = values[i]; }
			n[k]++;
		}
		m.onLo = lo[0]; m.onHi = hi[0]; m.offLo = lo[1]; m.offHi = hi[1]; m.onFrames = n[0]; m.offFrames = n[1];
		if (n[0] < 3 || n[1] < 3) return m;
		if (hash) {
			bool apart = lo[1] == hi[1];	// the other frames: one value
			for (int i = 0; i < count && apart; i++) if (valid[i] && beatFrame[i] && values[i] == lo[1]) apart = false;
			if (apart) { m.match = true; m.kind = 1; return m; }
		}
		else if (hi[0] < lo[1] || lo[0] > hi[1]) { m.match = true; m.kind = 1; return m; }
		// (b) changes exactly on the beat frames (kind 2) or exactly on the frames after them (kind 3)
		int changes = 0, onBeat = 0, afterBeat = 0, pairs = 0, beatPairs = 0, afterPairs = 0;
		for (int i = 1; i < count; i++) {
			if (!valid[i] || !valid[i - 1]) continue;
			pairs++;
			if (beatFrame[i]) beatPairs++;
			if (beatFrame[i - 1]) afterPairs++;
			if (values[i] == values[i - 1]) continue;
			changes++;
			if (beatFrame[i]) onBeat++;
			if (beatFrame[i - 1]) afterBeat++;
		}
		if (changes >= 3 && pairs > changes) {
			if (onBeat == changes && onBeat == beatPairs) { m.match = true; m.kind = 2; }
			else if (afterBeat == changes && afterBeat == afterPairs) { m.match = true; m.kind = 3; }
		}
		return m;
	}
}
