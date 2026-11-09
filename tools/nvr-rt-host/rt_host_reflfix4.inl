// ==============================================================================================================================
// (2026-11-08 perf-reflfix4) the RT test room on 1.0.6-test12 (docs/next/RT-PATH-TRACING.md "Team perf-reflfix4"):
//   reflsmooth   the gold sphere seen in a mirror (shot B) and in a chrome sphere (shot A): Resolve's average of the fixed 9-point lobe,
//                test 12's 3 x 3 box (3-texel blocks, staircase edges) vs the 5 x 5 tent; images (out\reflsmooth_*.pfm) + a block measure
//   mirrorf0     MetalBounce's F0 (ReflMetalF0.glsl): a mirror-smooth metal with a near-black texture reflects in neutral silver (test 12:
//                in its own near-black colour), rough gold keeps its colour
//   hairtint     (replaces perf-reflfix3's) the hair's tint as the game gives it: the shader property's +0x84 (RtHairTint.h), the noted
//                draw and the player's TESNPC colour (R in the low byte) as fallbacks; the rigid hair's TINTED static record
// Included by rt_host.cpp after rt_host_reflfix3.inl (uvSphere, vndfReflect, gRF, ReflConstants).

static void dumpPfmRgb(const std::string& name, const std::vector<V3>& px, UINT w, UINT h) {
	if (gOut.empty()) return;
	const std::string path = gOut + "\\" + name + ".pfm";
	FILE* f = nullptr; fopen_s(&f, path.c_str(), "wb");
	if (!f) return;
	std::fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
	for (int j = int(h) - 1; j >= 0; j--) for (UINT i = 0; i < w; i++) { const V3& v = px[size_t(j) * w + i]; const float c[3] = { v.x, v.y, v.z }; std::fwrite(c, 4, 3, f); }
	std::fclose(f);
}

