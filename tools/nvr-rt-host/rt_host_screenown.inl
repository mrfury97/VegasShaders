// (2026-11-11 PTScreenOwn + CapturedBlendedAlpha) Scene `screenown`: surfaces whose screen colour / material are not their own.
//
// A tester on an RTX 4090 (PTRealtime, PTPrimary): a head of hair seen from behind shaded as the plaster wall behind it; "hair, and
// sometimes skin, takes on the material of other surfaces it overlaps". The path tracer's first surface takes its colour from the
// AlbedoBuffer and its class / roughness / metal from the MaterialBuffer (RtPathTrace.comp ScreenHit). Both are written only by NVR's
// templates drawn opaque in the world's main list (core/CapturedNormals.h); whatever the game draws afterwards WITH depth leaves them as
// they were, and the skin template writes neither (its colour comes from the skin blur's target, its material from SkinFill).
// The same evening, another tester (Tale of Two Wastelands): rubble piles as the game's bright raster pixels beside path-traced ground.
// Those are writers drawn BLENDED in the main list: under MaterialAware's scene alpha blend ZERO / ONE they wrote the buffers' rgb and
// kept their alpha, 0 = "unwritten" (CapturedNormals.h BlendedAlphaBegin).
//
// The G-buffer here is built the way the game leaves it. All quads face the camera:
//   wall     y 1000, captured (object template): green, metal 1, roughness 0.1; in the RT scene
//   armour   y 640, captured: red, metal 1, roughness 0.2; in the RT scene; drawn before the body, partly behind it
//   body     y 600, SKIN drawn in the main list (the skin template: captured normal + distance, alpha 2 = an actor; no albedo, no
//            material of its own); in the RT scene as an actor's skin record
//   head     y 600, skin drawn AFTER the main list (the sorted blended list / a shader of the game's own): depth only; in the RT scene
//   hair     y 596, the game's blended hair (depth only; no alpha test on screen): over the head's upper half and beyond it over the
//            wall; in the RT scene a TINTED cut-out (BlendedHair: reference 128, alpha 0.6 everywhere = 59.8 % of the rays)
//   thing    y 600, depth only and NOT in the RT scene (hair that is not traced, an edge the pose misses)
//   land     y 400, captured (class 4); in the RT scene, flat. Three patches of it are DISPLACED toward the camera by 5, 20 and 60 units
//            (the depth buffer only; the captured distance stays the flat land's, as the object tessellation leaves it), and a second
//            hair card stands 10 units in front of it
//   rubble   y 600, a writer drawn BLENDED in the main list before anything lay behind it: rgb written, the alpha as the device left it
// Part 0: the blend state itself, on the fork's D3D9 device: two render targets, SRCALPHA / INVSRCALPHA, the scene's separate alpha blend
//   ZERO / ONE: target 1's alpha stays the clear's 0 (its rgb is the shader's); with ONE / ZERO + the scene target's alpha masked off
//   (the fix) target 1's alpha is the shader's 1 and target 0's alpha is still what it was. The rubble's G-buffer alpha in part 2 is
//   THIS measurement.
// Part 1: the prep pass (src/core/RtViewModel.h, ps_3_0, drawn as RtPathTrace.cpp ViewModelPrep draws it) marks every pixel against the
//   CPU's rule: SURE (+ 0.5) the hair / head / thing before the wall and skin without the skin blur's colour; MAYBE (+ 0.25) what lies
//   in front of its captured surface by less than the tessellation's reach (the 20 and 60 unit bumps, the hair before the land);
//   unmarked the wall, the armour, the land, the 5 unit bump, a surface whose depth lies behind its captured distance; the marks off =
//   the pass as it was; at half RenderScale the same marks.
// Part 2: RtPathTrace on that G-buffer, the normals image = part 1's OUTPUT. PTDebugView 1 (colour), 4 (roughness, metal), 2 (normal),
//   64 paths, per region, "before" (no marks: alpha 1 everywhere, the old DLL) against "after".

struct OwnQuad { float x0, x1, z0, z1, y; };
static bool ownInside(const OwnQuad& q, const Camera& cam, float px, float py) {
	const V3 d = cam.ray(px / S, py / S);   // (d . f = 1: a point at view z s is eye + d s)
	const V3 p = cam.eye + d * (q.y - cam.eye.y);
	return p.x > q.x0 && p.x < q.x1 && p.z > q.z0 && p.z < q.z1;
}

enum { kOwnWall, kOwnArmour, kOwnBody, kOwnHead, kOwnHair, kOwnThing, kOwnLand, kOwnBump5, kOwnBump20, kOwnBump60, kOwnHairLand, kOwnRubble,
	kOwnMeadow, kOwnGrassA, kOwnGrassB, kOwnGrassC, kOwnGrassD, kOwnQuads };   // (2026-11-12 ptgrass: the grass cases, see sceneScreenOwn's last block)
// the regions (a pixel whose centre and four corners agree; -1 an edge pixel)
enum { kRegWall, kRegArmour, kRegBodyArmour, kRegBodyWall, kRegHead, kRegHairHead, kRegHairWall, kRegThing,
	kRegLand, kRegBump5, kRegBump20, kRegBump60, kRegHairLand, kRegRubble,
	kRegMeadow, kRegGrassA, kRegGrassB, kRegGrassC, kRegGrassD, kRegions };
static const char* const kOwnRegionName[kRegions] = { "the wall", "the armour", "the body before the armour", "the body before the wall", "the head (face)",
	"hair over the head", "hair over the wall alone", "the untraced thing", "the land", "the land's bump, 5 units", "the land's bump, 20 units",
	"the land's bump, 60 units", "hair 10 units before the land", "the blended rubble",
	"the meadow (land)", "grass A, 30 units before the meadow", "grass B, drawn before the land", "grass C, 30 units before the meadow", "grass D, 3 units before the meadow" };

struct OwnBuffers {
	std::vector<float> depth, normals, captured, albedo, material, blur;
	std::vector<int> region;
	std::vector<float> blade;   // (2026-11-12 ptgrass) 1 where a grass card is the pixel's depth (the PTGrassOwnPixel mask's input)
};

// Part 0: one blended draw into two render targets under the scene's separate alpha blend; returns target 0's and target 1's texel
static bool ownBlendDraw(Host& h, bool fixed, float out0[4], float out1[4]) {
	Tex t0, t1;
	if (!makeTex(h, 16, 16, D3DFMT_A16B16G16R16F, true, t0) || !makeTex(h, 16, 16, D3DFMT_A16B16G16R16F, true, t1)) return false;
	static const char* kPS =
		"struct O { float4 c0 : COLOR0; float4 c1 : COLOR1; };"
		"O main(float2 uv : TEXCOORD0) { O o; o.c0 = float4(0.2, 0.4, 0.6, 0.5); o.c1 = float4(0.7, 0.6, 0.5, 1.0); return o; }";
	ID3DBlob* code = nullptr;
	if (!compileHlsl(h, kPS, "ps_3_0", &code)) return false;
	IDirect3DPixelShader9* ps = nullptr;
	CHECKHR(h.dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps), "CreatePixelShader");
	code->Release();
	IDirect3DSurface9* back = nullptr; h.dev->GetRenderTarget(0, &back);
	D3DVIEWPORT9 vp = { 0, 0, 16, 16, 0.0f, 1.0f };
	h.dev->SetRenderTarget(0, t1.s); h.dev->SetViewport(&vp); h.dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);   // "unwritten"
	h.dev->SetRenderTarget(0, t0.s); h.dev->SetViewport(&vp); h.dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(64, 0, 0, 0), 1.0f, 0);  // the scene: some alpha
	h.dev->SetRenderTarget(1, t1.s);
	h.dev->SetVertexShader(nullptr); h.dev->SetPixelShader(ps);
	h.dev->SetRenderState(D3DRS_ZENABLE, FALSE); h.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); h.dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); h.dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	h.dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);   // core/ReflMaterial.h SetSceneStates
	h.dev->SetRenderState(D3DRS_SRCBLENDALPHA, fixed ? D3DBLEND_ONE : D3DBLEND_ZERO);
	h.dev->SetRenderState(D3DRS_DESTBLENDALPHA, fixed ? D3DBLEND_ZERO : D3DBLEND_ONE);
	h.dev->SetRenderState(D3DRS_COLORWRITEENABLE, fixed ? 0x7 : 0xF);   // (CapturedNormals.h BlendedAlphaBegin)
	h.dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
	h.dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { 15.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, 15.5f, 0, 1, 0, 1 }, { 15.5f, 15.5f, 0, 1, 1, 1 } };
	h.dev->BeginScene();
	h.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	h.dev->EndScene();
	h.dev->SetRenderTarget(1, nullptr);
	h.dev->SetRenderTarget(0, back); if (back) back->Release();
	h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); h.dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	h.dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE); h.dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
	h.dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	h.dev->SetPixelShader(nullptr);
	ps->Release();
	std::vector<float> g0, g1;
	const bool read = readTex(h, t0, g0, 4) && readTex(h, t1, g1, 4);
	for (Tex* t : { &t0, &t1 }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	if (!read) return false;
	const size_t mid = (8 * 16 + 8) * 4;
	for (int c = 0; c < 4; c++) { out0[c] = g0[mid + c]; out1[c] = g1[mid + c]; }
	return true;
}

