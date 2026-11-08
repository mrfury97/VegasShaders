// UNOFFICIAL 2026-10-27 (NVR DXVK, [Shaders.TAA.DLSS] Enabled): NVIDIA DLSS behind the Upscaler interface, through a 64-bit
// helper process (docs\next\FORK2-DLSS.md; the helper is tools\nvr-dlss-helper, MIT, built on its own).
//
// No NVIDIA code is in this file or anywhere in VegasShaders.dll: NVIDIA's DLSS (NGX) is 64-bit only and FNV is 32-bit, so
// NvrDlssHelper.exe (64-bit) owns NGX and works on the same GPU with its own Vulkan device. The NVR DXVK fork (patch 1.7-dlss,
// ID3D9NvrExtShare) exports four images and two timeline semaphores; per frame NVR records, in DXVK's command stream:
//   copy colour / depth / motion into the shared images -> signal "inputs ready" = N (a flush) -> GPU wait "output ready" = N
//   -> copy the shared output into TESR_DlssOutput (what Output() returns)
// and writes the frame's values (sizes, jitter, reset, motion scale) into a shared-memory block (tools\nvr-dlss-helper\
// NvrDlssProtocol.h). The helper records DLSS between the GPU wait and the signal. Nobody waits on the CPU.
//
// Silent fallback to FSR 2 (logged once with the reason) whenever anything is missing: Upscaler 0, no NVR DXVK or a fork without
// ID3D9NvrExtShare, not an NVIDIA GPU (the Vulkan device's vendor, not the D3D9 one: dxvk.conf may report AMD), no
// NvrDlssHelper.exe (game folder or Data\NVSE\Plugins\NvrDlss\), no nvngx_dlss.dll next to it, the helper's own answer (no RTX
// GPU, an old driver: "NGX: DLSS not available on this GPU"), a start that takes over 15 s, or a helper that dies or stops
// answering (a watchdog thread releases the GPU wait from the CPU, then FSR 2 runs). While the helper starts, FSR 2 runs.
//
// [Shaders.TAA.DLSS] HelperTest = 1 (with Enabled): the helper's pass-through test, for any NVIDIA GPU (a GTX 1070 too):
// the same images, semaphores and order, but the helper only stretches the colour input over the output and draws a marker
// (a magenta box that moves along 8 places with the frame number, a green box that stays), so the whole cross-process path
// (sharing, synchronisation, latency, order) can be proven without DLSS. Not anti-aliased: a test, not a mode to play with.
//
// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction ([Shaders.TAA.DLSS] RayReconstruction, off by default; docs\next\FORK2-DLSS-RR.md,
// protocol 2): the same helper also runs NVIDIA's DLSS-D. When RR is wanted at the helper's start (and nvngx_dlssd.dll is next to
// it, or it is the pass-through test), four more shared images carry RR's guides (diffuse and specular albedo, normals,
// roughness), and EvaluateRayReconstruction sends a frame flagged NVR_DLSS_FRAME_RR from the pre-tonemap RR point instead of DLSS
// SR's frame after the tonemap. One frame counter and one exchange for both kinds; the helper keeps two NGX features with their own
// histories, so each starts without history after a frame of the other kind. RR not being available (no RR runtime, NGX says no,
// DLSS-D fails) never ends DLSS SR: the helper reports it in rrAvailable, and DLSS SR (or FSR 2) runs as before.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <dxgi1_4.h>	// UNOFFICIAL 2026-10-30: the video memory figure (ExchangeWatch); dxgi.dll is loaded when first asked

#include "Upscaler.h"
#include "DxvkCompute.h"
#include "GpuProfiler.h"
#include "../../tools/nvr-dlss-helper/NvrDlssProtocol.h"

namespace {
	const UINT kNvidiaVendor = 0x10DE;
	const UINT64 kReleaseMargin = 1ull << 20;	// the watchdog / Stop release every wait that could still be recorded

	// UNOFFICIAL 2026-10-30 (protocol 2): the shared images' names, in the protocol's table order (the fork's debug names and the log's).
	const char* const kImageNames[NVR_DLSS_IMAGE_COUNT] = {
		"NVR DLSS colour", "NVR DLSS depth", "NVR DLSS motion", "NVR DLSS output",
		"NVR DLSS RR diffuse albedo", "NVR DLSS RR specular albedo", "NVR DLSS RR normals", "NVR DLSS RR roughness",
	};

	// UNOFFICIAL 2026-10-30: a setting's number as a protocol enum's value, [0, count).
	UInt32 Clamped(int value, UInt32 count) { return (UInt32)(std::min)((std::max)(value, 0), (int)count - 1); }

