// UNOFFICIAL 2026-11-07 (ptaaa-fog) RT fog visibility: RtFogVisibility.h; the trace shader Compute\Rt\RtFogVisibility.comp (run by the
// helper), its constants and history rules RtFogGrid.h (shared with the RT test host's fogvis scene).

#include "RtFogVisibility.h"
#include "RtCore.h"
#include "RtFogGrid.h"
#include "RtDenoise.h"	// UNOFFICIAL 2026-11-07 (perf-history) HistoryClock

namespace {
	const char* const kSection = "Shaders.VolumetricLight.RT";
	const char* const kUserName = "RT fog visibility";

	struct Settings {
		bool	enabled = true;
		RtFogGrid::Settings grid;
		float	fadeLength = 1000.0f;
		int		debugView = 0;		// 1: traced, not used (the cascades: an A/B)
	} S;

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
	}

	// Outdoors, the volumetric light on, the helper already running for another RT effect
	bool Wants() {
		ReadSettings();
		if (!S.enabled || !RtCore::Running()) return false;
		VolumetricLightEffect* light = TheShaderManager->Effects.VolumetricLight;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (!light || !light->Enabled || !shadows || !TheShaderManager->GameState.isExterior || TheShaderManager->GameState.isUnderwater) return false;
		return RtCore::OthersWant(kUserName);
	}

	void Setup() {
		RtFogGrid::Columns(TheRenderManager->width, TheRenderManager->height, imageColumns, imageRows);
		image[0] = RtCore::AddImage("NVR RT fog visibility 0", imageColumns * 8, imageRows * 8, NVR_EXT_SHARED_R16F);
		image[1] = RtCore::AddImage("NVR RT fog visibility 1", imageColumns * 8, imageRows * 8, NVR_EXT_SHARED_R16F);
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
		const int atlas = RtCore::AlphaAtlasImage();
		s.alphaTested = RtCore::AlphaTested() && atlas >= 0;
		s.mask = 0xFFu;	// land, objects, alpha-tested, actors, the far LOD: every caster the RT sun shadows see
		if (!(lastTracedFrame != 0xFFFFFFFF && lastTracedFrame + 1 == frameNow && RtFogGrid::Continues(now, lastView))) since = 0;
		const float weight = RtFogGrid::HistoryWeight(since, s.temporal);
		RtFogGrid::PassConstants c;
		RtFogGrid::Fill(c, now, lastView, weight, sunUp, s, frames);

		const int out = image[nextOut], hist = image[nextOut ^ 1];
		const int images[3] = { out, hist, s.alphaTested ? atlas : hist };	// [2]: binding 4, a stand-in (never read) without alpha-tested casters
		if (!RtCore::AddPass(shader, images, 3, 0x1, &c, sizeof(c), (columns * 8 + 7) / 8, (rows * 8 + 7) / 8)) return;
		if (!RtCore::CopyOut(out, light->Textures.RtVisibilitySurface, columns * 8, rows * 8)) return;
		lastTracedFrame = frameNow;
		lastView = now;
		since++;
		nextOut ^= 1;
		tracedDistance = now.maxDistance;
		tracedRange = s.range;
		if (frames++ == 0 || (frames % 3600) == 0)
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: RT fog visibility traced (frame %u): grid %u x %u x 64 out to %.0f, %d ray%s a cell (%u rays), Temporal %.2f (weight %.2f), "
				"range %.0f (fade %.0f), alpha-tested casters %s, DebugView %d.", frames, columns, rows, now.maxDistance, s.rays, s.rays > 1 ? "s" : "",
				columns * rows * 64u * (unsigned)s.rays, s.temporal, weight, s.range, S.fadeLength, s.alphaTested ? "tested" : "not in the scene", S.debugView);
	}
}

namespace RtFogVisibility {
	void Register() {
		shader = RtCore::AddShader("RtFogVisibility");
		RtCore::AddUser({ kUserName, Wants, Setup, Record, 0, 2 });	// Order 2: set up after the others (the first left out when the images do not fit)
	}

	D3DXVECTOR4 Constant(float GridDistance) {
		const UInt32 frameNow = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		if (lastTracedFrame != frameNow || S.debugView == 1 || !S.enabled) return D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		const float mode = fabsf(GridDistance - tracedDistance) <= 0.01f * (std::max)(tracedDistance, 1.0f) ? 1.0f : 2.0f;
		return D3DXVECTOR4(mode, tracedRange, S.fadeLength, tracedDistance);
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
}
