#include "ShadowsInteriors.h"
#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

void ShadowsInteriorsEffect::UpdateConstants() {}
void ShadowsInteriorsEffect::UpdateSettings() {}
void ShadowsInteriorsEffect::RegisterConstants() {}


/*
* The generic path runs the technique's three passes through the frame chain: a full-resolution copy
* of the HDR frame for TESR_SourceBuffer, a clear of a full-resolution HDR target before each pass, and
* both blurs of the (one channel) shadow term on HDR targets. The DedicatedInteriorShadows technique
* blurs it on the two G16R16 scratch targets SunShadowsEffect already owns -- shadow buffer -> A -> B --
* and only its last pass writes the HDR frame, reading the untouched scene from TESR_RenderedBuffer
* and the blurred term from sampler s4. TESR_PointShadowBuffer is not modified, as before.
*/
// Whether Render below takes the dedicated path (else the generic one). UNOFFICIAL 2026-10-18 InteriorShadowFold: the fog's
// interior composite is that path's last pass, so it may stand in for it only when this holds.
bool ShadowsInteriorsEffect::DedicatedUsable(IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface) {
	if (!Enabled || !Effect || !ShouldRender()) return false;
	SunShadowsEffect* scratch = TheShaderManager->Effects.SunShadows;
	IDirect3DTexture9* shadowTexture = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassTexture;
	D3DXHANDLE technique = dedicatedFailed ? NULL : TechniqueHandle(techDedicated, "DedicatedInteriorShadows");
	D3DXTECHNIQUE_DESC description = {};
	D3DSURFACE_DESC target = {}, blur = {};
	return !(!technique || !scratch || !shadowTexture || !scratch->scratchTexture[0] || !scratch->scratchTexture[1] ||
		!scratch->scratchSurface[0] || !scratch->scratchSurface[1] || !RenderedSurface || !RenderTarget ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes != 3 ||
		FAILED(RenderTarget->GetDesc(&target)) || FAILED(scratch->scratchSurface[0]->GetDesc(&blur)) ||
		blur.Width != target.Width || blur.Height != target.Height ||
		!TheShaderManager->Chain.Owns(RenderTarget, RenderedSurface));
}

// Push blocks of the two Vulkan passes (Compute\ShadowsInteriorsBlur.frag, ShadowsInteriorsApply.frag). The sampler's heap index
// is filled in by the extension at the offset given to its binding (44 and 176).
struct ShadowsInteriorsBlurPush { D3DXVECTOR4 cameraData; D3DXVECTOR4 reciprocalResolution; D3DXVECTOR2 offsetMask; float maxDistance; UInt32 samplerIndex; D3DXVECTOR2 renderScale; };
struct ShadowsInteriorsApplyPush { D3DXVECTOR4 cameraData; D3DXVECTOR4 shadowData; D3DXVECTOR4 shadowFade; D3DXMATRIX viewTransform; D3DXMATRIX projectionTransform; UInt32 samplerIndex; float scaleX; float scaleY; UInt32 pad; };

