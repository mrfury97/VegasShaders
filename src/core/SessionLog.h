#pragma once
// UNOFFICIAL 2026-11-01 (user: "expand our logging to truly capture all info that could be valuable to us without leaking any info
// about a user"): what a bug report needs, in every log, kept small (a normal hour stays well under 2 MB). All lines start with SESSION,
// SETTING, CELL, WEATHER, LOADING, GAME, HEARTBEAT or FJ, so they can be grepped. Every line passes the Logger's privacy scrubber
// (base/LogScrub.h), and none of them prints a path, a user name, a computer name, a LUID, a serial or a save name in the first place.
//   - the session header: the build (version + git commit), xNVSE, Windows, CPU, RAM and address space, the GPUs with their driver and
//     VRAM, the D3D9 runtime (native / DXVK + version / NVR DXVK), the screen mode, the upscaler, a few of the game's INI values, the
//     plugin count + a load order hash + the names, the xNVSE plugin DLLs, and every NVR setting that is not its default (key = value
//     (default));
//   - live: each setting change (old -> new, coalesced while a slider is dragged, rate-limited), cell / worldspace changes, weather
//     changes, loading screens, saves, loads, new games, exits;
//   - HEARTBEAT every 60 s in game: frame times (avg, 1% low, worst, spikes), address space, the D3D9 texture memory estimate, effects on,
//     and the flicker counters of the minute (camera cuts, history resets, lamp shadow slot changes, cached sun cascade redraws,
//     shadow atlas re-converts, reflection cube restarts, AmbientLight grid rebuilds);
//   - SESSION END at exit, and CRASH lines from a chained unhandled-exception filter (the previous filter, e.g. a crash logger,
//     still runs): 2026-11-04 armed at plugin load (it came only once the player was in a cell, so a crash at start-up or in the
//     main menu left nothing), with the exception in words, the registers, a scanned call stack and the stage the thread was in.
// The frame journal ([Main.Develop.Main] FrameJournal, off by default) adds one FJ line per event that changes what is drawn, with the
// frame number of the line prefix, up to 20000 lines a session (for flicker hunting with the bench's shots).
// UNOFFICIAL 2026-11-01 (core/Diagnostics.cpp): the journal's events are ALSO kept in memory, always (the last 256, journal on or off),
// so a FLASH dump (the automatic flash detector, [Main.Main.Misc] FlashDetect) and a REPORT block (the report key, [Main.Main.Misc]
// ReportKey) can list what happened in the frames around it; with the journal off they reach the log only in those dumps.
#include "EffectStatus.h"

class GameSetting;
struct IDirect3DDevice9;
struct _EXCEPTION_RECORD;

namespace FrameJournal {
	inline bool On = false;			// [Main.Develop.Main] FrameJournal (read once a second by SessionLog::OnFrame)
	inline unsigned Lines = 0;		// FJ lines written this session
	constexpr unsigned Cap = 20000;
	void Note(const char* format, ...);	// one event into the ring; an FJ line too when On (nothing past the cap)
	// The ring's events of frames first..last (inclusive) and no older than maxAgeSeconds (0: any age), one log line each with head
	// in front; returns how many
	unsigned Dump(const char* head, unsigned first, unsigned last, double maxAgeSeconds = 0.0);
	// UNOFFICIAL 2026-11-01 (the report burst, core/Diagnostics.cpp): how many of the ring's events belong to frame (those whose text
	// starts with prefix, when one is given)
	unsigned CountFrame(unsigned frame, const char* prefix = nullptr);
	// Per-frame counts written as one FJ line when they change (SessionLog::OnFrame): events that happen every frame by design
	inline unsigned FadeDrops = 0;		// shadow map casters (sun + ortho walks) left out because their LOD fade is under CasterFadeMin, this frame
}
#define FRAME_JOURNAL(...) do { FrameJournal::Note(__VA_ARGS__); } while (0)	// (2026-11-01: always, for the in-memory ring; events are rare)

namespace SessionLog {
	// The flicker counters of the heartbeat's minute (always counted; cheap increments at the sites)
	struct Counters {
		unsigned historyResets = 0, lampSlotChanges = 0, cachedCascadeRedraws = 0, atlasReconverts = 0, cubeRestarts = 0, gridRebuilds = 0,
			poolDraws = 0, cameraCuts = 0;
	};
	inline Counters Count;

	// A temporal effect dropped its history (counted; an FJ line with the reason when the journal is on)
	void HistoryReset(const char* effect, const char* why);

