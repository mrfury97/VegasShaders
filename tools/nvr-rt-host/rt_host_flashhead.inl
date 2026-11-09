// ==============================================================================================================================
// Scene `flashhead` (2026-11-07 perf-flashlight; docs/next/RT-PATH-TRACING.md "Team perf-flashlight"). A lamp carried INSIDE the player's
// hidden first-person head (the Pip-Boy light as a plain game lamp, when the Flashlight effect does not drive it) under the SAME-FRAME lamp
// shadows a 4090 tester ran with test 10 ([Shaders.ShadowsInteriors.RT] SameFrame + ShadowDenoiser + RtLampEye, FirstPersonBody on):
// RtLampPrimary (this frame's camera rays) then RtLampTrace, RtLampTraceSigma (the denoiser's signal) and RtLampEye (the lamps as the eye
// sees them), in the exchanges the game records. The world: a floor, a back wall, a crate in front of the player; the hidden body of the
// ptaaa-firstperson scenes (head box round the eye, torso, legs, a third-person gun). Lamp 0 in the head; lamp 1 low behind the crate
// (short reach), which the eye cannot see (RtLampEye's control).
// Three TLAS variants (rt_host_firstperson.inl bodyVariant): body hidden (first person), absent, visible (no custom index).
// PASS:
//   hidden = absent, bit for bit: the camera rays' depth + normals, RtLampTrace's packed result, the Sigma signal, RtLampEye's 256 values
//   (before ptaaa-firstperson the hidden head blocked every ray toward lamp 0: the floor in front dark but for the gaps of the mesh);
//   absent vs the CPU (exact rays, same triangles): every stable pixel's lamp entry and depth code; lamp 0 lights the floor (> 3000 pixels);
//   control: the body VISIBLE (the G-buffer of the absent run uploaded: camera rays from inside a visible head see only the head) blocks
//   lamp 0 at >= 99 % of the pixels it lights without the body (the scene would catch a pass-over that stopped working), Sigma's signal too;
//   RtLampEye: lamp 0 = 1 in every variant (people are left out of its rays), lamp 1 = 0 (the crate), empty entries 1.

struct FlashHeadCtx { int eye = -1, shEye = -1; };
static FlashHeadCtx gFH;
static bool registerFlashHead(Host& h, const std::string& spv) {
	gFH.eye = h.AddImage("lamps from the eye", 256, 1, NVR_EXT_SHARED_R32F);   // RtLampShadows.h imageEye
	gFH.shEye = h.AddShader(spv + "\\RtLampEye.spv");
	return gFH.eye >= 0;
}

