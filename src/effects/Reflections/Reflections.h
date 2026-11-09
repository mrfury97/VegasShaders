#pragma once
#include "../../core/TemporalHistoryStats.h"

// UNOFFICIAL: screen-space reflections on the ground (Effects\Reflections.fx.hlsl): wet streets mirroring the neon and the
// sky in the rain, an optional dry sheen on outdoor ground and on interior floors. Half-resolution trace, history and
// blur into its own buffers, then a full-resolution composite in the HDR chain (after Specular, before the fog).
class ReflectionsEffect : public EffectRecord
{
public:
	ReflectionsEffect() : EffectRecord("Reflections") {};

	struct SettingsStruct {
		float	GroundSheen;	// outdoors: how much dry ground facing up reflects
		float	RainWetness;	// outdoors: how much the rain's wetness adds
		float	InteriorFloors;	// indoors: how much floors reflect
		float	Roughness;		// dry surfaces (0 mirror .. 1 matte)
		bool	MirrorSmoothOnly;	// 2026-11-06: Mirror only on surfaces whose own roughness is under 0.05 (the test room's mirror panels)
		float	Mirror;			// 2026-11-05: 0 real stone/tile (F0 0.04) .. 1 a true mirror (F0 1): how much of the reflection shows at any angle
		float	WetRoughness;	// wet surfaces
		float	MaxDistance;	// ray length, game units
		int		Steps;
		float	Thickness;		// how thick things count as, for a ray passing behind them
		float	Temporal;		// share of last frame's result kept (0: no history)
		float	BlurRadius;		// half-resolution texels each way
		float	DrawDistance;	// surfaces farther than this do not reflect
		bool	SkyFallback;	// outdoors: rays that find nothing take the sky
		bool	CubeMap;		// rays that find nothing take the surroundings cube
		float	CubeCapture;	// how much of the cube each frame's view replaces
		float	CubeFade;		// how much of what is off screen fades each frame
		int		DebugView;		// 1: the reflection alone, 2: where it found something, 3: the surroundings cube (4-6: see the .fx)
		bool	GridFallback;	// UNOFFICIAL 2026-10-19: rays that find nothing on screen also take AmbientLight's light grid
		float	GridBrightness;	// its brightness
		int		MaterialAware;	// UNOFFICIAL 2026-10-20: 1 = the _rmaos materials decide what reflects (core/ReflMaterial.h); 2 = filled, not used (test)
		float	MaterialReflect;	// how much a smooth known material reflects
		float	MaterialMaxRoughness;	// known materials at least this rough do not reflect
		float	ReflectAdd;		// UNOFFICIAL 2026-10-21: 0 the reflection replaces its share of the surface's colour (as before), 1 it is added on top
		float	SpecularOcclusion;		// UNOFFICIAL 2026-10-21: how much blurred reflections dim where the surface itself is dark (0 off)
		float	SpecularOcclusionLevel;	// the surface / reflection brightness ratio that keeps all of the reflection
		bool	HiZ;			// UNOFFICIAL 2026-10-26 (NVR DXVK, core/HiZ.h): the hierarchical ray march on the Hi-Z pyramid (Vulkan trace only), outdoors
		float	GlassReflect;	// UNOFFICIAL 2026-10-29 (core/GlassMask.h, Vulkan passes only): how much see-through glass reflects (0 off: nothing hooked)
		bool	MaterialBufferRoughness;	// UNOFFICIAL 2026-10-30 (Vulkan passes only): the floor guess takes TESR_MaterialBuffer's roughness where written
		bool	CloudSky;		// UNOFFICIAL 2026-10-30 (Vulkan passes only, with VolumetricClouds): the sky fallback shows the clouds (the cloud sky probe)
		bool	HiZSafeCells;		// test only: coarse cells read as min/max of their two channels, "behind" skip on the finest level only
		bool	HiZInteriorsTest;	// test only: HiZ indoors too (the interior picture is not yet the old trace's; DebugView 9 / 10 to find why)
		float	ReflectivityExterior;	// UNOFFICIAL 2026-10-25 [Shaders.PBR.Main] ReflectivityExterior / Interior: times the dry sheen and
		float	ReflectivityInterior;	// MaterialReflect (not the rain's wetness); 1 = as before
		// UNOFFICIAL 2026-10-28 [Shaders.Reflections.RT] (NVR DXVK + the RT core; docs/next/FORK-RT-REFLECTIONS.md): ray queries where the
		// screen-space march misses (Compute\ReflectionsTraceRT.frag). Off, or no RT on this GPU: the trace as before.
		bool	RtEnabled;
		float	RtMaxRoughness;		// rougher surfaces keep the old fallbacks (cube, grid, sky)
		float	RtDistance;			// ray length, game units
		bool	RtRaysOnly;		// 2026-11-07: RT on = the rays alone (RT DebugView 3's behaviour, the user's pick): no screen-march answers
		float	RtMetalBounce;	// 2026-11-07 (perf-reflfix): smooth metal hit by a ray reflects (one more ray) up to this roughness; 0 off
		int		RtDebugView;		// 1 sources, 2 hit distance, 3 RT without the screen march, 4 hit instance
		bool	RtAlphaSolid;		// UNOFFICIAL 2026-11-06 diagnostic: alpha-tested hits taken as solid (is the alpha test what the rays miss?)
		float	RtScreenTolerance;	// a hit counts as the screen's pixel when their depths differ by less than this share
		float	RtEstimate;			// brightness of the estimate for hits neither the screen nor the cube can show
		bool	RtSunShadows;		// the estimate's sun goes through a shadow ray
		bool	RtSplitTest;		// test only: RayGen + Resolve without rays (no helper needed): must be the normal trace's picture
		bool	RtVndf;				// 2026-10-29: glossy rays from the GGX distribution of visible normals (Heitz 2018)
		bool	RtHitLamps;			// 2026-10-29: the 12 nearest scene lamps light the hits the screen cannot show (one shadow ray to the key lamp)
		float	RtRoughReach;		// 2026-10-29: a ray's reach at roughness 1, as a share of Distance (0 = full reach)
		float	RtRoughShare;		// 2026-10-29: above this roughness, 1 ray per 2x2 pixels a frame (0 = every pixel)
		float	RtContactSharpness;	// 2026-10-29: the blur radius share at contact (1 = off: the blur as before)
		float	RtContactSpread;	// 2026-10-29: a hit this share of the eye's distance from the surface counts as half 'far'
		float	RtGlowBrightness;	// 2026-10-29: glowing hits (the core's emissive flag) add their colour x this as light
		bool	RtHitAmbientGrid;	// 2026-10-30: a hit's ambient from the light grid at the hit (AmbientLight's), else the cube near the camera
		bool	RtRayReuse;			// 2026-10-31: rough pixels reuse their neighbours' ray hits as samples of their own lobe (stochastic SSR resolve)
		float	RtReuseRoughness;	// 2026-10-31: from this roughness on (RayReuse)
		bool	RtShadeOffscreen;	// 2026-11-05: a hit out of view is shaded from itself (texture, normal, sun, lamps), not taken from the surroundings cube
		float	RtWeapon;			// 2026-10-30: the first-person weapon reflects the RT scene, this strong (0 off)
		bool	RtWater;			// 2026-10-31: water pixels get RT rays; the water shaders read them in place of the planar map
		bool	RtSkyPanorama;		// 2026-10-31: a sky panorama gathered from the screen's sky pixels; RT misses (and the sky fallback) take it
		bool	RtWaterSkipPlanar;	// 2026-10-31: with Water, the game's planar water reflection pass is not drawn while RT holds the water
		float	RtWaterRoughness;	// 2026-10-31: the water's roughness for its rays
		float	RtWeaponRoughness;	// 2026-10-30: the weapon's roughness where no material data says (MaterialAware / MaterialBuffer)
		float	RtWeaponMetal;		// 2026-10-30: the weapon's metal guessed from its colour (grey = metal) x this, without material data
		bool	RtHitTextures;		// 2026-10-29: with the core's [Main.Main.RayTracing] HitTextures, hits take the texture's colour (else the draw's average)
		// UNOFFICIAL 2026-11-07 the RT reflection denoiser (core/RtReflDenoise.h, docs/next/RT-REFLECTION-DENOISER.md): in place of Temporal +
		// Blur X / Y on frames the RT trace runs (native frames, not under DLSS RR); false: the old path exactly as before
		bool	RtDenoiser;
		int		RtDenoiserCore;		// 0 ours (ReBLUR-style), 1 AMD FidelityFX Reflections (ported; A/B)
		float	RtDenoiserHistory;	// frames of history at most (rough surfaces)
		float	RtDenoiserMirrorHistory;	// frames of history at the mirror limit (growing to History by roughness 0.25)
		float	RtDenoiserBlur;		// ours: the blur's widest reach (grid pixels)
		float	RtDenoiserAntiLag;	// ours: history further than this many sigmas from this frame's neighbourhood is cut
		float	RtDenoiserClip;		// FFX: its temporal stability (history clip weight)
		int		RtDenoiserDebugView;	// ours: 1 raw, 2 accumulated (no blur), 3 history length, 4 blur radius, 5 hit-point share, 6 hit distance
	};
	SettingsStruct	Settings;

