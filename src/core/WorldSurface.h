#pragma once

// UNOFFICIAL 2026-11-24 WorldSurface (Shaders/WorldSurface.hlsl): world-space micro-detail grain and physical
// weathering (Mojave crevice dust, exposed metal rust, waterline dampness/mud/algae, and mossy rock growth)
// applied to static geometry (class 1) and terrain (class 4) at the end of DeferredSpan, right before
// DeferredSun / DeferredLamps / FlashlightDeferred. Modifies both the G-buffer (COLOR1 albedo, COLOR2 material/normal)
// and the baked ambient in COLOR0 so every light source, shadow, and reflection reacts naturally.

#include <algorithm>
#include <cmath>
#include <cstring>
#include "GpuProfiler.h"
#include "CapturedNormals.h"
#include "DeferredDepth.h"
#include "DeferredRenderer.h"
#include "LinearScene.h"
#include "PassSums.h"
#include "RtLightUnits.h"
#include "ShaderRecord.h"

namespace WorldSurface {

	inline ShaderRecordPixel* Pass = nullptr;
	inline bool PassFailed = false;
	inline IDirect3DTexture9* CopyTex[3] = {};
	inline IDirect3DSurface9* Copy[3] = {};
	inline D3DFORMAT CopyFmt[3] = {};
	inline UINT CopyW = 0, CopyH = 0;
	inline IDirect3DStateBlock9* State = nullptr;
	inline float RainWetness = 0.0f;
	inline double LastTime = -1.0;

	inline UINT BitDepth(D3DFORMAT f) {
		switch (f) {
		case D3DFMT_A16B16G16R16F:
		case D3DFMT_A16B16G16R16:
		case D3DFMT_G32R32F:
			return 64;
		case D3DFMT_A32B32G32R32F:
			return 128;
		case D3DFMT_A8R8G8B8:
		case D3DFMT_X8R8G8B8:
		case D3DFMT_A2R10G10B10:
		case D3DFMT_A2B10G10R10:
		case D3DFMT_G16R16F:
		case D3DFMT_R32F:
			return 32;
		default:
			return 0;
		}
	}

	inline void ReleaseCopies() {
		for (int i = 0; i < 3; i++) {
			if (Copy[i]) { Copy[i]->Release(); Copy[i] = nullptr; }
			if (CopyTex[i]) { CopyTex[i]->Release(); CopyTex[i] = nullptr; }
			CopyFmt[i] = D3DFMT_UNKNOWN;
		}
		CopyW = CopyH = 0;
	}

