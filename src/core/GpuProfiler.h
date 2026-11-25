#pragma once

#include <algorithm>
#include <cfloat>
#include <vector>
#include "FrameTimeStats.h"
#ifndef NVR_GPU_PROFILER_TEST
#include "GpuDebug.h"	// UNOFFICIAL 2026-10-26: GpuDebugLabels (each GpuProfileScope is also a named group in a capture)
#endif

inline bool GpuProfileQueriesReady(HRESULT start, HRESULT end, HRESULT frequency) {
	return start == S_OK && end == S_OK && frequency == S_OK;
}

// Asynchronous D3D9 GPU timestamps. GetData is deliberately never called with
// D3DGETDATA_FLUSH: profiling must not serialize the CPU and GPU or change the
// workload we are trying to measure.
//
// Compatibility (DXVK and other D3D9 layers): only the two timestamp queries are required. The
// TIMESTAMPDISJOINT and TIMESTAMPFREQ queries are optional -- if a layer lacks them, or their
// GetData starts failing, the timer keeps going (no disjoint check; a 1 GHz clock, which is what
// DXVK reports on NVIDIA) instead of switching itself off. Every failure is logged with its HRESULT.
class GpuTimer {
public:
	explicit GpuTimer(const char* name, unsigned reportSamples = 120)
		: Name(name), ReportSamples(reportSamples) {}

	~GpuTimer() { ReleaseQueries(); }

	bool Begin(IDirect3DDevice9* device) {
		if (!device) return false;
#ifndef NVR_GPU_PROFILER_TEST
		// The diagnostic is armed explicitly after loading. F10 toggles it from
		// ShaderManager; no query objects are created during initial loading.
		if (!Enabled && !ProbeActive || InterfaceManager->IsActive(Menu::kMenuType_Loading)) return false;
#endif
		if (Active) return false;
		if (device != Device) {
			ReleaseQueries();
			Device = device;
			Unavailable = false;
		}
		if (Unavailable || (!QueriesCreated && !CreateQueries())) return false;

		CollectCompleted();
		// Collection can release the entire ring after a device/query error.
		if (Unavailable) return false;
		WarnIfNothingCompletes();
		for (auto& slot : Slots) {
			if (slot.Pending) continue;
			HRESULT hr = slot.Disjoint ? slot.Disjoint->Issue(D3DISSUE_BEGIN) : S_OK;
			if (SUCCEEDED(hr)) hr = slot.Start->Issue(D3DISSUE_END);
			if (FAILED(hr)) {
				Disable("could not issue the start timestamp", hr);
				return false;
			}
			Active = &slot;
			return true;
		}
		return false; // GPU is more than the query-ring depth behind; skip a sample.
	}

	void End() {
		if (!Active) return;
		Slot* slot = Active;
		Active = nullptr;
		HRESULT hr = slot->End->Issue(D3DISSUE_END);
		if (SUCCEEDED(hr) && slot->Frequency) hr = slot->Frequency->Issue(D3DISSUE_END);
		if (SUCCEEDED(hr) && slot->Disjoint) hr = slot->Disjoint->Issue(D3DISSUE_END);
		if (FAILED(hr)) {
			Disable("could not issue the end timestamp", hr);
			return;
		}
		slot->Pending = true;
		Issued++;
	}

#ifdef NVR_GPU_PROFILER_TEST
	unsigned GetSampleCount() const { return TotalSamples; }
	unsigned GetRejectedCount() const { return Rejected; }
	bool UsingFallbackClock() const { return FrequencyBroken; }
	bool DisjointTrusted() const { return !DisjointBroken; }
#endif
	inline static bool Enabled = false;
	inline static bool ProbeActive = false;
	inline static void (*OnSample)(const char* name, double ms) = nullptr;
	// UNOFFICIAL 2026-10-17, test only: told about every GpuProfileScope begin and end while set (the frame recorder's markers,
	// FrameRecorder.h). Null in normal play.
	inline static void (*MarkerHook)(const char* name, bool begin) = nullptr;
	const char* GetName() const { return Name; }

private:
	struct Slot {
		IDirect3DQuery9* Start = nullptr;
		IDirect3DQuery9* End = nullptr;
		IDirect3DQuery9* Frequency = nullptr; // optional
		IDirect3DQuery9* Disjoint = nullptr;  // optional
		bool Pending = false;
	};

