// (2026-11-07 ptaaa-denoiser) The path tracer's denoiser toward NRD ReLAX / DLSS RR (docs/next/RT-PATH-TRACING.md "ptaaa-denoiser"):
//  `ptsplit`       RtPathTrace.comp PTDenoiseSplit (flag 4194304): the diffuse and the specular signal, each divided by its own albedo, put
//                  back together (display x colour + specular x its albedo) equal the combined picture of the same paths (same seeds),
//                  pixel for pixel; the specular share per material; PTSpecular off = no specular; the specular hit distance on a
//                  mirror floor = the CPU's reflected ray.
//  `ptspecmove`    RtDenoiseTemporalSpec.frag: a camera sliding past a mirror floor that reflects a striped wall (analytic, clean and
//                  noisy): the reflection reprojected by its hit distance (the virtual point) vs by the surface only (PTDenoiseVirtual 0).
//  `ptobjmotion`   RtDenoiseTemporalRgb.frag with Settings.ObjectMotion: a box whose lit pattern moves with it over a still floor, with
//                  and without the object motion (the NPC ghost).
//  `ptsplitstill`  the whole chain on a still path traced scene: today's combined denoise vs the split (diffuse RGB chain + specular
//                  chain), RMSE against the path tracer's own 512-path converged mean.
//  `ptdenoisecost` the passes' GPU time at 1280 x 720 (D3D9 timestamp queries round the fork draws): combined vs split.

struct PtSplitImages { int spec = -1, gAlbedo = -1, gMaterial = -1; };
static PtSplitImages gPS;
// PTDenoiseMeanVariance's default (frames); NVR_RT_HOST_MEANVAR overrides it for a sweep
static const float kMeanVar = std::getenv("NVR_RT_HOST_MEANVAR") ? float(std::atof(std::getenv("NVR_RT_HOST_MEANVAR"))) : 2.0f;
static const int gIter = std::getenv("NVR_RT_HOST_ITER") ? std::atoi(std::getenv("NVR_RT_HOST_ITER")) : 2;   // (a diagnostic: the a-trous passes in ptfirefly)
static const float gAntiLag = std::getenv("NVR_RT_HOST_ANTILAG") ? float(std::atof(std::getenv("NVR_RT_HOST_ANTILAG"))) : 3.0f;   // (a diagnostic: the chains' AntiLag, PTDenoiseAntiLag 3)
static bool registerPtSplit(Host& h) {
	gPS.spec = h.AddImage("pt specular", S, S, NVR_EXT_SHARED_RGBA16F);
	gPS.gAlbedo = h.AddImage("split gbuffer albedo", S, S, NVR_EXT_SHARED_RGBA16F);
	gPS.gMaterial = h.AddImage("split gbuffer material", S, S, NVR_EXT_SHARED_RGBA16F);
	return gPS.spec >= 0 && gPS.gAlbedo >= 0 && gPS.gMaterial >= 0;
}

// A fork graphics pass with up to 11 images and 3 targets (RtDenoise.cpp Make / Draw: the sampler's heap index in the push block's last
// 16 bytes, or at byte 4 for RtPathTraceShow's 48-byte block)
static bool loadFragN(Host& h, const char* name, UINT images, UINT push, UINT targets, FragPass& out, UINT samplerAt = 0xFFFFFFFFu) {
	FILE* f = nullptr; const std::string file = gM.compute + "\\" + name + ".spv"; fopen_s(&f, file.c_str(), "rb");
	if (!f) { say("  error: no %s\n", file.c_str()); return false; }
	std::vector<char> spirv; char buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
	fclose(f);
	NVR_EXT_BINDING_DESC bindings[12] = {};
	for (UINT i = 0; i < images; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
	bindings[images] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, samplerAt != 0xFFFFFFFFu ? samplerAt : push - 16 };
	NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
	desc.pFragmentSpirv = spirv.data(); desc.FragmentSpirvSize = UINT(spirv.size());
	desc.BindingCount = images + 1; desc.pBindings = bindings; desc.PushConstantSize = push;
	desc.UsesSamplerHeap = TRUE; desc.SamplerHeapSet = 0; desc.SamplerHeapBinding = 0;
	desc.ColorTargetCount = targets; desc.Blend = NVR_EXT_BLEND_NONE; desc.ColorWriteMask = 0xF; desc.pDebugName = name;
	CHECKHR(h.ext->CreateGraphicsPipeline(&desc, &out.pipe), "CreateGraphicsPipeline");
	out.images = images; out.push = push;
	if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	return true;
}
static bool drawN(Host& h, const FragPass& p, IDirect3DSurface9* const* targets, UINT targetCount, IUnknown* const* images, const void* push) {
	NVR_EXT_RESOURCE_BINDING b[12] = {};
	for (UINT i = 0; i < p.images; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = images[i]; }
	b[p.images].Slot = 0; b[p.images].Type = NVR_EXT_BIND_SAMPLER; b[p.images].pResource = gPointClamp;
	NVR_EXT_DRAW_DESC d = {};
	d.pPipeline = p.pipe; d.TargetCount = targetCount;
	for (UINT t = 0; t < targetCount; t++) d.Targets[t].pResource = targets[t];
	d.BindingCount = p.images + 1; d.pBindings = b; d.pPushData = push; d.PushDataSize = p.push;
	CHECKHR(h.ext->Draw(&d), "Draw");
	return true;
}
static void freeTex(std::initializer_list<Tex*> list) { for (Tex* t : list) { if (t->sys) t->sys->Release(); if (t->s) t->s->Release(); if (t->t) t->t->Release(); *t = Tex(); } }

// RtDenoise.cpp's push blocks (the RGB temporal 200 bytes, the specular one 216, the a-trous 48, the firefly spread 32)
struct DnTemporalPush { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4]; uint32_t samplerIndex; float clampK, peopleCap, antiLag, fireflyK, fastFrames; };
static_assert(sizeof(DnTemporalPush) == 200, "RtDenoise.cpp's temporal push block");
struct DnSpecPush { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4], spec[4]; uint32_t samplerIndex; float clampK, peopleCap, antiLag, fireflyK, fastFrames; };
static_assert(sizeof(DnSpecPush) == 216, "RtDenoise.cpp's specular temporal push block");
struct DnAtrousPush { float params[4], size[4]; uint32_t samplerIndex; float guideSigma; uint32_t specular; float historyFix; };
static_assert(sizeof(DnAtrousPush) == 48, "RtDenoise.cpp's a-trous push block");

// The camera block as RtDenoise::Run fills it: this camera's basis, the previous camera's camera-relative view-projection (same axes
// here: the scenes slide, they do not turn), the move since
template <class P> static void dnCamera(P& t, const Camera& now, const Camera& prev, UINT w, UINT hh, float maxHistory, bool reset) {
	std::memset(&t, 0, sizeof(t));
	const V3 ax = prev.r * prev.p00, ay = prev.u * prev.p11, aw = prev.f;
	for (int col = 0; col < 3; col++) { t.prevViewProj[col * 4 + 0] = (&ax.x)[col]; t.prevViewProj[col * 4 + 1] = (&ay.x)[col]; t.prevViewProj[col * 4 + 3] = (&aw.x)[col]; }
	put4(t.forward, now.f, now.farZ); put4(t.right, now.r * (1.0f / now.p00), 0); put4(t.up, now.u * (1.0f / now.p11), 0);
	put4(t.move, now.eye - prev.eye, reset ? 1.0f : 0.0f);
	put4(t.params, maxHistory, 0.03f, 1.0f / w, 1.0f / hh);
	put4(t.rect, 1, 1, 1, 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The path tracer's pass with the G-buffer (flag 8 + 256: the screen's colour, normal, roughness / metal), PTSpecular, the demodulation,
// the energy table, and optionally the split; reads back the light put together (both ways) and the split's parts
struct PtFrame { std::vector<float> display, albedo, spec, specA, specB; };
static bool ptSplitTrace(Ctx& c, const World& w, const std::vector<uint32_t>& ids, const Camera& cam, uint32_t flags, float seed, int bounces, PtFrame& out,
	V3 sunDir = V3(0.4f, -0.3f, 0.866f), V3 sunColor = V3(2.0f, 1.9f, 1.7f), V3 sky = V3(0.25f, 0.3f, 0.4f)) {
	Host& h = *c.host;
	PathConstants k = {};
	cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
	k.origin[3] = seed;
	put4(k.accum, 0.0f, float(bounces), 1.0f, float(flags));
	const V3 sd = normalize(sunDir);
	put4(k.sunDir, sd, sunColor.x > 0 ? 1.0f : 0.0f); put4(k.sunColor, sunColor, std::tan(0.5f * 0.53f * 0.0174532925f));
	put4(k.skyZenith, sky, 1.0f); put4(k.skyHorizon, sky, 0.0f); put4(k.ambient, 0, 0, 0, 1.0f);
	put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f);
	k.rect[3] = 1.0f;
	{	// the lamp image: no lamps, the frame row's energy table from the game's RtRestirRow::Build
		std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
		RtRestirRow::State state; RtRestirRow::Keys keys;
		const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
		const float eye[3] = { 0, 0, 0 };
		RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), nullptr, 0, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
		if (!h.Upload(gM.ptLamps, image)) return false;
	}
	const bool split = (flags & 4194304u) != 0u;
	std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, gPS.gAlbedo, c.pano,
		c.aux16, c.ptDisplay, gPS.gMaterial, gM.ptAlbedoOut, gM.ptLamps };
	if (split) imgs.push_back(gPS.spec);
	h.AddPass(c.shPath, imgs, 0x7u | (1u << 13) | (split ? 1u << 15 : 0u), &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
	if (!h.Exchange(w, ids, cam.eye)) return false;
	bool ok = h.Read(c.ptDisplay, out.display) && h.Read(gM.ptAlbedoOut, out.albedo);
	if (ok && split) ok = h.Read(gPS.spec, out.spec) && h.Read(c.ptA, out.specA) && h.Read(c.ptB, out.specB);
	h.EndFrame();
	return ok;
}
// the specular light of a pixel (specular x its albedo), and the light put back together: display x colour (+ the specular light)
static V3 ptSplitSpec(const PtFrame& f, UINT i) {
	return V3(f.spec[i * 4] * f.specA[i * 2], f.spec[i * 4 + 1] * f.specA[i * 2 + 1], f.spec[i * 4 + 2] * f.specB[i * 2]);
}
static V3 ptSplitLight(const PtFrame& f, UINT i, bool split) {
	V3 l(f.display[i * 4] * f.albedo[i * 4], f.display[i * 4 + 1] * f.albedo[i * 4 + 1], f.display[i * 4 + 2] * f.albedo[i * 4 + 2]);
	if (split) l = l + ptSplitSpec(f, i);
	return l;
}

