#pragma once

// UNOFFICIAL 2026-10-28 LinearScene ([Shaders.PBR.Linear] LinearScene; docs/next/LINEAR-SCENE-PLAN.md; census: LinearSceneCensus.h).
// User: "seems like this bug keeps popping up over and over how can we solve this once and for all across all areas".
//
// The game draws some surfaces in several passes and blends them in the frame (extra lamps and highlights ADDED, the texture pass
// MULTIPLYING). With linear lighting each NVR pass encoded its own result first, so the frame summed enc(a) + enc(b) instead of
// enc(a + b): pieces lit in several passes came out lighter than their neighbours (Lucky 38 tiles, Novac rubble, Freeside roads).
// LampSums, HighlightSums, ExactPassSums and the run cap patched one route each.
//
// Here the world's main opaque list (RenderBatches(1, 0x25B), GrassFirst::MainListHook) keeps the frame in ONE space at a time:
//   - Linear: NVR's lit pixel shaders (LinearSceneShaders.h) drawn opaque or additive give their finished colour decoded
//     (LinearSceneOut, TESR_LinearLighting.w = 1 for that draw), so the game's own blends add in linear light;
//   - Stored: everything else (the game's texture passes, unlit signs, eyes, sky, water, alpha blends, any other shader) as before.
// When a draw needs the other space the whole frame is converted once (full screen: decode x^g or encode x^(1/g)); after the list it
// is left encoded. The census (Freeside, Lucky 38) found the main list in runs by pass ID: objects, their texture passes, highlights,
// land, then sky / water / alpha, so a frame takes a few conversions. Single-pass surfaces come out as before (decoded, then encoded
// again); several-pass sums become exact; FoldHighlights stays exact (the stored curve is a power: dec(a / t) = dec(a) / dec(t)) and the
// game's texture multiply runs in the stored curve as before. LampSums / HighlightSums / ExactPassSums are off while this is on.
// Off (default; compiled in only when on at startup, ShaderRecord LINEAR_SCENE_ON): nothing hooked, the shaders as before.
#include <d3dx9.h>
#include "LinearSceneShaders.h"
#include "DrawState.h"	// UNOFFICIAL 2026-10-29 SharedDrawState: CapturedNormals' tracked device state

namespace LinearScene {

	inline bool Checked = false, Compiled = false;	// on at startup with linear lighting (the shaders have LinearSceneOut)
	inline bool Live = false;					// this frame: compiled, on, linear lighting's gamma above 1
	inline bool InSpan = false;					// inside the world's main list
	inline bool Linear = false;					// the frame is in linear light now (only ever inside the span)
	inline IDirect3DSurface9* SpanTarget = nullptr;	// the scene target the span began with (draws into other targets are left alone)
	inline float Gamma = 1.0f;
	inline bool Hooked = false, Failed = false;
	inline IDirect3DTexture9* CopyTex = nullptr;  inline IDirect3DSurface9* Copy = nullptr;
	inline D3DSURFACE_DESC Made = {};
	inline IDirect3DPixelShader9* ConvertPS = nullptr;
	inline IDirect3DPixelShader9* LastPS = nullptr;  inline bool LastCapable = false;
	inline UInt32 Frames = 0, Conversions = 0, LinearDraws = 0, StoredDraws = 0, OtherTarget = 0;
	// UNOFFICIAL 2026-10-29 SharedDrawState ([Main.Main.Quality], live, NVR DXVK; ROOTPLAN round 4 step 11): CapturedNormals' lean hook
	// already keeps the render targets, pixel shader and blend states from the device's own Set calls inside the main list (the same span
	// as this one, opened inside it). On, this hook takes them from there instead of asking the device again (up to six Get calls a draw,
	// each through DXVK's device lock). Same values, same decisions: the picture is unchanged. Outside CapturedNormals' tracking (it off,
	// native D3D9, or a draw outside its span) the device is asked as before. Off: as before.
	inline bool SharedWanted = false;
	inline UInt32 SharedDraws = 0;

