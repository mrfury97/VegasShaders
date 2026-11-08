// (2026-11-07 perf-rr) Scene `rrrect`: DLSS Ray Reconstruction's own passes at RenderScale < 1, on the fork's device, no NGX (any GPU):
// what TAAFSR.cpp RayReconstruct runs in a RenderScale v2 rect frame before and after DLSS-D (core/RrRectPlan.h has the plan).
//  1. DlssRRGuides.comp dispatched one thread per RENDER pixel (224 x 128 of a 320 x 180 screen) on screen-size inputs that hold the frame in
//     their top-left rect and a sentinel everywhere else: every rect pixel's five guides must equal the CPU's (the decode, the view-space
//     normal turned to world space, the material's roughness, the path tracer's albedo, Karis' specular fit with the view ray at
//     uv = (pixel + 0.5) / render size: the control with uv over the SCREEN size must differ), every pixel outside the rect untouched.
//  2. DlssRRComposite.frag (rect = 1) over the whole screen: RR's output (screen size) encoded at every pixel, the frame before RR read in the
//     rect at the screen pixel's centre x scale: the alpha from the nearest rect texel, the weapon (TESR_DepthBufferViewModel > 0 there) as the
//     rect's bilinear stretch. Then rect = 0 on screen-size inputs: the native composite, texel for texel, as before.
// What DLSS-D itself does with them (and the helper's subrect on an RTX card) is not tested here.
#include "RrRectPlan.h"

namespace rrrect {
	bool loadSpv(const std::string& file, std::vector<char>& spirv) {
		FILE* f = nullptr; fopen_s(&f, file.c_str(), "rb");
		if (!f) { say("  error: no %s\n", file.c_str()); return false; }
		char buf[65536]; size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
		fclose(f);
		return true;
	}
	NVR_EXT_RESOURCE_BINDING image(UINT slot, IUnknown* r, bool storage = false) {
		NVR_EXT_RESOURCE_BINDING b = {}; b.Slot = slot; b.Type = storage ? NVR_EXT_BIND_STORAGE_IMAGE : NVR_EXT_BIND_SAMPLED_IMAGE; b.pResource = r; return b;
	}
	float saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
	// DlssRRGuides.comp EnvBRDFApprox (Karis), a dielectric f0
	float envBrdf(float f0, float roughness, float NdotV) {
		const float c0[4] = { -1.0f, -0.0275f, -0.572f, 0.022f }, c1[4] = { 1.0f, 0.0425f, 1.04f, -0.04f };
		float r[4]; for (int i = 0; i < 4; i++) r[i] = roughness * c0[i] + c1[i];
		const float a004 = std::fmin(r[0] * r[0], std::exp2(-9.28f * NdotV)) * r[0] + r[1];
		const float ax = -1.04f * a004 + r[2], ay = 1.04f * a004 + r[3];
		return f0 * ax + ay;
	}
}

