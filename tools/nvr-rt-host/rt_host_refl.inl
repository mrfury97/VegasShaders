// ==============================================================================================================================
// (2026-11-07) The RT reflection denoiser (src/core/RtReflDenoise.h, docs/next/RT-REFLECTION-DENOISER.md) against the EXACT glossy
// reflection, still and moving, next to the old path (ReflectionsTemporal.frag + ReflectionsBlur.frag X / Y, ported here line for line
// on the CPU: they read Reflections' whole constant block, which the host does not build).
//
// The world is analytic (no helper): a floor (z = 0) in four roughness bands (0.02 mirror, 0.08, 0.2, 0.4 along x), four boxes
// standing on it (red, green, a thin bright panel, blue) and a sky gradient. The boxes and the sky are constant-colour emitters, so a
// reflected ray's radiance is the colour of what it meets. The signal is what ReflectionsRayGen.frag + Resolve make: ONE ray a
// grid pixel a frame, its half vector from the GGX distribution of visible normals (VisibleNormals, the default) driven by the same
// interleaved gradient noise (NoiseFrame = frame % 64), RoughShare 0.3 (above it one pixel of each 2 x 2 block casts, the others borrow
// its hit), mirrors (< 0.03) the exact mirror ray. Exact = the same estimator's expectation: 32 x 32 stratified (u1, u2) per pixel.
// Errors: RMSE of rgb against the exact, per band, over the floor's glossy pixels; "contact" = floor pixels whose mean hit is < 60 units
// away (a reflection touching its object); "flicker" = mean |frame - last frame| at rest (frames 49-64).

struct RBox { V3 lo, hi, col; };
struct ReflWorld {
	std::vector<RBox> boxes;
	static float Rough(float x) { return x < -250.0f ? 0.02f : x < 50.0f ? 0.08f : x < 350.0f ? 0.2f : 0.4f; }
	static int Band(float r) { return r < 0.03f ? 0 : r < 0.1f ? 1 : r < 0.3f ? 2 : 3; }
	static V3 Sky(V3 d) {
		const float z = std::max(d.z, 0.0f);
		const V3 h = { 0.75f, 0.8f, 0.85f }, top = { 0.25f, 0.4f, 0.85f };
		const float s = std::sqrt(z);
		return h * (1 - s) + top * s;
	}
	// the nearest box along o + d t (t in (tMin, tMax)); -1: none
	int Boxes(V3 o, V3 d, float tMin, float& t) const {
		int best = -1;
		for (size_t k = 0; k < boxes.size(); k++) {
			float t0 = tMin, t1 = t;
			bool hit = true;
			for (int a = 0; a < 3 && hit; a++) {
				const float oa = (&o.x)[a], da = (&d.x)[a], lo = (&boxes[k].lo.x)[a], hi = (&boxes[k].hi.x)[a];
				if (std::fabs(da) < 1e-12f) { if (oa < lo || oa > hi) hit = false; continue; }
				float n = (lo - oa) / da, f = (hi - oa) / da;
				if (n > f) std::swap(n, f);
				t0 = std::max(t0, n); t1 = std::min(t1, f);
				if (t0 > t1) hit = false;
			}
			if (hit) { t = t0; best = int(k); }
		}
		return best;
	}
};
static ReflWorld reflWorld() {
	ReflWorld w;
	w.boxes = { { { -450, 150, 0 }, { -300, 300, 260 }, { 0.8f, 0.15f, 0.1f } },
		{ { -120, 350, 0 }, { 20, 450, 400 }, { 0.15f, 0.7f, 0.2f } },
		{ { 150, 250, 60 }, { 250, 270, 220 }, { 3.0f, 3.0f, 2.6f } },
		{ { 420, 150, 0 }, { 560, 300, 200 }, { 0.2f, 0.3f, 0.9f } } };
	return w;
}
static const float kReflMiss = 6000.0f;   // a ray that meets nothing: the hit distance Resolve writes (RT Distance)

