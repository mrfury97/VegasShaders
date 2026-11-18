#pragma once
// UNOFFICIAL 2026-11-14 DeferredSun (full deferred renderer, phase 3: docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.1 / 4.6,
// docs/next/FD-SUN-RESULTS.md; Shaders/DeferredSun.hlsl, Shaders/Includes/DeferredPixel.hlsl).
// The lighting-free records (LightFreeRecords.h, LAMPS_DEFERRED 2) compile the directional light out of every pixel they draw: the sun or
// moon outdoors, the cell's directional light indoors. This ONE full-screen pass adds it back where DeferredLamps adds the lamps (Render.cpp
// DeferredSpan: after CapturedNormals::EndSpan resolved the G-buffer and DeferredDepth made the frame's depth, before DeferredLamps and before
// LinearScene encodes the frame), in linear light, with the templates' own light maths and sun shadow lookup, x (1 - fog).
// [Shaders.ShadowsExteriors.Deferred] Enabled (test only, live; default off), DebugView, Shadows. The full deferred renderer turns it on
// through SetForced (claude/fd-interiors' DeferredRenderer 2). The pass is compiled at startup only while the lighting-free records are made
// (LightFreeRecords at startup): off, nothing is compiled, hooked or drawn.
// Order: the sun before the lamps. Both add (ONE / ONE) into the linear frame, so the order does not change the sum; the sun goes first so
// that DeferredLamps' "deferred record still bound" restore and its F10 lines stay the span's last, as before.
#include "CapturedNormals.h"
#include "DeferredDepth.h"
#include "DeferredLamps.h"
#include "LightFreeRecords.h"
#include "LinearScene.h"
#include "DeferredFog.h"
#include "LandLight.h"	// UNOFFICIAL 2026-11-15 the land's own sun (batch 3)
#include "DeferredDistant.h"	// UNOFFICIAL 2026-11-14 DeferredDistant (DS_DEPTH_AGREE)
#include "SunFadeDeferred.h"	// UNOFFICIAL 2026-11-15 fd-smoothsun (the sun steps' fade in this pass)
#include "ShaderRecord.h"
#include "FailOnce.h"	// UNOFFICIAL 2026-11-16 fd-batchB: the pass's real failures retried after the next cell change
#include "FdTestFault.h"	// UNOFFICIAL 2026-11-16 fd-batchB (test only: DebugVar4 901 / 902)
#include "ForwardCloudShadows.h"	// UNOFFICIAL 2026-11-16 fd-batchB: c169 for this pass (item 22)
#include <unordered_set>

namespace DeferredSun {
	inline const char* kSection = "Shaders.ShadowsExteriors.Deferred";

