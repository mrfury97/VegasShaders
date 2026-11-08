// NvrRtHost, the path tracer's ray budget (2026-11-07 perf-ptcost; included by rt_host.cpp after rt_host_emissive.inl): RtPathTrace.comp's
// PTRestirDIRays / PTBounceLights / one camera query (src/core/RtPtCost.h) against the old ray budget, on a PTRealtime-like frame. README.md
// has the table; docs/next/RT-PATH-TRACING.md section "perf-ptcost" the numbers.

// ==============================================================================================================================
// Scene: a yard under the sun and sky with the emissive scene's glowing sign + box (atlas slots 200 / 201, PTEmissive's list built each frame by
// RtEmissiveBuild), 6 lamps, a back wall, a side wall, a block and 7 posts throwing shadows; every surface lit and bouncing (albedo 0.2-0.6).
// The path tracer as PTRealtime runs it: 3 bounces, PTBounceAlpha 3, PTRestirDI, PTEmissive, PTSpecular + the energy table, one path a pixel,
// a still camera. Each run writes its ray queries a pixel (the count mode, RtPtCost.h texel 29.y) into the display's alpha:
//   A  the old budget (PTRestirDIRays 1, PTBounceLights 1, two camera queries, PTRestirDISpatial 4): the reference, 192 frames;
//   B  PTBounceLights 0 only; C  PTRestirDIRays 0 + PTRestirDISpatial 2 only; D  everything new (+ one camera query): 192 frames each.
// Checked: each run's picture mean (every pixel, luminance, over its frames) within max(1.5 %, 4 standard errors) of A's (the frames' spread);
// per-pixel noise D < 1.3 x A; D's rays < 0.8 x A's. Printed: rays a pixel, their split, the pass's GPU time per run (iGPU, 128 x 128).
static bool scenePtCost(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -1500, -1500, 0 }, { 1500, -1500, 0 }, { 1500, 1500, 0 }, { -1500, 1500, 0 });
	floor.uv.clear();
	const int N = 8;
	Mesh panel;   // (the emissive scene's sign) in the plane y = 400, facing -y
	for (int j = 0; j <= N; j++) for (int i = 0; i <= N; i++) {
		panel.pos.push_back({ -300.0f + 600.0f * i / N, 400.0f, 40.0f + 300.0f * j / N });
		panel.uv.push_back(float(i) / N); panel.uv.push_back(float(j) / N);
	}
	for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
		const uint16_t a = uint16_t(j * (N + 1) + i), b = uint16_t(a + 1), d = uint16_t(a + N + 1), e = uint16_t(d + 1);
		for (uint16_t k : { a, b, e, a, e, d }) panel.idx.push_back(k);
	}
	Mesh glowBox; glowBox.box({ 430, 80, 110 }, { 480, 130, 160 });
	Mesh block; block.box({ -150, 150, 0 }, { 150, 200, 120 }); block.uv.clear();
	Mesh backWall; backWall.box({ -1500, 900, 0 }, { 1500, 950, 500 }); backWall.uv.clear();
	Mesh sideWall; sideWall.box({ -1200, -800, 0 }, { -1150, 900, 400 }); sideWall.uv.clear();
	Mesh post; post.box({ -25, -25, 0 }, { 25, 25, 260 }); post.uv.clear();
	w.meshes = { floor, panel, glowBox, block, backWall, sideWall, post };
	{ Object o; o.mesh = 0; o.albedo = { 0.45f, 0.42f, 0.38f }; w.objects.push_back(o); }
	{ Object o; o.mesh = 1; o.glow = true; o.textured = true; o.alphaSlot = 200; o.albedo = { 0.2f, 0.2f, 0.2f }; w.objects.push_back(o); }
	{ Object o; o.mesh = 2; o.glow = true; o.textured = true; o.alphaSlot = 201; o.albedo = { 0.3f, 0.3f, 0.3f }; w.objects.push_back(o); }
	{ Object o; o.mesh = 3; o.albedo = { 0.6f, 0.55f, 0.5f }; w.objects.push_back(o); }
	{ Object o; o.mesh = 4; o.albedo = { 0.5f, 0.5f, 0.55f }; w.objects.push_back(o); }
	{ Object o; o.mesh = 5; o.albedo = { 0.55f, 0.5f, 0.45f }; o.rough = 3.0f / 7.0f; o.metal = 0.0f; w.objects.push_back(o); }
	const V3 posts[] = { { -700, -300, 0 }, { -200, -500, 0 }, { 300, -100, 0 }, { 650, -450, 0 }, { -450, 150, 0 }, { 800, 600, 0 }, { -900, 600, 0 } };
	for (const V3& p : posts) { Object o; o.mesh = 6; o.offset = p; o.albedo = { 0.4f, 0.4f, 0.4f }; w.objects.push_back(o); }
	auto panelTexel = [](int tx, int ty) -> V3 {
		const int row = ty / 8, col = tx / 8;
		if (row == 1) return { 1.0f, 0.25f, 0.1f };
		if (row == 2) return (col / 2) % 2 == 0 ? V3(0.9f, 0.9f, 0.9f) : V3(0, 0, 0);
		if (row == 4) return { 0.1f, 0.6f, 1.0f };
		if (row == 6) return { 1.0f, 0.8f, 0.2f };
		return { 0, 0, 0 };
	};
	{
		std::vector<float> t(64 * 64 * 4), u(64 * 64 * 4);
		for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
			const V3 p = panelTexel(x, y);
			float* a = &t[(y * 64 + x) * 4]; a[0] = p.x; a[1] = p.y; a[2] = p.z; a[3] = 1.0f;
			float* b = &u[(y * 64 + x) * 4]; b[0] = 1.0f; b[1] = 0.85f; b[2] = 0.6f; b[3] = 1.0f;
		}
		if (!h.Upload(gM.albedoAtlas, t, (200 % 32) * 64, (200 / 32) * 64, 64, 64) || !h.Upload(gM.albedoAtlas, u, (201 % 32) * 64, (201 / 32) * 64, 64, 64)) return false;
	}
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	struct L { V3 pos; float reach; V3 colour; };
	const std::vector<L> lamps = { { { -500, -300, 150 }, 700, { 1.0f, 0.9f, 0.75f } }, { { 300, -150, 120 }, 500, { 0.6f, 0.9f, 1.2f } },
		{ { 0, 100, 60 }, 400, { 0.9f, 0.9f, 0.9f } }, { { 600, 300, 200 }, 600, { 0.75f, 0.6f, 0.9f } }, { { -700, 200, 100 }, 500, { 0.9f, 0.75f, 0.6f } },
		{ { 100, -600, 250 }, 800, { 0.6f, 0.75f, 0.6f } } };
	Camera cam = Camera::lookDir({ 0, -1000, 800 }, { 0, 1100, -800 }, { 0, 0, 1 }, 1.7f, 1.7f);
	const V3 sun = normalize(V3(0.45f, -0.35f, 0.55f));
	bool interior = false;   // the second pass: walls and a ceiling round the yard, no sun, no sky (the bounces stay in the room)

	struct Run { std::vector<std::vector<float>> frames; double traceMs = 0, rays = 0; };
	auto run = [&](const RtPtCost::Keys& cost, float spatial, int frames, uint32_t seed, Run& out) {
		RtRestirRow::State state;
		RtRestirRow::Keys keys;
		keys.radius = 6.0f; keys.spatial = spatial;
		RtEmissiveRow::State emState;
		out.frames.clear();
		double raySum = 0; unsigned long long rayPixels = 0;
		for (int f = 0; f < frames; f++) {
			std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
			std::vector<float> list(lamps.size() * 8);
			const int n = int(lamps.size());
			for (int k = 0; k < n; k++) {
				const L& l = lamps[k];
				const V3 q = l.pos - cam.eye;
				const float v[8] = { q.x, q.y, q.z, l.reach, l.colour.x, l.colour.y, l.colour.z, 0.0f };
				std::memcpy(&image[size_t(k) * 8], v, sizeof(v)); std::memcpy(&list[size_t(k) * 8], v, sizeof(v));
			}
			PathConstants k = PathConstants();
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float((uint32_t(f + 1) * 7919u + seed) & 0xFFFFFu);
			const uint32_t flags = 1024u | 2048u | 4u | 262144u | 128u | 1048576u;   // the lamp image, the game's falloff, glow, ReSTIR DI, GGX + its table
			put4(k.accum, 0.0f, 3.0f, 1.0f, float(flags));
			put4(k.sunDir, sun, interior ? 0.0f : 1.0f); put4(k.sunColor, 2.2f, 2.0f, 1.8f, std::tan(0.5f * 0.53f * 0.0174533f));
			put4(k.skyZenith, 0.25f, 0.35f, 0.55f, interior ? 0.0f : 1.0f); put4(k.skyHorizon, 0.45f, 0.45f, 0.45f, 3.0f);   // (skyHorizon.w = PTGlow)
			put4(k.misc, float(n), 0.0f, 60000.0f, 0.0f);
			k.rect[3] = 3.0f;   // PTBounceAlpha 3 (PTRealtime)
			const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
			const float eye[3] = { 0, 0, 0 };
			float* frameRow = &image[size_t(kLightTexels) * kFrameRowIndex * 2];
			RtRestirRow::Build(state, frameRow, image.data(), list.data(), n, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
			RtPtCost::PackRow(frameRow, cost);
			frameRow[2 * RtPtCost::kTexel + 3] = 1.0f;   // the count mode
			int count = 0;
			for (Object& o : w.objects) o.emissive = 0;
			{
				std::vector<RtEmissiveRow::Source> src;
				for (size_t i = 0; i < w.objects.size(); i++) {
					const Object& o = w.objects[i];
					if (!o.glow) continue;
					RtEmissiveRow::Source s;
					s.record = uint32_t(i); s.geometryId = ids[o.mesh]; s.triangles = uint32_t(w.meshes[o.mesh].idx.size() / 3);
					s.transform[0][0] = s.transform[1][1] = s.transform[2][2] = o.scale;
					s.transform[0][3] = o.offset.x - cam.eye.x; s.transform[1][3] = o.offset.y - cam.eye.y; s.transform[2][3] = o.offset.z - cam.eye.z;
					s.center[0] = -cam.eye.x; s.center[1] = 400 - cam.eye.y; s.center[2] = 200 - cam.eye.z; s.radius = 600;
					s.rgb[0] = s.rgb[1] = s.rgb[2] = 0.5f;
					src.push_back(s);
				}
				std::vector<RtEmissiveRow::Mark> marks;
				count = RtEmissiveRow::Build(emState, src.data(), int(src.size()), RtEmissiveRow::kMax, RtEmissiveRow::kTrianglesMax, camWorld,
					&image[size_t(kLightTexels) * kEmissiveRowIndex * 2], marks);
				for (const RtEmissiveRow::Mark& m : marks) w.objects[m.record].emissive = m.number;
			}
			const unsigned emBase4096 = 1024u;   // (as the emissive scene: past the reservoirs)
			frameRow[2 * RtEmissiveRow::kFrameTexel] = float(count); frameRow[2 * RtEmissiveRow::kFrameTexel + 1] = float(emBase4096);
			if (!h.Upload(gM.ptLamps, image)) return false;
			if (count > 0) {
				const float ec[4] = { float(count), float(emBase4096), 0.0f, 0.0f };
				h.AddPass(gEmissiveShader, { gM.ptLamps, gM.albedoAtlas }, 0u, ec, sizeof(ec), uint32_t(count), 1);
			}
			const std::vector<int> imgs{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, gM.albedoAtlas, c.depth, c.normals, c.aux16, c.pano, c.aux16,
				c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(w, ids, cam.eye)) return false;
			std::vector<float> disp;
			if (!h.Read(c.ptDisplay, disp)) return false;
			h.EndFrame();
			for (size_t p = 0; p < size_t(S) * S; p++) { const float a = disp[p * 4 + 3]; if (a > 0.5f) { raySum += a - 1.0; rayPixels++; } }
			out.frames.push_back(std::move(disp));
		}
		if (h.Finish()) out.traceMs = h.shared->traceUsAvg / 1000.0;
		out.rays = rayPixels ? raySum / double(rayPixels) : 0.0;
		for (Object& o : w.objects) o.emissive = 0;
		return true;
	};
	auto luma = [](const float* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; };
	// the picture's mean luminance (every pixel) per frame -> mean + standard error; per-pixel noise (frames' std, RMS over the pixels)
	struct Stats { double mean = 0, se = 0, noise = 0; bool finite = true; std::vector<double> pixelMean; };
	auto stats = [&](const Run& r) {
		Stats s;
		const size_t F = r.frames.size(), P = size_t(S) * S;
		std::vector<double> frameMean(F, 0.0);
		s.pixelMean.assign(P, 0.0);
		std::vector<double> m2(P, 0.0);
		for (size_t f = 0; f < F; f++) for (size_t p = 0; p < P; p++) {
			const float* q = &r.frames[f][p * 4];
			if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2])) { s.finite = false; continue; }
			const double y = luma(q);
			frameMean[f] += y / double(P); s.pixelMean[p] += y / double(F); m2[p] += y * y / double(F);
		}
		for (double v : frameMean) s.mean += v / double(F);
		double var = 0; for (double v : frameMean) var += (v - s.mean) * (v - s.mean);
		s.se = F > 1 ? std::sqrt(var / double(F - 1) / double(F)) : 0.0;
		double nv = 0; for (size_t p = 0; p < P; p++) nv += std::max(m2[p] - s.pixelMean[p] * s.pixelMean[p], 0.0);
		s.noise = std::sqrt(nv / double(P));
		return s;
	};
	const int F = std::getenv("NVR_RT_HOST_PTCOST_FRAMES") ? std::max(16, std::atoi(std::getenv("NVR_RT_HOST_PTCOST_FRAMES"))) : 192;
	RtPtCost::Keys kA; kA.restirRays = 1; kA.bounceLights = 1; kA.oneCameraQuery = 0;
	RtPtCost::Keys kB = kA; kB.bounceLights = 0;
	RtPtCost::Keys kC = kA; kC.restirRays = 0;
	RtPtCost::Keys kD; kD.restirRays = 0; kD.bounceLights = 0; kD.oneCameraQuery = 1;
	RtPtCost::Keys kE = kD; kE.restirRays = 3;   // (informational: the cheapest budget that keeps visibility in ReSTIR DI's targets)
	// one pass of the four runs (the yard, then the room), its lines and its verdict
	auto pass4 = [&](const char* where, bool& ok) {
		Run a, b, cc, d, e;
		if (!run(kA, 4.0f, F, 11u, a) || !run(kB, 4.0f, F, 23u, b) || !run(kC, 2.0f, F, 37u, cc) || !run(kD, 2.0f, F, 53u, d) || !run(kE, 2.0f, F, 71u, e)) return false;
		const Stats sA = stats(a), sB = stats(b), sC = stats(cc), sD = stats(d), sE = stats(e);
		auto rmsDiff = [&](const Stats& x) { double e = 0; for (size_t p = 0; p < x.pixelMean.size(); p++) { const double q = x.pixelMean[p] - sA.pixelMean[p]; e += q * q; } return std::sqrt(e / double(x.pixelMean.size())); };
		auto line = [&](const char* name, const Run& r, const Stats& s) {
			say("  %s: rays %.2f a pixel, mean %.4f (x%.4f of A +- %.4f), per-pixel noise %.4f (%.2f x A), per-pixel mean vs A RMS %.4f, GPU %.3f ms\n", name, r.rays, s.mean,
				sA.mean > 0 ? s.mean / sA.mean : 0.0, sA.mean > 0 ? std::sqrt(s.se * s.se + sA.se * sA.se) / sA.mean : 0.0, s.noise, sA.noise > 0 ? s.noise / sA.noise : 0.0, rmsDiff(s), r.traceMs);
		};
		say("  %s: %d frames a run, 1 path, 3 bounces, %s6 lamps + the sign (PTEmissive, ReSTIR DI)\n", where, F, interior ? "" : "sun + sky + ");
		line("A old budget            ", a, sA);
		line("B PTBounceLights 0      ", b, sB);
		line("C PTRestirDIRays 0 + 2 n", cc, sC);
		line("D all new + 1 cam query ", d, sD);
		line("(E as D, PTRestirDIRays 3)", e, sE);
		auto within = [&](const Stats& s) { const double lim = std::max(0.015, 4.0 * std::sqrt(s.se * s.se + sA.se * sA.se) / std::max(sA.mean, 1e-9)); return std::fabs(s.mean / std::max(sA.mean, 1e-9) - 1.0) < lim; };
		const bool finite = sA.finite && sB.finite && sC.finite && sD.finite;
		ok = finite && sA.mean > 0.01 && within(sB) && within(sC) && within(sD) && sD.noise < 1.3 * sA.noise && d.rays < 0.8 * a.rays;
		say("  -> %s%s\n", ok ? "ok" : "BAD", finite ? "" : " (NaN / inf in the output)");
		return true;
	};
	bool okYard = false, okRoom = false;
	if (!pass4("the yard", okYard)) return false;
	// the room: the yard closed by walls and a ceiling (z 500), the camera inside; no sun, no sky
	releaseWorld(h, ids);
	{
		Mesh frontWall; frontWall.box({ -1500, -1450, 0 }, { 1500, -1400, 540 }); frontWall.uv.clear();
		Mesh rightWall; rightWall.box({ 1150, -1450, 0 }, { 1200, 950, 540 }); rightWall.uv.clear();
		Mesh ceiling; ceiling.box({ -1500, -1500, 500 }, { 1500, 1500, 540 }); ceiling.uv.clear();
		const int m0 = int(w.meshes.size());
		w.meshes.push_back(frontWall); w.meshes.push_back(rightWall); w.meshes.push_back(ceiling);
		for (int i = 0; i < 3; i++) { Object o; o.mesh = m0 + i; o.albedo = { 0.5f, 0.48f, 0.45f }; w.objects.push_back(o); }
	}
	ids.clear();
	if (!stageWorld(h, w, ids)) return false;
	interior = true;
	cam = Camera::lookDir({ 0, -1300, 320 }, { 0, 1100, -260 }, { 0, 0, 1 }, 1.7f, 1.7f);
	if (!pass4("the room", okRoom)) return false;
	releaseWorld(h, ids);
	return okYard && okRoom;
}
