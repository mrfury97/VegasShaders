// (2026-11-07) Scene `ptaniso`: anisotropic texture filtering at ray-traced hits (RtTextures.glsl rtTexSampleHit: the ray cone's ellipse on
// the hit triangle as textureGrad's gradients; the shared sampler's 8x anisotropy).
//
// Testers: "we're losing a lot of texture quality when RT is on" (mostly the path tracer's shading, docs/next/RT-PATH-TRACING.md section
// 55); at ray-traced hits the mip came from the cone's width alone (rtTexLod), so a surface seen at a glancing angle was under-filtered
// along the ray's tilt (aliasing, shimmer) or, with a wider cone, blurred across. Here a floor of real textures (256 texels, full mip chain)
// seen at about 3 degrees, the path tracer's first-hit colour (PTDebugView 1), each texture twice: anisotropic (the default) and one
// isotropic mip (flag 2097152):
//   - the truth: each pixel's average over 8 x 8 rays, the pattern at the full texture (CPU). Bands along the view (period 8 texels along
//     v): the anisotropic error against it at most 0.92 x the one mip's; stripes across (along u): no worse. Head-on both must agree.
//     (A first version asked the anisotropic stripes to keep the one mip's contrast: off-centre the footprint leans, and that extra
//     contrast was under-filtering, not detail.)
static bool scenePtAniso(Ctx& c) {
	Host& h = *c.host;
	const uint32_t res = 256, levels = 9;
	auto makeTexture = [&](bool alongV) {
		std::vector<uint8_t> out;
		std::vector<float> a(size_t(res) * res);
		for (uint32_t y = 0; y < res; y++) for (uint32_t x = 0; x < res; x++) a[size_t(y) * res + x] = (((alongV ? y : x) / 4) % 2 == 0) ? 1.0f : 0.0f;
		uint32_t r = res;
		for (uint32_t l = 0; l < levels; l++) {
			for (uint32_t i = 0; i < r * r; i++) {
				// sRGB-encoded (as the game's textures): the shader decodes, so the linear average of the bands is 0.5
				const float lin = a[i], enc = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
				const uint8_t v = uint8_t(std::min(std::max(enc * 255.0f + 0.5f, 0.0f), 255.0f));
				out.push_back(v); out.push_back(v); out.push_back(v); out.push_back(255);
			}
			if (r == 1) break;
			const uint32_t n = r / 2;
			std::vector<float> b(size_t(n) * n);
			for (uint32_t y = 0; y < n; y++) for (uint32_t x = 0; x < n; x++)
				b[size_t(y) * n + x] = 0.25f * (a[(2 * y) * r + 2 * x] + a[(2 * y) * r + 2 * x + 1] + a[(2 * y + 1) * r + 2 * x] + a[(2 * y + 1) * r + 2 * x + 1]);
			a.swap(b); r = n;
		}
		return out;
	};
	const int slotV = 60, slotU = 61;
	const uint32_t idV = 21, idU = 22;
	if (!h.StageTexture(idV, 44 /* VK_FORMAT_B8G8R8A8_UNORM */, res, res, levels, makeTexture(true)) ||
		!h.StageTexture(idU, 44, res, res, levels, makeTexture(false))) return false;
	h.SlotTextures(slotV, idV, 0, 0);
	h.SlotTextures(slotU, idU, 0, 0);
	World w;
	Mesh floor; floor.quad({ -6000, -6000, 0 }, { 6000, -6000, 0 }, { 6000, 6000, 0 }, { -6000, 6000, 0 });
	for (size_t i = floor.uv.size() - 8; i < floor.uv.size(); i++) floor.uv[i] *= 60.0f;   // 60 repeats over 12000 units: 200 units a texture
	w.meshes = { floor };
	Object f; f.mesh = 0; f.textured = true; f.albedo = { 0.5f, 0.5f, 0.5f };
	const Camera cam = Camera::lookDir({ 0, -3000, 70 }, { 0, 1, -0.05f }, { 0, 0, 1 }, 1.4f, 1.4f);
	const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, gM.albedoAtlas, gM.albedoAtlas, c.depth, c.normals, c.aux16, c.pano,
		c.aux16, gM.albedoAtlas, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
	// each pixel's floor distance (CPU): far = 1500+ units, mid = 300-900
	std::vector<float> dist(size_t(S) * S, -1.0f);
	auto run = [&](int slot, bool iso, std::vector<float>& out, const Camera& cam) {
		World v = w; Object o = f; o.alphaSlot = slot; v.objects = { o };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, v, ids)) return false;
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 9.0f;
		put4(k.accum, 0.0f, 1.0f, 1.0f, float(2u | (iso ? 2097152u : 0u)));   // the albedo atlas flag (rtHitAlbedo), the A/B flag
		put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0.6f, 0.7f, 0.8f, 1.0f); put4(k.skyHorizon, 0.6f, 0.7f, 0.8f, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 1.0f);   // DebugView 1: the first hit's colour
		k.rect[3] = 1.0f;
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(v, ids, cam.eye)) return false;
		const bool ok = h.Read(c.ptDisplay, out);
		h.EndFrame();
		releaseWorld(h, ids);
		return ok;
	};
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		World v = w; Object o = f; v.objects = { o };
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(v, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (hh.hit) dist[size_t(j) * S + i] = hh.t;
	}
	std::vector<float> vAniso, vIso, uAniso, uIso;
	if (!run(slotV, false, vAniso, cam) || !run(slotV, true, vIso, cam) || !run(slotU, false, uAniso, cam) || !run(slotU, true, uIso, cam)) return false;
	// head-on (straight down): the ellipse is a circle, the same mip as one isotropic mip: the two pictures must agree
	const Camera down = Camera::lookDir({ 0, 0, 900 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.4f, 1.4f);
	std::vector<float> dA, dI;
	if (!run(slotU, false, dA, down) || !run(slotU, true, dI, down)) return false;
	double headOn = 0; for (size_t q = 0; q < size_t(S) * S; q++) headOn = std::max(headOn, (double)std::fabs(dA[q * 4] - dI[q * 4]));
	say("  head-on: anisotropic vs one mip, largest difference %.4f (must be < 0.02)\n", headOn);
	// the truth: each pixel's average over 8 x 8 rays inside it, the pattern at the full texture (sRGB-decoded: 1 or 0)
	auto truth = [&](bool alongV, std::vector<float>& ref) {
		ref.assign(size_t(S) * S, -1.0f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			double sum = 0; int n = 0;
			for (int sy = 0; sy < 8; sy++) for (int sx = 0; sx < 8; sx++) {
				const V3 d = cam.ray((i + (sx + 0.5f) / 8.0f) / S, (j + (sy + 0.5f) / 8.0f) / S);
				if (!(d.z < -1e-6f)) continue;
				const float t = -cam.eye.z / d.z;
				const V3 q = cam.eye + d * t;
				const double u = (q.x + 6000.0) / 12000.0 * 60.0, v = (q.y + 6000.0) / 12000.0 * 60.0;
				const double c = alongV ? v : u;
				const int texel = int(std::floor((c - std::floor(c)) * 256.0));
				sum += ((texel / 4) % 2 == 0) ? 1.0 : 0.0; n++;
			}
			if (n == 64) ref[size_t(j) * S + i] = float(sum / n);
		}
	};
	std::vector<float> refV, refU;
	truth(true, refV); truth(false, refU);
	// mean error against the truth, at a glancing angle (floor distance 300 - 3000: well past the near rows, before the horizon)
	auto error = [&](const std::vector<float>& img, const std::vector<float>& ref, int& n) {
		double e = 0; n = 0;
		for (size_t q = 0; q < size_t(S) * S; q++) if (ref[q] >= 0.0f && dist[q] > 300.0f && dist[q] < 3000.0f) { e += std::fabs(img[q * 4] - ref[q]); n++; }
		return n ? e / n : 1.0;
	};
	int nf = 0, nu = 0;
	const double eA = error(vAniso, refV, nf), eI = error(vIso, refV, nf), uA = error(uAniso, refU, nu), uI = error(uIso, refU, nu);
	const bool aliasOk = nf > 200 && eA <= 0.92 * eI && headOn < 0.02;   // (2026-11-07 on the iGPU: 0.125 vs 0.140; stripes 0.069 vs 0.078)
	const bool sharpOk = nu > 200 && uA <= uI;
	say("  bands along the view (%d pixels at a glancing angle): mean error against 64 rays a pixel %.4f anisotropic, %.4f one mip -> %s\n", nf, eA, eI, aliasOk ? "ok" : "BAD");
	say("  stripes across: mean error %.4f anisotropic, %.4f one mip (must not be worse) -> %s\n", uA, uI, sharpOk ? "ok" : "BAD");
	if (!aliasOk) { dumpPfm("ptaniso_bands_aniso", vAniso, S, S, 4, 0); dumpPfm("ptaniso_bands_iso", vIso, S, S, 4, 0); }
	h.ReleaseTexture(idV); h.ReleaseTexture(idU);
	h.SlotTextures(slotV, 0, 0, 0); h.SlotTextures(slotU, 0, 0, 0);
	return aliasOk && sharpOk;
}
