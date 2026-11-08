// (2026-11-07 rtgap-tess) Scenes `tessslack` and `contact`: docs/next/RT-DETAIL-GAPS.md section 1 (the RT scene has the flat triangles,
// the depth buffer the tessellated bumps and everything else the game draws). The constants are packed by the game's own
// src/core/RtTessContact.h; the shaders share Compute/Rt/RtContact.glsl.
//
// tessslack (1a): the TLAS holds a flat floor, the G-buffer a bump field round it (+-10 units: Strength 20, Center 0.5, as the land's
//   tessellation draws it), the sun / one lamp above, nothing else in the scene. Every ray toward the light is free in truth, so a pixel
//   that comes out dark ("speckle") had its ray start under the flat RT floor. Four passes, slack 0 (as before) vs 20 (the fix):
//   RtPathTrace (PTPrimary's G-buffer, DebugView 3: the first hit's direct sun), RtSunShadow, RtLampTrace, RtAOTrace (its "covered" =
//   the straight-up sky ray blocked, and "inside" = a ray that met a back face at its start).
// contact (1b): the G-buffer holds a floor and a small step (a box 60 x 60 x 10) that the TLAS lacks (as grass, small props or the bumps
//   are missing from the RT scene). With the contact march on, the floor next to the step on its far side from the light must darken
//   (where the CPU's ray from it meets the step within half the march); with it off nothing does (the TLAS has no step). False
//   positives: floor and step pixels whose CPU ray is free must not darken (at most 1 %). And a flat floor alone, seen from above and at
//   a grazing angle: on = off at every pixel (the march's own-plane test). RtSunShadow, RtLampTrace, RtPathTrace (sun, then one lamp).

struct TessImages { int sunVis = -1, sunHit = -1; };
static TessImages gT;
static bool registerTess(Host& h) {
	gT.sunVis = h.AddImage("tess sun visibility", S, S, NVR_EXT_SHARED_R16F);
	gT.sunHit = h.AddImage("tess sun blocker", S, S, NVR_EXT_SHARED_R16F);
	return gT.sunVis >= 0 && gT.sunHit >= 0;
}

// The bump field the tessellation would draw: h = A sin(2 pi x / L) sin(2 pi y / L), analytic normal
struct BumpField {
	float A = 10.0f, L = 96.0f;
	float h(float x, float y) const { const float k = 6.2831853f / L; return A * std::sin(k * x) * std::sin(k * y); }
	V3 n(float x, float y) const {
		const float k = 6.2831853f / L;
		return normalize({ -A * k * std::cos(k * x) * std::sin(k * y), -A * k * std::sin(k * x) * std::cos(k * y), 1.0f });
	}
};
// The G-buffer of the bump field (as gbuffer() does it for a mesh world): march the camera ray between z = +A and -A, bisect the crossing
static void gbufferBumps(const BumpField& b, const Camera& cam, std::vector<GPixel>& px, std::vector<float>& depth, std::vector<float>& normals) {
	px.assign(S * S, GPixel()); depth.assign(S * S, 1.0f); normals.assign(S * S * 4, 0.5f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		if (!(d.z < -1e-4f)) continue;
		const float s0 = (cam.eye.z - b.A - 0.01f) / -d.z, s1 = (cam.eye.z + b.A + 0.01f) / -d.z;
		auto f = [&](float s) { const V3 q = cam.eye + d * s; return q.z - b.h(q.x, q.y); };
		float lo = s0, hi = -1.0f;
		const float step = 0.05f;
		for (float s = s0; s <= s1; s += step) { if (f(s) <= 0.0f) { hi = s; break; } lo = s; }
		if (hi < 0.0f) continue;
		for (int it = 0; it < 40; it++) { const float m = 0.5f * (lo + hi); if (f(m) > 0.0f) lo = m; else hi = m; }
		const float s = 0.5f * (lo + hi);
		const V3 q = cam.eye + d * s, nn = b.n(q.x, q.y);
		GPixel& g = px[j * S + i];
		g.valid = true; g.viewZ = s; g.pos = q; g.n = nn; g.object = 0;
		depth[j * S + i] = s / cam.farZ;
		float* n = &normals[(j * S + i) * 4];
		n[0] = dot(nn, cam.r) * 0.5f + 0.5f; n[1] = dot(nn, cam.u) * 0.5f + 0.5f; n[2] = dot(nn, cam.f) * 0.5f + 0.5f; n[3] = 1.0f;
	}
}