struct ReflPixel { bool floor = false; V3 pos; float viewZ = 0, rough = 0; };
static void reflGbuffer(const ReflWorld& w, const Camera& c, std::vector<ReflPixel>& px, std::vector<float>& depth, std::vector<float>& normals,
	std::vector<float>& ray) {
	px.assign(S * S, ReflPixel()); depth.assign(S * S, 1.0f); normals.assign(S * S * 4, 0.5f); ray.assign(S * S * 2, 0.0f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const V3 d = c.ray((i + 0.5f) / S, (j + 0.5f) / S);
		float t = 1e9f;
		const float tf = d.z < 0 ? -c.eye.z / d.z : 1e9f;
		t = tf;
		const int box = w.Boxes(c.eye, d, 1e-3f, t);
		if (t >= 1e9f) continue;
		const size_t p = j * S + i;
		depth[p] = t / c.farZ;
		V3 n = { 0, 0, 1 };
		if (box >= 0) {   // the box face's normal
			const V3 q = c.eye + d * t; const RBox& b = w.boxes[box];
			const float e[6] = { std::fabs(q.x - b.lo.x), std::fabs(q.x - b.hi.x), std::fabs(q.y - b.lo.y), std::fabs(q.y - b.hi.y), std::fabs(q.z - b.lo.z), std::fabs(q.z - b.hi.z) };
			const int k = int(std::min_element(e, e + 6) - e);
			const V3 ns[6] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
			n = ns[k];
		} else {
			ReflPixel& g = px[p];
			g.floor = true; g.pos = c.eye + d * t; g.viewZ = t; g.rough = ReflWorld::Rough(g.pos.x);
			ray[p * 2] = t; ray[p * 2 + 1] = g.rough + 2.0f;
		}
		float* nn = &normals[p * 4];
		nn[0] = dot(n, c.r) * 0.5f + 0.5f; nn[1] = dot(n, c.u) * 0.5f + 0.5f; nn[2] = dot(n, c.f) * 0.5f + 0.5f; nn[3] = 1.0f;
	}
}