// ---- reflsmooth -------------------------------------------------------------------------------------------------------------------
// The helper's pass (ReflectionsRT, the fixed 9-point lobe of BounceDirection) on a first surface that is a mirror floor (layout 0) or a
// chrome sphere (layout 1); the gold (code 4 = 0.33) met by its rays bounces. Resolve's average is emulated here on the helper's images
// with the same tests as ReflectionsRTResolve.frag: test 12's 3 x 3 box (first hits within 10 % + 16) and now the 5 x 5 tent (weights
// (3 - |dx|)(3 - |dy|), first hits within 25 % + 32). Truth: 64 stratified VNDF rays (8 x 8) a pixel about the exact normal on the CPU. Measured on
// the gold's pixels (luminance): shimmer (4 frames), RMS vs the truth, and BLOCKS: the RMS of the 4-neighbour Laplacian (what 3-texel
// steps and staircase edges add) over the truth's.
struct GoldResult { double shimmer = 0, err = 0, hf = 0; double samples = 0; int n = 0; };
static bool goldRun(Ctx& c, int layout, GoldResult& box, GoldResult& tent, double& hfTruth, int& goldPixels) {
	Host& h = *c.host;
	const V3 sky = { 0.6f, 0.7f, 0.8f };
	World w;
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh boxMesh; boxMesh.box({ -160, -160, 0 }, { 160, 160, 420 });
	Camera cam;
	V3 goldCentre;
	float goldRadius;
	int first, goldObj;
	if (layout == 0) {   // reflshimmer's: a mirror floor, the gold sphere on it, red / green / blue boxes
		goldCentre = { 0, 520, 240 }; goldRadius = 220.0f;
		cam = Camera::lookDir({ 0, -1500, 650 }, normalize({ 0, 1, -0.42f }), { 0, 0, 1 }, 1.3f, 1.3f);
		w.meshes = { floor, uvSphere(goldCentre, goldRadius, 48, 24), boxMesh };
		Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
		Object g; g.mesh = 1; g.albedo = { 0.95f, 0.75f, 0.35f }; g.textured = true; g.alphaSlot = 5; g.rough = 0.35f; g.metal = 1.0f;
		Object r; r.mesh = 2; r.offset = { -520, 300, 0 }; r.albedo = { 0.9f, 0.1f, 0.1f };
		Object gr; gr.mesh = 2; gr.offset = { 520, 300, 0 }; gr.albedo = { 0.1f, 0.8f, 0.2f };
		Object bl; bl.mesh = 2; bl.offset = { 0, 1100, 0 }; bl.albedo = { 0.1f, 0.2f, 0.9f };
		w.objects = { f, g, r, gr, bl };
		first = 0; goldObj = 1;
	}
	else {   // a chrome sphere in front of the camera; the gold sphere and a red box beside the camera, seen in the chrome
		const V3 chromeCentre = { 0, 400, 220 };
		goldCentre = { -430, -120, 170 }; goldRadius = 150.0f;
		cam = Camera::lookDir({ 0, -700, 330 }, normalize(chromeCentre - V3(0, -700, 330)), { 0, 0, 1 }, 1.1f, 1.1f);
		w.meshes = { floor, uvSphere(chromeCentre, 200.0f, 48, 24), uvSphere(goldCentre, goldRadius, 48, 24), boxMesh };
		Object f; f.mesh = 0; f.albedo = { 0.45f, 0.45f, 0.45f };
		Object ch; ch.mesh = 1; ch.albedo = { 0.9f, 0.9f, 0.92f };   // (the first surface: no metal rule, its rays are RayGen's)
		Object g; g.mesh = 2; g.albedo = { 0.95f, 0.75f, 0.35f }; g.textured = true; g.alphaSlot = 5; g.rough = 0.35f; g.metal = 1.0f;
		Object r; r.mesh = 3; r.offset = { -760, 120, 0 }; r.albedo = { 0.9f, 0.1f, 0.1f };
		Object bl; bl.mesh = 3; bl.offset = { 300, -900, 0 }; bl.albedo = { 0.1f, 0.2f, 0.9f };
		w.objects = { f, ch, g, r, bl };
		first = 1; goldObj = 2;
	}
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<int> gold(S * S, 0);
	std::vector<V3> vIn(S * S), nTrue(S * S), pHit(S * S);
	auto scan = [&]() {
	std::fill(dir.begin(), dir.end(), 0.0f); std::fill(ray.begin(), ray.end(), 0.0f); std::fill(gold.begin(), gold.end(), 0);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit || hh.object != first) continue;
		const V3 n0 = layout == 0 ? hh.n : normalize(cam.eye + d * hh.t - V3(0, 400, 220));
		const V3 v = normalize(d), rr = v - n0 * (2 * dot(v, n0));
		if (dot(rr, n0) <= 0.0f) continue;
		octEncode(rr, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;
		const V3 surface = d * hh.t, half = rr - normalize(surface);
		const V3 start = surface + half * ((0.5f + hh.t * 0.002f) / length(half));
		const Hit rh = trace(w, cam.eye + start, rr, 1.0f + hh.t * 0.0015f, 10000.0f, 0xEF, false);
		if (!rh.hit || rh.object != goldObj) continue;
		const V3 p = cam.eye + start + rr * rh.t, n = normalize(p - goldCentre);
		if (dot(n, rr) > -0.25f) continue;   // (the outline: left out of the measure)
		gold[j * S + i] = 1; vIn[j * S + i] = rr * -1.0f; nTrue[j * S + i] = n; pHit[j * S + i] = p;
	}
	};
	// the camera zoomed onto the gold's picture (as the tester's close-ups: the gold over many reflection texels, not a handful)
	scan();
	{
		float u0 = 1, u1 = 0, v0 = 1, v1 = 0;
		for (UINT p = 0; p < S * S; p++) if (gold[p]) {
			const float u = (p % S + 0.5f) / S, v = (p / S + 0.5f) / S;
			u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
		}
		if (u1 > u0) {
			const float z = std::min(std::max(0.8f / std::max(u1 - u0, v1 - v0), 1.0f), 16.0f);
			cam = Camera::lookDir(cam.eye, cam.ray(0.5f * (u0 + u1), 0.5f * (v0 + v1)), { 0, 0, 1 }, cam.p00 * z, cam.p11 * z);
			scan();
		}
	}
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) { releaseWorld(h, ids); return false; }
	auto frame = [&](float frameX, std::vector<float>& hit, std::vector<float>& shade) {
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 10000.0f; k.size[3] = 0.0f; k.up[3] = 0.5f;
		put4(k.sun, normalize({ 0.3f, -0.3f, 0.9f }), 1.0f);
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		put4(k.frame, frameX, 0.0f, 0.0f, 0.0f);
		h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		const bool ok = h.Read(c.refHit, hit) && h.Read(c.refShade, shade);
		h.EndFrame();
		return ok;
	};
	auto colourAt = [&](const std::vector<float>& hit, const std::vector<float>& shade, UINT p) {
		const uint32_t fl = uint32_t(hit[p * 4 + 1] + 0.5f);
		if (!(fl & 256u)) return V3(-1, -1, -1);
		return (fl & 512u) ? sky : V3(shade[p * 4], shade[p * 4 + 1], shade[p * 4 + 2]);
	};
	auto hitPoint = [&](const std::vector<float>& hit, UINT k) { const UINT kx = k % S, ky = k / S;
		return cam.ray((kx + 0.5f) / S, (ky + 0.5f) / S) * ray[k * 2] + octDecode(dir[k * 2], dir[k * 2 + 1]) * hit[k * 4]; };
	// Resolve's average (ReflectionsRTResolve.frag), test 12's box or now's tent
	auto averaged = [&](const std::vector<float>& hit, const std::vector<float>& shade, UINT p, bool useTent, double& samples) {
		const uint32_t fl = uint32_t(hit[p * 4 + 1] + 0.5f);
		const V3 own = colourAt(hit, shade, p);
		const uint32_t cls = (fl & 256u) ? ((fl & 1u) | ((fl >> 1u) & 2u)) : 0u;
		if (cls == 0u) { samples = 1; return own; }
		const int R = useTent ? 2 : 1;
		const float c0 = useTent ? 9.0f : 1.0f;
		V3 total = own * c0;
		float count = c0;
		int taken = 1;
		const int x = int(p % S), y = int(p / S);
		const V3 hp = hitPoint(hit, p);
		for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
			if (!dx && !dy) continue;
			const int nx = x + dx, ny = y + dy;
			if (nx < 0 || ny < 0 || nx >= int(S) || ny >= int(S)) continue;
			const UINT q = UINT(ny) * S + UINT(nx);
			if (ray[q * 2] <= 0 || ray[q * 2 + 1] < 1.5f || ray[q * 2 + 1] > 3.5f) continue;
			if (std::fabs(ray[q * 2] - ray[p * 2]) > ray[p * 2] * 0.05f + 4.0f) continue;
			const uint32_t nf = uint32_t(hit[q * 4 + 1] + 0.5f);
			if (hit[q * 4] <= 0 || !(nf & 256u) || ((nf & 1u) | ((nf >> 1u) & 2u)) != cls) continue;
			const float tol = useTent ? hit[p * 4] * 0.25f + 32.0f : hit[p * 4] * 0.1f + 16.0f;
			if (length(hitPoint(hit, q) - hp) > tol) continue;
			const float wgt = useTent ? float((3 - std::abs(dx)) * (3 - std::abs(dy))) : 1.0f;
			total = total + colourAt(hit, shade, q) * wgt; count += wgt; taken++;
		}
		samples = taken;
		return total * (1.0f / count);
	};
	auto lum = [](V3 v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
	// the truth: 64 stratified VNDF rays (8 x 8, a per-pixel shift of the grid)
	std::vector<V3> truth(S * S, V3(0, 0, 0));
	const float a = NvrRtRoughOf(NvrRtRoughCode(0.35f)) * NvrRtRoughOf(NvrRtRoughCode(0.35f));
	for (UINT p = 0; p < S * S; p++) {
		if (!gold[p]) continue;
		V3 sum = { 0, 0, 0 };
		const V3 off = pHit[p] + nTrue[p] * 1.0f;
		const float s1 = float((p * 2654435761u) >> 8) / 16777216.0f, s2 = float((p * 40503u + 12345u) & 0xFFFF) / 65536.0f;
		for (int sy = 0; sy < 8; sy++) for (int sx = 0; sx < 8; sx++) {
			const float u1 = std::fmod((sx + 0.5f) / 8.0f + s1, 1.0f), u2 = std::fmod((sy + 0.5f) / 8.0f + s2, 1.0f);
			const V3 d2 = vndfReflect(vIn[p], nTrue[p], a, u1, u2);
			const Hit b = trace(w, off, d2, 0.5f, 10000.0f, 0xEF, false);
			sum = sum + (b.hit ? w.objects[b.object].albedo : sky);
		}
		truth[p] = sum * (1.0f / 64);
	}
	const int F = 4;
	std::vector<std::vector<V3>> fb(F, std::vector<V3>(S * S, V3(0, 0, 0))), ft(F, std::vector<V3>(S * S, V3(0, 0, 0)));
	double sb = 0, st = 0; int sn = 0;
	for (int fr = 0; fr < F; fr++) {
		std::vector<float> hit, shade;
		if (!frame(float(1 + fr * 37), hit, shade)) { releaseWorld(h, ids); return false; }
		for (UINT p = 0; p < S * S; p++) {
			if (!gold[p]) continue;
			double nb, nt;
			fb[fr][p] = averaged(hit, shade, p, false, nb);
			ft[fr][p] = averaged(hit, shade, p, true, nt);
			if (fr == 0 && fb[fr][p].x >= 0) { sb += nb; st += nt; sn++; }
		}
	}
	releaseWorld(h, ids);
	auto stats = [&](const std::vector<std::vector<V3>>& fr, GoldResult& out) {
		double sh = 0, e = 0, l2 = 0; int n = 0, nl = 0;
		for (UINT p = 0; p < S * S; p++) {
			if (!gold[p] || fr[0][p].x < 0) continue;
			double m = 0, m2 = 0;
			for (int k = 0; k < F; k++) { const double l = lum(fr[k][p]); m += l / F; m2 += l * l / F; }
			sh += std::sqrt(std::max(m2 - m * m, 0.0));
			e += std::pow(lum(fr[0][p]) - lum(truth[p]), 2.0);
			n++;
			const int x = int(p % S), y = int(p / S);
			if (x < 1 || y < 1 || x >= int(S) - 1 || y >= int(S) - 1) continue;
			const UINT q[4] = { p - 1, p + 1, p - S, p + S };
			bool all = true;
			for (UINT k : q) all = all && gold[k] && fr[0][k].x >= 0;
			if (!all) continue;
			const double L = 4 * lum(fr[0][p]) - lum(fr[0][q[0]]) - lum(fr[0][q[1]]) - lum(fr[0][q[2]]) - lum(fr[0][q[3]]);
			l2 += L * L; nl++;
		}
		out.n = n; out.shimmer = n ? sh / n : 0; out.err = n ? std::sqrt(e / n) : 0; out.hf = nl ? std::sqrt(l2 / nl) : 0;
	};
	stats(fb, box); stats(ft, tent);
	box.samples = sn ? sb / sn : 0; tent.samples = sn ? st / sn : 0;
	{	// the truth's own Laplacian
		double l2 = 0; int nl = 0;
		for (UINT p = 0; p < S * S; p++) {
			const int x = int(p % S), y = int(p / S);
			if (!gold[p] || x < 1 || y < 1 || x >= int(S) - 1 || y >= int(S) - 1) continue;
			if (!gold[p - 1] || !gold[p + 1] || !gold[p - S] || !gold[p + S]) continue;
			const double L = 4 * lum(truth[p]) - lum(truth[p - 1]) - lum(truth[p + 1]) - lum(truth[p - S]) - lum(truth[p + S]);
			l2 += L * L; nl++;
		}
		hfTruth = nl ? std::sqrt(l2 / nl) : 0;
	}
	goldPixels = box.n;
	// the pictures: the gold's pixels, the rest dark grey
	std::vector<V3> ib(S * S, V3(0.05f, 0.05f, 0.05f)), it(S * S, V3(0.05f, 0.05f, 0.05f)), itr(S * S, V3(0.05f, 0.05f, 0.05f));
	for (UINT p = 0; p < S * S; p++) if (gold[p] && fb[0][p].x >= 0) { ib[p] = fb[0][p]; it[p] = ft[0][p]; itr[p] = truth[p]; }
	const std::string tag = layout == 0 ? "mirror" : "chrome";
	dumpPfmRgb("reflsmooth_" + tag + "_1_test12box", ib, S, S);
	dumpPfmRgb("reflsmooth_" + tag + "_2_tent", it, S, S);
	dumpPfmRgb("reflsmooth_" + tag + "_3_truth", itr, S, S);
	return true;
}

