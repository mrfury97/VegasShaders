#pragma once

// UNOFFICIAL, TEST ONLY (2026-10-12): A/B shots for the auto-test (work\NVRAutoTest, run_autotest.py --shots <file>). When the
// test plugin asks NVR for a run (EffectSelfTest's link, kLinkStart) and the launcher has put Data\NVSE\Plugins\NVRAutoTest.shots.txt
// in the game folder, NVR runs this instead of the neon self-test. Each shot in the file changes some settings IN MEMORY ONLY
// (never saved: the menu's Save is the only writer, and the test ends the game without it), waits a number of frames, saves the
// finished picture (before the HUD) as VegasShaders-Logs\NeonSelfTest_<run>_ab_<NN>_<slug>.png (the name the launcher
// collects), and puts every setting back before the next shot. Without the file (normal play) nothing here ever runs.
//
// The file, one command per line ('#' starts a comment):
//   frames 120                                   frames each shot waits before its picture (default 120: history buffers settle)
//   profile                                      switches the F10 GPU profile on for the run (its per-second lines go to the log)
//   shot <slug>                                  starts a shot; its picture is named after the slug
//   set <Section> <Key> <value>                  e.g. set Shaders.Reflections.Main GroundSheen 0.8 (typed as the setting is)
//   effect <Name> <0|1>                          an effect or shader collection off or on, as the menu's switch (e.g. effect Reflections 1)
//   console <command>                            a console command, run at the start of the shot's first frame (set GameHour to 13,
//                                                player.setangle x -30, fw 0015BD42, ...). NOT undone: it stays for the later shots,
//                                                and the place check allows a move during a shot that runs one.
//   final                                        (2026-10-15) every picture is the back buffer at ReShade's end of the frame
//                                                (after ReShade's effects and the HUD) instead of NVR's picture before the HUD;
//                                                needs ReShade's D3D9 runtime (ReShadeIntegration.cpp calls CaptureFinal), or
//                                                (2026-10-18) its Vulkan runtime under DXVK (capture_screenshot there).
//   world                                        (2026-10-16) each shot also saves the world scene's HDR target raw (.world, CaptureWorld)
//   record                                       (2026-10-17) the shot's frame is also RECORDED whole for tests/frame_replay.exe: when its
//                                                frames are done, the next frame (Present to Present) is written to
//                                                NeonSelfTest_<run>_ab_<NN>_<slug>.nvrf (FrameRecorder.h) and the picture is taken on the
//                                                frame after. Before the first 'shot' it applies to every shot, after one only to that shot.
//                                                Use with 'profile' (the GPU split marks need the F10 profile on); not with 'world'.
//   stage                                        (2026-10-18) each shot also saves the HDR picture right after the AmbientLight step
//                                                (whether it ran or not) raw, as <slug>-al.world (CaptureStage; the same file format)
//   noreload                                     (2026-10-21) a shot with no set/effect lines skips the settings reload (LoadSettings)
//                                                every shot otherwise starts with: nothing in the game is touched in that shot
//   cloudcoverage <0-1>                          (2026-10-21) at the start of the shot, the volumetric clouds' eased coverage is set to
//                                                this (as right after a load or a weather change); it eases back to the weather's over
//                                                about ten seconds. Not a setting: no reload, nothing to undo
//   wait <frames>                                (2026-10-21, button sweep) this shot waits that many frames instead of 'frames'
//                                                (e.g. a long first shot, so the scene has settled after the load)
//   sampler <hz>                                 (2026-10-21, cpulimits) a sampling profiler on the main and AI threads for the run
//                                                (CpuSampler.h; per-shot counts in VegasShaders-Logs\CpuSamples_<time>.txt)
//   affinity <hex mask>                          (2026-10-21, cpulimits) at the start of the shot the game process may use only these
//                                                logical processors (SetProcessAffinityMask on this process only; e.g. 3 = two, F = four):
//                                                a weaker CPU emulated in the same launch. NOT undone: it stays for the later shots.
//   facemoon                                     (2026-10-25, moon behind clouds) at the start of the shot the player looks at the moon
//                                                (player.setangle x and z from the moon's position in the sky)
//   reloadshaders                                (2026-10-25, bench warm slot) at the start of the shot every replaced game shader and
//                                                every effect is compiled again from its .hlsl (changed files only; the cache answers the
//                                                rest), at the end of that frame: a shader-only change tested without a new launch. Give
//                                                the shot a 'wait' (compiles take frames). NOT undone.
//   aim <ref form id> [height]                   (2026-10-25, bench framing check) at the start of the shot the camera turns to look at
//                                                that reference (+ height units up), from where the camera is: the subject is in the
//                                                picture even when the spot or a moveto left the view a little off. NOT undone.
//   require onscreen <ref form id> [margin]      (2026-10-25) before the picture: the reference's position must be inside the view,
//                                                margin = the fraction of the half-screen kept free at the edges (default 0.1). If not,
//                                                the camera is aimed at it and the shot waits 60 more frames, once; still off: the picture
//                                                is saved as <slug>-OFFFRAME and a FRAMING line is logged (the launcher reports it).
//   require detail                               (2026-10-25) the picture's middle third must not be flat (luma spread >= 4 of 255: not a
//                                                plain wall, sky or empty world); else saved as <slug>-FLAT with a FRAMING line.
//   flickerwatch [spike]                         (2026-10-25, bench sweep mode) FlickerWatch (FlickerWatch.h) on for the whole run, indoors
//                                                and out, with its spike score: a one-frame jump of a 4x3 picture cell away from both
//                                                neighbouring frames by more than <spike> (default 0.05 = 5 %). Each shot logs
//                                                "FLICKER SHOT '<slug>': n frames, n spikes, max x %" (multirun\sweep.py scores the log)
//   turn <degrees per second>                    (2026-10-25, cloudflicker) the player turns on the spot at this rate for the whole run
//                                                (player.setangle z on the main loop each tick), so the camera pans as in play
//   capture [frames]                           (2026-10-26, debug capture) asks RenderDoc for a capture of this shot's picture frame
//                                                (default 2 frames: the picture frame and its neighbour, as DXVK presents a frame
//                                                late); only when renderdoc.dll is already loaded (the game started from RenderDoc
//                                                on the DXVK d3d9.dll), else one log line. GpuDebug.h, docs/next/FORK-DEBUG-CAPTURE.md
//   inspect <x> <y>                              (2026-10-26, debugging tools) this shot switches the pixel inspector on for screen pixel
//                                                (x, y): [Main.Develop.PixelInspector] Enabled true, X, Y (undone after the shot like any
//                                                'set'); its first result is logged ("PIXEL INSPECTOR" lines). Needs NVR DXVK 1.5-debug.
//   renderdoc [<path template>]                  (2026-10-26, DXVK fork barriers) this shot asks RenderDoc for a capture of one of its
//                                                frames (TriggerCapture, two frames before the picture), saved as <template>_frame<N>.rdc
//                                                (default .\VegasShaders-Logs\RenderDoc_<run>_<slug>). Needs RenderDoc in the
//                                                game: under DXVK its Vulkan layer, ENABLE_VULKAN_RENDERDOC_CAPTURE=1 for the launch.
//                                                Without it: logged, nothing else. ('capture' above is the other way in, through
//                                                GpuDebug: it needs renderdoc.dll loaded the same way.)
//   keep                                         (2026-10-27, FSR switch flicker) this shot's set/effect lines are NOT undone after its
//                                                picture: they stay for the later shots (a mid-run switch, e.g. FSR switched on once and
//                                                pictures every few frames after it with 'noreload'); undone when the run ends
//   landflicker [frames]                         (2026-10-30, terrain flicker) the shot's last frames (default 60) measure the land's
//                                                flicker from a still camera, then one frame marks the land magenta: LAND FLICKER log
//                                                lines and <slug>-landstd.png (SettingShots::Land below; docs/next/TERRAIN-FLICKER.md)
// Shots start from the user's settings each time: a shot without changes is the user's own picture.
#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "EffectSelfTestCore.h"
#include "GpuProfiler.h"
#include "ImGuiManager.h"
#include "CpuSplit.h"
#include "FrameRecorder.h"	// UNOFFICIAL, test only: 'record' (2026-10-17, frame replay rig)
#include "CpuSampler.h"
#include "FlickerWatch.h"	// 2026-10-25 bench sweep mode ('flickerwatch')

namespace SettingShots {

	inline const char* const RequestFile = "Data\\NVSE\\Plugins\\NVRAutoTest.shots.txt";
	enum : UInt32 { kLinkStarted = 0x4E560011, kLinkStopped = 0x4E560012, kLinkPictures = 0x4E560014 };	// EffectSelfTest's link numbers

