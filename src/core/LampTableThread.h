#pragma once
// UNOFFICIAL 2026-11-14 LampTableThread (fd-lampthread; docs/next/FD-LAMPTHREAD-RESULTS.md): LampTableCore::BinFast (the froxel lists: keys,
// sort, binning, write-out) run on FrameWorker's thread instead of the game's main thread. Shared by ManyLamps::BuildTable and
// tests/lamp_table_bench.cpp, so the game and the bench run the SAME hand-off.
// The hand-off: the main thread walks the scene (engine objects: main thread only) into plain data (LampTableCore::SceneLamp, the active
// indices), locks the table, writes row 0, then Start hands the worker this frame's View and pointers to that plain data and to the locked
// rows. Until Finish returns, the job owns: the SceneLamps / active arrays (read), the FastState (read and written) and the list rows of
// the locked texture (written). No engine pointer is dereferenced on the worker (SceneLamp::id is only hashed and compared as a number).
// The main thread calls Finish before anything reads the lists or touches those arrays (the unlock, the next make, the check), and waits
// there only if the worker is not done yet. The job runs with the main thread's floating-point control words (FrameWorker copies them),
// so its results are bit for bit those BinFast gives on the main thread (the bench's fuzz compares them).
// FrameWorker runs one job at a time (SkyOnWorker's sky projection uses it too): Start does not queue behind another job; it returns
// false and the caller bins in place, so the main thread never waits for somebody else's job here.
#include <windows.h>
#include <intrin.h>
#include "LampTableCore.h"
#include "FrameWorker.h"

namespace LampTableThread {
	struct Job {
		LampTableCore::View view;
		const LampTableCore::SceneLamp* table = nullptr;
		const int* active = nullptr;
		int nActive = 0;
		float* lists = nullptr;		// row 1 of the locked table
		int pitch = 0;				// in floats
		uint32_t serial = 0;
		LampTableCore::FastState* state = nullptr;
		LampTableCore::BinStats stats;	// BinFast's counts (this job's alone)
		bool made = false;			// BinFast's return
		double workerMs = 0.0;		// the job's time on the worker
		volatile LONG done = 1;
		bool pending = false;		// started and not finished yet (main thread only)
	};

	inline double NowMs() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return c.QuadPart * 1000.0 / f.QuadPart; }

	inline void Run(void* arg) {
		Job& j = *(Job*)arg;
		const double t0 = NowMs();
		j.made = LampTableCore::BinFast(j.view, j.table, j.active, j.nActive, j.lists, j.pitch, j.serial, *j.state, j.stats);
		_mm_sfence();	// the list rows (write-combined memory on native D3D9) out before the main thread unlocks
		j.workerMs = NowMs() - t0;
		InterlockedExchange(&j.done, 1);
	}

	// Main thread: the job onto the worker. false = the worker holds another job (or the job was not started): bin in place instead.
	inline bool Start(Job& j) {
		if (j.pending) return false;
		if (FrameWorker::Busy && FrameWorker::PendingArg != &j) return false;	// someone else's job: never wait for it here
		j.stats = LampTableCore::BinStats();
		j.made = false;
		j.workerMs = 0.0;
		InterlockedExchange(&j.done, 0);
		j.pending = true;
		FrameWorker::Start(&Run, &j);	// (our own finished job: its Wait is instant; no thread: runs in place)
		return true;
	}

	// Main thread: the job finished (waits if it is still running). Returns the ms the main thread waited (0 when it was done already).
	inline double Finish(Job& j) {
		if (!j.pending) return 0.0;
		j.pending = false;
		if (j.done) {	// done: release FrameWorker's slot if it is still ours (instant), else another job's Start already waited for ours
			if (FrameWorker::Busy && FrameWorker::PendingArg == &j) FrameWorker::Wait();
			return 0.0;
		}
		// not done: FrameWorker's pending job is this one (any other Start would have waited for it first)
		const double t0 = NowMs();
		FrameWorker::Wait();
		return NowMs() - t0;
	}
}
