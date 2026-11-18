// The gun's real shape for GunFX's volumetric smoke: a map of how far every point around the gun is from its surface
// (a signed distance field), built once from the gun model's own triangles, in the model's own space. The smoke looks
// itself up in it to flow round the actual gun (its sights, magazine, scope, bipod) instead of a plain tube.
// Standalone (no game code), so the same code is tested offline (work\gunshape_test.cpp).
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>

struct GunShape {
	float lo[3] = {};          // the grid's corner (model space)
	float voxel = 0.5f;        // grid spacing (model units)
	int nx = 0, ny = 0, nz = 0;
	std::vector<float> d;      // signed distance to the gun's surface at each grid point (negative inside)

	bool Ready() const { return !d.empty(); }

	// Builds the map from `count` triangles (9 floats each: three corners). The grid covers the triangles plus `margin`
	// on every side, `voxelSize` apart (coarser if it would exceed `maxPoints` grid points). `shell`: how far out from the
	// drawn surface counts as solid. Cracks narrower than about one grid step are sealed (models are rarely closed), so
	// smoke treats a gun's enclosed body as solid. Returns false if there is nothing to build.
	bool Build(const float* tris, int count, float voxelSize, float margin, float shell, int maxPoints);

	// The signed distance at a point (model space) and the outward direction there (unit length; zero where unknown).
	// Beyond the grid, the distance grows on with the distance to the grid, pointing away from it.
	float Sample(const float* p, float* normal) const;
};

namespace GunShapeDetail {
	inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	inline void Sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
	inline void Mad(const float* a, const float* b, float k, float* o) { o[0] = a[0] + b[0] * k; o[1] = a[1] + b[1] * k; o[2] = a[2] + b[2] * k; }
	inline void Copy(const float* a, float* o) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }

	// The point of triangle abc closest to p (Ericson, Real-Time Collision Detection 5.1.5). The triangle must have area.
	inline void ClosestOnTriangle(const float* p, const float* a, const float* b, const float* c, float* out) {
		float ab[3], ac[3], ap[3], bp[3], cp[3];
		Sub(b, a, ab); Sub(c, a, ac); Sub(p, a, ap);
		const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
		if (d1 <= 0.0f && d2 <= 0.0f) { Copy(a, out); return; }
		Sub(p, b, bp);
		const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
		if (d3 >= 0.0f && d4 <= d3) { Copy(b, out); return; }
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { const float den = d1 - d3; Mad(a, ab, den > 1e-20f ? d1 / den : 0.0f, out); return; }
		Sub(p, c, cp);
		const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
		if (d6 >= 0.0f && d5 <= d6) { Copy(c, out); return; }
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { const float den = d2 - d6; Mad(a, ac, den > 1e-20f ? d2 / den : 0.0f, out); return; }
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
			const float den = (d4 - d3) + (d5 - d6);
			float bc[3]; Sub(c, b, bc);
			Mad(b, bc, den > 1e-20f ? (d4 - d3) / den : 0.0f, out);
			return;
		}
		const float sum = va + vb + vc;
		const float inv = sum > 1e-30f ? 1.0f / sum : 0.0f;
		out[0] = a[0] + ab[0] * vb * inv + ac[0] * vc * inv;
		out[1] = a[1] + ab[1] * vb * inv + ac[1] * vc * inv;
		out[2] = a[2] + ab[2] * vb * inv + ac[2] * vc * inv;
	}
}

