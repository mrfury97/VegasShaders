// Checks the number half of the in-game parity self-test (src/core/ParityCore.h) on made-up pictures: identical pictures give 0,
// a known patch gives the expected share of pixels over 8 and 16 levels and lands in the right 3 x 3 region, the thresholds are
// strict ("more than"), HDR values above 1 and NaN are handled, the kept picture is overwritten by each capture (one buffer), and
// the |A - B| x 8 picture has the expected value.
#include <cstdio>
#include <cmath>
#include <vector>
#include "../src/core/ParityCore.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<float> Flat(unsigned w, unsigned h, float v) {
	std::vector<float> p((size_t)w * h * 4, v);
	for (size_t i = 3; i < p.size(); i += 4) p[i] = 1.0f;
	return p;
}
static void Put(std::vector<float>& p, unsigned w, unsigned x, unsigned y, float r, float g, float b) {
	float* o = &p[((size_t)y * w + x) * 4];
	o[0] = r; o[1] = g; o[2] = b;
}
static bool Near(double a, double b, double eps = 1e-3) { return fabs(a - b) <= eps; }

int main() {
	const unsigned W = 90, H = 60;	// regions 30 x 20 = 600 pixels each; 5400 in all

	// 1. identical pictures
	{
		auto a = Flat(W, H, 0.4f), b = a;
		ParityCore::Result r = ParityCore::Compare(a, b, W, H);
		CHECK(r.valid && r.all.mean == 0.0 && r.all.gt8 == 0.0 && r.all.gt16 == 0.0, "identical: mean %.4f gt8 %.4f gt16 %.4f", r.all.mean, r.all.gt8, r.all.gt16);
		CHECK(Near(r.all.lumaA, 0.4 * 255.0, 0.05) && Near(r.all.lumaB, r.all.lumaA), "identical: luma %.3f / %.3f (102)", r.all.lumaA, r.all.lumaB);
	}

	// 2. a 30 x 20 patch (one region: the middle, region 4) 20 levels brighter in green only
	{
		auto a = Flat(W, H, 0.4f), b = a;
		for (unsigned y = 20; y < 40; y++) for (unsigned x = 30; x < 60; x++) Put(b, W, x, y, 0.4f, 0.4f + 20.0f / 255.0f, 0.4f);
		ParityCore::Result r = ParityCore::Compare(a, b, W, H);
		const double share = 100.0 * 600 / 5400;
		CHECK(Near(r.all.gt16, share) && Near(r.all.gt8, share), "patch: gt16 %.4f gt8 %.4f (%.4f)", r.all.gt16, r.all.gt8, share);
		CHECK(Near(r.region[4].gt16, 100.0) && r.region[0].gt16 == 0.0 && r.region[8].gt16 == 0.0, "patch: region 4 %.2f%%, 0 %.2f, 8 %.2f", r.region[4].gt16, r.region[0].gt16, r.region[8].gt16);
		// mean: 20 levels on one channel of 1/9 of the pixels = 20 / 3 / 9
		CHECK(Near(r.all.mean, 20.0 / 27.0, 0.01), "patch: mean %.4f (%.4f)", r.all.mean, 20.0 / 27.0);
		CHECK(r.all.lumaB > r.all.lumaA, "patch: B brighter (%.3f > %.3f)", r.all.lumaB, r.all.lumaA);
	}

	// 3. the thresholds are strict: exactly 16 levels is not "more than 16"; 16.5 is. 8.5 is over 8, not over 16.
	{
		auto a = Flat(W, H, 0.25f), b = a, c = a, d = a;
		for (unsigned x = 0; x < W; x++) { Put(b, W, x, 0, 0.25f + 16.0f / 255.0f, 0.25f, 0.25f); Put(c, W, x, 0, 0.25f + 16.5f / 255.0f, 0.25f, 0.25f); Put(d, W, x, 0, 0.25f - 8.5f / 255.0f, 0.25f, 0.25f); }
		const double row = 100.0 / H;
		ParityCore::Result rb = ParityCore::Compare(a, b, W, H), rc = ParityCore::Compare(a, c, W, H), rd = ParityCore::Compare(a, d, W, H);
		CHECK(rb.all.gt16 == 0.0 && Near(rb.all.gt8, row), "exactly 16: gt16 %.4f gt8 %.4f", rb.all.gt16, rb.all.gt8);
		CHECK(Near(rc.all.gt16, row), "16.5: gt16 %.4f (%.4f)", rc.all.gt16, row);
		CHECK(rd.all.gt16 == 0.0 && Near(rd.all.gt8, row), "8.5 darker: gt16 %.4f gt8 %.4f", rd.all.gt16, rd.all.gt8);
		CHECK(Near(rd.region[0].gt8, 100.0 / 20) && rd.region[3].gt8 == 0.0, "8.5 darker: top row only in the top regions (%.3f, %.3f)", rd.region[0].gt8, rd.region[3].gt8);
	}

	// 4. HDR values above 1 count as they are; NaN counts as black; values above 16 clamp
	{
		auto a = Flat(W, H, 3.0f), b = Flat(W, H, 3.1f);
		ParityCore::Result r = ParityCore::Compare(a, b, W, H);
		CHECK(Near(r.all.gt16, 100.0) && Near(r.all.mean, 0.1 * 255.0, 0.05), "HDR 3.0 vs 3.1: gt16 %.2f mean %.3f (25.5)", r.all.gt16, r.all.mean);
		auto n = Flat(W, H, 0.0f);
		Put(n, W, 0, 0, NAN, NAN, NAN);
		ParityCore::Result rn = ParityCore::Compare(Flat(W, H, 0.0f), n, W, H);
		CHECK(rn.all.gt8 == 0.0 && rn.all.mean == 0.0, "NaN = black: gt8 %.4f", rn.all.gt8);
		ParityCore::Result rc = ParityCore::Compare(Flat(W, H, 20.0f), Flat(W, H, 40.0f), W, H);
		CHECK(rc.all.gt8 == 0.0, "above 16 clamps: gt8 %.4f", rc.all.gt8);
	}

	// 5. the kept picture is overwritten: A1, A2 (same), B1 (patch), B2 (same as B1) in one buffer give AA 0, AB the patch, BB 0
	{
		auto a = Flat(W, H, 0.5f), b = a;
		for (unsigned y = 0; y < 20; y++) for (unsigned x = 0; x < 30; x++) Put(b, W, x, y, 0.1f, 0.1f, 0.1f);
		ParityCore::Picture kept; kept.Make(W, H);
		const std::vector<float>* seq[4] = { &a, &a, &b, &b };
		ParityCore::Result pairs[3];
		for (int k = 0; k < 4; k++) {
			ParityCore::Accum acc; acc.Begin(W, H);
			for (unsigned y = 0; y < H; y++) ParityCore::CompareRow(acc, kept, y, &(*seq[k])[(size_t)y * W * 4], k > 0);
			if (k) pairs[k - 1] = ParityCore::Finish(acc);
		}
		CHECK(pairs[0].all.gt8 == 0.0 && pairs[2].all.gt8 == 0.0, "sequence: AA %.4f BB %.4f", pairs[0].all.gt8, pairs[2].all.gt8);
		CHECK(Near(pairs[1].all.gt16, 100.0 / 9) && Near(pairs[1].region[0].gt16, 100.0), "sequence: AB %.4f (11.11), region 0 %.2f", pairs[1].all.gt16, pairs[1].region[0].gt16);
		CHECK(pairs[1].all.lumaB < pairs[1].all.lumaA, "sequence: B darker");
	}

	// 6. the |A - B| x 8 picture: 10 levels x 8 = 80 in green
	{
		auto a = Flat(4, 4, 0.5f), b = a;
		Put(b, 4, 1, 1, 0.5f, 0.5f + 10.0f / 255.0f, 0.5f);
		std::vector<uint32_t> diff;
		ParityCore::Compare(a, b, 4, 4, &diff);
		const uint32_t p = diff[5], q = diff[0];
		CHECK(((p >> 8) & 255) == 80 && ((p >> 16) & 255) == 0 && (p & 255) == 0 && (p >> 24) == 255, "diff picture: %08X (FF005000)", p);
		CHECK(q == 0xFF000000u, "diff picture elsewhere: %08X", q);
	}

	// 7. odd sizes: every pixel lands in some region, and regions add up
	{
		const unsigned w = 7, h = 5;
		ParityCore::Result r = ParityCore::Compare(Flat(w, h, 0.0f), Flat(w, h, 1.0f), w, h);
		unsigned long long sum = 0;
		for (const auto& f : r.region) sum += f.pixels;
		CHECK(sum == (unsigned long long)w * h && r.all.pixels == sum, "7 x 5: %llu of %u pixels in the regions", sum, w * h);
	}

	std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
