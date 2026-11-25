// UNOFFICIAL 2026-11-07 (ptaaa-fog) RT fog visibility: RtFogVisibility.h; the trace shader Compute\Rt\RtFogVisibility.comp (run by the
// helper), its constants and history rules RtFogGrid.h (shared with the RT test host's fogvis scene).

#include "RtFogVisibility.h"
#include "RtCore.h"
#include "RtFogGrid.h"
#include "RtFogLampGrid.h"	// UNOFFICIAL 2026-11-12 LampHazeShadows: the lamps' pass constants and slots
#include "RtDenoise.h"	// UNOFFICIAL 2026-11-07 (perf-history) HistoryClock
#include "RtLampShadows.h"	// UNOFFICIAL 2026-11-13 RidersWithSameFrame: SameFrameTraced

// UNOFFICIAL 2026-11-13 [Main.Main.RayTracing] RidersWithSameFrame (on; BUG FIX; a missing key: on). The two riders of this file trace
// only while another RT effect traces (RtCore::OthersWant: a user whose Wants() is true at the RT point of the frame). The RT lamp
// shadows' SameFrame mode (their default) traces BEFORE the world and then answers Wants() false for the rest of that frame, so with the
// RT lamp shadows as the only other RT effect the riders never traced: LampHazeShadows did nothing in a room with RT lamp shadows on
// (bench 2026-11-13, run 20261113-115304-slotC: "RT fog lamps traced" only once path tracing was switched on). On: a frame whose lamp
// shadows were traced before the world counts as a host too (the core then makes the frame's second exchange for the rider's pass, the
// same scene, as it does for RT ambient occlusion beside same-frame lamp shadows). Off: as before.
namespace {
	bool HostTraces(const char* rider) {
		if (RtCore::OthersWant(rider)) return true;
		if (!RtLampShadows::SameFrameTraced()) return false;
		SettingManager::Configuration::ConfigNode node;
		return !TheSettingManager->Config.FillNode(&node, "Main.Main.RayTracing", "RidersWithSameFrame") || node.BoolValue;
	}
}

namespace {
	const char* const kSection = "Shaders.VolumetricLight.RT";
	const char* const kUserName = "RT fog visibility";

	struct Settings {
		bool	enabled = true;
		RtFogGrid::Settings grid;
		float	fadeLength = 1000.0f;
		int		debugView = 0;		// 1: traced, not used (the cascades: an A/B)
		// UNOFFICIAL 2026-11-13 (rtvol2) Expanded: the look of a current game's traced / froxel volumetric light while the air is traced
		// (RtFogVisibility.h ExpandedLook). Off (the default): the traced visibility only replaces the shadow maps' lookup, as before.
		bool	expanded = false;
		float	expandedDistance = 8000.0f;	// the lit path (planar depth of the grid's far side)
		float	expandedDensity = 0.015f;	// per 1000 units, by day
		float	expandedHeight = 1500.0f;	// the dust's scale height above the eye (0: even air)
		float	expandedGlow = 0.3f;		// the tight forward lobe's share of the scattering
		float	expandedSharpness = 0.85f;	// its g
		bool	expandedClouds = true;		// the volumetric clouds' shadow in the air
	} S;
	// (rtvol2) ExpandedClouds: the clouds' shadow map in the helper (registered at the helper's start when the map exists then), and
	// whether this frame's trace carried it
	int			imageCloud = -1;
	UINT		imageCloudSize = 0;
	bool		cloudsInTrace = false;
	bool		expandedTraced = false;		// the last trace was made with the expanded look's grid and range

	int			shader = -1;
	int			image[2] = { -1, -1 };	// the ping-pong pair: this frame's output, last frame's (the history)
	unsigned	imageColumns = 0, imageRows = 0;
	int			nextOut = 0;
	RtFogGrid::View lastView;
	UInt32		lastTracedFrame = 0xFFFFFFFF;	// the game frame (TheFrameRateManager->FrameNumber) of the last trace
	unsigned	since = 0;					// frames the history has run
	UInt32		frames = 0, noRoom = 0;
	float		tracedDistance = 0.0f, tracedRange = 0.0f;

