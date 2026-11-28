#pragma once

// UNOFFICIAL 2026-10-16 ExactPassSums ([Shaders.PBR.Linear] ExactPassSums, live; bench\foundations\REPORT.md "Fix candidate:
// exact linear sums for the extra passes").
//
// With linear lighting on, an object lit by more lamps than its first pass holds is drawn as a light-only pass, extra lamp
// passes ADDED on top (ONE+ONE), a texture pass that MULTIPLIES, and highlight passes ADDED after it. Each added pass was
// encoded on its own (x^(1/g)), so the frame held enc(A) + enc(B) instead of enc(A + B): measured up to 3.7x too bright in
// linear light, 83% of the Ultra-Luxe frame. The game draws all extra lamp passes of the world as ONE run, and all highlight
// passes as ONE run (measured, LampPassOrderLog). So: while a run is drawn, its passes go into a separate fp16 buffer (same
// depth buffer) and output their LINEAR light (ObjectTemplate.hlsl, TESR_PassRedirect); when the run ends, one full-screen step
// writes scene = (scene^g + buffer)^(1/g) wherever the buffer is not 0 (the rest is discarded, untouched). Lamps are folded in
// before the texture pass multiplies them, highlights before the fog. Mode 1 highlights, 2 lamps, 3 both. World scene only.
// With the game's MSAA (iMultiSample) the world target is multisampled: the buffer is too (it shares the depth buffer), both are
// resolved for reading, and the step ADDS the difference (scene^g + buffer)^(1/g) - scene to every sample, so the edges keep
// their anti-aliasing (exact inside surfaces; at an object's edge the difference of the pixel's average is used).
#include <d3dx9.h>
#include "GpuProfiler.h"
#include "FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects
#include "LinearScene.h"	// UNOFFICIAL 2026-10-28: with LinearScene live the sums are exact already (every mode off)

namespace PassSums {
	// UNOFFICIAL 2026-10-29: called right after the fold-in's state block Apply (set by CapturedNormals' lean hook, which tracks the device
	// state from its Set calls; a state block bypasses them).
	inline void (*AfterStateBlock)(IDirect3DDevice9*) = nullptr;


	inline D3DXVECTOR4 Redirect = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// TESR_PassRedirect (c210): x 1 = this pass draws into the buffer, linear
	inline float Gamma = 1.0f;					// the gamma LinEncode uses (PBR.cpp: [Shaders.PBR.Linear] Gamma when linear lighting is on, else 1)

