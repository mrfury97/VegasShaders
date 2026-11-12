#pragma once
#include "DxvkCompute.h"
#include "RtCore.h"
#include "RenderRect.h"

/*
* UNOFFICIAL 2026-10-28 (NVR DXVK fork, hybrid RT item 2 of 4; docs/next/FORK-RT-AO.md) [Shaders.AmbientOcclusion.RT]: ambient
* occlusion and sky visibility from rays traced against the scene instead of the screen (GTAO / SSAO) and the top-down ortho map
* (Skylighting v2).
* Where the rays run: in the shared RT core's 64-bit helper (NvrRtHelper.exe; docs/next/FORK-RT-CORE.md), because Windows' 32-bit
* Vulkan drivers offer no ray queries to the game. AmbientOcclusionEffect is the core's user "RT ambient occlusion"; at the core's point
* of the frame (RtCore::Frame, before the sky, AO and AmbientLight passes) its RtRecord:
*   1. RtAOInput.frag: per half-resolution texel the view distance and the world normal -> NVR_RTAOInput (G32R32F);
*   2. RtCore CopyIn / AddPass / CopyOut: the helper runs Compute\Rt\RtAOTrace.comp (RayCount AO rays, SkyRays sky rays) and the
*      result comes back as NVR_RTAORaw (A16B16G16R16F: AO visibility, sky visibility, covered, DebugView 3's value);
*   3. NVR's own passes read it: AmbientLightEstimateRT.frag / AmbientOcclusionEstimateRT.frag in the screen estimate's place (the blurs
*      and Accumulate / AmbientOcclusionTemporalRT.frag after them are the denoiser) and AmbientOcclusionSkyRT.frag in SkyVisibilityV2's.
* Off by default. Whenever the core cannot trace (native D3D9, stock DXVK, no helper, a GPU without ray queries such as the GTX 1070)
* Ready() is false and every pass is exactly the old one: the current AO is the fallback and the A/B. Header only.
*/
namespace RtAmbient {
	struct Settings {
		bool	Enabled = false;	// Enabled
		int		RayCount = 2;		// [1-16] AO rays per half-resolution pixel and frame (2026-10-30: 2, rt-ao-6: denoised the same as 4)
		float	Distance = 120.0f;	// [8-1000] AO ray length, world units (about 70 = 1 m)
		bool	Sky = true;			// also the sky visibility (needs Skylighting on, as the ortho version)
		int		SkyRays = 16;		// 8 or 16 (one or two rings round the normal + straight up)
		float	SkyDistance = 800.0f;	// [64-8000] sky ray length (Skylighting's Distance: 800)
		// UNOFFICIAL 2026-11-08 (alsky; tester: Skylighting with RT AO draws hard straight lines on roads, RT AO alone looks right) BUG FIXES,
		// both on by default (a missing key, an older toml: on); RtAOTrace.comp SkyBlocked, AmbientOcclusionSkyRT.frag. SkyInsideFix: a sky
		// ray that meets a back face just after its start began under the traced surface and carries on past it (as the AO rays' 'inside').
		// SkySoftEdges: cover near the end of a sky ray counts less (the AO rays' falloff) and the traced factor opens up toward endFade.
		bool	SkyInsideFix = true;
		// UNOFFICIAL 2026-11-11 (sungi, F9) BUG FIX, on by default (a missing key, an older toml: on): the GI hits' ambient x the game's
		// ambient scale (LIN_SCALE_AMBIENT, TESR_LinearScales.y), as the sun already is by LinearScales.x, under GIGameLamps; AmbientOcclusion.cpp
		bool	GIAmbientUnits = true;
		bool	SkySoftEdges = true;
		// UNOFFICIAL 2026-11-08 (alsky; tester: RT GI strength + AmbientLight DrawDistance 12000 drew a hard line across the ground) BUG FIX,
		// on by default: AmbientLight's traced occlusion and RT GI bounce fade out before endFade (where the trace stops) instead of at
		// AmbientLight's DrawDistance (AmbientLightEstimateRT.frag, AmbientLight.fx.hlsl Estimate).
		bool	RtFarFade = true;
		int		DebugView = 0;		// 1 RT visibility (before strength), 2 RT sky visibility, 3 rays that started inside something / hit distance,
									// 4 / 5 the helper's input (view distance / 2000, normal up), test only, 6 the GI bounce light alone,
									// 7 (2026-10-31) as 6 with every bounce hit lit as if off screen (RadianceCache's whole share shows); 8 (2026-11-09, rtgi-lock) the AO / GI rays' tangent frame (white flat normals, grey tilted, the shade = the tangent's angle)
		// UNOFFICIAL 2026-10-29 (rt-ao-gi) one-bounce diffuse GI: one ray per half-resolution pixel and frame, GIDistance long; at the
		// hit the light it sends back = its albedo (the core's per-instance record) x (the sun with a shadow ray + the ambient + the
		// nearest lamps); into AmbientLight's bounce light (ComputeGI's accumulate and apply), strength GIStrength. AmbientLight only.
		bool	GI = false;
		float	GIDistance = 1500.0f;	// [100-8000] GI ray length, world units
		float	GIStrength = 1.0f;		// [0-20] the bounce light added (x the surface colour), as ComputeGIStrength (UNOFFICIAL 2026-11-09: the cap was 4; outdoors the sun's bounce is a small share of the light, so users asked for room)
		float	GIOffscreen = 1.0f;		// [0-1] (GI v2) the analytic light's scale for bounce hits the screen did not see (2026-11-03 rtgi-cal: 1, was 0.1)
		bool	GIScreenLight = false;	// UNOFFICIAL 2026-11-03 (rtgi-cal) on-screen bounce hits take the game's lit frame (GI v2); false (default): every hit the analytic light
		bool	GIGameLamps = true;		// UNOFFICIAL 2026-11-03 (rtspec) the bounce hits lit by the game's lamp table (its lamps, reach, colour, falloff)
		float	GIClamp = 1.0f;			// [0.05-4] (2026-10-29) one bounce sample's luminance cap (fireflies from lamp glows)
		bool	Denoise = true;			// (2026-10-29) the AO visibility through the shared RT denoiser (RtDenoise.h) before AO / AmbientLight use it
		int		Interleave = 1;
		int		AOAlpha = 1;			// (2026-10-30) alpha-tested meshes for the AO rays: 0 left out, 1 solid (no test), 2 alpha-tested
		int		DenoiseKernel = 3;		// (2026-10-30) RtDenoise's a-trous kernel for both denoisers: 3 (9 taps) or 5 (25 taps)
		int		GIInterleave = 1;		// 1, 2 or 4 (2026-10-29): the GI rays in every / every 2nd / every 4th 8 x 8 tile a frame			// 1, 2 or 4 (2026-10-29): trace every, every 2nd or every 4th texel a frame (cost / 1, 2, 4)
		float	GIStrengthNight = -1.0f, GISkyNight = -1.0f;	// UNOFFICIAL 2026-11-10 (tester request): outdoors at night, GIStrength / GISky use these instead (blended over dusk and dawn); below 0 = the same as by day
		float	GISky = 0.0f;			// [0-1] UNOFFICIAL 2026-11-10 (gisky) outdoors, a bounce ray that misses brings back the ambient (sky) light x this; 0 = misses bring nothing (as before)
		float	GIAmbient = 0.0f;		// [0-1] the ambient's share in what lights a hit (0: sun and lamps only; the game's ambient is already its indirect light)
		float	GlowGI = 0.0f;			// [0-4] (2026-10-30) glow-mapped surfaces (neon, lit windows, screens) light the bounce: their colour x this; 0 = off
		float	GlowDirect = 0.0f;		// [0-8] (2026-10-30) glow-mapped instances as sphere lights on the bounce light (one picked per texel, a
										// shadow ray): neon pools on floors and walls; 0 = off. Needs GIReuse > 0 (its passes carry the list)
		int		GISpatial = 0;			// [0-4] (2026-10-30) ReSTIR GI's spatial step: neighbours' reservoirs merged (GIReuse > 0)
		int		GIReuse = 8;			// [0-32] (2026-10-30) ReSTIR GI temporal reuse: the GI in its own pass (RtGITrace) keeping a reservoir per
										// texel, at most this many frames of samples behind it; 0 = the bounce inside the AO pass as before
		// UNOFFICIAL 2026-10-31 (rt-ao-sky, docs/next/FORK-RT-GAPS.md section 4) multi-bounce GI: a world-space radiance cache (32 x 32
		// x 16 cells round the camera, fixed in the world) that its own pass (RtRadianceCache) keeps lit by tracing from each cell, its hits
		// lit by the direct light + the cache itself (a bounce more each frame); the GI bounce's off-screen hits add its irradiance. Needs
		// GI and GIReuse > 0 (the reuse passes carry it).
		bool	RadianceCache = false;
		float	CacheStrength = 1.0f;	// [0-2] the cache's light at a bounce hit
		float	CacheCellSize = 128.0f;	// [32-512] world units a cell (the grid: 32 x 32 x 16 of them)
		int		CacheRays = 8;			// [1-32] rays a cell when it is updated
		int		CacheFrames = 16;		// [1-32] the frames of samples a cell's light averages at most (the lag when the light changes)
		// (2026-10-31 b) the cost (rt-rc-1: 3.1 ms Lucky 38 / 7.8 ms Goodsprings a frame with 8 rays and a quarter of the cells): interiors
		// only unless CacheExteriors; 1 in CacheSpread cells a frame (1, 2, 4, 8, 16 or 32); outdoors its own rays and spread; settled
		// cells far from every surface or inside something traced 1 visit in 8 (RtAOTrace.comp).
		bool	CacheExteriors = false;
		int		CacheSpread = 8;
		int		CacheRaysExterior = 4;
		int		CacheSpreadExterior = 8;
		// UNOFFICIAL 2026-11-12 (sungi p3, SunGI-study STUDY.md F6 / F8 / F18) radiance cache v2, each behind its own key; missing key = the default.
		// CacheVisibility (default true; only while RadianceCache runs): every cell also keeps its rays' mean distance and mean squared distance
		// toward 6 axis bins and a lookup weighs each of the 8 cells by DDGI's Chebyshev visibility (no light through walls thinner than a cell).
		// CacheCascades (1-2, default 2; 1 = exactly the old single level): a second grid, the same 32 x 32 x 16 at 4 x CacheCellSize, in the same
		// image, updated by its own pass 1 in 2 x its spread cells a frame; lookups use the fine grid inside it (a fade over its outer 2 cells)
		// and the coarse one beyond, so bounce hits far from the camera still find light. SkyMissSH (default true; only where misses bring sky
		// light: GISky > 0 or AmbientReplace): a ray that finds nothing brings the sky's radiance in ITS direction (TESR_SkyIrradianceObjects'
		// harmonics, un-convolved, x the ambient scale), in the GI passes and in the cache's rays, fading out below the horizon.
		bool	CacheVisibility = true;
		int		CacheCascades = 2;
		bool	SkyMissSH = true;
		// UNOFFICIAL 2026-10-31 (rt-ao-sky, docs/next/RT-DENOISE-BACKBONE.md "RT GI") with [Main.Main.RayTracing] DenoiseBackbone: the GI
		// denoiser's history on pixels that move themselves (B1's object motion > 0.25 units a frame: a walking person) at most this many
		// frames (their bounce light changes as they move; 16 frames there trail behind them); 0 = no cap
		int		GIMovingHistory = 4;
		// UNOFFICIAL 2026-11-08 (rtgi-ghost; tester: dark streaks fanning out over the sand behind a running player, ghosting when turning,
		// GIReuse 32) BUG FIXES, on by default (a missing key, an older toml: on); RtAOTrace.comp ReuseGI. GIReuseSpread: a GI sample
		// copied from last frame onto a surface now seen larger (the camera came closer) keeps only the share of its weight that one copy
		// covers, so the copies a running player makes do not each hold it for GIReuse frames. GIReuseShuffle: last frame's sample from
		// a texel of the reprojected one's 2 x 2 block, shuffled each frame (one-to-one), so neighbouring copies part ways: noise the
		// denoiser smooths instead of streaks along the motion.
		bool	GIReuseSpread = true;
		bool	GIReuseShuffle = true;
		// UNOFFICIAL 2026-11-08 (rtgi-ghost) the GI denoiser's anti-lag (RtDenoise Settings.AntiLag, as PTDenoiseAntiLag): beside its 16
		// frames a fast history of 4; where the long one leaves the fast one's window by this many standard errors the bounce light really
		// changed and the history restarts at 4 frames. 0 = off (as before).
		float	GIDenoiseAntiLag = 3.0f;
		// UNOFFICIAL 2026-11-09 (rtgi-lock; tester: a screen-fixed 45-degree step in the GI that pulses at 8 Hz) BUG FIX, on by default (a missing key, an
		// older toml: on); RtAOTrace.comp SampleU. The AO and GI rays' two sample numbers were locked together (one spiral of directions for the whole
		// screen, turning 0.1368 of a turn a frame: the bounce light pulsed every 7.3 frames, half a period apart where the normals' tangent frame
		// differs). Off: the old pair, for the A/B.
		bool	SampleLockFix = true;
		// UNOFFICIAL 2026-11-09 (rt-review) on by default (a missing key: on); RtAOInput.frag. A normals texel that is not a unit normal (an
		// undrawn or cleared buffer) gets no rays instead of a made-up normal that flips on the view plane X + Y + Z = 0. Off: as before.
		bool	NormalGuard = true;
		// UNOFFICIAL 2026-11-10 LegacyTilt (RtAOInput.frag; 0 = off): the rays' normal bent toward the one direction the blank-normals bug
		// gave every pixel: its much darker AO / sky light / bounce as a look, without the wedge.
		float	LegacyTilt = 0.0f;
		// UNOFFICIAL 2026-11-12 (sungi p4, SunGI-study STUDY.md F3 / F7) SunGI: [Shaders.AmbientLight.Main] SunGI, the one switch for the exterior sun
		// bounce (missing key: off). UpdateSettings reads it here (in memory only, never the toml) and, while the player is outdoors (not InteriorNow), forces
		// Enabled / GI / Sky / RadianceCache / CacheExteriors on: the user's strength / quality keys stay. AmbientLight forces its own side (ExteriorBalance,
		// AmbientReplace, AlbedoBuffer, the Status switch, the grid fallback; AmbientLight.cpp SunGIFrame). GIDenoiseSunGI (default true; missing key: true):
		// with SunGI the GI denoiser gets the sun-GI settings (AmbientOcclusion.cpp RtDenoisedGI); false = the old settings, for the A/B.
		// UNOFFICIAL 2026-11-13 (rtgi-review, RTGIReview/REVIEW.md R2 / R9 / R10 / R4), each on by default (a missing key: the default), read every frame:
		// GICutByEnergy (AmbientOcclusion.cpp, the GI reuse's light keys), GIDenoiseTune (0-2; RtDenoisedGI, with SunGI outdoors), InputSamePixel (RtAOInput.frag).
		bool	GICutByEnergy = true;
		int		GIDenoiseTune = 2;
		bool	InputSamePixel = true;
		// UNOFFICIAL 2026-11-13 (rtgi-review B3, REVIEW.md R3 / R18) GISamplePoints (default false until benched; a missing key: false): ReSTIR GI with the
		// sample's geometry (RtAOTrace.comp ReuseGIPoints): cosine at the receiving surface, Jacobian and one visibility ray on spatial reuse. GIValidate
		// (default true, with GISamplePoints): 1 texel in 8 a frame re-traces the ray to its kept sample point and drops the history when the geometry changed.
		bool	GISamplePoints = false;
		bool	GIValidate = true;
		int		GISpatialVisibility = 1;	// (B3b) with GISamplePoints: 1 = one visibility ray for a winner taken from a neighbour, 2 = one for every neighbour before it is merged
		bool	GIDenoiseGuideOnce = true;	// UNOFFICIAL 2026-11-13 (rtgi-review B2) the GI denoiser's guide worked out once a texel (RtDenoise Settings.GuideOnce); false = per tap in every pass, as before
		bool	GIHitMips = true;	// UNOFFICIAL 2026-11-13 (rtgi-review R19) the bounce hits' textures at the mip of the ray's footprint (0.5 x its length); false = mip 0 as before
		int		GICandidates = 1;	// UNOFFICIAL 2026-11-13 (rtgi-review B1) [1-2] new bounce candidates a texel and frame (GIReuse > 0; RtAOTrace.comp ReuseGI, reuse flag 8)
		bool	SunGI = false;
		bool	GIDenoiseSunGI = true;
	};
	inline Settings Current;
	inline bool InteriorNow = false;	// UNOFFICIAL 2026-11-10: set by AmbientOcclusion's RT pass each frame; UpdateSettings applies the <Key>Interior values while true

