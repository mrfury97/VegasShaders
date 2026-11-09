#pragma once
// UNOFFICIAL 2026-11-07 (perf-reflfix2) the path tracer's flashlight code (RtPathTrace.cpp Lamps; plain C++, shared with the RT test host's
// ptflashlight scene): the flashlight is lamp 0, its cone in one float: 1 + (angle x 512 + u) x 512 + v, the cone's half-angle in whole
// degrees (1..63) and its axis octahedral in 9 bits each (an exact integer up to 2^24); 0 = a plain lamp. RtPathTrace.comp LampSpot reads it
// from the frame row's texel 15 (the lamp image) or lampColor[0].w: full light inside half the cone, none outside it, smooth between.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RtFlashlightCode {
	// axis: the beam's direction (any length > 1e-3, scene / world axes); cosOuter: cos of the cone's half-angle. 0 when the axis is degenerate.
	inline float Encode(float ax, float ay, float az, float cosOuter) {
		const float len = std::sqrt(ax * ax + ay * ay + az * az);
		if (!(len > 1.0e-3f)) return 0.0f;
		const float dx = ax / len, dy = ay / len, dz = az / len;
		const float n1 = std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
		float ox = dx / n1, oy = dy / n1;
		if (dz < 0.0f) { const float tx = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f), ty = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f); ox = tx; oy = ty; }
		const uint32_t qu = (uint32_t)(std::min)((std::max)((ox * 0.5f + 0.5f) * 511.0f + 0.5f, 0.0f), 511.0f);
		const uint32_t qv = (uint32_t)(std::min)((std::max)((oy * 0.5f + 0.5f) * 511.0f + 0.5f, 0.0f), 511.0f);
		const float degrees = std::acos((std::min)((std::max)(cosOuter, -1.0f), 1.0f)) * 57.2957795f;
		const uint32_t angle = (uint32_t)(std::min)((std::max)(degrees + 0.5f, 1.0f), 63.0f);
		return (float)(1u + (angle * 512u + qu) * 512u + qv);
	}
	// The CPU's reading of a code, as LampSpot (the host's truth): the share of the light reaching the unit direction t (lamp -> point)
	inline float Spot(float code, float tx, float ty, float tz) {
		if (code < 0.5f) return 1.0f;
		const uint32_t c = (uint32_t)(code - 1.0f + 0.5f);
		const float ex = float((c >> 9u) & 511u) / 511.0f * 2.0f - 1.0f, ey = float(c & 511u) / 511.0f * 2.0f - 1.0f;
		float dx = ex, dy = ey, dz = 1.0f - std::fabs(ex) - std::fabs(ey);
		if (dz < 0.0f) { const float a = (1.0f - std::fabs(ey)) * (ex >= 0.0f ? 1.0f : -1.0f), b = (1.0f - std::fabs(ex)) * (ey >= 0.0f ? 1.0f : -1.0f); dx = a; dy = b; }
		const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float angle = float(c >> 18u) * 0.0174532925f;
		const float lo = std::cos(angle), hi = std::cos(0.5f * angle), x = (dx * tx + dy * ty + dz * tz) / l;
		const float s = (std::min)((std::max)((x - lo) / (hi - lo), 0.0f), 1.0f);
		return s * s * (3.0f - 2.0f * s);
	}
}
