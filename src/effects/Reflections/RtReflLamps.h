#pragma once
// UNOFFICIAL 2026-11-07 (perf-reflfix2) RT reflections' lamps as the game's shaders light them (plain C++, no game types: the RT test host's
// reflorbs scene packs the same lamps through it). Reflections.cpp RtGatherLamps fills Raw from each NiPointLight and Frame once a frame;
// Pack writes the two vec4s ReflectionsRT.glsl / Rt/ReflectionsRTBody.glsl read (Rt/RtLampLight.glsl: the falloff and N.L).
//
// The tester's "orbs" (RT test room, 1.0.6-test10): white discs on the ceiling near every lamp, only in the mirror. The estimate lit an
// out-of-view hit with diffuse x Dimmer UNCLAMPED, decoded together (the room's lamps are BasementLightKey*, FNAM fade 1.8-1.9: their
// colour x 1.9 decoded is 4.4x the game's red, 4.9x its blue), no light scale and (1 - (d / r)^2)^2. The game (ManyLamps.h BuildTable,
// the object shaders): the dimmer at most 1 without HDR, x the LOD dimmer, decoded, x LightingScale ([Shaders.PBR.*] LightScale, decoded:
// 0.725 by day outdoors and in "behave like exterior" cells = 0.49), x its falloff (1 - (d / r)^2 decoded like a colour; or
// [Shaders.PBR.LampFalloff]'s shape). The reflection works in the screen's colours decoded (Resolve linearizes the picture), so the
// game's light there is (diffuse x dimmer x LOD x LightScale)^2.2 x att^2.2 x N.L at the default gamma (any linear-lighting gamma: the
// game encodes back to the same screen value).
#include <algorithm>
#include <cmath>

namespace RtReflLamps {
	struct Raw {
		float pos[3] = { 0.0f, 0.0f, 0.0f };		// world position
		float reach = 0.0f;							// Spec.r
		float diffuse[3] = { 0.0f, 0.0f, 0.0f };	// Diff (0..1, the stored curve)
		float dimmer = 1.0f;						// Dimmer (the LIGH's fade)
		float lodDimmer = 1.0f;						// ShadowSceneLight::fLODDimmer
	};
	struct Frame {
		bool hdr = false;			// BSShaderManager::bHDR (with it the game keeps dimmers above 1)
		float lightScale = 1.0f;	// [Shaders.PBR.*] LightScale this frame, as set (PBRShaders Constants.Data.z: the stored curve)
		bool shaped = false;		// [Shaders.PBR.LampFalloff]'s physical shape this frame (Constants.LampFalloff.x)
		float shapeY = 0.0f;		// its 1 / Falloff^2 (.y)
		float shapeStrength = 1.0f;	// its strength (.z: the energy match x Brightness)
	};

	inline float Decode(float v) { return std::pow((std::max)(v, 0.0f), 2.2f); }

	// out[0..3]: camera-relative position, reach; out[4..7]: the linear colour (x the shape's strength), the falloff (0 the game's, > 0 the
	// shape's 1 / Falloff^2)
	inline void Pack(const Raw& r, const Frame& f, const float eye[3], float out[8]) {
		out[0] = r.pos[0] - eye[0]; out[1] = r.pos[1] - eye[1]; out[2] = r.pos[2] - eye[2]; out[3] = r.reach;
		const float dimmer = f.hdr ? r.dimmer : (std::min)(r.dimmer, 1.0f);
		const float scale = dimmer * r.lodDimmer * (std::max)(f.lightScale, 0.0f);
		const bool shaped = f.shaped && f.shapeY > 0.0f;
		const float strength = shaped ? f.shapeStrength : 1.0f;
		for (int k = 0; k < 3; k++) out[4 + k] = Decode(r.diffuse[k] * scale) * strength;
		out[7] = shaped ? f.shapeY : 0.0f;
	}

	// UNOFFICIAL 2026-11-07 (perf-reflfix2, the flashlight team: the beam missing in mirrors) the flashlight as FlashlightDeferred::LightParams
	// gives it (camera-relative position, reach, its colour decoded with the game's gamma x Dimmer: FlashlightDeferred.hlsl adds it in that
	// linear light) and [Shaders.Flashlight.Deferred] Falloff: out as Pack's (the colour in the reflection's linear space, x^(2.2 / gamma);
	// w = -1 - Falloff: RtLampLight.glsl's flashlight falloff). Its cone travels apart (RtFlashlightCode::Encode).
	inline void PackFlashlight(const float relPos[3], float reach, const float colour[3], float falloff, float gamma, float out[8]) {
		out[0] = relPos[0]; out[1] = relPos[1]; out[2] = relPos[2]; out[3] = reach;
		const float e = gamma > 1.0f ? 2.2f / gamma : 1.0f;
		for (int k = 0; k < 3; k++) out[4 + k] = std::pow((std::max)(colour[k], 0.0f), e);
		out[7] = -1.0f - (std::max)(falloff, 0.0f);
	}
	inline float FlashFall(float d, float reach, float falloff) {   // the CPU's twin of the flashlight branch
		const float q = (std::clamp)(d * d / (std::max)(reach * reach, 1.0e-6f), 0.0f, 1.0f);
		return falloff > 0.0f ? (1.0f - q) * (1.0f - q) / (1.0f + falloff * q) : 1.0f - q;
	}

	// The CPU's twin of Rt/RtLampLight.glsl RtLampFall (the host compares the GPU's against the game's maths written out separately)
	inline float Fall(float d, float reach, float shape) {
		const float q = (std::clamp)(d * d / (std::max)(reach * reach, 1.0e-6f), 0.0f, 1.0f);
		if (shape > 0.0f) { float w = 1.0f - q * q; w *= w; return w / (1.0f + q * shape); }
		return std::pow(1.0f - q, 2.2f);
	}
}
