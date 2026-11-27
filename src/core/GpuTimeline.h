#pragma once

#include <cfloat>
#include <vector>
#include "GpuProfiler.h"

// Charges each interval between successive timestamps to the key that was current during it:
// ticks[i + 1] - ticks[i] goes to keys[i] (the last key has no end and is not charged). Keys equal to
// noKey, or outside [0, keyCount), are not charged. Returns false, charging nothing, if the ticks run
// backwards (a broken or disjoint clock).
inline bool ChargeTimelineIntervals(const UINT64* ticks, const unsigned char* keys, unsigned count,
	double msPerTick, double* sums, unsigned keyCount, unsigned char noKey) {
	for (unsigned i = 0; i + 1 < count; ++i)
		if (ticks[i + 1] < ticks[i]) return false;
	for (unsigned i = 0; i + 1 < count; ++i) {
		if (keys[i] == noKey || keys[i] >= keyCount) continue;
		sums[keys[i]] += (double)(ticks[i + 1] - ticks[i]) * msPerTick;
	}
	return true;
}

// Splits a frame's GPU time by a small "key" (for the F10 profiler: which shader family is drawing)
// with ONE timestamp per key change instead of a begin/end pair per interval. Mark(key) records the
// moment the key changes; the GPU time up to the next mark is charged to that key. Per-frame sums are
// averaged over ReportFrames completed frames and handed to OnReport.
//
// Timestamps sit in the command stream, so an interval also contains any time the GPU spent idle,
// waiting for the CPU, inside it. The split is exact for a GPU-bound frame and a relative measure
// otherwise. Like GpuTimer, results are read without D3DGETDATA_FLUSH and a frame whose results are
// not back yet is simply collected later; a frame is skipped when all slots are still in flight.
class GpuTimeline {
public:
	static constexpr unsigned KeyCount = 252;
	static constexpr unsigned char NoKey = 255;
	static constexpr unsigned FramesInFlight = 6;
	static constexpr unsigned MaxMarks = 1024;	// per frame; a frame with more is dropped (counted)
	typedef void (*ReportFn)(const GpuTimeline& timeline);

	explicit GpuTimeline(const char* name, unsigned reportFrames = 120)
		: Name(name), ReportFrames(reportFrames) { ResetWindow(); }
	~GpuTimeline() { ReleaseQueries(); }

	bool BeginFrame(IDirect3DDevice9* device) {
		if (!device || Current) return false;
#ifndef NVR_GPU_PROFILER_TEST
		if (!GpuTimer::Enabled || InterfaceManager->IsActive(Menu::kMenuType_Loading)) return false;
#endif
		if (device != Device) {
			ReleaseQueries();
			Device = device;
			Unavailable = false;
		}
		if (Unavailable) return false;
		Collect();
		if (Unavailable) return false;
		for (auto& frame : Frames) {
			if (frame.Pending) continue;
			if (!frame.Disjoint && !DisjointBroken && FAILED(Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &frame.Disjoint))) {
				frame.Disjoint = nullptr;
				DisjointBroken = true;
			}
			if (!frame.Frequency && !FrequencyBroken && FAILED(Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &frame.Frequency))) {
				frame.Frequency = nullptr;
				FrequencyBroken = true;
			}
			if (frame.Disjoint && !DisjointBroken) frame.Disjoint->Issue(D3DISSUE_BEGIN);
			frame.Used = 0;
			frame.Overflow = false;
			Current = &frame;
			return true;
		}
		return false; // the GPU is FramesInFlight frames behind: skip this one
	}

	// No-op outside a frame, so callers need not check.
	void Mark(unsigned char key) {
		if (!Current || Current->Overflow) return;
		Frame& frame = *Current;
		if (frame.Used == frame.Marks.size()) {
			if (frame.Used >= MaxMarks) { frame.Overflow = true; return; }
			IDirect3DQuery9* query = nullptr;
			const HRESULT hr = Device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &query);
			if (FAILED(hr)) {
				Fail("could not create a timestamp query", hr);
				return;
			}
			frame.Marks.push_back(query);
			frame.Keys.push_back(NoKey);
		}
		if (FAILED(frame.Marks[frame.Used]->Issue(D3DISSUE_END))) { frame.Overflow = true; return; }
		frame.Keys[frame.Used++] = key;
	}

	void EndFrame() {
		if (!Current) return;
		Frame& frame = *Current;
		Current = nullptr;
		if (frame.Frequency && !FrequencyBroken) frame.Frequency->Issue(D3DISSUE_END);
		if (frame.Disjoint && !DisjointBroken) frame.Disjoint->Issue(D3DISSUE_END);
		if (frame.Overflow) { Dropped++; return; }
		if (frame.Used >= 2) frame.Pending = true;
	}

	bool InFrame() const { return Current != nullptr; }

	// Window results, meaningful inside OnReport.
	const char* GetName() const { return Name; }
	unsigned WindowFrames() const { return WindowCount; }
	double AverageMs(unsigned key) const { return key < KeyCount && WindowCount ? Sum[key] / WindowCount : 0.0; }
	double MaxMs(unsigned key) const { return key < KeyCount ? Max[key] : 0.0; }
	unsigned DroppedFrames() const { return Dropped; }		// more than MaxMarks changes in a frame
	unsigned RejectedFrames() const { return Rejected; }	// disjoint clock or ticks out of order
	unsigned MostMarks() const { return MostUsed; }			// largest number of marks in one frame

	ReportFn OnReport = nullptr;

