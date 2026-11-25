#pragma once

// UNOFFICIAL 2026-11-19 (Finding 4.1) shared linear-light unit helpers for the RT path tracer, matching forward / deferred PBR
// (effects/PBR/PBR.cpp, Includes/LinearLight.hlsl, Includes/Object.hlsl, core/ManyLamps.h):
//   - Gamma()       active [Shaders.PBR.Linear] decode exponent (TESR_LinearLighting.x, >= 1.0; 2.2 fallback when PBR Linear is off)
//   - LightScale()  LIN_SCALE_LIGHT   (TESR_LinearScales.x, fallback 1.0)
//   - AmbientScale() LIN_SCALE_AMBIENT (TESR_LinearScales.y, fallback 1.0)
//   - SkyScale()    LIN_SCALE_SKY     (TESR_LinearScales.z, fallback 1.0)
//   - DecodeColor() pow(max(rgb, 0), Gamma()) * scale
//   - DecodeLamp()  pow(max(col.rgb * col.w, 0), Gamma()) * scale  (diffuse * dimmer decoded jointly, as ManyLamps / Object.hlsl)

#include "../effects/PBR/PBR.h"
#include <algorithm>
#include <cmath>

namespace RtLightUnits {

	inline PBRShaders* Pbr() {
		return TheShaderManager ? TheShaderManager->Shaders.PBR : nullptr;
	}

	inline float Gamma() {
		PBRShaders* pbr = Pbr();
		if (!pbr || !pbr->LinearSettings.Enabled) return 2.2f;
		const float live = pbr->Constants.Linear.x;
		const float g = live > 1.0001f ? live : pbr->LinearSettings.Gamma;
		return (g >= 1.0f && g < 10.0f) ? g : 2.2f;
	}

	inline float ValidScale(float s) {
		return (s >= 0.0f && s < 1.0e10f) ? s : 1.0f;
	}

	inline float LightScale() {
		PBRShaders* pbr = Pbr();
		return pbr ? ValidScale(pbr->Constants.LinearScales.x) : 1.0f;
	}

	inline float AmbientScale() {
		PBRShaders* pbr = Pbr();
		return pbr ? ValidScale(pbr->Constants.LinearScales.y) : 1.0f;
	}

	inline float SkyScale() {
		PBRShaders* pbr = Pbr();
		return pbr ? ValidScale(pbr->Constants.LinearScales.z) : 1.0f;
	}

	inline D3DXVECTOR3 DecodeColor(float r, float g, float b, float scale = 1.0f) {
		const float exp = Gamma();
		return D3DXVECTOR3(
			powf((std::max)(r, 0.0f), exp) * scale,
			powf((std::max)(g, 0.0f), exp) * scale,
			powf((std::max)(b, 0.0f), exp) * scale);
	}

	inline void DecodeColor(float out[3], float r, float g, float b, float scale = 1.0f) {
		const D3DXVECTOR3 v = DecodeColor(r, g, b, scale);
		out[0] = v.x; out[1] = v.y; out[2] = v.z;
	}

	inline D3DXVECTOR3 DecodeLamp(const D3DXVECTOR4& colAndDimmer, float scale = 1.0f) {
		const float exp = Gamma();
		const float w = (std::max)(colAndDimmer.w, 0.0f);
		return D3DXVECTOR3(
			powf((std::max)(colAndDimmer.x * w, 0.0f), exp) * scale,
			powf((std::max)(colAndDimmer.y * w, 0.0f), exp) * scale,
			powf((std::max)(colAndDimmer.z * w, 0.0f), exp) * scale);
	}

}
