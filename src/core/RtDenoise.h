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
		bool	ClampHistory = false;	// UNOFFICIAL 2026-11-04 (one channel): the history pulled into this frame's 3 x 3 range (RT sun: moved shadows do not trail)
		int		MovingHistory = 0;		// UNOFFICIAL 2026-10-31 (RGB mode, with DenoiseBackbone and B1's object motion): pixels whose surface moves
										// itself (> 0.25 units a frame) keep at most this many frames of history (0 = no cap; RT GI: GIMovingHistory)
	};
	struct Denoiser;

	Denoiser*	Create(const char* Name);
	// UNOFFICIAL 2026-10-31 DenoiseBackbone: this frame's shared temporal backbone (RGBA16F, full size: rg = last frame's uv - uv, b = 1
	// disoccluded, a = view z / farZ), or nullptr (off, or no denoiser ran yet this frame). For other temporal effects to read.
	IDirect3DTexture9* Backbone();
	bool		Run(Denoiser* D, IDirect3DDevice9* Device, IDirect3DBaseTexture9* Noisy, IDirect3DSurface9* Output, const Settings& S);
	// UNOFFICIAL 2026-10-31 (rt-ao-sky) RT GI with DenoiseBackbone: B1's object motion as a texel shift for ReSTIR GI's reuse, into Target's
	// alpha (the GI frame light, Width x Height = the trace grid; Compute\RtGIMotion.frag). False: not drawn (no object motion this frame,
	// no last frame, the pass missing): the trace then reprojects with the camera only.
	bool		GIMotion(IDirect3DDevice9* Device, IDirect3DSurface9* Target, UINT Width, UINT Height);
}