	struct ConstantsStruct {
		D3DXVECTOR4	Data;		// x: dry reflectivity now, y: wetness now, z: dry roughness, w: wet roughness
		D3DXVECTOR4	Data2;		// x: ray length, y: steps, z: thickness scale, w: noise frame offset
		D3DXVECTOR4	Data3;		// x: history weight this frame, y: blur radius, z: debug view, w: sky fallback
		D3DXVECTOR4	Data4;		// x: draw distance
		D3DXVECTOR4	Layout;		// xy: one half-resolution texel in screen UV
		D3DXMATRIX	Reproject;	// camera-relative world -> last frame's clip space
		D3DXVECTOR4	Cube;		// x: face being drawn, y: its size, z: mip level, w: 1 = empty the cube
		D3DXVECTOR4	Cube2;		// x: capture blend, y: fade per frame, z: last mip level, w: 1 = the cube is in use
		D3DXVECTOR4	Grid;		// xyz: the light grid's corner relative to the camera, w: cell size
		D3DXVECTOR4	Grid2;		// x: 1 = the grid is in use, y: brightness, z: 1 = it carries colours
		D3DXVECTOR4	Grid3;		// rgb: the light every surface gets here (AmbientLight's, linear)
		D3DXVECTOR4	Material;	// UNOFFICIAL 2026-10-20 x: 1 = the material buffer is read, y: MaterialMaxRoughness, z: MaterialReflect
		D3DXVECTOR4	WaterData;		// UNOFFICIAL 2026-10-31 RT water (TESR_RTWaterData, the water shaders): x 1 = last frame's RT reflection
									// holds the water, y 1 = the planar pass is skipped this frame, z its strength, w (2026-11-03) the
									// traced frame's RenderScale (x; 1 native): Buffer's drawn part, WaterReproject is pre-scaled into it
		D3DXMATRIX	WaterReproject;	// TESR_RTWaterReproject: camera-relative world now -> the clip space of the frame that traced it
	};
	ConstantsStruct	Constants;
	IDirect3DBaseTexture9*	GridLightSlot = nullptr;	// AmbientLight's finished grid this frame (TextureManager slots)
	IDirect3DBaseTexture9*	GridColorSlot = nullptr;