	void ReadSettings() {
		S.enabled = TheSettingManager->GetSettingI(kSection, "Enabled") != 0;
		S.grid.rays = std::clamp(TheSettingManager->GetSettingI(kSection, "RaysPerCell"), 1, 8);
		S.grid.temporal = std::clamp(TheSettingManager->GetSettingF(kSection, "Temporal"), 0.0f, 0.97f);
		S.grid.temporalMoving = std::clamp(TheSettingManager->GetSettingF(kSection, "TemporalMoving"), 0.0f, 0.97f);
		S.grid.range = (std::max)(500.0f, TheSettingManager->GetSettingF(kSection, "Range"));
		S.grid.rayLength = (std::max)(100.0f, TheSettingManager->GetSettingF(kSection, "RayLength"));
		S.fadeLength = std::clamp(TheSettingManager->GetSettingF(kSection, "FadeLength"), 1.0f, 0.5f * S.grid.range);
		S.debugView = TheSettingManager->GetSettingI(kSection, "DebugView");
		S.grid.debug = 0;
		S.grid.jitter = true;
		// (rtvol2) the expanded look; a missing key: off / the defaults
		SettingManager::Configuration::ConfigNode node;
		auto readF = [&](const char* key, float fallback, float lo, float hi) {
			return TheSettingManager->Config.FillNode(&node, kSection, key) ? std::clamp(TheSettingManager->GetSettingF(kSection, key), lo, hi) : fallback;
		};
		S.expanded = TheSettingManager->Config.FillNode(&node, kSection, "Expanded") && node.BoolValue;
		S.expandedDistance = readF("ExpandedDistance", 8000.0f, 2000.0f, 16000.0f);
		S.expandedDensity = readF("ExpandedDensity", 0.015f, 0.0f, 2.0f);
		S.expandedHeight = readF("ExpandedHeight", 1500.0f, 0.0f, 20000.0f);
		S.expandedGlow = readF("ExpandedSunGlow", 0.3f, 0.0f, 0.9f);
		S.expandedSharpness = readF("ExpandedGlowSharpness", 0.85f, 0.6f, 0.95f);
		S.expandedClouds = !TheSettingManager->Config.FillNode(&node, kSection, "ExpandedClouds") || node.BoolValue;
	}

	// (rtvol2) the expanded look is asked for (it applies once a trace with it exists: ExpandedLook)
	// (2026-11-15 RT master switches) not while RtCore::Gated: the light's grid then reaches its own MaxDistance, as with Enabled false
	bool ExpandedWanted() { return S.enabled && S.expanded && S.debugView != 1 && !RtCore::Gated(); }

	// Outdoors, the volumetric light on, the helper already running for another RT effect
	bool Wants() {
		ReadSettings();
		if (!S.enabled || !RtCore::Running()) return false;
		VolumetricLightEffect* light = TheShaderManager->Effects.VolumetricLight;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (!light || !light->Enabled || !shadows || !TheShaderManager->GameState.isExterior || TheShaderManager->GameState.isUnderwater) return false;
		return HostTraces(kUserName);	// (2026-11-13 RidersWithSameFrame: was RtCore::OthersWant alone)
	}

	void Setup() {
		RtFogGrid::Columns(TheRenderManager->width, TheRenderManager->height, imageColumns, imageRows);
		image[0] = RtCore::AddImage("NVR RT fog visibility 0", imageColumns * 8, imageRows * 8, NVR_EXT_SHARED_R16F);
		image[1] = RtCore::AddImage("NVR RT fog visibility 1", imageColumns * 8, imageRows * 8, NVR_EXT_SHARED_R16F);
		// (rtvol2) ExpandedClouds: the volumetric clouds' shadow map, if it exists at the helper's start (as RtSunShadows' CloudShadows:
		// clouds switched on later count from the next start). 256 x 256 RGBA16F by default: 0.5 MB.
		imageCloud = -1;
		imageCloudSize = 0;
		{
			VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
			D3DSURFACE_DESC desc = {};
			if (clouds && clouds->Textures.ShadowTexture && SUCCEEDED(clouds->Textures.ShadowTexture->GetLevelDesc(0, &desc)) &&
				desc.Format == D3DFMT_A16B16G16R16F && desc.Width == desc.Height && desc.Width == clouds->shadowMapSize) {
				imageCloud = RtCore::AddImage("NVR RT fog cloud shadow", desc.Width, desc.Height, NVR_EXT_SHARED_RGBA16F);
				imageCloudSize = desc.Width;
			}
		}
		lastView.valid = false;
		since = 0;
		lastTracedFrame = 0xFFFFFFFF;
	}

