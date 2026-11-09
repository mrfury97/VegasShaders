// ==============================================================================================================================
// (2026-11-07 perf-reflfix2) the RT test room's three tester issues on 1.0.6-test10 (docs/next/RT-PATH-TRACING.md "perf-reflfix2"):
//   reflorbs     white discs at the lamps, only in reflections: RT reflections' lamp estimate vs the game's own lamp light
//   reflbounce   gold as sharp as chrome in a mirror: MetalBounce's direction from the metal's GGX lobe; the finer roughness code
//   ptflashlight the path tracer's flashlight (lamp 0, its cone in the frame row's texel 15) in a dark room, every variant
// Included by rt_host.cpp after rt_host_reflfix.inl (uvSphere, angleDeg, gRF).

struct ReflFix2Ctx { int lampCheck = -1; };
static ReflFix2Ctx gR2;
static bool registerReflFix2(Host& h, const std::string& spv) {
	gR2.lampCheck = h.AddShader(spv + "\\RtLampLightCheck.spv");
	return gR2.lampCheck >= 0;
}

// ---- reflorbs ------------------------------------------------------------------------------------------------------------------
// (A) Rt/RtLampLightCheck.comp evaluates Rt/RtLampLight.glsl (the function Resolve's RtShadeHit and the helper's key-lamp pick use) at a
// grid of ceiling points over the RT test room's ceiling lamp (VSRTRoomLightCeiling: BasementLightKeyNeutral, FNAM fade 1.9, radius 650,
// colour 255 226 190, 66 units under the ceiling), packed by the game's RtReflLamps::Pack, against the game's lamp light written out
// here from the raw values (ManyLamps.h / the object shaders: diffuse x min(dimmer, 1) x LOD dimmer x LightScale, decoded, x the decoded
// 1 - (d/r)^2, x N.L; or LampFalloff's shape). Frames: indoors (LightScale 1), the room by day ("behave like exterior": 0.725), the
// physical shape on, HDR on (the dimmer kept). Printed: the old estimate's light / the game's right over the lamp (the orbs' factor).
// (B) the helper's pass (ReflectionsRT) with those lamps, a mirror floor under the ceiling at night: the ceiling's hits pick the ceiling
// lamp as their key lamp (RtLampLight in the pick), its shadow ray finds nothing in the way.
struct LampCheckConstants { float size[4], origin[4], axisU[4], axisV[4], normal[4], lamps[24][4]; };   // Rt/RtLampLightCheck.comp Pass

