#pragma once

// UNOFFICIAL 2026-11-12 [Main.Main.Scope] PostEffects 1 (core/ScopeCamera.h): distance fog on the picture-in-picture scope's picture.
// The picture is the world drawn by another camera into its own texture, outside the frame: no post effect of this mod ever sees it,
// so distant things in the lens had no fog at all while the main view has it. This is one self-contained pass, drawn right after the
// scene's last draw (Render.cpp ScopeScene::Draw), while the picture's target and its depth buffer are still the device's:
//  1. the picture's depth buffer is resolved into an INTZ texture of the picture's own size (RenderManager::ResolveDepthBuffer's RESZ
//     path: it copies the depth surface bound NOW; the frame's own depth texture has the screen's size and cannot take it);
//  2. one quad over the picture, alpha blended (the target is not read: no copy of it is needed): fog colour, alpha = the fog's amount
//     at the pixel's distance, from the weather's own fog (TESR_FogData near / far / power, TESR_FogColor: what UpdateConstants read
//     from the sky last frame) times FogStrength. Pixels at the far plane (the sky) are left alone.
// What it is NOT: VolumetricFog's look (height fog, sun scattering, aerial tint, lamps: Effects/VolumetricFog.fx.hlsl, 1,300 lines tied
// to the frame's depth, normals, shadow atlas and history). The amount at a distance follows the game's fog curve only.
// Needs RESZ (AMD / Intel drivers, DXVK). Without it (NVIDIA's own D3D9 driver: the NVAPI path keeps one registered depth surface, the
// frame's) nothing is drawn, said once. UNTESTED in game: ReactiveMask.h notes one run in which a D3D9 pass read an INTZ texture as 0
// under this mod's DXVK build; here a depth of 0 gives "no fog" (reversed depth: the far plane; normal depth: the near plane), so the
// worst case is a picture without fog, as before. Off (PostEffects 0, the default): nothing is made, resolved or drawn.
#include "DrawState.h"	// SharedDrawState::OwnPass: the constants uploaded here are this pass's own (ConstantFilter's lamp-block rule)

namespace ScopeFog {
	inline IDirect3DTexture9* Depth = nullptr;	// INTZ, the picture's size
	inline UINT Width = 0, Height = 0;
	inline IDirect3DPixelShader9* PS = nullptr;
	inline bool Failed = false;
	inline unsigned Logged = 0;		// bit 0: drawn, bit 1: cannot run

	inline void Cannot(const char* why) {
		if (Logged & 2u) return;
		Logged |= 2u;
		Logger::Log("UNOFFICIAL Scope PostEffects: the scope picture's fog cannot run (%s); the picture stays without fog.", why);
	}

