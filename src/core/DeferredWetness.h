#pragma once
// UNOFFICIAL 2026-11-19 DeferredWetness ([Main.Main.Quality] DeferredWetness, restart, test only, off; docs/deferred-wetness-design.md step 1:
// the land; Shaders/DeferredWetness.hlsl).
// The land's water layer (Wetness Effects + Puddles: the darker albedo, the flattened puddle normal, the film) worked out once per pixel in a
// full-screen pass after the G-buffer is resolved, instead of inside every land shader: the near land's lighting-free records (LAMPS_DEFERRED 2)
// are made with DEFERRED_WETNESS (core/LightFreeRecords.h), which drops their own water layer (TerrainTemplate.hlsl TERRAIN_WET_DEFERRED) and
// leaves the layers' height in COLOR2.r's fraction. The pass runs inside DeferredSun::Apply, after the frame is put into linear light and
// before the sun is added, so the sun and the lamps light the wet albedo and flat normal; it rewrites the albedo and material targets and the
// frame (the record's ambient made wet + the film's sky reflection), and fills the film target (FilmTexture) that DeferredSun (the film's sun
// highlight, DS_WET_FILM) and the Wetness Effects screen pass (the lamps' streaks, the screen-space reflection) read this frame (FilmLive).
// COLOR2.r: the land records write 4 + 0.99 x height there under the switch; this pass writes 4 + 16 back for every such pixel, wet or dry (the
// restore record, DW_RESTORE, in dry frames), before any other reader of the material target runs.
// Needs the lighting-free records (the deferred renderer's mode 2), GBufferV2 and Wetness Effects at startup. Off (the default) or not loaded:
// nothing is compiled, the land records are what they were, nothing runs.
#include "CapturedNormals.h"
#include "DeferredDepth.h"
#include "DeferredFog.h"
#include "LightFreeRecords.h"
#include "LinearScene.h"
#include "ShaderRecord.h"
#include "GpuProfiler.h"

namespace DeferredWetness {
	inline ShaderRecordPixel* Pass = nullptr;		// the wet frames' pass (Shaders/DeferredWetness.hlsl)
	inline ShaderRecordPixel* Restore = nullptr;	// the dry frames' (DW_RESTORE): COLOR2.r back to the land's code
	inline bool PassFailed = false;
	inline int Wanted = -1;
	inline bool FilmLive = false;					// this frame's film target holds the land's film (DeferredSun, WetnessReflections)
	inline bool Done = false;						// Apply drew in this span (EnsureRestored)
	inline IDirect3DTexture9* FilmTexture = nullptr;  inline IDirect3DSurface9* FilmSurface = nullptr;
	inline IDirect3DTexture9* AlbedoCopy = nullptr;   inline IDirect3DSurface9* AlbedoCopySurface = nullptr;
	inline IDirect3DTexture9* MaterialCopy = nullptr; inline IDirect3DSurface9* MaterialCopySurface = nullptr;
	inline D3DSURFACE_DESC Made = {};
	inline bool TargetsFailed = false;
	inline unsigned long long StatWet = 0, StatDry = 0, StatSkipped = 0;

	inline bool WantedAtStartup() {
		if (Wanted < 0) Wanted = TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredWetness") != 0 && LightFreeRecords::WantedAtStartup() &&
			CapturedNormals::V2CompiledIn() && TheSettingManager->GetSettingI("Shaders.WetnessEffects.Status", "Enabled") != 0;
		return Wanted != 0;
	}
	inline bool CompiledIn() { return Pass && Restore; }
	// (review 2026-11-19) the pass cannot put the land's code back this session (its targets or copies failed): DeferredSun::Wanted() then
	// refuses, so the deferred renderer draws forward and the forward land records keep their own water layer
	inline bool Broken() { return CompiledIn() && TargetsFailed; }

	// ShaderManager::Initialize, before DeferredSun::LoadShaders (which compiles the film's highlight in only when this loaded) and before the
	// game's shaders load (LightFreeRecords::Load gives the near land DEFERRED_WETNESS only when this loaded).
	inline void LoadShaders() {
		if (!WantedAtStartup() || Pass || PassFailed) return;
		ShaderTemplate tmpl = {};
		tmpl.Name = "DeferredWetness";
		Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredWetness.pso", NULL, tmpl, "Deferred\\Wetness\\", nullptr);
		static const D3DXMACRO restore[] = { { "DW_RESTORE", "1" }, { NULL, NULL } };
		ShaderTemplate restoreTmpl = {};
		restoreTmpl.Name = "DeferredWetness";
		Restore = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredWetness.pso", NULL, restoreTmpl, "Deferred\\WetnessRestore\\", restore);
		if (!Pass || !Restore) {
			PassFailed = true;
			Pass = Restore = nullptr;
			Logger::Log("[ERROR] UNOFFICIAL DeferredWetness: the pass (Shaders\\DeferredWetness.hlsl) did not load: the land keeps its own water layer (the switch does nothing this session)");
			return;
		}
		Pass->ClearSamplers = false;
		Restore->ClearSamplers = false;
		LightFreeRecords::WetCompiled = true;
		Logger::Log("UNOFFICIAL DeferredWetness: the pass loaded; the near land's lighting-free records are made without their water layer (DEFERRED_WETNESS)");
	}