// ReflectionsRayGen.frag's direction for noise (u1, u2) (VisibleNormals), then the radiance along it and the hit distance
static float reflIgn(float x, float y, float frame) {
	x += frame * 5.588238f; y += frame * 5.588238f;
	const float a = x * 0.06711056f + y * 0.00583715f;
	const float f = 52.9829189f * (a - std::floor(a));
	return f - std::floor(f);
}
static V3 reflSample(const ReflWorld& w, const Camera& c, const ReflPixel& g, float u1, float u2, float& hitDist) {
	const V3 N = { 0, 0, 1 }, V = normalize(g.pos - c.eye);
	const V3 reflectN = V - N * (2 * dot(V, N));
	V3 R = reflectN;
	if (g.rough >= 0.03f) {
		const float a = g.rough * g.rough;
		const V3 T = normalize(std::fabs(N.z) < 0.999f ? cross(N, { 0, 0, 1 }) : cross(N, { 1, 0, 0 })), B = cross(N, T);
		const V3 Vt = { dot(-V, T), dot(-V, B), dot(-V, N) };
		const V3 Vh = normalize({ a * Vt.x, a * Vt.y, Vt.z });
		const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
		const V3 T1 = lensq > 0 ? V3(-Vh.y, Vh.x, 0) * (1.0f / std::sqrt(lensq)) : V3(1, 0, 0), T2 = cross(Vh, T1);
		const float r = std::sqrt(u1), ph = 6.2831853f * u2;
		float t1 = r * std::cos(ph), t2 = r * std::sin(ph);
		const float s = 0.5f * (1 + Vh.z);
		t2 = (1 - s) * std::sqrt(std::max(0.0f, 1 - t1 * t1)) + s * t2;
		const V3 Nh = T1 * t1 + T2 * t2 + Vh * std::sqrt(std::max(0.0f, 1 - t1 * t1 - t2 * t2));
		const V3 Ht = normalize({ a * Nh.x, a * Nh.y, std::max(0.0f, Nh.z) });
		const V3 H = normalize(T * Ht.x + B * Ht.y + N * Ht.z);
		R = V - H * (2 * dot(V, H));
		if (dot(R, N) <= 0) R = reflectN;
	}
	float t = kReflMiss;
	const int box = w.Boxes(g.pos + R * 0.01f, R, 0.0f, t);
	hitDist = box >= 0 ? t : kReflMiss;
	return box >= 0 ? w.boxes[box].col : ReflWorld::Sky(R);
}
// One frame's signal (Resolve's output: rgb, confidence 1) + hit distances, RoughShare's 2 x 2 borrowing as RayGen / Resolve do it
// gameNoise: RayGen's noise as it was (u2 = the same gradient noise at pixel + (37, 17): for every pixel and frame u2 - u1 is one of two
// constants, so a lobe is only ever sampled along one line); otherwise the fix the denoiser comes with (u2 from the swapped pixel
// + NoiseFrame x the golden ratio: the pairs fill the square per pixel over 64 frames and across a block)
static void reflSignal(const ReflWorld& w, const Camera& c, const std::vector<ReflPixel>& px, uint32_t frame, std::vector<float>& sig, std::vector<float>& hit,
	bool gameNoise) {
	sig.assign(S * S * 4, 0.0f); hit.assign(S * S, 0.0f);
	const float nf = float(frame % 64);
	std::vector<V3> L(S * S);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const size_t p = j * S + i;
		if (!px[p].floor) continue;
		float hd;
		float u2 = gameNoise ? reflIgn(i + 37.0f, j + 17.0f, nf) : reflIgn(j + 37.0f, i + 17.0f, nf) + nf * 0.61803398875f;
		u2 -= std::floor(u2);
		L[p] = reflSample(w, c, px[p], reflIgn(float(i), float(j), nf), u2, hd);
		hit[p] = hd;
	}
	const int caster = int(nf) & 3;
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
		const size_t p = j * S + i;
		if (!px[p].floor) continue;
		size_t from = p;
		if (px[p].rough > 0.3f && int((i & 1) + 2 * (j & 1)) != caster) {
			const size_t q = ((j & ~1u) + (caster >> 1)) * S + (i & ~1u) + (caster & 1);
			if (px[q].floor) from = q;
		}
		sig[p * 4] = L[from].x; sig[p * 4 + 1] = L[from].y; sig[p * 4 + 2] = L[from].z; sig[p * 4 + 3] = 1.0f;
		hit[p] = hit[from];
	}
}
// The exact: the estimator's expectation (32 x 32 stratified noise), + the mean hit distance (contact)
static void reflExact(const ReflWorld& w, const Camera& c, const std::vector<ReflPixel>& px, std::vector<float>& exact, std::vector<float>& meanHit) {
	exact.assign(S * S * 4, 0.0f); meanHit.assign(S * S, 0.0f);
	const int K = 32;
	for (UINT p = 0; p < S * S; p++) {
		if (!px[p].floor) continue;
		V3 sum; double hs = 0;
		for (int b = 0; b < K; b++) for (int a = 0; a < K; a++) {
			float hd;
			sum = sum + reflSample(w, c, px[p], (a + 0.5f) / K, (b + 0.5f) / K, hd);
			hs += std::min(hd, 2000.0f);
			if (px[p].rough < 0.03f) { a = b = K; }   // a mirror: one ray
		}
		const float n = px[p].rough < 0.03f ? 1.0f : float(K * K);
		exact[p * 4] = sum.x / n; exact[p * 4 + 1] = sum.y / n; exact[p * 4 + 2] = sum.z / n; exact[p * 4 + 3] = 1.0f;
		meanHit[p] = float(hs / n);
	}
}

// camera-relative world now -> last frame's clip (column-major: m[col * 4 + row]), as Reflections.cpp builds TESR_ReflectionsReproject
static void reflReproject(const Camera& now, const Camera& last, float* m) {
	std::memset(m, 0, 64);
	const V3 mv = now.eye - last.eye;
	const V3 rows[3] = { last.r * last.p00, last.u * last.p11, last.f };
	const int rowIndex[3] = { 0, 1, 3 };
	for (int k = 0; k < 3; k++) {
		const int row = rowIndex[k];
		m[0 * 4 + row] = rows[k].x; m[1 * 4 + row] = rows[k].y; m[2 * 4 + row] = rows[k].z; m[3 * 4 + row] = dot(mv, rows[k]);
	}
	m[2 * 4 + 2] = 0.0f;
}