// The G-buffer images (flag 8 / 256) for a world: depth (view z / farZ), view normals, colour (a 1), material (r = class 1 + 0.49 metal,
// b = roughness) per object
static bool ptSplitGBuffer(Ctx& c, const World& w, const Camera& cam, std::vector<GPixel>& px) {
	Host& h = *c.host;
	std::vector<float> dep, nor;
	gbuffer(w, cam, px, dep, nor);
	std::vector<float> alb(size_t(S) * S * 4, 0.0f), mat(size_t(S) * S * 4, 0.0f);
	for (UINT i = 0; i < S * S; i++) {
		if (!px[i].valid) continue;
		const Object& o = w.objects[px[i].object];
		alb[i * 4] = o.albedo.x; alb[i * 4 + 1] = o.albedo.y; alb[i * 4 + 2] = o.albedo.z; alb[i * 4 + 3] = 1.0f;
		mat[i * 4] = 1.0f + 0.49f * std::max(o.metal, 0.0f); mat[i * 4 + 2] = o.rough >= 0.0f ? o.rough : 0.7f; mat[i * 4 + 3] = 1.0f;
	}
	return h.Upload(c.depth, dep) && h.Upload(c.normals, nor) && h.Upload(gPS.gAlbedo, alb) && h.Upload(gPS.gMaterial, mat);
}

static bool scenePtSplit(Ctx& c) {
	Host& h = *c.host;
	bool ok = true;
	// ---- (1) energy: the same paths with and without the split, three materials (the G-buffer's: a glossy half-metal floor, a gold
	// block, a rough white wall), the sun and the sky, 2 bounces
	{
		World w;
		w.meshes.resize(3);
		w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
		w.meshes[1].box({ -250, 300, 0 }, { 250, 800, 500 });
		w.meshes[2].quad({ -4000, 1500, 0 }, { 4000, 1500, 0 }, { 4000, 1500, 2500 }, { -4000, 1500, 2500 });
		Object floor; floor.mesh = 0; floor.albedo = { 0.5f, 0.45f, 0.4f }; floor.rough = 0.3f; floor.metal = 0.4f;
		Object gold; gold.mesh = 1; gold.albedo = { 0.95f, 0.75f, 0.35f }; gold.rough = 0.25f; gold.metal = 1.0f;
		Object wall; wall.mesh = 2; wall.albedo = { 0.7f, 0.7f, 0.7f }; wall.rough = 0.9f; wall.metal = 0.0f;
		w.objects = { floor, gold, wall };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const Camera cam = Camera::lookDir({ 0, -1400, 900 }, normalize({ 0, 1, -0.55f }), { 0, 0, 1 }, 1.3f, 1.3f);
		std::vector<GPixel> px;
		if (!ptSplitGBuffer(c, w, cam, px)) return false;
		const uint32_t base = 8u | 256u | 128u | 512u | 1024u | 1048576u;
		int checked = 0, bad = 0, shown = 0;
		double total[3] = {}, totalSplit[3] = {}, share[3] = {}, shareN[3] = {};
		for (int f = 0; f < 6; f++) {
			PtFrame a, b;
			const float seed = float(31 + f * 7919);
			if (!ptSplitTrace(c, w, ids, cam, base, seed, 2, a) || !ptSplitTrace(c, w, ids, cam, base | 4194304u, seed, 2, b)) return false;
			for (UINT i = 0; i < S * S; i++) {
				const V3 la = ptSplitLight(a, i, false), lb = ptSplitLight(b, i, true);
				if (!std::isfinite(la.x + la.y + la.z + lb.x + lb.y + lb.z)) { bad++; continue; }
				checked++;
				float err = 0;
				for (int ch = 0; ch < 3; ch++) {
					const float x = (&la.x)[ch], y = (&lb.x)[ch];
					total[ch] += x; totalSplit[ch] += y;
					err = std::max(err, std::fabs(x - y) / (std::fabs(x) + 0.01f));
				}
				if (err > 4e-3f) { bad++; if (shown++ < 4) say("    pixel (%u, %u): combined %.4f %.4f %.4f, split %.4f %.4f %.4f\n", i % S, i / S, la.x, la.y, la.z, lb.x, lb.y, lb.z); }
				if (px[i].valid && px[i].object >= 0 && px[i].object < 3) {
					const V3 sp = ptSplitSpec(b, i);
					share[px[i].object] += dot(sp, V3(0.2126f, 0.7152f, 0.0722f)); shareN[px[i].object] += dot(lb, V3(0.2126f, 0.7152f, 0.0722f));
				}
			}
		}
		float worstSum = 0;
		for (int ch = 0; ch < 3; ch++) worstSum = std::max(worstSum, float(std::fabs(totalSplit[ch] / total[ch] - 1.0)));
		const bool pass = bad <= checked / 500 && worstSum < 1e-3f && checked > int(S * S) * 5;
		say("  split = combined, the same paths (6 frames, 2 bounces, sun + sky): %d pixel-frames, %d off by > 0.4 %%, the sums %.5f %.5f %.5f (worst %.4f %%) -> %s\n",
			checked, bad, totalSplit[0] / total[0], totalSplit[1] / total[1], totalSplit[2] / total[2], worstSum * 100, pass ? "ok" : "BAD");
		say("    specular share of the light: glossy half-metal floor %.1f %%, gold block %.1f %%, rough white wall %.1f %%\n",
			100 * share[0] / std::max(shareN[0], 1e-9), 100 * share[1] / std::max(shareN[1], 1e-9), 100 * share[2] / std::max(shareN[2], 1e-9));
		const bool shares = share[1] / shareN[1] > 0.8 && share[2] / shareN[2] < 0.15 && share[0] / shareN[0] > share[2] / shareN[2];
		say("    (gold > 80 %%, rough wall < 15 %%, floor > wall) -> %s\n", shares ? "ok" : "BAD");
		ok = ok && pass && shares;
		// PTSpecular off: no specular light at all, the sum still the combined one
		PtFrame a, b;
		if (!ptSplitTrace(c, w, ids, cam, base & ~128u, 77.0f, 2, a) || !ptSplitTrace(c, w, ids, cam, (base & ~128u) | 4194304u, 77.0f, 2, b)) return false;
		int specNonZero = 0, off = 0;
		for (UINT i = 0; i < S * S; i++) {
			if (b.spec[i * 4] != 0.0f || b.spec[i * 4 + 1] != 0.0f || b.spec[i * 4 + 2] != 0.0f) specNonZero++;
			const V3 la = ptSplitLight(a, i, false), lb = ptSplitLight(b, i, true);
			for (int ch = 0; ch < 3; ch++) if (std::fabs((&la.x)[ch] - (&lb.x)[ch]) > 4e-3f * (std::fabs((&la.x)[ch]) + 0.01f)) { off++; break; }
		}
		const bool passOff = specNonZero == 0 && off == 0;
		say("  PTSpecular off: %d pixels with specular light (want 0), %d off the combined -> %s\n", specNonZero, off, passOff ? "ok" : "BAD");
		ok = ok && passOff;
		releaseWorld(h, ids);
	}
	// ---- (2) the specular hit distance: a mirror floor (roughness 0.05, metal 1), a block and a wall to reflect; each pixel whose bounce
	// was the specular lobe's: its hit distance = the CPU's ray reflected at the pixel's floor point (the jittered first hit: 2 % + 8 units)
	{
		World w;
		w.meshes.resize(3);
		w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
		w.meshes[1].box({ -300, 200, 0 }, { 300, 700, 700 });
		w.meshes[2].quad({ -4000, 1600, 0 }, { 4000, 1600, 0 }, { 4000, 1600, 1500 }, { -4000, 1600, 1500 });
		Object floor; floor.mesh = 0; floor.albedo = { 0.9f, 0.9f, 0.9f }; floor.rough = 0.05f; floor.metal = 1.0f;
		Object block; block.mesh = 1; block.albedo = { 0.6f, 0.3f, 0.2f }; block.rough = 0.8f; block.metal = 0.0f;
		Object wall; wall.mesh = 2; wall.albedo = { 0.3f, 0.5f, 0.7f }; wall.rough = 0.8f; wall.metal = 0.0f;
		w.objects = { floor, block, wall };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const Camera cam = Camera::lookDir({ 0, -1800, 700 }, normalize({ 0, 1, -0.5f }), { 0, 0, 1 }, 1.3f, 1.3f);
		std::vector<GPixel> px;
		if (!ptSplitGBuffer(c, w, cam, px)) return false;
		PtFrame b;
		if (!ptSplitTrace(c, w, ids, cam, 8u | 256u | 128u | 512u | 1024u | 1048576u | 4194304u, 5.0f, 1, b)) return false;
		int lobe = 0, hits = 0, hitsOk = 0, misses = 0, missesOk = 0, floorPx = 0, shown = 0;
		for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) {
			const UINT p = j * S + i;
			if (!px[p].valid || px[p].object != 0) continue;
			floorPx++;
			const float t = b.spec[p * 4 + 3];
			if (t <= 0.0f) continue;
			lobe++;
			// the CPU: the view ray reflected at the floor (normal +z). The path tracer's first hit is jittered inside the pixel, so the CPU's
			// answer is the range over 5 points of the pixel; a pixel whose points disagree on hitting is on an edge of the reflection: left out
			float tLo = 1e9f, tHi = 0.0f; int hitN = 0;
			bool onFloor = true;
			for (int s = 0; s < 5 && onFloor; s++) {
				const float ox = s == 1 ? -0.45f : s == 2 ? 0.45f : 0.0f, oy = s == 3 ? -0.45f : s == 4 ? 0.45f : 0.0f;
				const V3 d = normalize(cam.ray((i + 0.5f + ox) / S, (j + 0.5f + oy) / S));
				const Hit first = trace(w, cam.eye, d, 1e-3f, 1e9f, 0xFF, false);   // (the camera ray: a point of the pixel on the block is an edge pixel)
				if (!first.hit || first.object != 0) { onFloor = false; break; }
				const float tf = -cam.eye.z / d.z;
				const V3 P = cam.eye + d * tf;
				const V3 r = V3(d.x, d.y, -d.z);
				const Hit hr = trace(w, P + V3(0, 0, 1.0f), r, 1e-3f, 60000.0f, 0xFF, false);
				if (hr.hit) { hitN++; tLo = std::min(tLo, hr.t); tHi = std::max(tHi, hr.t); }
			}
			if (!onFloor || (hitN != 0 && hitN != 5)) continue;
			if (hitN == 5) {
				hits++;
				if (t >= tLo * 0.98f - 8.0f && t <= tHi * 1.02f + 8.0f) hitsOk++;
				else if (shown++ < 6) say("    pixel (%u, %u): hit distance %.1f, the CPU's %.1f .. %.1f\n", i, j, t, tLo, tHi);
			}
			else { misses++; if (t > 50000.0f) missesOk++; }
		}
		const bool pass = lobe > floorPx * 3 / 4 && hits > 500 && hitsOk >= hits * 95 / 100 && misses > 100 && missesOk >= misses * 95 / 100;
		say("  specular hit distance on a mirror floor (roughness 0.05, metal 1): %d of %d floor pixels took the specular lobe; reflected hits %d / %d "
			"within the CPU's range over the pixel (+- 2 %% + 8), sky misses %d / %d written as 60000 -> %s\n", lobe, floorPx, hitsOk, hits, missesOk, misses, pass ? "ok" : "BAD");
		ok = ok && pass;
		releaseWorld(h, ids);
	}
	return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// `ptspecmove`: RtDenoiseTemporalSpec.frag on an analytic mirror. A floor (z 0, roughness 0.05) reflects a wall at y = 1400 (x stripes,