	// Once a frame (Render.cpp, with DeferredSun::BeginFrame).
	inline void BeginFrame() { FilmLive = false; }

	// The copies the pass reads and the film target, at the G-buffer's size and format.
	inline bool MakeTargets(IDirect3DDevice9* d) {
		if (TargetsFailed || !CapturedNormals::AlbedoTexture) return false;
		D3DSURFACE_DESC t = {};
		if (FAILED(CapturedNormals::AlbedoTexture->GetLevelDesc(0, &t))) return false;
		if (FilmTexture && Made.Width == t.Width && Made.Height == t.Height && Made.Format == t.Format) return true;
		auto release = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		release(FilmSurface); release(FilmTexture); release(AlbedoCopySurface); release(AlbedoCopy); release(MaterialCopySurface); release(MaterialCopy);
		auto make = [&](IDirect3DTexture9*& tex, IDirect3DSurface9*& surface) {
			return SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &tex, NULL)) && tex && SUCCEEDED(tex->GetSurfaceLevel(0, &surface));
		};
		if (!make(FilmTexture, FilmSurface) || !make(AlbedoCopy, AlbedoCopySurface) || !make(MaterialCopy, MaterialCopySurface)) {
			release(FilmSurface); release(FilmTexture); release(AlbedoCopySurface); release(AlbedoCopy); release(MaterialCopySurface); release(MaterialCopy);
			TargetsFailed = true;
			Logger::Log("[WARNING] UNOFFICIAL DeferredWetness: could not make its targets (%ux%u): the land is dry under the deferred renderer this session", t.Width, t.Height);
			return false;
		}
		Made = t;
		Logger::Log("UNOFFICIAL DeferredWetness: its targets are made (the film and two copies, %ux%u fp16)", t.Width, t.Height);
		return true;
	}

	// Is anything wet this frame (Wetness Effects' TESR_WetnessFlags.x: on and active).
	inline bool WetNow() {
		auto it = TheShaderManager->ConstantsTable.find("TESR_WetnessFlags");
		return it != TheShaderManager->ConstantsTable.end() && it->second && it->second->x > 0.5f;
	}

	// DeferredSun::Apply, after it put the frame (target, the span's scene target) into linear light and before it adds the sun; not for the
	// first-person view model. linearDepth: DeferredDepth 2's exact view z is ready this frame.
	// target null: the restore record only (EnsureRestored).
	inline void Apply(IDirect3DDevice9* d, IDirect3DSurface9* target, bool linearDepth) {
		FilmLive = false;
		if (!CompiledIn()) return;
		if (!CapturedNormals::AlbedoSurface || !CapturedNormals::MaterialSurface || !CapturedNormals::Texture || !MakeTargets(d)) { StatSkipped++; return; }
		// (review 2026-11-19) only with the exact linear view z (DeferredDepth 2, which the deferred renderer's mode 2 forces): without it the pass
		// cannot tell where something without a G-buffer covers the land, and would paint the land's wetness over it: restore only then
		const bool wet = target && linearDepth && WetNow();
		IDirect3DTexture9* frameCopy = nullptr;
		if (wet) frameCopy = DeferredFog::FrameCopy(d, target);
		const bool full = wet && frameCopy;	// (no frame copy: the land stays dry this frame, its code is still put back)
		// the copies (the restore record reads the albedo target itself: it does not write it)
		if ((full && FAILED(d->StretchRect(CapturedNormals::AlbedoSurface, NULL, AlbedoCopySurface, NULL, D3DTEXF_NONE))) ||
			FAILED(d->StretchRect(CapturedNormals::MaterialSurface, NULL, MaterialCopySurface, NULL, D3DTEXF_NONE))) {
			StatSkipped++;
			TargetsFailed = true;	// (latched: Broken(), the renderer draws forward from the next frame)
			Logger::Log("[WARNING] UNOFFICIAL DeferredWetness: a copy of the G-buffer failed: the deferred renderer draws forward for the rest of the session (the land keeps its own water layer there)");
			return;
		}
		static GpuTimer timer("Deferred wetness");
		GpuProfileScope gpu(timer, d);
		if (!DeferredDepth::Ready() && !linearDepth) DeferredDepth::FillFrame(d);	// the ray mapping (TESR_DeferredDepthFrame), as DeferredSun

		auto* rs = TheRenderManager->renderState;
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		D3DSURFACE_DESC t = Made;	// (the G-buffer's size: the frame's, which the pass covers through the viewport)
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		// every stage, through the game's state cache (as DeferredSun::Apply)
		IDirect3DBaseTexture9* tex[16] = {};
		static const int kSamplerStates = 13;	// D3DSAMP_ADDRESSU (1) .. D3DSAMP_DMAPOFFSET (13)
		DWORD savedSS[16][kSamplerStates];
		for (DWORD s = 0; s < 16; s++) {
			d->GetTexture(s, &tex[s]);
			for (int k = 0; k < kSamplerStates; k++) savedSS[s][k] = rs->GetSamplerState(s, (D3DSAMPLERSTATETYPE)(k + 1));
		}
		static float savedC[224][4];
		d->GetPixelShaderConstantF(0, &savedC[0][0], 224);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
			D3DRS_COLORWRITEENABLE3, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		// a replace, no depth test: every near-land pixel's code is put back, also where something nearer covers it (the shader keeps the
		// frame as it was there: the view z against the depth copy, as DeferredSun's DS_DEPTH_AGREE)
		// (target 0: the frame's rgb only in the full pass, as DeferredSun; the material target's four channels in the restore pass)
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, full ? 0x7u : 0xFu, 0xF, 0xF, 0xF, FALSE, FALSE, FALSE };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }

		ShaderRecordPixel* const pass = full ? Pass : Restore;
		for (UInt32 i = 0; i < pass->TextureShaderValuesCount; i++) {
			TextureRecord* rec = pass->TextureShaderValues[i].Texture;
			if (rec && rec->TextureRef) rec->Texture = *rec->TextureRef;
		}
		pass->SetCT();
		const D3DXMATRIX& proj = NiDX9Renderer::GetSingleton()->projMatrix;
		float fog[4] = {};
		if (!DeferredFog::Params(fog)) fog[3] = 0.0f;
		const float own[3][4] = { { linearDepth ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f }, { fog[0], fog[1], fog[2], fog[3] }, { proj._33, proj._43, 0.01f, 2.0f } };
		d->SetPixelShaderConstantF(6, own[0], 3);
		// the copies on s0-s2 (point, clamp), through the state cache like SetCT's stages (put back below)
		IDirect3DBaseTexture9* const copies[3] = { frameCopy, full ? AlbedoCopy : CapturedNormals::AlbedoTexture, MaterialCopy };
		for (DWORD s = 0; s < 3; s++) {
			rs->SetTexture(s, copies[s]);
			TheRenderManager->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); TheRenderManager->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			TheRenderManager->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE); TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); TheRenderManager->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		}
		if (full) {
			d->ColorFill(FilmSurface, NULL, 0);	// gloss 0 wherever the pass does not write
			d->SetRenderTarget(1, CapturedNormals::AlbedoSurface);
			d->SetRenderTarget(2, CapturedNormals::MaterialSurface);
			d->SetRenderTarget(3, FilmSurface);
		}
		else {
			d->SetRenderTarget(0, CapturedNormals::MaterialSurface);
			for (DWORD i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
		}
		d->SetViewport(&vp);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(pass->ShaderHandle);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		FilmLive = full;
		Done = true;
		if (full) StatWet++; else StatDry++;
		static int told = -1;
		if (told != (int)full) {
			told = full;
			Logger::Log(full ? "UNOFFICIAL DeferredWetness: wet: the land's water layer drawn by the pass" : "UNOFFICIAL DeferredWetness: dry (or no frame copy): the pass only puts the land's code back");
		}

		for (DWORD i = 0; i < 4; i++) { d->SetRenderTarget(i, rt[i]); if (rt[i]) rt[i]->Release(); }
		d->SetViewport(&vp);
		for (DWORD s = 0; s < 16; s++) {
			rs->SetTexture(s, tex[s]);
			for (int k = 0; k < kSamplerStates; k++)
				if (rs->GetSamplerState(s, (D3DSAMPLERSTATETYPE)(k + 1)) != savedSS[s][k]) TheRenderManager->SetSamplerState(s, (D3DSAMPLERSTATETYPE)(k + 1), savedSS[s][k]);
			if (tex[s]) tex[s]->Release();
		}
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, &savedC[0][0], 224);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
	}

	// UNOFFICIAL 2026-11-19 DeferredSun::Apply's end, on every return after its span had lighting-free binds (its guard): where Apply did not run
	// (a frame DeferredSun skipped: no directional light, no scene target, LinearScene's span, a failed conversion), the land's height code is
	// put back anyway, so no later reader of COLOR2 (the lamp pass, the flashlight, the path tracer, the DLSS guides) sees r = 4 + height.
	inline void EnsureRestored(IDirect3DDevice9* d) {
		if (CompiledIn() && !Done) Apply(d, nullptr, false);
		Done = false;
	}
}
