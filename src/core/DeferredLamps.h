#pragma once
// UNOFFICIAL 2026-10-31 DeferredLamps (rung C, step C1: docs/next/DEFERRED-C1-SPEC.md; plan docs/next/DEFERRED-RENDERING-PLAN.md sections 5-7, 12).
// [Shaders.ShadowsInteriors.Deferred] Enabled (restart; NVR DXVK with ManyLampsOnePass + LightCommonLamps, the shadow atlas, the forward lamp
// shadows, CapturedNormals + MaterialBuffer + AlbedoBuffer, LinearScene): indoors the many-lamp shiny shaders (SLS2034-2036) draw from a fourth
// record without their lamps (LAMPS_DEFERRED: ambient, emittance, fog, and the G-buffer channels the lamps need), and ONE full-screen pass adds
// every lamp of each pixel's froxel (the lamp table's lists) with the object shaders' own maths and the full filtered atlas shadow, after the
// main list's G-buffer is resolved and before LinearScene encodes the frame. Live (A/B): false binds the forward Interior records again.
// Off: no record is loaded, nothing is hooked, the shaders are as before.
#include "ManyLamps.h"
#include "CapturedNormals.h"
#include "LinearScene.h"
#include "ShaderRecord.h"
#include "SettingForce.h"	// UNOFFICIAL 2026-11-14 full deferred renderer (core/DeferredRenderer.h) drives this pass while it is on
#include "LightFreeRecords.h"
#include "DeferredDepth.h"	// UNOFFICIAL 2026-11-14 DeferredDistant: the exact linear view z for the pass (DL_LINEAR_DEPTH)
#include "DeferredDistant.h"
#include "DeferredFog.h"	// UNOFFICIAL 2026-11-14 the stored-curve fog (DeferredLamps.hlsl DL_FOG_EXACT)

namespace DeferredLamps {
	inline const char* kSection = "Shaders.ShadowsInteriors.Deferred";

	inline bool WantedAtStartup() { return ManyLamps::DeferredAtStartup(); }
	inline bool IsFamily(const char* name) {	// SLS2034.pso, SLS2035.pso, SLS2036.pso (ManyLamps::IsOurs)
		return ManyLamps::IsOurs(name);
	}

	inline std::unordered_map<const void*, ShaderRecordPixel*> Records;	// NiD3DPixelShaderEx* -> its deferred record (the ExteriorRecords pattern)
	inline std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> ForwardOf;	// deferred handle -> the forward Interior handle
	inline ShaderRecordPixel* Pass = nullptr;
	inline bool PassFailed = false;	// the lamp pass did not compile (for the session)
	// UNOFFICIAL 2026-11-15 DeferredSkin (core/DeferredSkin.h; DeferredLamps.hlsl FD_SKIN / DL_SKIN_ONLY): the skin lamp pass, drawn right after the
	// lamp pass with the same state (it lights the deferred skin's pixels, which the lamp pass leaves). With [Main.Main.Quality] DeferredSkin,
	// DeferredRenderer and GBufferV2 at startup only; without it nothing changes; if it fails to load the deferred skin refuses itself.
	inline ShaderRecordPixel* SkinPass = nullptr;
	inline bool SkinPassFailed = false;
	// UNOFFICIAL 2026-11-15 SSS v2 (core/DeferredSkin.h): the skin's highlight target (fp16, the frame's size), cleared and set by
	// DeferredSkin::BeforePasses while DeferredSkinSSS 2 runs, null otherwise; DeferredSun and the skin lamp pass add the skin's highlights to it.
	inline IDirect3DSurface9* SkinSpecSurface = nullptr;
	inline int SkinWanted = -1;
	inline bool SkinCompiledIn() {
		if (SkinWanted < 0) SkinWanted = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredSkin") != 0 && SettingForce::Mode() > 0 && CapturedNormals::V2CompiledIn();
		return SkinWanted != 0;
	}
	inline FailOnce::Latch CopyFailed{ "DeferredLamps frame copy" };	// UNOFFICIAL 2026-11-02 RetryFailedEffects
	// UNOFFICIAL 2026-10-31 the shadow passes (DL_SHADOW_PASS): the full filter for lamps 0-15 of each pixel's list, four lamps a pass into one
	// A8R8G8B8 target each, read by the lighting pass (which has no temp registers left for the full filter next to the specular lighting).
	inline ShaderRecordPixel* ShadowPass = nullptr;
	inline bool ShadowPassFailed = false;
	inline IDirect3DTexture9* VisTex[4] = {};
	inline IDirect3DSurface9* Vis[4] = {};
	inline UINT VisWidth = 0, VisHeight = 0;
	inline bool Live = false;				// this frame: the switch on (read once a frame)
	// UNOFFICIAL 2026-11-14 full deferred renderer (core/DeferredRenderer.h): while [Main.Main.Quality] DeferredRenderer is on, it loads the
	// deferred records (every lit family, on any runtime) and decides when this pass runs (RendererLive, set by DeferredRenderer::BeginFrame
	// before this BeginFrame); the many-lamp records below are not made and ManyLamps::Active (DXVK) is not required.
	inline bool RendererLive = false;
	inline int Unshadowed = 1;
	inline int DebugView = 0;
	inline int Shadows = 1;
	// UNOFFICIAL 2026-11-16 fd-batchA (docs/next/FD-BATCH-A.md), live keys of [Shaders.ShadowsInteriors.Deferred], read once a frame:
	// JitterExact (item 7): the pass's view rays through the projection the list was drawn with, TAA's jitter included (as DeferredDepth's DDRay)
	// LandLampModel (item 3): the land's lamps with the forward land's model, in their own pass (DeferredLamps.hlsl DL_LAND_ONLY, DLLampLand)
	// ReplaceScreenShadows (item 2): with the pass's atlas shadows drawn this frame, the old screen-space lamp shadows are not laid over them
	inline bool JitterExact = true, LandLampModel = true, ReplaceScreenShadows = true;
	inline ShaderRecordPixel* LandPass = nullptr;	// the land lamp pass (DL_LAND_ONLY), drawn right after the lamp pass with the same state
	inline bool LandPassFailed = false;
	inline bool LandCompiled = false;			// the lamp pass has the split (DL_LAND_SPLIT, EnsurePass's level 0) and the land pass loaded
	inline bool Past16Unshadowed = false;		// the lamp pass loaded at a DL_PAST16_LIT level (lamps past a pixel's 16th lit unshadowed)
	inline bool ShadowedThisFrame = false;		// the world run drew its shadow passes with Shadows on this frame (reset in BeginFrame)
	// EffectChain.cpp PreStep_ShadowPlan: the deferred lamp pass shadowed its lamps from the atlas this frame, as the forward lamp-shadow shaders
	// do (PointShadowForward::BoundThisFrame), so the screen-space lamp shadows are left out the same way
	inline bool LampShadowsDrawn() { return ShadowedThisFrame && ReplaceScreenShadows; }
	inline UInt32 Frame = 0;
	inline unsigned long long StatFrames = 0, StatBinds = 0, StatPasses = 0, StatCopies = 0, StatRestored = 0, StatSkipped = 0, StatLinearZ = 0;	// (StatLinearZ: 2026-11-14 DL_LINEAR_DEPTH)
	inline unsigned DeferredBindsThisSpan = 0;
	inline const char* LampFalloffOverride = nullptr;	// the Interior records' LAMP_FALLOFF_ON override (ShaderManager.cpp), compiled into the pass too
	inline IDirect3DTexture9* CopyTex = nullptr;
	inline IDirect3DSurface9* Copy = nullptr;
	inline D3DSURFACE_DESC CopyMade = {};

