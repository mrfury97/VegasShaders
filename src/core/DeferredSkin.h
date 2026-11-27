#pragma once
// UNOFFICIAL 2026-11-15 full deferred renderer, phase 4: skin (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.6 / 4.7, results and bench request:
// docs/next/FD-SKIN-RESULTS.md).
//
// [Main.Main.Quality] DeferredSkin (restart, test only, off): the skin's deferred record (SkinTemplate.hlsl SKIN_DEFERRED: LAMPS_DEFERRED 2 with
// DeferredRenderer 2, every direct light out; 3 with DeferredRenderer 1, the lamps out and the sun kept) for the full (ADTS: SKIN2000-2003)
// and many-lamp (ADTS10: SKIN2010-2012) families, writing G-buffer v2 with the skin model (6) and what the skin's own lighting needs
// (Includes/SkinDeferred.hlsl); DeferredSun and DeferredLamps light those pixels with Includes/Skin.hlsl's SkinLight (FD_SKIN). Not: the
// light-only (AD: SKIN2004-2007) passes, whose texture the game multiplies in by a later pass, and the added lamp passes (SKIN2008 / 2009),
// which a deferred skin's object skips (DeferredRenderer::Skip, their passes DIFFUSEPT2/3_S(Fg) are PassSums::Lamps). Needs DeferredRenderer
// (1 or 2) and GBufferV2 at startup (refused with one log line otherwise). Chosen inside the main list's G-buffer span only, while the renderer
// is live, the path tracer does not replace the picture (its prep reads COLOR3.a 3.5+ as the view model) and the draw is not blended (an
// actor fading: forward, DeferredRenderer::BeforeDraw / BlendedNow); everything else draws the forward skin as before.
//   - The records join DeferredRenderer's bookkeeping (Handles / Pairs / ForwardOf): the object is marked when its base pass drew deferred and
//     its added lamp passes are skipped; the blended-draw swap and the span-end swap back to forward work as for every deferred record.
//   - The G-buffer's targets: the records are noted as material / albedo writers (ShaderRecord.cpp), and the skin blur (SkinScattering) does not
//     bind its own targets for them (SkinScatteringEffect::BindForSkinDraw), so CapturedNormals binds the G-buffer's.
//   - SkinFill / SkinAlbedoFill (core/CapturedNormals.h) leave the deferred skin's pixels alone (their COLOR3.a code is 4 and up).
// (2026-11-15: superseded by [Main.Main.Quality] DeferredSkinSSS, below "the skin's scattering"; DeferredSkinBlur true counts as its 1)
// [Main.Main.Quality] DeferredSkinBlur (live, test only, off; needs DeferredSkin): the screen-space skin blur on the deferred skin. After the
// light passes the skin blur's two targets (SkinScattering: distance + highlights' share, albedo + the drawn luminance) are filled from the
// G-buffer for the deferred skin's pixels (FillBlurTargets), and the passes leave the curvature diffusion out (TESR_SkinDebug.w 1), as the
// forward skin does while the blur takes it; the blur itself is SkinScattering's (its settings, [Shaders.Skin.Scattering] / [Interiors]
// ScreenSpace on). v1 limit: the highlights' share is not known per pixel after the passes (written 0), so the blur diffuses the highlights
// with the light (forward keeps them sharp). It changes the look against forward: the owner's call; parity is measured without it.
// Off (DeferredSkin false): nothing is made, chosen or filled; every template and pass compiles as before.
#include "DeferredRenderer.h"
#include "DeferredSun.h"
#include "DeferredLamps.h"
#include "TemporalBackbone.h"	// (SSS v2: the disc's turn per frame, SharedNoise)
#include "DistantLandTerrain.h"	// UNOFFICIAL Phase 1 Distant Land Geo-Clipmap terrain

