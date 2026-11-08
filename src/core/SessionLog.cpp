// UNOFFICIAL 2026-11-01: the session header, live changes, heartbeat, exit / crash summary and the frame journal (SessionLog.h).
#include "SessionLog.h"
#include "FrameTimeStats.h"
#include "DxvkCompute.h"
#include "Diagnostics.h"
#include "SettingChanges.h"
#include "InstallCheck.h"
#include "DxvkLoader.h"
#include <intrin.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cmath>
#include <mutex>
#if __has_include("NvrBuildCommit.h")
#include "NvrBuildCommit.h"	// written by the project's pre-build step: #define NVR_BUILD_COMMIT "<git commit>[-dirty] <branch>"
#endif
#ifndef NVR_BUILD_COMMIT
#define NVR_BUILD_COMMIT "unknown"
#endif

// UNOFFICIAL 2026-11-01 (Environment): the game window (OnDevice) and the environment text last logged
static HWND GameWindow = nullptr;
static std::string LastEnvironment;

namespace {

	double Seconds() {
		static LARGE_INTEGER frequency = {}, start = {};
		LARGE_INTEGER now;
		if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); }
		QueryPerformanceCounter(&now);
		return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
	}

	std::string Uptime(double s) {
		char text[48];
		const unsigned t = (unsigned)s;
		sprintf_s(text, "%uh%02um%02us", t / 3600, t / 60 % 60, t % 60);
		return text;
	}

	// Lines of up to ~900 characters: "<head> item, item, ..." (long lists stay readable and under the Logger's stack buffer)
	void LogList(const char* head, const std::vector<std::string>& items, const char* separator = ", ") {
		std::string line;
		for (const std::string& item : items) {
			if (!line.empty() && line.size() + item.size() > 900) { Logger::Log("%s%s", head, line.c_str()); line.clear(); }
			if (!line.empty()) line += separator;
			line += item;
		}
		if (!line.empty()) Logger::Log("%s%s", head, line.c_str());
	}

	std::string RegString(HKEY root, const char* path, const char* name) {
		char value[512] = {};
		DWORD size = sizeof(value) - 1;
		if (RegGetValueA(root, path, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return std::string();
		return value;
	}

	std::string FileVersion(const char* module) {
		HMODULE handle = _stricmp(module, "d3d9.dll") ? GetModuleHandleA(module) : DxvkLoader::Runtime();	// (a mod's d3d9.dll is not the one named so)
		char path[MAX_PATH] = {};
		if (!handle || !GetModuleFileNameA(handle, path, MAX_PATH)) return std::string();
		DWORD unused = 0;
		const DWORD size = GetFileVersionInfoSizeA(path, &unused);
		if (!size) return std::string();
		std::vector<char> data(size);
		if (!GetFileVersionInfoA(path, 0, size, data.data())) return std::string();
		struct Translation { WORD language, codePage; } *translation = nullptr;
		UINT length = 0;
		std::string result;
		if (VerQueryValueA(data.data(), "\\VarFileInfo\\Translation", (LPVOID*)&translation, &length) && length >= sizeof(Translation)) {
			char block[96];
			char* text = nullptr;
			sprintf_s(block, "\\StringFileInfo\\%04x%04x\\ProductName", translation->language, translation->codePage);
			if (VerQueryValueA(data.data(), block, (LPVOID*)&text, &length) && text) result = text;
			sprintf_s(block, "\\StringFileInfo\\%04x%04x\\ProductVersion", translation->language, translation->codePage);
			if (VerQueryValueA(data.data(), block, (LPVOID*)&text, &length) && text) result += std::string(result.empty() ? "" : " ") + text;
		}
		VS_FIXEDFILEINFO* fixed = nullptr;
		if (VerQueryValueA(data.data(), "\\", (LPVOID*)&fixed, &length) && fixed) {
			char text[64];
			sprintf_s(text, "%s(file %u.%u.%u.%u)", result.empty() ? "" : " ", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
				HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
			result += text;
		}
		return result;
	}

	// --- live state ---
	std::mutex Lock;	// the setting changes can come from the menu and loader threads
	struct PendingChange { std::string key, before, after; double last, first; };	// first: when queued (SettingChanges.h: the batch rule)
	std::vector<PendingChange> Pending;
	constexpr size_t PendingCap = 1024;	// UNOFFICIAL 2026-11-01: a preset can change a few hundred settings in one frame (was 256)
	unsigned ChangesThisMinute = 0, ChangesSuppressed = 0, ChangesTotal = 0;
	double MinuteStart = 0.0;
	std::string LastChange = "none", LastCell = "none", LastWeather = "none";
	double StartSeconds = 0.0, InGameSeconds = 0.0;
	bool Ended = false;

	// heartbeat window
	std::vector<double> FrameMs;
	double LastFrameTime = -1.0, WindowStart = -1.0, LoadingSeconds = 0.0, WorstEver = 0.0, LoadingStart = -1.0;
	unsigned LoadingScreens = 0;
	double PeakAddressMB = 0.0;

	std::string GameClock() {
		if (!TheShaderManager) return "--:--";
		const float hour = TheShaderManager->ShaderConst.GameTime.y;
		char text[16];
		const int minutes = (int)(hour * 60.0f + 0.5f);
		sprintf_s(text, "%02d:%02d", minutes / 60 % 24, minutes % 60);
		return text;
	}

	std::string CellText(TESObjectCELL* cell) {
		char text[200];
		if (!cell) return "none";
		if (cell->IsInterior()) {
			const char* name = cell->GetEditorName();
			sprintf_s(text, "interior '%s' (%08X)", name && name[0] ? name : "?", cell->refID);
		}
		else {
			TESWorldSpace* world = cell->worldSpace;
			const char* name = world ? world->GetEditorName() : nullptr;
			if (cell->coords) sprintf_s(text, "exterior '%s' (%08X) cell %d,%d", name && name[0] ? name : "?", world ? world->refID : 0, cell->coords->x, cell->coords->y);
			else sprintf_s(text, "exterior '%s' (%08X)", name && name[0] ? name : "?", world ? world->refID : 0);
		}
		return text;
	}

	void FlushChanges(bool all) {
		const double now = Seconds();
		std::lock_guard<std::mutex> guard(Lock);
		if (now - MinuteStart > 60.0) {
			if (ChangesSuppressed) Logger::Log("SETTING %u more changes in the last minute not listed (at most %u a minute)", ChangesSuppressed, SettingChanges::PerMinute);
			ChangesSuppressed = 0; ChangesThisMinute = 0; MinuteStart = now;
		}
		std::vector<SettingChanges::Change> ready;
		for (size_t i = 0; i < Pending.size(); ) {
			PendingChange& c = Pending[i];
			if (!all && now - c.last < 0.75) { i++; continue; }	// a slider still being dragged
			if (c.before != c.after) ready.push_back({ c.key, c.before, c.after, c.first });
			Pending.erase(Pending.begin() + i);
		}
		if (ready.empty()) return;
		// UNOFFICIAL 2026-11-01 (SettingChanges.h): changes that arrived together in numbers (a preset load, a settings file reload) are
		// one compact block, never dropped; the rest one line each under the per-minute cap
		std::vector<std::vector<SettingChanges::Change>> batches;
		std::vector<SettingChanges::Change> singles;
		SettingChanges::Split(ready, batches, singles);
		for (const std::vector<SettingChanges::Change>& batch : batches) {
			ChangesTotal += (unsigned)batch.size();
			const SettingChanges::Change& c = batch.back();
			LastChange = c.key + " " + c.before + " -> " + c.after + " (in a batch of " + std::to_string(batch.size()) + ")";
			char head[96];
			sprintf_s(head, "SETTING batch (%u changes, game time %s): ", (unsigned)batch.size(), GameClock().c_str());
			LogList(head, SettingChanges::BatchItems(batch), " | ");
		}
		for (const SettingChanges::Change& c : singles) {
			ChangesTotal++;
			LastChange = c.key + " " + c.before + " -> " + c.after;
			if (ChangesThisMinute++ < SettingChanges::PerMinute) Logger::Log("SETTING %s: %s -> %s (game time %s)", c.key.c_str(), c.before.c_str(), c.after.c_str(), GameClock().c_str());
			else ChangesSuppressed++;
		}
	}

	unsigned EffectsOn() {
		if (!TheShaderManager) return 0;
		unsigned on = 0;
		for (const auto& entry : TheShaderManager->EffectsNames) if (entry.second && *entry.second && (*entry.second)->Enabled) on++;
		return on;
	}

	double AddressSpaceMB(double* totalMB = nullptr) {
		MEMORYSTATUSEX memory = { sizeof(memory) };
		if (!GlobalMemoryStatusEx(&memory)) return 0.0;
		if (totalMB) *totalMB = memory.ullTotalVirtual / 1048576.0;
		return (memory.ullTotalVirtual - memory.ullAvailVirtual) / 1048576.0;
	}

	void Heartbeat(double now) {
		FlushChanges(false);
		const double span = now - WindowStart;
		std::vector<double> ms = FrameMs;
		const FrameTimeSummary s = SummarizeFrameTimes(ms);
		const unsigned spikes = CountFrameSpikes(FrameMs, s.p50Ms);
		double totalMB = 0.0;
		const double usedMB = AddressSpaceMB(&totalMB);
		const unsigned textureMB = TheRenderManager && TheRenderManager->device ? TheRenderManager->device->GetAvailableTextureMem() / 1048576u : 0;
		const SessionLog::Counters& c = SessionLog::Count;
		Logger::Log("HEARTBEAT %.0f s: %u frames, avg %.2f ms (%.0f fps), 1%% low %.0f fps, worst %.1f ms, %u spikes | loading %.1f s | address space "
			"%.0f of %.0f MB (peak %.0f), D3D9 texture memory free ~%u MB | effects on %u | %s, game time %s | cuts %u, history resets %u, lamp slot "
			"changes %u, cached sun cascade redraws %u, lamp pool draws %u, shadow atlas re-converts %u, reflection cube restarts %u, AL grid rebuilds %u",
			span, s.frames, s.avgMs, s.avgMs > 0 ? 1000.0 / s.avgMs : 0.0, s.low1Fps, s.maxMs, spikes, LoadingSeconds, usedMB, totalMB, PeakAddressMB, textureMB,
			EffectsOn(), LastCell.c_str(), GameClock().c_str(), c.cameraCuts, c.historyResets, c.lampSlotChanges, c.cachedCascadeRedraws, c.poolDraws,
			c.atlasReconverts, c.cubeRestarts, c.gridRebuilds);
		SessionLog::Count = SessionLog::Counters();
		Diagnostics::Heartbeat();	// UNOFFICIAL 2026-11-01: the effects' drawn / skipped counts, the flash detector, shader cache, D3D failures
		{	// UNOFFICIAL 2026-11-01: a frame-generation / capture / overlay program started or stopped, or another window came in front
			const std::string environment = SessionLog::Environment();
			if (environment != LastEnvironment) { Logger::Log("ENVIRONMENT changed: %s", environment.c_str()); LastEnvironment = environment; }
		}
		FrameMs.clear();
		LoadingSeconds = 0.0;
		WindowStart = now;
	}

	// ---- CRASH lines ------------------------------------------------------------------------------------------------------------
	// 2026-11-04: armed at plugin load (ArmCrashReport); before, only once the player was in a cell. A crash logger loaded before us
	// (xNVSE loads plugins by name: CrashLogger.dll comes first) set its filter first, so ours runs first and hands on to it. One set
	// after ours is found in game and put under ours (OnFrame). Players send one file: the log.
	LPTOP_LEVEL_EXCEPTION_FILTER LoadPrevious = nullptr;	// the filter before ours at plugin load
	LPTOP_LEVEL_EXCEPTION_FILTER LatePrevious = nullptr;	// one that replaced ours later and was put under ours again
	DWORD MainThread = 0;
	volatile LONG FilterDepth = 0;

	// Whether address is just after a call instruction in an executable image: a return address. (No C++ objects here: __try.)
	bool LooksLikeReturn(UINT_PTR address) {
		if (address < 0x10000) return false;
		MEMORY_BASIC_INFORMATION info;
		if (!VirtualQuery((const void*)(address - 7), &info, sizeof(info)) || info.State != MEM_COMMIT || info.Type != MEM_IMAGE) return false;
		if (!(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
		__try {
			const unsigned char* b = (const unsigned char*)address;
			auto callModRm = [](unsigned char m) { return (m & 0x38) == 0x10; };	// FF /2: call r/m32
			if (b[-5] == 0xE8) return true;											// call rel32
			if (b[-2] == 0xFF && callModRm(b[-1]) && ((b[-1] & 0xC0) == 0xC0 || ((b[-1] & 0xC0) == 0 && (b[-1] & 7) != 4 && (b[-1] & 7) != 5))) return true;	// reg, [reg]
			if (b[-3] == 0xFF && callModRm(b[-2]) && (b[-2] & 0xC0) == 0x40 && (b[-2] & 7) != 4) return true;	// [reg+disp8]
			if (b[-3] == 0xFF && callModRm(b[-2]) && (b[-2] & 0xC7) == 0x04) return true;	// [sib]
			if (b[-4] == 0xFF && callModRm(b[-3]) && (b[-3] & 0xC7) == 0x44) return true;	// [sib+disp8]
			if (b[-6] == 0xFF && callModRm(b[-5]) && ((b[-5] & 0xC7) == 0x05 || ((b[-5] & 0xC0) == 0x80 && (b[-5] & 7) != 4))) return true;	// [disp32], [reg+disp32]
			if (b[-7] == 0xFF && callModRm(b[-6]) && (b[-6] & 0xC7) == 0x84) return true;	// [sib+disp32]
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return false;
	}

	// Return addresses on the crashing thread's stack, from ESP up (16 KB at most, inside the stack): a scan, not an unwind (release
	// code keeps no frame pointers), so a stale address can sit between real ones; the first few are nearly always the real callers.
	unsigned ScanStack(const CONTEXT* context, UINT_PTR* out, unsigned max) {
		unsigned found = 0;
		__try {
			const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
			const UINT_PTR top = (UINT_PTR)tib->StackBase, start = context->Esp;
			for (UINT_PTR p = start; p + 4 <= top && p < start + 16384 && found < max; p += 4) {
				const UINT_PTR value = *(const UINT_PTR*)p;
				if (LooksLikeReturn(value) && (!found || out[found - 1] != value)) out[found++] = value;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return found;
	}

	// "module.dll+1A2B" (the module's file name only), or the bare address outside every module
	void ModuleText(UINT_PTR address, char* out, size_t size) {
		HMODULE handle = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &handle) && handle) {
			char path[MAX_PATH] = {};
			GetModuleFileNameA(handle, path, MAX_PATH);
			const char* base = strrchr(path, '\\');
			sprintf_s(out, size, "%s+%lX", base ? base + 1 : path, (unsigned long)(address - (UINT_PTR)handle));
		}
		else sprintf_s(out, size, "%08lX (in no module)", (unsigned long)address);
	}

	void ExceptionText(const EXCEPTION_RECORD* r, char* out, size_t size) {
		switch (r->ExceptionCode) {
		case EXCEPTION_ACCESS_VIOLATION:
			if (r->NumberParameters >= 2) {
				const ULONG_PTR kind = r->ExceptionInformation[0], at = r->ExceptionInformation[1];
				sprintf_s(out, size, "access violation %s %08lX%s", kind == 0 ? "reading" : kind == 8 ? "executing" : "writing", (unsigned long)at,
					at < 0x10000 ? " (a null pointer)" : "");
			}
			else strcpy_s(out, size, "access violation");
			return;
		case 0xE06D7363: sprintf_s(out, size, "C++ exception %s, not caught", SessionLog::CppExceptionType(r)); return;
		case EXCEPTION_STACK_OVERFLOW: strcpy_s(out, size, "stack overflow (endless recursion?)"); return;
		case EXCEPTION_INT_DIVIDE_BY_ZERO: strcpy_s(out, size, "integer division by zero"); return;
		case EXCEPTION_ILLEGAL_INSTRUCTION: strcpy_s(out, size, "illegal instruction (one this processor does not have?)"); return;
		case EXCEPTION_PRIV_INSTRUCTION: strcpy_s(out, size, "privileged instruction"); return;
		case EXCEPTION_IN_PAGE_ERROR: strcpy_s(out, size, "in-page error (a file or the page file could not be read)"); return;
		case EXCEPTION_BREAKPOINT: strcpy_s(out, size, "breakpoint"); return;
		default: sprintf_s(out, size, "exception %08lX", (unsigned long)r->ExceptionCode); return;
		}
	}

	void WriteCrash(EXCEPTION_POINTERS* info) {
		const EXCEPTION_RECORD* r = info->ExceptionRecord;
		char what[200], where[MAX_PATH + 32], stage[400] = "";
		ExceptionText(r, what, sizeof(what));
		ModuleText((UINT_PTR)r->ExceptionAddress, where, sizeof(where));
		const unsigned depth = SessionLog::Stages::Depth < 8 ? SessionLog::Stages::Depth : 8;
		for (unsigned i = 0; i < depth; i++) {
			const char* s = SessionLog::Stages::Stack[i];
			if (i) strcat_s(stage, " > ");
			strncat_s(stage, s ? s : "?", _TRUNCATE);
		}
		const DWORD thread = GetCurrentThreadId();
		Logger::LogUnbuffered("CRASH: %s at %s, on the %s thread (%lu) after %s | stage: %s | last cell %s | last setting change %s | address space %.0f MB | frame time worst %.1f ms",
			what, where, thread == MainThread ? "main" : "a worker", (unsigned long)thread, Uptime(Seconds() - StartSeconds).c_str(),
			depth ? stage : "(none marked)", LastCell.c_str(), LastChange.c_str(), AddressSpaceMB(), WorstEver);
		if (const CONTEXT* c = info->ContextRecord) {
			Logger::LogUnbuffered("CRASH registers: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX EIP=%08lX",
				c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);
			UINT_PTR returns[20];
			const unsigned count = ScanStack(c, returns, 20);
			char line[2048] = "";
			for (unsigned i = 0; i < count; i++) {
				char entry[MAX_PATH + 32];
				ModuleText(returns[i], entry, sizeof(entry));
				if (i) strcat_s(line, ", ");
				strncat_s(line, entry, _TRUNCATE);
			}
			Logger::LogUnbuffered("CRASH call stack (callers, newest first; a stack scan, so an old entry can appear): %s", count ? line : "(none found)");
		}
		Logger::LogUnbuffered("CRASH: please send this whole log file (Fallout New Vegas\\VegasShaders-Logs) with the report.");
	}

	LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
		// A filter that replaced ours and then calls the one it replaced comes back here: on to the load-time one, no second report
		if (InterlockedIncrement(&FilterDepth) > 1) {
			const LONG result = LoadPrevious ? LoadPrevious(info) : EXCEPTION_CONTINUE_SEARCH;
			InterlockedDecrement(&FilterDepth);
			return result;
		}
		static volatile LONG entered = 0;
		if (info && info->ExceptionRecord && InterlockedExchange(&entered, 1) == 0) WriteCrash(info);
		const LPTOP_LEVEL_EXCEPTION_FILTER next = LatePrevious ? LatePrevious : LoadPrevious;
		const LONG result = next ? next(info) : EXCEPTION_CONTINUE_SEARCH;
		InterlockedDecrement(&FilterDepth);
		return result;
	}

	// The game INI values the header lists (as the game read them, after NVR's own adjustments in ReadSettingHook)
	const char* const IniNames[] = { "iSize W:Display", "iSize H:Display", "bFull Screen:Display", "bFull Screen", "iMultiSample:Display",
		"iPresentInterval:Display", "uGridsToLoad:General", "uExterior Cell Buffer:General", "iMaxAnisotropy:Display", "iShadowMapResolution:Display",
		"iTexMipMapSkip:Display", "b30GrassVS:Grass", "bUseThreadedAI:General", "iNumHWThreads:General", "bUseWaterReflections:Water",
		"iWaterReflectHeight:Water", "fLODFadeOutMultObjects:LOD", "uInterior Cell Buffer:General", "bAllowScreenShot:Display" };
	std::string IniValues[sizeof(IniNames) / sizeof(IniNames[0])];
}

namespace {
	// UNOFFICIAL 2026-11-01: the journal's last events, always (FrameJournal::Dump; core/Diagnostics.cpp)
	struct JournalEvent { unsigned frame; double time; char text[200]; };
	JournalEvent JournalRing[256];
	unsigned JournalNext = 0;
	std::mutex JournalLock;

	// The last frames' times for the REPORT block
	float FrameRing[1024];
	unsigned FrameRingNext = 0;
}

void FrameJournal::Note(const char* format, ...) {
	char text[600];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	{
		std::lock_guard<std::mutex> guard(JournalLock);
		JournalEvent& e = JournalRing[JournalNext++ % (sizeof(JournalRing) / sizeof(JournalRing[0]))];
		e.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		e.time = Seconds();
		strncpy_s(e.text, text, _TRUNCATE);
	}
	if (!On || Lines >= Cap) return;
	if (++Lines == Cap) Logger::Log("FJ %s | the frame journal reached its %u lines: no more this session", text, Cap);
	else Logger::Log("FJ %s", text);
}

unsigned FrameJournal::Dump(const char* head, unsigned first, unsigned last, double maxAgeSeconds) {
	const unsigned size = sizeof(JournalRing) / sizeof(JournalRing[0]);
	std::vector<JournalEvent> events;
	{
		std::lock_guard<std::mutex> guard(JournalLock);
		const unsigned count = JournalNext < size ? JournalNext : size;
		for (unsigned i = JournalNext - count; i != JournalNext; i++) events.push_back(JournalRing[i % size]);
	}
	const double now = Seconds();
	unsigned written = 0;
	for (const JournalEvent& e : events) {
		if (e.frame < first || e.frame > last) continue;
		if (maxAgeSeconds > 0.0 && now - e.time > maxAgeSeconds) continue;
		Logger::Log("%s f%u (%.2f s ago): %s", head, e.frame, now - e.time, e.text);
		written++;
	}
	if (!written) Logger::Log("%s (no frame journal events in these frames)", head);
	return written;
}

unsigned FrameJournal::CountFrame(unsigned frame, const char* prefix) {
	const unsigned size = sizeof(JournalRing) / sizeof(JournalRing[0]);
	const size_t length = prefix ? strlen(prefix) : 0;
	std::lock_guard<std::mutex> guard(JournalLock);
	const unsigned count = JournalNext < size ? JournalNext : size;
	unsigned found = 0;
	for (unsigned i = JournalNext - count; i != JournalNext; i++) {
		const JournalEvent& e = JournalRing[i % size];
		if (e.frame == frame && (!length || !strncmp(e.text, prefix, length))) found++;
	}
	return found;
}

std::string SessionLog::Where() { return LastCell; }
std::string SessionLog::Weather() { return LastWeather; }
std::string SessionLog::Clock() { return GameClock(); }
void SessionLog::RecentFrameTimes(std::vector<float>& out, unsigned count) {
	const unsigned size = sizeof(FrameRing) / sizeof(FrameRing[0]);
	if (count > size) count = size;
	if (count > FrameRingNext) count = FrameRingNext;
	out.clear();
	for (unsigned i = FrameRingNext - count; i != FrameRingNext; i++) out.push_back(FrameRing[i % size]);
}

void SessionLog::HistoryReset(const char* effect, const char* why) {
	Count.historyResets++;
	EffectStatus::History(EffectStatus::Register(effect), why);	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h)
	FRAME_JOURNAL("history reset: %s (%s)", effect, why);
}

void SessionLog::OnPluginLoad(unsigned nvseVersion, unsigned gameVersion, unsigned pluginVersion) {
	StartSeconds = Seconds();
	MinuteStart = StartSeconds;
	Logger::Log("SESSION build: VegasShaders %u, commit %s, compiled %s %s (%s)", pluginVersion, NVR_BUILD_COMMIT, __DATE__, __TIME__,
#if NVR_TEST_TOOLS
		"test tools in"
#else
		"release"
#endif
	);
	// 2026-11-04 (two testers' "fatal error ... while loading plugin", the log stopped after its first line): the C++ runtime in use.
	// Builds made with VS 2022 17.10+ crash inside an MSVCP140.dll older than 14.40 at the first std::mutex lock (the logger takes
	// one after every line); _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR (NewVegasReloaded.vcxproj) builds the mutexes the old way, so any
	// copy works now. An old copy in the game folder (left by some other mod) wins over Windows' own: this line says which one is in.
	{
		char path[MAX_PATH] = {};
		const HMODULE runtime = GetModuleHandleA("msvcp140.dll");
		if (runtime && GetModuleFileNameA(runtime, path, MAX_PATH)) {
			char version[64] = "?";
			DWORD handle = 0;
			const DWORD size = GetFileVersionInfoSizeA(path, &handle);
			std::vector<char> block(size ? size : 1);
			VS_FIXEDFILEINFO* info = nullptr;
			UINT length = 0;
			if (size && GetFileVersionInfoA(path, handle, size, block.data()) && VerQueryValueA(block.data(), "\\", (void**)&info, &length) && info)
				sprintf_s(version, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
			// (a 32-bit game reads SysWOW64's copy, but Windows names it by the System32 path: either counts as Windows' own)
			char system32[MAX_PATH] = {}, wow64[MAX_PATH] = {};
			GetSystemDirectoryA(system32, MAX_PATH);
			GetSystemWow64DirectoryA(wow64, MAX_PATH);
			auto under = [&](const char* folder) { return folder[0] && _strnicmp(path, folder, strlen(folder)) == 0; };
			const bool fromSystem = under(system32) || under(wow64);
			Logger::Log("SESSION C++ runtime: MSVCP140 %s, %s%s", version, fromSystem ? "Windows' own copy" : "a copy outside Windows: ",
				fromSystem ? "" : path);	// (the path passes the privacy scrubber: <GAME>\msvcp140.dll)
		}
	}
	// both packed as major << 24 | minor << 16 | build << 4 | type (the game's 1.4.0.525 = 040020D1)
	Logger::Log("SESSION xNVSE %u.%u.%u (%08X), game 1.%u.%u.%u (%08X)", nvseVersion >> 24, (nvseVersion >> 16) & 0xFF, (nvseVersion >> 4) & 0xFFF,
		nvseVersion, gameVersion >> 24, (gameVersion >> 16) & 0xFF, (gameVersion >> 4) & 0xFFF, gameVersion);

	// Windows: the build from RtlGetVersion (GetVersionEx lies to an exe without a manifest), the edition and release from the registry; Wine
	char windows[256] = "?";
	typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOEXW*);
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	OSVERSIONINFOEXW version = { sizeof(version) };
	RtlGetVersionFn getVersion = ntdll ? (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
	const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	DWORD ubr = 0, size = sizeof(ubr);
	RegGetValueA(HKEY_LOCAL_MACHINE, key, "UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
	std::string release = RegString(HKEY_LOCAL_MACHINE, key, "DisplayVersion");
	if (release.empty()) release = RegString(HKEY_LOCAL_MACHINE, key, "ReleaseId");
	if (getVersion && getVersion(&version) == 0)
		sprintf_s(windows, "%s %s, build %lu.%lu.%lu.%lu", RegString(HKEY_LOCAL_MACHINE, key, "ProductName").c_str(), release.c_str(),
			version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber, ubr);
	typedef const char* (__cdecl* WineVersionFn)();
	WineVersionFn wine = ntdll ? (WineVersionFn)GetProcAddress(ntdll, "wine_get_version") : nullptr;
	BOOL wow64 = FALSE;
	IsWow64Process(GetCurrentProcess(), &wow64);
	Logger::Log("SESSION Windows: %s%s%s%s", windows, wow64 ? ", 64-bit" : ", 32-bit", wine ? ", Wine / Proton " : "", wine ? wine() : "");

	// CPU: the brand string, cores and threads
	int regs[4] = {};
	char brand[49] = {};
	__cpuid(regs, 0x80000000);
	if ((unsigned)regs[0] >= 0x80000004u) {
		for (int i = 0; i < 3; i++) { __cpuid(regs, 0x80000002 + i); memcpy(brand + 16 * i, regs, 16); }
	}
	const char* cpu = brand;
	while (*cpu == ' ') cpu++;
	unsigned cores = 0;
	DWORD bytes = 0;
	GetLogicalProcessorInformation(nullptr, &bytes);
	std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
	if (bytes && GetLogicalProcessorInformation(info.data(), &bytes))
		for (DWORD i = 0; i < bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++) if (info[i].Relationship == RelationProcessorCore) cores++;
	SYSTEM_INFO system = {};
	GetNativeSystemInfo(&system);
	MEMORYSTATUSEX memory = { sizeof(memory) };
	GlobalMemoryStatusEx(&memory);
	Logger::Log("SESSION CPU: %s, %u cores, %lu threads | RAM %.1f GB (%.1f GB free) | address space %.0f MB (%s)", cpu[0] ? cpu : "?", cores,
		system.dwNumberOfProcessors, memory.ullTotalPhys / 1073741824.0, memory.ullAvailPhys / 1073741824.0, memory.ullTotalVirtual / 1048576.0,
		memory.ullTotalVirtual > 3ull * 1073741824ull ? "4 GB patch / large address aware" : "NOT large address aware: 2 GB, expect out-of-memory crashes");
}

void SessionLog::ArmCrashReport() {
	MainThread = GetCurrentThreadId();
	LoadPrevious = SetUnhandledExceptionFilter(CrashFilter);
	Logger::Log("SESSION crash report armed%s", LoadPrevious ? " (a crash handler set before ours, e.g. a crash logger's, still runs after it)" : "");
}

// A crash handler set after ours (a plugin loaded after this one, the game) is put under ours: ours on top again, handing on to it.
// 2026-11-04: at xNVSE's PostLoad too (the bench's old-runtime run: another plugin crashed while the game read its INIs, after
// PostLoad and before any cell, under the handler of a plugin loaded after ours, which then crashed in its own handler: no line).
void SessionLog::ReassertCrashReport(const char* when) {
	const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(CrashFilter);
	if (current == CrashFilter) return;
	if (!LatePrevious || current != LatePrevious) LatePrevious = current;
	Logger::Log("SESSION crash report: a crash handler was set after ours (%s); ours is on top again and hands crashes on to it", when);
}

// The decorated type name from the compiler's throw information (32-bit: plain pointers). No C++ objects here: __try.
const char* SessionLog::CppExceptionType(const _EXCEPTION_RECORD* record) {
	__try {
		if (!record || record->NumberParameters < 3 || record->ExceptionInformation[0] != 0x19930520) return "?";
		const DWORD* throwInfo = (const DWORD*)record->ExceptionInformation[2];
		if (!throwInfo) return "?";
		const DWORD* catchables = (const DWORD*)throwInfo[3];	// pCatchableTypeArray: count, then CatchableType*
		if (!catchables || !catchables[0]) return "?";
		const DWORD* catchable = (const DWORD*)catchables[1];	// the thrown type itself
		const char* typeDescriptor = catchable ? (const char*)catchable[1] : nullptr;	// pType
		return typeDescriptor ? typeDescriptor + 8 : "?";	// after the vftable and spare pointers: the decorated name
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return "?";
	}
}

void SessionLog::OnSettingsLoaded() {
	LogSettingsDiff("SESSION settings");
}

// UNOFFICIAL 2026-11-01: the live values (Config.TomlConfig: a change made in the menu, a preset loaded or a settings file reloaded
// after the start is in it), so the REPORT block's call lists what the tester has NOW, not what the SESSION header saw at the start
void SessionLog::LogSettingsDiff(const char* head) {
	unsigned differ = 0, total = 0;
	const std::vector<std::string> items = SettingsDiff(differ, total);
	if (!total) return;
	Logger::Log("%s: %u of %u differ from this build's defaults%s (key=yours (default)):", head, differ, total,
		TheSettingManager->Config.MaxQualityActive ? "; Max Quality is ON (its values are used in place of these)" : "");
	LogList((std::string(head) + ": ").c_str(), items, " | ");
}

// 2026-11-03: the walk LogSettingsDiff logged, as data (the bug report, InstallCheck.h, lists the same)
std::vector<std::string> SessionLog::SettingsDiff(unsigned& differ, unsigned& total) {
	typedef SettingManager::Configuration Config;
	std::vector<std::string> items;
	differ = total = 0;
	if (!TheSettingManager) return items;
	Config& config = TheSettingManager->Config;
	if (!config.DefaultConfig.is_table()) return items;
	auto number = [](const tomlValue& v, double& out) {
		if (v.is_floating()) { out = v.as_floating(); return true; }
		if (v.is_integer()) { out = (double)v.as_integer(); return true; }
		return false;
	};
	auto text = [](const tomlValue& v) -> std::string {
		char t[64];
		if (v.is_boolean()) return v.as_boolean() ? "true" : "false";
		if (v.is_integer()) { sprintf_s(t, "%lld", (long long)v.as_integer()); return t; }
		if (v.is_floating()) { sprintf_s(t, "%g", (double)(float)v.as_floating()); return t; }
		if (v.is_string()) { std::string s = v.as_string(); if (s.size() > 60) s = s.substr(0, 57) + "..."; return "\"" + s + "\""; }
		return "?";
	};
	auto walk = [&](auto& self, const tomlValue& defaults, const tomlValue* user, const std::string& path) -> void {
		std::vector<std::string> here;
		for (const auto& [key, value] : defaults.as_table()) {
			const tomlValue* mine = user && user->is_table() && user->contains(key) ? &user->at(key) : nullptr;
			if (value.is_table()) { self(self, value, mine, path.empty() ? (key[0] == '_' ? key.substr(1) : key) : path + "." + key); continue; }
			total++;
			if (!mine) continue;
			double a, b;
			bool same;
			if (number(value, a) && number(*mine, b)) same = fabs(a - b) <= 1e-5 * (std::max)(1.0, fabs(a));
			else if (value.is_boolean() && mine->is_boolean()) same = value.as_boolean() == mine->as_boolean();
			else if (value.is_string() && mine->is_string()) same = value.as_string().str == mine->as_string().str;	// (the text only: toml11's == also compares "basic" vs 'literal' quoting)
			else same = false;
			if (same) continue;
			differ++;
			here.push_back(key + "=" + text(*mine) + " (" + text(value) + ")");
		}
		if (!here.empty() && items.size() < 600) {
			std::string group = "[" + path + "] ";
			for (size_t i = 0; i < here.size(); i++) group += (i ? ", " : "") + here[i];
			items.push_back(group);
		}
	};
	walk(walk, config.DefaultConfig, &config.TomlConfig, "");
	return items;
}

// UNOFFICIAL 2026-11-01 the environment (SessionLog.h): programs that take the game's frames or draw over them. A tester's 4K smear
// video came from a Lossless Scaling capture; whether such a program runs is a fact a log should carry.
std::string SessionLog::Environment() {
	struct Known { const char* exe; const char* what; };
	static const Known programs[] = {
		{ "losslessscaling.exe", "Lossless Scaling (frame generation / scaling)" }, { "rtss.exe", "RivaTuner Statistics Server (overlay, frame limit)" },
		{ "msiafterburner.exe", "MSI Afterburner" }, { "obs64.exe", "OBS Studio" }, { "obs32.exe", "OBS Studio (32-bit)" }, { "nvidia overlay.exe", "NVIDIA app overlay" },
		{ "nvidia share.exe", "NVIDIA ShadowPlay" }, { "magpie.exe", "Magpie (scaling)" }, { "skif.exe", "Special K" }, { "specialk64.exe", "Special K" },
		{ "parsecd.exe", "Parsec (streaming)" }, { "medal.exe", "Medal (capture)" }, { "overwolf.exe", "Overwolf (overlay)" }, { "streamlabs obs.exe", "Streamlabs" },
		{ "gamebar.exe", "Xbox Game Bar" }, { "xboxgamebar.exe", "Xbox Game Bar" }, { "radeonsoftware.exe", "AMD Software overlay" }, { "frameview.exe", "NVIDIA FrameView" },
		{ "capframex.exe", "CapFrameX" }, { "presentmon.exe", "PresentMon" }, { "sunshine.exe", "Sunshine (streaming)" }, { "moonlight.exe", "Moonlight (streaming)" } };
	static const char* hooks[] = { "GameOverlayRenderer.dll", "DiscordHook.dll", "RTSSHooks.dll", "nvspcap.dll", "SpecialK32.dll", "graphics-hook32.dll", "ReShade32.dll",
		"d3d9_ow.dll", "OWClient.dll", "XInput1_3.dll" };
	std::vector<std::string> found;
	struct Running { DWORD pid; const Known* k; };
	std::vector<Running> running;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
			char name[MAX_PATH] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szExeFile, -1, name, sizeof(name), nullptr, nullptr);
			for (char& c : name) c = (char)tolower((unsigned char)c);
			for (const Known& k : programs) if (!strcmp(name, k.exe)) { running.push_back({ entry.th32ProcessID, &k }); break; }
		}
		CloseHandle(snapshot);
	}
	// Their windows over the game window (a frame-generation program shows its own window on top of the game's)
	RECT game = {};
	if (GameWindow) GetWindowRect(GameWindow, &game);
	struct Over { std::vector<Running>* running; const RECT* game; std::vector<std::string> notes; };
	Over over = { &running, &game, {} };
	if (GameWindow) EnumWindows([](HWND w, LPARAM p) -> BOOL {
		Over* o = (Over*)p;
		if (w == GameWindow || !IsWindowVisible(w)) return TRUE;
		DWORD pid = 0;
		GetWindowThreadProcessId(w, &pid);
		for (const Running& r : *o->running) {
			if (r.pid != pid) continue;
			RECT rect = {}, cut = {};
			if (GetWindowRect(w, &rect) && IntersectRect(&cut, &rect, o->game) && (cut.right - cut.left) * (cut.bottom - cut.top) > 0) {
				char t[160];
				sprintf_s(t, "%s has a %ldx%ld window over the game window%s", r.k->exe, rect.right - rect.left, rect.bottom - rect.top,
					(GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOPMOST) ? " (topmost)" : "");
				o->notes.push_back(t);
			}
			break;
		}
		return TRUE;
	}, (LPARAM)&over);
	for (const Running& r : running) found.push_back(std::string(r.k->exe) + " = " + r.k->what);
	std::string text = found.empty() ? "no frame generation / capture / overlay program from the known list is running" : "running: ";
	for (size_t i = 0; i < found.size(); i++) text += (i ? ", " : "") + found[i];
	for (const std::string& n : over.notes) text += " | " + n;
	std::vector<std::string> inProcess;
	for (const char* h : hooks) if (GetModuleHandleA(h)) inProcess.push_back(h);
	if (!inProcess.empty()) { text += " | hook DLLs in the game: "; for (size_t i = 0; i < inProcess.size(); i++) text += (i ? ", " : "") + inProcess[i]; }
	// The foreground window and the game window
	HWND fore = GetForegroundWindow();
	DWORD forePid = 0;
	if (fore) GetWindowThreadProcessId(fore, &forePid);
	const char* foreText = !fore ? "no foreground window" : fore == GameWindow || forePid == GetCurrentProcessId() ? "the game is the foreground window" : "ANOTHER program is the foreground window";
	for (const Running& r : running) if (fore && r.pid == forePid) { foreText = r.k->exe; break; }
	text += std::string(" | ") + foreText;
	if (GameWindow) {
		char t[160];
		MONITORINFO monitor = { sizeof(monitor) };
		GetMonitorInfoA(MonitorFromWindow(GameWindow, MONITOR_DEFAULTTONEAREST), &monitor);
		sprintf_s(t, " | game window %ldx%ld at %ld,%ld on a %ldx%ld monitor%s%s", game.right - game.left, game.bottom - game.top, game.left, game.top,
			monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, IsIconic(GameWindow) ? ", minimised" : "",
			IsWindowVisible(GameWindow) ? "" : ", hidden");
		text += t;
	}
	return text;
}

void SessionLog::OnDevice(IDirect3DDevice9* device) {
	// The GPUs Windows knows (the display class in the registry: name, driver, VRAM); DXVK may show the game another name
	const char* displayClass = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
	std::vector<std::string> gpus;
	for (int i = 0; i < 16; i++) {
		char path[160];
		sprintf_s(path, "%s\\%04d", displayClass, i);
		const std::string name = RegString(HKEY_LOCAL_MACHINE, path, "DriverDesc");
		if (name.empty()) continue;
		std::string driver = RegString(HKEY_LOCAL_MACHINE, path, "DriverVersion");
		// NVIDIA's own number from the Windows one: 32.0.15.6094 -> 560.94 (the last five digits of the last two parts)
		const std::string provider = RegString(HKEY_LOCAL_MACHINE, path, "ProviderName");
		if (provider.find("NVIDIA") != std::string::npos) {
			std::string digits;
			const size_t dot = driver.find('.', driver.find('.') + 1);
			for (size_t k = dot == std::string::npos ? 0 : dot; k < driver.size(); k++) if (isdigit((unsigned char)driver[k])) digits += driver[k];
			if (digits.size() >= 5) driver += " = NVIDIA " + digits.substr(digits.size() - 5, 3) + "." + digits.substr(digits.size() - 2);
		}
		unsigned long long vram = 0;
		DWORD size = sizeof(vram);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "HardwareInformation.qwMemorySize", RRF_RT_QWORD, nullptr, &vram, &size) != ERROR_SUCCESS) {
			DWORD vram32 = 0;
			size = sizeof(vram32);
			if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "HardwareInformation.MemorySize", RRF_RT_DWORD | RRF_RT_REG_BINARY, nullptr, &vram32, &size) == ERROR_SUCCESS) vram = vram32;
		}
		char item[400];
		sprintf_s(item, "%s (driver %s%s%s, VRAM %.1f GB)", name.c_str(), driver.empty() ? "?" : driver.c_str(), provider.empty() ? "" : ", ",
			provider.c_str(), vram / 1073741824.0);
		gpus.push_back(item);
	}
	LogList("SESSION GPU (Windows): ", gpus, " | ");
	IDirect3D9* d3d = nullptr;
	D3DADAPTER_IDENTIFIER9 adapter = {};
	D3DDISPLAYMODE desktop = {};
	if (device && SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
		d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter);
		d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &desktop);
		d3d->Release();
	}
	Logger::Log("SESSION GPU (as the game sees it): %s, vendor %04lX device %04lX, D3D9 driver %u.%u.%u.%u", adapter.Description, adapter.VendorId, adapter.DeviceId,
		HIWORD(adapter.DriverVersion.HighPart), LOWORD(adapter.DriverVersion.HighPart), HIWORD(adapter.DriverVersion.LowPart), LOWORD(adapter.DriverVersion.LowPart));
	const std::string d3d9 = FileVersion("d3d9.dll");
	const char* runtime = TheRenderManager->D3D9RuntimeDescription();
	Logger::Log("SESSION D3D9 runtime: %.*s, d3d9.dll %s | Vegas Shaders DXVK extension: %s", (int)(strchr(runtime, '(') ? strchr(runtime, '(') - runtime : strlen(runtime)), runtime,
		d3d9.empty() ? "(no version info)" : d3d9.c_str(), DxvkCompute::Available() ? DxvkCompute::Status() : "not there");
	// 2026-11-03: the same facts for the install check and the bug report (InstallCheck.h)
	std::string gpuWindows;
	for (const std::string& g : gpus) gpuWindows += (gpuWindows.empty() ? "" : " | ") + g;
	char gpuGame[320];
	sprintf_s(gpuGame, "%s, vendor %04lX device %04lX", adapter.Description, adapter.VendorId, adapter.DeviceId);
	InstallCheck::OnDevice(gpuWindows, gpuGame, !strncmp(runtime, "DXVK", 4), d3d9, DxvkCompute::Available() ? DxvkCompute::Status() : "");
	const char* wrappers[] = { "dxgi.dll", "d3d11.dll", "dinput8.dll", "ReShade32.dll", "dxvk_d3d9.dll" };
	std::vector<std::string> found;
	for (const char* w : wrappers) if (GetModuleHandleA(w)) { const std::string v = FileVersion(w); found.push_back(std::string(w) + (v.empty() ? "" : " " + v)); }
	if (!found.empty()) LogList("SESSION also loaded: ", found);

	IDirect3DSwapChain9* swap = nullptr;
	D3DPRESENT_PARAMETERS present = {};
	if (device && SUCCEEDED(device->GetSwapChain(0, &swap)) && swap) { swap->GetPresentParameters(&present); swap->Release(); }
	GameWindow = present.hDeviceWindow;	// UNOFFICIAL 2026-11-01 (Environment)
	const char* mode = present.Windowed ? "windowed" : "exclusive fullscreen";
	if (present.Windowed && present.hDeviceWindow) {
		const LONG style = GetWindowLongA(present.hDeviceWindow, GWL_STYLE);
		MONITORINFO monitor = { sizeof(monitor) };
		RECT window = {};
		GetWindowRect(present.hDeviceWindow, &window);
		if (!(style & WS_CAPTION) && GetMonitorInfoA(MonitorFromWindow(present.hDeviceWindow, MONITOR_DEFAULTTONEAREST), &monitor) &&
			EqualRect(&window, &monitor.rcMonitor)) mode = "borderless fullscreen window";
	}
	Logger::Log("SESSION screen: %ux%u %s, back buffer format %u, MSAA %u, present interval %s, refresh %u Hz (desktop %ux%u at %u Hz)",
		present.BackBufferWidth, present.BackBufferHeight, mode, (unsigned)present.BackBufferFormat, (unsigned)present.MultiSampleType,
		present.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? "immediate (no vsync)" : present.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "1 (vsync)" : "other",
		present.FullScreen_RefreshRateInHz ? present.FullScreen_RefreshRateInHz : desktop.RefreshRate, desktop.Width, desktop.Height, desktop.RefreshRate);
	SettingManager* s = TheSettingManager;
	Logger::Log("SESSION anti-aliasing / upscaling: TAA %s, FSR %s (version %d, RenderScale %.2f), DLSS %s (RenderScale %.2f, Ray Reconstruction %s), SMAA %s",
		s->GetSettingI("Shaders.TAA.Status", "Enabled") ? "on" : "off", s->GetSettingI("Shaders.TAA.FSR", "Enabled") ? "on" : "off",
		s->GetSettingI("Shaders.TAA.FSR", "Version"), s->GetSettingF("Shaders.TAA.FSR", "RenderScale"), s->GetSettingI("Shaders.TAA.DLSS", "Enabled") ? "on" : "off",
		s->GetSettingF("Shaders.TAA.DLSS", "RenderScale"), s->GetSettingI("Shaders.TAA.DLSS", "RayReconstruction") ? "on" : "off",
		s->GetSettingI("Shaders.SMAA.Status", "Enabled") ? "on" : "off");
}

