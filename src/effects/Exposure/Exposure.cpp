#include "Exposure.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include <algorithm>

void ExposureEffect::UpdateConstants() {
	TheShaderManager->avglumaRequired = true; // the screen average must be computed this frame

	bool stale = TheSettingManager->SettingsChanged || TheShaderManager->GameState.isDayTimeChanged;
	if (!stale) return;

	const ExposureSettingsStruct& s = Settings;
	ShaderManager* sm = TheShaderManager;
	Constants.Data.x = sm->GetTransitionValue(s.Main.MinBrightness, s.Night.MinBrightness, s.Interiors.MinBrightness);
	Constants.Data.y = sm->GetTransitionValue(s.Main.MaxBrightness, s.Night.MaxBrightness, s.Interiors.MaxBrightness);
	Constants.Data.z = sm->GetTransitionValue(s.Main.DarkAdaptSpeed, s.Night.DarkAdaptSpeed, s.Interiors.DarkAdaptSpeed);
	Constants.Data.w = sm->GetTransitionValue(s.Main.LightAdaptSpeed, s.Night.LightAdaptSpeed, s.Interiors.LightAdaptSpeed);
	// UNOFFICIAL GT7 adaptation (Mode 1): its own strengths, blended by the time of day like the values above.
	Constants.GT7.x = sm->GetTransitionValue(s.Main.GT7AutoExposure, s.Night.GT7AutoExposure, s.Interiors.GT7AutoExposure);
	Constants.GT7.y = sm->GetTransitionValue(s.Main.GT7WhiteBalance, s.Night.GT7WhiteBalance, s.Interiors.GT7WhiteBalance);
	Constants.GT7.z = sm->GetTransitionValue(s.Main.GT7WeatherLight, s.Night.GT7WeatherLight, s.Interiors.GT7WeatherLight);
	Constants.GT7.w = (float)s.Mode;
}

// Fills one brightness group from its settings section.
static void ReadExposureGroup(const char* section, ExposureEffect::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.MinBrightness = cfg->GetSettingF(section, "MinBrightness");
	out.MaxBrightness = cfg->GetSettingF(section, "MaxBrightness");
	out.DarkAdaptSpeed = cfg->GetSettingF(section, "DarkAdaptSpeed");
	out.LightAdaptSpeed = cfg->GetSettingF(section, "LightAdaptSpeed");
	out.GT7AutoExposure = std::clamp(cfg->GetSettingF(section, "AdaptExposure"), 0.0f, 1.0f);	// UNOFFICIAL GT7 adaptation (Mode 1)
	out.GT7WhiteBalance = std::clamp(cfg->GetSettingF(section, "AdaptWhiteBalance"), 0.0f, 1.0f);
	out.GT7WeatherLight = std::clamp(cfg->GetSettingF(section, "AdaptWeatherLight"), 0.0f, 1.0f);
}

void ExposureEffect::UpdateSettings(){
	ReadExposureGroup("Shaders.Exposure.Main", Settings.Main);
	ReadExposureGroup("Shaders.Exposure.Night", Settings.Night);
	ReadExposureGroup("Shaders.Exposure.Interiors", Settings.Interiors);

	// UNOFFICIAL 2026-10-26: histogram trimming (both percentages are capped at 90)
	const char* main = "Shaders.Exposure.Main";
	Settings.Histogram = TheSettingManager->GetSettingI(main, "Histogram") != 0;
	Settings.HistogramLowPercent = std::clamp(TheSettingManager->GetSettingF(main, "HistogramLowPercent"), 0.0f, 90.0f);
	Settings.HistogramHighPercent = std::clamp(TheSettingManager->GetSettingF(main, "HistogramHighPercent"), 0.0f, 90.0f);
	Settings.Mode = std::clamp(TheSettingManager->GetSettingI(main, "Mode"), 0, 1);	// UNOFFICIAL GT7 adaptation
}

void ExposureEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_ExposureData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_GT7AdaptData", &Constants.GT7);	// UNOFFICIAL GT7 adaptation (Effects/Includes/GT7Adaptation.hlsl)
}

// UNOFFICIAL 2026-11-15 fpnative-fix (Exposure.h): the same tests EffectRecord::Render makes before it draws.
void ExposureEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* Target, IDirect3DSurface9* Rendered, UINT techniqueIndex, bool ClearTarget, IDirect3DSurface9* Source) {
	if (Enabled && Effect && ShouldRender()) DrawnFrame = TheFrameRateManager->FrameNumber;
	EffectRecord::Render(Device, Target, Rendered, techniqueIndex, ClearTarget, Source);
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): Exposure.frag; the textures and constants the technique reads (EffectRecord::ExtTexturePass).
bool ExposureEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;	// (technique 1, the GT7 adaptation, has no Vulkan port: it draws its D3D9 pass)
	struct Push { D3DXVECTOR4 exposureData; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.exposureData = Constants.Data;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->RenderedTexture, TheShaderManager->Effects.AvgLuma ? TheShaderManager->Effects.AvgLuma->Textures.AvgLumaTexture : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Exposure", textures, _countof(textures), &push, sizeof(push), 16);
}