	void Record(IDirect3DDevice9* Device) {
		VolumetricLightEffect* light = TheShaderManager->Effects.VolumetricLight;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (shader < 0 || image[0] < 0 || image[1] < 0 || !light || !shadows || !light->Textures.RtVisibilitySurface) return;
		unsigned columns = 0, rows = 0;
		RtFogGrid::Columns(TheRenderManager->width, TheRenderManager->height, columns, rows);
		// the volumetric light's grid and its copy target must be this grid (they are made from the same screen size)
		if (columns != imageColumns || rows != imageRows || (UINT)light->Constants.GridLayout.x != columns || (UINT)light->Constants.GridLayout.y != rows) return;
		if (RtCore::PassesLeft() < 1) {	// the frame's passes are full (NVR_RT_MAX_PASSES): the cascades this frame
			if ((noRoom++ % 600) == 0) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT fog visibility: no pass left in this frame's RT record (%u frames so far): the cascades this frame.", noRoom);
			return;
		}
		const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const D3DXVECTOR3 eye = RtCore::Eye();	// the scene is relative to the eye
		RtFogGrid::View now;
		now.eye[0] = (double)TheRenderManager->CameraPosition.x + eye.x;
		now.eye[1] = (double)TheRenderManager->CameraPosition.y + eye.y;
		now.eye[2] = (double)TheRenderManager->CameraPosition.z + eye.z;
		// the view matrix's columns: right (0), up (1), forward (2) (Depth.hlsl toWorld)
		now.forward[0] = view._13; now.forward[1] = view._23; now.forward[2] = view._33;
		now.right[0] = view._11; now.right[1] = view._21; now.right[2] = view._31;
		now.up[0] = view._12; now.up[1] = view._22; now.up[2] = view._32;
		now.p00 = proj._11; now.p11 = proj._22;
		now.maxDistance = light->GridDistance();
		now.columns = columns; now.rows = rows;
		const D3DXVECTOR4& sun = shadows->Constants.SmoothedSunDir;	// toward the sun (the moon at night), as the cascades
		now.sun[0] = sun.x; now.sun[1] = sun.y; now.sun[2] = sun.z;
		now.valid = true;
		const bool sunUp = shadows->Constants.ShadowFade.x < 1.0f;

		RtFogGrid::Settings s = S.grid;
		// UNOFFICIAL 2026-11-07 (perf-history) Temporal / TemporalMoving are last frame's weight at 60 fps: the same decay a second here
		const RtHistoryTime::Clock clock = RtDenoise::HistoryClock();
		s.temporal = RtHistoryTime::Keep(s.temporal, clock);
		s.temporalMoving = RtHistoryTime::Keep(s.temporalMoving, clock);
		s.range = (std::min)(s.range, (std::max)(512.0f, TheSettingManager->GetSettingF("Main.Main.RayTracing", "SceneRadius")));
		// (rtvol2) Expanded: every cell of the longer grid is traced (its corners lie up to ~1.35 x its planar depth away), past SceneRadius
		// too: a sun ray from there still meets what the scene holds between it and the sun (the far LOD with FarLandLOD / FarObjectLOD),
		// and what it cannot see counts as lit, as the shadow maps' "past the shadow distance" did.
		const bool expandedNow = ExpandedWanted();
		if (expandedNow) s.range = (std::max)(s.range, now.maxDistance * 1.45f);
		const int atlas = RtCore::AlphaAtlasImage();
		s.alphaTested = RtCore::AlphaTested() && atlas >= 0;
		s.mask = 0xFFu;	// land, objects, alpha-tested, actors, the far LOD: every caster the RT sun shadows see
		if (!(lastTracedFrame != 0xFFFFFFFF && lastTracedFrame + 1 == frameNow && RtFogGrid::Continues(now, lastView))) since = 0;
		const float weight = RtFogGrid::HistoryWeight(since, s.temporal);
		RtFogGrid::PassConstants c;
		RtFogGrid::Fill(c, now, lastView, weight, sunUp, s, frames);

		// (rtvol2) ExpandedClouds: the clouds' shadow map as last drawn (drawn later in the frame: last frame's, with its own centre), as
		// RtSunShadows' CloudShadows takes it. Without it (or the expanded look off) cloud[0] stays 0 and binding 5 is not bound.
		cloudsInTrace = false;
		if (expandedNow && S.expandedClouds && imageCloud >= 0) {
			VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
			if (clouds && clouds->Enabled && clouds->rtMapValid && clouds->Textures.ShadowTexture && clouds->shadowMapSize == imageCloudSize) {
				const float strength = clouds->ShadowStrengthForSunFar();
				if (strength > 0.0f && RtCore::CopyIn(imageCloud, clouds->Textures.ShadowTexture)) {
					c.cloud[0] = strength; c.cloud[1] = clouds->rtMapRange; c.cloud[2] = (float)clouds->shadowMapSize; c.cloud[3] = clouds->rtMapBase;
					c.cloudCentre[0] = clouds->rtMapCentre[0]; c.cloudCentre[1] = clouds->rtMapCentre[1]; c.cloudCentre[2] = 1.0f / 70.0f;
					c.cloudLight[0] = clouds->rtMapLight[0]; c.cloudLight[1] = clouds->rtMapLight[1]; c.cloudLight[2] = clouds->rtMapLight[2];
					c.camera[0] = TheRenderManager->CameraPosition.x; c.camera[1] = TheRenderManager->CameraPosition.y; c.camera[2] = TheRenderManager->CameraPosition.z;
					cloudsInTrace = true;
				}
			}
		}

		const int out = image[nextOut], hist = image[nextOut ^ 1];
		const int images[4] = { out, hist, s.alphaTested ? atlas : hist,	// [2]: binding 4, a stand-in (never read) without alpha-tested casters
			imageCloud };	// [3]: binding 5 (rtvol2), only passed while cloudsInTrace
		if (!RtCore::AddPass(shader, images, cloudsInTrace ? 4 : 3, 0x1, &c, sizeof(c), (columns * 8 + 7) / 8, (rows * 8 + 7) / 8)) return;
		if (!RtCore::CopyOut(out, light->Textures.RtVisibilitySurface, columns * 8, rows * 8)) return;
		lastTracedFrame = frameNow;
		lastView = now;
		since++;
		nextOut ^= 1;
		tracedDistance = now.maxDistance;
		tracedRange = s.range;
		expandedTraced = expandedNow;
		static int expandedLogs = 0;	// (rtvol2) one line when the look changes (at most 8 a session)
		static int lastExpandedState = -1;
		const int expandedState = expandedNow ? (cloudsInTrace ? 2 : 1) : 0;
		if (expandedState != lastExpandedState && expandedLogs < 8) {
			lastExpandedState = expandedState;
			expandedLogs++;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT light shafts, expanded look %s: grid out to %.0f, traced to %.0f, density %.4f / 1000, dust height %.0f, "
				"sun glow %.2f (sharpness %.2f), cloud shadows in the air %s.", expandedNow ? "ON" : "off", now.maxDistance, s.range, S.expandedDensity, S.expandedHeight,
				S.expandedGlow, S.expandedSharpness, cloudsInTrace ? "on" : !S.expandedClouds ? "off (ExpandedClouds)" :
				imageCloud < 0 ? "not available (no volumetric cloud shadow map at the helper's start)" : "not this frame (clouds off or no shadow)");
		}
		if (frames++ == 0 || (frames % 3600) == 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT fog visibility traced (frame %u): grid %u x %u x 64 out to %.0f, %d ray%s a cell (%u rays), Temporal %.2f (weight %.2f), "
				"range %.0f (fade %.0f), alpha-tested casters %s, DebugView %d.", frames, columns, rows, now.maxDistance, s.rays, s.rays > 1 ? "s" : "",
				columns * rows * 64u * (unsigned)s.rays, s.temporal, weight, s.range, S.fadeLength, s.alphaTested ? "tested" : "not in the scene", S.debugView);
	}
}