// ---- the old path, line for line (ReflectionsTemporal.frag, ReflectionsBlur.frag; defaults Temporal 0.85, BlurRadius 12,
// ContactSharpness 0.3, ContactSpread 0.5); its history is its own Temporal output
struct OldPath {
	std::vector<float> history;
	bool have = false;
	void Run(const std::vector<float>& sig, const std::vector<float>& hit, const std::vector<ReflPixel>& px, const std::vector<float>& depth,
		const Camera& c, const Camera& last, bool usable, std::vector<float>& out) {
		const int N = int(S);
		auto at = [&](const std::vector<float>& v, int i, int j, int ch) { i = std::min(std::max(i, 0), N - 1); j = std::min(std::max(j, 0), N - 1); return &v[(size_t(j) * N + i) * ch]; };
		std::vector<float> temp(S * S * 4);
		const float weight = usable && have ? 0.85f : 0.0f;
		float m[16]; reflReproject(c, last, m);
		for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
			const float* cur = at(sig, i, j, 4);
			float lo[4], hi[4], h[4];
			for (int k = 0; k < 4; k++) lo[k] = hi[k] = cur[k];
			for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
				const float* t = at(sig, i + x, j + y, 4);
				for (int k = 0; k < 4; k++) { lo[k] = std::min(lo[k], t[k]); hi[k] = std::max(hi[k], t[k]); }
			}
			const float z = *at(depth, i, j, 1) * c.farZ;
			const V3 p = c.ray((i + 0.5f) / N, (j + 0.5f) / N) * z;
			const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
			const float cw = std::max(m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15], 0.001f);
			const float lu = cx / cw * 0.5f + 0.5f, lv = 0.5f - cy / cw * 0.5f;
			const bool inside = lu > 0 && lu < 1 && lv > 0 && lv < 1 && cur[3] + hi[3] > 0;
			const float* hs = have ? at(history, int(std::floor(lu * N)), int(std::floor(lv * N)), 4) : cur;
			const size_t q = size_t(j) * N + i;
			const float keep = px[q].floor && px[q].rough < 0.03f ? 0.0f : 1.0f;
			for (int k = 0; k < 4; k++) {
				h[k] = std::min(std::max(hs[k], lo[k]), hi[k]);
				temp[q * 4 + k] = cur[k] + (h[k] - cur[k]) * weight * (inside ? 1.0f : 0.0f) * keep;
			}
		}
		history = temp;
		have = true;
		// Blur X (Temp -> Trace), Blur Y (Trace -> Buffer)
		std::vector<float> mid(S * S * 4);
		for (int pass = 0; pass < 2; pass++) {
			const std::vector<float>& src = pass == 0 ? temp : mid;
			std::vector<float>& dst = pass == 0 ? mid : out;
			dst.assign(S * S * 4, 0.0f);
			for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
				const size_t q = size_t(j) * N + i;
				const float* centre = &src[q * 4];
				if (!px[q].floor) { for (int k = 0; k < 4; k++) dst[q * 4 + k] = centre[k]; continue; }
				const float r = px[q].rough;
				const float ss = std::min(std::max((r - 0.03f) / 0.12f, 0.0f), 1.0f);
				float radius = 12.0f * std::min(std::max(0.5f + r * 2.0f, 0.0f), 1.0f) * (ss * ss * (3 - 2 * ss));
				const float hd = hit[q];
				const float spread = hd > 0 && hd < kReflMiss ? hd / (hd + px[q].viewZ * 0.5f) : 1.0f;
				radius *= 0.3f + (1.0f - 0.3f) * spread;
				const float sigma = std::max(radius * 0.5f, 0.5f), falloff = -0.5f / (sigma * sigma);
				const float dz = *at(depth, i, j, 1) * c.farZ, tol = dz * 0.03f + 4.0f;
				float sum[4] = { centre[0], centre[1], centre[2], centre[3] }, ws = 1.0f;
				for (int n = 1; n <= 8; n++) {
					const float g = n <= radius ? std::exp(falloff * n * n) : 0.0f;
					const int ax = pass == 0 ? n : 0, ay = pass == 0 ? 0 : n;
					const float da = *at(depth, i + ax, j + ay, 1) * c.farZ, db = *at(depth, i - ax, j - ay, 1) * c.farZ;
					const float wa = g / (1 + std::fabs(da - dz) / tol), wb = g / (1 + std::fabs(db - dz) / tol);
					const float* sa = at(src, i + ax, j + ay, 4); const float* sb = at(src, i - ax, j - ay, 4);
					for (int k = 0; k < 4; k++) sum[k] += sa[k] * wa + sb[k] * wb;
					ws += wa + wb;
				}
				for (int k = 0; k < 4; k++) dst[q * 4 + k] = sum[k] / ws;
			}
		}
	}
};