void SessionLog::NoteIni(const GameSetting* setting) {
	if (!setting || !setting->Name) return;
	for (size_t i = 0; i < sizeof(IniNames) / sizeof(IniNames[0]); i++) {
		if (_stricmp(setting->Name, IniNames[i])) continue;
		char text[64];
		const char type = (char)tolower((unsigned char)setting->Name[0]);
		if (type == 'f') sprintf_s(text, "%g", setting->fValue);
		else if (type == 'b') strcpy_s(text, setting->iValue ? "1" : "0");
		else if (type == 'u') sprintf_s(text, "%u", (unsigned)setting->iValue);
		else if (type == 's') strcpy_s(text, "(text)");
		else sprintf_s(text, "%d", setting->iValue);
		IniValues[i] = text;
		return;
	}
}

void SessionLog::OnDeferredInit() {
	StageScope stage("the session header at DeferredInit");
	std::vector<std::string> ini;
	for (size_t i = 0; i < sizeof(IniNames) / sizeof(IniNames[0]); i++)
		if (!IniValues[i].empty()) { std::string name = IniNames[i]; ini.push_back(name.substr(0, name.find(':')) + "=" + IniValues[i]); }
	LogList("SESSION game INI (as read): ", ini);

	// The load order: count, a hash that tells two players' orders apart at a glance, and the names (not personal; one or two lines)
	if (DataHandler) {
		const ModList& mods = DataHandler->modList;
		const unsigned count = (std::min)((unsigned)mods.loadedModCount, 255u);
		unsigned hash = 2166136261u;
		std::vector<std::string> names;
		for (unsigned i = 0; i < count; i++) {
			const ModInfo* mod = mods.loadedMods[i];
			if (!mod) continue;
			char name[0x105] = {};
			memcpy(name, mod->name, 0x104);
			for (const char* c = name; *c; c++) { hash ^= (unsigned char)tolower((unsigned char)*c); hash *= 16777619u; }
			hash ^= '\n'; hash *= 16777619u;
			names.push_back(name);
		}
		Logger::Log("SESSION load order: %u plugins, hash %08X", (unsigned)names.size(), hash);
		InstallCheck::OnLoadOrder(names, hash);	// 2026-11-03: weather / lighting mods (InstallCheck.h)
		LogList("SESSION load order: ", names);
	}

	// The xNVSE plugin DLLs and anything else loaded from the game folder (wrappers, injectors): names only
	std::vector<std::string> plugins, gameFolder;
	char exe[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	std::string root = exe;
	root = root.substr(0, root.find_last_of("\\/") + 1);
	for (char& c : root) c = (char)tolower((unsigned char)c);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
			char path[MAX_PATH * 2] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szExePath, -1, path, sizeof(path), nullptr, nullptr);
			char name[MAX_PATH] = {};
			WideCharToMultiByte(CP_ACP, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
			std::string lower = path;
			for (char& c : lower) c = (char)tolower((unsigned char)c);
			if (lower.find("\\nvse\\plugins\\") != std::string::npos) plugins.push_back(name);
			else if (!root.empty() && lower.rfind(root, 0) == 0 && lower.find('\\', root.size()) == std::string::npos && lower.size() > 4 &&
				lower.compare(lower.size() - 4, 4, ".exe")) gameFolder.push_back(name);
		}
		CloseHandle(snapshot);
	}
	Logger::Log("SESSION xNVSE plugins: %u loaded", (unsigned)plugins.size());
	LogList("SESSION xNVSE plugins: ", plugins);
	LogList("SESSION DLLs from the game folder: ", gameFolder);
	LastEnvironment = SessionLog::Environment();	// UNOFFICIAL 2026-11-01
	Logger::Log("SESSION environment: %s", LastEnvironment.c_str());
}

