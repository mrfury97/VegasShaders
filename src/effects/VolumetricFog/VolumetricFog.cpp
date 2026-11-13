#include "VolumetricFog.h"
#include "../../core/GpuProfiler.h"
#include "../../core/TestTools.h"	// NVR_TEST_TOOLS (FogReferenceTest)
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

extern D3DXVECTOR4 NvrGameMultisample;	// UNOFFICIAL 2026-11-05 Hooks/Settings.cpp (TESR_GameMultisample: x samples, y shares made, z MsaaEdgeFix)

namespace {
	// UNOFFICIAL 2026-11-05 MsaaEdgeFix: the exact sky shares (core/SkyCoverage.h) for the volumetric light's grid applied here, when the
	// game's MSAA is on, this frame's shares were made and the fix is on (NVR_FogGridFlags.z / push.gridFlags.z); else null.
	IDirect3DBaseTexture9* FogSkyCoverage(const char* path) {
		if (NvrGameMultisample.x <= 1.0f || NvrGameMultisample.y < 0.5f || NvrGameMultisample.z < 0.5f) return nullptr;
		std::string name = "TESR_SkyCoverage";
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(name);
		IDirect3DBaseTexture9* texture = slot ? *slot : nullptr;
		static bool told = false;
		if (texture && !told) { told = true; Logger::Log("UNOFFICIAL MsaaEdgeFix: the volumetric light's grid applied in the fog takes each pixel's sky share (%s)", path); }
		return texture;
	}
}