	struct Change { bool effect = false, console = false, coverage = false; std::string section, key, value; };
	struct Shot { std::string slug; std::vector<Change> changes; std::string menu; bool menuTests = false; int frames = 0; DWORD_PTR affinity = 0; int capture = 0;
		bool renderdoc = false; std::string renderdocPath; bool keep = false; int landFrames = 0; bool record = false;
		bool reload = false; UInt32 aimRef = 0; float aimHeight = 0.0f; UInt32 needRef = 0; float needMargin = 0.1f; bool needDetail = false; bool retried = false; };	// reload ('reloadshaders'), aim/need*: framing check (2026-10-25, bench warm slot)	// landFrames: 'landflicker' (2026-10-30)	// menu: 2026-10-19; frames: 'wait' (2026-10-21); affinity: cpulimits (2026-10-21); capture: RenderDoc frames (2026-10-26); renderdoc: 2026-10-26
	struct Undo { bool effect = false; std::string section, key; UInt32 type = 0; bool b = false; int i = 0; float f = 0.0f; std::string s; };
	struct Job { std::vector<unsigned> bgra; unsigned width = 0, height = 0; char path[MAX_PATH] = ""; };

	inline std::vector<Shot> List;
	inline int Frames = 120;
	inline bool Pending = false, Running = false, AllQueued = false;
	inline size_t Index = 0;
	inline int Frame = 0;
	inline std::vector<Undo> Undos;
	inline std::vector<Undo> Kept;				// 'keep' (2026-10-27): changes left on for the later shots, undone at the run's end
	inline SelfTestCore::ShotReader Reader;
	inline std::atomic<int> InFlight{ 0 };
	inline char Run[32] = "";
	inline NVSEMessagingInterface* Messaging = nullptr;
	inline UInt32 Handle = 0;
	inline TESObjectCELL* Cell = nullptr;
	inline std::vector<std::string> Commands;	// this shot's console commands, run at the start of its first frame
	inline bool Moves = false;					// this shot ran console commands: the player may change place
	inline bool MainLoopSeen = false;			// 2026-10-20: xNVSE's main-loop message arrives (Main.cpp): the console commands run there
	inline bool FinalPicture = false;			// 'final': pictures from the back buffer at ReShade's present
	// 'final' state (2026-10-18, atomic: ReShade's Vulkan runtime presents on DXVK's own thread): 0 nothing asked; 1 the shot's
	// frames are done, the next present takes the picture; 2 being taken on the Vulkan runtime; 3 taken there, FinalJob holds it
	// and the render thread finishes the shot (BeginFrame).
	inline std::atomic<int> FinalState{ 0 };
	inline Job* FinalJob = nullptr;
	inline std::atomic<unsigned> FinalFrame{ 0 };	// UNOFFICIAL 2026-11-01: the rendered frame the Vulkan 'final' picture was read at
	// UNOFFICIAL 2026-11-01 (bench <-> log sync): the frame the shot's picture shows, as the "[time fN]" prefix of every log line counts them
	inline void LogCaptured(unsigned frame, const char* how) {
		if (Index < List.size()) Logger::Log("SETTING SHOTS %s: shot '%s' captured at frame %u (%s)", Run, List[Index].slug.c_str(), frame, how);
	}

	inline bool Requested() { return GetFileAttributesA(RequestFile) != INVALID_FILE_ATTRIBUTES; }

	inline void Notify(UInt32 type, const char* text) {
		if (Messaging) Messaging->Dispatch(Handle, type, (void*)text, (UInt32)strlen(text) + 1, "NVRAutoTest");
	}

	// The link's start, when the request file is there (EffectSelfTest::AutoTestMessage).
	inline void Request(NVSEMessagingInterface* messaging, UInt32 handle) { Messaging = messaging; Handle = handle; Pending = true; }

	inline bool WorldCapture = false;			// 'world' (2026-10-16): each shot also saves the world scene's HDR target (CaptureWorld)
	inline size_t WorldTaken = (size_t)-1;		// the shot whose world target is saved already
	inline bool StageCapture = false;			// 'stage' (2026-10-18): each shot also saves the picture after the AmbientLight step
	inline bool NoReload = false;				// 'noreload' (2026-10-21): shots without changes skip the settings reload
	inline float TurnRate = 0.0f;				// 'turn' (2026-10-25): degrees per second the player turns while the shots run
	inline float WalkRate = 0.0f, WalkPeriod = 2.0f;	// 'walk <units/s> [seconds]' (2026-10-30): the player glides forward / back, turning round every period
	inline size_t PreImageSpaceTaken = (size_t)-1;	// 2026-11-02 'world': the shot whose pre-image-space picture is saved already
	inline size_t StageTaken = (size_t)-1;		// the shot whose stage picture is saved already
	inline IDirect3DSurface9* WorldResolve = nullptr;
	inline IDirect3DSurface9* WorldReadback = nullptr;
	inline D3DSURFACE_DESC WorldDesc = {};

	inline bool RecordAll = false;				// 'record' before the first shot (2026-10-17)
	inline size_t RecordArmed = (size_t)-1, RecordFinished = (size_t)-1;