// 0.5 + 0.45 sin(2 pi x / 240)) and the sky above it (0.3); the camera slides along x 12 units a frame. Each pixel's signal = the exact
// reflection (clean) or x (1 + 0.35 n) (noisy, n a normal deviate), its hit distance the reflected ray's. 24 still frames, then 24 moving:
// the error against the exact reflection over the last 8 moving frames, with the virtual reprojection (PTDenoiseVirtual 1) and without.
static bool scenePtSpecMove(Ctx& c) {
	Host& h = *c.host;
	FragPass fp;
	if (!loadFragN(h, "RtDenoiseTemporalSpec", 10, 216, 4, fp, 192)) return false;
	const float wallY = 1400.0f, wallTop = 1600.0f;
	auto reflection = [&](const Camera& cam, UINT i, UINT j, float& value, float& hit, float& viewZ) -> bool {
		const V3 ray = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);   // a point at view z s: eye + ray s
		if (ray.z >= -1e-4f) return false;
		viewZ = -cam.eye.z / ray.z;
		const V3 P = cam.eye + ray * viewZ;
		const V3 d = normalize(ray), r(d.x, d.y, -d.z);
		const float t = r.y > 1e-4f ? (wallY - P.y) / r.y : -1.0f;
		const V3 q = P + r * t;
		if (t > 0.0f && q.z <= wallTop) { value = 0.5f + 0.45f * std::sin(2.0f * 3.14159265f * q.x / 240.0f); hit = t; }
		else { value = 0.3f; hit = 60000.0f; }
		return true;
	};
	Tex noisy, depth, rough, captured, hA, hB, hC, oA, oB, oC, oD;
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, F, false, rough) ||
		!makeTex(h, S, S, F, false, captured) || !makeTex(h, S, S, F, false, hA) || !makeTex(h, S, S, F, false, hB) || !makeTex(h, S, S, F, false, hC) ||
		!makeTex(h, S, S, F, true, oA) || !makeTex(h, S, S, F, true, oB) || !makeTex(h, S, S, F, true, oC) || !makeTex(h, S, S, F, true, oD)) return false;
	{ std::vector<float> r(size_t(S) * S * 4, 0.0f), z(size_t(S) * S * 4, 0.0f); for (UINT i = 0; i < S * S; i++) r[i * 4 + 1] = 0.05f;
	  if (!fillTex(rough, r, 4) || !fillTex(captured, z, 4)) return false; }
	uint32_t rng = 12345;
	auto gauss = [&]() { auto u = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return (float(((x >> 22u) ^ x) >> 8) + 0.5f) / 16777216.0f; };
		return std::sqrt(-2.0f * std::log(u())) * std::cos(6.2831853f * u()); };
	struct Result { double err = 0, rmse = 0; int n = 0; double still = 0; };
	// (2026-11-09 pt-mirrors) mirror: PTDenoiseMirrorSurface (the hit distance marked negative, spec.z 1: the full history where the virtual
	// one holds; the normals image a constant stand-in: a flat mirror, curvature 0)
	auto run = [&](bool virtualOn, float antiLag, bool noise, Result& res, bool mirror = false) -> bool {
		std::vector<float> a(size_t(S) * S * 4, 0.0f), b(size_t(S) * S * 4, 0.0f), cc(size_t(S) * S * 4, 0.0f);
		rng = 12345;
		Camera prev = Camera::lookDir({ 0, -600, 500 }, normalize({ 0, 1, -0.75f }), { 0, 0, 1 }, 1.3f, 1.3f);
		for (int f = 0; f < 48; f++) {
			Camera cam = prev;
			if (f >= 24) cam.eye = prev.eye + V3(12.0f, 0, 0);
			std::vector<float> sig(size_t(S) * S * 4, 0.0f), dz(size_t(S) * S, 1.0f), exact(size_t(S) * S, -1.0f);
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				float v, t, z;
				if (!reflection(cam, i, j, v, t, z)) continue;
				const size_t p = size_t(j) * S + i;
				exact[p] = v;
				const float n = noise ? std::max(1.0f + 0.35f * gauss(), 0.0f) : 1.0f;
				sig[p * 4] = sig[p * 4 + 1] = sig[p * 4 + 2] = v * n; sig[p * 4 + 3] = mirror ? -t : t;
				dz[p] = z / cam.farZ;
			}
			if (!fillTex(noisy, sig, 4) || !fillTex(depth, dz, 1) || !fillTex(hA, a, 4) || !fillTex(hB, b, 4) || !fillTex(hC, cc, 4)) return false;
			DnSpecPush t;
			dnCamera(t, cam, prev, S, S, 32.0f, f == 0);
			put4(t.spec, 12.0f, virtualOn ? 1.0f : 0.0f, mirror ? 1.0f : 0.0f, 0);
			t.antiLag = antiLag;
			IUnknown* images[10] = { noisy.t, hA.t, depth.t, hB.t, depth.t, depth.t, captured.t, rough.t, hC.t, rough.t };   // (pt-mirrors: [9] the normals, read only with spec.z)
			IDirect3DSurface9* targets[4] = { oA.s, oB.s, oC.s, oD.s };
			if (!drawN(h, fp, targets, 4, images, &t)) return false;
			if (!readTex(h, oA, a, 4) || !readTex(h, oB, b, 4) || !readTex(h, oC, cc, 4)) return false;
			if (f >= 40 || f == 23) {
				double e = 0, e2 = 0; int n = 0;
				for (UINT j = 3; j < S - 3; j++) for (UINT i = 3; i < S - 3; i++) {
					const size_t p = size_t(j) * S + i;
					if (exact[p] < 0.0f) continue;
					const double d = a[p * 4] - exact[p];
					e += std::fabs(d); e2 += d * d; n++;
				}
				if (f == 23) res.still = n ? e / n : 0;
				else { res.err += n ? e / n / 8.0 : 0; res.rmse += n ? std::sqrt(e2 / n) / 8.0 : 0; res.n = n; }
			}
			prev = cam;
		}
		return true;
	};
	Result sv0, vv0, sv3, vv3, sn, vn, mv3, mn;
	if (!run(false, 0.0f, false, sv0) || !run(true, 0.0f, false, vv0) || !run(false, 3.0f, false, sv3) || !run(true, 3.0f, false, vv3) ||
		!run(false, 3.0f, true, sn) || !run(true, 3.0f, true, vn) || !run(true, 3.0f, false, mv3, true) || !run(true, 3.0f, true, mn, true)) return false;
	say("  mirror reflection, camera sliding 12 units a frame (clean signal), mean |error| over the last 8 moving frames (%d pixels):\n", vv0.n);
	say("    AntiLag 0: surface-only reprojection %.4f, hit-distance (virtual) %.4f (x %.2f); still before the slide %.5f / %.5f\n",
		sv0.err, vv0.err, vv0.err / std::max(sv0.err, 1e-9), sv0.still, vv0.still);
	say("    AntiLag 3 (the default): surface-only %.4f, virtual %.4f (x %.2f)\n", sv3.err, vv3.err, vv3.err / std::max(sv3.err, 1e-9));
	say("    noisy (x (1 + 0.35 n)), AntiLag 3: RMSE surface-only %.4f, virtual %.4f (x %.2f)\n", sn.rmse, vn.rmse, vn.rmse / std::max(sn.rmse, 1e-9));
	// (2026-11-09 pt-mirrors) PTDenoiseMirrorSurface: the full history while the reflection stands still on screen (the still camera: less
	// noise), MirrorHistory as it slides 2+ pixels a frame (this slide: no worse than the virtual path)
	say("    PTDenoiseMirrorSurface (marked): still camera, noisy mean |error| %.4f (x %.2f of virtual's %.4f); sliding: clean %.4f (x %.2f of"
		" virtual), noisy RMSE %.4f (x %.2f of virtual); clean still %.5f\n", mn.still, mn.still / std::max(vn.still, 1e-9), vn.still,
		mv3.err, mv3.err / std::max(vv3.err, 1e-9), mn.rmse, mn.rmse / std::max(vn.rmse, 1e-9), mv3.still);
	const bool mirrorOk = mn.still < 1.0 * vn.still && mn.rmse < 1.05 * vn.rmse && mv3.err < 1.1 * vv3.err + 0.002 && mv3.still < 0.002;
	const bool pass = vv0.err < 0.6 * sv0.err && vv3.err < 0.6 * sv3.err && vn.rmse < sn.rmse && vv0.still < 0.002 && sv0.still < 0.002 && mirrorOk;
	say("    (virtual < 0.6 x surface-only at AntiLag 0 and 3, below it noisy; still < 0.002; mirror surface (this scene runs the temporal pass only: the gain is the a-trous pass' and the lobe's): still noisy <= virtual,"
		" sliding noisy < 1.05 x and clean < 1.1 x virtual + 0.002) -> %s\n", pass ? "ok" : "BAD");
	freeTex({ &noisy, &depth, &rough, &captured, &hA, &hB, &hC, &oA, &oB, &oC, &oD });
	return pass;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// `ptobjmotion`: RtDenoiseTemporalRgb.frag (the path tracer's diffuse signal, demodulated: what is left is light) with Settings.ObjectMotion.
