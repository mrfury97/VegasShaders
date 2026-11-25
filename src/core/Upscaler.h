#pragma once

// UNOFFICIAL 2026-10-27 (NVR DXVK): the temporal upscaler behind TAA's FSR path ([Shaders.TAA.FSR]), as an interface so a
// second upscaler (NVIDIA DLSS through a 64-bit helper process: core\UpscalerDlss.cpp, docs\next\FORK2-DLSS.md) takes FSR 2's
// place in the same two calls without touching what feeds it:
//   - TAA's native AA (effects\TAAFSR.cpp): the post-tonemap frame at the screen's own resolution (render size = display size);
//   - RenderScale (effects\TAAUpscale.cpp): the world scene drawn at a lower internal resolution, upscaled before NVR's effects.
// Both build the same inputs (jittered colour, a depth, camera motion vectors, the jitter) and read Output() afterwards.
//
// FSR 2 is AMD's (FidelityFX Super Resolution 2.2.1, GPUOpen, MIT; src\hlsl\NewVegas\Compute\FSR2\LICENSE-FidelityFX-FSR2.txt).
// Everything here needs NVR DXVK (compute passes through the extension). A failure is logged once and the upscaler reports
// Failed() for the session; its callers then keep their D3D9 paths.

#include "NvrDxvkExt.h"
#include <string>

namespace Upscaler {

	// What an upscaler gets each frame. The textures are D3D9 render-target textures the NVR extension can bind; the
	// render-resolution ones hold this frame's picture in their top-left RenderWidth x RenderHeight (the rest is not read).
	struct Inputs {
		IDirect3DBaseTexture9*	Color = nullptr;	// the jittered picture (render resolution); linear HDR when Hdr, else the tonemapped frame
		IDirect3DBaseTexture9*	Depth = nullptr;	// r: device depth in FSR's standard convention (0 near .. 1 far), g: reactive mask [0, 1]
		IDirect3DBaseTexture9*	Motion = nullptr;	// rg: previous uv - current uv (fractions of the picture), unjittered, render resolution
		// UNOFFICIAL 2026-11-01 (DLSS ghosting on the first-person weapon): TESR_DepthBufferViewModel (> 0 on the weapon, render resolution in
		// the same top-left rectangle), or null when the weapon is not in Color (RenderScale v1 draws it after the upscale). Only DLSS reads it
		// (its bias current colour mask, [Shaders.TAA.DLSS] WeaponMask); FSR has the weapon in Depth's reactive channel already.
		IDirect3DBaseTexture9*	ViewModel = nullptr;
		UINT	RenderWidth = 0, RenderHeight = 0;		// this frame's picture
		UINT	InputWidth = 0, InputHeight = 0;		// the input textures' size (at least the render size)
		UINT	DisplayWidth = 0, DisplayHeight = 0;	// the output
		float	JitterX = 0.0f, JitterY = 0.0f;			// this frame's sub-pixel offset in render pixels (FSR 2's sign convention)
		float	JitterPhases = 8.0f;					// length of the jitter sequence
		float	NearZ = 1.0f, FarZ = 1.0f;				// the depth's near and far planes (game units)
		float	TanHalfFovX = 1.0f, TanHalfFovY = 1.0f;
		float	DeltaSeconds = 0.0f;
		float	Sharpness = 0.0f;						// [0, 1]: the upscaler's own sharpening of its output, 0 off
		float	UnitsPerMetre = 70.0f;					// Gamebryo: about 70 units per metre
		bool	Hdr = false;							// Color is linear HDR (before tonemapping)
		bool	Reset = false;							// no usable history (first frame, a cut, a mode or size change)
	};

	class IUpscaler {
	public:
		virtual ~IUpscaler() {}
		virtual const char*			Name() const = 0;
		// Records the upscale into DXVK's command stream (no waits); the result is then in Output() (rgba16f, display size).
		// False: this frame failed (logged); Failed() says whether the upscaler is off for the session.
		virtual bool				Evaluate(IDirect3DDevice9* Device, const Inputs& In) = 0;
		virtual IDirect3DTexture9*	Output() const = 0;
		virtual bool				Failed() const = 0;
		virtual void				ReleaseResources() = 0;
		// UNOFFICIAL 2026-10-28: the next Evaluate starts without history (Choose() calls it when the chosen upscaler changes:
		// the other one's frames went by meanwhile).
		virtual void				RequestReset() {}
		// UNOFFICIAL 2026-11-01 (core/Diagnostics.h part 5): its own state as of its last Evaluate, one phrase for the log lines
		virtual std::string			DiagText() const { return ""; }
	};

