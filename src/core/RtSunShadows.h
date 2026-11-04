#pragma once

// UNOFFICIAL 2026-10-28 (NVR DXVK) RT sun shadows ([Shaders.ShadowsExteriors.RT]; docs\next\FORK-RT-SUN.md): one ray per pixel
// toward the sun against the ray tracing core's scene (RtCore.h: the 64-bit helper), from the depth and normal buffers, its
// visibility multiplied into the sun shadow buffer after the contact shadows (Mode 0, combine: the cascades stay, the rays add what
// the maps miss), or replacing the cascades inside the RT range (Mode 1: the forward cascade lookups off, the deferred apply fades
// the cascades in only where the rays fade out). Off, or the core not running: exactly the old path.

namespace RtSunShadows {
	void	Register();						// once: the core user
	void	Apply(IDirect3DDevice9* Device);	// after the sun contact shadows: the traced visibility into ShadowPassSurface's red
	bool	Replacing();					// Mode 1 is in force (the forward lookups off, the deferred technique, TESR_RtSunReplace)
	int		DepthImage();					// 2026-11-04 the helper's depth / normals images (RtLampShadows shares them), -1 before Setup
	int		NormalsImage();
	bool	Deferred();					// 2026-11-04 the cascades in the deferred apply (forward lookups off): Mode 1, and Mode 0 without KeepForward
	float	ReplaceRange();					// the RT range while Replacing, else 0 (TESR_RtSunReplace.x, SunShadows' flags.w)
	D3DXVECTOR4* ReplaceConstant();			// TESR_RtSunReplace
	// UNOFFICIAL 2026-11-01 (core/Diagnostics.cpp, the REPORT block; a tester's "Mode 1 turns the exterior shadows off entirely"): the
	// state as of the last frame, and the traced visibility texture's surface (null until a frame was traced; the whole surface is
	// the traced size) so the report can measure its mean: 1.000 = every ray missed (nothing in shadow).
	std::string	StatusText();
	IDirect3DSurface9* VisibilitySurface();
	void	RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);	// DebugView 4: the traced visibility as grey, last
	// UNOFFICIAL 2026-10-30 [Shaders.ShadowsExteriors.RT] CloudShadows: true when this frame's Apply put a visibility with the volumetric
	// clouds' shadow in it into the sun shadow buffer: SunFar and the clouds' composite leave the clouds' shadow out this frame
	bool	CloudsApplied();
}
