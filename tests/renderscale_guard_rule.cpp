// Checks the corner-box / sub-rect seam rule (src/core/RenderScaleGuardRule.h) on synthetic 4-band RGB edge strips:
// white+AO, black, double-squeezed, and partially weapon-occluded corner boxes trigger; gradients, horizontal-only,
// vertical-only, dark fadeouts, high-frequency noise, and NaN/Inf inputs do not.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include "../src/core/RenderScaleGuardRule.h"

static int failures = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } \
	else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

using namespace RenderScaleGuardRule;

static void FillGradient(Rgb h[4][Samples], Rgb v[4][Samples]) {
	for (int i = 0; i < Samples; i++) {
		const float t = (float)i / (Samples - 1);
		for (int b = 0; b < 4; b++) {
			const float y = (float)b * 0.008f;
			h[b][i] = { 0.35f + 0.15f * t - y, 0.32f + 0.12f * t - y, 0.28f + 0.10f * t - y };
			v[b][i] = { 0.40f + 0.12f * sinf(t * 2.0f) + y, 0.36f + 0.10f * cosf(t * 2.0f) + y, 0.30f + y };
		}
	}
}

static uint32_t rng = 0x5A17BEEFu;
static float RandSigned() {
	rng = rng * 1664525u + 1013904223u;
	return ((rng >> 8) / 16777216.0f - 0.5f) * 2.0f;
}

