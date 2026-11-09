// NvrRtHost, the second set of scenes (2026-11-06, included by rt_host.cpp before main): the sun shadow and AO / sky passes, the path
// tracer with the albedo atlas and the lamp image (all-lamp loop vs PTLightGrid, built by the game's RtLightGrid.h), both ReSTIR passes
// (RtLampTraceRestir, RtPathTraceGI + RtRestirGI), the game's D3D9 side (RtDenoise's fork passes, PointShadowForward.hlsl's
// PointShadowTraced lookup), and the scene picking rules (RtScenePick.h). README.md has the table.

struct Ctx2 {
	// images
	int sunVis = -1, sunHit = -1;
	int aoIn = -1, aoOut = -1, aoGi = -1, aoLight = -1;
	int albedoAtlas = -1, ptLamps = -1, ptAlbedoOut = -1, giResult = -1;
	int resA[2] = { -1, -1 }, resB[2] = { -1, -1 }, resC[2] = { -1, -1 }, surf[2] = { -1, -1 };
	int idsA = -1, idsB = -1;
	// shaders
	int shSun = -1, shAO = -1, shPathGrid = -1, shPathGI = -1, shRestirGI = -1, shRestirLamp = -1;
	std::string compute, includes;   // RtDenoise's .spv folder, PointShadowForward.hlsl's folder
};
static Ctx2 gM;
static const UINT kLightTexels = 4 * (256 + 32), kGridRows = 48;   // RtPathTrace.cpp: the lamp image

static bool registerMore(Host& h, const std::string& spv, const std::string& compute, const std::string& includes) {
	gM.compute = compute; gM.includes = includes;
	gM.sunVis = h.AddImage("sun visibility", S / 2, S / 2, NVR_EXT_SHARED_R16F);
	gM.sunHit = h.AddImage("sun blocker", S / 2, S / 2, NVR_EXT_SHARED_R16F);
	gM.aoIn = h.AddImage("ao input", S, S, NVR_EXT_SHARED_RG32F);
	gM.aoOut = h.AddImage("ao output", S, S, NVR_EXT_SHARED_RGBA16F);
	gM.aoGi = h.AddImage("ao gi", S, S, NVR_EXT_SHARED_RGBA16F);
	gM.aoLight = h.AddImage("ao frame light", S, S, NVR_EXT_SHARED_RGBA16F);
	gM.albedoAtlas = h.AddImage("albedo atlas", 2048, 2048, NVR_EXT_SHARED_RGBA8);
	gM.ptLamps = h.AddImage("pt lamps", kLightTexels, 3 + kGridRows, NVR_EXT_SHARED_RG32F);   // (2026-11-07: + the frame row, + the emissive row)
	gPtCostImage = gM.ptLamps;   // (2026-11-07 perf-ptcost) NVR_RT_HOST_PTCOST patches its frame row
	gM.ptAlbedoOut = h.AddImage("pt first albedo", S, S, NVR_EXT_SHARED_RGBA16F);
	gM.giResult = h.AddImage("pt gi result", S, S, NVR_EXT_SHARED_RGBA16F);
	for (int i = 0; i < 2; i++) {
		gM.resA[i] = h.AddImage("gi reservoir a", S, S, NVR_EXT_SHARED_RG32F);
		gM.resB[i] = h.AddImage("gi reservoir b", S, S, NVR_EXT_SHARED_RG32F);
		gM.resC[i] = h.AddImage("gi reservoir c", S, S, NVR_EXT_SHARED_RGBA16F);
		gM.surf[i] = h.AddImage("gi surface", S, S, NVR_EXT_SHARED_RG32F);
	}
	gM.idsA = h.AddImage("lamp ids history 0", S, S, NVR_EXT_SHARED_RGBA16F);
	gM.idsB = h.AddImage("lamp ids history 1", S, S, NVR_EXT_SHARED_RGBA16F);
	for (int i : { gM.sunVis, gM.sunHit, gM.aoIn, gM.aoOut, gM.aoGi, gM.aoLight, gM.albedoAtlas, gM.ptLamps, gM.ptAlbedoOut, gM.giResult,
		gM.resA[0], gM.resA[1], gM.resB[0], gM.resB[1], gM.resC[0], gM.resC[1], gM.surf[0], gM.surf[1], gM.idsA, gM.idsB }) if (i < 0) return false;
	gM.shSun = h.AddShader(spv + "\\RtSunShadow.spv");
	gM.shAO = h.AddShader(spv + "\\RtAOTrace.spv");
	gM.shPathGrid = h.AddShader(spv + "\\RtPathTraceGrid.spv");
	gM.shPathGI = h.AddShader(spv + "\\RtPathTraceGI.spv");
	gM.shRestirGI = h.AddShader(spv + "\\RtRestirGI.spv");
	gM.shRestirLamp = h.AddShader(spv + "\\RtLampTraceRestir.spv");
	return true;
}

static float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
static float fract(float x) { return x - std::floor(x); }

// ==============================================================================================================================
// Scene: sun shadows (RtSunShadow.comp, as RtSunShadows.cpp records it): traced at half resolution (64 x 64 of a 128 x 128 depth),
// alpha-tested card + a block under an oblique sun, Penumbra's blocker distances on. Every traced pixel's visibility and blocker
// distance must be the CPU's ray from the same start (the shader's own point: the traced uv with the full-size pixel's depth).
struct SunConstants {         // RtSunShadows.cpp PassConstants
	float size[4], forward[4], right[4], up[4], origin[4], sun[4], bias[4], range[4], alpha[4], full[4];
	float cloud[4], cloudCentre[4], cloudLight[4], camera[4];
	float slack[4], contact[4], contactGate[4];   // (2026-11-07 rtgap-tess) RtTessContact.h
};

static bool sceneSun(Ctx& c) {
	Host& h = *c.host;
	World w = cardWorld(false);
	Mesh block; block.box({ -150, -150, 0 }, { 150, 150, 400 });
	w.meshes.push_back(block);
	Object b; b.mesh = 2; b.offset = { 900, -700, 0 }; b.id = 3; w.objects.push_back(b);
	if (!uploadSlot(h, c.alphaAtlas, 37, w.slots[37])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 100, -200, 2600 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.25f, 1.25f);
	const V3 sun = normalize({ 0.8f, 0.35f, 0.5f });
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(w, cam, px, depth, normals);
	if (!h.Upload(c.depth, depth) || !h.Upload(c.normals, normals)) return false;
	const UINT T = S / 2;
	SunConstants k = {};
	put4(k.size, float(T), float(T), 1.0f / T, 1.0f / T);
	put4(k.forward, cam.f, cam.farZ); put4(k.right, cam.r * (1.0f / cam.p00), 0); put4(k.up, cam.u * (1.0f / cam.p11), 0);
	put4(k.sun, sun, 1.0f);
	put4(k.bias, 1.0f, 0.001f, 0.5f, 30000.0f);       // normal offset + per view unit, along the sun, ray length
	put4(k.range, 100000.0f, 1.0f, 255.0f, 0.0f);     // RT range, fade, cull mask, no debug view
	put4(k.alpha, 1.0f, 0.0f, 0.0f, 1.0f);            // alpha tested, frame 0, hard (one ray at the centre), Penumbra blocker out
	put4(k.full, float(S), float(S), 0.0f, 0.0f);
	put4(k.camera, cam.eye, 0.0f);
	h.AddPass(gM.shSun, { c.depth, c.normals, gM.sunVis, c.alphaAtlas, c.normals, gM.sunHit }, 0x4, &k, sizeof(k), (T + 7) / 8, (T + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> vis, hit;
	if (!h.Read(gM.sunVis, vis) || !h.Read(gM.sunHit, hit)) return false;
	h.EndFrame();
	int checked = 0, bad = 0, shadowed = 0, byCard = 0, shown = 0;
	for (UINT j = 0; j < T; j++) for (UINT i = 0; i < T; i++) {
		// the shader's point: the traced pixel's uv, the full-size pixel's (2i+1) depth and normal
		bool stable = true; float wantVis = 1, wantT = 0; int blocker = -1;
		for (int s = 0; s < 5 && stable; s++) {
			const float du = s == 1 ? -0.3f : s == 2 ? 0.3f : 0.0f, dv = s == 3 ? -0.3f : s == 4 ? 0.3f : 0.0f;
			const UINT si = 2 * i + 1, sj = 2 * j + 1;
			const V3 d = cam.ray((si + 0.5f + du) / S, (sj + 0.5f + dv) / S);
			const Hit sh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!sh.hit) { if (s == 0) { wantVis = 1; break; } stable = false; break; }
			const float viewZ = sh.t;
			const V3 pos = cam.ray((i + 0.5f) / T + du / S, (j + 0.5f) / T + dv / S) * viewZ;   // scene space (eye 0)
			float v = 1, t = 0; int who = -1;
			if (dot(sh.n, sun) > 0) {
				const V3 start = pos + sh.n * (1.0f + 0.001f * viewZ) + sun * 0.5f;
				const Hit r = trace(w, cam.eye + start, sun, 0.0f, 30000.0f, 0xFF, true);
				if (r.hit) { v = 0; t = std::max(r.t, 1.0f); who = r.object; }
			}
			if (s == 0) { wantVis = v; wantT = t; blocker = who; }
			else if (v != wantVis) stable = false;
		}
		if (!stable) continue;
		checked++;
		const float gv = vis[j * T + i], gt = hit[j * T + i];
		const bool ok = gv == wantVis && std::fabs(gt - wantT) <= 0.002f * wantT + 0.5f;
		if (wantVis == 0) { shadowed++; if (blocker == 1) byCard++; }
		if (!ok) { bad++; if (shown++ < 5) say("    pixel (%u, %u): got %.2f blocker %.2f, expected %.0f blocker %.2f\n", i, j, gv, gt, wantVis, wantT); }
	}
	const bool ok = bad == 0 && checked > int(T * T) / 2 && byCard > 40 && shadowed > byCard + 20;
	say("  sun (half resolution %ux%u, alpha card + block): %d pixels checked, %d mismatches; %d shadowed (%d by the card's checker)\n", T, T, checked, bad, shadowed, byCard);
	if (!ok) { dumpPfm("sun_vis", vis, T, T, 1); dumpPfm("sun_hit", hit, T, T, 1); }
	releaseWorld(h, ids);
	return ok;
}

// ==============================================================================================================================
// Scene: AO + sky visibility (RtAOTrace.comp, AmbientOcclusion.cpp's plain pass: no GI rays). A floor, a wall and a roof over part of the
// floor, an alpha-tested card. Both outputs are deterministic (fixed sky directions, the AO noise is per texel), so the CPU repeats the
// shader's rays exactly (its packed normal decoded the same way) and every stable texel must agree.
struct AOConstants {          // RtAmbient.h TraceConstants
	float viewToWorld[16], projection[4], origin[4], ao[4], sky[4], gi[4], sunDir[4], sunColor[4], ambient[4];
	float lampPos[8][4], lampColor[8][4];
	float gi2[4], gi3[4], prevRot[4], reuse[4];
};
static_assert(sizeof(AOConstants) == 512, "TraceConstants");
static float packAO(V3 n) {   // RtAOInput.frag OctEncode + Pack2
	n = n * (1.0f / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z)));
	float ex = n.x, ey = n.y;
	if (n.z < 0) { const float x = ex, y = ey; ex = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f); ey = (1 - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f); }
	ex = ex * 0.5f + 0.5f; ey = ey * 0.5f + 0.5f;
	const float a = std::floor(std::min(std::max(ex, 0.0f), 1.0f) * 4095.0f + 0.5f), b = std::floor(std::min(std::max(ey, 0.0f), 1.0f) * 4095.0f + 0.5f);
	return a * 4096.0f + b;
}
static V3 unpackAO(float p) {   // RtAOTrace.comp Unpack2 + OctDecode
	const float high = std::floor(p / 4096.0f);
	float ex = high / 4095.0f, ey = (p - high * 4096.0f) / 4095.0f;
	ex = ex * 2 - 1; ey = ey * 2 - 1;
	V3 n = { ex, ey, 1 - std::fabs(ex) - std::fabs(ey) };
	const float t = std::min(std::max(-n.z, 0.0f), 1.0f);
	n.x += n.x >= 0 ? -t : t; n.y += n.y >= 0 ? -t : t;
	return normalize(n);
}
static float ign(float x, float y) { return fract(52.9829189f * fract(x * 0.06711056f + y * 0.00583715f)); }
static V3 cosineDir(V3 n, float ux, float uy) {
	const V3 t = normalize(std::fabs(n.z) < 0.999f ? cross(n, { 0, 0, 1 }) : cross(n, { 1, 0, 0 }));
	const V3 b = cross(n, t);
	const float r = std::sqrt(ux), phi = 6.2831853f * uy;
	return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(1.0f - ux, 0.0f)));
}