	static constexpr unsigned RingSize = 12;
	static constexpr UINT64 FallbackFrequency = 1000000000ull; // DXVK reports timestamps in nanoseconds
	Slot Slots[RingSize];
	IDirect3DDevice9* Device = nullptr; // weak: owned by the renderer
	Slot* Active = nullptr;
	const char* Name;
	unsigned ReportSamples;
	unsigned WindowSamples = 0;
	unsigned TotalSamples = 0;
	unsigned ZeroTickSamples = 0;
	unsigned Issued = 0;      // timestamp pairs handed to the device
	unsigned Rejected = 0;    // completed pairs discarded (disjoint, zero frequency, end before start)
	double SumMs = 0.0;
	double MinMs = DBL_MAX;
	double MaxMs = 0.0;
	std::vector<float> WindowMs; // this window's samples, for the middle value and the spread (p10 / p90)
	bool Unavailable = false;
	bool QueriesCreated = false;
	bool DisjointBroken = false;   // GetData on the disjoint query failed: stop consulting it
	bool FrequencyBroken = false;  // GetData on the frequency query failed / missing: use FallbackFrequency
	bool WarnedNothing = false;
	bool WarnedRejected = false;

	// Failure reports are written to the log unless building the unit test, and never more than a
	// handful per session: a broken layer would otherwise flood the log once per timer per frame.
	void Report(const char* what, HRESULT hr) {
#ifndef NVR_GPU_PROFILER_TEST
		static unsigned reported = 0;
		if (reported++ < 16)
			Logger::Log("GPU PROFILE '%s': %s (hr=%08lX)", Name, what, (unsigned long)hr);
#else
		(void)what; (void)hr;
#endif
	}

	static void Release(IDirect3DQuery9*& query) {
		if (query) query->Release();
		query = nullptr;
	}

	void ReleaseQueries() {
		Active = nullptr;
		for (auto& slot : Slots) {
			Release(slot.Start);
			Release(slot.End);
			Release(slot.Frequency);
			Release(slot.Disjoint);
			slot.Pending = false;
		}
		QueriesCreated = false;
	}

