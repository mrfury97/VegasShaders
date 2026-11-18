#include "Tonemapping.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise


void TonemappingShaders::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_HDRBloomData", &Constants.BloomData);
	sm->RegisterConstant("TESR_HDRData", &Constants.HDRData);
	sm->RegisterConstant("TESR_LotteData", &Constants.LotteData);
	sm->RegisterConstant("TESR_ToneMapping", &Constants.ToneMapping);
	sm->RegisterConstant("TESR_ToneMappingClip", &Constants.Clip);
	sm->RegisterConstant("TESR_ToneMappingNight", &Constants.Night);	// 2026-11-04 Night colour shift
}


void TonemappingShaders::UpdateConstants() {
	// UNOFFICIAL HueSafeClip (ISHDRBLENDINSHADERCIN.pso.hlsl), live.
	Constants.Clip.x = TheSettingManager->GetSettingI("Shaders.Tonemapping.Main", "HueSafeClip") ? 1.0f : 0.0f;
	// UNOFFICIAL sky dither at the tonemap (both ISHDRBLENDINSHADERCIN shaders): on with Sky Dither and its AtToneMap, the
	// same size as Sky Dither's own noise, a new pattern every frame. Off: y 0, the shaders skip it.
	SkyDitherEffect* dither = TheShaderManager->Effects.SkyDither;
	const bool toneMapDither = dither && dither->Enabled && TheSettingManager->GetSettingI("Shaders.SkyDither.Main", "AtToneMap");
	Constants.Clip.y = toneMapDither ? dither->Settings.Strength / 255.0f : 0.0f;
	// UNOFFICIAL aastab: [Shaders.SkyDither.Main] Pattern 1 = one fixed pattern, so the dither no longer changes from frame to frame.
	// UNOFFICIAL 2026-11-01 SharedNoise ([Main.Main.Quality], core/TemporalBackbone.h): no history averages this dither unless TAA / FSR /
	// DLSS resolves, so it moves only then (the shared index), else stands still at frame 0. Off: the old count (+1 a call, two a frame).
	if (dither && dither->Settings.Pattern == 1) Constants.Clip.z = 0.0f;
	else if (TemporalBackbone::Shared()) Constants.Clip.z = (float)((TemporalBackbone::NoiseIndex() + TemporalBackbone::OffsetTonemap) % 64);
	else Constants.Clip.z = (float)(((int)Constants.Clip.z + 1) % 64);

	if (!(TheSettingManager->SettingsChanged || TheShaderManager->GameState.isDayTimeChanged)) return;

	// Point light multiplier disabled for now
	// (it would come from GetSettingTransition("Shaders.Tonemapping", "PointLightMultiplier", ...))
	ShaderManager* sm = TheShaderManager;
	const TonemappingSettingsStruct& s = Settings;
	// the day/night/interior blend of one value
	auto mix = [&](float ValuesStruct::* field) {
		return sm->GetTransitionValue(s.Main.*field, s.Night.*field, s.Interiors.*field);
	};

	Constants.ToneMapping = D3DXVECTOR4(mix(&ValuesStruct::HighlightSaturation), mix(&ValuesStruct::WeatherContrast),
		mix(&ValuesStruct::ToneMappingColor), mix(&ValuesStruct::Linearization));
	Constants.BloomData = D3DXVECTOR4(mix(&ValuesStruct::BloomBlend), mix(&ValuesStruct::BloomStrength),
		mix(&ValuesStruct::WeatherModifier), mix(&ValuesStruct::TonemapWhitePoint));
	// the operator is chosen, not blended: exteriors use Main's, everywhere else Interiors'
	Constants.HDRData.x = sm->GameState.isExterior ? s.Main.TonemappingMode : s.Interiors.TonemappingMode;
	Constants.HDRData.y = mix(&ValuesStruct::Exposure);
	Constants.HDRData.z = mix(&ValuesStruct::Saturation);
	Constants.HDRData.w = mix(&ValuesStruct::Gamma);

	Constants.LotteData = D3DXVECTOR4(mix(&ValuesStruct::TonemapContrast), mix(&ValuesStruct::TonemapBrightness),
		mix(&ValuesStruct::TonemapMidpoint), mix(&ValuesStruct::TonemapShoulder));

	// 2026-11-04 Night colour shift: blended by time of day and place like the rest; off = strength 0 (the shaders skip it)
	Constants.Night = D3DXVECTOR4(NightShiftEnabled ? max(0.0f, min(1.0f, mix(&ValuesStruct::NightShift))) : 0.0f,
		max(0.01f, mix(&ValuesStruct::NightShiftLevel)), 0.0f, 0.0f);

	// operator 1 (VTLottes/AMDLottes) wants its curve constants derived from the raw values
	if (Constants.HDRData.x == 1.0) ScaleLotte(Constants.LotteData, Constants.BloomData.w);
	// UNOFFICIAL 2026-11-13 (fork port) AgX reads LotteData.x as its contrast (Tonemapping.hlsl agx; 1 = AgX as it was): only with AgxContrast on
	if (Constants.HDRData.x == 9.0f && !AgxContrast) Constants.LotteData.x = 1.0f;
}