// UNOFFICIAL 2026-10-17 Skylighting v2: the quarter-resolution sky visibility on stage 12 (NVR_SkyVis), point sampled (the shader
// does its own depth-aware upsample). Not a TESR_ sampler: those bind by their order in the file.
static void BindSky(IDirect3DDevice9* Device, IDirect3DTexture9* sky) {
	Device->SetTexture(12, sky);
	TheRenderManager->renderState->NoteDeviceTexture(12, sky);
	if (!sky) return;
	const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
		{D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
	for (const auto& s : states) TheRenderManager->SetSamplerState(12, (D3DSAMPLERSTATETYPE)s[0], s[1]);
}

// UNOFFICIAL 2026-10-19 SunFar: the land's and the clouds' shadow factor (SunFarEffect::FactorTexture) on stage 13 (NVR_SunFar) for the
// sun-shadow composite, its flag set with it; null unbinds (and the flag must then be 0).
static void BindSunFar(IDirect3DDevice9* Device, IDirect3DTexture9* factor) {
	Device->SetTexture(13, factor);
	TheRenderManager->renderState->NoteDeviceTexture(13, factor);
	if (!factor) return;
	const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
		{D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
	for (const auto& s : states) TheRenderManager->SetSamplerState(13, (D3DSAMPLERSTATETYPE)s[0], s[1]);
}

// F10's timers for the dedicated path's two passes (the D3D9 technique's or the Vulkan passes').
static GpuTimer* FogPassTimers() {
	static GpuTimer timers[2] = { GpuTimer("  Fog estimate (half)"), GpuTimer("  Fog composite (full)") };
	return timers;
}

// The factor for a composite that applies the sun's shadows (null when SunFar drew nothing this frame), its flag set to match.
IDirect3DTexture9* VolumetricFogEffect::SunFarFor(bool shadow) {
	IDirect3DTexture9* factor = shadow && TheShaderManager->Effects.SunFar ? TheShaderManager->Effects.SunFar->FactorTexture() : nullptr;
	D3DXHANDLE handle = ParameterHandle(paramSunFarFlags, "NVR_SunFarFlags");
	if (!handle) return nullptr;
	D3DXVECTOR4 flags(factor ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	Effect->SetVector(handle, &flags);
	return factor;
}

void VolumetricFogEffect::UpdateConstants() {
	// live weather-driven sky-filter disable, smoothly animated the same way RainEffect
	// animates rain onset/offset (Animator started once on the true/false edge, sampled every frame).
	// Animator's clock runs in GAME HOURS, not real seconds (Animator.cpp derives currenttime from
	// GameDaysPassed) -- 0.1f here is a few game-minutes, a few real seconds at typical time scale,
	// matching the same order of magnitude RainEffect uses (0.05f/0.07f) rather than the ~6-real-minute
	// transition an unadjusted "3.0f meaning 3 seconds" mistake would have produced.
	const auto& game = TheShaderManager->GameState;
	const bool weatherHidesFilter = (game.isRainy && rainyDisablesSkyFilter) || (game.isCloudy && cloudyDisablesSkyFilter);

	if (weatherHidesFilter == weatherFilterWasActive) {
		// the weather changed on this frame: fade the filter out (it was active) or back in (it was not)
		weatherFilterWasActive = !weatherHidesFilter;
		Constants.WeatherFilterAnimator.Start(0.1f, weatherFilterWasActive ? 1.0f : 0.0f);
	}

	Constants.Weather.x = Constants.WeatherFilterAnimator.GetValue();
	Constants.Weather.y = game.isExterior ? 1.0f : 0.0f;
	// SkyAmbientRadiance's TESR_SkyIrradiance is only refreshed while Shaders.Sky is enabled;
	// fall back to flat sky color in the shader when it's off rather than reading stale/zero data.
	Constants.Weather.z = TheShaderManager->Shaders.Sky->Enabled ? 1.0f : 0.0f;

	// Moon-phase-driven night ambient ceiling, mirroring ShadowsExterior.cpp's own moon-phase
	// shadow-fade formula (same day-count/phaseLength inputs, same cosine curve) so fog's night
	// floor tracks the same lunar cycle the shadow system already uses. The day/night fade itself
	// is left to the shader's own continuous isDayTime curve rather than duplicated here, so the
	// transition stays smooth instead of snapping at a hard cutoff. Harmless to compute for
	// interiors too -- NightAmbientStrength (read in UpdateSettings) is gated to zero there by the
	// shader's own nightFactor (isExterior term), so this has no effect regardless.
	TimeGlobals* clock = TimeGlobals::Get();
	const float daysPassed = clock->GameDaysPassed ? clock->GameDaysPassed->data : 1.0f;
	const auto phaseLength = Tes->sky->firstClimate->phaseLength;
	float phase = (fmod(daysPassed, 8 * phaseLength & 0x3F)) / (phaseLength & 0x3F);	// 0..8 through the moon's cycle
	phase = std::lerp(-D3DX_PI, D3DX_PI, phase / 8) - D3DX_PI / 4;
	Constants.Global.z = std::lerp(0.0f, nightMinDarkness, cosf(phase) * 0.5f + 0.5f);

	// UNOFFICIAL 2026-10-30 GameFogFloor (user: NVR's fog is nearly invisible in clear weather at noon; the game's own fog clearly hazes
	// the hills): the shader tops NVR's fog up to the game's own fog curve for the weather, pow(saturate((d - near) / (far - near)), power),
	// times this. NVR's density is fixed and thin (about 1 % of the colour at 20000 units in every weather by day, the game's 25 % in
	// clear and 61 % in cloudy weather), and only ever saw the weather's distances as one number for the whole screen. Its Night scale
	// fades in as the shader's nightFactor does (1 - smoothstep(0.4, 0.8, sun amount)). w: the weather's own near, as the game fogs
	// with it (TESR_FogData.x holds 0.17 x far when the weather's near is negative, as most clear ones are: -10).
	ApplyLayer();	// UNOFFICIAL 2026-11-01 HeightLayer: every frame (the camera moves)

	// UNOFFICIAL 2026-11-13 (modern lighting) FoggyWeatherImpact: one WeatherImpact for every weather either veils the clear days
	// (a weather's fog far of 45000 with a power of 0.6 already greys trees at 50 m) or strips the foggy ones. The share follows
	// the weather's own fog distance instead (Sky::fogFarPlane, the game's blend of the day / night and old / new weather values):
	// WeatherImpact at ClearWeatherFar and beyond, FoggyWeatherImpact at FoggyWeatherFar and nearer, eased between on the distance's
	// logarithm. Off (FoggyWeatherImpact < 0) and indoors: WeatherImpact as set.
	if (TheShaderManager->GameState.isExterior && foggyImpact >= 0.0f && Tes->sky) {
		const float far_ = (std::max)(Tes->sky->fogFarPlane, 1.0f);
		float t = (logf(clearFar) - logf(far_)) / (logf(clearFar) - logf(foggyFar));
		t = std::clamp(t, 0.0f, 1.0f);
		t = t * t * (3.0f - 2.0f * t);
		Constants.Density.y = weatherImpactSet + (foggyImpact - weatherImpactSet) * t;
	}
	else Constants.Density.y = weatherImpactSet;

	if (TheShaderManager->GameState.isExterior && gameFogFloor > 0.0f) {
		const float t = std::clamp((TheShaderManager->ShaderConst.SunAmount.x - 0.4f) / 0.4f, 0.0f, 1.0f);
		const float night = 1.0f - t * t * (3.0f - 2.0f * t);
		Constants.Sun.z = gameFogFloor * (1.0f + (gameFogFloorNight - 1.0f) * night);
		Constants.Sun.w = Tes->sky->fogNearPlane;
	}
	else {
		Constants.Sun.z = 0.0f;
		Constants.Sun.w = 0.0f;
	}
}

// UNOFFICIAL 2026-11-01 HeightLayer ([Shaders.VolumetricFog.Main] HeightLayer, LayerBase, LayerThickness, LayerFollowsCamera; tester: the
// fog's height is hard to set since the rewrite; VolumetricFog.fx.hlsl FogLayerMean). A new float4 put the dedicated fog's composite
// pass over ps_3_0's 224 constant registers (X4507), so the layer rides in TESR_VolumetricFogShape, whose x / y (HeightFalloff /
// MaxHeight) only the old height curve reads, which the layer replaces: x = -1 / LayerThickness (negative = the layer is on), y = its
// base in world z (the camera's height added when it follows the camera: set every frame). Outdoors only, never with ClassicLook or
// the legacy fog (they keep HeightFalloff / MaxHeight). Off: x, y exactly the settings, as before.
void VolumetricFogEffect::ApplyLayer() {
	const bool on = layerOn > 0.5f && TheShaderManager->GameState.isExterior && classicLook == 0 && Constants.Legacy.x < 0.5f;
	if (on) {
		Constants.Shape.x = -1.0f / (std::max)(layerThickness, 1.0f);
		Constants.Shape.y = layerBase + (layerFollowsCamera ? TheRenderManager->CameraPosition.z : 0.0f);
	}
	else {
		Constants.Shape.x = shapeFalloff;
		Constants.Shape.y = shapeMaxHeight;
	}
}

void VolumetricFogEffect::UpdateSettings(){

	const bool exterior = TheShaderManager->GameState.isExterior;
	const char* SettingCategory = exterior ? "Shaders.VolumetricFog.Main" : "Shaders.VolumetricFog.Interiors";

	// shorthand for this group's float and integer settings, and for the Night group's floats
	auto F = [&](const char* key) { return TheSettingManager->GetSettingF(SettingCategory, key); };
	auto I = [&](const char* key) { return TheSettingManager->GetSettingI(SettingCategory, key); };
	auto N = [](const char* key) { return TheSettingManager->GetSettingF("Shaders.VolumetricFog.Night", key); };

	// Custom build: one switch for exteriors and interiors, in Main.
	Constants.Legacy.x = TheSettingManager->GetSettingI("Shaders.VolumetricFog.Main", "LegacyFog") ? 1.0f : 0.0f;
	if (Constants.Legacy.x > 0.5f) UpdateLegacySettings(SettingCategory);
	// UNOFFICIAL 2026-10-18 ClassicLook: also one switch for both, in Main (the *Classic techniques, see the header).
	const int classicWas = classicLook;
	const bool referenceWas = referenceTest;
	classicLook = std::clamp(TheSettingManager->GetSettingI("Shaders.VolumetricFog.Main", "ClassicLook"), 0, 2);
#if NVR_TEST_TOOLS
	referenceTest = TheSettingManager->GetSettingI("Main.Main.Performance", "FogReferenceTest") != 0;
#endif
	if (classicLook != classicWas || referenceTest != referenceWas)
		Logger::Log("VOLUMETRIC FOG: %s", referenceTest ? "REFERENCE TEST, the original NVR fog's own shader (FogReference), full resolution, shadow and AO applies as their own passes" :
			IsLegacy() ? "legacy fog (LegacyFog on; ClassicLook does not apply)" : classicLook == 2 ? "ClassicLook 2, the original look, full resolution in one pass" :
			classicLook == 1 ? "ClassicLook 1, the original look with the half-resolution coefficients" : "ClassicLook off (today's fog)");

	Constants.Global.x = F("Amount");
	Constants.Weather.w = F("FogSaturation");

	Constants.Density.x = F("BaseDensity");
	Constants.Density.y = weatherImpactSet = F("WeatherImpact");
	// UNOFFICIAL 2026-11-13 (modern lighting): the foggy weather's share (Main only: outdoors), see UpdateConstants
	foggyImpact = TheSettingManager->GetSettingF("Shaders.VolumetricFog.Main", "FoggyWeatherImpact");
	foggyFar = (std::max)(TheSettingManager->GetSettingF("Shaders.VolumetricFog.Main", "FoggyWeatherFar"), 100.0f);
	clearFar = (std::max)(TheSettingManager->GetSettingF("Shaders.VolumetricFog.Main", "ClearWeatherFar"), foggyFar + 100.0f);

	Constants.Shape.x = shapeFalloff = F("HeightFalloff");
	Constants.Shape.y = shapeMaxHeight = F("MaxHeight");
	Constants.Shape.z = F("Extinction");
	Constants.Shape.w = F("Inscattering");

	float windAngleRad = F("WindAngle") * 0.0174532925f; // degrees to radians
	Constants.Wind.x = cosf(windAngleRad);
	Constants.Wind.y = sinf(windAngleRad);
	Constants.Wind.z = F("WindSpeed");
	Constants.Wind.w = F("NoiseScale");

	Constants.Scatter.z = F("NoiseStrength");
	Constants.Scatter.w = F("HeightInfluence");

	rainyDisablesSkyFilter = I("RainyDisablesSkyFilter") != 0;
	cloudyDisablesSkyFilter = I("CloudyDisablesSkyFilter") != 0;

	if (exterior) {
		Constants.Density.z = F("MorningFogDip");
		Constants.Density.w = F("SunriseSunsetBoost");

		Constants.Scatter.x = F("PhaseAsymmetry");
		Constants.Scatter.y = F("ShadowStrength");
		Constants.Sun.x = std::clamp(F("SunGlow"), 0.0f, 2.0f);	// UNOFFICIAL 2026-10-15
		// UNOFFICIAL 2026-10-30 WeatherFogColor (missing key, an older defaults toml = 1: the weather's fog colour, not silently the old look)
		SettingManager::Configuration::ConfigNode weatherFogColor;
		Constants.Sun.y = TheSettingManager->Config.FillNode(&weatherFogColor, SettingCategory, "WeatherFogColor") ? std::clamp(weatherFogColor.FloatValue, 0.0f, 1.0f) : 1.0f;
		// UNOFFICIAL 2026-10-30 GameFogFloor (UpdateConstants; missing = 0, off) and its night scale (missing = 1: the same by night)
		gameFogFloor = std::clamp(F("GameFogFloor"), 0.0f, 2.0f);
		SettingManager::Configuration::ConfigNode gameFogNight;
		gameFogFloorNight = TheSettingManager->Config.FillNode(&gameFogNight, "Shaders.VolumetricFog.Night", "GameFogFloorScale") ? std::clamp(gameFogNight.FloatValue, 0.0f, 2.0f) : 1.0f;
		// UNOFFICIAL 2026-11-01 HeightLayer (missing keys, an older toml: off, the old height curve)
		SettingManager::Configuration::ConfigNode layerNode;
		layerOn = TheSettingManager->Config.FillNode(&layerNode, SettingCategory, "HeightLayer") && layerNode.BoolValue ? 1.0f : 0.0f;
		layerBase = TheSettingManager->Config.FillNode(&layerNode, SettingCategory, "LayerBase") ? std::clamp(layerNode.FloatValue, -100000.0f, 100000.0f) : 0.0f;
		layerThickness = TheSettingManager->Config.FillNode(&layerNode, SettingCategory, "LayerThickness") ? std::clamp(layerNode.FloatValue, 50.0f, 100000.0f) : 2000.0f;
		layerFollowsCamera = TheSettingManager->Config.FillNode(&layerNode, SettingCategory, "LayerFollowsCamera") && layerNode.BoolValue;

		Constants.Aerial.x = F("AerialStrength");
		Constants.Aerial.y = F("AerialRangeStart");
		Constants.Aerial.z = F("AerialTintBlend");
		Constants.Aerial.w = F("AerialDayFadeStart");

		Constants.AerialTintColor.x = F("AerialTintR");
		Constants.AerialTintColor.y = F("AerialTintG");
		Constants.AerialTintColor.z = F("AerialTintB");

		Constants.Distant.x = F("DistantFogRange");
		Constants.Distant.y = F("DistantFogBlend");
		Constants.Distant.z = F("DistantFogHeight");
		Constants.Distant.w = F("EdgeAA");

		// shared with ShadowsExterior's own moon-phase shadow fade -- keeps fog's night-ambient
		// ceiling consistent with the shadow system's, rather than a second, disconnected knob.
		nightMinDarkness = 1.0f - TheSettingManager->GetSettingF("Shaders.ShadowsExteriors.Main", "NightMinDarkness");

		Constants.Global.w = F("MinDensityFloor");
	}
	else {
		// these settings don't do anything in interiors: no sun, no horizon, no distant view
		Constants.Density.z = 0.0f;
		Constants.Density.w = 0.0f;

		Constants.Scatter.x = 0.0f;
		Constants.Scatter.y = 0.0f;
		Constants.Sun.x = 0.0f;
		Constants.Sun.y = 0.0f;	// (the fog's sky colour indoors is the cell's fog colour already)
		gameFogFloor = 0.0f;	// UNOFFICIAL 2026-10-30 GameFogFloor: outdoors only
		layerOn = 0.0f;	// UNOFFICIAL 2026-11-01 HeightLayer: outdoors only

		Constants.Aerial = D3DXVECTOR4(0, 0, 0, 0);
		Constants.AerialTintColor = D3DXVECTOR4(0, 0, 0, 0);
		Constants.Distant = D3DXVECTOR4(0, 0, 0, 0);

		Constants.Global.w = 0.0f;
	}

	// UNOFFICIAL 2026-11-03 LampHaze (indoors only; SetLampHaze): missing keys (an older toml) take the shipped defaults.
	{
		SettingManager::Configuration::ConfigNode lampNode;
		auto lampF = [&](const char* key, float fallback, float lo, float hi) {
			return std::clamp(TheSettingManager->Config.FillNode(&lampNode, "Shaders.VolumetricFog.Interiors", key) ? lampNode.FloatValue : fallback, lo, hi);
		};
		// 2026-11-04 LampHazeEnabled (off by default): the haze's on/off switch, apart from its strength
		const bool lampOn = TheSettingManager->Config.FillNode(&lampNode, "Shaders.VolumetricFog.Interiors", "LampHazeEnabled") && lampNode.BoolValue;
		lampHaze = (exterior || !lampOn) ? 0.0f : lampF("LampHaze", 0.02f, 0.0f, 4.0f);
		lampHazeAnisotropy = lampF("LampHazeAnisotropy", 0.3f, 0.0f, 0.9f);
		lampHazeCore = lampF("LampHazeCore", 0.1f, 0.05f, 1.0f);
		lampHazeMaxLamps = (int)lampF("LampHazeMaxLamps", 8.0f, 0.0f, (float)LampHazeMax);
	}

	// Own section (not Main/Interiors-switched via SettingCategory), so it gets its own settings-UI
	// tab instead of crowding Main. Harmless to read unconditionally for interiors too -- the shader
	// gates its actual effect on isExterior the same way it already does for MorningFogDip's
	// timeOfDayScale, so it has no effect there regardless of what's read here.
	Constants.Night.x = N("DensityScale");
	Constants.Night.y = N("AmountScale");
	Constants.Night.z = N("HeightFalloffScale");
	Constants.Night.w = N("MaxHeightOffset");

	Constants.NightScatter.x = N("NoiseStrengthScale");
	Constants.NightScatter.y = N("WindSpeedScale");
	Constants.NightScatter.z = N("ExtinctionScale");
	Constants.NightScatter.w = N("InscatteringScale");
	ApplyLayer();	// UNOFFICIAL 2026-11-01 HeightLayer (also every frame in UpdateConstants)

	Constants.NightTint.x = N("TintR");
	Constants.NightTint.y = N("TintG");
	Constants.NightTint.z = N("TintB");
	// Moved here from Global.y (Main-scoped) so it fades via the shared shader-side nightFactor like
	// the rest of this section, instead of needing its own interior-zeroing branch above.
	Constants.NightTint.w = N("NightAmbientStrength");
	// Packed into Global.y (its spare slot) rather than a new vector for one bool -- see the shader's
	// skyMaskFactor for how it's used.
	Constants.Global.y = TheSettingManager->GetSettingI("Shaders.VolumetricFog.Night", "DisableSkyMask") ? 1.0f : 0.0f;
}

/*
* Custom build: the settings of the fog from before the rewrite (upstream 25189928), read exactly as that version did, so
* old presets that set HeightFog*, SimpleFog*, MinimumBaseFog, SunPower, FogNight... look as they were made to look.
*/
void VolumetricFogEffect::UpdateLegacySettings(const char* SettingCategory) {
	Constants.LegacyData.x = TheSettingManager->GetSettingF(SettingCategory, "MinimumBaseFog");
	Constants.LegacyData.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogSaturation");
	Constants.LegacyData.z = TheSettingManager->GetSettingF(SettingCategory, "Amount");
	Constants.LegacyData.w = TheSettingManager->GetSettingF(SettingCategory, "HeightFogInscattering");

	Constants.LegacyLow.x = TheSettingManager->GetSettingF(SettingCategory, "FogSaturation");
	Constants.LegacyLow.y = TheSettingManager->GetSettingF(SettingCategory, "WeatherImpact");

	Constants.LegacyHigh.x = TheSettingManager->GetSettingF(SettingCategory, "HeightFogDensity");
	Constants.LegacyHigh.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogFalloff");
	Constants.LegacyHigh.z = TheSettingManager->GetSettingF(SettingCategory, "HeightFogDist");

	Constants.LegacyHeight.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogHeight");
	Constants.LegacyHeight.z = TheSettingManager->GetSettingF(SettingCategory, "SimpleFogHeight");
	Constants.LegacyHeight.w = TheShaderManager->GameState.isExterior ? 1.0f : 0.0f;

	Constants.LegacySimple.x = TheSettingManager->GetSettingF(SettingCategory, "Extinction");
	Constants.LegacySimple.y = TheSettingManager->GetSettingF(SettingCategory, "Inscattering");

	Constants.LegacyBlend.y = TheSettingManager->GetSettingF(SettingCategory, "HeightFogBlend");
	Constants.LegacyBlend.z = TheSettingManager->GetSettingF(SettingCategory, "HeightFogRolloff");
	Constants.LegacyBlend.w = TheSettingManager->GetSettingF(SettingCategory, "SimpleFogBlend");

	if (TheShaderManager->GameState.isExterior) {
		Constants.LegacyHeight.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogHeight");
		Constants.LegacyLow.z = TheSettingManager->GetSettingF(SettingCategory, "DistantFogRange");
		Constants.LegacyBlend.x = TheSettingManager->GetSettingF(SettingCategory, "DistantFogBlend");

		Constants.LegacyLow.w = TheSettingManager->GetSettingF(SettingCategory, "SunPower");
		Constants.LegacySimple.z = TheSettingManager->GetSettingF(SettingCategory, "FogNight");
		Constants.LegacySimple.w = TheSettingManager->GetSettingF(SettingCategory, "SimpleFogSkyColor");
		Constants.LegacyHigh.w = TheSettingManager->GetSettingF(SettingCategory, "HeightFogSkyColor");
	}
	else {
		// these settings don't do anything in interiors
		Constants.LegacyHeight.x = 0.0f;
		Constants.LegacyLow.z = 0.0f;
		Constants.LegacyBlend.x = 0.0f;

		Constants.LegacyLow.w = 0.0f;
		Constants.LegacySimple.z = 1.0f;
		Constants.LegacySimple.w = 0.0f;
		Constants.LegacyHigh.w = 0.0f;
	}
}

void VolumetricFogEffect::RegisterConstants(){
	// Name -> memory, in the order the constants were always registered.
	struct Binding {
		const char* name;
		D3DXVECTOR4* value;
	};
	const Binding bindings[] = {
		{ "TESR_VolumetricFogDensity", &Constants.Density },
		{ "TESR_VolumetricFogShape", &Constants.Shape },
		{ "TESR_VolumetricFogWind", &Constants.Wind },
		{ "TESR_VolumetricFogScatter", &Constants.Scatter },
		{ "TESR_VolumetricFogWeather", &Constants.Weather },
		{ "TESR_VolumetricFogAerial", &Constants.Aerial },
		{ "TESR_VolumetricFogAerialTint", &Constants.AerialTintColor },
		{ "TESR_VolumetricFogDistant", &Constants.Distant },
		{ "TESR_VolumetricFogGlobal", &Constants.Global },
		{ "TESR_VolumetricFogNight", &Constants.Night },
		{ "TESR_VolumetricFogNightScatter", &Constants.NightScatter },
		{ "TESR_VolumetricFogNightTint", &Constants.NightTint },
		{ "TESR_VolumetricFogSun", &Constants.Sun },
		{ "TESR_VolumetricFogLow", &Constants.LegacyLow },
		{ "TESR_VolumetricFogHigh", &Constants.LegacyHigh },
		{ "TESR_VolumetricFogSimple", &Constants.LegacySimple },
		{ "TESR_VolumetricFogBlend", &Constants.LegacyBlend },
		{ "TESR_VolumetricFogHeight", &Constants.LegacyHeight },
		{ "TESR_VolumetricFogData", &Constants.LegacyData },
		{ "TESR_VolumetricFogLamps", &Constants.Lamps },	// UNOFFICIAL 2026-11-03 LampHaze (SetLampHaze)
	};
	for (const Binding& binding : bindings) TheShaderManager->RegisterConstant(binding.name, binding.value);
}


/*
* UNOFFICIAL 2026-11-03 LampHaze ([Shaders.VolumetricFog.Interiors] LampHaze, LampHazeAnisotropy, LampHazeCore, LampHazeMaxLamps; the shader's
* LampHazeLight): the lamps whose light the indoor fog takes in this frame. The source is the list ShaderManager::GetNearbyLights made this
* frame, as AmbientLight's grid lamps and the path tracer take it: every shadow slot's lamp (BatchLightPosition / Color, all slots, not only
* the 12 the screen passes see) and the tracked unshadowed ones (LightPosition, LightColor from 12). Their w there is the reach x
* [Shaders.ShadowsInteriors.Main] LightRadiusMult (the cube maps' reach); the haze takes the game's own reach (Spec.r) back, so the glow
* ends where the lamp's light on the walls does. A lamp in both lists counts once; a sphere wholly behind the camera is left out; then the
* nearest spheres first (distance to the sphere, 0 inside it), at most LampHazeMaxLamps. Colour: the game's (gamma) colour linearized with
* the 2.2 power, as AmbientLight's grid, times the dimmer. Outdoors, with LampHaze 0, LampHazeMaxLamps 0, ClassicLook, LegacyFog or no lamp:
* x = 0, the shader's branch skips the term and the fog is exactly as before. Called by Render before any draw (the lists are this frame's).
* The list goes into lampTexture (created on first use, D3DUSAGE_DYNAMIC as AmbientOcclusion's RT glow table); if that fails x stays 0.
*/
void VolumetricFogEffect::SetLampHaze(IDirect3DDevice9* Device) {
	Constants.Lamps = D3DXVECTOR4(0.0f, 0.0f, lampHazeAnisotropy, lampHazeCore);
	if (!(lampHaze > 0.0f) || lampHazeMaxLamps <= 0 || TheShaderManager->GameState.isExterior || IsLegacy() || IsClassic()) return;
	ShaderManager* sm = TheShaderManager;
	const float radiusMult = (std::max)(sm->Effects.ShadowsExteriors->Settings.Interiors.LightRadiusMult, 0.01f);
	const D3DXVECTOR4 camera = TheRenderManager->CameraPosition;
	const D3DXMATRIX& view = TheRenderManager->InvViewMatrix;
	const D3DXVECTOR3 forward(view._31, view._32, view._33);
	struct Lamp { D3DXVECTOR4 pos, colour; float distance; };
	Lamp lamps[ShadowCubeMapsMax + TrackedLightsMax];
	int found = 0;
	auto add = [&](const D3DXVECTOR4& p, const D3DXVECTOR4& c) {
		const float reach = p.w / radiusMult;
		if (reach <= 10.0f || c.w <= 0.0f || found >= ShadowCubeMapsMax + TrackedLightsMax) return;
		const D3DXVECTOR3 rel(p.x - camera.x, p.y - camera.y, p.z - camera.z);
		if (D3DXVec3Dot(&rel, &forward) < -reach) return;	// wholly behind the camera
		for (int i = 0; i < found; i++)	// the same lamp in both lists
			if (fabsf(lamps[i].pos.x - rel.x) < 1.0f && fabsf(lamps[i].pos.y - rel.y) < 1.0f && fabsf(lamps[i].pos.z - rel.z) < 1.0f) return;
		const float dimmer = c.w;
		const D3DXVECTOR4 colour(powf((std::max)(c.x, 0.0f), 2.2f) * dimmer, powf((std::max)(c.y, 0.0f), 2.2f) * dimmer, powf((std::max)(c.z, 0.0f), 2.2f) * dimmer, 0.0f);
		if (colour.x + colour.y + colour.z <= 0.0f) return;
		lamps[found++] = { D3DXVECTOR4(rel.x, rel.y, rel.z, reach), colour, (std::max)(D3DXVec3Length(&rel) - reach, 0.0f) };
	};
	for (int s = 0; s < ShadowCubeMapsMax; s++) add(sm->BatchLightPosition[s], sm->BatchLightColor[s]);
	for (int t = 0; t < TrackedLightsMax; t++) add(sm->LightPosition[t], sm->LightColor[ShadowCubeMapsScreen + t]);
	std::sort(lamps, lamps + found, [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
	const int count = (std::min)(found, (std::min)(lampHazeMaxLamps, (int)LampHazeMax));
	if (count <= 0) return;
	if (!lampTexture && FAILED(Device->CreateTexture(LampHazeMax, 2, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &lampTexture, nullptr))) {
		lampTexture = nullptr;
		static bool failLogged = false;
		if (!failLogged) { failLogged = true; Logger::Log("[ERROR] UNOFFICIAL LampHaze: could not create the lamp table texture: the indoor fog stays without the lamps' light."); }
		return;
	}
	D3DLOCKED_RECT rect;
	if (FAILED(lampTexture->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD))) return;
	for (int row = 0; row < 2; row++) {
		D3DXVECTOR4* texels = (D3DXVECTOR4*)((BYTE*)rect.pBits + row * rect.Pitch);
		for (int i = 0; i < LampHazeMax; i++) texels[i] = i < count ? (row ? lamps[i].colour : lamps[i].pos) : D3DXVECTOR4(0, 0, 0, 0);
	}
	lampTexture->UnlockRect(0);
	Constants.Lamps = D3DXVECTOR4(lampHaze, (float)count, lampHazeAnisotropy, lampHazeCore);
	static bool logged = false;
	if (!logged && count > 0) {
		logged = true;
		Logger::Log("UNOFFICIAL LampHaze: the indoor fog takes in the light of %d lamp(s) (LampHaze %.2f, anisotropy %.2f, core %.2f, at most %d); "
			"the fog's Vulkan / SM6 passes are not used while it does (the D3D9 technique has the lamps' light).", count, lampHaze, lampHazeAnisotropy, lampHazeCore, lampHazeMaxLamps);
	}
}

bool VolumetricFogEffect::ShouldRender()
{
	return !TheShaderManager->GameState.isUnderwater;
};

void VolumetricFogEffect::RegisterTextures() {
	const int width = MaxQuality::Reduced(TheRenderManager->width, 2);
	const int height = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("NVR_FogBuffer0", &fogTexture[0], &fogSurface[0], width, height, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_FogBuffer1", &fogTexture[1], &fogSurface[1], width, height, D3DFMT_A16B16G16R16F);
}

/*
* Half-resolution estimate of the fog coefficients (multiply + depth, add) into two dedicated
* targets in a single MRT draw, then a depth-aware full-resolution reconstruct that applies them
* to the full-resolution scene. The scene is read from RenderedSurface, which every preceding
* effect leaves equal to the render target. Returns false without touching the render target
* if the dedicated path is unavailable.
*/
/*
* Whether this frame's fog can take over the shadow/AO apply passes: the dedicated path must be
* usable, and the composite AO upsample must know the AO targets' size -- from NVR_CompositeAOTexel,
* or, in an older effect file without it, by the AO targets matching the fog targets. Checked before
* those passes are skipped, so a later failure is exceptional.
*/
bool VolumetricFogEffect::CanComposite(IDirect3DSurface9* aoSurface) {
	if (!Enabled || !Effect || !ShouldRender() || dedicatedFogFailed || !fogSurface[0] || PlainOnly() ||
		TheSettingManager->SettingsMain.Main.DisableCompositeApply ||
		!CompositeTechnique() || !ParameterHandle(paramCompositeFlags, "NVR_CompositeFlags"))
		return false;
	if (aoSurface && !ParameterHandle(paramCompositeAOTexel, "NVR_CompositeAOTexel")) {
		D3DSURFACE_DESC fog = {}, ao = {};
		if (FAILED(fogSurface[0]->GetDesc(&fog)) || FAILED(aoSurface->GetDesc(&ao)) ||
			fog.Width != ao.Width || fog.Height != ao.Height)
			return false;
	}
	return true;
}

/*
* UNOFFICIAL 2026-10-18 InteriorShadowFold: the interior composite rounds the darkened colour to 16-bit float exactly as the separate
* pass's render target stored it, and GPUs differ in how they do that (round to nearest even, or toward zero: the GTX 1070,
* evidence\intshadow\fp16test). One draw of technique HalfStoreProbe into a small A16B16G16R16F target, read back once, says which
* of the two functions this GPU's store matches for every value; if neither, the fold is not used. All state it touches is put back.
*/
int VolumetricFogEffect::HalfStoreMode(IDirect3DDevice9* Device) {
	if (halfStoreMode != -2) return halfStoreMode;
	halfStoreMode = -1;
	D3DXHANDLE technique = Effect ? Effect->GetTechniqueByName("HalfStoreProbe") : NULL;
	if (!technique || !Device) { Logger::Log("INTERIOR SHADOW FOLD: no HalfStoreProbe technique in VolumetricFog.fx: the fold stays off"); return halfStoreMode; }
	const UINT W = 64, H = 16;
	IDirect3DTexture9* texture = nullptr;
	IDirect3DSurface9* surface = nullptr;
	IDirect3DSurface9* readback = nullptr;
	IDirect3DSurface9* oldTarget0 = nullptr;
	IDirect3DSurface9* oldTarget1 = nullptr;
	IDirect3DVertexDeclaration9* oldDeclaration = nullptr;
	DWORD oldFVF = 0;
	D3DVIEWPORT9 oldViewport = {};
	unsigned nearest = 0, toward = 0, total = 0;
	HRESULT hr = Device->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &texture, NULL);
	if (SUCCEEDED(hr)) hr = texture->GetSurfaceLevel(0, &surface);
	if (SUCCEEDED(hr)) hr = Device->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &readback, NULL);
	if (SUCCEEDED(hr)) {
		Device->GetRenderTarget(0, &oldTarget0);
		Device->GetRenderTarget(1, &oldTarget1);
		Device->GetVertexDeclaration(&oldDeclaration);
		Device->GetFVF(&oldFVF);
		Device->GetViewport(&oldViewport);
		if (oldTarget1) Device->SetRenderTarget(1, nullptr);
		hr = Device->SetRenderTarget(0, surface);
		UINT passes = 0;
		if (SUCCEEDED(hr)) hr = Effect->SetTechnique(technique);
		if (SUCCEEDED(hr)) hr = Effect->Begin(&passes, 0);
		if (SUCCEEDED(hr)) {
			hr = Effect->BeginPass(0);
			if (SUCCEEDED(hr)) {
				struct Vertex { float x, y, z, w, u, v; };
				const Vertex quad[4] = { { -1, 1, 0, 1, 0, 0 }, { 1, 1, 0, 1, 1, 0 }, { -1, -1, 0, 1, 0, 1 }, { 1, -1, 0, 1, 1, 1 } };
				Device->SetFVF(D3DFVF_XYZW | D3DFVF_TEX1);
				hr = Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
				Effect->EndPass();
			}
			Effect->End();
		}
		if (SUCCEEDED(hr)) hr = Device->GetRenderTargetData(surface, readback);
		D3DLOCKED_RECT locked = {};
		if (SUCCEEDED(hr) && SUCCEEDED(hr = readback->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
			for (UINT y = 0; y < H; y++) {
				const UInt16* row = (const UInt16*)((const UInt8*)locked.pBits + (size_t)y * locked.Pitch);
				for (UINT x = 0; x < W; x++, total++) {
					if (row[x * 4] == row[x * 4 + 1]) nearest++;
					if (row[x * 4] == row[x * 4 + 2]) toward++;
				}
			}
			readback->UnlockRect();
		}
		// put back what was bound
		Device->SetRenderTarget(0, oldTarget0);
		if (oldTarget1) Device->SetRenderTarget(1, oldTarget1);
		if (oldDeclaration) Device->SetVertexDeclaration(oldDeclaration);
		else if (oldFVF) Device->SetFVF(oldFVF);
		Device->SetViewport(&oldViewport);
	}
	if (oldTarget0) oldTarget0->Release();
	if (oldTarget1) oldTarget1->Release();
	if (oldDeclaration) oldDeclaration->Release();
	if (readback) readback->Release();
	if (surface) surface->Release();
	if (texture) texture->Release();
	if (total == W * H && nearest == total && toward != total) halfStoreMode = 0;
	else if (total == W * H && toward == total && nearest != total) halfStoreMode = 2;
	Logger::Log("INTERIOR SHADOW FOLD: this GPU stores 16-bit floats %s (probe %08lx: %u of %u values as round to nearest even, %u as round toward zero)",
		halfStoreMode == 0 ? "rounding to nearest even" : halfStoreMode == 2 ? "rounding toward zero" : "in neither way: the fold stays off",
		(unsigned long)hr, nearest, total, toward);
	return halfStoreMode;
}

bool VolumetricFogEffect::CanApplyShadowAO() {
	return Enabled && Effect && !TheSettingManager->SettingsMain.Main.DisableCompositeApply && !referenceTest &&	// (reference: two passes, as master)
		TechniqueHandle(techApplyShadowAO, "ApplyShadowAO") && ParameterHandle(paramCompositeFlags, "NVR_CompositeFlags") &&
		ParameterHandle(paramCompositeAOTexel, "NVR_CompositeAOTexel");
}

/*
* The exterior sun-shadow apply and the AO combine in one full-resolution pass (technique ApplyShadowAO: the composite's
* functions without the fog), into the frame chain's next image. The AO is the deferred result (AO->deferCombine).
*/
bool VolumetricFogEffect::RenderShadowAO(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, bool shadow, IDirect3DTexture9* aoTexture) {
	D3DXHANDLE technique = TechniqueHandle(techApplyShadowAO, "ApplyShadowAO");
	D3DXHANDLE flagsHandle = ParameterHandle(paramCompositeFlags, "NVR_CompositeFlags");
	D3DXHANDLE aoTexelHandle = ParameterHandle(paramCompositeAOTexel, "NVR_CompositeAOTexel");
	IDirect3DTexture9* scene = TheTextureManager->RenderedTexture;
	D3DSURFACE_DESC ao = {};
	if (!technique || !flagsHandle || !aoTexelHandle || !scene || !RenderedSurface || (!shadow && !aoTexture) ||
		(aoTexture && FAILED(aoTexture->GetLevelDesc(0, &ao))))
		return false;
	// UNOFFICIAL 2026-10-25 (NVR DXVK): as a Vulkan pass when NvrDxvkPasses is on.
	if (!ExtFailed && DxvkCompute::PassesOn() && ExtShadowAO(Device, RenderTarget, RenderedSurface, shadow, aoTexture)) return true;

	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);
	Effect->SetTechnique(technique);
	SetCT();
	IDirect3DTexture9* sky = aoTexture ? compositeSkyTexture : nullptr;	// UNOFFICIAL Skylighting v2
	D3DXVECTOR4 flags(shadow ? 1.0f : 0.0f, aoTexture ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, 0.0f);
	Effect->SetVector(flagsHandle, &flags);
	IDirect3DTexture9* sunFar = SunFarFor(shadow);	// UNOFFICIAL 2026-10-19
	if (aoTexture) {
		D3DXVECTOR4 aoTexel(1.0f / ao.Width, 1.0f / ao.Height, AmbientOcclusionEffect::SkyTakeComposite().x, AmbientOcclusionEffect::SkyTakeComposite().y);	// UNOFFICIAL 2026-10-20 (skyamb) zw: LightCap
		Effect->SetVector(aoTexelHandle, &aoTexel);
	}
	UINT passes = 0;
	HRESULT result = Device->SetRenderTarget(0, chained ? chain.Output() : RenderTarget);
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		result = Effect->BeginPass(0);
		if (SUCCEEDED(result)) {
			Device->SetTexture(0, scene); // TESR_SourceBuffer slot
			Device->SetTexture(6, nullptr);
			Device->SetTexture(7, nullptr);
			Device->SetTexture(8, aoTexture);
			BindSky(Device, sky);
			if (sunFar) BindSunFar(Device, sunFar);
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Effect->EndPass();
		}
		Effect->End();
	}
	Device->SetTexture(8, nullptr);
	if (sky) BindSky(Device, nullptr);
	if (sunFar) BindSunFar(Device, nullptr);
	Device->SetTexture(0, TheTextureManager->SourceTexture);
	Device->SetRenderTarget(0, RenderTarget);
	if (FAILED(result)) {
		// Chained: nothing was committed, the scene is as it was. Otherwise put the input back.
		if (!chained) Device->StretchRect(RenderedSurface, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		return false;
	}
	if (chained) chain.Commit();
	else Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL merged apply active: sun shadows %s and AO %s in one pass (effects between them and the fog).",
			shadow ? "yes" : "no", aoTexture ? "yes" : "no");
		reported = true;
	}
	return true;
}

bool VolumetricFogEffect::RenderDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface) {
	if (dedicatedFogFailed) return false;
	// UNOFFICIAL ReShadeDepthCompat: this two-target pass must unbind the game's depth-stencil (a GTX 1070 drops it
	// otherwise), so the plain full-resolution fog runs instead, as in the original NVR.
	if (ReShadeIntegration::KeepDepthBound()) return false;
	const bool composite = compositeShadow || compositeInteriorShadow || (compositeAO && compositeAOTexture);
	D3DXHANDLE technique = composite ? (compositeInteriorShadow ? CompositeInteriorTechnique() : CompositeTechnique()) : DedicatedTechnique();
	D3DXHANDLE flagsHandle = composite ? ParameterHandle(paramCompositeFlags, "NVR_CompositeFlags") : NULL;
	if (composite && !flagsHandle) return false;
	D3DXHANDLE layoutHandle = ParameterHandle(paramFogLayout, "NVR_FogLayout");
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {}, fog = {}, fogAdd = {};
	const D3DCAPS9* caps = TheRenderManager->DeviceCaps();	// UNOFFICIAL: fetched once, not per frame
	IDirect3DTexture9* scene = sceneOverride ? sceneOverride : TheTextureManager->RenderedTexture;	// (override: InteriorShadowFoldCheck)
	if (!technique || !layoutHandle || FAILED(Effect->GetTechniqueDesc(technique, &description)) ||
		description.Passes != 2 || !RenderedSurface || !scene ||
		!fogTexture[0] || !fogTexture[1] || !fogSurface[0] || !fogSurface[1] ||
		!caps || caps->NumSimultaneousRTs < 2 ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || FAILED(fogSurface[0]->GetDesc(&fog)) ||
		FAILED(fogSurface[1]->GetDesc(&fogAdd)) ||
		original.X || original.Y ||
		((original.Width != target.Width || original.Height != target.Height) && !RenderRect::Active()) ||
		scratch.Width != target.Width || scratch.Height != target.Height ||
		fog.Width != MaxQuality::Reduced(target.Width, 2) || fog.Height != MaxQuality::Reduced(target.Height, 2) ||
		fogAdd.Width != fog.Width || fogAdd.Height != fog.Height || fogAdd.Format != fog.Format ||
		target.MultiSampleType != D3DMULTISAMPLE_NONE)
		return false;
	// UNOFFICIAL 2026-10-27 RenderScale v2: an earlier pass may have left the rect viewport; the passes below start from the whole
	// target and ApplyViewport cuts each to its drawn part.
	if (RenderRect::Active()) { original.Width = target.Width; original.Height = target.Height; }

	// UNOFFICIAL 2026-10-26 (deep/fork-sm6, NvrSm6Shaders): both passes from their SM6 build when it is there; the D3D9 code below then
	// sets everything up as always and only the two draws go to Sm6Pass (null: the D3D9 draws, or the hand-ported passes below).
	Sm6Pass* sm6[2] = { nullptr, nullptr };
	// (UNOFFICIAL 2026-11-03 LampHaze: neither the SM6 nor the Vulkan passes have the lamps' light; while it is on, the D3D9 technique draws.)
	if (!targetOverride && !sceneOverride && Sm6Pass::On() && !LampHazeOn()) {
		sm6[0] = Sm6For(technique, 0);
		sm6[1] = Sm6For(technique, 1);
		if (!sm6[0] || !sm6[1]) sm6[0] = sm6[1] = nullptr;
	}

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the two passes as Vulkan passes when NvrDxvkPasses is on (not for the diagnostic overrides).
	if (!sm6[0] && !targetOverride && !sceneOverride && !ExtFailed && DxvkCompute::PassesOn() && !LampHazeOn()) {
		const D3DXVECTOR4 extLayout((float)fog.Width / original.Width, (float)fog.Height / original.Height, 1.0f / fog.Width, 1.0f / fog.Height);
		bool extGrid = false;
		if (ExtDedicated(Device, RenderTarget, RenderedSurface, scene, composite, extLayout, extGrid)) {
			compositeApplied = composite;
			gridApplied = extGrid;
			return true;
		}
	}

	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = !targetOverride && chain.Owns(RenderTarget, RenderedSurface);
	IDirect3DTexture9* sunFar = nullptr;	// UNOFFICIAL 2026-10-19 SunFar: bound for the composite pass (below)
	IDirect3DSurface9* finalTarget = targetOverride ? targetOverride : chained ? chain.Output() : RenderTarget;
	D3DVIEWPORT9 reduced = original;
	reduced.Width = fog.Width;
	reduced.Height = fog.Height;
	D3DXVECTOR4 layout((float)reduced.Width / original.Width,
		(float)reduced.Height / original.Height, 1.0f / reduced.Width, 1.0f / reduced.Height);
	IDirect3DSurface9* depthSurface = nullptr;
	DWORD oldWriteMask1 = 0xF;
	HRESULT result = Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &oldWriteMask1);
	if (SUCCEEDED(result)) result = Device->GetDepthStencilSurface(&depthSurface);
	if (SUCCEEDED(result)) result = Device->SetDepthStencilSurface(nullptr);
	if (FAILED(result)) {
		if (depthSurface) depthSurface->Release();
		dedicatedFogFailed = true;
		return false;
	}

	Effect->SetTechnique(technique);
	SetCT();
	Effect->SetVector(layoutHandle, &layout);
	if (composite) {
		D3DXVECTOR4 flags(compositeShadow ? 1.0f : 0.0f, (compositeAO && compositeAOTexture) ? 1.0f : 0.0f,
			(compositeAO && compositeAOTexture && compositeSkyTexture) ? 1.0f : 0.0f,	// z: UNOFFICIAL Skylighting v2
			compositeInteriorShadow ? compositeRound : 0.0f);	// w: UNOFFICIAL InteriorShadowFold rounding (interior technique only)
		Effect->SetVector(flagsHandle, &flags);
		sunFar = compositeShadow && !compositeInteriorShadow ? SunFarFor(true) : SunFarFor(false);	// UNOFFICIAL 2026-10-19
		// The AO targets are half or (AOLowRes) quarter resolution; tell the upsample which.
		D3DXHANDLE aoTexelHandle = ParameterHandle(paramCompositeAOTexel, "NVR_CompositeAOTexel");
		D3DSURFACE_DESC ao = {};
		if (aoTexelHandle && compositeAO && compositeAOTexture && SUCCEEDED(compositeAOTexture->GetLevelDesc(0, &ao))) {
			D3DXVECTOR4 aoTexel(1.0f / ao.Width, 1.0f / ao.Height, AmbientOcclusionEffect::SkyTakeComposite().x, AmbientOcclusionEffect::SkyTakeComposite().y);	// UNOFFICIAL 2026-10-20 (skyamb) zw: LightCap
			Effect->SetVector(aoTexelHandle, &aoTexel);
		}
	}
	// UNOFFICIAL froxel grid: on stage 11 while this draws, with the flags that make the fog read it.
	D3DXHANDLE gridHandle = ParameterHandle(paramGridFlags, "NVR_FogGridFlags");
	const bool grid = gridTexture && gridHandle;
	IDirect3DBaseTexture9* skyCoverage = grid ? FogSkyCoverage("D3D9") : nullptr;	// UNOFFICIAL 2026-11-05 MsaaEdgeFix (stage 15)
	if (gridHandle) {
		D3DXVECTOR4 gridFlags(grid ? 1.0f : 0.0f, grid ? 1.0f : 0.0f, skyCoverage ? 1.0f : 0.0f, 0.0f);
		Effect->SetVector(gridHandle, &gridFlags);
	}
	if (grid) {
		Device->SetTexture(11, gridTexture);
		TheRenderManager->renderState->NoteDeviceTexture(11, gridTexture);
		const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
			{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
		for (const auto& s : states) TheRenderManager->SetSamplerState(11, (D3DSAMPLERSTATETYPE)s[0], s[1]);
	}
	// (the shader reads stage 15 whenever it applies the grid: nothing bound there reads 0, the fix off)
	Device->SetTexture(15, skyCoverage);
	TheRenderManager->renderState->NoteDeviceTexture(15, skyCoverage);
	if (skyCoverage) {
		const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
			{D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
		for (const auto& s : states) TheRenderManager->SetSamplerState(15, (D3DSAMPLERSTATETYPE)s[0], s[1]);
	}
	UINT passes = 0;
	result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		GpuTimer* passTimers = FogPassTimers();
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			GpuProfileScope gpuPass(passTimers[combine ? 1 : 0], Device);
			// s5 is TESR_PointShadowBuffer (bound by SetCT); the fog targets are s6/s7, AO s8.
			Device->SetTexture(6, nullptr);
			Device->SetTexture(7, nullptr);
			Device->SetTexture(8, nullptr);
			// Unbind the half-resolution MRT before binding the full-resolution target, so the
			// two differently sized targets are never bound together.
			if (combine) result = Device->SetRenderTarget(1, nullptr);
			if (SUCCEEDED(result)) result = Device->SetRenderTarget(0, combine ? finalTarget : fogSurface[0]);
			if (SUCCEEDED(result) && !combine) result = Device->SetRenderTarget(1, fogSurface[1]);
			if (SUCCEEDED(result) && !combine) result = Device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
			if (SUCCEEDED(result)) result = Device->SetViewport(combine ? &original : &reduced);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			Device->SetTexture(0, scene); // TESR_SourceBuffer slot
			if (combine) {
				Device->SetTexture(6, fogTexture[0]);
				Device->SetTexture(7, fogTexture[1]);
				if (composite && compositeAO && compositeAOTexture) Device->SetTexture(8, compositeAOTexture);
				if (composite && compositeAO && compositeAOTexture && compositeSkyTexture) BindSky(Device, compositeSkyTexture);
				if (sunFar) BindSunFar(Device, sunFar);
			}
			// UNOFFICIAL 2026-10-26: the SM6 build reads what this draw would (the effect's parameters, the stages bound above); a failed
			// SM6 draw falls back to the D3D9 draw right here.
			Sm6Pass* sm6Pass = sm6[0] ? sm6[p] : nullptr;
			if (sm6Pass && sm6Pass->Draw(Device, combine ? &finalTarget : fogSurface, combine ? 1 : 2, combine ? ExtUv::ScreenQuad : ExtUv::TexelCentre)) result = D3D_OK;
			else {
				// UNOFFICIAL 2026-10-27 RenderScale v2: the half-size MRT is screen-shaped, so both passes draw their rect part (SM6 is
				// off in rect frames).
				RenderRect::ApplyViewport(Device);
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			}
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	Device->SetTexture(6, nullptr);
	Device->SetTexture(7, nullptr);
	Device->SetTexture(8, nullptr);
	if (compositeSkyTexture) BindSky(Device, nullptr);
	if (sunFar) BindSunFar(Device, nullptr);
	if (grid) {
		Device->SetTexture(11, nullptr);
		TheRenderManager->renderState->NoteDeviceTexture(11, nullptr);
	}
	if (skyCoverage) {
		Device->SetTexture(15, nullptr);
		TheRenderManager->renderState->NoteDeviceTexture(15, nullptr);
	}
	if (gridHandle) {
		// The other paths must not read a grid that is not bound.
		D3DXVECTOR4 off(0.0f, 0.0f, 0.0f, 0.0f);
		Effect->SetVector(gridHandle, &off);
	}
	Device->SetTexture(0, TheTextureManager->SourceTexture);
	Device->SetRenderTarget(1, nullptr);
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE1, oldWriteMask1);
	Device->SetViewport(&original);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	if (targetOverride) {	// DIAGNOSTIC InteriorShadowFoldCheck: nothing to commit or copy, nothing to undo
		compositeApplied = SUCCEEDED(result) && composite;
		return SUCCEEDED(result);
	}
	if (SUCCEEDED(result)) {
		if (chained) chain.Commit();
		else result = Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	if (FAILED(result)) {
		// RenderedSurface is only written by the final copy, so it still holds the input.
		Device->StretchRect(RenderedSurface, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		dedicatedFogFailed = true;
		Logger::Log("Dedicated volumetric fog failed (%08lx); using packed path until restart.", result);
		return false;
	}
	compositeApplied = composite;
	gridApplied = grid;
	static bool reportedComposite = false;
	if (composite && !reportedComposite) {
		Logger::Log("UNOFFICIAL composite apply active: sun shadows %s, AO %s folded into the fog pass.",
			compositeShadow ? "yes" : compositeInteriorShadow ? "no (the interior darkening instead)" : "no", (compositeAO && compositeAOTexture) ? "yes" : "no");
		reportedComposite = true;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL dedicated volumetric fog active: %ux%u -> %ux%u, full-resolution apply (MRT).",
			original.Width, original.Height, reduced.Width, reduced.Height);
		reported = true;
	}
	return true;
}

// UNOFFICIAL 2026-10-26 (deep/fork-sm6): see VolumetricFog.h.
Sm6Pass* VolumetricFogEffect::Sm6For(D3DXHANDLE technique, UINT pass) {
	D3DXTECHNIQUE_DESC description = {};
	if (!technique || !Effect || FAILED(Effect->GetTechniqueDesc(technique, &description)) || !description.Name) return nullptr;
	Sm6Pass& sm6 = sm6Passes[std::string(description.Name) + "." + std::to_string(pass)];
	return sm6.Prepare(this, description.Name, pass) ? &sm6 : nullptr;
}

/*
* Custom build: the full-resolution fallback technique. With LegacyFog on, the index of VolumetricFogLegacy (looked up by
* name), otherwise the caller's.
*/
UINT VolumetricFogEffect::PlainTechniqueIndex(UINT techniqueIndex) {
	// UNOFFICIAL 2026-10-18: FogReference (diagnostic) and VolumetricFogClassic (ClassicLook) the same way.
	const char* name = referenceTest ? "FogReference" : IsLegacy() ? "VolumetricFogLegacy" : IsClassic() ? "VolumetricFogClassic" : nullptr;
	if (!name || !Effect) return techniqueIndex;
	D3DXEFFECT_DESC effect = {};
	if (FAILED(Effect->GetDesc(&effect))) return techniqueIndex;
	for (UINT t = 0; t < effect.Techniques; t++) {
		D3DXTECHNIQUE_DESC description = {};
		D3DXHANDLE technique = Effect->GetTechnique(t);
		if (technique && SUCCEEDED(Effect->GetTechniqueDesc(technique, &description)) && description.Name &&
			!strcmp(description.Name, name))
			return t;
	}
	return techniqueIndex;
}

void VolumetricFogEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
	IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { NoteSkip(); renderTime = 0; return; }
	// UNOFFICIAL 2026-11-03 LampHaze: this frame's lamps, before any draw sets the constants; their table on stage 14 (NVR_FogLamps, point,
	// clamp) while the fog draws, taken off again when this returns (bound only while the term is on: only then does the shader read it).
	SetLampHaze(Device);
	struct LampBinding {
		IDirect3DDevice9* device; bool bound;
		LampBinding(IDirect3DDevice9* d, IDirect3DTexture9* t) : device(d), bound(t != nullptr) {
			if (!bound) return;
			device->SetTexture(14, t);
			TheRenderManager->renderState->NoteDeviceTexture(14, t);
			const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
				{D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
			for (const auto& s : states) TheRenderManager->SetSamplerState(14, (D3DSAMPLERSTATETYPE)s[0], s[1]);
		}
		~LampBinding() { if (bound) { device->SetTexture(14, nullptr); TheRenderManager->renderState->NoteDeviceTexture(14, nullptr); } }
	} lampBinding(Device, Constants.Lamps.x > 0.0f ? lampTexture : nullptr);

	// UNOFFICIAL: the fog's density noise, a 3D texture on stage 9 (NVR_FogNoise in the effect), for every path below;
	// taken off again when this returns, so nothing drawn later finds it there.
	static IDirect3DBaseTexture9* noise = nullptr;
	static bool noiseLoaded = false;
	if (!noiseLoaded) {
		noiseLoaded = true;
		noise = TheTextureManager->GetFileTexture("Data\\Textures\\Effects\\NoiseVolume.dds", TextureRecord::TextureRecordType::VolumeBuffer);
		if (!noise) Logger::Log("[ERROR] VolumetricFog: Textures\\Effects\\NoiseVolume.dds is missing: the fog's patches are off.");
	}
	noiseTexture = noise;	// (the Vulkan passes')
	struct NoiseBinding {
		IDirect3DDevice9* device;
		NoiseBinding(IDirect3DDevice9* d, IDirect3DBaseTexture9* t) : device(d) {
			device->SetTexture(9, t);
			TheRenderManager->renderState->NoteDeviceTexture(9, t);
			const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP},
				{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR} };
			for (const auto& s : states) TheRenderManager->SetSamplerState(9, (D3DSAMPLERSTATETYPE)s[0], s[1]);
		}
		~NoiseBinding() { device->SetTexture(9, nullptr); TheRenderManager->renderState->NoteDeviceTexture(9, nullptr); }
	} noiseBinding(Device, noise);

	// UNOFFICIAL physical sky (VolumetricFog.fx.hlsl PhysicalFogSky): the sky's table on stage 10 (NVR_FogSkyView) while it is
	// drawn this frame; taken off again when this returns.
	struct SkyViewBinding {
		IDirect3DDevice9* device; bool bound;
		SkyViewBinding(IDirect3DDevice9* d) : device(d), bound(false) {
			SkyViewEffect* table = TheShaderManager->Effects.SkyView;
			if (!table || !table->Textures.TableTexture || !table->ShouldRender()) return;
			bound = true;
			device->SetTexture(10, table->Textures.TableTexture);
			TheRenderManager->renderState->NoteDeviceTexture(10, table->Textures.TableTexture);
			const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
				{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
			for (const auto& s : states) TheRenderManager->SetSamplerState(10, (D3DSAMPLERSTATETYPE)s[0], s[1]);
		}
		~SkyViewBinding() { if (bound) { device->SetTexture(10, nullptr); TheRenderManager->renderState->NoteDeviceTexture(10, nullptr); } }
	} skyViewBinding(Device);

	// UNOFFICIAL 2026-10-18: ClassicLook 2 and the FogReference diagnostic draw every pixel in one full-resolution pass, as master
	// did (no composite was set up for them: CanComposite and CanCompositeInterior say no).
	if (PlainOnly() && !(compositeShadow || compositeAO || compositeInteriorShadow)) {
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, PlainTechniqueIndex(techniqueIndex), ClearRenderTarget, SourceBuffer);
		return;
	}

	{
		auto timer = TimeLogger();
		if (RenderDedicated(Device, RenderTarget, RenderedSurface)) {
			renderTime = timer.LogTime("VolumetricFog::DedicatedRender");
			return;
		}
	}

	// The caller must apply deferred shadows/AO before retrying ordinary fog.
	if (compositeShadow || compositeAO || compositeInteriorShadow) return;

	D3DXHANDLE technique = PackedTechnique();
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {};
	if (packedFogFailed || !technique || !RenderedSurface || !SourceBuffer ||
		FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes != 2 ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || original.X || original.Y ||
		original.Width != target.Width || original.Height != target.Height ||
		target.Width != scratch.Width || target.Height != scratch.Height ||
		(target.Width & 1) || (target.Height & 1) ||
		RenderRect::Active()) {	// UNOFFICIAL 2026-10-27 RenderScale v2: the packed layout assumes the whole screen; the plain pass instead
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, PlainTechniqueIndex(techniqueIndex), ClearRenderTarget, SourceBuffer);
		return;
	}

	auto timer = TimeLogger();
	TheShaderManager->Chain.Sync(); // the packed path reads the render target itself
	D3DVIEWPORT9 reduced = original;
	reduced.Width /= 2;
	reduced.Height /= 2;
	RECT packed = {0, 0, (LONG)reduced.Width, (LONG)reduced.Height};
	HRESULT result = Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_NONE);
	if (SUCCEEDED(result)) result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) SetCT();
	UINT passes = 0;
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		for (UINT p = 0; p < passes && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			result = Device->SetViewport(combine ? &original : &reduced);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
			if (SUCCEEDED(result)) result = Device->StretchRect(RenderTarget, combine ? NULL : &packed,
				RenderedSurface, combine ? NULL : &packed, D3DTEXF_NONE);
		}
		Effect->End();
	}
	Device->SetViewport(&original);

	if (FAILED(result)) {
		Device->StretchRect(SourceBuffer, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		Device->StretchRect(SourceBuffer, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
		packedFogFailed = true;
		Logger::Log("Packed volumetric fog failed (%08lx); using legacy path until restart.", result);
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, PlainTechniqueIndex(techniqueIndex), ClearRenderTarget, SourceBuffer);
		return;
	}
	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL packed volumetric fog active: %ux%u -> %ux%u.",
			original.Width, original.Height, reduced.Width, reduced.Height);
		reported = true;
	}
	renderTime = timer.LogTime("VolumetricFog::PackedRender");
}