static bool sceneAO(Ctx& c) {
	Host& h = *c.host;
	World w = cardWorld(false);
	Mesh wall; wall.quad({ -900, 600, 0 }, { 900, 600, 0 }, { 900, 600, 700 }, { -900, 600, 700 });
	Mesh roof; roof.quad({ -900, 200, 500 }, { 900, 200, 500 }, { 900, 600, 500 }, { -900, 600, 500 });
	w.meshes.push_back(wall); w.meshes.push_back(roof);
	Object o1; o1.mesh = 2; o1.id = 3; Object o2; o2.mesh = 3; o2.id = 4;
	w.objects.push_back(o1); w.objects.push_back(o2);
	w.objects[1].offset = { -600, -900, 0 };   // the card off to one side
	if (!uploadSlot(h, c.alphaAtlas, 37, w.slots[37])) return false;
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -2200, 2200 }, normalize({ 0, 0.85f, -1 }), { 0, 0, 1 }, 1.3f, 1.3f);
	// the input as RtAOInput.frag writes it: view z, the world normal packed
	std::vector<float> in(S * S * 2, 0.0f);
	std::vector<GPixel> px(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
		if (!hh.hit) continue;
		in[(j * S + i) * 2] = hh.t; in[(j * S + i) * 2 + 1] = packAO(hh.n);
		px[j * S + i].valid = true; px[j * S + i].viewZ = hh.t;
	}
	if (!h.Upload(gM.aoIn, in)) return false;
	AOConstants k = {};
	const V3 R = cam.r, U = cam.u, F = cam.f;
	const float m[16] = { R.x, R.y, R.z, 0, U.x, U.y, U.z, 0, F.x, F.y, F.z, 0, 0, 0, 0, 1 };   // columns right, up, forward (cb.M * v)
	std::memcpy(k.viewToWorld, m, sizeof(m));
	put4(k.projection, 1.0f / cam.p00, 1.0f / cam.p11, float(S), float(S));
	put4(k.origin, 0, 0, 0, 0);
	put4(k.ao, 300.0f, 8.0f, 0.0f, 0.0f);       // distance, rays, fixed noise, no input check
	put4(k.sky, 3000.0f, 16.0f, 0.0f, 0.0f);    // distance, rays
	put4(k.gi, 0, 0, 0, 0);                      // no GI rays
	put4(k.gi2, 0, 0, 0, 1);                     // Interleave 1
	put4(k.gi3, 1, 2, 0, 0);                     // GIInterleave 1, AOAlpha 2 (test), no reuse
	h.AddPass(gM.shAO, { gM.aoIn, gM.aoOut, gM.aoGi, c.alphaAtlas, gM.aoLight, gM.albedoAtlas }, 0x6, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	std::vector<float> out;
	if (!h.Read(gM.aoOut, out)) return false;
	h.EndFrame();
	// the CPU's rays, as the shader casts them
	auto firstHit = [&](V3 start, V3 d, float tMin, float tMax) { return trace(w, cam.eye + start, d, tMin, tMax, 0xFF, true); };
	int checked = 0, badSky = 0, badAO = 0, covered = 0, open = 0, shown = 0;
	double aoErr = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		if (!px[j * S + i].valid) continue;
		const float viewZ = in[(j * S + i) * 2];
		const V3 n = unpackAO(in[(j * S + i) * 2 + 1]);
		const float u = (i + 0.5f) / S, v = (j + 0.5f) / S;
		const V3 view = { (u * 2 - 1) / cam.p00 * viewZ, (1 - v * 2) / cam.p11 * viewZ, viewZ };
		const V3 p = R * view.x + U * view.y + F * view.z;
		const float bias = 0.75f + viewZ * 0.003f;
		bool stable = true;
		// AO
		const float nx = ign(float(i), float(j)), ny = ign(i + 17.0f, j + 17.0f);
		float occl = 0;
		const V3 start = p + n * bias;
		for (int r = 0; r < 8 && stable; r++) {
			const float ux = fract(nx + fract(r * 0.7548777f)), uy = fract(ny + fract(r * 0.5698403f));
			const V3 d = cosineDir(n, ux, uy);
			const Hit hh = firstHit(start, d, 0.05f * bias, 300.0f);
			if (!hh.hit) continue;
			if (hh.t < 6.0f * bias) { stable = false; break; }   // (a back face within 4 x bias counts as "inside": facing rules, not checked)
			const float x = std::min(std::max((hh.t - 150.0f) / 150.0f, 0.0f), 1.0f);
			occl += 1.0f - x * x * (3 - 2 * x);
		}
		// sky
		const V3 sstart = p + n * (bias + 8.0f);
		const float cov = firstHit(sstart, { 0, 0, 1 }, 0.0f, 3000.0f).hit ? 1.0f : 0.0f;
		float total = 0.034f * std::min(std::max(n.z, 0.0f), 1.0f), opened = total * (1 - cov);
		const float turn = ((i & 3) * 4.0f + (j & 3)) * (3.14159265f / 4.0f / 16.0f);
		for (int ring = 0; ring < 2; ring++) {
			const float cc = ring == 0 ? 0.5f : 0.8660254f, ss = ring == 0 ? 0.8660254f : 0.5f, rw = ring == 0 ? 0.0324f : 0.0884f;
			for (int q = 0; q < 8; q++) {
				const float a = turn + (q + 0.5f * ring) * (3.14159265f / 4.0f);
				const V3 d = { std::cos(a) * cc, std::sin(a) * cc, ss };
				const float wgt = rw * std::min(std::max(dot(d, n), 0.0f), 1.0f);
				if (wgt <= 0) continue;
				total += wgt;
				if (!firstHit(sstart, d, 0.0f, 3000.0f).hit) opened += wgt;
			}
		}
		const float sky = total > 1e-5f ? opened / total : 1.0f;
		if (!stable) continue;
		checked++;
		const float* o = &out[(j * S + i) * 4];
		const float ao = 1.0f - occl / 8.0f;
		aoErr += std::fabs(o[0] - ao);
		if (cov > 0.5f) covered++; else open++;
		const bool skyOk = std::fabs(o[1] - sky) < 2e-3f && o[2] == cov, aoOk = std::fabs(o[0] - ao) < 2e-3f;
		if (!skyOk) badSky++;
		if (!aoOk) badAO++;
		if ((!skyOk || !aoOk) && shown++ < 5) say("    texel (%u, %u): got ao %.3f sky %.3f covered %.0f, expected ao %.3f sky %.3f covered %.0f\n", i, j, o[0], o[1], o[2], ao, sky, cov);
	}
	// a ray grazing a triangle's edge can go either way: allow 0.3 % of texels
	const bool ok = checked > int(S * S) / 2 && badSky <= checked / 300 && badAO <= checked / 300 && covered > 300 && open > 300;
	say("  AO + sky (8 AO rays, 16 sky rays, alpha card, roof): %d texels checked, sky wrong at %d, AO wrong at %d (mean |AO diff| %.5f); %d covered, %d open\n",
		checked, badSky, badAO, checked ? aoErr / checked : 0.0, covered, open);
	if (!ok) dumpPfm("ao_out_sky", out, S, S, 4, 1);
	releaseWorld(h, ids);
	return ok;
}