// UNOFFICIAL 2026-11-12 LampHazeShadows ([Shaders.VolumetricFog.Interiors] LampHazeShadows; RtFogVisibility.h RtFogLamps): the second
// rider, indoors. The fog (VolumetricFogEffect::SetLampHaze) hands over the lamps it lights the air with each frame; the frame after,
// at the RT point, this user traces their visibility along every view (Compute\Rt\RtFogLamps.comp, constants and slots RtFogLampGrid.h)
// and copies it out into its own texture, which the fog binds on stage 11 (free indoors: the volumetric light's grid is outdoors only).
namespace {
	const char* const kLampUser = "RT fog lamps";
	const char* const kLampRtSection = "Shaders.ShadowsInteriors.RT";	// the RT lamp shadows' ray rules (read only), so the air and the walls agree

	struct LampState {
		int			shader = -1;
		int			image[2] = { -1, -1 };	// the ping-pong pair
		int			nextOut = 0;
		unsigned	cell = 8, columns = 0, rows = 0;	// as at the helper's start (Setup)
		RtFogLampGrid::Slots slots;			// the lamps of the texture's tiles (the last trace's)
		RtFogLampGrid::Lamp submitted[RtFogLampGrid::kSlots];	// the fog's lamps of its last frame
		int			submitCount = 0;
		UInt32		submitFrame = 0;
		bool		tracedOnce = false;
		UInt32		tracedFrame = 0;
		unsigned	since = 0;				// frames the history has run
		UInt32		frames = 0, noRoom = 0;
		IDirect3DTexture9* texture = nullptr;	// 4 x 4 tiles of columns x rows, R16F
		IDirect3DSurface9* surface = nullptr;
		unsigned	textureW = 0, textureH = 0;
		double		lastEye[3] = { 0.0, 0.0, 0.0 };
		float		lastForward[3] = { 0.0f, 1.0f, 0.0f };
		bool		lastValid = false;
		int			lastLamps = 0, lastSamples = 0;	// (the status line)
		float		lastWeight = 0.0f;
	} L;