void SessionLog::OnSettingChanged(const char* section, const char* key, const char* before, const char* after) {
	if (!section || !key || !before || !after) return;
	const std::string name = std::string(section) + "." + key;
	const double now = Seconds();
	std::lock_guard<std::mutex> guard(Lock);
	for (PendingChange& c : Pending)
		if (c.key == name) { c.after = after; c.last = now; return; }
	if (!strcmp(before, after)) return;
	if (Pending.size() < PendingCap) Pending.push_back({ name, before, after, now, now });
	else ChangesSuppressed++;
}

void SessionLog::OnEvent(const char* what) {
	FlushChanges(true);
	Logger::Log("GAME %s (game time %s, %s)", what, GameClock().c_str(), LastCell.c_str());
}

void SessionLog::OnExit(const char* how) {
	if (Ended) return;
	Ended = true;
	SessionLog::StallWatch::Stop();	// (2026-11-07 perf-ptcost: the exit's own long frames are no stall)
	FlushChanges(true);
	Logger::Log("SESSION END (%s): up %s, %s in game, %u loading screens, %u setting changes (last: %s), last cell %s, weather %s, address space peak %.0f MB, worst frame %.1f ms",
		how, Uptime(Seconds() - StartSeconds).c_str(), Uptime(InGameSeconds).c_str(), LoadingScreens, ChangesTotal, LastChange.c_str(), LastCell.c_str(),
		LastWeather.c_str(), PeakAddressMB, WorstEver);
}

