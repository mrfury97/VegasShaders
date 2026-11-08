// (2026-11-07 ptaaa-firstperson) Scenes `ptviewmodel` and `hiddennear`: the first-person view model lit by the path tracer, and the
// player's own body in first person (FirstPersonBody: NVR_RT_CUSTOM_HIDDEN) passed over by every ray that starts or ends within arm's
// reach of the eye (Compute/Rt/RtHidden.glsl, kRtHiddenNear 80). docs/next/RT-PATH-TRACING.md "ptaaa-firstperson".
//
// The hidden body (both scenes): a head box round the eye (0x20, an actor's skin), a torso and legs below it (0x20) and a third-person gun
// sticking forward from the chest (0x40, the player's rigid parts), all hidden. Three TLAS variants of the same world: the body hidden
// (first person), absent, and visible (third person: no custom index).

struct FpBody { Object head, torso, legs, gun; };
// Adds the body's meshes to w (4 meshes) round the eye; returns the four objects (hidden)
static FpBody addBody(World& w, V3 eye, float gunZ) {
	FpBody b;
	const int m0 = int(w.meshes.size());
	w.meshes.resize(m0 + 4);
	w.meshes[m0 + 0].box({ eye.x - 11, eye.y - 11, eye.z - 16 }, { eye.x + 11, eye.y + 11, eye.z + 14 });   // the head round the eye
	w.meshes[m0 + 1].box({ eye.x - 18, eye.y - 12, eye.z - 82 }, { eye.x + 18, eye.y + 10, eye.z - 16 });   // torso
	w.meshes[m0 + 2].box({ eye.x - 14, eye.y - 9, 0.5f }, { eye.x + 14, eye.y + 9, eye.z - 82 });          // legs
	w.meshes[m0 + 3].box({ eye.x + 4, eye.y + 8, gunZ }, { eye.x + 10, eye.y + 52, gunZ + 6 });            // the third-person gun
	b.head.mesh = m0; b.head.hookMask = 0x20; b.head.hidden = true; b.head.albedo = { 0.8f, 0.6f, 0.5f };
	b.torso.mesh = m0 + 1; b.torso.hookMask = 0x20; b.torso.hidden = true; b.torso.albedo = { 0.3f, 0.3f, 0.4f };
	b.legs.mesh = m0 + 2; b.legs.hookMask = 0x20; b.legs.hidden = true; b.legs.albedo = { 0.3f, 0.3f, 0.4f };
	b.gun.mesh = m0 + 3; b.gun.hookMask = 0x40; b.gun.hidden = true; b.gun.albedo = { 0.2f, 0.2f, 0.2f };
	return b;
}
// The three TLAS variants: 0 the body hidden (first person), 1 absent, 2 visible (third person)
static std::vector<Object> bodyVariant(const std::vector<Object>& world, const FpBody& b, int variant) {
	std::vector<Object> o = world;
	if (variant == 1) return o;
	for (Object x : { b.head, b.torso, b.legs, b.gun }) { x.hidden = variant == 0; o.push_back(x); }
	return o;
}
static World withObjects(const World& w, const std::vector<Object>& objects) { World v = w; v.objects = objects; return v; }

// ==============================================================================================================================
// ptviewmodel: RtPathTrace.comp's first hit on the first-person view model (gNormals alpha 4 + s: RtPathTrace.cpp ViewModelPrep).
// A floor and a back wall; the view model is a plate (a gun's side) in front of the eye drawn with the narrower first-person FOV
// (s = 1.45): its G-buffer pixels hold its depth (view z), normal, colour; some of them are marked "not captured" (alpha -1). The hidden
// body wraps the eye. Checks (DebugView 3: the first hit's direct light; 1: its colour; 5: the arriving one-bounce sky light):
//  A  a lamp near the gun and the eye (a flashlight / Pip-Boy light): the plate's light = the CPU's at its TRUE point (the first-person
//     FOV's), the body passed over; the same at the world-FOV point is far off (the test sees the position); the floor and wall lit by
//     it as if the body were absent (its rays end near the eye) where a visible body shadows them (the reverse shadow)
//  B  a lamp behind and above the player: the floor keeps the body's shadow (rays far from the eye), the plate is lit (rays from it)
//  C  DebugView 1: captured plate pixels show the plate's colour; not-captured ones the game's frame
//  D  DebugView 5 under a uniform sky: the plate's sky light with the body hidden = without it, below that with it visible
struct VmPlate { V3 c, a, b; };   // the parallelogram c + a s + b t, s, t in [-1, 1]
static bool vmHit(const VmPlate& pl, V3 o, V3 d, float& t) {
	const V3 n = cross(pl.a, pl.b);
	const float den = dot(n, d);
	if (std::fabs(den) < 1e-9f) return false;
	t = dot(n, pl.c - o) / den;
	if (!(t > 0)) return false;
	const V3 q = o + d * t - pl.c;
	// solve q = a s + b t (a, b orthogonal here)
	const float s1 = dot(q, pl.a) / dot(pl.a, pl.a), s2 = dot(q, pl.b) / dot(pl.b, pl.b);
	return std::fabs(s1) <= 1.0f && std::fabs(s2) <= 1.0f;
}

