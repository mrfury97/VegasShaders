#pragma once

// UNOFFICIAL 2026-10-29 (NVR DXVK) the shared RT denoiser: for one-sample-a-pixel ray traced signals (soft sun shadows, AO, sky,
// lamp shadows, reflection masks). SVGF / ReBLUR-style, on the game's GPU with NVR DXVK passes: a temporal pass (the signal blended
// with last frame's history, reprojected through the depth and last frame's camera; disocclusions restart) and 3 edge-stopping
// a-trous passes (steps 1, 2, 4; depth, view normal and variance-guided value weights). One channel (.r) or RGB (Settings.Channels).
// docs\next\FORK-RT-SUN.md "Denoiser". Use:
//   static RtDenoise::Denoiser* d = RtDenoise::Create("my effect");
//   RtDenoise::Run(d, Device, noisyTexture, outputSurface, settings);   // once a frame, after the noisy signal is there
// Run keeps its own history (2 RGBA16F screen textures + 2 for the a-trous chain); false: not run (no NVR DXVK, a pass missing):
// use the noisy signal. Camera cuts (a jump of the camera) restart the history by themselves.
// UNOFFICIAL 2026-10-30 DLSS RR (docs\next\FORK2-DLSS-RR.md): Run also returns false, without a pass, while Upscaler::
// RayReconstructionWantsRaw() is true (DLSS Ray Reconstruction denoises the raw signal); every history restarts after such frames.
// UNOFFICIAL 2026-11-07 (perf-history, RtHistoryTime.h): every length in frames below (MaxHistory, PeopleHistory, MovingHistory,
// MirrorHistory, HistoryFix, the anti-lag's fast history) means FRAMES AT 60 FPS: Run turns it into the same seconds at this frame rate
// (HistoryClock(); [Main.Main.RayTracing] DenoiseTimeBased, DenoiseMaxHistorySeconds). At 60 fps nothing changes.

#include "RtHistoryTime.h"

namespace RtDenoise {
	struct Settings {
		float	MaxHistory = 16.0f;		// frames the temporal blend averages at most (more: smoother, slower to react)
		float	DepthTolerance = 0.03f;	// relative depth mismatch that counts as a different surface (disocclusion)
		int		Iterations = 3;			// a-trous passes (steps 1, 2, 4, ...): 0 = temporal only
		float	DepthSigma = 1.0f;
		float	NormalPower = 64.0f;
		float	ValueSigma = 4.0f;
		bool	Reset = false;			// drop the history this frame
		bool	IgnoreRr = false;		// UNOFFICIAL 2026-10-30 (review): run under DLSS Ray Reconstruction too (RT lamp shadows: read by the
										// next frame's object shaders, so raw they would reach RR late and stay raw a frame after RR stops)
		int		Channels = 1;			// 1: Noisy .r -> Output .r; 3: Noisy .rgb -> Output .rgb (RGBA16F output; GI / sky colour)
		int		Downscale = 1;
		IDirect3DBaseTexture9* Ids = nullptr;
		int		Kernel = 5;				// 5: a 5 x 5 a-trous kernel (25 taps a pass), 3: 3 x 3 (9 taps; ~2.5x cheaper, a bit less smooth)	// optional: .r = a source id per pixel (e.g. which lamp); the spatial passes only mix equal ids			// 1 full screen, 2 half (Noisy and Output must then be w/2 x h/2; ~4x cheaper; the caller upsamples)
		float	HistoryClamp = 0.0f;	// UNOFFICIAL 2026-11-06 (RGB mode): the history clamped to the new frame's 3 x 3 mean +- this many deviations (0 off; PTDenoiseClamp)
		float	PeopleHistory = 0.0f;	// UNOFFICIAL 2026-11-07 (RGB mode): pixels of people (the skin templates' draws: skin, skinned armour and clothes;
										// TESR_CapturedNormalsBuffer alpha > 1.5) keep at most this many frames of history (0 = no cap). A talking
										// face changes shape with no motion the reprojection knows of (lip-sync, expressions): 32 frames of the
										// path tracer's light there left a second mouth and the hat's old shadow trailing (PTDenoisePeopleHistory)
		IDirect3DBaseTexture9* People = nullptr;	// UNOFFICIAL 2026-11-07 (ptaaa-firstperson; RGB mode, with PeopleHistory): the people mask in its
										// alpha instead of TESR_CapturedNormalsBuffer: > 1.5 people, > 3.5 the first-person view model, whose history
										// stays at the same screen point (it moves with the camera; RtPathTrace.cpp ViewModelPrep's normals)
		IDirect3DBaseTexture9* Guide = nullptr;			// UNOFFICIAL 2026-11-07 (RGB mode): the game's lit frame (the scene as stored) and the
		IDirect3DBaseTexture9* GuideAlbedo = nullptr;	// first surface's colour: the a-trous passes stop at the game's own shading edges
		float	GuideSigma = 0.0f;						// (their ratio; this relative difference weighs a tap down by 1 / e; 0 = off)
		bool	GuideOnce = false;						// UNOFFICIAL 2026-11-13 (rtgi-review B2, RGB mode with Guide): the first a-trous pass works the guide's
														// value out per texel and hands it on in B's blue; the later passes read it there (the same picture, fewer reads)
		float	AntiLag = 0.0f;			// UNOFFICIAL 2026-11-07 (RGB mode): a fast history (4 frames at 60 fps) beside the long one; where the long
										// one's luminance leaves the fast mean's window (this many standard errors; 0 = off) it is clamped and
										// restarts at the fast history's length: trails clear in a few frames (NRD ReBLUR's anti-lag; PTDenoiseAntiLag)
		int		MovingHistory = 0;		// UNOFFICIAL 2026-10-31 (RGB mode, with DenoiseBackbone and B1's object motion): pixels whose surface moves
										// itself (> 0.25 units a frame) keep at most this many frames of history (0 = no cap; RT GI: GIMovingHistory)
		// UNOFFICIAL 2026-11-07 (ptaaa-denoiser, docs/next/RT-PATH-TRACING.md "ptaaa-denoiser"):
		bool	ObjectMotion = false;	// (RGB mode) B1's object motion ([Main.Main.Quality] ObjectMotion) subtracted before reprojecting also
										// without DenoiseBackbone: a walking NPC's history follows him instead of trailing behind (MovingHistory then
										// applies too). The path tracer's denoiser (PTDenoiseObjectMotion)
		bool	Specular = false;		// (RGB mode) a SPECULAR signal (the path tracer's PTDenoiseSplit): Noisy .a = the specular hit distance
										// (0 unknown, 60000 a miss); Compute\RtDenoiseTemporalSpec.frag reprojects by the reflected (virtual) point
										// mixed with the surface by roughness, the a-trous passes keep smooth surfaces sharp
		IDirect3DBaseTexture9* Roughness = nullptr;	// (Specular) .g = roughness (required)
		float	MirrorHistory = 12.0f;	// (Specular) the history a mirror keeps (frames); MaxHistory from roughness 0.4 up
		bool	Virtual = true;			// (Specular) the hit-distance (virtual) reprojection; false = the surface's only (the A/B)
		bool	MirrorSurface = false;	// UNOFFICIAL 2026-11-09 (pt-mirrors, Specular) PTDenoiseMirrorSurface: pixels the path tracer marks (Noisy .a
										// < 0: a smooth metal whose reflected thing stands in for it, its colour divided out) keep the reflected
										// surface's full history (the virtual point of a curved mirror too) and are smoothed in reflection space;
										// false = as before (mirrors: MirrorHistory frames, no spatial pass)
		float	MeanVariance = 0.0f;	// (RGB mode) the a-trous brightness test with the variance of the accumulated mean of up to this many
										// frames (per-path variance / min(history, this); 0 / 1 = one path's, as before): converged fine shadows (a
										// chain-link wire's) stay edges instead of noise to smooth away. PTDenoiseMeanVariance (2)
		float	AntiFirefly = 0.0f;		// (RGB mode) k > 0: a new sample brighter than its 3 x 3 neighbours (the brightest, the centre left out)
										// and its own fast history's mean + k deviations is clamped to that, and the excess is spread over the 5 x 5
										// round into the history (Compute\RtDenoiseFirefly.frag): no white dots, the light kept. 0 = off.
										// The path tracer: PTDenoiseAntiFirefly / PTDenoiseFireflyK
		float	HistoryFix = 0.0f;		// (RGB mode) pixels with this many frames of history or fewer are smoothed by geometry only in the a-trous
										// passes, more the newer (NRD ReLAX's history fix; 0 off): a newly seen pixel's single noisy path is not kept
										// as a speckle by the brightness test (trails behind a moving lamp / object). The path tracer: PTDenoiseHistoryFix
	};
	struct Denoiser;

