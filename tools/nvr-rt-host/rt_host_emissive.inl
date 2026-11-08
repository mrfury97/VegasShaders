// NvrRtHost, PTEmissive (2026-11-07 ptaaa-emissive; included by rt_host.cpp after rt_host_pt.inl): glowing triangles as lights in the path
// tracer (RtPathTrace.comp EmissiveSample / RestirDirect, RtEmissiveBuild.comp, the game's src/core/RtEmissiveRow.h). README.md has the table.

#include "../../src/core/RtEmissiveRow.h"

static int gEmissiveShader = -1;   // RtEmissiveBuild.spv

// ==============================================================================================================================
// Scene: a dark room (no sky, no sun) with a glowing sign: a 600 x 300 panel of 8 x 8 quads whose glow map (atlas slot 200) lights some quad
// rows (red, a white row in 2-quad blocks, blue, amber: 40 of its 128 triangles), a small glowing box (slot 201) and a block that shadows the
// floor. Everything but the floor (albedo 0.5) has albedo 0, so the floor's picture is EXACTLY albedo x its irradiance from the glow (one
// bounce: the emitters' own light, no inter-reflection), which the CPU integrates (6 x 6 points a triangle, the shader's sRGB decode of the
// stored bytes, shadow rays against the block / box / panel, the shader's lift above the floor). Optionally 6 lamps (the game's falloff).
// Runs, 64 frames each (A0 / As 256):
//   A0  no lamps, PTEmissive OFF: the glow only through bounce rays (brute force), 4 paths a frame;
//   B0  no lamps, PTEmissive ON (one emissive sample a hit, MIS against the bounce ray), 4 paths a frame;
//   B   lamps + PTEmissive ON, plain one-lamp pick, 1 path; C lamps + PTEmissive in ReSTIR DI (PTRestirDI), 1 path;
//   C'  lamps + ReSTIR DI with PTEmissive OFF (the glow only through bounce rays, as before), 1 path;
//   As / Bs  the floor glossy (GGX, roughness 2/7): A0 / B0 again with PTSpecular.
// Checked: B0, B and C within 2.5 % of the exact mean, A0 (256 frames) and B0 / A0 within max(2.5 %, 3 standard errors) (MIS: none counted twice
// or lost), per-frame noise B0 < 0.5 x A0 and C < 0.7 x C' (the glow as a light, in ReSTIR); Bs / As within max(3 %, 3 errors); the list
// usable (both instances), no NaN. Printed: the noise ratios and the pass's GPU time with and without PTEmissive.
static bool sceneEmissive(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -1500, -1500, 0 }, { 1500, -1500, 0 }, { 1500, 1500, 0 }, { -1500, 1500, 0 });
	floor.uv.clear();
	const int N = 8;
	Mesh panel;   // in the plane y = 400, facing -y; uv (i / N, j / N) over the whole panel
	for (int j = 0; j <= N; j++) for (int i = 0; i <= N; i++) {
		panel.pos.push_back({ -300.0f + 600.0f * i / N, 400.0f, 40.0f + 300.0f * j / N });
		panel.uv.push_back(float(i) / N); panel.uv.push_back(float(j) / N);
	}
	for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
		const uint16_t a = uint16_t(j * (N + 1) + i), b = uint16_t(a + 1), d = uint16_t(a + N + 1), e = uint16_t(d + 1);
		for (uint16_t k : { a, b, e, a, e, d }) panel.idx.push_back(k);
	}
	Mesh glowBox; glowBox.box({ 430, 80, 110 }, { 480, 130, 160 });
	Mesh block; block.box({ -150, 150, 0 }, { 150, 200, 120 });
	block.uv.clear();
	w.meshes = { floor, panel, glowBox, block };
	{ Object o; o.mesh = 0; o.albedo = { 0.5f, 0.5f, 0.5f }; w.objects.push_back(o); }
	{ Object o; o.mesh = 1; o.glow = true; o.textured = true; o.alphaSlot = 200; o.albedo = { 0, 0, 0 }; w.objects.push_back(o); }
	{ Object o; o.mesh = 2; o.glow = true; o.textured = true; o.alphaSlot = 201; o.albedo = { 0, 0, 0 }; w.objects.push_back(o); }
	{ Object o; o.mesh = 3; o.albedo = { 0, 0, 0 }; w.objects.push_back(o); }
	// the glow maps (stored sRGB bytes, as the game's): the panel's quad rows j (texel rows 8 j .. 8 j + 7)
	auto panelTexel = [](int tx, int ty) -> V3 {
		const int row = ty / 8, col = tx / 8;
		if (row == 1) return { 1.0f, 0.25f, 0.1f };
		if (row == 2) return (col / 2) % 2 == 0 ? V3(0.9f, 0.9f, 0.9f) : V3(0, 0, 0);
		if (row == 4) return { 0.1f, 0.6f, 1.0f };
		if (row == 6) return { 1.0f, 0.8f, 0.2f };
		return { 0, 0, 0 };
	};
	const V3 boxGlow = { 1.0f, 0.85f, 0.6f };
	{
		std::vector<float> t(64 * 64 * 4), u(64 * 64 * 4);
		for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
			const V3 p = panelTexel(x, y);
			float* a = &t[(y * 64 + x) * 4]; a[0] = p.x; a[1] = p.y; a[2] = p.z; a[3] = 1.0f;
			float* b = &u[(y * 64 + x) * 4]; b[0] = boxGlow.x; b[1] = boxGlow.y; b[2] = boxGlow.z; b[3] = 1.0f;
		}
		if (!h.Upload(gM.albedoAtlas, t, (200 % 32) * 64, (200 / 32) * 64, 64, 64) || !h.Upload(gM.albedoAtlas, u, (201 % 32) * 64, (201 / 32) * 64, 64, 64)) return false;
	}
	auto decode = [](float v) { const float q = std::floor(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f) / 255.0f; return q <= 0.04045f ? q / 12.92f : std::pow((q + 0.055f) / 1.055f, 2.4f); };
	auto lin = [&](V3 s) { return V3(decode(s.x), decode(s.y), decode(s.z)); };
	const float glowScale = 3.0f;   // PTGlow
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;

	struct L { V3 pos; float reach; V3 colour; };
	const std::vector<L> lamps = { { { -500, -300, 150 }, 700, { 0.35f, 0.3f, 0.25f } }, { { 300, -150, 120 }, 500, { 0.2f, 0.3f, 0.4f } },
		{ { 0, 100, 60 }, 400, { 0.3f, 0.3f, 0.3f } }, { { 600, 300, 200 }, 600, { 0.25f, 0.2f, 0.3f } }, { { -700, 200, 100 }, 500, { 0.3f, 0.25f, 0.2f } },
		{ { 100, -600, 250 }, 800, { 0.2f, 0.25f, 0.2f } } };

	// ---- the CPU's exact light at a floor point (scene = world - eye)
	World occPanel, occBox;   // the occluders of the panel's points (block + box) and of the box's (block + panel); the floor never blocks
	occPanel.meshes = w.meshes; occBox.meshes = w.meshes;
	occPanel.objects = { w.objects[2], w.objects[3] }; occBox.objects = { w.objects[1], w.objects[3] };
	for (Object& o : occPanel.objects) { o.glow = false; o.textured = false; }
	for (Object& o : occBox.objects) { o.glow = false; o.textured = false; }
	const int K = 6;
	auto glowAt = [&](const Camera& cam, V3 at) {   // at: scene space on the floor; the light from the shader's lifted point
		const V3 nrm(0, 0, 1);
		const V3 lift = at + nrm * (1.0f + 0.002f * length(at)) + cam.eye;   // (world)
		double e[3] = {};
		for (int obj : { 1, 2 }) {
			const Mesh& m = w.meshes[w.objects[obj].mesh];
			for (size_t k = 0; k + 2 < m.idx.size(); k += 3) {
				const V3 a = m.pos[m.idx[k]], b = m.pos[m.idx[k + 1]], cc = m.pos[m.idx[k + 2]];
				const V3 x = cross(b - a, cc - a);
				const float area = 0.5f * length(x);
				const V3 ny = x * (1.0f / (2.0f * area));
				if (obj == 2 && dot(ny, lift - a) <= 0.0f) continue;   // (the box is convex: a face turned away is hidden by the box itself; its normals point out)
				const float* ua = &m.uv[2 * m.idx[k]]; const float* ub = &m.uv[2 * m.idx[k + 1]]; const float* uc = &m.uv[2 * m.idx[k + 2]];
				for (int s = 0; s < K * K; s++) {
					const float u = (float(s % K) + 0.5f) / K, v = (float(s / K) + 0.5f) / K, su = std::sqrt(u);
					const float bx = su * (1 - v), by = su * v;
					const V3 y = a * (1 - bx - by) + b * bx + cc * by;
					const float tu = ua[0] * (1 - bx - by) + ub[0] * bx + uc[0] * by, tv = ua[1] * (1 - bx - by) + ub[1] * bx + uc[1] * by;
					V3 le;
					if (obj == 1) {
						const int tx = std::min(std::max(int((tu - std::floor(tu)) * 64.0f), 0), 63), ty = std::min(std::max(int((tv - std::floor(tv)) * 64.0f), 0), 63);
						le = lin(panelTexel(tx, ty));
					}
					else le = lin(boxGlow);
					if (le.x + le.y + le.z <= 0.0f) continue;
					const V3 d = y - lift;
					const float d2 = dot(d, d), dl = std::sqrt(d2);
					const float cx = d.z / dl, cy = std::fabs(dot(ny, d)) / dl;
					if (cx <= 0.0f || cy <= 1e-6f) continue;
					const Hit hh = trace(obj == 1 ? occPanel : occBox, lift, d * (1.0f / dl), 1e-3f, std::max(dl * 0.998f - 0.02f, 0.0f), 0xFF, true);
					if (hh.hit) continue;
					const float g = cx * cy / (3.14159265f * d2) * area / (K * K) * glowScale;
					e[0] += le.x * g; e[1] += le.y * g; e[2] += le.z * g;
				}
			}
		}
		return V3(float(e[0]), float(e[1]), float(e[2]));
	};
	auto lampsAt = [&](const Camera& cam, V3 at) {   // as rt_host_pt.inl exactAt (the game's falloff, the fixture gap)
		const V3 nrm(0, 0, 1);
		const V3 lift = at + nrm * (1.0f + 0.002f * length(at));
		V3 e(0, 0, 0);
		for (const L& l : lamps) {
			const V3 lp = l.pos - cam.eye, d = lp - at;
			const float dist = length(d);
			if (dist >= l.reach || dist < 1e-3f) continue;
			const float x = dist / l.reach, fall = std::pow(std::max(1 - x * x, 0.0f), 2.2f), cs = std::max(d.z / dist, 0.0f);
			if (!(fall * cs > 0)) continue;
			const V3 ld = lp - lift;
			const float ldist = length(ld);
			if (trace(w, cam.eye + lift, ld * (1.0f / ldist), 0.0f, std::max(ldist - 16.0f, 0.0f), 0xFF, true).hit) continue;
			e = e + l.colour * (fall * cs);
		}
		return e;
	};
	const float albedo = 0.5f;
	const Camera cam = Camera::lookDir({ 0, -1000, 800 }, { 0, 1100, -800 }, { 0, 0, 1 }, 1.7f, 1.7f);
	// reference pixels: every 4th in x and y, on the floor, centre + two diagonal points within 5 % of each other (off shadow edges)
	struct Ref { bool use = false; V3 glow, lamps; };
	std::vector<Ref> ref(size_t(S) * S);
	int refCount = 0;
	for (UINT j = 2; j < S; j += 4) for (UINT i = 2; i < S; i += 4) {
		V3 gs(0, 0, 0), ls(0, 0, 0); float lo = 1e30f, hi = -1e30f; bool ok = true;
		for (int s = 0; s < 3 && ok; s++) {
			const float ox = s == 0 ? 0.5f : s == 1 ? 0.2f : 0.8f, oy = s == 0 ? 0.5f : s == 1 ? 0.2f : 0.8f;
			const V3 d = cam.ray((i + ox) / S, (j + oy) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!hh.hit || hh.object != 0) { ok = false; break; }
			const V3 g = glowAt(cam, d * hh.t), l = lampsAt(cam, d * hh.t);
			gs = gs + g; ls = ls + l;
			const float y = 0.2126f * g.x + 0.7152f * g.y + 0.0722f * g.z;
			lo = std::min(lo, y); hi = std::max(hi, y);
		}
		if (!ok || !(hi > 1e-3f) || hi - lo > 0.05f * hi) continue;
		Ref& r = ref[size_t(j) * S + i];
		r.use = true; r.glow = gs * (albedo / 3.0f); r.lamps = ls * (albedo / 3.0f);
		refCount++;
	}

	// ---- runs: the lamp image (lamps, the frame row from the game's RtRestirRow::Build, the emissive row from the game's RtEmissiveRow::Build),
	// the build pass, the trace; the display read back each frame
	struct Run { std::vector<std::vector<float>> frames; double traceMs = 0; int listed = 0; };
	auto run = [&](bool withLamps, bool emissive, bool restir, bool specular, int paths, int frames, Run& out) {
		RtRestirRow::State state;
		RtRestirRow::Keys keys;
		keys.radius = 6.0f;
		RtEmissiveRow::State emState;
		out.frames.clear();
		Object& floorObj = w.objects[0];
		floorObj.textured = specular; floorObj.alphaSlot = specular ? 202 : -1; floorObj.rough = specular ? 2.0f / 7.0f : -1.0f; floorObj.metal = specular ? 0.0f : -1.0f;
		if (specular) w.meshes[0].uv = { 0, 0, 1, 0, 1, 1, 0, 1 };
		for (int f = 0; f < frames; f++) {
			std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
			std::vector<float> list(lamps.size() * 8);
			const int n = withLamps ? int(lamps.size()) : 0;
			for (int k = 0; k < n; k++) {
				const L& l = lamps[k];
				const V3 q = l.pos - cam.eye;
				const float v[8] = { q.x, q.y, q.z, l.reach, l.colour.x, l.colour.y, l.colour.z, 0.0f };
				std::memcpy(&image[size_t(k) * 8], v, sizeof(v)); std::memcpy(&list[size_t(k) * 8], v, sizeof(v));
			}
			PathConstants k = PathConstants();
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float((uint32_t(f + 1) * 7919u + (emissive ? 17u : 0u) + (restir ? 101u : 0u)) & 0xFFFFFu);
			const uint32_t flags = 1024u | 2048u | 4u | (restir ? 262144u : 0u) | (specular ? 128u | 1048576u : 0u);
			put4(k.accum, 0.0f, 1.0f, float(paths), float(flags));
			put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0, 0, 0, 0); put4(k.skyHorizon, 0, 0, 0, glowScale);
			put4(k.misc, float(n), 0.0f, 60000.0f, 0.0f);
			k.rect[3] = 1.0f;
			const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
			const float eye[3] = { 0, 0, 0 };
			float* frameRow = &image[size_t(kLightTexels) * kFrameRowIndex * 2];
			RtRestirRow::Build(state, frameRow, image.data(), list.data(), n, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
			// the emissive instances as RtPathTrace.cpp hands them over (records = the objects in order: the host stages every geometry)
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
			const unsigned emBase4096 = 1024u;   // (16 MB into the host's 64 MB scratch store: past the reservoirs and the cache scene's table)
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
			out.frames.push_back(std::move(disp));
			out.listed = count;
		}
		if (h.Finish()) out.traceMs = h.shared->traceUsAvg / 1000.0;   // (the helper's average over its last 64 frames: this run's)
		for (Object& o : w.objects) o.emissive = 0;
		floorObj.textured = false; floorObj.alphaSlot = -1; floorObj.rough = floorObj.metal = -1.0f;
		w.meshes[0].uv.clear();
		return true;
	};
	auto luma = [](const float* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; };
	struct Stats { double ratio = 0, noise = 0, se = 0; bool finite = true; };   // se: the ratio's standard error (frames' spread)
	auto stats = [&](const Run& r, bool withLamps) {
		Stats s; double sumMean = 0, sumExact = 0, var = 0; int nPix = 0;
		const size_t F = r.frames.size();
		for (const auto& fr : r.frames) for (float v : fr) if (!std::isfinite(v)) s.finite = false;
		for (size_t p = 0; p < size_t(S) * S; p++) {
			if (!ref[p].use) continue;
			const V3 e = withLamps ? ref[p].glow + ref[p].lamps : ref[p].glow;
			double m = 0, m2 = 0;
			for (size_t f = 0; f < F; f++) { const double y = luma(&r.frames[f][p * 4]); m += y; m2 += y * y; }
			m /= F; m2 /= F;
			sumMean += m; sumExact += 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z;
			var += std::max(m2 - m * m, 0.0); nPix++;
		}
		s.ratio = sumExact > 0 ? sumMean / sumExact : 0; s.noise = nPix ? std::sqrt(var / nPix) : 0;
		s.se = sumExact > 0 && F > 1 ? std::sqrt(var / double(F - 1)) / sumExact : 0;
		return s;
	};
	// frames: 64 a run; the bounce-only brute force (A0, As) 256 (its mean's standard error is ~3 % at 64: heavy-tailed, rare hits on the sign)
	const int F = std::getenv("NVR_RT_HOST_EM_FRAMES") ? std::max(8, std::atoi(std::getenv("NVR_RT_HOST_EM_FRAMES"))) : 64;   // (a test knob)
	const int FB = 4 * F;
	Run a0, b0, b, cr, cOff, as, bs;
	if (!run(false, false, false, false, 4, FB, a0) || !run(false, true, false, false, 4, F, b0) || !run(true, true, false, false, 1, F, b) ||
		!run(true, true, true, false, 1, F, cr) || !run(true, false, true, false, 1, F, cOff) || !run(false, false, false, true, 4, FB, as) ||
		!run(false, true, false, true, 4, F, bs)) return false;
	const Stats sA0 = stats(a0, false), sB0 = stats(b0, false), sB = stats(b, true), sC = stats(cr, true), sCo = stats(cOff, true), sAs = stats(as, false), sBs = stats(bs, false);
	say("  %d reference floor pixels (exact glow + lamps, CPU); %d emissive instances listed\n", refCount, b0.listed);
	say("  glow only, 4 paths a frame: bounce rays only (A0, %d frames) mean x%.4f +- %.4f noise %.4f; PTEmissive (B0, %d frames) mean x%.4f +- %.4f noise %.4f = %.2f x A0; B0 / A0 means %.4f\n",
		FB, sA0.ratio, sA0.se, sA0.noise, F, sB0.ratio, sB0.se, sB0.noise, sB0.noise / sA0.noise, sB0.ratio / sA0.ratio);
	say("    (glossy: A0s +- %.4f, B0s +- %.4f; lamps: B +- %.4f, C +- %.4f, C' +- %.4f: one standard error of the frames' mean)\n", sAs.se, sBs.se, sB.se, sC.se, sCo.se);
	say("  lamps + glow, 1 path a frame, %d frames: plain + PTEmissive (B) mean x%.4f noise %.4f; ReSTIR DI + PTEmissive (C) mean x%.4f noise %.4f; "
		"ReSTIR DI, glow by bounce rays only (C') mean x%.4f noise %.4f; C = %.2f x B, %.2f x C'\n", F, sB.ratio, sB.noise, sC.ratio, sC.noise, sCo.ratio, sCo.noise,
		sC.noise / sB.noise, sC.noise / sCo.noise);
	say("  glossy floor (GGX 2/7), glow only: A0s mean x%.4f noise %.4f, B0s mean x%.4f noise %.4f; Bs / As means %.4f\n", sAs.ratio, sAs.noise, sBs.ratio, sBs.noise,
		sBs.ratio / sAs.ratio);
	say("  the pass's GPU time (helper, avg of the run's last 64 frames, this iGPU, 128 x 128, shared with other work): A0 %.3f ms, B0 %.3f ms (4 paths); "
		"B %.3f, C %.3f, C' %.3f ms (1 path)\n", a0.traceMs, b0.traceMs, b.traceMs, cr.traceMs, cOff.traceMs);
	const bool finite = sA0.finite && sB0.finite && sB.finite && sC.finite && sCo.finite && sAs.finite && sBs.finite;
	// the brute force's and the ratios' limits: 0.025 or 3 standard errors, whichever is wider (B0's own error is ~0.3 %)
	const double limA = std::max(0.025, 3.0 * sA0.se), limRatio = std::max(0.025, 3.0 * std::sqrt(sA0.se * sA0.se + sB0.se * sB0.se)),
		limGloss = std::max(0.03, 3.0 * std::sqrt(sAs.se * sAs.se + sBs.se * sBs.se));
	const bool pass = finite && refCount > 200 && b0.listed == 2 && std::fabs(sA0.ratio - 1.0) < limA && std::fabs(sB0.ratio - 1.0) < 0.025 &&
		std::fabs(sB0.ratio / sA0.ratio - 1.0) < limRatio && sB0.noise < 0.5 * sA0.noise && std::fabs(sB.ratio - 1.0) < 0.025 && std::fabs(sC.ratio - 1.0) < 0.025 &&
		sC.noise < 0.7 * sCo.noise && std::fabs(sBs.ratio / sAs.ratio - 1.0) < limGloss;
	say("  limits: A0 %.3f, B0 / A0 %.3f, Bs / As %.3f\n", limA, limRatio, limGloss);
	say("  -> %s%s\n", pass ? "ok" : "BAD", finite ? "" : " (NaN / inf in the output)");
	if (!pass) {
		std::vector<float> mean(size_t(S) * S * 4, 0.0f), ex(size_t(S) * S * 4, 0.0f);
		for (const auto& fr : b0.frames) for (size_t i = 0; i < mean.size(); i++) mean[i] += fr[i] / float(b0.frames.size());
		for (size_t p = 0; p < size_t(S) * S; p++) { ex[p * 4] = ref[p].glow.x; ex[p * 4 + 3] = ref[p].use ? 1.0f : 0.0f; }
		dumpPfm("emissive_b0_mean_r", mean, S, S, 4, 0); dumpPfm("emissive_exact_r", ex, S, S, 4, 0); dumpPfm("emissive_exact_use", ex, S, S, 4, 3);
	}
	releaseWorld(h, ids);
	return pass;
}
