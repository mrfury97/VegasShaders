#pragma once

// UNOFFICIAL 2026-11-28 (deepprobe-live): the Performance probe's background writer (DeepProbe.h). Every file the probe writes, and
// every log line its sampler and watchdog threads want, goes through this one thread, so neither the main thread nor the sampler
// ever waits on the disk (under MO2, usvfs hooks the file APIs) or on the log's locks:
// - jobs (report files, CSVs, log lines, open/close of frames.csv) go through a lock-free Windows SList and run in posting order;
// - frames.csv: the main thread pushes fixed-size records into a single-producer ring the writer drains every 200 ms; the file is
//   renamed to frames.1.csv at the size cap (one old file kept: bounded disk use). A full ring drops records, never waits.
// The thread starts at the probe's first use and runs until the game exits (Flush waits a bounded time at exit).

#include <windows.h>
#include <malloc.h>
#include <functional>
#include <string>
#include <vector>
#include <stdio.h>
#include <share.h>

namespace DeepProbe { namespace Writer {

	struct FrameRec { UInt32 frame; float ms, scriptMs, probeMs; UInt8 flags; };	// flags: 1 spike, 2 loading
	static constexpr UInt32 kRing = 1u << 14;	// 16384 frames (~4.5 minutes at 60 fps of slack if the disk stalls)

	namespace Detail {
		struct alignas(MEMORY_ALLOCATION_ALIGNMENT) JobNode { SLIST_ENTRY entry; std::function<void()> fn; };

		inline SLIST_HEADER* Jobs() {
			static SLIST_HEADER* head = [] {
				SLIST_HEADER* h = (SLIST_HEADER*)_aligned_malloc(sizeof(SLIST_HEADER), MEMORY_ALLOCATION_ALIGNMENT);
				if (h) InitializeSListHead(h);
				return h;
			}();
			return head;
		}
		inline HANDLE Thread = nullptr, Wake = nullptr;
		inline volatile LONG Pending = 0;		// jobs posted and not yet run (Flush waits for 0; an autosave waits for 0)

		inline FrameRec Ring[kRing];
		inline volatile LONG Head = 0, Tail = 0;	// Head written by the main thread, Tail by the writer
		inline volatile LONG Dropped = 0;
		inline volatile LONG FramesWanted = 0;

		inline std::string FramesPath;
		inline FILE* Frames = nullptr;
		inline long long FramesBytes = 0, FramesCapBytes = 64ll * 1024 * 1024;

		inline void OpenFramesFile() {
			Frames = _fsopen(FramesPath.c_str(), "w", _SH_DENYWR);
			FramesBytes = 0;
			if (Frames) FramesBytes += fprintf(Frames, "frame,ms,script_ms,probe_ms,flags\n");
		}

		inline void DrainFrames() {
			LONG tail = Tail;
			const LONG head = Head;
			MemoryBarrier();
			while (tail != head) {
				const FrameRec& r = Ring[(UInt32)tail & (kRing - 1)];
				if (Frames) {
					FramesBytes += fprintf(Frames, "%u,%.3f,%.3f,%.3f,%u\n", r.frame, r.ms, r.scriptMs, r.probeMs, (unsigned)r.flags);
					if (FramesBytes > FramesCapBytes) {
						fclose(Frames);
						const std::string old = FramesPath.substr(0, FramesPath.size() - 4) + ".1.csv";
						MoveFileExA(FramesPath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
						OpenFramesFile();
					}
				}
				tail++;
			}
			MemoryBarrier();
			Tail = tail;
			if (Frames) fflush(Frames);
		}

		inline void RunJobs() {
			// The SList is last-in first-out: take it whole and run the nodes oldest first.
			PSLIST_ENTRY e = InterlockedFlushSList(Jobs());
			std::vector<JobNode*> nodes;
			for (; e; e = e->Next) nodes.push_back(CONTAINING_RECORD(e, JobNode, entry));
			for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
				DrainFrames();	// frames pushed before a job (a close, a report) are written before it runs
				(*it)->fn();
				(*it)->~JobNode();
				_aligned_free(*it);
				InterlockedDecrement(&Pending);
			}
		}

		inline void (*Poll)() = nullptr;	// DeepProbe's flags from the sampler and watchdog threads, turned into log lines here

		inline DWORD WINAPI ThreadMain(void*) {
			for (;;) {
				WaitForSingleObject(Wake, 200);
				RunJobs();
				DrainFrames();
				if (Poll) Poll();
			}
		}
	}

	inline bool Start() {
		using namespace Detail;
		if (Thread) return true;
		if (!Jobs()) return false;
		Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Thread = CreateThread(nullptr, 256 * 1024, &ThreadMain, nullptr, 0, nullptr);
		if (Thread) SetThreadPriority(Thread, THREAD_PRIORITY_BELOW_NORMAL);
		return Thread != nullptr;
	}

	// Lock-free; any thread except while another thread is suspended by the sampler (it allocates). Runs on the writer thread, in
	// posting order. Before Start (or if the thread could not start) the job runs here.
	inline void Post(std::function<void()> job) {
		using namespace Detail;
		if (!Thread && !Start()) { job(); return; }
		void* mem = _aligned_malloc(sizeof(JobNode), MEMORY_ALLOCATION_ALIGNMENT);
		if (!mem) return;
		JobNode* n = new (mem) JobNode();
		n->fn = std::move(job);
		InterlockedIncrement(&Pending);
		InterlockedPushEntrySList(Jobs(), &n->entry);
		SetEvent(Wake);
	}

	// Waits at most maxMs for the posted jobs to finish (the game's exit only: the final report). Pending drops only after a job
	// has run, so this cannot return early.
	inline void Flush(DWORD maxMs) {
		using namespace Detail;
		if (!Thread) return;
		const ULONGLONG deadline = GetTickCount64() + maxMs;
		while (Pending > 0 && GetTickCount64() < deadline) {
			SetEvent(Wake);
			Sleep(5);
		}
	}

	inline void OpenFrames(const std::string& path, unsigned capMB) {
		Detail::FramesWanted = 1;
		const long long cap = (long long)(capMB < 1 ? 1 : capMB) * 1024 * 1024;
		Post([path, cap]() {
			using namespace Detail;
			if (Frames) fclose(Frames);
			FramesPath = path;
			FramesCapBytes = cap;
			OpenFramesFile();
		});
	}

	inline void CloseFrames() {
		Detail::FramesWanted = 0;
		Post([]() { using namespace Detail; if (Frames) { fclose(Frames); Frames = nullptr; } });
	}

	// Main thread only (single producer), lock-free: a full ring drops the record.
	inline void PushFrame(const FrameRec& rec) {
		using namespace Detail;
		if (!FramesWanted) return;
		const LONG head = Head;
		if ((UInt32)(head - Tail) >= kRing) { InterlockedIncrement(&Dropped); return; }
		Ring[(UInt32)head & (kRing - 1)] = rec;
		MemoryBarrier();
		Head = head + 1;
	}

	inline LONG DroppedFrames() { return Detail::Dropped; }

	// Writes text to path through path.tmp and a rename, so a reader (or a game that exits mid-write) never sees half a file.
	inline bool WriteFileAtomic(const std::string& path, const std::string& text) {
		const std::string tmp = path + ".tmp";
		FILE* f = _fsopen(tmp.c_str(), "wb", _SH_DENYWR);
		if (!f) return false;
		const size_t written = fwrite(text.data(), 1, text.size(), f);
		fclose(f);
		if (written != text.size()) return false;
		return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
	}
} }