// A still camera over a floor of light 0.3; a 600 x 600 box top 400 units up whose light is a stripe pattern that moves with it
// (0.5 + 0.4 sin(2 pi (x - box x) / 480): its lit and shadowed sides) slides 40 units a frame along x. TESR_ObjectMotionBuffer = its move
// on its pixels. 16 still frames, then 16 moving: the error on the box (the smeared pattern: the ghost) and the floor's excess light
// behind it (a trail), with and without the object motion, AntiLag 0 and 3 + MovingHistory 8 (the defaults).
static bool scenePtObjMotion(Ctx& c) {
	Host& h = *c.host;
	FragPass fp;
	if (!loadFragN(h, "RtDenoiseTemporalRgb", 7, 200, 3, fp, 176)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	Tex noisy, depth, motion, captured, hA, hB, oA, oB, oD;
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, F, false, motion) ||
		!makeTex(h, S, S, F, false, captured) || !makeTex(h, S, S, F, false, hA) || !makeTex(h, S, S, F, false, hB) ||
		!makeTex(h, S, S, F, true, oA) || !makeTex(h, S, S, F, true, oB) || !makeTex(h, S, S, F, true, oD)) return false;
	{ std::vector<float> z(size_t(S) * S * 4, 0.0f); if (!fillTex(captured, z, 4)) return false; }
	struct Result { double box = 0, trail = 0; };
	auto run = [&](bool objectMotion, float antiLag, float movingCap, Result& res) -> bool {
		std::vector<float> a(size_t(S) * S * 4, 0.0f), b(size_t(S) * S * 4, 0.0f);
		float boxX = -900.0f;
		for (int f = 0; f < 32; f++) {
			const float move = f >= 16 ? 40.0f : 0.0f;
			boxX += move;
			std::vector<float> sig(size_t(S) * S * 4, 0.0f), dz(size_t(S) * S, 0.0f), mo(size_t(S) * S * 4, 0.0f), exact(size_t(S) * S, 0.0f);
			std::vector<char> onBox(size_t(S) * S, 0);
			for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
				const size_t p = size_t(j) * S + i;
				const V3 ray = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
				const float zTop = (cam.eye.z - 400.0f), zFloor = cam.eye.z;   // view z of the box top / the floor (looking straight down)
				const V3 q = cam.eye + ray * zTop;
				const bool box = std::fabs(q.x - boxX) < 300.0f && std::fabs(q.y) < 300.0f;
				float v; float z;
				if (box) { v = 0.5f + 0.4f * std::sin(2.0f * 3.14159265f * (q.x - boxX) / 480.0f); z = zTop; mo[p * 4] = move; onBox[p] = 1; }
				else { v = 0.3f; z = zFloor; }
				exact[p] = v;
				sig[p * 4] = sig[p * 4 + 1] = sig[p * 4 + 2] = v; sig[p * 4 + 3] = 1.0f;
				dz[p] = z / cam.farZ;
			}
			if (!fillTex(noisy, sig, 4) || !fillTex(depth, dz, 1) || !fillTex(motion, mo, 4) || !fillTex(hA, a, 4) || !fillTex(hB, b, 4)) return false;
			DnTemporalPush t;
			dnCamera(t, cam, cam, S, S, 32.0f, f == 0);
			put4(t.backbone, objectMotion ? 1.0f : 0.0f, 0.0f, objectMotion ? movingCap : 0.0f, 0.0f);
			t.antiLag = antiLag;
			IUnknown* images[7] = { noisy.t, hA.t, depth.t, hB.t, objectMotion ? (IUnknown*)motion.t : (IUnknown*)depth.t, depth.t, captured.t };
			IDirect3DSurface9* targets[3] = { oA.s, oB.s, oD.s };
			if (!drawN(h, fp, targets, 3, images, &t)) return false;
			if (!readTex(h, oA, a, 4) || !readTex(h, oB, b, 4)) return false;
			if (f >= 24) {
				double e = 0, tr = 0; int n = 0, m = 0;
				for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) {
					const size_t p = size_t(j) * S + i;
					if (onBox[p]) { e += std::fabs(a[p * 4] - exact[p]); n++; }
					else { tr += std::max(a[p * 4] - exact[p], 0.0f); m++; }
				}
				res.box += n ? e / n / 8.0 : 0; res.trail += tr / 8.0;
			}
		}
		return true;
	};
	Result off0, on0, off3, on3;
	if (!run(false, 0.0f, 0.0f, off0) || !run(true, 0.0f, 0.0f, on0) || !run(false, 3.0f, 8.0f, off3) || !run(true, 3.0f, 8.0f, on3)) return false;
	say("  a box whose light pattern moves with it, 40 units (~1.5 px) a frame, last 8 of 16 moving frames:\n");
	say("    AntiLag 0: box error (the ghost) %.4f without the object motion, %.4f with it (x %.2f); floor trail energy %.2f / %.2f\n",
		off0.box, on0.box, on0.box / std::max(off0.box, 1e-9), off0.trail, on0.trail);
	say("    AntiLag 3 + MovingHistory 8 (the defaults): box error %.4f without, %.4f with (x %.2f); floor trail %.2f / %.2f\n",
		off3.box, on3.box, on3.box / std::max(off3.box, 1e-9), off3.trail, on3.trail);
	const bool pass = on0.box < 0.3 * off0.box && on3.box < 0.6 * off3.box && on3.trail <= off3.trail + 0.5;
	say("    (with < 0.3 x without at AntiLag 0, < 0.6 x with the defaults, no more trail) -> %s\n", pass ? "ok" : "BAD");
	freeTex({ &noisy, &depth, &motion, &captured, &hA, &hB, &oA, &oB, &oD });
	return pass;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The game's RtDenoise::Run RGB chain on D3D9 textures (temporal + 2 a-trous, steps 1 and 2; Settings as RtPathTrace.cpp gives them):
// one frame. histA / histB (+ histC: specular) hold last frame's and get this frame's.
struct DnChain {
	FragPass temporalRgb, temporalSpec, atrous, firefly;
	Tex hA, hB, hC, oA, oB, oC, oD, tA, tB, cA[2], cB[2], scratch, out;
	float meanVariance = 0.0f;	// the a-trous' Settings.MeanVariance (frames, 0 off)
	float antiFirefly = 0.0f;	// the temporal passes' Settings.AntiFirefly (k, 0 off)
	bool make(Host& h, UINT w, UINT hh) {
		const D3DFORMAT F = D3DFMT_A16B16G16R16F;
		// (only oA and out are read back: the others without a system-memory twin, so two 720p chains fit the 32-bit host)
		auto target = [&](Tex& t) {
			t.f = F; t.w = w; t.h = hh;
			CHECKHR(h.dev->CreateTexture(w, hh, 1, D3DUSAGE_RENDERTARGET, F, D3DPOOL_DEFAULT, &t.t, nullptr), "texture");
			t.t->GetSurfaceLevel(0, &t.s);
			return true;
		};
		for (Tex* t : { &oA, &out }) if (!makeTex(h, w, hh, F, true, *t)) return false;
		for (Tex* t : { &oB, &oC, &oD, &tA, &tB, &cA[0], &cA[1], &cB[0], &cB[1], &scratch, &hA, &hB, &hC }) if (!target(*t)) return false;
		return loadFragN(h, "RtDenoiseTemporalRgb", 7, 200, 3, temporalRgb, 176) && loadFragN(h, "RtDenoiseTemporalSpec", 10, 216, 4, temporalSpec, 192) && loadFragN(h, "RtDenoiseFirefly", 3, 32, 2, firefly) &&
			loadFragN(h, "RtDenoiseAtrousRgb", 5, 48, 2, atrous);
	}
	void free() { freeTex({ &hA, &hB, &hC, &oA, &oB, &oC, &oD, &tA, &tB, &cA[0], &cA[1], &cB[0], &cB[1], &scratch, &out }); }
	// the history for next frame (RtDenoise ping-pongs its textures; here a copy, which the cost scene leaves out of its timing)
	void keep(Host& h, bool spec) {
		h.dev->StretchRect(oA.s, nullptr, hA.s, nullptr, D3DTEXF_POINT); h.dev->StretchRect(oB.s, nullptr, hB.s, nullptr, D3DTEXF_POINT);
		if (spec) h.dev->StretchRect(oC.s, nullptr, hC.s, nullptr, D3DTEXF_POINT);
	}
	// noisy: the signal; rough: .g roughness (specular); normals: TESR_NormalsBuffer; depth: TESR_DepthBuffer; standIn any texture
	bool run(Host& h, bool spec, IUnknown* noisy, IUnknown* depth, IUnknown* normals, IUnknown* rough, IUnknown* standIn, const Camera& cam, UINT w, UINT hh,
		bool reset, int iterations = 2, float sigma = 1.0f, bool keepHistory = true, float historyFix = 0.0f) {
		// (AntiFirefly, as RtDenoise::Run: the temporal A into tA, the excess into oD, then RtDenoiseFirefly.frag spreads it into oA)
		const bool ff = antiFirefly > 0.0f;
		IDirect3DSurface9* firstA = ff ? tA.s : oA.s;
		IDirect3DSurface9* firstB = ff ? tB.s : oB.s;
		if (!spec) {
			IDirect3DSurface9* t3[3] = { firstA, firstB, oD.s };
			DnTemporalPush t; dnCamera(t, cam, cam, w, hh, 32.0f, reset); t.antiLag = gAntiLag; t.peopleCap = 0.0f; t.fireflyK = antiFirefly;
			IUnknown* images[7] = { noisy, hA.t, depth, hB.t, standIn, standIn, standIn };
			if (!drawN(h, temporalRgb, t3, 3, images, &t)) return false;
		} else {
			IDirect3DSurface9* t4[4] = { firstA, firstB, oC.s, oD.s };
			DnSpecPush t; dnCamera(t, cam, cam, w, hh, 32.0f, reset); t.antiLag = gAntiLag; put4(t.spec, 12.0f, 1.0f, 0, 0); t.fireflyK = antiFirefly;
			IUnknown* images[10] = { noisy, hA.t, depth, hB.t, standIn, standIn, standIn, rough, hC.t, normals };   // (2026-11-09 pt-mirrors: + the normals)
			if (!drawN(h, temporalSpec, t4, 4, images, &t)) return false;
		}
		if (ff) {
			struct { float size[4]; uint32_t samplerIndex, pad0, pad1, pad2; } f = { { 1.0f / w, 1.0f / hh, 0.0f, 0.0f }, 0, 0, 0, 0 };
			IUnknown* images[3] = { tA.t, oD.t, tB.t };
			IDirect3DSurface9* t2[2] = { oA.s, oB.s };
			if (!drawN(h, firefly, t2, 2, images, &f)) return false;
		}
		if (keepHistory) keep(h, spec);
		IUnknown* srcA = oA.t; IUnknown* srcB = oB.t;
		DnAtrousPush a = {};
		put4(a.params, 0.0f, 1.0f, 64.0f, sigma); put4(a.size, 1.0f / w, 1.0f / hh, meanVariance, 0.0f);
		a.specular = (spec ? 1u : 0u) | (antiFirefly > 0.0f ? 2u : 0u); a.historyFix = historyFix;
		const int n = std::max(iterations, 1);
		for (int i = 0; i < n; i++) {
			const bool last = i == n - 1;
			a.params[0] = iterations ? float(1 << i) : 0.0f;
			IUnknown* images[5] = { srcA, normals, srcB, spec ? rough : srcA, srcA };
			IDirect3DSurface9* t2[2] = { last ? out.s : cA[i & 1].s, last ? scratch.s : cB[i & 1].s };
			if (!drawN(h, atrous, t2, 2, images, &a)) return false;
			srcA = cA[i & 1].t; srcB = cB[i & 1].t;
		}
		return true;
	}
};