	// UNOFFICIAL 2026-11-14 the stored-curve fog's resolve pass (DeferredLamps.hlsl DL_FOG_RESOLVE) and the lighting pass's own light target under fog
	inline ShaderRecordPixel* FogPass = nullptr;
	inline bool FogPassFailed = false;
	inline IDirect3DTexture9* LightTex = nullptr;
	inline IDirect3DSurface9* LightSurf = nullptr;
	inline UINT LightW = 0, LightH = 0;
	inline unsigned long long StatFogExact = 0;
	inline unsigned long long StatLand = 0;	// (fd-batchA) land lamp passes drawn
	// UNOFFICIAL 2026-11-16 fd-batchA item 2, DIAGNOSTIC (F10 profile only): which forward lamp-shadow shader keeps
	// PointShadowForward::BoundThisFrame set, and how often the screen-space lamp shadows still ran indoors
	inline char ForwardBinder[64] = {};		// the first forward lamp-shadow shader bound this frame ("" none)
	inline unsigned ForwardBindsFrame = 0;
	inline std::string ForwardBinderWindow;	// the binders seen in this stats window, by name
	inline unsigned long long StatIntFrames = 0, StatFwdBound = 0, StatOwnShadows = 0, StatScreenRan = 0;
	inline void NoteScreenShadows(bool interior, bool forwardBound, bool screenRuns) {	// EffectChain.cpp PreStep_ShadowPlan, once a frame
		if (!interior || !GpuTimer::Enabled) return;
		StatIntFrames++;
		if (forwardBound) StatFwdBound++;
		if (ShadowedThisFrame) StatOwnShadows++;
		if (screenRuns) StatScreenRan++;
		if (ForwardBinder[0] && ForwardBinderWindow.find(ForwardBinder) == std::string::npos && ForwardBinderWindow.size() < 400)
			ForwardBinderWindow += std::string(ForwardBinderWindow.empty() ? "" : ", ") + ForwardBinder;
	}
	inline bool FogCompiled() { return LightFreeRecords::WantedAtStartup() || CapturedNormals::V2CompiledIn(); }	// (the lighting pass has DL_FOG_EXACT)
	// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/DeferredFirstPerson.h): Apply run a second time for the first-person view model, into its
	// light target (linear, no fog), at the stencil's view-model mark: c holds c1-c7 (the screen share rescaled to the world camera's tiles,
	// the first-person axes with the world's tangents, the lamps' rows from kLightingOffset), depth the first-person projection's _33 / _43.
	// Off (on false, always without the switch): Apply exactly as before.
	struct ViewModelPass { bool on = false; float c[7][4] = {}; float depth[2] = {}; };
	inline ViewModelPass Vm;
	inline bool EnsurePass();			// (below)
	inline bool EnsureShadowShader();	// (below)

	// UNOFFICIAL 2026-11-15 DeferredTiledLight ([Main.Main.Quality], live, test only, default off; docs/next/FD-LIGHTPASS-RESULTS.md): the lamp
	// passes drawn only over the 64-pixel tiles whose froxel lists hold a lamp, instead of full-screen quads. The CPU made the lists this frame
	// (ManyLamps::BinFroxels), so it knows each tile's lamp count in each of its 8 depth slices; a tile's count here is the largest of its slices
	// (a pixel's slice is only known on the GPU). Classes: Quads[k] = the tiles holding more than 4k lamps: [0] the lighting pass, the skin pass
	// and the fog resolve (a pixel with no lamp adds nothing: its loop never runs, its raw light is 0), [k] shadow pass k (lamps 4k .. 4k + 3 of
	// the list: the lighting pass reads its target only for a lamp the pixel's list holds, k < count, so a tile with no more than 4k lamps never
	// reads target k there, and leaving it unwritten changes nothing). The same pixels the full-screen quads lit get the same light; the others
	// are not shaded. Full-screen as before when the switch is off, in the debug views (they write over every pixel), when the lists are not
	// this frame's, or when the pass's viewport is not the one the lists were made for (the tile edges below assume it is).
	namespace Tiles {
		struct Vertex { float x, y, z, w, u, v; };
		inline bool Live = false;		// this frame: the switch
		inline std::vector<Vertex> Quads[4];
		inline const char* Fallback = nullptr;	// why the last Apply drew full-screen quads (null: tiled)
		inline unsigned long long StatTiled = 0, StatFull = 0, StatTiles = 0, StatDrawn[4] = {};
		// The tile quads for this pass's viewport and target, or false = full-screen quads (Fallback says why).
		inline unsigned long long StatViewModel = 0;	// (fd-batch8) the first-person runs, always full-screen (not in StatFull)
		inline bool Build(const D3DVIEWPORT9& vp, const D3DSURFACE_DESC& t, bool lists) {
			for (auto& q : Quads) q.clear();
			// (fd-batch8 merge) DeferredFirstPerson's run (Vm.on): the lists' tiles are the world camera's and the view model's pixels map to
			// them through the rescaled DL_View, not one to one, so its passes stay full-screen (the stencil mark limits them); the world's
			// Fallback reason and counts are left as the world pass set them
			if (Vm.on) { if (Live) StatViewModel++; return false; }
			Fallback = !Live ? "off" : DebugView >= 2 ? "a debug view" : !lists || ManyLamps::CountsFrame != ManyLamps::FrameId ? "no lists this frame" :
				ManyLamps::TilesX <= 0 || ManyLamps::TilesY <= 0 ? "no tiles" :
				(float)vp.Width != ManyLamps::BinWidth || (float)vp.Height != ManyLamps::BinHeight ? "the pass's viewport is not the lists'" :
				ManyLamps::TileSize < 64 || (int)(vp.Width + ManyLamps::TileSize - 1) / ManyLamps::TileSize > 64 ? "an unexpected tile size" : nullptr;
			if (Fallback) { if (Live) StatFull++; return false; }
			// The shader's tile (DeferredLamps.hlsl DLPixel): min(floor(saturate((vpos - corner + 0.5) / size) x lists' size / S), the grid - 1),
			// S = ManyLamps::TileSize (64, larger past 2,560 x 1,536); with the pass's size the lists' size that is the pixel's offset / S: tile n
			// covers pixels S n .. S n + S - 1 of the viewport (the last one cut at its edge), and the grid holds every tile (TileSizeFor).
			// Its list: (slice x TilesY + ty) x TilesX + tx, as the shader indexes it (an index past the lists' end: unknown, the tile is drawn).
			const int tx = ManyLamps::TilesX, ty = ManyLamps::TilesY, S = ManyLamps::TileSize;
			const int cols = (std::min)((int)(vp.Width + S - 1) / S, tx), rows = (std::min)((int)(vp.Height + S - 1) / S, ty);	// (the shader's clamp)
			auto put = [&](std::vector<Vertex>& q, int c0, int c1, int r0, int r1) {	// pixels [c0, c1) x [r0, r1) of the viewport
				const float x0 = (float)(vp.X + c0) - 0.5f, x1 = (float)(vp.X + c1) - 0.5f, y0 = (float)(vp.Y + r0) - 0.5f, y1 = (float)(vp.Y + r1) - 0.5f;
				const float u0 = (float)(vp.X + c0) / t.Width, u1 = (float)(vp.X + c1) / t.Width, v0 = (float)(vp.Y + r0) / t.Height, v1 = (float)(vp.Y + r1) / t.Height;
				q.push_back({ x0, y0, 0, 1, u0, v0 }); q.push_back({ x1, y0, 0, 1, u1, v0 }); q.push_back({ x0, y1, 0, 1, u0, v1 });
				q.push_back({ x1, y0, 0, 1, u1, v0 }); q.push_back({ x1, y1, 0, 1, u1, v1 }); q.push_back({ x0, y1, 0, 1, u0, v1 });
			};
			int most[64];	// one row of tiles: the most lamps any slice's list holds (cols <= TilesX <= 40)
			for (int r = 0; r < rows; r++) {
				const int n = cols;	// (<= TilesX)
				for (int c = 0; c < n; c++) {
					int m = 0;
					for (int sl = 0; sl < (int)ManyLamps::Slices && m < 64; sl++) {
						const int count = ManyLamps::FroxelCount((sl * ty + r) * tx + c);
						m = count < 0 ? 64 : (std::max)(m, count);
					}
					most[c] = m;
				}
				for (int k = 0; k < 4; k++) {	// runs of tiles over the class's threshold, one quad a run (edges on whole pixels: no pixel twice)
					for (int c = 0; c < n;) {
						if (most[c] <= 4 * k) { c++; continue; }
						int e = c;
						while (e < n && most[e] > 4 * k) e++;
						put(Quads[k], c * S, e == n ? (int)vp.Width : e * S, r * S, r + 1 == rows ? (int)vp.Height : (r + 1) * S);
						StatDrawn[k] += e - c;
						c = e;
					}
				}
			}
			StatTiles += (unsigned long long)rows * cols;
			StatTiled++;
			return true;
		}
	}

