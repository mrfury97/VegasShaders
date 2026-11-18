#pragma once
// UNOFFICIAL 2026-11-11 PTEarlyLook ([Main.Main.RayTracing] PTEarlyLook, 0 off; core\RtPathTrace.cpp Show / Replace;
// docs/next/PT-EARLY-KICK-STEP2-2026-11-11.md slices 1 and 2). Bench only, with the early kick (env NVR_RT_EARLY_PT).
//
// An early-submitted path traced frame (RtPathTrace::SubmitBeforeWorld) cannot read this frame's G-buffer: it finds the first surface
// with its own camera rays, so its picture lacked the game's sky and clouds, the far land, grass, the land's blended layers, and had pale
// faces. At SHOW time this frame's raster buffers exist. One full-screen D3D9 draw (before the denoiser) makes the MERGED FIRST SURFACE,
// which the show pass, the denoiser's guide, PTBlendedLayer and DLSS RR's guides then read instead of the trace's own colour image:
//   rgb  the trace's first-surface colour; with slice 2 (PTEarlyLook >= 2) the GAME's (TESR_AlbedoBuffer, clamped 0-1, floor 0.02 as the
//        trace's) where the traced first hit is the game's surface (its view depth within max(3 %, (8 + slack / 0.35) units) of the game's)
//   a    the trace's (>= 0.5 a lit surface, 1 + metal with PTDenoiseSplit), or
//        -1 the game's own pixel (RtPathTraceShow.frag modulate bit 8 discards: the scene already holds it), where the live mode shows the
//           game's pixel too: the game's depth at the far plane (sky), or the depth valid and the AlbedoBuffer not written there (far LOD,
//           grass, skin the skin blur did not fill, any shader that is not an NVR template), or the trace met no surface (a < 0.5);
//        -2 (slice 2) the traced first hit is NOT the game's surface (or a glow the game draws: the trace's depth -1): the game's pixel too.
// PTEarlyLook 3 paints the classes over the finished scene (Paint): magenta -1, cyan -2, green a traced-lit surface.
// Off, or outside early frames: nothing here runs, and every reader gets the trace's own image as before.
#include <d3d9.h>
#include <d3dx9.h>
#include "DxvkCompute.h"

namespace RtEarlyLook {
	inline IDirect3DPixelShader9* MergePS = nullptr;
	inline IDirect3DPixelShader9* PaintPS = nullptr;
	inline bool Failed = false;
	inline IDirect3DTexture9* Merged = nullptr;
	inline IDirect3DSurface9* MergedSurface = nullptr;
	inline UInt32 MergedFrames = 0, DepthFrames = 0, SkippedFrames = 0;	// (the 600-frame log line in RtPathTrace.cpp)

	// ONE source, two entry points: main (the merge) and paint (PTEarlyLook 3). ps_3_0.
	// main:  s0 the trace's first-surface colour (RtPathTrace.cpp albedoTexture), s1 TESR_DepthBuffer (view z / farZ), s2 TESR_AlbedoBuffer,
	//        s3 the raw trace (displayTexture; with slice 2 its a = the first hit's view z / farZ, 0 a miss, -1 a glow the game draws);
	//        c0 x 1 = slice 2, y slice 2's depth tolerance floor ((8 + slack / 0.35) / farZ)
	// paint: s0 the merged image
	inline const char* const kPS =
		"sampler2D T : register(s0); sampler2D D : register(s1); sampler2D G : register(s2); sampler2D R : register(s3); float4 K : register(c0);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 t = tex2D(T, uv); float z0 = tex2D(D, uv).r; float4 g = tex2D(G, uv);"
		"  bool game = !(z0 > 0.0f && z0 < 0.9999f) || g.a < 0.5f || t.a < 0.5f;"
		"  if (game) return float4(t.rgb, -1.0f);"
		"  if (K.x < 0.5f) return t;"
		"  float tz = tex2D(R, uv).a;"
		"  bool same = tz > 0.0f && abs(tz - z0) <= max(0.03f * z0, K.y);"
		"  return same ? float4(max(saturate(g.rgb), 0.02f), t.a) : float4(t.rgb, -2.0f); }"
		"float4 paint(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float a = tex2D(T, uv).a;"
		"  return a < -1.5f ? float4(0.0f, 1.0f, 1.0f, 1.0f) : a < -0.5f ? float4(1.0f, 0.0f, 1.0f, 1.0f) : float4(0.0f, 1.0f, 0.0f, 1.0f); }";

