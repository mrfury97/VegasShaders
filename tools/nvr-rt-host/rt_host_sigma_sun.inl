// NvrRtHost, the RT shadow denoiser on the sun (2026-11-07, docs/next/RT-DENOISER.md; included by rt_host.cpp after rt_host_sigma.inl).
// RtSunShadow.comp (half resolution, one ray a pixel over the sun's disc, blocker distances) + the module in sun mode, against the exact
// share of the disc each traced pixel sees (n x n stratified directions with the shader's own disc sampling). Two discs: a wide one (10
// degrees across: penumbrae a 128-pixel test screen can show) and the real sun (0.53 degrees: hard, contact shadows everywhere).

static bool sceneSigmaSun(Ctx& c) {
	Host& h = *c.host;
	const World w = sigmaWorld();
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const V3 sun = normalize({ -0.70f, 0.50f, 0.42f });   // low: long shadows
	const UINT T = S / 2;
	auto cameraAt = [](float dx) { return Camera::lookDir({ 40 + dx, -300, 260 }, V3(60 + dx, 60, 0) - V3(40 + dx, -300, 260), { 0, 0, 1 }, 1.0f, 1.0f); };
	RtShadowDenoise::Settings settings;
	if (const char* e = std::getenv("NVR_SIGMA_CORE")) settings.core = std::atoi(e);   // 0 ours, 1 AMD FidelityFX Shadows
	if (const char* e = std::getenv("NVR_SIGMA_HIST")) settings.maxHistory = (float)std::atof(e);   // (experiments)
	if (const char* e = std::getenv("NVR_SIGMA_STAB")) settings.stabilize = (float)std::atof(e);
	if (const char* e = std::getenv("NVR_SIGMA_MINHIST")) settings.minHistory = (float)std::atof(e);
	bool ok = true;
	for (int disc = 0; disc < 2; disc++) {
		const float tanHalf = std::tan((disc == 0 ? 10.0f : 0.53f) * 0.5f * 3.14159265f / 180.0f);
		// the traced pixel's point (as sceneSun: the traced uv, the full-size pixel's depth) and the exact share of the disc
		auto reference = [&](const Camera& cam, int n, std::vector<float>& exact, std::vector<uint8_t>& stable, std::vector<uint8_t>& contact) {
			exact.assign(T * T, -1.0f); stable.assign(T * T, 0); contact.assign(T * T, 0);
			const V3 t1 = normalize(std::fabs(sun.z) < 0.9f ? cross(sun, V3(0, 0, 1)) : cross(sun, V3(1, 0, 0))), t2 = cross(sun, t1);
			for (UINT j = 0; j < T; j++) for (UINT i = 0; i < T; i++) {
				const UINT si = 2 * i + 1, sj = 2 * j + 1;
				Hit hs[5]; bool st = true;
				for (int s = 0; s < 5; s++) {
					const float du = s == 1 ? -1.0f : s == 2 ? 1.0f : 0.0f, dv = s == 3 ? -1.0f : s == 4 ? 1.0f : 0.0f;
					hs[s] = trace(w, cam.eye, cam.ray((si + 0.5f + du) / S, (sj + 0.5f + dv) / S), 1e-3f, 1e9f, 0xFF, true);
					if (hs[s].hit != hs[0].hit || (hs[s].hit && (hs[s].object != hs[0].object || std::fabs(hs[s].t - hs[0].t) > 0.05f * hs[0].t))) st = false;
				}
				const Hit& sh = hs[0];
				if (!sh.hit) continue;
				stable[j * T + i] = st;
				const V3 dir0 = cam.ray(float(si + 0.5f) / S, float(sj + 0.5f) / S);
				const V3 pos = cam.ray((i + 0.5f) / T, (j + 0.5f) / T) * sh.t;   // the shader's point: the traced uv at the source pixel's depth
				const V3 world = cam.eye + dir0 * sh.t;
				contact[j * T + i] = sh.object == 0 && (length(V3(world.x, world.y, 0)) < 30.0f || length(V3(world.x + 120, world.y - 90, 0)) < 30.0f);
				if (dot(sh.n, sun) <= 0) continue;
				const V3 start = pos + sh.n * (1.0f + 0.001f * sh.t) + sun * 0.5f;
				int lit = 0;
				for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
					const float r = std::sqrt((a + 0.5f) / n) * tanHalf, phi = 6.2831853f * (b + 0.5f) / n;
					const V3 dir = normalize(sun + (t1 * std::cos(phi) + t2 * std::sin(phi)) * r);
					if (!trace(w, cam.eye + start, dir, 0.0f, 30000.0f, 0xFF, true).hit) lit++;
				}
				exact[j * T + i] = float(lit) / float(n * n);
			}
		};
		auto runFrame = [&](const Camera& cam, const Camera* before, unsigned frame, bool freeze, bool denoise, std::vector<float>& vis) -> bool {
			std::vector<GPixel> px; std::vector<float> depth, normals;
			gbuffer(w, cam, px, depth, normals);
			if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
			SunConstants k = {};
			put4(k.size, float(T), float(T), 1.0f / T, 1.0f / T);
			put4(k.forward, cam.f, cam.farZ); put4(k.right, cam.r * (1.0f / cam.p00), 0); put4(k.up, cam.u * (1.0f / cam.p11), 0);
			put4(k.sun, sun, 1.0f);
			put4(k.bias, 1.0f, 0.001f, 0.5f, 30000.0f);
			put4(k.range, 100000.0f, 1.0f, 255.0f, 0.0f);
			put4(k.alpha, 1.0f, float(frame), tanHalf, 1.0f);    // alpha tested, the frame (the disc pattern), the disc, blocker distances out
			put4(k.full, float(S), float(S), 0.0f, 0.0f);
			put4(k.camera, cam.eye, 0.0f);
			h.AddPass(gM.shSun, { c.depth, c.normals, gM.sunVis, c.alphaAtlas, c.normals, gM.sunHit }, 0x24, &k, sizeof(k), (T + 7) / 8, (T + 7) / 8);
			if (denoise) {
				RtShadowDenoise::View now = viewOf(cam), prev = before ? viewOf(*before) : RtShadowDenoise::View();
				if (freeze && before) for (int a = 0; a < 3; a++) prev.eye[a] = now.eye[a];
				RtShadowDenoise::DenoiseConstants d;
				RtShadowDenoise::Fill(d, now, prev, T, T, S, S, 0, 1, frame, settings, false, tanHalf);
				d.layerReset[0][0] = before ? 0.0f : 1.0f;
				h.AddPass(gS.shDenoise, { c.depth, c.normals, c.aux16, gM.sunVis, gM.sunHit }, 0x8, &d, sizeof(d), 1, 1);
			}
			if (!h.Exchange(w, ids, cam.eye)) return false;
			const bool okRead = h.Read(gM.sunVis, vis);
			h.EndFrame();
			return okRead;
		};
		auto err = [&](const std::vector<float>& est, const std::vector<float>& ex, const std::vector<uint8_t>& st, const std::vector<uint8_t>& co) {
			SigmaErr e;
			double se = 0, sp = 0, sc = 0, bias = 0;
			for (size_t i = 0; i < ex.size(); i++) {
				if (!st[i] || ex[i] < 0) continue;
				const double d = double(est[i]) - ex[i];
				se += d * d; e.nAll++;
				if (ex[i] > 0.02f && ex[i] < 0.98f) { sp += d * d; bias += d; e.nPen++; }
				if (co[i]) { sc += d * d; e.nContact++; }
			}
			e.all = std::sqrt(se / std::max(e.nAll, 1)); e.pen = std::sqrt(sp / std::max(e.nPen, 1)); e.contact = std::sqrt(sc / std::max(e.nContact, 1));
			e.bias = bias / std::max(e.nPen, 1);
			return e;
		};
		const char* label = disc == 0 ? "10 deg disc" : "the real sun, 0.53 deg";
		const Camera still = cameraAt(0.0f);
		std::vector<float> exact, vis; std::vector<uint8_t> stable, contact;
		reference(still, 24, exact, stable, contact);
		if (!runFrame(still, nullptr, 1, false, false, vis)) { releaseWorld(h, ids); return false; }
		const SigmaErr raw = err(vis, exact, stable, contact);
		if (disc == 0) dumpPfm("sigma_sun_raw", vis, T, T, 1);
		SigmaErr settled;
		Camera prev = still;
		for (unsigned f = 1; f <= 48; f++) {
			if (!runFrame(still, f > 1 ? &prev : nullptr, f, false, true, vis)) { releaseWorld(h, ids); return false; }
			if (f == 48) settled = err(vis, exact, stable, contact);
		}
		if (disc == 0) { dumpPfm("sigma_sun_denoised", vis, T, T, 1); dumpPfm("sigma_sun_exact", exact, T, T, 1); }
		say("  sun, %s: RMSE vs exact (all / penumbrae / contact; %d / %d / %d pixels): raw ray %.4f / %.4f / %.4f, denoised (frame 48) %.4f / %.4f / %.4f (bias %+.4f)\n",
			label, raw.nAll, raw.nPen, raw.nContact, raw.all, raw.pen, raw.contact, settled.all, settled.pen, settled.contact, settled.bias);
		// a soft disc: well under the raw ray's error; the real sun (penumbrae under a pixel here, the ray is almost the answer): better
		// at rest, no worse in motion (a denoiser must not smear a hard shadow)
		const bool soft = disc == 0;
		bool discOk = settled.contact < 0.5 * raw.contact + 0.02 && (soft ? settled.all < 0.5 * raw.all && std::fabs(settled.bias) < 0.03 : settled.all < raw.all);
		if (soft && raw.nPen > 50) discOk = discOk && settled.pen < 0.35 * raw.pen;
		// moving: 32 frames, 6 units a frame, and the frozen control
		double pen[2] = {}, all[2] = {};
		for (int control = 0; control < 2; control++) {
			prev = cameraAt(0.0f);
			for (unsigned f = 1; f <= 32; f++) {
				const Camera cam = cameraAt(6.0f * float(f));
				if (!runFrame(cam, f > 1 ? &prev : nullptr, 100 + f, control == 1, true, vis)) { releaseWorld(h, ids); return false; }
				prev = cam;
				if (f == 24 || f == 32) {
					std::vector<float> ex; std::vector<uint8_t> st, co;
					reference(cam, 16, ex, st, co);
					const SigmaErr e = err(vis, ex, st, co);
					pen[control] += e.pen * 0.5; all[control] += e.all * 0.5;
				}
			}
		}
		if (disc == 0) { dumpPfm("sigma_sun_moving", vis, T, T, 1); }
		say("    moving: denoised all %.4f, penumbrae %.4f; the frozen reprojection all %.4f, penumbrae %.4f\n", all[0], pen[0], all[1], pen[1]);
		discOk = discOk && all[0] < (soft ? 0.6 : 1.1) * raw.all && all[1] > 1.2 * all[0];
		say("  sun, %s -> %s\n", label, discOk ? "ok" : "FAIL");
		ok = ok && discOk;
	}
	releaseWorld(h, ids);
	return ok;
}