	inline void UpdateSettings() {
		const char* s = "Shaders.AmbientOcclusion.RT";
		Current.Enabled = TheSettingManager->GetSettingI(s, "Enabled") != 0;
		Current.RayCount = std::clamp(TheSettingManager->GetSettingI(s, "RayCount"), 1, 16);
		Current.Distance = std::clamp(TheSettingManager->GetSettingF(s, "Distance"), 8.0f, 1000.0f);
		Current.Sky = TheSettingManager->GetSettingI(s, "Sky") != 0;
		Current.SkyRays = TheSettingManager->GetSettingI(s, "SkyRays") <= 8 ? 8 : 16;
		Current.SkyDistance = std::clamp(TheSettingManager->GetSettingF(s, "SkyDistance"), 64.0f, 8000.0f);
		{	// UNOFFICIAL 2026-11-08 (alsky): on unless the toml says false
			SettingManager::Configuration::ConfigNode node;
			Current.SkyInsideFix = !TheSettingManager->Config.FillNode(&node, s, "SkyInsideFix") || node.BoolValue;
			Current.GIAmbientUnits = !TheSettingManager->Config.FillNode(&node, s, "GIAmbientUnits") || node.BoolValue;	// UNOFFICIAL 2026-11-11 (sungi, F9)
			Current.SkySoftEdges = !TheSettingManager->Config.FillNode(&node, s, "SkySoftEdges") || node.BoolValue;
			Current.RtFarFade = !TheSettingManager->Config.FillNode(&node, s, "RtFarFade") || node.BoolValue;
			// UNOFFICIAL 2026-11-12 (sungi p3): a missing key = the default (visibility on, 2 cascades, directional sky on)
			Current.CacheVisibility = !TheSettingManager->Config.FillNode(&node, s, "CacheVisibility") || node.BoolValue;
			Current.CacheCascades = TheSettingManager->Config.FillNode(&node, s, "CacheCascades") ? std::clamp((int)node.IntValue, 1, 2) : 2;
			Current.SkyMissSH = !TheSettingManager->Config.FillNode(&node, s, "SkyMissSH") || node.BoolValue;
		}
		Current.DebugView = std::clamp(TheSettingManager->GetSettingI(s, "DebugView"), 0, 8);
		Current.GI = TheSettingManager->GetSettingI(s, "GI") != 0;
		Current.GIDistance = std::clamp(TheSettingManager->GetSettingF(s, "GIDistance"), 100.0f, 8000.0f);
		Current.GIStrength = std::clamp(TheSettingManager->GetSettingF(s, "GIStrength"), 0.0f, 20.0f);	// UNOFFICIAL 2026-11-09: 20 (was 4); the menu clamps to the same [0-20] its description gives
		Current.GIAmbient = std::clamp(TheSettingManager->GetSettingF(s, "GIAmbient"), 0.0f, 1.0f);
		Current.GISky = std::clamp(TheSettingManager->GetSettingF(s, "GISky"), 0.0f, 1.0f);	// (0-1: 4 x GIStrength 4 blew the ground out white)	// UNOFFICIAL 2026-11-10 (gisky)
		Current.GIOffscreen = std::clamp(TheSettingManager->GetSettingF(s, "GIOffscreen"), 0.0f, 1.0f);
		Current.GIScreenLight = TheSettingManager->GetSettingI(s, "GIScreenLight") != 0;	// UNOFFICIAL 2026-11-03 (rtgi-cal)
		Current.GIGameLamps = TheSettingManager->GetSettingI(s, "GIGameLamps") != 0;	// UNOFFICIAL 2026-11-03 (rtspec)
		Current.GIClamp = std::clamp(TheSettingManager->GetSettingF(s, "GIClamp"), 0.05f, 4.0f);
		{ const int i = TheSettingManager->GetSettingI(s, "Interleave"); Current.Interleave = i >= 4 ? 4 : i >= 2 ? 2 : 1; }
		Current.Denoise = TheSettingManager->GetSettingI(s, "Denoise") != 0;
		{ const int i = TheSettingManager->GetSettingI(s, "GIInterleave"); Current.GIInterleave = i >= 4 ? 4 : i >= 2 ? 2 : 1; }
		{ const int i = TheSettingManager->GetSettingI(s, "AOAlpha"); Current.AOAlpha = i <= 0 ? 0 : i >= 2 ? 2 : 1; }
		Current.DenoiseKernel = TheSettingManager->GetSettingI(s, "DenoiseKernel") >= 5 ? 5 : 3;
		Current.GIReuse = std::clamp(TheSettingManager->GetSettingI(s, "GIReuse"), 0, 32);
		Current.GISpatial = std::clamp(TheSettingManager->GetSettingI(s, "GISpatial"), 0, 4);
		Current.GlowGI = std::clamp(TheSettingManager->GetSettingF(s, "GlowGI"), 0.0f, 4.0f);
		Current.GlowDirect = std::clamp(TheSettingManager->GetSettingF(s, "GlowDirect"), 0.0f, 8.0f);
		Current.RadianceCache = TheSettingManager->GetSettingI(s, "RadianceCache") != 0;
		Current.CacheStrength = std::clamp(TheSettingManager->GetSettingF(s, "CacheStrength"), 0.0f, 2.0f);
		Current.CacheCellSize = std::clamp(TheSettingManager->GetSettingF(s, "CacheCellSize"), 32.0f, 512.0f);
		Current.CacheRays = std::clamp(TheSettingManager->GetSettingI(s, "CacheRays"), 1, 32);
		Current.CacheFrames = std::clamp(TheSettingManager->GetSettingI(s, "CacheFrames"), 1, 32);
		auto pow2 = [](int v) { int p = 1; while (p * 2 <= v && p < 32) p *= 2; return p; };	// (the cells split into whole 64-thread groups)
		Current.CacheExteriors = TheSettingManager->GetSettingI(s, "CacheExteriors") != 0;
		Current.CacheSpread = pow2(std::clamp(TheSettingManager->GetSettingI(s, "CacheSpread"), 1, 32));
		Current.CacheRaysExterior = std::clamp(TheSettingManager->GetSettingI(s, "CacheRaysExterior"), 1, 32);
		Current.CacheSpreadExterior = pow2(std::clamp(TheSettingManager->GetSettingI(s, "CacheSpreadExterior"), 1, 32));
		Current.GIMovingHistory = std::clamp(TheSettingManager->GetSettingI(s, "GIMovingHistory"), 0, 16);
		{	// UNOFFICIAL 2026-11-08 (rtgi-ghost): on / 3 unless the toml says otherwise
			SettingManager::Configuration::ConfigNode node;
			Current.GIReuseSpread = !TheSettingManager->Config.FillNode(&node, s, "GIReuseSpread") || node.BoolValue;
			Current.GIReuseShuffle = !TheSettingManager->Config.FillNode(&node, s, "GIReuseShuffle") || node.BoolValue;
			Current.GIDenoiseAntiLag = TheSettingManager->Config.FillNode(&node, s, "GIDenoiseAntiLag") ? std::clamp(node.FloatValue, 0.0f, 16.0f) : 3.0f;
			Current.SampleLockFix = !TheSettingManager->Config.FillNode(&node, s, "SampleLockFix") || node.BoolValue;	// UNOFFICIAL 2026-11-09 (rtgi-lock)
			Current.NormalGuard = !TheSettingManager->Config.FillNode(&node, s, "NormalGuard") || node.BoolValue;	// UNOFFICIAL 2026-11-09 (rt-review)
			Current.GIStrengthNight = TheSettingManager->Config.FillNode(&node, s, "GIStrengthNight") ? std::clamp(node.FloatValue, -1.0f, 20.0f) : -1.0f;	// UNOFFICIAL 2026-11-10
			Current.GISkyNight = TheSettingManager->Config.FillNode(&node, s, "GISkyNight") ? std::clamp(node.FloatValue, -1.0f, 1.0f) : -1.0f;
			Current.LegacyTilt = TheSettingManager->Config.FillNode(&node, s, "LegacyTilt") ? std::clamp(node.FloatValue, 0.0f, 1.0f) : 0.0f;	// UNOFFICIAL 2026-11-10
			// UNOFFICIAL 2026-11-10 (tester request: interiors and exteriors balanced apart): indoors, a <Key>Interior value of 0 or more replaces
			// the key's own (below 0 or a missing key: the same value everywhere, as before). AmbientOcclusion's RT pass calls this when the
			// player goes in or out (InteriorNow).
			if (InteriorNow) {
				auto indoors = [&](const char* key, float& value, float lo, float hi) {
					if (TheSettingManager->Config.FillNode(&node, s, key) && node.FloatValue >= 0.0f) value = std::clamp(node.FloatValue, lo, hi);
				};
				indoors("GIStrengthInterior", Current.GIStrength, 0.0f, 20.0f);
				indoors("GIAmbientInterior", Current.GIAmbient, 0.0f, 1.0f);
				indoors("GIClampInterior", Current.GIClamp, 0.05f, 4.0f);
				indoors("GIDistanceInterior", Current.GIDistance, 100.0f, 8000.0f);
				indoors("GlowGIInterior", Current.GlowGI, 0.0f, 4.0f);
				indoors("GlowDirectInterior", Current.GlowDirect, 0.0f, 8.0f);
				indoors("LegacyTiltInterior", Current.LegacyTilt, 0.0f, 1.0f);
			}
		}
		{	// UNOFFICIAL 2026-11-12 (sungi p4) SunGI: the one switch. Outdoors only: indoors every key is the user's own (SunGI changes no interior).
			SettingManager::Configuration::ConfigNode node;
			Current.SunGI = TheSettingManager->Config.FillNode(&node, "Shaders.AmbientLight.Main", "SunGI") && node.BoolValue;
			Current.GIDenoiseSunGI = !TheSettingManager->Config.FillNode(&node, s, "GIDenoiseSunGI") || node.BoolValue;
			// UNOFFICIAL 2026-11-13 (rtgi-review)
			Current.GICutByEnergy = !TheSettingManager->Config.FillNode(&node, s, "GICutByEnergy") || node.BoolValue;
			Current.GIDenoiseTune = TheSettingManager->Config.FillNode(&node, s, "GIDenoiseTune") ? std::clamp((int)node.IntValue, 0, 2) : 2;
			Current.InputSamePixel = !TheSettingManager->Config.FillNode(&node, s, "InputSamePixel") || node.BoolValue;
			Current.GICandidates = TheSettingManager->Config.FillNode(&node, s, "GICandidates") ? std::clamp((int)node.IntValue, 1, 2) : 1;
			Current.GIHitMips = !TheSettingManager->Config.FillNode(&node, s, "GIHitMips") || node.BoolValue;
			Current.GIDenoiseGuideOnce = !TheSettingManager->Config.FillNode(&node, s, "GIDenoiseGuideOnce") || node.BoolValue;
			Current.GISamplePoints = TheSettingManager->Config.FillNode(&node, s, "GISamplePoints") && node.BoolValue;
			Current.GIValidate = !TheSettingManager->Config.FillNode(&node, s, "GIValidate") || node.BoolValue;
			Current.GISpatialVisibility = TheSettingManager->Config.FillNode(&node, s, "GISpatialVisibility") ? std::clamp((int)node.IntValue, 1, 2) : 1;
			if (Current.SunGI && !InteriorNow) Current.Enabled = Current.GI = Current.Sky = Current.RadianceCache = Current.CacheExteriors = true;
			// UNOFFICIAL 2026-11-12 (sungi p6, user screenshot: the GI-only view lit a shaded shack evenly, cream all over, the sunlit floor patch no
			// brighter than the far corners) GIAmbient lights EVERY bounce hit with the game's ambient, with no visibility test: inside a room each
			// wall got the open sky's light. With SunGI the sky already comes in through the ray misses (and the cache, both visibility-tested), so
			// GIAmbient is an unshadowed second copy of it that drowns the sun's bounce: 0 outdoors with SunGI (the key stays for the old chain).
			if (Current.SunGI && !InteriorNow) Current.GIAmbient = 0.0f;
		}
	}