static bool sceneReflSmooth(Ctx& c) {
	bool allOk = true;
	for (int layout = 0; layout < 2; layout++) {
		GoldResult box, tent;
		double hfTruth = 0;
		int n = 0;
		if (!goldRun(c, layout, box, tent, hfTruth, n)) return false;
		// smooth: the tent's Laplacian at most 0.6 x the box's (the blocks gone) and no worse vs the truth; steady: no shimmer at all
		const bool ok = n > 150 && tent.shimmer < 1e-6 && box.shimmer < 1e-6 && tent.hf < 0.6 * box.hf && tent.err <= box.err * 1.05;
		say("  %s: the gold (code 4 = 0.33) seen %s, %d pixels; Laplacian RMS (blocks, staircase edges; the truth %.4f): test 12's 3 x 3 box %.4f, "
			"the 5 x 5 tent %.4f; RMS vs the truth: box %.4f, tent %.4f; shimmer (4 frames): box %.6f, tent %.6f; samples a pixel: box %.1f, tent %.1f -> %s\n",
			layout == 0 ? "B" : "A", layout == 0 ? "in a mirror floor (shot B)" : "in a chrome sphere (shot A)", n, hfTruth, box.hf, tent.hf, box.err, tent.err,
			box.shimmer, tent.shimmer, box.samples, tent.samples, ok ? "ok" : "BAD");
		allOk = allOk && ok;
	}
	say("  (pictures: out\\reflsmooth_<mirror|chrome>_<1_test12box|2_tent|3_truth>.pfm; py -3 -I tools\\nvr-rt-host\\refl_sheet.py <out> reflsmooth 4)\n");
	return allOk;
}

