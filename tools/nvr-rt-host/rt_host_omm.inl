// rt_host_omm.inl (2026-11-07, ptaaa-omm, protocol 9): opacity micromaps for alpha-tested geometry (tools/nvr-rt-helper/NvrRtOmm.inl,
// NvrRtOmmMath.h, RtOmm.comp). Included by rt_host.cpp.
//
// This PC's GPU (AMD iGPU) has no VK_EXT_opacity_micromap, so no micromap is ever traced here. What runs: the helper's state builder and
// its check on the GPU (NVR_RT_OMM_SELF_TEST: built and checked on any GPU), and the CPU half against brute force:
//   omm       1. CPU: the spec's curve (every micro-triangle one index, every point the index of the micro-triangle holding it, levels 0-10);
//                the thresholds (every 8-bit / half alpha the builder calls solid passes rtAlphaPass's test however its float maths
//                rounds: ref within 2.5 ulp, fused or not, every dither)
//             2. GPU: alpha-tested cards and a tiled 288-triangle grid (atlas slots, refs 128 / 30 / 100, a real texture at lod 0 and at every
//                mip, a decal that must be left out), one object a step: the helper builds each one's states and checks them with the
//                shader's own rtAlphaPass on 262144 points (a quarter on micro-triangle edges, 8 dithers, class B also at random ray cones).
//                PASS: 0 violations, every key built once (the decal never), sensible opaque shares (more for ref 30 than 128, fewer at every
//                mip than at lod 0), the state SELF_TEST (or ON on a GPU with the extension)
//   ommbreak  the negative control (last scene: it turns the micromaps off for the rest of the helper's session): every micro-triangle
//             marked opaque (NVR_RT_OMM_BREAK_TEST): the check must catch it and switch the micromaps off.

#include "NvrRtOmmMath.h"

static int gShOmmAnyMip = -1;	// RtLampTrace.spv under a name the helper does not know: its frames count as "any mip" (class B)

static bool registerOmm(Host& h, const std::string& spv) {
	char tmp[MAX_PATH] = {};
	GetTempPathA(MAX_PATH, tmp);
	const std::string dir = std::string(tmp) + "NvrRtHostOmm";
	CreateDirectoryA(dir.c_str(), nullptr);
	const std::string copy = dir + "\\OmmTestAnyMipLampTrace.spv";
	if (!CopyFileA((spv + "\\RtLampTrace.spv").c_str(), copy.c_str(), FALSE)) { say("  error: could not copy RtLampTrace.spv to %s\n", copy.c_str()); return false; }
	gShOmmAnyMip = h.AddShader(copy);
	return true;
}

// 1a. the curve: a bijection per level, and every point's index is the index of the micro-triangle that holds it
static int ommCurveCpuTest(uint64_t& points) {
	int bad = 0;
	points = 0;
	uint32_t s = 12345u;
	auto rnd = [&]() { s = s * 747796405u + 2891336453u; uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; return float(((w >> 22u) ^ w) >> 8) / 16777216.0f; };
	for (uint32_t level = 0; level <= 10; level++) {
		const uint32_t n = 1u << level, cells = n * n;
		std::vector<uint8_t> seen(cells, 0);
		for (uint32_t iv = 0; iv < n; iv++) for (uint32_t iu = 0; iu < n; iu++) for (int f = 0; f < 2; f++) {
			if (f ? iu + iv + 2 > n : iu + iv + 1 > n) continue;
			const uint32_t i = OmmMath::OmmIndexOfCell(iu, iv, f != 0, level);
			if (i >= cells || seen[i]++) bad++;
		}
		for (uint32_t i = 0; i < cells; i++) if (seen[i] != 1) bad++;
		for (int k = 0; k < 20000; k++) {
			float u = rnd(), v = rnd();
			if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
			const float fu = u * n, fv = v * n;
			const float iu = std::floor(fu), iv = std::floor(fv), ru = fu - iu, rv = fv - iv;
			if (ru < 1e-3f || rv < 1e-3f || ru > 1 - 1e-3f || rv > 1 - 1e-3f || std::fabs(ru + rv - 1.0f) < 1e-3f || (u + v) * n > n - 1e-3f) continue;
			const bool flipped = ru + rv > 1.0f;
			points++;
			if (OmmMath::OmmCurveIndex(u, v, level) != OmmMath::OmmIndexOfCell(uint32_t(iu), uint32_t(iv), flipped, level)) bad++;
		}
	}
	return bad;
}

