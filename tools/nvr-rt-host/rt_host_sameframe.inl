// ==============================================================================================================================
// Scene `sameframe` (2026-11-07): SameFrame RT lamp shadows ([Shaders.ShadowsInteriors.RT] SameFrame, RtLampShadows.h TraceBeforeWorld,
// RtLampPrimary.comp; docs/next/FORK-RT-LAMPS.md "Same-frame lamp shadows"). A person-sized box walks between a low lamp and a wall, 50
// units a frame, restaged every frame as the game poses an actor (a DYNAMIC build of the same id, scene space, mask 0x20; REBUILD every 3rd
// pose: ActorRebuildInterval); its shadow on the wall moves ~9 pixels a frame. A second box stands just in front of the camera with the custom index NVR_RT_CUSTOM_HIDDEN (the player's
// own body in first person: traced, not drawn). The depth's jitter changes every frame (TAA). Per frame:
//   same frame:  RtLampPrimary (this frame's camera rays -> depth + normals) then RtLampTrace, one exchange; the game's own lookup
//                (PointShadowForward.hlsl PointShadowTraced, ps_3_0) with THIS frame's camera (Aim's matrix: only the jitter).
//   pipelined:   (the control) RtLampTrace on LAST frame's G-buffer and scene, the lookup reprojected into last frame (the old way).
// Checked against the CPU (exact rays, same triangles): the primary pass's depth (view z) and normal per pixel, the hidden box never
// seen by the camera rays (pixels where it would be: counted, must be > 0), every stable lookup answer = the lamp's CPU visibility at
// this frame's point. PASS: same frame 0 wrong answers in every frame and > 90 % answered; pipelined > 40 wrong a frame (the test
// sees the ghost); 0 depth / normal mismatches.

struct PrimaryConstantsHost {   // RtLampShadows.h PrimaryConstants (rt_layout_check.py)
	float size[4], forward[4], right[4], up[4], origin[4];
	float jitter[4];
	float masks[4];
};
static_assert(sizeof(PrimaryConstantsHost) == 7 * 16, "as RtLampShadows.h");

static int gShPrimary = -1;
static bool registerSameFrame(Host& h, const std::string& spv) {
	gShPrimary = h.AddShader(spv + "\\RtLampPrimary.spv");
	return gShPrimary >= 0;
}

// the camera ray through pixel (i, j) + the jitter (screen uv), unnormalized (a point at view z s is eye + ray x s)
static V3 jitteredRay(const Camera& c, UINT i, UINT j, float jx, float jy, float di = 0, float dj = 0) {
	return c.ray((i + 0.5f + di) / S + jx, (j + 0.5f + dj) / S + jy);
}

