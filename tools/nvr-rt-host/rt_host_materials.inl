// NvrRtHost, the path tracer's material models (2026-11-07, ptaaa-materials; included by rt_host.cpp after rt_host_pt.inl): glass panes,
// light through leaves, skin's wrap and light through thin parts, cloth sheen (RtPathTrace.comp; the keys in the frame row, the game's
// src/core/RtMaterials.h; the kinds in the records, src/core/RtScenePick.h). README.md has the table; docs/next/RT-PATH-TRACING.md the
// section "materials".
//   (a) FURNACE: an open white box under a uniform sky L with a glass pane over it (the camera looks through it), a white leaf card and a
//       white skin card inside: every path that escapes carries exactly L (glass R + T = 1, leaf / skin R + T = 1 per colour at albedo 1),
//       so the image mean must be L per channel; the same box with the materials off as the baseline.
//   (b) SUN THROUGH A WINDOW: a closed room, a hole in its roof glazed with a tinted pane; the floor's direct light (DebugView 3) against
//       the CPU: albedo x sun x cos x the pane's transmittance (thin slab, Fresnel n 1.5, the tint) where the CPU's sun ray passes the
//       hole; without the pane: untinted; with the pane but PTGlass 0: the pane passed clear (as before).
//   (c) BACK-LIT LEAF: a leaf card seen from below, the sun below (front-lit) and above (back-lit): front = albedo x sun x cos, back =
//       PTFoliage x that; PTFoliage 0: back = 0. The same card as skin: back = 0.25 PTSkin x flesh red x albedo x sun x cos.
//   (d) SKIN WRAP: a skin floor under the sun at 90 / 45 / 15 / 5 degrees: albedo x sun x (c + w) / (1 + w)^2 per colour (w = 0.3, 0.12,
//       0.08), against a plain floor (albedo x sun x c): the difference bounded by 0.25 x albedo x sun.
//   (e) CLOTH SHEEN: a G-buffer floor of class 3 (cloth) vs class 1 under a uniform sky (never above L: the sheen's energy comes out of the
//       diffuse) and under a low sun against the camera (the sheen's rim: brighter than plain).
//   (f) THE CAMERA THROUGH GLASS: a pane between the camera and a uniform sky: every pixel L x (R + T) of the slab at its angle (tinted);
//       PTGlass 1 (the light only): the camera passes it, L.
//   (g) no GPU: RtScenePick's glass picking and the kinds' record bits, InstanceMask, OpFlags, SkinTexturePath.
#include "../../src/core/RtMaterials.h"

// The CPU's thin pane, as RtPathTrace.comp FresnelGlass / GlassSlab
static float cpuFresnelGlass(float c) {
	const float eta = 1.5f;
	c = std::min(std::max(c, 1.0e-3f), 1.0f);
	const float ct = std::sqrt(std::max(1.0f - (1.0f - c * c) / (eta * eta), 0.0f));
	const float rs = (c - eta * ct) / (c + eta * ct), rp = (eta * c - ct) / (eta * c + ct);
	return 0.5f * (rs * rs + rp * rp);
}
static void cpuGlassSlab(float c, const float a[3], float R[3], float T[3]) {
	const float r = cpuFresnelGlass(c);
	for (int k = 0; k < 3; k++) {
		const float d = std::max(1.0f - r * r * a[k] * a[k], 1.0e-6f);
		T[k] = (1.0f - r) * (1.0f - r) * a[k] / d;
		R[k] = r + (1.0f - r) * (1.0f - r) * r * a[k] * a[k] / d;
	}
}
static void cpuGlassTint(V3 albedo, float strength, float a[3]) {
	const float m = std::max(std::max(albedo.x, albedo.y), std::max(albedo.z, 1.0e-3f));
	const float c[3] = { albedo.x / m, albedo.y / m, albedo.z / m };
	for (int k = 0; k < 3; k++) a[k] = 1.0f + (std::min(std::max(c[k], 0.0f), 1.0f) - 1.0f) * strength;
}

// The lamp image with no lamps and the materials' keys in the frame row (RtMaterials::PackRow, as RtPathTrace.cpp)
static std::vector<float> matLampImage(const RtMaterials::Keys& keys) {
	std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
	RtMaterials::PackRow(&image[size_t(kLightTexels) * kFrameRowIndex * 2], keys);
	return image;
}
static RtMaterials::Keys matOff() { RtMaterials::Keys k; k.glass = 0; k.glassTint = 0; k.foliage = 0; k.skin = 0; k.sheen = 0; return k; }

