#pragma once

// Full-game performance probe ("Help > Performance probe..."). Nothing samples, patches ScriptRunner::Run or inspects geometry until
// the user starts a timed capture (5s / 10s / 20s) or live mode. After the first capture a little stays for the session: the
// ScriptRunner::Run detour (passes straight through), CpuSplit's patches (a stamp at each patched call every frame) and the writer
// thread (wakes 5 times a second). Combines:
//   1. Frame time distribution (0.1 ms histogram) & CPU Update vs Render CPU vs Present/GPU-Wait split
//   2. CpuSplit per-engine-stage breakdown (Main thread + AI worker threads)
//   3. Stack-sampling profiler broken down by every loaded DLL (FalloutNV.exe, VegasShaders.dll, every NVSE plugin, wrappers,
//      DXVK/driver): "own" (exclusive), "near" (first plugin DLL among the next return addresses: engine work a plugin's hook calls)
//      and inclusive time
//   4. Lazy Detours hook on ScriptRunner::Run (0x005E0D20) for per-script & per-ESP/ESM mod inclusive/self timing
//   5. 1-frame Scene Census: draw calls by owning ESP/ESM & mesh, skinned draws, shader binds, active ShadowSceneNode lights, ProcessLists actors, VAS memory
//   6. Spike frames (> SpikeMs or > 2.5x the 10 s mean, loading screens left out) with their top DLLs, stage and script
// Timed capture: a report VegasShaders-Logs\DeepProbe_<timestamp>.txt and the in-menu tabs. Live mode (UNOFFICIAL 2026-11-28,
// ported from room 01's Frame Cost Logger): runs until stopped, 1 s / 10 s / session windows in the menu and a small HUD, a report
// folder VegasShaders-Logs\DeepProbe_Live_<timestamp>\ rewritten every AutosaveSeconds. Reports are built and written on the
// writer thread (DeepProbeWriter.h); the main thread only copies counters.
//
// Sampler safety rules (2026-11-28, from Frame Cost Logger 1.2 after a sampler froze a heavily modded game):
// - While a thread is suspended the sampler calls nothing but GetThreadContext and reads only that thread's TEB and its stack
//   between ESP and TEB.StackBase, after checking ESP >= TEB.StackLimit (read fresh: that range is committed). No locks, no
//   allocation, no exception handling, no module lookup, no other API (TakeSample).
// - Return addresses are checked only against executable sections listed (and checked readable) when the module table was built
//   (ScanModules, outside any suspension, with no lock: the probe has none).
// - A watchdog thread (made before the sampler runs, gone only after it has exited) resumes the thread and turns the sampler off when a
//   suspension lasts over 20 ms, exactly one resume per suspend, and notes when no frame came for 4 s with the game in front (the
//   writer thread logs both: the watchdog never allocates, logs or calls user32 itself).
// - One sampler at a time also means none while the test build's CpuSampler or ArrivalSampler runs.
// - The main thread never waits on the sampler: there is no lock; stopping waits at most 250 ms for both threads, and a thread that
//   does not stop leaves the sampler off for the session (its state is leaked, never freed under it).
// - One sampler at a time: one capture at a time, and none while room 01's FrameCostLogger.dll is loaded.

#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <tlhelp32.h>
#include <intrin.h>
#include <share.h>
#include <stdio.h>

#include "CpuSplit.h"
#include "GpuProfiler.h"
#include "FrameTimeStats.h"
#include "Branding.h"
#include "Paths.h"
#include "DxvkCompute.h"
#include "DeepProbeWriter.h"

namespace DeepProbe {

	static constexpr unsigned kMaxModules = 256;
	static constexpr unsigned kNoModule = kMaxModules;			// tally index: code outside any listed module
	static constexpr unsigned kMaxRanges = 1024;
	static constexpr unsigned kSamplerHz = 1000;				// timed capture
	static constexpr unsigned kStackCopyMax = 128 * 1024;		// timed capture (the buffer's size)
	static constexpr unsigned kLiveStackCopy = 16 * 1024;		// live mode: shorter copy, shorter suspension
	static constexpr unsigned kMaxScripts = 8192;
	static constexpr unsigned kMaxCensusMeshes = 4096;
	static constexpr unsigned kNoBucket = CpuSplit::kBucketCount;
	static constexpr unsigned kRows = CpuSplit::kBucketCount + 1;
	static constexpr unsigned kFrameBins = 10000;				// 0.1 ms bins up to 1 s (+1 overflow bin)
	static constexpr double kFrameBinMs = 0.1;
	static constexpr unsigned kStageBins = 1000;				// 0.05 ms bins up to 50 ms (+1 overflow bin)
	static constexpr double kStageBinMs = 0.05;
	static constexpr unsigned kSnapCount = 11;					// once-a-second history: 10 whole seconds
	static constexpr unsigned kExeSlots = 8192;					// FalloutNV.exe hot addresses (bounded table)
	static constexpr unsigned kFrameRing = 1024;
	static constexpr unsigned kSpikeRing = 64;
	static constexpr unsigned kSpikeTop = 4;
	static constexpr unsigned kTouchedMax = 64;
	static constexpr unsigned kRecent = 300;
	static constexpr double kMaxSuspendMs = 20.0;
	static constexpr double kStallNoteMs = 4000.0;
	static constexpr DWORD kStopWaitMs = 250;

	enum ModRole : UInt8 {
		rGame = 0,
		rVegasShaders,
		rNvsePlugin,
		rOtherTool,
		rDriver,
		rWindowsOther,
		rSystemWrap,
	};

	static const char* RoleLabel(ModRole r) {
		switch (r) {
		case rGame:         return "Game";
		case rVegasShaders: return "Vegas Shaders";
		case rNvsePlugin:   return "NVSE Plugin";
		case rOtherTool:    return "DLL / Overlay";
		case rDriver:       return "Driver / DXVK";
		case rWindowsOther: return "Windows";
		case rSystemWrap:   return "OS / CRT";
		default:            return "Other";
		}
	}
	// The DLLs "near" time is charged to: mods (plugins, overlays) and Vegas Shaders; not the game, drivers or Windows.
	inline bool IsPluginRole(ModRole r) { return r == rVegasShaders || r == rNvsePlugin || r == rOtherTool; }

	// ---- Summary data surfaced to ImGui window ---------------------------------------------------------------------------

	struct StageRow {
		std::string name;
		double avgMs = 0.0, p95Ms = 0.0, maxMs = 0.0, pct = 0.0, busyPct = -1.0;
	};
	struct DllRow {
		std::string name, role, topStage, mo2Mod;
		double exclMs = 0.0, exclPct = 0.0, inclMs = 0.0, inclPct = 0.0, waitPct = 0.0, nearMs = 0.0, nearPct = 0.0;
		unsigned exclSamples = 0, inclSamples = 0, nearSamples = 0;
	};
	struct DriverCallerRow {
		std::string callerName, callerRole;
		double ms = 0.0, pct = 0.0;
		unsigned samples = 0;
	};
	struct ScriptModRow {
		std::string modName;
		UInt8 modIdx = 0xFF;
		double selfMs = 0.0, callsPerFrame = 0.0, maxCallMs = 0.0;
		unsigned uniqueScripts = 0;
	};
	struct ScriptRow {
		std::string label, modName, contextMod;
		UInt32 formID = 0, targetID = 0;
		double selfMs = 0.0, inclMs = 0.0, callsPerFrame = 0.0, maxCallMs = 0.0;
		unsigned long long calls = 0;
	};
	struct GpuPassRow {
		std::string name;
		double avgMs = 0.0, maxMs = 0.0;
		unsigned samples = 0;
	};
	struct DrawModRow {
		std::string modName;
		unsigned draws = 0, skinned = 0;
	};
	struct DrawMeshRow {
		std::string meshName, modName;
		unsigned draws = 0;
	};
	struct SpikeRow {
		UInt32 frame = 0;
		double atSec = 0.0, ms = 0.0, stageMs = 0.0, scriptMs = 0.0;
		std::string dlls, stage, script;	// dlls: "name 40%; name 25%" (sampled, near view); empty when no sample fell in it
	};

	struct Summary {
		bool valid = false;
		bool live = false, final = true;
		double durationSec = 0.0;
		unsigned frames = 0, loadingFrames = 0, spikesTotal = 0;
		double loadingMs = 0.0;
		double avgMs = 0.0, p50Ms = 0.0, p95Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
		double avgFps = 0.0, low1Fps = 0.0, low01Fps = 0.0;
		double updateMs = 0.0, renderCpuMs = 0.0, presentMs = 0.0, gpuVegasMs = 0.0;
		double ai1BusyMs = 0.0, ai2BusyMs = 0.0;
		double vasUsedMB = 0.0, vasTotalMB = 0.0;
		double probeMs = 0.0, probeSamplerMs = 0.0, probeScriptMs = 0.0, probeOwnMs = 0.0, stallUsPerSample = 0.0;
		unsigned loadedPlugins = 0, loadedDlls = 0;
		unsigned censusWorldDraws = 0, censusFirstPersonDraws = 0, censusReflDraws = 0, censusOtherDraws = 0;
		unsigned censusTotalDraws = 0, censusSkinnedDraws = 0, censusShaderBinds = 0;
		unsigned activeLights = 0, totalLights = 0;
		unsigned actorsHigh = 0, actorsMidHigh = 0, actorsMidLow = 0, actorsLow = 0;
		unsigned samplerMainSamples = 0, samplerAi1Samples = 0, samplerAi2Samples = 0, samplerMissed = 0, watchdogTrips = 0;
		unsigned long long totalScriptCalls = 0;
		double totalScriptSelfMs = 0.0;
		bool scriptHookOk = false;
		std::string cellDesc, runtimeDesc, verdict, folder;
		std::vector<StageRow> stages;
		std::vector<DllRow> dlls;
		std::vector<DriverCallerRow> driverCallers;
		std::vector<ScriptModRow> scriptMods;
		std::vector<ScriptRow> scripts;
		std::vector<GpuPassRow> gpuPasses;
		std::vector<DrawModRow> drawMods;
		std::vector<DrawMeshRow> drawMeshes;
		std::vector<SpikeRow> spikes;		// newest first
	};

	// ---- Live view (main thread, rebuilt at most 4 times a second while shown) ---------------------------------------------

	struct LiveDll { std::string name, role; double nearMs = 0.0, ownMs = 0.0; };
	struct LiveScriptMod { std::string name; double ms = 0.0; };
	struct LiveStage { const char* name = ""; double ms = 0.0; };
	struct LiveSpike { double agoSec = 0.0; float ms = 0.0f; std::string dlls, stage, script; };
	struct LiveView {
		bool live = false;
		int windowSec = 10;
		double recSec = 0.0, windowAvgMs = 0.0, avg1Ms = 0.0, avg10Ms = 0.0, avgSessionMs = 0.0, p99SessionMs = 0.0, worst10Ms = 0.0;
		double updateMs = 0.0, renderMs = 0.0, presentMs = 0.0;
		double probeMs = 0.0, probeSamplerMs = 0.0, probeScriptMs = 0.0, probeOwnMs = 0.0, stallUs = 0.0, samplesPerSec = 0.0;
		double nextAutosaveSec = -1.0;
		unsigned frames = 0, spikes = 0, windowSamples = 0, missed = 0, watchdogTrips = 0;
		bool samplerOff = false;
		std::string folder;
		std::vector<LiveDll> dlls;
		std::vector<LiveScriptMod> scriptMods;
		std::vector<LiveStage> stages;
		std::vector<LiveSpike> spikesList;	// newest first
		float recent[kRecent] = {};
		int recentHead = 0;
	};

	// ---- Pure-POD SEH helpers (no C++ objects requiring stack unwinding inside __try) -------------------------------------

	inline void SafeCopyModName(UInt8 modIdx, char* out, size_t cap) {
		if (!out || cap == 0) return;
		out[0] = 0;
		if (modIdx == 0xFF) { strcpy_s(out, cap, "[Runtime / xNVSE]"); return; }
		if (DataHandler) {
			ModInfo* const* table = nullptr;
			const UInt32 count = DataHandler->modList.SafeLoadedMods(table);
			if (table && (UInt32)modIdx < count && ModList::SafeCopyName(table[modIdx], out, cap)) return;
		}
		sprintf_s(out, cap, "[Mod %02X]", (unsigned)modIdx);
	}