// ==============================================================================================================================
// Scene: the path tracer's textures and lamps. (1) The albedo atlas (HitTextures, flag 2) on a textured floor whose record also has
// material bits: DebugView 1 (the first hit's albedo) = the atlas texel's colour, sRGB-decoded. (2) 64 white lamps through the lamp image
// (flag 1024, the game's falloff 2048): DebugView 3 (the first hit's direct light) on an open floor = albedo x the sum of every reaching
// lamp's light, exact per path for white lamps (the pick's weight cancels). (3) The same frame through RtPathTraceGrid.spv with the grid
// built by the game's RtLightGrid::Build (flag 4096): bit for bit the all-lamp loop's picture, also with blocks shadowing some lamps.
static bool scenePathTextures(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.pos = { { -1600, -1600, 0 }, { 1600, -1600, 0 }, { 1600, 1600, 0 }, { -1600, 1600, 0 } };
	floor.uv = { 0, 0, 4, 0, 4, 4, 0, 4 };   // the texture repeats 4 x 4 times
	floor.idx = { 0, 1, 2, 0, 2, 3 };
	Mesh block; block.box({ -70, -70, 0 }, { 70, 70, 260 });
	w.meshes = { floor, block };
	Object f; f.mesh = 0; f.textured = true; f.alphaSlot = 45; f.rough = 0.3f; f.metal = 1.0f; f.albedo = { 0.5f, 0.5f, 0.5f };
	w.objects = { f };
	// the slot's texture: 8 x 8 cells of colours (stored sRGB, as the game's textures)
	auto texel = [](int x, int y, int ch) { const int cx = x / 8, cy = y / 8; return ((cx * 37 + cy * 91 + ch * 53) % 200 + 40) / 255.0f; };
	{
		std::vector<float> t(64 * 64 * 4);
		for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) for (int ch = 0; ch < 4; ch++) t[(y * 64 + x) * 4 + ch] = ch == 3 ? 1.0f : texel(x, y, ch);
		if (!h.Upload(gM.albedoAtlas, t, (45 % 32) * 64, (45 / 32) * 64, 64, 64)) return false;
	}
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 2400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	auto base = [&](PathConstants& k, uint32_t flags, float view, float paths, float seed) {
		k = PathConstants();
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = seed;
		put4(k.accum, 0.0f, 3.0f, paths, float(flags));
		put4(k.sunDir, 0, 0, 1, 0); put4(k.skyZenith, 0.3f, 0.4f, 0.5f, 1.0f); put4(k.skyHorizon, 0.3f, 0.4f, 0.5f, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, view);
		k.rect[3] = 1.0f;
	};
	auto imgs = [&](uint32_t flags) {
		const int albedo = (flags & 2u) ? gM.albedoAtlas : c.ptDisplay;
		return std::vector<int>{ c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, albedo, albedo, c.depth, c.normals, c.aux16, c.pano, c.aux16, albedo,
			c.aux16, gM.ptAlbedoOut, gM.ptLamps };
	};
	bool ok = true;
	// (1) the albedo atlas
	{
		PathConstants k; base(k, 2u, 1.0f, 16.0f, 3.0f);
		h.AddPass(c.shPath, imgs(2u), 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		std::vector<float> disp;
		if (!h.Read(c.ptDisplay, disp)) return false;
		h.EndFrame();
		int checked = 0, bad = 0, shown = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			// the pixel's whole footprint must lie in one texture cell (the paths jitter inside the pixel)
			int cell = -1; bool one = true; int tx = 0, ty = 0;
			for (int s = 0; s < 4; s++) {
				const V3 d = cam.ray((i + (s & 1 ? 0.98f : 0.02f)) / S, (j + (s & 2 ? 0.98f : 0.02f)) / S);
				const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
				if (!hh.hit) { one = false; break; }
				const float u = fract(hh.u), v = fract(hh.v);
				tx = std::min(int(u * 64), 63); ty = std::min(int(v * 64), 63);
				const int cl = (ty / 8) * 8 + tx / 8;
				if (cell >= 0 && cl != cell) one = false;
				cell = cl;
			}
			if (!one) continue;
			checked++;
			bool good = true;
			for (int ch = 0; ch < 3; ch++) {
				const float stored = std::floor(texel(tx, ty, ch) * 255.0f + 0.5f) / 255.0f;
				if (std::fabs(disp[(j * S + i) * 4 + ch] - srgbToLinear(stored)) > 2e-3f) good = false;
			}
			if (!good) { bad++; if (shown++ < 5) say("    pixel (%u, %u): albedo %.4f %.4f %.4f\n", i, j, disp[(j * S + i) * 4], disp[(j * S + i) * 4 + 1], disp[(j * S + i) * 4 + 2]); }
		}
		const bool pass = bad == 0 && checked > int(S * S) / 2;
		say("  path tracer albedo atlas (slot 45, record with material bits): %d pixels checked, %d wrong -> %s\n", checked, bad, pass ? "ok" : "BAD");
		if (!pass) { ok = false; dumpPfm("pt_albedo_r", disp, S, S, 4, 0); }
	}
	// (2) + (3) the lamp image and the grid
	std::vector<V3> lampPos; std::vector<float> reach;
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { lampPos.push_back({ -1400 + x * 400.0f, -1400 + y * 400.0f, 150.0f + 10.0f * ((x + y) % 3) }); reach.push_back(520.0f + 15.0f * ((x * 3 + y) % 5)); }
	const int lamps = int(lampPos.size());
	std::vector<float> image(size_t(kLightTexels) * (3 + kGridRows) * 2, 0.0f);
	std::vector<float> scene(size_t(lamps) * 8);
	for (int i = 0; i < lamps; i++) {
		const V3 q = lampPos[i] - cam.eye;
		const float bright = 0.6f + 0.1f * (i % 4);
		const float v[8] = { q.x, q.y, q.z, reach[i], bright, bright, bright, 0.0f };
		std::memcpy(&image[size_t(i) * 8], v, sizeof(v)); std::memcpy(&scene[size_t(i) * 8], v, sizeof(v));
	}
	PathConstants gridK = {};
	RtLightGrid::Info info;
	std::vector<float> gridData(size_t(kLightTexels) * kGridRows * 2, 0.0f);
	if (!RtLightGrid::Build(scene.data(), lamps, 8, gridData.data(), int(kLightTexels * kGridRows), int(kLightTexels), gridK.grid, info)) {
		say("  the game's RtLightGrid::Build refused the grid (%d cells, %d entries)\n", info.cells, info.entries); return false;
	}
	std::memcpy(&image[size_t(kLightTexels) * 2], gridData.data(), gridData.size() * sizeof(float));
	if (!h.Upload(gM.ptLamps, image)) return false;
	const uint32_t lampFlags = 1024u | 2048u;
	std::vector<float> plain[2], gridded[2];
	for (int withBlocks = 0; withBlocks < 2; withBlocks++) {
		w.objects = { w.objects[0] };
		w.objects[0].textured = false; w.objects[0].albedo = { 0.5f, 0.5f, 0.5f };
		if (withBlocks) for (int i = 0; i < lamps; i += 5) { Object b; b.mesh = 1; b.offset = { lampPos[i].x + 120, lampPos[i].y - 60, 0 }; w.objects.push_back(b); }
		for (int g = 0; g < 2; g++) {
			PathConstants k; base(k, lampFlags | (g ? 4096u : 0u), 3.0f, 8.0f, 11.0f);
			k.misc[0] = float(lamps);
			if (g) std::memcpy(k.grid, gridK.grid, sizeof(k.grid));
			h.AddPass(g ? gM.shPathGrid : c.shPath, imgs(lampFlags), 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(w, ids, cam.eye)) return false;
			if (!h.Read(c.ptDisplay, g ? gridded[withBlocks] : plain[withBlocks])) return false;
			h.EndFrame();
		}
	}
	// (2) the open floor against the exact sum
	w.objects = { w.objects[0] };
	int checked = 0, bad = 0, shown = 0;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
		const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0x02, false);
		if (!hh.hit) continue;
		const V3 hit = d * hh.t;   // scene space
		// the sum varies inside the pixel: compare with the mean of the corners' sums, allow the spread
		double sum[5] = {}; float lo = 1e30f, hiv = -1e30f;
		for (int s = 0; s < 5; s++) {
			const V3 dd = s == 0 ? d : cam.ray((i + (s & 1 ? 0.95f : 0.05f)) / S, (j + (s & 2 ? 0.95f : 0.05f)) / S);
			const V3 at = dd * (hh.t * (dot(d, V3(0, 0, 1)) / dot(dd, V3(0, 0, 1))));
			for (int l = 0; l < lamps; l++) {
				const V3 L = (lampPos[l] - cam.eye) - at;
				const float dist = length(L);
				if (dist >= reach[l] || dist < 1e-3f) continue;
				const float x = dist / reach[l], fall = std::pow(std::max(1 - x * x, 0.0f), 2.2f);
				sum[s] += (0.6f + 0.1f * (l % 4)) * fall * std::max(L.z / dist, 0.0f);
			}
			lo = std::min(lo, float(sum[s])); hiv = std::max(hiv, float(sum[s]));
		}
		(void)hit;
		const float got = plain[0][(j * S + i) * 4];
		checked++;
		if (!(got >= 0.5f * lo * 0.995f - 1e-3f && got <= 0.5f * hiv * 1.005f + 1e-3f)) {
			bad++; if (shown++ < 5) say("    pixel (%u, %u): direct %.4f, the exact range %.4f .. %.4f\n", i, j, got, 0.5f * lo, 0.5f * hiv);
		}
	}
	int diff[2] = {};
	for (int b = 0; b < 2; b++) for (size_t i = 0; i < plain[b].size(); i++) diff[b] += plain[b][i] != gridded[b][i];
	const bool pass = bad <= checked / 500 && checked > int(S * S) / 2 && diff[0] == 0 && diff[1] == 0;
	say("  path tracer, 64 lamps through the lamp image: direct light in the exact range at %d of %d pixels; PTLightGrid (%d cells, %d entries, "
		"the game's builder) vs the all-lamp loop: %d / %d values differ (open floor / with blocks) -> %s\n", checked - bad, checked, info.cells, info.entries,
		diff[0], diff[1], pass ? "ok" : "BAD");
	if (!pass) { ok = false; dumpPfm("pt_lamps_plain", plain[1], S, S, 4, 0); dumpPfm("pt_lamps_grid", gridded[1], S, S, 4, 0); }
	releaseWorld(h, ids);
	return ok;
}