	UInt32 LampFrameNow() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }

	// The texture holds a trace of the last few frames (a frame whose RT record was full keeps the one before)
	// (2026-11-15 RT master switches) not while RtCore::Gated: no older trace is used after the gate shuts (unshadowed haze at once)
	bool LampUsable() { return L.texture && L.tracedOnce && LampFrameNow() - L.tracedFrame <= 8u && !RtCore::Gated(); }

	// Indoors, the fog asked within the last two frames, the helper already running for another RT effect. (Every early return comes
	// before OthersWant: the sun's user above asks the same question outdoors, and each must answer the other's without asking back.)
	bool LampWants() {
		if (L.shader < 0 || L.submitCount <= 0 || !RtCore::Running()) return false;
		if (LampFrameNow() - L.submitFrame > 2u) return false;	// LampHazeShadows or LampHaze off, outdoors, the fog off: nothing asked
		if (TheShaderManager->GameState.isExterior || TheShaderManager->GameState.isUnderwater) return false;
		return HostTraces(kLampUser);	// (2026-11-13 RidersWithSameFrame: was RtCore::OthersWant alone)
	}

	void LampSetup() {
		VolumetricFogEffect* fog = TheShaderManager->Effects.VolumetricFog;
		L.cell = (unsigned)std::clamp(fog ? fog->lampHazeShadowCell : 8, 4, 32);
		RtFogLampGrid::Tile(TheRenderManager->width, TheRenderManager->height, L.cell, L.columns, L.rows);
		L.image[0] = RtCore::AddImage("NVR RT fog lamps 0", L.columns * RtFogLampGrid::kTilesAcross, L.rows * RtFogLampGrid::kTilesAcross, NVR_EXT_SHARED_R16F);
		L.image[1] = RtCore::AddImage("NVR RT fog lamps 1", L.columns * RtFogLampGrid::kTilesAcross, L.rows * RtFogLampGrid::kTilesAcross, NVR_EXT_SHARED_R16F);
		L.slots = RtFogLampGrid::Slots();
		L.tracedOnce = false;
		L.lastValid = false;
		L.since = 0;
	}

	void LampRecord(IDirect3DDevice9* Device) {
		VolumetricFogEffect* fog = TheShaderManager->Effects.VolumetricFog;
		if (L.shader < 0 || L.image[0] < 0 || L.image[1] < 0 || !fog || !Device || L.submitCount <= 0) return;
		unsigned columns = 0, rows = 0;
		RtFogLampGrid::Tile(TheRenderManager->width, TheRenderManager->height, L.cell, columns, rows);
		if (!columns || !rows || columns != L.columns || rows != L.rows) return;	// not the screen the images were made for
		if (RtCore::PassesLeft() < 1) {	// the frame's passes are full (NVR_RT_MAX_PASSES): the fog keeps the last trace (LampUsable)
			if ((L.noRoom++ % 600) == 0) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT fog lamps (LampHazeShadows): no pass left in this frame's RT record (%u frames so far): the last trace is kept.", L.noRoom);
			return;
		}
		const unsigned w = columns * RtFogLampGrid::kTilesAcross, h = rows * RtFogLampGrid::kTilesAcross;
		if (!L.texture || !L.surface || L.textureW != w || L.textureH != h) {
			if (L.surface) { L.surface->Release(); L.surface = nullptr; }
			if (L.texture) { L.texture->Release(); L.texture = nullptr; }
			L.tracedOnce = false;
			if (FAILED(Device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &L.texture, nullptr)) || !L.texture ||
				FAILED(L.texture->GetSurfaceLevel(0, &L.surface)) || !L.surface) {
				if (L.texture) { L.texture->Release(); L.texture = nullptr; }
				L.surface = nullptr;
				static bool failLogged = false;
				if (!failLogged) { failLogged = true; Logger::Log("[ERROR] UNOFFICIAL RT fog lamps (LampHazeShadows): could not create the %u x %u visibility texture: the lamps' light in the fog stays unshadowed.", w, h); }
				return;
			}
			L.textureW = w; L.textureH = h;
		}

		const UInt32 frameNow = LampFrameNow();
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		// the view matrix's columns: right (0), up (1), forward (2) (Depth.hlsl toWorld; as RtFogVisibility's Record above)
		const float forward[3] = { view._13, view._23, view._33 }, right[3] = { view._11, view._21, view._31 }, up[3] = { view._12, view._22, view._32 };
		const D3DXVECTOR3 eyeScene = RtCore::Eye();	// the scene is relative to the eye
		const double eye[3] = { (double)TheRenderManager->CameraPosition.x + eyeScene.x, (double)TheRenderManager->CameraPosition.y + eyeScene.y,
			(double)TheRenderManager->CameraPosition.z + eyeScene.z };

		// The history is last frame's value of the same texel (a whole view's number: nothing to reproject), so it counts fully only while
		// the camera stands still; moving, at most half (two frames' lag), and not at all after a frame without a trace
		if (!(L.tracedOnce && L.tracedFrame + 1 == frameNow)) L.since = 0;
		bool still = L.lastValid;
		if (still) {
			const double dx = eye[0] - L.lastEye[0], dy = eye[1] - L.lastEye[1], dz = eye[2] - L.lastEye[2];
			still = dx * dx + dy * dy + dz * dz < 1.0 && forward[0] * L.lastForward[0] + forward[1] * L.lastForward[1] + forward[2] * L.lastForward[2] > 0.99999f;
		}
		float temporal = std::clamp(fog->lampHazeShadowTemporal, 0.0f, 0.95f);
		if (!still) temporal = (std::min)(temporal, 0.5f);
		// (perf-history) the setting is last frame's weight at 60 fps: the same decay a second here
		const float weight = RtFogGrid::HistoryWeight(L.since, RtHistoryTime::Keep(temporal, RtDenoise::HistoryClock()));

		RtFogLampGrid::Settings s;
		s.samples = std::clamp(fog->lampHazeShadowSamples, 1, 8);
		s.weight = weight;
		s.jitter = true;
		s.core = fog->lampHazeCore;
		// the rays' rules are the RT lamp shadows' (RtLampShadows.h Record: LampStop, SkipLampModels + LampModelDistance, SkipLampGlass)
		s.lampStop = (std::max)(TheSettingManager->GetSettingF(kLampRtSection, "LampStop"), 0.0f);
		const bool skipModels = TheSettingManager->GetSettingI(kLampRtSection, "SkipLampModels") != 0;
		s.modelDistance = skipModels ? (std::max)(TheSettingManager->GetSettingF("Shaders.ShadowsInteriors.Forward", "LampModelDistance"), 0.0f) : 0.0f;
		unsigned farMask = 0xEFu;	// without the alpha-tested casters (0x10): the shader adds them when it tests them
		if (TheSettingManager->GetSettingI(kLampRtSection, "SkipLampGlass")) farMask &= ~0x04u;	// RtLampShadows::MaskGlass
		s.farMask = farMask;
		s.nearMask = skipModels ? (farMask & ~0x08u) : farMask;	// RtLampShadows::MaskLampModel
		const int atlas = RtCore::AlphaAtlasImage();
		s.alphaTested = RtCore::AlphaTested() && atlas >= 0;

		// the fog's lamps into the tiles' slots (a lamp keeps its slot, so its history); committed only when the pass is recorded
		RtFogLampGrid::Slots next = L.slots;
		const unsigned kept = RtFogLampGrid::Assign(next, L.submitted, L.submitCount);
		RtFogLampGrid::PassConstants c;
		RtFogLampGrid::Fill(c, forward, right, up, proj._11, proj._22, eye, columns, rows, next, weight > 0.0f ? kept : 0u, s, L.frames);
		if (c.grid[3] < 1.0f) return;	// no lamp
		const int out = L.image[L.nextOut], hist = L.image[L.nextOut ^ 1];
		const int images[3] = { out, hist, s.alphaTested ? atlas : hist };	// [2]: binding 4, a stand-in (never read) without alpha-tested casters
		// UNOFFICIAL 2026-11-19: 1 thread per screen cell (shares 1 primary camera ray across all lamp slots instead of up to 16).
		if (!RtCore::AddPass(L.shader, images, 3, 0x1, &c, sizeof(c), (columns + 7) / 8, (rows + 7) / 8)) return;
		if (!RtCore::CopyOut(out, L.surface, w, h)) return;
		L.slots = next;
		L.tracedFrame = frameNow;
		L.tracedOnce = true;
		L.since++;
		L.nextOut ^= 1;
		for (int i = 0; i < 3; i++) { L.lastEye[i] = eye[i]; L.lastForward[i] = forward[i]; }
		L.lastValid = true;
		L.lastLamps = L.submitCount; L.lastSamples = s.samples; L.lastWeight = weight;
		if (L.frames++ == 0 || (L.frames % 3600) == 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT fog lamps traced (LampHazeShadows, frame %u): %d lamp(s), %u x %u cells a lamp (one per %u pixels), %d point(s) a cell "
				"(at most %u rays), last frame's weight %.2f, lamp stop %.0f, lamp models skipped over the last %.0f, alpha-tested casters %s.", L.frames, L.submitCount, columns, rows, L.cell,
				s.samples, columns * rows * (1u + (unsigned)L.submitCount * (unsigned)s.samples), weight, s.lampStop, s.modelDistance, s.alphaTested ? "tested" : "not in the scene");
	}
}