// The VTLottes (mode 1) constants from the raw LotteData (contrast, brightness, midpoint, shoulder) and the white point; moved out of
// UpdateConstants unchanged (UNOFFICIAL, 2026-10-12) so the self-test's VTLottes shot can make them for one frame.
void TonemappingShaders::ScaleLotte(D3DXVECTOR4& LotteData, float whitePoint) {
	const float hdrMax = max(1.0, whitePoint * 100.0);
	const float contrast = max(0.01, LotteData.x * 1.35);
	const float shoulder = max(0.0, (min(1.0, LotteData.w * 0.993)));	// never above 1
	const float midIn = max(0.01, LotteData.z * 0.18);
	const float midOut = max(0.01, (LotteData.y * 0.18) / shoulder);

	// the four power terms the two curve coefficients are built from
	const float hdrToContrastShoulder = pows(hdrMax, contrast * shoulder);
	const float midInToContrast = pows(midIn, contrast);
	const float hdrToContrast = pows(hdrMax, contrast);
	const float midInToContrastShoulder = pows(midIn, contrast * shoulder);

	const float numeratorC = hdrToContrastShoulder * midInToContrast - hdrToContrast * midInToContrastShoulder * midOut;
	const float denominator = hdrToContrastShoulder * midOut - midInToContrastShoulder * midOut;

	const float colToneB = -((-midInToContrast + (midOut * numeratorC) / denominator) / (midInToContrastShoulder * midOut));
	const float colToneC = numeratorC / denominator;

	LotteData.x = contrast;
	LotteData.w = shoulder;
	LotteData.z = colToneB;
	LotteData.y = colToneC;
}

// Reads one Tonemapping group; the bloom blend and strength live in the matching Bloom group.
static void ReadTonemapGroup(const char* tonemapSection, const char* bloomSection, TonemappingShaders::ValuesStruct& out) {
	SettingManager* cfg = TheSettingManager;
	out.HighlightSaturation = cfg->GetSettingF(tonemapSection, "HighlightSaturation");
	out.WeatherContrast = cfg->GetSettingF(tonemapSection, "WeatherContrast");
	out.ToneMappingColor = cfg->GetSettingF(tonemapSection, "ToneMappingColor");
	out.Linearization = cfg->GetSettingF(tonemapSection, "Linearization");
	out.BloomBlend = cfg->GetSettingF(bloomSection, "Blending");
	out.BloomStrength = cfg->GetSettingF(bloomSection, "Strength");
	out.WeatherModifier = cfg->GetSettingF(tonemapSection, "WeatherModifier");
	out.TonemapWhitePoint = cfg->GetSettingF(tonemapSection, "TonemapWhitePoint");
	out.TonemappingMode = cfg->GetSettingF(tonemapSection, "TonemappingMode");
	out.Exposure = cfg->GetSettingF(tonemapSection, "Exposure");
	out.Saturation = cfg->GetSettingF(tonemapSection, "Saturation");
	out.Gamma = cfg->GetSettingF(tonemapSection, "Gamma");

	out.TonemapContrast = cfg->GetSettingF(tonemapSection, "TonemapContrast");
	out.TonemapBrightness = cfg->GetSettingF(tonemapSection, "TonemapBrightness");
	out.TonemapMidpoint = cfg->GetSettingF(tonemapSection, "TonemapMidpoint");
	out.TonemapShoulder = cfg->GetSettingF(tonemapSection, "TonemapShoulder");
	out.NightShift = cfg->GetSettingF(tonemapSection, "NightShift");
	out.NightShiftLevel = cfg->GetSettingF(tonemapSection, "NightShiftLevel");
}

void TonemappingShaders::UpdateSettings() {
	ReadTonemapGroup("Shaders.Tonemapping.Main", "Shaders.Bloom.Main", Settings.Main);
	ReadTonemapGroup("Shaders.Tonemapping.Night", "Shaders.Bloom.Night", Settings.Night);
	ReadTonemapGroup("Shaders.Tonemapping.Interiors", "Shaders.Bloom.Interiors", Settings.Interiors);
	NightShiftEnabled = TheSettingManager->GetSettingI("Shaders.Tonemapping.Main", "NightShiftEnabled") != 0;
	{	// UNOFFICIAL 2026-11-13 (fork port) a missing key: off
		SettingManager::Configuration::ConfigNode node;
		AgxContrast = TheSettingManager->Config.FillNode(&node, "Shaders.Tonemapping.Main", "AgxContrast") && node.BoolValue;
	}
}