inline bool GunShape::Build(const float* tris, int count, float voxelSize, float margin, float shell, int maxPoints) {
	using namespace GunShapeDetail;
	d.clear();
	nx = ny = nz = 0;
	if (!tris || count <= 0) return false;
	float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
	for (int i = 0; i < count * 3; i++)
		for (int k = 0; k < 3; k++) {
			const float v = tris[i * 3 + k];
			if (!(v > -1e6f && v < 1e6f)) return false;              // not a sane model
			mn[k] = v < mn[k] ? v : mn[k];
			mx[k] = v > mx[k] ? v : mx[k];
		}
	voxel = voxelSize > 0.05f ? voxelSize : 0.05f;
	for (int tries = 0;; tries++) {
		nx = (int)ceilf((mx[0] - mn[0] + 2.0f * margin) / voxel) + 1;
		ny = (int)ceilf((mx[1] - mn[1] + 2.0f * margin) / voxel) + 1;
		nz = (int)ceilf((mx[2] - mn[2] + 2.0f * margin) / voxel) + 1;
		if ((double)nx * ny * nz <= maxPoints || tries > 40) break;
		voxel *= 1.15f;
	}
	if ((double)nx * ny * nz > maxPoints) return false;
	for (int k = 0; k < 3; k++) lo[k] = mn[k] - margin;
	const int sx = 1, sy = nx, sz = nx * ny;
	const size_t N = (size_t)nx * ny * nz;
	std::vector<float> dist2(N, 1e30f);          // squared distance to the nearest surface point found so far
	std::vector<float> near3(N * 3, 0.0f);       // that point
	const float inv = 1.0f / voxel;

	// 1. Exactly, within one and a half grid steps of each triangle.
	const float band = 1.5f * voxel;
	for (int t = 0; t < count; t++) {
		const float* a = tris + t * 9;
		const float* b = a + 3;
		const float* c = a + 6;
		float ab[3], ac[3], nrm[3];
		Sub(b, a, ab); Sub(c, a, ac);
		nrm[0] = ab[1] * ac[2] - ab[2] * ac[1]; nrm[1] = ab[2] * ac[0] - ab[0] * ac[2]; nrm[2] = ab[0] * ac[1] - ab[1] * ac[0];
		const float area2 = sqrtf(Dot(nrm, nrm));
		if (area2 < 1e-8f) continue;                                // no area
		nrm[0] /= area2; nrm[1] /= area2; nrm[2] /= area2;
		int i0[3], i1[3];
		for (int k = 0; k < 3; k++) {
			const float tmin = fminf(a[k], fminf(b[k], c[k])), tmax = fmaxf(a[k], fmaxf(b[k], c[k]));
			const int n = k == 0 ? nx : k == 1 ? ny : nz;
			i0[k] = (int)floorf((tmin - band - lo[k]) * inv); i1[k] = (int)ceilf((tmax + band - lo[k]) * inv);
			i0[k] = i0[k] < 0 ? 0 : i0[k]; i1[k] = i1[k] > n - 1 ? n - 1 : i1[k];
		}
		for (int z = i0[2]; z <= i1[2]; z++)
			for (int y = i0[1]; y <= i1[1]; y++)
				for (int x = i0[0]; x <= i1[0]; x++) {
					const float p[3] = { lo[0] + x * voxel, lo[1] + y * voxel, lo[2] + z * voxel };
					float ap[3]; Sub(p, a, ap);
					const float plane = Dot(ap, nrm);
					if (plane > band || plane < -band) continue;          // too far from the triangle's plane
					float q[3]; ClosestOnTriangle(p, a, b, c, q);
					float e[3]; Sub(p, q, e);
					const float dd = Dot(e, e);
					const size_t v = (size_t)x * sx + (size_t)y * sy + (size_t)z * sz;
					if (dd < dist2[v]) { dist2[v] = dd; Copy(q, &near3[v * 3]); }
				}
	}

	// 2. Outward: each grid point takes a neighbour's nearest surface point when that is nearer to it (raster passes
	// forward and backward over the 13 neighbours already visited in that order; twice).
	int offs[13][3], m = 0;
	for (int dz = -1; dz <= 1; dz++)
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++) {
				if (dz > 0 || (dz == 0 && dy > 0) || (dz == 0 && dy == 0 && dx >= 0)) continue;
				offs[m][0] = dx; offs[m][1] = dy; offs[m][2] = dz; m++;
			}
	for (int round = 0; round < 2; round++)
		for (int dir = 1; dir >= -1; dir -= 2) {
			const int zs = dir > 0 ? 0 : nz - 1, ze = dir > 0 ? nz : -1;
			const int ys = dir > 0 ? 0 : ny - 1, ye = dir > 0 ? ny : -1;
			const int xs = dir > 0 ? 0 : nx - 1, xe = dir > 0 ? nx : -1;
			for (int z = zs; z != ze; z += dir)
				for (int y = ys; y != ye; y += dir)
					for (int x = xs; x != xe; x += dir) {
						const size_t v = (size_t)x * sx + (size_t)y * sy + (size_t)z * sz;
						const float p[3] = { lo[0] + x * voxel, lo[1] + y * voxel, lo[2] + z * voxel };
						for (int o = 0; o < 13; o++) {
							const int qx = x + offs[o][0] * dir, qy = y + offs[o][1] * dir, qz = z + offs[o][2] * dir;
							if (qx < 0 || qy < 0 || qz < 0 || qx >= nx || qy >= ny || qz >= nz) continue;
							const size_t w = (size_t)qx * sx + (size_t)qy * sy + (size_t)qz * sz;
							if (dist2[w] >= 1e29f) continue;
							float e[3]; Sub(p, &near3[w * 3], e);
							const float dd = Dot(e, e);
							if (dd < dist2[v]) { dist2[v] = dd; Copy(&near3[w * 3], &near3[v * 3]); }
						}
					}
		}
	near3.clear(); near3.shrink_to_fit();

	// 3. Inside or outside: flood in from the grid's edges through every point that is not up against the surface;
	// what the flood cannot reach is enclosed by the gun (inside).
	const float wall = fmaxf(shell, 0.75f * voxel);
	const float wall2 = wall * wall;
	std::vector<uint8_t> outside(N, 0);
	std::vector<int> stack;
	stack.reserve(N / 8 + 16);
	auto seed = [&](int x, int y, int z) {
		const size_t v = (size_t)x * sx + (size_t)y * sy + (size_t)z * sz;
		if (!outside[v] && dist2[v] > wall2) { outside[v] = 1; stack.push_back((int)v); }
	};
	for (int z = 0; z < nz; z++) for (int y = 0; y < ny; y++) { seed(0, y, z); seed(nx - 1, y, z); }
	for (int z = 0; z < nz; z++) for (int x = 0; x < nx; x++) { seed(x, 0, z); seed(x, ny - 1, z); }
	for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++) { seed(x, y, 0); seed(x, y, nz - 1); }
	while (!stack.empty()) {
		const int v = stack.back();
		stack.pop_back();
		const int z = v / sz, y = (v - z * sz) / sy, x = v - z * sz - y * sy;
		if (x > 0) seed(x - 1, y, z);
		if (x < nx - 1) seed(x + 1, y, z);
		if (y > 0) seed(x, y - 1, z);
		if (y < ny - 1) seed(x, y + 1, z);
		if (z > 0) seed(x, y, z - 1);
		if (z < nz - 1) seed(x, y, z + 1);
	}

	// 4. The signed distance to the solid's skin (`shell` out from the drawn surface).
	d.resize(N);
	for (size_t v = 0; v < N; v++) {
		const float r = dist2[v] < 1e29f ? sqrtf(dist2[v]) : 1e3f;
		d[v] = (outside[v] || dist2[v] <= wall2) ? r - shell : -r - shell;
	}
	return true;
}