static World tessFloor() {
	World w;
	Mesh floor; floor.quad({ -6000, -6000, 0 }, { 6000, -6000, 0 }, { 6000, 6000, 0 }, { -6000, 6000, 0 });
	w.meshes.push_back(floor);
	Object o; o.mesh = 0; o.land = true; o.albedo = { 0.5f, 0.5f, 0.5f }; o.id = 1;
	w.objects.push_back(o);
	return w;
}

// ---- the passes, filled as the game's C++ fills them (the slack / contact vec4s by RtTessContact.h)
static bool tessSun(Ctx& c, const World& tlas, const std::vector<uint32_t>& ids, const Camera& cam, V3 sun, const RtTessContact::Slack& slack,
	const RtTessContact::Contact& contact, std::vector<float>& vis) {
	Host& h = *c.host;
	SunConstants k = {};
	put4(k.size, float(S), float(S), 1.0f / S, 1.0f / S);
	put4(k.forward, cam.f, cam.farZ); put4(k.right, cam.r * (1.0f / cam.p00), 0); put4(k.up, cam.u * (1.0f / cam.p11), 0);
	put4(k.sun, sun, 1.0f);
	put4(k.bias, 2.0f, 0.0005f, 1.0f, 30000.0f);      // the defaults: NormalBias, NormalBiasPerUnit, StartBias, RayLength
	put4(k.range, 100000.0f, 1.0f, 255.0f, 0.0f);
	put4(k.alpha, 0.0f, 3.0f, 0.0f, 0.0f);            // no alpha test, frame 3, hard, no blocker image
	put4(k.full, float(S), float(S), 0.0f, 0.0f);
	put4(k.camera, cam.eye, 0.0f);
	RtTessContact::SlackVec(slack, k.slack);
	RtTessContact::ContactVecs(contact, k.contact, k.contactGate);
	h.AddPass(gM.shSun, { c.depth, c.normals, gT.sunVis, c.alphaStandIn, c.normals, gT.sunHit }, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(tlas, ids, cam.eye)) return false;
	if (!h.Read(gT.sunVis, vis)) return false;
	h.EndFrame();
	return true;
}
// RtLampTrace: one lamp (slot 0); out: per pixel the lamp's visibility steps (0-15, -1 = not ranked / no surface)
static bool tessLamp(Ctx& c, const World& tlas, const std::vector<uint32_t>& ids, const Camera& cam, V3 lampPos, const RtTessContact::Slack& slack,
	const RtTessContact::Contact& contact, std::vector<int>& steps) {
	Host& h = *c.host;
	std::vector<Lamp> lamps(1);
	lamps[0].pos = lampPos; lamps[0].reach = 3000; lamps[0].intensity = 1; lamps[0].radius = 3000;
	if (!h.Upload(c.lampTable, lampTable(lamps, cam.eye), 0, 0, 1024, 1)) return false;
	LampConstants k; lampPassConstants(k, cam, 1, 1);
	put4(k.jitter, 0.0f, 0.0f, 0.0f, 0.0f);   // no alpha test (binding 12: a stand-in)
	RtTessContact::SlackVec(slack, k.slack);
	RtTessContact::ContactVecs(contact, k.contact, k.contactGate);
	const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn };
	h.AddPass(c.shLamp, imgs, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(tlas, ids, cam.eye)) return false;
	std::vector<float> res;
	if (!h.Read(c.lampResult, res)) return false;
	h.EndFrame();
	steps.assign(S * S, -1);
	for (UINT p = 0; p < S * S; p++) {
		const int e0 = int(res[p * 2]) % 1024;
		if (e0 != 1023 && e0 % 64 == 0) steps[p] = e0 / 64;
	}
	return true;
}
// RtPathTrace, PTPrimary's G-buffer (flag 8) + the lamp image (1024; the frame row carries the slack and the contact), DebugView 3: the
// first hit's direct light alone. sunOn: the sun (else none); lamp: one lamp of colour 1 (lampOn)
static bool tessPt(Ctx& c, const World& tlas, const std::vector<uint32_t>& ids, const Camera& cam, const std::vector<GPixel>& px, V3 sun, bool sunOn,
	bool lampOn, V3 lampPos, const RtTessContact::Slack& slack, const RtTessContact::Contact& contact, std::vector<float>& disp) {
	Host& h = *c.host;
	std::vector<float> albedo(S * S * 4, 0.0f);
	for (UINT p = 0; p < S * S; p++) if (px[p].valid) { albedo[p * 4] = albedo[p * 4 + 1] = albedo[p * 4 + 2] = 0.5f; albedo[p * 4 + 3] = 1.0f; }
	if (!h.Upload(c.aux16, albedo)) return false;
	std::vector<float> image(size_t(kLightTexels) * (3 + kGridRows) * 2, 0.0f);
	if (lampOn) { const V3 q = lampPos - cam.eye; const float v[8] = { q.x, q.y, q.z, 3000.0f, 1.0f, 1.0f, 1.0f, 0.0f }; std::memcpy(image.data(), v, sizeof(v)); }
	RtTessContact::PackRow(&image[size_t(kLightTexels) * (1 + kGridRows) * 2], slack, contact);   // the frame row (49)
	if (!h.Upload(gM.ptLamps, image)) return false;
	PathConstants k = {};
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.origin[3] = 29.0f;
	put4(k.accum, 0.0f, 1.0f, 8.0f, float(8u | 1024u | 2048u));   // fresh mean, 1 bounce, 8 paths; the G-buffer, the lamp image, the game's falloff
	put4(k.sunDir, sun, sunOn ? 1.0f : 0.0f);
	put4(k.sunColor, 1.0f, 1.0f, 1.0f, 0.0f);   // a hard sun
	put4(k.skyZenith, 0.0f, 0.0f, 0.0f, 1.0f); put4(k.skyHorizon, 0.0f, 0.0f, 0.0f, 0.0f);
	put4(k.misc, lampOn ? 1.0f : 0.0f, 0.0f, 60000.0f, 3.0f);
	k.rect[3] = 1.0f;
	const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano,
		c.aux16, c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
	h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(tlas, ids, cam.eye)) return false;
	if (!h.Read(c.ptDisplay, disp)) return false;
	h.EndFrame();
	return true;
}

