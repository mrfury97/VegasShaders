#pragma once
// UNOFFICIAL 2026-11-07 (rtgap-tess; docs/next/RT-DETAIL-GAPS.md section 1) the RT passes' values for the detail the RT scene lacks but the
// depth buffer has. Plain C++, no game types: the game (RtPathTrace.cpp, RtSunShadows.cpp, RtLampShadows.h, AmbientOcclusion.cpp) and
// tools/nvr-rt-host (scenes `tessslack`, `contact`) fill the shaders' constants with it. The shaders' side: Compute/Rt/RtContact.glsl.
//  (1a) Slack: with the tessellated bumps on (DxvkCompute::DisplaceRtSlack) the RT passes start their rays the Strength further off the
//       surface (faded out by view depth as the bumps are), and the path tracer's same-surface test grows by it.
//  (1b) Contact: a screen-space march over the depth buffer toward the light, multiplied into the ray's answer.
// Every pass takes three vec4s: slack (x Strength, y FadeStart, z FadeEnd), contact (x length in pixels of the pass's depth image, y the
// longest in units, z thickness, w strength; 0 = off) and gate (xy: the march fades out between these view depths, y <= x no fade; z
// thickness per unit of view depth). The path tracer's block is full: they ride in its lamp image's frame row, texels 16-21 (RtRestirRow.h).

namespace RtTessContact {
	struct Slack { float strength = 0.0f, fadeStart = 0.0f, fadeEnd = 0.0f; };
	struct Contact {
		float pixels = 0.0f, longest = 0.0f, thickness = 0.0f, strength = 0.0f;	// strength 0: off
		float gateStart = 0.0f, gateEnd = 0.0f, thicknessPerUnit = 0.0f;
	};

	// The defaults every pass uses (the settings give strength and length): a march of at most 64 units, 16 units thick + 2 % of the view
	// depth (the march inside a prop 10-20 units high still counts; a wall far in front of the march does not)
	constexpr float kLongest = 64.0f, kThickness = 16.0f, kThicknessPerUnit = 0.02f;
	inline Contact Make(float strength, float pixels) {
		Contact c;
		c.strength = strength > 0.0f ? (strength < 1.0f ? strength : 1.0f) : 0.0f;
		c.pixels = pixels > 0.0f ? (pixels < 128.0f ? pixels : 128.0f) : 0.0f;
		c.longest = kLongest; c.thickness = kThickness; c.thicknessPerUnit = kThicknessPerUnit;
		if (!(c.pixels > 0.0f)) c.strength = 0.0f;
		return c;
	}
	// The raster twin draws contact shadows from `start` to `end` (smoothstep, view depth: SunShadows.fx ContactNearFade, its start pushed
	// out to the bumps' FadeEnd by DisplaceContactFade): the RT march fades out over the same band, so the two add up to one, never two
	inline void GateBy(Contact& c, float start, float end) { c.gateStart = start; c.gateEnd = end > start ? end : start + 1.0f; }

	inline void SlackVec(const Slack& s, float out[4]) {
		out[0] = s.strength > 0.0f ? s.strength : 0.0f; out[1] = s.fadeStart; out[2] = s.fadeEnd > s.fadeStart ? s.fadeEnd : s.fadeStart + 1.0f; out[3] = 0.0f;
	}
	inline void ContactVecs(const Contact& c, float contact[4], float gate[4]) {
		contact[0] = c.pixels; contact[1] = c.longest; contact[2] = c.thickness; contact[3] = c.strength;
		gate[0] = c.gateStart; gate[1] = c.gateEnd; gate[2] = c.thicknessPerUnit; gate[3] = 0.0f;
	}

	// The path tracer's frame row (two floats a texel): texels 16-17 the slack, 18-19 the contact, 20-21 the gate (RtPathTrace.comp PtSlack /
	// PtContact). `row` is the whole row (RtRestirRow::Build's; texels 16-31 are free there)
	constexpr int kRowTexel = 16;
	inline void PackRow(float* row, const Slack& s, const Contact& c) {
		float* t = row + 2 * kRowTexel;
		SlackVec(s, t);
		ContactVecs(c, t + 4, t + 8);
	}
}
