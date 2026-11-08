// NvrRtHost (2026-11-07, ptaaa-fog): RT fog visibility (Compute\Rt\RtFogVisibility.comp, core\RtFogVisibility.cpp, constants by the
// game's src\core\RtFogGrid.h): the sun's visibility per cell of the volumetric light's froxel grid, traced, against the CPU's rays.
// The world: a closed room whose sun-facing wall has a window (a shaft of sunlit air inside, the rest of the room shaded), an
// alpha-tested checker card outside (the cascade-miss case: cut-out cards are not in the shadow maps' casters the way the RT scene has
// them; here every cell under it must take the checker's shadow), a floor. A 32 x 32 column grid (64 slices: 65536 cells).
//   1 exact: one ray a cell at its centre, fixed: every stable cell (its answer the same 0.5 unit round the point) = the CPU's ray
//   2 exact: four fixed rays a cell: the share = the CPU's four rays
//   3 temporal: two jittered rays a cell, 24 frames with the history: against the cell's sunlit share (a 4 x 4 x 4 point reference), the
//     RMS error of the partly lit cells < 0.5 x one frame's, no bias
//   4 moving: then 16 frames of a camera sliding 8 units and turning 0.5 degree a frame: RMS < 0.65 x one frame's at the end; a control
//     whose history ignores the move (last frame's camera = this one's) > 1.3 x worse
// Temporal 0.9 / TemporalMoving 0.7 as the game's defaults (NVR_FOG_TEMPORAL, NVR_FOG_TEMPORAL_MOVING, NVR_FOG_RAYS override them for a sweep)

struct FogCtx { int visA = -1, visB = -1, shader = -1; };
static FogCtx gFog;
static const unsigned kFogCols = 32, kFogRows = 32;
static const int kFogSlot = 97;	// the card's alpha atlas slot (no other scene uses it)
static const float kYawA = 0.197f;	// the camera's heading (radians from +x)
static const int kMoveFrames = 16;	// the moving part: a walk past the window (FNV runs ~6 units a frame at 60 fps), turning
static const float kMoveStep = 8.0f, kMoveYaw = 0.5f;

static bool registerFog(Host& h, const std::string& spv) {
	gFog.visA = h.AddImage("fog visibility 0", kFogCols * 8, kFogRows * 8, NVR_EXT_SHARED_R16F);
	gFog.visB = h.AddImage("fog visibility 1", kFogCols * 8, kFogRows * 8, NVR_EXT_SHARED_R16F);
	gFog.shader = h.AddShader(spv + "\\RtFogVisibility.spv");
	return gFog.visA > 0 && gFog.visB > 0;
}

static World fogWorld() {
	World w;
	Mesh floor; floor.quad({ -6000, -6000, 0 }, { 6000, -6000, 0 }, { 6000, 6000, 0 }, { -6000, 6000, 0 });
	Mesh room;
	const float X = 600, Y = 600, H = 500;
	room.quad({ -X, -Y, 0 }, { -X, Y, 0 }, { -X, Y, H }, { -X, -Y, H });     // -x wall
	room.quad({ -X, Y, 0 }, { X, Y, 0 }, { X, Y, H }, { -X, Y, H });         // +y
	room.quad({ -X, -Y, 0 }, { -X, -Y, H }, { X, -Y, H }, { X, -Y, 0 });     // -y
	room.quad({ -X, -Y, H }, { X, -Y, H }, { X, Y, H }, { -X, Y, H });       // roof
	// the +x wall (toward the sun) round a window y -150 .. 150, z 150 .. 350
	room.quad({ X, -Y, 0 }, { X, -Y, 150 }, { X, Y, 150 }, { X, Y, 0 });
	room.quad({ X, -Y, 350 }, { X, -Y, H }, { X, Y, H }, { X, Y, 350 });
	room.quad({ X, -Y, 150 }, { X, -Y, 350 }, { X, -150, 350 }, { X, -150, 150 });
	room.quad({ X, 150, 150 }, { X, 150, 350 }, { X, Y, 350 }, { X, Y, 150 });
	Mesh card; card.quad({ -250, -250, 0 }, { 250, -250, 0 }, { 250, 250, 0 }, { -250, 250, 0 });
	w.meshes = { floor, room, card };
	Object f; f.mesh = 0; f.id = 1; f.land = true;
	Object r; r.mesh = 1; r.id = 2;
	Object k; k.mesh = 2; k.id = 3; k.offset = { 1500, -300, 380 }; k.alphaTested = true; k.alphaSlot = kFogSlot; k.alphaRef = 128;
	w.objects = { f, r, k };
	w.slots[kFogSlot] = [](int x, int y) { return ((x / 8 + y / 8) & 1) ? 1.0f : 0.0f; };
	return w;
}

