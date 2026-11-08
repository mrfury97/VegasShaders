// (2026-11-07 perf-history) Scene `historytime`: the RT / PT denoisers' history measured in TIME (src/core/RtHistoryTime.h,
// docs/next/RT-PATH-TRACING.md "perf-history"). A 4090 tester at ~20 fps (2560 x 1440, PTRealtime) saw trails linger ~1.5 s: every
// history setting is a frame count tuned at ~60 fps (PTDenoiseHistory 32 = 0.53 s there, 1.6 s at 20 fps). The settings now mean
// "frames at 60 fps" and RtDenoise::Run gives the passes the same SECONDS (RtHistoryTime::Frames; the anti-lag's fast history, hard-coded
// 4 before, is the push value fastFrames).
//   1. the rules on the CPU: Frames / Keep at 60, 20, 144 fps, the cap, the switch off, RtShadowDenoise::TimeScale, the frame-time smoother.
//   2. at 60 fps the real pass (RtDenoiseTemporalRgb.spv) gives the SAME bits with the time-based push as with today's (32, kFast 4):
//      40 noisy frames with a step, AntiLag 3, PeopleHistory 4 on half the screen.
//   3. a step (the light goes from 0.8 to 0.1, Bernoulli noise every frame): the ghost (mean - 0.1) against SECONDS, at 60 fps and at 20
//      fps time-based, and at 20 fps frame-based (today); AntiLag 0 and 3.
//   4. a still noisy scene from nothing (Bernoulli 0.3): the mean stays unbiased; the spread at 20 fps is the price of the shorter history.
//   5. a lit square moving 40 px/s over a dim floor (a lamp's pool): the trail behind it in PIXELS (= seconds x speed).
#include "RtHistoryTime.h"	// (src/core) the game's rules