	inline constexpr int GILampsMax = 8;

	// RtAOTrace.comp's uniform block (set 0, binding 1 in the helper), std140.
	struct TraceConstants {
		D3DXMATRIX	viewToWorld;	// NVR's view -> scene directions: rows right, up, forward of the view (row-vector matrix)
		D3DXVECTOR4	projection;		// x 1 / P00, y 1 / P11, zw the trace grid's size
		D3DXVECTOR4	origin;			// xyz the view origin in scene space (RtCore::Eye(), 0), w the frame number (0-1023)
		D3DXVECTOR4	ao;				// x distance, y rays (0 none), z 1 = move the noise each frame
		D3DXVECTOR4	sky;			// x distance, y rays (0 none)
		// UNOFFICIAL 2026-10-29 (rt-ao-gi) one-bounce GI (all linear rgb; 0 rays = no GI rays)
		D3DXVECTOR4	gi;				// x distance, y rays (0 or 1 + GISky outdoors: the shader tests > 0.5), z the lamps given, w GIAmbient
		D3DXVECTOR4	sunDir;			// xyz toward the sun (scene = world axes), w 1 = the sun lights (exteriors, above the horizon)
		D3DXVECTOR4	sunColor;		// rgb the sun's light on a surface facing it
		D3DXVECTOR4	ambient;		// rgb the ambient (sky) light, the upper hemisphere's; w GlowGI (2026-10-30)
		D3DXVECTOR4	lampPos[GILampsMax];	// xyz scene space, w reach
		D3DXVECTOR4	lampColor[GILampsMax];	// rgb x dimmer
		D3DXVECTOR4	gi2;			// (GI v2) x GIOffscreen, y the on-screen depth tolerance (share), z GIClamp, w Interleave
		D3DXVECTOR4	gi3;			// x GIInterleave, y AOAlpha, z 1 = GIReuse in this pass (2026-10-30: full rate, one pass instead of two)
		D3DXVECTOR4	prevRot;		// (gi3.z) last frame's viewToWorld rows: xyz row 0, w row 1 x
		D3DXVECTOR4	reuse;			// (gi3.z) xy row 1 yz, zw the camera's move xy (z: gi3.w); sky.z the M cap + 64 x GISpatial, sky.w det sign
	};
	static_assert(sizeof(TraceConstants) <= 512, "RtCore: 512 bytes of constants a pass");