// 1b. the thresholds: what the builder calls solid is solid for rtAlphaPass whatever its float maths does
static int ommThresholdCpuTest(uint64_t& cases, bool& oneSolidAt128) {
	std::vector<float> alphas;
	for (int k = 0; k <= 255; k++) alphas.push_back(float(k) / 255.0f);
	for (uint32_t hbits = 0; hbits <= 0x3C00u; hbits++) alphas.push_back(fromHalf(uint16_t(hbits)));   // every half float in [0, 1]
	const float dmax = float(16777215u) * (1.0f / 16777216.0f) - 0.5f;
	int bad = 0;
	cases = 0;
	for (uint32_t r8 = 0; r8 < 255; r8++) {
		float tDither = 0, tPlain = 0;
		OmmMath::OmmThresholds(r8, tDither, tPlain);
		const float ref0 = float(r8) / 255.0f;
		float refs[5] = { ref0, ref0, ref0, ref0, ref0 };
		if (r8) { refs[1] = std::nextafter(ref0, 2.0f); refs[2] = std::nextafter(refs[1], 2.0f); refs[3] = std::nextafter(ref0, -1.0f); refs[4] = std::nextafter(refs[3], -1.0f); }
		for (float ref : refs) {
			const float width = std::min(1.0f, 2.0f * std::min(ref, 1.0f - ref));
			const float dithers[4] = { dmax, dmax * 0.999f, 0.25f, -0.5f };
			for (float a : alphas) {
				if (a > tDither) for (float d : dithers) {
					volatile float prod = d * width;
					const float t1 = ref + prod, t2 = std::fma(d, width, ref);
					cases++;
					if (!(a > t1) || !(a > t2)) bad++;
				}
				if (a > tPlain) { cases++; if (!(a > ref)) bad++; }
			}
		}
	}
	float d128 = 0, p128 = 0;
	OmmMath::OmmThresholds(128, d128, p128);
	oneSolidAt128 = 1.0f > d128 && !(254.0f / 255.0f > d128) && 129.0f / 255.0f > p128 && !(128.0f / 255.0f > p128);
	return bad;
}

// 2. the GPU
static float ommAlphaCard(int x, int y) {          // 64 x 64: a solid square, a checker strip, a gradient strip, the rest empty
	if (x >= 16 && x < 48 && y >= 16 && y < 48) return 1.0f;
	if (x < 8) return float((x + y) & 1);
	if (y >= 56) return float(x) / 63.0f;
	return 0.0f;
}
static float ommAlphaBlobs(int x, int y) {          // 64 x 64: soft blobs
	const float fx = (x + 0.5f) / 64.0f, fy = (y + 0.5f) / 64.0f;
	const float a = 0.5f + 0.5f * std::sin(fx * 12.566f) * std::cos(fy * 18.85f);
	return a > 0.8f ? 1.0f : a < 0.3f ? 0.0f : (a - 0.3f) / 0.5f;
}
static std::vector<uint8_t> ommDiscTexture(uint32_t& levels) {   // 256 x 256 BGRA8: a solid disc with a soft rim and 4 holes, every mip
	const int R = 256;
	std::vector<float> a(R * R);
	for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
		const float fx = (x + 0.5f) / R - 0.5f, fy = (y + 0.5f) / R - 0.5f, r = std::sqrt(fx * fx + fy * fy);
		float v = r < 0.3f ? 1.0f : r > 0.4f ? 0.0f : (0.4f - r) / 0.1f;
		for (int k = 0; k < 4; k++) { const float hx = fx - 0.12f * std::cos(k * 1.5708f), hy = fy - 0.12f * std::sin(k * 1.5708f); if (hx * hx + hy * hy < 0.0016f) v = 0.0f; }
		a[y * R + x] = v;
	}
	std::vector<uint8_t> out;
	int r = R;
	levels = 0;
	while (true) {
		for (int i = 0; i < r * r; i++) { out.push_back(90); out.push_back(140); out.push_back(60); out.push_back(uint8_t(a[i] * 255.0f + 0.5f)); }
		levels++;
		if (r == 1) break;
		const int n = r / 2;
		std::vector<float> b(n * n);
		for (int y = 0; y < n; y++) for (int x = 0; x < n; x++)
			b[y * n + x] = 0.25f * (a[(2 * y) * r + 2 * x] + a[(2 * y) * r + 2 * x + 1] + a[(2 * y + 1) * r + 2 * x] + a[(2 * y + 1) * r + 2 * x + 1]);
		a.swap(b); r = n;
	}
	return out;
}