struct MatRun {
	Camera cam;
	uint32_t flags = 1u | 1024u;   // the alpha test, the lamp image (its frame row: the keys)
	int bounces = 1, paths = 8, view = 0;
	V3 sunDir = { 0, 0, 1 }; bool sun = false; V3 sunColor = { 0, 0, 0 };
	V3 sky = { 0, 0, 0 }; bool outdoors = true;
	RtMaterials::Keys keys;
	float seed = 1.0f;
	int gAlbedo = -1, gMaterial = -1;   // (e) the G-buffer's colour and material images (flag 8 | 256)
};
static bool matRun(Ctx& c, const World& w, const std::vector<uint32_t>& ids, const MatRun& r, std::vector<float>& out) {
	Host& h = *c.host;
	PathConstants k = {};
	cameraConstants(r.cam, k.size, k.forward, k.right, k.up, k.origin);
	k.origin[3] = r.seed;
	put4(k.accum, 0.0f, float(r.bounces), float(r.paths), float(r.flags));
	put4(k.sunDir, normalize(r.sunDir), r.sun ? 1.0f : 0.0f);
	put4(k.sunColor, r.sunColor, std::tan(0.5f * 0.53f * 0.0174532925f));
	put4(k.skyZenith, r.sky, r.outdoors ? 1.0f : 0.0f);
	put4(k.skyHorizon, r.sky, 0.0f);   // PTGlow 0
	put4(k.misc, 0.0f, 0.0f, 60000.0f, float(r.view));
	k.rect[3] = 1.0f;   // PTBounceAlpha 1
	if (!h.Upload(gM.ptLamps, matLampImage(r.keys))) return false;
	const int gA = r.gAlbedo >= 0 ? r.gAlbedo : c.aux16, gMat = r.gMaterial >= 0 ? r.gMaterial : c.aux16;
	const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.alphaAtlas, c.ptDisplay, c.ptDisplay, c.depth, c.normals, gA, c.pano,
		c.aux16, c.ptDisplay, gMat, gM.ptAlbedoOut, gM.ptLamps };
	h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, r.cam.eye)) return false;
	const bool read = h.Read(c.ptDisplay, out);
	h.EndFrame();
	return read;
}

static const int kMatSlot = 120;   // a solid alpha slot (every texel 1) for the cut-out leaf cards

// (a) the furnace
static bool matFurnace(Ctx& c, int& bad) {
	Host& h = *c.host;
	World w;
	Mesh box; const float half = 1000, height = 1500;
	box.quad({ -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 });
	box.quad({ -half, -half, 0 }, { -half, half, 0 }, { -half, half, height }, { -half, -half, height });
	box.quad({ half, -half, 0 }, { half, -half, height }, { half, half, height }, { half, half, 0 });
	box.quad({ -half, -half, 0 }, { -half, -half, height }, { half, -half, height }, { half, -half, 0 });
	box.quad({ -half, half, 0 }, { half, half, 0 }, { half, half, height }, { -half, half, height });
	Mesh pane; pane.quad({ -700, -700, 1700 }, { 700, -700, 1700 }, { 700, 700, 1700 }, { -700, 700, 1700 });   // over the box: the camera looks through it
	Mesh leaf; leaf.quad({ -600, -100, 100 }, { 200, -100, 100 }, { 200, -100, 900 }, { -600, -100, 900 });    // standing cards inside
	Mesh skin; skin.quad({ 100, 200, 50 }, { 800, 500, 50 }, { 800, 500, 800 }, { 100, 200, 800 });
	w.meshes = { box, pane, leaf, skin };
	Object ob; ob.mesh = 0; ob.albedo = { 1, 1, 1 };
	Object gl; gl.mesh = 1; gl.kind = NVR_RT_KIND_GLASS; gl.albedo = { 1, 1, 1 };
	Object lf; lf.mesh = 2; lf.kind = NVR_RT_KIND_FOLIAGE; lf.albedo = { 1, 1, 1 }; lf.alphaTested = true; lf.alphaSlot = kMatSlot; lf.alphaRef = 128;
	Object sk; sk.mesh = 3; sk.kind = NVR_RT_KIND_SKIN; sk.albedo = { 1, 1, 1 };
	w.objects = { ob, gl, lf, sk };
	w.slots[kMatSlot] = [](int, int) { return 1.0f; };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const V3 L = { 0.8f, 0.6f, 0.4f };
	for (int on = 1; on >= 0; on--) {
		double sum[3] = {}; int nan = 0, n = 0;
		for (int f = 0; f < 4; f++) {
			MatRun r;
			r.cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.2f, 2.2f);
			r.bounces = 48; r.paths = 16; r.sky = L; r.seed = float(17 + 7919 * f);
			r.keys.glass = 2.0f; r.keys.glassTint = 0.5f; r.keys.foliage = 0.4f; r.keys.skin = 1.0f; r.keys.sheen = 0.5f;
			if (!on) r.keys = matOff();
			std::vector<float> disp;
			if (!matRun(c, w, ids, r, disp)) return false;
			for (UINT i = 0; i < S * S; i++) for (int ch = 0; ch < 3; ch++) { const float v = disp[i * 4 + ch]; if (!std::isfinite(v)) { nan++; continue; } sum[ch] += v; }
			n += S * S;
		}
		float worst = 0;
		for (int ch = 0; ch < 3; ch++) worst = std::max(worst, float(std::fabs(sum[ch] / n / (&L.x)[ch] - 1.0)));
		const bool pass = nan == 0 && worst < 0.015f;
		say("  (a) furnace, white box + %s, 48 bounces, 4 x 16 paths: mean / L = %.4f %.4f %.4f (worst %.2f %%, limit 1.5 %%), NaN %d -> %s\n",
			on ? "glass pane (camera through it) + leaf card + skin card, materials on" : "the same cards, materials off (baseline)",
			sum[0] / n / L.x, sum[1] / n / L.y, sum[2] / n / L.z, worst * 100, nan, pass ? "ok" : "BAD");
		if (!pass) bad++;
	}
	releaseWorld(h, ids);
	return true;
}