// ---- mirrorf0 ---------------------------------------------------------------------------------------------------------------------
// MetalBounce's Fresnel tint (the bounce image's rgb, ReflectionsRTBody.glsl) on two spheres in a mirror floor: a mirror-smooth metal with a
// near-black texture (code 1 = 0.02, metal 1, albedo 0.03: the test room's vs_mirror01 under a chrome-like rule) and the gold (0.33). Want:
// the smooth one's F0 the neutral silver 0.95 (test 12: its texture's 0.03, a mirror in a mirror black), the gold's its own colour.
static bool sceneMirrorF0(Ctx& c) {
	Host& h = *c.host;
	const Camera cam = Camera::lookDir({ 0, -1500, 650 }, normalize({ 0, 1, -0.42f }), { 0, 0, 1 }, 1.3f, 1.3f);
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	const V3 cA = { -300, 520, 240 }, cB = { 300, 520, 240 };
	World w; w.meshes = { floor, uvSphere(cA, 200.0f, 48, 24), uvSphere(cB, 200.0f, 48, 24) };
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
	Object m; m.mesh = 1; m.albedo = { 0.03f, 0.03f, 0.03f }; m.textured = true; m.alphaSlot = 5; m.rough = 0.02f; m.metal = 1.0f;
	Object g; g.mesh = 2; g.albedo = { 0.95f, 0.75f, 0.35f }; g.textured = true; g.alphaSlot = 6; g.rough = 0.35f; g.metal = 1.0f;
	w.objects = { f, m, g };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<int> which(S * S, -1);
	std::vector<float> facing(S * S, 0.0f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit || hh.object != 0) continue;
		const V3 v = normalize(d), rr = v - hh.n * (2 * dot(v, hh.n));
		octEncode(rr, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;
		const V3 surface = d * hh.t, half = rr - normalize(surface);
		const V3 start = surface + half * ((0.5f + hh.t * 0.002f) / length(half));
		const Hit rh = trace(w, cam.eye + start, rr, 1.0f + hh.t * 0.0015f, 10000.0f, 0xEF, false);
		if (!rh.hit || rh.object == 0) continue;
		const V3 p = cam.eye + start + rr * rh.t, n = normalize(p - (rh.object == 1 ? cA : cB));
		if (dot(n, rr) > -0.4f) continue;
		which[j * S + i] = rh.object; facing[j * S + i] = -dot(n, rr);
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) { releaseWorld(h, ids); return false; }
	ReflConstants k = {};
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.size[2] = 10000.0f; k.size[3] = 0.0f; k.up[3] = 0.5f;
	put4(k.sun, normalize({ 0.3f, -0.3f, 0.9f }), 1.0f);
	put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
	h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	std::vector<float> hit, bounce;
	const bool read = h.Exchange(w, ids, cam.eye) && h.Read(c.refHit, hit) && h.Read(gRF.bounce, bounce);
	h.EndFrame();
	releaseWorld(h, ids);
	if (!read) return false;
	// the CPU's Schlick of the rule: ReflMetalF0 = mix(0.95, albedo, smoothstep(0.06, 0.15, roughness))
	auto f0Of = [](float albedo, float rough) { float t = std::min(std::max((rough - 0.06f) / 0.09f, 0.0f), 1.0f); t = t * t * (3 - 2 * t); return 0.95f + (albedo - 0.95f) * t; };
	const float rm = NvrRtRoughOf(NvrRtRoughCode(0.02f)), rg = NvrRtRoughOf(NvrRtRoughCode(0.35f));
	int nA = 0, nB = 0, bad = 0;
	double worst = 0, meanA = 0, meanOldA = 0, meanB = 0;
	for (UINT p = 0; p < S * S; p++) {
		if (which[p] < 0) continue;
		const uint32_t fl = uint32_t(hit[p * 4 + 1] + 0.5f);
		if (!(fl & 256u)) continue;
		const float c5 = std::pow(1.0f - facing[p], 5.0f);
		const bool isA = which[p] == 1;
		const V3 alb = isA ? m.albedo : g.albedo;
		const V3 want = { f0Of(alb.x, isA ? rm : rg), f0Of(alb.y, isA ? rm : rg), f0Of(alb.z, isA ? rm : rg) };
		const V3 tint = want + (V3{ 1, 1, 1 } - want) * c5;
		const double e = std::max(std::fabs(bounce[p * 4] - tint.x), std::max(std::fabs(bounce[p * 4 + 1] - tint.y), std::fabs(bounce[p * 4 + 2] - tint.z)));
		worst = std::max(worst, e);
		if (e > 0.04) bad++;
		if (isA) { nA++; meanA += bounce[p * 4 + 1]; meanOldA += alb.y + (1 - alb.y) * c5; }
		else { nB++; meanB += bounce[p * 4 + 2] / std::max(bounce[p * 4], 1e-4f); }
	}
	const bool ok = nA > 100 && nB > 100 && bad == 0 && meanA / nA > 0.9;
	say("  the mirror-smooth metal with a near-black texture (0.02): %d bounce pixels, its tint (green) %.3f (test 12's rule: %.3f, a black mirror in "
		"the mirror); the gold (0.33): %d pixels, blue / red %.3f (its own colour: %.3f); worst vs ReflMetalF0 + Schlick %.4f -> %s\n",
		nA, nA ? meanA / nA : 0.0, nA ? meanOldA / nA : 0.0, nB, nB ? meanB / nB : 0.0, g.albedo.z / g.albedo.x, worst, ok ? "ok" : "BAD");
	return ok;
}