static bool sceneFlashHead(Ctx& c) {
	Host& h = *c.host;
	if (gShPrimary < 0 || gFH.shEye < 0 || gS.shSigma < 0) { say("  error: RtLampPrimary / RtLampEye / RtLampTraceSigma not registered\n"); return false; }
	const V3 eye = { 0, 0, 140 };
	World w;
	w.meshes.resize(3);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });   // floor
	w.meshes[1].quad({ -900, 520, 0 }, { -900, 520, 420 }, { 900, 520, 420 }, { 900, 520, 0 });       // back wall
	w.meshes[2].box({ -40, 170, 0 }, { 40, 230, 55 });                                                 // the crate
	Object floor; floor.mesh = 0; floor.land = true; floor.id = 1;
	Object wall; wall.mesh = 1; wall.id = 2;
	Object crate; crate.mesh = 2; crate.id = 3;
	const std::vector<Object> plain = { floor, wall, crate };
	const FpBody body = addBody(w, eye, eye.z - 30);
	w.objects = plain;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, withObjects(w, bodyVariant(plain, body, 2)), ids)) return false;
	const Camera cam = Camera::lookDir(eye, { 0.05f, 1, -0.6f }, { 0, 0, 1 }, 1.0f, 1.0f);

	std::vector<Lamp> lamps(2);
	lamps[0].pos = eye + V3(0, 4, -6); lamps[0].reach = 3000; lamps[0].intensity = 1; lamps[0].radius = 3000;   // in the head
	lamps[1].pos = { 0, 260, 8 }; lamps[1].reach = 150; lamps[1].intensity = 1; lamps[1].radius = 150;          // behind the crate
	if (!h.Upload(c.lampTable, lampTable(lamps, eye), 0, 0, 1024, 1)) return false;
	const uint32_t shadowMask = 0x63;   // land + objects + people (0x20 actors, 0x40 the player's parts), as the game's farMask keeps them

	LampConstants k; lampPassConstants(k, cam, int(lamps.size()), 1);
	put4(k.params2, 0.0f, float(shadowMask), float(shadowMask), float(lamps.size()));
	put4(k.jitter, 0.0f, 0.0f, 0.0f, 0.0f);   // no jitter, no alpha test
	LampConstants ks = k;                      // RtLampTraceSigma: one layer, lamp 0 (RtLampShadows.h: many = layers / frame, reproject = their slots)
	put4(ks.many, 1.0f, 0.0f, 0.0f, 0.0f);
	for (int q = 0; q < 8; q++) ks.reproject[q / 4][q % 4] = q == 0 ? 0.0f : -1.0f;
	LampConstants ke = k;                      // RtLampEye: the trace's own block, 256 entries (RtLampShadows.h e = c)
	ke.size[0] = 256.0f; ke.size[1] = 1.0f;
	PrimaryConstantsHost pc = {};
	cameraConstants(cam, pc.size, pc.forward, pc.right, pc.up, pc.origin);
	put4(pc.jitter, 0.0f, 0.0f, 0.0f, 0.0f);
	put4(pc.masks, float(0xFFu & ~0x60u), float(0x60u), 0.0f, 0.0f);
	const std::vector<int> primaryImgs = { c.depth, c.normals, c.depth, c.depth, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn };
	const std::vector<int> lampImgs = { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn };
	const std::vector<int> sigmaImgs = { c.depth, c.normals, c.lampResult, c.lampTable, gS.signal, c.depth, c.depth, c.depth, c.alphaStandIn };
	const std::vector<int> eyeImgs = { c.depth, c.normals, c.lampResult, c.lampTable, gFH.eye, c.depth, c.depth, c.depth, c.alphaStandIn };

	struct Run { std::vector<float> depth, normals, result, signal, eyeVis; };
	Run r[3];
	for (int v = 0; v < 3; v++) {
		const World tw = withObjects(w, bodyVariant(plain, body, v));
		const bool sameFrame = v != 2;   // the visible control: the absent run's G-buffer (camera rays in a visible head see the head)
		if (!sameFrame && (!h.Upload(c.depth, r[1].depth) || !h.Upload(c.normals, r[1].normals))) return false;
		// exchange 1: [RtLampPrimary] + RtLampTrace + RtLampEye (RtLampShadows.h's order)
		if (sameFrame) h.AddPass(gShPrimary, primaryImgs, 0x3, &pc, sizeof(pc), (S + 7) / 8, (S + 7) / 8);
		h.AddPass(c.shLamp, lampImgs, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		h.AddPass(gFH.shEye, eyeImgs, 0x10, &ke, sizeof(ke), 4, 1);
		if (!h.Exchange(tw, ids, eye)) return false;
		if (!h.Read(c.depth, r[v].depth) || !h.Read(c.normals, r[v].normals) || !h.Read(c.lampResult, r[v].result) || !h.Read(gFH.eye, r[v].eyeVis)) return false;
		h.EndFrame();
		// exchange 2: [RtLampPrimary] + RtLampTraceSigma (the ShadowDenoiser's trace; its packed result is the denoiser's to fill)
		if (!sameFrame && (!h.Upload(c.depth, r[1].depth) || !h.Upload(c.normals, r[1].normals))) return false;
		if (sameFrame) h.AddPass(gShPrimary, primaryImgs, 0x3, &pc, sizeof(pc), (S + 7) / 8, (S + 7) / 8);
		h.AddPass(gS.shSigma, sigmaImgs, 0x14, &ks, sizeof(ks), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(tw, ids, eye)) return false;
		if (!h.Read(gS.signal, r[v].signal)) return false;
		h.EndFrame();
	}
	releaseWorld(h, ids);

	bool ok = true;
	// ---- hidden = absent, bit for bit
	auto same = [&](const char* what, const std::vector<float>& a, const std::vector<float>& b, size_t count) {
		size_t diff = 0;
		for (size_t i = 0; i < count && i < a.size() && i < b.size(); i++) if (a[i] != b[i]) diff++;
		const bool pass = diff == 0 && a.size() >= count && b.size() >= count;
		say("    %s: body hidden vs absent: %zu of %zu values differ -> %s\n", what, diff, count, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	};
	same("camera rays' depth (RtLampPrimary)", r[0].depth, r[1].depth, size_t(S) * S);
	same("camera rays' normals (RtLampPrimary)", r[0].normals, r[1].normals, size_t(S) * S * 4);
	same("lamp result (RtLampTrace)", r[0].result, r[1].result, size_t(S) * S * 2);
	same("denoiser signal, lamp 0 (RtLampTraceSigma)", r[0].signal, r[1].signal, size_t(S) * S * 2);   // layer 0: the first S rows
	same("lamps from the eye (RtLampEye)", r[0].eyeVis, r[1].eyeVis, 256);
	if (!ok) dumpPfm("flashhead_hidden_result", r[0].result, S, S, 2);

	// ---- absent vs the CPU; lamp 0 lights the floor, the crate shadows it
	const World absent = withObjects(w, plain);
	int checked = 0, bad = 0, lit0 = 0, shadow0 = 0, shown = 0;
	std::vector<char> lamp0Lit(S * S, 0);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const UINT p = j * S + i;
		const float a = r[1].result[p * 2], g = r[1].result[p * 2 + 1];
		const int entry = int(a) % 1024, code = int(g) / 1024;
		if (entry != 1023 && entry % 64 == 0) { if (entry / 64 == 15) { lit0++; lamp0Lit[p] = 1; } else if (entry / 64 == 0) shadow0++; }
		const LampExpect e = expectLamps(absent, lamps, cam, i, j, 1, 0.5f);
		if (!e.stable) continue;
		checked++;
		bool good;
		if (!e.valid) good = entry == 1023;
		else {
			const int want = e.slot[0] >= 0 ? e.slot[0] + 64 * e.lit[0] : 1023;
			const int wantCode = int(std::min(std::max(std::floor(std::log2(e.viewZ) * k.origin[3] + 0.5f), 1.0f), 4095.0f));
			good = entry == want && std::abs(code - wantCode) <= 1;
			if (!good && shown++ < 5) say("    absent pixel (%u, %u): entry %d code %d, CPU slot %d lit %d code %d\n", i, j, entry, code, e.slot[0], e.lit[0], wantCode);
		}
		if (!good) bad++;
	}
	// (a lamp at the eye hides its own shadows behind their casters: the crate's shows only as a sliver, 6 units of offset; counted, not required)
	const bool cpuOk = bad == 0 && checked > S * S / 2 && lit0 > 3000;
	say("    absent body vs the CPU: %d stable pixels, %d wrong; lamp 0 (in the head) lights %d pixels, %d in the crate's shadow (seen from the eye) -> %s\n",
		checked, bad, lit0, shadow0, cpuOk ? "ok" : "BAD");
	if (!cpuOk) ok = false;

	// ---- the control: a VISIBLE head round the lamp blocks it (the scene sees a pass-over that stopped working)
	int ctlN = 0, ctlBlocked = 0, ctlSigmaBlocked = 0;
	for (UINT p = 0; p < S * S; p++) {
		if (!lamp0Lit[p]) continue;
		ctlN++;
		const int entry = int(r[2].result[p * 2]) % 1024;
		if (entry % 64 == 0 && entry / 64 == 0) ctlBlocked++;
		if (r[2].signal[p * 2] < 0.5f) ctlSigmaBlocked++;
	}
	const bool ctlOk = ctlN > 3000 && ctlBlocked * 100 >= ctlN * 99 && ctlSigmaBlocked * 100 >= ctlN * 99;
	say("    control, body visible: of the %d pixels lamp 0 lights without the body, RtLampTrace blocks %d, the Sigma signal %d (>= 99 %% each) -> %s\n",
		ctlN, ctlBlocked, ctlSigmaBlocked, ctlOk ? "ok" : "BAD");
	if (!ctlOk) ok = false;

	// ---- RtLampEye: lamp 0 seen (people left out), lamp 1 behind the crate not, the empty entries 1
	bool eyeOk = true;
	for (int v = 0; v < 3; v++) {
		int emptyBad = 0;
		for (int s = 2; s < 256; s++) if (r[v].eyeVis[s] != 1.0f) emptyBad++;
		const bool pass = r[v].eyeVis[0] == 1.0f && r[v].eyeVis[1] == 0.0f && emptyBad == 0;
		say("    RtLampEye, body %s: lamp 0 (in the head) %.3f, lamp 1 (behind the crate) %.3f, %d empty entries not 1 -> %s\n",
			v == 0 ? "hidden" : v == 1 ? "absent" : "visible", r[v].eyeVis[0], r[v].eyeVis[1], emptyBad, pass ? "ok" : "BAD");
		if (!pass) eyeOk = false;
	}
	if (!eyeOk) ok = false;
	return ok;
}
