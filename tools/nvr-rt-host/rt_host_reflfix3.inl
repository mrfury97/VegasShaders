// ==============================================================================================================================
// (2026-11-08 perf-reflfix3) the RT test room on 1.0.6-test11 (docs/next/RT-PATH-TRACING.md "Team perf-reflfix3"):
//   reflnear     a lamp 4 units under a ceiling (the estimate vs the game's per-pixel lamp light: bounded, no blow-up); the flashlight at
//                hits round its own origin (the orb round the reflected player's head): nothing within 24 units, full from 48
//   reflshimmer  the gold sphere in the mirror: test 11's per-frame bounce sample vs the fixed 9-point lobe + Resolve's 3 x 3 mean
//                (frame-to-frame shimmer of a still view, error vs the 64-ray truth)
//   (hairtint    moved to rt_host_reflfix4.inl, 2026-11-08 perf-reflfix4: the tint now from the hair's shader property)
// Included by rt_host.cpp after rt_host_reflfix2.inl (gR2.lampCheck, LampCheckConstants, uvSphere, angleDeg, vndfReflect, gRF).

// ---- reflnear -------------------------------------------------------------------------------------------------------------------
static bool sceneReflNear(Ctx& c) {
	Host& h = *c.host;
	bool allOk = true;
	// (1) the room's ceiling lamp 4 units under a ceiling (BasementLightKeyNeutral: 650, 255 226 190, fade 1.9; indoors)
	{
		const float ceilingZ = 194.0f;
		RtReflLamps::Raw raw;
		raw.pos[0] = 0; raw.pos[1] = 0; raw.pos[2] = 190; raw.reach = 650;
		raw.diffuse[0] = 1.0f; raw.diffuse[1] = 226 / 255.0f; raw.diffuse[2] = 190 / 255.0f; raw.dimmer = 1.9f;
		const float eye[3] = { 0, 0, 0 };
		float lp[8];
		RtReflLamps::Pack(raw, RtReflLamps::Frame(), eye, lp);
		const float span = 64.0f;   // the 128 x 128 grid: 1 unit a texel round the lamp
		LampCheckConstants k = {};
		put4(k.size, float(S), float(S), 1.0f, 0.0f);
		put4(k.origin, V3(-span, -span, ceilingZ), 0.0f);
		put4(k.axisU, V3(2 * span, 0, 0), 0.0f); put4(k.axisV, V3(0, 2 * span, 0), 0.0f); put4(k.normal, V3(0, 0, -1), 0.0f);
		std::memcpy(k.lamps[0], lp, 16); std::memcpy(k.lamps[1], lp + 4, 16);
		h.AddPass(gR2.lampCheck, { c.aux16 }, 0x1u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(World(), {}, V3())) return false;
		std::vector<float> got;
		if (!h.Read(c.aux16, got)) return false;
		h.EndFrame();
		double worst = 0; float peak = 0, peakGame = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 p = V3(-span + 2 * span * (i + 0.5f) / S, -span + 2 * span * (j + 0.5f) / S, ceilingZ);
			const V3 toL = V3(0, 0, 190) - p;
			const float d = length(toL), q = d * d / (650.0f * 650.0f);
			const float game = std::pow(raw.diffuse[0] * 1.0f, 2.2f) * std::pow(1 - q, 2.2f) * std::max(-toL.z / d, 0.0f);   // red, dimmer clamped to 1
			const float g = got[(j * S + i) * 4];
			if (game > 1e-3f) worst = std::max(worst, std::fabs(double(g) / game - 1.0));
			peak = std::max(peak, g); peakGame = std::max(peakGame, game);
		}
		const bool ok = worst < 0.005 && peak <= lp[4] * 1.002f;
		say("  1 a lamp 4 units under the ceiling: the estimate vs the game's per-pixel lamp light worst %.3f %%; its peak %.3f = the game's %.3f, "
			"never above the lamp's colour %.3f (the light on the surface it nearly touches is bounded: a 4-unit disc, not a white orb) -> %s\n",
			worst * 100, peak, peakGame, lp[4], ok ? "ok" : "BAD");
		allOk = allOk && ok;
	}
	// (2) the flashlight at hits round its own origin: a sphere of points 0..120 units from it, all inside its cone, facing it
	{
		const V3 fpos = { 0, 0, 0 };
		const V3 axis = { 0, 1, 0 };
		const float rel[3] = { 0, 0, 0 }, col[3] = { 2.267f, 4.0f, 4.0f };
		float fp[8];
		RtReflLamps::PackFlashlight(rel, 1000.0f, col, 0.0f, 2.2f, fp);
		const float code = RtFlashlightCode::Encode(axis.x, axis.y, axis.z, std::cos(30.0f * 0.0174532925f));
		// the grid: a plane facing the light (normal -y) at y = t, t from 1 to 128 units along the rows; x across, inside the cone's core
		std::vector<float> got;
		double badNear = 0, worstFar = 0;
		for (int row = 0; row < 2; row++) {
			LampCheckConstants k = {};
			put4(k.size, float(S), float(S), 1.0f, 1.0f);
			put4(k.origin, V3(-2.0f, 1.0f + 64.0f * row, 0.0f), code);
			put4(k.axisU, V3(4.0f, 0, 0), 0.0f); put4(k.axisV, V3(0, 64.0f, 0), 0.0f); put4(k.normal, V3(0, -1, 0), 0.0f);
			std::memcpy(k.lamps[0], fp, 16); std::memcpy(k.lamps[1], fp + 4, 16);
			h.AddPass(gR2.lampCheck, { c.aux16 }, 0x1u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(World(), {}, V3())) return false;
			if (!h.Read(c.aux16, got)) return false;
			h.EndFrame();
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				const V3 p = V3(-2.0f + 4.0f * (i + 0.5f) / S, 1.0f + 64.0f * row + 64.0f * (j + 0.5f) / S, 0.0f);
				const float d = length(p - fpos), q = d * d / 1e6f;
				const float x = std::min(std::max((d - 24.0f) / 24.0f, 0.0f), 1.0f), nearW = x * x * (3 - 2 * x);
				const float want = col[1] * (1 - q) * std::max(p.y / d, 0.0f) * nearW;   // (green; cone 1: within 2 units of the axis)
				const float g = got[(j * S + i) * 4 + 1];
				if (d < 24.0f) badNear = std::max(badNear, double(g));
				else if (want > 1e-3f) worstFar = std::max(worstFar, std::fabs(double(g) / want - 1.0));
			}
		}
		const bool ok = badNear < 1e-4 && worstFar < 0.005;
		say("  2 the flashlight's own origin: hits within 24 units take %.5f of its light (test 11: up to 4.0, the white orb round the reflected "
			"head), from 24 to 48 a smooth fade-in, past 48 FlashlightDeferred.hlsl's light: worst %.3f %% -> %s\n", badNear, worstFar * 100, ok ? "ok" : "BAD");
		allOk = allOk && ok;
	}
	return allOk;
}

