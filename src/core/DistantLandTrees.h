#pragma once

// UNOFFICIAL Phase 2 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Hardware-instanced crossed-card far trees & desert flora (Joshua, Pine, DeadTree, Yucca/Mesquite/Bush, Palm/Cottonwood)
// parsed from ESM/ESP exterior REFR records alongside LandHeights. Rendered in 1 DrawIndexedPrimitive call via SetStreamSourceFreq.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../effects/Terrain/LandHeights.h"

namespace DistantLandTrees {
	struct CardVertex {
		float x, y, z;
		float u, v;
		float nx, ny, nz;
	};
	struct TreeInstance {
		float x, y, z, scale;
		float w, h, rotZ, style;
	};

	inline bool Initialized = false;
	inline bool Failed      = false;

	inline IDirect3DVertexShader9*      TreeVS   = nullptr;
	inline IDirect3DPixelShader9*       TreePS   = nullptr;
	inline IDirect3DVertexDeclaration9* InstDecl = nullptr;
	inline IDirect3DVertexBuffer9*      CardVB   = nullptr;
	inline IDirect3DIndexBuffer9*       CardIB   = nullptr;
	inline IDirect3DVertexBuffer9*      InstVB   = nullptr;
	inline UINT                         InstanceCount = 0;

	inline void ReleaseInstances() {
		if (InstVB) { InstVB->Release(); InstVB = nullptr; }
		InstanceCount = 0;
	}