static bool scenePtViewModel(Ctx& c) {
	Host& h = *c.host;
	const V3 eye = { 0, 0, 150 };
	World w;
	w.meshes.resize(2);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });     // floor
	w.meshes[1].quad({ -3000, 500, 0 }, { 3000, 500, 0 }, { 3000, 500, 1500 }, { -3000, 500, 1500 });     // back wall
	Object floor; floor.mesh = 0; floor.land = true; floor.albedo = { 0.5f, 0.5f, 0.5f }; floor.id = 1;
	Object wall; wall.mesh = 1; wall.albedo = { 0.5f, 0.5f, 0.5f }; wall.id = 2;
	const std::vector<Object> plain = { floor, wall };
	const FpBody body = addBody(w, eye, eye.z - 30);
	w.objects = plain;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, withObjects(w, bodyVariant(plain, body, 2)), ids)) return false;
	const Camera cam = Camera::lookDir(eye, { 0, 1, -0.45f }, { 0, 0, 1 }, 1.2f, 1.2f);
	const float s = 1.45f;   // tan(world FOV / 2) / tan(view-model FOV / 2)
	const Camera vmCam = Camera::lookDir(eye, { 0, 1, -0.45f }, { 0, 0, 1 }, 1.2f * s, 1.2f * s);
	// the plate: right of and below the view centre, 25-55 units out, facing up and toward the eye
	VmPlate pl;
	pl.c = eye + cam.f * 40.0f + cam.r * 14.0f - cam.u * 12.0f;
	pl.a = cam.r * 10.0f;
	pl.b = normalize(cam.f * 1.0f + cam.u * 0.25f) * 15.0f;
	V3 pn = normalize(cross(pl.a, pl.b));
	if (dot(pn, eye - pl.c) < 0) pn = -pn;
	const V3 plateAlbedo = { 0.6f, 0.45f, 0.3f };
	// the G-buffer: the world (no body: it is not drawn) through the world camera; over it the plate through the view model's camera
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(withObjects(w, plain), cam, px, depth, normals);
	std::vector<int> vm(S * S, 0);   // 0 world, 1 the plate (captured), 2 the plate not captured
	std::vector<V3> vmPos(S * S);
	std::vector<float> albedo(S * S * 4, 0.0f), frame(S * S * 4, 0.0f);
	int plateN = 0, keptN = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const UINT p = j * S + i;
		float t;
		const V3 d = vmCam.ray((i + 0.5f) / S, (j + 0.5f) / S);   // (view z along it = t: d . f = 1)
		frame[p * 4] = 0.3f; frame[p * 4 + 1] = 0.6f; frame[p * 4 + 2] = 0.9f; frame[p * 4 + 3] = 1.0f;   // "the game's pixel"
		if (vmHit(pl, eye, d, t)) {
			vm[p] = i < S * 0.62f ? 2 : 1;   // the plate's left part: no capture there
			vmPos[p] = eye + d * t;
			depth[p] = t / cam.farZ;
			float* n = &normals[p * 4];
			n[0] = dot(pn, cam.r) * 0.5f + 0.5f; n[1] = dot(pn, cam.u) * 0.5f + 0.5f; n[2] = dot(pn, cam.f) * 0.5f + 0.5f;
			n[3] = vm[p] == 1 ? 4.0f + s : -1.0f;
			if (vm[p] == 1) { albedo[p * 4] = plateAlbedo.x; albedo[p * 4 + 1] = plateAlbedo.y; albedo[p * 4 + 2] = plateAlbedo.z; albedo[p * 4 + 3] = 1.0f; plateN++; }
			else { albedo[p * 4] = albedo[p * 4 + 1] = albedo[p * 4 + 2] = 0.5f; albedo[p * 4 + 3] = 1.0f; keptN++; }   // (the world's colour stays under it)
		}
		else if (px[p].valid) { albedo[p * 4] = albedo[p * 4 + 1] = albedo[p * 4 + 2] = 0.5f; albedo[p * 4 + 3] = 1.0f; }
	}
	say("  view model: %d plate pixels lit by the path tracer, %d not captured; FOV scale %.2f\n", plateN, keptN, s);
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals) || !h.Upload(c.aux16, albedo) || !h.Upload(gM.aoLight, frame)) return false;
	auto run = [&](int variant, V3 lampPos, float reach, int view, int paths, bool sky, std::vector<float>& disp) {
		std::vector<float> image(size_t(kLightTexels) * (3 + kGridRows) * 2, 0.0f);   // (2026-11-07 ptaaa-hangfix: all 51 rows of gM.ptLamps; 50 let Upload read past the vector: an access violation)
		const V3 q = lampPos - eye;
		const float v[8] = { q.x, q.y, q.z, reach, 1.0f, 1.0f, 1.0f, 0.0f };
		std::memcpy(image.data(), v, sizeof(v));
		if (!h.Upload(gM.ptLamps, image)) return false;
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 31.0f;
		put4(k.accum, 0.0f, 1.0f, float(paths), float(8u | 32u | 1024u | 2048u));   // G-buffer, frame, lamp image, the game's falloff
		put4(k.sunDir, 0, 0, 1, 0);
		put4(k.sunColor, 0, 0, 0, 0);
		put4(k.skyZenith, sky ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, 1.0f); put4(k.skyHorizon, sky ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, 0.0f);
		put4(k.misc, sky ? 0.0f : 1.0f, 0.0f, 60000.0f, float(view));
		k.rect[3] = 1.0f;
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano,
			gM.aoLight, c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(withObjects(w, bodyVariant(plain, body, variant)), ids, eye)) return false;
		const bool ok = h.Read(c.ptDisplay, disp);
		h.EndFrame();
		return ok;
	};
	const World wHidden = withObjects(w, bodyVariant(plain, body, 0)), wAbsent = withObjects(w, plain);
	auto nearEye = [&](V3 a) { return length(a - eye) < 80.0f; };
	// the CPU's direct light from one lamp of colour 1 at point P (normal n, albedo a, distance t from the eye), the shader's rule
	auto lampCpu = [&](V3 P, V3 n, float a, float t, V3 L, float reach, float& value) {
		const V3 l = L - P; const float d = length(l);
		value = 0.0f;
		if (d >= reach) return;
		const float x = d / reach, fall = std::pow(std::max(1.0f - x * x, 0.0f), 2.2f), cs = std::max(dot(n, l * (1.0f / d)), 0.0f);
		if (!(cs > 0)) return;
		const V3 lift = P + n * (1.0f + 0.002f * t);
		const V3 dl = L - lift; const float dd = length(dl); const V3 dir = dl * (1.0f / dd);
		const float tMax = std::max(dd - 16.0f, 0.0f);
		const World& tw = nearEye(lift) || nearEye(lift + dir * (tMax + 16.0f)) ? wAbsent : wHidden;
		if (trace(tw, lift, dir, 0.0f, tMax, 0xFF, true, true).hit) return;
		value = a * fall * cs;
	};
	bool ok = true;
	// ---- A: a lamp in the head (the flashlight / Pip-Boy light at the eye): reach 150 for the plate (its light then hangs on its point),
	// 2000 for the floor and wall
	{
		const V3 L = eye + cam.f * 4.0f + cam.r * 3.0f - cam.u * 3.0f;
		const float reach = 150.0f, reachFar = 2000.0f;
		std::vector<float> got, gotFar, third;
		if (!run(0, L, reach, 3, 8, false, got) || !run(0, L, reachFar, 3, 8, false, gotFar) || !run(2, L, reachFar, 3, 8, false, third)) return false;
		int n = 0, bad = 0, shown = 0; double err = 0, errWorldFov = 0;
		int wn = 0, wbad = 0, reverse = 0, reverseLit = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const UINT p = j * S + i;
			if (vm[p] == 1) {
				float want, wantW;
				lampCpu(vmPos[p], pn, plateAlbedo.x, length(vmPos[p] - eye), L, reach, want);
				const V3 pw = eye + cam.ray((i + 0.5f) / S, (j + 0.5f) / S) * (depth[p] * cam.farZ);   // the world FOV's point (as if unmarked)
				lampCpu(pw, pn, plateAlbedo.x, length(pw - eye), L, reach, wantW);
				const float g = got[p * 4];
				const float e = std::fabs(g - want) / std::max(want, 0.01f);
				n++; err += e; errWorldFov += std::fabs(wantW - want) / std::max(want, 0.01f);
				if (e > 0.02f) { bad++; if (shown++ < 4) say("    plate pixel (%u, %u): %.4f, the CPU %.4f\n", i, j, g, want); }
			}
			else if (vm[p] == 0 && px[p].valid) {
				// the floor / wall: the stable pixels (the same answer at +-0.3 pixel) against the CPU without the body (rays end near the eye)
				float want = 0.0f; bool stable = true;
				for (int sub = 0; sub < 5 && stable; sub++) {
					const float du = sub == 1 ? -0.3f : sub == 2 ? 0.3f : 0.0f, dv = sub == 3 ? -0.3f : sub == 4 ? 0.3f : 0.0f;
					const V3 d = cam.ray((i + 0.5f + du) / S, (j + 0.5f + dv) / S);
					const Hit hh = trace(wAbsent, eye, d, 1e-3f, 1e9f, 0xFF, true);
					if (!hh.hit) { stable = false; break; }
					float v; lampCpu(eye + d * hh.t, hh.n, 0.5f, hh.t, L, reachFar, v);
					if (sub == 0) want = v; else if ((v > 0) != (want > 0)) stable = false;
				}
				if (!stable) continue;
				// would a visible (third-person) body shadow it? (the reverse shadow: the lamp sits in the body)
				const V3 P = px[p].pos, lift = P + px[p].n * (1.0f + 0.002f * px[p].viewZ);
				const V3 dl = L - lift; const float dd = length(dl);
				const bool bodyBlocks = want > 0 && trace(withObjects(w, bodyVariant(plain, body, 2)), lift, dl * (1.0f / dd), 0.0f, std::max(dd - 16.0f, 0.0f), 0xFF, true, true).hit;
				const float g = gotFar[p * 4];
				wn++;
				if (std::fabs(g - want) > 0.03f * want + 0.003f) wbad++;
				if (bodyBlocks) { reverse++; if (g > 0.5f * want && third[p * 4] < 0.1f * want + 1e-4f) reverseLit++; }
			}
		}
		const bool pass = n > 300 && bad <= n / 100 && errWorldFov / std::max(n, 1) > 0.05 && wn > 4000 && wbad <= wn / 100 && reverse > 200 && reverseLit >= reverse * 98 / 100;
		say("  A lamp in the head (the flashlight / Pip-Boy light at the eye), body hidden: plate %d pixels, mean error %.3f %%, %d over 2 %% (at the world FOV's point it would be off %.1f %%); "
			"floor + wall %d stable pixels, %d off the CPU without the body; %d of them a visible body shadows (the reverse shadow): %d lit with it hidden, dark with it visible -> %s\n",
			n, 100 * err / std::max(n, 1), bad, 100 * errWorldFov / std::max(n, 1), wn, wbad, reverse, reverseLit, pass ? "ok" : "BAD");
		if (!pass) { ok = false; dumpPfm("ptviewmodel_A", got, S, S, 4, 0); }
	}
	// ---- B: a lamp behind and above the player, far from the eye: the floor keeps the body's shadow, the plate is lit
	{
		const V3 L = { 0, -120, 420 };
		const float reach = 2500.0f;
		std::vector<float> got, absent;
		if (!run(0, L, reach, 3, 8, false, got) || !run(1, L, reach, 3, 8, false, absent)) return false;
		int n = 0, bad = 0, shadow = 0, shadowKept = 0, fn = 0, fbad = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const UINT p = j * S + i;
			if (vm[p] == 1) {
				float want; lampCpu(vmPos[p], pn, plateAlbedo.x, length(vmPos[p] - eye), L, reach, want);
				n++; if (std::fabs(got[p * 4] - want) > 0.02f * want + 0.002f) bad++;
			}
			else if (vm[p] == 0 && px[p].valid) {
				float want = 0.0f; bool stable = true;
				for (int sub = 0; sub < 5 && stable; sub++) {
					const float du = sub == 1 ? -0.3f : sub == 2 ? 0.3f : 0.0f, dv = sub == 3 ? -0.3f : sub == 4 ? 0.3f : 0.0f;
					const V3 d = cam.ray((i + 0.5f + du) / S, (j + 0.5f + dv) / S);
					const Hit hh = trace(wAbsent, eye, d, 1e-3f, 1e9f, 0xFF, true);
					if (!hh.hit) { stable = false; break; }
					float v; lampCpu(eye + d * hh.t, hh.n, 0.5f, hh.t, L, reach, v);
					if (sub == 0) want = v; else if ((v > 0) != (want > 0)) stable = false;
				}
				if (!stable) continue;
				fn++;
				if (std::fabs(got[p * 4] - want) > 0.03f * want + 0.003f) fbad++;
				if (want == 0.0f && absent[p * 4] > 0.01f) { shadow++; if (got[p * 4] < 0.25f * absent[p * 4]) shadowKept++; }
			}
		}
		// (the floor's check: the camera rays' sub-pixel jitter reaches +-0.5 pixel, the CPU's stability test +-0.3: a few shadow-edge pixels)
		const bool pass = n > 300 && bad <= n / 100 && fn > 4000 && fbad <= fn / 100 && shadow > 100 && shadowKept >= shadow * 95 / 100;
		say("  B lamp behind and above (far from the eye), body hidden: plate %d pixels, %d off the CPU (lit: rays from near the eye pass the body); floor + wall %d stable pixels, %d off; "
			"the body's shadow %d pixels, %d kept -> %s\n", n, bad, fn, fbad, shadow, shadowKept, pass ? "ok" : "BAD");
		if (!pass) { ok = false; dumpPfm("ptviewmodel_B", got, S, S, 4, 0); }
	}
	// ---- C: DebugView 1: the plate's colour where captured, the game's pixel where not
	{
		std::vector<float> got;
		if (!run(0, eye, 10.0f, 1, 1, false, got)) return false;
		int n = 0, bad = 0, kn = 0, kbad = 0;
		const float fr[3] = { std::pow(0.3f, 2.2f), std::pow(0.6f, 2.2f), std::pow(0.9f, 2.2f) };
		const float al[3] = { plateAlbedo.x, plateAlbedo.y, plateAlbedo.z };
		for (UINT p = 0; p < S * S; p++) {
			if (vm[p] == 1) { n++; for (int ch = 0; ch < 3; ch++) if (std::fabs(got[p * 4 + ch] - al[ch]) > 2e-3f) { bad++; break; } }
			else if (vm[p] == 2) { kn++; for (int ch = 0; ch < 3; ch++) if (std::fabs(got[p * 4 + ch] - fr[ch]) > 2e-3f) { kbad++; break; } }
		}
		const bool pass = n > 300 && bad == 0 && kn > 100 && kbad == 0;
		say("  C first hit's colour (DebugView 1): %d captured plate pixels, %d not the plate's colour; %d not-captured ones, %d not the game's pixel -> %s\n", n, bad, kn, kbad, pass ? "ok" : "BAD");
		if (!pass) { ok = false; dumpPfm("ptviewmodel_C", got, S, S, 4, 0); }
	}
	// ---- D: DebugView 5 under a uniform sky (1): the plate's arriving sky light, body hidden / absent / visible
	{
		double mean[3] = {};
		for (int variant = 0; variant < 3; variant++) {
			std::vector<float> got;
			if (!run(variant, eye, 10.0f, 5, 16, true, got)) return false;
			int n = 0;
			for (UINT p = 0; p < S * S; p++) if (vm[p] == 1) { mean[variant] += got[p * 4]; n++; }
			mean[variant] /= std::max(n, 1);
		}
		const bool pass = std::fabs(mean[0] - mean[1]) < 0.01 && mean[1] - mean[2] > 0.03;
		say("  D the plate's one-bounce sky light (DebugView 5, 16 paths): body hidden %.4f, absent %.4f, visible %.4f -> %s\n", mean[0], mean[1], mean[2], pass ? "ok" : "BAD");
		if (!pass) ok = false;
	}
	releaseWorld(h, ids);
	return ok;
}

