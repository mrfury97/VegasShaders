#pragma once
// UNOFFICIAL 2026-11-07 (perf-mirrorfix) MaterialAware reflections: what each pass of the world (and first-person) scene does to the
// scene's alpha, where the material lives (core/ReflMaterial.h: 2 + 0.25 x metal + 0.24 x roughness, [2, 3) = a material; anything
// else = none). Plain C++ (no D3D, no game types): ReflMaterial.h applies it per pass, tests/refl_material_rule.cpp checks it on a
// simulated pixel.
//
// The bug it fixes (tester, VSRTRoom mirror wall, 2026-11-07): the game draws the world without alpha writes (COLORWRITEENABLE 7), and
// only the material passes added alpha. Every other draw left the pixel's alpha as it found it, so whatever was drawn IN FRONT of a
// mirror panel (a person's shirt, a fence post, the hair) kept the panel's material and the Reflections effect turned it into a mirror
// (chrome shirt, the room painted on a fence post). Now every pass that replaces the pixel's colour writes its own alpha too:
//   Material  the object's first pass with an _rmaos map (world scene only): c212.x = 1, its material (as before);
//   Own       any other pass the colour of which is not blended by its alpha property: alpha writes on, the shader's own alpha (the game's
//             0..1, never a material) replaces what was there. Lamp and highlight passes the device blends anyway keep the material
//             under them: the scene's separate alpha blend (ZERO x source + ONE x destination) is set for the whole scene;
//   Cover     a blended first pass of a skinned object (hair cards, a fading person): the material under it is scaled by (1 - its alpha)
//             (separate alpha blend ZERO / INVSRCALPHA for that draw): solid hair clears it, its see-through parts keep it;
//   Keep      everything else: blended static draws (glass, decals, particles) leave the material under them as before (a mirror seen
//             through a window still reflects); a pass with no colour writes (depth only); separate alpha blending off (never seen).
#include <stdint.h>

namespace ReflMaterialRule {
	enum Write { Keep = 0, Material = 1, Own = 2, Cover = 3 };

	struct Pass {
		bool		live = false;			// the scene writes materials now (MaterialAware on; the world scene or first person)
		bool		world = true;			// the world scene (material passes allowed); false: the first-person scene
		bool		hasGeometry = false;
		bool		hasMap = false;			// an _rmaos map (RmaosDraw::ThisDraw)
		bool		firstKind = true;		// PassSums::KindOf == None (not a lamp / highlight run)
		bool		refused = false;		// on ReflMaterial::Refused (drew blended with the flag once)
		bool		blendProperty = false;	// NiAlphaProperty blend
		bool		testProperty = false;	// NiAlphaProperty alpha test
		bool		skinned = false;		// the geometry has a skin instance
		bool		separateAlpha = true;	// the device's SEPARATEALPHABLENDENABLE (the scene's ZERO / ONE alpha blend)
		uint32_t	colorWrite = 7;			// the device's COLORWRITEENABLE before the pass
	};

	inline Write Decide(const Pass& p) {
		if (!p.live || !p.hasGeometry) return Keep;
		if (p.world && p.hasMap && p.firstKind && !p.refused && !p.blendProperty && !p.testProperty) return Material;
		if (!p.separateAlpha || !(p.colorWrite & 7u)) return Keep;
		if (!p.blendProperty) return Own;
		if (p.skinned && p.firstKind && !p.refused) return Cover;
		return Keep;
	}

	// The scene's alpha after the pass drew over a pixel (its depth and alpha tests passed). dst: the alpha before; src: the shader's alpha
	// output; encoded: what a material pass writes; blendOn: the device blends this draw (the scene's separate alpha blend then applies).
	inline float After(Write w, float dst, float src, float encoded, bool blendOn) {
		switch (w) {
		case Material: return blendOn ? dst : encoded;
		case Own: return blendOn ? dst : src;
		case Cover: return dst * (1.0f - (src < 0.0f ? 0.0f : src > 1.0f ? 1.0f : src));
		default: return dst;
		}
	}

	// What the material copy reads as a material (Reflections.fx.hlsl MaterialCapturePS, Compute/ReflectionsMaterial.frag)
	inline bool IsMaterial(float a) { return a >= 2.0f && a < 3.0f; }

	// The material a pass writes (Includes/Object.hlsl ReflMaterialAlpha): metal 0..3 steps, roughness 0..1
	inline float Encode(int metalSteps, float roughness) { return 2.0f + 0.25f * float(metalSteps) + 0.24f * roughness; }
}