	inline bool CompileShaders(IDirect3DDevice9* d) {
		const std::string path = std::string(Paths::ShadersDir()) + "DistantLandTrees.hlsl";
		auto compile = [&](const char* entry, const char* target, ID3DXBuffer** code) -> bool {
			ID3DXBuffer* err = nullptr;
			const HRESULT hr = D3DXCompileShaderFromFileA(path.c_str(), nullptr, nullptr, entry, target, 0, code, &err, nullptr);
			if (FAILED(hr) || !*code) {
				Logger::Log("[ERROR] UNOFFICIAL DistantLandTrees: %s (%s) compile failed in %s (%s)",
					entry, target, path.c_str(), err ? (const char*)err->GetBufferPointer() : "no compiler output");
				if (err) err->Release();
				return false;
			}
			if (err) err->Release();
			return true;
		};
		ID3DXBuffer* vsBuf = nullptr;
		ID3DXBuffer* psBuf = nullptr;
		if (!compile("TreeVS", "vs_3_0", &vsBuf) || !compile("TreePS", "ps_3_0", &psBuf)) {
			if (vsBuf) vsBuf->Release();
			if (psBuf) psBuf->Release();
			return false;
		}
		const HRESULT hr1 = d->CreateVertexShader((const DWORD*)vsBuf->GetBufferPointer(), &TreeVS);
		const HRESULT hr2 = d->CreatePixelShader((const DWORD*)psBuf->GetBufferPointer(), &TreePS);
		vsBuf->Release();
		psBuf->Release();
		if (FAILED(hr1) || FAILED(hr2) || !TreeVS || !TreePS) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTrees: CreateVertexShader/PixelShader failed (%08lX %08lX)", hr1, hr2);
			return false;
		}
		return true;
	}

	inline bool InitResources(IDirect3DDevice9* d) {
		if (Initialized) return !Failed;
		Initialized = true;
		if (!CompileShaders(d)) { Failed = true; return false; }

		static const D3DVERTEXELEMENT9 kDecl[] = {
			{ 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 0, 20, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 1, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
			{ 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
			D3DDECL_END()
		};
		if (FAILED(d->CreateVertexDeclaration(kDecl, &InstDecl)) || !InstDecl) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTrees: CreateVertexDeclaration failed");
			Failed = true; return false;
		}

		// Two crossed vertical cards (8 verts, 4 tris)
		static const CardVertex kCards[8] = {
			// Card A (X axis)
			{ -0.5f,  0.0f, 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f },
			{  0.5f,  0.0f, 1.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f },
			{ -0.5f,  0.0f, 0.0f, 0.0f, 1.0f,  0.0f, 1.0f, 0.0f },
			{  0.5f,  0.0f, 0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f },
			// Card B (Y axis)
			{  0.0f, -0.5f, 1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f },
			{  0.0f,  0.5f, 1.0f, 1.0f, 0.0f,  1.0f, 0.0f, 0.0f },
			{  0.0f, -0.5f, 0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f },
			{  0.0f,  0.5f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f, 0.0f },
		};
		void* ptr = nullptr;
		if (FAILED(d->CreateVertexBuffer(sizeof(kCards), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &CardVB, nullptr)) || !CardVB ||
			FAILED(CardVB->Lock(0, sizeof(kCards), &ptr, 0))) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTrees: CreateVertexBuffer(CardVB) failed");
			Failed = true; return false;
		}
		memcpy(ptr, kCards, sizeof(kCards));
		CardVB->Unlock();

		static const uint16_t kIdx[12] = { 0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6 };
		if (FAILED(d->CreateIndexBuffer(sizeof(kIdx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &CardIB, nullptr)) || !CardIB ||
			FAILED(CardIB->Lock(0, sizeof(kIdx), &ptr, 0))) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandTrees: CreateIndexBuffer(CardIB) failed");
			Failed = true; return false;
		}
		memcpy(ptr, kIdx, sizeof(kIdx));
		CardIB->Unlock();

		return true;
	}

	inline void UploadInstances(IDirect3DDevice9* d, std::vector<LandHeights::FoliageRef>& refs) {
		ReleaseInstances();
		if (refs.empty()) return;

		static const float kBaseW[5] = { 230.0f, 430.0f, 200.0f, 145.0f, 360.0f };
		static const float kBaseH[5] = { 360.0f, 920.0f, 370.0f, 118.0f, 680.0f };

		std::vector<TreeInstance> inst(refs.size());
		for (size_t i = 0; i < refs.size(); ++i) {
			const auto& r = refs[i];
			const int s = (r.style < 5) ? r.style : 0;
			inst[i] = TreeInstance{ r.x, r.y, r.z, r.scale, kBaseW[s], kBaseH[s], r.rotZ, (float)s };
		}
		std::vector<LandHeights::FoliageRef>().swap(refs);

		const UINT bytes = (UINT)(inst.size() * sizeof(TreeInstance));
		IDirect3DVertexBuffer9* vb = nullptr;
		void* ptr = nullptr;
		if (SUCCEEDED(d->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr)) && vb &&
			SUCCEEDED(vb->Lock(0, bytes, &ptr, 0))) {
			memcpy(ptr, inst.data(), bytes);
			vb->Unlock();
			InstVB = vb;
			InstanceCount = (UINT)inst.size();
			Logger::Log("UNOFFICIAL DistantLandTrees: uploaded %u hardware-instanced distant trees/shrubs (%.2f MB)",
				InstanceCount, (double)bytes / (1024.0 * 1024.0));
		} else {
			if (vb) vb->Release();
			Logger::Log("[WARNING] UNOFFICIAL DistantLandTrees: CreateVertexBuffer(InstVB, %u bytes) failed", bytes);
		}
	}

	// Called inside DistantLandTerrain::Render while GBufferV2 MRTs, viewport, and hardware reversed depth are already bound.
	// Caller restores D3D9 device state afterward; we save & restore StreamSource(1)/StreamSourceFreq immediately here.
	inline void RenderInGBuffer(IDirect3DDevice9* d, const float vsConst7[7][4], const float psConst15[15][4]) {
		if (Failed || !InstVB || InstanceCount == 0 || !InitResources(d)) return;

		static GpuTimer timer("DistantLand trees", 12);
		GpuProfileScope gpu(timer, d);

		IDirect3DVertexBuffer9* savedVB1 = nullptr; UINT savedOff1 = 0, savedStr1 = 0;
		d->GetStreamSource(1, &savedVB1, &savedOff1, &savedStr1);
		UINT savedFreq0 = 1, savedFreq1 = 1;
		d->GetStreamSourceFreq(0, &savedFreq0);
		d->GetStreamSourceFreq(1, &savedFreq1);

		d->SetVertexShaderConstantF(0, &vsConst7[0][0], 7);
		d->SetPixelShaderConstantF(0, &psConst15[0][0], 15);

		d->SetVertexShader(TreeVS);
		d->SetPixelShader(TreePS);
		d->SetVertexDeclaration(InstDecl);
		d->SetStreamSource(0, CardVB, 0, sizeof(CardVertex));
		d->SetStreamSource(1, InstVB, 0, sizeof(TreeInstance));
		d->SetIndices(CardIB);
		d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | InstanceCount);
		d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
		d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 8, 0, 4);

		d->SetStreamSourceFreq(0, savedFreq0);
		d->SetStreamSourceFreq(1, savedFreq1);
		d->SetStreamSource(1, savedVB1, savedOff1, savedStr1);
		if (savedVB1) savedVB1->Release();
	}
}