static bool sceneRrRect(Ctx& c) {
	using namespace rrrect;
	Host& h = *c.host;
	const UINT SW = 320, SH = 180;
	const UINT RW = RrRect::SnapRenderSize(SW, 0.7f), RH = RrRect::SnapRenderSize(SH, 0.7f);
	const RrRect::Plan plan = RrRect::Make(true, SW, SH, RW, RH, true, 0.25f, -0.125f, RrRect::JitterPhases(SW, RW), false, 0, 0, 8);
	say("  screen %ux%u, render rect %ux%u (RenderScale 0.70), guides %ux%u groups; helper subrect check: %s\n", SW, SH, RW, RH, plan.groupsX, plan.groupsY,
		RrRect::Problem(plan) ? RrRect::Problem(plan) : "ok");
	if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }

	// --- the two pipelines, as TAAFSR.cpp makes them (RRGuidesBindings / RRCompositeBindings) ---
	ID3D9NvrExtPipeline* guides = nullptr;
	ID3D9NvrExtPipeline* composite = nullptr;
	{
		std::vector<char> spirv;
		if (!loadSpv(gM.compute + "\\DlssRRGuides.spv", spirv)) return false;
		NVR_EXT_BINDING_DESC b[15] = {};
		for (UINT i = 0; i < 4; i++) b[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		for (UINT i = 4; i < 9; i++) b[i] = { 1, i, NVR_EXT_BIND_STORAGE_IMAGE, i, NVR_EXT_VIEW_2D, TRUE, 0 };
		b[9] = { 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
		b[10] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
		for (UINT i = 9; i < 13; i++) b[i + 2] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		NVR_EXT_COMPUTE_PIPELINE_DESC d = {};
		d.pSpirv = spirv.data(); d.SpirvSize = UINT(spirv.size()); d.BindingCount = 15; d.pBindings = b; d.PushConstantSize = 4;
		d.UsesSamplerHeap = TRUE; d.SamplerHeapSet = 0; d.SamplerHeapBinding = 0; d.pDebugName = "DlssRRGuides";
		CHECKHR(h.ext->CreateComputePipeline(&d, &guides), "CreateComputePipeline DlssRRGuides");
	}
	{
		std::vector<char> spirv;
		if (!loadSpv(gM.compute + "\\DlssRRComposite.spv", spirv)) return false;
		NVR_EXT_BINDING_DESC b[4] = {};
		for (UINT i = 0; i < 3; i++) b[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
		b[3] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
		NVR_EXT_GRAPHICS_PIPELINE_DESC d = {};
		d.pFragmentSpirv = spirv.data(); d.FragmentSpirvSize = UINT(spirv.size()); d.BindingCount = 4; d.pBindings = b; d.PushConstantSize = 32;
		d.UsesSamplerHeap = TRUE; d.SamplerHeapSet = 0; d.SamplerHeapBinding = 0;
		d.ColorTargetCount = 1; d.Blend = NVR_EXT_BLEND_NONE; d.ColorWriteMask = 0xF; d.pDebugName = "DlssRRComposite";
		CHECKHR(h.ext->CreateGraphicsPipeline(&d, &composite), "CreateGraphicsPipeline DlssRRComposite");
	}

	// --- 1. the guides at the render size ---
	const float lin = 2.2f;
	auto inRect = [&](UINT x, UINT y) { return x < RW && y < RH; };
	std::vector<float> color(SW * SH * 4), normals(SW * SH * 4), depth(SW * SH), material(SW * SH * 4), albedo(SW * SH * 4), ptAlbedo(SW * SH * 4);
	for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
		const size_t i = size_t(y) * SW + x;
		float* cl = &color[i * 4]; float* nr = &normals[i * 4]; float* mt = &material[i * 4]; float* al = &albedo[i * 4]; float* pa = &ptAlbedo[i * 4];
		if (inRect(x, y)) {
			cl[0] = 0.2f + 0.5f * x / RW; cl[1] = 0.3f; cl[2] = 0.6f * y / RH; cl[3] = 1.0f;
			const float sx = (float)x / RW - 0.5f, len = std::sqrt(sx * sx + 1.0f);
			nr[0] = sx / len * 0.5f + 0.5f; nr[1] = 0.5f; nr[2] = -1.0f / len * 0.5f + 0.5f; nr[3] = 1.0f;   // view space, facing the camera
			depth[i] = x == RW - 1 ? 1.0f : 0.5f;                                                        // the last rect column: sky
			mt[0] = 1.0f; mt[1] = 0.0f; mt[2] = 0.25f + 0.5f * y / RH; mt[3] = 1.0f;                     // class 1, metal 0, roughness
			al[0] = 0.7f; al[1] = 0.2f; al[2] = 0.1f; al[3] = 1.0f;
			pa[0] = 0.1f; pa[1] = 0.8f; pa[2] = 0.3f; pa[3] = 1.0f;                                      // the path tracer's first surface (metal 0)
		}
		else {	// sentinels: a guide pass that read outside the rect would show them
			cl[0] = cl[1] = cl[2] = 50.0f; cl[3] = 1.0f;
			nr[0] = 1.0f; nr[1] = 1.0f; nr[2] = 1.0f; nr[3] = 1.0f;
			depth[i] = 0.25f;
			mt[0] = 1.0f; mt[1] = 0.0f; mt[2] = 0.9f; mt[3] = 1.0f;
			al[0] = 0.0f; al[1] = 0.0f; al[2] = 1.0f; al[3] = 1.0f;
			pa[0] = 1.0f; pa[1] = 0.0f; pa[2] = 1.0f; pa[3] = 1.0f;
		}
	}
	Tex tColor, tNormals, tDepth, tMaterial, tAlbedo, tPt, oColor, oDiffuse, oSpecular, oNormals, oRough;
	if (!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tColor) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tNormals) ||
		!makeTex(h, SW, SH, D3DFMT_R32F, false, tDepth) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tMaterial) ||
		!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tAlbedo) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tPt) ||
		!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oColor) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oDiffuse) ||
		!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oSpecular) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oNormals) ||
		!makeTex(h, SW, SH, D3DFMT_R16F, true, oRough)) return false;
	if (!fillTex(tColor, color, 4) || !fillTex(tNormals, normals, 4) || !fillTex(tDepth, depth, 1) || !fillTex(tMaterial, material, 4) ||
		!fillTex(tAlbedo, albedo, 4) || !fillTex(tPt, ptAlbedo, 4)) return false;
	const D3DCOLOR sentinel = D3DCOLOR_ARGB(255, 255, 0, 255);   // (1, 0, 1, 1): what an output pixel outside the rect must keep
	for (Tex* t : { &oColor, &oDiffuse, &oSpecular, &oNormals, &oRough }) CHECKHR(h.dev->ColorFill(t->s, nullptr, sentinel), "ColorFill");
	// the camera's axes in world space: right +x, up +z, forward +y; a 90 x 59 degree frustum (1 / proj._11 = 1, 1 / proj._22 = 0.5625)
	struct { float right[4], up[4], forward[4], view[4], options[4], size[4]; } cb = {
		{ 1, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, 1, 0, 0 }, { 1.0f, 0.5625f, lin, 0.6f }, { 2.0f, 0.0f, 1.0f, 1.0f }, { (float)plan.renderW, (float)plan.renderH, 0, 0 } };
	ID3D9NvrExtBuffer* buffer = nullptr;
	{
		NVR_EXT_BUFFER_DESC bd = { UINT(sizeof(cb)), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "rrrect guides constants" };
		CHECKHR(h.ext->CreateBuffer(&bd, &buffer), "CreateBuffer");
		CHECKHR(h.ext->UpdateBuffer(buffer, 0, sizeof(cb), &cb), "UpdateBuffer");
	}
	{
		NVR_EXT_RESOURCE_BINDING b[15] = { image(0, tColor.t), image(1, tNormals.t), image(2, tDepth.t), image(3, tMaterial.t),
			image(4, oColor.t, true), image(5, oDiffuse.t, true), image(6, oSpecular.t, true), image(7, oNormals.t, true), image(8, oRough.t, true) };
		b[9].Slot = 0; b[9].Type = NVR_EXT_BIND_UNIFORM_BUFFER; b[9].pResource = buffer;
		b[10].Slot = 0; b[10].Type = NVR_EXT_BIND_SAMPLER; b[10].pResource = gPointClamp;
		b[11] = image(9, tAlbedo.t); b[12] = image(10, tPt.t); b[13] = image(11, tDepth.t); b[14] = image(12, tDepth.t);
		uint32_t push = 0;
		NVR_EXT_DISPATCH_DESC d = {};
		d.pPipeline = guides; d.BindingCount = 15; d.pBindings = b; d.pPushData = &push; d.PushDataSize = 4;
		d.GroupsX = plan.groupsX; d.GroupsY = plan.groupsY; d.GroupsZ = 1;
		CHECKHR(h.ext->Dispatch(&d), "Dispatch DlssRRGuides");
	}
	std::vector<float> rc, rd, rs, rn, rr;
	if (!readTex(h, oColor, rc, 4) || !readTex(h, oDiffuse, rd, 4) || !readTex(h, oSpecular, rs, 4) || !readTex(h, oNormals, rn, 4) || !readTex(h, oRough, rr, 1)) return false;
	bool ok = true;
	{
		int bad = 0, outsideBad = 0, shown = 0;
		float worstRight = 0.0f, worstWrong = 0.0f;
		for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
			const size_t i = size_t(y) * SW + x;
			if (!inRect(x, y)) {
				const bool kept = std::fabs(rc[i * 4] - 1.0f) < 1e-3f && std::fabs(rc[i * 4 + 1]) < 1e-3f && std::fabs(rd[i * 4 + 2] - 1.0f) < 1e-3f &&
					std::fabs(rn[i * 4 + 1]) < 1e-3f && std::fabs(rr[i] - 1.0f) < 1e-3f;
				if (!kept) { outsideBad++; if (shown++ < 4) say("    outside the rect (%u, %u) written: colour %.3f %.3f\n", x, y, rc[i * 4], rc[i * 4 + 1]); }
				continue;
			}
			const bool sky = x == RW - 1;
			const float* cl = &color[i * 4];
			const float roughness = sky ? 1.0f : material[i * 4 + 2];
			const float sx = (float)x / RW - 0.5f, len = std::sqrt(sx * sx + 1.0f);
			float n[3] = { sx / len, 0.0f, -1.0f / len };
			// the view ray: uv over the RENDER size (right) and, as the control, over the screen size (what a pass dispatched at render size
			// with the screen's size in its constants would use)
			auto toCamera = [&](float u, float v, float* t) {
				float a = (2.0f * u - 1.0f) * 1.0f, b = (1.0f - 2.0f * v) * 0.5625f, l = std::sqrt(a * a + b * b + 1.0f);
				t[0] = -a / l; t[1] = -b / l; t[2] = -1.0f / l;
			};
			float tc[3], tw[3];
			toCamera((x + 0.5f) / RW, (y + 0.5f) / RH, tc);
			toCamera((x + 0.5f) / SW, (y + 0.5f) / SH, tw);
			if (sky) { n[0] = tc[0]; n[1] = tc[1]; n[2] = tc[2]; }
			const float spec = saturate(envBrdf(0.04f, roughness, saturate(n[0] * tc[0] + n[1] * tc[1] + n[2] * tc[2])));
			const float specWrong = saturate(envBrdf(0.04f, roughness, saturate(n[0] * tw[0] + n[1] * tw[1] + n[2] * tw[2])));
			worstRight = std::fmax(worstRight, std::fabs(rs[i * 4] - spec));
			if (!sky) worstWrong = std::fmax(worstWrong, std::fabs(spec - specWrong));
			const float wantColor[3] = { std::pow(cl[0], lin), std::pow(cl[1], lin), std::pow(cl[2], lin) };
			const float wantDiffuse[3] = { sky ? 0.7f : 0.1f, sky ? 0.2f : 0.8f, sky ? 0.1f : 0.3f };   // the sky keeps the chroma / albedo stand-in: checked off-sky only
			const float world[3] = { n[0], n[2], n[1] };   // n.x right + n.y up + n.z forward with right +x, up +z, forward +y
			bool good = true;
			for (int k = 0; k < 3; k++) {
				good = good && std::fabs(rc[i * 4 + k] - wantColor[k]) < 2e-3f + 2e-3f * wantColor[k];
				if (!sky) good = good && std::fabs(rd[i * 4 + k] - wantDiffuse[k]) < 2e-3f;
				good = good && std::fabs(rn[i * 4 + k] - world[k]) < 3e-3f;
			}
			good = good && std::fabs(rn[i * 4 + 3] - roughness) < 2e-3f && std::fabs(rr[i] - roughness) < 2e-3f && std::fabs(rs[i * 4] - spec) < 3e-3f;
			if (!good) { bad++; if (shown++ < 6) say("    rect pixel (%u, %u): colour %.4f (want %.4f), diffuse %.3f (%.3f), normal %.3f %.3f %.3f (%.3f %.3f %.3f), rough %.3f (%.3f), spec %.4f (%.4f)\n",
				x, y, rc[i * 4], wantColor[0], rd[i * 4], wantDiffuse[0], rn[i * 4], rn[i * 4 + 1], rn[i * 4 + 2], world[0], world[1], world[2], rr[i], roughness, rs[i * 4], spec); }
		}
		const bool control = worstWrong > 10.0f * (worstRight + 1e-4f);
		const bool pass = bad == 0 && outsideBad == 0 && control;
		say("  guides at the render size: %u rect pixels, %d wrong; %u outside, %d written; specular vs the CPU (render-size view ray) worst %.5f, the screen-size "
			"view ray (control) would be off by up to %.4f -> %s\n", RW * RH, bad, SW * SH - RW * RH, outsideBad, worstRight, worstWrong, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	}

	// --- 2. the composite over the whole screen ---
	std::vector<float> rrOut(SW * SH * 4), source(SW * SH * 4), viewModel(SW * SH);
	for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
		const size_t i = size_t(y) * SW + x;
		rrOut[i * 4] = (float)x / SW; rrOut[i * 4 + 1] = (float)y / SH; rrOut[i * 4 + 2] = 0.5f; rrOut[i * 4 + 3] = 1.0f;
		if (inRect(x, y)) {
			source[i * 4] = 0.1f + 0.8f * x / RW; source[i * 4 + 1] = 0.2f + 0.6f * y / RH; source[i * 4 + 2] = 0.05f; source[i * 4 + 3] = (float)((x * 7 + y * 3) % 16) / 16.0f;
			viewModel[i] = x < RW / 4 && y > RH / 2 ? 0.5f : 0.0f;   // the weapon: the rect's lower-left
		}
		else { source[i * 4] = source[i * 4 + 1] = source[i * 4 + 2] = 9.0f; source[i * 4 + 3] = 0.97f; viewModel[i] = 0.5f; }
	}
	Tex tRr, tSource, tViewModel, out;
	if (!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tRr) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tSource) ||
		!makeTex(h, SW, SH, D3DFMT_R32F, false, tViewModel) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, out)) return false;
	if (!fillTex(tRr, rrOut, 4) || !fillTex(tSource, source, 4) || !fillTex(tViewModel, viewModel, 1)) return false;
	struct { uint32_t pointIndex; float invLinearization; uint32_t rawFrame, rect; float scaleX, scaleY, renderW, renderH; } push;
	static_assert(sizeof(push) == 32, "TAAFSR.cpp's composite push block");
	auto drawComposite = [&](bool rect) -> bool {
		push = { 0, 1.0f / lin, 0u, rect ? 1u : 0u, rect ? plan.scaleX : 1.0f, rect ? plan.scaleY : 1.0f, (float)(rect ? RW : SW), (float)(rect ? RH : SH) };
		NVR_EXT_RESOURCE_BINDING b[4] = { image(0, tRr.t), image(1, tSource.t), image(2, tViewModel.t) };
		b[3].Slot = 0; b[3].Type = NVR_EXT_BIND_SAMPLER; b[3].pResource = gPointClamp;
		NVR_EXT_DRAW_DESC d = {};
		d.pPipeline = composite; d.TargetCount = 1; d.Targets[0].pResource = out.s;
		d.BindingCount = 4; d.pBindings = b; d.pPushData = &push; d.PushDataSize = 32;
		CHECKHR(h.ext->Draw(&d), "Draw DlssRRComposite");
		return true;
	};
	for (int pass = 0; pass < 2; pass++) {
		const bool rect = pass == 0;
		if (!drawComposite(rect)) return false;
		std::vector<float> o;
		if (!readTex(h, out, o, 4)) return false;
		int bad = 0, weaponPixels = 0, shown = 0, tied = 0;
		for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
			const size_t i = size_t(y) * SW + x;
			float want[4];
			const float* r = &rrOut[i * 4];
			if (rect) {
				// a screen pixel whose centre falls exactly between two rect texels (t + 0.5 an integer: y 22, 67, ... at 128 / 180) may round
				// either way on the GPU (fused multiply-add): left out
				float px, py;
				RrRect::RectPoint(plan, x, y, px, py);
				if (std::fabs(px + 0.5f - std::round(px + 0.5f)) < 1e-3f || std::fabs(py + 0.5f - std::round(py + 0.5f)) < 1e-3f) { tied++; continue; }
				int nx, ny;
				RrRect::NearestRectPixel(plan, x, y, nx, ny);
				const size_t n = size_t(ny) * SW + nx;
				const bool weapon = viewModel[n] > 0.0f;
				want[3] = source[n * 4 + 3];
				if (weapon) {
					weaponPixels++;
					float tx, ty;
					RrRect::RectPoint(plan, x, y, tx, ty);
					const int ix = (int)std::floor(tx), iy = (int)std::floor(ty);
					const float fx = tx - ix, fy = ty - iy;
					auto at = [&](int px, int py, int k) { px = px < 0 ? 0 : (px > (int)RW - 1 ? (int)RW - 1 : px); py = py < 0 ? 0 : (py > (int)RH - 1 ? (int)RH - 1 : py); return source[(size_t(py) * SW + px) * 4 + k]; };
					for (int k = 0; k < 3; k++) {
						const float a = at(ix, iy, k) + (at(ix + 1, iy, k) - at(ix, iy, k)) * fx, b = at(ix, iy + 1, k) + (at(ix + 1, iy + 1, k) - at(ix, iy + 1, k)) * fx;
						want[k] = a + (b - a) * fy;
					}
				}
				else for (int k = 0; k < 3; k++) want[k] = std::pow(r[k], 1.0f / lin);
			}
			else {	// native: texel for texel (outside the "rect" the sentinels are the frame here: weapon 0.5 there -> source 9)
				const bool weapon = viewModel[i] > 0.0f;
				if (weapon) weaponPixels++;
				for (int k = 0; k < 3; k++) want[k] = weapon ? source[i * 4 + k] : std::pow(r[k], 1.0f / lin);
				want[3] = source[i * 4 + 3];
			}
			bool good = true;
			for (int k = 0; k < 4; k++) good = good && std::fabs(o[i * 4 + k] - want[k]) < 3e-3f + 2e-3f * std::fabs(want[k]);
			if (!good) { bad++; if (shown++ < 6) say("    %s composite (%u, %u): %.4f %.4f %.4f a %.3f, want %.4f %.4f %.4f a %.3f\n", rect ? "rect" : "native", x, y,
				o[i * 4], o[i * 4 + 1], o[i * 4 + 2], o[i * 4 + 3], want[0], want[1], want[2], want[3]); }
		}
		const bool pass1 = bad == 0 && weaponPixels > 0 && tied < int(SW * SH) / 20;
		say("  composite, %s: %u screen pixels (%d on a texel tie left out), %d weapon pixels (%s), %d wrong -> %s\n", rect ? "rect frame (RR's output everywhere, the frame before RR read in the rect)" :
			"native frame (texel for texel)", SW * SH, tied, weaponPixels, rect ? "the rect's bilinear stretch" : "as rendered", bad, pass1 ? "ok" : "BAD");
		if (!pass1) ok = false;
	}
	buffer->Release();
	guides->Release();
	composite->Release();
	for (Tex* t : { &tColor, &tNormals, &tDepth, &tMaterial, &tAlbedo, &tPt, &oColor, &oDiffuse, &oSpecular, &oNormals, &oRough, &tRr, &tSource, &tViewModel, &out }) {
		if (t->sys) t->sys->Release();
		if (t->s) t->s->Release();
		if (t->t) t->t->Release();
	}
	return ok;
}
