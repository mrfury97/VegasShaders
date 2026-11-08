// (2026-11-07) Scene `pthidden`: the path tracer's camera rays and the player's own body in first person.
//
// A tester's video (PTRealtime, first person, outdoors at night): the sky was filled by the inside of the player's head (hair, a hat,
// an eye). In first person the player's third-person body is traced (FirstPersonBody: shadows, reflections) but not drawn: its instances
// carry the custom index NVR_RT_CUSTOM_HIDDEN, and the eye sits inside its head. RtPathTrace.comp's camera ray (bounce 0) went through
// everything, met the head first, and where the screen had no surface (the sky) that hit was the picture.
//
// Here: a floor, a block and the sky, seen from an eye wrapped in hidden boxes as the game's body is: the head (an actor, mask 0x20), a
// helmet (the player's rigid parts, mask 0x40) and a hair card (alpha-tested, mask 0x10, solid texture). The first hit's colour
// (PTDebugView 1: the albedo, the sky on a miss) must be the same with and without the hidden boxes, pixel for pixel, with the alpha
// test on and off. Before the fix every pixel showed the boxes' colour.
static bool scenePtHidden(Ctx& c) {
	Host& h = *c.host;
	const V3 eye = { 0, 0, 170 };
	World w;
	w.meshes.resize(5);
	w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });   // floor
	w.meshes[1].box({ -150, 600, 0 }, { 150, 700, 400 });                                               // a block ahead
	w.meshes[2].box({ eye.x - 12, eye.y - 12, eye.z - 15 }, { eye.x + 12, eye.y + 12, eye.z + 15 });     // the head round the eye
	w.meshes[3].box({ eye.x - 20, eye.y - 20, eye.z - 25 }, { eye.x + 20, eye.y + 20, eye.z + 35 });     // a helmet
	w.meshes[4].box({ eye.x - 16, eye.y - 16, eye.z - 20 }, { eye.x + 16, eye.y + 16, eye.z + 25 });     // a hair card
	w.meshes[4].uv.assign(w.meshes[4].pos.size() * 2, 0.5f);
	const int hairSlot = 52;
	w.slots[hairSlot] = [](int, int) { return 1.0f; };   // solid: the alpha test passes everywhere on it
	if (!uploadSlot(h, c.alphaAtlas, hairSlot, w.slots[hairSlot])) return false;
	Object floor; floor.mesh = 0; floor.land = true; floor.albedo = { 0.30f, 0.25f, 0.20f };
	Object block; block.mesh = 1; block.albedo = { 0.20f, 0.45f, 0.30f };
	Object head; head.mesh = 2; head.hookMask = 0x20; head.hidden = true; head.albedo = { 0.9f, 0.1f, 0.1f };
	Object helmet; helmet.mesh = 3; helmet.hookMask = 0x40; helmet.hidden = true; helmet.albedo = { 0.1f, 0.9f, 0.1f };
	Object hair; hair.mesh = 4; hair.alphaTested = true; hair.alphaSlot = hairSlot; hair.alphaRef = 100; hair.hidden = true; hair.albedo = { 0.1f, 0.1f, 0.9f };
	std::vector<uint32_t> ids;
	w.objects = { floor, block, head, helmet, hair };
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir(eye, { 0, 1, 0.12f }, { 0, 0, 1 }, 1.4f, 1.4f);   // the horizon a little below the middle: sky + ground
	const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano,
		c.aux16, c.ptDisplay, c.aux16, c.aux16, c.lampTable };
	auto run = [&](const std::vector<Object>& objects, bool alphaTest, std::vector<float>& out) {
		World v = w; v.objects = objects;
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 5.0f;
		put4(k.accum, 0.0f, 3.0f, 1.0f, alphaTest ? 1.0f : 0.0f);   // fresh mean, 3 bounces, 1 path, AlphaTest flag
		put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0.6f, 0.7f, 0.8f, 1.0f); put4(k.skyHorizon, 0.6f, 0.7f, 0.8f, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 1.0f);   // DebugView 1: the first hit's colour (the sky on a miss)
		k.rect[3] = 1.0f;
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(v, ids, cam.eye)) return false;
		const bool read = h.Read(c.ptDisplay, out);
		h.EndFrame();
		return read;
	};
	bool ok = true;
	for (int alphaTest = 1; alphaTest >= 0; alphaTest--) {
		std::vector<float> plain, wrapped;
		if (!run({ floor, block }, alphaTest != 0, plain) || !run({ floor, block, head, helmet, hair }, alphaTest != 0, wrapped)) return false;
		int differ = 0, sky = 0, ground = 0, shown = 0;
		for (UINT i = 0; i < S * S; i++) {
			float d = 0.0f;
			for (int ch = 0; ch < 3; ch++) d = std::max(d, std::fabs(plain[i * 4 + ch] - wrapped[i * 4 + ch]));
			const bool isSky = std::fabs(plain[i * 4] - 0.6f) < 0.02f && std::fabs(plain[i * 4 + 2] - 0.8f) < 0.02f;
			(isSky ? sky : ground)++;
			if (d > 1e-3f) { differ++; if (shown++ < 4) say("    pixel (%u, %u): %.3f %.3f %.3f without the body, %.3f %.3f %.3f with it\n", i % S, i / S,
				plain[i * 4], plain[i * 4 + 1], plain[i * 4 + 2], wrapped[i * 4], wrapped[i * 4 + 1], wrapped[i * 4 + 2]); }
		}
		const bool pass = differ == 0 && sky > int(S * S) / 8 && ground > int(S * S) / 8;
		say("  camera rays with the hidden first-person body round the eye (head 0x20, helmet 0x40, hair card 0x10), alpha test %s: "
			"%d sky + %d ground pixels, %d changed by the body -> %s\n", alphaTest ? "on" : "off", sky, ground, differ, pass ? "ok" : "BAD");
		if (!pass) { ok = false; dumpPfm(alphaTest ? "pthidden_a1" : "pthidden_a0", wrapped, S, S, 4, 0); }
	}
	releaseWorld(h, ids);
	return ok;
}