// The prep pass on the fork's D3D9 device: textures of size W (the world in the top-left S x S), as RtPathTrace.cpp ViewModelPrep
// (2026-11-12 PTWaterMask) water: the game's water mask (S x S, 1 = water; RtViewModel.h s5 / c9), null = none (c9 = 0: the pass as it was)
static bool ownPrep(Host& h, const Camera& cam, const OwnBuffers& b, UINT W, bool marks, int skinMode, float reach, std::vector<float>& outAlpha, std::vector<float>* outFull = nullptr,
	const std::vector<float>* water = nullptr, float maskThreshold = 0.0f) {
	auto place = [&](const std::vector<float>& v, int ch) {
		if (W == S) return v;
		std::vector<float> o(size_t(W) * W * ch, 0.0f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) for (int c = 0; c < ch; c++) o[(size_t(j) * W + i) * ch + c] = v[(size_t(j) * S + i) * ch + c];
		return o;
	};
	Tex tn, tc, td, tk, out;
	if (!makeTex(h, W, W, D3DFMT_A16B16G16R16F, false, tn) || !makeTex(h, W, W, D3DFMT_A16B16G16R16F, false, tc) || !makeTex(h, W, W, D3DFMT_R32F, false, td) ||
		!makeTex(h, W, W, D3DFMT_A16B16G16R16F, false, tk) || !makeTex(h, W, W, D3DFMT_A16B16G16R16F, true, out)) return false;
	if (!fillTex(tn, place(b.normals, 4), 4) || !fillTex(tc, place(b.captured, 4), 4) || !fillTex(td, place(b.depth, 1), 1) || !fillTex(tk, place(b.blur, 4), 4)) return false;
	Tex twater;
	if (water && (!makeTex(h, W, W, D3DFMT_R32F, false, twater) || !fillTex(twater, place(*water, 1), 1))) return false;
	ID3DBlob* code = nullptr;
	if (!compileHlsl(h, RtViewModel::kPrepPS, "ps_3_0", &code)) return false;
	IDirect3DPixelShader9* ps = nullptr;
	CHECKHR(h.dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps), "CreatePixelShader");
	code->Release();
	const float scale = float(W) / float(S);
	const float k[10][4] = {
		{ 1.0f / cam.p00, 1.0f / cam.p11, 4.0f, cam.farZ },
		{ 1.0f, 1.0f, RtViewModel::kMatchShare, RtViewModel::kMatchUnits },
		{ cam.r.x, cam.u.x, cam.f.x, 0 }, { cam.r.y, cam.u.y, cam.f.y, 0 }, { cam.r.z, cam.u.z, cam.f.z, 0 }, { 0, 0, 0, 1 },
		{ 0.0f, marks ? 1.0f : 0.0f, RtViewModel::kFrontShare, RtViewModel::kFrontUnits },   // no view model (third person)
		{ scale, scale, 0.0f, 0.0f },
		{ marks ? float(skinMode) : 0.0f, reach, 0.0f, 0.0f },
		{ water ? 1.0f : 0.0f, 1.0f, 1.0f, maskThreshold } };   // (PTWaterMask) the mask in the buffers' own layout: uv scale 1
	h.dev->SetVertexShader(nullptr); h.dev->SetPixelShader(ps);
	h.dev->SetPixelShaderConstantF(0, &k[0][0], 10);
	h.dev->SetTexture(0, tn.t); h.dev->SetTexture(1, tc.t); h.dev->SetTexture(2, tn.t); h.dev->SetTexture(3, td.t); h.dev->SetTexture(4, skinMode == 2 ? tk.t : tn.t);
	h.dev->SetTexture(5, water ? twater.t : tn.t);
	for (DWORD st = 0; st < 6; st++) {
		h.dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT); h.dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		h.dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		h.dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); h.dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	h.dev->SetRenderState(D3DRS_ZENABLE, FALSE); h.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DSurface9* back = nullptr; h.dev->GetRenderTarget(0, &back);
	h.dev->SetRenderTarget(0, out.s);
	D3DVIEWPORT9 vp = { 0, 0, W, W, 0.0f, 1.0f }; h.dev->SetViewport(&vp);
	h.dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const float x1 = float(W) - 0.5f, y1 = float(W) - 0.5f;
	const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
	h.dev->BeginScene();
	h.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	h.dev->EndScene();
	h.dev->SetRenderTarget(0, back); if (back) back->Release();
	for (DWORD st = 0; st < 6; st++) h.dev->SetTexture(st, nullptr);
	{ const float none[4] = {}; h.dev->SetPixelShaderConstantF(9, none, 1); }   // (PTWaterMask) c9 back to "no mask" for the scenes that set c0-c8 only (vmprep)
	h.dev->SetPixelShader(nullptr);
	ps->Release();
	std::vector<float> got;
	const bool read = readTex(h, out, got, 4);
	for (Tex* t : { &tn, &tc, &td, &tk, &out }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	if (water) { if (twater.sys) twater.sys->Release(); twater.s->Release(); twater.t->Release(); }
	if (!read) return false;
	outAlpha.assign(S * S, 0.0f);
	if (outFull) outFull->assign(S * S * 4, 0.0f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		outAlpha[j * S + i] = got[(size_t(j) * W + i) * 4 + 3];
		if (outFull) for (int c = 0; c < 4; c++) (*outFull)[(j * S + i) * 4 + c] = got[(size_t(j) * W + i) * 4 + c];
	}
	return true;
}

