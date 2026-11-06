#pragma once

// UNOFFICIAL 2026-11-06 forward contact shadows ([Shaders.ShadowsExteriors.ScreenSpace] Bend + InLighting; user: "lets try porting
// the skyrim CS screen space shadows as forward to fnv"). Skyrim Community Shaders' Screen-Space Shadows (Bend Studio's, Apache 2.0,
// Copyright 2023 Sony Interactive Entertainment) multiply only the SUN term inside the lit shaders. Skyrim draws a depth pre-pass;
// New Vegas does not, so here:
//   1. after the frame's world (effect chain step 11, where the old contact pass runs), Compute/BendContact.comp marches Bend's rays
//      over this frame's depth on the NVR DXVK extension and writes TESR_ContactForward (rg16f: visibility, view depth), and the
//      camera it used is kept (Made);
//   2. next frame, before the world, UpdateConstants makes TESR_ContactReproject: a camera-relative point of the new frame -> the
//      made frame's clip space (render scale and TAA jitter folded in, as RtLampShadows::Submit);
//   3. every lit shader that opts in (Includes/Shadow.hlsl SunContactForward, inside GetSunShadow) takes the visibility where the
//      stored depth is its own (within DepthTolerance), else none: one frame late on things that move, nothing wrong elsewhere.
// The old screen-space contact pass then writes "no contact" into the shadow buffer (ScreenSpaceData.x 0 for it), so nothing is
// darkened twice. Without the DXVK fork, or if anything here fails, Data.x stays 0 (the shaders take 1) and the old pass runs.
// Settings (all under kSection): Bend, InLighting (compiled in at start, with [Main] ForwardShadows), SampleMultiplier,
// SurfaceThickness, BilinearThreshold, ShadowContrast (CS's four, CS's defaults and ranges), DepthTolerance.

#include <algorithm>
#include <cmath>
#include "DxvkCompute.h"
#include "RenderRect.h"
#include "GpuProfiler.h"
#include "BendDispatch.h"
#include "../effects/TAA/TAA.h"	// TAAEffect::DepthJitterUV

namespace BendContact {
	inline constexpr const char* kSection = "Shaders.ShadowsExteriors.ScreenSpace";

	// The settings that compile the lookups into the lit shaders (ShaderRecord::BuildDefines appends SUN_CONTACT_FORWARD with it)
	inline bool WantedAtStart() {
		return TheSettingManager->GetSettingI(kSection, "Bend") != 0 && TheSettingManager->GetSettingI(kSection, "InLighting") != 0 &&
			TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") != 0;
	}

	inline bool CompiledIn = false;					// WantedAtStart, read once when the texture is made
	inline IDirect3DTexture9* Texture = nullptr;	// TESR_ContactForward
	inline IDirect3DSurface9* Surface = nullptr;
	inline D3DXMATRIX Reproject;					// TESR_ContactReproject (4 registers, row_major in the shaders)
	inline D3DXVECTOR4 Data(0.0f, 0.02f, 64.0f, 0.0f);	// TESR_ContactForwardData: x on, y depth tolerance (fraction), z view-model cutoff
	inline ID3D9NvrExtPipeline* Pipeline = nullptr;
	inline bool Tried = false, Failed = false;
	inline UINT Frames = 0;
	inline UINT ProducedFrame = 0xFFFFFFFF;			// the frame the newest result was made in

	struct Camera { D3DXMATRIX view; D3DXVECTOR3 location; float p11, p22, sx, sy, jx, jy; bool valid; };
	inline Camera Made = {};

	struct Push {
		float	lightCoordinate[4];
		INT32	waveOffset[2];
		INT32	size[2];
		float	surfaceThickness, bilinearThreshold, shadowContrast, nearZ, farZ, invertedDepth;
		UINT32	sampleCount, readCount;
	};
	static_assert(sizeof(Push) == 64, "BendContact.comp push block");

