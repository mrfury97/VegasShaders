// UNOFFICIAL 2026-11-02 the reference path tracer: RtPathTrace.h; the trace shader is Compute\Rt\RtPathTrace.comp (run by the helper),
// the display pass Compute\RtPathTraceShow.frag (a fork pass in the game).

#include "RtLightGrid.h"	// (2026-11-06) PTLightGrid's builder (shared with tools/nvr-rt-host)
#include "RtRestirRow.h"	// (2026-11-07) PTRestirDI's frame row (shared with tools/nvr-rt-host)
#include "RtTessContact.h"	// (2026-11-07 rtgap-tess) the bumps' slack + PTContactShadows in the frame row (shared with tools/nvr-rt-host)
#include "RtMaterials.h"	// (2026-11-07 ptaaa-materials) glass, foliage, skin, cloth keys in the frame row (shared with tools/nvr-rt-host)
#include "RtEmissiveRow.h"	// (2026-11-07 ptaaa-emissive) PTEmissive's emissive row + remap (shared with tools/nvr-rt-host)
#include "RtPtCost.h"	// (2026-11-07 perf-ptcost) the ray budget keys in the frame row (shared with tools/nvr-rt-host)
#include "RtFlashlightCode.h"	// (2026-11-07 perf-reflfix2) the flashlight's cone code (shared with tools/nvr-rt-host)
#include "RtPathTrace.h"
#include "RtCore.h"
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "RtDenoise.h"
#include "ManyLamps.h"
#include "../effects/PBR/PBR.h"	// (2026-11-04 rtgap) the game's light scale	// (2026-11-03 rtspec) PTGameLamps 4: the game's lamp table
#include "FlashlightDeferred.h"	// (2026-11-07) the flashlight as its own pass draws it (LightParams)
#include "RenderRect.h"	// (2026-11-03) RenderScale < 1: the trace over the frame's top-left rect (docs/next/RT-RENDERSCALE.md)
#include "RtViewModel.h"	// (2026-11-07 ptaaa-firstperson) PTViewModel's prep pass (shared with tools/nvr-rt-host)
#include "CapturedNormals.h"	// (2026-11-07 ptaaa-firstperson) PTViewModel: the first-person model's G-buffer (VmWanted / VmFrame)
#include "Upscaler.h"	// (2026-11-07 ptaaa-denoiser) DLSS RR denoises the raw path traced picture (RayReconstructionWantsRaw)
#include <algorithm>
#include <cfloat>

namespace {
	const char* const kSection = "Main.Main.RayTracing";
	const char* const kUserName = "RT reference path tracer";	// (2026-11-05) RtCore::LeftOut

	struct Settings {
		bool	enabled = false;		// ReferencePT
		int		bounces = 4;			// PTBounces [1-16]
		int		samples = 1;			// PTSamples [1-16] paths a pixel and frame
		int		maxSamples = 4096;		// PTMaxSamples: the mean stops growing (and the helper stops tracing) here
		float	sky = 1.0f;				// PTSky: the sky's light on misses outdoors
		float	ambient = 0.0f;			// PTAmbient: the interior's light on misses (x the game's ambient)
		float	glow = 1.0f;			// PTGlow: glow maps' light (needs GlowTextures)
		float	clamp = 0.0f;			// PTClamp: a path's luminance cap (0 = none: unbiased)
		float	exposure = 1.0f;		// PTExposure (display only, live without a restart of the mean)
		float	split = 0.0f;			// PTSplit: 0 the whole screen, 0.5 the right half (the game's frame on the left)
		int		debugView = 0;			// PTDebugView: 1 albedo, 2 normals, 3 the first hit's direct light only; (2026-11-03) 5 the first hit's arriving indirect light, 6 / 7 RT GI's bounce (raw / denoised) shown the same way;
										// (rtspec) 8 the first hit's direct highlight only, 9 its reflection only
		bool	primary = true;			// PTPrimary (2026-11-02 b): the first hit's colour and normal from the screen's G-buffer
		bool	replace = false;		// PTReplace (2026-11-03): the traced picture INTO the game's HDR scene (the post chain runs on it)
		bool	denoise = false;		// PTDenoise (2026-11-03): interactive: each frame's own paths through the shared RT denoiser, no mean
		bool	specular = true;		// PTSpecular (2026-11-02 e): GGX highlights + reflections (roughness / metal: the MaterialBuffer on screen)
		bool	glowLights = false;		// PTGlowLights (2026-11-03 b): glow-mapped meshes sampled as lights (RtPathTrace.comp GlowSample)
		bool	glowFromFrame = true;	// PTGlowFromFrame (2026-11-05): glowing surfaces the camera sees take the game's pixel
		bool	lightGrid = true;		// PTLightGrid (2026-11-04): the lamps through a world grid (each cell's list), not every lamp at every hit
		int		lightGridMin = 40;		// PTLightGridMinLamps (test only): the grid from this many lamps on
		int		bounceAlpha = 1;		// PTBounceAlpha (2026-11-04 b): bounce rays' alpha test: 1 as the camera's, 0 cut-outs solid, 2 cut-outs left out, 3 the nearest card's texel
		float	bounceDistance = 0.0f;	// PTBounceDistance (2026-11-04 b): bounce rays' length (0 = 60000, as the camera's)
		float	indirectClamp = 0.0f;	// PTIndirectClamp (2026-11-04 c): light through a bounce, luminance cap (0 = none)
		// UNOFFICIAL 2026-11-09 the real-time path tracer's firefly guards (a tester on DLSS Ray Reconstruction: single white pixels over matte
		// surfaces round shiny ones; RR gets the raw paths and the path tracer's own anti-firefly pass steps aside there):
		float	realtimeClamp = 0.0f;	// PTRealtimeClamp: PTIndirectClamp's value under PTRealtime while that key is 0 (0 = none there too)
		float	bounceRoughen = 0.0f;	// PTBounceRoughen: path regularisation (RtPathTrace.comp gBounceRoughen); -1 = 0.35 under PTRealtime, else 0
		int		lastSky = 0;			// (2026-11-06) PTLastSky: the last hit's sky light (flag 32768)
		int		lastBounce = 0;		// (2026-11-06 b) PTLastBounce: the last hit's further bounces, estimated from its own direct light (flag 65536)
		int		lampFalloff = 0;	// (2026-11-06 c) PTLampFalloff: 0 the game's (1 - (d/r)^2)^2.2, 1 physical (windowed inverse square, reach x 2; flag 131072)
		float	lampGain = 1.6f;	// (2026-11-06 c) PTLampGain: the physical falloff's lamp brightness (1 = the game's total light per lamp)
		int		bounceHalf = 0;			// (2026-11-06) PTBounceHalf: -1 auto (on under PTRealtime), 0 off, 1 on
		float	denoiseSigma = 1.0f;	// (2026-11-06) PTDenoiseSigma: the a-trous passes' luminance edge stop (RtDenoise ValueSigma)
		float	denoiseClamp = 0.0f;	// (2026-11-06) PTDenoiseClamp
		int		denoiseIterations = 2;	// PTDenoiseIterations / PTDenoiseHistory (2026-11-04 c, test only): PTDenoise's a-trous passes (2: section 30; was 4), history frames
		float	denoiseHistory = 32.0f;
		float	denoisePeopleHistory = 4.0f;
		float	denoiseAntiLag = 3.0f;	// (2026-11-07) PTDenoiseAntiLag: the fast history's window in standard errors (0 off)
		float	denoiseGuide = 0.35f;	// (2026-11-07) PTDenoiseGuide: the a-trous passes stop at the game's shading edges (0 off)
		// UNOFFICIAL 2026-11-07 (ptaaa-denoiser, RT-PATH-TRACING.md "ptaaa-denoiser"): NRD ReLAX-style split. PTDenoiseSplit: the diffuse and
		// the specular light denoised apart (each divided by its own albedo; the specular one reprojected by its hit distance, PTDenoiseVirtual,
		// mixed with the surface's by roughness; mirrors keep PTDenoiseMirrorHistory frames); its image is made at the helper's start (on there:
		// live off / on; off there: on needs a restart). PTDenoiseObjectMotion: the object motion under the path tracer (moving NPCs do not
		// ghost) with PTDenoiseMovingHistory frames on moving surfaces. PTDenoiseUnderRr 0: while DLSS Ray Reconstruction runs, it gets the
		// raw paths and the path tracer's own albedos (its denoiser steps aside); 1: the path tracer keeps its own denoiser under RR.
		bool	denoiseSplit = true;
		bool	denoiseObjectMotion = true;
		int		denoiseMovingHistory = 8;
		float	denoiseMirrorHistory = 12.0f;
		bool	denoiseVirtual = true;
		float	denoiseMirrorSurface = 0.1f;	// UNOFFICIAL 2026-11-09 (pt-mirrors) PTDenoiseMirrorSurface: smooth metals under this roughness are denoised as the
												// thing they reflect (RtPathTrace.comp gReflAlbedo, RtDenoise Settings.MirrorSurface); 0 = as before
		int		denoiseUnderRr = 0;
		float	denoiseHistoryFix = 3.0f;	// (ptaaa-denoiser) PTDenoiseHistoryFix: RtDenoise Settings.HistoryFix (newly seen pixels: no speckles)
		float	denoiseMeanVariance = 2.0f;	// (ptaaa-denoiser) PTDenoiseMeanVariance: RtDenoise Settings.MeanVariance (fine shadows kept)
		int		denoiseAntiFirefly = -1;	// (ptaaa-denoiser) PTDenoiseAntiFirefly: -1 auto (on under PTRealtime), 0 off, 1 on
		float	denoiseFireflyK = 2.0f;	// PTDenoiseFireflyK (test key): its k
		int		denoiseFireflySpread = 4;	// PTDenoiseFireflySpread (2026-11-09): the most of a clamped firefly that is spread on, x what the pixel supports (0: all of it)
		float	detail = 1.0f;			// (2026-11-07) PTDetail: the game's fine shading on the traced light (0 off, 1 all)
		float	contact = 1.0f;			// (2026-11-07 rtgap-tess) PTContactShadows: the depth buffer's contact march at the first hit (strength, 0 off)
		float	contactLength = 24.0f;	// PTContactLength: its length on screen (pixels)
		float	detailRadius = 4.0f;	// (2026-11-07) PTDetailRadius: its local average's reach (pixels)	// (2026-11-07) PTDenoisePeopleHistory: people's pixels keep at most this many frames (0 off)
		bool	viewModel = true;		// (2026-11-07 ptaaa-firstperson) PTViewModel: the first-person model lit by the path tracer (its G-buffer captured)
		bool	restirGI = false;		// PTRestirGI (2026-11-04 d): ReSTIR GI on the first bounce (with PTDenoise; RtPathTraceGI.spv + RtRestirGI.spv)
		float	restirMaxM = 20.0f, restirSpatial = 4.0f, restirRadius = 16.0f, restirDepth = 0.03f, restirNormal = 0.9f;	// its test keys
		// UNOFFICIAL 2026-11-07 PTRestirDI (RT-PATH-TRACING.md section 47): ReSTIR DI for the lamps at the first hit, reservoirs in the helper's
		// scratch store (protocol 6). -1 auto = on under PTRealtime; its test keys: neighbours, their radius (pixels), the reservoirs' count cap,
		// the normal test (cos) and the plane test (share of the hit's distance)
		int		restirDI = -1;
		int		restirDISpatial = 2;	// (2026-11-07 perf-ptcost: 2, was 4)
		float	restirDIRadius = 16.0f, restirDIMaxM = 20.0f, restirDINormal = 0.9f, restirDIPlane = 0.05f;
		// UNOFFICIAL 2026-11-07 PTRadianceCache (section 48): the world-space radiance cache for the path's last hit (-1 auto: on under PTRealtime);
		// its cell near the camera (units), how many samples deep a cell's mean is, the samples a cell needs before it is read, the frames an
		// unwritten cell lives, log2 of the entries (test key)
		int		radianceCache = -1;
		float	cacheCell = 8.0f, cacheMaxSamples = 256.0f, cacheMinSamples = 8.0f, cacheMaxAge = 120.0f, cacheMaxFrames = 8.0f;
		int		cacheLog2 = 20;
		int		allLamps = -1;		// (2026-11-07) PTAllLamps: -1 auto (on under PTRealtime), 0 the game's lamps of this view, 1 every lamp not hidden
		RtMaterials::Keys materials;	// (2026-11-07 ptaaa-materials) PTGlass, PTGlassTint, PTFoliage, PTSkin, PTClothSheen (RtMaterials.h)
		// UNOFFICIAL 2026-11-07 (ptaaa-emissive) PTEmissive: glowing triangles as lights (RtEmissiveRow.h, RtEmissiveBuild.comp; -1 auto: on under
		// PTRealtime), at most PTEmissiveMax instances and PTEmissiveTriangles triangles in the list a frame (test keys)
		int		emissive = -1;
		int		emissiveMax = 64;
		int		emissiveTriangles = (int)RtEmissiveRow::kTrianglesMax;
		// UNOFFICIAL 2026-11-07 (perf-ptcost, RT-PATH-TRACING.md "perf-ptcost"; RtPtCost.h) the ray budget: PTRestirDIRays 0 = ReSTIR DI's
		// targets unshadowed + one shadow ray for the chosen sample (1 = a ray inside every target, as before; 2 / 3 test variants),
		// PTBounceLights 0 = one light sample a bounce hit (1 = the sun, the lamp and the emissive point each, as before). A missing key: 0
		int		restirDIRays = 0;
		int		bounceLights = 0;
		int		gameLamps = 4;			// PTGameLamps (2026-11-03 rtspec): the lamps as the game lights them: 1 its falloff ((1 - (d/r)^2)^2.2), 2 its colour
										// (colour x dimmer decoded together), 3 both, 4 the game's own lamp table (its lamps, reach and colour) + its falloff,
										// 0 neither (the older guess, as RT GI)
	} S;

	// RtPathTrace.comp's uniform block, std140
	struct PassConstants {
		float size[4], forward[4], right[4], up[4], origin[4], accum[4], sunDir[4], sunColor[4], skyZenith[4], skyHorizon[4], ambient[4], misc[4];
		float lampPos[8][4], lampColor[8][4];
		float rect[4];	// (2026-11-03) xy the depth's jitter (screen uv)
		float grid[3][4];	// (2026-11-04) PTLightGrid: the grid's low corner (scene space) + cells x; 1 / cell size xyz + cells y; cells z, the image's width
	};
	static_assert(sizeof(PassConstants) <= 512, "RtCore: 512 bytes of constants a pass");