struct ReflErr { double band[4] = { 0, 0, 0, 0 }; double all = 0, contact = 0; int n[4] = { 0, 0, 0, 0 }, nAll = 0, nContact = 0; };
static ReflErr reflError(const std::vector<float>& res, const std::vector<float>& exact, const std::vector<ReflPixel>& px, const std::vector<float>& meanHit) {
	ReflErr e;
	for (UINT p = 0; p < S * S; p++) {
		if (!px[p].floor) continue;
		const int b = ReflWorld::Band(px[p].rough);
		double d = 0;
		for (int k = 0; k < 3; k++) { const double x = double(res[p * 4 + k]) - exact[p * 4 + k]; d += x * x; }
		d /= 3;
		if (!std::isfinite(d)) d = 1e6;
		e.band[b] += d; e.n[b]++;
		if (b > 0) { e.all += d; e.nAll++; if (meanHit[p] < 60.0f) { e.contact += d; e.nContact++; } }
	}
	for (int b = 0; b < 4; b++) e.band[b] = e.n[b] ? std::sqrt(e.band[b] / e.n[b]) : 0;
	e.all = e.nAll ? std::sqrt(e.all / e.nAll) : 0;
	e.contact = e.nContact ? std::sqrt(e.contact / e.nContact) : 0;
	return e;
}
static std::string reflErrText(const ReflErr& e) {
	char t[200];
	snprintf(t, sizeof(t), "all %.4f | r0.08 %.4f  r0.2 %.4f  r0.4 %.4f | contact %.4f | mirror %.4f", e.all, e.band[1], e.band[2], e.band[3], e.contact, e.band[0]);
	return t;
}
static void dumpPfmRgb(const char* name, const std::vector<float>& rgba) {
	if (gOut.empty()) return;
	FILE* f = nullptr; fopen_s(&f, (gOut + "\\" + name + ".pfm").c_str(), "wb");
	if (!f) return;
	std::fprintf(f, "PF\n%u %u\n-1.0\n", S, S);
	for (int j = int(S) - 1; j >= 0; j--) for (UINT i = 0; i < S; i++) std::fwrite(&rgba[(size_t(j) * S + i) * 4], 4, 3, f);
	std::fclose(f);
}
static bool readSurfaceRgba(Host& h, IDirect3DSurface9* s, std::vector<float>& v) {
	static IDirect3DSurface9* sys = nullptr;
	if (!sys) CHECKHR(h.dev->CreateOffscreenPlainSurface(S, S, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr), "sys surface");
	CHECKHR(h.dev->GetRenderTargetData(s, sys), "GetRenderTargetData");
	D3DLOCKED_RECT lr;
	CHECKHR(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "lock");
	v.assign(S * S * 4, 0.0f);
	for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S * 4; i++) v[j * S * 4 + i] = fromHalf(((const uint16_t*)((const char*)lr.pBits + j * lr.Pitch))[i]);
	sys->UnlockRect();
	return true;
}

