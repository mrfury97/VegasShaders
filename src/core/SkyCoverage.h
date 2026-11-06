#pragma once

// UNOFFICIAL 2026-11-05 (from Balebandro's fork, mo2-dxvk-loader 256de14 / 8c9017b; written there 2026-11-04) SkyCoverage: with the game's MSAA, how much of each pixel is sky, and how much holds objects, exactly. NVR's
// effects and the terrain blending read one depth per pixel (the nearest sample's under NVR DXVK), so a pixel on an edge counted
// whole as the surface in it. The multisampled depth buffer still holds every sample: a full-screen quad at the far plane drawn into
// a multisampled mask with the depth test on, resolved, gives the share of the pixel's samples that pass.
//   Draw (Render.cpp, after every world draw, before the world's depth resolve): the samples still at the far plane -> TESR_SkyCoverage,
//      read by VolumetricClouds and VolumetricLight (Effects/Includes/MsaaSkyEdge.hlsl): their sky edges, not a bright rim.
//   DrawViewModel (Render.cpp, after the first-person render): the first-person model's samples (its own cleared depth buffer) taken out
//      of it again, so the clouds do not take the weapon for sky.
//   FixBlendDepth (Terrain.cpp, right after terrain blending's depth copy): the objects' samples; every pixel they cover only in part is
//      set to "nothing there" in the copy. The land is blended (and clipped) per pixel but shown per sample: a pixel half twig, half
//      nothing faded over the nothing too, white blotches at the foot of every bush. Now it fades only where the objects cover the whole
//      pixel and is solid in the mixed ones, as without blending.
// Without the game's MSAA nothing is made or drawn (the effects gate on TESR_GameMultisample).
#include <d3dx9.h>
#include "PassSums.h"	// AfterStateBlock

extern D3DXVECTOR4 NvrGameMultisample;	// Hooks/Settings.cpp

namespace SkyCoverage {
	inline IDirect3DSurface9* MaskMS = nullptr;				// the multisampled mask the quads are drawn into
	inline IDirect3DTexture9* Texture = nullptr;			// the sky share: TESR_SkyCoverage
	inline IDirect3DSurface9* TextureSurface = nullptr;
	inline IDirect3DTexture9* ObjectTexture = nullptr;		// the objects' share at terrain blending's depth copy
	inline IDirect3DSurface9* ObjectSurface = nullptr;
	inline IDirect3DPixelShader9* WhitePS = nullptr;
	inline IDirect3DPixelShader9* BlackPS = nullptr;
	inline IDirect3DPixelShader9* PartialPS = nullptr;		// keeps only the pixels the objects cover in part (s0: ObjectTexture)
	inline IDirect3DStateBlock9* State = nullptr;
	inline D3DSURFACE_DESC Made = {};
	inline bool Failed = false, Drawn = false, ToldFix = false;

