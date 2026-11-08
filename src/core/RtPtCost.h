#pragma once
// UNOFFICIAL 2026-11-07 (perf-ptcost; docs/next/RT-PATH-TRACING.md "perf-ptcost") the path tracer's ray budget keys, in the lamp image's FRAME
// ROW (core\RtPathTrace.cpp, RtPathTrace.comp LoadCostKeys): texels 28-29, two floats a texel. Plain C++, no game types: shared with
// tools/nvr-rt-host (its NVR_RT_HOST_PTCOST knob). 0 in every key = the cheap path (what a row without these texels, e.g. a host scene, gets).
//   texel 28  PTRestirDIRays: 0 ReSTIR DI as before (visibility inside the targets) but no temporal rays on the same spot (last frame's point
//             within 1.5 pixels); 1 every ray as before (canonical 1-2, temporal 2, 2 per spatial neighbour); test values: 2 unshadowed
//             targets + one final ray (RTXDI's final visibility, W 0 when occluded), 3 as 0 + one ray for the canonical candidates' winner
//             + the neighbours' MIS weights unshadowed
//             PTBounceLights: 0 one light sample a bounce hit and a thin leaf's far side (the sun, the picked lamp or the emissive point, by
//             their unshadowed light; one shadow ray), 1 all three (three shadow rays, as before)
//   texel 29  1 when no instance of this frame's scene is the hidden first-person body (NVR_RT_CUSTOM_HIDDEN, RtCore::HiddenInstances): the
//             camera ray is one query; 0 the two queries (world, then cut-outs + people passing over the hidden body); unused

namespace RtPtCost {
	constexpr int kTexel = 28;	// RtPathTrace.comp LoadCostKeys: FrameTexel(28), FrameTexel(29)
	struct Keys {
		float restirRays = 0.0f;	// PTRestirDIRays
		float bounceLights = 0.0f;	// PTBounceLights
		float oneCameraQuery = 0.0f;	// 1: no hidden body this frame
	};
	inline void PackRow(float* row, const Keys& k) {
		row[2 * kTexel + 0] = k.restirRays;     row[2 * kTexel + 1] = k.bounceLights;
		row[2 * kTexel + 2] = k.oneCameraQuery; row[2 * kTexel + 3] = 0.0f;
	}
}