	bool CreateQueries() {
		for (auto& slot : Slots) {
			slot.Start = slot.End = slot.Frequency = slot.Disjoint = nullptr;
			HRESULT hr = Device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.Start);
			if (SUCCEEDED(hr)) hr = Device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.End);
			if (FAILED(hr)) {
#ifndef NVR_GPU_PROFILER_TEST
				static bool reported = false;
				if (!reported) {
					Logger::Log("GPU PROFILE unavailable: this D3D9 device does not support timestamp queries (hr=%08lX).", (unsigned long)hr);
					reported = true;
				}
#endif
				Disable(nullptr, hr);
				return false;
			}
			// Optional: without them the timer still works (see the class comment).
			if (FAILED(Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &slot.Frequency))) { slot.Frequency = nullptr; FrequencyBroken = true; }
			if (FAILED(Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &slot.Disjoint))) { slot.Disjoint = nullptr; DisjointBroken = true; }
		}
		if (DisjointBroken || FrequencyBroken)
			Report(DisjointBroken && FrequencyBroken ? "TIMESTAMPDISJOINT and TIMESTAMPFREQ queries are missing; timing without a disjoint check, assuming a 1 GHz clock"
				: DisjointBroken ? "TIMESTAMPDISJOINT query is missing; timing without a disjoint check"
				: "TIMESTAMPFREQ query is missing; assuming a 1 GHz clock", S_OK);
		QueriesCreated = true;
		return true;
	}

	void Disable(const char* why, HRESULT hr) {
		if (why) Report(why, hr);
		ReleaseQueries();
		Unavailable = true;
	}

	// If a long run of timestamp pairs never produces a single result, say so once: results that stay
	// pending forever are otherwise indistinguishable from "profiling is off".
	void WarnIfNothingCompletes() {
		if (WarnedNothing || Issued < 480 || TotalSamples || Rejected) return;
		WarnedNothing = true;
		Report("480 timestamp pairs issued but none has completed (GetData keeps answering 'not ready')", S_FALSE);
	}

	void CollectCompleted() {
		for (auto& slot : Slots) {
			if (!slot.Pending) continue;

			BOOL disjoint = FALSE;
			if (slot.Disjoint && !DisjointBroken) {
				const HRESULT disjointReady = slot.Disjoint->GetData(&disjoint, sizeof(disjoint), 0);
				if (disjointReady == S_FALSE) continue;
				if (disjointReady != S_OK) {
					// Keep timing without it rather than losing the whole profile.
					DisjointBroken = true;
					disjoint = FALSE;
					Report("TIMESTAMPDISJOINT GetData failed; timing without a disjoint check from now on", disjointReady);
				}
			}

			UINT64 start = 0, end = 0, frequency = FallbackFrequency;
			const HRESULT startReady = slot.Start->GetData(&start, sizeof(start), 0);
			const HRESULT endReady = slot.End->GetData(&end, sizeof(end), 0);
			HRESULT frequencyReady = S_OK;
			if (slot.Frequency && !FrequencyBroken) {
				frequency = 0;
				frequencyReady = slot.Frequency->GetData(&frequency, sizeof(frequency), 0);
				if (FAILED(frequencyReady)) {
					const HRESULT failure = frequencyReady;
					FrequencyBroken = true;
					frequency = FallbackFrequency;
					frequencyReady = S_OK;
					Report("TIMESTAMPFREQ GetData failed; assuming a 1 GHz clock from now on", failure);
				}
			}
			if (FAILED(startReady) || FAILED(endReady)) {
				Disable("timestamp GetData failed", FAILED(startReady) ? startReady : endReady);
				return;
			}
			if (!GpuProfileQueriesReady(startReady, endReady, frequencyReady)) continue;
			slot.Pending = false;
			if (disjoint || !frequency || end < start) {
				if (++Rejected == 50 && !TotalSamples && !WarnedRejected) {
					WarnedRejected = true;
#ifndef NVR_GPU_PROFILER_TEST
					Logger::Log("GPU PROFILE '%s': 50 results discarded and none used (disjoint=%d, frequency=%llu, start=%llu, end=%llu)",
						Name, (int)disjoint, (unsigned long long)frequency, (unsigned long long)start, (unsigned long long)end);
#endif
				}
				continue;
			}

			const double ms = (double)(end - start) * 1000.0 / (double)frequency;
			TotalSamples++;
			if (OnSample) OnSample(Name, ms);
			if (!Enabled) continue;
			SumMs += ms;
			MinMs = MinMs < ms ? MinMs : ms;
			MaxMs = MaxMs > ms ? MaxMs : ms;
			WindowMs.push_back((float)ms);
			WindowSamples++;
			if (end == start) ZeroTickSamples++;

			if (WindowSamples >= ReportSamples) {
				// The middle value and the spread: one slow frame moves the average but not these (2026-10-16).
				std::sort(WindowMs.begin(), WindowMs.end());
				const size_t n = WindowMs.size();
				const double p50 = n ? WindowMs[n / 2] : 0.0, p10 = n ? WindowMs[n / 10] : 0.0, p90 = n ? WindowMs[(n * 9) / 10] : 0.0;
#ifdef NVR_GPU_PROFILER_TEST
				std::printf("GPU PROFILE %-24s avg %.4f ms  min %.4f  max %.4f  (%u samples, %u zero) | p50 %.4f  p10 %.4f  p90 %.4f\n",
					Name, SumMs / WindowSamples, MinMs, MaxMs, WindowSamples, ZeroTickSamples, p50, p10, p90);
#else
				Logger::Log("GPU PROFILE %-24s avg %.4f ms  min %.4f  max %.4f  (%u samples, %u zero) | p50 %.4f  p10 %.4f  p90 %.4f",
					Name, SumMs / WindowSamples, MinMs, MaxMs, WindowSamples, ZeroTickSamples, p50, p10, p90);
#endif
				WindowMs.clear();
				WindowSamples = 0;
				SumMs = 0.0;
				MinMs = DBL_MAX;
				MaxMs = 0.0;
				ZeroTickSamples = 0;
			}
		}
	}
};