	bool FileExists(const std::wstring& path) {
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	// The fork's share interface codes (NvrDxvkExt.h): a Vulkan result, a Win32 error, or not shared.
	std::string HrText(HRESULT hr) {
		char text[96];
		if (HRESULT_FACILITY(hr) == NVR_EXT_FACILITY_VK) sprintf_s(text, "VkResult %d", -(int)(hr & 0xFFFF));
		else if (HRESULT_FACILITY(hr) == FACILITY_WIN32) sprintf_s(text, "Win32 error %lu", (unsigned long)HRESULT_CODE(hr));
		else if (hr == D3DERR_NOTAVAILABLE) sprintf_s(text, "D3DERR_NOTAVAILABLE: not shared / not supported");
		else if (SUCCEEDED(hr)) sprintf_s(text, "it returned no handle");
		else sprintf_s(text, "HRESULT 0x%08lX", (unsigned long)hr);
		return text;
	}

	std::wstring GameDir() {
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir(path);
		const size_t slash = dir.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
	}

	// UNOFFICIAL 2026-10-30 (tester report: DLSS at native size (DLAA), RTX 5060, "fps dropped to ~20 at random while turning the
	// camera", gone with DLAA off; their logs had only the 25-second averages below, all healthy: helper GPU 0.95 ms, response 0.15 ms).
	// What a user's log needs to tell the DLSS exchange's share of a slow frame from the rest of the frame, always on while the helper
	// runs (no F10), a few microseconds a frame:
	//  - the game's frame interval (from one exchange to the next; gaps over a second, menus and loading, left out): average, p99, max,
	//    and how many frames took over 33 / 50 ms;
	//  - the exchange on the game's queue, with timestamp queries read back frames later without waiting (DXVK reports nanoseconds;
	//    TIMESTAMPFREQ when it answers): the copies in (start to "inputs ready": one command list, the copies' own GPU time), and the span
	//    to the helper's output copied back (review 2026-10-30: this one crosses the flush that "inputs ready" makes, so in a CPU-bound
	//    frame it also holds the CPU time until the frame's next submit; the helper's own share is its GPU time and response, from the
	//    helper itself);
	//  - the game process's video memory against its budget (DXGI 1.4 QueryVideoMemoryInfo on the GPU DXVK uses, from the fork's LUID):
	//    over budget, Windows pages memory in and out, a classic cause of drops while turning (new textures stream in).
	// A frame over 50 ms that is also 3x the typical one gets its own "DLSS SLOW FRAME" line (30 a session at most, one a second).
	class ExchangeWatch {
	public:
		void SetAdapter(const BYTE luid[8], bool valid) {
			if (!valid) return;
			memcpy(&luidWanted, luid, sizeof(luidWanted));
			luidValid = true;
		}

		void Reset() { intervals.clear(); lastQpc.QuadPart = 0; typicalMs = 0.0; ResetGpuWindow(); }
		// (review 2026-10-30) frames went by without an exchange (FSR 2 ran, the effects did not run): the next interval is not a frame's
		void Gap() { lastQpc.QuadPart = 0; }

		void BeginGpu(IDirect3DDevice9* Device) {
			active = nullptr;
			if (!Device || broken) return;
			if (Device != device) {
				ReleaseQueries();
				device = Device;
			}
			if (!created && !CreateQueries()) return;
			Collect();
			for (Slot& slot : slots) {
				if (slot.pending) continue;
				if (FAILED(slot.start->Issue(D3DISSUE_END))) { broken = true; ReleaseQueries(); return; }
				active = &slot;
				return;
			}
		}
		// Right before "inputs ready" (the flush): the copies' end, in the same command list as the start. The frame's number and the
		// frames in flight then go with the slot, for the exchange-stall line.
		void MidGpu(UInt64 frame, UInt64 inFlight, const char* mode) {
			if (!active) return;
			active->frame = frame;
			active->inFlight = inFlight;
			active->mode = mode;
			if (FAILED(active->mid->Issue(D3DISSUE_END))) { broken = true; ReleaseQueries(); }
		}
		void SetShared(const NvrDlssShared* s) { sharedBlock = s; }
		void EndGpu(bool keep) {
			Slot* slot = active;
			active = nullptr;
			if (!slot) return;
			if (FAILED(slot->end->Issue(D3DISSUE_END)) || (slot->frequency && FAILED(slot->frequency->Issue(D3DISSUE_END)))) {
				broken = true;
				ReleaseQueries();
				return;
			}
			slot->pending = keep;	// a frame that recorded nothing: its pair is issued (no open query left) but not read
		}

		// Once a frame, after the exchange. The slow-frame line names what was going on.
		void Frame(const char* mode, const NvrDlssShared* shared, UInt64 inFlight) {
			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			if (lastQpc.QuadPart) {
				const double ms = (double)(now.QuadPart - lastQpc.QuadPart) * 1000.0 / (double)frequency.QuadPart;
				if (ms < 1000.0) {
					intervals.push_back((float)ms);
					const bool slow = typicalMs > 0.0 && ms > 50.0 && ms > typicalMs * 3.0;
					if (slow && slowLogged < 30 && (now.QuadPart - lastSlowQpc.QuadPart) > frequency.QuadPart) {
						slowLogged++;
						lastSlowQpc = now;
						char vram[128];
						VideoMemory(vram, sizeof(vram));
						Logger::Log("DLSS SLOW FRAME %.1f ms (typical %.1f ms) with %s: the copies in lately %.2f ms; the span to the helper's output lately %.2f ms "
							"(max of the last 8 read %.2f ms; with CPU time until the next submit when CPU-bound); %llu frames in flight; the helper itself: response "
							"%.3f ms avg (max %.3f), GPU %.3f ms; video memory %s",
							ms, typicalMs, mode, copyLastMs, gpuLastMs, RecentMaxMs(), (unsigned long long)inFlight, shared ? shared->responseUsAvg / 1000.0 : 0.0,
							shared ? shared->responseUsMax / 1000.0 : 0.0, shared ? shared->gpuTimeUsLast / 1000.0 : 0.0, vram);
					}
					// The typical frame, not dragged up by the slow ones.
					const double sample = typicalMs > 0.0 && ms > typicalMs * 2.0 ? typicalMs * 2.0 : ms;
					typicalMs = typicalMs > 0.0 ? typicalMs * 0.98 + sample * 0.02 : ms;
				}
			}
			lastQpc = now;
		}

		// The window's figures for the periodic DLSS HELPER line, then a new window.
		void Report(char* text, size_t size) {
			Collect();
			char frames[192] = "no frame intervals yet";
			if (!intervals.empty()) {
				std::vector<float> sorted = intervals;
				std::sort(sorted.begin(), sorted.end());
				double sum = 0.0;
				UInt32 over33 = 0, over50 = 0;
				for (float ms : sorted) { sum += ms; if (ms > 33.3f) over33++; if (ms > 50.0f) over50++; }
				const size_t n = sorted.size();
				sprintf_s(frames, "frame interval %.2f ms avg, p99 %.2f, max %.1f (%u of %u over 33 ms, %u over 50 ms)", sum / n,
					sorted[(std::min)(n - 1, (n * 99) / 100)], sorted[n - 1], over33, (UInt32)n, over50);
			}
			char gpu[224] = "the exchange not measured (no timestamp queries)";
			if (gpuCount) sprintf_s(gpu, "the copies in %.2f ms avg; the span to the helper's output %.2f ms avg, max %.2f (%u of %u over 4 ms, %u over 20 ms; "
				"with CPU time until the next submit when CPU-bound)", copySumMs / gpuCount, gpuSumMs / gpuCount, gpuMaxMs, gpuOver4, gpuCount, stallsWindow);
			char vram[128];
			VideoMemory(vram, sizeof(vram));
			sprintf_s(text, size, "; %s; %s; video memory %s", frames, gpu, vram);
			intervals.clear();
			ResetGpuWindow();
		}

	private:
		struct Slot {
			IDirect3DQuery9* start = nullptr; IDirect3DQuery9* mid = nullptr; IDirect3DQuery9* end = nullptr; IDirect3DQuery9* frequency = nullptr;
			bool pending = false;
			UInt64 frame = 0, inFlight = 0;		// (DLSS EXCHANGE STALL line)
			const char* mode = "";
		};
		const NvrDlssShared* sharedBlock = nullptr;
		UInt32	stallsLogged = 0, stallsWindow = 0;
		LARGE_INTEGER lastStallQpc = {};
		Slot	slots[8];
		Slot*	active = nullptr;
		IDirect3DDevice9* device = nullptr;
		bool	created = false, broken = false;
		std::vector<float> intervals;
		LARGE_INTEGER lastQpc = {}, lastSlowQpc = {};
		double	typicalMs = 0.0;
		UInt32	slowLogged = 0;
		double	gpuSumMs = 0.0, gpuMaxMs = 0.0, gpuLastMs = 0.0, copySumMs = 0.0, copyLastMs = 0.0;
		UInt32	gpuCount = 0, gpuOver4 = 0;
		float	recent[8] = {};
		UInt32	recentCount = 0;
		LUID	luidWanted = {};
		bool	luidValid = false, dxgiTried = false;
		IDXGIAdapter3* adapter = nullptr;

		void ResetGpuWindow() { gpuSumMs = gpuMaxMs = copySumMs = 0.0; gpuCount = gpuOver4 = stallsWindow = 0; }
		double RecentMaxMs() const {
			double m = 0.0;
			for (UInt32 i = 0; i < (std::min)(recentCount, (UInt32)8); i++) m = (std::max)(m, (double)recent[i]);
			return m;
		}
		bool CreateQueries() {
			for (Slot& slot : slots) {
				if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.start)) || FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.mid)) ||
					FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &slot.end))) {
					broken = true;
					ReleaseQueries();
					return false;
				}
				if (FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &slot.frequency))) slot.frequency = nullptr;
			}
			created = true;
			return true;
		}
		void ReleaseQueries() {
			for (Slot& slot : slots) {
				IDirect3DQuery9** queries[] = { &slot.start, &slot.mid, &slot.end, &slot.frequency };
				for (IDirect3DQuery9** q : queries) if (*q) { (*q)->Release(); *q = nullptr; }
				slot.pending = false;
			}
			created = false;
			active = nullptr;
		}
		void Collect() {
			for (Slot& slot : slots) {
				if (!slot.pending) continue;
				UINT64 start = 0, mid = 0, end = 0, frequency = 1000000000ull;
				if (slot.start->GetData(&start, sizeof(start), 0) != S_OK || slot.mid->GetData(&mid, sizeof(mid), 0) != S_OK ||
					slot.end->GetData(&end, sizeof(end), 0) != S_OK) continue;
				if (slot.frequency && slot.frequency->GetData(&frequency, sizeof(frequency), 0) != S_OK) continue;
				slot.pending = false;
				if (!frequency || end < start || mid < start || end < mid) continue;
				const double copies = (double)(mid - start) * 1000.0 / (double)frequency;
				copyLastMs = copies;
				copySumMs += copies;
				const double ms = (double)(end - start) * 1000.0 / (double)frequency;
				gpuLastMs = ms;
				gpuSumMs += ms;
				gpuMaxMs = (std::max)(gpuMaxMs, ms);
				gpuCount++;
				if (ms > 4.0) gpuOver4++;
				// UNOFFICIAL 2026-10-30 (DLAA stall report): an exchange whose span is over 20 ms gets a line of its own (60 a session, at
				// most 2 a second), with what the helper says about itself: its own GPU time and response normal, a long span = the game's
				// GPU waited for a helper that was not scheduled (or a CPU-bound frame's late submit: see "copies").
				if (ms > 20.0) {
					stallsWindow++;
					LARGE_INTEGER now, qpf;
					QueryPerformanceCounter(&now);
					QueryPerformanceFrequency(&qpf);
					if (stallsLogged < 60 && (now.QuadPart - lastStallQpc.QuadPart) * 2 > qpf.QuadPart) {
						stallsLogged++;
						lastStallQpc = now;
						Logger::Log("DLSS EXCHANGE STALL frame %llu (%s): the span to the helper's output %.1f ms (the copies in %.2f ms), %llu frames in flight "
							"then; the helper itself: GPU %.3f ms (last), response %.3f ms avg (max %.3f)", (unsigned long long)slot.frame, slot.mode, ms, copies,
							(unsigned long long)slot.inFlight, sharedBlock ? sharedBlock->gpuTimeUsLast / 1000.0 : 0.0,
							sharedBlock ? sharedBlock->responseUsAvg / 1000.0 : 0.0, sharedBlock ? sharedBlock->responseUsMax / 1000.0 : 0.0);
					}
				}
				recent[recentCount++ % 8] = (float)ms;
			}
		}
		void VideoMemory(char* text, size_t size) {
			if (!dxgiTried) {
				dxgiTried = true;
				typedef HRESULT(WINAPI* CreateFactory)(REFIID, void**);
				const HMODULE dxgi = LoadLibraryA("dxgi.dll");
				const CreateFactory create = dxgi ? (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
				IDXGIFactory4* factory = nullptr;
				if (create && luidValid && SUCCEEDED(create(__uuidof(IDXGIFactory4), (void**)&factory)) && factory) {
					if (FAILED(factory->EnumAdapterByLuid(luidWanted, __uuidof(IDXGIAdapter3), (void**)&adapter))) adapter = nullptr;
					factory->Release();
				}
			}
			DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
			if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) { sprintf_s(text, size, "unknown (no DXGI 1.4 adapter)"); return; }
			sprintf_s(text, size, "%llu MB used by the game of a %llu MB budget%s", (unsigned long long)(info.CurrentUsage >> 20),
				(unsigned long long)(info.Budget >> 20), info.CurrentUsage > info.Budget ? " (OVER BUDGET: Windows pages video memory)" : "");
		}
	};

	// UNOFFICIAL 2026-10-30: DLSS's quality mode as the helper picks it from render / display size (NvrDlssHelper QualityFor), and
	// [Shaders.TAA.DLSS] Preset's name, for the log.
	const char* DlssModeName(UINT renderWidth, UINT displayWidth) {
		const float r = displayWidth ? (float)renderWidth / (float)displayWidth : 1.0f;
		return r >= 0.99f ? "DLAA" : r >= 0.66f ? "Quality" : r >= 0.57f ? "Balanced" : r >= 0.49f ? "Performance" : "Ultra Performance";
	}
	const char* DlssPresetName(UInt32 preset) {
		static const char* const names[NVR_DLSS_PRESET_COUNT] = { "default", "CNN", "transformer", "transformer J" };
		return preset < NVR_DLSS_PRESET_COUNT ? names[preset] : "default";
	}

	class DlssUpscaler : public Upscaler::IUpscaler {
	public:
		~DlssUpscaler() override {}	// nothing at DLL unload: the helper exits with the game by itself

		const char* Name() const override {
			if (state != Running) return "FSR 2.2.1 (DLSS not running)";
			return passThrough ? "DLSS helper pass-through test (no DLSS)" : "NVIDIA DLSS (64-bit helper)";
		}
		IDirect3DTexture9*	Output() const override { return usedSelf ? output.Texture : Upscaler::Fsr2()->Output(); }
		bool				Failed() const override { return state == Running ? false : Upscaler::Fsr2()->Failed(); }
		bool				Evaluate(IDirect3DDevice9* Device, const Upscaler::Inputs& In) override;
		void				ReleaseResources() override { Stop("its resources were released"); state = Idle; }
		std::string			DiagText() const override {	// UNOFFICIAL 2026-11-01 (the history watch's lines)
			const char* names[] = { "idle", "starting", "running", "off for the session" };
			std::string text = std::string("DLSS helper ") + names[state] + (passThrough ? " (pass-through test)" : "") + ", this frame " +
				(usedSelf ? "the helper's output" : "FSR 2's output in its place");
			if (!usedSelf) text += " (" + Upscaler::Fsr2()->DiagText() + ")";
			return text;
		}

		bool	IsOff() const { return state == Off; }
		// a frame went to FSR 2 by the setting: DLSS's history is stale (and FSR 2's textures are freed again once DLSS has run 600 frames)
		void	MarkUnused() { dlssNeedsReset = true; runningFrames = 0; fsrFreed = false; watch.Gap(); }
		void	SetOptions(bool test, int jitterFlip, bool cpuWait, bool highPriority, bool exactInputs, int failTest) {
			if (test != passThrough && state != Idle) {
				// Another mode: the helper is started again (FSR 2 in between). UNOFFICIAL 2026-10-30: also after the other mode turned
				// itself off for the session (real DLSS on a GTX card gives up: the pass-through test can still run there).
				if (state != Off) Stop(test ? "HelperTest turned on" : "HelperTest turned off");
				else Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper: HelperTest turned %s: the helper is tried again in that mode.", test ? "on" : "off");
				state = Idle;
			}
			// UNOFFICIAL 2026-10-30 (DLAA stall report): the helper's start options; a change starts it again (FSR 2 in between)
			if ((cpuWait != helperCpuWait || highPriority != helperHighPriority) && state != Idle && state != Off) {
				Stop(cpuWait != helperCpuWait ? (cpuWait ? "HelperWait turned to 1 (the helper waits on the CPU)" : "HelperWait turned to 0 (the GPU waits)") :
					(highPriority ? "HelperGpuPriority turned on" : "HelperGpuPriority turned off"));
				state = Idle;
			}
			// UNOFFICIAL 2026-10-30 (top-left picture report): HelperExactInputs as the two above; HelperFailTest (test only) also from Off,
			// so a test list can fail the helper in each way in turn.
			if (exactInputs != helperExactInputs && state != Idle && state != Off) {
				Stop(exactInputs ? "HelperExactInputs turned to 1 (the helper copies the render rectangle)" : "HelperExactInputs turned to 0 (DLSS reads the rectangle in place)");
				state = Idle;
			}
			if (failTest != helperFailTest) {
				if (state != Idle && state != Off) Stop("HelperFailTest changed");
				else if (state == Off) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper: HelperFailTest %d: the helper is tried again.", failTest);
				state = Idle;
			}
			helperCpuWait = cpuWait;
			helperHighPriority = highPriority;
			helperExactInputs = exactInputs;
			helperFailTest = failTest;
			passThrough = test;
			flipX = (jitterFlip & 1) != 0;
			flipY = (jitterFlip & 2) != 0;
		}

		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (Upscaler.h). RR's images are made at the helper's start, so a change of
		// Wanted starts the helper again (FSR 2 in between), as HelperTest does; the other options are per frame.
		void	SetRrOptions(const Upscaler::RrOptions& options) {
			if (options.Wanted != rr.Wanted) {
				// Turned off while this start has no RR images (nvngx_dlssd.dll was missing): the helper is already what an RR-off
				// start makes, so it goes on.
				if ((state == Starting || state == Running) && (options.Wanted || rrImages)) {
					Stop(options.Wanted ? "RayReconstruction turned on" : "RayReconstruction turned off");
					state = Idle;
				}
				if (options.Wanted) rrDllReported = false;	// a missing nvngx_dlssd.dll is logged again for this turn-on
			}
			rr = options;
		}
		// UNOFFICIAL 2026-11-01 [Shaders.TAA.DLSS] WeaponMask (per frame; the mask's image is made at every start its pass can run)
		void	SetWeaponMask(float value) { weaponMask = (std::min)((std::max)(value, 0.0f), 1.0f); }
		const Upscaler::RrOptions&	RrSettings() const { return rr; }
		bool				RrReady();
		bool				EvaluateRr(IDirect3DDevice9* Device, const Upscaler::RrInputs& In);
		IDirect3DTexture9*	RrOutput() const { return output.Texture; }
		// (review 2026-10-30: with RR wanted the helper is polled here too, so a helper that died is seen before the RT point, not halfway
		// through the frame's effects; nothing more with RR off)
		void				RrFrameBegin() {
			if (rr.Wanted && (state == Starting || state == Running)) Poll();
			frameStamp++;	// (ExchangeWatch: a frame without an exchange in between is a gap)
			rrLastRan = rrRanThisFrame;
			rrRanThisFrame = false;
			rrPlannedThisFrame = false;
		}
		void				RrPlan(bool planned) { rrPlannedThisFrame = planned; }
		void				RrDelivered() { rrRanThisFrame = true; }
		// (also a live helper with RR available, and this frame planned for RR: a frame after the helper died or DLSS-D failed, or one that
		// will not go to RR (the Pip-Boy, a menu), gets the denoisers back at once)
		bool				RrWantsRaw() const {
			return rrLastRan && rrPlannedThisFrame && rr.Wanted && !rr.KeepDenoisers && state == Running && rrImages && !rrFailed && shared &&
				shared->rrAvailable != 0 && !tripped;
		}

	private:
		enum State { Idle, Starting, Running, Off };
		struct Tex { IDirect3DTexture9* Texture = nullptr; IDirect3DSurface9* Surface = nullptr; };

		State		state = Idle;
		bool		passThrough = false;
		bool		flipX = false, flipY = false;
		bool		helperCpuWait = true, helperHighPriority = true;	// UNOFFICIAL 2026-10-30: the helper's start options (DlssOptions)
		bool		helperExactInputs = true;	// UNOFFICIAL 2026-10-30 (top-left picture report): HelperExactInputs
		int			helperFailTest = 0;			// ... and HelperFailTest (test only: 1 NVR ends the helper, 2 it reports a failure, 3 it hangs)
		bool		usedSelf = false;			// this frame's Output() is the helper's (else FSR 2's)
		bool		fsrNeedsReset = false;		// FSR 2's history is stale after DLSS frames
		bool		dlssNeedsReset = true;
		ID3D9NvrExtShare*		share = nullptr;
		ID3D9NvrExtSharedImage*	images[NVR_DLSS_IMAGE_COUNT] = {};
		ID3D9NvrExtSharedFence*	inputsReady = nullptr;
		ID3D9NvrExtSharedFence*	outputReady = nullptr;
		Tex			output;
		UINT		inputWidth = 0, inputHeight = 0, displayWidth = 0, displayHeight = 0;
		HANDLE		mapping = nullptr, frameEvent = nullptr, statusEvent = nullptr, process = nullptr, watchdog = nullptr;
		NvrDlssShared*	shared = nullptr;
		ULONGLONG	startTick = 0;
		UInt64		frame = 0;
		volatile LONG64	lastRecorded = 0;			// read by the watchdog thread
		volatile LONG	watchStop = 0;
		volatile LONG	tripped = 0;
		char		tripReason[160] = {};
		UInt32		reported = 0;
		UInt32		lastSerial = 0;
		double		recordUs = 0.0;
		UInt32		recordCount = 0;
		std::wstring helperDir;
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction
		Upscaler::RrOptions	rr;					// DlssRrOptions' copy
		UInt32		imageCount = 0;				// images this start made: NVR_DLSS_IMAGE_COUNT_SR, or NVR_DLSS_IMAGE_COUNT with RR's guides
		bool		rrImages = false;			// this start made RR's guides and told the helper (NVR_DLSS_FEATURE_RR)
		bool		rrFailed = false;			// an RR input could not be copied: no RR until the helper starts again (SR goes on)
		bool		rrNeedsReset = true;		// the helper's DLSS-D history is stale (a start, or DLSS SR frames went by)
		bool		rrRanThisFrame = false;		// EvaluateRayReconstruction recorded this frame's exchange
		bool		rrLastRan = false;			// ... last frame's (RayReconstructionFrameBegin)
		bool		rrPlannedThisFrame = false;	// RayReconstructionPlan: this frame is meant for RR (review 2026-10-30)
		bool		rrSkipLogged = false;		// EvaluateRr turned a frame down (missing input / other sizes): logged once a start
		bool		rrDllReported = false;		// "nvngx_dlssd.dll is not next to the helper" logged
		int			rrReported = 0;				// readiness logged since this start: 0 nothing, 1 "ready", 2 "not available"
		UInt64		rrSent = 0;					// RR frames sent since this start
		// UNOFFICIAL 2026-10-30 (DLAA drops report): the always-on figures, and FSR 2's own textures freed once DLSS runs
		ExchangeWatch	watch;
		UInt64		runningFrames = 0;			// exchanges since this start
		bool		fsrFreed = false;			// FSR 2's textures were freed after DLSS's first 600 frames (it makes them again if it runs)
		UInt64		frameStamp = 0, exchangeStamp = 0;	// RayReconstructionFrameBegin's frame count, and the last exchange's (review 2026-10-30)
		// UNOFFICIAL 2026-11-01 (player report: the first-person arms and gun ghost with DLSS) DLSS SR's bias current colour mask (protocol 3):
		// DlssBiasMask.comp writes it from FSR's depth input (g = reactive) and the weapon's depth into maskTex, which is copied to maskImage
		// (NvrDlssShared::biasMask) with the other inputs. Any failure: no mask (logged once), DLSS runs as before.
		float		weaponMask = 1.0f;			// [Shaders.TAA.DLSS] WeaponMask
		ID3D9NvrExtPipeline*	maskPipeline = nullptr;	// made once a session
		bool		maskTried = false;
		Tex			maskTex;					// r16f, the inputs' size
		ID3D9NvrExtSharedImage*	maskImage = nullptr;
		bool		maskReady = false;			// this start gave the helper the mask (NVR_DLSS_FEATURE_BIAS_MASK)
		bool		maskThisFrame = false;		// Record wrote this frame's mask (NVR_DLSS_FRAME_BIAS_MASK)
		bool		maskFirstLogged = false;

		bool	MakeMask(const Upscaler::Inputs& In);
		bool	RecordMask(const Upscaler::Inputs& In);
		bool	TurnOff(const char* why);
		bool	Start(const Upscaler::Inputs& In);
		void	Stop(const char* why);
		void	Poll();
		bool	Record(IDirect3DDevice9* Device, const Upscaler::Inputs& In);
		bool	RecordRr(const Upscaler::RrInputs& In);
		void	Exchange(const Upscaler::Inputs& In, const Upscaler::RrInputs* Rr, const LARGE_INTEGER& t0);
		bool	FallBack(IDirect3DDevice9* Device, const Upscaler::Inputs& In);
		void	LogHelperMessage();
		static DWORD WINAPI WatchdogMain(void* self);
	};

	DlssUpscaler TheDlss;

	// Off for the session: logged once with the reason. FSR 2 runs from now on.
	bool DlssUpscaler::TurnOff(const char* why) {
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: [Shaders.TAA.DLSS] Enabled (%s): %s; FSR 2 is used instead for this session.",
			passThrough ? "DLSS helper pass-through test" : "DLSS", why);
		Stop(nullptr);
		state = Off;
		return false;
	}

	void DlssUpscaler::LogHelperMessage() {
		if (!shared || shared->messageSerial == lastSerial) return;
		lastSerial = shared->messageSerial;
		char message[sizeof(shared->message)];
		memcpy(message, (const void*)shared->message, sizeof(message));
		message[sizeof(message) - 1] = 0;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper: %s", message);
	}

	DWORD WINAPI DlssUpscaler::WatchdogMain(void* self) {
		DlssUpscaler* d = (DlssUpscaler*)self;
		UINT64 lastValue = 0;
		ULONGLONG lastProgress = GetTickCount64();
		const char* why = nullptr;
		while (!InterlockedCompareExchange(&d->watchStop, 0, 0)) {
			if (WaitForSingleObject(d->process, 100) == WAIT_OBJECT_0) { why = "the helper process exited (NvrDlssHelper.log says why)"; break; }
			UINT64 done = 0;
			if (FAILED(d->outputReady->GetCompletedValue(&done))) continue;
			const LONG64 recorded = InterlockedCompareExchange64(&d->lastRecorded, 0, 0);
			const ULONGLONG now = GetTickCount64();
			if (done != lastValue || (LONG64)done >= recorded) { lastValue = done; lastProgress = now; }
			else if (now - lastProgress > 3000) { why = "no frame came back from the helper for 3 seconds"; break; }
		}
		if (!why) return 0;
		// The game's GPU queue may be waiting for a frame the helper will never finish: end the helper, then release the wait.
		strncpy_s(d->tripReason, why, _TRUNCATE);
		TerminateProcess(d->process, 1);
		WaitForSingleObject(d->process, 2000);
		UINT64 done = 0;
		d->outputReady->GetCompletedValue(&done);
		const UINT64 recorded = (UINT64)InterlockedCompareExchange64(&d->lastRecorded, 0, 0);
		d->outputReady->SignalFromHost((std::max)(done, recorded) + kReleaseMargin);
		InterlockedExchange(&d->tripped, 1);
		return 0;
	}

	bool DlssUpscaler::Start(const Upscaler::Inputs& In) {
		// UNOFFICIAL 2026-11-07 (perf-ptcost; the 11-07 tester log: 4.3-5.3 s on the render thread between "Registering Texture TESR_DlssOutput"
		// and "... given to the helper" at every helper start) the stage for the STALL lines, and each step's time in the "given" line
		SessionLog::StageScope stage("DLSS helper: start (images, the helper process, handles)");
		LARGE_INTEGER stepFreq, stepT[6] = {};
		QueryPerformanceFrequency(&stepFreq);
		QueryPerformanceCounter(&stepT[0]);
		auto stepMs = [&](int a, int b) { return stepT[a].QuadPart && stepT[b].QuadPart ? (double)(stepT[b].QuadPart - stepT[a].QuadPart) * 1000.0 / (double)stepFreq.QuadPart : -1.0; };
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!ext || !DxvkCompute::Available()) return TurnOff("no Vegas Shaders DXVK");
		if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtShare), (void**)&share)) || !share) {
			share = nullptr;
			return TurnOff("this Vegas Shaders DXVK d3d9.dll cannot share images with another process (fork patch 1.7-dlss or newer needed)");
		}
		NVR_EXT_SHARE_INFO info = {};
		share->GetShareInfo(&info);
		if (!info.Supported) return TurnOff("the Vulkan driver cannot share images and semaphores between processes");
		if (info.VendorId != kNvidiaVendor) return TurnOff("not an NVIDIA GPU (DLSS needs an NVIDIA RTX GPU)");

		// The helper: the game folder, else Data\NVSE\Plugins\NvrDlss (its own folder: xNVSE loads every DLL right in Plugins).
		const std::wstring game = GameDir();
		const std::wstring candidates[] = { game, game + L"\\Data\\NVSE\\Plugins\\NvrDlss" };
		std::wstring exe;
		for (const std::wstring& dir : candidates) {
			if (FileExists(dir + L"\\NvrDlssHelper.exe")) { helperDir = dir; exe = dir + L"\\NvrDlssHelper.exe"; break; }
		}
		if (exe.empty()) return TurnOff("NvrDlssHelper.exe was not found (the game folder, or Data\\NVSE\\Plugins\\NvrDlss)");
		if (!passThrough && !FileExists(helperDir + L"\\nvngx_dlss.dll"))
			return TurnOff("nvngx_dlss.dll (NVIDIA's DLSS runtime) is not next to NvrDlssHelper.exe");
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: RR's guide images are made only when RR is wanted at this start, and in DLSS
		// mode only with NVIDIA's RR runtime next to the helper (the pass-through test needs none). Without it DLSS SR starts as before.
		bool withRr = rr.Wanted;
		if (withRr && !passThrough && !FileExists(helperDir + L"\\nvngx_dlssd.dll")) {
			if (!rrDllReported) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction is off: nvngx_dlssd.dll (NVIDIA's Ray Reconstruction runtime) is not next to NvrDlssHelper.exe; DLSS SR runs.");
			rrDllReported = true;
			withRr = false;
		}

		// The shared images (the fork's), at the inputs' and the display's size, and the output NVR reads; then RR's guides, at the
		// inputs' size (UNOFFICIAL 2026-10-30, protocol 2).
		const NVR_EXT_SHARED_IMAGE_DESC descs[NVR_DLSS_IMAGE_COUNT] = {
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_RGBA16F, kImageNames[NVR_DLSS_IMAGE_COLOR] },
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_R32F, kImageNames[NVR_DLSS_IMAGE_DEPTH] },
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_RG16F, kImageNames[NVR_DLSS_IMAGE_MOTION] },
			{ In.DisplayWidth, In.DisplayHeight, NVR_EXT_SHARED_RGBA16F, kImageNames[NVR_DLSS_IMAGE_OUTPUT] },
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_RGBA16F, kImageNames[NVR_DLSS_IMAGE_DIFFUSE_ALBEDO] },
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_RGBA16F, kImageNames[NVR_DLSS_IMAGE_SPECULAR_ALBEDO] },
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_RGBA16F, kImageNames[NVR_DLSS_IMAGE_NORMALS] },
			{ In.InputWidth, In.InputHeight, NVR_EXT_SHARED_R16F, kImageNames[NVR_DLSS_IMAGE_ROUGHNESS] },
		};
		char why[320];
		imageCount = withRr ? (UInt32)NVR_DLSS_IMAGE_COUNT : (UInt32)NVR_DLSS_IMAGE_COUNT_SR;
		for (UInt32 i = 0; i < imageCount; i++) {
			const HRESULT hr = share->CreateSharedImage(&descs[i], &images[i]);
			if (FAILED(hr)) {
				if (i >= NVR_DLSS_IMAGE_COUNT_SR) {
					// One of RR's guides: no RR for this start, DLSS SR starts with its own four images.
					Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction is off: the shared image '%s' could not be made (fork CreateSharedImage: %s; the DXVK log says more); DLSS SR runs.",
						descs[i].pDebugName, HrText(hr).c_str());
					for (UInt32 j = NVR_DLSS_IMAGE_COUNT_SR; j < NVR_DLSS_IMAGE_COUNT; j++) DxvkCompute::Drop(images[j]);
					imageCount = NVR_DLSS_IMAGE_COUNT_SR;
					break;
				}
				sprintf_s(why, "the shared image '%s' could not be made (fork CreateSharedImage: %s; the DXVK log says more)", descs[i].pDebugName, HrText(hr).c_str());
				return TurnOff(why);
			}
		}
		rrImages = imageCount == NVR_DLSS_IMAGE_COUNT;
		maskReady = MakeMask(In);	// UNOFFICIAL 2026-11-01 protocol 3 (false: no mask, logged; DLSS as before)
		ID3D9NvrExtSharedFence** fences[2] = { &inputsReady, &outputReady };
		for (UInt32 i = 0; i < 2; i++) {
			const HRESULT hr = share->CreateSharedFence(0, fences[i]);
			if (FAILED(hr)) {
				sprintf_s(why, "the shared semaphore '%s' could not be made (fork CreateSharedFence: %s; the DXVK log says more)", i ? "output ready" : "inputs ready", HrText(hr).c_str());
				return TurnOff(why);
			}
		}
		QueryPerformanceCounter(&stepT[1]);	// (perf-ptcost) the shared images, the mask, the fences
		TheTextureManager->InitTexture("TESR_DlssOutput", &output.Texture, &output.Surface, In.DisplayWidth, In.DisplayHeight, D3DFMT_A16B16G16R16F);
		if (!output.Surface) return TurnOff("its output texture could not be made");
		QueryPerformanceCounter(&stepT[2]);	// the output texture
		inputWidth = In.InputWidth; inputHeight = In.InputHeight; displayWidth = In.DisplayWidth; displayHeight = In.DisplayHeight;

		// The shared-memory block and the two events, then the helper.
		wchar_t base[96];
		swprintf_s(base, L"Local\\NvrDlss-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NvrDlssShared), base);
		shared = mapping ? (NvrDlssShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NvrDlssShared)) : nullptr;
		frameEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_DLSS_EVENT_FRAME_SUFFIX).c_str());
		statusEvent = CreateEventW(nullptr, FALSE, FALSE, (std::wstring(base) + NVR_DLSS_EVENT_STATUS_SUFFIX).c_str());
		if (!shared || !frameEvent || !statusEvent) return TurnOff("its shared-memory block could not be made");
		memset(shared, 0, sizeof(NvrDlssShared));
		shared->magic = NVR_DLSS_MAGIC;
		shared->version = NVR_DLSS_PROTOCOL_VERSION;
		shared->size = sizeof(NvrDlssShared);
		shared->mode = passThrough ? NVR_DLSS_MODE_PASSTHROUGH : NVR_DLSS_MODE_DLSS;
		shared->parentPid = GetCurrentProcessId();
		shared->vendorId = info.VendorId;
		memcpy(shared->deviceLuid, info.DeviceLUID, 8);
		shared->deviceLuidValid = info.DeviceLUIDValid ? 1u : 0u;
		memcpy(shared->deviceUuid, info.DeviceUUID, 16);
		memcpy(shared->driverUuid, info.DriverUUID, 16);
		shared->displayWidth = displayWidth;
		shared->displayHeight = displayHeight;
		shared->inputWidth = inputWidth;
		shared->inputHeight = inputHeight;
		shared->imageCount = imageCount;
		shared->features = (rrImages ? NVR_DLSS_FEATURE_RR : 0u) | (maskReady ? NVR_DLSS_FEATURE_BIAS_MASK : 0u);

		std::wstring cmdLine = L"\"" + exe + L"\" --shm " + base;
		// UNOFFICIAL 2026-10-30 (DLAA stall report): HelperWait / HelperGpuPriority (NvrDlssHelper.cpp's header says what they do)
		cmdLine += helperCpuWait ? L" --wait cpu" : L" --wait gpu";
		cmdLine += helperHighPriority ? L" --gpu-priority high" : L" --gpu-priority normal";
		// UNOFFICIAL 2026-10-30 (top-left picture report): HelperExactInputs / HelperFailTest 2 and 3 (1 is NVR's own: Exchange)
		cmdLine += helperExactInputs ? L" --inputs exact" : L" --inputs subrect";
		if (helperFailTest == 2) cmdLine += L" --fail-test report";
		else if (helperFailTest == 3) cmdLine += L" --fail-test hang";
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		QueryPerformanceCounter(&stepT[3]);	// the shared-memory block
		if (!CreateProcessW(exe.c_str(), &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS, nullptr, helperDir.c_str(), &si, &pi)) {
			sprintf_s(why, "NvrDlssHelper.exe could not be started (CreateProcess: Win32 error %lu)", GetLastError());
			return TurnOff(why);
		}
		CloseHandle(pi.hThread);
		process = pi.hProcess;
		QueryPerformanceCounter(&stepT[4]);	// CreateProcess

		// The NT handles (each a duplicate the fork makes of the one it exported), duplicated into the helper, which closes them
		// after importing. Every failure says which step and its code.
		auto give = [&](const char* what, HRESULT created, HANDLE local, uint64_t& remote) {
			if (FAILED(created) || !local) {
				sprintf_s(why, "the %s's handle could not be made (fork CreateHandle: %s; the DXVK log says more)", what, HrText(created).c_str());
				return false;
			}
			DWORD exitCode = 0;
			if (GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE) {
				CloseHandle(local);
				sprintf_s(why, "NvrDlssHelper.exe exited right after it started (exit code %lu; NvrDlssHelper.log next to it says why)", exitCode);
				return false;
			}
			HANDLE target = nullptr;
			if (!DuplicateHandle(GetCurrentProcess(), local, process, &target, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
				sprintf_s(why, "the %s's handle could not be given to the helper (DuplicateHandle into the helper process: Win32 error %lu)", what, GetLastError());
				return false;	// DUPLICATE_CLOSE_SOURCE closes the local handle even on failure
			}
			remote = (uint64_t)(uintptr_t)target;
			return true;
		};
		for (UInt32 i = 0; i < imageCount; i++) {
			NVR_EXT_SHARED_IMAGE_INFO image = {};
			images[i]->GetInfo(&image);
			NvrDlssImageDesc& d = shared->images[i];
			d.allocationSize = image.AllocationSize;
			d.memoryTypeIndex = image.MemoryTypeIndex;
			d.vkFormat = image.VkFormat;
			d.width = image.Width;
			d.height = image.Height;
			d.usage = image.Usage;
			d.flags = image.Flags;
			d.tiling = image.Tiling;
			d.layout = image.Layout;
			d.handleType = image.HandleType;
			d.dedicated = image.Dedicated ? 1u : 0u;
			HANDLE handle = nullptr;
			const HRESULT hr = images[i]->CreateHandle(&handle);
			char what[64];
			sprintf_s(what, "shared image '%s'", descs[i].pDebugName);
			if (!give(what, hr, handle, d.handle)) return TurnOff(why);
		}
		// UNOFFICIAL 2026-11-01 protocol 3: the bias current colour mask's image, the same way
		if (maskReady) {
			NVR_EXT_SHARED_IMAGE_INFO image = {};
			maskImage->GetInfo(&image);
			NvrDlssImageDesc& d = shared->biasMask;
			d.allocationSize = image.AllocationSize;
			d.memoryTypeIndex = image.MemoryTypeIndex;
			d.vkFormat = image.VkFormat;
			d.width = image.Width;
			d.height = image.Height;
			d.usage = image.Usage;
			d.flags = image.Flags;
			d.tiling = image.Tiling;
			d.layout = image.Layout;
			d.handleType = image.HandleType;
			d.dedicated = image.Dedicated ? 1u : 0u;
			HANDLE handle = nullptr;
			const HRESULT hr = maskImage->CreateHandle(&handle);
			if (!give("shared image 'NVR DLSS bias mask'", hr, handle, d.handle)) return TurnOff(why);
		}
		HANDLE handle = nullptr;
		HRESULT hr = inputsReady->CreateHandle(&handle);
		if (!give("'inputs ready' semaphore", hr, handle, shared->semInputsReady)) return TurnOff(why);
		handle = nullptr;
		hr = outputReady->CreateHandle(&handle);
		if (!give("'output ready' semaphore", hr, handle, shared->semOutputReady)) return TurnOff(why);
		QueryPerformanceCounter(&stepT[5]);	// the handles made and given
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper: %u images%s and 2 semaphores given to the helper (process %lu); the render thread spent %.1f ms: "
			"images + mask + fences %.1f, output texture %.1f, shared block %.1f, CreateProcess %.1f, handles %.1f.", imageCount,
			maskReady ? " + the bias current colour mask" : "", GetProcessId(process), stepMs(0, 5), stepMs(0, 1), stepMs(1, 2), stepMs(2, 3), stepMs(3, 4), stepMs(4, 5));
		MemoryBarrier();
		shared->setupDone = 1;
		SetEvent(frameEvent);

		frame = 0;
		rrFailed = false;
		rrNeedsReset = true;
		rrReported = 0;
		rrSent = 0;
		rrSkipLogged = false;
		watch.SetAdapter(info.DeviceLUID, info.DeviceLUIDValid != FALSE);
		watch.SetShared(shared);
		watch.Reset();
		runningFrames = 0;
		fsrFreed = false;
		InterlockedExchange64(&lastRecorded, 0);
		InterlockedExchange(&watchStop, 0);
		InterlockedExchange(&tripped, 0);
		watchdog = CreateThread(nullptr, 64 * 1024, WatchdogMain, this, 0, nullptr);
		if (!watchdog) return TurnOff("its watchdog thread could not be started");
		startTick = GetTickCount64();
		state = Starting;
		lastSerial = 0;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper started (%s, %ls): inputs %ux%u, display %ux%u, GPU %s; HelperWait %d (%s), HelperGpuPriority %d, "
			"HelperExactInputs %d, WeaponMask %.2f (%s)%s; FSR 2 runs until it is ready.", passThrough ? "pass-through test" : "DLSS", exe.c_str(), inputWidth, inputHeight, displayWidth,
			displayHeight, info.DeviceName, helperCpuWait ? 1 : 0, helperCpuWait ? "the helper waits on the CPU" : "the GPU waits", helperHighPriority ? 1 : 0,
			helperExactInputs ? 1 : 0, weaponMask, maskReady ? "bias current colour mask made" : "no bias mask this start", helperFailTest == 1 ? ", HelperFailTest 1 (TEST: Vegas Shaders ends the helper at its 120th frame)" :
			helperFailTest == 2 ? ", HelperFailTest 2 (TEST: the helper reports a failure at its 120th frame)" :
			helperFailTest == 3 ? ", HelperFailTest 3 (TEST: the helper stops answering at its 120th frame)" : "");
		return true;
	}

	// Ends the helper and releases everything. Waits recorded on the GPU are released first (the helper finishes them, or the
	// semaphore is signalled from the CPU), so the game's GPU queue never waits for a frame that will not come.
	void DlssUpscaler::Stop(const char* why) {
		SessionLog::StageScope stage("DLSS helper: stop (waits: its watchdog 3 s, submitted frames 1 s, the process 3 s at most)");	// (2026-11-07 perf-ptcost: STALL lines)
		if (why && state != Idle && state != Off) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper stopped: %s.", why);
		if (watchdog) {
			InterlockedExchange(&watchStop, 1);
			WaitForSingleObject(watchdog, 3000);
			CloseHandle(watchdog);
			watchdog = nullptr;
		}
		const UInt64 recorded = (UInt64)InterlockedCompareExchange64(&lastRecorded, 0, 0);
		if (process) {
			if (shared && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
				for (int i = 0; i < 200 && (UInt64)shared->submittedFrame < recorded; i++) Sleep(5);
				shared->quit = 1;
				SetEvent(frameEvent);
				if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0) TerminateProcess(process, 1);
			}
			CloseHandle(process);
			process = nullptr;
		}
		if (outputReady) {
			UINT64 done = 0;
			outputReady->GetCompletedValue(&done);
			if (done < recorded) outputReady->SignalFromHost(recorded + kReleaseMargin);
		}
		for (ID3D9NvrExtSharedImage*& image : images) DxvkCompute::Drop(image);
		// UNOFFICIAL 2026-11-01 the bias mask's image and texture (the pass stays: made once a session)
		DxvkCompute::Drop(maskImage);
		if (maskTex.Surface) { maskTex.Surface->Release(); maskTex.Surface = nullptr; }
		if (maskTex.Texture) { maskTex.Texture->Release(); maskTex.Texture = nullptr; }
		maskReady = false;
		maskThisFrame = false;
		DxvkCompute::Drop(inputsReady);
		DxvkCompute::Drop(outputReady);
		DxvkCompute::Drop(share);
		if (output.Surface) { output.Surface->Release(); output.Surface = nullptr; }
		if (output.Texture) { output.Texture->Release(); output.Texture = nullptr; }
		if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
		HANDLE* handles[] = { &mapping, &frameEvent, &statusEvent };
		for (HANDLE* h : handles) if (*h) { CloseHandle(*h); *h = nullptr; }
		inputWidth = inputHeight = displayWidth = displayHeight = 0;
		usedSelf = false;
		fsrNeedsReset = true;
		dlssNeedsReset = true;
		// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: no RR images any more; the RT denoisers come back next frame (WantsRaw).
		imageCount = 0;
		rrImages = false;
		rrNeedsReset = true;
		rrRanThisFrame = false;
		rrReported = 0;
	}

	void DlssUpscaler::Poll() {
		LogHelperMessage();
		if (state == Starting) {
			const UInt32 helperState = shared->state;
			if (helperState == NVR_DLSS_STATE_READY) {
				state = Running;
				dlssNeedsReset = true;
				rrNeedsReset = true;	// UNOFFICIAL 2026-10-30 (RR)
				reported = 0;
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper ready after %llu ms (%s); it upscales from the next frame on.",
					(unsigned long long)(GetTickCount64() - startTick), passThrough ? "pass-through test: the picture is only stretched, with a marker" : "NVIDIA DLSS");
			}
			else if (helperState == NVR_DLSS_STATE_FAILED || helperState == NVR_DLSS_STATE_EXITED) TurnOff("the helper gave up (its message is above)");
			else if (InterlockedCompareExchange(&tripped, 0, 0)) TurnOff(tripReason);
			else if (GetTickCount64() - startTick > 15000) TurnOff("the helper did not get ready within 15 seconds");
			return;
		}
		if (state == Running) {
			if (InterlockedCompareExchange(&tripped, 0, 0)) TurnOff(tripReason);
			else if (shared->state == NVR_DLSS_STATE_FAILED) TurnOff("the helper stopped (its message is above)");
		}
	}

	bool DlssUpscaler::FallBack(IDirect3DDevice9* Device, const Upscaler::Inputs& In) {
		usedSelf = false;
		runningFrames = 0;	// (review 2026-10-30) FSR 2 makes its textures again now; DLSS frees them again after 600 frames of its own
		fsrFreed = false;
		watch.Gap();
		Upscaler::Inputs in = In;
		if (fsrNeedsReset) in.Reset = true;
		fsrNeedsReset = false;
		return Upscaler::Fsr2()->Evaluate(Device, in);
	}

	// One frame's exchange, recorded in DXVK's stream (no waits). False: nothing was recorded (the caller falls back).
	bool DlssUpscaler::Record(IDirect3DDevice9* Device, const Upscaler::Inputs& In) {
		LARGE_INTEGER t0;
		QueryPerformanceCounter(&t0);
		const RECT render = { 0, 0, (LONG)In.RenderWidth, (LONG)In.RenderHeight };
		if (FAILED(share->CopyToShared(images[NVR_DLSS_IMAGE_COLOR], 0, 0, In.Color, &render)) ||
			FAILED(share->CopyToShared(images[NVR_DLSS_IMAGE_DEPTH], 0, 0, In.Depth, &render)) ||
			FAILED(share->CopyToShared(images[NVR_DLSS_IMAGE_MOTION], 0, 0, In.Motion, &render))) {
			TurnOff("an input could not be copied to the helper (formats or sizes; the DXVK log says more)");
			return false;
		}
		maskThisFrame = RecordMask(In);	// UNOFFICIAL 2026-11-01 (false: this frame without the mask, as before)
		Exchange(In, nullptr, t0);
		maskThisFrame = false;
		return true;
	}

	// UNOFFICIAL 2026-11-01 (player report: with DLSS the first-person arms and gun ghost and smear while they animate) DLSS SR's bias current
	// colour mask. FSR gets the weapon as reactive (WeaponReactive in FSRPrep / FSRUpPrep's depth.g) and DLSS got only colour, depth and
	// motion, where the weapon is depth 0 (nearest) and motion 0 (fixed to the camera): right for the camera, but its own sway, bob, recoil
	// and reload are in no motion vector, so DLSS's history smeared it. The mask tells DLSS to trust this frame's colour on the weapon.
	// The pass (Compute\DlssBiasMask.spv) once a session; the r16f texture and the shared image at every start. False: no mask (logged).
	bool DlssUpscaler::MakeMask(const Upscaler::Inputs& In) {
		auto no = [&](const char* why) {
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS: no bias current colour mask for the first-person weapon ([Shaders.TAA.DLSS] WeaponMask) this start: %s; "
				"DLSS runs without it, as before.", why);
			DxvkCompute::Drop(maskImage);
			if (maskTex.Surface) { maskTex.Surface->Release(); maskTex.Surface = nullptr; }
			if (maskTex.Texture) { maskTex.Texture->Release(); maskTex.Texture = nullptr; }
			return false;
		};
		if (!maskTried) {
			maskTried = true;
			const NVR_EXT_BINDING_DESC bindings[] = {
				{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 1, 2, NVR_EXT_BIND_STORAGE_IMAGE, 2, NVR_EXT_VIEW_2D, TRUE, 0 },
				{ 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },
			};
			maskPipeline = DxvkCompute::CreatePipeline("DlssBiasMask", bindings, _countof(bindings), 16, true);
		}
		if (!maskPipeline) return no("its pass could not be made (Compute\\DlssBiasMask.spv missing or refused)");
		TheTextureManager->InitTexture("TESR_DlssBiasMask", &maskTex.Texture, &maskTex.Surface, In.InputWidth, In.InputHeight, D3DFMT_R16F);
		if (!maskTex.Surface) return no("its texture could not be made");
		const NVR_EXT_SHARED_IMAGE_DESC desc = { In.InputWidth, In.InputHeight, NVR_EXT_SHARED_R16F, "NVR DLSS bias mask" };
		const HRESULT hr = share->CreateSharedImage(&desc, &maskImage);
		if (FAILED(hr) || !maskImage) {
			maskImage = nullptr;
			char why[192];
			sprintf_s(why, "its shared image could not be made (fork CreateSharedImage: %s)", HrText(hr).c_str());
			return no(why);
		}
		return true;
	}

	// This frame's mask into the shared image: true. False (nothing recorded for it): WeaponMask 0, no mask this start, or the pass failed
	// (logged; no mask until the helper starts again).
	bool DlssUpscaler::RecordMask(const Upscaler::Inputs& In) {
		if (!maskReady || weaponMask <= 0.0f || !maskPipeline || !maskTex.Texture || !maskImage) return false;
		ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
		if (!point) return false;
		NVR_EXT_RESOURCE_BINDING b[4] = {};
		b[0].Slot = 0; b[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[0].pResource = In.Depth;
		b[1].Slot = 1; b[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[1].pResource = In.ViewModel ? In.ViewModel : In.Depth;	// (a placeholder without the weapon)
		b[2].Slot = 2; b[2].Type = NVR_EXT_BIND_STORAGE_IMAGE; b[2].pResource = maskTex.Texture;
		b[3].Slot = 0; b[3].Type = NVR_EXT_BIND_SAMPLER; b[3].pResource = point;
		struct { UInt32 pointIndex, width, height; float weapon; } push = { 0, In.RenderWidth, In.RenderHeight, In.ViewModel ? weaponMask : -1.0f };
		NVR_EXT_DISPATCH_DESC dispatch = {};
		dispatch.pPipeline = maskPipeline;
		dispatch.BindingCount = _countof(b);
		dispatch.pBindings = b;
		dispatch.pPushData = &push;
		dispatch.PushDataSize = sizeof(push);
		dispatch.GroupsX = (In.RenderWidth + 7) / 8;
		dispatch.GroupsY = (In.RenderHeight + 7) / 8;
		dispatch.GroupsZ = 1;
		DxvkCompute::PreLoad(b, _countof(b));
		const RECT render = { 0, 0, (LONG)In.RenderWidth, (LONG)In.RenderHeight };
		if (FAILED(DxvkCompute::Extension()->Dispatch(&dispatch)) || FAILED(share->CopyToShared(maskImage, 0, 0, maskTex.Texture, &render))) {
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS: the bias current colour mask could not be made or copied to the helper; DLSS runs without it until "
				"the helper starts again.");
			maskReady = false;
			return false;
		}
		if (!maskFirstLogged) {
			maskFirstLogged = true;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS gets the bias current colour mask: %s, FSR's reactive value elsewhere (logged once).",
				In.ViewModel ? "the first-person weapon at WeaponMask" : "no weapon in this picture (RenderScale v1)");
		}
		return true;
	}

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: an RR frame's inputs (the seven: every image but the output) copied into the
	// shared images, then the same exchange as DLSS SR's. A failed copy ends RR until the helper starts again, never DLSS SR (the
	// copies already recorded only fill shared images nobody reads without a signal).
	bool DlssUpscaler::RecordRr(const Upscaler::RrInputs& In) {
		LARGE_INTEGER t0;
		QueryPerformanceCounter(&t0);
		const RECT render = { 0, 0, (LONG)In.RenderWidth, (LONG)In.RenderHeight };
		IDirect3DBaseTexture9* const sources[NVR_DLSS_IMAGE_COUNT] = {
			In.Color, In.Depth, In.Motion, nullptr, In.DiffuseAlbedo, In.SpecularAlbedo, In.Normals, In.Roughness,
		};
		for (UInt32 i = 0; i < imageCount; i++) {
			if (i == NVR_DLSS_IMAGE_OUTPUT) continue;
			if (FAILED(share->CopyToShared(images[i], 0, 0, sources[i], &render))) {
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction is off until the helper starts again: '%s' could not be copied to the helper (formats or sizes; the DXVK log says more); DLSS SR runs.",
					kImageNames[i]);
				rrFailed = true;
				return false;
			}
		}
		Exchange(In, &In, t0);
		return true;
	}

	// The part of a frame's exchange that DLSS SR and (UNOFFICIAL 2026-10-30) RR frames share, after the inputs' copies: the frame's
	// values, the semaphores, the copy back into TESR_DlssOutput, the timing and the log. Rr null: a DLSS SR frame (as before RR);
	// else an RR frame, with its matrices and options.
	void DlssUpscaler::Exchange(const Upscaler::Inputs& In, const Upscaler::RrInputs* Rr, const LARGE_INTEGER& t0) {
		LARGE_INTEGER t1, frequency;
		const UInt64 n = ++frame;
		NvrDlssFrame& f = shared->frames[n % NVR_DLSS_FRAME_RING];
		f.frame = 0;	// marks the slot as being written
		MemoryBarrier();
		f.renderWidth = In.RenderWidth;
		f.renderHeight = In.RenderHeight;
		// The helper keeps DLSS SR and DLSS-D as two NGX features, each with its own history: a frame of the other kind makes it stale.
		f.reset = (In.Reset || (Rr ? rrNeedsReset : dlssNeedsReset)) ? 1u : 0u;
		const bool hdr = Rr != nullptr || In.Hdr;	// RR's colour is always the linear pre-tonemap picture
		f.hdr = hdr ? 1u : 0u;
		// NVR's jitter is FSR 2's (render pixels); DLSS uses the motion vectors' coordinate system for it, as FSR 2 does.
		f.jitterX = flipX ? -In.JitterX : In.JitterX;
		f.jitterY = flipY ? -In.JitterY : In.JitterY;
		// Motion is previous uv - current uv of the picture: render pixels = uv x render size (DLSS: current + mv = previous).
		f.mvScaleX = (float)In.RenderWidth;
		f.mvScaleY = (float)In.RenderHeight;
		f.preExposure = 1.0f;
		f.frameTimeMs = In.DeltaSeconds * 1000.0f;
		f.quality = 0;		// the helper picks DLSS's quality mode from render / display size
		// UNOFFICIAL 2026-10-30 protocol 2: the frame's kind, DLSS SR's preset ([Shaders.TAA.DLSS] Preset; 0 = no hint, as before),
		// and for RR frames RR's preset, its two convention switches and the matrices.
		f.flags = Rr ? NVR_DLSS_FRAME_RR : (maskThisFrame ? NVR_DLSS_FRAME_BIAS_MASK : 0u);	// (UNOFFICIAL 2026-11-01 protocol 3: the mask)
		f.preset = Clamped(rr.Preset, NVR_DLSS_PRESET_COUNT);
		f.rrPreset = Rr ? Clamped(rr.RrPreset, NVR_DLSS_RR_PRESET_COUNT) : 0u;
		f.rrOptions = Rr ? ((rr.NormalsView ? NVR_DLSS_RR_NORMALS_VIEW : 0u) | (rr.DepthLinear ? NVR_DLSS_RR_DEPTH_LINEAR : 0u)) : 0u;
		f.pad = 0;
		if (Rr) {
			memcpy(f.worldToView, Rr->WorldToView, sizeof(f.worldToView));
			memcpy(f.viewToClip, Rr->ViewToClip, sizeof(f.viewToClip));
		}
		else {
			memset(f.worldToView, 0, sizeof(f.worldToView));
			memset(f.viewToClip, 0, sizeof(f.viewToClip));
		}
		LARGE_INTEGER written;
		QueryPerformanceCounter(&written);
		f.writtenQpc = written.QuadPart;
		MemoryBarrier();
		f.frame = n;
		MemoryBarrier();
		InterlockedExchange64((volatile LONG64*)&shared->latestFrame, (LONG64)n);
		SetEvent(frameEvent);
		InterlockedExchange64(&lastRecorded, (LONG64)n);
		{
			// UNOFFICIAL 2026-10-30: the copies' end, before the flush "inputs ready" makes (ExchangeWatch), with this frame's number and
			// the frames in flight then (the exchange-stall line)
			UINT64 back = 0;
			outputReady->GetCompletedValue(&back);
			watch.MidGpu(n, n > back ? n - back - 1 : 0, Rr ? "Ray Reconstruction" : (passThrough ? "pass-through test" : DlssModeName(In.RenderWidth, In.DisplayWidth)));
		}
		share->SignalShared(inputsReady, n);
		share->WaitShared(outputReady, n);
		share->CopyFromShared(output.Texture, 0, 0, images[NVR_DLSS_IMAGE_OUTPUT], nullptr);
		if (Rr) {
			rrNeedsReset = false;
			dlssNeedsReset = true;
			rrSent++;
		}
		else {
			dlssNeedsReset = false;
			rrNeedsReset = true;
			usedSelf = true;
		}
		fsrNeedsReset = true;
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&frequency);
		recordUs += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)frequency.QuadPart;
		recordCount++;

		// UNOFFICIAL 2026-10-30 (DLAA drops report): the mode for the log lines, the frame's interval (and its slow-frame line).
		char mode[96];
		sprintf_s(mode, "%s %s, %s, preset %s", passThrough ? "the pass-through test as" : "DLSS", DlssModeName(In.RenderWidth, In.DisplayWidth),
			Rr ? "Ray Reconstruction" : (hdr ? "HDR" : "LDR"), DlssPresetName(f.preset));
		{
			UINT64 back = 0;
			outputReady->GetCompletedValue(&back);
			if (frameStamp - exchangeStamp > 1) watch.Gap();	// frames without an exchange went by (the effects did not run)
			exchangeStamp = frameStamp;
			watch.Frame(mode, shared, n > back ? n - back : 0);
		}
		// FSR 2's own textures (about 190 MB at 1920x1080) were made for the second or so the helper needed to start; while DLSS runs
		// they only take video memory (an 8 GB card plays at its limit). Freed after 600 good frames; FSR 2 makes them again on its
		// first frame should DLSS stop (logged).
		// UNOFFICIAL 2026-10-30 TEST ONLY (HelperFailTest 1): the helper process ended from here, as a crash would end it (the watchdog sees it,
		// releases this frame's wait and NVR goes to FSR 2 for the session).
		if (helperFailTest == 1 && runningFrames == 119 && process) {
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS helper: HelperFailTest 1: Vegas Shaders ends the helper process now (test).");
			TerminateProcess(process, 3);
		}
		if (!fsrFreed && ++runningFrames >= 600) {
			fsrFreed = true;
			Upscaler::Fsr2()->ReleaseResources();
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS has run 600 frames: FSR 2's own textures (its stand-in while the helper starts, about %u MB here) are freed; "
				"FSR 2 makes them again if DLSS stops.", (unsigned)(190.0 * In.DisplayWidth * In.DisplayHeight / (1920.0 * 1080.0)));
		}

		// The log: the first three reports at 300 frame steps, then every 3600 frames (every 300 with F10 profiling on).
		if ((n % 300) == 0 && (reported < 3 || GpuTimer::Enabled || (n % 3600) == 0)) {
			reported++;
			UINT64 done = 0;
			outputReady->GetCompletedValue(&done);
			char rrText[128] = "";	// UNOFFICIAL 2026-10-30: only for a start with RR's images (the line is as before without)
			if (rrImages) sprintf_s(rrText, "; Ray Reconstruction: %llu frames sent, %u evaluated by the helper", (unsigned long long)rrSent, shared->rrEvaluated);
			// UNOFFICIAL 2026-10-30 (DLAA drops report): the mode first, then the window's frame / exchange / video memory figures.
			char watched[512];
			watch.Report(watched, sizeof(watched));
			Logger::Log("DLSS HELPER (%s): %llu frames exchanged, last recorded %llu, back from the helper %llu; helper GPU %.3f ms avg (last %.3f), "
				"helper response %.3f ms avg (max %.3f), %u overwritten, %u DLSS evaluations; Vegas Shaders records %.3f ms CPU avg; %ux%u -> %ux%u %s%s%s",
				mode, (unsigned long long)shared->framesSubmitted, (unsigned long long)n, (unsigned long long)done,
				shared->gpuTimeUsAvg / 1000.0, shared->gpuTimeUsLast / 1000.0, shared->responseUsAvg / 1000.0, shared->responseUsMax / 1000.0,
				shared->paramMismatches, shared->dlssEvaluated, recordCount ? recordUs / recordCount / 1000.0 : 0.0,
				In.RenderWidth, In.RenderHeight, In.DisplayWidth, In.DisplayHeight, hdr ? "HDR" : "LDR", rrText, watched);
			shared->responseUsMax = 0;
			recordUs = 0.0;
			recordCount = 0;
		}
	}

	bool DlssUpscaler::Evaluate(IDirect3DDevice9* Device, const Upscaler::Inputs& In) {
		if (state == Off) return FallBack(Device, In);
		if (!In.Color || !In.Depth || !In.Motion || !In.RenderWidth || !In.RenderHeight || !In.DisplayWidth || !In.DisplayHeight ||
			In.RenderWidth > In.InputWidth || In.RenderHeight > In.InputHeight) return FallBack(Device, In);
		if (state == Idle) {
			Start(In);	// FSR 2 runs this frame either way; the helper takes over once it says it is ready
			return FallBack(Device, In);
		}
		Poll();
		if (state != Running) return FallBack(Device, In);
		if (In.InputWidth != inputWidth || In.InputHeight != inputHeight || In.DisplayWidth != displayWidth || In.DisplayHeight != displayHeight) {
			// Another resolution (the native path and RenderScale take turns, or the screen changed): the helper starts again.
			Stop("the input or output size changed");
			state = Idle;
			return FallBack(Device, In);
		}
		static GpuTimer timer("  DLSS helper (copies + helper's work + waits)");
		GpuProfileScope gpu(timer, Device);
		watch.BeginGpu(Device);	// UNOFFICIAL 2026-10-30: the always-on exchange time (ExchangeWatch)
		const bool recorded = Record(Device, In);
		watch.EndGpu(recorded);
		if (!recorded) return FallBack(Device, In);
		return true;
	}

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction: RR can run this frame. Nothing at all while RR is not wanted (the old path). The
	// helper is polled here too, so a dead or failed helper is noticed on frames where only the RR point asks (Poll's own TurnOff:
	// FSR 2 for the session, as from Evaluate). Each change of readiness is logged once.
	bool DlssUpscaler::RrReady() {
		if (!rr.Wanted) return false;
		if (state == Starting || state == Running) Poll();
		const bool running = state == Running && rrImages && !rrFailed && shared && !InterlockedCompareExchange(&tripped, 0, 0);
		const bool ready = running && shared->rrAvailable != 0;
		if (ready && rrReported != 1) {
			rrReported = 1;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction ready (%s).", passThrough ? "the helper's pass-through test" : "NVIDIA DLSS-D");
		}
		else if (running && !ready && rrReported != 2) {
			// The helper runs DLSS SR but not RR (NGX's SuperSamplingDenoising says no, or DLSS-D failed): the helper's message says why.
			rrReported = 2;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction not available (the helper's message is above); DLSS SR runs.");
		}
		return ready;
	}

	// One RR frame: false (nothing recorded) when RR is not ready, an input is missing or the sizes are not the helper's. A size change
	// is not restarted from here: the frame goes to DLSS SR after the tonemap, whose Evaluate starts the helper again.
	bool DlssUpscaler::EvaluateRr(IDirect3DDevice9* Device, const Upscaler::RrInputs& In) {
		if (!RrReady()) return false;
		const bool missing = !In.Color || !In.Depth || !In.Motion || !In.DiffuseAlbedo || !In.SpecularAlbedo || !In.Normals || !In.Roughness ||
			!In.RenderWidth || !In.RenderHeight || In.RenderWidth > In.InputWidth || In.RenderHeight > In.InputHeight;
		const bool otherSize = In.InputWidth != inputWidth || In.InputHeight != inputHeight || In.DisplayWidth != displayWidth || In.DisplayHeight != displayHeight;
		if (missing || otherSize) {
			if (!rrSkipLogged) {
				rrSkipLogged = true;
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: DLSS Ray Reconstruction skipped a frame: %s (inputs %ux%u -> %ux%u, the helper's %ux%u -> %ux%u); "
					"DLSS SR runs that frame (logged once per helper start).", missing ? "an input is missing" : "the sizes are not the helper's",
					In.InputWidth, In.InputHeight, In.DisplayWidth, In.DisplayHeight, inputWidth, inputHeight, displayWidth, displayHeight);
			}
			return false;
		}
		static GpuTimer timer("  DLSS RR (copies + helper's work + waits)");
		GpuProfileScope gpu(timer, Device);
		watch.BeginGpu(Device);
		const bool recorded = RecordRr(In);
		watch.EndGpu(recorded);
		if (!recorded) return false;
		return true;	// (rrRanThisFrame: RayReconstructionDelivered, once the RR point's composite is in the frame)
	}
}