	// AMD FSR 2.2.1 (core\Upscaler.cpp). One shared instance: TAA's native AA and RenderScale use it in turn (each switch
	// between them is a Reset), so its textures (about 190 MB at 1080p, made on first use) exist once.
	IUpscaler*	Fsr2();

	// UNOFFICIAL 2026-10-28 ([Shaders.TAA.FSR] Version 1): AMD FSR 3.1's upscaler (core\UpscalerFsr31.cpp; FidelityFX SDK 1.1.4,
	// MIT), the same inputs as FSR 2. Upscaling only. Its own textures (made on first use); FSR 2's are not made while it runs.
	IUpscaler*	Fsr31();

	// NVIDIA DLSS through NvrDlssHelper.exe (64-bit; core\UpscalerDlss.cpp, tools\nvr-dlss-helper). No NVIDIA code in NVR's DLL.
	// It runs FSR 2 itself while the helper starts, and from the first problem on (logged once with the reason).
	IUpscaler*	Dlss();

	// [Shaders.TAA.DLSS] HelperTest (the helper's pass-through test: the same exchange, no DLSS) and JitterFlip (bit 0
	// flips the jitter's x, bit 1 its y, for checks with NVIDIA's development DLL). Set by TAAEffect::UpdateSettings.
	// UNOFFICIAL 2026-10-30 (DLAA stall report, RTX 5060: the game's GPU sat ~45 ms in the wait for the helper with interior shadows on):
	// HelperWait 1 (the helper waits on the CPU for the game's inputs, then submits with no GPU wait in it; 0 = its submission waits on the
	// GPU, as before) and HelperGpuPriority 1 (the helper's queue and process at the high GPU priority; 0 = normal). Both are the helper's
	// start options: a change starts it again.
	// UNOFFICIAL 2026-10-30 (tester report: with DLSS on, the render-scaled picture stayed small in the top-left corner): HelperExactInputs 1
	// (the helper copies the render rectangle into images of exactly its size, DLSS reads those whole; 0 = DLSS reads the rectangle out of the
	// screen-sized images, as before) and HelperFailTest (TEST ONLY: 1 NVR ends the helper at its 120th frame, 2 the helper reports a failure
	// there, 3 it stops answering there; every one must leave FSR 2 upscaling the whole screen). Start options too; a HelperFailTest change
	// also starts a helper that turned itself off again.
	void		DlssOptions(bool PassThroughTest, int JitterFlip, bool HelperCpuWait = true, bool HelperHighPriority = true, bool HelperExactInputs = true,
				int HelperFailTest = 0);
	// UNOFFICIAL 2026-11-01 (player report: with DLSS the first-person arms and gun ghost and smear while they animate): [Shaders.TAA.DLSS]
	// WeaponMask [0, 1], DLSS SR's bias current colour mask on the weapon (Inputs::ViewModel; FSR's reactive value elsewhere). 0: no mask is
	// given to DLSS (as before). Per frame, no restart. Set by TAAEffect::UpdateSettings next to DlssOptions.
	void		DlssWeaponMask(float Value);
	float		DlssWeaponMaskValue();

