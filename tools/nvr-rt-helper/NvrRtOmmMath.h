// NvrRtOmmMath.h -- UNOFFICIAL 2026-11-07 (ptaaa-omm, protocol 9) the CPU half of the opacity micromaps (NvrRtOmm.inl). MIT licence
// (tools/nvr-rt-helper/LICENSE). Shared by the helper and the RT test host (scene `omm` tests these against brute force).
//
//   OmmCurveIndex   the micro-triangle a hit's barycentrics fall in: the Vulkan spec's reference code (appendix VK_EXT_opacity_micromap,
//                   "BarycentricsToSpaceFillingCurveIndex"), as is. RtOmm.comp / RtOmmProbe.comp carry the same lines in GLSL.
//   OmmThresholds   the alpha a micro-triangle's texels must all clear for it to be marked opaque, from the record's reference: the
//                   largest float at or below the highest threshold RtAlpha.glsl's rtAlphaPass can compare with (any dither, any
//                   rounding of its float maths), so the builder's `alpha > threshold` in float means "every ray passes it".
//   OmmLevel        the subdivision level a geometry gets (one level for all its triangles: the build's usage counts are known up front).
#pragma once

#include <stdint.h>
#include <cmath>
#include <algorithm>

namespace OmmMath {

	inline uint32_t OmmCurveIndex(float u, float v, uint32_t level) {
		u = std::min(std::max(u, 0.0f), 1.0f);
		v = std::min(std::max(v, 0.0f), 1.0f);
		uint32_t iu, iv, iw;
		const float fu = u * float(1u << level);
		const float fv = v * float(1u << level);
		iu = (uint32_t)fu;
		iv = (uint32_t)fv;
		const float uf = fu - float(iu);
		const float vf = fv - float(iv);
		if (iu >= (1u << level)) iu = (1u << level) - 1u;
		if (iv >= (1u << level)) iv = (1u << level) - 1u;
		const uint32_t iuv = iu + iv;
		if (iuv >= (1u << level)) iu -= iuv - (1u << level) + 1u;
		iw = ~(iu + iv);
		if (uf + vf >= 1.0f && iuv < (1u << level) - 1u) --iw;
		uint32_t b0 = ~(iu ^ iw);
		b0 &= ((1u << level) - 1u);
		const uint32_t t = (iu ^ iv) & b0;
		uint32_t f = t;
		f ^= f >> 1u;
		f ^= f >> 2u;
		f ^= f >> 4u;
		f ^= f >> 8u;
		uint32_t b1 = ((f ^ iu) & ~b0) | t;
		b0 = (b0 | (b0 << 8u)) & 0x00ff00ffu;
		b0 = (b0 | (b0 << 4u)) & 0x0f0f0f0fu;
		b0 = (b0 | (b0 << 2u)) & 0x33333333u;
		b0 = (b0 | (b0 << 1u)) & 0x55555555u;
		b1 = (b1 | (b1 << 8u)) & 0x00ff00ffu;
		b1 = (b1 | (b1 << 4u)) & 0x0f0f0f0fu;
		b1 = (b1 | (b1 << 2u)) & 0x33333333u;
		b1 = (b1 | (b1 << 1u)) & 0x55555555u;
		return b0 | (b1 << 1u);
	}

	// The micro-triangle (iu, iv, flipped) of a level, as RtOmm.comp enumerates them: upright (iu, iv) (iu + 1, iv) (iu, iv + 1) for
	// iu + iv <= N - 1, flipped (iu + 1, iv) (iu, iv + 1) (iu + 1, iv + 1) for iu + iv <= N - 2 (barycentrics u, v = corner / N)
	inline uint32_t OmmIndexOfCell(uint32_t iu, uint32_t iv, bool flipped, uint32_t level) {
		const float n = float(1u << level);
		const float u = flipped ? (float(iu) + 2.0f / 3.0f) / n : (float(iu) + 1.0f / 3.0f) / n;
		const float v = flipped ? (float(iv) + 2.0f / 3.0f) / n : (float(iv) + 1.0f / 3.0f) / n;
		return OmmCurveIndex(u, v, level);
	}

	inline float FloorToFloat(double x) {   // the largest float at or below x
		float f = (float)x;
		if ((double)f > x) f = std::nextafter(f, -INFINITY);
		return f;
	}

	// RtAlpha.glsl: ref = float(r) / 255.0 (OpFDiv: within 2.5 ulp in Vulkan); width = min(1, 2 min(ref, 1 - ref)); the hashed test is
	// alpha > ref + dither * width with dither = float(k) / 2^24 - 0.5, k < 2^24, so dither <= 0.5 - 2^-24 (exact in float). The plain test
	// (real textures at lod < 0.5, RT_ALPHA_PLAIN) is alpha > ref. Bounds on what the GPU's float maths gives:
	//   ref >= 0.5: T = ref + d 2 (1 - ref) = 2 d + ref (1 - 2 d): the slope in ref is 2^-23 (ref's own error vanishes); the product's and
	//               the sum's rounding (round to nearest, fused or not) add at most 2^-26 + 2^-25 < 4.5e-8
	//   ref < 0.5:  T = ref (1 + 2 d): slope ~2, ref's 2.5 ulp (< 7.5e-8) doubled + the roundings < 2e-7
	//   ref = 0:    T = 0 exactly (0 / 255 = 0, width 0); ref = 255: never opaque (alpha > 1 - tiny is not knowable from 8-bit alpha)
	//   plain:      ref + 2.5 ulp (< 1.5e-7)
	inline void OmmThresholds(uint32_t ref8, float& dither, float& plain) {
		if (ref8 >= 255u) { dither = plain = 2.0f; return; }
		if (ref8 == 0u) { dither = plain = 0.0f; return; }
		const double ref = ref8 / 255.0;
		const double width = std::min(1.0, 2.0 * std::min(ref, 1.0 - ref));
		const double dmax = 16777215.0 / 16777216.0 - 0.5;
		const double t = ref + dmax * width;
		dither = FloorToFloat(t + (ref < 0.5 ? 2.0e-7 : 4.5e-8));
		plain = FloorToFloat(ref + 2.0e-7);
	}

	// One level for every triangle of a geometry: as fine as kMicroPerGeometry micro-triangles allow, at most `maxLevel` (the device's
	// maxOpacity4StateSubdivisionLevel, capped). 0: too many triangles for a level-1 micromap (the geometry keeps the shader's test).
	inline uint32_t OmmLevel(uint64_t triangles, uint32_t maxLevel, uint64_t microPerGeometry) {
		uint32_t level = 0;
		while (level < maxLevel && triangles * (1ull << (2 * (level + 1))) <= microPerGeometry) level++;
		return level;
	}

	inline uint32_t OmmBytesPerTriangle(uint32_t level) { return level ? 1u << (2 * (level - 1)) : 1u; }   // 4^level states, 2 bits each
}