class GpuProfileScope {
public:
#ifndef NVR_GPU_PROFILER_TEST
	// UNOFFICIAL 2026-10-26 GpuDebugLabels: the timer's name is also a debug group around the scope (GpuDebug.h), timer on or off.
	GpuProfileScope(GpuTimer& timer, IDirect3DDevice9* device) : Label(GpuDebug::Labels && GpuDebug::Begin(timer.GetName())), Timer(timer), Active((GpuTimer::MarkerHook ? GpuTimer::MarkerHook(timer.GetName(), true) : (void)0, timer.Begin(device))) {}
	~GpuProfileScope() { if (Active) Timer.End(); if (Label) GpuDebug::End(); if (GpuTimer::MarkerHook) GpuTimer::MarkerHook(Timer.GetName(), false); }
#else
	GpuProfileScope(GpuTimer& timer, IDirect3DDevice9* device) : Label(false), Timer(timer), Active(timer.Begin(device)) {}
	~GpuProfileScope() { if (Active) Timer.End(); }
#endif

private:
	bool Label;
	GpuTimer& Timer;
	bool Active;
};

// CPU wall-clock counterpart, reported in the same log format and toggled by the same F10
// switch. Comparing CPU submit time and the frame interval against the GPU buckets shows
// whether a scene is CPU- or GPU-bound, which decides which kind of optimisation can help.
class CpuTimer {
public:
	explicit CpuTimer(const char* name, unsigned reportSamples = 120)
		: Name(name), ReportSamples(reportSamples) { Registry().push_back(this); }

	// Every timer, so a frame-time spike can list which of them were slow in that frame.
	static std::vector<CpuTimer*>& Registry() { static std::vector<CpuTimer*> timers; return timers; }
	static unsigned& FrameStamp() { static unsigned stamp = 0; return stamp; }

	const char* GetName() const { return Name; }
	double LastMs() const { return Last; }
	bool RanRecently() const { return LastStamp + 1 >= FrameStamp(); } // this frame or the one before

	void Add(double ms) {
		Last = ms;
		LastStamp = FrameStamp();
		SumMs += ms;
		MinMs = MinMs < ms ? MinMs : ms;
		MaxMs = MaxMs > ms ? MaxMs : ms;
		if (++WindowSamples < ReportSamples) return;
#ifndef NVR_GPU_PROFILER_TEST
		Logger::Log("CPU PROFILE %-24s avg %.4f ms  min %.4f  max %.4f  (%u samples)",
			Name, SumMs / WindowSamples, MinMs, MaxMs, WindowSamples);
#endif
		WindowSamples = 0;
		SumMs = 0.0;
		MinMs = DBL_MAX;
		MaxMs = 0.0;
	}

	static double NowMs() {
		static LARGE_INTEGER frequency = {};
		if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
	}

	// Interval between successive calls, e.g. once per frame. Gaps longer than a second (loading,
	// menus, profiling just enabled) are dropped rather than skewing the average. Returns the
	// interval that was recorded, or 0 when it was dropped.
	double Tick() {
		const double now = NowMs();
		double interval = 0.0;
		if (LastTick > 0.0 && now - LastTick < 1000.0) { interval = now - LastTick; FrameStamp()++; Add(interval); }
		LastTick = now;
		return interval;
	}

private:
	const char* Name;
	unsigned ReportSamples;
	unsigned WindowSamples = 0;
	double SumMs = 0.0;
	double MinMs = DBL_MAX;
	double MaxMs = 0.0;
	double LastTick = 0.0;
	double Last = 0.0;
	unsigned LastStamp = 0;
};

// Frame-time percentiles and spike attribution for the F10 profiler. Fed one frame interval per
// frame (from the "Frame interval (CPU)" tick); needs no GPU queries, so it works the same on native
// D3D9 and under DXVK, which makes it the yardstick for comparing the two.
//
// Every 1200 frames (and when profiling is switched off, if at least 200 were collected) it logs
//   FRAME TIMES ... avg / p50 / p95 / p99 / p99.9 / max, 1% low and 0.1% low fps, spike count
// and, when some frames fell within three seconds of a cell change or loading screen, a second line
// for the steady frames only. A frame much slower than a typical one gets a FRAME SPIKE line naming
// the CPU timers that were slow in it (at most 60 per session).
class FrameTimeMonitor {
public:
	static constexpr unsigned WindowFrames = 1200;

