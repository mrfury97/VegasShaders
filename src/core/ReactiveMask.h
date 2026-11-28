#pragma once

// UNOFFICIAL 2026-10-30 ReactiveMask ([Shaders.TAA.FSR] ReactiveMask, 0 = off; docs/next/ROOT-ASSUMPTIONS-PLAN.md D1: the reactive mask).
// FSR keeps a pixel's history unless its reactive mask says "this changed by itself": NVR's mask held only the first-person weapon, so
// smoke, fire, sparks, glass, decals and every other alpha-blended thing drawn over the world were accumulated like solid walls (they
// trail and smear as the camera moves). AMD's own recipe (ffxFsr2ContextGenerateReactiveMask): the scene before the alpha draws against
// the scene after them; where they differ, the pixel is reactive.
//   - GrassFirst::MainListHook copies the world's colour when the game's main opaque list (RenderBatches(1, 0x25B)) ends, after
//     LinearScene has encoded it back (NoteOpaque);
//   - RenderWorldSceneGraphHook, when the game's world scene ends and its depth is resolved into TESR_DepthBufferWorld (no NVR effect
//     yet; read before that resolve, the depth texture gave 0 everywhere, run 20261030-190111-slotA), copies the colour again and draws mask = largest
//     channel change of c / (1 + c) x ReactiveMask, 0 below ReactiveThreshold, at most ReactiveMax (Build);
//   - the FSR prep shaders keep it only nearer than ReactiveMaxDistance (their own depth): the sky and the distant land are drawn after
//     the main list too (run 20261030-182413-slotA marked them), and no alpha draw writes depth. (A D3D9 pass reads the INTZ world depth
//     as 0 under NVR DXVK, run 20261030-190349-slotA; CombineDepth reads it through the extension too.) (The blended stage alone, 0xB650C0, holds almost none of it:
//     the windows' glow cards and the neon are drawn outside it, run 20261030-183310-slotA, 0.00 %.)
//   - FSRPrep.comp / FSRUpPrep.comp (FSR 2, FSR 3.1, RenderScale v1 and v2) take max(weapon, mask) as the reactive value.
// Off: nothing is copied or drawn and the readers keep the weapon-only mask, as before.
// ReactiveDebug (diagnostic): the mask drawn over the world (before the distance cut). With the F10 profile on: a REACTIVE MASK line every 240 frames (the share of pixels above 0.1, from an occlusion query).

#include "GpuProfiler.h"

namespace ReactiveMask {

	inline IDirect3DTexture9* Opaque = nullptr;		// the colour as the main opaque list left it
	inline IDirect3DSurface9* OpaqueSurface = nullptr;
	inline IDirect3DTexture9* Final = nullptr;		// the colour as the world scene left it
	inline IDirect3DSurface9* FinalSurface = nullptr;
	inline IDirect3DTexture9* Texture = nullptr;	// the mask (r), read by the FSR prep shaders
	inline IDirect3DSurface9* Surface = nullptr;
	inline IDirect3DPixelShader9* PS = nullptr;
	inline IDirect3DPixelShader9* CountPS = nullptr;
	inline IDirect3DQuery9* Query = nullptr;
	inline UINT Width = 0, Height = 0;
	inline D3DFORMAT Format = D3DFMT_UNKNOWN;
	inline UInt32 OpaqueFrame = 0;		// FrameNumber the opaque copy was made in
	// UNOFFICIAL 2026-11-10 (core/RtBlendedLayer.h): another reader of the two copies. WantCopies: make Opaque and Final also with ReactiveMask 0
	// (the mask itself is then not drawn); CopiesFrame: the frame both copies were last made in.
	inline bool WantCopies = false;
	inline UInt32 CopiesFrame = 0;
	inline UInt32 ReadyFrame = 0;		// FrameNumber the mask was made in
	inline bool Failed = false;
	inline UInt32 StatFrames = 0, StatMade = 0, StatCounted = 0;
	inline double StatShare = 0.0;