	void OnPluginLoad(unsigned nvseVersion, unsigned gameVersion, unsigned pluginVersion);	// NVSEPlugin_Load, after the log opens
	void OnSettingsLoaded();				// NVSEPlugin_Load, after the first LoadSettings: the settings that are not their default
	// UNOFFICIAL 2026-11-01: every setting that differs from this build's defaults NOW (the live values: a preset loaded after the
	// start counts), as "<head>: N of M differ ..." then "<head>: [section] key=yours (default), ... | [section] ..." lines; the
	// SESSION header's list (head "SESSION settings") and the REPORT block's (head "REPORT n settings", core/Diagnostics.cpp)
	void LogSettingsDiff(const char* head);
	// 2026-11-03: the same list as data ("[section] key=yours (default), ..." per section), and the counts
	std::vector<std::string> SettingsDiff(unsigned& differ, unsigned& total);
	void OnDevice(IDirect3DDevice9* device);	// RenderManager::Initialize: GPU, runtime, screen
	void OnDeferredInit();					// xNVSE DeferredInit: the game INI values, the load order, the plugin DLLs
	void NoteIni(const GameSetting* setting);	// ReadSettingHook: keeps the few INI values the header lists
	void OnFrame();							// ShaderManager::RenderEffects, first thing every rendered frame
	void OnSettingChanged(const char* section, const char* key, const char* before, const char* after);	// SettingManager::SetSetting
	void OnEvent(const char* what);			// GAME line: saves, loads, menus (Main.cpp MessageHandler)
	void OnExit(const char* how);			// SESSION END line
	// UNOFFICIAL 2026-11-01 for the REPORT block and FLASH dumps (core/Diagnostics.cpp)
	std::string Where();					// the current cell as the CELL lines write it
	std::string Weather();					// the current weather as the WEATHER lines write it
	std::string Clock();					// the game time, hh:mm
	// UNOFFICIAL 2026-11-01: frame generation, scaling, capture and overlay programs running beside the game (Lossless Scaling, RTSS,
	// OBS...; a fixed list of names, nothing else is named), their hook DLLs in the game, which window is in front, the game window's
	// state. Logged at start (SESSION environment), when it changes (ENVIRONMENT line, checked once a minute) and in the REPORT block.
	std::string Environment();
	// The last frames' times (ms, oldest first, loading screens left out), at most count
	void RecentFrameTimes(std::vector<float>& out, unsigned count);

	// 2026-11-04 (user: "we constantly need to ask other users for crash logs. is there anyway we can get some of that info in our own
	// log?"): the CRASH lines name the stage the crashing thread was in. A StageScope marks one for its lifetime; nested ones stack,
	// per thread, 8 deep, so a crash reads "game frame > effects > AmbientOcclusion". A store and a decrement: cheap enough per effect.
	namespace Stages {
		inline thread_local const char* Stack[8] = {};
		inline thread_local unsigned Depth = 0;
	}
	struct StageScope {
		explicit StageScope(const char* stage) { if (Stages::Depth < 8) Stages::Stack[Stages::Depth] = stage; Stages::Depth++; }
		~StageScope() { Stages::Depth--; }
		StageScope(const StageScope&) = delete;
		StageScope& operator=(const StageScope&) = delete;
	};
	// NVSEPlugin_Load, after OnPluginLoad: the CRASH lines from here on (the filter set before ours, a crash logger's, still runs)
	void ArmCrashReport();

	// UNOFFICIAL 2026-11-07 (perf-ptcost; the 11-07 tester log: game freezes of 9-55 s with no line saying why) the STALL watchdog. A thread
	// wakes 5 times a second; when the render thread has not begun or ended a game frame (RenderHook) for over 1 s, in game, not on a loading
	// screen, with the game window in front and not minimised, it writes STALL lines at 1, 4, 16 and 64 s, and "STALL over" when frames come
	// again: where the render thread is (inside the frame or outside it: the game's update, Present, a pause), its stage stack (the StageScope
	// names: "game frame > effects > <chain step> > ..."), what it runs (its instruction pointer's module + offset and the return addresses
	// on its stack: d3d9.dll = the D3D9 runtime / DXVK, ntdll / KERNELBASE = a Windows wait or I/O, FalloutNV.exe = the game's code), the
	// CPU it used meanwhile (near 0 = it waits; near 100 % = it computes), page faults (a paging stall), the 32-bit address space (used,
	// largest free block) and system memory, and the extra state registered with SetExtra (RtCore: the RT helper's frames). The thread is
	// suspended only for GetThreadContext and a copy of its stack (nothing that takes a lock). At most 40 stalls a session.
	namespace StallWatch {
		void RenderBegin();		// NewVegas/Hooks/Render.cpp RenderHook (FrameScope)
		void RenderEnd();
		struct FrameScope { FrameScope() { RenderBegin(); } ~FrameScope() { RenderEnd(); } FrameScope(const FrameScope&) = delete; FrameScope& operator=(const FrameScope&) = delete; };
		void SetExtra(void (*text)(char* out, unsigned size));	// called on the watchdog thread: read only what is safe to read from there
		void Stop();			// at exit (SessionLog::OnExit)
	}
	// xNVSE PostLoad and once in game: a crash handler set after ours goes under ours (when: for the log line)
	void ReassertCrashReport(const char* when);
	// A C++ exception's type as the compiler names it (".?AVtype_error@toml@@"), or "?"; safe on a broken record
	const char* CppExceptionType(const _EXCEPTION_RECORD* record);
}
