#pragma once
// 2026-11-12 Puddles, map mode (README.md "Puddle map"; Compute/PuddleMap.comp): the large-scale puddle terms (the noise field, the
// rim wander, the rain cover as the top surface's height) computed once per texel of a world-space map round the camera on the NVR
// DXVK extension, instead of in every surface pixel, where they cost the ps_3_0 shaders the temps that forced the lean and dry
// fallbacks (and left 7-layer land, which has no sampler for the ortho map, without any rain cover).
//
// Mode: WETNESS_EFFECTS 3 (ShaderRecord::BuildDefines) when [Shaders.Puddles.Status] and [Shaders.WetnessEffects.Status] are on at
// startup, [Shaders.Puddles.Main] MapMode is on and the extension is there (ModeAtStart). Otherwise WETNESS_EFFECTS 2: the in-shader
// path, unchanged.
//
// TESR_PuddleMap: rgba16f 2048 x 1024, two cascades of 1024 x 1024 side by side (near 4 units a texel, far 20), each snapped to its own
// texel grid round the camera. Built only on frames the ortho map was drawn (its transform is this frame's then), when a cascade
// should move (the camera left the middle quarter of its window), the settings it uses changed, or it holds nothing yet. The
// surfaces read it through TESR_PuddleMapNear / Far, which always describe the BUILT windows (Wetness.hlsl PuddleMapRead):
//   Near: xy the near cascade's texel (0, 0) corner (world), z 1 / its extent (world units), w the camera z the heights are relative to
//   Far:  xyz the same for the far cascade, w 1 = the map holds data (0: the surfaces take "no water, open sky")
#include <algorithm>
#include <cmath>
#include "../../core/DxvkCompute.h"
#include "../../core/GpuProfiler.h"

namespace PuddleMap {
	inline constexpr UINT kSize = 1024;						// one cascade's side, texels
	inline constexpr float kTexel[2] = { 4.0f, 20.0f };		// world units per texel: near, far
	inline bool MapMode = false;							// decided once, before the first shader compiles (ModeAtStart)
	inline bool ModeDecided = false;
	inline IDirect3DTexture9* Texture = nullptr;
	inline IDirect3DSurface9* Surface = nullptr;
	inline ID3D9NvrExtPipeline* Pipeline = nullptr;
	inline bool Failed = false;
	inline D3DXVECTOR4 Near(0.0f, 0.0f, 0.0f, 0.0f), Far(0.0f, 0.0f, 0.0f, 0.0f);	// TESR_PuddleMapNear / Far
	inline float Built[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };		// the built windows' corners
	inline float BuiltCameraZ = 0.0f, BuiltInvSize = -1.0f, BuiltQuality = -1.0f;
	inline bool HasData = false;
	inline UINT Builds = 0;

