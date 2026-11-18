#include "Rain.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include "../../core/RainState.h"	// UNOFFICIAL 2026-11-12 RainStateGuard

void RainEffect::UpdateConstants() {
	// UNOFFICIAL 2026-11-12 RainStateGuard ([Shaders.WetWorld.Main] RainStateGuard, core/RainState.h): on, the one rain state drives
	// RainAnimator and fills Data.x every frame, whether this effect is on or off (WetWorld, the Reflections effect's wetness,
	// Specular's Rain look and the volumetric light read the animator too). Off: animated here as before, so only while this effect is on.
	if (RainState::Tick()) return;
	Animator& fall = Constants.RainAnimator;
	const bool raining = TheShaderManager->GameState.isRainy;

	if (raining && !fall.switched) {
		// rain has just begun: fade the drops in
		fall.switched = true;
		fall.Start(0.05f, 1);
	}
	else if (!raining && fall.switched) {
		// rain has just ended: fade them out
		fall.switched = false;
		fall.Start(0.07f, 0);
	}

	Constants.Data.x = fall.GetValue();
}

void RainEffect::UpdateSettings() {
	const char* section = "Shaders.Precipitations.Main";
	SettingManager* cfg = TheSettingManager;
	Constants.Data.y = cfg->GetSettingF(section, "VerticalScale");
	Constants.Data.z = cfg->GetSettingF(section, "Speed");
	Constants.Data.w = cfg->GetSettingF(section, "Opacity");

	Constants.Aspect.x = cfg->GetSettingF(section, "Refraction");
	Constants.Aspect.y = cfg->GetSettingF(section, "Coloring");
	Constants.Aspect.z = cfg->GetSettingF(section, "Bloom");
	// UNOFFICIAL 2026-10-17 (Precipitations.fx.hlsl): skip the rain layers behind each pixel (they add exactly 0). A key missing
	// from the user's settings file reads the shipped default (on); only a key missing from the DEFAULTS toml reads 0 = off.
	// Note (fixes-b #37): GetOrtho samples with tex2Dlod level 0 whether this option is on or off (identical unless a driver
	// anisotropic override acts in minification).
	Constants.Aspect.w = cfg->GetSettingI(section, "SkipHiddenLayers") != 0 ? 1.0f : 0.0f;
}

void RainEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_RainData", &Constants.Data);
	sm->RegisterConstant("TESR_RainAspect", &Constants.Aspect);
}

bool RainEffect::ShouldRender() {
	const auto& game = TheShaderManager->GameState;
	if (game.isUnderwater || !game.isExterior) return false;
	return Constants.Data.x > 0.0f;
};

// UNOFFICIAL 2026-10-25 (NVR DXVK): Precipitations.frag; the textures the D3D9 technique binds by name (the source copy, TESR_DepthBuffer,
// TESR_OrthoMapBuffer, TESR_BloomBuffer) and the values it uploads (the same objects RegisterConstant points at).
bool RainEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!shadows) return false;
	struct Push { D3DXMATRIX shadowCameraToLight, viewTransform; D3DXVECTOR4 viewScale, rainData, rainAspect, sunDirection, sunColor; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.shadowCameraToLight = shadows->ShadowMaps[ShadowsExteriorEffect::MapOrtho].ShadowCameraToLight;
	push.viewTransform = TheRenderManager->viewMatrix;
	push.viewScale = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, TheRenderManager->CameraData.y, TheShaderManager->ShaderConst.GameTime.z);
	push.rainData = Constants.Data;
	push.rainAspect = Constants.Aspect;
	push.sunDirection = TheShaderManager->ShaderConst.SunDir;
	push.sunColor = TheShaderManager->ShaderConst.sunColor;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->SourceTexture,
		TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr,
		shadows->ShadowMapOrthoTexture,
		TheShaderManager->Effects.Bloom ? TheShaderManager->Effects.Bloom->Textures.BloomTexture[0] : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Precipitations", textures, _countof(textures), &push, sizeof(push), 208);
}
