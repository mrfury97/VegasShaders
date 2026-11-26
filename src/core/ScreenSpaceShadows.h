#pragma once

// UNOFFICIAL 2026-11-26 Screen-Space Shadows, a one-to-one port of Bottled Shaders' (github.com/InTheBottle/Bottled-Shaders, a Skyrim
// Community Shaders fork, GPL-3.0; src/Features/ScreenSpaceShadows.cpp + features/Screen-Space Shadows/Shaders) Bend Studio screen-space
// shadows (Bend's code Copyright 2023 Sony Interactive Entertainment, Apache-2.0), fully deferred (owner: "make it fully deffered one
// to one port of bottle shaders versions"). docs/EFFECTS.md "ShadowsExteriors" section 12.
//
// Theirs: each frame after the depth pre-pass, the mask is cleared to 1 and (Enable, full sky) Bend's march toward the sun writes it
// (RaymarchCS, BuildDispatchList's dispatches); the deferred lighting multiplies the sun's shadow by it where N.L >= 0, outdoors.
// Here, translated (one line each):
//   - the depth pre-pass -> the main opaque list's depth at the deferred span's end (core/DeferredDepth.h's copy, which DeferredRenderer 2
//     always makes: SettingForce forces DeferredDepth 2);
//   - D3D11 compute -> the NVR DXVK extension's Vulkan compute (Compute/ScreenSpaceShadows.comp); native D3D9 has no compute: off there;
//   - their deferred Lighting.hlsl -> Shaders/DeferredSun.hlsl (DS_SSS c19, TESR_ScreenSpaceShadows s13), the deferred sun pass only;
//   - full sky (Sky::Mode::kFull) -> outdoors (GameState.isExterior); the sun light's direction -> ShaderConst.SunDir (the shadow maps');
//   - their JSON settings -> [Shaders.ShadowsExteriors.SSShadows] with their defaults and ranges (Enabled, SampleCount,
//     SurfaceThickness, BilinearThreshold, ShadowContrast);
//   - the R8_UNORM mask -> G16R16F (r used): the storage format the fork's compute passes already write;
//   - DynamicResolution -> RenderRect (the drawn part of the frame).
// Not ported: nothing of theirs is left out except the debug outputs (off in theirs). The first-person pass (DeferredFirstPerson)
// does not take it: the view model is not in the depth the march read (theirs is, through the pre-pass).

#include <algorithm>
#include <cmath>
#include "DxvkCompute.h"
#include "RenderRect.h"
#include "GpuProfiler.h"
#include "BendDispatch.h"
#include "DeferredDepth.h"

namespace ScreenSpaceShadows {
	inline constexpr const char* kSection = "Shaders.ShadowsExteriors.SSShadows";

	// Startup (TextureManager::Initialize, DeferredDepth::ReadStartup): the switch and the deferred renderer that reads the result
	inline bool WantedAtStart() {
		return TheSettingManager->GetSettingI(kSection, "Enabled") != 0 && TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRenderer") > 0;
	}

	inline IDirect3DTexture9* Texture = nullptr;	// TESR_ScreenSpaceShadows (G16R16F, r: 1 lit .. 0 shadowed)
	inline IDirect3DSurface9* Surface = nullptr;
	inline ID3D9NvrExtPipeline* Pipeline = nullptr;
	inline bool Tried = false, Failed = false;
	inline UINT MadeFrame = 0xFFFFFFFF;		// the frame (and span: MadeSpan) whose depth the mask was made from
	inline UINT MadeSpan = 0xFFFFFFFF;
	inline UINT SpanCounter = 0;			// spans seen (DeferredDepth::CopiesThisFrame is per frame)
	inline UINT UsedFrame = 0xFFFFFFFF;		// the frame the deferred sun pass took it (the old contact pass then stands aside)
	inline UINT Frames = 0;

