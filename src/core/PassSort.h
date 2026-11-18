#pragma once

// UNOFFICIAL 2026-11-14 PassSort ([Main.Main.Performance] PassSort, test only, live; default 0 = off). Phase 8 of the full deferred
// renderer plan, its first cheap step (docs/next/FD-BATCH-DESIGN.md section iv; bench request in docs/next/FD-BATCH-RESULTS.md).
//
// The October batch-renderer prototype (branch deep/batchrender, c85526b5, Instancing mode 3; ENGINE-ANSWERS D8) measured -0.15 ms of
// main-list CPU in Freeside (2.20 -> 2.05 ms, 6 windows each, one launch) by sorting pass 0xFC's bucket lists before the main opaque
// list is drawn: the game's order interleaves meshes, so neighbouring draws re-bind streams, indices and textures they had a draw ago.
// This is that sort, ported on its own (no instancing).
//
// Where: GrassFirst::MainListHook, before the game's RenderBatches(1, 0x25B) of the world's main list (ENGINE-ANSWERS D1, safe point 2:
// everything opaque is registered, nothing of it drawn), and before LampsDepthFirst's depth pass reads the same lists.
// What moves: only the RenderPass pointers carried by the list nodes (the nodes themselves stay linked as they are), only inside one
// run of equal first-pass flag (RenderPass +7) of one bucket (BSBatchRenderer pass group +i*0x14+4, 5 buckets) of pass 0xFC: first
// passes stay ahead of add-on passes, nothing crosses a pass ID or a bucket (D1's rules). Order inside a run: the shader in order of
// first use (geometry +0xC0, so BeginPass runs no more often than before), then the mesh (geometry +0xB8, model data), then the
// texture set (BSShaderPPLightingProperty +0xAC: the Xbox atlas's offset, unchecked on PC; a wrong key only sorts worse). Stable.
// Risk: coplanar draws inside one run tie on LEQUAL and may swap which one wins (z-fighting decals on walls); 0xFC is an opaque base
// pass, which the October picture check showed equal (only animated neon / NPCs / clouds differed, as off against off).
// F10 on: a "PASS SORT" line every 240 frames (mode, the main-list hook's CPU time, the sort's own time, entries sorted / moved), so an
// A/B in one launch reads the same clock in both modes.
// Off (0): one settings read per main list and nothing else.
#include <vector>
#include <algorithm>

namespace PassSort {
	inline int Mode = 0;	// 0 off, 1 sort pass 0xFC, 2 (2026-11-14, MergeCopies v2) also every other pass MergeCopies plans
	inline const UInt16 kTargetPass = 0xFC;
	// UNOFFICIAL 2026-11-14 PassSort = 2 (bench batch 2: the census's rule c, "sorted inside a pass bucket", is where the copies are: indoors in
	// the light-only passes AD2 / AD3, outdoors also ADTS / ADT; game order (rule b) leaves a third of them): the same sort, inside the same
	// runs, for 0xFD, the lit ShadowLightShader base passes with fewer than 4 lights (0x95-0xC2, 0xD7-0xFA, 0xFB, 0xFE-0x12B but 0x102).
	// The same rules as for 0xFC (only pointers inside one run of one bucket move; first passes stay ahead); the same z-tie risk (FD-BATCH-DESIGN iv).
	inline bool Wanted(UInt16 id) {
		if (id == kTargetPass) return Mode >= 1;
		if (Mode < 2) return false;
		return id == 0xFD || (id >= 0x95 && id <= 0xC2) || (id >= 0xD7 && id <= 0x12B && id != 0x102);
	}

	struct Node { UInt32 next, prev, pass; };	// BSTPersistentList node: [0] next, [2] the RenderPass
	struct PassIdNode { UInt16 id; UInt16 pad; PassIdNode* next; };
	struct Entry { UInt32* node; void* pass; UInt32 rank, mesh, tex, place; };
	inline std::vector<Entry> Run;
	inline std::vector<UInt32*> RunNodes;
	inline std::vector<UInt32> RunShaders;

	// stats (F10 windows)
	constexpr unsigned kWindow = 240;
	inline unsigned Frames = 0, LastFrame = 0xFFFFFFFF;
	inline LONGLONG ListTicks = 0, SortTicks = 0;
	inline unsigned long long Sorted = 0, Moved = 0, Lists = 0;
	inline int WindowMode = -1;

	inline UInt32 TextureKey(const UInt8* geometry) {
		const UInt8* shade = (const UInt8*)((NiGeometry*)geometry)->GetProperty(NiProperty::PropertyType::kType_Shade);
		if (!shade) return 0;
		const UInt32 vtable = *(const UInt32*)shade;
		if (vtable == 0x10AE0D0 || vtable == 0x10B9910) return *(const UInt32*)(shade + 0xAC);	// BSShaderPPLightingProperty, Lighting30ShaderProperty
		return (UInt32)shade;
	}