	void Add(double ms, bool nearTransition) {
		All.push_back(ms);
		if (!nearTransition) Steady.push_back(ms);

		if (Typical > 0.0 && ms > FrameSpikeThresholdMs(Typical)) ReportSpike(ms, nearTransition);
		// Track the typical frame time without letting spikes drag it up.
		const double sample = Typical > 0.0 && ms > Typical * 2.0 ? Typical * 2.0 : ms;
		Typical = Typical > 0.0 ? Typical * 0.98 + sample * 0.02 : ms;

		if (All.size() >= WindowFrames) Report();
	}

	// Optional: appends what else is known about the current frame to each FRAME SPIKE line (set by
	// Hooks/Render.cpp: shader binds, first uses of a shader, the longest stall between binds).
	inline static void (*SpikeDetail)(char* buffer, size_t size) = nullptr;

	// Called when profiling is switched off: report what was collected if it is enough to mean something.
	void Flush() {
		if (All.size() >= 200) Report();
		All.clear();
		Steady.clear();
		Typical = 0.0;
	}

private:
	std::vector<double> All, Steady;
	double Typical = 0.0;
	unsigned SpikesLogged = 0;

	void ReportSpike(double ms, bool nearTransition) {
#ifndef NVR_GPU_PROFILER_TEST
		if (SpikesLogged >= 60) return;
		SpikesLogged++;
		char slow[256] = {};
		size_t used = 0;
		for (CpuTimer* timer : CpuTimer::Registry()) {
			if (!timer->RanRecently() || timer->LastMs() < 0.75 || !strcmp(timer->GetName(), "Frame interval (CPU)")) continue;
			const int written = _snprintf_s(slow + used, sizeof(slow) - used, _TRUNCATE, "%s%s %.2f ms", used ? ", " : "", timer->GetName(), timer->LastMs());
			if (written < 0) break;
			used += written;
		}
		char detail[512] = {};
		if (SpikeDetail) SpikeDetail(detail, sizeof(detail));
		Logger::Log("FRAME SPIKE %.1f ms (typical %.1f ms) %s | Vegas Shaders CPU timers over 0.75 ms in it: %s%s",
			ms, Typical, nearTransition ? "near a cell change or loading" : "during steady play", used ? slow : "none", detail);
#else
		(void)ms; (void)nearTransition;
#endif
	}

	void Report() {
#ifndef NVR_GPU_PROFILER_TEST
		std::vector<double> all = All;
		const FrameTimeSummary s = SummarizeFrameTimes(all);
		const unsigned spikes = CountFrameSpikes(all, s.p50Ms);
		Logger::Log("FRAME TIMES %u frames: avg %.2f ms (%.1f fps) | p50 %.2f  p95 %.2f  p99 %.2f  p99.9 %.2f  max %.2f ms | 1%% low %.1f fps  0.1%% low %.1f fps | spikes %u",
			s.frames, s.avgMs, s.avgMs > 0 ? 1000.0 / s.avgMs : 0.0, s.p50Ms, s.p95Ms, s.p99Ms, s.p999Ms, s.maxMs, s.low1Fps, s.low01Fps, spikes);
		if (Steady.size() >= 100 && Steady.size() < All.size()) {
			std::vector<double> steady = Steady;
			const FrameTimeSummary t = SummarizeFrameTimes(steady);
			Logger::Log("  steady play only (%u frames, %u within 3 s of a cell change or loading left out): p99 %.2f ms  1%% low %.1f fps  0.1%% low %.1f fps  max %.2f ms | spikes %u",
				t.frames, s.frames - t.frames, t.p99Ms, t.low1Fps, t.low01Fps, t.maxMs, CountFrameSpikes(steady, t.p50Ms));
		}
#endif
		All.clear();
		Steady.clear();
	}
};

inline FrameTimeMonitor& TheFrameTimeMonitor() { static FrameTimeMonitor monitor; return monitor; }

class CpuProfileScope {
public:
	explicit CpuProfileScope(CpuTimer& timer) : Timer(timer), Active(GpuTimer::Enabled),
		Start(Active ? CpuTimer::NowMs() : 0.0) {}
	~CpuProfileScope() { if (Active) Timer.Add(CpuTimer::NowMs() - Start); }

private:
	CpuTimer& Timer;
	bool Active;
	double Start;
};