	struct Push {
		float cascade[2][4];
		float ortho[3][4];
		float inverse[3][4];
		float camera[4];
		float params[4];
		INT32 orthoSize[2];
	};
	static_assert(sizeof(Push) == 168, "PuddleMap.comp push block");
	inline const NVR_EXT_BINDING_DESC Bindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// TESR_OrthoMapBuffer
		{ 1, 1, NVR_EXT_BIND_STORAGE_IMAGE, 1, NVR_EXT_VIEW_2D, TRUE,  0 },	// TESR_PuddleMap
	};

	// ShaderRecord::BuildDefines: the mode, decided once (the shaders of a session all compile one way)
	inline bool ModeAtStart() {
		if (!ModeDecided) {
			ModeDecided = true;
			MapMode = TheSettingManager->GetSettingI("Shaders.Puddles.Status", "Enabled") && TheSettingManager->GetSettingI("Shaders.WetnessEffects.Status", "Enabled") &&
				TheSettingManager->GetSettingI("Shaders.Puddles.Main", "MapMode") != 0 && DxvkCompute::Available() && DxvkCompute::HasSpirv("PuddleMap");
			Logger::Log("UNOFFICIAL Puddles: %s", MapMode ? "map mode (WETNESS_EFFECTS 3: the large-scale terms in Compute\\PuddleMap on the NVR DXVK extension)" :
				"in-shader mode (WETNESS_EFFECTS 2)");
		}
		return MapMode;
	}

	inline void RegisterConstants() {
		TheShaderManager->RegisterConstant("TESR_PuddleMapNear", &Near);
		TheShaderManager->RegisterConstant("TESR_PuddleMapFar", &Far);
	}

	// the texture, only in map mode (the shaders bind TESR_PuddleMap by name)
	inline void RegisterTextures() {
		if (!ModeAtStart() || Texture) return;
		TheTextureManager->InitTexture("TESR_PuddleMap", &Texture, &Surface, kSize * 2, kSize, D3DFMT_A16B16G16R16F);
		Logger::Log("UNOFFICIAL Puddles: the puddle map %s (%ux%u rgba16f)", Texture ? "is made" : "was REFUSED: no water and no rain cover this session",
			kSize * 2, kSize);
		if (!Texture) Failed = true;
	}

	inline void Fail(const char* why) {
		Logger::Log("UNOFFICIAL Puddles: the puddle map is off for this session (%s): the surfaces see no standing water and open sky.", why);
		Failed = true; HasData = false;
		DxvkCompute::Drop(Pipeline);
	}

	// 2026-11-12 diagnostic: the frame after the first build, the near cascade's texels round the camera are read back once and logged
	// (field, top height relative to the build's camera z, wander), with the camera's own height: what the map holds, in numbers.
	inline UINT ReadbackAfter = 0;
	inline bool ReadbackDone = false;
	inline float Half(unsigned short h) {
		const unsigned sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
		float v = exp == 0 ? std::ldexp((float)man, -24) : exp == 31 ? 65504.0f : std::ldexp((float)(man | 0x400), (int)exp - 25);
		return sign ? -v : v;
	}
	inline void Readback() {
		ReadbackDone = true;
		IDirect3DDevice9* device = TheRenderManager->device;
		IDirect3DSurface9* copy = nullptr;
		if (!Surface || FAILED(device->CreateOffscreenPlainSurface(kSize * 2, kSize, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &copy, nullptr))) return;
		if (SUCCEEDED(device->GetRenderTargetData(Surface, copy))) {
			D3DLOCKED_RECT lock;
			if (SUCCEEDED(copy->LockRect(&lock, nullptr, D3DLOCK_READONLY))) {
				const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
				const int cx = (int)((camera.x - Built[0][0]) / kTexel[0]), cy = (int)((camera.y - Built[0][1]) / kTexel[0]);
				for (int dy = -64; dy <= 64; dy += 64)
					for (int dx = -64; dx <= 64; dx += 64) {
						const int x = std::clamp(cx + dx, 0, (int)kSize - 1), y = std::clamp(cy + dy, 0, (int)kSize - 1);
						const unsigned short* t = (const unsigned short*)((const char*)lock.pBits + y * lock.Pitch) + x * 4;
						Logger::Log("UNOFFICIAL Puddles: map texel near (%d, %d): field %.3f, top %.1f (world %.1f), wander %.3f | camera z %.1f, built at z %.1f",
							x, y, Half(t[0]), Half(t[1]), Half(t[1]) + BuiltCameraZ, Half(t[2]), camera.z, BuiltCameraZ);
					}
				copy->UnlockRect();
			}
		}
		copy->Release();
	}

	// once a frame, before the world (PuddlesShaders::UpdateConstants): the readers' constants from the BUILT windows
	inline void UpdateConstants() {
		if (!MapMode) return;
		if (HasData && !ReadbackDone && ReadbackAfter && TheFrameRateManager->FrameNumber > ReadbackAfter) Readback();	// once a session (the log)
		Near = D3DXVECTOR4(Built[0][0], Built[0][1], 1.0f / (kTexel[0] * kSize), BuiltCameraZ);
		Far = D3DXVECTOR4(Built[1][0], Built[1][1], 1.0f / (kTexel[1] * kSize), HasData && !Failed ? 1.0f : 0.0f);
	}

	// after the ortho map's draw in a frame it was drawn (ShadowManager::RenderShadowMaps; the transform is this frame's camera's)
	inline void Build(IDirect3DDevice9* device, const D3DXMATRIX& cameraToOrtho, IDirect3DTexture9* ortho, UINT orthoRes, float invSize, float quality) {
		if (!MapMode || Failed || !Texture || !ortho || !orthoRes) return;
		if (!DxvkCompute::Available()) { Fail("no NVR DXVK extension"); return; }
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		bool rebuild = !HasData || invSize != BuiltInvSize || quality != BuiltQuality;
		float wanted[2][2];
		for (int c = 0; c < 2; c++) {
			const float extent = kTexel[c] * kSize;
			const float cx = Built[c][0] + extent * 0.5f, cy = Built[c][1] + extent * 0.5f;
			const bool move = !HasData || std::fabs(camera.x - cx) > extent * 0.125f || std::fabs(camera.y - cy) > extent * 0.125f;
			wanted[c][0] = move ? std::floor((camera.x - extent * 0.5f) / kTexel[c]) * kTexel[c] : Built[c][0];
			wanted[c][1] = move ? std::floor((camera.y - extent * 0.5f) / kTexel[c]) * kTexel[c] : Built[c][1];
			rebuild |= move;
		}
		// the cover heights change with the world's top surfaces and the camera's height: the near map also on a vertical move, and every
		// 8th ortho draw anyway (things drawn in, doors, a cell streaming in)
		rebuild |= std::fabs(camera.z - BuiltCameraZ) > 64.0f || (Builds % 8) == 7;
		Builds++;
		if (!rebuild) return;
		if (!Pipeline) {
			Pipeline = DxvkCompute::CreatePipeline("PuddleMap", Bindings, _countof(Bindings), sizeof(Push), false);
			if (!Pipeline) { Fail("the PuddleMap pipeline was refused"); return; }
		}
		D3DXMATRIX inverse;
		if (!D3DXMatrixInverse(&inverse, nullptr, &cameraToOrtho)) return;
		Push push = {};
		for (int c = 0; c < 2; c++) {
			push.cascade[c][0] = wanted[c][0]; push.cascade[c][1] = wanted[c][1]; push.cascade[c][2] = kTexel[c];
			// w: the same window as the last build (its texels may keep their cover where the rain map has nothing now; the heights are
			// relative to the build's camera z, so a vertical move counts as moved too)
			push.cascade[c][3] = HasData && wanted[c][0] == Built[c][0] && wanted[c][1] == Built[c][1] && std::fabs(camera.z - BuiltCameraZ) < 4.0f ? 1.0f : 0.0f;
		}
		// the shaders use the matrix row_major as mul(float4(p, 1), M): output j = dot(p, column j)
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 4; k++) { push.ortho[r][k] = cameraToOrtho.m[k][r]; push.inverse[r][k] = inverse.m[k][r]; }
		push.camera[0] = camera.x; push.camera[1] = camera.y; push.camera[2] = camera.z; push.camera[3] = 0.0f;
		push.params[0] = invSize; push.params[1] = quality; push.params[2] = 1.0f / (float)orthoRes; push.params[3] = 1.0f;
		push.orthoSize[0] = (INT32)orthoRes; push.orthoSize[1] = (INT32)orthoRes;
		NVR_EXT_RESOURCE_BINDING bindings[2] = {};
		bindings[0] = { 0, NVR_EXT_BIND_SAMPLED_IMAGE, ortho,   0, 0, 0, FALSE };
		bindings[1] = { 1, NVR_EXT_BIND_STORAGE_IMAGE, Texture, 0, 0, 0, FALSE };
		NVR_EXT_DISPATCH_DESC dispatch = {};
		dispatch.pPipeline = Pipeline;
		dispatch.BindingCount = 2;
		dispatch.pBindings = bindings;
		dispatch.pPushData = &push;
		dispatch.PushDataSize = sizeof(push);
		dispatch.GroupsX = kSize * 2 / 8;
		dispatch.GroupsY = kSize / 8;
		dispatch.GroupsZ = 1;
		HRESULT result;
		{
			static GpuTimer timer("  Puddle map");
			GpuProfileScope gpu(timer, device);
			DxvkCompute::PreLoad(bindings, 2);
			result = DxvkCompute::Extension()->Dispatch(&dispatch);
		}
		if (FAILED(result)) { Fail("the dispatch failed"); return; }
		for (int c = 0; c < 2; c++) { Built[c][0] = wanted[c][0]; Built[c][1] = wanted[c][1]; }
		BuiltCameraZ = camera.z; BuiltInvSize = invSize; BuiltQuality = quality;
		if (!HasData) {
			Logger::Log("UNOFFICIAL Puddles: the puddle map is built (first time this session). Ortho %u texels; transform rows x (%.5f %.5f %.5f %.3f) y (%.5f %.5f %.5f %.3f) z (%.6f %.6f %.6f %.4f); camera %.1f %.1f %.1f",
				orthoRes, push.ortho[0][0], push.ortho[0][1], push.ortho[0][2], push.ortho[0][3], push.ortho[1][0], push.ortho[1][1], push.ortho[1][2], push.ortho[1][3],
				push.ortho[2][0], push.ortho[2][1], push.ortho[2][2], push.ortho[2][3], camera.x, camera.y, camera.z);
			ReadbackAfter = TheFrameRateManager->FrameNumber + 2;
		}
		HasData = true;
		UpdateConstants();	// the world after this reads the new windows
	}
}
