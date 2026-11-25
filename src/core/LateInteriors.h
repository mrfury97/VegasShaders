#pragma once
// UNOFFICIAL 2026-11-16 fast-restart (docs/next/FAST-RESTART.md): the Interior lamp-shadow records off the start.
//
// Why: a cold start (after an update, or a setting that changes the shaders) compiled every lit object, skin and hair pixel shader's
// Interior record (Cache\Interiors\, the forward lamp-shadow records: about 68 shaders, ~40 % of the start's compile time with the default
// settings, the three slowest 11-12 s each) and the start waited for all of them, although nothing draws with them before the first world
// frame. They are still needed after it, with the deferred renderer on as well: indoors every draw the renderer keeps forward (blended and
// fading objects, the game's lamp and highlight passes of objects drawn forward, skin and hair without their deferred records, the first-
// person model, reflections, menus' 3D views, the frames the renderer refuses) binds them, and the renderer's blended-draw swap takes them
// as the forward partner outdoors too (DeferredRenderer::Load / DeferredSkin::Load: Pairs, DeferredLamps::ForwardOf).
//
// What: while the start's shaders load (ShaderManager::LoadShader, pixel), an eligible shader's lamp-shadow records (the Interior record with
// its levels and everything made from it, then the RT ExteriorForward record; ShaderManager.cpp LoadLampShadowRecords) are put on a list;
// their compiles are queued on the worker threads AFTER everything the start needs (ShaderPrecompile::StartOnce). Tick (Render.cpp, every
// frame) makes them all at once, in load order: at the main menu as soon as the workers are done, and at the latest at the first world frame
// (waiting there for the compiles still running: one log line). The deferred records made at load with the Default record as their forward
// partner get the Interior one then. After that first attach every load makes its records in place, as before.
// Off (the old order, everything at load): [Main.Main.Performance] ParallelShaderCompile off, forward lamp shadows off, DeferredRenderer 1.
#include "ShaderTemplate.h"

namespace LateInteriors {
	bool Wanted();										// the late records are used this session (read once)
	bool Defer(void* pixelShader, const ShaderTemplate& tmpl);	// ShaderManager::LoadShader: true = put on the list (made by Tick later)
	void Forget(void* pixelShader);						// the game freed the shader (NiD3DPixelShaderEx::Free)
	bool Any();											// still waiting
	void Tick(bool gameLoaded);							// every frame (Render.cpp): attach when done, or now when a game is loaded
}

// UNOFFICIAL 2026-11-16 fast-restart, log only, while the F10 profile is on (ShaderRecord.cpp): which draws indoors still bind an Interior
// record, with the deferred renderer live or not (the proof that it cannot be left out when the renderer is on). One line per 240 interior
// frames: binds per frame (in the G-buffer span / after it), blended draws the renderer swapped to one, the busiest shader names.
namespace InteriorRecordCensus {
	void Count(const char* shaderName, bool inSpan);	// NiD3DPixelShaderEx::SetupShader, an Interior record chosen
	void Swap();										// DeferredRenderer::BeforeDraw, a blended draw swapped to an Interior record
	void Frame(bool interior, bool rendererLive);		// Render.cpp, once a frame
}
