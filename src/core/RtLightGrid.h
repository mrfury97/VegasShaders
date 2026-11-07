#pragma once

// UNOFFICIAL 2026-11-04 PTLightGrid's builder (moved here from core\RtPathTrace.cpp BuildGrid on 2026-11-06, unchanged, so that
// tools/nvr-rt-host builds the grid with the game's own code and checks RtPathTraceGrid.spv against the all-lamp RtPathTrace.spv).
// Plain C++ only: no game, D3D or NVR headers.
//
// A world grid over the lamps' spheres. Each cell lists, in rising lamp order, every lamp whose sphere meets the cell's box: a lamp
// left out has weight 0 at every point of the cell anyway (dist >= reach), and the trace's reservoir draws a random number only for
// lamps of weight > 0, so the pick (and the picture) is the same as the loop over every lamp, bit for bit. Writes the cells (one texel
// each: first entry, count) then the lists (two lamp numbers a texel) into `out` (gridTexels float pairs, zeroed by the caller) and
// the pass constants' grid[3][4] (RtPathTrace.comp p.grid); false when the lists do not fit (the trace then loops over every lamp).

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace RtLightGrid {
	constexpr int kGridXY = 32, kGridZ = 16;

	struct Info { int cells = 0, entries = 0; float cell[3] = {}; int dims[3] = {}; };

	// lamps: `count` lamps, each `stride` floats apart, starting with x, y, z, reach (scene space); lightTexels: the lamp image's width
	inline bool Build(const float* lamps, int count, int stride, float* out, int gridTexels, int lightTexels, float grid[3][4], Info& info) {
		info = Info();
		auto pos = [&](int i, int a) { return lamps[(size_t)i * stride + a]; };
		float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (int i = 0; i < count; i++)
			for (int a = 0; a < 3; a++) { lo[a] = (std::min)(lo[a], pos(i, a) - pos(i, 3)); hi[a] = (std::max)(hi[a], pos(i, a) + pos(i, 3)); }
		int dims[3];
		float cell[3];
		for (int a = 0; a < 3; a++) {
			const float extent = (std::max)(hi[a] - lo[a], 1.0f);
			const int most = a == 2 ? kGridZ : kGridXY;
			dims[a] = std::clamp((int)ceilf(extent / 64.0f), 1, most);
			cell[a] = extent / dims[a];	// (the cells span the bounds exactly)
		}
		const int cells = dims[0] * dims[1] * dims[2];
		std::vector<int> number(cells, 0), start;
		// the sphere against the cell's box (the box's nearest point within reach, with float slack: never leave out a lamp that reaches it)
		auto touches = [&](int i, const int idx[3]) {
			float d2 = 0.0f;
			for (int a = 0; a < 3; a++) {
				const float b0 = lo[a] + idx[a] * cell[a], b1 = b0 + cell[a], p = pos(i, a);
				const float d = p < b0 ? b0 - p : p > b1 ? p - b1 : 0.0f;
				d2 += d * d;
			}
			const float r = pos(i, 3) * 1.001f + 0.01f;
			return d2 <= r * r;
		};
		int entries = 0;
		for (int pass = 0; pass < 2; pass++) {
			if (pass == 1) {
				if (cells + (entries + 1) / 2 > gridTexels) { info.cells = cells; info.entries = entries; return false; }
				start.assign(cells, 0);
				for (int k = 0, sum = 0; k < cells; k++) { start[k] = sum; sum += number[k]; }
				for (int k = 0; k < cells; k++) { out[2 * k] = (float)start[k]; out[2 * k + 1] = (float)number[k]; }
				number.assign(cells, 0);
			}
			for (int i = 0; i < count; i++) {	// (rising lamp order: each list keeps the all-lamp loop's order)
				int r0[3], r1[3];
				for (int a = 0; a < 3; a++) {
					r0[a] = std::clamp((int)floorf((pos(i, a) - pos(i, 3) - lo[a]) / cell[a]) - 1, 0, dims[a] - 1);
					r1[a] = std::clamp((int)floorf((pos(i, a) + pos(i, 3) - lo[a]) / cell[a]) + 1, 0, dims[a] - 1);
				}
				int idx[3];
				for (idx[2] = r0[2]; idx[2] <= r1[2]; idx[2]++)
					for (idx[1] = r0[1]; idx[1] <= r1[1]; idx[1]++)
						for (idx[0] = r0[0]; idx[0] <= r1[0]; idx[0]++) {
							if (!touches(i, idx)) continue;
							const int k = (idx[2] * dims[1] + idx[1]) * dims[0] + idx[0];
							if (pass == 0) { number[k]++; entries++; }
							else out[2 * cells + start[k] + number[k]++] = (float)i;	// (entry e: float 2 x cells + e = texel cells + e / 2, channel e % 2)
						}
			}
		}
		grid[0][0] = lo[0]; grid[0][1] = lo[1]; grid[0][2] = lo[2]; grid[0][3] = (float)dims[0];
		grid[1][0] = 1.0f / cell[0]; grid[1][1] = 1.0f / cell[1]; grid[1][2] = 1.0f / cell[2]; grid[1][3] = (float)dims[1];
		grid[2][0] = (float)dims[2]; grid[2][1] = (float)lightTexels;
		info.cells = cells; info.entries = entries;
		for (int a = 0; a < 3; a++) { info.cell[a] = cell[a]; info.dims[a] = dims[a]; }
		return true;
	}
}
