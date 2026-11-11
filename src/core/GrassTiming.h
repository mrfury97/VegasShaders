#pragma once

// UNOFFICIAL GrassTiming (TEST BUILDS ONLY, 2026-11-09, grass-cache study; docs/next/GRASS-CACHE-STUDY-2026-11-09.md).
// How much CPU time does the game spend on its grass, where and on which thread? The game's own grass functions (placement when
// the player moves: TESTerrainLODManager::UpdateGrass -> UpdateGrassForCell -> CreateGrass; the per-quarter density grid when a
// land attaches: TESObjectLAND::CalculateGrassParams; the grass model's geometry: TallGrassShaderProperty::GetCachedGeometry;
// clean-up: RemoveFromCell; and the per-draw submission: TallGrassShader::SetupGeometryConstants + the two RenderGrass draws)
// are timed on entry and exit, whatever thread calls them. No argument is read or changed: the hook swaps the return address
// for its own exit stub and calls the game's function unchanged (any calling convention).
// [Main.Main.Performance] GrassTiming (test only, restart): 0 off (nothing hooked), 1 = the functions above, 2 = also count each
// tuft (TallGrassShaderProperty::AddGrass: called per tuft, so its hook adds ~0.1 us per tuft to CreateGrass's time).
// Log: a GRASSTIME FRAME line for every frame with placement work, a GRASSTIME SHOT line per shot of a shot list (frames, hitch
// time, placement on the main thread, the worst frame), a GRASSTIME AVG line every 600 frames (per-frame submission cost).
#include "TestTools.h"

namespace GrassTiming {
#if NVR_TEST_TOOLS
	enum Fn : int { fUpdateGrass, fForCell, fCreateGrass, fCalcParams, fCachedGeom, fRemoveFromCell, fSetupGeomConst, fRenderStrips, fRenderShape, fAddGrass, kFns };
	static const char* const Names[kFns] = { "UpdateGrass", "UpdateGrassForCell", "CreateGrass", "CalculateGrassParams", "GetCachedGeometry",
		"RemoveFromCell", "SetupGeometryConstants", "RenderGrass(strips)", "RenderGrass(shape)", "AddGrass" };
	static const DWORD Address[kFns] = { 0x0057D0A0, 0x0057D4F0, 0x0057DE00, 0x0053BC10, 0x00B61980, 0x00B61570, 0x00BAC9C0, 0x00C1A500, 0x00C1A6A0, 0x00B62DE0 };

	inline int Mode = 0;
	inline PVOID Real[kFns] = {};
	inline DWORD MainThread = 0;
	inline DWORD TlsSlot = TLS_OUT_OF_INDEXES;
	inline double TickMs = 0.0;	// ms per QPC tick

	// per function, main thread / other threads: calls and ticks since the last frame (atomics: other threads add too)
	inline volatile LONG Calls[kFns][2] = {};
	inline volatile LONG64 Ticks[kFns][2] = {};

	struct Entry { DWORD ret; int fn; LONGLONG t0; };
	struct ThreadStack { Entry e[64]; int n; };

	inline LONGLONG Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

	// Called from the exit stub: books the time, returns the caller's real return address.
	inline DWORD __stdcall OnLeave() {
		const LONGLONG t1 = Qpc();
		ThreadStack* s = (ThreadStack*)TlsGetValue(TlsSlot);
		Entry& e = s->e[--s->n];
		const int side = GetCurrentThreadId() == MainThread ? 0 : 1;
		InterlockedIncrement(&Calls[e.fn][side]);
		InterlockedAdd64(&Ticks[e.fn][side], t1 - e.t0);
		return e.ret;
	}

	// The exit stub every timed function returns to.
	__declspec(naked) inline void ExitStub() {
		__asm {
			push eax	// slot for the real return address
			push eax	// the function's results (eax:edx; x87 st0 is left alone: Leave uses integers only)
			push edx
			call OnLeave
			mov [esp + 8], eax
			pop edx
			pop eax
			ret
		}
	}

