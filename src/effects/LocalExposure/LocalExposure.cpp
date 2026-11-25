// 2026-11-04 local exposure (LocalExposure.h, docs/EFFECTS.md "LocalExposure").

#include <algorithm>

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

void LocalExposureEffect::RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	Constants.Data.w = 0.0f;	// the tone mapper skips it unless the buffer is drawn below, this frame
	if (!Enabled || !Effect || !Textures.BufferSurface || !Textures.TempSurface || !Textures.MeanSurface || !ShouldRender()) return;
	ShaderManager* sm = TheShaderManager;
	sm->RenderEffectToRT(Textures.BufferSurface, this, false, 0);	// the scene's log brightness (reads the current image)
	sm->RenderEffectToRT(Textures.TempSurface, this, false, 1);		// horizontal blur
	sm->RenderEffectToRT(Textures.BufferSurface, this, false, 2);	// vertical blur, back into the buffer the tone mapper reads
	sm->RenderEffectToRT(Textures.MeanSurface, this, false, 3);		// the whole view's mean (what neighbourhoods are pulled toward)
	Device->SetRenderTarget(0, RenderTarget);
	// RenderScale v2: drawn in the buffer's top-left part in a rect frame; the tone mapper reads it after the upscale, at full size
	// (as Bloom's top level).
	RenderRect::RescaleAfterUpscale(Textures.BufferSurface);
	Constants.Data.w = 1.0f;
}
