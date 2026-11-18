// UNOFFICIAL 2026-11-12 farhandover: the path tracer's far hand-over (src/core/RtFarHandOver.h: PTGameFog, PTFarMatch, PTFarBlend), the
// game's own ps_3_0 sources drawn on the fork's D3D9 device as RtPathTrace.cpp FarDraw draws them (the same blend states), against the
// header's CPU functions. A floor seen from a rise, out to 40000 units; the loaded square is 10000 units wide, off-centre round the
// camera, so the square's edge crosses the floor in the picture: traced pixels inside it, a block of them marked "not traced" (the
// game's own pixel), a block the first-person model's kept pixels; the rest of the floor past the edge is traced too (nothing is ever
// traced there in the game: here it checks that such a pixel becomes the game's own). Checked per pixel:
//   A  the level gain then the fog + band blend: scene = T x gain(ramp) x (1 - f)(1 - w) + fog f (1 - w) + frame w; others untouched
//   B  the shadow buffer under PTOwnShadows: 1 inside the band's inner edge, the game's value by the share w in the band, untouched outside
//   C  the measurement: the map pass + RtFarHandOver::Ratio give back the ratio the game frame was built with; LoadedSquare's cases
static bool farFillTarget(Host& h, Tex& t, const std::vector<float>& v) {   // an RGBA16F target's content through its system-memory twin
	D3DLOCKED_RECT lr;
	CHECKHR(t.sys->LockRect(&lr, nullptr, 0), "lock");
	for (UINT j = 0; j < t.h; j++) { uint16_t* row = (uint16_t*)((char*)lr.pBits + j * lr.Pitch); for (UINT i = 0; i < t.w * 4; i++) row[i] = toHalf(v[size_t(j) * t.w * 4 + i]); }
	t.sys->UnlockRect();
	CHECKHR(h.dev->UpdateSurface(t.sys, nullptr, t.s, nullptr), "UpdateSurface");
	return true;
}
static void farDraw(Host& h, Tex& target, IDirect3DPixelShader9* ps, IDirect3DTexture9* const tex[4], const float k[8][4], int mode) {   // RtPathTrace.cpp FarDraw
	h.dev->SetVertexShader(nullptr); h.dev->SetPixelShader(ps);
	h.dev->SetPixelShaderConstantF(0, &k[0][0], 8);
	for (DWORD st = 0; st < 4; st++) {
		h.dev->SetTexture(st, tex[st] ? tex[st] : tex[0]);
		h.dev->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT); h.dev->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		h.dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		h.dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); h.dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	h.dev->SetRenderState(D3DRS_ZENABLE, FALSE); h.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); h.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	h.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); h.dev->SetRenderState(D3DRS_STENCILENABLE, FALSE); h.dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, mode != 0 ? TRUE : FALSE);
	h.dev->SetRenderState(D3DRS_COLORWRITEENABLE, mode != 0 ? 0x7u : 0xFu);
	h.dev->SetRenderState(D3DRS_SRCBLEND, mode == 1 ? D3DBLEND_SRCALPHA : mode == 2 ? D3DBLEND_DESTCOLOR : D3DBLEND_ONE);
	h.dev->SetRenderState(D3DRS_DESTBLEND, mode == 1 ? D3DBLEND_INVSRCALPHA : mode == 2 ? D3DBLEND_SRCCOLOR : D3DBLEND_ZERO);
	h.dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD); h.dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	IDirect3DSurface9* back = nullptr; h.dev->GetRenderTarget(0, &back);
	h.dev->SetRenderTarget(0, target.s);
	D3DVIEWPORT9 vp = { 0, 0, target.w, target.h, 0.0f, 1.0f }; h.dev->SetViewport(&vp);
	h.dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const float x1 = float(target.w) - 0.5f, y1 = float(target.h) - 0.5f;
	const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
	h.dev->BeginScene();
	h.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	h.dev->EndScene();
	h.dev->SetRenderTarget(0, back); if (back) back->Release();
	for (DWORD st = 0; st < 4; st++) h.dev->SetTexture(st, nullptr);
	h.dev->SetPixelShader(nullptr);
	h.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); h.dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
}
static bool sceneFarHandOver(Ctx& c) {
	Host& h = *c.host;
	bool ok = true;
	const V3 eye = { 300, -200, 1200 };
	World w;
	w.meshes.resize(1);
	w.meshes[0].quad({ -40000, -40000, 0 }, { 40000, -40000, 0 }, { 40000, 40000, 0 }, { -40000, 40000, 0 });
	Object floor; floor.mesh = 0; floor.land = true;
	w.objects = { floor };
	Camera cam = Camera::lookDir(eye, { 0.25f, 1, -0.2f }, { 0, 0, 1 }, 1.2f, 1.2f);
	cam.farZ = 60000.0f;
	std::vector<GPixel> px; std::vector<float> depth, normals;
	gbuffer(w, cam, px, depth, normals);

	RtFarHandOver::Frame f;
	f.forward[0] = cam.f.x; f.forward[1] = cam.f.y;
	f.right[0] = cam.r.x / cam.p00; f.right[1] = cam.r.y / cam.p00;
	f.up[0] = cam.u.x / cam.p11; f.up[1] = cam.u.y / cam.p11;
	f.farZ = cam.farZ;
	f.keepViewModel = 1.0f;
	const float half = 5000.0f;
	f.square[0] = -half - eye.x; f.square[1] = -half - eye.y; f.square[2] = half - eye.x; f.square[3] = half - eye.y;
	f.band = 1500.0f; f.frameScale = 1.1f;
	f.fogNear = -10.0f; f.fogFar = 40000.0f; f.fogPower = 0.8f; f.fogStrength = 1.0f;
	f.fogColour[0] = 0.60f; f.fogColour[1] = 0.65f; f.fogColour[2] = 0.70f;
	f.gain[0] = 1.30f; f.gain[1] = 1.15f; f.gain[2] = 0.80f;
	f.reach = 3000.0f;
	float k[8][4] = {};
	RtFarHandOver::Pack(f, k);

	// the inputs: the first surface (a 1 traced; a block "not traced"), the view model's depth (1 = none; a block of it kept), the
	// traced picture T and the game's frame G (C: built so that (G x scale / T with fog)^2.2 is `truth` on every pixel)
	const float truth[3] = { 1.5f, 1.2f, 0.9f };
	std::vector<float> first(S * S * 4, 0.0f), vm(S * S, 1.0f), traced(S * S * 4, 0.0f), game(S * S * 4, 0.0f), shadow(S * S * 4, 0.3f);
	std::vector<int> kind(S * S, 0);   // 0 sky / no depth, 1 traced, 2 not traced (a < 0.5), 3 the view model's kept pixel
	std::vector<float> edge(S * S, 0.0f), fog(S * S, 0.0f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const UINT p = j * S + i;
		float* t = &traced[p * 4]; float* g = &game[p * 4];
		t[0] = 0.20f + 0.50f * float(i) / S; t[1] = 0.25f + 0.40f * float(j) / S; t[2] = 0.30f + 0.20f * float((i * 7 + j * 3) % 16) / 16.0f; t[3] = 0.37f;
		g[3] = 1.0f;
		for (int ch = 0; ch < 3; ch++) g[ch] = 0.5f;
		if (!px[p].valid) continue;
		float dist = 0.0f;
		edge[p] = RtFarHandOver::Edge(f, (i + 0.5f) / S, (j + 0.5f) / S, depth[p], &dist);
		fog[p] = RtFarHandOver::Fog(f, dist);
		const bool lod = i >= 20 && i < 44 && j >= 80 && j < 104, model = i >= 90 && i < 120 && j >= 96 && j < 124;
		kind[p] = lod ? 2 : model ? 3 : 1;
		first[p * 4 + 3] = lod ? 0.0f : 1.0f;
		for (int ch = 0; ch < 3; ch++) first[p * 4 + ch] = 0.4f;
		if (model) vm[p] = 0.5f;
		for (int ch = 0; ch < 3; ch++) {
			const float fogged = t[ch] + (f.fogColour[ch] * f.frameScale - t[ch]) * fog[p];
			g[ch] = fogged * std::pow(truth[ch], 1.0f / 2.2f) / f.frameScale;
		}
	}
	Tex ta, td, tm, tg, scene, shadowT, mapT;
	if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, ta) || !makeTex(h, S, S, D3DFMT_R32F, false, td) || !makeTex(h, S, S, D3DFMT_R32F, false, tm) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, tg) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, scene) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, shadowT) || !makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, mapT)) return false;
	if (!fillTex(ta, first, 4) || !fillTex(td, depth, 1) || !fillTex(tm, vm, 1) || !fillTex(tg, game, 4) || !farFillTarget(h, scene, traced) || !farFillTarget(h, shadowT, shadow)) return false;
	IDirect3DPixelShader9* ps[4] = {};
	const char* const source[4] = { RtFarHandOver::kBlendPS, RtFarHandOver::kGainPS, RtFarHandOver::kShadowPS, RtFarHandOver::kMapPS };
	for (int s = 0; s < 4; s++) {
		ID3DBlob* code = nullptr;
		if (!compileHlsl(h, source[s], "ps_3_0", &code)) return false;
		CHECKHR(h.dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps[s]), "CreatePixelShader");
		code->Release();
	}
	IDirect3DTexture9* const tex[4] = { ta.t, td.t, tm.t, tg.t };

	// C first (the scene still holds T): the map, then the ratio over the band as RtPathTrace.cpp FarMeasure takes it
	{
		farDraw(h, mapT, ps[3], tex, k, 0);
		std::vector<float> map;
		if (!readTex(h, mapT, map, 4)) return false;
		std::vector<float> samples;
		int mapBad = 0, zone = 0;
		for (UINT p = 0; p < S * S; p++) {
			const bool want = kind[p] == 1;
			if ((map[p * 4] > 0.5f) != want) mapBad++;
			if (want && (std::fabs(map[p * 4 + 1] - fog[p]) > 2e-3f || std::fabs(map[p * 4 + 2] * 65536.0f - edge[p]) > std::max(2.0f, 2e-3f * std::fabs(edge[p])))) mapBad++;
			if (!want || edge[p] < 0.0f || edge[p] > f.band) continue;
			zone++;
			const float s[7] = { traced[p * 4], traced[p * 4 + 1], traced[p * 4 + 2], game[p * 4], game[p * 4 + 1], game[p * 4 + 2], map[p * 4 + 1] };
			samples.insert(samples.end(), s, s + 7);
		}
		float ratio[3] = {}; int used = 0;
		const bool got = RtFarHandOver::Ratio(f, samples.data(), int(samples.size() / 7), 48, ratio, &used);
		float sq[4];
		const bool in1 = RtFarHandOver::LoadedSquare(-4, 28, 5, -15412.0f, 117802.0f, sq);   // og-desert-near's cell: cells -6..-2 x 26..30
		const bool sqOk = in1 && std::fabs(sq[0] - (-6 * 4096.0f + 15412.0f)) < 0.5f && std::fabs(sq[2] - (-1 * 4096.0f + 15412.0f)) < 0.5f &&
			std::fabs(sq[1] - (26 * 4096.0f - 117802.0f)) < 0.5f && std::fabs(sq[3] - (31 * 4096.0f - 117802.0f)) < 0.5f;
		const bool in2 = RtFarHandOver::LoadedSquare(10, 10, 7, 0.0f, 0.0f, sq);   // a grid elsewhere: a square of its size round the camera
		const bool sq2Ok = !in2 && sq[0] == -14336.0f && sq[3] == 14336.0f;
		const bool pass = mapBad == 0 && zone > 200 && got && std::fabs(ratio[0] - truth[0]) < 0.02f && std::fabs(ratio[1] - truth[1]) < 0.02f && std::fabs(ratio[2] - truth[2]) < 0.02f && sqOk && sq2Ok;
		say("  C the measurement: map wrong on %d pixels; %d band pixels, %d used, ratio %.3f %.3f %.3f (built with %.1f %.1f %.1f); LoadedSquare %s / %s -> %s\n",
			mapBad, zone, used, ratio[0], ratio[1], ratio[2], truth[0], truth[1], truth[2], sqOk ? "ok" : "BAD", sq2Ok ? "ok" : "BAD", pass ? "ok" : "BAD");
		if (!pass) ok = false;
	}
	// A: the gain (stored curve), then the fog + band
	{
		farDraw(h, scene, ps[1], tex, k, 2);
		farDraw(h, scene, ps[0], tex, k, 1);
		std::vector<float> got;
		if (!readTex(h, scene, got, 4)) return false;
		int n[4] = {}, bad[4] = {}, shown = 0, inBand = 0, pastEdge = 0, plain = 0;
		float worst = 0.0f;
		for (UINT p = 0; p < S * S; p++) {
			const float* t = &traced[p * 4]; const float* g = &got[p * 4];
			float want[3] = { t[0], t[1], t[2] };
			if (kind[p] == 1) {
				const float wB = RtFarHandOver::BandWeight(f, edge[p]), r = RtFarHandOver::Ramp(f, edge[p]), fg = fog[p];
				if (wB > 0.0f && wB < 1.0f) inBand++; else if (wB >= 1.0f) pastEdge++; else plain++;
				for (int ch = 0; ch < 3; ch++) {
					const float lifted = t[ch] * (1.0f + (f.gain[ch] - 1.0f) * r);
					want[ch] = lifted * (1.0f - fg) * (1.0f - wB) + f.fogColour[ch] * f.frameScale * fg * (1.0f - wB) + game[p * 4 + ch] * f.frameScale * wB;
				}
			}
			bool good = std::fabs(g[3] - t[3]) < 1e-3f;   // (colour only: the scene's alpha carries the materials)
			for (int ch = 0; ch < 3; ch++) { const float e = std::fabs(g[ch] - want[ch]); worst = std::max(worst, e); if (e > 4e-3f + 0.006f * want[ch]) good = false; }
			n[kind[p]]++;
			if (!good) { bad[kind[p]]++; if (shown++ < 4) say("    A pixel (%u, %u) kind %d: got %.4f %.4f %.4f a %.3f, want %.4f %.4f %.4f\n", p % S, p / S, kind[p], g[0], g[1], g[2], g[3], want[0], want[1], want[2]); }
		}
		const bool pass = bad[0] + bad[1] + bad[2] + bad[3] == 0 && inBand > 200 && pastEdge > 200 && plain > 1000 && n[2] > 300 && n[3] > 300;
		say("  A gain + fog + band (ps_3_0, RtFarHandOver.h): traced %d (%d wrong; %d inside, %d in the band, %d past the edge = the game's own), not traced %d (%d), the kept model %d (%d), no surface %d (%d); largest error %.4f -> %s\n",
			n[1], bad[1], plain, inBand, pastEdge, n[2], bad[2], n[3], bad[3], n[0], bad[0], worst, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	}
	// B: the shadow buffer (0.3 everywhere before): the solid part, then the band's part
	{
		float ks[8][4];
		std::memcpy(ks, k, sizeof(ks));
		ks[7][0] = 0.0f; farDraw(h, shadowT, ps[2], tex, ks, 0);
		ks[7][0] = 1.0f; farDraw(h, shadowT, ps[2], tex, ks, 1);
		std::vector<float> got;
		if (!readTex(h, shadowT, got, 4)) return false;
		int bad = 0, lit = 0, mixed = 0, kept = 0, shown = 0;
		for (UINT p = 0; p < S * S; p++) {
			float want = 0.3f;
			if (kind[p] == 1) {
				const float wB = RtFarHandOver::BandWeight(f, edge[p]);
				want = 1.0f - wB + 0.3f * wB;
				if (wB <= 0.0f) lit++; else if (wB < 1.0f) mixed++; else kept++;
			}
			if (std::fabs(got[p * 4] - want) > 3e-3f || std::fabs(got[p * 4 + 1] - want) > 3e-3f) { bad++; if (shown++ < 4) say("    B pixel (%u, %u) kind %d: got %.4f, want %.4f\n", p % S, p / S, kind[p], got[p * 4], want); }
		}
		const bool pass = bad == 0 && lit > 1000 && mixed > 200 && kept > 200;
		say("  B the shadow buffer under the band: %d wrong; lit %d, the game's shadow by the band's share %d, the game's own %d -> %s\n", bad, lit, mixed, kept, pass ? "ok" : "BAD");
		if (!pass) ok = false;
	}
	// D: the render rect (a RenderScale below 1): the picture is the buffers' top-left quarter, c3.zw = 2 2. The blend touches only the
	// quarter and places each of its pixels by the quarter's own uv; the map on a half-size target (c7.zw = 0.5 0.5) reads the same texels
	{
		RtFarHandOver::Frame f2 = f;
		f2.rectScale[0] = f2.rectScale[1] = 2.0f;
		f2.keepViewModel = 0.0f;
		float k2[8][4] = {};
		RtFarHandOver::Pack(f2, k2);
		Tex scene2, map2;
		if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, true, scene2) || !makeTex(h, S / 2, S / 2, D3DFMT_A16B16G16R16F, true, map2) || !farFillTarget(h, scene2, traced)) return false;
		farDraw(h, scene2, ps[0], tex, k2, 1);
		float km[8][4];
		std::memcpy(km, k2, sizeof(km));
		km[7][2] = km[7][3] = 0.5f;
		farDraw(h, map2, ps[3], tex, km, 0);
		std::vector<float> got, map;
		if (!readTex(h, scene2, got, 4) || !readTex(h, map2, map, 4)) return false;
		int bad = 0, in = 0, outside = 0, mapBad = 0, shown = 0;
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const UINT p = j * S + i;
			const bool quarter = i < S / 2 && j < S / 2;
			const bool surface = px[p].valid && first[p * 4 + 3] >= 0.5f;
			float want[3] = { traced[p * 4], traced[p * 4 + 1], traced[p * 4 + 2] };
			float e2 = 0.0f, fg = 0.0f;
			if (quarter && surface) {
				float dist = 0.0f;
				e2 = RtFarHandOver::Edge(f2, (i + 0.5f) / S, (j + 0.5f) / S, depth[p], &dist);
				fg = RtFarHandOver::Fog(f2, dist);
				const float wB = RtFarHandOver::BandWeight(f2, e2);
				for (int ch = 0; ch < 3; ch++) want[ch] = want[ch] * (1.0f - fg) * (1.0f - wB) + f2.fogColour[ch] * f2.frameScale * fg * (1.0f - wB) + game[p * 4 + ch] * f2.frameScale * wB;
				in++;
			}
			else if (!quarter) outside++;
			bool good = true;
			for (int ch = 0; ch < 3; ch++) if (std::fabs(got[p * 4 + ch] - want[ch]) > 4e-3f + 0.006f * want[ch]) good = false;
			if (!good) { bad++; if (shown++ < 4) say("    D pixel (%u, %u): got %.4f %.4f %.4f, want %.4f %.4f %.4f\n", i, j, got[p * 4], got[p * 4 + 1], got[p * 4 + 2], want[0], want[1], want[2]); }
			if (quarter) {
				const float* m = &map[(j * (S / 2) + i) * 4];
				if ((m[0] > 0.5f) != surface || (surface && (std::fabs(m[1] - fg) > 2e-3f || std::fabs(m[2] * 65536.0f - e2) > std::max(2.0f, 2e-3f * std::fabs(e2))))) mapBad++;
			}
		}
		const bool pass = bad == 0 && mapBad == 0 && in > 300 && outside == int(S * S * 3 / 4);
		say("  D the render rect (the picture in the top-left quarter): %d pixels wrong; %d surface pixels in the quarter by its own uv, %d outside untouched; the half-size map wrong on %d -> %s\n",
			bad, in, outside, mapBad, pass ? "ok" : "BAD");
		if (!pass) ok = false;
		for (Tex* t : { &scene2, &map2 }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	}
	for (IDirect3DPixelShader9* s : ps) if (s) s->Release();
	for (Tex* t : { &ta, &td, &tm, &tg, &scene, &shadowT, &mapT }) { if (t->sys) t->sys->Release(); t->s->Release(); t->t->Release(); }
	return ok;
}
