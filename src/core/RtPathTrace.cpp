// UNOFFICIAL 2026-11-02 the reference path tracer: RtPathTrace.h; the trace shader is Compute\Rt\RtPathTrace.comp (run by the helper),
// the display pass Compute\RtPathTraceShow.frag (a fork pass in the game).

#include "RtLightGrid.h"	// (2026-11-06) PTLightGrid's builder (shared with tools/nvr-rt-host)
#include "RtPathTrace.h"
#include "RtCore.h"
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "RtDenoise.h"
#include "ManyLamps.h"
#include "../effects/PBR/PBR.h"	// (2026-11-04 rtgap) the game's light scale	// (2026-11-03 rtspec) PTGameLamps 4: the game's lamp table
#include "RenderRect.h"	// (2026-11-03) RenderScale < 1: the trace over the frame's top-left rect (docs/next/RT-RENDERSCALE.md)
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
		int		lastSky = 0;			// (2026-11-06) PTLastSky: the last hit's sky light (flag 32768)
		int		lastBounce = 0;		// (2026-11-06 b) PTLastBounce: the last hit's further bounces, estimated from its own direct light (flag 65536)
		int		lampFalloff = 0;	// (2026-11-06 c) PTLampFalloff: 0 the game's (1 - (d/r)^2)^2.2, 1 physical (windowed inverse square, reach x 2; flag 131072)
		float	lampGain = 1.6f;	// (2026-11-06 c) PTLampGain: the physical falloff's lamp brightness (1 = the game's total light per lamp)
		int		bounceHalf = 0;			// (2026-11-06) PTBounceHalf: -1 auto (on under PTRealtime), 0 off, 1 on
		float	denoiseSigma = 1.0f;	// (2026-11-06) PTDenoiseSigma: the a-trous passes' luminance edge stop (RtDenoise ValueSigma)
		float	denoiseClamp = 0.0f;	// (2026-11-06) PTDenoiseClamp
		int		denoiseIterations = 2;	// PTDenoiseIterations / PTDenoiseHistory (2026-11-04 c, test only): PTDenoise's a-trous passes (2: section 30; was 4), history frames
		float	denoiseHistory = 32.0f;
		bool	restirGI = false;		// PTRestirGI (2026-11-04 d): ReSTIR GI on the first bounce (with PTDenoise; RtPathTraceGI.spv + RtRestirGI.spv)
		float	restirMaxM = 20.0f, restirSpatial = 4.0f, restirRadius = 16.0f, restirDepth = 0.03f, restirNormal = 0.9f;	// its test keys
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
	int			shaderGrid = -1;	// (2026-11-04) PTLightGrid's variant (RtPathTraceGrid.spv: RtPathTrace.comp with PT_LIGHT_GRID)
	// UNOFFICIAL 2026-11-04 d PTRestirGI (RT-PATH-TRACING.md section 29): the trace's variant (the first bounce written out), the reuse pass,
	// the result image (the path's other light in, the picture out), the reservoirs (point xy | point z, normal + M | light, W) and the
	// first hits (distance, normal) of this frame and the last (ping-pong), and last frame's camera for the reprojection
	int			shaderGI = -1, shaderReuse = -1, imageGIResult = -1, imageResA[2] = { -1, -1 }, imageResB[2] = { -1, -1 }, imageResC[2] = { -1, -1 }, imageSurf[2] = { -1, -1 };
	UInt32		giFrame = 0;		// ping-pong: this frame writes set giFrame & 1
	bool		giHistory = false;	// last frame ran the reuse at this size (its reservoirs are valid)
	float		giPrevCamera[4][4] = {};	// last frame's forward, right, up (as the pass's), eye
	struct RestirConstants { float size[4], forward[4], right[4], up[4], origin[4], rect[4], prevForward[4], prevRight[4], prevUp[4], prevEye[4], params[4]; };
	int			imageDepth = -1, imageNormals = -1, imageAlbedo = -1, imagePano = -1, imageFrame = -1, imageMaterial = -1, imageAlbedoOut = -1, imageLamps = -1;
	constexpr int kLampsMax = 256;	// (2026-11-03) the lamp image: ShadowCubeMapsMax + TrackedLightsMax (76) fit; (rtspec) the game's table's nearest 256
	constexpr int kGlowMax = 32;	// (2026-11-03 b) PTGlowLights: at most this many glow lights, in the lamp image after the lamps
	constexpr int kLightTexels = 4 * (kLampsMax + kGlowMax);
	// UNOFFICIAL 2026-11-04 PTLightGrid: the lamp image's rows 1.. hold a world grid of the lamps (BuildGrid): per cell one texel (where its
	// list starts, how many), then the lists (two lamp numbers a texel). 32 x 32 x 16 cells at most; kGridRows rows of kLightTexels.
	constexpr int kGridRows = 48;
	constexpr int kGridTexels = kLightTexels * kGridRows;
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
		S.debugView = i("PTDebugView", 0, 9);
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
		S.denoiseIterations = i("PTDenoiseIterations", 0, 5);
		S.denoiseHistory = f("PTDenoiseHistory", 1.0f, 256.0f);
		S.denoiseClamp = f("PTDenoiseClamp", 0.0f, 8.0f);
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
			if (S.bounces == 4) S.bounces = 1;
			if (S.bounceHalf < 0) S.bounceHalf = 1;	// (2026-11-06, section 35) the bounce for half the pixels a frame
			if (S.lastSky < 0) S.lastSky = 1;	// (2026-11-06, section 39) PTLastSky: the last hit's sky light
			if (S.lastBounce < 0) S.lastBounce = 1;	// (2026-11-06, section 44) PTLastBounce: the last hit's further bounces
		}
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
		auto physical = [&](Lamp& l) { if (S.lampFalloff == 1) { l.pos.w *= 2.0f; l.colour *= S.lampGain; l.distance = (std::max)(D3DXVec3Length((const D3DXVECTOR3*)&l.pos) - l.pos.w, 0.0f); } };
		Lamp lamps[ShadowCubeMapsMax + TrackedLightsMax];
		int found = 0;
		const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
		auto add = [&](const D3DXVECTOR4& p, const D3DXVECTOR4& col) {
			if (p.w <= 10.0f || col.w <= 0.0f || found >= ShadowCubeMapsMax + TrackedLightsMax) return;
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
		// (rtspec) PTGameLamps 4: the game's lamp table's active lamps this frame (the deferred pass's lamps), nearest first, kLampsMax at most
		if (S.gameLamps == 4 && ManyLamps::TableFrame + 1 >= ManyLamps::FrameId && !ManyLamps::ActiveLamps.empty()) {
			static std::vector<Lamp> table;
			table.clear();
			for (const ManyLamps::ActiveLamp& a : ManyLamps::ActiveLamps) {
				if (a.reach <= 0.0f) continue;
				const D3DXVECTOR3 rel(a.pos[0] - eye.x, a.pos[1] - eye.y, a.pos[2] - eye.z);
				const float ls = GameLightScale();	// (2026-11-04 rtgap) as the game's lamps: x LIN_SCALE_LIGHT
				table.push_back({ D3DXVECTOR4(rel.x, rel.y, rel.z, a.reach), D3DXVECTOR4(a.colour[0] * ls, a.colour[1] * ls, a.colour[2] * ls, 0.0f),
					(std::max)(D3DXVec3Length(&rel) - a.reach, 0.0f) });
			}
			for (Lamp& l : table) physical(l);
			std::sort(table.begin(), table.end(), [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
			lampCount = (std::min)((int)table.size(), kLampsMax);
			for (int i = 0; i < lampCount; i++) { memcpy(lampData[i].pos, &table[i].pos, 16); memcpy(lampData[i].colour, &table[i].colour, 16); }
			const int first = (std::min)(lampCount, 8);
			for (int i = 0; i < first; i++) { memcpy(c.lampPos[i], &table[i].pos, 16); memcpy(c.lampColor[i], &table[i].colour, 16); }
			return first;
		}
		for (int s = 0; s < ShadowCubeMapsMax; s++) add(sm->BatchLightPosition[s], sm->BatchLightColor[s]);
		for (int t = 0; t < TrackedLightsMax; t++) add(sm->LightPosition[t], sm->LightColor[ShadowCubeMapsScreen + t]);
		for (int i = 0; i < found; i++) physical(lamps[i]);
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
	int Glows() {
		if (!S.glowLights || S.glow <= 0.0f) return 0;
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
		return S.enabled;
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
		imageLamps = RtCore::AddImage("NVR RT path trace lamps", kLightTexels, 1 + kGridRows, NVR_EXT_SHARED_RG32F);
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
		const float key[8] = { (float)S.bounces + 100.0f * (float)S.bounceHalf + 1000.0f * (float)S.lastSky + 10000.0f * (float)S.lastBounce + 100000.0f * (float)S.lampFalloff + 1000000.0f * S.lampGain, S.sky, S.ambient, S.glow, S.clamp + 1000.0f * S.indirectClamp, (float)S.debugView + (S.primary ? 16.0f : 0.0f) + (S.specular ? 32.0f : 0.0f) + (S.glowLights ? 64.0f : 0.0f) + 128.0f * (float)S.gameLamps + (S.lightGrid ? 1024.0f : 0.0f) + 2048.0f * (float)S.bounceAlpha + 8192.0f * floorf(S.bounceDistance), proj._11, proj._22 };	// (2026-11-04: PTLightGrid too, so an A/B starts each mean fresh)
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
		if (S.primary && imageDepth >= 0 && imageNormals >= 0 && imageAlbedo >= 0 && c.forward[3] > 0.0f && albedoData && albedoData->x > 0.5f &&
			gDepth && gNormals && gAlbedo && RtCore::CopyIn(imageDepth, gDepth) && RtCore::CopyIn(imageNormals, gNormals) && RtCore::CopyIn(imageAlbedo, gAlbedo))
			flags |= 8u;
		// (2026-11-02 e) the screen's roughness / metal (CapturedNormals' MaterialBuffer) with the first hit's G-buffer
		const D3DXVECTOR4* materialData = constant("TESR_MaterialBufferData");
		IDirect3DBaseTexture9* gMaterial = texture("TESR_MaterialBuffer");
		if ((flags & 8u) && S.specular && imageMaterial >= 0 && materialData && materialData->x > 0.5f && gMaterial && RtCore::CopyIn(imageMaterial, gMaterial))
			flags |= 256u;
		if (S.specular) flags |= 128u;
		if ((S.gameLamps & 1) || S.gameLamps == 4) flags |= 2048u;	// (2026-11-03 rtspec) PTGameLamps: the game's falloff
		if (S.glowFromFrame) flags |= 8192u;	// (2026-11-05) PTGlowFromFrame
		if (S.bounceHalf > 0 && !S.restirGI) flags |= 16384u;	// (2026-11-06) PTBounceHalf (not with PTRestirGI: it moves the first direct light first)
		if (S.lastSky > 0) flags |= 32768u;	// (2026-11-06) PTLastSky
		if (S.lastBounce > 0) flags |= 65536u;	// (2026-11-06 b) PTLastBounce
		if (S.lampFalloff == 1) flags |= 131072u;	// (2026-11-06 c) PTLampFalloff physical
		if (S.denoise && imageAlbedoOut >= 0) flags |= 512u;	// (2026-11-03) demodulated for the denoiser
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
		const int glowCount = Glows();
		if (imageLamps >= 0 && (lampCount > 8 || glowCount > 0)) {
			if (!lampTexture) TheRenderManager->device->CreateTexture(kLightTexels, 1 + kGridRows, 1, D3DUSAGE_DYNAMIC, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &lampTexture, nullptr);
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
				lampTexture->UnlockRect(0);
				if (RtCore::CopyIn(imageLamps, lampTexture)) { flags |= 1024u | (gridOn ? 4096u : 0u); c.misc[0] = (float)lampCount; c.rect[2] = (float)glowCount; }
			}
		}
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
		const bool gi = S.restirGI && S.denoise && (flags & 512u) && shaderGI >= 0 && shaderReuse >= 0 && imageGIResult >= 0 && imageResA[1] >= 0;
		if (gi) {
			const int images16[16] = { imageA, imageB, imageDisplay, images[3], images[4], images[5], imageDepth, imageNormals, imageAlbedo, imagePano,
				imageFrame, images[11], imageMaterial, imageAlbedoOut, imageLamps, imageGIResult };
			if (!RtCore::AddPass(shaderGI, images16, 16, 0x7u | (1u << 13) | (1u << 15), &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
			const int cur = (int)(giFrame & 1u), prev = cur ^ 1;
			RestirConstants r = {};
			memcpy(r.size, c.size, 16); memcpy(r.forward, c.forward, 16); r.forward[3] = 0.0f; memcpy(r.right, c.right, 16); memcpy(r.up, c.up, 16);
			memcpy(r.origin, c.origin, 16);
			r.rect[0] = c.rect[0]; r.rect[1] = c.rect[1];
			r.rect[2] = (float)((flags & 1u ? 1u : 0u) | (giHistory ? 2u : 0u) | 4u);
			memcpy(r.prevForward, giPrevCamera[0], 16); memcpy(r.prevRight, giPrevCamera[1], 16); memcpy(r.prevUp, giPrevCamera[2], 16);
			for (int k = 0; k < 3; k++) r.prevEye[k] = giPrevCamera[3][k] - c.origin[k];
			r.prevEye[3] = S.restirMaxM;
			r.params[0] = S.restirSpatial; r.params[1] = S.restirRadius; r.params[2] = S.restirDepth; r.params[3] = S.restirNormal;
			const int reuseImages[14] = { imageA, imageB, imageDisplay, imageGIResult, imageAlbedoOut, images[3], imageResA[prev], imageResB[prev], imageResC[prev],
				imageResA[cur], imageResB[cur], imageResC[cur], imageSurf[prev], imageSurf[cur] };
			if (!RtCore::AddPass(shaderReuse, reuseImages, 14, (1u << 3) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 13), &r, sizeof(r), (tw + 7) / 8, (th + 7) / 8)) { giHistory = false; return; }
			memcpy(giPrevCamera[0], c.forward, 16); memcpy(giPrevCamera[1], c.right, 16); memcpy(giPrevCamera[2], c.up, 16); memcpy(giPrevCamera[3], c.origin, 16);
			giFrame++;
			giHistory = true;
			if (!RtCore::CopyOut(imageGIResult, displaySurface, tw, th)) return;
		}
		else {
			giHistory = false;
			if (!RtCore::AddPass(flags & 4096u ? shaderGrid : shader, images, 15, 0x7u | (flags & 512u ? 1u << 13 : 0u), &c, sizeof(c), (tw + 7) / 8, (th + 7) / 8)) return;
			if (!RtCore::CopyOut(imageDisplay, displaySurface, tw, th)) return;
		}
		tracedScale[0] = (float)tw / (float)w; tracedScale[1] = (float)th / (float)h;
		albedoThisFrame = false;
		if (flags & 512u) {	// (2026-11-03) the first surface's colour for the show pass (demodulation)
			if (!albedoTexture) TheTextureManager->InitTexture("NVR_RtPathTraceAlbedo", &albedoTexture, &albedoSurface, w, h, D3DFMT_A16B16G16R16F);
			albedoThisFrame = albedoSurface && RtCore::CopyOut(imageAlbedoOut, albedoSurface, tw, th);
		}
		if (!S.denoise) samples += (UInt32)S.samples;	// (2026-11-03: PTDenoise keeps 0, so leaving it starts a fresh mean; rtpt-8c)
		haveImage = true;
		static UInt32 logged = 0;
		if (samples == (UInt32)S.samples && logged < 8) {
			logged++;
			Logger::Log("UNOFFICIAL RT path trace: started (%ux%u, %d bounces, %d paths a pixel and frame, %s, %d lamps, sun %s, albedo %s, glow %s, alpha test %s, "
				"first hit %s, sky %s, %d glow lights, light grid %s %dx%dx%d cells of %.0fx%.0fx%.0f, %d list entries).", w, h, S.bounces, S.samples, exterior ? "outdoors: sky on misses" : "indoors", (int)c.misc[0], sunUp ? "up" : "down / indoors",
				flags & 2u ? "atlas" : "averages", flags & 4u ? "atlas" : "none", flags & 1u ? "on" : "off", flags & 8u ? "the screen's G-buffer" : "traced",
				flags & 16u ? "the reflections' panorama" : "gradient", (int)c.rect[2], flags & 4096u ? "on" : "off", gridInfo.dims[0], gridInfo.dims[1], gridInfo.dims[2],
				gridInfo.cell[0], gridInfo.cell[1], gridInfo.cell[2], gridInfo.entries);
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
		NVR_EXT_BINDING_DESC bindings[4] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },
			{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 4 },
		};
		NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
		desc.pFragmentSpirv = spirv.data();
		desc.FragmentSpirvSize = (UINT)spirv.size();
		desc.BindingCount = 4;
		desc.pBindings = bindings;
		desc.PushConstantSize = 32;
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
			}
			if (!denoiser) denoiser = RtDenoise::Create("RT path trace");
			RtDenoise::Settings settings;
			settings.Channels = 3;
			settings.IgnoreRr = true;
			settings.MaxHistory = S.denoiseHistory;
			settings.HistoryClamp = S.denoiseClamp;	// (2026-11-06)	// (2026-11-04 c: test keys; 32 frames as before, 2 a-trous passes since section 30, was 4)
			settings.Iterations = S.denoiseIterations;
			settings.ValueSigma = S.denoiseSigma;	// (2026-11-06) PTDenoiseSigma (4 = RtDenoise's default, as before)
			if (denoisedSurface && RtDenoise::Run(denoiser, Device, displayTexture, denoisedSurface, settings)) shown = denoisedTexture;
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
		const bool modulate = shown == denoisedTexture && albedoThisFrame && albedoTexture;	// (2026-11-03) the denoised light x the first surface's colour
		struct { float exposure; UInt32 samplerIndex; float split, gamma; UInt32 modulate, pad[3]; } push = { S.exposure, 0, S.split,
			S.debugView == 1 || S.debugView == 2 || S.debugView == 4 ? 1.0f : 1.0f / 2.2f, (modulate ? 1u : 0u) | (hdr ? 2u : 0u), { 0, 0, 0 } };
		// (2026-11-03) the overlay after a RenderScale upscale reads the rect-layout trace at uv x scale (into the scene: texel for texel)
		const float uvScale[2] = { hdr ? 1.0f : tracedScale[0], hdr ? 1.0f : tracedScale[1] };
		memcpy(push.pad, uvScale, sizeof(uvScale));
		NVR_EXT_RESOURCE_BINDING bindings[4] = {};
		bindings[0].Slot = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].pResource = shown;
		bindings[1].Slot = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].pResource = modulate ? albedoTexture : shown;
		bindings[2].Slot = 2; bindings[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[2].pResource = displayTexture;	// (the raw trace: sky / frame pixels)
		bindings[3].Slot = 0; bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].pResource = point;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = pipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = RenderTarget;
		draw.BindingCount = 4;
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