	int			imageA = -1, imageB = -1, imageDisplay = -1, shader = -1;
	int			shaderGrid = -1;
	int			shaderCache = -1;	// (2026-11-07) PTRadianceCache's resolve pass (RtPathTraceCache.spv)
	int			cacheLog2Setup = 0;	// (2026-11-07) the cache's entries (log2) the scratch store was sized for at this helper start (0: none)	// (2026-11-04) PTLightGrid's variant (RtPathTraceGrid.spv: RtPathTrace.comp with PT_LIGHT_GRID)
	// UNOFFICIAL 2026-11-04 d PTRestirGI (RT-PATH-TRACING.md section 29): the trace's variant (the first bounce written out), the reuse pass,
	// the result image (the path's other light in, the picture out), the reservoirs (point xy | point z, normal + M | light, W) and the
	// first hits (distance, normal) of this frame and the last (ping-pong), and last frame's camera for the reprojection
	int			shaderGI = -1, shaderReuse = -1, imageGIResult = -1, imageResA[2] = { -1, -1 }, imageResB[2] = { -1, -1 }, imageResC[2] = { -1, -1 }, imageSurf[2] = { -1, -1 };
	UInt32		giFrame = 0;		// ping-pong: this frame writes set giFrame & 1
	bool		giHistory = false;	// last frame ran the reuse at this size (its reservoirs are valid)
	float		giPrevCamera[4][4] = {};	// last frame's forward, right, up (as the pass's), and the camera's WORLD position (2026-11-09: was the
										// scene-space eye, RtCore::Eye() = 0 every frame, so the reuse never saw the camera move)
	UINT		giPrevSize[2] = {};		// (2026-11-09 rt-review P6) the rect the reservoirs were written at
	struct RestirConstants { float size[4], forward[4], right[4], up[4], origin[4], rect[4], prevForward[4], prevRight[4], prevUp[4], prevEye[4], params[4]; };
	int			imageDepth = -1, imageNormals = -1, imageAlbedo = -1, imagePano = -1, imageFrame = -1, imageMaterial = -1, imageAlbedoOut = -1, imageLamps = -1;
	// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: the specular signal's image (pass image 15) and the game-side copies: the specular light
	// (+ hit distance), the specular albedo (r, g) and (b, roughness) from the mean images, the denoised specular light; the second denoiser
	int			imageSpec = -1;
	IDirect3DTexture9*	specTexture = nullptr;		IDirect3DSurface9*	specSurface = nullptr;
	IDirect3DTexture9*	specAlbedoA = nullptr;		IDirect3DSurface9*	specAlbedoASurface = nullptr;
	IDirect3DTexture9*	specAlbedoB = nullptr;		IDirect3DSurface9*	specAlbedoBSurface = nullptr;
	IDirect3DTexture9*	specDenoised = nullptr;		IDirect3DSurface9*	specDenoisedSurface = nullptr;
	RtDenoise::Denoiser* specDenoiser = nullptr;
	bool		splitThisFrame = false;		// this frame's trace wrote the two signals and their copies were recorded
	bool		rrDenoising = false;		// (ptaaa-denoiser) DLSS RR denoised the path tracer's raw paths last frame (the log line's state)
	UInt32		rrSwitches = 0;
	constexpr int kLampsMax = 256;	// (2026-11-03) the lamp image: ShadowCubeMapsMax + TrackedLightsMax (76) fit; (rtspec) the game's table's nearest 256
	constexpr int kGlowMax = 32;	// (2026-11-03 b) PTGlowLights: at most this many glow lights, in the lamp image after the lamps
	constexpr int kLightTexels = 4 * (kLampsMax + kGlowMax);
	// UNOFFICIAL 2026-11-04 PTLightGrid: the lamp image's rows 1.. hold a world grid of the lamps (BuildGrid): per cell one texel (where its
	// list starts, how many), then the lists (two lamp numbers a texel). 32 x 32 x 16 cells at most; kGridRows rows of kLightTexels.
	constexpr int kGridRows = 48;
	constexpr int kGridTexels = kLightTexels * kGridRows;
	// (2026-11-07) the lamp image's FRAME ROW after the grid rows (RtPathTrace.comp kFrameRow = 49: keep them equal): per-frame values the
	// full constant block has no room for (last frame's camera, the reservoir set, the camera's world position, PTRestirDI's keys)
	constexpr int kFrameRow = 1 + kGridRows;
	// (2026-11-07 ptaaa-emissive) PTEmissive's EMISSIVE ROW after it (RtEmissiveRow.h: an entry per emissive instance, the remap)
	constexpr int kEmissiveRow = kFrameRow + 1;
	constexpr int kLampRows = kEmissiveRow + 1;
	static_assert(kFrameRow == 49, "RtPathTrace.comp kFrameRow");
	static_assert(kEmissiveRow == RtEmissiveRow::kRow && RtEmissiveRow::kMax * RtEmissiveRow::kEntryFloats <= 2 * 4 * (256 + 32), "RtEmissiveRow.h kRow / the row's room");
	RtRestirRow::State restirState;
	// (2026-11-07) the scratch store's layout: the ReSTIR DI reservoirs (two sets of w x h x 4 words) at word 0, the radiance cache from here
	// (2026-11-07 ptaaa-emissive: two sets of w x h x 6 words, was x 4: an emissive sample's triangle and barycentrics)
	unsigned long long CacheBaseWords(UINT w, UINT h) { return ((12ull * w * h + 4095ull) / 4096ull) * 4096ull; }
	// (2026-11-07 ptaaa-emissive) PTEmissive's blocks + triangle list after the radiance cache's table (RtEmissiveRow.h kScratchWords)
	unsigned long long EmissiveBaseWords(UINT w, UINT h, int cacheLog2) { return CacheBaseWords(w, h) + (16ull << cacheLog2); }
	RtEmissiveRow::State emissiveState;
	int			shaderEmissive = -1;	// RtEmissiveBuild.spv
	int			emissiveCount = 0;	// this frame's
	struct EmissiveConstants { float params[4]; };	// RtEmissiveBuild.comp's block	// (2026-11-07) PTRestirDI: last frame's camera, rect, lamps (RtRestirRow.h, shared with tools/nvr-rt-host)
	constexpr int kGridXY = 32, kGridZ = 16;
	// Below this many lamps the plain shader is cheaper (RtPathTrace.comp: every thread reads the same lamp; the grid's per-thread lists
	// are divergent, dependent loads): helper replays of a Lucky 38 frame (iGPU 720p, 4 bounces) cost the loop ~0.92 ms a lamp and the
	// grid ~18 ms more at 21 lamps, ~190 ms less at 256 (RT-PATH-TRACING.md section 25)
	constexpr int kGridMinLamps = 40;	// (the default of the test key PTLightGridMinLamps)
	IDirect3DTexture9*	lampTexture = nullptr;	// (2026-11-03) the lamp list for the helper (G32R32F, 4 texels a lamp)
	struct LampData { float pos[4], colour[4]; };
	LampData	lampData[kLampsMax];
	int			lampCount = 0;	// (2026-11-02 b) PTPrimary's G-buffer, the sky panorama
	IDirect3DTexture9*	displayTexture = nullptr;
	IDirect3DSurface9*	displaySurface = nullptr;
	IDirect3DTexture9*	denoisedTexture = nullptr;	// (2026-11-03) PTDenoise's output
	IDirect3DTexture9*	albedoTexture = nullptr;	// (2026-11-03) PTDenoise: the first surface's colour (the helper's albedo out)
	IDirect3DSurface9*	albedoSurface = nullptr;
	bool		albedoThisFrame = false;
	float		tracedScale[2] = { 1.0f, 1.0f };	// (2026-11-03) the RenderScale rect the shown trace covers (the overlay after the upscale reads it at uv x this)
	UINT		lastRect[2] = {};
	IDirect3DSurface9*	denoisedSurface = nullptr;
	RtDenoise::Denoiser* denoiser = nullptr;
	IDirect3DTexture9*	parkTexture = nullptr;		// a pass may not draw into the device's current render target: park it here meanwhile
	IDirect3DSurface9*	parkSurface = nullptr;
	UINT		textureWidth = 0, textureHeight = 0;
	ID3D9NvrExtPipeline* showPipeline = nullptr;
	bool		showTried = false;
	UInt32		samples = 0;				// in the helper's mean (counted when this frame's pass and copy were recorded)
	bool		haveImage = false;			// displayTexture holds a traced mean (also after the trace stopped at PTMaxSamples)
	UInt32		resets = 0;
	// what the mean was traced with: any change starts it over
	D3DXMATRIX	lastView;
	D3DXVECTOR4	lastCamera, lastSun;
	float		lastKey[8] = {};
	bool		lastValid = false;