namespace Upscaler {
	IUpscaler* Dlss() { return &TheDlss; }

	void DlssOptions(bool PassThroughTest, int JitterFlip, bool HelperCpuWait, bool HelperHighPriority, bool HelperExactInputs, int HelperFailTest) {
		TheDlss.SetOptions(PassThroughTest, JitterFlip, HelperCpuWait, HelperHighPriority, HelperExactInputs, HelperFailTest);
	}
	void DlssWeaponMask(float Value) { TheDlss.SetWeaponMask(Value); }	// UNOFFICIAL 2026-11-01

	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (Upscaler.h): the same helper, semaphores and frame counter as Dlss().
	void DlssRrOptions(const RrOptions& Options) { TheDlss.SetRrOptions(Options); }
	const RrOptions& DlssRrSettings() { return TheDlss.RrSettings(); }
	bool RayReconstructionReady() { return TheDlss.RrReady(); }
	bool EvaluateRayReconstruction(IDirect3DDevice9* Device, const RrInputs& In) { return TheDlss.EvaluateRr(Device, In); }
	IDirect3DTexture9* RayReconstructionOutput() { return TheDlss.RrOutput(); }
	void RayReconstructionFrameBegin() { TheDlss.RrFrameBegin(); }
	bool RayReconstructionWantsRaw() { return TheDlss.RrWantsRaw(); }
	void RayReconstructionPlan(bool Planned) { TheDlss.RrPlan(Planned); }
	void RayReconstructionDelivered() { TheDlss.RrDelivered(); }

