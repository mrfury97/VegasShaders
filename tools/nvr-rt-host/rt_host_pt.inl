// NvrRtHost, the path tracer's quality scenes (2026-11-07, included by rt_host.cpp after rt_host_light.inl): ReSTIR DI for the lamps
// (PTRestirDI, RtPathTrace.comp RestirLamps + the game's RtRestirRow.h), README.md has the table.

#include "../../src/core/RtRestirRow.h"

static const UINT kFrameRowIndex = 1 + kGridRows;   // RtPathTrace.cpp kFrameRow (49): the lamp image's frame row
static const UINT kEmissiveRowIndex = kFrameRowIndex + 1;   // (2026-11-07 ptaaa-emissive) RtEmissiveRow.h kRow (50)
static const UINT kLampRows = kEmissiveRowIndex + 1;

// ==============================================================================================================================
// Scene: ReSTIR DI (PTRestirDI, flag 262144) at the first hit, DebugView 3 (the first hit's direct light only), Lambert floor + blocks,
// 40 coloured lamps of different reach. The exact answer per pixel is albedo x the sum over EVERY lamp of its light x the CPU's shadow ray
// (the shader's own lift and fixture gap). Every frame the lamp list is shuffled (as the game's list re-sorts by distance when the camera
// moves), so the reservoirs only survive through the game's remap (RtRestirRow::Build). Checked:
//   (a) still camera, frames 8-47: the mean over the frames vs the exact sum: overall ratio within -5 % .. +3 % (the 1 / M estimator is
//       a little dark where reuse crosses shadow edges), and the plain one-lamp pick (flag off) within 2 % (it is unbiased);
//   (b) one frame's error (RMSE against the exact) with ReSTIR < 0.6 x the plain pick's;
//   (c) a camera slide (6 frames of 25 units): the frames after it still < 0.8 x the plain pick's error and their mean within 6 %;
//   (d) control: the same run with the remap broken (each old lamp number taken as the same number) is > 1.2 x worse: the remap matters.
static bool sceneRestirDI(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh block; block.box({ -60, -60, 0 }, { 60, 60, 220 });
	Mesh wall; wall.box({ -500, -20, 0 }, { 500, 20, 300 });
	w.meshes = { floor, block, wall };
	{ Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f }; w.objects.push_back(f); }
	const V3 blocks[] = { { -700, -300, 0 }, { -200, 200, 0 }, { 300, -100, 0 }, { 650, 450, 0 }, { -450, 650, 0 }, { 100, 800, 0 }, { 900, -500, 0 } };
	for (const V3& b : blocks) { Object o; o.mesh = 1; o.offset = b; w.objects.push_back(o); }
	{ Object o; o.mesh = 2; o.offset = { 0, 350, 0 }; w.objects.push_back(o); }
	// 40 lamps: positions on a jittered grid, colours and reaches varied (a few bright ones far apart: the plain pick's worst case)
	struct L { V3 pos; float reach; V3 colour; };
	std::vector<L> lamps;
	uint32_t rng = 12345u;
	auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) / 16777216.0f; };
	for (int y = 0; y < 5; y++) for (int x = 0; x < 8; x++) {
		L l;
		l.pos = { -1400 + x * 400.0f + 120 * (rnd() - 0.5f), -900 + y * 450.0f + 120 * (rnd() - 0.5f), 120.0f + 180.0f * rnd() };
		l.reach = 450.0f + 450.0f * rnd();
		const float b = (x + y) % 7 == 0 ? 3.0f : 0.4f + 0.8f * rnd();
		l.colour = { b * (0.6f + 0.4f * rnd()), b * (0.6f + 0.4f * rnd()), b * (0.6f + 0.4f * rnd()) };
		lamps.push_back(l);
	}
	const int n = int(lamps.size());
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const float albedo = 0.5f;

	// The exact direct light at a scene-space point on the floor (z = 0 in the world): albedo x sum of light x visibility (CPU rays)
	auto exactAt = [&](const Camera& cam, V3 at) {
		const V3 nrm(0, 0, 1);
		const float t = length(at);
		const V3 lift = at + nrm * (1.0f + 0.002f * t);
		double e[3] = {};
		for (const L& l : lamps) {
			const V3 lp = l.pos - cam.eye;
			const V3 d = lp - at;
			const float dist = length(d);
			if (dist >= l.reach || dist < 1e-3f) continue;
			const float x = dist / l.reach, fall = std::pow(std::max(1 - x * x, 0.0f), 2.2f), cs = std::max(d.z / dist, 0.0f);
			if (!(fall * cs > 0)) continue;
			const V3 ld = lp - lift;
			const float ldist = length(ld);
			const Hit hh = trace(w, cam.eye + lift, ld * (1.0f / ldist), 0.0f, std::max(ldist - 16.0f, 0.0f), 0xFF, true);
			if (hh.hit) continue;
			e[0] += l.colour.x * fall * cs; e[1] += l.colour.y * fall * cs; e[2] += l.colour.z * fall * cs;
		}
		return V3(float(e[0] * albedo), float(e[1] * albedo), float(e[2] * albedo));
	};
	// Per pixel: the exact light averaged over 2 x 2 points inside the pixel (the paths jitter there), and whether the pixel is "flat" (the
	// four agree within 4 %: off shadow / reach edges) and on the floor
	struct Ref { bool use = false; V3 e; };
	auto reference = [&](const Camera& cam, std::vector<Ref>& ref) {
		ref.assign(S * S, Ref());
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			V3 sum(0, 0, 0); float lo = 1e30f, hi = -1e30f; bool floorOnly = true;
			for (int s = 0; s < 4; s++) {
				const V3 d = cam.ray((i + (s & 1 ? 0.75f : 0.25f)) / S, (j + (s & 2 ? 0.75f : 0.25f)) / S);
				const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
				if (!hh.hit || hh.object != 0) { floorOnly = false; break; }
				const V3 e = exactAt(cam, d * hh.t);
				sum = sum + e;
				const float y = 0.2126f * e.x + 0.7152f * e.y + 0.0722f * e.z;
				lo = std::min(lo, y); hi = std::max(hi, y);
			}
			Ref& r = ref[j * S + i];
			r.e = sum * 0.25f;
			r.use = floorOnly && hi > 0.02f && (hi - lo) <= 0.04f * hi;
		}
	};
	auto luma = [](const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; };

	// One run of `frames` frames: each frame the lamp list shuffled, the frame row from the game's RtRestirRow::Build, one pass, its display
	// read. moves[f] = the camera of frame f. flags: the pass's; brokenRemap: the control (each old number taken as itself).
	struct Run { std::vector<std::vector<float>> frames; };
	auto run = [&](const std::vector<Camera>& moves, uint32_t flags, bool brokenRemap, Run& out) {
		RtRestirRow::State state;
		RtRestirRow::Keys keys;
		keys.radius = 6.0f;   // (~30 world units on the floor here, as 16 pixels at 720p+)
		std::vector<int> order(n);
		for (int i = 0; i < n; i++) order[i] = i;
		uint32_t shuffle = 777u;
		out.frames.clear();
		for (size_t f = 0; f < moves.size(); f++) {
			const Camera& cam = moves[f];
			for (int i = n - 1; i > 0; i--) { shuffle = shuffle * 1664525u + 1013904223u; std::swap(order[i], order[(shuffle >> 8) % uint32_t(i + 1)]); }
			std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
			std::vector<float> list(size_t(n) * 8);
			for (int k = 0; k < n; k++) {
				const L& l = lamps[order[k]];
				const V3 q = l.pos - cam.eye;
				const float v[8] = { q.x, q.y, q.z, l.reach, l.colour.x, l.colour.y, l.colour.z, 0.0f };
				std::memcpy(&image[size_t(k) * 8], v, sizeof(v)); std::memcpy(&list[size_t(k) * 8], v, sizeof(v));
			}
			PathConstants k = PathConstants();
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float((uint32_t(f + 1) * 7919u) & 0xFFFFFu);
			put4(k.accum, 0.0f, 1.0f, 1.0f, float(flags));
			put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0, 0, 0, 0); put4(k.skyHorizon, 0, 0, 0, 0);
			put4(k.misc, float(n), 0.0f, 60000.0f, 3.0f);
			k.rect[3] = 1.0f;
			const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
			const float eye[3] = { 0, 0, 0 };
			RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), list.data(), n, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
			if (const char* e = std::getenv("NVR_RT_HOST_DI_SPATIAL"))   // (a test knob: neighbours a pixel)
				 image[size_t(kLightTexels) * kFrameRowIndex * 2 + 21] = float(std::atoi(e));
			if (brokenRemap) for (int j = 0; j < n; j++) if (image[size_t(j) * 8 + 7] > 0.0f) image[size_t(j) * 8 + 7] = float(j + 1);
			if (!h.Upload(gM.ptLamps, image)) return false;
			const std::vector<int> imgs{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
				c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(w, ids, cam.eye)) return false;
			std::vector<float> disp;
			if (!h.Read(c.ptDisplay, disp)) return false;
			h.EndFrame();
			out.frames.push_back(std::move(disp));
		}
		return true;
	};
	// the statistics of frames [from, to) against the reference: mean ratio (sum of the frames' mean luma / sum of the exact), the frames'
	// RMSE (luma), the share of used pixels whose mean is off by > 15 %
	struct Stats { double ratio = 0, rmse = 0, off15 = 0, noise = 0; int used = 0; };
	const Run* noiseRef = nullptr;   // the plain pick's run: its frames' mean is the noise metric's reference (unbiased, no sub-pixel model)
	auto stats = [&](const Run& r, size_t from, size_t to, const std::vector<Ref>& ref) {
		Stats s; double sumMean = 0, sumExact = 0, se = 0, sn = 0; int seN = 0, off = 0;
		for (size_t p = 0; p < size_t(S) * S; p++) {
			if (!ref[p].use) continue;
			const float ex = 0.2126f * ref[p].e.x + 0.7152f * ref[p].e.y + 0.0722f * ref[p].e.z;
			double m = 0;
			double pm = 0;
			if (noiseRef) { for (size_t f = from; f < to; f++) pm += luma(&noiseRef->frames[f][p * 4]); pm /= double(to - from); }
			for (size_t f = from; f < to; f++) { const float y = luma(&r.frames[f][p * 4]); m += y; se += double(y - ex) * (y - ex); sn += (y - pm) * (y - pm); seN++; }
			m /= double(to - from);
			sumMean += m; sumExact += ex; s.used++;
			if (std::fabs(m / ex - 1.0) > 0.15) off++;
		}
		s.ratio = sumExact > 0 ? sumMean / sumExact : 0; s.rmse = seN ? std::sqrt(se / seN) : 0; s.noise = seN ? std::sqrt(sn / seN) : 0; s.off15 = s.used ? double(off) / s.used : 1;
		return s;
	};

	// (a narrow view: a pixel ~5 units on the floor, as the game's ~1-3 at 720p+; at 36 units a lamp's light changed 10-20 % inside one
	// pixel and the jittered hits made every reused weight noisy, a host-only effect)
	const Camera camA = Camera::lookDir({ 0, -1900, 1500 }, { 0, 1900, -1500 }, { 0, 0, 1 }, 8.0f, 8.0f);
	std::vector<Ref> refA, refB;
	reference(camA, refA);
	const int still = 48;
	std::vector<Camera> stillMoves(still, camA);
	Run plain, restir, broken;
	if (!run(stillMoves, 1024u | 2048u, false, plain) || !run(stillMoves, 1024u | 2048u | 262144u, false, restir) ||
		!run(stillMoves, 1024u | 2048u | 262144u, true, broken)) return false;
	{   // the error split: frame-to-frame noise per pixel, and each pixel's mean against the plain pick's mean
		for (const Run* rr : { &plain, &restir }) {
			double var = 0, bias = 0; int c2 = 0;
			for (size_t q = 0; q < size_t(S) * S; q++) {
				if (!refA[q].use) continue;
				double m = 0, m2 = 0, pm = 0;
				for (size_t f2 = 8; f2 < size_t(still); f2++) { const double y2 = luma(&rr->frames[f2][q * 4]); m += y2; m2 += y2 * y2; pm += luma(&plain.frames[f2][q * 4]); }
				m /= (still - 8); m2 /= (still - 8); pm /= (still - 8);
				var += m2 - m * m; bias += (m - pm) * (m - pm); c2++;
			}
			say("    %s: per-pixel std over frames %.4f, per-pixel mean vs the plain mean RMS %.4f", rr == &plain ? "plain" : "restir", std::sqrt(var / c2), std::sqrt(bias / c2)); say("%c", 10);
		}
	}
	noiseRef = &plain;
	const Stats sp = stats(plain, 8, still, refA), sr = stats(restir, 8, still, refA), sb = stats(broken, 8, still, refA);
	say("  still, frames 8-47 (%d flat floor pixels): plain pick mean x%.4f, noise %.4f (RMSE vs exact %.4f); ReSTIR DI mean x%.4f, noise %.4f "
		"(%.2f x plain), RMSE vs exact %.4f, %.1f %% of pixels off > 15 %%; broken remap noise %.4f (%.2f x ReSTIR)\n", sr.used, sp.ratio, sp.noise, sp.rmse,
		sr.ratio, sr.noise, sr.noise / sp.noise, sr.rmse, 100.0 * sr.off15, sb.noise, sb.noise / sr.noise);
	// the slide: 24 frames still at A, 6 frames of 25 units sideways, 10 frames at B
	std::vector<Camera> slide(24, camA);
	for (int s = 1; s <= 6; s++) slide.push_back(Camera::lookDir(camA.eye + V3(25.0f * s, 0, 0), camA.f, { 0, 0, 1 }, 8.0f, 8.0f));
	const Camera camB = slide.back();
	for (int s = 0; s < 10; s++) slide.push_back(camB);
	reference(camB, refB);
	Run plainS, restirS;
	if (!run(slide, 1024u | 2048u, false, plainS) || !run(slide, 1024u | 2048u | 262144u, false, restirS)) return false;
	noiseRef = &plainS;
	const Stats tp = stats(plainS, 30, 40, refB), tr = stats(restirS, 30, 40, refB);
	say("  after a 150-unit slide (frames 30-39 at B): plain mean x%.4f, noise %.4f; ReSTIR DI mean x%.4f, noise %.4f (%.2f x plain)\n",
		tp.ratio, tp.noise, tr.ratio, tr.noise, tr.noise / tp.noise);
	bool finite = true;
	for (const Run* r : { &restir, &restirS, &broken }) for (const auto& fr : r->frames) for (float v : fr) if (!std::isfinite(v)) finite = false;
	const bool pass = finite && sr.used > int(S * S) / 4 && std::fabs(sp.ratio - 1.0) < 0.02 && std::fabs(sr.ratio - 1.0) < 0.02 && sr.noise < 0.6 * sp.noise &&
		tr.noise < 0.8 * tp.noise && std::fabs(tr.ratio - 1.0) < 0.03 && sb.noise > 1.2 * sr.noise;
	say("  -> %s%s\n", pass ? "ok" : "BAD", finite ? "" : " (NaN / inf in the output)");
	if (!pass) {
		std::vector<float> mean(size_t(S) * S * 4, 0.0f), ex(size_t(S) * S * 4, 0.0f);
		for (size_t f = 8; f < size_t(still); f++) for (size_t i = 0; i < mean.size(); i++) mean[i] += restir.frames[f][i] / float(still - 8);
		for (size_t p = 0; p < size_t(S) * S; p++) { ex[p * 4] = refA[p].e.x; ex[p * 4 + 1] = refA[p].e.y; ex[p * 4 + 2] = refA[p].e.z; ex[p * 4 + 3] = refA[p].use ? 1.0f : 0.0f; }
		dumpPfm("restirdi_mean_r", mean, S, S, 4, 0); dumpPfm("restirdi_exact_r", ex, S, S, 4, 0); dumpPfm("restirdi_exact_use", ex, S, S, 4, 3);
		dumpPfm("restirdi_frame_r", restir.frames[still - 1], S, S, 4, 0); dumpPfm("restirdi_plain_frame_r", plain.frames[still - 1], S, S, 4, 0);
	}
	releaseWorld(h, ids);
	return pass;
}

