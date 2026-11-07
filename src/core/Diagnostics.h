#pragma once
#include <string>

// UNOFFICIAL 2026-11-01 diagnostics for logs players share (user: "logs players share publicly must let us diagnose problems we can't
// reproduce"). Three parts, each switchable, privacy-safe (every line goes through the Logger's scrubber; no character or save names),
// bounded and close to free while nothing happens (docs/UNOFFICIAL-TECHNICAL.md "Diagnostics"):
//   1. the automatic flash / flicker detector ([Main.Main.Misc] FlashDetect, on): each rendered frame the finished picture (before the
//      HUD) is shrunk to 8 x 8 on the GPU (a chain of 2:1 StretchRects, an exact box average) and copied to one of 4 system-memory
//      surfaces (GetRenderTargetData); the copy made 3 frames earlier is locked with D3DLOCK_DONOTWAIT (the CPU never waits for the
//      GPU: a copy not finished yet is skipped). The 64 cell luminances of the last frames are judged by core/FlashRule.h. A FLASH
//      writes one line (which cells, how big, darker or brighter, black / white frame), the frame journal's events of frames
//      N-30 .. N+3 and every effect's state that frame (core/EffectStatus.h); at most one dump per 10 s and 50 a session, the rest
//      as short lines (at most one a second, 300 a session) and counted in the HEARTBEAT. Skipped around loads, cell changes, menus,
//      the Pip-Boy, VATS, dialogue, the NVR overlay's "overlay on" state, fast turns (over 5 degrees a frame) and history resets.
//      If it ever costs over 0.5 ms a frame on average (a driver whose GetRenderTargetData waits) it turns itself off (logged).
//   2. (REMOVED 2026-11-05: no key starts it any more; Help > Copy bug report carries the session log) the report key ([Main.Main.Misc] ReportKey, DIK, default 27 = ']', 0 = off; the game's key check Global->OnKeyDown on the
//      render thread, not while an NVR menu text box has the keyboard): a REPORT block (where, weather, game hour, camera, the upscaler
//      path, the last 5 s of events and frame times, every effect's state, levels / fallbacks, counters), then a BURST (2026-11-01):
//      the press frame and the next ReportFrames-1 rendered frames (16, 1..32), each with one "REPORT n frame k/N f<frame>" line
//      written in that frame (frame time, exposure, effects that changed state, VolumetricLight, the journal's counts) and its picture
//      copied on the GPU; after the burst the pictures NVR-Report-<date>-<n>-f<frame>-<k>of<N>.png are read back and written by a
//      worker thread, each with a "REPORT n picture k/N f<frame>" line (mean luma, AvgLuma / exposure values). Diagnostics.cpp has
//      the ring size, the read-back and the fallback. The game shows a short message.
//      The burst also records the volumetric light's own stages in each burst frame (2026-11-01, docs/next/FLICKER-PROGRAM.md
//      "Established facts"): the march, the history it read, the temporal blend, the blur, the frame before and after its composite,
//      each shrunk to 8 x 8 on the GPU, plus the effect's values that frame; after the burst one "REPORT n VL k/N f<frame>" line per
//      frame and a "REPORT n VL pattern" line (Diagnostics.cpp "VL stages").
//   3. the effects' status registry (core/EffectStatus.h), summarised in the HEARTBEAT.
struct IDirect3DDevice9;
struct IDirect3DSurface9;

//   4. the stage probes ([Main.Main.Misc] FlashStages: 0 off, 1 = ARMED, the default, 2 = always; docs/next/FLICKER-ISOLATION-PLAN.md
//      step 1): the detector's 8 x 8 summary taken AFTER EVERY EFFECT of the frame (the scene before NVR, each effect that drew, the
//      tonemapped picture), so a FLASH says which stage's own contribution jumped (core/FlashRule.h Attribute: the first stage whose
//      jump minus the stage before it is large enough), which later stages amplified or damped it, and what the stages were. One
//      StretchRect to 64 x 64 then 2:1 to 8 x 8 into a row of one strip surface per format; one GetRenderTargetData a frame per format,
//      read 3 frames late without waiting, as the detector. ARMED = the probes run for StageArmSeconds after a FLASH (or a report key
//      press), so the next flash gets attributed and idle frames cost nothing; its own 0.5 ms self-off. Effects with their own
//      buffers add EFFECT_PROBE("<effect> <pass>", surface) where a pass ends: those are listed with their own jump (not differenced).
class EffectRecord;