void SessionLog::OnFrame() {
	const double now = Seconds();
	const double frameMs = LastFrameTime >= 0.0 ? (now - LastFrameTime) * 1000.0 : 0.0;
	LastFrameTime = now;
	static double lastSecond = -10.0;
	if (now - lastSecond >= 1.0) {	// once a second: the journal switch, pending setting changes, the address space peak
		lastSecond = now;
		const bool on = TheSettingManager->GetSettingI("Main.Develop.Main", "FrameJournal") != 0;
		if (on != FrameJournal::On) Logger::Log("FJ frame journal %s (%u of %u lines used)", on ? "ON" : "off", FrameJournal::Lines, FrameJournal::Cap);
		FrameJournal::On = on;
		// 2026-11-04 [Main.Develop.Main] CrashTest: a crash on purpose, to check the CRASH lines (inside the effects stage)
		if (const int crash = TheSettingManager->GetSettingI("Main.Develop.Main", "CrashTest")) {
			Logger::Log("SESSION CrashTest %d: crashing the game on purpose now", crash);
			if (crash == 2) throw std::runtime_error("CrashTest");
			*(volatile int*)nullptr = 0;
		}
		// UNOFFICIAL 2026-11-07 (perf-ptcost) [Main.Develop.Main] StallTest: a freeze on purpose (once per value), to check the STALL lines
		{
			static int lastStallTest = 0;
			const int stall = (std::min)((std::max)(TheSettingManager->GetSettingI("Main.Develop.Main", "StallTest"), 0), 120);
			if (stall > 0 && stall != lastStallTest) {
				Logger::Log("SESSION StallTest %d: the render thread sleeps %d s on purpose now (the STALL lines should follow)", stall, stall);
				SessionLog::StageScope stage("StallTest");
				Sleep((DWORD)stall * 1000u);
			}
			lastStallTest = stall;
		}
		FlushChanges(false);
		PeakAddressMB = (std::max)(PeakAddressMB, AddressSpaceMB());
		if (Player && Player->parentCell) {	// in game, once more: a crash handler set since PostLoad goes under ours
			static bool checked = false;
			if (!checked) { checked = true; SessionLog::ReassertCrashReport("in game"); }
		}
	}

	// Loading screens
	const bool loading = InterfaceManager && InterfaceManager->IsActive(Menu::kMenuType_Loading);
	if (loading && LoadingStart < 0.0) { LoadingStart = now; LoadingScreens++; FlushChanges(true); Logger::Log("LOADING screen"); }
	else if (!loading && LoadingStart >= 0.0) {
		LoadingSeconds += now - LoadingStart;
		Logger::Log("LOADING done after %.1f s", now - LoadingStart);
		LoadingStart = -1.0;
	}

	TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
	if (!cell) return;

	// Cell and worldspace changes (exterior cells: only when the worldspace changes or from an interior; the heartbeat says where)
	static TESObjectCELL* lastCell = nullptr;
	static TESWorldSpace* lastWorld = nullptr;
	static bool lastInterior = false;
	if (cell != lastCell) {
		const bool interior = cell->IsInterior();
		TESWorldSpace* world = interior ? nullptr : cell->worldSpace;
		const bool report = interior || lastInterior || world != lastWorld || !lastCell;
		LastCell = CellText(cell);
		if (report) { FlushChanges(true); Logger::Log("CELL %s (game time %s)", LastCell.c_str(), GameClock().c_str()); }
		lastCell = cell; lastWorld = world; lastInterior = interior;
	}
	// Weather changes
	static TESWeather* lastWeather = nullptr;
	TESWeather* weather = Tes && Tes->sky ? Tes->sky->firstWeather : nullptr;
	if (weather != lastWeather) {
		lastWeather = weather;
		char text[160];
		const char* name = weather ? weather->GetEditorName() : nullptr;
		sprintf_s(text, "%08X '%s'", weather ? weather->refID : 0, name && name[0] ? name : "?");
		LastWeather = text;
		Logger::Log("WEATHER %s (game time %s)", text, GameClock().c_str());
	}

	if (TheShaderManager->GameState.isCameraCut) Count.cameraCuts++;
	if (FrameJournal::On) {	// per-frame counts: one line when they change by more than the frame-to-frame noise
		static unsigned lastDrops = 0;
		const unsigned drops = FrameJournal::FadeDrops;
		if (drops > lastDrops + (std::max)(4u, lastDrops / 8) || drops + (std::max)(4u, lastDrops / 8) < lastDrops) {
			FRAME_JOURNAL("shadow map casters dropped by CasterFadeMin (LOD fade): %u (was %u)", drops, lastDrops);
			lastDrops = drops;
		}
	}
	FrameJournal::FadeDrops = 0;

	// The heartbeat's frame times: frames in game, loading screens left out
	if (WindowStart < 0.0) WindowStart = now;
	if (!loading && frameMs > 0.0 && frameMs < 60000.0) {
		FrameMs.push_back(frameMs);
		FrameRing[FrameRingNext++ % (sizeof(FrameRing) / sizeof(FrameRing[0]))] = (float)frameMs;	// UNOFFICIAL 2026-11-01 (REPORT)
		InGameSeconds += frameMs / 1000.0;
		if (frameMs > WorstEver && now - StartSeconds > 1.0) WorstEver = frameMs;
	}
	if (now - WindowStart >= 60.0) Heartbeat(now);
}

