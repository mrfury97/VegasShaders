#pragma once

// UNOFFICIAL Phase 3 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Hardware-instanced low-poly 3D rock boulders, canyon outcrops & cliff slabs beyond uGridsToLoad = 5.
// Rendered in 1 DrawIndexedPrimitive call via SetStreamSourceFreq into GBufferV2 + hardware reversed depth,
// picking up DeferredSun lighting, terrain shadows, and VolumetricFog automatically.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../effects/Terrain/LandHeights.h"

namespace DistantLandRocks {
	struct RockVertex {
		float x, y, z;
		float nx, ny, nz;
		float u, v;
	};
	struct RockInstance {
		float x, y, z, scale;
		float w, h, rotZ, style;
	};

	inline bool Initialized = false;
	inline bool Failed      = false;

	inline IDirect3DVertexShader9*      RockVS   = nullptr;
	inline IDirect3DPixelShader9*       RockPS   = nullptr;
	inline IDirect3DVertexDeclaration9* InstDecl = nullptr;
	inline IDirect3DVertexBuffer9*      MeshVB   = nullptr;
	inline IDirect3DIndexBuffer9*       MeshIB   = nullptr;
	inline IDirect3DVertexBuffer9*      InstVB   = nullptr;
	inline UINT                         InstanceCount = 0;

	static constexpr UINT kVertCount = 15;
	static constexpr UINT kPrimCount = 24;

	inline void ReleaseInstances() {
		if (InstVB) { InstVB->Release(); InstVB = nullptr; }
		InstanceCount = 0;
	}

	inline bool CompileShaders(IDirect3DDevice9* d) {
		const std::string path = std::string(Paths::ShadersDir()) + "DistantLandRocks.hlsl";
		auto compile = [&](const char* entry, const char* target, ID3DXBuffer** code) -> bool {
			ID3DXBuffer* err = nullptr;
			const HRESULT hr = D3DXCompileShaderFromFileA(path.c_str(), nullptr, nullptr, entry, target, 0, code, &err, nullptr);
			if (FAILED(hr) || !*code) {
				Logger::Log("[ERROR] UNOFFICIAL DistantLandRocks: %s (%s) compile failed in %s (%s)",
					entry, target, path.c_str(), err ? (const char*)err->GetBufferPointer() : "no compiler output");
				if (err) err->Release();
				return false;
			}
			if (err) err->Release();
			return true;
		};
		ID3DXBuffer* vsBuf = nullptr;
		ID3DXBuffer* psBuf = nullptr;
		if (!compile("RockVS", "vs_3_0", &vsBuf) || !compile("RockPS", "ps_3_0", &psBuf)) {
			if (vsBuf) vsBuf->Release();
			if (psBuf) psBuf->Release();
			return false;
		}
		const HRESULT hr1 = d->CreateVertexShader((const DWORD*)vsBuf->GetBufferPointer(), &RockVS);
		const HRESULT hr2 = d->CreatePixelShader((const DWORD*)psBuf->GetBufferPointer(), &RockPS);
		vsBuf->Release();
		psBuf->Release();
		if (FAILED(hr1) || FAILED(hr2) || !RockVS || !RockPS) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandRocks: CreateVertexShader/PixelShader failed (%08lX %08lX)", hr1, hr2);
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
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 1, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
			{ 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
			D3DDECL_END()
		};
		if (FAILED(d->CreateVertexDeclaration(kDecl, &InstDecl)) || !InstDecl) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandRocks: CreateVertexDeclaration failed");
			Failed = true; return false;
		}

		// Irregular 15-vertex faceted desert boulder/outcrop mesh sunk slightly (-0.22) at base so rocks embed cleanly into sloped terrain.
		static const float kPos[kVertCount][3] = {
			// Bottom skirt ring (7 verts, z = -0.22)
			{  0.54f,  0.04f, -0.22f },
			{  0.32f,  0.46f, -0.22f },
			{ -0.22f,  0.48f, -0.22f },
			{ -0.52f,  0.14f, -0.22f },
			{ -0.44f, -0.36f, -0.22f },
			{  0.08f, -0.50f, -0.22f },
			{  0.46f, -0.32f, -0.22f },
			// Upper shoulder ring (7 verts, varied heights)
			{  0.34f,  0.02f,  0.58f },
			{  0.18f,  0.29f,  0.66f },
			{ -0.14f,  0.30f,  0.52f },
			{ -0.33f,  0.08f,  0.64f },
			{ -0.28f, -0.24f,  0.46f },
			{  0.05f, -0.31f,  0.60f },
			{  0.30f, -0.20f,  0.50f },
			// Craggy apex (1 vert)
			{  0.02f,  0.03f,  0.88f },
		};
		static const uint16_t kIdx[kPrimCount * 3] = {
			// Side quads (14 tris)
			0, 1, 8,   0, 8, 7,
			1, 2, 9,   1, 9, 8,
			2, 3, 10,  2, 10, 9,
			3, 4, 11,  3, 11, 10,
			4, 5, 12,  4, 12, 11,
			5, 6, 13,  5, 13, 12,
			6, 0, 7,   6, 7, 13,
			// Upper cap (7 tris)
			7, 8, 14,  8, 9, 14,  9, 10, 14,  10, 11, 14,  11, 12, 14,  12, 13, 14,  13, 7, 14,
			// Extra cap fan closure -> 24 tris (bottom open/sunk into terrain; add 3 far base tris or mirror top for watertightness? 21 tris side+cap; pad 3 bottom anchor tris)
			0, 4, 2,   0, 6, 4,   2, 4, 3,
		};