	// The surroundings cube: two capture cubes (this frame's, last frame's) and the blurred reflection cube with mips.
	static const UINT CubeSize = 64;
	static const UINT CubeLevels = 7;	// 64 .. 1
	struct CubesStruct {
		IDirect3DCubeTexture9*	Capture[2] = { nullptr, nullptr };
		IDirect3DSurface9*		CaptureFace[2][6] = {};
		IDirect3DCubeTexture9*	Env = nullptr;
		IDirect3DSurface9*		EnvFace[CubeLevels][6] = {};
		IDirect3DBaseTexture9*	CaptureLastSlot = nullptr;	// what the effect's samplers read (TextureManager slots)
		IDirect3DBaseTexture9*	CaptureNewSlot = nullptr;
		IDirect3DBaseTexture9*	EnvSlot = nullptr;
	};
	CubesStruct	Cubes;

	struct TexturesStruct {
		IDirect3DTexture9* TraceTexture = nullptr;
		IDirect3DSurface9* TraceSurface = nullptr;
		IDirect3DTexture9* BufferTexture = nullptr;
		IDirect3DSurface9* BufferSurface = nullptr;
		IDirect3DTexture9* TempTexture = nullptr;
		IDirect3DSurface9* TempSurface = nullptr;
		IDirect3DTexture9* HistoryTexture = nullptr;
		IDirect3DSurface9* HistorySurface = nullptr;
		IDirect3DTexture9* MaterialTexture = nullptr;	// UNOFFICIAL 2026-10-20 MaterialAware: full resolution, made only when compiled in
		IDirect3DSurface9* MaterialSurface = nullptr;
	};
	TexturesStruct	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	void	RenderBuffers(IDirect3DDevice9* Device);	// trace, history, blur: everything before the composite
	void	UpdateWaterConstants();						// UNOFFICIAL 2026-10-31 RT water: every frame, the effect on or off (ShaderManager)
	bool	WaterSkipsPlanar() const { return Constants.WaterData.y > 0.5f; }	// the planar water reflection pass is skipped this frame
	bool	CubeWanted();								// the cube is kept up even when no floor reflects (metal, env maps)
	void	RenderCubeOnly(IDirect3DDevice9* Device);	// the cube alone, on a frame RenderBuffers does not run
	bool	CubeReady() const;							// the cube was kept up last frame (what the object shaders read now)
	void	CaptureMaterial(IDirect3DDevice9* Device);	// UNOFFICIAL 2026-10-20 MaterialAware: the scene's alpha into the material buffer, before any effect draws
	void	BindMaterial();								// the material buffer on stage 15 for the next passes (ClearPassTextures unbinds it)