// (b) sunlight through a tinted window into a closed room
static bool matWindow(Ctx& c, int& bad) {
	Host& h = *c.host;
	World w;
	const float half = 400, height = 400, hole = 100;
	Mesh room;
	room.quad({ -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 });   // floor
	room.quad({ -half, -half, 0 }, { -half, half, 0 }, { -half, half, height }, { -half, -half, height });
	room.quad({ half, -half, 0 }, { half, -half, height }, { half, half, height }, { half, half, 0 });
	room.quad({ -half, -half, 0 }, { -half, -half, height }, { half, -half, height }, { half, -half, 0 });
	room.quad({ -half, half, 0 }, { half, half, 0 }, { half, half, height }, { -half, half, height });
	// the roof round the hole (four strips)
	room.quad({ -half, -half, height }, { half, -half, height }, { half, -hole, height }, { -half, -hole, height });
	room.quad({ -half, hole, height }, { half, hole, height }, { half, half, height }, { -half, half, height });
	room.quad({ -half, -hole, height }, { -hole, -hole, height }, { -hole, hole, height }, { -half, hole, height });
	room.quad({ hole, -hole, height }, { half, -hole, height }, { half, hole, height }, { hole, hole, height });
	Mesh pane; pane.quad({ -hole, -hole, height }, { hole, -hole, height }, { hole, hole, height }, { -hole, hole, height });
	w.meshes = { room, pane };
	Object rm; rm.mesh = 0; rm.albedo = { 0.5f, 0.5f, 0.5f };
	Object gl; gl.mesh = 1; gl.kind = NVR_RT_KIND_GLASS; gl.albedo = { 0.9f, 0.7f, 0.5f };
	std::vector<uint32_t> ids;
	w.objects = { rm, gl };
	if (!stageWorld(h, w, ids)) return false;
	const V3 sunDir = normalize(V3(0.25f, 0.15f, 1.0f));
	const V3 sunColor = { 2.0f, 1.8f, 1.5f };
	const float tintStrength = 1.0f;
	float a[3], R[3], T[3];
	cpuGlassTint(gl.albedo, tintStrength, a);
	cpuGlassSlab(sunDir.z, a, R, T);
	const Camera cam = Camera::lookDir({ 0, 0, 380 }, { 0, 0, -1 }, { 0, 1, 0 }, 0.6f, 0.6f);
	auto run = [&](bool withPane, float glassMode, std::vector<float>& out) {
		World v = w; if (!withPane) v.objects = { rm };
		MatRun r; r.cam = cam; r.view = 3; r.paths = 8; r.sun = true; r.sunDir = sunDir; r.sunColor = sunColor; r.seed = 5.0f;
		r.keys = matOff(); r.keys.glass = glassMode; r.keys.glassTint = tintStrength;
		return matRun(c, v, ids, r, out);
	};
	std::vector<float> withGlass, noPane, passed;
	if (!run(true, 2.0f, withGlass) || !run(false, 2.0f, noPane) || !run(true, 0.0f, passed)) return false;
	// the CPU: the floor point the pixel's centre ray meets, lifted as the shader's, its sun rays (centre + 4 disc edges) past the roof
	// (the pane left out: mask 0xEF); every corner of the pixel on the floor and every sun ray agreeing, or the pixel is an edge (skipped)
	const float tanSun = std::tan(0.5f * 0.53f * 0.0174532925f);
	V3 sb; { V3 t0 = normalize(cross(sunDir, V3(0, 0, 1))); if (length(cross(sunDir, V3(0, 0, 1))) < 1e-3f) t0 = V3(1, 0, 0); sb = cross(sunDir, t0); }
	const V3 st = normalize(cross(sb, sunDir));
	int lit = 0, dark = 0, edge = 0, wrong = 0, wrongPassed = 0, wrongPane = 0, shown = 0;
	double ratio[3] = {};
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		// the pixel's four corners: each must meet the floor, and each corner's lifted point's five sun rays (the disc's centre and edges)
		// must all agree (all past the roof or all blocked); else the pixel is an edge (the shader's jittered rays may land either way)
		int open = 0, rays = 0;
		bool floorAll = true;
		for (int q = 0; q < 4 && floorAll; q++) {
			const V3 d = cam.ray((i + (q & 1 ? 0.98f : 0.02f)) / S, (j + (q & 2 ? 0.98f : 0.02f)) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xEF, true);
			if (!hh.hit || std::fabs(std::fabs(hh.n.z) - 1.0f) > 1e-3f || (cam.eye + d * hh.t).z > 1.0f) { floorAll = false; break; }
			const V3 lift = cam.eye + d * hh.t + V3(0, 0, 1) * (1.0f + 0.002f * hh.t);
			for (int s = 0; s < 5; s++) {
				V3 l = sunDir;
				if (s > 0) { const float ph = (s - 1) * 1.5707963f; l = normalize(sunDir + (st * std::cos(ph) + sb * std::sin(ph)) * tanSun); }
				rays++;
				if (!trace(w, lift, l, 0.0f, 30000.0f, 0xEF, true).hit) open++;
			}
		}
		if (!floorAll || (open != 0 && open != rays)) { edge++; continue; }
		const float* g = &withGlass[(j * S + i) * 4]; const float* np = &noPane[(j * S + i) * 4]; const float* ps = &passed[(j * S + i) * 4];
		if (open == 0) {
			dark++;
			if (std::max(std::max(g[0], g[1]), g[2]) > 1e-4f) { wrong++; if (shown++ < 5) say("    pixel (%u, %u) shadowed by the roof: %.4f %.4f %.4f\n", i, j, g[0], g[1], g[2]); }
			continue;
		}
		lit++;
		const float c0 = sunDir.z;
		for (int k = 0; k < 3; k++) {
			const float plain = 0.5f * (&sunColor.x)[k] * c0, want = plain * T[k];
			if (std::fabs(g[k] - want) > 5e-3f * want + 1e-4f) { wrong++; if (shown++ < 5) say("    pixel (%u, %u) ch %d: %.4f through the pane, expected %.4f\n", i, j, k, g[k], want); }
			if (std::fabs(np[k] - plain) > 5e-3f * plain + 1e-4f) wrongPane++;
			if (std::fabs(ps[k] - plain) > 5e-3f * plain + 1e-4f) wrongPassed++;
			ratio[k] += g[k] / std::max(np[k], 1e-6f);
		}
	}
	const bool pass = lit > 300 && dark > 300 && wrong == 0 && wrongPane == 0 && wrongPassed == 0;
	say("  (b) sun through a tinted pane (n 1.5, tint %.2f %.2f %.2f, sun at %.1f deg off the pane's normal): %d lit floor pixels, %d in the roof's "
		"shadow, %d edges skipped; through the pane / open: %.4f %.4f %.4f (CPU slab T %.4f %.4f %.4f, R %.4f); %d values off the CPU (limit 0); "
		"no pane: %d off the untinted sun; pane with PTGlass 0 (passed clear): %d off -> %s\n", a[0], a[1], a[2],
		std::acos(sunDir.z) * 57.29578f, lit, dark, edge, ratio[0] / std::max(lit, 1), ratio[1] / std::max(lit, 1), ratio[2] / std::max(lit, 1),
		T[0], T[1], T[2], R[0], wrong, wrongPane, wrongPassed, pass ? "ok" : "BAD");
	if (!pass) { bad++; dumpPfm("materials_window", withGlass, S, S, 4, 0); }
	releaseWorld(h, ids);
	return true;
}