// ==============================================================================================================================
// Scene: the radiance cache (PTRadianceCache, flag 524288 + RtPathTraceCache.spv's resolve pass after each frame). The furnace's open box
// (5 faces, 2000 x 2000 x 1500) of albedo 0.8 under a uniform sky L, seen from above: deep inside it most of the light has bounced several
// times. Reference: the same tracer with 32 bounces, no cache, 8 frames x 16 paths. Checked:
//   (a) 2 bounces WITHOUT the cache lose light here (mean < 0.97 x the reference: the scene really needs multi-bounce light);
//   (b) 2 bounces + the cache, frames 40-79 (one path a pixel): mean within 2.5 % of the reference, every 16 x 16 pixel tile within 6 %;
//   (c) the sky halves: 60 frames later (frames 40-59 of them) the mean is within 3 % of half the reference (the cache follows the light).
static int gCacheShader = -1;
static bool sceneRadianceCache(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh box; const float half = 1000, height = 1500;
	box.quad({ -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 });   // floor
	box.quad({ -half, -half, 0 }, { -half, half, 0 }, { -half, half, height }, { -half, -half, height });
	box.quad({ half, -half, 0 }, { half, -half, height }, { half, half, height }, { half, half, 0 });
	box.quad({ -half, -half, 0 }, { -half, -half, height }, { half, -half, height }, { half, -half, 0 });
	box.quad({ -half, half, 0 }, { half, half, 0 }, { half, half, height }, { -half, half, height });
	w.meshes.push_back(box);
	{ Object o; o.mesh = 0; o.albedo = { 0.8f, 0.8f, 0.8f }; w.objects.push_back(o); }
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.2f, 2.2f);
	const V3 L = { 0.8f, 0.6f, 0.4f };
	const int log2Entries = 16;
	const unsigned base4096 = (12u * S * S + 4095u) / 4096u;   // after the ReSTIR DI reservoirs (RtPathTrace.cpp CacheBaseWords; 2026-11-07: 6 words a reservoir)
	RtRestirRow::State state;
	RtRestirRow::Keys keys;
	keys.cacheLog2 = float(log2Entries); keys.cacheBase4096 = float(base4096); keys.cacheCell = 8.0f; keys.cacheMaxSamples = std::getenv("NVR_RT_HOST_CACHE_MAX") ? float(std::atof(std::getenv("NVR_RT_HOST_CACHE_MAX"))) : 256.0f; keys.cacheMinSamples = 8.0f;
	uint32_t frame = 0;
	// one frame: bounces, paths, the cache on / off, the sky's scale; its picture (display rgb) back
	auto trace = [&](int bounces, int paths, bool cache, float sky, std::vector<float>& out) {
		std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
		PathConstants k = PathConstants();
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = float((++frame * 7919u) & 0xFFFFFu);
		put4(k.accum, 0.0f, float(bounces), float(paths), float(1024u | (cache ? 524288u : 0u)));
		put4(k.sunDir, 0, 0, 1, 0);
		put4(k.skyZenith, L * sky, 1.0f); put4(k.skyHorizon, L * sky, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f);
		k.rect[3] = 1.0f;
		const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
		const float eye[3] = { 0, 0, 0 };
		RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), nullptr, 0, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
		if (!h.Upload(gM.ptLamps, image)) return false;
		const std::vector<int> imgs{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
			c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (cache) {
			const float frames = std::getenv("NVR_RT_HOST_CACHE_FRAMES") ? float(std::atof(std::getenv("NVR_RT_HOST_CACHE_FRAMES"))) : 8.0f;
			const float params[8] = { float(1u << log2Entries), float(base4096), keys.cacheMaxSamples, 120.0f, frames, 0.0f, 0.0f, 0.0f };
			h.AddPass(gCacheShader, {}, 0u, params, sizeof(params), ((1u << log2Entries) + 63) / 64, 1);
		}
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (!h.Read(c.ptDisplay, out)) return false;
		h.EndFrame();
		return true;
	};
	auto accumulate = [&](int bounces, int paths, bool cache, float sky, int frames, int keepFrom, std::vector<double>& mean) {
		mean.assign(size_t(S) * S * 3, 0.0);
		std::vector<float> f;
		for (int i = 0; i < frames; i++) {
			if (!trace(bounces, paths, cache, sky, f)) return false;
			if (i < keepFrom) continue;
			for (size_t p = 0; p < size_t(S) * S; p++) for (int ch = 0; ch < 3; ch++) mean[p * 3 + ch] += std::isfinite(f[p * 4 + ch]) ? f[p * 4 + ch] : 1e6f;
		}
		for (double& v : mean) v /= double(frames - keepFrom);
		return true;
	};
	auto luma = [](const std::vector<double>& m, size_t p) { return 0.2126 * m[p * 3] + 0.7152 * m[p * 3 + 1] + 0.0722 * m[p * 3 + 2]; };
	// the overall ratio and the 16 x 16 tiles' ratio range against a reference (scaled)
	auto compare = [&](const std::vector<double>& a, const std::vector<double>& ref, double scale, double& ratio, double& lo, double& hi) {
		double sa = 0, sr = 0;
		lo = 1e30; hi = -1e30;
		for (UINT ty = 0; ty < S / 16; ty++) for (UINT tx = 0; tx < S / 16; tx++) {
			double ta = 0, tr = 0;
			for (UINT y = 0; y < 16; y++) for (UINT x = 0; x < 16; x++) { const size_t p = size_t(ty * 16 + y) * S + tx * 16 + x; ta += luma(a, p); tr += luma(ref, p) * scale; }
			sa += ta; sr += tr;
			lo = std::min(lo, ta / tr); hi = std::max(hi, ta / tr);
		}
		ratio = sa / sr;
	};
	std::vector<double> ref, noCache, withCache, halved;
	if (!accumulate(32, 16, false, 1.0f, 8, 0, ref)) return false;
	if (!accumulate(2, 4, false, 1.0f, 16, 0, noCache)) return false;
	if (!accumulate(2, 1, true, 1.0f, 80, 40, withCache)) return false;
	if (!accumulate(2, 1, true, 0.5f, 60, 40, halved)) return false;
	double r0, lo0, hi0, r1, lo1, hi1, r2, lo2, hi2;
	compare(noCache, ref, 1.0, r0, lo0, hi0);
	compare(withCache, ref, 1.0, r1, lo1, hi1);
	compare(halved, ref, 0.5, r2, lo2, hi2);
	say("  open box albedo 0.8 under the sky (reference: 32 bounces, 128 paths a pixel): 2 bounces no cache x%.4f (tiles %.3f .. %.3f); "
		"2 bounces + the cache x%.4f (tiles %.3f .. %.3f); the sky halved, 60 frames later x%.4f of half (tiles %.3f .. %.3f)\n", r0, lo0, hi0, r1, lo1, hi1, r2, lo2, hi2);
	const bool pass = r0 < 0.97 && std::fabs(r1 - 1.0) < 0.025 && lo1 > 0.94 && hi1 < 1.06 && std::fabs(r2 - 1.0) < 0.03;
	say("  -> %s\n", pass ? "ok" : "BAD");
	if (!pass) {
		std::vector<float> a(size_t(S) * S * 4), b(size_t(S) * S * 4);
		for (size_t p = 0; p < size_t(S) * S; p++) for (int ch = 0; ch < 3; ch++) { a[p * 4 + ch] = float(withCache[p * 3 + ch]); b[p * 4 + ch] = float(ref[p * 3 + ch]); }
		dumpPfm("cache_with_r", a, S, S, 4, 0); dumpPfm("cache_ref_r", b, S, S, 4, 0);
	}
	releaseWorld(h, ids);
	return pass;
}