// the lookup pass of sceneLampLookup, as a function: pos (camera-relative point, w 1 = valid), slot -> PointShadowTraced's answer
struct LookupRig { IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexShader9* vs = nullptr; ID3DBlob* psCode = nullptr; ID3DBlob* vsCode = nullptr; Tex pos, slots, out; };
static bool makeLookupRig(Host& h, LookupRig& r) {
	static const char* ps =
		"#define INTERIOR_SHADOWS 1\n#define POINT_SHADOW_RT_ONLY 1\n#include \"PointShadowForward.hlsl\"\n"
		"sampler2D HostPos : register(s0);\nsampler2D HostSlot : register(s1);\nfloat4 HostSize : register(c0);\n"
		"float4 main(float2 vpos : VPOS) : COLOR0 {\n  float2 uv = (vpos + 0.5) * HostSize.zw;\n  float4 p = tex2D(HostPos, uv);\n"
		"  float slot = tex2D(HostSlot, uv).r;\n  return p.w > 0.5 ? PointShadowTraced(slot, p.xyz) : -9.0;\n}\n";
	static const char* vs = "float4 main(float4 p : POSITION) : POSITION { return p; }\n";
	if (!compileHlsl(h, ps, "ps_3_0", &r.psCode) || !compileHlsl(h, vs, "vs_3_0", &r.vsCode)) return false;
	CHECKHR(h.dev->CreatePixelShader((const DWORD*)r.psCode->GetBufferPointer(), &r.ps), "CreatePixelShader");
	CHECKHR(h.dev->CreateVertexShader((const DWORD*)r.vsCode->GetBufferPointer(), &r.vs), "CreateVertexShader");
	return makeTex(h, S, S, D3DFMT_A32B32G32R32F, false, r.pos) && makeTex(h, S, S, D3DFMT_R32F, false, r.slots) && makeTex(h, S, S, D3DFMT_R32F, true, r.out);
}
static void freeLookupRig(LookupRig& r) {
	for (Tex* t : { &r.pos, &r.slots, &r.out }) { if (t->sys) t->sys->Release(); if (t->s) t->s->Release(); if (t->t) t->t->Release(); }
	if (r.ps) r.ps->Release(); if (r.vs) r.vs->Release(); if (r.psCode) r.psCode->Release(); if (r.vsCode) r.vsCode->Release();
}
// The lookup matrix as RtLampShadows::Aim builds it: camera-relative point of the drawing frame (camera `now`) -> the traced frame's clip
// (camera `traced`, its jitter jx jy off the uv: uv' = uv - j)
static void aimMatrix(const Camera& traced, const Camera& now, float jx, float jy, float m[16]) {
	const V3 pe = traced.eye - now.eye, ax = traced.r * traced.p00, ay = traced.u * traced.p11, aw = traced.f;
	const float r[16] = { ax.x, ay.x, 0, aw.x, ax.y, ay.y, 0, aw.y, ax.z, ay.z, 0, aw.z, -dot(ax, pe), -dot(ay, pe), 0, -dot(aw, pe) };
	for (int i = 0; i < 4; i++) {
		m[4 * i + 0] = r[4 * i + 0] - 2.0f * jx * r[4 * i + 3];
		m[4 * i + 1] = r[4 * i + 1] + 2.0f * jy * r[4 * i + 3];
		m[4 * i + 2] = r[4 * i + 2];
		m[4 * i + 3] = r[4 * i + 3];
	}
}
static bool runLookup(Host& h, LookupRig& r, int resultImage, const std::vector<float>& pos, const std::vector<float>& slot, const float reproject[16],
	float farZ, std::vector<float>& got) {
	if (!fillTex(r.pos, pos, 4) || !fillTex(r.slots, slot, 1)) return false;
	const float rt[4] = { 1.0f, 0.02f, std::log2(farZ) / 4095.0f, 0.0f };
	const float slotsC[8] = { -1, -1, -1, -1, -1, -1, -1, 1.0f / 1024.0f };
	const float size[4] = { float(S), float(S), 1.0f / S, 1.0f / S };
	h.dev->SetVertexShader(r.vs); h.dev->SetPixelShader(r.ps);
	h.dev->SetPixelShaderConstantF(200, reproject, 4); h.dev->SetPixelShaderConstantF(215, rt, 1); h.dev->SetPixelShaderConstantF(216, slotsC, 2);
	h.dev->SetPixelShaderConstantF(0, size, 1);
	h.dev->SetTexture(0, r.pos.t); h.dev->SetTexture(1, r.slots.t); h.dev->SetTexture(15, h.images[resultImage].readTex);
	for (DWORD s : { 0u, 1u, 15u }) {
		h.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); h.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		h.dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		h.dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); h.dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	h.dev->SetRenderState(D3DRS_ZENABLE, FALSE); h.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DSurface9* back = nullptr; h.dev->GetRenderTarget(0, &back);
	h.dev->SetRenderTarget(0, r.out.s);
	D3DVIEWPORT9 vp = { 0, 0, S, S, 0.0f, 1.0f }; h.dev->SetViewport(&vp);
	const float quad[4][4] = { { -1, -1, 0.5f, 1 }, { -1, 1, 0.5f, 1 }, { 1, -1, 0.5f, 1 }, { 1, 1, 0.5f, 1 } };
	h.dev->SetFVF(D3DFVF_XYZW);
	h.dev->BeginScene();
	h.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 16);
	h.dev->EndScene();
	h.dev->SetRenderTarget(0, back); if (back) back->Release();
	h.dev->SetTexture(0, nullptr); h.dev->SetTexture(1, nullptr); h.dev->SetTexture(15, nullptr);
	h.dev->SetPixelShader(nullptr); h.dev->SetVertexShader(nullptr);
	return readTex(h, r.out, got, 1);
}

static bool lampLitMask(const World& w, V3 origin, const Lamp& l, float offset, uint32_t mask) {
	const V3 to = l.pos - origin;
	const float dist = length(to);
	if (!(dist > l.stop + offset)) return true;
	const float invLen = 1.0f / dist, tEnd = std::min(std::max(1.0f - l.stop * invLen, 0.0f), 1.0f), tMin = std::min(offset * invLen, tEnd);
	return !trace(w, origin, to, tMin, tEnd, mask, true).hit;
}