static bool sceneHistoryTime(Ctx& c) {
	Host& h = *c.host;
	using namespace RtHistoryTime;
	bool ok = true;
	// ---- 1. the rules
	{
		Clock c60; c60.dt = 1.0f / 60.0f;
		Clock c20; c20.dt = 1.0f / 20.0f;
		Clock c144; c144.dt = 1.0f / 144.0f;
		Clock off = c20; off.on = false;
		Clock cap = c60; cap.maxSeconds = 0.4f;
		Clock cap20 = c20; cap20.maxSeconds = 0.4f;
		struct Row { const char* key; float at60; float floor; };
		const Row rows[] = { { "PTDenoiseHistory", 32.0f, 1.0f }, { "PTDenoisePeopleHistory", 4.0f, 1.0f }, { "PTDenoiseMovingHistory", 8.0f, 1.0f },
			{ "PTDenoiseMirrorHistory", 12.0f, 1.0f }, { "PTDenoiseHistoryFix", 3.0f, 1.0f }, { "anti-lag fast history", kFastFrames, kFastFramesMin },
			{ "ShadowDenoiser DenoiserHistory", 32.0f, 1.0f }, { "DenoiserStabilize", 4.0f, 1.0f }, { "CacheFrames", 16.0f, 1.0f }, { "PTCacheMaxFrames", 8.0f, 1.0f } };
		say("  frames a setting keeps (frames at 60 fps -> 60 / 20 / 144 fps, frame-based at 20, cap 0.4 s at 60 / 20):\n");
		bool rulesOk = true;
		for (const Row& r : rows) {
			const float f60 = Frames(r.at60, c60, r.floor), f20 = Frames(r.at60, c20, r.floor), f144 = Frames(r.at60, c144, r.floor);
			const float fo = Frames(r.at60, off, r.floor), fc = Frames(r.at60, cap, r.floor), fc20 = Frames(r.at60, cap20, r.floor);
			say("    %-32s %5.1f -> %6.2f  %6.2f (%.2f s)  %6.2f  | %5.1f | %5.2f %5.2f\n", r.key, r.at60, f60, f20, f20 / 20.0f, f144, fo, fc, fc20);
			const float want20 = (std::max)(r.at60 / 3.0f, (std::min)(r.floor, r.at60));
			if (f60 != r.at60 || std::fabs(f20 - want20) > 1e-4f || fo != r.at60 || f144 > r.at60 * kMaxGrow + 1e-4f || f144 < r.at60 - 1e-4f) rulesOk = false;
			if (fc > 24.0f + 1e-3f || fc20 > 8.0f + 1e-3f) rulesOk = false;
		}
		const float k60 = Keep(0.9f, c60), k20 = Keep(0.9f, c20), k144 = Keep(0.9f, c144), kf = Keep(0.7f, c20);
		say("    keep shares: RT AO / fog Temporal 0.9 -> %.4f at 60, %.4f at 20 (0.9^3 = 0.7290), %.4f at 144; TemporalMoving 0.7 -> %.4f at 20\n", k60, k20, k144, kf);
		// the same fade a second: k20 per 1/20 s = k60^3 per 3 frames of 1/60 s
		if (k60 != 0.9f || std::fabs(k20 - 0.729f) > 1e-4f || std::fabs(kf - 0.343f) > 1e-4f || !(k144 > 0.9f && k144 < 0.95f) || Keep(0.0f, c20) != 0.0f) rulesOk = false;
		RtShadowDenoise::Settings sd; RtShadowDenoise::TimeScale(sd, c20);
		say("    RtShadowDenoise::TimeScale at 20 fps: maxHistory %.2f, stabilize %.2f, blurFrames %.2f, minHistory %.2f (stays 1)\n", sd.maxHistory, sd.stabilize, sd.blurFrames, sd.minHistory);
		if (std::fabs(sd.maxHistory - 32.0f / 3.0f) > 1e-4f || std::fabs(sd.stabilize - 4.0f / 3.0f) > 1e-4f || sd.minHistory != 1.0f) rulesOk = false;
		// the smoother: a steady 20 fps; a 2 s hitch restarts it on the next frame; a 30 / 90 fps alternation averages; values held to 10..240 fps
		Smoother sm;
		for (int i = 0; i < 100; i++) sm.Feed(0.05f, false);
		const float steady = sm.average;
		sm.Feed(2.0f, false); const float afterHitch = sm.average;
		sm.Feed(1.0f / 60.0f, false); const float restarted = sm.average;
		double alt = 0; for (int i = 0; i < 200; i++) { sm.Feed(i & 1 ? 1.0f / 90.0f : 1.0f / 30.0f, false); if (i >= 100) alt += sm.average / 100.0; }
		sm.Feed(1.0f / 20.0f, true); const float afterCut = sm.average;
		Smoother sm2; for (int i = 0; i < 50; i++) sm2.Feed(0.2f, false); const float slow = sm2.average; sm2.Feed(0.001f, false);
		say("    smoother: steady 20 fps -> %.2f ms; a 2 s hitch left out (%.2f ms) and restarts: next frame %.2f ms; 30/90 fps alternating -> %.2f ms (mean %.2f); a cut restarts: %.2f ms; 5 fps held to %.0f ms, 1000 fps to >= %.2f ms\n",
			steady * 1000, afterHitch * 1000, restarted * 1000, alt * 1000, 0.5 * (1000.0 / 30 + 1000.0 / 90), afterCut * 1000, slow * 1000, sm2.average * 1000);
		if (std::fabs(steady - 0.05f) > 1e-5f || afterHitch != steady || restarted != 1.0f / 60.0f || std::fabs(alt - 0.5 * (1.0 / 30 + 1.0 / 90)) > 0.0015 ||
			afterCut != 0.05f || slow != kMaxDt || sm2.average < kMinDt) rulesOk = false;
		say("    -> %s\n", rulesOk ? "ok" : "BAD");
		ok = ok && rulesOk;
	}

	// ---- the real temporal pass
	FragPass fp;
	if (!loadFragN(h, "RtDenoiseTemporalRgb", 7, 200, 3, fp, 176)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	Tex noisy, depth, captured, hA, hB, oA, oB, oD;
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, F, false, captured) ||
		!makeTex(h, S, S, F, false, hA) || !makeTex(h, S, S, F, false, hB) || !makeTex(h, S, S, F, true, oA) || !makeTex(h, S, S, F, true, oB) ||
		!makeTex(h, S, S, F, true, oD)) return false;
	{	// a floor seen straight down: one view depth; the left half marked as a person (captured normal alpha 2) for part 2
		std::vector<float> dz(size_t(S) * S, cam.eye.z / cam.farZ), cap(size_t(S) * S * 4, 0.0f);
		for (UINT i = 0; i < S * S; i++) cap[i * 4 + 3] = (i % S) < S / 2 ? 2.0f : 1.0f;
		if (!fillTex(depth, dz, 1) || !fillTex(captured, cap, 4)) return false;
	}
	// The push as RtDenoise::Run fills it: today's (clock null: MaxHistory 32 frames, kFast 4 in the shader) or time-based
	auto push = [&](const Clock* clock, float antiLag, float people, bool reset) {
		DnTemporalPush t;
		dnCamera(t, cam, cam, S, S, 32.0f, reset);
		t.antiLag = antiLag; t.peopleCap = people;
		if (clock) {
			t.params[0] = Frames(32.0f, *clock);
			t.fastFrames = Frames(kFastFrames, *clock, kFastFramesMin);
			t.peopleCap = Frames(people, *clock);
		}
		return t;
	};
	auto frame = [&](const std::vector<float>& sig, std::vector<float>& a, std::vector<float>& b, const DnTemporalPush& t) -> bool {
		if (!fillTex(noisy, sig, 4) || !fillTex(hA, a, 4) || !fillTex(hB, b, 4)) return false;
		IUnknown* images[7] = { noisy.t, hA.t, depth.t, hB.t, depth.t, depth.t, captured.t };
		IDirect3DSurface9* targets[3] = { oA.s, oB.s, oD.s };
		return drawN(h, fp, targets, 3, images, &t) && readTex(h, oA, a, 4) && readTex(h, oB, b, 4);
	};
	uint32_t rng = 1;
	auto rnd = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return float(((x >> 22u) ^ x) >> 8) / 16777216.0f; };
	auto bernoulli = [&](std::vector<float>& v, float p) { for (UINT i = 0; i < S * S; i++) { const float x = rnd() < p ? 1.0f : 0.0f; v[i * 4] = v[i * 4 + 1] = v[i * 4 + 2] = x; v[i * 4 + 3] = 1.0f; } };
	auto stats = [&](const std::vector<float>& a, double* spread) {
		double s1 = 0, s2 = 0; int n = 0;
		for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) { const double v = a[(size_t(j) * S + i) * 4]; s1 += v; s2 += v * v; n++; }
		const double m = s1 / n; if (spread) *spread = std::sqrt((std::max)(s2 / n - m * m, 0.0)); return m;
	};
	const size_t N4 = size_t(S) * S * 4;

	// ---- 2. 60 fps: the same bits
	{
		Clock c60; c60.dt = 1.0f / 60.0f;
		std::vector<float> a0(N4, 0.0f), b0(N4, 0.0f), a1(N4, 0.0f), b1(N4, 0.0f), v(N4);
		size_t diff = 0;
		rng = 4242;
		for (int f = 0; f < 40; f++) {
			bernoulli(v, f < 20 ? 0.7f : 0.15f);
			if (!frame(v, a0, b0, push(nullptr, 3.0f, 4.0f, f == 0)) || !frame(v, a1, b1, push(&c60, 3.0f, 4.0f, f == 0))) return false;
			for (size_t i = 0; i < N4; i++) if (std::memcmp(&a0[i], &a1[i], 4) || std::memcmp(&b0[i], &b1[i], 4)) diff++;
		}
		const bool same = diff == 0;
		say("  60 fps, today's push vs the time-based one (40 noisy frames, a step at 20, AntiLag 3, PeopleHistory 4 on half): %zu of %zu values differ -> %s\n",
			diff, N4 * 2 * 40, same ? "ok (bit for bit)" : "BAD");
		ok = ok && same;
	}

	// ---- 3. the step: ghost against seconds
	struct Cfg { const char* name; float dt; bool timed; };
	const Cfg cfgs[3] = { { "60 fps", 1.0f / 60.0f, true }, { "20 fps time-based", 1.0f / 20.0f, true }, { "20 fps frame-based (today)", 1.0f / 20.0f, false } };
	const double marks[5] = { 0.1, 0.2, 0.35, 0.5, 1.0 };
	double t90[2][3] = {}, ghostAt[2][3][5] = {};
	for (int al = 0; al < 2; al++) {
		const float antiLag = al ? 3.0f : 0.0f;
		for (int k = 0; k < 3; k++) {
			Clock clock; clock.dt = cfgs[k].dt;
			const int fps = int(std::lround(1.0 / cfgs[k].dt));
			std::vector<float> a(N4, 0.0f), b(N4, 0.0f), v(N4);
			rng = 99;
			for (int f = 0; f < fps * 3 / 2; f++) { bernoulli(v, 0.8f); if (!frame(v, a, b, push(cfgs[k].timed ? &clock : nullptr, antiLag, 0.0f, f == 0))) return false; }
			t90[al][k] = -1.0;
			for (int f = 0; f < fps * 2; f++) {
				bernoulli(v, 0.1f);
				if (!frame(v, a, b, push(cfgs[k].timed ? &clock : nullptr, antiLag, 0.0f, false))) return false;
				const double t = (f + 1) / double(fps), ghost = stats(a, nullptr) - 0.1;
				for (int m = 0; m < 5; m++) if (std::fabs(t - marks[m]) < 1e-6) ghostAt[al][k][m] = ghost;
				if (t90[al][k] < 0.0 && ghost < 0.07) t90[al][k] = t;
			}
			if (t90[al][k] < 0.0) t90[al][k] = 99.0;	// (not within the 2 s)
		}
		say("  a step 0.8 -> 0.1 (Bernoulli noise), AntiLag %.0f: the ghost (mean - 0.1) at 0.1 / 0.2 / 0.35 / 0.5 / 1.0 s, and when it is under 10 %%:\n", antiLag);
		for (int k = 0; k < 3; k++) {
			char when[32]; if (t90[al][k] > 50.0) std::snprintf(when, sizeof(when), "> 2 s"); else std::snprintf(when, sizeof(when), "%.2f s", t90[al][k]);
			say("    %-27s %.3f %.3f %.3f %.3f %.3f   %s\n", cfgs[k].name, ghostAt[al][k][0], ghostAt[al][k][1], ghostAt[al][k][2], ghostAt[al][k][3], ghostAt[al][k][4], when);
		}
	}
	{
		// time-based 20 fps: the ghost within 0.03 of 60 fps' at every mark, cleared within a frame + 10 % of 60 fps' time; frame-based 20 fps
		// (today) at least 1.5x as long (here: not within the 2 s)
		bool stepOk = true;
		for (int al = 0; al < 2; al++) {
			for (int m = 0; m < 5; m++) if (std::fabs(ghostAt[al][1][m] - ghostAt[al][0][m]) > 0.03) stepOk = false;
			if (std::fabs(t90[al][1] - t90[al][0]) > 0.05 + 0.1 * t90[al][0] || t90[al][2] < 1.5 * t90[al][1]) stepOk = false;
		}
		say("    (time-based 20 fps: the ghost within 0.03 of 60 fps' at every mark, cleared within a frame + 10 %%; frame-based >= 1.5x longer) -> %s\n", stepOk ? "ok" : "BAD");
		ok = ok && stepOk;
	}

	// ---- 4. a still noisy scene from nothing
	{
		// the mean over the pixels (its bias averaged over the last 0.5 s of 2 s) and the per-pixel spread at 0.25 / 0.5 / 2 s
		double bias[2][3] = {}, spread[2][3][3] = {};
		const double at[3] = { 0.25, 0.5, 2.0 };
		for (int al = 0; al < 2; al++)
			for (int k = 0; k < 3; k++) {
				Clock clock; clock.dt = cfgs[k].dt;
				const int fps = int(std::lround(1.0 / cfgs[k].dt));
				std::vector<float> a(N4, 0.0f), b(N4, 0.0f), v(N4);
				rng = 7;
				for (int f = 0; f < fps * 2; f++) {
					bernoulli(v, 0.3f);
					if (!frame(v, a, b, push(cfgs[k].timed ? &clock : nullptr, al ? 3.0f : 0.0f, 0.0f, f == 0))) return false;
					const double t = (f + 1) / double(fps);
					double sp; const double m = stats(a, &sp);
					if (f >= fps * 3 / 2) bias[al][k] += (m - 0.3) / (fps / 2);
					for (int q = 0; q < 3; q++) if (std::fabs(t - at[q]) < 1e-6) spread[al][k][q] = sp;
				}
			}
		bool stillOk = true;
		for (int al = 0; al < 2; al++) {
			say("  a still noisy scene (Bernoulli 0.3) from nothing, AntiLag %d: mean - 0.3 over 1.5-2 s; per-pixel spread at 0.25 / 0.5 / 2 s:\n", al ? 3 : 0);
			for (int k = 0; k < 3; k++) {
				say("    %-27s %+.4f   %.3f %.3f %.3f\n", cfgs[k].name, bias[al][k], spread[al][k][0], spread[al][k][1], spread[al][k][2]);
				if (std::fabs(bias[al][k]) > (al ? 0.03 : 0.005)) stillOk = false;
			}
		}
		// AntiLag 0 is a plain running mean: unbiased (0.005); AntiLag's clamp is not linear on two-valued noise (today's 60 fps shows it too): 0.03
		say("    (|bias| < 0.005 without AntiLag, < 0.03 with it; at 20 fps the shorter history keeps fewer paths: the spread is the price of 3x shorter trails) -> %s\n", stillOk ? "ok" : "BAD");
		ok = ok && stillOk;
	}

	// ---- 5. a moving lit square: the trail in pixels
	{
		// behind the square: the excess light's mean distance (pixels; = speed x its mean age, at most the 90 px travelled), its total a row,
		// and the excess 10 / 20 / 40 px behind (0.25 / 0.5 / 1 s ago)
		double trailPx[2][3] = {}, trailE[2][3] = {}, profile[2][3][3] = {};
		for (int al = 0; al < 2; al++) {
			const float antiLag = al ? 3.0f : 0.0f;
			for (int k = 0; k < 3; k++) {
				Clock clock; clock.dt = cfgs[k].dt;
				const int fps = int(std::lround(1.0 / cfgs[k].dt));
				std::vector<float> a(N4, 0.0f), b(N4, 0.0f), v(N4), exact(size_t(S) * S);
				const double speed = 40.0;	// pixels a second
				const int frames = fps * 2;		// 2 s: the square from x 4 to 84
				for (int f = 0; f <= frames; f++) {
					const double x0 = 4.0 + speed * f / fps;
					for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
						const bool lit = i + 0.5 >= x0 - 8.0 && i + 0.5 < x0 + 8.0 && j >= S / 2 - 8 && j < S / 2 + 8;
						const float val = lit ? 1.0f : 0.1f;
						const size_t p = size_t(j) * S + i;
						v[p * 4] = v[p * 4 + 1] = v[p * 4 + 2] = val; v[p * 4 + 3] = 1.0f; exact[p] = val;
					}
					if (!frame(v, a, b, push(cfgs[k].timed ? &clock : nullptr, antiLag, 0.0f, f == 0))) return false;
					if (f == frames) {	// at 2 s: the square at x 84: the trail behind it
						const double left = x0 - 8.0;
						double moment = 0.0, e = 0.0;
						for (UINT j = S / 2 - 8; j < S / 2 + 8; j++) for (UINT i = 0; i < S; i++) {
							const size_t p = size_t(j) * S + i;
							const double excess = a[p * 4] - exact[p], dist = left - (i + 0.5);
							if (dist > 0.0 && excess > 0.0) { e += excess; moment += excess * dist; }
							for (int q = 0; q < 3; q++) { const double want = q == 0 ? 10.0 : q == 1 ? 20.0 : 40.0; if (std::fabs(dist - (want + 0.5)) < 0.25) profile[al][k][q] += excess / 16.0; }
						}
						trailPx[al][k] = e > 0.0 ? moment / e : 0.0; trailE[al][k] = e / 16.0;	// (a row)
					}
				}
			}
			say("  a lit square (1.0 on 0.1) moving 40 px/s, AntiLag %.0f, at 2 s: the excess light behind it: mean distance, total a row, at 10 / 20 / 40 px:\n", antiLag);
			for (int k = 0; k < 3; k++) say("    %-27s %5.1f px  %6.2f   %.3f %.3f %.3f\n", cfgs[k].name, trailPx[al][k], trailE[al][k], profile[al][k][0], profile[al][k][1], profile[al][k][2]);
		}
		// time-based 20 fps: the trail's profile in pixels (= in seconds) within 0.03 of 60 fps' (AntiLag 0: the plain running mean); the
		// frame-based one sits farther back (its mean distance >= 1.2x: the 90 px travelled cut it off); with AntiLag 3 no more light than today
		bool trailOk = std::fabs(trailPx[0][1] - trailPx[0][0]) <= 3.0 + 0.1 * trailPx[0][0] && trailPx[0][2] >= 1.2 * trailPx[0][1] && trailE[1][1] <= trailE[1][2] + 0.01;
		for (int q = 0; q < 3; q++) if (std::fabs(profile[0][1][q] - profile[0][0][q]) > 0.03) trailOk = false;
		say("    (time-based 20 fps = 60 fps' profile within 0.03 and mean distance within 3 px + 10 %%; frame-based >= 1.2x farther back) -> %s\n", trailOk ? "ok" : "BAD");
		ok = ok && trailOk;
	}
	freeTex({ &noisy, &depth, &captured, &hA, &hB, &oA, &oB, &oD });
	return ok;
}
