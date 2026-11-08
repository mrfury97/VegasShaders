// (2026-11-07, rtgap-tessblas, protocol 8) displaced geometry in the RT scene (docs/next/RT-DETAIL-GAPS.md 1c; NvrRtDisplace.inl +
// RtDisplace.comp in the helper). A rolling land grid with two height layers blended by per-vertex weights (staged as NVR does it: the
// weights sent through NVR_RT_OP_DATA with the weight element as the "position"), a quadrant border across it and the distance fade
// inside the view, plus a scaled parallax "rock" (a box, height in red, edges pinned). The CPU builds the TRUTH: the same N x N split
// with the game's tessellation formulas (TerrainDisplace.tesc/.tese, ObjectDisplace.tesc/.tese, transcribed here independently of the
// helper's kernel) from the same texels. ReflectionsRT's rays (start in the air, aimed down over the bumps) are compared hit for hit
// with the CPU's rays on the truth: their distance, and the sun's shadow ray at the hit (the bumps' own shadows). Controls: the same
// rays against the FLAT geometry must disagree (the test sees the bumps), and with the displacement switched off the helper must give
// the flat answers. Also printed: how far the RT's split (N from the spacing) is from a much finer split of the same formula.

namespace TessBlas {

	// ---- textures: RGBA8, a full mip chain (box filter), sampled on the CPU as Vulkan does (linear, repeat, textureLod)
	struct HTex {
		int size = 0, levels = 0;
		std::vector<std::vector<uint8_t>> lv;   // RGBA8 per level
		std::vector<uint8_t> packed() const { std::vector<uint8_t> o; for (auto& l : lv) o.insert(o.end(), l.begin(), l.end()); return o; }
		float fetch(int l, int x, int y, int c) const {
			const int s = std::max(1, size >> l);
			x = ((x % s) + s) % s; y = ((y % s) + s) % s;
			return lv[l][(size_t(y) * s + x) * 4 + c] / 255.0f;
		}
		float bilinear(int l, float u, float v, int c) const {
			const int s = std::max(1, size >> l);
			const float px = u * s - 0.5f, py = v * s - 0.5f;
			const float fx = std::floor(px), fy = std::floor(py);
			const float ax = px - fx, ay = py - fy;
			const int x0 = int(fx), y0 = int(fy);
			const float a = fetch(l, x0, y0, c) * (1 - ax) + fetch(l, x0 + 1, y0, c) * ax;
			const float b = fetch(l, x0, y0 + 1, c) * (1 - ax) + fetch(l, x0 + 1, y0 + 1, c) * ax;
			return a * (1 - ay) + b * ay;
		}
		float atLevel(float u, float v, float lod, int c) const {   // textureLod at a whole level (clamped to the chain)
			const int l = std::min(std::max(int(lod), 0), levels - 1);
			return bilinear(l, u, v, c);
		}
		float between(float u, float v, float lod, int c) const {   // the TES's HEIGHT_AT: two levels mixed by the fraction
			const float fl = std::floor(lod);
			return atLevel(u, v, fl, c) * (1 - (lod - fl)) + atLevel(u, v, fl + 1, c) * (lod - fl);
		}
		float mean(int c) const { return levels > 1 ? fetch(levels - 1, 0, 0, c) : 0.5f; }
	};
	static HTex makeTex(int size, const std::function<void(float, float, float*)>& fn) {
		HTex t; t.size = size;
		int s = size; t.levels = 0;
		while (true) { t.levels++; if (s == 1) break; s >>= 1; }
		t.lv.resize(t.levels);
		t.lv[0].resize(size_t(size) * size * 4);
		for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
			float c[4]; fn((x + 0.5f) / size, (y + 0.5f) / size, c);
			for (int k = 0; k < 4; k++) t.lv[0][(size_t(y) * size + x) * 4 + k] = uint8_t(std::min(std::max(c[k], 0.0f), 1.0f) * 255.0f + 0.5f);
		}
		for (int l = 1; l < t.levels; l++) {
			const int ps = size >> (l - 1), cs = std::max(1, size >> l);
			t.lv[l].resize(size_t(cs) * cs * 4);
			for (int y = 0; y < cs; y++) for (int x = 0; x < cs; x++) for (int k = 0; k < 4; k++) {
				const auto& p = t.lv[l - 1];
				const int sum = p[(size_t(2 * y) * ps + 2 * x) * 4 + k] + p[(size_t(2 * y) * ps + 2 * x + 1) * 4 + k] +
					p[(size_t(2 * y + 1) * ps + 2 * x) * 4 + k] + p[(size_t(2 * y + 1) * ps + 2 * x + 1) * 4 + k];
				t.lv[l][(size_t(y) * cs + x) * 4 + k] = uint8_t((sum + 2) / 4);
			}
		}
		return t;
	}

	// ---- the CPU's triangles (world space) and a BVH over them
	struct Soup {
		std::vector<V3> p;
		std::vector<uint32_t> tri;     // 3 a triangle
		std::vector<uint8_t> land;     // per triangle
		struct Node { float lo[3], hi[3]; int left = -1, right = -1, first = 0, count = 0; };
		std::vector<Node> nodes;
		std::vector<uint32_t> order;
		void add(const std::vector<V3>& pts, const std::vector<uint32_t>& idx, bool isLand) {
			const uint32_t base = uint32_t(p.size());
			p.insert(p.end(), pts.begin(), pts.end());
			for (uint32_t i : idx) tri.push_back(base + i);
			land.insert(land.end(), idx.size() / 3, uint8_t(isLand ? 1 : 0));
		}
		void build() {
			const uint32_t n = uint32_t(tri.size() / 3);
			order.resize(n);
			for (uint32_t i = 0; i < n; i++) order[i] = i;
			std::vector<V3> cen(n);
			for (uint32_t i = 0; i < n; i++) cen[i] = (p[tri[3 * i]] + p[tri[3 * i + 1]] + p[tri[3 * i + 2]]) * (1.0f / 3.0f);
			nodes.clear(); nodes.reserve(2 * n / 4 + 2);
			std::function<int(int, int)> rec = [&](int first, int count) -> int {
				Node nd; nd.first = first; nd.count = count;
				for (int k = 0; k < 3; k++) { nd.lo[k] = 1e30f; nd.hi[k] = -1e30f; }
				float clo[3] = { 1e30f, 1e30f, 1e30f }, chi[3] = { -1e30f, -1e30f, -1e30f };
				for (int i = first; i < first + count; i++) {
					for (int v = 0; v < 3; v++) {
						const V3 q = p[tri[3 * order[i] + v]];
						const float a[3] = { q.x, q.y, q.z };
						for (int k = 0; k < 3; k++) { nd.lo[k] = std::min(nd.lo[k], a[k]); nd.hi[k] = std::max(nd.hi[k], a[k]); }
					}
					const V3 c = cen[order[i]]; const float a[3] = { c.x, c.y, c.z };
					for (int k = 0; k < 3; k++) { clo[k] = std::min(clo[k], a[k]); chi[k] = std::max(chi[k], a[k]); }
				}
				const int at = int(nodes.size());
				nodes.push_back(nd);
				if (count <= 4) return at;
				int axis = 0;
				for (int k = 1; k < 3; k++) if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
				const int mid = first + count / 2;
				std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count, [&](uint32_t a, uint32_t b) {
					const float ca = axis == 0 ? cen[a].x : axis == 1 ? cen[a].y : cen[a].z, cb = axis == 0 ? cen[b].x : axis == 1 ? cen[b].y : cen[b].z;
					return ca < cb; });
				const int l = rec(first, mid - first), r = rec(mid, first + count - mid);
				nodes[at].left = l; nodes[at].right = r; nodes[at].count = 0;
				return at;
			};
			if (n) rec(0, int(n));
		}
		struct Hit { bool hit = false; float t = 0; V3 n; bool land = false; };
		Hit trace(V3 o, V3 d, float tMin, float tMax) const {
			Hit best; best.t = tMax;
			if (nodes.empty()) return best;
			const float inv[3] = { 1.0f / (d.x != 0 ? d.x : 1e-30f), 1.0f / (d.y != 0 ? d.y : 1e-30f), 1.0f / (d.z != 0 ? d.z : 1e-30f) };
			const float oo[3] = { o.x, o.y, o.z };
			int stack[128]; int sp = 0; stack[sp++] = 0;
			while (sp) {
				const Node& nd = nodes[stack[--sp]];
				float t0 = tMin, t1 = best.t;
				bool miss = false;
				for (int k = 0; k < 3 && !miss; k++) {
					float a = (nd.lo[k] - oo[k]) * inv[k], b = (nd.hi[k] - oo[k]) * inv[k];
					if (a > b) std::swap(a, b);
					t0 = std::max(t0, a); t1 = std::min(t1, b * 1.0000005f + 1e-4f);
					if (t0 > t1) miss = true;
				}
				if (miss) continue;
				if (nd.left >= 0) { stack[sp++] = nd.left; stack[sp++] = nd.right; continue; }
				for (int i = nd.first; i < nd.first + nd.count; i++) {
					const uint32_t k = order[i];
					const V3 a = p[tri[3 * k]], b = p[tri[3 * k + 1]], c = p[tri[3 * k + 2]];
					const V3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
					const float det = dot(e1, pv);
					if (std::fabs(det) < 1e-12f) continue;
					const float id = 1.0f / det;
					const V3 tv = o - a;
					const float bu = dot(tv, pv) * id;
					if (bu < 0 || bu > 1) continue;
					const V3 qv = cross(tv, e1);
					const float bv = dot(d, qv) * id;
					if (bv < 0 || bu + bv > 1) continue;
					const float t = dot(e2, qv) * id;
					if (!(t > tMin && t < best.t)) continue;
					best.hit = true; best.t = t; best.land = land[k] != 0;
					V3 nn = normalize(cross(e1, e2));
					best.n = dot(nn, d) > 0 ? -nn : nn;
				}
			}
			return best;
		}
	};

	// ---- the game's tessellation formulas (TerrainDisplace.tesc/.tese, ObjectDisplace.tesc/.tese), on the CPU
	struct Shape { float strength, center, contrast, bump, lodBias, grid, fadeStart, fadeEnd, edgeFade; };
	static float smooth01(float a, float b, float x) { float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f); return t * t * (3 - 2 * t); }
	static float fadeAt(const Shape& s, float d) { return 1.0f - smooth01(s.fadeStart, std::max(s.fadeEnd, s.fadeStart + 1.0f), d); }
	static float ease(float x) { return 0.5f + 0.5f * x / std::sqrt(1.0f + x * x); }
	static float round256(float x) { return std::floor(x * 256.0f + 0.5f) / 256.0f; }

	// A source mesh with everything the formulas read per vertex
	struct Src {
		std::vector<V3> pos;            // object space
		std::vector<float> uv;          // 2 a vertex
		std::vector<uint32_t> idx;
		std::vector<float> weights;     // land: 7 a vertex
		std::vector<V3> nrm;            // smooth normals as the helper makes them (fixed point, land turned up)
		float maxEdge = 0;
		void normals(bool landUp) {
			std::vector<long long> acc(pos.size() * 3, 0);
			for (size_t t = 0; t + 2 < idx.size(); t += 3) {
				const V3 a = pos[idx[t]], b = pos[idx[t + 1]], c = pos[idx[t + 2]];
				V3 f = cross(b - a, c - a);
				if (dot(f, f) < 1e-12f) continue;
				f = normalize(f);
				if (landUp && f.z < 0) f = -f;
				const long long q[3] = { (long long)std::lround(f.x * 65536.0f), (long long)std::lround(f.y * 65536.0f), (long long)std::lround(f.z * 65536.0f) };
				for (int k = 0; k < 3; k++) for (int c2 = 0; c2 < 3; c2++) acc[idx[t + k] * 3 + c2] += q[c2];
			}
			nrm.resize(pos.size());
			for (size_t v = 0; v < pos.size(); v++) nrm[v] = normalize(V3(float(acc[3 * v]), float(acc[3 * v + 1]), float(acc[3 * v + 2])));
			maxEdge = 0;
			for (size_t t = 0; t + 2 < idx.size(); t += 3) {
				const V3 a = pos[idx[t]], b = pos[idx[t + 1]], c = pos[idx[t + 2]];
				maxEdge = std::max(maxEdge, std::max(length(b - a), std::max(length(c - b), length(a - c))));
			}
		}
	};

	// The displaced copy: every triangle N x N, every point by the formula. Land: eye in object space; object: forward + scene transform.
	static void displace(const Src& s, bool land, int n, const Shape& sh, const HTex* tex[2], uint32_t heightBits, V3 eyeObj, V3 fwd,
		float scale, V3 offsetScene, std::vector<V3>& out, std::vector<uint32_t>& outIdx) {
		out.clear(); outIdx.clear();
		const int points = (n + 1) * (n + 2) / 2;
		auto rowStart = [&](int b) { return b * (n + 1) - (b * (b - 1)) / 2; };
		const float spacing = s.maxEdge / float(n);
		for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
			const uint32_t i[3] = { s.idx[t], s.idx[t + 1], s.idx[t + 2] };
			const V3 P[3] = { s.pos[i[0]], s.pos[i[1]], s.pos[i[2]] };
			float U[3][2];
			for (int k = 0; k < 3; k++) { U[k][0] = s.uv[2 * i[k]]; U[k][1] = s.uv[2 * i[k] + 1]; }
			const uint32_t base = uint32_t(out.size());
			// corners (the TCS)
			float f[3], r[3], ofade[3];
			for (int k = 0; k < 3; k++) {
				if (land) {
					f[k] = fadeAt(sh, length(P[k] - eyeObj));
					r[k] = f[k];
					if (sh.grid > 0) {
						const float mx = std::fabs((P[k].x / sh.grid + 0.5f) - std::floor(P[k].x / sh.grid + 0.5f) - 0.5f) * sh.grid;
						const float my = std::fabs((P[k].y / sh.grid + 0.5f) - std::floor(P[k].y / sh.grid + 0.5f) - 0.5f) * sh.grid;
						if (std::min(mx, my) < 0.5f) r[k] = 0;
					}
				}
				else ofade[k] = fadeAt(sh, dot(fwd, P[k] * scale + offsetScene));
			}
			// land: the height's detail level through this triangle's uv-per-unit map
			float lodUv = -30.0f;
			if (land) {
				const float e1x = P[1].x - P[0].x, e1y = P[1].y - P[0].y, e2x = P[2].x - P[0].x, e2y = P[2].y - P[0].y;
				const float det2 = e1x * e2y - e1y * e2x;
				if (std::fabs(det2) > 1e-3f) {
					// J = [du1 du2; dv1 dv2] * inverse([e1 e2]) (columns), |det J| = |det(uv edges)| / |det(xy edges)|
					const float du1 = U[1][0] - U[0][0], dv1 = U[1][1] - U[0][1], du2 = U[2][0] - U[0][0], dv2 = U[2][1] - U[0][1];
					const float uvPerUnit = std::sqrt(std::fabs((du1 * dv2 - du2 * dv1) / det2));
					if (uvPerUnit > 0) {
						lodUv = round256(std::log2(spacing * uvPerUnit));
						if (sh.bump > 0) lodUv = std::max(lodUv, round256(std::log2(sh.bump * uvPerUnit)));
					}
				}
			}
			const V3 face = cross(P[1] - P[0], P[2] - P[0]);
			const bool degenerate = dot(face, face) < 1e-12f;
			for (int b = 0; b <= n; b++) for (int a = 0; a + b <= n; a++) {
				const float w1 = float(a) / n, w2 = float(b) / n, w0 = float(n - a - b) / n;
				const float w[3] = { w0, w1, w2 };
				V3 pt = P[0] * w0 + P[1] * w1 + P[2] * w2;
				const float u = U[0][0] * w0 + U[1][0] * w1 + U[2][0] * w2, v = U[0][1] * w0 + U[1][1] * w1 + U[2][1] * w2;
				if (!degenerate && sh.strength != 0) {
					const V3 nn = normalize(s.nrm[i[0]] * w0 + s.nrm[i[1]] * w1 + s.nrm[i[2]] * w2);
					float disp = 0;
					if (land) {
						const float weight = r[0] * w0 + r[1] * w1 + r[2] * w2, fadeZ = f[0] * w0 + f[1] * w1 + f[2] * w2;
						float h = 0.5f;
						if (weight > 0) {
							float hs = 0, ws = 0;
							for (int l = 0; l < 2; l++) {
								const float lw = s.weights[7 * i[0] + l] * w0 + s.weights[7 * i[1] + l] * w1 + s.weights[7 * i[2] + l] * w2;
								if (!(lw > 0.0005f)) continue;
								float rr = 0;
								if ((heightBits >> l) & 1u) {
									const float lod = std::max(lodUv + std::log2(float(tex[l]->size)) + sh.lodBias, 0.0f);
									rr = tex[l]->between(u, v, lod, 3) - tex[l]->mean(3);
								}
								hs += lw * rr; ws += lw;
							}
							h = ease(2.0f * sh.contrast * (ws > 0 ? hs / ws : 0.0f));
						}
						disp = sh.strength * (weight * (h - 0.5f) + fadeZ * (0.5f - sh.center));
					}
					else {
						const float fade = ofade[0] * w0 + ofade[1] * w1 + ofade[2] * w2;
						float pin = 1;
						if (sh.edgeFade > 0) {
							pin = std::min(std::min(std::min(std::max(w0 * n / sh.edgeFade, 0.0f), 1.0f), std::min(std::max(w1 * n / sh.edgeFade, 0.0f), 1.0f)),
								std::min(std::max(w2 * n / sh.edgeFade, 0.0f), 1.0f));
							pin = pin * pin * (3 - 2 * pin);
						}
						const float weight = fade * pin;
						if (weight > 0) {
							const float uvStep = std::max(std::hypot(U[1][0] - U[0][0], U[1][1] - U[0][1]), std::max(std::hypot(U[2][0] - U[1][0], U[2][1] - U[1][1]),
								std::hypot(U[0][0] - U[2][0], U[0][1] - U[2][1]))) / n;
							const float lod = std::min(std::max(std::log2(std::max(uvStep, 1e-8f)) + std::log2(float(tex[0]->size)) + sh.lodBias, 0.0f), float(tex[0]->levels - 1));
							const float h = ease(2.0f * sh.contrast * (tex[0]->between(u, v, lod, 0) - tex[0]->mean(0)));
							disp = sh.strength * weight * (h - sh.center) / scale;
						}
					}
					pt = pt + nn * disp;
				}
				out.push_back(pt * scale + offsetScene);   // world (the CPU traces in world space)
			}
			for (int b = 0; b < n; b++) {
				for (int a = 0; a + b < n; a++) {
					outIdx.push_back(base + rowStart(b) + a); outIdx.push_back(base + rowStart(b) + a + 1); outIdx.push_back(base + rowStart(b + 1) + a);
				}
				for (int a = 0; a + b + 1 < n; a++) {
					outIdx.push_back(base + rowStart(b) + a + 1); outIdx.push_back(base + rowStart(b + 1) + a + 1); outIdx.push_back(base + rowStart(b + 1) + a);
				}
			}
			(void)points;
		}
	}
}