#ifdef NVR_GPU_PROFILER_TEST
	unsigned PendingFrames() const { unsigned n = 0; for (auto& frame : Frames) n += frame.Pending ? 1 : 0; return n; }
	bool IsUnavailable() const { return Unavailable; }
#endif

private:
	struct Frame {
		std::vector<IDirect3DQuery9*> Marks;	// timestamp queries, created as the frame needs them
		std::vector<unsigned char> Keys;		// Keys[i]: key current from Marks[i] to Marks[i + 1]
		unsigned Used = 0;
		IDirect3DQuery9* Frequency = nullptr;	// optional
		IDirect3DQuery9* Disjoint = nullptr;	// optional
		bool Pending = false;
		bool Overflow = false;
	};

	static constexpr UINT64 FallbackFrequency = 1000000000ull; // see GpuTimer
	const char* Name;
	unsigned ReportFrames;
	Frame Frames[FramesInFlight];
	Frame* Current = nullptr;
	IDirect3DDevice9* Device = nullptr; // weak: owned by the renderer
	std::vector<UINT64> Ticks;
	double Sum[KeyCount];
	double Max[KeyCount];
	unsigned WindowCount = 0;
	unsigned Dropped = 0;
	unsigned Rejected = 0;
	unsigned MostUsed = 0;
	bool Unavailable = false;
	bool DisjointBroken = false;
	bool FrequencyBroken = false;

	void ResetWindow() {
		for (unsigned k = 0; k < KeyCount; ++k) Sum[k] = Max[k] = 0.0;
		WindowCount = Dropped = Rejected = MostUsed = 0;
	}

	static void Release(IDirect3DQuery9*& query) {
		if (query) query->Release();
		query = nullptr;
	}

	void ReleaseQueries() {
		Current = nullptr;
		for (auto& frame : Frames) {
			for (auto*& query : frame.Marks) Release(query);
			frame.Marks.clear();
			frame.Keys.clear();
			Release(frame.Frequency);
			Release(frame.Disjoint);
			frame.Used = 0;
			frame.Pending = frame.Overflow = false;
		}
	}

	void Fail(const char* why, HRESULT hr) {
#ifndef NVR_GPU_PROFILER_TEST
		Logger::Log("GPU PROFILE '%s': %s (hr=%08lX); the split is off until the device changes.", Name, why, (unsigned long)hr);
#else
		(void)why; (void)hr;
#endif
		ReleaseQueries();
		Unavailable = true;
	}

	void Collect() {
		for (auto& frame : Frames) {
			if (!frame.Pending) continue;

			BOOL disjoint = FALSE;
			if (frame.Disjoint && !DisjointBroken) {
				const HRESULT hr = frame.Disjoint->GetData(&disjoint, sizeof(disjoint), 0);
				if (hr == S_FALSE) continue;
				if (hr != S_OK) { DisjointBroken = true; disjoint = FALSE; }
			}
			UINT64 frequency = FallbackFrequency;
			if (frame.Frequency && !FrequencyBroken) {
				const HRESULT hr = frame.Frequency->GetData(&frequency, sizeof(frequency), 0);
				if (hr == S_FALSE) continue;
				if (hr != S_OK) { FrequencyBroken = true; frequency = FallbackFrequency; }
			}
			// Last mark first: until it is back there is no point reading the others.
			Ticks.resize(frame.Used);
			bool ready = true;
			for (unsigned i = frame.Used; i-- > 0;) {
				const HRESULT hr = frame.Marks[i]->GetData(&Ticks[i], sizeof(UINT64), 0);
				if (hr == S_FALSE) { ready = false; break; }
				if (FAILED(hr)) { Fail("timestamp GetData failed", hr); return; }
			}
			if (!ready) continue;
			frame.Pending = false;

			double frameMs[KeyCount] = {};
			if (disjoint || !frequency ||
				!ChargeTimelineIntervals(Ticks.data(), frame.Keys.data(), frame.Used, 1000.0 / (double)frequency, frameMs, KeyCount, NoKey)) {
				Rejected++;
				continue;
			}
			for (unsigned k = 0; k < KeyCount; ++k) {
				Sum[k] += frameMs[k];
				if (frameMs[k] > Max[k]) Max[k] = frameMs[k];
			}
			if (frame.Used > MostUsed) MostUsed = frame.Used;
			if (++WindowCount >= ReportFrames) {
				if (OnReport) OnReport(*this);
				ResetWindow();
			}
		}
	}
};
