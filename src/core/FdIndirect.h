#pragma once
// UNOFFICIAL 2026-11-15 full deferred renderer, phase 6: indirect light and the RT / DLSS contract read G-buffer v2
// (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.6 and phase 6; results, numbers and bench requests: docs/next/FD-GI-RESULTS.md).
//
// Every switch below is live, test only, off by default, and does something only while [Main.Main.Quality] DeferredRenderer is on at
// startup (1 or 2; it forces GBufferV2, CapturedNormals, MaterialBuffer and AlbedoBuffer: core/SettingForce.h) and the G-buffer holds this
// frame's targets. Off, or the renderer off: nothing here runs, nothing is made, every effect reads what it read before. No shader file is
// touched by these three: the passes are the C++ strings in core/FdIndirectShaders.h, and the effects get a different texture on a stage
// they already read (the shader code and its constants as before).
//  - DeferredReflections: Reflections (the D3D9 effect, its Vulkan passes on the fork, the RT reflection rays made by its RayGen pass and the
//    RT reflection denoiser) read the normal-mapped shading normal (ShadingNormals) in place of the smooth normals buffer, and their material
//    copy takes the G-buffer's specular bit and model (ReflMaterial): shiny objects reflect by their smoothness whatever way they face.
//  - DeferredALAlbedo (+ DeferredALAlbedoGain): AmbientLight's bounce (its grid bounce and the RT GI composite) takes COLOR1 x (1 - metal)
//    x the gain in place of COLOR1 (ALAlbedo).
//  - DeferredRRGuides: the DLSS Ray Reconstruction guides take the shading normal (ShadingNormals) and the G-buffer's albedo (its RRAlbedo 2
//    path: base x (1 - metal) diffuse, the metal's F0 specular) whatever RRAlbedo says.
// The fourth phase-6 item (RT sun shadows as DeferredSun's visibility term) is a shader define of the sun pass: DeferredRtSunFirst
// (core/DeferredSun.h, DeferredSun.hlsl DS_RT_SUN_FIRST).
#include "CapturedNormals.h"
#include "SettingForce.h"
#include "FdIndirectShaders.h"

namespace FdIndirect {
	inline const char* kSection = "Main.Main.Quality";

	// The renderer on at startup, G-buffer v2 compiled in, the captured targets alive.
	inline bool Base() { return SettingForce::Mode() > 0 && CapturedNormals::V2CompiledIn() && !CapturedNormals::Failed; }
	inline bool Key(const char* key) { return TheSettingManager->GetSettingI(kSection, key) != 0; }
	inline bool MaterialNow() { return CapturedNormals::MaterialTexture && CapturedNormals::MaterialData.x > 0.5f; }
	inline bool ReflectionsOn() { return Base() && MaterialNow() && Key("DeferredReflections"); }
	inline bool ALAlbedoOn() { return Base() && MaterialNow() && CapturedNormals::AlbedoTexture && CapturedNormals::AlbedoData.x > 0.5f && Key("DeferredALAlbedo"); }
	inline bool RRGuidesOn() { return Base() && MaterialNow() && Key("DeferredRRGuides"); }