static bool sceneSameFrame(Ctx& c) {
	Host& h = *c.host;
	if (gShPrimary < 0) { say("  error: RtLampPrimary.spv not registered\n"); return false; }
	LookupRig rig;
	if (!makeLookupRig(h, rig)) return false;
	const uint32_t shadowMask = 0x23;   // land, objects, people (the game's farMask keeps 0x20 / 0x40)
	// the world: floor (land), back wall, the walker (restaged each frame), the hidden body
	World w;
	w.meshes.resize(3);
	w.meshes[0].quad({ -3000, -3000, 0 }, { 3000, -3000, 0 }, { 3000, 3000, 0 }, { -3000, 3000, 0 });
	w.meshes[0].quad({ -1600, 700, 0 }, { -1600, 700, 1000 }, { 1600, 700, 1000 }, { 1600, 700, 0 });
	w.meshes[1].box({ -30, -370, 0 }, { 30, -330, 140 });                         // the hidden first-person body, in front of the eye
	Object floorWall; floorWall.mesh = 0; floorWall.land = false; floorWall.hookMask = -1;
	Object body; body.mesh = 1; body.hookMask = 0x20; body.hidden = true;            // custom index NVR_RT_CUSTOM_HIDDEN
	Object walker; walker.mesh = 2; walker.hookMask = 0x20;                         // posed in world space here (the game: scene space)
	w.objects = { floorWall, body, walker };
	const Lamp lamp = { { 0, 150, 170 }, 4000, 1.0f, 4000, 0.0f, 0.0f };
	const std::vector<Lamp> lamps = { lamp };
	const Camera cam = Camera::lookDir({ 0, -400, 170 }, { 0, 1, -0.15f }, { 0, 0, 1 }, 1.2f, 1.2f);
	auto walkerAt = [](int f) { Mesh m; const float x = -250.0f + 50.0f * f; m.box({ x - 60, 380, 0 }, { x + 60, 440, 230 }); return m; };
	auto jit = [](int f, float& jx, float& jy) { jx = (float((f * 3) % 4) - 1.5f) * 0.25f / S; jy = (float((f * 5 + 1) % 4) - 1.5f) * 0.25f / S; };

	// stage the static meshes; the walker each frame with its own id (DYNAMIC; REBUILD every 3rd pose)
	std::vector<uint32_t> ids(3, 0);
	if (!h.Stage(w.meshes[0], false, ids[0]) || !h.Stage(w.meshes[1], false, ids[1])) return false;
	const uint32_t walkerId = h.nextGeometry++;
	ids[2] = walkerId;
	struct Frame { World w; std::vector<float> depth, normals; Camera cam; float jx = 0, jy = 0; };
	auto cpuFrame = [&](int f, Frame& out) {
		out.w = w; out.w.meshes[2] = walkerAt(f); out.cam = cam; jit(f, out.jx, out.jy);
	};
	World primaryWorld;   // what the camera rays see: without the hidden body
	auto noBody = [](const World& src) { World r = src; r.objects.erase(r.objects.begin() + 1); return r; };

	const int frames = 7;
	int sameWrongAll = 0, sameCheckedAll = 0, sameAnsweredAll = 0, lateWrongMin = 1 << 30, depthBad = 0, normalBad = 0, hiddenPixels = 0, pixelsChecked = 0;
	bool ok = true;
	std::vector<float> lastDepth, lastNormals;
	Frame prev;
	for (int f = 0; f < frames && ok; f++) {
		Frame fr; cpuFrame(f, fr);
		primaryWorld = noBody(fr.w);
		const uint32_t flags = NVR_RT_OP_FLAG_DYNAMIC | ((f > 0 && f % 3 == 0) ? NVR_RT_OP_FLAG_REBUILD : 0u);
		if (!h.StageAs(fr.w.meshes[2], walkerId, flags)) return false;
		// ---- same frame: the camera rays, then the lamp trace, in one exchange
		PrimaryConstantsHost pc = {};
		cameraConstants(fr.cam, pc.size, pc.forward, pc.right, pc.up, pc.origin);
		put4(pc.jitter, fr.jx, fr.jy, 0.0f, 0.0f);
		put4(pc.masks, float(0xFFu & ~0x60u), float(0x60u), 0.0f, 0.0f);
		h.AddPass(gShPrimary, { c.depth, c.normals, c.depth, c.depth, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn }, 0x3, &pc, sizeof(pc), (S + 7) / 8, (S + 7) / 8);
		LampConstants k; lampPassConstants(k, fr.cam, 1, 1);
		k.params2[1] = k.params2[2] = float(shadowMask);
		put4(k.jitter, fr.jx, fr.jy, 0.0f, 0.0f);
		if (!h.Upload(c.lampTable, lampTable(lamps, fr.cam.eye), 0, 0, 1024, 1)) return false;
		h.AddPass(c.shLamp, { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn }, 0x4, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(fr.w, ids, fr.cam.eye)) return false;
		std::vector<float> gotDepth, gotNormals, result;
		if (!h.Read(c.depth, gotDepth) || !h.Read(c.normals, gotNormals) || !h.Read(c.lampResult, result)) return false;
		h.EndFrame();
		// ---- the CPU: per pixel the camera ray's hit (no hidden body), its lamp visibility; edges skipped
		std::vector<float> pos(S * S * 4, 0.0f), slot(S * S, 0.0f), want(S * S, -1.0f);
		std::vector<char> stable(S * S, 0);
		std::vector<float> cpuDepth(S * S, 1.0f), cpuNormals(S * S * 4, 0.5f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const UINT p = j * S + i;
			const V3 d = jitteredRay(fr.cam, i, j, fr.jx, fr.jy);
			const Hit hh = trace(primaryWorld, fr.cam.eye, d, 0.5f / length(d), 1e9f, 0xFF, true);
			const Hit withBody = trace(fr.w, fr.cam.eye, d, 0.5f / length(d), 1e9f, 0xFF, true);
			if (withBody.hit && withBody.object == 1) hiddenPixels++;
			if (!hh.hit) continue;
			const V3 world = fr.cam.eye + d * hh.t;
			cpuDepth[p] = hh.t / fr.cam.farZ;
			float* n = &cpuNormals[p * 4];
			n[0] = dot(hh.n, fr.cam.r) * 0.5f + 0.5f; n[1] = dot(hh.n, fr.cam.u) * 0.5f + 0.5f; n[2] = dot(hh.n, fr.cam.f) * 0.5f + 0.5f; n[3] = 1.0f;
			const float lit = lampLitMask(fr.w, world + hh.n * 0.5f, lamp, 0.5f, shadowMask) ? 1.0f : 0.0f;
			bool st = true;
			for (int s = 1; s < 5 && st; s++) {
				const V3 dd = jitteredRay(fr.cam, i, j, fr.jx, fr.jy, s == 1 ? -0.6f : s == 2 ? 0.6f : 0, s == 3 ? -0.6f : s == 4 ? 0.6f : 0);
				const Hit h2 = trace(primaryWorld, fr.cam.eye, dd, 0.5f / length(dd), 1e9f, 0xFF, true);
				if (!h2.hit || h2.object != hh.object || (lampLitMask(fr.w, fr.cam.eye + dd * h2.t + h2.n * 0.5f, lamp, 0.5f, shadowMask) ? 1.0f : 0.0f) != lit) st = false;
			}
			stable[p] = st;
			want[p] = lit;
			const V3 q = world - fr.cam.eye;
			pos[p * 4] = q.x; pos[p * 4 + 1] = q.y; pos[p * 4 + 2] = q.z; pos[p * 4 + 3] = 1.0f;
			if (!st) continue;
			pixelsChecked++;
			// the camera rays' depth and normal (the game's encodings)
			const float gz = gotDepth[p] * fr.cam.farZ;
			if (!(std::fabs(gz - hh.t) <= 1e-4f * hh.t + 1e-3f)) { if (depthBad++ < 4) say("    frame %d pixel (%u, %u): primary view z %.3f, CPU %.3f\n", f, i, j, gz, hh.t); }
			const V3 gn(gotNormals[p * 4] * 2 - 1, gotNormals[p * 4 + 1] * 2 - 1, gotNormals[p * 4 + 2] * 2 - 1);
			const V3 cn(n[0] * 2 - 1, n[1] * 2 - 1, n[2] * 2 - 1);
			if (!(dot(normalize(gn), cn) > 0.998f)) { if (normalBad++ < 4) say("    frame %d pixel (%u, %u): primary normal %.3f %.3f %.3f, CPU %.3f %.3f %.3f\n", f, i, j, gn.x, gn.y, gn.z, cn.x, cn.y, cn.z); }
		}
		// ---- the lookup with this frame's camera (only the jitter in Aim's matrix)
		float m[16]; aimMatrix(fr.cam, fr.cam, fr.jx, fr.jy, m);
		std::vector<float> got;
		if (!runLookup(h, rig, c.lampResult, pos, slot, m, fr.cam.farZ, got)) return false;
		int wrong = 0, checked = 0, answered = 0, shown = 0;
		for (UINT p = 0; p < S * S; p++) {
			if (!stable[p] || want[p] < 0) continue;
			checked++;
			if (got[p] >= 0.0f) { answered++; if (std::fabs(got[p] - want[p]) > 1e-4f) { wrong++; if (shown++ < 4) say("    same frame %d pixel %u: lookup %.3f, CPU lit %.0f\n", f, p, got[p], want[p]); } }
		}
		sameWrongAll += wrong; sameCheckedAll += checked; sameAnsweredAll += answered;
		if (wrong) dumpPfm("sameframe_lookup", got, S, S, 1);
		// ---- the control: last frame traced on last frame's G-buffer, looked up now through the reprojection (the old pipelined way)
		int lateWrong = -1, lateAnswered = 0;
		if (f > 0) {
			if (!h.StageAs(prev.w.meshes[2], walkerId, NVR_RT_OP_FLAG_DYNAMIC)) return false;   // last frame's pose again
			if (!h.Upload(c.depth, lastDepth) || !h.Upload(c.normals, lastNormals)) return false;
			LampConstants kl; lampPassConstants(kl, prev.cam, 1, 1);
			kl.params2[1] = kl.params2[2] = float(shadowMask);
			put4(kl.jitter, prev.jx, prev.jy, 0.0f, 0.0f);
			h.AddPass(c.shLamp, { c.depth, c.normals, c.lampResult, c.lampTable, c.depth, c.depth, c.depth, c.depth, c.alphaStandIn }, 0x4, &kl, sizeof(kl), (S + 7) / 8, (S + 7) / 8);
			if (!h.Exchange(prev.w, ids, prev.cam.eye)) return false;
			std::vector<float> late;
			if (!h.Read(c.lampResult, late)) return false;
			h.EndFrame();
			float ml[16]; aimMatrix(prev.cam, fr.cam, prev.jx, prev.jy, ml);
			std::vector<float> gotLate;
			if (!runLookup(h, rig, c.lampResult, pos, slot, ml, fr.cam.farZ, gotLate)) return false;
			lateWrong = 0;
			for (UINT p = 0; p < S * S; p++) if (stable[p] && want[p] >= 0 && gotLate[p] >= 0.0f) { lateAnswered++; if (std::fabs(gotLate[p] - want[p]) > 1e-4f) lateWrong++; }
			lateWrongMin = std::min(lateWrongMin, lateWrong);
		}
		say("  frame %d: walker at x %.0f, jitter (%.2f, %.2f) px%s: same frame %d of %d stable pixels answered, %d wrong; pipelined (last frame's trace "
			"reprojected) %d answered, %d wrong\n", f, -250.0f + 50.0f * f, fr.jx * S, fr.jy * S, (f > 0 && f % 3 == 0) ? ", walker rebuilt" : "",
			answered, checked, wrong, lateAnswered, lateWrong);
		lastDepth = cpuDepth; lastNormals = cpuNormals;
		prev = fr;
	}
	releaseWorld(h, { ids[0], ids[1], walkerId });
	freeLookupRig(rig);
	const bool pass = ok && sameWrongAll == 0 && sameAnsweredAll > (sameCheckedAll * 9) / 10 && lateWrongMin > 40 && depthBad == 0 && normalBad == 0 && hiddenPixels > 0;
	say("  same frame: %d stable pixels over %d frames, %d answered from this frame's trace, %d wrong; the camera rays: %d pixels checked, %d depth and %d normal "
		"mismatches, %d pixels where the hidden first-person body would have been seen (passed over); pipelined control: at least %d wrong a frame "
		"(must be > 40: the ghost)\n", sameCheckedAll, frames, sameAnsweredAll, sameWrongAll, pixelsChecked, depthBad, normalBad, hiddenPixels, lateWrongMin);
	return pass;
}
