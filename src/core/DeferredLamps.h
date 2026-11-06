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
	inline FailOnce::Latch CopyFailed{ "DeferredLamps frame copy" };	// UNOFFICIAL 2026-11-02 RetryFailedEffects
	// UNOFFICIAL 2026-10-31 the shadow passes (DL_SHADOW_PASS): the full filter for lamps 0-15 of each pixel's list, four lamps a pass into one
	// A8R8G8B8 target each, read by the lighting pass (which has no temp registers left for the full filter next to the specular lighting).
	inline ShaderRecordPixel* ShadowPass = nullptr;
	inline bool ShadowPassFailed = false;
	inline IDirect3DTexture9* VisTex[4] = {};
	inline IDirect3DSurface9* Vis[4] = {};
	inline UINT VisWidth = 0, VisHeight = 0;
	inline bool Live = false;				// this frame: the switch on (read once a frame)
	inline int Unshadowed = 1;
	inline int DebugView = 0;
	inline int Shadows = 1;
	inline UInt32 Frame = 0;
	inline unsigned long long StatFrames = 0, StatBinds = 0, StatPasses = 0, StatCopies = 0, StatRestored = 0, StatSkipped = 0;
	inline unsigned DeferredBindsThisSpan = 0;
	inline const char* LampFalloffOverride = nullptr;	// the Interior records' LAMP_FALLOFF_ON override (ShaderManager.cpp), compiled into the pass too
	inline IDirect3DTexture9* CopyTex = nullptr;
	inline IDirect3DSurface9* Copy = nullptr;
	inline D3DSURFACE_DESC CopyMade = {};

	inline bool EnsurePass();			// (below)
	inline bool EnsureShadowShader();	// (below)

	// ShaderManager::LoadShader, after a lit shader's level-0 Interior record loaded: its deferred record, from the same source with the same
	// overrides + LAMPS_DEFERRED, in its own cache folder.
	inline void LoadRecord(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl, const D3DXMACRO* interiorOverrides, ShaderRecordPixel* forward) {
		if (!WantedAtStartup() || !IsFamily(name) || !forward) return;
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
		if (!Live || !CapturedNormals::InSpan || Records.empty()) return nullptr;
		auto found = Records.find(shader);
		return found == Records.end() ? nullptr : found->second;
	}

	// Once a frame (Render.cpp, with ManyLamps::BeginFrame).
	inline void BeginFrame() {
		Frame++;
		if (!WantedAtStartup()) { Live = false; return; }
		Live = TheSettingManager->GetSettingI(kSection, "Live") != 0 && !PassFailed && !CopyFailed.Blocked() && !CapturedNormals::Failed && ManyLamps::Active() &&
			CapturedNormals::AlbedoCompiledIn() && CapturedNormals::MaterialCompiledIn() && !TheShaderManager->GameState.isExterior;
		// UNOFFICIAL 2026-11-01 (user: the lock is black in the lockpick view): the lock is drawn in its own scene, lit by the menu's own light,
		// which the frame's lamp table and the deferred pass never see. The objects light their own lamps while the lockpick screen is open.
		if (Live && LockPickMenu::GetSingleton() && InterfaceManager->IsActive(Menu::kMenuType_LockPick)) {
			Live = false;
			static bool lockLogged = false;
			if (!lockLogged) { lockLogged = true; Logger::Log("UNOFFICIAL DeferredLamps: off while the lockpick screen is open (logged once)"); }
		}
		{	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h): live this frame, or which condition kept it forward
			static int statusSlot = -1;
			const char* why = Live ? "live (interior)" : TheShaderManager->GameState.isExterior ? "exterior (forward lamps)" : PassFailed ? "the lamp pass failed to load" : CopyFailed.failed ? "the frame copy could not be made" :
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
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED LAMPS %u frames: deferred binds %.1f a frame, the pass %.2f a frame (copy path %.2f), skipped (no G-buffer / not linear-capable) %.2f, deferred shaders left bound at the span's end and restored %.2f",
			(unsigned)StatFrames, StatBinds / f, StatPasses / f, StatCopies / f, StatSkipped / f, StatRestored / f);
		StatFrames = 0; StatBinds = StatPasses = StatCopies = StatRestored = StatSkipped = 0;
	}

	// Render.cpp's bind hook: a deferred record was bound (counted; the pass runs only in spans that had one).
	inline void OnBind(IDirect3DPixelShader9* handle) {
		if (!Live || !handle || ForwardOf.find(handle) == ForwardOf.end()) return;
		StatBinds++;
		DeferredBindsThisSpan++;
	}

	inline bool EnsurePass() {
		if (Pass || PassFailed) return Pass != nullptr;
		// POINT_SHADOW_FIT 3 (no blocker search) only: FIT 1 and 2 never fit next to the specular lighting in the lamp loop (X4505 in every
		// offline compile), and the shader compiler's PREFER_FLOW_CONTROL retry on them ran for minutes in game: a black screen at the first
		// interior (2026-10-31 22:23, the user's install). The full-filter shadows of a pixel's first 16 lamps come from the shadow passes.
		static const char* const fits[1] = { "3" };
		static D3DXMACRO overrides[1][8] = {};
		ShaderTemplate tmpl = {};
		tmpl.Name = "DeferredLamps";
		for (int level = 0; level < 1 && !Pass; level++) {
			D3DXMACRO* o = overrides[level];
			int n = 0;
			o[n++] = { "INTERIOR_SHADOWS", "1" }; o[n++] = { "FORWARD_SHADOWS", "0" }; o[n++] = { "SUN_CROSSFADE", "0" }; o[n++] = { "CONTACT_HARDENING", "0" };
			o[n++] = { "POINT_SHADOW_FIT", fits[level] };
			if (LampFalloffOverride) o[n++] = { "LAMP_FALLOFF_ON", LampFalloffOverride };	// the lamp shape the Interior records were compiled with
			o[n] = { NULL, NULL };
			static const char* const cache[1] = { "Deferred\\Fit3\\" };
			Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLamps.pso", NULL, tmpl, cache[level], o);
			if (Pass) Logger::Log("UNOFFICIAL DeferredLamps: the lamp pass loaded (POINT_SHADOW_FIT %s for lamps past a pixel's 16th: no blocker search)", fits[level]);
		}
		EffectStatus::Level("DeferredLamps lamp pass", Pass ? "POINT_SHADOW_FIT 3" : "did not load: forward lamps", Pass != nullptr);	// UNOFFICIAL 2026-11-01
		if (!Pass) { PassFailed = true; Live = false; Logger::Log("[ERROR] UNOFFICIAL DeferredLamps: the lamp pass (Shaders\\DeferredLamps.hlsl) did not load at any shadow level: forward from now on"); }
		return Pass != nullptr;
	}

	inline bool EnsureShadowShader() {
		if (ShadowPass || ShadowPassFailed) return ShadowPass != nullptr;
		{
			// FIT 0 (the full filter), else 3: not the loop versions 1 / 2, whose X4505 retry can run for minutes (see EnsurePass)
			static const char* const fits[2] = { "0", "3" };
			static D3DXMACRO overrides[2][9] = {};
			ShaderTemplate tmpl = {};
			tmpl.Name = "DeferredLamps";
			for (int level = 0; level < 2 && !ShadowPass; level++) {
				D3DXMACRO* o = overrides[level];
				int n = 0;
				o[n++] = { "INTERIOR_SHADOWS", "1" }; o[n++] = { "FORWARD_SHADOWS", "0" }; o[n++] = { "SUN_CROSSFADE", "0" }; o[n++] = { "CONTACT_HARDENING", "0" };
				o[n++] = { "DL_SHADOW_PASS", "1" }; o[n++] = { "POINT_SHADOW_FIT", fits[level] };
				if (LampFalloffOverride) o[n++] = { "LAMP_FALLOFF_ON", LampFalloffOverride };
				o[n] = { NULL, NULL };
				static const char* const cache[2] = { "Deferred\\Shadows\\", "Deferred\\Shadows\\Fit3\\" };
				ShadowPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredLampShadows.pso", NULL, tmpl, cache[level], o);
				if (ShadowPass) Logger::Log("UNOFFICIAL DeferredLamps: the shadow passes loaded with POINT_SHADOW_FIT %s (lamps 0-15 of a pixel's list; the rest at the lamp pass's level)", fits[level]);
				if (ShadowPass) EffectStatus::Level("DeferredLamps shadow passes", level ? "POINT_SHADOW_FIT 3 (the full filter did not load)" : "POINT_SHADOW_FIT 0", level == 0);	// UNOFFICIAL 2026-11-01
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
		const bool linear = LinearScene::Linear;
		if (!linear) {	// the copy path: the frame's copy for enc(dec(v) + L)
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
		static GpuTimer timer("Deferred lamps");
		GpuProfileScope gpu(timer, d);

		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &rt[i])) && rt[i]) d->SetRenderTarget(i, nullptr);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		static const DWORD kStages[] = { 0, 1, 3, 4, 5, 8, 9, 10, 11, 12, 15 };	// 15: (2026-11-03 rtfwd) the RT lamp trace's result
		IDirect3DBaseTexture9* tex[11] = {};
		for (int s = 0; s < 11; s++) d->GetTexture(kStages[s], &tex[s]);
		// every constant the pass sets, saved and put back (c0-c11, c32-c36, c135, c168, c180-c189, c191, c205, c206-c209, c211, c215-c217, c223)
		struct Range { UINT reg, count; float v[16][4]; } saved[] = { { 0, 12 }, { 32, 5 }, { 135, 1 }, { 168, 1 }, { 180, 10 }, { 191, 1 }, { 205, 1 }, { 206, 4 }, { 211, 1 }, { 215, 3 }, { 223, 1 } };
		for (Range& r : saved) d->GetPixelShaderConstantF(r.reg, &r.v[0][0], r.count);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		// UNOFFICIAL 2026-11-05 (Balebandro 256de14): the copy path adds too (DeferredLamps.hlsl: the difference it makes), so every MSAA
		// sample keeps its own value; written over, each sample took the resolved copy's one value (edges and terrain blending flattened).
		const bool add = DebugView < 2;	// the debug views of the inputs write over the frame (2026-11-03: and 7, the lamps alone)
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0x7, add ? (DWORD)TRUE : (DWORD)FALSE, D3DBLEND_ONE, D3DBLEND_ONE,
			D3DBLENDOP_ADD, FALSE, FALSE };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		DWORD savedSS[11][sizeof(kSS) / sizeof(kSS[0])];
		for (int s = 0; s < 11; s++) {
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
		const float own[10][4] = {
			{ (float)ManyLamps::TilesX, (float)ManyLamps::TilesY, lists ? 1.0f : 0.0f, Unshadowed ? 1.0f : 0.0f },
			{ 1.0f / (float)vp.Width, 1.0f / (float)vp.Height, (float)vp.X, (float)vp.Y },
			{ iv._11, iv._12, iv._13, tanX },
			{ iv._21, iv._22, iv._23, tanY },
			{ iv._31, iv._32, iv._33, linear ? 1.0f : 0.0f },
			{ 1.0f, 0.0f, 0.0f, -cam.x }, { 0.0f, 1.0f, 0.0f, -cam.y }, { 0.0f, 0.0f, 1.0f, -cam.z },
			{ (float)ManyLamps::TableRowsNow(), (float)ManyLamps::TileRow(), gamma, 1.0f / gamma },
			{ (float)DebugView, Shadows ? 1.0f : 0.0f, ManyLamps::BinWidth / (float)ManyLamps::TilePx, ManyLamps::BinHeight / (float)ManyLamps::TilePx } };	// zw: tiles across / down the lists' viewport (fractional)
		d->SetPixelShaderConstantF(0, own[0], 10);

		d->SetVertexShader(nullptr);
		d->SetPixelShader(Pass->ShaderHandle);
		d->SetTexture(0, linear ? nullptr : CopyTex);
		d->SetTexture(1, CapturedNormals::AlbedoTexture);
		d->SetTexture(3, ManyLamps::Table);
		d->SetTexture(4, CapturedNormals::MaterialTexture);
		d->SetTexture(5, CapturedNormals::Texture);
		d->SetTexture(8, PointShadowForward::AtlasTexture);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		// the shadow passes: lamps 0-3, 4-7, 8-11, 12-15 of each pixel's list into the four targets (blend off), then the lighting pass reads them
		bool vis = false;
		// UNOFFICIAL 2026-11-01 PrecisionBias (DeferredLamps.hlsl DLShadow): the shadow lookup's push off the surface per unit of distance
		const float bias = (std::max)(TheSettingManager->GetSettingF(kSection, "PrecisionBias"), 0.0f) * 0.001f;
		if ((DebugView < 2 || DebugView == 7) && EnsureShadowPass(t.Width, t.Height)) {
			static GpuTimer shadowTimer("Deferred lamp shadows");
			GpuProfileScope shadowGpu(shadowTimer, d);
			d->SetPixelShader(ShadowPass->ShaderHandle);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			for (int k = 0; k < 4; k++) {
				const float c10[4] = { 4.0f * (float)k, 0.0f, bias, 0.0f };
				d->SetPixelShaderConstantF(10, c10, 1);
				d->SetRenderTarget(0, Vis[k]);
				d->SetViewport(&vp);
				d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			}
			d->SetRenderTarget(0, target);
			d->SetViewport(&vp);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, want[8]);
			d->SetRenderState(D3DRS_COLORWRITEENABLE, want[7]);
			for (int k = 0; k < 4; k++) d->SetTexture(9 + k, VisTex[k]);
			d->SetPixelShader(Pass->ShaderHandle);
			vis = true;
		}
		const float c10[4] = { 0.0f, vis ? 1.0f : 0.0f, bias, 0.0f };
		d->SetPixelShaderConstantF(10, c10, 1);
		// UNOFFICIAL 2026-11-05 (Balebandro 256de14) the lighting draw depth-tested, not written, at each pixel's G-buffer surface (the
		// shader's own depth, DeferredLamps.hlsl): what was drawn over it without writing the G-buffer (hair) is not lit as the surface
		// behind, and under MSAA each sample is tested. Only this draw: the shadow passes above write their own targets.
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		const float c11[4] = { proj._33, proj._43, 0.01f, 2.0f };	// DL_Depth: the depth of a view z, and the pull nearer (1 % and 2 units)
		d->SetPixelShaderConstantF(11, c11, 1);
		DWORD zEnable = 0, zFunc = 0;
		d->GetRenderState(D3DRS_ZENABLE, &zEnable); d->GetRenderState(D3DRS_ZFUNC, &zFunc);
		d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		d->SetRenderState(D3DRS_ZFUNC, (1.0 - NiDX9Renderer::GetSingleton()->m_fZClear) ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);	// reversed while it clears to 0
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		d->SetRenderState(D3DRS_ZENABLE, zEnable); d->SetRenderState(D3DRS_ZFUNC, zFunc);
		StatPasses++;

		for (int s = 0; s < 11; s++) {
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