	// 0 FSR 2 ([Shaders.TAA.FSR] Enabled), 1 DLSS ([Shaders.TAA.DLSS] Enabled) (Dlss(), which falls back to FSR 2 whenever DLSS cannot run),
	// 2 FSR 3.1 ([Shaders.TAA.FSR] Enabled + Version 1; FSR 2 from its first failure on, logged once).
	IUpscaler*	Choose(int setting);

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (docs\next\FORK2-DLSS-RR.md; [Shaders.TAA.DLSS] RayReconstruction, off by default):
	// the DLSS helper's second NGX feature (DLSS-D), evaluated at the RR point of the pre-tonemap chain (TAAEffect::RayReconstruct,
	// after the Reflections composite, native resolution only) instead of DLSS SR after the tonemap. The same helper, semaphores and
	// frame counter as Dlss(): a frame goes either to DLSS SR (Evaluate) or to RR (EvaluateRayReconstruction), never both.
	// Inputs' Color is the noisy lit picture decoded to linear HDR (Hdr true); Depth / Motion as the upscalers get them (FSRPrep's).
	struct RrInputs : Inputs {
		IDirect3DBaseTexture9*	DiffuseAlbedo = nullptr;	// rgba16f: rgb diffuse albedo
		IDirect3DBaseTexture9*	SpecularAlbedo = nullptr;	// rgba16f: rgb specular albedo
		IDirect3DBaseTexture9*	Normals = nullptr;			// rgba16f: xyz unit normal (camera-relative world space; view space with NormalsView)
		IDirect3DBaseTexture9*	Roughness = nullptr;		// r16f: linear roughness
		float	WorldToView[16] = {};	// camera-relative view (rotation only), D3DX row-vector layout
		float	ViewToClip[16] = {};	// the unjittered projection, the same layout
	};
	struct RrOptions {
		bool	Wanted = false;			// RayReconstruction (with [Shaders.TAA.DLSS] Enabled); a change restarts the helper (its images)
		bool	KeepDenoisers = false;	// RRKeepDenoisers: NVR's own RT denoisers keep running under RR (A/B)
		int		Preset = 0;				// Preset: DLSS SR's model (NvrDlssPreset: 0 default, 1 CNN, 2 transformer, 3 transformer J)
		int		RrPreset = 0;			// RRPreset (NvrDlssRrPreset: 0 default, 1 D, 2 E, 3 F = the newer model, 4 auto by GPU generation)
		bool	NormalsView = false;	// RRNormals 1: the normals guide in view space
		bool	DepthLinear = false;	// RRDepthLinear: DLSS.Use.HW.Depth 0
	};
	// TAAEffect::UpdateSettings, every settings pass, next to DlssOptions.
	void		DlssRrOptions(const RrOptions& Options);
	const RrOptions& DlssRrSettings();
	// True when an RR evaluate this frame can work: Wanted, the helper running with rrAvailable (RTX + nvngx_dlssd.dll + NGX's
	// SuperSamplingDenoising, or the pass-through test). The RR point's own frame checks (native size, a world frame) are the caller's.
	bool		RayReconstructionReady();
	// This frame's DLSS-D: the inputs go to the helper, the result is in RayReconstructionOutput() (rgba16f, linear, display size).
	// False: nothing was recorded (RR not ready, an input missing, the helper stopped): the caller leaves the frame alone, and DLSS SR
	// (or FSR 2) runs after the tonemap as before.
	bool		EvaluateRayReconstruction(IDirect3DDevice9* Device, const RrInputs& In);
	IDirect3DTexture9*	RayReconstructionOutput();
	// Once a frame, at the start of the pre-tonemap chain (before the RT point): last frame's RR result becomes this frame's
	// WantsRaw, and this frame's starts at "not run" (and "not planned").
	void		RayReconstructionFrameBegin();
	// UNOFFICIAL 2026-10-30 (review): the frame's plan, set once after FrameBegin and before the RT point (TAAEffect::
	// RayReconstructionPlanned: every frame check that is known then). WantsRaw needs it, so a frame that will not go to RR (the Pip-Boy,
	// a menu, a RenderScale frame...) keeps every denoiser even right after RR frames.
	void		RayReconstructionPlan(bool Planned);
	// The RR point put DLSS-D's result into the frame (after its composite and the chain's Commit): that is what next frame's WantsRaw
	// goes by (an evaluate whose composite failed does not count).
	void		RayReconstructionDelivered();
	// The per-effect RT denoisers hand RR their raw signals this frame: RR ran last frame, Wanted, KeepDenoisers off. False: every
	// effect runs exactly its old path.
	bool		RayReconstructionWantsRaw();
}
