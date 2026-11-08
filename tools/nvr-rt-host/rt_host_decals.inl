// (2026-11-07 rtgap-decals, docs/next/RT-DETAIL-GAPS.md 5) Scene `decals`: decals in the traced scene.
//
// The game's decals (posters, cracks, blood: BSSP_DECAL / BSSP_DYNAMIC_DECAL) go in as RtCore takes them: alpha-tested (RtScenePick::
// Classify), their reference by RtScenePick::DecalAlphaRef (blended ones 128), lifted RtScenePick::kDecalLift along their average normal
// (RtScenePick::AverageNormal + DecalLift on the instance's transform), custom index NVR_RT_CUSTOM_DECAL (RtScenePick::CustomIndex).
// Rays that must pass them (shadow, AO, visibility: RtAlpha.glsl rtAlphaPassShadow) and rays that must see them (reflections, GI, the path
// tracer's bounces, the camera's) are checked against the CPU caster (trace(): skipDecals for the first kind):
//   1. lamp: a floor decal and a floating checker card under a lamp (RtLampTrace): the card as a decal casts NO shadow (every floor pixel
//      lit, 0 mismatches); the same card as a plain alpha-tested card (today's `alphacard`) does (control);
//   2. mirror: a mirror floor reflects a wall with a decal on it and a floating decal card between the wall and the sun
//      (ReflectionsRTAlpha): the reflection rays meet the decal (its colour, its distance 0.5 in front of the wall), the sun rays at the
//      hits pass the floating card (which would shadow the wall if it were not a decal);
//   3. ao: AO + sky rays (RtAOTrace) with a decal card and a plain card overhead: AOAlpha 2 (tested) and 3 (cut-outs whole, decals
//      passed: what AmbientOcclusion.cpp sends for the default AOAlpha 1 while decals are traced): the decal never covers.

// Atlas slots 80-85: the scene's own (the realtex scene leaves slot 40 mapped to its real texture in the helper's set-1 table, so a later
// scene's cut-out in slot 40 reads that texture's alpha instead of the atlas: found here on the full run).
static int gShReflAlpha = -1;
static bool registerDecals(Host& h, const std::string& spv) {
	gShReflAlpha = h.AddShader(spv + "\\ReflectionsRTAlpha.spv");
	return gShReflAlpha >= 0;
}

// A decal as RtCore takes it. `place`: the model's translation (the transform is otherwise the identity, as the host's); the mesh's own
// face normals stand in for the vertex normals the game keeps (NiGeometryData::m_pkNormal).
static bool decalObject(const Mesh& m, int mesh, V3 place, int slot, bool blendedOnly, V3 albedo, Object& o, int& bad) {
	RtScenePick::Geometry g; g.decal = true; g.hasAlphaProperty = blendedOnly; g.alphaFlags = blendedOnly ? RtScenePick::kAlphaBlend : 0;
	RtScenePick::Switches s; s.alphaTested = true; s.decals = true;
	bool alpha = false;
	if (RtScenePick::Classify(g, s, alpha) != RtScenePick::Taken || !alpha) { bad++; say("    decal not taken alpha-tested by Classify\n"); return false; }
	std::vector<float> normals;
	for (size_t k = 0; k + 2 < m.idx.size(); k += 3) {
		const V3 a = m.pos[m.idx[k]], b = m.pos[m.idx[k + 1]], c = m.pos[m.idx[k + 2]];
		const V3 n = normalize(cross(b - a, c - a));
		for (int r = 0; r < 3; r++) { normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z); }
	}
	float mn[3];
	if (!RtScenePick::AverageNormal(normals.data(), uint32_t(normals.size() / 3), mn)) { bad++; say("    decal: no average normal\n"); return false; }
	float t[3][4] = { { 1, 0, 0, place.x }, { 0, 1, 0, place.y }, { 0, 0, 1, place.z } };
	if (!RtScenePick::DecalLift(t, mn, RtScenePick::kDecalLift)) { bad++; say("    decal: not lifted\n"); return false; }
	o = Object();
	o.mesh = mesh; o.offset = { t[0][3], t[1][3], t[2][3] };
	o.decal = true; o.alphaTested = true; o.alphaSlot = slot;
	o.alphaRef = RtScenePick::DecalAlphaRef(g.hasAlphaProperty, g.alphaFlags, 0);
	o.albedo = albedo;
	const V3 lift = o.offset - place;
	if (std::fabs(length(lift) - RtScenePick::kDecalLift) > 1e-4f) { bad++; say("    decal lift %.4f, expected %.4f\n", length(lift), RtScenePick::kDecalLift); return false; }
	return true;
}

