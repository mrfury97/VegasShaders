#pragma once

// Full-game performance probe ("Help > Performance probe..."). Zero cost when idle: nothing samples, patches ScriptRunner::Run,
// or inspects geometry until the user explicitly triggers a timed capture (5s / 10s / 20s). Combines:
//   1. Frame time distribution & CPU Update vs Render CPU vs Present/GPU-Wait split
//   2. CpuSplit per-engine-stage breakdown (Main thread + AI worker threads)
//   3. 1 kHz stack-sampling profiler broken down individually by every loaded DLL (FalloutNV.exe, VegasShaders.dll, every NVSE plugin, wrappers, DXVK/driver)
//   4. Lazy Detours hook on ScriptRunner::Run (0x005E0D20) for per-script & per-ESP/ESM mod inclusive/self timing
//   5. 1-frame Scene Census: draw calls by owning ESP/ESM & mesh, skinned draws, shader binds, active ShadowSceneNode lights, ProcessLists actors, VAS memory
// Writes a self-contained report to VegasShaders-Logs\DeepProbe_<timestamp>.txt and populates an in-menu UI summary.

#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ctime>
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

namespace DeepProbe {

	static constexpr unsigned kMaxModules = 256;
	static constexpr unsigned kMaxRanges = 1024;
	static constexpr unsigned kSamplerHz = 1000;
	static constexpr unsigned kStackCopyMax = 128 * 1024;
	static constexpr unsigned kMaxScripts = 8192;
	static constexpr unsigned kMaxCensusMeshes = 4096;
	static constexpr unsigned kNoBucket = CpuSplit::kBucketCount;
	static constexpr unsigned kRows = CpuSplit::kBucketCount + 1;

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

	// ---- Summary data surfaced to ImGui window ---------------------------------------------------------------------------

	struct StageRow {
		std::string name;
		double avgMs = 0.0, p95Ms = 0.0, maxMs = 0.0, pct = 0.0, busyPct = -1.0;
	};
	struct DllRow {
		std::string name, role, topStage;
		double exclMs = 0.0, exclPct = 0.0, inclMs = 0.0, inclPct = 0.0, waitPct = 0.0;
		unsigned exclSamples = 0, inclSamples = 0;
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

	struct Summary {
		bool valid = false;
		double durationSec = 0.0;
		unsigned frames = 0;
		double avgMs = 0.0, p50Ms = 0.0, p95Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
		double avgFps = 0.0, low1Fps = 0.0, low01Fps = 0.0;
		double updateMs = 0.0, renderCpuMs = 0.0, presentMs = 0.0, gpuVegasMs = 0.0;
		double ai1BusyMs = 0.0, ai2BusyMs = 0.0;
		double vasUsedMB = 0.0, vasTotalMB = 0.0;
		unsigned loadedPlugins = 0, loadedDlls = 0;
		unsigned censusWorldDraws = 0, censusFirstPersonDraws = 0, censusReflDraws = 0, censusOtherDraws = 0;
		unsigned censusTotalDraws = 0, censusSkinnedDraws = 0, censusShaderBinds = 0;
		unsigned activeLights = 0, totalLights = 0;
		unsigned actorsHigh = 0, actorsMidHigh = 0, actorsMidLow = 0, actorsLow = 0;
		unsigned samplerMainSamples = 0, samplerAi1Samples = 0, samplerAi2Samples = 0;
		unsigned long long totalScriptCalls = 0;
		double totalScriptSelfMs = 0.0;
		bool scriptHookOk = false;
		std::string cellDesc, runtimeDesc, verdict;
		std::vector<StageRow> stages;
		std::vector<DllRow> dlls;
		std::vector<DriverCallerRow> driverCallers;
		std::vector<ScriptModRow> scriptMods;
		std::vector<ScriptRow> scripts;
		std::vector<GpuPassRow> gpuPasses;
		std::vector<DrawModRow> drawMods;
		std::vector<DrawMeshRow> drawMeshes;
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

		struct CaptureState {
			double durationSec = 10.0;
			bool reopenMenu = true;
			bool armStop = false;
			double startWallMs = 0.0;
			double stopWallMs = 0.0;
			double prevRenderStartMs = 0.0;
			double pendingRenderMs = 0.0;
			bool hasPendingSplit = false;
			double sumUpdateMs = 0.0, sumRenderMs = 0.0, sumPresentMs = 0.0;
			unsigned splitFrames = 0;
			unsigned frames = 0;
			std::vector<double> frameTimesMs;

			// CpuSplit per-frame buckets
			std::vector<float> bucketSamples[CpuSplit::kBucketCount];
			double bucketSumMs[CpuSplit::kBucketCount] = {};
			double bucketMaxMs[CpuSplit::kBucketCount] = {};
			unsigned bucketFrames = 0;

			// GPU passes
			std::unordered_map<std::string, GpuStat> gpuPasses;

			// 1-frame Census
			bool censusArmed = false;
			bool censusDone = false;
			unsigned censusWorldDraws = 0, censusFirstPersonDraws = 0, censusReflDraws = 0, censusOtherDraws = 0;
			unsigned censusTotalDraws = 0, censusSkinnedDraws = 0, censusShaderBinds = 0;
			unsigned activeLights = 0, totalLights = 0;
			unsigned actorsHigh = 0, actorsMidHigh = 0, actorsMidLow = 0, actorsLow = 0;
			unsigned loadedPlugins = 0;
			double vasUsedMB = 0.0, vasTotalMB = 0.0;
			char cellDesc[200] = {};
			CensusMod censusByMod[256] = {};
			std::unordered_map<std::string, unsigned> censusMeshes;