namespace DeferredSkin {
	inline const char* kSection = "Main.Main.Quality";
	inline const char* kCache = "Deferred\\Skin\\";
	inline int Wanted = -1;
	// at startup: the key, a renderer mode that has the passes, and the G-buffer v2 the record writes
	inline bool WantedAtStartup() {
		if (Wanted < 0) {
			const bool key = TheSettingManager->GetSettingI(kSection, "DeferredSkin") != 0;
			Wanted = key && DeferredRenderer::Mode() > 0 && CapturedNormals::V2CompiledIn();
			if (key && !Wanted) Logger::Log("[WARNING] UNOFFICIAL DeferredSkin: needs [Main.Main.Quality] DeferredRenderer 1 or 2 and GBufferV2 at startup: the skin stays forward");
		}
		return Wanted != 0;
	}

	inline std::unordered_map<const void*, ShaderRecordPixel*> Records;	// NiD3DPixelShaderEx* -> its deferred skin record
	inline std::unordered_set<IDirect3DPixelShader9*> Handles;
	inline unsigned Loaded = 0, Refused = 0;
	inline bool Live = false;
	inline UInt32 LiveFrame = 0;
	inline bool DrawnThisSpan = false;
	inline unsigned long long StatFrames = 0, StatBinds = 0, StatBlended = 0, StatFills = 0;

	inline void SssLoad();	// (below)
	inline bool IsHandle(IDirect3DPixelShader9* handle) { return handle && Handles.count(handle) != 0; }

	// The full, many-lamp and (2026-11-25 fd-complete batch 4) light-only AD skin pixel shaders (Skin.h SkinTemplates: not DIFFUSE, the added
	// lamp passes, which a deferred skin's object skips). [Main.Main.Quality] DeferredSkinAD (restart, default true) false: the AD family forward.
	inline bool Eligible(const char* name, const ShaderTemplate& tmpl) {
		if (!name || !tmpl.Name || strcmp(tmpl.Name, "SkinTemplate")) return false;
		const size_t length = strlen(name);
		if (length < 4 || strcmp(name + length - 4, ".pso")) return false;
		for (int d = 0; d < ShaderTemplate::kMaxDefines - 1 && tmpl.Defines[d].Name; d++)
			if (!strcmp(tmpl.Defines[d].Name, "DIFFUSE") ||
				(!strcmp(tmpl.Defines[d].Name, "ONLY_LIGHT") && !TheSettingManager->GetSettingI(kSection, "DeferredSkinAD"))) return false;
		return true;
	}

	// ShaderManager::LoadShader (pixel), after the shader's own records: the deferred skin record, compiled now (at load, never on the render
	// thread), from the default overrides + LAMPS_DEFERRED (2 in mode 2, 3 in mode 1: mode 1 is live indoors only, where the default record's
	// sun code is the interior one: GetSunShadowMaps returns 1 indoors by its own gate). Both light passes are made with it.
	inline void Load(NiD3DPixelShaderEx* shader, const char* name, ShaderTemplate tmpl) {
		if (!shader || !shader->ShaderProg[ShaderRecordType::Default] || !Eligible(name, tmpl) || !WantedAtStartup() || Records.count(shader)) return;
		static const D3DXMACRO mode2[2] = { { "LAMPS_DEFERRED", "2" }, { NULL, NULL } };
		static const D3DXMACRO mode1[2] = { { "LAMPS_DEFERRED", "3" }, { NULL, NULL } };
		ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(name, NULL, tmpl, kCache, DeferredRenderer::Mode() >= 2 ? mode2 : mode1);
		if (!record || !record->ShaderHandle) {
			Refused++;
			Logger::Log("[WARNING] UNOFFICIAL DeferredSkin record %s: did not load (see the device's line above): it stays forward; %u loaded, %u not so far", name, Loaded, Refused);
			return;
		}
		ShaderRecordPixel* forward = (ShaderRecordPixel*)(shader->ShaderProg[ShaderRecordType::Interior] ? shader->ShaderProg[ShaderRecordType::Interior] : shader->ShaderProg[ShaderRecordType::Default]);
		ShaderRecordPixel* outdoor = DeferredRenderer::OutdoorRecord(shader);
		Records[shader] = record;
		Handles.insert(record->ShaderHandle);
		CapturedNormals::SkinDeferredCodes = true;	// SkinFill / SkinAlbedoFill leave COLOR3.a 3.5+ alone (their shaders are made at their first use, later)
		// DeferredRenderer's bookkeeping (not its Records: DeferredRenderer::Choose stays the object families'): marking and pass skipping
		// (OnBind / AfterDraw / Skip), the blended-draw swap (Pairs), the span-end swap (ForwardOf). No SPECULAR: the game's highlight passes on
		// skin are kept as forward keeps them (muted by [Shaders.Skin.Main] MuteGameHighlights, the default)
		DeferredRenderer::Handles.insert(record->ShaderHandle);
		DeferredRenderer::PlainHandles.insert(record->ShaderHandle);
		DeferredRenderer::Pairs[record->ShaderHandle] = { record, forward && forward->ShaderHandle ? forward : nullptr, outdoor && outdoor->ShaderHandle ? outdoor : nullptr };
		if (forward && forward->ShaderHandle) DeferredLamps::ForwardOf[record->ShaderHandle] = forward->ShaderHandle;
		if (outdoor && outdoor->ShaderHandle) DeferredLamps::ForwardOfOutdoor[record->ShaderHandle] = outdoor->ShaderHandle;
		// a lighting-free record's binds open DeferredSun's pass for the span (mode 2; DeferredSun::OnBind counts this set)
		if (DeferredRenderer::Mode() >= 2) DeferredSun::FoliageHandles.insert(record->ShaderHandle);
		DeferredLamps::EnsurePass();
		DeferredLamps::EnsureShadowShader();
		SssLoad();	// (2026-11-15 SSS v2: its passes at load, never on the render thread)
		Loaded++;
		Logger::Log("UNOFFICIAL DeferredSkin record %s: loaded (LAMPS_DEFERRED %s; %u loaded, %u not so far)", name, DeferredRenderer::Mode() >= 2 ? "2" : "3", Loaded, Refused);
	}