// ---- UNOFFICIAL 2026-10-25 (NVR DXVK): the fog as Vulkan passes (see VolumetricFog.h) ----
// The constants are read from the same registered memory the D3D9 path uploads (ConstantsTable; SetCT sets the matrices with
// SetMatrix, the logical matrix, copied here as stored), in the order of the GLSL's uniform block (Compute/VolumetricFog.glsl).
namespace {
	const char* const FogExtMatrixNames[] = { "TESR_ShadowCameraToLightTransformNear", "TESR_ShadowCameraToLightTransformMiddle",
		"TESR_ShadowCameraToLightTransformFar", "TESR_ShadowCameraToLightTransformLod", "TESR_ViewTransform", "TESR_ProjectionTransform" };
	// UNOFFICIAL 2026-10-30: the fog colour is the fog's own copy with the sky look in it (Sky.cpp UpdateFogSky; the GLSL's fogColor)
	const char* const FogExtVectorNames[] = { "TESR_FogWeatherColor", "TESR_FogData", "TESR_ReciprocalResolution", "TESR_GameTime", "TESR_SmoothedSunDir",
		"TESR_SunPosition", "TESR_SunColor", "TESR_SunDiskColor", "TESR_SunAmbient", "TESR_FogHorizonColor", "TESR_FogSkyLowColor", "TESR_FogSkyColor",
		"TESR_SkyData", "TESR_SunsetColor", "TESR_SunAmount", "TESR_PBRData",
		"TESR_ShadowNearCenter", "TESR_ShadowMiddleCenter", "TESR_ShadowFarCenter", "TESR_ShadowLodCenter", "TESR_ShadowFormatData", "TESR_ShadowBlur",
		"TESR_ShadowFade",
		"TESR_VolumetricFogDensity", "TESR_VolumetricFogShape", "TESR_VolumetricFogWind", "TESR_VolumetricFogScatter", "TESR_VolumetricFogWeather",
		"TESR_VolumetricFogAerial", "TESR_VolumetricFogAerialTint", "TESR_VolumetricFogDistant", "TESR_VolumetricFogGlobal", "TESR_VolumetricFogNight",
		"TESR_VolumetricFogNightScatter", "TESR_VolumetricFogSun", "TESR_VolumetricFogNightTint",
		"TESR_PhysSky", "TESR_PhysSkySun",
		"TESR_VolumetricLightData", "TESR_VolumetricLightData2", "TESR_VolumetricLightGridLayout", "TESR_VolumetricLightGridData",
		"TESR_VolumetricFogLow", "TESR_VolumetricFogHigh", "TESR_VolumetricFogSimple", "TESR_VolumetricFogBlend", "TESR_VolumetricFogHeight",
		"TESR_VolumetricFogData",
		"TESR_ShadowData", "TESR_WaterSettings", "TESR_AmbientOcclusionAOData", "TESR_AmbientOcclusionData",
		"TESR_CameraData", "TESR_CameraPosition" };
	struct FogExtConstants {
		D3DXMATRIX	Matrices[_countof(FogExtMatrixNames)];
		D3DXVECTOR4	Vectors[_countof(FogExtVectorNames)];
		D3DXVECTOR4	SkyIrradiance[9];		// TESR_SkyIrradiance
		D3DXVECTOR4	SkyIrradianceGame[9];	// TESR_SkyIrradianceGame
		// UNOFFICIAL 2026-10-26: these colours linearized once a draw (what the D3D9 effect's preshader takes off the pixels; the GLSL
		// linearized them on every pixel: most of the Vulkan fog estimate's extra 0.04 ms, desert). Order: FogExtLinearNames.
		D3DXVECTOR4	Linear[9];
	};
	// UNOFFICIAL 2026-10-27: the sky colours are the fog's own copies with the sky look in them (Sky.cpp UpdateFogSky; the GLSL is unchanged)
	const char* const FogExtLinearNames[] = { "TESR_FogWeatherColor", "TESR_FogSkyColor", "TESR_FogSkyLowColor", "TESR_FogHorizonColor", "TESR_SunDiskColor",
		"TESR_SunsetColor", "TESR_SunAmbient", "TESR_VolumetricFogAerialTint", "TESR_SunColor" };

