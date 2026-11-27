#pragma once

// UNOFFICIAL Phase 2 MaterialSSS (Task 2, docs/next/MATERIAL-SSS-PLAN.md):
// Production integration: Chan rough diffuse, warm Mojave bounce, cloth sheen & shadow-gated backlit transmission
// now run inside DeferredSun (DeferredSun.comp + DeferredSun.hlsl) with zero extra passes when DebugVar2 >= 0.5.
// This post-pass handles ONLY optional sparse 8-tap IGN SS Burley scatter on translucent solids (models 11..14)
// and GBufferV2 class visualization:
//   DebugVar2 = 0: off
//   DebugVar2 = 1: in-pass DeferredSun only (0 extra passes)
//   DebugVar2 = 2: in-pass DeferredSun + sparse translucent-solid (models 11..14) SS Burley scatter
//   DebugVar2 = 3: in-pass DeferredSun + sparse solids/foliage/cloth close-range SS scatter
//   DebugVar2 = 4: GBufferV2 material & SSS profile debug visualizer (1..15)

#include <algorithm>
#include <cmath>
#include <string>

#include "CapturedNormals.h"
#include "DeferredDepth.h"
#include "GpuProfiler.h"
#include "LandLight.h"
#include "LinearScene.h"
#include "Paths.h"
#include "RenderManager.h"
#include "SettingManager.h"
#include "ShaderManager.h"
#include "TemporalBackbone.h"

namespace MaterialSSSProto {
	inline int EffectiveMode() {
		if (!TheSettingManager) return 0;
		const float dbg = TheSettingManager->GetSettingF("Main.Develop.Main", "DebugVar2");
		if (dbg >= 0.5f) return (int)(dbg + 0.5f);
		return std::clamp(TheSettingManager->GetSettingI("Main.Main.Quality", "MaterialSSS"), 0, 2);
	}

	struct QuadVert {
		float x, y, z, rhw;
		float u, v;
	};

	inline bool Initialized = false;
	inline bool Failed = false;
	inline IDirect3DPixelShader9* PassPS = nullptr;
	inline IDirect3DTexture9* CopyTex = nullptr;
	inline IDirect3DSurface9* CopySurf = nullptr;
	inline D3DSURFACE_DESC CopyDesc = {};

