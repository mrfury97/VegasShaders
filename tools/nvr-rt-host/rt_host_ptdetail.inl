// (2026-11-07) Scene `ptdetail`: the path-traced picture keeps the game's fine shading (testers: "losing a lot of texture quality when RT
// is on": the path tracer shades with smooth normals and its denoiser smooths the light; docs/next/RT-PATH-TRACING.md section 55).
//  (1) RtPathTraceShow.frag PTDetail: flat traced light 1, a surface colour of 0.5, the game's frame = 0.5 x (1 + 0.4 sin(2 pi x / 8)) (a
//      fine shading ripple, 8 pixels a period): with PTDetail 1 the output is 0.5 x (1 + 0.4 sin) (the ripple; the 4 taps 4 pixels out
//      average it to 1 exactly), with 0 a flat 0.5; across a depth edge the taps on the other surface are left out.
//  (2) RtDenoiseAtrousRgb.frag's guide (PTDenoiseGuide): clean light 0.2 left / 1.0 right of a column, and the game's shading with the
//      same edge: without the guide the a-trous blurs the light across it, with the guide the pixel next to the edge keeps its value.
static bool loadFragRaw(Host& h, const char* name, UINT images, UINT push, UINT targets, FragPass& out) {
	FILE* f = nullptr; const std::string file = gM.compute + "\\" + name + ".spv"; fopen_s(&f, file.c_str(), "rb");
	if (!f) { say("  error: no %s\n", file.c_str()); return false; }
	std::vector<char> spirv; char buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
	fclose(f);
	NVR_EXT_BINDING_DESC bindings[12] = {};	// (2026-11-07 ptaaa-denoiser: 12, the show pass has 10 images)
	for (UINT i = 0; i < images; i++) bindings[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
	bindings[images] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, push == 48 && targets == 1 ? 4u : push - 16 };
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
static bool drawRaw(Host& h, const FragPass& p, IDirect3DSurface9* t0, IDirect3DSurface9* t1, IUnknown* const* images, const void* push) {
	NVR_EXT_RESOURCE_BINDING b[12] = {};
	for (UINT i = 0; i < p.images; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = images[i]; }
	b[p.images].Slot = 0; b[p.images].Type = NVR_EXT_BIND_SAMPLER; b[p.images].pResource = gPointClamp;
	NVR_EXT_DRAW_DESC d = {};
	d.pPipeline = p.pipe; d.TargetCount = t1 ? 2 : 1; d.Targets[0].pResource = t0; d.Targets[1].pResource = t1;
	d.BindingCount = p.images + 1; d.pBindings = b; d.pPushData = push; d.PushDataSize = p.push;
	CHECKHR(h.ext->Draw(&d), "Draw");
	return true;
}

static bool scenePtDetail(Ctx& c) {
	Host& h = *c.host;
	bool ok = true;
	// ---- (1) the show pass
	{
		FragPass show;
		if (!loadFragRaw(h, "RtPathTraceShow", 10, 52, 1, show)) return false;	// (2026-11-07 ptaaa-denoiser: + the specular images 6-9, stand-ins here)
		Tex traced, albedo, raw, viewModel, frame, depth, out;
		if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, traced) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, albedo) ||
			!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, raw) || !makeTex(h, S, S, D3DFMT_R32F, false, viewModel) ||
			!makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, frame) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) ||
			!makeTex(h, S, S, D3DFMT_A32B32G32R32F, true, out)) return false;
		const float pi = 3.14159265f;
		std::vector<float> t(S * S * 4, 1.0f), a(S * S * 4, 0.5f), r(S * S * 4, 0.0f), vm(S * S, 0.0f), fr(S * S * 4, 1.0f), dz(S * S, 0.2f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const size_t p = size_t(j) * S + i;
			a[p * 4 + 3] = 1.0f;
			const float shade = 1.0f + 0.4f * std::sin(2.0f * pi * (i + 0.5f) / 8.0f);
			for (int ch = 0; ch < 3; ch++) fr[p * 4 + ch] = std::pow(0.5f * shade, 1.0f / 2.2f);   // the scene as stored
			if (i >= 96) dz[p] = 0.6f;   // another surface from column 96: the taps across it are left out
		}
		if (!fillTex(traced, t, 4) || !fillTex(albedo, a, 4) || !fillTex(raw, r, 4) || !fillTex(viewModel, vm, 1) || !fillTex(frame, fr, 4) || !fillTex(depth, dz, 1)) return false;
		struct { float exposure; uint32_t samplerIndex; float split, gamma; uint32_t modulate; float uvx, uvy; uint32_t keep; float detail, radius, texel[2]; float detailEdge; } push =
			{ 1.0f, 0, 0.0f, 1.0f / 2.2f, 1u | 2u, 1.0f, 1.0f, 0u, 1.0f, 4.0f, { 1.0f / S, 1.0f / S }, 0.0f };	// (2026-11-10: + PTDetailEdge, 0 = off here)
		static_assert(sizeof(push) == 52, "RtPathTraceShow's push block");
		IUnknown* imgs[10] = { traced.t, albedo.t, raw.t, viewModel.t, frame.t, depth.t, traced.t, traced.t, traced.t, traced.t };
		for (int on = 1; on >= 0; on--) {
			push.detail = on ? 1.0f : 0.0f;
			if (!drawRaw(h, show, out.s, nullptr, imgs, &push)) return false;
			std::vector<float> got;
			if (!readTex(h, out, got, 4)) return false;
			double worst = 0; int n = 0;
			for (UINT j = 4; j < S - 4; j++) for (UINT i = 8; i < 88; i++) {   // one surface, the taps all on it
				const float want = on ? 0.5f * (1.0f + 0.4f * std::sin(2.0f * pi * (i + 0.5f) / 8.0f)) : 0.5f;
				const float lin = std::pow(std::max(got[(size_t(j) * S + i) * 4], 0.0f), 2.2f);
				worst = std::max(worst, (double)std::fabs(lin - want)); n++;
			}
			const bool pass = worst < 0.01;
			say("  PTDetail %d: the traced light %s the game's fine shading (%d pixels, worst off by %.4f) -> %s\n", on, on ? "takes" : "keeps without", n, worst, pass ? "ok" : "BAD");
			if (!pass) ok = false;
		}
		for (Tex* x : { &traced, &albedo, &raw, &viewModel, &frame, &depth, &out }) { if (x->sys) x->sys->Release(); x->s->Release(); x->t->Release(); }
	}
	// ---- (2) the a-trous guide
	{
		FragPass atrous;
		if (!loadFragRaw(h, "RtDenoiseAtrousRgb", 5, 48, 2, atrous)) return false;
		Tex inA, normals, inB, guideFrame, guideAlbedo, outA, outB;
		if (!makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, inA) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, normals) ||
			!makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, inB) || !makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, guideFrame) ||
			!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, guideAlbedo) ||
			!makeTex(h, S, S, D3DFMT_A32B32G32R32F, true, outA) || !makeTex(h, S, S, D3DFMT_A32B32G32R32F, true, outB)) return false;
		const UINT edge = 64;
		std::vector<float> a(S * S * 4), nrm(S * S * 4), b(S * S * 4), gf(S * S * 4), ga(S * S * 4);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const size_t p = size_t(j) * S + i;
			const float light = i < edge ? 0.2f : 1.0f;
			a[p * 4] = a[p * 4 + 1] = a[p * 4 + 2] = light; a[p * 4 + 3] = 1.0f;            // one frame of history: the value spread is wide
			nrm[p * 4] = 0.5f; nrm[p * 4 + 1] = 0.5f; nrm[p * 4 + 2] = 1.0f; nrm[p * 4 + 3] = 1.0f;
			b[p * 4] = light * light; b[p * 4 + 1] = 500.0f; b[p * 4 + 2] = b[p * 4 + 3] = 0.0f;
			for (int ch = 0; ch < 3; ch++) { gf[p * 4 + ch] = std::pow(0.5f * light, 1.0f / 2.2f); ga[p * 4 + ch] = 0.5f; }
			gf[p * 4 + 3] = 1.0f; ga[p * 4 + 3] = 1.0f;
		}
		if (!fillTex(inA, a, 4) || !fillTex(normals, nrm, 4) || !fillTex(inB, b, 4) || !fillTex(guideFrame, gf, 4) || !fillTex(guideAlbedo, ga, 4)) return false;
		struct { float params[4], size[4]; uint32_t samplerIndex; float guideSigma; uint32_t pad[2]; } push = {};
		static_assert(sizeof(push) == 48, "RtDenoise's a-trous push block");
		put4(push.params, 2.0f, 1.0f, 64.0f, 4.0f); put4(push.size, 1.0f / S, 1.0f / S, 0.0f, 0.0f);
		IUnknown* imgs[5] = { inA.t, normals.t, inB.t, guideFrame.t, guideAlbedo.t };
		float leftOf[2] = {};
		for (int g = 0; g < 2; g++) {
			push.guideSigma = g ? 0.35f : 0.0f;
			if (!drawRaw(h, atrous, outA.s, outB.s, imgs, &push)) return false;
			std::vector<float> got;
			if (!readTex(h, outA, got, 4)) return false;
			double m = 0; int n = 0;
			for (UINT j = 8; j < S - 8; j++) { m += got[(size_t(j) * S + edge - 1) * 4]; n++; }
			leftOf[g] = float(m / n);
		}
		const bool pass = leftOf[0] > 0.24f && leftOf[1] < 0.21f;
		say("  a-trous next to a light edge the game's shading also shows: %.3f without the guide (blurred, > 0.24), %.3f with it (< 0.21; true 0.2) -> %s\n",
			leftOf[0], leftOf[1], pass ? "ok" : "BAD");
		if (!pass) ok = false;
		for (Tex* x : { &inA, &normals, &inB, &guideFrame, &guideAlbedo, &outA, &outB }) { if (x->sys) x->sys->Release(); x->s->Release(); x->t->Release(); }
	}
	return ok;
}
