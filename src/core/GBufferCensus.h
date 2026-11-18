#pragma once

// UNOFFICIAL 2026-11-14 GBufferCensus ([Main.Main.Misc] GBufferCensus, test only, live; default 0 = off). The full deferred renderer's
// phase 0 "exterior census" and, later, its per-frame coverage counter (docs/next/FULL-DEFERRED-RENDERER-PLAN.md sections 4.7 and 6;
// results and the log format: docs/next/FD-CENSUS-RESULTS.md; parser: tools/fd/census_parse.py).
//
// On: one frame every [Main.Main.Misc] GBufferCensusEvery seconds is a census frame. Every draw of that frame (DrawPrimitive,
// DrawIndexedPrimitive and their UP forms, seen through CapturedNormals' device hooks, which the census puts in when it is on) is
// grouped by: the scene context (world, reflections, first person, other), the stretch of the frame it is drawn in (the main opaque
// list RenderBatches(1, 0x25B), GrassFirst's early grass, the late grass list, LampsDepthFirst, the sorted blended list, or none of
// these), the game's pass ID of the RenderPassImmediately call around it (none outside one), the render target (the main view's size
// or not), the pixel shader (named from the pass binds: the game's or this mod's, with its template family), blend, depth write and
// what the draw wrote to the G-buffer (read from the device after CapturedNormals::BeforeDraw: its targets bound with their write
// masks), with the reason when it wrote none. GBufferCensus = 2 adds an occlusion query around every world / first-person draw into
// the main-size target: the samples that passed the depth test (shaded pixels at that moment, overdraw included; MSAA counts
// samples). The block is logged when the queries are back (a few frames later): "GBC BEGIN", one "GBC D" line per group, "GBC END".
// Off: nothing is hooked or counted; the hooks below cost one flag test.
//
// UNOFFICIAL 2026-11-14 merge census ([Main.Main.Misc] GBufferCensusMerge = 1 with GBufferCensus 1 or 2; test only, live; plan section
// 4.9, docs/next/FD-BATCH-RESULTS.md): the batch-renderer question. Every draw of the census frame's main opaque list (world context,
// main-size target) is also written as one "GBC I" line: its game pass (ID, first-pass flag, lamp count and the lamp set as a hash),
// and as hashes its mesh (vertex streams, index buffer, declaration, the draw call's arguments), its shaders, its textures, its blend /
// depth / cull / sampler state and its constants (vertex constants; pixel constants without and with the lamp registers), only the
// registers the bound shaders read (their constant tables). "GBC V" lines say which constant registers differ between copies of one
// mesh. tools/fd/census_parse.py groups the I lines into mergeable draws under the rules of FD-BATCH-RESULTS.md.
#include <d3d9.h>

namespace GBufferCensus {
	// (SpanDistant: UNOFFICIAL 2026-11-14 DeferredDistant, core/DeferredDistant.h: the game's geometry group 0, the far world, drawn in the G-buffer span)
	enum Span : unsigned char { SpanNone, SpanMain, SpanGrassEarly, SpanGrassLate, SpanDepthFirst, SpanBlended, SpanDistant, SpanCount };

	inline int Mode = 0;				// the key: 0 off, 1 draws, 2 draws + pixel counts
	inline bool Active = false;			// this frame is a census frame: every draw is recorded
	inline unsigned char CurrentSpan = SpanNone;
	inline UInt32 CurrentPass = 0xFFFF;	// the pass ID of the RenderPassImmediately call running now (0xFFFF: none)
	inline UInt32 Passes = 0;			// RenderPassImmediately calls of the census frame (the lost-hook check)
	inline const void* CurrentRenderPass = nullptr;	// the BSShaderProperty::RenderPass of that call (nullptr: none or not known)
	inline int (*ContextOf)() = nullptr;	// Render.cpp: ShaderSplit::CurrentContext (0 world, 1 reflections, 2 first person, 3 other, 4 extra camera)
	inline void (*EnsureHooks)(IDirect3DDevice9*) = nullptr;	// Render.cpp: LinearScene's, then CapturedNormals' EnsureHooked (native D3D9 puts its table back)

	// Render.cpp, once a frame before the world is drawn: ends the last census (logs it when its queries are back) and decides whether
	// this frame is one. True while the key is on (the caller puts CapturedNormals' draw hooks in).
	bool BeginFrame(IDirect3DDevice9* d);
	// CapturedNormals' draw hooks, census frames only: Before after its BeforeDraw (the state the draw is made with), After after it.
	// kind: 0 DrawPrimitive, 1 DrawIndexedPrimitive, 2 / 3 their UP forms; a: the call's arguments (the merge census's mesh key).
	enum DrawKind : unsigned char { KindDP, KindDIP, KindDPUP, KindDIPUP };
	void Before(IDirect3DDevice9* d, unsigned char kind, UInt32 a0, UInt32 a1, UInt32 a2, UInt32 a3 = 0, UInt32 a4 = 0, UInt32 a5 = 0);
	void After(IDirect3DDevice9* d);
	// SetShadersHook: the bound pass's pixel shader by name (while the key is on).
	void OnBind(IDirect3DPixelShader9* handle, const char* name, bool nvr);
	// UNOFFICIAL 2026-11-14 MergeCopies (core/MergeCopies.h) self-check: the float constant registers a shader reads (bit r of bits[r / 32]),
	// from its constant table or its instructions, cached per shader. False when not known (then every register is marked).
	bool RegistersRead(IDirect3DVertexShader9* vs, UInt32 bits[8]);
	bool RegistersRead(IDirect3DPixelShader9* ps, UInt32 bits[8]);

	struct SpanScope {
		unsigned char was;
		explicit SpanScope(unsigned char span) : was(CurrentSpan) { CurrentSpan = span; }
		~SpanScope() { CurrentSpan = was; }
	};
	struct PassScope {
		UInt32 was;
		const void* wasPass;
		explicit PassScope(UInt32 pass, const void* renderPass = nullptr) : was(CurrentPass), wasPass(CurrentRenderPass) {
			CurrentPass = pass; CurrentRenderPass = renderPass; if (Active) Passes++;
		}
		~PassScope() { CurrentPass = was; CurrentRenderPass = wasPass; }
	};
}
