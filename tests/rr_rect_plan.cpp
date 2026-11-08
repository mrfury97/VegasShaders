// UNOFFICIAL 2026-11-07 (perf-rr) CPU check of DLSS Ray Reconstruction's plan at RenderScale < 1 (src/core/RrRectPlan.h: what
// TAAFSR.cpp RayReconstruct sends and what DlssRRComposite.frag reads). No GPU, no game. Build and run (x86 or x64 Developer prompt):
//   cl /nologo /EHsc /O2 /std:c++17 /I src tests\rr_rect_plan.cpp /Fe:%TEMP%\rr_rect_plan.exe && %TEMP%\rr_rect_plan.exe
// Exit code 0 = every check passed. Prints the planned sizes for the usual screens and RenderScale presets.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/RrRectPlan.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// The helper's DLSS quality mode for render / display (tools/nvr-dlss-helper/NvrDlssHelper.cpp QualityFor, mirrored for the table only)
static const char* QualityName(unsigned render, unsigned display) {
	const float r = display ? (float)render / (float)display : 1.0f;
	if (r >= 0.99f) return "DLAA";
	if (r >= 0.66f) return "Quality";
	if (r >= 0.57f) return "Balanced";
	if (r >= 0.49f) return "Performance";
	return "Ultra Performance";
}

