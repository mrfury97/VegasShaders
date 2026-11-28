#pragma once
// UNOFFICIAL 2026-11-14 full deferred renderer, phase 3: the distant world writes the G-buffer (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.7,
// results and the bench request: docs/next/FD-LOD-RESULTS.md).
//
// Where the far world is drawn (read in the engine, addresses from the Xbox-PDB decompilation and WallSoGB's map): the far land (LOD land,
// SLS2003 / NVR's TerrainLODTemplate, pass 0x0FE), the far objects (SLS2000 / SLS2031, passes 0x0FB / 0x0C9) and the distant trees (the game's
// DISTTREE, pass 0x236) are NOT in the main opaque list. Their render passes carry accumulation hint 10 (BSShaderProperty::RenderPass +6):
// BSShaderPPLightingProperty::GetRenderPasses_2x 0x00BDF790 gives it to every opaque pass of a mesh flagged LOD_Landscape or LOD_Objects
// (shader flags 2: bits 2 / 4), GetRenderPasses_1x 0x00BDB4A0 the same, and BSDistantTreeShaderProperty::GetRenderPasses 0x00BB73F0 to the
// distant trees' pass. RegisterObject_Standard 0x00B64020 sends hint 10 to the batch renderer's geometry group 0, which
// BSShaderAccumulator::FinishAccumulating_Standard_PreResolveDepth 0x00B65AE0 draws right AFTER the main list (RenderBatches(1, 0x25B) at
// 0x00B65BED, GrassFirst::MainListHook) through BSShaderAccumulator::RenderGeometryGroup(0, false) 0x00B639E0 (-> RenderBatches(1, 0x2F1,
// false, 0)), with the max-anisotropy global 0x011A9608 set to 1 around it; then blended decals and the late grass. So the far world drew after
// CapturedNormals::EndSpan, the deferred passes and LinearScene's encode: no G-buffer, no deferred light ("none" in the census).
//
// [Main.Main.Quality] DeferredDistant (test only; restart for the records, live for the draw order; the deferred renderer's mode 2 turns it on,
// core/SettingForce.h). On at startup:
//  - the far land's pixel shaders (TerrainLODTemplate) are compiled with DISTANT_GBUFFER 1 (they write the three G-buffer targets) and count as
//    G-buffer writers (ShaderRecord.cpp); with LightFreeRecords (mode 2) they get a lighting-free record too (LAMPS_DEFERRED 2), which the
//    deferred renderer binds inside the span (DeferredRenderer::Eligible), and DeferredSun gives them their sun;
//  - RenderGeometryGroup is detoured (Hooks.cpp), and while the key is on GrassFirst::MainListHook draws group 0 ITSELF at the main list's end,
//    still inside the G-buffer span and LinearScene's span, before DeferredDepth's copy and the deferred passes (DrawEarly). The game's own
//    group-0 call that follows in the same accumulator is then skipped (the lists are used up anyway; skipped so nothing can draw twice).
//    The far objects already use NVR's Object records, so they write the G-buffer once the targets are bound; the distant trees (the game's
//    shader) and anything else non-NVR in the group stay forward (masked off by CapturedNormals::BeforeDraw), drawn at their depth over it;
//  - LinearScene: the group's draws convert the frame as any draw in its span does; if the frame was linear when the main list ended and the
//    group left it in the stored curve (the distant trees), it is converted back once, so the deferred passes find the state the main list
//    left (DeferredSun needs it linear);
//  - DeferredLamps (and DeferredSun) take the pixel's position from DeferredDepth's exact linear view z where it agrees with the G-buffer's
//    half-float distance (DL_LINEAR_DEPTH / DS_DEPTH_AGREE, DeferredLamps.hlsl / DeferredSun.hlsl): the far band is exact; where something
//    that writes no G-buffer was drawn nearer (hair, the distant trees) the G-buffer's own distance is kept, so its depth test still refuses
//    that pixel.
// Off (default): nothing is detoured, compiled or drawn differently; every template and record as before.
#include "GpuProfiler.h"
#include "CapturedNormals.h"
#include "LinearScene.h"
#include "MergeCopies.h"
#include "GBufferCensus.h"