static bool sceneTessBlas(Ctx& c) {
	using namespace TessBlas;
	Host& h = *c.host;
	// ---- the textures: two land layers (height in alpha) and the rock's height (red)
	const HTex texA = makeTex(256, [](float u, float v, float* o) {
		o[0] = 0.5f; o[1] = 0.45f; o[2] = 0.4f; o[3] = 0.45f + 0.35f * std::sin(6.2831853f * 3 * u) * std::sin(6.2831853f * 2 * v); });
	const HTex texB = makeTex(256, [](float u, float v, float* o) {
		o[0] = 0.4f; o[1] = 0.4f; o[2] = 0.45f; o[3] = 0.5f + 0.3f * std::sin(6.2831853f * (2 * u + 3 * v)) + 0.1f * std::cos(6.2831853f * 5 * u); });
	const HTex texR = makeTex(128, [](float u, float v, float* o) {
		o[0] = 0.5f + 0.4f * std::sin(6.2831853f * 2 * u) * std::cos(6.2831853f * 2 * v); o[1] = o[2] = 0.5f; o[3] = 1.0f; });
	const uint32_t idA = 3901, idB = 3902, idR = 3903;
	if (!h.StageTexture(idA, 37u, 256, 256, uint32_t(texA.levels), texA.packed()) || !h.StageTexture(idB, 37u, 256, 256, uint32_t(texB.levels), texB.packed()) ||
		!h.StageTexture(idR, 37u, 128, 128, uint32_t(texR.levels), texR.packed())) return false;

	// ---- the land: a shared grid (as the game's strips), rolling, 32 x 48 cells of 64, uv = x / 1024; two layers blended along x
	Src land;
	const int GX = 32, GY = 48;
	for (int y = 0; y <= GY; y++) for (int x = 0; x <= GX; x++) {
		const float px = -1024.0f + 64.0f * x, py = -256.0f + 64.0f * y;
		land.pos.push_back({ px, py, 25.0f * std::sin(px / 300.0f) * std::cos(py / 450.0f) });
		land.uv.push_back(px / 1024.0f); land.uv.push_back(py / 1024.0f);
		const float s = std::min(std::max((px + 512.0f) / 1536.0f, 0.0f), 1.0f);
		const float wl[7] = { 1.0f - s, s, 0, 0, 0, 0, 0 };
		land.weights.insert(land.weights.end(), wl, wl + 7);
	}
	for (int y = 0; y < GY; y++) for (int x = 0; x < GX; x++) {
		const uint32_t a = y * (GX + 1) + x, b = a + 1, cc = a + GX + 1, d = cc + 1;
		const uint32_t q[6] = { a, b, d, a, d, cc };
		land.idx.insert(land.idx.end(), q, q + 6);
	}
	land.normals(true);
	// ---- the rock: a box, edges pinned, scaled 1.25, sitting on the land
	Src rock;
	{ Mesh bx; bx.box({ -120, -120, 0 }, { 120, 120, 240 });
	  rock.pos = bx.pos; rock.uv = bx.uv; for (uint16_t i : bx.idx) rock.idx.push_back(i); }
	rock.normals(false);
	const V3 landOffset = { 100, 0, 0 };
	const V3 rockOffset = { -250, 700, -20 };
	const float rockScale = 1.25f;

	// ---- stage: the land (positions + uv) and its weights as two DATA ops (the weight elements staged as positions, as RtCore does)
	uint32_t landId = 0, dataA = 0, dataB = 0, rockId = 0;
	{
		const UINT stride = 52, vcount = UINT(land.pos.size());   // pos 0, uv 12, weights0 20 (float4), weights1 36 (float4)
		IDirect3DVertexBuffer9* vb = nullptr; IDirect3DIndexBuffer9* ib = nullptr;
		CHECKHR(h.dev->CreateVertexBuffer(stride * vcount, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr), "vb");
		{ float* v; vb->Lock(0, 0, (void**)&v, 0);
		  for (UINT i = 0; i < vcount; i++) { float* o = v + i * 13; o[0] = land.pos[i].x; o[1] = land.pos[i].y; o[2] = land.pos[i].z; o[3] = land.uv[2 * i]; o[4] = land.uv[2 * i + 1];
		    for (int k = 0; k < 8; k++) o[5 + k] = k < 7 ? land.weights[7 * i + k] : 0.0f; }
		  vb->Unlock(); }
		std::vector<uint16_t> idx16(land.idx.begin(), land.idx.end());
		CHECKHR(h.dev->CreateIndexBuffer(UINT(idx16.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr), "ib");
		{ void* p; ib->Lock(0, 0, &p, 0); std::memcpy(p, idx16.data(), idx16.size() * 2); ib->Unlock(); }
		NVR_EXT_RT_RANGE r = { NVR_EXT_RT_TRIANGLE_LIST, 0, UINT(idx16.size() / 3), 0 };
		auto stageOne = [&](UINT offset, NVR_EXT_RT_POSITION_FORMAT fmt, UINT flags, uint32_t op, uint32_t opFlags, uint32_t& id) -> bool {
			NVR_EXT_RT_GEOMETRY_DESC desc = { vb, offset, stride, vcount, fmt, ib, 1, &r, flags, "tessblas" };
			NVR_EXT_RT_GEOMETRY_INFO gi = {};
			CHECKHR(h.rt->MeasureGeometry(&desc, &gi), "MeasureGeometry");
			if (h.arenaTexel + gi.Texels + 1 > h.arenaW * h.arenaH) { say("  error: arena full\n"); return false; }
			CHECKHR(h.rt->StageGeometry(&desc, h.images[0].shared, h.arenaTexel, &gi), "StageGeometry");
			id = h.nextGeometry++;
			NvrRtGeometryOp o = {}; o.op = op; o.id = id; o.vertexCount = gi.VertexCount; o.triangleCount = gi.TriangleCount; o.arenaTexel = h.arenaTexel; o.flags = opFlags;
			h.ops.push_back(o);
			h.arenaTexel += gi.Texels;
			return true;
		};
		if (!stageOne(0, NVR_EXT_RT_POSITION_FLOAT3, NVR_EXT_RT_TEXCOORD_FLAGS(12, false), NVR_RT_OP_BUILD, NVR_RT_OP_FLAG_TEXCOORDS, landId) ||
			!stageOne(20, NVR_EXT_RT_POSITION_FLOAT4, NVR_EXT_RT_TEXCOORD_FLAGS(8, false), NVR_RT_OP_DATA, NVR_RT_OP_FLAG_TEXCOORDS, dataA) ||
			!stageOne(36, NVR_EXT_RT_POSITION_FLOAT4, 0u, NVR_RT_OP_DATA, 0u, dataB)) return false;
		Geometry g; g.id = landId; g.vb = vb; g.ib = ib; g.vertices = vcount; g.triangles = UINT(idx16.size() / 3);
		h.staged.push_back(g);
		h.live[landId] = g; h.live[dataA] = g; h.live[dataB] = g;
		Mesh bx; bx.box({ -120, -120, 0 }, { 120, 120, 240 });
		if (!h.Stage(bx, false, rockId)) return false;
	}
	// ---- the sources (layer textures, weights) and the settings
	{
		NvrRtDisplaceSourceOp s = {}; s.op = NVR_RT_OP_DISPLACE_SOURCE; s.id = landId; s.weightsA = dataA; s.weightsB = dataB; s.normals = 0;
		const uint32_t t[7] = { idA, idB, 0, 0, 0, 0, 0 };
		NvrRtDisplacePack(s.packed, t, 2, 0x3, NVR_RT_DISPLACE_KIND_LAND);
		NvrRtGeometryOp g; std::memcpy(&g, &s, 32); h.ops.push_back(g);
		NvrRtDisplaceSourceOp o = {}; o.op = NVR_RT_OP_DISPLACE_SOURCE; o.id = rockId;
		const uint32_t tr[7] = { idR, 0, 0, 0, 0, 0, 0 };
		NvrRtDisplacePack(o.packed, tr, 1, 0x1, NVR_RT_DISPLACE_KIND_OBJECT);
		std::memcpy(&g, &o, 32); h.ops.push_back(g);
	}
	const Shape landShape = { 20.0f, 0.5f, 4.0f, 48.0f, 0.0f, 2048.0f, 1024.0f, 2048.0f, 0.0f };
	const Shape rockShape = { 16.0f, 0.5f, 2.0f, 0.0f, 0.0f, 0.0f, 1024.0f, 2048.0f, 1.5f };
	const float spacing = 16.0f, maxSplit = 8.0f, refresh = 32.0f;
	auto params = [&](bool on, V3 fwd) {
		auto put = [&](uint32_t block, float a, float b, float cc, float d, float e, float f) {
			NvrRtDisplaceParamsOp p = { NVR_RT_OP_DISPLACE_PARAMS, block, { a, b, cc, d, e, f } };
			NvrRtGeometryOp g; std::memcpy(&g, &p, 32); h.ops.push_back(g);
		};
		put(NVR_RT_DISPLACE_LAND_SHAPE, landShape.strength, landShape.center, landShape.contrast, landShape.bump, landShape.lodBias, landShape.grid);
		put(NVR_RT_DISPLACE_LAND_REACH, landShape.fadeStart, landShape.fadeEnd, spacing, maxSplit, refresh, on ? 1.0f : 0.0f);
		put(NVR_RT_DISPLACE_OBJ_SHAPE, rockShape.strength, rockShape.center, rockShape.contrast, rockShape.edgeFade, rockShape.lodBias, 0.0f);
		put(NVR_RT_DISPLACE_OBJ_REACH, rockShape.fadeStart, rockShape.fadeEnd, spacing, maxSplit, refresh, on ? 1.0f : 0.0f);
		put(NVR_RT_DISPLACE_CAMERA, fwd.x, fwd.y, fwd.z, 2000000.0f, 262144.0f, 0.0f);
	};

	World w;   // for the instances (the host's Exchange); the CPU traces its own soups
	w.meshes.resize(2);
	Object lo; lo.mesh = 0; lo.land = true; lo.offset = landOffset; lo.albedo = { 0.5f, 0.45f, 0.4f }; lo.id = 1;
	lo.instFlags = NVR_RT_INSTANCE_DISPLACE | (0x1234u << NVR_RT_INSTANCE_KEY_SHIFT);
	Object ro; ro.mesh = 1; ro.offset = rockOffset; ro.scale = rockScale; ro.albedo = { 0.6f, 0.55f, 0.5f }; ro.id = 2;
	ro.instFlags = NVR_RT_INSTANCE_DISPLACE | (0x2345u << NVR_RT_INSTANCE_KEY_SHIFT);
	w.objects = { lo, ro };
	const std::vector<uint32_t> ids = { landId, rockId };
	const V3 sun = normalize({ 0.55f, 0.25f, 0.32f });   // low: the bumps throw shadows

	// The splits the helper chooses (spacing / longest edge, capped), for the CPU's copy
	const int nLand = std::min(int(std::ceil(land.maxEdge / spacing)), int(maxSplit));
	const int nRock = std::min(int(std::ceil(rock.maxEdge * rockScale / spacing)), int(maxSplit));
	const HTex* landTex[2] = { &texA, &texB };
	const HTex* rockTex[2] = { &texR, &texR };
	auto soupFor = [&](V3 eye, V3 fwd, int nl, int nr, bool flat, Soup& s) {
		s = Soup();
		std::vector<V3> pts; std::vector<uint32_t> ix;
		if (flat) {
			for (const V3& q : land.pos) pts.push_back(q + landOffset);
			s.add(pts, land.idx, true);
			pts.clear();
			for (const V3& q : rock.pos) pts.push_back(q * rockScale + rockOffset);
			s.add(pts, rock.idx, false);
		} else {
			displace(land, true, nl, landShape, landTex, 0x3, eye - landOffset, fwd, 1.0f, landOffset, pts, ix);
			s.add(pts, ix, true);
			displace(rock, false, nr, rockShape, rockTex, 0x1, eye, fwd, rockScale, rockOffset - eye, pts, ix);
			for (V3& q : pts) q = q + eye;   // (the rock's soup was made in scene space: back to world)
			s.add(pts, ix, false);
		}
		s.build();
	};

	// One frame of rays at camera `cam`: the GPU's answer vs the CPU's on the truth / flat / fine soups
	struct Stats { int rays = 0, checked = 0, tOk = 0, sunOk = 0, flatOk = 0, landHits = 0, rockHits = 0, shadowed = 0, shown = 0, rockFlatOk = 0, staleOk = 0; double fineErr = 0; int fineN = 0, fineOk = 0; };
	auto frame = [&](const Camera& cam, bool on, const Soup& truth, const Soup& flat, const Soup* fine, bool expectFlat, Stats& st, const char* label, const Soup* stale = nullptr) -> bool {
		std::vector<float> dir(S * S * 2, 0.0f), ray(S * S * 2, 0.0f);
		struct Ref { bool valid = false; V3 start, r; float viewZ = 0, tMin = 0; };
		std::vector<Ref> refs(S * S);
		const Soup& eyeSoup = expectFlat ? flat : truth;   // what the screen (the raster's depth) shows
		auto setup = [&](float px, float py, Ref& rf) {
			const V3 d = cam.ray(px / S, py / S);
			const Soup::Hit ch = eyeSoup.trace(cam.eye, d, 1e-3f, 1e9f);
			if (!ch.hit || ch.t > 3500.0f) return false;
			rf.viewZ = 0.6f * ch.t;
			const V3 v = normalize(d);
			rf.r = normalize(v + V3(0, 0, -0.35f));
			const V3 surface = d * rf.viewZ;
			const V3 half = rf.r - v;
			rf.start = surface + half * ((0.5f + rf.viewZ * 0.002f) / length(half));
			rf.tMin = 1.0f + rf.viewZ * 0.0015f;
			rf.valid = true;
			return true;
		};
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			Ref& rf = refs[j * S + i];
			if (!setup(i + 0.5f, j + 0.5f, rf)) continue;
			octEncode(rf.r, dir[(j * S + i) * 2], dir[(j * S + i) * 2 + 1]);
			ray[(j * S + i) * 2] = rf.viewZ; ray[(j * S + i) * 2 + 1] = 2.0f;
		}
		if (!h.Upload(c.refDir, dir) || !h.Upload(c.refRay, ray)) return false;
		params(on, cam.f);
		ReflConstants k = {};
		cameraConstants(cam, k.size, k.forward, k.right, k.up, k.origin);
		k.size[2] = 10000.0f; k.size[3] = 1.0f; k.forward[3] = 0.0f;
		put4(k.sun, sun, 1.0f);
		put4(k.layout, 1.0f / S, 1.0f / S, 1.0f / S, 1.0f / S);
		h.AddPass(c.shRefl, { c.refDir, c.refRay, c.refHit, c.refShade }, 0xC, &k, sizeof(k), (S + 7) / 8, (S + 7) / 8);
		if (!h.Exchange(w, ids, cam.eye)) return false;
		std::vector<float> hit, shade;
		if (!h.Read(c.refHit, hit) || !h.Read(c.refShade, shade)) return false;
		h.EndFrame();
		// the shader's ray on a soup: first hit (a land hit right at the start taken as the ground itself: once more past it), sun at the hit
		auto cpu = [&](const Soup& s, const Ref& rf, float& t, float& sunVis, bool& isLand) {
			const V3 o = cam.eye + rf.start;
			Soup::Hit hh = s.trace(o, rf.r, rf.tMin, 10000.0f);
			if (hh.hit && hh.land && hh.t < 40.0f + rf.viewZ * 0.01f) hh = s.trace(o, rf.r, hh.t + 1.0f, 10000.0f);
			t = hh.hit ? hh.t : 0.0f; isLand = hh.land; sunVis = 1.0f;
			if (hh.hit && dot(hh.n, sun) > 0) sunVis = s.trace(o + rf.r * hh.t + hh.n * std::max(1.0f, hh.t * 0.001f), sun, 0.5f, 30000.0f).hit ? 0.0f : 1.0f;
			else if (hh.hit) sunVis = 1.0f;
		};
		for (UINT j = 0; j < S; j++) for (UINT i = 0; i < S; i++) {
			const Ref& rf = refs[j * S + i];
			if (!rf.valid) continue;
			st.rays++;
			float t, sv; bool isLand;
			cpu(expectFlat ? flat : truth, rf, t, sv, isLand);
			// stable only: the same answer a quarter pixel either way (silhouettes and shadow edges are not checked)
			bool stable = true;
			for (int s2 = 0; s2 < 2 && stable; s2++) {
				Ref rj; const float off = s2 == 0 ? -0.25f : 0.25f;
				if (!setup(i + 0.5f + off, j + 0.5f + off, rj)) { stable = false; break; }
				float tj, svj; bool lj;
				cpu(expectFlat ? flat : truth, rj, tj, svj, lj);
				if ((tj > 0) != (t > 0) || std::fabs(tj - t) > 0.05f * t + 4.0f || svj != sv) stable = false;
			}
			if (!stable) continue;
			st.checked++;
			const float* H = &hit[(j * S + i) * 4]; const float* C = &shade[(j * S + i) * 4];
			const float tol = 0.75f + 0.002f * t;
			const bool tOk = t > 0 ? std::fabs(H[0] - t) <= tol : H[0] == 0.0f;
			if (tOk) st.tOk++;
			if (t > 0 && C[3] == sv) st.sunOk++;
			else if (t == 0) st.sunOk++;
			if (t > 0) { if (isLand) st.landHits++; else st.rockHits++; if (sv == 0.0f) st.shadowed++; }
			if (!tOk && st.shown++ < 4) say("    %s pixel (%u, %u): GPU t %.2f, CPU t %.2f (%s), sun GPU %.0f CPU %.0f\n", label, i, j, H[0], t, isLand ? "land" : "rock", C[3], sv);
			// the control: the same rays on the other geometry (flat when the truth is displaced)
			float tf, svf; bool lf;
			cpu(expectFlat ? truth : flat, rf, tf, svf, lf);
			if (tf > 0 ? std::fabs(H[0] - tf) <= tol : H[0] == 0.0f) { st.flatOk++; if (t > 0 && !isLand) st.rockFlatOk++; }
			if (stale) {   // the copies as they were at the last camera (no refresh): would they still agree?
				float ts, svs; bool ls;
				cpu(*stale, rf, ts, svs, ls);
				if ((ts > 0 ? std::fabs(H[0] - ts) <= tol : H[0] == 0.0f) && svs == C[3]) st.staleOk++;
			}
			if (fine && t > 0) {   // the RT's split against a much finer one of the same formula (information)
				float tq, svq; bool lq;
				cpu(*fine, rf, tq, svq, lq);
				if (tq > 0) { st.fineErr += std::fabs(tq - t); st.fineN++; if (std::fabs(tq - t) <= tol) st.fineOk++; }
			}
		}
		return true;
	};

	bool ok = true;
	// frame 0: the geometry, textures, sources and settings arrive; the helper measures the sources (no rays yet)
	const Camera camA = Camera::lookDir({ 0, -200, 260 }, normalize({ 0.05f, 1, -0.38f }), { 0, 0, 1 }, 1.3f, 1.3f);
	params(true, camA.f);
	if (!h.Exchange(w, ids, camA.eye) || !h.Finish()) return false;
	h.EndFrame();
	Soup flatSoup, truthA, fineA;
	soupFor(camA.eye, camA.f, nLand, nRock, true, flatSoup);
	soupFor(camA.eye, camA.f, nLand, nRock, false, truthA);
	soupFor(camA.eye, camA.f, 16, 16, false, fineA);
	say("  land %zu triangles, longest edge %.1f -> N %d (%zu copy triangles); rock %zu triangles x %.2f, longest %.1f -> N %d\n", land.idx.size() / 3, land.maxEdge,
		nLand, land.idx.size() / 3 * nLand * nLand, rock.idx.size() / 3, rockScale, rock.maxEdge * rockScale, nRock);
	// frame 1: the copies are built; rays at camera A
	Stats a;
	if (!frame(camA, true, truthA, flatSoup, &fineA, false, a, "A") || !h.Finish()) return false;
	// frame 2: the camera moved 300 units (the copies are redone and refit: the fade moved with it)
	const Camera camB = Camera::lookDir({ 60, 100, 240 }, normalize({ -0.05f, 1, -0.36f }), { 0, 0, 1 }, 1.3f, 1.3f);
	Soup truthB;
	soupFor(camB.eye, camB.f, nLand, nRock, false, truthB);
	Stats b;
	if (!frame(camB, true, truthB, flatSoup, nullptr, false, b, "B", &truthA) || !h.Finish()) return false;
	// frame 3: displacement off: the flat geometry again
	Stats f;
	if (!frame(camB, false, truthB, flatSoup, nullptr, true, f, "off") || !h.Finish()) return false;

	auto pct = [](int x, int n) { return n ? 100.0 * x / n : 0.0; };
	say("  camera A: %d rays, %d checked (%d on land, %d on the rock, %d of them in the sun's shadow): distance = truth %.2f %%, sun = truth %.2f %%; "
		"the FLAT geometry would agree on %.2f %% (on the rock %.2f %%)\n", a.rays, a.checked, a.landHits, a.rockHits, a.shadowed, pct(a.tOk, a.checked), pct(a.sunOk, a.checked), pct(a.flatOk, a.checked),
		pct(a.rockFlatOk, a.rockHits));
	say("  camera A: the RT's split (land N %d, rock N %d) vs a 16 x 16 split of the same formula: mean |dt| %.2f units, %.2f %% within the tolerance\n",
		nLand, nRock, a.fineN ? a.fineErr / a.fineN : 0.0, pct(a.fineOk, a.fineN));
	say("  camera B (moved 300 units, copies refit): %d checked, distance = truth %.2f %%, sun %.2f %%, flat would agree %.2f %%, the copies of camera A (not refit) %.2f %%\n", b.checked, pct(b.tOk, b.checked),
		pct(b.sunOk, b.checked), pct(b.flatOk, b.checked), pct(b.staleOk, b.checked));
	say("  displacement off: %d checked, distance = the flat geometry's %.2f %%, sun %.2f %% (the displaced truth would agree %.2f %%)\n", f.checked, pct(f.tOk, f.checked),
		pct(f.sunOk, f.checked), pct(f.flatOk, f.checked));
	ok = a.checked > 4000 && pct(a.tOk, a.checked) >= 99.0 && pct(a.sunOk, a.checked) >= 98.5 && pct(a.flatOk, a.checked) < 80.0 && a.rockHits > 100 && pct(a.rockFlatOk, a.rockHits) < 95.0 && a.shadowed > 50 &&
		b.checked > 4000 && pct(b.tOk, b.checked) >= 99.0 && pct(b.sunOk, b.checked) >= 98.5 && pct(b.flatOk, b.checked) < 80.0 && pct(b.staleOk, b.checked) < 99.0 &&
		f.checked > 4000 && pct(f.tOk, f.checked) >= 99.5 && pct(f.sunOk, f.checked) >= 99.0;
	// release: the geometry, the data, and the displacement off for the scenes after this one
	h.Release(landId); h.Release(dataA); h.Release(dataB); h.Release(rockId);
	params(false, camB.f);
	if (!h.Exchange(World(), {}, V3()) || !h.Finish()) return false;
	h.EndFrame();
	return ok;
}