	inline bool EnsureCopies(IDirect3DDevice9* d, UINT w, UINT h, const D3DFORMAT fmts[3]) {
		if (CopyTex[0] && CopyTex[1] && CopyTex[2] && CopyW == w && CopyH == h &&
			CopyFmt[0] == fmts[0] && CopyFmt[1] == fmts[1] && CopyFmt[2] == fmts[2]) return true;
		ReleaseCopies();
		for (int i = 0; i < 3; i++) {
			if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmts[i], D3DPOOL_DEFAULT, &CopyTex[i], NULL)) ||
				FAILED(CopyTex[i]->GetSurfaceLevel(0, &Copy[i]))) {
				ReleaseCopies();
				Logger::Log("[ERROR] UNOFFICIAL WorldSurface: could not create copy targets (%ux%u): world surface detail disabled", w, h);
				PassFailed = true;
				return false;
			}
			CopyFmt[i] = fmts[i];
		}
		CopyW = w; CopyH = h;
		return true;
	}

	inline void Load() {
		if (!DeferredRenderer::Mode() || !CapturedNormals::V2CompiledIn() || Pass || PassFailed) return;
		ShaderTemplate tmpl = {};
		tmpl.Name = "WorldSurface";
		static D3DXMACRO overrides[3] = {
			{ "CAPTURED_NORMALS_ON", "4" },
			{ "GBUFFER_V2", "1" },
			{ NULL, NULL }
		};
		Pass = (ShaderRecordPixel*)ShaderRecord::LoadShader("WorldSurface.pso", NULL, tmpl, "Deferred\\WorldSurface\\", overrides);
		if (Pass) {
			Pass->ClearSamplers = false;
			Logger::Log("UNOFFICIAL WorldSurface: loaded (Shaders\\WorldSurface.hlsl)");
		} else {
			PassFailed = true;
			Logger::Log("[ERROR] UNOFFICIAL WorldSurface: Shaders\\WorldSurface.hlsl did not load: world surface detail disabled");
		}
	}

	inline void Apply(IDirect3DDevice9* d) {
		if (!d || !Pass || PassFailed || !DeferredRenderer::Live || !CapturedNormals::V2Compiled || !CapturedNormals::Drawn) return;
		if (!CapturedNormals::AlbedoSurface || !CapturedNormals::MaterialSurface || !CapturedNormals::Texture) return;
		if (!TheSettingManager || !TheShaderManager) return;

		if (TheSettingManager->GetSettingI("Shaders.WorldSurface.Status", "Enabled") == 0) return;

		const float strength = (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.WorldSurface.Main", "Strength"));
		if (strength <= 1.0e-4f) return;

		const float detail    = (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.WorldSurface.Main", "Detail")) * strength;
		const float dust      = TheSettingManager->GetSettingI("Shaders.WorldSurface.Dust", "Enabled") != 0
			? (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.WorldSurface.Dust", "Strength")) * strength : 0.0f;
		const float rust      = TheSettingManager->GetSettingI("Shaders.WorldSurface.Rust", "Enabled") != 0
			? (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.WorldSurface.Rust", "Strength")) * strength : 0.0f;
		const float waterline = TheSettingManager->GetSettingI("Shaders.WorldSurface.Waterline", "Enabled") != 0
			? (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.WorldSurface.Waterline", "Strength")) * strength : 0.0f;
		const float moss      = TheSettingManager->GetSettingI("Shaders.WorldSurface.Moss", "Enabled") != 0
			? (std::max)(0.0f, TheSettingManager->GetSettingF("Shaders.WorldSurface.Moss", "Strength")) * strength : 0.0f;
		const int debug       = TheSettingManager->GetSettingI("Shaders.WorldSurface.Main", "DebugView");
		const float maxDist   = std::clamp(TheSettingManager->GetSettingF("Shaders.WorldSurface.Main", "Distance"), 200.0f, 10000.0f);

		if (detail <= 1.0e-4f && dust <= 1.0e-4f && rust <= 1.0e-4f && waterline <= 1.0e-4f && moss <= 1.0e-4f && debug == 0) return;

		IDirect3DBaseTexture9* noiseVol = TheTextureManager
			? TheTextureManager->GetFileTexture("Data\\Textures\\Effects\\NoiseVolume.dds", TextureRecord::TextureRecordType::VolumeBuffer)
			: nullptr;
		if (!noiseVol) {
			static bool warnedNoise = false;
			if (!warnedNoise) {
				warnedNoise = true;
				Logger::Log("[ERROR] UNOFFICIAL WorldSurface: Data\\Textures\\Effects\\NoiseVolume.dds could not be loaded; pass skipped");
			}
			return;
		}

		IDirect3DSurface9* target = nullptr;
		if (FAILED(d->GetRenderTarget(0, &target)) || !target) return;
		D3DSURFACE_DESC t = {}, aDesc = {}, mDesc = {};
		if (FAILED(target->GetDesc(&t)) || FAILED(CapturedNormals::AlbedoSurface->GetDesc(&aDesc)) ||
			FAILED(CapturedNormals::MaterialSurface->GetDesc(&mDesc))) {
			target->Release();
			return;
		}
		const UINT bppT = BitDepth(t.Format), bppA = BitDepth(aDesc.Format), bppM = BitDepth(mDesc.Format);
		if (t.Width != aDesc.Width || t.Height != aDesc.Height || t.Width != mDesc.Width || t.Height != mDesc.Height ||
			t.MultiSampleType != D3DMULTISAMPLE_NONE || aDesc.MultiSampleType != D3DMULTISAMPLE_NONE || mDesc.MultiSampleType != D3DMULTISAMPLE_NONE ||
			bppT == 0 || bppT != bppA || bppT != bppM) {
			target->Release();
			return;
		}

		const D3DFORMAT copyFmts[3] = { t.Format, aDesc.Format, mDesc.Format };
		if (!EnsureCopies(d, t.Width, t.Height, copyFmts)) { target->Release(); return; }
		if (!State && FAILED(d->CreateStateBlock(D3DSBT_ALL, &State))) { target->Release(); return; }

		const bool linearDepth = DeferredDepth::EnsureLinear(d);
		const bool rawDepth = !linearDepth && DeferredDepth::Ready() && DeferredDepth::Texture != nullptr;
		if (!DeferredDepth::Ready() && !linearDepth) DeferredDepth::FillFrame(d);

		IDirect3DTexture9* depthTex = nullptr;
		float depthMode = 0.0f;
		D3DXVECTOR4 camFrame[6] = {};
		memcpy(camFrame, DeferredDepth::Frame, sizeof(camFrame));
		if (linearDepth && DeferredDepth::LinearTexture) {
			depthTex = DeferredDepth::LinearTexture;
			depthMode = 1.0f;
		} else if (rawDepth) {
			depthTex = DeferredDepth::Texture;
			depthMode = 2.0f;
		} else if (TheTextureManager && TheTextureManager->DepthTexture && TheRenderManager) {
			D3DSURFACE_DESC dDesc = {};
			if (SUCCEEDED(TheTextureManager->DepthTexture->GetLevelDesc(0, &dDesc)) && dDesc.Width > 0 && dDesc.Height > 0) {
				TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);
				depthTex = TheTextureManager->DepthTexture;
				depthMode = 2.0f;
				camFrame[5].z = 1.0f / (float)dDesc.Width;
				camFrame[5].w = 1.0f / (float)dDesc.Height;
			}
		}
		if (camFrame[5].z <= 0.0f || camFrame[5].w <= 0.0f) {
			camFrame[5].z = 1.0f / (float)t.Width;
			camFrame[5].w = 1.0f / (float)t.Height;
		}
		D3DVIEWPORT9 vp = {}; d->GetViewport(&vp);
		RECT box = {
			(std::max)(0L, (LONG)vp.X),
			(std::max)(0L, (LONG)vp.Y),
			(std::min)((LONG)t.Width, (LONG)(vp.X + vp.Width)),
			(std::min)((LONG)t.Height, (LONG)(vp.Y + vp.Height))
		};
		if (box.right <= box.left || box.bottom <= box.top) { target->Release(); return; }

		const double now = TheFrameRateManager ? TheFrameRateManager->Time : 0.0;
		const float dt = (LastTime >= 0.0 && now > LastTime && now - LastTime < 2.0) ? (float)(now - LastTime) : 0.0f;
		LastTime = now;
		const auto& gs = TheShaderManager->GameState;
		if (gs.isExterior && gs.isRainy) RainWetness = (std::min)(1.0f, RainWetness + dt / 15.0f);
		else RainWetness = (std::max)(0.0f, RainWetness - dt / 45.0f);

		static GpuTimer timer("World surface detail");
		GpuProfileScope gpu(timer, d);

		d->StretchRect(target, &box, Copy[0], &box, D3DTEXF_NONE);
		d->StretchRect(CapturedNormals::AlbedoSurface, &box, Copy[1], &box, D3DTEXF_NONE);
		d->StretchRect(CapturedNormals::MaterialSurface, &box, Copy[2], &box, D3DTEXF_NONE);

		IDirect3DSurface9* extra[3] = {};
		for (DWORD i = 1; i < 4; i++) d->GetRenderTarget(i, &extra[i - 1]);
		State->Capture();

		d->SetRenderTarget(0, target);
		d->SetRenderTarget(1, CapturedNormals::AlbedoSurface);
		d->SetRenderTarget(2, CapturedNormals::MaterialSurface);
		d->SetRenderTarget(3, nullptr);

		const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
		d->SetViewport(&full);

		const struct { D3DRENDERSTATETYPE s; DWORD v; } kStates[] = {
			{ D3DRS_ZENABLE, D3DZB_FALSE }, { D3DRS_ZWRITEENABLE, FALSE },
			{ D3DRS_STENCILENABLE, FALSE }, { D3DRS_TWOSIDEDSTENCILMODE, FALSE },
			{ D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE },
			{ D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
			{ D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
			{ D3DRS_FOGENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
			{ D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_COLORWRITEENABLE1, 0xF },
			{ D3DRS_COLORWRITEENABLE2, 0xF }, { D3DRS_COLORWRITEENABLE3, 0 }
		};
		for (const auto& k : kStates) d->SetRenderState(k.s, k.v);

		d->SetVertexShader(nullptr);
		d->SetPixelShader(Pass->ShaderHandle);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, CopyTex[0]);
		d->SetTexture(1, CopyTex[1]);
		d->SetTexture(2, CopyTex[2]);
		d->SetTexture(3, CapturedNormals::Texture);
		d->SetTexture(4, depthTex);
		for (DWORD s = 0; s < 5; s++) {
			d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		}
		d->SetSamplerState(4, D3DSAMP_MIPMAPLODBIAS, 0);

		d->SetTexture(5, noiseVol);
		d->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		d->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		d->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		d->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		d->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		d->SetSamplerState(5, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
		d->SetSamplerState(5, D3DSAMP_SRGBTEXTURE, FALSE);
		d->SetSamplerState(5, D3DSAMP_MIPMAPLODBIAS, 0);

		d->SetPixelShaderConstantF(0, &camFrame[0].x, 6);

		const D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
		const float world[4] = {
			_finite(camPos.x) ? camPos.x : 0.0f,
			_finite(camPos.y) ? camPos.y : 0.0f,
			_finite(camPos.z) ? camPos.z : 0.0f,
			gs.isExterior ? 1.0f : 0.0f
		};
		d->SetPixelShaderConstantF(6, world, 1);

		const float weights[4] = { detail, dust, rust, moss };
		d->SetPixelShaderConstantF(7, weights, 1);

		float waterRelZ = -100000.0f;
		{
			auto wIt = TheShaderManager->ConstantsTable.find("TESR_WaterSettings");
			if (wIt != TheShaderManager->ConstantsTable.end() && wIt->second) {
				const float wh = wIt->second->x;
				if (_finite(wh) && fabsf(wh) < 50000.0f && _finite(camPos.z)) waterRelZ = wh - camPos.z;
			}
		}
		const float water[4] = { waterRelZ, waterline, RainWetness, debug ? 1.0f : 0.0f };
		d->SetPixelShaderConstantF(8, water, 1);

		const float fade[4] = { maxDist * 0.65f, maxDist, (std::min)(maxDist * 0.70f, 1500.0f), gs.isSnow ? 1.0f : 0.0f };
		d->SetPixelShaderConstantF(9, fade, 1);

		const float gamma = LinearScene::Gamma > 1.0f ? LinearScene::Gamma : 2.2f;
		const float gammaVec[4] = { gamma, 1.0f / gamma, LinearScene::Linear ? 1.0f : 0.0f, depthMode };
		d->SetPixelShaderConstantF(10, gammaVec, 1);

		D3DXVECTOR4 sh[9] = {};
		D3DXVECTOR4 amb = { 0.0f, 0.0f, 0.0f, 0.0f };
		if (gs.isExterior) {
			const D3DXVECTOR4& sa = TheShaderManager->ShaderConst.sunAmbient;
			const D3DXVECTOR3 flat = RtLightUnits::DecodeColor(sa.x, sa.y, sa.z, RtLightUnits::AmbientScale());
			amb = D3DXVECTOR4(flat.x, flat.y, flat.z, RtLightUnits::SkyScale());
			auto found = TheShaderManager->ConstantsTable.find("TESR_SkyIrradianceObjects");
			if (found != TheShaderManager->ConstantsTable.end() && found->second) memcpy(sh, found->second, sizeof(sh));
		}
		d->SetPixelShaderConstantF(11, &amb.x, 1);
		d->SetPixelShaderConstantF(12, &sh[0].x, 9);

		const float x0 = (float)box.left - 0.5f, y0 = (float)box.top - 0.5f;
		const float x1 = (float)box.right - 0.5f, y1 = (float)box.bottom - 0.5f;
		const float u0 = (float)box.left / t.Width, v0 = (float)box.top / t.Height;
		const float u1 = (float)box.right / t.Width, v1 = (float)box.bottom / t.Height;
		const float quad[4][6] = {
			{ x0, y0, 0.0f, 1.0f, u0, v0 },
			{ x1, y0, 0.0f, 1.0f, u1, v0 },
			{ x0, y1, 0.0f, 1.0f, u0, v1 },
			{ x1, y1, 0.0f, 1.0f, u1, v1 }
		};
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));

		d->SetRenderTarget(0, target);
		for (DWORD i = 1; i < 4; i++) {
			d->SetRenderTarget(i, extra[i - 1]);
			if (extra[i - 1]) extra[i - 1]->Release();
		}
		State->Apply();
		if (PassSums::AfterStateBlock) PassSums::AfterStateBlock(d);
		target->Release();
	}

}