struct OmmCounts { uint64_t built, micro, opaque, checked, violations; };
static OmmCounts ommCounts(Host& h) {
	return { h.shared->ommBuilt, h.shared->ommMicro, h.shared->ommOpaque, h.shared->ommChecked, h.shared->ommViolations };
}

// One exchange with one lamp pass (its result is not looked at: the pass names the frame's class), waited for
static bool ommFrame(Ctx& c, const World& w, const std::vector<uint32_t>& ids, int shader) {
	Host& h = *c.host;
	const Camera cam = Camera::lookDir({ 0, 0, 2400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	LampConstants k; lampPassConstants(k, cam, 1, 1);
	const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaAtlas };
	h.AddPass(shader, imgs, 0x4, &k, sizeof(k), 1, 1);
	if (!h.Exchange(w, ids, cam.eye) || !h.Finish()) return false;
	h.EndFrame();
	return true;
}

// Frames until `want` more state sets were built and collected (a build is collected the frame after its own)
static bool ommUntilBuilt(Ctx& c, const World& w, const std::vector<uint32_t>& ids, int shader, uint64_t want, OmmCounts& before, OmmCounts& after) {
	before = ommCounts(*c.host);
	for (int f = 0; f < 12; f++) {
		if (!ommFrame(c, w, ids, shader)) return false;
		after = ommCounts(*c.host);
		if (after.built >= before.built + want) {
			for (int extra = 0; extra < 2; extra++) if (!ommFrame(c, w, ids, shader)) return false;   // nothing more may come
			after = ommCounts(*c.host);
			return true;
		}
	}
	return true;
}

static bool sceneOmm(Ctx& c) {
	Host& h = *c.host;
	bool ok = true;
	// 1. CPU
	uint64_t points = 0, cases = 0;
	bool oneSolid = false;
	const int curveBad = ommCurveCpuTest(points);
	const int thrBad = ommThresholdCpuTest(cases, oneSolid);
	say("  CPU: curve (levels 0-10, every micro-triangle once, %llu random points in their own): %d wrong; thresholds (255 refs x 5 roundings x %u alphas x 4 "
		"dithers, fused and not): %d of %llu solid calls fail the shader's test; alpha 1 solid at ref 128 hashed, 254/255 not, 129/255 at lod 0: %s\n",
		(unsigned long long)points, curveBad, 256u + 0x3C01u, thrBad, (unsigned long long)cases, oneSolid ? "yes" : "NO");
	ok = ok && curveBad == 0 && thrBad == 0 && oneSolid && points > 100000;

	// 2. GPU: the world (every mesh staged once; each step's world names the objects it shows)
	const int slotCard = 60, slotBlobs = 61, slotReal = 62;
	const uint32_t texId = 9;
	if (!uploadSlot(h, c.alphaAtlas, slotCard, ommAlphaCard) || !uploadSlot(h, c.alphaAtlas, slotBlobs, ommAlphaBlobs)) return false;
	{	// the real texture's slot: an empty atlas slot (the shaders read the real texture)
		uint32_t levels = 0;
		const std::vector<uint8_t> tex = ommDiscTexture(levels);
		if (!h.StageTexture(texId, 44 /* VK_FORMAT_B8G8R8A8_UNORM */, 256, 256, levels, tex)) return false;
		h.SlotTextures(slotReal, texId, 0, 0);
	}
	World all;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	Mesh card; card.quad({ -500, -500, 300 }, { 500, -500, 300 }, { 500, 500, 300 }, { -500, 500, 300 });
	Mesh grid;   // 12 x 12 quads, texcoords tiled -1.3 .. 2.7 and jittered (wrap, negative uv, uneven triangles)
	for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++) {
		const float x0 = -600 + x * 100.0f, y0 = -600 + y * 100.0f;
		grid.quad({ x0, y0, 400 }, { x0 + 100, y0, 400 }, { x0 + 100, y0 + 100, 400 }, { x0, y0 + 100, 400 });
		float* q = &grid.uv[grid.uv.size() - 8];
		for (int k = 0; k < 4; k++) {
			const float cx = (k == 1 || k == 2) ? 1.0f : 0.0f, cy = (k >= 2) ? 1.0f : 0.0f;
			q[2 * k] = -1.3f + (x + cx) / 3.0f + 0.013f * std::sin(float(x * 7 + y * 3 + k));
			q[2 * k + 1] = -1.3f + (y + cy) / 3.0f + 0.011f * std::cos(float(x * 5 + y * 11 + k));
		}
	}
	all.meshes = { floor, card, grid };
	Object f; f.mesh = 0; f.id = 1;
	Object a128; a128.mesh = 1; a128.id = 2; a128.alphaTested = true; a128.alphaSlot = slotCard; a128.alphaRef = 128;
	Object a30 = a128; a30.id = 3; a30.alphaRef = 30;
	Object blobs; blobs.mesh = 2; blobs.id = 4; blobs.alphaTested = true; blobs.alphaSlot = slotBlobs; blobs.alphaRef = 100;
	Object real = a128; real.id = 5; real.alphaSlot = slotReal;
	Object decal = a128; decal.id = 6; decal.alphaSlot = slotBlobs; decal.alphaRef = 77; decal.decal = true;
	all.objects = { f, a128, a30, blobs, real, decal };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, all, ids)) return false;
	const uint32_t control = NVR_RT_OMM_ON | NVR_RT_OMM_SELF_TEST | (uint32_t(c.alphaAtlas + 1) & NVR_RT_OMM_ATLAS_MASK);
	h.shared->opacityMicromaps = control;
	if (!ommFrame(c, World(), ids, c.shLamp)) return false;   // the geometry and the texture in (no instance yet)

	struct Step { const char* what; Object o; int shader; uint64_t want; double share = 0; uint64_t checked = 0, viol = 0, built = 0; };
	std::vector<Step> steps = {
		{ "atlas card, ref 128 (lod 0 frames)", a128, c.shLamp, 1 },
		{ "the same card, ref 30", a30, c.shLamp, 1 },
		{ "288-triangle grid, tiled / negative uv, ref 100", blobs, c.shLamp, 1 },
		{ "real texture, lod 0 (class A)", real, c.shLamp, 1 },
		{ "real texture, every mip (class B frames)", real, gShOmmAnyMip, 1 },
		{ "the atlas card in class B frames (no rebuild: the atlas has no mips)", a128, gShOmmAnyMip, 0 },
		{ "a decal (left out: shadow rays must never find it solid)", decal, c.shLamp, 0 },
	};
	for (Step& s : steps) {
		World w; w.meshes = all.meshes; w.objects = { f, s.o };
		OmmCounts b = {}, e = {};
		if (!ommUntilBuilt(c, w, ids, s.shader, s.want ? s.want : 1, b, e)) return false;
		s.built = e.built - b.built;
		s.checked = e.checked - b.checked;
		s.viol = e.violations - b.violations;
		s.share = e.micro > b.micro ? double(e.opaque - b.opaque) / double(e.micro - b.micro) : 0.0;
		say("  %-70s built %llu, opaque %.3f of the micro-triangles, %llu check samples on them, %llu violations\n", s.what, (unsigned long long)s.built, s.share,
			(unsigned long long)s.checked, (unsigned long long)s.viol);
		if (s.built != s.want || s.viol) ok = false;
		if (s.want && s.share > 0.001 && s.checked < 10000) ok = false;   // (a step with nothing opaque has nothing to check)
	}
	// the shares: ref 30 keeps more than 128 (the gradient strip's upper part); every mip keeps fewer than lod 0 (a mip mixes the rim)
	const bool shares = steps[0].share > 0.2 && steps[0].share < 0.4 && steps[1].share > steps[0].share && steps[2].share > 0.02 &&
		steps[3].share > 0.15 && steps[4].share < steps[3].share;
	const uint32_t state = h.shared->ommState;
	const bool stateOk = state == NVR_RT_OMM_STATE_SELF_TEST || state == NVR_RT_OMM_STATE_ON;
	say("  totals: %llu built, %llu of %llu micro-triangles opaque, %llu check samples, %llu violations; state %u (%s), flags 0x%x; shares %s\n",
		(unsigned long long)h.shared->ommBuilt, (unsigned long long)h.shared->ommOpaque, (unsigned long long)h.shared->ommMicro,
		(unsigned long long)h.shared->ommChecked, (unsigned long long)h.shared->ommViolations, state,
		state == NVR_RT_OMM_STATE_SELF_TEST ? "self-test: no VK_EXT_opacity_micromap here, nothing traced through them" : state == NVR_RT_OMM_STATE_ON ? "ON: traced" : "?",
		h.shared->ommFlags, shares ? "as expected" : "NOT as expected");
	ok = ok && shares && stateOk && h.shared->ommViolations == 0;
	h.shared->opacityMicromaps = NVR_RT_OMM_ON | (uint32_t(c.alphaAtlas + 1) & NVR_RT_OMM_ATLAS_MASK);   // as the game sends it
	releaseWorld(h, ids);
	h.SlotTextures(slotReal, 0, 0, 0);
	h.ReleaseTexture(texId);
	if (!h.Exchange(World(), {}, V3()) || !h.Finish()) return false;
	h.EndFrame();
	return ok;
}