// `ptsplitstill`: a still path traced scene (the energy scene's three materials), 32 frames of one path a pixel through today's chain
// (combined) and the split's two chains, against the path tracer's own 512-path mean (its reference mode). RMSE over the surface pixels,
// frames 24-31.
static bool scenePtSplitStill(Ctx& c) {
	Host& h = *c.host;
	World w;
	w.meshes.resize(3);
	w.meshes[0].quad({ -4000, -4000, 0 }, { 4000, -4000, 0 }, { 4000, 4000, 0 }, { -4000, 4000, 0 });
	w.meshes[1].box({ -250, 300, 0 }, { 250, 800, 500 });
	w.meshes[2].quad({ -4000, 1500, 0 }, { 4000, 1500, 0 }, { 4000, 1500, 2500 }, { -4000, 1500, 2500 });
	Object floor; floor.mesh = 0; floor.albedo = { 0.5f, 0.45f, 0.4f }; floor.rough = 0.3f; floor.metal = 0.4f;
	Object gold; gold.mesh = 1; gold.albedo = { 0.95f, 0.75f, 0.35f }; gold.rough = 0.25f; gold.metal = 1.0f;
	Object wall; wall.mesh = 2; wall.albedo = { 0.7f, 0.7f, 0.7f }; wall.rough = 0.9f; wall.metal = 0.0f;
	w.objects = { floor, gold, wall };
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;
	const Camera cam = Camera::lookDir({ 0, -1400, 900 }, normalize({ 0, 1, -0.55f }), { 0, 0, 1 }, 1.3f, 1.3f);
	std::vector<GPixel> px;
	if (!ptSplitGBuffer(c, w, cam, px)) return false;
	const uint32_t base = 8u | 256u | 128u | 1024u | 1048576u;
	// the reference: the path tracer's running mean (no demodulation), 512 paths a pixel (32 frames x 16 paths)
	std::vector<float> ref;
	{
		for (int f = 0; f < 32; f++) {
			PathConstants k = {};
			cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
			k.origin[3] = float(9001 + f * 7919);
			put4(k.accum, float(f * 16), 2.0f, 16.0f, float(base));
			const V3 sd = normalize(V3(0.4f, -0.3f, 0.866f));
			put4(k.sunDir, sd, 1.0f); put4(k.sunColor, V3(2.0f, 1.9f, 1.7f), std::tan(0.5f * 0.53f * 0.0174532925f));
			put4(k.skyZenith, V3(0.25f, 0.3f, 0.4f), 1.0f); put4(k.skyHorizon, V3(0.25f, 0.3f, 0.4f), 0.0f); put4(k.ambient, 0, 0, 0, 1.0f);
			put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f); k.rect[3] = 1.0f;
			std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
			RtRestirRow::State state; RtRestirRow::Keys keys;
			const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z }; const float eye[3] = { 0, 0, 0 };
			RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), nullptr, 0, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
			if (!h.Upload(gM.ptLamps, image)) return false;
			const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, gPS.gAlbedo, c.pano,
				c.aux16, c.ptDisplay, gPS.gMaterial, gM.ptAlbedoOut, gM.ptLamps };
			h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(w, ids, cam.eye)) return false;
			if (f == 31 && !h.Read(c.ptDisplay, ref)) return false;
			h.EndFrame();
		}
	}
	// the D3D9 side: the G-buffer as the game's textures
	Tex depthT, normalsT, noisyD, noisyS, roughT;
	std::vector<float> dep, nor;
	{ std::vector<GPixel> p2; gbuffer(w, cam, p2, dep, nor); }
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, D3DFMT_R32F, false, depthT) || !makeTex(h, S, S, F, false, normalsT) || !makeTex(h, S, S, F, false, noisyD) ||
		!makeTex(h, S, S, F, false, noisyS) || !makeTex(h, S, S, F, false, roughT)) return false;
	if (!fillTex(depthT, dep, 1) || !fillTex(normalsT, nor, 4)) return false;
	double rmse[3] = {}, mean[3] = {}, refMean = 0;
	int counted = 0;
	for (int mode = 0; mode < 3; mode++) {
		const bool split = mode >= 1;	// 0 combined (today), 1 split, 2 split + MeanVariance + HistoryFix 3 (the new defaults)
		DnChain diff, spec;
		if (!diff.make(h, S, S) || (split && !spec.make(h, S, S))) return false;
		diff.meanVariance = spec.meanVariance = mode == 2 ? kMeanVar : 0.0f;
		for (int f = 0; f < 32; f++) {
			PtFrame fr;
			if (!ptSplitTrace(c, w, ids, cam, base | 512u | (split ? 4194304u : 0u), float(501 + f * 7919), 2, fr)) return false;
			if (!fillTex(noisyD, fr.display, 4)) return false;
			if (!diff.run(h, false, noisyD.t, depthT.t, normalsT.t, depthT.t, depthT.t, cam, S, S, f == 0, 2, 1.0f, true, mode == 2 ? 3.0f : 0.0f)) return false;
			std::vector<float> outD, outS;
			if (!readTex(h, diff.out, outD, 4)) return false;
			if (split) {
				std::vector<float> rg(size_t(S) * S * 4, 0.0f);
				for (UINT i = 0; i < S * S; i++) rg[i * 4 + 1] = fr.specB[i * 2 + 1];
				if (!fillTex(noisyS, fr.spec, 4) || !fillTex(roughT, rg, 4)) return false;
				if (!spec.run(h, true, noisyS.t, depthT.t, normalsT.t, roughT.t, depthT.t, cam, S, S, f == 0, 2, 1.0f, true, mode == 2 ? 3.0f : 0.0f)) return false;
				if (!readTex(h, spec.out, outS, 4)) return false;
			}
			if (f < 24) continue;
			double e2 = 0, m = 0, rm = 0; int n = 0;
			for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) {
				const UINT p = j * S + i;
				if (!px[p].valid || fr.albedo[p * 4 + 3] < 0.5f) continue;
				for (int ch = 0; ch < 3; ch++) {
					double v = outD[p * 4 + ch] * fr.albedo[p * 4 + ch];
					if (split) v += outS[p * 4 + ch] * (ch < 2 ? fr.specA[p * 2 + ch] : fr.specB[p * 2]);
					const double r = ref[p * 4 + ch];
					e2 += (v - r) * (v - r); m += v; rm += r; n++;
				}
			}
			rmse[mode] += std::sqrt(e2 / n) / 8.0; mean[mode] += m / n / 8.0; if (mode == 0) refMean += rm / n / 8.0;
			counted = n;
		}
		diff.free(); if (split) spec.free();
	}
	freeTex({ &depthT, &normalsT, &noisyD, &noisyS, &roughT });
	releaseWorld(h, ids);
	say("  still scene (glossy half-metal floor, gold block, rough wall; sun + sky, 2 bounces), frames 24-31 vs the 512-path mean (%d values):\n", counted);
	say("    RMSE combined (today) %.4f, split %.4f (x %.3f); mean / reference %.4f / %.4f\n", rmse[0], rmse[1], rmse[1] / rmse[0], mean[0] / refMean, mean[1] / refMean);
	say("    + MeanVariance + HistoryFix 3 (the new defaults): RMSE %.4f (x %.3f of today's); mean / reference %.4f\n", rmse[2], rmse[2] / rmse[0], mean[2] / refMean);
	const bool pass = rmse[1] <= rmse[0] * 1.05 && rmse[2] <= rmse[0] * 1.05 && std::fabs(mean[1] / refMean - 1.0) < 0.03 && std::fabs(mean[2] / refMean - 1.0) < 0.03;
	say("    (split and the new defaults <= 1.05 x combined, means within 3 %%) -> %s\n", pass ? "ok" : "BAD");
	return pass;
}