namespace RtFogLamps {
	void Submit(const D3DXVECTOR4* WorldReach, int Count) {
		L.submitCount = 0;
		if (!WorldReach) return;
		for (int i = 0; i < Count && L.submitCount < (int)RtFogLampGrid::kSlots; i++) {
			RtFogLampGrid::Lamp& lamp = L.submitted[L.submitCount++];
			lamp.world[0] = WorldReach[i].x; lamp.world[1] = WorldReach[i].y; lamp.world[2] = WorldReach[i].z;
			lamp.reach = WorldReach[i].w;
		}
		L.submitFrame = LampFrameNow();
	}

	float Code(const D3DXVECTOR4& WorldReach) {
		if (!LampUsable()) return 0.0f;
		RtFogLampGrid::Lamp lamp;
		lamp.world[0] = WorldReach.x; lamp.world[1] = WorldReach.y; lamp.world[2] = WorldReach.z;
		lamp.reach = WorldReach.w;
		const int slot = RtFogLampGrid::Find(L.slots, lamp);
		return slot < 0 ? -1.0f : (float)(slot + 1 + 32 * (int)L.cell);
	}

	IDirect3DTexture9* Texture() { return LampUsable() ? L.texture : nullptr; }

	std::string StatusText() {
		char t[320];
		if (L.submitCount <= 0 || LampFrameNow() - L.submitFrame > 2u)
			sprintf_s(t, "RT fog lamps off ([Shaders.VolumetricFog.Interiors] LampHazeShadows false, LampHaze off, or outdoors): frames traced %u", L.frames);
		else
			sprintf_s(t, "RT fog lamps on (%s): %d lamp(s), tile %u x %u (one cell per %u pixels), %d point(s) a cell, weight %.2f, frames traced %u%s, frames without a pass left %u",
				!RtCore::Running() ? "the RT core is not running: unshadowed" : RtCore::LeftOut(kLampUser) ? "its images did not fit: unshadowed" : "rides along with the other RT effects",
				L.lastLamps, L.columns, L.rows, L.cell, L.lastSamples, L.lastWeight, L.frames, LampUsable() ? " (in use)" : "", L.noRoom);
		return t;
	}
}

