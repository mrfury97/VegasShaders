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
#include "SettingShots.h"	// UNOFFICIAL 2026-11-11 InternalTools: a test run's shot list may set the internal keys
#include "RtCore.h"
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "RtDenoise.h"
#include "RtShadowDenoise.h"	// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: helper SIGMA shadow denoiser for decoupled PT direct sun
#include "RtSunShadows.h"		// UNOFFICIAL 2026-11-20: avoid mode-0 RtShadowDenoise collision if PTStandDownRT is off
#include "ManyLamps.h"
#include "../effects/PBR/PBR.h"	// (2026-11-04 rtgap) the game's light scale	// (2026-11-03 rtspec) PTGameLamps 4: the game's lamp table
#include "RtLightUnits.h"	// UNOFFICIAL 2026-11-19 (Finding 4.1) shared PBR decode gamma + light / ambient / sky scales
#include "FlashlightDeferred.h"	// (2026-11-07) the flashlight as its own pass draws it (LightParams)
#include "RenderRect.h"	// (2026-11-03) RenderScale < 1: the trace over the frame's top-left rect (docs/next/RT-RENDERSCALE.md)
#include "RtViewModel.h"	// (2026-11-07 ptaaa-firstperson) PTViewModel's prep pass (shared with tools/nvr-rt-host)
#include "WaterMask.h"	// UNOFFICIAL 2026-11-12 PTWaterMask: the pixels the game drew water on (Render.cpp fills it; the prep pass reads it)
#include "RtBlendedLayer.h"	// UNOFFICIAL 2026-11-10 PTBlendedLayer
#include "RtEarlyLook.h"	// UNOFFICIAL 2026-11-11 PTEarlyLook (the early kick's look from the game's own buffers)
#include "RtFarHandOver.h"	// UNOFFICIAL 2026-11-12 PTFarBlend / PTGameFog / PTFarMatch: the traced picture's edge at the loaded cells (shared with tools/nvr-rt-host)
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
		// UNOFFICIAL 2026-11-10 PTCacheShort (RtPathTrace.comp gCacheShort; frame row texel 27.y): with PTRadianceCache, one path in N is traced in
		// full, the others end at their first bounce hit in the cache. 0 = off (every path in full).
		float	cacheShort = 0.0f;
		// UNOFFICIAL 2026-11-10 PTRouletteFrom (RtPathTrace.comp gRouletteFrom; frame row texel 30.y): 1 = Russian roulette already before the
		// second bounce ray. Measured by replaying a captured Lucky 38 frame: the pass -12 % on an NVIDIA L4 (4.96 -> 4.37 ms), -18 % on the
		// iGPU; the converged picture the same (mean x1.0000, 32-pixel tiles within 0.4 %); one frame's noise +12 % (relative MSE 0.46 -> 0.51).
		int		rouletteFrom = 0;
		float	denoiseCeiling = 200.0f;	// UNOFFICIAL 2026-11-11 PTDenoiseCeiling (frame row texel 23.y; RtPathTrace.comp gDenoiseCeil)
		float	restirDILampMatch = 0.0f;
		bool	blendedLayer = false;	// UNOFFICIAL 2026-11-10 PTBlendedLayer (core/RtBlendedLayer.h)
		// UNOFFICIAL 2026-11-11 PTEarlyLook (core/RtEarlyLook.h; only in frames the early kick submitted, env NVR_RT_EARLY_PT): 0 off, 1 the game's
		// own pixel where the live mode shows it (slice 1), 2 + the game's colour on matched surfaces (slice 2), 3 = 2 with the classes painted
		int		earlyLook = 0;
		bool	screenOwn = true;	// UNOFFICIAL 2026-11-11 [Main.Main.RayTracing] PTScreenOwn: the prep pass marks pixels whose colour / material buffers are another surface's (core/RtViewModel.h)
		bool	ownShadows = true;	// UNOFFICIAL 2026-11-12 [Main.Main.RayTracing] PTOwnShadows: the game's deferred sun / lamp shadow apply leaves the traced pixels alone (MaskShadowBuffer)
		bool	albedoGamma = true;	// UNOFFICIAL 2026-11-12 [Main.Main.RayTracing] PTAlbedoGamma: the AlbedoBuffer holds texture^([Shaders.PBR.Linear] Gamma); the trace reads it as texture^2.2 (frame row texel 291.y)
		bool	screenOwnNarrow = true;	// UNOFFICIAL 2026-11-12 [Main.Main.RayTracing] PTScreenOwnNarrow: a marked pixel takes a traced record only for hair, skin and cut-outs over a person; else the game's pixel (frame row texel 292.x)
		bool	albedoCoverage = true;	// UNOFFICIAL 2026-11-12 / 13 [Main.Main.RayTracing] PTAlbedoCoverage: with the game's MSAA a pixel whose samples were not all written is the game's own pixel (frame row texel 297.x; the prep pass's c9.w; CapturedNormals: no grass records then)
		bool	hairOverSky = true;	// UNOFFICIAL 2026-11-12 [Main.Main.RayTracing] PTHairOverSky: traced hair where nothing is recorded behind it (frame row texel 292.y)
		// UNOFFICIAL 2026-11-13 the flashlight under path tracing (docs/next/FLASHLIGHT-AUDIT-2026-11-13.md; RtPathTrace.comp gFlashOwn, FlashShape).
		// PTFlashOwnPixels (frame row texel 305.x): the flashlight's direct light on the game's own pixels whose surface the traced scene
		// holds (objects drawn without the mod's records: neither the light pass nor, since GameLightUnderPT, the game lamp lit them).
		// PTFlashRasterShape (texels 305.y, 306, 298): lamp 0's falloff, cone and light scale as the raster light pass shapes them.
		bool	flashOwn = true, flashRasterShape = false;
		// UNOFFICIAL 2026-11-12 (ptbump; RtPathTrace.comp gBumpOn / gBumpDetail / gMatteOn, frame row texels 293.y, 294.x, 294.y)
		bool	bumpNormals = true;		// PTBumpNormals: the first hit lit with its normal-mapped normal (the land's record, a traced surface's own map)
		float	bumpDetail = 1.0f;		// PTBumpDetail: PTDetail's share kept on those pixels (the show pass divides out the fine contrast the traced light holds itself)
		bool	matteNoHighlight = true;	// PTMatteNoHighlight: a mesh the game draws with no highlight is matte at the first hit (CapturedNormals::NoHighlightFlag)
		float	roughFresnel = 1.0f;	// PTRoughFresnel (frame row texel 295.x): a rough non-metal's grazing reflectance falls with its roughness (0 = as before)
		float	glowTexel = 0.02f;	// UNOFFICIAL 2026-11-12 [Main.Main.RayTracing] PTGlowTexel: PTGlowFromFrame only where the glow map's texel is this bright (frame row texel 293.x; 0 = the whole mesh, as before)
		// UNOFFICIAL 2026-11-12 the far hand-over (core/RtFarHandOver.h; FarHandOver below; a tester: "it stops abruptly at range"): the traced
		// picture ends at the loaded cells' edge. PTFarBlend: over the last PTFarBlendWidth of the loaded range it fades into the game's own
		// pixel; PTGameFog: the weather's fog on the traced pixels (the game's own formula x this); PTFarMatch: 1 = the traced picture's level
		// eased to the game's toward the edge (the measured ratio, at most PTFarMatchLimit, from PTFarMatchReach of the range inside the edge),
		// 2 = measured and logged only; PTFarView: 1 the map (red the band, green the fog), 2 the game's frame on every traced pixel.
		bool	farBlend = true, farBlendShadows = false;	// (PTFarBlendShadows: the band also lets the game's deferred shadow back in by its share)
		float	farBlendWidth = 0.2f, gameFog = 1.0f, farMatchLimit = 2.0f, farMatchReach = 0.5f;
		int		farMatch = 0, farView = 0;
		bool	hairFirst = true;	// UNOFFICIAL 2026-11-10 [Main.Main.RayTracing] BlendedHair, the path tracer's half: frame row texel 30.x
		// UNOFFICIAL 2026-11-10 PTRealtimeLevel (a tester's RTX 4090: 23 fps in a small room, the trace 45 of the frame's 59 ms; the same card's
		// 1.0.6 log: the plain 4-bounce reference trace 14 ms, PTRealtime 40). What PTRealtime trades for speed, each step measured in
		// tools/nvr-rt-playground (the room, 320 x 180, this PC's iGPU; the trace 65.6 ms at level 0):
		//   0 as before (3 bounces, every extra);
		//   1 two bounces + PTCacheShort 8 (most paths end at their first bounce hit in the radiance cache): 48.4 ms, -26 %;
		//     (2026-11-13 b) + no PTEmissive, as level 2;
		//   2 one bounce (its hit's further light from the radiance cache) and no PTEmissive (glowing triangles light through the bounce
		//     rays only: noisier next to neon, the same brightness): 27.4 ms, -58 %; the picture 3 % darker in that room.
		//     (2026-11-11) + no PTRestirDI (the lamps at the first hit sampled plainly, without reservoirs): the menu's Performance preset.
		// A key the player set themselves stays (PTBounces off its default 4, PTEmissive off -1, PTCacheShort off 0, PTRestirDI off -1).
		int		realtimeLevel = 0;
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
		bool	denoiseSplitSun = true;	// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: denoise primary direct sun visibility via RtShadowDenoise (SIGMA + actor anti-lag) separately from 32-frame GI
		bool	denoiseObjectMotion = true;
		int		denoiseMovingHistory = 8;
		float	denoiseMirrorHistory = 12.0f;
		bool	denoiseVirtual = true;
		float	denoiseMirrorSurface = 0.1f;	// UNOFFICIAL 2026-11-09 (pt-mirrors) PTDenoiseMirrorSurface: smooth metals under this roughness are denoised as the
												// thing they reflect (RtPathTrace.comp gReflAlbedo, RtDenoise Settings.MirrorSurface); 0 = as before
		int		denoiseUnderRr = 0;
		float	denoiseHistoryFix = 3.0f;	// (ptaaa-denoiser) PTDenoiseHistoryFix: RtDenoise Settings.HistoryFix (newly seen pixels: no speckles)
		float	denoiseConfidence = 0.0f;	// UNOFFICIAL 2026-11-15 (rt-denoise-inputs) PTDenoiseConfidence: RtDenoise Settings.Confidence (0 off)
		float	denoiseMeanVariance = 2.0f;	// (ptaaa-denoiser) PTDenoiseMeanVariance: RtDenoise Settings.MeanVariance (fine shadows kept)
		int		denoiseAntiFirefly = -1;	// (ptaaa-denoiser) PTDenoiseAntiFirefly: -1 auto (on under PTRealtime), 0 off, 1 on
		float	denoiseFireflyK = 2.0f;	// PTDenoiseFireflyK (test key): its k
		int		denoiseFireflySpread = 4;	// PTDenoiseFireflySpread (2026-11-09): the most of a clamped firefly that is spread on, x what the pixel supports (0: all of it)
		float	detail = 1.0f;			// (2026-11-07) PTDetail: the game's fine shading on the traced light (0 off, 1 all)
		float	contact = 1.0f;			// (2026-11-07 rtgap-tess) PTContactShadows: the depth buffer's contact march at the first hit (strength, 0 off)
		float	contactLength = 24.0f;	// PTContactLength: its length on screen (pixels)
		float	detailEdge = 2.0f;		// (2026-11-10) PTDetailEdge: the detail fades where the game's shading steps by this factor (0 off)
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
		float	glassSurface = 1.0f;	// UNOFFICIAL 2026-11-11 PTGlassSurface (frame row texel kGlassSurfaceTexel: texels 24-26 have no room left)
		// UNOFFICIAL 2026-11-07 (ptaaa-emissive) PTEmissive: glowing triangles as lights (RtEmissiveRow.h, RtEmissiveBuild.comp; -1 auto: on under
		// PTRealtime), at most PTEmissiveMax instances and PTEmissiveTriangles triangles in the list a frame (test keys)
		int		emissive = -1;
		int		emissiveMax = 64;
		int		emissiveTriangles = (int)RtEmissiveRow::kTrianglesMax;
		// UNOFFICIAL 2026-11-11 (pt-emissive-cost; RtPathTrace.comp LoadEmCostKeys has the reasoning, frame row texels 289.y-291.x) PTEmissive cost
		// 12.8 / 19.8 / 12.7 ms of a ~36 ms pass sum on an NVIDIA L4 (the 19.8 at Goodsprings at noon, nothing glowing on screen): every glowing
		// mesh was a light-sample candidate at every hit however dim there, and ReSTIR DI then reused and ray-tested those samples in every pixel.
		// All four leave the picture's mean alone (what is not light-sampled is counted in full by the bounce rays); 0 = as before.
		//   PTEmissiveCut    an instance is a candidate at a hit only where its estimated light is at least this share of the sky's light +
		//                    the sun's unshadowed light on that surface (so: by day, outdoors; 0 indoors and little at night)
		//   PTEmissiveFloor  the same test against an absolute light level (the trace's linear units; works indoors too)
		//   PTEmissiveHits   n >= 1: only the path's first n hits take an emissive sample (1 = the first hit)
		//   PTEmissiveRoulette  k: an emissive candidate under k x the hit's other unshadowed direct light: Russian roulette before its shadow ray
		float	emissiveCut = 0.02f, emissiveFloor = 0.0f, emissiveRoulette = 0.0f;	// (PTEmissiveCut: on by default, the others off)
		int		emissiveHits = 0;
		// UNOFFICIAL 2026-11-07 (perf-ptcost, RT-PATH-TRACING.md "perf-ptcost"; RtPtCost.h) the ray budget: PTRestirDIRays 0 = ReSTIR DI's
		// targets unshadowed + one shadow ray for the chosen sample (1 = a ray inside every target, as before; 2 / 3 test variants),
		// PTBounceLights 0 = one light sample a bounce hit (1 = the sun, the lamp and the emissive point each, as before). A missing key: 0
		int		restirDIRays = 0;
		// UNOFFICIAL 2026-11-15 (rt-reuse, docs/next/RT-REUSE-CGNS.md) test switches for both reservoirs (PTRestirDI, PTRestirGI): PTRestirCGNS
		// compatibility-guided neighbour picks from this many candidates (0 off), PTRestirDupMap ReSTIR GI's duplication count (off)
		int		restirCGNS = 0;
		bool	restirDupMap = false;
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
	int			shaderPrimary = -1, shaderPrimaryGrid = -1, shaderSpatial = -1, shaderBounce = -1, shaderBounceGrid = -1;	// UNOFFICIAL 2026-11-19 (Phase 3B): 3-pass PT split
	bool		splitScratchReady = false;
	// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: decoupled primary direct sun shadow denoising (helper RtPathTraceSunUnpack + RtShadowDenoise mode 0)
	int			shaderSunUnpack = -1, shaderSunDenoise = -1;
	int			imageSunVis = -1, imageSunHit = -1, imageSunDirect = -1, imageSunMotion = -1;
	bool		sunSplitScratchReady = false;
	IDirect3DTexture9*	sunVisTexture = nullptr;	IDirect3DSurface9*	sunVisSurface = nullptr;
	IDirect3DTexture9*	sunDirectTexture = nullptr;	IDirect3DSurface9*	sunDirectSurface = nullptr;
	bool		sunSplitThisFrame = false;
	RtShadowDenoise::View sunDenoiseView;
	UInt32		sunDenoiseFrames = 0;
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
	// UNOFFICIAL 2026-11-11 PTEarlyLook slice 2: the frame row's texel 288 (RtPathTrace.comp kEarlyLookTexel: keep them equal), x 1 = the trace writes
	// its first hit's view depth into the display's alpha, y 0 (reserved). The first texel after the GGX energy table (RtRestirRow.h: texels
	// 32-287 = kRowFloatsAll floats; the design's "texel 32" is that table's first texel); the row is kLightTexels (1152) texels wide.
	constexpr int kEarlyLookTexel = 288;
	static_assert(2 * kEarlyLookTexel >= RtRestirRow::kRowFloatsAll && kEarlyLookTexel < kLightTexels, "PTEarlyLook's frame row texel: after the energy table, inside the row");
	// UNOFFICIAL 2026-11-11 PTGlassSurface: the frame row's texel 289 (RtPathTrace.comp kGlassSurfaceTexel: keep them equal), x the key (0 = the
	// clear glass as before), y 0 (reserved). The material texels 24-26 are full (RtMaterials.h), so it sits after the early-look texel.
	constexpr int kGlassSurfaceTexel = kEarlyLookTexel + 1;
	// UNOFFICIAL 2026-11-11 (pt-emissive-cost) PTEmissive's cost keys (RtPathTrace.comp kEmCostTexel: keep them equal): texel 289.y the weight floor's
	// base, 290.x its sun term (both in EmWeight's units: light x pi / PTGlow), 290.y PTEmissiveHits, 291.x PTEmissiveRoulette, 291.y free.
	// The frame row's texels in use: 0-15, 16-21 (RtTessContact.h), 22 (RtEmissiveRow.h), 23, 24-26 (RtMaterials.h), 27, 28-29 (RtPtCost.h), 30, 31,
	// 32-287 (the GGX energy table), 288, 289.x (PTGlassSurface), and these. The next free float: 291.y, then texel 292.
	constexpr int kEmCostTexel = RtEmissiveRow::kCostTexel;
	static_assert(kEmCostTexel == kGlassSurfaceTexel && kEmCostTexel == 289 && kEmCostTexel + 2 < kLightTexels && 2 * kEmCostTexel >= RtRestirRow::kRowFloatsAll, "PTEmissive's cost keys: frame row texels 289.y-291.x (RtPathTrace.comp kEmCostTexel)");
	static_assert(2 * kGlassSurfaceTexel >= RtRestirRow::kRowFloatsAll && kGlassSurfaceTexel < kLightTexels, "PTGlassSurface's frame row texel: after the energy table, inside the row");
	// UNOFFICIAL 2026-11-13 the flashlight's keys (RtPathTrace.comp kFlashOwnTexel / kFlashRasterTexel / kFlashRasterTexel2: keep them equal):
	// 305.x PTFlashOwnPixels, 305.y the lamps' light scale for PTFlashRasterShape, 306.x its decode exponent (0 = off), 306.y the physical
	// lamp shape's exponent (0 = that shape off), 298.x the shape's 1 / Falloff^2, 298.y its strength. 300-304 stay unused.
	constexpr int kFlashOwnTexel = 305, kFlashRasterTexel = 306, kFlashRasterTexel2 = 298;
	// UNOFFICIAL 2026-11-15 (rt-reuse) PTRestirCGNS's candidates for ReSTIR DI: texel 299.x (RtPathTrace.comp kCgnsTexel: keep them equal), y 0
	constexpr int kCgnsTexel = 299;
	constexpr int kSplitBaseTexel = 300;	// UNOFFICIAL 2026-11-19 (Phase 3B): 3-pass PT split scratch base (4096-word pages; RtPathTrace.comp kSplitBaseTexel)
	static_assert(kCgnsTexel > kFlashRasterTexel2 && kSplitBaseTexel < kFlashOwnTexel && kSplitBaseTexel < kLightTexels, "PTRestirCGNS / 3-pass split frame row texel");
	RtRestirRow::State restirState;
	// (2026-11-07) the scratch store's layout: the ReSTIR DI reservoirs (two sets of w x h x 4 words) at word 0, the radiance cache from here
	// (2026-11-07 ptaaa-emissive: two sets of w x h x 6 words, was x 4: an emissive sample's triangle and barycentrics)
	unsigned long long CacheBaseWords(UINT w, UINT h) { return ((12ull * w * h + 4095ull) / 4096ull) * 4096ull; }
	// (2026-11-07 ptaaa-emissive) PTEmissive's blocks + triangle list after the radiance cache's table (RtEmissiveRow.h kScratchWords)
	unsigned long long EmissiveBaseWords(UINT w, UINT h, int cacheLog2) { return CacheBaseWords(w, h) + (16ull << cacheLog2); }
	// UNOFFICIAL 2026-11-19 (Phase 3B): 3-pass PT split region (6 words/px intermediate temporal reservoirs + 16 words/px primary payload)
	unsigned long long SplitBaseWords(UINT w, UINT h, int cacheLog2) { return ((EmissiveBaseWords(w, h, cacheLog2) + (unsigned long long)RtEmissiveRow::kScratchWords + 4095ull) / 4096ull) * 4096ull; }
	unsigned long long SplitNeedWords(UINT w, UINT h) { return 22ull * w * h; }
	// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: 3 words/px (vis+signedHitT, sunDiffDemod.rgb) right after the 3-pass split region
	unsigned long long SunSplitBaseWords(UINT w, UINT h, int cacheLog2) { return ((SplitBaseWords(w, h, cacheLog2) + (splitScratchReady ? SplitNeedWords(w, h) : 0ull) + 4095ull) / 4096ull) * 4096ull; }
	unsigned long long SunSplitNeedWords(UINT w, UINT h) { return 3ull * w * h; }
	RtEmissiveRow::State emissiveState;
	int			shaderEmissive = -1;	// RtEmissiveBuild.spv
	int			emissiveCount = 0;	// this frame's
	float		emissiveFloorBase = 0.0f;	// (2026-11-11 pt-emissive-cost) this frame's weight floor (frame row texel 289.y; RtEmissiveBuild.comp params.z)
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
		const bool tools = RtPathTrace::InternalTools();	// UNOFFICIAL 2026-11-11: ReferencePT / PTReplace / PTDenoise are internal tools
		S.enabled = tools && TheSettingManager->GetSettingI(kSection, "ReferencePT") != 0;
		S.bounces = i("PTBounces", 1, 16);
		S.samples = i("PTSamples", 1, 16);
		S.maxSamples = i("PTMaxSamples", 1, 1 << 20);
		S.sky = f("PTSky", 0.0f, 16.0f);
		S.ambient = f("PTAmbient", 0.0f, 4.0f);
		S.glow = f("PTGlow", 0.0f, 16.0f);
		S.clamp = f("PTClamp", 0.0f, 1000.0f);
		S.exposure = f("PTExposure", 0.01f, 64.0f);
		S.split = f("PTSplit", 0.0f, 1.0f);
		S.debugView = i("PTDebugView", 0, 11);	// (2026-11-07: 10 the radiance cache at the first hit)
		S.primary = TheSettingManager->GetSettingI(kSection, "PTPrimary") != 0;
		S.specular = TheSettingManager->GetSettingI(kSection, "PTSpecular") != 0;
		S.denoise = tools && TheSettingManager->GetSettingI(kSection, "PTDenoise") != 0;
		S.replace = tools && TheSettingManager->GetSettingI(kSection, "PTReplace") != 0;
		if (!tools) {
			static bool told = false;
			if (!told && (TheSettingManager->GetSettingI(kSection, "ReferencePT") != 0 || TheSettingManager->GetSettingI(kSection, "PTDenoise") != 0 ||
				TheSettingManager->GetSettingI(kSection, "PTReplace") != 0)) {
				told = true;
				Logger::Log("UNOFFICIAL RT path trace: ReferencePT / PTReplace / PTDenoise are set in the settings file but are internal comparison tools: "
					"ignored. PTRealtime is the path tracing switch (it turns all three on by itself).");
			}
		}
		S.glowLights = TheSettingManager->GetSettingI(kSection, "PTGlowLights") != 0;
		S.glowFromFrame = TheSettingManager->GetSettingI(kSection, "PTGlowFromFrame") != 0;
		S.lightGrid = TheSettingManager->GetSettingI(kSection, "PTLightGrid") != 0;
		S.lightGridMin = (std::max)(TheSettingManager->GetSettingI(kSection, "PTLightGridMinLamps"), 1);
		S.bounceAlpha = i("PTBounceAlpha", 0, 3);
		S.bounceDistance = f("PTBounceDistance", 0.0f, 60000.0f);
		S.indirectClamp = f("PTIndirectClamp", 0.0f, 1000.0f);
		S.realtimeClamp = f("PTRealtimeClamp", 0.0f, 1000.0f);	// UNOFFICIAL 2026-11-09 (a missing key: 0, off)
		S.bounceRoughen = f("PTBounceRoughen", -1.0f, 1.0f);		// UNOFFICIAL 2026-11-09 (-1 = on under PTRealtime; a missing key reads 0: off)
		S.cacheShort = f("PTCacheShort", 0.0f, 64.0f);	// UNOFFICIAL 2026-11-10 (a missing key: 0, off)
		S.realtimeLevel = i("PTRealtimeLevel", 0, 2);	// UNOFFICIAL 2026-11-10 (a missing key: 0, as before)
		{ SettingManager::Configuration::ConfigNode hn; S.hairFirst = !TheSettingManager->Config.FillNode(&hn, kSection, "BlendedHair") || hn.BoolValue; }	// UNOFFICIAL 2026-11-10 (a missing key: on)
		{ SettingManager::Configuration::ConfigNode on; S.screenOwn = !TheSettingManager->Config.FillNode(&on, kSection, "PTScreenOwn") || on.BoolValue; }	// UNOFFICIAL 2026-11-11 (a missing key: on)
		{ SettingManager::Configuration::ConfigNode on; WaterMask::GrassOn = !TheSettingManager->Config.FillNode(&on, kSection, "PTGrassOwnPixel") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 PTGrassOwnPixel (a missing key: on; core/WaterMask.h, core/CapturedNormals.h GrassOwnDraw)
		{ SettingManager::Configuration::ConfigNode on; WaterMask::Enabled = !TheSettingManager->Config.FillNode(&on, kSection, "PTWaterMask") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 PTWaterMask (a missing key: on; core/WaterMask.h)
		{ SettingManager::Configuration::ConfigNode on; S.ownShadows = !TheSettingManager->Config.FillNode(&on, kSection, "PTOwnShadows") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 (a missing key: on)
		{ SettingManager::Configuration::ConfigNode on; S.albedoGamma = !TheSettingManager->Config.FillNode(&on, kSection, "PTAlbedoGamma") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 (a missing key: on)
		{ SettingManager::Configuration::ConfigNode on; S.screenOwnNarrow = !TheSettingManager->Config.FillNode(&on, kSection, "PTScreenOwnNarrow") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 (a missing key: on)
		{ SettingManager::Configuration::ConfigNode on; S.albedoCoverage = !TheSettingManager->Config.FillNode(&on, kSection, "PTAlbedoCoverage") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 (a missing key: on)
		{ SettingManager::Configuration::ConfigNode on; S.hairOverSky = !TheSettingManager->Config.FillNode(&on, kSection, "PTHairOverSky") || on.BoolValue; }	// UNOFFICIAL 2026-11-12 (a missing key: on)
		{	// UNOFFICIAL 2026-11-13 (a missing key: PTFlashOwnPixels on, a bug fix; PTFlashRasterShape off, it changes the beam's look)
			SettingManager::Configuration::ConfigNode on;
			S.flashOwn = !TheSettingManager->Config.FillNode(&on, kSection, "PTFlashOwnPixels") || on.BoolValue;
			S.flashRasterShape = TheSettingManager->Config.FillNode(&on, kSection, "PTFlashRasterShape") && on.BoolValue;
		}
		{	// UNOFFICIAL 2026-11-12 (ptbump; a missing key: on / 1)
			SettingManager::Configuration::ConfigNode node;
			S.bumpNormals = !TheSettingManager->Config.FillNode(&node, kSection, "PTBumpNormals") || node.BoolValue;
			S.bumpDetail = TheSettingManager->Config.FillNode(&node, kSection, "PTBumpDetail") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTBumpDetail"), 0.0f, 1.0f) : 1.0f;
			S.matteNoHighlight = !TheSettingManager->Config.FillNode(&node, kSection, "PTMatteNoHighlight") || node.BoolValue;
			S.roughFresnel = TheSettingManager->Config.FillNode(&node, kSection, "PTRoughFresnel") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTRoughFresnel"), 0.0f, 1.0f) : 1.0f;
		}
		{ SettingManager::Configuration::ConfigNode v; S.glowTexel = TheSettingManager->Config.FillNode(&v, kSection, "PTGlowTexel") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTGlowTexel"), 0.0f, 100.0f) : 0.02f; }	// UNOFFICIAL 2026-11-12 (a missing key: 0.02)
		{	// UNOFFICIAL 2026-11-12 the far hand-over (a missing key: its documented default)
			SettingManager::Configuration::ConfigNode n;
			S.farBlend = !TheSettingManager->Config.FillNode(&n, kSection, "PTFarBlend") || n.BoolValue;
			S.farBlendShadows = TheSettingManager->Config.FillNode(&n, kSection, "PTFarBlendShadows") && n.BoolValue;
			S.farBlendWidth = TheSettingManager->Config.FillNode(&n, kSection, "PTFarBlendWidth") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTFarBlendWidth"), 0.02f, 0.5f) : 0.2f;
			S.gameFog = TheSettingManager->Config.FillNode(&n, kSection, "PTGameFog") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTGameFog"), 0.0f, 1.0f) : 1.0f;
			S.farMatch = i("PTFarMatch", 0, 2);
			S.farMatchLimit = TheSettingManager->Config.FillNode(&n, kSection, "PTFarMatchLimit") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTFarMatchLimit"), 1.0f, 4.0f) : 2.0f;
			S.farMatchReach = TheSettingManager->Config.FillNode(&n, kSection, "PTFarMatchReach") ? (std::clamp)(TheSettingManager->GetSettingF(kSection, "PTFarMatchReach"), 0.05f, 1.0f) : 0.5f;
			S.farView = i("PTFarView", 0, 2);
		}
		S.rouletteFrom = i("PTRouletteFrom", 0, 1);	// UNOFFICIAL 2026-11-10 (a missing key: 0, as before)
		S.denoiseIterations = i("PTDenoiseIterations", 0, 5);
		S.denoiseHistory = f("PTDenoiseHistory", 1.0f, 256.0f);
		S.denoiseClamp = f("PTDenoiseClamp", 0.0f, 8.0f);
		S.denoisePeopleHistory = f("PTDenoisePeopleHistory", 0.0f, 256.0f);
		S.denoiseAntiLag = f("PTDenoiseAntiLag", 0.0f, 16.0f);
		S.denoiseGuide = f("PTDenoiseGuide", 0.0f, 4.0f);
		// (2026-11-07 ptaaa-denoiser; a missing key reads 0: the toml defaults carry the ON values)
		S.denoiseSplit = TheSettingManager->GetSettingI(kSection, "PTDenoiseSplit") != 0;
		{ SettingManager::Configuration::ConfigNode sn; S.denoiseSplitSun = TheSettingManager->Config.FillNode(&sn, kSection, "PTDenoiseSplitSun") ? (sn.IntValue != 0) : true; }
		S.denoiseObjectMotion = TheSettingManager->GetSettingI(kSection, "PTDenoiseObjectMotion") != 0;
		S.denoiseMovingHistory = i("PTDenoiseMovingHistory", 0, 256);
		S.denoiseMirrorHistory = f("PTDenoiseMirrorHistory", 1.0f, 256.0f);
		S.denoiseVirtual = TheSettingManager->GetSettingI(kSection, "PTDenoiseVirtual") != 0;
		S.denoiseMirrorSurface = f("PTDenoiseMirrorSurface", 0.0f, 1.0f);	// UNOFFICIAL 2026-11-09 (pt-mirrors; a missing key: 0, off)
		S.denoiseUnderRr = i("PTDenoiseUnderRr", 0, 1);
		S.denoiseHistoryFix = f("PTDenoiseHistoryFix", 0.0f, 16.0f);
		S.denoiseConfidence = f("PTDenoiseConfidence", 0.0f, 16.0f);	// UNOFFICIAL 2026-11-15 (rt-denoise-inputs; a missing key: 0, off)
		S.denoiseMeanVariance = f("PTDenoiseMeanVariance", 0.0f, 64.0f);
		S.denoiseAntiFirefly = i("PTDenoiseAntiFirefly", -1, 1);
		S.denoiseFireflyK = f("PTDenoiseFireflyK", 0.0f, 64.0f);
		S.denoiseFireflySpread = (int)f("PTDenoiseFireflySpread", 0.0f, 64.0f);	// UNOFFICIAL 2026-11-09 (RtDenoiseTemporalRgb / Spec .frag FireflyExcess)
		S.detail = f("PTDetail", 0.0f, 1.0f);
		S.detailRadius = f("PTDetailRadius", 1.0f, 16.0f);
		{	// UNOFFICIAL 2026-11-10 PTDetailEdge (a missing key, an older toml: 2)
			SettingManager::Configuration::ConfigNode node;
			S.detailEdge = TheSettingManager->Config.FillNode(&node, kSection, "PTDetailEdge") ? std::clamp(node.FloatValue, 0.0f, 16.0f) : 2.0f;
		}
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
		// UNOFFICIAL 2026-11-11 (pt-emissive-cost; a missing key: PTEmissiveCut its documented 0.02, the others 0 = off, PTEmissive as before)
		{ SettingManager::Configuration::ConfigNode en; S.emissiveCut = TheSettingManager->Config.FillNode(&en, kSection, "PTEmissiveCut") ? (std::min)((std::max)(en.FloatValue, 0.0f), 1.0f) : 0.02f; }
		S.emissiveFloor = f("PTEmissiveFloor", 0.0f, 16.0f);
		S.emissiveHits = i("PTEmissiveHits", 0, 16);
		S.emissiveRoulette = f("PTEmissiveRoulette", 0.0f, 1.0f);
		S.restirDISpatial = i("PTRestirDISpatial", 0, 8);
		S.restirDIRadius = f("PTRestirDIRadius", 1.0f, 64.0f);
		S.restirDIMaxM = f("PTRestirDIMaxM", 1.0f, 255.0f);
		S.restirDINormal = f("PTRestirDINormal", -1.0f, 1.0f);
		S.restirDIPlane = f("PTRestirDIPlane", 0.001f, 1.0f);
		S.blendedLayer = i("PTBlendedLayer", 0, 1) != 0;	// UNOFFICIAL 2026-11-10 (a missing key: 0, off)
		S.earlyLook = i("PTEarlyLook", 0, 3);	// UNOFFICIAL 2026-11-11 (a missing key: 0, off)
		{ SettingManager::Configuration::ConfigNode cn; S.denoiseCeiling = TheSettingManager->Config.FillNode(&cn, kSection, "PTDenoiseCeiling") ? (std::min)((std::max)(cn.FloatValue, 0.0f), 60000.0f) : 200.0f; }	// UNOFFICIAL 2026-11-11 (a missing key: the documented 200; 0 = the old 60000)
		{ SettingManager::Configuration::ConfigNode ln; S.restirDILampMatch = TheSettingManager->Config.FillNode(&ln, kSection, "PTRestirDILampMatch") ? (std::min)((std::max)(ln.FloatValue, 0.0f), 256.0f) : 32.0f; }	// UNOFFICIAL 2026-11-10 (BUG FIX: a missing key reads the documented 32; 0 = the old exact match)
		S.restirDIRays = i("PTRestirDIRays", 0, 4);	// (2026-11-07 perf-ptcost; 2026-11-19 mode 4 = RTXDI 2-ray visibility)
		S.restirCGNS = i("PTRestirCGNS", 0, 31);	// UNOFFICIAL 2026-11-15 (rt-reuse; a missing key: 0, off)
		S.restirDupMap = i("PTRestirDupMap", 0, 1) != 0;
		S.bounceLights = i("PTBounceLights", 0, 1);
		// UNOFFICIAL 2026-11-07 (ptaaa-materials, RT-PATH-TRACING.md "materials") the material models (a missing key: 0, off)
		S.materials.glass = (float)i("PTGlass", 0, 2);
		S.materials.glassTint = f("PTGlassTint", 0.0f, 1.0f);
		S.glassSurface = f("PTGlassSurface", 0.0f, 1.0f);	// UNOFFICIAL 2026-11-11 (a missing key: 0, the clear glass as before)
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
			if (S.bounces == 4) S.bounces = S.realtimeLevel >= 2 ? 1 : S.realtimeLevel == 1 ? 2 : 3;	// (2026-11-10 PTRealtimeLevel)
			if (S.realtimeLevel == 1 && S.cacheShort <= 0.0f) S.cacheShort = 8.0f;
			if (S.realtimeLevel >= 1) S.rouletteFrom = 1;
			// (2026-11-13 b) from level 1 (was 2): PTEmissive cost 12-24 ms of a ~36 ms trace on the RTX bench (L4, 720p: lamp rooms 12, the neon
			// strip 24, noon with nothing glowing 20) and no still or motion run showed a difference without it. A PTEmissive 1 stays.
			if (S.realtimeLevel >= 1 && S.emissive < 0) S.emissive = 0;
			// UNOFFICIAL 2026-11-11 level 2 = the Performance preset as measured (NVIDIA L4, 1280 x 720, three scenes: 57.7 / 39.5 / 76.7 fps
			// against level 0's 20.5 / 26.2 / 23.1; no loss seen in the night, fire and neon sheets): no ReSTIR DI either. A PTRestirDI 1 stays.
			if (S.realtimeLevel >= 2 && S.restirDI < 0) S.restirDI = 0;
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
		// UNOFFICIAL 2026-11-12 (ptbump) PTMatteNoHighlight: the object shaders code "the game draws no highlight on this mesh" into their
		// record only while the path tracer is on and wants it (Render.cpp MaterialForPass): off, every record is bit for bit what it was
		CapturedNormals::NoHighlightFlag = S.enabled && S.matteNoHighlight;
	}

	float Lin(float v) { return powf((std::max)(v, 0.0f), 2.2f); }
	// UNOFFICIAL 2026-11-04 (rtgap) the game's light scale: its lit shaders multiply the sun and the lamps by [Shaders.PBR.*] / [Shaders.Terrain.*]
	// LightingScale, decoded to linear (LIN_SCALE_LIGHT, TESR_LinearScales.x): 0.725 by day outdoors = 0.49 linear, 1 indoors and at night.
	// Without it the reference's sunlit surfaces were 2.06x the game's at Goodsprings noon (rtgap-gs); with the game's own lights only.
	inline float GameLightScale() { return RtLightUnits::LightScale(); }

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
			// (2026-11-03 rtspec / 2026-11-19 Finding 4.1) PTGameLamps: the colour as the game's lamp table makes it (ManyLamps.h / Object.hlsl:
			// diffuse x dimmer, the dimmer at most 1 without HDR, then decoded to linear together with RtLightUnits::Gamma(), x LightScale in mode 4);
			// off (0 / 1): diffuse decoded x dimmer, as RT GI
			float dim = col.w;
			const bool gameColour = (S.gameLamps & 2) != 0 || S.gameLamps == 4;
			if (gameColour && !*(const bool*)0x011F941E && dim > 1.0f) dim = 1.0f;	// BSShaderManager::bHDR
			const D3DXVECTOR3 dec = gameColour ? RtLightUnits::DecodeLamp(D3DXVECTOR4(col.x, col.y, col.z, dim), S.gameLamps == 4 ? RtLightUnits::LightScale() : 1.0f) :
				D3DXVECTOR3(Lin(col.x) * col.w, Lin(col.y) * col.w, Lin(col.z) * col.w);
			const D3DXVECTOR4 colour(dec.x, dec.y, dec.z, 0.0f);
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
				float fdim = fc.w;
				const bool gameFlash = (S.gameLamps & 2) != 0 || S.gameLamps == 4;
				if (gameFlash && !*(const bool*)0x011F941E && fdim > 1.0f) fdim = 1.0f;
				colour = gameFlash ? RtLightUnits::DecodeLamp(D3DXVECTOR4(fc.x, fc.y, fc.z, fdim), S.gameLamps == 4 ? RtLightUnits::LightScale() : 1.0f) :
					D3DXVECTOR3(Lin(fc.x) * fc.w, Lin(fc.y) * fc.w, Lin(fc.z) * fc.w);
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
			const float ls = RtLightUnits::LightScale();	// (2026-11-04 rtgap) as the game's lamps: x LIN_SCALE_LIGHT
			for (const ManyLamps::ActiveLamp& a : source) {
				if (a.reach <= 0.0f) continue;
				// (2026-11-17, as the test for the game's lamps above) a NaN / Inf lamp would break std::sort's ordering below
				if (!(fabsf(a.pos[0]) + fabsf(a.pos[1]) + fabsf(a.pos[2]) + fabsf(a.reach) < 1.0e30f)) continue;
				const D3DXVECTOR3 rel(a.pos[0] - eye.x, a.pos[1] - eye.y, a.pos[2] - eye.z);
				const D3DXVECTOR3 tc = ManyLamps::LinearCompiled ? D3DXVECTOR3(a.colour[0] * ls, a.colour[1] * ls, a.colour[2] * ls) :
					RtLightUnits::DecodeColor(a.colour[0], a.colour[1], a.colour[2], ls);
				table.push_back({ D3DXVECTOR4(rel.x, rel.y, rel.z, a.reach), D3DXVECTOR4(tc.x, tc.y, tc.z, 0.0f),
					(std::max)(D3DXVec3Length(&rel) - a.reach, 0.0f) });
			}
			for (Lamp& l : table) physical(l);
			if (haveFlash) { Lamp f = flash; physical(f); if (S.flashRasterShape && S.lampFalloff == 1) { const float code = f.colour.w; f.colour /= S.lampGain; f.colour.w = code; } f.distance = -1.0f; table.push_back(f); }	// (2026-11-07) the flashlight (2026-11-13 PTFlashRasterShape: the light pass's own brightness, no PTLampGain)
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
		if (haveFlash && S.flashRasterShape && S.lampFalloff == 1) { const float code = lamps[0].colour.w; lamps[0].colour /= S.lampGain; lamps[0].colour.w = code; }	// (2026-11-13 PTFlashRasterShape, as above)
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
			if (!(fabsf(g.center[0]) + fabsf(g.center[1]) + fabsf(g.center[2]) + fabsf(g.rgb[0]) + fabsf(g.rgb[1]) + fabsf(g.rgb[2]) < 1.0e30f)) continue;	// (2026-11-17: a NaN weight in the sort below)
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
		keys.maxM = S.restirDIMaxM; keys.spatial = (float)S.restirDISpatial; keys.radius = S.restirDIRadius; keys.normal = S.restirDINormal; keys.plane = S.restirDIPlane; keys.lampMatch = S.restirDILampMatch;
		if (cacheLog2Setup > 0) {	// (2026-11-07) PTRadianceCache
			keys.cacheLog2 = (float)cacheLog2Setup;
			keys.cacheBase4096 = (float)(CacheBaseWords(TheRenderManager->width, TheRenderManager->height) / 4096ull);
			keys.cacheCell = S.cacheCell; keys.cacheMaxSamples = S.cacheMaxSamples; keys.cacheMinSamples = S.cacheMinSamples;
		}
		const int lampsBefore = restirState.lamps;
		RtRestirRow::Build(restirState, row, lamps, lampData[0].pos, lampCount, (int)(sizeof(LampData) / sizeof(float)), camWorld, eyeScene, c.forward, c.right, c.up, tw, th, keys);
		{	// UNOFFICIAL 2026-11-10, measurement: how the lamps kept their ReSTIR history over the last 300 frames (a lamp unmatched every frame = its light all but lost)
			static int frames = 0, sumUnmatched = 0, sumLoose = 0, sumLamps = 0, maxUnmatched = 0;
			sumUnmatched += restirState.unmatched; sumLoose += restirState.loose; sumLamps += lampsBefore; maxUnmatched = (std::max)(maxUnmatched, restirState.unmatched);
			if (++frames >= 300) {
				Logger::Log("UNOFFICIAL RT path trace lamp history: %.1f lamps a frame, of last frame's %.2f a frame found no match (most in one frame %d), %.2f matched only by the loose rule (PTRestirDILampMatch %.0f units); ReSTIR DI %s",
					sumLamps / 300.0f, sumUnmatched / 300.0f, maxUnmatched, sumLoose / 300.0f, S.restirDILampMatch, S.restirDI > 0 ? "on" : "off");
				frames = sumUnmatched = sumLoose = sumLamps = maxUnmatched = 0;
			}
		}
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

	// (2026-11-18) Setup made no images (the path tracer was off as the helper started) and it was switched on since: one SetupAgain asked
	bool		setupAsked = false;

	bool Wants() {
		ReadSettings();
		if (!S.enabled || !S.viewModel) CapturedNormals::VmWanted = false;	// (2026-11-07 ptaaa-firstperson; Record turns it on)
		if (!S.enabled) { setupAsked = false; return false; }
		// UNOFFICIAL 2026-11-18 (fork port, Setup): switched on (the "Path tracing" menu rows are live) while the helper runs without its images:
		// the helper is started again once (RtCore::SetupAgain, at the next Ready) so Setup makes them; wanted this frame so that Ready gets
		// there. Asked once and still none (left out, or the images failed): not wanted, so none of PTRealtime's costs run for no picture.
		if (imageA < 0 && RtCore::Running()) {
			if (setupAsked) return false;
			setupAsked = true;
			RtCore::SetupAgain("the path tracer was switched on: the helper starts again to make its shared images");
		}
		return true;
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
	float FovScale() {	// match ReflectionsEffect::WeaponFovScale (handles ADS/zoom FOV too via CapturedNormals::ViewModelFovScale)
		return CapturedNormals::ViewModelFovScale();
	}
	// (2026-11-11 PTScreenOwn) viewModel: the view model's part (as before); own: the marks (RtViewModel.h; tw, th the render rect, jitter the
	// depth's, as the trace's ScreenHit). At least one of the two.
	// UNOFFICIAL 2026-11-19 (Finding 4.2) RtViewModel::kPrepPS + FD_SV_F0 bridge: when c8.z > 0.5, decodes the lighting-free G-buffer's
	// dielectric highlight F0 slot s (1..9 -> F0 0.00..0.08, Includes/DeferredPixel.hlsl FDSunVisCode) from TESR_CapturedNormalsBuffer.a
	// and carries +s/128 in the clean / ownMaybe sub-band of gNormals.a (exact in fp16; preserves every existing nv.a predicate).
	const char* const kVmPrepF0PS =
		"sampler2D N : register(s0); sampler2D C : register(s1); sampler2D M : register(s2); sampler2D D : register(s3); sampler2D K : register(s4);"
		"sampler2D W : register(s5);"
		"float4 P : register(c0); float4 Q : register(c1); row_major float4x4 V : register(c2);"
		"float4 S : register(c6); float4 R : register(c7); float4 T : register(c8); float4 U : register(c9);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 nb = tex2D(N, uv); float4 c = tex2D(C, uv); float raw = tex2D(M, uv).x; float dw = tex2D(D, uv).x; float ka = tex2D(K, uv).a;"
		"  float wm = tex2D(W, uv * U.yz).x;"
		"  bool rev = Q.x > 0.5;"
		"  bool vm = S.x > 0.5 && (rev ? raw > 0.01 : raw < 0.9);"
		"  if (!vm) {"
		"    if (U.x > 0.5 && wm > (U.w > 0.0 ? U.w : 0.45)) return float4(nb.rgb, -1.0);"
		"    float a = c.a > 1.5 ? 2.0 : 1.0;"
		"    if (S.y > 0.5 && dw > 0.0 && dw < 0.9999 && c.a > 0.99) {"
		"      float2 r = uv * R.xy + R.zw;"
		"      float dist = length(float3((r.x * 2.0 - 1.0) * P.x * Q.y, (1.0 - r.y * 2.0) * P.y * Q.y, 1.0)) * dw * P.w;"
		"      float gap = c.b - dist; float tol = c.b * S.z + S.w;"
		"      bool bare = c.a > 1.5 && (T.x > 1.5 ? ka <= 0.0 : T.x > 0.5);"
		"      if (bare || gap > tol + T.y) a += 0.5;"
		"      else if (gap > tol) a += 0.25;"
		"    }"
		"    if (T.z > 0.5 && frac(a) < 0.375 && c.a > 0.99 && c.a < 2.005) {"
		"      float qf = floor(abs(c.a - (c.a > 1.5 ? 2.0 : 1.0)) * 1024.0 + 0.5);"
		"      a += floor((qf + 0.5) / 48.0) * (1.0 / 128.0);"
		"    }"
		"    return float4(nb.rgb, a);"
		"  }"
		"  float z = rev ? P.z * P.w / (P.z + raw * (P.w - P.z)) : P.z * P.w / (P.w - raw * (P.w - P.z));"
		"  float3 vp = float3((uv.x * 2.0 - 1.0) * P.x, (1.0 - uv.y * 2.0) * P.y, 1.0) * z;"
		"  float d = length(vp);"
		"  if (!(c.a > 0.99 && abs(c.b - d) <= d * Q.z + Q.w)) return float4(nb.rgb, -1.0);"
		"  float3 n = float3(c.rg, 1.0 - abs(c.r) - abs(c.g)); float t = saturate(-n.z);"
		"  n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t); n = normalize(n);"
		"  float3 nv = normalize(mul(n, (float3x3)V));"
		"  return float4(nv * 0.5 + 0.5, 4.0 + Q.y); }";

	UInt32		ownFrames = 0;
	bool ViewModelPrep(IDirect3DDevice9* Device, IDirect3DBaseTexture9* normals, UINT w, UINT h, bool viewModel = true, bool own = false, UINT tw = 0, UINT th = 0, float jitterX = 0.0f, float jitterY = 0.0f, bool f0 = false) {
		auto constant = [](const char* name) { auto f = TheShaderManager->ConstantsTable.find(name); return f == TheShaderManager->ConstantsTable.end() ? nullptr : f->second; };
		auto texture = [](const char* name) -> IDirect3DBaseTexture9* { auto f = TheTextureManager->TextureNames.find(name); return f == TheTextureManager->TextureNames.end() || !f->second ? nullptr : *f->second; };
		IDirect3DBaseTexture9* captured = texture("TESR_CapturedNormalsBuffer");
		IDirect3DTexture9* depthVm = TheTextureManager->DepthTextureViewModel;
		const D3DXMATRIX* view = (const D3DXMATRIX*)constant("TESR_ViewTransform");
		const D3DXVECTOR4* camera = constant("TESR_CameraData");
		IDirect3DBaseTexture9* depthWorld = texture("TESR_DepthBuffer");
		if (viewModel && !depthVm) viewModel = false;
		if (own && !depthWorld) own = false;
		// UNOFFICIAL 2026-11-12 PTWaterMask (core/WaterMask.h; RtViewModel.h c9 / s5): the pixels this frame's world scene drew water on (null: no
		// water in view, the key off, or the mask not made): coded -1, the game's own pixel. The pass also runs for the mask alone.
		IDirect3DTexture9* water = WaterMask::ThisFrame(Device, TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0);
		if (vmPrepFailed || !normals || !captured || (!viewModel && !own && !water && !f0) || !view || !camera || camera->y <= 0.0f) return false;
		// the skin blur's albedo target, where this span's skin draws bound it (CapturedNormals::SkinAlbedoFill copied it into the AlbedoBuffer)
		IDirect3DBaseTexture9* skinAlbedo = own && CapturedNormals::SkinAlbedoTex && !CapturedNormals::SkinAlbedoFillFailed ? (IDirect3DBaseTexture9*)CapturedNormals::SkinAlbedoTex : nullptr;
		if (!vmPrepPS) {
			// the source and its constants: core/RtViewModel.h (+ Finding 4.2 FD_SV_F0 bridge when c8.z > 0.5)
			vmPrepPS = CapturedNormals::FullScreenShader(Device, kVmPrepF0PS, "RT path trace view model", vmPrepFailed);
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
		// (2026-11-11) the tessellation's bumps lie up to Strength before the distance their pixel shader captured: within that a mark is "maybe"
		const float reach = own ? RtViewModel::OwnReach(DxvkCompute::DisplaceMaxStrength()) : 0.0f;
		const float c[10][4] = {
			{ 1.0f / (proj._11 * s), 1.0f / (proj._22 * s), nearVm, camera->y },
			{ TheRenderManager->DepthConstants.z == 0.0f ? 0.0f : 1.0f, s, RtViewModel::kMatchShare, RtViewModel::kMatchUnits },
			{ view->_11, view->_12, view->_13, view->_14 }, { view->_21, view->_22, view->_23, view->_24 },
			{ view->_31, view->_32, view->_33, view->_34 }, { view->_41, view->_42, view->_43, view->_44 },
			{ viewModel ? 1.0f : 0.0f, own ? 1.0f : 0.0f, RtViewModel::kFrontShare, RtViewModel::kFrontUnits },
			{ tw ? (float)w / (float)tw : 1.0f, th ? (float)h / (float)th : 1.0f, jitterX, jitterY },
			{ !own ? 0.0f : skinAlbedo ? 2.0f : 1.0f, reach, f0 ? 1.0f : 0.0f, 0.0f },
			// (PTWaterMask) c9: the mask on; its uv scale: this target's uv -> the share of the render rect (c7) -> the mask's own rect (the
			// viewport its water draws had, top-left) of its own size. 1, 1 whenever the mask and the depth buffer share one layout
			{ water ? 1.0f : 0.0f, water && WaterMask::Made.Width ? (tw ? (float)w / (float)tw : 1.0f) * (float)WaterMask::Rect.Width / (float)WaterMask::Made.Width : 0.0f,
			  water && WaterMask::Made.Height ? (th ? (float)h / (float)th : 1.0f) * (float)WaterMask::Rect.Height / (float)WaterMask::Made.Height : 0.0f,
			  0.0f } };	// (w: 2026-11-13 PTAlbedoCoverage, 0 = 0.45; game MSAA is retired)
		// one full-screen draw, everything it changes put back (as CapturedNormals::MergeIntoNormals)
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; IDirect3DBaseTexture9* tex[6] = {};	// (PTWaterMask: + s5, c9)
		IDirect3DSurface9* rt[4] = {}; D3DVIEWPORT9 vp = {};
		DWORD fvf = 0; float saved[10][4] = {};
		Device->GetVertexShader(&vs); Device->GetPixelShader(&ps); Device->GetVertexDeclaration(&decl); Device->GetFVF(&fvf);
		Device->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		for (DWORD i = 0; i < 6; i++) Device->GetTexture(i, &tex[i]);
		for (DWORD i = 0; i < 4; i++) Device->GetRenderTarget(i, &rt[i]);
		Device->GetViewport(&vp); Device->GetPixelShaderConstantF(0, &saved[0][0], 10);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(Device->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) Device->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[6][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD st = 0; st < 6; st++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(Device->GetSamplerState(st, kSS[i], &savedS[st][i]))) savedS[st][i] = wantS[i]; if (savedS[st][i] != wantS[i]) Device->SetSamplerState(st, kSS[i], wantS[i]); }
		for (DWORD i = 1; i < 4; i++) if (rt[i]) Device->SetRenderTarget(i, nullptr);
		Device->SetRenderTarget(0, vmNormalsSurface);
		const D3DVIEWPORT9 full = { 0, 0, w, h, 0.0f, 1.0f };
		Device->SetViewport(&full);
		Device->SetVertexShader(nullptr);
		Device->SetPixelShader(vmPrepPS);
		Device->SetPixelShaderConstantF(0, &c[0][0], 10);
		// (stages the pass does not read this frame hold the normals: any texture that is not the target)
		Device->SetTexture(0, normals); Device->SetTexture(1, captured); Device->SetTexture(2, viewModel ? (IDirect3DBaseTexture9*)depthVm : normals);
		Device->SetTexture(3, own ? depthWorld : normals); Device->SetTexture(4, skinAlbedo ? skinAlbedo : normals);
		Device->SetTexture(5, water ? (IDirect3DBaseTexture9*)water : normals);	// (PTWaterMask)
		Device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		for (DWORD st = 0; st < 6; st++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[st][i] != wantS[i]) Device->SetSamplerState(st, kSS[i], savedS[st][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) Device->SetRenderState(kRS[i], savedRS[i]);
		Device->SetRenderTarget(0, rt[0]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) Device->SetRenderTarget(i, rt[i]);
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		Device->SetViewport(&vp);
		for (DWORD i = 0; i < 6; i++) { Device->SetTexture(i, tex[i]); if (tex[i]) tex[i]->Release(); }
		Device->SetPixelShaderConstantF(0, &saved[0][0], 10);
		if (decl) { Device->SetVertexDeclaration(decl); decl->Release(); } else Device->SetFVF(fvf);
		Device->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		Device->SetVertexShader(vs); if (vs) vs->Release();
		Device->SetPixelShader(ps); if (ps) ps->Release();
		// A log line now and then (the first time, then every 1800 prepared frames): how many of the view model's texels the path tracer
		// lights (a 64 x 64 point copy read back: one small stall)
		if (own) ownFrames++;
		if (vmPrepFrames++ % 1800 == 0) {
			if (!vmCheckTex && SUCCEEDED(Device->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &vmCheckTex, nullptr)) && vmCheckTex) {
				vmCheckTex->GetSurfaceLevel(0, &vmCheckSurface);
				Device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &vmCheckSys, nullptr);
			}
			D3DLOCKED_RECT r = {};
			if (vmCheckSurface && vmCheckSys && SUCCEEDED(Device->StretchRect(vmNormalsSurface, nullptr, vmCheckSurface, nullptr, D3DTEXF_POINT)) &&
				SUCCEEDED(Device->GetRenderTargetData(vmCheckSurface, vmCheckSys)) && SUCCEEDED(vmCheckSys->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
				int lit = 0, kept = 0, people = 0, ownWorld = 0, ownPeople = 0, ownMaybe = 0;
				for (int y = 0; y < 64; y++) {
					const D3DXFLOAT16* row = (const D3DXFLOAT16*)((const char*)r.pBits + y * r.Pitch);
					for (int x = 0; x < 64; x++) {
						float a = 0.0f; D3DXFloat16To32Array(&a, row + x * 4 + 3, 1);
						if (a > 3.5f) lit++; else if (a < -0.5f) kept++; else if (a > 1.5f) people++;
						if (a > 0.5f && a < 3.5f && a - floorf(a) > 0.375f && a - floorf(a) < 0.625f) { if (a > 1.5f) ownPeople++; else ownWorld++; }
						else if (a > 0.5f && a < 3.5f && a - floorf(a) > 0.125f && a - floorf(a) < 0.375f) ownMaybe++;
					}
				}
				vmCheckSys->UnlockRect();
				// (2026-11-11 PTScreenOwn) what a tester's log shows: some marked texels where hair or a head stands before a wall; most of the
				// screen marked = the distance test is wrong for this setup (send the line; PTScreenOwn = false is the old behaviour)
				if (own) Logger::Log("UNOFFICIAL RT path trace: PTScreenOwn: of 4096 sample texels %d world + %d skin are marked (their colour / material buffers hold another surface: hair, a head or one of the game's own shaders in front of it; skin without the skin blur's colour), %d more lie within the tessellation's reach (%.1f units) before their captured surface (kept as the screen's unless hair or skin is traced there); skin colour this frame: %s; render rect %ux%u of %ux%u",
					ownWorld, ownPeople, ownMaybe, reach, skinAlbedo ? "the skin blur's target, per pixel" : "none (no skin draw bound the skin blur: every skin pixel is marked)", tw ? tw : w, th ? th : h, w, h);
				// (2026-11-12 PTWaterMask) what a tester's log shows with water in view: some texels kept; none = the mask holds no water this frame
				if (water) Logger::Log("UNOFFICIAL RT path trace: PTWaterMask: of 4096 sample texels %d keep the game's own pixel (water the game drew this frame%s); mask uv scale %.3f %.3f",
					kept, viewModel ? ", and the first-person model's pixels without a capture" : "", c[9][1], c[9][2]);
				if (viewModel) Logger::Log("UNOFFICIAL RT path trace: PTViewModel: of 4096 sample texels %d are the first-person model lit by the path tracer, %d keep the game's pixel (no capture there), %d people; FOV scale %.3f, near %.2f",
					lit, kept, people, s, nearVm);
			}
		}
		return true;
	}

	void Setup() {
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		// UNOFFICIAL 2026-11-07 (fork port): the path tracer's shared images (11, up to 20 with PTDenoiseSplit and PTRestirGI) and its
		// scratch store only when it is on as the helper starts. Order 1 already keeps it from pushing the other RT effects out, but its
		// screen-sized images were still made, in the game and in the helper, while it was off (by default, and whenever no path tracing
		// preset is picked). Switched on later, Wants asks RtCore::SetupAgain once and this runs again; Record does nothing without imageA.
		ReadSettings();
		if (!S.enabled) {
			imageA = imageB = imageDisplay = imageDepth = imageNormals = imageAlbedo = imagePano = imageFrame = imageMaterial = imageAlbedoOut = imageLamps = -1;
			imageSpec = imageGIResult = -1;
			for (int i = 0; i < 2; i++) imageResA[i] = imageResB[i] = imageResC[i] = imageSurf[i] = -1;
			cacheLog2Setup = 0;
			haveImage = false;
			samples = 0;
			Logger::Log("UNOFFICIAL RT reference path tracer: off as the RT helper starts, so its shared images are not made (switched on later, the helper starts again once to make them).");
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
		// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: decoupled primary direct sun shadow images (R16F vis, R16F signed hit, RGBA16F unshadowed sun, RGBA16F motion)
		{
			SettingManager::Configuration::ConfigNode sn;
			const bool wantSun = TheSettingManager->Config.FillNode(&sn, kSection, "PTDenoiseSplitSun") ? (sn.IntValue != 0) : true;
			imageSunVis = wantSun ? RtCore::AddImage("NVR RT path trace sun vis", w, h, NVR_EXT_SHARED_R16F) : -1;
			imageSunHit = wantSun ? RtCore::AddImage("NVR RT path trace sun hit", w, h, NVR_EXT_SHARED_R16F) : -1;
			imageSunDirect = wantSun ? RtCore::AddImage("NVR RT path trace sun direct", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
			imageSunMotion = wantSun ? RtCore::AddImage("NVR RT path trace sun motion", w, h, NVR_EXT_SHARED_RGBA16F) : -1;
		}
		imageLamps = RtCore::AddImage("NVR RT path trace lamps", kLightTexels, kLampRows, NVR_EXT_SHARED_RG32F);
		// (2026-11-07) the scratch store (binding 20): PTRestirDI's reservoirs, two sets of 16 bytes a pixel at the screen's size
		// + (2026-11-07) PTRadianceCache's table after them (64 bytes an entry; its size is fixed at the helper start: PTCacheEntriesLog2 needs a restart)
		cacheLog2Setup = (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "PTCacheEntriesLog2"), 12), 22);
		const UINT baseScratchMB = (UINT)((EmissiveBaseWords(w, h, cacheLog2Setup) * 4ull + 4ull * RtEmissiveRow::kScratchWords + (1ull << 20) - 1) >> 20) + 1u;
		const UINT splitScratchMB = (UINT)(((SplitBaseWords(w, h, cacheLog2Setup) + SplitNeedWords(w, h)) * 4ull + (1ull << 20) - 1) >> 20) + 1u;
		splitScratchReady = splitScratchMB <= 1024u;
		const UINT sunSplitScratchMB = (UINT)(((SunSplitBaseWords(w, h, cacheLog2Setup) + SunSplitNeedWords(w, h)) * 4ull + (1ull << 20) - 1) >> 20) + 1u;
		sunSplitScratchReady = sunSplitScratchMB <= 1024u;
		RtCore::AskScratch(sunSplitScratchReady ? sunSplitScratchMB : (splitScratchReady ? splitScratchMB : baseScratchMB));
		sunDenoiseView = RtShadowDenoise::View();
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
		const float uKey = S.gameLamps == 4 ? (RtLightUnits::Gamma() * 17.0f + RtLightUnits::LightScale() * 257.0f + RtLightUnits::SkyScale() * 4099.0f + RtLightUnits::AmbientScale() * 65537.0f) : 0.0f;
		const float key[8] = { (float)S.bounces + 100.0f * (float)S.bounceHalf + 1000.0f * (float)S.lastSky + 10000.0f * (float)S.lastBounce + 100000.0f * (float)S.lampFalloff + 1000000.0f * S.lampGain,
			S.sky + 100.0f * (float)S.restirDIRays + 1000.0f * (float)S.bounceLights, S.ambient + uKey,	// (perf-ptcost: + the ray budget keys; 2026-11-19: + live PBR unit scales)
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

	// UNOFFICIAL 2026-11-10 early kick (RtPathTrace.h SubmitBeforeWorld): Record runs inside RtCore::SubmitEarly at the world's start
	// (earlyRecording), when nothing of this frame's world is drawn: no G-buffer, no frame copy. earlyFrame: the frame whose trace went
	// early; RtCore::Frame's exchange after the world must not record it again (its copies out would follow a second, other trace).
	bool		earlyRecording = false;
	UInt32		earlyFrame = ~0u;
	// UNOFFICIAL 2026-11-11 PTEarlyLook (core/RtEarlyLook.h): set by an early Record that reached its copies out. earlyLookFrame: its first-surface
	// colour was copied (slice 1 can run); earlyDepthFrame: + its trace wrote the first hit's depth into the display's alpha (slice 2; not with
	// PTRestirGI, whose result has none: earlyDepthGI); earlyTolerance: slice 2's depth floor ((8 + the bumps' slack / 0.35) / farZ, as the trace's
	// `same`). mergedFrame: Show made the merged first surface this frame; FirstSurface() is then what every reader of the trace's colour gets.
	UInt32		earlyLookFrame = ~0u, earlyDepthFrame = ~0u, mergedFrame = ~0u;
	bool		earlyDepthGI = false;
	float		earlyTolerance = 0.0f;
	IDirect3DTexture9* FirstSurface() {
		return mergedFrame == TheFrameRateManager->FrameNumber && RtEarlyLook::Merged ? RtEarlyLook::Merged : albedoTexture;
	}

	void Record(IDirect3DDevice9* Device) {
		if (!earlyRecording && earlyFrame == TheFrameRateManager->FrameNumber) return;	// (2026-11-10 early kick: recorded at the world's start)
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
		// UNOFFICIAL 2026-11-10 early kick: recorded before the world is drawn, the G-buffer (and the view model's, the frame copy) would be
		// last frame's at this frame's camera: traced first surface instead, as PTPrimary false (the player's setting stays as it is)
		const bool primary = S.primary && !earlyRecording;
		// UNOFFICIAL 2026-11-12 (docs/next/PT-COMPAT-AUDIT-2026-11-12.md items 14, 16): said once, not refused. With the game's MSAA the resolved
		// material / colour buffers hold AVERAGES of class codes and flags at edges (material rims, dark outlines), and the first-person
		// model is not captured (it stays the game's raster pixel), as at a RenderScale below 1.
		if (primary) {
			static bool toldRect = false;
			if (!toldRect && RenderRect::On) { toldRect = true; Logger::Log("UNOFFICIAL RT path trace: RenderScale is below 1: the first-person arms and gun are not path traced at a reduced render size (they keep the game's own lighting)."); }
		}
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
		// UNOFFICIAL 2026-11-11 PTScreenOwn: the same pass in every PTPrimary frame (also third person, without the denoiser, at RenderScale
		// below 1): it marks the pixels whose AlbedoBuffer / MaterialBuffer values are another surface's (hair, a head, the game's own shaders
		// in front of a captured wall; skin without the skin blur's colour: core/RtViewModel.h), read by the trace's ScreenHit
		const bool vmWant = primary && S.viewModel && S.denoise && fullRect && CapturedNormals::VmFrame == TheFrameRateManager->FrameNumber;
		const D3DXVECTOR4* capturedData = constant("TESR_CapturedNormalsData");
		const bool ownWant = primary && S.screenOwn && capturedData && capturedData->x > 0.5f;
		// UNOFFICIAL 2026-11-12 PTWaterMask (core/WaterMask.h): the world scenes of the next frames redraw their water into a mask while a
		// PTPrimary frame asks for it here; the prep pass (run for it alone too, when this frame's world drew water) codes those pixels -1
		const bool waterWant = primary && WaterMask::Enabled;
		if (waterWant) WaterMask::Request(TheFrameRateManager->FrameNumber);
		bool prepped = false;
		const bool maskWant = primary && (WaterMask::Enabled || WaterMask::GrassOn);	// (2026-11-12 PTGrassOwnPixel: the mask may hold grass alone)
		const bool f0Want = primary && S.specular && capturedData && capturedData->x > 0.5f;
		if ((vmWant || ownWant || f0Want || (maskWant && WaterMask::WrittenFrame == TheFrameRateManager->FrameNumber)) && gNormals && albedoData && albedoData->x > 0.5f) {
			prepped = ViewModelPrep(Device, gNormals, w, h, vmWant, ownWant, tw, th, c.rect[0], c.rect[1], f0Want);
			vmThisFrame = prepped && vmWant;
		}
		// (2026-11-19) RenderRect::On (tw < w || th < h): copy only the active top-left render rect of the screen-shaped G-buffer / frame textures
		const RECT subRect = { 0, 0, (LONG)tw, (LONG)th };
		const RECT* const inRect = (!fullRect && tw > 0 && th > 0) ? &subRect : nullptr;
		IDirect3DBaseTexture9* normalsIn = prepped ? (IDirect3DBaseTexture9*)vmNormals : gNormals;
		if (primary && imageDepth >= 0 && imageNormals >= 0 && imageAlbedo >= 0 && c.forward[3] > 0.0f && albedoData && albedoData->x > 0.5f &&
			gDepth && normalsIn && gAlbedo && RtCore::CopyIn(imageDepth, gDepth, inRect) && RtCore::CopyIn(imageNormals, normalsIn, inRect) && RtCore::CopyIn(imageAlbedo, gAlbedo, inRect))
			flags |= 8u;
		else vmThisFrame = false;
		// the next frame's first-person render captures the model while the path tracer can light it (the screen's G-buffer, the full rect)
		// (2026-11-10 early kick: recorded at the world's start this is THIS frame's first-person render, and the trace does not use it)
		CapturedNormals::VmWanted = S.viewModel && primary && S.denoise && fullRect;
		// (2026-11-02 e) the screen's roughness / metal (CapturedNormals' MaterialBuffer) with the first hit's G-buffer
		const D3DXVECTOR4* materialData = constant("TESR_MaterialBufferData");
		IDirect3DBaseTexture9* gMaterial = texture("TESR_MaterialBuffer");
		if ((flags & 8u) && S.specular && imageMaterial >= 0 && materialData && materialData->x > 0.5f && gMaterial && RtCore::CopyIn(imageMaterial, gMaterial, inRect))
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
		if (primary && imageFrame >= 0 && SUCCEEDED(Device->GetRenderTarget(0, &frame)) && frame) {
			D3DSURFACE_DESC fd = {};
			const RECT* const frameRect = (inRect && SUCCEEDED(frame->GetDesc(&fd)) && fd.Width >= tw && fd.Height >= th) ? inRect : nullptr;
			if (RtCore::CopyIn(imageFrame, frame, frameRect)) flags |= 32u;
			frame->Release();
		}
		const int normalAtlas = RtCore::NormalAtlasImage();	// (2026-11-02 d) NormalTextures
		if (normalAtlas >= 0 && albedoAtlas >= 0) flags |= 64u;
		static UInt32 lastFlags = 0;
		if (flags != lastFlags) { samples = 0; lastFlags = flags; }	// a source came or went (the G-buffer, the panorama, an atlas): start over
		c.accum[0] = (float)samples; c.accum[1] = (float)S.bounces; c.accum[2] = (float)S.samples; c.accum[3] = (float)flags;
		const ShaderConstants& k = TheShaderManager->ShaderConst;
		memcpy(c.sunDir, &sunKey, 16);
		if (S.gameLamps == 4) {	// UNOFFICIAL 2026-11-19 (Finding 4.1) match forward / deferred PBR decode gamma + light / sky / ambient scales
			RtLightUnits::DecodeColor(c.sunColor, k.sunColor.x, k.sunColor.y, k.sunColor.z, RtLightUnits::LightScale());
			RtLightUnits::DecodeColor(c.skyZenith, k.skyColor.x, k.skyColor.y, k.skyColor.z, RtLightUnits::SkyScale() * S.sky);
			RtLightUnits::DecodeColor(c.skyHorizon, k.horizonColor.x, k.horizonColor.y, k.horizonColor.z, RtLightUnits::SkyScale() * S.sky);
			RtLightUnits::DecodeColor(c.ambient, k.sunAmbient.x, k.sunAmbient.y, k.sunAmbient.z, RtLightUnits::AmbientScale() * S.ambient);
		}
		else {
			c.sunColor[0] = Lin(k.sunColor.x); c.sunColor[1] = Lin(k.sunColor.y); c.sunColor[2] = Lin(k.sunColor.z);
			c.skyZenith[0] = Lin(k.skyColor.x) * S.sky; c.skyZenith[1] = Lin(k.skyColor.y) * S.sky; c.skyZenith[2] = Lin(k.skyColor.z) * S.sky;
			c.skyHorizon[0] = Lin(k.horizonColor.x) * S.sky; c.skyHorizon[1] = Lin(k.horizonColor.y) * S.sky; c.skyHorizon[2] = Lin(k.horizonColor.z) * S.sky;
			c.ambient[0] = Lin(k.sunAmbient.x) * S.ambient; c.ambient[1] = Lin(k.sunAmbient.y) * S.ambient; c.ambient[2] = Lin(k.sunAmbient.z) * S.ambient;
		}
		c.sunColor[3] = tanf(0.5f * 0.53f * 0.0174532925f);	// the sun's disc
		c.skyZenith[3] = exterior ? 1.0f : 0.0f;
		c.skyHorizon[3] = S.glow;
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
		// UNOFFICIAL 2026-11-11 PTEarlyLook slice 2: the trace writes its first hit's view depth (frame row texel kEarlyLookTexel); not with PTRestirGI
		const bool earlyDepth = earlyRecording && S.earlyLook >= 2 && !giReady;
		if (earlyRecording) earlyDepthGI = S.earlyLook >= 2 && giReady;
		emissiveCount = 0;
		// (2026-11-10 early kick: + earlyRecording, its camera jitter flag rides in the frame row's texel 23)
		// The lamp image is filled from the CPU lists: at the world's start the game's lamp table (ManyLamps) is last frame's, built at its
		// first world draw (the flashlight, the shadow / tracked lamp lists as ShaderManager left them); the glow lights and PTEmissive's
		// records are the scene RtCore gathered for this exchange.
		if (imageLamps >= 0 && (lampCount > 8 || glowCount > 0 || restir || cache || energy || tess || materials || emissiveWanted || earlyRecording)) {
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
					// UNOFFICIAL 2026-11-10 early kick, texel 23.x (RtPathTrace.comp gCameraJitter): 1 = the camera rays at the pixel centre + the
					// depth's jitter (c.rect.xy), as ScreenHit places the screen's first surface: the traced first surface then lies on the pixel
					// the game rasterises, which the denoiser's and the upscaler's depth / normals / motion describe. Written every frame (the
					// lock discards: an unwritten texel holds anything).
					row[46] = earlyRecording ? 1.0f : 0.0f; row[47] = S.denoiseCeiling;	// (23.y: 2026-11-11 PTDenoiseCeiling)
					// UNOFFICIAL 2026-11-11 PTEarlyLook slice 2 (RtPathTrace.comp gEarlyLook): 1 = the display's alpha is the first hit's view z / farZ
					// (0 a miss, -1 a glow the game draws) instead of 1. Written every frame (the lock discards), 0 outside early-look frames.
					row[2 * kEarlyLookTexel] = earlyDepth ? 1.0f : 0.0f; row[2 * kEarlyLookTexel + 1] = 0.0f;
					// UNOFFICIAL 2026-11-15 (rt-reuse) PTRestirCGNS for ReSTIR DI (RtPathTrace.comp RestirCgnsK): written every frame (the lock discards)
					row[2 * kCgnsTexel] = (float)S.restirCGNS; row[2 * kCgnsTexel + 1] = 0.0f;
					// UNOFFICIAL 2026-11-11 texel 31 (RtPathTrace.comp FlashShape): the flashlight's NearFade (x) and HotspotLimit (y) as its light pass
					// takes them (FlashlightEffect::ApplyLook: Main or Interiors), only while lamp 0 is the flashlight (texel 15 holds its cone code);
					// 0 = off, as every other frame (the lock discards: written every frame)
					{
						const FlashlightEffect* fe = TheShaderManager->Effects.Flashlight;
						const bool flashLamp = fe && row[30] > 0.5f;
						row[62] = flashLamp ? fe->Settings.NearFade : 0.0f; row[63] = flashLamp ? fe->Settings.HotspotLimit : 0.0f;
						row[31] = flashLamp ? fe->Deferred.Cookie : 0.0f;	// texel 15.y (2026-11-11): the lens pattern's strength (RtPathTrace.comp FlashCookie)
						row[2 * kEarlyLookTexel + 1] = flashLamp ? fe->Deferred.Falloff : 0.0f;	// texel 288.y (was 0, reserved; written above, unread): the beam's Deferred Falloff, HotspotLimit's shape
					}
					RtTessContact::PackRow(row, slack, contact);	// (2026-11-07 rtgap-tess) texels 16-21
					{	// (2026-11-07 ptaaa-materials) texels 24-26; (2026-11-09 pt-mirrors) + PTDenoiseMirrorSurface, only while the split specular
						// light goes through the path tracer's own denoiser (not to DLSS RR, whose specular albedo must stay the BRDF's)
						RtMaterials::Keys keys = S.materials;
						keys.mirrorSurface = (flags & 4194304u) && S.denoise && !rrDenoising ? S.denoiseMirrorSurface : 0.0f;
						RtMaterials::PackRow(row, keys);
						// UNOFFICIAL 2026-11-11 PTGlassSurface (RtPathTrace.comp gGlassSurface): a pane's own lit share. Written every frame (the lock discards)
						row[2 * kGlassSurfaceTexel] = S.glassSurface;	// (289.y: PTEmissive's weight floor, written below with the emissive row)
					}
					row[54] = S.bounceRoughen; row[55] = S.cacheShort; row[60] = (S.hairFirst && !S.blendedLayer ? 1.0f : 0.0f) + (S.hairFirst ? 2.0f : 0.0f);	/* (not with PTBlendedLayer: the game's own hair comes over) */ row[61] = (float)S.rouletteFrom;	/* texel 30.y: 2026-11-10 PTRouletteFrom */	// UNOFFICIAL 2026-11-09 PTBounceRoughen: texel 27 (RtPathTrace.comp LoadBounceRoughen); y: 2026-11-10 PTCacheShort
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
					{	// UNOFFICIAL 2026-11-11 (pt-emissive-cost) PTEmissive's cost keys, texels 289.y-291.x (RtPathTrace.comp LoadEmCostKeys). The floor in
						// EmWeight's units (an instance's power x cos / d^2; x PTGlow / pi = the light it gives a matte surface, in the units of the
						// sun's colour): base = PTEmissiveFloor + PTEmissiveCut x the sky's luminance (outdoors), sun = PTEmissiveCut x the sun's
						// (while it is up; the trace multiplies it by the surface's cosine to the sun). Written every frame (the lock discards).
						const float skyLuma = c.skyZenith[3] > 0.5f ? 0.5f * (0.2126f * (c.skyZenith[0] + c.skyHorizon[0]) + 0.7152f * (c.skyZenith[1] + c.skyHorizon[1]) + 0.0722f * (c.skyZenith[2] + c.skyHorizon[2])) : 0.0f;
						const float sunLuma = c.sunDir[3] > 0.5f ? 0.2126f * c.sunColor[0] + 0.7152f * c.sunColor[1] + 0.0722f * c.sunColor[2] : 0.0f;
						RtEmissiveRow::CostKeys costKeys;
						costKeys.cut = S.emissiveCut; costKeys.floor = S.emissiveFloor; costKeys.hits = (float)S.emissiveHits; costKeys.roulette = S.emissiveRoulette;
						emissiveFloorBase = RtEmissiveRow::PackCost(row, costKeys, skyLuma, sunLuma, S.glow, emissiveCount > 0);	// (RtEmissiveRow.h, shared with tools/nvr-rt-host)
						// UNOFFICIAL 2026-11-12 PTAlbedoGamma (audit item 6): texel 291.y = the exponent that turns the AlbedoBuffer's texture^G (G = [Shaders.PBR.Linear]
						// Gamma as the templates' LinColor uses it this frame, 1 with Linear off) into the texture^2.2 the trace works in; 0 = none (G is 2.2, or the key off)
						{
							const float g = TheShaderManager->Shaders.PBR ? (std::max)(TheShaderManager->Shaders.PBR->Constants.Linear.x, 1.0f) : 1.0f;
							const float e = 2.2f / g;
							row[2 * (kEmCostTexel + 2) + 1] = S.albedoGamma && fabsf(e - 1.0f) > 0.002f ? e : 0.0f;
							static_assert(kEmCostTexel + 3 < kLightTexels, "PTScreenOwnNarrow: frame row texel 292");
							row[2 * (kEmCostTexel + 3)] = S.screenOwn && S.screenOwnNarrow ? 1.0f : 0.0f;	// UNOFFICIAL 2026-11-12 PTScreenOwnNarrow (RtPathTrace.comp gOwnNarrow), 292.y free
							row[2 * (kEmCostTexel + 3) + 1] = S.hairOverSky ? 1.0f : 0.0f;	// UNOFFICIAL 2026-11-12 PTHairOverSky (RtPathTrace.comp gHairSky); the next free texel: 293
							static_assert(kEmCostTexel + 4 < kLightTexels, "PTGlowTexel: frame row texel 293");
							row[2 * (kEmCostTexel + 4)] = S.glowTexel;	// UNOFFICIAL 2026-11-12 PTGlowTexel (RtPathTrace.comp gGlowTexel)
							// UNOFFICIAL 2026-11-12 (ptbump) texels 293.y, 294.x, 294.y (RtPathTrace.comp LoadMatKeys); 295-296 reserved for it, unused
							static_assert(kEmCostTexel + 6 < kLightTexels, "ptbump: frame row texels 294-295");
							row[2 * (kEmCostTexel + 6)] = S.roughFresnel; row[2 * (kEmCostTexel + 6) + 1] = 0.0f;	// 295.x PTRoughFresnel; 295.y and 296 reserved for ptbump, unused
							row[2 * (kEmCostTexel + 4) + 1] = S.bumpNormals ? 1.0f : 0.0f;
							row[2 * (kEmCostTexel + 5)] = S.bumpDetail; row[2 * (kEmCostTexel + 5) + 1] = S.matteNoHighlight ? 1.0f : 0.0f;
							{
								static int toldBump = -1;
								const bool landBumpReady = CapturedNormals::BumpCompiledIn() || CapturedNormals::V2CompiledIn();
								const int nowBump = (S.bumpNormals ? 1 : 0) | (S.matteNoHighlight ? 2 : 0) | (landBumpReady ? 4 : 0) | (S.roughFresnel > 0.0f ? 8 : 0);
								if (toldBump != nowBump) {
									toldBump = nowBump;
									Logger::Log("UNOFFICIAL RT path trace: PTBumpNormals %s (the first hit's shading normal: %s; a traced surface's own normal map%s), PTBumpDetail %.2f, PTMatteNoHighlight %s (meshes the game draws without a highlight are matte at the first hit%s), PTRoughFresnel %.2f",
										S.bumpNormals ? "on" : "off", landBumpReady ? "the land's recorded bumped normal" : "NO land record in this start's shaders ([Main.Main.Quality] CapturedBumpNormals or GBufferV2, needs the DXVK fork and AlbedoBuffer)",
										TheSettingManager->GetSettingI(kSection, "RealNormalMaps") ? "" : " needs RealNormalMaps, which is off", S.bumpDetail, S.matteNoHighlight ? "on" : "off",
										CapturedNormals::PtCodesCompiled ? "" : "; NOT in this start's shaders: needs the DXVK fork and AlbedoBuffer", S.roughFresnel);
								}
							}
							// UNOFFICIAL 2026-11-12 PTAlbedoCoverage (RtPathTrace.comp gAlbedoCover), texel 297.x (294-296 are another change's): only in a
							// frame whose records are multisampled (the game's MSAA: their resolve is the samples' mean); 297.y free
							static_assert(kEmCostTexel + 8 < kLightTexels, "PTAlbedoCoverage: frame row texel 297");
							row[2 * (kEmCostTexel + 8)] = 0.0f; row[2 * (kEmCostTexel + 8) + 1] = 0.0f;
							{	// one line whenever what is sent changes (a run's log then says whether the key reached the trace)
								static float toldCover = -1.0f;
								const float sent = row[2 * (kEmCostTexel + 8)];
								if (toldCover != sent && (sent > 0.5f || toldCover >= 0.0f)) {
									toldCover = sent;
									Logger::Log("UNOFFICIAL RT path trace: PTAlbedoCoverage %s (key %s, the records not multisampled): a pixel counts as written from half its alpha, as without MSAA",
										sent > 0.5f ? "ACTIVE" : "not active", S.albedoCoverage ? "on" : "off");
								}
							}
							{	// UNOFFICIAL 2026-11-13 the flashlight's keys (RtPathTrace.comp gFlashOwn, FlashShape), texels 305, 306 and 298; written every
								// frame (the lock discards), all 0 unless lamp 0 is the flashlight (row[30]: its cone's code)
								static_assert(kFlashOwnTexel + 1 < kLightTexels && 2 * kFlashRasterTexel2 >= RtRestirRow::kRowFloatsAll, "the flashlight's frame row texels 298, 305, 306");
								const FlashlightEffect* fe = TheShaderManager->Effects.Flashlight;
								const PBRShaders* pbr = TheShaderManager->Shaders.PBR;
								const bool flashLamp = fe && row[30] > 0.5f;
								// PTFlashOwnPixels: not while the flashlight's game lamp lights those pixels itself (GameLight + GameLightUnderPT)
								const bool gameLamp = fe && FlashlightDeferred::Usable() && fe->Deferred.GameLight && fe->Deferred.GameLightUnderPT;
								const bool own = flashLamp && S.flashOwn && !gameLamp;
								row[2 * kFlashOwnTexel] = own ? 1.0f : 0.0f;
								// PTFlashRasterShape: the light pass's decode of its beam (Includes/LampFalloff.hlsl LampAtt as FlashlightDeferred.hlsl is
								// compiled: att^k, k = FalloffMatch's exponent or the linear gamma; or the physical shape's when it is on this frame) and
								// the lamps' light scale the pass multiplies its light by (LIN_SCALE_LIGHT)
								const bool shape = flashLamp && S.flashRasterShape;
								const float gammaL = pbr ? pbr->Constants.Linear.x : 1.0f;
								const float k = gammaL > 1.0f ? (pbr->Constants.Linear.z > 0.0f ? pbr->Constants.Linear.z : gammaL) : 1.0f;
								const bool shaped = shape && pbr && PBRShaders::LampFalloffShapeIn() && pbr->Constants.LampFalloff.x > 0.0f;
								row[2 * kFlashOwnTexel + 1] = shape ? GameLightScale() : 0.0f;
								row[2 * kFlashRasterTexel] = shape ? k : 0.0f;
								row[2 * kFlashRasterTexel + 1] = shaped ? (std::max)(gammaL, 1.0f) / 2.2f : 0.0f;
								row[2 * kFlashRasterTexel2] = shaped ? pbr->Constants.LampFalloff.y : 0.0f;
								row[2 * kFlashRasterTexel2 + 1] = shaped ? pbr->Constants.LampFalloff.z : 0.0f;
								static int toldFlash = -1;
								const int nowFlash = !flashLamp ? 0 : 1 | (own ? 2 : 0) | (shape ? 4 : 0) | (shaped ? 8 : 0) | (gameLamp ? 16 : 0);
								if (toldFlash != nowFlash) {
									toldFlash = nowFlash;
									if (flashLamp) Logger::Log("UNOFFICIAL RT path trace: flashlight: PTFlashOwnPixels %s (%s), PTFlashRasterShape %s (the beam %s)",
										S.flashOwn ? "on" : "off", own ? "the game's own pixels whose surface is in the traced scene get the beam's direct light from the tracer" :
										gameLamp ? "not used while GameLightUnderPT is on: the flashlight's game lamp is lit then" : "the game's own pixels without records get no flashlight light",
										S.flashRasterShape ? "on" : "off", !shape ? "falls off as the path tracer's lamps do, x its cone" : shaped ? "as the light pass draws it: Falloff, cone squared, the physical lamp shape's decode" :
										"as the light pass draws it: Falloff, cone squared, the lamps' falloff decode");
								}
							}
							row[2 * (kEmCostTexel + 4)] = S.glowTexel;	// UNOFFICIAL 2026-11-12 PTGlowTexel (RtPathTrace.comp gGlowTexel); 293.y free
							static float toldGamma = -1.0f;
							if (toldGamma != g) { toldGamma = g; Logger::Log("UNOFFICIAL RT path trace: PTAlbedoGamma %s: the scene's linear gamma is %.2f, the screen's surface colours are read ^%.3f", S.albedoGamma ? "on" : "off", g, S.albedoGamma ? e : 1.0f); }
						}
						// the frame's decision, logged when it changes (indoors <-> outdoors, the sun up <-> down, a key, emissive instances or none)
						static float told[6] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
						static int toldLines = 0;
						const float now[6] = { c.skyZenith[3], c.sunDir[3], S.emissiveCut, S.emissiveFloor, (float)S.emissiveHits + 100.0f * S.emissiveRoulette, emissiveCount > 0 ? 1.0f : 0.0f };
						const bool keysOn = S.emissiveCut > 0.0f || S.emissiveFloor > 0.0f || S.emissiveHits > 0 || S.emissiveRoulette > 0.0f;
						if (emissiveWanted && keysOn && memcmp(told, now, sizeof(now)) != 0 && toldLines < 48) {
							memcpy(told, now, sizeof(now));
							toldLines++;
							Logger::Log("UNOFFICIAL RT path trace: PTEmissive this frame (%s, sun %s): %d emissive instances listed of %d glowing meshes; an instance is light-sampled at a "
								"surface only where its estimated light is at least %.5f + %.5f x the surface's cosine to the sun (PTEmissiveFloor %.4g + PTEmissiveCut %.3g x sky %.4f / sun %.4f; "
								"under it the bounce rays carry its glow), PTEmissiveHits %d, PTEmissiveRoulette %.3g.", c.skyZenith[3] > 0.5f ? "outdoors" : "indoors", c.sunDir[3] > 0.5f ? "up" : "down",
								emissiveCount, (int)RtCore::GlowLights().size(), S.emissiveFloor + S.emissiveCut * skyLuma, S.emissiveCut * sunLuma, S.emissiveFloor, S.emissiveCut, skyLuma, sunLuma,
								S.emissiveHits, S.emissiveRoulette);
						}
					}
					row[2 * RtEmissiveRow::kFrameTexel] = (float)emissiveCount;
					row[2 * RtEmissiveRow::kFrameTexel + 1] = (float)(EmissiveBaseWords(w, h, cacheLog2Setup) / 4096ull);
					row[2 * kSplitBaseTexel] = splitScratchReady ? (float)(SplitBaseWords(w, h, cacheLog2Setup) / 4096ull) : 0.0f;
					// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: decouple primary direct sun diffuse for SIGMA shadow denoising when room permits
					const bool sunSplitRoom = RtCore::PassesLeft() >= (emissiveCount > 0 ? 1 : 0) + 1 + (cache ? 1 : 0) + 2;
					const bool sunSplitWant = sunSplitScratchReady && sunSplitRoom && S.denoise && S.denoiseSplitSun && (flags & 512u) != 0u && !giReady &&
						S.debugView == 0 && S.samples == 1 && sunUp && !earlyRecording &&
						imageSunVis >= 0 && imageSunHit >= 0 && imageSunDirect >= 0 && shaderSunUnpack >= 0 && shaderSunDenoise >= 0 &&
						!RtSunShadows::HelperDenoisedThisFrame();
					row[2 * kSplitBaseTexel + 1] = sunSplitWant ? (float)(SunSplitBaseWords(w, h, cacheLog2Setup) / 4096ull) : 0.0f;
					sunSplitThisFrame = sunSplitWant;
				}
				lampTexture->UnlockRect(0);
				if (RtCore::CopyIn(imageLamps, lampTexture)) { flags |= 1024u | (gridOn ? 4096u : 0u) | (restir ? 262144u : 0u) | (cache ? 524288u : 0u) | (restir || cache || energy ? 1048576u : 0u); c.misc[0] = (float)lampCount; c.rect[2] = (float)glowCount; }
				else { restirState.valid = false; sunSplitThisFrame = false; }
			}
		}
		else sunSplitThisFrame = false;
		if (!(flags & 1024u)) sunSplitThisFrame = false;
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
			r.rect[2] = (float)((flags & 1u ? 1u : 0u) | (giHistory ? 2u : 0u) | 4u
				| (S.restirDupMap ? 8u : 0u) | ((unsigned)S.restirCGNS << 4));	// UNOFFICIAL 2026-11-15 (rt-reuse) RtRestirGI.comp DupMap / CgnsCandidates; both off: as before
			r.rect[3] = S.denoiseCeiling;	// UNOFFICIAL 2026-11-11 PTDenoiseCeiling (was unused; 0 = no ceiling)
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
				const EmissiveConstants ec = { { (float)emissiveCount, (float)(EmissiveBaseWords(w, h, cacheLog2Setup) / 4096ull), emissiveFloorBase, 0.0f } };	// (z: 2026-11-11 pt-emissive-cost)
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
			const UINT imgCount = split ? 16u : 15u;
			const UINT writeMask = 0x7u | (flags & 512u ? 1u << 13 : 0u) | (split ? 1u << 15 : 0u);
			const UINT gx = (tw + 7) / 8, gy = (th + 7) / 8;
			// UNOFFICIAL 2026-11-19 (Phase 3B): 3-pass split (Primary+Temporal -> Spatial+Boiling -> Secondary Bounces) for realtime 1-SPP ReSTIR DI.
			const bool useGrid = (flags & 4096u) != 0u;
			const int p1Shader = useGrid ? shaderPrimaryGrid : shaderPrimary;
			const int p3Shader = useGrid ? shaderBounceGrid : shaderBounce;
			const bool sunSplitActive = sunSplitThisFrame;
			const bool want3Pass = splitScratchReady && (flags & 512u) != 0u && (flags & 262144u) != 0u && S.debugView == 0 && S.samples == 1 &&
				p1Shader >= 0 && shaderSpatial >= 0 && p3Shader >= 0 && RtCore::PassesLeft() >= (3 + ((flags & 524288u) ? 1 : 0) + (sunSplitActive ? 2 : 0));
			bool dispatched = false;
			if (want3Pass && RtCore::AddPass(p1Shader, images16, imgCount, 0u, &c, sizeof(c), gx, gy)) {
				if (RtCore::AddPass(shaderSpatial, images16, imgCount, 0u, &c, sizeof(c), gx, gy) &&
					RtCore::AddPass(p3Shader, images16, imgCount, writeMask, &c, sizeof(c), gx, gy)) {
					dispatched = true;
					static bool splitLogged = false;
					if (!splitLogged) { splitLogged = true; Logger::Log("UNOFFICIAL RT path trace: 3-pass split active (Primary+Temporal -> Spatial+Boiling -> Secondary Bounces)"); }
				} else return;
			}
			if (!dispatched && !RtCore::AddPass(useGrid ? shaderGrid : shader, images16, imgCount, writeMask, &c, sizeof(c), gx, gy)) return;
			// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun: unpack decoupled primary direct sun and denoise sun visibility via helper RtShadowDenoise (SIGMA + actor anti-lag)
			if (sunSplitActive) {
				const float unpackParams[4] = { (float)tw, (float)th, (float)(SunSplitBaseWords(w, h, cacheLog2Setup) / 4096ull), 0.0f };
				const int unpackImages[3] = { imageSunVis, imageSunHit, imageSunDirect };
				bool sunOk = RtCore::AddPass(shaderSunUnpack, unpackImages, 3, 0x7u, unpackParams, sizeof(unpackParams), gx, gy);
				if (sunOk && RtCore::PassesLeft() >= 1) {
					RtShadowDenoise::View now;
					for (int i = 0; i < 3; i++) { now.forward[i] = c.forward[i]; now.right[i] = c.right[i]; now.up[i] = c.up[i]; }
					const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
					now.eye[0] = (double)cam.x + eye.x; now.eye[1] = (double)cam.y + eye.y; now.eye[2] = (double)cam.z + eye.z;
					now.jitter[0] = c.rect[0]; now.jitter[1] = c.rect[1]; now.farZ = c.forward[3]; now.valid = true;
					static UINT lastSunW = 0, lastSunH = 0;
					static UInt32 lastSunFrame = 0;
					const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
					RtShadowDenoise::View before = sunDenoiseView;
					before.valid = sunDenoiseView.valid && lastSunW == tw && lastSunH == th && frameNow - lastSunFrame <= 2 && !TheShaderManager->GameState.isCameraCut;
					static D3DXVECTOR3 lastPtSun(0.0f, 0.0f, 0.0f);
					const D3DXVECTOR3 sunNow(c.sunDir[0], c.sunDir[1], c.sunDir[2]);
					if (D3DXVec3Dot(&sunNow, &lastPtSun) < 0.99985f) before.valid = false;
					lastPtSun = sunNow;
					lastSunW = tw; lastSunH = th; lastSunFrame = frameNow;
					IDirect3DBaseTexture9* motion = texture("TESR_ObjectMotionBuffer");
					const D3DXVECTOR4* motionData = constant("TESR_ObjectMotionData");
					const bool moved = imageSunMotion >= 0 && tw == w && th == h && motion && motionData && motionData->x > 0.0f && RtCore::CopyIn(imageSunMotion, motion);
					RtShadowDenoise::Settings ds;
					ds.maxHistory = 16.0f; ds.maxRadius = 24.0f; ds.stabilize = 4.0f;
					RtShadowDenoise::TimeScale(ds, RtDenoise::HistoryClock());
					RtShadowDenoise::DenoiseConstants d;
					RtShadowDenoise::Fill(d, now, before, tw, th, tw, th, 0, 1, sunDenoiseFrames, ds, moved, c.sunColor[3]);
					d.history[3] = 1.0f;	// motion-adaptive history so shadows stay crisp under camera movement
					d.layerReset[0][0] = before.valid ? 0.0f : 1.0f;
					const int denoiseImages[5] = { imageDepth, imageNormals, moved ? imageSunMotion : imageNormals, imageSunVis, imageSunHit };
					if (RtCore::AddPass(shaderSunDenoise, denoiseImages, 5, 0x8u, &d, sizeof(d), 1, 1)) {
						sunDenoiseView = now;
						sunDenoiseView.valid = true;
						if (sunDenoiseFrames++ == 0)
							Logger::Log("UNOFFICIAL RT path trace: PTDenoiseSplitSun active (%ux%u, SIGMA sun shadow denoiser + actor anti-lag, ObjectMotion %s)", tw, th, moved ? "yes" : "no");
					} else {
						sunDenoiseView.valid = false;
					}
				} else {
					sunDenoiseView.valid = false;
				}
				if (sunOk) {
					auto makeSun = [&](IDirect3DTexture9*& t, IDirect3DSurface9*& s, D3DFORMAT format) {
						if (t) { D3DSURFACE_DESC d = {}; t->GetLevelDesc(0, &d); if (d.Width != w || d.Height != h) { DxvkCompute::Drop(s); DxvkCompute::Drop(t); } }
						if (!t && SUCCEEDED(TheRenderManager->device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t, nullptr)) && t) t->GetSurfaceLevel(0, &s);
						return s != nullptr;
					};
					sunSplitThisFrame = makeSun(sunVisTexture, sunVisSurface, D3DFMT_R16F) &&
						makeSun(sunDirectTexture, sunDirectSurface, D3DFMT_A16B16G16R16F) &&
						RtCore::CopyOut(imageSunVis, sunVisSurface, tw, th) &&
						RtCore::CopyOut(imageSunDirect, sunDirectSurface, tw, th);
				} else {
					sunSplitThisFrame = false;
				}
			} else {
				sunDenoiseView.valid = false;
			}
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
		if (earlyRecording) {	// UNOFFICIAL 2026-11-11 PTEarlyLook: what Show may merge this frame (the copies run at the collect, before Show)
			const UInt32 now = TheFrameRateManager->FrameNumber;
			earlyLookFrame = albedoThisFrame ? now : ~0u;
			// (flag 1024: the lamp image, and with it the texel, went in; else the trace never read it and wrote 1 as before)
			earlyDepthFrame = albedoThisFrame && earlyDepth && (flags & 1024u) && c.forward[3] > 0.0f ? now : ~0u;
			earlyTolerance = c.forward[3] > 0.0f ? (8.0f + slack.strength / 0.35f) / c.forward[3] : 0.0f;
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

	// the core user (Register); (2026-11-10 early kick) one object, so SubmitBeforeWorld hands RtCore::SubmitEarly the same user
	RtCore::User ptUser = { kUserName, Wants, Setup, Record, 0, 1 };	// Order 1: set up last (the first left out when the images do not fit)

	ID3D9NvrExtPipeline* ShowPipeline() {
		if (showTried) return showPipeline;
		showTried = true;
		std::vector<char> spirv;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::LoadSpirv("RtPathTraceShow", spirv)) return nullptr;
		NVR_EXT_BINDING_DESC bindings[13] = {
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
			{ 1, 10, NVR_EXT_BIND_SAMPLED_IMAGE, 10, NVR_EXT_VIEW_2D, FALSE, 0 },	// (2026-11-20 PTDenoiseSplitSun) denoised sun visibility
			{ 1, 11, NVR_EXT_BIND_SAMPLED_IMAGE, 11, NVR_EXT_VIEW_2D, FALSE, 0 },	// (2026-11-20 PTDenoiseSplitSun) unshadowed demodulated diffuse sun
			{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 4 },
		};
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data();
		desc.FragmentSpirvSize = (UINT)spirv.size();
		desc.BindingCount = 13;
		desc.pBindings = bindings;
		desc.PushConstantSize = 52;	// RtPathTraceShow.frag's Push block (2026-11-10: + detailEdge; at 48 the draw with the 52-byte block was dropped: the game's own picture stayed)
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

	// UNOFFICIAL 2026-11-11 PTEarlyLook (core/RtEarlyLook.h; docs/next/PT-EARLY-KICK-STEP2-2026-11-11.md): in a frame the early kick submitted,
	// at the top of Show (before the denoiser), the merged first surface from the trace's colour and this frame's raster buffers; every reader
	// of the trace's colour (the show pass, the denoiser's guide, PTBlendedLayer, DLSS RR's guides) then gets it through FirstSurface(). Not
	// run (the trace's own image, as before) with the key 0, outside early frames, or when an input is missing (said once).
	void EarlyLook(IDirect3DDevice9* Device) {
		const UInt32 now = TheFrameRateManager->FrameNumber;
		if (S.earlyLook <= 0 || earlyFrame != now || !S.denoise || S.debugView != 0) return;
		auto constant = [](const char* name) { auto f = TheShaderManager->ConstantsTable.find(name); return f == TheShaderManager->ConstantsTable.end() ? nullptr : f->second; };
		auto texture = [](const char* name) -> IDirect3DBaseTexture9* { auto f = TheTextureManager->TextureNames.find(name); return f == TheTextureManager->TextureNames.end() || !f->second ? nullptr : *f->second; };
		const D3DXVECTOR4* albedoData = constant("TESR_AlbedoBufferData");
		IDirect3DBaseTexture9* depth = texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* gameAlbedo = texture("TESR_AlbedoBuffer");
		const char* why = nullptr;
		if (earlyLookFrame != now || !albedoThisFrame || !albedoTexture || !displayTexture) why = "the trace's first-surface colour was not copied out this frame";
		else if (!depth) why = "no TESR_DepthBuffer";
		else if (!gameAlbedo || !albedoData || albedoData->x < 0.5f) why = "TESR_AlbedoBuffer does not hold this frame's ([Main.Main.Quality] AlbedoBuffer, captured while a reader asks for it)";
		else if (RtEarlyLook::Failed) why = "its shader or target failed (logged)";
		const bool slice2 = S.earlyLook >= 2 && earlyDepthFrame == now;
		if (!why) {
			static GpuTimer timer("  RT early look");
			GpuProfileScope gpu(timer, Device);
			if (RtEarlyLook::Merge(Device, albedoTexture, depth, gameAlbedo, displayTexture, slice2, earlyTolerance)) mergedFrame = now;
			else why = "its pass could not run";
		}
		if (mergedFrame == now) {
			RtEarlyLook::MergedFrames++;
			if (slice2) RtEarlyLook::DepthFrames++;
			static bool told = false;
			if (!told) {
				told = true;
				Logger::Log("UNOFFICIAL RT path trace: PTEarlyLook %d engaged in an early frame: slice 1 (the game's own pixel where the live mode shows it: sky, far land, grass, uncaptured skin)%s%s",
					S.earlyLook, slice2 ? " + slice 2 (the game's colour where the traced first hit is its surface, else its pixel)" : "", S.earlyLook >= 3 ? "; the classes painted (magenta the game's pixel, cyan a depth mismatch, green traced light)" : "");
			}
			if (S.earlyLook >= 2 && !slice2) {
				static bool toldNoDepth = false;
				if (!toldNoDepth) {
					toldNoDepth = true;
					Logger::Log("UNOFFICIAL RT path trace: PTEarlyLook %d: slice 2 is off in this frame (%s): slice 1 only", S.earlyLook,
						earlyDepthGI ? "PTRestirGI: its result carries no first-hit depth" : "the trace's depth was not written: no lamp image this frame");
				}
			}
		}
		else {
			RtEarlyLook::SkippedFrames++;
			static bool toldSkip = false;
			if (!toldSkip && why) { toldSkip = true; Logger::Log("UNOFFICIAL RT path trace: PTEarlyLook skipped in an early frame (%s): the traced first surface stays, as with the key 0 (logged once)", why); }
		}
		static int frames = 0;
		if (++frames >= 600) {
			Logger::Log("UNOFFICIAL RT path trace: PTEarlyLook %d in the last %d early frames: merged %u (%u with slice 2), skipped %u",
				S.earlyLook, frames, RtEarlyLook::MergedFrames, RtEarlyLook::DepthFrames, RtEarlyLook::SkippedFrames);
			frames = 0; RtEarlyLook::MergedFrames = RtEarlyLook::DepthFrames = RtEarlyLook::SkippedFrames = 0;
		}
	}

	// The traced picture (denoised with PTDenoise) into Target: over the finished frame (hdr false: clamped, gamma, PTExposure) or into
	// the HDR scene (hdr true, PTReplace: the stored curve x^(1/2.2), not clamped above 1)
	bool showDrew = false;	// (2026-11-10) Show reached its draw this call (Replace: the blended layer goes onto a traced picture only)
	// UNOFFICIAL 2026-11-12 the far hand-over: the game's frame as Show copied it before the replace (this call; nullptr: no copy), and
	// whether this frame's hand-over wants that copy (FarWanted: the keys, outdoors, the denoised live picture)
	IDirect3DTexture9* shownGameFrame = nullptr;
	bool shownKeepViewModel = false;	// Show kept the first-person model's pixels as the game drew them (its keepViewModel)
	bool FarWanted() {
		return S.replace && S.denoise && S.debugView == 0 && TheShaderManager->GameState.isExterior &&
			(S.farBlend || S.gameFog > 0.0f || S.farMatch != 0 || S.farView != 0);	// (PTGameFog alone: FarHandOver draws nothing while the VolumetricFog effect is on)
	}
	void Show(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool hdr) {
		showDrew = false;
		shownGameFrame = nullptr;
		if (!S.enabled || !haveImage || !displayTexture || !RenderTarget || !RtCore::Running()) return;
		if (hdr) EarlyLook(Device);	// UNOFFICIAL 2026-11-11 PTEarlyLook: before the denoiser (its guide reads FirstSurface() too)
		IDirect3DTexture9* shown = displayTexture;
		IDirect3DTexture9* specShown = nullptr;	// (2026-11-07 ptaaa-denoiser) PTDenoiseSplit: the specular light shown (denoised, or raw)
		// (2026-11-07) PTDetail / PTDenoiseGuide: the game's own lit frame, copied before the traced picture replaces it (the scene as stored)
		IDirect3DTexture9* gameFrame = nullptr;
		const bool layerCopies = hdr && S.blendedLayer && ReactiveMask::Opaque && ReactiveMask::CopiesFrame == ReactiveMask::FrameNumber();
		if (layerCopies) {	// (2026-11-10 PTBlendedLayer) the game's frame WITHOUT its blended layer, if it is the scene's size
			D3DSURFACE_DESC rd = {};
			RenderTarget->GetDesc(&rd);
			if (rd.Width == ReactiveMask::Width && rd.Height == ReactiveMask::Height) gameFrame = ReactiveMask::Opaque;
		}
		if (!gameFrame && hdr && (S.detail > 0.0f || S.denoiseGuide > 0.0f || FarWanted())) {	// (2026-11-12: + the far hand-over's game pixel)
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
		if (hdr) shownGameFrame = gameFrame;	// (2026-11-12 the far hand-over, Replace)
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
			settings.Confidence = S.denoiseConfidence;	// UNOFFICIAL 2026-11-15 (rt-denoise-inputs) the diffuse chain only (RtDenoise::Run skips it in the specular mode)
			settings.MeanVariance = S.denoiseMeanVariance;	// (2026-11-07 ptaaa-denoiser) the playground's chain-link wire shadow
			settings.AntiFirefly = S.denoiseAntiFirefly > 0 ? S.denoiseFireflyK + (S.denoiseFireflyK > 0.0f ? 128.0f * (float)S.denoiseFireflySpread : 0.0f) : 0.0f;	// (2026-11-09: + 128 x PTDenoiseFireflySpread, the shaders' packing)
			//	// (2026-11-07 ptaaa-denoiser) the playground's fireflies
			settings.MaxHistory = S.denoiseHistory;
			settings.PeopleHistory = S.denoisePeopleHistory;
			settings.AntiLag = S.denoiseAntiLag;	// (2026-11-07) testers: "a lot of ghosting"
			if (gameFrame && albedoThisFrame && albedoTexture && S.denoiseGuide > 0.0f) {	// (2026-11-07) PTDenoiseGuide
				settings.Guide = gameFrame; settings.GuideAlbedo = FirstSurface(); settings.GuideSigma = S.denoiseGuide;	// (2026-11-11 PTEarlyLook: the merged one in its frames)
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
		ID3D9NvrExtSampler* point = S.debugView == 7 ? DxvkCompute::LinearClamp() : DxvkCompute::PointClamp();
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
		const bool sunSplitShow = modulate && sunSplitThisFrame && sunVisTexture && sunDirectTexture;	// (2026-11-20 PTDenoiseSplitSun)
		// UNOFFICIAL 2026-11-11 PTEarlyLook: the merged first surface (bit 8: its a < -0.5 keeps the game's pixel); else the trace's own, as before
		IDirect3DTexture9* const firstSurface = modulate ? FirstSurface() : nullptr;
		const bool earlyLook = modulate && firstSurface != albedoTexture;
		const bool rrRawShow = S.denoise && S.denoiseUnderRr == 0 && Upscaler::RayReconstructionWantsRaw();
		struct { float exposure; UInt32 samplerIndex; float split, gamma; UInt32 modulate, pad[3]; float detail, detailRadius, texel[2], detailEdge; } push = { S.exposure, 0, S.split,
			S.debugView == 1 || S.debugView == 2 || S.debugView == 4 ? 1.0f : 1.0f / 2.2f, (modulate ? 1u : 0u) | (hdr ? 2u : 0u) | (splitShow ? 4u : 0u) | (earlyLook ? 8u : 0u) | (sunSplitShow ? 16u : 0u) | (rrRawShow ? 32u : 0u), { 0, 0, 0 }, 0.0f, 4.0f, { 0.0f, 0.0f }, S.detailEdge };	// (the last: 2026-11-10 PTDetailEdge)
		static_assert(sizeof(push) == 52, "RtPathTraceShow's push block: keep desc.PushConstantSize (the pipeline, above) the same");
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
		shownKeepViewModel = push.pad[2] != 0u;	// (2026-11-12 the far hand-over leaves those pixels alone too)
		NVR_EXT_RESOURCE_BINDING bindings[13] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = shown;
		bindings[1].Slot = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].pResource = modulate ? firstSurface : shown;
		bindings[2].Slot = 2; bindings[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[2].pResource = displayTexture;	// (the raw trace: sky / frame pixels)
		bindings[3].Slot = 3; bindings[3].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[3].pResource = viewModel ? (IDirect3DBaseTexture9*)viewModel : (IDirect3DBaseTexture9*)displayTexture;
		bindings[4].Slot = 4; bindings[4].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[4].pResource = gameFrame ? (IDirect3DBaseTexture9*)gameFrame : (IDirect3DBaseTexture9*)displayTexture;
		bindings[5].Slot = 5; bindings[5].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[5].pResource = depthBuffer ? depthBuffer : (IDirect3DBaseTexture9*)displayTexture;
		// (2026-11-07 ptaaa-denoiser) the specular light (shown, raw) and its albedo (r, g | b, roughness); stand-ins without the split
		IDirect3DBaseTexture9* const specImages[4] = { splitShow ? specShown : displayTexture, splitShow ? specTexture : displayTexture,
			splitShow ? specAlbedoA : displayTexture, splitShow ? specAlbedoB : displayTexture };
		for (UINT k = 0; k < 4; k++) { bindings[6 + k].Slot = 6 + k; bindings[6 + k].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[6 + k].pResource = specImages[k]; }
		bindings[10].Slot = 10; bindings[10].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[10].pResource = sunSplitShow ? sunVisTexture : displayTexture;
		bindings[11].Slot = 11; bindings[11].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[11].pResource = sunSplitShow ? sunDirectTexture : displayTexture;
		bindings[12].Slot = 0; bindings[12].Type = NVR_EXT_BIND_SAMPLER; bindings[12].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = RenderTarget;
		draw.BindingCount = 13;
		draw.pBindings = bindings;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		showDrew = SUCCEEDED(DxvkCompute::Extension()->Draw(&draw));
		Device->SetRenderTarget(0, current);
		if (current) current->Release();
	}

	// UNOFFICIAL 2026-11-12 the far hand-over (core/RtFarHandOver.h has the reasoning, the shaders and their constants; the RT host's scene
	// `farhandover` runs the same shaders against the CPU). Called by Replace right after Show put the traced picture into the scene:
	// the weather's fog on the traced pixels (PTGameFog), their level eased to the game's toward the loaded cells' edge (PTFarMatch) and
	// the fade into the game's own pixel over the last part of the loaded range (PTFarBlend). Outdoors only, the live denoised picture
	// only (PTReplace + PTDenoise, PTDebugView 0). At a RenderScale below 1 every buffer holds the picture in its top-left render rect: the
	// passes read them texel for texel and place the pixel by the rect's own uv (c3.zw, c7.zw). Every key off: nothing here runs.
	IDirect3DPixelShader9* farPS[4] = {};	// blend, gain, shadow, map
	bool		farFailed = false;
	UInt32		farFrame = ~0u;				// the frame FarHandOver last drew in (MaskShadowBuffer: this frame's band)
	bool		farBand = false;			// ... with a band (or PTFarView 2)
	float		farK[8][4] = {};			// ... its constants
	float		farGain[3] = { 1.0f, 1.0f, 1.0f };	// PTFarMatch: the eased ratio game / traced at the edge, linear light
	float		farRatio[3] = { 1.0f, 1.0f, 1.0f };	// the last measured one
	bool		farGainValid = false;
	int			farSamples = 0;
	DWORD		farMeasuredAt = 0;
	UInt32		farFrames = 0, farMeasures = 0;
	constexpr UINT kFarW = 256, kFarH = 144;	// the measurement's sample grid
	IDirect3DTexture9*	farSmall[3] = {};		IDirect3DSurface9* farSmallSurface[3] = {};	IDirect3DSurface9* farSys[3] = {};	// traced, game, map
	// One full-screen draw of ps into target with s0-s3 and c0-c7, everything it changes put back (as ViewModelPrep).
	// mode 0: no blend, all channels; 1: SRCALPHA / INVSRCALPHA, colour only; 2: DESTCOLOR / SRCCOLOR (x 2 x the output), colour only
	void FarDraw(IDirect3DDevice9* Device, IDirect3DSurface9* target, IDirect3DPixelShader9* shaderPS, IDirect3DBaseTexture9* const textures[4], const float k[8][4], int mode) {
		D3DSURFACE_DESC td = {};
		if (!target || !shaderPS || FAILED(target->GetDesc(&td))) return;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; IDirect3DBaseTexture9* tex[4] = {};
		IDirect3DSurface9* rt[4] = {}; D3DVIEWPORT9 vp = {};
		DWORD fvf = 0; float saved[8][4] = {};
		Device->GetVertexShader(&vs); Device->GetPixelShader(&ps); Device->GetVertexDeclaration(&decl); Device->GetFVF(&fvf);
		Device->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		for (DWORD i = 0; i < 4; i++) Device->GetTexture(i, &tex[i]);
		for (DWORD i = 0; i < 4; i++) Device->GetRenderTarget(i, &rt[i]);
		Device->GetViewport(&vp); Device->GetPixelShaderConstantF(0, &saved[0][0], 8);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE,
			D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE };
		const DWORD want[] = { FALSE, FALSE, (DWORD)(mode != 0 ? TRUE : FALSE), FALSE, FALSE, FALSE, D3DCULL_NONE, mode != 0 ? 0x7u : 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0,
			(DWORD)(mode == 1 ? D3DBLEND_SRCALPHA : mode == 2 ? D3DBLEND_DESTCOLOR : D3DBLEND_ONE), (DWORD)(mode == 1 ? D3DBLEND_INVSRCALPHA : mode == 2 ? D3DBLEND_SRCCOLOR : D3DBLEND_ZERO),
			D3DBLENDOP_ADD, FALSE };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(Device->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) Device->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[4][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD st = 0; st < 4; st++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(Device->GetSamplerState(st, kSS[i], &savedS[st][i]))) savedS[st][i] = wantS[i]; if (savedS[st][i] != wantS[i]) Device->SetSamplerState(st, kSS[i], wantS[i]); }
		for (DWORD i = 1; i < 4; i++) if (rt[i]) Device->SetRenderTarget(i, nullptr);
		Device->SetRenderTarget(0, target);
		const D3DVIEWPORT9 full = { 0, 0, td.Width, td.Height, 0.0f, 1.0f };
		Device->SetViewport(&full);
		Device->SetVertexShader(nullptr);
		Device->SetPixelShader(shaderPS);
		Device->SetPixelShaderConstantF(0, &k[0][0], 8);
		for (DWORD i = 0; i < 4; i++) Device->SetTexture(i, textures[i] ? textures[i] : textures[0]);	// (a stage the pass does not read: any texture that is not the target)
		Device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)td.Width - 0.5f, y1 = (float)td.Height - 0.5f;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		for (DWORD st = 0; st < 4; st++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[st][i] != wantS[i]) Device->SetSamplerState(st, kSS[i], savedS[st][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) Device->SetRenderState(kRS[i], savedRS[i]);
		Device->SetRenderTarget(0, rt[0]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) Device->SetRenderTarget(i, rt[i]);
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		Device->SetViewport(&vp);
		for (DWORD i = 0; i < 4; i++) { Device->SetTexture(i, tex[i]); if (tex[i]) tex[i]->Release(); }
		Device->SetPixelShaderConstantF(0, &saved[0][0], 8);
		if (decl) { Device->SetVertexDeclaration(decl); decl->Release(); } else Device->SetFVF(fvf);
		Device->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		Device->SetVertexShader(vs); if (vs) vs->Release();
		Device->SetPixelShader(ps); if (ps) ps->Release();
	}
	IDirect3DPixelShader9* FarShader(IDirect3DDevice9* Device, int which) {
		static const char* const source[4] = { RtFarHandOver::kBlendPS, RtFarHandOver::kGainPS, RtFarHandOver::kShadowPS, RtFarHandOver::kMapPS };
		static const char* const name[4] = { "RT path trace far blend", "RT path trace far match", "RT path trace far shadows", "RT path trace far map" };
		if (!farPS[which] && !farFailed) farPS[which] = CapturedNormals::FullScreenShader(Device, source[which], name[which], farFailed);
		return farPS[which];
	}
	// PTFarMatch's measurement: the traced picture (the scene right after Show) and the game's frame of the same pixels, sampled on a
	// kFarW x kFarH grid, over the traced pixels within `zone` units inside the loaded square's edge; three small read-backs (a stall of a
	// millisecond or two: every 16th frame only). False: not measured (a target could not be made or read; too few samples).
	bool FarMeasure(IDirect3DDevice9* Device, IDirect3DSurface9* scene, IDirect3DTexture9* gameFrame, IDirect3DBaseTexture9* const textures[4], const float k[8][4], const RtFarHandOver::Frame& frame, float zone) {
		for (int i = 0; i < 3; i++) {
			if (!farSmall[i] && (FAILED(Device->CreateTexture(kFarW, kFarH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &farSmall[i], nullptr)) || !farSmall[i] ||
				FAILED(farSmall[i]->GetSurfaceLevel(0, &farSmallSurface[i])))) return false;
			if (!farSys[i] && FAILED(Device->CreateOffscreenPlainSurface(kFarW, kFarH, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &farSys[i], nullptr))) return false;
		}
		IDirect3DSurface9* gameSurface = nullptr;
		if (FAILED(gameFrame->GetSurfaceLevel(0, &gameSurface)) || !gameSurface) return false;
		// (RenderScale below 1: the picture is the buffers' top-left render rect; the small copies and the map cover that rect only)
		const RECT rect = { 0, 0, (LONG)lastRect[0], (LONG)lastRect[1] };
		const bool copied = lastRect[0] > 0 && lastRect[1] > 0 && SUCCEEDED(Device->StretchRect(scene, &rect, farSmallSurface[0], nullptr, D3DTEXF_POINT)) &&
			SUCCEEDED(Device->StretchRect(gameSurface, &rect, farSmallSurface[1], nullptr, D3DTEXF_POINT));
		gameSurface->Release();
		if (!copied) return false;
		float km[8][4];
		memcpy(km, k, sizeof(km));
		km[7][2] = tracedScale[0]; km[7][3] = tracedScale[1];
		FarDraw(Device, farSmallSurface[2], FarShader(Device, 3), textures, km, 0);
		static std::vector<float> texels[3], samples;
		for (int i = 0; i < 3; i++) {
			D3DLOCKED_RECT r = {};
			if (FAILED(Device->GetRenderTargetData(farSmallSurface[i], farSys[i])) || FAILED(farSys[i]->LockRect(&r, nullptr, D3DLOCK_READONLY))) return false;
			texels[i].resize((size_t)kFarW * kFarH * 4);
			for (UINT y = 0; y < kFarH; y++) D3DXFloat16To32Array(&texels[i][(size_t)y * kFarW * 4], (const D3DXFLOAT16*)((const char*)r.pBits + (size_t)y * r.Pitch), kFarW * 4);
			farSys[i]->UnlockRect();
		}
		samples.clear();
		for (size_t p = 0; p < (size_t)kFarW * kFarH; p++) {
			const float* map = &texels[2][p * 4];
			const float edge = map[2] * 65536.0f;
			if (map[0] < 0.5f || edge < 0.0f || edge > zone) continue;
			const float* t = &texels[0][p * 4]; const float* g = &texels[1][p * 4];
			const float s[7] = { t[0], t[1], t[2], g[0], g[1], g[2], map[1] };
			samples.insert(samples.end(), s, s + 7);
		}
		float ratio[3];
		const bool ok = RtFarHandOver::Ratio(frame, samples.data(), (int)(samples.size() / 7), 48, ratio, &farSamples);
		if (ok) memcpy(farRatio, ratio, sizeof(ratio));
		return ok;
	}
	void FarHandOver(IDirect3DDevice9* Device, IDirect3DSurface9* scene) {
		if (!showDrew || !FarWanted() || farFailed || !albedoThisFrame || !albedoTexture || !shownGameFrame || !scene) return;
		auto constant = [](const char* name) { auto f = TheShaderManager->ConstantsTable.find(name); return f == TheShaderManager->ConstantsTable.end() ? nullptr : f->second; };
		auto texture = [](const char* name) -> IDirect3DBaseTexture9* { auto f = TheTextureManager->TextureNames.find(name); return f == TheTextureManager->TextureNames.end() || !f->second ? nullptr : *f->second; };
		IDirect3DTexture9* first = FirstSurface();
		IDirect3DBaseTexture9* depth = texture("TESR_DepthBuffer");
		const D3DXVECTOR4* cameraData = constant("TESR_CameraData");
		if (!first || !depth || !cameraData || cameraData->y <= 0.0f || !Player || !Player->GetWorldSpace() || !Tes || !Tes->gridCellArray || !Tes->sky) return;
		// one layout for the scene, the game's copy of it, the first surface and the depth: the hand-over reads them texel for texel
		D3DSURFACE_DESC sd = {}, fd = {}, gd = {}, dd = {};
		if (FAILED(scene->GetDesc(&sd)) || FAILED(first->GetLevelDesc(0, &fd)) || FAILED(shownGameFrame->GetLevelDesc(0, &gd)) || depth->GetType() != D3DRTYPE_TEXTURE ||
			FAILED(static_cast<IDirect3DTexture9*>(depth)->GetLevelDesc(0, &dd)) || sd.Width != fd.Width || sd.Height != fd.Height || sd.Width != gd.Width || sd.Height != gd.Height ||
			sd.Width != dd.Width || sd.Height != dd.Height) {
			static bool told = false;
			if (!told) { told = true; Logger::Log("UNOFFICIAL RT path trace: the far hand-over (PTFarBlend / PTGameFog / PTFarMatch) is skipped: the scene (%ux%u), its copy (%ux%u), the traced first surface (%ux%u) and the depth buffer (%ux%u) do not share one size (logged once)", sd.Width, sd.Height, gd.Width, gd.Height, fd.Width, fd.Height, dd.Width, dd.Height); }
			return;
		}
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		if (!(fabsf(proj._11) > 1.0e-6f) || !(fabsf(proj._22) > 1.0e-6f)) return;
		GridCellArray* cells = Tes->gridCellArray;
		const Sky* sky = Tes->sky;
		RtFarHandOver::Frame f;
		// (as Record's pass constants: the view matrix's columns are right, up and forward)
		f.forward[0] = view._13; f.forward[1] = view._23;
		f.right[0] = view._11 / proj._11; f.right[1] = view._21 / proj._11;
		f.up[0] = view._12 / proj._22; f.up[1] = view._22 / proj._22;
		f.farZ = cameraData->y;
		f.keepViewModel = shownKeepViewModel ? (TheRenderManager->DepthConstants.z == 0.0f ? 1.0f : 2.0f) : 0.0f;
		const bool inGrid = RtFarHandOver::LoadedSquare(cells->worldX, cells->worldY, cells->size, camera.x, camera.y, f.square);
		const float range = 0.5f * (f.square[2] - f.square[0]);	// the loaded range: half the square's side
		f.band = S.farBlend ? S.farBlendWidth * range : 0.0f;
		f.frameScale = powf(S.exposure, 1.0f / 2.2f);	// (Show: the game's pixels come back x PTExposure, in linear light)
		f.rectScale[0] = tracedScale[0] > 0.0f ? 1.0f / tracedScale[0] : 1.0f; f.rectScale[1] = tracedScale[1] > 0.0f ? 1.0f / tracedScale[1] : 1.0f;
		// PTGameFog: only while the GAME draws its fog. With the VolumetricFog effect on (the default) the forward shaders are handed a fog
		// property that fogs nothing (Render.cpp ShadowSceneNode__GetFogPropertyEx: the mod's own fog pass hazes the whole picture after
		// this point, traced and raster pixels alike), so there is nothing to match and the key does nothing. With that effect off the
		// world scene's fog property is what every raster pixel was fogged with: its start, end, power and colour (not the weather's
		// near / far, which the engine turns into these).
		const bool gameFogDrawn = !(TheSettingManager->SettingsMain.Main.RenderEffects && TheShaderManager->Effects.VolumetricFog && TheShaderManager->Effects.VolumetricFog->Enabled &&
			!TheShaderManager->GameState.isUnderwater);
		const ShadowSceneNode* worldNode = BSShaderManager::GetShadowSceneNode(0);
		const BSFogProperty* fog = worldNode ? worldNode->fogProperty : nullptr;
		if (gameFogDrawn && fog && S.gameFog > 0.0f) {
			f.fogNear = fog->fStartDistance; f.fogFar = fog->fEndDistance; f.fogPower = fog->fPower; f.fogStrength = S.gameFog;
			f.fogColour[0] = fog->m_kColor.r; f.fogColour[1] = fog->m_kColor.g; f.fogColour[2] = fog->m_kColor.b;
		}
		const bool fogOn = f.fogStrength > 0.0f && f.fogFar - f.fogNear > 1.0f;
		f.view = (float)S.farView;
		f.reach = S.farMatchReach * range;
		IDirect3DTexture9* viewModel = TheTextureManager->DepthTextureViewModel;
		if (!viewModel) f.keepViewModel = 0.0f;
		IDirect3DBaseTexture9* const textures[4] = { first, depth, viewModel ? (IDirect3DBaseTexture9*)viewModel : (IDirect3DBaseTexture9*)first, shownGameFrame };
		float k[8][4] = {};
		RtFarHandOver::Pack(f, k);
		// PTFarMatch: measured every 16th frame over the band (at least 5 % of the range), eased with a 2 s time constant; held while a
		// measurement finds too little traced land at the edge (a view at the sky, a wall in front)
		if (S.farMatch != 0 && S.farView == 0 && (farFrames % 16u) == 0u) {
			static GpuTimer timer("  RT far match measure");
			GpuProfileScope gpu(timer, Device);
			if (FarMeasure(Device, scene, shownGameFrame, textures, k, f, (std::max)(S.farBlendWidth, 0.05f) * range)) {
				const DWORD now = GetTickCount();
				const float dt = farGainValid ? (float)(now - farMeasuredAt) * 0.001f : 1.0e6f;
				const float ease = 1.0f - expf(-(std::max)(dt, 0.0f) / 2.0f);
				for (int c = 0; c < 3; c++) {
					const float target = (std::clamp)(farRatio[c], 1.0f / S.farMatchLimit, S.farMatchLimit);
					farGain[c] += (target - farGain[c]) * ease;
				}
				farGainValid = true; farMeasuredAt = now;
				farMeasures++;
				// a test run's shot list: every measurement; else the first, then one in 64 (about every 17 s at 60 fps)
				if (SettingShots::Running || farMeasures == 1 || (farMeasures % 64u) == 0u)
					Logger::Log("UNOFFICIAL RT path trace: PTFarMatch %d: over %d samples in the outer %.0f units of the traced land the game's own picture is %.3f %.3f %.3f (r g b, linear, median) times the traced one%s; the level gain at the edge is %.3f %.3f %.3f (limit %.2f, from %.0f units inside)%s",
						S.farMatch, farSamples, (std::max)(S.farBlendWidth, 0.05f) * range, farRatio[0], farRatio[1], farRatio[2], fogOn ? " with the game's fog on it" : "", farGain[0], farGain[1], farGain[2], S.farMatchLimit, f.reach, S.farMatch == 2 ? " (measured only: not applied)" : "");
			}
		}
		farFrames++;
		if (S.farMatch != 1) { farGain[0] = farGain[1] = farGain[2] = 1.0f; if (S.farMatch == 0) farGainValid = false; }
		static GpuTimer timer("  RT far hand-over");
		GpuProfileScope gpu(timer, Device);
		if (S.farMatch == 1 && S.farView == 0 && farGainValid && (fabsf(farGain[0] - 1.0f) > 0.004f || fabsf(farGain[1] - 1.0f) > 0.004f || fabsf(farGain[2] - 1.0f) > 0.004f)) {
			for (int c = 0; c < 3; c++) k[6][c] = (std::min)(powf((std::max)(farGain[c], 0.0f), 1.0f / 2.2f), 2.0f);	// the stored curve; the draw doubles an output of 0-1
			FarDraw(Device, scene, FarShader(Device, 1), textures, k, 2);
		}
		if (S.farBlend || fogOn || S.farView != 0) FarDraw(Device, scene, FarShader(Device, 0), textures, k, S.farView == 1 ? 0 : 1);
		memcpy(farK, k, sizeof(k));
		farFrame = TheFrameRateManager->FrameNumber;
		farBand = (S.farBlend && f.band > 0.0f) || S.farView == 2;
		static int toldState = -1;
		const int state = (S.farBlend ? 1 : 0) | (fogOn ? 2 : 0) | (S.farMatch << 2) | (S.farView << 4) | (inGrid ? 64 : 0) | (gameFogDrawn ? 128 : 0) | (S.farBlendShadows ? 256 : 0);
		if (state != toldState) {
			toldState = state;
			Logger::Log("UNOFFICIAL RT path trace: far hand-over: PTFarBlend %s (the last %.0f of %.0f units of loaded land fade into the game's own pixel; PTFarBlendShadows %s), PTGameFog %.2f (%s; the world's fog property: start %.0f, end %.0f, power %.2f, colour %.3f %.3f %.3f; the weather's: near %.0f, far %.0f, power %.2f), PTFarMatch %d, PTFarView %d; the loaded cells: %d x %d round cell %d, %d, the camera %s (edges %.0f / %.0f units west / east, %.0f / %.0f south / north)",
				S.farBlend ? "on" : "off", f.band, range, S.farBlendShadows ? "on" : "off", S.gameFog, !gameFogDrawn ? "not applied: the VolumetricFog effect is on, so the game draws no fog of its own to match" : fogOn ? "applied to the traced pixels" : "off",
				fog ? fog->fStartDistance : 0.0f, fog ? fog->fEndDistance : 0.0f, fog ? fog->fPower : 0.0f, fog ? fog->m_kColor.r : 0.0f, fog ? fog->m_kColor.g : 0.0f, fog ? fog->m_kColor.b : 0.0f,
				sky->fogNearPlane, sky->fogFarPlane, sky->fogPower, S.farMatch, S.farView,
				(int)cells->size, (int)cells->size, (int)cells->worldX, (int)cells->worldY, inGrid ? "inside" : "NOT inside them: a square of that size round the camera instead",
				-f.square[0], f.square[2], -f.square[1], f.square[3]);
		}
	}
}

namespace RtPathTrace {
	bool InternalTools() {
		return SettingShots::Running || SettingShots::Pending || TheSettingManager->GetSettingI("Main.Menu.Developer", "InternalTools") != 0;
	}
	bool StandDownRT() { return Replacing() && TheSettingManager->GetSettingI(kSection, "PTStandDownRT") != 0; }
	bool ReferenceOn() { return InternalTools() && TheSettingManager->GetSettingI(kSection, "ReferencePT") != 0; }

	void Register() {
		shader = RtCore::AddShader("RtPathTrace");
		shaderGrid = RtCore::AddShader("RtPathTraceGrid");	// (2026-11-04) PTLightGrid
		shaderPrimary = RtCore::AddShader("RtPathTracePrimary");	// UNOFFICIAL 2026-11-19 (Phase 3B): 3-pass PT split
		shaderPrimaryGrid = RtCore::AddShader("RtPathTracePrimaryGrid");
		shaderSpatial = RtCore::AddShader("RtPathTraceSpatial");
		shaderBounce = RtCore::AddShader("RtPathTraceBounce");
		shaderBounceGrid = RtCore::AddShader("RtPathTraceBounceGrid");
		shaderSunUnpack = RtCore::AddShader("RtPathTraceSunUnpack");	// UNOFFICIAL 2026-11-20 PTDenoiseSplitSun
		shaderSunDenoise = RtCore::AddShader("RtShadowDenoise");
		shaderCache = RtCore::AddShader("RtPathTraceCache");	// (2026-11-07) PTRadianceCache's resolve pass
		shaderEmissive = RtCore::AddShader("RtEmissiveBuild");	// (2026-11-07 ptaaa-emissive) PTEmissive's light list
		shaderGI = RtCore::AddShader("RtPathTraceGI");		// (2026-11-04 d) PTRestirGI
		shaderReuse = RtCore::AddShader("RtRestirGI");
		RtCore::AddUser(ptUser);	// (ptUser: Order 1, set up last)
	}

	// UNOFFICIAL 2026-11-10 early kick (RtPathTrace.h; docs/next/PT-EARLY-KICK-PLAN-2026-11-10.md). It submits only where the PT's Record
	// would trace this frame at RtCore::Frame anyway: live path tracing (ReferencePT or PTRealtime, PTReplace + PTDenoise), not left out,
	// the helper running, its images and shader made, and the pre-tonemap chain (where Frame and Replace run) will run this frame.
	void SubmitBeforeWorld(IDirect3DDevice9* Device) {
		if (!RtCore::EarlyPtWanted() || !RtCore::Running()) return;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (earlyFrame == frame) return;	// once a frame
		if (!Wants() || !S.replace || !S.denoise) return;	// (Wants reads the settings: PTRealtime's overrides, LeftOut)
		if (imageA < 0 || imageB < 0 || imageDisplay < 0 || shader < 0) return;	// (Record's own first test)
		// ShaderManager::RenderEffectsPreTonemapping's early returns: without the chain nothing would collect the frame or show it
		if (!TheSettingManager->SettingsMain.Main.RenderEffects || !Player || !Player->parentCell || TheShaderManager->GameState.OverlayIsOn) return;
		earlyRecording = true;
		const bool submitted = RtCore::SubmitEarly(Device, &ptUser);
		earlyRecording = false;
		if (!submitted) return;	// refused (grass outdoors, the helper not ready ...): Record runs at RtCore::Frame as before
		earlyFrame = frame;
		static bool told = false;
		if (!told) {
			told = true;
			Logger::Log("UNOFFICIAL RT path trace: early kick on (NVR_RT_EARLY_PT): traced first surface, submitted at world start");
		}
	}

	// Right before Replace: the early frame's results in (RtCore waits for the helper and runs the copies Record noted). Everything Show,
	// Replace, RrGuides and RtBlendedLayer read from the trace (haveImage, albedoThisFrame, splitThisFrame, tracedScale, vmThisFrame) was
	// set by Record when it noted the copies, as in a normal exchange; only the copies themselves wait for this.
	void CollectBeforeShow(IDirect3DDevice9* Device) {
		if (RtCore::EarlyPending()) RtCore::CollectEarly(Device);
	}

	void RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
		if (S.replace) return;	// (2026-11-03) PTReplace: shown in the scene already (Replace)
		Show(Device, RenderTarget, false);
	}

	// (2026-11-03) PTReplace: the traced picture into the game's scene right after the RT core's point (HDR, the stored curve), so
	// NVR's and the game's later passes (tonemap, bloom, grading) treat it as the frame
	void Replace(IDirect3DDevice9* Device) {
		ReactiveMask::WantCopies = S.enabled && S.replace && S.denoise && S.blendedLayer && !RtCore::Gated();	// (2026-11-10 PTBlendedLayer) for the next frame's world (2026-11-15: not while the RT master switches gate this space)
		if (!S.enabled || !S.replace) return;
		IDirect3DSurface9* scene = nullptr;
		if (FAILED(Device->GetRenderTarget(0, &scene)) || !scene) return;
		Show(Device, scene, true);
		FarHandOver(Device, scene);	// UNOFFICIAL 2026-11-12 PTGameFog / PTFarMatch / PTFarBlend: the traced picture's edge at the loaded cells (before the blended layer comes over)
		scene->Release();
		// UNOFFICIAL 2026-11-10 PTBlendedLayer: what the game drew after its opaque list, onto the traced picture (core/RtBlendedLayer.h)
		if (showDrew && S.blendedLayer && S.denoise && haveImage && albedoThisFrame && albedoTexture && RtCore::Running() && S.debugView == 0) {
			IDirect3DTexture9* viewModel = TheTextureManager->DepthTextureViewModel;
			RtBlendedLayer::Apply(Device, FirstSurface(), viewModel, viewModel ? (TheRenderManager->DepthConstants.z == 0.0f ? 1 : 2) : 0);	// (2026-11-11 PTEarlyLook: the merged one in its frames)
			static int frames = 0;
			if (++frames >= 600) {
				Logger::Log("UNOFFICIAL RT path trace: PTBlendedLayer applied in %u of the last %d frames (%u without this frame's copies)", RtBlendedLayer::AppliedFrames, frames, RtBlendedLayer::SkippedFrames);
				frames = 0; RtBlendedLayer::AppliedFrames = RtBlendedLayer::SkippedFrames = 0;
			}
		}
		// UNOFFICIAL 2026-11-11 PTEarlyLook 3: the merged image's pixel classes painted over the scene (core/RtEarlyLook.h Paint)
		if (showDrew && S.earlyLook >= 3 && mergedFrame == TheFrameRateManager->FrameNumber) RtEarlyLook::Paint(Device);
	}

	bool Replacing() { return S.enabled && S.replace && haveImage && RtCore::Running(); }
	// UNOFFICIAL 2026-11-12 PTOwnShadows (docs/next/PT-COMPAT-AUDIT-2026-11-12.md item 3). The path traced picture has its own sun and lamp
	// shadows, but the chain's deferred shadow apply (ShadowsExteriors.fx Shadow, the interior darkening; also folded into the fog composite,
	// the AO pair and AmbientLight) still ran over it: contact-shadow rims twice, with RT sun shadows on the sun's shadows twice as dark and
	// sky-tinted, rooms darkened twice. Every one of those paths reads TESR_PointShadowBuffer (r the sun's term, g the lamps' light), so one
	// small pass after the shadow passes writes "fully lit" (1, 1: both applies' identity) into it where the trace has a first surface
	// (FirstSurface's a >= 0.5). The game's own pixels in the traced picture (sky, far land, grass, leaves, the first-person model the show
	// pass kept) keep their shadows, and the cloud / far-land factor (SunFar) is not touched.
	void MaskShadowBuffer(IDirect3DDevice9* Device, IDirect3DSurface9* shadowSurface) {
		static IDirect3DPixelShader9* ps = nullptr; static bool failed = false, told = false;
		if (!S.ownShadows || !Replacing() || !S.denoise || !albedoThisFrame || !albedoTexture || S.debugView != 0 || !shadowSurface || failed) return;
		IDirect3DTexture9* first = FirstSurface();
		D3DSURFACE_DESC a = {}, b = {};
		if (!first || FAILED(first->GetLevelDesc(0, &a)) || FAILED(shadowSurface->GetDesc(&b)) || a.Width != b.Width || a.Height != b.Height) return;
		if (!ps) {
			ps = CapturedNormals::FullScreenShader(Device,
				"sampler2D A : register(s0); sampler2D M : register(s1); float4 K : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  clip(tex2D(A, uv).a - 0.5);"
				"  float vm = tex2D(M, uv).x;"
				"  if (K.x > 0.5 && (K.x > 1.5 ? vm > 0.01 : vm < 0.9)) clip(-1.0);"
				"  return float4(1.0, 1.0, 1.0, 1.0); }", "RT path trace own shadows", failed);
			if (!ps) return;
		}
		// the first-person model's pixels where the show pass kept the game's own (Show: keepViewModel): they keep the game's shadows too
		IDirect3DTexture9* viewModel = TheTextureManager->DepthTextureViewModel;
		const float keep = viewModel && !vmThisFrame ? (TheRenderManager->DepthConstants.z == 0.0f ? 1.0f : 2.0f) : 0.0f;
		// UNOFFICIAL 2026-11-12 PTFarBlendShadows (core/RtFarHandOver.h kShadowPS; off by default): in PTFarBlend's band the picture is
		// partly the game's own pixel again; with the key on the buffer is "lit" only inside the band's inner edge and keeps the game's
		// shadow by the game pixel's share in the band (and in full on traced pixels outside the loaded square). Off: "lit" on every traced
		// pixel, as before the band existed. Why off: in the bench (a camera high above the loaded land, RT sun shadows on) the game's
		// buffer held a dark shadow over land the path tracer and the forward shaders both lit, and the band let it back in as a dark
		// fringe (the last 150 units 34 % darker). Only in a frame whose hand-over drew a band; else the pass below, as before.
		if (farBand && S.farBlendShadows && farFrame == TheFrameRateManager->FrameNumber && !farFailed) {
			auto found = TheTextureManager->TextureNames.find("TESR_DepthBuffer");
			IDirect3DBaseTexture9* depth = found != TheTextureManager->TextureNames.end() && found->second ? *found->second : nullptr;
			IDirect3DPixelShader9* bandPS = FarShader(Device, 2);
			if (depth && bandPS) {
				IDirect3DBaseTexture9* const textures[4] = { first, depth, viewModel ? (IDirect3DBaseTexture9*)viewModel : (IDirect3DBaseTexture9*)first, nullptr };
				float bk[8][4];
				memcpy(bk, farK, sizeof(bk));
				bk[0][3] = viewModel ? keep : 0.0f;
				bk[7][0] = 0.0f; FarDraw(Device, shadowSurface, bandPS, textures, bk, 0);
				bk[7][0] = 1.0f; FarDraw(Device, shadowSurface, bandPS, textures, bk, 1);
				if (!told) { told = true; Logger::Log("UNOFFICIAL RT path trace: PTOwnShadows on: the game's deferred sun / lamp shadow apply skips the traced pixels (the shadow buffer reads lit there; in PTFarBlend's band it keeps the game's shadow by the game pixel's share)"); }
				return;
			}
		}
		float saved[4] = {}; const float k[4] = { keep, 0.0f, 0.0f, 0.0f };
		Device->GetPixelShaderConstantF(0, saved, 1);
		Device->SetPixelShaderConstantF(0, k, 1);
		CapturedNormals::FullScreenPass(Device, shadowSurface, ps, first, false, D3DBLEND_ONE, D3DBLEND_ZERO, viewModel ? (IDirect3DBaseTexture9*)viewModel : (IDirect3DBaseTexture9*)first);
		Device->SetPixelShaderConstantF(0, saved, 1);
		if (!told) { told = true; Logger::Log("UNOFFICIAL RT path trace: PTOwnShadows on: the game's deferred sun / lamp shadow apply skips the traced pixels (the shadow buffer reads lit there)"); }
	}

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
		*Albedo = FirstSurface();	// (2026-11-11 PTEarlyLook: the merged one in its frames; its negative alpha = the G-buffer's own guide there)
		if (splitThisFrame) { *SpecularRG = specAlbedoA; *SpecularBRough = specAlbedoB; }
		return true;
	}
	IDirect3DTexture9* RrSpecular() {	// UNOFFICIAL 2026-11-15 (rt-denoise-inputs)
		IDirect3DTexture9 *a = nullptr, *rg = nullptr, *b = nullptr;
		return RrGuides(&a, &rg, &b) && splitThisFrame && S.denoiseMirrorSurface > 0.0f ? specTexture : nullptr;
	}

	std::string StatusText() {
		if (!S.enabled) return "RT reference path tracer off ([Main.Main.RayTracing] ReferencePT false)";
		if (imageA < 0 && RtCore::Running()) return setupAsked ? "RT path tracer switched on, but the RT helper started again without its images (left out or failed: see the log): restart the game"
			: "RT path tracer switched on: the RT helper starts again to make its images";
		char t[300];
		sprintf_s(t, "RT reference path tracer on | %s | %ux%u, %u samples a pixel (of PTMaxSamples %d), %u restarts, %d bounces, %d paths a frame, PTDebugView %d",
			RtCore::Running() ? "the core runs" : "the RT core is not running: nothing traced", textureWidth, textureHeight, samples, S.maxSamples, resets,
			S.bounces, S.samples, S.debugView);
		return t;
	}
}
