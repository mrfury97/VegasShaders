// (2026-11-15 fd-gi, docs/next/FD-GI-RESULTS.md) Scene `rrv2`: the DLSS Ray Reconstruction guides (DlssRRGuides.comp, unchanged) on G-buffer
// v2 inputs, as TAAFSR.cpp RayReconstruct feeds them with [Main.Main.Quality] DeferredRRGuides on: the material in layout v2 (r = model + 16 x
// specular bit + 0.49 x metal, b roughness incl. the path tracer's -3 .. -2 code, -1 none) and the albedo buffer (COLOR1, a 1 forward or
// 1 + (1 - fog) deferred, 0 unwritten), both in A16B16G16R16F as the game's targets; the normals as core/FdIndirect.h's shading normals (view
// space, n x 0.5 + 0.5, the normals buffer's encoding). Options {2, 0, 1, 0}: the albedo buffer path, world normals, the material read, no
// path tracer. Native frame (render = screen). Every pixel's diffuse (base x (1 - metal)), specular (Karis with f0 = mix(0.04, base, metal)),
// normal (world) and roughness must equal the CPU's from the fp16-stored values; and the metal the guides decode must be within fp16's step of
// the metal the G-buffer was given (the spec bit puts r at 17-26.49: steps of 1/64, metal error up to 0.016; FD-GBUFFER-RESULTS.md 1.1).
namespace rrv2 {
	float envBrdf3(float f0, float roughness, float NdotV) { return rrrect::envBrdf(f0, roughness, NdotV); }
	float q(float x) { return fromHalf(toHalf(x)); }	// what an fp16 target holds
	float frac(float x) { return x - std::floor(x); }
}