// UNOFFICIAL 2026-10-25 (NVR DXVK): the dedicated path as Vulkan passes, in the same order with the same inputs. The blurs write the
// two scratch targets through ExtDrawTo (the frame chain is not touched); the apply writes the frame through ExtFullScreen, the one
// Commit Render makes. A failure before that Commit returns false with nothing committed, and Render then runs the D3D9 path.
bool ShadowsInteriorsEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
	UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!DedicatedUsable(RenderTarget, RenderedSurface)) return false;
	SunShadowsEffect* scratch = TheShaderManager->Effects.SunShadows;
	IDirect3DTexture9* shadowTexture = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassTexture;
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	if (!depth || !TheTextureManager->RenderedTexture) return false;

	// Textures bind in order (set 1, binding i, slot i); the LINEAR/CLAMP sampler (slot 0) takes its heap index from the push block.
	// Every texture and the sampler match the .fx: 2D views, CLAMP, LINEAR (the one-level textures make MIP NONE and LINEAR the same).
	auto make = [](ExtPass& pass, const char* spvName, UINT textureCount, UINT samplerOffset, UINT pushSize) {
		if (pass.Tried) return;
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[4] = {};
		for (UINT i = 0; i < textureCount; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[textureCount].Type = NVR_EXT_BIND_SAMPLER; bindings[textureCount].Slot = 0; bindings[textureCount].PushOffset = samplerOffset;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, textureCount + 1, pushSize, true);
	};
	make(extBlur, "ShadowsInteriorsBlur", 2, 44, sizeof(ShadowsInteriorsBlurPush));
	make(extApply, "ShadowsInteriorsApply", 3, 176, sizeof(ShadowsInteriorsApplyPush));
	ID3D9NvrExtSampler* sampler = DxvkCompute::LinearClamp();
	if (!extBlur.Pipeline || !extApply.Pipeline || !sampler) return false;

	ShadowsInteriorsBlurPush blur = {};
	blur.cameraData = TheRenderManager->CameraData;
	blur.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	blur.maxDistance = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowFade.w; // MAXDISTANCE = TESR_ShadowFade.w
	const bool rect = RenderRect::Active();
	blur.renderScale = rect ? D3DXVECTOR2(RenderRect::ScaleX, RenderRect::ScaleY) : D3DXVECTOR2(1.0f, 1.0f);
	RECT blurVp = {};
	const RECT* pBlurVp = nullptr;
	if (rect) {
		D3DSURFACE_DESC blurDesc = {};
		if (FAILED(scratch->scratchSurface[0]->GetDesc(&blurDesc))) return false;
		blurVp.right = (LONG)RenderRect::Width(blurDesc.Width);
		blurVp.bottom = (LONG)RenderRect::Height(blurDesc.Height);
		pBlurVp = &blurVp;
	}

	// The blurs: the shadow term into scratch 0 (horizontal), then scratch 0 into scratch 1 (vertical). skipBlur: the apply reads the term as it is.
	IDirect3DBaseTexture9* mask = shadowTexture;
	if (!skipBlur) {
		const D3DXVECTOR2 offsetMasks[2] = { D3DXVECTOR2(1.0f, 0.0f), D3DXVECTOR2(0.0f, 1.0f) };	// OffsetMaskH, OffsetMaskV
		for (UINT p = 0; p < 2; p++) {
			blur.offsetMask = offsetMasks[p];
			IDirect3DBaseTexture9* textures[2] = { mask, depth };
			NVR_EXT_RESOURCE_BINDING resources[3] = {};
			for (UINT i = 0; i < 2; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i]; }
			resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_SAMPLER; resources[2].pResource = sampler;
			IDirect3DSurface9* target = scratch->scratchSurface[p];
			if (!ExtDrawTo(extBlur.Pipeline, &target, 1, resources, _countof(resources), &blur, sizeof(blur), pBlurVp, rect ? ExtUv::TexelCentre : ExtUv::ScreenQuad)) return false;
			mask = scratch->scratchTexture[p];
		}
	}

	// The apply: the scene as it is (TESR_RenderedBuffer is the chain's current image, untouched by the blurs) with the blurred term and the depth.
	ShadowsInteriorsApplyPush apply = {};
	apply.cameraData = TheRenderManager->CameraData;
	apply.shadowData = TheShaderManager->Effects.ShadowsExteriors->Constants.Data;
	apply.shadowFade = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowFade;
	apply.viewTransform = TheRenderManager->viewMatrix;		// as stored, as the D3D9 constants carry them
	apply.projectionTransform = TheRenderManager->projMatrix;
	apply.scaleX = rect ? RenderRect::ScaleX : 1.0f;
	apply.scaleY = rect ? RenderRect::ScaleY : 1.0f;
	IDirect3DBaseTexture9* inputs[3] = { TheTextureManager->RenderedTexture, mask, depth };
	NVR_EXT_RESOURCE_BINDING resources[4] = {};
	for (UINT i = 0; i < 3; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = inputs[i]; }
	resources[3].Slot = 0; resources[3].Type = NVR_EXT_BIND_SAMPLER; resources[3].pResource = sampler;
	// last = false: Render's dedicated path always Commits (never the direct write), so this does the same.
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, nullptr, extApply.Pipeline, resources, _countof(resources), &apply, sizeof(apply), false);
}

void ShadowsInteriorsEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
	UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { NoteSkip(); renderTime = 0; return; }

	FrameChain& chain = TheShaderManager->Chain;
	SunShadowsEffect* scratch = TheShaderManager->Effects.SunShadows;
	IDirect3DTexture9* shadowTexture = TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassTexture;
	if (!DedicatedUsable(RenderTarget, RenderedSurface)) {
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the dedicated path as Vulkan passes when [Main.Main.Performance] NvrDxvkPasses is on (or PassesOnRect at RenderScale < 1).
	if (!ExtFailed && DxvkCompute::PassesOnRect()) {
		auto extTimer = TimeLogger();
		if (ExtRender(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer)) {
			Device->SetRenderTarget(0, RenderTarget);
			renderTime = extTimer.LogTime("ShadowsInteriors::Extension");
			return;
		}
	}

	D3DXHANDLE technique = TechniqueHandle(techDedicated, "DedicatedInteriorShadows");

	auto timer = TimeLogger();
	HRESULT result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) SetCT();
	UINT passes = 0;
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		static GpuTimer passTimers[3] = { GpuTimer("  Interior shadow blur H"), GpuTimer("  Interior shadow blur V"),
			GpuTimer("  Interior shadow apply") };
		IDirect3DBaseTexture9* source = shadowTexture;
		for (UINT p = skipBlur ? 2 : 0; p < 3 && SUCCEEDED(result); ++p) {
			GpuProfileScope gpuPass(passTimers[p], Device);
			const bool combine = p == 2;
			Device->SetTexture(4, nullptr); // s4 must not still hold the texture this pass renders into
			result = Device->SetRenderTarget(0, combine ? chain.Output() : scratch->scratchSurface[p]);
			if (FAILED(result)) break;
			// The blur passes skip pixels beyond the draw distance and rely on a black destination, as
			// the original passes did; the last pass writes every pixel.
			if (!combine) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0L);
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			RebindSlotTextures(); // TESR_RenderedBuffer is the current scene image
			Device->SetTexture(4, source); // NVR_InteriorShadowIn
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the drawn part only in rect frames
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
			source = combine ? nullptr : scratch->scratchTexture[p];
		}
		Effect->End();
	}
	Device->SetTexture(4, nullptr);
	TheRenderManager->renderState->NoteDeviceTexture(4, nullptr);	// UNOFFICIAL 2026-10-27: keep the game's texture cache truthful (ManyLampsGlow's s4)
	Device->SetRenderTarget(0, RenderTarget);
	if (FAILED(result)) {
		// Nothing was committed to the chain, so the generic path can still do this frame.
		dedicatedFailed = true;
		Logger::Log("Dedicated interior shadows failed (%08lx); using the generic path until restart.", result);
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	chain.Commit();

	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL dedicated interior shadows active: blur on two G16R16 targets, one HDR pass, no frame copy.");
		reported = true;
	}
	renderTime = timer.LogTime("ShadowsInteriors::Dedicated");
}
