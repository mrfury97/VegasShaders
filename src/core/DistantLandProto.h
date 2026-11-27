#pragma once

// UNOFFICIAL prototype (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Hardware-instanced distant Strip & Mojave settlement night lights in RenderEffectsPreTonemapping.
// Gated behind [Main.Develop.Main] DebugVar1 > 0.5 (1 = auto night factor, 2 = force full night glow).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "DistantLandLightStore.h"

namespace DistantLandProto {
	struct QuadVertex {
		float x, y;
		float u, v;
	};
	struct LightInstance {
		float x, y, z, radius;
		float r, g, b, depthBias;
	};
	struct ScreenQuadVertex {
		float x, y, z, rhw;
		float u, v;
	};

	inline bool Initialized = false;
	inline bool Failed = false;
	inline IDirect3DVertexShader9* LightVS = nullptr;
	inline IDirect3DPixelShader9* LightPS = nullptr;
	inline IDirect3DPixelShader9* CompositePS = nullptr;
	inline IDirect3DVertexDeclaration9* InstDecl = nullptr;
	inline IDirect3DVertexBuffer9* QuadVB = nullptr;
	inline IDirect3DIndexBuffer9* QuadIB = nullptr;
	inline IDirect3DVertexBuffer9* InstVB = nullptr;
	inline UINT InstanceCount = 0;
	inline uint32_t UploadedLightGen = 0;

	inline IDirect3DTexture9* ProtoTex = nullptr;
	inline IDirect3DSurface9* ProtoSurf = nullptr;
	inline IDirect3DTexture9* CopyTex = nullptr;
	inline IDirect3DSurface9* CopySurf = nullptr;
	inline D3DSURFACE_DESC TargetDesc = {};