	void ReadSettings() {
		auto f = [](const char* k, float lo, float hi) { return (std::min)((std::max)(TheSettingManager->GetSettingF(kSection, k), lo), hi); };
		auto i = [](const char* k, int lo, int hi) { return (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, k), lo), hi); };
		S.enabled = TheSettingManager->GetSettingI(kSection, "ReferencePT") != 0;
		S.bounces = i("PTBounces", 1, 16);
		S.samples = i("PTSamples", 1, 16);
		S.maxSamples = i("PTMaxSamples", 1, 1 << 20);
		S.sky = f("PTSky", 0.0f, 16.0f);
		S.ambient = f("PTAmbient", 0.0f, 4.0f);
		S.glow = f("PTGlow", 0.0f, 16.0f);
		S.clamp = f("PTClamp", 0.0f, 1000.0f);
		S.exposure = f("PTExposure", 0.01f, 64.0f);
		S.split = f("PTSplit", 0.0f, 1.0f);
		S.debugView = i("PTDebugView", 0, 10);	// (2026-11-07: 10 the radiance cache at the first hit)
		S.primary = TheSettingManager->GetSettingI(kSection, "PTPrimary") != 0;
		S.specular = TheSettingManager->GetSettingI(kSection, "PTSpecular") != 0;
		S.denoise = TheSettingManager->GetSettingI(kSection, "PTDenoise") != 0;
		S.replace = TheSettingManager->GetSettingI(kSection, "PTReplace") != 0;
		S.glowLights = TheSettingManager->GetSettingI(kSection, "PTGlowLights") != 0;
		S.glowFromFrame = TheSettingManager->GetSettingI(kSection, "PTGlowFromFrame") != 0;
		S.lightGrid = TheSettingManager->GetSettingI(kSection, "PTLightGrid") != 0;
		S.lightGridMin = (std::max)(TheSettingManager->GetSettingI(kSection, "PTLightGridMinLamps"), 1);
		S.bounceAlpha = i("PTBounceAlpha", 0, 3);
		S.bounceDistance = f("PTBounceDistance", 0.0f, 60000.0f);
		S.indirectClamp = f("PTIndirectClamp", 0.0f, 1000.0f);
		S.realtimeClamp = f("PTRealtimeClamp", 0.0f, 1000.0f);	// UNOFFICIAL 2026-11-09 (a missing key: 0, off)
		S.bounceRoughen = f("PTBounceRoughen", -1.0f, 1.0f);		// UNOFFICIAL 2026-11-09 (-1 = on under PTRealtime; a missing key reads 0: off)
		S.denoiseIterations = i("PTDenoiseIterations", 0, 5);
		S.denoiseHistory = f("PTDenoiseHistory", 1.0f, 256.0f);
		S.denoiseClamp = f("PTDenoiseClamp", 0.0f, 8.0f);
		S.denoisePeopleHistory = f("PTDenoisePeopleHistory", 0.0f, 256.0f);
		S.denoiseAntiLag = f("PTDenoiseAntiLag", 0.0f, 16.0f);
		S.denoiseGuide = f("PTDenoiseGuide", 0.0f, 4.0f);
		// (2026-11-07 ptaaa-denoiser; a missing key reads 0: the toml defaults carry the ON values)
		S.denoiseSplit = TheSettingManager->GetSettingI(kSection, "PTDenoiseSplit") != 0;
		S.denoiseObjectMotion = TheSettingManager->GetSettingI(kSection, "PTDenoiseObjectMotion") != 0;
		S.denoiseMovingHistory = i("PTDenoiseMovingHistory", 0, 256);
		S.denoiseMirrorHistory = f("PTDenoiseMirrorHistory", 1.0f, 256.0f);
		S.denoiseVirtual = TheSettingManager->GetSettingI(kSection, "PTDenoiseVirtual") != 0;
		S.denoiseMirrorSurface = f("PTDenoiseMirrorSurface", 0.0f, 1.0f);	// UNOFFICIAL 2026-11-09 (pt-mirrors; a missing key: 0, off)
		S.denoiseUnderRr = i("PTDenoiseUnderRr", 0, 1);
		S.denoiseHistoryFix = f("PTDenoiseHistoryFix", 0.0f, 16.0f);
		S.denoiseMeanVariance = f("PTDenoiseMeanVariance", 0.0f, 64.0f);
		S.denoiseAntiFirefly = i("PTDenoiseAntiFirefly", -1, 1);
		S.denoiseFireflyK = f("PTDenoiseFireflyK", 0.0f, 64.0f);
		S.denoiseFireflySpread = (int)f("PTDenoiseFireflySpread", 0.0f, 64.0f);	// UNOFFICIAL 2026-11-09 (RtDenoiseTemporalRgb / Spec .frag FireflyExcess)
		S.detail = f("PTDetail", 0.0f, 1.0f);
		S.detailRadius = f("PTDetailRadius", 1.0f, 16.0f);
		S.viewModel = TheSettingManager->GetSettingI(kSection, "PTViewModel") != 0;	// (2026-11-07 ptaaa-firstperson; a missing key: 0, off)
		S.contact = f("PTContactShadows", 0.0f, 1.0f);	// (2026-11-07 rtgap-tess; a missing key: 0, off)
		S.contactLength = f("PTContactLength", 0.0f, 128.0f);
		S.lastSky = i("PTLastSky", -1, 1);
		S.lastBounce = i("PTLastBounce", -1, 1);
		S.lampFalloff = i("PTLampFalloff", 0, 1);
		S.lampGain = f("PTLampGain", 0.1f, 8.0f);
		S.bounceHalf = i("PTBounceHalf", -1, 1);	// (2026-11-06) the bounce for half the pixels a frame
		S.denoiseSigma = f("PTDenoiseSigma", 0.25f, 16.0f);	// (2026-11-06) the a-trous luminance edge stop
		S.restirGI = TheSettingManager->GetSettingI(kSection, "PTRestirGI") != 0;
		S.restirMaxM = f("PTRestirMaxM", 1.0f, 255.0f);
		S.restirSpatial = f("PTRestirSpatial", 0.0f, 16.0f);
		S.restirRadius = f("PTRestirRadius", 1.0f, 64.0f);
		S.restirDepth = f("PTRestirDepth", 0.001f, 1.0f);
		S.restirNormal = f("PTRestirNormal", -1.0f, 1.0f);
		S.restirDI = i("PTRestirDI", -1, 1);
		S.allLamps = i("PTAllLamps", -1, 1);
		S.radianceCache = i("PTRadianceCache", -1, 1);
		S.cacheCell = f("PTCacheCell", 1.0f, 256.0f);
		S.cacheMaxSamples = f("PTCacheMaxSamples", 1.0f, 65536.0f);
		S.cacheMinSamples = f("PTCacheMinSamples", 0.0f, 1024.0f);
		S.cacheMaxAge = f("PTCacheMaxAge", 1.0f, 100000.0f);
		S.cacheMaxFrames = f("PTCacheMaxFrames", 1.0f, 1024.0f);
		S.cacheLog2 = i("PTCacheEntriesLog2", 12, 22);
		S.emissive = i("PTEmissive", -1, 1);	// (2026-11-07 ptaaa-emissive)
		S.emissiveMax = i("PTEmissiveMax", 0, RtEmissiveRow::kMax);
		S.emissiveTriangles = i("PTEmissiveTriangles", 0, (int)RtEmissiveRow::kTrianglesMax);
		S.restirDISpatial = i("PTRestirDISpatial", 0, 8);
		S.restirDIRadius = f("PTRestirDIRadius", 1.0f, 64.0f);
		S.restirDIMaxM = f("PTRestirDIMaxM", 1.0f, 255.0f);
		S.restirDINormal = f("PTRestirDINormal", -1.0f, 1.0f);
		S.restirDIPlane = f("PTRestirDIPlane", 0.001f, 1.0f);
		S.restirDIRays = i("PTRestirDIRays", 0, 3);	// (2026-11-07 perf-ptcost)
		S.bounceLights = i("PTBounceLights", 0, 1);
		// UNOFFICIAL 2026-11-07 (ptaaa-materials, RT-PATH-TRACING.md "materials") the material models (a missing key: 0, off)
		S.materials.glass = (float)i("PTGlass", 0, 2);
		S.materials.glassTint = f("PTGlassTint", 0.0f, 1.0f);
		S.materials.foliage = f("PTFoliage", 0.0f, 1.0f);
		S.materials.skin = f("PTSkin", 0.0f, 2.0f);
		S.materials.sheen = f("PTClothSheen", 0.0f, 1.0f);
		// UNOFFICIAL 2026-11-04 PTRealtime: one switch for the path traced game (RT-PATH-TRACING.md section 31): the path tracer on, its picture
		// into the scene (PTReplace) through the shared denoiser (PTDenoise, 2 a-trous passes unless set), one path a pixel and frame, and the
		// bounce rays testing only the nearest cut-out card (PTBounceAlpha 3: -31 % outdoors, the same picture indoors; section 27) unless
		// another bounce mode is set. It overrides those settings while on; off, they are read as they are.
		if (TheSettingManager->GetSettingI(kSection, "PTRealtime") != 0) {
			S.enabled = true;
			S.replace = true;
			S.denoise = true;
			S.samples = 1;
			if (S.bounceAlpha == 1) S.bounceAlpha = 3;
			// (2026-11-06, RT-PATH-TRACING.md section 34) one bounce unless PTBounces was changed from its default 4: replays of Goodsprings and
			// Lucky 38 (48 paths each): 4 -> 1 bounce halves the trace (337 -> 172 ms), the picture 0.2 % darker, RMSE 0.27 / 1.20 against a
			// 2.32 / 7.62 noise floor (deeper bounces carry almost nothing after Russian roulette)
			// (2026-11-07, section 46) QUALITY FIRST (the users are RTX 40 / 50 owners): 3 bounces unless PTBounces was changed from its default
			// 4 (was 1: outdoors the ground 1.2 % dark, indoors 2.5 %, patched by PTLastSky / PTLastBounce estimates); PTBounceHalf no longer on
			// by itself (its 8 x 8 tile checkerboard showed as a TILE GRID while the camera turned: the denoiser's history lags the half-rate
			// tiles); set PTBounceHalf 1 to get it back
			if (S.bounces == 4) S.bounces = 3;
			if (S.bounceHalf < 0) S.bounceHalf = 0;
			// PTLastSky / PTLastBounce estimate the light of the bounces a 1-bounce path leaves out: on by themselves only at 1 bounce
			if (S.lastSky < 0) S.lastSky = S.bounces == 1 ? 1 : 0;	// (2026-11-06, section 39) PTLastSky: the last hit's sky light
			if (S.lastBounce < 0) S.lastBounce = S.bounces == 1 ? 1 : 0;	// (2026-11-06, section 44) PTLastBounce: the last hit's further bounces
			if (S.restirDI < 0) S.restirDI = 1;	// (2026-11-07, section 47) PTRestirDI
			if (S.allLamps < 0) S.allLamps = 1;	// (2026-11-07, section 47) PTAllLamps
			if (S.radianceCache < 0) S.radianceCache = 1;	// (2026-11-07, section 48) PTRadianceCache
			if (S.emissive < 0) S.emissive = 1;	// (2026-11-07 ptaaa-emissive) PTEmissive
			if (S.denoiseAntiFirefly < 0) S.denoiseAntiFirefly = 1;	// (2026-11-07 ptaaa-denoiser) PTDenoiseAntiFirefly
			// UNOFFICIAL 2026-11-09 the firefly guards in the trace itself (they reach DLSS Ray Reconstruction too): the bounce light's cap
			// unless PTIndirectClamp sets one, and path regularisation unless PTBounceRoughen is set
			if (S.indirectClamp <= 0.0f) S.indirectClamp = S.realtimeClamp;
			if (S.bounceRoughen < 0.0f) S.bounceRoughen = 0.35f;
		}
		if (S.bounceRoughen < 0.0f) S.bounceRoughen = 0.0f;
		if (S.emissive < 0) S.emissive = 0;
		if (S.radianceCache < 0) S.radianceCache = 0;
		if (S.denoiseAntiFirefly < 0) S.denoiseAntiFirefly = 0;
		if (S.allLamps < 0) S.allLamps = 0;
		if (S.restirDI < 0) S.restirDI = 0;
		if (!S.denoise) S.restirDI = 0;	// (a running mean of reused samples would converge to the reuse's own picture: the reference stays plain)
		S.gameLamps = std::clamp(TheSettingManager->GetSettingI(kSection, "PTGameLamps"), 0, 4);
		if (S.enabled && RtCore::LeftOut(kUserName)) S.enabled = false;	// (2026-11-05) its images did not fit at the helper start: no picture of its own
	}

	float Lin(float v) { return powf((std::max)(v, 0.0f), 2.2f); }
	// UNOFFICIAL 2026-11-04 (rtgap) the game's light scale: its lit shaders multiply the sun and the lamps by [Shaders.PBR.*] / [Shaders.Terrain.*]
	// LightingScale, decoded to linear (LIN_SCALE_LIGHT, TESR_LinearScales.x): 0.725 by day outdoors = 0.49 linear, 1 indoors and at night.
	// Without it the reference's sunlit surfaces were 2.06x the game's at Goodsprings noon (rtgap-gs); with the game's own lights only.
	inline float GameLightScale() { PBRShaders* pbr = TheShaderManager->Shaders.PBR; return pbr ? pbr->Constants.LinearScales.x : 1.0f; }

	// The lamps that light the hits: the shadow slots' lamps and the tracked ones as ShaderManager listed them this frame, nearest
	// first, at most 8; scene space (world minus CameraPosition), colour linear x dimmer (as AmbientOcclusion.cpp RtGILamps).
	int Lamps(PassConstants& c) {
		struct Lamp { D3DXVECTOR4 pos, colour; float distance; };
		// (2026-11-06 c) PTLampFalloff 1: the lamp reaches twice as far (the inverse-square tail; RtPathTrace.comp LampFall) and its colour is
		// x PTLampGain; the cull, the lamp image and the light grid all see the stretched reach
		auto physical = [&](Lamp& l) { if (S.lampFalloff == 1) { l.pos.w *= 2.0f; { const float code = l.colour.w; l.colour *= S.lampGain; l.colour.w = code; } l.distance = (std::max)(D3DXVec3Length((const D3DXVECTOR3*)&l.pos) - l.pos.w, 0.0f); } };
		Lamp lamps[ShadowCubeMapsMax + TrackedLightsMax];
		int found = 0;
		const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
		auto add = [&](const D3DXVECTOR4& p, const D3DXVECTOR4& col) {
			if (p.w <= 10.0f || col.w <= 0.0f || found >= ShadowCubeMapsMax + TrackedLightsMax) return;
			// (2026-11-07 ptaaa-hangfix) a lamp with a NaN / Inf position or reach is left out: it passed the tests above (NaN compares false)
			// and its NaN distance broke std::sort's ordering below (undefined behaviour)
			if (!(fabsf(p.x) + fabsf(p.y) + fabsf(p.z) + fabsf(p.w) < 1.0e30f) || !(col.w < 1.0e30f)) return;
			const D3DXVECTOR3 rel(p.x - eye.x, p.y - eye.y, p.z - eye.z);
			for (int i = 0; i < found; i++)	// the same lamp in both lists
				if (fabsf(lamps[i].pos.x - rel.x) < 1.0f && fabsf(lamps[i].pos.y - rel.y) < 1.0f && fabsf(lamps[i].pos.z - rel.z) < 1.0f) return;
			// (2026-11-03 rtspec) PTGameLamps: the colour as the game's lamp table makes it (ManyLamps.h: diffuse x dimmer, the dimmer at most 1
			// without HDR, then decoded to linear together; the light's LOD dimmer is not in these lists); off: diffuse decoded x dimmer, as RT GI
			float dim = col.w;
			const bool gameColour = (S.gameLamps & 2) != 0;
			if (gameColour && !*(const bool*)0x011F941E && dim > 1.0f) dim = 1.0f;	// BSShaderManager::bHDR
			const D3DXVECTOR4 colour = gameColour ? D3DXVECTOR4(Lin(col.x * dim), Lin(col.y * dim), Lin(col.z * dim), 0.0f) :
				D3DXVECTOR4(Lin(col.x) * col.w, Lin(col.y) * col.w, Lin(col.z) * col.w, 0.0f);
			lamps[found++] = { D3DXVECTOR4(rel.x, rel.y, rel.z, p.w), colour,
				(std::max)(D3DXVec3Length(&rel) - p.w, 0.0f) };
		};
		ShaderManager* sm = TheShaderManager;
		// UNOFFICIAL 2026-11-07 (a tester's video: the flashlight under PTRealtime lit only a rubble pile and a curb, the pixels where the
		// picture falls back to the game's frame) the flashlight is NVR's own spot light (Flashlight.cpp PublishLightConstants: the shared
		// TESR_SpotLight* constants; FlashlightDeferred draws it on the G-buffer), not a game lamp, so the path tracer never had it. It is a
		// lamp here, its cone in the colour's w: 1 + (angle x 512 + u) x 512 + v, the cone's half-angle in whole degrees and its direction
		// octahedral in 9 bits each (an exact integer below 2^24; RtPathTrace.comp LampSpot); 0 = a plain lamp.
		Lamp flash = {};
		bool haveFlash = false;
		{
			// (2026-11-07, tester: "the flashlight fix did not work with PT") the light exactly as the flashlight's own pass draws it
			// (FlashlightDeferred::LightParams: the beam on the weapon / camera, reach, axis, cone, [Shaders.Flashlight.Main] Color x Dimmer,
			// linear). The first version took SpotLightColor (the NiSpotLight's Diff x its Dimmer), which can be 0 while NVR draws the light
			// itself: then no lamp at all. The TESR_SpotLight* constants remain the fallback when that pass cannot draw.
			D3DXVECTOR4 fl[3];
			D3DXVECTOR3 pos(0.0f, 0.0f, 0.0f), axis(0.0f, 0.0f, 0.0f), colour(0.0f, 0.0f, 0.0f);
			float reach = 0.0f, cosOuter = 1.0f;
			if (FlashlightDeferred::LightParams(fl)) {
				pos = D3DXVECTOR3(fl[0].x, fl[0].y, fl[0].z); reach = fl[0].w;
				axis = D3DXVECTOR3(fl[1].x, fl[1].y, fl[1].z); cosOuter = fl[1].w;
				colour = D3DXVECTOR3(fl[2].x, fl[2].y, fl[2].z);
			}
			else {
				const D3DXVECTOR4& fp = sm->SpotLightPosition[0];
				const D3DXVECTOR4& fd = sm->SpotLightDirection[0];
				const D3DXVECTOR4& fc = sm->SpotLightColor[0];
				pos = D3DXVECTOR3(fp.x - eye.x, fp.y - eye.y, fp.z - eye.z); reach = fp.w;
				axis = D3DXVECTOR3(fd.x, fd.y, fd.z); cosOuter = cosf(D3DXToRadian((std::min)((std::max)(fd.w, 1.0f), 89.0f)));
				colour = D3DXVECTOR3(Lin(fc.x) * fc.w, Lin(fc.y) * fc.w, Lin(fc.z) * fc.w);
			}
			const float len = D3DXVec3Length(&axis);
			if (reach > 10.0f && len > 1.0e-3f && (colour.x + colour.y + colour.z) > 0.0f) {
				// (2026-11-07 perf-reflfix2) the code in RtFlashlightCode.h (the RT test host's ptflashlight scene checks the same function)
				flash.pos = D3DXVECTOR4(pos.x, pos.y, pos.z, reach);
				flash.colour = D3DXVECTOR4(colour.x, colour.y, colour.z, RtFlashlightCode::Encode(axis.x, axis.y, axis.z, cosOuter));
				flash.distance = -1.0f;	// at the eye: always the first lamp
				haveFlash = true;
			}
			static UInt32 told = 0;
			static int lastState = -1;
			if (told < 12 && lastState != (haveFlash ? 1 : 0)) {	// each change (on / off), a few times a session
				told++; lastState = haveFlash ? 1 : 0;
				Logger::Log("UNOFFICIAL RT path trace: flashlight %s (reach %.0f, colour %.3f %.3f %.3f, cone %.1f deg)", haveFlash ? "is lamp 0" : "not lit",
					reach, colour.x, colour.y, colour.z, D3DXToDegree(acosf((std::min)((std::max)(cosOuter, -1.0f), 1.0f))));
			}
		}
		// (rtspec) PTGameLamps 4: the game's lamp table's active lamps this frame (the deferred pass's lamps), nearest first, kLampsMax at most
		// (2026-11-07) PTAllLamps: every table lamp that is not hidden, not only those the game lit this view with (section 47)
		const std::vector<ManyLamps::ActiveLamp>& source = S.allLamps > 0 ? ManyLamps::AllLamps : ManyLamps::ActiveLamps;
		if (S.gameLamps == 4 && ManyLamps::TableFrame + 1 >= ManyLamps::FrameId && !source.empty()) {
			static std::vector<Lamp> table;
			table.clear();
			for (const ManyLamps::ActiveLamp& a : source) {
				if (a.reach <= 0.0f) continue;
				const D3DXVECTOR3 rel(a.pos[0] - eye.x, a.pos[1] - eye.y, a.pos[2] - eye.z);
				const float ls = GameLightScale();	// (2026-11-04 rtgap) as the game's lamps: x LIN_SCALE_LIGHT
				table.push_back({ D3DXVECTOR4(rel.x, rel.y, rel.z, a.reach), D3DXVECTOR4(a.colour[0] * ls, a.colour[1] * ls, a.colour[2] * ls, 0.0f),
					(std::max)(D3DXVec3Length(&rel) - a.reach, 0.0f) });
			}
			for (Lamp& l : table) physical(l);
			if (haveFlash) { Lamp f = flash; physical(f); f.distance = -1.0f; table.push_back(f); }	// (2026-11-07) the flashlight
			std::sort(table.begin(), table.end(), [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
			lampCount = (std::min)((int)table.size(), kLampsMax);
			for (int i = 0; i < lampCount; i++) { memcpy(lampData[i].pos, &table[i].pos, 16); memcpy(lampData[i].colour, &table[i].colour, 16); }
			const int first = (std::min)(lampCount, 8);
			for (int i = 0; i < first; i++) { memcpy(c.lampPos[i], &table[i].pos, 16); memcpy(c.lampColor[i], &table[i].colour, 16); }
			return first;
		}
		if (haveFlash) lamps[found++] = flash;	// (2026-11-07) first: add() then leaves out a game lamp at its place (the Pip-Boy light on the beam)
		for (int s = 0; s < ShadowCubeMapsMax; s++) add(sm->BatchLightPosition[s], sm->BatchLightColor[s]);
		for (int t = 0; t < TrackedLightsMax; t++) add(sm->LightPosition[t], sm->LightColor[ShadowCubeMapsScreen + t]);
		for (int i = 0; i < found; i++) physical(lamps[i]);
		if (haveFlash) lamps[0].distance = -1.0f;	// the flashlight: always the first lamp
		std::sort(lamps, lamps + found, [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
		lampCount = (std::min)(found, kLampsMax);	// (2026-11-03) all of them for the lamp image
		for (int i = 0; i < lampCount; i++) { memcpy(lampData[i].pos, &lamps[i].pos, 16); memcpy(lampData[i].colour, &lamps[i].colour, 16); }
		found = (std::min)(found, 8);
		for (int i = 0; i < found; i++) { memcpy(c.lampPos[i], &lamps[i].pos, 16); memcpy(c.lampColor[i], &lamps[i].colour, 16); }
		return found;
	}

	// UNOFFICIAL 2026-11-03 b PTGlowLights: the glow lights for the lamp image (4 floats a texel pair: centre, radius; a guessed colour):
	// the frame's glow-mapped instances (RtCore::GlowLights(): world bound sphere, the record's average colour), the kGlowMax that
	// should give the camera's surroundings the most light (guessed colour x radius^2 / distance^2). The sphere only tells the trace
	// WHERE to look (RtPathTrace.comp GlowSample shoots a ray into it and takes the real mesh's glow); the colour only steers how often
	// it is picked. Bounds over 512 units (rooms, whole facades with a few lit windows) stay out: aiming at a sphere that size is no
	// better than a bounce ray, and their glow keeps coming from bounce rays. The radius is grown by 1 % + 1 unit (float slack: every
	// glowing point of the mesh must be inside its sphere).
	struct GlowData { float pos[4], colour[4]; };
	GlowData	glowData[kGlowMax];
	int Glows(bool emissive) {
		if (!S.glowLights || S.glow <= 0.0f || emissive) return 0;	// (2026-11-07 ptaaa-emissive: PTEmissive replaces them)
		struct Pick { const RtCore::GlowLight* g; float weight; };
		std::vector<Pick> picks;
		for (const RtCore::GlowLight& g : RtCore::GlowLights()) {
			if (!(g.radius > 0.0f) || g.radius > 512.0f) continue;
			const float luma = 0.2126f * g.rgb[0] + 0.7152f * g.rgb[1] + 0.0722f * g.rgb[2] + 0.05f;
			const float d2 = g.center[0] * g.center[0] + g.center[1] * g.center[1] + g.center[2] * g.center[2];
			picks.push_back({ &g, luma * g.radius * g.radius / (std::max)(d2, g.radius * g.radius) });
		}
		const size_t keep = (std::min)(picks.size(), (size_t)kGlowMax);
		std::partial_sort(picks.begin(), picks.begin() + keep, picks.end(), [](const Pick& a, const Pick& b) { return a.weight > b.weight; });
		for (size_t i = 0; i < keep; i++) {
			const RtCore::GlowLight& g = *picks[i].g;
			GlowData& d = glowData[i];
			d.pos[0] = g.center[0]; d.pos[1] = g.center[1]; d.pos[2] = g.center[2]; d.pos[3] = g.radius * 1.01f + 1.0f;
			// a guess of the light it gives (only how often it is picked): its colour (floor 0.05: never 0, a listed light must be
			// pickable) x PTGlow x 0.25 (the share of a bound sphere's view a glowing mesh tends to fill)
			for (int k = 0; k < 3; k++) d.colour[k] = ((std::max)(g.rgb[k], 0.0f) + 0.05f) * S.glow * 0.25f;
			d.colour[3] = 0.0f;
		}
		return (int)keep;
	}

	// UNOFFICIAL 2026-11-04 PTLightGrid: a world grid over the lamps (lampData, scene space), so a hit weighs only the lamps whose reach
	// touches its cell (RtPathTrace.comp Direct). The grid covers the union of the lamps' spheres (a hit outside it is out of every lamp's
	// reach: no lamp light, as before), at most kGridXY x kGridXY x kGridZ cells of at least 64 units. Each cell lists, in rising lamp
	// order, every lamp whose sphere meets the cell's box: a lamp left out has weight 0 at every point of the cell anyway (dist >= reach),
	// and the reservoir draws a random number only for lamps of weight > 0, so the pick (and the picture) is the same as the loop over
	// every lamp, bit for bit. Only with PTLightGridMinLamps (kGridMinLamps) lamps or more (below, the all-lamp loop is cheaper). Writes the cells (one texel each: first entry, count) then the lists (two lamp numbers a texel) into `out`
	// (kGridTexels float pairs) and the constants; false (the trace loops over every lamp) when the lists do not fit.
	// UNOFFICIAL 2026-11-07 PTRestirDI: the lamp image's frame row and the lamp numbers' remap (RtRestirRow.h has the layout)
	void FrameRow(const PassConstants& c, float* row, float* lamps, UINT tw, UINT th) {
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const D3DXVECTOR3 eye = RtCore::Eye();
		const double camWorld[3] = { cam.x, cam.y, cam.z };
		const float eyeScene[3] = { eye.x, eye.y, eye.z };
		RtRestirRow::Keys keys;
		keys.maxM = S.restirDIMaxM; keys.spatial = (float)S.restirDISpatial; keys.radius = S.restirDIRadius; keys.normal = S.restirDINormal; keys.plane = S.restirDIPlane;
		if (cacheLog2Setup > 0) {	// (2026-11-07) PTRadianceCache
			keys.cacheLog2 = (float)cacheLog2Setup;
			keys.cacheBase4096 = (float)(CacheBaseWords(TheRenderManager->width, TheRenderManager->height) / 4096ull);
			keys.cacheCell = S.cacheCell; keys.cacheMaxSamples = S.cacheMaxSamples; keys.cacheMinSamples = S.cacheMinSamples;
		}
		RtRestirRow::Build(restirState, row, lamps, lampData[0].pos, lampCount, (int)(sizeof(LampData) / sizeof(float)), camWorld, eyeScene, c.forward, c.right, c.up, tw, th, keys);
	}

	struct GridInfo { int cells = 0, entries = 0; float cell[3] = {}; int dims[3] = {}; };
	GridInfo	gridInfo;
	bool BuildGrid(PassConstants& c, float* out) {	// (2026-11-06) the builder is RtLightGrid::Build (RtLightGrid.h, tested by tools/nvr-rt-host)
		gridInfo = {};
		if (lampCount < S.lightGridMin) return false;
		RtLightGrid::Info info;
		if (!RtLightGrid::Build(lampData[0].pos, lampCount, (int)(sizeof(LampData) / sizeof(float)), out, kGridTexels, kLightTexels, c.grid, info)) {
			static int logged = 0;
			if (logged++ < 4) Logger::Log("UNOFFICIAL RT path trace: the light grid does not fit (%d lamps, %d cells, %d list entries): every hit weighs every lamp this frame.", lampCount, info.cells, info.entries);
			return false;
		}
		gridInfo.cells = info.cells; gridInfo.entries = info.entries;
		for (int a = 0; a < 3; a++) { gridInfo.cell[a] = info.cell[a]; gridInfo.dims[a] = info.dims[a]; }
		return true;
	}

	bool Wants() {
		ReadSettings();
		if (!S.enabled || !S.viewModel) CapturedNormals::VmWanted = false;	// (2026-11-07 ptaaa-firstperson; Record turns it on)
		return S.enabled;
	}

	// UNOFFICIAL 2026-11-07 (ptaaa-firstperson; docs/next/RT-PATH-TRACING.md "ptaaa-firstperson") PTViewModel: the first-person model (arms,
	// gun, Pip-Boy) lit by the path tracer. It is drawn after the world, from the same eye with its own FOV and near plane, into the depth
	// buffer CombineDepth merges (its view z); since 2026-11-07 its colour, normals and material go into the G-buffer too (core/CapturedNormals.h
	// BeginViewModelSpan, round the game's first-person render, while CapturedNormals::VmWanted). Here, before the trace, one D3D9 pass makes
	// the path tracer's normals image from TESR_NormalsBuffer + the captured normals + the view model's depth:
	//   rgb  the view-space normal (TESR_NormalsBuffer's; on the view model the captured smooth normal, view space: the depth-rebuilt one
	//        there comes from points spread by the world's wider FOV)
	//   a    1 the world, 2 a person (captured a > 1.5: RtDenoise's PeopleHistory), 4 + s the view model where the first-person pass captured
	//        it (its captured distance = its depth's at the view model's FOV, within 3 % + 2 units; s = tan(world FOV / 2) / tan(first-person
	//        FOV / 2)), -1 the view model where it did not (a shader that writes no G-buffer: the game's own pixel stays)
	// RtPathTrace.comp ScreenHit reads the code; RtDenoiseTemporalRgb.frag (Settings.People) keeps a > 3.5 screen-locked.
	IDirect3DTexture9*	vmNormals = nullptr;	IDirect3DSurface9* vmNormalsSurface = nullptr;
	IDirect3DPixelShader9* vmPrepPS = nullptr;
	bool		vmPrepFailed = false, vmThisFrame = false;
	UInt32		vmPrepFrames = 0;
	IDirect3DTexture9*	vmCheckTex = nullptr;	IDirect3DSurface9* vmCheckSurface = nullptr;	IDirect3DSurface9* vmCheckSys = nullptr;
	float FovScale() {	// as ReflectionsEffect::WeaponFovScale: tan(world FOV / 2) / tan(first-person FOV / 2), 1 when either is unknown
		if (!Player) return 1.0f;
		const float world = Player->worldFoV, first = Player->firstPersonFoV;
		if (world <= 1.0f || world >= 179.0f || first <= 1.0f || first >= 179.0f) return 1.0f;
		const float toRad = 3.14159265f / 360.0f;
		return tanf(world * toRad) / tanf(first * toRad);
	}
	bool ViewModelPrep(IDirect3DDevice9* Device, IDirect3DBaseTexture9* normals, UINT w, UINT h) {
		auto constant = [](const char* name) { auto f = TheShaderManager->ConstantsTable.find(name); return f == TheShaderManager->ConstantsTable.end() ? nullptr : f->second; };
		auto texture = [](const char* name) -> IDirect3DBaseTexture9* { auto f = TheTextureManager->TextureNames.find(name); return f == TheTextureManager->TextureNames.end() || !f->second ? nullptr : *f->second; };
		IDirect3DBaseTexture9* captured = texture("TESR_CapturedNormalsBuffer");
		IDirect3DTexture9* depthVm = TheTextureManager->DepthTextureViewModel;
		const D3DXMATRIX* view = (const D3DXMATRIX*)constant("TESR_ViewTransform");
		const D3DXVECTOR4* camera = constant("TESR_CameraData");
		if (vmPrepFailed || !normals || !captured || !depthVm || !view || !camera || camera->y <= 0.0f) return false;
		if (!vmPrepPS) {
			// the source and its constants: core/RtViewModel.h (also run by tools/nvr-rt-host scene `vmprep`)
			vmPrepPS = CapturedNormals::FullScreenShader(Device, RtViewModel::kPrepPS, "RT path trace view model", vmPrepFailed);
			if (!vmPrepPS) return false;
		}
		if (vmNormals) { D3DSURFACE_DESC d = {}; vmNormals->GetLevelDesc(0, &d); if (d.Width != w || d.Height != h) { DxvkCompute::Drop(vmNormalsSurface); DxvkCompute::Drop(vmNormals); } }
		if (!vmNormals) {
			if (FAILED(Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &vmNormals, nullptr)) || !vmNormals ||
				FAILED(vmNormals->GetSurfaceLevel(0, &vmNormalsSurface))) { DxvkCompute::Drop(vmNormals); vmPrepFailed = true; return false; }
		}
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const float s = FovScale();
		const float nearVm = (std::max)(TheRenderManager->DepthConstants.x, 1.0e-4f);
		const float c[6][4] = {
			{ 1.0f / (proj._11 * s), 1.0f / (proj._22 * s), nearVm, camera->y },
			{ TheRenderManager->DepthConstants.z == 0.0f ? 0.0f : 1.0f, s, RtViewModel::kMatchShare, RtViewModel::kMatchUnits },
			{ view->_11, view->_12, view->_13, view->_14 }, { view->_21, view->_22, view->_23, view->_24 },
			{ view->_31, view->_32, view->_33, view->_34 }, { view->_41, view->_42, view->_43, view->_44 } };
		// one full-screen draw, everything it changes put back (as CapturedNormals::MergeIntoNormals)
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; IDirect3DBaseTexture9* tex[3] = {};
		IDirect3DSurface9* rt[4] = {}; D3DVIEWPORT9 vp = {};
		DWORD fvf = 0; float saved[6][4] = {};
		Device->GetVertexShader(&vs); Device->GetPixelShader(&ps); Device->GetVertexDeclaration(&decl); Device->GetFVF(&fvf);
		Device->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		for (DWORD i = 0; i < 3; i++) Device->GetTexture(i, &tex[i]);
		for (DWORD i = 0; i < 4; i++) Device->GetRenderTarget(i, &rt[i]);
		Device->GetViewport(&vp); Device->GetPixelShaderConstantF(0, &saved[0][0], 6);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(Device->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) Device->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[3][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD st = 0; st < 3; st++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(Device->GetSamplerState(st, kSS[i], &savedS[st][i]))) savedS[st][i] = wantS[i]; if (savedS[st][i] != wantS[i]) Device->SetSamplerState(st, kSS[i], wantS[i]); }
		for (DWORD i = 1; i < 4; i++) if (rt[i]) Device->SetRenderTarget(i, nullptr);
		Device->SetRenderTarget(0, vmNormalsSurface);
		const D3DVIEWPORT9 full = { 0, 0, w, h, 0.0f, 1.0f };
		Device->SetViewport(&full);
		Device->SetVertexShader(nullptr);
		Device->SetPixelShader(vmPrepPS);
		Device->SetPixelShaderConstantF(0, &c[0][0], 6);
		Device->SetTexture(0, normals); Device->SetTexture(1, captured); Device->SetTexture(2, depthVm);
		Device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		for (DWORD st = 0; st < 3; st++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[st][i] != wantS[i]) Device->SetSamplerState(st, kSS[i], savedS[st][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) Device->SetRenderState(kRS[i], savedRS[i]);
		Device->SetRenderTarget(0, rt[0]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) Device->SetRenderTarget(i, rt[i]);
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		Device->SetViewport(&vp);
		for (DWORD i = 0; i < 3; i++) { Device->SetTexture(i, tex[i]); if (tex[i]) tex[i]->Release(); }
		Device->SetPixelShaderConstantF(0, &saved[0][0], 6);
		if (decl) { Device->SetVertexDeclaration(decl); decl->Release(); } else Device->SetFVF(fvf);
		Device->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		Device->SetVertexShader(vs); if (vs) vs->Release();
		Device->SetPixelShader(ps); if (ps) ps->Release();
		// A log line now and then (the first time, then every 1800 prepared frames): how many of the view model's texels the path tracer
		// lights (a 64 x 64 point copy read back: one small stall)
		if (vmPrepFrames++ % 1800 == 0) {
			if (!vmCheckTex && SUCCEEDED(Device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &vmCheckTex, nullptr)) && vmCheckTex) {
				vmCheckTex->GetSurfaceLevel(0, &vmCheckSurface);
				Device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &vmCheckSys, nullptr);
			}
			D3DLOCKED_RECT r = {};
			if (vmCheckSurface && vmCheckSys && SUCCEEDED(Device->StretchRect(vmNormalsSurface, nullptr, vmCheckSurface, nullptr, D3DTEXF_POINT)) &&
				SUCCEEDED(Device->GetRenderTargetData(vmCheckSurface, vmCheckSys)) && SUCCEEDED(vmCheckSys->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
				int lit = 0, kept = 0, people = 0;
				for (int y = 0; y < 64; y++) {
					const D3DXFLOAT16* row = (const D3DXFLOAT16*)((const char*)r.pBits + y * r.Pitch);
					for (int x = 0; x < 64; x++) {
						float a = 0.0f; D3DXFloat16To32Array(&a, row + x * 4 + 3, 1);
						if (a > 3.5f) lit++; else if (a < -0.5f) kept++; else if (a > 1.5f) people++;
					}
				}
				vmCheckSys->UnlockRect();
				Logger::Log("UNOFFICIAL RT path trace: PTViewModel: of 4096 sample texels %d are the first-person model lit by the path tracer, %d keep the game's pixel (no capture there), %d people; FOV scale %.3f, near %.2f",
					lit, kept, people, s, nearVm);
			}
		}
		return true;
	}

	void Setup() {
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		// UNOFFICIAL 2026-11-07 (mrfury97 fork): the path tracer's 20 shared images only when it is on as the core starts. Registered
		// on or off, they left too few of the protocol's 48 for the other RT effects together (RT lamp shadows with ManyLights: 57 asked,
		// "the RT effects asked for more shared images than the protocol holds", the whole core off). Turning PT on later needs a restart
		// (StatusText says so); Record already does nothing without its images.
		ReadSettings();
		if (!S.enabled) {
			imageA = imageB = imageDisplay = imageDepth = imageNormals = imageAlbedo = imagePano = imageFrame = imageMaterial = imageAlbedoOut = imageLamps = -1;
			imageGIResult = -1;
			for (int i = 0; i < 2; i++) imageResA[i] = imageResB[i] = imageResC[i] = imageSurf[i] = -1;
			haveImage = false;
			Logger::Log("UNOFFICIAL RT reference path tracer: off as the RT core starts, so its 20 shared images are not reserved (turning ReferencePT / PTRealtime on needs a restart).");
			return;
		}
		imageA = RtCore::AddImage("NVR RT path trace mean rg", w, h, NVR_EXT_SHARED_RG32F);
		imageB = RtCore::AddImage("NVR RT path trace mean b", w, h, NVR_EXT_SHARED_RG32F);
		imageDisplay = RtCore::AddImage("NVR RT path trace", w, h, NVR_EXT_SHARED_RGBA16F);
		imageDepth = RtCore::AddImage("NVR RT path trace depth", w, h, NVR_EXT_SHARED_R32F);
		imageNormals = RtCore::AddImage("NVR RT path trace normals", w, h, NVR_EXT_SHARED_RGBA16F);
		imageAlbedo = RtCore::AddImage("NVR RT path trace albedo", w, h, NVR_EXT_SHARED_RGBA16F);
		imagePano = RtCore::AddImage("NVR RT path trace sky", 128, 128, NVR_EXT_SHARED_RGBA16F);	// (Reflections.h pano[]: 128 x 128)
		imageFrame = RtCore::AddImage("NVR RT path trace frame", w, h, NVR_EXT_SHARED_RGBA16F);
		imageMaterial = RtCore::AddImage("NVR RT path trace material", w, h, NVR_EXT_SHARED_RGBA16F);	// (2026-11-02 e) TESR_MaterialBuffer
		imageAlbedoOut = RtCore::AddImage("NVR RT path trace first albedo", w, h, NVR_EXT_SHARED_RGBA16F);	// (2026-11-03) PTDenoise's demodulation
		// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit's specular signal: only when it is on at the helper start (one more full-size image)
		imageSpec = TheSettingManager->GetSettingI(kSection, "PTDenoiseSplit") != 0 ? RtCore::AddImage("NVR RT path trace specular", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		imageLamps = RtCore::AddImage("NVR RT path trace lamps", kLightTexels, kLampRows, NVR_EXT_SHARED_RG32F);
		// (2026-11-07) the scratch store (binding 20): PTRestirDI's reservoirs, two sets of 16 bytes a pixel at the screen's size
		// + (2026-11-07) PTRadianceCache's table after them (64 bytes an entry; its size is fixed at the helper start: PTCacheEntriesLog2 needs a restart)
		cacheLog2Setup = (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "PTCacheEntriesLog2"), 12), 22);
		RtCore::AskScratch((UINT)((EmissiveBaseWords(w, h, cacheLog2Setup) * 4ull + 4ull * RtEmissiveRow::kScratchWords + (1ull << 20) - 1) >> 20) + 1u);	// (+ PTEmissive)
		emissiveState = RtEmissiveRow::State();
		restirState.valid = false;
		// (2026-11-04 d) PTRestirGI's images (9). (2026-11-05) Only when PTRestirGI is on at the helper start (off by default; turning it on
		// later needs a restart): the protocol holds 48 shared images and lamps + sun + AO + reflections + the path tracer asked for more.
		imageGIResult = -1;
		for (int i = 0; i < 2; i++) imageResA[i] = imageResB[i] = imageResC[i] = imageSurf[i] = -1;
		if (TheSettingManager->GetSettingI(kSection, "PTRestirGI") != 0) {
			imageGIResult = RtCore::AddImage("NVR RT path trace GI result", w, h, NVR_EXT_SHARED_RGBA16F);
			for (int i = 0; i < 2; i++) {
				imageResA[i] = RtCore::AddImage(i ? "NVR RT path trace GI reservoir A1" : "NVR RT path trace GI reservoir A0", w, h, NVR_EXT_SHARED_RG32F);
				imageResB[i] = RtCore::AddImage(i ? "NVR RT path trace GI reservoir B1" : "NVR RT path trace GI reservoir B0", w, h, NVR_EXT_SHARED_RG32F);
				imageResC[i] = RtCore::AddImage(i ? "NVR RT path trace GI reservoir C1" : "NVR RT path trace GI reservoir C0", w, h, NVR_EXT_SHARED_RGBA16F);
				imageSurf[i] = RtCore::AddImage(i ? "NVR RT path trace GI surface 1" : "NVR RT path trace GI surface 0", w, h, NVR_EXT_SHARED_RG32F);
			}
		}
		giHistory = false;	// (2026-11-04: + the light grid's rows)	// (2026-11-03) all the frame's lamps	// (2026-11-02 c) the game's frame at the core's point
		samples = 0;	// new images: the mean starts over
		DxvkCompute::Drop(showPipeline);	// (a helper restart by reloadshaders: the display pass's .spv may have changed too)
		showTried = false;
	}

	// Whether anything the mean depends on moved since the last traced frame (a still first-person camera keeps its matrix exactly;
	// the tolerances only absorb float noise). The projection's jitter (FSR / DLSS) is left out: only its scale counts.
	bool Moved(const D3DXVECTOR4& sun) {
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		const float key[8] = { (float)S.bounces + 100.0f * (float)S.bounceHalf + 1000.0f * (float)S.lastSky + 10000.0f * (float)S.lastBounce + 100000.0f * (float)S.lampFalloff + 1000000.0f * S.lampGain,
			S.sky + 100.0f * (float)S.restirDIRays + 1000.0f * (float)S.bounceLights, S.ambient,	// (perf-ptcost: + the ray budget keys)
			S.glow + (S.emissive > 0 ? 1000.0f : 0.0f) + 16.0f * S.materials.glass + 64.0f * S.materials.glassTint + 256.0f * S.materials.foliage + 1024.0f * S.materials.skin + 4096.0f * S.materials.sheen, S.clamp + 1000.0f * S.indirectClamp, (float)S.debugView + (S.primary ? 16.0f : 0.0f) + (S.specular ? 32.0f : 0.0f) + (S.glowLights ? 64.0f : 0.0f) + 128.0f * (float)S.gameLamps + (S.lightGrid ? 1024.0f : 0.0f) + 2048.0f * (float)S.bounceAlpha + 8192.0f * floorf(S.bounceDistance), proj._11, proj._22 };	// (2026-11-04: PTLightGrid too, so an A/B starts each mean fresh)
		bool moved = !lastValid;
		for (int i = 0; i < 16 && !moved; i++) moved = fabsf(((const float*)&view)[i] - ((const float*)&lastView)[i]) > 1.0e-4f;
		if (!moved) moved = fabsf(camera.x - lastCamera.x) > 0.05f || fabsf(camera.y - lastCamera.y) > 0.05f || fabsf(camera.z - lastCamera.z) > 0.05f;
		if (!moved) moved = fabsf(sun.x - lastSun.x) + fabsf(sun.y - lastSun.y) + fabsf(sun.z - lastSun.z) > 0.002f || sun.w != lastSun.w;
		for (int i = 0; i < 8 && !moved; i++) moved = key[i] != lastKey[i];
		lastView = view; lastCamera = camera; lastSun = sun;
		memcpy(lastKey, key, sizeof(key));
		lastValid = true;
		return moved;
	}

	void Record(IDirect3DDevice9* Device) {
		vmThisFrame = false;	// (2026-11-07 ptaaa-firstperson) set below when this frame's trace lights the view model
		if (imageA < 0 || imageB < 0 || imageDisplay < 0 || shader < 0) return;
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		if (!displayTexture || textureWidth != w || textureHeight != h) {
			DxvkCompute::Drop(displaySurface); DxvkCompute::Drop(displayTexture);
			TheTextureManager->InitTexture("NVR_RtPathTrace", &displayTexture, &displaySurface, w, h, D3DFMT_A16B16G16R16F);
			textureWidth = w;
			textureHeight = h;
			samples = 0;
			haveImage = false;
			if (!displayTexture) return;
		}
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		const bool exterior = TheShaderManager->GameState.isExterior;
		const bool sunUp = exterior && shadows && shadows->Constants.ShadowFade.x < 1.0f;
		const D3DXVECTOR4 sunDir = shadows ? shadows->Constants.SmoothedSunDir : D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);
		const D3DXVECTOR4 sunKey(sunDir.x, sunDir.y, sunDir.z, sunUp ? 1.0f : 0.0f);
		if (Moved(sunKey)) {
			if (samples > 0) resets++;
			samples = 0;
		}
		if (S.denoise) samples = 0;	// (2026-11-03) PTDenoise: every frame its own paths (the denoiser keeps the history, through camera moves)
		else if ((int)samples >= S.maxSamples) return;	// converged: the last copy stays on show, the helper rests

		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		PassConstants c = {};
		// (2026-11-03) RenderScale < 1: the frame is the top-left RenderRect part of the screen-size G-buffer: trace only that (its uv
		// = pixel / rect size is the screen's), into the top-left of the full-size display texture (Replace writes it texel for texel into
		// the rect-layout scene; the overlay after the upscale reads it at uv x the scale). The mean starts over when the rect changes.
		const UINT tw = RenderRect::Width(w), th = RenderRect::Height(h);
		if (tw != lastRect[0] || th != lastRect[1]) { lastRect[0] = tw; lastRect[1] = th; samples = 0; }
		c.size[0] = (float)tw; c.size[1] = (float)th; c.size[2] = 1.0f / tw; c.size[3] = 1.0f / th;
		if (TAAEffect* taa = TheShaderManager->Effects.TAA) { const D3DXVECTOR2 j = taa->DepthJitterUV(); c.rect[0] = j.x; c.rect[1] = j.y; }
		// as RtSunShadows: the view matrix's columns are right (0), up (1) and forward (2)
		c.forward[0] = view._13; c.forward[1] = view._23; c.forward[2] = view._33;
		auto constant = [](const char* name) { auto f = TheShaderManager->ConstantsTable.find(name); return f == TheShaderManager->ConstantsTable.end() ? nullptr : f->second; };
		auto texture = [](const char* name) -> IDirect3DBaseTexture9* { auto f = TheTextureManager->TextureNames.find(name); return f == TheTextureManager->TextureNames.end() || !f->second ? nullptr : *f->second; };
		const D3DXVECTOR4* cameraData = constant("TESR_CameraData");
		c.forward[3] = cameraData ? cameraData->y : 0.0f;	// farZ: the depth buffer's scale
		c.right[0] = view._11 / proj._11; c.right[1] = view._21 / proj._11; c.right[2] = view._31 / proj._11;
		c.up[0] = view._12 / proj._22; c.up[1] = view._22 / proj._22; c.up[2] = view._32 / proj._22;
		const D3DXVECTOR3 eye = RtCore::Eye();
		c.origin[0] = eye.x; c.origin[1] = eye.y; c.origin[2] = eye.z;
		c.origin[3] = (float)((TheFrameRateManager->FrameNumber * 7919u) & 0xFFFFFu);	// the random seed (exact in a float)
		const int alphaAtlas = RtCore::AlphaAtlasImage(), albedoAtlas = RtCore::AlbedoAtlasImage(), glowAtlas = RtCore::GlowAtlasImage();
		UInt32 flags = (RtCore::AlphaTested() && alphaAtlas >= 0 ? 1u : 0u) | (albedoAtlas >= 0 ? 2u : 0u) | (glowAtlas >= 0 && albedoAtlas >= 0 ? 4u : 0u);
		// (2026-11-02 b) PTPrimary: the screen's depth, normals and albedo (CapturedNormals' AlbedoBuffer: linear base colour, the land's
		// blended layers) for the first hit, when this frame's albedo capture ran
		const D3DXVECTOR4* albedoData = constant("TESR_AlbedoBufferData");
		IDirect3DBaseTexture9* gDepth = texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* gNormals = texture("TESR_NormalsBuffer");
		IDirect3DBaseTexture9* gAlbedo = texture("TESR_AlbedoBuffer");
		// (2026-11-07 ptaaa-firstperson) PTViewModel: the first-person model's G-buffer was captured this frame (CapturedNormals::VmFrame):
		// the normals image comes from ViewModelPrep (its pixels coded in the alpha); else TESR_NormalsBuffer as before (the show pass then
		// keeps the game's own pixels there)
		// (with PTDenoise only: the reference's running mean would smear a model that sways while the camera is still)
		const bool fullRect = tw == w && th == h;
		vmThisFrame = false;
		if (S.primary && S.viewModel && S.denoise && fullRect && gNormals && CapturedNormals::VmFrame == TheFrameRateManager->FrameNumber && albedoData && albedoData->x > 0.5f)
			vmThisFrame = ViewModelPrep(Device, gNormals, w, h);
		IDirect3DBaseTexture9* normalsIn = vmThisFrame ? (IDirect3DBaseTexture9*)vmNormals : gNormals;
		if (S.primary && imageDepth >= 0 && imageNormals >= 0 && imageAlbedo >= 0 && c.forward[3] > 0.0f && albedoData && albedoData->x > 0.5f &&
			gDepth && normalsIn && gAlbedo && RtCore::CopyIn(imageDepth, gDepth) && RtCore::CopyIn(imageNormals, normalsIn) && RtCore::CopyIn(imageAlbedo, gAlbedo))
			flags |= 8u;
		else vmThisFrame = false;
		// the next frame's first-person render captures the model while the path tracer can light it (the screen's G-buffer, the full rect)
		CapturedNormals::VmWanted = S.viewModel && S.primary && S.denoise && fullRect;
		// (2026-11-02 e) the screen's roughness / metal (CapturedNormals' MaterialBuffer) with the first hit's G-buffer
		const D3DXVECTOR4* materialData = constant("TESR_MaterialBufferData");
		IDirect3DBaseTexture9* gMaterial = texture("TESR_MaterialBuffer");
		if ((flags & 8u) && S.specular && imageMaterial >= 0 && materialData && materialData->x > 0.5f && gMaterial && RtCore::CopyIn(imageMaterial, gMaterial))
			flags |= 256u;
		if (S.specular) flags |= 128u;
		if ((S.gameLamps & 1) || S.gameLamps == 4) flags |= 2048u;	// (2026-11-03 rtspec) PTGameLamps: the game's falloff
		if (S.glowFromFrame) flags |= 8192u;	// (2026-11-05) PTGlowFromFrame
		// UNOFFICIAL 2026-11-07 (perf-ptcost FIX, the tester log of 11-07 19:45) PTRestirGI runs only where its images were made at the helper's
		// start (Setup): turned on later, the `gi` path below never ran, yet every `!S.restirGI` here turned ReSTIR DI, the radiance cache,
		// PTEmissive, PTBounceHalf and the denoiser split off: a cheaper, worse picture with nothing said (the tester's "10 ms"). Now the
		// setting counts only while it can run; the menu marks it "restart" (ImGuiManager kRestartSettings) and the log says so once.
		const bool giReady = S.restirGI && S.denoise && shaderGI >= 0 && shaderReuse >= 0 && imageGIResult >= 0 && imageResA[1] >= 0;
		{
			static bool told = false;
			if (S.restirGI && !giReady && imageGIResult < 0 && !told) {
				told = true;
				Logger::Log("UNOFFICIAL RT path trace: PTRestirGI is on but its images were not made at the RT helper's start: it starts with the next helper "
					"start (restart the game, or reloadshaders); until then the path tracer runs without it (ReSTIR DI, the radiance cache, PTEmissive stay on).");
			}
			if (giReady) told = false;
		}
		if (S.bounceHalf > 0 && !giReady) flags |= 16384u;	// (2026-11-06) PTBounceHalf (not with PTRestirGI: it moves the first direct light first)
		if (S.lastSky > 0) flags |= 32768u;	// (2026-11-06) PTLastSky
		if (S.lastBounce > 0) flags |= 65536u;	// (2026-11-06 b) PTLastBounce
		if (S.lampFalloff == 1) flags |= 131072u;	// (2026-11-06 c) PTLampFalloff physical
		if (S.denoise && imageAlbedoOut >= 0) flags |= 512u;	// (2026-11-03) demodulated for the denoiser
		// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: diffuse and specular apart (not with PTRestirGI: its variant has no split)
		if ((flags & 512u) && S.denoiseSplit && imageSpec >= 0 && !giReady) flags |= 4194304u;
		// (2026-11-02 b) the sky on misses: RT reflections' SkyPanorama (the sky as the screen saw it, clouds and all; needs the Reflections
		// effect with [Shaders.Reflections.RT] SkyPanorama), last frame's, while it is fresh; else the 2-colour gradient
		ReflectionsEffect* reflections = TheShaderManager->Effects.Reflections;
		IDirect3DTexture9* pano = exterior && reflections ? reflections->SkyPanoramaTexture(TheFrameRateManager->FrameNumber, 3) : nullptr;
		if (pano && imagePano >= 0 && RtCore::CopyIn(imagePano, pano))
			flags |= 16u;
		// (2026-11-02 c) the frame as drawn so far (the world, the sky; NVR's effects not yet): camera rays that leave the RT scene show it
		IDirect3DSurface9* frame = nullptr;
		if (S.primary && imageFrame >= 0 && SUCCEEDED(Device->GetRenderTarget(0, &frame)) && frame) {
			if (RtCore::CopyIn(imageFrame, frame)) flags |= 32u;
			frame->Release();
		}
		const int normalAtlas = RtCore::NormalAtlasImage();	// (2026-11-02 d) NormalTextures
		if (normalAtlas >= 0 && albedoAtlas >= 0) flags |= 64u;
		static UInt32 lastFlags = 0;
		if (flags != lastFlags) { samples = 0; lastFlags = flags; }	// a source came or went (the G-buffer, the panorama, an atlas): start over
		c.accum[0] = (float)samples; c.accum[1] = (float)S.bounces; c.accum[2] = (float)S.samples; c.accum[3] = (float)flags;
		const ShaderConstants& k = TheShaderManager->ShaderConst;
		memcpy(c.sunDir, &sunKey, 16);
		const float lightScale = S.gameLamps == 4 ? GameLightScale() : 1.0f;	// (2026-11-04 rtgap)
		c.sunColor[0] = Lin(k.sunColor.x) * lightScale; c.sunColor[1] = Lin(k.sunColor.y) * lightScale; c.sunColor[2] = Lin(k.sunColor.z) * lightScale;
		c.sunColor[3] = tanf(0.5f * 0.53f * 0.0174532925f);	// the sun's disc
		c.skyZenith[0] = Lin(k.skyColor.x) * S.sky; c.skyZenith[1] = Lin(k.skyColor.y) * S.sky; c.skyZenith[2] = Lin(k.skyColor.z) * S.sky;
		c.skyZenith[3] = exterior ? 1.0f : 0.0f;
		c.skyHorizon[0] = Lin(k.horizonColor.x) * S.sky; c.skyHorizon[1] = Lin(k.horizonColor.y) * S.sky; c.skyHorizon[2] = Lin(k.horizonColor.z) * S.sky;
		c.skyHorizon[3] = S.glow;
		c.ambient[0] = Lin(k.sunAmbient.x) * S.ambient; c.ambient[1] = Lin(k.sunAmbient.y) * S.ambient; c.ambient[2] = Lin(k.sunAmbient.z) * S.ambient;
		c.ambient[3] = S.sky;	// the panorama's scale
		c.misc[0] = (float)Lamps(c);
		// (2026-11-03) all the lamps through the lamp image (G32R32F, 4 texels a lamp), when it can be filled and copied this frame
		// (2026-11-03 b) PTGlowLights: the glow lights after the lamps (rect.z = how many)
		// (2026-11-07 ptaaa-emissive) PTEmissive: needs the glow (flag 4), the lamp image, its build pass and room for it before the trace
		const bool emissiveWanted = S.emissive > 0 && S.glow > 0.0f && shaderEmissive >= 0 && imageLamps >= 0 && (flags & 4u) && !giReady &&
			cacheLog2Setup > 0 && RtCore::PassesLeft() >= (S.radianceCache > 0 ? 3u : 2u);	// (the build, the trace, the cache's resolve)
		const int glowCount = Glows(emissiveWanted);
		const bool restir = S.restirDI > 0 && (flags & 512u) && !giReady;	// (2026-11-07) PTRestirDI needs the lamp image (its frame row)
		const bool cache = S.radianceCache > 0 && shaderCache >= 0 && cacheLog2Setup > 0 && !giReady;	// (2026-11-07) PTRadianceCache too
		const bool energy = S.specular;	// (2026-11-07) PTSpecular's energy table rides in the frame row (section 49)
		// (2026-11-07 rtgap-tess; RT-DETAIL-GAPS.md section 1, RT-PATH-TRACING.md section 56) the bumps' slack (1a) and PTContactShadows (1b)
		// ride in the frame row's texels 16-21 (RtTessContact.h): they need the lamp image too
		RtTessContact::Slack slack;
		{ float v[3]; DxvkCompute::DisplaceRtSlack(v); slack.strength = v[0]; slack.fadeStart = v[1]; slack.fadeEnd = v[2]; }
		const RtTessContact::Contact contact = RtTessContact::Make((flags & 8u) ? S.contact : 0.0f, S.contactLength);	// (the screen's G-buffer: flag 8)
		const bool tess = slack.strength > 0.0f || contact.strength > 0.0f;
		const bool materials = RtMaterials::Any(S.materials);	// (2026-11-07 ptaaa-materials) their keys ride in the frame row (texels 24-26)
		emissiveCount = 0;
		if (imageLamps >= 0 && (lampCount > 8 || glowCount > 0 || restir || cache || energy || tess || materials || emissiveWanted)) {
			if (!lampTexture) TheRenderManager->device->CreateTexture(kLightTexels, kLampRows, 1, D3DUSAGE_DYNAMIC, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &lampTexture, nullptr);
			D3DLOCKED_RECT r = {};
			if (lampTexture && SUCCEEDED(lampTexture->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) {
				float* t = (float*)r.pBits;
				memset(t, 0, sizeof(float) * 2 * kLightTexels);
				// (2026-11-04) PTLightGrid: the grid in rows 1.. as one run of texels (only when the rows are tight: else the all-lamp loop)
				static std::vector<float> grid;
				bool gridOn = false;
				if (S.lightGrid && shaderGrid >= 0 && r.Pitch == (INT)(sizeof(float) * 2 * kLightTexels)) {
					grid.assign(2 * (size_t)kGridTexels, 0.0f);
					gridOn = BuildGrid(c, grid.data());
					if (gridOn) memcpy((char*)r.pBits + r.Pitch, grid.data(), sizeof(float) * 2 * ((size_t)gridInfo.cells + (gridInfo.entries + 1) / 2));
				}
				for (int i = 0; i < lampCount; i++) {
					const LampData& d = lampData[i];
					const float v[8] = { d.pos[0], d.pos[1], d.pos[2], d.pos[3], d.colour[0], d.colour[1], d.colour[2], 0.0f };
					memcpy(t + 8 * i, v, sizeof(v));
				}
				for (int i = 0; i < glowCount; i++) {
					const GlowData& d = glowData[i];
					const float v[8] = { d.pos[0], d.pos[1], d.pos[2], d.pos[3], d.colour[0], d.colour[1], d.colour[2], 0.0f };
					memcpy(t + 8 * (lampCount + i), v, sizeof(v));
				}
				if (restir || cache || energy) FrameRow(c, (float*)((char*)r.pBits + (size_t)r.Pitch * kFrameRow), t, tw, th);
				{	// (2026-11-07) the flashlight's cone (lamp 0) in the frame row's texel 15 (RtPathTrace.comp LampSpot; RtRestirRow fills 0-14)
					float* row = (float*)((char*)r.pBits + (size_t)r.Pitch * kFrameRow);
					row[30] = lampCount > 0 ? lampData[0].colour[3] : 0.0f; row[31] = 0.0f;
					RtTessContact::PackRow(row, slack, contact);	// (2026-11-07 rtgap-tess) texels 16-21
					{	// (2026-11-07 ptaaa-materials) texels 24-26; (2026-11-09 pt-mirrors) + PTDenoiseMirrorSurface, only while the split specular
						// light goes through the path tracer's own denoiser (not to DLSS RR, whose specular albedo must stay the BRDF's)
						RtMaterials::Keys keys = S.materials;
						keys.mirrorSurface = (flags & 4194304u) && S.denoise && !rrDenoising ? S.denoiseMirrorSurface : 0.0f;
						RtMaterials::PackRow(row, keys);
					}
					row[54] = S.bounceRoughen; row[55] = 0.0f;	// UNOFFICIAL 2026-11-09 PTBounceRoughen: texel 27 (RtPathTrace.comp LoadBounceRoughen)
					{	// (2026-11-07 perf-ptcost) texels 28-29: the ray budget keys + whether this frame's scene holds the hidden first-person body
						RtPtCost::Keys cost;
						cost.restirRays = (float)S.restirDIRays; cost.bounceLights = (float)S.bounceLights;
						cost.oneCameraQuery = RtCore::HiddenInstances() == 0 ? 1.0f : 0.0f;
						RtPtCost::PackRow(row, cost);
					}
					// (2026-11-07 ptaaa-emissive) PTEmissive: the emissive row (RtEmissiveRow.h), the records' numbers, texel 22 (how many, where)
					float* emRow = (float*)((char*)r.pBits + (size_t)r.Pitch * kEmissiveRow);
					memset(emRow, 0, sizeof(float) * 2 * kLightTexels);
					if (emissiveWanted) {
						static std::vector<RtEmissiveRow::Source> sources;
						static std::vector<RtEmissiveRow::Mark> marks;
						sources.clear();
						for (const RtCore::GlowLight& g : RtCore::GlowLights()) {
							RtEmissiveRow::Source s;
							if (!g.triangles || !RtCore::EmissiveTransform(g.record, s.transform)) continue;
							s.record = g.record; s.geometryId = g.geometryId; s.triangles = g.triangles;
							for (int k = 0; k < 3; k++) { s.center[k] = g.center[k]; s.rgb[k] = g.rgb[k]; }
							s.radius = g.radius;
							sources.push_back(s);
						}
						const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
						const double camWorld[3] = { camera.x, camera.y, camera.z };
						emissiveCount = RtEmissiveRow::Build(emissiveState, sources.data(), (int)sources.size(), S.emissiveMax, (UInt32)S.emissiveTriangles, camWorld, emRow, marks);
						for (const RtEmissiveRow::Mark& m : marks) RtCore::MarkEmissive(m.record, m.number);
					}
					else emissiveState = RtEmissiveRow::State();
					row[2 * RtEmissiveRow::kFrameTexel] = (float)emissiveCount;
					row[2 * RtEmissiveRow::kFrameTexel + 1] = (float)(EmissiveBaseWords(w, h, cacheLog2Setup) / 4096ull);
				}
				lampTexture->UnlockRect(0);
				if (RtCore::CopyIn(imageLamps, lampTexture)) { flags |= 1024u | (gridOn ? 4096u : 0u) | (restir ? 262144u : 0u) | (cache ? 524288u : 0u) | (restir || cache || energy ? 1048576u : 0u); c.misc[0] = (float)lampCount; c.rect[2] = (float)glowCount; }
				else restirState.valid = false;
			}
		}
		if (!(flags & 262144u)) restirState.valid = false;
		c.accum[3] = (float)flags;	// (the lamp image's flag 1024 comes after the reset check: a lamp list change does not restart the mean)
		// (2026-11-03 b) FIX: these three were inside the comment above since 940b1ef2 (the lamp image), so the ray length was 0: no
		// traced hit at all (bounce rays all "missed": outdoors they took the sky, indoors nothing), PTClamp and PTDebugView did nothing
		c.misc[1] = S.clamp; c.misc[2] = 60000.0f; c.misc[3] = (float)S.debugView;
		c.rect[3] = (float)S.bounceAlpha; c.grid[2][2] = S.bounceDistance;	// (2026-11-04 b: the block is full, these were free)
		c.grid[2][3] = S.indirectClamp;	// (2026-11-04 c: the last free float)

		// images: the mean (2), the display copy, then the atlases (a written image stands in for a missing one: its flag is off)
		const int images[15] = { imageA, imageB, imageDisplay, flags & 1u ? alphaAtlas : imageDisplay, albedoAtlas >= 0 ? albedoAtlas : imageDisplay,
			flags & 4u ? glowAtlas : albedoAtlas >= 0 ? albedoAtlas : imageDisplay, imageDepth, imageNormals, imageAlbedo, imagePano, imageFrame,
			flags & 64u ? normalAtlas : albedoAtlas >= 0 ? albedoAtlas : imageDisplay, imageMaterial, imageAlbedoOut, imageLamps };
		// (2026-11-04 d) PTRestirGI (with PTDenoise): the trace's variant writes the first-bounce sample out, RtRestirGI.spv reuses it
		const bool gi = giReady && (flags & 512u);
		if (gi) {
			const int images16[16] = { imageA, imageB, imageDisplay, images[3], images[4], images[5], imageDepth, imageNormals, imageAlbedo, imagePano,
				imageFrame, images[11], imageMaterial, imageAlbedoOut, imageLamps, imageGIResult };
			if (!RtCore::AddPass(shaderGI, images16, 16, 0x7u | (1u << 13) | (1u << 15), &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
			const int cur = (int)(giFrame & 1u), prev = cur ^ 1;
			RestirConstants r = {};
			memcpy(r.size, c.size, 16); memcpy(r.forward, c.forward, 16); r.forward[3] = 0.0f; memcpy(r.right, c.right, 16); memcpy(r.up, c.up, 16);
			memcpy(r.origin, c.origin, 16);
			r.rect[0] = c.rect[0]; r.rect[1] = c.rect[1];
			// UNOFFICIAL 2026-11-09 (rt-review; docs/next/RT-PT-REVIEW-2026-11-09.md P1, P6) the reservoirs of another rect (a RenderScale
			// change, rect <-> native) hold other texels: no history that frame, as ReSTIR DI's row does it (RtRestirRow.h)
			if (giPrevSize[0] != tw || giPrevSize[1] != th) giHistory = false;
			r.rect[2] = (float)((flags & 1u ? 1u : 0u) | (giHistory ? 2u : 0u) | 4u);
			memcpy(r.prevForward, giPrevCamera[0], 16); memcpy(r.prevRight, giPrevCamera[1], 16); memcpy(r.prevUp, giPrevCamera[2], 16);
			// (P1) last frame's eye in THIS frame's scene space, minus this frame's eye. The scene is camera-relative, so that is the camera's
			// world move back. It was giPrevCamera[3] - c.origin with both the scene-space eye (0 - 0): the reuse reprojected the view's
			// turn only, and every kept sample slid along with the camera while walking.
			const D3DXVECTOR4& giCamera = TheRenderManager->CameraPosition;
			const float giWorld[3] = { giCamera.x, giCamera.y, giCamera.z };
			for (int k = 0; k < 3; k++) r.prevEye[k] = giHistory ? giPrevCamera[3][k] - giWorld[k] : 0.0f;
			r.prevEye[3] = S.restirMaxM;
			r.params[0] = S.restirSpatial; r.params[1] = S.restirRadius; r.params[2] = S.restirDepth; r.params[3] = S.restirNormal;
			const int reuseImages[14] = { imageA, imageB, imageDisplay, imageGIResult, imageAlbedoOut, images[3], imageResA[prev], imageResB[prev], imageResC[prev],
				imageResA[cur], imageResB[cur], imageResC[cur], imageSurf[prev], imageSurf[cur] };
			if (!RtCore::AddPass(shaderReuse, reuseImages, 14, (1u << 3) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 13), &r, sizeof(r), (tw + 7) / 8, (th + 7) / 8)) { giHistory = false; return; }
			memcpy(giPrevCamera[0], c.forward, 16); memcpy(giPrevCamera[1], c.right, 16); memcpy(giPrevCamera[2], c.up, 16);
			giPrevCamera[3][0] = giWorld[0]; giPrevCamera[3][1] = giWorld[1]; giPrevCamera[3][2] = giWorld[2]; giPrevCamera[3][3] = 0.0f;
			giPrevSize[0] = tw; giPrevSize[1] = th;
			giFrame++;
			giHistory = true;
			if (!RtCore::CopyOut(imageGIResult, displaySurface, tw, th)) return;
		}
		else {
			giHistory = false;
			if (emissiveCount > 0 && (flags & 1024u)) {	// (2026-11-07 ptaaa-emissive) PTEmissive's list, before the trace
				const EmissiveConstants ec = { { (float)emissiveCount, (float)(EmissiveBaseWords(w, h, cacheLog2Setup) / 4096ull), 0.0f, 0.0f } };
				const int emImages[2] = { imageLamps, glowAtlas };
				// (2026-11-07 ptaaa-hangfix) the lamp image already went in with the count (texel 22): when the build pass is not added, flag
				// 8388608 tells the trace the list was not rebuilt this frame (RtPathTrace.comp LoadScratchLayout: no emissive light), so it
				// never walks last frame's blocks against this frame's records
				if (!RtCore::AddPass(shaderEmissive, emImages, 2, 0u, &ec, sizeof(ec), (UINT)emissiveCount, 1)) { emissiveCount = 0; c.accum[3] = (float)(flags | 8388608u); }
			}
			// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: + the specular image (pass image 15, written; the mean images carry its albedo)
			const bool split = (flags & 4194304u) != 0u;
			int images16[16];
			memcpy(images16, images, sizeof(images));
			images16[15] = split ? imageSpec : imageDisplay;
			if (!RtCore::AddPass(flags & 4096u ? shaderGrid : shader, images16, split ? 16 : 15, 0x7u | (flags & 512u ? 1u << 13 : 0u) | (split ? 1u << 15 : 0u), &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
			if (flags & 524288u) {	// (2026-11-07) PTRadianceCache: fold this frame's samples into the cells (RtPathTraceCache.comp)
				const UINT entries = 1u << cacheLog2Setup;
				// (2026-11-07 perf-history) PTCacheMaxFrames is frames at 60 fps (the lag behind a change of light): the same seconds here.
				// PTCacheMaxAge (when an unseen cell is freed) and PTCacheMaxSamples (a noise cap) stay frames / samples.
				const float params[8] = { (float)entries, (float)(CacheBaseWords(w, h) / 4096ull), S.cacheMaxSamples, S.cacheMaxAge,
					RtHistoryTime::Frames(S.cacheMaxFrames, RtDenoise::HistoryClock()), 0.0f, 0.0f, 0.0f };
				if (!RtCore::AddPass(shaderCache, nullptr, 0, 0u, params, sizeof(params), (entries + 63) / 64, 1)) {
					static int logged = 0;
					if (logged++ < 4) Logger::Log("UNOFFICIAL RT path trace: the radiance cache's resolve pass did not fit this frame (the frame's passes are full): the cache keeps last frame's light.");
				}
			}
			if (!RtCore::CopyOut(imageDisplay, displaySurface, tw, th)) return;
		}
		tracedScale[0] = (float)tw / (float)w; tracedScale[1] = (float)th / (float)h;
		albedoThisFrame = false;
		if (flags & 512u) {	// (2026-11-03) the first surface's colour for the show pass (demodulation)
			if (!albedoTexture) TheTextureManager->InitTexture("NVR_RtPathTraceAlbedo", &albedoTexture, &albedoSurface, w, h, D3DFMT_A16B16G16R16F);
			albedoThisFrame = albedoSurface && RtCore::CopyOut(imageAlbedoOut, albedoSurface, tw, th);
		}
		// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: the specular light and its albedo back (render-target copies, made at the screen's size)
		splitThisFrame = false;
		if ((flags & 4194304u) && albedoThisFrame) {
			auto make = [&](IDirect3DTexture9*& t, IDirect3DSurface9*& s, D3DFORMAT format) {
				if (t) { D3DSURFACE_DESC d = {}; t->GetLevelDesc(0, &d); if (d.Width != w || d.Height != h) { DxvkCompute::Drop(s); DxvkCompute::Drop(t); } }
				if (!t && SUCCEEDED(TheRenderManager->device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t, nullptr)) && t) t->GetSurfaceLevel(0, &s);
				return s != nullptr;
			};
			splitThisFrame = make(specTexture, specSurface, D3DFMT_A16B16G16R16F) && make(specAlbedoA, specAlbedoASurface, D3DFMT_G32R32F) &&
				make(specAlbedoB, specAlbedoBSurface, D3DFMT_G32R32F) && RtCore::CopyOut(imageSpec, specSurface, tw, th) &&
				RtCore::CopyOut(imageA, specAlbedoASurface, tw, th) && RtCore::CopyOut(imageB, specAlbedoBSurface, tw, th);
		}
		if (!S.denoise) samples += (UInt32)S.samples;	// (2026-11-03: PTDenoise keeps 0, so leaving it starts a fresh mean; rtpt-8c)
		haveImage = true;
		static UInt32 logged = 0;
		if (samples == (UInt32)S.samples && logged < 8) {
			logged++;
			Logger::Log("UNOFFICIAL RT path trace: started (%ux%u, %d bounces, %d paths a pixel and frame, %s, %d lamps, sun %s, albedo %s, glow %s, alpha test %s, "
				"first hit %s, sky %s, %d glow lights, %d emissive instances, light grid %s %dx%dx%d cells of %.0fx%.0fx%.0f, %d list entries; the game's lamps: %d lit this view, %d not hidden; ReSTIR DI %s, radiance cache %s).", w, h, S.bounces, S.samples, exterior ? "outdoors: sky on misses" : "indoors", (int)c.misc[0], sunUp ? "up" : "down / indoors",
				flags & 2u ? "atlas" : "averages", flags & 4u ? "atlas" : "none", flags & 1u ? "on" : "off", flags & 8u ? "the screen's G-buffer" : "traced",
				flags & 16u ? "the reflections' panorama" : "gradient", (int)c.rect[2], emissiveCount, flags & 4096u ? "on" : "off", gridInfo.dims[0], gridInfo.dims[1], gridInfo.dims[2],
				gridInfo.cell[0], gridInfo.cell[1], gridInfo.cell[2], gridInfo.entries, (int)ManyLamps::ActiveLamps.size(), (int)ManyLamps::AllLamps.size(), flags & 262144u ? "on" : "off", flags & 524288u ? "on" : "off");
		}
		if (samples == 64u || samples == 256u || samples == 1024u || samples == 4096u)
			Logger::Log("UNOFFICIAL RT path trace: %u samples a pixel (camera still; %u restarts so far).", samples, resets);
	}

	ID3D9NvrExtPipeline* ShowPipeline() {
		if (showTried) return showPipeline;
		showTried = true;
		std::vector<char> spirv;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::LoadSpirv("RtPathTraceShow", spirv)) return nullptr;
		NVR_EXT_BINDING_DESC bindings[11] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE, 3, NVR_EXT_VIEW_2D, FALSE, 0 },	// (2026-11-07) TESR_DepthBufferViewModel
			{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE, 4, NVR_EXT_VIEW_2D, FALSE, 0 },	// (2026-11-07) PTDetail: the game's frame before the replace
			{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE, 5, NVR_EXT_VIEW_2D, FALSE, 0 },	// (2026-11-07) TESR_DepthBuffer
			{ 1, 6, NVR_EXT_BIND_SAMPLED_IMAGE, 6, NVR_EXT_VIEW_2D, FALSE, 0 },	// (2026-11-07 ptaaa-denoiser) the specular light shown
			{ 1, 7, NVR_EXT_BIND_SAMPLED_IMAGE, 7, NVR_EXT_VIEW_2D, FALSE, 0 },	// its raw trace
			{ 1, 8, NVR_EXT_BIND_SAMPLED_IMAGE, 8, NVR_EXT_VIEW_2D, FALSE, 0 },	// its albedo r, g
			{ 1, 9, NVR_EXT_BIND_SAMPLED_IMAGE, 9, NVR_EXT_VIEW_2D, FALSE, 0 },	// its albedo b, roughness
			{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 4 },
		};
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data();
		desc.FragmentSpirvSize = (UINT)spirv.size();
		desc.BindingCount = 11;
		desc.pBindings = bindings;
		desc.PushConstantSize = 48;
		desc.UsesSamplerHeap = TRUE;
		desc.SamplerHeapSet = 0;
		desc.SamplerHeapBinding = 0;
		desc.ColorTargetCount = 1;
		desc.Blend = NVR_EXT_BLEND_NONE;
		// (2026-11-06) colour only: under PTReplace the scene's alpha carries the materials MaterialAware reflections read (roughness, metal;
		// ReflectionsMaterial.frag); writing 1 there wiped them, so with PTRealtime the test room's mirrors reflected as rough grey stone
		desc.ColorWriteMask = 0x7u;
		desc.pDebugName = "RtPathTraceShow";
		if (FAILED(ext->CreateGraphicsPipeline(&desc, &showPipeline))) {
			showPipeline = nullptr;
			Logger::Log("UNOFFICIAL RT path trace: the display pass could not be made (Compute\\RtPathTraceShow.spv; the DXVK log says why)");
		}
		return showPipeline;
	}

	// The traced picture (denoised with PTDenoise) into Target: over the finished frame (hdr false: clamped, gamma, PTExposure) or into
	// the HDR scene (hdr true, PTReplace: the stored curve x^(1/2.2), not clamped above 1)
	void Show(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool hdr) {
		if (!S.enabled || !haveImage || !displayTexture || !RenderTarget || !RtCore::Running()) return;
		IDirect3DTexture9* shown = displayTexture;
		IDirect3DTexture9* specShown = nullptr;	// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: the specular light shown (denoised, or raw)
		// (2026-11-07) PTDetail / PTDenoiseGuide: the game's own lit frame, copied before the traced picture replaces it (the scene as stored)
		IDirect3DTexture9* gameFrame = nullptr;
		if (hdr && (S.detail > 0.0f || S.denoiseGuide > 0.0f)) {
			D3DSURFACE_DESC rd = {};
			RenderTarget->GetDesc(&rd);
			static IDirect3DTexture9* copy = nullptr;
			static IDirect3DSurface9* copySurface = nullptr;
			static UINT cw = 0, ch = 0; static D3DFORMAT cf = D3DFMT_UNKNOWN;
			if (!copy || cw != rd.Width || ch != rd.Height || cf != rd.Format) {
				DxvkCompute::Drop(copySurface); DxvkCompute::Drop(copy);
				cw = rd.Width; ch = rd.Height; cf = rd.Format;
				if (SUCCEEDED(Device->CreateTexture(cw, ch, 1, D3DUSAGE_RENDERTARGET, cf, D3DPOOL_DEFAULT, &copy, nullptr)) && copy) copy->GetSurfaceLevel(0, &copySurface);
			}
			if (copySurface && SUCCEEDED(Device->StretchRect(RenderTarget, nullptr, copySurface, nullptr, D3DTEXF_POINT))) gameFrame = copy;
		}
		// UNOFFICIAL 2026-11-03 (rtgi-cal) PTDebugView 6 / 7: RT GI's bounce light ([Shaders.AmbientOcclusion.RT] GI: NVR_RTGIRaw / the shared
		// RT denoiser's), shown through this same pass (x PTExposure, gamma): the same units and encoding as PTDebugView 5, the
		// reference's arriving one-bounce light, so the two compare pixel for pixel. Nothing shown while RT GI is not traced.
		if (S.debugView == 6 || S.debugView == 7) {
			AmbientOcclusionEffect* ao = TheShaderManager->Effects.AmbientOcclusion;
			IDirect3DTexture9* gi = ao ? (S.debugView == 7 ? ao->RtDenoisedGI(Device) : nullptr) : nullptr;
			if (!gi && ao) gi = ao->RtGI();
			if (!gi) return;
			shown = gi;
		}
		else if (S.denoise) {	// (2026-11-03) PTDenoise: the frame's noisy paths through the shared RT denoiser (temporal + a-trous, RGB)
			D3DSURFACE_DESC desc = {};
			displayTexture->GetLevelDesc(0, &desc);
			static UINT dw = 0, dh = 0;
			if (!denoisedTexture || dw != desc.Width || dh != desc.Height) {
				dw = desc.Width; dh = desc.Height;
				DxvkCompute::Drop(denoisedSurface); DxvkCompute::Drop(denoisedTexture);
				Device->CreateTexture(dw, dh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &denoisedTexture, nullptr);
				if (denoisedTexture) denoisedTexture->GetSurfaceLevel(0, &denoisedSurface);
				DxvkCompute::Drop(specDenoisedSurface); DxvkCompute::Drop(specDenoised);	// (ptaaa-denoiser)
				Device->CreateTexture(dw, dh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &specDenoised, nullptr);
				if (specDenoised) specDenoised->GetSurfaceLevel(0, &specDenoisedSurface);
			}
			// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) PTDenoiseUnderRr 0: while DLSS Ray Reconstruction denoises the frame (Upscaler::
			// RayReconstructionWantsRaw: RR delivered last frame, RRKeepDenoisers off) the path tracer's raw paths go into the scene and RR
			// denoises them with the path tracer's own albedos as its guides (TAAFSR.cpp RayReconstruct, RtPathTrace::RrGuides). Each start
			// and stop is logged; the own denoiser's history restarts after RR frames.
			const bool rrRaw = S.denoiseUnderRr == 0 && Upscaler::RayReconstructionWantsRaw();
			if (rrRaw != rrDenoising) {
				rrSwitches++;
				if (rrSwitches <= 40 || (rrSwitches % 100) == 0) {
					if (rrRaw) Logger::Log("UNOFFICIAL DLSS RR: starts denoising the path tracer (its raw paths go into the scene; RR's diffuse albedo%s are the path "
						"tracer's own; its own denoiser steps aside; switch %u)", splitThisFrame ? ", specular albedo and roughness" : " (the specular albedo: the G-buffer's)", rrSwitches);
					else Logger::Log("UNOFFICIAL DLSS RR: stops denoising the path tracer (its own denoiser is back, its history restarted; switch %u)", rrSwitches);
				}
				rrDenoising = rrRaw;
			}
			static bool hadRr = false;
			const bool restart = hadRr && !rrRaw;
			hadRr = rrRaw;
			if (!denoiser) denoiser = RtDenoise::Create("RT path trace");
			RtDenoise::Settings settings;
			settings.Channels = 3;
			settings.IgnoreRr = true;	// (the switch is ours: rrRaw above)
			settings.Reset = restart;
			settings.ObjectMotion = S.denoiseObjectMotion;	// (2026-11-07 ptaaa-denoiser)
			settings.MovingHistory = S.denoiseObjectMotion ? S.denoiseMovingHistory : 0;
			settings.HistoryFix = S.denoiseHistoryFix;	// (2026-11-07 ptaaa-denoiser) the lead's RT playground: speckle trails behind a moving lamp / object
			settings.MeanVariance = S.denoiseMeanVariance;	// (2026-11-07 ptaaa-denoiser) the playground's chain-link wire shadow
			settings.AntiFirefly = S.denoiseAntiFirefly > 0 ? S.denoiseFireflyK + (S.denoiseFireflyK > 0.0f ? 128.0f * (float)S.denoiseFireflySpread : 0.0f) : 0.0f;	// (2026-11-09: + 128 x PTDenoiseFireflySpread, the shaders' packing)
			//	// (2026-11-07 ptaaa-denoiser) the playground's fireflies
			settings.MaxHistory = S.denoiseHistory;
			settings.PeopleHistory = S.denoisePeopleHistory;
			settings.AntiLag = S.denoiseAntiLag;	// (2026-11-07) testers: "a lot of ghosting"
			if (gameFrame && albedoThisFrame && albedoTexture && S.denoiseGuide > 0.0f) {	// (2026-11-07) PTDenoiseGuide
				settings.Guide = gameFrame; settings.GuideAlbedo = albedoTexture; settings.GuideSigma = S.denoiseGuide;
			}	// (2026-11-07) a tester's dialogue ghosting: a talking face, 32 frames of history
			if (vmThisFrame && vmNormals) settings.People = vmNormals;	// (2026-11-07 ptaaa-firstperson) people + the view model (screen-locked history)
			settings.HistoryClamp = S.denoiseClamp;	// (2026-11-06)	// (2026-11-04 c: test keys; 32 frames as before, 2 a-trous passes since section 30, was 4)
			settings.Iterations = S.denoiseIterations;
			settings.ValueSigma = S.denoiseSigma;	// (2026-11-06) PTDenoiseSigma (4 = RtDenoise's default, as before)
			if (!rrRaw && denoisedSurface && RtDenoise::Run(denoiser, Device, displayTexture, denoisedSurface, settings)) shown = denoisedTexture;
			// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: the specular light through its own denoiser (the specular mode: hit-distance
			// reprojection, roughness-shaped smoothing; no guide: the game's shading edges are the diffuse light's)
			specShown = splitThisFrame ? specTexture : nullptr;
			if (splitThisFrame && !rrRaw && specDenoisedSurface) {
				if (!specDenoiser) specDenoiser = RtDenoise::Create("RT path trace specular");
				RtDenoise::Settings spec = settings;
				spec.Guide = nullptr; spec.GuideAlbedo = nullptr; spec.GuideSigma = 0.0f;
				spec.Specular = true;
				spec.Roughness = specAlbedoB;
				spec.MirrorHistory = S.denoiseMirrorHistory;
				spec.Virtual = S.denoiseVirtual;
				spec.MirrorSurface = S.denoiseMirrorSurface > 0.0f;	// (2026-11-09 pt-mirrors; the trace marks the pixels, RtPathTrace.comp)
				if (RtDenoise::Run(specDenoiser, Device, specTexture, specDenoisedSurface, spec)) specShown = specDenoised;
			}
		}
		ID3D9NvrExtPipeline* pipeline = ShowPipeline();
		ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
		if (!pipeline || !point) return;
		if (!parkSurface) {
			Device->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &parkTexture, nullptr);
			if (parkTexture) parkTexture->GetSurfaceLevel(0, &parkSurface);
			if (!parkSurface) return;
		}
		static GpuTimer timer("  RT path trace show");
		GpuProfileScope gpu(timer, Device);
		IDirect3DSurface9* current = nullptr;
		Device->GetRenderTarget(0, &current);
		Device->SetRenderTarget(0, parkSurface);
		// (2026-11-03) the denoised light x the first surface's colour. (2026-11-07 ptaaa-denoiser FIX) whenever the trace wrote demodulated
		// light (PTDenoise), also when the denoiser did not run (it failed, or DLSS RR takes the raw paths): it showed the light / colour then
		const bool modulate = S.denoise && (shown == denoisedTexture || shown == displayTexture) && albedoThisFrame && albedoTexture;
		const bool splitShow = modulate && specShown && splitThisFrame;	// (ptaaa-denoiser) + the specular light x its albedo
		struct { float exposure; UInt32 samplerIndex; float split, gamma; UInt32 modulate, pad[3]; float detail, detailRadius, texel[2]; } push = { S.exposure, 0, S.split,
			S.debugView == 1 || S.debugView == 2 || S.debugView == 4 ? 1.0f : 1.0f / 2.2f, (modulate ? 1u : 0u) | (hdr ? 2u : 0u) | (splitShow ? 4u : 0u), { 0, 0, 0 }, 0.0f, 4.0f, { 0.0f, 0.0f } };
		IDirect3DBaseTexture9* depthBuffer = nullptr;
		{ auto found = TheTextureManager->TextureNames.find("TESR_DepthBuffer"); if (found != TheTextureManager->TextureNames.end() && found->second) depthBuffer = *found->second; }
		if (gameFrame && depthBuffer && S.detail > 0.0f && S.debugView == 0) {	// (2026-11-07) PTDetail
			D3DSURFACE_DESC td = {};
			RenderTarget->GetDesc(&td);
			push.detail = S.detail; push.detailRadius = S.detailRadius;
			push.texel[0] = 1.0f / (float)(std::max)(td.Width, 1u); push.texel[1] = 1.0f / (float)(std::max)(td.Height, 1u);
		}
		// (2026-11-03) the overlay after a RenderScale upscale reads the rect-layout trace at uv x scale (into the scene: texel for texel)
		const float uvScale[2] = { hdr ? 1.0f : tracedScale[0], hdr ? 1.0f : tracedScale[1] };
		memcpy(push.pad, uvScale, sizeof(uvScale));
		// UNOFFICIAL 2026-11-07 (testers: "the first-person model is see-through", hands that show the mirror behind them, the gun gone
		// under PTRealtime) the first-person model (arms, gun, Pip-Boy) is drawn before this point but is not in the traced scene, and the
		// G-buffer the trace starts from is the world's: under it the trace saw the world behind, and this pass painted that over the
		// model. Now the model's pixels keep the game's own (TESR_DepthBufferViewModel: 1 = normal depth, the model where < 0.9; 2 =
		// reversed, where > 0.01, as Reflections' IsWeapon)
		// (2026-11-07 ptaaa-firstperson) PTViewModel: when this frame's trace lit the model (its G-buffer captured, ViewModelPrep), every
		// pixel is the trace's: the model's own lit by the path tracer, its not-captured ones the game's frame (the trace's exact answer there)
		IDirect3DTexture9* viewModel = TheTextureManager->DepthTextureViewModel;
		push.pad[2] = viewModel && !(vmThisFrame && hdr) ? (TheRenderManager->DepthConstants.z == 0.0f ? 1u : 2u) : 0u;
		NVR_EXT_RESOURCE_BINDING bindings[11] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = shown;
		bindings[1].Slot = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].pResource = modulate ? albedoTexture : shown;
		bindings[2].Slot = 2; bindings[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[2].pResource = displayTexture;	// (the raw trace: sky / frame pixels)
		bindings[3].Slot = 3; bindings[3].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[3].pResource = viewModel ? (IDirect3DBaseTexture9*)viewModel : (IDirect3DBaseTexture9*)displayTexture;
		bindings[4].Slot = 4; bindings[4].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[4].pResource = gameFrame ? (IDirect3DBaseTexture9*)gameFrame : (IDirect3DBaseTexture9*)displayTexture;
		bindings[5].Slot = 5; bindings[5].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[5].pResource = depthBuffer ? depthBuffer : (IDirect3DBaseTexture9*)displayTexture;
		// (2026-11-07 ptaaa-denoiser) the specular light (shown, raw) and its albedo (r, g | b, roughness); stand-ins without the split
		IDirect3DBaseTexture9* const specImages[4] = { splitShow ? specShown : displayTexture, splitShow ? specTexture : displayTexture,
			splitShow ? specAlbedoA : displayTexture, splitShow ? specAlbedoB : displayTexture };
		for (UINT k = 0; k < 4; k++) { bindings[6 + k].Slot = 6 + k; bindings[6 + k].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[6 + k].pResource = specImages[k]; }
		bindings[10].Slot = 0; bindings[10].Type = NVR_EXT_BIND_SAMPLER; bindings[10].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = RenderTarget;
		draw.BindingCount = 11;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		DxvkCompute::Extension()->Draw(&draw);
		Device->SetRenderTarget(0, current);
		if (current) current->Release();
	}
}