namespace RtFogVisibility {
	void Register() {
		shader = RtCore::AddShader("RtFogVisibility");
		// Order 2: set up after the others (the first left out when the images do not fit). UNOFFICIAL 2026-11-11 RidesEarly: with
		// NVR_RT_EARLY_PT its Record goes with the path tracer's world-start submit (it copies nothing in: camera, sun, settings only), so
		// outdoors the frame needs no second exchange; its copy out lands in CollectEarly, before the volumetric light's grid reads it
		RtCore::AddUser({ kUserName, Wants, Setup, Record, 0, 2, true });
		// UNOFFICIAL 2026-11-12 LampHazeShadows: the lamps' rider, after the sun's (Order 3: the first left out when the images do not
		// fit). Not RidesEarly: it is recorded at the RT point of the frame as before (indoors the early submit gains it nothing)
		L.shader = RtCore::AddShader("RtFogLamps");
		RtCore::AddUser({ kLampUser, LampWants, LampSetup, LampRecord, 0, 3, false });
	}

	D3DXVECTOR4 Constant(float GridDistance) {
		const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (lastTracedFrame != frameNow || S.debugView == 1 || !S.enabled) return D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		const float mode = fabsf(GridDistance - tracedDistance) <= 0.01f * (std::max)(tracedDistance, 1.0f) ? 1.0f : 2.0f;
		// (rtvol2) Expanded: the whole longer grid is traced, so no fade back to the shadow maps inside it (they end at 6000 anyway)
		if (expandedTraced) return D3DXVECTOR4(mode, tracedRange, 1.0f, tracedDistance);
		return D3DXVECTOR4(mode, tracedRange, S.fadeLength, tracedDistance);
	}

	float ExpandedGridDistance() { return ExpandedWanted() ? S.expandedDistance : 0.0f; }

	bool ExpandedLook(Look& Out) {
		const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		// the last trace was the expanded one, and recent (this frame or the one before: the light's constants may be set before the trace)
		if (!ExpandedWanted() || !expandedTraced || lastTracedFrame == 0xFFFFFFFF || frameNow - lastTracedFrame > 1u) return false;
		Out.distance = tracedDistance;
		Out.density = S.expandedDensity;
		Out.heightInv = S.expandedHeight > 0.0f ? 1.0f / S.expandedHeight : 0.0f;
		Out.glow = S.expandedGlow;
		Out.sharpness = S.expandedSharpness;
		return true;
	}

	std::string StatusText() {
		if (!S.enabled) return "RT fog visibility off ([Shaders.VolumetricLight.RT] Enabled false): the shafts and the fog's sun from the cascades";
		const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		char t[320];
		sprintf_s(t, "RT fog visibility on (%s): grid %u x %u, frames traced %u, last f%u%s, RaysPerCell %d, Temporal %.2f, Range %.0f, DebugView %d, frames without a pass left %u",
			!RtCore::Running() ? "the RT core is not running: the cascades" : "rides along with the other RT effects", imageColumns, imageRows, frames,
			lastTracedFrame == 0xFFFFFFFF ? 0u : lastTracedFrame, lastTracedFrame == frameNow ? " (this frame)" : "", S.grid.rays, S.grid.temporal, tracedRange, S.debugView, noRoom);
		return t;
	}