// ==============================================================================================================================
// Scene: ReSTIR GI on the path tracer's first bounce (RtPathTraceGI.spv + RtRestirGI.spv, as RtPathTrace.cpp records them with PTDenoise +
// PTRestirGI). A floor and two walls under the sun and a sky. Reference: the plain path tracer's running mean (32 frames x 16 paths).
// Checked: (a) the reuse's picture (x the demodulated albedo), averaged over frames 8-39 of a still camera, has the reference's mean
// within 3 %; (b) its single frames are closer to the reference with the history than without (the reuse works); (c) a frame right after a
// camera slide stays finite and within 5 % of the reference's mean.
static bool sceneRestirGI(Ctx& c) {
	Host& h = *c.host;
	World w;
	Mesh floor; floor.quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	Mesh wall; wall.quad({ -700, 300, 0 }, { 700, 300, 0 }, { 700, 300, 900 }, { -700, 300, 900 });
	Mesh side; side.quad({ 500, -800, 0 }, { 500, -800, 900 }, { 500, 300, 900 }, { 500, 300, 0 });
	w.meshes = { floor, wall, side };
	Object f; f.mesh = 0; f.albedo = { 0.5f, 0.45f, 0.4f };
	Object a; a.mesh = 1; a.albedo = { 0.8f, 0.3f, 0.2f };
	Object b; b.mesh = 2; b.albedo = { 0.2f, 0.6f, 0.8f };
	w.objects = { f, a, b };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	Camera cam = Camera::lookDir({ -300, -1500, 1300 }, normalize({ 0.25f, 1, -0.75f }), { 0, 0, 1 }, 1.4f, 1.4f);
	auto constants = [&](PathConstants& k, const Camera& cm, uint32_t flags, float paths, float samples, float seed) {
		k = PathConstants();
		cameraConstants(cm, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = seed;
		put4(k.accum, samples, 3.0f, paths, float(flags));
		put4(k.sunDir, normalize({ 0.5f, -0.6f, 0.62f }), 1.0f);
		put4(k.sunColor, 2.0f, 1.9f, 1.7f, std::tan(0.5f * 0.53f * 0.0174532925f));
		put4(k.skyZenith, 0.25f, 0.35f, 0.6f, 1.0f); put4(k.skyHorizon, 0.5f, 0.55f, 0.6f, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f);
		k.rect[3] = 1.0f;
	};
	const std::vector<int> ptImgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano, c.aux16,
		c.ptDisplay, c.aux16, gM.ptAlbedoOut, gM.ptLamps };
	// the reference: the plain tracer's mean
	std::vector<float> ref;
	for (int n = 0; n < 32; n++) {
		PathConstants k; constants(k, cam, 0u, 16.0f, float(n * 16), 1000.0f + n);
		h.AddPass(c.shPath, ptImgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (n == 31 ? !h.Read(c.ptDisplay, ref) : !h.Finish()) return false;
		h.EndFrame();
	}
	// the reuse, as RtPathTrace.cpp records it
	uint32_t giFrame = 0;
	bool giHistory = false;
	float prev[4][4] = {};
	auto restirFrame = [&](const Camera& cm, bool history, float seed, std::vector<float>* picture) -> bool {
		const uint32_t flags = 512u;   // demodulated (PTDenoise)
		PathConstants k; constants(k, cm, flags, 1.0f, 0.0f, seed);
		std::vector<int> i16 = ptImgs; i16.push_back(gM.giResult);
		h.AddPass(gM.shPathGI, i16, 0x7u | (1u << 13) | (1u << 15), &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		struct RestirConstants { float size[4], forward[4], right[4], up[4], origin[4], rect[4], prevForward[4], prevRight[4], prevUp[4], prevEye[4], params[4]; } r = {};
		const int cur = int(giFrame & 1u), prv = cur ^ 1;
		std::memcpy(r.size, k.size, 16); std::memcpy(r.forward, k.forward, 16); r.forward[3] = 0; std::memcpy(r.right, k.right, 16); std::memcpy(r.up, k.up, 16);
		std::memcpy(r.origin, k.origin, 16);
		r.rect[2] = float((giHistory && history ? 2u : 0u) | 4u);
		std::memcpy(r.prevForward, prev[0], 16); std::memcpy(r.prevRight, prev[1], 16); std::memcpy(r.prevUp, prev[2], 16);
		for (int a = 0; a < 3; a++) r.prevEye[a] = prev[3][a] - (cm.eye - cam.eye).x * 0;   // (set below: last eye minus this eye)
		const V3 lastEye = { prev[3][0], prev[3][1], prev[3][2] };
		put4(r.prevEye, lastEye - cm.eye, 20.0f);   // PTRestirMaxM
		put4(r.params, 4.0f, 16.0f, 0.03f, 0.9f);    // PTRestirSpatial, Radius, Depth, Normal
		h.AddPass(gM.shRestirGI, { c.ptA, c.ptB, c.ptDisplay, gM.giResult, gM.ptAlbedoOut, c.ptDisplay, gM.resA[prv], gM.resB[prv], gM.resC[prv],
			gM.resA[cur], gM.resB[cur], gM.resC[cur], gM.surf[prv], gM.surf[cur] }, (1u << 3) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 13), &r, sizeof(r), (S + 7) / 8, (S + 7) / 8);
		std::memcpy(prev[0], k.forward, 16); std::memcpy(prev[1], k.right, 16); std::memcpy(prev[2], k.up, 16);
		put4(prev[3], cm.eye, 0.0f);   // (the eye in world space: prevEye is made from it)
		giFrame++;
		giHistory = true;
		if (!h.Exchange(w, ids, cm.eye)) return false;
		if (picture) {
			std::vector<float> g, al;
			if (!h.Read(gM.giResult, g) || !h.Read(gM.ptAlbedoOut, al)) return false;
			picture->resize(g.size());
			for (size_t i = 0; i < g.size(); i++) (*picture)[i] = (i % 4) == 3 ? 1.0f : g[i] * al[i];
		}
		else if (!h.Finish()) return false;
		h.EndFrame();
		return true;
	};
	auto meanRgb = [](const std::vector<float>& v) { double s = 0; int n = 0; for (size_t i = 0; i < v.size(); i += 4) if (std::isfinite(v[i])) { s += v[i] + v[i + 1] + v[i + 2]; n++; } return n ? s / n : 0.0; };
	auto rmse = [&](const std::vector<float>& v) { double s = 0; for (size_t i = 0; i < v.size(); i += 4) for (int ch = 0; ch < 3; ch++) { const double d = v[i + ch] - ref[i + ch]; s += d * d; } return std::sqrt(s / (v.size() / 4 * 3)); };
	std::vector<float> avg(ref.size(), 0.0f), one, oneNoHist;
	int nan = 0;
	for (int n = 0; n < 40; n++) {
		std::vector<float> pic;
		if (!restirFrame(cam, true, 77.0f + n, &pic)) return false;
		for (size_t i = 0; i < pic.size(); i++) { if (!std::isfinite(pic[i])) { nan++; continue; } if (n >= 8) avg[i] += pic[i] / 32.0f; }
		if (n == 39) one = pic;
	}
	giHistory = false;
	for (int n = 0; n < 2; n++) if (!restirFrame(cam, false, 300.0f + n, n == 1 ? &oneNoHist : nullptr)) return false;
	// a camera slide with the history
	giHistory = false;
	for (int n = 0; n < 12; n++) if (!restirFrame(cam, true, 500.0f + n, nullptr)) return false;
	Camera moved = cam; moved.eye = cam.eye + V3(60, 0, 0);
	std::vector<float> afterMove;
	if (!restirFrame(moved, true, 600.0f, &afterMove)) return false;
	for (float v : afterMove) if (!std::isfinite(v)) nan++;
	const double mRef = meanRgb(ref), mAvg = meanRgb(avg), mMove = meanRgb(afterMove);
	const double eHist = rmse(one), eNone = rmse(oneNoHist);
	const bool ok = nan == 0 && std::fabs(mAvg / mRef - 1) < 0.03 && eHist < 0.85 * eNone && std::fabs(mMove / mRef - 1) < 0.05;
	say("  ReSTIR GI: mean of frames 8-39 / the reference %.4f (limit 3 %%); one frame's RMSE vs the reference %.4f with the history, %.4f without "
		"(must be < 0.85 x); right after a slide mean / reference %.4f (limit 5 %%); NaN %d\n", mAvg / mRef, eHist, eNone, mMove / mRef, nan);
	if (!ok) { dumpPfm("restirgi_avg_r", avg, S, S, 4, 0); dumpPfm("restirgi_ref_r", ref, S, S, 4, 0); }
	releaseWorld(h, ids);
	if (!h.Exchange(World(), {}, V3()) || !h.Finish()) return false;
	h.EndFrame();
	return ok;
}

// ==============================================================================================================================
// Scene: RT lamp shadows, ManyLights' one ray a pixel (RtLampTraceRestir.comp: V0, V1, R each traced in turn over a 3 x 3 pattern,
// spatial + temporal reuse, two history pairs). The cameramove world: after 60 frames the two strongest lamps' entries are exact
// (their visibility is binary: lamp discs 0), R is near the exact mean; right after the slide R stays near it (and a stale reprojection
// is worse); the lamp ids past 32 show up.
static bool sceneRestirLamps(Ctx& c) {
	Host& h = *c.host;
	std::vector<Lamp> lamps;
	World w = lampFloorWorld(lamps, 8, 5, 520, 260, 1400, { 2, 9, 17, 22, 33, 36, 39 });
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera camA = Camera::lookDir({ 0, 0, 3400 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.6f, 1.6f);
	Camera camB = camA; camB.eye = camA.eye + V3(260, 0, 0);
	std::vector<GPixel> px; std::vector<float> depthA, normalsA, depthB, normalsB;
	gbuffer(w, camA, px, depthA, normalsA); gbuffer(w, camB, px, depthB, normalsB);
	{ std::vector<float> z(size_t(S) * S * 4, 0.0f); h.Upload(gM.idsA, z); h.Upload(gM.idsB, z); h.Upload(c.histA, z); h.Upload(c.histB, z); }
	int parity = 0; uint32_t frameNo = 0;
	auto reprojectTo = [&](LampConstants& k, const Camera& prev, const Camera& cur) {
		const V3 pe = prev.eye - cur.eye, ax = prev.r * prev.p00, ay = prev.u * prev.p11, aw = prev.f;
		put4(k.reproject[0], ax.x, ay.x, 0.0f, aw.x); put4(k.reproject[1], ax.y, ay.y, 0.0f, aw.y); put4(k.reproject[2], ax.z, ay.z, 0.0f, aw.z);
		put4(k.reproject[3], -dot(ax, pe), -dot(ay, pe), 0.0f, -dot(aw, pe));
	};
	auto trace1 = [&](const Camera& cam, const Camera& prev, bool history, std::vector<float>* result) -> bool {
		const bool atA = cam.eye.x == camA.eye.x;
		if (!h.Upload(c.depth, atA ? depthA : depthB) || !h.Upload(c.normals, atA ? normalsA : normalsB) ||
			!h.Upload(c.lampTable, lampTable(lamps, cam.eye), 0, 0, 1024, 1)) return false;
		LampConstants k; lampPassConstants(k, cam, int(lamps.size()), 2);
		put4(k.many, 0.0f, 31.0f, 1.0f, history ? 1.0f : 0.0f);   // history 31 frames, spatial radius 1
		put4(k.many2, float(frameNo++), 0.05f, 0.0f, 0.0f);      // seed, depth tolerance, -, no motion image this frame
		reprojectTo(k, prev, cam);
		const std::vector<int> imgs = { c.depth, c.normals, c.lampResult, c.lampTable, parity ? c.histB : c.histA, parity ? c.histA : c.histB,
			parity ? gM.idsB : gM.idsA, parity ? gM.idsA : gM.idsB, c.normals, c.alphaAtlas };
		h.AddPass(gM.shRestirLamp, imgs, 0xA4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		parity = 1 - parity;
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (result ? !h.Read(c.lampResult, *result) : !h.Finish()) return false;
		h.EndFrame();
		return true;
	};
	// the exact R and entries (as sceneCameraMove)
	struct Ref { bool valid = false, hasRest = false, stable = true; float R = 1; int best[2] = { -1, -1 }; int lit[2] = { 0, 0 }; };
	auto reference = [&](const Camera& cam, std::vector<Ref>& out) {
		out.assign(S * S, Ref());
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			Ref& r = out[j * S + i];
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const Hit hh = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, true);
			if (!hh.hit) continue;
			r.valid = true;
			const V3 origin = cam.eye + d * hh.t + hh.n * 0.5f;
			std::vector<float> wt(lamps.size());
			int b0 = -1, b1 = -1; float w0 = 0, w1 = 0;
			for (size_t s = 0; s < lamps.size(); s++) {
				wt[s] = lampWeightCpu(lamps[s], origin, hh.n);
				if (!(wt[s] > 0)) continue;
				if (wt[s] > w1) { if (wt[s] > w0) { b1 = b0; w1 = w0; b0 = int(s); w0 = wt[s]; } else { b1 = int(s); w1 = wt[s]; } }
			}
			double sum = 0, vis = 0;
			for (size_t s = 0; s < lamps.size(); s++) {
				if (!(wt[s] > 0) || int(s) == b0 || int(s) == b1) continue;
				if (std::fabs(wt[s] - w1) < 1e-3f * w1) r.stable = false;
				sum += wt[s]; vis += wt[s] * (lampLitCpu(w, origin, lamps[s], 0.5f) ? 1.0 : 0.0);
			}
			if (b1 >= 0 && std::fabs(w0 - w1) < 1e-3f * w0) r.stable = false;
			r.hasRest = sum > 0; r.R = sum > 0 ? float(vis / sum) : 1.0f;
			r.best[0] = b0; r.best[1] = b1;
			for (int a = 0; a < 2; a++) r.lit[a] = r.best[a] >= 0 && lampLitCpu(w, origin, lamps[r.best[a]], 0.5f) ? 15 : 0;
			// a pixel on a shadow edge (its neighbours' rays reach other answers through the reuse): not checked
			for (int s = 0; s < 9 && r.stable; s++) {
				if (s == 4) continue;
				const V3 dd = cam.ray((i + 0.5f + float(s % 3 - 1)) / S, (j + 0.5f + float(s / 3 - 1)) / S);   // the spatial reuse's radius 1
				const Hit h2 = trace(w, cam.eye, dd, 1e-3f, 1e9f, 0xFF, true);
				if (!h2.hit) { r.stable = false; break; }
				const V3 o2 = cam.eye + dd * h2.t + h2.n * 0.5f;
				for (int a = 0; a < 2; a++) if (r.best[a] >= 0 && (lampLitCpu(w, o2, lamps[r.best[a]], 0.5f) ? 15 : 0) != r.lit[a]) r.stable = false;
			}
		}
	};
	std::vector<Ref> refA, refB;
	reference(camA, refA); reference(camB, refB);
	auto score = [&](const std::vector<float>& res, const std::vector<Ref>& ref, int& n, int& badEntries, int& high) -> double {
		double err = 0; n = 0; badEntries = 0;
		for (UINT p = 0; p < S * S; p++) {
			const Ref& r = ref[p];
			if (!r.valid || !r.stable) continue;
			const float x = res[p * 2], y = res[p * 2 + 1];
			const int e0 = int(x) % 4096, e1 = int(x) / 4096, restCode = int(y) % 4096;
			for (int a = 0; a < 2; a++) {
				const int want = r.best[a] >= 0 ? r.best[a] + 256 * r.lit[a] : 4095, got = a ? e1 : e0;
				if (got != want) badEntries++; else if (r.best[a] >= 32) high++;
			}
			if (!r.hasRest) continue;
			err += std::fabs(restCode / 4095.0 - r.R); n++;
		}
		return n ? err / n : 1.0;
	};
	double errA = 0, errB = 0, errStale = 0;
	int nA = 0, nB = 0, nS = 0, badA = 0, badB = 0, badS = 0, high = 0;
	for (int run = 0; run < 2; run++) {
		std::vector<float> resA, resB;
		for (int f = 0; f < 60; f++) if (!trace1(camA, camA, f > 0, f == 59 ? &resA : nullptr)) return false;
		for (int f = 0; f < 1; f++) if (!trace1(camB, run == 0 ? camA : camB, true, &resB)) return false;
		if (run == 0) { errA = score(resA, refA, nA, badA, high); errB = score(resB, refB, nB, badB, high); }
		else errStale = score(resB, refB, nS, badS, high);
	}
	// right after the slide, a pixel's V0 / V1 were traced this frame for one pixel in 3 only: the entries come from the reprojected
	// history (exact for a right reprojection) - so the entries are checked at B too
	const bool ok = nA > int(S * S) / 3 && errA < 0.05 && errB < std::max(1.5 * errA, errA + 0.015) && errStale > 1.5 * errB && badA <= nA / 200 &&
		badB <= nB / 100 && high > 300;
	say("  ManyLights one ray a pixel (ReSTIR-style reuse, 40 lamps): R off the exact mean by %.4f after 60 frames, %.4f right after the slide, %.4f with a "
		"stale reprojection; exact entries wrong %d of %d at A, %d at B (%d right ones name slots 32-39)\n", errA, errB, errStale, badA, 2 * nA, badB, high);
	releaseWorld(h, ids);
	if (!h.Exchange(World(), {}, V3()) || !h.Finish()) return false;
	h.EndFrame();
	return ok;
}

// ==============================================================================================================================
// The game's D3D9 side, 1: RtDenoise (core/RtDenoise.cpp) - its fork passes RtDenoiseTemporal.spv + RtDenoiseAtrous.spv on this device,
// with the push blocks and bindings RtDenoise::Run uses (single channel). A noisy one-sample signal (each pixel lit with probability =
// a known clean value: a floor with a darker disc, a box top) through 16 frames of a still camera, then the camera slides.
struct FragPass {
	ID3D9NvrExtPipeline* pipe = nullptr;
	UINT images = 0, push = 0;
};
static ID3D9NvrExtSampler* gPointClamp = nullptr;
static bool makeFrag(Host& h, const std::string& file, UINT images, UINT pushSize, FragPass& out) {
	FILE* f = nullptr; fopen_s(&f, file.c_str(), "rb");
	if (!f) { say("  error: no %s\n", file.c_str()); return false; }
	std::vector<char> spirv; char buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
	fclose(f);
	NVR_EXT_BINDING_DESC bindings[8] = {};
	for (UINT i = 0; i < images; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
	bindings[images] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, pushSize - 16 };
	NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
	desc.pFragmentSpirv = spirv.data(); desc.FragmentSpirvSize = UINT(spirv.size());
	desc.BindingCount = images + 1; desc.pBindings = bindings; desc.PushConstantSize = pushSize;
	desc.UsesSamplerHeap = TRUE; desc.SamplerHeapSet = 0; desc.SamplerHeapBinding = 0;
	desc.ColorTargetCount = 1; desc.Blend = NVR_EXT_BLEND_NONE; desc.ColorWriteMask = 0xF; desc.pDebugName = file.c_str();
	CHECKHR(h.ext->CreateGraphicsPipeline(&desc, &out.pipe), "CreateGraphicsPipeline");
	out.images = images; out.push = pushSize;
	if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	return true;
}
static bool drawFrag(Host& h, const FragPass& p, IDirect3DSurface9* target, IUnknown* const* images, const void* push) {
	NVR_EXT_RESOURCE_BINDING b[8] = {};
	for (UINT i = 0; i < p.images; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = images[i]; }
	b[p.images].Slot = 0; b[p.images].Type = NVR_EXT_BIND_SAMPLER; b[p.images].pResource = gPointClamp;
	NVR_EXT_DRAW_DESC d = {};
	d.pPipeline = p.pipe; d.TargetCount = 1; d.Targets[0].pResource = target;
	d.BindingCount = p.images + 1; d.pBindings = b; d.pPushData = push; d.PushDataSize = p.push;
	CHECKHR(h.ext->Draw(&d), "Draw");
	return true;
}
struct Tex { IDirect3DTexture9* t = nullptr; IDirect3DSurface9* s = nullptr; IDirect3DSurface9* sys = nullptr; D3DFORMAT f; UINT w, h; };
static bool makeTex(Host& h, UINT w, UINT hh, D3DFORMAT f, bool target, Tex& out) {
	out.f = f; out.w = w; out.h = hh;
	CHECKHR(h.dev->CreateTexture(w, hh, 1, target ? D3DUSAGE_RENDERTARGET : 0, f, target ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &out.t, nullptr), "texture");
	out.t->GetSurfaceLevel(0, &out.s);
	if (target) CHECKHR(h.dev->CreateOffscreenPlainSurface(w, hh, f, D3DPOOL_SYSTEMMEM, &out.sys, nullptr), "sys surface");
	return true;
}
static bool fillTex(Tex& t, const std::vector<float>& v, int ch) {   // a managed texture: R32F, R16F, A16B16G16R16F or A32B32G32R32F
	D3DLOCKED_RECT lr;
	CHECKHR(t.t->LockRect(0, &lr, nullptr, 0), "lock");
	for (UINT j = 0; j < t.h; j++) for (UINT i = 0; i < t.w; i++) for (int c = 0; c < ch; c++) {
		const float x = v[(size_t(j) * t.w + i) * ch + c];
		char* row = (char*)lr.pBits + j * lr.Pitch;
		if (t.f == D3DFMT_R32F || t.f == D3DFMT_A32B32G32R32F || t.f == D3DFMT_G32R32F) ((float*)row)[i * ch + c] = x;
		else ((uint16_t*)row)[i * ch + c] = toHalf(x);
	}
	t.t->UnlockRect(0);
	return true;
}
static bool readTex(Host& h, Tex& t, std::vector<float>& v, int ch) {
	CHECKHR(h.dev->GetRenderTargetData(t.s, t.sys), "GetRenderTargetData");
	D3DLOCKED_RECT lr;
	CHECKHR(t.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "lock");
	v.assign(size_t(t.w) * t.h * ch, 0.0f);
	for (UINT j = 0; j < t.h; j++) for (UINT i = 0; i < t.w; i++) for (int c = 0; c < ch; c++) {
		const char* row = (const char*)lr.pBits + j * lr.Pitch;
		v[(size_t(j) * t.w + i) * ch + c] = (t.f == D3DFMT_R32F || t.f == D3DFMT_A32B32G32R32F || t.f == D3DFMT_G32R32F) ? ((const float*)row)[i * ch + c] : fromHalf(((const uint16_t*)row)[i * ch + c]);
	}
	t.sys->UnlockRect();
	return true;
}

static bool sceneDenoise(Ctx& c) {
	Host& h = *c.host;
	FragPass temporal, atrous;
	if (!makeFrag(h, gM.compute + "\\RtDenoiseTemporal.spv", 5, 192, temporal) || !makeFrag(h, gM.compute + "\\RtDenoiseAtrous.spv", 3, 48, atrous)) return false;
	World w;
	Mesh floor; floor.quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	Mesh box; box.box({ -300, -300, 0 }, { 300, 300, 600 });
	w.meshes = { floor, box };
	Object f; f.mesh = 0; Object b; b.mesh = 1; b.offset = { 500, 300, 0 }; b.id = 2;
	w.objects = { f, b };
	auto clean = [&](V3 p, int object) -> float {
		if (object == 1) return 0.55f;                                       // the box
		const float dx = p.x + 400, dy = p.y + 300;
		return dx * dx + dy * dy < 500.0f * 500.0f ? 0.2f : 0.9f;           // the floor: a darker disc
	};
	const Camera camA = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	Camera camB = camA; camB.eye = camA.eye + V3(320, 0, 0);
	Tex noisy, depth, normals, hist[2], chain[2], out;
	if (!makeTex(h, S, S, D3DFMT_R16F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, normals) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, hist[0]) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, hist[1]) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, chain[0]) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, chain[1]) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, out)) return false;
	struct TemporalPush { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4]; uint32_t samplerIndex; float clampK; uint32_t pad[2]; };
	struct AtrousPush { float params[4], size[4]; uint32_t samplerIndex, pad[3]; };
	static_assert(sizeof(TemporalPush) == 192 && sizeof(AtrousPush) == 48, "RtDenoise.cpp's push blocks");
	// the camera-relative view-projection as RtDenoise uploads TheRenderManager->ViewProjMatrix (D3D rows as GLSL columns: clip = M * p)
	auto viewProj = [&](const Camera& cm, float* m) {
		std::memset(m, 0, 64);
		const V3 ax = cm.r * cm.p00, ay = cm.u * cm.p11, aw = cm.f;
		for (int col = 0; col < 3; col++) { m[col * 4 + 0] = (&ax.x)[col]; m[col * 4 + 1] = (&ay.x)[col]; m[col * 4 + 3] = (&aw.x)[col]; }
	};
	uint32_t frame = 0, rng = 12345;
	auto rnd = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return float(((x >> 22u) ^ x) >> 8) / 16777216.0f; };
	std::vector<float> cleanA(S * S), cleanB(S * S), depthA, normalsA, depthB, normalsB;
	std::vector<int> objA(S * S), objB(S * S);
	auto prepare = [&](const Camera& cm, std::vector<float>& cl, std::vector<int>& ob, std::vector<float>& dep, std::vector<float>& nor) {
		std::vector<GPixel> px;
		gbuffer(w, cm, px, dep, nor);
		for (UINT i = 0; i < S * S; i++) { cl[i] = px[i].valid ? clean(px[i].pos, px[i].object) : 0.0f; ob[i] = px[i].object; }
	};
	prepare(camA, cleanA, objA, depthA, normalsA); prepare(camB, cleanB, objB, depthB, normalsB);
	// RtDenoise::Run, one channel, Settings defaults (MaxHistory 16, DepthTolerance 0.03, 3 a-trous passes, DepthSigma 1, NormalPower 64, ValueSigma 4)
	auto run = [&](const Camera& cm, const Camera& prev, bool cut, std::vector<float>& result, std::vector<float>* noisyOut) -> bool {
		const bool atA = cm.eye.x == camA.eye.x;
		const std::vector<float>& cl = atA ? cleanA : cleanB;
		std::vector<float> nz(S * S);
		for (UINT i = 0; i < S * S; i++) nz[i] = rnd() < cl[i] ? 1.0f : 0.0f;
		if (noisyOut) *noisyOut = nz;
		if (!fillTex(noisy, nz, 1) || !fillTex(depth, atA ? depthA : depthB, 1) || !fillTex(normals, atA ? normalsA : normalsB, 4)) return false;
		TemporalPush t = {};
		viewProj(prev, t.prevViewProj);
		put4(t.forward, cm.f, cm.farZ); put4(t.right, cm.r * (1.0f / cm.p00), 0); put4(t.up, cm.u * (1.0f / cm.p11), 0);
		put4(t.move, cm.eye - prev.eye, cut ? 1.0f : 0.0f);
		put4(t.params, 16.0f, 0.03f, 1.0f / S, 1.0f / S);
		put4(t.rect, 1, 1, 1, 1);
		const uint32_t last = frame & 1, now = (frame + 1) & 1;
		IUnknown* ti[5] = { noisy.t, hist[last].t, depth.t, depth.t, depth.t };
		if (!drawFrag(h, temporal, hist[now].s, ti, &t)) return false;
		AtrousPush a = {};
		put4(a.params, 0, 1.0f, 64.0f, 4.0f); put4(a.size, 1.0f / S, 1.0f / S, 0, 0);
		IDirect3DTexture9* source = hist[now].t;
		for (int i = 0; i < 3; i++) {
			a.params[0] = float(1 << i);
			IUnknown* ai[3] = { source, normals.t, normals.t };
			if (!drawFrag(h, atrous, i == 2 ? out.s : chain[i & 1].s, ai, &a)) return false;
			source = chain[i & 1].t;
		}
		frame++;
		return readTex(h, out, result, 4);
	};
	// errors: over pixels whose 5 x 5 neighbourhood is one surface with one clean value (the a-trous kernel reaches 2 x 4 = 8 pixels,
	// edge-stopped: the "edge" score is over floor pixels next to the box, where bleeding would show)
	auto ring = [&](const std::vector<float>& res, const std::vector<float>& cl, const std::vector<int>& ob) {   // where a slide would ghost
		double e = 0; int n = 0;
		for (int j = 7; j < int(S) - 7; j++) for (int i = 7; i < int(S) - 7; i++) {
			const int p = j * S + i;
			if (ob[p] != 0) continue;
			bool change = false;
			for (int dy = -7; dy <= 7 && !change; dy++) for (int dx = -7; dx <= 7; dx++) { const int q = (j + dy) * S + i + dx; if (ob[q] == 0 && cl[q] != cl[p]) { change = true; break; } }
			if (change) { e += std::fabs(res[p * 4] - cl[p]); n++; }
		}
		return n ? e / n : 1.0;
	};
	auto errors = [&](const std::vector<float>& res, const std::vector<float>& cl, const std::vector<int>& ob, double& flat, double& edge) {
		double fs = 0, es = 0; int fn = 0, en = 0;
		for (int j = 2; j < int(S) - 2; j++) for (int i = 2; i < int(S) - 2; i++) {
			const int p = j * S + i;
			if (ob[p] < 0) continue;
			bool same = true, nearBox = false;
			for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
				const int q = (j + dy) * S + i + dx;
				if (ob[q] != ob[p] || cl[q] != cl[p]) same = false;
				if (ob[p] == 0 && ob[q] == 1) nearBox = true;
			}
			const double e = std::fabs(res[p * 4] - cl[p]);
			if (same) { fs += e; fn++; }
			if (nearBox && std::fabs(cl[p] - 0.9f) < 1e-6f) { es += e; en++; }
		}
		flat = fn ? fs / fn : 1; edge = en ? es / en : 1;
	};
	std::vector<float> res, noisyA;
	bool okRun = true;
	for (int f2 = 0; f2 < 16 && okRun; f2++) okRun = run(camA, camA, f2 == 0, res, f2 == 15 ? &noisyA : nullptr);
	if (!okRun) return false;
	double flatA, edgeA, noiseFlat = 0, dummy;
	errors(res, cleanA, objA, flatA, edgeA);
	{ std::vector<float> n4(S * S * 4); for (UINT i = 0; i < S * S; i++) n4[i * 4] = noisyA[i]; errors(n4, cleanA, objA, noiseFlat, dummy); }
	std::vector<float> resB, resStale;
	if (!run(camB, camA, false, resB, nullptr)) return false;
	double flatB, edgeB;
	errors(resB, cleanB, objB, flatB, edgeB);
	// stale: the same slide, but last frame's camera given as this one (no move)
	frame = 0;
	for (int f2 = 0; f2 < 16 && okRun; f2++) okRun = run(camA, camA, f2 == 0, res, nullptr);
	if (!okRun || !run(camB, camB, false, resStale, nullptr)) return false;
	double flatS, edgeS;
	errors(resStale, cleanB, objB, flatS, edgeS);
	const double ringB = ring(resB, cleanB, objB), ringS = ring(resStale, cleanB, objB);
	const bool ok = flatA < 0.3 * noiseFlat && edgeA < 0.12 && flatB < 1.6 * flatA + 0.01 && ringS > 1.5 * ringB;
	say("  RtDenoise (temporal + 3 a-trous, one channel): mean |error| on flat areas %.4f after 16 frames (the noisy signal's %.4f; limit 0.3 x), "
		"%.4f next to the box (bleeding, limit 0.12); right after a slide %.4f on flat areas, %.4f round the disc's edge, with a stale camera %.4f there "
		"(must be > 1.5 x)\n", flatA, noiseFlat, edgeA, flatB, ringB, ringS);
	if (!ok) { dumpPfm("denoise_a", res, S, S, 4, 0); dumpPfm("denoise_b", resB, S, S, 4, 0); }
	for (Tex* t : { &noisy, &depth, &normals, &hist[0], &hist[1], &chain[0], &chain[1], &out }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	temporal.pipe->Release(); atrous.pipe->Release();
	return ok;
}