static bool sceneReflOrbs(Ctx& c) {
	Host& h = *c.host;
	bool allOk = true;
	const V3 lampPos = { 0, 0, 190 };
	const float ceilingZ = 256.0f, reach = 650.0f;
	RtReflLamps::Raw raw;
	raw.pos[0] = lampPos.x; raw.pos[1] = lampPos.y; raw.pos[2] = lampPos.z; raw.reach = reach;
	raw.diffuse[0] = 255 / 255.0f; raw.diffuse[1] = 226 / 255.0f; raw.diffuse[2] = 190 / 255.0f;
	raw.dimmer = 1.9f; raw.lodDimmer = 1.0f;
	const float eye[3] = { 0, 0, 0 };   // (the eye at the world origin: scene space = world)
	// the game's light at a ceiling point (normal down), independently of RtReflLamps / RtLampLight
	auto gameLight = [&](const RtReflLamps::Frame& f, V3 p, int ch) {
		const V3 toLamp = lampPos - p;
		const float d = length(toLamp);
		if (!(d < reach)) return 0.0f;
		const float ndl = std::max(dot(V3(0, 0, -1), toLamp * (1.0f / d)), 0.0f);
		const float dimmer = f.hdr ? raw.dimmer : std::min(raw.dimmer, 1.0f);
		const float colour = std::pow(raw.diffuse[ch] * dimmer * raw.lodDimmer * f.lightScale, 2.2f);
		const float q = (d / reach) * (d / reach);
		float fall;
		if (f.shaped) { const float w = (1 - q * q) * (1 - q * q); fall = f.shapeStrength * w / (1 + q * f.shapeY); }
		else fall = std::pow(1 - q, 2.2f);
		return colour * fall * ndl;
	};
	// the estimate before (ReflectionsRT.glsl until test 10): linearize(diffuse x dimmer) (sRGB), (1 - (d/r)^2)^2, N.L, no light scale
	auto oldLight = [&](V3 p, int ch) {
		const V3 toLamp = lampPos - p;
		const float d = length(toLamp);
		const float x = raw.diffuse[ch] * raw.dimmer;
		const float lin = x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
		const float fall = std::max(1.0f - d * d / (reach * reach), 0.0f);
		return lin * fall * fall * std::max(dot(V3(0, 0, -1), toLamp * (1.0f / d)), 0.0f);
	};
	struct Case { const char* name; RtReflLamps::Frame f; };
	std::vector<Case> cases(4);
	cases[0].name = "indoors (LightScale 1)";
	cases[1].name = "the room by day (LightScale 0.725)"; cases[1].f.lightScale = 0.725f;
	cases[2].name = "LampFalloff's shape (Falloff 0.25, strength 1.3)"; cases[2].f.shaped = true; cases[2].f.shapeY = 16.0f; cases[2].f.shapeStrength = 1.3f;
	cases[3].name = "HDR on (dimmer kept)"; cases[3].f.hdr = true;
	const float span = 700.0f;   // the grid: x, y in [-700, 700] round the lamp, on the ceiling
	for (const Case& cs : cases) {
		LampCheckConstants k = {};
		put4(k.size, float(S), float(S), 1.0f, 0.0f);
		put4(k.origin, V3(-span, -span, ceilingZ), 0.0f);
		put4(k.axisU, V3(2 * span, 0, 0), 0.0f);
		put4(k.axisV, V3(0, 2 * span, 0), 0.0f);
		put4(k.normal, V3(0, 0, -1), 0.0f);
		float packed[8];
		RtReflLamps::Pack(raw, cs.f, eye, packed);
		std::memcpy(k.lamps[0], packed, 16); std::memcpy(k.lamps[1], packed + 4, 16);
		h.AddPass(gR2.lampCheck, { c.aux16 }, 0x1u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(World(), {}, V3())) return false;
		std::vector<float> got;
		if (!h.Read(c.aux16, got)) return false;
		h.EndFrame();
		double worst = 0; int n = 0, zeroBad = 0;
		float peakGot = 0, peakTruth = 0, peakOld = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 p = V3(-span, -span, ceilingZ) + V3(2 * span * (i + 0.5f) / S, 2 * span * (j + 0.5f) / S, 0);
			for (int ch = 0; ch < 3; ch++) {
				const float truth = gameLight(cs.f, p, ch), g = got[(j * S + i) * 4 + ch];
				if (truth > 1e-3f) { worst = std::max(worst, std::fabs(double(g) / truth - 1.0)); n++; }
				else if (g > 2e-3f) zeroBad++;
				if (ch == 0) { if (truth > peakTruth) { peakTruth = truth; peakGot = g; } peakOld = std::max(peakOld, oldLight(p, 0)); }
			}
		}
		const bool ok = n > 1000 && worst < 0.005 && zeroBad == 0;
		say("  A %-44s %d values: worst error vs the game's light %.3f %% (limit 0.5 %%), %d lit where the game is dark; red over the lamp: %.3f "
			"(game %.3f; the old estimate %.3f = %.2f x) -> %s\n", cs.name, n, worst * 100, zeroBad, peakGot, peakTruth, peakOld,
			peakTruth > 0 ? peakOld / peakTruth : 0.0f, ok ? "ok" : "BAD");
		allOk = allOk && ok;
		if (!ok) dumpPfm("reflorbs_A_r", got, S, S, 4, 0);
	}

	// (C) the flashlight lights RT hits (HitFlashlight: lamp 0, RtSpotLight) as FlashlightDeferred.hlsl lights the screen's pixels: a beam
	// pointing up at the ceiling from 300 units under it, the room's ceiling lamp beside it (lamp 1); Falloff 0 (1 - (d/r)^2) and 4 (the
	// windowed inverse square). Truth: colour^(2.2 / gamma) x the pass's falloff x its cone^2 (smoothstep(cos outer, cos outer / 2)) x N.L
	// + the ceiling lamp's game light.
	for (float falloffF : { 0.0f, 4.0f }) {
		const V3 fpos = { 120, -80, ceilingZ - 300 };
		const V3 faxis = normalize(V3(0.2f, 0.1f, 1.0f));
		const float outer = 30.0f * 0.0174532925f, freach = 1000.0f;
		const float fcol[3] = { 0.8f, 0.9f, 1.0f }, gammaG = 1.4f;   // (the tester's linear-lighting gamma: the colour re-encoded to 2.2)
		LampCheckConstants k = {};
		const float rel[3] = { fpos.x, fpos.y, fpos.z };
		float fp[8], lp[8];
		RtReflLamps::PackFlashlight(rel, freach, fcol, falloffF, gammaG, fp);
		RtReflLamps::Pack(raw, cases[0].f, eye, lp);
		const float code = RtFlashlightCode::Encode(faxis.x, faxis.y, faxis.z, std::cos(outer));
		put4(k.size, float(S), float(S), 2.0f, 1.0f);   // two lamps, the flashlight lamp 0
		put4(k.origin, V3(-span, -span, ceilingZ), code);
		put4(k.axisU, V3(2 * span, 0, 0), 0.0f); put4(k.axisV, V3(0, 2 * span, 0), 0.0f); put4(k.normal, V3(0, 0, -1), 0.0f);
		std::memcpy(k.lamps[0], fp, 16); std::memcpy(k.lamps[1], fp + 4, 16); std::memcpy(k.lamps[2], lp, 16); std::memcpy(k.lamps[3], lp + 4, 16);
		h.AddPass(gR2.lampCheck, { c.aux16 }, 0x1u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(World(), {}, V3())) return false;
		std::vector<float> got;
		if (!h.Read(c.aux16, got)) return false;
		h.EndFrame();
		// the cone: in full (inside half its angle) and outside it exactly as the pass's (the exact axis; points within 1.5 degrees of those
		// two edges skipped: the code's 9-bit axis); between them through the code's decode (RtFlashlightCode::Spot, the CPU's twin of
		// RtSpotCone), the twin's own difference from the exact smoothstep there printed
		double worst = 0, twinOff = 0; int n = 0, beam = 0, skipped = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 p = V3(-span, -span, ceilingZ) + V3(2 * span * (i + 0.5f) / S, 2 * span * (j + 0.5f) / S, 0);
			const V3 toL = fpos - p;
			const float d = length(toL);
			const V3 t = toL * (-1.0f / d);
			const float ang = std::acos(std::min(1.0f, std::max(-1.0f, dot(t, faxis))));
			if (std::fabs(ang - outer) < 0.026f || std::fabs(ang - 0.5f * outer) < 0.026f) { skipped++; continue; }
			const float coneCos = std::cos(ang), lo = std::cos(outer), hi = std::cos(0.5f * outer);
			float s = std::min(std::max((coneCos - lo) / (hi - lo), 0.0f), 1.0f); s = s * s * (3 - 2 * s);
			if (s > 0.0f && s < 1.0f) {
				const float twin = RtFlashlightCode::Spot(code, t.x, t.y, t.z);
				twinOff = std::max(twinOff, double(std::fabs(twin - s)));
				s = twin;
			}
			const float ndl = std::max(-toL.z / d, 0.0f);   // (the ceiling's normal points down)
			const float fall = d < freach ? RtReflLamps::FlashFall(d, freach, falloffF) : 0.0f;
			if (s * fall * ndl > 1e-3f) beam++;
			for (int ch = 0; ch < 3; ch++) {
				const float truth = std::pow(fcol[ch], 2.2f / gammaG) * fall * s * s * ndl + gameLight(cases[0].f, p, ch);
				const float g = got[(j * S + i) * 4 + ch];
				if (truth > 1e-3f) { worst = std::max(worst, std::fabs(double(g) / truth - 1.0)); n++; }
			}
		}
		const bool ok = beam > 300 && worst < 0.006;
		say("  C the flashlight at the hits (Falloff %.0f, gamma 1.4): %d values, %d in its beam, worst error vs FlashlightDeferred.hlsl's light + the "
			"lamp's %.3f %% (limit 0.6 %%; %d points by the cone's edges skipped; the code's cone vs the exact one between them: %.3f at most) -> %s\n",
			falloffF, n, beam, worst * 100, skipped, twinOff, ok ? "ok" : "BAD");
		allOk = allOk && ok;
		if (!ok) dumpPfm("reflorbs_C_g", got, S, S, 4, 1);
	}

	// (B) the helper's key-lamp pick with the packed lamps
	{
		World w;
		Mesh floor; floor.quad({ -2000, -2000, 0 }, { 2000, -2000, 0 }, { 2000, 2000, 0 }, { -2000, 2000, 0 });
		Mesh ceiling; ceiling.quad({ -2000, -2000, ceilingZ }, { -2000, 2000, ceilingZ }, { 2000, 2000, ceilingZ }, { 2000, -2000, ceilingZ });
		w.meshes = { floor, ceiling };
		Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
		Object cl; cl.mesh = 1; cl.albedo = { 0.6f, 0.55f, 0.5f };
		w.objects = { f, cl };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const Camera cam = Camera::lookDir({ 0, -700, 120 }, normalize({ 0, 1, -0.35f }), { 0, 0, 1 }, 1.3f, 1.3f);
		std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!hh.hit || hh.object != 0) continue;
			const V3 v = normalize(d), r = v - hh.n * (2 * dot(v, hh.n));
			octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
			ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;
		}
		if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 6000.0f; k.size[3] = 1.0f;   // Distance; shadow rays on
		put4(k.sun, normalize({ 0.3f, -0.3f, 0.9f }), 0.0f);   // night: the lamp branch
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		// three lamps: a bright flashlight right under the ceiling pointing up (lamp 0: frame.z 1, its cone frame.w; never the key lamp: no
		// shadow ray), the ceiling lamp (lamp 1) and a dim far one (lamp 2)
		const float eyeF[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
		float a[8], b[8], fl[8];
		RtReflLamps::Pack(raw, RtReflLamps::Frame(), eyeF, a);
		RtReflLamps::Raw farLamp = raw; farLamp.pos[0] = 1500; farLamp.pos[1] = 1500; farLamp.diffuse[0] = farLamp.diffuse[1] = farLamp.diffuse[2] = 0.2f;
		RtReflLamps::Pack(farLamp, RtReflLamps::Frame(), eyeF, b);
		{ const float rel[3] = { -cam.eye.x, -cam.eye.y, ceilingZ - 40 - cam.eye.z }, col[3] = { 4, 4, 4 }; RtReflLamps::PackFlashlight(rel, 1000.0f, col, 0.0f, 2.2f, fl); }
		std::memcpy(k.lamps[0], fl, 16); std::memcpy(k.lamps[1], fl + 4, 16);
		std::memcpy(k.lamps[2], a, 16); std::memcpy(k.lamps[3], a + 4, 16); std::memcpy(k.lamps[4], b, 16); std::memcpy(k.lamps[5], b + 4, 16);
		k.forward[3] = 3.0f;   // three lamps, RoughReach 0
		put4(k.frame, 0.0f, 0.0f, 1.0f, RtFlashlightCode::Encode(0, 0, 1, std::cos(60.0f * 0.0174532925f)));
		h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		std::vector<float> hit, shade;
		const bool read = h.Read(c.refHit, hit) && h.Read(c.refShade, shade);
		h.EndFrame();
		releaseWorld(h, ids);
		if (!read) return false;
		int ceilingHits = 0, keyLamp0 = 0, lit = 0, inReach = 0;
		for (UINT p = 0; p < S * S; p++) {
			if (hit[p * 4] <= 0.0f) continue;
			const V3 d = octDecode(dir[p * 2], dir[p * 2 + 1]);
			const UINT i = p % S, j = p / S;
			const V3 surf = cam.ray((i + 0.5f) / S, (j + 0.5f) / S) * ray[p * 2];
			const V3 at = cam.eye + surf + d * hit[p * 4];
			if (std::fabs(at.z - ceilingZ) > 2.0f) continue;
			ceilingHits++;
			if (length(at - lampPos) > reach * 0.95f) continue;
			inReach++;
			const int key = (int(hit[p * 4 + 1] + 0.5f) >> 4) & 15;
			if (key == 2) keyLamp0++;   // (lamp 1, the ceiling lamp; never 1: the flashlight)
			if (shade[p * 4 + 3] > 0.5f) lit++;
		}
		const bool ok = inReach > 200 && keyLamp0 == inReach && lit == inReach;
		say("  B helper (ReflectionsRT, night, the flashlight + 2 lamps): %d ceiling hits, %d within the ceiling lamp's reach: key lamp = the ceiling lamp (never the flashlight) at %d, its "
			"shadow ray clear at %d -> %s\n", ceilingHits, inReach, keyLamp0, lit, ok ? "ok" : "BAD");
		allOk = allOk && ok;
	}
	return allOk;
}

