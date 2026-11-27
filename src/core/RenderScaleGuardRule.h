#pragma once

// UNOFFICIAL 2026-11-23 RenderScaleGuard (core/Diagnostics.cpp, [Main.Main.Misc] RenderScaleGuard).
// Pure math on 4-band RGB strips straddling the horizontal (y = renderHeight) and vertical (x = renderWidth) RenderScale edges:
//   band 0 = inFar  (10..8 px inside)
//   band 1 = inNear (3..1 px inside)
//   band 2 = outNear (1..3 px outside)
//   band 3 = outFar  (8..10 px outside)
// A corner-box / sub-rect mismatch produces a simultaneous step discontinuity along BOTH boundaries (inNear <-> outNear significantly
// larger than the local intra-side variation inFar <-> inNear and outNear <-> outFar on both edges). Requiring both edges, high hit
// share (MinHits / Samples), minimum mean seam magnitude, and at least one non-black side rejects smooth gradients, horizons/letterboxes,
// vertical doorframes, loading/fade blackouts, and high-frequency textures/noise.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RenderScaleGuardRule {

	constexpr int Samples = 64;

	struct Rgb {
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
	};

	struct Thresholds {
		float	MinEdgeDiff = 0.06f;	// minimum normalized RGB step between inNear and outNear per sample
		float	CtrlRatio = 2.5f;		// seam step must exceed local intra-side variation by at least this factor
		float	CtrlMargin = 0.03f;		// seam step must also exceed local intra-side variation by at least this margin
		int		MinHits = 28;			// >= 28 / 64 (43.75%) samples on BOTH horizontal and vertical seams
		float	MinMeanSeam = 0.08f;	// minimum mean seam step along both horizontal and vertical seams
		float	MinBrightSide = 0.02f;	// reject dark fades / blackouts where both sides are near black
		int		ConfirmNeeded = 3;		// consecutive probed frames before escalating (v2 -> v1, then v1 -> native 1.0x)
	};

	struct Verdict {
		bool	seam = false;
		int		hHits = 0;
		int		vHits = 0;
		float	hMean = 0.0f;
		float	vMean = 0.0f;
		float	hInLuma = 0.0f;
		float	hOutLuma = 0.0f;
		float	vInLuma = 0.0f;
		float	vOutLuma = 0.0f;
	};

	inline Rgb Sanitize(Rgb c) {
		auto fix = [](float v) { return (std::isfinite(v) && v > 0.0f) ? (v > 4.0f ? 4.0f : v) : 0.0f; };
		return { fix(c.r), fix(c.g), fix(c.b) };
	}

	inline float Luma(Rgb c) {
		c = Sanitize(c);
		return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
	}

	inline float ColorDist(Rgb a, Rgb b) {
		a = Sanitize(a);
		b = Sanitize(b);
		const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
		constexpr float invSqrt3 = 0.5773502691896258f;
		return sqrtf(dr * dr + dg * dg + db * db) * invSqrt3;
	}

	inline void EvalEdge(const Rgb s[4][Samples], const Thresholds& t, int& hitsOut, float& meanOut, float& inLumaOut, float& outLumaOut) {
		int hits = 0;
		double seamSum = 0.0, inLumaSum = 0.0, outLumaSum = 0.0;
		for (int i = 0; i < Samples; i++) {
			const Rgb& inFar = s[0][i], inNear = s[1][i], outNear = s[2][i], outFar = s[3][i];
			const float dSeam = ColorDist(inNear, outNear);
			const float dCtrl = (std::max)(ColorDist(inFar, inNear), ColorDist(outNear, outFar));
			seamSum += dSeam;
			inLumaSum += Luma(inNear);
			outLumaSum += Luma(outNear);
			if (dSeam >= t.MinEdgeDiff && dSeam >= t.CtrlRatio * dCtrl && dSeam >= dCtrl + t.CtrlMargin) hits++;
		}
		hitsOut = hits;
		meanOut = (float)(seamSum / Samples);
		inLumaOut = (float)(inLumaSum / Samples);
		outLumaOut = (float)(outLumaSum / Samples);
	}

	inline Verdict Judge(const Rgb h[4][Samples], const Rgb v[4][Samples], const Thresholds& t = Thresholds()) {
		Verdict out;
		EvalEdge(h, t, out.hHits, out.hMean, out.hInLuma, out.hOutLuma);
		EvalEdge(v, t, out.vHits, out.vMean, out.vInLuma, out.vOutLuma);
		out.seam = out.hHits >= t.MinHits && out.vHits >= t.MinHits &&
			out.hMean >= t.MinMeanSeam && out.vMean >= t.MinMeanSeam &&
			(std::max)(out.hInLuma, out.hOutLuma) >= t.MinBrightSide &&
			(std::max)(out.vInLuma, out.vOutLuma) >= t.MinBrightSide;
		return out;
	}

	// Secondary seam coordinates when an un-stretched pass double-squeezes the top-left rect by another RenderScale factor.
	inline bool SecondarySeam(uint32_t rw, uint32_t rh, uint32_t sw, uint32_t sh, uint32_t& rw2, uint32_t& rh2) {
		if (rw == 0 || rh == 0 || sw == 0 || sh == 0 || rw >= sw || rh >= sh) return false;
		const uint32_t x2 = (uint32_t)floorf((float)rw * ((float)rw / (float)sw) + 0.5f);
		const uint32_t y2 = (uint32_t)floorf((float)rh * ((float)rh / (float)sh) + 0.5f);
		if (x2 < 24 || y2 < 24 || x2 + 12 >= rw || y2 + 12 >= rh || x2 + 12 >= sw || y2 + 12 >= sh) return false;
		rw2 = x2;
		rh2 = y2;
		return true;
	}

}