// `ptdenoisecost`: the D3D9 side's GPU time at 1280 x 720 (D3D9 timestamp queries round the fork draws), 20 frames each, the median:
// today's chain (RGB temporal + 2 a-trous) and the split's extra (specular temporal + 2 a-trous). Informational (fails only on errors).
static bool scenePtDenoiseCost(Ctx& c) {
	Host& h = *c.host;
	const UINT W = 1280, H = 720;
	IDirect3DQuery9 *freqQ = nullptr;
	if (FAILED(h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freqQ)) || !freqQ) { say("  no timestamp queries on this device: skipped\n"); return true; }
	Tex noisy, depth, normals, rough;
	if (!makeTex(h, W, H, D3DFMT_A16B16G16R16F, false, noisy) || !makeTex(h, W, H, D3DFMT_R32F, false, depth) || !makeTex(h, W, H, D3DFMT_A16B16G16R16F, false, normals) ||
		!makeTex(h, W, H, D3DFMT_A16B16G16R16F, false, rough)) return false;
	{
		std::vector<float> n4(size_t(W) * H * 4), d1(size_t(W) * H), nn(size_t(W) * H * 4), r4(size_t(W) * H * 4, 0.0f);
		uint32_t s = 7;
		for (size_t i = 0; i < size_t(W) * H; i++) {
			s = s * 1664525u + 1013904223u;
			const float v = float(s >> 8) / 16777216.0f;
			n4[i * 4] = n4[i * 4 + 1] = n4[i * 4 + 2] = v; n4[i * 4 + 3] = (s & 3) ? 900.0f : 0.0f;
			d1[i] = 0.1f + 0.05f * float(i % W) / W;
			nn[i * 4] = 0.5f; nn[i * 4 + 1] = 0.5f; nn[i * 4 + 2] = 0.0f; nn[i * 4 + 3] = 1.0f;
			r4[i * 4 + 1] = float(i % 7) / 7.0f;
		}
		if (!fillTex(noisy, n4, 4) || !fillTex(depth, d1, 1) || !fillTex(normals, nn, 4) || !fillTex(rough, r4, 4)) return false;
	}
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.78f);
	// the chains made first, then run in turn frame by frame (the GPU is shared with other runs: interleaving puts them under the same
	// load); the history copies outside the timed part. 0: today's RGB chain; 1: the RGB chain with the new defaults (AntiFirefly 2 + its
	// spread pass, MeanVariance 2, HistoryFix 3); 2: the specular chain with them
	double ms[3] = {};
	DnChain chains[3];
	for (int k = 0; k < 3; k++) {
		if (!chains[k].make(h, W, H)) return false;
		if (k) { chains[k].antiFirefly = 2.0f; chains[k].meanVariance = 2.0f; }
	}
	std::vector<double> times[3];
	for (int f = 0; f < 44; f++) for (int k = 0; k < 3; k++) {
		const bool spec = k == 2;
		IDirect3DQuery9 *q0 = nullptr, *q1 = nullptr, *dis = nullptr;
		h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &dis); h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q0); h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q1);
		if (!dis || !q0 || !q1) { say("  timestamp queries failed: skipped\n"); return true; }
		dis->Issue(D3DISSUE_BEGIN); q0->Issue(D3DISSUE_END);
		if (!chains[k].run(h, spec, noisy.t, depth.t, normals.t, rough.t, depth.t, cam, W, H, f == 0, 2, 1.0f, false, k ? 3.0f : 0.0f)) return false;
		q1->Issue(D3DISSUE_END); dis->Issue(D3DISSUE_END); freqQ->Issue(D3DISSUE_END);
		chains[k].keep(h, spec);
		UINT64 t0 = 0, t1 = 0, freq = 0; BOOL disjoint = TRUE;
		for (int tries = 0; tries < 2000 && q1->GetData(&t1, sizeof(t1), D3DGETDATA_FLUSH) == S_FALSE; tries++) Sleep(1);
		q0->GetData(&t0, sizeof(t0), D3DGETDATA_FLUSH); dis->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH);
		for (int tries = 0; tries < 2000 && freqQ->GetData(&freq, sizeof(freq), D3DGETDATA_FLUSH) == S_FALSE; tries++) Sleep(1);
		if (f >= 4 && freq && t1 > t0 && !disjoint) times[k].push_back(double(t1 - t0) * 1000.0 / double(freq));
		q0->Release(); q1->Release(); dis->Release();
	}
	for (int k = 0; k < 3; k++) {
		chains[k].free();
		std::sort(times[k].begin(), times[k].end());
		ms[k] = times[k].empty() ? -1.0 : times[k][times[k].size() / 2];
	}
	freqQ->Release();
	freeTex({ &noisy, &depth, &normals, &rough });
	say("  1280 x 720, median of 40 frames each, interleaved (this GPU, shared with other runs): today's chain (RGB temporal + 2 a-trous) %.3f ms; "
		"with AntiFirefly (+ its spread pass), MeanVariance and HistoryFix %.3f ms; the split's specular chain with them %.3f ms more (the new diffuse + "
		"specular = %.3f ms in all)\n", ms[0], ms[1], ms[2], ms[1] + ms[2]);
	return true;
}

// `ptdisocclusion` (the lead's RT playground, 2026-11-07: speckle trails behind a moving lamp's bulb and a spinning cube, from the
// disocclusion restarts): RtDenoiseAtrousRgb's history fix (Settings.HistoryFix, PTDenoiseHistoryFix). A flat lit surface (0.6) with a
// shadow band (0.1, 12 px) and a 1-pixel shadow line (0.1); one path a pixel modelled as lit with chance 0.25 (value / 0.25, else 0).
//  (a) newly seen pixels (a reset frame: one frame of history everywhere), 4 trials: RMSE after the chain against the clean light, with
//      the history fix 3 < 0.6 x without;
//  (b) 32 still frames: the output with the fix = without it (the fix touches only pixels of <= 3 frames);
//  (c) the playground's chain-link wire shadow (it vanished under the denoiser): how much of the 1-pixel line's depth the temporal output
//      and the a-trous output keep after 32 frames; Settings.MeanVariance (PTDenoiseMeanVariance, 2) must keep > 15 points more of it for
//      < 1.1 x the flat light's noise.
static bool scenePtDisocclusion(Ctx& c) {
	Host& h = *c.host;
	Tex noisy, depth, normals;
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, F, false, normals)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	{
		std::vector<float> d(size_t(S) * S, 3000.0f / cam.farZ), n(size_t(S) * S * 4, 0.0f);
		for (UINT i = 0; i < S * S; i++) { n[i * 4] = 0.5f; n[i * 4 + 1] = 0.5f; n[i * 4 + 2] = 0.0f; n[i * 4 + 3] = 1.0f; }   // facing the camera
		if (!fillTex(depth, d, 1) || !fillTex(normals, n, 4)) return false;
	}
	auto clean = [](UINT i) { return (i >= 40 && i < 52) || i == 90 ? 0.1f : 0.6f; };
	uint32_t rng = 99;
	auto rnd = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return float(((x >> 22u) ^ x) >> 8) / 16777216.0f; };
	auto frame = [&](std::vector<float>& v) {
		v.assign(size_t(S) * S * 4, 0.0f);
		for (UINT p = 0; p < S * S; p++) { const float x = rnd() < 0.25f ? clean(p % S) / 0.25f : 0.0f; v[p * 4] = v[p * 4 + 1] = v[p * 4 + 2] = x; v[p * 4 + 3] = 1.0f; }
	};
	auto rmse = [&](const std::vector<float>& o) {
		double e2 = 0; int n = 0;
		for (UINT j = 3; j < S - 3; j++) for (UINT i = 3; i < S - 3; i++) { const double d = o[(j * S + i) * 4] - clean(i); e2 += d * d; n++; }
		return std::sqrt(e2 / n);
	};
	bool ok = true;
	// (a) newly seen pixels
	double fresh[2] = {};
	for (int fix = 0; fix < 2; fix++) {
		DnChain chain;
		if (!chain.make(h, S, S)) return false;
		rng = 99;
		for (int trial = 0; trial < 4; trial++) {
			std::vector<float> v, out;
			frame(v);
			if (!fillTex(noisy, v, 4)) return false;
			if (!chain.run(h, false, noisy.t, depth.t, normals.t, depth.t, depth.t, cam, S, S, true, 2, 1.0f, true, fix ? 3.0f : 0.0f)) return false;
			if (!readTex(h, chain.out, out, 4)) return false;
			fresh[fix] += rmse(out) / 4.0;
		}
		chain.free();
	}
	const bool passA = fresh[1] < 0.6 * fresh[0];
	say("  newly seen pixels (one frame of history), one path a pixel: RMSE after the chain %.4f without the history fix, %.4f with it (x %.2f; the raw paths' %.3f) -> %s\n",
		fresh[0], fresh[1], fresh[1] / fresh[0], std::sqrt(0.6 * 0.6 * 3.0), passA ? "ok" : "BAD");
	ok = ok && passA;
	// (b) + (c) 32 still frames
	std::vector<float> outs[3], temporals[3];
	for (int fix = 0; fix < 3; fix++) {   // 0 no history fix, 1 with it, 2 with it + MeanVariance
		DnChain chain;
		if (!chain.make(h, S, S)) return false;
		chain.meanVariance = fix == 2 ? kMeanVar : 0.0f;
		rng = 4242;
		for (int f = 0; f < 32; f++) {
			std::vector<float> v;
			frame(v);
			if (!fillTex(noisy, v, 4)) return false;
			if (!chain.run(h, false, noisy.t, depth.t, normals.t, depth.t, depth.t, cam, S, S, f == 0, 2, 1.0f, true, fix ? 3.0f : 0.0f)) return false;
		}
		if (!readTex(h, chain.out, outs[fix], 4) || !readTex(h, chain.oA, temporals[fix], 4)) return false;
		chain.free();
	}
	float worst = 0;
	for (size_t i = 0; i < outs[0].size(); i++) worst = std::max(worst, std::fabs(outs[0][i] - outs[1][i]));
	const bool passB = worst < 1e-4f;
	say("  32 still frames: the output with the history fix vs without, worst difference %.6f (want 0: only pixels of <= 3 frames change) -> %s\n", worst, passB ? "ok" : "BAD");
	ok = ok && passB;
	auto depthKept = [&](const std::vector<float>& o) {
		double line = 0, around = 0;
		for (UINT j = 8; j < S - 8; j++) { line += o[(j * S + 90) * 4]; around += 0.5 * (o[(j * S + 86) * 4] + o[(j * S + 94) * 4]); }
		return (around - line) / (S - 16) / 0.5;   // 1 = the full 0.6 -> 0.1 depth
	};
	auto flatNoise = [&](const std::vector<float>& o) {   // RMSE against the clean light away from the band and the line
		double e2 = 0; int n = 0;
		for (UINT j = 3; j < S - 3; j++) for (UINT i = 3; i < S - 3; i++) {
			if ((i >= 34 && i < 58) || (i >= 84 && i <= 96)) continue;
			const double d = o[(j * S + i) * 4] - clean(i); e2 += d * d; n++;
		}
		return std::sqrt(e2 / n);
	};
	// (c) the playground's chain-link wire: a 1-pixel shadow line's depth kept, and the flat light's leftover noise (the price)
	const double kept1 = depthKept(outs[1]), kept2 = depthKept(outs[2]);
	say("  a 1-pixel shadow line after 32 frames keeps %.0f %% of its depth in the temporal output; after the 2 a-trous passes %.0f %% (one path's variance, "
		"as before), %.0f %% with MeanVariance; the flat light's RMSE %.4f / %.4f\n", 100 * depthKept(temporals[1]), 100 * kept1, 100 * kept2, flatNoise(outs[1]), flatNoise(outs[2]));
	const bool passC = kept2 > kept1 + 0.15 && flatNoise(outs[2]) < 1.1 * flatNoise(outs[1]);
	say("    (MeanVariance %.0f keeps > 15 points more of the line, the flat RMSE < 1.1 x) -> %s\n", kMeanVar, passC ? "ok" : "BAD");
	ok = ok && passC;
	freeTex({ &noisy, &depth, &normals });
	return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// `ptfirefly` (the user's RT playground, 2026-11-07: a room lit through a window and by a bright panel only bounces find, chrome / gold