// ---- reflbounce ----------------------------------------------------------------------------------------------------------------
// A gold sphere (the material rule colouredmetal's middle 0.35 -> code 4 = 0.33) and a chrome one (chrome's middle 0.125 -> code 2 = 0.08)
// in the mirror floor (reflfix C's world). MetalBounce 0.5: (1) frame.y 0 (no denoiser): the bounce is the mirror direction, the same every
// frame (perf-reflfix's); (2) frame.y 1: 32 frames, each pixel's bounce direction vs its frame.y-0 direction: the mean angle = the CPU's
// GGX VNDF lobe of that roughness (Heitz 2018, its own code here) within 20 %, every direction above the hit triangle, chrome still sharp
// (< 1.5 degrees). Also the roughness code: every material class's middle roughness round-trips to the nearest of the 7 steps.
static V3 vndfReflect(V3 v, V3 n, float a, float u1, float u2) {   // v: toward the eye; the reflected direction (the shader's fallbacks)
	const V3 t = normalize(std::fabs(n.z) < 0.999f ? cross(n, V3(0, 0, 1)) : cross(n, V3(1, 0, 0)));
	const V3 b = cross(n, t);
	const V3 vt = { dot(v, t), dot(v, b), dot(v, n) };
	const V3 vh = normalize(V3(a * vt.x, a * vt.y, std::max(vt.z, 1e-4f)));
	const float lensq = vh.x * vh.x + vh.y * vh.y;
	const V3 t1 = lensq > 0 ? V3(-vh.y, vh.x, 0) * (1.0f / std::sqrt(lensq)) : V3(1, 0, 0);
	const V3 t2 = cross(vh, t1);
	const float r = std::sqrt(u1), ph = 6.2831853f * u2;
	const float p1 = r * std::cos(ph), s = 0.5f * (1 + vh.z);
	const float p2 = (1 - s) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + s * r * std::sin(ph);
	const V3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
	const V3 ht = normalize(V3(a * nh.x, a * nh.y, std::max(0.0f, nh.z)));
	const V3 m = normalize(t * ht.x + b * ht.y + n * ht.z);
	const V3 d = v * -1.0f;
	const V3 out = d - m * (2 * dot(d, m));
	return dot(out, n) > 0 ? out : d - n * (2 * dot(d, n));
}