	typedef HRESULT(STDMETHODCALLTYPE* DIPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
	typedef HRESULT(STDMETHODCALLTYPE* DPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
	inline DIPFn NextDIP = nullptr;
	inline DPFn NextDP = nullptr;

	inline bool MakeResources(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (CopyTex && Made.Width == t.Width && Made.Height == t.Height && Made.Format == t.Format) return true;
		if (Copy) { Copy->Release(); Copy = nullptr; }
		if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
		if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &CopyTex, NULL)) || FAILED(CopyTex->GetSurfaceLevel(0, &Copy))) {
			Logger::Log("UNOFFICIAL LinearScene: could not make the frame's copy (%ux%u format %u): off for the session", t.Width, t.Height, (unsigned)t.Format);
			Failed = true;
			return false;
		}
		Made = t;
		if (!ConvertPS) {
			// c0: x the exponent, y 1 = the target is multisampled (add the difference to every sample instead of writing).
			static const char* kPS =
				"sampler2D S : register(s0); float4 E : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float4 c = tex2D(S, uv); float3 v = max(c.rgb, 0.0); float3 f = pow(v, E.x);"
				"  return E.y > 0.5 ? float4(f - v, 0.0) : float4(f, c.a); }";
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			if (FAILED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
				FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ConvertPS))) {
				Logger::Log("UNOFFICIAL LinearScene: conversion shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
				Failed = true;
			}
			if (errors) errors->Release();
			if (code) code->Release();
			if (Failed) return false;
		}
		Logger::Log("UNOFFICIAL LinearScene: on (frame copy %ux%u format %u, msaa %u)", t.Width, t.Height, (unsigned)t.Format, (unsigned)t.MultiSampleType);
		return true;
	}

	// The whole frame (the viewport the world draws into) to the other space. Everything the game set up for the draw about to happen is
	// put back exactly (it is called from inside that draw).
	inline bool Convert(IDirect3DDevice9* d, IDirect3DSurface9* target, bool toLinear) {
		D3DSURFACE_DESC t = {};
		if (Failed || !target || FAILED(target->GetDesc(&t)) || !MakeResources(d, t)) return false;
		D3DVIEWPORT9 vp = {};
		d->GetViewport(&vp);
		if (FAILED(d->StretchRect(target, NULL, Copy, NULL, D3DTEXF_NONE))) { Failed = true; Logger::Log("UNOFFICIAL LinearScene: frame copy failed: off for the session"); return false; }
		const bool msaa = t.MultiSampleType != D3DMULTISAMPLE_NONE;

		// Save what the draw needs.
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; IDirect3DBaseTexture9* tex0 = nullptr;
		DWORD fvf = 0; float c0[4] = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride); d->GetTexture(0, &tex0); d->GetPixelShaderConstantF(0, c0, 1);
		IDirect3DSurface9* rt[4] = {};
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &rt[i])) && rt[i]) d->SetRenderTarget(i, nullptr);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE,
			D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, msaa ? TRUE : FALSE, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE,
			msaa ? 7u : 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		static_assert(sizeof(kRS) / sizeof(kRS[0]) == sizeof(want) / sizeof(want[0]), "one value per state");
		DWORD saved[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &saved[i]))) saved[i] = want[i]; if (saved[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(0, kSS[i], &savedS[i]))) savedS[i] = wantS[i]; if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], wantS[i]); }

		d->SetVertexShader(nullptr);
		d->SetPixelShader(ConvertPS);
		const float e[4] = { toLinear ? Gamma : 1.0f / Gamma, msaa ? 1.0f : 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(0, e, 1);
		d->SetTexture(0, CopyTex);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f, x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / t.Width, v0 = (float)vp.Y / t.Height, u1 = (float)(vp.X + vp.Width) / t.Width, v1 = (float)(vp.Y + vp.Height) / t.Height;
		const float quad[4][6] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));

		// Put everything back for the game's draw.
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], savedS[i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (saved[i] != want[i]) d->SetRenderState(kRS[i], saved[i]);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) { d->SetRenderTarget(i, rt[i]); rt[i]->Release(); }
		d->SetViewport(&vp);	// (SetRenderTarget resets it)
		d->SetTexture(0, tex0); if (tex0) tex0->Release();
		d->SetPixelShaderConstantF(0, c0, 1);
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetVertexShader(vs); if (vs) vs->Release();
		d->SetPixelShader(ps); if (ps) ps->Release();
		Linear = toLinear;
		Conversions++;
		return true;
	}

	inline void SetFlag(IDirect3DDevice9* d, bool linear) {
		const D3DXVECTOR4& l = TheShaderManager->Shaders.PBR->Constants.Linear;
		const float c[4] = { l.x, l.y, l.z, linear ? 1.0f : 0.0f };
		d->SetPixelShaderConstantF(191, c, 1);
	}

	inline void BeforeDraw(IDirect3DDevice9* d) {
		if (!Live || !InSpan) return;
		const bool shared = SharedWanted && SharedDrawState::Valid;
		if (shared) SharedDraws++;
		IDirect3DPixelShader9* ps = nullptr;
		if (shared) ps = SharedDrawState::T.ps;
		else { d->GetPixelShader(&ps); if (ps) ps->Release(); }	// a key only
		if (ps != LastPS) { LastPS = ps; LastCapable = ps && Capable.count(ps) != 0; }
		IDirect3DSurface9* rt0 = nullptr;
		if (shared) rt0 = SharedDrawState::T.rt[0];
		else { d->GetRenderTarget(0, &rt0); if (rt0) rt0->Release(); }
		if (rt0 != SpanTarget) { OtherTarget++; if (LastCapable) SetFlag(d, false); return; }	// a copy, a shadow map, a mask: as before
		DWORD write = 0xF, blend = FALSE, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
		if (shared) write = SharedDrawState::T.colorWrite; else d->GetRenderState(D3DRS_COLORWRITEENABLE, &write);
		if (!(write & 7)) { if (LastCapable) SetFlag(d, false); return; }	// depth only
		if (shared) { blend = SharedDrawState::T.blend; if (blend) { src = SharedDrawState::T.src; dst = SharedDrawState::T.dst; } }
		else {
			d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			if (blend) { d->GetRenderState(D3DRS_SRCBLEND, &src); d->GetRenderState(D3DRS_DESTBLEND, &dst); }
		}
		const bool additive = blend && dst == D3DBLEND_ONE && (src == D3DBLEND_ONE || src == D3DBLEND_SRCALPHA);
		const bool wantLinear = LastCapable && (!blend || additive);
		if (wantLinear != Linear && !Convert(d, rt0, wantLinear)) { Live = false; if (LastCapable) SetFlag(d, false); return; }
		if (LastCapable) SetFlag(d, wantLinear);
		if (wantLinear) LinearDraws++; else StoredDraws++;
	}

	inline HRESULT STDMETHODCALLTYPE DIPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT pc) {
		BeforeDraw(d);
		return NextDIP(d, t, b, mi, nv, s, pc);
	}
	inline HRESULT STDMETHODCALLTYPE DPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT pc) {
		BeforeDraw(d);
		return NextDP(d, t, s, pc);
	}

	inline void Hook(IDirect3DDevice9* d) {
		if (Hooked || !d) return;
		void** table = *(void***)d;
		DWORD old = 0;
		if (!VirtualProtect(&table[81], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
		NextDP = (DPFn)table[81];  table[81] = (void*)&DPHook;
		NextDIP = (DIPFn)table[82]; table[82] = (void*)&DIPHook;
		VirtualProtect(&table[81], 2 * sizeof(void*), old, &old);
		Hooked = true;	// stays for the session (other hooks chain through it); off, it only passes the draw on
	}

	// UNOFFICIAL 2026-10-28 (native D3D9; found by the CapturedNormals session): the native runtime writes its own entries back into the
	// device's table, so the hook above was gone for nearly the whole session there (2.4 draws a frame seen against ~1,400 under NVR
	// DXVK: LinearScene silently off on native). Called inside the main list at every pass (GrassFirst::RenderPassImmediatelyHook) and
	// at BeginSpan: a slot holding exactly what this hook calls on to gets the hook back; anything else (another hook on top) is left.
	inline UInt32 Reinstalls = 0;
	inline void EnsureHooked(IDirect3DDevice9* d) {
		if (!Hooked || !d) return;
		void** table = *(void***)d;
		struct Slot { int i; void* ours; void* next; };
		const Slot slots[2] = { { 81, (void*)&DPHook, (void*)NextDP }, { 82, (void*)&DIPHook, (void*)NextDIP } };
		for (const Slot& s : slots) {
			if (table[s.i] == s.ours || table[s.i] != s.next) continue;
			DWORD old = 0;
			if (!VirtualProtect(&table[s.i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
			table[s.i] = s.ours;
			VirtualProtect(&table[s.i], sizeof(void*), old, &old);
			if (++Reinstalls <= 3) Logger::Log("UNOFFICIAL LinearScene: device table slot %d had been put back to the runtime's entry: hook put in again (logged 3 times)", s.i);
		}
	}

	// Render.cpp, once a frame.
	inline void BeginFrame(IDirect3DDevice9* d) {
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.PBR.Linear", "Enabled") && TheSettingManager->GetSettingI("Shaders.PBR.Linear", "LinearScene");
			Logger::Log("UNOFFICIAL LinearScene: %s", Compiled ? "compiled into the lit shaders (the world's main list is kept in linear light)" : "off (not compiled into the shaders)");
		}
		const D3DXVECTOR4& l = TheShaderManager->Shaders.PBR->Constants.Linear;
		Gamma = l.x;
		Live = Compiled && !Failed && Gamma > 1.0f && TheSettingManager->GetSettingI("Shaders.PBR.Linear", "LinearScene") != 0;
		SharedWanted = TheSettingManager->GetSettingI("Main.Main.Quality", "SharedDrawState") != 0;
		if (Live && !Hooked) Hook(d);
		if (Live && ++Frames >= 600) {
			Logger::Log("UNOFFICIAL LinearScene: per frame %.2f conversions, %.1f draws in linear light, %.1f in the stored curve, %.1f into other targets; %.1f draws read the state CapturedNormals tracks (SharedDrawState)",
				Conversions / (double)Frames, LinearDraws / (double)Frames, StoredDraws / (double)Frames, OtherTarget / (double)Frames, SharedDraws / (double)Frames);
			Frames = Conversions = LinearDraws = StoredDraws = OtherTarget = SharedDraws = 0;
		}
	}

	// GrassFirst::MainListHook, around the world's main list (world context only).
	inline void BeginSpan(IDirect3DDevice9* d) {
		if (!Live || InSpan) return;
		// UNOFFICIAL 2026-11-05 (Balebandro 256de14): not with the game's MSAA. Convert can only add one difference per pixel to all of its
		// samples (worked out from their resolved average), so where a pixel holds two surfaces -- every edge, and all over grass and leaves
		// with transparency multisampling -- the darker samples were pushed below zero (the target is float) and resolved black: a black
		// speckled outline on every bush and grass tuft (seen at 8x, gone with LinearScene off). The span is skipped: the main list stays in
		// the stored curve, as with LinearScene off.
		{
			IDirect3DSurface9* target = nullptr;
			D3DSURFACE_DESC desc = {};
			if (SUCCEEDED(d->GetRenderTarget(0, &target)) && target) { target->GetDesc(&desc); target->Release(); }
			if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
				static bool told = false;
				if (!told) { told = true; Logger::Log("UNOFFICIAL LinearScene: the world is multisampled (the game's MSAA %u): off while it is (its conversion cannot be exact per sample)", (unsigned)desc.MultiSampleType); }
				return;
			}
		}
		InSpan = true;
		Linear = false;
		LastPS = nullptr;
		EnsureHooked(d);	// (native D3D9 puts its own table entries back)
		if (SpanTarget) { SpanTarget->Release(); SpanTarget = nullptr; }
		d->GetRenderTarget(0, &SpanTarget);
	}
	inline void EndSpan(IDirect3DDevice9* d) {
		if (!InSpan) return;
		if (Linear && SpanTarget) Convert(d, SpanTarget, false);
		Linear = false;
		InSpan = false;
		if (SpanTarget) { SpanTarget->Release(); SpanTarget = nullptr; }
		IDirect3DPixelShader9* ps = nullptr;
		d->GetPixelShader(&ps);
		if (ps) { if (Capable.count(ps)) SetFlag(d, false); ps->Release(); }	// the lit shader still bound gives the stored curve again
	}
}