static RtFogGrid::View fogView(const Camera& cam, float maxDistance, V3 sun) {
	RtFogGrid::View v;
	v.eye[0] = cam.eye.x; v.eye[1] = cam.eye.y; v.eye[2] = cam.eye.z;
	const V3 axes[3] = { cam.f, cam.r, cam.u };
	for (int i = 0; i < 3; i++) { v.forward[i] = (&axes[0].x)[i]; v.right[i] = (&axes[1].x)[i]; v.up[i] = (&axes[2].x)[i]; v.sun[i] = (&sun.x)[i]; }
	v.p00 = cam.p00; v.p11 = cam.p11; v.maxDistance = maxDistance; v.columns = kFogCols; v.rows = kFogRows; v.valid = true;
	return v;
}

// RtFogVisibility.comp cellPoint + viewRay, in the same float steps: sample kk of n in cell (cx, cy, cz), scene space (eye 0)
static V3 fogCellPoint(const RtFogGrid::PassConstants& k, int cx, int cy, int cz, int kk, int n, float ou = -1.0f, float ov = -1.0f, float od = -1.0f) {
	float o[3] = { fract(0.5f + float(kk) * 0.7548777f), fract(0.5f + float(kk) * 0.5698403f), (float(kk) + 0.5f) / float(n) };
	if (k.trace[1] > 0.5f) {
		const float h = fract(52.9829189f * fract(0.06711056f * float(cx + 13 * cz) + 0.00583715f * float(cy + 29 * cz)));
		const float j[3] = { fract(h + k.origin[3] * 0.7548777f), fract(fract(h * 1.618034f) + k.origin[3] * 0.5698403f), fract(fract(h * 2.236068f) + k.origin[3] * 0.6180340f) };
		o[0] = fract(o[0] + j[0]); o[1] = fract(o[1] + j[1]); o[2] = (float(kk) + j[2]) / float(n);
	}
	if (ou >= 0.0f) { o[0] = ou; o[1] = ov; o[2] = od; }   // a given point (the reference's)
	const float s0 = float(cz) / 64.0f, s1 = (float(cz) + 1.0f) / 64.0f;
	const float nearD = k.grid[3] * s0 * s0, farD = k.grid[3] * s1 * s1;
	const float u = (float(cx) + o[0]) / k.grid[0], v = (float(cy) + o[1]) / k.grid[1];
	const V3 f = { k.forward[0], k.forward[1], k.forward[2] }, r = { k.right[0], k.right[1], k.right[2] }, up = { k.up[0], k.up[1], k.up[2] };
	const V3 dir = f + r * (2.0f * u - 1.0f) - up * (2.0f * v - 1.0f);
	return dir * (nearD + (farD - nearD) * o[2]);
}

// The CPU's ray from a scene-space point toward the sun: 1 free, 0 blocked (the shader's mask, alpha test, decals never block); who: the blocker
static float fogRayCpu(const World& w, V3 eye, const RtFogGrid::PassConstants& k, V3 point, int* who = nullptr) {
	if (who) *who = -1;
	if (length(point) > k.forward[3]) return 1.0f;
	const V3 sun = { k.sun[0], k.sun[1], k.sun[2] };
	const Hit h = trace(w, eye + point, sun, 0.0f, k.trace[0], uint32_t(k.rays[2]) & (k.rays[3] > 0.5f ? 0xFFu : 0xEFu), k.rays[3] > 0.5f, true);
	if (h.hit && who) *who = h.object;
	return h.hit ? 0.0f : 1.0f;
}