	inline void SortRun() {
		const UInt32 n = (UInt32)Run.size();
		if (n < 2) return;
		RunShaders.clear();
		for (UInt32 i = 0; i < n; i++) {
			Entry& e = Run[i];
			const UInt8* geometry = *(const UInt8* const*)e.pass;
			const UInt32 shader = geometry ? *(const UInt32*)(geometry + 0xC0) : 0;
			e.place = i;
			e.rank = (UInt32)(std::find(RunShaders.begin(), RunShaders.end(), shader) - RunShaders.begin());
			if (e.rank == RunShaders.size()) RunShaders.push_back(shader);
			e.mesh = geometry ? *(const UInt32*)(geometry + 0xB8) : 0;
			e.tex = geometry ? TextureKey(geometry) : 0;
		}
		std::stable_sort(Run.begin(), Run.end(), [](const Entry& a, const Entry& b) {
			if (a.rank != b.rank) return a.rank < b.rank;
			if (a.mesh != b.mesh) return a.mesh < b.mesh;
			return a.tex < b.tex;
		});
		RunNodes.resize(n);
		for (UInt32 i = 0; i < n; i++) RunNodes[Run[i].place] = Run[i].node;	// the nodes keep their places; the pointers they carry move
		for (UInt32 i = 0; i < n; i++) { if (Run[i].place != i) Moved++; RunNodes[i][2] = (UInt32)Run[i].pass; }
		Sorted += n;
	}

	// The world accumulator's pass-0xFC bucket lists, sorted in place (both batch renderers when there are two).
	inline void SortLists(void* accumulator) {
		const UInt8* a = (const UInt8*)accumulator;
		const UInt32 count = *(const UInt32*)(a + 0x18C);
		for (UInt32 r = 0; r < count && r < 2; r++) {
			UInt8* renderer = *(UInt8* const*)(a + 0x174 + 4 * r);
			if (!renderer) continue;
			PassIdNode* list = (PassIdNode*)(renderer + 0x2C);
			if (!list->next && !list->id) continue;
			for (PassIdNode* p = list; p; p = p->next) {
				if (!Wanted(p->id)) continue;
				UInt32 index = 0;
				if (!(UInt8)ThisCall(0x00853130, renderer + 0x0C, (UInt32)p->id, &index)) continue;	// NiTMapBase<UInt32, UInt32>::GetAt
				UInt8* group = *(UInt8**)(renderer + 8) + index * 0x68;
				for (unsigned b = 0; b < 5; b++) {
					Run.clear();
					int flag = -1;
					for (UInt32* node = *(UInt32**)(group + b * 0x14 + 4); node; node = (UInt32*)node[0]) {
						void* pass = (void*)node[2];
						const int f = pass ? ((const UInt8*)pass)[7] : -2;
						if (f != flag) { SortRun(); Run.clear(); flag = f; }
						if (pass) Run.push_back({ node, pass, 0, 0, 0, 0 });
						else { SortRun(); Run.clear(); flag = -3; }
					}
					SortRun();
				}
			}
		}
		Lists++;
	}

	inline void Report() {
		if (!Frames) return;
		LARGE_INTEGER f; QueryPerformanceFrequency(&f);
		const double ms = 1000.0 / (double)f.QuadPart / Frames;
		Logger::Log("PASS SORT mode %d, %u frames, per frame: main list hook %.3f ms (of it sorting %.3f ms); lists %.2f, pass-0x%X entries sorted %.0f, moved %.0f",
			WindowMode, Frames, ListTicks * ms, SortTicks * ms, (double)Lists / Frames, (unsigned)kTargetPass, (double)Sorted / Frames, (double)Moved / Frames);
		Frames = 0; ListTicks = SortTicks = 0; Sorted = Moved = Lists = 0;
	}

	// GrassFirst::MainListHook, first thing: the world's main list (RenderBatches(1, 0x25B), no geometry group). Sorts when on; times
	// the hook while F10 is on (both modes).
	struct ListScope {
		LARGE_INTEGER start = {};
		bool timed = false;
		ListScope(void* accumulator, UInt32 first, UInt32 last, SInt32 group, bool world) {
			if (first != 1 || last != 0x25B || group >= 0 || !world) return;
			Mode = TheSettingManager->GetSettingI("Main.Main.Performance", "PassSort");
			const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
			if (GpuTimer::Enabled) {
				if (Mode != WindowMode) { Report(); WindowMode = Mode; }
				if (frame != LastFrame) { LastFrame = frame; if (Frames >= kWindow) Report(); Frames++; }
				timed = true;
				QueryPerformanceCounter(&start);
			}
			const UInt8* a = (const UInt8*)accumulator;
			if ((Mode != 1 && Mode != 2) || *(const UInt32*)(a + 0x19C) != 0 || a[0x164] || a[0x165]) return;	// standard render mode, not the water passes
			if (timed) {
				LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
				SortLists(accumulator);
				LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
				SortTicks += t1.QuadPart - t0.QuadPart;
			}
			else SortLists(accumulator);
		}
		~ListScope() {
			if (!timed) return;
			LARGE_INTEGER end; QueryPerformanceCounter(&end);
			ListTicks += end.QuadPart - start.QuadPart;
		}
	};
}