// ==============================================================================================================================
// The game's D3D9 side, 2: the lamp shaders' lookup of a traced result (PointShadowForward.hlsl PointShadowTraced, compiled here as the
// game compiles it: ps_3_0, POINT_SHADOW_RT_ONLY's registers). Frame A traces 48 lamps (RtLampTrace) or ManyLights (RtLampTraceMany);
// the next frame's camera B (slid sideways) looks each pixel's lamps up through NVR_RtLampReproject. Checked per pixel of B:
//   - its strongest lamp (the CPU's): where the lookup answers (>= 0), the answer is the CPU's visibility at B's point; most pixels answer;
//   - where it says "not traced here" (-4) the lamp really is not among A's entries there; off A's screen (-1) really is off it;
//   - ManyLights: a lamp outside the two entries gets R (as RtLampTraceMany wrote it).
struct ShaderInclude : public ID3DInclude {
	std::string dir;
	HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
		FILE* f = nullptr; fopen_s(&f, (dir + "\\" + name).c_str(), "rb");
		if (!f) return E_FAIL;
		std::vector<char>* v = new std::vector<char>(); char buf[65536]; size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v->insert(v->end(), buf, buf + n);
		fclose(f);
		*data = v->data(); *bytes = UINT(v->size()); held.push_back(v);
		return S_OK;
	}
	HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
	std::vector<std::vector<char>*> held;
};
static bool compileHlsl(Host& h, const char* src, const char* profile, ID3DBlob** code) {
	ShaderInclude inc; inc.dir = gM.includes;
	ID3DBlob* errors = nullptr;
	const HRESULT hr = D3DCompile(src, std::strlen(src), "rt_host_lookup.hlsl", nullptr, &inc, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, &errors);
	for (auto* v : inc.held) delete v;
	if (FAILED(hr)) { say("  error: D3DCompile %s: %s\n", profile, errors ? (const char*)errors->GetBufferPointer() : "?"); if (errors) errors->Release(); return false; }
	if (errors) errors->Release();
	(void)h;
	return true;
}

