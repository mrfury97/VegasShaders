// ==============================================================================================================================
// (2026-11-07 perf-reflfix) Scene `reflfix`: the tester's mirror wall (RTX 4090, test 9: spheres in the RT test room's mirror came out
// FACETED, the chrome sphere a plain white ball and the gold one banded orange, neither reflecting anything). docs/next/RT-PATH-TRACING.md
// "perf-reflfix". A UV sphere (48 x 24, 2,208 triangles, its vertices shared as a game mesh's are, a texcoord seam) on a mirror floor:
//   A  smooth normals, RT reflections (ReflectionsRT): the hit normal the floor's reflection rays bring back from the sphere vs the
//      sphere's exact normal at the CPU's hit point. Before (the flat triangle normal, what the shader wrote until now): the CPU's
//      triangle normal against the same truth; after: the helper's vertex normals (RtNormals.comp) interpolated (RtHit.glsl
//      rtHitSmoothNormal).
//   B  smooth normals, the path tracer (RtPathTrace PTDebugView 2: the first hit's normal; a camera ray's traced hit, no G-buffer).
//   C  MetalBounce (ReflectionsRT, Pass up.w): the sphere is CHROME (a textured record with the material rule chrome: metal 1, roughness
//      1 / 7), a red box and a green box stand beside it. The floor's rays that meet the sphere send ONE mirror ray on from it: its
//      direction about the sphere's (smooth) normal, its distance and what it meets (colour) vs the CPU's mirror ray about the exact
//      normal; the metal's Fresnel tint vs Schlick. Before (MetalBounce 0): the chrome's own colour on every such pixel, no red / green.
// Not here: Resolve (ReflectionsRTResolve.frag: the screen at the bounced hit, the facing test) and RayGen (RaysOnly skips the march)
// are D3D9-side passes the host does not run (README "Still not covered").

struct ReflFixCtx { int bounce = -1; };
static ReflFixCtx gRF;
static bool registerReflFix(Host& h) {
	gRF.bounce = h.AddImage("refl bounce", S, S, NVR_EXT_SHARED_RGBA16F);
	return gRF.bounce >= 0;
}

// A UV sphere: (seg + 1) x (rings + 1) vertices (the u = 1 column repeats u = 0's positions: a texcoord seam, as game meshes have), the
// pole rows' degenerate halves left out; counter-clockwise from outside
static Mesh uvSphere(V3 c, float r, int seg, int rings) {
	Mesh m;
	for (int j = 0; j <= rings; j++) for (int i = 0; i <= seg; i++) {
		const float th = 3.14159265f * j / rings, ph = 6.2831853f * i / seg;
		m.pos.push_back(c + V3{ std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th) } * r);
		m.uv.push_back(float(i) / seg); m.uv.push_back(float(j) / rings);
	}
	auto at = [&](int i, int j) { return uint16_t(j * (seg + 1) + i); };
	for (int j = 0; j < rings; j++) for (int i = 0; i < seg; i++) {
		const uint16_t a = at(i, j), b = at(i + 1, j), cc = at(i + 1, j + 1), d = at(i, j + 1);
		if (j != 0) { m.idx.push_back(a); m.idx.push_back(d); m.idx.push_back(b); }
		if (j != rings - 1) { m.idx.push_back(b); m.idx.push_back(d); m.idx.push_back(cc); }
	}
	return m;
}
static float angleDeg(V3 a, V3 b) { return std::acos(std::min(1.0f, std::max(-1.0f, dot(normalize(a), normalize(b))))) * 57.29578f; }

