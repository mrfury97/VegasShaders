// NvrRtHost, the RT shadow denoiser's scenes (2026-11-07, docs/next/RT-DENOISER.md; included by rt_host.cpp before main).
// The denoiser ("ShadowDenoiser 1": RtLampTraceSigma + the helper's RtShadowDenoise module, the sun through RtSunShadow + the same module)
// against the EXACT soft shadow: each pixel's visible share of the lamp's disc (or the sun's), from many rays per pixel on the CPU with the
// shader's own sampling (the same disc, start offset and masks). Errors are reported for the raw one-ray signal, the old paths
// (RtLampFilter over one ray; 8 rays unfiltered) and the denoiser, over three regions: everything lit by the lamp, the penumbrae
// (0.02 < exact < 0.98), and the contact zone (floor points next to a pole's foot, where the shadow must stay hard).

#include "RtShadowDenoise.h"   // the game's constants + layer picking (src/core)

struct SigmaCtx {
	int signal = -1;                          // RtLampTraceSigma's per-lamp signal (S x 2S, RG16F: two layers)
	int shSigma = -1, shDenoise = -1, shFilter = -1;
};
static SigmaCtx gS;

static bool registerSigma(Host& h, const std::string& spv) {
	gS.signal = h.AddImage("sigma signal", S, 2 * S, NVR_EXT_SHARED_RG16F);
	gS.shSigma = h.AddShader(spv + "\\RtLampTraceSigma.spv");
	gS.shDenoise = h.AddShader(spv + "\\RtShadowDenoise.spv");
	gS.shFilter = h.AddShader(spv + "\\RtLampFilter.spv");
	return gS.signal >= 0;
}

// A floor, a pole standing on it (contact shadow at its foot), a plate floating above it (a wide penumbra), a second pole
static World sigmaWorld() {
	World w;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 }); w.meshes.push_back(floor);
	Mesh pole; pole.box({ -10, -10, 0 }, { 10, 10, 170 }); w.meshes.push_back(pole);
	Mesh plate; plate.box({ -60, -45, 0 }, { 60, 45, 5 }); w.meshes.push_back(plate);
	Object f; f.mesh = 0; f.land = true; f.id = 0; w.objects.push_back(f);
	Object p; p.mesh = 1; p.offset = { 0, 0, 0 }; p.id = 1; w.objects.push_back(p);
	Object q; q.mesh = 2; q.offset = { 150, 60, 110 }; q.id = 2; w.objects.push_back(q);
	Object p2; p2.mesh = 1; p2.offset = { -120, 90, 0 }; p2.id = 3; w.objects.push_back(p2);
	return w;
}

// The exact visible share of a lamp's disc from a surface point (RtLampTraceSigma's sampling with n x n stratified rays); -1 = the lamp
// does not light this point (out of reach, behind the surface)
static float lampShareExact(const World& w, V3 origin, V3 normal, const Lamp& l, float offset, int n) {
	const V3 to = l.pos - origin;
	const float d2 = dot(to, to);
	if (!(l.reach > 0) || d2 >= l.reach * l.reach || dot(to, normal) <= 0) return -1.0f;
	const float dist = std::sqrt(d2);
	if (!(dist > l.stop + offset)) return 1.0f;
	const V3 dir = to * (1.0f / dist);
	const V3 t = normalize(cross(dir, std::fabs(dir.z) < 0.9f ? V3(0, 0, 1) : V3(1, 0, 0))), bt = cross(dir, t);
	int lit = 0;
	for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
		const float rr = l.disc * std::sqrt((a + 0.5f) / n), ang = 6.2831853f * (b + 0.5f) / n;
		const V3 seg = to + (t * std::cos(ang) + bt * std::sin(ang)) * rr;
		const float invLen = 1.0f / length(seg);
		const float tEnd = std::min(std::max(1.0f - l.stop * invLen, 0.0f), 1.0f), tMin = std::min(offset * invLen, tEnd);
		if (!trace(w, origin, seg, tMin, tEnd, 0x13, true).hit) lit++;
	}
	return float(lit) / float(n * n);
}

// A lamp's visibility from the packed result (RtLampTrace's encoding), -1 when the pixel has no entry for that slot
static float entryVisibility(const std::vector<float>& res, UINT i, UINT j, int slot) {
	const float r = res[(j * S + i) * 2], g = res[(j * S + i) * 2 + 1];
	const int e[3] = { int(r) % 1024, int(r) / 1024, int(g) % 1024 };
	for (int k = 0; k < 3; k++) if (e[k] % 64 == slot) return float(e[k] / 64) / 15.0f;
	return -1.0f;
}