	inline IDirect3DPixelShader9* Compile(IDirect3DDevice9* d, const char* entry) {
		IDirect3DPixelShader9* out = nullptr;
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		const bool ok = SUCCEEDED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, entry, "ps_3_0", 0, &code, &errors, NULL)) && code &&
			SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &out));
		if (!ok) { Logger::Log("[ERROR] PTEarlyLook: its shader (%s) failed (%s): off for the session", entry, errors ? (const char*)errors->GetBufferPointer() : "?"); out = nullptr; }
		if (code) code->Release();
		if (errors) errors->Release();
		return out;
	}

	// One full-screen draw of ps into target (nullptr: the device's current render target 0), textures on stages 0-3, k in c0, colour write
	// mask `write`; every state it changes is put back (as RtBlendedLayer::Apply / RtPathTrace.cpp ViewModelPrep).
	inline void Draw(IDirect3DDevice9* d, IDirect3DSurface9* target, IDirect3DPixelShader9* psNew, IDirect3DBaseTexture9* const* textures, const float* k, DWORD write, UINT w, UINT h) {
		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0;
		IDirect3DSurface9* zs = nullptr; IDirect3DSurface9* rt[4] = {}; IDirect3DBaseTexture9* tex[4] = {}; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		d->GetDepthStencilSurface(&zs); d->GetViewport(&vp);
		for (DWORD i = 0; i < 4; i++) d->GetTexture(i, &tex[i]);
		if (target) for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		RECT scissor = {}; const bool haveScissor = SUCCEEDED(d->GetScissorRect(&scissor));	// (SetRenderTarget(0) resets it)
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, write, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[4][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD s = 0; s < 4; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; d->SetSamplerState(s, kSS[i], wantS[i]); }
		float savedPS[4] = {};
		d->GetPixelShaderConstantF(0, savedPS, 1);

		if (target) {
			for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
			d->SetRenderTarget(0, target);
		}
		d->SetDepthStencilSurface(NULL);
		const D3DVIEWPORT9 full = { 0, 0, w, h, 0.0f, 1.0f };
		d->SetViewport(&full);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(psNew, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		for (DWORD i = 0; i < 4; i++) d->SetTexture(i, textures[i]);
		d->SetPixelShaderConstantF(0, k, 1);
		struct V { float x, y, z, w, u, v; };
		const float fw = (float)w, fh = (float)h;
		const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { fw - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, fh - 0.5f, 0, 1, 0, 1 }, { fw - 0.5f, fh - 0.5f, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));

		for (DWORD s = 0; s < 4; s++) for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, savedPS, 1);
		if (target) {
			d->SetRenderTarget(0, rt[0]);
			for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, rt[i]);
			for (DWORD i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
		}
		d->SetDepthStencilSurface(zs); if (zs) zs->Release();
		d->SetViewport(&vp);
		if (haveScissor) d->SetScissorRect(&scissor);
		for (DWORD i = 0; i < 4; i++) { d->SetTexture(i, tex[i]); if (tex[i]) tex[i]->Release(); }
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();
	}

	// The merged first surface (Merged, the size of `traced`). Every input must be there (the caller checks this frame's AlbedoBuffer is
	// this frame's): false = not made (the caller keeps the trace's own image). slice2: the raw trace's alpha is its first hit's depth this
	// frame; tolerance: (8 + slack / 0.35) / farZ.
	inline bool Merge(IDirect3DDevice9* d, IDirect3DTexture9* traced, IDirect3DBaseTexture9* depth, IDirect3DBaseTexture9* gameAlbedo, IDirect3DTexture9* raw,
		bool slice2, float tolerance) {
		if (Failed || !traced || !depth || !gameAlbedo || !raw) return false;
		if (!MergePS) { MergePS = Compile(d, "main"); if (!MergePS) { Failed = true; return false; } }
		D3DSURFACE_DESC td = {};
		if (FAILED(traced->GetLevelDesc(0, &td))) return false;
		if (Merged) { D3DSURFACE_DESC md = {}; Merged->GetLevelDesc(0, &md); if (md.Width != td.Width || md.Height != td.Height) { DxvkCompute::Drop(MergedSurface); DxvkCompute::Drop(Merged); } }
		if (!Merged) {
			if (FAILED(d->CreateTexture(td.Width, td.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Merged, nullptr)) || !Merged ||
				FAILED(Merged->GetSurfaceLevel(0, &MergedSurface)) || !MergedSurface) {
				DxvkCompute::Drop(MergedSurface); DxvkCompute::Drop(Merged);
				Logger::Log("[ERROR] PTEarlyLook: its %ux%u target could not be made: off for the session", td.Width, td.Height);
				Failed = true;
				return false;
			}
		}
		IDirect3DBaseTexture9* const textures[4] = { traced, depth, gameAlbedo, raw };
		const float k[4] = { slice2 ? 1.0f : 0.0f, tolerance, 0.0f, 0.0f };
		Draw(d, MergedSurface, MergePS, textures, k, 0xFu, td.Width, td.Height);
		return true;
	}

	// PTEarlyLook 3: the classes over the device's current render target (the scene), colour only (the scene's alpha keeps its material flags)
	inline void Paint(IDirect3DDevice9* d) {
		if (Failed || !Merged) return;
		if (!PaintPS) { PaintPS = Compile(d, "paint"); if (!PaintPS) { Failed = true; return; } }
		IDirect3DSurface9* rt0 = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return;
		D3DSURFACE_DESC t = {}, md = {};
		rt0->GetDesc(&t);
		rt0->Release();
		Merged->GetLevelDesc(0, &md);
		if (t.Width != md.Width || t.Height != md.Height) return;	// (texel for texel only)
		IDirect3DBaseTexture9* const textures[4] = { Merged, Merged, Merged, Merged };
		const float k[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Draw(d, nullptr, PaintPS, textures, k, 0x7u, t.Width, t.Height);
	}
}