// ---- hairtint (CPU) ---------------------------------------------------------------------------------------------------------------
static bool sceneHairTint4(Ctx&) {
	int bad = 0;
	auto near3 = [](const float* a, float r, float g, float b) { return std::fabs(a[0] - r) < 1e-4f && std::fabs(a[1] - g) < 1e-4f && std::fabs(a[2] - b) < 1e-4f; };
	RtHairTint::Clear();
	const int geoA = 1, geoB = 2, geoC = 3;
	float rgb[3];
	// 1. the property's colour as ApplyFaceGenTextures writes it (alpha 1): red hair (170, 40, 20)
	const float written[4] = { 170 / 255.0f, 40 / 255.0f, 20 / 255.0f, 1.0f };
	if (RtHairTint::For(&geoA, written, true, false, 0, rgb) != RtHairTint::Property || !near3(rgb, 170 / 255.0f, 40 / 255.0f, 20 / 255.0f)) { bad++; say("    the property's colour not taken\n"); }
	// ... not for a part that is not hair, nor a value the game never wrote (the constructor's zeroes; alpha not 1; out of range)
	if (RtHairTint::For(&geoA, written, false, false, 0, rgb) != RtHairTint::None) { bad++; say("    a non-hair part took a tint\n"); }
	const float zeroes[4] = { 0, 0, 0, 0 }, odd[4] = { 2.0f, 0.1f, 0.1f, 1.0f };
	if (RtHairTint::For(&geoB, zeroes, true, false, 0, rgb) != RtHairTint::None || RtHairTint::For(&geoB, odd, true, false, 0, rgb) != RtHairTint::None) { bad++; say("    an unwritten property gave a tint\n"); }
	// 2. no written colour: a noted draw's c2 (at any age), else the player's TESNPC colour
	RtHairTint::Frame = 100; RtHairTint::Wanted = true;
	{ RtHairTint::Tint t; t.rgb[0] = 0.5f; t.rgb[1] = 0.25f; t.rgb[2] = 0.125f; t.frame = 100; RtHairTint::Tints[&geoB] = t; }
	RtHairTint::Frame = 5100;
	if (RtHairTint::For(&geoB, zeroes, true, false, 0, rgb) != RtHairTint::Noted || !near3(rgb, 0.5f, 0.25f, 0.125f)) { bad++; say("    the noted tint not taken\n"); }
	// the ESM's HCLR bytes (R, G, B, 0) read into the word as the game reads them (little-endian): Sunny Smiles (66, 28, 15)
	const unsigned char hclr[4] = { 66, 28, 15, 0 };
	uint32_t packed; std::memcpy(&packed, hclr, 4);
	if (RtHairTint::For(&geoC, nullptr, true, true, packed, rgb) != RtHairTint::PlayerColour || !near3(rgb, 66 / 255.0f, 28 / 255.0f, 15 / 255.0f)) { bad++; say("    the player's colour %.3f %.3f %.3f\n", rgb[0], rgb[1], rgb[2]); }
	if (RtHairTint::For(&geoC, nullptr, true, false, packed, rgb) != RtHairTint::None) { bad++; say("    an NPC's part took the player's colour\n"); }
	// TESNPC's default 0x19324B is brown with R in the low byte (75, 50, 25); perf-reflfix3's order read it blue (25, 50, 75)
	RtHairTint::Unpack(0x19324Bu, rgb);
	if (!near3(rgb, 75 / 255.0f, 50 / 255.0f, 25 / 255.0f)) { bad++; say("    TESNPC's default colour %.3f %.3f %.3f\n", rgb[0], rgb[1], rgb[2]); }
	// 3. names ApplyFaceGenTextures writes the colour for; the shader types it writes it for
	if (!RtHairTint::HairName("FaceGenHairNoHat") || !RtHairTint::HairName("FaceGenHairHat") || !RtHairTint::HairName("FaceGenAccessory") || RtHairTint::HairName("Hat") ||
		RtHairTint::HairName("BeardFull:0") || RtHairTint::HairName(nullptr)) { bad++; say("    HairName wrong\n"); }
	if (!RtHairTint::TintClass(8) || !RtHairTint::TintClass(12) || RtHairTint::TintClass(9) || RtHairTint::TintClass(1)) { bad++; say("    TintClass wrong\n"); }
	RtHairTint::Clear(); RtHairTint::Wanted = false;
	// 4. the rigid hair's static record: TINTED (both bits) as the actors' hair, nothing else changed; untinted as before
	{
		using namespace RtScenePick;
		Record r; r.alphaTested = true; r.alphaRef = 128; r.slot = 77; r.albedoKnown = true; r.tinted = true;
		const uint32_t f = RecordFlags(r);
		ActorPart a; a.albedoKnown = true; a.textured = true; a.slot = 77; a.alphaTested = true; a.alphaRef = 128; a.tinted = true;
		if (!IsTinted(f) || f != ActorRecordFlags(a)) { bad++; say("    the rigid hair's record %08X vs the actor's %08X\n", f, ActorRecordFlags(a)); }
		r.tinted = false;
		if (IsTinted(RecordFlags(r))) { bad++; say("    an untinted cut-out came out tinted\n"); }
		r.alphaTested = false; r.textured = true; r.tinted = true;
		if (IsTinted(RecordFlags(r))) { bad++; say("    an opaque textured record came out tinted\n"); }
	}
	const float red[3] = { RtScenePick::TintLinear(170 / 255.0f), RtScenePick::TintLinear(40 / 255.0f), RtScenePick::TintLinear(20 / 255.0f) };
	say("  the hair's tint from its shader property (red hair 170 40 20 -> the record's %.4f %.4f %.4f linear; x 2^2.2 on the grey texture in "
		"RtHit.glsl), not for non-hair parts nor unwritten properties; a noted draw, then the player's TESNPC colour (R in the low byte: the ESM's "
		"HCLR bytes; TESNPC's default 0x19324B brown) as fallbacks; the rigid hair's static record TINTED like the actors'; %d wrong -> %s\n",
		red[0], red[1], red[2], bad, bad ? "BAD" : "ok");
	return bad == 0;
}

