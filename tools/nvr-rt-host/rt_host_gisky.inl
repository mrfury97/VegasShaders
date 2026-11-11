// (2026-11-10) Scene `gisky` ([Shaders.AmbientOcclusion.RT] GISky, RtAOTrace.comp SkyMiss): RT GI's bounce rays that find nothing bring back
// the ambient (sky) light x GISky (gi.y - 1), full above the horizon and none below it (clamp(0.5 + 2 d.z, 0, 1)), capped by GIClamp.
// An open world (one small quad far out of the rays' reach), the AO input a floor (normal up) or a wall (normal sideways) at every texel,
// one GI ray a texel (the plain pass, no reuse):
//   - GISky 0 (gi.y 1, the game's value before the change): every texel's bounce exactly 0 (bit for bit as before);
//   - GISky 0.5: no texel above ambient x 0.5, each the ambient's hue, the mean = ambient x 0.5 x the CPU's mean weight over the cosine
//     lobe (Malley, 1024 x 1024 directions) within 2 %; the wall: about half its rays point below the horizon, so its mean is lower;
//   - GIClamp below the sky's luminance: the cap holds.
static bool sceneGISky(Ctx& c) {
	Host& h = *c.host;
	Mesh distant; distant.quad({ 90000, 90000, -90000 }, { 90010, 90000, -90000 }, { 90010, 90010, -90000 }, { 90000, 90010, -90000 });
	World w; w.meshes = { distant };
	Object o; o.mesh = 0; o.albedo = { 0.5f, 0.5f, 0.5f }; w.objects = { o };
	const Camera cam = Camera::lookDir({ 0, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.0f);
	const float H = 100.0f;
	const V3 amb = { 0.2f, 0.3f, 0.4f };
	auto weightMean = [](V3 n, float sky, float capShare) {   // E[min(sky x the shader's horizon weight, capShare)] over the cosine lobe round n
		const V3 t = std::fabs(n.z) < 0.9f ? normalize(cross({ 0, 0, 1 }, n)) : V3{ 1, 0, 0 };
		const V3 b = cross(n, t);
		const int N = 1024;
		double sum = 0.0;
		for (int a = 0; a < N; a++) for (int e = 0; e < N; e++) {
			const float r = std::sqrt((a + 0.5f) / N), phi = 6.2831853f * (e + 0.5f) / N;
			const float lz = std::sqrt(std::max(1.0f - r * r, 0.0f));
			const V3 d = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * lz;
			sum += std::min(sky * std::min(std::max(0.5f + 2.0f * d.z, 0.0f), 1.0f), capShare);
		}
		return sum / (double(N) * N);
	};
	auto run = [&](V3 n, float giY, float clampLum, std::vector<float>& gi) {
		std::vector<float> aoIn(size_t(S) * S * 2);
		for (UINT q = 0; q < S * S; q++) { aoIn[q * 2] = H; aoIn[q * 2 + 1] = packAO(n); }
		if (!h.Upload(gM.aoIn, aoIn)) return false;
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		AOConstants a = {};
		const V3 Rr = cam.r, U = cam.u, F = cam.f;
		const float m[16] = { Rr.x, Rr.y, Rr.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, 0, 0, 0, 1 };
		std::memcpy(a.viewToWorld, m, sizeof(m));
		put4(a.projection, 1.0f / cam.p00, 1.0f / cam.p11, float(S), float(S));
		put4(a.origin, 0, 0, 0, 3);
		put4(a.ao, 300.0f, 0.0f, 1.0f, 0.0f);      // no AO rays
		put4(a.sky, 3000.0f, 0.0f, 0.0f, 0.0f);    // no sky rays
		put4(a.gi, 5000.0f, giY, 0.0f, 0.0f);      // GI: 5000 units, 1 ray (+ GISky), no lamps, GIAmbient 0
		put4(a.sunDir, V3{ 0, 0, 1 }, 1.0f); put4(a.sunColor, 1.0f, 1.0f, 1.0f, 0.0f);
		put4(a.ambient, amb, 0.0f);                // the sky light; GlowGI 0
		put4(a.gi2, 1.0f, 0.03f, clampLum, 1.0f);  // GIOffscreen 1, tolerance, GIClamp, Interleave 1
		put4(a.gi3, 1.0f, 1.0f, 0.0f, 0.0f);       // GIInterleave 1, AOAlpha 1, no reuse
		h.AddPass(gM.shAO, { gM.aoIn, gM.aoOut, gM.aoGi, c.alphaAtlas, gM.aoLight, gM.albedoAtlas }, 0x6, &a, sizeof(a), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		const bool ok = h.Read(gM.aoGi, gi);
		h.EndFrame();
		releaseWorld(h, ids);
		return ok;
	};
	const float ambLum = 0.2126f * amb.x + 0.7152f * amb.y + 0.0722f * amb.z;
	bool allOk = true;
	struct Case { const char* name; V3 n; float sky; float clampLum; };
	const Case cases[4] = { { "floor, GISky 0", { 0, 0, 1 }, 0.0f, 10000.0f }, { "floor, GISky 0.5", { 0, 0, 1 }, 0.5f, 10000.0f },
		{ "wall, GISky 0.5", { 1, 0, 0 }, 0.5f, 10000.0f }, { "floor, GISky 0.5, GIClamp 0.05", { 0, 0, 1 }, 0.5f, 0.05f } };
	for (const Case& k : cases) {
		std::vector<float> gi;
		if (!run(k.n, 1.0f + k.sky, k.clampLum, gi)) return false;
		const float cap = std::min(k.sky, k.clampLum / ambLum);   // the largest share of amb a texel may get
		const double want = weightMean(k.n, k.sky, k.clampLum / ambLum);   // (GIClamp caps a sample's luminance: a share of amb of clamp / its luminance)
		int bad = 0, hue = 0, alpha = 0;
		double mean = 0.0;
		for (UINT q = 0; q < S * S; q++) {
			const float r = gi[q * 4], g = gi[q * 4 + 1], b = gi[q * 4 + 2], al = gi[q * 4 + 3];
			if (k.sky == 0.0f) { if (r != 0.0f || g != 0.0f || b != 0.0f) bad++; }
			else if (r > amb.x * cap * 1.003f + 1e-4f || g > amb.y * cap * 1.003f + 1e-4f || b > amb.z * cap * 1.003f + 1e-4f || r < 0 || g < 0 || b < 0) bad++;
			if (b > 1e-4f && (std::fabs(r / b - amb.x / amb.z) > 0.01f || std::fabs(g / b - amb.y / amb.z) > 0.01f)) hue++;
			if (std::fabs(al - 1.0f) > 1e-3f) alpha++;
			mean += b / amb.z;
		}
		mean /= double(S) * S;
		const bool ok = bad == 0 && hue == 0 && alpha == 0 && (k.sky == 0.0f ? mean == 0.0 : std::fabs(mean - want) <= 0.02 * want);
		say("  %-32s mean %.4f x ambient (CPU %.4f), %d texels out of range, %d off the ambient's hue, %d without alpha 1 -> %s\n", k.name, mean, want, bad,
			hue, alpha, ok ? "ok" : "BAD");
		if (!ok) dumpPfm("gisky_gi", gi, S, S, 4, 0);
		allOk = allOk && ok;
	}
	return allOk;
}
