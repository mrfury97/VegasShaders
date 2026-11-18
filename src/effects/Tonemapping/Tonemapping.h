#pragma once

class TonemappingShaders : public ShaderCollection
{
public:
	TonemappingShaders() : ShaderCollection("Tonemapping") {};

	struct ValuesStruct {
		float HighlightSaturation, WeatherContrast, ToneMappingColor, Linearization;
		float BloomBlend, BloomStrength, WeatherModifier, TonemapWhitePoint;
		float TonemappingMode, Exposure, Saturation, Gamma;

		float TonemapContrast, TonemapBrightness, TonemapMidpoint, TonemapShoulder;
		float NightShift, NightShiftLevel;	// 2026-11-04 Night colour shift
	};

	struct TonemappingSettingsStruct { ValuesStruct Main, Night, Interiors; };
	TonemappingSettingsStruct Settings;
	bool NightShiftEnabled = false;	// [Shaders.Tonemapping.Main] NightShiftEnabled
	bool AgxContrast = false;	// UNOFFICIAL 2026-11-13 (fork port) [Shaders.Tonemapping.Main] AgxContrast: TonemapContrast also acts in AgX (mode 9)

	struct TonemappingStruct {
		D3DXVECTOR4		ToneMapping, BloomData, SkyData, HDRData, LotteData;
		D3DXVECTOR4		Clip;			// UNOFFICIAL TESR_ToneMappingClip: x HueSafeClip
		D3DXVECTOR4		Night;			// 2026-11-04 TESR_ToneMappingNight: x strength (0 off), y level
		float			PointLightMult;
	};
	TonemappingStruct Constants;

	void	UpdateConstants();
	static void	ScaleLotte(D3DXVECTOR4& LotteData, float whitePoint);	// UNOFFICIAL: the VTLottes constants (also the self-test's VTLottes shot)
	void	RegisterConstants();
	void	UpdateSettings();
};