	// Once a frame, lazily (the first Choose of a frame): live with the renderer, not under the path tracer.
	inline void Refresh() {
		if (LiveFrame == DeferredRenderer::FrameId) return;
		LiveFrame = DeferredRenderer::FrameId;
		// (+ the skin lamp pass loaded: the lamp pass compiled with FD_SKIN leaves the deferred skin's pixels to it)
		// (2026-11-16 fd-fwdgroups) + [Main.Main.Quality] DeferredSkinLive (live, test only, default true): the one-launch A/B for the bench, the
		// skin forward vs deferred with everything else deferred in both halves
		const bool live = DeferredRenderer::Live && !Records.empty() && WantedAtStartup() && DeferredLamps::SkinPass && !RtPathTrace::Replacing() &&
			TheSettingManager->GetSettingI(kSection, "DeferredSkinLive") != 0;
		if (live != Live) Logger::Log("UNOFFICIAL DeferredSkin: %s (%u records)", live ? "live: the full and many-lamp skin draw their deferred records; DeferredSun / DeferredLamps light them" :
			"not live (the renderer forward this frame, or the path tracer replaces the picture): the skin draws forward", Loaded);
		Live = live;
		if (!GpuTimer::Enabled) { StatFrames = 0; return; }
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		Logger::Log("DEFERRED SKIN %u frames: deferred skin binds %.1f a frame, blended skin draws left forward %.2f, blur targets filled %.2f", (unsigned)StatFrames, StatBinds / f, StatBlended / f, StatFills / f);
		StatFrames = StatBinds = StatBlended = StatFills = 0;
	}

	// NiD3DPixelShaderEx::SetupShader: the deferred skin record inside the G-buffer span (mode 1 indoors only, as the renderer's own records).
	inline ShaderRecordPixel* Choose(const void* shader, bool exterior) {
		if (Records.empty()) return nullptr;
		auto found = Records.find(shader);
		if (found == Records.end()) return nullptr;
		Refresh();
		if (!Live || !CapturedNormals::InSpan || (CapturedNormals::VmSpan && !CapturedNormals::VmDeferred)) return nullptr;
		if (exterior && (DeferredRenderer::Mode() < 2 || !DeferredRenderer::SunRuns())) return nullptr;
		if (DeferredRenderer::BlendedNow) { StatBlended++; return nullptr; }
		StatBinds++;
		DrawnThisSpan = true;
		return found->second;
	}