// spheres, PTIndirectClamp off: white dots hundreds of times brighter than their neighbours survived the denoiser): Settings.AntiFirefly
// (PTDenoiseAntiFirefly / PTDenoiseFireflyK) in both temporal passes + RtDenoiseFirefly.frag's spread.
//  (a) the path tracer: a closed room whose only light is the sun through a 200 x 200 hole in the roof (sun 30: a very bright patch on the
//      floor that lights the rest only by bounces), white walls, a gold block and a chrome block; the split's two chains (diffuse +
//      specular), 32 frames of one path a pixel, against the path tracer's own 512-path mean: firefly pixels (luma > 3 x the reference's
//      + 0.05), RMSE, mean darkening, with and without;
//  (b) analytic, the RGB chain: a flat surface lit 0.5 (noisy) + rare fireflies (chance 0.005 a frame, + 100: real light, the true mean
//      is 1.0), a sun glint (one pixel, 40 every frame) and a thin bright line (one column, 10 every frame): dots, mean, and how much of
//      the glint and the line survive (with / without);
//  (c) a furnace (an open white box, albedo 0.8, a uniform sky, 8 bounces) through the chain: the mean with / without (~ 0 bias).
static bool ptReferenceMean(Ctx& c, const World& w, const std::vector<uint32_t>& ids, const Camera& cam, uint32_t flags, int bounces, V3 sunDir, V3 sunColor,
	V3 sky, std::vector<float>& ref) {
	Host& h = *c.host;
	for (int f = 0; f < 32; f++) {   // the running mean (no demodulation), 32 frames x 16 paths
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = float(9001 + f * 7919);
		put4(k.accum, float(f * 16), float(bounces), 16.0f, float(flags));
		put4(k.sunDir, normalize(sunDir), sunColor.x > 0 ? 1.0f : 0.0f); put4(k.sunColor, sunColor, std::tan(0.5f * 0.53f * 0.0174532925f));
		put4(k.skyZenith, sky, 1.0f); put4(k.skyHorizon, sky, 0.0f); put4(k.ambient, 0, 0, 0, 1.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, 0.0f); k.rect[3] = 1.0f;
		std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
		RtRestirRow::State state; RtRestirRow::Keys keys;
		const double camWorld[3] = { cam.eye.x, cam.eye.y, cam.eye.z }; const float eye[3] = { 0, 0, 0 };
		RtRestirRow::Build(state, &image[size_t(kLightTexels) * kFrameRowIndex * 2], image.data(), nullptr, 0, 8, camWorld, eye, k.forward, k.right, k.up, S, S, keys);
		if (!h.Upload(gM.ptLamps, image)) return false;
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.ptDisplay, c.depth, c.normals, gPS.gAlbedo, c.pano,
			c.aux16, c.ptDisplay, gPS.gMaterial, gM.ptAlbedoOut, gM.ptLamps };
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		if (f == 31 && !h.Read(c.ptDisplay, ref)) return false;
		h.EndFrame();
	}
	return true;
}

// The path tracer's split through the two chains for 32 frames (frames 24-31 measured against ref): dots, RMSE, mean / reference
struct FireflyStats { double dots = 0, rmse = 0, mean = 0; };
static bool ptSplitChains(Ctx& c, const World& w, const std::vector<uint32_t>& ids, const Camera& cam, uint32_t flags, int bounces, V3 sunDir, V3 sunColor, V3 sky,
	const std::vector<GPixel>& px, const std::vector<float>& ref, float antiFirefly, bool split, FireflyStats& st) {
	Host& h = *c.host;
	Tex depthT, normalsT, noisyD, noisyS, roughT;
	std::vector<float> dep, nor;
	{ std::vector<GPixel> p2; gbuffer(w, cam, p2, dep, nor); }
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, D3DFMT_R32F, false, depthT) || !makeTex(h, S, S, F, false, normalsT) || !makeTex(h, S, S, F, false, noisyD) ||
		!makeTex(h, S, S, F, false, noisyS) || !makeTex(h, S, S, F, false, roughT)) return false;
	if (!fillTex(depthT, dep, 1) || !fillTex(normalsT, nor, 4)) return false;
	DnChain diff, spec;
	if (!diff.make(h, S, S) || (split && !spec.make(h, S, S))) return false;
	diff.antiFirefly = spec.antiFirefly = antiFirefly;
	diff.meanVariance = spec.meanVariance = kMeanVar;
	st = FireflyStats();
	double refSum = 0, outSum = 0;
	for (int f = 0; f < 32; f++) {
		PtFrame fr;
		if (!ptSplitTrace(c, w, ids, cam, flags | 512u | (split ? 4194304u : 0u), float(501 + f * 7919), bounces, fr, sunDir, sunColor, sky)) return false;
		if (!fillTex(noisyD, fr.display, 4)) return false;
		if (!diff.run(h, false, noisyD.t, depthT.t, normalsT.t, depthT.t, depthT.t, cam, S, S, f == 0, gIter, 1.0f, true, 3.0f)) return false;
		std::vector<float> outD, outS;
		if (!readTex(h, diff.out, outD, 4)) return false;
		if (split) {
			std::vector<float> rg(size_t(S) * S * 4, 0.0f);
			for (UINT i = 0; i < S * S; i++) rg[i * 4 + 1] = fr.specB[i * 2 + 1];
			if (!fillTex(noisyS, fr.spec, 4) || !fillTex(roughT, rg, 4)) return false;
			if (!spec.run(h, true, noisyS.t, depthT.t, normalsT.t, roughT.t, depthT.t, cam, S, S, f == 0, gIter, 1.0f, true, 3.0f)) return false;
			if (!readTex(h, spec.out, outS, 4)) return false;
		}
		if (f < 24) continue;
		double e2 = 0; int n = 0, dots = 0;
		std::vector<double> lum(size_t(S) * S, -1.0);   // the output's luma (-1: no surface)
		for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) {
			const UINT p = j * S + i;
			if (!px[p].valid || fr.albedo[p * 4 + 3] < 0.5f) continue;
			double v[3];
			for (int ch = 0; ch < 3; ch++) {
				v[ch] = outD[p * 4 + ch] * fr.albedo[p * 4 + ch];
				if (split) v[ch] += outS[p * 4 + ch] * (ch < 2 ? fr.specA[p * 2 + ch] : fr.specB[p * 2]);
				const double r = ref[p * 4 + ch];
				e2 += (v[ch] - r) * (v[ch] - r);
			}
			const double lo = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
			const double lr = 0.2126 * ref[p * 4] + 0.7152 * ref[p * 4 + 1] + 0.0722 * ref[p * 4 + 2];
			lum[p] = lo;
			outSum += lo; refSum += lr; n++;
		}
		// a firefly = a white dot: brighter than 3 x the mean of its 8 neighbours (+ 0.05) where the reference is not (a real lone highlight)
		for (UINT j = 3; j < S - 3; j++) for (UINT i = 3; i < S - 3; i++) {
			const UINT p = j * S + i;
			if (lum[p] < 0.0) continue;
			double m = 0, mr = 0; int k = 0;
			for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
				const UINT q = (j + dy) * S + (i + dx);
				if ((dx || dy) && lum[q] >= 0.0) { m += lum[q]; mr += 0.2126 * ref[q * 4] + 0.7152 * ref[q * 4 + 1] + 0.0722 * ref[q * 4 + 2]; k++; }
			}
			if (!k) continue;
			const double lr = 0.2126 * ref[p * 4] + 0.7152 * ref[p * 4 + 1] + 0.0722 * ref[p * 4 + 2];
			if (lum[p] > 3.0 * m / k + 0.05 && !(lr > 3.0 * mr / k + 0.05)) dots++;
		}
		st.dots += dots / 8.0; st.rmse += std::sqrt(e2 / (3.0 * n)) / 8.0;
	}
	st.mean = outSum / std::max(refSum, 1e-9);
	diff.free(); if (split) spec.free();
	freeTex({ &depthT, &normalsT, &noisyD, &noisyS, &roughT });
	return true;
}

