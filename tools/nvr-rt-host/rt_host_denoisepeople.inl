// (2026-11-07) Scene `denoisepeople`: RtDenoise's RGB temporal pass (RtDenoiseTemporalRgb.spv, the path tracer's denoiser) with
// Settings.PeopleHistory: people's pixels (TESR_CapturedNormalsBuffer alpha > 1.5) keep at most PeopleHistory frames of history.
//
// A tester's video (PTRealtime, dialogue): the NPC's mouth and the hat's shadow trailed behind as he talked; a face changes shape with no
// motion the reprojection knows of, and the path tracer's denoiser kept 32 frames there. Here a still camera over a floor, a history of
// 32 frames of black, a new frame of white, the captured normals marking the left half as a person's: with PeopleHistory 4 the left
// half must blend by 1 / 5 (history capped to 4, + this frame) and the right half by 1 / 32 (MaxHistory); with 0 both by 1 / 32.
static bool sceneDenoisePeople(Ctx& c) {
	Host& h = *c.host;
	// the pass with its two targets (as RtDenoise.cpp Make("RtDenoiseTemporalRgb", 7, 192, 2))
	FragPass pass;
	{
		FILE* f = nullptr; const std::string file = gM.compute + "\\RtDenoiseTemporalRgb.spv"; fopen_s(&f, file.c_str(), "rb");
		if (!f) { say("  error: no %s\n", file.c_str()); return false; }
		std::vector<char> spirv; char buf[65536]; size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
		fclose(f);
		NVR_EXT_BINDING_DESC bindings[8] = {};
		for (UINT i = 0; i < 7; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		bindings[7] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 176 };
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data(); desc.FragmentSpirvSize = UINT(spirv.size());
		desc.BindingCount = 8; desc.pBindings = bindings; desc.PushConstantSize = 200;	// (ptaaa-denoiser: + fireflyK; perf-history: + fastFrames)
		desc.UsesSamplerHeap = TRUE; desc.SamplerHeapSet = 0; desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = 2; desc.Blend = NVR_EXT_BLEND_NONE; desc.ColorWriteMask = 0xF; desc.pDebugName = "RtDenoiseTemporalRgb";
		CHECKHR(h.ext->CreateGraphicsPipeline(&desc, &pass.pipe), "CreateGraphicsPipeline");
		pass.images = 7; pass.push = 200;
		if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	}
	World w;
	Mesh floor; floor.quad({ -8000, -8000, 0 }, { 8000, -8000, 0 }, { 8000, 8000, 0 }, { -8000, 8000, 0 });
	w.meshes = { floor };
	Object f; f.mesh = 0; w.objects = { f };
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	std::vector<GPixel> px; std::vector<float> dep, nor;
	gbuffer(w, cam, px, dep, nor);
	Tex noisy, depth, histA, histB, captured, outA, outB;
	if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, histA) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, histB) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, captured) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, outA) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, outB)) return false;
	std::vector<float> white(size_t(S) * S * 4, 1.0f), black32(size_t(S) * S * 4, 0.0f), hb(size_t(S) * S * 4, 0.0f), cap(size_t(S) * S * 4, 0.0f);
	for (UINT i = 0; i < S * S; i++) {
		black32[i * 4 + 3] = 32.0f;                          // 32 frames of history, black
		hb[i * 4 + 1] = dep[i] * cam.farZ;                   // the history's view depth (the same surface)
		cap[i * 4 + 3] = (i % S) < S / 2 ? 2.0f : 1.0f;      // the left half: a person's pixel (alpha 2); the right: a lit surface (1)
	}
	if (!fillTex(noisy, white, 4) || !fillTex(depth, dep, 1) || !fillTex(histA, black32, 4) || !fillTex(histB, hb, 4) || !fillTex(captured, cap, 4)) return false;
	struct Push { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4]; uint32_t samplerIndex; float clampK, peopleCap, antiLag, fireflyK, fastFrames; };
	static_assert(sizeof(Push) == 200, "RtDenoise.cpp's push block");
	bool ok = true;
	for (float people : { 4.0f, 0.0f }) {
		Push t = {};
		const V3 ax = cam.r * cam.p00, ay = cam.u * cam.p11, aw = cam.f;   // RtDenoise's camera-relative view-projection (clip = M * p)
		for (int col = 0; col < 3; col++) { t.prevViewProj[col * 4 + 0] = (&ax.x)[col]; t.prevViewProj[col * 4 + 1] = (&ay.x)[col]; t.prevViewProj[col * 4 + 3] = (&aw.x)[col]; }
		put4(t.forward, cam.f, cam.farZ); put4(t.right, cam.r * (1.0f / cam.p00), 0); put4(t.up, cam.u * (1.0f / cam.p11), 0);
		put4(t.move, V3(0, 0, 0), 0.0f);
		put4(t.params, 32.0f, 0.03f, 1.0f / S, 1.0f / S);
		put4(t.rect, 1, 1, 1, 1);
		t.peopleCap = people;
		IUnknown* images[7] = { noisy.t, histA.t, depth.t, histB.t, depth.t, depth.t, captured.t };
		NVR_EXT_RESOURCE_BINDING b[8] = {};
		for (UINT i = 0; i < 7; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = images[i]; }
		b[7].Slot = 0; b[7].Type = NVR_EXT_BIND_SAMPLER; b[7].pResource = gPointClamp;
		NVR_EXT_DRAW_DESC d = {};
		d.pPipeline = pass.pipe; d.TargetCount = 2; d.Targets[0].pResource = outA.s; d.Targets[1].pResource = outB.s;
		d.BindingCount = 8; d.pBindings = b; d.pPushData = &t; d.PushDataSize = 200;
		CHECKHR(h.ext->Draw(&d), "Draw");
		std::vector<float> out;
		if (!readTex(h, outA, out, 4)) return false;
		int checked = 0, bad = 0, shown = 0;
		for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) {
			if (!px[j * S + i].valid || i == S / 2 - 1 || i == S / 2) continue;
			const bool person = i < S / 2;
			const float want = person && people > 0.0f ? 1.0f / 5.0f : 1.0f / 32.0f, wantLen = person && people > 0.0f ? 5.0f : 32.0f;
			const float* o = &out[(j * S + i) * 4];
			checked++;
			if (std::fabs(o[0] - want) > 2e-3f || std::fabs(o[3] - wantLen) > 1e-2f) { bad++; if (shown++ < 4) say("    pixel (%u, %u) %s: %.4f (length %.2f), want %.4f (%.0f)\n", i, j, person ? "person" : "surface", o[0], o[3], want, wantLen); }
		}
		const bool pass1 = bad == 0 && checked > int(S * S) / 2;
		say("  RGB temporal, PeopleHistory %.0f: person pixels blend %s, others 1/32; %d pixels checked, %d wrong -> %s\n", people,
			people > 0.0f ? "1/5 (history capped to 4)" : "1/32 (no cap)", checked, bad, pass1 ? "ok" : "BAD");
		if (!pass1) ok = false;
	}
	// (2026-11-07 ptaaa-firstperson) the first-person view model (RtPathTrace.cpp ViewModelPrep's mask: alpha 4 + s, Settings.People) moves
	// with the camera: its history must be read at the SAME screen point. The camera has moved 300 units sideways since the history (a
	// world point reprojects ~6 pixels over); the history is a column gradient, the new frame white, the left half marked: as the view model
	// (4.45) each marked pixel must give 0.8 x its own column's history + 0.2 (PeopleHistory 4 caps it to 4 frames); as a person (2) the
	// shifted column's (the control: the test sees the difference)
	{
		std::vector<float> grad(size_t(S) * S * 4, 0.0f);
		for (UINT i = 0; i < S * S; i++) { const float g = float(i % S) / S; grad[i * 4] = grad[i * 4 + 1] = grad[i * 4 + 2] = g; grad[i * 4 + 3] = 32.0f; }
		if (!fillTex(histA, grad, 4)) return false;
		for (float mark : { 4.45f, 2.0f }) {
			for (UINT i = 0; i < S * S; i++) cap[i * 4 + 3] = (i % S) < S / 2 ? mark : 1.0f;
			if (!fillTex(captured, cap, 4)) return false;
			Push t = {};
			const V3 ax = cam.r * cam.p00, ay = cam.u * cam.p11, aw = cam.f;
			for (int col = 0; col < 3; col++) { t.prevViewProj[col * 4 + 0] = (&ax.x)[col]; t.prevViewProj[col * 4 + 1] = (&ay.x)[col]; t.prevViewProj[col * 4 + 3] = (&aw.x)[col]; }
			put4(t.forward, cam.f, cam.farZ); put4(t.right, cam.r * (1.0f / cam.p00), 0); put4(t.up, cam.u * (1.0f / cam.p11), 0);
			put4(t.move, cam.r * 300.0f, 0.0f);
			put4(t.params, 32.0f, 0.03f, 1.0f / S, 1.0f / S);
			put4(t.rect, 1, 1, 1, 1);
			t.peopleCap = 4.0f;
			IUnknown* images[7] = { noisy.t, histA.t, depth.t, histB.t, depth.t, depth.t, captured.t };
			NVR_EXT_RESOURCE_BINDING b[8] = {};
			for (UINT i = 0; i < 7; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = images[i]; }
			b[7].Slot = 0; b[7].Type = NVR_EXT_BIND_SAMPLER; b[7].pResource = gPointClamp;
			NVR_EXT_DRAW_DESC d = {};
			d.pPipeline = pass.pipe; d.TargetCount = 2; d.Targets[0].pResource = outA.s; d.Targets[1].pResource = outB.s;
			d.BindingCount = 8; d.pBindings = b; d.pPushData = &t; d.PushDataSize = 200;
			CHECKHR(h.ext->Draw(&d), "Draw");
			std::vector<float> out;
			if (!readTex(h, outA, out, 4)) return false;
			int checked = 0, same = 0;
			for (UINT j = 2; j < S - 2; j++) for (UINT i = 8; i < S / 2 - 1; i++) {
				if (!px[j * S + i].valid) continue;
				const float want = 0.8f * (float(i) / S) + 0.2f;
				checked++;
				if (std::fabs(out[(j * S + i) * 4] - want) < 2e-3f) same++;
			}
			const bool vmMark = mark > 3.5f;
			const bool pass2 = checked > int(S * S) / 4 && (vmMark ? same == checked : same < checked / 10);
			say("  RGB temporal, the camera 300 units aside: marked %s (%.2f): %d of %d marked pixels take their own screen point's history -> %s\n",
				vmMark ? "the view model" : "a person (control)", mark, same, checked, pass2 ? "ok" : "BAD");
			if (!pass2) ok = false;
		}
	}
	return ok;
}


