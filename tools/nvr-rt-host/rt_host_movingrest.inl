// ==============================================================================================================================
// Scene `movingrest` (2026-11-07): MovingCasterHistory ([Shaders.ShadowsInteriors.RT], ManyLights' histories). 40 lamps over a floor,
// seen from above; a tall walker (people's mask 0x20, restaged each frame as an actor) stands still for 30 frames (the histories fill:
// 16 frames RtLampTraceMany, 31 RtLampTraceRestir), then walks 70 units a frame for 8 frames. At the last frame the traced rest share R
// against the CPU's exact R on the floor where the walker's shadow is now or was at the start (|R exact now - R exact then| > 0.2):
// with the cap 0 (no limit, as before 2026-11-07) the old shadow lingers there; with MovingCasterHistory 2 it follows. A third run
// (cap 2, the walker standing at its last place all along) is the noise floor of 2 frames of history in those pixels: the smear is
// the error above it. PASS per variant: the smear with no cap is real (> 0.06), the cap leaves under 0.35 x of it, and the rest of
// the floor stays as good (mean error < 0.06 Many / 0.1 one ray). Uses rt_host_sameframe.inl's lampLitMask.
static bool sceneMovingRest(Ctx& c) {
	Host& h = *c.host;
	std::vector<Lamp> lamps;
	World w = lampFloorWorld(lamps, 8, 5, 520, 260, 1400, {});
	Mesh walkerMesh; walkerMesh.box({ -60, -60, 0 }, { 60, 60, 420 });
	w.meshes.push_back(walkerMesh);
	Object walker; walker.mesh = int(w.meshes.size()) - 1; walker.hookMask = 0x20;
	w.objects.push_back(walker);
	const int walkerObject = int(w.objects.size()) - 1;
	std::vector<uint32_t> ids(w.meshes.size(), 0);
	if (!h.Stage(w.meshes[0], false, ids[0]) || !h.Stage(w.meshes[1], false, ids[1])) return false;
	const uint32_t walkerId = h.nextGeometry++;
	ids[walker.mesh] = walkerId;
	const Camera cam = Camera::lookDir({ 0, 0, 3400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	const float offset = 0.5f;
	const uint32_t shadowMask = 0x23;
	auto at = [&](float x) { World r = w; r.meshes[walker.mesh] = Mesh(); r.meshes[walker.mesh].box({ x - 60, 40, 0 }, { x + 60, 160, 420 }); return r; };
	struct Ref { bool valid = false, hasRest = false, stable = true, walker = false; float R = 1; };
	auto reference = [&](const World& wf, std::vector<Ref>& out) {
		out.assign(S * S, Ref());
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			Ref& r = out[j * S + i];
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(wf, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!hh.hit) continue;
			r.valid = true; r.walker = hh.object == walkerObject;
			const V3 origin = cam.eye + d * hh.t + hh.n * offset;
			std::vector<float> wt(lamps.size());
			int b0 = -1, b1 = -1; float w0 = 0, w1 = 0;
			for (size_t s = 0; s < lamps.size(); s++) {
				wt[s] = lampWeightCpu(lamps[s], origin, hh.n);
				if (!(wt[s] > 0)) continue;
				if (wt[s] > w1) { if (wt[s] > w0) { b1 = b0; w1 = w0; b0 = int(s); w0 = wt[s]; } else { b1 = int(s); w1 = wt[s]; } }
			}
			double sum = 0, vis = 0;
			for (size_t s = 0; s < lamps.size(); s++) {
				if (!(wt[s] > 0) || int(s) == b0 || int(s) == b1) continue;
				if (std::fabs(wt[s] - w1) < 1e-3f * w1) r.stable = false;
				sum += wt[s];
				vis += wt[s] * (lampLitMask(wf, origin, lamps[s], offset, shadowMask) ? 1.0 : 0.0);
			}
			if (b1 >= 0 && std::fabs(w0 - w1) < 1e-3f * w0) r.stable = false;
			r.hasRest = sum > 0; r.R = sum > 0 ? float(vis / sum) : 1.0f;
		}
	};
	const float x0 = -600.0f, step = 70.0f;
	const int still = 30, moving = 8;
	std::vector<Ref> refThen, refNow;
	reference(at(x0), refThen);
	reference(at(x0 + step * moving), refNow);
	bool ok = true;
	for (int variant = 0; variant < 2; variant++) {
		double changedErr[3] = { 0, 0, 0 }, farErr[3] = { 0, 0, 0 };
		int changedN = 0, farN = 0;
		for (int capRun = 0; capRun < 3; capRun++) {   // 0 no cap, 1 MovingCasterHistory 2, 2 the same with the walker at its last place all along (noise floor)
			const float cap = capRun ? 2.0f : 0.0f;
			std::vector<float> z(size_t(S) * S * 4, 0.0f);
			if (!h.Upload(c.histA, z) || !h.Upload(c.histB, z) || !h.Upload(gM.idsA, z) || !h.Upload(gM.idsB, z)) return false;
			int parity = 0;
			std::vector<float> result;
			for (int f = 0; f < still + moving; f++) {
				const float x = capRun == 2 ? x0 + step * moving : f < still ? x0 : x0 + step * (f - still + 1);
				const World wf = at(x);
				if (!h.StageAs(wf.meshes[walker.mesh], walkerId, NVR_RT_OP_FLAG_DYNAMIC | ((f % 8) == 7 ? NVR_RT_OP_FLAG_REBUILD : 0u))) return false;
				std::vector<GPixel> px; std::vector<float> depth, normals;
				gbuffer(wf, cam, px, depth, normals);
				if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals) || !h.Upload(c.lampTable, lampTable(lamps, cam.eye), 0, 0, 1024, 1)) return false;
				LampConstants k; lampPassConstants(k, cam, int(lamps.size()), 2);
				k.params2[1] = k.params2[2] = float(shadowMask);
				put4(k.jitter, 0.0f, 0.0f, 0.0f, 0.0f);
				if (variant == 0) { put4(k.many, 4.0f, 16.0f, 1.0f, f > 0 ? 1.0f : 0.0f); put4(k.many2, float(f), 0.05f, 1.0f, 0.0f); }
				else { put4(k.many, 0.0f, 31.0f, 1.0f, f > 0 ? 1.0f : 0.0f); put4(k.many2, float(f), 0.05f, 0.0f, 0.0f); }
				const V3 ax = cam.r * cam.p00, ay = cam.u * cam.p11, aw = cam.f;
				put4(k.reproject[0], ax.x, ay.x, 0.0f, aw.x); put4(k.reproject[1], ax.y, ay.y, 0.0f, aw.y); put4(k.reproject[2], ax.z, ay.z, 0.0f, aw.z);
				put4(k.reproject[3], 0.0f, 0.0f, 0.0f, 0.0f);
				put4(k.rect, float(S), float(S), cap, float(0x60u));
				if (variant == 0)
					h.AddPass(c.shMany, { c.depth, c.normals, c.lampResult, c.lampTable, parity ? c.histB : c.histA, parity ? c.histA : c.histB, c.depth, c.depth, c.alphaStandIn },
						0x24, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
				else
					h.AddPass(gM.shRestirLamp, { c.depth, c.normals, c.lampResult, c.lampTable, parity ? c.histB : c.histA, parity ? c.histA : c.histB,
						parity ? gM.idsB : gM.idsA, parity ? gM.idsA : gM.idsB, c.normals, c.alphaStandIn }, 0xA4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
				parity = 1 - parity;
				if (!h.Exchange(wf, ids, cam.eye)) return false;
				if (f + 1 == still + moving) { if (!h.Read(c.lampResult, result)) return false; }
				else if (!h.Finish()) return false;
				h.EndFrame();
			}
			changedN = farN = 0;
			for (UINT p = 0; p < S * S; p++) {
				const Ref& r = refNow[p]; const Ref& t = refThen[p];
				if (!r.valid || !r.stable || !r.hasRest || r.walker || !t.valid || t.walker) continue;
				const float got = (int(result[p * 2 + 1]) % 4096) / 4095.0f;
				const double e = std::fabs(got - r.R);
				if (std::fabs(r.R - t.R) > 0.2f) { changedErr[capRun] += e; changedN++; }
				else { farErr[capRun] += e; farN++; }
			}
			if (changedN) changedErr[capRun] /= changedN;
			if (farN) farErr[capRun] /= farN;
			static const char* names[2][3] = { { "movingrest_many_nocap", "movingrest_many_cap2", "movingrest_many_still" },
				{ "movingrest_restir_nocap", "movingrest_restir_cap2", "movingrest_restir_still" } };
			dumpPfm(names[variant][capRun], result, S, S, 2, 1);
		}
		const double farLimit = variant ? 0.10 : 0.06;
		// the smear = the error above the noise floor (the same pixels and cap, the walker never moved)
		const double smearNoCap = changedErr[0] - changedErr[2], smearCap = changedErr[1] - changedErr[2];
		const bool pass = changedN > 50 && smearNoCap > 0.06 && smearCap < 0.35 * smearNoCap && farErr[1] < farLimit;
		say("  %s: %d floor pixels where the walker's shadow is now or was (R changed > 0.2): mean |R - exact| %.3f with no cap, %.3f with "
			"MovingCasterHistory 2, %.3f the noise floor (cap 2, the walker never moved): smear %.3f -> %.3f (must fall under 0.35 x); %d other "
			"floor pixels: %.3f / %.3f (limit %.2f) -> %s\n", variant ? "ManyLightsOneRay (RtLampTraceRestir, 31 frames)" : "ManyLights (RtLampTraceMany, 16 frames)",
			changedN, changedErr[0], changedErr[1], changedErr[2], smearNoCap, smearCap, farN, farErr[0], farErr[1], farLimit, pass ? "ok" : "BAD");
		ok = ok && pass;
	}
	releaseWorld(h, { ids[0], ids[1], walkerId });
	return ok;
}