// ---- reflshimmer ----------------------------------------------------------------------------------------------------------------
// The gold sphere (code 4 = 0.33) in a mirror floor beside red / green / blue boxes and a sky colour: the reflection's colour at each
// mirror pixel that meets the gold = what its bounce meets (the hit's albedo; the sky's colour on a miss) x the tint (left out: the same
// in both). Resolve is a D3D9-side pass the host does not run: its RtBounceAverage is applied here on the helper's images, with the same
// rule (the 3 x 3 neighbours that bounce with the same class and meet the gold at about the same distance). Before (frame.y 2: test 11's
// per-frame random sample, no average: a mirror pixel keeps no history); after (the fixed 9-point lobe + the average). 16 frames of a
// still view each: shimmer = the mean over the gold's pixels of the frames' standard deviation (luminance); error = RMS of the frames'
// mean (and of one frame) vs the truth: 256 VNDF rays a pixel about the exact normal on the CPU.
static bool sceneReflShimmer(Ctx& c) {
	Host& h = *c.host;
	const V3 centre = { 0, 520, 240 };
	const float radius = 220.0f;
	const Camera cam = Camera::lookDir({ 0, -1500, 650 }, normalize({ 0, 1, -0.42f }), { 0, 0, 1 }, 1.3f, 1.3f);
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh box; box.box({ -160, -160, 0 }, { 160, 160, 420 });
	World w; w.meshes = { floor, uvSphere(centre, radius, 48, 24), box };
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
	Object g; g.mesh = 1; g.albedo = { 0.95f, 0.75f, 0.35f }; g.textured = true; g.alphaSlot = 5; g.rough = 0.35f; g.metal = 1.0f;
	Object r; r.mesh = 2; r.offset = { -520, 300, 0 }; r.albedo = { 0.9f, 0.1f, 0.1f };
	Object gr; gr.mesh = 2; gr.offset = { 520, 300, 0 }; gr.albedo = { 0.1f, 0.8f, 0.2f };
	Object bl; bl.mesh = 2; bl.offset = { 0, 1100, 0 }; bl.albedo = { 0.1f, 0.2f, 0.9f };
	w.objects = { f, g, r, gr, bl };
	const V3 sky = { 0.6f, 0.7f, 0.8f };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<int> gold(S * S, 0);
	std::vector<V3> vIn(S * S), nTrue(S * S), pHit(S * S);
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
		if (!rh.hit || rh.object != 1) continue;
		const V3 p = cam.eye + start + rr * rh.t, n = normalize(p - centre);
		if (dot(n, rr) > -0.25f) continue;   // (the outline: left out of the measure)
		gold[j * S + i] = 1; vIn[j * S + i] = rr * -1.0f; nTrue[j * S + i] = n; pHit[j * S + i] = p;
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) { releaseWorld(h, ids); return false; }
	auto frame = [&](float frameX, float mode, std::vector<float>& hit, std::vector<float>& shade) {
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 10000.0f; k.size[3] = 0.0f; k.up[3] = 0.5f;
		put4(k.sun, normalize({ 0.3f, -0.3f, 0.9f }), 1.0f);
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		put4(k.frame, frameX, mode, 0.0f, 0.0f);
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
	// Resolve's RtBounceAverage on the helper's images (ReflectionsRTResolve.frag, the same tests)
	double avgCount = 0; int avgCalls = 0, reject[4] = {};
	auto averaged = [&](const std::vector<float>& hit, const std::vector<float>& shade, UINT p) {
		const uint32_t fl = uint32_t(hit[p * 4 + 1] + 0.5f);
		V3 total = colourAt(hit, shade, p);
		const uint32_t cls = (fl & 256u) ? ((fl & 1u) | ((fl >> 1u) & 2u)) : 0u;
		if (cls == 0u) return total;
		// (2026-11-08 perf-reflfix4) Resolve's 5 x 5 tent now (weights (3 - |dx|)(3 - |dy|); first hits within 25 % + 32): scene reflsmooth
		total = total * 9.0f;
		float count = 9;
		int taken = 1;
		const int x = int(p % S), y = int(p / S);
		for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
			if (!dx && !dy) continue;
			const float wgt = float((3 - std::abs(dx)) * (3 - std::abs(dy)));
			const int nx = x + dx, ny = y + dy;
			if (nx < 0 || ny < 0 || nx >= int(S) || ny >= int(S)) continue;
			const UINT q = UINT(ny) * S + UINT(nx);
			if (ray[q * 2] <= 0 || ray[q * 2 + 1] < 1.5f || ray[q * 2 + 1] > 3.5f) { reject[0]++; continue; }
			if (std::fabs(ray[q * 2] - ray[p * 2]) > ray[p * 2] * 0.05f + 4.0f) { reject[1]++; continue; }
			const uint32_t nf = uint32_t(hit[q * 4 + 1] + 0.5f);
			if (hit[q * 4] <= 0 || !(nf & 256u) || ((nf & 1u) | ((nf >> 1u) & 2u)) != cls) { reject[2]++; continue; }
			auto hitPoint = [&](UINT k) { const UINT kx = k % S, ky = k / S;
				return cam.ray((kx + 0.5f) / S, (ky + 0.5f) / S) * ray[k * 2] + octDecode(dir[k * 2], dir[k * 2 + 1]) * hit[k * 4]; };
			if (length(hitPoint(q) - hitPoint(p)) > hit[p * 4] * 0.25f + 32.0f) { reject[3]++; continue; }
			total = total + colourAt(hit, shade, q) * wgt; count += wgt; taken++;
		}
		avgCount += taken; avgCalls++;
		return total * (1.0f / count);
	};
	auto lum = [](V3 v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
	// the truth
	std::vector<V3> truth(S * S, V3(0, 0, 0));
	uint32_t rng = 777u;
	auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) / 16777216.0f; };
	const float a = NvrRtRoughOf(NvrRtRoughCode(0.35f)) * NvrRtRoughOf(NvrRtRoughCode(0.35f));
	for (UINT p = 0; p < S * S; p++) {
		if (!gold[p]) continue;
		V3 sum = { 0, 0, 0 };
		const V3 off = pHit[p] + nTrue[p] * 1.0f;
		for (int s = 0; s < 256; s++) {
			const V3 d2 = vndfReflect(vIn[p], nTrue[p], a, rnd(), rnd());
			const Hit b = trace(w, off, d2, 0.5f, 10000.0f, 0xEF, false);
			sum = sum + (b.hit ? w.objects[b.object].albedo : sky);
		}
		truth[p] = sum * (1.0f / 256);
	}
	struct Run { double shimmer = 0, errMean = 0, errOne = 0; int n = 0; };
	auto measure = [&](float mode, bool average, Run& out) {
		const int F = 16;
		std::vector<std::vector<V3>> frames(F, std::vector<V3>(S * S));
		for (int fr = 0; fr < F; fr++) {
			std::vector<float> hit, shade;
			if (!frame(float(1 + fr * 37), mode, hit, shade)) return false;
			for (UINT p = 0; p < S * S; p++) if (gold[p]) frames[fr][p] = average ? averaged(hit, shade, p) : colourAt(hit, shade, p);
		}
		double sh = 0, em = 0, e1 = 0; int n = 0;
		for (UINT p = 0; p < S * S; p++) {
			if (!gold[p] || frames[0][p].x < 0) continue;
			double m = 0, m2 = 0; V3 mean = { 0, 0, 0 };
			for (int fr = 0; fr < F; fr++) { const double l = lum(frames[fr][p]); m += l / F; m2 += l * l / F; mean = mean + frames[fr][p] * (1.0f / F); }
			sh += std::sqrt(std::max(m2 - m * m, 0.0));
			em += std::pow(lum(mean) - lum(truth[p]), 2.0);
			e1 += std::pow(lum(frames[0][p]) - lum(truth[p]), 2.0);
			n++;
		}
		out.n = n; out.shimmer = n ? sh / n : 0; out.errMean = n ? std::sqrt(em / n) : 0; out.errOne = n ? std::sqrt(e1 / n) : 0;
		return true;
	};
	Run before, sharp, after;
	if (!measure(2.0f, false, before) || !measure(0.0f, false, sharp) || !measure(0.0f, true, after)) { releaseWorld(h, ids); return false; }
	releaseWorld(h, ids);
	say("  the gold sphere (code %u = %.3f) in the mirror, %d pixels, 16 frames of a still view (luminance; the truth: 256 rays a pixel):\n",
		NvrRtRoughCode(0.35f), NvrRtRoughOf(NvrRtRoughCode(0.35f)), after.n);
	say("    test 11 (a new random sample each frame, no history on a mirror): shimmer %.4f, one frame vs the truth RMS %.4f, 16 frames' mean %.4f\n",
		before.shimmer, before.errOne, before.errMean);
	say("    one fixed sample, no average (what the 3 x 3 mean starts from):    shimmer %.4f, one frame %.4f\n", sharp.shimmer, sharp.errOne);
	say("    now (the fixed 9-point lobe + Resolve's 5 x 5 tent, perf-reflfix4): shimmer %.4f, one frame vs the truth RMS %.4f (%.1f samples a pixel)\n", after.shimmer, after.errOne,
		avgCalls ? avgCount / avgCalls : 0.0);
	say("    (neighbours left out: no own ray %d, another surface %d, not the same bounce %d, another first hit %d)\n", reject[0], reject[1], reject[2], reject[3]);
	const bool ok = after.n > 200 && after.shimmer < 1e-6 && after.errOne < 0.7 * before.errOne && before.shimmer > 0.01;
	say("  -> %s (now: no shimmer at all, one frame closer to the truth than test 11's single frame)\n", ok ? "ok" : "BAD");
	return ok;
}