	inline bool Ensure(IDirect3DDevice9* d, UINT w, UINT h) {
		if (Depth && Width == w && Height == h) return true;
		if (Depth) { Depth->Release(); Depth = nullptr; }
		Width = Height = 0;
		if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &Depth, NULL)) || !Depth) {
			Depth = nullptr;
			Failed = true;
			Cannot("no INTZ depth texture of the picture's size could be made");
			return false;
		}
		Width = w; Height = h;
		return true;
	}

	inline bool Compile(IDirect3DDevice9* d) {
		if (PS) return true;
		// s0 the picture's depth. c0: x, y the projection's z terms (_33, _43), z, w 1 / its x and y scales (_11, _22);
		// c1: fog near, far, power, strength; c2: rgb the fog's colour, w the far plane.
		static const char* kPS =
			"sampler2D D : register(s0); float4 P : register(c0); float4 F : register(c1); float4 C : register(c2);"
			"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
			"  float d = tex2D(D, uv).x;"
			"  float z = P.y / (d - P.x);"
			"  float2 n = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);"
			"  float dist = z * length(float3(n.x * P.z, n.y * P.w, 1.0f));"
			"  float fog = pow(saturate((dist - F.x) / max(F.y - F.x, 1.0f)), max(F.z, 0.01f)) * F.w;"
			"  if (z <= 0.0f || z >= C.w * 0.999f) fog = 0.0f;"
			"  return float4(C.rgb, saturate(fog)); }";
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		const bool ok = SUCCEEDED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_2_0", 0, &code, &errors, NULL)) && code &&
			SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &PS));
		if (!ok) {
			Logger::Log("[ERROR] Scope PostEffects: the fog shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
			Failed = true;
			PS = nullptr;
		}
		if (code) code->Release();
		if (errors) errors->Release();
		return ok;
	}

	// After the scene's last draw: the device's render target and depth buffer are the picture's. projection: the picture's camera
	// (RenderManager::SetupExtraCamera). Every device state touched is read first and put back (shaders through the game's state cache,
	// as ReactiveMask::Build does), so the game's render-state cache stays true.
	inline void Draw(IDirect3DDevice9* d, const D3DXMATRIX& projection, UINT width, UINT height, float strength) {
		if (Failed || !(strength > 0.0f) || !width || !height) return;
		if (!TheRenderManager->RESZ) { Cannot("this Direct3D 9 driver has no RESZ depth resolve"); return; }
		IDirect3DSurface9* zs = nullptr;
		d->GetDepthStencilSurface(&zs);
		if (!zs) { Cannot("the picture has no depth buffer bound"); return; }
		D3DSURFACE_DESC zdesc = {};
		zs->GetDesc(&zdesc);
		// the depth buffer may be larger than the picture (a pooled one): the texture takes the picture's part only when they match
		if (zdesc.Width != width || zdesc.Height != height) {
			char why[200];
			sprintf_s(why, "the picture's depth buffer is %ux%u (format %d), the picture %ux%u: a RESZ copy needs the same size",
				zdesc.Width, zdesc.Height, (int)zdesc.Format, width, height);
			zs->Release();
			Cannot(why);
			return;
		}
		if (!Ensure(d, width, height) || !Compile(d)) { zs->Release(); return; }
		const D3DXVECTOR4& fogData = TheShaderManager->ShaderConst.fogData;		// near, far, glare, power
		const D3DXVECTOR4& fogColor = TheShaderManager->ShaderConst.fogColor;
		if (!(fogData.y > fogData.x) || !(projection._11 > 0.0f) || !(projection._22 > 0.0f)) { zs->Release(); return; }

		TheRenderManager->ResolveDepthBuffer(Depth);	// (RESZ: the depth surface bound now, i.e. the picture's)

		SharedDrawState::OwnPass ownPass;	// (the uploads below are this pass's: no lamp block is put "back" over them)
		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DBaseTexture9* tex0 = nullptr; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetTexture(0, &tex0); d->GetViewport(&vp);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
			D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE,
			D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		// (colour only: the picture's alpha is left as the scene wrote it)
		const DWORD want[] = { FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD,
			FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE,
			D3DFILL_SOLID, FALSE, FALSE, 0 };
		const unsigned stateCount = sizeof(kRS) / sizeof(kRS[0]);
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < stateCount; i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		const unsigned samplerCount = sizeof(kSS) / sizeof(kSS[0]);
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < samplerCount; i++) { if (FAILED(d->GetSamplerState(0, kSS[i], &savedS[i]))) savedS[i] = wantS[i]; if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], wantS[i]); }
		float savedPS[12] = {};
		d->GetPixelShaderConstantF(0, savedPS, 3);

		const D3DVIEWPORT9 full = { 0, 0, width, height, 0.0f, 1.0f };
		d->SetViewport(&full);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(PS, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, Depth);
		const float k[12] = { projection._33, projection._43, 1.0f / projection._11, 1.0f / projection._22,
			fogData.x, fogData.y, fogData.w, strength,
			fogColor.x, fogColor.y, fogColor.z, TheRenderManager->CameraData.y };
		d->SetPixelShaderConstantF(0, k, 3);
		struct V { float x, y, z, w, u, v; };
		const float w = (float)width, h = (float)height;
		const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));

		for (unsigned i = 0; i < samplerCount; i++) if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], savedS[i]);
		for (unsigned i = 0; i < stateCount; i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, savedPS, 3);
		d->SetViewport(&vp);
		d->SetTexture(0, tex0); if (tex0) tex0->Release();
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();
		zs->Release();

		if (!(Logged & 1u)) {
			Logged |= 1u;
			Logger::Log("UNOFFICIAL Scope PostEffects 1: distance fog drawn on the scope picture (%ux%u) from its own depth: the weather's fog from %.0f to %.0f units, power %.2f, strength %.2f.",
				width, height, fogData.x, fogData.y, fogData.w, strength);
		}
	}
}