	inline const NVR_EXT_BINDING_DESC Bindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// CombineDepth
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },	// the view model's depth
		{ 1, 2, NVR_EXT_BIND_STORAGE_IMAGE, 2, NVR_EXT_VIEW_2D, TRUE,  0 },	// TESR_ContactForward
	};

	// ShadowsExteriorEffect::RegisterConstants
	inline void RegisterConstants() {
		D3DXMatrixIdentity(&Reproject);
		TheShaderManager->RegisterConstant("TESR_ContactReproject", (D3DXVECTOR4*)&Reproject);
		TheShaderManager->RegisterConstant("TESR_ContactForwardData", &Data);
	}

	// ShadowsExteriorEffect::RegisterTextures: the result texture, screen sized, only when compiled in
	inline void RegisterTextures() {
		CompiledIn = WantedAtStart();
		if (!CompiledIn || Texture) return;
		TheTextureManager->InitTexture("TESR_ContactForward", &Texture, &Surface, TheRenderManager->width, TheRenderManager->height, D3DFMT_G16R16F);
		Logger::Log("UNOFFICIAL Bend contact shadows in the lit shaders: %s (%ux%u)", Texture ? "result texture made" : "the result texture was REFUSED: off",
			(UINT)TheRenderManager->width, (UINT)TheRenderManager->height);
		if (!Texture) CompiledIn = false;
	}

	// ShadowsExteriorEffect::UpdateConstants (before the world): the newest result, seen from this frame's camera
	inline void UpdateConstants() {
		Data.x = 0.0f;
		if (!CompiledIn || Failed || !Texture || !Made.valid) return;
		if (!TheShaderManager->GameState.isExterior) { Made.valid = false; return; }
		const UINT now = TheFrameRateManager->FrameNumber;
		if (ProducedFrame == 0xFFFFFFFF || now - ProducedFrame > 1) return;	// not made last frame (faded sun, menus): stale
		const D3DXMATRIX& v = Made.view;
		const D3DXVECTOR3 right(v._11, v._21, v._31), up(v._12, v._22, v._32), forward(v._13, v._23, v._33);
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		const D3DXVECTOR3 d(camera.x - Made.location.x, camera.y - Made.location.y, camera.z - Made.location.z);
		const float rows[3][3] = { { right.x, up.x, forward.x }, { right.y, up.y, forward.y }, { right.z, up.z, forward.z } };
		for (int i = 0; i < 3; i++) {
			Reproject.m[i][0] = rows[i][0] * Made.p11;
			Reproject.m[i][1] = rows[i][1] * Made.p22;
			Reproject.m[i][2] = 0.0f;
			Reproject.m[i][3] = rows[i][2];
		}
		Reproject.m[3][0] = D3DXVec3Dot(&d, &right) * Made.p11;
		Reproject.m[3][1] = D3DXVec3Dot(&d, &up) * Made.p22;
		Reproject.m[3][2] = 0.0f;
		Reproject.m[3][3] = D3DXVec3Dot(&d, &forward);
		// RenderScale and the made frame's depth jitter, as RtLampShadows::Submit: uv' = s (uv - j)
		if (Made.sx != 1.0f || Made.sy != 1.0f || Made.jx != 0.0f || Made.jy != 0.0f)
			for (int i = 0; i < 4; i++) {
				Reproject.m[i][0] = Made.sx * Reproject.m[i][0] + (Made.sx - 1.0f - 2.0f * Made.sx * Made.jx) * Reproject.m[i][3];
				Reproject.m[i][1] = Made.sy * Reproject.m[i][1] - (Made.sy - 1.0f - 2.0f * Made.sy * Made.jy) * Reproject.m[i][3];
			}
		Data.y = std::clamp(TheSettingManager->GetSettingF(kSection, "DepthTolerance"), 0.002f, 0.2f);
		// Where it starts (2026-11-06): past the sun's shadow maps, which do the near field. StartDistance 0 = [ShadowMaps] Distance - FadeLength.
		const int debug = std::clamp(TheSettingManager->GetSettingI(kSection, "BendDebug"), 0, 2);
		const float fadeLength = (std::max)(TheSettingManager->GetSettingF(kSection, "FadeLength"), 1.0f);
		float start = TheSettingManager->GetSettingF(kSection, "StartDistance");
		if (start <= 0.0f) start = TheSettingManager->GetSettingF("Shaders.ShadowsExteriors.ShadowMaps", "Distance") - fadeLength;
		Data.z = debug == 2 ? 64.0f : (std::max)(start, 64.0f);
		Data.w = 1.0f / fadeLength;
		Data.x = 1.0f + (float)debug;
	}

	// CS's sample count: 60 x SampleMultiplier at 1920x1080, with the square root of the drawn area, a multiple of 8
	inline UINT32 Samples(UINT w, UINT h) {
		const int multiplier = std::clamp(TheSettingManager->GetSettingI(kSection, "SampleMultiplier"), 1, 4);
		UINT32 samples = (UINT32)ceilf((float)multiplier * 60.0f * sqrtf(((float)w * (float)h) / (1920.0f * 1080.0f)));
		samples = (std::max<UInt32>)(((samples + 7u) / 8u) * 8u, 8u);
		return (std::min<UInt32>)(samples, 248u);	// the shader's groupshared size (MAX_SAMPLES)
	}

	// LOD only (2026-11-06, hardcoded): whether the draw being set up is object LOD (its geometry under the object LOD root), into NVR_ContactDraw
	// (c153; the object shaders only) every pass (RenderPassStandardHook / Skinned): not cached, the land's RT lamps raw-write c146-c165.
	inline bool IsObjectLod(NiAVObject* object) {
		NiNode* root = BGSTerrainManager::GetRootObjectLODNode();
		if (!root) return false;
		for (int i = 0; object && i < 32; i++) {
			if (object == (NiAVObject*)root) return true;
			object = (NiAVObject*)object->m_parent;
		}
		return false;
	}
	inline void ForDraw(IDirect3DDevice9* device, NiGeometry* geometry, bool world) {
		if (!CompiledIn || Data.x < 0.5f) return;
		const float v = world && geometry && IsObjectLod((NiAVObject*)geometry) ? 1.0f : 0.0f;
		const float c[4] = { v, 0.0f, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(153, c, 1);
	}

	// Nothing is made this frame (Bend did not run, the shadow effect is off, indoors): next frame's shaders take none
	inline void Invalidate() { Made.valid = false; Data.x = 0.0f; }

	// Effect chain step 11, outdoors, in place of the old contact pass: the march into TESR_ContactForward. False: not run
	// (the old pass then keeps its contact).
	inline bool Produce(IDirect3DDevice9* Device) {
		if (!CompiledIn || Failed || !Texture || !DxvkCompute::Available()) return false;
		IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
		IDirect3DTexture9* viewModel = TheTextureManager->DepthTextureViewModel;
		const D3DXVECTOR4* depthConstants = nullptr;
		{
			auto found = TheShaderManager->ConstantsTable.find("TESR_DepthConstants");
			if (found != TheShaderManager->ConstantsTable.end()) depthConstants = found->second;
		}
		if (!depth || !viewModel || !depthConstants) return false;
		if (!Pipeline) {
			if (Tried) return false;
			Tried = true;
			Pipeline = DxvkCompute::CreatePipeline("BendContact", Bindings, _countof(Bindings), sizeof(Push), false);
			if (!Pipeline) {
				Logger::Log("UNOFFICIAL Bend contact shadows: Compute\\BendContact.spv could not be set up; the old contact pass is used");
				Failed = true;
				return false;
			}
		}

		// Bend's light coordinate: the direction toward the sun (w 0) through the view and projection, not divided
		const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.SunDir;
		const D3DXVECTOR4 towardSun(sun.x, sun.y, sun.z, 0.0f);
		D3DXVECTOR4 view, clip;
		D3DXVec4Transform(&view, &towardSun, &TheRenderManager->viewMatrix);
		D3DXVec4Transform(&clip, &view, &TheRenderManager->projMatrix);
		if (D3DXVec4LengthSq(&view) < 0.25f || !_finite(clip.x) || !_finite(clip.y) || !_finite(clip.w)) return false;

		const UINT w = RenderRect::Width(TheRenderManager->width), h = RenderRect::Height(TheRenderManager->height);
		const int size[2] = { (int)w, (int)h }, minBounds[2] = { 0, 0 };
		const float lightProjection[4] = { clip.x, clip.y, clip.z, clip.w };
		const Bend::DispatchList list = Bend::BuildDispatchList(lightProjection, size, minBounds, size);

		const float nearZ = TheRenderManager->CameraData.x, farZ = TheRenderManager->CameraData.y;
		Push push = {};
		push.lightCoordinate[0] = list.LightCoordinate_Shader[0];
		push.lightCoordinate[1] = list.LightCoordinate_Shader[1];
		push.lightCoordinate[2] = -nearZ / (farZ - nearZ);	// a direction's depth in the non-linear depth the shader makes
		push.lightCoordinate[3] = list.LightCoordinate_Shader[3];
		push.size[0] = size[0]; push.size[1] = size[1];
		push.surfaceThickness = std::clamp(TheSettingManager->GetSettingF(kSection, "SurfaceThickness"), 0.005f, 0.05f);
		push.bilinearThreshold = std::clamp(TheSettingManager->GetSettingF(kSection, "BilinearThreshold"), 0.02f, 1.0f);
		push.shadowContrast = std::clamp(TheSettingManager->GetSettingF(kSection, "ShadowContrast"), 0.0f, 4.0f);
		push.nearZ = nearZ;
		push.farZ = farZ;
		push.invertedDepth = depthConstants->z;
		push.sampleCount = Samples(w, h);
		push.readCount = push.sampleCount / 64u + 2u;

		// The lit shaders read the texture on s12 / s13 and leave it bound: off those stages before it is written
		for (DWORD stage : { 12u, 13u, 15u }) {
			IDirect3DBaseTexture9* bound = nullptr;
			if (SUCCEEDED(Device->GetTexture(stage, &bound)) && bound) {
				const bool same = bound == Texture;
				bound->Release();
				if (same) { Device->SetTexture(stage, nullptr); TheRenderManager->renderState->NoteDeviceTexture(stage, nullptr); }
			}
		}
		// Pixels no wave reaches keep "lit, no data"
		Device->ColorFill(Surface, nullptr, D3DCOLOR_ARGB(0, 255, 0, 0));

		NVR_EXT_RESOURCE_BINDING bindings[] = {
			{ 0, NVR_EXT_BIND_SAMPLED_IMAGE, depth,     0, 0, 0, FALSE },
			{ 1, NVR_EXT_BIND_SAMPLED_IMAGE, viewModel, 0, 0, 0, FALSE },
			{ 2, NVR_EXT_BIND_STORAGE_IMAGE, Texture,   0, 0, 0, FALSE },
		};
		{
			static GpuTimer timer("  Bend contact (compute)");
			GpuProfileScope gpu(timer, Device);
			DxvkCompute::PreLoad(bindings, _countof(bindings));
			for (int i = 0; i < list.DispatchCount; i++) {
				const Bend::DispatchData& dd = list.Dispatch[i];
				push.waveOffset[0] = dd.WaveOffset_Shader[0];
				push.waveOffset[1] = dd.WaveOffset_Shader[1];
				NVR_EXT_DISPATCH_DESC dispatch = {};
				dispatch.pPipeline = Pipeline;
				dispatch.BindingCount = _countof(bindings);
				dispatch.pBindings = bindings;
				dispatch.pPushData = &push;
				dispatch.PushDataSize = sizeof(push);
				dispatch.GroupsX = (UINT)dd.WaveCount[0];
				dispatch.GroupsY = (UINT)dd.WaveCount[1];
				dispatch.GroupsZ = (UINT)dd.WaveCount[2];
				const HRESULT result = DxvkCompute::Extension()->Dispatch(&dispatch);
				if (FAILED(result)) {
					Logger::Log("UNOFFICIAL Bend contact shadows: a dispatch failed (%08lX); off for this session, the old contact pass is used", result);
					Failed = true;
					Made.valid = false;
					return false;
				}
			}
		}

		D3DXVECTOR2 jitter(0.0f, 0.0f);
		if (TAAEffect* taa = TheShaderManager->Effects.TAA) jitter = taa->DepthJitterUV();
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		Made = { TheRenderManager->viewMatrix, D3DXVECTOR3(camera.x, camera.y, camera.z), TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22,
			(float)w / (float)TheRenderManager->width, (float)h / (float)TheRenderManager->height, jitter.x, jitter.y, true };
		ProducedFrame = TheFrameRateManager->FrameNumber;
		// BendDebug 3 (diagnostic): the result as it is now, once per switch to 3, with the light it was made for
		static int lastDebug = 0;
		const int debug = TheSettingManager->GetSettingI(kSection, "BendDebug");
		if (debug == 3 && lastDebug != 3) {
			IDirect3DSurface9* copy = nullptr;
			if (SUCCEEDED(Device->CreateOffscreenPlainSurface(TheRenderManager->width, TheRenderManager->height, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &copy, nullptr))) {
				const HRESULT got = Device->GetRenderTargetData(Surface, copy);
				HRESULT saved = got;
				char path[MAX_PATH] = "";
				GetFullPathNameA(".\\" VS_LOGS_FOLDER "\\BendContact.raw", MAX_PATH, path, nullptr);
				D3DLOCKED_RECT locked = {};
				if (SUCCEEDED(got) && SUCCEEDED(saved = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
					if (FILE* f = fopen(path, "wb")) {
						const UINT32 header[3] = { (UINT32)TheRenderManager->width, (UINT32)TheRenderManager->height, (UINT32)locked.Pitch };
						fwrite(header, sizeof(header), 1, f);
						fwrite(locked.pBits, (size_t)locked.Pitch * TheRenderManager->height, 1, f);
						fclose(f);
					}
					else saved = E_FAIL;
					copy->UnlockRect();
				}
				Logger::Log("UNOFFICIAL Bend contact DUMP to %s", path);
				copy->Release();
				Logger::Log("UNOFFICIAL Bend contact DUMP: %08lX; light %.2f %.2f z %.6f w %+.0f, sun clip %.4f %.4f %.4f %.4f, view %.3f %.3f %.3f, %u samples, rect %ux%u",
					saved, push.lightCoordinate[0], push.lightCoordinate[1], push.lightCoordinate[2], push.lightCoordinate[3], clip.x, clip.y, clip.z, clip.w,
					view.x, view.y, view.z, push.sampleCount, w, h);
			}
		}
		lastDebug = debug;
		if (++Frames == 1)
			Logger::Log("UNOFFICIAL Bend contact shadows in the lit shaders: running (%d dispatches, %ux%u, %u samples per ray, light %.1f %.1f w %+.0f)",
				list.DispatchCount, w, h, push.sampleCount, push.lightCoordinate[0], push.lightCoordinate[1], push.lightCoordinate[3]);
		return true;
	}
}