int main() {
	Rgb h[4][Samples], v[4][Samples];

	// 1. White + AO corner-box (exact uploaded bug signature: mid-dark exterior inside, washed ~0.85 outside).
	for (int i = 0; i < Samples; i++) {
		const float wave = 0.04f * sinf(i * 0.25f);
		for (int b = 0; b < 2; b++) {
			h[b][i] = { 0.22f + wave, 0.18f + wave, 0.14f + wave };
			v[b][i] = { 0.45f + wave, 0.34f + wave, 0.29f + wave };
		}
		for (int b = 2; b < 4; b++) {
			h[b][i] = { 0.85f + 0.01f * wave, 0.84f + 0.01f * wave, 0.85f + 0.01f * wave };
			v[b][i] = { 0.86f + 0.01f * wave, 0.85f + 0.01f * wave, 0.86f + 0.01f * wave };
		}
	}
	{ Verdict r = Judge(h, v); CHECK(r.seam && r.hHits >= 50 && r.vHits >= 50, "white + AO corner-box triggers (H=%d/64 mean=%.3f, V=%d/64 mean=%.3f)", r.hHits, r.hMean, r.vHits, r.vMean); }

	// 2. Black corner-box (scene inside top-left, black outside).
	for (int i = 0; i < Samples; i++) {
		for (int b = 0; b < 2; b++) {
			h[b][i] = { 0.35f, 0.30f, 0.24f };
			v[b][i] = { 0.50f, 0.55f, 0.65f };
		}
		for (int b = 2; b < 4; b++) {
			h[b][i] = { 0.0f, 0.0f, 0.0f };
			v[b][i] = { 0.0f, 0.0f, 0.0f };
		}
	}
	{ Verdict r = Judge(h, v); CHECK(r.seam, "black corner-box triggers (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 3. Stale / double-squeezed color corner-box (different color buffer outside).
	for (int i = 0; i < Samples; i++) {
		for (int b = 0; b < 2; b++) {
			h[b][i] = { 0.28f, 0.24f, 0.18f };
			v[b][i] = { 0.30f, 0.25f, 0.20f };
		}
		for (int b = 2; b < 4; b++) {
			h[b][i] = { 0.18f, 0.36f, 0.52f };
			v[b][i] = { 0.50f, 0.42f, 0.38f };
		}
	}
	{ Verdict r = Judge(h, v); CHECK(r.seam, "stale / double-squeezed color corner-box triggers (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 4. 25%-weapon-occluded corner-box (16/64 samples occluded smoothly by full-res weapon, 48/64 still show seam).
	for (int i = 0; i < Samples; i++) {
		if (i >= 40 && i < 56) {
			for (int b = 0; b < 4; b++) {
				h[b][i] = { 0.20f, 0.15f, 0.10f };
				v[b][i] = { 0.22f, 0.16f, 0.11f };
			}
		} else {
			for (int b = 0; b < 2; b++) {
				h[b][i] = { 0.25f, 0.20f, 0.15f };
				v[b][i] = { 0.40f, 0.35f, 0.28f };
			}
			for (int b = 2; b < 4; b++) {
				h[b][i] = { 0.80f, 0.79f, 0.80f };
				v[b][i] = { 0.82f, 0.81f, 0.82f };
			}
		}
	}
	{ Verdict r = Judge(h, v); CHECK(r.seam && r.hHits == 48 && r.vHits == 48, "25%% weapon-occluded corner-box triggers (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 5. Normal smooth scene gradient: must NOT trigger.
	FillGradient(h, v);
	{ Verdict r = Judge(h, v); CHECK(!r.seam, "smooth gradient stays quiet (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 6. Horizontal-only sharp edge (horizon / cinema letterbox): must NOT trigger.
	FillGradient(h, v);
	for (int i = 0; i < Samples; i++) {
		h[0][i] = h[1][i] = { 0.65f, 0.72f, 0.85f };
		h[2][i] = h[3][i] = { 0.05f, 0.05f, 0.05f };
	}
	{ Verdict r = Judge(h, v); CHECK(!r.seam && r.hHits == 64 && r.vHits == 0, "horizontal-only edge does not trigger (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 7. Vertical-only sharp edge (wall corner / doorframe): must NOT trigger.
	FillGradient(h, v);
	for (int i = 0; i < Samples; i++) {
		v[0][i] = v[1][i] = { 0.70f, 0.60f, 0.50f };
		v[2][i] = v[3][i] = { 0.15f, 0.15f, 0.18f };
	}
	{ Verdict r = Judge(h, v); CHECK(!r.seam && r.hHits == 0 && r.vHits == 64, "vertical-only edge does not trigger (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 8. Dark fadeout screen: must NOT trigger even with slight step.
	for (int b = 0; b < 4; b++) {
		for (int i = 0; i < Samples; i++) {
			h[b][i] = v[b][i] = (b < 2) ? Rgb{ 0.01f, 0.01f, 0.01f } : Rgb{ 0.0f, 0.0f, 0.0f };
		}
	}
	{ Verdict r = Judge(h, v); CHECK(!r.seam, "dark fade screen rejected (inLuma=%.4f outLuma=%.4f)", r.hInLuma, r.hOutLuma); }

	// 9. High-frequency noisy / checkerboard foliage texture: intra-side dCtrl prevents false hits.
	for (int i = 0; i < Samples; i++) {
		for (int b = 0; b < 4; b++) {
			const float n0 = 0.25f + 0.18f * RandSigned();
			const float n1 = 0.25f + 0.18f * RandSigned();
			h[b][i] = { n0, n0 * 0.9f, n0 * 0.8f };
			v[b][i] = { n1, n1 * 0.9f, n1 * 0.8f };
		}
	}
	{ Verdict r = Judge(h, v); CHECK(!r.seam, "high-frequency texture noise does not trigger (H=%d/64, V=%d/64)", r.hHits, r.vHits); }

	// 10. NaN / Inf values sanitized safely to 0 without firing.
	const float qnan = std::numeric_limits<float>::quiet_NaN();
	const float pinf = std::numeric_limits<float>::infinity();
	for (int b = 0; b < 4; b++) {
		for (int i = 0; i < Samples; i++) {
			h[b][i] = { qnan, pinf, -pinf };
			v[b][i] = { qnan, 0.0f, 0.0f };
		}
	}
	{ Verdict r = Judge(h, v); CHECK(!r.seam, "NaN / Inf inputs sanitized safely without triggering"); }

	// 11. Secondary scale^2 seam helper checks.
	{
		uint32_t x2 = 0, y2 = 0;
		CHECK(SecondarySeam(1280, 720, 1920, 1080, x2, y2) && x2 == 853 && y2 == 480,
			"SecondarySeam(1280x720 in 1920x1080) -> %ux%u", x2, y2);
		CHECK(SecondarySeam(1480, 832, 1920, 1080, x2, y2) && x2 == 1141 && y2 == 641,
			"SecondarySeam(1480x832 in 1920x1080) -> %ux%u", x2, y2);
		CHECK(!SecondarySeam(1912, 1076, 1920, 1080, x2, y2),
			"SecondarySeam rejects near-native 1912x1076 overlap (too close to primary seam)");
		CHECK(!SecondarySeam(128, 72, 1920, 1080, x2, y2),
			"SecondarySeam rejects tiny sub-24px secondary rect");
		CHECK(!SecondarySeam(1920, 1080, 1920, 1080, x2, y2),
			"SecondarySeam rejects native 1.0x 1920x1080");
	}

	std::printf("renderscale_guard_rule: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
