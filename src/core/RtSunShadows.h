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
	// UNOFFICIAL 2026-11-16 [Shaders.ShadowsExteriors.RT] RealSun (port of mrfury97's fef7c30ba F2, off by default): every sun shadow follows the
	// real sun (no SunSmoothing steps) while the traced lookup runs; never while the deferred sun fade holds the sun
	bool	RealSun();
	// UNOFFICIAL 2026-11-20: true when RT sun shadows used the helper's RtShadowDenoise (mode 0) this frame
	bool	HelperDenoisedThisFrame();

	// UNOFFICIAL 2026-11-13 ForwardLookup ([Shaders.ShadowsExteriors.RT], on; Shaders\Includes\Shadow.hlsl RT_SUN_FORWARD): Mode 1 through
	// the object / skin / land shaders' own sun lookup. The old Mode 1 switched those lookups off and left the sun to the deferred apply,
	// which scales the finished pixel (ambient and lamp light with the sun's) and which the lamps' mask cancels wherever a lamp reaches:
	// surfaces the trace had in shade came out sunlit. Now Apply packs the traced visibility with the frame's depth into
	// TESR_RtSunForward and the next frame's lookups take it where the depth there is their own point's (else the cascades); the
	// deferred apply keeps only the contact shadows, as it does with the forward lookups running. Off, the forward lookups not compiled
	// ([Shaders.ShadowsExteriors.Main] ForwardShadows off): the old Mode 1.
	void	Initialize();					// RenderManager, before any shader is loaded: RT_SUN_FORWARD for the session (logged)
	bool	ForwardCompiled();				// the shaders hold the lookup (ShaderRecord::BuildDefines)
	bool	ForwardActive();				// Mode 1 runs through the lookup now
	void	RegisterForwardTexture();		// ShadowsExteriorEffect::RegisterTextures: TESR_RtSunForward (once: game shaders keep the pointer)
	D3DXVECTOR4* ForwardConstant(int Index);	// 0-3: TESR_RtSunRowX, RowY, RowW, TESR_RtSunData
	void	UpdateForward(bool Exterior, bool LookupsRun);	// ShadowsExteriorEffect::UpdateConstants, every frame: this frame's constants
}