// ---- mirrorlamp -------------------------------------------------------------------------------------------------------------------
// MirrorLampHighlight on a synthetic lit picture through Rt/ReflHighlightCheck.comp (the composite's own ReflLampHighlightScale + its ring
// + its mix). Left half: a mirror (roughness 0.02, the reflection's share 0.95, a grey 0.2 reflection): a dim textured surface (0.02-0.10)
// with three point-lamp highlights (GGX-like spots ~1.5 px wide, peaks 60000 / 2000 / 50: what the game's object shader leaves on a 0.02
// mirror), a sun glint (keep 1) and broad stripes 10 px wide (an environment pattern, 5 x contrast). Right half: the gold sphere's soft
// highlight (roughness 0.33, share 0.7, a Gaussian 12 px, peak 3) and a small dot on it. Before (MirrorLampHighlight 1) vs now (0):
// pixels over 1.0 linear after the mix (what the bloom picks up) at the lamps; the sun's glint, the stripes, the gold unchanged.
static int gShHighlight = -1;
static bool registerReflFix3(Host& h, const std::string& spv) { gShHighlight = h.AddShader(spv + "\\ReflHighlightCheck.spv"); return gShHighlight >= 0; }
static bool sceneMirrorLamp(Ctx& c) {
	Host& h = *c.host;
	if (gShHighlight < 0) return false;
	const int N = int(S);
	std::vector<float> lit(size_t(N) * N * 4, 0.0f);
	struct Spot { int x, y; float peak; bool keep; };
	const Spot lamps[] = { { 16, 20, 60000.0f, false }, { 40, 30, 2000.0f, false }, { 20, 50, 50.0f, false }, { 46, 56, 3000.0f, true } };   // (the last: the sun)
	auto at = [&](int x, int y) -> float* { return &lit[(size_t(y) * N + x) * 4]; };
	for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
		float* p = at(x, y);
		float v;
		if (x < N / 2) {
			v = 0.02f + 0.08f * (0.5f + 0.5f * std::sin(x * 0.7f) * std::cos(y * 0.9f));   // the mirror's dim texture
			if (y >= 80) v = ((x / 10) % 2) ? 0.5f : 0.1f;                                // the stripes
			// (a GGX-shaped spot, D ~ 1 / (1 + (r / w)^2)^2 with w 0.75 px: its long tail reaches the ring)
			for (const Spot& s : lamps) { const float r2 = float((x - s.x) * (x - s.x) + (y - s.y) * (y - s.y)); const float q = 1.0f + r2 / (0.75f * 0.75f); v += s.peak / (q * q); if (r2 < 9 && s.keep) p[3] = 1.0f; }
		}
		else {
			const float r2 = float((x - 96) * (x - 96) + (y - 64) * (y - 64));
			v = 0.15f + 3.0f * std::exp(-r2 / (2 * 12.0f * 12.0f)) + 40.0f * std::exp(-float((x - 100) * (x - 100) + (y - 30) * (y - 30)) / (2 * 0.75f * 0.75f));
		}
		p[0] = v; p[1] = v * 0.95f; p[2] = v * 0.9f;
	}
	struct K { float size[4], left[4], right[4]; };
	auto run = [&](float strength, std::vector<float>& out) {
		K k = {};
		put4(k.size, float(N), float(N), strength, 12.0f);
		put4(k.left, 0.02f, 0.95f, 0.2f, 0.0f);
		put4(k.right, 0.33f, 0.7f, 0.2f, 0.0f);
		if (!h.Upload(c.aux16, lit)) return false;
		h.AddPass(gShHighlight, { c.aux16, c.histA }, 0x2u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(World(), {}, V3())) return false;
		const bool ok = h.Read(c.histA, out);
		h.EndFrame();
		return ok;
	};
	std::vector<float> before, after;
	if (!run(0.0f, before) || !run(1.0f, after)) return false;
	auto lumAt = [&](const std::vector<float>& v, int x, int y) { const float* q = &v[(size_t(y) * N + x) * 4]; return 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2]; };
	int bloomBefore = 0, bloomAfter = 0, changedElsewhere = 0, goldChanged = 0;
	double lampPeakBefore = 0, lampPeakAfter = 0, sunBefore = 0, sunAfter = 0, stripesWorst = 0;
	for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
		const double b = lumAt(before, x, y), a = lumAt(after, x, y);
		bool nearLamp = false, nearSun = false, tail = false;
		for (const Spot& s : lamps) { const int d2 = (x - s.x) * (x - s.x) + (y - s.y) * (y - s.y); if (d2 <= 36) (s.keep ? nearSun : nearLamp) = true; else if (d2 <= 400) tail = true; }
		if (tail && !nearLamp && !nearSun) continue;   // (a lamp's tail, 6-20 px out: what is left of it is whatever the ring sees there)
		if (x < N / 2 && nearLamp) { lampPeakBefore = std::max(lampPeakBefore, b); lampPeakAfter = std::max(lampPeakAfter, a); if (b > 1.0) bloomBefore++; if (a > 1.0) bloomAfter++; }
		else if (x < N / 2 && nearSun) { sunBefore = std::max(sunBefore, b); sunAfter = std::max(sunAfter, a); }
		else if (x < N / 2 && y >= 80) stripesWorst = std::max(stripesWorst, std::fabs(a - b) / std::max(b, 1e-6));
		else if (x < N / 2) { if (std::fabs(a - b) > 1e-3 * std::max(b, 1e-3) + 1e-4) changedElsewhere++; }
		else if (a != b) goldChanged++;
	}
	const bool ok = bloomBefore > 0 && bloomAfter == 0 && lampPeakAfter < 1.0 && std::fabs(sunAfter / sunBefore - 1.0) < 1e-3 && stripesWorst < 1e-3 &&
		changedElsewhere == 0 && goldChanged == 0;
	say("  a mirror (0.02, share 0.95) after the composite's mix: point-lamp highlights' brightest pixel %.1f -> %.3f linear, pixels over 1.0 (bloom) "
		"%d -> %d; the sun's glint %.1f -> %.1f (kept); the 10 px stripes changed %.4f %% at most; the rest of the mirror: %d pixels changed; the gold "
		"(0.33): %d pixels changed -> %s\n", lampPeakBefore, lampPeakAfter, bloomBefore, bloomAfter, sunBefore, sunAfter, stripesWorst * 100,
		changedElsewhere, goldChanged, ok ? "ok" : "BAD");
	if (!ok) { dumpPfm("mirrorlamp_before", before, S, S, 4, 1); dumpPfm("mirrorlamp_after", after, S, S, 4, 1); }
	return ok;
}