	// UNOFFICIAL 2026-11-07 (perf-history) this frame's history clock (RtHistoryTime.h): the smoothed frame time (updated on the first
	// call of a rendered frame from TheFrameRateManager->ElapsedTime; a hitch, a load or a camera cut restarts it), DenoiseTimeBased and
	// DenoiseMaxHistorySeconds. Every RT denoiser converts its frame counts with it (RtHistoryTime::Frames / Keep).
	RtHistoryTime::Clock HistoryClock();

	Denoiser*	Create(const char* Name);
	// UNOFFICIAL 2026-10-31 DenoiseBackbone: this frame's shared temporal backbone (RGBA16F, full size: rg = last frame's uv - uv, b = 1
	// disoccluded, a = view z / farZ), or nullptr (off, or no denoiser ran yet this frame). For other temporal effects to read.
	IDirect3DTexture9* Backbone();
	bool		Run(Denoiser* D, IDirect3DDevice9* Device, IDirect3DBaseTexture9* Noisy, IDirect3DSurface9* Output, const Settings& S);
	// UNOFFICIAL 2026-10-31 (rt-ao-sky) RT GI with DenoiseBackbone: B1's object motion as a texel shift for ReSTIR GI's reuse, into Target's
	// alpha (the GI frame light, Width x Height = the trace grid; Compute\RtGIMotion.frag). False: not drawn (no object motion this frame,
	// no last frame, the pass missing): the trace then reprojects with the camera only.
	// UNOFFICIAL 2026-11-04 RT GI at RenderScale < 1 (docs/next/RT-RENDERSCALE.md, phase 2): ScaleX / ScaleY = this frame's RenderRect
	// scale (1 in native frames): the trace grid is then the top-left rect (Width x Height = its size), the screen point = uv / scale.
	bool		GIMotion(IDirect3DDevice9* Device, IDirect3DSurface9* Target, UINT Width, UINT Height, float ScaleX = 1.0f, float ScaleY = 1.0f);
}