	// --- the skin's scattering on the deferred skin (Render.cpp's DeferredSpan: BeforePasses ahead of the light passes, AfterPasses after them)
	// CREDIT Conor (mrfury97): the screen-space skin scattering is his (src/effects/SkinScattering.h), with Community Shaders' kernel generator
	// (GPL-3.0-or-later) and Jimenez & Gutierrez's separable SSS; v2's profile is Christensen & Burley's normalized diffusion, from the paper.
	// [Main.Main.Quality] DeferredSkinSSS (live, test only, off): 0 off; 1 v1 (the separable blur, its targets filled from the G-buffer, the
	// highlights blurred with the light); 2 v2: the light passes write the skin's highlights to their own target (DeferredLamps::SkinSpecSurface),
	// so only diffuse light spreads, and every lamp the deferred passes add is diffused. [Shaders.Skin.SSS] Kernel 0: the Burley profile on a
	// 24-tap disc in linear light right after the passes (Shaders\DeferredSkinSSS.hlsl); 1: the separable blur fed with the highlights' share.
	// DeferredSkinBlur true (the first switch) counts as 1.
	inline int SssMode() {
		if (!WantedAtStartup()) return 0;
		int m = TheSettingManager->GetSettingI(kSection, "DeferredSkinSSS");
		if (m <= 0 && TheSettingManager->GetSettingI(kSection, "DeferredSkinBlur") != 0) m = 1;
		return m < 0 ? 0 : m > 2 ? 2 : m;
	}
	inline bool BurleyKernel() { return TheSettingManager->GetSettingI("Shaders.Skin.SSS", "Kernel") == 0; }
	inline bool BlurWanted() { const int m = SssMode(); return m == 1 || (m == 2 && !BurleyKernel()); }	// the separable blur's fill
	inline bool BurleyWanted() { return SssMode() == 2 && BurleyKernel(); }
	inline IDirect3DPixelShader9* FillScatterPS = nullptr;  inline IDirect3DPixelShader9* FillAlbedoPS = nullptr;  inline IDirect3DPixelShader9* FillSharePS = nullptr;
	inline bool FillFailed = false;
	inline IDirect3DTexture9* FrameCopyTex = nullptr;  inline IDirect3DSurface9* FrameCopy = nullptr;  inline D3DSURFACE_DESC FrameCopyDesc = {};
	inline IDirect3DTexture9* SpecTex = nullptr;  inline IDirect3DSurface9* Spec = nullptr;  inline D3DSURFACE_DESC SpecDesc = {};	// the highlight target
	inline IDirect3DTexture9* LightTex = nullptr;  inline IDirect3DSurface9* LightSurf = nullptr;  inline D3DSURFACE_DESC LightDesc = {};	// v2's SSS_PREP target
	inline ShaderRecordPixel* SssPrep = nullptr;  inline ShaderRecordPixel* SssBlur = nullptr;  inline bool SssFailed = false, SssTried = false;
	inline bool SpecThisFrame = false;
	inline unsigned long long StatSss = 0;