static AlphaFn checker8() { return [](int x, int y) { return ((x / 8 + y / 8) & 1) ? 1.0f : 0.0f; }; }
static AlphaFn stripes16() { return [](int x, int) { return ((x / 16) & 1) ? 1.0f : 0.0f; }; }

// 1. lamp shadows
static bool decalLamp(Ctx& c, bool cardIsDecal, int& bad, int& shadowed, int& lit, int& decalPixels) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(3);
	w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });   // floor
	w.meshes[1].quad({ -700, -300, 0 }, { 700, -300, 0 }, { 700, 300, 0 }, { -700, 300, 0 });           // a floor decal (lying on it: z 0)
	w.meshes[2].quad({ -500, -500, 300 }, { 500, -500, 300 }, { 500, 500, 300 }, { -500, 500, 300 });   // the floating card (as cardWorld)
	w.slots[80] = checker8(); w.slots[81] = stripes16();
	Object f; f.mesh = 0; f.id = 1;
	Object fd;
	if (!decalObject(w.meshes[1], 1, V3(), 81, true, { 0.6f, 0.05f, 0.05f }, fd, bad)) return false;
	fd.id = 2;
	Object card;
	if (cardIsDecal) { if (!decalObject(w.meshes[2], 2, V3(), 80, true, { 0.5f, 0.5f, 0.5f }, card, bad)) return false; }
	else { card.mesh = 2; card.alphaTested = true; card.alphaSlot = 80; card.alphaRef = 128; }
	card.id = 3;
	w.objects = { f, fd, card };
	if (!uploadSlot(h, c.alphaAtlas, 80, w.slots[80]) || !uploadSlot(h, c.alphaAtlas, 81, w.slots[81])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const std::vector<Lamp> lamps = { { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 },
		{ V3(0, 0, 900), 0, 0, 0 }, { V3(0, 0, 900), 0, 0, 0 }, { V3(60, -40, 900), 6000, 1.0f, 6000 } };   // slot 5, as alphacard
	const Camera cam = Camera::lookDir({ 0, 0, 2500 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.0f, 2.0f);
	int checked = 0, wrong = 0;
	std::vector<float> res;
	if (!runLampTrace(c, w, ids, lamps, cam, 1, cardIsDecal ? "decal card" : "plain card", checked, wrong, nullptr, &res)) return false;
	shadowed = lit = decalPixels = 0;
	for (UINT i = 0; i < S * S; i++) { const int e0 = int(res[i * 2]) % 1024; if (e0 == 5) shadowed++; else if (e0 == 5 + 64 * 15) lit++; }
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(w, cam, px, depth, normals);
	for (const GPixel& g : px) if (g.valid && w.objects[g.object].decal) decalPixels++;
	say("  lamp, %s: %d pixels checked (%d on edges skipped), %d mismatches; %d shadowed + %d lit by the lamp; %d pixels see a decal\n",
		cardIsDecal ? "the floating card a DECAL" : "the floating card a plain cut-out (control)", checked, int(S * S) - checked, wrong, shadowed, lit, decalPixels);
	bad += wrong;
	if (checked < int(S * S) / 2) { bad++; say("    too few pixels checked\n"); }
	if (wrong) dumpPfm(cardIsDecal ? "decals_lamp_result" : "decals_lamp_control", res, S, S, 2, 0);
	releaseWorld(h, ids);
	return true;
}

// 2. reflections (ReflectionsRTAlpha: the alpha-tested variant, which also sees decals)
static bool decalMirror(Ctx& c, int& bad, int& decalHits, int& wouldShadow, int& sunLit) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(4);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });     // the mirror floor
	w.meshes[1].box({ -900, 900, 0 }, { 900, 1300, 1400 });                                                // a wall block
	w.meshes[2].quad({ -500, 900, 60 }, { 500, 900, 60 }, { 500, 900, 1000 }, { -500, 900, 1000 });        // a poster on its front (y 900), facing -y
	w.meshes[3].quad({ -800, 150, 1250 }, { 800, 150, 1250 }, { 800, 880, 1250 }, { -800, 880, 1250 });    // a floating decal card between the wall and the sun
	w.slots[82] = checker8(); w.slots[83] = [](int, int) { return 1.0f; };
	Object floor; floor.mesh = 0; floor.albedo = { 0.5f, 0.5f, 0.5f }; floor.id = 1;
	Object wall; wall.mesh = 1; wall.albedo = { 0.3f, 0.6f, 0.3f }; wall.id = 2;
	Object poster, sky;
	if (!decalObject(w.meshes[2], 2, V3(), 82, true, { 0.8f, 0.1f, 0.1f }, poster, bad)) return false;
	if (!decalObject(w.meshes[3], 3, V3(), 83, false, { 0.1f, 0.1f, 0.8f }, sky, bad)) return false;
	poster.id = 3; sky.id = 4;
	if (std::fabs(poster.offset.y + 0.5f) > 1e-4f || std::fabs(sky.offset.z - 0.5f) > 1e-4f) {
		bad++; say("    the poster was lifted to y %.3f (expected 899.5: off the wall, toward the camera), the card to z %.3f (1250.5)\n", 900 + poster.offset.y, 1250 + sky.offset.z); }
	w.objects = { floor, wall, poster, sky };
	if (!uploadSlot(h, c.alphaAtlas, 82, w.slots[82]) || !uploadSlot(h, c.alphaAtlas, 83, w.slots[83])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -1600, 700 }, normalize({ 0, 1, -0.45f }), { 0, 0, 1 }, 1.3f, 1.3f);
	const V3 sun = normalize({ 0.25f, -0.35f, 0.9f });
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<GPixel> px(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit || hh.object != 0) continue;
		const V3 v = normalize(d), r = v - hh.n * (2 * dot(v, hh.n));
		octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;   // view z, roughness 0 + 2 (cast)
		GPixel& g = px[j * S + i]; g.valid = true; g.viewZ = hh.t; g.pos = cam.eye + d * hh.t; g.n = hh.n;
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
	ReflConstants k = {};
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.size[2] = 10000.0f; k.size[3] = 1.0f;     // Distance, sun shadow rays (no AlphaSolid)
	k.forward[3] = 0.0f;
	put4(k.sun, sun, 1.0f);
	put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
	// Reflections.cpp's image list with the alpha atlas (binding 6)
	h.AddPass(gShReflAlpha, { c.refDir, c.refRay, c.refHit, c.refShade, c.alphaAtlas }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> hit, shade;
	if (!h.Read(c.refHit, hit) || !h.Read(c.refShade, shade)) return false;
	h.EndFrame();
	int checked = 0, wrong = 0, shown = 0;
	decalHits = wouldShadow = sunLit = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const float* H = &hit[(j * S + i) * 4]; const float* C = &shade[(j * S + i) * 4];
		const GPixel& g = px[j * S + i];
		if (!g.valid) continue;
		bool stable = true; Hit ref; V3 start, rdir;
		for (int s = 0; s < 3; s++) {
			const float off = s == 0 ? 0.0f : s == 1 ? -0.25f : 0.25f;
			const V3 d = cam.ray((i + 0.5f + off) / S, (j + 0.5f + off) / S);
			const Hit fh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!fh.hit || fh.object != 0) { stable = false; break; }
			const V3 surface = d * fh.t;
			const V3 v = normalize(d), r = v - fh.n * (2 * dot(v, fh.n));
			const V3 half = r - normalize(surface);
			const V3 st = surface + half * ((0.5f + fh.t * 0.002f) / length(half));
			const Hit rh = trace(w, cam.eye + st, r, 1.0f + fh.t * 0.0015f, 10000.0f, 0xFF, true);   // the reflection ray: decals SEEN
			if (s == 0) { ref = rh; start = st; rdir = r; }
			else if (rh.hit != ref.hit || rh.object != ref.object) stable = false;
		}
		if (!stable || !ref.hit) continue;
		const V3 at = cam.eye + start + rdir * ref.t;
		float sunVis = 1.0f;
		if (dot(ref.n, sun) > 0) {
			const V3 from = at + ref.n * std::max(1.0f, ref.t * 0.001f);
			sunVis = trace(w, from, sun, 0.5f, 30000.0f, 0xFF, true, true).hit ? 0.0f : 1.0f;   // the shadow ray: decals PASSED
			if (sunVis > 0.5f && trace(w, from, sun, 0.5f, 30000.0f, 0xFF, true, false).hit) wouldShadow++;   // what a decal-blind shader gives
		}
		checked++;
		if (w.objects[ref.object].decal) decalHits++;
		if (sunVis > 0.5f) sunLit++;
		const Object& o = w.objects[ref.object];
		const V3 n = octDecode(H[2], H[3]);
		const bool ok = std::fabs(H[0] - ref.t) <= 0.002f * ref.t + 1.0f && (int(H[1]) & 6) == 6 && dot(n, ref.n) > 0.995f &&
			std::fabs(C[0] - o.albedo.x) < 4e-3f && std::fabs(C[1] - o.albedo.y) < 4e-3f && std::fabs(C[2] - o.albedo.z) < 4e-3f && C[3] == sunVis;
		if (!ok) { wrong++; if (shown++ < 5) say("    pixel (%u, %u): got t %.2f flags %d albedo (%.2f %.2f %.2f) sun %.0f; expected t %.2f object %d albedo (%.2f %.2f %.2f) sun %.0f\n",
			i, j, H[0], int(H[1]), C[0], C[1], C[2], C[3], ref.t, ref.object, o.albedo.x, o.albedo.y, o.albedo.z, sunVis); }
	}
	say("  mirror: %d reflection rays checked, %d mismatches; %d met the poster decal; %d hits sunlit, %d of them through the floating decal card "
		"(a shader that let decals shadow would darken those)\n", checked, wrong, decalHits, sunLit, wouldShadow);
	bad += wrong;
	if (checked < 2000) { bad++; say("    too few reflection rays checked\n"); }
	if (wrong) { dumpPfm("decals_mirror_t", hit, S, S, 4, 0); dumpPfm("decals_mirror_sun", shade, S, S, 4, 3); }
	releaseWorld(h, ids);
	return true;
}

