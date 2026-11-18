#pragma once

// UNOFFICIAL 2026-11-14 MergeCopies ([Main.Main.Performance] MergeCopies, test only, live; default 0 = off). Phase 8 v1 of the full
// deferred renderer plan (docs/next/FD-BATCH-DESIGN.md sections i-ii; results and the bench request: docs/next/FD-MERGE-RESULTS.md).
//
// What: copies of one mesh in the world's main opaque list, pass 0xFC (the game's "Opt" base pass, most outdoor objects), drawn as ONE
// instanced draw, with the engine's per-entry setup (BSBatchRenderer::RenderPassImmediately_Standard 0x00B98E80: texture stages,
// AddObjectToGroup, Setup_ADT_Opt 0x00B7DDE0, the streams, the draw, PostGeometry) run only for the group's first entry.
// Why 0xFC is exact without the deferred renderer (read in the exe, function names from WallSoGB's map): for a 0xFC entry the engine's
// per-object work is textures (the property's diffuse, normal map, clamp mode), the alpha states and two vertex constants:
// c0-c3 = transpose(world-view x projection) (SetupGeometryOpt_WorldViewProj 0x00B7C750: NiDX9Renderer::SetModelTransform 0x00E6FBB0,
// then D3DXMatrixMultiplyTranspose of the renderer's +0x940 and +0xA00) and c25 = the sun direction (the pass's first lamp, negated)
// through the inverse world matrix (BSShader::SetLightDirection 0x00B7C7B0, BSUtilities::GetInverseWorldMatrix 0x00C4C2D0). Its
// pixel constants are pass level (BeginPass). So copies with one mesh, shader, texture set and alpha state differ only in c0-c3 and
// c25, which this computes per copy with the engine's own math (the same functions and D3DX entry points the exe calls, so the bits
// are the engine's) and feeds through an instance stream to NVR's NVR_INSTANCED copy of the bound vertex shader (ObjectTemplate.hlsl).
//
// Where: the groups are planned at the main list's start (GrassFirst::MainListHook, after PassSort): consecutive entries of one run
// (one bucket without alpha test, first passes) with the same CPU key (geometry kind, model data, shader, property kind, diffuse,
// normal map, clamp mode, shader flags, alpha, fade, NVR's specular fade, alpha property). In RenderPassStandardHook every one of NVR's
// own per-object steps still runs for every copy (ObjectMotion, ReflMaterial, the counters...); only the call into the engine's
// _Standard is skipped for a merged copy. The first entry's draw is caught in the innermost of NVR's DrawIndexedPrimitive hooks
// (CapturedNormals / LinearScene, after their own per-draw work) and sent as one instanced draw; the device is put back after it.
//
// The self-check (what proves the merge exact before anyone looks at pictures): on a check frame (every 8th) a group runs the engine's
// setup for every copy and compares, at the draw, everything the draw would use: the shaders, declaration, streams, indices, draw
// arguments, textures and their sampler states, 36 render states, the targets, every vertex and pixel constant register the bound
// shaders read (their constant tables), against the first entry's; and c0-c3 / c25 bit for bit against what this code computed for
// that copy. Any difference blacklists that key for the session and is logged ("MERGE CHECK mismatch").
//   MergeCopies = 1: self-check only (every group of a check frame; nothing merged; the picture is the game's).
//   MergeCopies = 2: merge. A key is merged only after 3 of its groups passed the check in this session (probation: drawn the plain
//   way and checked); then 1 merged group in 16 on a check frame is checked live: the copies run the engine's setup with their draw
//   suppressed and are compared against what the merged draw used.
// Fallbacks (drawn the normal way): not pass 0xFC, not the world's main list, census frames, LampsDepthFirst or HiddenObjectCulling
// on, skinned or special geometry (only NiTriShape / NiTriStrips with static data), alpha-tested buckets (2-4) or an alpha property
// with blend or test, a fade in progress (property alpha or fade alpha below 1), not a first pass, a vertex shader that is not one of
// NVR's ObjectTemplate records (or whose instanced copy does not compile or moved a register), a pixel shader not in NVR's records,
// a declaration with a second stream, a draw hook not in place, a draw call that did not come, more than one draw in the entry.
// F10 on: a "MERGE COPIES" line every 240 frames: groups, copies merged (= draws saved = setup calls skipped), instanced draws,
// fallbacks, the checks, and the main list hook's CPU ms in this mode (an A/B 0 / 2 in one launch reads the same clock).
// Off (0): one settings read per world main list; the entry hook and the draw hooks test one flag.
//
// UNOFFICIAL 2026-11-14 v2 (after bench batch 2: v1 merged 0-8 copies a frame; indoors there is no pass 0xFC at all, and 65 of Freeside's 79
// 0xFC entries were alpha-tested, which v1 left out). Now: alpha-tested entries are taken (blend still not); pass 0xFD too; and, while
// DeferredRenderer is live, the light-only base passes AD2 / AD3 (0xD7-0xFA: where the census finds the copies indoors) with a deferred
// record bound, and in DeferredRenderer 2 the other lit ShadowLightShader base passes with fewer than 4 lights (ADTS, ADTS2, ADT, ADT2).
// The instance stream carries c0-c3, c16 and c25. Registers the bound record does not read (proved offline from the records' pixel shader
// inputs, see MergeCopies.cpp "Ignore") are left as the first entry's and not compared by the self-check; everything else still is,
// and c0-c3 / c16 / c25 are computed per copy as the game's non-Opt setup computes them where the record reads them. The F10 line counts
// planned and merged copies by kind (opt / ad / lit). The fallbacks above stand, plus: a pass past 0xFC without a deferred record bound.
#include <d3d9.h>

