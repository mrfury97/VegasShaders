#pragma once

// UNOFFICIAL 2026-11-07 (perf-rr) DLSS Ray Reconstruction at RenderScale < 1: the sizes, jitter and mappings one RR frame uses, as plain
// C++ (no game or D3D types) so tests/rr_rect_plan.cpp and the RT host's `rrrect` scene check the same numbers TAAFSR.cpp sends.
//
// A RenderScale v2 frame (core/RenderRect.h) draws the world, the first-person weapon and the pre-tonemap effects into the TOP-LEFT
// render rect of screen-size targets. The RR point there (TAAFSR.cpp RayReconstruct, rect frame):
//   - FSRPrep and DlssRRGuides run one thread per RENDER pixel (texelFetch at that pixel = the rect of every screen-size input; the view
//     ray's uv = (pixel + 0.5) / render size spans the whole frustum), writing the rect of their screen-size outputs;
//   - the helper copies the render rect of each input into its shared images at 0, 0 and gives DLSS-D the subrect 0, 0 render size
//     (NvrDlssHelper.cpp DlssSubrectFor / SubrectProblem), DLSS-D made render size -> display size (EnsureDlssd);
//   - jitter: RenderScale's own (TAAEffect::Up: Halton over Up.JitterPhases steps, in render pixels; FSR's sign: (-x, +y)), motion in uv
//     units of the render picture, scaled by the render size (UpscalerDlss.cpp Exchange: mvScale = render size);
//   - DlssRRComposite writes the whole screen: RR's output at each screen pixel; the frame before RR (alpha, the weapon) is read in the
//     rect at the screen pixel's centre x scale (nearest for the alpha and the weapon test, bilinear for the weapon's colour).
// At RenderScale 1 (a native frame) every size is the screen's and the jitter is TAA's own 8 steps, as before.

#include <cmath>

namespace RrRect {
	// UpscaleBegin's internal size (TAAUpscale.cpp, 2026-10-27 "half screen AL" fix): a multiple of 8, at least 16, the screen's at scale 1.
	inline unsigned SnapRenderSize(unsigned full, float scale) {
		if (scale >= 1.0f) return full;
		const unsigned r = (unsigned)floorf((float)full * scale / 8.0f + 0.5f) * 8u;
		return r < 16u ? 16u : (r > full ? full : r);
	}
	// ffxFsr2GetJitterPhaseCount as UpscaleBegin uses it: 8 x (display / render)^2, at least 8.
	inline float JitterPhases(unsigned display, unsigned render) {
		const int n = (int)(8.0f * powf((float)display / (float)render, 2.0f));
		return (float)(n > 8 ? n : 8);
	}

	struct Plan {
		bool		rect = false;					// a RenderScale v2 rect frame (else native)
		unsigned	renderW = 0, renderH = 0;		// what is traced / drawn and what DLSS-D's render subrect is
		unsigned	inputW = 0, inputH = 0;			// the textures' (and the helper's shared input images') size: the screen's
		unsigned	displayW = 0, displayH = 0;		// RR's output: the screen's
		float		jitterX = 0.0f, jitterY = 0.0f;	// in render pixels, FSR / DLSS convention (the sample shows the unjittered point p - jitter)
		float		jitterPhases = 8.0f;
		float		mvScaleX = 0.0f, mvScaleY = 0.0f;	// motion (uv of the render picture) to render pixels
		unsigned	groupsX = 0, groupsY = 0;		// FSRPrep / DlssRRGuides dispatch (8 x 8 threads)
		float		scaleX = 1.0f, scaleY = 1.0f;	// render / display: DlssRRComposite's rect mapping
	};

	// rect: this frame is a rect frame (RenderRect::Active() && TAAEffect::Up.Deferred). up*: TAAEffect::Up's render size and jitter;
	// native*: TAA's own jitter (frameJitterX / Y, 8 steps).
	inline Plan Make(bool rect, unsigned screenW, unsigned screenH, unsigned upRenderW, unsigned upRenderH, bool upJittered, float upJitterX,
		float upJitterY, float upPhases, bool nativeJittered, float nativeJitterX, float nativeJitterY, float nativePhases) {
		Plan p;
		p.rect = rect;
		p.inputW = p.displayW = screenW;
		p.inputH = p.displayH = screenH;
		p.renderW = rect ? upRenderW : screenW;
		p.renderH = rect ? upRenderH : screenH;
		// BeginJitter moved the frustum right / up by (x, y) pixels, the picture left / down: FSR's and DLSS's offset is (-x, +y).
		const bool jittered = rect ? upJittered : nativeJittered;
		p.jitterX = jittered ? -(rect ? upJitterX : nativeJitterX) : 0.0f;
		p.jitterY = jittered ? (rect ? upJitterY : nativeJitterY) : 0.0f;
		p.jitterPhases = rect ? upPhases : nativePhases;
		p.mvScaleX = (float)p.renderW;
		p.mvScaleY = (float)p.renderH;
		p.groupsX = (p.renderW + 7) / 8;
		p.groupsY = (p.renderH + 7) / 8;
		p.scaleX = screenW ? (float)p.renderW / (float)screenW : 1.0f;
		p.scaleY = screenH ? (float)p.renderH / (float)screenH : 1.0f;
		return p;
	}

	// What the helper would refuse (nullptr: nothing), with SubrectProblem's rules for RR's subrect 0, 0 render size on the shared images:
	// not empty, inside the inputs, no larger than the output.
	inline const char* Problem(const Plan& p) {
		if (!p.renderW || !p.renderH) return "the render size is empty";
		if (p.renderW > p.inputW || p.renderH > p.inputH) return "the render rect reaches past the input images";
		if (p.renderW > p.displayW || p.renderH > p.displayH) return "the render rect is larger than the output";
		if (p.inputW != p.displayW || p.inputH != p.displayH) return "the inputs are not the screen's size (the helper's images are)";
		return nullptr;
	}

	// DlssRRComposite.frag's mapping, mirrored: the screen pixel (x, y) reads the frame before RR at rect position
	// t = (pixel + 0.5) x scale - 0.5 (in rect pixel centres); nearest = round(t) clamped into the rect.
	inline void RectPoint(const Plan& p, unsigned x, unsigned y, float& tx, float& ty) {
		tx = ((float)x + 0.5f) * p.scaleX - 0.5f;
		ty = ((float)y + 0.5f) * p.scaleY - 0.5f;
	}
	inline void NearestRectPixel(const Plan& p, unsigned x, unsigned y, int& rx, int& ry) {
		float tx, ty;
		RectPoint(p, x, y, tx, ty);
		rx = (int)floorf(tx + 0.5f);
		ry = (int)floorf(ty + 0.5f);
		const int maxX = (int)p.renderW - 1, maxY = (int)p.renderH - 1;
		rx = rx < 0 ? 0 : (rx > maxX ? maxX : rx);
		ry = ry < 0 ? 0 : (ry > maxY ? maxY : ry);
	}
}
