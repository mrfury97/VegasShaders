#pragma once
// UNOFFICIAL 2026-10-29 SharedDrawState (ROOTPLAN round 4 step 11): the device state CapturedNormals' lean hook tracks from the device's
// own Set calls inside the world's main list (core/CapturedNormals.h), in a header of its own so other per-draw hooks (LinearScene) can
// read it without including CapturedNormals (its includes reach LinearScene.h). Valid only while CapturedNormals keeps it (lean hook on,
// NVR DXVK, inside its span); anywhere else readers ask the device.
#include <d3d9.h>

namespace SharedDrawState {
	struct TrackedState { IDirect3DSurface9* rt[4]; IDirect3DPixelShader9* ps; DWORD colorWrite, blend, src, dst, colorWrite2, colorWrite3, colorWrite1; };	// colorWrite1: UNOFFICIAL 2026-10-31 AlbedoBuffer
	inline TrackedState T = {};
	inline bool Valid = false;
	// UNOFFICIAL 2026-11-04 DrawStateTracking (CapturedNormals.h): ConstantFilter's detours of the device's SetPixelShader body and of the
	// state-block Apply / EndStateBlock / Reset bodies tell the tracker here (null until it is in). Observers: set once, never cleared.
	inline void (*PixelShaderSet)(IDirect3DPixelShader9*) = nullptr;
	inline void (*StateLost)() = nullptr;
	inline bool ObserversLive = false;	// ConstantFilter's detours are in (the tracker relies on them for the pixel shader and state blocks)
	// UNOFFICIAL 2026-11-11 (the "black stain" on reflecting surfaces under FSR RenderScale < 1): ConstantFilter's pixel hook calls
	// PixelWriteAfter (ManyLamps / LandLamps: "the game wrote over our lamp registers, put them back") after EVERY pixel-constant upload,
	// and those two only know which shader the GAME bound last (their Bound flag, set at the game's SetShaders, never cleared). When the
	// world's last bind was a many-lamp shader, the flag stayed up through NVR's own D3D9 effect passes, and the lamp block (c37-c63,
	// c76-c87, c98-c107) went over the constants D3DX had just uploaded for the effect's pixel shader. Reflections' D3D9 composite (the
	// one a RenderScale rect frame draws) keeps its preshader's "DebugView 8" flag in c40.x there: a lamp's position in it made every
	// reflecting pixel return the unset debug colour (0, 0, 0, 0) while SpecularOcclusion was above 0. While an OwnPass is open (an
	// effect's D3D9 draw, core/EffectRecord.cpp Render) the hook leaves the uploads alone: they are the effect's, not the game's.
	inline int OwnPassDepth = 0;
	struct OwnPass { OwnPass() { OwnPassDepth++; } ~OwnPass() { OwnPassDepth--; } };
}