	enum Kind { None = 0, Lamps = 1, Highlights = 2 };
	inline int Mode = 0;							// the setting, read once a frame
	inline Kind Active = None;						// the run being redirected now
	inline int Runs[3] = { 0, 0, 0 };				// runs started this frame per kind (at most RunCap each: past it the game's passes are kept)
	// UNOFFICIAL 2026-10-28 (tester, Freeside at night with POM: one road piece lighter than its neighbours, the 2026-10-25 daytime
	// patch back): the cap of 2 runs a frame was set for the fold-in (a full-screen step per run). Outside at night more objects are lamp
	// lit and the world's lamp and highlight passes can come in more runs, and every pass past the second run was summed in the stored
	// curve again. The increments cost one copy of the frame per run, so they take up to kIncrementRunCap; the fold-in keeps 2.
	constexpr int kFoldRunCap = 2;
	constexpr int kIncrementRunCap = 8;
	inline IDirect3DSurface9* Scene = nullptr;		// the render target the run replaced (the game's scene buffer)
	inline D3DVIEWPORT9 SceneViewport = {};			// UNOFFICIAL 2026-10-27 (RenderScale): the scene's viewport as the run began
	inline bool SceneViewportSaved = false;
	inline IDirect3DTexture9* AccTex = nullptr;   inline IDirect3DSurface9* Acc = nullptr;
	inline IDirect3DSurface9* AccMS = nullptr;		// the multisampled buffer drawn into when the world target is multisampled
	inline IDirect3DTexture9* CopyTex = nullptr;  inline IDirect3DSurface9* Copy = nullptr;
	inline IDirect3DPixelShader9* ComposePS = nullptr;
	inline D3DSURFACE_DESC Made = {};
	inline FailOnce::Latch Failed{ "Exact pass sums" };	// UNOFFICIAL 2026-11-02 RetryFailedEffects (was a bool until restart)
	inline UInt32 Composes = 0, Frames = 0;
	inline int LogFrames = 0;						// frames left whose runs are logged (set when the mode changes)
	inline UInt32 RunPasses = 0;	inline UInt16 RunFirst = 0, RunLast = 0;
	inline int LastMode = 0;
	// UNOFFICIAL 2026-10-21 LampSums ([Shaders.PBR.Main] LampSums: compiled in at startup, live while compiled; ObjectTemplate.hlsl
	// NVR_LAMP_SUMS; bench\deep-roadmap\evidence\lampsums\LOG.md). The per-pass increments of 2026-10-17 (experiment/pass-increments,
	// shelved then: the HIGHLIGHT runs overlap a lot, and FoldHighlights now draws no highlight passes on these objects) for the LAMP run
	// only, instead of the buffer and the fold-in: when the run of extra lamp passes begins, the frame is copied once (one resolve) to
	// IncTex on stage kRunStartStage and c210 y is set; each extra lamp pass (the DIFFUSE shaders) then adds enc(dec(s) + its light) - s
	// itself through the game's own ONE+ONE blend, s being the copy (the object's light-only pass). Exact where one extra lamp pass
	// covers a pixel (objects with 3-5 lamps have exactly one); where two do (6+ lamps), each starts from s. Mode bit kIncrements
	// (Render.cpp sets mode kIncrements | Lamps when LampSums is on and ExactPassSums is 0); highlight runs never take it.
	constexpr int kIncrements = 4;
	constexpr DWORD kRunStartStage = 9;				// NVR_LampRunStart: the sun's atlas slot, never read by the DIFFUSE passes
	// UNOFFICIAL 2026-10-25 HighlightSums ([Shaders.PBR.Main] HighlightSums, compiled in at startup like LampSums; ObjectTemplate.hlsl and
	// ParallaxTemplate.hlsl, their ONLY_SPECULAR variants): the same increments for the world's run of HIGHLIGHT passes, the ones FoldHighlights
	// leaves to the game (parallax objects -- it never folds them -- glow-mapped ones, hair, objects whose fold broke, or with it off). Each
	// highlight pass adds enc(dec(s) + its highlight) - s, s the frame as the run began (after the texture passes). Where two highlight passes
	// of one object cover a pixel (the sun's and its lamps'), each starts from s: a little under the exact sum, against the several-times-too-
	// bright stored-curve add (1% light -> 12% stored; Novac road and rubble pieces, tester report 2026-10-25). Mode bit kHighlightIncrements;
	// the copy is read on s6 there (free in every ONLY_SPECULAR variant; s9 is the sun's atlas, which those passes read).
	constexpr int kHighlightIncrements = 8;
	constexpr DWORD kHighlightStartStage = 6;
	inline DWORD IncStage = kRunStartStage;		// the stage the open increments run reads its copy on
	inline bool HighlightSumsChecked = false, HighlightSumsCompiled = false;
	inline UInt32 IncHlFrames = 0, IncHlCopies = 0, IncHlPasses = 0, IncHlLeft = 0;
	inline IDirect3DTexture9* IncTex = nullptr;  inline IDirect3DSurface9* IncSurf = nullptr;
	inline D3DSURFACE_DESC IncMade = {};
	inline bool RunIsIncrements = false;	// the open run is a LampSums run
	inline bool IncLive = false;			// ... and its copy was made (the passes add increments)
	inline bool LampSumsChecked = false, LampSumsCompiled = false;
	inline UInt32 IncFrames = 0, IncCopies = 0, IncPasses = 0, IncLeft = 0, IncFailed = 0;

	inline Kind KindOf(UInt16 e) {
		if ((e >= 0x132 && e <= 0x13C && e != 0x137) || (e >= 0x13E && e <= 0x148 && e != 0x143)) return Lamps;	// as LampPassTest
		switch (e) {
		case 0x15A: case 0x15C: case 0x15D: case 0x160: case 0x162: case 0x163:
		case 0x166: case 0x168: case 0x169: case 0x16C: case 0x16E: case 0x16F:
		case 0x172: case 0x174: case 0x175:
			return Highlights;
		}
		return None;
	}

	inline void Release() {
		for (IUnknown** p : { (IUnknown**)&AccMS, (IUnknown**)&Acc, (IUnknown**)&AccTex, (IUnknown**)&Copy, (IUnknown**)&CopyTex })
			if (*p) { (*p)->Release(); *p = nullptr; }
		Made = {};
	}