// ==============================================================================================================================
static bool sceneTessSlack(Ctx& c) {
	Host& h = *c.host;
	const World tlas = tessFloor();
	std::vector<uint32_t> ids;
	if (!stageWorld(h, tlas, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -170, 120 }, normalize({ 0, 1, -0.75f }), { 0, 0, 1 }, 1.0f, 1.0f);
	const BumpField bumps;
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbufferBumps(bumps, cam, px, depth, normals);
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
	int below = 0, valid = 0;
	for (UINT p = 0; p < S * S; p++) if (px[p].valid) { valid++; if (px[p].pos.z < -1.0f) below++; }
	say("  bump field +-%.0f (Strength %.0f, Center 0.5) under a flat RT floor: %d pixels, %d of them more than 1 unit under the RT floor\n", bumps.A, 2 * bumps.A, valid, below);
	const V3 sun = normalize({ 0.35f, -0.25f, 0.9f });
	const V3 lamp = { 30, 140, 260 };
	RtTessContact::Slack none, on; on.strength = 2 * bumps.A; on.fadeStart = 1024; on.fadeEnd = 2048;
	const RtTessContact::Contact off;   // the contact march off: only the start points are tested here
	bool ok = valid > int(S * S) / 2 && below > 1000;
	double share[4][2] = {};
	for (int fix = 0; fix < 2; fix++) {
		const RtTessContact::Slack& sl = fix ? on : none;
		// the path tracer: E = albedo x max(n . sun, 0) with the screen's normal; dark = under half of it
		std::vector<float> disp;
		if (!tessPt(c, tlas, ids, cam, px, sun, true, false, {}, sl, off, disp)) return false;
		int dark = 0, n = 0; double worst = 0;
		for (UINT p = 0; p < S * S; p++) {
			if (!px[p].valid) continue;
			const float* nn = &normals[p * 4];
			const V3 ns = normalize(cam.r * (nn[0] * 2 - 1) + cam.u * (nn[1] * 2 - 1) + cam.f * (nn[2] * 2 - 1));
			const float E = 0.5f * std::max(dot(ns, sun), 0.0f);
			if (E < 0.05f) continue;
			n++;
			const float got = disp[p * 4];
			if (got < 0.5f * E) dark++; else worst = std::max(worst, double(std::fabs(got - E) / E));
		}
		share[0][fix] = n ? double(dark) / n : 1.0;
		say("    path tracer (DebugView 3, sun), slack %2.0f: %5d of %5d lit pixels dark (%.2f %%), the others off by %.2f %% at worst\n", sl.strength, dark, n, 100 * share[0][fix], 100 * worst);
		if (fix && worst > 0.03) ok = false;
		// RT sun shadows
		std::vector<float> vis;
		if (!tessSun(c, tlas, ids, cam, sun, sl, off, vis)) return false;
		dark = n = 0;
		for (UINT p = 0; p < S * S; p++) { if (!px[p].valid || dot(px[p].n, sun) <= 0.05f) continue; n++; if (vis[p] < 0.5f) dark++; }
		share[1][fix] = n ? double(dark) / n : 1.0;
		say("    RT sun (NormalBias 2), slack %2.0f: %5d of %5d sunlit pixels shadowed (%.2f %%)\n", sl.strength, dark, n, 100 * share[1][fix]);
		// RT lamp shadows
		std::vector<int> st;
		if (!tessLamp(c, tlas, ids, cam, lamp, sl, off, st)) return false;
		dark = n = 0;
		for (UINT p = 0; p < S * S; p++) { if (!px[p].valid || st[p] < 0 || dot(px[p].n, normalize(lamp - px[p].pos)) <= 0.05f) continue; n++; if (st[p] < 8) dark++; }
		share[2][fix] = n ? double(dark) / n : 1.0;
		say("    RT lamp (StartOffset 0.5), slack %2.0f: %5d of %5d lit pixels shadowed (%.2f %%)\n", sl.strength, dark, n, 100 * share[2][fix]);
		// RT AO + sky (the plain pass): covered (the straight-up ray blocked: nothing is above) or a ray that began inside the floor
		{
			std::vector<float> in(S * S * 2, 0.0f);
			for (UINT p = 0; p < S * S; p++) if (px[p].valid) { in[p * 2] = px[p].viewZ; in[p * 2 + 1] = packAO(px[p].n); }
			if (!h.Upload(gM.aoIn, in)) return false;
			AOConstants k = {};
			const V3 R = cam.r, U = cam.u, F = cam.f;
			const float m[16] = { R.x, R.y, R.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, 0, 0, 0, 1 };
			std::memcpy(k.viewToWorld, m, sizeof(m));
			float sv[4]; RtTessContact::SlackVec(sl, sv);
			k.viewToWorld[12] = sv[0]; k.viewToWorld[13] = sv[1]; k.viewToWorld[14] = sv[2];   // as AmbientOcclusion.cpp: _41.._43
			put4(k.projection, 1.0f / cam.p00, 1.0f / cam.p11, float(S), float(S));
			put4(k.ao, 300.0f, 8.0f, 0.0f, 0.0f); put4(k.sky, 3000.0f, 16.0f, 0.0f, 0.0f);
			put4(k.gi2, 0, 0, 0, 1); put4(k.gi3, 1, 1, 0, 0);
			h.AddPass(gM.shAO, { gM.aoIn, gM.aoOut, gM.aoGi, c.alphaStandIn, gM.aoLight, gM.albedoAtlas }, 0x6, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(tlas, ids, cam.eye)) return false;
			std::vector<float> out;
			if (!h.Read(gM.aoOut, out)) return false;
			h.EndFrame();
			int covered = 0, inside = 0; n = 0;
			for (UINT p = 0; p < S * S; p++) { if (!px[p].valid) continue; n++; if (out[p * 4 + 2] > 0.5f) covered++; if (out[p * 4 + 3] == 0.0f) inside++; }
			share[3][fix] = n ? double(covered + inside) / n : 1.0;
			say("    RT AO + sky, slack %2.0f: of %5d texels %d covered (sky ray blocked), %d with a ray started inside the floor\n", sl.strength, n, covered, inside);
		}
	}
	const char* names[4] = { "path tracer", "sun", "lamp", "AO" };
	for (int i = 0; i < 4; i++) {
		const bool pass = share[i][1] <= 0.002 && share[i][0] >= 0.01;   // fixed: no speckles; before: the bench sees them
		if (!pass) { ok = false; say("    %s: %.2f %% -> %.2f %% -> BAD\n", names[i], 100 * share[i][0], 100 * share[i][1]); }
	}
	say("  speckles (rays started under the flat RT floor), before -> after: path tracer %.2f %% -> %.2f %%, sun %.2f %% -> %.2f %%, lamp %.2f %% -> %.2f %%, AO %.2f %% -> %.2f %%\n",
		100 * share[0][0], 100 * share[0][1], 100 * share[1][0], 100 * share[1][1], 100 * share[2][0], 100 * share[2][1], 100 * share[3][0], 100 * share[3][1]);
	releaseWorld(h, ids);
	return ok;
}