	inline float MaxDistance() { return (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.TAA.FSR", "ReactiveMaxDistance")); }
	inline float Scale() { return (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.TAA.FSR", "ReactiveMask")); }
	inline UInt32 FrameNumber() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }
	// This frame's mask is made (the readers' switch).
	inline bool Ready() { return !Failed && Texture && ReadyFrame == FrameNumber() && Scale() > 0.0f; }

	inline void Release() {
		for (IUnknown** p : { (IUnknown**)&OpaqueSurface, (IUnknown**)&Opaque, (IUnknown**)&FinalSurface, (IUnknown**)&Final, (IUnknown**)&Surface, (IUnknown**)&Texture })
			if (*p) { (*p)->Release(); *p = nullptr; }
		Width = Height = 0;
	}

	inline bool Ensure(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (Texture && Width == t.Width && Height == t.Height && Format == t.Format) return true;
		Release();
		auto make = [&](IDirect3DTexture9*& tex, IDirect3DSurface9*& surf, D3DFORMAT f) {
			return SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &tex, NULL)) && SUCCEEDED(tex->GetSurfaceLevel(0, &surf));
		};
		if (!make(Opaque, OpaqueSurface, t.Format) || !make(Final, FinalSurface, t.Format) || !make(Texture, Surface, D3DFMT_A8R8G8B8)) {
			Logger::Log("[ERROR] ReactiveMask: could not make its %ux%u targets (format %d): off for the session", t.Width, t.Height, (int)t.Format);
			Release();
			Failed = true;
			return false;
		}
		Width = t.Width; Height = t.Height; Format = t.Format;
		Logger::Log("ReactiveMask: %ux%u targets made (scene format %d)", Width, Height, (int)Format);
		return true;
	}

	inline bool Compile(IDirect3DDevice9* d) {
		if (PS) return true;
		// s0 the opaque copy, s1 the final copy; c0 x ReactiveMask, y ReactiveMax, z ReactiveThreshold.
		static const char* kPS =
			"sampler2D O : register(s0); sampler2D C : register(s1); float4 K : register(c0);"
			"float reactive(float2 uv) {"
			"  float3 o = max(tex2D(O, uv).rgb, 0.0f); float3 c = max(tex2D(C, uv).rgb, 0.0f);"
			"  float3 e = abs(c / (1.0f + c) - o / (1.0f + o));"
			"  float r = max(max(e.r, e.g), e.b) * K.x;"
			"  return r < K.z ? 0.0f : min(r, K.y); }"
			"float4 main(float2 uv : TEXCOORD0) : COLOR0 { float r = reactive(uv); return float4(r, r, r, 1.0f); }"
			"float4 count(float2 uv : TEXCOORD0) : COLOR0 { clip(reactive(uv) - 0.1f); return 0.0f; }";
		auto build = [&](const char* entry, IDirect3DPixelShader9*& out) {
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			const bool ok = SUCCEEDED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, entry, "ps_2_0", 0, &code, &errors, NULL)) && code &&
				SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &out));
			if (!ok) Logger::Log("[ERROR] ReactiveMask: the shader '%s' failed (%s): off for the session", entry, errors ? (const char*)errors->GetBufferPointer() : "?");
			if (code) code->Release();
			if (errors) errors->Release();
			return ok;
		};
		if (!build("main", PS) || !build("count", CountPS)) { Failed = true; return false; }
		return true;
	}

	// The main view's target: the screen's size (reflections, menus and other views draw the list into smaller targets).
	inline IDirect3DSurface9* MainTarget(IDirect3DDevice9* d, D3DSURFACE_DESC& t) {
		IDirect3DSurface9* rt0 = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return nullptr;
		rt0->GetDesc(&t);
		if (t.Width != TheRenderManager->width || t.Height != TheRenderManager->height) { rt0->Release(); return nullptr; }
		return rt0;
	}

	// GrassFirst::MainListHook, after the game's main list (and LinearScene's re-encode), world context only. True = copied.
	inline bool NoteOpaque(IDirect3DDevice9* d) {
		if (Failed || !(Scale() > 0.0f || WantCopies) || OpaqueFrame == FrameNumber()) return false;
		D3DSURFACE_DESC t = {};
		IDirect3DSurface9* rt0 = MainTarget(d, t);
		if (!rt0) return false;
		const bool ok = Ensure(d, t) && SUCCEEDED(d->StretchRect(rt0, NULL, OpaqueSurface, NULL, D3DTEXF_NONE));
		if (ok) OpaqueFrame = FrameNumber();
		rt0->Release();
		return ok;
	}

	// RenderWorldSceneGraphHook, after the world scene and its depth resolve (before any NVR effect, the weapon and a v2 upscale).
	inline void Build(IDirect3DDevice9* d) {
		const float scale = Scale();
		if (!Failed && !(scale > 0.0f) && WantCopies && OpaqueFrame == FrameNumber() && CopiesFrame != FrameNumber()) {	// (2026-11-10) the copies alone
			D3DSURFACE_DESC ct = {};
			if (IDirect3DSurface9* crt = MainTarget(d, ct)) {
				if (Ensure(d, ct) && SUCCEEDED(d->StretchRect(crt, NULL, FinalSurface, NULL, D3DTEXF_NONE))) CopiesFrame = FrameNumber();
				crt->Release();
			}
			return;
		}
		if (Failed || !(scale > 0.0f) || OpaqueFrame != FrameNumber() || ReadyFrame == FrameNumber()) return;
		D3DSURFACE_DESC t = {};
		IDirect3DSurface9* rt0 = MainTarget(d, t);
		if (!rt0) return;
		if (!Ensure(d, t) || !Compile(d) || FAILED(d->StretchRect(rt0, NULL, FinalSurface, NULL, D3DTEXF_NONE))) { rt0->Release(); return; }
		CopiesFrame = FrameNumber();
		const bool count = GpuTimer::Enabled && (Query || SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &Query)));

		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DSurface9* zs = nullptr; IDirect3DBaseTexture9* tex[3] = {}; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetDepthStencilSurface(&zs); d->GetTexture(0, &tex[0]); d->GetTexture(1, &tex[1]); d->GetTexture(2, &tex[2]); d->GetViewport(&vp);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[3][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD s = 0; s < 3; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], wantS[i]); }
		float savedPS[8] = {};
		d->GetPixelShaderConstantF(0, savedPS, 2);

		d->SetRenderTarget(0, Surface);
		d->SetDepthStencilSurface(NULL);
		const D3DVIEWPORT9 full = { 0, 0, Width, Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(PS, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, Opaque);
		d->SetTexture(1, Final);
		const float k[4] = { scale, std::clamp(TheSettingManager->GetSettingF("Shaders.TAA.FSR", "ReactiveMax"), 0.0f, 1.0f),
			(std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.TAA.FSR", "ReactiveThreshold")), 0.0f };
		d->SetPixelShaderConstantF(0, k, 1);
		struct V { float x, y, z, w, u, v; };
		const float w = (float)Width, h = (float)Height;
		const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
		ReadyFrame = FrameNumber();
		if (count) {	// the share of pixels above 0.1: drawn again with nothing written, counted
			if (StatFrames + 1 >= 240) {	// the last frame of the period: the last period's answer read, a new one issued
				DWORD pixels = 0;
				if (StatCounted && Query->GetData(&pixels, sizeof(pixels), 0) == S_OK) StatShare = 100.0 * pixels / ((double)Width * Height);
				StatCounted = 1;
				d->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
				state->SetPixelShader(CountPS, false);
				Query->Issue(D3DISSUE_BEGIN);
				d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
				Query->Issue(D3DISSUE_END);
				d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			}
		}

		for (DWORD s = 0; s < 3; s++) for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, savedPS, 2);
		// [Shaders.TAA.FSR] ReactiveDebug (diagnostic): the mask shown in place of the world (white = reactive), to see what it marks
		if (TheSettingManager->GetSettingI("Shaders.TAA.FSR", "ReactiveDebug") != 0) d->StretchRect(Surface, NULL, rt0, NULL, D3DTEXF_POINT);
		d->SetRenderTarget(0, rt0); rt0->Release();
		d->SetDepthStencilSurface(zs); if (zs) zs->Release();
		d->SetViewport(&vp);
		d->SetTexture(0, tex[0]); if (tex[0]) tex[0]->Release();
		d->SetTexture(1, tex[1]); if (tex[1]) tex[1]->Release();
		d->SetTexture(2, tex[2]); if (tex[2]) tex[2]->Release();
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();

		if (GpuTimer::Enabled) {
			StatMade++;
			if (++StatFrames >= 240) {
				Logger::Log("REACTIVE MASK ReactiveMask %.2f: made %u of %u frames; %.2f%% of the screen above 0.1 (the previous period's last frame)",
					scale, StatMade, StatFrames, StatShare);
				StatFrames = StatMade = 0;
			}
		}
	}
}
