// (2026-11-15 rt-denoise-inputs, docs/next/RT-DENOISE-INPUTS.md) The denoiser inputs NVIDIA's NRD / DLSS-RR documentation recommends:
//  `ptconfidence`  RtDenoise's RGB chain (the path tracer's / RT GI's) with Settings.Confidence (RtDenoiseConfidence.frag + the temporal
//                  pass' -conf variant) vs without: a lamp's pool switched off, switched on, and sliding; one noisy path a pixel. The ghost
//                  after the change, the still noise and frame-to-frame flicker, the false alarms on a still picture, the GPU cost at 720p.
//  `rrmirror`      DlssRRGuides-mirror (RRMirrorMotion): the motion written at a flat marked mirror vs the CPU's mirror image of the reflected
//                  point (exact reflection, not the shader's formula); a curved marked mirror, unmarked floor and the sky untouched; the other
//                  five guides bit for bit the plain pass'.
//  `rrsky`         FSRPrep.comp's motion on sky pixels (depth 1, the far plane) vs the exact motion of a point at infinity (rotation only),
//                  the camera turning and moving: is the sky's motion for DLSS RR already right (the dlssrr sample's "sky trails" pitfall)?

namespace dninputs {
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
	bool computePipe(Host& h, const char* name, const NVR_EXT_BINDING_DESC* b, UINT count, ID3D9NvrExtPipeline*& out) {
		std::vector<char> spirv;
		if (!loadSpv(gM.compute + "\\" + name + ".spv", spirv)) return false;
		NVR_EXT_COMPUTE_PIPELINE_DESC d = {};
		d.pSpirv = spirv.data(); d.SpirvSize = UINT(spirv.size()); d.BindingCount = count; d.pBindings = b; d.PushConstantSize = 4;
		d.UsesSamplerHeap = TRUE; d.SamplerHeapSet = 0; d.SamplerHeapBinding = 0; d.pDebugName = name;
		CHECKHR(h.ext->CreateComputePipeline(&d, &out), "CreateComputePipeline");
		return true;
	}
	bool uniform(Host& h, const void* data, UINT size, ID3D9NvrExtBuffer*& out) {
		NVR_EXT_BUFFER_DESC bd = { size, NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "dninputs constants" };
		CHECKHR(h.ext->CreateBuffer(&bd, &out), "CreateBuffer");
		CHECKHR(h.ext->UpdateBuffer(out, 0, size, data), "UpdateBuffer");
		return true;
	}
	bool dispatch(Host& h, ID3D9NvrExtPipeline* p, NVR_EXT_RESOURCE_BINDING* b, UINT count, UINT w, UINT hh) {
		uint32_t push = 0;
		NVR_EXT_DISPATCH_DESC d = {};
		d.pPipeline = p; d.BindingCount = count; d.pBindings = b; d.pPushData = &push; d.PushDataSize = 4;
		d.GroupsX = (w + 7) / 8; d.GroupsY = (hh + 7) / 8; d.GroupsZ = 1;
		CHECKHR(h.ext->Dispatch(&d), "Dispatch");
		return true;
	}
	// a D3DX row-vector view matrix whose columns are the camera's axes (as TheRenderManager->viewMatrix, rotation only)
	void viewMatrix(float* m, V3 r, V3 u, V3 f) {
		std::memset(m, 0, 64);
		m[0] = r.x; m[1] = u.x; m[2] = f.x; m[4] = r.y; m[5] = u.y; m[6] = f.y; m[8] = r.z; m[9] = u.z; m[10] = f.z; m[15] = 1.0f;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------
// `ptconfidence`: a still camera over a flat floor (one surface: the reprojection is exact, so what is measured is the history's response
// to a change of LIGHT, which no motion vector can tell). One path a pixel modelled as lit with chance 0.25 or 0.8 (value / chance, else 0), the
// chain as the path tracer runs it (RGB temporal with AntiLag 3, MeanVariance 2, 2 a-trous passes with HistoryFix 3, sigma 1). The light:
// 0.1 everywhere + a lamp's pool (0.9 at its centre, gaussian, 14 px). 32 still frames, then at frame 32:
//   off:  the lamp goes out;   on: it comes on;   move: it slides 1.5 px a frame (a carried lamp, a turning object's light).
// Measured against the clean light: the pool's mean |error| over frames 33-40 (off / on), all pixels over frames 40-63 (move); before the
// change (frames 24-31) the RMSE and the frame-to-frame flicker (mean |out(f) - out(f-1)|), and the confidence tiles under 0.5 (false alarms).
static bool scenePtConfidence(Ctx& c) {
	Host& h = *c.host;
	Tex noisy, depth, normals;
	const D3DFORMAT F = D3DFMT_A32B32G32R32F;
	if (!makeTex(h, S, S, F, false, noisy) || !makeTex(h, S, S, D3DFMT_R32F, false, depth) || !makeTex(h, S, S, F, false, normals)) return false;
	const Camera cam = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.5f, 1.5f);
	{
		std::vector<float> d(size_t(S) * S, 3000.0f / cam.farZ), n(size_t(S) * S * 4, 0.0f);
		for (UINT i = 0; i < S * S; i++) { n[i * 4] = 0.5f; n[i * 4 + 1] = 0.5f; n[i * 4 + 2] = 0.0f; n[i * 4 + 3] = 1.0f; }
		if (!fillTex(depth, d, 1) || !fillTex(normals, n, 4)) return false;
	}
	auto pool = [](float x, float y, float cx, float cy) { const float r2 = (x - cx) * (x - cx) + (y - cy) * (y - cy); return 0.9f * std::exp(-r2 / (2.0f * 14.0f * 14.0f)); };
	auto clean = [&](int kind, int f, UINT x, UINT y) {
		const float px = x + 0.5f, py = y + 0.5f;
		float L = 0.1f;
		if (kind == 0) { if (f < 32) L += pool(px, py, 64, 64); }
		else if (kind == 1) { if (f >= 32) L += pool(px, py, 64, 64); }
		else L += pool(px, py, 40.0f + (f >= 32 ? 1.5f * (f - 32) : 0.0f), 64);
		return L;
	};
	struct Result { double ghost = 0, ghost1 = 0, ghost4 = 0, still = 0, flicker = 0, falseTiles = 0; };
	auto run = [&](int kind, float k, float lit, Result& res) -> bool {
		DnChain ch;
		if (!ch.make(h, S, S)) return false;
		if (k > 0.0f && !ch.makeConf(h, S, S)) return false;
		ch.confK = k; ch.meanVariance = 2.0f;
		uint32_t rng = 1234567u;
		auto rnd = [&]() { rng = rng * 747796405u + 2891336453u; uint32_t x = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u; return float(((x >> 22u) ^ x) >> 8) / 16777216.0f; };
		std::vector<float> prev, out, sig(size_t(S) * S * 4);
		const int frames = kind == 2 ? 64 : 41;
		for (int f = 0; f < frames; f++) {
			for (UINT y = 0; y < S; y++) for (UINT x = 0; x < S; x++) {
				const size_t p = size_t(y) * S + x;
				const float v = rnd() < lit ? clean(kind, f, x, y) / lit : 0.0f;
				sig[p * 4] = sig[p * 4 + 1] = sig[p * 4 + 2] = v; sig[p * 4 + 3] = 1.0f;
			}
			if (!fillTex(noisy, sig, 4)) return false;
			if (!ch.run(h, false, noisy.t, depth.t, normals.t, depth.t, depth.t, cam, S, S, f == 0, 2, 1.0f, true, 3.0f)) return false;
			if (!readTex(h, ch.out, out, 4)) return false;
			double e = 0, e2 = 0, fl = 0; int n = 0, m = 0;
			for (UINT y = 3; y < S - 3; y++) for (UINT x = 3; x < S - 3; x++) {
				const size_t p = size_t(y) * S + x;
				const float want = clean(kind, f, x, y), d = out[p * 4] - want;
				e2 += d * d; n++;
				if (!prev.empty()) fl += std::fabs(out[p * 4] - prev[p * 4]);
				const bool inPool = kind == 2 || std::sqrt((x + 0.5f - 64) * (x + 0.5f - 64) + (y + 0.5f - 64) * (y + 0.5f - 64)) < 28.0f;
				if (inPool) { e += std::fabs(d); m++; }
			}
			if (f >= 24 && f < 32) {
				res.still += std::sqrt(e2 / n) / 8.0; res.flicker += fl / n / 8.0;
				if (k > 0.0f) {
					std::vector<float> cv;
					if (!readTex(h, ch.conf, cv, 2)) return false;
					int low = 0; for (UINT i = 0; i < ch.confW * ch.confH; i++) if (cv[i * 2] < 0.5f) low++;
					res.falseTiles += double(low) / (ch.confW * ch.confH) / 8.0;
				}
			}
			if (kind != 2 && f >= 33 && f <= 40) res.ghost += e / m / 8.0;
			if (kind != 2 && f == 33) res.ghost1 = e / m;
			if (kind != 2 && f == 36) res.ghost4 = e / m;
			if (kind == 2 && f >= 40) res.ghost += e / m / 24.0;
			prev = out;
		}
		ch.free();
		return true;
	};
	// Two noise levels: one path a pixel lit with chance 0.25 (the path tracer's hard case: the fresh 25-sample mean's own spread is about
	// the size of the light, so k standard errors hide a change that small) and with chance 0.8 (a low-noise signal, as RT GI's denoised-
	// before-reuse light or a sunlit floor). The gate is what the method must do whatever the noise: a light that goes OUT clears faster at
	// 0.25 and the still picture is not noisier / does not flicker more at either level. "On" and "sliding" are printed as measured.
	const char* names[3] = { "lamp off", "lamp on", "lamp sliding 1.5 px/frame" };
	const float ks[3] = { 0.0f, 3.0f, 4.0f };
	const float lits[2] = { 0.25f, 0.8f };
	Result r[2][3][3];
	for (int L = 0; L < 2; L++) for (int kind = 0; kind < 3; kind++) for (int i = 0; i < 3; i++) if (!run(kind, ks[i], lits[L], r[L][kind][i])) return false;
	freeTex({ &noisy, &depth, &normals });
	bool pass = true;
	bool okK[3] = { true, true, true };
	for (int L = 0; L < 2; L++) {
		say("  one path a pixel lit with chance %.2f:\n", lits[L]);
		for (int kind = 0; kind < 3; kind++) {
			const Result* q = r[L][kind];
			say("    %s: %s mean |error| (frames %s): off %.4f, k 3 %.4f (x %.2f), k 4 %.4f (x %.2f)", names[kind], kind == 2 ? "all pixels'" : "the pool's",
				kind == 2 ? "40-63" : "33-40", q[0].ghost, q[1].ghost, q[1].ghost / q[0].ghost, q[2].ghost, q[2].ghost / q[0].ghost);
			if (kind != 2) say("; 1st frame after %.4f / %.4f / %.4f, 4th %.4f / %.4f / %.4f", q[0].ghost1, q[1].ghost1, q[2].ghost1, q[0].ghost4, q[1].ghost4, q[2].ghost4);
			say("\n");
		}
		const Result& o = r[L][0][0]; const Result& k3 = r[L][0][1]; const Result& k2 = r[L][0][2];
		say("    still frames 24-31: RMSE off %.4f, k 3 %.4f (x %.3f), k 4 %.4f (x %.3f); flicker off %.4f, k 3 %.4f (x %.3f), k 4 %.4f (x %.3f); "
			"confidence tiles < 0.5 (false alarms) k 3 %.2f %%, k 4 %.2f %%\n", o.still, k3.still, k3.still / o.still, k2.still, k2.still / o.still,
			o.flicker, k3.flicker, k3.flicker / o.flicker, k2.flicker, k2.flicker / o.flicker, 100.0 * k3.falseTiles, 100.0 * k2.falseTiles);
		for (int i = 1; i < 3; i++) if (!(r[L][0][i].still <= 1.05 * o.still && r[L][0][i].flicker <= 1.10 * o.flicker)) okK[i] = false;
		(void)k3; (void)k2;
	}
	for (int i = 1; i < 3; i++) if (!(r[0][0][i].ghost < 0.7 * r[0][0][0].ghost)) okK[i] = false;
	pass = okK[1] || okK[2];
	say("    k 3 meets every gate: %s; k 4: %s\n", okK[1] ? "yes" : "no", okK[2] ? "yes" : "no");
	say("    (k 3 or k 4: the lamp-off ghost < 0.7 x at chance 0.25; still RMSE <= 1.05 x and flicker <= 1.10 x at both levels) -> %s\n", pass ? "ok" : "BAD");

	// cost: 1280 x 720, the chain off vs k 3, interleaved, median of 40 frames each (the iGPU is shared: noisy)
	{
		const UINT W = 1280, H = 720;
		IDirect3DQuery9* freqQ = nullptr;
		if (FAILED(h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freqQ)) || !freqQ) { say("  no timestamp queries: cost not measured\n"); return pass; }
		Tex n2, d2, nn2;
		if (!makeTex(h, W, H, D3DFMT_A16B16G16R16F, false, n2) || !makeTex(h, W, H, D3DFMT_R32F, false, d2) || !makeTex(h, W, H, D3DFMT_A16B16G16R16F, false, nn2)) return false;
		{
			std::vector<float> a(size_t(W) * H * 4), b(size_t(W) * H), nv(size_t(W) * H * 4);
			uint32_t s = 7;
			for (size_t i = 0; i < size_t(W) * H; i++) {
				s = s * 1664525u + 1013904223u;
				const float v = float(s >> 8) / 16777216.0f;
				a[i * 4] = a[i * 4 + 1] = a[i * 4 + 2] = v; a[i * 4 + 3] = 1.0f;
				b[i] = 0.1f + 0.05f * float(i % W) / W;
				nv[i * 4] = 0.5f; nv[i * 4 + 1] = 0.5f; nv[i * 4 + 2] = 0.0f; nv[i * 4 + 3] = 1.0f;
			}
			if (!fillTex(n2, a, 4) || !fillTex(d2, b, 1) || !fillTex(nn2, nv, 4)) return false;
		}
		const Camera cam2 = Camera::lookDir({ 0, 0, 3000 }, { 0, 0, -1 }, { 0, 1, 0 }, 1.0f, 1.78f);
		DnChain chains[3];   // 0 off, 1 on, 2 the confidence pass + the plain temporal pass (the pass' own cost)
		for (int k = 0; k < 3; k++) {
			if (!chains[k].make(h, W, H)) return false;
			chains[k].meanVariance = 2.0f;
			if (k && !chains[k].makeConf(h, W, H)) return false;
			chains[k].confK = k ? 3.0f : 0.0f;
			chains[k].confPassOnly = k == 2;
		}
		std::vector<double> times[3];
		for (int f = 0; f < 44; f++) for (int k = 0; k < 3; k++) {
			IDirect3DQuery9 *q0 = nullptr, *q1 = nullptr, *dis = nullptr;
			h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &dis); h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q0); h.dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q1);
			if (!dis || !q0 || !q1) { say("  timestamp queries failed: cost not measured\n"); return pass; }
			dis->Issue(D3DISSUE_BEGIN); q0->Issue(D3DISSUE_END);
			if (!chains[k].run(h, false, n2.t, d2.t, nn2.t, d2.t, d2.t, cam2, W, H, f == 0, 2, 1.0f, false, 3.0f)) return false;
			q1->Issue(D3DISSUE_END); dis->Issue(D3DISSUE_END); freqQ->Issue(D3DISSUE_END);
			chains[k].keep(h, false);
			UINT64 t0 = 0, t1 = 0, freq = 0; BOOL disjoint = TRUE;
			for (int tries = 0; tries < 2000 && q1->GetData(&t1, sizeof(t1), D3DGETDATA_FLUSH) == S_FALSE; tries++) Sleep(1);
			q0->GetData(&t0, sizeof(t0), D3DGETDATA_FLUSH); dis->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH);
			for (int tries = 0; tries < 2000 && freqQ->GetData(&freq, sizeof(freq), D3DGETDATA_FLUSH) == S_FALSE; tries++) Sleep(1);
			if (f >= 4 && freq && t1 > t0 && !disjoint) times[k].push_back(double(t1 - t0) * 1000.0 / double(freq));
			q0->Release(); q1->Release(); dis->Release();
		}
		double ms[3] = {};
		for (int k = 0; k < 3; k++) {
			chains[k].free();
			std::sort(times[k].begin(), times[k].end());
			ms[k] = times[k].empty() ? -1.0 : times[k][times[k].size() / 2];
		}
		freqQ->Release();
		freeTex({ &n2, &d2, &nn2 });
		say("  cost at 1280 x 720 (this GPU, median of 40 interleaved frames): the RGB chain %.3f ms, with the confidence (its 256 x 144 pass + the -conf "
			"temporal) %.3f ms: + %.3f ms, of which the 256 x 144 pass %.3f ms\n", ms[0], ms[1], ms[1] - ms[0], ms[2] - ms[0]);
	}
	return pass;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// `rrmirror`: DlssRRGuides-mirror (RRMirrorMotion). A camera 100 units over a floor looking level (right +x, up +z, forward +y), a wall