// ==============================================================================================================================
static bool sceneContact(Ctx& c) {
	Host& h = *c.host;
	const World tlas = tessFloor();
	std::vector<uint32_t> ids;
	if (!stageWorld(h, tlas, ids)) return false;
	World screen = tessFloor();   // the G-buffer's world: the floor + a step the TLAS does not have
	{ Mesh step; step.box({ -30, -30, 0 }, { 30, 30, 10 }); screen.meshes.push_back(step); Object o; o.mesh = 1; o.id = 2; screen.objects.push_back(o); }
	const Camera cam = Camera::lookDir({ 0, -150, 130 }, normalize({ 0, 1, -0.87f }), { 0, 0, 1 }, 1.2f, 1.2f);
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(screen, cam, px, depth, normals);
	const RtTessContact::Slack none;
	const RtTessContact::Contact on = RtTessContact::Make(1.0f, 24.0f), off;
	const V3 sun = normalize({ 1.0f, 0.25f, 0.42f });
	const V3 lamp = { 110, 20, 28 };
	bool ok = true;
	// the CPU's answer per pixel (the screen's world): blocked within `near` units of the march (in the shadow the march must see) / free
	auto classify = [&](const GPixel& g, V3 toward, float lightDist, float nearDist, int& cls) {
		cls = 0;   // 0 skip, 1 blocked near, 2 free
		if (!g.valid || dot(g.n, toward) <= 0.05f) return;
		const Hit hh = trace(screen, g.pos + g.n * 0.05f, toward, 0.0f, lightDist, 0xFF, false);
		if (!hh.hit) { cls = 2; return; }
		if (hh.t < nearDist) cls = 1;
	};
	// the march's reach at a pixel: min(24 pixels, 64 units)
	auto reach = [&](const GPixel& g) { return std::min(24.0f * 2.0f * g.viewZ / (cam.p11 * S), 64.0f); };
	struct Res { int blockedDark = 0, blocked = 0, freeDark = 0, free = 0, offDark = 0; };
	auto report = [&](const char* name, const Res& r) {
		const double hit = r.blocked ? double(r.blockedDark) / r.blocked : 0, fp = r.free ? double(r.freeDark) / r.free : 1;
		const bool pass = r.blocked > 40 && hit >= 0.7 && fp <= 0.01 && r.offDark == 0;
		say("    %s: contact on: %d of %d pixels in the step's near shadow dark (%.1f %%), %d of %d free pixels darkened (%.2f %%); off: %d of the shadow dark -> %s\n",
			name, r.blockedDark, r.blocked, 100 * hit, r.freeDark, r.free, 100 * fp, r.offDark, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	};
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
	// RT sun
	{
		std::vector<float> vOn, vOff;
		if (!tessSun(c, tlas, ids, cam, sun, none, on, vOn) || !tessSun(c, tlas, ids, cam, sun, none, off, vOff)) return false;
		Res r;
		for (UINT p = 0; p < S * S; p++) {
			int cls; classify(px[p], sun, 30000.0f, 0.5f * reach(px[p]), cls);
			if (cls == 1) { r.blocked++; if (vOn[p] < 0.5f) r.blockedDark++; if (vOff[p] < 0.5f) r.offDark++; }
			if (cls == 2) { r.free++; if (vOn[p] < 0.9f) r.freeDark++; }
		}
		report("RT sun", r);
	}
	// RT lamp
	{
		std::vector<int> sOn, sOff;
		if (!tessLamp(c, tlas, ids, cam, lamp, none, on, sOn) || !tessLamp(c, tlas, ids, cam, lamp, none, off, sOff)) return false;
		Res r;
		for (UINT p = 0; p < S * S; p++) {
			if (sOff[p] < 0) continue;
			const V3 to = lamp - px[p].pos;
			int cls; classify(px[p], normalize(to), length(to) - 1.0f, 0.5f * reach(px[p]), cls);
			if (cls == 1) { r.blocked++; if (sOn[p] < 8) r.blockedDark++; if (sOff[p] < 8) r.offDark++; }
			if (cls == 2) { r.free++; if (sOn[p] < 14) r.freeDark++; }
		}
		report("RT lamp", r);
	}
	// path tracer: the sun, then the lamp
	for (int which = 0; which < 2; which++) {
		std::vector<float> dOn, dOff;
		if (!tessPt(c, tlas, ids, cam, px, sun, which == 0, which == 1, lamp, none, on, dOn) || !tessPt(c, tlas, ids, cam, px, sun, which == 0, which == 1, lamp, none, off, dOff)) return false;
		Res r;
		for (UINT p = 0; p < S * S; p++) {
			const V3 to = which ? normalize(lamp - px[p].pos) : sun;
			const float dist = which ? length(lamp - px[p].pos) - 1.0f : 30000.0f;
			int cls; classify(px[p], to, dist, 0.5f * reach(px[p]), cls);
			const float a = dOn[p * 4], b = dOff[p * 4];
			if (!(b > 1e-3f)) continue;
			if (cls == 1) { r.blocked++; if (a < 0.5f * b) r.blockedDark++; }
			if (cls == 2) { r.free++; if (a < 0.97f * b) r.freeDark++; }
		}
		report(which ? "path tracer, lamp" : "path tracer, sun", r);
	}
	// a flat floor alone (from above as before, then at a grazing angle): the march must never darken it
	const Camera graze = Camera::lookDir({ 0, -400, 40 }, normalize({ 0, 1, -0.05f }), { 0, 0, 1 }, 1.2f, 1.2f);
	for (int view = 0; view < 2; view++) {
		const Camera& cv = view ? graze : cam;
		gbuffer(tlas, cv, px, depth, normals);
		if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
		std::vector<float> vOn, vOff, dOn, dOff; std::vector<int> sOn, sOff;
		const V3 lampF = { 60, view ? 300.0f : 20.0f, 30 };
		if (!tessSun(c, tlas, ids, cv, sun, none, on, vOn) || !tessSun(c, tlas, ids, cv, sun, none, off, vOff) ||
			!tessLamp(c, tlas, ids, cv, lampF, none, on, sOn) || !tessLamp(c, tlas, ids, cv, lampF, none, off, sOff) ||
			!tessPt(c, tlas, ids, cv, px, sun, true, false, lampF, none, on, dOn) || !tessPt(c, tlas, ids, cv, px, sun, true, false, lampF, none, off, dOff)) return false;
		int n = 0, diffSun = 0, diffLamp = 0, diffPt = 0;
		for (UINT p = 0; p < S * S; p++) {
			if (!px[p].valid) continue;
			n++;
			if (vOn[p] != vOff[p]) diffSun++;
			if (sOn[p] != sOff[p]) diffLamp++;
			if (dOn[p * 4] < 0.999f * dOff[p * 4]) diffPt++;
		}
		const bool pass = n > int(S * S) / 3 && diffSun == 0 && diffLamp == 0 && diffPt == 0;
		say("    a flat floor alone (%s): contact on vs off differ at %d (sun), %d (lamp), %d (path tracer) of %d pixels -> %s\n", view ? "grazing view" : "from above",
			diffSun, diffLamp, diffPt, n, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	}
	releaseWorld(h, ids);
	return ok;
}