inline float GunShape::Sample(const float* p, float* normal) const {
	normal[0] = normal[1] = normal[2] = 0.0f;
	if (d.empty()) return 1e3f;
	const int n[3] = { nx, ny, nz };
	float g[3], ext[3], f[3];
	int i[3];
	for (int k = 0; k < 3; k++) {
		g[k] = (p[k] - lo[k]) / voxel;
		const float top = (float)(n[k] - 1) - 0.0001f;
		const float gc = g[k] < 0.0f ? 0.0f : g[k] > top ? top : g[k];
		ext[k] = (g[k] - gc) * voxel;                                   // how far beyond the grid (model units)
		i[k] = (int)gc;
		f[k] = gc - (float)i[k];
	}
	const size_t sy = (size_t)nx, sz = (size_t)nx * ny;
	const size_t b = (size_t)i[0] + i[1] * sy + i[2] * sz;
	const float c000 = d[b], c100 = d[b + 1], c010 = d[b + sy], c110 = d[b + sy + 1];
	const float c001 = d[b + sz], c101 = d[b + sz + 1], c011 = d[b + sz + sy], c111 = d[b + sz + sy + 1];
	const float fx = f[0], fy = f[1], fz = f[2];
	const float x00 = c000 + (c100 - c000) * fx, x10 = c010 + (c110 - c010) * fx;
	const float x01 = c001 + (c101 - c001) * fx, x11 = c011 + (c111 - c011) * fx;
	const float y0 = x00 + (x10 - x00) * fy, y1 = x01 + (x11 - x01) * fy;
	float value = y0 + (y1 - y0) * fz;
	float gr[3];
	gr[0] = ((c100 - c000) * (1 - fy) * (1 - fz) + (c110 - c010) * fy * (1 - fz) + (c101 - c001) * (1 - fy) * fz + (c111 - c011) * fy * fz);
	gr[1] = ((x10 - x00) * (1 - fz) + (x11 - x01) * fz);
	gr[2] = (y1 - y0);
	const float out = sqrtf(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
	if (out > 0.0f) {
		value += out;
		const float k = out > voxel ? 1.0f : out / voxel;                // away from the grid, more so the farther out
		const float gl = sqrtf(gr[0] * gr[0] + gr[1] * gr[1] + gr[2] * gr[2]);
		for (int a = 0; a < 3; a++) gr[a] = (gl > 1e-6f ? gr[a] / gl : 0.0f) * (1.0f - k) + ext[a] / out * k;
	}
	const float gl = sqrtf(gr[0] * gr[0] + gr[1] * gr[1] + gr[2] * gr[2]);
	if (gl > 1e-6f) { normal[0] = gr[0] / gl; normal[1] = gr[1] / gl; normal[2] = gr[2] / gl; }
	return value;
}