// ---- UNOFFICIAL 2026-11-07 (perf-ptcost) the STALL watchdog (SessionLog.h StallWatch) ---------------------------------------------------
namespace {
	volatile LONG64 StallProgressQpc = 0;	// the last RenderBegin / RenderEnd (QPC)
	volatile LONG StallFrames = 0, StallInRender = 0, StallArmed = 0, StallLoading = 0;
	volatile LONG StallStop = 0;
	HANDLE StallThread = nullptr, StallRenderThread = nullptr;
	DWORD StallRenderThreadId = 0;
	UINT_PTR StallStackBase = 0;
	const char* const* StallStageStack = nullptr;	// the render thread's SessionLog::Stages (thread_local: its addresses, read from here)
	const unsigned* StallStageDepth = nullptr;
	void (*StallExtra)(char*, unsigned) = nullptr;
	LONG64 StallQpcFrequency = 0;

	double StallNowMs(LONG64 qpc) { return StallQpcFrequency ? (double)qpc * 1000.0 / (double)StallQpcFrequency : 0.0; }

	// The render thread's instruction pointer and the return addresses on its stack (a scan, as the CRASH lines'), suspended only while
	// GetThreadContext runs and its stack is copied (no lock, no allocation in between)
	unsigned StallSample(UINT_PTR& eip, UINT_PTR* out, unsigned max) {
		static UINT_PTR copy[4096];	// 16 KB of stack (the watchdog thread is the only user)
		eip = 0;
		if (!StallRenderThread || SuspendThread(StallRenderThread) == (DWORD)-1) return 0;
		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		size_t words = 0;
		if (GetThreadContext(StallRenderThread, &ctx)) {
			eip = ctx.Eip;
			const UINT_PTR start = ctx.Esp;
			if (start && StallStackBase > start) {
				words = (std::min)((size_t)((StallStackBase - start) / sizeof(UINT_PTR)), sizeof(copy) / sizeof(copy[0]));
				__try { memcpy(copy, (const void*)start, words * sizeof(UINT_PTR)); }
				__except (EXCEPTION_EXECUTE_HANDLER) { words = 0; }
			}
		}
		ResumeThread(StallRenderThread);
		unsigned found = 0;
		for (size_t i = 0; i < words && found < max; i++)
			if (LooksLikeReturn(copy[i]) && (!found || out[found - 1] != copy[i])) out[found++] = copy[i];
		return found;
	}