// (2026-11-07) Scene `denoiseantilag`: RtDenoiseTemporalRgb's AntiLag (PTDenoiseAntiLag; NRD ReBLUR-style fast history beside the long one).
// Testers: "a lot of ghosting" under PTRealtime (32 frames of history, the camera's reprojection only). Frame by frame through the real pass,
// the history fed back: (1) a step: 32 frames of black, then white every frame: with AntiLag 3 the output must be most of the way after 6
// frames (> 0.6; without it ~0.17); (2) a still, noisy pixel (each frame 1 with probability 0.3, else 0) over 64 frames from nothing: the
// mean of the last 16 frames within 0.01 of 0.3 with and without, and AntiLag no noisier (spread over the pixels within 15 %).
static bool sceneDenoiseAntiLag(Ctx& c) {
	Host& h = *c.host;
	FragPass pass;
	{
		FILE* f = nullptr; const std::string file = gM.compute + "\\RtDenoiseTemporalRgb.spv"; fopen_s(&f, file.c_str(), "rb");
		if (!f) { say("  error: no %s\n", file.c_str()); return false; }
		std::vector<char> spirv; char buf[65536]; size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
		fclose(f);
		NVR_EXT_BINDING_DESC bindings[8] = {};
		for (UINT i = 0; i < 7; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		bindings[7] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 176 };
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data(); desc.FragmentSpirvSize = UINT(spirv.size());
		desc.BindingCount = 8; desc.pBindings = bindings; desc.PushConstantSize = 200;	// (ptaaa-denoiser: + fireflyK; perf-history: + fastFrames)
		desc.UsesSamplerHeap = TRUE; desc.SamplerHeapSet = 0; desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = 2; desc.Blend = NVR_EXT_BLEND_NONE; desc.ColorWriteMask = 0xF; desc.pDebugName = "RtDenoiseTemporalRgb";
		CHECKHR(h.ext->CreateGraphicsPipeline(&desc, &pass.pipe), "CreateGraphicsPipeline");
		pass.images = 7; pass.push = 200;
		if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	}
	World w;
	Mesh floor; floor.quad({ -8000, -8000, 0 }, { 8000, -8000, 0 }, { 8000, 8000, 0 }, { -8000, 8000, 0 });
	w.meshes = { floor };
	Object fo; fo.mesh = 0; w.objects = { fo };
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	std::vector<GPixel> px; std::vector<float> dep, nor;
	gbuffer(w, cam, px, dep, nor);
	Tex noisy, depth, histA, histB, captured, outA, outB;
	if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) ||
		!makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, histA) || !makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, histB) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, captured) ||
		!makeTex(h, S, S, D3DFMT_A32B32G32R32F, true, outA) || !makeTex(h, S, S, D3DFMT_A32B32G32R32F, true, outB)) return false;
	if (!fillTex(depth, dep, 1)) return false;
	{ std::vector<float> z(size_t(S) * S * 4, 0.0f); if (!fillTex(captured, z, 4)) return false; }
	struct Push { float prevViewProj[16], forward[4], right[4], up[4], move[4], params[4], backbone[4], rect[4]; uint32_t samplerIndex; float clampK, peopleCap, antiLag, fireflyK, fastFrames; };
	static_assert(sizeof(Push) == 200, "the RtDenoise push block");
	uint32_t rng = 777;
	auto rnd = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return float(((x >> 22u) ^ x) >> 8) / 16777216.0f; };
	// one frame through the pass: noisy in, history (a, b) in, the output read back as the next history
	auto frame = [&](const std::vector<float>& value, std::vector<float>& a, std::vector<float>& b, float antiLag) -> bool {
		if (!fillTex(noisy, value, 4) || !fillTex(histA, a, 4) || !fillTex(histB, b, 4)) return false;
		Push t = {};
		const V3 ax = cam.r * cam.p00, ay = cam.u * cam.p11, aw = cam.f;
		for (int col = 0; col < 3; col++) { t.prevViewProj[col * 4 + 0] = (&ax.x)[col]; t.prevViewProj[col * 4 + 1] = (&ay.x)[col]; t.prevViewProj[col * 4 + 3] = (&aw.x)[col]; }
		put4(t.forward, cam.f, cam.farZ); put4(t.right, cam.r * (1.0f / cam.p00), 0); put4(t.up, cam.u * (1.0f / cam.p11), 0);
		put4(t.move, V3(0, 0, 0), 0.0f);
		put4(t.params, 32.0f, 0.03f, 1.0f / S, 1.0f / S);
		put4(t.rect, 1, 1, 1, 1);
		t.antiLag = antiLag;
		IUnknown* images[7] = { noisy.t, histA.t, depth.t, histB.t, depth.t, depth.t, captured.t };
		NVR_EXT_RESOURCE_BINDING bd[8] = {};
		for (UINT i = 0; i < 7; i++) { bd[i].Slot = i; bd[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bd[i].pResource = images[i]; }
		bd[7].Slot = 0; bd[7].Type = NVR_EXT_BIND_SAMPLER; bd[7].pResource = gPointClamp;
		NVR_EXT_DRAW_DESC d = {};
		d.pPipeline = pass.pipe; d.TargetCount = 2; d.Targets[0].pResource = outA.s; d.Targets[1].pResource = outB.s;
		d.BindingCount = 8; d.pBindings = bd; d.pPushData = &t; d.PushDataSize = 200;
		CHECKHR(h.ext->Draw(&d), "Draw");
		return readTex(h, outA, a, 4) && readTex(h, outB, b, 4);
	};
	auto meanOf = [&](const std::vector<float>& a, double* spread) {
		double s1 = 0, s2 = 0; int n = 0;
		for (UINT j = 2; j < S - 2; j++) for (UINT i = 2; i < S - 2; i++) { if (!px[j * S + i].valid) continue; const double v = a[(j * S + i) * 4]; s1 += v; s2 += v * v; n++; }
		const double m = n ? s1 / n : 0; if (spread) *spread = n ? std::sqrt(std::max(s2 / n - m * m, 0.0)) : 0; return m;
	};
	double step[2] = {}, stillMean[2] = {}, stillSpread[2] = {};
	for (int mode = 0; mode < 2; mode++) {
		const float antiLag = mode ? 3.0f : 0.0f;
		// (1) the step
		std::vector<float> a(size_t(S) * S * 4, 0.0f), b(size_t(S) * S * 4, 0.0f), white(size_t(S) * S * 4, 1.0f);
		for (UINT i = 0; i < S * S; i++) { a[i * 4 + 3] = 32.0f; b[i * 4 + 1] = dep[i] * cam.farZ; }
		for (int f = 0; f < 6; f++) if (!frame(white, a, b, antiLag)) return false;
		step[mode] = meanOf(a, nullptr);
		// (2) the still, noisy pixel
		std::fill(a.begin(), a.end(), 0.0f); std::fill(b.begin(), b.end(), 0.0f);
		for (UINT i = 0; i < S * S; i++) b[i * 4 + 1] = dep[i] * cam.farZ;
		double m = 0, sp = 0;
		for (int f = 0; f < 64; f++) {
			std::vector<float> v(size_t(S) * S * 4, 0.0f);
			for (UINT i = 0; i < S * S; i++) { const float x = rnd() < 0.3f ? 1.0f : 0.0f; v[i * 4] = v[i * 4 + 1] = v[i * 4 + 2] = x; v[i * 4 + 3] = 1.0f; }
			if (!frame(v, a, b, antiLag)) return false;
			if (f >= 48) { double s1; m += meanOf(a, &s1) / 16.0; sp += s1 / 16.0; }
		}
		stillMean[mode] = m; stillSpread[mode] = sp;
	}
	const bool stepOk = step[1] > 0.6 && step[0] < 0.3;
	const bool biasOk = std::fabs(stillMean[0] - 0.3) < 0.01 && std::fabs(stillMean[1] - 0.3) < 0.01;
	const bool noiseOk = stillSpread[1] <= stillSpread[0] * 1.15;
	say("  AntiLag step 0 -> 1 after 6 frames: %.3f with it, %.3f without (need > 0.6, < 0.3) -> %s\n", step[1], step[0], stepOk ? "ok" : "BAD");
	say("  AntiLag still noisy pixel (p 0.3), frames 48-63: mean %.4f with, %.4f without; spread %.4f with, %.4f without -> %s\n",
		stillMean[1], stillMean[0], stillSpread[1], stillSpread[0], biasOk && noiseOk ? "ok" : "BAD");
	for (Tex* t : { &noisy, &depth, &histA, &histB, &captured, &outA, &outB }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	return stepOk && biasOk && noiseOk;
}
