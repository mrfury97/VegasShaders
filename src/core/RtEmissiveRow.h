#pragma once
// UNOFFICIAL 2026-11-07 (ptaaa-emissive; docs/next/RT-PATH-TRACING.md, section "ptaaa-emissive") PTEmissive: glowing surfaces (glow maps:
// neon, lit windows, lamp glass) as real lights in the path tracer, sampled per TRIANGLE by their emitted power, as RTXDI samples emissive
// triangles. Plain C++, no game types: shared by core\RtPathTrace.cpp and tools/nvr-rt-host (scene `emissive`).
//
// The game's side picks the frame's emissive instances (RtCore::GlowLights(): every instance whose draw has a glow map) and tells the GPU
// three things:
//   1. the record's bits 8-15 (free on records that are not alpha-tested) = the instance's emissive number + 1 (RtScenePick::WithEmissive),
//      so a bounce ray that meets a glowing triangle knows its light (the MIS weight needs the light's density there);
//   2. the lamp image's EMISSIVE ROW (row kRow = 50, after the frame row), one entry of kEntryFloats floats a number e:
//        [0..11]  the instance's transform, 3 rows of 4 (camera relative, as the scene's instances)
//        [12]     its triangle count, [13] its geometry id (exact in a float: ids < 2^24)
//        [14]     the REMAP for ReSTIR DI: 1 + this frame's number of LAST frame's emissive instance e (0: gone), matched by geometry id
//                 and world position (the reservoirs name an emissive sample by number, triangle and barycentrics)
//        [15]     its index in the frame record (the build pass looks for its record at or below it: the helper drops instances whose
//                 geometry is not built, never reorders them)
//   3. the frame row's texel kFrameTexel: (how many, the scratch store word where the list starts / 4096).
// RtEmissiveBuild.comp (a pass before the trace, one workgroup an instance) finds each instance's record, reads its triangles (the
// geometry data) and its glow (the real glow map, or the glow atlas: rtHitGlow), and writes into the scratch store at that word:
//   kMax blocks of kBlockWords words: [0] record + 1 (0: not usable this frame), [1] total power, [2..4] bound centre (camera relative),
//   [5] radius, [6] triangles, [7] first triangle's place in the list, [8..19] the transform; then the list: kTrianglesMax floats, each
//   triangle's running sum of power (luminance of its mean glow x its area) within its instance (the CDF the trace picks triangles by).
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

namespace RtEmissiveRow {
	constexpr int		kRow = 50;				// the lamp image's row (RtPathTrace.cpp kEmissiveRow; RtPathTrace.comp / RtEmissiveBuild.comp)
	constexpr int		kMax = 128;				// emissive instances a frame (bits 8-15 hold number + 1)
	constexpr int		kEntryFloats = 16;		// 8 RG32F texels an instance
	constexpr int		kRowFloats = kMax * kEntryFloats;	// 1024 texels (the lamp image is 1152 wide)
	constexpr uint32_t	kTrianglesMax = 65536;	// the list's room
	constexpr uint32_t	kBlockWords = 32;
	constexpr uint32_t	kScratchWords = kMax * kBlockWords + kTrianglesMax;
	constexpr int		kFrameTexel = 22;		// the frame row's texel: (count, the list's first scratch word / 4096)

	struct Source {
		uint32_t	record = 0;			// the instance's index in the frame record
		uint32_t	geometryId = 0;
		uint32_t	triangles = 0;
		float		transform[3][4] = {};	// camera relative
		float		center[3] = {}, radius = 0.0f;	// its world bound, camera relative
		float		rgb[3] = {};		// a guess of its colour (the record's average): only which instances are kept when there are more than kMax
	};
	struct Key { uint32_t geometryId = 0; double world[3] = {}; };
	struct State { int count = 0; Key keys[kMax]; };
	struct Mark { uint32_t record, number; };	// the record's bits 8-15 = number (emissive index + 1)

	// Picks at most maxCount sources (by guess x r^2 / max(d^2, r^2): near and big first) whose triangles fit triangleBudget in all, fills `row`
	// (kRowFloats floats: zeroed here) and `marks`, writes the remap of last frame's numbers, and makes this frame "last frame". cam: the
	// camera's world position (the transforms' origin). Returns how many.
	inline int Build(State& st, const Source* src, int n, int maxCount, uint32_t triangleBudget, const double cam[3], float* row, std::vector<Mark>& marks) {
		std::memset(row, 0, sizeof(float) * kRowFloats);
		marks.clear();
		maxCount = (std::min)((std::max)(maxCount, 0), kMax);
		triangleBudget = (std::min)(triangleBudget, kTrianglesMax);
		struct Pick { int i; float weight; };
		std::vector<Pick> picks;
		picks.reserve((size_t)(std::max)(n, 0));
		for (int i = 0; i < n; i++) {
			const Source& s = src[i];
			if (!s.triangles || s.triangles > triangleBudget || !(s.radius >= 0.0f)) continue;
			const float luma = 0.2126f * s.rgb[0] + 0.7152f * s.rgb[1] + 0.0722f * s.rgb[2] + 0.05f;
			const float r = (std::max)(s.radius, 1.0f), d2 = s.center[0] * s.center[0] + s.center[1] * s.center[1] + s.center[2] * s.center[2];
			picks.push_back({ i, luma * r * r / (std::max)(d2, r * r) });
		}
		std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.weight > b.weight; });
		Key keys[kMax];
		int count = 0;
		uint32_t used = 0;
		for (const Pick& pk : picks) {
			if (count >= maxCount) break;
			const Source& s = src[pk.i];
			if (used + s.triangles > triangleBudget) continue;
			used += s.triangles;
			float* e = row + count * kEntryFloats;
			for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) e[r * 4 + c] = s.transform[r][c];
			e[12] = (float)s.triangles;
			e[13] = (float)(s.geometryId & 0xFFFFFFu);
			e[15] = (float)s.record;
			keys[count].geometryId = s.geometryId;
			for (int a = 0; a < 3; a++) keys[count].world[a] = (double)s.transform[a][3] + cam[a];
			marks.push_back({ s.record, (uint32_t)count + 1u });
			count++;
		}
		// the remap: last frame's entry j -> this frame's (same geometry within half a unit of where it was)
		for (int j = 0; j < st.count; j++) {
			float to = 0.0f;
			for (int k = 0; k < count; k++) {
				if (keys[k].geometryId != st.keys[j].geometryId) continue;
				if (std::fabs(keys[k].world[0] - st.keys[j].world[0]) > 0.5 || std::fabs(keys[k].world[1] - st.keys[j].world[1]) > 0.5 ||
					std::fabs(keys[k].world[2] - st.keys[j].world[2]) > 0.5) continue;
				to = (float)(k + 1);
				break;
			}
			row[j * kEntryFloats + 14] = to;
		}
		st.count = count;
		for (int k = 0; k < count; k++) st.keys[k] = keys[k];
		return count;
	}
}