	IUpscaler* Choose(int setting) {
		// UNOFFICIAL 2026-10-28 ([Shaders.TAA.FSR] Version 1): FSR 3.1, or FSR 2 once it has failed (logged by it). A switch to or
		// from FSR 3.1 restarts the history of the one that runs now (the other's frames went by meanwhile) and frees the textures of
		// the FSR that stopped (both sets would otherwise stay in video memory); FSR 2 / DLSS switches are as before.
		static IUpscaler* last = nullptr;
		IUpscaler* chosen;
		if (setting == 1 && !TheDlss.IsOff()) chosen = &TheDlss;	// it runs FSR 2 itself while the helper starts or when it fails
		else {
			TheDlss.MarkUnused();
			chosen = (setting == 2 && !Fsr31()->Failed()) ? Fsr31() : Fsr2();
		}
		// UNOFFICIAL 2026-10-29 (live switching): every change of the one that runs restarts its history (the other one's frames went by;
		// FSR 2 <-> DLSS too, not only to or from FSR 3.1), frees the textures of an FSR that stopped -- except FSR 2's while DLSS runs,
		// which falls back on them -- and is logged.
		if (last && chosen != last) {
			chosen->RequestReset();
			if (last == Fsr31() || (last == Fsr2() && chosen != &TheDlss)) last->ReleaseResources();
			if (last == &TheDlss && chosen == Fsr31()) Fsr2()->ReleaseResources();	// DLSS's fallback textures, unused now
			Logger::Log("UNOFFICIAL upscaler switched live: %s -> %s (history restarted)", last->Name(), chosen->Name());
		}
		last = chosen;
		return chosen;
	}
}