	inline void SafeCopyCellText(char* out, size_t cap) {
		if (!out || cap == 0) return;
		strcpy_s(out, cap, "none");
		__try {
			TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
			if (!cell) return;
			if (cell->IsInterior()) {
				const char* name = cell->GetEditorName();
				sprintf_s(out, cap, "interior '%s' (%08X)", name && name[0] ? name : "?", cell->refID);
			}
			else {
				TESWorldSpace* world = cell->worldSpace;
				const char* name = world ? world->GetEditorName() : nullptr;
				if (cell->coords)
					sprintf_s(out, cap, "exterior '%s' (%08X) cell %d,%d", name && name[0] ? name : "?", world ? world->refID : 0, cell->coords->x, cell->coords->y);
				else
					sprintf_s(out, cap, "exterior '%s' (%08X)", name && name[0] ? name : "?", world ? world->refID : 0);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			strcpy_s(out, cap, "unavailable");
		}
	}

	struct ScriptSnapshot {
		UInt32 formID = 0;
		UInt32 targetID = 0;
		UInt32 targetBaseID = 0;
		UInt8 modIdx = 0xFF;
		UInt8 targetModIdx = 0xFF;
		char editorName[64] = {};
		char snippet[64] = {};
		bool ok = false;
	};

	static void SafeSanitizeFirstLine(const char* src, char* dst, size_t cap) {
		if (!dst || cap == 0) return;
		dst[0] = 0;
		if (!src) return;
		size_t r = 0, w = 0;
		while (src[r] && (src[r] == ' ' || src[r] == '\t' || src[r] == '\r' || src[r] == '\n') && r < 256) r++;
		while (src[r] && src[r] != '\r' && src[r] != '\n' && w + 1 < cap && r < 256) {
			const unsigned char c = (unsigned char)src[r++];
			dst[w++] = (c >= 32 && c < 127) ? (char)c : ' ';
		}
		while (w > 0 && dst[w - 1] == ' ') w--;
		dst[w] = 0;
	}

	inline bool SafeReadScriptKey(void* scriptPtr, UInt32& formID, UInt8& modIdx) {
		formID = 0; modIdx = 0xFF;
		__try {
			if (!scriptPtr || (uintptr_t)scriptPtr < 0x10000) return false;
			formID = ((const TESForm*)scriptPtr)->refID;
			modIdx = (UInt8)(formID >> 24);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	inline void SafeSnapshotScript(void* scriptPtr, void* objPtr, ScriptSnapshot* out) {
		if (!out) return;
		memset(out, 0, sizeof(*out));
		out->modIdx = 0xFF;
		out->targetModIdx = 0xFF;
		__try {
			if (!scriptPtr || (uintptr_t)scriptPtr < 0x10000) return;
			TESForm* f = (TESForm*)scriptPtr;
			out->formID = f->refID;
			out->modIdx = (UInt8)(out->formID >> 24);
			const uintptr_t vt = *(const uintptr_t*)scriptPtr;
			if (vt > 0x10000) {
				const char* ed = f->GetEditorName();
				if (ed && (uintptr_t)ed > 0x10000 && ed[0]) SafeSanitizeFirstLine(ed, out->editorName, sizeof(out->editorName));
			}
			// Script::text lives at +0x2C (verified from Script::SetText at 0x005ABE50)
			const char* rawText = *(const char* const*)((const UInt8*)scriptPtr + 0x2C);
			if (rawText && (uintptr_t)rawText > 0x10000 && rawText[0]) {
				SafeSanitizeFirstLine(rawText, out->snippet, sizeof(out->snippet));
			}
			if (objPtr && (uintptr_t)objPtr > 0x10000) {
				const TESForm* of = (const TESForm*)objPtr;
				out->targetID = of->refID;
				// Check reference form types (REFR 0x3A .. PFLA 0x40)
				if (of->formType >= 0x3A && of->formType <= 0x40) {
					const TESObjectREFR* ref = (const TESObjectREFR*)objPtr;
					if (ref->baseForm && (uintptr_t)ref->baseForm > 0x10000) {
						out->targetBaseID = ref->baseForm->refID;
						out->targetModIdx = (UInt8)(out->targetBaseID >> 24);
					}
					else {
						out->targetModIdx = (UInt8)(out->targetID >> 24);
					}
				}
				else {
					out->targetModIdx = (UInt8)(out->targetID >> 24);
				}
			}
			out->ok = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	inline void SafeExtractDrawOwner(const NiGeometry* geom, UInt8& modIdx, char* meshName, size_t meshCap) {
		modIdx = 0xFF;
		if (meshName && meshCap) meshName[0] = 0;
		__try {
			if (!geom) return;
			if (meshName && meshCap && geom->m_pcName && geom->m_pcName[0]) {
				strncpy_s(meshName, meshCap, geom->m_pcName, _TRUNCATE);
			}
			const NiAVObject* cur = (const NiAVObject*)geom;
			const TESObjectREFR* foundRef = nullptr;
			const char* fadeName = nullptr;
			for (int depth = 0; cur && depth < 16; depth++) {
				if (const_cast<NiAVObject*>(cur)->IsFadeNode()) {
					if (!fadeName && cur->m_pcName && cur->m_pcName[0]) fadeName = cur->m_pcName;
					// BSFadeNode::GetLinkedObj (0x009AD610): mov eax, [ecx+0xCC]
					const TESObjectREFR* r = *(const TESObjectREFR* const*)((const UInt8*)cur + 0xCC);
					if (r && (uintptr_t)r > 0x10000) { foundRef = r; break; }
				}
				cur = (const NiAVObject*)cur->m_parent;
			}
			if (meshName && meshCap) {
				if (fadeName && fadeName[0]) strncpy_s(meshName, meshCap, fadeName, _TRUNCATE);
				else if (!meshName[0] && geom->m_parent && geom->m_parent->m_pcName && geom->m_parent->m_pcName[0])
					strncpy_s(meshName, meshCap, geom->m_parent->m_pcName, _TRUNCATE);
				if (!meshName[0]) strcpy_s(meshName, meshCap, "(unnamed)");
			}
			if (foundRef) {
				if (foundRef->baseForm && (uintptr_t)foundRef->baseForm > 0x10000)
					modIdx = (UInt8)(foundRef->baseForm->refID >> 24);
				else
					modIdx = (UInt8)(foundRef->refID >> 24);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	inline void SafeSnapshotWorldStats(unsigned& activeLights, unsigned& totalLights, unsigned& aHigh, unsigned& aMidH, unsigned& aMidL, unsigned& aLow, unsigned& modCount) {
		activeLights = totalLights = aHigh = aMidH = aMidL = aLow = modCount = 0;
		if (DataHandler) { ModInfo* const* t = nullptr; modCount = (unsigned)DataHandler->modList.SafeLoadedMods(t); }
		__try {
			if (const ShadowSceneNode* ssn = BSShaderManager::GetShadowSceneNode(BSShaderManager::BSSM_SSN_WORLD)) {
				unsigned guard = 0;
				for (NiTList<ShadowSceneLight>::Entry* e = ssn->lights.start; e && guard < 4096; e = e->next, guard++) {
					const ShadowSceneLight* l = e->data;
					if (!l || !l->bPointLight || !l->sourceLight) continue;
					totalLights++;
					const UInt8* raw8 = (const UInt8*)l;
					const bool hidden = (((const UInt8*)l->sourceLight)[0x30] & 1) != 0;
					const bool active = *(const UInt16*)(raw8 + 0x110) != 0xFF && !hidden && !raw8[0xEC];
					if (active) activeLights++;
				}
			}
			const UInt8* lists = (const UInt8*)0x11E0E80;
			auto levelCount = [&](int lvl) -> unsigned {
				const UInt32 begin = *(const UInt32*)(lists + 0x14 + lvl * 4);
				const UInt32 end = *(const UInt32*)(lists + 0x24 + lvl * 4);
				if (end < begin || end - begin > 200000) return 0;
				return (unsigned)(end - begin);
			};
			aHigh = levelCount(0);
			aMidH = levelCount(1);
			aMidL = levelCount(2);
			aLow = levelCount(3);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// ---- Hot-path state ---------------------------------------------------------------------------------------------------

	inline std::atomic<bool> Running{ false };
	inline bool InCensusFrame = false;

	namespace Detail {
		struct Range { DWORD Start, End; int Module; };

		struct ScriptRec {
			UInt64 key = 0;
			UInt32 formID = 0;
			UInt32 targetID = 0;
			UInt8 modIdx = 0xFF;
			UInt8 targetModIdx = 0xFF;
			unsigned long long calls = 0;
			unsigned long long selfTicks = 0;
			unsigned long long inclTicks = 0;
			unsigned long long maxCallTicks = 0;
			char editorName[64] = {};
			char snippet[64] = {};
		};

		struct GpuStat {
			double sumMs = 0.0, maxMs = 0.0;
			unsigned samples = 0;
		};

		struct CensusMod {
			unsigned draws = 0, skinned = 0;
		};

		// FalloutNV.exe hot address: key = (bucket << 24) | rva (rva < 16 MB), 0 = empty. Written by the sampler thread only.
		struct ExeLeaf { volatile LONG key; volatile LONG count; };

		// One spike frame's sampled share, by near module (the sampler thread's output; the main thread joins it to its SpikeRec)
		struct SpikeDll { LONG frame; UInt16 total; UInt8 n; SInt16 mod[kSpikeTop]; UInt16 cnt[kSpikeTop]; };

		struct SpikeRec {
			LONG frame = 0;
			float ms = 0.0f, stageMs = 0.0f, scriptMs = 0.0f, scriptTotalMs = 0.0f;
			double atMs = 0.0;
			UInt8 stage = 0xFF;				// CpuSplit bucket, 0xFF = none
			UInt16 scriptMod = 0x100;		// load-order index, 0x100 = none
			bool dllsIn = false;
			UInt16 dllTotal = 0;
			UInt8 dllN = 0;
			SInt16 dllMod[kSpikeTop] = {};
			UInt16 dllCnt[kSpikeTop] = {};
		};

		// Every plain counter of a capture: copied whole (no locks) when a report is made. The sampler and watchdog threads write
		// the sampler part while the main thread copies it: a copy may be a sample behind, never inconsistent in a way that matters.
		struct Counters {
			// module table (sampler thread; an entry is complete before modCount covers it)
			char modNames[kMaxModules][64];
			char modPaths[kMaxModules][260];
			DWORD modBase[kMaxModules], modSize[kMaxModules];
			ModRole modRole[kMaxModules];
			volatile LONG modCount;
			int wow64Cpu;

			// sampler counts (sampler thread)
			unsigned threadSamples[3];
			unsigned exclSamples[kMaxModules][3];
			unsigned inclSamples[kMaxModules][3];
			unsigned nearSamples[kMaxModules + 1];	// main thread's samples; [kNoModule] = code outside any module
			unsigned modKernelSamples[kMaxModules];
			unsigned driverCallerSamples[kMaxModules];
			unsigned bucketModSamples[kMaxModules][kRows];
			double busySum[3][kRows];
			unsigned busyN[3][kRows];
			LONGLONG stallQpc;						// main thread time spent suspended by the sampler (QPC ticks)
			unsigned missed;
			volatile LONG watchdogTrips;
			ExeLeaf exeLeaves[kExeSlots];

			// frames (main thread)
			unsigned renderedFrames;				// every render call while capturing
			unsigned frames, loadingFrames, spikeCount;
			double sumMs, worstMs, loadingMs;
			UInt32 frameHist[kFrameBins + 1];
			double sumUpdateMs, sumRenderMs, sumPresentMs;
			unsigned splitFrames;

			// CpuSplit per-frame buckets (main thread)
			double bucketSumMs[CpuSplit::kBucketCount], bucketMaxMs[CpuSplit::kBucketCount];
			unsigned bucketFrames;
			UInt32 bucketHist[CpuSplit::kBucketCount][kStageBins + 1];

			// scripts per load-order index (main thread, TSC ticks)
			unsigned long long modSelfTicks[256];
			unsigned long long scriptCalls;

			// the probe's own bookkeeping on the main thread (QPC ticks)
			LONGLONG ownQpc;

			// 1-frame census (main thread)
			unsigned censusWorldDraws, censusFirstPersonDraws, censusReflDraws, censusOtherDraws;
			unsigned censusTotalDraws, censusSkinnedDraws, censusShaderBinds;
			unsigned activeLights, totalLights;
			unsigned actorsHigh, actorsMidHigh, actorsMidLow, actorsLow;
			unsigned loadedPlugins;
			double vasUsedMB, vasTotalMB;
			char cellDesc[200];
			CensusMod censusByMod[256];
		};

		// Cumulative values once a second (live mode), for the 1 s / 10 s windows.
		struct SecSnap {
			double tMs;
			unsigned frames, mainSamples, spikes, bucketFrames, splitFrames;
			double sumMs, worstMs, upd, rnd, pres;
			LONGLONG stallQpc, ownQpc;
			unsigned long long scriptCalls, tsc;
			unsigned own[kMaxModules + 1], near_[kMaxModules + 1];
			unsigned long long modTicks[256];
			double bucketSum[CpuSplit::kBucketCount];
		};

		struct FrameMark { float ms; UInt8 spike; };

		struct CaptureState {
			Counters d;

			bool live = false;
			double durationSec = 10.0;
			bool reopenMenu = true;
			bool armStop = false;
			double startWallMs = 0.0;
			double stopWallMs = 0.0;
			double prevRenderStartMs = 0.0;
			double pendingRenderMs = 0.0;
			bool hasPendingSplit = false;

			// settings, read at start
			unsigned sampleHz = kSamplerHz, nearDepth = 8, maxSpikes = 200, csvMB = 64, stackCopyLimit = kStackCopyMax;
			double spikeMs = 33.4, autosaveSec = 60.0;
			bool sampleAI = true;

			// GPU passes (main thread)
			std::unordered_map<std::string, GpuStat> gpuPasses;

			// 1-frame Census
			bool censusArmed = false;
			bool censusDone = false;
			std::unordered_map<std::string, unsigned> censusMeshes;

			// Script profiler (main thread)
			unsigned long long startTsc = 0, endTsc = 0;
			std::unordered_map<UInt64, ScriptRec> scripts;
			unsigned long long frameModTicks[256] = {};
			double scriptCostPerCallMs = 0.0;
			std::string modNameTable[256];		// load-order names, cached at start (the writer thread never reads engine data)

			// the last frame's CpuSplit buckets, for a spike's top stage
			double lastBucketMs[CpuSplit::kBucketCount] = {};
			bool lastBucketValid = false;

			// Sampler (its thread) and watchdog
			HANDLE threadHandle = nullptr, watchHandle = nullptr;
			std::atomic<bool> stopSampler{ false };
			HANDLE threadHandles[3] = {};
			DWORD threadIds[3] = {};
			const UInt8* teb[3] = {};			// 32-bit TEB: +0x04 StackBase, +0x08 StackLimit
			unsigned char* stackCopy = nullptr;
			Range ranges[kMaxRanges] = {};		// sampler thread only
			int rangeCount = 0;
			LONGLONG qpcFreq = 1, calQpc = 0;
			unsigned long long calTsc = 0;
			ULONG64 lastCycles[3] = {};
			LONGLONG lastBusyQpc[3] = {};
			volatile LONG64 suspendedSince = 0;	// QPC while the sampler holds a thread suspended, else 0
			HANDLE volatile suspendedHandle = nullptr;
			volatile LONG samplerOff = 0;		// the watchdog turned the sampler off
			volatile LONG64 lastFrameQpc = 0;
			volatile LONG loading = 0;

			// frame boundaries for the sampler's spike tallies
			FrameMark frameRing[kFrameRing] = {};
			volatile LONG frameIndex = 0;		// the frame in progress (published by the main thread)
			UInt16 frameCnt[kMaxModules + 1] = {};	// sampler thread: this frame's samples by near module
			SInt16 touched[kTouchedMax] = {};
			unsigned touchedN = 0;
			LONG curFrame = -1;
			SpikeDll spikeOut[kSpikeRing] = {};
			volatile LONG spikeHead = 0;		// written by the sampler thread
			LONG spikeTail = 0;					// main thread

			// live (main thread)
			std::deque<SpikeRec> spikes;		// last maxSpikes, oldest first
			SecSnap snaps[kSnapCount] = {};
			unsigned snapCount = 0, snapNext = 0;
			double lastSecondMs = 0.0, lastAutosaveMs = 0.0, secWorstMs = 0.0;
			LONGLONG lastStallSeen = 0, lastOwnSeen = 0;
			float recent[kRecent] = {};
			int recentHead = 0;
			std::string folder;
		};

		inline CaptureState* Active = nullptr;

		// UI / Control state (main thread only)
		inline bool PendingStart = false;
		inline bool PendingLive = false;
		inline double PendingDurationSec = 10.0;
		inline bool PendingReopenMenu = true;
		inline bool JustFinished = false;
		inline bool JustFinishedReopen = false;
		inline double RemainingSec = 0.0;
		inline std::string ReportText;
		inline std::string ReportPath;
		inline std::string StatusText = "Idle";
		inline Summary CachedSummary;
		inline volatile LONG GameInFront = 1;	// the game window is in front (main thread, PollProbeKey); read by the watchdog
		inline const char* (*OtherSampler)() = nullptr;	// another stack sampler running (test builds: Render.cpp), or null
		inline bool SamplerZombie = false;		// a sampler thread did not stop in time: no sampler again this session

		// Notes from the sampler and watchdog threads, logged by the writer thread (they never log or allocate themselves)
		inline volatile LONG NoteTrip = 0, NoteTripUs = 0, NoteStall = 0, NoteStallMs = 0, NoteStallSampler = 0;

		inline void PollNotes() {
			if (InterlockedExchange(&NoteTrip, 0))
				Logger::Log("PERF PROBE WATCHDOG: a sample kept a game thread suspended %.1f ms (limit %.0f): resumed it and turned the sampler off for this capture.",
					NoteTripUs / 1000.0, kMaxSuspendMs);
			if (InterlockedExchange(&NoteStall, 0))
				Logger::Log("PERF PROBE: no frame for %.1f s with the game in front (sampler %s, no thread held suspended by the probe); the STALL lines say where the render thread is.",
					NoteStallMs / 1000.0, NoteStallSampler ? "on" : "off");
		}

		// The report: built from a copy on the writer thread, handed back through Result.
		struct ReportInput {
			Counters d;
			bool live = false, final = true, scriptHookOk = false;
			double durationSec = 0.0, ticksPerMs = 3000000.0, nowMs = 0.0, startWallMs = 0.0, scriptCostPerCallMs = 0.0, qpcFreq = 1.0;
			double spikeMs = 33.4;
			unsigned sampleHz = 0;
			std::string runtimeDesc, outPath, folder;
			std::string modNameTable[256];
			std::vector<ScriptRec> scripts;
			std::vector<std::pair<std::string, GpuStat>> gpuPasses;
			std::vector<std::pair<std::string, unsigned>> censusMeshes;
			std::vector<SpikeRec> spikes;
		};
		struct ReportResult {
			Summary sum;
			std::string text, path;
			bool live = false, final = true, reopenMenu = false;
		};
		inline std::atomic<ReportResult*> Result{ nullptr };

		// ---- ScriptRunner::Run hook (lazy, zero-overhead when ScriptActive == false) ---------------------------------------

		inline volatile bool ScriptActive = false;
		inline DWORD MainThreadId = 0;
		inline bool ScriptHookTried = false;
		inline bool ScriptHookOk = false;

		typedef bool (__thiscall* ScriptRunnerRunFn)(void* self, void* script, void* context, void* eventList, void* containingObj, UInt32 a5, UInt32 a6, UInt32 a7, UInt32 a8);
		inline ScriptRunnerRunFn OrigScriptRunnerRun = (ScriptRunnerRunFn)0x005E0D20;

		struct StackFrame {
			unsigned long long t0;
			unsigned long long childTicks;
		};
		static constexpr unsigned kMaxScriptDepth = 64;
		inline StackFrame ScriptStack[kMaxScriptDepth] = {};
		inline unsigned ScriptDepth = 0;

		struct DepthGuard {
			unsigned slot;
			bool entered;
			unsigned long long t0;
			DepthGuard(bool active) : slot(ScriptDepth), entered(active && slot < kMaxScriptDepth), t0(0) {
				if (entered) {
					ScriptDepth = slot + 1;
					ScriptStack[slot].childTicks = 0;
					t0 = __rdtsc();
					ScriptStack[slot].t0 = t0;
				}
			}
			~DepthGuard() {
				if (entered && ScriptDepth == slot + 1) ScriptDepth = slot;
			}
		};

		inline bool __fastcall HookScriptRunnerRun(void* self, UInt32, void* script, void* context, void* eventList, void* containingObj, UInt32 a5, UInt32 a6, UInt32 a7, UInt32 a8) {
			CaptureState* st = Active;
			if (!ScriptActive || !st || !script || GetCurrentThreadId() != MainThreadId)
				return OrigScriptRunnerRun(self, script, context, eventList, containingObj, a5, a6, a7, a8);

			DepthGuard guard(true);
			const bool ok = OrigScriptRunnerRun(self, script, context, eventList, containingObj, a5, a6, a7, a8);
			const unsigned long long t1 = __rdtsc();
			if (guard.entered) {
				const unsigned long long incl = t1 >= guard.t0 ? (t1 - guard.t0) : 0;
				const unsigned long long ch = ScriptStack[guard.slot].childTicks;
				const unsigned long long selfT = incl > ch ? (incl - ch) : 0;
				if (guard.slot > 0) ScriptStack[guard.slot - 1].childTicks += incl;

				UInt32 formID = 0; UInt8 modIdx = 0xFF;
				if (SafeReadScriptKey(script, formID, modIdx)) {
					st->d.modSelfTicks[modIdx] += selfT;
					st->frameModTicks[modIdx] += selfT;
					st->d.scriptCalls++;
					const UInt64 key = (formID != 0 && modIdx != 0xFF)
						? (UInt64)formID
						: ((UInt64)(UInt32)(uintptr_t)script | (1ULL << 63));
					auto it = st->scripts.find(key);
					if (it == st->scripts.end()) {
						if (st->scripts.size() < kMaxScripts) {
							void* ownerObj = containingObj ? containingObj : context;
							ScriptSnapshot snap;
							SafeSnapshotScript(script, ownerObj, &snap);
							if (snap.ok) {
								ScriptRec& r = st->scripts[key];
								r.key = key;
								r.formID = snap.formID;
								r.targetID = snap.targetID;
								r.modIdx = snap.modIdx;
								r.targetModIdx = snap.targetModIdx;
								strcpy_s(r.editorName, snap.editorName);
								strcpy_s(r.snippet, snap.snippet);
								r.calls = 1;
								r.selfTicks = selfT;
								r.inclTicks = incl;
								r.maxCallTicks = selfT;
							}
						}
					}
					else {
						ScriptRec& r = it->second;
						r.calls++;
						r.selfTicks += selfT;
						r.inclTicks += incl;
						if (selfT > r.maxCallTicks) r.maxCallTicks = selfT;
					}
				}
			}
			return ok;
		}

		inline bool EnsureScriptHook() {
			if (ScriptHookTried) return ScriptHookOk;
			ScriptHookTried = true;
			const UInt8* target = (const UInt8*)OrigScriptRunnerRun;
			MEMORY_BASIC_INFORMATION mbi = {};
			const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!VirtualQuery(target, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & exec)) {
				Logger::Log("DEEP PROBE: ScriptRunner::Run (005E0D20) is not executable; per-script profiling skipped");
				return false;
			}
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			const LONG a = DetourAttach(&(PVOID&)OrigScriptRunnerRun, (PVOID)&HookScriptRunnerRun);
			const LONG c = (a == NO_ERROR) ? DetourTransactionCommit() : (DetourTransactionAbort(), a);
			if (c != NO_ERROR) {
				Logger::Log("DEEP PROBE: DetourAttach on ScriptRunner::Run failed (%ld); per-script profiling skipped", c);
				return false;
			}
			ScriptHookOk = true;
			Logger::Log("DEEP PROBE: ScriptRunner::Run hook ready");
			return true;
		}

		// ---- Sampler helpers ----------------------------------------------------------------------------------------------

		inline ModRole ClassifyModule(const char* name, const char* fullPath) {
			char n[64], p[260];
			strcpy_s(n, name ? name : ""); _strlwr_s(n);
			strcpy_s(p, fullPath ? fullPath : ""); _strlwr_s(p);
			if (!strcmp(n, "falloutnv.exe")) return rGame;
			if (!strcmp(n, "vegasshaders.dll") || !strcmp(n, "newvegasreloaded.dll")) return rVegasShaders;
			if (strstr(n, "d3d9") || strstr(n, "dxvk") || strstr(n, "nvd3d") || strstr(n, "nvwgf") ||
				strstr(n, "nvgpucomp") || strstr(n, "nvoglv") || strstr(n, "nvapi") || strstr(n, "nvspcap") ||
				strstr(n, "amdvlk") || strstr(n, "atiumd") || strstr(n, "aticfx") || strstr(n, "amdxn32") ||
				strstr(n, "amdenc32") || strstr(n, "amdihk") || strstr(n, "igdumd") || strstr(n, "igd9") ||
				strstr(n, "igdumdim") || strstr(n, "vulkan-") || strstr(n, "dxgi") || strstr(n, "d3dx9") ||
				strstr(n, "d3dcompiler"))
				return rDriver;
			if (!strcmp(n, "ntdll.dll") || !strcmp(n, "kernel32.dll") || !strcmp(n, "kernelbase.dll") ||
				!strcmp(n, "ucrtbase.dll") || !strcmp(n, "msvcrt.dll") || strstr(n, "msvcp") ||
				strstr(n, "vcruntime") || !strcmp(n, "user32.dll") || !strcmp(n, "win32u.dll") ||
				!strcmp(n, "gdi32.dll") || !strcmp(n, "gdi32full.dll") || !strcmp(n, "wow64.dll") ||
				!strcmp(n, "wow64cpu.dll") || !strcmp(n, "combase.dll") || !strcmp(n, "ole32.dll") ||
				!strcmp(n, "rpcrt4.dll") || !strcmp(n, "sechost.dll") || !strcmp(n, "advapi32.dll"))
				return rSystemWrap;
			if (strstr(p, "\\nvse\\plugins\\") || strstr(n, "nvse") || !strcmp(n, "mlf.dll") || !strcmp(n, "nvtf.dll") || !strcmp(n, "nvhr_avx2.dll"))
				return rNvsePlugin;
			if (strstr(p, "\\windows\\")) return rWindowsOther;
			return rOtherTool;
		}

		// Committed and readable for its whole length (one VirtualQuery region).
		inline bool Readable(const void* p, size_t n) {
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
			if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
			const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(mbi.Protect & rd)) return false;
			return (const UInt8*)p + n <= (const UInt8*)mbi.BaseAddress + mbi.RegionSize;
		}

		// The executable sections of a loaded image, split by protection region and kept only where readable now, so the
		// return-address checks never read memory that can fault. Sampler thread, never while a thread is suspended.
		inline void ParsePeExecSections(DWORD base, int m, Range* ranges, int& rc) {
			// Outside any suspension; the __try covers a module unloaded between a VirtualQuery and a header read.
			__try {
				const UInt8* b = (const UInt8*)(uintptr_t)base;
				if (!Readable(b, sizeof(IMAGE_DOS_HEADER))) return;
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
				if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return;
				const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(b + dos->e_lfanew);
				if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return;
				const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
				const WORD count = nt->FileHeader.NumberOfSections;
				if (count > 96 || !Readable(s, count * sizeof(*s))) return;
				for (WORD i = 0; i < count && rc < (int)kMaxRanges; i++) {
					if (!(s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
					DWORD lo = base + s[i].VirtualAddress;
					const DWORD hi = lo + (std::max)(s[i].Misc.VirtualSize, s[i].SizeOfRawData);
					while (lo < hi && rc < (int)kMaxRanges) {
						MEMORY_BASIC_INFORMATION mbi;
						if (!VirtualQuery((const void*)(uintptr_t)lo, &mbi, sizeof(mbi))) break;
						const DWORD regionEnd = (DWORD)((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
						const DWORD end = (std::min)(hi, regionEnd);
						if (end <= lo) break;
						if (Readable((const void*)(uintptr_t)lo, 1)) ranges[rc++] = { lo, end, m };
						lo = end;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		// Sampler thread. A new module's entry is written in full before modCount covers it (the main thread reads the names).
		inline void ScanModules(CaptureState* st) {
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
			if (snap == INVALID_HANDLE_VALUE) return;
			MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
			Range ranges[kMaxRanges];
			int rc = 0;
			Counters& d = st->d;
			for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
				const DWORD base = (DWORD)(uintptr_t)me.modBaseAddr, size = me.modBaseSize;
				const int count = (int)d.modCount;
				int m = -1;
				for (int i = 0; i < count; i++) {
					if (d.modBase[i] == base && d.modSize[i] == size) { m = i; break; }
				}
				if (m < 0 && count < (int)kMaxModules) {
					m = count;
					d.modBase[m] = base; d.modSize[m] = size;
					if (!WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, d.modNames[m], 64, nullptr, nullptr)) strcpy_s(d.modNames[m], "?");
					if (!WideCharToMultiByte(CP_ACP, 0, me.szExePath, -1, d.modPaths[m], 260, nullptr, nullptr)) strcpy_s(d.modPaths[m], "");
					d.modRole[m] = ClassifyModule(d.modNames[m], d.modPaths[m]);
					if (!_stricmp(d.modNames[m], "wow64cpu.dll")) d.wow64Cpu = m;
					MemoryBarrier();
					InterlockedExchange(&d.modCount, count + 1);
				}
				if (m >= 0) ParsePeExecSections(base, m, ranges, rc);
			}
			CloseHandle(snap);
			std::sort(ranges, ranges + rc, [](const Range& a, const Range& b) { return a.Start < b.Start; });
			memcpy(st->ranges, ranges, sizeof(Range) * rc);
			st->rangeCount = rc;
		}

		inline int RangeAt(const CaptureState* st, DWORD v) {
			int lo = 0, hi = st->rangeCount - 1;
			while (lo <= hi) {
				const int mid = (lo + hi) >> 1;
				if (v < st->ranges[mid].Start) hi = mid - 1;
				else if (v >= st->ranges[mid].End) lo = mid + 1;
				else return mid;
			}
			return -1;
		}

		inline int ModuleAt(const CaptureState* st, DWORD v) {
			const int r = RangeAt(st, v);
			return r >= 0 ? st->ranges[r].Module : -1;
		}

		// True when the bytes before v look like a call (E8 rel32, or FF /2 in its usual encodings). Reads only inside one listed,
		// readable executable section (checked above), never while a thread is suspended. The __try is a last guard for a module
		// unloaded since the last scan (two seconds at most).
		inline bool AfterCall(const CaptureState* st, DWORD v) {
			const int r = RangeAt(st, v);
			if (r < 0 || v < 8 || v - 7 < st->ranges[r].Start) return false;
			__try {
				const unsigned char* p = (const unsigned char*)(uintptr_t)v;
				if (p[-5] == 0xE8) return true;
				if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && ((p[-1] & 0xC0) == 0xC0 || (p[-1] & 0xC0) == 0x00)) return true;	// call reg / [reg]
				if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xC0) == 0x40) return true;	// call [reg+d8]
				if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;								// call [d32] / [reg+d32]
				if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10 && (p[-3] & 7) == 4) return true;			// SIB + d8
				if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10 && (p[-6] & 7) == 4) return true;			// SIB + d32
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return false;
		}

		inline unsigned BucketNow(unsigned thread) {
			using namespace CpuSplit;
			unsigned list = kMain;
			if (thread == 1) list = Lists[kAICombined].count > 0 && Lists[kAI1].count == 0 ? kAICombined : kAI1;
			else if (thread == 2) list = kAI2;
			const LONG n = Lists[list].count;
			if (n <= 0 || (UInt32)n > Lists[list].cap) return kNoBucket;
			const UInt8 b = BucketOf(Lists[list].items[n - 1].id);
			return b < kBucketCount ? b : kNoBucket;
		}

		inline LONGLONG QpcNow() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

		// Sampler thread, outside any suspension: opens the AI threads (timed captures only) and notes their TEBs.
		inline void SafeOpenAIThreads(CaptureState* st) {
			using namespace CpuSplit;
			typedef LONG (NTAPI* QueryFn)(HANDLE, int, PVOID, ULONG, PULONG);
			static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
			for (unsigned t = 1; t < 3; t++) {
				if (st->threadHandles[t]) continue;
				const DWORD id = t == 1 ? (AIThreadIds[kAI1] ? AIThreadIds[kAI1] : AIThreadIds[kAICombined]) : AIThreadIds[kAI2];
				if (!id) continue;
				HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
				if (!h) continue;
				ThreadBasicInformation info = {};
				if (!query || query(h, 0, &info, sizeof(info), NULL) < 0 || !info.Teb || !Readable(info.Teb, 0x10)) { CloseHandle(h); continue; }
				st->threadIds[t] = id;
				st->teb[t] = (const UInt8*)info.Teb;
				st->threadHandles[t] = h;
			}
		}

		// The sampler finishes a frame's tally once a sample of a later frame arrives; a spike frame's top near modules go to
		// spikeOut (single producer: this thread; single consumer: the main thread).
		inline void FinishFrame(CaptureState* st, LONG frame) {
			const LONG published = st->frameIndex;
			const bool closed = frame < published && published - frame < (LONG)kFrameRing;
			if (closed && st->frameRing[(UInt32)frame % kFrameRing].spike && st->touchedN) {
				SpikeDll out = {};
				out.frame = frame;
				unsigned total = 0;
				for (unsigned i = 0; i < st->touchedN; i++) total += st->frameCnt[st->touched[i]];
				out.total = (UInt16)(std::min)(total, 65535u);
				for (unsigned k = 0; k < kSpikeTop; k++) {
					int best = -1; unsigned bestCnt = 0;
					for (unsigned i = 0; i < st->touchedN; i++) {
						const unsigned c = st->frameCnt[st->touched[i]];
						if (c > bestCnt) { bestCnt = c; best = (int)i; }
					}
					if (best < 0) break;
					out.mod[out.n] = st->touched[best];
					out.cnt[out.n] = (UInt16)bestCnt;
					out.n++;
					st->frameCnt[st->touched[best]] = 0;	// taken: the next round finds the next one
				}
				const LONG head = st->spikeHead;
				st->spikeOut[(UInt32)head % kSpikeRing] = out;
				MemoryBarrier();
				st->spikeHead = head + 1;
			}
			for (unsigned i = 0; i < st->touchedN; i++) st->frameCnt[st->touched[i]] = 0;
			st->touchedN = 0;
		}

		inline void TakeSample(CaptureState* st, unsigned t) {
			HANDLE th = st->threadHandles[t];
			const UInt8* teb = st->teb[t];
			if (!th || !teb || !st->stackCopy || st->samplerOff) return;
			CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			DWORD len = 0;
			const LONGLONG t0 = QpcNow();
			if (SuspendThread(th) == (DWORD)-1) { if (t == 0) st->d.missed++; return; }
			// The watchdog may resume it from here on (only after the suspend took: a resume before it would be lost)
			st->suspendedHandle = th;
			MemoryBarrier();
			InterlockedExchange64(&st->suspendedSince, t0);
			// ---- Suspended: GetThreadContext, the TEB and the stack only. No allocation, locks, logging, SEH or other API ----
			const BOOL got = GetThreadContext(th, &ctx);
			if (got) {
				const DWORD base = *(const volatile DWORD*)(teb + 0x04), limit = *(const volatile DWORD*)(teb + 0x08);
				// [ESP, StackBase) is committed when ESP >= StackLimit: below it lie the guard page and reserved memory
				if (ctx.Esp >= limit && ctx.Esp < base && base - limit <= 64u * 1024 * 1024) {
					len = (std::min)(base - ctx.Esp, (DWORD)st->stackCopyLimit) & ~3u;
					memcpy(st->stackCopy, (const void*)(uintptr_t)ctx.Esp, len);
				}
			}
			const unsigned bucket = BucketNow(t);
			const LONG frame = st->frameIndex;
			// Whoever swaps suspendedSince back to 0 resumes, so the thread is resumed exactly once
			if (InterlockedCompareExchange64(&st->suspendedSince, 0, t0) == t0) ResumeThread(th);
			// ---- Running again ----
			if (t == 0) st->d.stallQpc += QpcNow() - t0;

			ULONG64 cycles = 0;
			if (st->sampleAI && QueryThreadCycleTime(th, &cycles)) {
				const LONGLONG cycleQpc = QpcNow();
				const double tscPerQpc = cycleQpc > st->calQpc ? (double)(__rdtsc() - st->calTsc) / (double)(cycleQpc - st->calQpc) : 0.0;
				if (st->lastBusyQpc[t] && tscPerQpc > 0.0 && cycleQpc > st->lastBusyQpc[t]) {
					double busy = (double)(cycles - st->lastCycles[t]) / ((double)(cycleQpc - st->lastBusyQpc[t]) * tscPerQpc);
					busy = busy < 0.0 ? 0.0 : busy > 1.0 ? 1.0 : busy;
					st->d.busySum[t][bucket] += busy;
					st->d.busyN[t][bucket]++;
				}
				st->lastCycles[t] = cycles;
				st->lastBusyQpc[t] = cycleQpc;
			}
			if (!got) { if (t == 0) st->d.missed++; return; }

			Counters& d = st->d;
			d.threadSamples[t]++;
			const int leafMod = ModuleAt(st, ctx.Eip);
			const ModRole leafRole = leafMod >= 0 ? d.modRole[leafMod] : rSystemWrap;
			const bool inKernel = (leafMod < 0 || leafMod == d.wow64Cpu || (leafRole == rSystemWrap && AfterCall(st, ctx.Eip)));

			int exclMod = (leafMod >= 0 && leafRole != rSystemWrap) ? leafMod : -1;
			DWORD exclAddr = exclMod >= 0 ? ctx.Eip : 0;
			int driverCaller = -1;
			// near: the leaf when it is a plugin, else the first plugin among the next nearDepth validated return addresses
			int nearMod = (leafMod >= 0 && IsPluginRole(leafRole)) ? leafMod : -1;
			unsigned validated = 0;

			int seen[32];
			int seenCount = 0;
			if (leafMod >= 0 && leafRole != rSystemWrap) seen[seenCount++] = leafMod;

			const DWORD* w = (const DWORD*)st->stackCopy;
			const DWORD words = len / 4;
			for (DWORD i = 0; i < words; i++) {
				const DWORD v = w[i];
				if (v < 0x10000) continue;
				const int m = ModuleAt(st, v);
				if (m < 0 || !AfterCall(st, v)) continue;
				const ModRole r = d.modRole[m];
				if (nearMod < 0 && validated < st->nearDepth) {
					validated++;
					if (IsPluginRole(r)) nearMod = m;
				}
				if (exclMod < 0 && r != rSystemWrap) {
					exclMod = m;
					exclAddr = v;
				}
				if (exclMod >= 0 && driverCaller < 0 && d.modRole[exclMod] == rDriver &&
					(r == rGame || r == rVegasShaders || r == rNvsePlugin || r == rOtherTool)) {
					driverCaller = m;
				}
				if (r != rSystemWrap && seenCount < 32) {
					bool dup = false;
					for (int k = 0; k < seenCount; k++) if (seen[k] == m) { dup = true; break; }
					if (!dup) seen[seenCount++] = m;
				}
			}
			if (nearMod < 0) nearMod = exclMod;	// no plugin above it: the own (exclusive) module

			if (exclMod >= 0) {
				d.exclSamples[exclMod][t]++;
				d.bucketModSamples[exclMod][bucket]++;
				if (inKernel) d.modKernelSamples[exclMod]++;
				if (d.modRole[exclMod] == rGame && t == 0 && exclAddr >= d.modBase[exclMod]) {
					const DWORD rva = exclAddr - d.modBase[exclMod];
					// key = (bucket << 24) | rva: bucket <= kBucketCount < 128, so the key is positive; rva 0 (the PE header) is never code
					if (rva > 0 && rva < (1u << 24)) {
						const LONG k = (LONG)((bucket << 24) | rva);
						UInt32 h = ((UInt32)k * 2654435761u) & (kExeSlots - 1);
						for (unsigned probe = 0; probe < 32; probe++, h = (h + 1) & (kExeSlots - 1)) {
							ExeLeaf& e = d.exeLeaves[h];
							if (e.key == k) { e.count++; break; }
							if (e.key == 0) { e.count = 1; MemoryBarrier(); e.key = k; break; }
						}
					}
				}
				if (d.modRole[exclMod] == rDriver && t == 0 && driverCaller >= 0) {
					d.driverCallerSamples[driverCaller]++;
				}
			}
			for (int k = 0; k < seenCount; k++) {
				d.inclSamples[seen[k]][t]++;
			}

			if (t == 0) {
				const unsigned id = nearMod >= 0 ? (unsigned)nearMod : kNoModule;
				d.nearSamples[id]++;
				if (frame != st->curFrame) {
					if (st->curFrame >= 0) FinishFrame(st, st->curFrame);
					st->curFrame = frame;
				}
				// a module new to this frame is tallied only while the touched list has room (FinishFrame resets only what it lists)
				if (st->frameCnt[id] == 0) {
					if (st->touchedN < kTouchedMax) { st->touched[st->touchedN++] = (SInt16)id; st->frameCnt[id] = 1; }
				}
				else if (st->frameCnt[id] < 65535) st->frameCnt[id]++;
			}
		}

		inline DWORD WINAPI SamplerThreadMain(void* param) {
			CaptureState* st = (CaptureState*)param;
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
			const LONGLONG period = -10000000LL / (LONGLONG)(st->sampleHz ? st->sampleHz : kSamplerHz);
			ScanModules(st);
			LONGLONG lastScan = QpcNow();
			const unsigned threads = st->sampleAI ? 3u : 1u;
			while (!st->stopSampler.load(std::memory_order_relaxed) && !st->samplerOff) {
				if (timer) {
					LARGE_INTEGER due; due.QuadPart = period;
					if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
					else Sleep(1);
				}
				else Sleep(1);
				if (st->stopSampler.load(std::memory_order_relaxed) || st->samplerOff) break;
				if (st->sampleAI) SafeOpenAIThreads(st);
				const LONGLONG now = QpcNow();
				if (now - lastScan > 2 * st->qpcFreq) { ScanModules(st); lastScan = now; }
				if (st->loading) continue;	// loading screens: no samples
				for (unsigned t = 0; t < threads; t++) TakeSample(st, t);
			}
			if (st->curFrame >= 0) FinishFrame(st, st->curFrame);
			if (timer) CloseHandle(timer);
			return 0;
		}

		// One watchdog round: resumes the thread the sampler holds when that suspension is over kMaxSuspendMs.
		// Exactly one resume per successful suspend: whoever swaps suspendedSince from the sample's start to 0 resumes. Each
		// sample's start is a fresh QPC value, and the sampler writes suspendedHandle before it publishes the start and clears the
		// start before it suspends another thread, so a handle read between reading the start and a successful swap is that
		// sample's thread. The sampler is turned off before the resume, so it cannot start another sample on a stale decision.
		inline void WatchCheck(CaptureState* st, double qpcPerMs, bool force) {
			// Read the start first, then the clock: a sample that starts in between must not look endless.
			const LONG64 since = InterlockedCompareExchange64(&st->suspendedSince, 0, 0);
			if (!since) return;
			const HANDLE held = st->suspendedHandle;
			const LONGLONG now = QpcNow();
			if (!force && (now <= since || (now - since) / qpcPerMs <= kMaxSuspendMs)) return;
			if (InterlockedCompareExchange64(&st->suspendedSince, 0, since) != since) return;	// the sampler resumed it itself
			InterlockedExchange(&st->samplerOff, 1);
			ResumeThread(held);
			InterlockedIncrement(&st->d.watchdogTrips);
			NoteTripUs = now > since ? (LONG)((now - since) / qpcPerMs * 1000.0) : 0;
			InterlockedExchange(&NoteTrip, 1);
		}

		// Resumes a thread the sampler has held suspended over kMaxSuspendMs (and turns the sampler off), and notes a run of 4 s
		// with no frame while the game is in front (GameInFront, published by the main thread). Never allocates, logs, takes a lock
		// or calls user32: the writer thread logs its notes. Created before the sampler thread and runs until that thread has
		// exited, so no sample is ever left without it.
		inline DWORD WINAPI WatchThreadMain(void* param) {
			CaptureState* st = (CaptureState*)param;
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			// 1 ms waits without changing the system timer resolution (Sleep would round to the 15.6 ms tick)
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
			const double qpcPerMs = (double)st->qpcFreq / 1000.0;
			bool stallNoted = false;
			for (;;) {
				if (timer) {
					LARGE_INTEGER due; due.QuadPart = -10000;	// 1 ms, relative
					if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 20);
					else Sleep(1);
				}
				else Sleep(1);
				WatchCheck(st, qpcPerMs, false);
				if (st->stopSampler.load(std::memory_order_relaxed) &&
					(!st->threadHandle || WaitForSingleObject(st->threadHandle, 0) == WAIT_OBJECT_0)) {
					WatchCheck(st, qpcPerMs, true);	// the sampler is gone: a start still set would be a thread left held: resume it now
					break;
				}
				const LONGLONG now = QpcNow();
				const LONG64 last = st->lastFrameQpc;
				if (!last || st->loading) { stallNoted = false; continue; }
				const double age = (now - last) / qpcPerMs;
				if (age < kStallNoteMs) { stallNoted = false; continue; }
				if (stallNoted || !GameInFront) continue;	// alt-tabbed: the game makes no frames
				stallNoted = true;
				NoteStallMs = (LONG)age;
				NoteStallSampler = st->samplerOff ? 0 : 1;
				InterlockedExchange(&NoteStall, 1);
			}
			if (timer) CloseHandle(timer);
			return 0;
		}

		inline void OnCpuSplitBuckets(const double* ms, UInt32 count) {
			CaptureState* st = Active;
			if (!st || !ms) return;
			Counters& d = st->d;
			const UInt32 n = (std::min)(count, (UInt32)CpuSplit::kBucketCount);
			for (UInt32 b = 0; b < n; b++) {
				const double v = ms[b];
				d.bucketSumMs[b] += v;
				if (v > d.bucketMaxMs[b]) d.bucketMaxMs[b] = v;
				const unsigned bin = v <= 0.0 ? 0u : (unsigned)(std::min)(v / kStageBinMs, (double)kStageBins);
				d.bucketHist[b][bin]++;
				st->lastBucketMs[b] = v;
			}
			st->lastBucketValid = true;
			d.bucketFrames++;
		}

		inline void OnGpuPassSample(const char* name, double ms) {
			CaptureState* st = Active;
			if (!st || !name) return;
			GpuStat& g = st->gpuPasses[name];
			g.sumMs += ms;
			if (ms > g.maxMs) g.maxMs = ms;
			g.samples++;
		}

		// ---- Histograms -------------------------------------------------------------------------------------------------

		// Nearest-rank percentile from bins of binMs (a bin's middle; the overflow bin and anything past the worst give the worst).
		inline double HistPercentile(const UInt32* h, unsigned bins, double binMs, unsigned long long n, double worst, double p) {
			if (!n) return 0.0;
			unsigned long long want = (unsigned long long)std::ceil(p * (double)n);
			if (want < 1) want = 1;
			unsigned long long acc = 0;
			for (unsigned i = 0; i <= bins; i++) {
				acc += h[i];
				if (acc >= want) return i == bins ? worst : (std::min)((i + 0.5) * binMs, worst > 0.0 ? worst : (i + 0.5) * binMs);
			}
			return worst;
		}

		// Average fps of the slowest fraction of frames (as SummarizeFrameTimes), from the histogram.
		inline double HistLowFps(const UInt32* h, unsigned bins, double binMs, unsigned long long n, double worst, double fraction) {
			if (!n) return 0.0;
			unsigned long long count = (unsigned long long)std::ceil(fraction * (double)n);
			if (count < 1) count = 1;
			unsigned long long left = count;
			double sum = 0.0;
			for (int i = (int)bins; i >= 0 && left; i--) {
				if (!h[i]) continue;
				const unsigned long long take = (std::min)((unsigned long long)h[i], left);
				const double v = i == (int)bins ? worst : (std::min)((i + 0.5) * binMs, worst);
				sum += v * (double)take;
				left -= take;
			}
			return sum > 0.0 ? 1000.0 / (sum / (double)count) : 0.0;
		}

		// ---- Report generation (writer thread: reads only the copy) -------------------------------------------------------

		inline std::string TimeStampText() {
			SYSTEMTIME t; GetLocalTime(&t);
			char name[64];
			sprintf_s(name, "%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
			return name;
		}

		inline std::string FormatTimeStampFilename() {
			return Paths::GameRoot() + "\\" + Paths::LogsFolder + "\\DeepProbe_" + TimeStampText() + ".txt";
		}

		// The MO2 mod a module was loaded from, when its path is a real one inside MO2's mods folder (usvfs usually shows the game's
		// virtual path instead: then tools\deepprobe_mo2.py finds the mod from the profile).
		inline std::string Mo2ModOf(const char* path) {
			if (!path || !path[0]) return "";
			std::string low = path;
			for (auto& c : low) c = (char)tolower((unsigned char)c);
			const size_t at = low.rfind("\\mods\\");
			if (at == std::string::npos) return "";
			const size_t start = at + 6, end = low.find('\\', start);
			if (end == std::string::npos) return "";
			return std::string(path + start, path + end);
		}

		inline std::string CsvText(const std::string& s) {
			std::string out = "\"";
			for (char c : s) { if (c == '"') out += "\"\""; else out += c; }
			return out + "\"";
		}

		inline std::string SpikeDllText(const SpikeRec& r, const ReportInput& in) {
			if (!r.dllsIn || !r.dllTotal) return "";
			std::string out;
			char buf[160];
			for (unsigned k = 0; k < r.dllN; k++) {
				const int m = r.dllMod[k];
				const char* name = m == (int)kNoModule ? "(outside modules)" : (m >= 0 && m < (int)in.d.modCount ? in.d.modNames[m] : "?");
				sprintf_s(buf, "%s%s %d%%", k ? "; " : "", name, (int)(100.0 * r.dllCnt[k] / r.dllTotal + 0.5));
				out += buf;
			}
			return out;
		}

		inline const std::string& ModNameAt(const ReportInput& in, unsigned idx) {
			static const std::string none = "-";
			return idx < 256 ? in.modNameTable[idx] : none;
		}

		inline ReportResult* BuildReport(const ReportInput& in) {
			ReportResult* res = new ReportResult();
			res->live = in.live;
			res->final = in.final;
			Summary& sum = res->sum;
			const Counters& d = in.d;
			const int modCount = (std::min)((int)d.modCount, (int)kMaxModules);
			sum.valid = true;
			sum.live = in.live;
			sum.final = in.final;
			sum.folder = in.folder;
			sum.durationSec = in.durationSec < 0.001 ? 0.001 : in.durationSec;
			sum.scriptHookOk = in.scriptHookOk;
			sum.loadedPlugins = d.loadedPlugins;
			sum.loadedDlls = (unsigned)modCount;
			sum.vasUsedMB = d.vasUsedMB;
			sum.vasTotalMB = d.vasTotalMB;
			sum.cellDesc = d.cellDesc;
			sum.runtimeDesc = in.runtimeDesc;
			sum.activeLights = d.activeLights;
			sum.totalLights = d.totalLights;
			sum.actorsHigh = d.actorsHigh;
			sum.actorsMidHigh = d.actorsMidHigh;
			sum.actorsMidLow = d.actorsMidLow;
			sum.actorsLow = d.actorsLow;
			sum.censusWorldDraws = d.censusWorldDraws;
			sum.censusFirstPersonDraws = d.censusFirstPersonDraws;
			sum.censusReflDraws = d.censusReflDraws;
			sum.censusOtherDraws = d.censusOtherDraws;
			sum.censusTotalDraws = d.censusTotalDraws;
			sum.censusSkinnedDraws = d.censusSkinnedDraws;
			sum.censusShaderBinds = d.censusShaderBinds;
			sum.samplerMainSamples = d.threadSamples[0];
			sum.samplerAi1Samples = d.threadSamples[1];
			sum.samplerAi2Samples = d.threadSamples[2];
			sum.samplerMissed = d.missed;
			sum.watchdogTrips = (unsigned)d.watchdogTrips;
			sum.loadingFrames = d.loadingFrames;
			sum.loadingMs = d.loadingMs;
			sum.spikesTotal = d.spikeCount;

			sum.frames = d.frames;
			sum.avgMs = d.frames ? d.sumMs / d.frames : 0.0;
			sum.p50Ms = HistPercentile(d.frameHist, kFrameBins, kFrameBinMs, d.frames, d.worstMs, 0.50);
			sum.p95Ms = HistPercentile(d.frameHist, kFrameBins, kFrameBinMs, d.frames, d.worstMs, 0.95);
			sum.p99Ms = HistPercentile(d.frameHist, kFrameBins, kFrameBinMs, d.frames, d.worstMs, 0.99);
			sum.maxMs = d.worstMs;
			sum.avgFps = sum.avgMs > 0.0 ? 1000.0 / sum.avgMs : 0.0;
			sum.low1Fps = HistLowFps(d.frameHist, kFrameBins, kFrameBinMs, d.frames, d.worstMs, 0.01);
			sum.low01Fps = HistLowFps(d.frameHist, kFrameBins, kFrameBinMs, d.frames, d.worstMs, 0.001);

			if (d.splitFrames > 0) {
				sum.updateMs = d.sumUpdateMs / d.splitFrames;
				sum.renderCpuMs = d.sumRenderMs / d.splitFrames;
				sum.presentMs = d.sumPresentMs / d.splitFrames;
			}

			// The probe's own cost per frame: the main thread's time suspended by the sampler, the script hook's clock reads, and
			// the probe's bookkeeping and drawing on the main thread
			const double frameDiv = sum.frames ? (double)sum.frames : 1.0;
			const double qpcPerMs = in.qpcFreq / 1000.0;
			sum.probeSamplerMs = (double)d.stallQpc / qpcPerMs / frameDiv;
			sum.probeScriptMs = (double)d.scriptCalls * in.scriptCostPerCallMs / frameDiv;
			sum.probeOwnMs = (double)d.ownQpc / qpcPerMs / frameDiv;
			sum.probeMs = sum.probeSamplerMs + sum.probeScriptMs + sum.probeOwnMs;
			sum.stallUsPerSample = d.threadSamples[0] ? (double)d.stallQpc / qpcPerMs * 1000.0 / d.threadSamples[0] : 0.0;

			// GPU passes
			for (const auto& kv : in.gpuPasses) {
				if (!kv.second.samples) continue;
				GpuPassRow r;
				r.name = kv.first;
				r.avgMs = kv.second.sumMs / kv.second.samples;
				r.maxMs = kv.second.maxMs;
				r.samples = kv.second.samples;
				if (r.name != "Game frame total" &&
					r.name.compare(0, 2, "  ") != 0 &&
					r.name.find("(game)") == std::string::npos &&
					r.name != "Game call 0x871A50" &&
					r.name != "Game image space" &&
					r.name != "Pre-scene (to world scene)" &&
					r.name != "First person: deferred (all)") {
					sum.gpuVegasMs += r.avgMs;
				}
				sum.gpuPasses.push_back(std::move(r));
			}
			std::sort(sum.gpuPasses.begin(), sum.gpuPasses.end(), [](const GpuPassRow& a, const GpuPassRow& b) { return a.avgMs > b.avgMs; });

			// CpuSplit stages
			const unsigned bFrames = d.bucketFrames ? d.bucketFrames : 1;
			sum.ai1BusyMs = d.bucketSumMs[CpuSplit::bAI1Busy] / bFrames;
			sum.ai2BusyMs = d.bucketSumMs[CpuSplit::bAI2Busy] / bFrames;
			for (UInt32 b = 0; b < CpuSplit::kBucketCount; b++) {
				if (b == CpuSplit::bAI1Busy || b == CpuSplit::bAI2Busy) continue;
				const double avg = d.bucketSumMs[b] / bFrames;
				if (avg < 0.005 && d.bucketMaxMs[b] < 0.05) continue;
				StageRow r;
				r.name = CpuSplit::BucketNames[b];
				r.avgMs = avg;
				r.maxMs = d.bucketMaxMs[b];
				r.pct = sum.avgMs > 0.0 ? (100.0 * avg / sum.avgMs) : 0.0;
				r.p95Ms = HistPercentile(d.bucketHist[b], kStageBins, kStageBinMs, d.bucketFrames, d.bucketMaxMs[b], 0.95);
				const unsigned tIdx = (b >= CpuSplit::bAICombat) ? (b == CpuSplit::bAI2Wait ? 2u : 1u) : 0u;
				if (d.busyN[tIdx][b] > 0) r.busyPct = 100.0 * (d.busySum[tIdx][b] / d.busyN[tIdx][b]);
				sum.stages.push_back(std::move(r));
			}
			std::sort(sum.stages.begin(), sum.stages.end(), [](const StageRow& a, const StageRow& b) { return a.avgMs > b.avgMs; });

			// Sampler DLLs (main thread wall time attribution)
			const unsigned mainTotal = d.threadSamples[0];
			const double refFrameMs = sum.avgMs > 0.0 ? sum.avgMs : (sum.durationSec * 1000.0 / frameDiv);
			for (int m = 0; m < modCount; m++) {
				const unsigned excl = d.exclSamples[m][0];
				const unsigned incl = d.inclSamples[m][0];
				const unsigned nearS = d.nearSamples[m];
				if (!excl && !incl && !nearS) continue;
				const double inclPct = mainTotal ? (100.0 * incl / mainTotal) : 0.0;
				const double exclPct = mainTotal ? (100.0 * excl / mainTotal) : 0.0;
				const double nearPct = mainTotal ? (100.0 * nearS / mainTotal) : 0.0;
				if (excl == 0 && nearS == 0 && inclPct < 0.3) continue;
				DllRow r;
				r.name = d.modNames[m];
				r.role = RoleLabel(d.modRole[m]);
				r.mo2Mod = Mo2ModOf(d.modPaths[m]);
				r.exclSamples = excl;
				r.inclSamples = incl;
				r.nearSamples = nearS;
				r.exclPct = exclPct;
				r.inclPct = inclPct;
				r.nearPct = nearPct;
				r.exclMs = (exclPct * 0.01) * refFrameMs;
				r.inclMs = (inclPct * 0.01) * refFrameMs;
				r.nearMs = (nearPct * 0.01) * refFrameMs;
				r.waitPct = excl ? (100.0 * d.modKernelSamples[m] / excl) : 0.0;
				unsigned bestB = kNoBucket, bestCnt = 0;
				for (unsigned b = 0; b < CpuSplit::kBucketCount; b++) {
					if (d.bucketModSamples[m][b] > bestCnt) { bestCnt = d.bucketModSamples[m][b]; bestB = b; }
				}
				r.topStage = bestB < CpuSplit::kBucketCount ? CpuSplit::BucketNames[bestB] : "-";
				sum.dlls.push_back(std::move(r));
			}
			if (d.nearSamples[kNoModule]) {
				DllRow r;
				r.name = "(code outside any module)";
				r.role = "Trampolines / JIT";
				r.nearSamples = d.nearSamples[kNoModule];
				r.nearPct = mainTotal ? 100.0 * r.nearSamples / mainTotal : 0.0;
				r.nearMs = r.nearPct * 0.01 * refFrameMs;
				r.topStage = "-";
				sum.dlls.push_back(std::move(r));
			}
			std::sort(sum.dlls.begin(), sum.dlls.end(), [](const DllRow& a, const DllRow& b) {
				if (a.exclSamples != b.exclSamples) return a.exclSamples > b.exclSamples;
				if (a.nearSamples != b.nearSamples) return a.nearSamples > b.nearSamples;
				return a.inclSamples > b.inclSamples;
			});

			// Driver callers
			for (int m = 0; m < modCount; m++) {
				const unsigned c = d.driverCallerSamples[m];
				if (!c || !mainTotal) continue;
				DriverCallerRow r;
				r.callerName = d.modNames[m];
				r.callerRole = RoleLabel(d.modRole[m]);
				r.samples = c;
				r.pct = 100.0 * c / mainTotal;
				r.ms = (r.pct * 0.01) * refFrameMs;
				sum.driverCallers.push_back(std::move(r));
			}
			std::sort(sum.driverCallers.begin(), sum.driverCallers.end(), [](const DriverCallerRow& a, const DriverCallerRow& b) { return a.samples > b.samples; });

			// Script profiler conversion
			const double ticksPerMs = in.ticksPerMs > 1.0 ? in.ticksPerMs : 3000000.0;
			std::unordered_map<UInt8, ScriptModRow> byMod;
			for (const ScriptRec& sr : in.scripts) {
				const double selfMs = (double)sr.selfTicks / ticksPerMs / frameDiv;
				const double inclMs = (double)sr.inclTicks / ticksPerMs / frameDiv;
				const double maxMs = (double)sr.maxCallTicks / ticksPerMs;
				sum.totalScriptCalls += sr.calls;
				sum.totalScriptSelfMs += selfMs;

				const std::string& modName = ModNameAt(in, sr.modIdx);
				ScriptModRow& mr = byMod[sr.modIdx];
				mr.modIdx = sr.modIdx;
				mr.modName = modName;
				mr.selfMs += selfMs;
				mr.callsPerFrame += (double)sr.calls / frameDiv;
				mr.uniqueScripts++;
				if (maxMs > mr.maxCallMs) mr.maxCallMs = maxMs;

				ScriptRow row;
				row.formID = sr.formID;
				row.targetID = sr.targetID;
				row.modName = modName;
				row.selfMs = selfMs;
				row.inclMs = inclMs;
				row.callsPerFrame = (double)sr.calls / frameDiv;
				row.maxCallMs = maxMs;
				row.calls = sr.calls;
				if (sr.modIdx == 0xFF && sr.targetModIdx != 0xFF) row.contextMod = ModNameAt(in, sr.targetModIdx);
				if (sr.editorName[0]) {
					row.label = sr.editorName;
				}
				else if (sr.snippet[0]) {
					row.label = sr.snippet;
				}
				else if (sr.formID) {
					char fb[32]; sprintf_s(fb, "Form %08X", sr.formID);
					row.label = fb;
				}
				else {
					char fb[32]; sprintf_s(fb, "Script @%08X", (unsigned)(sr.key & 0xFFFFFFFFu));
					row.label = fb;
				}
				sum.scripts.push_back(std::move(row));
			}
			for (auto& kv : byMod) sum.scriptMods.push_back(std::move(kv.second));
			std::sort(sum.scriptMods.begin(), sum.scriptMods.end(), [](const ScriptModRow& a, const ScriptModRow& b) { return a.selfMs > b.selfMs; });
			std::sort(sum.scripts.begin(), sum.scripts.end(), [](const ScriptRow& a, const ScriptRow& b) { return a.selfMs > b.selfMs; });

			// Draw Census by mod and mesh
			for (int i = 0; i < 256; i++) {
				if (!d.censusByMod[i].draws) continue;
				DrawModRow r;
				r.modName = i == 0xFF ? std::string("[Land / LOD / Unlinked]") : ModNameAt(in, (unsigned)i);
				r.draws = d.censusByMod[i].draws;
				r.skinned = d.censusByMod[i].skinned;
				sum.drawMods.push_back(std::move(r));
			}
			std::sort(sum.drawMods.begin(), sum.drawMods.end(), [](const DrawModRow& a, const DrawModRow& b) { return a.draws > b.draws; });
			for (const auto& kv : in.censusMeshes) {
				const size_t sep = kv.first.find('\t');
				const unsigned mIdx = sep != std::string::npos ? (unsigned)atoi(kv.first.substr(0, sep).c_str()) : 0xFFu;
				DrawMeshRow r;
				r.meshName = sep != std::string::npos ? kv.first.substr(sep + 1) : kv.first;
				r.modName = mIdx >= 0xFF ? std::string("[Land / LOD / Unlinked]") : ModNameAt(in, mIdx);
				r.draws = kv.second;
				sum.drawMeshes.push_back(std::move(r));
			}
			std::sort(sum.drawMeshes.begin(), sum.drawMeshes.end(), [](const DrawMeshRow& a, const DrawMeshRow& b) { return a.draws > b.draws; });

			// Spikes, newest first
			for (auto it = in.spikes.rbegin(); it != in.spikes.rend(); ++it) {
				SpikeRow r;
				r.frame = (UInt32)it->frame;
				r.atSec = (it->atMs - in.startWallMs) / 1000.0;
				r.ms = it->ms;
				r.dlls = SpikeDllText(*it, in);
				r.stage = it->stage < CpuSplit::kBucketCount ? CpuSplit::BucketNames[it->stage] : "";
				r.stageMs = it->stageMs;
				r.script = it->scriptMod < 0x100 ? ModNameAt(in, it->scriptMod) : "";
				r.scriptMs = it->scriptMs;
				sum.spikes.push_back(std::move(r));
			}

			// Compute plain-English Verdict
			{
				double pluginsMs = 0.0, vegasCpuMs = 0.0;
				for (const auto& dl : sum.dlls) {
					if (dl.role == "NVSE Plugin" || dl.role == "DLL / Overlay") pluginsMs += dl.exclMs;
					else if (dl.role == "Vegas Shaders") vegasCpuMs += dl.exclMs;
				}
				char vbuf[512];
				if (sum.avgMs <= 0.0) {
					sprintf_s(vbuf, "Too few frames collected (%u) to diagnose.", sum.frames);
				}
				else if (sum.presentMs > sum.updateMs && sum.presentMs > sum.renderCpuMs && sum.presentMs > 0.45 * sum.avgMs) {
					sprintf_s(vbuf, "GPU / VSync / Present-limited: %.1f ms/f spent in Present/GPU wait vs %.1f ms Game Update + %.1f ms Render CPU (Vegas Shaders GPU ~%.1f ms).",
						sum.presentMs, sum.updateMs, sum.renderCpuMs, sum.gpuVegasMs);
				}
				else if (sum.totalScriptSelfMs > 3.5 || (!sum.scriptMods.empty() && sum.scriptMods[0].selfMs > 2.0)) {
					sprintf_s(vbuf, "Script-heavy CPU bottleneck: %.2f ms/f inside scripts (%.0f calls/frame), Update %.1f ms + Render %.1f ms (Vegas Shaders CPU ~%.1f ms).",
						sum.totalScriptSelfMs, (double)sum.totalScriptCalls / frameDiv, sum.updateMs, sum.renderCpuMs, vegasCpuMs);
				}
				else if (pluginsMs > 3.5) {
					sprintf_s(vbuf, "Native DLL / NVSE plugin CPU bottleneck: %.1f ms/f exclusive inside third-party DLLs, Update %.1f ms + Render %.1f ms (Vegas Shaders CPU ~%.1f ms).",
						pluginsMs, sum.updateMs, sum.renderCpuMs, vegasCpuMs);
				}
				else if (sum.renderCpuMs > sum.updateMs && sum.renderCpuMs > 8.0) {
					sprintf_s(vbuf, "CPU Draw-Call / Culling / Render submission bottleneck: Game Render CPU %.1f ms/f (%u draws, %u active lights), Game Update %.1f ms/f (Vegas Shaders CPU ~%.1f ms, GPU ~%.1f ms).",
						sum.renderCpuMs, sum.censusTotalDraws, sum.activeLights, sum.updateMs, vegasCpuMs, sum.gpuVegasMs);
				}
				else {
					sprintf_s(vbuf, "CPU Update %.1f ms/f | Game Render CPU %.1f ms/f | Present/GPU wait %.1f ms/f | Scripts %.2f ms/f | Native Plugins %.1f ms/f | Vegas Shaders CPU ~%.1f ms, GPU ~%.1f ms.",
						sum.updateMs, sum.renderCpuMs, sum.presentMs, sum.totalScriptSelfMs, pluginsMs, vegasCpuMs, sum.gpuVegasMs);
				}
				sum.verdict = vbuf;
			}

			// Top FalloutNV.exe addresses
			std::vector<std::pair<UInt32, unsigned>> exeList;
			for (unsigned i = 0; i < kExeSlots; i++) {
				if (d.exeLeaves[i].key && d.exeLeaves[i].count) exeList.push_back({ (UInt32)d.exeLeaves[i].key, (unsigned)d.exeLeaves[i].count });
			}
			std::sort(exeList.begin(), exeList.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

			// Format full text report
			std::string txt;
			txt.reserve(24576);
			auto appendf = [&](const char* fmt, ...) {
				char buf[1024];
				va_list args;
				va_start(args, fmt);
				_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
				va_end(args);
				txt += buf;
			};

			appendf("========================================================================================\n");
			appendf("%s %s - Deep Performance Probe Report%s\n", Branding::ProductName, Branding::Version,
				in.live ? (in.final ? " (live mode, final)" : " (live mode, autosave: still recording)") : "");
			appendf("========================================================================================\n");
			appendf("Runtime      : %s\n", sum.runtimeDesc.c_str());
			appendf("Location     : %s\n", sum.cellDesc.c_str());
			appendf("Capture      : %.2f s | %u frames (+%u loading frames, %.1f s, left out) | Loaded ESP/ESM: %u | Loaded DLLs: %u\n",
				sum.durationSec, sum.frames, sum.loadingFrames, sum.loadingMs / 1000.0, sum.loadedPlugins, sum.loadedDlls);
			appendf("32-bit VAS   : %.0f / %.0f MB (%.1f%% used)\n",
				sum.vasUsedMB, sum.vasTotalMB, sum.vasTotalMB > 0.0 ? (100.0 * sum.vasUsedMB / sum.vasTotalMB) : 0.0);
			appendf("Probe cost   : ~%.3f ms/frame (sampler %.3f: %.1f us per sample at %u Hz; script timing ~%.3f; bookkeeping + drawing %.3f); %u samples missed\n",
				sum.probeMs, sum.probeSamplerMs, sum.stallUsPerSample, in.sampleHz, sum.probeScriptMs, sum.probeOwnMs, sum.samplerMissed);
			if (sum.watchdogTrips)
				appendf("WATCHDOG     : a sample held a game thread over %.0f ms (%u times): the sampler was turned off; DLL numbers stop there (see the log).\n",
					kMaxSuspendMs, sum.watchdogTrips);
			appendf("Verdict      : %s\n\n", sum.verdict.c_str());

			appendf("--- 1. FRAME TIME & SPLIT --------------------------------------------------------------\n");
			appendf("FPS / Frame  : avg %.1f fps (%.2f ms) | p50 %.1f ms | p95 %.1f ms | p99 %.1f ms | max %.2f ms (percentiles: 0.1 ms bins)\n",
				sum.avgFps, sum.avgMs, sum.p50Ms, sum.p95Ms, sum.p99Ms, sum.maxMs);
			appendf("Lows         : 1%% low %.1f fps | 0.1%% low %.1f fps\n", sum.low1Fps, sum.low01Fps);
			appendf("Spikes       : %u frames over %.1f ms or 2.5x the 10 s mean\n", sum.spikesTotal, in.spikeMs);
			appendf("CPU Split    : Game Update %.2f ms | Game Render CPU %.2f ms | Present/Wait %.2f ms | Vegas Shaders GPU ~%.2f ms\n",
				sum.updateMs, sum.renderCpuMs, sum.presentMs, sum.gpuVegasMs);
			appendf("AI Threads   : AI1 busy %.2f ms/frame | AI2 busy %.2f ms/frame\n\n", sum.ai1BusyMs, sum.ai2BusyMs);

			appendf("--- 2. TOP ENGINE STAGES (CpuSplit) ----------------------------------------------------\n");
			appendf("%-32s %8s %8s %8s %7s %8s\n", "Stage", "Avg ms", "p95 ms", "Max ms", "Frame%", "CPU Busy");
			for (size_t i = 0; i < sum.stages.size() && i < 24; i++) {
				const StageRow& r = sum.stages[i];
				char busyBuf[16];
				if (r.busyPct >= 0.0) sprintf_s(busyBuf, "%6.1f%%", r.busyPct); else strcpy_s(busyBuf, "      -");
				appendf("%-32s %8.2f %8.2f %8.2f %6.1f%% %8s\n", r.name.c_str(), r.avgMs, r.p95Ms, r.maxMs, r.pct, busyBuf);
			}
			appendf("\n");

			appendf("--- 3. LOADED DLLs / PLUGINS (%u Hz Stack Sampler, %u Main Samples) -------------------\n", in.sampleHz, mainTotal);
			appendf("  own = the module the time is spent in (Windows code charged to its caller); near = the first plugin DLL among the\n");
			appendf("  next return addresses (engine work a plugin's hook calls is charged to the plugin; heuristic); incl = on the stack.\n");
			appendf("%-28s %-14s %8s %7s %8s %8s %6s  %-26s %s\n",
				"Module", "Role", "Own ms", "Own %", "Near ms", "Incl ms", "Wait%", "Top Stage", "MO2 mod");
			for (size_t i = 0; i < sum.dlls.size() && i < 40; i++) {
				const DllRow& r = sum.dlls[i];
				appendf("%-28.28s %-14s %8.2f %6.1f%% %8.2f %8.2f %5.0f%%  %-26.26s %s\n",
					r.name.c_str(), r.role.c_str(), r.exclMs, r.exclPct, r.nearMs, r.inclMs, r.waitPct, r.topStage.c_str(), r.mo2Mod.c_str());
			}
			if (!sum.driverCallers.empty()) {
				appendf("\n  Driver / DXVK CPU Time by Calling Module:\n");
				for (size_t i = 0; i < sum.driverCallers.size() && i < 10; i++) {
					const DriverCallerRow& dc = sum.driverCallers[i];
					appendf("    <- %-26s (%-12s) : %.2f ms/f (%.1f%% of main thread, %u samples)\n",
						dc.callerName.c_str(), dc.callerRole.c_str(), dc.ms, dc.pct, dc.samples);
				}
			}
			appendf("\n");

			appendf("--- 4. SCRIPTS BY ESP/ESM MOD (ScriptRunner::Run, %s) --------------------------------\n",
				sum.scriptHookOk ? "active" : "hook unavailable");
			appendf("Total Script Self Time: %.2f ms/frame across %.1f script calls/frame (%u unique scripts)\n",
				sum.totalScriptSelfMs, (double)sum.totalScriptCalls / frameDiv, (unsigned)sum.scripts.size());
			appendf("%-40s %8s %9s %8s %8s\n", "Mod (.esp/.esm)", "Self ms", "Calls/f", "Scripts", "MaxCall");
			for (size_t i = 0; i < sum.scriptMods.size() && i < 25; i++) {
				const ScriptModRow& m = sum.scriptMods[i];
				appendf("%-40.40s %8.3f %9.1f %8u %7.2fms\n", m.modName.c_str(), m.selfMs, m.callsPerFrame, m.uniqueScripts, m.maxCallMs);
			}
			appendf("\n  Top Individual Scripts:\n");
			appendf("  %-28s %-26s %8s %8s %8s %7s  %s\n", "Script", "Mod", "Self ms", "Incl ms", "Calls/f", "Max ms", "FormID / Target");
			for (size_t i = 0; i < sum.scripts.size() && i < 30; i++) {
				const ScriptRow& s = sum.scripts[i];
				appendf("  %-28.28s %-26.26s %8.3f %8.3f %8.1f %7.2f  %08X%s%s\n",
					s.label.c_str(), s.modName.c_str(), s.selfMs, s.inclMs, s.callsPerFrame, s.maxCallMs,
					s.formID, s.contextMod.empty() ? "" : " on ", s.contextMod.c_str());
			}
			appendf("\n");

			appendf("--- 5. VEGAS SHADERS GPU PASSES --------------------------------------------------------\n");
			appendf("%-32s %8s %8s %8s\n", "Pass", "Avg ms", "Max ms", "Samples");
			for (size_t i = 0; i < sum.gpuPasses.size() && i < 24; i++) {
				const GpuPassRow& g = sum.gpuPasses[i];
				appendf("%-32.32s %8.3f %8.3f %8u\n", g.name.c_str(), g.avgMs, g.maxMs, g.samples);
			}
			appendf("\n");

			appendf("--- 6. 1-FRAME SCENE / DRAW-CALL / LIGHT / ACTOR CENSUS -------------------------------\n");
			appendf("Draws        : %u total captured (World %u | 1st-Person %u | Refl %u | Other %u) | Skinned: %u | Shader binds: %u\n",
				sum.censusTotalDraws, sum.censusWorldDraws, sum.censusFirstPersonDraws, sum.censusReflDraws,
				sum.censusOtherDraws, sum.censusSkinnedDraws, sum.censusShaderBinds);
			if (sum.censusTotalDraws == 0 && sum.censusShaderBinds > 0) {
				appendf("  (Note: per-draw mesh hook inactive because Forward Shadows / HiddenObjectCulling / LampScissor / MaterialRules were all off at startup; shader binds still counted.)\n");
			}
			appendf("Point Lights : %u active / %u in ShadowSceneNode\n", sum.activeLights, sum.totalLights);
			appendf("Actors       : High %u | Mid-High %u | Mid-Low %u | Low %u\n",
				sum.actorsHigh, sum.actorsMidHigh, sum.actorsMidLow, sum.actorsLow);
			if (!sum.drawMods.empty()) {
				appendf("\n  Top Mods by Draw Calls (1 frame):\n");
				for (size_t i = 0; i < sum.drawMods.size() && i < 15; i++) {
					appendf("    %-42.42s : %5u draws (%4u skinned)\n",
						sum.drawMods[i].modName.c_str(), sum.drawMods[i].draws, sum.drawMods[i].skinned);
				}
			}
			if (!sum.drawMeshes.empty()) {
				appendf("\n  Top Meshes / Nodes by Draw Calls (1 frame):\n");
				for (size_t i = 0; i < sum.drawMeshes.size() && i < 20; i++) {
					appendf("    %-32.32s %-32.32s : %5u draws\n",
						sum.drawMeshes[i].meshName.c_str(), sum.drawMeshes[i].modName.c_str(), sum.drawMeshes[i].draws);
				}
			}
			appendf("\n");

			if (!exeList.empty() && mainTotal > 0) {
				appendf("--- 7. TOP FalloutNV.exe HOT ADDRESSES -------------------------------------------------\n");
				for (size_t i = 0; i < exeList.size() && i < 15; i++) {
					const unsigned b = exeList[i].first >> 24;
					const DWORD rva = exeList[i].first & 0x00FFFFFFu;
					const double pct = 100.0 * exeList[i].second / mainTotal;
					appendf("  FalloutNV.exe+0x%06X  %5u samples (%5.2f%%)  [%s]\n",
						(unsigned)rva, exeList[i].second, pct, b < CpuSplit::kBucketCount ? CpuSplit::BucketNames[b] : "-");
				}
				appendf("\n");
			}

			if (!sum.spikes.empty()) {
				appendf("--- 8. SPIKE FRAMES (last %u kept of %u; top near DLLs by sampled share, top stage, top script plugin) ---\n",
					(unsigned)sum.spikes.size(), sum.spikesTotal);
				std::vector<const SpikeRow*> worst;
				for (const auto& s : sum.spikes) worst.push_back(&s);
				std::sort(worst.begin(), worst.end(), [](const SpikeRow* a, const SpikeRow* b) { return a->ms > b->ms; });
				appendf("  Worst:\n");
				for (size_t i = 0; i < worst.size() && i < 10; i++) {
					const SpikeRow& s = *worst[i];
					appendf("    %7.1f s  %6.1f ms  stage %s %.1f ms | script %s %.2f ms | DLLs %s\n", s.atSec, s.ms,
						s.stage.empty() ? "-" : s.stage.c_str(), s.stageMs, s.script.empty() ? "-" : s.script.c_str(), s.scriptMs,
						s.dlls.empty() ? "(no sample)" : s.dlls.c_str());
				}
				appendf("  Latest:\n");
				for (size_t i = 0; i < sum.spikes.size() && i < 10; i++) {
					const SpikeRow& s = sum.spikes[i];
					appendf("    %7.1f s  %6.1f ms  stage %s %.1f ms | script %s %.2f ms | DLLs %s\n", s.atSec, s.ms,
						s.stage.empty() ? "-" : s.stage.c_str(), s.stageMs, s.script.empty() ? "-" : s.script.c_str(), s.scriptMs,
						s.dlls.empty() ? "(no sample)" : s.dlls.c_str());
				}
				appendf("\n");
			}
			if (in.live) {
				appendf("Files: report.txt, modules.csv (per DLL, with its path), script_plugins.csv, scripts.csv, spikes.csv, frames.csv.\n");
				appendf("MO2 mod names: py -3.10 -I tools\\deepprobe_mo2.py \"%s\" --mo2 <MO2 folder>\n", in.folder.c_str());
			}

			// CSV files of a live session
			if (in.live) {
				std::string t = "name,role,path,mo2_mod,own_samples,own_ms_per_frame,near_samples,near_ms_per_frame,incl_ms_per_frame,top_stage\n";
				for (const auto& r : sum.dlls) {
					char line[256];
					sprintf_s(line, ",%u,%.4f,%u,%.4f,%.4f,", r.exclSamples, r.exclMs, r.nearSamples, r.nearMs, r.inclMs);
					std::string path;
					for (int m = 0; m < modCount; m++) if (r.name == d.modNames[m]) { path = d.modPaths[m]; break; }
					t += CsvText(r.name) + "," + CsvText(r.role) + "," + CsvText(path) + "," + CsvText(r.mo2Mod) + line + CsvText(r.topStage) + "\n";
				}
				Writer::WriteFileAtomic(in.folder + "\\modules.csv", t);

				t = "load_index,plugin,self_ms_per_frame,calls_per_frame,scripts,max_call_ms\n";
				for (const auto& m : sum.scriptMods) {
					char line[160];
					sprintf_s(line, "%02X,", (unsigned)m.modIdx);
					char nums[160];
					sprintf_s(nums, ",%.4f,%.2f,%u,%.3f\n", m.selfMs, m.callsPerFrame, m.uniqueScripts, m.maxCallMs);
					t += line + CsvText(m.modName) + nums;
				}
				Writer::WriteFileAtomic(in.folder + "\\script_plugins.csv", t);

				t = "form_id,label,plugin,self_ms_per_frame,incl_ms_per_frame,calls,max_call_ms\n";
				for (const auto& s : sum.scripts) {
					char id[16]; sprintf_s(id, "%08X,", s.formID);
					char nums[160]; sprintf_s(nums, ",%.5f,%.5f,%llu,%.3f\n", s.selfMs, s.inclMs, s.calls, s.maxCallMs);
					t += id + CsvText(s.label) + "," + CsvText(s.modName) + nums;
				}
				Writer::WriteFileAtomic(in.folder + "\\scripts.csv", t);

				t = "frame,at_s,ms,top_near_dlls,top_stage,stage_ms,top_script_plugin,script_ms\n";
				for (auto it = sum.spikes.rbegin(); it != sum.spikes.rend(); ++it) {
					char head[64]; sprintf_s(head, "%u,%.2f,%.2f,", it->frame, it->atSec, it->ms);
					char st1[32]; sprintf_s(st1, ",%.2f,", it->stageMs);
					char st2[32]; sprintf_s(st2, ",%.3f\n", it->scriptMs);
					t += head + CsvText(it->dlls) + "," + CsvText(it->stage) + st1 + CsvText(it->script) + st2;
				}
				Writer::WriteFileAtomic(in.folder + "\\spikes.csv", t);
			}

			// Keep UI vectors concise
			if (sum.stages.size() > 32) sum.stages.resize(32);
			if (sum.dlls.size() > 40) sum.dlls.resize(40);
			if (sum.scriptMods.size() > 30) sum.scriptMods.resize(30);
			if (sum.scripts.size() > 40) sum.scripts.resize(40);
			if (sum.gpuPasses.size() > 30) sum.gpuPasses.resize(30);
			if (sum.drawMods.size() > 25) sum.drawMods.resize(25);
			if (sum.drawMeshes.size() > 25) sum.drawMeshes.resize(25);
			if (sum.spikes.size() > 50) sum.spikes.resize(50);

			const std::string logsDir = Paths::GameRoot() + "\\" + Paths::LogsFolder;
			CreateDirectoryA(logsDir.c_str(), nullptr);
			const bool ok = Writer::WriteFileAtomic(in.outPath, txt);
			if (ok) {
				if (in.final) Logger::Log("DEEP PROBE: saved report to %s (%.1f fps avg, %.2f ms/f scripts, probe ~%.3f ms/f)", in.outPath.c_str(), sum.avgFps, sum.totalScriptSelfMs, sum.probeMs);
			}
			else {
				Logger::Log("[WARNING] DEEP PROBE: could not write report file %s", in.outPath.c_str());
			}
			res->text = std::move(txt);
			res->path = ok ? in.outPath : "(failed to write file - use Copy Report)";
			return res;
		}

		// Main thread: copies everything the report needs and hands it to the writer thread. No engine data is read there.
		inline void PostReport(CaptureState* st, bool final) {
			auto in = std::make_shared<ReportInput>();
			memcpy(&in->d, &st->d, sizeof(Counters));
			in->live = st->live;
			in->final = final;
			in->scriptHookOk = ScriptHookOk;
			const double nowMs = final && st->stopWallMs > 0.0 ? st->stopWallMs : CpuTimer::NowMs();
			in->nowMs = nowMs;
			in->startWallMs = st->startWallMs;
			in->durationSec = (nowMs - st->startWallMs) / 1000.0;
			const unsigned long long endTsc = final && st->endTsc ? st->endTsc : __rdtsc();
			if (endTsc > st->startTsc && nowMs - st->startWallMs > 1.0) in->ticksPerMs = (double)(endTsc - st->startTsc) / (nowMs - st->startWallMs);
			in->scriptCostPerCallMs = st->scriptCostPerCallMs;
			in->qpcFreq = (double)st->qpcFreq;
			in->spikeMs = st->spikeMs;
			in->sampleHz = st->sampleHz;
			in->runtimeDesc = TheRenderManager && TheRenderManager->D3D9RuntimeDescription() ? TheRenderManager->D3D9RuntimeDescription() : "?";
			for (unsigned i = 0; i < 256; i++) in->modNameTable[i] = st->modNameTable[i];
			in->scripts.reserve(st->scripts.size());
			for (const auto& kv : st->scripts) in->scripts.push_back(kv.second);
			in->gpuPasses.assign(st->gpuPasses.begin(), st->gpuPasses.end());
			in->censusMeshes.assign(st->censusMeshes.begin(), st->censusMeshes.end());
			in->spikes.assign(st->spikes.begin(), st->spikes.end());
			in->folder = st->folder;
			in->outPath = st->live ? st->folder + "\\report.txt" : FormatTimeStampFilename();
			const bool reopen = st->reopenMenu;
			Writer::Post([in, reopen]() {
				ReportResult* r = BuildReport(*in);
				r->reopenMenu = reopen;
				delete Result.exchange(r);	// an older result nobody took yet is replaced
			});
		}

		// Main thread, every frame: takes a finished report.
		inline void AdoptResult() {
			if (!Result.load(std::memory_order_relaxed)) return;
			ReportResult* r = Result.exchange(nullptr);
			if (!r) return;
			ReportText = std::move(r->text);
			ReportPath = std::move(r->path);
			CachedSummary = std::move(r->sum);
			char sbuf[200];
			if (r->live && !r->final)
				sprintf_s(sbuf, "Live: autosaved (%.1f fps avg, %u spikes so far)", CachedSummary.avgFps, CachedSummary.spikesTotal);
			else
				sprintf_s(sbuf, "Done: %.1f fps avg (1%% low %.1f fps), %u frames captured", CachedSummary.avgFps, CachedSummary.low1Fps, CachedSummary.frames);
			StatusText = sbuf;
			if (r->final && !r->live) {
				JustFinished = true;
				JustFinishedReopen = r->reopenMenu;
			}
			delete r;
		}

		inline int SettingI(const char* key, int def, int lo, int hi) {
			if (!TheSettingManager) return def;
			const int v = TheSettingManager->GetSettingI("Main.Develop.PerformanceProbe", key);
			return v < lo ? lo : v > hi ? hi : v;
		}
		inline double SettingF(const char* key, double def, double lo, double hi) {
			if (!TheSettingManager) return def;
			const double v = TheSettingManager->GetSettingF("Main.Develop.PerformanceProbe", key);
			return v < lo ? lo : v > hi ? hi : v;
		}

		// What two clock reads and the map update per script call cost, measured once at start (the report states it).
		inline double MeasureScriptCostMs() {
			std::unordered_map<UInt64, int> dummy;
			for (int i = 0; i < 512; i++) dummy[(UInt64)i * 7919] = i;
			LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
			volatile unsigned long long sink = 0;
			for (int i = 0; i < 2000; i++) {
				const unsigned long long t0 = __rdtsc();
				auto it = dummy.find((UInt64)(i & 511) * 7919);
				sink += __rdtsc() - t0 + (it != dummy.end() ? 1 : 0) + GetCurrentThreadId();
			}
			QueryPerformanceCounter(&b);
			return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / 2000.0 * 1.5;
		}

		inline bool StartCapture(double durationSec, bool reopenMenu, bool live) {
			if (Active) return false;
			if (SamplerZombie) {
				StatusText = "Not started: an earlier sampler thread did not stop in time; the probe stays off until the game restarts.";
				return false;
			}
			if (const char* other = OtherSampler ? OtherSampler() : nullptr) {	// test builds: their own samplers run until the game exits
				StatusText = std::string("Not started: ") + other + " is running (only one stack sampler may run).";
				Logger::Log("DEEP PROBE: not started: %s is running (only one stack sampler may run)", other);
				return false;
			}
			if (GetModuleHandleA("FrameCostLogger.dll")) {	// room 01's logger has its own sampler: one at a time
				StatusText = "Not started: FrameCostLogger.dll is loaded and has its own sampler (only one may run). Disable that mod to use the probe.";
				Logger::Log("DEEP PROBE: not started: FrameCostLogger.dll is loaded (only one stack sampler may run)");
				return false;
			}
			EnsureScriptHook();
			CaptureState* st = new CaptureState();
			memset(&st->d, 0, sizeof(Counters));
			st->d.wow64Cpu = -1;
			st->live = live;
			st->durationSec = live ? 0.0 : (durationSec < 1.0 ? 1.0 : durationSec > 60.0 ? 60.0 : durationSec);
			st->reopenMenu = !live && reopenMenu;
			st->nearDepth = (unsigned)SettingI("NearDepth", 8, 0, 16);
			st->spikeMs = SettingF("SpikeMs", 33.4, 1.0, 1000.0);
			st->maxSpikes = (unsigned)SettingI("MaxSpikes", 200, 1, 5000);
			if (live) {
				st->sampleHz = (unsigned)SettingI("LiveSampleHz", 500, 50, 2000);
				st->autosaveSec = SettingF("AutosaveSeconds", 60.0, 0.0, 3600.0);
				if (st->autosaveSec > 0.0 && st->autosaveSec < 5.0) st->autosaveSec = 5.0;	// 0 = only at stop; else at least 5 s
				st->csvMB = (unsigned)SettingI("FrameCsvMaxMB", 64, 1, 4096);
				st->stackCopyLimit = kLiveStackCopy;
				st->sampleAI = false;	// live mode samples the main thread only (cheaper; the AI threads' busy share needs them)
			}
			{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); st->qpcFreq = f.QuadPart ? f.QuadPart : 1; }
			st->calQpc = QpcNow();
			st->calTsc = __rdtsc();
			MainThreadId = GetCurrentThreadId();
			st->threadIds[0] = MainThreadId;
			st->teb[0] = (const UInt8*)NtCurrentTeb();
			st->threadHandles[0] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, MainThreadId);
			st->stackCopy = (unsigned char*)VirtualAlloc(nullptr, kStackCopyMax, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			st->scripts.reserve(512);
			st->scriptCostPerCallMs = ScriptHookOk ? MeasureScriptCostMs() : 0.0;
			for (unsigned i = 0; i < 256; i++) {
				char nb[128] = {};
				SafeCopyModName((UInt8)i, nb, sizeof(nb));
				st->modNameTable[i] = nb;
			}
			st->startWallMs = CpuTimer::NowMs();
			st->lastSecondMs = st->lastAutosaveMs = st->startWallMs;
			st->startTsc = __rdtsc();

			Writer::Detail::Poll = &PollNotes;
			Writer::Start();
			if (live) {
				const std::string logsDir = Paths::GameRoot() + "\\" + Paths::LogsFolder;
				CreateDirectoryA(logsDir.c_str(), nullptr);
				st->folder = logsDir + "\\DeepProbe_Live_" + TimeStampText();
				CreateDirectoryA(st->folder.c_str(), nullptr);
				Writer::OpenFrames(st->folder + "\\frames.csv", st->csvMB);
			}

			Active = st;
			Running.store(true, std::memory_order_release);
			RemainingSec = st->durationSec;
			StatusText = live ? "Live..." : "Capturing...";

			GpuTimer::OnSample = &OnGpuPassSample;
			GpuTimer::ProbeActive = true;
			CpuSplit::OnFrameBuckets = &OnCpuSplitBuckets;
			ScriptDepth = 0;
			ScriptActive = ScriptHookOk;

			st->lastFrameQpc = QpcNow();
			if (st->threadHandles[0] && st->stackCopy) {
				// The sampler is made suspended, the watchdog next (it watches the sampler's handle), then the sampler runs: no
				// sample is ever taken without a watchdog. No watchdog: the sampler is told to stop before it runs its first line.
				st->threadHandle = CreateThread(nullptr, 256 * 1024, &SamplerThreadMain, st, CREATE_SUSPENDED, nullptr);
				if (st->threadHandle) {
					st->watchHandle = CreateThread(nullptr, 64 * 1024, &WatchThreadMain, st, 0, nullptr);
					if (!st->watchHandle) {
						InterlockedExchange(&st->samplerOff, 1);
						st->stopSampler.store(true, std::memory_order_release);
					}
					ResumeThread(st->threadHandle);
					if (!st->watchHandle) WaitForSingleObject(st->threadHandle, kStopWaitMs);
				}
			}
			if (live) Logger::Log("DEEP PROBE: live mode started (%u Hz, near depth %u, spikes > %.1f ms, autosave %.0f s) -> %s",
				st->sampleHz, st->nearDepth, st->spikeMs, st->autosaveSec, st->folder.c_str());
			else Logger::Log("DEEP PROBE: started %.0f s capture", st->durationSec);
			return true;
		}

		inline void StopCapture() {
			CaptureState* st = Active;
			if (!st) return;

			InCensusFrame = false;
			ScriptActive = false;
			CpuSplit::OnFrameBuckets = nullptr;
			GpuTimer::OnSample = nullptr;
			GpuTimer::ProbeActive = false;
			st->endTsc = __rdtsc();
			st->stopWallMs = CpuTimer::NowMs();

			// Both threads wake at least every 50 ms: a healthy pair is gone well within kStopWaitMs. One that is not keeps its
			// state (it still uses it) and the sampler stays off for the session.
			st->stopSampler.store(true, std::memory_order_release);
			const LONGLONG deadline = QpcNow() + st->qpcFreq * kStopWaitMs / 1000;
			bool stopped = true;
			HANDLE threads[2] = { st->threadHandle, st->watchHandle };
			for (HANDLE h : threads) {
				if (!h) continue;
				const LONGLONG left = deadline - QpcNow();
				const DWORD waitMs = left > 0 ? (DWORD)(left * 1000 / st->qpcFreq) : 0;
				if (WaitForSingleObject(h, waitMs) != WAIT_OBJECT_0) stopped = false;
			}
			if (stopped) {
				for (HANDLE& h : threads) if (h) CloseHandle(h);
				st->threadHandle = st->watchHandle = nullptr;
			}
			else {
				SamplerZombie = true;
				Logger::Log("DEEP PROBE: the sampler thread did not stop within %u ms: left to run down on its own; the probe stays off until the game restarts.", kStopWaitMs);
			}

			if (!st->censusDone) {
				SafeCopyCellText(st->d.cellDesc, sizeof(st->d.cellDesc));
				SafeSnapshotWorldStats(st->d.activeLights, st->d.totalLights, st->d.actorsHigh, st->d.actorsMidHigh, st->d.actorsMidLow, st->d.actorsLow, st->d.loadedPlugins);
				MEMORYSTATUSEX ms = { sizeof(ms) };
				if (GlobalMemoryStatusEx(&ms)) {
					st->d.vasTotalMB = ms.ullTotalVirtual / 1048576.0;
					st->d.vasUsedMB = (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0;
				}
			}
			if (st->live) Writer::CloseFrames();

			Running.store(false, std::memory_order_release);
			RemainingSec = 0.0;
			Active = nullptr;
			StatusText = "Building the report...";

			PostReport(st, true);
			if (!stopped) return;	// leaked on purpose: the threads may still read it
			for (int t = 0; t < 3; t++) {
				if (st->threadHandles[t]) { CloseHandle(st->threadHandles[t]); st->threadHandles[t] = nullptr; }
			}
			if (st->stackCopy) { VirtualFree(st->stackCopy, 0, MEM_RELEASE); st->stackCopy = nullptr; }
			delete st;
		}

		// ---- Live bookkeeping (main thread) ---------------------------------------------------------------------------------

		inline const SecSnap* SnapBack(const CaptureState* st, unsigned back) {	// 0 = newest
			if (!st->snapCount) return nullptr;
			if (back > st->snapCount - 1) back = st->snapCount - 1;
			return &st->snaps[(st->snapNext + kSnapCount - 1 - back) % kSnapCount];
		}

		inline void TakeSecSnap(CaptureState* st, double nowMs) {
			SecSnap& s = st->snaps[st->snapNext];
			const Counters& d = st->d;
			s.tMs = nowMs;
			s.frames = d.frames; s.sumMs = d.sumMs; s.worstMs = st->secWorstMs;
			s.mainSamples = d.threadSamples[0]; s.spikes = d.spikeCount;
			s.bucketFrames = d.bucketFrames; s.splitFrames = d.splitFrames;
			s.upd = d.sumUpdateMs; s.rnd = d.sumRenderMs; s.pres = d.sumPresentMs;
			s.stallQpc = d.stallQpc; s.ownQpc = d.ownQpc;
			s.scriptCalls = d.scriptCalls; s.tsc = __rdtsc();
			const int modCount = (std::min)((int)d.modCount, (int)kMaxModules);
			for (int m = 0; m < modCount; m++) s.own[m] = d.exclSamples[m][0];
			for (int m = modCount; m < (int)kMaxModules; m++) s.own[m] = 0;
			s.own[kNoModule] = 0;
			memcpy(s.near_, d.nearSamples, sizeof(s.near_));
			memcpy(s.modTicks, d.modSelfTicks, sizeof(s.modTicks));
			memcpy(s.bucketSum, d.bucketSumMs, sizeof(s.bucketSum));
			st->snapNext = (st->snapNext + 1) % kSnapCount;
			if (st->snapCount < kSnapCount) st->snapCount++;
			st->secWorstMs = 0.0;
		}

		// The 10 s mean for the spike rule (the session's mean until 10 s exist)
		inline double Mean10(const CaptureState* st) {
			const SecSnap* a = SnapBack(st, 10);
			const Counters& d = st->d;
			if (a && d.frames > a->frames) return (d.sumMs - a->sumMs) / (d.frames - a->frames);
			return d.frames ? d.sumMs / d.frames : 0.0;
		}

		// The sampler's spike tallies, joined to the main thread's spike records by frame number.
		inline void DrainSpikeDlls(CaptureState* st) {
			const LONG head = st->spikeHead;
			MemoryBarrier();
			if (head - st->spikeTail > (LONG)kSpikeRing) st->spikeTail = head - kSpikeRing;	// fell behind: the oldest are gone
			for (; st->spikeTail != head; st->spikeTail++) {
				const SpikeDll& s = st->spikeOut[(UInt32)st->spikeTail % kSpikeRing];
				for (auto it = st->spikes.rbegin(); it != st->spikes.rend(); ++it) {
					if (it->frame != s.frame) continue;
					it->dllsIn = true;
					it->dllTotal = s.total;
					it->dllN = s.n;
					for (unsigned k = 0; k < s.n && k < kSpikeTop; k++) { it->dllMod[k] = s.mod[k]; it->dllCnt[k] = s.cnt[k]; }
					break;
				}
			}
		}

		// A frame ended (render start to render start). Loading frames are counted apart and never spikes.
		inline void CloseFrame(CaptureState* st, double ft, bool loading, double renderStartMs, double prevRenderEndMs, double presentMs, bool hasPresent) {
			Counters& d = st->d;
			const LONG f = st->frameIndex;
			const bool isLoading = loading || ft >= 1000.0 || ft <= 0.0;
			const double tscPerMs = st->startTsc && renderStartMs > st->startWallMs + 1.0 ? (double)(__rdtsc() - st->startTsc) / (renderStartMs - st->startWallMs) : 0.0;
			unsigned long long scriptTicks = 0;
			for (unsigned i = 0; i < 256; i++) scriptTicks += st->frameModTicks[i];
			const double scriptMs = tscPerMs > 0.0 ? scriptTicks / tscPerMs : 0.0;
			bool spike = false;
			if (isLoading) {
				d.loadingFrames++;
				if (ft > 0.0) d.loadingMs += ft;
			}
			else {
				const double mean10 = Mean10(st);
				spike = ft > st->spikeMs || (d.frames > 120 && mean10 > 0.0 && ft > 2.5 * mean10);
				d.frames++;
				d.sumMs += ft;
				if (ft > d.worstMs) d.worstMs = ft;
				if (ft > st->secWorstMs) st->secWorstMs = ft;
				d.frameHist[(unsigned)(std::min)(ft / kFrameBinMs, (double)kFrameBins)]++;
				st->recent[st->recentHead] = (float)ft;
				st->recentHead = (st->recentHead + 1) % kRecent;
				if (st->hasPendingSplit && prevRenderEndMs > 0.0 && hasPresent) {
					const double between = renderStartMs - prevRenderEndMs;
					if (between >= 0.0 && between < 1000.0) {
						const double pres = (std::min)(presentMs, between);
						d.sumUpdateMs += between - pres;
						d.sumRenderMs += st->pendingRenderMs;
						d.sumPresentMs += pres;
						d.splitFrames++;
					}
				}
				if (spike) {
					d.spikeCount++;
					SpikeRec r;
					r.frame = f;
					r.ms = (float)ft;
					r.atMs = renderStartMs;
					r.scriptTotalMs = (float)scriptMs;
					if (st->lastBucketValid) {
						double best = 0.0;
						for (UInt32 b = 0; b < CpuSplit::bAICombat; b++) {
							if (b == CpuSplit::bOverhead) continue;
							if (st->lastBucketMs[b] > best) { best = st->lastBucketMs[b]; r.stage = (UInt8)b; }
						}
						r.stageMs = (float)best;
					}
					unsigned long long bestT = 0;
					for (unsigned i = 0; i < 256; i++) if (st->frameModTicks[i] > bestT) { bestT = st->frameModTicks[i]; r.scriptMod = (UInt16)i; }
					r.scriptMs = tscPerMs > 0.0 ? (float)(bestT / tscPerMs) : 0.0f;
					st->spikes.push_back(r);
					while (st->spikes.size() > st->maxSpikes) st->spikes.pop_front();
				}
			}
			st->frameRing[(UInt32)f % kFrameRing] = { (float)ft, (UInt8)(spike ? 1 : 0) };
			MemoryBarrier();
			st->frameIndex = f + 1;
			if (st->live) {
				const LONGLONG stall = d.stallQpc, own = d.ownQpc;
				const double probeMs = (double)((stall - st->lastStallSeen) + (own - st->lastOwnSeen)) * 1000.0 / (double)st->qpcFreq;
				st->lastStallSeen = stall; st->lastOwnSeen = own;
				Writer::PushFrame({ (UInt32)f, (float)ft, (float)scriptMs, (float)probeMs, (UInt8)((spike ? 1 : 0) | (isLoading ? 2 : 0)) });
			}
			memset(st->frameModTicks, 0, sizeof(st->frameModTicks));
		}

		// ---- Live view ------------------------------------------------------------------------------------------------------

		inline LiveView View;
		inline double ViewBuiltMs = -1.0;

		inline void BuildLiveView(CaptureState* st, int windowSec, double nowMs) {
			LiveView& v = View;
			const Counters& d = st->d;
			v.live = st->live;
			v.windowSec = windowSec;
			v.recSec = (nowMs - st->startWallMs) / 1000.0;
			v.frames = d.frames;
			v.spikes = d.spikeCount;
			v.folder = st->folder;
			v.missed = d.missed;
			v.watchdogTrips = (unsigned)d.watchdogTrips;
			v.samplerOff = st->samplerOff != 0 || !st->threadHandle;
			v.nextAutosaveSec = st->live && st->autosaveSec > 0.0 ? (std::max)(0.0, st->autosaveSec - (nowMs - st->lastAutosaveMs) / 1000.0) : -1.0;
			v.avgSessionMs = d.frames ? d.sumMs / d.frames : 0.0;
			v.p99SessionMs = HistPercentile(d.frameHist, kFrameBins, kFrameBinMs, d.frames, d.worstMs, 0.99);
			memcpy(v.recent, st->recent, sizeof(v.recent));
			v.recentHead = st->recentHead;

			// Window ends: "to" = the newest second snapshot, "from" = windowSec back; the session = from zero to now
			const SecSnap* to = SnapBack(st, 0);
			const SecSnap* from = windowSec > 0 ? SnapBack(st, (unsigned)windowSec) : nullptr;
			const bool session = windowSec <= 0 || !to || !from || from == to;
			static SecSnap now;		// the session's "to": the counters as they are
			if (session) {
				memset(&now, 0, sizeof(now));
				now.frames = d.frames; now.sumMs = d.sumMs; now.mainSamples = d.threadSamples[0]; now.spikes = d.spikeCount;
				now.bucketFrames = d.bucketFrames; now.splitFrames = d.splitFrames; now.upd = d.sumUpdateMs; now.rnd = d.sumRenderMs; now.pres = d.sumPresentMs;
				now.stallQpc = d.stallQpc; now.ownQpc = d.ownQpc; now.scriptCalls = d.scriptCalls; now.tsc = __rdtsc(); now.tMs = nowMs;
				const int mc = (std::min)((int)d.modCount, (int)kMaxModules);
				for (int m = 0; m < mc; m++) now.own[m] = d.exclSamples[m][0];
				memcpy(now.near_, d.nearSamples, sizeof(now.near_));
				memcpy(now.modTicks, d.modSelfTicks, sizeof(now.modTicks));
				memcpy(now.bucketSum, d.bucketSumMs, sizeof(now.bucketSum));
				to = &now;
				from = nullptr;
			}
			static const SecSnap zero = {};
			const SecSnap& A = from ? *from : zero;
			const SecSnap& B = *to;
			const double startMs = from ? A.tMs : st->startWallMs;
			const unsigned long long startTsc = from ? A.tsc : st->startTsc;
			const unsigned frames = B.frames - A.frames;
			v.windowAvgMs = frames ? (B.sumMs - A.sumMs) / frames : 0.0;
			if (const SecSnap* s1 = SnapBack(st, 1)) { const SecSnap* s0 = SnapBack(st, 0); v.avg1Ms = s0->frames > s1->frames ? (s0->sumMs - s1->sumMs) / (s0->frames - s1->frames) : 0.0; }
			v.avg10Ms = Mean10(st);
			v.worst10Ms = 0.0;
			for (unsigned i = 0; i < st->snapCount; i++) v.worst10Ms = (std::max)(v.worst10Ms, st->snaps[i].worstMs);
			const unsigned splits = B.splitFrames - A.splitFrames;
			v.updateMs = splits ? (B.upd - A.upd) / splits : 0.0;
			v.renderMs = splits ? (B.rnd - A.rnd) / splits : 0.0;
			v.presentMs = splits ? (B.pres - A.pres) / splits : 0.0;

			const double qpcPerMs = (double)st->qpcFreq / 1000.0;
			const double fdiv = frames ? (double)frames : 1.0;
			const unsigned samples = B.mainSamples - A.mainSamples;
			v.windowSamples = samples;
			v.probeSamplerMs = (double)(B.stallQpc - A.stallQpc) / qpcPerMs / fdiv;
			v.probeOwnMs = (double)(B.ownQpc - A.ownQpc) / qpcPerMs / fdiv;
			v.probeScriptMs = (double)(B.scriptCalls - A.scriptCalls) * st->scriptCostPerCallMs / fdiv;
			v.probeMs = v.probeSamplerMs + v.probeOwnMs + v.probeScriptMs;
			v.stallUs = samples ? (double)(B.stallQpc - A.stallQpc) / qpcPerMs * 1000.0 / samples : 0.0;
			const double spanSec = (B.tMs - startMs) / 1000.0;
			v.samplesPerSec = spanSec > 0.0 ? samples / spanSec : 0.0;

			// DLLs: share of the window's samples x the window's mean frame time
			v.dlls.clear();
			const int modCount = (std::min)((int)d.modCount, (int)kMaxModules);
			for (int m = 0; m <= modCount; m++) {
				const unsigned idx = m == modCount ? kNoModule : (unsigned)m;
				const unsigned nearS = B.near_[idx] - A.near_[idx];
				const unsigned ownS = idx == kNoModule ? 0u : B.own[idx] - A.own[idx];
				if (!nearS && !ownS) continue;
				LiveDll r;
				r.name = idx == kNoModule ? "(code outside any module)" : d.modNames[idx];
				r.role = idx == kNoModule ? "Trampolines / JIT" : RoleLabel(d.modRole[idx]);
				r.nearMs = samples ? (double)nearS / samples * v.windowAvgMs : 0.0;
				r.ownMs = samples ? (double)ownS / samples * v.windowAvgMs : 0.0;
				v.dlls.push_back(std::move(r));
			}
			std::sort(v.dlls.begin(), v.dlls.end(), [](const LiveDll& a, const LiveDll& b) { return a.nearMs > b.nearMs; });
			if (v.dlls.size() > 20) v.dlls.resize(20);

			// Scripts by plugin: exact ticks
			v.scriptMods.clear();
			const double tscPerMs = B.tMs > startMs ? (double)(B.tsc - startTsc) / (B.tMs - startMs) : 0.0;
			for (unsigned i = 0; i < 256 && tscPerMs > 0.0; i++) {
				const unsigned long long t = B.modTicks[i] - A.modTicks[i];
				if (!t) continue;
				v.scriptMods.push_back({ st->modNameTable[i], (double)t / tscPerMs / fdiv });
			}
			std::sort(v.scriptMods.begin(), v.scriptMods.end(), [](const LiveScriptMod& a, const LiveScriptMod& b) { return a.ms > b.ms; });
			if (v.scriptMods.size() > 12) v.scriptMods.resize(12);

			// Engine stages (main thread buckets and the AI threads' busy time)
			v.stages.clear();
			const unsigned bf = B.bucketFrames - A.bucketFrames;
			for (UInt32 b = 0; b < CpuSplit::kBucketCount && bf; b++) {
				if (b >= CpuSplit::bAICombat && b != CpuSplit::bAI1Busy && b != CpuSplit::bAI2Busy) continue;
				const double ms = (B.bucketSum[b] - A.bucketSum[b]) / bf;
				if (ms < 0.01) continue;
				v.stages.push_back({ CpuSplit::BucketNames[b], ms });
			}
			std::sort(v.stages.begin(), v.stages.end(), [](const LiveStage& a, const LiveStage& b) { return a.ms > b.ms; });
			if (v.stages.size() > 14) v.stages.resize(14);

			// Spikes, newest first
			v.spikesList.clear();
			for (auto it = st->spikes.rbegin(); it != st->spikes.rend() && v.spikesList.size() < 12; ++it) {
				LiveSpike s;
				s.agoSec = (nowMs - it->atMs) / 1000.0;
				s.ms = it->ms;
				if (it->dllsIn && it->dllTotal) {
					char buf[120];
					for (unsigned k = 0; k < it->dllN; k++) {
						const int m = it->dllMod[k];
						const char* name = m == (int)kNoModule ? "(outside modules)" : (m >= 0 && m < modCount ? d.modNames[m] : "?");
						sprintf_s(buf, "%s%s %d%%", k ? ", " : "", name, (int)(100.0 * it->dllCnt[k] / it->dllTotal + 0.5));
						s.dlls += buf;
					}
				}
				if (it->stage < CpuSplit::kBucketCount) {
					char buf[80]; sprintf_s(buf, "%s %.1f ms", CpuSplit::BucketNames[it->stage], it->stageMs);
					s.stage = buf;
				}
				if (it->scriptMod < 0x100 && it->scriptMs >= 0.05f) {
					char buf[160]; sprintf_s(buf, "%s %.2f ms", st->modNameTable[it->scriptMod].c_str(), it->scriptMs);
					s.script = buf;
				}
				v.spikesList.push_back(std::move(s));
			}
		}
	}

	// ---- Public API called by Render.cpp & ImGuiManager.cpp -------------------------------------------------------------

	inline bool Active() { return Running.load(std::memory_order_acquire); }
	inline bool LiveActive() { return Detail::Active && Detail::Active->live; }
	inline bool CensusActive() { return InCensusFrame; }
	inline float RemainingSeconds() { return (float)Detail::RemainingSec; }
	inline float LiveSeconds() { return Detail::Active ? (float)((CpuTimer::NowMs() - Detail::Active->startWallMs) / 1000.0) : 0.0f; }

	inline void RequestStart(double durationSec, bool reopenMenuOnFinish) {
		Detail::PendingDurationSec = durationSec;
		Detail::PendingReopenMenu = reopenMenuOnFinish;
		Detail::PendingLive = false;
		Detail::PendingStart = true;
	}

	inline void RequestStartLive() {
		Detail::PendingLive = true;
		Detail::PendingStart = true;
	}

	inline void RequestStop() {
		if (Detail::Active) Detail::Active->armStop = true;
	}

	// The live-mode key: starts live mode, or stops whatever capture runs.
	inline void ToggleLive() {
		if (Detail::Active) RequestStop();
		else RequestStartLive();
	}

	inline bool ConsumeJustFinished(bool& shouldReopenMenu) {
		if (!Detail::JustFinished) return false;
		Detail::JustFinished = false;
		shouldReopenMenu = Detail::JustFinishedReopen;
		return true;
	}

	// The menu's and HUD's drawing of probe data (QPC ticks), counted as the probe's own cost while a capture runs.
	// Main thread (PollProbeKey): the game window is in front; the watchdog's no-frame note needs it and must not call user32.
	inline void SetGameInFront(bool inFront) { Detail::GameInFront = inFront ? 1 : 0; }
	// Render.cpp: names another running stack sampler (the test build's CpuSampler / ArrivalSampler), or null.
	inline void SetOtherSamplerCheck(const char* (*check)()) { Detail::OtherSampler = check; }

	inline void AddUiCost(LONGLONG qpcTicks) { if (Detail::Active) Detail::Active->d.ownQpc += qpcTicks; }

	// The live view for a window of 1 or 10 seconds (0 = the whole session); rebuilt at most 4 times a second.
	inline const LiveView& Live(int windowSec) {
		Detail::CaptureState* st = Detail::Active;
		const double nowMs = CpuTimer::NowMs();
		if (st && (nowMs - Detail::ViewBuiltMs > 250.0 || Detail::View.windowSec != windowSec || !Detail::View.live)) {
			Detail::BuildLiveView(st, windowSec, nowMs);
			Detail::ViewBuiltMs = nowMs;
		}
		if (!st) Detail::View.live = false;
		return Detail::View;
	}

	// The game is closing (its window): stop live mode and give the writer a moment for the final report.
	inline void OnGameExit() {
		if (!Detail::Active) return;
		const bool live = Detail::Active->live;
		Detail::StopCapture();
		if (live) Writer::Flush(1000);
	}

	inline bool TickFrameBegin(IDirect3DDevice9* device, double renderStartMs, double prevRenderEndMs, double presentMs, bool hasPresent) {
		(void)device;
		Detail::AdoptResult();
		if (Detail::PendingStart && !Detail::Active) {
			Detail::PendingStart = false;
			Detail::StartCapture(Detail::PendingDurationSec, Detail::PendingReopenMenu, Detail::PendingLive);
		}
		Detail::CaptureState* st = Detail::Active;
		if (!st) return false;
		const LONGLONG own0 = Detail::QpcNow();

		if (st->armStop || (!st->live && (renderStartMs - st->startWallMs) >= st->durationSec * 1000.0)) {
			Detail::StopCapture();
			return false;
		}

		Detail::RemainingSec = st->live ? 0.0 : (std::max)(0.0, st->durationSec - (renderStartMs - st->startWallMs) * 0.001);
		const bool loading = InterfaceManager && InterfaceManager->IsActive(Menu::kMenuType_Loading);
		st->loading = loading ? 1 : 0;
		st->lastFrameQpc = own0;
		Detail::DrainSpikeDlls(st);

		if (st->prevRenderStartMs > 0.0)
			Detail::CloseFrame(st, renderStartMs - st->prevRenderStartMs, loading, renderStartMs, prevRenderEndMs, presentMs, hasPresent);
		else
			memset(st->frameModTicks, 0, sizeof(st->frameModTicks));
		st->prevRenderStartMs = renderStartMs;
		st->d.renderedFrames++;

		// Arm the 1-frame draw/light/actor census ~2s in (or frame 25) so shader warmup / menu fade-out settled
		if (!st->censusDone && !loading && (st->d.renderedFrames >= 25 || (renderStartMs - st->startWallMs) >= 1500.0)) {
			st->censusArmed = true;
			InCensusFrame = true;
		}

		if (st->live) {
			if (renderStartMs - st->lastSecondMs >= 1000.0) {
				st->lastSecondMs = renderStartMs;
				Detail::TakeSecSnap(st, renderStartMs);
			}
			// an autosave waits while the writer still has jobs (a slow disk): the queue never grows with autosaves
			if (st->autosaveSec > 0.0 && renderStartMs - st->lastAutosaveMs >= st->autosaveSec * 1000.0 && Writer::Detail::Pending == 0) {
				st->lastAutosaveMs = renderStartMs;
				Detail::PostReport(st, false);
			}
		}
		st->d.ownQpc += Detail::QpcNow() - own0;
		return true;
	}

	inline void TickFrameEnd(double renderMs) {
		Detail::CaptureState* st = Detail::Active;
		if (!st) { InCensusFrame = false; return; }
		st->pendingRenderMs = renderMs;
		st->hasPendingSplit = true;

		if (InCensusFrame) {
			const LONGLONG own0 = Detail::QpcNow();
			InCensusFrame = false;
			st->censusArmed = false;
			st->censusDone = true;
			SafeCopyCellText(st->d.cellDesc, sizeof(st->d.cellDesc));
			SafeSnapshotWorldStats(st->d.activeLights, st->d.totalLights, st->d.actorsHigh, st->d.actorsMidHigh, st->d.actorsMidLow, st->d.actorsLow, st->d.loadedPlugins);
			MEMORYSTATUSEX ms = { sizeof(ms) };
			if (GlobalMemoryStatusEx(&ms)) {
				st->d.vasTotalMB = ms.ullTotalVirtual / 1048576.0;
				st->d.vasUsedMB = (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0;
			}
			st->d.ownQpc += Detail::QpcNow() - own0;
		}
	}

	inline void NoteShaderBind() {
		if (InCensusFrame && Detail::Active) Detail::Active->d.censusShaderBinds++;
	}

	inline void NoteDraw(const NiGeometry* geom, bool skinned, int shaderContext) {
		Detail::CaptureState* st = Detail::Active;
		if (!InCensusFrame || !st) return;
		Detail::Counters& d = st->d;
		d.censusTotalDraws++;
		if (skinned) d.censusSkinnedDraws++;
		if (shaderContext == 0) d.censusWorldDraws++;
		else if (shaderContext == 1) d.censusReflDraws++;
		else if (shaderContext == 2) d.censusFirstPersonDraws++;
		else d.censusOtherDraws++;

		UInt8 modIdx = 0xFF;
		char meshName[80] = {};
		SafeExtractDrawOwner(geom, modIdx, meshName, sizeof(meshName));
		d.censusByMod[modIdx].draws++;
		if (skinned) d.censusByMod[modIdx].skinned++;
		if (meshName[0] && st->censusMeshes.size() < kMaxCensusMeshes) {
			char keyBuf[96];
			sprintf_s(keyBuf, "%u\t%s", (unsigned)modIdx, meshName);
			st->censusMeshes[keyBuf]++;
		}
	}

	inline const std::string& LastReportText() { return Detail::ReportText; }
	inline const std::string& LastReportPath() { return Detail::ReportPath; }
	inline const std::string& LastStatus() { return Detail::StatusText; }
	inline const Summary& LastSummary() { return Detail::CachedSummary; }
}