	// Called on the hook's entry with the slot holding the caller's return address: keeps it, returns where to jump (the game's function).
	inline DWORD __stdcall OnEnter(int fn, DWORD* retSlot) {
		ThreadStack* s = (ThreadStack*)TlsGetValue(TlsSlot);
		if (!s) { s = (ThreadStack*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadStack)); TlsSetValue(TlsSlot, s); }
		if (s && s->n < 64) {
			Entry& e = s->e[s->n++];
			e.ret = *retSlot; e.fn = fn; e.t0 = Qpc();
			*retSlot = (DWORD)&ExitStub;
		}
		else InterlockedIncrement(&Calls[fn][GetCurrentThreadId() == MainThread ? 0 : 1]);	// too deep: counted, not timed
		return (DWORD)Real[fn];
	}

	__declspec(naked) inline void Common() {
		__asm {
			// stack: [esp] fn, [esp+4] caller's return address, then the arguments
			push ecx
			push edx
			push eax
			lea eax, [esp + 16]	// the return address slot
			push eax
			push dword ptr [esp + 16]	// fn
			call OnEnter
			mov [esp + 12], eax	// fn's slot becomes the jump target
			pop eax
			pop edx
			pop ecx
			ret	// to the game's function (via its trampoline), with the exit stub as its return address
		}
	}

	// One stub per function: pushes its number (a literal: MSVC's inline assembler reads a template parameter as a memory operand).
#define GRASSTIMING_HOOK(n) __declspec(naked) inline void Hook##n() { __asm { push n } __asm { jmp Common } }
	GRASSTIMING_HOOK(0) GRASSTIMING_HOOK(1) GRASSTIMING_HOOK(2) GRASSTIMING_HOOK(3) GRASSTIMING_HOOK(4)
	GRASSTIMING_HOOK(5) GRASSTIMING_HOOK(6) GRASSTIMING_HOOK(7) GRASSTIMING_HOOK(8) GRASSTIMING_HOOK(9)
