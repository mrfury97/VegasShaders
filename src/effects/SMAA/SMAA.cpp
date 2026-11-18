#include <algorithm>

#include "SMAA.h"
#include "../Tonemapping/Tonemapping.h"				// UNOFFICIAL 2026-10-25 (NVR DXVK): TESR_ToneMappingClip for the stable edge pass
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void SMAAEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SMAAResolution", &Constants.Resolution);
	TheShaderManager->RegisterConstant("TESR_SMAAData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_SMAADepthData", &Constants.DepthData);
	TheShaderManager->RegisterConstant("TESR_SMAAPred", &Constants.Pred);
};

// UNOFFICIAL 2026-11-12 (memleak) idle targets: SMAA stays enabled as the fallback while TAA runs and draws nothing then (EffectChain's
// PostStep_SMAA, the same test): its two screen-size buffers are not needed until TAA goes off or [Shaders.SMAA.Main] WithTAA is set.
bool SMAAEffect::TargetsWanted() {
	if (!Enabled) return false;
	const TAAEffect* taa = TheShaderManager->Effects.TAA;
	if (!taa || !taa->Enabled) return true;
	SettingManager::Configuration::ConfigNode node;
	return TheSettingManager->Config.FillNode(&node, "Shaders.SMAA.Main", "WithTAA") && node.BoolValue;
}

void SMAAEffect::RegisterTextures() {
	int width = TheRenderManager->width;
	int height = TheRenderManager->height;

	TheTextureManager->InitTexture("TESR_SMAA_Edges", &Textures.SMAA_Edges_Texture, &Textures.SMAA_Edges_Surface, width, height, D3DFMT_A8R8G8B8);
	TheTextureManager->InitTexture("TESR_SMAA_Blend", &Textures.SMAA_Blend_Texture, &Textures.SMAA_Blend_Surface, width, height, D3DFMT_A8R8G8B8);
};

void SMAAEffect::UpdateSettings() {
	Settings.Main.EdgeDetection = static_cast<Input>(std::clamp(TheSettingManager->GetSettingI("Shaders.SMAA.Main", "EdgeDetection"), 0, 3));
	// A key absent from the toml reads as 0, which the clamps below would floor to the bottom of
	// each range rather than to the documented default - a much more sensitive threshold and a
	// far shorter search than intended, silently, for anyone who updates the plugin without the
	// defaults file. Fall back to the SMAA_PRESET_ULTRA values instead, matching what the shader
	// does when the constants themselves are missing.
	float threshold = TheSettingManager->GetSettingF("Shaders.SMAA.Main", "Threshold");
	Settings.Main.Threshold = threshold > 0.0f ? std::clamp(threshold, 0.005f, 0.5f) : 0.05f;

	float depthThreshold = TheSettingManager->GetSettingF("Shaders.SMAA.Main", "DepthThreshold");
	Settings.Main.DepthThreshold = depthThreshold > 0.0f ? std::clamp(depthThreshold, 0.0001f, 0.5f) : 0.005f;

	int searchSteps = TheSettingManager->GetSettingI("Shaders.SMAA.Main", "MaxSearchSteps");
	Settings.Main.MaxSearchSteps = searchSteps > 0 ? (float)std::clamp(searchSteps, 4, 112) : 32.0f;
	Settings.Main.SubpixelShift = std::clamp(TheSettingManager->GetSettingF("Shaders.SMAA.Main", "SubpixelShift"), -2.0f, 2.0f);
	Settings.Main.DepthOffset = std::clamp(TheSettingManager->GetSettingF("Shaders.SMAA.Main", "DepthOffset"), -2.0f, 2.0f);
	float localContrast = TheSettingManager->GetSettingF("Shaders.SMAA.Main", "LocalContrast");
	Settings.Main.LocalContrast = localContrast > 0.0f ? std::clamp(localContrast, 1.0f, 20.0f) : 2.0f;
	// UNOFFICIAL aastab. A missing key reads as 0 / false: predication and de-dither stay off, the scale and strength fall back to the SMAA reference values.
	Settings.Main.Predication = TheSettingManager->GetSettingI("Shaders.SMAA.Main", "Predication") != 0;
	const float predScale = TheSettingManager->GetSettingF("Shaders.SMAA.Main", "PredicationScale");
	Settings.Main.PredicationScale = predScale > 0.0f ? std::clamp(predScale, 1.0f, 8.0f) : 2.0f;
	const float predStrength = TheSettingManager->GetSettingF("Shaders.SMAA.Main", "PredicationStrength");
	Settings.Main.PredicationStrength = std::clamp(predStrength > 0.0f ? predStrength : 0.4f, 0.0f, 1.0f);
	Settings.Main.DeDither = TheSettingManager->GetSettingI("Shaders.SMAA.Main", "DeDither") != 0;
};

