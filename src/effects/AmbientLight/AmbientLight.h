#pragma once

#include "../../core/NvrDxvkExt.h"

// UNOFFICIAL 2026-10-14: ambient occlusion and bounce light in one pass (Effects\AmbientLight.fx.hlsl). Off by default
// ([Shaders.AmbientLight.Status] Enabled); while it renders, ShaderManager skips the AmbientOcclusion and BounceLight effects
// (it replaces both). Four half-resolution passes into the buffers below, then the apply in the frame chain.
class AmbientLightEffect : public EffectRecord
{
public:
	AmbientLightEffect() : EffectRecord("AmbientLight") {};

	struct ConstantsStruct {
		D3DXVECTOR4	Data;	// x: radius, y: occlusion strength, z: bounce strength, w: draw distance
		D3DXVECTOR4	Data2;	// x: darkest occlusion, y: brightness above which it fades, z: blur radius, w: debug view
	};
	ConstantsStruct	Constants = {};

	IDirect3DTexture9*	prepTexture = nullptr;		// depth, normal and colour per half-resolution pixel (A32B32G32R32F)
	IDirect3DSurface9*	prepSurface = nullptr;
	IDirect3DTexture9*	bufferTexture[2] = {};		// rgb bounce, a visibility (A16B16G16R16F), ping-pong for the blur
	IDirect3DSurface9*	bufferSurface[2] = {};
	bool				buffersTried = false;
	CachedHandle		paramLayout, paramFlags, paramTemporal, paramReproject, paramReach;
	CachedHandle		paramSunFarFlags;	// UNOFFICIAL 2026-10-19 SunFar (NVR_SunFarFlags)
	CachedHandle		paramGI;			// UNOFFICIAL 2026-11-03 NVR_ALGI: the RT bounce light in the D3D9 passes (Render)
	CachedHandle		paramScene;					// UNOFFICIAL 2026-10-18 (ambfix): NVR_ALScene, the scene's own light
	CachedHandle		techSky;					// UNOFFICIAL 2026-10-20 (skyamb) Skylighting: technique AmbientLightSky
	D3DXVECTOR4			sceneLight = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f);	// the light every surface gets (linear; Render)
	// UNOFFICIAL 2026-10-14 Temporal: the bounce kept from frame to frame (rgb bounce, a view depth; A16B16G16R16F), one read
	// while the other is written, and last frame's camera to find each spot in it.
	IDirect3DTexture9*	historyTexture[2] = {};
	IDirect3DSurface9*	historySurface[2] = {};
	IDirect3DTexture9*	occlusionHistory[2] = {};		// UNOFFICIAL 2026-10-14: the occlusion's (r visibility, g view depth; A16B16G16R16F like the bounce history: one MRT bit depth)
	IDirect3DSurface9*	occlusionHistorySurface[2] = {};
	int					historyIndex = 0;		// the one holding last frame's
	bool				historyValid = false;
	bool				exteriorBalance = true;	// UNOFFICIAL 2026-11-11 (sungi) [Shaders.AmbientLight.Main] ExteriorBalance (Apply)
	bool				giDenoisedNoBlur = true;	// UNOFFICIAL 2026-11-13 (rtgi-review R1) [Shaders.AmbientLight.Main] GIDenoisedNoBlur (Blur)
	bool				giSingleHistory = true;	// UNOFFICIAL 2026-11-11 (sungi) [Shaders.AmbientLight.Main] GISingleHistory (GI accumulate)
	float				ambientReplace = 0.0f;	// UNOFFICIAL 2026-11-11 (sungi p2) [Shaders.AmbientLight.Main] AmbientReplace (0-1; 0 = off)
	float				ambientFloor = 0.15f;	// UNOFFICIAL 2026-11-11 (sungi p2) [Shaders.AmbientLight.Main] AmbientFloor (0-1)
	// UNOFFICIAL 2026-11-12 (sungi p4) SunGI ([Shaders.AmbientLight.Main] SunGI, in memory only): UpdateSettings keeps the toml's keys apart from the values in use
	// (gridBounceKey / ambientReplaceKey; ambientReplace and gridBounce are the values in use) and SunGIFrame, once a frame, applies the parts that depend on
	// the RT core running (the grid fallback) and on being outdoors; sunGiForced = SunGI has Status Enabled on (the user's switch is off): ShouldRender then
	// keeps the effect off indoors.
	bool				sunGi = false, sunGiForced = false, settingsOutdoors = true;
	float				gridBounceKey = 0.0f, ambientReplaceKey = 0.0f, ambientReplaceSunGI = 0.8f;
	int					sunGiState = 0;
	void				SunGIFrame();
	bool				AmbientReplaceActive() const;	// UNOFFICIAL 2026-11-11 (sungi p2) outdoors and AmbientReplace > 0 (AmbientOcclusion forces the sky-miss weight)
	bool				temporal = true;
	D3DXMATRIX			lastView, lastInvView, lastProj;
	double				lastTime = 0.0;
	unsigned			frameNumber = 0;

	// UNOFFICIAL 2026-10-14 light grid (GridBounce; AmbientLight.fx.hlsl GridInject): the sun's bounce light in a 32 x 32 x 16
	// grid of cells round the player, from a small depth map of the world seen from the sun (drawn by ShadowManager).
	static const int	GridSunMapSize = 256;
	static const int	GridAtlasSize = 128;	// 4 x 4 tiles of 32 x 32 cells
	static const int	GridAtlasWidth = 256;	// UNOFFICIAL 2026-10-22 (lightgrid2): two levels side by side (coarse, fine; GridFine)
	bool				gridFine = true;		// GridFine: the fine level (a quarter the cell size) round the camera, outdoors with the sun map
	double				gridPendingOrigin1[3] = {};	// the fine level's corner for the grid being made (absolute world)
	double				gridOrigin1[3] = {};		// ... and for the finished grid
	bool				gridFineValid = false;		// the finished grid has a fine level
	CachedHandle		paramGridOrigin1;
	float				gridBounce = 0.0f;		// strength (0: off, nothing made or drawn)
	float				gridCell = 200.0f;		// cell size, world units
	int					gridSteps = 8;			// spreading steps
	float				gridSpread = 0.3f;		// share of a neighbour's light passed on per step
	float				gridBlocking = 1.0f;	// how much the surfaces in a cell stop light crossing it
	IDirect3DTexture9*	gridGeometry = nullptr;	// per cell: the surfaces' average normal and cover (blocking; A16B16G16R16F)
	IDirect3DSurface9*	gridGeometrySurface = nullptr;
	CachedHandle		paramGridStep, paramGridShift, paramGridScreen;
	float				gridScreenStrength = 6.0f;	// GridScreenStrength
	float				gridColour = 0.25f;			// UNOFFICIAL 2026-10-18 (ambfix) GridColour: share of the grid light's own colour kept
	// UNOFFICIAL 2026-10-18 (ambfix2) indoor balance (InteriorBalance, InteriorBounceShare; passes 5-6 of AmbientLight.fx.hlsl).
	static const int	StatsWidth = 16, StatsHeight = 9;	// 2 x 2 samples per texel (StatsSample)
	bool				interiorBalance = true;
	float				interiorBounceShare = 1.0f;
	// UNOFFICIAL 2026-10-21 (aloutline) NoEdgeOutline: the busy-depth rule (Apply) needs a bend at three pixels in a row, so the edges
	// of things no longer count as grass (NVR_ALFlags.y).
	bool				noEdgeOutline = true;
	bool				edgeReject = true;			// UNOFFICIAL 2026-11-01 EdgeReject (Apply's upsample)
	bool				temporalJitterFix = true;	// UNOFFICIAL 2026-11-01 TemporalJitterFix (the history read where the jitter's change puts it)
	bool				minReach = true;			// UNOFFICIAL 2026-11-01 MinReach (Estimate's reads at least minReachTexels texels out)
	float				minReachTexels = 0.75f;		// UNOFFICIAL 2026-11-02 MinReachTexels
	D3DXVECTOR2			lastDepthJitter = D3DXVECTOR2(0.0f, 0.0f);
	D3DXVECTOR2			jitterDelta = D3DXVECTOR2(0.0f, 0.0f);
	bool				fullResBuffers = false;		// UNOFFICIAL 2026-11-01 LowScaleFullRes: the buffers were made at the full screen size
	// UNOFFICIAL 2026-10-31 AlbedoBuffer ([Shaders.AmbientLight.Main] AlbedoBuffer, live): the bounce's surface colour from TESR_AlbedoBuffer where written
	bool				albedoBuffer = false;
	float				albedoFallbackCap = 0.12f;	// UNOFFICIAL 2026-11-10 AlbedoFallbackCap: the guessed colour's cap where the albedo buffer has no texel (0 = the old 0.6)
	bool				albedoLogged = false;
	bool				giD3D9Logged = false;	// UNOFFICIAL 2026-11-03 RT GI on the D3D9 passes: logged once
	// UNOFFICIAL 2026-10-21 (alrocks; the occlusion cut off hard along low-poly rocks' triangle edges): NormalSmoothing (half-resolution
	// texels the normal is averaged over after Prepare, 0 off) and HorizonBias (degrees above the tangent plane each horizon starts, 0 off); NVR_ALEdges.
	int					normalSmoothing = 36;
	// UNOFFICIAL 2026-10-24 (charfacets; owner: indoors every triangle of a character showed, flat, hard-edged, even with SmoothObjectNormals):
	// NormalSmoothingWorld, the smoothing's reach in WORLD units, used where it is wider than NormalSmoothing's texels (close to the camera,
	// where a character's triangles are 100-300 pixels across and the pixel reach covered only a band at each edge). NVR_ALEdges.z, 0 off.
	float				normalSmoothingWorld = 0.0f;
	float				horizonBias = 0.0f;
	// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz; core/HiZ.h) HiZ: 0 off; N > 0: Estimate's horizon taps at least 2^(N+1) prep texels
	// away read a coarse level of the Hi-Z pyramid (Compute\AmbientLightEstimateHiZ.frag). Vulkan passes only.
	int					hizLogQ = 0;
	bool				hizEstimateFailed = false;	// its shader file is missing: the normal Estimate for the session
	bool				hizEstimateChecked = false;
	UINT				hizEstimateFrames = 0;
	bool	HiZWanted() const { return false; }	// retired 2026-10-26 (measured no win; see AmbientLight.cpp Estimate)
	CachedHandle		paramEdges, techSmooth;
	bool				prepared = false;			// RenderPrepare drew Prepare + the smoothing this frame (Render skips its Prepare)
	bool	RenderPrepare(IDirect3DDevice9* Device);	// ShaderManager, before the AO's sky pass: Prepare, then technique AmbientLightSmooth
	IDirect3DTexture9* SmoothedNormals() const { return prepared ? prepTexture : nullptr; }
	// UNOFFICIAL 2026-10-25 (NVR DXVK) [Main.Main.Performance] AmbientLightSmoothCompute: technique AmbientLightSmooth's two passes as
	// one compute dispatch (src\hlsl\NewVegas\Compute\AmbientLightSmooth.comp) into smoothResult, copied into the prep. False = not
	// done (off, no extension, reach above SmoothComputeMaxReach, or failed: logged once, the D3D9 passes for the session).
	static const int	SmoothComputeMaxReach = 64;	// the shader's shared memory holds pass X for 256 rows = a tile + 2 x the reach
	ID3D9NvrExtPipeline*	smoothPipeline = nullptr;
	ID3D9NvrExtBuffer*		smoothResult = nullptr;	// width x height RGBA32F (the prep's format)
	UINT				smoothWidth = 0, smoothHeight = 0;
	bool				smoothComputeFailed = false;
	bool				smoothComputeReachLogged = false;
	UINT				smoothComputeFrames = 0;
	bool	SmoothCompute(IDirect3DDevice9* Device, IDirect3DSurface9* Target, UINT Width, UINT Height);
	void	SmoothComputeRelease();
	IDirect3DTexture9*	statsTexture = nullptr;			// per sample: light, the occlusion's darkening, the bounce (luma)
	IDirect3DSurface9*	statsSurface = nullptr;
	IDirect3DTexture9*	balanceTexture[2] = {};		// x: the bounce's scale (eased), yzw: the averages
	IDirect3DSurface9*	balanceSurface[2] = {};
	int					balanceIndex = 0;			// the one holding last frame's
	bool				balanceValid = false;
	CachedHandle		paramInterior;
	// UNOFFICIAL 2026-10-14 screen memory (indoors, at night): per cell the light its surfaces showed on screen (ScreenInject).
	IDirect3DTexture9*	gridScreen[2] = {};			// harmonics (A16B16G16R16F, the atlas), ping-pong
	IDirect3DSurface9*	gridScreenSurface[2] = {};
	IDirect3DTexture9*	gridScreenColor[2] = {};	// colour x light
	IDirect3DSurface9*	gridScreenColorSurface[2] = {};
	// UNOFFICIAL 2026-10-22 (lightgrid2) GridWallMemory: per cell the cover of its surfaces per axis, from the screen memory's reads
	// (blocking indoors and at night, where the sun map gives none). Ping-pong with gridScreen (same index).
	bool				gridWallMemory = true;
	IDirect3DTexture9*	gridScreenWalls[2] = {};	// A16B16G16R16F, the atlas
	IDirect3DSurface9*	gridScreenWallsSurface[2] = {};
	bool				gridWallsValid = false;		// the memory at gridScreenIndex holds walls
	CachedHandle		paramGridWalls;
	// UNOFFICIAL 2026-10-22 (lightgrid2) GridLamps: the game's lamps light the grid's surfaces (indoors and at night).
	static const int	GridLampsMax = 32;			// AmbientLight.fx.hlsl GridLampsMax
	float				gridLamps = 4.0f;			// strength (0: off)
	CachedHandle		paramGridLamps, paramGridLampColors, paramGridLampInfo;
	int		SetGridLamps();						// fills NVR_GridLamps*; returns how many
	int					gridScreenIndex = 0;
	bool				gridScreenValid = false;
	double				gridScreenOrigin[3] = {};	// the grid corner the memory was stored for
	bool				gridSunActive = false;		// the sun map feeds the grid now (outdoors, sun up; set by WantsGridSunMap)
	bool	GridMoveOrDue();						// works the grid's corner out; true when it is time to update
	IDirect3DTexture9*	gridSunTexture = nullptr;	// R32F depth from the sun
	IDirect3DSurface9*	gridSunSurface = nullptr;
	IDirect3DSurface9*	gridSunDepth = nullptr;
	IDirect3DTexture9*	gridSunColorTexture = nullptr;	// A8R8G8B8: the sunlit surfaces' own colours (ShadowGridColor.pso)
	IDirect3DSurface9*	gridSunColorSurface = nullptr;
	bool				gridSunHasColor = false;
	bool				gridColorValid = false;			// the finished grid carries colours
	IDirect3DTexture9*	gridWaveColor[2] = {};			// rgb: colour x light, a: light (each step, as gridWave)
	IDirect3DSurface9*	gridWaveColorSurface[2] = {};
	IDirect3DTexture9*	gridAccumColor[2] = {};			// the same over all steps
	IDirect3DSurface9*	gridAccumColorSurface[2] = {};
	IDirect3DTexture9*	gridWave[2] = {};			// this step's light (A16B16G16R16F, the atlas)
	IDirect3DSurface9*	gridWaveSurface[2] = {};
	IDirect3DTexture9*	gridAccum[2] = {};			// the light over all steps so far
	IDirect3DSurface9*	gridAccumSurface[2] = {};
	int					gridAccumIndex = 0;			// the one holding the finished grid
	bool				gridBuffersTried = false;
	bool				gridSunFresh = false;		// the sun map was drawn this frame: inject and spread again
	bool				gridValid = false;			// a finished grid exists (for gridOrigin)
	double				gridOrigin[3] = {};			// the finished grid's corner (absolute world)
	double				gridPendingOrigin[3] = {};	// the corner of the grid the sun map just drawn is for
	unsigned			gridFrame = 0;
	UINT				gridRenderedFrame = 0;		// UNOFFICIAL 2026-10-19: FrameNumber of the last finished grid (Reflections' GridFallback)
	float				gridSunTexel = 1.0f;		// world units per sun map texel
	D3DXMATRIX			gridSunToGrid, gridToSun;
	ShadowsExteriorEffect::ShadowMapSettings gridSunMap = {};
	CachedHandle		paramGridOrigin, paramGridData, paramGridLight, paramGridSunToGrid, paramGridToSun;

	void	EnsureGridBuffers();
	bool	WantsGridSunMap(const D3DXVECTOR3& sunDir);		// ShadowManager: draw the sun map this frame?
	D3DXMATRIX GridSunViewProj(const D3DXVECTOR3& sunDir);	// sets gridSunMap up for it and returns its view-projection
	void	RenderGrid(IDirect3DDevice9* Device, bool sun, bool screen);
	// UNOFFICIAL 2026-10-14 (set by ShaderManager for one Render): apply the sun shadows in this effect's passes, their own pass
	// skipped; shadowApplied reports that it was done.
	bool				foldShadow = false;
	bool				shadowApplied = false;

	// UNOFFICIAL 2026-10-25 (NVR DXVK, core\DxvkCompute.h) [Main.Main.Performance] AmbientLightBlurCompute: passes BlurX and BlurY as
	// one compute dispatch (src\hlsl\NewVegas\Compute\AmbientLightBlur.comp), the same result, written into bufferTexture[1] (the
	// D3D9 BlurY writes bufferTexture[0], which the dispatch reads); the passes after it read it there. False = not done (off, no
	// extension, or a failure: logged once, then the D3D9 passes for the session).
	bool	RenderBlurCompute(const D3DXVECTOR4& layout, UINT width, UINT height);
	ID3D9NvrExtPipeline*	blurPipeline = nullptr;
	bool				blurComputeTried = false;
	bool				blurComputeOk = false;
	UINT				blurComputeFrames = 0;

	// UNOFFICIAL 2026-10-25 (NVR DXVK, core\DxvkCompute.h) [Main.Main.Performance] NvrDxvkPasses: every pass of techniques AmbientLight /
	// AmbientLightSky, AmbientLightGrid and AmbientLightSmooth as a Vulkan pass (src\hlsl\NewVegas\Compute\AmbientLight*.frag, AmbientLight.glsl),
	// drawn where Render / RenderGrid / RenderPrepare draw the D3D9 pass, into the same targets with the same textures; the constants are
	// the effect's own parameter values, read back right before each draw. ExtDraw false (off, no extension, a texture missing, a failure:
	// ExtFailed, logged once) = that pass's D3D9 draw. AmbientLightBlurCompute / AmbientLightSmoothCompute keep their passes when on.
	enum ExtShader { ExtPrepare, ExtSmooth, ExtEstimate, ExtBlur, ExtAccumulate, ExtStats, ExtReduce, ExtApply, ExtScreen, ExtInject, ExtPropagate, ExtEstimateHiZ,
		ExtGILight, ExtGIEstimate0, ExtGIEstimate1, ExtGIEstimate2, ExtGIAccumulate, ExtGIApply,
		ExtEstimateRT, ExtShaderCount };	// ExtGI*: ComputeGI (below); ExtEstimateRT: UNOFFICIAL 2026-10-28 (fork RT) [Shaders.AmbientOcclusion.RT]
	ExtPass				extPasses[ExtShaderCount][NVR_EXT_MAX_TARGETS];	// per target count
	ExtUniformBuffer	extUniforms;
	CachedHandle		extHandles[38];
	bool	ExtOn() const;
	bool	ExtOnRect() const;	// UNOFFICIAL 2026-11-03: ExtOn's rule in a RenderScale rect frame (the RT estimate only)
	bool	ExtDraw(ExtShader Shader, IDirect3DSurface9* const* Targets, UINT TargetCount, IDirect3DBaseTexture9* const* Textures, UINT Variant, const D3DVIEWPORT9& Viewport);

	// UNOFFICIAL 2026-10-27 (NVR DXVK, deep/fork2-ssgi) [Shaders.AmbientLight.Main] ComputeGI: needs NVR DXVK with NvrDxvkPasses on (does
	// nothing on native D3D9 or stock DXVK; off = exactly the old passes). Before Estimate, Compute\AmbientLightGILight.frag writes the
	// frame's light per prep texel into giLightTexture (A8R8G8B8, sqrt); Estimate is then its own shader compiled with NVR_GI
	// (AmbientLightEstimate-gi0/1/2.spv): the same GTAO, the same taps, plus a visibility bitmask per slice whose newly covered sectors add
	// the taps' light (one bounce from what is on screen; ComputeGIThinOcclusion: the occlusion from the bitmask too). After the blurs
	// AmbientLightGIAccumulate.frag keeps the light from frame to frame in the bounce history (historyTexture, unused since the screen bounce
	// was removed) and AmbientLightApply-gi adds it (x the surface colour x ComputeGIStrength). DebugView 13: that light alone. Any failure:
	// logged once, ComputeGI off for the session, the old passes.
	// (v1, 510684a0: a compute prefilter into a storage buffer with 4 levels. In game it changed the occlusion even at strength 0 and its
	// 8-byte taps cost no less than Estimate's: replaced by this.)
	bool				computeGI = false;		// ComputeGI
	float				giStrength = 1.0f;		// ComputeGIStrength
	float				giThickness = 100.0f;	// ComputeGIThickness (world units)
	int					giQuality = 0;			// ComputeGIQuality: 0 = 2 slices x 4 steps (Estimate's 16 taps), 1 = 3 x 6, 2 = 4 x 8
	bool				giThinOcclusion = false;	// ComputeGIThinOcclusion: the occlusion from the bitmask (thickness) instead of Estimate's GTAO
	int					giTest = 0;				// ComputeGITest (test only): 1 = the GI pass alone (plain accumulate and apply), 2 = + its accumulate
	IDirect3DTexture9*	giLightTexture = nullptr;	// the frame's light per prep texel (made the first time ComputeGI runs)
	IDirect3DSurface9*	giLightSurface = nullptr;
	bool				giLightTried = false;
	bool				giFailed = false;
	int					giFiles = -1;			// the .spv files: -1 not looked for yet, 0 missing, 1 there
	int					giReported = -1;		// the last state logged (why it does not run, or running)
	UINT				giFrames = 0;
	bool				giDrawn = false;		// this frame's estimate was the GI pass (the accumulate and apply follow it)
	bool	GIActive();

	void	EnsureBuffers();
	void	UpdateConstants() {};
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
		IDirect3DSurface9* SourceBuffer) override;
};
