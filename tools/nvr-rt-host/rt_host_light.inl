// rt_host_light.inl (2026-11-06): the path tracer's lamp falloff (included by rt_host.cpp after rt_host_more.inl).
// The user saw a hard edge where a lamp's light ends on the demo room's ceiling: the game's falloff (1 - (d/r)^2)^2.2 (PTGameLamps 4) is
// bright almost to the radius, then stops. PTLampFalloff 1 = physical: K / ((d/r)^2 + 0.2^2) x (1 - (d/2r)^4)^2 out to 2r (the game side
// doubles the reach and multiplies the colour by PTLampGain: RtPathTrace.cpp `physical`, mirrored here). Scene: one lamp 150 units over an
// open floor, DebugView 3 (the first hit's direct light), both modes. Checked against the formulas pixel by pixel (the exact range over the
// pixel's footprint), plus: the physical light goes on past the game radius (no rim) and ends at 2r, and at PTLampGain 1 the light the
// floor gets is reported against the game falloff's.

static float lampFallGame(float x) { return x < 1.0f ? std::pow(std::max(1.0f - x * x, 0.0f), 2.2f) : 0.0f; }
static float lampFallPhys(float x2r) {   // x2r = d / (2 r): the doubled reach the shader sees
	const float w = std::max(1.0f - x2r * x2r * x2r * x2r, 0.0f);
	return x2r < 1.0f ? 0.0612f / (4.0f * x2r * x2r + 0.04f) * w * w : 0.0f;
}

static bool sceneFalloff(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.pos = { { -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 } };
	floor.uv = { 0, 0, 1, 0, 1, 1, 0, 1 }; floor.idx = { 0, 1, 2, 0, 2, 3 };
	w.meshes = { floor };
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
	w.objects = { f };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 2600 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.0f);
	const V3 lampAt = { 0, 0, 150 };
	const float gameReach = 900.0f, bright = 1.0f;
	bool ok = true;
	std::vector<float> out[2];
	for (int mode = 0; mode < 2; mode++) {
		// the lamp as RtPathTrace.cpp hands it over: physical = reach x 2, colour x PTLampGain (1 here)
		const float reach = mode ? 2.0f * gameReach : gameReach;
		std::vector<float> image(size_t(kLightTexels) * (1 + kGridRows) * 2, 0.0f);
		const V3 q = lampAt - cam.eye;
		const float v[8] = { q.x, q.y, q.z, reach, bright, bright, bright, 0.0f };
		std::memcpy(image.data(), v, sizeof(v));
		if (!h.Upload(gM.ptLamps, image)) return false;
		PathConstants k = PathConstants();
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 5.0f;
		const uint32_t flags = 1024u | 2048u | (mode ? 131072u : 0u);
		put4(k.accum, 0.0f, 3.0f, 4.0f, float(flags));
		put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0, 0, 0, 1.0f); put4(k.skyHorizon, 0, 0, 0, 0.0f);
		put4(k.misc, 1.0f, 0.0f, 60000.0f, 3.0f);   // 1 lamp, DebugView 3 (the first hit's direct light)
		k.rect[3] = 1.0f;
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
			c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (!h.Read(c.ptDisplay, out[mode])) return false;
		h.EndFrame();
	}
	// pixel by pixel against the formula (albedo 0.5 / pi x pi: the shader's diffuse x the light; checked as a ratio of the centre's)
	int checked[2] = {}, bad[2] = {}, shown = 0, tail = 0, beyond = 0;
	double floorLight[2] = {};
	const float centreRef[2] = { lampFallGame(150.0f / gameReach), lampFallPhys(150.0f / (2.0f * gameReach)) };   // the floor under the lamp: 150 units, cos 1
	const UINT ci = S / 2, cj = S / 2;
	const float centreGot[2] = { out[0][(cj * S + ci) * 4], out[1][(cj * S + ci) * 4] };
	for (int mode = 0; mode < 2; mode++) for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		float lo = 1e30f, hi = -1e30f;
		for (int s = 0; s < 5; s++) {
			const V3 d = cam.ray((i + (s == 0 ? 0.5f : (s & 1 ? 0.95f : 0.05f))) / S, (j + (s == 0 ? 0.5f : (s & 2 ? 0.95f : 0.05f))) / S);
			const float t = -cam.eye.z / d.z;
			const V3 at = cam.eye + d * t, L = lampAt - at;
			const float dist = length(L), cosT = std::max(L.z / dist, 0.0f);
			const float val = (mode ? lampFallPhys(dist / (2.0f * gameReach)) : lampFallGame(dist / gameReach)) * cosT;
			lo = std::min(lo, val); hi = std::max(hi, val);
		}
		const float got = out[mode][(j * S + i) * 4] / centreGot[mode] * centreRef[mode];   // normalised to the formula's units
		floorLight[mode] += out[mode][(j * S + i) * 4];
		checked[mode]++;
		if (!(got >= lo * 0.99f - 1e-4f && got <= hi * 1.01f + 1e-4f)) { bad[mode]++; if (shown++ < 4) say("    %s pixel (%u, %u): %.5f, exact %.5f .. %.5f\n", mode ? "physical" : "game", i, j, got, lo, hi); }
		if (mode == 1) {
			const float dx = (float(i) + 0.5f - S / 2.0f) / (S / 2.0f) * 2600.0f, dy = (float(j) + 0.5f - S / 2.0f) / (S / 2.0f) * 2600.0f;
			const float r = std::sqrt(dx * dx + dy * dy + 150.0f * 150.0f);
			if (r > gameReach * 1.1f && r < gameReach * 1.8f && out[1][(j * S + i) * 4] > 0.0f) tail++;
			if (r > gameReach * 2.05f && out[1][(j * S + i) * 4] > 0.0f) beyond++;
		}
	}
	const float ratio = floorLight[0] > 0 ? float(floorLight[1] / floorLight[0]) : 0.0f;
	ok = bad[0] <= checked[0] / 500 && bad[1] <= checked[1] / 500 && tail > 1000 && beyond == 0;
	say("  game falloff: %d of %d pixels in the exact range; physical: %d of %d; physical light past the game radius at %d pixels (none past 2r: %d); "
		"floor light physical / game at PTLampGain 1: %.2f -> %s\n", checked[0] - bad[0], checked[0], checked[1] - bad[1], checked[1], tail, beyond, ratio, ok ? "ok" : "BAD");
	if (!ok) { dumpPfm("falloff_game", out[0], S, S, 4, 0); dumpPfm("falloff_phys", out[1], S, S, 4, 0); }
	releaseWorld(h, ids);
	return ok;
}