static bool sceneRrV2(Ctx& c) {
	using namespace rrrect;
	Host& h = *c.host;
	const UINT SW = 192, SH = 128;
	const RrRect::Plan plan = RrRect::Make(false, SW, SH, SW, SH, true, 0.0f, 0.0f, 8, false, 0, 0, 8);
	if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	ID3D9NvrExtPipeline* guides = nullptr;
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
		d.UsesSamplerHeap = TRUE; d.SamplerHeapSet = 0; d.SamplerHeapBinding = 0; d.pDebugName = "DlssRRGuides (rrv2)";
		CHECKHR(h.ext->CreateComputePipeline(&d, &guides), "CreateComputePipeline DlssRRGuides");
	}
	const float lin = 2.2f;
	std::vector<float> color(SW * SH * 4), normals(SW * SH * 4), depth(SW * SH), material(SW * SH * 4), albedo(SW * SH * 4), metalGiven(SW * SH);
	std::vector<int> written(SW * SH);
	unsigned seed = 777u;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
	for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
		const size_t i = size_t(y) * SW + x;
		float* cl = &color[i * 4]; float* nr = &normals[i * 4]; float* mt = &material[i * 4]; float* al = &albedo[i * 4];
		cl[0] = 0.1f + 0.6f * rnd(); cl[1] = 0.1f + 0.6f * rnd(); cl[2] = 0.1f + 0.6f * rnd(); cl[3] = 1.0f;
		// a bumped view-space normal facing the camera
		V3 n = normalize(V3{ (rnd() - 0.5f) * 1.2f, (rnd() - 0.5f) * 1.2f, -1.0f });
		nr[0] = n.x * 0.5f + 0.5f; nr[1] = n.y * 0.5f + 0.5f; nr[2] = n.z * 0.5f + 0.5f; nr[3] = 1.0f;
		depth[i] = 0.3f;
		const int model = 1 + int(rnd() * 10.0f) % 10, spec = rnd() < 0.5f ? 1 : 0;
		const float metal = rnd() < 0.3f ? 0.0f : (rnd() < 0.2f ? 1.0f : rnd());
		metalGiven[i] = metal;
		const float pick = rnd();
		const float b = pick < 0.1f ? -1.0f : pick < 0.2f ? -2.0f - rnd() : rnd();
		mt[0] = float(model + 16 * spec) + 0.49f * metal; mt[1] = (rnd() - 0.5f) * 2.0f; mt[2] = b; mt[3] = (rnd() - 0.5f) * 2.0f;
		const float apick = rnd();
		al[0] = rnd(); al[1] = rnd(); al[2] = rnd(); al[3] = apick < 0.15f ? 0.0f : apick < 0.5f ? 1.0f : 1.0f + rnd();
		written[i] = al[3] > 0.5f;
	}
	Tex tColor, tNormals, tDepth, tMaterial, tAlbedo, oColor, oDiffuse, oSpecular, oNormals, oRough;
	if (!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tColor) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tNormals) ||
		!makeTex(h, SW, SH, D3DFMT_R32F, false, tDepth) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tMaterial) ||
		!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, false, tAlbedo) ||
		!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oColor) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oDiffuse) ||
		!makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oSpecular) || !makeTex(h, SW, SH, D3DFMT_A16B16G16R16F, true, oNormals) ||
		!makeTex(h, SW, SH, D3DFMT_R16F, true, oRough)) return false;
	if (!fillTex(tColor, color, 4) || !fillTex(tNormals, normals, 4) || !fillTex(tDepth, depth, 1) || !fillTex(tMaterial, material, 4) || !fillTex(tAlbedo, albedo, 4)) return false;
	// camera axes in world space: right +x, up +z, forward +y (as rrrect); options {2 albedo buffer, 0 world normals, 1 material, 0 no path tracer}
	struct { float right[4], up[4], forward[4], view[4], options[4], size[4]; } cb = {
		{ 1, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, 1, 0, 0 }, { 1.0f, 0.5625f, lin, 0.6f }, { 2.0f, 0.0f, 1.0f, 0.0f }, { (float)SW, (float)SH, 0, 0 } };
	ID3D9NvrExtBuffer* buffer = nullptr;
	{
		NVR_EXT_BUFFER_DESC bd = { UINT(sizeof(cb)), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "rrv2 guides constants" };
		CHECKHR(h.ext->CreateBuffer(&bd, &buffer), "CreateBuffer");
		CHECKHR(h.ext->UpdateBuffer(buffer, 0, sizeof(cb), &cb), "UpdateBuffer");
	}
	{
		NVR_EXT_RESOURCE_BINDING b[15] = { image(0, tColor.t), image(1, tNormals.t), image(2, tDepth.t), image(3, tMaterial.t),
			image(4, oColor.t, true), image(5, oDiffuse.t, true), image(6, oSpecular.t, true), image(7, oNormals.t, true), image(8, oRough.t, true) };
		b[9].Slot = 0; b[9].Type = NVR_EXT_BIND_UNIFORM_BUFFER; b[9].pResource = buffer;
		b[10].Slot = 0; b[10].Type = NVR_EXT_BIND_SAMPLER; b[10].pResource = gPointClamp;
		b[11] = image(9, tAlbedo.t); b[12] = image(10, tDepth.t); b[13] = image(11, tDepth.t); b[14] = image(12, tDepth.t);
		uint32_t push = 0;
		NVR_EXT_DISPATCH_DESC d = {};
		d.pPipeline = guides; d.BindingCount = 15; d.pBindings = b; d.pPushData = &push; d.PushDataSize = 4;
		d.GroupsX = plan.groupsX; d.GroupsY = plan.groupsY; d.GroupsZ = 1;
		CHECKHR(h.ext->Dispatch(&d), "Dispatch DlssRRGuides");
	}
	std::vector<float> rc, rd, rs, rn, rr;
	if (!readTex(h, oColor, rc, 4) || !readTex(h, oDiffuse, rd, 4) || !readTex(h, oSpecular, rs, 4) || !readTex(h, oNormals, rn, 4) || !readTex(h, oRough, rr, 1)) return false;
	int bad = 0, shown = 0, metals = 0, specBits = 0, codes = 0, unwritten = 0;
	float worstMetal = 0.0f, worstMetalSpec = 0.0f, worst = 0.0f;
	for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
		const size_t i = size_t(y) * SW + x;
		// the stored (fp16) values the shader reads
		const float r = rrv2::q(material[i * 4]), b = rrv2::q(material[i * 4 + 2]);
		float roughness = 0.6f;
		if (r > 0.5f && b >= 0.0f) roughness = b;
		else if (r > 0.5f && b < -1.99f && b > -3.01f) { roughness = -2.0f - b; codes++; }
		roughness = saturate(roughness);
		const float metal = r > 0.5f ? saturate(rrv2::frac(r) / 0.49f) : 0.0f;
		const bool hasSpec = r >= 16.0f;
		if (hasSpec) specBits++;
		if (metalGiven[i] > 0.0f) metals++;
		const float metalErr = std::fabs(metal - metalGiven[i]);
		worstMetal = std::fmax(worstMetal, metalErr);
		if (hasSpec) worstMetalSpec = std::fmax(worstMetalSpec, metalErr);
		float base[3], diffuse[3], f0[3];
		const bool w = written[i] != 0;
		if (!w) unwritten++;
		float n[3] = { rrv2::q(normals[i * 4]) * 2 - 1, rrv2::q(normals[i * 4 + 1]) * 2 - 1, rrv2::q(normals[i * 4 + 2]) * 2 - 1 };
		const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); for (float& v : n) v /= nl;
		const float u = (x + 0.5f) / SW, v = (y + 0.5f) / SH;
		float ta = (2.0f * u - 1.0f) * 1.0f, tb = (1.0f - 2.0f * v) * 0.5625f, tl = std::sqrt(ta * ta + tb * tb + 1.0f);
		const float tc[3] = { -ta / tl, -tb / tl, -1.0f / tl };
		const float NdotV = saturate(n[0] * tc[0] + n[1] * tc[1] + n[2] * tc[2]);
		bool good = true;
		for (int k = 0; k < 3; k++) {
			base[k] = saturate(rrv2::q(albedo[i * 4 + k]));
			if (w) { diffuse[k] = base[k] * (1.0f - metal); f0[k] = 0.04f + (base[k] - 0.04f) * metal; }
			else { diffuse[k] = -1.0f; f0[k] = 0.04f; }	// unwritten: the chroma stand-in (options.x 2 > 0.5): not checked here
			const float sp = saturate(rrv2::envBrdf3(f0[k], roughness, NdotV));
			if (w) { good = good && std::fabs(rd[i * 4 + k] - diffuse[k]) < 2e-3f; worst = std::fmax(worst, std::fabs(rd[i * 4 + k] - diffuse[k])); }
			good = good && std::fabs(rs[i * 4 + k] - sp) < 3e-3f;
		}
		const float world[3] = { n[0], n[2], n[1] };
		for (int k = 0; k < 3; k++) good = good && std::fabs(rn[i * 4 + k] - world[k]) < 3e-3f;
		good = good && std::fabs(rr[i] - roughness) < 2e-3f;
		if (!good) { bad++; if (shown++ < 6) say("    pixel (%u, %u): r %.4f b %.4f: diffuse %.4f (%.4f), spec %.4f, rough %.4f (%.4f), normal %.3f %.3f %.3f (%.3f %.3f %.3f)\n",
			x, y, r, b, rd[i * 4], diffuse[0], rs[i * 4], rr[i], roughness, rn[i * 4], rn[i * 4 + 1], rn[i * 4 + 2], world[0], world[1], world[2]); }
	}
	const bool pass = bad == 0 && worstMetalSpec <= 0.017f;
	say("  v2 guides: %u pixels (%d with the specular bit, %d metal, %d roughness codes -3..-2, %d albedo unwritten), %d wrong vs the CPU from the stored "
		"values (worst diffuse %.5f); metal decoded from the fp16 target vs given: worst %.4f, with the specular bit %.4f (fp16 step at 16-32: 1/64 -> 0.016) -> %s\n",
		SW * SH, specBits, metals, codes, unwritten, bad, worst, worstMetal, worstMetalSpec, pass ? "ok" : "BAD");
	buffer->Release();
	guides->Release();
	for (Tex* t : { &tColor, &tNormals, &tDepth, &tMaterial, &tAlbedo, &oColor, &oDiffuse, &oSpecular, &oNormals, &oRough }) {
		if (t->sys) t->sys->Release();
		if (t->s) t->s->Release();
		if (t->t) t->t->Release();
	}
	return pass;
}
