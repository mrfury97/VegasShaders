#pragma once

// UNOFFICIAL, optional (off by default; Shaders.BounceLight): one bounce of screen-space indirect light
// (BounceLight.fx.hlsl). Runs in the pre-tonemap chain right after ambient occlusion. Gather and blur run at
// half (or, with QuarterResolution, quarter) resolution in the buffers below; only the combine is full resolution.
// UNOFFICIAL options (each off: as before): light guessed for samples that leave the screen, more bounces fed back from
// last frame's result, and last frame's gather reused (found again by reprojection: BounceLightMath.h).
class BounceLightEffect : public EffectRecord
{
public:
	BounceLightEffect() : EffectRecord("BounceLight") {};

	struct BounceLightStruct {
		D3DXVECTOR4		Data;	// x: strength, y: radius, z: draw distance, w: blur radius
	};
	BounceLightStruct	Constants;
	bool				quarterResolution = false; // [Shaders.BounceLight.Main] QuarterResolution
	// UNOFFICIAL, [Shaders.BounceLight.Main] OffScreenLight, MultiBounce, Temporal, HalfSamples (BounceLight.fx.hlsl's
	// techniques BounceLightPlus, BounceLightTemporal, BounceLightTemporalHalf). All off: the first technique, as before.
	float				offScreenLight = 0.0f;	// light guessed for samples off screen (0: none, as before)
	float				multiBounce = 0.0f;		// extra bounces from last frame's result (0: none, as before; 1: all of it)
	bool				temporal = false;		// reuse last frame's gather (the history buffers below)
	bool				halfSamples = false;	// with temporal: 4 samples a frame instead of 8
	// UNOFFICIAL NeonSpill ([Shaders.BounceLight.Main] NeonSpill, NeonSpillReach, NeonSpillThreshold; BounceLight.fx.hlsl):
	// long-range light from bright lights (neon, lamps), gathered from a pyramid of ever smaller copies of the lights on
	// screen (seven buffers, level 0 320 texels wide at any resolution, made the first time it runs: about 0.6 MB). With it
	// on, the gather applies Strength to the short-range bounce and the combine's strength (Constants.Data.x) is 1. 0: off,
	// as before.
	static const int	SpillLevels = 7;
	float				strength = 0.0f;		// [Shaders.BounceLight.Main] Strength
	float				neonSpill = 0.0f;
	float				spillReach = 1.0f;
	float				spillLightLevel = 1.5f;	// NeonSpillLightLevel: times a white surface in the scene's own light a light must reach
	float				spillReportedLight = -1.0f;	// the scene light last logged
	int					spillDebug = 0;			// NeonSpillDebug: 0 off, 1 what counts as a light, 2 the bounce light alone
	// With the spill on, the combine is this effect's own (not folded into the specular's): it has the debug views and the
	// slope-aware smoothing on the street (BounceLight.fx.hlsl's Combine). Read by ShaderManager.
	bool	SpillOn() const { return Enabled && neonSpill > 0.0f; }
	// UNOFFICIAL HardEdges: the hard depth test in this effect's own Combine; the specular's folded combine has only the soft one,
	// so ShaderManager does not fold it while this is on.
	bool	HardEdgesOn() const { return Enabled && hardEdges; }
	bool	hardEdges = false;				// HardEdges: Combine's (and Blur's) hard depth test for every Plus/Temporal technique
	float				spillMemorySeconds = 1.0f;	// NeonSpillMemory: how long the light of a sign that left the screen lingers
	float				spillNear = 1.0f, spillFar = 1.0f;	// NeonSpillNear / NeonSpillFar: weight of the closest / farthest reads
	float				spillCap = 0.0f;			// NeonSpillBrightCap: most a light counts, x the light test (0: no limit)
	// UNOFFICIAL round 6, each 0 = round 5 exactly (public, so a self-test can override them per frame after UpdateSettings).
	float				spillHoldAfterTurn = 0.0f;	// NeonSpillHoldAfterTurn: seconds the memory's hold keeps working after the view stops
	float				spillSeparateDepth = 0.0f;	// NeonSpillSeparateDepth: > 0 = technique BounceSpillGatherSeparate (no light on a sign's own wall)
	float				spillCapSceneLight = 0.0f;	// NeonSpillCapSceneLight: > 0 = BrightCap in multiples of the scene's own light, not the light test
	float				spillHardEdges = 0.0f;		// NeonSpillHardEdges: > 0 = Combine's hard depth test (no 4 x 4 blocks of wall light on thin things)
	float				spillMaskSpillOnly = 0.0f;	// NeonSpillMaskSpillOnly: > 0 = light pixels lose only the spill (GatherPlus), not all bounce (Combine)
	float				spillStillSeconds = 0.0f;	// how long the view has not moved (for NeonSpillHoldAfterTurn)
	IDirect3DTexture9*	spillTexture[SpillLevels] = {};
	IDirect3DSurface9*	spillSurface[SpillLevels] = {};
	bool				spillTried = false;
	// The spill's own low-resolution result with memory, per resolution [0 half, 1 quarter] and ping-pong (this frame's /
	// last frame's). Made with the pyramid's buffers.
	IDirect3DTexture9*	spillMemoryTexture[2][2] = {};
	IDirect3DSurface9*	spillMemorySurface[2][2] = {};
	int					spillMemoryIndex = 0;		// the buffer last frame's spill went to
	bool				spillMemoryValid = false;
	// UNOFFICIAL NeonSpillSteadySeconds (2026-10-12, the user's muzzle flashes spilling light through walls): each level-0 texel's
	// light averaged over that long (rgb, a = depth), ping-pong, written by the pyramid's level-0 pass as its second target; a
	// light counts only as far as it has been there that long, so a flash of a few frames barely counts. 0: off.
	float				spillSteadySeconds = 0.0f;
	IDirect3DTexture9*	spillSteadyTexture[2] = {};
	IDirect3DSurface9*	spillSteadySurface[2] = {};
	int					spillSteadyIndex = 0;		// the buffer last frame's average went to
	bool				spillSteadyValid = false;
	double				spillSteadyTime = 0.0;
	D3DXMATRIX			spillSteadyView, spillSteadyInvView, spillSteadyProj;
	bool	EnsureSpill();
	bool	RenderSpillPyramid(IDirect3DDevice9* Device, IDirect3DTexture9* scene, float threshold);
	float	SpillThreshold();
	IDirect3DTexture9*	RenderSpillGather(IDirect3DDevice9* Device, bool quarter, const D3DVIEWPORT9& reduced);
	// Temporal: last frame's gather and this frame's, per resolution, made the first time Temporal runs (EnsureHistory).
	IDirect3DTexture9*	halfHistory[2] = {};
	IDirect3DSurface9*	halfHistorySurface[2] = {};
	IDirect3DTexture9*	quarterHistory[2] = {};
	IDirect3DSurface9*	quarterHistorySurface[2] = {};
	bool				halfHistoryTried = false;
	bool				quarterHistoryTried = false;
	void	EnsureHistory(bool quarter);
	// What the last frame's low-resolution passes left for this one: their camera (to find last frame's bounce at the same
	// spot), and whether their final result (low buffer 0) and their gather (history buffer historyIndex) can be used.
	D3DXMATRIX			lastView, lastInvView, lastProj;
	D3DXVECTOR2			lastJitter = D3DXVECTOR2(0.0f, 0.0f);	// UNOFFICIAL 2026-11-09 TemporalJitterFix: the depth jitter of the frame lastView is
	bool				jitterFix = true;	// UNOFFICIAL 2026-11-09 TemporalJitterFix ([Shaders.BounceLight.Main])
	bool				lastValid = false;
	bool				lastQuarter = false;
	bool				historyValid = false;
	int					historyIndex = 0;
	unsigned			frameNumber = 0;
	double				lastTime = 0.0;
	IDirect3DTexture9*	halfTexture[2] = {};
	IDirect3DSurface9*	halfSurface[2] = {};
	IDirect3DTexture9*	quarterTexture[2] = {};
	IDirect3DSurface9*	quarterSurface[2] = {};
	// Depth, normal and colour packed per low-resolution pixel for the gather (A32B32G32R32F).
	IDirect3DTexture9*	halfPrepTexture = nullptr;
	IDirect3DSurface9*	halfPrepSurface = nullptr;
	IDirect3DTexture9*	quarterPrepTexture = nullptr;
	IDirect3DSurface9*	quarterPrepSurface = nullptr;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	EnsureBuffers(bool quarter);
	bool	halfTried = false;
	bool	quarterTried = false;
	// UNOFFICIAL: handles looked up once per loaded effect (EffectRecord::CachedHandle).
	CachedHandle paramLayout, paramTemporal, paramAmbient, paramReproject, techTemporalHalf, techTemporal, techPlus;
	CachedHandle paramSpill, paramSpillTarget, paramSpillDebug, paramSpillMemory, paramSpillShape, techSpillPyramid, techSpillGather;
	CachedHandle paramSpillOptions, techSpillGatherSeparate, techPlusHard, techTemporalHard, techTemporalHalfHard;
	CachedHandle paramSpillSteady;
	// With deferCombine, Render runs the low-resolution passes only and leaves their result for
	// SpecularEffect::RenderWithBounce (deferredReady, deferredTexture, deferredLayout); with combineOnly, the combine
	// alone, from that result (if the merged pass could not run). Set by ShaderManager.
	bool				deferCombine = false;
	bool				combineOnly = false;
	bool				deferredReady = false;
	IDirect3DTexture9*	deferredTexture = nullptr;
	D3DXVECTOR4			deferredLayout = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-25 (NVR DXVK, [Main.Main.Performance] NvrDxvkPasses): the five passes of every bounce technique as Vulkan
	// passes (Compute\BounceLight*.frag, the .fx.hlsl line for line), in place of Render's D3D9 loop: the low-resolution ones into
	// the same buffers (ExtDrawTo), the combine through the frame chain (ExtFullScreen). The constants go into one uniform buffer per
	// frame. False: the combine was not committed and Render's D3D9 loop draws the frame (after a failed draw, every frame).
	bool	ExtPasses(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, D3DXHANDLE technique,
		UINT firstPass, UINT endPass, IDirect3DTexture9* scene, IDirect3DTexture9* const* lowTexture, IDirect3DSurface9* const* lowSurface,
		IDirect3DTexture9* prepTexture, IDirect3DSurface9* prepSurface, IDirect3DTexture9* const* historyTexture,
		IDirect3DSurface9* const* historySurface, int writeIndex, const D3DXVECTOR4& layout, const D3DXVECTOR4& temporalData,
		const D3DXVECTOR4& ambientData, const D3DXMATRIX& reproject, class GpuTimer* passTimers);
	ExtPass				extPrepare, extGather, extBlur, extCombine;
	ExtUniformBuffer	extUniforms;
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
		IDirect3DSurface9* SourceBuffer) override;
};