		RockVertex verts[kVertCount] = {};
		for (UINT i = 0; i < kVertCount; ++i) {
			float nx = kPos[i][0], ny = kPos[i][1], nz = (i < 7) ? 0.20f : (i < 14 ? 0.62f : 1.0f);
			const float len = sqrtf(nx * nx + ny * ny + nz * nz);
			verts[i] = RockVertex{
				kPos[i][0], kPos[i][1], kPos[i][2],
				nx / len, ny / len, nz / len,
				kPos[i][0] + 0.5f, 1.0f - (kPos[i][2] + 0.22f) / 1.10f
			};
		}

		void* ptr = nullptr;
		if (FAILED(d->CreateVertexBuffer(sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &MeshVB, nullptr)) || !MeshVB ||
			FAILED(MeshVB->Lock(0, sizeof(verts), &ptr, 0))) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandRocks: CreateVertexBuffer(MeshVB) failed");
			Failed = true; return false;
		}
		memcpy(ptr, verts, sizeof(verts));
		MeshVB->Unlock();

		if (FAILED(d->CreateIndexBuffer(sizeof(kIdx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &MeshIB, nullptr)) || !MeshIB ||
			FAILED(MeshIB->Lock(0, sizeof(kIdx), &ptr, 0))) {
			Logger::Log("[ERROR] UNOFFICIAL DistantLandRocks: CreateIndexBuffer(MeshIB) failed");
			Failed = true; return false;
		}
		memcpy(ptr, kIdx, sizeof(kIdx));
		MeshIB->Unlock();

		return true;
	}

	inline void UploadInstances(IDirect3DDevice9* d, std::vector<LandHeights::RockRef>& refs) {
		ReleaseInstances();
		if (refs.empty()) return;

		static const float kBaseW[3] = { 125.0f, 195.0f, 320.0f };
		static const float kBaseH[3] = {  78.0f, 128.0f, 215.0f };

		std::vector<RockInstance> inst(refs.size());
		for (size_t i = 0; i < refs.size(); ++i) {
			const auto& r = refs[i];
			const int s = (r.style < 3) ? r.style : 1;
			const float sScale = std::clamp(r.scale, 0.50f, 2.0f);
			inst[i] = RockInstance{ r.x, r.y, r.z, sScale, kBaseW[s], kBaseH[s], r.rotZ, (float)s };
		}
		std::vector<LandHeights::RockRef>().swap(refs);

		const UINT bytes = (UINT)(inst.size() * sizeof(RockInstance));
		IDirect3DVertexBuffer9* vb = nullptr;
		void* ptr = nullptr;
		if (SUCCEEDED(d->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr)) && vb &&
			SUCCEEDED(vb->Lock(0, bytes, &ptr, 0))) {
			memcpy(ptr, inst.data(), bytes);
			vb->Unlock();
			InstVB = vb;
			InstanceCount = (UINT)inst.size();
			Logger::Log("UNOFFICIAL DistantLandRocks: uploaded %u hardware-instanced distant rocks/cliffs (%.2f MB)",
				InstanceCount, (double)bytes / (1024.0 * 1024.0));
		} else {
			if (vb) vb->Release();
			Logger::Log("[WARNING] UNOFFICIAL DistantLandRocks: CreateVertexBuffer(InstVB, %u bytes) failed", bytes);
		}
	}

	inline void RenderInGBuffer(IDirect3DDevice9* d, const float vsConst8[8][4], const float psConst15[15][4]) {
		if (Failed || !InstVB || InstanceCount == 0 || !InitResources(d)) return;

		static GpuTimer timer("DistantLand rocks", 12);
		GpuProfileScope gpu(timer, d);

		IDirect3DVertexBuffer9* savedVB1 = nullptr; UINT savedOff1 = 0, savedStr1 = 0;
		d->GetStreamSource(1, &savedVB1, &savedOff1, &savedStr1);
		UINT savedFreq0 = 1, savedFreq1 = 1;
		d->GetStreamSourceFreq(0, &savedFreq0);
		d->GetStreamSourceFreq(1, &savedFreq1);

		d->SetVertexShaderConstantF(0, &vsConst8[0][0], 8);
		d->SetPixelShaderConstantF(0, &psConst15[0][0], 15);

		d->SetVertexShader(RockVS);
		d->SetPixelShader(RockPS);
		d->SetVertexDeclaration(InstDecl);
		d->SetStreamSource(0, MeshVB, 0, sizeof(RockVertex));
		d->SetStreamSource(1, InstVB, 0, sizeof(RockInstance));
		d->SetIndices(MeshIB);
		d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | InstanceCount);
		d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
		d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kVertCount, 0, kPrimCount);

		d->SetStreamSourceFreq(0, savedFreq0);
		d->SetStreamSourceFreq(1, savedFreq1);
		d->SetStreamSource(1, savedVB1, savedOff1, savedStr1);
		if (savedVB1) savedVB1->Release();
	}
}