// ==============================================================================================================================
// Scene: the specular lobe's energy (PTSpecular, flag 128): the furnace's open box made of white METAL (a textured record with the material
// rule metal 1, roughness 3/7 or 6/7; albedo 1 = F0 1) under a uniform sky L, 48 bounces, 16 paths x 4 frames. A metal of F0 1 reflects all
// it gets, so every pixel must show L. Checks the GGX sampling (visible normals) against its density (BsdfPdf) and the multiple-scattering
// compensation (a single-scattering lobe kept 35-60 % at roughness 1: section 49). Mean / L within 2 % (3/7) or 3 % (6/7), no NaN; and one
// bounce off a white metal floor = L at every view (printed by view cosine), mean within 1.5 %.
static bool sceneSpecularFurnace(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh box; const float half = 1000, height = 1500;
	box.quad({ -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 });
	box.quad({ -half, -half, 0 }, { -half, half, 0 }, { -half, half, height }, { -half, -half, height });
	box.quad({ half, -half, 0 }, { half, -half, height }, { half, half, height }, { half, half, 0 });
	box.quad({ -half, -half, 0 }, { -half, -half, height }, { half, -half, height }, { half, -half, 0 });
	box.quad({ -half, half, 0 }, { half, half, 0 }, { half, half, height }, { -half, half, height });
	box.uv.assign(box.pos.size() * 2, 0.0f);
	w.meshes.push_back(box);
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.2f, 2.2f);   // (as the furnace: every camera ray enters the box)
	const V3 L = { 0.8f, 0.6f, 0.4f };
	bool ok = true;
	const bool noTable = std::getenv("NVR_RT_HOST_NO_ENERGY") != nullptr;   // (a control: the lobe without its compensation)
	for (int k7 : { 3, 6 }) {
		Object o; o.mesh = 0; o.albedo = { 1, 1, 1 }; o.textured = true; o.alphaSlot = 50; o.rough = k7 / 7.0f; o.metal = 1.0f;
		w.objects = { o };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		double sum[3] = {}; int nan = 0, n = 0;
		for (int f = 0; f < 16; f++) {   // (16 frames: at roughness 6/7 four frames gave 0.99 .. 1.09 by seed)
			PathConstants k = PathConstants();
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float(101 + f * 7919 + (std::getenv("NVR_RT_HOST_SEED") ? std::atoi(std::getenv("NVR_RT_HOST_SEED")) : 0));
			put4(k.accum, 0.0f, 48.0f, 16.0f, float(128u | 1024u | (noTable ? 0u : 1048576u)));   // PTSpecular + the frame row's energy table, no atlases
			put4(k.sunDir, 0, 0, 1, 0);
			put4(k.skyZenith, L, 1.0f); put4(k.skyHorizon, L, 0.0f);
			put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f);
			k.rect[3] = 1.0f;
			{	// the lamp image: no lamps, the frame row (its energy table) from the game's RtRestirRow::Build
				std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
				RtRestirRow::State state; RtRestirRow::Keys keys;
				const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
				const float eye[3] = { 0, 0, 0 };
				RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), nullptr, 0, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
				if (!h.Upload(gM.ptLamps, image)) return false;
			}
			const std::vector<int> imgs{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
				c.ptDisplay, c.aux16, c.aux16, gM.ptLamps };
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(w, ids, cam.eye)) return false;
			std::vector<float> disp;
			if (!h.Read(c.ptDisplay, disp)) return false;
			h.EndFrame();
			for (UINT i = 0; i < S * S; i++) for (int ch = 0; ch < 3; ch++) { const float v = disp[i * 4 + ch]; if (!std::isfinite(v)) { nan++; continue; } sum[ch] += v; }
			n += S * S;
		}
		float worst = 0;
		for (int ch = 0; ch < 3; ch++) worst = std::max(worst, float(std::fabs(sum[ch] / n / (&L.x)[ch] - 1.0)));
		const float limit = k7 >= 6 ? 0.03f : 0.02f;   // (6/7: a bounce weighs 0 .. 2.1, the product over many bounces is heavy-tailed)
		const bool pass = nan == 0 && worst < limit;
		say("  white metal box, roughness %d/7, 16 frames x 16 paths: mean / L = %.4f %.4f %.4f (worst %.2f %%, limit %.0f %%), NaN %d -> %s\n", k7,
			sum[0] / n / L.x, sum[1] / n / L.y,
			sum[2] / n / L.z, worst * 100, limit * 100, nan, pass ? "ok" : "BAD");
		ok = ok && pass;
		releaseWorld(h, ids);
	}
	// (b) one bounce off a metal floor under the same sky: every pixel = L x the lobe's albedo for its view = L (with the compensation); the
	// mean over pixels whose view cosine is in [0.3, 0.95] within 1.5 %
	{
		Mesh floor; floor.quad({ -20000, -20000, 0 }, { 20000, -20000, 0 }, { 20000, 20000, 0 }, { -20000, 20000, 0 });
		floor.uv.assign(floor.pos.size() * 2, 0.0f);
		World fw; fw.meshes = { floor };
		Object o; o.mesh = 0; o.albedo = { 1, 1, 1 }; o.textured = true; o.alphaSlot = 50; o.rough = 6 / 7.0f; o.metal = 1.0f;
		fw.objects = { o };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, fw, ids)) return false;
		const Camera fc = Camera::lookDir({ 0, -1500, 1500 }, { 0, 1, -1 }, { 0, 0, 1 }, 0.6f, 0.6f);
		std::vector<double> acc(size_t(S) * S, 0.0);
		const int frames = 8;
		for (int f = 0; f < frames; f++) {
			PathConstants k = PathConstants();
			cameraConstants(fc, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float(303 + f * 7919);
			put4(k.accum, 0.0f, 1.0f, 16.0f, float(128u | 1024u | (noTable ? 0u : 1048576u)));
			put4(k.sunDir, 0, 0, 1, 0);
			put4(k.skyZenith, L, 1.0f); put4(k.skyHorizon, L, 0.0f);
			put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f);
			k.rect[3] = 1.0f;
			std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
			RtRestirRow::State state; RtRestirRow::Keys keys;
			const double camWorld[3] = { fc.eye.x, fc.eye.y, fc.eye.z };
			const float eye[3] = { 0, 0, 0 };
			RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), nullptr, 0, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
			if (!h.Upload(gM.ptLamps, image)) return false;
			const std::vector<int> imgs{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
				c.ptDisplay, c.aux16, c.aux16, gM.ptLamps };
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(fw, ids, fc.eye)) return false;
			std::vector<float> disp;
			if (!h.Read(c.ptDisplay, disp)) return false;
			h.EndFrame();
			for (size_t p = 0; p < size_t(S) * S; p++) acc[p] += disp[p * 4] / L.x / frames;
		}
		double sum = 0; int n = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = normalize(fc.ray((i + 0.5f) / S, (j + 0.5f) / S));
			const float nv = -d.z;
			if (nv < 0.3f || nv > 0.95f) continue;
			sum += acc[size_t(j) * S + i]; n++;
		}
		{	// by view cosine (a diagnostic line)
			double bs[10] = {}; int bn[10] = {};
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				const V3 d = normalize(fc.ray((i + 0.5f) / S, (j + 0.5f) / S));
				const int b = std::min(9, std::max(0, int(-d.z * 10.0f)));
				if (-d.z <= 0.0f) continue;
				bs[b] += acc[size_t(j) * S + i]; bn[b]++;
			}
			say("    by view cosine:"); for (int b = 0; b < 10; b++) if (bn[b]) say(" %.1f-%.1f %.4f (%d)", b / 10.0, (b + 1) / 10.0, bs[b] / bn[b], bn[b]); say("%c", 10);
		}
		const double ratio = n ? sum / n : 0;
		const bool pass = n > 1000 && std::fabs(ratio - 1.0) < 0.015;
		say("  one bounce off a white metal floor, roughness 6/7 (%d pixels): mean / L = %.4f -> %s\n", n, ratio, pass ? "ok" : "BAD");
		ok = ok && pass;
		releaseWorld(h, ids);
	}
	return ok;
}