// 3. AO + sky: as sceneAO (rt_host_more.inl), with a decal card and a plain card overhead, AOAlpha 2 or 3
static bool decalAO(Ctx& c, int aoAlpha, int& bad, int& covered, int& open, int& decalWouldCover) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(4);
	w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	w.meshes[1].quad({ -500, -500, 300 }, { 500, -500, 300 }, { 500, 500, 300 }, { -500, 500, 300 });
	w.meshes[2].quad({ -900, 600, 0 }, { 900, 600, 0 }, { 900, 600, 700 }, { -900, 600, 700 });
	w.meshes[3].quad({ -900, 200, 500 }, { 900, 200, 500 }, { 900, 600, 500 }, { -900, 600, 500 });
	w.slots[84] = checker8();
	Object f; f.mesh = 0; f.id = 1;
	Object dc;
	if (!decalObject(w.meshes[1], 1, V3(-600, -900, 0), 84, true, { 0.5f, 0.5f, 0.5f }, dc, bad)) return false;
	dc.id = 2;
	Object pc; pc.mesh = 1; pc.offset = { 650, -900, 0 }; pc.alphaTested = true; pc.alphaSlot = 84; pc.alphaRef = 128; pc.id = 3;
	Object wall; wall.mesh = 2; wall.id = 4; Object roof; roof.mesh = 3; roof.id = 5;
	w.objects = { f, dc, pc, wall, roof };
	if (!uploadSlot(h, c.alphaAtlas, 84, w.slots[84])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -2200, 2200 }, normalize({ 0, 0.85f, -1 }), { 0, 0, 1 }, 1.3f, 1.3f);
	std::vector<float> in(S * S * 2, 0.0f);
	std::vector<GPixel> px(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);   // the camera's view: decals seen
		if (!hh.hit) continue;
		in[(j * S + i) * 2] = hh.t; in[(j * S + i) * 2 + 1] = packAO(hh.n);
		px[j * S + i].valid = true; px[j * S + i].viewZ = hh.t;
	}
	if (!h.Upload(gM.aoIn, in)) return false;
	AOConstants k = {};
	const V3 R = cam.r, U = cam.u, F = cam.f;
	const float m[16] = { R.x, R.y, R.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, 0, 0, 0, 1 };
	std::memcpy(k.viewToWorld, m, sizeof(m));
	put4(k.projection, 1.0f / cam.p00, 1.0f / cam.p11, float(S), float(S));
	put4(k.origin, 0, 0, 0, 0);
	put4(k.ao, 300.0f, 8.0f, 0.0f, 0.0f);
	put4(k.sky, 3000.0f, 16.0f, 0.0f, 0.0f);
	put4(k.gi, 0, 0, 0, 0);
	put4(k.gi2, 0, 0, 0, 1);
	put4(k.gi3, 1, float(aoAlpha), 0, 0);
	h.AddPass(gM.shAO, { gM.aoIn, gM.aoOut, gM.aoGi, c.alphaAtlas, gM.aoLight, gM.albedoAtlas }, 0x6, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> out;
	if (!h.Read(gM.aoOut, out)) return false;
	h.EndFrame();
	// the shader's rays: decals passed; AOAlpha 2 tests the cut-outs, 3 takes them whole
	auto firstHit = [&](V3 start, V3 d, float tMin, float tMax, bool skipDecals) {
		return trace(w, cam.eye + start, d, tMin, tMax, 0xFF, aoAlpha == 2, skipDecals, aoAlpha == 3); };
	int checked = 0, badSky = 0, badAO = 0, shown = 0;
	covered = open = decalWouldCover = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		if (!px[j * S + i].valid) continue;
		const float viewZ = in[(j * S + i) * 2];
		const V3 n = unpackAO(in[(j * S + i) * 2 + 1]);
		const float u = (i + 0.5f) / S, v = (j + 0.5f) / S;
		const V3 view = { (u * 2 - 1) / cam.p00 * viewZ, (1 - v * 2) / cam.p11 * viewZ, viewZ };
		const V3 p = R * view.x + U * view.y + F * view.z;
		const float bias = 0.75f + viewZ * 0.003f;
		bool stable = true;
		const float nx = ign(float(i), float(j)), ny = ign(i + 17.0f, j + 17.0f);
		float occl = 0;
		const V3 start = p + n * bias;
		for (int r = 0; r < 8 && stable; r++) {
			const float ux = fract(nx + fract(r * 0.7548777f)), uy = fract(ny + fract(r * 0.5698403f));
			const V3 d = cosineDir(n, ux, uy);
			const Hit hh = firstHit(start, d, 0.05f * bias, 300.0f, true);
			if (!hh.hit) continue;
			if (hh.t < 6.0f * bias) { stable = false; break; }
			const float x = std::min(std::max((hh.t - 150.0f) / 150.0f, 0.0f), 1.0f);
			occl += 1.0f - x * x * (3 - 2 * x);
		}
		const V3 sstart = p + n * (bias + 8.0f);
		const float cov = firstHit(sstart, { 0, 0, 1 }, 0.0f, 3000.0f, true).hit ? 1.0f : 0.0f;
		if (cov < 0.5f && firstHit(sstart, { 0, 0, 1 }, 0.0f, 3000.0f, false).hit) decalWouldCover++;
		float total = 0.034f * std::min(std::max(n.z, 0.0f), 1.0f), opened = total * (1 - cov);
		const float turn = ((i & 3) * 4.0f + (j & 3)) * (3.14159265f / 4.0f / 16.0f);
		for (int ring = 0; ring < 2; ring++) {
			const float cc = ring == 0 ? 0.5f : 0.8660254f, ss = ring == 0 ? 0.8660254f : 0.5f, rw = ring == 0 ? 0.0324f : 0.0884f;
			for (int q = 0; q < 8; q++) {
				const float a = turn + (q + 0.5f * ring) * (3.14159265f / 4.0f);
				const V3 d = { std::cos(a) * cc, std::sin(a) * cc, ss };
				const float wgt = rw * std::min(std::max(dot(d, n), 0.0f), 1.0f);
				if (wgt <= 0) continue;
				total += wgt;
				// (the ring rays are always traced tested, AOAlpha 2: RtAOTrace.comp's sky loop)
				if (!trace(w, cam.eye + sstart, d, 0.0f, 3000.0f, 0xFF, true, true).hit) opened += wgt;
			}
		}
		const float sky = total > 1e-5f ? opened / total : 1.0f;
		if (!stable) continue;
		checked++;
		const float* o = &out[(j * S + i) * 4];
		const float ao = 1.0f - occl / 8.0f;
		if (cov > 0.5f) covered++; else open++;
		const bool skyOk = std::fabs(o[1] - sky) < 2e-3f && o[2] == cov, aoOk = std::fabs(o[0] - ao) < 2e-3f;
		if (!skyOk) badSky++;
		if (!aoOk) badAO++;
		if ((!skyOk || !aoOk) && shown++ < 5) say("    texel (%u, %u): got ao %.3f sky %.3f covered %.0f, expected ao %.3f sky %.3f covered %.0f\n", i, j, o[0], o[1], o[2], ao, sky, cov);
	}
	const bool ok = checked > int(S * S) / 2 && badSky <= checked / 300 && badAO <= checked / 300;
	say("  AO + sky, AOAlpha %d (%s): %d texels checked, sky wrong at %d, AO wrong at %d; %d covered, %d open, %d open only because the decal card "
		"is passed\n", aoAlpha, aoAlpha == 2 ? "cut-outs tested" : "cut-outs whole, decals passed", checked, badSky, badAO, covered, open, decalWouldCover);
	if (!ok) { bad++; dumpPfm(aoAlpha == 2 ? "decals_ao2_sky" : "decals_ao3_sky", out, S, S, 4, 1); }
	releaseWorld(h, ids);
	return true;
}