// ==============================================================================================================================
// vmprep: the game's own prep pass (src/core/RtViewModel.h kPrepPS, ps_3_0 on the fork's D3D9 device, drawn as RtPathTrace.cpp
// ViewModelPrep draws it) against the CPU: the same plate as ptviewmodel in the view model's depth (reversed, its own near plane) and in
// the captured normals (octahedral camera-relative world normal, its true camera distance); part of the plate left with the world's
// captured values (not captured), a block of world pixels marked a person (alpha 2). Every pixel's code and normal must be the CPU's:
// the plate 4 + s with its view-space normal, the uncaptured part -1, people 2, the rest 1 with TESR_NormalsBuffer's normal.
static void octEncodeSigned(V3 n, float& x, float& y) {   // Includes/Shadow.hlsl ShadowEncodeNormal: octahedral in [-1, 1]
	const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
	x = n.x / l1; y = n.y / l1;
	if (n.z < 0) { const float tx = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f), ty = (1 - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f); x = tx; y = ty; }
}
static bool sceneVmPrep(Ctx& c) {
	Host& h = *c.host;
	const V3 eye = { 0, 0, 150 };
	World w;
	w.meshes.resize(2);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	w.meshes[1].quad({ -3000, 500, 0 }, { 3000, 500, 0 }, { 3000, 500, 1500 }, { -3000, 500, 1500 });
	Object floor; floor.mesh = 0; floor.land = true; Object wall; wall.mesh = 1;
	w.objects = { floor, wall };
	const Camera cam = Camera::lookDir(eye, { 0, 1, -0.45f }, { 0, 0, 1 }, 1.2f, 1.2f);
	const float s = 1.45f, nearVm = 4.0f, farZ = cam.farZ;
	const Camera vmCam = Camera::lookDir(eye, { 0, 1, -0.45f }, { 0, 0, 1 }, 1.2f * s, 1.2f * s);
	VmPlate pl;
	pl.c = eye + cam.f * 40.0f + cam.r * 14.0f - cam.u * 12.0f;
	pl.a = cam.r * 10.0f;
	pl.b = normalize(cam.f * 1.0f + cam.u * 0.25f) * 15.0f;
	V3 pn = normalize(cross(pl.a, pl.b));
	if (dot(pn, eye - pl.c) < 0) pn = -pn;
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(w, cam, px, depth, normals);
	std::vector<float> captured(S * S * 4, 0.0f), vmDepth(S * S, 0.0f);
	std::vector<int> want(S * S, 0);   // 1 world, 2 person, 3 the plate captured, 4 the plate not captured
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const UINT p = j * S + i;
		float* cp = &captured[p * 4];
		if (px[p].valid) {   // the world's captured surface
			float ex, ey; octEncodeSigned(px[p].n, ex, ey);
			cp[0] = ex; cp[1] = ey; cp[2] = px[p].viewZ * length(cam.ray((i + 0.5f) / S, (j + 0.5f) / S)); cp[3] = (i < 20 && j > 100) ? 2.0f : 1.0f;
		}
		want[p] = cp[3] > 1.5f ? 2 : 1;
		float t;
		const V3 d = vmCam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		if (vmHit(pl, eye, d, t)) {
			vmDepth[p] = (nearVm * farZ / t - nearVm) / (farZ - nearVm);   // reversed: the inverse of CombineDepth's ViewZFromRaw
			if (i >= S * 0.62f) {   // captured: the plate's own normal and distance
				float ex, ey; octEncodeSigned(pn, ex, ey);
				cp[0] = ex; cp[1] = ey; cp[2] = length(d * t); cp[3] = 1.0f;
				want[p] = 3;
			}
			else want[p] = 4;
		}
	}
	Tex tn, tc, tm, out;
	if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, tn) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, tc) || !makeTex(h, S, S, D3DFMT_R32F, false, tm) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, out)) return false;
	if (!fillTex(tn, normals, 4) || !fillTex(tc, captured, 4) || !fillTex(tm, vmDepth, 1)) return false;
	ID3DBlob* code = nullptr;
	if (!compileHlsl(h, RtViewModel::kPrepPS, "ps_3_0", &code)) return false;
	IDirect3DPixelShader9* ps = nullptr;
	CHECKHR(h.dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps), "CreatePixelShader");
	code->Release();
	const float k[6][4] = {
		{ 1.0f / (cam.p00 * s), 1.0f / (cam.p11 * s), nearVm, farZ },
		{ 1.0f, s, RtViewModel::kMatchShare, RtViewModel::kMatchUnits },
		{ cam.r.x, cam.u.x, cam.f.x, 0 }, { cam.r.y, cam.u.y, cam.f.y, 0 }, { cam.r.z, cam.u.z, cam.f.z, 0 }, { 0, 0, 0, 1 } };   // world -> view (x right, y up, z forward)
	h.dev->SetVertexShader(nullptr); h.dev->SetPixelShader(ps);
	h.dev->SetPixelShaderConstantF(0, &k[0][0], 6);
	h.dev->SetTexture(0, tn.t); h.dev->SetTexture(1, tc.t); h.dev->SetTexture(2, tm.t);
	for (DWORD st = 0; st < 3; st++) {
		h.dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT); h.dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		h.dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		h.dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); h.dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	h.dev->SetRenderState(D3DRS_ZENABLE, FALSE); h.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DSurface9* back = nullptr; h.dev->GetRenderTarget(0, &back);
	h.dev->SetRenderTarget(0, out.s);
	D3DVIEWPORT9 vp = { 0, 0, S, S, 0.0f, 1.0f }; h.dev->SetViewport(&vp);
	h.dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);   // as RtPathTrace.cpp ViewModelPrep
	const float x1 = float(S) - 0.5f, y1 = float(S) - 0.5f;
	const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
	h.dev->BeginScene();
	h.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	h.dev->EndScene();
	h.dev->SetRenderTarget(0, back); if (back) back->Release();
	h.dev->SetTexture(0, nullptr); h.dev->SetTexture(1, nullptr); h.dev->SetTexture(2, nullptr); h.dev->SetPixelShader(nullptr);
	ps->Release();
	std::vector<float> got;
	if (!readTex(h, out, got, 4)) return false;
	int n[5] = {}, bad[5] = {}, shown = 0;
	const V3 pnView = { dot(pn, cam.r), dot(pn, cam.u), dot(pn, cam.f) };
	for (UINT p = 0; p < S * S; p++) {
		const int kind = want[p];
		const float* g = &got[p * 4];
		bool ok = true;
		if (kind == 3) {
			ok = std::fabs(g[3] - (4.0f + s)) < 0.01f && std::fabs(g[0] - (pnView.x * 0.5f + 0.5f)) < 3e-3f && std::fabs(g[1] - (pnView.y * 0.5f + 0.5f)) < 3e-3f && std::fabs(g[2] - (pnView.z * 0.5f + 0.5f)) < 3e-3f;
		}
		else {
			const float wantA = kind == 4 ? -1.0f : kind == 2 ? 2.0f : 1.0f;
			ok = g[3] == wantA;
			for (int ch = 0; ch < 3; ch++) if (std::fabs(g[ch] - normals[p * 4 + ch]) > 2e-3f) ok = false;
		}
		n[kind]++;
		if (!ok) { bad[kind]++; if (shown++ < 4) say("    pixel (%u, %u) kind %d: %.4f %.4f %.4f %.3f\n", p % S, p / S, kind, g[0], g[1], g[2], g[3]); }
	}
	const bool pass = n[3] > 300 && n[4] > 100 && n[2] > 300 && n[1] > 5000 && bad[1] + bad[2] + bad[3] + bad[4] == 0;
	say("  the prep pass (ps_3_0, RtViewModel.h): world %d (%d wrong), people %d (%d), the plate captured %d (%d: code 4 + s and its view-space normal), not captured %d (%d: code -1) -> %s\n",
		n[1], bad[1], n[2], bad[2], n[3], bad[3], n[4], bad[4], pass ? "ok" : "BAD");
	for (Tex* t : { &tn, &tc, &tm, &out }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	return pass;
}

