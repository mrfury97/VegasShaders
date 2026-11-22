#pragma once
// UNOFFICIAL 2026-11-15 (port of mrfury97's GIGrassBlur / GIGrassDenoise; docs/next/FURY-RTGI-PORT.md "Grass"): the albedo buffer with alpha 0 on
// the grass. The fork finds grass as "no albedo record" (its grass never writes one). Here [Main.Main.Quality] CapturedGrass (on by default)
// makes the grass write albedo records whenever RT GI runs, so that test missed our grass. This copy keeps the albedo's colour and clears its
// alpha where the MaterialBuffer carries the grass mark (CapturedNormals.h kGrassMark 0.5 with "written" in a; kGrassMarkV2 4.5 with
// GBufferV2, plus the deferred grass record's g range [32, 1024) while DeferredFoliage makes such records: the same tests as MergeIntoNormals).
// Pixels with no record keep alpha 0 as before, so CapturedGrass off still works. The readers (AmbientLight's Blur mask, the GI denoiser's
// Settings.Soft with GuideAlbedo) take this in place of the albedo buffer only while those switches run. Made once a frame, before any pass.
#include "CapturedNormals.h"

namespace GrassMaskedAlbedo {
	inline IDirect3DTexture9* Texture = nullptr;  inline IDirect3DSurface9* Surface = nullptr;
	inline UINT Width = 0, Height = 0;  inline bool Failed = false, PSFailed = false, Logged = false;
	inline IDirect3DPixelShader9* PS = nullptr;
	inline UInt32 Frame = 0xFFFFFFFFu;  inline bool Made = false;

	inline const char* kPS =
		"sampler2D A : register(s0); sampler2D M : register(s1); float4 G : register(c0);"
		"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
		"  float4 a = tex2D(A, uv); float4 m = tex2D(M, uv);"
		"  bool grass = m.r > 0.5f && ((abs(m.g - G.x) < 0.1f && (G.y < 0.5f || m.a > 0.5f)) || (G.z > 0.5f && m.g > 31.99f && m.g < 1024.0f));"
		"  return float4(a.rgb, grass ? 0.0f : a.a); }";

	inline void Release() {
		if (Surface) { Surface->Release(); Surface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		Width = Height = 0;
	}
	// The masked copy for this frame, or the albedo buffer itself when there is no MaterialBuffer this frame or the copy cannot be made.
	inline IDirect3DBaseTexture9* Get(IDirect3DDevice9* d) {
		IDirect3DTexture9* plain = CapturedNormals::AlbedoTexture;
		if (!d || !plain || CapturedNormals::AlbedoData.x < 0.5f || !CapturedNormals::MaterialTexture || CapturedNormals::MaterialData.x < 0.5f || Failed || PSFailed) return plain;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame == Frame) return Made ? (IDirect3DBaseTexture9*)Texture : (IDirect3DBaseTexture9*)plain;
		Frame = frame; Made = false;
		D3DSURFACE_DESC desc = {};
		if (FAILED(plain->GetLevelDesc(0, &desc))) return plain;
		if (!Texture || Width != desc.Width || Height != desc.Height) {
			Release();
			if (FAILED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Texture, NULL)) ||
				FAILED(Texture->GetSurfaceLevel(0, &Surface))) {
				Release(); Failed = true;
				Logger::Log("[ERROR] UNOFFICIAL GrassMaskedAlbedo: its %ux%u target could not be made: the grass masks fall back to the albedo buffer (no record = soft) for this session", desc.Width, desc.Height);
				return plain;
			}
			d->ColorFill(Surface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));	// clean border outside active RenderRect on first make
			Width = desc.Width; Height = desc.Height;
		}
		if (!PS && !(PS = CapturedNormals::FullScreenShader(d, kPS, "UNOFFICIAL GrassMaskedAlbedo", PSFailed))) return plain;
		const bool v2 = CapturedNormals::V2Compiled;
		const float g[4] = { v2 ? CapturedNormals::kGrassMarkV2 : CapturedNormals::kGrassMark, v2 ? 0.0f : 1.0f, v2 && CapturedNormals::FoliageGrassRange ? 1.0f : 0.0f, 0.0f };
		float saved[4] = {};
		d->GetPixelShaderConstantF(0, saved, 1);
		d->SetPixelShaderConstantF(0, g, 1);
		CapturedNormals::FullScreenPass(d, Surface, PS, plain, false, D3DBLEND_ONE, D3DBLEND_ZERO, CapturedNormals::MaterialTexture);
		d->SetPixelShaderConstantF(0, saved, 1);
		Made = true;
		if (!Logged) { Logged = true; Logger::Log("UNOFFICIAL GrassMaskedAlbedo: the RT GI grass masks (GIGrassBlur, GIGrassDenoise) take the MaterialBuffer's grass mark (%s) as well as pixels without an albedo record.", v2 ? "4.5, GBufferV2" : "0.5"); }
		return Texture;
	}
}