	inline UINT Now() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }
	// This span's mask exists (DeferredSun::Apply runs right after Produce in the same span)
	inline bool MadeThisSpan() { return Texture && MadeFrame == Now() && MadeSpan == SpanCounter; }
	inline bool UsedThisFrame() { return UsedFrame == Now(); }

	struct Push {
		float	lightCoordinate[4];
		INT32	waveOffset[2];
		INT32	size[2];
		float	farDepthValue, nearDepthValue, surfaceThickness, bilinearThreshold, shadowContrast;
		UINT32	sampleCount, readCount, pad;
	};
	static_assert(sizeof(Push) == 64, "ScreenSpaceShadows.comp push block");

	inline const NVR_EXT_BINDING_DESC Bindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// the depth (DeferredDepth::Texture)
		{ 1, 1, NVR_EXT_BIND_STORAGE_IMAGE, 1, NVR_EXT_VIEW_2D, TRUE,  0 },	// TESR_ScreenSpaceShadows
	};

	// TextureManager::Initialize, after DeferredDepth's textures: the mask, screen sized, only when wanted and the depth copy exists
	inline void MakeTextures(TextureManager* mgr, UINT w, UINT h) {
		if (!WantedAtStart() || !DeferredDepth::Texture || Texture) return;
		mgr->InitTexture("TESR_ScreenSpaceShadows", &Texture, &Surface, w, h, D3DFMT_G16R16F);
		Logger::Log("UNOFFICIAL Screen-Space Shadows (Bottled Shaders port): %s (%ux%u)", Texture ? "mask made" : "the mask was REFUSED: off", w, h);
	}

	// Their GetScaledSampleCount: SampleCount x 60 at 1920x1080, by the square root of the drawn area, rounded, up to a multiple of 8
	// (at least 8). The shader's groupshared arrays hold 512 (SampleCount 4 at 4K).
	inline UINT32 Samples(UINT w, UINT h) {
		const int multiplier = std::clamp(TheSettingManager->GetSettingI(kSection, "SampleCount"), 1, 4);
		const float areaScale = sqrtf(((float)w * (float)h) / (1920.0f * 1080.0f));
		UINT32 samples = (UINT32)lroundf((float)multiplier * 60.0f * areaScale);
		samples = ((samples + 7u) / 8u) * 8u;
		samples = (std::max<UINT32>)(samples, 8u);
		return (std::min<UINT32>)(samples, 512u);
	}

	// Render.cpp's DeferredSpan, right after DeferredDepth::AtSpanEnd and before DeferredSun::Apply: their Prepass (clear to 1, then
	// the march when on). False: no mask for this span (the sun pass then takes none).
	inline bool Produce(IDirect3DDevice9* Device) {
		SpanCounter++;
		if (!Texture || Failed || !DeferredDepth::Ready() || !DxvkCompute::Available()) return false;
		if (!TheShaderManager->GameState.isExterior || TheSettingManager->GetSettingI(kSection, "Enabled") == 0) return false;
		if (!Pipeline) {
			if (Tried) return false;
			Tried = true;
			Pipeline = DxvkCompute::CreatePipeline("ScreenSpaceShadows", Bindings, _countof(Bindings), sizeof(Push), false);
			if (!Pipeline) {
				Logger::Log("UNOFFICIAL Screen-Space Shadows: Compute\\ScreenSpaceShadows.spv could not be set up; off for this session");
				Failed = true;
				return false;
			}
		}

		// Their light projection: the direction toward the sun (w 0) through the camera's view and projection, not divided
		const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.SunDir;
		const D3DXVECTOR4 towardSun(sun.x, sun.y, sun.z, 0.0f);
		D3DXVECTOR4 view, clip;
		D3DXVec4Transform(&view, &towardSun, &TheRenderManager->viewMatrix);
		D3DXVec4Transform(&clip, &view, &TheRenderManager->projMatrix);
		if (D3DXVec4LengthSq(&view) < 0.25f || !_finite(clip.x) || !_finite(clip.y) || !_finite(clip.z) || !_finite(clip.w)) return false;

		const UINT w = RenderRect::Width(TheRenderManager->width), h = RenderRect::Height(TheRenderManager->height);
		const int size[2] = { (int)w, (int)h }, minBounds[2] = { 0, 0 };
		const float lightProjection[4] = { clip.x, clip.y, clip.z, clip.w };
		const Bend::DispatchList list = Bend::BuildDispatchList(lightProjection, size, minBounds, size);

		// Theirs: FarDepthValue 1, NearDepthValue 0 (the game's depth); swapped for a reversed depth buffer
		const bool reversed = TheRenderManager->IsReversedDepth();
		Push push = {};
		for (int i = 0; i < 4; i++) push.lightCoordinate[i] = list.LightCoordinate_Shader[i];
		push.size[0] = size[0]; push.size[1] = size[1];
		push.farDepthValue = reversed ? 0.0f : 1.0f;
		push.nearDepthValue = reversed ? 1.0f : 0.0f;
		push.surfaceThickness = std::clamp(TheSettingManager->GetSettingF(kSection, "SurfaceThickness"), 0.005f, 0.05f);
		push.bilinearThreshold = std::clamp(TheSettingManager->GetSettingF(kSection, "BilinearThreshold"), 0.02f, 1.0f);
		push.shadowContrast = std::clamp(TheSettingManager->GetSettingF(kSection, "ShadowContrast"), 0.0f, 4.0f);
		push.sampleCount = Samples(w, h);
		push.readCount = push.sampleCount / 64u + 2u;

		// DeferredSun reads the mask on s13 and leaves it bound: off that stage before it is written
		{
			IDirect3DBaseTexture9* bound = nullptr;
			if (SUCCEEDED(Device->GetTexture(13, &bound)) && bound) {
				const bool same = bound == Texture;
				bound->Release();
				if (same) { Device->SetTexture(13, nullptr); TheRenderManager->renderState->NoteDeviceTexture(13, nullptr); }
			}
		}
		// Their Prepass: the mask cleared to 1 (lit) first
		Device->ColorFill(Surface, nullptr, D3DCOLOR_ARGB(255, 255, 255, 255));

		NVR_EXT_RESOURCE_BINDING bindings[] = {
			{ 0, NVR_EXT_BIND_SAMPLED_IMAGE, DeferredDepth::Texture, 0, 0, 0, FALSE },
			{ 1, NVR_EXT_BIND_STORAGE_IMAGE, Texture,                0, 0, 0, FALSE },
		};
		{
			static GpuTimer timer("  Screen-space shadows (compute)");
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
					Logger::Log("UNOFFICIAL Screen-Space Shadows: a dispatch failed (%08lX); off for this session", result);
					Failed = true;
					return false;
				}
			}
		}
		MadeFrame = Now();
		MadeSpan = SpanCounter;
		if (++Frames == 1)
			Logger::Log("UNOFFICIAL Screen-Space Shadows: running (%d dispatches, %ux%u, %u samples per ray, light %.1f %.1f z %.4f w %+.0f, depth %s)",
				list.DispatchCount, w, h, push.sampleCount, push.lightCoordinate[0], push.lightCoordinate[1], push.lightCoordinate[2], push.lightCoordinate[3],
				reversed ? "reversed" : "standard");
		return true;
	}

	// UNOFFICIAL 2026-11-27 LAMPS (owner: "Yes, add for lamps"; the producer is core/LampScreenShadows.h, Render.cpp's DeferredSpan; the
	// reader DeferredLamps.hlsl DLShadow in the shadow passes): the same raymarch toward up to four lamps' positions, one lamp a channel of
	// TESR_LampScreenShadows (A16B16G16R16F, cleared to 1). [Shaders.ShadowsInteriors.SSShadows] Enabled, Lamps (0-4), FadeStart, FadeLength;
	// the march's own knobs (SampleCount, SurfaceThickness, BilinearThreshold, ShadowContrast) are the sun's section's, one set for both.
	namespace Lamps {
		inline constexpr const char* kSection = "Shaders.ShadowsInteriors.SSShadows";
		inline bool WantedAtStart() {
			return TheSettingManager->GetSettingI(kSection, "Enabled") != 0 && TheSettingManager->GetSettingI("Main.Main.Quality", "DeferredRenderer") > 0;
		}
		inline IDirect3DTexture9* Texture = nullptr;	// TESR_LampScreenShadows
		inline IDirect3DSurface9* Surface = nullptr;
		inline ID3D9NvrExtPipeline* Pipeline = nullptr;
		inline bool Tried = false, Failed = false;
		inline UINT MadeFrame = 0xFFFFFFFF, MadeSpan = 0xFFFFFFFF;
		inline float TableIndex[4] = { -1.0f, -1.0f, -1.0f, -1.0f };	// DL_SSLamp: the table index each channel holds (-1 none)
		inline const void* Chosen[4] = {};								// the lamps (ShadowSceneLight) chosen last time (hysteresis)
		inline float Fade[2] = { 1000.0f, 1.0f / 1000.0f };				// DL_SSL y, z
		inline UINT Frames = 0;
		inline bool MadeThisSpan() { return Texture && MadeFrame == Now() && MadeSpan == SpanCounter; }
		inline void MakeTextures(TextureManager* mgr, UINT w, UINT h) {
			if (!WantedAtStart() || !DeferredDepth::Texture || Texture) return;
			mgr->InitTexture("TESR_LampScreenShadows", &Texture, &Surface, w, h, D3DFMT_A16B16G16R16F);
			Logger::Log("UNOFFICIAL Screen-Space Shadows for lamps: %s (%ux%u, up to 4 lamps)", Texture ? "mask made" : "the mask was REFUSED: off", w, h);
		}
	}
}