	inline void ReleaseIncrements() {
		if (IncSurf) { IncSurf->Release(); IncSurf = nullptr; }
		if (IncTex) { IncTex->Release(); IncTex = nullptr; }
		IncMade = {};
	}

	inline bool MakeIncrements(IDirect3DDevice9* device, const D3DSURFACE_DESC& d) {
		if (IncTex && IncMade.Width == d.Width && IncMade.Height == d.Height && IncMade.Format == d.Format) return true;
		ReleaseIncrements();
		if (FAILED(device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &IncTex, NULL)) ||
			FAILED(IncTex->GetSurfaceLevel(0, &IncSurf))) {
			Logger::Log("LampSums: could not make the frame's copy (%ux%u format %u): off", d.Width, d.Height, (unsigned)d.Format);
			ReleaseIncrements();
			return false;
		}
		IncMade = d;
		Logger::Log("LampSums: copy made (%ux%u format %u, the frame msaa %u)", d.Width, d.Height, (unsigned)d.Format, (unsigned)d.MultiSampleType);
		return true;
	}

	inline bool Make(IDirect3DDevice9* device, const D3DSURFACE_DESC& d) {
		if (AccTex && Made.Width == d.Width && Made.Height == d.Height && Made.Format == d.Format && Made.MultiSampleType == d.MultiSampleType &&
			Made.MultiSampleQuality == d.MultiSampleQuality) return true;
		Release();
		if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&
			FAILED(device->CreateRenderTarget(d.Width, d.Height, d.Format, d.MultiSampleType, d.MultiSampleQuality, FALSE, &AccMS, NULL))) {
			Logger::Log("ExactPassSums: no multisampled buffer (%ux%u, msaa %u): off", d.Width, d.Height, (unsigned)d.MultiSampleType);
			return false;
		}
		if (FAILED(device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &AccTex, NULL)) ||
			FAILED(AccTex->GetSurfaceLevel(0, &Acc)) ||
			FAILED(device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &CopyTex, NULL)) ||
			FAILED(CopyTex->GetSurfaceLevel(0, &Copy))) {
			Logger::Log("ExactPassSums: could not make its buffers (%ux%u format %u): off", d.Width, d.Height, (unsigned)d.Format);
			Release();
			return false;
		}
		if (!ComposePS) {
			static const char* kPS =
				"sampler2D SceneS : register(s0); sampler2D AccS : register(s1); float4 G : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float3 a = tex2D(AccS, uv).rgb;"
				"  clip(max(a.r, max(a.g, a.b)) - 1e-10);"
				"  float3 s = max(tex2D(SceneS, uv).rgb, 0.0);"
				"  return float4(pow(pow(s, G.x) + max(a, 0.0), G.y) - s, 1.0); }";
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			if (FAILED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
				FAILED(device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ComposePS))) {
				Logger::Log("ExactPassSums: compose shader failed: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
				if (errors) errors->Release();
				if (code) code->Release();
				Release();
				return false;
			}
			code->Release();
		}
		Made = d;
		Logger::Log("ExactPassSums: buffers made (%ux%u format %u msaa %u)", d.Width, d.Height, (unsigned)d.Format, (unsigned)d.MultiSampleType);
		return true;
	}

	inline void SetFlag(IDirect3DDevice9* device, float v) {
		Redirect.x = v;
		device->SetPixelShaderConstantF(210, (const float*)&Redirect, 1);	// the bound shader too; the next binds upload it themselves
	}

	inline void Begin(IDirect3DDevice9* device, Kind kind) {
		IDirect3DSurface9* target = nullptr;
		if (FAILED(device->GetRenderTarget(0, &target)) || !target) return;
		D3DSURFACE_DESC d = {};
		target->GetDesc(&d);
		if (!Make(device, d)) { target->Release(); Failed.Set("its target could not be made"); return; }
		static GpuTimer timer("  Pass sums (clear)");
		GpuProfileScope gpu(timer, device);
		Scene = target;	// keeps the reference until End
		// UNOFFICIAL 2026-10-27 (RenderScale, effects\TAAUpscale.cpp): SetRenderTarget puts the viewport to the whole target; the run's
		// passes keep the scene's (the top-left render size while the world is drawn smaller; the whole target otherwise, as before).
		SceneViewportSaved = SUCCEEDED(device->GetViewport(&SceneViewport));
		device->SetRenderTarget(0, AccMS ? AccMS : Acc);
		if (SceneViewportSaved) device->SetViewport(&SceneViewport);
		DWORD scissor = FALSE;
		device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
		if (scissor) device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 0.0f, 0);
		if (scissor) device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
		SetFlag(device, 1.0f);
		Active = kind;
		Runs[kind]++;
		RunPasses = 0;
	}

	// Every state the fold-in sets (recorded once into a state block to restore them after, then set for real).
	inline void RecordFoldStates(IDirect3DDevice9* device) {
		const float g = Gamma > 0.0f ? Gamma : 1.0f;
		const float G[4] = { g, 1.0f / g, 0.0f, 0.0f };
		device->SetVertexShader(NULL);
		device->SetPixelShader(ComposePS);
		device->SetPixelShaderConstantF(0, G, 1);
		device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		for (DWORD s = 0; s < 2; s++) {
			device->SetTexture(s, s ? (IDirect3DBaseTexture9*)AccTex : (IDirect3DBaseTexture9*)CopyTex);
			device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		}
		const DWORD rs[][2] = { { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, TRUE },
			{ D3DRS_SRCBLEND, D3DBLEND_ONE }, { D3DRS_DESTBLEND, D3DBLEND_ONE }, { D3DRS_BLENDOP, D3DBLENDOP_ADD },
			{ D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
			{ D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_SCISSORTESTENABLE, FALSE },
			{ D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 7 }, { D3DRS_FILLMODE, D3DFILL_SOLID },
			{ D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE } };
		for (auto& r : rs) device->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
	}

	// LampSums: the copy on its stage (the game's texture cache told; the states as the fold-in's: point, clamp, no sRGB).
	inline void BindRunStart() {
		TheRenderManager->renderState->SetTexture(IncStage, IncTex);	// (through the game's cache: nothing sent when already bound)
		TheRenderManager->SetSamplerState(IncStage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		TheRenderManager->SetSamplerState(IncStage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		TheRenderManager->SetSamplerState(IncStage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		TheRenderManager->SetSamplerState(IncStage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		TheRenderManager->SetSamplerState(IncStage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		TheRenderManager->SetSamplerState(IncStage, D3DSAMP_SRGBTEXTURE, FALSE);
	}

	// LampSums: copy the frame as the lamp run begins, give its passes the copy and the flag. If the copy fails the run is still taken
	// (no retry on every pass of it) and keeps the old sums (flag 0).
	inline void BeginIncrements(IDirect3DDevice9* device, Kind kind) {
		Active = kind;
		Runs[kind]++;
		RunPasses = 0;
		RunIsIncrements = true;
		IncLive = false;
		IncStage = kind == Highlights ? kHighlightStartStage : kRunStartStage;	// UNOFFICIAL 2026-10-25 HighlightSums: its own stage
		IDirect3DSurface9* target = nullptr;
		if (FAILED(device->GetRenderTarget(0, &target)) || !target) { IncFailed++; return; }
		D3DSURFACE_DESC d = {};
		target->GetDesc(&d);
		const bool made = MakeIncrements(device, d);
		static GpuTimer timer("  Lamp sums (copy)");
		bool copied = false;
		if (made) {
			GpuProfileScope gpu(timer, device);
			copied = SUCCEEDED(device->StretchRect(target, NULL, IncSurf, NULL, D3DTEXF_NONE));
		}
		target->Release();
		if (!copied) { IncFailed++; if (!made) Failed.Set("its copy target could not be made"); return; }
		BindRunStart();
		Redirect = D3DXVECTOR4(0.0f, 1.0f, 1.0f / (float)d.Width, 1.0f / (float)d.Height);
		device->SetPixelShaderConstantF(210, (const float*)&Redirect, 1);	// the bound shader too; the next binds upload it themselves
		IncLive = true;
		if (kind == Highlights) IncHlCopies++; else IncCopies++;
	}

	inline void EndIncrements(IDirect3DDevice9* device) {
		const Kind kind = Active;
		Active = None;
		RunIsIncrements = false;
		if (!IncLive) return;
		IncLive = false;
		if (kind == Highlights) IncHlPasses += RunPasses; else IncPasses += RunPasses;
		Redirect = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		device->SetPixelShaderConstantF(210, (const float*)&Redirect, 1);
		TheRenderManager->renderState->SetTexture(IncStage, nullptr);
	}

	// The run is over: the scene back as the target, and the buffer folded into it in linear light.
	inline void End(IDirect3DDevice9* device) {
		if (Active == None) return;
		if (RunIsIncrements) { EndIncrements(device); return; }
		Active = None;
		SetFlag(device, 0.0f);
		static GpuTimer timer("  Pass sums (fold in)");
		GpuProfileScope gpu(timer, device);
		device->SetRenderTarget(0, Scene);
		if (SceneViewportSaved) device->SetViewport(&SceneViewport);	// UNOFFICIAL 2026-10-27 (RenderScale): the scene's viewport, as in Begin
		IDirect3DStateBlock9* saved = nullptr;
		if (SUCCEEDED(device->BeginStateBlock())) {	// records only the states the fold-in sets (cheaper than capturing all)
			RecordFoldStates(device);
			device->EndStateBlock(&saved);
			if (saved) saved->Capture();
		}
		if (saved && SUCCEEDED(device->StretchRect(Scene, NULL, Copy, NULL, D3DTEXF_NONE)) &&
			(!AccMS || SUCCEEDED(device->StretchRect(AccMS, NULL, Acc, NULL, D3DTEXF_NONE)))) {
			RecordFoldStates(device);
			const float w = (float)Made.Width, h = (float)Made.Height;
			const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 },
				{ -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
			device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
			saved->Apply();
			if (AfterStateBlock) AfterStateBlock(device);	// UNOFFICIAL 2026-10-29 (CapturedNormals' lean hook re-reads the state)
			Composes++;
		}
		else Logger::Log("ExactPassSums: fold-in skipped (state block or copy failed): this frame's run keeps its separate sums wrong way");
		if (saved) saved->Release();
		Scene->Release();
		Scene = nullptr;
	}

	// Render.cpp, once a frame (image space), and before every world pass.
	inline void BeginFrame(int mode) {
		if (mode != LastMode) { LastMode = mode; LogFrames = 3; } else if (LogFrames > 0) LogFrames--;
		Mode = Failed.Blocked() ? 0 : mode;
		Runs[0] = Runs[1] = Runs[2] = 0;
		const bool foldIn = ((Mode & Highlights) && !(Mode & kHighlightIncrements)) || ((Mode & Lamps) && !(Mode & kIncrements));
		if (foldIn && ++Frames >= 600) { Logger::Log("ExactPassSums (mode %d): %.2f fold-ins per frame", Mode, Composes / (float)Frames); Frames = Composes = 0; }
		if ((Mode & kIncrements) && (Mode & Lamps) && ++IncFrames >= 600) {
			Logger::Log("UNOFFICIAL LampSums: per frame %.2f lamp runs copied (%.1f extra lamp passes each), %.2f extra lamp passes left as before (past the run cap), "
				"%.2f copies failed", IncCopies / (float)IncFrames, IncCopies ? IncPasses / (float)IncCopies : 0.0f, IncLeft / (float)IncFrames, IncFailed / (float)IncFrames);
			IncFrames = IncCopies = IncPasses = IncLeft = IncFailed = 0;
		}
		if ((Mode & kHighlightIncrements) && (Mode & Highlights) && ++IncHlFrames >= 600) {
			Logger::Log("UNOFFICIAL HighlightSums: per frame %.2f highlight runs copied (%.1f highlight passes each), %.2f highlight passes left as before (past the run cap)",
				IncHlCopies / (float)IncHlFrames, IncHlCopies ? IncHlPasses / (float)IncHlCopies : 0.0f, IncHlLeft / (float)IncHlFrames);
			IncHlFrames = IncHlCopies = IncHlPasses = IncHlLeft = 0;
		}
		if (!foldIn && AccTex) Release();
		const bool anyIncrements = ((Mode & kIncrements) && (Mode & Lamps)) || ((Mode & kHighlightIncrements) && (Mode & Highlights));
		if (!anyIncrements && IncTex) ReleaseIncrements();
	}

	// Render.cpp, once a frame: the mode. ExactPassSums (a test switch) wins; else LampSums when it was compiled into the shaders (on at
	// startup, with linear lighting: ShaderRecord LAMP_SUMS_ON) and linear lighting is on now.
	inline int ModeFromSettings() {
		if (!LampSumsChecked) {
			LampSumsChecked = true;
			LampSumsCompiled = TheSettingManager->GetSettingI("Shaders.PBR.Main", "LampSums") != 0;
			Logger::Log("UNOFFICIAL LampSums: %s", LampSumsCompiled ? "compiled in (the extra lamp passes add their light in linear light)" :
				"off (not compiled into the shaders)");
		}
		if (!HighlightSumsChecked) {	// UNOFFICIAL 2026-10-25 HighlightSums (HIGHLIGHT_SUMS_ON), the same way
			HighlightSumsChecked = true;
			HighlightSumsCompiled = TheSettingManager->GetSettingI("Shaders.PBR.Main", "HighlightSums") != 0;
			Logger::Log("UNOFFICIAL HighlightSums: %s", HighlightSumsCompiled ? "compiled in (the highlight passes FoldHighlights leaves to the game add their light in linear light)" :
				"off (not compiled into the shaders)");
		}
		if (LinearScene::Live) return 0;	// UNOFFICIAL 2026-10-28 LinearScene: the game's blends already add in linear light
		const int exact = TheSettingManager->GetSettingI("Shaders.PBR.Linear", "ExactPassSums");
		int mode = 0;
		if (LampSumsCompiled && Gamma > 1.0f && TheSettingManager->GetSettingI("Shaders.PBR.Main", "LampSums")) mode |= kIncrements | Lamps;
		if (HighlightSumsCompiled && Gamma > 1.0f && TheSettingManager->GetSettingI("Shaders.PBR.Main", "HighlightSums")) mode |= kHighlightIncrements | Highlights;
		// UNOFFICIAL 2026-10-24 / 2026-10-25: ExactPassSums (test) and the increments combine per kind: a kind the test asks for (1 lamps,
		// 2 highlights, 3 both) goes through the fold-in buffer instead of its increments; the other kind keeps its increments. (2026-10-24,
		// owner: the lamp fold-in whitened rubble on floors -- a run of lamp passes the hooks closed only after the object's colour pass, so
		// its light was added on top of the colour; the increments add each pass's light at once.)
		if (exact & Lamps) mode = (mode & ~(kIncrements | Lamps)) | Lamps;
		if (exact & Highlights) mode = (mode & ~(kHighlightIncrements | Highlights)) | Highlights;
		return mode;
	}

	inline void BeforePass(const void* pass, bool world, IDirect3DDevice9* device) {
		if (!Mode || !pass || !device) return;
		if (!world) { if (Active) End(device); return; }
		const UInt16 e = *(const UInt16*)((const UInt8*)pass + 0x04);
		const Kind k = KindOf(e);
		if (k == Active) {
			RunPasses++; RunLast = e;
			if (IncLive) TheRenderManager->renderState->SetTexture(IncStage, IncTex);	// LampSums / HighlightSums: still the copy (a bind in the run may set it)
			return;
		}
		if (Active) {
			if (LogFrames > 0) Logger::Log("ExactPassSums run: kind %d (%s), %u passes (%03X..%03X), ended by pass %03X", (int)Active,
				RunIsIncrements ? (Active == Highlights ? "HighlightSums" : "LampSums") : "fold-in", RunPasses, RunFirst, RunLast, e);
			End(device);
		}
		const bool increments = (k == Lamps && (Mode & kIncrements)) || (k == Highlights && (Mode & kHighlightIncrements));
		const int cap = increments ? kIncrementRunCap : kFoldRunCap;
		if (k != None && (Mode & k) && Runs[k] >= cap) {
			if (LogFrames > 0) Logger::Log("ExactPassSums: pass %03X of kind %d NOT redirected (run %d this frame, past the cap of %d)", e, (int)k, Runs[k] + 1, cap);
			if (k == Lamps && (Mode & kIncrements)) IncLeft++;
			else if (k == Highlights && (Mode & kHighlightIncrements)) IncHlLeft++;
		}
		if (k != None && (Mode & k) && Runs[k] < cap) {
			if (increments) BeginIncrements(device, k); else Begin(device, k);
			RunPasses = 1; RunFirst = RunLast = e;
		}
	}

	inline void EndWorld(IDirect3DDevice9* device) {
		if (!Active || !device) return;
		if (LogFrames > 0) Logger::Log("ExactPassSums run: kind %d (%s), %u passes (%03X..%03X), ended by the world's end", (int)Active,
			RunIsIncrements ? (Active == Highlights ? "HighlightSums" : "LampSums") : "fold-in", RunPasses, RunFirst, RunLast);
		End(device);
	}
}