namespace RtPathTrace {
	void Register() {
		shader = RtCore::AddShader("RtPathTrace");
		shaderGrid = RtCore::AddShader("RtPathTraceGrid");	// (2026-11-04) PTLightGrid
		shaderCache = RtCore::AddShader("RtPathTraceCache");	// (2026-11-07) PTRadianceCache's resolve pass
		shaderEmissive = RtCore::AddShader("RtEmissiveBuild");	// (2026-11-07 ptaaa-emissive) PTEmissive's light list
		shaderGI = RtCore::AddShader("RtPathTraceGI");		// (2026-11-04 d) PTRestirGI
		shaderReuse = RtCore::AddShader("RtRestirGI");
		RtCore::AddUser({ kUserName, Wants, Setup, Record, 0, 1 });	// Order 1: set up last (the first left out when the images do not fit)
	}

	void RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
		if (S.replace) return;	// (2026-11-03) PTReplace: shown in the scene already (Replace)
		Show(Device, RenderTarget, false);
	}

	// (2026-11-03) PTReplace: the traced picture into the game's scene right after the RT core's point (HDR, the stored curve), so
	// NVR's and the game's later passes (tonemap, bloom, grading) treat it as the frame
	void Replace(IDirect3DDevice9* Device) {
		if (!S.enabled || !S.replace) return;
		IDirect3DSurface9* scene = nullptr;
		if (FAILED(Device->GetRenderTarget(0, &scene)) || !scene) return;
		Show(Device, scene, true);
		scene->Release();
	}

	bool Replacing() { return S.enabled && S.replace && haveImage && RtCore::Running(); }

	// UNOFFICIAL 2026-11-07 (ptaaa-denoiser) DLSS RR's guides from the path tracer (RtPathTrace.h)
	bool RrGuides(IDirect3DTexture9** Albedo, IDirect3DTexture9** SpecularRG, IDirect3DTexture9** SpecularBRough) {
		*Albedo = *SpecularRG = *SpecularBRough = nullptr;
		// (2026-11-07 perf-rr) RR at RenderScale < 1: the trace covered the frame's render rect and copied its albedos into the top-left of
		// these screen-size textures (CopyOut tw x th), exactly where RR's guide pass reads every other input in a rect frame (one thread per
		// render pixel). So they are RR's guides whenever the traced rect is this frame's rect (was: only at scale 1). A rect that does not
		// match (the RenderScale changed since the trace) leaves the G-buffer's guides.
		const float frameScaleX = RenderRect::On ? RenderRect::ScaleX : 1.0f, frameScaleY = RenderRect::On ? RenderRect::ScaleY : 1.0f;
		const UINT frameW = RenderRect::Width(textureWidth), frameH = RenderRect::Height(textureHeight);
		const bool sameRect = fabsf(tracedScale[0] - frameScaleX) < 1e-4f && fabsf(tracedScale[1] - frameScaleY) < 1e-4f && lastRect[0] == frameW && lastRect[1] == frameH;
		if (!Replacing() || !S.denoise || !albedoThisFrame || !albedoTexture || !sameRect) return false;
		*Albedo = albedoTexture;
		if (splitThisFrame) { *SpecularRG = specAlbedoA; *SpecularBRough = specAlbedoB; }
		return true;
	}

	std::string StatusText() {
		if (!S.enabled) return "RT reference path tracer off ([Main.Main.RayTracing] ReferencePT false)";
		if (imageA < 0 && RtCore::Running()) return "RT reference path tracer turned on after the RT core started: restart the game (its images are reserved only when it is on at start)";
		char t[300];
		sprintf_s(t, "RT reference path tracer on | %s | %ux%u, %u samples a pixel (of PTMaxSamples %d), %u restarts, %d bounces, %d paths a frame, PTDebugView %d",
			RtCore::Running() ? "the core runs" : "the RT core is not running: nothing traced", textureWidth, textureHeight, samples, S.maxSamples, resets,
			S.bounces, S.samples, S.debugView);
		return t;
	}
}