	// UNOFFICIAL 2026-10-25 (NVR DXVK): every technique as src\hlsl\NewVegas\Compute\Reflections*.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	// UNOFFICIAL 2026-10-26 (NVR DXVK, core/HiZ.h): HiZ on, outdoors (or HiZInteriorsTest), its pass usable (ShaderManager builds the pyramid then).
	bool	HiZWanted() const;

private:
	ExtPass				extPasses[8];		// by technique (Blur X and Blur Y share [2])
	ExtPass				extTraceHiZ;		// UNOFFICIAL 2026-10-26: Compute\ReflectionsTraceHiZ.spv (HiZ)
	bool				hizTraceFailed = false;	// its pipeline could not be made: the normal trace for the session
	UINT				hizTraceFrames = 0;
	ExtPass				extTraceHiZCount;	// UNOFFICIAL 2026-10-26 debugging tool: Compute\ReflectionsTraceHiZCount.spv (the trace + shader counters,
											// [Main.Develop.ShaderCounters] ReflectionsHiZ; core/ShaderCounters.h). Failed: the normal Hi-Z trace.
	// UNOFFICIAL 2026-10-28 RT reflections ([Shaders.Reflections.RT], Compute\ReflectionsRT.glsl): the trace as RayGen (technique 8,
	// Compute\ReflectionsRayGen.spv) -> the RT core's helper (Compute\Rt\ReflectionsRT.spv) -> Resolve (technique 9, ReflectionsRTResolve.spv).
	ExtPass				extRtPasses[2];		// RayGen, Resolve
	bool				rtTraceFailed = false;	// a pipeline or an image could not be made: the normal trace for the session
	UINT				rtTraceFrames = 0, rtFallbackFrames = 0;	// for the log: frames traced with RT / RT wanted but the helper had nothing
	ExtUniformBuffer	extRtUniforms;		// the RtConstants block
public:	// (the RT core user in Reflections.cpp reads the images)
	// UNOFFICIAL 2026-11-02 (the reference path tracer, core/RtPathTrace.cpp): the sky panorama written last (SkyPanorama), null when it
	// was not written in the last few frames (Reflections off, indoors, SkyPanorama off)
	IDirect3DTexture9*	SkyPanoramaTexture(UInt32 frame, UInt32 maxAge) const { return pano[panoIndex] && panoFrame + maxAge >= frame ? pano[panoIndex] : nullptr; }
	struct RtImagesStruct {					// half resolution, made on first use: march, hit A16B16G16R16F; dir, ray G32R32F
		IDirect3DTexture9* March = nullptr;	IDirect3DSurface9* MarchSurface = nullptr;
		IDirect3DTexture9* Dir = nullptr;	IDirect3DSurface9* DirSurface = nullptr;
		IDirect3DTexture9* Ray = nullptr;	IDirect3DSurface9* RaySurface = nullptr;
		IDirect3DTexture9* Hit = nullptr;	IDirect3DSurface9* HitSurface = nullptr;	// written by the helper (shared back by the RT core)
		IDirect3DTexture9* Shade = nullptr;	IDirect3DSurface9* ShadeSurface = nullptr;	// the same: the hit's albedo and sun visibility
		IDirect3DTexture9* Spread = nullptr;	IDirect3DSurface9* SpreadSurface = nullptr;	// Resolve: x the hit-distance share for the blur, y (2026-11-07) the hit distance (G16R16F)
		IDirect3DTexture9* MarchHit = nullptr;	IDirect3DSurface9* MarchHitSurface = nullptr;	// UNOFFICIAL 2026-11-07 RayGen: the march's hit distance (R16F)
		IDirect3DTexture9* Bounce = nullptr;	IDirect3DSurface9* BounceSurface = nullptr;	// UNOFFICIAL 2026-11-07 (perf-reflfix) the helper: MetalBounce's tint + distance
	};
	RtImagesStruct		Rt;
	void				RtRegister();		// its RT core user (ShaderManager, once, before the core's first frame)
	std::string			RtStatusText() const;	// UNOFFICIAL 2026-11-01: the RT trace's state as of its last frame, for the REPORT block
	bool				NothingReflects() const { return nothingReflects; }	// UNOFFICIAL 2026-11-05: ShouldRender found no reflecting surface (the Ray tracing page)
private:
	bool				nothingReflects = false;	// dry, GroundSheen / InteriorFloors 0, no material reflections (ShouldRender, last asked)
	bool				PuddlesActive = false;	// 2026-11-08 Puddles: standing water left outdoors (the passes run for it after the rain)
	bool				rtRays = false;		// this frame's RayGen casts rays (the helper is up; false in SplitTest)
	float				WeaponFovScale() const;	// UNOFFICIAL 2026-10-30
	bool				weaponFrame = false;	// UNOFFICIAL 2026-10-30: this frame's RT trace takes the first-person weapon too ([Shaders.Reflections.RT] Weapon)
	bool				waterFrame = false;		// UNOFFICIAL 2026-10-31: this frame's RT trace takes the water too ([Shaders.Reflections.RT] Water)
	// UNOFFICIAL 2026-10-31 SkyPanorama (ReflectionsSkyPano.frag): two 128 x 128 maps, last frame's and this frame's.
	IDirect3DTexture9*	pano[2] = {};
	IDirect3DSurface9*	panoSurface[2] = {};
	int					panoIndex = 0;			// the one written last
	UINT				panoFrame = 0;			// the FrameNumber it was written in
	bool				panoReady = false;		// written this frame: the passes after it read it
	bool				panoDrawing = false;	// inside its own pass (technique 10)
	bool				panoFailed = false;
	ExtPass				extPanoPass;
	void				RenderSkyPano(IDirect3DDevice9* Device);
	UINT				rtWaterFrame = 0;		// the FrameNumber whose Buffer holds the water's RT reflection (0 none)
	D3DXMATRIX			rtWaterView, rtWaterInvView, rtWaterProj;	// that frame's camera
	float				rtWaterScaleX = 1.0f, rtWaterScaleY = 1.0f;	// UNOFFICIAL 2026-11-03 that frame's RenderScale rect (1: native)
	bool				rtResolved = false;	// Resolve drew this frame (the blur passes read Rt.Spread)
	bool				rtDenoiseFrame = false;	// UNOFFICIAL 2026-11-07: this frame's RT trace feeds the RT reflection denoiser (RayGen / Resolve: rt5.x)
	bool				RunRtDenoiser(IDirect3DDevice9* Device, IDirect3DTexture9* signal, const D3DXMATRIX* reproject);	// false: not run (the old path then)
	void				RtDenoiseIdle();
	bool				RtWanted() const;	// RT on, nothing failed, the RT core up (its helper alive, RT on that GPU)
	bool				RtImages();			// makes Rt on first use
	float				RtSunAmount() const;	// the sun's share for the hits (0 at night and indoors)
	bool				RtTrace(IDirect3DDevice9* Device, IDirect3DSurface9* target);	// RayGen, the helper, Resolve into target; false: draw the normal trace
	ExtUniformBuffer	extUniforms;
	D3DXVECTOR4			extSky = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// NVR_ReflectionsSky as RenderBuffers set it
	IDirect3DTexture9*	extSkyTexture = nullptr;	// the sky visibility it bound on stage 14 (null: none)
	D3DXMATRIX	lastView, lastInvView, lastProj;
	UINT		lastRenderedFrame = 0;	// TheFrameRateManager->FrameNumber of the last frame this effect drew (UNOFFICIAL 2026-10-18: it counted
										// UpdateConstants calls, two a frame, so last frame's history and cube were never kept)
	bool		haveHistory = false;
	TemporalHistoryStats	stats;		// the trace's history use, for the log
	UINT		cubeSteps = 0, cubeEmptied = 0;	// for the log: cube updates, and how many of them started from an empty cube
	void		ReportHistory(const char* when);
	int			captureIndex = 0;		// Cubes.Capture[captureIndex] holds the latest capture
	bool		cubeValid = false;
	void*		cubeCell = nullptr;		// the cell the cube was captured in
	UINT		cubeFrame = 0;			// FrameNumber of the last cube update
	D3DXMATRIX	cubeInvView;			// the camera at the last cube update (for the jump test)
	bool		cubeOnly = false;		// RenderCubeOnly is drawing (ShouldRender lets its passes through)
	bool		materialOnly = false;	// CaptureMaterial is drawing
	bool		materialFilled = false;	// the material buffer was filled this frame
	UINT		gridFrames = 0, gridUsedFrames = 0;	// for the log: frames drawn with GridFallback on, and of them with a grid to read
	void	GridStep();							// the light grid's constants and textures for this frame's trace
	void	CubeStep(IDirect3DDevice9* Device);
	void	RenderCube(IDirect3DDevice9* Device, bool reset);
};