	// One render target, remade when the size or format it must have changes.
	struct Target {
		IDirect3DTexture9* Texture = nullptr; IDirect3DSurface9* Surface = nullptr;
		UINT Width = 0, Height = 0; D3DFORMAT Format = D3DFMT_UNKNOWN; bool Failed = false;
		void Release() { if (Surface) { Surface->Release(); Surface = nullptr; } if (Texture) { Texture->Release(); Texture = nullptr; } Width = Height = 0; }
		bool Ensure(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, const char* what) {
			if (Texture && Width == w && Height == h && Format == f) return true;
			if (Failed) return false;
			Release();
			if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &Texture, NULL)) || FAILED(Texture->GetSurfaceLevel(0, &Surface))) {
				Release(); Failed = true;
				Logger::Log("[ERROR] UNOFFICIAL %s: its %ux%u target could not be made: off for this session", what, w, h);
				return false;
			}
			Width = w; Height = h; Format = f;
			return true;
		}
	};
	inline bool TextureDesc(IDirect3DBaseTexture9* t, D3DSURFACE_DESC& desc) {
		return t && t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &desc));
	}
	inline const D3DXVECTOR4* Constant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	// --- the shading normals (DeferredReflections, DeferredRRGuides) -------------------------------------------------------------------
	inline Target Normals;
	inline IDirect3DPixelShader9* NormalsPS = nullptr;  inline bool NormalsPSFailed = false;
	inline UInt32 NormalsFrame = 0xFFFFFFFFu;  inline IDirect3DBaseTexture9* NormalsFrom = nullptr;  inline bool NormalsMade = false;
	inline UInt32 NormalsRuns = 0;
	// normals: the effect's TESR_NormalsBuffer (view space); depth: TESR_DepthBuffer (linear 0-1 of far). Returns the texture to read in the
	// normals' place this frame (made once a frame, shared by Reflections and the RR guides), or normals itself when it cannot be made.
	inline IDirect3DBaseTexture9* ShadingNormals(IDirect3DDevice9* d, IDirect3DBaseTexture9* normals, IDirect3DBaseTexture9* depth) {
		if (!d || !normals || !depth || NormalsPSFailed || !CapturedNormals::Texture || CapturedNormals::Data.x <= 0.0f) return normals;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame == NormalsFrame && normals == NormalsFrom) return NormalsMade ? (IDirect3DBaseTexture9*)Normals.Texture : normals;
		NormalsFrame = frame; NormalsFrom = normals; NormalsMade = false;
		D3DSURFACE_DESC desc = {};
		const D3DXMATRIX* view = (const D3DXMATRIX*)Constant("TESR_ViewTransform");
		const D3DXMATRIX* proj = (const D3DXMATRIX*)Constant("TESR_ProjectionTransform");
		const D3DXVECTOR4* camera = Constant("TESR_CameraData");
		const D3DXVECTOR4* scale = Constant("TESR_RenderScale");
		if (!view || !proj || !camera || !TextureDesc(normals, desc) || !Normals.Ensure(d, desc.Width, desc.Height, desc.Format, "DeferredReflections / DeferredRRGuides shading normals")) return normals;
		if (!NormalsPS && !(NormalsPS = CapturedNormals::FullScreenShader(d, FdIndirectShaders::kShadingNormalsPS, "UNOFFICIAL FdIndirect shading normals", NormalsPSFailed))) return normals;
		static GpuTimer timer("FD shading normals");
		GpuProfileScope gpu(timer, d);
		float saved[7][4] = {};
		d->GetPixelShaderConstantF(0, &saved[0][0], 7);
		const float c[7][4] = {
			{ 1.0f / proj->_11, 1.0f / proj->_22, CapturedNormals::Data.w, camera->y },
			{ scale ? scale->x : 1.0f, scale ? scale->y : 1.0f, scale ? scale->z : 1.0f, scale ? scale->w : 1.0f },
			{ view->_11, view->_12, view->_13, view->_14 }, { view->_21, view->_22, view->_23, view->_24 },
			{ view->_31, view->_32, view->_33, view->_34 }, { view->_41, view->_42, view->_43, view->_44 },
			{ CapturedNormals::Data.y, CapturedNormals::Data.z, 0.0f, 0.0f } };
		d->SetPixelShaderConstantF(0, &c[0][0], 7);
		// stage 3 (FullScreenPass takes three): the depth, point / clamp, put back after
		IDirect3DBaseTexture9* tex3 = nullptr; d->GetTexture(3, &tex3);
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(3, kSS[i], &savedS[i]))) savedS[i] = wantS[i]; if (savedS[i] != wantS[i]) d->SetSamplerState(3, kSS[i], wantS[i]); }
		d->SetTexture(3, depth);
		CapturedNormals::FullScreenPass(d, Normals.Surface, NormalsPS, normals, false, D3DBLEND_ONE, D3DBLEND_ZERO, CapturedNormals::Texture, CapturedNormals::MaterialTexture);
		d->SetTexture(3, tex3); if (tex3) tex3->Release();
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[i] != wantS[i]) d->SetSamplerState(3, kSS[i], savedS[i]);
		d->SetPixelShaderConstantF(0, &saved[0][0], 7);
		NormalsMade = true;
		if (!NormalsRuns++) Logger::Log("UNOFFICIAL FdIndirect: the shading normals made (%ux%u): the G-buffer's normal-mapped normal where the normals buffer shows the captured surface (least cosine %.2f)",
			desc.Width, desc.Height, CapturedNormals::Data.w);
		return Normals.Texture;
	}

	// --- Reflections' material copy (DeferredReflections) ------------------------------------------------------------------------------
	inline IDirect3DPixelShader9* ReflPS = nullptr;  inline bool ReflPSFailed = false;  inline bool ReflLogged = false;
	// In place of CapturedNormals::MergeIntoReflMaterial (same target, same blend). False: not run (the caller then runs the old merge or none).
	inline bool ReflMaterial(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		if (!d || !target || ReflPSFailed || !ReflectionsOn()) return false;
		if (!ReflPS && !(ReflPS = CapturedNormals::FullScreenShader(d, FdIndirectShaders::kReflMaterialPS, "UNOFFICIAL DeferredReflections material", ReflPSFailed))) return false;
		CapturedNormals::FullScreenPass(d, target, ReflPS, CapturedNormals::MaterialTexture, true, D3DBLEND_INVDESTALPHA, D3DBLEND_DESTALPHA);
		if (!ReflLogged) { ReflLogged = true; Logger::Log("UNOFFICIAL DeferredReflections: the reflections' material from G-buffer v2 (shiny objects reflect by their smoothness whatever way they face; plants, skin, grass, hair, eyes rough) and the shading normal in place of the smooth one"); }
		return true;
	}

	// --- AmbientLight's albedo (DeferredALAlbedo) --------------------------------------------------------------------------------------
	inline Target ALAlbedoTarget;
	inline IDirect3DPixelShader9* ALPS = nullptr;  inline bool ALPSFailed = false;  inline float ALLoggedGain = -1.0f;
	inline UInt32 ALFrame = 0xFFFFFFFFu;  inline bool ALMade = false;
	// Returns the texture AmbientLight binds as NVR_ALAlbedo this frame: the metal-split one, or CapturedNormals::AlbedoTexture (switch off,
	// renderer off, or it could not be made).
	inline IDirect3DBaseTexture9* ALAlbedo(IDirect3DDevice9* d) {
		IDirect3DBaseTexture9* plain = CapturedNormals::AlbedoTexture;
		if (!d || !plain || ALPSFailed || !ALAlbedoOn()) return plain;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame == ALFrame) return ALMade ? (IDirect3DBaseTexture9*)ALAlbedoTarget.Texture : plain;
		ALFrame = frame; ALMade = false;
		D3DSURFACE_DESC desc = {};
		if (!TextureDesc(plain, desc) || !ALAlbedoTarget.Ensure(d, desc.Width, desc.Height, D3DFMT_A16B16G16R16F, "DeferredALAlbedo")) return plain;
		if (!ALPS && !(ALPS = CapturedNormals::FullScreenShader(d, FdIndirectShaders::kALAlbedoPS, "UNOFFICIAL DeferredALAlbedo", ALPSFailed))) return plain;
		float gain = TheSettingManager->GetSettingF(kSection, "DeferredALAlbedoGain");
		gain = gain < 0.25f ? 0.25f : gain > 3.0f ? 3.0f : gain;
		static GpuTimer timer("FD AL albedo");
		GpuProfileScope gpu(timer, d);
		float saved[4] = {};
		d->GetPixelShaderConstantF(0, saved, 1);
		const float g[4] = { gain, 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(0, g, 1);
		CapturedNormals::FullScreenPass(d, ALAlbedoTarget.Surface, ALPS, plain, false, D3DBLEND_ONE, D3DBLEND_ZERO, CapturedNormals::MaterialTexture);
		d->SetPixelShaderConstantF(0, saved, 1);
		ALMade = true;
		if (gain != ALLoggedGain) { ALLoggedGain = gain; Logger::Log("UNOFFICIAL DeferredALAlbedo: AmbientLight's bounce takes the G-buffer's albedo x (1 - metal) x %.2f (DeferredALAlbedoGain)", gain); }
		return ALAlbedoTarget.Texture;
	}
}