static RtShadowDenoise::View viewOf(const Camera& c) {
	RtShadowDenoise::View v;
	const V3 r = c.r * (1.0f / c.p00), u = c.u * (1.0f / c.p11);
	v.forward[0] = c.f.x; v.forward[1] = c.f.y; v.forward[2] = c.f.z;
	v.right[0] = r.x; v.right[1] = r.y; v.right[2] = r.z;
	v.up[0] = u.x; v.up[1] = u.y; v.up[2] = u.z;
	v.eye[0] = c.eye.x; v.eye[1] = c.eye.y; v.eye[2] = c.eye.z;
	v.farZ = c.farZ; v.valid = true;
	return v;
}

// Region errors of one estimate (per lamp, per pixel) against the exact share
struct SigmaErr { double all = 0, pen = 0, contact = 0, bias = 0; int nAll = 0, nPen = 0, nContact = 0; };
static SigmaErr sigmaError(const std::vector<float>& est, const std::vector<float>& exact, const std::vector<uint8_t>& stable, const std::vector<uint8_t>& contact) {
	SigmaErr e;
	double se = 0, sp = 0, sc = 0, bias = 0;
	for (size_t i = 0; i < exact.size(); i++) {
		if (!stable[i % (S * S)] || exact[i] < 0 || est[i] < 0) continue;
		const double d = double(est[i]) - exact[i];
		se += d * d; e.nAll++;
		if (exact[i] > 0.02f && exact[i] < 0.98f) { sp += d * d; bias += d; e.nPen++; }
		if (contact[i % (S * S)]) { sc += d * d; e.nContact++; }
	}
	e.all = std::sqrt(se / std::max(e.nAll, 1)); e.pen = std::sqrt(sp / std::max(e.nPen, 1)); e.contact = std::sqrt(sc / std::max(e.nContact, 1));
	e.bias = bias / std::max(e.nPen, 1);
	return e;
}