static bool sceneReflFix(Ctx& c) {
	Host& h = *c.host;
	bool allOk = true;
	const V3 centre = { 0, 500, 260 };
	const float radius = 250.0f;
	const Camera cam = Camera::lookDir({ 0, -1600, 700 }, normalize({ 0, 1, -0.45f }), { 0, 0, 1 }, 1.3f, 1.3f);
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh ball = uvSphere(centre, radius, 48, 24);
	Mesh block; block.box({ -150, -150, 0 }, { 150, 150, 300 });
	say("  sphere: %zu vertices, %zu triangles (shared vertices, a texcoord seam)\n", ball.pos.size(), ball.idx.size() / 3);

	// RayGen's inputs: the floor's mirror rays (as sceneMirror)
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	auto rayGen = [&](const World& w) {
		std::fill(dir.begin(), dir.end(), 0.0f); std::fill(ray.begin(), ray.end(), 0.0f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!hh.hit || hh.object != 0) continue;
			const V3 v = normalize(d), r = v - hh.n * (2 * dot(v, hh.n));
			octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
			ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;   // view z, roughness 0 + 2 (cast)
		}
	};
	// the CPU's reflection ray of a floor pixel (the shader's start: off the surface along the half vector), and its first hit
	auto cpuRay = [&](const World& w, UINT i, UINT j, V3& start, V3& rdir, Hit& rh) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit fh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!fh.hit || fh.object != 0) return false;
		const V3 surface = d * fh.t;
		const V3 v = normalize(d);
		rdir = v - fh.n * (2 * dot(v, fh.n));
		const V3 half = rdir - normalize(surface);
		start = surface + half * ((0.5f + fh.t * 0.002f) / length(half));
		rh = trace(w, cam.eye + start, rdir, 1.0f + fh.t * 0.0015f, 10000.0f, 0xEF, false);
		return true;
	};
	auto reflPass = [&](const World& w, float metalBounce, std::vector<float>& hit, std::vector<float>& shade, std::vector<float>& bounce) {
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		rayGen(w);
		if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 10000.0f; k.size[3] = 0.0f;   // Distance; no sun / lamp shadow rays (the shade's visibility stays 1)
		k.forward[3] = 0.0f;
		k.up[3] = metalBounce;                    // MetalBounce
		put4(k.sun, normalize({ 0.3f, -0.3f, 0.9f }), 1.0f);
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		const bool ok = h.Read(c.refHit, hit) && h.Read(c.refShade, shade) && h.Read(gRF.bounce, bounce);
		h.EndFrame();
		releaseWorld(h, ids);
		return ok;
	};

	// ---- A. smooth normals in RT reflections
	{
		World w; w.meshes = { floor, ball };
		Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f }; f.id = 1;
		Object s; s.mesh = 1; s.albedo = { 0.8f, 0.3f, 0.2f }; s.id = 2;
		w.objects = { f, s };
		std::vector<float> hit, shade, bounce;
		if (!reflPass(w, 0.0f, hit, shade, bounce)) return false;
		int n = 0, noNormal = 0, shown = 0;
		double sumAfter = 0, sumBefore = 0, maxAfter = 0, maxBefore = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			V3 start, rdir; Hit rh;
			if (!cpuRay(w, i, j, start, rdir, rh) || !rh.hit || rh.object != 1) continue;
			const float* H = &hit[(j * S + i) * 4];
			if (std::fabs(H[0] - rh.t) > 0.002f * rh.t + 1.0f) continue;   // (an outline pixel the GPU's ray passed: not this test's)
			const V3 p = cam.eye + start + rdir * rh.t;
			V3 truth = normalize(p - centre);
			if (dot(truth, rdir) > 0) continue;   // (the inside: never seen from outside)
			if ((int(H[1]) & 4) == 0) { noNormal++; continue; }
			const float after = angleDeg(octDecode(H[2], H[3]), truth), before = angleDeg(rh.n, truth);
			n++; sumAfter += after; sumBefore += before; maxAfter = std::max(maxAfter, (double)after); maxBefore = std::max(maxBefore, (double)before);
			if (after > 3.0f && shown++ < 4) say("    pixel (%u, %u): normal %.2f deg off the sphere's (flat %.2f)\n", i, j, after, before);
		}
		const double mA = n ? sumAfter / n : 99, mB = n ? sumBefore / n : 0;
		const bool ok = n > 150 && noNormal == 0 && mA < 0.5 && mA < mB / 3.0 && maxAfter < 3.0;
		say("  A RT reflections: %d sphere hits; normal vs the exact sphere: mean %.3f deg, max %.2f deg (before, the triangle's: mean %.3f, max %.2f)%s -> %s\n",
			n, mA, maxAfter, mB, maxBefore, noNormal ? " (hits without a normal!)" : "", ok ? "ok" : "BAD");
		allOk = allOk && ok;
		if (!ok) dumpPfm("reflfix_A_hit_z", hit, S, S, 4, 2);
	}

	// ---- B. smooth normals in the path tracer (a camera ray's traced hit, PTDebugView 2)
	{
		World w; w.meshes = { floor, ball };
		Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
		Object s; s.mesh = 1; s.albedo = { 0.5f, 0.5f, 0.5f };
		w.objects = { f, s };
		const Camera pc = Camera::lookDir({ 0, -300, 450 }, normalize(centre - V3{ 0, -300, 450 }), { 0, 0, 1 }, 1.4f, 1.4f);
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, gM.albedoAtlas, gM.albedoAtlas, c.depth, c.normals, c.aux16, c.pano,
			c.aux16, gM.albedoAtlas, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		PathConstants k = {};
		cameraConstants(pc, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 5.0f;
		put4(k.accum, 0.0f, 0.0f, 1.0f, 0.0f);
		put4(k.sunDir, normalize({ 0.3f, -0.3f, 0.9f }), 1.0f); put4(k.sunColor, 1.0f, 1.0f, 1.0f, 0.0f);
		put4(k.skyZenith, 0, 0, 0, 1); put4(k.skyHorizon, 0, 0, 0, 0);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 2.0f);   // PTDebugView 2: the first hit's normal x 0.5 + 0.5
		k.rect[3] = 1.0f;
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, pc.eye)) return false;
		std::vector<float> out;
		const bool read = h.Read(c.ptDisplay, out);
		h.EndFrame();
		releaseWorld(h, ids);
		if (!read) return false;
		// (the path tracer's camera rays are jittered inside the pixel: the GPU's normal is checked against the exact normal at the
		// nearest of 5 x 5 points of the pixel, the flat one likewise; a pixel any of whose points misses the sphere is not checked)
		int n = 0;
		double sumAfter = 0, sumBefore = 0, maxAfter = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const float* g = &out[(j * S + i) * 4];
			const V3 got = { g[0] * 2 - 1, g[1] * 2 - 1, g[2] * 2 - 1 };
			if (dot(got, got) < 0.25f) continue;
			bool all = true;
			float after = 1e9f, before = 1e9f;
			for (int sy = 0; sy < 5 && all; sy++) for (int sx = 0; sx < 5 && all; sx++) {
				const V3 d = pc.ray((i + (sx + 0.5f) / 5.0f) / S, (j + (sy + 0.5f) / 5.0f) / S);
				const Hit hh = trace(w, pc.eye, d, 1e-3f, 1e9f, 0xFF, true);
				if (!hh.hit || hh.object != 1) { all = false; break; }
				const V3 truth = normalize(pc.eye + d * hh.t - centre);
				after = std::min(after, angleDeg(got, truth));
				before = std::min(before, angleDeg(hh.n, truth));
			}
			if (!all) continue;
			n++; sumAfter += after; sumBefore += before; maxAfter = std::max(maxAfter, (double)after);
		}
		const double mA = n ? sumAfter / n : 99, mB = n ? sumBefore / n : 0;
		const bool ok = n > 2000 && mA < 0.6 && mA < mB / 3.0 && maxAfter < 3.0;
		say("  B path tracer (PTDebugView 2): %d sphere pixels; normal vs the exact sphere: mean %.3f deg, max %.2f deg (before, the triangle's: mean %.3f) -> %s\n",
			n, mA, maxAfter, mB, ok ? "ok" : "BAD");
		allOk = allOk && ok;
		if (!ok) dumpPfm("reflfix_B_pt_normal_x", out, S, S, 4, 0);
	}

	// ---- C. MetalBounce: a chrome sphere in the mirror reflects the red and green boxes beside it (one bounce)
	{
		World w; w.meshes = { floor, ball, block };
		Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
		Object s; s.mesh = 1; s.albedo = { 0.96f, 0.96f, 0.97f }; s.textured = true; s.alphaSlot = 5; s.rough = 0.125f; s.metal = 1.0f;   // the material rule chrome
		Object red; red.mesh = 2; red.offset = { -480, 260, 0 }; red.albedo = { 0.8f, 0.1f, 0.1f };
		Object green; green.mesh = 2; green.offset = { 480, 260, 0 }; green.albedo = { 0.1f, 0.7f, 0.2f };
		w.objects = { f, s, red, green };
		std::vector<float> hit, shade, bounce, hit0, shade0, bounce0;
		if (!reflPass(w, 0.5f, hit, shade, bounce) || !reflPass(w, 0.0f, hit0, shade0, bounce0)) return false;
		int sphere = 0, checked = 0, bad = 0, shown = 0, coloured = 0, colouredBefore = 0, misses = 0, notBounced = 0, beforeBounced = 0;
		double tintErr = 0, dirSum = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			V3 start, rdir; Hit rh;
			if (!cpuRay(w, i, j, start, rdir, rh) || !rh.hit || rh.object != 1) continue;
			const float* H = &hit[(j * S + i) * 4]; const float* C = &shade[(j * S + i) * 4]; const float* B = &bounce[(j * S + i) * 4];
			const float* H0 = &hit0[(j * S + i) * 4]; const float* C0 = &shade0[(j * S + i) * 4];
			if (std::fabs(H[0] - rh.t) > 0.002f * rh.t + 1.0f) continue;
			sphere++;
			if (int(H0[1]) & 256) beforeBounced++;
			if (C0[0] > 0.5f && C0[1] < 0.3f || C0[1] > 0.5f && C0[0] < 0.3f || C0[2] > 0.5f && C0[0] < 0.3f) colouredBefore++;
			if (!(int(H[1]) & 256)) { notBounced++; continue; }
			// the CPU's bounce: the mirror direction about the exact normal, from the hit off along the triangle's normal (the shader's offset)
			const V3 p = cam.eye + start + rdir * rh.t;
			const V3 nTrue = normalize(p - centre);
			const V3 d2 = rdir - nTrue * (2 * dot(rdir, nTrue));
			const V3 off = p + rh.n * std::max(1.0f, rh.t * 0.001f);
			const Hit b = trace(w, off, d2, 0.5f, 10000.0f - rh.t, 0xEF, false);
			// stable: the same object for the bounce turned 1 degree either way (the interpolated normal is within a degree of the exact)
			bool stable = true;
			for (int k = 0; k < 4 && stable; k++) {
				const V3 tw = normalize(nTrue + V3{ k == 0 ? 0.02f : k == 1 ? -0.02f : 0.0f, k == 2 ? 0.02f : k == 3 ? -0.02f : 0.0f, 0.0f });
				const V3 dd = rdir - tw * (2 * dot(rdir, tw));
				const Hit bb = trace(w, off, dd, 0.5f, 10000.0f - rh.t, 0xEF, false);
				stable = bb.hit == b.hit && bb.object == b.object;
			}
			if (!stable) continue;
			checked++;
			const V3 gotD = octDecode(H[2], H[3]);
			const float c5 = std::pow(1.0f - std::max(0.0f, dot(nTrue, -rdir)), 5.0f);
			const V3 tint = s.albedo + (V3{ 1, 1, 1 } - s.albedo) * c5;
			tintErr = std::max(tintErr, (double)std::max(std::fabs(B[0] - tint.x), std::max(std::fabs(B[1] - tint.y), std::fabs(B[2] - tint.z))));
			// the direction: within 5 degrees, twice part A's largest normal error (8 near the outline seen from the floor, where the interpolated normal leaves the
			// exact one most); the distance only where the direction is within 0.3 degree (a grazing bounce onto the floor turns a tenth
			// of a degree into tens of units: 4 % then)
			const float dirErr = angleDeg(gotD, d2), grazing = dot(nTrue, -rdir);
			dirSum += dirErr;
			bool ok = dirErr < (grazing < 0.25f ? 8.0f : 5.0f) && std::fabs(B[0] - tint.x) < 0.03f && std::fabs(B[2] - tint.z) < 0.03f;
			if (!b.hit) { misses++; ok = ok && (int(H[1]) & 512) != 0 && B[3] == 0.0f; }
			else {
				const Object& o = w.objects[b.object];
				ok = ok && !(int(H[1]) & 512) && (dirErr > 0.3f || std::fabs(B[3] - b.t) < 0.04f * b.t + 2.0f) && std::fabs(C[0] - o.albedo.x) < 0.01f &&
					std::fabs(C[1] - o.albedo.y) < 0.01f && std::fabs(C[2] - o.albedo.z) < 0.01f;
				if (b.object >= 2) coloured++;
			}
			if (!ok) { bad++; if (shown++ < 5) say("    pixel (%u, %u): bounce dir %.2f deg off, t2 %.1f (want %.1f, object %d), colour (%.2f %.2f %.2f), tint (%.3f %.3f %.3f) want (%.3f %.3f %.3f), flags %d\n",
				i, j, angleDeg(gotD, d2), B[3], b.hit ? b.t : 0.0f, b.object, C[0], C[1], C[2], B[0], B[1], B[2], tint.x, tint.y, tint.z, int(H[1])); }
		}
		const bool ok = sphere > 150 && notBounced == 0 && beforeBounced == 0 && checked > 100 && bad <= checked / 100 && coloured > 10 && colouredBefore == 0;
		say("  C MetalBounce: %d chrome-sphere hits in the mirror, %d checked (stable): %d mismatches, %d bounces met the coloured boxes, %d the sky; "
			"bounce direction vs the exact mirror direction: mean %.3f deg; largest tint error %.4f; not bounced %d. Before (MetalBounce 0): %d bounced, %d pixels coloured (the chrome's own white) -> %s\n",
			sphere, checked, bad, coloured, misses, checked ? dirSum / checked : 0.0, tintErr, notBounced, beforeBounced, colouredBefore, ok ? "ok" : "BAD");
		allOk = allOk && ok;
		if (!ok) { dumpPfm("reflfix_C_shade_r", shade, S, S, 4, 0); dumpPfm("reflfix_C_bounce_t", bounce, S, S, 4, 3); }
	}
	return allOk;
}