static bool scenePtFirefly(Ctx& c) {
	Host& h = *c.host;
	const float kFF = std::getenv("NVR_RT_HOST_FIREFLYK") ? float(std::atof(std::getenv("NVR_RT_HOST_FIREFLYK"))) : 2.0f;   // PTDenoiseFireflyK's default
	bool ok = true;
	// ---- (a) the room lit through a hole in the roof
	{
		World w;
		w.meshes.resize(3);
		const float R = 1000, H = 800, hole = 100;
		Mesh& room = w.meshes[0];
		room.quad({ -R, -R, 0 }, { R, -R, 0 }, { R, R, 0 }, { -R, R, 0 });                       // floor
		room.quad({ -R, -R, 0 }, { -R, R, 0 }, { -R, R, H }, { -R, -R, H });                      // walls
		room.quad({ R, -R, 0 }, { R, -R, H }, { R, R, H }, { R, R, 0 });
		room.quad({ -R, -R, 0 }, { -R, -R, H }, { R, -R, H }, { R, -R, 0 });
		room.quad({ -R, R, 0 }, { R, R, 0 }, { R, R, H }, { -R, R, H });
		room.quad({ -R, -R, H }, { -R, R, H }, { -hole, R, H }, { -hole, -R, H });                // the roof round the hole
		room.quad({ hole, -R, H }, { hole, R, H }, { R, R, H }, { R, -R, H });
		room.quad({ -hole, -R, H }, { -hole, -hole, H }, { hole, -hole, H }, { hole, -R, H });
		room.quad({ -hole, hole, H }, { -hole, R, H }, { hole, R, H }, { hole, hole, H });
		w.meshes[1].box({ 300, 200, 0 }, { 550, 450, 250 });     // gold
		w.meshes[2].box({ -550, 300, 0 }, { -300, 550, 300 });   // chrome
		Object room0; room0.mesh = 0; room0.albedo = { 0.75f, 0.72f, 0.68f }; room0.rough = 0.85f; room0.metal = 0.0f;
		Object gold; gold.mesh = 1; gold.albedo = { 0.95f, 0.75f, 0.35f }; gold.rough = 0.25f; gold.metal = 1.0f;
		Object chrome; chrome.mesh = 2; chrome.albedo = { 0.9f, 0.9f, 0.9f }; chrome.rough = 0.08f; chrome.metal = 1.0f;
		w.objects = { room0, gold, chrome };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const Camera cam = Camera::lookDir({ -850, -850, 500 }, normalize({ 1, 1, -0.45f }), { 0, 0, 1 }, 1.2f, 1.2f);
		std::vector<GPixel> px;
		if (!ptSplitGBuffer(c, w, cam, px)) return false;
		const V3 sunDir(0.15f, 0.1f, 1.0f), sunColor(30.0f, 29.0f, 27.0f), sky(0.0f, 0.0f, 0.0f);
		const uint32_t base = 8u | 256u | 128u | 1024u | 1048576u;
		std::vector<float> ref;
		if (!ptReferenceMean(c, w, ids, cam, base, 2, sunDir, sunColor, sky, ref)) return false;
		FireflyStats off, on;
		if (!ptSplitChains(c, w, ids, cam, base, 2, sunDir, sunColor, sky, px, ref, 0.0f, true, off) ||
			!ptSplitChains(c, w, ids, cam, base, 2, sunDir, sunColor, sky, px, ref, kFF, true, on)) return false;
		say("  room lit only through a roof hole (sun 30), gold + chrome blocks, 2 bounces, the split's chains, frames 24-31 vs the 512-path mean:\n");
		say("    without AntiFirefly: %.1f white dots a frame (> 3 x their 8 neighbours, not in the reference), RMSE %.4f, mean / reference %.4f\n", off.dots, off.rmse, off.mean);
		say("    with it (k %.1f):    %.1f white dots a frame, RMSE %.4f, mean / reference %.4f\n", kFF, on.dots, on.rmse, on.mean);
		const bool pass = on.dots < 0.1 * off.dots + 1.0 && on.rmse < off.rmse && std::fabs(on.mean - 1.0) < 0.05 && std::fabs(on.mean - 1.0) <= std::fabs(off.mean - 1.0);
		say("    (white dots < 0.1 x, RMSE lower, mean within 5 %% of the reference and no further from it than without) -> %s\n", pass ? "ok" : "BAD");
		ok = ok && pass;
		releaseWorld(h, ids);
	}
	// ---- (b) analytic: rare real fireflies, a glint, a thin bright line (the RGB chain)
	{
		Tex noisy, depth, normals;
		const D3DFORMAT F = D3DFMT_A32B32G32R32F;
		if (!makeTex(h, S, S, F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, F, false, normals)) return false;
		const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
		{
			std::vector<float> d(size_t(S) * S, 3000.0f / cam.farZ), n(size_t(S) * S * 4, 0.0f);
			for (UINT i = 0; i < S * S; i++) { n[i * 4] = 0.5f; n[i * 4 + 1] = 0.5f; n[i * 4 + 2] = 0.0f; n[i * 4 + 3] = 1.0f; }
			if (!fillTex(depth, d, 1) || !fillTex(normals, n, 4)) return false;
		}
		struct Res { double dots = 0, mean = 0, glint = 0, line = 0, temporalMean = 0; } res[2];
		for (int mode = 0; mode < 2; mode++) {
			DnChain chain;
			if (!chain.make(h, S, S)) return false;
			chain.antiFirefly = mode ? kFF : 0.0f;
			chain.meanVariance = kMeanVar;
			uint32_t rng = 31337;
			auto rnd = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return float(((x >> 22u) ^ x) >> 8) / 16777216.0f; };
			for (int f = 0; f < 48; f++) {
				std::vector<float> v(size_t(S) * S * 4, 0.0f), out;
				for (UINT p = 0; p < S * S; p++) {
					const UINT i = p % S, j = p / S;
					float x = rnd() < 0.5f ? 1.0f : 0.0f;
					if (rnd() < 0.005f) x += 100.0f;
					if (i == 64 && j == 64) x = 40.0f;   // the glint
					if (i == 100) x = 10.0f;             // the line
					v[p * 4] = v[p * 4 + 1] = v[p * 4 + 2] = x; v[p * 4 + 3] = 1.0f;
				}
				if (!fillTex(noisy, v, 4)) return false;
				if (!chain.run(h, false, noisy.t, depth.t, normals.t, depth.t, depth.t, cam, S, S, f == 0, gIter, 1.0f, true, 3.0f)) return false;
				if (f < 40) continue;
				std::vector<float> temporal;
				if (!readTex(h, chain.out, out, 4) || !readTex(h, chain.oA, temporal, 4)) return false;
				double sum = 0, tsum = 0; int n = 0, dots = 0;
				for (UINT j = 3; j < S - 3; j++) for (UINT i = 3; i < S - 3; i++) {
					if (i >= 96 && i <= 104) continue;
					if (i >= 60 && i <= 68 && j >= 60 && j <= 68) continue;
					const float o = out[(j * S + i) * 4];
					sum += o; tsum += temporal[(j * S + i) * 4]; n++;
					double m = 0; for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) m += out[((j + dy) * S + i + dx) * 4];
					if (o > 3.0 * m / 8.0 + 0.05) dots++;   // a white dot: 3 x its neighbours' mean
				}
				res[mode].dots += dots / 8.0; res[mode].mean += sum / n / 8.0; res[mode].temporalMean += tsum / n / 8.0;
				res[mode].glint += out[(64 * S + 64) * 4] / 8.0;
				double line = 0; for (UINT j = 8; j < S - 8; j++) line += out[(j * S + 100) * 4];
				res[mode].line += line / (S - 16) / 8.0;
			}
			chain.free();
		}
		say("  analytic: background 0.5 + rare real fireflies (true mean 1.0), a glint (40) and a 1-pixel bright line (10), frames 40-47:\n");
		say("    white dots a frame %.1f -> %.1f; background mean %.4f -> %.4f (after the temporal pass %.4f -> %.4f); the glint %.2f -> %.2f; the line %.2f -> %.2f (without -> with)\n",
			res[0].dots, res[1].dots, res[0].mean, res[1].mean, res[0].temporalMean, res[1].temporalMean, res[0].glint, res[1].glint, res[0].line, res[1].line);
		const bool pass = res[1].dots < 0.1 * res[0].dots + 1.0 && std::fabs(res[1].mean - 1.0) < 0.02 &&
			res[1].glint > 0.9 * res[0].glint && res[1].line > 0.9 * res[0].line;
		say("    (white dots < 0.1 x, mean within 2 %% of the true 1.0, glint and line > 90 %% of without) -> %s\n", pass ? "ok" : "BAD");
		ok = ok && pass;
		freeTex({ &noisy, &depth, &normals });
	}
	// ---- (c) the furnace through the chain
	{
		World w;
		Mesh box; const float half = 1000, height = 1500;
		box.quad({ -half, -half, 0 }, { half, -half, 0 }, { half, half, 0 }, { -half, half, 0 });
		box.quad({ -half, -half, 0 }, { -half, half, 0 }, { -half, half, height }, { -half, -half, height });
		box.quad({ half, -half, 0 }, { half, -half, height }, { half, half, height }, { half, half, 0 });
		box.quad({ -half, -half, 0 }, { -half, -half, height }, { half, -half, height }, { half, -half, 0 });
		box.quad({ -half, half, 0 }, { half, half, 0 }, { half, half, height }, { -half, half, height });
		w.meshes.push_back(box);
		Object o; o.mesh = 0; o.albedo = { 0.8f, 0.8f, 0.8f }; o.rough = 0.9f; o.metal = 0.0f;
		w.objects = { o };
		std::vector<uint32_t> ids;
		if (!stageWorld(h, w, ids)) return false;
		const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 2.2f, 2.2f);
		std::vector<GPixel> px;
		if (!ptSplitGBuffer(c, w, cam, px)) return false;
		const V3 sky(0.8f, 0.6f, 0.4f), noSun(0, 0, 0);
		const uint32_t base = 8u | 256u | 128u | 1024u | 1048576u;
		std::vector<float> ref;
		if (!ptReferenceMean(c, w, ids, cam, base, 8, V3(0, 0, 1), noSun, sky, ref)) return false;
		FireflyStats off, on;
		if (!ptSplitChains(c, w, ids, cam, base, 8, V3(0, 0, 1), noSun, sky, px, ref, 0.0f, true, off) ||
			!ptSplitChains(c, w, ids, cam, base, 8, V3(0, 0, 1), noSun, sky, px, ref, kFF, true, on)) return false;
		say("  furnace (open box albedo 0.8, uniform sky, 8 bounces) through the chains: mean / reference %.4f without, %.4f with (RMSE %.4f / %.4f)\n",
			off.mean, on.mean, off.rmse, on.rmse);
		const bool pass = std::fabs(on.mean - off.mean) < 0.01;
		say("    (with = without within 1 %%) -> %s\n", pass ? "ok" : "BAD");
		ok = ok && pass;
		releaseWorld(h, ids);
	}
	return ok;
}