namespace DeferredDistant {
	inline const char* kSection = "Main.Main.Quality";
	inline const UInt32 kRenderGeometryGroup = 0x00B639E0;	// BSShaderAccumulator::RenderGeometryGroup(UInt32 group, bool) (thiscall)
	inline const UInt32 kMaxAnisotropy = 0x011A9608;		// the global FinishAccumulating_Standard_PreResolveDepth sets to 1 around group 0

	typedef void(__thiscall* RenderGeometryGroupFn)(void* accumulator, UInt32 group, bool flag);
	inline RenderGeometryGroupFn RenderGeometryGroup = (RenderGeometryGroupFn)kRenderGeometryGroup;	// (the trampoline once detoured)

	inline int Wanted = -1;
	inline bool Installed = false;
	// The switch at startup (SettingForce's overlay applies: the deferred renderer's mode 2 reads it as on).
	inline bool WantedAtStartup() {
		return true;
	}
	// The far land's records are compiled with the G-buffer outputs (ShaderRecord::BuildDefines: DISTANT_GBUFFER for TerrainLODTemplate).
	inline bool CompiledIn() { return WantedAtStartup() && CapturedNormals::CompiledIn(); }

	// the early draw of this frame: the accumulator whose own group-0 call is to be skipped
	inline void* SkipAccumulator = nullptr;
	inline UInt32 SkipFrame = 0xFFFFFFFFu;
	inline bool ComputeHandlesStoredCurve = false;
	inline UInt32 FrameNow() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }

	// F10 line
	inline unsigned StatFrames = 0, StatLastFrame = 0xFFFFFFFFu;
	inline unsigned long long StatEarly = 0, StatSkipped = 0, StatRefused = 0, StatRelinear = 0, StatGameCalls = 0;

	// UNOFFICIAL 2026-11-24 fd-complete (docs/next/FD-COMPLETE.md, decals): the game's RenderDecals (xFNV BSShaderAccumulator::RenderDecals,
	// called by FinishAccumulating right after the main list and group 0) draws geometry groups 2 (static decals) and 3 (dynamic decals)
	// through this function with the alpha flag, z-write off and a depth bias: after the deferred light passes, so decals kept the game's
	// own few lamps (and, with [Shaders.Flashlight.Deferred] GameLight, the flashlight's game lamp) while the wall under them is lit by
	// every lamp and the deferred flashlight. DeferredTransparents sets this to draw those groups inside its forward+ list scope.
	inline void (*DecalGroupDraw)(void* accumulator, UInt32 group, bool flag) = nullptr;
	inline unsigned long long StatGroupCalls[4] = {};	// calls per group 0-3 (DeferredTransparents' F10 line reports 2 and 3)

	// The detour: the game's group-0 call after an early draw of the same accumulator in this frame is skipped; everything else as the game.
	inline void __fastcall RenderGeometryGroupHook(void* accumulator, void* edx, UInt32 group, bool flag) {
		if (group < 4) StatGroupCalls[group]++;
		if ((group == 2 || group == 3) && DecalGroupDraw) {
			DecalGroupDraw(accumulator, group, flag);
			return;
		}
		if (group == 0) {
			StatGameCalls++;
			if (accumulator == SkipAccumulator && SkipFrame == FrameNow()) {
				SkipAccumulator = nullptr;
				StatSkipped++;
				return;
			}
		}
		RenderGeometryGroup(accumulator, group, flag);
	}

	// Hooks.cpp, inside AttachHooks' Detours transaction: only with the switch on at startup.
	inline void Attach() {
		if (!WantedAtStartup()) return;
		if (!CapturedNormals::CompiledIn()) { Logger::Log("UNOFFICIAL DeferredDistant: on, but CapturedNormals is off: nothing to write into (the far world as before)"); return; }
		NoteHookSite(kRenderGeometryGroup, 8, "DeferredDistant");
		const UInt8* code = (const UInt8*)kRenderGeometryGroup;
		const LONG attached = DetourAttach(&(PVOID&)RenderGeometryGroup, (PVOID)&RenderGeometryGroupHook);
		Installed = attached == NO_ERROR;
		Logger::Log("UNOFFICIAL DeferredDistant: RenderGeometryGroup (%08X, first bytes %02X %02X %02X) %s (%ld): the far land, far objects and distant trees "
			"(geometry group 0) are drawn inside the main list's G-buffer span while [Main.Main.Quality] DeferredDistant is on",
			kRenderGeometryGroup, code[0], code[1], code[2], Installed ? "detoured" : "NOT detoured: the far world as before", attached);
	}

	// GrassFirst::MainListHook, right after the game's RenderBatches(1, 0x25B) returned, inside its CaptureSpan and LinearSpan (before the
	// DeferredSpan's passes): geometry group 0 drawn now, as the game would draw it next.
	inline void DrawEarly(void* accumulator, IDirect3DDevice9* d) {
		if (!Installed || !accumulator) return;
		const UInt32 frame = FrameNow();
		if (frame != StatLastFrame) { StatLastFrame = frame; StatFrames++; }
		const UInt8* a = (const UInt8*)accumulator;
		// the G-buffer span open (the world's main view), the standard render mode (not the depth-only modes 7 / 8), not a water pass
		if (!CapturedNormals::InSpan || *(const UInt32*)(a + 0x19C) != 0 || a[0x164] || a[0x165]) { StatRefused++; return; }
		const bool wasLinear = LinearScene::InSpan && LinearScene::Linear;
		const bool mergeList = MergeCopies::InList;
		MergeCopies::InList = false;	// (its plan is the main list's; the group's passes go to the engine as they are)
		UInt32* aniso = (UInt32*)kMaxAnisotropy;
		const UInt32 savedAniso = *aniso;
		*aniso = 1;
		{
			GBufferCensus::SpanScope census(GBufferCensus::SpanDistant);
			RenderGeometryGroup(accumulator, 0, false);
		}
		*aniso = savedAniso;
		MergeCopies::InList = mergeList;
		SkipAccumulator = accumulator;
		SkipFrame = frame;
		StatEarly++;
		// the deferred passes find the frame in the space the main list left it in (the distant trees are drawn in the stored curve),
		// unless Vulkan compute deferred passes handle stored-curve inputs directly in registers.
		if (!ComputeHandlesStoredCurve && wasLinear && LinearScene::InSpan && LinearScene::Live && !LinearScene::Linear && LinearScene::SpanTarget &&
			LinearScene::Convert(d, LinearScene::SpanTarget, true)) StatRelinear++;
		static bool told = false;
		if (!told) { told = true; Logger::Log("UNOFFICIAL DeferredDistant: geometry group 0 (the far world) drawn inside the main list's G-buffer span (first time; frame %u)", frame); }
	}

	// Once a frame (Render.cpp, with the deferred renderer's BeginFrame): the F10 line.
	inline void BeginFrame() {
		if (!Installed) return;
		if (!GpuTimer::Enabled) { StatFrames = 0; StatEarly = StatSkipped = StatRefused = StatRelinear = StatGameCalls = 0; return; }
		if (StatFrames < 600) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED DISTANT %u frames: geometry group 0 drawn in the G-buffer span %.2f a frame, the game's own group-0 call skipped after it %.2f (game calls %.2f), "
			"not drawn early (no span / another render mode / water) %.2f, frame converted back to linear after it %.2f",
			StatFrames, StatEarly / f, StatSkipped / f, StatGameCalls / f, StatRefused / f, StatRelinear / f);
		StatFrames = 0;
		StatEarly = StatSkipped = StatRefused = StatRelinear = StatGameCalls = 0;
	}
}