// ---- hairglow ---------------------------------------------------------------------------------------------------------------------
// (the lead's in-game run of bb2dc764: the log showed the hair's record tinted, the reflected hair still white) The RIGID hair as RtCore::
// AddGeometry packs it (a static alpha-tested card, TINTED: RtScenePick::RecordFlags with tinted), seen through mirror pixels (one exact ray
// each) on the variant the game ran, ReflectionsRTAlphaTexGlow. Texture slot 2 of a hair property is its LAYER map (<name>_hl.dds), which
// HasGlowMap took for a glow map: the record carried GLOW (0x04), and the glow variant's ShadeHit REPLACES the hit's colour with the glow
// map's (bright: the layer map) and keeps the glow flag (Resolve adds it as light). Here the slot's colour = the grey hair texture (the
// albedo atlas, bound as the glow atlas too). Before (glow on: test 12 and bb2dc764): the hit's colour the untinted grey, glow flag set;
// now (glow off: SlotTwoIsLayer): the grey x the tint x 2^2.2, no glow flag.
static int gShReflAlphaTexGlow = -1;
static bool registerReflFix4(Host& h, const std::string& spv) {
	gShReflAlphaTexGlow = h.AddShader(spv + "\\ReflectionsRTAlphaTexGlow.spv");
	return gShReflAlphaTexGlow >= 0;
}
static bool sceneHairGlow(Ctx& c) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(3);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });   // the mirror floor
	w.meshes[1].box({ -900, 900, 0 }, { 900, 1300, 1400 });                                              // a wall block behind
	w.meshes[2].quad({ -600, 450, 50 }, { 600, 450, 50 }, { 600, 450, 1300 }, { -600, 450, 1300 });       // the hair card
	const int slot = 87;
	w.slots[slot] = [](int x, int) { return ((x / 16) & 1) ? 1.0f : 0.0f; };   // strands
	const float grey = 160.0f / 255.0f, greyLinear = std::pow((grey + 0.055f) / 1.055f, 2.4f);
	const V3 tintStored = { 0.133f, 0.137f, 0.176f };   // the lead's log: the player's hair at +0x84 (near-black)
	const V3 tintLinear = { RtScenePick::TintLinear(tintStored.x), RtScenePick::TintLinear(tintStored.y), RtScenePick::TintLinear(tintStored.z) };
	if (!uploadSlot(h, c.alphaAtlas, slot, w.slots[slot])) return false;
	{
		std::vector<float> t(64 * 64 * 4);
		for (size_t i = 0; i < t.size(); i++) t[i] = (i % 4) == 3 ? 1.0f : grey;
		if (!h.Upload(gM.albedoAtlas, t, (slot % 32) * 64, (slot / 32) * 64, 64, 64)) return false;
	}
	const Camera cam = Camera::lookDir({ 0, -1600, 700 }, normalize({ 0, 1, -0.45f }), { 0, 0, 1 }, 1.3f, 1.3f);
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		if (!(d.z < 0)) continue;
		const float t = -cam.eye.z / d.z;   // the floor (z = 0)
		const V3 v = normalize(d), r = { v.x, v.y, -v.z };
		octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = t; ray[(j * S + i) * 2 + 1] = 2.0f;   // (view z: d's forward component is 1)
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
	bool allOk = true;
	for (int run = 0; run < 2; run++) {
		const bool glowOn = run == 0;   // 0: as test 12 / bb2dc764 sent it; 1: now
		Object floor; floor.mesh = 0; floor.albedo = { 0.5f, 0.5f, 0.5f };
		Object wall; wall.mesh = 1; wall.albedo = { 0.3f, 0.6f, 0.3f };
		Object card; card.mesh = 2; card.alphaTested = true; card.alphaSlot = slot; card.alphaRef = 128; card.tinted = true; card.glow = glowOn;
		card.albedo = tintLinear;
		w.objects = { floor, wall, card };
		const uint32_t f = card.recordFlags();
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 10000.0f; k.size[3] = 0.0f;
		put4(k.sun, normalize({ 0.1f, -0.8f, 0.6f }), 1.0f);
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		h.AddPass(gShReflAlphaTexGlow, { c.refDir, c.refRay, c.refHit, c.refShade, c.alphaAtlas, gM.albedoAtlas, gM.albedoAtlas, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		std::vector<float> hit, shade;
		const bool read = h.Exchange(w, ids, cam.eye) && h.Read(c.refHit, hit) && h.Read(c.refShade, shade);
		h.EndFrame();
		releaseWorld(h, ids);
		if (!read) return false;
		// the card's hits: the mirror ray met the card (CPU) on a strand
		int n = 0, glowFlag = 0; double sum[3] = { 0, 0, 0 };
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const UINT p = j * S + i;
			if (ray[p * 2] <= 0) continue;
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const V3 v = normalize(d), r = { v.x, v.y, -v.z };
			const V3 at = cam.eye + d * ray[p * 2];
			const Hit rh = trace(w, at + V3(0, 0, 1), r, 1.0f, 10000.0f, 0xFF, true);
			if (!rh.hit || rh.object != 2 || std::fabs(hit[p * 4] - rh.t) > 4.0f + 0.01f * rh.t) continue;
			n++;
			if (int(hit[p * 4 + 1] + 0.5f) & 8) glowFlag++;
			for (int ch = 0; ch < 3; ch++) sum[ch] += shade[p * 4 + ch];
		}
		const V3 want = glowOn ? V3(greyLinear, greyLinear, greyLinear) : V3(std::min(1.0f, greyLinear * tintLinear.x * RtScenePick::kTintScale),
			std::min(1.0f, greyLinear * tintLinear.y * RtScenePick::kTintScale), std::min(1.0f, greyLinear * tintLinear.z * RtScenePick::kTintScale));
		const V3 got = n ? V3(float(sum[0] / n), float(sum[1] / n), float(sum[2] / n)) : V3(0, 0, 0);
		const bool ok = n > 200 && std::fabs(got.x - want.x) < 0.01f + 0.02f * want.x && std::fabs(got.z - want.z) < 0.01f + 0.02f * want.z &&
			(glowOn ? glowFlag == n : glowFlag == 0) && RtScenePick::IsTinted(f);
		say("  %s: record %08X (tinted %d, glow %d); %d mirror rays met the hair card: its colour %.4f %.4f %.4f (expected %.4f %.4f %.4f: %s), glow flag on %d -> %s\n",
			glowOn ? "as test 12 / bb2dc764 sent it (slot 2 taken as a glow map)" : "now (slot 2 the hair layer map)", f, RtScenePick::IsTinted(f) ? 1 : 0,
			(f & NVR_RT_RECORD_GLOW) ? 1 : 0, n, got.x, got.y, got.z, want.x, want.y, want.z, glowOn ? "the untinted map: white-grey hair, glowing" : "the texture x the near-black tint",
			glowFlag, ok ? "reproduced" : "BAD");
		if (!glowOn) say("    -> %s\n", ok ? "ok: the tint reaches the reflection" : "BAD");
		allOk = allOk && ok;
	}
	return allOk;
}