// (c) a back-lit leaf (and the same card as skin)
static bool matLeaf(Ctx& c, int& bad) {
	Host& h = *c.host;
	World w;
	Mesh card; card.quad({ -400, -400, 100 }, { 400, -400, 100 }, { 400, 400, 100 }, { -400, 400, 100 });
	w.meshes = { card };
	w.slots[kMatSlot] = [](int, int) { return 1.0f; };
	const V3 albedo = { 0.3f, 0.5f, 0.2f };
	const V3 sunColor = { 2.0f, 1.8f, 1.5f };
	const Camera cam = Camera::lookDir({ 0, 0, -200 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f);
	for (int asSkin = 0; asSkin < 2; asSkin++) {
		Object lf; lf.mesh = 0; lf.albedo = albedo;
		if (asSkin) lf.kind = NVR_RT_KIND_SKIN;
		else { lf.kind = NVR_RT_KIND_FOLIAGE; lf.alphaTested = true; lf.alphaSlot = kMatSlot; lf.alphaRef = 128; }
		w.objects = { lf };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const V3 below = normalize(V3(0.2f, 0.1f, -1.0f)), above = normalize(V3(0.2f, 0.1f, 1.0f));
		const float c0 = std::fabs(below.z);
		std::vector<float> front, back, backOff;
		auto run = [&](V3 sunDir, bool on, std::vector<float>& out) {
			MatRun r; r.cam = cam; r.view = 3; r.paths = 4; r.sun = true; r.sunDir = sunDir; r.sunColor = sunColor; r.seed = 9.0f;
			r.keys = matOff();
			if (on) { r.keys.foliage = 0.4f; r.keys.skin = 1.0f; }
			return matRun(c, w, ids, r, out);
		};
		if (!run(below, true, front) || !run(above, true, back) || !run(above, false, backOff)) return false;
		const float tau = 0.4f;
		const float skinT[3] = { 0.25f * 1.0f, 0.25f * 0.28f, 0.25f * 0.12f };
		const float w3[3] = { 0.3f, 0.12f, 0.08f };
		// per pixel within 2 % (the sun disc's jitter moves one sample's cosine ~0.1 %), the image means within 0.3 %
		int checked = 0, wrong = 0, offLit = 0, shown = 0, nan = 0;
		double fr[3] = {}, bk[3] = {}, wf[3] = {}, wb[3] = {};
		for (UINT p = 0; p < S * S; p++) {
			checked++;
			for (int k = 0; k < 3; k++) {
				const float a = (&albedo.x)[k], s = (&sunColor.x)[k];
				const float wantFront = asSkin ? a * s * (c0 + w3[k]) / ((1 + w3[k]) * (1 + w3[k])) : a * s * c0;
				const float wantBack = asSkin ? skinT[k] * a * s * c0 : tau * a * s * c0;
				const float f = front[p * 4 + k], b = back[p * 4 + k];
				if (!std::isfinite(f) || !std::isfinite(b)) { nan++; continue; }
				fr[k] += f; bk[k] += b; wf[k] += wantFront; wb[k] += wantBack;
				if (std::fabs(f - wantFront) > 0.02f * wantFront + 1e-4f || std::fabs(b - wantBack) > 0.02f * wantBack + 1e-4f) {
					wrong++;
					if (shown++ < 4) say("    pixel %u ch %d: front %.4f (want %.4f), back %.4f (want %.4f)\n", p, k, f, wantFront, b, wantBack);
				}
				if (backOff[p * 4 + k] > 1e-4f) offLit++;
			}
		}
		float meanOff = 0;
		for (int k = 0; k < 3; k++) meanOff = std::max(meanOff, float(std::max(std::fabs(fr[k] / wf[k] - 1.0), std::fabs(bk[k] / wb[k] - 1.0))));
		const bool pass = wrong == 0 && offLit == 0 && nan == 0 && meanOff < 3e-3f;
		say("  (c) %s card seen from below: front-lit %.4f %.4f %.4f, back-lit %.4f %.4f %.4f (back / front %.3f %.3f %.3f; expected %s); means off "
			"the formula by %.3f %% at most (limit 0.3 %%), %d of %d values off by > 2 %%, NaN %d; back-lit with the setting 0: %d lit values (limit 0) -> %s\n",
			asSkin ? "skin" : "leaf", fr[0] / checked, fr[1] / checked, fr[2] / checked, bk[0] / checked, bk[1] / checked, bk[2] / checked,
			bk[0] / std::max(fr[0], 1e-9), bk[1] / std::max(fr[1], 1e-9), bk[2] / std::max(fr[2], 1e-9),
			asSkin ? "0.25 x (1, 0.28, 0.12) x the plain front over the wrapped one" : "PTFoliage 0.4", meanOff * 100, wrong, checked * 3, nan, offLit,
			pass ? "ok" : "BAD");
		if (!pass) { bad++; dumpPfm(asSkin ? "materials_skin_back" : "materials_leaf_back", back, S, S, 4, 0); }
		releaseWorld(h, ids);
	}
	return true;
}

// (d) skin's wrap under the sun at falling elevations, against a plain floor
static bool matSkinWrap(Ctx& c, int& bad) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	w.meshes = { floor };
	const V3 albedo = { 0.6f, 0.45f, 0.4f };
	const V3 sunColor = { 1.5f, 1.5f, 1.5f };
	const float w3[3] = { 0.3f, 0.12f, 0.08f };
	const Camera cam = Camera::lookDir({ 0, 0, 500 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.0f);
	float worstDiff = 0;
	int wrong = 0;
	std::string line;
	for (float elev : { 90.0f, 45.0f, 15.0f, 5.0f }) {
		const float e = elev * 0.0174532925f;
		const V3 sunDir = { std::cos(e), 0.0f, std::sin(e) };
		std::vector<float> skin, plain;
		for (int asSkin = 1; asSkin >= 0; asSkin--) {
			Object o; o.mesh = 0; o.albedo = albedo; o.kind = asSkin ? NVR_RT_KIND_SKIN : NVR_RT_KIND_PLAIN;
			w.objects = { o };
			std::vector<uint32_t> ids;
			if (!stageWorld(h, w, ids)) return false;
			MatRun r; r.cam = cam; r.view = 3; r.paths = 2; r.sun = true; r.sunDir = sunDir; r.sunColor = sunColor; r.seed = 3.0f;
			r.keys = matOff(); r.keys.skin = 1.0f;
			if (!matRun(c, w, ids, r, asSkin ? skin : plain)) return false;
			releaseWorld(h, ids);
		}
		// the image means against the formulas within 0.3 % (one sample's cosine moves with the sun disc's jitter: 5 % at 5 degrees)
		double ms[3] = {}, mp[3] = {};
		const float cs = std::sin(e);
		for (UINT p = 0; p < S * S; p++) for (int k = 0; k < 3; k++) { ms[k] += skin[p * 4 + k] / double(S * S); mp[k] += plain[p * 4 + k] / double(S * S); }
		for (int k = 0; k < 3; k++) {
			const float a = (&albedo.x)[k], s = (&sunColor.x)[k];
			const float wantSkin = a * s * (cs + w3[k]) / ((1 + w3[k]) * (1 + w3[k])), wantPlain = a * s * cs;
			if (!(std::fabs(ms[k] - wantSkin) <= 3e-3 * wantSkin + 1e-5) || !(std::fabs(mp[k] - wantPlain) <= 3e-3 * wantPlain + 1e-5)) {
				wrong++;
				say("    %.0f deg ch %d: skin %.5f (want %.5f), plain %.5f (want %.5f)\n", elev, k, ms[k], wantSkin, mp[k], wantPlain);
			}
			worstDiff = std::max(worstDiff, float(std::fabs(ms[k] - mp[k]) / (a * s)));
		}
		char b[200];
		snprintf(b, sizeof(b), " %.0f deg: %.3f %.3f %.3f;", elev, ms[0] / std::max(mp[0], 1e-9), ms[1] / std::max(mp[1], 1e-9), ms[2] / std::max(mp[2], 1e-9));
		line += b;
	}
	const bool pass = wrong == 0 && worstDiff <= 0.25f;
	say("  (d) skin floor / plain floor (image means) under the sun at%s means off the formulas %d (limit 0); largest |skin - plain| / (albedo x sun) %.3f "
		"(limit 0.25) -> %s\n", line.c_str(), wrong, worstDiff, pass ? "ok" : "BAD");
	if (!pass) bad++;
	return true;
}

// (e) cloth sheen through the G-buffer's MaterialBuffer class 3
static bool matCloth(Ctx& c, int& bad) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -20000, -20000, 0 }, { 20000, -20000, 0 }, { 20000, 20000, 0 }, { -20000, 20000, 0 });
	w.meshes = { floor };
	Object o; o.mesh = 0; o.albedo = { 0.9f, 0.9f, 0.9f };   // (near white: the sky part tests that nothing comes out above L)
	w.objects = { o };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -1500, 600 }, { 0, 1, -0.45f }, { 0, 0, 1 }, 0.9f, 0.9f);
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(w, cam, px, depth, normals);
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
	std::vector<float> albedo(size_t(S) * S * 4, 0.0f);
	for (UINT p = 0; p < S * S; p++) if (px[p].valid) { albedo[p * 4] = o.albedo.x; albedo[p * 4 + 1] = o.albedo.y; albedo[p * 4 + 2] = o.albedo.z; albedo[p * 4 + 3] = 1.0f; }
	if (!h.Upload(c.aux16, albedo)) return false;
	const V3 L = { 1.0f, 1.0f, 1.0f };
	// view cosine per pixel (the binning)
	auto nvOf = [&](UINT p) { const V3 d = normalize(cam.ray(((p % S) + 0.5f) / S, ((p / S) + 0.5f) / S)); return -d.z; };
	double skyCloth[4] = {}, skyPlain[4] = {}; int skyN[4] = {};
	double sunCloth = 0, sunPlain = 0; int sunN = 0;
	for (int part = 0; part < 2; part++) {
		for (int cls = 3; cls >= 1; cls -= 2) {
			std::vector<float> mat(size_t(S) * S * 4, 0.0f);
			for (UINT p = 0; p < S * S; p++) if (px[p].valid) { mat[p * 4] = float(cls); mat[p * 4 + 2] = 0.7f; mat[p * 4 + 3] = 1.0f; }
			if (!h.Upload(c.histA, mat)) return false;
			std::vector<double> acc(size_t(S) * S * 3, 0.0);
			const int frames = part == 0 ? 8 : 2;
			for (int f = 0; f < frames; f++) {
				MatRun r; r.cam = cam; r.flags = 1u | 1024u | 8u | 128u | 256u; r.gAlbedo = c.aux16; r.gMaterial = c.histA; r.seed = float(31 + 7919 * f);
				r.keys = matOff(); r.keys.sheen = 1.0f;
				if (part == 0) { r.bounces = 1; r.paths = 16; r.sky = L; }
				else {   // a low sun behind the camera (the sheen's retro rim at grazing angles), DebugView 3
					r.view = 3; r.paths = 2; r.sun = true; r.sunDir = normalize(V3(0.0f, -1.0f, 0.17f)); r.sunColor = { 1.0f, 1.0f, 1.0f };
				}
				std::vector<float> disp;
				if (!h.Upload(c.histA, mat) || !matRun(c, w, ids, r, disp)) return false;
				for (UINT p = 0; p < S * S; p++) for (int k = 0; k < 3; k++) acc[p * 3 + k] += disp[p * 4 + k] / frames;
			}
			for (UINT p = 0; p < S * S; p++) {
				if (!px[p].valid) continue;
				const double v = acc[p * 3];   // red: the brightest albedo
				if (part == 0) {
					const float nv = nvOf(p);
					const int bin = nv < 0.25f ? 0 : nv < 0.45f ? 1 : nv < 0.65f ? 2 : 3;
					(cls == 3 ? skyCloth : skyPlain)[bin] += v;
					if (cls == 3) skyN[bin]++;
				} else {
					(cls == 3 ? sunCloth : sunPlain) += v;
					if (cls == 3) sunN++;
				}
			}
		}
	}
	bool pass = sunN > 1000;
	std::string bins;
	for (int b = 0; b < 4; b++) {
		if (!skyN[b]) continue;
		const double cl = skyCloth[b] / skyN[b], pl = skyPlain[b] / skyN[b];
		char t[120]; snprintf(t, sizeof(t), " [%s] cloth %.3f plain %.3f;", b == 0 ? "nv<.25" : b == 1 ? ".25-.45" : b == 2 ? ".45-.65" : ">.65", cl, pl);
		bins += t;
		if (cl > std::max(1.02 * L.x, pl + 0.01)) pass = false;   // never above the sky (nor above plain): the sheen comes out of the diffuse
		if (cl < 0.5 * pl) pass = false;     // and not a black hole
	}
	const double rim = sunCloth / std::max(sunPlain, 1e-9);
	if (!(rim > 1.1)) pass = false;
	say("  (e) cloth sheen (MaterialBuffer class 3, PTClothSheen 1) under a uniform sky L = 1, red channel by view cosine:%s under a low sun "
		"against the camera cloth / plain %.3f (limit > 1.1); never above L (limit 1.02) -> %s\n", bins.c_str(), rim, pass ? "ok" : "BAD");
	if (!pass) bad++;
	releaseWorld(h, ids);
	return true;
}