static bool sceneSigmaLamps(Ctx& c) {
	Host& h = *c.host;
	const World w = sigmaWorld();
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	// two lamps of physical size (radius 14 and 7 units: a globe, a bulb); reach 2000
	std::vector<Lamp> lamps(2);
	lamps[0].pos = { -40, 230, 290 }; lamps[0].reach = 2000; lamps[0].intensity = 1; lamps[0].radius = 2000; lamps[0].disc = 14;
	lamps[1].pos = { 330, -110, 250 }; lamps[1].reach = 2000; lamps[1].intensity = 0.8f; lamps[1].radius = 2000; lamps[1].disc = 7;
	const float offset = 0.5f;
	auto cameraAt = [](float dx) { return Camera::lookDir({ 40 + dx, -300, 260 }, V3(60 + dx, 60, 0) - V3(40 + dx, -300, 260), { 0, 0, 1 }, 1.0f, 1.0f); };

	RtShadowDenoise::Settings settings;
	if (const char* e = std::getenv("NVR_SIGMA_CORE")) settings.core = std::atoi(e);   // 0 ours, 1 AMD FidelityFX Shadows
	RtShadowDenoise::Layers layers;
	RtShadowDenoise::Candidate cands[2] = { { 0, 2.0f, { 0, 0, 0 } }, { 1, 1.0f, { 1, 0, 0 } } };

	// One frame of the denoiser path (trace + module); fills the packed result and the raw signal
	auto runFrame = [&](const Camera& cam, const Camera* before, unsigned frame, bool freezeMove, std::vector<float>& result, std::vector<float>& signal) -> bool {
		std::vector<GPixel> px; std::vector<float> depth, normals;
		gbuffer(w, cam, px, depth, normals);
		if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals) || !h.Upload(c.lampTable, lampTable(lamps, cam.eye), 0, 0, 1024, 1)) return false;
		layers.Update(cands, 2, 2);
		LampConstants k; lampPassConstants(k, cam, int(lamps.size()), 3);
		put4(k.many, float(layers.count), float(frame), 0.0f, 0.0f);   // sigma: layers, frame
		for (int q = 0; q < 8; q++) k.reproject[q / 4][q % 4] = float(layers.slot[q]);
		const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, gS.signal, c.depth, c.depth, c.depth, c.alphaAtlas };
		h.AddPass(gS.shSigma, imgs, 0x14, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		RtShadowDenoise::View now = viewOf(cam), prev = before ? viewOf(*before) : RtShadowDenoise::View();
		if (freezeMove && before) for (int a = 0; a < 3; a++) prev.eye[a] = now.eye[a];   // the control: history looked up as if nothing moved
		RtShadowDenoise::DenoiseConstants d;
		RtShadowDenoise::Fill(d, now, prev, S, S, S, S, 1, layers.count, frame, settings, false, 0.0f);
		layers.Write(d);
		h.AddPass(gS.shDenoise, { c.depth, c.normals, c.aux16, gS.signal, c.lampResult }, 0x10, &d, sizeof(d), 1, 1);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		const bool ok = h.Read(c.lampResult, result) && h.Read(gS.signal, signal);
		h.EndFrame();
		return ok;
	};
	// The exact shares + which pixels count (no geometry edge) + the contact zone, for a camera
	auto reference = [&](const Camera& cam, int n, std::vector<float>& exact, std::vector<uint8_t>& stable, std::vector<uint8_t>& contact) {
		std::vector<GPixel> px; std::vector<float> depth, normals;
		gbuffer(w, cam, px, depth, normals);
		exact.assign(2 * S * S, -1.0f); stable.assign(S * S, 0); contact.assign(S * S, 0);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const GPixel& g = px[j * S + i];
			if (!g.valid) continue;
			bool st = i > 0 && j > 0 && i + 1 < S && j + 1 < S;
			for (int dj = -1; dj <= 1 && st; dj++) for (int di = -1; di <= 1 && st; di++) {
				const GPixel& q = px[(j + dj) * S + i + di];
				st = q.valid && q.object == g.object && std::fabs(q.viewZ - g.viewZ) < 0.05f * g.viewZ;
			}
			stable[j * S + i] = st;
			// the contact zone: floor within 30 units of either pole's foot
			const float d0 = length(V3(g.pos.x, g.pos.y, 0)), d1 = length(V3(g.pos.x + 120, g.pos.y - 90, 0));
			contact[j * S + i] = g.object == 0 && (d0 < 30.0f || d1 < 30.0f);
			const V3 origin = g.pos + g.n * offset;
			for (int l = 0; l < 2; l++) exact[l * S * S + j * S + i] = lampShareExact(w, origin, g.n, lamps[l], offset, n);
		}
	};
	auto split = [&](const std::vector<float>& result, std::vector<float>& out) {   // the packed result -> per lamp, per pixel
		out.assign(2 * S * S, -1.0f);
		for (int l = 0; l < 2; l++) for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) out[l * S * S + j * S + i] = entryVisibility(result, i, j, l);
	};
	auto rawOf = [&](const std::vector<float>& signal, std::vector<float>& out) {   // the raw signal's layers -> per lamp (layer = slot here)
		out.assign(2 * S * S, -1.0f);
		for (int k = 0; k < 2; k++) for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) out[layers.slot[k] * S * S + j * S + i] = signal[((k * S + j) * S + i) * 2];
	};

	bool ok = true;
	// --- 1. still camera: 48 frames
	const Camera still = cameraAt(0.0f);
	std::vector<float> exact; std::vector<uint8_t> stable, contact;
	const ULONGLONG tRef = GetTickCount64();
	reference(still, 24, exact, stable, contact);
	say("  exact soft shadows: 576 rays a pixel per lamp on the CPU (%.1f s)\n", (GetTickCount64() - tRef) / 1000.0);
	std::vector<float> result, signal, est, raw;
	SigmaErr rawErr, at[6];
	const unsigned marks[6] = { 1, 2, 4, 8, 16, 48 };
	int mark = 0;
	layers.Clear();
	for (unsigned f = 1; f <= 48; f++) {
		static Camera prevStill;
		if (!runFrame(still, f > 1 ? &prevStill : nullptr, f, false, result, signal)) { releaseWorld(h, ids); return false; }
		prevStill = still;
		if (f == 1) { rawOf(signal, raw); rawErr = sigmaError(raw, exact, stable, contact); }
		if (mark < 6 && f == marks[mark]) { split(result, est); at[mark++] = sigmaError(est, exact, stable, contact); }
	}
	dumpPfm("sigma_lamp0_denoised", est, S, S, 1); dumpPfm("sigma_lamp0_exact", exact, S, S, 1); dumpPfm("sigma_lamp0_raw", raw, S, S, 1);
	say("  still, RMSE vs exact (all lit / penumbrae / contact; %d / %d / %d lamp-pixels):\n", rawErr.nAll, rawErr.nPen, rawErr.nContact);
	say("    raw one ray        %.4f / %.4f / %.4f\n", rawErr.all, rawErr.pen, rawErr.contact);
	for (int m = 0; m < 6; m++) say("    denoised frame %2u  %.4f / %.4f / %.4f  (penumbra bias %+.4f)\n", marks[m], at[m].all, at[m].pen, at[m].contact, at[m].bias);

	// --- 2. the old paths on the same frame: one ray + RtLampFilter (the default without TAA), and 8 rays unfiltered
	SigmaErr oldFilter, oldRays8;
	{
		std::vector<Lamp> discs = lamps;
		for (int pass = 0; pass < 2; pass++) {
			std::vector<GPixel> px; std::vector<float> depth, normals;
			gbuffer(w, still, px, depth, normals);
			h.Upload(c.depth, depth); h.Upload(c.normals, normals); h.Upload(c.lampTable, lampTable(discs, still.eye), 0, 0, 1024, 1);
			LampConstants k; lampPassConstants(k, still, 2, 3);
			k.params[0] = pass == 0 ? 1.0f : 8.0f;
			const bool filter = pass == 0;
			const std::vector<int> imgs = { c.depth, c.normals, filter ? c.ptA : c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaAtlas };
			h.AddPass(c.shLamp, imgs, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (filter) {
				struct { float size[4], params[4]; } fc = {};
				put4(fc.size, float(S), float(S), 2.0f, 1.0f);   // FilterStep 2, FilterDepthSigma 1 (the defaults)
				put4(fc.params, 32.0f, 0, 0, 0);                  // FilterNormalPower 32
				h.AddPass(gS.shFilter, { c.depth, c.normals, c.ptA, c.lampResult }, 0x8, &fc, sizeof(fc), (S + 7) / 8, (S + 7) / 8);
			}
			if (!h.Exchange(w, ids, still.eye) || !h.Read(c.lampResult, result)) { releaseWorld(h, ids); return false; }
			h.EndFrame();
			split(result, est);
			(pass == 0 ? oldFilter : oldRays8) = sigmaError(est, exact, stable, contact);
			dumpPfm(pass == 0 ? "sigma_lamp0_filter" : "sigma_lamp0_rays8", est, S, S, 1);
		}
		say("    old: 1 ray + Filter %.4f / %.4f / %.4f\n", oldFilter.all, oldFilter.pen, oldFilter.contact);
		say("    old: 8 rays         %.4f / %.4f / %.4f\n", oldRays8.all, oldRays8.pen, oldRays8.contact);
	}
	const SigmaErr& settled = at[5];
	const bool stillOk = settled.pen < 0.35 * rawErr.pen && settled.pen < oldFilter.pen && settled.pen < oldRays8.pen &&
		std::fabs(settled.bias) < 0.03 && settled.contact < 0.5 * rawErr.contact + 0.02;
	say("  still: denoised (frame 48) penumbra error %.2f x the raw ray's, %.2f x Filter's, %.2f x 8 rays'; contact %.2f x raw -> %s\n",
		settled.pen / std::max(rawErr.pen, 1e-6), settled.pen / std::max(oldFilter.pen, 1e-6), settled.pen / std::max(oldRays8.pen, 1e-6),
		settled.contact / std::max(rawErr.contact, 1e-6), stillOk ? "ok" : "FAIL");
	ok = ok && stillOk;

	// --- 3. moving camera: 32 frames sliding 6 units a frame (history reprojected), and the control with the reprojection frozen
	for (int control = 0; control < 2; control++) {
		layers.Clear();
		Camera prev = cameraAt(0.0f);
		SigmaErr moving[3];
		int mi = 0;
		for (unsigned f = 1; f <= 32; f++) {
			const Camera cam = cameraAt(6.0f * float(f));
			if (!runFrame(cam, f > 1 ? &prev : nullptr, 100 + f, control == 1, result, signal)) { releaseWorld(h, ids); return false; }
			prev = cam;
			if (f == 16 || f == 24 || f == 32) {
				std::vector<float> ex; std::vector<uint8_t> st, co;
				reference(cam, 16, ex, st, co);
				split(result, est);
				moving[mi++] = sigmaError(est, ex, st, co);
				if (f == 32 && control == 0) { rawOf(signal, raw); const SigmaErr r = sigmaError(raw, ex, st, co); say("    moving, raw one ray (frame 32): %.4f / %.4f / %.4f\n", r.all, r.pen, r.contact); }
			}
		}
		say("    moving%s, denoised at frames 16 / 24 / 32: penumbrae %.4f / %.4f / %.4f, all %.4f / %.4f / %.4f\n", control ? " (CONTROL: reprojection frozen)" : "",
			moving[0].pen, moving[1].pen, moving[2].pen, moving[0].all, moving[1].all, moving[2].all);
		static double movingPen = 0, frozenPen = 0;
		(control ? frozenPen : movingPen) = (moving[0].pen + moving[1].pen + moving[2].pen) / 3.0;
		if (control) {
			const bool moveOk = movingPen < 0.5 * rawErr.pen && frozenPen > 1.2 * movingPen;
			say("  moving: penumbra error %.2f x the still raw ray's; the frozen reprojection %.2f x worse -> %s\n", movingPen / std::max(rawErr.pen, 1e-6),
				frozenPen / std::max(movingPen, 1e-9), moveOk ? "ok" : "FAIL");
			ok = ok && moveOk;
		}
	}
	releaseWorld(h, ids);
	return ok;
}
