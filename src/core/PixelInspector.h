#pragma once

// UNOFFICIAL 2026-10-26 (debugging tool, needs NVR DXVK 1.5-debug = fork patch 0016 (0015 before the 10-27 renumbering); docs\next\FORK-DEBUG-PIXEL.md).
//
// The pixel inspector: [Main.Develop.PixelInspector] Enabled, X, Y, LogEvery. For one screen pixel it records the value that pixel
// has at every stage NVR can see during a frame: the scene before NVR (HDR), the linear depth, the view normal, the picture after
// each NVR effect that drew (and the effects' own buffers, e.g. the shadow buffer the shadow passes write), the tonemapped picture
// and the final picture before the HUD. Each stage is one tiny compute dispatch (Compute\PixelInspect.comp: the 3x3 texels round
// the pixel into a storage buffer); the buffer is read back a few frames later through the non-waiting readback
// (core\ShaderCounters.h DebugReadbackRing), so the game never waits for it. Results: an ImGui window while the overlay is open
// (pick the pixel with the mouse there) and the log ("PIXEL INSPECTOR" lines when the pixel changes, and every LogEvery frames).
// Shot lists: 'inspect <x> <y>' (core\SettingShots.h).
//
// Off (the default), or on native D3D9 / stock DXVK / an NVR DXVK without 1.5-debug: nothing is created and each hook is one bool
// test (Active). Without the extension the log says once that it needs NVR DXVK, and the window says so.

class EffectRecord;

namespace PixelInspector {
	enum Kind { Color = 0, Depth = 1, Normal = 2, Buffer = 3, RtInput = 4 };	// RtInput (2026-11-09): NVR_RTAOInput (x view distance, y the packed world normal)

	extern bool Active;	// this frame records stages (set once a frame by EndFrame)

	// One stage: Resource = an IDirect3DSurface9* or a 2D texture (its top level). Label is copied. A resource bound as a render
	// target at that moment is unbound around the copy and put back (with the viewport and scissor rectangle).
	void Capture(const char* Label, IUnknown* Resource, Kind kind = Color);
	inline void CaptureIf(const char* Label, IUnknown* Resource, Kind kind = Color) { if (Active && Resource) Capture(Label, Resource, kind); }

	// After each frame effect's Render call (ShaderManager's call sites; many effects override Render): the picture after it, if the
	// frame was written since the last stage (FrameChain::WriteCount; without the chain: when the effect is enabled).
	void AfterFrame(EffectRecord* Effect);
	// Passes into an effect's own target (ShaderManager::RenderEffectToRT, and EffectRecord::Render for passes drawn with RenderTarget ==
	// RenderedSurface outside it): that target after the pass. Cube map faces are left out (the reflection captures: dozens a frame).
	void AfterOwnTarget(EffectRecord* Effect, IDirect3DSurface9* Target, UINT Technique);
	extern int InOwnTarget;	// inside RenderEffectToRT: EffectScope leaves the stage to OwnTargetScope
	struct OwnTargetScope {
		EffectRecord* effect; IDirect3DSurface9* target; UINT technique;
		OwnTargetScope(EffectRecord* e, IDirect3DSurface9* t, UINT tech) : effect(e), target(t), technique(tech) { InOwnTarget++; }
		~OwnTargetScope() { InOwnTarget--; if (Active) AfterOwnTarget(effect, target, technique); }
	};
	struct EffectScope {	// EffectRecord::Render: own-target passes drawn without RenderEffectToRT
		EffectRecord* effect; IDirect3DSurface9* target; IDirect3DSurface9* rendered; UINT technique;
		EffectScope(EffectRecord* e, IDirect3DSurface9* t, IDirect3DSurface9* r, UINT tech) : effect(e), target(t), rendered(r), technique(tech) {}
		~EffectScope() { if (Active && !InOwnTarget && target && target == rendered) AfterOwnTarget(effect, target, technique); }
	};

	// Once a frame, after NVR's post chain (Hooks\Render.cpp): queues this frame's records, reads the finished ones (never waits),
	// logs, and reads the settings for the next frame.
	void EndFrame();

	// The inspector's pixel (the X, Y settings, even with the inspector off) mapped into a target of Width x Height (a pass's own
	// buffer): false when no pixel is set. For ShaderCounters' probe pixel.
	bool PixelFor(UINT Width, UINT Height, int& x, int& y);

	// The ImGui window and the picker (ImGuiManager::BuildUI, overlay open). Shown while Enabled or a shader counter is on.
	void DrawUI();
}
