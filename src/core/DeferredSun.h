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
#include "RtSunShadows.h"
#include "FailOnce.h"	// UNOFFICIAL 2026-11-16 fd-batchB: the pass's real failures retried after the next cell change
#include "FdTestFault.h"	// UNOFFICIAL 2026-11-16 fd-batchB (test only: DebugVar4 901 / 902)
#include "ForwardCloudShadows.h"	// UNOFFICIAL 2026-11-16 fd-batchB: c169 for this pass (item 22)
#include "DeferredWetness.h"	// UNOFFICIAL 2026-11-19 DeferredWetness: the land's water layer, drawn inside Apply; the film's sun highlight
#include "RainState.h"	// UNOFFICIAL 2026-11-26 SunSheen: no dry sheen on wet ground (FillSheenConst)
#include "ScreenSpaceShadows.h"	// UNOFFICIAL 2026-11-26 Screen-Space Shadows (Bottled Shaders port): this span's mask (DS_SSS c19, s13)
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
	inline bool LandCrackShadow = true, AuthoredHighlight = true, HighlightFade = true, MatchSunDisc = false;
	// UNOFFICIAL 2026-11-26 SunSheen (ported by hand from upstream 1.1.5-test4, live; DeferredSun.hlsl / Compute\DeferredSun.comp EvalSunSheen):
	// a broad dry glaze and backscatter on sunlit matte surfaces outdoors by day.
	inline float SunSheen = 0.9f, SunSheenRoughness = 0.78f;
	inline float SmoothStep01(float edge0, float edge1, float x) {
		float t = (x - edge0) / (edge1 - edge0);
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		return t * t * (3.0f - 2.0f * t);
	}
	// The sheen's constant (D3D9 c18 DS_Sheen; the compute pass's dsSheen, 16-byte slot 72): x the strength, y the lobe's roughness, z 1 = the
	// land's smooth-roughness gate (below). Upstream's gates: outdoors, not the first-person view model, the day light past 0.35 with a
	// smoothstep to 0.65; the roughness clamped to 0.45-0.95. Ours on top (a dry glow must not sit on wet ground): the strength x (1 - RainState::
	// Factor()), the ground's wetness (the rain or what it left), which is all the pass knows of wetness without DeferredWetness's film; the
	// film's per-pixel gloss is gated in the shader (DS_WET_FILM). Puddles' standing water outlasts that wetness (its own slower clock) and,
	// without the film, the land record writes it into the G-buffer only as a near-zero roughness: so while standing water is out and no film
	// is live this frame, z asks the shader to drop the sheen on the land's smooth pixels.
	inline void FillSheenConst(bool isVm, bool filmLive, float out[4]) {
		const ShaderManager* const sm = TheShaderManager;
		float on = (!isVm && sm && sm->GameState.isExterior && sm->GameState.dayLight > 0.35f && SunSheen > 0.0f)
			? SunSheen * SmoothStep01(0.35f, 0.65f, sm->GameState.dayLight) : 0.0f;
		if (on > 0.0f) on *= 1.0f - RainState::Factor();
		bool puddles = false;
		if (on > 0.0f && !filmLive) {
			auto it = TheShaderManager->ConstantsTable.find("TESR_PuddleData");
			puddles = it != TheShaderManager->ConstantsTable.end() && it->second && it->second->x > 0.001f;
		}
		out[0] = on;
		out[1] = SunSheenRoughness < 0.45f ? 0.45f : (SunSheenRoughness > 0.95f ? 0.95f : SunSheenRoughness);
		out[2] = puddles ? 1.0f : 0.0f;
		out[3] = 0.0f;
	}
	inline float BlendMatchSunDisc(D3DXVECTOR3& toSun, bool& has) {
		const float len = D3DXVec3Length(&toSun);
		has = len > 1.0e-6f;
		if (has) toSun /= len;
		if (!MatchSunDisc || !TheShaderManager || !TheShaderManager->GameState.isExterior) return 0.0f;
		const D3DXVECTOR4& disc = TheShaderManager->ShaderConst.SunDir;
		D3DXVECTOR3 discV(disc.x, disc.y, disc.z);
		const float discLen = D3DXVec3Length(&discV);
		if (discLen <= 1.0e-6f) return 0.0f;
		const D3DXVECTOR3 discDir = discV / discLen;
		const float blend = SmoothStep01(0.45f, 0.65f, TheShaderManager->GameState.dayLight) * SmoothStep01(0.0f, 0.12f, discDir.z);
		if (blend <= 0.0f) return 0.0f;
		if (has) {
			D3DXVec3Lerp(&toSun, &toSun, &discDir, blend);
			const float blendedLen = D3DXVec3Length(&toSun);
			if (blendedLen > 1.0e-6f) toSun /= blendedLen;
			else toSun = discDir;
		} else {
			toSun = discDir;
			has = true;
		}
		return blend;
	}
	inline const float* ResolveLandDir(const D3DXVECTOR3& toSun, bool has, bool landFresh, float discBlend, D3DXVECTOR3& scratch, bool& validOut) {
		validOut = landFresh || (discBlend > 0.0f && has);
		if (discBlend <= 0.0f || !has) return LandLight::DeferredDir;
		if (!landFresh) return &toSun.x;
		const D3DXVECTOR3 raw(LandLight::DeferredDir[0], LandLight::DeferredDir[1], LandLight::DeferredDir[2]);
		D3DXVec3Lerp(&scratch, &raw, &toSun, discBlend);
		const float len = D3DXVec3Length(&scratch);
		if (len > 1.0e-6f) scratch /= len;
		else scratch = toSun;
		return &scratch.x;
	}
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
	inline bool WetFilmCompiled = false;	// UNOFFICIAL 2026-11-19 DeferredWetness: the pass has the film's sun highlight (DS_WET_FILM)
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
	// UNOFFICIAL 2026-11-21 (Phase 2): Vulkan Compute DeferredSun pass (Compute\DeferredSun.spv)
	inline ID3D9NvrExtPipeline* ComputePipeline = nullptr;
	inline ID3D9NvrExtBuffer* ComputeUbo = nullptr;
	inline IDirect3DTexture9* ComputeRt0DummyTex = nullptr;
	inline IDirect3DSurface9* ComputeRt0DummySurf = nullptr;
	inline IDirect3DTexture9* ComputeSpecDummyTex = nullptr;
	inline IDirect3DSurface9* ComputeSpecDummySurf = nullptr;
	inline bool ComputeTried = false, ComputeFailed = false;
	inline unsigned long long StatCompute = 0;
	// Cached compile-time flags (set once in LoadShaders) + SettingsRevision cache for BeginFrame/Wanted
	inline bool ForwardShadowsCompiled = true, ContactHardeningCompiled = false, CharMapCompiled = false;
	inline bool RtSunCompiled = false, RtSunFirstCompiled = false, SlackCapAllCompiled = false;
	inline bool SkylightingMode1 = false, LinearCompiled = true, DepthAgreeCompiled = false;
	inline struct CachedSettings {
		bool valid = false;
		UInt32 revision = 0;
		bool enabled = false, shadows = true, landCrackShadow = true, authoredHighlight = true;
		bool matchSunDisc = false, highlightFade = true, specFadeLive = true, grassShadowFix = false;
		int debugView = 0, nvrDxvkDeferred = 1, smithMode = 0;
		float sunSheen = 0.9f, sunSheenRoughness = 0.78f;	// (2026-11-26 SunSheen)
	} gSettings;
	inline const CachedSettings& SyncSettings() {
		if (!TheSettingManager) return gSettings;
		const UInt32 rev = TheSettingManager->SettingsRevision;
		if (!gSettings.valid || gSettings.revision != rev) {
			gSettings.revision = rev;
			gSettings.valid = true;
			gSettings.enabled = TheSettingManager->GetSettingI(kSection, "Enabled") != 0;
			gSettings.debugView = TheSettingManager->GetSettingI(kSection, "DebugView");
			gSettings.shadows = TheSettingManager->GetSettingI(kSection, "Shadows") != 0;
			gSettings.landCrackShadow = TheSettingManager->GetSettingI(kSection, "LandCrackShadow") != 0;
			gSettings.authoredHighlight = TheSettingManager->GetSettingI(kSection, "AuthoredHighlight") != 0;
			gSettings.matchSunDisc = TheSettingManager->GetSettingI(kSection, "MatchSunDisc") != 0;
			gSettings.sunSheen = TheSettingManager->GetSettingF(kSection, "SunSheen");
			gSettings.sunSheenRoughness = TheSettingManager->GetSettingF(kSection, "SunSheenRoughness");
			gSettings.highlightFade = TheSettingManager->GetSettingI(kSection, "HighlightFade") != 0;
			gSettings.specFadeLive = TheSettingManager->GetSettingI("Shaders.PBR.Main", "SpecularFade") != 0;
			gSettings.grassShadowFix = TheSettingManager->GetSettingI("Shaders.Grass.Main", "ShadowFix") != 0;
			gSettings.nvrDxvkDeferred = TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkDeferred");
			gSettings.smithMode = TheSettingManager->GetSettingI("Shaders.PBR.Main", "SmithCorrelated");
		}
		return gSettings;
	}
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
		return WantedAtStartup() && Pass && !PassFailed && !Fail.Blocked() && (Forced || SyncSettings().enabled) &&
			LinearScene::Live && CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn() && !CapturedNormals::Failed &&
			Tes && Tes->directionalLight && !DeferredWetness::Broken();	// (2026-11-19 DeferredWetness: the land's code cannot be put back: forward)
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
		DeferredWetness::LoadShaders();	// UNOFFICIAL 2026-11-19 (first: DS_WET_FILM below only when its pass loaded)
		if (!WantedAtStartup() || Pass || PassFailed) return;
		static D3DXMACRO overrides[] = { { "INTERIOR_SHADOWS", "0" }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL } };	// (room for the six optional defines + the terminator)
		int n = 1;
		// UNOFFICIAL 2026-11-14 foliage (DeferredFoliage.h, which needs this pass and GBufferV2): its branch only with the key on at startup
		FoliageCompiled = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredFoliage") != 0 && GBufferV2();
		if (FoliageCompiled) overrides[n++] = { "FD_FOLIAGE", "1" };
		// UNOFFICIAL 2026-11-14 DeferredDistant (core/DeferredDistant.h, DeferredSun.hlsl DS_DEPTH_AGREE): the exact view z only where it is the G-buffer's surface
		DepthAgreeCompiled = DeferredDistant::CompiledIn();
		if (DepthAgreeCompiled) overrides[n++] = { "DS_DEPTH_AGREE", "1" };
		const int core = n;	// (fd-lampfit: the test-only parts below are the ones the fallback after the compile leaves out)
		// UNOFFICIAL 2026-11-15 DeferredRtSunFirst ([Main.Main.Quality], restart, test only; docs/next/FD-GI-RESULTS.md): where RT sun shadows'
		// ForwardLookup fully replaces the cascades (the point seen by the trace, inside 85 % of its range), the pass reads the traced visibility
		// and skips the cascades' filter; elsewhere GetSunShadow as before (DeferredSun.hlsl DS_RT_SUN_FIRST; nothing without RT_SUN_FORWARD)
		RtSunFirstCompiled = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRtSunFirst") != 0;
		if (RtSunFirstCompiled) overrides[n++] = { "DS_RT_SUN_FIRST", "1" };
		ForwardShadowsCompiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") != 0;
		ContactHardeningCompiled = TheSettingManager->GetSettingI("Shaders.ContactHardening.Status", "Enabled") != 0;
		CharMapCompiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap") != 0;
		RtSunCompiled = RtSunShadows::ForwardCompiled();
		SlackCapAllCompiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "CasterReachEverywhere") != 0;
		SkylightingMode1 = TheSettingManager->GetSettingI("Shaders.PBR.Main", "SkylightingMode") != 0;
		LinearCompiled = TheSettingManager->GetSettingI("Shaders.PBR.Linear", "Enabled") != 0;
		// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h, DeferredSun.hlsl FD_SKIN): the skin's own sun on the deferred skin's pixels
		if (DeferredLamps::SkinCompiledIn()) overrides[n++] = { "FD_SKIN", "1" };
		if (LightFreeRecords::HairDeferred()) overrides[n++] = { "FD_HAIR", "1" };	// UNOFFICIAL 2026-11-15 DeferredHair: model 8 takes the sun here too
		// UNOFFICIAL 2026-11-19 DeferredWetness (core/DeferredWetness.h; DeferredSun.hlsl DS_WET_FILM): the wet film's sun highlight on the near land,
		// from the film target, with the pass's own shadow (test only, so a pass that does not fit with it is compiled again without it, below)
		WetFilmCompiled = DeferredWetness::CompiledIn();
		if (WetFilmCompiled) overrides[n++] = { "DS_WET_FILM", "1" };
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
				WetFilmCompiled = false;	// (2026-11-19: no film highlight; DeferredWetness's albedo, normal, ambient and sky stay)
				LightFreeRecords::HairWanted = 0;	// (the HAIR records stay forward)
				EffectStatus::Level("DeferredSun.pso", "loaded without its test-only parts (see the DeferredSun line)", false);	// (ShaderRecord's FAILED line for the full one)
				Logger::Log("[WARNING] UNOFFICIAL DeferredSun: the sun pass did not load with its test-only parts (%s%s%s%s): compiled without them, they stay off this session "
					"(the skin and hair draw forward; the wet land has no film highlight); the deferred renderer stays on", TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRtSunFirst") ? "DeferredRtSunFirst " : "",
					skin ? "DeferredSkin " : "", hair ? "DeferredHair " : "", DeferredWetness::CompiledIn() ? "DeferredWetness's film highlight" : "");
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
		DeferredWetness::BeginFrame();	// (2026-11-19: no film until its pass draws one this frame)
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
		const CachedSettings& cfg = SyncSettings();
		// the land's c3 read-back (core/LandLight.h) only while the pass may use it: the deferred renderer in mode 2 or the pass's own switch
		LandLight::ReadColor = SettingForce::Mode() >= 2 || cfg.enabled;
		if (LinearScene::DrawWatch == &LandDrawWatch || LinearScene::DrawWatch == &ObjectDrawWatch) LinearScene::DrawWatch = nullptr;	// (no watch across frames)
		DebugView = cfg.debugView;
		Shadows = cfg.shadows;
		LandCrackShadow = cfg.landCrackShadow;
		AuthoredHighlight = cfg.authoredHighlight;
		MatchSunDisc = cfg.matchSunDisc;
		SunSheen = cfg.sunSheen;
		SunSheenRoughness = cfg.sunSheenRoughness;
		if (SpecFadeCompiled < 0) SpecFadeCompiled = cfg.specFadeLive ? 1 : 0;
		HighlightFade = cfg.highlightFade && SpecFadeCompiled == 1 && cfg.specFadeLive;	// (as the records fade: compiled at startup, live while compiled)
		{
			static int statusSlot = -1;
			const bool asked = Forced || cfg.enabled;
			const char* why = Live ? (Forced ? "live (the deferred renderer)" : "live (Enabled)") : !asked ? "off" : PassFailed ? "the pass failed to load" :
				Fail.failed ? Refused : !LinearScene::Live ? "needs LinearScene live" : "needs CapturedNormals + MaterialBuffer + AlbedoBuffer";
			EffectStatus::Note(statusSlot, "DeferredSun", Live ? EffectStatus::Drawn : EffectStatus::Skipped, why);
		}
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED SUN %u frames: the pass %.2f a frame (compute %.2f; the exact linear depth %.2f; the frame converted to linear light first %.2f; the exact stored-curve fog %.2f), skipped %.2f, mixed spans lit (sun-keeping records in the same span, told apart by their code) %.2f%s%s",
			(unsigned)StatFrames, StatPasses / f, StatCompute / f, StatLinearDepth / f, StatConverted / f, StatFogExact / f, StatSkipped / f, StatRefusedMixed / f,
			Fail.failed ? (LatchFinal() ? "; off for the session: " : "; off until the next cell change: ") : "", Fail.failed && Refused ? Refused : "");
		StatFrames = 0; StatPasses = StatCompute = StatSkipped = StatRefusedMixed = StatLinearDepth = StatConverted = StatFogExact = 0;
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

	inline void LogLight(const D3DXVECTOR3& toSun, const NiDirectionalLight* sun, bool landFresh);

	// UNOFFICIAL 2026-11-21 (Phase 2): Single-pass Vulkan Compute DeferredSun (Compute\DeferredSun.spv).
	// Eliminates saving/restoring 224 pixel constants, 208 sampler states, 15 render states, DeferredFog::FrameCopy,
	// and DEPTH output while supporting both full resolution and RenderScale < 1.0 natively.
	inline bool TryApplyCompute(IDirect3DDevice9* d, IDirect3DSurface9* target, const D3DSURFACE_DESC& t, bool linearDepth, const NiDirectionalLight* sun) {
		if (ComputeFailed || Vm.on || SunFadeDeferred::Fading || SkylightingMode1) return false;
		// (2026-11-26 upstream 1.1.4 merge, review) Compute\DeferredSun.comp has no DS_WET_FILM: with DeferredWetness's film live this
		// frame the D3D9 pass draws, so the film's sun highlight stays
		if (WetFilmCompiled && DeferredWetness::FilmLive) return false;
		// (2026-11-26) Compute\DeferredSun.comp has no screen-space shadows lookup: with this span's mask the D3D9 pass draws
		if (ScreenSpaceShadows::MadeThisSpan()) return false;
		if (!DxvkCompute::PassesOnRect() || !DxvkCompute::Available() || DxvkCompute::KeptOnD3D9("DeferredSun")) return false;
		const CachedSettings& cfg = SyncSettings();
		if (cfg.nvrDxvkDeferred == 0) return false;
		// Storage image RMW requires single-sample FP16 (identity RGBA swizzle in DXVK NVR ext)
		if (t.Format != D3DFMT_A16B16G16R16F || t.MultiSampleType != D3DMULTISAMPLE_NONE) return false;
		const bool depthIsLinear = DeferredDepth::LinearReady();
		IDirect3DTexture9* depthTex = depthIsLinear ? DeferredDepth::LinearTexture : (DeferredDepth::Ready() ? DeferredDepth::Texture : nullptr);
		if (!depthTex) return false;

		struct ComputePush {
			UINT linearSampler;
			UINT pointSampler;
			UINT flags;
			UINT pad;
		};
		struct ComputeConstants {
			int   viewport[4];               // 0
			float dd[6][4];                  // 1..6
			float dsSun[4];                  // 7
			float dsSunColor[4];             // 8
			float dsCtrl[4];                 // 9
			float dsDepth[4];                // 10
			float dsFog[4];                  // 11
			float dsLandSun[4];              // 12
			float dsLandColor[4];            // 13
			float dsVis[4];                  // 14
			float terrainData[4];            // 15
			float terrainExtraData[4];       // 16
			float terrainParallaxIso[4];     // 17
			float pbrData[4];                // 18
			float pbrExtraData[4];           // 19
			float linearLighting[4];         // 20
			float pbrMath[4];                // 21
			float skinMaterial[4];           // 22
			float skinLook[4];               // 23
			float skinDebug[4];              // 24
			float skinSSS[4];                // 25
			float skyIrradiance[9][4];       // 26..34
			float shadowNear[16];            // 35..38
			float shadowMiddle[16];          // 39..42
			float shadowFar[16];             // 43..46
			float shadowLod[16];             // 47..50
			float shadowNearCenter[4];       // 51
			float shadowMiddleCenter[4];     // 52
			float shadowFarCenter[4];        // 53
			float shadowLodCenter[4];        // 54
			float shadowFormatData[4];       // 55
			float shadowFade[4];             // 56
			float smoothedSunDir[4];         // 57
			float shadowBlur[4];             // 58
			float shadowForwardData[4];      // 59
			float contactHardeningData[4];   // 60
			float charShadowCameraToLight[16]; // 61..64
			float charShadowCenter[4];       // 65
			float charShadowData[4];         // 66
			float cloudSun[4];               // 67
			float rtSunRowX[4];              // 68
			float rtSunRowY[4];              // 69
			float rtSunRowW[4];              // 70
			float rtSunData[4];              // 71
			float dsSheen[4];                // 72 (2026-11-26 SunSheen: FillSheenConst; Compute\DeferredSun.comp cb.dsSheen)
		};
		static_assert(sizeof(ComputeConstants) == 73 * 16, "DeferredSun ComputeConstants layout mismatch");

		auto copyBytes = [](void* dst, const char* name, size_t bytes) {
			auto found = TheShaderManager->ConstantsTable.find(name);
			if (found != TheShaderManager->ConstantsTable.end() && found->second) memcpy(dst, found->second, bytes);
		};
		auto namedTex = [](const char* name) -> IDirect3DBaseTexture9* {
			auto found = TheTextureManager->TextureNames.find(name);
			return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
		};

		if (!ComputePipeline) {
			if (ComputeTried) return false;
			ComputeTried = true;
			static const NVR_EXT_BINDING_DESC kBindings[] = {
				{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_albedo
				{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_material
				{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_normals
				{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_depth
				{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE,  4, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_shadowAtlas
				{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE,  5, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_charShadowMap
				{ 1, 6, NVR_EXT_BIND_SAMPLED_IMAGE,  6, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_cloudShadow
				{ 1, 7, NVR_EXT_BIND_SAMPLED_IMAGE,  7, NVR_EXT_VIEW_2D, FALSE, 0 }, // t_rtSunForward
				{ 1, 8, NVR_EXT_BIND_STORAGE_IMAGE,  8, NVR_EXT_VIEW_2D, TRUE,  0 }, // i_scene
				{ 1, 9, NVR_EXT_BIND_STORAGE_IMAGE,  9, NVR_EXT_VIEW_2D, TRUE,  0 }, // i_skinSpec
				{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 }, // Constants
				{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 }, // linearSampler
				{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 4 }, // pointSampler
			};
			ComputePipeline = DxvkCompute::CreatePipeline("DeferredSun", kBindings, _countof(kBindings), sizeof(ComputePush), true);
			if (ComputePipeline) ComputeUbo = DxvkCompute::CreateBuffer(sizeof(ComputeConstants), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "DeferredSun UBO");
			if (!ComputePipeline || !ComputeUbo) {
				DxvkCompute::Drop(ComputePipeline);
				DxvkCompute::Drop(ComputeUbo);
				ComputeFailed = true;
				Logger::Log("[WARNING] UNOFFICIAL DeferredSun: Compute\\DeferredSun.spv could not be set up; using D3D9 pass");
				return false;
			}
			Logger::Log("UNOFFICIAL DeferredSun: Vulkan compute path ready (Compute\\DeferredSun.spv)");
		}
		if (!ComputeRt0DummySurf) {
			if (FAILED(d->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &ComputeRt0DummyTex, NULL)) ||
				FAILED(ComputeRt0DummyTex->GetSurfaceLevel(0, &ComputeRt0DummySurf))) {
				ComputeFailed = true;
				return false;
			}
		}
		if (!ComputeSpecDummySurf) {
			if (FAILED(d->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &ComputeSpecDummyTex, NULL)) ||
				FAILED(ComputeSpecDummyTex->GetSurfaceLevel(0, &ComputeSpecDummySurf))) {
				ComputeFailed = true;
				return false;
			}
		}

		D3DVIEWPORT9 vp = {};
		d->GetViewport(&vp);
		if (vp.Width == 0 || vp.Height == 0) return false;

		ComputeConstants cb = {};
		cb.viewport[0] = (int)vp.X; cb.viewport[1] = (int)vp.Y; cb.viewport[2] = (int)vp.Width; cb.viewport[3] = (int)vp.Height;
		memcpy(cb.dd, DeferredDepth::Frame, sizeof(cb.dd));

		copyBytes(cb.terrainData, "TESR_TerrainData", 16);
		copyBytes(cb.terrainExtraData, "TESR_TerrainExtraData", 16);
		copyBytes(cb.terrainParallaxIso, "TESR_TerrainParallaxIso", 16);
		copyBytes(cb.pbrData, "TESR_PBRData", 16);
		copyBytes(cb.pbrExtraData, "TESR_PBRExtraData", 16);
		copyBytes(cb.linearLighting, "TESR_LinearLighting", 16);
		copyBytes(cb.pbrMath, "TESR_PBRMath", 16);
		if (DeferredLamps::SkinCompiledIn()) {
			copyBytes(cb.skinMaterial, "TESR_SkinMaterial", 16);
			copyBytes(cb.skinLook, "TESR_SkinLook", 16);
			copyBytes(cb.skinDebug, "TESR_SkinDebug", 16);
			copyBytes(cb.skinSSS, "TESR_SkinSSS", 16);
			copyBytes(cb.skyIrradiance, "TESR_SkyIrradianceObjects", sizeof(cb.skyIrradiance));
		}
		copyBytes(cb.shadowNear, "TESR_ShadowCameraToLightTransformNear", 64);
		copyBytes(cb.shadowMiddle, "TESR_ShadowCameraToLightTransformMiddle", 64);
		copyBytes(cb.shadowFar, "TESR_ShadowCameraToLightTransformFar", 64);
		copyBytes(cb.shadowLod, "TESR_ShadowCameraToLightTransformLod", 64);
		copyBytes(cb.shadowNearCenter, "TESR_ShadowNearCenter", 16);
		copyBytes(cb.shadowMiddleCenter, "TESR_ShadowMiddleCenter", 16);
		copyBytes(cb.shadowFarCenter, "TESR_ShadowFarCenter", 16);
		copyBytes(cb.shadowLodCenter, "TESR_ShadowLodCenter", 16);
		copyBytes(cb.shadowFormatData, "TESR_ShadowFormatData", 16);
		copyBytes(cb.shadowFade, "TESR_ShadowFade", 16);
		copyBytes(cb.smoothedSunDir, "TESR_SmoothedSunDir", 16);
		copyBytes(cb.shadowBlur, "TESR_ShadowBlur", 16);
		copyBytes(cb.shadowForwardData, "TESR_ShadowForwardData", 16);
		if (ContactHardeningCompiled) copyBytes(cb.contactHardeningData, "TESR_ContactHardeningData", 16);
		if (CharMapCompiled) {
			copyBytes(cb.charShadowCameraToLight, "TESR_CharShadowCameraToLight", 64);
			copyBytes(cb.charShadowCenter, "TESR_CharShadowCenter", 16);
			copyBytes(cb.charShadowData, "TESR_CharShadowData", 16);
		}
		if (RtSunCompiled) {
			copyBytes(cb.rtSunRowX, "TESR_RtSunRowX", 16);
			copyBytes(cb.rtSunRowY, "TESR_RtSunRowY", 16);
			copyBytes(cb.rtSunRowW, "TESR_RtSunRowW", 16);
			copyBytes(cb.rtSunData, "TESR_RtSunData", 16);
		}

		// Non-PCF shadow mode fallback (if any non-standard configuration ever reaches here)
		if (Shadows && ForwardShadowsCompiled && cb.shadowForwardData[0] == 0.0f &&
			cb.shadowFormatData[2] != 0.0f && cb.shadowFade[1] != 0.0f && cb.shadowFormatData[0] != 3.0f) {
			return false;
		}

		IDirect3DBaseTexture9* atlasTex = namedTex("TESR_ShadowAtlas");
		IDirect3DBaseTexture9* charTex = CharMapCompiled ? namedTex("TESR_CharShadowMap") : nullptr;
		IDirect3DBaseTexture9* cloudTex = (ForwardShadowsCompiled && ForwardCloudShadows::Active) ? namedTex("TESR_CloudShadow") : nullptr;
		IDirect3DBaseTexture9* rtSunTex = RtSunCompiled ? namedTex("TESR_RtSunForward") : nullptr;

		if (cloudTex) {
			memcpy(cb.cloudSun, ForwardCloudShadows::Data, 16);
		}
		const float foliageW = ForwardCloudShadows::FoliageW(Pass && Pass->CloudSunReg && ForwardCloudShadows::Active);

		const NiPoint3& dir = sun->direction;
		D3DXVECTOR3 toSun(-dir.x, -dir.y, -dir.z);
		bool has = false;
		const float discBlend = BlendMatchSunDisc(toSun, has);
		const bool objectFresh = LandLight::ObjectReadFrame && (UInt32)TheFrameRateManager->FrameNumber - LandLight::ObjectReadFrame <= 4;
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		cb.dsSun[0] = toSun.x; cb.dsSun[1] = toSun.y; cb.dsSun[2] = toSun.z; cb.dsSun[3] = has ? 1.0f : 0.0f;
		cb.dsSunColor[0] = objectFresh ? LandLight::ObjectColor[0] : sun->Diff.r * sun->Dimmer;
		cb.dsSunColor[1] = objectFresh ? LandLight::ObjectColor[1] : sun->Diff.g * sun->Dimmer;
		cb.dsSunColor[2] = objectFresh ? LandLight::ObjectColor[2] : sun->Diff.b * sun->Dimmer;
		cb.dsSunColor[3] = foliageW;
		cb.dsCtrl[0] = (float)DebugView;
		cb.dsCtrl[1] = linearDepth ? 1.0f : 0.0f;
		cb.dsCtrl[2] = Shadows ? 1.0f : 0.0f;
		cb.dsCtrl[3] = FoliageCompiled && cfg.grassShadowFix ? 2.0f : 1.0f;
		cb.dsDepth[0] = proj._33; cb.dsDepth[1] = proj._43; cb.dsDepth[2] = 0.01f; cb.dsDepth[3] = 2.0f;

		const UInt32 frameNow = (UInt32)TheFrameRateManager->FrameNumber;
		const bool landFresh = LandLight::ReadFrame && frameNow - LandLight::ReadFrame <= 4;
		D3DXVECTOR3 blendedLandDir;
		bool landDirValid = false;
		const float* const landDir = ResolveLandDir(toSun, has, landFresh, discBlend, blendedLandDir, landDirValid);
		cb.dsLandSun[0] = landDir[0]; cb.dsLandSun[1] = landDir[1]; cb.dsLandSun[2] = landDir[2]; cb.dsLandSun[3] = landDirValid ? 1.0f : 0.0f;
		cb.dsLandColor[0] = LandLight::Color[0]; cb.dsLandColor[1] = LandLight::Color[1]; cb.dsLandColor[2] = LandLight::Color[2];
		cb.dsLandColor[3] = (landFresh && LandLight::ColorGood) ? 1.0f : 0.0f;

		{
			const float start = *BSShaderManager::fSpecularLODStartFade, end = *BSShaderManager::fSpecularLODEnd;
			const bool fade = HighlightFade && end > start + 1.0f;
			cb.dsVis[0] = LandCrackShadow ? 1.0f : 0.0f;
			cb.dsVis[1] = AuthoredHighlight ? 1.0f : 0.0f;
			cb.dsVis[2] = fade ? start : 0.0f;
			cb.dsVis[3] = fade ? 1.0f / (end - start) : 0.0f;
		}
		LogLight(toSun, sun, landFresh);

		float fog[4] = {};
		const bool fogExact = LinearCompiled && DebugView == 0 && DeferredFog::Params(fog);
		if (fogExact) memcpy(cb.dsFog, fog, 16);
		FillSheenConst(false, false, cb.dsSheen);	// (2026-11-26 SunSheen: this pass never runs for the view model or with the film live, above)

		UINT flags = 0;
		if (GBufferV2()) flags |= 1u << 0u;
		if (LinearCompiled) flags |= 1u << 1u;
		if (DeferredLamps::SkinCompiledIn()) flags |= 1u << 2u;
		if (DeferredLamps::SkinCompiledIn() && DeferredLamps::SkinSpecSurface) flags |= 1u << 3u;
		if (FoliageCompiled) flags |= 1u << 4u;
		if (LightFreeRecords::HairDeferred()) flags |= 1u << 5u;
		if (depthIsLinear) flags |= 1u << 6u;
		if ((1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) != 0.0) flags |= 1u << 7u;
		if (DepthAgreeCompiled) flags |= 1u << 8u;
		if (ForwardShadowsCompiled && atlasTex) flags |= 1u << 9u;
		if (ContactHardeningCompiled && atlasTex) flags |= 1u << 10u;
		if (CharMapCompiled && charTex && atlasTex) flags |= 1u << 11u;
		if (RtSunCompiled && rtSunTex) flags |= 1u << 12u;
		if (RtSunCompiled && RtSunFirstCompiled && rtSunTex) flags |= 1u << 13u;
		if (cfg.smithMode == 1) flags |= 1u << 14u;
		else if (cfg.smithMode == 2) flags |= 1u << 15u;
		else flags |= 1u << 16u;
		if (ForwardShadowsCompiled && cloudTex) flags |= 1u << 17u;
		if (SlackCapAllCompiled) flags |= 1u << 18u;

		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (FAILED(ext->UpdateBuffer(ComputeUbo, 0, sizeof(cb), &cb))) return false;

		IDirect3DSurface9* skinSpecTarget = (DeferredLamps::SkinCompiledIn() && DeferredLamps::SkinSpecSurface)
			? DeferredLamps::SkinSpecSurface : ComputeSpecDummySurf;
		NVR_EXT_RESOURCE_BINDING bindings[] = {
			{ 0, NVR_EXT_BIND_SAMPLED_IMAGE,  CapturedNormals::AlbedoTexture,   0, 0, 0, FALSE },
			{ 1, NVR_EXT_BIND_SAMPLED_IMAGE,  CapturedNormals::MaterialTexture, 0, 0, 0, FALSE },
			{ 2, NVR_EXT_BIND_SAMPLED_IMAGE,  CapturedNormals::Texture,         0, 0, 0, FALSE },
			{ 3, NVR_EXT_BIND_SAMPLED_IMAGE,  depthTex,                         0, 0, 0, FALSE },
			{ 4, NVR_EXT_BIND_SAMPLED_IMAGE,  atlasTex ? atlasTex : depthTex,   0, 0, 0, FALSE },
			{ 5, NVR_EXT_BIND_SAMPLED_IMAGE,  charTex ? charTex : depthTex,     0, 0, 0, FALSE },
			{ 6, NVR_EXT_BIND_SAMPLED_IMAGE,  cloudTex ? cloudTex : depthTex,   0, 0, 0, FALSE },
			{ 7, NVR_EXT_BIND_SAMPLED_IMAGE,  rtSunTex ? rtSunTex : depthTex,   0, 0, 0, FALSE },
			{ 8, NVR_EXT_BIND_STORAGE_IMAGE,  target,                           0, 0, 0, FALSE },
			{ 9, NVR_EXT_BIND_STORAGE_IMAGE,  skinSpecTarget,                   0, 0, 0, FALSE },
			{ 0, NVR_EXT_BIND_UNIFORM_BUFFER, ComputeUbo,                       0, 0, sizeof(cb), FALSE },
			{ 0, NVR_EXT_BIND_SAMPLER,        DxvkCompute::LinearClamp(),       0, 0, 0, FALSE },
			{ 1, NVR_EXT_BIND_SAMPLER,        DxvkCompute::PointClamp(),        0, 0, 0, FALSE },
		};
		ComputePush push = { 0, 0, flags, 0 };
		NVR_EXT_DISPATCH_DESC dispatch = {};
		dispatch.pPipeline = ComputePipeline;
		dispatch.BindingCount = _countof(bindings);
		dispatch.pBindings = bindings;
		dispatch.pPushData = &push;
		dispatch.PushDataSize = sizeof(push);
		dispatch.GroupsX = (vp.Width + 7u) / 8u;
		dispatch.GroupsY = (vp.Height + 7u) / 8u;
		dispatch.GroupsZ = 1u;

		static GpuTimer computeTimer("  Deferred sun (compute)");
		GpuProfileScope gpu(computeTimer, d);
		DxvkCompute::PreLoad(bindings, _countof(bindings));
		d->SetRenderTarget(0, ComputeRt0DummySurf);
		const HRESULT hr = ext->Dispatch(&dispatch);
		d->SetRenderTarget(0, target);
		d->SetViewport(&vp);
		if (FAILED(hr)) {
			ComputeFailed = true;
			Logger::Log("[WARNING] UNOFFICIAL DeferredSun: compute dispatch failed (%08lX); falling back to D3D9 pass", hr);
			return false;
		}
		StatPasses++;
		StatCompute++;
		if (fogExact) StatFogExact++;
		if (SkipStreak && SkipLogs <= 21) Logger::Log("UNOFFICIAL DeferredSun: the sun pass drew again after %u skipped frame%s", SkipStreak, SkipStreak == 1 ? "" : "s");
		SkipStreak = 0;
		static int latchTold = 0;
		if (Fail.tries != latchTold) { latchTold = Fail.tries; Logger::Log("UNOFFICIAL DeferredSun: the sun pass draws again after its latch (attempt %d of %d)", Fail.tries + 1, FailOnce::kTries); }
		return true;
	}

	// The pass (DeferredSpan, Render.cpp): after DeferredDepth::AtSpanEnd, before DeferredLamps::Apply.
	inline void Apply(IDirect3DDevice9* d) {
		const unsigned binds = LightFreeBindsThisSpan, older = OlderBindsThisSpan;
		LightFreeBindsThisSpan = OlderBindsThisSpan = 0;
		// UNOFFICIAL 2026-11-19 DeferredWetness: on every return below, the land's COLOR2.r code put back if its pass did not run (the world's view,
		// a span with lighting-free binds, also when the pass is not live)
		struct WetGuard { IDirect3DDevice9* d; bool on; ~WetGuard() { if (on) DeferredWetness::EnsureRestored(d); } } wetGuard{ d, !Vm.on && binds != 0 };
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


		const bool linearDepth = !Vm.on && DeferredDepth::EnsureLinear(d);	// (the world's view z: not the view model's)
		// UNOFFICIAL 2026-11-19 DeferredWetness (core/DeferredWetness.h): the land's water layer, now that the frame is linear and before the sun
		// lights the albedo and normal it makes wet (it saves and puts back every state it changes; the world's view only)
		if (!Vm.on) DeferredWetness::Apply(d, target, linearDepth);
		if (!DeferredDepth::Ready() && !linearDepth && !Vm.on) DeferredDepth::FillFrame(d);	// the ray mapping (TESR_DeferredDepthFrame) without the copy
		if (!Vm.on) LogSources(linearDepth);
		if (linearDepth) StatLinearDepth++;
		if (TryApplyCompute(d, target, t, linearDepth, sun)) { target->Release(); return; }

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
		ForwardCloudShadows::SendFrame(d);	// (2026-11-16 fast-restart: always; strength 0 while off)
		// (2026-11-16 fast-restart) the foliage records' clouds: with ForwardShadows on, as every pixel's; off: none (SunFar shades them, as before)
		const float foliageW = ForwardCloudShadows::FoliageW(pass->CloudSunReg && ForwardCloudShadows::Active);
		// the pass's own (c6-c9)
		const NiPoint3& dir = sun->direction;
		D3DXVECTOR3 toSun(-dir.x, -dir.y, -dir.z);
		// UNOFFICIAL 2026-11-19 MatchSunDisc: outdoors by day, vanilla FNV clamps Tes->directionalLight (and land c18) to a
		// 45-degree southern plane (only 24.9 deg high at 14:17 while the sun disc and shadow maps are at 70.6 deg). Align
		// surface sun lighting to the true sun disc direction (ShaderConst.SunDir) while above the horizon so N.L matches
		// the sky and shadow cascades. Interiors and nighttime (moon) keep Tes->directionalLight unchanged.
		bool has = false;
		const float discBlend = BlendMatchSunDisc(toSun, has);
		const bool objectFresh = LandLight::ObjectReadFrame && (UInt32)TheFrameRateManager->FrameNumber - LandLight::ObjectReadFrame <= 4;
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		const float own[4][4] = {
			{ toSun.x, toSun.y, toSun.z, has ? 1.0f : 0.0f },
			// (2026-11-15 batch 6) the objects' sun colour as the game handed them this frame (c3 read back at a sun-lit object record's draw:
			// 1.21 x Diff x Dimmer in every recording so far), else the directional light's Diff x Dimmer as before
			{ objectFresh ? LandLight::ObjectColor[0] : sun->Diff.r * sun->Dimmer, objectFresh ? LandLight::ObjectColor[1] : sun->Diff.g * sun->Dimmer,
			  // w (2026-11-16 fd-merged-1116, FD-BATCH-D item 22): 1 = the forward clouds shade plants ([Shaders.VolumetricClouds.Main]
			  // PlantShadows), 0 = they do not: DeferredSun.hlsl leaves model 5's pixels out of the cloud term then, as forward's PassPlant does
			  // (2026-11-16 fast-restart) the clouds' strength for the foliage records' pixels (ForwardShadows on; 0 off), -1 = PlantShadows off
			  objectFresh ? LandLight::ObjectColor[2] : sun->Diff.b * sun->Dimmer, foliageW },
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
		D3DXVECTOR3 blendedLandDir;
		bool landDirValid = false;
		const float* const landDir = ResolveLandDir(toSun, has, landFresh, discBlend, blendedLandDir, landDirValid);
		const float land[2][4] = {
			{ landDir[0], landDir[1], landDir[2], landDirValid ? 1.0f : 0.0f },
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
		// (2026-11-19 DeferredWetness, DS_WET_FILM) c17.x 1 = the film target holds this frame's land film, on s12 (point; put back below)
		if (WetFilmCompiled) {
			const bool film = DeferredWetness::FilmLive && !Vm.on;
			const float wetFilm[4] = { film ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
			d->SetPixelShaderConstantF(17, wetFilm, 1);
			rs->SetTexture(12, film ? DeferredWetness::FilmTexture : nullptr);
			if (film) {
				TheRenderManager->SetSamplerState(12, D3DSAMP_MINFILTER, D3DTEXF_POINT); TheRenderManager->SetSamplerState(12, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				TheRenderManager->SetSamplerState(12, D3DSAMP_MIPFILTER, D3DTEXF_NONE); TheRenderManager->SetSamplerState(12, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				TheRenderManager->SetSamplerState(12, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); TheRenderManager->SetSamplerState(12, D3DSAMP_SRGBTEXTURE, FALSE);
			}
		}
		// (2026-11-26 SunSheen) c18 DS_Sheen (FillSheenConst); upstream 1.1.5-test4 put it in c18 too (our c17 is the wet film's, theirs MaterialSSS)
		{
			float sheen[4];
			FillSheenConst(Vm.on, WetFilmCompiled && DeferredWetness::FilmLive && !Vm.on, sheen);
			d->SetPixelShaderConstantF(18, sheen, 1);
		}
		LogLight(toSun, sun, landFresh);
		const float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(211, one, 1);	// NVR_SpecFade: full (no per-draw fade in a screen pass)
		const float zero[4] = {};
		d->SetPixelShaderConstantF(204, zero, 1);	// NVR_ShadowCharacterDraw (Shadow.hlsl, per draw for actors' heads): not an actor's draw
		// UNOFFICIAL 2026-11-26 Screen-Space Shadows (core/ScreenSpaceShadows.h): this span's mask (TESR_ScreenSpaceShadows, bound by name on s13
		// by SetCT) taken by the world's pass; never by the first-person pass (the view model is not in the depth the march read)
		{
			const bool sss = !Vm.on && ScreenSpaceShadows::MadeThisSpan();
			const float sssConst[4] = { sss ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
			d->SetPixelShaderConstantF(19, sssConst, 1);
			if (sss) ScreenSpaceShadows::UsedFrame = ScreenSpaceShadows::Now();	// the old contact pass stands aside this frame (EffectChain.cpp)
		}
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