			// Script profiler
			unsigned long long startTsc = 0, endTsc = 0;
			std::unordered_map<UInt64, ScriptRec> scripts;

			// 1 kHz Sampler data
			HANDLE threadHandle = nullptr;
			std::atomic<bool> stopSampler{ false };
			HANDLE threadHandles[3] = {};
			DWORD threadIds[3] = {}, stackBase[3] = {}, stackBottom[3] = {};
			unsigned char* stackCopy = nullptr;
			Range ranges[kMaxRanges] = {};
			int rangeCount = 0;
			char modNames[kMaxModules][64] = {};
			char modPaths[kMaxModules][260] = {};
			DWORD modBase[kMaxModules] = {}, modSize[kMaxModules] = {};
			ModRole modRole[kMaxModules] = {};
			int modCount = 0;
			int wow64Cpu = -1;
			LONGLONG qpcFreq = 1, calQpc = 0;
			unsigned long long calTsc = 0;
			ULONG64 lastCycles[3] = {};
			LONGLONG lastBusyQpc[3] = {};
			unsigned threadSamples[3] = {};
			unsigned exclSamples[kMaxModules][3] = {};
			unsigned inclSamples[kMaxModules][3] = {};
			unsigned modKernelSamples[kMaxModules] = {};
			unsigned driverCallerSamples[kMaxModules] = {};
			unsigned bucketModSamples[kMaxModules][kRows] = {};
			double busySum[3][kRows] = {};
			unsigned busyN[3][kRows] = {};
			std::unordered_map<UInt64, unsigned> exeLeaves;	// ((UInt64)bucket << 32) | rva
		};

		inline CaptureState* Active = nullptr;

		// UI / Control state (main thread only)
		inline bool PendingStart = false;
		inline double PendingDurationSec = 10.0;
		inline bool PendingReopenMenu = true;
		inline bool JustFinished = false;
		inline bool JustFinishedReopen = false;
		inline double RemainingSec = 0.0;
		inline std::string ReportText;
		inline std::string ReportPath;
		inline std::string StatusText = "Idle";
		inline Summary CachedSummary;

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