// 4. the path tracer (RtPathTrace.comp, DebugView 3: the first hit's direct light from 64 lamps through the lamp image, as pathtex): a
// floor, lamps above, a floating checker card between them. Where the camera sees the floor (every corner of the pixel), the light must
// be the open floor's, value for value, with the card a decal (Visible passes it); as a plain cut-out it shadows (control).
static bool decalPT(Ctx& c, int& bad, int& same, int& shadedByPlain) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(2);
	w.meshes[0].quad({ -1600, -1600, 0 }, { 1600, -1600, 0 }, { 1600, 1600, 0 }, { -1600, 1600, 0 });
	w.meshes[1].quad({ -700, -700, 100 }, { 700, -700, 100 }, { 700, 700, 100 }, { -700, 700, 100 });
	w.slots[85] = checker8();
	if (!uploadSlot(h, c.alphaAtlas, 85, w.slots[85])) return false;
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
	Object dc;
	if (!decalObject(w.meshes[1], 1, V3(), 85, true, { 0.7f, 0.1f, 0.1f }, dc, bad)) return false;
	Object pc = dc; pc.decal = false;   // the same card, not a decal
	w.objects = { f, dc };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 2400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	std::vector<float> image(size_t(kLightTexels) * (3 + kGridRows) * 2, 0.0f);
	const int lamps = 64;
	for (int y = 0, i = 0; y < 8; y++) for (int x = 0; x < 8; x++, i++) {
		const V3 q = V3(-1400 + x * 400.0f, -1400 + y * 400.0f, 150.0f + 10.0f * ((x + y) % 3)) - cam.eye;
		const float bright = 0.6f + 0.1f * (i % 4);
		const float v[8] = { q.x, q.y, q.z, 520.0f + 15.0f * ((x * 3 + y) % 5), bright, bright, bright, 0.0f };
		std::memcpy(&image[size_t(i) * 8], v, sizeof(v));
	}
	if (!h.Upload(gM.ptLamps, image)) return false;
	const uint32_t flags = 1u | 1024u | 2048u;   // alpha test, the lamp image, the game's falloff
	const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.alphaAtlas, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano,
		c.aux16, c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
	auto run = [&](const std::vector<Object>& objects, std::vector<float>& out) {
		World v = w; v.objects = objects;
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 11.0f;
		put4(k.accum, 0.0f, 3.0f, 8.0f, float(flags));
		put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0.3f, 0.4f, 0.5f, 1.0f); put4(k.skyHorizon, 0.3f, 0.4f, 0.5f, 0.0f);
		put4(k.misc, float(lamps), 0.0f, 60000.0f, 3.0f);   // DebugView 3: the first hit's direct light
		k.rect[3] = 1.0f;
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(v, ids, cam.eye)) return false;
		const bool read = h.Read(c.ptDisplay, out);
		h.EndFrame();
		return read;
	};
	std::vector<float> open, withDecal, withPlain;
	if (!run({ f }, open) || !run({ f, dc }, withDecal) || !run({ f, pc }, withPlain)) return false;
	int checked = 0, wrong = 0, shown = 0;
	same = shadedByPlain = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		bool floorOnly = true, under = false;
		for (int s = 0; s < 4 && floorOnly; s++) {
			const V3 d = cam.ray((i + (s & 1 ? 0.98f : 0.02f)) / S, (j + (s & 2 ? 0.98f : 0.02f)) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);   // the decal seen where solid
			if (!hh.hit || hh.object != 0) floorOnly = false;
			const V3 at = cam.eye + d * hh.t;
			if (std::fabs(at.x) < 700 && std::fabs(at.y) < 700) under = true;
		}
		if (!floorOnly) continue;
		checked++;
		float d = 0, dp = 0;
		for (int ch = 0; ch < 3; ch++) { d = std::max(d, std::fabs(open[(j * S + i) * 4 + ch] - withDecal[(j * S + i) * 4 + ch]));
			dp = std::max(dp, std::fabs(open[(j * S + i) * 4 + ch] - withPlain[(j * S + i) * 4 + ch])); }
		if (d <= 1e-4f) { if (under) same++; }
		else { wrong++; if (shown++ < 5) say("    pixel (%u, %u): direct light %.4f open, %.4f with the decal card\n", i, j, open[(j * S + i) * 4], withDecal[(j * S + i) * 4]); }
		if (dp > 1e-3f) shadedByPlain++;
	}
	say("  path tracer (DebugView 3, 64 lamps): %d floor pixels checked, %d differ with the decal card overhead (%d of the same ones under the card); "
		"the same card as a plain cut-out changes %d (control)\n", checked, wrong, same, shadedByPlain);
	bad += wrong;
	if (checked < int(S * S) / 2) { bad++; say("    too few floor pixels\n"); }
	if (wrong) dumpPfm("decals_pt_direct", withDecal, S, S, 4, 0);
	releaseWorld(h, ids);
	return true;
}