namespace MergeCopies {
	typedef HRESULT(STDMETHODCALLTYPE* DrawFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef void(__cdecl* StandardFn)(void*, UInt32, UInt32, UInt32);

	inline int Mode = 0;				// the key: 0 off, 1 self-check only, 2 merge
	inline bool InList = false;			// inside the world's main list with a plan (the entry hook looks)
	inline unsigned char DrawAction = 0;	// set around the engine's setup of a planned entry: what the innermost draw hook does
	inline void* OurHooks[2] = {};		// CapturedNormals' and LinearScene's DrawIndexedPrimitive hooks (the innermost one acts)
	inline bool Inner(void* next) { return next != OurHooks[0] && next != OurHooks[1]; }
	inline float (*SpecFadeOf)(const void* geometry) = nullptr;	// Render.cpp: NVR's highlight fade for the object (PbrPop::SpecFade, pixel c211.x), a key part
	// UNOFFICIAL 2026-11-14 v2 (batch 2): Render.cpp: DeferredRenderer's mode while it is live this frame (0: not live), and whether a pixel
	// shader is one of its deferred records (the passes past 0xFC merge only while one is bound: see MergeCopies.cpp "v2").
	inline int (*DeferredMode)() = nullptr;
	inline bool (*DeferredPS)(IDirect3DPixelShader9* ps) = nullptr;	// (Render.cpp sets both before the first frame)
	// UNOFFICIAL 2026-11-15 (batch 3: PS c211.w differed between copies of 10 keys, e.g. 3569 vs 2161: NVR's material class + 8 x the world
	// bound radius, Render.cpp MaterialForPass): Render.cpp: the value MaterialForPass will send for this pass (false: it sends none), a key part.
	inline bool (*MaterialWOf)(const void* pass, float& w) = nullptr;

	// Render.cpp, once a frame before the world: reads the key. True while it is on (the caller puts CapturedNormals' draw hooks in).
	bool BeginFrame(IDirect3DDevice9* d);
	// The innermost DrawIndexedPrimitive hook, while DrawAction is set: the merged draw, a check, or a suppressed draw.
	HRESULT AtDraw(IDirect3DDevice9* d, DrawFn next, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT pc);
	// RenderPassStandardHook, in place of the call into the engine's _Standard while InList.
	void Entry(void* pass, UInt32 a2, UInt32 a3, UInt32 a4, StandardFn standard);

	// GrassFirst::MainListHook, right after PassSort's scope: plans the groups of the world's main list and times the hook (F10 on).
	void ListBegin(void* accumulator, bool main, bool world, bool excluded);
	void ListEnd();
	struct ListScope {
		bool on;
		ListScope(void* accumulator, UInt32 first, UInt32 last, SInt32 group, bool world, bool excluded) : on(first == 1 && last == 0x25B && group < 0 && world) {
			if (on) ListBegin(accumulator, true, world, excluded);
		}
		~ListScope() { if (on) ListEnd(); }
	};
}