// The negative control: the builder told to mark every micro-triangle opaque; the check must find it and switch the micromaps off
static bool sceneOmmBreak(Ctx& c) {
	Host& h = *c.host;
	const int slotCard = 63;
	if (!uploadSlot(h, c.alphaAtlas, slotCard, ommAlphaCard)) return false;
	World w;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	Mesh card; card.quad({ -500, -500, 300 }, { 500, -500, 300 }, { 500, 500, 300 }, { -500, 500, 300 });
	w.meshes = { floor, card };
	Object f; f.mesh = 0; f.id = 1;
	Object k; k.mesh = 1; k.id = 2; k.alphaTested = true; k.alphaSlot = slotCard; k.alphaRef = 131;
	w.objects = { f, k };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	h.shared->opacityMicromaps = NVR_RT_OMM_ON | NVR_RT_OMM_SELF_TEST | NVR_RT_OMM_BREAK_TEST | (uint32_t(c.alphaAtlas + 1) & NVR_RT_OMM_ATLAS_MASK);
	OmmCounts b = {}, e = {};
	const bool ran = ommUntilBuilt(c, w, ids, c.shLamp, 1, b, e);
	h.shared->opacityMicromaps = NVR_RT_OMM_ON | (uint32_t(c.alphaAtlas + 1) & NVR_RT_OMM_ATLAS_MASK);
	for (int i = 0; i < 2 && ran; i++) ommFrame(c, w, ids, c.shLamp);
	const uint32_t state = h.shared->ommState;
	const uint64_t viol = h.shared->ommViolations - b.violations;
	const bool ok = ran && viol > 0 && state == NVR_RT_OMM_STATE_DISABLED;
	say("  every micro-triangle marked opaque (a broken builder): %llu violations found, state %u (%s); helper says: %s\n", (unsigned long long)viol, state,
		state == NVR_RT_OMM_STATE_DISABLED ? "off for the session, as it must" : "NOT switched off", h.shared->message);
	releaseWorld(h, ids);
	if (!h.Exchange(World(), {}, V3()) || !h.Finish()) return false;
	h.EndFrame();
	return ok;
}