static bool sceneReflDenoise(Ctx& c) {
	Host& h = *c.host;
	const ReflWorld w = reflWorld();
	Tex sigT, rayT, hitT, nrmT;
	if (!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, sigT) || !makeTex(h, S, S, D3DFMT_G32R32F, false, rayT) || !makeTex(h, S, S, D3DFMT_G16R16F, false, hitT) ||
		!makeTex(h, S, S, D3DFMT_A16B16G16R16F, false, nrmT)) return false;
	ID3D9NvrExtSampler *linear = nullptr, *point = nullptr;
	{ NVR_EXT_SAMPLER_DESC a = { NVR_EXT_FILTER_LINEAR, NVR_EXT_ADDRESS_CLAMP }, b = { NVR_EXT_FILTER_NEAREST, NVR_EXT_ADDRESS_CLAMP };
	  CHECKHR(h.ext->CreateSampler(&a, &linear), "sampler"); CHECKHR(h.ext->CreateSampler(&b, &point), "sampler"); }
	auto loader = [&](const char* name, std::vector<char>& spirv) {
		FILE* f = nullptr; fopen_s(&f, (gM.compute + "\\" + name + ".spv").c_str(), "rb");
		if (!f) { say("  error: no %s.spv in %s\n", name, gM.compute.c_str()); return false; }
		char buf[65536]; size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) spirv.insert(spirv.end(), buf, buf + n);
		fclose(f);
		return true;
	};
	const char* coreEnv = std::getenv("NVR_REFL_CORES");   // which cores to run (default "1": FFX; "0,1" both)
	std::vector<int> cores;
	for (const char* s = coreEnv ? coreEnv : "0,1"; *s; s++) if (*s >= '0' && *s <= '1') cores.push_back(*s - '0');
	std::vector<RtReflDenoise::Denoiser> dn(cores.size());
	for (auto& d : dn) if (!d.Init(h.dev, h.ext, S, S, linear, point, loader)) { say("  error: denoiser images\n"); return false; }
	RtReflDenoise::Settings set;

	const Camera camA = Camera::lookDir({ 50, -650, 300 }, V3(50, 350, 0) - V3(50, -650, 300), { 0, 0, 1 }, 1.0f, 1.0f);
	auto frameOf = [&](const Camera& cm, const Camera& last, bool usable) {
		RtReflDenoise::Frame f = {};
		reflReproject(cm, last, f.reproject);
		f.forward[0] = cm.f.x; f.forward[1] = cm.f.y; f.forward[2] = cm.f.z; f.farZ = cm.farZ;
		const V3 r = cm.r * (1.0f / cm.p00), u = cm.u * (1.0f / cm.p11);
		f.right[0] = r.x; f.right[1] = r.y; f.right[2] = r.z; f.up[0] = u.x; f.up[1] = u.y; f.up[2] = u.z;
		f.historyUsable = usable;
		return f;
	};
	struct Run { const char* name; std::vector<Camera> cams; std::vector<int> eval; };
	// still: 64 frames at A. slide: 32 still, then 48 frames sideways 6 units a frame. turn: 32 still, then 48 frames turning 0.4 deg.
	std::vector<Run> runs(3);
	runs[0].name = "still"; for (int f = 0; f < 64; f++) runs[0].cams.push_back(camA);
	runs[0].eval = { 0, 3, 15, 63 };
	runs[1].name = "slide"; runs[2].name = "turn";
	for (int f = 0; f < 80; f++) {
		Camera s = camA; if (f >= 32) s.eye = camA.eye + V3(6.0f * (f - 31), 0, 0);
		runs[1].cams.push_back(s);
		const float yaw = f >= 32 ? 0.4f * (f - 31) * 3.14159265f / 180.0f : 0.0f;
		const V3 fw = camA.f;
		const V3 rot = { fw.x * std::cos(yaw) - fw.y * std::sin(yaw), fw.x * std::sin(yaw) + fw.y * std::cos(yaw), fw.z };
		runs[2].cams.push_back(Camera::lookDir(camA.eye, rot, { 0, 0, 1 }, 1.0f, 1.0f));
	}
	runs[1].eval = runs[2].eval = { 39, 47, 55, 63, 71, 79 };

	bool pass = true;
	double stillOld = 0, stillCore[2] = { 0, 0 }, moveWorstOld = 0, moveCore[2] = { 0, 0 }, rawStill = 0, contactOld = 0, contactCore[2] = { 0, 0 };
	for (const Run& run : runs) {
		OldPath old, oldGame;
		std::vector<float> sigGame, hitGame, oldGameOut, meanRaw(S * S * 4, 0.0f), meanGame(S * S * 4, 0.0f);
		for (auto& d : dn) d.Restart();
		std::vector<ReflPixel> px; std::vector<float> depth, normals, ray, sig, hit, exact, meanHit, oldOut, prevOld, prevCore[2], res;
		double flickRaw = 0, flickOld = 0, flickCore[2] = { 0, 0 }; int flickN = 0;
		std::vector<float> prevSig;
		say("  %s (%s):\n", run.name, run.cams.size() == 64 ? "64 frames at rest" : "32 frames at rest, then 48 moving");
		for (size_t f = 0; f < run.cams.size(); f++) {
			const Camera& cm = run.cams[f];
			const Camera& last = run.cams[f ? f - 1 : 0];
			reflGbuffer(w, cm, px, depth, normals, ray);
			reflSignal(w, cm, px, uint32_t(f), sig, hit, false);
			reflSignal(w, cm, px, uint32_t(f), sigGame, hitGame, true);
			oldGame.Run(sigGame, hitGame, px, depth, cm, last, f > 0, oldGameOut);
			if (run.cams.size() == 64) for (size_t q = 0; q < meanRaw.size(); q++) { meanRaw[q] += sig[q] / 64.0f; meanGame[q] += sigGame[q] / 64.0f; }
			std::vector<float> hit2(S * S * 2, 0.0f);   // Resolve's Spread image: x the old blur's share, y the hit distance
			for (UINT q = 0; q < S * S; q++) hit2[q * 2 + 1] = hit[q];
			if (!fillTex(sigT, sig, 4) || !fillTex(rayT, ray, 2) || !fillTex(hitT, hit2, 2) || !fillTex(nrmT, normals, 4)) return false;
			old.Run(sig, hit, px, depth, cm, last, f > 0, oldOut);
			std::vector<float> coreOut[2];
			for (size_t k = 0; k < cores.size(); k++) {
				set.Core = cores[k];
				if (!dn[k].Run(sigT.t, rayT.t, hitT.t, nrmT.t, nullptr, frameOf(cm, last, f > 0), set)) { say("  error: denoiser pass (core %d)\n", cores[k]); return false; }
				if (!readSurfaceRgba(h, dn[k].ResultSurface(), coreOut[cores[k]])) return false;
			}
			const bool evalNow = std::find(run.eval.begin(), run.eval.end(), int(f)) != run.eval.end();
			if (run.cams.size() == 64 && f >= 48) {   // flicker at rest
				auto diff = [&](const std::vector<float>& a, const std::vector<float>& b) { double s = 0; int n = 0; for (UINT p = 0; p < S * S; p++) if (px[p].floor && px[p].rough >= 0.03f) { for (int k = 0; k < 3; k++) s += std::fabs(a[p * 4 + k] - b[p * 4 + k]); n += 3; } return n ? s / n : 0.0; };
				flickRaw += diff(sig, prevSig); flickOld += diff(oldOut, prevOld);
				for (int k : cores) flickCore[k] += diff(coreOut[k], prevCore[k]);
				flickN++;
			}
			prevSig = sig; prevOld = oldOut; for (int k : cores) prevCore[k] = coreOut[k];
			if (!evalNow) continue;
			reflExact(w, cm, px, exact, meanHit);
			const ReflErr eRaw = reflError(sig, exact, px, meanHit), eOld = reflError(oldOut, exact, px, meanHit);
			say("    frame %2zu  raw  %s\n", f + 1, reflErrText(eRaw).c_str());
			const ReflErr eOldGame = reflError(oldGameOut, exact, px, meanHit);
			say("              old  %s   (today: RayGen's noise as shipped)\n", reflErrText(eOldGame).c_str());
			say("              old' %s   (the old path on the fixed noise)\n", reflErrText(eOld).c_str());
			if (run.cams.size() == 64 && int(f) == run.eval.back()) {
				say("    64-frame mean of the raw signal (what endless averaging reaches): today's noise %s\n", reflErrText(reflError(meanGame, exact, px, meanHit)).c_str());
				say("                                                                      fixed noise   %s\n", reflErrText(reflError(meanRaw, exact, px, meanHit)).c_str());
			}
			ReflErr eCore[2];
			for (int k : cores) { eCore[k] = reflError(coreOut[k], exact, px, meanHit); say("              %s %s\n", k == 1 ? "FFX " : "ours", reflErrText(eCore[k]).c_str()); }
			const bool lastEval = int(f) == run.eval.back();
			if (run.cams.size() == 64 && lastEval) {
				stillOld = std::min(eOld.all, eOldGame.all); rawStill = eRaw.all; contactOld = std::min(eOld.contact, eOldGame.contact);
				for (int k : cores) { stillCore[k] = eCore[k].all; contactCore[k] = eCore[k].contact; }
				char nm[64];
				dumpPfmRgb("refl_still_exact", exact); dumpPfmRgb("refl_still_raw", sig); dumpPfmRgb("refl_still_old", oldGameOut); dumpPfmRgb("refl_still_oldfixed", oldOut); dumpPfmRgb("refl_still_rawtoday", sigGame);
				for (int k : cores) { snprintf(nm, sizeof(nm), "refl_still_%s", k ? "ffx" : "ours"); dumpPfmRgb(nm, coreOut[k]); }
			}
			if (run.cams.size() != 64 && f >= 39) {
				moveWorstOld = std::max(moveWorstOld, std::min(eOld.all, eOldGame.all));
				for (int k : cores) moveCore[k] = std::max(moveCore[k], eCore[k].all);
				if (f == 55) {
					char nm[64];
					snprintf(nm, sizeof(nm), "refl_%s_exact", run.name); dumpPfmRgb(nm, exact);
					snprintf(nm, sizeof(nm), "refl_%s_raw", run.name); dumpPfmRgb(nm, sig);
					snprintf(nm, sizeof(nm), "refl_%s_old", run.name); dumpPfmRgb(nm, oldGameOut);
					for (int k : cores) { snprintf(nm, sizeof(nm), "refl_%s_%s", run.name, k ? "ffx" : "ours"); dumpPfmRgb(nm, coreOut[k]); }
					// ours' moments: red = the virtual (hit-point) share, green = history length / MaxSamples
					for (size_t k = 0; k < cores.size(); k++) if (cores[k] == 0) {
						std::vector<float> mo;
						if (readSurfaceRgba(h, dn[k].MomentsSurface(), mo)) {
							for (UINT q = 0; q < S * S; q++) { const float n = mo[q * 4 + 1] / set.MaxSamples, v = mo[q * 4 + 3]; mo[q * 4] = v; mo[q * 4 + 1] = n; mo[q * 4 + 2] = 0; }
							snprintf(nm, sizeof(nm), "refl_%s_moments", run.name); dumpPfmRgb(nm, mo);
						}
					}
				}
			}
		}
		if (flickN) {
			say("    flicker at rest (mean |frame - last frame|, frames 49-64): raw %.4f, old %.4f", flickRaw / flickN, flickOld / flickN);
			for (int k : cores) say(", %s %.4f", k ? "FFX" : "ours", flickCore[k] / flickN);
			say("\n");
		}
	}
	for (auto& d : dn) d.Release();
	linear->Release(); point->Release();
	for (Tex* t : { &sigT, &rayT, &hitT, &nrmT }) { if (t->s) t->s->Release(); if (t->t) t->t->Release(); }
	// PASS: the default core (ours, else the only one run) beats the old path at rest (all + contact) and in motion (worst frame)
	const int k = std::find(cores.begin(), cores.end(), 0) != cores.end() ? 0 : cores[0];
	pass = stillCore[k] < stillOld && contactCore[k] < contactOld * 1.05 && moveCore[k] < moveWorstOld && stillCore[k] < rawStill * 0.5;
	say("  verdict (%s): at rest %.4f vs old %.4f (raw %.4f), contact %.4f vs old %.4f, moving worst %.4f vs old %.4f\n", k ? "FFX" : "ours",
		stillCore[k], stillOld, rawStill, contactCore[k], contactOld, moveCore[k], moveWorstOld);
	return pass;
}
