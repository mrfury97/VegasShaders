#include "Cinema.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void CinemaEffect::UpdateConstants() {
	const auto& game = TheShaderManager->GameState;

	// 1.0 keeps the native aspect ratio (no letterbox)
	Constants.Data.x = 1.0f;
	if (!Enabled) return;

	// Mode 0: always on; 1: off in dialogue; 2: dialogue only; 3: never (it was Oblivion's persuasion only; New Vegas has no
	// persuasion menu); 4: dialogue only (it was dialogue or persuasion)
	bool suppressed = false;
	if (Mode == 1) suppressed = game.isDialog;
	else if (Mode == 2 || Mode == 4) suppressed = !game.isDialog;
	else if (Mode == 3) suppressed = true;

	if (!suppressed) Constants.Data.x = aspectRatio;
}

void CinemaEffect::UpdateSettings() {
	const char* sec = "Shaders.Cinema.Main";
	SettingManager* cfg = TheSettingManager;
	Mode = cfg->GetSettingI(sec, "Mode");
	aspectRatio = cfg->GetSettingF(sec, "AspectRatio");
	Constants.Data.y = cfg->GetSettingF(sec, "VignetteRadius");
	Constants.Data.z = cfg->GetSettingF(sec, "VignetteDarkness");
	Constants.Data.w = cfg->GetSettingF(sec, "OverlayStrength");
	Constants.Settings.y = cfg->GetSettingF(sec, "FilmGrainAmount");
	Constants.Settings.z = cfg->GetSettingF(sec, "ChromaticAberration");
	Constants.Settings.w = cfg->GetSettingF(sec, "LetterBoxDepth");
}

void CinemaEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_CinemaData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_CinemaSettings", &Constants.Settings);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): Cinema.frag; the textures and constants the techniques read (EffectRecord::ExtTexturePass).
// The blue noise file texture is POINT/WRAP (its sampler's MIN/MAG NONE); the frame and the depth are LINEAR/CLAMP.
// The sky dither's constant is SkyDither's own (TESR_SkyDitherData); the depth is CombineDepth's (TESR_DepthBuffer).
bool CinemaEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex > 1) return false;
	struct Push {
		D3DXVECTOR4 cinemaData, cinemaSettings, gameTime, reciprocalResolution, cameraData, skyDitherData;
		UInt32 samplerIndex, noiseSamplerIndex, dither, pad;
	} push = {};
	push.cinemaData = Constants.Data;
	push.cinemaSettings = Constants.Settings;
	push.gameTime = TheShaderManager->ShaderConst.GameTime;
	push.reciprocalResolution = TheShaderManager->ShaderConst.ReciprocalResolution;
	push.cameraData = TheRenderManager->CameraData;
	push.skyDitherData = TheShaderManager->Effects.SkyDither ? TheShaderManager->Effects.SkyDither->Constants.Data : D3DXVECTOR4(0, 0, 0, 0);
	push.dither = techniqueIndex;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture,
		TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr,
		ExtFileTexture("TESR_BlueNoiseSampler") };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Cinema", textures, _countof(textures), &push, sizeof(push), 96, false, 100, true);
}