// 800 units ahead; the floor's left 60 % a flat mirror the path tracer marked (its specular image's alpha = -hit distance to the wall,
// the CPU's reflected ray), the right part a CURVED marked mirror (normals bent per column), the upper half the sky. Last frame's camera
// 1.5 degrees turned and (15, 8, 0) behind. Expected motion at the flat mirror: the reflected wall point's MIRROR IMAGE (behind the floor)
// projected with last frame's camera (computed from the reflection, not with the shader's lengthened view ray). The motion image starts
// as a sentinel: everything but the flat mirror must keep it.
static bool sceneRrMirror(Ctx& c) {
	using namespace dninputs;
	Host& h = *c.host;
	const UINT SW = 160, SH = 120;
	const float vx = 1.0f, vy = 0.75f, H = 100.0f, Y = 800.0f, farZ = 353840.0f;
	const float th = 1.5f * 0.0174532925f;
	const V3 r(1, 0, 0), u(0, 0, 1), f(0, 1, 0);
	const V3 pr(std::cos(th), std::sin(th), 0), pu(0, 0, 1), pf(-std::sin(th), std::cos(th), 0);
	const V3 delta(15.0f, 8.0f, 0.0f);
	if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	auto IMG = [](UINT b) { NVR_EXT_BINDING_DESC d = { 1, b, NVR_EXT_BIND_SAMPLED_IMAGE, b, NVR_EXT_VIEW_2D, FALSE, 0 }; return d; };
	auto UAV = [](UINT b) { NVR_EXT_BINDING_DESC d = { 1, b, NVR_EXT_BIND_STORAGE_IMAGE, b, NVR_EXT_VIEW_2D, TRUE, 0 }; return d; };
	const NVR_EXT_BINDING_DESC CB = { 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }, PT = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
	const NVR_EXT_BINDING_DESC plainB[15] = { IMG(0), IMG(1), IMG(2), IMG(3), UAV(4), UAV(5), UAV(6), UAV(7), UAV(8), CB, PT, IMG(9), IMG(10), IMG(11), IMG(12) };
	const NVR_EXT_BINDING_DESC mirrorB[17] = { IMG(0), IMG(1), IMG(2), IMG(3), UAV(4), UAV(5), UAV(6), UAV(7), UAV(8), CB, PT, IMG(9), IMG(10), IMG(11), IMG(12), UAV(13), IMG(14) };
	ID3D9NvrExtPipeline *plain = nullptr, *mirror = nullptr;
	if (!computePipe(h, "DlssRRGuides", plainB, 15, plain) || !computePipe(h, "DlssRRGuides-mirror", mirrorB, 17, mirror)) return false;
	// the scene
	std::vector<float> color(SW * SH * 4), normals(SW * SH * 4), depth(SW * SH), spec(SW * SH * 4, 0.0f), want(SW * SH * 2, 0.0f), surf(SW * SH * 2, 0.0f);
	std::vector<int> kind(SW * SH, 0);   // 0 sky, 1 flat mirror, 2 curved mirror, 3 floor beyond the wall (unmarked)
	auto project = [&](V3 q, float uvx, float uvy, float* m) {   // q relative to last frame's camera
		const float z = dot(q, pf);
		const float nx = dot(q, pr) / vx / z, ny = dot(q, pu) / vy / z;
		m[0] = (nx * 0.5f + 0.5f) - uvx; m[1] = (0.5f - ny * 0.5f) - uvy;
	};
	for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
		const size_t i = size_t(y) * SW + x;
		const float uvx = (x + 0.5f) / SW, uvy = (y + 0.5f) / SH;
		color[i * 4] = color[i * 4 + 1] = color[i * 4 + 2] = 0.5f; color[i * 4 + 3] = 1.0f;
		const V3 ray = f + r * ((2 * uvx - 1) * vx) + u * ((1 - 2 * uvy) * vy);
		normals[i * 4] = 0.5f; normals[i * 4 + 1] = 1.0f; normals[i * 4 + 2] = 0.5f; normals[i * 4 + 3] = 1.0f;   // view-space up
		if (ray.z > -0.02f) { depth[i] = 1.0f; continue; }   // the sky
		const float t = -H / ray.z;   // view depth (the ray's forward part is 1)
		depth[i] = t / farZ;
		if (t >= 0.95f * Y) { kind[i] = 3; spec[i * 4 + 3] = 50.0f; continue; }   // past the wall's foot: an unmarked floor
		const V3 P = ray * t, D = normalize(ray), R(D.x, D.y, -D.z);
		const float s = (Y - P.y) / R.y;
		const V3 W = P + R * s, image(W.x, W.y, -2.0f * H - W.z);
		const bool flat = x < SW * 6 / 10;
		kind[i] = flat ? 1 : 2;
		spec[i * 4 + 3] = -s;
		if (!flat) { const float b = 0.2f * std::sin(0.9f * x); normals[i * 4] = 0.5f + 0.5f * b; normals[i * 4 + 1] = 0.5f + 0.5f * std::sqrt(1.0f - b * b); }
		project(image + delta, uvx, uvy, &want[i * 2]);
		project(P + delta, uvx, uvy, &surf[i * 2]);
	}
	Tex tColor, tNormals, tDepth, tSpec, oColor[2], oDiffuse[2], oSpecular[2], oNormals[2], oRough[2], oMotion;
	auto mk = [&](Tex& t, D3DFORMAT fm, bool target) { return makeTex(h, SW, SH, fm, target, t); };
	if (!mk(tColor, D3DFMT_A16B16G16R16F, false) || !mk(tNormals, D3DFMT_A16B16G16R16F, false) || !mk(tDepth, D3DFMT_R32F, false) || !mk(tSpec, D3DFMT_A16B16G16R16F, false) ||
		!mk(oMotion, D3DFMT_G16R16F, true)) return false;
	for (int k = 0; k < 2; k++)
		if (!mk(oColor[k], D3DFMT_A16B16G16R16F, true) || !mk(oDiffuse[k], D3DFMT_A16B16G16R16F, true) || !mk(oSpecular[k], D3DFMT_A16B16G16R16F, true) ||
			!mk(oNormals[k], D3DFMT_A16B16G16R16F, true) || !mk(oRough[k], D3DFMT_R16F, true)) return false;
	if (!fillTex(tColor, color, 4) || !fillTex(tNormals, normals, 4) || !fillTex(tDepth, depth, 1) || !fillTex(tSpec, spec, 4)) return false;
	CHECKHR(h.dev->ColorFill(oMotion.s, nullptr, D3DCOLOR_ARGB(255, 64, 192, 0)), "ColorFill");
	std::vector<float> sentinel;
	if (!readTex(h, oMotion, sentinel, 2)) return false;
	struct { float right[4], up[4], forward[4], view[4], options[4], size[4], prevRight[4], prevUp[4], prevForward[4], prevProjection[4], cameraDelta[4]; } cb = {
		{ 1, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, 1, 0, 0 }, { vx, vy, 2.2f, 0.6f }, { 0, 0, 0, 0 }, { (float)SW, (float)SH, 0, 0 },
		{ pr.x, pr.y, pr.z, 0 }, { pu.x, pu.y, pu.z, 0 }, { pf.x, pf.y, pf.z, 0 }, { 1.0f / vx, 1.0f / vy, farZ, 0 }, { delta.x, delta.y, delta.z, 1.0f } };
	ID3D9NvrExtBuffer* buffer = nullptr;
	if (!uniform(h, &cb, sizeof(cb), buffer)) return false;
	for (int k = 0; k < 2; k++) {
		NVR_EXT_RESOURCE_BINDING b[17] = { image(0, tColor.t), image(1, tNormals.t), image(2, tDepth.t), image(3, tDepth.t),
			image(4, oColor[k].t, true), image(5, oDiffuse[k].t, true), image(6, oSpecular[k].t, true), image(7, oNormals[k].t, true), image(8, oRough[k].t, true) };
		b[9].Slot = 0; b[9].Type = NVR_EXT_BIND_UNIFORM_BUFFER; b[9].pResource = buffer;
		b[10].Slot = 0; b[10].Type = NVR_EXT_BIND_SAMPLER; b[10].pResource = gPointClamp;
		b[11] = image(9, tDepth.t); b[12] = image(10, tDepth.t); b[13] = image(11, tDepth.t); b[14] = image(12, tDepth.t);
		b[15] = image(13, oMotion.t, true); b[16] = image(14, tSpec.t);
		if (!dispatch(h, k ? mirror : plain, b, k ? 17 : 15, SW, SH)) return false;
	}
	std::vector<float> mo, g[2][5];
	if (!readTex(h, oMotion, mo, 2)) return false;
	for (int k = 0; k < 2; k++)
		if (!readTex(h, oColor[k], g[k][0], 4) || !readTex(h, oDiffuse[k], g[k][1], 4) || !readTex(h, oSpecular[k], g[k][2], 4) || !readTex(h, oNormals[k], g[k][3], 4) ||
			!readTex(h, oRough[k], g[k][4], 1)) return false;
	buffer->Release(); plain->Release(); mirror->Release();
	int guideDiff = 0;
	for (int j = 0; j < 5; j++) for (size_t i = 0; i < g[0][j].size(); i++) if (g[0][j][i] != g[1][j][i]) guideDiff++;
	double worst = 0, sumErr = 0, sumSurf = 0; int flat = 0, untouchedBad = 0, others = 0;
	for (UINT y = 1; y < SH - 1; y++) for (UINT x = 1; x < SW - 1; x++) {
		const size_t i = size_t(y) * SW + x;
		const bool edge = kind[i - 1] != kind[i] || kind[i + 1] != kind[i] || kind[i - SW] != kind[i] || kind[i + SW] != kind[i];
		if (kind[i] == 1 && !edge) {
			const double ex = (mo[i * 2] - want[i * 2]) * SW, ey = (mo[i * 2 + 1] - want[i * 2 + 1]) * SH;
			const double e = std::sqrt(ex * ex + ey * ey);
			worst = std::max(worst, e); sumErr += e; flat++;
			const double sx = (surf[i * 2] - want[i * 2]) * SW, sy = (surf[i * 2 + 1] - want[i * 2 + 1]) * SH;
			sumSurf += std::sqrt(sx * sx + sy * sy);
		}
		else if (kind[i] != 1) {
			others++;
			if (mo[i * 2] != sentinel[i * 2] || mo[i * 2 + 1] != sentinel[i * 2 + 1]) untouchedBad++;
		}
	}
	say("  flat marked mirror: %d pixels, the virtual point's motion vs the CPU's mirror image: mean %.4f px, worst %.4f px; the surface's own motion "
		"(what FSRPrep gives RR there today) is off by %.2f px on average\n", flat, flat ? sumErr / flat : 0.0, worst, flat ? sumSurf / flat : 0.0);
	say("  curved mirror / unmarked floor / sky: %d pixels, %d changed; the five other guides: %d values differ from the plain pass'\n", others, untouchedBad, guideDiff);
	const bool pass = flat > 1000 && worst < 0.05 && untouchedBad == 0 && guideDiff == 0 && sumSurf / flat > 1.0;
	say("    (worst < 0.05 px, the rest untouched, guides identical) -> %s\n", pass ? "ok" : "BAD");
	freeTex({ &tColor, &tNormals, &tDepth, &tSpec, &oMotion });
	for (int k = 0; k < 2; k++) freeTex({ &oColor[k], &oDiffuse[k], &oSpecular[k], &oNormals[k], &oRough[k] });
	return pass;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// `rrsky`: FSRPrep.comp (the motion RR and FSR 2 get) on an all-sky screen (TESR_DepthBuffer 1: the far plane, farZ 353840 as the game's)
