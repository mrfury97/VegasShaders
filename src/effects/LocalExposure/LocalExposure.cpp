// 2026-11-04 local exposure (LocalExposure.h, docs/EFFECTS.md "LocalExposure").

#include <algorithm>

#include "../../core/DxvkCompute.h"
#include "LocalExposure.h"
#include "../../core/RenderRect.h"

void LocalExposureEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_LocalExposureData", &Constants.Data);
	sm->RegisterConstant("TESR_LocalExposureSize", &Constants.Size);
	sm->RegisterConstant("TESR_LocalExposureBlur", &Constants.Blur);
}

void LocalExposureEffect::RegisterTextures() {
	// 1/16 resolution: a neighbourhood is far larger than a texel, so this is plenty, and the three passes cost next to nothing.
	// 16-bit float: log2 brightness runs from about -13 (black) to +10 (the sun).
	const UINT w = (std::max)((UINT)TheRenderManager->width / 16, 8u);
	const UINT h = (std::max)((UINT)TheRenderManager->height / 16, 8u);
	TheTextureManager->InitTexture("TESR_LocalExposureBuffer", &Textures.BufferTexture, &Textures.BufferSurface, w, h, D3DFMT_R16F);
	TheTextureManager->InitTexture("TESR_LocalExposureTemp", &Textures.TempTexture, &Textures.TempSurface, w, h, D3DFMT_R16F);
	TheTextureManager->InitTexture("TESR_LocalExposureMean", &Textures.MeanTexture, &Textures.MeanSurface, 1, 1, D3DFMT_R16F);
	Constants.Size = D3DXVECTOR4(1.0f / (float)w, 1.0f / (float)h, 1.0f / (float)TheRenderManager->width, 1.0f / (float)TheRenderManager->height);
}

void LocalExposureEffect::UpdateSettings() {
	// The tone mapper's flag: UpdateConstants and RenderFrame only run while the effect is on, so switching it off clears it here.
	Constants.Data.w = 0.0f;

	const char* section = "Shaders.LocalExposure.Main";
	SettingManager* cfg = TheSettingManager;
	Settings.Shadows = std::clamp(cfg->GetSettingF(section, "Shadows"), 0.0f, 1.0f);
	Settings.Highlights = std::clamp(cfg->GetSettingF(section, "Highlights"), 0.0f, 1.0f);
	Settings.EdgeGuard = std::clamp(cfg->GetSettingF(section, "EdgeGuard"), 0.0f, 4.0f);
	Settings.Blur = std::clamp(cfg->GetSettingF(section, "Blur"), 0.5f, 12.0f);
	Settings.Interiors = cfg->GetSettingI(section, "Interiors") != 0;
	Settings.Exteriors = cfg->GetSettingI(section, "Exteriors") != 0;
	Settings.DebugView = std::clamp(cfg->GetSettingI(section, "DebugView"), 0, 1);
}

void LocalExposureEffect::UpdateConstants() {
	Constants.Data.x = Settings.Shadows;
	Constants.Data.y = Settings.Highlights;
	Constants.Data.z = Settings.EdgeGuard;
	Constants.Blur = D3DXVECTOR4(Settings.Blur, (float)Settings.DebugView, 0.0f, 0.0f);
}

bool LocalExposureEffect::ShouldRender() {
	const auto& state = TheShaderManager->GameState;
	const bool here = state.isExterior ? Settings.Exteriors : Settings.Interiors;
	return here && (Settings.Shadows > 0.0f || Settings.Highlights > 0.0f || Settings.DebugView > 0);
}

