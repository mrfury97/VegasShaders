#include "WaterLens.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include <algorithm>

void WaterLensEffect::Reset() {
	fadeValue = 0.0f;
	wasUnderwater = false;
	needsSync = true;
	lastUpdateTime = -1.0;
	Constants.Data.w = 0.0f;
	Constants.WaterLensAnimator.Initialize(0.0f);
	Constants.WaterLensAnimator.switched = false;
}

void WaterLensEffect::UpdateConstants() {
	// UNOFFICIAL 2026-11-20: real-time fade (seconds, not GameDaysPassed game hours) + reset on save load, camera cut, or gap.
	// Previously WaterLens used Animator on GameDaysPassed: reloading an earlier save while fading caused invLerp to extrapolate
	// wildly negative (strength >> 1.0, blinding white contour lines and smeared refraction stuck until game time caught up),
	// low/zero timescale stalled the fade, and reloading from underwater into a dry save fired a false exit-water fade.
	const bool underwater = TheShaderManager->GameState.isUnderwater;
	const double now = TheFrameRateManager->Time;
	const bool gap = lastUpdateTime >= 0.0 && (now < lastUpdateTime || now - lastUpdateTime >= 0.5);

	if (needsSync || gap || TheShaderManager->GameState.isCameraCut || TheSettingManager->GameLoading) {
		wasUnderwater = underwater;
		fadeValue = 0.0f;
		needsSync = false;
		lastUpdateTime = now;
		Constants.Data.w = 0.0f;
		return;
	}

	const float dt = lastUpdateTime >= 0.0 ? (std::clamp)((float)(now - lastUpdateTime), 0.0f, 0.25f) : 0.0f;
	lastUpdateTime = now;

	if (underwater) {
		wasUnderwater = true;
		fadeValue = 0.0f;
	} else if (wasUnderwater) {
		wasUnderwater = false;
		fadeValue = 1.0f;
	} else if (fadeValue > 0.0f) {
		constexpr float kFadeSeconds = 1.5f;
		fadeValue = (std::max)(0.0f, fadeValue - dt / kFadeSeconds);
	}

	fadeValue = (std::clamp)(fadeValue, 0.0f, 1.0f);
	Constants.Data.w = (std::max)(0.0f, amount) * fadeValue;
}

void WaterLensEffect::UpdateSettings(){
	const char* sec = "Shaders.WaterLens.Main";
	Constants.Data.x = TheSettingManager->GetSettingF(sec, "TimeMultA");
	Constants.Data.y = TheSettingManager->GetSettingF(sec, "TimeMultB");
	Constants.Data.z = TheSettingManager->GetSettingF(sec, "Viscosity");
	amount = TheSettingManager->GetSettingF(sec, "Amount");
}

void WaterLensEffect::RegisterConstants(){
	TheShaderManager->RegisterConstant("TESR_WaterLensData", &Constants.Data);
}

bool WaterLensEffect::ShouldRender() {
	return !TheShaderManager->GameState.isUnderwater && Constants.Data.w > 0.0f;
};

// UNOFFICIAL 2026-10-23 (NVR DXVK): WaterLens.frag; the textures (the file texture through the WRAP sampler) and constants the technique reads.
bool WaterLensEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 gameTime, waterLensData; UInt32 samplerIndex; UInt32 wrapIndex; UInt32 pad[2]; } push = {};
	push.gameTime = TheShaderManager->ShaderConst.GameTime;
	push.waterLensData = Constants.Data;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, ExtFileTexture("TESR_waterlensSampler") };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "WaterLens", textures, _countof(textures), &push, sizeof(push), 32, false, 36);
}