	inline bool Parse(std::string& error) {
		Kept.clear();
		List.clear(); Frames = 120; RecordAll = false; RecordArmed = RecordFinished = (size_t)-1; FinalPicture = false; WorldCapture = false; WorldTaken = (size_t)-1; PreImageSpaceTaken = (size_t)-1; StageCapture = false; StageTaken = (size_t)-1; NoReload = false; TurnRate = 0.0f; FlickerWatch::Forced = false; WalkRate = 0.0f; WalkPeriod = 2.0f;
		if (FinalState.load() == 3) { delete FinalJob; FinalJob = nullptr; }
		FinalState.store(0);
		std::ifstream in(RequestFile);
		if (!in) { error = "the shot list could not be read"; return false; }
		std::string line;
		int number = 0;
		while (std::getline(in, line)) {
			number++;
			const size_t hash = line.find('#');
			if (hash != std::string::npos) line.resize(hash);
			std::istringstream words(line);
			std::string command;
			if (!(words >> command)) continue;
			if (command == "frames") { words >> Frames; Frames = std::clamp(Frames, 2, 1200); }
			else if (command == "profile") GpuTimer::Enabled = true;	// UNOFFICIAL 2026-10-13: the F10 GPU profile, on for the whole run
			else if (command == "final") FinalPicture = true;
			else if (command == "world") WorldCapture = true;
			else if (command == "stage") StageCapture = true;
			else if (command == "noreload") NoReload = true;
			else if (command == "keep") { if (List.empty()) { error = "line " + std::to_string(number) + ": 'keep' before any 'shot'"; return false; } List.back().keep = true; }
			else if (command == "reloadshaders") {	// 2026-10-25 bench warm slot
				if (List.empty()) { error = "line " + std::to_string(number) + ": a reloadshaders before the first 'shot'"; return false; }
				List.back().reload = true;
			}
			else if (command == "aim") {	// 2026-10-25 bench framing check
				if (List.empty()) { error = "line " + std::to_string(number) + ": an aim before the first 'shot'"; return false; }
				std::string ref; words >> ref; List.back().aimRef = strtoul(ref.c_str(), nullptr, 16); words >> List.back().aimHeight;
				if (!List.back().aimRef) { error = "line " + std::to_string(number) + ": aim needs a reference form id (hex)"; return false; }
			}
			else if (command == "require") {	// 2026-10-25 bench framing check
				if (List.empty()) { error = "line " + std::to_string(number) + ": a require before the first 'shot'"; return false; }
				std::string what; words >> what;
				if (what == "onscreen") {
					std::string ref; words >> ref; List.back().needRef = strtoul(ref.c_str(), nullptr, 16);
					float margin = 0.1f; if (words >> margin) List.back().needMargin = std::clamp(margin, 0.0f, 0.9f);
					if (!List.back().needRef) { error = "line " + std::to_string(number) + ": require onscreen needs a reference form id (hex)"; return false; }
				}
				else if (what == "detail") List.back().needDetail = true;
				else { error = "line " + std::to_string(number) + ": require onscreen <ref> [margin] | require detail"; return false; }
			}
			else if (command == "flickerwatch") {	// 2026-10-25 bench sweep mode
				FlickerWatch::Forced = true; float spike = 0.05f; if (words >> spike) FlickerWatch::SpikeThreshold = std::clamp(spike, 0.005f, 0.5f);
			}
			else if (command == "turn") { words >> TurnRate; TurnRate = std::clamp(TurnRate, -360.0f, 360.0f); }
			else if (command == "walk") { words >> WalkRate >> WalkPeriod; WalkRate = std::clamp(WalkRate, -2000.0f, 2000.0f); WalkPeriod = std::clamp(WalkPeriod, 0.1f, 60.0f); }
			else if (command == "record") { if (List.empty()) RecordAll = true; else List.back().record = true; }
			else if (command == "shot") { Shot s; words >> s.slug; if (s.slug.empty()) s.slug = "shot"; s.record = RecordAll; List.push_back(s); }
			// 2026-10-19: 'menu <section>' = this shot's picture is NVR's menu open at that page ("@NewInPatch" = the new tab),
			// taken after the menu is drawn (CaptureMenu); 'menutests' also ticks Display > "Show test settings" for it.
			// 2026-10-27: 'menu game' = the back buffer with the GAME's menus on it (race menu, Pip-Boy...), NVR's menu stays shut.
			else if (command == "menu" || command == "menutests") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a menu before the first 'shot'"; return false; }
				if (command == "menu") words >> List.back().menu;
				else List.back().menuTests = true;
			}
			else if (command == "console") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a command before the first 'shot'"; return false; }
				std::string rest;
				std::getline(words, rest);
				const size_t start = rest.find_first_not_of(" 	");
				if (start == std::string::npos) { error = "line " + std::to_string(number) + ": empty console command"; return false; }
				// 2026-10-19: coc/cow load the new cell inside the console call, i.e. inside this render-thread hook: the game hung
				// (or, before the fix below, re-entered 117 times and crashed). player.moveto <a marker in the cell> works.
				std::string head = rest.substr(start, rest.find_first_of(" 	", start) - start);
				for (char& ch : head) ch = (char)tolower((unsigned char)ch);
				if (head == "coc" || head == "cow" || head == "centeron") {
					error = "line " + std::to_string(number) + ": coc/cow hang the game from a shot; use player.moveto <a reference in that cell>";
					return false;
				}
				Change c; c.console = true; c.value = rest.substr(start);
				List.back().changes.push_back(c);
			}
			else if (command == "facemoon") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a facemoon before the first 'shot'"; return false; }
				Change c; c.console = true; c.value = "facemoon";
				List.back().changes.push_back(c);
			}
			else if (command == "wait") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a wait before the first 'shot'"; return false; }
				words >> List.back().frames;
				List.back().frames = std::clamp(List.back().frames, 2, 20000);
			}
			else if (command == "cloudcoverage") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a change before the first 'shot'"; return false; }
				Change c; c.coverage = true; words >> c.value;
				if (c.value.empty()) { error = "line " + std::to_string(number) + ": incomplete"; return false; }
				List.back().changes.push_back(c);
			}
			else if (command == "capture") {	// 2026-10-26: RenderDoc capture of this shot's picture frame (GpuDebug.h)
				if (List.empty()) { error = "line " + std::to_string(number) + ": a capture before the first 'shot'"; return false; }
				int frames = 2;
				words >> frames;
				List.back().capture = std::clamp(frames, 1, 10);
			}
			else if (command == "renderdoc") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a renderdoc before the first 'shot'"; return false; }
				List.back().renderdoc = true;
				words >> List.back().renderdocPath;
			}
			else if (command == "landflicker") {	// 2026-10-30 terrain flicker: Land below
				if (List.empty()) { error = "line " + std::to_string(number) + ": a landflicker before the first 'shot'"; return false; }
				int frames = 60;
				words >> frames;
				List.back().landFrames = std::clamp(frames, 4, 240);
			}
			else if (command == "sampler") { words >> CpuSampler::Hz; CpuSampler::Hz = std::clamp(CpuSampler::Hz, 100, 4000); }
			else if (command == "affinity") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": an affinity before the first 'shot'"; return false; }
				std::string mask;
				words >> mask;
				List.back().affinity = (DWORD_PTR)strtoull(mask.c_str(), nullptr, 16);
				if (!List.back().affinity) { error = "line " + std::to_string(number) + ": affinity needs a hex mask"; return false; }
			}
			else if (command == "inspect") {	// 2026-10-26: = set Main.Develop.PixelInspector Enabled true / X x / Y y
				if (List.empty()) { error = "line " + std::to_string(number) + ": an inspect before the first 'shot'"; return false; }
				std::string x, y;
				words >> x >> y;
				if (x.empty() || y.empty()) { error = "line " + std::to_string(number) + ": inspect needs <x> <y>"; return false; }
				const char* keys[3] = { "Enabled", "X", "Y" };
				const std::string values[3] = { "true", x, y };
				for (int k = 0; k < 3; k++) {
					Change c; c.section = "Main.Develop.PixelInspector"; c.key = keys[k]; c.value = values[k];
					List.back().changes.push_back(c);
				}
			}
			else if (command == "set" || command == "effect") {
				if (List.empty()) { error = "line " + std::to_string(number) + ": a change before the first 'shot'"; return false; }
				Change c;
				c.effect = command == "effect";
				if (c.effect) words >> c.section >> c.value;
				else words >> c.section >> c.key >> c.value;
				if (c.section.empty() || c.value.empty() || (!c.effect && c.key.empty())) { error = "line " + std::to_string(number) + ": incomplete"; return false; }
				List.back().changes.push_back(c);
			}
			else { error = "line " + std::to_string(number) + ": unknown command '" + command + "'"; return false; }
		}
		if (List.empty()) { error = "no shots in the list"; return false; }
		if (List.size() > 80) { error = "more than 80 shots"; return false; }
		return true;
	}

	inline bool Truthy(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

	// The frames the current shot waits before its picture: its own 'wait', or the list's 'frames'.
	inline int ShotFrames() { return Index < List.size() && List[Index].frames > 0 ? List[Index].frames : Frames; }

	// 2026-10-25 bench framing check: a reference's position (LookupFormByID, the game's 0x4839C0), where it is on screen, and the
	// console angles that look at it from the camera.
	inline TESObjectREFR* Reference(UInt32 id) {
		TESForm* form = CdeclCall<TESForm*>(0x4839C0, id);
		return form && form->formType >= 0x3A && form->formType <= 0x40 ? (TESObjectREFR*)form : nullptr;	// REFR 0x3A, ACHR, ACRE, the projectile refs, PFLA 0x40 (FNV form types)
	}
	inline bool OnScreen(const NiPoint3& p, float margin, float& sx, float& sy) {
		const D3DXMATRIX& v = TheRenderManager->ViewMatrix;
		const D3DXVECTOR4& c = TheRenderManager->CameraPosition;
		const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
		const float x = dx * v._11 + dy * v._21 + dz * v._31, y = dx * v._12 + dy * v._22 + dz * v._32, z = dx * v._13 + dy * v._23 + dz * v._33;
		const float aspect = TheRenderManager->height ? (float)TheRenderManager->width / (float)TheRenderManager->height : 1.7778f;
		const float th = tanf(TheRenderManager->FOVData.w * 0.5f), tv = th / aspect;	// (cameraFOV read as the horizontal one: the stricter case)
		sx = z > 1.0f ? x / (z * th) : 99.0f; sy = z > 1.0f ? y / (z * tv) : 99.0f;	// -1..1 = inside
		return z > 1.0f && fabsf(sx) <= 1.0f - margin && fabsf(sy) <= 1.0f - margin;
	}
	inline void AimAt(UInt32 id, float height) {
		TESObjectREFR* r = Reference(id);
		if (!r) { Logger::Log("SETTING SHOTS %s: aim: no reference %08X", Run, id); return; }
		const D3DXVECTOR4& c = TheRenderManager->CameraPosition;
		const float dx = r->pos.x - c.x, dy = r->pos.y - c.y, dz = r->pos.z + height - c.z;
		const float yaw = D3DXToDegree(atan2f(dx, dy)), pitch = D3DXToDegree(atan2f(-dz, sqrtf(dx * dx + dy * dy)));
		char line[96];
		sprintf_s(line, "player.setangle z %.3f", yaw < 0.0f ? yaw + 360.0f : yaw); Commands.push_back(line);
		sprintf_s(line, "player.setangle x %.3f", pitch); Commands.push_back(line);
		Moves = true;
		Logger::Log("SETTING SHOTS %s: aim at %08X (%.0f units away): yaw %.2f, pitch %.2f", Run, id, sqrtf(dx * dx + dy * dy + dz * dz), yaw, pitch);
	}

	// One shot's changes, the old values kept for Revert.
	inline void Apply(const Shot& s) {
		Commands.clear();
		Moves = false;
		FlickerWatch::ShotFrames = 0; FlickerWatch::ShotSpikes = 0; FlickerWatch::ShotMax = 0.0f; FlickerWatch::ShotMaxCell = -1; FlickerWatch::SkipFrames = 10; strncpy_s(FlickerWatch::Label, s.slug.c_str(), _TRUNCATE);	// 2026-10-25
		if (s.aimRef) AimAt(s.aimRef, s.aimHeight);	// 2026-10-25 framing (the console lines run with this shot's others)
		if (s.reload) {	// 2026-10-25: done at this frame's end (Render.cpp NiDX9Renderer__Do_EndFrame)
			TheShaderManager->ShaderReloadQueued = true;
			TheShaderManager->EffectReloadQueued = true;
			Logger::Log("SETTING SHOTS %s: shot '%s': shaders and effects compiled again from their .hlsl at this frame's end", Run, s.slug.c_str());
		}
		if (s.affinity) {	// 2026-10-21 cpulimits: a weaker CPU, this process only
			DWORD_PTR process = 0, system = 0;
			GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
			const DWORD_PTR want = (s.affinity & system) ? (s.affinity & system) : system;
			const BOOL set = SetProcessAffinityMask(GetCurrentProcess(), want);
			unsigned threads = 0, outside = 0;
			CpuSplit::CountThreadsOutside(want, threads, outside);
			Logger::Log("SETTING SHOTS %s: shot '%s': affinity %llX (was %llX, system %llX) %s; %u threads, %u still allowed outside it", Run, s.slug.c_str(),
				(unsigned long long)want, (unsigned long long)process, (unsigned long long)system, set ? "set" : "NOT set", threads, outside);
		}
		for (const Change& c : s.changes) {
			if (c.console && c.value == "facemoon") {	// 2026-10-25: look at the moon (its direction from the sky's centre)
				D3DXVECTOR3 d;
				float distance, radius;
				const char* why = "";
				if (VolumetricCloudsEffect::MoonPlace(Tes ? Tes->sky : nullptr, d, distance, radius, &why)) {
					float yaw = D3DXToDegree(atan2f(d.x, d.y));
					if (yaw < 0.0f) yaw += 360.0f;
					const float pitch = -D3DXToDegree(asinf(d.z));
					char line[64];
					sprintf_s(line, "player.setangle z %.2f", yaw); Commands.push_back(line);
					sprintf_s(line, "player.setangle x %.2f", pitch); Commands.push_back(line);
					Moves = true;
					Logger::Log("SETTING SHOTS %s: shot '%s': facemoon: yaw %.1f pitch %.1f", Run, s.slug.c_str(), yaw, pitch);
				}
				else Logger::Log("SETTING SHOTS %s: shot '%s': facemoon: %s", Run, s.slug.c_str(), why);
				continue;
			}
			if (c.console) { Commands.push_back(c.value); Moves = true; continue; }
			if (c.coverage) {
				if (VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds) clouds->TestSetCoverage((float)atof(c.value.c_str()));
				Logger::Log("SETTING SHOTS %s: shot '%s': cloud coverage set to %s (eases back to the weather's)", Run, s.slug.c_str(), c.value.c_str());
				continue;
			}
			if (c.effect) {
				const bool want = Truthy(c.value);
				if (TheSettingManager->GetMenuShaderEnabled(c.section.c_str()) != want) {
					Undo u; u.effect = true; u.section = c.section; Undos.push_back(u);
					TheShaderManager->SwitchShaderStatus(c.section.c_str());
				}
				Logger::Log("SETTING SHOTS %s: shot '%s': effect %s %s", Run, s.slug.c_str(), c.section.c_str(), want ? "on" : "off");
				continue;
			}
			SettingManager::Configuration::ConfigNode node;
			if (!TheSettingManager->Config.FillNode(&node, c.section.c_str(), c.key.c_str())) {
				Logger::Log("SETTING SHOTS %s: shot '%s': no setting %s %s (skipped)", Run, s.slug.c_str(), c.section.c_str(), c.key.c_str());
				continue;
			}
			Undo u; u.section = c.section; u.key = c.key; u.type = node.Type; u.b = node.BoolValue; u.i = node.IntValue; u.f = node.FloatValue;
			u.s = node.LongValue.empty() ? node.Value : node.LongValue;
			Undos.push_back(u);
			switch (node.Type) {
			case SettingManager::Configuration::Boolean: TheSettingManager->SetSetting(c.section.c_str(), c.key.c_str(), Truthy(c.value)); break;
			case SettingManager::Configuration::Integer: TheSettingManager->SetSetting(c.section.c_str(), c.key.c_str(), atoi(c.value.c_str())); break;
			case SettingManager::Configuration::String: TheSettingManager->SetSettingS(c.section.c_str(), c.key.c_str(), c.value.substr(0, 79).c_str()); break;	// 2026-10-18 (Value holds 79)
			default: TheSettingManager->SetSetting(c.section.c_str(), c.key.c_str(), (float)atof(c.value.c_str())); break;
			}
			Logger::Log("SETTING SHOTS %s: shot '%s': %s %s = %s (was %s)", Run, s.slug.c_str(), c.section.c_str(), c.key.c_str(), c.value.c_str(), node.Value);
		}
		const bool changes = std::any_of(s.changes.begin(), s.changes.end(), [](const Change& c) { return !c.console && !c.coverage; });
		if (s.keep) { Kept.insert(Kept.end(), Undos.begin(), Undos.end()); Undos.clear(); }
		if (changes || !NoReload) TheSettingManager->LoadSettings();
		else Logger::Log("SETTING SHOTS %s: shot '%s': no changes, no settings reload (noreload)", Run, s.slug.c_str());
		if (!s.menu.empty() && s.menu != "game") ImGuiManager::TestShowMenu(s.menu.c_str(), s.menuTests);
	}

	// Every change of the shot undone, newest first.
	inline void Revert() {
		if (Index < List.size() && !List[Index].menu.empty() && List[Index].menu != "game") ImGuiManager::TestShowMenu(nullptr, false);	// 2026-10-19: menu shots
		for (auto u = Undos.rbegin(); u != Undos.rend(); ++u) {
			if (u->effect) { TheShaderManager->SwitchShaderStatus(u->section.c_str()); continue; }
			switch (u->type) {
			case SettingManager::Configuration::Boolean: TheSettingManager->SetSetting(u->section.c_str(), u->key.c_str(), u->b); break;
			case SettingManager::Configuration::Integer: TheSettingManager->SetSetting(u->section.c_str(), u->key.c_str(), u->i); break;
			case SettingManager::Configuration::String: TheSettingManager->SetSettingS(u->section.c_str(), u->key.c_str(), u->s.substr(0, 79).c_str()); break;
			default: TheSettingManager->SetSetting(u->section.c_str(), u->key.c_str(), u->f); break;
			}
		}
		const bool any = !Undos.empty();
		Undos.clear();
		if (any) TheSettingManager->LoadSettings();
	}

	// 'keep' changes go back with the last shot's (oldest first in Undos: Revert undoes newest first).
	inline void RestoreKept() {
		Undos.insert(Undos.begin(), Kept.begin(), Kept.end());
		Kept.clear();
	}

	inline void Stop(const char* why) {
		Logger::Log("SETTING SHOTS %s: stopped, %s", Run, why);
		FrameRecorder::Journal::Stop();
		GpuTimer::MarkerHook = nullptr;
		RestoreKept();
		Revert();
		Running = false;
		Notify(kLinkStopped, why);
	}

	inline DWORD WINAPI Worker(LPVOID parameter) {
		Job* job = (Job*)parameter;
		const bool saved = SelfTestCore::SavePng(job->path, job->width, job->height, job->bgra);
		Logger::Log("SETTING SHOTS: %s %s (%ux%u)", job->path, saved ? "saved" : "NOT saved", job->width, job->height);
		delete job;
		InFlight--;
		return 0;
	}

	inline void ReleaseWorld() {
		if (WorldResolve) { WorldResolve->Release(); WorldResolve = nullptr; }
		if (WorldReadback) { WorldReadback->Release(); WorldReadback = nullptr; }
		WorldDesc = {};
	}

	// UNOFFICIAL 2026-10-16 'world' (foundations study: how big the lamp-pass error is): Render.cpp calls this at the start of the
	// game's image space, right after BeginFrame, with the game's scene buffer (fp16): the finished world and first-person draw,
	// before NVR's effects and the tonemapper. In the frame whose picture is the shot, it is saved raw next to the picture:
	// NeonSelfTest_<run>_ab_<NN>_<slug>.world = "NVRW", then width, height, D3DFORMAT and bytes per pixel (UInt32 each), then the
	// rows. A test-only stall (the GPU is waited for); nothing of it runs in normal play.
	inline void SaveTarget(IDirect3DDevice9* device, IDirect3DSurface9* target, const char* suffix);
	inline void CaptureWorld(IDirect3DDevice9* device, IDirect3DSurface9* target) {
		if (!Running || !WorldCapture || !device || !target || Index >= List.size() || WorldTaken == Index || Frame < ShotFrames()) return;
		WorldTaken = Index;
		SaveTarget(device, target, "");
	}
	// UNOFFICIAL 2026-11-02 (colour lift with the effects before FSR / DLSS): with 'world', also the scene buffer right before the game's image
	// space, after NVR's pre-tonemap effects and the RenderScale upscale (Render.cpp), saved as <slug>-preis.world.
	inline void CapturePreImageSpace(IDirect3DDevice9* device, IDirect3DSurface9* target) {
		if (!Running || !WorldCapture || !device || !target || Index >= List.size() || PreImageSpaceTaken == Index || Frame < ShotFrames()) return;
		PreImageSpaceTaken = Index;
		SaveTarget(device, target, "-preis");
	}
	// UNOFFICIAL 2026-10-18 'stage' (ambfix: the AmbientLight tint measured in linear light): ShaderManager calls this right after the
	// AmbientLight step with the current picture (TextureManager::RenderedSurface); saved as <slug>-al.world in the shot's last frame.
	inline void CaptureStage(IDirect3DDevice9* device, IDirect3DSurface9* target) {
		if (!Running || !StageCapture || !device || !target || Index >= List.size() || StageTaken == Index || Frame < ShotFrames()) return;
		StageTaken = Index;
		SaveTarget(device, target, "-al");
	}
	inline void SaveTarget(IDirect3DDevice9* device, IDirect3DSurface9* target, const char* suffix) {
		const char* slug = List[Index].slug.c_str();
		target->AddRef();
		D3DSURFACE_DESC d = {};
		target->GetDesc(&d);
		const UInt32 bpp = d.Format == D3DFMT_A16B16G16R16F ? 8 : d.Format == D3DFMT_A32B32G32R32F ? 16 :
			(d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A2R10G10B10 || d.Format == D3DFMT_A2B10G10R10) ? 4 : 0;
		bool ok = bpp != 0;
		if (ok && (!WorldReadback || WorldDesc.Width != d.Width || WorldDesc.Height != d.Height || WorldDesc.Format != d.Format || WorldDesc.MultiSampleType != d.MultiSampleType)) {
			ReleaseWorld();
			ok = (d.MultiSampleType == D3DMULTISAMPLE_NONE || SUCCEEDED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &WorldResolve, NULL))) &&
				SUCCEEDED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &WorldReadback, NULL));
			if (ok) WorldDesc = d;
		}
		IDirect3DSurface9* source = target;
		if (ok && WorldResolve) { ok = SUCCEEDED(device->StretchRect(target, NULL, WorldResolve, NULL, D3DTEXF_NONE)); source = WorldResolve; }
		ok = ok && SUCCEEDED(device->GetRenderTargetData(source, WorldReadback));
		D3DLOCKED_RECT lock = {};
		ok = ok && SUCCEEDED(WorldReadback->LockRect(&lock, NULL, D3DLOCK_READONLY));
		char path[MAX_PATH];
		sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\NeonSelfTest_%s_ab_%02u_%s%s.world", Run, (unsigned)Index, slug, suffix);
		bool saved = false;
		if (ok) {
			FILE* f = nullptr;
			if (!fopen_s(&f, path, "wb") && f) {
				const UInt32 header[5] = { 0x5752564E, d.Width, d.Height, (UInt32)d.Format, bpp };	// "NVRW"
				saved = fwrite(header, sizeof(header), 1, f) == 1;
				for (UInt32 y = 0; saved && y < d.Height; y++) saved = fwrite((const unsigned char*)lock.pBits + (size_t)y * lock.Pitch, (size_t)d.Width * bpp, 1, f) == 1;
				fclose(f);
			}
			WorldReadback->UnlockRect();
		}
		target->Release();
		Logger::Log("SETTING SHOTS %s: shot '%s': world target %ux%u format %u msaa %u -> %s %s", Run, slug, d.Width, d.Height, (unsigned)d.Format,
			(unsigned)d.MultiSampleType, path, saved ? "saved" : "NOT saved");
	}

	// UNOFFICIAL 2026-10-30 'landflicker <frames>' (terrain flicker deep dive, docs/next/TERRAIN-FLICKER.md): the shot's last <frames>
	// frames before its picture are a still-camera flicker measurement of the LAND only. Each of them, the finished picture (before the
	// HUD) is halved to at most 640 wide and read back as luma (three surfaces in turn: no stall); then one frame (three before the
	// picture) draws every land shader (the TerrainShaders templates: land, LOD land, fade) flat magenta, which marks the land pixels.
	// Logged per shot ('LAND FLICKER' lines): the land's share of the picture, the per-pixel temporal standard deviation over the frames
	// (8-bit levels: mean, p95, p99; pixels over 2 and 5 levels), one-frame spikes (a pixel that jumps 10+ levels and comes back the next
	// frame), the land's mean brightness frame by frame (a whole-ground pulse), by screen thirds (far / middle / near rows), and the same
	// for the pixels that are not land. A heatmap is saved next to the picture (<slug>-landstd.png: land coloured by its deviation, the
	// rest dim grey). The camera must be still (no 'turn' / 'walk'): every change of a pixel counts.
	namespace Land {
		inline UInt32 W = 0, H = 0;
		inline int Frames = 0, First = 0, MaskAt = 0;	// this shot's: measured frames, the shot frame of the first, the mask frame
		inline bool Active = false, MaskNow = false, Failed = false;
		inline IDirect3DSurface9* Rt[3] = {};
		inline IDirect3DSurface9* Mem[3] = {};
		inline int SlotFrame[3] = { -1, -1, -1 };
		inline D3DFORMAT Format = D3DFMT_UNKNOWN;
		inline std::vector<unsigned char> Stack;		// Frames x W x H luma
		inline std::vector<unsigned> Reference, Mask;	// BGRA: the last measured frame, the mask frame
		inline bool HaveMask = false;
		inline IDirect3DPixelShader9* Magenta = nullptr;
		inline unsigned Replaced = 0;

		inline void Release() {
			for (int i = 0; i < 3; i++) {
				if (Rt[i]) { Rt[i]->Release(); Rt[i] = nullptr; }
				if (Mem[i]) { Mem[i]->Release(); Mem[i] = nullptr; }
				SlotFrame[i] = -1;
			}
			Format = D3DFMT_UNKNOWN;
		}

		// SettingShots::BeginFrame, after Frame++ (frame: 1 .. total, the picture at total).
		inline void OnShotFrame(int frame, int total, int frames) {
			if (frames <= 0) { Active = MaskNow = false; return; }
			if (frame == 1) {
				Frames = (std::min)(frames, (std::max)(total - 12, 0));
				MaskAt = total - 3;
				First = MaskAt - Frames;
				Active = Frames >= 4;
				HaveMask = false; Replaced = 0;
				for (int& s : SlotFrame) s = -1;
				if (!Active) Logger::Log("LAND FLICKER %s: shot '%s' too short for landflicker %d (wait %d)", Run, List[Index].slug.c_str(), frames, total);
			}
			// BeginFrame runs in image space, after the world of this frame is drawn: the flag set here colours the NEXT frame's
			// land, whose picture is frame MaskAt (run 20261030-141538: set at MaskAt itself, no picture ever had the land masked).
			MaskNow = Active && frame == MaskAt - 1;
		}

		// SetShadersHook, after the game bound a pass's shaders: in the mask frame a land shader is swapped for flat magenta.
		inline void OnBind(IDirect3DDevice9* device, const char* ps, bool world) {
			if (!MaskNow || !Running || !world || !ps || Failed || !TheShaderManager->Shaders.Terrain) return;
			if (!TheShaderManager->Shaders.Terrain->GetTemplate(ps).Name) return;
			if (!Magenta) {
				const char* src = "float4 main() : COLOR0 { return float4(1.0, 0.0, 1.0, 1.0); }";
				ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
				if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
					FAILED(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &Magenta))) {
					Failed = true;
					Logger::Log("LAND FLICKER: the mask shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
				}
				if (errors) errors->Release();
				if (code) code->Release();
				if (!Magenta) return;
			}
			device->SetPixelShader(Magenta);
			Replaced++;
		}

		inline unsigned char Luma(float r, float g, float b) {
			const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			return (unsigned char)(std::clamp(l, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		// Reads slot i (BGRA into out when asked, luma into luma when asked).
		inline bool Read(int i, unsigned char* luma, std::vector<unsigned>* out) {
			D3DLOCKED_RECT lock = {};
			if (FAILED(TheRenderManager->device->GetRenderTargetData(Rt[i], Mem[i])) || FAILED(Mem[i]->LockRect(&lock, NULL, D3DLOCK_READONLY))) return false;
			if (out) out->resize((size_t)W * H);
			for (UInt32 y = 0; y < H; y++) {
				const unsigned char* row = (const unsigned char*)lock.pBits + (size_t)y * lock.Pitch;
				for (UInt32 x = 0; x < W; x++) {
					float r, g, b;
					if (Format == D3DFMT_A16B16G16R16F) {
						float v[4];
						D3DXFloat16To32Array(v, (const D3DXFLOAT16*)(row + x * 8), 4);
						r = powf((std::max)(v[0], 0.0f), 1.0f / 2.2f); g = powf((std::max)(v[1], 0.0f), 1.0f / 2.2f); b = powf((std::max)(v[2], 0.0f), 1.0f / 2.2f);
					}
					else { const unsigned char* p = row + x * 4; b = p[0] / 255.0f; g = p[1] / 255.0f; r = p[2] / 255.0f; }
					if (luma) luma[(size_t)y * W + x] = Luma(r, g, b);
					if (out) {
						const unsigned R = (unsigned)(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f), G = (unsigned)(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f),
							B = (unsigned)(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
						(*out)[(size_t)y * W + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
					}
				}
			}
			Mem[i]->UnlockRect();
			return true;
		}

		inline void Analyse();

		// Render.cpp, the finished picture before the HUD, BEFORE SettingShots::Capture (which may end the shot).
		inline void OnPicture(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
			if (!Running || !Active || Failed || !surface || Index >= List.size()) return;
			if (Frame < First || Frame > MaskAt + 2) return;
			D3DSURFACE_DESC d = {};
			surface->GetDesc(&d);
			if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A16B16G16R16F) {
				Failed = true;
				Logger::Log("LAND FLICKER: picture format %d not handled, off", (int)d.Format);
				return;
			}
			if (!Rt[0] || d.Format != Format) {
				Release();
				W = (std::min)(d.Width / 2, 640u); H = (UInt32)((float)W * d.Height / d.Width);
				for (int i = 0; i < 3; i++)
					if (FAILED(device->CreateRenderTarget(W, H, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &Rt[i], NULL)) ||
						FAILED(device->CreateOffscreenPlainSurface(W, H, d.Format, D3DPOOL_SYSTEMMEM, &Mem[i], NULL))) {
						Release();
						Failed = true;
						Logger::Log("LAND FLICKER: could not make the read-back surfaces, off");
						return;
					}
				Format = d.Format;
			}
			if (Frame == First) { Stack.assign((size_t)Frames * W * H, 0); Reference.clear(); Mask.clear(); }
			// the frame written two frames ago
			const int r = (Frame + 1) % 3;
			if (SlotFrame[r] >= First && SlotFrame[r] <= MaskAt) {
				const int f = SlotFrame[r];
				if (f == MaskAt) { HaveMask = Read(r, nullptr, &Mask); Analyse(); }
				else Read(r, &Stack[(size_t)(f - First) * W * H], f == MaskAt - 1 ? &Reference : nullptr);
				SlotFrame[r] = -1;
			}
			if (Frame <= MaskAt) {
				const int w = Frame % 3;
				SlotFrame[w] = SUCCEEDED(device->StretchRect(surface, NULL, Rt[w], NULL, D3DTEXF_LINEAR)) ? Frame : -1;
			}
		}

		inline void Analyse() {
			const Shot& shot = List[Index];
			if (!HaveMask || Reference.size() != (size_t)W * H) { Logger::Log("LAND FLICKER %s: shot '%s': no mask frame read, nothing measured", Run, shot.slug.c_str()); return; }
			const size_t n = (size_t)W * H;
			// land: magenta in the mask frame and clearly changed from the frame before it; eroded by one pixel (object edges)
			std::vector<unsigned char> raw(n, 0), land(n, 0);
			for (size_t i = 0; i < n; i++) {
				const int r = (Mask[i] >> 16) & 255, g = (Mask[i] >> 8) & 255, b = Mask[i] & 255;
				const int r0 = (Reference[i] >> 16) & 255, g0 = (Reference[i] >> 8) & 255, b0 = Reference[i] & 255;
				// (TAA / FSR blend the mask frame with their history: the magenta comes out weaker than drawn)
				raw[i] = (r - g) - (r0 - g0) > 25 && (b - g) - (b0 - g0) > 25;
			}
			for (UInt32 y = 1; y + 1 < H; y++) for (UInt32 x = 1; x + 1 < W; x++) {
				const size_t i = (size_t)y * W + x;
				land[i] = raw[i] && raw[i - 1] && raw[i + 1] && raw[i - W] && raw[i + W];
			}
			struct Group { double sum = 0.0; size_t count = 0, over2 = 0, over5 = 0; std::vector<float> stds; double spikes = 0.0; double third[3] = {}; size_t thirdCount[3] = {}; };
			Group g[2];	// 0 land, 1 the rest
			std::vector<float> std_(n, 0.0f);
			std::vector<double> landMean(Frames, 0.0);
			size_t landCount = 0;
			for (size_t i = 0; i < n; i++) {
				double s = 0.0, s2 = 0.0;
				unsigned spikes = 0;
				for (int f = 0; f < Frames; f++) {
					const double v = Stack[(size_t)f * n + i];
					s += v; s2 += v * v;
					if (land[i]) landMean[f] += v;
					if (f >= 2) {
						const int a = Stack[(size_t)(f - 2) * n + i], b = Stack[(size_t)(f - 1) * n + i], c = Stack[(size_t)f * n + i];
						if (abs(b - a) >= 10 && abs(c - b) >= 10 && (b - a) * (c - b) < 0) spikes++;
					}
				}
				const double mean = s / Frames, var = (std::max)(s2 / Frames - mean * mean, 0.0);
				const float sd = (float)sqrt(var);
				std_[i] = sd;
				Group& G = g[land[i] ? 0 : 1];
				G.sum += sd; G.count++; G.stds.push_back(sd); G.spikes += spikes;
				if (sd > 2.0f) G.over2++;
				if (sd > 5.0f) G.over5++;
				const int t = (int)((size_t)(i / W) * 3 / H);
				G.third[t] += sd; G.thirdCount[t]++;
				if (land[i]) landCount++;
			}
			auto pct = [](std::vector<float>& v, float p) { if (v.empty()) return 0.0f; const size_t k = (size_t)(p * (v.size() - 1)); std::nth_element(v.begin(), v.begin() + k, v.end()); return v[k]; };
			char line[2][400];
			for (int k = 0; k < 2; k++) {
				Group& G = g[k];
				const double c = (std::max)((double)G.count, 1.0);
				const float p95 = pct(G.stds, 0.95f), p99 = pct(G.stds, 0.99f);
				sprintf_s(line[k], "std mean %.2f p95 %.2f p99 %.2f levels; over 2 levels %.2f%%, over 5 %.2f%%; spikes %.2f per 1000 pixel-frames; rows far/mid/near std %.2f/%.2f/%.2f",
					G.sum / c, p95, p99, 100.0 * G.over2 / c, 100.0 * G.over5 / c, 1000.0 * G.spikes / (c * (std::max)(Frames - 2, 1)),
					G.third[0] / (std::max)((double)G.thirdCount[0], 1.0), G.third[1] / (std::max)((double)G.thirdCount[1], 1.0), G.third[2] / (std::max)((double)G.thirdCount[2], 1.0));
			}
			// the land's mean brightness frame by frame: a pulse of the whole ground
			std::string series;
			double lo = 1e9, hi = -1e9, sum = 0.0;
			for (int f = 0; f < Frames; f++) {
				landMean[f] /= (std::max)((double)landCount, 1.0);
				lo = (std::min)(lo, landMean[f]); hi = (std::max)(hi, landMean[f]); sum += landMean[f];
				char v[16]; sprintf_s(v, " %.1f", landMean[f]); series += v;
			}
			const double avg = sum / (std::max)(Frames, 1);
			double step = 0.0;
			for (int f = 1; f < Frames; f++) step = (std::max)(step, fabs(landMean[f] - landMean[f - 1]));
			Logger::Log("LAND FLICKER %s: shot '%s': %d frames %ux%u, land %.1f%% of the picture (%u land draws masked)", Run, shot.slug.c_str(), Frames, W, H,
				100.0 * landCount / n, Replaced);
			Logger::Log("LAND FLICKER %s: shot '%s': land: %s", Run, shot.slug.c_str(), line[0]);
			Logger::Log("LAND FLICKER %s: shot '%s': rest: %s", Run, shot.slug.c_str(), line[1]);
			Logger::Log("LAND FLICKER %s: shot '%s': land mean %.2f, range %.2f%%, largest frame step %.2f%%; frames:%s", Run, shot.slug.c_str(), avg,
				avg > 0.0 ? 100.0 * (hi - lo) / avg : 0.0, avg > 0.0 ? 100.0 * step / avg : 0.0, series.c_str());
			// heatmap: land by its deviation (0 black, 2 blue, 5 yellow, 10+ red), the rest dim grey
			Job* job = new (std::nothrow) Job();
			if (!job) return;
			job->width = W; job->height = H; job->bgra.resize(n);
			for (size_t i = 0; i < n; i++) {
				if (!land[i]) { const unsigned v = Stack[(size_t)(Frames - 1) * n + i] / 4; job->bgra[i] = 0xFF000000u | (v << 16) | (v << 8) | v; continue; }
				const float s = std_[i];
				float r, gg, b;
				if (s < 2.0f) { r = 0.0f; gg = 0.0f; b = 0.25f + 0.75f * s / 2.0f; }
				else if (s < 5.0f) { const float t = (s - 2.0f) / 3.0f; r = t; gg = t; b = 1.0f - t; }
				else { const float t = (std::min)((s - 5.0f) / 5.0f, 1.0f); r = 1.0f; gg = 1.0f - t; b = 0.0f; }
				job->bgra[i] = 0xFF000000u | ((unsigned)(r * 255) << 16) | ((unsigned)(gg * 255) << 8) | (unsigned)(b * 255);
			}
			sprintf_s(job->path, ".\\" VS_LOGS_FOLDER "\\NeonSelfTest_%s_ab_%02u_%s-landstd.png", Run, (unsigned)Index, shot.slug.c_str());
			InFlight++;
			HANDLE thread = CreateThread(nullptr, 0, Worker, job, 0, nullptr);
			if (thread) CloseHandle(thread);
			else { InFlight--; delete job; }
			// the mask frame as read (what was taken as land: the check for this measurement)
			if (Job* mask = new (std::nothrow) Job()) {
				mask->width = W; mask->height = H; mask->bgra = Mask;
				for (size_t i = 0; i < n; i++) if (land[i]) mask->bgra[i] = 0xFFFF00FFu;
				sprintf_s(mask->path, ".\\" VS_LOGS_FOLDER "\\NeonSelfTest_%s_ab_%02u_%s-landmask.png", Run, (unsigned)Index, shot.slug.c_str());
				InFlight++;
				HANDLE t2 = CreateThread(nullptr, 0, Worker, mask, 0, nullptr);
				if (t2) CloseHandle(t2);
				else { InFlight--; delete mask; }
			}
			Stack.clear(); Stack.shrink_to_fit();
		}
	}

	inline void QueueJob(Job* job);
	inline void NextShot();

	// The shot's console commands. 2026-10-19: taken out of the list BEFORE running. A 'coc' draws loading-screen frames inside
	// RunScriptLine, which come back to BeginFrame; with the list still full every one of them ran the coc again (117 nested coc calls,
	// then a crash).
	inline int WalkPause = 0;	// 'walk' waits this many main-loop ticks after the list's own commands (a moveto / setpos it would undo)
	inline void RunCommands() {
		if (Commands.empty()) return;
		WalkPause = 30;
		std::vector<std::string> lines;
		lines.swap(Commands);
		for (const std::string& line : lines) {
			Logger::Log("SETTING SHOTS %s: shot '%s': console %s", Run, List[Index].slug.c_str(), line.c_str());
			if (g_ConsoleInterface) g_ConsoleInterface->RunScriptLine(line.c_str(), nullptr);
		}
	}
	// 2026-10-20 (cloth C3-18: two test games froze for good at a 'placeatme'): BeginFrame runs inside the game's render, which holds the
	// renderer's lock (NiDX9Renderer + 0x80); placeatme then waits for the game's loading lock (0x011D5A9C), and a loading thread that
	// holds that one and is creating a mesh waits for the renderer's lock: a deadlock (stacks in evidence\clothauto\run\freeze-B4c).
	// So the commands run here, on xNVSE's main-loop message (Main.cpp), between frames, where the game runs its own scripts.
	inline void MainLoop() {
		if (!MainLoopSeen) { MainLoopSeen = true; Logger::Log("SETTING SHOTS: console commands run on the game's main loop (between frames)"); }
		if (Running) RunCommands();
		static ULONGLONG lastTurn = 0;
		static float turnAngle = 0.0f;
		if (Running && TurnRate != 0.0f && g_ConsoleInterface && Player) {
			const ULONGLONG now = GetTickCount64();
			if (!lastTurn) turnAngle = D3DXToDegree(Player->rot.z);
			else turnAngle = fmodf(turnAngle + TurnRate * (float)min(now - lastTurn, 100ULL) / 1000.0f, 360.0f);
			lastTurn = now;
			char line[64];
			sprintf_s(line, "player.setangle z %.3f", turnAngle < 0.0f ? turnAngle + 360.0f : turnAngle);
			g_ConsoleInterface->RunScriptLine(line, nullptr);
		}
		else lastTurn = 0;
		// 'walk' (2026-10-30): forward along the facing, back the same way after WalkPeriod seconds (stays in the same place over a run)
		static ULONGLONG lastWalk = 0;
		static float walkClock = 0.0f;
		if (WalkPause > 0) { WalkPause--; lastWalk = 0; }
		else if (Running && WalkRate != 0.0f && g_ConsoleInterface && Player) {
			const ULONGLONG now = GetTickCount64();
			const float dt = lastWalk ? (float)min(now - lastWalk, 100ULL) / 1000.0f : 0.0f;
			lastWalk = now;
			walkClock = fmodf(walkClock + dt, 2.0f * WalkPeriod);
			const float step = (walkClock < WalkPeriod ? WalkRate : -WalkRate) * dt;
			if (step != 0.0f) {
				char line[64];
				sprintf_s(line, "player.setpos x %.3f", Player->pos.x + sinf(Player->rot.z) * step);
				g_ConsoleInterface->RunScriptLine(line, nullptr);
				sprintf_s(line, "player.setpos y %.3f", Player->pos.y + cosf(Player->rot.z) * step);
				g_ConsoleInterface->RunScriptLine(line, nullptr);
			}
		}
		else { lastWalk = 0; walkClock = 0.0f; }
	}

	// 'renderdoc' (2026-10-26): RenderDoc's in-application API (renderdoc_app.h, RENDERDOC_API_1_1_2: the entries up to TriggerCapture),
	// when RenderDoc is in the process (its renderdoc.dll: under DXVK the Vulkan layer). The capture is the next whole frame.
	struct RenderDocApi {
		void* getVersion; void* setOptionU32; void* setOptionF32; void* getOptionU32; void* getOptionF32; void* setFocusKeys; void* setCaptureKeys;
		void* getOverlay; void* maskOverlay; void* removeHooks; void* unloadCrashHandler;
		void (__cdecl* SetCaptureFilePathTemplate)(const char* pathTemplate);
		void* getPathTemplate; void* getNumCaptures; void* getCapture;
		void (__cdecl* TriggerCapture)();
	};
	inline void TriggerRenderDoc(const Shot& s) {
		static RenderDocApi* api = nullptr;
		if (!api) {
			typedef int (__cdecl* GetApiFn)(int version, void** api);
			HMODULE module = GetModuleHandleA("renderdoc.dll");
			GetApiFn getApi = module ? (GetApiFn)GetProcAddress(module, "RENDERDOC_GetAPI") : nullptr;
			if (!getApi || !getApi(10102, (void**)&api) || !api) {
				api = nullptr;
				Logger::Log("SETTING SHOTS %s: shot '%s': no RenderDoc in the game (under DXVK: ENABLE_VULKAN_RENDERDOC_CAPTURE=1 for the launch), no capture", Run, s.slug.c_str());
				return;
			}
		}
		char path[MAX_PATH];
		if (!s.renderdocPath.empty()) strcpy_s(path, s.renderdocPath.c_str());
		else sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\RenderDoc_%s_%s", Run, s.slug.c_str());
		api->SetCaptureFilePathTemplate(path);
		api->TriggerCapture();
		Logger::Log("SETTING SHOTS %s: shot '%s': RenderDoc capture of the next frame asked (%s_frame<N>.rdc)", Run, s.slug.c_str(), path);
	}

	// Render.cpp, right after ShaderManager::UpdateConstants and EffectSelfTest::BeginFrame.
	inline void BeginFrame() {
		if (AllQueued && InFlight == 0) {
			AllQueued = false;
			Reader.Release();
			ReleaseWorld();
			FrameRecorder::Journal::Stop();	// 'record': the upload journal is not needed any more
			Logger::Log("SETTING SHOTS %s: done, every picture written", Run);
			Notify(kLinkPictures, Run);
		}
		if (Pending && !Running) {
			Pending = false;
			SYSTEMTIME t; GetLocalTime(&t);
			sprintf_s(Run, "%02u-%02u-%02u", (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond);
			std::string error;
			if (!Player || !Player->parentCell) { Notify(kLinkStopped, "no game loaded"); return; }
			if (!Parse(error)) { Logger::Log("SETTING SHOTS %s: %s", Run, error.c_str()); Notify(kLinkStopped, error.c_str()); return; }
			Running = true; Index = 0; Frame = 0; Cell = Player->parentCell;
			CpuSampler::Start();	// 2026-10-21 cpulimits: only with a 'sampler' line
			Logger::Log("SETTING SHOTS %s: started, %u shots, %d frames each (settings changed in memory only, never saved)", Run, (unsigned)List.size(), Frames);
			Notify(kLinkStarted, Run);
			Apply(List[0]);
			return;
		}
		if (!Running) return;
		if (FinalState.load() == 3) {	// 2026-10-18: a 'final' picture taken on ReShade's Vulkan runtime
			Job* job = FinalJob;
			FinalJob = nullptr;
			FinalState.store(0);
			LogCaptured(FinalFrame.load(), "the presented frame, on the Vulkan runtime");
			QueueJob(job);
			NextShot();
			if (!Running) return;
		}
		if (!MainLoopSeen) RunCommands();	// (an xNVSE without the main-loop message: here, as before)
		if ((Moves || WalkRate != 0.0f) && Player) Cell = Player->parentCell;	// 2026-11-09 grass-cache: an outdoor 'walk' crosses cells
		if (!Player || Player->parentCell != Cell) { Stop("the player changed place"); return; }
		if (TheRenderManager->device->TestCooperativeLevel() != D3D_OK) { Reader.Release(); ReleaseWorld(); Stop("the device was lost"); return; }
		Frame++;
		Land::OnShotFrame(Frame, ShotFrames(), List[Index].landFrames);	// 2026-10-30 'landflicker'
		// 2026-10-26 'capture': asked one frame ahead, so RenderDoc's next frame (from the next present) is the picture frame.
		if (List[Index].capture && Frame == ShotFrames() - 1) {
			char why[96];
			sprintf_s(why, "shot %u '%s' of run %s", (unsigned)Index, List[Index].slug.c_str(), Run);
			GpuDebug::RequestCapture(List[Index].capture, why);
		}
		if (List[Index].renderdoc && Frame == ShotFrames() - 2) TriggerRenderDoc(List[Index]);	// the captured frame is still this shot's
	}

	// The picture of the current shot: named after it and written on a worker thread.
	inline void QueueJob(Job* job) {
		const Shot& shot = List[Index];
		sprintf_s(job->path, ".\\" VS_LOGS_FOLDER "\\NeonSelfTest_%s_ab_%02u_%s.png", Run, (unsigned)Index, shot.slug.c_str());
		InFlight++;
		HANDLE thread = CreateThread(nullptr, 0, Worker, job, 0, nullptr);
		if (thread) CloseHandle(thread);
		else { InFlight--; delete job; Logger::Log("SETTING SHOTS %s: shot '%s' not written: no thread", Run, shot.slug.c_str()); }
	}

	inline void NextShot() {
		if (FlickerWatch::Forced && Index < List.size())	// 2026-10-25 bench sweep mode: this shot's flicker score
			Logger::Log("FLICKER SHOT '%s': %u frames, %u spikes, max %.2f%% (cell %d)", List[Index].slug.c_str(), FlickerWatch::ShotFrames, FlickerWatch::ShotSpikes,
				FlickerWatch::ShotMax * 100.0f, FlickerWatch::ShotMaxCell);
		if (Index + 1 >= List.size()) RestoreKept();
		Revert();
		Index++;
		Frame = 0;
		if (Index < List.size()) Apply(List[Index]);
		else { Running = false; AllQueued = true; FlickerWatch::Forced = false; }
	}

	// 2026-10-25 'require detail': the luma spread of the picture's middle third (0-255).
	inline float MiddleSpread(const Job* job) {
		double sum = 0, sum2 = 0; unsigned n = 0;
		for (unsigned y = job->height / 3; y < job->height * 2 / 3; y += 2)
			for (unsigned x = job->width / 3; x < job->width * 2 / 3; x += 2) {
				const unsigned p = job->bgra[(size_t)y * job->width + x];
				const double l = 0.0722 * (p & 255) + 0.7152 * ((p >> 8) & 255) + 0.2126 * ((p >> 16) & 255);
				sum += l; sum2 += l * l; n++;
			}
		return n ? (float)sqrt((std::max)(0.0, sum2 / n - (sum / n) * (sum / n))) : 0.0f;
	}

	// 2026-10-25 'require onscreen': checked when the shot's frames are done; once: aim at it and 60 more frames; then -OFFFRAME.
	inline bool FramingHolds() {
		Shot& shot = List[Index];
		if (!shot.needRef) return true;
		TESObjectREFR* r = Reference(shot.needRef);
		float sx = 0, sy = 0;
		if (r && OnScreen(r->pos, shot.needMargin, sx, sy)) {
			if (shot.retried) Logger::Log("SETTING SHOTS %s: FRAMING shot '%s': %08X on screen after the retry (%.2f, %.2f)", Run, shot.slug.c_str(), shot.needRef, sx, sy);
			return true;
		}
		if (!shot.retried && r) {
			shot.retried = true;
			Logger::Log("SETTING SHOTS %s: FRAMING shot '%s': %08X off screen (%.2f, %.2f): aiming at it, 60 more frames", Run, shot.slug.c_str(), shot.needRef, sx, sy);
			AimAt(shot.needRef, shot.aimHeight);
			Frame = (std::max)(0, ShotFrames() - 60);
			return false;
		}
		Logger::Log("SETTING SHOTS %s: FRAMING shot '%s': %08X %s: saved as -OFFFRAME", Run, shot.slug.c_str(), shot.needRef, r ? "still off screen" : "is not a loaded reference");
		shot.slug += "-OFFFRAME";
		return true;
	}

	inline void TakeShot(IDirect3DSurface9* surface) {
		Job* job = new (std::nothrow) Job();
		if (job && Reader.Read(TheRenderManager->device, surface, job->bgra, job->width, job->height)) {
			LogCaptured(TheFrameRateManager->FrameNumber, "this frame's picture");
			if (List[Index].needDetail && !job->bgra.empty()) {
				const float spread = MiddleSpread(job);
				if (spread < 4.0f) { Logger::Log("SETTING SHOTS %s: FRAMING shot '%s': the middle of the picture is flat (luma spread %.2f < 4): saved as -FLAT", Run, List[Index].slug.c_str(), spread); List[Index].slug += "-FLAT"; }
			}
			QueueJob(job);
		}
		else {
			Logger::Log("SETTING SHOTS %s: shot '%s' NOT taken: %s", Run, List[Index].slug.c_str(), Reader.error ? Reader.error : "no memory");
			delete job;
		}
		NextShot();
	}

	inline void RecorderMarker(const char* name, bool begin) { FrameRecorder::Mark(begin ? FrameFormat::kMarkBegin : FrameFormat::kMarkEnd, name); }

	// 'record' (2026-10-17): true while the shot's picture must wait for its recorded frame. The frame after this one is recorded
	// (FrameRecorder arms now and starts at this frame's Present); the picture is taken once the recording is written.
	inline bool RecordGate() {
		const Shot& shot = List[Index];
		if (!shot.record || RecordFinished == Index) return false;
		if (RecordArmed != Index) {
			char path[MAX_PATH];
			sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\NeonSelfTest_%s_ab_%02u_%s.nvrf", Run, (unsigned)Index, shot.slug.c_str());
			if (!FrameRecorder::Arm(TheRenderManager->device, path)) { Logger::Log("SETTING SHOTS %s: shot '%s': frame NOT recorded (recorder unavailable)", Run, shot.slug.c_str()); RecordFinished = Index; return false; }
			RecordArmed = Index;
			GpuTimer::MarkerHook = &RecorderMarker;
			Logger::Log("SETTING SHOTS %s: shot '%s': recording the next frame", Run, shot.slug.c_str());
			return true;
		}
		if (FrameRecorder::State == FrameRecorder::kArmed || FrameRecorder::State == FrameRecorder::kRecording) return true;
		RecordFinished = Index;
		GpuTimer::MarkerHook = nullptr;
		Logger::Log("SETTING SHOTS %s: shot '%s': frame recording %s", Run, shot.slug.c_str(), FrameRecorder::State == FrameRecorder::kDone ? "written" : "FAILED");
		return false;
	}

	// Render.cpp, the finished picture before the HUD (where the neon self-test takes its Final shots).
	inline void Capture(IDirect3DSurface9* surface) {
		if (!Running || Frame < ShotFrames() || !surface) return;
		if (!FramingHolds()) return;	// 'require onscreen' (2026-10-25)
		if (RecordGate()) return;	// 'record': the shot's frame first
		if (!List[Index].menu.empty()) return;	// 2026-10-19: taken after the menu is drawn (CaptureMenu)
		if (FinalPicture) { int idle = 0; FinalState.compare_exchange_strong(idle, 1); return; }	// taken at the present instead, the settings still the shot's
		TakeShot(surface);
	}

	// 2026-10-19, Render.cpp right after ImGuiManager::Render: a 'menu' shot's picture, the back buffer with the menu drawn on it.
	inline void CaptureMenu() {
		if (!Running || Frame < ShotFrames() || Index >= List.size() || List[Index].menu.empty()) return;
		IDirect3DSurface9* backBuffer = nullptr;
		if (FAILED(TheRenderManager->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) { Stop("no back buffer for a menu picture"); return; }
		TakeShot(backBuffer);
		backBuffer->Release();
	}

	// ReShadeIntegration.cpp, ReShade's D3D9 reshade_present event (render thread): the frame as presented (ReShade's effects and the HUD in it).
	inline void CaptureFinal() {
		int wanted = 1;
		if (!Running || !FinalState.compare_exchange_strong(wanted, 0)) return;
		IDirect3DSurface9* backBuffer = nullptr;
		if (FAILED(TheRenderManager->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) { Stop("no back buffer for a final picture"); return; }
		TakeShot(backBuffer);
		backBuffer->Release();
	}

	// 2026-10-18, ReShadeIntegration.cpp, ReShade's Vulkan reshade_present event (DXVK's presenting thread): Begin says whether a
	// picture is wanted now; End hands it over (null: try again at the next present). The render thread finishes the shot.
	inline bool BeginFinalCapture() { int wanted = 1; return Running && FinalState.compare_exchange_strong(wanted, 2); }
	inline void EndFinalCapture(Job* job) {
		if (!job) { FinalState.store(1); return; }
		FinalFrame.store(TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0);
		FinalJob = job;
		FinalState.store(3);
	}
}

#if NVR_TEST_TOOLS
// CpuSampler's view of the run (read on its own thread; the list does not change during a run)
inline int CpuSampler::ShotNow() { return SettingShots::Running ? (int)SettingShots::Index : -1; }
inline const char* CpuSampler::SlugOf(int shot) { return shot >= 0 && (size_t)shot < SettingShots::List.size() ? SettingShots::List[shot].slug.c_str() : "?"; }
#endif