// ==============================================================================================================================
// hiddennear: the same rule in RtSunShadow, RtLampTrace and RtAOTrace. A counter in front of the player (its top within reach of the eye),
// the floor further away; the hidden body round the eye, its third-person gun over the counter. Each pass runs on three TLAS variants
// (body hidden / absent / visible) and must give, bit for bit: within reach of the eye (the counter top; every pixel for a lamp AT the
// eye) the absent body's answer; beyond it (the floor) the visible body's (its shadow and occlusion stay). And the visible body must
// change some of both (else the test sees nothing).
static bool sceneHiddenNear(Ctx& c) {
	Host& h = *c.host;
	const V3 eye = { 0, 0, 140 };
	World w;
	w.meshes.resize(2);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });   // floor
	w.meshes[1].box({ -45, 16, 0 }, { 45, 60, 95 });                                                     // the counter
	Object floor; floor.mesh = 0; floor.land = true; floor.id = 1;
	Object counter; counter.mesh = 1; counter.id = 2;
	const std::vector<Object> plain = { floor, counter };
	const FpBody body = addBody(w, eye, 106.0f);   // the third-person gun 11 units over the counter
	w.objects = plain;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, withObjects(w, bodyVariant(plain, body, 2)), ids)) return false;
	const Camera cam = Camera::lookDir(eye, { 0.25f, 1, -0.75f }, { 0, 0, 1 }, 1.0f, 1.0f);
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(withObjects(w, plain), cam, px, depth, normals);
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
	// the classes: 1 near (the surface within 70 of the eye), 2 far (beyond 95), 0 the band between / nothing
	std::vector<int> cls(S * S, 0);
	int nearN = 0, farN = 0;
	for (UINT p = 0; p < S * S; p++) if (px[p].valid) {
		const float d = length(px[p].pos - eye);
		cls[p] = d < 70.0f ? 1 : d > 95.0f ? 2 : 0;
		if (cls[p] == 1) nearN++; else if (cls[p] == 2) farN++;
	}
	say("  %d pixels within reach of the eye (the counter top), %d beyond (the floor, the counter's side)\n", nearN, farN);
	bool ok = nearN > 800 && farN > 2000;
	// compares variant results: hidden must equal absent on `nearSet`, visible on `farSet`; visible must differ from absent on both somewhere
	auto judge = [&](const char* name, const std::vector<float>* r, int stride, int ch, bool allNear) {
		int nearSame = 0, nearN2 = 0, farSame = 0, farN2 = 0, nearSeen = 0, farSeen = 0;
		for (UINT p = 0; p < S * S; p++) {
			const int k = allNear ? (px[p].valid ? 1 : 0) : cls[p];
			if (k == 0) continue;
			const float hid = r[0][p * stride + ch], abs_ = r[1][p * stride + ch], vis = r[2][p * stride + ch];
			if (k == 1) { nearN2++; if (hid == abs_) nearSame++; if (vis != abs_) nearSeen++; }
			else { farN2++; if (hid == vis) farSame++; if (vis != abs_) farSeen++; }
		}
		const bool pass = nearSame == nearN2 && farSame == farN2 && nearSeen > 20 && (allNear || farSeen > 20);
		say("    %s: %s %d pixels: %d = the absent body's (a visible body changes %d); beyond reach %d pixels: %d = the visible body's (it changes %d) -> %s\n",
			name, allNear ? "every" : "within reach", nearN2, nearSame, nearSeen, farN2, farSame, farSeen, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	};
	// ---- RtSunShadow: a low sun from behind (the body's shadow falls forward over the counter and the floor)
	{
		const V3 sun = normalize({ 0.15f, -0.75f, 0.55f });
		std::vector<float> r[3];
		for (int v = 0; v < 3; v++) {
			SunConstants k = {};
			put4(k.size, float(S), float(S), 1.0f / S, 1.0f / S);
			put4(k.forward, cam.f, cam.farZ); put4(k.right, cam.r * (1.0f / cam.p00), 0); put4(k.up, cam.u * (1.0f / cam.p11), 0);
			put4(k.sun, sun, 1.0f);
			put4(k.bias, 1.0f, 0.001f, 0.5f, 30000.0f);
			put4(k.range, 100000.0f, 1.0f, 255.0f, 0.0f);
			put4(k.alpha, 0.0f, 0.0f, 0.0f, 1.0f);   // no alpha test, hard, blocker distances out
			put4(k.full, float(S), float(S), 0.0f, 0.0f);
			put4(k.camera, eye, 0.0f);
			h.AddPass(gM.shSun, { c.depth, c.normals, gT.sunVis, c.alphaStandIn, c.normals, gT.sunHit }, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(withObjects(w, bodyVariant(plain, body, v)), ids, eye)) return false;
			if (!h.Read(gT.sunVis, r[v])) return false;
			h.EndFrame();
		}
		judge("RT sun", r, 1, 0, false);
	}
	// ---- RtLampTrace: a lamp in the head (a Pip-Boy light / the flashlight at the eye), then a ceiling lamp behind the player
	for (int lampCase = 0; lampCase < 2; lampCase++) {
		std::vector<Lamp> lamps(1);
		lamps[0].pos = lampCase == 0 ? eye + V3(0, 4, -6) : V3(10, -110, 380);
		lamps[0].reach = 3000; lamps[0].intensity = 1; lamps[0].radius = 3000;
		if (!h.Upload(c.lampTable, lampTable(lamps, eye), 0, 0, 1024, 1)) return false;
		std::vector<float> r[3];
		for (int v = 0; v < 3; v++) {
			LampConstants k; lampPassConstants(k, cam, 1, 1);
			put4(k.params2, 0.0f, float(0x63), float(0x63), 1.0f);   // land + objects + people (0x20 actors, 0x40 the player's parts)
			put4(k.jitter, 0.0f, 0.0f, 0.0f, 0.0f);                   // no alpha test
			const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn };
			h.AddPass(c.shLamp, imgs, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(withObjects(w, bodyVariant(plain, body, v)), ids, eye)) return false;
			if (!h.Read(c.lampResult, r[v])) return false;
			h.EndFrame();
		}
		judge(lampCase == 0 ? "RT lamp in the head (rays end at the eye)" : "RT lamp behind the player", r, 2, 0, lampCase == 0);
	}
	// ---- RtAOTrace (the plain pass: AO + sky visibility)
	{
		std::vector<float> in(S * S * 2, 0.0f);
		for (UINT p = 0; p < S * S; p++) if (px[p].valid) { in[p * 2] = px[p].viewZ; in[p * 2 + 1] = packAO(px[p].n); }
		if (!h.Upload(gM.aoIn, in)) return false;
		std::vector<float> r[3];
		for (int v = 0; v < 3; v++) {
			AOConstants k = {};
			const V3 R = cam.r, U = cam.u, F = cam.f;
			const float m[16] = { R.x, R.y, R.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, 0, 0, 0, 1 };
			std::memcpy(k.viewToWorld, m, sizeof(m));
			put4(k.projection, 1.0f / cam.p00, 1.0f / cam.p11, float(S), float(S));
			put4(k.ao, 300.0f, 8.0f, 0.0f, 0.0f); put4(k.sky, 3000.0f, 16.0f, 0.0f, 0.0f);
			put4(k.gi2, 0, 0, 0, 1); put4(k.gi3, 1, 1, 0, 0);
			h.AddPass(gM.shAO, { gM.aoIn, gM.aoOut, gM.aoGi, c.alphaStandIn, gM.aoLight, gM.albedoAtlas }, 0x6, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(withObjects(w, bodyVariant(plain, body, v)), ids, eye)) return false;
			if (!h.Read(gM.aoOut, r[v])) return false;
			h.EndFrame();
		}
		judge("RT AO", r, 4, 0, false);
		judge("RT sky visibility", r, 4, 1, false);
	}
	releaseWorld(h, ids);
	return ok;
}
