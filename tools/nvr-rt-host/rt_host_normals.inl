// (2026-11-07) Scene `normalmaps` (docs/next/RT-DETAIL-GAPS.md section 2): ray-traced hits shade with the surface's REAL normal map
// ([Main.Main.RayTracing] RealNormalMaps: the slot's normal map as a real texture, set 1 id .y; record flag 0x80; no normal atlas),
// in the three places that shade traced hits: the path tracer (a camera ray's traced hit and a bounce's hit), RT reflections' hit
// normal (ReflectionsRTTex) and RT GI's bounce (RtAOTrace). Every surface lit by a sun at a grazing angle (15 degrees), so a bump
// tilted toward it is lit ~2.7x the flat surface and one tilted away is dark:
//   - the maps: stripes along u (tilted 30 degrees toward +u, flat, tilted away, flat; 16 texels each of 64) or one uniform tilt;
//   - the CPU's expectation: the shader's own tangent frame (the triangle's positions and texcoords) and bend, from the map's bytes;
//   - each case also with the record's normal flag off (as RealNormalMaps off) and with the flag on but no normal map in the slot (id 0,
//     the path tracer's normal-atlas binding holding a stand-in): both must be the flat surface's light exactly.
struct NormalCtx { int shReflTex = -1; };
static NormalCtx gN;
static bool registerNormals(Host& h, const std::string& spv) {
	gN.shReflTex = h.AddShader(spv + "\\ReflectionsRTTex.spv");
	return gN.shReflTex >= 0;
}