static bool sceneLampLookup(Ctx& c) {
	Host& h = *c.host;
	static const char* ps =
		"#define INTERIOR_SHADOWS 1\n#define POINT_SHADOW_RT_ONLY 1\n#include \"PointShadowForward.hlsl\"\n"
		"sampler2D HostPos : register(s0);\nsampler2D HostSlot : register(s1);\nfloat4 HostSize : register(c0);\n"
		"float4 main(float2 vpos : VPOS) : COLOR0 {\n  float2 uv = (vpos + 0.5) * HostSize.zw;\n  float4 p = tex2D(HostPos, uv);\n"
		"  float slot = tex2D(HostSlot, uv).r;\n  return p.w > 0.5 ? PointShadowTraced(slot, p.xyz) : -9.0;\n}\n";
	static const char* vs = "float4 main(float4 p : POSITION) : POSITION { return p; }\n";
	ID3DBlob *psCode = nullptr, *vsCode = nullptr;
	if (!compileHlsl(h, ps, "ps_3_0", &psCode) || !compileHlsl(h, vs, "vs_3_0", &vsCode)) return false;
	IDirect3DPixelShader9* psObj = nullptr; IDirect3DVertexShader9* vsObj = nullptr;
	CHECKHR(h.dev->CreatePixelShader((const DWORD*)psCode->GetBufferPointer(), &psObj), "CreatePixelShader");
	CHECKHR(h.dev->CreateVertexShader((const DWORD*)vsCode->GetBufferPointer(), &vsObj), "CreateVertexShader");
	Tex pos, slots, out;
	if (!makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, pos) || !makeTex(h, S, S, D3DFMT_R32F, false, slots) || !makeTex(h, S, S, D3DFMT_R32F, true, out)) return false;
	bool ok = true;
	for (int many = 0; many < 2; many++) {
		std::vector<Lamp> lamps;
		World w = many ? lampFloorWorld(lamps, 8, 5, 520, 260, 1400, { 2, 9, 17, 22, 33, 36, 39 }) : lampFloorWorld(lamps, 8, 6, 800, 220, 900, { 3, 33, 40, 47 });
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const Camera camA = Camera::lookDir({ 0, 0, many ? 3400.0f : 5000.0f }, { 0, 0, -1 }, { 0, 1, 0 }, many ? 1.6f : 1.45f, many ? 1.6f : 1.45f);
		Camera camB = camA; camB.eye = camA.eye + V3(many ? 240.0f : 420.0f, 130.0f, 0.0f);
		// frame A's trace
		std::vector<GPixel> pxA; std::vector<float> depthA, normalsA;
		gbuffer(w, camA, pxA, depthA, normalsA);
		if (!h.Upload(c.depth, depthA) || !h.Upload(c.normals, normalsA) || !h.Upload(c.lampTable, lampTable(lamps, camA.eye), 0, 0, 1024, 1)) return false;
		LampConstants k; lampPassConstants(k, camA, int(lamps.size()), 3);
		if (many) {
			std::vector<float> z(size_t(S) * S * 4, 0.0f); h.Upload(c.histA, z); h.Upload(c.histB, z);
			put4(k.many, 4.0f, 64.0f, 0.0f, 0.0f); put4(k.many2, 0.0f, 0.05f, 1.0f, 0.0f);
		}
		// (many: 24 frames so R settles; the history pair ping-pongs)
		const int frames = many ? 24 : 1;
		for (int fr = 0; fr < frames; fr++) {
			if (many) { k.many[3] = fr > 0 ? 1.0f : 0.0f; k.many2[0] = float(fr); }
			for (int r = 0; r < 4; r++) std::memset(k.reproject[r], 0, 16);
			{ const V3 ax = camA.r * camA.p00, ay = camA.u * camA.p11, aw = camA.f;
			  put4(k.reproject[0], ax.x, ay.x, 0, aw.x); put4(k.reproject[1], ax.y, ay.y, 0, aw.y); put4(k.reproject[2], ax.z, ay.z, 0, aw.z); }
			const std::vector<int> imgs = many ? std::vector<int>{ c.depth, c.normals, c.lampResult, c.lampTable, (fr & 1) ? c.histB : c.histA, (fr & 1) ? c.histA : c.histB, c.depth, c.depth, c.alphaAtlas }
				: std::vector<int>{ c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaAtlas };
			h.AddPass(many ? c.shMany : c.shLamp, imgs, many ? 0x24 : 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(w, ids, camA.eye)) return false;
			if (fr + 1 < frames) { if (!h.Finish()) return false; h.EndFrame(); }
		}
		std::vector<float> traced;
		if (!h.Read(c.lampResult, traced)) return false;   // (also leaves it in the Image's D3D9 texture: NVR_RtLampVis)
		h.EndFrame();
		// frame B: each pixel's point (camera-B relative) and the lamp to look up
		std::vector<float> posB(S * S * 4, 0.0f), slotB(S * S, -1.0f);
		struct Want { bool valid = false, stable = true; int lamp = -1; float lit = 0; bool inA = false; V3 world; };
		std::vector<Want> want(S * S);
		for (int pass = 0; pass < 2; pass++) {   // 0: the strongest lamp, 1 (ManyLights): a lamp outside the top two
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				Want& wt = want[j * S + i]; wt = Want();
				const V3 d = camB.ray((i + 0.5f) / S, (j + 0.5f) / S);
				const Hit hh = trace(w, camB.eye, d, 1e-3f, 1e9f, 0xFF, true);
				if (!hh.hit) continue;
				wt.valid = true; wt.world = camB.eye + d * hh.t;
				const V3 origin = wt.world + hh.n * 0.5f;
				std::vector<std::pair<float, int>> ranked;
				for (size_t s = 0; s < lamps.size(); s++) { const float v = lampWeightCpu(lamps[s], origin, hh.n); if (v > 0) ranked.push_back({ v, int(s) }); }
				std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
				if (ranked.empty()) { wt.valid = false; continue; }
				const int pick = pass == 0 ? 0 : 4;
				if (int(ranked.size()) <= pick) { wt.valid = false; continue; }
				wt.lamp = ranked[pick].second;
				for (size_t q = 0; q + 1 < ranked.size() && q < 4; q++) if (ranked[q].first - ranked[q + 1].first < 2e-3f * ranked[q].first) wt.stable = false;
				wt.lit = lampLitCpu(w, origin, lamps[wt.lamp], 0.5f) ? 1.0f : 0.0f;
				// the same answer a little around (the lookup reads A's texel nearest the reprojected point)
				for (int s = 1; s < 5 && wt.stable; s++) {
					const V3 dd = camB.ray((i + 0.5f + (s == 1 ? -0.8f : s == 2 ? 0.8f : 0)) / S, (j + 0.5f + (s == 3 ? -0.8f : s == 4 ? 0.8f : 0)) / S);
					const Hit h2 = trace(w, camB.eye, dd, 1e-3f, 1e9f, 0xFF, true);
					if (!h2.hit || h2.object != hh.object || (lampLitCpu(w, camB.eye + dd * h2.t + h2.n * 0.5f, lamps[wt.lamp], 0.5f) ? 1.0f : 0.0f) != wt.lit) wt.stable = false;
				}
				const V3 p = wt.world - camB.eye;
				posB[(j * S + i) * 4] = p.x; posB[(j * S + i) * 4 + 1] = p.y; posB[(j * S + i) * 4 + 2] = p.z; posB[(j * S + i) * 4 + 3] = 1.0f;
				slotB[j * S + i] = float(wt.lamp);
			}
			if (pass == 1 && !many) break;
			if (!fillTex(pos, posB, 4) || !fillTex(slots, slotB, 1)) return false;
			// (2026-11-07) twice: as before (c215.w 0: no edge search), then with the edge search (c215.w = 1 / the result's width, as
			// RtLampShadows sets it): silhouette pixels whose own texel holds the other side of the edge take a neighbour with their depth.
			// It must answer more pixels, leave fewer "another depth" ones, and never answer wrong.
			int edgesWithout = -1;
			for (int search = 0; search < 2; search++) {
			// the lookup's constants, as PointShadowForward::Bind sets them (RT_ONLY: the reprojection at c200)
			const V3 pe = camA.eye - camB.eye, ax = camA.r * camA.p00, ay = camA.u * camA.p11, aw = camA.f;
			const float reproject[16] = { ax.x, ay.x, 0, aw.x, ax.y, ay.y, 0, aw.y, ax.z, ay.z, 0, aw.z, -dot(ax, pe), -dot(ay, pe), 0, -dot(aw, pe) };
			const float rt[4] = { 1.0f, 0.02f, std::log2(camA.farZ) / 4095.0f, search ? 1.0f / S : 0.0f };
			const float slotsC[8] = { -1, -1, -1, -1, -1, -1, -1, many ? 1.0f / 4096.0f : 1.0f / 1024.0f };
			const float size[4] = { float(S), float(S), 1.0f / S, 1.0f / S };
			h.dev->SetVertexShader(vsObj); h.dev->SetPixelShader(psObj);
			h.dev->SetPixelShaderConstantF(200, reproject, 4); h.dev->SetPixelShaderConstantF(215, rt, 1); h.dev->SetPixelShaderConstantF(216, slotsC, 2);
			h.dev->SetPixelShaderConstantF(0, size, 1);
			h.dev->SetTexture(0, pos.t); h.dev->SetTexture(1, slots.t); h.dev->SetTexture(15, h.images[c.lampResult].readTex);
			for (DWORD s : { 0u, 1u, 15u }) {
				h.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); h.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				h.dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				h.dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); h.dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			}
			h.dev->SetRenderState(D3DRS_ZENABLE, FALSE); h.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			IDirect3DSurface9* back = nullptr; h.dev->GetRenderTarget(0, &back);
			h.dev->SetRenderTarget(0, out.s);
			D3DVIEWPORT9 vp = { 0, 0, S, S, 0.0f, 1.0f }; h.dev->SetViewport(&vp);
			const float quad[4][4] = { { -1, -1, 0.5f, 1 }, { -1, 1, 0.5f, 1 }, { 1, -1, 0.5f, 1 }, { 1, 1, 0.5f, 1 } };
			h.dev->SetFVF(D3DFVF_XYZW);
			h.dev->BeginScene();
			h.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 16);
			h.dev->EndScene();
			h.dev->SetRenderTarget(0, back); if (back) back->Release();
			h.dev->SetTexture(0, nullptr); h.dev->SetTexture(1, nullptr); h.dev->SetTexture(15, nullptr);
			h.dev->SetPixelShader(nullptr); h.dev->SetVertexShader(nullptr);
			std::vector<float> got;
			if (!readTex(h, out, got, 1)) return false;
			// the CPU: where does B's point land in A, and what did A's trace hold for that lamp there
			int answered = 0, checked = 0, bad = 0, notTraced = 0, off = 0, shown = 0, restAnswers = 0, edges = 0;
			for (UINT p = 0; p < S * S; p++) {
				const Want& wt = want[p];
				if (!wt.valid || !wt.stable) continue;
				const float g = got[p];
				// the point in A
				const V3 q = wt.world - camA.eye;
				const float vz = dot(q, camA.f);
				const float ua = dot(q, camA.r) * camA.p00 / vz * 0.5f + 0.5f, va = 0.5f - dot(q, camA.u) * camA.p11 / vz * 0.5f;
				const bool onA = vz > 1 && ua >= 0 && ua <= 1 && va >= 0 && va <= 1;
				checked++;
				bool ok1 = true;
				if (!onA) { off++; ok1 = g == -1.0f || (g >= 0.0f && g == wt.lit); }
				else if (g >= 0.0f) {
					answered++;
					if (pass == 1) { restAnswers++; ok1 = g >= 0.0f && g <= 1.0f; }   // R (checked against the exact R in cameramove)
					else ok1 = std::fabs(g - wt.lit) < 1e-4f;
				} else if (g < -3.5f && g > -4.5f) {
					notTraced++;   // the lamp is not among the texel's entries: then A's CPU top three there must not hold it either (near ties skipped)
					const UINT ai = std::min(UINT(ua * S), S - 1), aj = std::min(UINT(va * S), S - 1);
					const LampExpect e = expectLamps(w, lamps, camA, ai, aj, many ? 2 : 3, 0.5f);
					if (e.stable) for (int a = 0; a < (many ? 2 : 3); a++) if (e.slot[a] == wt.lamp) ok1 = false;
				} else if (g <= -2.0f && g >= -3.0f) {
					ok1 = true;   // another depth there in A (an edge the B point is hidden behind in A): allowed, counted below
					edges++;
				} else ok1 = false;
				if (!ok1) { bad++; if (shown++ < 5) say("    %s pixel %u: lookup %.4f for lamp %d, expected lit %.0f (in A at %.3f %.3f)\n", many ? "ManyLights" : "48 lamps", p, g, wt.lamp, wt.lit, ua, va); }
			}
			const bool fewerEdges = !search || edgesWithout < 0 || edges <= edgesWithout;
			const bool pass1 = bad == 0 && fewerEdges && (pass == 1 ? restAnswers > checked / 2 : answered > checked / 2);
			say("  lamp lookup (PointShadowTraced, %s, %s, %s): %d pixels of B checked, %d answered from A's trace, %d not traced there, %d off A's screen, %d another depth, %d wrong -> %s\n",
				many ? "ManyLights" : "RtLampTrace 48 lamps", pass == 0 ? "each pixel's strongest lamp" : "a lamp outside the top two (R)",
				search ? "edge search on" : "no edge search", checked, answered, notTraced, off, edges, bad, pass1 ? "ok" : "BAD");
			if (!pass1) { ok = false; dumpPfm(many ? "lookup_many" : "lookup_lamps", got, S, S, 1); }
			if (!search) edgesWithout = edges;
			}
		}
		releaseWorld(h, ids);
	}
	for (Tex* t : { &pos, &slots, &out }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	psObj->Release(); vsObj->Release(); psCode->Release(); vsCode->Release();
	return ok;
}

