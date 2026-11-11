#pragma once

// UNOFFICIAL 2026-11-01 the temporal backbone (docs/next/FLICKER-PROGRAM.md "Part 2"; [Main.Main.Quality] SharedNoise, live, on).
// One owner for the three things every frame-to-frame effect used to work out on its own:
//   - FrameIndex: one counter (FrameRateManager::FrameNumber, advanced once per rendered frame), wrapped at 1024;
//   - TemporalResolve: TAA, FSR 2 / 3.1 or DLSS (all inside TAAEffect) averages frames this frame;
//   - HistoryValid: last frame's picture is worth reprojecting (no camera cut, no move above 512 units, no gap of a frame or more
//     than 0.5 s, the same render size, RenderScale and upscaler).
// The rule (SharedNoise on): an effect WITHOUT its own history animates its noise only when TemporalResolve is on (NoiseIndex() =
// FrameIndex, else 0: a still pattern instead of one that twinkles with nothing to average it); an effect WITH a history keeps
// animating, from EffectIndex(its own offset) (one counter, each effect shifted so their patterns do not beat), and ANDs
// HistoryValid() (or CameraContinuous()) into its own reset conditions. SharedNoise off: every effect runs its old counters.
// Shaders: TESR_FrameNoise (x NoiseIndex, y TemporalResolve, z/w the R2 offset of NoiseIndex), Effects/Includes/FrameNoise.hlsl.
// Update() runs once a frame (idempotent per FrameNumber) from ShaderManager::UpdateConstants, right after the frame's camera-cut
// flag is made (it reads it); the accessors return the answers of the last Update (every effect reads them after that point).

namespace TemporalBackbone {

	// Fixed per-effect offsets into the shared index (decorrelation; the still values of the dithers are the ones Pattern 1 used).
	enum Offset : UInt32 {
		OffsetTonemap = 0, OffsetSkyDither = 32, OffsetVolumetricLight = 11, OffsetReflections = 23, OffsetAmbientLight = 101,
		OffsetBounceLight = 211, OffsetClouds = 0, OffsetRtAmbient = 307, OffsetSkin = 47,
	};

	inline UInt32 UpdatedFrame = 0xFFFFFFFF;
	inline bool SharedOn = true;
	inline UInt32 Index = 0;			// FrameIndex (0-1023)
	inline bool Resolve = false;		// TemporalResolve
	inline bool CameraOk = false;		// cut / move / gap only
	inline bool Valid = false;			// CameraOk and the same render size / RenderScale / upscaler
	inline D3DXVECTOR4 Constant = { 0.0f, 0.0f, 0.5f, 0.5f };	// TESR_FrameNoise
	// last frame's
	inline UInt32 LastFrame = 0xFFFFFFFF;
	inline double LastTime = -1.0;
	inline D3DXVECTOR3 LastCamera = { 0.0f, 0.0f, 0.0f };
	inline UInt32 LastWidth = 0, LastHeight = 0;
	inline float LastScale = 0.0f;
	inline int LastUpscaler = -1;
	inline UInt32 Resets = 0, Frames = 0;

	// R2 (Roberts 2018, the plastic number): well spread 2D points; slot shifts the whole sequence by its own fixed point.
	inline D3DXVECTOR2 R2(UInt32 index, UInt32 slot = 0) {
		const double a1 = 0.7548776662466927, a2 = 0.5698402909980532;
		const double n = (double)index + (double)slot * 0.5;	// (slot: half-steps, so slot k never lands on another slot's index)
		return D3DXVECTOR2((float)(0.5 + n * a1 - floor(0.5 + n * a1)), (float)(0.5 + n * a2 - floor(0.5 + n * a2)));
	}

	inline void Update() {
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame == UpdatedFrame) return;
		UpdatedFrame = frame;
		SharedOn = TheSettingManager->GetSettingI("Main.Main.Quality", "SharedNoise") != 0;
		Index = frame % 1024;

		// TemporalResolve: TAAEffect::Render's own gates (FSR 2 / 3.1 and DLSS run inside it, before or after the effects) and those
		// RenderEffects applies before any effect; not its jitter (an unjittered TAA still averages the frames).
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		Resolve = taa && taa->Enabled && taa->Effect != nullptr && taa->ShouldRender() &&
			TheSettingManager->SettingsMain.Main.RenderEffects && Player && Player->parentCell && !TheShaderManager->GameState.OverlayIsOn;