	// "module+offset" WITHOUT the loader lock (GetModuleHandleEx / GetModuleFileName take it, and a frozen render thread may be holding it:
	// a LoadLibrary in a driver): the image's base from VirtualQuery, its name from its own export directory (the exe has none: its base is
	// known), else "module@base"
	HMODULE StallExeBase = nullptr;
	void StallModuleText(UINT_PTR address, char* out, size_t size) {
		MEMORY_BASIC_INFORMATION info;
		if (address < 0x10000 || !VirtualQuery((const void*)address, &info, sizeof(info)) || info.Type != MEM_IMAGE || !info.AllocationBase) {
			sprintf_s(out, size, "%08lX (in no module)", (unsigned long)address);
			return;
		}
		const UINT_PTR base = (UINT_PTR)info.AllocationBase;
		char name[64] = "";
		if ((HMODULE)base == StallExeBase) strcpy_s(name, "FalloutNV.exe");
		else {
			__try {
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
				const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
				const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
				if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE && dir.VirtualAddress && dir.Size) {
					const IMAGE_EXPORT_DIRECTORY* exp = (const IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
					if (exp->Name) strncpy_s(name, (const char*)(base + exp->Name), _TRUNCATE);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; }
		}
		if (name[0]) sprintf_s(out, size, "%s+%lX", name, (unsigned long)(address - base));
		else sprintf_s(out, size, "module@%08lX+%lX", (unsigned long)base, (unsigned long)(address - base));
	}

	const char* StallKind(UINT_PTR address) {	// what kind of code the address is in, from its module's name
		char text[MAX_PATH + 32];
		StallModuleText(address, text, sizeof(text));
		_strlwr_s(text);
		if (strstr(text, "d3d9.dll") || strstr(text, "dxvk")) return "the D3D9 runtime (d3d9.dll: DXVK / Vegas Shaders DXVK)";
		if (strstr(text, "ntdll") || strstr(text, "kernelbase") || strstr(text, "kernel32") || strstr(text, "win32u") || strstr(text, "user32"))
			return "Windows (a wait, I/O or a system call)";
		if (strstr(text, "vegasshaders") || strstr(text, "newvegasreloaded")) return "Vegas Shaders";
		if (strstr(text, "falloutnv")) return "the game's own code";
		if (strstr(text, "nvoglv") || strstr(text, "nvd3d") || strstr(text, "amdvlk") || strstr(text, "atio") || strstr(text, "igvk") || strstr(text, "vulkan"))
			return "the graphics driver";
		if (strstr(text, "in no module")) return "no module (generated code?)";
		return "another module (a plugin or a library)";
	}

	struct StallState {
		bool on = false;
		LONG frame = 0;
		bool inRender = false;
		LONG64 startQpc = 0;
		double nextReportMs = 1000.0;
		ULONGLONG cpuStart = 0;	// the render thread's CPU time (100 ns) when the stall began
		DWORD faultsStart = 0;
		unsigned samples = 0;
		struct Spot { UINT_PTR eip; unsigned count; } spots[8] = {};
	};

	ULONGLONG StallThreadCpu() {
		FILETIME c, e, k, u;
		if (!StallRenderThread || !GetThreadTimes(StallRenderThread, &c, &e, &k, &u)) return 0;
		return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
	}
	DWORD StallPageFaults(SIZE_T* commitMB = nullptr) {
		struct Counters { DWORD cb, faults; SIZE_T peakWs, ws, qpp, qp, qnpp, qnp, pagefile, peakPagefile; } m = {};	// PROCESS_MEMORY_COUNTERS
		m.cb = sizeof(m);
		typedef BOOL (WINAPI* Fn)(HANDLE, void*, DWORD);
		static Fn fn = (Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
		if (!fn || !fn(GetCurrentProcess(), &m, sizeof(m))) return 0;
		if (commitMB) *commitMB = m.pagefile >> 20;
		return m.faults;
	}
	void StallMemoryText(char* out, size_t size) {	// the address space: used, the largest free block; the system's memory
		SIZE_T largest = 0, freeTotal = 0;
		MEMORY_BASIC_INFORMATION info;
		for (UINT_PTR a = 0x10000; a < 0xFFFF0000u; ) {
			if (!VirtualQuery((const void*)a, &info, sizeof(info))) break;
			if (info.State == MEM_FREE) { freeTotal += info.RegionSize; largest = (std::max)(largest, info.RegionSize); }
			const UINT_PTR next = (UINT_PTR)info.BaseAddress + info.RegionSize;
			if (next <= a) break;
			a = next;
		}
		MEMORYSTATUSEX m = { sizeof(m) };
		GlobalMemoryStatusEx(&m);
		SIZE_T commitMB = 0;
		const DWORD faults = StallPageFaults(&commitMB);
		sprintf_s(out, size, "address space %.0f of %.0f MB used, largest free block %.0f MB (free %.0f MB in all); committed %u MB; system RAM free %.1f of %.1f GB, "
			"commit free %.1f GB; page faults so far %lu", (m.ullTotalVirtual - m.ullAvailVirtual) / 1048576.0, m.ullTotalVirtual / 1048576.0, largest / 1048576.0,
			freeTotal / 1048576.0, (unsigned)commitMB, m.ullAvailPhys / 1073741824.0, m.ullTotalPhys / 1073741824.0, m.ullAvailPageFile / 1073741824.0, (unsigned long)faults);
	}
	void StallStagesText(char* out, size_t size) {
		out[0] = 0;
		if (!StallStageStack || !StallStageDepth) { strcpy_s(out, size, "(unknown)"); return; }
		__try {
			const unsigned depth = (std::min)(*StallStageDepth, 8u);
			for (unsigned i = 0; i < depth; i++) {
				const char* s = StallStageStack[i];
				if (i) strcat_s(out, size, " > ");
				strncat_s(out, size, s ? s : "?", _TRUNCATE);
			}
			if (!depth) strcpy_s(out, size, "(none: outside every marked stage)");
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, size, "(unreadable)"); }
	}
	void StallExtraText(char* out, unsigned size) {
		out[0] = 0;
		if (!StallExtra) return;
		__try { StallExtra(out, size); }
		__except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, size, "(unreadable)"); }
	}

	void StallReport(StallState& s, double ms, bool final) {
		char stages[400], memory[400], extra[600], where[MAX_PATH + 32], stack[1600] = "";
		StallStagesText(stages, sizeof(stages));
		StallMemoryText(memory, sizeof(memory));
		StallExtraText(extra, sizeof(extra));
		const ULONGLONG cpu = StallThreadCpu();
		const double cpuMs = cpu > s.cpuStart ? (cpu - s.cpuStart) / 10000.0 : 0.0;
		const DWORD faults = StallPageFaults();
		if (final) {
			// the most sampled instruction pointer of the stall
			unsigned best = 0;
			for (unsigned i = 1; i < 8; i++) if (s.spots[i].count > s.spots[best].count) best = i;
			if (s.spots[best].count) StallModuleText(s.spots[best].eip, where, sizeof(where)); else strcpy_s(where, "(no sample)");
			Logger::Log("STALL over after %.1f s (frame %ld, %s): the render thread used %.0f ms of CPU in it (%.0f %%), %lu page faults; most of %u samples at %s "
				"(%s, %u times) | %s", ms / 1000.0, (long)s.frame, s.inRender ? "inside the game frame" : "between game frames", cpuMs, ms > 0 ? 100.0 * cpuMs / ms : 0.0,
				(unsigned long)(faults - s.faultsStart), s.samples, where, s.spots[best].count ? StallKind(s.spots[best].eip) : "-", s.spots[best].count, memory);
			return;
		}
		UINT_PTR eip = 0, returns[14];
		const unsigned count = StallSample(eip, returns, 14);
		StallModuleText(eip, where, sizeof(where));
		for (unsigned i = 0; i < count; i++) {
			char entry[MAX_PATH + 32];
			StallModuleText(returns[i], entry, sizeof(entry));
			if (i) strcat_s(stack, ", ");
			strncat_s(stack, entry, _TRUNCATE);
		}
		Logger::Log("STALL: the render thread has not finished a game frame for %.1f s (frame %ld, %s; stages: %s) | now in %s: %s | callers (a stack scan, newest first): "
			"%s | CPU used in the stall %.0f ms (%.0f %%: %s), page faults %lu | %s%s%s", ms / 1000.0, (long)s.frame,
			s.inRender ? "inside the game frame (RenderHook)" : "BETWEEN game frames: the game's update, Present, or a pause", stages, where, eip ? StallKind(eip) : "?",
			count ? stack : "(none found)", cpuMs, ms > 0 ? 100.0 * cpuMs / ms : 0.0, ms > 0 && cpuMs < 0.2 * ms ? "it waits" : "it computes", (unsigned long)(faults - s.faultsStart),
			memory, extra[0] ? " | " : "", extra);
	}

	DWORD WINAPI StallMain(void*) {
		StallState s;
		unsigned stalls = 0;
		while (!InterlockedCompareExchange(&StallStop, 0, 0)) {
			Sleep(200);
			if (!InterlockedCompareExchange(&StallArmed, 0, 0)) continue;
			LARGE_INTEGER now; QueryPerformanceCounter(&now);
			const LONG64 last = InterlockedCompareExchange64(&StallProgressQpc, 0, 0);
			const double ms = StallNowMs(now.QuadPart - last);
			const LONG frame = InterlockedCompareExchange(&StallFrames, 0, 0);
			if (s.on && frame != s.frame) {	// frames again
				StallReport(s, StallNowMs(last - s.startQpc), true);
				s = StallState();
				continue;
			}
			if (s.on) {
				if (s.samples < 1000) {	// where it is, 5 times a second (the "over" line's most common spot)
					UINT_PTR eip = 0, none[1];
					StallSample(eip, none, 0);
					s.samples++;
					unsigned i = 0;
					for (; i < 8 && s.spots[i].count && s.spots[i].eip != eip; i++) {}
					if (i < 8) { s.spots[i].eip = eip; s.spots[i].count++; }
				}
				if (ms >= s.nextReportMs) { StallReport(s, ms, false); s.nextReportMs *= 4.0; }
				continue;
			}
			if (ms < 1000.0 || last == 0) continue;
			// not a stall: a loading screen, the game minimised or behind another window (the game pauses its loop there)
			if (InterlockedCompareExchange(&StallLoading, 0, 0)) continue;
			if (GameWindow && (IsIconic(GameWindow) || GetForegroundWindow() != GameWindow)) continue;
			if (stalls >= 40) continue;
			stalls++;
			s.on = true; s.frame = frame; s.inRender = InterlockedCompareExchange(&StallInRender, 0, 0) != 0; s.startQpc = last;
			s.cpuStart = StallThreadCpu(); s.faultsStart = StallPageFaults();
			StallReport(s, ms, false);
			s.nextReportMs = 4000.0;
		}
		return 0;
	}
}