	inline std::vector<LightInstance> BuildMojaveLights() {
		std::vector<LightInstance> out;
		out.reserve(320);
		auto add = [&](float x, float y, float z, float radius, float r, float g, float b, float bias = 1000.0f) {
			out.push_back({ x, y, z, radius, r, g, b, bias });
		};
		constexpr float kTwoPi = 6.2831853f;

		// 1. Lucky 38 Tower (-18888, 96472, base 4056) — verified FalloutNV.esm WastelandNV REFRs
		// Crown red/amber aviation & spire beacons
		add(-18888.0f, 96472.0f, 9050.0f, 180.0f, 8.5f, 1.4f, 0.5f, 1200.0f);
		add(-18888.0f, 96472.0f, 8550.0f, 150.0f, 6.5f, 3.6f, 1.0f, 1200.0f);
		// Upper observation & cocktail lounge saucer rings
		for (int i = 0; i < 20; ++i) {
			const float a = kTwoPi * (float)i / 20.0f;
			const float cx = -18888.0f + cosf(a) * 390.0f;
			const float cy = 96472.0f + sinf(a) * 390.0f;
			if (i % 2 == 0)
				add(cx, cy, 7620.0f, 105.0f, 1.8f, 5.2f, 6.5f, 1200.0f); // iconic cyan rim
			else
				add(cx, cy, 7460.0f, 95.0f, 6.2f, 3.8f, 1.2f, 1200.0f); // warm gold interior deck
		}
		// Shaft vertical neon ribs up the Lucky 38 column
		for (int rib = 0; rib < 4; ++rib) {
			const float a = kTwoPi * (float)rib / 4.0f + 0.35f;
			for (int k = 0; k < 6; ++k) {
				const float z = 4900.0f + (float)k * 410.0f;
				add(-18888.0f + cosf(a) * 230.0f, 96472.0f + sinf(a) * 230.0f, z, 95.0f, 5.0f, 2.7f, 0.9f, 1200.0f);
			}
		}

		// 2. The Tops Hotel & Casino (-22764, 94080, base 4056) — turquoise/teal & gold tower strips
		for (int s = 0; s < 3; ++s) {
			const float ox = (float)(s - 1) * 420.0f;
			for (int k = 0; k < 5; ++k) {
				const float z = 4500.0f + (float)k * 380.0f;
				add(-22764.0f + ox, 94080.0f - 280.0f, z, 110.0f, 1.4f, 5.6f, 5.2f, 1300.0f);
				add(-22764.0f + 340.0f, 94080.0f + ox, z + 150.0f, 105.0f, 5.5f, 4.1f, 1.3f, 1300.0f);
			}
		}
		// Tops roof crown marquee
		for (int i = 0; i < 8; ++i) {
			const float a = kTwoPi * (float)i / 8.0f;
			add(-22764.0f + cosf(a) * 460.0f, 94080.0f + sinf(a) * 460.0f, 6500.0f, 125.0f, 1.6f, 6.2f, 5.4f, 1300.0f);
		}

		// 3. Gomorrah (-22504, 99096, base 3916) — rich magenta, crimson & amber neon curves
		for (int i = 0; i < 10; ++i) {
			const float a = kTwoPi * (float)i / 10.0f;
			const float r = 420.0f;
			const bool hot = (i % 2) == 0;
			add(-22504.0f + cosf(a) * r, 99096.0f + sinf(a) * r, 5850.0f, 130.0f,
				hot ? 6.8f : 6.0f, hot ? 1.5f : 3.2f, hot ? 3.8f : 0.9f, 1300.0f);
		}
		for (int k = 0; k < 6; ++k) {
			const float z = 4350.0f + (float)k * 290.0f;
			add(-22100.0f, 99096.0f - 320.0f + (float)(k % 3) * 280.0f, z, 115.0f, 6.5f, 1.9f, 3.2f, 1300.0f);
		}

		// 4. Ultra-Luxe (-18440.6, 88153.1, base 4086.6) — warm champagne, ivory gold & arched facade
		for (int i = 0; i < 12; ++i) {
			const float a = kTwoPi * (float)i / 12.0f;
			add(-18440.6f + cosf(a) * 520.0f, 88153.1f + sinf(a) * 520.0f, 4950.0f, 120.0f, 5.8f, 4.3f, 1.9f, 1250.0f);
		}
		add(-18440.6f, 88153.1f, 5580.0f, 185.0f, 6.2f, 4.7f, 2.2f, 1250.0f);
		add(-17860.0f, 88153.1f, 4520.0f, 145.0f, 4.5f, 3.9f, 1.8f, 1250.0f);

		// 5. Strip Promenade, Vault 21, Wrangler & Freeside Neon Corridor
		struct StripNode { float x, y, z, rad, r, g, b; };
		static const StripNode kCorridor[] = {
			{ -19800.0f, 92200.0f, 4520.0f, 130.0f, 5.2f, 3.1f, 0.9f },
			{ -20600.0f, 95400.0f, 4560.0f, 130.0f, 2.1f, 4.9f, 5.4f },
			{ -21200.0f, 97600.0f, 4480.0f, 125.0f, 6.0f, 2.2f, 3.2f },
			{ -21700.0f, 100500.0f, 4550.0f, 130.0f, 5.6f, 3.8f, 1.1f },
			{ -17900.0f, 90200.0f, 4460.0f, 115.0f, 5.0f, 3.4f, 1.2f },
			{ -17400.0f, 94500.0f, 4500.0f, 115.0f, 1.8f, 4.8f, 4.6f },
			{ -13800.0f, 117595.0f, 4650.0f, 135.0f, 5.5f, 2.5f, 0.8f }, // Freeside/North Vegas neon signs
			{ -13216.0f, 117603.0f, 4720.0f, 125.0f, 2.0f, 4.5f, 5.0f },
			{ -17502.0f, 116201.0f, 4800.0f, 130.0f, 5.8f, 1.8f, 2.2f },
		};
		for (const auto& n : kCorridor) add(n.x, n.y, n.z, n.rad, n.r, n.g, n.b, 1250.0f);

		// Strip street-level warm sodium & neon glow clusters
		for (int i = 0; i < 24; ++i) {
			const float t = (float)i / 23.0f;
			const float x = -18200.0f - t * 5000.0f + sinf((float)i * 1.7f) * 320.0f;
			const float y = 88200.0f + t * 14500.0f;
			const float z = 4280.0f + (float)(i % 3) * 160.0f;
			const bool cool = (i % 4) == 1;
			add(x, y, z, 95.0f, cool ? 1.8f : 4.6f, cool ? 4.4f : 2.9f, cool ? 4.8f : 0.9f, 1250.0f);
		}

		// 6. Camp McCarran (-10088, 62474, 4592) — concourse worklamps & red runway/tower beacons
		add(-10088.0f, 62474.0f, 5400.0f, 120.0f, 5.2f, 1.1f, 0.4f, 1100.0f);
		add(-8670.0f, 63473.0f, 4900.0f, 105.0f, 4.0f, 2.7f, 1.1f, 1100.0f);
		add(-12424.0f, 60455.0f, 4850.0f, 105.0f, 4.0f, 2.7f, 1.1f, 1100.0f);
		add(-3500.0f, 61860.0f, 4850.0f, 95.0f, 3.8f, 2.6f, 1.0f, 900.0f);

		// 7. HELIOS One (21081.4, -1265.5, 5235.1)
		add(21081.4f, -1265.5f, 8350.0f, 160.0f, 3.2f, 4.8f, 6.2f, 1200.0f);
		add(21081.4f, -1265.5f, 8950.0f, 140.0f, 6.2f, 1.4f, 0.5f, 1200.0f);
		add(22839.5f, -387.2f, 5650.0f, 105.0f, 4.4f, 2.8f, 1.0f, 900.0f);

		// 8. Novac (24789.1, -32500.8, 6980.2)
		add(24789.1f, -32500.8f, 7650.0f, 115.0f, 4.6f, 2.2f, 0.8f, 900.0f);
		add(23258.3f, -32546.1f, 7450.0f, 110.0f, 5.4f, 1.5f, 0.7f, 900.0f);

		// 9. Goodsprings (-67970.2, 3904.5, 8368.1)
		add(-67970.2f, 3904.5f, 8720.0f, 95.0f, 4.4f, 2.6f, 0.9f, 900.0f);
		add(-65385.9f, -1683.7f, 8620.0f, 85.0f, 3.8f, 2.4f, 0.9f, 900.0f);

		return out;
	}