// The map's texel -> the tangent-space vector the shader uses (rgb x 2 - 1, as stored: UNORM)
static V3 nmDecode(const uint8_t* rgb) { return { rgb[0] / 255.0f * 2 - 1, rgb[1] / 255.0f * 2 - 1, rgb[2] / 255.0f * 2 - 1 }; }
static void nmEncode(V3 t, uint8_t* out) {
	const float c[3] = { t.x, t.y, t.z };
	for (int k = 0; k < 3; k++) out[k] = uint8_t(std::min(std::max((c[k] * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f), 255.0f));
	out[3] = 255;
}
// RtHit.glsl rtHitNormalMapped on the CPU: n = the triangle normal facing the ray, (p0, p1, p2) / (u0, u1, u2) the triangle
static V3 nmBend(V3 n, V3 p0, V3 p1, V3 p2, const float* u0, const float* u1, const float* u2, V3 t) {
	const V3 e1 = p1 - p0, e2 = p2 - p0;
	const float d1x = u1[0] - u0[0], d1y = u1[1] - u0[1], d2x = u2[0] - u0[0], d2y = u2[1] - u0[1];
	const float det = d1x * d2y - d2x * d1y;
	V3 tangent = (e1 * d2y - e2 * d1y) * (1.0f / det), bitangent = (e2 * d1x - e1 * d2x) * (1.0f / det);
	tangent = normalize(tangent - n * dot(n, tangent));
	const V3 c = cross(n, tangent);
	const V3 b = c * (dot(c, bitangent) < 0 ? -1.0f : 1.0f);
	return normalize(tangent * t.x + b * t.y + n * std::max(t.z, 0.05f));
}

static bool sceneNormalMaps(Ctx& c) {
	Host& h = *c.host;
	const float kTilt = 0.5235988f;   // 30 degrees
	const V3 tiltPlus = { std::sin(kTilt), 0.0f, std::cos(kTilt) }, tiltMinus = { -std::sin(kTilt), 0.0f, std::cos(kTilt) }, flat = { 0, 0, 1 };
	// the striped map (64 x 64, one level): u texel 0-15 toward +u, 16-31 flat, 32-47 away, 48-63 flat
	const uint32_t R = 64;
	std::vector<uint8_t> stripes(size_t(R) * R * 4), uniform(size_t(R) * R * 4), away(size_t(R) * R * 4), white(size_t(R) * R * 4, 255);
	for (uint32_t y = 0; y < R; y++) for (uint32_t x = 0; x < R; x++) {
		const int kind = int(x / 16);
		nmEncode(kind == 0 ? tiltPlus : kind == 2 ? tiltMinus : flat, &stripes[(size_t(y) * R + x) * 4]);
		nmEncode(tiltPlus, &uniform[(size_t(y) * R + x) * 4]);
		nmEncode(tiltMinus, &away[(size_t(y) * R + x) * 4]);
	}
	const V3 stripeT[4] = { nmDecode(&stripes[0]), nmDecode(&stripes[16 * 4]), nmDecode(&stripes[32 * 4]), nmDecode(&stripes[48 * 4]) };
	const V3 uniformT = nmDecode(&uniform[0]), awayT = nmDecode(&away[0]);
	const uint32_t idStripes = 41, idUniform = 42, idAway = 43, idWhite = 44;
	const int slotStripes = 70, slotUniform = 71, slotAway = 72, slotNone = 73;
	if (!h.StageTexture(idStripes, 37 /* VK_FORMAT_R8G8B8A8_UNORM */, R, R, 1, stripes) || !h.StageTexture(idUniform, 37, R, R, 1, uniform) ||
		!h.StageTexture(idAway, 37, R, R, 1, away) || !h.StageTexture(idWhite, 37, R, R, 1, white)) return false;
	h.SlotTextures(slotStripes, idWhite, idStripes, 0);
	h.SlotTextures(slotUniform, idWhite, idUniform, 0);
	h.SlotTextures(slotAway, idWhite, idAway, 0);
	h.SlotTextures(slotNone, idWhite, 0, 0);   // a record with the normal flag whose slot has no normal map: flat
	bool allOk = true;
	const float flatLight = 0.258819f;   // sin 15: the flat surface under the grazing sun

	// ---- 1. the path tracer, a camera ray's traced hit (no G-buffer: the hit is the RT scene's): DebugView 3 (direct light) and 2 (normals)
	{
		const float L = 1200.0f, K = 4.0f;   // the floor: 2400 units, the map 4 times (600 units, 9.4 units a texel)
		Mesh floor; floor.quad({ -L, -L, 0 }, { L, -L, 0 }, { L, L, 0 }, { -L, L, 0 });
		for (size_t i = floor.uv.size() - 8; i < floor.uv.size(); i++) floor.uv[i] *= K;
		World w; w.meshes = { floor };
		const Camera cam = Camera::lookDir({ 0, 0, 700 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.0f);
		const V3 sun = normalize({ 0.9659258f, 0.0f, 0.2588190f });
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, gM.albedoAtlas, gM.albedoAtlas, c.depth, c.normals, c.aux16, c.pano,
			c.aux16, gM.albedoAtlas /* the normal atlas' stand-in (flag 64 off) */, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		auto run = [&](int slot, bool flag, int view, std::vector<float>& out) {
			World v = w; Object o; o.mesh = 0; o.textured = true; o.alphaSlot = slot; o.normalMap = flag; o.albedo = { 0.5f, 0.5f, 0.5f }; v.objects = { o };
			std::vector<uint32_t> ids;
			if (!stageWorld(h, v, ids)) return false;
			PathConstants k = {};
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = 5.0f;
			put4(k.accum, 0.0f, 0.0f, 1.0f, 0.0f);   // start over, 0 bounces, 1 path; no atlas / G-buffer flags: the record's albedo
			put4(k.sunDir, sun, 1.0f); put4(k.sunColor, 1.0f, 1.0f, 1.0f, 0.0f);   // (w 0: the sun's direction exactly)
			put4(k.skyZenith, 0, 0, 0, 1); put4(k.skyHorizon, 0, 0, 0, 0);
			put4(k.misc, 0.0f, 0.0f, 60000.0f, float(view));
			k.rect[3] = 1.0f;
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(v, ids, cam.eye)) return false;
			const bool ok = h.Read(c.ptDisplay, out);
			h.EndFrame();
			releaseWorld(h, ids);
			return ok;
		};
		std::vector<float> lit, nrm, litOff, litNone;
		if (!run(slotStripes, true, 3, lit) || !run(slotStripes, true, 2, nrm) || !run(slotStripes, false, 3, litOff) || !run(slotNone, true, 3, litNone)) return false;
		int checked = 0, bad = 0, shown = 0, kinds[4] = {};
		double sum[4] = {}, offErr = 0, noneErr = 0;
		const float uv0[2] = { 0, 0 }, uv1[2] = { K, 0 }, uv2[2] = { K, K };
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			if (!(d.z < 0)) continue;
			const V3 q = cam.eye + d * (-cam.eye.z / d.z);
			if (std::fabs(q.x) > L - 10 || std::fabs(q.y) > L - 10) continue;
			const float u = (q.x + L) / (2 * L) * K, tex = fract(u) * R;
			const float edge = std::fmod(tex, 16.0f);
			if (edge < 2.0f || edge > 14.0f) continue;   // within 2 texels of a stripe's edge: filtered across it, not checked
			const int kind = int(tex / 16.0f) & 3;
			const V3 bent = nmBend({ 0, 0, 1 }, { -L, -L, 0 }, { L, -L, 0 }, { L, L, 0 }, uv0, uv1, uv2, stripeT[kind]);
			const float want = 0.5f * std::max(dot(bent, sun), 0.0f);
			const float* g = &lit[(j * S + i) * 4];
			const float* gn = &nrm[(j * S + i) * 4];
			const V3 gotN = normalize({ gn[0] * 2 - 1, gn[1] * 2 - 1, gn[2] * 2 - 1 });
			checked++;
			kinds[kind]++; sum[kind] += g[0];
			offErr = std::max(offErr, (double)std::fabs(litOff[(j * S + i) * 4] - 0.5f * flatLight));
			noneErr = std::max(noneErr, (double)std::fabs(litNone[(j * S + i) * 4] - 0.5f * flatLight));
			const bool ok = std::fabs(g[0] - want) < 3e-3f && std::fabs(g[1] - want) < 3e-3f && dot(gotN, bent) > 0.999f;
			if (!ok) { bad++; if (shown++ < 5) say("    pixel (%u, %u) stripe %d: light %.4f normal (%.3f %.3f %.3f), expected %.4f (%.3f %.3f %.3f)\n", i, j, kind, g[0], gotN.x, gotN.y, gotN.z, want, bent.x, bent.y, bent.z); }
		}
		const bool ok = checked > 4000 && bad <= checked / 500 && kinds[0] > 500 && kinds[2] > 500 && offErr < 2e-3 && noneErr < 2e-3;
		say("  path tracer, camera ray's hit: %d pixels checked, %d off the CPU's bend; direct light by stripe: toward the sun %.4f, flat %.4f, away %.4f "
			"(flat surface %.4f); the flag off: largest difference from flat %.5f, flag on without a map: %.5f -> %s\n", checked, bad, kinds[0] ? sum[0] / kinds[0] : 0.0,
			kinds[1] + kinds[3] ? (sum[1] + sum[3]) / (kinds[1] + kinds[3]) : 0.0, kinds[2] ? sum[2] / kinds[2] : 0.0, 0.5 * flatLight, offErr, noneErr, ok ? "ok" : "BAD");
		if (!ok) { dumpPfm("normalmaps_pt_lit", lit, S, S, 4, 0); dumpPfm("normalmaps_pt_off", litOff, S, S, 4, 0); }
		allOk = allOk && ok;
	}

	// ---- 2. bounce hits: a ceiling with one uniform tilt over a floor that is only in the G-buffer (no RT floor: the ceiling's sun rays
	// go down unblocked). The path tracer's PTDebugView 5 (the light arriving at the first hit through one cosine bounce: the bounce
	// hit's albedo x its direct light) and RT GI's bounce (RtAOTrace, one ray a texel: the hit's albedo x its light) are then the same
	// number at every pixel, whatever direction the ray took: albedo x max(N.L, 0) with N the bent normal.
	{
		const float H = 100.0f, C = 60000.0f;
		Mesh ceiling; ceiling.quad({ -C, -C, H }, { C, -C, H }, { C, C, H }, { -C, C, H });
		for (size_t i = ceiling.uv.size() - 8; i < ceiling.uv.size(); i++) ceiling.uv[i] *= 100.0f;
		World w; w.meshes = { ceiling };
		const Camera cam = Camera::lookDir({ 0, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.0f);
		const V3 sun = normalize({ 0.9659258f, 0.0f, -0.2588190f });   // from below, 15 degrees under the ceiling's plane
		const float uv0[2] = { 0, 0 }, uv1[2] = { 100, 0 }, uv2[2] = { 100, 100 };
		auto bentOf = [&](V3 t) { return nmBend({ 0, 0, -1 }, { -C, -C, H }, { C, -C, H }, { C, C, H }, uv0, uv1, uv2, t); };   // (the ceiling's normal faces the upward rays)
		auto expectLight = [&](V3 t) { return std::max(dot(bentOf(t), sun), 0.0f); };
		// The path tracer keeps the flat normal where the bent one turns away from the ray (RtPathTrace.comp: dot(bent, v) > 0; it also
		// samples its next direction round it): the share of its cosine bounce rays from the floor for which that happens, exactly
		auto flatShare = [&](V3 t) {
			const V3 b = bentOf(t);
			const int N = 1024;
			long long flatCount = 0;
			for (int a = 0; a < N; a++) for (int e = 0; e < N; e++) {   // Malley: the unit disc lifted to the hemisphere round (0, 0, 1)
				const float r = std::sqrt((a + 0.5f) / N), phi = 6.2831853f * (e + 0.5f) / N;
				const V3 d = { r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(1.0f - r * r, 0.0f)) };
				if (!(dot(b, -d) > 0.0f)) flatCount++;
			}
			return double(flatCount) / (double(N) * N);
		};
		// the floor (z = -H, normal up) in the G-buffer for the path tracer, in the AO input for RT GI
		std::vector<float> depth(size_t(S) * S), normals(size_t(S) * S * 4), albedo(size_t(S) * S * 4), aoIn(size_t(S) * S * 2);
		for (UINT q = 0; q < S * S; q++) {
			depth[q] = H / cam.farZ;   // view z / farZ: the floor H under an eye looking straight down
			const V3 n = { 0, 0, 1 }, vs = { dot(n, cam.r), dot(n, cam.u), dot(n, cam.f) };
			put4(&normals[q * 4], vs * 0.5f + V3(0.5f, 0.5f, 0.5f), 0.0f);
			put4(&albedo[q * 4], 0.5f, 0.5f, 0.5f, 1.0f);
			aoIn[q * 2] = H; aoIn[q * 2 + 1] = packAO(n);
		}
		if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals) || !h.Upload(c.aux16, albedo) || !h.Upload(gM.aoIn, aoIn)) return false;
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, gM.albedoAtlas, gM.albedoAtlas, c.depth, c.normals, c.aux16 /* the G-buffer's albedo */,
			c.pano, c.aux16, gM.albedoAtlas /* the normal atlas' stand-in */, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		auto run = [&](int slot, bool flag, std::vector<float>& pt, std::vector<float>& gi) {
			World v = w; Object o; o.mesh = 0; o.textured = true; o.alphaSlot = slot; o.normalMap = flag; o.albedo = { 0.5f, 0.5f, 0.5f }; v.objects = { o };
			std::vector<uint32_t> ids;
			if (!stageWorld(h, v, ids)) return false;
			PathConstants k = {};
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = 11.0f;
			put4(k.accum, 0.0f, 1.0f, 1.0f, 8.0f);   // 1 bounce, 1 path; flag 8: the G-buffer's floor is the first hit
			put4(k.sunDir, sun, 1.0f); put4(k.sunColor, 1.0f, 1.0f, 1.0f, 0.0f);
			put4(k.skyZenith, 0, 0, 0, 1); put4(k.skyHorizon, 0, 0, 0, 0);
			put4(k.misc, 0.0f, 0.0f, 60000.0f, 5.0f);   // DebugView 5: the first hit's arriving light through one bounce
			k.rect[3] = 1.0f;
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			AOConstants a = {};
			const V3 Rr = cam.r, U = cam.u, F = cam.f;
			const float m[16] = { Rr.x, Rr.y, Rr.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, 0, 0, 0, 1 };
			std::memcpy(a.viewToWorld, m, sizeof(m));
			put4(a.projection, 1.0f / cam.p00, 1.0f / cam.p11, float(S), float(S));
			put4(a.origin, 0, 0, 0, 3);
			put4(a.ao, 300.0f, 0.0f, 1.0f, 0.0f);      // no AO rays
			put4(a.sky, 3000.0f, 0.0f, 0.0f, 0.0f);    // no sky rays
			put4(a.gi, 5000.0f, 1.0f, 0.0f, 0.0f);     // GI: 5000 units, 1 ray, no lamps, GIAmbient 0
			put4(a.sunDir, sun, 1.0f); put4(a.sunColor, 1.0f, 1.0f, 1.0f, 0.0f);
			put4(a.gi2, 1.0f, 0.03f, 10000.0f, 1.0f);  // GIOffscreen 1, tolerance, GIClamp (none), Interleave 1
			put4(a.gi3, 1.0f, 1.0f, 0.0f, 0.0f);       // GIInterleave 1, AOAlpha 1, no reuse
			h.AddPass(gM.shAO, { gM.aoIn, gM.aoOut, gM.aoGi, c.alphaAtlas, gM.aoLight, gM.albedoAtlas }, 0x6, &a, sizeof(a), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(v, ids, cam.eye)) return false;
			const bool ok = h.Read(c.ptDisplay, pt) && h.Read(gM.aoGi, gi);
			h.EndFrame();
			releaseWorld(h, ids);
			return ok;
		};
		struct Case { const char* name; int slot; bool flag; float want; double share; };
		const Case cases[4] = { { "tilted toward the sun", slotUniform, true, expectLight(uniformT), flatShare(uniformT) },
			{ "tilted away", slotAway, true, expectLight(awayT), flatShare(awayT) },
			{ "flag off", slotUniform, false, flatLight, 1.0 }, { "flag on, no map", slotNone, true, flatLight, 1.0 } };
		double ptMean[4] = {}, giMean[4] = {};
		for (int ci = 0; ci < 4; ci++) {
			std::vector<float> pt, gi;
			if (!run(cases[ci].slot, cases[ci].flag, pt, gi)) return false;
			int ptN = 0, ptBad = 0, ptFlat = 0, giN = 0, giBad = 0, giMiss = 0;
			for (UINT q = 0; q < S * S; q++) {
				// the path tracer: the bounce hit's albedo (the record's 0.5) x its light (bent, or flat where the bend faces away from the
				// ray); RT GI: its albedo (the white real texture: 1) x its light with the bent normal always (diffuse)
				const float p0 = pt[q * 4], g0 = gi[q * 4], ga = gi[q * 4 + 3];
				ptN++; ptMean[ci] += p0;
				if (std::fabs(p0 - 0.5f * flatLight) <= 3e-3f && (cases[ci].share >= 1.0 || std::fabs(p0 - 0.5f * cases[ci].want) > 3e-3f)) ptFlat++;
				else if (std::fabs(p0 - 0.5f * cases[ci].want) > 3e-3f) ptBad++;
				if (ga > 0.999f) { giMiss++; continue; }   // (a ray grazing past the ceiling's edge)
				giN++; giMean[ci] += g0;
				if (std::fabs(g0 - cases[ci].want) > 5e-3f) giBad++;
			}
			ptMean[ci] /= std::max(ptN, 1); giMean[ci] /= std::max(giN, 1);
			const double share = double(ptFlat) / std::max(ptN, 1);
			const bool ok = ptBad <= ptN / 500 && std::fabs(share - cases[ci].share) < 0.015 && giBad <= giN / 500 && giN > int(S * S) * 9 / 10;
			say("  bounce hits, %-22s expected light %.4f: path tracer (/ albedo 0.5) mean %.4f, %d of %d pixels off, flat where the bend faces away %.3f "
				"(CPU %.3f); RT GI mean %.4f, %d of %d texels off (%d missed) -> %s\n", cases[ci].name, cases[ci].want, ptMean[ci] / 0.5, ptBad, ptN, share,
				cases[ci].share, giMean[ci], giBad, giN, giMiss, ok ? "ok" : "BAD");
			if (!ok) { dumpPfm("normalmaps_bounce_pt", pt, S, S, 4, 0); dumpPfm("normalmaps_bounce_gi", gi, S, S, 4, 0); }
			allOk = allOk && ok;
		}
	}

	// ---- 3. RT reflections' hit normal (ReflectionsRTTex): rays from a "surface" 10 units ahead of the eye along the view (a pass-through
	// mirror) to a striped wall; the hit's normal (hit.zw) must be the CPU's bent normal there; with the flag off, the wall's flat normal
	{
		const float W = 2000.0f, Y = 800.0f, K = 8.0f;   // the map every 500 units: a stripe 125 units, ~10 pixels
		Mesh wall; wall.quad({ -W, Y, -W }, { W, Y, -W }, { W, Y, W }, { -W, Y, W });
		for (size_t i = wall.uv.size() - 8; i < wall.uv.size(); i++) wall.uv[i] *= K;
		World w; w.meshes = { wall };
		const Camera cam = Camera::lookDir({ 0, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, 1.0f, 1.0f);
		const V3 sun = normalize({ 0.9659258f, -0.2588190f, 0.0f });   // along the wall, 15 degrees off it
		std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			octEncode(normalize(d), dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
			ray[(j * S + i) * 2] = 10.0f; ray[(j * S + i) * 2 + 1] = 2.0f;   // view z 10, roughness 0 (+2: cast)
		}
		if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
		auto run = [&](bool flag, std::vector<float>& hit) {
			World v = w; Object o; o.mesh = 0; o.textured = true; o.alphaSlot = slotStripes; o.normalMap = flag; o.albedo = { 0.5f, 0.5f, 0.5f }; v.objects = { o };
			std::vector<uint32_t> ids;
			if (!stageWorld(h, v, ids)) return false;
			ReflConstants k = {};
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.size[2] = 10000.0f; k.size[3] = 0.0f;   // Distance; no shadow rays
			k.forward[3] = 0.0f;
			put4(k.sun, sun, 1.0f);
			put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
			h.AddPass(gN.shReflTex, { c.refDir, c.refRay, c.refHit, c.refShade, gM.albedoAtlas }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(v, ids, cam.eye)) return false;
			const bool ok = h.Read(c.refHit, hit);
			h.EndFrame();
			releaseWorld(h, ids);
			return ok;
		};
		std::vector<float> on, off;
		if (!run(true, on) || !run(false, off)) return false;
		const float uv0[2] = { 0, 0 }, uv1[2] = { K, 0 }, uv2[2] = { K, K };
		int checked = 0, bad = 0, offBad = 0, shown = 0, kinds[4] = {};
		double nl[4] = {}, nlOff = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 ru = cam.ray((i + 0.5f) / S, (j + 0.5f) / S), d = normalize(ru);
			const V3 q = d * (Y / d.y);
			const float tWant = Y / d.y - 10.0f * length(ru);   // from the ray's start (the "surface" at view z 10)
			if (std::fabs(q.x) > W - 10 || std::fabs(q.z) > W - 10) continue;
			const float u = (q.x + W) / (2 * W) * K, tex = fract(u) * R;
			const float edge = std::fmod(tex, 16.0f);
			if (edge < 2.0f || edge > 14.0f) continue;
			const int kind = int(tex / 16.0f) & 3;
			const V3 bent = nmBend({ 0, -1, 0 }, { -W, Y, -W }, { W, Y, -W }, { W, Y, W }, uv0, uv1, uv2, stripeT[kind]);
			const float* H = &on[(j * S + i) * 4];
			const float* Ho = &off[(j * S + i) * 4];
			const V3 n = octDecode(H[2], H[3]), no = octDecode(Ho[2], Ho[3]);
			checked++;
			kinds[kind]++; nl[kind] += std::max(dot(n, sun), 0.0f); nlOff += std::max(dot(no, sun), 0.0f);
			const bool ok = std::fabs(H[0] - tWant) < 2.0f && (int(H[1]) & 4) && dot(n, bent) > 0.998f;
			if (!ok) { bad++; if (shown++ < 5) say("    pixel (%u, %u) stripe %d: t %.1f flags %d n (%.3f %.3f %.3f), expected (%.3f %.3f %.3f)\n", i, j, kind, H[0], int(H[1]), n.x, n.y, n.z, bent.x, bent.y, bent.z); }
			if (dot(no, { 0, -1, 0 }) < 0.999f) offBad++;
		}
		const bool ok = checked > 4000 && bad <= checked / 500 && offBad == 0;
		say("  RT reflections' hit normal: %d rays checked, %d off the CPU's bend, flag off: %d not flat; N.L of the grazing sun by stripe: toward %.4f, flat %.4f, "
			"away %.4f (all flat with the flag off: %.4f) -> %s\n", checked, bad, offBad, kinds[0] ? nl[0] / kinds[0] : 0.0, kinds[1] + kinds[3] ? (nl[1] + nl[3]) / (kinds[1] + kinds[3]) : 0.0,
			kinds[2] ? nl[2] / kinds[2] : 0.0, checked ? nlOff / checked : 0.0, ok ? "ok" : "BAD");
		if (!ok) dumpPfm("normalmaps_refl_hit", on, S, S, 4, 2);
		allOk = allOk && ok;
	}

	for (uint32_t id : { idStripes, idUniform, idAway, idWhite }) h.ReleaseTexture(id);
	for (int s : { slotStripes, slotUniform, slotAway, slotNone }) h.SlotTextures(uint32_t(s), 0, 0, 0);
	return allOk;
}