	// (2026-10-30) RtGITrace.spv's uniform block (RtAOTrace.comp built with RT_GI_PASS), std140: the GI fields of TraceConstants plus last
	// frame's view for the reservoirs' reprojection.
	struct GIConstants {
		D3DXMATRIX	viewToWorld;
		D3DXMATRIX	prevViewToWorld;	// last frame's viewToWorld
		D3DXVECTOR4	projection;
		D3DXVECTOR4	origin;
		D3DXVECTOR4	gi;
		D3DXVECTOR4	sunDir;
		D3DXVECTOR4	sunColor;
		D3DXVECTOR4	ambient;
		D3DXVECTOR4	lampPos[GILampsMax];
		D3DXVECTOR4	lampColor[GILampsMax];
		D3DXVECTOR4	gi2;			// x GIOffscreen, y depth tolerance, z GIClamp, w GIInterleave
		D3DXVECTOR4	reuse;			// xyz this frame's camera minus last frame's, w the M cap (GIReuse; 0 = no history this frame) + 64 x GISpatial
	};
	static_assert(sizeof(GIConstants) <= 512, "RtCore: 512 bytes of constants a pass");

	// (2026-10-31) RtRadianceCache.spv's uniform block (RtAOTrace.comp built with RT_CACHE_PASS), std140. With the cache on, the GI passes
	// take at most 7 lamps and carry cacheA / cacheB in lampPos[7] / lampColor[7] (their constants are full; lampColor[7].w > 0 = on).
	struct CacheConstants {
		D3DXVECTOR4	origin;			// w the frame number
		D3DXVECTOR4	gi;
		D3DXVECTOR4	sunDir;
		D3DXVECTOR4	sunColor;
		D3DXVECTOR4	ambient;
		D3DXVECTOR4	lampPos[GILampsMax];
		D3DXVECTOR4	lampColor[GILampsMax];
		D3DXVECTOR4	gi2;
		D3DXVECTOR4	cacheA;			// xyz the camera's world position minus its cell's corner, w the cell size + 1024 x flags (2026-11-12 sungi p3: 1 CacheVisibility, 2 CacheCascades 2)
		D3DXVECTOR4	cacheB;			// xyz the camera's cell (world cell coordinates), w CacheStrength
		D3DXVECTOR4	cacheC;			// x CacheRays, y the frames of history at most (negative: all cells start over), z this frame's turn, w 1 in this many cells a frame
		D3DXVECTOR4	cacheD;			// (2026-10-31 b) x the visit (frame / cacheC.w, mod 4096), y lazy cells' period, z "far" in cells; w (2026-11-12 sungi p3) the level: 0 fine, 1 coarse
	};
	static_assert(sizeof(CacheConstants) <= 512, "RtCore: 512 bytes of constants a pass");
	inline constexpr UINT CacheCells = 32 * 32 * 16, CacheLazyPeriod = 8;	// RtAOTrace.comp CacheGrid; the image: 512 x 512 since 2026-11-12 (8 texels a cell, 2 levels of 256 rows; RtAOTrace.comp CacheTexel)