static bool sceneReflBounce(Ctx& c) {
	Host& h = *c.host;
	bool allOk = true;
	// the roughness code: the materials file's classes (middle of their range, as Render.cpp RtMaterialRuleOf) -> the nearest step
	{
		const float mids[] = { 0.125f, 0.35f, 0.06f, 0.425f, 0.35f, 0.55f, 0.6f, 0.575f, 0.5f, 0.45f, 0.625f, 0.7f, 0.85f, 0.775f, 0.8f, 0.875f, 0.925f, 0.02f, 1.0f };
		int bad = 0; double errOld = 0, errNew = 0;
		for (float m : mids) {
			const uint32_t k = NvrRtRoughCode(m);
			const float back = NvrRtRoughOf(k);
			for (uint32_t o = 1; o <= 7; o++) if (std::fabs(NvrRtRoughOf(o) - m) < std::fabs(back - m) - 0.03f) bad++;   // (sqrt-space nearest: within 0.03 of the best)
			errNew = std::max(errNew, double(std::fabs(back - m)));
			const int ko = std::min(std::max(int(m * 7 + 0.5f), 1), 7);
			errOld = std::max(errOld, double(std::fabs(ko / 7.0f - m)));
		}
		const bool ok = bad == 0 && NvrRtRoughCode(0.125f) == 2 && NvrRtRoughCode(0.02f) == 1 && NvrRtRoughCode(1.0f) == 7 && NvrRtRoughCode(0.0f) == 1;
		say("  roughness code (k / 7)^2: chrome 0.125 -> %.3f (was 0.143), glass 0.06 -> %.3f (was 0.143), colouredmetal 0.35 -> %.3f (was 0.286), "
			"a mirror 0.02 -> %.3f (was 0.143); largest error over the classes %.3f (was %.3f) -> %s\n", NvrRtRoughOf(NvrRtRoughCode(0.125f)),
			NvrRtRoughOf(NvrRtRoughCode(0.06f)), NvrRtRoughOf(NvrRtRoughCode(0.35f)), NvrRtRoughOf(NvrRtRoughCode(0.02f)), errNew, errOld, ok ? "ok" : "BAD");
		allOk = allOk && ok;
	}
	const V3 gold = { -320, 500, 230 }, chrome = { 320, 500, 230 };
	const float radius = 200.0f;
	const Camera cam = Camera::lookDir({ 0, -1600, 700 }, normalize({ 0, 1, -0.45f }), { 0, 0, 1 }, 1.3f, 1.3f);
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh wall; wall.box({ -1500, 1400, 0 }, { 1500, 1450, 900 });
	World w; w.meshes = { floor, uvSphere(gold, radius, 48, 24), uvSphere(chrome, radius, 48, 24), wall };
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.5f, 0.5f };
	Object g; g.mesh = 1; g.albedo = { 0.95f, 0.75f, 0.35f }; g.textured = true; g.alphaSlot = 5; g.rough = 0.35f; g.metal = 1.0f;
	Object cr; cr.mesh = 2; cr.albedo = { 0.96f, 0.96f, 0.97f }; cr.textured = true; cr.alphaSlot = 5; cr.rough = 0.125f; cr.metal = 1.0f;
	Object wl; wl.mesh = 3; wl.albedo = { 0.3f, 0.5f, 0.7f };
	w.objects = { f, g, cr, wl };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
	std::vector<int> sphereOf(S * S, -1);
	std::vector<V3> vIn(S * S), nTrue(S * S), nFlat(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit || hh.object != 0) continue;
		const V3 v = normalize(d), r = v - hh.n * (2 * dot(v, hh.n));
		octEncode(r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
		ray[(j * S + i) * 2] = hh.t; ray[(j * S + i) * 2 + 1] = 2.0f;
		// the floor ray's first hit: one of the spheres? (the CPU, as reflfix C's cpuRay)
		const V3 surface = d * hh.t;
		const V3 half = r - normalize(surface);
		const V3 start = surface + half * ((0.5f + hh.t * 0.002f) / length(half));
		const Hit rh = trace(w, cam.eye + start, r, 1.0f + hh.t * 0.0015f, 10000.0f, 0xEF, false);
		if (!rh.hit || (rh.object != 1 && rh.object != 2)) continue;
		const V3 p = cam.eye + start + r * rh.t;
		const V3 n = normalize(p - (rh.object == 1 ? gold : chrome));
		if (dot(n, r) > -0.2f) continue;   // (near the outline: the interpolated normal leaves the exact one most)
		sphereOf[j * S + i] = rh.object; vIn[j * S + i] = r * -1.0f; nTrue[j * S + i] = n; nFlat[j * S + i] = dot(rh.n, r) < 0 ? rh.n : rh.n * -1.0f;
	}
	if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) { releaseWorld(h, ids); return false; }
	auto pass = [&](float frameX, float jitter, std::vector<float>& hit) {
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 10000.0f; k.size[3] = 0.0f;
		k.up[3] = 0.5f;   // MetalBounce
		put4(k.sun, normalize({ 0.3f, -0.3f, 0.9f }), 1.0f);
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		put4(k.frame, frameX, jitter, 0.0f, 0.0f);
		h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade, gRF.bounce }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		const bool ok = h.Read(c.refHit, hit);
		h.EndFrame();
		return ok;
	};
	std::vector<float> base0, base1;
	if (!pass(1.0f, 0.0f, base0) || !pass(2.0f, 0.0f, base1)) { releaseWorld(h, ids); return false; }
	int bounced[3] = {}, same = 0, total = 0;
	for (UINT p = 0; p < S * S; p++) {
		if (sphereOf[p] < 0 || !(int(base0[p * 4 + 1] + 0.5f) & 256)) continue;
		total++;
		if (base0[p * 4 + 2] == base1[p * 4 + 2] && base0[p * 4 + 3] == base1[p * 4 + 3]) same++;
	}
	const int F = 32;
	std::vector<double> sumAngle(S * S, 0.0);
	std::vector<V3> sumDir(S * S, V3(0, 0, 0));
	int below = 0;
	for (int fr = 0; fr < F; fr++) {
		std::vector<float> hit;
		if (!pass(float(100 + fr * 37), 2.0f, hit)) { releaseWorld(h, ids); return false; }
		for (UINT p = 0; p < S * S; p++) {
			if (sphereOf[p] < 0 || !(int(base0[p * 4 + 1] + 0.5f) & 256) || !(int(hit[p * 4 + 1] + 0.5f) & 256)) continue;
			// (2026-11-08 perf-reflfix3: against the mirror direction about the exact normal: the frame.y 0 run is the fixed lobe point now, not the mirror)
			const V3 d0 = vIn[p] * -1.0f - nTrue[p] * (2 * dot(vIn[p] * -1.0f, nTrue[p])), d = octDecode(hit[p * 4 + 2], hit[p * 4 + 3]);
			sumAngle[p] += angleDeg(d, d0);
			sumDir[p] = sumDir[p] + d;
			if (dot(d, nFlat[p]) < -1e-3f) below++;   // (the shader keeps it above the hit TRIANGLE: its flat normal)
			if (fr == 0) bounced[sphereOf[p]]++;
		}
	}
	releaseWorld(h, ids);
	// the CPU's lobe: the same pixels, 256 samples each, about the exact normal
	uint32_t rng = 12345u;
	auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) / 16777216.0f; };
	double gpuMean[3] = {}, cpuMean[3] = {}, meanDirOff[3] = {};
	int count[3] = {};
	for (UINT p = 0; p < S * S; p++) {
		const int s = sphereOf[p];
		if (s < 0 || !(int(base0[p * 4 + 1] + 0.5f) & 256)) continue;
		const float rough = NvrRtRoughOf(NvrRtRoughCode(s == 1 ? 0.35f : 0.125f)), a = rough * rough;
		const V3 mirror = vIn[p] * -1.0f - nTrue[p] * (2 * dot(vIn[p] * -1.0f, nTrue[p]));
		double cpu = 0;
		for (int k = 0; k < 256; k++) cpu += angleDeg(vndfReflect(vIn[p], nTrue[p], a, rnd(), rnd()), mirror);
		cpuMean[s] += cpu / 256; gpuMean[s] += sumAngle[p] / F; count[s]++;
		meanDirOff[s] += angleDeg(sumDir[p], vIn[p] * -1.0f - nTrue[p] * (2 * dot(vIn[p] * -1.0f, nTrue[p])));
	}
	for (int s = 1; s <= 2; s++) if (count[s]) { cpuMean[s] /= count[s]; gpuMean[s] /= count[s]; meanDirOff[s] /= count[s]; }
	const bool okStill = total > 200 && same == total;
	const bool okGold = count[1] > 100 && std::fabs(gpuMean[1] / std::max(cpuMean[1], 1e-6) - 1.0) < 0.2 && gpuMean[1] > 2.0;
	// (2026-11-08 perf-reflfix3) chrome (class 0, roughness under 0.1) bounces along the exact mirror direction now: no spread at all
	const bool okChrome = count[2] > 100 && gpuMean[2] < 0.6;   // (the smooth normal's own error vs the exact sphere: reflfix A, ~0.1-0.3 deg)
	const bool ok = okStill && okGold && okChrome && below == 0;
	say("  MetalBounce without a denoiser (frame.y 0): %d sphere pixels bounced, the same direction two frames running at %d -> %s\n", total, same, okStill ? "ok" : "BAD");
	say("  the lobe's per-frame random sample (frame.y 2: test 11's, kept as a test; the game now uses the fixed 9 points, scene reflshimmer), %d frames: gold (code %u = %.3f) %d pixels, mean angle from the mirror direction %.2f deg (the CPU's GGX lobe "
		"%.2f), the frames' mean direction %.2f deg off; chrome (code %u = %.3f) %d pixels, %.2f deg (CPU %.2f); below the surface %d -> %s\n",
		F, NvrRtRoughCode(0.35f), NvrRtRoughOf(NvrRtRoughCode(0.35f)), count[1], gpuMean[1], cpuMean[1], meanDirOff[1], NvrRtRoughCode(0.125f),
		NvrRtRoughOf(NvrRtRoughCode(0.125f)), count[2], gpuMean[2], cpuMean[2], below, okGold && okChrome && below == 0 ? "ok" : "BAD");
	(void)bounced;
	return allOk && ok;
}

