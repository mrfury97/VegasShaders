#pragma once
// UNOFFICIAL 2026-11-10 PTBlendedLayer ([Main.Main.RayTracing] PTBlendedLayer; core\RtPathTrace.cpp Replace).
//
// Seen on an RTX-class test machine with PTRealtime: a fire's flames and smoke, neon tubes and glow cards, strings of light bulbs, lit
// window glows, the near water surface and blended hair were missing from the path traced picture. The game draws all of those AFTER
// its main opaque list, blended or with its no-lighting / water shaders: they are not in the ray tracing scene and not in the G-buffer,
// so the trace shows the opaque surface behind them and the replace paints that over the game's own pixels.
//
// The game's picture holds them, and ReactiveMask already keeps the scene's colour at the two moments that bracket them: Opaque (as the
// main opaque list leaves it) and Final (after every world draw, before the first-person model). What the game added between the two is
// carried over onto the path traced picture, per pixel and channel, in the scene's stored curve (the space the game blended in):
//     traced' = traced x min(1, (Final + e) / (Opaque + e)) + max(Final - Opaque, 0)
// An additive draw (fire, glow, neon: ONE + ONE) comes over exactly as the game added it; an alpha blend (smoke, water, hair, decals)
// as its darkening share plus its own light. Where nothing was drawn the two copies are equal: x 1, + 0.
// (2026-11-10, after the first run on an RTX-class card: water came back but pale where a rock lay under it, the traced rock lit as in air
// plus the water's light on top) where the game's layer CHANGED the pixel by a large share (water over its bed, hair over skin, a flame
// over dark ground: the largest channel's change over the larger of the two values, 25 % .. 75 %) the result goes over to the game's own
// pixel: traced' = lerp(the transfer above, Final, w). A light haze or a faint glow (a small share) stays a transfer.
// Only where the trace has a first surface (its albedo image's alpha): the sky, the distant land and whatever else the trace already
// took from the game's frame are left alone (those are in Final but not in Opaque). The first-person model is in neither copy: skipped
// by its depth. Two full-screen D3D9 draws into the scene after the traced picture went in (a multiply, then an add); nothing traced.
// Off (0, the default until measured): nothing here runs and ReactiveMask makes no copies on its behalf.
#include <d3d9.h>
#include <d3dx9.h>
#include "ReactiveMask.h"

namespace RtBlendedLayer {
	inline IDirect3DPixelShader9* PS = nullptr;
	inline bool Failed = false;
	inline UInt32 AppliedFrames = 0, SkippedFrames = 0;

	// vmMode: 0 no first-person depth, 1 normal depth (the model where < 0.9), 2 reversed (where > 0.01). Returns true when applied.
	inline bool Apply(IDirect3DDevice9* d, IDirect3DTexture9* firstSurface, IDirect3DTexture9* viewModelDepth, int vmMode) {
		if (Failed || !firstSurface) return false;
		if (ReactiveMask::Failed || !ReactiveMask::Opaque || !ReactiveMask::Final || ReactiveMask::CopiesFrame != ReactiveMask::FrameNumber()) { SkippedFrames++; return false; }
		IDirect3DSurface9* rt0 = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return false;
		D3DSURFACE_DESC t = {};
		rt0->GetDesc(&t);
		if (t.Width != ReactiveMask::Width || t.Height != ReactiveMask::Height) { rt0->Release(); SkippedFrames++; return false; }
		if (!PS) {
			// s0 the opaque copy, s1 the final copy, s2 the trace's first-surface colour (a >= 0.5: a surface), s3 the first-person depth;
			// c0: x 0 = the multiply draw, 1 = the add draw; y the first-person depth's kind; z e
			static const char* kPS =
				"sampler2D O : register(s0); sampler2D F : register(s1); sampler2D A : register(s2); sampler2D M : register(s3); float4 K : register(c0);"
				"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
				"  float3 o = max(tex2D(O, uv).rgb, 0.0f); float3 f = max(tex2D(F, uv).rgb, 0.0f);"
				"  float keep = step(0.5f, tex2D(A, uv).a);"
				"  float vm = tex2D(M, uv).x;"
				"  keep *= K.y > 1.5f ? step(vm, 0.01f) : K.y > 0.5f ? step(0.9f, vm) : 1.0f;"
				"  float3 mul = min((f + K.z) / (o + K.z), 1.0f); float3 add = max(f - o, 0.0f);"
				"  float3 dd = abs(f - o); float3 mm = max(f, o);"
				"  float w = smoothstep(0.25f, 0.75f, max(max(dd.r, dd.g), dd.b) / (max(max(mm.r, mm.g), mm.b) + 0.02f));"
				"  mul *= 1.0f - w; add = add * (1.0f - w) + f * w;"
				"  float3 r = K.x < 0.5f ? lerp(float3(1.0f, 1.0f, 1.0f), mul, keep) : add * keep;"
				"  return float4(r, 1.0f); }";
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			const bool ok = SUCCEEDED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_2_0", 0, &code, &errors, NULL)) && code &&
				SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &PS));
			if (!ok) Logger::Log("[ERROR] PTBlendedLayer: its shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
			if (code) code->Release();
			if (errors) errors->Release();
			if (!ok) { Failed = true; rt0->Release(); return false; }
			Logger::Log("UNOFFICIAL RT path trace: PTBlendedLayer on: what the game draws after its opaque list (fire, smoke, glow, neon, water, blended hair) is carried over onto the traced picture");
		}

		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DSurface9* zs = nullptr; IDirect3DBaseTexture9* tex[4] = {}; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetDepthStencilSurface(&zs); d->GetViewport(&vp);
		for (DWORD i = 0; i < 4; i++) d->GetTexture(i, &tex[i]);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE,
			D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE };
		// (the scene's alpha is left alone: other passes keep material flags there)
		const DWORD want[] = { FALSE, FALSE, TRUE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0x7u, D3DFILL_SOLID, FALSE, FALSE, 0,
			D3DBLEND_ZERO, D3DBLEND_SRCCOLOR, D3DBLENDOP_ADD, FALSE };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[4][sizeof(kSS) / sizeof(kSS[0])];
		for (DWORD s = 0; s < 4; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; d->SetSamplerState(s, kSS[i], wantS[i]); }
		float savedPS[4] = {};
		d->GetPixelShaderConstantF(0, savedPS, 1);

		d->SetDepthStencilSurface(NULL);
		const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(PS, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, ReactiveMask::Opaque);
		d->SetTexture(1, ReactiveMask::Final);
		d->SetTexture(2, firstSurface);
		d->SetTexture(3, viewModelDepth && vmMode ? (IDirect3DBaseTexture9*)viewModelDepth : (IDirect3DBaseTexture9*)firstSurface);
		struct V { float x, y, z, w, u, v; };
		const float w = (float)t.Width, h = (float)t.Height;
		const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
		float k[4] = { 0.0f, viewModelDepth ? (float)vmMode : 0.0f, 1.0e-3f, 0.0f };
		d->SetPixelShaderConstantF(0, k, 1);
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));	// scene x min(1, final / opaque)
		d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		k[0] = 1.0f;
		d->SetPixelShaderConstantF(0, k, 1);
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));	// + max(final - opaque, 0)

		for (DWORD s = 0; s < 4; s++) for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, savedPS, 1);
		d->SetDepthStencilSurface(zs); if (zs) zs->Release();
		d->SetViewport(&vp);
		for (DWORD i = 0; i < 4; i++) { d->SetTexture(i, tex[i]); if (tex[i]) tex[i]->Release(); }
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();
		rt0->Release();
		AppliedFrames++;
		return true;
	}
}