// A cell's value in the pass's image (slice z in tile (z mod 8, z / 8) of the columns x rows)
static float fogCellValue(const std::vector<float>& img, int cx, int cy, int cz) {
	return img[size_t((cz / 8) * kFogRows + cy) * (kFogCols * 8) + size_t((cz % 8) * kFogCols + cx)];
}

// The cell's sunlit share: 4 x 4 x 4 points at the cell's (u, v, depth-share) centres of a regular split
static void fogReference(const World& w, const Camera& cam, const RtFogGrid::PassConstants& k, std::vector<float>& ref) {
	ref.assign(size_t(kFogCols) * kFogRows * 64, 0.0f);
	for (int cz = 0; cz < 64; cz++) for (int cy = 0; cy < int(kFogRows); cy++) for (int cx = 0; cx < int(kFogCols); cx++) {
		float lit = 0;
		for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int d = 0; d < 4; d++)
			lit += fogRayCpu(w, cam.eye, k, fogCellPoint(k, cx, cy, cz, 0, 1, (a + 0.5f) / 4, (b + 0.5f) / 4, (d + 0.5f) / 4));
		ref[(size_t(cz) * kFogRows + cy) * kFogCols + cx] = lit / 64.0f;
	}
}

// RMS error (all cells, and the partly lit ones: reference in 0.02 .. 0.98) and the mean signed error, image vs reference
struct FogErr { double rmsAll = 0, rmsPartial = 0, bias = 0; int partial = 0; };
static FogErr fogError(const std::vector<float>& img, const std::vector<float>& ref) {
	FogErr e; double sa = 0, sp = 0, sb = 0; int n = 0;
	for (int cz = 0; cz < 64; cz++) for (int cy = 0; cy < int(kFogRows); cy++) for (int cx = 0; cx < int(kFogCols); cx++) {
		const float r = ref[(size_t(cz) * kFogRows + cy) * kFogCols + cx], g = fogCellValue(img, cx, cy, cz);
		const double d = double(g) - r;
		sa += d * d; sb += d; n++;
		if (r > 0.02f && r < 0.98f) { sp += d * d; e.partial++; }
	}
	e.rmsAll = std::sqrt(sa / n); e.rmsPartial = e.partial ? std::sqrt(sp / e.partial) : 0.0; e.bias = sb / n;
	return e;
}