void SMAAEffect::UpdateConstants() {
	Constants.Resolution.x = 1.0f / (float) TheRenderManager->width;
	Constants.Resolution.y = 1.0f / (float) TheRenderManager->height;
	// SMAA_RT_METRICS is float4(1/width, 1/height, width, height) - see the definition in the
	// include. z and w were filled the other way round, and because SMAABlendingWeightCalculation
	// derives pixcoord from the same swapped pair, the error is self consistent per axis rather
	// than obviously broken: horizontal edge distances came out scaled by height/width and
	// vertical ones by width/height. At 16:9 that is 44% short horizontally, so every edge looked
	// shorter than it was and got less blending, and 78% long vertically, which clamps against
	// SMAA_AREATEX_MAX_DISTANCE. It reads as SMAA being too weak, in every edge detection mode,
	// because it is downstream of all three.
	Constants.Resolution.z = (float)TheRenderManager->width;
	Constants.Resolution.w = (float)TheRenderManager->height;

	Constants.Data.x = Settings.Main.Threshold;
	Constants.Data.y = Settings.Main.DepthThreshold;
	Constants.Data.z = Settings.Main.MaxSearchSteps;
	Constants.Data.w = Settings.Main.SubpixelShift;
	Constants.DepthData.x = Settings.Main.DepthOffset;
	Constants.DepthData.y = Settings.Main.LocalContrast;
	Constants.DepthData.z = 0.0f;
	Constants.DepthData.w = 0.0f;
	Constants.Pred = D3DXVECTOR4(Settings.Main.Predication ? 1.0f : 0.0f, Settings.Main.PredicationScale, Settings.Main.PredicationStrength, Settings.Main.DeDither ? 1.0f : 0.0f);
};

// UNOFFICIAL: SMAA's own edge mask (D24S8, screen size, no MSAA), made on first use by either path; a failure is logged once.
static void MakeStencil(IDirect3DDevice9* Device, IDirect3DSurface9*& Surface, bool& Failed) {
	if (Surface || Failed) return;
	if (FAILED(Device->CreateDepthStencilSurface(TheRenderManager->width, TheRenderManager->height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &Surface, NULL))) {
		Surface = nullptr;
		Failed = true;
		Logger::Log("UNOFFICIAL SMAA: no depth-stencil surface of its own; uses the bound one as before");
	}
}

/*
 * Render SMAA by performing the three main passes (edge detection, blending weight calculation, and final blending).
 */
void SMAAEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled) {
		renderTime = 0.0f;
		return; // skip rendering if the effect is disabled
	}

	auto timer = TimeLogger();

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the ported passes when the extension is there and on (as EffectRecord::Render).
	if (!ExtFailed && DxvkCompute::PassesOn() && ExtRender(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer)) {
		Device->SetRenderTarget(0, RenderTarget);
		renderTime = timer.LogTime("EffectRecord::ExtRender SMAA");
		return;
	}

	// UNOFFICIAL: the edge pass marks edge pixels in the stencil and the costly weight pass (32-step searches) runs only
	// there. Until 2026-10-11 the stencil was whatever depth buffer the game had left bound (perhaps multisampled, another
	// size or none), so the mask may have done nothing. SMAA now binds its own screen-size D24S8 for its three passes and
	// puts the old one back after. Same picture (pixels without an edge get weight 0 either way), less work.
	IDirect3DSurface9* previousDepth = nullptr;
	MakeStencil(Device, Textures.StencilSurface, Textures.StencilFailed);
	// UNOFFICIAL ReShadeDepthCompat: keep the game's depth-stencil bound and clear its stencil, as before P78.
	const bool ownStencil = Textures.StencilSurface != nullptr && !ReShadeIntegration::KeepDepthBound();
	if (ownStencil) {
		if (FAILED(Device->GetDepthStencilSurface(&previousDepth))) previousDepth = nullptr;	// none bound
		Device->SetDepthStencilSurface(Textures.StencilSurface);
	}

	// Clear the stencil buffer.
	Device->Clear(0, nullptr, D3DCLEAR_STENCIL, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

	SetCT();

	// Under the frame chain the final pass writes the chain's spare texture, which then becomes
	// the current image, instead of writing the render target and copying it back.
	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);

	EdgesDetectionPass(Settings.Main.EdgeDetection);
	BlendingWeightsCalculationPass();
	NeighborhoodBlendingPass(chained ? chain.Output() : RenderTarget);
	if (ownStencil) Device->SetDepthStencilSurface(previousDepth);	// null if none was bound
	if (previousDepth) previousDepth->Release();

	if (chained) {
		chain.Commit();
		Device->SetRenderTarget(0, RenderTarget);
	}
	else if (RenderedSurface) Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_LINEAR);

	renderTime = timer.LogTime("EffectRecord::Render SMAA");
}