		inline bool SafeParsePeExecSections(DWORD base, int m, Range* ranges, int& rc) {
			__try {
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(uintptr_t)base;
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
				const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((uintptr_t)base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
				const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
				for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && rc < (int)kMaxRanges; i++) {
					if (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
						const DWORD sz = (std::max)(s[i].Misc.VirtualSize, s[i].SizeOfRawData);
						ranges[rc++] = { base + s[i].VirtualAddress, base + s[i].VirtualAddress + sz, m };
					}
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		inline void ScanModules(CaptureState* st) {
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
			if (snap == INVALID_HANDLE_VALUE) return;
			MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
			Range ranges[kMaxRanges];
			int rc = 0;
			for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
				const DWORD base = (DWORD)(uintptr_t)me.modBaseAddr, size = me.modBaseSize;
				int m = -1;
				for (int i = 0; i < st->modCount; i++) {
					if (st->modBase[i] == base && st->modSize[i] == size) { m = i; break; }
				}
				if (m < 0 && st->modCount < (int)kMaxModules) {
					m = st->modCount++;
					st->modBase[m] = base; st->modSize[m] = size;
					if (!WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, st->modNames[m], 64, nullptr, nullptr)) strcpy_s(st->modNames[m], "?");
					if (!WideCharToMultiByte(CP_ACP, 0, me.szExePath, -1, st->modPaths[m], 260, nullptr, nullptr)) strcpy_s(st->modPaths[m], "");
					st->modRole[m] = ClassifyModule(st->modNames[m], st->modPaths[m]);
					if (!_stricmp(st->modNames[m], "wow64cpu.dll")) st->wow64Cpu = m;
				}
				if (m >= 0) SafeParsePeExecSections(base, m, ranges, rc);
			}
			CloseHandle(snap);
			std::sort(ranges, ranges + rc, [](const Range& a, const Range& b) { return a.Start < b.Start; });
			memcpy(st->ranges, ranges, sizeof(Range) * rc);
			st->rangeCount = rc;
		}

		inline int ModuleAt(const CaptureState* st, DWORD v) {
			int lo = 0, hi = st->rangeCount - 1;
			while (lo <= hi) {
				const int mid = (lo + hi) >> 1;
				if (v < st->ranges[mid].Start) hi = mid - 1;
				else if (v >= st->ranges[mid].End) lo = mid + 1;
				else return st->ranges[mid].Module;
			}
			return -1;
		}

		inline bool AfterCall(DWORD v) {
			__try {
				const unsigned char* p = (const unsigned char*)(uintptr_t)v;
				if (p[-5] == 0xE8) return true;
				if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;
				if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;
				if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;
				if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;
				if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;
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
				if (!query || query(h, 0, &info, sizeof(info), NULL) < 0 || !info.Teb) { CloseHandle(h); continue; }
				DWORD base = 0, bottom = 0;
				__try {
					base = *(const DWORD*)((const UInt8*)info.Teb + 0x04);
					bottom = *(const DWORD*)((const UInt8*)info.Teb + 0xE0C);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { CloseHandle(h); continue; }
				st->threadIds[t] = id;
				st->threadHandles[t] = h;
				st->stackBase[t] = base;
				st->stackBottom[t] = bottom;
			}
		}

		inline void TakeSample(CaptureState* st, unsigned t) {
			HANDLE th = st->threadHandles[t];
			if (!th || !st->stackCopy) return;
			CONTEXT ctx = {}; ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			DWORD len = 0;
			unsigned bucket = kNoBucket;
			if (SuspendThread(th) == (DWORD)-1) return;
			// ---- Suspended: NO heap allocation, NO locks, NO logging ----
			const BOOL got = GetThreadContext(th, &ctx);
			if (got && ctx.Esp >= st->stackBottom[t] && ctx.Esp < st->stackBase[t]) {
				len = (std::min)(st->stackBase[t] - ctx.Esp, (DWORD)kStackCopyMax);
				memcpy(st->stackCopy, (const void*)(uintptr_t)ctx.Esp, len);
			}
			bucket = BucketNow(t);
			ULONG64 cycles = 0;
			const BOOL gotCycles = QueryThreadCycleTime(th, &cycles);
			const LONGLONG cycleQpc = QpcNow();
			ResumeThread(th);
			// ---- Running again ----

			if (gotCycles) {
				const double tscPerQpc = cycleQpc > st->calQpc ? (double)(__rdtsc() - st->calTsc) / (double)(QpcNow() - st->calQpc) : 0.0;
				if (st->lastBusyQpc[t] && tscPerQpc > 0.0 && cycleQpc > st->lastBusyQpc[t]) {
					double busy = (double)(cycles - st->lastCycles[t]) / ((double)(cycleQpc - st->lastBusyQpc[t]) * tscPerQpc);
					busy = busy < 0.0 ? 0.0 : busy > 1.0 ? 1.0 : busy;
					st->busySum[t][bucket] += busy;
					st->busyN[t][bucket]++;
				}
				st->lastCycles[t] = cycles;
				st->lastBusyQpc[t] = cycleQpc;
			}
			if (!got) return;

			st->threadSamples[t]++;
			const int leafMod = ModuleAt(st, ctx.Eip);
			const ModRole leafRole = leafMod >= 0 ? st->modRole[leafMod] : rSystemWrap;
			const bool inKernel = (leafMod < 0 || leafMod == st->wow64Cpu || (leafRole == rSystemWrap && AfterCall(ctx.Eip)));

			int exclMod = (leafMod >= 0 && leafRole != rSystemWrap) ? leafMod : -1;
			DWORD exclAddr = exclMod >= 0 ? ctx.Eip : 0;
			int driverCaller = -1;

			int seen[32];
			int seenCount = 0;
			if (leafMod >= 0 && leafRole != rSystemWrap) seen[seenCount++] = leafMod;

			const DWORD* w = (const DWORD*)st->stackCopy;
			const DWORD words = len / 4;
			for (DWORD i = 0; i < words; i++) {
				const DWORD v = w[i];
				if (v < 0x10000) continue;
				const int m = ModuleAt(st, v);
				if (m < 0 || !AfterCall(v)) continue;
				const ModRole r = st->modRole[m];
				if (exclMod < 0 && r != rSystemWrap) {
					exclMod = m;
					exclAddr = v;
				}
				if (exclMod >= 0 && driverCaller < 0 && st->modRole[exclMod] == rDriver &&
					(r == rGame || r == rVegasShaders || r == rNvsePlugin || r == rOtherTool)) {
					driverCaller = m;
				}
				if (r != rSystemWrap && seenCount < 32) {
					bool dup = false;
					for (int k = 0; k < seenCount; k++) if (seen[k] == m) { dup = true; break; }
					if (!dup) seen[seenCount++] = m;
				}
			}

			if (exclMod >= 0) {
				st->exclSamples[exclMod][t]++;
				st->bucketModSamples[exclMod][bucket]++;
				if (inKernel) st->modKernelSamples[exclMod]++;
				if (st->modRole[exclMod] == rGame && t == 0 && exclAddr >= st->modBase[exclMod]) {
					const DWORD rva = exclAddr - st->modBase[exclMod];
					st->exeLeaves[((UInt64)bucket << 32) | (UInt64)rva]++;
				}
				if (st->modRole[exclMod] == rDriver && t == 0 && driverCaller >= 0) {
					st->driverCallerSamples[driverCaller]++;
				}
			}
			for (int k = 0; k < seenCount; k++) {
				st->inclSamples[seen[k]][t]++;
			}
		}

		inline DWORD WINAPI SamplerThreadMain(void* param) {
			CaptureState* st = (CaptureState*)param;
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
			const LONGLONG period = -10000000LL / (LONGLONG)kSamplerHz;
			ScanModules(st);
			LONGLONG lastScan = QpcNow();
			while (!st->stopSampler.load(std::memory_order_relaxed)) {
				if (timer) {
					LARGE_INTEGER due; due.QuadPart = period;
					if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
					else Sleep(1);
				}
				else Sleep(1);
				if (st->stopSampler.load(std::memory_order_relaxed)) break;
				SafeOpenAIThreads(st);
				const LONGLONG now = QpcNow();
				if (now - lastScan > 2 * st->qpcFreq) { ScanModules(st); lastScan = now; }
				for (unsigned t = 0; t < 3; t++) TakeSample(st, t);
			}
			if (timer) CloseHandle(timer);
			return 0;
		}

		inline void OnCpuSplitBuckets(const double* ms, UInt32 count) {
			CaptureState* st = Active;
			if (!st || !ms) return;
			const UInt32 n = (std::min)(count, (UInt32)CpuSplit::kBucketCount);
			for (UInt32 b = 0; b < n; b++) {
				const double v = ms[b];
				st->bucketSamples[b].push_back((float)v);
				st->bucketSumMs[b] += v;
				if (v > st->bucketMaxMs[b]) st->bucketMaxMs[b] = v;
			}
			st->bucketFrames++;
		}

		inline void OnGpuPassSample(const char* name, double ms) {
			CaptureState* st = Active;
			if (!st || !name) return;
			GpuStat& g = st->gpuPasses[name];
			g.sumMs += ms;
			if (ms > g.maxMs) g.maxMs = ms;
			g.samples++;
		}

		// ---- Report generation -------------------------------------------------------------------------------------------

		inline std::string FormatTimeStampFilename() {
			SYSTEMTIME t; GetLocalTime(&t);
			char name[128];
			sprintf_s(name, "%s\\%s\\DeepProbe_%04u-%02u-%02u_%02u-%02u-%02u.txt",
				Paths::GameRoot().c_str(), Paths::LogsFolder,
				t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
			return name;
		}

		inline void FinalizeAndBuildReport(CaptureState* st) {
			Summary sum;
			sum.valid = true;
			sum.durationSec = (st->stopWallMs - st->startWallMs) / 1000.0;
			if (sum.durationSec < 0.001) sum.durationSec = 0.001;
			sum.scriptHookOk = ScriptHookOk;
			sum.loadedPlugins = st->loadedPlugins;
			sum.loadedDlls = (unsigned)st->modCount;
			sum.vasUsedMB = st->vasUsedMB;
			sum.vasTotalMB = st->vasTotalMB;
			sum.cellDesc = st->cellDesc;
			sum.runtimeDesc = TheRenderManager && TheRenderManager->D3D9RuntimeDescription() ? TheRenderManager->D3D9RuntimeDescription() : "?";
			sum.activeLights = st->activeLights;
			sum.totalLights = st->totalLights;
			sum.actorsHigh = st->actorsHigh;
			sum.actorsMidHigh = st->actorsMidHigh;
			sum.actorsMidLow = st->actorsMidLow;
			sum.actorsLow = st->actorsLow;
			sum.censusWorldDraws = st->censusWorldDraws;
			sum.censusFirstPersonDraws = st->censusFirstPersonDraws;
			sum.censusReflDraws = st->censusReflDraws;
			sum.censusOtherDraws = st->censusOtherDraws;
			sum.censusTotalDraws = st->censusTotalDraws;
			sum.censusSkinnedDraws = st->censusSkinnedDraws;
			sum.censusShaderBinds = st->censusShaderBinds;
			sum.samplerMainSamples = st->threadSamples[0];
			sum.samplerAi1Samples = st->threadSamples[1];
			sum.samplerAi2Samples = st->threadSamples[2];

			std::vector<double> ft = st->frameTimesMs;
			const FrameTimeSummary fs = SummarizeFrameTimes(ft);
			sum.frames = fs.frames > 0 ? fs.frames : st->frames;
			sum.avgMs = fs.avgMs;
			sum.p50Ms = fs.p50Ms;
			sum.p95Ms = fs.p95Ms;
			sum.p99Ms = fs.p99Ms;
			sum.maxMs = fs.maxMs;
			sum.avgFps = fs.avgMs > 0.0 ? 1000.0 / fs.avgMs : 0.0;
			sum.low1Fps = fs.low1Fps;
			sum.low01Fps = fs.low01Fps;

			if (st->splitFrames > 0) {
				sum.updateMs = st->sumUpdateMs / st->splitFrames;
				sum.renderCpuMs = st->sumRenderMs / st->splitFrames;
				sum.presentMs = st->sumPresentMs / st->splitFrames;
			}

			// GPU passes
			double frameTotalGpuMs = 0.0;
			for (const auto& kv : st->gpuPasses) {
				if (!kv.second.samples) continue;
				GpuPassRow r;
				r.name = kv.first;
				r.avgMs = kv.second.sumMs / kv.second.samples;
				r.maxMs = kv.second.maxMs;
				r.samples = kv.second.samples;
				if (r.name == "Game frame total") {
					frameTotalGpuMs = r.avgMs;
				}
				else if (r.name.compare(0, 2, "  ") != 0 &&
					r.name.find("(game)") == std::string::npos &&
					r.name != "Game call 0x871A50" &&
					r.name != "Game image space" &&
					r.name != "Pre-scene (to world scene)" &&
					r.name != "First person: deferred (all)") {
					sum.gpuVegasMs += r.avgMs;
				}
				sum.gpuPasses.push_back(std::move(r));
			}
			(void)frameTotalGpuMs;
			std::sort(sum.gpuPasses.begin(), sum.gpuPasses.end(), [](const GpuPassRow& a, const GpuPassRow& b) { return a.avgMs > b.avgMs; });

			// CpuSplit stages
			const unsigned bFrames = st->bucketFrames ? st->bucketFrames : 1;
			sum.ai1BusyMs = st->bucketSumMs[CpuSplit::bAI1Busy] / bFrames;
			sum.ai2BusyMs = st->bucketSumMs[CpuSplit::bAI2Busy] / bFrames;
			for (UInt32 b = 0; b < CpuSplit::kBucketCount; b++) {
				if (b == CpuSplit::bAI1Busy || b == CpuSplit::bAI2Busy) continue;
				const double avg = st->bucketSumMs[b] / bFrames;
				if (avg < 0.005 && st->bucketMaxMs[b] < 0.05) continue;
				StageRow r;
				r.name = CpuSplit::BucketNames[b];
				r.avgMs = avg;
				r.maxMs = st->bucketMaxMs[b];
				r.pct = sum.avgMs > 0.0 ? (100.0 * avg / sum.avgMs) : 0.0;
				auto& vec = st->bucketSamples[b];
				if (!vec.empty()) {
					std::sort(vec.begin(), vec.end());
					size_t idx = (size_t)std::ceil(0.95 * vec.size());
					if (idx < 1) idx = 1;
					if (idx > vec.size()) idx = vec.size();
					r.p95Ms = vec[idx - 1];
				}
				const unsigned tIdx = (b >= CpuSplit::bAICombat) ? (b == CpuSplit::bAI2Wait ? 2u : 1u) : 0u;
				if (st->busyN[tIdx][b] > 0) r.busyPct = 100.0 * (st->busySum[tIdx][b] / st->busyN[tIdx][b]);
				sum.stages.push_back(std::move(r));
			}
			std::sort(sum.stages.begin(), sum.stages.end(), [](const StageRow& a, const StageRow& b) { return a.avgMs > b.avgMs; });

			// Sampler DLLs (main thread wall time attribution)
			const unsigned mainTotal = st->threadSamples[0];
			const double refFrameMs = sum.avgMs > 0.0 ? sum.avgMs : (sum.durationSec * 1000.0 / (sum.frames ? sum.frames : 1u));
			for (int m = 0; m < st->modCount; m++) {
				const unsigned excl = st->exclSamples[m][0];
				const unsigned incl = st->inclSamples[m][0];
				if (!excl && !incl) continue;
				const double inclPct = mainTotal ? (100.0 * incl / mainTotal) : 0.0;
				const double exclPct = mainTotal ? (100.0 * excl / mainTotal) : 0.0;
				if (excl == 0 && inclPct < 0.3) continue;
				DllRow d;
				d.name = st->modNames[m];
				d.role = RoleLabel(st->modRole[m]);
				d.exclSamples = excl;
				d.inclSamples = incl;
				d.exclPct = exclPct;
				d.inclPct = inclPct;
				d.exclMs = (exclPct * 0.01) * refFrameMs;
				d.inclMs = (inclPct * 0.01) * refFrameMs;
				d.waitPct = excl ? (100.0 * st->modKernelSamples[m] / excl) : 0.0;
				unsigned bestB = kNoBucket, bestCnt = 0;
				for (unsigned b = 0; b < CpuSplit::kBucketCount; b++) {
					if (st->bucketModSamples[m][b] > bestCnt) { bestCnt = st->bucketModSamples[m][b]; bestB = b; }
				}
				d.topStage = bestB < CpuSplit::kBucketCount ? CpuSplit::BucketNames[bestB] : "-";
				sum.dlls.push_back(std::move(d));
			}
			std::sort(sum.dlls.begin(), sum.dlls.end(), [](const DllRow& a, const DllRow& b) {
				if (a.exclSamples != b.exclSamples) return a.exclSamples > b.exclSamples;
				return a.inclSamples > b.inclSamples;
			});

			// Driver callers
			for (int m = 0; m < st->modCount; m++) {
				const unsigned c = st->driverCallerSamples[m];
				if (!c || !mainTotal) continue;
				DriverCallerRow r;
				r.callerName = st->modNames[m];
				r.callerRole = RoleLabel(st->modRole[m]);
				r.samples = c;
				r.pct = 100.0 * c / mainTotal;
				r.ms = (r.pct * 0.01) * refFrameMs;
				sum.driverCallers.push_back(std::move(r));
			}
			std::sort(sum.driverCallers.begin(), sum.driverCallers.end(), [](const DriverCallerRow& a, const DriverCallerRow& b) { return a.samples > b.samples; });

			// Script profiler conversion
			const double captureWallMs = st->stopWallMs - st->startWallMs;
			double ticksPerMs = 3000000.0;
			if (st->endTsc > st->startTsc && captureWallMs > 1.0) {
				ticksPerMs = (double)(st->endTsc - st->startTsc) / captureWallMs;
			}
			const double frameDiv = sum.frames ? (double)sum.frames : 1.0;

			std::unordered_map<UInt8, ScriptModRow> byMod;
			for (const auto& kv : st->scripts) {
				const ScriptRec& sr = kv.second;
				const double selfMs = (double)sr.selfTicks / ticksPerMs / frameDiv;
				const double inclMs = (double)sr.inclTicks / ticksPerMs / frameDiv;
				const double maxMs = (double)sr.maxCallTicks / ticksPerMs;
				sum.totalScriptCalls += sr.calls;
				sum.totalScriptSelfMs += selfMs;

				char modBuf[128] = {};
				SafeCopyModName(sr.modIdx, modBuf, sizeof(modBuf));
				ScriptModRow& mr = byMod[sr.modIdx];
				mr.modIdx = sr.modIdx;
				mr.modName = modBuf;
				mr.selfMs += selfMs;
				mr.callsPerFrame += (double)sr.calls / frameDiv;
				mr.uniqueScripts++;
				if (maxMs > mr.maxCallMs) mr.maxCallMs = maxMs;

				ScriptRow row;
				row.formID = sr.formID;
				row.targetID = sr.targetID;
				row.modName = modBuf;
				row.selfMs = selfMs;
				row.inclMs = inclMs;
				row.callsPerFrame = (double)sr.calls / frameDiv;
				row.maxCallMs = maxMs;
				row.calls = sr.calls;
				if (sr.modIdx == 0xFF && sr.targetModIdx != 0xFF) {
					char tbuf[128] = {};
					SafeCopyModName(sr.targetModIdx, tbuf, sizeof(tbuf));
					row.contextMod = tbuf;
				}
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
				if (!st->censusByMod[i].draws) continue;
				DrawModRow r;
				if (i == 0xFF) r.modName = "[Land / LOD / Unlinked]";
				else {
					char mb[128] = {};
					SafeCopyModName((UInt8)i, mb, sizeof(mb));
					r.modName = mb;
				}
				r.draws = st->censusByMod[i].draws;
				r.skinned = st->censusByMod[i].skinned;
				sum.drawMods.push_back(std::move(r));
			}
			std::sort(sum.drawMods.begin(), sum.drawMods.end(), [](const DrawModRow& a, const DrawModRow& b) { return a.draws > b.draws; });
			for (const auto& kv : st->censusMeshes) {
				const size_t sep = kv.first.find('\t');
				const UInt8 mIdx = sep != std::string::npos ? (UInt8)atoi(kv.first.substr(0, sep).c_str()) : 0xFF;
				DrawMeshRow r;
				r.meshName = sep != std::string::npos ? kv.first.substr(sep + 1) : kv.first;
				if (mIdx == 0xFF) r.modName = "[Land / LOD / Unlinked]";
				else { char mb[128] = {}; SafeCopyModName(mIdx, mb, sizeof(mb)); r.modName = mb; }
				r.draws = kv.second;
				sum.drawMeshes.push_back(std::move(r));
			}
			std::sort(sum.drawMeshes.begin(), sum.drawMeshes.end(), [](const DrawMeshRow& a, const DrawMeshRow& b) { return a.draws > b.draws; });

			// Compute plain-English Verdict
			{
				double pluginsMs = 0.0, vegasCpuMs = 0.0;
				for (const auto& d : sum.dlls) {
					if (d.role == "NVSE Plugin" || d.role == "DLL / Overlay") pluginsMs += d.exclMs;
					else if (d.role == "Vegas Shaders") vegasCpuMs += d.exclMs;
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
			std::vector<std::pair<UInt64, unsigned>> exeList(st->exeLeaves.begin(), st->exeLeaves.end());
			std::sort(exeList.begin(), exeList.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

			// Format full text report
			std::string txt;
			txt.reserve(16384);
			auto appendf = [&](const char* fmt, ...) {
				char buf[1024];
				va_list args;
				va_start(args, fmt);
				_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
				va_end(args);
				txt += buf;
			};

			appendf("========================================================================================\n");
			appendf("%s %s - Deep Performance Probe Report\n", Branding::ProductName, Branding::Version);
			appendf("========================================================================================\n");
			appendf("Runtime      : %s\n", sum.runtimeDesc.c_str());
			appendf("Location     : %s\n", sum.cellDesc.c_str());
			appendf("Capture      : %.2f s | %u frames | Loaded ESP/ESM: %u | Loaded DLLs: %u\n",
				sum.durationSec, sum.frames, sum.loadedPlugins, sum.loadedDlls);
			appendf("32-bit VAS   : %.0f / %.0f MB (%.1f%% used)\n",
				sum.vasUsedMB, sum.vasTotalMB, sum.vasTotalMB > 0.0 ? (100.0 * sum.vasUsedMB / sum.vasTotalMB) : 0.0);
			appendf("Verdict      : %s\n\n", sum.verdict.c_str());

			appendf("--- 1. FRAME TIME & SPLIT --------------------------------------------------------------\n");
			appendf("FPS / Frame  : avg %.1f fps (%.2f ms) | p50 %.2f ms | p95 %.2f ms | p99 %.2f ms | max %.2f ms\n",
				sum.avgFps, sum.avgMs, sum.p50Ms, sum.p95Ms, sum.p99Ms, sum.maxMs);
			appendf("Lows         : 1%% low %.1f fps | 0.1%% low %.1f fps\n", sum.low1Fps, sum.low01Fps);
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

			appendf("--- 3. LOADED DLLs / PLUGINS (1 kHz Stack Sampler, %u Main Samples) -------------------\n", mainTotal);
			appendf("%-28s %-14s %8s %7s %8s %7s %6s  %s\n",
				"Module", "Role", "Excl ms", "Excl %", "Incl ms", "Incl %", "Wait%", "Top Stage");
			for (size_t i = 0; i < sum.dlls.size() && i < 35; i++) {
				const DllRow& d = sum.dlls[i];
				appendf("%-28s %-14s %8.2f %6.1f%% %8.2f %6.1f%% %5.0f%%  %s\n",
					d.name.c_str(), d.role.c_str(), d.exclMs, d.exclPct, d.inclMs, d.inclPct, d.waitPct, d.topStage.c_str());
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
					const unsigned b = (unsigned)(exeList[i].first >> 32);
					const DWORD rva = (DWORD)(exeList[i].first & 0xFFFFFFFFu);
					const double pct = 100.0 * exeList[i].second / mainTotal;
					appendf("  FalloutNV.exe+0x%06X  %5u samples (%5.2f%%)  [%s]\n",
						(unsigned)rva, exeList[i].second, pct, b < CpuSplit::kBucketCount ? CpuSplit::BucketNames[b] : "-");
				}
				appendf("\n");
			}

			// Keep UI vectors concise
			if (sum.stages.size() > 32) sum.stages.resize(32);
			if (sum.dlls.size() > 40) sum.dlls.resize(40);
			if (sum.scriptMods.size() > 30) sum.scriptMods.resize(30);
			if (sum.scripts.size() > 40) sum.scripts.resize(40);
			if (sum.gpuPasses.size() > 30) sum.gpuPasses.resize(30);
			if (sum.drawMods.size() > 25) sum.drawMods.resize(25);
			if (sum.drawMeshes.size() > 25) sum.drawMeshes.resize(25);

			const std::string logsDir = Paths::GameRoot() + "\\" + Paths::LogsFolder;
			CreateDirectoryA(logsDir.c_str(), nullptr);
			const std::string outPath = FormatTimeStampFilename();
			FILE* out = _fsopen(outPath.c_str(), "w", _SH_DENYWR);
			if (out) {
				fwrite(txt.data(), 1, txt.size(), out);
				fclose(out);
				Logger::Log("DEEP PROBE: saved report to %s (%.1f fps avg, %.2f ms/f scripts)", outPath.c_str(), sum.avgFps, sum.totalScriptSelfMs);
			}
			else {
				Logger::Log("[WARNING] DEEP PROBE: could not write report file %s", outPath.c_str());
			}

			ReportText = std::move(txt);
			ReportPath = out ? outPath : "(failed to write file - use Copy Report)";
			CachedSummary = std::move(sum);
			char sbuf[128];
			sprintf_s(sbuf, "Done: %.1f fps avg (1%% low %.1f fps), %u frames captured", CachedSummary.avgFps, CachedSummary.low1Fps, CachedSummary.frames);
			StatusText = sbuf;
		}

		inline void StartCapture(double durationSec, bool reopenMenu) {
			if (Active) return;
			EnsureScriptHook();
			CaptureState* st = new CaptureState();
			st->durationSec = durationSec < 1.0 ? 1.0 : durationSec > 60.0 ? 60.0 : durationSec;
			st->reopenMenu = reopenMenu;
			st->qpcFreq = QpcNow();
			{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); st->qpcFreq = f.QuadPart ? f.QuadPart : 1; }
			st->calQpc = QpcNow();
			st->calTsc = __rdtsc();
			MainThreadId = GetCurrentThreadId();
			st->threadIds[0] = MainThreadId;
			st->stackBase[0] = __readfsdword(0x04);
			st->stackBottom[0] = __readfsdword(0xE0C);
			st->threadHandles[0] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, MainThreadId);
			st->stackCopy = (unsigned char*)VirtualAlloc(nullptr, kStackCopyMax, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			st->frameTimesMs.reserve((size_t)(st->durationSec * 240.0) + 64);
			st->scripts.reserve(512);
			st->startWallMs = CpuTimer::NowMs();
			st->startTsc = __rdtsc();

			Active = st;
			Running.store(true, std::memory_order_release);
			RemainingSec = st->durationSec;
			StatusText = "Capturing...";

			GpuTimer::OnSample = &OnGpuPassSample;
			GpuTimer::ProbeActive = true;
			CpuSplit::OnFrameBuckets = &OnCpuSplitBuckets;
			ScriptDepth = 0;
			ScriptActive = ScriptHookOk;

			st->threadHandle = CreateThread(nullptr, 256 * 1024, &SamplerThreadMain, st, 0, nullptr);
			Logger::Log("DEEP PROBE: started %.0f s capture", st->durationSec);
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

			st->stopSampler.store(true, std::memory_order_release);
			if (st->threadHandle) {
				WaitForSingleObject(st->threadHandle, 3000);
				CloseHandle(st->threadHandle);
				st->threadHandle = nullptr;
			}
			for (int t = 0; t < 3; t++) {
				if (st->threadHandles[t]) { CloseHandle(st->threadHandles[t]); st->threadHandles[t] = nullptr; }
			}
			if (st->stackCopy) { VirtualFree(st->stackCopy, 0, MEM_RELEASE); st->stackCopy = nullptr; }

			if (!st->censusDone) {
				SafeCopyCellText(st->cellDesc, sizeof(st->cellDesc));
				SafeSnapshotWorldStats(st->activeLights, st->totalLights, st->actorsHigh, st->actorsMidHigh, st->actorsMidLow, st->actorsLow, st->loadedPlugins);
				MEMORYSTATUSEX ms = { sizeof(ms) };
				if (GlobalMemoryStatusEx(&ms)) {
					st->vasTotalMB = ms.ullTotalVirtual / 1048576.0;
					st->vasUsedMB = (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0;
				}
			}

			Running.store(false, std::memory_order_release);
			RemainingSec = 0.0;
			Active = nullptr;
			JustFinished = true;
			JustFinishedReopen = st->reopenMenu;

			FinalizeAndBuildReport(st);
			delete st;
		}
	}

	// ---- Public API called by Render.cpp & ImGuiManager.cpp -------------------------------------------------------------

	inline bool Active() { return Running.load(std::memory_order_acquire); }
	inline bool CensusActive() { return InCensusFrame; }
	inline float RemainingSeconds() { return (float)Detail::RemainingSec; }

	inline void RequestStart(double durationSec, bool reopenMenuOnFinish) {
		Detail::PendingDurationSec = durationSec;
		Detail::PendingReopenMenu = reopenMenuOnFinish;
		Detail::PendingStart = true;
	}

	inline void RequestStop() {
		if (Detail::Active) Detail::Active->armStop = true;
	}

	inline bool ConsumeJustFinished(bool& shouldReopenMenu) {
		if (!Detail::JustFinished) return false;
		Detail::JustFinished = false;
		shouldReopenMenu = Detail::JustFinishedReopen;
		return true;
	}

	inline bool TickFrameBegin(IDirect3DDevice9* device, double renderStartMs, double prevRenderEndMs, double presentMs, bool hasPresent) {
		(void)device;
		if (Detail::PendingStart && !Detail::Active) {
			Detail::PendingStart = false;
			Detail::StartCapture(Detail::PendingDurationSec, Detail::PendingReopenMenu);
		}
		Detail::CaptureState* st = Detail::Active;
		if (!st) return false;

		if (st->armStop || (renderStartMs - st->startWallMs) >= st->durationSec * 1000.0) {
			Detail::StopCapture();
			return false;
		}

		Detail::RemainingSec = (std::max)(0.0, st->durationSec - (renderStartMs - st->startWallMs) * 0.001);

		if (st->prevRenderStartMs > 0.0) {
			const double ft = renderStartMs - st->prevRenderStartMs;
			if (ft > 0.0 && ft < 1000.0) {
				st->frameTimesMs.push_back(ft);
				if (st->hasPendingSplit && prevRenderEndMs > 0.0 && hasPresent) {
					const double between = renderStartMs - prevRenderEndMs;
					if (between >= 0.0 && between < 1000.0) {
						const double pres = (std::min)(presentMs, between);
						const double upd = between - pres;
						st->sumUpdateMs += upd;
						st->sumRenderMs += st->pendingRenderMs;
						st->sumPresentMs += pres;
						st->splitFrames++;
					}
				}
			}
		}
		st->prevRenderStartMs = renderStartMs;
		st->frames++;

		// Arm the 1-frame draw/light/actor census ~2s in (or frame 25) so shader warmup / menu fade-out settled
		if (!st->censusDone && (st->frames >= 25 || (renderStartMs - st->startWallMs) >= 1500.0)) {
			st->censusArmed = true;
			InCensusFrame = true;
		}
		return true;
	}

	inline void TickFrameEnd(double renderMs) {
		Detail::CaptureState* st = Detail::Active;
		if (!st) { InCensusFrame = false; return; }
		st->pendingRenderMs = renderMs;
		st->hasPendingSplit = true;

		if (InCensusFrame) {
			InCensusFrame = false;
			st->censusArmed = false;
			st->censusDone = true;
			SafeCopyCellText(st->cellDesc, sizeof(st->cellDesc));
			SafeSnapshotWorldStats(st->activeLights, st->totalLights, st->actorsHigh, st->actorsMidHigh, st->actorsMidLow, st->actorsLow, st->loadedPlugins);
			MEMORYSTATUSEX ms = { sizeof(ms) };
			if (GlobalMemoryStatusEx(&ms)) {
				st->vasTotalMB = ms.ullTotalVirtual / 1048576.0;
				st->vasUsedMB = (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0;
			}
		}
	}

	inline void NoteShaderBind() {
		if (InCensusFrame && Detail::Active) Detail::Active->censusShaderBinds++;
	}

	inline void NoteDraw(const NiGeometry* geom, bool skinned, int shaderContext) {
		Detail::CaptureState* st = Detail::Active;
		if (!InCensusFrame || !st) return;
		st->censusTotalDraws++;
		if (skinned) st->censusSkinnedDraws++;
		if (shaderContext == 0) st->censusWorldDraws++;
		else if (shaderContext == 1) st->censusReflDraws++;
		else if (shaderContext == 2) st->censusFirstPersonDraws++;
		else st->censusOtherDraws++;

		UInt8 modIdx = 0xFF;
		char meshName[80] = {};
		SafeExtractDrawOwner(geom, modIdx, meshName, sizeof(meshName));
		st->censusByMod[modIdx].draws++;
		if (skinned) st->censusByMod[modIdx].skinned++;
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
