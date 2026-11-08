// (2026-11-07 perf-mirrorfix) Scene `hairmirror`: an actor's alpha-tested part (a hair card) in a mirror.
//
// The tester's mirror wall (VSRTRoom): the reflected hair was a SOLID card (its texture with no cut-out, a white fringe at its edges) and the
// face inside it dark. RtCore took every skinned partition opaque. Now an alpha-tested partition that is not skin goes in as a cut-out
// (RtScenePick::ActorAlphaTested / ActorAlphaRef / ActorRecordFlags / ActorOpFlags: record ALPHA_TESTED + its reference + slot, a
// non-opaque DYNAMIC build, the actors' mask 0x20), and hair is TINTED (ALPHA_TESTED + TEXTURED: RtHit.glsl multiplies the texture's colour
// by the record's linear tint x 2^2.2, as the hair shaders' albedo x 2 x HairTint). Checked here, against the CPU caster:
//   1. ReflectionsRTAlpha (averages only): a mirror floor reflects a wall behind a striped hair card: the reflection rays meet the wall
//      through the card's gaps (and the card's strands, in the tint's colour: the record's albedo), and the sun rays at the wall pass the
//      gaps too (the card's shadow is striped, not solid);
//   2. ReflectionsRTAlphaTex (the albedo atlas): the same, the card's colour = the atlas texel (decoded) x tint x 2^2.2;
//   3. control: the same card packed as before (opaque, TEXTURED only): the rays stop at the card's gaps (the solid card the tester saw).
// Atlas slots 86 (alpha) / 86 (colour): the scene's own.

static int gShReflAlphaTex = -1;
static bool registerHair(Host& h, const std::string& spv) {
	gShReflAlphaTex = h.AddShader(spv + "\\ReflectionsRTAlphaTex.spv");
	return gShReflAlphaTex >= 0;
}