	inline bool EnsureShader(IDirect3DDevice9* d) {
		if (Initialized) return !Failed;
		Initialized = true;
		const std::string path = std::string(Paths::ShadersDir()) + "MaterialSSSProto.hlsl";
		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* err = nullptr;
		const HRESULT hr = D3DXCompileShaderFromFileA(path.c_str(), nullptr, nullptr, "MainPS", "ps_3_0", 0, &code, &err, nullptr);
		if (FAILED(hr) || !code) {
			Logger::Log("[ERROR] UNOFFICIAL MaterialSSSProto: compile failed in %s (%s)",
				path.c_str(), err ? (const char*)err->GetBufferPointer() : "no compiler output");
			if (err) err->Release();
			Failed = true;
			return false;
		}
		if (err) err->Release();
		const HRESULT psHr = d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &PassPS);
		code->Release();
		if (FAILED(psHr) || !PassPS) {
			Logger::Log("[ERROR] UNOFFICIAL MaterialSSSProto: CreatePixelShader failed (%08lX)", psHr);
			Failed = true;
			return false;
		}
		Logger::Log("UNOFFICIAL MaterialSSSProto: ready (Shaders\\MaterialSSSProto.hlsl)");
		return true;
	}

	inline void ReleaseDefaultPool() {
		if (CopySurf) { CopySurf->Release(); CopySurf = nullptr; }
		if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
		CopyDesc = {};
	}

	inline bool EnsureCopy(IDirect3DDevice9* d, const D3DSURFACE_DESC& desc) {
		if (CopyTex && CopyDesc.Width == desc.Width && CopyDesc.Height == desc.Height && CopyDesc.Format == desc.Format)
			return true;
		if (CopySurf) { CopySurf->Release(); CopySurf = nullptr; }
		if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
		if (FAILED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &CopyTex, nullptr)) ||
			FAILED(CopyTex->GetSurfaceLevel(0, &CopySurf))) {
			Logger::Log("[ERROR] UNOFFICIAL MaterialSSSProto: failed to allocate %ux%u copy RT", desc.Width, desc.Height);
			Failed = true;
			return false;
		}
		CopyDesc = desc;
		return true;
	}

	inline void Apply(IDirect3DDevice9* d) {
		if (!d || Failed) return;
		const int mode = EffectiveMode();
		if (mode <= 0) return;
		if (mode == 1) {
			static bool loggedInPass = false;
			if (!loggedInPass) {
				loggedInPass = true;
				Logger::Log("UNOFFICIAL MaterialSSS Phase2: in-pass DeferredSun active (0 extra StretchRect/blur passes)");
			}
			return;
		}
		if (!LinearScene::InSpan || !LinearScene::SpanTarget) return;
		if (!CapturedNormals::Drawn || !CapturedNormals::Texture || !CapturedNormals::AlbedoTexture || !CapturedNormals::MaterialTexture) return;

		IDirect3DSurface9* target = nullptr;
		if (FAILED(d->GetRenderTarget(0, &target)) || !target) return;
		if (target != LinearScene::SpanTarget) { target->Release(); return; }

		D3DSURFACE_DESC desc = {};
		if (FAILED(target->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
			target->Release();
			return;
		}

		// Ensure the world frame is currently in linear light (LinearScene::EndSpan converts back to stored curve once after us).
		if (!LinearScene::Linear) {
			if (!LinearScene::Convert(d, target, true)) {
				target->Release();
				return;
			}
		}

		if (!EnsureShader(d) || !EnsureCopy(d, desc)) {
			target->Release();
			return;
		}

		static GpuTimer timer("MaterialSSS Phase1", 12);
		GpuProfileScope gpu(timer, d);

		if (FAILED(d->StretchRect(target, nullptr, CopySurf, nullptr, D3DTEXF_NONE))) {
			target->Release();
			return;
		}

		const bool linearZ = DeferredDepth::EnsureLinear(d);
		if (!linearZ && !DeferredDepth::Ready()) DeferredDepth::FillFrame(d);

		// Compute normalized to-sun vector and linear sun color matching DeferredSun
		D3DXVECTOR3 toSun(0.0f, 0.0f, 1.0f);
		float sunCol[3] = { 0.8f, 0.75f, 0.65f };
		NiDirectionalLight* sun = Tes ? Tes->directionalLight : nullptr;
		const D3DXVECTOR4& disc = TheShaderManager->ShaderConst.SunDir;
		const bool isExt = TheShaderManager->GameState.isExterior;
		if (isExt && disc.z > 0.0f) {
			const float len = sqrtf(disc.x * disc.x + disc.y * disc.y + disc.z * disc.z);
			if (len > 1.0e-4f) toSun = D3DXVECTOR3(disc.x / len, disc.y / len, disc.z / len);
		} else if (sun) {
			const float len = sqrtf(sun->direction.x * sun->direction.x + sun->direction.y * sun->direction.y + sun->direction.z * sun->direction.z);
			if (len > 1.0e-4f) toSun = D3DXVECTOR3(-sun->direction.x / len, -sun->direction.y / len, -sun->direction.z / len);
		}
		if (sun) {
			const float g = (std::max)(LinearScene::Gamma, 1.0f);
			const float lightScaleLin = (TheShaderManager->Shaders.PBR && TheShaderManager->Shaders.PBR->Constants.LinearScales.x > 0.0f)
				? TheShaderManager->Shaders.PBR->Constants.LinearScales.x
				: 1.0f;
			const float r = (std::max)(sun->Diff.r * sun->Dimmer, 0.0f);
			const float gr = (std::max)(sun->Diff.g * sun->Dimmer, 0.0f);
			const float b = (std::max)(sun->Diff.b * sun->Dimmer, 0.0f);
			sunCol[0] = (g > 1.01f ? powf(r, g) : r) * lightScaleLin;
			sunCol[1] = (g > 1.01f ? powf(gr, g) : gr) * lightScaleLin;
			sunCol[2] = (g > 1.01f ? powf(b, g) : b) * lightScaleLin;
		}

		D3DVIEWPORT9 vp = {};
		d->GetViewport(&vp);
		IDirect3DSurface9* savedRT[4] = {};
		for (DWORD i = 1; i < 4; ++i) {
			if (SUCCEEDED(d->GetRenderTarget(i, &savedRT[i])) && savedRT[i]) d->SetRenderTarget(i, nullptr);
		}
		IDirect3DVertexShader9* savedVS = nullptr; d->GetVertexShader(&savedVS);
		IDirect3DPixelShader9* savedPS = nullptr; d->GetPixelShader(&savedPS);
		IDirect3DVertexDeclaration9* savedDecl = nullptr; d->GetVertexDeclaration(&savedDecl);
		DWORD savedFVF = 0; d->GetFVF(&savedFVF);
		IDirect3DVertexBuffer9* savedVB = nullptr; UINT savedOff = 0, savedStr = 0;
		d->GetStreamSource(0, &savedVB, &savedOff, &savedStr);

		IDirect3DBaseTexture9* savedTex[5] = {};
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		DWORD savedSS[5][6] = {};
		for (DWORD s = 0; s < 5; ++s) {
			d->GetTexture(s, &savedTex[s]);
			for (int i = 0; i < 6; ++i) d->GetSamplerState(s, kSS[i], &savedSS[s][i]);
		}

		float savedC[10][4] = {};
		d->GetPixelShaderConstantF(0, &savedC[0][0], 10);

		static const D3DRENDERSTATETYPE kRS[] = {
			D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE,
			D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE
		};
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])] = {};
		for (size_t i = 0; i < sizeof(kRS) / sizeof(kRS[0]); ++i) d->GetRenderState(kRS[i], &savedRS[i]);

		d->SetRenderState(D3DRS_ZENABLE, FALSE);
		d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		d->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);
		d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_FOGENABLE, FALSE);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

		const float tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const float projScalePx = 0.5f * (float)vp.Height / (std::max)(tanY, 1.0e-3f);
		const bool turn = TemporalBackbone::Shared() ? TemporalBackbone::TemporalResolve() : false;
		const float noiseAngle = turn ? 6.2831853f * fmodf(0.61803399f * (float)((TemporalBackbone::NoiseIndex() + 73u) % 1024u), 1.0f) : 0.0f;

		float c[10][4] = {};
		c[0][0] = 1.0f / (float)desc.Width;
		c[0][1] = 1.0f / (float)desc.Height;
		c[0][2] = projScalePx;
		c[0][3] = (float)mode;
		c[1][0] = toSun.x; c[1][1] = toSun.y; c[1][2] = toSun.z; c[1][3] = linearZ ? 1.0f : 0.0f;
		c[2][0] = sunCol[0]; c[2][1] = sunCol[1]; c[2][2] = sunCol[2]; c[2][3] = noiseAngle;
		memcpy(&c[3][0], DeferredDepth::Frame, sizeof(float) * 24);
		c[9][0] = isExt ? 1.0f : 0.0f;
		c[9][1] = (std::max)(LinearScene::Gamma, 1.0f);
		c[9][2] = std::clamp(TheShaderManager->ShaderConst.SunAmount.x, 0.0f, 1.0f);
		d->SetPixelShaderConstantF(0, &c[0][0], 10);

		IDirect3DBaseTexture9* tex[5] = {
			CopyTex,
			CapturedNormals::AlbedoTexture,
			CapturedNormals::MaterialTexture,
			CapturedNormals::Texture,
			linearZ ? DeferredDepth::LinearTexture : nullptr
		};
		const DWORD ptClamp[6] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		for (DWORD s = 0; s < 5; ++s) {
			d->SetTexture(s, tex[s]);
			for (int i = 0; i < 6; ++i) d->SetSamplerState(s, kSS[i], ptClamp[i]);
		}

		d->SetRenderTarget(0, target);
		d->SetViewport(&vp);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(PassPS);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

		const float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f;
		const float x1 = (float)(vp.X + vp.Width) - 0.5f, y1 = (float)(vp.Y + vp.Height) - 0.5f;
		const float u0 = (float)vp.X / (float)desc.Width, v0 = (float)vp.Y / (float)desc.Height;
		const float u1 = (float)(vp.X + vp.Width) / (float)desc.Width, v1 = (float)(vp.Y + vp.Height) / (float)desc.Height;
		const QuadVert quad[4] = {
			{ x0, y0, 0.0f, 1.0f, u0, v0 },
			{ x1, y0, 0.0f, 1.0f, u1, v0 },
			{ x0, y1, 0.0f, 1.0f, u0, v1 },
			{ x1, y1, 0.0f, 1.0f, u1, v1 },
		};
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVert));

		d->SetPixelShaderConstantF(0, &savedC[0][0], 10);
		for (size_t i = 0; i < sizeof(kRS) / sizeof(kRS[0]); ++i) d->SetRenderState(kRS[i], savedRS[i]);
		for (DWORD s = 0; s < 5; ++s) {
			d->SetTexture(s, savedTex[s]);
			if (savedTex[s]) savedTex[s]->Release();
			for (int i = 0; i < 6; ++i) d->SetSamplerState(s, kSS[i], savedSS[s][i]);
		}
		for (DWORD i = 1; i < 4; ++i) {
			if (savedRT[i]) { d->SetRenderTarget(i, savedRT[i]); savedRT[i]->Release(); }
		}
		d->SetViewport(&vp);
		if (savedDecl) { d->SetVertexDeclaration(savedDecl); savedDecl->Release(); }
		else if (savedFVF) d->SetFVF(savedFVF);
		d->SetStreamSource(0, savedVB, savedOff, savedStr); if (savedVB) savedVB->Release();
		d->SetVertexShader(savedVS); if (savedVS) savedVS->Release();
		d->SetPixelShader(savedPS); if (savedPS) savedPS->Release();
		target->Release();
	}
}