namespace Diagnostics {
	// NewVegas/Hooks/Render.cpp: the finished picture before the HUD, once a rendered frame (after the screenshot key)
	void OnFinalPicture(IDirect3DDevice9* device, IDirect3DSurface9* picture);
	// SessionLog's HEARTBEAT (every 60 s in game): the minute's effect counts, the detector's counts and cost, cache and failure counts
	void Heartbeat();
	// UNOFFICIAL 2026-11-01 stage probes (above). StageAfterEffect: after every frame effect's Render (PixelInspector::AfterFrame's call
	// sites: ShaderManager), the picture after it when it wrote the frame. StageMark: a named point of the chain (the scene before NVR,
	// the tonemapped picture). ProbeOwn: an effect's own buffer after one of its passes (EFFECT_PROBE). All no-ops unless the probes run.
	void StageAfterEffect(EffectRecord* effect);
	void StageMark(const char* name, IDirect3DSurface9* picture);
	// The marks' names (ShaderManager passes these; the attribution re-bases the chain at the tonemap mark: HDR before, display after)
	constexpr const char* SceneMarkName = "scene before Vegas Shaders";
	constexpr const char* TonemapMarkName = "tonemapped (the game's tonemap, before the post effects)";
	void ProbeOwn(const char* name, IDirect3DSurface9* buffer);
	// UNOFFICIAL 2026-11-01 the detector's self-test ([Main.Develop.Main] FlashSelfTestEvery = N frames, 0 = off; FlashSelfTestEffect = the
	// effect's name): every Nth rendered frame that effect's Render returns without drawing, a one-frame dropout on purpose. The flash
	// detector must then write a FLASH and the stage probes must name that effect as the source: the end-to-end check of both, in the
	// bench (its toml) or in the user's game. EffectRecord::Render asks this first thing.
	bool SelfTestSkip(const EffectRecord* effect);

	// --- UNOFFICIAL 2026-11-01 the history watch (part 5) ---
	//   5. the history watch ([Main.Main.Misc] HistoryWatch, on; docs/next/TESTER-REPORTS-2026-11-01.md): for pictures that smear, melt or
	//      wash out over several frames (a temporal history that stops converging on the picture) rather than flash. A temporal effect
	//      hands its input and its output of the frame here each frame (TAA / FSR 2 / DLSS at native resolution: the picture before the
	//      resolve and the upscaler's output; RenderScale's upscale: the small picture and the upscaled one). Each is shrunk to 8 x 8 as
	//      the detector does and read back 3 frames late without waiting. Three rules: NaN / infinite cells in the output; a RUNAWAY
	//      (the output's mean cell difference from its input, relative to the input's mean, over DriftLimit for DriftFrames judged frames
	//      in a row); a CYCLE (core/FlashRule.h CycleRule: the output repeats exactly every P frames while changing, the same few
	//      pictures in a fixed order = a history that is a function of the jitter step and the ping-pong alone; the 2026-11-01 4K FSR 2
	//      smear cycled 5 pictures every 16 frames). Each line carries the effect's own state (state(): the upscaler, its reset, the
	//      jitter step, sizes, frames since its reset) and the conditions. One line per 10 s and rule, 300 a session, counted in the
	//      HEARTBEAT; its own 0.5 ms self-off. input may be null (no drift rule then); inputWidth x inputHeight: the drawn part of input
	//      (0: all of it). At most 4 named probes; one per name and frame.
	void HistoryProbe(const char* name, IDirect3DSurface9* input, unsigned inputWidth, unsigned inputHeight, IDirect3DSurface9* output, std::string (*state)());
	std::string HistoryWatchText();	// its probes' last verdicts, for the REPORT block

	// --- the report burst's volumetric light stages (Diagnostics.cpp "VL stages") ---
	// True only while a report burst captures (from the end of its first frame to the end of its last). The effect's hooks
	// (VolumetricLight.cpp RenderBuffers, ShaderManager.cpp around the composite) test this alone: outside a burst nothing else runs.
	inline bool VLCapturing = false;
	enum VLStage { VLMarch, VLHistory, VLTemporal, VLBlur, VLBefore, VLAfter, VLStageCount };
	// The effect's own values in this frame (RenderBuffers, before its passes)
	struct VLValues {
		float	noise = 0.0f;			// Constants.Data2.w: the march's noise index
		float	weight = 0.0f;			// Constants.Data3.x: the history weight used (0: no history this frame)
		float	fade = 0.0f;			// Constants.Data2.z
		float	strength = 0.0f, darken = 0.0f;	// Constants.Data.x / .y (the day / night blend)
		bool	temporal = false, usable = false, haveHistory = false, drewLastFrame = false;
		bool	sharedNoise = false, historyValid = true;	// [Main.Main.Quality] SharedNoise, TemporalBackbone::HistoryValid()
		bool	rect = false, scaleChanged = false;			// RenderRect::Active(), ScaleChanged()
		double	cameraMove = 0.0;		// game units since the frame the effect last drew
		unsigned technique = 0;			// CompositeTechnique() (8: the froxel grid, no march stages)
	};
	void VLNote(const VLValues& values);
	// A volumetric light buffer's content now (its stage: march, history, temporal, blur), into this burst frame's slot
	void VLCapture(IDirect3DDevice9* device, VLStage stage, IDirect3DSurface9* surface);
	// The frame right before (after = false: what the composite reads) or right after the composite (the picture it wrote)
	void VLFrame(IDirect3DDevice9* device, bool after);
	// The path a technique took this frame (0 / 8 composite, 1 march, 2 blur X, 3 temporal, 4 blur Y): true = the Vulkan pass
	// (ExtRender drew it), false = the D3D9 technique
	void VLPass(unsigned technique, bool vulkan);
}
#define EFFECT_PROBE(name, surface) do { Diagnostics::ProbeOwn(name, surface); } while (0)