	// ShaderManager::LoadShader, after a lit shader's level-0 Interior record loaded: its deferred record, from the same source with the same
	// overrides + LAMPS_DEFERRED, in its own cache folder.
	inline void LoadRecord(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl, const D3DXMACRO* interiorOverrides, ShaderRecordPixel* forward) {
		if (!WantedAtStartup() || !IsFamily(name) || !forward) return;
		if (SettingForce::Mode() > 0) return;	// UNOFFICIAL 2026-11-14 the deferred renderer's own records (DeferredRenderer::LoadInterior)
		static D3DXMACRO overrides[12] = {};
		int n = 0;
		for (const D3DXMACRO* o = interiorOverrides; o && o->Name && n < 10; o++) {
			overrides[n++] = *o;
			if (!strcmp(o->Name, "LAMP_FALLOFF_ON")) LampFalloffOverride = o->Definition;
		}
		overrides[n++] = { "LAMPS_DEFERRED", "1" };
		overrides[n] = { NULL, NULL };
		ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, "Deferred\\", overrides);
		if (!record) { Logger::Log("[ERROR] UNOFFICIAL DeferredLamps: %s's deferred record did not load: it stays forward", name); return; }
		Records[shader] = record;
		ForwardOf[record->ShaderHandle] = forward->ShaderHandle;
		Logger::Log("UNOFFICIAL DeferredLamps: %s has a deferred record", name);
		EnsurePass();			// UNOFFICIAL 2026-10-31: both passes compiled now, with the other shaders at load, not on the render thread
		EnsureShadowShader();	// at the first interior (a black screen while D3DX ran there)
	}

	// NiD3DPixelShaderEx::SetupShader, before the interior branch: the deferred record while the main list's G-buffer is bound.
	inline ShaderRecordPixel* Choose(const void* shader) {
		if (!Live || !CapturedNormals::InSpan || Records.empty()) return nullptr;	// (empty while the deferred renderer is on: it chooses)
		auto found = Records.find(shader);
		return found == Records.end() ? nullptr : found->second;
	}

	// Once a frame (Render.cpp, with ManyLamps::BeginFrame).
	inline void BeginFrame() {
		Frame++;
		ShadowedThisFrame = false;	// (fd-batchA item 2)
		ForwardBinder[0] = 0; ForwardBindsFrame = 0;
		if (!WantedAtStartup()) { Live = false; return; }
		const bool renderer = SettingForce::Mode() > 0;	// UNOFFICIAL 2026-11-14 (above): the renderer's live switch, any runtime, outdoors only when it says so
		Live = (renderer ? RendererLive : TheSettingManager->GetSettingI(kSection, "Live") != 0 && ManyLamps::Active() && !TheShaderManager->GameState.isExterior) &&
			!PassFailed && !CopyFailed.Blocked() && !CapturedNormals::Failed && CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn();
		// UNOFFICIAL 2026-11-01 (user: the lock is black in the lockpick view): the lock is drawn in its own scene, lit by the menu's own light,
		// which the frame's lamp table and the deferred pass never see. The objects light their own lamps while the lockpick screen is open.
		if (Live && LockPickMenu::GetSingleton() && InterfaceManager->IsActive(Menu::kMenuType_LockPick)) {
			Live = false;
			static bool lockLogged = false;
			if (!lockLogged) { lockLogged = true; Logger::Log("UNOFFICIAL DeferredLamps: off while the lockpick screen is open (logged once)"); }
		}
		{	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h): live this frame, or which condition kept it forward
			static int statusSlot = -1;
			const char* why = Live ? (renderer ? "live (the deferred renderer)" : "live (interior)") : renderer && !RendererLive ? "the deferred renderer is forward this frame" : TheShaderManager->GameState.isExterior ? "exterior (forward lamps)" : PassFailed ? "the lamp pass failed to load" : CopyFailed.failed ? "the frame copy could not be made" :
				!TheSettingManager->GetSettingI(kSection, "Live") ? "Live off" : !ManyLamps::Active() ? "ManyLamps not active" : "needs CapturedNormals + buffers / lockpick";
			EffectStatus::Note(statusSlot, "DeferredLamps", Live ? EffectStatus::Drawn : EffectStatus::Skipped, why);
		}
		static bool missingLogged = false;
		if (!missingLogged && TheSettingManager->GetSettingI(kSection, "Live") && (!CapturedNormals::AlbedoCompiledIn() || !CapturedNormals::MaterialCompiledIn())) {
			missingLogged = true;
			Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: needs [Main.Main.Quality] CapturedNormals + MaterialBuffer + AlbedoBuffer (restart): forward until then");
		}
		Unshadowed = TheSettingManager->GetSettingI(kSection, "UnshadowedLamps");
		DebugView = TheSettingManager->GetSettingI(kSection, "DebugView");
		Shadows = TheSettingManager->GetSettingI(kSection, "Shadows");
		JitterExact = TheSettingManager->GetSettingI(kSection, "JitterExact") != 0;	// UNOFFICIAL 2026-11-16 fd-batchA (above)
		LandLampModel = TheSettingManager->GetSettingI(kSection, "LandLampModel") != 0;
		ReplaceScreenShadows = TheSettingManager->GetSettingI(kSection, "ReplaceScreenShadows") != 0;
		Tiles::Live = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredTiledLight") != 0;	// UNOFFICIAL 2026-11-15 DeferredTiledLight (live)
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED LAMPS %u frames: deferred binds %.1f a frame, the pass %.2f a frame (copy path %.2f), land pass %.2f a frame (%s), skipped (no G-buffer / not linear-capable) %.2f, deferred shaders left bound at the span's end and restored %.2f%s",
			(unsigned)StatFrames, StatBinds / f, StatPasses / f, StatCopies / f, StatLand / f, LandCompiled ? (LandLampModel ? "the land's own lamp model" : "LandLampModel off") : "not loaded", StatSkipped / f, StatRestored / f,
			DeferredDistant::CompiledIn() ? (StatLinearZ ? ", positions from the exact linear view z (DeferredDistant)" : ", positions from the half-float distance (DeferredDepth 2 made no linear view z)") : "");
		StatFrames = 0; StatBinds = StatPasses = StatCopies = StatRestored = StatSkipped = StatLinearZ = StatLand = 0;
		if (StatIntFrames) {	// (fd-batchA item 2 diagnostic)
			Logger::Log("DEFERRED LAMPS screen shadows %llu interior frames: forward lamp-shadow shaders bound in %llu (%s), the pass's own atlas shadows in %llu, the screen-space lamp shadows ran in %llu",
				StatIntFrames, StatFwdBound, ForwardBinderWindow.empty() ? "none" : ForwardBinderWindow.c_str(), StatOwnShadows, StatScreenRan);
			StatIntFrames = StatFwdBound = StatOwnShadows = StatScreenRan = 0; ForwardBinderWindow.clear();
		}
		if (Tiles::Live || Tiles::StatTiled || Tiles::StatFull) {	// UNOFFICIAL 2026-11-15 DeferredTiledLight: the share of the tiles each pass drew
			const double tiles = (double)(std::max)(Tiles::StatTiles, 1ull);
			Logger::Log("DEFERRED LAMPS tiled light %s: passes tiled %llu, full-screen %llu%s%s%s, first-person runs (full-screen) %llu; tiles drawn: lamp pass %.1f %%, shadow passes 1-4 %.1f / %.1f / %.1f / %.1f %%",
				Tiles::Live ? "on" : "off", Tiles::StatTiled, Tiles::StatFull, Tiles::Fallback ? " (the last: " : "", Tiles::Fallback ? Tiles::Fallback : "", Tiles::Fallback ? ")" : "", Tiles::StatViewModel,
				100.0 * Tiles::StatDrawn[0] / tiles, 100.0 * Tiles::StatDrawn[0] / tiles, 100.0 * Tiles::StatDrawn[1] / tiles, 100.0 * Tiles::StatDrawn[2] / tiles, 100.0 * Tiles::StatDrawn[3] / tiles);
			Tiles::StatTiled = Tiles::StatFull = Tiles::StatTiles = Tiles::StatViewModel = 0;
			for (auto& n : Tiles::StatDrawn) n = 0;
		}
	}

	// Render.cpp's bind hook: a deferred record was bound (counted; the pass runs only in spans that had one).
	inline void OnBind(IDirect3DPixelShader9* handle) {
		if (PointShadowForward::Bound) {	// (fd-batchA item 2 diagnostic) right after SetForDraw: this bind is a forward lamp-shadow record
			ForwardBindsFrame++;
			if (!ForwardBinder[0] && PointShadowForward::LastPixelShaderName) strncpy_s(ForwardBinder, PointShadowForward::LastPixelShaderName, _TRUNCATE);
		}
		if (!Live || !handle || ForwardOf.find(handle) == ForwardOf.end()) return;
		StatBinds++;
		DeferredBindsThisSpan++;
	}

	inline bool EnsurePass() {
		if (Pass || PassFailed) return Pass != nullptr;
		// POINT_SHADOW_FIT 3 (no blocker search) only: FIT 1 and 2 never fit next to the specular lighting in the lamp loop (X4505 in every
		// offline compile), and the shader compiler's PREFER_FLOW_CONTROL retry on them ran for minutes in game: a black screen at the first
		// interior (2026-10-31 22:23, the user's install). The full-filter shadows of a pixel's first 16 lamps come from the shadow passes.
		// UNOFFICIAL 2026-11-15 fd-lampfit (docs/next/FD-LAMPFIT.md): the pass sits at ps_3_0's 32 temp registers, and a failed compile here used to
		// turn the whole deferred renderer forward for the session (a tester's 4090 with RT lamp shadows: X4505). So an optional part that cannot
		// fit is left out instead, one level at a time, each named in ONE log line, and the renderer stays on. Level 1 lights lamps past a pixel's
		// 16th without their shadow lookup (DL_PAST16_LIT, as the skin pass does; lamps 0-15 keep the shadow passes' full filter); level 2 (only
		// with DeferredSkin) also leaves the deferred skin out (no FD_SKIN: the skin pass is not loaded, so the skin draws forward). Level 0 is the
		// pass as before (the same define list, cache folder and bytecode); the others have their own cache folders. The skin and fog passes take
		// the shared list (without DL_PAST16_LIT), as before.
		// UNOFFICIAL 2026-11-16 fd-batchA item 3 (docs/next/FD-BATCH-A.md): a level before those, with GBufferV2: + DL_LAND_SPLIT (the lamp pass
		// leaves the land's pixels to the land pass, DL_LAND_ONLY: the forward land's lamp model, DeferredLamps.hlsl DLLampLand; a third loop in this
		// pass ran out of temp registers, X4505), cache Deferred\Fit3\Land\. If it cannot fit, the next level is the pass exactly as before (its
		// define list, cache folder and bytecode): the land's own model is the first optional part left out.
		// Levels: 0 + the split (only with GBufferV2), 1 the pass as before, 2 DL_PAST16_LIT, 3 (only with DeferredSkin) DL_PAST16_LIT, no FD_SKIN.
		static const char* const fits[1] = { "3" };
		static D3DXMACRO overrides[4][14] = {};	// (2026-11-14: + POINT_SHADOW_ATLAS on native, + FD_LIGHT_FREE, + DL_LINEAR_DEPTH; 2026-11-15 + FD_SKIN, + DL_PAST16_LIT; 2026-11-16 + DL_LAND_SPLIT)
		ShaderTemplate tmpl = {};
		tmpl.Name = "DeferredLamps";
		const bool landWanted = CapturedNormals::V2CompiledIn();
		const int first = landWanted ? 0 : 1;
		const int levels = SkinCompiledIn() ? 4 : 3;
		int loadedLevel = -1;
		for (int level = first; level < levels && !Pass; level++) {
			D3DXMACRO* o = overrides[level];
			const bool skin = SkinCompiledIn() && level < 3;
			int n = 0;
			o[n++] = { "INTERIOR_SHADOWS", "1" }; o[n++] = { "FORWARD_SHADOWS", "0" }; o[n++] = { "SUN_CROSSFADE", "0" }; o[n++] = { "CONTACT_HARDENING", "0" };
			o[n++] = { "POINT_SHADOW_FIT", fits[0] };
			if (LampFalloffOverride) o[n++] = { "LAMP_FALLOFF_ON", LampFalloffOverride };	// the lamp shape the Interior records were compiled with
			// UNOFFICIAL 2026-11-14 full deferred renderer on native D3D9: the atlas is made for this pass only (PointShadowForward::AtlasRuns), so the
			// object records' define list has no POINT_SHADOW_ATLAS: the pass reads the atlas tiles, never the cube maps
			if (!PointShadowForward::AtlasWanted() && PointShadowForward::AtlasRuns()) o[n++] = { "POINT_SHADOW_ATLAS", "1" };
			if (LightFreeRecords::WantedAtStartup()) o[n++] = { "FD_LIGHT_FREE", "1" };	// UNOFFICIAL 2026-11-14 DeferredSun: the keep from the lighting-free code
			if (DeferredDistant::CompiledIn()) o[n++] = { "DL_LINEAR_DEPTH", "1" };	// UNOFFICIAL 2026-11-14 DeferredDistant: the position from the exact view z
			if (skin) o[n++] = { "FD_SKIN", "1" };	// UNOFFICIAL 2026-11-15 DeferredSkin: the lamp pass leaves the deferred skin to the skin pass
			const int shared = n;	// (fd-lampfit) what the skin and fog passes copy
			if (level >= 2) o[n++] = { "DL_PAST16_LIT", "1" };
			if (level == 0) o[n++] = { "DL_LAND_SPLIT", "1" };	// (fd-batchA; after shared: the skin and fog passes never take it)
			o[n] = { NULL, NULL };
			static const char* const cache[4] = { "Deferred\\Fit3\\Land\\", "Deferred\\Fit3\\", "Deferred\\Fit3\\Past16Lit\\", "Deferred\\Fit3\\Past16LitNoSkin\\" };
			Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLamps.pso", NULL, tmpl, cache[level], o);
			if (Pass) loadedLevel = level;
			Past16Unshadowed = Pass && level >= 2;
			if (Pass && level > first) EffectStatus::Level("DeferredLamps.pso", "loaded without an optional part (see DeferredLamps lamp pass)", false);	// (ShaderRecord's FAILED line for the first level)
			if (Pass && level == 1 && landWanted)
				Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: the lamp pass did not load with the land pass's split (DL_LAND_SPLIT): compiled without it "
					"(the land's lamps take the objects' model and falloff, as in 1.1.0); the deferred renderer stays on");
			if (Pass && level == 2)
				Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: the lamp pass did not load with the shadow lookup for lamps past a pixel's 16th: "
					"compiled without it (DL_PAST16_LIT: those lamps unshadowed, lamps 0-15 keep the shadow passes' full filter); the deferred renderer stays on");
			if (Pass && level == 3) {
				SkinPassFailed = true;	// (DeferredSkin::BeginFrame needs SkinPass: the skin's deferred records are not bound, it draws forward)
				Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: the lamp pass did not load with DeferredSkin's part: compiled without it (FD_SKIN: "
					"the skin draws forward) and without the shadow lookup for lamps past a pixel's 16th (DL_PAST16_LIT); the deferred renderer stays on");
			}
			// UNOFFICIAL 2026-11-15 DeferredSkin: the skin lamp pass, the same defines + DL_SKIN_ONLY, at load with the lamp pass
			if (Pass && skin && !SkinPass && !SkinPassFailed) {
				static D3DXMACRO skinOverrides[13] = {};
				int m = 0;
				for (; m < shared && m < 11; m++) skinOverrides[m] = o[m];
				skinOverrides[m++] = { "DL_SKIN_ONLY", "1" };
				skinOverrides[m] = { NULL, NULL };
				SkinPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLampSkin.pso", NULL, tmpl, "Deferred\\Skin\\Pass\\", skinOverrides);
				if (SkinPass) Logger::Log("UNOFFICIAL DeferredSkin: the skin lamp pass loaded (DeferredLamps.hlsl DL_SKIN_ONLY)");
				else { SkinPassFailed = true; Logger::Log("[ERROR] UNOFFICIAL DeferredSkin: the skin lamp pass did not load: the deferred skin draws forward"); }
			}
			// UNOFFICIAL 2026-11-16 fd-batchA item 3: the land lamp pass, the shared defines + DL_LAND_ONLY, at load with the lamp pass (level 0 only:
			// the other levels light the land in the lamp pass, as before). If it fails, the lamp pass's split stays off (DL_Land.x 0) and lights the land.
			if (Pass && level == 0 && !LandPass && !LandPassFailed) {
				static D3DXMACRO landOverrides[13] = {};
				int m = 0;
				for (; m < shared && m < 11; m++) landOverrides[m] = o[m];
				landOverrides[m++] = { "DL_LAND_ONLY", "1" };
				landOverrides[m] = { NULL, NULL };
				LandPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLampLand.pso", NULL, tmpl, "Deferred\\Land\\Pass\\", landOverrides);
				if (LandPass) Logger::Log("UNOFFICIAL DeferredLamps: the land lamp pass loaded (DeferredLamps.hlsl DL_LAND_ONLY: the land's lamps with the forward land's model)");
				else { LandPassFailed = true; Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: the land lamp pass did not load: the land's lamps take the objects' model and falloff, as in 1.1.0"); }
			}
			LandCompiled = Pass && level == 0 && LandPass;
			if (Pass && level <= 1) Logger::Log("UNOFFICIAL DeferredLamps: the lamp pass loaded (POINT_SHADOW_FIT %s for lamps past a pixel's 16th: no blocker search%s)", fits[0],
				level == 0 ? "; the land's lamps with the forward land's model" : "");
			// UNOFFICIAL 2026-11-14 the fog resolve: the same defines + DL_FOG_RESOLVE, at load with the lamp pass
			if (Pass && FogCompiled() && !FogPass && !FogPassFailed) {
				static D3DXMACRO fogOverrides[12] = {};
				int m = 0;
				for (; m < shared && m < 10; m++) fogOverrides[m] = o[m];
				fogOverrides[m++] = { "DL_FOG_RESOLVE", "1" };
				fogOverrides[m] = { NULL, NULL };
				FogPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLampFog.pso", NULL, tmpl, "Deferred\\Fog\\", fogOverrides);
				if (FogPass) Logger::Log("UNOFFICIAL DeferredLamps: the fog resolve loaded (the lamps' light under the game's fog, exact in the stored curve)");
				else { FogPassFailed = true; Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: the fog resolve did not load: under the game's fog the lamps add light x (1 - fog) (off by up to 50 / 255)"); }
			}
		}
		EffectStatus::Level("DeferredLamps lamp pass", loadedLevel == 0 ? "POINT_SHADOW_FIT 3, the land's lamp model" :
			loadedLevel == 1 ? (landWanted ? "POINT_SHADOW_FIT 3, the land's lamps with the objects' model (the full pass did not load)" : "POINT_SHADOW_FIT 3") :
			loadedLevel == 2 ? "POINT_SHADOW_FIT 3, lamps past the 16th unshadowed (the full pass did not load)" :
			loadedLevel == 3 ? "POINT_SHADOW_FIT 3, lamps past the 16th unshadowed, the skin forward (the full pass did not load)" : "did not load: forward lamps", loadedLevel == first);	// UNOFFICIAL 2026-11-01
		if (!Pass) { PassFailed = true; Live = false; Logger::Log("[ERROR] UNOFFICIAL DeferredLamps: the lamp pass (Shaders\\DeferredLamps.hlsl) did not load at any shadow level: forward from now on"); }
		return Pass != nullptr;
	}

	inline bool EnsureShadowShader() {
		if (ShadowPass || ShadowPassFailed) return ShadowPass != nullptr;
		{
			// FIT 0 (the full filter), else 3: not the loop versions 1 / 2, whose X4505 retry can run for minutes (see EnsurePass)
			// UNOFFICIAL 2026-11-15 (docs/next/FD-NATIVEFIX.md, bench batch 8): with the traced / capsule read compiled in (POINT_SHADOW_RT:
			// CapsuleShadows on native) FIT 0 is 4,929 slots, over native's cap. Level 1 then: FIT 1 (the High/Ultra taps in a loop, the same
			// shadows) without the RT DebugView 8 (DL_NO_RT_VIEW: its second traced lookup per lamp), 3,584 slots, no X4505 retry (2.5 s
			// offline). Only with POINT_SHADOW_RT: without it FIT 0 is ~3,000 slots and FIT 1's retry is the slow case above. Levels 0 and FIT 3
			// as before (same defines, same cache folders).
			static const char* const fits[3] = { "0", "1", "3" };
			static D3DXMACRO overrides[3][14] = {};	// (2026-11-14: + POINT_SHADOW_ATLAS on native, + FD_LIGHT_FREE, + DL_LINEAR_DEPTH; 2026-11-15 + FD_SKIN, + DL_NO_RT_VIEW)
			ShaderTemplate tmpl = {};
			tmpl.Name = "DeferredLamps";
			for (int level = 0; level < 3 && !ShadowPass; level++) {
				if (level == 1 && !RtLampShadows::CompiledIn) continue;
				D3DXMACRO* o = overrides[level];
				int n = 0;
				o[n++] = { "INTERIOR_SHADOWS", "1" }; o[n++] = { "FORWARD_SHADOWS", "0" }; o[n++] = { "SUN_CROSSFADE", "0" }; o[n++] = { "CONTACT_HARDENING", "0" };
				o[n++] = { "DL_SHADOW_PASS", "1" }; o[n++] = { "POINT_SHADOW_FIT", fits[level] };
				if (level == 1) o[n++] = { "DL_NO_RT_VIEW", "1" };
				if (LampFalloffOverride) o[n++] = { "LAMP_FALLOFF_ON", LampFalloffOverride };
				if (!PointShadowForward::AtlasWanted() && PointShadowForward::AtlasRuns()) o[n++] = { "POINT_SHADOW_ATLAS", "1" };	// (2026-11-14, as above)
				if (LightFreeRecords::WantedAtStartup()) o[n++] = { "FD_LIGHT_FREE", "1" };	// UNOFFICIAL 2026-11-14 DeferredSun: the shadow passes take the lighting-free pixels too (DL_DEFERRED_CLIP)
				if (DeferredDistant::CompiledIn()) o[n++] = { "DL_LINEAR_DEPTH", "1" };	// UNOFFICIAL 2026-11-14 DeferredDistant (as the lamp pass)
				if (SkinCompiledIn()) o[n++] = { "FD_SKIN", "1" };	// UNOFFICIAL 2026-11-15 DeferredSkin: the deferred skin's pixels get their shadows too
				o[n] = { NULL, NULL };
				static const char* const cache[3] = { "Deferred\\Shadows\\", "Deferred\\Shadows\\Fit1NoView\\", "Deferred\\Shadows\\Fit3\\" };
				ShadowPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLampShadows.pso", NULL, tmpl, cache[level], o);
				if (ShadowPass) Logger::Log("UNOFFICIAL DeferredLamps: the shadow passes loaded with POINT_SHADOW_FIT %s%s (lamps 0-15 of a pixel's list; the rest at the lamp pass's level)", fits[level],
					level == 1 ? " without the RT DebugView 8 (the full filter's shadows; the FIT 0 version is too long for this GPU)" : "");
				if (ShadowPass) EffectStatus::Level("DeferredLamps shadow passes", level == 2 ? "POINT_SHADOW_FIT 3 (the full filter did not load)" :
					level == 1 ? "POINT_SHADOW_FIT 1 (the full filter), no RT DebugView 8" : "POINT_SHADOW_FIT 0", level < 2);	// UNOFFICIAL 2026-11-01
			}
			if (!ShadowPass) EffectStatus::Level("DeferredLamps shadow passes", "did not load: the lamp pass's level for every lamp", false);
			if (!ShadowPass) { ShadowPassFailed = true; Logger::Log("[ERROR] UNOFFICIAL DeferredLamps: the shadow passes did not load: every lamp's shadow at the lamp pass's level"); return false; }
		}
		return true;
	}
	inline bool EnsureShadowPass(UINT width, UINT height) {
		if (!EnsureShadowShader()) return false;
		if (!VisTex[0] || VisWidth != width || VisHeight != height) {
			for (int k = 0; k < 4; k++) { if (Vis[k]) { Vis[k]->Release(); Vis[k] = nullptr; } if (VisTex[k]) { VisTex[k]->Release(); VisTex[k] = nullptr; } }
			for (int k = 0; k < 4; k++)
				if (FAILED(TheRenderManager->device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &VisTex[k], NULL)) ||
					FAILED(VisTex[k]->GetSurfaceLevel(0, &Vis[k]))) {
					Logger::Log("[ERROR] UNOFFICIAL DeferredLamps: could not make the shadow targets (%ux%u): every lamp's shadow at the lamp pass's level", width, height);
					ShadowPassFailed = true;
					return false;
				}
			VisWidth = width; VisHeight = height;
		}
		return true;
	}

	// The pass: after CapturedNormals::EndSpan (G-buffer unbound and resolved), before LinearScene::EndSpan (DeferredSpan, Render.cpp).
	inline void Apply(IDirect3DDevice9* d) {
		ManyLamps::FinishTable();	// UNOFFICIAL 2026-11-14 LampTableThread: the lists complete and the table unlocked before the pass reads it
		const unsigned binds = DeferredBindsThisSpan;
		DeferredBindsThisSpan = 0;
		if (!binds) return;
		const bool skipDraw = DebugView == 6;	// the records' tangent-frame view ([Shaders.ShadowsInteriors.Forward] DebugView 6): no lamps over it
		// any deferred handle still bound goes back to its forward one (draws after the list must light their lamps)
		IDirect3DPixelShader9* bound = nullptr;
		d->GetPixelShader(&bound);
		if (bound) {
			auto f = ForwardOf.find(bound);
			if (f != ForwardOf.end()) { d->SetPixelShader(f->second); StatRestored++; }
			bound->Release();
		}
		if (!CapturedNormals::Drawn || !CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture || !CapturedNormals::Texture ||
			!ManyLamps::Table || !PointShadowForward::AtlasTexture || !EnsurePass() || skipDraw) { StatSkipped++; return; }
		IDirect3DSurface9* target = nullptr;
		d->GetRenderTarget(0, &target);
		if (!target) { StatSkipped++; return; }
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);
		const bool linear = LinearScene::Linear || Vm.on;	// (DeferredFirstPerson: the light target, added ONE / ONE)
		// UNOFFICIAL 2026-11-14 the stored-curve fog (core/DeferredFog.h; DeferredLamps.hlsl DL_FOG_EXACT, compiled in with the lighting-free
		// records or GBufferV2): with the game's fog drawn the pass reads the frame (the copy below, also when it is linear) and rebuilds the
		// forward fog lerp (Includes/DeferredPixel.hlsl FDAddFogged) instead of adding light x keep
		float fog[4];
		bool fogExact = !Vm.on && FogCompiled() && FogPass && DebugView < 2 && DeferredFog::Params(fog);
		if (fogExact && (!LightTex || LightW != t.Width || LightH != t.Height)) {	// the lighting pass's own target under fog (raw light, fp16)
			if (LightSurf) { LightSurf->Release(); LightSurf = nullptr; }
			if (LightTex) { LightTex->Release(); LightTex = nullptr; }
			if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &LightTex, NULL)) || FAILED(LightTex->GetSurfaceLevel(0, &LightSurf))) {
				if (LightTex) { LightTex->Release(); LightTex = nullptr; }
				FogPassFailed = true; FogPass = nullptr;
				Logger::Log("[WARNING] UNOFFICIAL DeferredLamps: could not make the fog resolve's light target: under the game's fog the lamps add light x (1 - fog)");
			}
			else { LightW = t.Width; LightH = t.Height; }
		}
		fogExact = fogExact && LightSurf;
		if (!fogExact) fog[0] = fog[1] = fog[2] = fog[3] = 0.0f;
		if (!linear || fogExact) {	// the copy path: the frame's copy for enc(dec(v) + L) (and the fog's rebuild)
			if (!CopyTex || CopyMade.Width != t.Width || CopyMade.Height != t.Height || CopyMade.Format != t.Format) {
				if (Copy) { Copy->Release(); Copy = nullptr; }
				if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
				if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &CopyTex, NULL)) || FAILED(CopyTex->GetSurfaceLevel(0, &Copy))) {
					CopyFailed.Set("could not make the frame's copy: forward lamps");
					target->Release(); StatSkipped++; return;
				}
				CopyMade = t;
			}
			if (FAILED(d->StretchRect(target, NULL, Copy, NULL, D3DTEXF_NONE))) { target->Release(); StatSkipped++; return; }
			StatCopies++;
		}
		static GpuTimer timer("Deferred lamps"), vmTimer("Deferred lamps (first person)");	// (2026-11-15 DeferredFirstPerson: its own timer)
		GpuProfileScope gpu(Vm.on ? vmTimer : timer, d);

		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &rt[i])) && rt[i]) d->SetRenderTarget(i, nullptr);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		// 15: (2026-11-03 rtfwd) the RT lamp trace's result; 7, last: (2026-11-14) DL_LINEAR_DEPTH's view z, saved and touched only with DeferredDistant
		static const DWORD kStages[] = { 0, 1, 3, 4, 5, 8, 9, 10, 11, 12, 15, 7 };
		constexpr int kStageCount = sizeof(kStages) / sizeof(kStages[0]);
		const int stageCount = DeferredDistant::CompiledIn() ? kStageCount : kStageCount - 1;
		IDirect3DBaseTexture9* tex[kStageCount] = {};
		for (int s = 0; s < stageCount; s++) d->GetTexture(kStages[s], &tex[s]);
		// every constant the pass sets, saved and put back (c0-c11, c32-c36, c135, c168, c180-c189, c191, c205, c206-c209, c211, c215-c217, c223)
		struct Range { UINT reg, count; float v[16][4]; } saved[] = { { 0, 15 }, { 32, 5 }, { 135, 1 }, { 168, 1 }, { 180, 10 }, { 191, 1 }, { 205, 1 }, { 206, 4 }, { 211, 1 }, { 215, 3 }, { 223, 1 },	// (c13-c14: fd-batchA DL_Land, 2026-11-16)
			{ 192, SkinPass ? 5u : 0u } };	// (2026-11-15 DeferredSkin: the skin's constants c192-c195, only with the skin pass)
		for (Range& r : saved) d->GetPixelShaderConstantF(r.reg, &r.v[0][0], r.count);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const bool add = DebugView < 2;	// the debug views of the inputs write over the frame (2026-11-03: and 7, the lamps alone); 2026-11-04 the copy path adds too (DeferredLamps.hlsl)
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, Vm.on ? (DWORD)TRUE : (DWORD)FALSE, FALSE, FALSE, 0x7, add ? (DWORD)TRUE : (DWORD)FALSE, D3DBLEND_ONE, D3DBLEND_ONE,
			D3DBLENDOP_ADD, FALSE, FALSE };	// (stencil: DeferredFirstPerson's view-model mark, its test set by the caller)
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		DWORD savedSS[kStageCount][sizeof(kSS) / sizeof(kSS[0])];
		for (int s = 0; s < stageCount; s++) {
			const bool linearFilter = kStages[s] == 8;	// the atlas: linear (PointShadowForward::Bind)
			const DWORD wantS[] = { linearFilter ? (DWORD)D3DTEXF_LINEAR : (DWORD)D3DTEXF_POINT, linearFilter ? (DWORD)D3DTEXF_LINEAR : (DWORD)D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { d->GetSamplerState(kStages[s], kSS[i], &savedSS[s][i]); if (savedSS[s][i] != wantS[i]) d->SetSamplerState(kStages[s], kSS[i], wantS[i]); }
		}

		// constants: the object shaders' frame globals in their own registers, the point-shadow knobs as SetForPass writes them, the pass's own
		// (from the constant registry by name: the deferred records have the lamp code compiled out, so their binds do not set these)
		static const struct { const char* name; UINT reg; } kGlobals[] = { { "TESR_PBRData", 32 }, { "TESR_PBRExtraData", 33 }, { "TESR_PBRMetal", 34 },
			{ "TESR_PBRMetalLook", 36 }, { "TESR_DebugVar", 135 }, { "TESR_LinearScales", 168 }, { "TESR_LinearLighting", 191 }, { "TESR_LampFalloff", 205 },
			{ "TESR_PBRMath", 223 } };
		for (const auto& g : kGlobals) {
			auto found = TheShaderManager->ConstantsTable.find(g.name);
			if (found != TheShaderManager->ConstantsTable.end() && found->second) d->SetPixelShaderConstantF(g.reg, (const float*)found->second, 1);
		}
		// UNOFFICIAL 2026-11-15 DeferredSkin: the skin's material, look and blur flag for the skin pass (Includes/Skin.hlsl's registers)
		if (SkinPass) {
			static const struct { const char* name; UINT reg; } kSkin[] = { { "TESR_SkinMaterial", 192 }, { "TESR_SkinLook", 193 }, { "TESR_SkinDebug", 195 }, { "TESR_SkinSSS", 196 } };
			for (const auto& g : kSkin) {
				auto found = TheShaderManager->ConstantsTable.find(g.name);
				if (found != TheShaderManager->ConstantsTable.end() && found->second) d->SetPixelShaderConstantF(g.reg, (const float*)found->second, 1);
			}
		}
		float shadowK[10][4] = {};
		memcpy(shadowK[0], PointShadowForward::Params, sizeof(shadowK[0]));	// c180
		if (!Shadows) shadowK[0][0] = 0.0f;	// the Shadows test: strength 0, every lamp lit
		shadowK[1][0] = 0.0f; shadowK[1][1] = PointShadowForward::NearFade; shadowK[1][2] = PointShadowForward::InvisibleSize; shadowK[1][3] = PointShadowForward::Quality;	// c181
		memcpy(shadowK[8], PointShadowForward::Magnified, sizeof(shadowK[8]));	// c188
		memcpy(shadowK[9], PointShadowForward::Soft, sizeof(shadowK[9]));	// c189
		d->SetPixelShaderConstantF(180, shadowK[0], 10);
		const float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f }, zero[4] = {};
		d->SetPixelShaderConstantF(211, one, 1);	// NVR_SpecFade: full (no per-draw fade in the pass)
		// UNOFFICIAL 2026-11-03 (rtfwd) RT lamp shadows: the traced result where a lamp has one (DeferredLamps.hlsl DLShadow reads each lamp's
		// trace id from the table), as RtLampShadows::SetForPass gives the forward draws, without its upload cache (the constants are put back
		// below); the draw slots -1 (the pass names its lamps itself). Was c215 = 0: the atlas only, so RT lamp shadows changed almost nothing
		// indoors once the deferred pass lit most of the picture (a tester: "RT shadows don't work").
		if (RtLampShadows::CompiledIn) {
			float rc[7][4] = {};
			memcpy(rc, RtLampShadows::Reproject, sizeof(RtLampShadows::Reproject));
			memcpy(rc[4], RtLampShadows::RtConstant, sizeof(RtLampShadows::RtConstant));
			rc[4][0] = RtLampShadows::Mode;
			for (int k = 0; k < 4; k++) rc[5][k] = -1.0f;
			rc[6][0] = rc[6][1] = -1.0f; rc[6][2] = RtLampShadows::DebugLamp; rc[6][3] = RtLampShadows::PackingInverse();
			d->SetPixelShaderConstantF(206, &rc[0][0], 4);
			d->SetPixelShaderConstantF(215, &rc[4][0], 3);
			d->SetTexture(15, RtLampShadows::Stage15Texture());
		}
		else d->SetPixelShaderConstantF(215, zero, 1);	// NVR_PointShadowRT: the atlas
		const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const float tanX = fabsf(TheRenderManager->InvProjMatrix._11), tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const bool lists = ManyLamps::ListsFrame == ManyLamps::FrameId;
		const float gamma = LinearScene::Gamma > 1.0f ? LinearScene::Gamma : 2.2f;
		// UNOFFICIAL 2026-11-16 fd-batchA item 7 (JitterExact): the view ray through the projection the list was drawn with, TAA's jitter (its
		// _31 / _32, as DeferredDepth::FillFrame takes them) included, as Includes/DeferredDepth.hlsl DDRay builds the sun pass's: ndc - jitter.
		// The shader's ray is Fwd + Right (ndc.x tanX) + Up (ndc.y tanY), so the jitter goes into the forward row it sends (c4): the shader and
		// its bytecode are unchanged. Was: the unjittered ray, so every position sat up to half a pixel off its surface, a different way each
		// frame (lamp shadows shimmered at grazing angles; PrecisionBias hid most of it). DeferredFirstPerson's run sends its own c1-c7 below.
		const D3DXMATRIX& drawn = NiDX9Renderer::GetSingleton()->projMatrix;
		const float jx = JitterExact ? drawn._31 * tanX : 0.0f, jy = JitterExact ? drawn._32 * tanY : 0.0f;
		const float own[10][4] = {
			{ (float)ManyLamps::TilesX, (float)ManyLamps::TilesY, lists ? 1.0f : 0.0f, Unshadowed ? 1.0f : 0.0f },
			{ 1.0f / (float)vp.Width, 1.0f / (float)vp.Height, (float)vp.X, (float)vp.Y },
			{ iv._11, iv._12, iv._13, tanX },
			{ iv._21, iv._22, iv._23, tanY },
			{ iv._31 - iv._11 * jx - iv._21 * jy, iv._32 - iv._12 * jx - iv._22 * jy, iv._33 - iv._13 * jx - iv._23 * jy, linear ? 1.0f : 0.0f },
			{ 1.0f, 0.0f, 0.0f, -cam.x }, { 0.0f, 1.0f, 0.0f, -cam.y }, { 0.0f, 0.0f, 1.0f, -cam.z },
			{ (float)ManyLamps::TableRowsNow(), (float)ManyLamps::TileRow(), gamma, 1.0f / gamma },
			{ (float)DebugView, Shadows ? 1.0f : 0.0f, ManyLamps::BinWidth / (float)ManyLamps::TileSize, ManyLamps::BinHeight / (float)ManyLamps::TileSize } };	// zw: tiles across / down the lists' viewport (fractional; 2026-11-15 TileSize)
		d->SetPixelShaderConstantF(0, own[0], 10);
		// UNOFFICIAL 2026-11-16 fd-batchA item 3 (DeferredLamps.hlsl DL_Land, c14; c13 spare): the forward land's lamp terms (Includes/Terrain.hlsl
		// getPointLightLighting): x 1 = the land loop runs (LandLampModel, and the pass has it), y UsePBR, z its metal saturate(TerrainData.x),
		// w its light scale LinScale(TerrainData.z), decoded here as PBR.cpp decodes LinearScales (pow(s, gamma) when gamma > 1)
		bool landLive = false;
		{
			float land[2][4] = {};
			auto constant = [](const char* name) -> const float* {
				auto found = TheShaderManager->ConstantsTable.find(name);
				return found != TheShaderManager->ConstantsTable.end() ? (const float*)found->second : nullptr;
			};
			const float* terrain = constant("TESR_TerrainData");
			const float* terrainExtra = constant("TESR_TerrainExtraData");
			const float* linearLighting = constant("TESR_LinearLighting");
			// (bench 2026-11-16: the land pass ran a full-screen draw in every interior frame, where no land is drawn: outdoors only)
			if (LandCompiled && LandLampModel && terrain && terrainExtra && !Vm.on && TheShaderManager->GameState.isExterior) {
				const float g = linearLighting ? linearLighting[0] : 1.0f, s = terrain[2];
				land[1][0] = 1.0f;
				landLive = true;
				land[1][1] = terrainExtra[0] != 0.0f ? 1.0f : 0.0f;
				land[1][2] = (std::min)((std::max)(terrain[0], 0.0f), 1.0f);
				land[1][3] = g > 1.0f ? powf(s > 0.0f ? s : 0.0f, g) : s;
			}
			d->SetPixelShaderConstantF(13, land[0], 2);
		}
		if (Vm.on) d->SetPixelShaderConstantF(1, &Vm.c[0][0], 7);	// UNOFFICIAL 2026-11-15 DeferredFirstPerson: the first-person camera's mapping and lamp rows

		d->SetVertexShader(nullptr);
		d->SetPixelShader(Pass->ShaderHandle);
		d->SetTexture(0, linear && !fogExact ? nullptr : CopyTex);
		if (!fogExact) fog[3] = 0.0f;
		d->SetPixelShaderConstantF(12, fog, 1);	// DL_Fog (2026-11-14)
		d->SetTexture(1, CapturedNormals::AlbedoTexture);
		d->SetTexture(3, ManyLamps::Table);
		d->SetTexture(4, CapturedNormals::MaterialTexture);
		d->SetTexture(5, CapturedNormals::Texture);
		d->SetTexture(8, PointShadowForward::AtlasTexture);
		// UNOFFICIAL 2026-11-14 DeferredDistant (DL_LINEAR_DEPTH, DeferredLamps.hlsl DLViewZ): this frame's exact linear view z (DeferredDepth 2,
		// made by DeferredDepth::AtSpanEnd just before), c10.w 1 below; else nothing on s7 and the half-float distance as before
		const bool linearZ = DeferredDistant::CompiledIn() && DeferredDepth::LinearReady() && !Vm.on;	// (the world's view z: not the view model's)
		if (DeferredDistant::CompiledIn()) d->SetTexture(7, linearZ ? DeferredDepth::LinearTexture : nullptr);
		if (linearZ) StatLinearZ++;
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		// UNOFFICIAL 2026-11-15 DeferredTiledLight (Tiles above): class k's tile quads, or the full-screen quad as before
		const bool tiled = Tiles::Build(vp, t, lists);
		auto draw = [&](int k) {
			if (!tiled) d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			else if (!Tiles::Quads[k].empty()) d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)Tiles::Quads[k].size() / 3, Tiles::Quads[k].data(), sizeof(Tiles::Vertex));
		};
		// the shadow passes: lamps 0-3, 4-7, 8-11, 12-15 of each pixel's list into the four targets (blend off), then the lighting pass reads them
		bool vis = false;
		// UNOFFICIAL 2026-11-01 PrecisionBias (DeferredLamps.hlsl DLShadow): the shadow lookup's push off the surface per unit of distance
		const float bias = (std::max)(TheSettingManager->GetSettingF(kSection, "PrecisionBias"), 0.0f) * 0.001f;
		if ((DebugView < 2 || DebugView == 7 || DebugView == 8) && EnsureShadowPass(t.Width, t.Height)) {	// (8, 2026-11-06: the shadow passes write its codes)
			static GpuTimer shadowTimer("Deferred lamp shadows"), vmShadowTimer("Deferred lamp shadows (first person)");
			GpuProfileScope shadowGpu(Vm.on ? vmShadowTimer : shadowTimer, d);
			d->SetPixelShader(ShadowPass->ShaderHandle);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			for (int k = 0; k < 4; k++) {
				const float c10[4] = { 4.0f * (float)k, 0.0f, bias, linearZ ? 1.0f : 0.0f };
				d->SetPixelShaderConstantF(10, c10, 1);
				d->SetRenderTarget(0, Vis[k]);
				d->SetViewport(&vp);
				draw(k);
			}
			d->SetRenderTarget(0, target);
			d->SetViewport(&vp);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, want[8]);
			d->SetRenderState(D3DRS_COLORWRITEENABLE, want[7]);
			for (int k = 0; k < 4; k++) d->SetTexture(9 + k, VisTex[k]);
			d->SetPixelShader(Pass->ShaderHandle);
			vis = true;
		}
		const float c10[4] = { 0.0f, vis ? 1.0f : 0.0f, bias, linearZ ? 1.0f : 0.0f };
		d->SetPixelShaderConstantF(10, c10, 1);
		// UNOFFICIAL 2026-11-04 (as FlashlightDeferred::Apply) the lighting draw depth-tested, not written, at each pixel's G-buffer surface (the
		// shader's own depth, DeferredLamps.hlsl): what was drawn over it without writing the G-buffer (hair) is not lit as the surface behind.
		// Only this draw: the shadow passes above write their own targets. The game's depth runs either way (reversed while it clears to 0).
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		const float c11[4] = { Vm.on ? Vm.depth[0] : proj._33, Vm.on ? Vm.depth[1] : proj._43, 0.01f, 2.0f };	// DL_Depth: the depth of a view z, and the pull nearer (1 % and 2 units)
		d->SetPixelShaderConstantF(11, c11, 1);
		DWORD zEnable = 0, zFunc = 0;
		d->GetRenderState(D3DRS_ZENABLE, &zEnable); d->GetRenderState(D3DRS_ZFUNC, &zFunc);
		d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		d->SetRenderState(D3DRS_ZFUNC, (1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
		IDirect3DBaseTexture9* stage2 = nullptr;
		if (fogExact) {	// (2026-11-14) the raw light into its own target (cleared, blend off); the resolve below adds it under the fog
			d->SetRenderTarget(0, LightSurf);
			d->SetViewport(&vp);
			d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		}
		draw(0);
		// UNOFFICIAL 2026-11-16 fd-batchA item 2 (LampShadowsDrawn): the world's lamps were shadowed from the atlas in this pass (the shadow passes'
		// full filter, or the lighting pass's own lookup; not at the DL_PAST16_LIT level without the shadow passes, which lit them unshadowed)
		if (Shadows && !Vm.on && (vis || !Past16Unshadowed)) ShadowedThisFrame = true;
		if (landLive) {	// UNOFFICIAL 2026-11-16 fd-batchA item 3: the land lamp pass, the same state (as the skin pass below); its pixels are the ones the lamp
			d->SetPixelShader(LandPass->ShaderHandle);	// pass clipped (under the exact fog: the light target's clear 0 there, blend off: a write)
			draw(0);	// (DeferredTiledLight: the lamp tiles, or the full-screen quad)
			d->SetPixelShader(Pass->ShaderHandle);
			StatLand++;
		}
		if (SkinPass) {	// UNOFFICIAL 2026-11-15 DeferredSkin: the skin lamp pass, the same state; its pixels are the ones the lamp pass clipped
			d->SetPixelShader(SkinPass->ShaderHandle);
			// (2026-11-15 SSS v2) its highlights to the skin's highlight target (COLOR1), ADDED: also under the exact fog, where this draw goes
			// into the lamps' own raw-light target with blend off; the lamp pass left the skin's pixels at the clear's 0 there, so an add is
			// the same as a write (the debug views keep their write)
			DWORD blendSaved = FALSE, mask1Saved = 0xF;
			if (SkinSpecSurface) {
				d->SetRenderTarget(1, SkinSpecSurface); d->SetViewport(&vp);
				d->GetRenderState(D3DRS_COLORWRITEENABLE1, &mask1Saved); d->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
				d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendSaved);
				if (!blendSaved && DebugView < 2) { d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); }
			}
			draw(0);	// (DeferredTiledLight: the lamp tiles, or the full-screen quad)
			if (SkinSpecSurface) {
				if (!blendSaved && DebugView < 2) d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
				d->SetRenderTarget(1, nullptr); d->SetViewport(&vp);
				d->SetRenderState(D3DRS_COLORWRITEENABLE1, mask1Saved);
			}
			d->SetPixelShader(Pass->ShaderHandle);
		}
		d->SetRenderState(D3DRS_ZENABLE, zEnable); d->SetRenderState(D3DRS_ZFUNC, zFunc);
		if (fogExact) {
			d->SetRenderTarget(0, target);
			d->SetViewport(&vp);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, want[8]);
			d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
			d->GetTexture(2, &stage2);
			DWORD ss2[3] = {};
			d->GetSamplerState(2, D3DSAMP_MINFILTER, &ss2[0]); d->GetSamplerState(2, D3DSAMP_MAGFILTER, &ss2[1]); d->GetSamplerState(2, D3DSAMP_MIPFILTER, &ss2[2]);
			d->SetTexture(2, LightTex);
			d->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			d->SetPixelShader(FogPass->ShaderHandle);
			draw(0);	// (outside the lamp tiles the light target holds the clear's 0: the resolve clips there)
			d->SetRenderState(D3DRS_ZENABLE, zEnable);
			d->SetSamplerState(2, D3DSAMP_MINFILTER, ss2[0]); d->SetSamplerState(2, D3DSAMP_MAGFILTER, ss2[1]); d->SetSamplerState(2, D3DSAMP_MIPFILTER, ss2[2]);
			d->SetTexture(2, stage2); if (stage2) stage2->Release();
			StatFogExact++;
		}
		StatPasses++;

		for (int s = 0; s < stageCount; s++) {
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) d->SetSamplerState(kStages[s], kSS[i], savedSS[s][i]);
			d->SetTexture(kStages[s], tex[s]); if (tex[s]) tex[s]->Release();
		}
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		for (Range& r : saved) d->SetPixelShaderConstantF(r.reg, &r.v[0][0], r.count);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) { d->SetRenderTarget(i, rt[i]); rt[i]->Release(); }
		d->SetViewport(&vp);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		// (the forward handle if a deferred one was bound, see above)
		d->SetPixelShader(ps); if (ps) ps->Release();
		target->Release();
	}
}
