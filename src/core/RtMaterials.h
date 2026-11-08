#pragma once
// UNOFFICIAL 2026-11-07 (ptaaa-materials; docs/next/RT-PATH-TRACING.md "materials") the path tracer's material models' keys, in the lamp
// image's FRAME ROW (core\RtPathTrace.cpp, RtPathTrace.comp MatKeys): texels 24-26, two floats a texel. Plain C++, no game types: shared with
// tools/nvr-rt-host (scene `materials`), which packs its rows with it.
//   texel 24  PTGlass (0 off, 1 glass for the light: shadow and bounce rays, 2 the camera's rays too), PTGlassTint (0-1)
//   texel 25  PTFoliage (thin leaves' transmission, 0 off), PTSkin (skin's scattering, 0 off)
//   texel 26  PTClothSheen (0 off), unused
// The kinds themselves ride in the instance records (NvrRtProtocol.h NVR_RT_RECORD_KIND_*) and, at the first hit, in the MaterialBuffer's
// class (3 cloth, 5 plants, 6 skin).

namespace RtMaterials {
	constexpr int kTexel = 24;	// RtPathTrace.comp MatKeys: FrameTexel(24) .. (26)
	struct Keys {
		float glass = 2.0f;			// PTGlass
		float glassTint = 0.5f;		// PTGlassTint: how much of the glass texture's hue tints the light through it (its darkness never does)
		float foliage = 0.4f;		// PTFoliage: the share of a leaf's diffuse light that goes through it
		float skin = 1.0f;			// PTSkin: skin's wrap (soft terminator, red widest) and its light through thin parts (x 0.25)
		float sheen = 0.5f;			// PTClothSheen
	};
	inline void PackRow(float* row, const Keys& k) {
		row[2 * kTexel + 0] = k.glass;   row[2 * kTexel + 1] = k.glassTint;
		row[2 * kTexel + 2] = k.foliage; row[2 * kTexel + 3] = k.skin;
		row[2 * kTexel + 4] = k.sheen;   row[2 * kTexel + 5] = 0.0f;
	}
	inline bool Any(const Keys& k) { return k.glass > 0.0f || k.foliage > 0.0f || k.skin > 0.0f || k.sheen > 0.0f; }
}