#undef GRASSTIMING_HOOK
	inline PVOID HookOf(int fn) {
		static const PVOID hooks[kFns] = { (PVOID)&Hook0, (PVOID)&Hook1, (PVOID)&Hook2, (PVOID)&Hook3, (PVOID)&Hook4,
			(PVOID)&Hook5, (PVOID)&Hook6, (PVOID)&Hook7, (PVOID)&Hook8, (PVOID)&Hook9 };
		return fn >= 0 && fn < kFns ? hooks[fn] : nullptr;
	}

	// Frame bookkeeping (main thread, from the render hook)
	inline LONGLONG LastFrame = 0;
	inline unsigned long long Frame = 0;
	inline int LastShot = -2;
	struct ShotSum { unsigned frames, hitchFrames, placeFrames; double ms, hitchMs, placeMs, placeMaxMs, placeInHitchMs, calcMs, otherMs, geomMs; };
	inline ShotSum Shot = {};
	inline double AvgSubmit = 0.0, AvgSubmitCalls = 0.0, AvgUpdate = 0.0, AvgAll = 0.0; inline unsigned AvgN = 0;

	inline void Install() {
		Mode = TheSettingManager->GetSettingI("Main.Main.Performance", "GrassTiming");
		if (Mode <= 0) return;
		MainThread = GetCurrentThreadId();
		TlsSlot = TlsAlloc();
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); TickMs = 1000.0 / (double)f.QuadPart;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		LONG result[kFns] = {};
		for (int i = 0; i < kFns; i++) {
			if (i == fAddGrass && Mode < 2) continue;
			Real[i] = (PVOID)Address[i];
			result[i] = DetourAttach(&Real[i], HookOf(i));
		}
		const LONG commit = DetourTransactionCommit();
		Logger::Log("UNOFFICIAL GRASSTIME installed (mode %i, main thread %lu): commit %ld; attach %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld", Mode, MainThread, commit,
			result[0], result[1], result[2], result[3], result[4], result[5], result[6], result[7], result[8], result[9]);
	}

	inline void TakeSnapshot(LONG calls[kFns][2], double ms[kFns][2]) {
		for (int i = 0; i < kFns; i++) for (int s = 0; s < 2; s++) {
			calls[i][s] = InterlockedExchange(&Calls[i][s], 0);
			ms[i][s] = (double)InterlockedExchange64(&Ticks[i][s], 0) * TickMs;
		}
	}

	inline void WriteShot(int shot) {
		if (Shot.frames)
			Logger::Log("UNOFFICIAL GRASSTIME SHOT %i: %u frames %.0f ms | hitch frames (>50 ms) %u, hitch %.0f ms (sum of frame - 16.7) | placement on the main thread "
				"%.1f ms in %u frames (max %.1f ms in one frame), of it in hitch frames %.1f ms | CalculateGrassParams %.1f ms | placement on other threads %.1f ms | "
				"GetCachedGeometry %.1f ms", shot, Shot.frames, Shot.ms, Shot.hitchFrames, Shot.hitchMs, Shot.placeMs, Shot.placeFrames, Shot.placeMaxMs, Shot.placeInHitchMs,
				Shot.calcMs, Shot.otherMs, Shot.geomMs);
		Shot = {};
	}

	// Once per frame at the start of the render hook: the work since the previous call belongs to the frame that is ending.
	inline void OnFrame(int shot) {
		if (Mode <= 0) return;
		MainThread = GetCurrentThreadId();	// the render hook's thread = the game's main thread
		const LONGLONG now = Qpc();
		const double frameMs = LastFrame ? (double)(now - LastFrame) * TickMs : 0.0;
		LastFrame = now; Frame++;
		LONG c[kFns][2]; double ms[kFns][2];
		TakeSnapshot(c, ms);
		if (shot != LastShot) { if (LastShot >= -1) WriteShot(LastShot); LastShot = shot; }
		if (frameMs <= 0.0 || frameMs > 5000.0) return;	// loading screens etc.
		const double place = ms[fUpdateGrass][0] + ms[fCalcParams][0];	// UpdateGrass includes ForCell and CreateGrass
		const double submit = ms[fSetupGeomConst][0] + ms[fRenderStrips][0] + ms[fRenderShape][0];
		Shot.frames++; Shot.ms += frameMs;
		const bool hitch = frameMs > 50.0;
		if (hitch) { Shot.hitchFrames++; Shot.hitchMs += frameMs - 16.7; }
		Shot.placeMs += place; Shot.placeMaxMs = (std::max)(Shot.placeMaxMs, place); if (hitch) Shot.placeInHitchMs += place;
		if (place > 0.5) Shot.placeFrames++;
		Shot.calcMs += ms[fCalcParams][0] + ms[fCalcParams][1];
		Shot.otherMs += ms[fUpdateGrass][1] + ms[fCalcParams][1];
		Shot.geomMs += ms[fCachedGeom][0] + ms[fCachedGeom][1];
		AvgSubmit += submit; AvgSubmitCalls += c[fRenderStrips][0] + c[fRenderShape][0]; AvgUpdate += ms[fUpdateGrass][0]; AvgN++;
		if (c[fCreateGrass][0] + c[fCreateGrass][1] + c[fCalcParams][0] + c[fCalcParams][1] + c[fCachedGeom][0] + c[fCachedGeom][1] + c[fRemoveFromCell][0] > 0 || place > 1.0)
			Logger::Log("UNOFFICIAL GRASSTIME FRAME %llu shot %i: frame %.1f ms | UpdateGrass %.2f ms (%li) > ForCell %.2f (%li) > CreateGrass %.2f (%li)%s | "
				"CalculateGrassParams main %.2f (%li) other %.2f (%li) | GetCachedGeometry %.2f (%li) other %.2f (%li) | RemoveFromCell %.2f (%li) | "
				"tufts added %li | submit %.2f ms (%li draws)",
				Frame, shot, frameMs, ms[fUpdateGrass][0], c[fUpdateGrass][0], ms[fForCell][0], c[fForCell][0], ms[fCreateGrass][0], c[fCreateGrass][0],
				(c[fUpdateGrass][1] || c[fCreateGrass][1]) ? " (+ OTHER THREAD)" : "",
				ms[fCalcParams][0], c[fCalcParams][0], ms[fCalcParams][1], c[fCalcParams][1], ms[fCachedGeom][0], c[fCachedGeom][0], ms[fCachedGeom][1], c[fCachedGeom][1],
				ms[fRemoveFromCell][0], c[fRemoveFromCell][0], c[fAddGrass][0] + c[fAddGrass][1], submit, c[fRenderStrips][0] + c[fRenderShape][0]);
		if (AvgN >= 600) {
			Logger::Log("UNOFFICIAL GRASSTIME AVG (600 frames): grass submission on the CPU %.3f ms per frame (%.1f grass draws: SetupGeometryConstants + RenderGrass, "
				"incl. the Direct3D calls) | UpdateGrass %.3f ms per frame", AvgSubmit / AvgN, AvgSubmitCalls / AvgN, AvgUpdate / AvgN);
			AvgSubmit = AvgSubmitCalls = AvgUpdate = 0.0; AvgN = 0;
		}
	}
#else
	inline void Install() {}
	inline void OnFrame(int) {}
#endif
}