	// The RT core (RtCore.h) runs: the helper is ready on a GPU with ray tracing. Off = NVR's own passes.
	inline bool CoreReady() { return RtCore::Running(); }

	// The RT AO / sky passes run this frame (logged once per change of state).
	// (2026-10-31, tester at RenderScale 0.67: the log said "running" and "passes are off" in turn, every frame, 66k lines.) The
	// passes check (DxvkCompute::PassesOn) is false inside a rect frame only, and this is asked both before the rect starts (the
	// constants: the sky's ortho map) and inside it (the AO pass): two answers a frame. Rect frames now count for the whole frame
	// (RenderRect::InUse) and say so: RT AO does not run at RenderScale below 1 (the trace, GI, denoisers read and write whole screens).
	// UNOFFICIAL 2026-11-03 RT AO at RenderScale < 1 (docs/next/RT-RENDERSCALE.md "RT AO / GI plan", phase 1): it does now. The trace
	// covers the frame's top-left rect, and in rect frames the RT estimate runs as the one Vulkan pass inside AmbientOcclusion's (and
	// AmbientLight's) rect-aware D3D9 passes. State 4 is gone (its text stays for the numbering); the GI bounce is not traced in rect
	// frames (phase 2: its consumers are Vulkan passes that are off there). (2026-11-04, phase 2: it is, over the same rect; AmbientLight's
	// D3D9 GI path applies it there; AmbientOcclusion.cpp RtRecord.)
	inline bool PassesWanted() { return TheSettingManager->SettingsMain.Main.NvrDxvkPasses && DxvkCompute::Available(); }
	// UNOFFICIAL 2026-11-12 (sungi p4) the ray-traced path can run at all (the Ready() states 1 and 2 passed): false = SunGI's fallback (the light grid's bounce).
	inline bool SunGIRtRunning() { return PassesWanted() && CoreReady(); }
	inline bool SunGIOutdoors() { return Current.SunGI && !InteriorNow; }
	inline bool Ready() {
		static int reported = -1;
		const int state = !Current.Enabled ? 0 : !PassesWanted() ? 1 : !CoreReady() ? 2 : 3;
		if (state != reported && (state != 0 || reported > 0)) {
			const char* text[] = { "off", "on but the Vegas Shaders DXVK passes are off ([Main.Main.Performance] NvrDxvkPasses): Vegas Shaders' AO runs",
				"on but the RT core is not running (see its log line: no NvrRtHelper, no fork 0028, or a GPU without ray tracing): Vegas Shaders' AO runs",
				"running (ray-traced occlusion in place of the screen estimate)",
				"on but not at RenderScale below 1 (FSR / DLSS with LowResEffects: frames drawn small, not supported yet): Vegas Shaders' AO runs" };
			Logger::Log("UNOFFICIAL RT AO: %s.", text[state]);
		}
		reported = state;
		return state == 3;
	}
	inline bool SkyReady() { return Current.Sky && Ready(); }
}