	// A render target texture of the frame's size and format (made again when either changes).
	inline bool EnsureTarget(IDirect3DDevice9* d, const D3DSURFACE_DESC& t, D3DFORMAT format, IDirect3DTexture9*& tex, IDirect3DSurface9*& surf, D3DSURFACE_DESC& made) {
		if (surf && made.Width == t.Width && made.Height == t.Height && made.Format == format) return true;
		if (surf) { surf->Release(); surf = nullptr; }
		if (tex) { tex->Release(); tex = nullptr; }
		if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &tex, NULL)) || FAILED(tex->GetSurfaceLevel(0, &surf))) {
			if (tex) { tex->Release(); tex = nullptr; }
			surf = nullptr;
			return false;
		}
		made = t; made.Format = format;
		return true;
	}
	inline bool CopyFrame(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);
		if (!EnsureTarget(d, t, t.Format, FrameCopyTex, FrameCopy, FrameCopyDesc)) return false;
		return SUCCEEDED(d->StretchRect(target, NULL, FrameCopy, NULL, D3DTEXF_NONE));
	}
	// Clear one target alone (D3D9's Clear clears every bound target), everything it touches put back.
	inline void ClearAlone(IDirect3DDevice9* d, IDirect3DSurface9* surf) {
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		D3DVIEWPORT9 vp; d->GetViewport(&vp);
		DWORD scissor = FALSE, mask = 0xF;
		d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor); d->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
		d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
		d->SetRenderTarget(0, surf);
		d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
		d->SetRenderTarget(0, rt[0]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, rt[i]);
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		d->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor); d->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
		d->SetViewport(&vp);
	}

	// One full-screen draw of ps into target with up to 6 textures on stages 0-5 (point, clamp), blend off; every state it changes put back.
	inline void Quad(IDirect3DDevice9* d, IDirect3DSurface9* target, IDirect3DPixelShader9* psNew, IDirect3DBaseTexture9* const* textures, DWORD count) {
		D3DSURFACE_DESC t = {};
		if (!target || !psNew || FAILED(target->GetDesc(&t))) return;
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
		d->SetRenderTarget(0, target);
		const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_FOGENABLE };
		const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0xF, FALSE, FALSE, FALSE };
		DWORD saved[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &saved[i]))) saved[i] = want[i]; if (saved[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[6][sizeof(kSS) / sizeof(kSS[0])];
		IDirect3DBaseTexture9* savedT[6] = {};
		if (count > 6) count = 6;
		for (DWORD s = 0; s < count; s++) {
			d->GetTexture(s, &savedT[s]);
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], wantS[i]); }
			d->SetTexture(s, textures[s]);
		}
		d->SetVertexShader(nullptr);
		d->SetPixelShader(psNew);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x1 = (float)t.Width - 0.5f, y1 = (float)t.Height - 0.5f;
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		for (DWORD s = 0; s < count; s++) {
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], savedS[s][i]);
			d->SetTexture(s, savedT[s]); if (savedT[s]) savedT[s]->Release();
		}
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (saved[i] != want[i]) d->SetRenderState(kRS[i], saved[i]);
		d->SetRenderTarget(0, rt[0]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, rt[i]);
		for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		d->SetViewport(&vp);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
	}

	// Render.cpp's DeferredSpan, before DeferredSun::Apply: v2's highlight target, cleared, for the passes to add to (null otherwise).
	inline void BeforePasses(IDirect3DDevice9* d) {
		DeferredLamps::SkinSpecSurface = nullptr;
		SpecThisFrame = false;
		if (!DrawnThisSpan || SssMode() != 2) return;
		IDirect3DSurface9* target = nullptr;
		d->GetRenderTarget(0, &target);
		if (!target) return;
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);
		target->Release();
		if (t.MultiSampleType != D3DMULTISAMPLE_NONE) return;	// (the renderer refuses the game's MSAA anyway)
		if (!EnsureTarget(d, t, D3DFMT_A16B16G16R16F, SpecTex, Spec, SpecDesc)) { static bool told = false; if (!told) { told = true; Logger::Log("[WARNING] UNOFFICIAL DeferredSkinSSS: could not make the skin's highlight target: v2 off"); } return; }
		ClearAlone(d, Spec);
		DeferredLamps::SkinSpecSurface = Spec;
		SpecThisFrame = true;
	}

	// v2's taps (Shaders\DeferredSkinSSS.hlsl SSS_Taps): radii in units of d_red drawn from red's Burley distribution at u = (i + 0.5) / N,
	// CDF(x) = 1 - e^(-x) / 4 - 3 e^(-x / 3) / 4 inverted by bisection, each weighted per colour by R_c(r) / R_red(r) (the shader normalises
	// the weights by their sum). d: the three scatter distances (any unit, red first).
	static const int kSssTaps = 24;
	inline void SssTaps(const float d[3], float out[kSssTaps][4]) {
		for (int i = 0; i < kSssTaps; i++) {
			const double u = (i + 0.5) / kSssTaps;
			double lo = 0.0, hi = 64.0;
			for (int k = 0; k < 60; k++) {
				const double x = 0.5 * (lo + hi);
				const double cdf = 1.0 - 0.25 * exp(-x) - 0.75 * exp(-x / 3.0);
				if (cdf < u) lo = x; else hi = x;
			}
			const double x = 0.5 * (lo + hi), r = x * d[0];
			const double red = (exp(-r / d[0]) + exp(-r / (3.0 * d[0]))) / d[0];
			out[i][0] = (float)x;
			for (int c = 0; c < 3; c++) {
				const double dc = d[c] > 1.0e-4 ? d[c] : 1.0e-4;
				out[i][1 + c] = (float)(((exp(-r / dc) + exp(-r / (3.0 * dc))) / dc) / red);
			}
		}
	}
	inline void SssLoad() {
		if (SssTried) return;
		SssTried = true;
		ShaderTemplate tmpl = {};
		tmpl.Name = "DeferredSkinSSS";
		static const D3DXMACRO prep[2] = { { "SSS_PREP", "1" }, { NULL, NULL } };
		SssPrep = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredSkinSSSPrep.pso", NULL, tmpl, "Deferred\\Skin\\SSS\\", prep);
		SssBlur = SssPrep ? (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredSkinSSS.pso", NULL, tmpl, "Deferred\\Skin\\SSS\\", nullptr) : nullptr;
		SssFailed = !SssPrep || !SssBlur;
		Logger::Log(SssFailed ? "[ERROR] UNOFFICIAL DeferredSkinSSS: the v2 passes (Shaders\\DeferredSkinSSS.hlsl) did not load: v2 with Kernel 0 does nothing" :
			"UNOFFICIAL DeferredSkinSSS: the v2 passes loaded (Shaders\\DeferredSkinSSS.hlsl: the Burley profile, 24 taps)");
	}

	// v2, Kernel 0: the Burley disc over the deferred skin (and optionally non-skin SSS solids 11..14), in linear light, into the frame.
	inline void BurleyApply(IDirect3DDevice9* d, bool includeSolids = false) {
		if (SssFailed || (!SpecThisFrame && !includeSolids) || !LinearScene::Linear || !CapturedNormals::Texture || !CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture) return;
		if (!SssPrep || !SssBlur) return;
		IDirect3DSurface9* target = nullptr;
		d->GetRenderTarget(0, &target);
		if (!target) return;
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);
		if (!CopyFrame(d, target) || !EnsureTarget(d, t, D3DFMT_A16B16G16R16F, LightTex, LightSurf, LightDesc)) { target->Release(); return; }
		static GpuTimer timer("Deferred skin SSS");
		GpuProfileScope gpu(timer, d);
		const bool linearZ = DeferredDepth::EnsureLinear(d);
		float savedC[3 + kSssTaps][4];	// (+ c26 SSS_Debug, 2026-11-16)
		d->GetPixelShaderConstantF(0, &savedC[0][0], 3 + kSssTaps);
		// the constants: scatter distances in world units (1 unit = 1/70 m: 14.29 mm), the pixels a world unit covers at view z 1, the angle
		const float radius = (std::max)(0.1f, TheSettingManager->GetSettingF("Shaders.Skin.SSS", "Radius"));
		float dmm[3] = { TheSettingManager->GetSettingF("Shaders.Skin.SSS", "ScatterRed"), TheSettingManager->GetSettingF("Shaders.Skin.SSS", "ScatterGreen"), TheSettingManager->GetSettingF("Shaders.Skin.SSS", "ScatterBlue") };
		for (float& v : dmm) v = (std::max)(0.05f, v) * radius / 14.2857f;
		float c[3 + kSssTaps][4] = {};
		c[2 + kSssTaps][0] = (float)TheSettingManager->GetSettingI("Shaders.Skin.SSS", "DebugView");	// (2026-11-16) 1 the prep's light, 2 covered skin red
		c[2 + kSssTaps][1] = includeSolids ? 1.0f : 0.0f;
		SssTaps(dmm, &c[2]);
		const float tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const bool turn = TemporalBackbone::Shared() ? TemporalBackbone::TemporalResolve() : false;
		const float angle = turn ? 6.2831853f * fmodf(0.61803399f * (float)((TemporalBackbone::NoiseIndex() + TemporalBackbone::OffsetSkin) % 1024), 1.0f) : 0.0f;
		c[0][0] = 1.0f / t.Width; c[0][1] = 1.0f / t.Height; c[0][2] = 0.5f * t.Height / (std::max)(tanY, 1.0e-3f); c[0][3] = angle;
		c[1][0] = dmm[0];
		c[1][1] = (std::max)(0.1f, TheSettingManager->GetSettingF("Shaders.Skin.SSS", "DepthFollow"));
		c[1][2] = std::clamp(TheSettingManager->GetSettingF("Shaders.Skin.SSS", "AlbedoDetail"), 0.0f, 1.0f);
		c[1][3] = linearZ ? 1.0f : 0.0f;
		d->SetPixelShaderConstantF(0, &c[0][0], 3 + kSssTaps);
		ClearAlone(d, LightSurf);
		IDirect3DBaseTexture9* specOrDummy = (SpecThisFrame && SpecTex) ? SpecTex : FrameCopyTex;
		IDirect3DBaseTexture9* prepTex[6] = { FrameCopyTex, CapturedNormals::AlbedoTexture, specOrDummy, CapturedNormals::Texture, CapturedNormals::MaterialTexture, linearZ ? DeferredDepth::LinearTexture : FrameCopyTex };
		Quad(d, LightSurf, SssPrep->ShaderHandle, prepTex, 6);
		IDirect3DBaseTexture9* blurTex[5] = { LightTex, FrameCopyTex, CapturedNormals::AlbedoTexture, specOrDummy, CapturedNormals::MaterialTexture };
		Quad(d, target, SssBlur->ShaderHandle, blurTex, 5);
		d->SetPixelShaderConstantF(0, &savedC[0][0], 3 + kSssTaps);
		target->Release();
		StatSss++;
	}

	// The blur's target 1 (rgb the highlights' share, a the camera distance) and target 2 (rgb the albedo as the texture holds it, a the
	// luminance of the pixel as the effect chain will find it: the frame's stored curve) for the deferred skin's pixels (COLOR3.a 3.5 and up).
	// blurSurface / albedoSurface: the effect's draw surfaces; frameLinear: the frame holds linear light now (LinearScene), gamma: its decode.
	// The share: 0 in v1; with v2's highlight target, enc(frame) - enc(frame - highlights x keep), as the forward skin record writes it.
	// True = filled (the blur then has skin to take this frame).
	inline bool FillBlurTargets(IDirect3DDevice9* d, IDirect3DSurface9* blurSurface, IDirect3DSurface9* albedoSurface, float gamma, bool frameLinear) {
		if (!DrawnThisSpan || FillFailed || !blurSurface || !albedoSurface || !CapturedNormals::Texture || !CapturedNormals::AlbedoTexture) return false;
		if (!FillScatterPS) {
			static const char* kScatter =
				"sampler2D N : register(s0); sampler2D A : register(s1);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float4 n = tex2D(N, uv); clip(min(n.a - 3.5, tex2D(A, uv).a - 0.5));"
				"  return float4(0.0, 0.0, 0.0, n.b); }";
			// (2026-11-15 SSS v2) the share from the highlight target: N normals, A albedo (its a: the keep, Includes/DeferredPixel.hlsl
			// FDAddedLightKeep under GBufferV2), F the frame's copy, S the highlights; G as below
			static const char* kShare =
				"sampler2D N : register(s0); sampler2D A : register(s1); sampler2D F : register(s2); sampler2D S : register(s3); float4 G : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float4 n = tex2D(N, uv); float a = tex2D(A, uv).a; clip(min(n.a - 3.5, a - 0.5));"
				"  float keep = saturate(a > 4.5 ? a - 5.0 : a > 2.5 ? a - 3.0 : a > 1.0001 ? a - 1.0 : a);"
				"  float3 f = max(tex2D(F, uv).rgb, 0.0); float3 s = max(tex2D(S, uv).rgb, 0.0) * keep;"
				"  float e = G.x > 1.0 ? 1.0 / G.x : 1.0;"
				"  float3 fLin = G.y > 0.5 ? f : (G.x > 1.0 ? pow(f, G.x) : f);"
				"  float3 dLin = max(fLin - s, 0.0);"
				"  float3 share = (G.y > 0.5 || G.x > 1.0) ? (G.y > 0.5 ? pow(fLin, e) : f) - pow(dLin, e) : s;"
				"  return float4(max(share, 0.0), n.b); }";
			// G: x the linear lighting's gamma (1 = none), y 1 = the frame is linear now. Written as the forward skin record writes them
			// (SkinTemplate.hlsl COLOR2): the albedo as the texture holds it (saturate(texColor): the G-buffer's LinColor undone), and the luminance
			// of the pixel in the stored curve (what the effect chain finds; its Helpers.hlsl luma weights)
			static const char* kAlbedo =
				"sampler2D N : register(s0); sampler2D A : register(s1); sampler2D F : register(s2); float4 G : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float4 a4 = tex2D(A, uv); clip(min(tex2D(N, uv).a - 3.5, a4.a - 0.5));"
				"  float3 a = max(a4.rgb, 0.0); float3 f = max(tex2D(F, uv).rgb, 0.0);"
				"  if (G.x > 1.0) a = pow(a, 1.0 / G.x);"
				"  if (G.y > 0.5 && G.x > 1.0) f = pow(f, 1.0 / G.x);"
				"  return float4(saturate(a), dot(f, float3(0.2126, 0.7152, 0.0722))); }";
			FillScatterPS = CapturedNormals::FullScreenShader(d, kScatter, "DeferredSkinBlur distance fill", FillFailed);
			FillAlbedoPS = FillScatterPS ? CapturedNormals::FullScreenShader(d, kAlbedo, "DeferredSkinBlur albedo fill", FillFailed) : nullptr;
			FillSharePS = FillAlbedoPS ? CapturedNormals::FullScreenShader(d, kShare, "DeferredSkinSSS highlight share fill", FillFailed) : nullptr;
			if (!FillScatterPS || !FillAlbedoPS || !FillSharePS) { FillFailed = true; return false; }
		}
		IDirect3DSurface9* target = nullptr;
		d->GetRenderTarget(0, &target);
		if (!target) return false;
		if (!CopyFrame(d, target)) {
			static bool told = false;
			if (!told) { told = true; Logger::Log("[WARNING] UNOFFICIAL DeferredSkinBlur: could not copy the frame: the deferred skin is not blurred"); }
			target->Release(); return false;
		}
		target->Release();
		static GpuTimer timer("Deferred skin blur fill");
		GpuProfileScope gpu(timer, d);
		float saved[4] = {};
		d->GetPixelShaderConstantF(0, saved, 1);
		const float g[4] = { gamma, frameLinear ? 1.0f : 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(0, g, 1);
		if (SpecThisFrame && SpecTex) {	// v2's highlights kept sharp
			IDirect3DBaseTexture9* share[4] = { CapturedNormals::Texture, CapturedNormals::AlbedoTexture, FrameCopyTex, SpecTex };
			Quad(d, blurSurface, FillSharePS, share, 4);
		}
		else CapturedNormals::FullScreenPass(d, blurSurface, FillScatterPS, CapturedNormals::Texture, false, D3DBLEND_ONE, D3DBLEND_ZERO, CapturedNormals::AlbedoTexture);
		CapturedNormals::FullScreenPass(d, albedoSurface, FillAlbedoPS, CapturedNormals::Texture, false, D3DBLEND_ONE, D3DBLEND_ZERO, CapturedNormals::AlbedoTexture, FrameCopyTex);
		d->SetPixelShaderConstantF(0, saved, 1);
		StatFills++;
		return true;
	}

	// Render.cpp's DeferredSpan, after the light passes: v2's Burley disc, or the separable blur's fill (SkinScattering.cpp, where the
	// effect's targets live); then the span is over (DrawnThisSpan cleared, the highlight target taken off the passes).
	void AfterPasses(IDirect3DDevice9* d);
}