	// Common.glsl linearize (rgb; alpha as it is).
	float FogLinear(float c) { return c <= 0.04045f ? c / 12.92f : powf(fabsf((c + 0.055f) / 1.055f), 2.4f); }

	bool FogExtCopy(void* to, size_t bytes, const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		if (found == TheShaderManager->ConstantsTable.end() || !found->second) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				Logger::Log("UNOFFICIAL Vegas Shaders DXVK: VolumetricFog: %s is not registered; the D3D9 techniques are used", name);
			}
			return false;
		}
		memcpy(to, found->second, bytes);
		return true;
	}

	// Set 1 binding i = slot i (the textures; NVR_FogNoise, binding 5, is 3D), the uniform block (set 2, binding 0), then the three
	// samplers (heap indices at push offsets 0, 4, 8: POINT/CLAMP, LINEAR/CLAMP, LINEAR/WRAP).
	UINT FogExtBindings(NVR_EXT_BINDING_DESC* bindings, UINT images) {
		for (UINT i = 0; i < images; i++) {
			bindings[i] = {};
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i;
			bindings[i].ViewType = i == 5 ? NVR_EXT_VIEW_3D : NVR_EXT_VIEW_2D;
		}
		bindings[images] = { 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
		for (UINT s = 0; s < 3; s++) {
			bindings[images + 1 + s] = {};
			bindings[images + 1 + s].Type = NVR_EXT_BIND_SAMPLER; bindings[images + 1 + s].Slot = s; bindings[images + 1 + s].PushOffset = s * 4;
		}
		return images + 4;
	}

	UINT FogExtResources(NVR_EXT_RESOURCE_BINDING* resources, IDirect3DBaseTexture9* const* textures, UINT images, ID3D9NvrExtBuffer* uniforms) {
		ID3D9NvrExtSampler* samplers[3] = { DxvkCompute::PointClamp(), DxvkCompute::LinearClamp(), DxvkCompute::LinearWrap() };
		for (UINT i = 0; i < images; i++) {
			resources[i] = {};
			resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
		}
		resources[images] = {};
		resources[images].Slot = 0; resources[images].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[images].pResource = uniforms;
		for (UINT s = 0; s < 3; s++) {
			if (!samplers[s]) return 0;
			resources[images + 1 + s] = {};
			resources[images + 1 + s].Slot = s; resources[images + 1 + s].Type = NVR_EXT_BIND_SAMPLER; resources[images + 1 + s].pResource = samplers[s];
		}
		return images + 4;
	}
}