static bool sceneScreenOwn(Ctx& c) {
	Host& h = *c.host;
	const V3 eye = { 0, 0, 150 };
	const Camera cam = Camera::lookDir(eye, { 0, 1, 0 }, { 0, 0, 1 }, 1.2f, 1.2f);
	const OwnQuad q[kOwnQuads] = {
		{ -3000, 3000, -2000, 3000, 1000 },   // wall
		{ -260, 20, -160, 80, 640 },          // armour
		{ -100, 100, -160, 80, 600 },         // body (skin, the main list)
		{ -100, 100, 100, 300, 600 },         // head (skin, drawn after the list)
		{ -170, 170, 200, 390, 596 },         // hair
		{ 250, 400, 100, 300, 600 },          // the untraced thing
		{ 90, 320, -160, 80, 400 },           // land
		{ 100, 140, -110, -60, 395 },         // its bump, 5 units toward the camera
		{ 150, 190, -110, -60, 380 },         // 20 units
		{ 200, 260, -110, -60, 340 },         // 60 units
		{ 100, 230, -10, 70, 390 },           // a hair card 10 units before the land
		{ -480, -270, 120, 330, 600 },        // the blended rubble
		// (2026-11-12 ptgrass) a second piece of land across the top of the picture and four grass cards before it (the last block)
		{ -300, 300, 340, 460, 400 },         // the meadow
		{ -255, -150, 340, 425, 370 },        // grass A
		{ -120, -15, 340, 425, 370 },         // grass B
		{ 15, 120, 340, 425, 370 },           // grass C
		{ 150, 255, 350, 445, 397 } };        // grass D: 3 units before the meadow (a blade's base)
	const V3 wallC = { 0.30f, 0.60f, 0.30f }, armourC = { 0.70f, 0.20f, 0.10f }, skinRecord = { 0.55f, 0.40f, 0.33f }, skinScreen = { 0.60f, 0.45f, 0.35f };
	const V3 landC = { 0.45f, 0.40f, 0.30f }, rubbleC = { 0.70f, 0.60f, 0.50f };
	const V3 meadowC = { 0.40f, 0.30f, 0.20f }, grassC = { 0.20f, 0.35f, 0.10f }, twinC = { 0.05f, 0.80f, 0.05f };   // (ptgrass) the meadow's record, the grass's own record, the traced tufts' record
	const V3 tintStored = { 0.55f, 0.25f, 0.12f };
	const V3 tint = { RtScenePick::TintLinear(tintStored.x), RtScenePick::TintLinear(tintStored.y), RtScenePick::TintLinear(tintStored.z) };
	const float frameStored[3] = { 0.3f, 0.6f, 0.9f };
	const float alpha = 0.6f;
	const int slot = 87;
	const float reach = RtViewModel::OwnReach(64.0f);   // a tessellation Strength of 64 units (the setting's most): 81 (the bumps here move along the view axis: up to 1.4 x that in camera distance)
	bool ok = true;

	// ---- part 0: what a blended writer leaves in a second render target under the scene's separate alpha blend
	float rubbleAlpha[2] = { 0.0f, 1.0f };
	{
		float s0[4], s1[4], f0[4], f1[4];
		if (!ownBlendDraw(h, false, s0, s1) || !ownBlendDraw(h, true, f0, f1)) return false;
		const float sceneA = 64.0f / 255.0f;
		const bool rgb = std::fabs(s1[0] - 0.7f) < 2e-3f && std::fabs(s1[1] - 0.6f) < 2e-3f && std::fabs(s1[2] - 0.5f) < 2e-3f && std::fabs(f1[0] - 0.7f) < 2e-3f;
		const bool pass = rgb && std::fabs(s1[3]) < 1e-3f && std::fabs(f1[3] - 1.0f) < 1e-3f && std::fabs(s0[3] - sceneA) < 3e-3f && std::fabs(f0[3] - sceneA) < 3e-3f &&
			std::fabs(s0[0] - 0.1f) < 3e-3f && std::fabs(f0[0] - 0.1f) < 3e-3f;
		say("  a SRCALPHA / INVSRCALPHA draw into two targets, the scene's alpha blend ZERO / ONE (MaterialAware): target 1 = %.3f %.3f %.3f alpha %.3f (rgb written, the alpha still the clear's 0: \"unwritten\"); "
			"with ONE / ZERO + the scene's alpha masked (CapturedBlendedAlpha): target 1 alpha %.3f; the scene target's alpha %.3f / %.3f (kept at %.3f both ways), its rgb blended %.3f / %.3f -> %s\n",
			s1[0], s1[1], s1[2], s1[3], f1[3], s0[3], f0[3], sceneA, s0[0], f0[0], pass ? "ok" : "BAD");
		if (!pass) ok = false;
		rubbleAlpha[0] = s1[3]; rubbleAlpha[1] = f1[3];
	}

	// ---- the RT scene
	World w;
	w.meshes.resize(13);
	auto mq = [&](int m, const OwnQuad& o) { w.meshes[m].quad({ o.x0, o.y, o.z0 }, { o.x1, o.y, o.z0 }, { o.x1, o.y, o.z1 }, { o.x0, o.y, o.z1 }); };
	mq(0, q[kOwnWall]); mq(1, q[kOwnArmour]); mq(2, q[kOwnBody]); mq(3, q[kOwnHead]); mq(4, q[kOwnHair]); mq(5, q[kOwnLand]); mq(6, q[kOwnHairLand]); mq(7, q[kOwnRubble]);
	w.slots[slot] = [alpha](int, int) { return alpha; };
	if (!uploadSlot(h, c.alphaAtlas, slot, w.slots[slot])) return false;
	Object wall; wall.mesh = 0; wall.albedo = wallC; wall.id = 1;
	Object armour; armour.mesh = 1; armour.albedo = armourC; armour.id = 2;
	Object body; body.mesh = 2; body.actor = true; body.skin = true; body.albedo = skinRecord; body.id = 3;
	Object head; head.mesh = 3; head.actor = true; head.skin = true; head.albedo = skinRecord; head.id = 4;
	// the game's own hair: rigid (FaceGenHairNoHat), blended only -> a cut-out at 128 (RtCore.cpp BlendedHair), TINTED
	Object hair; hair.mesh = 4; hair.alphaTested = true; hair.alphaSlot = slot; hair.alphaRef = 128; hair.tinted = true; hair.albedo = tint; hair.id = 5;
	Object land; land.mesh = 5; land.land = true; land.albedo = landC; land.id = 6;   // (flat: the bumps are only in the depth buffer)
	Object hairLand = hair; hairLand.mesh = 6; hairLand.id = 7;
	const V3 rubbleRecord = { 0.35f, 0.30f, 0.25f };   // (the traced copy's own colour: not what the screen's surface shows)
	Object rubble; rubble.mesh = 7; rubble.albedo = rubbleRecord; rubble.id = 8;
	// (2026-11-12 ptgrass) the meadow, and the traced scene's own grass (RtCore::AddGrass: each tuft a still copy of its model, a foliage
	// cut-out; here alpha 0.6 everywhere = 59.8 % of the camera's rays), standing 2 / 1 units off the screen's blades (wind, bending)
	OwnQuad twinQ[4] = { q[kOwnGrassA], q[kOwnGrassB], q[kOwnGrassC], q[kOwnGrassD] };
	twinQ[0].y += 2.0f; twinQ[1].y += 2.0f; twinQ[2].y += 2.0f; twinQ[3].y += 1.0f;
	mq(8, q[kOwnMeadow]); for (int k = 0; k < 4; k++) mq(9 + k, twinQ[k]);
	Object meadow; meadow.mesh = 8; meadow.land = true; meadow.albedo = meadowC; meadow.id = 9;
	Object twin[4];
	for (int k = 0; k < 4; k++) { twin[k].mesh = 9 + k; twin[k].alphaTested = true; twin[k].alphaSlot = slot; twin[k].alphaRef = 128; twin[k].kind = NVR_RT_KIND_FOLIAGE; twin[k].albedo = twinC; twin[k].id = 10 + k; }
	w.objects = { wall, armour, body, head, hair, land, hairLand, rubble, meadow, twin[0], twin[1], twin[2], twin[3] };
	if (!RtScenePick::IsTinted(hair.recordFlags()) || NvrRtKindOf(body.recordFlags()) != NVR_RT_KIND_SKIN) {
		say("    packing: hair flags %08X (expected a tinted cut-out), body flags %08X (expected skin)\n", hair.recordFlags(), body.recordFlags()); ok = false; }
	std::vector<uint32_t> ids;
	if (!stageWorld(h, w, ids)) return false;

	// ---- the G-buffer as the game leaves it
	// blurOn: the skin blur's albedo target bound for the body's draw (SkinAlbedoFill copies it); newSkin: the fixed SkinTemplate (its
	// alpha code 2.05 + 0.4 x roughness also with AlbedoBuffer on) + SkinFill (class 6); else the skin's material texel is what lies behind;
	// rubbleA: the alpha the rubble's blended draw left in the three targets (part 0's measurement)
	const float skinRough = 0.45f;
	// (2026-11-12 ptgrass) grass: 0 = the grass cards wrote depth only, as the game's grass does without records ([Main.Main.Quality]
	// CapturedGrass off, the game's own grass shaders, lamp-lit grass): A, C and D were drawn AFTER the land (its records lie behind
	// them), B BEFORE it (GrassFirst: the land failed the depth test there, nothing is recorded); 1 = the cards wrote their own records
	// (GRASS23x000TMS.pso: colour, class 7 with the grass mark g = 0.5, roughness 0.9, their own distance) and MergeIntoNormals put the
	// record's normal into the normals buffer: A and D facing the camera, B EDGE-ON ((1, 0, 0): 2 to 15 degrees off the view's tangent
	// across the card, as the grass shader leaves a tuft's normal seen along the ground: bent just enough to face the eye), C facing
	// AWAY from the camera (a two-sided card seen from behind)
	auto build = [&](bool blurOn, bool newSkin, float rubbleA, OwnBuffers& b, int grass = 0) {
		b.depth.assign(S * S, 1.0f); b.normals.assign(S * S * 4, 0.5f); b.captured.assign(S * S * 4, 0.0f);
		b.albedo.assign(S * S * 4, 0.0f); b.material.assign(S * S * 4, 0.0f); b.blur.assign(S * S * 4, 0.0f); b.region.assign(S * S, -1); b.blade.assign(S * S, 0.0f);
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const UINT p = j * S + i;
			bool in[kOwnQuads]; bool interior = true;
			for (int k = 0; k < kOwnQuads; k++) {
				in[k] = ownInside(q[k], cam, i + 0.5f, j + 0.5f);
				for (int corner = 0; corner < 4; corner++) if (ownInside(q[k], cam, i + (corner & 1 ? 1.02f : -0.02f), j + (corner & 2 ? 1.02f : -0.02f)) != in[k]) interior = false;
			}
			const V3 d = cam.ray((i + 0.5f) / S, (j + 0.5f) / S);
			const float len = length(d);
			// the depth buffer: the nearest of everything drawn
			int nearest = -1;
			for (int k = 0; k < kOwnQuads; k++) if (in[k] && (nearest < 0 || q[k].y < q[nearest].y)) nearest = k;
			if (nearest < 0) continue;
			b.depth[p] = (q[nearest].y - eye.y) / cam.farZ;
			float* n = &b.normals[p * 4];
			n[0] = 0.5f; n[1] = 0.5f; n[2] = 0.0f; n[3] = 1.0f;   // every quad faces the camera: view-space (0, 0, -1); TESR_NormalsBuffer's alpha 1
			// the captured normals: the nearest of the main list's NVR templates (wall, armour, land, rubble: 1; the body's skin: 2, or its
			// roughness code). The land's bumps are the land's own draw: its captured distance, the FLAT land's.
			int cap = -1;
			for (int k : { kOwnWall, kOwnArmour, kOwnBody, kOwnLand, kOwnRubble, kOwnMeadow }) if (in[k] && (cap < 0 || q[k].y < q[cap].y)) cap = k;
			const int blade = nearest >= kOwnGrassA && nearest <= kOwnGrassD ? nearest : -1;   // (ptgrass) a grass card is the pixel's depth
			if (blade >= 0) b.blade[p] = 1.0f;
			if (blade == kOwnGrassB || (blade >= 0 && grass == 1)) cap = -1;
			if (cap >= 0) {
				float ex, ey; octEncodeSigned(V3(0, -1, 0), ex, ey);
				float* cp = &b.captured[p * 4];
				cp[0] = ex; cp[1] = ey; cp[2] = (q[cap].y - eye.y) * len;
				cp[3] = cap == kOwnBody ? (newSkin ? 2.05f + 0.4f * skinRough : 2.0f) : cap == kOwnRubble ? rubbleA : 1.0f;
			}
			// the albedo and material buffers: the object / land templates only; skin: the fills after the list
			int obj = -1;
			for (int k : { kOwnWall, kOwnArmour, kOwnLand, kOwnRubble, kOwnMeadow }) if (in[k] && (obj < 0 || q[k].y < q[obj].y)) obj = k;
			if (blade == kOwnGrassB || (blade >= 0 && grass == 1)) obj = -1;
			float* al = &b.albedo[p * 4]; float* mt = &b.material[p * 4];
			if (obj >= 0) {
				const V3 col = obj == kOwnWall ? wallC : obj == kOwnArmour ? armourC : obj == kOwnLand ? landC : obj == kOwnMeadow ? meadowC : rubbleC;
				const float a = obj == kOwnRubble ? rubbleA : 1.0f;
				al[0] = col.x; al[1] = col.y; al[2] = col.z; al[3] = a;
				if (obj == kOwnLand || obj == kOwnMeadow) { mt[0] = 4.0f; mt[1] = 100.0f; mt[2] = -1.0f; mt[3] = a; }            // class 4 (land): roughness 0.95
				else if (obj == kOwnRubble) { mt[0] = 1.0f; mt[1] = 100.0f; mt[2] = 0.7f; mt[3] = a; }      // class 1, no metal
				else { mt[0] = 1.0f + 0.49f * 1.0f; mt[1] = 100.0f; mt[2] = obj == kOwnWall ? 0.1f : 0.2f; mt[3] = a; }   // class 1 (object), metal 1
			}
			if (cap == kOwnBody) {
				if (blurOn) {   // SkinAlbedoFill: the skin blur's albedo (its luma in a) where the normals mark an actor
					al[0] = skinScreen.x; al[1] = skinScreen.y; al[2] = skinScreen.z; al[3] = 1.0f;
					float* bl = &b.blur[p * 4]; bl[0] = skinScreen.x; bl[1] = skinScreen.y; bl[2] = skinScreen.z; bl[3] = 0.5f;
				}
				if (newSkin) { mt[0] = 6.0f; mt[1] = 0.0f; mt[2] = skinRough; mt[3] = 1.0f; }   // SkinFill: clip(a - 2.02) passes only with the roughness code
			}
			if (blade >= 0 && grass == 1) {   // (ptgrass) the grass's own records
				const V3 gn = blade == kOwnGrassB ? V3(1, 0, 0) : blade == kOwnGrassC ? V3(0, 1, 0) : V3(0, -1, 0);
				float ex, ey; octEncodeSigned(gn, ex, ey);
				float* cp = &b.captured[p * 4];
				cp[0] = ex; cp[1] = ey; cp[2] = (q[blade].y - eye.y) * len; cp[3] = 1.0f;
				al[0] = grassC.x; al[1] = grassC.y; al[2] = grassC.z; al[3] = 1.0f;
				mt[0] = 7.0f; mt[1] = 0.5f; mt[2] = 0.9f; mt[3] = 1.0f;
				n[0] = 0.5f + 0.5f * dot(gn, cam.r); n[1] = 0.5f + 0.5f * dot(gn, cam.u); n[2] = 0.5f + 0.5f * dot(gn, cam.f);   // the view-space normal, as the merge writes it
			}
			if (!interior) continue;
			int r = -1;
			if (blade >= 0) r = kRegGrassA + (blade - kOwnGrassA);
			else if (in[kOwnMeadow]) r = kRegMeadow;
			else if (in[kOwnHair]) r = in[kOwnHead] ? kRegHairHead : kRegHairWall;
			else if (in[kOwnHead]) r = kRegHead;
			else if (in[kOwnThing]) r = kRegThing;
			else if (in[kOwnRubble]) r = kRegRubble;
			else if (in[kOwnBump5]) r = kRegBump5;
			else if (in[kOwnBump20]) r = kRegBump20;
			else if (in[kOwnBump60]) r = kRegBump60;
			else if (in[kOwnHairLand]) r = kRegHairLand;
			else if (in[kOwnLand]) r = kRegLand;
			else if (in[kOwnBody]) r = in[kOwnArmour] ? kRegBodyArmour : kRegBodyWall;
			else if (in[kOwnArmour]) r = kRegArmour;
			else r = kRegWall;
			b.region[p] = r;
		}
	};
	// the CPU's marks (RtViewModel.h's rule)
	auto wantAlpha = [&](const OwnBuffers& b, UINT p, bool marks, int skinMode, float rch) {
		const float* cp = &b.captured[p * 4];
		float a = cp[3] > 1.5f ? 2.0f : 1.0f;
		const float dw = b.depth[p];
		if (marks && dw > 0.0f && dw < 0.9999f && cp[3] > 0.99f) {
			const V3 d = cam.ray((p % S + 0.5f) / S, (p / S + 0.5f) / S);
			const float dist = length(d) * dw * cam.farZ;
			const float gap = cp[2] - dist, tol = cp[2] * RtViewModel::kFrontShare + RtViewModel::kFrontUnits;
			const bool bare = cp[3] > 1.5f && (skinMode == 2 ? b.blur[p * 4 + 3] <= 0.0f : skinMode == 1);
			if (bare || gap > tol + rch) a += RtViewModel::kOwnSure;
			else if (gap > tol) a += RtViewModel::kOwnMaybe;
		}
		return a;
	};
	auto markOf = [](float a) { const float f = a - std::floor(a); return f > 0.375f ? 2 : f > 0.125f ? 1 : 0; };   // 2 sure, 1 maybe, 0 none

	// ---- part 1: the prep pass against the CPU
	OwnBuffers gBlur, gBlurNew, gNoBlur, gRubbleOld;
	build(true, false, rubbleAlpha[1], gBlur); build(true, true, rubbleAlpha[1], gBlurNew); build(false, false, rubbleAlpha[1], gNoBlur);
	build(true, false, rubbleAlpha[0], gRubbleOld);
	int regionN[kRegions] = {};
	for (UINT p = 0; p < S * S; p++) if (gBlur.region[p] >= 0) regionN[gBlur.region[p]]++;
	{
		std::string line = "  regions (interior pixels):";
		for (int r = 0; r < kRegions; r++) { char t[96]; std::snprintf(t, sizeof(t), " %s %d,", kOwnRegionName[r], regionN[r]); line += t; if (regionN[r] < 20) ok = false; }
		say("%s\n", line.c_str());
	}
	std::vector<float> aOff, aBlur, aBlurNew, aNoBlur, aHalf, aNoReach, aRubbleOld, full;
	{
		// two more pixels for the one-sided test: a surface 1 unit before its captured distance (hair lying on a captured scalp: within the
		// tolerance) and one whose depth lies 30 units BEHIND its captured distance (parallax / displacement depth): neither is marked
		OwnBuffers t = gBlur;
		int nearPix = -1, behindPix = -1;
		for (UINT p = 0; p < S * S && (nearPix < 0 || behindPix < 0); p++) if (t.region[p] == kRegWall) { if (nearPix < 0) nearPix = int(p); else if (behindPix < 0 && p > UINT(nearPix) + 3) behindPix = int(p); }
		if (nearPix < 0 || behindPix < 0) return false;
		t.depth[nearPix] = (q[kOwnWall].y - eye.y - 1.0f) / cam.farZ;
		t.depth[behindPix] = (q[kOwnWall].y - eye.y + 30.0f) / cam.farZ;
		struct Run { const OwnBuffers* b; UINT W; bool marks; int mode; float reach; std::vector<float>* out; const char* name; };
		const Run runs[] = {
			{ &t, S, false, 0, reach, &aOff, "the marks off (the pass as it was)" },
			{ &t, S, true, 2, reach, &aBlur, "marks, the skin blur's target bound, tessellation Strength 64" },
			{ &gBlurNew, S, true, 2, reach, &aBlurNew, "marks, the fixed skin template's alpha code" },
			{ &gNoBlur, S, true, 1, reach, &aNoBlur, "marks, no skin draw bound the skin blur" },
			{ &t, 2 * S, true, 2, reach, &aHalf, "marks, RenderScale 0.5 (the world in the buffers' top-left quarter)" },
			{ &gBlur, S, true, 2, 0.0f, &aNoReach, "marks WITHOUT the tessellation's reach (the first PTScreenOwn build with tessellated bumps)" },
			{ &gRubbleOld, S, true, 2, reach, &aRubbleOld, "marks, the rubble's alpha as the old blend state left it" } };
		for (const Run& r : runs) {
			if (!ownPrep(h, cam, *r.b, r.W, r.marks, r.mode, r.reach, *r.out, r.b == &t && r.W == S && r.marks ? &full : nullptr)) return false;
			int bad = 0, marked[3] = {}, shown = 0;
			for (UINT p = 0; p < S * S; p++) {
				const float want = wantAlpha(*r.b, p, r.marks, r.mode, r.reach), got = (*r.out)[p];
				if (got != want) { bad++; if (shown++ < 4) say("    pixel (%u, %u): alpha %.3f, the CPU %.3f\n", p % S, p / S, got, want); }
				marked[markOf(got)]++;
			}
			// per region: the mark it must carry (2 sure, 1 maybe, 0 none)
			int wrong = 0;
			for (UINT p = 0; p < S * S; p++) {
				const int reg = r.b->region[p];
				if (reg < 0) continue;
				const bool body = reg == kRegBodyArmour || reg == kRegBodyWall;
				int want = 0;
				if (r.marks) {
					if (reg == kRegHead || reg == kRegHairHead || reg == kRegHairWall || reg == kRegThing || (body && r.mode == 1)) want = 2;
					else if (reg == kRegBump20 || reg == kRegBump60 || reg == kRegHairLand || reg == kRegGrassA || reg == kRegGrassC) want = r.reach > 0.0f ? 1 : 2;   // (ptgrass: grass 30 units before the land's record)
				}
				if (markOf((*r.out)[p]) != want) wrong++;
				if (body != ((*r.out)[p] > 1.5f)) wrong++;   // the person code on the body's skin only
			}
			if (r.b == &t && r.marks) {
				if ((*r.out)[nearPix] != 1.0f || (*r.out)[behindPix] != 1.0f) { wrong++; say("    the one-sided test: 1 unit before the captured distance %.2f, 30 units behind it %.2f (both must stay 1)\n", (*r.out)[nearPix], (*r.out)[behindPix]); }
			}
			const bool pass = bad == 0 && wrong == 0;
			say("  prep pass, %s: %d pixels differ from the CPU, %d interior pixels with the wrong mark; marked: %d sure, %d maybe -> %s\n", r.name, bad, wrong, marked[2], marked[1], pass ? "ok" : "BAD");
			if (!pass) ok = false;
		}
		// UNOFFICIAL 2026-11-12 PTWaterMask (RtViewModel.h s5 / c9; core/WaterMask.h): the game's water mask over the left third of the picture
		// (whatever lies there: wall, hair, a head, skin, bumps), a strip of it below the threshold (one sample of four). Every masked pixel
		// must come out -1 (the game's own pixel) and every other pixel exactly as the CPU's rule without a mask; at RenderScale 0.5 too.
		{
			std::vector<float> mask(S * S, 0.0f), aWater;
			for (UINT p = 0; p < S * S; p++) if (p % S < S / 3) mask[p] = (p / S) % 16 == 5 ? 0.25f : 1.0f;
			for (UINT W : { S, 2 * S }) {
				if (!ownPrep(h, cam, t, W, true, 2, reach, aWater, nullptr, &mask)) return false;
				int bad = 0, masked = 0, shown = 0;
				for (UINT p = 0; p < S * S; p++) {
					const bool wet = mask[p] > RtViewModel::kWaterShare;
					const float want = wet ? -1.0f : wantAlpha(t, p, true, 2, reach), got = aWater[p];
					if (wet) masked++;
					if (got != want) { bad++; if (shown++ < 4) say("    pixel (%u, %u): alpha %.3f, the CPU %.3f\n", p % S, p / S, got, want); }
				}
				const bool pass = bad == 0 && masked > 0;
				say("  prep pass, PTWaterMask (buffers %u, world %u): %d masked pixels must be -1, the rest as without a mask: %d pixels differ from the CPU -> %s\n", W, S, masked, bad, pass ? "ok" : "BAD");
				if (!pass) ok = false;
			}
			// (2026-11-13 PTAlbedoCoverage) c9.w = 0.1, as with the game's MSAA: the strip at one sample of four (0.25) is masked too
			{
				if (!ownPrep(h, cam, t, S, true, 2, reach, aWater, nullptr, &mask, 0.1f)) return false;
				int bad = 0, strip = 0;
				for (UINT p = 0; p < S * S; p++) {
					const bool wet = mask[p] > 0.1f;
					if (wet && mask[p] < 0.3f) strip++;
					if (aWater[p] != (wet ? -1.0f : wantAlpha(t, p, true, 2, reach))) bad++;
				}
				const bool pass = bad == 0 && strip > 0;
				say("  prep pass, the mask's threshold 0.1 (PTAlbedoCoverage, MSAA): %d pixels at one sample of four are masked too; %d pixels differ from the CPU -> %s\n", strip, bad, pass ? "ok" : "BAD");
				if (!pass) ok = false;
			}
		}
		// the normal passes through untouched
		int nbad = 0;
		for (UINT p = 0; p < S * S; p++) for (int ch = 0; ch < 3; ch++) if (std::fabs(full[p * 4 + ch] - t.normals[p * 4 + ch]) > 2e-3f) { nbad++; break; }
		if (nbad) { say("    the prep pass changed %d pixels' normals\n", nbad); ok = false; }
		// the two test pixels are not part of the trace's G-buffer: the marks of the clean buffers
		aBlur[nearPix] = aBlur[behindPix] = 1.0f;
	}

	// ---- part 2: the path tracer's first surface
	std::vector<float> frame(S * S * 4, 0.0f);
	for (UINT p = 0; p < S * S; p++) { frame[p * 4] = frameStored[0]; frame[p * 4 + 1] = frameStored[1]; frame[p * 4 + 2] = frameStored[2]; frame[p * 4 + 3] = 1.0f; }
	if (!h.Upload(gM.aoLight, frame)) return false;
	// hairBits: frame row texel 30.x (RtPathTrace.cpp: 3 = BlendedHair, 2 = BlendedHair with PTBlendedLayer: no first-surface bit)
	// (2026-11-12) the frame row's texels 291.y PTAlbedoGamma's exponent, 292.x PTScreenOwnNarrow, 292.y PTHairOverSky (0: as the 11-11 build)
	float keyGamma = 0.0f, keyNarrow = 0.0f, keyHairSky = 0.0f;
	// (2026-11-12 ptbump) 293.y PTBumpNormals, 294.x PTBumpDetail, 294.y PTMatteNoHighlight (0: as before)
	float keyBump = 0.0f, keyBumpDetail = 0.0f, keyMatte = 0.0f;
	float keyCover = 0.0f;   // (2026-11-12 ptgrass) texel 297.x PTAlbedoCoverage (set by the DLL only in a frame with the game's MSAA)
	auto run = [&](const OwnBuffers& b, const std::vector<float>& alphaCode, int view, float hairBits, std::vector<float>& out) {
		std::vector<float> normals = b.normals;
		for (UINT p = 0; p < S * S; p++) normals[p * 4 + 3] = alphaCode[p];
		if (!h.Upload(c.depth, b.depth) || !h.Upload(c.normals, normals) || !h.Upload(c.aux16, b.albedo) || !h.Upload(c.histA, b.material)) return false;
		std::vector<float> image(size_t(kLightTexels) * kLampRows * 2, 0.0f);
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 60] = hairBits;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 291 + 1] = keyGamma;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 292] = keyNarrow;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 292 + 1] = keyHairSky;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 293 + 1] = keyBump;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 294] = keyBumpDetail;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 294 + 1] = keyMatte;
		image[size_t(kLightTexels) * kFrameRowIndex * 2 + 2 * 297] = keyCover;
		if (!h.Upload(gM.ptLamps, image)) return false;
		PathConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.origin[3] = 17.0f;
		put4(k.accum, 0.0f, 1.0f, 64.0f, float(1u | 8u | 32u | 256u | 1024u));   // alpha test, G-buffer, frame, MaterialBuffer, lamp image
		put4(k.sunDir, 0, 0, 1, 0); put4(k.sunColor, 0, 0, 0, 0);
		put4(k.skyZenith, 0.0f, 0.0f, 0.0f, 0.0f); put4(k.skyHorizon, 0.0f, 0.0f, 0.0f, 0.0f);
		put4(k.misc, 0.0f, 0.0f, 60000.0f, float(view));
		k.rect[3] = 1.0f;
		const std::vector<int> imgs = { c.ptA, c.ptB, c.ptDisplay, c.alphaAtlas, c.ptDisplay, c.ptDisplay, c.depth, c.normals, c.aux16, c.pano,
			gM.aoLight, c.ptDisplay, c.histA, gM.ptAlbedoOut, gM.ptLamps };
		h.AddPass(c.shPath, imgs, 0x7u, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, eye)) return false;
		const bool read = h.Read(c.ptDisplay, out);
		h.EndFrame();
		return read;
	};
	auto mean = [&](const OwnBuffers& b, const std::vector<float>& img, int region, float m[3]) {
		double s[3] = {}; int n = 0;
		for (UINT p = 0; p < S * S; p++) if (b.region[p] == region) { for (int ch = 0; ch < 3; ch++) s[ch] += img[p * 4 + ch]; n++; }
		for (int ch = 0; ch < 3; ch++) m[ch] = float(s[ch] / std::max(n, 1));
	};
	// the share of the camera's rays the hair card keeps (RtAlpha.glsl's hashed test: threshold uniform over ref +- width / 2)
	const float ref = 128.0f / 255.0f, width = std::min(1.0f, 2.0f * std::min(ref, 1.0f - ref));
	const float keep = std::min(std::max((alpha - (ref - 0.5f * width)) / width, 0.0f), 1.0f);
	const V3 frameLin = { std::pow(frameStored[0], 2.2f), std::pow(frameStored[1], 2.2f), std::pow(frameStored[2], 2.2f) };
	auto mix = [&](V3 a, V3 b2) { return a * keep + b2 * (1.0f - keep); };
	struct Want { bool check; V3 v; };
	auto judge = [&](const char* what, const OwnBuffers& b, const std::vector<float>& img, const Want want[kRegions], float tol) {
		bool all = true; std::string line;
		for (int r = 0; r < kRegions; r++) {
			if (!want[r].check) continue;
			float m[3]; mean(b, img, r, m);
			const bool good = std::fabs(m[0] - want[r].v.x) <= tol && std::fabs(m[1] - want[r].v.y) <= tol && std::fabs(m[2] - want[r].v.z) <= tol;
			if (!good) all = false;
			char t[200]; std::snprintf(t, sizeof(t), "\n      %-30s %.3f %.3f %.3f  (expected %.3f %.3f %.3f)%s", kOwnRegionName[r], m[0], m[1], m[2], want[r].v.x, want[r].v.y, want[r].v.z, good ? "" : "  <--");
			line += t;
		}
		say("  %s -> %s%s\n", what, all ? "ok" : "BAD", line.c_str());
		if (!all) ok = false;
	};
	// "before": the old DLL ran no prep pass in third person: TESR_NormalsBuffer's alpha, 1 everywhere
	const std::vector<float> ones(S * S, 1.0f);
	std::vector<float> o;
	// -- colour (PTDebugView 1), skin blur on
	{
		Want before[kRegions] = {}, after[kRegions] = {};
		const V3 same[kRegions] = { wallC, armourC, skinScreen, skinScreen, wallC, wallC, wallC, wallC, landC, landC, landC, landC, landC, rubbleC };
		for (int r = 0; r < kRegMeadow; r++) { before[r] = { true, same[r] }; after[r] = { true, same[r] }; }   // (the grass cases: the last block)
		before[kRegHairHead] = { true, mix(tint, wallC) }; before[kRegHairWall] = { true, mix(tint, wallC) }; before[kRegHairLand] = { true, mix(tint, landC) };
		after[kRegHead] = { true, skinRecord }; after[kRegHairHead] = { true, mix(tint, skinRecord) }; after[kRegHairWall] = { true, mix(tint, wallC) };
		after[kRegThing] = { true, frameLin }; after[kRegHairLand] = { true, mix(tint, landC) };
		if (!run(gBlur, ones, 1, 3.0f, o)) return false;
		judge("BEFORE (no marks), colour: the head and the thing are the WALL's colour, hair over the head is hair + the wall", gBlur, o, before, 0.012f);
		if (!run(gBlur, aBlur, 1, 3.0f, o)) return false;
		judge("AFTER, colour: the head its own skin, hair over it hair + the head's skin, the thing the game's pixel; the land's bumps and the rubble their own captured colour; the rest unchanged", gBlur, o, after, 0.012f);
	}
	// -- the same with PTBlendedLayer's hair bits (no first-surface bit): before, every hair pixel is what lies behind it
	{
		Want bl[kRegions] = {}; bl[kRegHairHead] = { true, wallC }; bl[kRegHairLand] = { true, landC };
		if (!run(gBlur, ones, 1, 2.0f, o)) return false;
		judge("BEFORE with PTBlendedLayer's hair bits, colour: hair over the head is 100 % the wall, hair before the land 100 % the land", gBlur, o, bl, 0.012f);
		bl[kRegHairHead] = { true, mix(tint, skinRecord) }; bl[kRegHairLand] = { true, mix(tint, landC) };
		if (!run(gBlur, aBlur, 1, 2.0f, o)) return false;
		judge("AFTER with PTBlendedLayer's hair bits, colour: hair + the head's skin; hair + the land (a MAYBE mark: the traced hair takes over)", gBlur, o, bl, 0.012f);
	}
	// -- tessellated bumps with the first PTScreenOwn build's marks (no reach): a bump raised past the same-surface test is the game's pixel
	{
		Want nb[kRegions] = {}; nb[kRegBump5] = { true, landC }; nb[kRegBump20] = { true, frameLin }; nb[kRegBump60] = { true, frameLin };
		if (!run(gBlur, aNoReach, 1, 3.0f, o)) return false;
		judge("CONTROL, marks without the tessellation's reach: the 20 and 60 unit bumps are the game's raw pixel (what the two-level mark prevents)", gBlur, o, nb, 0.012f);
	}
	// -- the blended rubble with the alpha the old blend state left: the game's pixel (the tester's bright patches), marks on or off
	{
		Want rb[kRegions] = {}; rb[kRegRubble] = { true, frameLin };
		if (!run(gRubbleOld, ones, 1, 3.0f, o)) return false;
		judge("BEFORE CapturedBlendedAlpha (1.0.7, no marks), colour: the blended rubble is the game's raw pixel", gRubbleOld, o, rb, 0.012f);
		if (!run(gRubbleOld, aRubbleOld, 1, 3.0f, o)) return false;
		judge("BEFORE CapturedBlendedAlpha, PTScreenOwn on: the same (the marks do not touch it)", gRubbleOld, o, rb, 0.012f);
		Want rm[kRegions] = {}; rm[kRegRubble] = { true, { 0.7f, 0, 0 } };
		if (!run(gBlur, aBlur, 4, 3.0f, o)) return false;
		judge("AFTER CapturedBlendedAlpha, roughness / metal: the rubble's own captured material", gBlur, o, rm, 0.012f);
	}
	// -- roughness, metal (PTDebugView 4), skin blur on, the old skin shader (the body's material texel = the wall's / armour's)
	const float hairRough = 0.8f, recordRough = 0.8f;
	{
		Want before[kRegions] = {}, after[kRegions] = {};
		before[kRegWall] = after[kRegWall] = { true, { 0.1f, 1, 0 } }; before[kRegArmour] = after[kRegArmour] = { true, { 0.2f, 1, 0 } };
		before[kRegLand] = after[kRegLand] = { true, { 0.95f, 0, 0 } };
		before[kRegBodyArmour] = { true, { 0.2f, 1, 0 } }; before[kRegBodyWall] = { true, { 0.1f, 1, 0 } }; before[kRegHead] = { true, { 0.1f, 1, 0 } };
		before[kRegHairHead] = { true, mix(V3(hairRough, 0, 0), V3(0.1f, 1, 0)) };
		after[kRegBodyArmour] = { true, { 0.6f, 0, 0 } }; after[kRegBodyWall] = { true, { 0.6f, 0, 0 } }; after[kRegHead] = { true, { recordRough, 0, 0 } };
		after[kRegHairHead] = { true, mix(V3(hairRough, 0, 0), V3(recordRough, 0, 0)) };
		if (!run(gBlur, ones, 4, 3.0f, o)) return false;
		judge("BEFORE, roughness / metal: the body's skin, the head and 40 % of the hair carry the METAL of the wall / armour behind", gBlur, o, before, 0.012f);
		if (!run(gBlur, aBlur, 4, 3.0f, o)) return false;
		judge("AFTER, roughness / metal (old skin shader in the cache): no metal on skin or hair; the body a skin roughness 0.6", gBlur, o, after, 0.012f);
		Want m2[kRegions] = {}; m2[kRegBodyArmour] = { true, { skinRough, 0, 0 } }; m2[kRegBodyWall] = { true, { skinRough, 0, 0 } };
		if (!run(gBlurNew, aBlurNew, 4, 3.0f, o)) return false;
		judge("AFTER, roughness / metal with the fixed SkinTemplate (SkinFill: class 6): the body's own roughness 0.45, no metal", gBlurNew, o, m2, 0.012f);
	}
	// -- the normal (PTDebugView 2): the head's own (traced, smooth)
	{
		Want n2[kRegions] = {}; n2[kRegHead] = { true, { 0.5f, 0.0f, 0.5f } }; n2[kRegBodyWall] = { true, { 0.5f, 0.0f, 0.5f } };
		if (!run(gBlur, aBlur, 2, 3.0f, o)) return false;
		judge("AFTER, normal: the head's and the body's own (0, -1, 0)", gBlur, o, n2, 0.01f);
	}
	// -- no skin blur (SkinScattering off / ScreenSpace off / not supported): the body's colour
	{
		Want b0[kRegions] = {}; b0[kRegBodyArmour] = { true, armourC }; b0[kRegBodyWall] = { true, wallC };
		if (!run(gNoBlur, ones, 1, 3.0f, o)) return false;
		judge("BEFORE, no skin blur, colour: the body is the colour of the armour / wall drawn before it", gNoBlur, o, b0, 0.012f);
		b0[kRegBodyArmour] = { true, skinRecord }; b0[kRegBodyWall] = { true, skinRecord };
		if (!run(gNoBlur, aNoBlur, 1, 3.0f, o)) return false;
		judge("AFTER, no skin blur, colour: the body its own skin record", gNoBlur, o, b0, 0.012f);
		Want m0[kRegions] = {}; m0[kRegBodyArmour] = { true, { recordRough, 0, 0 } }; m0[kRegBodyWall] = { true, { recordRough, 0, 0 } };
		if (!run(gNoBlur, aNoBlur, 4, 3.0f, o)) return false;
		judge("AFTER, no skin blur, roughness / metal: the body's own record, no metal", gNoBlur, o, m0, 0.012f);
	}
	// ---- 2026-11-12 (docs/next/PT-COMPAT-AUDIT-2026-11-12.md): the fix batch's keys
	// -- PTScreenOwnNarrow: everything PTScreenOwn fixed still holds ...
	{
		Want after[kRegions] = {};
		const V3 same[kRegions] = { wallC, armourC, skinScreen, skinScreen, wallC, wallC, wallC, wallC, landC, landC, landC, landC, landC, rubbleC };
		for (int r = 0; r < kRegMeadow; r++) after[r] = { true, same[r] };
		after[kRegHead] = { true, skinRecord }; after[kRegHairHead] = { true, mix(tint, skinRecord) }; after[kRegHairWall] = { true, mix(tint, wallC) };
		after[kRegThing] = { true, frameLin }; after[kRegHairLand] = { true, mix(tint, landC) };
		keyNarrow = 1.0f;
		if (!run(gBlur, aBlur, 1, 3.0f, o)) return false;
		judge("NARROW, colour: exactly the AFTER picture (hair, the head's skin, the untraced thing, the bumps, the rubble)", gBlur, o, after, 0.012f);
		// ... and a SURE-marked pixel whose traced hit at its depth is a plain world surface (the marks without the reach: the hair card 10
		// units before the land; the 40 % of rays that pass it meet the land inside the same-surface distance: a puddle over its bed,
		// faded rubble over land) is the game's own pixel, where the 11-11 rule took the traced land's record
		Want hl[kRegions] = {}; hl[kRegHairLand] = { true, mix(tint, landC) };
		keyNarrow = 0.0f;
		if (!run(gBlur, aNoReach, 1, 3.0f, o)) return false;
		judge("CONTROL (11-11 rule), sure mark over the land: rays past the card take the traced LAND's record", gBlur, o, hl, 0.012f);
		hl[kRegHairLand] = { true, mix(tint, frameLin) };
		keyNarrow = 1.0f;
		if (!run(gBlur, aNoReach, 1, 3.0f, o)) return false;
		judge("NARROW, sure mark over the land: rays past the card are the game's own pixel", gBlur, o, hl, 0.012f);
		keyNarrow = 0.0f;
	}
	// -- PTAlbedoGamma: the screen's colours ^ (2.2 / 1.45); the traced records' colours unchanged
	{
		const float e = 2.2f / 1.45f;
		auto pw = [&](V3 v) { return V3(std::pow(v.x, e), std::pow(v.y, e), std::pow(v.z, e)); };
		Want g[kRegions] = {}; g[kRegWall] = { true, pw(wallC) }; g[kRegArmour] = { true, pw(armourC) }; g[kRegLand] = { true, pw(landC) };
		g[kRegHead] = { true, skinRecord }; g[kRegThing] = { true, frameLin };
		keyGamma = e;
		if (!run(gBlur, aBlur, 1, 3.0f, o)) return false;
		judge("GAMMA 1.45, colour: the wall, armour and land are their buffer colour ^ 1.517; the head's traced record and the game's pixel unchanged", gBlur, o, g, 0.012f);
		keyGamma = 0.0f;
	}
	// -- PTHairOverSky: the hair card with NOTHING recorded behind it (the wall's AlbedoBuffer alpha 0 there, as the sky leaves it; no marks:
	// the prep pass marks only written texels)
	{
		OwnBuffers sky = gBlur;
		for (UINT p2 = 0; p2 < S * S; p2++) if (sky.region[p2] == kRegHairWall) sky.albedo[p2 * 4 + 3] = 0.0f;
		Want hs[kRegions] = {}; hs[kRegHairWall] = { true, frameLin }; hs[kRegWall] = { true, wallC };
		if (!run(sky, ones, 1, 3.0f, o)) return false;
		judge("BEFORE PTHairOverSky, hair with nothing recorded behind it: 100 % the game's own pixel", sky, o, hs, 0.012f);
		hs[kRegHairWall] = { true, mix(tint, frameLin) };
		keyHairSky = 1.0f;
		if (!run(sky, ones, 1, 3.0f, o)) return false;
		judge("AFTER PTHairOverSky: the rays the card keeps are the traced hair, the rest the game's pixel", sky, o, hs, 0.012f);
		keyHairSky = 0.0f;
	}
	// ---- 2026-11-12 (ptbump; a tester: "textures are missing their normals/bumpmaps", "a gross sheen over everything")
	// -- PTMatteNoHighlight: the rubble's record coded "the game draws no highlight on this mesh" (b = -2 - its roughness 0.1: a normal
	// map's alpha of 0.9 on a mesh without the specular flag, Includes/CapturedNormals.hlsl). Key off: the roughness as recorded before
	// the code (0.1: a near mirror to the tracer); on: matte (1). The wall's and the armour's uncoded records (0.1 / 0.2, metal 1) and
	// the land (0.95) stay.
	{
		OwnBuffers coded = gBlur;
		for (UINT p2 = 0; p2 < S * S; p2++) if (coded.material[p2 * 4] > 0.5f && coded.material[p2 * 4] < 1.2f && coded.material[p2 * 4 + 2] == 0.7f) coded.material[p2 * 4 + 2] = -2.0f - 0.1f;
		Want m[kRegions] = {}; m[kRegRubble] = { true, { 0.1f, 0, 0 } }; m[kRegWall] = { true, { 0.1f, 1, 0 } }; m[kRegArmour] = { true, { 0.2f, 1, 0 } }; m[kRegLand] = { true, { 0.95f, 0, 0 } };
		keyNarrow = 1.0f;
		if (!run(coded, aBlur, 4, 3.0f, o)) return false;
		judge("BEFORE PTMatteNoHighlight, roughness / metal: a no-highlight mesh's coded record reads as its old roughness 0.1 (a mirror)", coded, o, m, 0.004f);
		m[kRegRubble] = { true, { 1.0f, 0, 0 } };
		keyMatte = 1.0f;
		if (!run(coded, aBlur, 4, 3.0f, o)) return false;
		judge("AFTER PTMatteNoHighlight: that mesh is matte (roughness 1); the shiny wall and armour and the land as before", coded, o, m, 0.004f);
		keyMatte = 0.0f;
	}
	// -- PTBumpNormals, the land's record: its bumped normal in the MaterialBuffer's g / b (TerrainTemplate.hlsl / CapturedLandBumpOut:
	// g = 3 + e.x, b = -7 + e.y, e hemi-octahedral about +z). Key off: the smooth normal (0, -1, 0) everywhere; on: the land's pixels
	// (flat, where the traced hit is the same surface, and its 60-unit bump, where only the screen's surface counts) take the recorded
	// normal as their shading normal; the wall (no record) keeps its own. The display's alpha says where: 2 + PTBumpDetail.
	{
		const V3 bump = normalize(V3(0.30f, -0.80f, 0.52f));
		const float sum = std::fabs(bump.x) + std::fabs(bump.y) + bump.z, hx = bump.x / sum, hy = bump.y / sum;
		OwnBuffers land2 = gBlur;
		for (UINT p2 = 0; p2 < S * S; p2++) if (land2.material[p2 * 4] == 4.0f) { land2.material[p2 * 4 + 1] = 3.0f + hx + hy; land2.material[p2 * 4 + 2] = -7.0f + hx - hy; }
		const V3 flatN = { 0.5f, 0.0f, 0.5f }, bumpN = bump * 0.5f + V3(0.5f, 0.5f, 0.5f);
		Want n[kRegions] = {}; n[kRegLand] = { true, flatN }; n[kRegBump60] = { true, flatN }; n[kRegWall] = { true, flatN };
		keyNarrow = 1.0f;
		if (!run(land2, aBlur, 2, 3.0f, o)) return false;
		judge("BEFORE PTBumpNormals, normal: the land with a recorded bump is lit with its smooth normal (0, -1, 0)", land2, o, n, 0.004f);
		n[kRegLand] = { true, bumpN }; n[kRegBump60] = { true, bumpN };
		keyBump = 1.0f; keyBumpDetail = 0.25f;
		if (!run(land2, aBlur, 2, 3.0f, o)) return false;
		judge("AFTER PTBumpNormals: the land's shading normal is its recorded bumped normal (0.30, -0.80, 0.52); the wall keeps its own", land2, o, n, 0.004f);
		// the display's alpha (the show pass's PTDetail share + 2): 2.25 on the land, 1 on the wall
		double aLand = 0, aWall = 0; int nLand = 0, nWall = 0;
		for (UINT p2 = 0; p2 < S * S; p2++) { if (land2.region[p2] == kRegLand) { aLand += o[p2 * 4 + 3]; nLand++; } else if (land2.region[p2] == kRegWall) { aWall += o[p2 * 4 + 3]; nWall++; } }
		aLand /= std::max(nLand, 1); aWall /= std::max(nWall, 1);
		const bool alphaOk = std::fabs(aLand - 2.25) < 2e-3 && std::fabs(aWall - 1.0) < 2e-3;
		say("  PTBumpNormals, the display's alpha (PTDetail's share there = it - 2): the land %.3f (expected 2.250 with PTBumpDetail 0.25), the wall %.3f (1: all of PTDetail) -> %s\n", aLand, aWall, alphaOk ? "ok" : "BAD");
		if (!alphaOk) ok = false;
		// a record averaged at an edge (the land's class, its g out of the code's range): no bump
		OwnBuffers edge = land2;
		for (UINT p2 = 0; p2 < S * S; p2++) if (edge.material[p2 * 4] == 4.0f) edge.material[p2 * 4 + 1] = 100.0f;
		n[kRegLand] = { true, flatN }; n[kRegBump60] = { true, flatN };
		if (!run(edge, aBlur, 2, 3.0f, o)) return false;
		judge("PTBumpNormals, a land texel without the code (g = a radius, as the 6- and 7-layer shaders leave it): the smooth normal", edge, o, n, 0.004f);
		keyBump = 0.0f; keyBumpDetail = 0.0f; keyNarrow = 0.0f;
	}
	// ---- 2026-11-12 (ptgrass; a tester: "grass disappears / reappears depending on viewing angle" under path tracing): GRASS.
	// Without records ([Main.Main.Quality] CapturedGrass off; the game's own grass shaders; lamp-lit grass) a blade's pixel has the grass's
	// depth and whatever the buffers held before it. What it becomes is decided by how far the blade stands before the recorded land ALONG
	// THE VIEW RAY against the prep pass's tolerance (1 % of the distance + 2 units; + the tessellation's reach): beyond it the game's own
	// pixel, within it the LAND's colour at the blade's place: the blade is gone. That distance is the blade's height over the ground
	// divided by the sine of the angle between the ray and the ground: it changes with the view angle. With the records every blade pixel
	// is the grass's own surface, whatever its normal does and whatever the traced scene's own tufts do.
	{
		OwnBuffers gOff, gOn;
		build(true, false, rubbleAlpha[1], gOff, 0); build(true, false, rubbleAlpha[1], gOn, 1);
		std::vector<float> aOffSure, aOffReach, aOn, aOnReach;
		if (!ownPrep(h, cam, gOff, S, true, 2, 0.0f, aOffSure) || !ownPrep(h, cam, gOff, S, true, 2, reach, aOffReach) ||
			!ownPrep(h, cam, gOn, S, true, 2, 0.0f, aOn) || !ownPrep(h, cam, gOn, S, true, 2, reach, aOnReach)) return false;
		// the prep pass: recorded grass carries no mark (its captured distance is its own), with or without the tessellation's reach
		{
			int marked = 0, n = 0;
			for (UINT p2 = 0; p2 < S * S; p2++) if (gOn.region[p2] >= kRegGrassA) { n++; if (markOf(aOn[p2]) != 0 || markOf(aOnReach[p2]) != 0) marked++; }
			say("  GRASS, prep pass with the grass's own records: %d of %d grass pixels marked (must be 0) -> %s\n", marked, n, marked == 0 && n > 80 ? "ok" : "BAD");
			if (marked != 0 || n <= 80) ok = false;
		}
		keyNarrow = 1.0f;   // (as 1.0.8: PTScreenOwn + PTScreenOwnNarrow)
		Want g[kRegions] = {};
		// -- no records: A and C (30 units before the land's record: a sure mark) and B (nothing recorded behind it) are the game's own
		// pixel; D (3 units before it, inside the tolerance: a blade's base, or a whole blade seen from above at a distance) is the LAND
		g[kRegMeadow] = { true, meadowC }; g[kRegGrassA] = { true, frameLin }; g[kRegGrassB] = { true, frameLin }; g[kRegGrassC] = { true, frameLin }; g[kRegGrassD] = { true, meadowC };
		if (!run(gOff, aOffSure, 1, 3.0f, o)) return false;
		judge("GRASS without records, colour: 30 units before the land = the game's own pixel; 3 units before it = the LAND's colour (the blade is gone)", gOff, o, g, 0.012f);
		// -- no records, the tessellation on (its reach makes the 30 unit blades a MAYBE mark): they take the land's colour too
		g[kRegGrassA] = { true, meadowC }; g[kRegGrassC] = { true, meadowC };
		if (!run(gOff, aOffReach, 1, 3.0f, o)) return false;
		judge("GRASS without records, land tessellation on (reach 81), colour: the blades 30 units before the land are the LAND's colour as well", gOff, o, g, 0.012f);
		// -- PTGrassOwnPixel: the grass draws that wrote no record are drawn once more into the mask of the game's own pixels (core/
		// WaterMask.h RedrawGrass; the prep pass's s5 / c9): every blade is the game's own pixel, also the one inside the tolerance and the
		// ones inside the tessellation's reach: nothing of the grass takes the land's colour, at any distance before it
		{
			std::vector<float> aMask, aMaskReach;
			if (!ownPrep(h, cam, gOff, S, true, 2, 0.0f, aMask, nullptr, &gOff.blade) || !ownPrep(h, cam, gOff, S, true, 2, reach, aMaskReach, nullptr, &gOff.blade)) return false;
			Want gm2[kRegions] = {};
			gm2[kRegMeadow] = { true, meadowC };
			for (int r = kRegGrassA; r <= kRegGrassD; r++) gm2[r] = { true, frameLin };
			if (!run(gOff, aMask, 1, 3.0f, o)) return false;
			judge("GRASS without records, PTGrassOwnPixel (the grass in the mask), colour: all four blades the game's own pixel, the meadow the land", gOff, o, gm2, 0.012f);
			if (!run(gOff, aMaskReach, 1, 3.0f, o)) return false;
			judge("GRASS without records, PTGrassOwnPixel, land tessellation on: the same", gOff, o, gm2, 0.012f);
		}
		// -- PTAlbedoCoverage (the game's MSAA; captured frame of bench run 20261113-005828-slotC): the meadow as a resolve leaves it where a
		// grass card without records covered one sample of four (left third), two (middle), three (right): the colour, the material and
		// the alpha at 3 / 4, 2 / 4, 1 / 4 of the land's. Before: from half the alpha up the pixel is a surface with the land's colour at
		// 75 % / 50 % (and a class of 3 / 2), below it the game's own pixel. With the key: the game's own pixel at every share, and a frame
		// whose alphas are all 1 is not touched. (What would have caught the first form: that one only made the 75 % land its own colour
		// again; the picture needs the pixel NOT to be a surface at all.)
		{
			OwnBuffers gMs = gOff;
			int third[3] = {};
			for (UINT p2 = 0; p2 < S * S; p2++) if (gMs.region[p2] == kRegMeadow) {
				const int k3 = int(p2 % S) * 3 / int(S);
				const float share = k3 == 0 ? 0.75f : k3 == 1 ? 0.5f : 0.25f;
				for (int ch = 0; ch < 4; ch++) { gMs.albedo[p2 * 4 + ch] *= share; gMs.material[p2 * 4 + ch] *= share; }
				gMs.region[p2] = k3 == 0 ? kRegMeadow : k3 == 1 ? kRegGrassA : kRegGrassC;   // (the three shares, judged as three regions; the cards' own pixels below)
				third[k3]++;
			}
			for (UINT p2 = 0; p2 < S * S; p2++) if (gOff.region[p2] >= kRegGrassA) gMs.region[p2] = -1;
			Want gc[kRegions] = {};
			gc[kRegMeadow] = { true, meadowC * 0.75f }; gc[kRegGrassA] = { true, meadowC * 0.5f }; gc[kRegGrassC] = { true, frameLin };
			if (!run(gMs, aOffSure, 1, 3.0f, o)) return false;
			judge("MSAA means, BEFORE PTAlbedoCoverage, colour (regions: the meadow = 3 of 4 samples written, grass A = 2 of 4, grass C = 1 of 4): surfaces at 75 % and 50 % of the land's colour, the game's pixel at 25 %", gMs, o, gc, 0.012f);
			keyCover = 1.0f;
			gc[kRegMeadow] = { true, frameLin }; gc[kRegGrassA] = { true, frameLin }; gc[kRegGrassC] = { true, frameLin };
			if (!run(gMs, aOffSure, 1, 3.0f, o)) return false;
			judge("MSAA means, PTAlbedoCoverage, colour: the game's own pixel at every share", gMs, o, gc, 0.012f);
			if (third[0] < 20 || third[1] < 20 || third[2] < 20) { say("    too few pixels in a share (%d %d %d)\n", third[0], third[1], third[2]); ok = false; }
			// ... and a frame without unwritten samples (every alpha 1) is not touched by the key
			Want gs[kRegions] = {}; gs[kRegMeadow] = { true, meadowC }; gs[kRegWall] = { true, wallC }; gs[kRegLand] = { true, landC };
			if (!run(gOff, aOffSure, 1, 3.0f, o)) return false;
			judge("PTAlbedoCoverage on a frame whose alphas are 1: unchanged", gOff, o, gs, 0.012f);
			keyCover = 0.0f;
		}
		// -- the grass's own records: every blade its own colour, 100 % of the rays (never the land, the game's pixel or the traced tufts')
		for (int r = kRegGrassA; r <= kRegGrassD; r++) g[r] = { true, grassC };
		if (!run(gOn, aOn, 1, 3.0f, o)) return false;
		judge("GRASS with its own records (CapturedGrass), colour: all four blades the grass's own colour", gOn, o, g, 0.012f);
		if (!run(gOn, aOnReach, 1, 3.0f, o)) return false;
		judge("GRASS with its own records, land tessellation on, colour: the same", gOn, o, g, 0.012f);
		// -- its normal: facing the camera as recorded; EDGE-ON ((1, 0, 0), nearly at right angles to the view) kept as it is; facing AWAY
		// (a two-sided card seen from behind) turned toward the viewer, not refused
		Want gn2[kRegions] = {};
		gn2[kRegGrassA] = { true, { 0.5f, 0.0f, 0.5f } }; gn2[kRegGrassB] = { true, { 1.0f, 0.5f, 0.5f } }; gn2[kRegGrassC] = { true, { 0.5f, 0.0f, 0.5f } }; gn2[kRegGrassD] = { true, { 0.5f, 0.0f, 0.5f } };
		if (!run(gOn, aOn, 2, 3.0f, o)) return false;
		judge("GRASS with its own records, normal: facing (0, -1, 0); edge-on (1, 0, 0) kept; from behind (0, 1, 0) turned to (0, -1, 0)", gOn, o, gn2, 0.01f);
		// -- its material: matte (the record's roughness 0.9), no metal, on every blade
		Want gm[kRegions] = {};
		for (int r = kRegGrassA; r <= kRegGrassD; r++) gm[r] = { true, { 0.9f, 0, 0 } };
		gm[kRegMeadow] = { true, { 0.95f, 0, 0 } };
		if (!run(gOn, aOn, 4, 3.0f, o)) return false;
		judge("GRASS with its own records, roughness / metal: 0.9, no metal; the meadow the land's 0.95", gOn, o, gm, 0.012f);
		keyNarrow = 0.0f;
	}
	releaseWorld(h, ids);
	return ok;
}