// ---- ptflashlight --------------------------------------------------------------------------------------------------------------
// A dark room (floor + walls + ceiling, no sun, no sky), the flashlight as lamp 0 as RtPathTrace.cpp Lamps makes it (RtFlashlightCode::
// Encode, the frame row's texel 15 with the lamp image, lampColor[0].w without), a 30-degree cone (the tester's log: reach 1000, colour
// 2.267 4 4) from just beside the eye toward the floor ahead (the only lamp: the direct light is exact per path). PTDebugView 3 (the first hit's direct
// light): inside half the cone = albedo x colour x the game's falloff x N.L (within 3 %), outside the cone 0 (no light of the flashlight
// at all), in every variant: (1) no lamp image, (2) the lamp image, (3) + PTLightGrid (RtPathTraceGrid,
// the game's RtLightGrid::Build), (4) + PTRestirDI (RtRestirRow), (5) RtPathTraceGI (PTRestirGI's tracer), each with the ray budget's
// defaults (PTRestirDIRays 0, PTBounceLights 0, one camera query: RtPtCost). (6) The picture (PTDebugView 0, 3 bounces, ReSTIR DI) over 32
// frames: the beam's floor = its direct light + the room's bounce light (1 .. 1.35 x the direct), beside the beam well under it (bounce only).
static bool scenePtFlashlight(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -1500, -1500, 0 }, { 1500, -1500, 0 }, { 1500, 1500, 0 }, { -1500, 1500, 0 }); floor.uv.clear();
	Mesh room; room.box({ -1500, -1500, 400 }, { 1500, 1500, 440 }); room.uv.clear();   // the ceiling
	Mesh back; back.box({ -1500, 1450, 0 }, { 1500, 1500, 400 }); back.uv.clear();
	Mesh left; left.box({ -1500, -1500, 0 }, { -1450, 1500, 400 }); left.uv.clear();
	Mesh right; right.box({ 1450, -1500, 0 }, { 1500, 1500, 400 }); right.uv.clear();
	Mesh front; front.box({ -1500, -1500, 0 }, { 1500, -1450, 400 }); front.uv.clear();
	w.meshes = { floor, room, back, left, right, front };
	for (int m = 0; m < 6; m++) { Object o; o.mesh = m; o.albedo = m == 0 ? V3(0.5f, 0.5f, 0.5f) : V3(0.4f, 0.4f, 0.4f); w.objects.push_back(o); }
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -900, 170 }, normalize({ 0, 1, -0.35f }), { 0, 0, 1 }, 1.0f, 1.0f);
	const V3 flashPos = cam.eye + cam.r * 8.0f - cam.u * 10.0f;   // (beside the eye, as the weapon's light)
	const V3 axis = normalize(V3(0, 350, 0) - flashPos);   // toward the floor ahead
	const float cone = 30.0f, reachF = 1000.0f;
	const V3 flashColour = { 2.267f, 4.0f, 4.0f };
	const float code = RtFlashlightCode::Encode(axis.x, axis.y, axis.z, std::cos(cone * 0.0174532925f));
	const V3 dimPos = { 0, 1300, 300 }; const float dimReach = 900.0f; const V3 dimColour = { 0.0f, 0.0f, 0.0f };   // (unused: the list holds the flashlight only)
	// the lamps relative to the eye: lamp 0 the flashlight (ONE lamp: the first hit's direct light is then exact per path, the pick's weight cancels)
	struct L { V3 pos; float reach; V3 colour; float w; };
	const L lamps[2] = { { flashPos - cam.eye, reachF, flashColour, code }, { dimPos - cam.eye, dimReach, dimColour, 0.0f } };
	const int n = 1;
	// the CPU's direct light at a floor point (scene space), view 3: albedo x sum of colour x fall x N.L x spot; the flashlight's share apart
	auto lampLight = [&](int l, V3 at, float& spot) {
		const V3 Lv = lamps[l].pos - at;
		const float d = length(Lv);
		spot = 1.0f;
		if (d >= lamps[l].reach || d < 1e-3f) return 0.0f;
		const float x = d / lamps[l].reach, fall = std::pow(std::max(1 - x * x, 0.0f), 2.2f), ndl = std::max(Lv.z / d, 0.0f);
		if (l == 0) { const V3 t = Lv * (-1.0f / d); spot = RtFlashlightCode::Spot(lamps[l].w, t.x, t.y, t.z); }
		return fall * ndl * spot;
	};
	auto image = [&](bool grid, PathConstants& k, std::vector<float>& img, bool restir) {
		img.assign(size_t(kLightTexels) * kLampRows * 2, 0.0f);
		std::vector<float> list(size_t(n) * 8);
		for (int l = 0; l < n; l++) {
			const float v[8] = { lamps[l].pos.x, lamps[l].pos.y, lamps[l].pos.z, lamps[l].reach, lamps[l].colour.x, lamps[l].colour.y, lamps[l].colour.z, 0.0f };
			std::memcpy(&img[size_t(l) * 8], v, sizeof(v)); std::memcpy(&list[size_t(l) * 8], v, sizeof(v));
		}
		float* frameRow = &img[size_t(kLightTexels) * kFrameRowIndex * 2];
		if (restir) {
			static RtRestirRow::State state;
			RtRestirRow::Keys keys; keys.radius = 6.0f; keys.spatial = 2.0f;
			const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
			const float eye[3] = { 0, 0, 0 };
			RtRestirRow::Build(state, frameRow, img.data(), list.data(), n, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
		}
		frameRow[30] = code; frameRow[31] = 0.0f;   // (RtPathTrace.cpp: texel 15, after RtRestirRow's 0-14)
		RtPtCost::Keys cost; cost.restirRays = 0; cost.bounceLights = 0; cost.oneCameraQuery = 1;   // (the defaults)
		RtPtCost::PackRow(frameRow, cost);
		if (grid) {
			RtLightGrid::Info info;
			std::vector<float> gridData(size_t(kLightTexels) * kGridRows * 2, 0.0f);
			if (!RtLightGrid::Build(list.data(), n, 8, gridData.data(), int(kLightTexels * kGridRows), int(kLightTexels), k.grid, info)) return false;
			std::memcpy(&img[size_t(kLightTexels) * 2], gridData.data(), gridData.size() * sizeof(float));
		}
		return true;
	};
	auto frame = [&](int variant, float view, float bounces, float seed, std::vector<float>& out) {
		// variant: 1 no lamp image, 2 lamp image, 3 + grid, 4 + ReSTIR DI, 5 the GI tracer, 6 the picture (ReSTIR DI)
		PathConstants k = PathConstants();
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = seed;
		const bool lampImage = variant != 1, grid = variant == 3, restir = variant == 4 || variant == 6;
		const uint32_t flags = 2048u | (lampImage ? 1024u : 0u) | (grid ? 4096u : 0u) | (restir ? 262144u | 1048576u : 0u);
		put4(k.accum, 0.0f, bounces, 4.0f, float(flags));
		put4(k.sunDir, 0, 0, 1, 0); put4(k.sunColor, 0, 0, 0, 0);
		put4(k.skyZenith, 0, 0, 0, 0); put4(k.skyHorizon, 0, 0, 0, 0);
		put4(k.misc, float(n), 0.0f, 60000.0f, view);
		k.rect[3] = 1.0f;
		for (int l = 0; l < n && l < 8; l++) { put4(k.lampPos[l], lamps[l].pos, lamps[l].reach); put4(k.lampColor[l], lamps[l].colour, lamps[l].w); }
		std::vector<float> img;
		if (!image(grid, k, img, restir)) { say("  the grid builder refused\n"); return false; }
		if (!h.Upload(gM.ptLamps, img)) return false;
		std::vector<int> imgs{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
			c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		uint32_t mask = 0x7u;
		int shader = grid ? gM.shPathGrid : c.shPath;
		if (variant == 5) { imgs.push_back(gM.giResult); mask |= (1u << 13) | (1u << 15); shader = gM.shPathGI; }
		h.AddPass(shader, imgs, mask, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		const bool ok = h.Read(variant == 5 ? gM.giResult : c.ptDisplay, out);   // (the GI tracer: the first hit's direct light in giDirect, the display holds its bounce sample)
		h.EndFrame();
		return ok;
	};
	bool allOk = true;
	double directIn = 0;   // (variant 4's mean green inside the beam: the picture's check)
	const char* names[6] = { "no lamp image (lampColor[0].w)", "the lamp image (texel 15)", "PTLightGrid", "PTRestirDI", "PTRestirGI's tracer", "" };
	for (int variant = 1; variant <= 5; variant++) {
		std::vector<float> out;
		if (!frame(variant, 3.0f, 3.0f, 17.0f + variant, out)) { releaseWorld(h, ids); return false; }
		int inside = 0, outside = 0, badIn = 0, badOut = 0, shown = 0, nan = 0;
		double worstIn = 0, meanIn = 0, meanOut = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const float* g = &out[(j * S + i) * 4];
			if (!std::isfinite(g[0]) || !std::isfinite(g[1]) || !std::isfinite(g[2])) { nan++; continue; }
			// the pixel's footprint: 5 points; it counts as inside when the spot is 1 at all, outside when 0 at all (the floor only)
			float spotLo = 1, spotHi = 0, flashLo = 1e30f, flashHi = 0, dimLo = 1e30f, dimHi = 0;
			bool floorAll = true;
			for (int s = 0; s < 5; s++) {
				const V3 d = s == 0 ? cam.ray((i + 0.5f) / S, (j + 0.5f) / S) : cam.ray((i + (s & 1 ? 0.95f : 0.05f)) / S, (j + (s & 2 ? 0.95f : 0.05f)) / S);
				const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, false);
				if (!hh.hit || hh.object != 0) { floorAll = false; break; }
				const V3 at = d * hh.t;
				float sp = 1, spd = 1;
				const float fl = lampLight(0, at, sp), dm = lampLight(1, at, spd);
				spotLo = std::min(spotLo, sp); spotHi = std::max(spotHi, sp);
				flashLo = std::min(flashLo, fl); flashHi = std::max(flashHi, fl); dimLo = std::min(dimLo, dm); dimHi = std::max(dimHi, dm);
			}
			if (!floorAll) continue;
			if (spotLo >= 0.999f) {
				inside++;
				for (int ch = 0; ch < 3; ch++) {
					const float c3 = (&flashColour.x)[ch], d3 = (&dimColour.x)[ch];
					const float lo = 0.5f * (c3 * flashLo + d3 * dimLo), hi = 0.5f * (c3 * flashHi + d3 * dimHi);
					// (relative to the larger of the pixel's light and 0.01: the GI tracer jitters its one path over the whole pixel, past the 5 points' range, at the beam's faint far end)
					const float err = (g[ch] < lo ? lo - g[ch] : g[ch] > hi ? g[ch] - hi : 0.0f) / std::max(hi, 0.01f);
					worstIn = std::max(worstIn, double(err));
					if (err > 0.03f) { badIn++; if (shown++ < 4) say("    pixel (%u, %u) inside: %.4f, want %.4f .. %.4f\n", i, j, g[ch], lo, hi); }
				}
				meanIn += g[1];
			}
			else if (spotHi <= 0.0f) {
				outside++;
				for (int ch = 0; ch < 3; ch++) {
					const float d3 = (&dimColour.x)[ch], lo = 0.5f * d3 * dimLo, hi = 0.5f * d3 * dimHi;
					if (g[ch] > hi * 1.02f + 1e-4f || g[ch] < lo * 0.98f - 1e-4f) { badOut++; if (shown++ < 4) say("    pixel (%u, %u) outside: %.5f, want %.5f .. %.5f\n", i, j, g[ch], lo, hi); }
				}
				meanOut += g[1];
			}
		}
		const bool ok = nan == 0 && inside > 300 && outside > 300 && badIn == 0 && badOut == 0;
		say("  %d %-32s first hit's direct light: %d pixels inside the beam (worst %.2f %%, mean green %.3f), %d outside (mean green %.5f), "
			"%d / %d wrong, NaN %d -> %s\n", variant, names[variant - 1], inside, worstIn * 100, inside ? meanIn / inside : 0.0, outside, outside ? meanOut / outside : 0.0,
			badIn, badOut, nan, ok ? "ok" : "BAD");
		if (variant == 4 && inside) directIn = meanIn / inside;
		allOk = allOk && ok;
		if (!ok) dumpPfm("ptflashlight_g", out, S, S, 4, 1);
	}
	// (6) the picture, 32 frames, 3 bounces, ReSTIR DI (the default with PTRealtime)
	{
		std::vector<double> acc(size_t(S) * S, 0.0);
		int nan = 0;
		for (int f = 0; f < 32; f++) {
			std::vector<float> out;
			if (!frame(6, 0.0f, 3.0f, float(1000 + f * 7919), out)) { releaseWorld(h, ids); return false; }
			for (UINT p = 0; p < S * S; p++) { if (!std::isfinite(out[p * 4 + 1])) { nan++; continue; } acc[p] += out[p * 4 + 1] / 32.0; }
		}
		double in = 0, outv = 0; int ni = 0, no = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, false);
			if (!hh.hit || hh.object != 0) continue;
			float sp = 1;
			lampLight(0, d * hh.t, sp);
			if (sp >= 0.999f) { in += acc[j * S + i]; ni++; }
			else if (sp <= 0.0f && length(d * hh.t - (flashPos - cam.eye)) < 900.0f) { outv += acc[j * S + i]; no++; }
		}
		in = ni ? in / ni : 0; outv = no ? outv / no : 0;
		// the beam's floor = its direct light (variant 4) + the room's bounce light (a closed grey room: a few to 30 %); beside the beam only
		// the bounce light (well under the beam's)
		const double ratio = directIn > 0 ? in / directIn : 0.0;
		const bool ok = nan == 0 && ni > 300 && no > 100 && ratio > 0.98 && ratio < 1.35 && outv < 0.15 * in;
		say("  6 the picture (3 bounces, PTRestirDI, 32 frames): the beam's floor %.4f = %.3f x its direct light (limit 0.98 .. 1.35), the floor "
			"beside it (within 900 units, bounce light only) %.5f = %.3f x the beam's (limit 0.15) -> %s\n", in, ratio, outv, in > 0 ? outv / in : 0.0, ok ? "ok" : "BAD");
		allOk = allOk && ok;
	}
	releaseWorld(h, ids);
	return allOk;
}
