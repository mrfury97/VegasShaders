// ==============================================================================================================================
// (2026-11-08 perf-reflfix4, the user: "fix blocky mirrors") scene mirroraa. Included by rt_host.cpp after rt_host_reflfix4.inl
// (dumpPfmRgb, gRF, ReflConstants).
//
// A mirror floor (roughness 0.02) reflecting a box with sharp outlines and a hard sun shadow, seen at the OUTPUT resolution F = 128 (the
// reflection grid is half: 64). Three ways to fill a mirror pixel, against a reference of 4 stratified rays per OUTPUT pixel (2 x 2), over a
// still phase (frames 0-23), a camera pan (24-39) and the box moving through the reflection with the camera still (40-63):
//   old : one ray through each grid texel's centre, every frame the same, no history; the composite's bilinear upsample (test 12)
//   A   : one ray per OUTPUT pixel (4x the rays on mirror pixels), no history
//   B   : MirrorJitter: one ray per grid texel from 8 Halton (2, 3) points of the texel (RtMirrorJitter.glsl), accumulated as the RT
//         reflection denoiser does on its virtual path (RtReflDenoiseOursReproject.frag: the hit point along the lengthened view ray in last
//         frame's camera, bilinear over texels whose hit distance agrees, at most MirrorJitterHistory 8 frames, the 3 x 3 anti-lag at 2.5
//         sigma), then the bilinear upsample
// The colours are the CPU tracer's (what a ray meets, lit by the sun through one shadow ray): the study is the sampling and the history, the
// same rules as the shaders. Measured (luminance): edges = RMS vs the reference on the reference's edge pixels at frame 23 (stair-steps);
// shimmer = per-pixel standard deviation over frames 16-23; error still / moving (24-63): what motion adds (ghosting and lag).
// Part 1 (GPU): the helper moves the ray's start by the code in -frame.y as RayGen / Resolve do (ReflectionsRTBody.glsl).
// Part 2 (GPU timing on this GPU): the helper's trace pass at one ray per grid texel vs one per output pixel (option A's cost).
static bool sceneMirrorAA(Ctx& c) {
	Host& h = *c.host;
	const int F = int(S), G = F / 2;
	const V3 sun = normalize({ -0.7f, -0.25f, 0.45f });
	const V3 sky = { 0.6f, 0.7f, 0.8f };
	// a box turned 30 degrees (slanted outlines), a sphere (a curved one), a wall behind them taking their sun shadows (hard, slanted edges)
	Mesh turned; turned.box({ -130, -130, 0 }, { 130, 130, 600 });
	for (V3& p : turned.pos) { const float cs = 0.866f, sn = 0.5f; p = V3(p.x * cs - p.y * sn, p.x * sn + p.y * cs, p.z); }
	const Mesh ball = uvSphere({ 0, 0, 230 }, 200.0f, 40, 20);
	auto makeWorld = [&](float boxX) {
		World w; w.meshes.resize(4);
		w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
		w.meshes[1] = turned;
		w.meshes[2].box({ -1600, 1000, 0 }, { 1600, 1100, 1300 });
		w.meshes[3] = ball;
		Object fl; fl.mesh = 0; fl.albedo = { 0.5f, 0.5f, 0.5f };
		Object bx; bx.mesh = 1; bx.offset = { boxX, 450, 0 }; bx.albedo = { 0.9f, 0.15f, 0.1f };
		Object wl; wl.mesh = 2; wl.albedo = { 0.85f, 0.85f, 0.8f };
		Object bl; bl.mesh = 3; bl.offset = { 380, 600, 0 }; bl.albedo = { 0.2f, 0.4f, 0.9f };
		w.objects = { fl, bx, wl, bl };
		return w;
	};
	struct Shaded { V3 colour; float t; };
	auto shade = [&](const World& w, V3 o, V3 d) {
		const Hit hh = trace(w, o, d, 1.0f, 20000.0f, 0xEF, false);
		Shaded r; r.colour = sky; r.t = 0.0f;
		if (!hh.hit) return r;
		const V3 p = o + d * hh.t, n = dot(hh.n, d) > 0 ? hh.n * -1.0f : hh.n;
		const float lit = dot(n, sun) > 0 && !trace(w, p + n * 0.5f, sun, 0.5f, 20000.0f, 0xEF, false).hit ? 1.0f : 0.0f;
		r.colour = w.objects[hh.object].albedo * (0.3f + 0.7f * lit * std::max(dot(n, sun), 0.0f));
		r.t = hh.t;
		return r;
	};
	struct Sample { V3 colour = { 0, 0, 0 }; float t = 0; V3 virt; float z = 0; };
	auto sampleAt = [&](const World& w, const Camera& cam, float u, float v) {
		Sample s;
		const V3 d = cam.ray(u, v);
		if (!(d.z < 0)) return s;
		const float z = -cam.eye.z / d.z;   // the floor (view z: d's forward component is 1)
		const Hit first = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xEF, false);
		if (first.hit && first.object != 0) return s;   // something stands in front of the mirror
		const V3 vd = normalize(d), r = { vd.x, vd.y, -vd.z };
		const V3 p = cam.eye + d * z;
		const Shaded c2 = shade(w, p + V3(0, 0, 0.5f), r);
		s.colour = c2.colour; s.t = c2.t; s.z = z;
		s.virt = cam.eye + vd * (length(d * z) + (c2.t > 0 ? c2.t : 4000.0f));
		return s;
	};
	auto lum = [](V3 v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
	auto halton = [](int i, int b) { float f = 1, r = 0; for (i += 1; i > 0; i /= b) { f /= b; r += f * (i % b); } return r; };
	auto upsample = [&](const std::vector<V3>& g, const std::vector<int>& ok, int x, int y) {   // the composite's bilinear upsample
		const float gx = (x + 0.5f) / 2.0f - 0.5f, gy = (y + 0.5f) / 2.0f - 0.5f;
		const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
		const float fx = gx - x0, fy = gy - y0;
		V3 sum = { 0, 0, 0 }; float ws = 0;
		for (int k = 0; k < 4; k++) {
			const int qx = std::min(std::max(x0 + (k & 1), 0), G - 1), qy = std::min(std::max(y0 + (k >> 1), 0), G - 1);
			if (!ok[qy * G + qx]) continue;
			const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
			sum = sum + g[qy * G + qx] * wgt; ws += wgt;
		}
		return ws > 1e-4f ? sum * (1.0f / ws) : V3(-1, -1, -1);
	};
	const int frames = 64;
	// B's state (the denoiser's history), for two history lengths
	struct BState { std::vector<V3> hist; std::vector<float> n, t; std::vector<int> ok; float cap; std::vector<std::vector<double>> out; };
	BState bs[2];
	for (int k = 0; k < 2; k++) { bs[k].hist.assign(G * G, V3(0, 0, 0)); bs[k].n.assign(G * G, 0.0f); bs[k].t.assign(G * G, 0.0f); bs[k].ok.assign(G * G, 0);
		bs[k].cap = 8.0f; bs[k].out.resize(frames); }
	std::vector<std::vector<double>> outOld(frames), outA(frames), outRef(frames), outIdeal(frames);
	Camera lastCam;
	for (int f = 0; f < frames; f++) {
		const float pan = f < 24 ? 0.0f : f < 40 ? (f - 23) * 12.0f : 16 * 12.0f;
		const float boxX = f < 40 ? -300.0f : -300.0f + (f - 39) * 18.0f;
		const World w = makeWorld(boxX);
		const Camera cam = Camera::lookDir({ pan, -800, 450 }, normalize({ 0, 1, -0.55f }), { 0, 0, 1 }, 1.6f, 1.6f);
		std::vector<Sample> sc(G * G), sj(G * G);
		const float jx = halton(f & 7, 2) - 0.5f, jy = halton(f & 7, 3) - 0.5f;
		for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) {
			sc[y * G + x] = sampleAt(w, cam, (x + 0.5f) / G, (y + 0.5f) / G);
			sj[y * G + x] = sampleAt(w, cam, (x + 0.5f + jx) / G, (y + 0.5f + jy) / G);
		}
		std::vector<V3> gc(G * G, V3(0, 0, 0)); std::vector<int> okC(G * G, 0);
		for (int p = 0; p < G * G; p++) { okC[p] = sc[p].z > 0; gc[p] = sc[p].colour; }
		std::vector<V3> accB[2];
		std::vector<float> track(G * G, 4000.0f);   // the tracking hit distance: the nearest of the 3 x 3 jittered hits (RtReflDenoiseOursReproject.frag)
		for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) { float best = 1e9f; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { const int qx = x + dx, qy = y + dy; if (qx < 0 || qy < 0 || qx >= G || qy >= G) continue; const float t = sj[qy * G + qx].t; if (t > 0) best = std::min(best, t); } track[y * G + x] = best < 1e8f ? best : 4000.0f; }
		std::vector<int> okB(G * G, 0);
		for (int v = 0; v < 2; v++) {
			BState& st = bs[v];
			accB[v].assign(G * G, V3(0, 0, 0));
			std::vector<float> nB(G * G, 0.0f);
			for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) {
				const int p = y * G + x;
				const Sample& s = sj[p];
				if (!(s.z > 0)) continue;
				okB[p] = 1;
				V3 signal = s.colour; float n = 1;
				V3 history = { 0, 0, 0 }; float hw = 0, hn = 0;
				if (f > 0 && sc[p].z > 0) {   // (as the shader: from the texel CENTRE (SurfaceAt), lengthened by this frame's hit distance) the virtual point in last frame's camera, bilinear over texels whose tracking hit distance agrees
					const V3 dcn = normalize(cam.ray((x + 0.5f) / G, (y + 0.5f) / G)); const V3 rel = (cam.eye + dcn * (length(cam.ray((x + 0.5f) / G, (y + 0.5f) / G) * sc[p].z) + (s.t > 0 ? s.t : 4000.0f))) - lastCam.eye;
					const float vz = dot(rel, lastCam.f);
					if (vz > 1) {
						const float u = (dot(rel, lastCam.r) / vz * lastCam.p00 + 1) * 0.5f, vv = (1 - dot(rel, lastCam.u) / vz * lastCam.p11) * 0.5f;
						const float gx = u * G - 0.5f, gy = vv * G - 0.5f;
						const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
						const float fx = gx - x0, fy = gy - y0;
						const float hit = track[p];   // (the shader's tracking hit: the nearest of the 3 x 3)
						for (int k = 0; k < 4; k++) {
							const int qx = x0 + (k & 1), qy = y0 + (k >> 1);
							if (qx < 0 || qy < 0 || qx >= G || qy >= G) continue;
							const int q = qy * G + qx;
							if (!st.ok[q] || std::fabs(st.t[q] - hit) > 0.25f * hit + 10.0f) continue;
							const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
							history = history + st.hist[q] * wgt; hn += st.n[q] * wgt; hw += wgt;
						}
					}
				}
				if (hw > 0.25f) {
					history = history * (1.0f / hw); hn /= hw;
					n = std::min(hn + 1.0f, st.cap);
					V3 m1 = { 0, 0, 0 }, m2 = { 0, 0, 0 }; float cnt = 0;
					for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
						const int qx = x + dx, qy = y + dy;
						if (qx < 0 || qy < 0 || qx >= G || qy >= G || !(sj[qy * G + qx].z > 0)) continue;
						const V3 c3 = sj[qy * G + qx].colour; m1 = m1 + c3; m2 = m2 + V3(c3.x * c3.x, c3.y * c3.y, c3.z * c3.z); cnt++;
					}
					m1 = m1 * (1.0f / cnt); m2 = m2 * (1.0f / cnt);
					const V3 sg = { std::sqrt(std::max(m2.x - m1.x * m1.x, 0.0f)), std::sqrt(std::max(m2.y - m1.y * m1.y, 0.0f)), std::sqrt(std::max(m2.z - m1.z * m1.z, 0.0f)) };
					const V3 df = { std::fabs(history.x - m1.x), std::fabs(history.y - m1.y), std::fabs(history.z - m1.z) };
					const float stray = float(lum(df)), allowed = 2.5f * float(lum(sg)) + 0.05f * float(lum(m1)) + 0.01f;
					if (stray > allowed) n = std::max(1.0f, n * allowed / stray);
					if (v == 1) {   // (B + clamp) the history clamped into this frame's 3 x 3 neighbourhood, mean +- 1.25 sigma (TAA's variance clip: what moved is cut at once)
						const V3 lo = m1 - sg * 1.25f, hi = m1 + sg * 1.25f;
						history = V3(std::min(std::max(history.x, lo.x), hi.x), std::min(std::max(history.y, lo.y), hi.y), std::min(std::max(history.z, lo.z), hi.z));
					}
					signal = history + (signal - history) * (1.0f / n);
				}
				accB[v][p] = signal; nB[p] = n;
			}
			st.hist = accB[v]; st.n = nB; st.ok = okB;
			st.t = track;
			st.out[f].assign(F * F, -1);
		}
		lastCam = cam;
		outOld[f].assign(F * F, -1); outA[f].assign(F * F, -1); outRef[f].assign(F * F, -1); outIdeal[f].assign(F * F, -1);
		std::vector<V3> refColour(F * F, V3(0, 0, 0));
		for (int y = 0; y < F; y++) for (int x = 0; x < F; x++) {
			const int p = y * F + x;
			V3 ref = { 0, 0, 0 }; int nr = 0;
			for (int k = 0; k < 4; k++) {
				const Sample s = sampleAt(w, cam, (x + 0.25f + 0.5f * (k & 1)) / F, (y + 0.25f + 0.5f * (k >> 1)) / F);
				if (s.z > 0) { ref = ref + s.colour; nr++; }
			}
			if (nr < 4) continue;   // (the mirror's own outline: left out)
			refColour[p] = ref * 0.25f;
			outRef[f][p] = lum(ref * 0.25f);
		}
		// the best a half-resolution image can be: each grid texel the mean of its 4 output pixels' references (16 rays), upsampled as the composite does
		std::vector<V3> ideal(G * G, V3(0, 0, 0)); std::vector<int> okI(G * G, 0);
		for (int y = 0; y < G; y++) for (int x = 0; x < G; x++) {
			V3 s = { 0, 0, 0 }; int n = 0;
			for (int k = 0; k < 4; k++) { const int p = (2 * y + (k >> 1)) * F + 2 * x + (k & 1); if (outRef[f][p] >= 0) { s = s + refColour[p]; n++; } }
			if (n == 4) { ideal[y * G + x] = s * 0.25f; okI[y * G + x] = 1; }
		}
		for (int y = 0; y < F; y++) for (int x = 0; x < F; x++) {
			const int p = y * F + x;
			if (outRef[f][p] < 0) continue;
			const Sample a = sampleAt(w, cam, (x + 0.5f) / F, (y + 0.5f) / F);
			outA[f][p] = a.z > 0 ? lum(a.colour) : -1;
			const V3 o = upsample(gc, okC, x, y), id = upsample(ideal, okI, x, y);
			outOld[f][p] = o.x >= 0 ? lum(o) : -1; outIdeal[f][p] = id.x >= 0 ? lum(id) : -1;
			for (int v = 0; v < 2; v++) { const V3 b = upsample(accB[v], okB, x, y); bs[v].out[f][p] = b.x >= 0 ? lum(b) : -1; }
		}
		if (f == 23 || f == 47) {   // a 2 x 2 sheet: reference | old / A | B (history 8)
			std::vector<V3> sheet(size_t(F) * 2 * F * 2, V3(0, 0, 0));
			const std::vector<double>* srcs[4] = { &outRef[f], &outOld[f], &outA[f], &bs[1].out[f] };
			for (int k = 0; k < 4; k++) for (int y = 0; y < F; y++) for (int x = 0; x < F; x++) {
				const double l = (*srcs[k])[y * F + x];
				const float g = l < 0 ? 0.02f : float(l);
				sheet[size_t(y + (k >> 1) * F) * (2 * F) + x + (k & 1) * F] = V3(g, g, g);
			}
			dumpPfmRgb(f == 23 ? "mirroraa_still_ref_old_A_B" : "mirroraa_moving_ref_old_A_B", sheet, 2 * F, 2 * F);
		}
	}
	// edges: RMS on the reference's edge pixels at frame 23 vs the 4-ray reference (full) and vs the ideal half-resolution image (half: what
	// is left once the half resolution's own softness is taken out: the stair-steps); shimmer over frames 16-23; mean error still / moving
	struct M { double edges = 0, edgesHalf = 0, shimmer = 0, still = 0, moving = 0; };
	auto measure = [&](const std::vector<std::vector<double>>& o) {
		M m; int ne = 0, ns = 0, nst = 0, nm = 0;
		const auto& R = outRef[23];
		const auto& I = outIdeal[23];
		for (int y = 1; y < F - 1; y++) for (int x = 1; x < F - 1; x++) {
			const int p = y * F + x;
			if (R[p] < 0 || o[23][p] < 0 || I[p] < 0) continue;
			const double e = o[23][p] - R[p], eh = o[23][p] - I[p];
			nst++; m.still += std::fabs(e);
			if (R[p + 1] >= 0 && R[p - 1] >= 0 && R[p + F] >= 0 && R[p - F] >= 0 && std::max(std::fabs(R[p + 1] - R[p - 1]), std::fabs(R[p + F] - R[p - F])) > 0.08) {
				m.edges += e * e; m.edgesHalf += eh * eh; ne++; }
			double s1 = 0, s2 = 0; int k = 0;
			for (int f = 16; f < 24; f++) if (o[f][p] >= 0) { s1 += o[f][p]; s2 += o[f][p] * o[f][p]; k++; }
			if (k == 8) { m.shimmer += std::sqrt(std::max(s2 / 8 - (s1 / 8) * (s1 / 8), 0.0)); ns++; }
		}
		for (int f = 24; f < frames; f++) for (int p = 0; p < F * F; p++) if (outRef[f][p] >= 0 && o[f][p] >= 0) { m.moving += std::fabs(o[f][p] - outRef[f][p]); nm++; }
		m.edges = ne ? std::sqrt(m.edges / ne) : 0; m.edgesHalf = ne ? std::sqrt(m.edgesHalf / ne) : 0; m.shimmer = ns ? m.shimmer / ns : 0;
		m.still = nst ? m.still / nst : 0; m.moving = nm ? m.moving / nm : 0;
		return m;
	};
	const M mo = measure(outOld), ma = measure(outA), mb = measure(bs[0].out), mb16 = measure(bs[1].out), mi = measure(outIdeal);
	say("  vs a reference of 4 rays an output pixel (luminance; %d x %d output, %d x %d grid); edges: RMS on the reference's edge pixels, vs it and "
		"vs the ideal half-resolution image (16 rays a grid texel, upsampled: what is left is stair-steps):\n", F, F, G, G);
	auto line = [&](const char* name, const M& m) {
		say("    %-46s edges %.4f (vs the ideal half-res %.4f), shimmer %.5f, mean error still %.4f, moving %.4f (motion adds %+.4f)\n", name, m.edges,
			m.edgesHalf, m.shimmer, m.still, m.moving, m.moving - m.still);
	};
	line("old: a centred ray a grid texel (test 12)", mo);
	line("A: a ray an output pixel (4x the rays)", ma);
	line("B: MirrorJitter, history 8 (the denoiser as is)", mb);
	line("B + clamp: history 8, variance clip 1.25", mb16);
	line("(the ideal half-res image itself)", mi);
	for (int v = 0; v < 2; v++) { double s = 0; int n = 0; for (int p = 0; p < G * G; p++) if (bs[v].ok[p]) { s += bs[v].n[p]; n++; } say("    (%s: mean history length at the end %.1f frames)\n", v ? "B + clamp" : "B", n ? s / n : 0.0); }
	bool ok = mb16.edgesHalf < 0.8 * mo.edgesHalf && mb16.shimmer < 0.002 && mb16.moving - mb16.still < 0.002;

	// Part 1: the helper moves the start as RtMirrorJitter does
	{
		const World w = makeWorld(-300.0f);
		const Camera cam = Camera::lookDir({ 0, -800, 450 }, normalize({ 0, 1, -0.55f }), { 0, 0, 1 }, 1.6f, 1.6f);
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		int checked = 0, bad = 0, likeCentre = 0;
		for (int code : { 1 + 10 + 64 * 50, 1 + 60 + 64 * 5 }) {
			const uint32_t cc = uint32_t(code - 1);
			const float jx = ((cc & 63) + 0.5f) / 64.0f - 0.5f, jy = (((cc >> 6) & 63) + 0.5f) / 64.0f - 0.5f;
			std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				const V3 d = cam.ray((i + 0.5f + jx) / S, (j + 0.5f + jy) / S);
				if (!(d.z < 0)) continue;
				const V3 vd = normalize(d), r = { vd.x, vd.y, -vd.z };
				octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
				ray[(j * S + i) * 2] = -cam.eye.z / d.z; ray[(j * S + i) * 2 + 1] = 2.02f;   // a mirror (roughness 0.02)
			}
			if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) { releaseWorld(h, ids); return false; }
			ReflConstants k = {};
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.size[2] = 20000.0f; k.size[3] = 0.0f;
			put4(k.sun, sun, 1.0f);
			put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
			put4(k.frame, 1.0f, -float(code), 0.0f, 0.0f);
			h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			std::vector<float> hit;
			const bool read = h.Exchange(w, ids, cam.eye) && h.Read(c.refHit, hit);
			h.EndFrame();
			if (!read) { releaseWorld(h, ids); return false; }
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				const UINT p = j * S + i;
				if (ray[p * 2] <= 0 || hit[p * 4] <= 0) continue;
				const V3 d = cam.ray((i + 0.5f + jx) / S, (j + 0.5f + jy) / S), dc = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
				const V3 vd = normalize(d), r = { vd.x, vd.y, -vd.z };
				const Hit want = trace(w, cam.eye + d * ray[p * 2] + V3(0, 0, 0.5f), r, 1.0f, 20000.0f, 0xEF, false);
				const Hit centre = trace(w, cam.eye + dc * ray[p * 2] + V3(0, 0, 0.5f), r, 1.0f, 20000.0f, 0xEF, false);
				if (!want.hit || !centre.hit || want.object != 2 || centre.object != 2 || std::fabs(want.t - centre.t) < 3.0f) continue;   // (the far wall, where the start's place shows)
				checked++;
				if (std::fabs(hit[p * 4] - want.t) > 1.5f + 0.002f * want.t) bad++;
				if (std::fabs(hit[p * 4] - centre.t) <= 1.5f + 0.002f * centre.t) likeCentre++;
			}
		}
		releaseWorld(h, ids);
		const bool ok1 = checked > 50 && bad == 0;
		say("  the helper's start with the code: %d mirror rays on the far wall checked (start moved at least 3 units' worth), %d off the jittered "
			"start, %d matching the centre's instead -> %s\n", checked, bad, likeCentre, ok1 ? "ok" : "BAD");
		ok = ok && ok1;
	}
	// Part 2: the trace pass's GPU time, one ray a grid texel vs one an output pixel (160 frames each: the helper averages its last 64)
	{
		const World w = makeWorld(-300.0f);
		const Camera cam = Camera::lookDir({ 0, -800, 450 }, normalize({ 0, 1, -0.55f }), { 0, 0, 1 }, 1.6f, 1.6f);
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		double ms[2] = { 0, 0 }; int rays[2] = { 0, 0 };
		for (int mode = 0; mode < 2; mode++) {
			std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				if (mode == 0 && ((i & 1) || (j & 1))) continue;   // one ray per 2 x 2 (a grid texel)
				const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
				if (!(d.z < 0)) continue;
				const V3 vd = normalize(d), r = { vd.x, vd.y, -vd.z };
				octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
				ray[(j * S + i) * 2] = -cam.eye.z / d.z; ray[(j * S + i) * 2 + 1] = 2.02f; rays[mode]++;
			}
			if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) { releaseWorld(h, ids); return false; }
			for (int fr = 0; fr < 160; fr++) {
				ReflConstants k = {};
				cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
				k.size[2] = 20000.0f; k.size[3] = 1.0f;   // (with the sun's shadow ray, as by day)
				put4(k.sun, sun, 1.0f);
				put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
				h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
				if (!h.Exchange(w, ids, cam.eye)) { releaseWorld(h, ids); return false; }
				h.EndFrame();
			}
			if (h.Finish()) ms[mode] = h.shared->traceUsAvg / 1000.0;
		}
		releaseWorld(h, ids);
		say("  the trace pass on this GPU: %d rays (one a grid texel) %.3f ms, %d rays (one an output pixel: option A) %.3f ms -> A costs %.2f x the "
			"mirror rays' trace time\n", rays[0], ms[0], rays[1], ms[1], ms[0] > 0 ? ms[1] / ms[0] : 0.0);
	}
	say("  (pictures: out\\mirroraa_still_ref_old_A_B.pfm, mirroraa_moving_ref_old_A_B.pfm: reference | old, A | B) -> %s\n", ok ? "ok" : "BAD");
	return ok;
}
