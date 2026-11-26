#pragma once

// UNOFFICIAL 2026-11-27 lamp screen-space shadows (owner: "Yes, add for lamps"). The Bottled Shaders port's raymarch (Bend Studio's; core/
// ScreenSpaceShadows.h, Compute/ScreenSpaceShadowsLamp.comp) toward up to four lamps' positions, for the deferred lamp passes:
//   1. at each deferred span's end (Render.cpp DeferredSpan, right after the sun's march, on the same depth copy), the lamps that matter most
//      on screen are chosen from the lamp table (ManyLamps: brightness x how much of the view their reach covers; a lamp chosen last frame
//      keeps its place unless another scores a quarter more: no flicker between two equal lamps);
//   2. each one's march runs only over its reach's screen rectangle (LampScissor's exact sphere bound) into one channel of
//      TESR_LampScreenShadows (cleared to 1);
//   3. DeferredLamps' shadow passes (DeferredLamps.hlsl DLShadow) multiply that lamp's shadow by its channel, faded out from FadeStart to
//      FadeStart + FadeLength (camera distance). Lamps 0-15 of a pixel's list only (the lighting pass has no registers for it).
// Not while RT lamp shadows run with their own contact march ([ShadowsInteriors.RT] ContactShadows): one contact shadow, not two. Not in the
// first-person run (the view model is not in the depth). Native D3D9: no compute, off. A point light in Bend: the light's clip position
// with w = +-1 (BuildDispatchList), rays toward / away from it.

#include "ScreenSpaceShadows.h"
#include "ManyLamps.h"
#include "LampScissor.h"
#include "RtLampShadows.h"

namespace LampScreenShadows {

	struct PushL {
		float	lightCoordinate[4];
		INT32	waveOffset[2];
		INT32	size[2];
		float	farDepthValue, nearDepthValue, surfaceThickness, bilinearThreshold, shadowContrast;
		UINT32	sampleCount, readCount, channel;
	};
	static_assert(sizeof(PushL) == 64, "ScreenSpaceShadowsLamp.comp push block");