	inline void Release() {
		if (MaskMS) { MaskMS->Release(); MaskMS = nullptr; }
		if (TextureSurface) { TextureSurface->Release(); TextureSurface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		if (ObjectSurface) { ObjectSurface->Release(); ObjectSurface = nullptr; }
		if (ObjectTexture) { ObjectTexture->Release(); ObjectTexture = nullptr; }
	}

	inline IDirect3DPixelShader9* Compile(IDirect3DDevice9* d, const char* source) {
		IDirect3DPixelShader9* shader = nullptr;
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
			FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &shader))) {
			Logger::Log("UNOFFICIAL SkyCoverage: shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
			shader = nullptr;
		}
		if (errors) errors->Release();
		if (code) code->Release();
		return shader;
	}

	inline bool Ensure(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (Failed) return false;
		if (Texture && Made.Width == t.Width && Made.Height == t.Height && Made.MultiSampleType == t.MultiSampleType && Made.MultiSampleQuality == t.MultiSampleQuality) return true;
		const bool remade = Texture != nullptr;
		Release();
		if (FAILED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A8R8G8B8, t.MultiSampleType, t.MultiSampleQuality, FALSE, &MaskMS, NULL)) ||
			FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &Texture, NULL)) ||
			FAILED(Texture->GetSurfaceLevel(0, &TextureSurface)) ||
			FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ObjectTexture, NULL)) ||
			FAILED(ObjectTexture->GetSurfaceLevel(0, &ObjectSurface))) {
			Logger::Log("UNOFFICIAL SkyCoverage: could not make the masks (%ux%u, msaa %u): off for the session", t.Width, t.Height, (unsigned)t.MultiSampleType);
			Release();
			Failed = true;
			return false;
		}
		if (!WhitePS) WhitePS = Compile(d, "float4 main() : COLOR0 { return 1.0; }");
		if (!BlackPS) BlackPS = Compile(d, "float4 main() : COLOR0 { return 0.0; }");
		if (!PartialPS) PartialPS = Compile(d, "sampler2D C : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { clip(0.999 - tex2D(C, uv).r); return 0.0; }");
		if (!WhitePS || !BlackPS || !PartialPS) { Release(); Failed = true; return false; }
		if (remade) {	// the effects keep the texture they bound first
			if (TheShaderManager->Effects.VolumetricClouds) TheShaderManager->Effects.VolumetricClouds->ClearSampler("TESR_SkyCoverage", 16);
			if (TheShaderManager->Effects.VolumetricLight) TheShaderManager->Effects.VolumetricLight->ClearSampler("TESR_SkyCoverage", 16);
		}
		Made = t;
		Logger::Log("UNOFFICIAL SkyCoverage: on (masks %ux%u, msaa %u): the clouds, the volumetric light and the terrain blending take each edge pixel's exact share",
			t.Width, t.Height, (unsigned)t.MultiSampleType);
		return true;
	}

	// The device state round the passes below, put back as it was (a state block; the targets and viewport by hand).
	struct Scope {
		IDirect3DDevice9* d;
		IDirect3DSurface9* target = nullptr; IDirect3DSurface9* depth = nullptr; IDirect3DSurface9* extra[3] = {};
		D3DVIEWPORT9 viewport = {};
		D3DSURFACE_DESC t = {};
		bool ok = false;
		explicit Scope(IDirect3DDevice9* device) : d(device) {
			if (Failed || NvrGameMultisample.x <= 1.0f) return;
			d->GetRenderTarget(0, &target);
			d->GetDepthStencilSurface(&depth);
			D3DSURFACE_DESC z = {};
			if (!target || !depth || FAILED(target->GetDesc(&t)) || FAILED(depth->GetDesc(&z)) || t.MultiSampleType == D3DMULTISAMPLE_NONE ||
				z.MultiSampleType != t.MultiSampleType || z.Width < t.Width || z.Height < t.Height || !Ensure(d, t) ||
				(!State && FAILED(d->CreateStateBlock(D3DSBT_ALL, &State)))) return;
			State->Capture();
			d->GetViewport(&viewport);
			for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &extra[i - 1])) && extra[i - 1]) d->SetRenderTarget(i, nullptr);
			ok = true;
		}
		~Scope() {
			if (ok) {
				d->SetRenderTarget(0, target);
				for (DWORD i = 1; i < 4; i++) if (extra[i - 1]) { d->SetRenderTarget(i, extra[i - 1]); extra[i - 1]->Release(); }
				d->SetDepthStencilSurface(depth);
				d->SetViewport(&viewport);	// (SetRenderTarget resets it)
				State->Apply();
				// 2026-11-04: CapturedNormals' lean hook re-reads the state: the block put COLORWRITEENABLE back without a Set call, so after
				// FixBlendDepth's quads every land draw read as depth-only and the land never reached the G-buffer (terrain blending on)
				if (PassSums::AfterStateBlock) PassSums::AfterStateBlock(d);
			}
			if (target) target->Release();
			if (depth) depth->Release();
		}
		// A full-screen quad at the far plane into the bound target, against the bound depth buffer with zFunc.
		void Quad(DWORD zFunc, BOOL zWrite, DWORD colourWrite, IDirect3DPixelShader9* ps) {
			const bool reversed = TheRenderManager->IsReversedDepth();
			static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE, D3DRS_ALPHATESTENABLE,
				D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE,
				D3DRS_SRGBWRITEENABLE, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK };
			const DWORD want[] = { D3DZB_TRUE, (DWORD)zWrite, zFunc, FALSE, FALSE, FALSE, D3DCULL_NONE, colourWrite, FALSE, 0, FALSE, FALSE, TRUE, 0xFFFFFFFF };
			for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) d->SetRenderState(kStates[i], want[i]);
			const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
			d->SetViewport(&full);
			d->SetVertexShader(nullptr);
			d->SetPixelShader(ps);
			d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			const float z = reversed ? 0.0f : 1.0f;
			const float x0 = -0.5f, y0 = -0.5f, x1 = (float)t.Width - 0.5f, y1 = (float)t.Height - 0.5f;
			const float quad[4][6] = { { x0, y0, z, 1.0f, 0.0f, 0.0f }, { x1, y0, z, 1.0f, 1.0f, 0.0f }, { x0, y1, z, 1.0f, 0.0f, 1.0f }, { x1, y1, z, 1.0f, 1.0f, 1.0f } };
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		}
	};
	// The depth test that passes the samples at the far plane, and the one that passes the samples in front of it.
	inline DWORD AtFar() { return TheRenderManager->IsReversedDepth() ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL; }
	inline DWORD InFront() { return TheRenderManager->IsReversedDepth() ? D3DCMP_LESS : D3DCMP_GREATER; }

	// Before the world's depth resolve (Render.cpp): the scene target and its multisampled depth buffer are bound.
	inline void Draw(IDirect3DDevice9* d) {
		Drawn = false;
		Scope s(d);
		if (!s.ok) return;
		d->SetRenderTarget(0, MaskMS);
		d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
		s.Quad(AtFar(), FALSE, 0xF, WhitePS);
		d->StretchRect(MaskMS, NULL, TextureSurface, NULL, D3DTEXF_NONE);	// the resolve averages the samples: the sky share
		Drawn = true;
	}

	// After the first-person render (Render.cpp): the depth buffer holds the first-person model alone (cleared before it).
	inline void DrawViewModel(IDirect3DDevice9* d) {
		if (!Drawn) return;
		Drawn = false;
		Scope s(d);
		if (!s.ok) return;
		d->SetRenderTarget(0, MaskMS);	// (still this frame's sky samples)
		s.Quad(InFront(), FALSE, 0xF, BlackPS);
		d->StretchRect(MaskMS, NULL, TextureSurface, NULL, D3DTEXF_NONE);
	}

	// Right after terrain blending copied the depth drawn so far (Terrain.cpp CopySceneDepth): the scene's multisampled depth is bound.
	inline void FixBlendDepth(IDirect3DDevice9* d, IDirect3DTexture9* copy) {
		IDirect3DSurface9* copySurface = nullptr;
		if (!copy || FAILED(copy->GetSurfaceLevel(0, &copySurface)) || !copySurface) return;
		{
			Scope s(d);
			if (s.ok) {
				d->SetRenderTarget(0, MaskMS);
				d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
				s.Quad(InFront(), FALSE, 0xF, WhitePS);
				d->StretchRect(MaskMS, NULL, ObjectSurface, NULL, D3DTEXF_NONE);	// the objects' share
				// The copy as the depth buffer, the sky mask's texture as a target nothing is written to: every pixel the objects cover
				// in part gets the far plane's depth.
				d->SetRenderTarget(0, TextureSurface);
				d->SetDepthStencilSurface(copySurface);
				d->SetTexture(0, ObjectTexture);
				d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
				s.Quad(D3DCMP_ALWAYS, TRUE, 0, PartialPS);
				d->SetTexture(0, nullptr);
				if (!ToldFix) { ToldFix = true; Logger::Log("UNOFFICIAL SkyCoverage: terrain blending's depth copy: the pixels objects cover in part read as nothing there (the land stays solid in them)"); }
			}
		}
		copySurface->Release();
	}
}