void SMAAEffect::EdgesDetectionPass(Input input) {
    IDirect3DDevice9* Device = TheRenderManager->device;

    // Set the render target and clear both the color and the stencil buffers.
    Device->SetRenderTarget(0, Textures.SMAA_Edges_Surface);
    Device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

    // Select the technique accordingly.
    switch (input) {
    case INPUT_LUMA:
        Effect->SetTechnique(TechniqueHandle(techLuma, "LumaEdgeDetection"));
        break;
    case INPUT_COLOR:
        Effect->SetTechnique(TechniqueHandle(techColor, "ColorEdgeDetection"));
        break;
    case INPUT_DEPTH:
        Effect->SetTechnique(TechniqueHandle(techDepth, "DepthEdgeDetection"));
        break;
    case INPUT_LUMADEPTH:
        // UNOFFICIAL aastab: the de-dithered / predicated variant only when one of its options is on; otherwise the original pass.
        if (Settings.Main.Predication || Settings.Main.DeDither) {
            // fixes-a #33: an older SMAA.fx has no Stable technique: fall back to the original pass instead of drawing nothing.
            D3DXHANDLE stable = TechniqueHandle(techLumaDepthStable, "LumaDepthEdgeDetectionStable");
            Effect->SetTechnique(stable ? stable : TechniqueHandle(techLumaDepth, "LumaDepthEdgeDetection"));
        } else
            Effect->SetTechnique(TechniqueHandle(techLumaDepth, "LumaDepthEdgeDetection"));
        break;
    default:
		return;
    }

    UINT passes;
    Effect->Begin(&passes, 0);
    Effect->BeginPass(0);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    Effect->EndPass();
    Effect->End();
}


void SMAAEffect::BlendingWeightsCalculationPass() {
	IDirect3DDevice9* Device = TheRenderManager->device;

    // Set the render target and clear it.
    Device->SetRenderTarget(0, Textures.SMAA_Blend_Surface);
    Device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

    Effect->SetTechnique(TechniqueHandle(techBlend, "BlendWeightCalculation"));

	UINT passes;
	Effect->Begin(&passes, 0);
	Effect->BeginPass(0);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	Effect->EndPass();
	Effect->End();
}


void SMAAEffect::NeighborhoodBlendingPass(IDirect3DSurface9* RenderTarget) {
	IDirect3DDevice9* Device = TheRenderManager->device;

    Device->SetRenderTarget(0, RenderTarget);
    Effect->SetTechnique(TechniqueHandle(techNeighborhood, "NeighborhoodBlending"));

	UINT passes;
	Effect->Begin(&passes, 0);
	Effect->BeginPass(0);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	Effect->EndPass();
	Effect->End();
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the three passes through the extension. Edges and weights go into SMAA's own targets
// (ExtDrawTo), the neighborhood blend writes the frame through the chain (ExtFullScreen). Without a stencil the edge pass writes
// its mark (the pixel was not discarded) into the edges' alpha and the weight pass writes 0 where it is missing, as the D3D9
// stencil test leaves the cleared target; every pixel of both targets is written, so neither needs the D3D9 clear.
// 2026-10-26 (extension 1.6, NvrDxvkStencil): as the D3D9 techniques instead: both targets and SMAA's own stencil cleared (D3D9
// Clear, in order with the passes), the edge pass discards the pixels without an edge and marks the others (REPLACE 1), the weight
// pass (32-step searches) runs only where the mark is (EQUAL 1). Measured before it: SMAA +0.10-0.15 ms over its D3D9 passes.
namespace {
	struct SMAAPush {
		D3DXVECTOR4 Resolution, Data, DepthData, Pred, ToneMappingClip;	// TESR_SMAA*, TESR_ToneMappingClip
		UInt32 samplerIndex, pointSamplerIndex, mode, flags;			// LINEAR/CLAMP at 80, POINT/CLAMP at 84; flags 1: the edge pass discards
	};
	const NVR_EXT_BINDING_DESC SMAAEdgesBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 0, 0, NVR_EXT_BIND_SAMPLER,       0, NVR_EXT_VIEW_2D, FALSE, 80 },
	};
	const NVR_EXT_BINDING_DESC SMAAWeightsBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_edges: TESR_SMAA_Edges
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_area: TESR_SMAA_AreaTex
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_search: TESR_SMAA_SearchTex
		{ 0, 0, NVR_EXT_BIND_SAMPLER,       0, NVR_EXT_VIEW_2D, FALSE, 80 },
		{ 0, 0, NVR_EXT_BIND_SAMPLER,       1, NVR_EXT_VIEW_2D, FALSE, 84 },
	};
	const NVR_EXT_BINDING_DESC SMAANeighborhoodBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_blend: TESR_SMAA_Blend
		{ 0, 0, NVR_EXT_BIND_SAMPLER,       0, NVR_EXT_VIEW_2D, FALSE, 80 },
	};
}