		// HistoryValid
		const D3DXVECTOR4& c = TheRenderManager->CameraPosition;
		const D3DXVECTOR3 camera(c.x, c.y, c.z);
		const D3DXVECTOR3 move = camera - LastCamera;
		const double now = TheFrameRateManager->Time;
		const bool upscaling = taa && taa->Enabled && taa->FSRSettings.Enabled;
		const float scale = upscaling ? taa->FSRSettings.RenderScale : 1.0f;
		const int upscaler = upscaling ? taa->FSRSettings.Upscaler : -1;
		const UInt32 w = TheRenderManager->width, h = TheRenderManager->height;
		CameraOk = LastFrame != 0xFFFFFFFF && LastFrame + 1 == frame && now - LastTime >= 0.0 && now - LastTime <= 0.5 &&
			D3DXVec3Length(&move) <= 512.0f && !TheShaderManager->GameState.isCameraCut;
		Valid = CameraOk && w == LastWidth && h == LastHeight && scale == LastScale && upscaler == LastUpscaler;
		LastFrame = frame; LastTime = now; LastCamera = camera; LastWidth = w; LastHeight = h; LastScale = scale; LastUpscaler = upscaler;
		Frames++;
		if (!Valid) Resets++;

		const UInt32 noise = Resolve ? Index : 0;
		const D3DXVECTOR2 r2 = R2(noise);
		Constant = D3DXVECTOR4((float)noise, Resolve ? 1.0f : 0.0f, r2.x, r2.y);
		if ((Frames % 3600) == 1)
			Logger::Log("UNOFFICIAL TemporalBackbone: SharedNoise %s, temporal resolve %s, %u history resets in %u frames",
				SharedOn ? "on" : "off", Resolve ? "on" : "off", Resets, Frames);
	}

	inline bool Shared() { return SharedOn; }
	inline UInt32 FrameIndex() { return Index; }
	inline bool TemporalResolve() { return Resolve; }
	// For an effect without a history: FrameIndex while a temporal resolve averages frames, else 0 (a still pattern).
	inline UInt32 NoiseIndex() { return Resolve ? Index : 0; }
	// For an effect with a history: the shared index shifted by the effect's own offset, wrapped at period (a divisor of 1024 keeps
	// the sequence seamless at the wrap).
	inline UInt32 EffectIndex(UInt32 offset, UInt32 period = 1024) { return (Index + offset) % period; }
	inline bool HistoryValid() { return Valid; }
	// HistoryValid without the render-size part (an effect whose history does not depend on the render size: the clouds).
	inline bool CameraContinuous() { return CameraOk; }

	// UNOFFICIAL 2026-11-09 TemporalJitterFix for the reprojecting effects (roadmap track 1 step 2; AmbientLight's own since 2026-11-01):
	// TAA / FSR / DLSS draw the world shifted by a fraction of a pixel that changes every frame, and an effect drawn from that depth holds
	// each surface where its frame's shift put it. A reprojection that ignores the shift reads the history off by the shift's change
	// (up to a rendered pixel, three screen pixels at RenderScale 0.33): moving, the history smeared into streaks. DepthJitter is this
	// frame's shift in screen uv (TAAEffect::DepthJitterUV; 0 when nothing is jittered); an effect keeps last frame's next to its last
	// camera and shifts its reprojection by now - last, which reads the history at last.xy + (now - last), as AmbientLight's Accumulate.
	inline D3DXVECTOR2 DepthJitter() {
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		return taa ? taa->DepthJitterUV() : D3DXVECTOR2(0.0f, 0.0f);
	}
	// reproject: a point (row vector) -> last frame's clip space, read as uv = (clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5). Shifted
	// so that uv moves by delta (screen uv): clip.x += 2 dx w, clip.y -= 2 dy w (a column operation, so the D3D9 effect and the GLSL twin,
	// which read the same matrix, both get it).
	inline void ShiftReprojection(D3DXMATRIX& reproject, const D3DXVECTOR2& delta) {
		if (delta.x == 0.0f && delta.y == 0.0f) return;
		for (int i = 0; i < 4; i++) {
			reproject.m[i][0] += 2.0f * delta.x * reproject.m[i][3];
			reproject.m[i][1] -= 2.0f * delta.y * reproject.m[i][3];
		}
	}
}