static bool sceneFogVis(Ctx& c) {
	Host& h = *c.host;
	World w = fogWorld();
	if (!uploadSlot(h, c.alphaAtlas, kFogSlot, w.slots[kFogSlot])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const V3 sun = normalize({ 1.0f, 0.3f, 0.6f });
	const float maxDistance = 4500.0f;
	// inside the room, looking at the window (the shaft's cells near the eye are small) and past it at the card outside
	const Camera camA = Camera::lookDir({ -450, -400, 250 }, { std::cos(kYawA), std::sin(kYawA), -0.05f }, { 0, 0, 1 }, 1.0f, 1.0f);
	RtFogGrid::Settings s;
	s.range = 8000.0f; s.rayLength = 20000.0f; s.mask = 0xFFu; s.alphaTested = true;
	bool ok = true;
	auto run = [&](const Camera& cam, const RtFogGrid::PassConstants& k, int out, int hist, std::vector<float>& img) -> bool {
		const std::vector<int> imgs = { out, hist, c.alphaAtlas };
		h.AddPass(gFog.shader, imgs, 0x1u, &k, sizeof(k), (kFogCols * 8 + 7) / 8, (kFogRows * 8 + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (!h.Read(out, img)) return false;
		h.EndFrame();
		return true;
	};
	const RtFogGrid::View viewA = fogView(camA, maxDistance, sun);

	// ---- 1 + 2: fixed points, one and four rays a cell, against the CPU's rays (cells whose answer changes within 0.5 unit skipped)
	for (int n : { 1, 4 }) {
		s.rays = n; s.temporal = 0.0f; s.jitter = false;
		RtFogGrid::PassConstants k;
		RtFogGrid::Fill(k, viewA, viewA, 0.0f, true, s, 0);
		std::vector<float> img;
		if (!run(camA, k, gFog.visA, gFog.visB, img)) return false;
		int checked = 0, bad = 0, shadowed = 0, byCard = 0, throughHoles = 0, shaft = 0, unstable = 0, shown = 0;
		for (int cz = 0; cz < 64; cz++) for (int cy = 0; cy < int(kFogRows); cy++) for (int cx = 0; cx < int(kFogCols); cx++) {
			float want = 0; bool stable = true;
			for (int kk = 0; kk < n && stable; kk++) {
				const V3 p = fogCellPoint(k, cx, cy, cz, kk, n);
				int who = -1;
				const float v = fogRayCpu(w, camA.eye, k, p, &who);
				const V3 e[6] = { {0.5f,0,0}, {-0.5f,0,0}, {0,0.5f,0}, {0,-0.5f,0}, {0,0,0.5f}, {0,0,-0.5f} };
				for (const V3& d : e) if (fogRayCpu(w, camA.eye, k, p + d) != v) { stable = false; break; }
				want += v;
				if (n == 1 && stable) {
					if (v == 0.0f) { shadowed++; if (who == 2) byCard++; }
					else {
						// lit through the card's cut-outs: the same ray with the card solid would have been blocked by it
						const V3 sd = { k.sun[0], k.sun[1], k.sun[2] };
						const Hit solid = trace(w, camA.eye + p, sd, 0.0f, k.trace[0], 0xFF, true, true, true);
						if (solid.hit && solid.object == 2) throughHoles++;
						const V3 wp = camA.eye + p;
						if (wp.x > -600 && wp.x < 600 && wp.y > -600 && wp.y < 600 && wp.z > 0 && wp.z < 500) shaft++;
					}
				}
			}
			if (!stable) { unstable++; continue; }
			want /= float(n);
			const float got = fogCellValue(img, cx, cy, cz);
			checked++;
			if (std::fabs(got - want) > 1e-3f) { bad++; if (shown++ < 5) say("    cell (%d, %d, slice %d): got %.3f, the CPU's rays %.3f\n", cx, cy, cz, got, want); }
		}
		const int cells = int(kFogCols * kFogRows * 64);
		bool pass = bad == 0 && checked > cells * 9 / 10;
		if (n == 1) pass = pass && byCard > 50 && throughHoles > 50 && shaft > 50 && shadowed > 1000;
		say("  fixed points, %d ray%s a cell: %d of %d cells checked (%d on an edge skipped), %d mismatches%s", n, n > 1 ? "s" : "", checked, cells, unstable, bad,
			n == 1 ? "" : "\n");
		if (n == 1) say("; %d shadowed (%d by the alpha-tested card), %d lit through the card's cut-outs, %d lit in the window's shaft\n", shadowed, byCard, throughHoles, shaft);
		if (!pass) { ok = false; dumpPfm(n == 1 ? "fog_fixed1" : "fog_fixed4", img, kFogCols * 8, kFogRows * 8, 1); }
	}

	// ---- 3: jittered points + the history, the camera still
	s.rays = 2; s.temporal = 0.9f; s.jitter = true;
	s.temporalMoving = 0.7f;
	if (const char* e = std::getenv("NVR_FOG_TEMPORAL")) s.temporal = float(std::atof(e));   // (experiments)
	if (const char* e = std::getenv("NVR_FOG_TEMPORAL_MOVING")) s.temporalMoving = float(std::atof(e));
	if (const char* e = std::getenv("NVR_FOG_RAYS")) s.rays = std::atoi(e);
	std::vector<float> refA;
	{ RtFogGrid::PassConstants k; RtFogGrid::Fill(k, viewA, viewA, 0.0f, true, s, 0); fogReference(w, camA, k, refA); }
	FogErr single;
	{
		RtFogGrid::PassConstants k; RtFogGrid::Fill(k, viewA, viewA, 0.0f, true, s, 7);
		std::vector<float> img;
		if (!run(camA, k, gFog.visA, gFog.visB, img)) return false;
		single = fogError(img, refA);
	}
	// the moving part runs twice: with the history reprojected (the game's), and the control (last frame's camera taken as this one's)
	FogErr still, moved[2], singleB;
	for (int variant = 0; variant < 2; variant++) {
		RtFogGrid::View last;
		unsigned frame = 100, since = 0;
		std::vector<float> img;
		int out = gFog.visA, hist = gFog.visB;
		for (int i = 0; i < 24; i++) {
			RtFogGrid::PassConstants k;
			RtFogGrid::Fill(k, viewA, last, RtFogGrid::HistoryWeight(since, s.temporal), true, s, frame++);
			if (!run(camA, k, out, hist, img)) return false;
			std::swap(out, hist);
			last = viewA; since++;
		}
		if (variant == 0) still = fogError(img, refA);
		// kMoveFrames frames sliding kMoveStep units sideways and turning kMoveYaw degrees a frame
		Camera cam = camA;
		RtFogGrid::PassConstants kEnd;
		for (int i = 1; i <= kMoveFrames; i++) {
			const float yaw = kYawA + kMoveYaw * i * 3.14159265f / 180.0f;
			const V3 fwd = { std::cos(yaw), std::sin(yaw), -0.05f };
			cam = Camera::lookDir(camA.eye + V3(0, kMoveStep * i, 0), fwd, { 0, 0, 1 }, 1.0f, 1.0f);
			const RtFogGrid::View now = fogView(cam, maxDistance, sun);
			const RtFogGrid::View before = variant == 0 ? last : now;   // the control: no reprojection
			RtFogGrid::Fill(kEnd, now, before, RtFogGrid::HistoryWeight(since, s.temporal), true, s, frame++);
			if (!run(cam, kEnd, out, hist, img)) return false;
			std::swap(out, hist);
			last = now; since++;
		}
		std::vector<float> refB;
		fogReference(w, cam, kEnd, refB);
		moved[variant] = fogError(img, refB);
		if (variant == 0) {
			RtFogGrid::PassConstants k; RtFogGrid::Fill(k, last, last, 0.0f, true, s, 3);
			std::vector<float> one;
			if (!run(cam, k, gFog.visA, gFog.visB, one)) return false;
			singleB = fogError(one, refB);
		}
	}
	const bool passStill = still.rmsPartial < 0.5 * single.rmsPartial && std::fabs(still.bias) < 0.01;
	const bool passMove = moved[0].rmsAll < 0.65 * singleB.rmsAll && moved[1].rmsAll > 1.3 * moved[0].rmsAll;
	say("  two jittered rays a cell vs the cell's sunlit share (4 x 4 x 4 points; %d partly lit cells): one frame RMS %.4f (partly lit %.4f); "
		"24 frames with the history %.4f (partly lit %.4f, bias %+.4f) -> %s\n", single.partial, single.rmsAll, single.rmsPartial, still.rmsAll, still.rmsPartial, still.bias,
		passStill ? "ok" : "BAD");
	say("  then %d frames sliding %.0f units + turning %.1f deg a frame: RMS %.4f (partly lit %.4f) vs one frame there %.4f (partly lit %.4f); control without the "
		"reprojection %.4f (partly lit %.4f) -> %s\n", kMoveFrames, kMoveStep, kMoveYaw, moved[0].rmsAll, moved[0].rmsPartial, singleB.rmsAll, singleB.rmsPartial,
		moved[1].rmsAll, moved[1].rmsPartial, passMove ? "ok" : "BAD");
	if (!passStill || !passMove) ok = false;
	releaseWorld(h, ids);
	return ok;
}
