#pragma once
// UNOFFICIAL 2026-11-19 DeferredWetness ([Main.Main.Quality] DeferredWetness, restart, test only, off; docs/DESIGN.md "Deferred wetness design" step 1:
// the land; step 2, 2026-11-20: the objects and parallax too, their lighting-free records with DEFERRED_WETNESS; Shaders/DeferredWetness.hlsl).
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
#include "RenderRect.h"	// (2026-11-20 step 3) the fallback draws the render rect only

namespace DeferredWetness {
	// UNOFFICIAL 2026-11-20 unified reflections step 2 (effects/Reflections/Reflections.cpp ReflectionsEffect::WetReach): the distance out to which the
	// Reflections effect mirrors the wet floors alone this frame (0: it does not). The pass and the fallback take their sky reflection out there.
	float ReflectionsWetReach();
	inline ShaderRecordPixel* Pass = nullptr;		// the wet frames' pass (Shaders/DeferredWetness.hlsl)
	inline ShaderRecordPixel* Restore = nullptr;	// the dry frames' (DW_RESTORE): COLOR2.r back to the land's code
	inline bool PassFailed = false;
	inline int Wanted = -1;
	inline bool FilmLive = false;					// this frame's film target holds the land's film (DeferredSun, WetnessReflections)
	inline bool Done = false;						// Apply drew in this span (EnsureRestored)
	// (2026-11-20) the land lamp pass drew the lamps' streak on the film this frame (DeferredLamps.hlsl DL_WET_FILM, with the lamps' shadows):
	// the Wetness Effects screen pass then leaves those pixels out of its own, unshadowed streak
	inline bool LampFilmDrawn = false;
	inline IDirect3DTexture9* FilmTexture = nullptr;  inline IDirect3DSurface9* FilmSurface = nullptr;
	inline IDirect3DTexture9* AlbedoCopy = nullptr;   inline IDirect3DSurface9* AlbedoCopySurface = nullptr;
	inline IDirect3DTexture9* MaterialCopy = nullptr; inline IDirect3DSurface9* MaterialCopySurface = nullptr;
	inline D3DSURFACE_DESC Made = {};
	inline bool TargetsFailed = false;
	inline unsigned long long StatWet = 0, StatDry = 0, StatSkipped = 0;
	// UNOFFICIAL 2026-11-20 step 3 (docs/deferred-wetness-design.md "Step 3 as built"): the fallback (Shaders/DeferredWetness.hlsl DW_FALLBACK, Fallback
	// below): the film for every pixel the pass did not work out, and the water layer on the lit colour where no record drew at all
	inline ShaderRecordPixel* FallbackPass = nullptr;
	inline bool FallbackDrawn = false;				// this frame's film target holds the fallback's film too (Reflections, WetnessReflections)
	inline UInt32 FallbackFrame = 0xFFFFFFFFu;		// (2026-11-20 unified reflections step 2) the FrameNumber the fallback last drew in (ReflectionsEffect::WetReach)
	inline bool FallbackOff = false;				// latched: it cannot draw this session (logged once)
	inline IDirect3DTexture9* FrameCopy = nullptr;    inline IDirect3DSurface9* FrameCopySurface = nullptr;  inline D3DSURFACE_DESC FrameCopyMade = {};
	inline constexpr float FallbackRange = 10240.0f;	// game units: about the loaded cells' reach (uGridsToLoad 5); the game's own far shaders stay dry

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
		Logger::Log("UNOFFICIAL DeferredWetness: the pass loaded; the near land's, objects' and parallax's lighting-free records are made without their water layer (DEFERRED_WETNESS)");
		// (2026-11-20 step 3) the fallback: not needed for the pass above to work (without it the other pixels keep their own wetness, and the
		// Reflections effect its own guess for frames without a film)
		static const D3DXMACRO fallback[] = { { "DW_FALLBACK", "1" }, { NULL, NULL } };
		ShaderTemplate fallbackTmpl = {};
		fallbackTmpl.Name = "DeferredWetness";
		FallbackPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredWetness.pso", NULL, fallbackTmpl, "Deferred\\WetnessFallback\\", fallback);
		if (FallbackPass) FallbackPass->ClearSamplers = false;
		Logger::Log(FallbackPass ? "UNOFFICIAL DeferredWetness: the fallback loaded (the film for the pixels the pass does not draw; the game's own shaders made wet on the lit colour)" :
			"[WARNING] UNOFFICIAL DeferredWetness: the fallback (DW_FALLBACK) did not load: the pixels no record drew stay dry, and the Reflections effect keeps its own guess there");
	}

	// Once a frame (Render.cpp, with DeferredSun::BeginFrame).
	inline void BeginFrame() { FilmLive = false; LampFilmDrawn = false; FallbackDrawn = false; }

	// (2026-11-20 step 3) the film target holds this frame's film: the pass's (inside the sun pass) and / or the fallback's (in the effect chain,
	// before the Wetness Effects screen pass). The Reflections effect and the screen pass read it from then on.
	// (review 2026-11-20) not when the G-buffer does not line up with the effects' frame this frame (CapturedNormals::Data.x 0: EffectChain.cpp
	// step 23 clears it when the effects run after the upscale, where the film holds only the render rect)
	inline bool FilmReady() { return (FilmLive || FallbackDrawn) && FilmTexture && CapturedNormals::Data.x > 0.0f; }
	// The film covers the whole screen (the fallback drew too): only then may a reader drop its own wetness for pixels without a film (the
	// Reflections effect); with the fallback off the film holds only the pass's pixels
	inline bool FilmWhole() { return FallbackDrawn && FilmReady(); }

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
		// c6.y (2026-11-20 step 2): [Shaders.Puddles.Main] ObjectPuddles, as effects/Puddles/Puddles.cpp SetDrawConstants gives the objects their level
		const float objectPuddles = TheSettingManager->GetSettingI("Shaders.Puddles.Main", "ObjectPuddles") != 0 ? 1.0f : 0.0f;
		// c6.z (2026-11-20 unified reflections step 2): the Reflections effect's reach over the wet floors (DWSkyKeep; 0: the sky reflection as before)
		const float own[3][4] = { { linearDepth ? 1.0f : 0.0f, objectPuddles, full ? ReflectionsWetReach() : 0.0f, 0.0f }, { fog[0], fog[1], fog[2], fog[3] }, { proj._33, proj._43, 0.01f, 2.0f } };
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

	// UNOFFICIAL 2026-11-20 step 3 (docs/deferred-wetness-design.md "Step 3 as built"): the fallback, in the effect chain right before the Wetness
	// Effects screen pass (EffectChain.cpp PreStep_WetFlashlightSpecularUnderwater), outdoors in wet frames. One full-screen draw on the lit frame
	// with two targets: the frame (rgb: the water layer where no record drew) and the film target (the film for every pixel the pass inside the sun
	// pass did not work out; cleared first in a frame that pass did not run). The frame is drawn in place: the chain's current image (or, off the
	// chain, the effects' target, copied back to the rendered buffer after), read from a copy. Needs the frame and the film at one size and format
	// (the targets of one draw); otherwise it is off for the session, logged once.
	// (2026-11-21, review of the WetWorld removal) Fallback may draw this frame: its own cheap tests, for the frame-order decisions made before it
	// (EffectChain.cpp effectsBetween, bounceIntoSpecular), which WetWorld's wet frames used to hold off.
	inline bool FallbackMayDraw() {
		return FallbackPass && !FallbackOff && CompiledIn() && TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater && WetNow();
	}
	inline void Fallback(IDirect3DDevice9* d, IDirect3DSurface9* renderTarget) {
		if (!FallbackMayDraw()) return;
		// (review 2026-11-20) the G-buffer and the depth frame block must be this frame's and line up with the effects' frame (Data.x: see FilmReady)
		if (!CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture || CapturedNormals::AlbedoData.x < 0.5f || CapturedNormals::MaterialData.x < 0.5f ||
			CapturedNormals::Data.x <= 0.0f || !DeferredDepth::Ready() || !MakeTargets(d) || !renderTarget) return;
		FrameChain& chain = TheShaderManager->Chain;
		const bool chained = chain.Owns(renderTarget, TheTextureManager->RenderedSurface);	// (puts back an image an earlier effect wrote to the screen)
		IDirect3DSurface9* const target = chained ? TheTextureManager->RenderedSurface : renderTarget;
		D3DSURFACE_DESC t = {};
		if (!target || FAILED(target->GetDesc(&t))) return;
		if (t.Width != Made.Width || t.Height != Made.Height || t.Format != Made.Format || t.MultiSampleType != D3DMULTISAMPLE_NONE) {
			FallbackOff = true;
			Logger::Log("[WARNING] UNOFFICIAL DeferredWetness: the fallback is off this session: the frame (%ux%u, format %u, multisample %u) and the film (%ux%u, format %u) differ",
				t.Width, t.Height, (unsigned)t.Format, (unsigned)t.MultiSampleType, Made.Width, Made.Height, (unsigned)Made.Format);
			return;
		}
		if (!FrameCopy || FrameCopyMade.Width != t.Width || FrameCopyMade.Height != t.Height || FrameCopyMade.Format != t.Format) {
			if (FrameCopySurface) { FrameCopySurface->Release(); FrameCopySurface = nullptr; }
			if (FrameCopy) { FrameCopy->Release(); FrameCopy = nullptr; }
			if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &FrameCopy, NULL)) || !FrameCopy ||
				FAILED(FrameCopy->GetSurfaceLevel(0, &FrameCopySurface))) {
				if (FrameCopy) { FrameCopy->Release(); FrameCopy = nullptr; }
				FallbackOff = true;
				Logger::Log("[WARNING] UNOFFICIAL DeferredWetness: the fallback is off this session: no copy of the frame could be made");
				return;
			}
			FrameCopyMade = t;
		}
		if (FAILED(d->StretchRect(target, NULL, FrameCopySurface, NULL, D3DTEXF_NONE))) return;
		static GpuTimer timer("Deferred wetness fallback");
		GpuProfileScope gpu(timer, d);

		auto* rs = TheRenderManager->renderState;
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
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
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_ALPHABLENDENABLE,
			D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0x7u, 0xFu, FALSE, FALSE, FALSE };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }

		for (UInt32 i = 0; i < FallbackPass->TextureShaderValuesCount; i++) {
			TextureRecord* rec = FallbackPass->TextureShaderValues[i].Texture;
			if (rec && rec->TextureRef) rec->Texture = *rec->TextureRef;
		}
		FallbackPass->SetCT();
		// DW_FB (c15): z (review 2026-11-20) [Shaders.Puddles.Main] ObjectPuddles, as Apply's c6.y: only the land gets the water level unless it is on
		const float objectPuddles = TheSettingManager->GetSettingI("Shaders.Puddles.Main", "ObjectPuddles") != 0 ? 1.0f : 0.0f;
		const float own[4] = { FilmLive ? 1.0f : 0.0f, FallbackRange, objectPuddles, ReflectionsWetReach() };	// w (2026-11-20): as Apply's c6.z
		d->SetPixelShaderConstantF(15, own, 1);
		IDirect3DBaseTexture9* const inputs[3] = { FrameCopy, CapturedNormals::AlbedoTexture, CapturedNormals::MaterialTexture };
		for (DWORD s = 0; s < 3; s++) {
			rs->SetTexture(s, inputs[s]);
			TheRenderManager->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); TheRenderManager->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			TheRenderManager->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE); TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			TheRenderManager->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); TheRenderManager->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		}
		for (DWORD s = 3; s < 16; s++) {	// (no stage may hold the film while it is a target)
			IDirect3DBaseTexture9* bound = nullptr;
			if (SUCCEEDED(d->GetTexture(s, &bound)) && bound) { if (bound == FilmTexture) rs->SetTexture(s, nullptr); bound->Release(); }
		}
		if (!FilmLive) d->ColorFill(FilmSurface, NULL, 0);	// (the pass did not run this frame: no film anywhere yet)
		d->SetRenderTarget(0, target);
		d->SetRenderTarget(1, FilmSurface);
		for (DWORD i = 2; i < 4; i++) d->SetRenderTarget(i, nullptr);
		// the drawn part only: the whole target, or under RenderScale its top-left rect (review 2026-11-20: the pixels past it are not shown)
		const UINT w = RenderRect::Width(t.Width), h = RenderRect::Active() ? (UINT)floorf((float)t.Height * RenderRect::ScaleY + 0.5f) : t.Height;
		const D3DVIEWPORT9 drawn = { 0, 0, w, h, 0.0f, 1.0f };
		d->SetViewport(&drawn);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(FallbackPass->ShaderHandle);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)w - 0.5f, y1 = (float)h - 0.5f, u1 = (float)w / t.Width, v1 = (float)h / t.Height;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0.0f, 0.0f }, { x1, -0.5f, 0, 1, u1, 0.0f }, { -0.5f, y1, 0, 1, 0.0f, v1 }, { x1, y1, 0, 1, u1, v1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		FallbackDrawn = true;
		FallbackFrame = TheFrameRateManager->FrameNumber;
		static bool told = false;
		if (!told) { told = true; Logger::Log("UNOFFICIAL DeferredWetness: the fallback drew (%s)", chained ? "on the chain's image" : "on the effects' target"); }

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
		// off the chain the next effect reads the rendered buffer, which the effects keep as a copy of their target
		if (!chained && TheTextureManager->RenderedSurface && TheTextureManager->RenderedSurface != target)
			d->StretchRect(target, NULL, TheTextureManager->RenderedSurface, NULL, D3DTEXF_NONE);
	}
}