bool SMAAEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	IDirect3DBaseTexture9* area = ExtFileTexture("TESR_SMAA_AreaTex");
	IDirect3DBaseTexture9* search = ExtFileTexture("TESR_SMAA_SearchTex");
	if (!depth || !area || !search || !Textures.SMAA_Edges_Texture || !Textures.SMAA_Blend_Texture || !TheTextureManager->RenderedTexture) return false;

	// The edge technique EdgesDetectionPass picks.
	UInt32 mode;
	switch (Settings.Main.EdgeDetection) {
	case INPUT_LUMA: mode = 0; break;
	case INPUT_COLOR: mode = 1; break;
	case INPUT_DEPTH: mode = 2; break;
	case INPUT_LUMADEPTH:
		mode = (Settings.Main.Predication || Settings.Main.DeDither) && TechniqueHandle(techLumaDepthStable, "LumaDepthEdgeDetectionStable") ? 4 : 3;
		break;
	default: return false;
	}

	// Whether the D3D9 weight pass has a stencil to test: SMAA's own surface (Render makes it unless that failed), else the bound one
	// (ReShadeDepthCompat, or no own surface) if it has stencil bits. Without one every pixel runs there, so here too.
	bool masked = !Textures.StencilFailed && !ReShadeIntegration::KeepDepthBound();
	if (!masked) {
		IDirect3DSurface9* bound = nullptr;
		D3DSURFACE_DESC desc;
		if (SUCCEEDED(Device->GetDepthStencilSurface(&bound)) && bound && SUCCEEDED(bound->GetDesc(&desc)))
			masked = desc.Format == D3DFMT_D24S8 || desc.Format == D3DFMT_D24FS8 || desc.Format == D3DFMT_D24X4S4 || desc.Format == D3DFMT_D15S1;
		if (bound) bound->Release();
	}
	// UNOFFICIAL 2026-10-26: the D3D9 techniques' stencil itself (extension 1.6), on SMAA's own surface (not the ReShadeDepthCompat one).
	const bool stencil = masked && DxvkCompute::StencilOn() && !ReShadeIntegration::KeepDepthBound()
		&& (MakeStencil(Device, Textures.StencilSurface, Textures.StencilFailed), Textures.StencilSurface != nullptr);

	auto make =[](ExtPass& pass, const char* spvName, const NVR_EXT_BINDING_DESC* bindings, UINT count) {
		if (!pass.Tried) {
			pass.Tried = true;
			pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, count, sizeof(SMAAPush), true);
		}
		return pass.Pipeline != nullptr;
	};
	// UNOFFICIAL 2026-10-26: the edge technique's own variant (one D3D9 technique per mode; Compute\variants.txt), else the general shader.
	char edgeSuffix[8];
	_snprintf_s(edgeSuffix, sizeof(edgeSuffix), _TRUNCATE, "m%u", mode);
	ID3D9NvrExtPipeline* edgesPipeline = ExtVariant(extVariants, extEdges, "SMAAEdges", edgeSuffix, [](const char* name) {
		return DxvkCompute::CreateGraphicsPipeline(name, SMAAEdgesBindings, _countof(SMAAEdgesBindings), sizeof(SMAAPush), true);
	});
	if (!edgesPipeline ||
		!make(extWeights, "SMAAWeights", SMAAWeightsBindings, _countof(SMAAWeightsBindings)) ||
		!make(extNeighborhood, "SMAANeighborhood", SMAANeighborhoodBindings, _countof(SMAANeighborhoodBindings))) return false;
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!linear || !point) return false;

	SMAAPush push = {};
	push.Resolution = Constants.Resolution;
	push.Data = Constants.Data;
	push.DepthData = Constants.DepthData;
	push.Pred = Constants.Pred;
	if (TheShaderManager->Shaders.Tonemapping) push.ToneMappingClip = TheShaderManager->Shaders.Tonemapping->Constants.Clip;
	push.mode = mode;

	// The stencil route clears what the D3D9 path clears: both targets and the stencil (render target 0 is put back by Render).
	NVR_EXT_DEPTH_STENCIL mark = {}, test = {};
	if (stencil) {
		IDirect3DSurface9* previousDepth = nullptr;
		if (FAILED(Device->GetDepthStencilSurface(&previousDepth))) previousDepth = nullptr;	// none bound
		Device->SetRenderTarget(0, Textures.SMAA_Edges_Surface);
		Device->SetDepthStencilSurface(Textures.StencilSurface);
		Device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
		Device->SetRenderTarget(0, Textures.SMAA_Blend_Surface);
		Device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
		Device->SetDepthStencilSurface(previousDepth);
		if (previousDepth) previousDepth->Release();

		mark.pResource = test.pResource = Textures.StencilSurface;
		mark.StencilTest = test.StencilTest = TRUE;
		mark.StencilReference = test.StencilReference = 1;
		mark.StencilReadMask = test.StencilReadMask = 0xFF;
		mark.StencilCompare = NVR_EXT_COMPARE_ALWAYS;	// the edge techniques: StencilFunc ALWAYS, StencilPass REPLACE, StencilRef 1
		mark.StencilPassOp = NVR_EXT_STENCIL_REPLACE;
		mark.StencilWriteMask = 0xFF;
		test.StencilCompare = NVR_EXT_COMPARE_EQUAL;	// BlendWeightCalculation: StencilFunc EQUAL, StencilPass KEEP, StencilRef 1
	}

	// Edge detection into TESR_SMAA_Edges. Nothing of the frame is touched before the last pass, so a failure here still leaves
	// this frame to the D3D9 technique.
	push.flags = stencil ? 1u : 0u;
	NVR_EXT_RESOURCE_BINDING edges[3] = {};
	edges[0].Slot = 0; edges[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; edges[0].pResource = TheTextureManager->RenderedTexture;
	edges[1].Slot = 1; edges[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; edges[1].pResource = depth;
	edges[2].Slot = 0; edges[2].Type = NVR_EXT_BIND_SAMPLER; edges[2].pResource = linear;
	if (!ExtDrawTo(edgesPipeline, &Textures.SMAA_Edges_Surface, 1, edges, _countof(edges), &push, sizeof(push), nullptr, ExtUv::TexelCentre,
		stencil ? &mark : nullptr)) return false;

	// Blending weights into TESR_SMAA_Blend, only where the edge pass marked: the stencil test, else the edges' alpha (mode 1).
	push.flags = 0u;
	push.mode = masked && !stencil ? 1 : 0;
	NVR_EXT_RESOURCE_BINDING weights[5] = {};
	weights[0].Slot = 0; weights[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; weights[0].pResource = Textures.SMAA_Edges_Texture;
	weights[1].Slot = 1; weights[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; weights[1].pResource = area;
	weights[2].Slot = 2; weights[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; weights[2].pResource = search;
	weights[3].Slot = 0; weights[3].Type = NVR_EXT_BIND_SAMPLER; weights[3].pResource = linear;
	weights[4].Slot = 1; weights[4].Type = NVR_EXT_BIND_SAMPLER; weights[4].pResource = point;
	if (!ExtDrawTo(extWeights.Pipeline, &Textures.SMAA_Blend_Surface, 1, weights, _countof(weights), &push, sizeof(push), nullptr, ExtUv::TexelCentre,
		stencil ? &test : nullptr)) return false;

	// Neighborhood blending: the frame. SMAA's D3D9 path never refreshes TESR_SourceBuffer (it declares no such sampler): no SourceBuffer.
	push.mode = 0;
	NVR_EXT_RESOURCE_BINDING blend[3] = {};
	blend[0].Slot = 0; blend[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; blend[0].pResource = TheTextureManager->RenderedTexture;
	blend[1].Slot = 1; blend[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; blend[1].pResource = Textures.SMAA_Blend_Texture;
	blend[2].Slot = 0; blend[2].Type = NVR_EXT_BIND_SAMPLER; blend[2].pResource = linear;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, nullptr, extNeighborhood.Pipeline, blend, _countof(blend), &push, sizeof(push), true);
}