	inline bool CompileShaders(IDirect3DDevice9* d) {
		const std::string path = std::string(Paths::ShadersDir()) + "DistantLandProto.hlsl";
		auto compile = [&](const char* entry, const char* target, ID3DXBuffer** code) -> bool {
			ID3DXBuffer* err = nullptr;
			const HRESULT hr = D3DXCompileShaderFromFileA(path.c_str(), nullptr, nullptr, entry, target, 0, code, &err, nullptr);
			if (FAILED(hr) || !*code) {
				Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: %s (%s) compile failed in %s (%s)",
					entry, target, path.c_str(), err ? (const char*)err->GetBufferPointer() : "no compiler output");
				if (err) err->Release();
				return false;
			}
			if (err) err->Release();
			return true;
		};
		ID3DXBuffer* vsBuf = nullptr;
		ID3DXBuffer* psBuf = nullptr;
		ID3DXBuffer* compBuf = nullptr;
		if (!compile("LightVS", "vs_3_0", &vsBuf) || !compile("LightPS", "ps_3_0", &psBuf) || !compile("CompositePS", "ps_3_0", &compBuf)) {
			if (vsBuf) vsBuf->Release();
			if (psBuf) psBuf->Release();
			if (compBuf) compBuf->Release();
			return false;
		}
		const HRESULT hr1 = d->CreateVertexShader((const DWORD*)vsBuf->GetBufferPointer(), &LightVS);
		const HRESULT hr2 = d->CreatePixelShader((const DWORD*)psBuf->GetBufferPointer(), &LightPS);
		const HRESULT hr3 = d->CreatePixelShader((const DWORD*)compBuf->GetBufferPointer(), &CompositePS);
		vsBuf->Release(); psBuf->Release(); compBuf->Release();
		if (FAILED(hr1) || FAILED(hr2) || FAILED(hr3) || !LightVS || !LightPS || !CompositePS) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: CreateVertexShader/PixelShader failed (%08lX %08lX %08lX)", hr1, hr2, hr3);
			return false;
		}
		return true;
	}

	inline bool InitResources(IDirect3DDevice9* d) {
		if (Initialized) return !Failed;
		Initialized = true;
		if (!CompileShaders(d)) { Failed = true; return false; }

		static const D3DVERTEXELEMENT9 kDecl[] = {
			{ 0, 0,  D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 8,  D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 1, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
			{ 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
			D3DDECL_END()
		};
		if (FAILED(d->CreateVertexDeclaration(kDecl, &InstDecl)) || !InstDecl) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: CreateVertexDeclaration failed");
			Failed = true; return false;
		}

		static const QuadVertex kQuad[4] = {
			{ -1.0f, -1.0f, 0.0f, 1.0f },
			{  1.0f, -1.0f, 1.0f, 1.0f },
			{ -1.0f,  1.0f, 0.0f, 0.0f },
			{  1.0f,  1.0f, 1.0f, 0.0f },
		};
		if (FAILED(d->CreateVertexBuffer(sizeof(kQuad), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &QuadVB, nullptr)) || !QuadVB) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: CreateVertexBuffer(QuadVB) failed");
			Failed = true; return false;
		}
		void* ptr = nullptr;
		if (SUCCEEDED(QuadVB->Lock(0, sizeof(kQuad), &ptr, 0))) {
			memcpy(ptr, kQuad, sizeof(kQuad));
			QuadVB->Unlock();
		}

		static const unsigned short kIndices[6] = { 0, 1, 2, 1, 3, 2 };
		if (FAILED(d->CreateIndexBuffer(sizeof(kIndices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &QuadIB, nullptr)) || !QuadIB) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: CreateIndexBuffer failed");
			Failed = true; return false;
		}
		if (SUCCEEDED(QuadIB->Lock(0, sizeof(kIndices), &ptr, 0))) {
			memcpy(ptr, kIndices, sizeof(kIndices));
			QuadIB->Unlock();
		}

		std::vector<LightInstance> instances = BuildMojaveLights();
		InstanceCount = (UINT)instances.size();
		const UINT instBytes = InstanceCount * (UINT)sizeof(LightInstance);
		if (FAILED(d->CreateVertexBuffer(instBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &InstVB, nullptr)) || !InstVB) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: CreateVertexBuffer(InstVB) failed");
			Failed = true; return false;
		}
		if (SUCCEEDED(InstVB->Lock(0, instBytes, &ptr, 0))) {
			memcpy(ptr, instances.data(), instBytes);
			InstVB->Unlock();
		}

		Logger::Log("UNOFFICIAL DistantLandProto: ready (%u hardware-instanced Mojave landmark lights)", InstanceCount);
		return true;
	}

	inline void UploadDynamicLights(IDirect3DDevice9* d, const std::vector<LandHeights::NightLightRef>& lights) {
		if (!d || lights.empty() || Failed) return;
		const UINT newCount = (UINT)lights.size();
		const UINT instBytes = newCount * (UINT)sizeof(LightInstance);
		IDirect3DVertexBuffer9* vb = nullptr;
		void* ptr = nullptr;
		if (FAILED(d->CreateVertexBuffer(instBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr)) || !vb ||
			FAILED(vb->Lock(0, instBytes, &ptr, 0))) {
			if (vb) vb->Release();
			return;
		}
		static_assert(sizeof(LightInstance) == sizeof(LandHeights::NightLightRef), "NightLightRef layout mismatch");
		memcpy(ptr, lights.data(), instBytes);
		vb->Unlock();

		if (InstVB) InstVB->Release();
		InstVB = vb;
		InstanceCount = newCount;
		Logger::Log("UNOFFICIAL DistantLandProto: updated to %u NIF-calibrated + ESM dynamic night lights (%.1f KB)",
			InstanceCount, (double)instBytes / 1024.0);
	}

	inline bool EnsureTargets(IDirect3DDevice9* d, const D3DSURFACE_DESC& desc) {
		if (ProtoTex && CopyTex && TargetDesc.Width == desc.Width && TargetDesc.Height == desc.Height && TargetDesc.Format == desc.Format)
			return true;
		if (ProtoSurf) { ProtoSurf->Release(); ProtoSurf = nullptr; }
		if (ProtoTex) { ProtoTex->Release(); ProtoTex = nullptr; }
		if (CopySurf) { CopySurf->Release(); CopySurf = nullptr; }
		if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
		if (FAILED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &ProtoTex, nullptr)) ||
			FAILED(ProtoTex->GetSurfaceLevel(0, &ProtoSurf)) ||
			FAILED(d->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &CopyTex, nullptr)) ||
			FAILED(CopyTex->GetSurfaceLevel(0, &CopySurf))) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandProto: failed to allocate %ux%u RTs", desc.Width, desc.Height);
			Failed = true;
			return false;
		}
		TargetDesc = desc;
		return true;
	}

	inline void ReleaseTargets() {
		if (ProtoSurf) { ProtoSurf->Release(); ProtoSurf = nullptr; }
		if (ProtoTex) { ProtoTex->Release(); ProtoTex = nullptr; }
		if (CopySurf) { CopySurf->Release(); CopySurf = nullptr; }
		if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
		TargetDesc = {};
	}

	inline void ReleaseDefaultPool() {
		ReleaseTargets();
		UploadedLightGen = 0;
	}

	inline void Render(IDirect3DDevice9* d, IDirect3DSurface9* renderTarget) {
		if (!d || !renderTarget || Failed) return;
		const int mode = DistantLandLightStore::EffectiveDistantLandMode();
		if (mode <= 0) return;
		if (!TheShaderManager->GameState.isExterior) return;

		const float nightFactor = DistantLandLightStore::ForceNightLights() ? 1.0f : std::clamp(1.0f - TheShaderManager->ShaderConst.SunAmount.x, 0.0f, 1.0f);
		if (nightFactor <= 0.001f) return;

		if (!InitResources(d)) return;
		if (DistantLandLightStore::Generation != UploadedLightGen) {
			UploadedLightGen = DistantLandLightStore::Generation;
			if (!DistantLandLightStore::Lights.empty()) {
				UploadDynamicLights(d, DistantLandLightStore::Lights);
			}
		}

		IDirect3DSurface9* image = (TheShaderManager->Chain.IsActive() && TheTextureManager->RenderedSurface)
			? TheTextureManager->RenderedSurface
			: renderTarget;
		IDirect3DTexture9* depthTex = (TheShaderManager->Effects.CombineDepth && TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture)
			? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture
			: nullptr;
		if (!image || !depthTex) return;

		D3DSURFACE_DESC desc = {};
		if (FAILED(image->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) return;
		if (!EnsureTargets(d, desc)) return;

		const unsigned rectW = RenderRect::Width(desc.Width);
		const unsigned rectH = RenderRect::Height(desc.Height);
		if (rectW == 0 || rectH == 0) return;

		static GpuTimer timer("DistantLand proto lights", 12);
		GpuProfileScope gpu(timer, d);

		// Copy current HDR scene before drawing composite
		if (FAILED(d->StretchRect(image, nullptr, CopySurf, nullptr, D3DTEXF_NONE))) return;

		// Save device state thoroughly
		D3DVIEWPORT9 savedVP = {};
		d->GetViewport(&savedVP);
		IDirect3DSurface9* savedRT[4] = {};
		for (DWORD i = 0; i < 4; ++i) d->GetRenderTarget(i, &savedRT[i]);
		IDirect3DVertexShader9* savedVS = nullptr; d->GetVertexShader(&savedVS);
		IDirect3DPixelShader9* savedPS = nullptr; d->GetPixelShader(&savedPS);
		IDirect3DVertexDeclaration9* savedDecl = nullptr; d->GetVertexDeclaration(&savedDecl);
		DWORD savedFVF = 0; d->GetFVF(&savedFVF);
		IDirect3DVertexBuffer9* savedVB0 = nullptr; UINT savedOff0 = 0, savedStr0 = 0;
		IDirect3DVertexBuffer9* savedVB1 = nullptr; UINT savedOff1 = 0, savedStr1 = 0;
		d->GetStreamSource(0, &savedVB0, &savedOff0, &savedStr0);
		d->GetStreamSource(1, &savedVB1, &savedOff1, &savedStr1);
		UINT savedFreq0 = 1, savedFreq1 = 1;
		d->GetStreamSourceFreq(0, &savedFreq0);
		d->GetStreamSourceFreq(1, &savedFreq1);
		IDirect3DIndexBuffer9* savedIB = nullptr;
		d->GetIndices(&savedIB);

		IDirect3DBaseTexture9* savedTex[2] = {};
		d->GetTexture(0, &savedTex[0]);
		d->GetTexture(1, &savedTex[1]);
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		DWORD savedSS[2][6] = {};
		for (DWORD s = 0; s < 2; ++s) {
			for (int i = 0; i < 6; ++i) d->GetSamplerState(s, kSS[i], &savedSS[s][i]);
		}

		float savedVSConst[7][4] = {};
		d->GetVertexShaderConstantF(0, &savedVSConst[0][0], 7);
		float savedPSConst[2][4] = {};
		d->GetPixelShaderConstantF(0, &savedPSConst[0][0], 2);

		static const D3DRENDERSTATETYPE kRS[] = {
			D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
			D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE,
			D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE
		};
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])] = {};
		for (size_t i = 0; i < sizeof(kRS) / sizeof(kRS[0]); ++i) d->GetRenderState(kRS[i], &savedRS[i]);

		// Configure common state
		for (DWORD i = 1; i < 4; ++i) d->SetRenderTarget(i, nullptr);
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
		d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

		D3DVIEWPORT9 vp = { 0, 0, rectW, rectH, 0.0f, 1.0f };
		d->SetViewport(&vp);

		// --- Pass 1: Hardware-instanced radial light splats into linear FP16 ProtoSurf ---
		d->SetRenderTarget(0, ProtoSurf);
		d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);

		d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);

		const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
		const D3DXMATRIX& p = TheRenderManager->projMatrix;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const float timeSec = (float)((GetTickCount64() % 1000000ULL) * 0.001);

		const float vsC[7][4] = {
			{ iv._11, iv._12, iv._13, 0.0f },
			{ iv._21, iv._22, iv._23, 0.0f },
			{ iv._31, iv._32, iv._33, 0.0f },
			{ cam.x,  cam.y,  cam.z,  0.0f },
			{ p._11,  p._22,  p._33,  p._43 },
			{ p._31,  p._32,  timeSec, nightFactor },
			{ 0.0022f, 3800.0f, 9500.0f, 1.0f },
		};
		d->SetVertexShaderConstantF(0, &vsC[0][0], 7);

		const float farZ = (std::max)(TheRenderManager->CameraData.y, 1000.0f);
		const float psC[2][4] = {
			{ 1.0f / (float)desc.Width, 1.0f / (float)desc.Height, farZ, 0.000014f },
			{ 0.0f, 0.0f, 3800.0f, 9500.0f },
		};
		d->SetPixelShaderConstantF(0, &psC[0][0], 2);

		d->SetTexture(0, depthTex);
		const DWORD ptClamp[6] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		for (int i = 0; i < 6; ++i) d->SetSamplerState(0, kSS[i], ptClamp[i]);

		d->SetVertexShader(LightVS);
		d->SetPixelShader(LightPS);
		d->SetVertexDeclaration(InstDecl);
		d->SetStreamSource(0, QuadVB, 0, sizeof(QuadVertex));
		d->SetStreamSource(1, InstVB, 0, sizeof(LightInstance));
		d->SetIndices(QuadIB);
		d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | InstanceCount);
		d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
		d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);

		// Reset stream frequencies immediately so no downstream draw keeps instancing state!
		d->SetStreamSourceFreq(0, 1);
		d->SetStreamSourceFreq(1, 1);
		d->SetStreamSource(1, nullptr, 0, 0);

		// --- Pass 2: Composite accumulated linear light into stored-curve HDR scene target ---
		d->SetRenderTarget(0, image);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		d->SetVertexShader(nullptr);
		d->SetPixelShader(CompositePS);
		d->SetTexture(0, CopyTex);
		d->SetTexture(1, ProtoTex);
		for (DWORD s = 0; s < 2; ++s) {
			for (int i = 0; i < 6; ++i) d->SetSamplerState(s, kSS[i], ptClamp[i]);
		}

		const float uMax = (float)rectW / (float)desc.Width;
		const float vMax = (float)rectH / (float)desc.Height;
		const ScreenQuadVertex quad[4] = {
			{ -0.5f,               -0.5f,                0.0f, 1.0f, 0.0f, 0.0f },
			{ (float)rectW - 0.5f, -0.5f,                0.0f, 1.0f, uMax, 0.0f },
			{ -0.5f,               (float)rectH - 0.5f,  0.0f, 1.0f, 0.0f, vMax },
			{ (float)rectW - 0.5f, (float)rectH - 0.5f,  0.0f, 1.0f, uMax, vMax },
		};
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenQuadVertex));

		// Restore all device state
		d->SetVertexShaderConstantF(0, &savedVSConst[0][0], 7);
		d->SetPixelShaderConstantF(0, &savedPSConst[0][0], 2);
		for (size_t i = 0; i < sizeof(kRS) / sizeof(kRS[0]); ++i) d->SetRenderState(kRS[i], savedRS[i]);
		for (DWORD s = 0; s < 2; ++s) {
			d->SetTexture(s, savedTex[s]);
			if (savedTex[s]) savedTex[s]->Release();
			for (int i = 0; i < 6; ++i) d->SetSamplerState(s, kSS[i], savedSS[s][i]);
		}
		d->SetIndices(savedIB); if (savedIB) savedIB->Release();
		d->SetStreamSourceFreq(0, savedFreq0);
		d->SetStreamSourceFreq(1, savedFreq1);
		d->SetStreamSource(0, savedVB0, savedOff0, savedStr0); if (savedVB0) savedVB0->Release();
		d->SetStreamSource(1, savedVB1, savedOff1, savedStr1); if (savedVB1) savedVB1->Release();
		d->SetVertexDeclaration(savedDecl); if (savedDecl) savedDecl->Release();
		if (savedFVF) d->SetFVF(savedFVF);
		d->SetVertexShader(savedVS); if (savedVS) savedVS->Release();
		d->SetPixelShader(savedPS); if (savedPS) savedPS->Release();
		for (DWORD i = 0; i < 4; ++i) {
			d->SetRenderTarget(i, savedRT[i]);
			if (savedRT[i]) savedRT[i]->Release();
		}
		d->SetRenderTarget(0, renderTarget);
		d->SetViewport(&savedVP);
	}
}
