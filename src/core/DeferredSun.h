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
#include "DeferredWetness.h"	// UNOFFICIAL 2026-11-19 DeferredWetness: the land's water layer, drawn inside Apply; the film's sun highlight
#include "Sm6Record.h"	// UNOFFICIAL 2026-11-21 NvrDxvkDeferredResolve (the draw on the DXVK fork, below)
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
	// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/DeferredFirstPerson.h): Apply run a second time for the first-person view model, into its
	// light target (linear, no fog), at the stencil's view-model mark: frame is the first-person camera in the DD layout (c0-c5), depth its
	// projection's _33 / _43 (c9), charM / charCenter / charData the view model's sun map (c206-c209, c213, c214: the light's view x
	// projection, for the true camera-relative point; charData.w 0 = no map this frame). Off (always without the switch): Apply as before.
	struct ViewModelPass { bool on = false; float frame[6][4] = {}; float depth[2] = {}; float charM[4][4] = {}; float charCenter[4] = {}; float charData[4] = {}; };
	inline ViewModelPass Vm;

	// UNOFFICIAL 2026-11-21 NvrDxvkDeferredResolve ([Main.Main.Performance], live, test only; 0 = off, the shipped default; 1 = this pass on
	// Vulkan; 2 and 3 are the later steps' (docs/deferred-single-pass.md step plan). With 1, on the DXVK fork, Apply's draw is one Vulkan
	// full-screen Draw (core/Sm6Record.h) from the SPIR-V tools\nvr-sm6 built from the record's own cache text (Compute\Deferred\
	// DeferredSun-<key>.spv + .layout; key = ShaderRecord::SourceKey), after all of Apply's setup: the same pixel constants (read back from the
	// device), the textures on their stages, samplers matching their states, the D3D9 draw's depth test done by the hardware on the device's
	// depth-stencil surface against the shader's SV_Depth (VkDraw; 2026-11-21, the in-shader test discarded ground the D3D9 draw lights), the
	// device viewport, the scene and the skin highlight as the Draw's targets. Same picture by construction. Every gate below that does
	// not hold, and every refusal or failure, draws today's D3D9 quad in the same frame; a failure (a bad layout, a refused pipeline or Draw)
	// also latches this path off for the session (never DxvkCompute::Fail, which would turn every Vulkan pass off).
	inline int VkMode = 0;					// the switch this frame (BeginFrame)
	inline Sm6Record VkSun, VkSunFade;		// Pass's and PassFade's builds
	inline bool VkLatched = false;
	inline char VkLatchWhy[192] = "";
	inline IDirect3DSurface9* VkScratch = nullptr;	// RT0 during the Draw (a Draw's target must not be the device's render target)
	enum VkReason { kVkLatched, kVkPassesOff, kVkKept, kVkFirstPerson, kVkDebugView, kVkNoLinearZ, kVkNoSpirv, kVkTargets, kVkSizes, kVkRefused, kVkDepthRange, kVkProbe, kVkReasons };
	inline const char* const kVkReasonNames[kVkReasons] = { "latched off", "Vulkan passes off (NvrDxvkPasses, a RenderRect frame or no fork)",
		"NvrDxvkKeepD3D9", "first person", "debug view", "no linear view z", "no SPIR-V for the variant", "skin target without a 2-target build",
		"target / depth sizes differ", "a stage state refused", "viewport depth range not 0-1", "parity probe (14: the frame keeps D3D9)" };
	// TEST ONLY (2026-11-22, NvrDxvkDeferredResolve 14, the parity probe: ParityProbe below) the Vulkan side's reason when it did not draw
	inline char ProbeWhy[192] = "";
	inline unsigned long long VkStatVulkan = 0, VkStatD3D9 = 0, VkStatWhy[kVkReasons] = {};
	inline unsigned VkRefusalLogs = 0;
	inline void VkLatch(const char* why) {
		if (VkLatched) return;
		VkLatched = true;
		strncpy_s(VkLatchWhy, why ? why : "unknown", _TRUNCATE);
		Logger::Log("[WARNING] UNOFFICIAL DeferredSun: Vulkan resolve off for the session (%s): the D3D9 draw from now on; the other Vulkan passes stay on", VkLatchWhy);
	}
	// The record's build, looked for once (BeginFrame, the first frame the switch is on; or the first draw). A missing build is logged once and
	// kept on D3D9; a bad one latches.
	inline bool VkPrepare(Sm6Record& vk, ShaderRecordPixel* record, const char* which) {
		if (vk.Status == Sm6Record::State::NotTried && record) {
			const char* why = nullptr;
			const Sm6Record::State state = vk.Prepare("DeferredSun", record->SourceKey, NVR_EXT_BLEND_ADD, why);
			if (state == Sm6Record::State::Missing) Logger::Log("UNOFFICIAL DeferredSun: Vulkan resolve has no SPIR-V for key %016llx (D3D9 kept), the %s record", record->SourceKey, which);
			else if (state == Sm6Record::State::Failed) VkLatch(why);
			else Logger::Log("UNOFFICIAL DeferredSun: Vulkan resolve ready for the %s record (Compute\\%s.spv: %u registers, %u target%s, %u push bytes, depth %s)",
				which, vk.File.c_str(), vk.Registers, vk.TargetCount, vk.TargetCount == 1 ? "" : "s", vk.PushBytes,
				vk.WritesDepth ? "written (tested on the device's depth-stencil surface; NvrDxvkDeferredResolve 11 / 12 test it in the shader)" : "none");
		}
		return vk.Status == Sm6Record::State::Ready;
	}
	// Apply's draw on the fork (above). True = drawn there; false = draw the D3D9 quad (the reason counted; with the switch 0 nothing runs and
	// nothing is counted). target / t: the scene target (RT0) and its size; vp: the device viewport; zFunc: the D3D9 draw's depth test;
	// fogCopyBound: s0 holds the frame's copy this draw (otherwise whatever was bound last: a stand-in goes there).
	// probe (TEST ONLY, NvrDxvkDeferredResolve 14's ParityProbe): the Draw's one target instead of the scene (and the skin highlight), with
	// the overwrite pipeline; nothing counted or latched, a reason for not drawing left in ProbeWhy.
	inline bool VkDraw(IDirect3DDevice9* d, ShaderRecordPixel* pass, IDirect3DSurface9* target, const D3DSURFACE_DESC& t, const D3DVIEWPORT9& vp,
		const D3DXMATRIX& proj, DWORD zFunc, bool fogCopyBound, IDirect3DSurface9* probe = nullptr) {
		if (VkMode < 1) return false;
		auto onD3D9 = [probe](VkReason why) {
			if (probe) { strncpy_s(ProbeWhy, kVkReasonNames[why], _TRUNCATE); return false; }
			VkStatD3D9++; VkStatWhy[why]++; return false;
		};
		if (VkMode == 14 && !probe) return onD3D9(kVkProbe);	// (the probe's frames: the D3D9 quad, as at 0)
		if (VkLatched) return onD3D9(kVkLatched);
		if (!DxvkCompute::PassesOn()) return onD3D9(kVkPassesOff);	// (false in RenderRect frames; native D3D9 and stock DXVK: no extension)
		if (DxvkCompute::KeptOnD3D9("DeferredSun")) return onD3D9(kVkKept);
		if (Vm.on) return onD3D9(kVkFirstPerson);	// (DeferredFirstPerson's re-run: its stencil, step 5)
		// (the debug views return alpha 1 and write over the frame; NvrDxvkDeferredResolve 9, test only: drawn on Vulkan too, to compare a term at a time)
		if (DebugView != 0 && VkMode < 9) return onD3D9(kVkDebugView);
		if (!DeferredDepth::LinearReady()) return onD3D9(kVkNoLinearZ);	// (the in-shader depth test's reference)
		Sm6Record* vk = pass == Pass ? &VkSun : pass == PassFade ? &VkSunFade : nullptr;
		if (!vk || !VkPrepare(*vk, pass, pass == Pass ? "sun" : "sun-fade")) return onD3D9(VkLatched ? kVkLatched : kVkNoSpirv);
		// the targets: the scene, and SSS v2's skin highlight while it is bound for this pass (DeferredSun.hlsl COLOR1, FD_SKIN). The shader's
		// main returns alpha 0 into the scene, so writing every channel under the ADD blend equals the D3D9 draw's RGB mask.
		IDirect3DSurface9* const skin = probe ? nullptr : DeferredLamps::SkinSpecSurface;
		if (skin && vk->TargetCount < 2) return onD3D9(kVkTargets);
		IDirect3DSurface9* const targets[2] = { probe ? probe : target, skin };
		const UINT count = skin ? 2 : 1;
		if (skin) { D3DSURFACE_DESC s = {}; if (FAILED(skin->GetDesc(&s)) || s.Width != t.Width || s.Height != t.Height) return onD3D9(kVkSizes); }
		if (DeferredDepth::Width != t.Width || DeferredDepth::Height != t.Height) return onD3D9(kVkSizes);	// (zref is read at the target's pixel)
		if (!VkScratch && FAILED(d->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &VkScratch, NULL))) {
			VkScratch = nullptr;
			VkLatch("the 16 x 16 scratch render target could not be made");
			return onD3D9(kVkLatched);
		}
		// uvmap: the D3D9 quad's TEXCOORD0 (Apply: u = (x + 0.5) / W over the viewport) is FragCoord / size, whatever the viewport.
		// (bench diagnosis, test only) 13: the G-buffer uv half a texel right and down (u = (x + 1) / W), as if the D3D9 quad read on texel edges
		const float uvHalf = VkMode == 13 ? 0.5f : 0.0f;
		const float uvmap[4] = { 1.0f / (float)t.Width, 1.0f / (float)t.Height, uvHalf / (float)t.Width, uvHalf / (float)t.Height };
		// The depth test (2026-11-21 bench: the in-shader test, against either reference, discarded a band of mid-distance ground the D3D9
		// draw lights): the D3D9 draw's own, by the hardware. The device's current depth-stencil surface (the one the D3D9 draw is tested on)
		// attached, Apply's zFunc, no depth writes, no stencil (want[] has STENCILENABLE FALSE outside first person, which stays on D3D9), the
		// shader's SV_Depth tested: ztest.x 0. No surface bound: the D3D9 draw has no depth test either, and neither has this one.
		// Test values (bench A/B, test only): 10 no test at all; 11 the in-shader test against the INTZ copy (zref = TESR_DeferredDepth, read
		// with texelFetch); 12 the in-shader test against the view z rebuilt to device depth (the first build's: zref = the linear view z,
		// reference = _33 + _43 / z). ztest.w = Apply's compare against the shader's DEPTH (DS_Depth = c9, the same _33 / _43): 1 passes where
		// DEPTH >= reference (GREATEREQUAL, the game's reversed depth), 2 where <= (LESSEQUAL).
		const float ztestMode = VkMode == 11 ? 2.0f : VkMode == 12 ? 1.0f : 0.0f;
		const float ztest[4] = { ztestMode, proj._33, proj._43, zFunc == D3DCMP_GREATEREQUAL ? 1.0f : 2.0f };
		struct HeldSurface { IDirect3DSurface9* s = nullptr; ~HeldSurface() { if (s) s->Release(); } } ds;
		if (VkMode != 10 && ztestMode == 0.0f && (FAILED(d->GetDepthStencilSurface(&ds.s)) || !ds.s)) ds.s = nullptr;
		NVR_EXT_DEPTH_STENCIL depth = {};
		if (ds.s) {
			// The scene pass's depth range is 0-1 (the fork's DrawGeometryScene); the D3D9 draw's oDepth is clamped to the viewport's
			// MinZ-MaxZ (DXVK passes them as the Vulkan range). Only equal for 0-1: otherwise this frame keeps D3D9 (logged with the values).
			if (vp.MinZ != 0.0f || vp.MaxZ != 1.0f) {
				static unsigned rangeLogs = 0;
				if (rangeLogs++ < 5) Logger::Log("UNOFFICIAL DeferredSun: Vulkan resolve kept on D3D9 this frame: the viewport's depth range is %.6f-%.6f, the depth-attached draw's 0-1", vp.MinZ, vp.MaxZ);
				return onD3D9(kVkDepthRange);
			}
			depth.pResource = ds.s;
			depth.Depth = 0.0f;	// (ignored by a scene pass: the shader's SV_Depth)
			depth.DepthTest = TRUE;
			depth.DepthWrite = FALSE;
			depth.DepthCompare = (NVR_EXT_COMPARE)(zFunc - 1);	// (D3DCMPFUNC minus 1)
			depth.StencilTest = FALSE;
			depth.StencilCompare = NVR_EXT_COMPARE_ALWAYS;
			depth.StencilReadMask = depth.StencilWriteMask = 0xFF;
		}
		// RT0 swapped to the scratch around the Draw (the Draw leaves the device's state as it was), then put back before Apply's own viewport
		// restore: setting RT0 resets the viewport and the scissor rectangle, both put back here
		RECT scissor = {};
		const bool hadScissor = SUCCEEDED(d->GetScissorRect(&scissor));
		d->SetRenderTarget(0, VkScratch);
		const char* why = nullptr;
		Sm6Record::Result result;
		{
			static GpuTimer timer("Deferred sun (Vulkan)");
			GpuProfileScope gpu(timer, d);
			vk->Overwrite = probe || DebugView != 0;	// (bench diagnosis, NvrDxvkDeferredResolve 9-12) the debug views write, as the D3D9 draw does
			result = vk->Draw(d, targets, count, vp, uvmap, ztest, VkMode == 11 ? DeferredDepth::Texture : DeferredDepth::LinearTexture, ds.s ? &depth : nullptr,
				fogCopyBound ? 0u : 1u, why);
		}
		d->SetRenderTarget(0, target);
		if (hadScissor) d->SetScissorRect(&scissor);
		d->SetViewport(&vp);
		if (probe) {	// (TEST ONLY: no stats, no latch)
			if (result != Sm6Record::Result::Drawn) _snprintf_s(ProbeWhy, sizeof(ProbeWhy), _TRUNCATE, "%s: %s", result == Sm6Record::Result::Failed ? "failed" : "refused", why ? why : "");
			return result == Sm6Record::Result::Drawn;
		}
		if (result == Sm6Record::Result::Drawn) {
			VkStatVulkan++;
			if (vk->Frames == 1) {
				D3DSURFACE_DESC dsDesc = {};
				if (ds.s) ds.s->GetDesc(&dsDesc);
				Logger::Log("UNOFFICIAL DeferredSun: drawn on Vulkan (Compute\\%s.spv, %u target%s %ux%u); depth test: %s (depth-stencil %ux%u format %u, compare %lu, viewport depth %.3f-%.3f)",
					vk->File.c_str(), count, count == 1 ? "" : "s", t.Width, t.Height,
					ds.s ? "the hardware's, on the device's depth-stencil surface" : VkMode == 10 ? "none (test value 10)" : VkMode == 11 ? "in the shader against the INTZ copy (test value 11)" :
					VkMode == 12 ? "in the shader against the view z (test value 12)" : "none (no depth-stencil surface bound, as the D3D9 draw)",
					dsDesc.Width, dsDesc.Height, (unsigned)dsDesc.Format, zFunc, vp.MinZ, vp.MaxZ);
			}
			return true;
		}
		if (result == Sm6Record::Result::Failed) { VkLatch(why); return onD3D9(kVkLatched); }
		if (VkRefusalLogs++ < 10) Logger::Log("UNOFFICIAL DeferredSun: Vulkan resolve refused this frame (%s): the D3D9 draw (%u refusals so far)", why ? why : "", VkRefusalLogs);
		return onD3D9(kVkRefused);
	}

	// ---- TEST ONLY (2026-11-22): NvrDxvkDeferredResolve 14, the same-frame parity probe (docs/deferred-single-pass.md fact list; tests/parity_probe.py
	// reads its files). kProbeAfter frames after the switch becomes 14, ONE world-view Apply draws the pass twice more, inside Apply's own setup:
	// the D3D9 quad into A and the Vulkan Draw (overwrite pipeline, the depth attachment as value 1) into B, both float scratch targets cleared
	// to -1 first (pixels a side discarded or did not draw stay -1), blend off, every channel written, with the current DebugView. Then A, B and
	// the draw's stages 4 / 5 / 1 are read back and written to VegasShaders-Logs\parity_<frame>_{A,B,m,n,alb}.bin, and one line gives |A - B|.
	// The frame itself draws the D3D9 quad as at 0. Setting the switch off 14 and back shoots again. Nothing latches, nothing calls
	// DxvkCompute::Fail; the scratch targets are freed after the shot.
	constexpr unsigned kProbeAfter = 300;
	inline unsigned ProbeFrames = 0;	// frames with the switch at 14 (BeginFrame); reset when it leaves 14
	inline bool ProbeDone = false;
	// bytes a pixel for the formats the probe writes (0 = not written)
	inline UINT ProbeBpp(D3DFORMAT f) {
		switch (f) {
		case D3DFMT_A32B32G32R32F: return 16;
		case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 8;
		case D3DFMT_R32F: case D3DFMT_G16R16F: case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: return 4;
		case D3DFMT_R16F: return 2;
		default: return 0;
		}
	}
	// A render target's SYSTEMMEM copy (GetRenderTargetData), or null with why
	inline IDirect3DSurface9* ProbeRead(IDirect3DDevice9* d, IDirect3DSurface9* rt, D3DSURFACE_DESC& desc, const char*& why) {
		IDirect3DSurface9* sys = nullptr;
		why = nullptr;
		if (!rt) { why = "nothing bound"; return nullptr; }
		if (FAILED(rt->GetDesc(&desc))) { why = "no description"; return nullptr; }
		if (FAILED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys) { why = "no SYSTEMMEM surface"; return nullptr; }
		if (FAILED(d->GetRenderTargetData(rt, sys))) { sys->Release(); why = "GetRenderTargetData failed (not a render target?)"; return nullptr; }
		return sys;
	}
	// <logs>\parity_<frame>_<tag>.bin: a 28-byte header (char[4] "NVRP", uint32 version 1, width, height, D3DFORMAT, bytes a pixel, DebugView) then the rows,
	// tightly packed, in the surface's own format (A16B16G16R16F: half floats r g b a)
	inline bool ProbeWrite(IDirect3DSurface9* sys, const D3DSURFACE_DESC& desc, UInt32 frame, const char* tag) {
		const UINT bpp = ProbeBpp(desc.Format);
		if (!sys || !bpp) return false;
		CreateDirectoryA(".\\" VS_LOGS_FOLDER, NULL);
		char path[MAX_PATH];
		sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\parity_%u_%s.bin", frame, tag);
		D3DLOCKED_RECT lock = {};
		if (FAILED(sys->LockRect(&lock, NULL, D3DLOCK_READONLY))) return false;
		FILE* f = nullptr;
		bool ok = fopen_s(&f, path, "wb") == 0 && f;
		if (ok) {
			const UInt32 header[6] = { 1, desc.Width, desc.Height, (UInt32)desc.Format, bpp, (UInt32)DebugView };
			ok = fwrite("NVRP", 1, 4, f) == 4 && fwrite(header, sizeof(header), 1, f) == 1;
			for (UINT y = 0; ok && y < desc.Height; y++) ok = fwrite((const char*)lock.pBits + y * lock.Pitch, bpp, desc.Width, f) == desc.Width;
			fclose(f);
		}
		sys->UnlockRect();
		return ok;
	}
	inline void ParityProbe(IDirect3DDevice9* d, ShaderRecordPixel* pass, IDirect3DSurface9* target, const D3DSURFACE_DESC& t, const D3DVIEWPORT9& vp,
		const D3DXMATRIX& proj, DWORD zFunc, bool fogCopyBound, const float (*quad)[6]) {
		ProbeDone = true;
		const UInt32 frame = TheFrameRateManager ? (UInt32)TheFrameRateManager->FrameNumber : 0;
		// the draw's inputs as bound now, and the states asked about
		static const DWORD kStages[3] = { 4, 5, 1 };
		IDirect3DBaseTexture9* st[3] = {};
		for (int i = 0; i < 3; i++) d->GetTexture(kStages[i], &st[i]);
		DWORD maxMip = 0, lodBias = 0, srgb4 = 0, srgbWrite = 0, colorWrite = 0;
		d->GetSamplerState(4, D3DSAMP_MAXMIPLEVEL, &maxMip); d->GetSamplerState(4, D3DSAMP_MIPMAPLODBIAS, &lodBias); d->GetSamplerState(4, D3DSAMP_SRGBTEXTURE, &srgb4);
		d->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite); d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
		float bias = 0.0f; memcpy(&bias, &lodBias, 4);
		Logger::Log("TEST ONLY DeferredSun parity probe (NvrDxvkDeferredResolve 14) frame %u, DebugView %d: s4 %p (%s CapturedNormals::MaterialTexture %p), s5 %p (%s CapturedNormals::Texture %p), "
			"s1 %p (%s CapturedNormals::AlbedoTexture %p); s4 MAXMIPLEVEL %lu MIPMAPLODBIAS %.4f SRGBTEXTURE %lu; scene target format %d %ux%u, viewport %lu,%lu %lux%lu; "
			"device SRGBWRITEENABLE %lu COLORWRITEENABLE 0x%lX (Apply's; the probe draws write 0xF), zFunc %lu, fog copy on s0 %d",
			frame, DebugView, st[0], st[0] == CapturedNormals::MaterialTexture ? "==" : "!=", CapturedNormals::MaterialTexture, st[1], st[1] == CapturedNormals::Texture ? "==" : "!=",
			CapturedNormals::Texture, st[2], st[2] == CapturedNormals::AlbedoTexture ? "==" : "!=", CapturedNormals::AlbedoTexture, maxMip, bias, srgb4,
			(int)t.Format, t.Width, t.Height, vp.X, vp.Y, vp.Width, vp.Height, srgbWrite, colorWrite, zFunc, fogCopyBound ? 1 : 0);
		// the sentinel: a pixel shader writing -1 to every channel (no Clear can write a negative)
		IDirect3DPixelShader9* sentinel = nullptr;
		{
			static const char kSrc[] = "ps_2_0\ndef c0, -1, -1, -1, -1\nmov oC0, c0\n";
			ID3DXBuffer* code = nullptr;
			if (SUCCEEDED(D3DXAssembleShader(kSrc, sizeof(kSrc) - 1, NULL, NULL, 0, &code, NULL)) && code) {
				if (FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &sentinel))) sentinel = nullptr;
				code->Release();
			}
		}
		const float w = (float)t.Width - 0.5f, h = (float)t.Height - 0.5f;
		const float full[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w, -0.5f, 0, 1, 1, 0 }, { -0.5f, h, 0, 1, 0, 1 }, { w, h, 0, 1, 1, 1 } };
		RECT scissor = {};
		const bool hadScissor = SUCCEEDED(d->GetScissorRect(&scissor));
		DWORD zEnable = D3DZB_TRUE, blend = FALSE;
		d->GetRenderState(D3DRS_ZENABLE, &zEnable); d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		// A32B32G32R32F first; A16B16G16R16F if that cannot be made or the fork's Draw does not take it
		static const D3DFORMAT kFormats[2] = { D3DFMT_A32B32G32R32F, D3DFMT_A16B16G16R16F };
		IDirect3DTexture9* tex[2] = {}; IDirect3DSurface9* ab[2] = {};
		auto freeAB = [&]() { for (int i = 0; i < 2; i++) { if (ab[i]) ab[i]->Release(); if (tex[i]) tex[i]->Release(); ab[i] = nullptr; tex[i] = nullptr; } };
		bool vkDrawn = false;
		std::string tries;
		for (D3DFORMAT f : kFormats) {
			freeAB();
			char one[256];
			bool made = true;
			for (int i = 0; i < 2 && made; i++)
				made = SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &tex[i], NULL)) && tex[i] && SUCCEEDED(tex[i]->GetSurfaceLevel(0, &ab[i])) && ab[i];
			if (!made) { freeAB(); _snprintf_s(one, sizeof(one), _TRUNCATE, "%sformat %d: targets not made", tries.empty() ? "" : "; ", (int)f); tries += one; continue; }
			d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			for (int i = 0; i < 2; i++) {
				d->SetRenderTarget(0, ab[i]);
				if (sentinel) { d->SetPixelShader(sentinel); d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, full, sizeof(full[0])); }
				else d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
			}
			// A: the D3D9 quad with Apply's depth test, blend off, every channel
			d->SetRenderState(D3DRS_ZENABLE, zEnable);
			d->SetPixelShader(pass->ShaderHandle);
			d->SetRenderTarget(0, ab[0]);
			d->SetViewport(&vp);
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			// Apply's states and the scene as RT0 again, then B: the Vulkan Draw as value 1 draws it, into B through the overwrite pipeline
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, blend); d->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
			d->SetRenderTarget(0, target);
			if (hadScissor) d->SetScissorRect(&scissor);
			d->SetViewport(&vp);
			ProbeWhy[0] = 0;
			vkDrawn = VkDraw(d, pass, target, t, vp, proj, zFunc, fogCopyBound, ab[1]);
			_snprintf_s(one, sizeof(one), _TRUNCATE, "%sformat %d: Vulkan %s%s", tries.empty() ? "" : "; ", (int)f, vkDrawn ? "drawn" : "not drawn: ", vkDrawn ? "" : ProbeWhy);
			tries += one;
			if (vkDrawn) break;
		}
		const bool hadSentinel = sentinel != nullptr;
		if (sentinel) sentinel->Release();
		// read back and write: A, B, then the inputs
		IDirect3DSurface9* level[3] = {};
		for (int i = 0; i < 3; i++) if (st[i] && st[i]->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(st[i])->GetSurfaceLevel(0, &level[i]);
		IDirect3DSurface9* const sources[5] = { ab[0], ab[1], level[0], level[1], level[2] };
		static const char* const kTags[5] = { "A", "B", "m", "n", "alb" };
		IDirect3DSurface9* sys[5] = {};
		D3DSURFACE_DESC desc[5] = {};
		std::string files;
		for (int i = 0; i < 5; i++) {
			const char* why = nullptr;
			sys[i] = ProbeRead(d, sources[i], desc[i], why);
			const bool written = sys[i] && ProbeWrite(sys[i], desc[i], frame, kTags[i]);
			char one[160];
			_snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s %s (format %d)", files.empty() ? "" : ", ", kTags[i], written ? "written" : why ? why : "not written (format)", (int)desc[i].Format);
			files += one;
		}
		// |A - B| where neither side holds the sentinel
		double sum[4] = {}; float maxd[4] = {};
		unsigned long long sentA = 0, sentB = 0, onlyA = 0, onlyB = 0, compared = 0, nonFinite = 0;
		D3DLOCKED_RECT la = {}, lb = {};
		const bool sameFormat = sys[0] && sys[1] && desc[0].Format == desc[1].Format && (desc[0].Format == D3DFMT_A32B32G32R32F || desc[0].Format == D3DFMT_A16B16G16R16F);
		if (sameFormat && SUCCEEDED(sys[0]->LockRect(&la, NULL, D3DLOCK_READONLY))) {
			if (SUCCEEDED(sys[1]->LockRect(&lb, NULL, D3DLOCK_READONLY))) {
				const bool half = desc[0].Format == D3DFMT_A16B16G16R16F;
				for (UINT y = 0; y < desc[0].Height; y++) {
					const char* ra = (const char*)la.pBits + y * la.Pitch; const char* rb = (const char*)lb.pBits + y * lb.Pitch;
					for (UINT x = 0; x < desc[0].Width; x++) {
						float a[4], b[4];
						if (half) { D3DXFloat16To32Array(a, (const D3DXFLOAT16*)ra + x * 4, 4); D3DXFloat16To32Array(b, (const D3DXFLOAT16*)rb + x * 4, 4); }
						else { memcpy(a, (const float*)ra + x * 4, 16); memcpy(b, (const float*)rb + x * 4, 16); }
						const bool sa = a[0] == -1.0f && a[1] == -1.0f && a[2] == -1.0f && a[3] == -1.0f, sb = b[0] == -1.0f && b[1] == -1.0f && b[2] == -1.0f && b[3] == -1.0f;
						sentA += sa; sentB += sb; onlyA += sa && !sb; onlyB += sb && !sa;
						if (sa || sb) continue;
						bool finite = true;
						for (int c = 0; c < 4; c++) finite = finite && _finite(a[c]) && _finite(b[c]);
						if (!finite) { nonFinite++; continue; }
						compared++;
						for (int c = 0; c < 4; c++) { const float e = fabsf(a[c] - b[c]); sum[c] += e; if (e > maxd[c]) maxd[c] = e; }
					}
				}
				sys[1]->UnlockRect();
			}
			sys[0]->UnlockRect();
		}
		const double n = compared ? (double)compared : 1.0;
		Logger::Log("TEST ONLY DeferredSun parity probe frame %u, DebugView %d (%s; sentinel %s): mean |A-B| r %.6f g %.6f b %.6f a %.6f, max r %.6f g %.6f b %.6f a %.6f over %llu pixels; "
			"sentinel pixels A %llu, B %llu (only A %llu, only B %llu), non-finite %llu; files (VegasShaders-Logs\\parity_%u_*.bin): %s",
			frame, DebugView, tries.c_str(), hadSentinel ? "-1 (pixel shader)" : "0 (Clear: the -1 shader did not assemble)", sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n,
			maxd[0], maxd[1], maxd[2], maxd[3], compared, sentA, sentB, onlyA, onlyB, nonFinite, frame, files.c_str());
		for (int i = 0; i < 5; i++) if (sys[i]) sys[i]->Release();
		for (int i = 0; i < 3; i++) { if (level[i]) level[i]->Release(); if (st[i]) st[i]->Release(); }
		freeAB();
	}
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
		// UNOFFICIAL 2026-11-21 NvrDxvkDeferredResolve: the records below keep the key of their cache text (ShaderRecord::SourceKey), which
		// names their SPIR-V build; on for this function's loads only
		struct KeyGuard { KeyGuard() { ShaderRecord::KeepSourceKey = true; } ~KeyGuard() { ShaderRecord::KeepSourceKey = false; } } keyGuard;
		static D3DXMACRO overrides[] = { { "INTERIOR_SHADOWS", "0" }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL } };	// (room for the six optional defines + the terminator)
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
		// (2026-11-21) the keys, so a log says which Compute\Deferred\DeferredSun-<key>.spv the Vulkan resolve looks for
		Logger::Log("UNOFFICIAL DeferredSun: record keys (NvrDxvkDeferredResolve's SPIR-V names): sun %016llx, sun-fade %016llx", Pass->SourceKey, PassFade ? PassFade->SourceKey : 0ull);
	}

	// Once a frame (Render.cpp, with DeferredLamps::BeginFrame).
	inline void BeginFrame() {
		FdTestFault::BeginFrame();	// (2026-11-16 fd-batchB, test only: DebugVar4)
		DeferredWetness::BeginFrame();	// (2026-11-19: no film until its pass draws one this frame)
		Live = Wanted() && CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn() && !CapturedNormals::Failed;
		SunFadeDeferred::PassLive = Live && PassFade;	// (2026-11-15 fd-smoothsun: the sun steps by half a degree while this pass draws it)
		if (!WantedAtStartup()) return;
		// UNOFFICIAL 2026-11-21 NvrDxvkDeferredResolve (above): the switch, and the builds looked for (and their pipelines made) the first frame
		// it is on, so no frame waits for the file reads; nothing while it is 0 or without the fork
		VkMode = TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkDeferredResolve");
		if (VkMode == 14) ProbeFrames++; else { ProbeFrames = 0; ProbeDone = false; }	// (TEST ONLY: the parity probe, ParityProbe)
		if (VkMode >= 1 && !VkLatched && DxvkCompute::Available()) {
			static bool told = false;
			if (!told) { told = true; Logger::Log("UNOFFICIAL DeferredSun: NvrDxvkDeferredResolve %d: the sun pass draws on Vulkan where its build exists (test switch)", VkMode); }
			if (Pass) VkPrepare(VkSun, Pass, "sun");
			if (PassFade) VkPrepare(VkSunFade, PassFade, "sun-fade");
		}
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
		// (2026-11-21 NvrDxvkDeferredResolve) where the draws went: Vulkan, or D3D9 and why (counted per draw; the first-person re-run counts too).
		// Logged every 240 frames while the switch is on, with or without the F10 profiler (the bench reads it to prove the Vulkan draw).
		auto logVk = [](unsigned frames) {
			std::string why;
			for (int i = 0; i < kVkReasons; i++) if (VkStatWhy[i]) { char one[128]; _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s %llu", why.empty() ? "" : ", ", kVkReasonNames[i], VkStatWhy[i]); why += one; }
			Logger::Log("DEFERRED SUN VULKAN %u frames: NvrDxvkDeferredResolve %d; draws on Vulkan %llu, on D3D9 %llu%s%s%s%s%s", frames, VkMode, VkStatVulkan, VkStatD3D9,
				why.empty() ? "" : " (", why.c_str(), why.empty() ? "" : ")", VkLatched ? "; latched off: " : "", VkLatched ? VkLatchWhy : "");
			VkStatVulkan = VkStatD3D9 = 0;
			for (auto& n : VkStatWhy) n = 0;
		};
		if (!GpuTimer::Enabled) {
			StatFrames = 0;
			if (VkMode >= 1) { static unsigned vkFrames = 0; if (++vkFrames >= 240) { logVk(vkFrames); vkFrames = 0; } }
			else { VkStatVulkan = VkStatD3D9 = 0; for (auto& n : VkStatWhy) n = 0; }
			return;
		}
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED SUN %u frames: the pass %.2f a frame (the exact linear depth %.2f; the frame converted to linear light first %.2f; the exact stored-curve fog %.2f), skipped %.2f, mixed spans lit (sun-keeping records in the same span, told apart by their code) %.2f%s%s",
			(unsigned)StatFrames, StatPasses / f, StatLinearDepth / f, StatConverted / f, StatFogExact / f, StatSkipped / f, StatRefusedMixed / f,
			Fail.failed ? (LatchFinal() ? "; off for the session: " : "; off until the next cell change: ") : "", Fail.failed && Refused ? Refused : "");
		if (VkMode >= 1 || VkStatVulkan || VkStatD3D9) logVk((unsigned)StatFrames);
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


		const bool linearDepth = DeferredDepth::LinearReady() && !Vm.on;	// (the world's view z: not the view model's)
		// UNOFFICIAL 2026-11-19 DeferredWetness (core/DeferredWetness.h): the land's water layer, now that the frame is linear and before the sun
		// lights the albedo and normal it makes wet (it saves and puts back every state it changes; the world's view only)
		if (!Vm.on) DeferredWetness::Apply(d, target, linearDepth);
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
		LogLight(toSun, sun, landFresh);
		const float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(211, one, 1);	// NVR_SpecFade: full (no per-draw fade in a screen pass)
		const float zero[4] = {};
		d->SetPixelShaderConstantF(204, zero, 1);	// NVR_ShadowCharacterDraw (Shadow.hlsl, per draw for actors' heads): not an actor's draw
		// (2026-11-14) the stored-curve fog (core/DeferredFog.h, Includes/DeferredPixel.hlsl FDAddFogged): with the game's fog drawn, the fog
		// colour in c13 and the frame's copy (linear now) on s0, through the state cache like SetCT's stages (put back below)
		float fog[4] = {};	// (2026-11-15: zero for the first-person call, which uploads it unread)
		bool fogCopyBound = false;	// (2026-11-21 NvrDxvkDeferredResolve: s0 holds this draw's frame copy; else a stand-in for the Vulkan draw)
		if (!Vm.on && DeferredFog::Params(fog) && DebugView == 0) {
			IDirect3DTexture9* frameCopy = DeferredFog::FrameCopy(d, target);
			if (frameCopy) {
				fogCopyBound = true;
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
		// TEST ONLY (2026-11-22) NvrDxvkDeferredResolve 14: the parity probe, once, before this frame's own D3D9 quad (ParityProbe)
		if (VkMode == 14 && !Vm.on && !ProbeDone && ProbeFrames >= kProbeAfter) ParityProbe(d, pass, target, t, vp, proj, zFunc, fogCopyBound, quad);
		// UNOFFICIAL 2026-11-21 NvrDxvkDeferredResolve 1 (VkDraw above): the same draw on the DXVK fork; the switch 0, a gate that does not
		// hold, a refusal or a failure: this D3D9 quad, as before
		if (!VkDraw(d, pass, target, t, vp, proj, zFunc, fogCopyBound)) {
			if (DeferredLamps::SkinSpecSurface) { d->SetRenderTarget(1, DeferredLamps::SkinSpecSurface); d->SetViewport(&vp); d->GetRenderState(D3DRS_COLORWRITEENABLE1, &mask1); d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF); }
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			if (DeferredLamps::SkinSpecSurface) { d->SetRenderTarget(1, nullptr); d->SetViewport(&vp); d->SetRenderState(D3DRS_COLORWRITEENABLE1, mask1); }
		}
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