// ==============================================================================================================================
// Scene: the scene picking rules (RtScenePick.h, what RtCore::AddGeometry runs on every geometry): each rule's case, and every record
// field packed without touching another (all 1024 atlas slots x alpha refs x material rules), read back the way the shaders read it.
static bool sceneScenePick(Ctx&) {
	using namespace RtScenePick;
	int bad = 0, cases = 0;
	auto expectFate = [&](const char* what, const RtScenePick::Geometry& g, const Switches& s, Fate want, bool wantAlpha) {
		bool a = false; const Fate f = Classify(g, s, a); cases++;
		if (f != want || a != wantAlpha) { bad++; say("    %s: fate %d alpha %d, expected %d alpha %d\n", what, int(f), int(a), int(want), int(wantAlpha)); }
	};
	Switches on; on.alphaTested = true; on.cullAngle = 0.01f; on.leafCards = true;
	Switches off; off.alphaTested = false;
	RtScenePick::Geometry g;
	expectFate("opaque object", g, on, Taken, false);
	{ RtScenePick::Geometry t = g; t.land = true; expectFate("land", t, on, Taken, false); }
	{ RtScenePick::Geometry t = g; t.hasBound = true; t.distance = 10000; t.radius = 50; expectFate("small far object (CullAngle)", t, on, Culled, false); }
	{ RtScenePick::Geometry t = g; t.hasBound = true; t.distance = 10000; t.radius = 150; expectFate("big far object", t, on, Taken, false); }
	{ RtScenePick::Geometry t = g; t.land = true; t.hasBound = true; t.distance = 10000; t.radius = 50; expectFate("small land (never culled)", t, on, Taken, false); }
	{ RtScenePick::Geometry t = g; t.hasBound = true; t.distance = 40; t.radius = 50; expectFate("camera inside the bound", t, on, Taken, false); }
	{ RtScenePick::Geometry t = g; t.hasShaderAndData = false; expectFate("no buffer data", t, on, NoShader, false); }
	{ RtScenePick::Geometry t = g; t.skinned = true; expectFate("skinned", t, on, Skinned, false); }
	{ RtScenePick::Geometry t = g; t.speedTreeLeaf = true; t.lightingProperty = false; expectFate("leaf, LeafCards on", t, on, Leaves, false); }
	{ RtScenePick::Geometry t = g; t.speedTreeLeaf = true; t.lightingProperty = false; expectFate("leaf, LeafCards off", t, off, NotLighting, false); }
	{ RtScenePick::Geometry t = g; t.speedTreeLeaf = true; t.land = true; t.lightingProperty = false; expectFate("leaf as land", t, on, NotLighting, false); }
	{ RtScenePick::Geometry t = g; t.hasShaderProperty = false; expectFate("no shader property", t, on, NotLighting, false); }
	{ RtScenePick::Geometry t = g; t.lightingProperty = false; expectFate("not lighting", t, on, NotLighting, false); }
	// (2026-11-07 rtgap-decals) refraction stays out; decals go in alpha-tested with Decals + AlphaTested on, whatever their alpha property
	{ RtScenePick::Geometry t = g; t.refraction = true; expectFate("refraction", t, on, DecalOrRefraction, false); }
	{ Switches d = on; d.decals = true;
	  { RtScenePick::Geometry t = g; t.refraction = true; t.decal = true; expectFate("refraction + decal", t, d, DecalOrRefraction, false); }
	  { RtScenePick::Geometry t = g; t.decal = true; expectFate("decal, Decals off", t, on, DecalOrRefraction, false); }
	  { RtScenePick::Geometry t = g; t.decal = true; Switches o = off; o.decals = true; expectFate("decal, AlphaTested off", t, o, DecalOrRefraction, false); }
	  { RtScenePick::Geometry t = g; t.decal = true; expectFate("decal (no alpha property)", t, d, Taken, true); }
	  { RtScenePick::Geometry t = g; t.decal = true; t.hasAlphaProperty = true; t.alphaFlags = kAlphaBlend; expectFate("decal, blended only", t, d, Taken, true); }
	  { RtScenePick::Geometry t = g; t.decal = true; t.hasAlphaProperty = true; t.alphaFlags = kAlphaTest; expectFate("decal, alpha-tested", t, d, Taken, true); }
	  { RtScenePick::Geometry t = g; t.decal = true; t.lightingProperty = false; expectFate("decal, not lighting", t, d, NotLighting, false); }
	  { RtScenePick::Geometry t = g; t.decal = true; t.skinned = true; expectFate("decal, skinned", t, d, Skinned, false); }
	  { RtScenePick::Geometry t = g; t.hasAlphaProperty = true; t.alphaFlags = kAlphaBlend; expectFate("blended glass, Decals on", t, d, BlendedOnly, false); } }
	{ RtScenePick::Geometry t = g; t.hasAlphaProperty = true; t.alphaFlags = kAlphaBlend; expectFate("blended glass", t, on, BlendedOnly, false); }
	{ RtScenePick::Geometry t = g; t.hasAlphaProperty = true; t.alphaFlags = kAlphaTest; expectFate("cut-out, AlphaTested on", t, on, Taken, true); }
	{ RtScenePick::Geometry t = g; t.hasAlphaProperty = true; t.alphaFlags = kAlphaTest | kAlphaBlend; expectFate("cut-out + blend", t, on, Taken, true); }
	{ RtScenePick::Geometry t = g; t.hasAlphaProperty = true; t.alphaFlags = kAlphaTest; expectFate("cut-out, AlphaTested off", t, off, AlphaOff, false); }
	{ RtScenePick::Geometry t = g; t.hasAlphaProperty = true; t.alphaFlags = 0x00EC; expectFate("alpha property, neither bit", t, on, Taken, false); }
	{ RtScenePick::Geometry t = g; t.farLod = true; t.hasAlphaProperty = true; t.alphaFlags = kAlphaBlend; expectFate("far LOD fade (blend only)", t, on, Taken, false); }
	{ RtScenePick::Geometry t = g; t.farLod = true; t.hasAlphaProperty = true; t.alphaFlags = kAlphaTest | kAlphaBlend; expectFate("far tree LOD (cut out)", t, on, Taken, true); }
	// masks and op flags
	auto expectU = [&](const char* what, uint32_t got, uint32_t want) { cases++; if (got != want) { bad++; say("    %s: 0x%08x, expected 0x%08x\n", what, got, want); } };
	{ Record r; expectU("object mask", InstanceMask(r), 0x02); r.land = true; expectU("land mask", InstanceMask(r), 0x01);
	  r.hookMask = 0x0C; expectU("hook mask", InstanceMask(r), 0x0C); r.hookMask = 0x1FF; expectU("hook mask low 8 bits", InstanceMask(r), 0xFF);
	  r.alphaTested = true; expectU("alpha-tested over the hook", InstanceMask(r), NVR_RT_MASK_ALPHA);
	  Record p; p.player = true; expectU("player", InstanceMask(p), NVR_RT_MASK_PLAYER); p.farLod = true; expectU("player before far", InstanceMask(p), NVR_RT_MASK_PLAYER);
	  Record fr; fr.farLod = true; expectU("far LOD", InstanceMask(fr), NVR_RT_MASK_FAR); fr.alphaTested = true; expectU("far cut-out", InstanceMask(fr), NVR_RT_MASK_ALPHA); }
	{ Record r; expectU("opaque op", OpFlags(r), 0); r.textured = true; expectU("textured op", OpFlags(r), NVR_RT_OP_FLAG_TEXCOORDS);
	  r.alphaTested = true; expectU("alpha op", OpFlags(r), NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA); r.compact = true;
	  expectU("compact alpha op", OpFlags(r), NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA | NVR_RT_OP_FLAG_COMPACT); }
	{ Record r; r.albedoKnown = true; r.glowMap = true; r.land = true; expectU("plain record", RecordFlags(r), NVR_RT_RECORD_LAND | NVR_RT_RECORD_ALBEDO_KNOWN | NVR_RT_RECORD_GLOW);
	  r.materialRule = true; r.rough = 1; r.metal = 1; r.normalMap = true; expectU("rule + normal ignored without texcoords", RecordFlags(r), NVR_RT_RECORD_LAND | NVR_RT_RECORD_ALBEDO_KNOWN | NVR_RT_RECORD_GLOW); }
	// (2026-11-07 rtgap-decals) a decal's record, mask, custom index, alpha reference, normal and lift
	{ Record r; r.decal = true; r.alphaTested = true; r.slot = 7; r.alphaRef = 128; r.hookMask = 0x0C;
	  expectU("decal mask (alpha, over the hook)", InstanceMask(r), NVR_RT_MASK_ALPHA);
	  expectU("decal custom index", CustomIndex(r), NVR_RT_CUSTOM_DECAL);
	  expectU("decal record (alpha-tested, ref 128, slot 7)", RecordFlags(r), NVR_RT_RECORD_ALPHA_TESTED | (128u << 8) | (7u << 16));
	  expectU("decal op", OpFlags(r), NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA);
	  Record l; l.land = true; l.hidden = true; expectU("land + hidden custom index", CustomIndex(l), NVR_RT_CUSTOM_LAND | NVR_RT_CUSTOM_HIDDEN);
	  expectU("custom index bits distinct", NVR_RT_CUSTOM_DECAL & (NVR_RT_CUSTOM_LAND | NVR_RT_CUSTOM_HIDDEN), 0u);
	  expectU("decal ref: alpha test", DecalAlphaRef(true, kAlphaTest | kAlphaBlend, 0x1A0), 0xA0);
	  expectU("decal ref: blended only", DecalAlphaRef(true, kAlphaBlend, 77), 128);
	  expectU("decal ref: alpha property, neither bit", DecalAlphaRef(true, 0x00EC, 77), 0);
	  expectU("decal ref: no alpha property", DecalAlphaRef(false, 0, 77), 0); }
	{ const float up[12] = { 0, 0, 1, 0, 0.2f, 0.98f, 0, -0.2f, 0.98f, 0, 0, 1 };
	  float n[3] = {};
	  cases++; if (!AverageNormal(up, 4, n) || std::fabs(n[2] - 1.0f) > 1e-3f || std::fabs(n[0]) > 1e-6f || std::fabs(n[1]) > 1e-6f) { bad++; say("    average normal: %f %f %f\n", n[0], n[1], n[2]); }
	  const float cancel[6] = { 0, 0, 1, 0, 0, -1 };
	  cases++; if (AverageNormal(cancel, 2, n)) { bad++; say("    average normal of a cancelling pair: should be none\n"); }
	  cases++; if (AverageNormal(nullptr, 0, n)) { bad++; say("    average normal of nothing: should be none\n"); }
	  // a decal model lying in its own xy plane (normal +z), turned 90 degrees about x (+z -> -y) and scaled 3, at (10, 20, 30): lifted 0.5
	  // along -y exactly (the scale does not lengthen the lift)
	  float t[3][4] = { { 3, 0, 0, 10 }, { 0, 0, -3, 20 }, { 0, 3, 0, 30 } };
	  const float mz[3] = { 0, 0, 1 };
	  cases++;
	  if (!DecalLift(t, mz, kDecalLift) || std::fabs(t[0][3] - 10.0f) > 1e-5f || std::fabs(t[1][3] - 19.5f) > 1e-5f || std::fabs(t[2][3] - 30.0f) > 1e-5f ||
	      t[0][0] != 3 || t[1][2] != -3 || t[2][1] != 3) { bad++; say("    decal lift: translation %f %f %f\n", t[0][3], t[1][3], t[2][3]); }
	  float z[3][4] = {}; const float zn[3] = { 0, 0, 1 };
	  cases++; if (DecalLift(z, zn, kDecalLift)) { bad++; say("    decal lift with a zero transform: should refuse\n"); } }
	// every slot x refs x rules: each field reads back as the shaders read it, nothing spills
	int packBad = 0, packed = 0;
	for (int slot = 0; slot < 1024; slot++) for (int kind = 0; kind < 2; kind++) for (int ref = 0; ref < 256; ref += kind ? 255 : 51) for (int rule = 0; rule < 9; rule++) {
		Record r; r.slot = slot; r.alphaTested = kind == 1; r.textured = kind == 0; r.alphaRef = uint32_t(ref); r.normalMap = (slot & 1) != 0;
		r.albedoKnown = (slot & 2) != 0; r.glowMap = (slot & 4) != 0;
		r.materialRule = rule > 0; r.rough = rule == 0 ? -1.0f : (rule - 1) / 7.0f; r.metal = rule == 0 ? -1.0f : (8 - rule) / 7.0f;
		if (rule == 8) r.rough = -1.0f;
		const uint32_t f = RecordFlags(r);
		packed++;
		const uint32_t rk = r.rough >= 0 ? uint32_t(std::min(std::max(int(std::sqrt(r.rough) * 7 + 0.5f), 1), 7)) : 0u;   // (2026-11-07 perf-reflfix2: (k / 7)^2)
		const uint32_t mk = r.metal >= 0 ? uint32_t(std::min(int(r.metal * 6 + 0.5f) + 1, 7)) : 0u;
		const bool good = ((f >> 16) & 0x3FFu) == uint32_t(slot) &&                                   // RtAlpha.glsl / RtHit.glsl
			((f & NVR_RT_RECORD_ALPHA_TESTED) != 0) == (kind == 1) && ((f & NVR_RT_RECORD_TEXTURED) != 0) == (kind == 0) &&
			(kind == 0 || ((f >> 8) & 0xFFu) == uint32_t(ref)) && (kind == 1 || ((f >> 8) & 0xFFu) == 0u) &&
			((f >> 26) & 7u) == rk && ((f >> 29) & 7u) == mk &&                                          // RtPathTrace.comp HitSurface
			((f & NVR_RT_RECORD_NORMAL) != 0) == r.normalMap && ((f & NVR_RT_RECORD_ALBEDO_KNOWN) != 0) == r.albedoKnown &&
			((f & NVR_RT_RECORD_GLOW) != 0) == r.glowMap && (f & NVR_RT_RECORD_LAND) == 0 && (f & NVR_RT_RECORD_WATER) == 0 && (f & NVR_RT_RECORD_METAL) == 0;
		if (!good && packBad++ < 5) say("    slot %d kind %d ref %d rule %d -> flags 0x%08x\n", slot, kind, ref, rule, f);
	}
	// what an unmasked reader (the 11-06 bug) would have read: the test above would catch it
	Record worst; worst.textured = true; worst.slot = 1023; worst.materialRule = true; worst.rough = 1; worst.metal = 1;
	const uint32_t wf = RecordFlags(worst);
	const bool ok = bad == 0 && packBad == 0 && (wf >> 16) != 1023u && ((wf >> 16) & 0x3FFu) == 1023u;
	say("  scene picking: %d rule / mask / op cases, %d wrong; %d record packings (1024 slots x refs x rules), %d wrong; slot 1023 + rough 7 + metal 7 = 0x%08x "
		"(flags >> 16 unmasked would read %u)\n", cases, bad, packed, packBad, wf, wf >> 16);
	return ok;
}