static bool sceneDecals(Ctx& c) {
	int bad = 0;
	int shadowed = 0, lit = 0, decalPixels = 0, cShadowed = 0, cLit = 0, cDecal = 0;
	if (!decalLamp(c, true, bad, shadowed, lit, decalPixels)) return false;
	if (!decalLamp(c, false, bad, cShadowed, cLit, cDecal)) return false;
	if (shadowed != 0 || lit < int(S * S) / 2) { bad++; say("    the decal card shadowed %d pixels (expected none)\n", shadowed); }
	if (cShadowed < 500) { bad++; say("    the control (a plain card) shadowed only %d pixels: the test has no teeth\n", cShadowed); }
	if (decalPixels < 1000) { bad++; say("    only %d pixels see a decal: the camera rays must see them\n", decalPixels); }
	int decalHits = 0, wouldShadow = 0, sunLit = 0;
	if (!decalMirror(c, bad, decalHits, wouldShadow, sunLit)) return false;
	if (decalHits < 200) { bad++; say("    only %d reflection rays met the poster: reflections must see decals\n", decalHits); }
	if (wouldShadow < 50) { bad++; say("    only %d sunlit hits behind the floating decal card: the test has no teeth\n", wouldShadow); }
	for (int mode : { 2, 3 }) {
		int covered = 0, open = 0, would = 0;
		if (!decalAO(c, mode, bad, covered, open, would)) return false;
		if (would < 100) { bad++; say("    AOAlpha %d: only %d texels under the decal card: the test has no teeth\n", mode, would); }
		if (covered < 300 || open < 300) { bad++; say("    AOAlpha %d: covered %d / open %d: the plain card and the roof must cover, the rest not\n", mode, covered, open); }
	}
	int ptSame = 0, ptPlain = 0;
	if (!decalPT(c, bad, ptSame, ptPlain)) return false;
	if (ptSame < 500) { bad++; say("    only %d floor pixels under the decal card checked\n", ptSame); }
	if (ptPlain < 500) { bad++; say("    the plain card changed only %d pixels: the test has no teeth\n", ptPlain); }
	say("  decals: %s\n", bad ? "FAILED" : "shadow / AO / visibility rays pass them, reflections and camera rays see them");
	return bad == 0;
}