// A TESR_ texture as SetCT binds it to the D3D9 pass (its TextureManager slot); null if none.
IDirect3DBaseTexture9* VolumetricFogEffect::ExtTextureOf(const char* name) {
	for (UInt32 c = 0; c < TextureShaderValuesCount; c++) {
		ShaderTextureValue* sampler = &TextureShaderValues[c];
		if (!sampler->Name || strcmp(sampler->Name, name) || !sampler->Texture) continue;
		if (sampler->Texture->TextureRef) sampler->Texture->Texture = *sampler->Texture->TextureRef;
		if (!sampler->Texture->Texture) sampler->Texture->BindTexture(sampler->Name);
		return sampler->Texture->Texture;
	}
	return nullptr;
}

/*
* What every fog pass shares: this frame's constants into the uniform block, the inputs as the D3D9 passes find them (inputs[0],
* the scene, is the caller's), and the push block's fixed part. The composites' own inputs (fog buffers, AO, sky factor, SunFar)
* start as stand-ins (the depth buffer), never read while their flags are 0, as the D3D9 shader then never reads their stages.
* False when an input the shader may read is missing (the D3D9 technique then reads a stage with nothing bound).
*/
bool VolumetricFogEffect::ExtSetup(IDirect3DBaseTexture9* (&inputs)[ExtInputCount], ExtFogPush& push) {
	IDirect3DBaseTexture9* depth = ExtTextureOf("TESR_DepthBuffer");
	IDirect3DBaseTexture9* atlas = ExtTextureOf("TESR_ShadowAtlas");
	IDirect3DBaseTexture9* normals = ExtTextureOf("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* pointShadow = ExtTextureOf("TESR_PointShadowBuffer");
	if (!depth || !atlas || !normals || !pointShadow || !noiseTexture) return false;
	// The physical sky's table (Render's SkyViewBinding): read where the effect was compiled with it, outdoors, with TESR_PhysSky.x > 0.
	const bool physical = ParameterHandle(paramPhysSky, "TESR_PhysSky") != NULL;
	SkyViewEffect* table = TheShaderManager->Effects.SkyView;
	IDirect3DBaseTexture9* skyView = table && table->Textures.TableTexture && table->ShouldRender() ? table->Textures.TableTexture : nullptr;

	FogExtConstants constants;
	bool registered = true;
	for (UINT i = 0; i < _countof(FogExtMatrixNames); i++) registered = FogExtCopy(&constants.Matrices[i], sizeof(D3DXMATRIX), FogExtMatrixNames[i]) && registered;
	for (UINT i = 0; i < _countof(FogExtVectorNames); i++) registered = FogExtCopy(&constants.Vectors[i], sizeof(D3DXVECTOR4), FogExtVectorNames[i]) && registered;
	registered = FogExtCopy(constants.SkyIrradiance, sizeof(constants.SkyIrradiance), "TESR_SkyIrradiance") && registered;
	registered = FogExtCopy(constants.SkyIrradianceGame, sizeof(constants.SkyIrradianceGame), "TESR_SkyIrradianceGame") && registered;
	for (UINT i = 0; i < _countof(FogExtLinearNames); i++) {
		D3DXVECTOR4& c = constants.Linear[i];
		registered = FogExtCopy(&c, sizeof(D3DXVECTOR4), FogExtLinearNames[i]) && registered;
		c = D3DXVECTOR4(FogLinear(c.x), FogLinear(c.y), FogLinear(c.z), c.w);
	}
	if (!registered) return false;
	const D3DXVECTOR4& physSky = constants.Vectors[36];	// TESR_PhysSky
	if (physical && !skyView && physSky.x > 0.0f && TheShaderManager->GameState.isExterior) return false;
	ID3D9NvrExtBuffer* uniforms = ExtUniforms(extUniforms, &constants, sizeof(constants));
	if (!uniforms) return false;

	inputs[0] = nullptr;
	inputs[1] = depth; inputs[2] = atlas; inputs[3] = normals; inputs[4] = pointShadow; inputs[5] = noiseTexture;
	inputs[6] = skyView ? skyView : depth;
	for (UINT i = 7; i < ExtInputCount; i++) inputs[i] = depth;	// the grid, the fog buffers, AO, sky factor, SunFar
	push = {};
	push.physicalSky = physical ? 1 : 0;
	return true;
}

// One full-resolution pass (VolumetricFogApply.frag, push.mode) through the frame chain (ExtFullScreen).
bool VolumetricFogEffect::ExtApplyPass(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
	IDirect3DBaseTexture9* const (&inputs)[ExtInputCount], const ExtFogPush& push, bool last) {
	// UNOFFICIAL 2026-10-26: the variant of this pass (the D3D9 pass's compile-time switches; Compute\variants.txt), else the general shader.
	char suffix[32];
	_snprintf_s(suffix, sizeof(suffix), _TRUNCATE, "m%ul%uc%up%u", push.mode, push.legacy, push.classic, push.physicalSky);
	ID3D9NvrExtPipeline* pipeline = ExtVariant(extVariants, extApply, "VolumetricFogApply", suffix, [](const char* name) {
		NVR_EXT_BINDING_DESC bindings[ExtInputCount + 4];
		const UINT count = FogExtBindings(bindings, ExtInputCount);
		return DxvkCompute::CreateGraphicsPipeline(name, bindings, count, sizeof(ExtFogPush), true);
	});
	if (!pipeline || !extUniforms.Buffer) return false;
	for (UINT i = 0; i < ExtInputCount; i++) if (!inputs[i]) return false;
	NVR_EXT_RESOURCE_BINDING resources[ExtInputCount + 4];
	const UINT count = FogExtResources(resources, inputs, ExtInputCount, extUniforms.Buffer);
	if (!count) return false;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, pipeline, resources, count, &push, sizeof(push), last);
}