int main() {
	const unsigned screens[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 } };
	const float scales[] = { 0.5f, 0.58f, 0.67f, 0.77f, 1.0f };
	std::printf("screen       scale  render       pixels  jitter steps  DLSS-D mode        guides groups\n");
	for (const auto& s : screens) for (float scale : scales) {
		const unsigned sw = s[0], sh = s[1];
		const unsigned rw = RrRect::SnapRenderSize(sw, scale), rh = RrRect::SnapRenderSize(sh, scale);
		const float phases = RrRect::JitterPhases(sw, rw);
		const bool rect = scale < 0.999f;
		// a rect frame's plan (Up's jitter 0.3, -0.2 render pixels) and a native frame's (TAA's 0.1, 0.4)
		const RrRect::Plan p = RrRect::Make(rect, sw, sh, rw, rh, true, 0.3f, -0.2f, phases, true, 0.1f, 0.4f, 8.0f);
		std::printf("%4ux%-4u    %.2f   %4ux%-4u    %5.1f%%  %5.0f         %-17s  %ux%u\n", sw, sh, scale, p.renderW, p.renderH,
			100.0 * p.renderW * p.renderH / ((double)sw * sh), p.jitterPhases, QualityName(p.renderW, p.displayW), p.groupsX, p.groupsY);

		// sizes: a multiple of 8 below 1 (UpscaleBegin's rule), the screen at 1; inside the inputs; inputs = output = the screen
		if (rect) CHECK(p.renderW % 8 == 0 && p.renderH % 8 == 0, "%ux%u at %.2f: render %ux%u not a multiple of 8", sw, sh, scale, p.renderW, p.renderH);
		else CHECK(p.renderW == sw && p.renderH == sh, "native render size is not the screen");
		CHECK(p.inputW == sw && p.inputH == sh && p.displayW == sw && p.displayH == sh, "inputs / output not the screen's size");
		CHECK(RrRect::Problem(p) == nullptr, "%ux%u at %.2f: the helper would refuse: %s", sw, sh, scale, RrRect::Problem(p));
		CHECK(std::fabs((float)p.renderW / sw - scale) < 0.01f || !rect, "render width %u far from scale %.2f", p.renderW, scale);
		// jitter: RenderScale's in a rect frame, TAA's at native, both with FSR's sign (-x, +y); phases 8 x (display / render)^2, >= 8
		if (rect) { CHECK(p.jitterX == -0.3f && p.jitterY == -0.2f, "rect jitter %.2f %.2f, want -0.30 -0.20", p.jitterX, p.jitterY);
			CHECK(p.jitterPhases == phases && phases >= 8.0f, "rect jitter phases"); }
		else { CHECK(p.jitterX == -0.1f && p.jitterY == 0.4f, "native jitter %.2f %.2f, want -0.10 0.40", p.jitterX, p.jitterY);
			CHECK(p.jitterPhases == 8.0f, "native jitter phases %.0f", p.jitterPhases); }
		// motion (uv of the render picture) to render pixels
		CHECK(p.mvScaleX == (float)p.renderW && p.mvScaleY == (float)p.renderH, "motion scale %.0f %.0f", p.mvScaleX, p.mvScaleY);
		// the guide / prep dispatch: 8 x 8 groups cover the render rect and no more than 7 pixels past it; with the shaders' early-out
		// (pixel >= size) exactly the rect is written, nothing outside it
		CHECK(p.groupsX * 8 >= p.renderW && (p.groupsX - 1) * 8 < p.renderW && p.groupsY * 8 >= p.renderH && (p.groupsY - 1) * 8 < p.renderH, "groups");
		{
			unsigned long long written = 0, outside = 0;
			for (unsigned gy = 0; gy < p.groupsY * 8; gy++) for (unsigned gx = 0; gx < p.groupsX * 8; gx++) {
				if (gx >= p.renderW || gy >= p.renderH) continue;	// DlssRRGuides / FSRPrep: if (pixel >= size) return
				written++;
				if (gx >= sw || gy >= sh) outside++;
			}
			CHECK(written == (unsigned long long)p.renderW * p.renderH && outside == 0, "guides wrote %llu (outside %llu)", written, outside);
		}
		// the guides' view ray: render pixel q at uv (q + 0.5) / render size spans the whole frustum: its first and last pixel centres sit
		// half a render pixel inside the frustum's edges (the full-screen picture's edges), as at native size
		{
			const float u0 = 0.5f / p.renderW, u1 = (p.renderW - 0.5f) / p.renderW;
			CHECK(std::fabs(u0 - (1.0f - u1)) < 1e-6f && u0 > 0.0f && u1 < 1.0f, "view ray uv");
		}
		// the composite's mapping: every screen pixel reads a rect texel inside the rect, at the same uv as the screen pixel (same view ray),
		// monotonic, and every rect texel is read by some screen pixel (nothing of the rect dropped)
		{
			std::vector<unsigned char> hitX(p.renderW, 0), hitY(p.renderH, 0);
			int lastX = -1, lastY = -1;
			float worstUv = 0.0f;
			for (unsigned x = 0; x < sw; x++) {
				int rx, ry;
				RrRect::NearestRectPixel(p, x, 0, rx, ry);
				CHECK(rx >= 0 && rx < (int)p.renderW, "x %u -> rect %d outside", x, rx);
				CHECK(rx >= lastX, "x mapping not monotonic at %u", x);
				lastX = rx;
				hitX[rx] = 1;
				float tx, ty;
				RrRect::RectPoint(p, x, 0, tx, ty);
				worstUv = std::fmax(worstUv, std::fabs((tx + 0.5f) / p.renderW - (x + 0.5f) / sw));
			}
			for (unsigned y = 0; y < sh; y++) {
				int rx, ry;
				RrRect::NearestRectPixel(p, 0, y, rx, ry);
				CHECK(ry >= 0 && ry < (int)p.renderH, "y %u -> rect %d outside", y, ry);
				CHECK(ry >= lastY, "y mapping not monotonic at %u", y);
				lastY = ry;
				hitY[ry] = 1;
			}
			unsigned missedX = 0, missedY = 0;
			for (unsigned char h : hitX) missedX += h ? 0 : 1;
			for (unsigned char h : hitY) missedY += h ? 0 : 1;
			CHECK(missedX == 0 && missedY == 0, "%ux%u at %.2f: %u rect columns / %u rows never read by the composite", sw, sh, scale, missedX, missedY);
			CHECK(worstUv < 2e-6f, "composite uv off by %g", worstUv);
			if (!rect) {	// native: pixel for pixel
				int rx, ry;
				RrRect::NearestRectPixel(p, sw / 3, sh / 3, rx, ry);
				CHECK(rx == (int)(sw / 3) && ry == (int)(sh / 3), "native composite is not pixel for pixel");
			}
		}
	}
	// an empty / oversized render rect is refused (what the helper's SubrectProblem refuses)
	{
		RrRect::Plan bad = RrRect::Make(true, 1920, 1080, 0, 0, false, 0, 0, 8, false, 0, 0, 8);
		CHECK(RrRect::Problem(bad) != nullptr, "an empty render rect passed");
		bad = RrRect::Make(true, 1920, 1080, 2000, 1080, false, 0, 0, 8, false, 0, 0, 8);
		CHECK(RrRect::Problem(bad) != nullptr, "a render rect past the inputs passed");
		const RrRect::Plan still = RrRect::Make(true, 1920, 1080, 1280, 720, false, 0.3f, 0.3f, 18, true, 0.2f, 0.2f, 8);
		CHECK(still.jitterX == 0.0f && still.jitterY == 0.0f, "an unjittered rect frame got jitter");
	}
	std::printf("\nRR RECT PLAN: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