bool LocalExposureEffect::ExtRenderFrame(IDirect3DDevice9* Device) {
	if (!DxvkCompute::Available() || !TheTextureManager->RenderedTexture || !Textures.BufferTexture || !Textures.TempTexture) return false;

	struct LocalExposureUniforms {
		float toneMapping[4];
		float size[4];
		float blur[4];
		float renderScale[4];
	} uniforms = {};

	auto tmIt = TheShaderManager->ConstantsTable.find("TESR_ToneMapping");
	if (tmIt != TheShaderManager->ConstantsTable.end() && tmIt->second) {
		memcpy(uniforms.toneMapping, tmIt->second, sizeof(uniforms.toneMapping));
	}
	memcpy(uniforms.size, &Constants.Size, sizeof(uniforms.size));
	memcpy(uniforms.blur, &Constants.Blur, sizeof(uniforms.blur));
	auto rsIt = TheShaderManager->ConstantsTable.find("TESR_RenderScale");
	if (rsIt != TheShaderManager->ConstantsTable.end() && rsIt->second) {
		memcpy(uniforms.renderScale, rsIt->second, sizeof(uniforms.renderScale));
	}
	else {
		uniforms.renderScale[0] = uniforms.renderScale[1] = uniforms.renderScale[2] = uniforms.renderScale[3] = 1.0f;
	}

	ID3D9NvrExtBuffer* cb = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	if (!cb || !linear) return false;

	struct Push { UInt32 linearSampler; UInt32 pad[3]; } push = {};
	auto getPipe = [&](ExtPass& pass, const char* spvName) -> ID3D9NvrExtPipeline* {
		if (!pass.Tried) {
			pass.Tried = true;
			NVR_EXT_BINDING_DESC bindings[3] = {};
			bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0; bindings[0].ViewType = NVR_EXT_VIEW_2D;
			bindings[1].Set = 2; bindings[1].Binding = 0; bindings[1].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[1].Slot = 0; bindings[1].ViewType = NVR_EXT_VIEW_2D;
			bindings[2].Type = NVR_EXT_BIND_SAMPLER; bindings[2].Slot = 0; bindings[2].PushOffset = 0;
			pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, _countof(bindings), sizeof(push), true);
		}
		return pass.Pipeline;
	};

	ID3D9NvrExtPipeline* pMeasure = getPipe(extMeasure, "LocalExposureMeasure");
	ID3D9NvrExtPipeline* pBlurH = getPipe(extBlurH, "LocalExposureBlurH");
	ID3D9NvrExtPipeline* pBlurV = getPipe(extBlurV, "LocalExposureBlurV");
	ID3D9NvrExtPipeline* pMean = getPipe(extMean, "LocalExposureMean");
	if (!pMeasure || !pBlurH || !pBlurV || !pMean) return false;

	RECT rectVp = {};
	const RECT* lowVp = nullptr;
	if (RenderRect::Active()) {
		D3DSURFACE_DESC desc = {};
		if (SUCCEEDED(Textures.BufferSurface->GetDesc(&desc))) {
			rectVp = { 0, 0, (LONG)RenderRect::Width(desc.Width), (LONG)RenderRect::Height(desc.Height) };
			lowVp = &rectVp;
		}
	}

	auto drawPass = [&](ID3D9NvrExtPipeline* pipe, IDirect3DSurface9* target, IDirect3DBaseTexture9* src, const RECT* vp) {
		NVR_EXT_RESOURCE_BINDING res[3] = {};
		res[0].Slot = 0; res[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[0].pResource = src;
		res[1].Slot = 0; res[1].Type = NVR_EXT_BIND_UNIFORM_BUFFER; res[1].pResource = cb;
		res[2].Slot = 0; res[2].Type = NVR_EXT_BIND_SAMPLER; res[2].pResource = linear;
		return ExtDrawTo(pipe, &target, 1, res, _countof(res), &push, sizeof(push), vp, ExtUv::ScreenQuad);
	};

	return drawPass(pMeasure, Textures.BufferSurface, TheTextureManager->RenderedTexture, lowVp) &&
		   drawPass(pBlurH, Textures.TempSurface, Textures.BufferTexture, lowVp) &&
		   drawPass(pBlurV, Textures.BufferSurface, Textures.TempTexture, lowVp) &&
		   drawPass(pMean, Textures.MeanSurface, Textures.BufferTexture, nullptr);
}

void LocalExposureEffect::RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	Constants.Data.w = 0.0f;	// the tone mapper skips it unless the buffer is drawn below, this frame
	if (!Enabled || !Textures.BufferSurface || !Textures.TempSurface || !Textures.MeanSurface || !ShouldRender()) return;
	const bool extOk = !ExtFailed && DxvkCompute::PassesOnRect() && !DxvkCompute::KeptOnD3D9(Name) && ExtRenderFrame(Device);
	if (!extOk) {
		if (!Effect) return;
		ShaderManager* sm = TheShaderManager;
		sm->RenderEffectToRT(Textures.BufferSurface, this, false, 0);	// the scene's log brightness (reads the current image)
		sm->RenderEffectToRT(Textures.TempSurface, this, false, 1);		// horizontal blur
		sm->RenderEffectToRT(Textures.BufferSurface, this, false, 2);	// vertical blur, back into the buffer the tone mapper reads
		sm->RenderEffectToRT(Textures.MeanSurface, this, false, 3);		// the whole view's mean (what neighbourhoods are pulled toward)
	}
	Device->SetRenderTarget(0, RenderTarget);
	// RenderScale v2: drawn in the buffer's top-left part in a rect frame; the tone mapper reads it after the upscale, at full size
	// (as Bloom's top level).
	RenderRect::RescaleAfterUpscale(Textures.BufferSurface);
	Constants.Data.w = 1.0f;
}
