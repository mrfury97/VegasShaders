#pragma once
// UNOFFICIAL 2026-11-15 full deferred renderer, phase 6: indirect light and the RT / DLSS contract read G-buffer v2
// (docs/next/FULL-DEFERRED-RENDERER-PLAN.md 4.6 and phase 6; results, numbers and bench requests: docs/next/FD-GI-RESULTS.md).
//
// Every switch below is live, test only, off by default, and does something only while [Main.Main.Quality] DeferredRenderer is on at
// startup (2026-11-28: always, the renderer is locked to mode 2 since the upstream 1.1.6 merge) and the G-buffer holds this
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
#include "FdIndirectShaders.h"
#include "RtAmbient.h"

namespace FdIndirect {
	inline const char* kSection = "Main.Main.Quality";

	// G-buffer v2 compiled in, the captured targets alive.
	inline bool Base() { return CapturedNormals::V2CompiledIn() && !CapturedNormals::Failed; }
	inline bool Key(const char* key) { return TheSettingManager->GetSettingI(kSection, key) != 0; }
	inline bool MaterialNow() { return CapturedNormals::MaterialTexture && CapturedNormals::MaterialData.x > 0.5f; }
	inline bool ReflectionsOn() { return Base() && MaterialNow() && Key("DeferredReflections"); }
	// UNOFFICIAL 2026-11-19: RT GI always splits diffuse albedo by (1 - metal) so metals receive zero diffuse GI (reflections carry metal indirect).
	inline bool ALAlbedoOn() { return Base() && MaterialNow() && CapturedNormals::AlbedoTexture && CapturedNormals::AlbedoData.x > 0.5f && (Key("DeferredALAlbedo") || RtAmbient::Current.GI); }
	inline bool RRGuidesOn() { return Base() && MaterialNow() && Key("DeferredRRGuides"); }
	// UNOFFICIAL 2026-11-20 UnifiedReflections (live, test only, off; docs/DESIGN.md "Unified reflections design" step 1): with the renderer in mode 2
	// the Reflections effect reads its material (COLOR2: roughness, metal, model, specular bit; COLOR1: the metal's colour; COLOR3: is the
	// record the visible surface) straight from the G-buffer instead of MaterialAware's copy, and always the shading normals below. True only
	// in a frame whose three targets hold this frame's records (the renderer is always mode 2 since the 1.1.6 merge).
	inline bool UnifiedReflectionsOn() {
		return Base() && MaterialNow() && CapturedNormals::AlbedoTexture && CapturedNormals::AlbedoData.x > 0.5f &&
			CapturedNormals::Texture && CapturedNormals::Data.x > 0.0f && Key("UnifiedReflections");
	}
	// The D3D9 technique's half (Effects/Reflections.fx.hlsl REFL_GBUFFER_ON): compiled in only when the switch is on at startup with the
	// renderer in mode 2, so that off the effect's preprocessed source is exactly what it was. The Vulkan passes follow the switch live.
	inline int UnifiedStartup = -1;
	inline bool UnifiedCompiledIn() {
		if (UnifiedStartup < 0) UnifiedStartup = CapturedNormals::V2CompiledIn() && Key("UnifiedReflections") ? 1 : 0;
		return UnifiedStartup == 1;
	}
	// UNOFFICIAL 2026-11-18/19 DeferredBumpedNormals (live; default off in this fork, on upstream since 1.1.1: the owner decides;
	// docs/bumped-normals-study.md): feeds the G-buffer's normal-mapped
	// shading normals (with distance fade and grazing silhouette softening) to Reflections (SSR, Vulkan & RT), WetnessReflections, WetWorld,
	// and DLSS Ray Reconstruction guide normals. AO, bounce light and shadows keep the smooth normals buffer so they never shimmer or self-shadow.
	inline bool BumpedNormalsOn() { return Base() && MaterialNow() && Key("DeferredBumpedNormals"); }
	inline constexpr float kBumpFadeStart = 1500.0f;	// game units (~21 m): full normal-map bump detail up to here
	inline constexpr float kBumpFadeEnd = 4000.0f;		// game units (~57 m): smoothly relaxed to smooth normal past here (no distant aliasing)

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
			d->ColorFill(Surface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));	// clean border outside active RenderRect on first make
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

	// --- the shading normals (DeferredReflections, DeferredRRGuides, DeferredBumpedNormals) ---------------------------------------------
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
		if (!view || !proj || !camera || !TextureDesc(normals, desc) || !Normals.Ensure(d, desc.Width, desc.Height, desc.Format, "DeferredReflections / DeferredRRGuides / DeferredBumpedNormals shading normals")) return normals;
		if (!NormalsPS && !(NormalsPS = CapturedNormals::FullScreenShader(d, FdIndirectShaders::kShadingNormalsPS, "UNOFFICIAL FdIndirect shading normals", NormalsPSFailed))) return normals;
		static GpuTimer timer("FD shading normals");
		GpuProfileScope gpu(timer, d);
		IDirect3DBaseTexture9* vmDepth = CapturedNormals::VmCapturedThisFrame(frame) ? CapturedNormals::ViewModelDepthTexture() : nullptr;
		float saved[8][4] = {};
		d->GetPixelShaderConstantF(0, &saved[0][0], 8);
		const float c[8][4] = {
			{ 1.0f / proj->_11, 1.0f / proj->_22, CapturedNormals::Data.w, camera->y },
			{ scale ? scale->x : 1.0f, scale ? scale->y : 1.0f, scale ? scale->z : 1.0f, scale ? scale->w : 1.0f },
			{ view->_11, view->_12, view->_13, view->_14 }, { view->_21, view->_22, view->_23, view->_24 },
			{ view->_31, view->_32, view->_33, view->_34 }, { view->_41, view->_42, view->_43, view->_44 },
			{ CapturedNormals::Data.y, CapturedNormals::Data.z, kBumpFadeStart, kBumpFadeEnd },
			{ vmDepth ? 1.0f / CapturedNormals::ViewModelFovScale() : 0.0f, CapturedNormals::DepthReversed() ? 1.0f : 0.0f, 0.0f, 0.0f } };
		d->SetPixelShaderConstantF(0, &c[0][0], 8);
		CapturedNormals::FullScreenPass(d, Normals.Surface, NormalsPS, normals, false, D3DBLEND_ONE, D3DBLEND_ZERO, CapturedNormals::Texture, CapturedNormals::MaterialTexture, depth, vmDepth);
		d->SetPixelShaderConstantF(0, &saved[0][0], 8);
		NormalsMade = true;
		if (!NormalsRuns++) Logger::Log("UNOFFICIAL FdIndirect: the shading normals made (%ux%u): the G-buffer's normal-mapped normal where the normals buffer shows the captured surface (least cosine %.2f, distance fade %.0f-%.0f)",
			desc.Width, desc.Height, CapturedNormals::Data.w, kBumpFadeStart, kBumpFadeEnd);
		return Normals.Texture;
	}

	// --- Reflections' material copy (DeferredReflections) ------------------------------------------------------------------------------
	inline IDirect3DPixelShader9* ReflPS = nullptr;  inline bool ReflPSFailed = false;  inline bool ReflLogged = false;
	// In place of CapturedNormals::MergeIntoReflMaterial (same target, same blend). False: not run (the caller then runs the old merge or none).
	inline bool ReflMaterial(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		if (!d || !target || ReflPSFailed || !ReflectionsOn()) return false;
		if (!ReflPS && !(ReflPS = CapturedNormals::FullScreenShader(d, FdIndirectShaders::kReflMaterialPS, "UNOFFICIAL DeferredReflections material", ReflPSFailed))) return false;
		IDirect3DBaseTexture9* vmDepth = CapturedNormals::ViewModelDepthTexture();
		const bool clipVm = vmDepth && !CapturedNormals::VmCapturedThisFrame(TheFrameRateManager->FrameNumber);
		float saved[4] = {};
		d->GetPixelShaderConstantF(0, saved, 1);
		const float q[4] = { clipVm ? 1.0f : 0.0f, CapturedNormals::DepthReversed() ? 1.0f : 0.0f, 0.0f, 0.0f };
		d->SetPixelShaderConstantF(0, q, 1);
		CapturedNormals::FullScreenPass(d, target, ReflPS, CapturedNormals::MaterialTexture, true, D3DBLEND_INVDESTALPHA, D3DBLEND_DESTALPHA, clipVm ? vmDepth : nullptr);
		d->SetPixelShaderConstantF(0, saved, 1);
		if (!ReflLogged) { ReflLogged = true; Logger::Log("UNOFFICIAL DeferredReflections: the reflections' material from G-buffer v2 (shiny objects reflect by their smoothness whatever way they face; plants, skin, grass, hair, eyes rough) and the shading normal in place of the smooth one"); }
		return true;
	}

	// --- AmbientLight's albedo (DeferredALAlbedo) --------------------------------------------------------------------------------------
	inline Target ALAlbedoTarget;
	inline IDirect3DPixelShader9* ALPS = nullptr;  inline bool ALPSFailed = false;  inline float ALLoggedGain = -1.0f;
	inline UInt32 ALFrame = 0xFFFFFFFFu;  inline bool ALMade = false;
	// Returns the texture AmbientLight binds as NVR_ALAlbedo this frame: depth-validated against TESR_DepthBuffer (and metal-split
	// when ALAlbedoOn() is true), or CapturedNormals::AlbedoTexture if depth validation & ALAlbedoOn are unavailable.
	inline IDirect3DBaseTexture9* ALAlbedo(IDirect3DDevice9* d, IDirect3DBaseTexture9* depth = nullptr) {
		IDirect3DBaseTexture9* plain = CapturedNormals::AlbedoTexture;
		const bool metalSplit = ALAlbedoOn();
		const bool depthValidate = depth && CapturedNormals::Texture && CapturedNormals::Data.x > 0.0f;
		if (!d || !plain || ALPSFailed || (!metalSplit && !depthValidate)) return plain;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame == ALFrame) return ALMade ? (IDirect3DBaseTexture9*)ALAlbedoTarget.Texture : plain;
		ALFrame = frame; ALMade = false;
		D3DSURFACE_DESC desc = {};
		if (!TextureDesc(plain, desc) || !ALAlbedoTarget.Ensure(d, desc.Width, desc.Height, D3DFMT_A16B16G16R16F, "DeferredALAlbedo")) return plain;
		if (!ALPS && !(ALPS = CapturedNormals::FullScreenShader(d, FdIndirectShaders::kALAlbedoPS, "UNOFFICIAL DeferredALAlbedo", ALPSFailed))) return plain;
		float gain = metalSplit ? TheSettingManager->GetSettingF(kSection, "DeferredALAlbedoGain") : 1.0f;
		if (metalSplit) gain = gain < 0.25f ? 0.25f : gain > 3.0f ? 3.0f : gain;
		static GpuTimer timer("FD AL albedo");
		GpuProfileScope gpu(timer, d);
		const D3DXMATRIX* proj = (const D3DXMATRIX*)Constant("TESR_ProjectionTransform");
		const D3DXVECTOR4* camera = Constant("TESR_CameraData");
		const D3DXVECTOR4* scale = Constant("TESR_RenderScale");
		const bool canValidate = depthValidate && proj && camera && fabs(proj->_11) > 1.0e-6f && fabs(proj->_22) > 1.0e-6f;
		IDirect3DBaseTexture9* vmDepth = canValidate ? CapturedNormals::ViewModelDepthTexture() : nullptr;
		const bool vmCaptured = vmDepth && CapturedNormals::VmCapturedThisFrame(frame);
		const float vmScale = vmDepth ? (1.0f / CapturedNormals::ViewModelFovScale()) * (vmCaptured ? 1.0f : -1.0f) : 0.0f;
		float saved[3][4] = {};
		d->GetPixelShaderConstantF(0, &saved[0][0], 3);
		const float c[3][4] = {
			{ gain, metalSplit ? 1.0f : 0.0f, CapturedNormals::Data.y, CapturedNormals::Data.z },
			{ proj ? 1.0f / proj->_11 : 1.0f, proj ? 1.0f / proj->_22 : 1.0f, vmScale, camera ? camera->y : 1.0f },
			{ scale ? scale->z : 1.0f, scale ? scale->w : 1.0f, CapturedNormals::DepthReversed() ? 1.0f : 0.0f, canValidate ? 1.0f : 0.0f }
		};
		d->SetPixelShaderConstantF(0, &c[0][0], 3);
		CapturedNormals::FullScreenPass(d, ALAlbedoTarget.Surface, ALPS, plain, false, D3DBLEND_ONE, D3DBLEND_ZERO,
			CapturedNormals::MaterialTexture ? CapturedNormals::MaterialTexture : plain,
			CapturedNormals::Texture ? CapturedNormals::Texture : plain,
			depth ? depth : plain,
			vmDepth);
		d->SetPixelShaderConstantF(0, &saved[0][0], 3);
		ALMade = true;
		if (metalSplit && gain != ALLoggedGain) { ALLoggedGain = gain; Logger::Log("UNOFFICIAL DeferredALAlbedo: AmbientLight's bounce takes the G-buffer's albedo x (1 - metal) x %.2f (DeferredALAlbedoGain)", gain); }
		return ALAlbedoTarget.Texture;
	}
}