	inline const NVR_EXT_BINDING_DESC Bindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// the depth (DeferredDepth::Texture)
		{ 1, 1, NVR_EXT_BIND_STORAGE_IMAGE, 1, NVR_EXT_VIEW_2D, TRUE,  0 },	// TESR_LampScreenShadows (read and written)
	};

	inline unsigned long long StatFrames = 0, StatLamps = 0;

	// Render.cpp DeferredSpan, after ScreenSpaceShadows::Produce. False: no lamp mask this span.
	inline bool Produce(IDirect3DDevice9* Device) {
		using namespace ScreenSpaceShadows::Lamps;
		for (float& t : TableIndex) t = -1.0f;
		if (!Texture || Failed || !DeferredDepth::Ready() || !DxvkCompute::Available()) return false;
		if (TheSettingManager->GetSettingI(kSection, "Enabled") == 0) return false;
		const int wanted = std::clamp(TheSettingManager->GetSettingI(kSection, "Lamps"), 0, 4);
		if (wanted == 0) return false;
		// RT lamp shadows with their own contact march: theirs, not two
		if (RtLampShadows::EnabledNow() && RtLampShadows::SyncSettings().contactShadows > 0.0f) return false;
		if (ManyLamps::TableFrame != ManyLamps::FrameId || ManyLamps::ActiveIndex.empty()) return false;
		NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
		if (!camera || camera->Frustum.Ortho) return false;
		if (!Pipeline) {
			if (Tried) return false;
			Tried = true;
			Pipeline = DxvkCompute::CreatePipeline("ScreenSpaceShadowsLamp", Bindings, _countof(Bindings), sizeof(PushL), false);
			if (!Pipeline) {
				Logger::Log("UNOFFICIAL Screen-Space Shadows for lamps: Compute\\ScreenSpaceShadowsLamp.spv could not be set up; off for this session");
				Failed = true;
				return false;
			}
		}

		// The camera as LampScissor reads it: forward, up, right = the rotation's columns 0, 1, 2 (RenderManager's view axes)
		const NiMatrix33& rot = camera->m_worldTransform.rot;
		const NiPoint3& eye = camera->m_worldTransform.pos;
		const NiFrustum& frustum = camera->Frustum;
		const float frustumWidth = frustum.Right - frustum.Left, frustumHeight = frustum.Top - frustum.Bottom;
		if (frustumWidth <= 0.0f || frustumHeight <= 0.0f || frustum.Near <= 0.0f) return false;
		const float forward[3] = { rot.data[0][0], rot.data[1][0], rot.data[2][0] };
		const float up[3] = { rot.data[0][1], rot.data[1][1], rot.data[2][1] };
		const float right[3] = { rot.data[0][2], rot.data[1][2], rot.data[2][2] };
		const UINT w = RenderRect::Width(TheRenderManager->width), h = RenderRect::Height(TheRenderManager->height);
		auto screenX = [&](float t) { return ((2.0f * t - (frustum.Right + frustum.Left)) / frustumWidth + 1.0f) * 0.5f * (float)w; };
		auto screenY = [&](float t) { return (1.0f - (2.0f * t - (frustum.Top + frustum.Bottom)) / frustumHeight) * 0.5f * (float)h; };

		// Score every active lamp: brightness x the share of the view its reach covers (1 with the camera inside it), on screen only
		struct Candidate { int index; const void* id; float score; float view[3]; int rect[4]; };
		Candidate best[4] = {};
		int count = 0;
		for (int index : ManyLamps::ActiveIndex) {
			const LampTableCore::SceneLamp& lamp = ManyLamps::SceneLamps[index];
			const float reach = lamp.reach;
			if (!(reach > 0.0f) || reach > 1.0e6f) continue;
			const float p[3] = { lamp.pos[0] - eye.x, lamp.pos[1] - eye.y, lamp.pos[2] - eye.z };
			const float z = p[0] * forward[0] + p[1] * forward[1] + p[2] * forward[2];
			const float x = p[0] * right[0] + p[1] * right[1] + p[2] * right[2];
			const float y = p[0] * up[0] + p[1] * up[1] + p[2] * up[2];
			float xLo, xHi, yLo, yHi;
			if (!LampScissor::AxisRange(x, z, reach, frustum.Near * 0.5f, xLo, xHi) || !LampScissor::AxisRange(y, z, reach, frustum.Near * 0.5f, yLo, yHi)) continue;
			const int rect[4] = { (std::max)(0, (int)floorf(screenX(xLo))), (std::max)(0, (int)floorf(screenY(yHi))),
				(std::min)((int)w, (int)ceilf(screenX(xHi))), (std::min)((int)h, (int)ceilf(screenY(yLo))) };
			if (rect[2] <= rect[0] || rect[3] <= rect[1]) continue;	// off screen
			const float brightness = (std::max)(0.0f, (0.2126f * lamp.diff[0] + 0.7152f * lamp.diff[1] + 0.0722f * lamp.diff[2]) * lamp.dimmer * lamp.lod);
			const float d2 = x * x + y * y + z * z;
			const float cover = d2 <= reach * reach ? 1.0f : (std::min)(1.0f, reach * reach / d2);
			float score = brightness * cover;
			for (const void* kept : Chosen) if (kept && kept == lamp.id) score *= 1.25f;	// hysteresis: the last frame's lamps keep their place
			if (!(score > 0.0f)) continue;
			Candidate c = { index, lamp.id, score, { x, y, z }, { rect[0], rect[1], rect[2], rect[3] } };
			int slot = count < wanted ? count++ : -1;
			if (slot < 0) {	// replace the weakest if this one is stronger
				int weakest = 0;
				for (int k = 1; k < count; k++) if (best[k].score < best[weakest].score) weakest = k;
				if (best[weakest].score >= score) continue;
				slot = weakest;
			}
			best[slot] = c;
		}
		for (const void*& kept : Chosen) kept = nullptr;
		if (!count) return false;

		// The sun section's march knobs (one set for both)
		const char* const sun = ScreenSpaceShadows::kSection;
		const bool reversed = TheRenderManager->IsReversedDepth();
		PushL push = {};
		push.size[0] = (int)w; push.size[1] = (int)h;
		push.farDepthValue = reversed ? 0.0f : 1.0f;
		push.nearDepthValue = reversed ? 1.0f : 0.0f;
		push.surfaceThickness = std::clamp(TheSettingManager->GetSettingF(sun, "SurfaceThickness"), 0.005f, 0.05f);
		push.bilinearThreshold = std::clamp(TheSettingManager->GetSettingF(sun, "BilinearThreshold"), 0.02f, 1.0f);
		push.shadowContrast = std::clamp(TheSettingManager->GetSettingF(sun, "ShadowContrast"), 0.0f, 4.0f);
		push.sampleCount = ScreenSpaceShadows::Samples(w, h);
		push.readCount = push.sampleCount / 64u + 2u;

		// DeferredLamps reads the mask on s13 and leaves it bound: off that stage before it is written; then cleared to 1 (lit)
		{
			IDirect3DBaseTexture9* bound = nullptr;
			if (SUCCEEDED(Device->GetTexture(13, &bound)) && bound) {
				const bool same = bound == Texture;
				bound->Release();
				if (same) { Device->SetTexture(13, nullptr); TheRenderManager->renderState->NoteDeviceTexture(13, nullptr); }
			}
		}
		Device->ColorFill(Surface, nullptr, D3DCOLOR_ARGB(255, 255, 255, 255));

		NVR_EXT_RESOURCE_BINDING bindings[] = {
			{ 0, NVR_EXT_BIND_SAMPLED_IMAGE, DeferredDepth::Texture, 0, 0, 0, FALSE },
			{ 1, NVR_EXT_BIND_STORAGE_IMAGE, Texture,                0, 0, 0, FALSE },
		};
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		int made = 0;
		{
			static GpuTimer timer("  Screen-space shadows, lamps (compute)");
			GpuProfileScope gpu(timer, Device);
			DxvkCompute::PreLoad(bindings, _countof(bindings));
			for (int k = 0; k < count; k++) {
				const Candidate& c = best[k];
				// the lamp's clip position (view space: x right, y up, z forward, as RenderManager's view axes; the frame's projection)
				const D3DXVECTOR4 view(c.view[0], c.view[1], c.view[2], 1.0f);
				D3DXVECTOR4 clip;
				D3DXVec4Transform(&clip, &view, &proj);
				if (!_finite(clip.x) || !_finite(clip.y) || !_finite(clip.z) || !_finite(clip.w) || fabsf(clip.w) < 1.0e-4f) continue;
				const float lightProjection[4] = { clip.x, clip.y, clip.z, clip.w };
				const int size[2] = { (int)w, (int)h };
				const int minBounds[2] = { c.rect[0], c.rect[1] }, maxBounds[2] = { c.rect[2], c.rect[3] };
				const Bend::DispatchList list = Bend::BuildDispatchList(lightProjection, size, minBounds, maxBounds);
				for (int i = 0; i < 4; i++) push.lightCoordinate[i] = list.LightCoordinate_Shader[i];
				push.channel = (UINT32)made;
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
					if (FAILED(DxvkCompute::Extension()->Dispatch(&dispatch))) {
						Logger::Log("UNOFFICIAL Screen-Space Shadows for lamps: a dispatch failed; off for this session");
						Failed = true;
						for (float& t : TableIndex) t = -1.0f;
						return false;
					}
				}
				TableIndex[made] = (float)c.index;
				Chosen[made] = c.id;
				made++;
			}
		}
		if (!made) return false;
		const float fadeStart = (std::max)(TheSettingManager->GetSettingF(kSection, "FadeStart"), 0.0f);
		const float fadeLength = (std::max)(TheSettingManager->GetSettingF(kSection, "FadeLength"), 1.0f);
		Fade[0] = fadeStart; Fade[1] = 1.0f / fadeLength;
		MadeFrame = ScreenSpaceShadows::Now();
		MadeSpan = ScreenSpaceShadows::SpanCounter;
		StatFrames++; StatLamps += (unsigned long long)made;
		if (++Frames == 1)
			Logger::Log("UNOFFICIAL Screen-Space Shadows for lamps: running (%d of %d wanted this frame, %u samples per ray, %ux%u)", made, wanted, push.sampleCount, w, h);
		return true;
	}
}
