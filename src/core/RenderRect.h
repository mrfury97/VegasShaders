#pragma once

// UNOFFICIAL 2026-10-27 RenderScale v2 (docs/next/RENDERSCALE-V2.md; user: "shouldn't everything in the image be rendered lower then
// upscaled?"). With [Shaders.TAA.FSR] / [Shaders.TAA.DLSS] RenderScale below 1, the world scene, the first-person weapon and NVR's
// effects before tonemapping all draw at the lower internal resolution, into the TOP-LEFT part of their full-size targets, and FSR 2 /
// DLSS upscale the frame at the end of ShaderManager::RenderEffectsPreTonemapping, before the game's image space. Nothing is
// reallocated: a screen-shaped texture keeps its size and the effects use its top-left ScaleX x ScaleY part. Shaders see the same
// numbers as TESR_RenderScale (xy = Scale, zw = 1 / Scale; Includes/RenderRect.hlsl). Off: On false, Scale 1, every helper a no-op.
//
// The state is set by TAAEffect (effects/TAAUpscale.cpp): Begin after a world scene drawn small, End right after the upscale.

#include <cmath>

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct _D3DVIEWPORT9;	// D3DVIEWPORT9

namespace RenderRect {
	inline bool		On = false;					// this frame's NVR pre-tonemap effects draw at the internal resolution
	inline float	ScaleX = 1.0f;				// render width / screen width (exactly, TAAEffect::Up.RenderWidth / Up.Width)
	inline float	ScaleY = 1.0f;
	inline float	LastScaleX = 1.0f;			// the previous rect frame's, for effects whose history must start again when it changes
	inline float	LastScaleY = 1.0f;

	inline unsigned	LastRectFrame = 0xFFFFFFFF;	// (2026-10-31) the frame number of the last Begin (TAAEffect::UpscaleBeginEffects)

	inline bool Active() { return On; }
	// (2026-10-31, tester: RT AO's log flipped "running" / "passes off" every frame at RenderScale 0.67) Rect frames are in use: this
	// frame is one, or one of the last 2 was. The same answer before Begin, inside and after End, for features that must decide once a
	// frame (their constants are set before Begin, their passes run inside).
	inline bool InUse(unsigned frame) { return On || (LastRectFrame != 0xFFFFFFFF && frame - LastRectFrame <= 2); }
	// The part of a w-pixel-wide / h-pixel-high screen-shaped texture the effects draw into this frame (the whole of it when off).
	inline unsigned Width(unsigned w) { return On ? (unsigned)floorf((float)w * ScaleX + 0.5f) : w; }
	inline unsigned Height(unsigned h) { return On ? (unsigned)floorf((float)h * ScaleY + 0.5f) : h; }
	// True while a rect frame uses another scale than the one before (temporal effects reset their history then).
	inline bool ScaleChanged() { return On && (ScaleX != LastScaleX || ScaleY != LastScaleY); }

	// TAAEffect only: Begin after a world scene drawn small (LowResEffects), End right after the upscale.
	inline void Begin(float sx, float sy) { On = true; ScaleX = sx; ScaleY = sy; }
	inline void End() { if (On) { LastScaleX = ScaleX; LastScaleY = ScaleY; } On = false; ScaleX = ScaleY = 1.0f; }

	// Suspend (2026-10-27, VolumetricClouds): passes inside a rect frame that work on their own full-size targets in screen space,
	// independent of the scene's resolution, run as in a native frame while it lasts (whole viewports, NVR DXVK's Vulkan passes on):
	// On false, back when it ends. The shader constant TESR_RenderScale is the caller's to set to 1 for them.
	struct Suspend {
		bool was;
		Suspend() : was(On) { On = false; }
		~Suspend() { On = was; }
	};

	// Defined in ShaderManager.cpp (they need the render manager and the shared quads; no new .cpp in the project).
	// ScreenShaped: w x h is not larger than the screen and has its aspect (within 1 %): shadow maps, sky / LUT textures, 1x1 and
	// small square reduction targets are not. ApplyViewport: call right before a full-screen quad's DrawPrimitive (after
	// SetRenderTarget): in a rect frame with a screen-shaped render target 0, the viewport becomes its top-left Width x Height part
	// and the quad with texture coordinates 0..Scale is bound (ShaderManager::FrameVertexRect); otherwise FrameVertex replaces the
	// rect quad if that is bound and the viewport is left as SetRenderTarget set it. Off: does nothing.
	bool ScreenShaped(unsigned w, unsigned h);
	void ApplyViewport(IDirect3DDevice9* Device);
	// WholeViewport: an effect that saves the viewport it starts with (to work out a layout from its size, or to put it back) calls
	// this on what GetViewport gave: in a rect frame, the drawn part an earlier ApplyViewport left on render target 0 becomes the
	// whole target again (ApplyViewport sets the drawn part per draw). Off, or any other viewport: unchanged.
	void WholeViewport(IDirect3DDevice9* Device, _D3DVIEWPORT9& Viewport);
	// RescaleAfterUpscale (user: a ghosted copy of the picture over the whole screen with NVR's tonemapping on): a buffer drawn in a
	// rect frame that something reads AFTER the upscale, at full size (Bloom's top level: the game's tonemap shader, the rain), is
	// registered here while On; RescaleRegistered (TAAEffect, right after End: uses LastScale) stretches its drawn part over the
	// whole of it, so those readers see the layout they always did.
	void RescaleAfterUpscale(IDirect3DSurface9* Surface);
	void RescaleRegistered(IDirect3DDevice9* Device);
	void ForgetSurface(IDirect3DSurface9* Surface);	// UNOFFICIAL 2026-11-12 (memleak): a registered buffer is being released: its spare too
}
