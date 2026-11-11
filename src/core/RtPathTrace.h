#pragma once

// UNOFFICIAL 2026-11-02 the REFERENCE path tracer ([Main.Main.RayTracing] ReferencePT, off; docs\next\RT-PATH-TRACING.md): a ground
// truth for the raster and hybrid look, not a real-time effect. The RT helper (RtCore.h) traces PTSamples paths a pixel and frame from
// the camera (Compute\Rt\RtPathTrace.comp: PTBounces diffuse bounces, the albedo atlas, the sun, the sky, the lamps and glow maps) and
// keeps a running mean while the camera, the sun and the settings stay still; RenderDebug shows it over the finished picture
// (RtPathTraceShow.frag; PTSplit keeps the game's frame on the left for an A/B). Needs the same as every RT effect: the DXVK fork, a GPU
// with ray tracing for 64-bit programs (the 9800X3D's iGPU here; not the GTX 1070) and NvrRtHelper.exe. Off: nothing runs.

namespace RtPathTrace {
	void	Register();						// once: the core user
	void	RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);	// the traced picture over the finished frame
	void	Replace(IDirect3DDevice9* Device);
	bool	Replacing();	// (2026-11-03) the traced picture replaces the scene this frame: AO / AmbientLight / Specular stay out (no light twice)	// (2026-11-03) PTReplace: the traced picture into the scene right after RtCore::Frame
	std::string	StatusText();				// the REPORT block's line: on / why not, samples so far
	// UNOFFICIAL 2026-11-11 (user: "these keys have no reason to exist ... a second level dev menu only for our eyes"; a tester had set
	// ReferencePT + PTReplace + PTDenoise by hand, which IS path tracing, so PTRealtime looked dead): the three are internal comparison
	// tools (the converged reference, the trace before the denoiser, the picture over the finished frame). They show in the menu and are
	// honoured only with [Main.Menu.Developer] InternalTools on (a key of the settings file, never in the menu) or while a test run's
	// shot list drives the game (the bench sets them). Otherwise they read as off and PTRealtime is the one path tracing switch.
	bool	InternalTools();
	bool	ReferenceOn();					// ReferencePT, as honoured (false without InternalTools)
	// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) DLSS Ray Reconstruction's guides from the path tracer (TAAFSR.cpp RayReconstruct): true when
	// this frame's traced picture is in the scene at the screen's size (PTReplace + PTDenoise, native). Albedo: rgb the first surface's
	// colour the diffuse light was divided by, a 0 = no surface (sky / the game's frame), 1 + metal (PTDenoiseSplit) or 1; SpecularRG /
	// SpecularBRough (G32R32F; nullptr without PTDenoiseSplit): the specular albedo the specular light was divided by, + the roughness.
	bool	RrGuides(IDirect3DTexture9** Albedo, IDirect3DTexture9** SpecularRG, IDirect3DTexture9** SpecularBRough);
	// UNOFFICIAL 2026-11-10 early kick (bench only: env NVR_RT_EARLY_PT; docs/next/PT-EARLY-KICK-PLAN-2026-11-10.md): live path tracing
	// (PTReplace + PTDenoise) hands its frame to the helper when the world STARTS drawing, with a traced first surface (no G-buffer of
	// this frame), and collects it right before Replace. Without the env both do nothing.
	void	SubmitBeforeWorld(IDirect3DDevice9* Device);	// the world-start hook (the frame's first world scene, after TAA's jitter)
	void	CollectBeforeShow(IDirect3DDevice9* Device);	// right before Replace
}
