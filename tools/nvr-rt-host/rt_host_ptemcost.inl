// NvrRtHost, PTEmissive's cost keys (2026-11-11 pt-emissive-cost; included by rt_host.cpp after rt_host_ptcost.inl): RtPathTrace.comp's
// PTEmissiveCut / PTEmissiveFloor / PTEmissiveHits / PTEmissiveRoulette (src/core/RtEmissiveRow.h CostKeys, frame row texels 289.y-291.x)
// against PTEmissive as it was, on a PTRealtime-like frame. README.md has the table.

// ==============================================================================================================================
// Scene: ptcost's yard (a floor, the emissive scene's glowing sign + box, a back wall, a glossy side wall, a block, 7 posts), the path tracer
// as PTRealtime runs it (3 bounces, PTBounceAlpha 3, PTRestirDI with 2 neighbours, PTRestirDIRays 0, PTBounceLights 0, one camera query,
// PTSpecular, one path a pixel, a still camera, PTGlow 1). Three lightings:
//   noon   the sun + the sky, NO lamps: the sign gives a few percent of the light next to it and nothing that matters elsewhere (the bench's
//          Goodsprings at noon: PTEmissive cost 19.8 ms there);
//   night  no sun, a dim sky, no lamps: the sign is the main light (where PTEmissive earns its cost);
//   room   the yard closed (walls, ceiling), no sun / sky, 6 lamps + the sign (a lamp-lit interior).
// Runs per lighting, the ray queries a pixel counted by the shader (the count mode):
//   off    PTEmissive off (the glow through bounce rays only);
//   A      PTEmissive on, every cost key 0 (as before): the reference;
//   then one key each (PTEmissiveCut 0.02, 0.005; PTEmissiveFloor 0.002, 0.01; PTEmissiveHits 1; PTEmissiveRoulette 0.1) and all of them.
// Checked: every keyed run's picture mean (every pixel, luminance, over its frames) within max(1.5 %, 4 standard errors) of A's (the keys are
// unbiased), no NaN; at noon PTEmissiveCut 0.02 traces fewer rays than A and its per-pixel noise is under 1.1 x A's. Printed: rays a pixel,
// mean, noise, the pass's GPU time per run (the PTEmissive off run's mean is printed, not checked: brute force glow is heavy-tailed).
static bool scenePtEmCost(Ctx& c) {
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
	const std::vector<L> roomLamps = { { { -500, -300, 150 }, 700, { 1.0f, 0.9f, 0.75f } }, { { 300, -150, 120 }, 500, { 0.6f, 0.9f, 1.2f } },
		{ { 0, 100, 60 }, 400, { 0.9f, 0.9f, 0.9f } }, { { 600, 300, 200 }, 600, { 0.75f, 0.6f, 0.9f } }, { { -700, 200, 100 }, 500, { 0.9f, 0.75f, 0.6f } },
		{ { 100, -600, 250 }, 800, { 0.6f, 0.75f, 0.6f } } };
	Camera cam = Camera::lookDir({ 0, -1000, 800 }, { 0, 1100, -800 }, { 0, 0, 1 }, 1.7f, 1.7f);
	const V3 sun = normalize(V3(0.45f, -0.35f, 0.55f));
	// the lighting of the runs that follow
	struct Light { bool sunUp, exterior; float sky; std::vector<L> lamps; } light;
	const float kGlow = 1.0f;   // PTGlow (the game's default)

	struct Run { std::vector<std::vector<float>> frames; double traceMs = 0, rays = 0; };
	auto run = [&](bool emissive, const RtEmissiveRow::CostKeys& em, int frames, uint32_t seed, Run& out) {
		RtRestirRow::State state;
		RtRestirRow::Keys keys;
		keys.radius = 6.0f; keys.spatial = 2.0f;
		RtPtCost::Keys cost; cost.restirRays = 0; cost.bounceLights = 0; cost.oneCameraQuery = 1;   // (the game's defaults)
		RtEmissiveRow::State emState;
		out.frames.clear();
		double raySum = 0; unsigned long long rayPixels = 0;
		for (int f = 0; f < frames; f++) {
			std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
			const int n = int(light.lamps.size());
			std::vector<float> list(size_t(std::max(n, 1)) * 8);
			for (int k = 0; k < n; k++) {
				const L& l = light.lamps[k];
				const V3 q = l.pos - cam.eye;
				const float v[8] = { q.x, q.y, q.z, l.reach, l.colour.x, l.colour.y, l.colour.z, 0.0f };
				std::memcpy(&image[size_t(k) * 8], v, sizeof(v)); std::memcpy(&list[size_t(k) * 8], v, sizeof(v));
			}
			PathConstants k = PathConstants();
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float((uint32_t(f + 1) * 7919u + seed) & 0xFFFFFu);
			const uint32_t flags = 1024u | 2048u | 4u | 262144u | 128u | 1048576u;   // the lamp image, the game's falloff, glow, ReSTIR DI, GGX + its table
			put4(k.accum, 0.0f, 3.0f, 1.0f, float(flags));
			put4(k.sunDir, sun, light.sunUp ? 1.0f : 0.0f); put4(k.sunColor, 2.2f, 2.0f, 1.8f, std::tan(0.5f * 0.53f * 0.0174533f));
			put4(k.skyZenith, 0.25f * light.sky, 0.35f * light.sky, 0.55f * light.sky, light.exterior ? 1.0f : 0.0f);
			put4(k.skyHorizon, 0.45f * light.sky, 0.45f * light.sky, 0.45f * light.sky, kGlow);   // (skyHorizon.w = PTGlow)
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
			if (emissive) {
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
			// the cost keys as core\RtPathTrace.cpp packs them (the sky's and the sun's luminance as the trace gets them)
			const float skyLuma = light.exterior ? 0.5f * (0.2126f * (k.skyZenith[0] + k.skyHorizon[0]) + 0.7152f * (k.skyZenith[1] + k.skyHorizon[1]) + 0.0722f * (k.skyZenith[2] + k.skyHorizon[2])) : 0.0f;
			const float sunLuma = light.sunUp ? 0.2126f * k.sunColor[0] + 0.7152f * k.sunColor[1] + 0.0722f * k.sunColor[2] : 0.0f;
			const float floorBase = RtEmissiveRow::PackCost(frameRow, em, skyLuma, sunLuma, kGlow, count > 0);
			if (!h.Upload(gM.ptLamps, image)) return false;
			if (count > 0) {
				const float ec[4] = { float(count), float(emBase4096), floorBase, 0.0f };
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
	const int F = std::getenv("NVR_RT_HOST_PTEMCOST_FRAMES") ? std::max(16, std::atoi(std::getenv("NVR_RT_HOST_PTEMCOST_FRAMES"))) : 128;
	struct Case { const char* name; bool emissive; RtEmissiveRow::CostKeys keys; };
	auto K = [](float cut, float floorLight, float hits, float roulette) { RtEmissiveRow::CostKeys k; k.cut = cut; k.floor = floorLight; k.hits = hits; k.roulette = roulette; return k; };
	const std::vector<Case> cases = {
		{ "A  PTEmissive, keys 0    ", true, K(0, 0, 0, 0) },
		{ "off PTEmissive 0         ", false, K(0, 0, 0, 0) },
		{ "PTEmissiveCut 0.02       ", true, K(0.02f, 0, 0, 0) },
		{ "PTEmissiveCut 0.005      ", true, K(0.005f, 0, 0, 0) },
		{ "PTEmissiveFloor 0.002    ", true, K(0, 0.002f, 0, 0) },
		{ "PTEmissiveFloor 0.01     ", true, K(0, 0.01f, 0, 0) },
		{ "PTEmissiveHits 1         ", true, K(0, 0, 1, 0) },
		{ "PTEmissiveRoulette 0.1   ", true, K(0, 0, 0, 0.1f) },
		{ "all (0.02, 0.002, 1, 0.1)", true, K(0.02f, 0.002f, 1, 0.1f) },
	};
	auto pass = [&](const char* where, bool noon, bool& ok) {
		std::vector<Run> runs(cases.size());
		std::vector<Stats> st(cases.size());
		for (size_t i = 0; i < cases.size(); i++) {
			if (!run(cases[i].emissive, cases[i].keys, F, 11u + 12u * uint32_t(i), runs[i])) return false;
			st[i] = stats(runs[i]);
		}
		const Stats& sA = st[0];
		say("  %s: %d frames a run, 1 path, 3 bounces, ReSTIR DI, %d lamps, sun %s, sky x %.2f, PTGlow %.1f\n", where, F, int(light.lamps.size()), light.sunUp ? "up" : "down", light.exterior ? light.sky : 0.0f, kGlow);
		ok = sA.finite && sA.mean > 0.001;
		for (size_t i = 0; i < cases.size(); i++) {
			const Stats& s = st[i];
			const double lim = std::max(0.015, 4.0 * std::sqrt(s.se * s.se + sA.se * sA.se) / std::max(sA.mean, 1e-9));
			const double ratio = s.mean / std::max(sA.mean, 1e-9);
			const bool within = std::fabs(ratio - 1.0) < lim;
			const bool checked = cases[i].emissive && i > 0;
			say("    %s rays %.2f a pixel (%+.0f %%), mean x%.4f of A (limit +-%.4f%s), per-pixel noise %.4f (%.2f x A), GPU %.3f ms (%+.0f %%)\n", cases[i].name, runs[i].rays,
				runs[0].rays > 0 ? 100.0 * (runs[i].rays / runs[0].rays - 1.0) : 0.0, ratio, lim, checked ? (within ? "" : ": BAD") : ", not checked", s.noise,
				sA.noise > 0 ? s.noise / sA.noise : 0.0, runs[i].traceMs, runs[0].traceMs > 0 ? 100.0 * (runs[i].traceMs / runs[0].traceMs - 1.0) : 0.0);
			if (checked && (!within || !s.finite)) ok = false;
		}
		if (noon) {   // PTEmissiveCut 0.02 (cases[2]): fewer rays than A, no more noise
			const bool cut = runs[2].rays < 0.9 * runs[0].rays && st[2].noise < 1.1 * sA.noise;
			if (!cut) { ok = false; say("    PTEmissiveCut 0.02 at noon: rays %.2f vs %.2f, noise %.2f x A: BAD (wanted < 0.9 x the rays, < 1.1 x the noise)\n", runs[2].rays, runs[0].rays, sA.noise > 0 ? st[2].noise / sA.noise : 0.0); }
		}
		say("  -> %s\n", ok ? "ok" : "BAD");
		return true;
	};
	bool okNoon = false, okNight = false, okRoom = false;
	light.sunUp = true; light.exterior = true; light.sky = 1.0f; light.lamps.clear();
	if (!pass("noon", true, okNoon)) return false;
	light.sunUp = false; light.exterior = true; light.sky = 0.02f; light.lamps.clear();
	if (!pass("night", false, okNight)) return false;
	// the room: the yard closed by walls and a ceiling (z 500), the camera inside; no sun, no sky, 6 lamps
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
	light.sunUp = false; light.exterior = false; light.sky = 0.0f; light.lamps = roomLamps;
	cam = Camera::lookDir({ 0, -1300, 320 }, { 0, 1100, -260 }, { 0, 0, 1 }, 1.7f, 1.7f);
	if (!pass("room", false, okRoom)) return false;
	releaseWorld(h, ids);
	return okNoon && okNight && okRoom;
}