// (f) the camera's rays through a pane (PTGlass 2) against a uniform sky; PTGlass 1: the camera passes it
static bool matCameraGlass(Ctx& c, int& bad) {
	Host& h = *c.host;
	World w;
	Mesh pane; pane.quad({ -100000, 0, -100000 }, { 100000, 0, -100000 }, { 100000, 0, 100000 }, { -100000, 0, 100000 });
	w.meshes = { pane };
	Object gl; gl.mesh = 0; gl.kind = NVR_RT_KIND_GLASS; gl.albedo = { 1.0f, 0.6f, 0.3f };
	w.objects = { gl };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -1000, 0 }, { 1, 1, 0 }, { 0, 0, 1 }, 0.6f, 0.6f);
	const V3 L = { 0.7f, 0.8f, 0.9f };
	float a[3]; cpuGlassTint(gl.albedo, 1.0f, a);
	std::vector<double> acc2(size_t(S) * S * 3, 0.0), acc1(size_t(S) * S * 3, 0.0);
	const int frames = 8;
	for (int mode = 2; mode >= 1; mode--) for (int f = 0; f < frames; f++) {
		MatRun r; r.cam = cam; r.bounces = 2; r.paths = 16; r.sky = L; r.seed = float(77 + 7919 * f);
		r.keys = matOff(); r.keys.glass = float(mode); r.keys.glassTint = 1.0f;
		std::vector<float> disp;
		if (!matRun(c, w, ids, r, disp)) return false;
		std::vector<double>& acc = mode == 2 ? acc2 : acc1;
		for (UINT p = 0; p < S * S; p++) for (int k = 0; k < 3; k++) acc[p * 3 + k] += disp[p * 4 + k] / frames;
	}
	// bins of the incidence cosine: the mean over the bin vs the CPU's mean of L (R + T) over the same pixels
	// (the shader's sky without a panorama: L above the horizon, 0.3 L below it; a reflection off the vertical pane and the ray through it
	// keep the ray's height, so a pixel's sky is the camera ray's; the rows at the horizon, where a pixel's samples fall either side, skipped)
	double got[5][3] = {}, want[5][3] = {}; int n[5] = {}, sky = 0, skyWrong = 0, passWrong = 0;
	for (UINT p = 0; p < S * S; p++) {
		const V3 d = normalize(cam.ray(((p % S) + 0.5f) / S, ((p / S) + 0.5f) / S));
		if (std::fabs(d.z) < 0.02f) continue;
		const float skyScale = d.z < 0.0f ? 0.3f : 1.0f;
		for (int k = 0; k < 3; k++) if (std::fabs(acc1[p * 3 + k] - skyScale * (&L.x)[k]) > 2e-3 * (&L.x)[k]) passWrong++;
		if (d.y <= 0.0f) {   // misses the pane: the sky (a pixel whose samples may still meet it: skipped)
			if (d.y > -0.03f) continue;
			sky++;
			for (int k = 0; k < 3; k++) if (std::fabs(acc2[p * 3 + k] - skyScale * (&L.x)[k]) > 2e-3 * (&L.x)[k]) skyWrong++;
			continue;
		}
		if (d.y < 0.05f) continue;   // (the ray length, 60000, ends short of the pane below this; the pixel's own spread of angles too wide)
		// the CPU's slab averaged over 4 x 4 points of the pixel (near grazing R + T bends within a pixel's footprint)
		double sumRT[3] = {};
		for (int s = 0; s < 16; s++) {
			const V3 ds = normalize(cam.ray(((p % S) + (s % 4 + 0.5f) / 4.0f) / S, ((p / S) + (s / 4 + 0.5f) / 4.0f) / S));
			float R[3], T[3];
			cpuGlassSlab(ds.y, a, R, T);
			for (int k = 0; k < 3; k++) sumRT[k] += (R[k] + T[k]) / 16.0;
		}
		const int b = std::min(int(d.y * 5.0f), 4);
		n[b]++;
		for (int k = 0; k < 3; k++) { got[b][k] += acc2[p * 3 + k]; want[b][k] += skyScale * (&L.x)[k] * sumRT[k]; }
	}
	bool pass = true;
	std::string bins;
	for (int b = 0; b < 5; b++) {
		if (n[b] < 50) continue;
		float worst = 0;
		for (int k = 0; k < 3; k++) worst = std::max(worst, float(std::fabs(got[b][k] / want[b][k] - 1.0)));
		char t[160]; snprintf(t, sizeof(t), " cos %.1f-%.1f: %.3f %.3f %.3f vs %.3f %.3f %.3f (%d px);", b * 0.2f, b * 0.2f + 0.2f, got[b][0] / n[b], got[b][1] / n[b],
			got[b][2] / n[b], want[b][0] / n[b], want[b][1] / n[b], want[b][2] / n[b], n[b]);
		bins += t;
		if (worst > 0.015f) pass = false;
	}
	if (skyWrong || passWrong) pass = false;
	say("  (f) the camera through a pane (tint %.2f %.2f %.2f) against a uniform sky, 8 x 16 paths, bins of the incidence cosine (traced vs L (R + T) of the slab, "
		"limit 1.5 %%):%s pixels past the pane %d (off L: %d); PTGlass 1 (the camera passes the pane): %d values off L (limit 0) -> %s\n",
		a[0], a[1], a[2], bins.c_str(), sky, skyWrong, passWrong, pass ? "ok" : "BAD");
	if (!pass) bad++;
	releaseWorld(h, ids);
	return true;
}

