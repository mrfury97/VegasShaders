#pragma once
#define CommandPrefix "NVR"
#include "../core/Paths.h"	// 2026-11-02 Vegas Shaders: every runtime file and folder name, with the old names as fallback
#define SettingsFile "\\Data\\NVSE\\Plugins\\NewVegasReloaded.dll.config"	// the legacy file keeps its name (only read; Paths::LegacyConfigFile)
#define ShadersPath Paths::ShadersDir()	// Data\Shaders\VegasShaders\Shaders\ (or the old NewVegasReloaded folder, Paths.h)
#define EffectsPath Paths::EffectsDir()
#define RenderStateArgs 0, 0
#define TerrainShadersNames "SLS2002.vso SLS2003.pso SLS2006.pso SLS2080.vso SLS2082.pso SLS2100.vso SLS2092.pso SLS2096.pso SLS2100.pso SLS2104.pso SLS2108.pso SLS2112.pso SLS2116.pso SLS2120.pso SLS2124.pso SLS2128.pso SLS2132.pso SLS2136.pso SLS2140.pso SLS2144.pso"

#define SamplerStatesMax 12
// UNOFFICIAL (2026-10-08): cube-map slots. The screen-space passes know ShadowCubeMapsScreen of them (PointShadows.fx:
// TESR_ShadowLightPosition[12], TESR_LightColor[24] = 12 shadow lamps then 12 tracked ones); interior forward shadows
// ([Shaders.ShadowsInteriors.Forward]) bind each object's own lamps and can use up to ShadowCubeMapsMax
// ([Shaders.ShadowsInteriors.Main] LightPoints). Slots past ShadowCubeMapsScreen get their cube maps when first used.
#define ShadowCubeMapsScreen 12
#define ShadowCubeMapsMax 64	// UNOFFICIAL (2026-10-08): 32 -> 64, busy interiors (TOPS: 50-70 lamps) ran out of slots
// PointShadows.fx / PointShadows2.fx sample the cubemaps of slots 0..10 only; the last slot's light is lit without a
// shadow lookup, so its cubemap is never read and is not drawn.
#define ShadowCubeMapsSampled (ShadowCubeMapsScreen - 1)
#define ExteriorBatchedMax 44	// UNOFFICIAL ExteriorLightPoints: outdoors at most 4 batches of 11 shadowed lamps
#define TrackedLightsMax 12
// UNOFFICIAL (2026-10-08): lamps the darkening mask (PointShadows.fx, TESR_ExtraLights) knows past its 12 shadow and 12
// tracked ones: the forward shadow slots past 24 and the tracked lamps they push off, nearest first (GetNearbyLights). 16,
// one register each: the effect's pixel shader has room for about 20 more constants (its shadow sampling's own constants
// fill the rest of the 224; tests/effect_compile checks it with the game's compiler).
#define ExtraLightsMax 16
#define SpotLightsMax 1

#define WordWaterHeightMapBuffer "TESR_WaterHeightMapBuffer"
#define WordWaterReflectionMapBuffer "TESR_WaterReflectionMapBuffer"

// NVR's replacements for the game's intro, main menu movie and music (Hooks/Settings.cpp hands them to the game's INI reads)
static const char* const IntroMovie = "NVRGameStudios.bik";
static const char* const MainMenuMovie = "\\Data\\Video\\NVRMainMenu.bik";
static const char* const MainMenuMusic = "NVRMainMenu";
static char* TitleMenu = (char*)"Vegas Shaders - Settings";

// Key names of the per-weather settings, in the game's own index order (TESWeather colour types, times of day, HDR values).
static const char* const WeatherColorTypes[TESWeather::kNumColorTypes] = {
	"SkyUpper", "Fog", "CloudsLower", "Ambient", "Sunlight",
	"Sun", "Stars", "SkyLower", "Horizon", "CloudsUpper",
};
static const char* const WeatherTimesOfDay[TESWeather::kNumTimeOfDay] = { "Sunrise", "Day", "Sunset", "Night", "Noon", "Midnight" };
static const char* const WeatherHDRTypes[14] = {
	"EyeAdaptation", "BlurRadius", "BlurPasses", "EmissiveMult", "TargetLUM", "UpperLUMClamp", "BrightScale",
	"BrightClamp", "LUMRampNoTex", "LUMRampMin", "LUMRampMax", "SunlightDimmer", "GrassDimmer", "TreeDimmer",
};