// vs the exact motion of a direction at infinity (last frame's camera rotation only). Two cases: a turn (2 degrees yaw + 1 pitch) with a
// walk (10 units a frame), and the same turn with a 100-units-a-frame move. Errors in pixels of a 1920-wide screen.
static bool sceneRrSky(Ctx& c) {
	using namespace dninputs;
	Host& h = *c.host;
	const UINT SW = 320, SH = 180;
	const float p00 = 1.0f / 1.0f, p11 = 1.0f / 0.5625f, farZ = 353840.0f;
	if (!gPointClamp) { NVR_EXT_SAMPLER_DESC sd = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP }; CHECKHR(h.ext->CreateSampler(&sd, &gPointClamp), "CreateSampler"); }
	auto IMG = [](UINT b) { NVR_EXT_BINDING_DESC d = { 1, b, NVR_EXT_BIND_SAMPLED_IMAGE, b, NVR_EXT_VIEW_2D, FALSE, 0 }; return d; };
	auto UAV = [](UINT b) { NVR_EXT_BINDING_DESC d = { 1, b, NVR_EXT_BIND_STORAGE_IMAGE, b, NVR_EXT_VIEW_2D, TRUE, 0 }; return d; };
	const NVR_EXT_BINDING_DESC bd[8] = { IMG(0), IMG(1), UAV(2), UAV(3), { 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }, IMG(4), IMG(5) };   // TAAFSR.cpp PrepBindings
	ID3D9NvrExtPipeline* prep = nullptr;
	if (!computePipe(h, "FSRPrep", bd, 8, prep)) return false;
	Tex tDepth, tZero, oMotion, oDepth;
	if (!makeTex(h, SW, SH, D3DFMT_R32F, false, tDepth) || !makeTex(h, SW, SH, D3DFMT_R32F, false, tZero) || !makeTex(h, SW, SH, D3DFMT_G16R16F, true, oMotion) ||
		!makeTex(h, SW, SH, D3DFMT_G32R32F, true, oDepth)) return false;
	if (!fillTex(tDepth, std::vector<float>(SW * SH, 1.0f), 1) || !fillTex(tZero, std::vector<float>(SW * SH, 0.0f), 1)) return false;
	const V3 r(1, 0, 0), u(0, 0, 1), f(0, 1, 0);
	const float yaw = 2.0f * 0.0174532925f, pitch = 1.0f * 0.0174532925f;
	// last frame's camera: this one turned back by the yaw (about z) and the pitch (about its right axis)
	const V3 f1 = normalize(V3(-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)));
	const V3 r1 = normalize(cross(f1, V3(0, 0, 1))), u1 = cross(r1, f1);
	bool pass = true;
	const float moves[2] = { 10.0f, 100.0f };
	for (int k = 0; k < 2; k++) {
		struct { float projection[16], view[16], prevView[16], cameraData[4], prevProjection[4], cameraDelta[4], size[4], extra[4]; } cb = {};
		cb.projection[0] = p00; cb.projection[5] = p11; cb.projection[10] = 1.0f; cb.projection[15] = 1.0f;
		viewMatrix(cb.view, r, u, f); viewMatrix(cb.prevView, r1, u1, f1);
		cb.cameraData[0] = 5.0f; cb.cameraData[1] = farZ;
		cb.prevProjection[0] = p00; cb.prevProjection[1] = p11;
		const V3 delta = normalize(V3(0.6f, 0.8f, 0.0f)) * moves[k];
		cb.cameraDelta[0] = delta.x; cb.cameraDelta[1] = delta.y; cb.cameraDelta[2] = delta.z; cb.cameraDelta[3] = 0.0f;
		cb.size[0] = (float)SW; cb.size[1] = (float)SH;
		ID3D9NvrExtBuffer* buffer = nullptr;
		if (!uniform(h, &cb, sizeof(cb), buffer)) return false;
		NVR_EXT_RESOURCE_BINDING b[8] = { image(0, tDepth.t), image(1, tZero.t), image(2, oMotion.t, true), image(3, oDepth.t, true) };
		b[4].Slot = 0; b[4].Type = NVR_EXT_BIND_UNIFORM_BUFFER; b[4].pResource = buffer;
		b[5].Slot = 0; b[5].Type = NVR_EXT_BIND_SAMPLER; b[5].pResource = gPointClamp;
		b[6] = image(4, tDepth.t); b[7] = image(5, tDepth.t);
		if (!dispatch(h, prep, b, 8, SW, SH)) return false;
		std::vector<float> mo;
		if (!readTex(h, oMotion, mo, 2)) return false;
		buffer->Release();
		double worst = 0, mean = 0, size = 0;
		for (UINT y = 0; y < SH; y++) for (UINT x = 0; x < SW; x++) {
			const size_t i = size_t(y) * SW + x;
			const float uvx = (x + 0.5f) / SW, uvy = (y + 0.5f) / SH;
			const V3 dir = f + r * ((2 * uvx - 1) / p00) + u * ((1 - 2 * uvy) / p11);   // FSRPrep's toWorld
			const float z = dot(dir, f1);
			const float wx = (dot(dir, r1) * p00 / z) * 0.5f + 0.5f - uvx, wy = 0.5f - (dot(dir, u1) * p11 / z) * 0.5f - uvy;
			const double ex = (mo[i * 2] - wx) * 1920.0, ey = (mo[i * 2 + 1] - wy) * 1080.0, e = std::sqrt(ex * ex + ey * ey);
			worst = std::max(worst, e); mean += e / (SW * SH); size += std::sqrt(double(wx) * wx * 1920.0 * 1920.0 + double(wy) * wy * 1080.0 * 1080.0) / (SW * SH);
		}
		say("  camera turned 2 + 1 degrees and moved %.0f units: the sky's motion (mean %.1f px) vs a direction at infinity: mean error %.4f px, worst %.4f px (1920 x 1080)\n",
			moves[k], size, mean, worst);
		if (k == 0 && worst > 0.1) pass = false;
	}
	prep->Release();
	freeTex({ &tDepth, &tZero, &oMotion, &oDepth });
	say("    (a walk's sky motion within 0.1 px of the exact: the sky does not need its own motion for RR) -> %s\n", pass ? "ok" : "BAD");
	return pass;
}