	inline ShaderRecordPixel* Pass = nullptr;
	inline bool PassFailed = false;
	inline bool Forced = false;			// the full deferred renderer's switch (SetForced)
	inline bool Live = false;			// this frame
	inline int DebugView = 0;
	inline bool Shadows = true;
	// UNOFFICIAL 2026-11-16 fd-sunvis ([Shaders.ShadowsExteriors.Deferred], live; DeferredSun.hlsl DS_Vis, Includes/DeferredPixel.hlsl FDSunVisCode):
	// the forward sun's terms the G-buffer now carries. The pass's own: the land's height self-shadow, the recorded highlight F0, SpecularFade's
	// highlight fade rebuilt from the pixel's distance. (CanopyShadow, ParallaxShadow and MicroShadow are applied by the records: PBR.cpp.)
	inline bool LandCrackShadow = true, AuthoredHighlight = true, HighlightFade = true;
	inline int SpecFadeCompiled = -1;	// [Shaders.PBR.Main] SpecularFade at startup (the records' SPEC_FADE_ON; core/PbrPop.h SpecFade::Compiled), -1 not read yet
	inline std::unordered_set<IDirect3DPixelShader9*> Handles;	// the lighting-free records' handles (from LightFreeRecords::Records)
	inline size_t HandlesFrom = 0;
	// UNOFFICIAL 2026-11-15 (bench batch 4: the land's own light was never used: "no recent land draw read back" in every view) the land's
	// lighting-free records. core/LandLight.h reads c18 / c3 back after a land pixel shader whose record DECLARES SunDir (c18) was bound
	// (ShaderRecord::SetCT, IsLandLight); the lighting-free land records compile the sun out, so SunDir is not in their constant table and in
	// a deferred frame no land bind ever asked for the read: only the forward frames (the parity's A blocks) did, which is where the logged
	// values came from. The game still uploads its land constants for these draws (it knows nothing of the record), so OnBind asks for the
	// read at their binds. Found from the forward record of the same shader (IsLandLight).
	// (2026-11-15 batch 6) the near land's lighting-free records (TerrainTemplate: direction c18) and the far land's (TerrainLODTemplate: the
	// vertex c25), read at their first draw after the bind (LandLight::ReadAtDraw through LinearScene::DrawWatch). Found by template, not by
	// the forward record's IsLandLight (which needs TESR_TerrainSkyData and missed shaders); the hand-over band's for its load line.
	inline std::unordered_set<IDirect3DPixelShader9*> LandHandles;
	inline std::unordered_set<IDirect3DPixelShader9*> FarLandHandles;
	inline std::unordered_set<IDirect3DPixelShader9*> FadeHandles;
	inline std::unordered_set<IDirect3DPixelShader9*> SunObjectHandles;	// (batch 6) objects whose c3 is the sun's colour
	inline bool WatchFar = false;
	inline void ObjectDrawWatch(IDirect3DDevice9* d) {
		LinearScene::DrawWatch = nullptr;
		LandLight::ReadObjectAtDraw(d);
	}
	inline unsigned long long StatFadeBinds = 0;
	inline void LandDrawWatch(IDirect3DDevice9* d) {
		LinearScene::DrawWatch = nullptr;	// one read per bind
		LandLight::ReadAtDraw(d, WatchFar);
	}
	inline unsigned LightFreeBindsThisSpan = 0, OlderBindsThisSpan = 0;
	// UNOFFICIAL 2026-11-14 foliage (core/DeferredFoliage.h): the leaf / grass deferred records' handles (their binds count as lighting-free ones),
	// and whether the pass was compiled with their branch (FD_FOLIAGE: [Main.Main.Quality] DeferredFoliage with GBufferV2 at startup)
	inline std::unordered_set<IDirect3DPixelShader9*> FoliageHandles;
	inline bool FoliageCompiled = false;
	inline ShaderRecordPixel* PassFade = nullptr;	// UNOFFICIAL 2026-11-15 fd-smoothsun: the pass with the sun steps' fade (SunFadeDeferred), drawn while one runs
	inline unsigned long long StatFrames = 0, StatPasses = 0, StatSkipped = 0, StatRefusedMixed = 0, StatLinearDepth = 0, StatConverted = 0, StatFogExact = 0;
	// UNOFFICIAL 2026-11-14 (bench batch 2: outdoors the pass never drew and the lighting-free records' pixels had no sun) the safety rule:
	// the lighting-free records may be bound only in frames the pass will light. A refusal found in Apply after they drew (nothing else can be
	// known before the span) turned the pass off for the session.
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 8: one odd frame turned DeferredRenderer 2 forward until the game restarted)
	// now two kinds:
	//   - a per-frame cause (no directional light, no G-buffer, no or another scene target at the span's end, LinearScene's span not open):
	//     that frame is skipped (its lighting-free pixels miss the sun once, as before) and the pass runs again the next frame. Only when
	//     kSkipsToLatch spans IN A ROW are refused (the cause lasts: a mod's target, the game's MSAA) does it latch as below, so a lasting
	//     cause cannot draw frame after frame without the sun;
	//   - a real failure (the conversion to linear light failed) or such a streak: a FailOnce latch. Wanted() is false for the rest of the
	//     cell, so DeferredRenderer 2 draws forward from the next frame (it reads Wanted through SunRuns before it binds anything); after
	//     the next cell change it is tried again, 3 tries in all ([Main.Main.Misc] RetryFailedEffects false: off for the session, as before).
	inline FailOnce::Latch Fail{ "DeferredSun" };
	inline const char* Refused = nullptr;		// the latch's last reason (shown while it holds)
	constexpr unsigned kSkipsToLatch = 3;
	inline unsigned SkipStreak = 0;				// refused spans in a row (a drawn pass resets it)
	inline unsigned long long SkipLogs = 0, StatSkippedFrames = 0;
	// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/DeferredFirstPerson.h): Apply run a second time for the first-person view model, into its
	// light target (linear, no fog), at the stencil's view-model mark: frame is the first-person camera in the DD layout (c0-c5), depth its
	// projection's _33 / _43 (c9), charM / charCenter / charData the view model's sun map (c206-c209, c213, c214: the light's view x
	// projection, for the true camera-relative point; charData.w 0 = no map this frame). Off (always without the switch): Apply as before.
	struct ViewModelPass { bool on = false; float frame[6][4] = {}; float depth[2] = {}; float charM[4][4] = {}; float charCenter[4] = {}; float charData[4] = {}; };
	inline ViewModelPass Vm;
	// A real failure: off for the rest of the cell (the latch logs it and lists it in the status bar). why: a literal.
	inline void Latch(const char* why) {
		if (Fail.failed) return;
		Refused = why;
		SkipStreak = 0;
		Logger::Log("[WARNING] UNOFFICIAL DeferredSun: off (%s): the lighting-free records are not bound from the next frame, the deferred renderer draws forward until the pass is tried again", why);
		Fail.Set(why);
	}
	// A per-frame cause: this frame only, unless it is the kSkipsToLatch-th refused span in a row. why: a literal.
	inline void SkipFrame(const char* why) {
		StatSkippedFrames++;
		if (++SkipStreak >= kSkipsToLatch) {
			Logger::Log("[WARNING] UNOFFICIAL DeferredSun: %u frames in a row without the sun pass (%s): the cause lasts", SkipStreak, why);
			Latch(why);
			return;
		}
		if (SkipLogs++ < 20 || SkipLogs % 200 == 0)
			Logger::Log("UNOFFICIAL DeferredSun: one frame skipped (%s): its lighting-free pixels had no sun this frame; the pass runs again next frame "
				"(%u in a row so far; %u in a row turn it off until the next cell change; %llu frames skipped this session)", why, SkipStreak, kSkipsToLatch, StatSkippedFrames);
	}
	// true while the latch holds for good (out of tries, or RetryFailedEffects off)
	inline bool LatchFinal() { return Fail.failed && (Fail.tries >= FailOnce::kTries || !FailOnce::RetryOn()); }

	inline bool WantedAtStartup() { return LightFreeRecords::WantedAtStartup(); }
	// The G-buffer's layout: true = GBufferV2 (CAPTURED_NORMALS_ON 4; its rules and the lighting-free code: Includes/DeferredPixel.hlsl).
	inline bool GBufferV2() { return CapturedNormals::V2CompiledIn(); }
	// The full deferred renderer's hook (DeferredRenderer 2: every direct light deferred): on = the pass runs whatever Enabled says.
	inline void SetForced(bool on) { Forced = on; }
	// Whether the pass is wanted this frame (the setting or the renderer), and can run (compiled).
	// (2026-11-14 batch 2) and nothing known before the frame stops it: LinearScene live (the pass adds in linear light), the G-buffer's
	// levels, a directional light, the latch not holding (Fail). DeferredRenderer 2 binds the lighting-free records only while this is true.
	inline bool Wanted() {
		return WantedAtStartup() && Pass && !PassFailed && !Fail.Blocked() && (Forced || TheSettingManager->GetSettingI(kSection, "Enabled") != 0) &&
			LinearScene::Live && CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn() && !CapturedNormals::Failed &&
			Tes && Tes->directionalLight;
	}
	// UNOFFICIAL 2026-11-15 (batch 3: DeferredRenderer 2's "[WARNING] the DeferredSun pass cannot run" at frame 1, before anything was
	// ready) why the pass will NOT run for this session, or null while it only is not ready yet (the first frames: LinearScene not live yet,
	// no directional light yet). DeferredRenderer warns on a reason only.
	inline const char* Failure() {
		if (!WantedAtStartup()) return "the lighting-free records are not made (LightFreeRecords off at startup)";
		if (PassFailed) return "its pass did not load (see its lines)";
		if (LatchFinal()) return Refused;	// (2026-11-16 fd-batchB: a latch that will be tried again is "not ready", not a reason to warn once)
		if (!CapturedNormals::AlbedoCompiledIn() || !CapturedNormals::MaterialCompiledIn()) return "needs CapturedNormals + MaterialBuffer + AlbedoBuffer (restart)";
		// (2026-11-16 fd-batchB) the G-buffer's and LinearScene's latches only once they hold for good (out of tries, or RetryFailedEffects off)
		auto final = [](const FailOnce::Latch& l) { return l.failed && (l.tries >= FailOnce::kTries || !FailOnce::RetryOn()); };
		if (CapturedNormals::Failed && (final(CapturedNormals::MakeFail) || final(CapturedNormals::CheckFail))) return "the G-buffer failed (see its lines)";
		if (LinearScene::Checked && !LinearScene::Compiled) return "LinearScene is not compiled in";
		if (final(LinearScene::Fail)) return "LinearScene failed (see its lines)";
		return nullptr;
	}

	// ShaderManager::Initialize, with the other passes (never on the render thread): the pass from Shaders\DeferredSun.hlsl with the global
	// define list (the forward shaders' own shadow options) and INTERIOR_SHADOWS 0 (the outdoor sun lookups compiled in; indoors
	// GetSunShadowMaps returns 1 by its own exterior gate, as for the forward records).
	inline void LoadShaders() {
		if (!WantedAtStartup() || Pass || PassFailed) return;
		static D3DXMACRO overrides[] = { { "INTERIOR_SHADOWS", "0" }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL } };	// (room for the five optional defines + the terminator)
		int n = 1;
		// UNOFFICIAL 2026-11-14 foliage (DeferredFoliage.h, which needs this pass and GBufferV2): its branch only with the key on at startup
		FoliageCompiled = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredFoliage") != 0 && GBufferV2();
		if (FoliageCompiled) overrides[n++] = { "FD_FOLIAGE", "1" };
		// UNOFFICIAL 2026-11-14 DeferredDistant (core/DeferredDistant.h, DeferredSun.hlsl DS_DEPTH_AGREE): the exact view z only where it is the G-buffer's surface
		if (DeferredDistant::CompiledIn()) overrides[n++] = { "DS_DEPTH_AGREE", "1" };
		const int core = n;	// (fd-lampfit: the test-only parts below are the ones the fallback after the compile leaves out)
		// UNOFFICIAL 2026-11-15 DeferredRtSunFirst ([Main.Main.Quality], restart, test only; docs/next/FD-GI-RESULTS.md): where RT sun shadows'
		// ForwardLookup fully replaces the cascades (the point seen by the trace, inside 85 % of its range), the pass reads the traced visibility
		// and skips the cascades' filter; elsewhere GetSunShadow as before (DeferredSun.hlsl DS_RT_SUN_FIRST; nothing without RT_SUN_FORWARD)
		if (TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRtSunFirst") != 0) overrides[n++] = { "DS_RT_SUN_FIRST", "1" };
		// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h, DeferredSun.hlsl FD_SKIN): the skin's own sun on the deferred skin's pixels
		if (DeferredLamps::SkinCompiledIn()) overrides[n++] = { "FD_SKIN", "1" };
		if (LightFreeRecords::HairDeferred()) overrides[n++] = { "FD_HAIR", "1" };	// UNOFFICIAL 2026-11-15 DeferredHair: model 8 takes the sun here too
		ShaderTemplate tmpl = {};
		tmpl.Name = "DeferredSun";
		Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredSun.pso", NULL, tmpl, "Deferred\\Sun\\", overrides);
		// UNOFFICIAL 2026-11-15 fd-lampfit (docs/next/FD-LAMPFIT.md): in DeferredRenderer 2 a sun pass that does not load turns the whole renderer
		// forward, so a test-only part (DeferredRtSunFirst, DeferredSkin, DeferredHair) never may: without it the pass is compiled again (its own
		// cache folder), the skin and hair it leaves out draw forward (their records are decided after this, at the game shaders' load), and ONE
		// line says so. Nothing changes where the full pass loads.
		if (!Pass && n > core) {
			const bool skin = DeferredLamps::SkinCompiledIn(), hair = LightFreeRecords::HairDeferred();
			n = core;
			overrides[n] = { NULL, NULL };
			Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredSun.pso", NULL, tmpl, "Deferred\\Sun\\Lean\\", overrides);
			if (Pass) {
				DeferredLamps::SkinWanted = 0;	// (no FD_SKIN in the lamp passes, no skin lamp pass: DeferredSkin stays forward)
				LightFreeRecords::HairWanted = 0;	// (the HAIR records stay forward)
				EffectStatus::Level("DeferredSun.pso", "loaded without its test-only parts (see the DeferredSun line)", false);	// (ShaderRecord's FAILED line for the full one)
				Logger::Log("[WARNING] UNOFFICIAL DeferredSun: the sun pass did not load with its test-only parts (%s%s%s): compiled without them, they stay off this session "
					"(the skin and hair draw forward); the deferred renderer stays on", TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRtSunFirst") ? "DeferredRtSunFirst " : "",
					skin ? "DeferredSkin " : "", hair ? "DeferredHair" : "");
			}
		}
		if (Pass) {
			Pass->ClearSamplers = false;	// (its stages are saved and put back by Apply; nothing to clear)
			Logger::Log("UNOFFICIAL DeferredSun: the sun pass loaded (Shaders\\DeferredSun.hlsl)");
		}
		else {
			PassFailed = true;
			Logger::Log("[ERROR] UNOFFICIAL DeferredSun: the sun pass (Shaders\\DeferredSun.hlsl) did not load: the lighting-free records' pixels get no sun");
			return;
		}
		// UNOFFICIAL 2026-11-15 fd-smoothsun (core/SunFadeDeferred.h): the same pass with the sun steps' fade (Includes/Shadow.hlsl
		// SUN_CROSSFADE_PASS), drawn only while a fade runs (Apply). A second record rather than the fade in the one: compiled in, the fade
		// costs even while it does not run (frame replay, og-ground, GTX 1070 native 1080p: the pass 0.91 -> 1.08 ms with the fade's branch never
		// taken; docs/next/FD-SMOOTHSUN-RESULTS.md).
		if (SunFadeDeferred::CompiledIn()) {
			static D3DXMACRO fadeOverrides[sizeof(overrides) / sizeof(overrides[0]) + 2] = {};
			for (int i = 0; i < n; i++) fadeOverrides[i] = overrides[i];
			const int m = SunFadeDeferred::AppendDefines(fadeOverrides, n);
			fadeOverrides[m] = { NULL, NULL };
			ShaderTemplate fadeTmpl = {};
			fadeTmpl.Name = "DeferredSun";
			PassFade = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredSun.pso", NULL, fadeTmpl, "Deferred\\SunFade\\", fadeOverrides);
			if (PassFade) PassFade->ClearSamplers = false;
			Logger::Log(PassFade ? "UNOFFICIAL DeferredSun: the sun pass with the sun steps' fade loaded (SUN_CROSSFADE_PASS)" :
				"[WARNING] UNOFFICIAL DeferredSun: the sun pass with the sun steps' fade did not load: the sun shadows step as before (the 15-degree glide)");
		}
	}

	// Once a frame (Render.cpp, with DeferredLamps::BeginFrame).
	inline void BeginFrame() {
		FdTestFault::BeginFrame();	// (2026-11-16 fd-batchB, test only: DebugVar4)
		Live = Wanted() && CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn() && !CapturedNormals::Failed;
		SunFadeDeferred::PassLive = Live && PassFade;	// (2026-11-15 fd-smoothsun: the sun steps by half a degree while this pass draws it)
		if (!WantedAtStartup()) return;
		if (HandlesFrom != LightFreeRecords::Records.size()) {	// records are made as the game's shaders load
			Handles.clear();
			LandHandles.clear();
			FarLandHandles.clear();
			FadeHandles.clear();
			SunObjectHandles.clear();
			for (const auto& r : LightFreeRecords::Records) {
				if (!r.second || !r.second->ShaderHandle) continue;
				Handles.insert(r.second->ShaderHandle);
				if (LightFreeRecords::NearLand.count(r.first)) LandHandles.insert(r.second->ShaderHandle);
				else if (LightFreeRecords::FarLand.count(r.first)) FarLandHandles.insert(r.second->ShaderHandle);
				else if (LightFreeRecords::FadeBand.count(r.first)) FadeHandles.insert(r.second->ShaderHandle);
				else if (LightFreeRecords::SunObjects.count(r.first)) SunObjectHandles.insert(r.second->ShaderHandle);
			}
			HandlesFrom = LightFreeRecords::Records.size();
		}
		// the land's c3 read-back (core/LandLight.h) only while the pass may use it: the deferred renderer in mode 2 or the pass's own switch
		LandLight::ReadColor = SettingForce::Mode() >= 2 || TheSettingManager->GetSettingI(kSection, "Enabled") != 0;
		if (LinearScene::DrawWatch == &LandDrawWatch || LinearScene::DrawWatch == &ObjectDrawWatch) LinearScene::DrawWatch = nullptr;	// (no watch across frames)
		DebugView = TheSettingManager->GetSettingI(kSection, "DebugView");
		Shadows = TheSettingManager->GetSettingI(kSection, "Shadows") != 0;
		LandCrackShadow = TheSettingManager->GetSettingI(kSection, "LandCrackShadow") != 0;
		AuthoredHighlight = TheSettingManager->GetSettingI(kSection, "AuthoredHighlight") != 0;
		if (SpecFadeCompiled < 0) SpecFadeCompiled = TheSettingManager->GetSettingI("Shaders.PBR.Main", "SpecularFade") != 0 ? 1 : 0;
		HighlightFade = TheSettingManager->GetSettingI(kSection, "HighlightFade") != 0 && SpecFadeCompiled == 1 &&
			TheSettingManager->GetSettingI("Shaders.PBR.Main", "SpecularFade") != 0;	// (as the records fade: compiled at startup, live while compiled)
		{
			static int statusSlot = -1;
			const bool asked = Forced || TheSettingManager->GetSettingI(kSection, "Enabled") != 0;
			const char* why = Live ? (Forced ? "live (the deferred renderer)" : "live (Enabled)") : !asked ? "off" : PassFailed ? "the pass failed to load" :
				Fail.failed ? Refused : !LinearScene::Live ? "needs LinearScene live" : "needs CapturedNormals + MaterialBuffer + AlbedoBuffer";
			EffectStatus::Note(statusSlot, "DeferredSun", Live ? EffectStatus::Drawn : EffectStatus::Skipped, why);
		}
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED SUN %u frames: the pass %.2f a frame (the exact linear depth %.2f; the frame converted to linear light first %.2f; the exact stored-curve fog %.2f), skipped %.2f, mixed spans lit (sun-keeping records in the same span, told apart by their code) %.2f%s%s",
			(unsigned)StatFrames, StatPasses / f, StatLinearDepth / f, StatConverted / f, StatFogExact / f, StatSkipped / f, StatRefusedMixed / f,
			Fail.failed ? (LatchFinal() ? "; off for the session: " : "; off until the next cell change: ") : "", Fail.failed && Refused ? Refused : "");
		StatFrames = 0; StatPasses = StatSkipped = StatRefusedMixed = StatLinearDepth = StatConverted = StatFogExact = 0;
	}

	// Render.cpp's bind hook: a lighting-free record bound (the pass runs only in spans that had one), or one of DeferredLamps' LAMPS_DEFERRED 1
	// records (which keep their sun: see Apply).
	inline void OnBind(IDirect3DPixelShader9* handle) {
		if (!Live || !handle) return;
		if (Handles.count(handle) || FoliageHandles.count(handle)) LightFreeBindsThisSpan++;	// (2026-11-14 + the foliage deferred records)
		// (2026-11-15 batch 6) the land's own light, read at this land record's first draw; any other bind disarms the watch
		const bool nearLand = LandHandles.count(handle) != 0, farLand = !nearLand && FarLandHandles.count(handle) != 0;
		if (nearLand || farLand) { WatchFar = farLand; LinearScene::DrawWatch = &LandDrawWatch; }
		else if (TheFrameRateManager && LandLight::ObjectReadFrame != (UInt32)TheFrameRateManager->FrameNumber && SunObjectHandles.count(handle))
			LinearScene::DrawWatch = &ObjectDrawWatch;	// (batch 6) the objects' sun colour, once a frame
		else if (LinearScene::DrawWatch == &LandDrawWatch || LinearScene::DrawWatch == &ObjectDrawWatch) LinearScene::DrawWatch = nullptr;
		if (FadeHandles.count(handle)) {
			StatFadeBinds++;
			static bool fadeTold = false;
			if (!fadeTold) { fadeTold = true; Logger::Log("UNOFFICIAL DeferredSun: the land's hand-over band drew with its lighting-free record (TerrainFadeTemplate, LAMPS_DEFERRED 2; logged once)"); }
		}
		else if (DeferredLamps::ForwardOf.count(handle)) OlderBindsThisSpan++;
	}

	// UNOFFICIAL 2026-11-15 (batch 3) the lights the pass and the forward shaders use, every 600 passes and at the first: the directional
	// light (the objects' sun: direction, elevation, Diff, Dimmer) against the land's own (c18 / c3 as read back after a land draw), so a
	// bench log shows whether they differ and by how much.
	inline void LogLight(const D3DXVECTOR3& toSun, const NiDirectionalLight* sun, bool landFresh) {
		static unsigned n = 0;
		if (n++ % 600) return;
		const float elevSun = asinf((std::max)(-1.0f, (std::min)(1.0f, toSun.z))) * 57.29578f;
		const float* landDir = LandLight::DeferredDir;
		const float elevLand = asinf((std::max)(-1.0f, (std::min)(1.0f, landDir[2]))) * 57.29578f;
		const float dot = toSun.x * landDir[0] + toSun.y * landDir[1] + toSun.z * landDir[2];
		Logger::Log("DEFERRED SUN light: directional to-light (%.3f %.3f %.3f) %.1f deg high, Diff (%.3f %.3f %.3f) x Dimmer %.3f | land c18 (%.3f %.3f %.3f) %.1f deg high, "
			"angle to the directional %.1f deg, c3 (%.3f %.3f %.3f)%s | the land is lit with %s | land reads so far: near %u, far %u, rejected %u; hand-over band binds %llu | objects' c3 (%.3f %.3f %.3f), %u reads",
			toSun.x, toSun.y, toSun.z, elevSun, sun->Diff.r, sun->Diff.g, sun->Diff.b, sun->Dimmer, landDir[0], landDir[1], landDir[2], elevLand,
			acosf((std::max)(-1.0f, (std::min)(1.0f, dot))) * 57.29578f, LandLight::Color[0], LandLight::Color[1], LandLight::Color[2], LandLight::ColorGood ? "" : " (not read)",
			landFresh ? "its own light (c18 / c3)" : "the directional light (no recent land draw read back)", LandLight::ReadsNear, LandLight::ReadsFar, LandLight::Rejected, StatFadeBinds,
			LandLight::ObjectColor[0], LandLight::ObjectColor[1], LandLight::ObjectColor[2], LandLight::ReadsObject);
	}

	// Which source the pass took, logged when it changes (so a log says what a shot was made with).
	inline void LogSources(bool linearDepth) {
		static int lastDepth = -1, lastNormal = -1;
		if (lastDepth != (int)linearDepth) {
			lastDepth = linearDepth;
			Logger::Log(linearDepth ? "UNOFFICIAL DeferredSun: the position from the exact linear view z (DeferredDepth 2)" :
				"UNOFFICIAL DeferredSun: the position from COLOR3's camera distance (a half float: DeferredDepth 2 is off or made no depth this frame)");
		}
		const int normal = CapturedNormals::AlbedoCompiledIn() ? (GBufferV2() ? 2 : 1) : 0;
		if (lastNormal != normal) {
			lastNormal = normal;
			Logger::Log(normal == 2 ? "UNOFFICIAL DeferredSun: the shading normal from GBufferV2 (COLOR2.g / a), COLOR3's smooth normal where a record wrote none" :
				"UNOFFICIAL DeferredSun: the shading normal from the older deferred code where a record wrote it (the many-lamp family, the land), else COLOR3's smooth normal (the < 4-lamp objects and parallax lose their normal maps' detail in the sun until GBufferV2)");
		}
	}

	// The pass (DeferredSpan, Render.cpp): after DeferredDepth::AtSpanEnd, before DeferredLamps::Apply.
	inline void Apply(IDirect3DDevice9* d) {
		const unsigned binds = LightFreeBindsThisSpan, older = OlderBindsThisSpan;
		LightFreeBindsThisSpan = OlderBindsThisSpan = 0;
		if (!Live || !binds) return;
		// 2026-11-14 fd-fixes: no mixed-span refusal any more. A span with both a "sun kept" record (LAMPS_DEFERRED 1 / 3) and a lighting-free
		// one is lit: the pixels are told apart on both layouts (Includes/DeferredPixel.hlsl). Under GBufferV2 the sun-kept records write
		// FD_LAMPS_V2_BASE + keep (5..6) and FDSunDeferredPixel takes only a in (1, 2]; without it they write keep (<= 1, the old mark in COLOR2.a)
		// and FDSunDeferredPixel takes only a in (2.5, 6.5). The old guard (older && GBufferV2) predates FD_LAMPS_CODE and dropped correct sun
		// light. Such spans are counted (StatRefusedMixed: now "mixed spans lit").
		if (older) StatRefusedMixed++;
		NiDirectionalLight* sun = Tes ? Tes->directionalLight : nullptr;
		if (!sun) { StatSkipped++; if (!Vm.on) SkipFrame("no directional light at the span's end"); return; }
		if (!CapturedNormals::Drawn || !CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture || !CapturedNormals::Texture) { StatSkipped++; if (!Vm.on) SkipFrame("the G-buffer was not made this frame"); return; }
		// (2026-11-16 fd-batchB, test only) DebugVar4 901: one refused frame, as a per-frame cause above
		if (!Vm.on && FdTestFault::Take(901)) { StatSkipped++; SkipFrame("test fault 901 (DebugVar4)"); return; }
		// (test only) DebugVar4 902: a real failure, as a failed conversion below (here, so it does not wait for a frame in the stored curve)
		if (!Vm.on && FdTestFault::Take(902)) { StatSkipped++; Latch("test fault 902 (DebugVar4): as a failed conversion to linear light"); return; }
		IDirect3DSurface9* target = nullptr;
		d->GetRenderTarget(0, &target);
		if (!target) { StatSkipped++; if (!Vm.on) SkipFrame("no scene target at the span's end"); return; }
		static GpuTimer timer("Deferred sun"), vmTimer("Deferred sun (first person)");	// (with the conversion to linear light below, when it runs)
		GpuProfileScope gpu(Vm.on ? vmTimer : timer, d);
		// The space (bench batch 2): LinearScene switches the frame between linear light and the stored curve draw by draw (BeforeDraw: a
		// draw that is not one of NVR's lit records, or that blends other than additively, gets the stored curve), and the main list outdoors
		// ends with such draws (the game's own shaders, blended ones), so at the span's end the frame is usually in the stored curve. The pass
		// puts it into linear light itself with LinearScene's own conversion (the same one BeforeDraw makes; exact without MSAA, and the span
		// does not run under the game's MSAA), then adds its light; DeferredLamps and the flashlight after it find the frame linear, and
		// LinearScene::EndSpan converts back once, as for any span that ends linear. No span (LinearScene off, the game's MSAA) or a failed
		// conversion: refused (2026-11-16 fd-batchB: SkipFrame for the span, Latch for a failed conversion).
		if (!LinearScene::Linear && !Vm.on) {	// (DeferredFirstPerson: its own light target, linear)
			if (!LinearScene::InSpan || !LinearScene::SpanTarget || target != LinearScene::SpanTarget) {
				target->Release(); StatSkipped++;
				SkipFrame(!LinearScene::InSpan ? "LinearScene's span is not running (LinearScene off, or the game's MSAA)" : "the scene target at the span's end is not LinearScene's");
				return;
			}
			if (!LinearScene::Convert(d, target, true)) {
				target->Release(); StatSkipped++;
				// LinearScene's own latch (its line says why; it turns the renderer forward by itself): this frame only here
				if (LinearScene::Fail.failed) SkipFrame("LinearScene failed (see its line)");
				else Latch("LinearScene's conversion to linear light failed");
				return;
			}
			StatConverted++;
		}
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);


		const bool linearDepth = DeferredDepth::LinearReady() && !Vm.on;	// (the world's view z: not the view model's)
		if (!DeferredDepth::Ready() && !linearDepth && !Vm.on) DeferredDepth::FillFrame(d);	// the ray mapping (TESR_DeferredDepthFrame) without the copy
		if (!Vm.on) LogSources(linearDepth);
		if (linearDepth) StatLinearDepth++;

		auto* rs = TheRenderManager->renderState;
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &rt[i])) && rt[i]) d->SetRenderTarget(i, nullptr);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		// every stage: SetCT binds the pass's samplers through the game's state cache, so they go back the same way (cache and device agree)
		IDirect3DBaseTexture9* tex[16] = {};
		static const int kSamplerStates = 13;	// D3DSAMP_ADDRESSU (1) .. D3DSAMP_DMAPOFFSET (13)
		DWORD savedSS[16][kSamplerStates];
		for (DWORD s = 0; s < 16; s++) {
			d->GetTexture(s, &tex[s]);
			for (int k = 0; k < kSamplerStates; k++) savedSS[s][k] = rs->GetSamplerState(s, (D3DSAMPLERSTATETYPE)(k + 1));
		}
		// every pixel constant (SetCT writes the TESR_ ones wherever this pass declares them)
		static float savedC[224][4];
		d->GetPixelShaderConstantF(0, &savedC[0][0], 224);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const bool add = DebugView == 0;	// the debug views write over the frame
		// the light depth-tested (not written) at each pixel's G-buffer surface, as DeferredLamps' lighting draw (the shader's own depth)
		const DWORD zFunc = (1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL;
		const DWORD want[] = { D3DZB_TRUE, FALSE, zFunc, FALSE, D3DCULL_NONE, Vm.on ? (DWORD)TRUE : (DWORD)FALSE, FALSE, FALSE, 0x7, add ? (DWORD)TRUE : (DWORD)FALSE, D3DBLEND_ONE, D3DBLEND_ONE,
			D3DBLENDOP_ADD, FALSE, FALSE };	// (stencil: DeferredFirstPerson's view-model mark, its test set by the caller)
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }

		// the pass's TESR_ constants and samplers, by name (the G-buffer, the depth, the cascades, the cloud / character / RT sun maps, the
		// PBR and land knobs); the registered textures re-read from their slots first (they are remade on a resolution change, and this record
		// is not in the shader collections ShaderManager::ClearShaderSamplers walks)
		// (2026-11-15 fd-smoothsun) the record with the sun steps' fade while one runs (SunFadeDeferred::Fading, set with the shadow maps' sun
		// before the frame's world is drawn)
		// (fd-batch8 merge) DeferredFirstPerson's run (Vm.on) takes the same record as the world's in the same frame: the view model reads the
		// same world-camera-relative cascades, so the gun fades from the old maps to the new ones with the ground it stands over (its own sun
		// map, c206-c214, is set after SetCT below either way; the fade's rows are c146-c166 / c170-c173 and s10, which it does not touch)
		ShaderRecordPixel* const pass = SunFadeDeferred::Fading && PassFade ? PassFade : Pass;
		for (UInt32 i = 0; i < pass->TextureShaderValuesCount; i++) {
			TextureRecord* rec = pass->TextureShaderValues[i].Texture;
			if (rec && rec->TextureRef) rec->Texture = *rec->TextureRef;
		}
		pass->SetCT();
		// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 22): ForwardCloudShadows' c169 for THIS pass. SetCT sends it through
		// ForwardCloudShadows::ForShader, whose strength is 0 when the last object pass drawn was a plant's with PlantShadows off (PassPlant):
		// the whole frame's cloud shadow then came and went with the last object drawn. Here the frame's own strength, whatever was drawn last
		// (put back with the other pixel constants below). Plants with PlantShadows off: their pixels are told apart in the shader (batch D).
		if (ForwardCloudShadows::Compiled) ForwardCloudShadows::SendFrame(d);
		// the pass's own (c6-c9)
		const NiPoint3& dir = sun->direction;
		D3DXVECTOR3 toSun(-dir.x, -dir.y, -dir.z);
		const float len = D3DXVec3Length(&toSun);
		const bool has = len > 1.0e-6f;
		if (has) toSun /= len;
		const bool objectFresh = LandLight::ObjectReadFrame && (UInt32)TheFrameRateManager->FrameNumber - LandLight::ObjectReadFrame <= 4;
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		const float own[4][4] = {
			{ toSun.x, toSun.y, toSun.z, has ? 1.0f : 0.0f },
			// (2026-11-15 batch 6) the objects' sun colour as the game handed them this frame (c3 read back at a sun-lit object record's draw:
			// 1.21 x Diff x Dimmer in every recording so far), else the directional light's Diff x Dimmer as before
			{ objectFresh ? LandLight::ObjectColor[0] : sun->Diff.r * sun->Dimmer, objectFresh ? LandLight::ObjectColor[1] : sun->Diff.g * sun->Dimmer,
			  // w (2026-11-16 fd-merged-1116, FD-BATCH-D item 22): 1 = the forward clouds shade plants ([Shaders.VolumetricClouds.Main]
			  // PlantShadows), 0 = they do not: DeferredSun.hlsl leaves model 5's pixels out of the cloud term then, as forward's PassPlant does
			  objectFresh ? LandLight::ObjectColor[2] : sun->Diff.b * sun->Dimmer, ForwardCloudShadows::PlantShadows ? 1.0f : 0.0f },
			// w (2026-11-14, read only by the foliage branch, FD_FOLIAGE): 2 = [Shaders.Grass.Main] ShadowFix (the grass's lookup takes the world's up)
			{ (float)DebugView, linearDepth ? 1.0f : 0.0f, Shadows ? 1.0f : 0.0f, FoliageCompiled && TheSettingManager->GetSettingI("Shaders.Grass.Main", "ShadowFix") ? 2.0f : 1.0f },
			{ proj._33, proj._43, 0.01f, 2.0f } };
		d->SetPixelShaderConstantF(6, own[0], 4);
		if (Vm.on) {	// UNOFFICIAL 2026-11-15 DeferredFirstPerson: the first-person camera, its depth terms, the view model's sun map
			const float depthVm[4] = { Vm.depth[0], Vm.depth[1], 0.01f, 2.0f };
			d->SetPixelShaderConstantF(0, &Vm.frame[0][0], 6);
			d->SetPixelShaderConstantF(9, depthVm, 1);
			d->SetPixelShaderConstantF(206, &Vm.charM[0][0], 4);
			d->SetPixelShaderConstantF(213, Vm.charCenter, 1);
			d->SetPixelShaderConstantF(214, Vm.charData, 1);
		}
		// UNOFFICIAL 2026-11-15 (bench batch 3: the deferred land 15-25 % darker than forward by day while the objects matched) the land's
		// own light: the land's pixel shaders light with SunDir (c18) and SunColor (c3) as the game hands THEM, not the directional light the
		// objects get (core/LandLight.h: measured 10.8 against 27.4 degrees high at 17.4 h). Read back after this frame's (or a recent) land
		// draw; c14 / c15, w 1 = valid (else the directional light, as before).
		const UInt32 frameNow = (UInt32)TheFrameRateManager->FrameNumber;
		const bool landFresh = LandLight::ReadFrame && frameNow - LandLight::ReadFrame <= 4;
		const float land[2][4] = {
			{ LandLight::DeferredDir[0], LandLight::DeferredDir[1], LandLight::DeferredDir[2], landFresh ? 1.0f : 0.0f },
			{ LandLight::Color[0], LandLight::Color[1], LandLight::Color[2], landFresh && LandLight::ColorGood ? 1.0f : 0.0f } };
		d->SetPixelShaderConstantF(14, land[0], 2);
		// (2026-11-16 fd-sunvis) c16 DS_Vis: x 1 = the land's height self-shadow, y 1 = the recorded highlight F0, z / w SpecularFade's band
		// (the game's fSpecularLODStartFade and 1 / (fSpecularLODEnd - it); w 0 = no fade: SpecularFade off or not compiled, or the key off)
		{
			const float start = *BSShaderManager::fSpecularLODStartFade, end = *BSShaderManager::fSpecularLODEnd;
			const bool fade = HighlightFade && end > start + 1.0f;
			const float vis[4] = { LandCrackShadow ? 1.0f : 0.0f, AuthoredHighlight ? 1.0f : 0.0f, fade ? start : 0.0f, fade ? 1.0f / (end - start) : 0.0f };
			d->SetPixelShaderConstantF(16, vis, 1);
		}
		LogLight(toSun, sun, landFresh);
		const float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(211, one, 1);	// NVR_SpecFade: full (no per-draw fade in a screen pass)
		const float zero[4] = {};
		d->SetPixelShaderConstantF(204, zero, 1);	// NVR_ShadowCharacterDraw (Shadow.hlsl, per draw for actors' heads): not an actor's draw
		// (2026-11-14) the stored-curve fog (core/DeferredFog.h, Includes/DeferredPixel.hlsl FDAddFogged): with the game's fog drawn, the fog
		// colour in c13 and the frame's copy (linear now) on s0, through the state cache like SetCT's stages (put back below)
		float fog[4] = {};	// (2026-11-15: zero for the first-person call, which uploads it unread)
		if (!Vm.on && DeferredFog::Params(fog) && DebugView == 0) {
			IDirect3DTexture9* frameCopy = DeferredFog::FrameCopy(d, target);
			if (frameCopy) {
				rs->SetTexture(0, frameCopy);
				TheRenderManager->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); TheRenderManager->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				TheRenderManager->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE); TheRenderManager->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				TheRenderManager->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); TheRenderManager->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
				StatFogExact++;
			}
			else fog[3] = 0.0f;
		}
		else fog[3] = 0.0f;
		d->SetPixelShaderConstantF(13, fog, 1);

		d->SetVertexShader(nullptr);
		d->SetPixelShader(pass->ShaderHandle);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		// UNOFFICIAL 2026-11-15 SSS v2 (core/DeferredSkin.h BeforePasses): the skin's highlight target on stage 1 for this draw (DeferredSun.hlsl's
		// COLOR1, FD_SKIN), added like the light (ONE / ONE); taken off after. Null unless DeferredSkinSSS 2 runs this frame.
		DWORD mask1 = 0xF;
		if (DeferredLamps::SkinSpecSurface) { d->SetRenderTarget(1, DeferredLamps::SkinSpecSurface); d->SetViewport(&vp); d->GetRenderState(D3DRS_COLORWRITEENABLE1, &mask1); d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF); }
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		if (DeferredLamps::SkinSpecSurface) { d->SetRenderTarget(1, nullptr); d->SetViewport(&vp); d->SetRenderState(D3DRS_COLORWRITEENABLE1, mask1); }
		StatPasses++;
		if (!Vm.on) {	// (2026-11-16 fd-batchB) a drawn pass ends a streak of refused frames; one line when it follows one, or a latch's retry
			if (SkipStreak && SkipLogs <= 21) Logger::Log("UNOFFICIAL DeferredSun: the sun pass drew again after %u skipped frame%s", SkipStreak, SkipStreak == 1 ? "" : "s");
			SkipStreak = 0;
			static int latchTold = 0;
			if (Fail.tries != latchTold) { latchTold = Fail.tries; Logger::Log("UNOFFICIAL DeferredSun: the sun pass draws again after its latch (attempt %d of %d)", Fail.tries + 1, FailOnce::kTries); }
		}

		for (DWORD s = 0; s < 16; s++) {
			rs->SetTexture(s, tex[s]);
			for (int k = 0; k < kSamplerStates; k++)
				if (rs->GetSamplerState(s, (D3DSAMPLERSTATETYPE)(k + 1)) != savedSS[s][k]) TheRenderManager->SetSamplerState(s, (D3DSAMPLERSTATETYPE)(k + 1), savedSS[s][k]);
			if (tex[s]) tex[s]->Release();
		}
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, &savedC[0][0], 224);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) { d->SetRenderTarget(i, rt[i]); rt[i]->Release(); }
		d->SetViewport(&vp);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
		target->Release();
	}
}