/*
* RenderDedicated's two passes (after its own checks): DedicatedFogEstimate into the two half-resolution fog targets at once, then
* DedicatedFogCombine, DedicatedFogComposite or DedicatedFogCompositeInterior into the chain's next image (Commit; never the direct
* final write, as the D3D9 path), with the same flags and inputs the D3D9 path sets. grid: whether the froxel grid was read.
*/
bool VolumetricFogEffect::ExtDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DTexture9* scene,
	bool composite, const D3DXVECTOR4& layout, bool& grid) {
	IDirect3DBaseTexture9* inputs[ExtInputCount];
	ExtFogPush push;
	if (!ExtSetup(inputs, push)) return false;
	inputs[0] = scene;

	push.legacy = IsLegacy() ? 1 : 0;
	push.classic = IsClassic() ? 1 : 0;
	push.interior = composite && compositeInteriorShadow ? 1 : 0;
	push.mode = !composite ? 0 : compositeInteriorShadow ? 2 : 1;
	push.fogLayout = layout;
	const bool ao = composite && compositeAO && compositeAOTexture;
	if (composite) {
		push.compositeFlags = D3DXVECTOR4(compositeShadow ? 1.0f : 0.0f, ao ? 1.0f : 0.0f, (ao && compositeSkyTexture) ? 1.0f : 0.0f,
			compositeInteriorShadow ? compositeRound : 0.0f);
		IDirect3DTexture9* sunFar = compositeShadow && !compositeInteriorShadow ? SunFarFor(true) : SunFarFor(false);
		push.sunFarFlags = D3DXVECTOR4(sunFar ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
		if (sunFar) inputs[12] = sunFar;
		D3DSURFACE_DESC aoDesc = {};
		if (ao && SUCCEEDED(compositeAOTexture->GetLevelDesc(0, &aoDesc)))
			push.compositeAOTexel = D3DXVECTOR4(1.0f / aoDesc.Width, 1.0f / aoDesc.Height, AmbientOcclusionEffect::SkyTakeComposite().x, AmbientOcclusionEffect::SkyTakeComposite().y);
		if (ao) inputs[10] = compositeAOTexture;
		if (ao && compositeSkyTexture) inputs[11] = compositeSkyTexture;
	}
	grid = gridTexture && ParameterHandle(paramGridFlags, "NVR_FogGridFlags");
	IDirect3DBaseTexture9* skyCoverage = grid ? FogSkyCoverage("Vulkan") : nullptr;	// UNOFFICIAL 2026-11-05 MsaaEdgeFix (binding 13)
	push.gridFlags = grid ? D3DXVECTOR4(1.0f, 1.0f, skyCoverage ? 1.0f : 0.0f, 0.0f) : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	if (grid) inputs[7] = gridTexture;
	if (skyCoverage) inputs[13] = skyCoverage;

	char suffix[32];
	_snprintf_s(suffix, sizeof(suffix), _TRUNCATE, "l%uc%ui%up%u", push.legacy, push.classic, push.interior, push.physicalSky);
	ID3D9NvrExtPipeline* estimate = ExtVariant(extVariants, extEstimate, "VolumetricFogEstimate", suffix, [](const char* name) {
		NVR_EXT_BINDING_DESC bindings[8 + 4];
		const UINT count = FogExtBindings(bindings, 8);
		return DxvkCompute::CreateGraphicsPipeline(name, bindings, count, sizeof(ExtFogPush), true, 2);
	});
	if (!estimate) return false;
	NVR_EXT_RESOURCE_BINDING resources[8 + 4];
	const UINT count = FogExtResources(resources, inputs, 8, extUniforms.Buffer);
	if (!count) return false;
	GpuTimer* passTimers = FogPassTimers();
	{
		GpuProfileScope gpuPass(passTimers[0], Device);
		if (!ExtDrawTo(estimate, fogSurface, 2, resources, count, &push, sizeof(push))) return false;
	}
	inputs[8] = fogTexture[0];
	inputs[9] = fogTexture[1];
	GpuProfileScope gpuPass(passTimers[1], Device);
	return ExtApplyPass(Device, RenderTarget, RenderedSurface, nullptr, inputs, push, false);
}

// RenderShadowAO (technique ApplyShadowAO, after its own checks) into the chain's next image, with the D3D9 path's flags and inputs.
bool VolumetricFogEffect::ExtShadowAO(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, bool shadow, IDirect3DTexture9* aoTexture) {
	IDirect3DBaseTexture9* inputs[ExtInputCount];
	ExtFogPush push;
	if (!ExtSetup(inputs, push)) return false;
	inputs[0] = TheTextureManager->RenderedTexture;
	IDirect3DTexture9* sky = aoTexture ? compositeSkyTexture : nullptr;
	push.mode = 3;
	push.compositeFlags = D3DXVECTOR4(shadow ? 1.0f : 0.0f, aoTexture ? 1.0f : 0.0f, sky ? 1.0f : 0.0f, 0.0f);
	IDirect3DTexture9* sunFar = SunFarFor(shadow);
	push.sunFarFlags = D3DXVECTOR4(sunFar ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	if (sunFar) inputs[12] = sunFar;
	D3DSURFACE_DESC aoDesc = {};
	if (aoTexture && SUCCEEDED(aoTexture->GetLevelDesc(0, &aoDesc)))
		push.compositeAOTexel = D3DXVECTOR4(1.0f / aoDesc.Width, 1.0f / aoDesc.Height, AmbientOcclusionEffect::SkyTakeComposite().x, AmbientOcclusionEffect::SkyTakeComposite().y);
	if (aoTexture) inputs[10] = aoTexture;
	if (sky) inputs[11] = sky;
	return ExtApplyPass(Device, RenderTarget, RenderedSurface, nullptr, inputs, push, false);
}

/*
* The full-resolution techniques EffectRecord::Render draws (ClassicLook 2, and the fallback when neither the dedicated nor the
* packed path can run): VolumetricFog, VolumetricFogLegacy and VolumetricFogClassic, read by name. The rest stay D3D9.
*/
bool VolumetricFogEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (ClearRenderTarget || (SourceBuffer && SourceBuffer == RenderTarget)) return false;
	D3DXTECHNIQUE_DESC description = {};
	D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
	if (!technique || FAILED(Effect->GetTechniqueDesc(technique, &description)) || description.Passes != 1) return false;
	const char* name = description.Name ? description.Name : "";
	const bool legacy = !strcmp(name, "VolumetricFogLegacy"), classic = !strcmp(name, "VolumetricFogClassic");
	if (*name && !legacy && !classic) return false;	// (the first technique has no name)
	if (!legacy && !classic && LampHazeOn()) return false;	// (UNOFFICIAL 2026-11-03 LampHaze: the Vulkan pass has no lamps' light; D3D9 draws)
	IDirect3DBaseTexture9* inputs[ExtInputCount];
	ExtFogPush push;
	if (!ExtSetup(inputs, push)) return false;
	inputs[0] = TheTextureManager->SourceTexture;	// TESR_SourceBuffer (ExtFullScreen refreshes it from the frame first, as Render does)
	push.mode = 4;
	push.legacy = legacy ? 1 : 0;
	push.classic = classic ? 1 : 0;
	return ExtApplyPass(Device, RenderTarget, RenderedSurface, SourceBuffer, inputs, push, true);
}
