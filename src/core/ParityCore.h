#pragma once

// UNOFFICIAL, TEST ONLY (2026-11-14, full deferred renderer, plan section 6 "Parity"): the number half of the in-game parity
// self-test (ParityTest.h has the game half: the switch, the blocks, the read-backs, the log). No Direct3D or game types here, so
// tests\parity_core.cpp checks it on made-up pictures.
//
// A picture is kept at full size as 16-bit fixed point per channel (value x 4096, 0 .. 16: a 16th of an 8-bit level per step, and
// the scene target's HDR values up to 16 x white), 6 bytes a pixel: 12.4 MB at 1920 x 1080. Only ONE picture per capture point is
// kept: each new capture is compared row by row against the kept one and written over it in the same pass (CompareRow), so a
// measurement of four blocks never holds two pictures at once.
//
// Per pixel the difference is the largest of |a - b| over r, g, b, in 8-bit levels (the encoded value x 255, as a screenshot
// would store it; HDR values above 1 count as they are). Figures, for the whole picture and per region of a 3 x 3 grid (region 0
// top left, row by row, region 8 bottom right):
//   mean  - mean of |a - b| over every pixel and channel, in 8-bit levels (x / 255)
//   gt8   - % of pixels whose largest channel difference is MORE than 8 levels
//   gt16  - the same, more than 16 levels (DEFERRED-RESULTS rounds 3-3e measured "% px > 16" this way on 8-bit shots)
//   lumaA / lumaB - mean Rec.709 luma of each picture, in 8-bit levels (which way a difference goes)
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace ParityCore {

	enum { GridSide = 3, Regions = GridSide * GridSide };
	constexpr float Quant = 4096.0f;						// stored = value x Quant
	constexpr float MaxValue = 65535.0f / Quant;			// 15.9998
	constexpr int Thresholds[2] = { 8, 16 };				// in 8-bit levels

	inline uint16_t Pack(float v) {
		if (!(v == v)) v = 0.0f;							// NaN counts as black
		v = (std::min)((std::max)(v, 0.0f), MaxValue);
		return (uint16_t)(v * Quant + 0.5f);
	}

	struct Picture {
		unsigned width = 0, height = 0;
		std::vector<uint16_t> rgb;							// width x height x 3
		bool Make(unsigned w, unsigned h) {
			width = w; height = h;
			try { rgb.assign((size_t)w * h * 3, 0); }
			catch (...) { width = height = 0; rgb.clear(); rgb.shrink_to_fit(); return false; }
			return true;
		}
		void Free() { width = height = 0; rgb.clear(); rgb.shrink_to_fit(); }
		bool Matches(unsigned w, unsigned h) const { return width == w && height == h && rgb.size() == (size_t)w * h * 3; }
	};

	struct Figures { double mean = 0.0, gt8 = 0.0, gt16 = 0.0, lumaA = 0.0, lumaB = 0.0; unsigned long long pixels = 0; };
	struct Result { bool valid = false; unsigned width = 0, height = 0; Figures all; Figures region[Regions]; };

	// The running sums of one comparison.
	struct Accum {
		unsigned width = 0, height = 0;
		struct Sums { double absSum = 0.0, lumaA = 0.0, lumaB = 0.0; unsigned long long over[2] = {}, pixels = 0; };
		Sums all, region[Regions];
		void Begin(unsigned w, unsigned h) { width = w; height = h; all = Sums(); for (Sums& s : region) s = Sums(); }
	};

	inline double Luma(double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }

	// Row y of a new capture (rgba floats, width x 4) against row y of the kept picture, which then holds the new row. compare
	// false: only stored (the first capture). diffBgra (optional, width pixels): |a - b| x gain as 8-bit BGRA for a picture.
	inline void CompareRow(Accum& acc, Picture& kept, unsigned y, const float* rgba, bool compare, uint32_t* diffBgra = nullptr, float gain = 8.0f) {
		uint16_t* row = &kept.rgb[(size_t)y * kept.width * 3];
		const unsigned w = kept.width;
		const int ry = (int)((unsigned long long)y * GridSide / kept.height);
		// thresholds in stored units: d * 255 > t * Quant  <=>  d > t / 255 (exact integer test)
		for (unsigned x = 0; x < w; x++) {
			const uint16_t n[3] = { Pack(rgba[x * 4]), Pack(rgba[x * 4 + 1]), Pack(rgba[x * 4 + 2]) };
			uint16_t* o = row + (size_t)x * 3;
			if (compare) {
				const int rx = (int)((unsigned long long)x * GridSide / w);
				Accum::Sums& s = acc.region[ry * GridSide + rx];
				int big = 0, sum = 0;
				for (int c = 0; c < 3; c++) { const int d = abs((int)n[c] - (int)o[c]); sum += d; big = (std::max)(big, d); }
				const double la = Luma(o[0], o[1], o[2]), lb = Luma(n[0], n[1], n[2]);
				for (Accum::Sums* t : { &s, &acc.all }) {
					t->absSum += sum; t->lumaA += la; t->lumaB += lb; t->pixels++;
					for (int k = 0; k < 2; k++) if ((long long)big * 255 > (long long)Thresholds[k] * (long long)Quant) t->over[k]++;
				}
				if (diffBgra) {
					unsigned v[3];
					for (int c = 0; c < 3; c++) v[c] = (unsigned)(std::min)(255.0f, abs((int)n[c] - (int)o[c]) * 255.0f / Quant * gain + 0.5f);
					diffBgra[x] = 0xFF000000u | (v[0] << 16) | (v[1] << 8) | v[2];
				}
			}
			o[0] = n[0]; o[1] = n[1]; o[2] = n[2];
		}
	}

	inline Figures FiguresOf(const Accum::Sums& s) {
		Figures f;
		f.pixels = s.pixels;
		if (!s.pixels) return f;
		const double n = (double)s.pixels, level = 255.0 / Quant;
		f.mean = s.absSum / (3.0 * n) * level;
		f.gt8 = 100.0 * s.over[0] / n;
		f.gt16 = 100.0 * s.over[1] / n;
		f.lumaA = s.lumaA / n * level;
		f.lumaB = s.lumaB / n * level;
		return f;
	}

	inline Result Finish(const Accum& acc) {
		Result r;
		r.width = acc.width; r.height = acc.height;
		r.all = FiguresOf(acc.all);
		for (int i = 0; i < Regions; i++) r.region[i] = FiguresOf(acc.region[i]);
		r.valid = acc.all.pixels > 0;
		return r;
	}

	// Two whole pictures (rgba floats each, w x h x 4): the offline test's way in. a is kept, b compared against it.
	inline Result Compare(const std::vector<float>& a, const std::vector<float>& b, unsigned w, unsigned h, std::vector<uint32_t>* diff = nullptr) {
		Picture kept;
		if (!kept.Make(w, h)) return Result();
		Accum acc;
		acc.Begin(w, h);
		if (diff) diff->assign((size_t)w * h, 0);
		for (unsigned y = 0; y < h; y++) CompareRow(acc, kept, y, &a[(size_t)y * w * 4], false);
		for (unsigned y = 0; y < h; y++) CompareRow(acc, kept, y, &b[(size_t)y * w * 4], true, diff ? &(*diff)[(size_t)y * w] : nullptr);
		return Finish(acc);
	}
}