// (g) the picking rules and the record bits (no GPU)
static bool matPicking(int& bad) {
	int wrong = 0;
	auto expect = [&](bool ok, const char* what) { if (!ok) { wrong++; say("    picking: %s WRONG\n", what); } };
	RtScenePick::Switches on; on.alphaTested = true; on.glass = true;
	RtScenePick::Switches off = on; off.glass = false;
	RtScenePick::Geometry g; g.hasAlphaProperty = true; g.alphaFlags = RtScenePick::kAlphaBlend; g.glassRule = true;
	bool at = false, gl = false;
	expect(RtScenePick::Classify(g, on, at, gl) == RtScenePick::Taken && gl && !at, "blended glass-rule mesh -> glass");
	expect(RtScenePick::Classify(g, off, at, gl) == RtScenePick::BlendedOnly && !gl, "PTGlass off -> blended only, as before");
	RtScenePick::Switches noAlpha = on; noAlpha.alphaTested = false;
	expect(RtScenePick::Classify(g, noAlpha, at, gl) == RtScenePick::BlendedOnly && !gl, "AlphaTested off -> no glass");
	RtScenePick::Geometry fx = g; fx.glassRule = false;
	expect(RtScenePick::Classify(fx, on, at, gl) == RtScenePick::BlendedOnly && !gl, "blended, no glass rule (fx) -> out");
	RtScenePick::Geometry refr; refr.refraction = true; refr.glassRule = true;
	expect(RtScenePick::Classify(refr, on, at, gl) == RtScenePick::Taken && gl, "refraction-flagged glass-rule mesh -> glass");
	refr.glassRule = false;
	expect(RtScenePick::Classify(refr, on, at, gl) == RtScenePick::DecalOrRefraction && !gl, "refraction without the rule (heat haze) -> out");
	RtScenePick::Geometry tested = g; tested.alphaFlags = RtScenePick::kAlphaBlend | RtScenePick::kAlphaTest;
	expect(RtScenePick::Classify(tested, on, at, gl) == RtScenePick::Taken && at && !gl, "blended + tested glass-rule -> a cut-out, as before");
	RtScenePick::Geometry farGlass = g; farGlass.farLod = true;
	expect(!(RtScenePick::Classify(farGlass, on, at, gl) == RtScenePick::Taken && gl), "far LOD -> never glass");
	RtScenePick::Geometry landGlass = g; landGlass.land = true;
	expect(!(RtScenePick::Classify(landGlass, on, at, gl) == RtScenePick::Taken && gl), "land -> never glass");
	for (uint32_t kind = 0; kind < 4; kind++) {
		RtScenePick::Record r; r.kind = kind; r.textured = true; r.slot = 1023; r.materialRule = true; r.rough = 1.0f; r.metal = 1.0f;
		const uint32_t f = RtScenePick::RecordFlags(r);
		expect(NvrRtKindOf(f) == kind, "kind round trip");
		expect(((f >> 16) & 0x3FFu) == 1023u && ((f >> 26) & 7u) == 7u && ((f >> 29) & 7u) == 7u, "kind bits leave slot / material alone");
		expect((RtScenePick::InstanceMask(r) == NVR_RT_MASK_ALPHA) == (kind == NVR_RT_KIND_GLASS), "glass mask 0x10, others as before");
		expect(((RtScenePick::OpFlags(r) & NVR_RT_OP_FLAG_ALPHA) != 0) == (kind == NVR_RT_KIND_GLASS), "glass built non-opaque");
	}
	{ RtScenePick::Record r; expect(RtScenePick::RecordFlags(r) == 0u, "a plain record: no kind bits"); }
	expect(RtScenePick::SkinTexturePath("Textures\\Characters\\Male\\UpperBodyMale.dds"), "body texture -> skin");
	expect(RtScenePick::SkinTexturePath("textures/characters/female/headhuman.dds"), "head texture -> skin");
	expect(!RtScenePick::SkinTexturePath("textures\\characters\\hair\\hairshort.dds"), "hair -> not skin");
	expect(!RtScenePick::SkinTexturePath("textures\\characters\\eyes\\eyeblue.dds"), "eyes -> not skin");
	expect(!RtScenePick::SkinTexturePath("textures\\armor\\leatherarmor\\leather.dds"), "armour -> not skin");
	expect(!RtScenePick::SkinTexturePath(nullptr), "no path");
	say("  (g) picking + packing: glass from blended / refraction meshes with the glass rule only (not fx, not cut-outs, not far LOD, not with "
		"PTGlass or AlphaTested off), kind bits round trip beside slot + material, glass mask 0x10 + non-opaque, skin texture paths: %d wrong -> %s\n",
		wrong, wrong ? "BAD" : "ok");
	bad += wrong;
	return true;
}

static bool sceneMaterials(Ctx& c) {
	Host& h = *c.host;
	if (!uploadSlot(h, c.alphaAtlas, kMatSlot, [](int, int) { return 1.0f; })) return false;
	int bad = 0;
	if (!matPicking(bad)) return false;
	if (!matFurnace(c, bad) || !matWindow(c, bad) || !matLeaf(c, bad) || !matSkinWrap(c, bad) || !matCloth(c, bad) || !matCameraGlass(c, bad)) return false;
	// leave the lamp image's frame row clear for the scenes after this one (the keys off)
	if (!h.Upload(gM.ptLamps, std::vector<float>(size_t(kLightTexels) * kLampRows * 2, 0.0f))) return false;
	return bad == 0;
}