static bool hairRun(Ctx& c, bool textured, bool oldPacking, int& bad, int& wallThroughGaps, int& cardHits, int& sunThroughGaps, int& stoppedAtGaps) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(3);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });   // the mirror floor
	w.meshes[1].box({ -900, 900, 0 }, { 900, 1300, 1400 });                                              // the wall block
	w.meshes[2].quad({ -600, 450, 50 }, { 600, 450, 50 }, { 600, 450, 1300 }, { -600, 450, 1300 });       // the hair card (facing -y)
	const int slot = 86;
	w.slots[slot] = [](int x, int) { return ((x / 16) & 1) ? 1.0f : 0.0f; };   // strands: 16-texel stripes
	const V3 tintStored = { 0.8f, 0.3f, 0.1f };                                   // HairTint (c2) as the shader gets it
	const V3 tintLinear = { RtScenePick::TintLinear(tintStored.x), RtScenePick::TintLinear(tintStored.y), RtScenePick::TintLinear(tintStored.z) };
	const float grey = 128.0f / 255.0f;                                            // the hair texture's colour (stored sRGB)
	const float greyLinear = std::pow((grey + 0.055f) / 1.055f, 2.4f);
	Object floor; floor.mesh = 0; floor.albedo = { 0.5f, 0.5f, 0.5f }; floor.id = 1;
	Object wall; wall.mesh = 1; wall.albedo = { 0.3f, 0.6f, 0.3f }; wall.id = 2;
	// the card as RtCore::AddSkinned packs it now: alpha test + blend (the game's hair), reference 0 -> 128, not skin
	const uint16_t hairFlags = RtScenePick::kAlphaTest | RtScenePick::kAlphaBlend;
	Object card; card.mesh = 2; card.id = 3; card.actor = true; card.alphaSlot = slot;
	card.alphaTested = !oldPacking && RtScenePick::ActorAlphaTested(true, hairFlags, false, true);
	card.textured = oldPacking;
	card.alphaRef = RtScenePick::ActorAlphaRef(hairFlags, 0);
	card.tinted = !oldPacking;
	card.albedo = oldPacking ? V3(greyLinear, greyLinear, greyLinear) : tintLinear;
	if (!oldPacking && (!card.alphaTested || card.alphaRef != 128u || !RtScenePick::IsTinted(card.recordFlags()) || card.mask() != NVR_RT_MASK_ACTOR)) {
		bad++; say("    the hair card's packing: alpha-tested %d, reference %u, flags %08X, mask %02X (expected a tinted cut-out, 128, mask 0x20)\n",
			int(card.alphaTested), card.alphaRef, card.recordFlags(), card.mask());
	}
	w.objects = { floor, wall, card };
	if (!uploadSlot(h, c.alphaAtlas, slot, w.slots[slot])) return false;
	{	// the colour atlas' slot: uniform grey (64 x 64)
		std::vector<float> t(64 * 64 * 4);
		for (size_t i = 0; i < t.size(); i++) t[i] = (i % 4) == 3 ? 1.0f : grey;
		if (!h.Upload(gM.albedoAtlas, t, (slot % 32) * 64, (slot / 32) * 64, 64, 64)) return false;
	}
	// staged as the game stages them: the card as an actor's pose (DYNAMIC; non-opaque only with the new packing)
	std::vector<uint32_t> ids(w.meshes.size(), 0);
	for (size_t m = 0; m < w.meshes.size(); m++) {
		if (!h.Stage(w.meshes[m], false, ids[m])) return false;
		if (m == 2) h.ops.back().flags = RtScenePick::ActorOpFlags(true, card.alphaTested);
	}
	if (((h.ops.back().flags & NVR_RT_OP_FLAG_ALPHA) != 0) == oldPacking) { bad++; say("    the card's build: op flags %X\n", h.ops.back().flags); }
	// the CPU's world: the card cut out only with the new packing (the old one: solid to every ray)
	World cpu = w;
	if (oldPacking) { cpu.objects[2].alphaTested = false; }
	World truth = w;	// what the game draws: the card cut out (its alpha test), whatever the packing
	truth.objects[2].alphaTested = true; truth.objects[2].textured = false;
	const Camera cam = Camera::lookDir({ 0, -1600, 700 }, normalize({ 0, 1, -0.45f }), { 0, 0, 1 }, 1.3f, 1.3f);
	const V3 sun = normalize({ 0.1f, -0.8f, 0.6f });
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<GPixel> px(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(cpu, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit || hh.object != 0) continue;
		const V3 v = normalize(d), r = v - hh.n * (2 * dot(v, hh.n));
		octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;   // view z, roughness 0 + 2 (cast)
		px[j * S + i].valid = true;
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
	ReflConstants k = {};
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.size[2] = 10000.0f; k.size[3] = 1.0f;     // Distance, sun shadow rays (no AlphaSolid)
	k.forward[3] = 0.0f;
	put4(k.sun, sun, 1.0f);
	put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
	if (textured) h.AddPass(gShReflAlphaTex, { c.refDir, c.refRay, c.refHit, c.refShade, c.alphaAtlas, gM.albedoAtlas }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	else h.AddPass(gShReflAlpha, { c.refDir, c.refRay, c.refHit, c.refShade, c.alphaAtlas }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> hit, shade;
	if (!h.Read(c.refHit, hit) || !h.Read(c.refShade, shade)) return false;
	h.EndFrame();
	int checked = 0, wrong = 0, shown = 0;
	wallThroughGaps = cardHits = sunThroughGaps = stoppedAtGaps = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const float* H = &hit[(j * S + i) * 4]; const float* C = &shade[(j * S + i) * 4];
		if (!px[j * S + i].valid) continue;
		bool stable = true; Hit ref, cut; V3 start, rdir;
		for (int s = 0; s < 3; s++) {
			const float off = s == 0 ? 0.0f : s == 1 ? -0.25f : 0.25f;
			const V3 d = cam.ray((i + 0.5f + off) / S, (j + 0.5f + off) / S);
			const Hit fh = trace(cpu, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!fh.hit || fh.object != 0) { stable = false; break; }
			const V3 surface = d * fh.t;
			const V3 v = normalize(d), r = v - fh.n * (2 * dot(v, fh.n));
			const V3 half = r - normalize(surface);
			const V3 st = surface + half * ((0.5f + fh.t * 0.002f) / length(half));
			const Hit rh = trace(cpu, cam.eye + st, r, 1.0f + fh.t * 0.0015f, 10000.0f, 0xFF, true);
			const Hit ch = trace(truth, cam.eye + st, r, 1.0f + fh.t * 0.0015f, 10000.0f, 0xFF, true);   // the card cut out (the truth)
			if (s == 0) { ref = rh; cut = ch; start = st; rdir = r; }
			else if (rh.hit != ref.hit || rh.object != ref.object || ch.object != cut.object) stable = false;
		}
		if (!stable || !ref.hit) continue;
		const V3 at = cam.eye + start + rdir * ref.t;
		float sunVis = 1.0f;
		if (dot(ref.n, sun) > 0) {
			const V3 from = at + ref.n * std::max(1.0f, ref.t * 0.001f);
			sunVis = trace(cpu, from, sun, 0.5f, 30000.0f, 0xFF, true, true).hit ? 0.0f : 1.0f;
			// a wall hit lit through the card's gaps (a solid card would shadow it)
			World solid = w; solid.objects[2].alphaTested = false;
			if (!oldPacking && ref.object == 1 && sunVis > 0.5f && trace(solid, from, sun, 0.5f, 30000.0f, 0xFF, true, true).hit) sunThroughGaps++;
		}
		checked++;
		if (ref.object == 2) cardHits++;
		if (!oldPacking && ref.object == 1) {
			World solid = w; solid.objects[2].alphaTested = false;
			if (trace(solid, cam.eye + start, rdir, 1.0f, 10000.0f, 0xFF, true).object == 2) wallThroughGaps++;
		}
		if (oldPacking && ref.object == 2 && cut.object == 1) stoppedAtGaps++;
		const Object& o = cpu.objects[ref.object];
		V3 want = o.albedo;
		if (ref.object == 2 && textured) want = oldPacking ? V3(greyLinear, greyLinear, greyLinear) : V3(greyLinear * tintLinear.x, greyLinear * tintLinear.y, greyLinear * tintLinear.z) * RtScenePick::kTintScale;
		const V3 n = octDecode(H[2], H[3]);
		auto closeTo = [](float a, float b) { return std::fabs(a - b) <= 4e-3f + 0.01f * std::fabs(b); };
		const bool ok = std::fabs(H[0] - ref.t) <= 0.002f * ref.t + 1.0f && (int(H[1]) & 6) == 6 && dot(n, ref.n) > 0.995f &&
			closeTo(C[0], want.x) && closeTo(C[1], want.y) && closeTo(C[2], want.z) && C[3] == sunVis;
		if (!ok) { wrong++; if (shown++ < 5) say("    pixel (%u, %u): got t %.2f flags %d albedo (%.3f %.3f %.3f) sun %.0f; expected t %.2f object %d albedo (%.3f %.3f %.3f) sun %.0f\n",
			i, j, H[0], int(H[1]), C[0], C[1], C[2], C[3], ref.t, ref.object, want.x, want.y, want.z, sunVis); }
	}
	say("  %s, %s: %d reflection rays checked, %d mismatches; %d met the card, %d met the wall through its gaps, %d wall hits sunlit through "
		"its gaps%s\n", textured ? "ReflectionsRTAlphaTex (atlas colour x tint)" : "ReflectionsRTAlpha (the record's tint)",
		oldPacking ? "the OLD packing (opaque, control)" : "the card a tinted cut-out", checked, wrong, cardHits, wallThroughGaps, sunThroughGaps,
		oldPacking ? "" : "");
	if (oldPacking) say("    control: %d rays stopped at the card where its gaps are (the solid card)\n", stoppedAtGaps);
	bad += wrong;
	if (checked < 1500) { bad++; say("    too few reflection rays checked\n"); }
	if (wrong) { dumpPfm(oldPacking ? "hair_old_t" : textured ? "hair_tex_t" : "hair_t", hit, S, S, 4, 0); dumpPfm(oldPacking ? "hair_old_albedo" : textured ? "hair_tex_albedo" : "hair_albedo", shade, S, S, 4, 0); }
	releaseWorld(h, ids);
	return true;
}

static bool sceneHairMirror(Ctx& c) {
	int bad = 0;
	// the packing rules themselves
	{
		using namespace RtScenePick;
		if (!ActorAlphaTested(true, kAlphaTest, false, true) || ActorAlphaTested(true, kAlphaTest, true, true) || ActorAlphaTested(true, kAlphaBlend, false, true) ||
			ActorAlphaTested(false, 0, false, true) || ActorAlphaTested(true, kAlphaTest, false, false)) { bad++; say("    ActorAlphaTested: wrong answer (test, not skin, AlphaTested on only)\n"); }
		if (ActorAlphaRef(kAlphaTest | kAlphaBlend, 0) != 128u || ActorAlphaRef(kAlphaTest, 0) != 0u || ActorAlphaRef(kAlphaTest | kAlphaBlend, 200) != 200u) { bad++; say("    ActorAlphaRef wrong\n"); }
		ActorPart a; a.albedoKnown = true; a.textured = true; a.slot = 77; a.alphaTested = true; a.alphaRef = 128; a.tinted = true;
		const uint32_t f = ActorRecordFlags(a);
		if (!IsTinted(f) || ((f >> 8) & 0xFF) != 128u || ((f >> 16) & 0x3FF) != 77u) { bad++; say("    ActorRecordFlags (tinted cut-out) %08X\n", f); }
		a.tinted = false;
		if (IsTinted(ActorRecordFlags(a)) || !(ActorRecordFlags(a) & NVR_RT_RECORD_ALPHA_TESTED)) { bad++; say("    ActorRecordFlags (cut-out) wrong\n"); }
		a.alphaTested = false; a.skin = true;
		const uint32_t g = ActorRecordFlags(a);
		if ((g & NVR_RT_RECORD_ALPHA_TESTED) || !(g & NVR_RT_RECORD_TEXTURED) || NvrRtKindOf(g) != NVR_RT_KIND_SKIN) { bad++; say("    ActorRecordFlags (skin) %08X\n", g); }
		a.textured = false;
		if (ActorRecordFlags(a) & (NVR_RT_RECORD_TEXTURED | NVR_RT_RECORD_ALPHA_TESTED)) { bad++; say("    ActorRecordFlags (no texcoords) wrong\n"); }
		if (ActorOpFlags(true, true) != (NVR_RT_OP_FLAG_DYNAMIC | NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA) || ActorOpFlags(false, true) != NVR_RT_OP_FLAG_DYNAMIC ||
			ActorOpFlags(true, false) != (NVR_RT_OP_FLAG_DYNAMIC | NVR_RT_OP_FLAG_TEXCOORDS)) { bad++; say("    ActorOpFlags wrong\n"); }
		// no static record is ever both (RtScenePick::RecordFlags): only actors' hair is tinted
		Record r; r.alphaTested = true; r.textured = true; r.slot = 3;
		if (IsTinted(RecordFlags(r))) { bad++; say("    a static record came out tinted\n"); }
	}
	int through = 0, cards = 0, sunGaps = 0, stopped = 0;
	if (!hairRun(c, false, false, bad, through, cards, sunGaps, stopped)) return false;
	if (through < 200) { bad++; say("    only %d reflection rays reached the wall through the card's gaps: the card must be cut out\n", through); }
	if (cards < 200) { bad++; say("    only %d reflection rays met the card's strands\n", cards); }
	if (sunGaps < 50) { bad++; say("    only %d wall hits lit through the card's gaps (its shadow must be striped)\n", sunGaps); }
	if (!hairRun(c, true, false, bad, through, cards, sunGaps, stopped)) return false;
	if (through < 200 || cards < 200) { bad++; say("    textured: %d through the gaps, %d on the card\n", through, cards); }
	if (!hairRun(c, true, true, bad, through, cards, sunGaps, stopped)) return false;
	if (stopped < 200) { bad++; say("    control: only %d rays stopped at the old solid card's gaps: the test has no teeth\n", stopped); }
	say("  hairmirror: %s\n", bad ? "FAILED" : "the hair card is cut out in reflections and in the sun's shadow, tinted (record + texture x tint); the old packing stops the rays (control)");
	return bad == 0;
}