	// UNOFFICIAL 2026-11-13 (RtFogVisibility.h). The order of the tests is the order a player would have to fix them in. Reads only.
	int MenuState(std::string& Text, int* Code) {
		auto say = [&](int state, int code, const std::string& text) { Text = text; if (Code) *Code = code; return state; };
		ReadSettings();
		if (!S.enabled) return say(0, 1, "off");
		if (RtCore::Gated()) return say(0, 14, "off here: a ray tracing master switch (indoors / outdoors) is off for this space: the shafts use the shadow maps");	// UNOFFICIAL 2026-11-15
		ShaderManager* sm = TheShaderManager;
		VolumetricLightEffect* light = sm->Effects.VolumetricLight;
		if (!sm->GameState.isExterior) {
			// the sun's shafts do not exist indoors; the indoor shafts are the fog's lamp haze, with its own traced shadows
			VolumetricFogEffect* fog = sm->Effects.VolumetricFog;
			const char* haze = "it is off: Volumetric fog > Interiors > LampHazeEnabled";
			int code = 10;
			if (fog && fog->Enabled && fog->lampHaze > 0.0f) {
				if (!fog->lampHazeShadows) { haze = "it is on, without traced shadows (LampHazeShadows is off)"; code = 11; }
				else if (LampUsable()) { haze = "it is on, with traced shadows (LampHazeShadows)"; code = 12; }
				else { haze = "it is on; LampHazeShadows waits for another ray-traced effect to run here"; code = 13; }
			}
			return say(0, code, std::string("indoors: the sun's light shafts are outdoors only. Indoor shafts are the fog's lamp haze: ") + haze);
		}
		if (sm->GameState.isUnderwater) return say(0, 2, "under water: no light shafts");
		if (!light || !light->Enabled) return say(2, 3, "on, but Volumetric light is off: there are no light shafts to trace");
		if (!light->ShouldRender())
			return say(2, 4, "on, but no light shafts are drawn now (the sun's shadows are off or faded out: sunrise, sunset, a dim moon)");
		if (!RtCore::Running() || !HostTraces(kUserName))
			return say(2, 5, "waiting: it traces only while another ray-traced effect runs here (sun shadows, occlusion, reflections, lamp shadows "
				"or path tracing). Now the shafts use the shadow maps");
		if (RtCore::LeftOut(kUserName)) return say(2, 6, "on, but its images did not fit when ray tracing started: the shafts use the shadow maps");
		const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (lastTracedFrame == 0xFFFFFFFF || frameNow - lastTracedFrame > 2u)
			return say(2, 7, "on, but not traced these frames (the frame's ray tracing passes are full, or the grid does not fit the screen): "
				"the shafts use the shadow maps");
		if (S.debugView == 1) return say(2, 8, "traced but not used (DebugView 1): the shafts use the shadow maps");
		// Running. It only moves the shadows in the lit air: against the shadow maps the picture changes within its frame-to-frame noise
		// at the default look (bench 2026-11-13, og-goodsprings: mean |difference| 0.7 of 255 against 0.6 between two equal shots), so say
		// what to expect and where the look is set
		char t[400];
		if (expandedTraced) {	// (rtvol2)
			sprintf_s(t, "running, modern look (Expanded): the lit air reaches %.0f units, all of it traced (far hills, buildings and people cast "
				"shafts of shade), the dust thins with height, the glow gathers round the sun; cloud shadows in the air %s", tracedDistance,
				cloudsInTrace ? "on" : !S.expandedClouds ? "off" : imageCloud < 0 ? "not available (Volumetric clouds' shadows were off when ray tracing started)" : "none now");
			return say(1, cloudsInTrace ? 22 : 21, t);
		}
		sprintf_s(t, "running: the lit air's shadows are traced out to %.0f units (fences, leaves, people and far buildings shade the air as "
			"they shade the ground). Expect a small change against the shadow maps: it adds no light of its own. How strong the lit air and "
			"its shafts are is Volumetric light > Strength, Density and Darken", tracedRange);
		return say(1, 20, t);
	}

	void LogIfChanged() {
		static int lastCode = -1, lines = 0;
		if (lines >= 12 || !RtCore::Running()) return;
		std::string text;
		int code = 0;
		MenuState(text, &code);
		if (code == lastCode) return;
		lastCode = code;
		lines++;
		Logger::Log("RT LIGHT SHAFTS ([Shaders.VolumetricLight.RT], line %d of at most 12): %s. | %s | %s", lines, text.c_str(), StatusText().c_str(), RtFogLamps::StatusText().c_str());
	}
}