void SessionLog::StallWatch::RenderBegin() {
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	if (!StallRenderThread) {
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); StallQpcFrequency = f.QuadPart;
		StallRenderThreadId = GetCurrentThreadId();
		StallExeBase = GetModuleHandleA(nullptr);
		StallRenderThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, StallRenderThreadId);
		StallStackBase = (UINT_PTR)((const NT_TIB*)NtCurrentTeb())->StackBase;
		StallStageStack = SessionLog::Stages::Stack;
		StallStageDepth = &SessionLog::Stages::Depth;
		StallThread = CreateThread(nullptr, 64 * 1024, StallMain, nullptr, 0, nullptr);
		if (StallThread) SetThreadPriority(StallThread, THREAD_PRIORITY_ABOVE_NORMAL);
		Logger::Log("SESSION STALL watchdog %s (render thread %lu): a game frame that does not end within 1 s gets STALL lines (where the render thread is)",
			StallThread && StallRenderThread ? "on" : "could not start", (unsigned long)StallRenderThreadId);
	}
	if (GetCurrentThreadId() != StallRenderThreadId) return;
	InterlockedExchange64(&StallProgressQpc, now.QuadPart);
	InterlockedExchange(&StallInRender, 1);
	InterlockedIncrement(&StallFrames);
	// in game (a cell) and not on a loading screen: the main thread's view, kept for the watchdog
	InterlockedExchange(&StallArmed, Player && Player->parentCell ? 1 : 0);
	InterlockedExchange(&StallLoading, (InterfaceManager && InterfaceManager->IsActive(Menu::kMenuType_Loading)) || LoadingStart >= 0.0 ? 1 : 0);
}

void SessionLog::StallWatch::RenderEnd() {
	if (GetCurrentThreadId() != StallRenderThreadId) return;
	LARGE_INTEGER now; QueryPerformanceCounter(&now);
	InterlockedExchange64(&StallProgressQpc, now.QuadPart);
	InterlockedExchange(&StallInRender, 0);
}

void SessionLog::StallWatch::SetExtra(void (*text)(char* out, unsigned size)) { StallExtra = text; }

void SessionLog::StallWatch::Stop() {
	InterlockedExchange(&StallArmed, 0);
	InterlockedExchange(&StallStop, 1);
	if (StallThread) { WaitForSingleObject(StallThread, 1000); CloseHandle(StallThread); StallThread = nullptr; }
}
