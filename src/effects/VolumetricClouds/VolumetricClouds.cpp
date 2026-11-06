#include "VolumetricClouds.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "../../core/FrameClock.h"
#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
extern D3DXVECTOR4 NvrGameMultisample;	// UNOFFICIAL 2026-11-05 Hooks/Settings.cpp (the game's MSAA samples)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/RtSunShadows.h"	// UNOFFICIAL 2026-10-30 RT sun CloudShadows

void VolumetricCloudsEffect::UpdateSettings() {
	const SettingsStruct before = Settings;
	const char* section = "Shaders.VolumetricClouds.Main";
	Settings.Base = std::clamp(TheSettingManager->GetSettingF(section, "CloudBase"), 200.0f, 8000.0f);
	Settings.Thickness = std::clamp(TheSettingManager->GetSettingF(section, "Thickness"), 100.0f, 6000.0f);
	Settings.Coverage[0] = std::clamp(TheSettingManager->GetSettingF(section, "CoveragePleasant"), 0.0f, 1.0f);
	Settings.Coverage[1] = std::clamp(TheSettingManager->GetSettingF(section, "CoverageCloudy"), 0.0f, 1.0f);
	Settings.Coverage[2] = std::clamp(TheSettingManager->GetSettingF(section, "CoverageRainy"), 0.0f, 1.0f);
	Settings.Coverage[3] = std::clamp(TheSettingManager->GetSettingF(section, "CoverageSnow"), 0.0f, 1.0f);
	// fair weather piles up into heaps, rain and snow spread into flat grey layers
	Settings.TypeBias[0] = 0.2f; Settings.TypeBias[1] = 0.0f; Settings.TypeBias[2] = -0.4f; Settings.TypeBias[3] = -0.4f;
	Settings.Density = std::clamp(TheSettingManager->GetSettingF(section, "Density"), 0.05f, 5.0f);
	Settings.ShapeSize = std::clamp(TheSettingManager->GetSettingF(section, "ShapeSize"), 500.0f, 50000.0f);
	Settings.DetailSize = std::clamp(TheSettingManager->GetSettingF(section, "DetailSize"), 50.0f, 10000.0f);
	Settings.WeatherSize = std::clamp(TheSettingManager->GetSettingF(section, "WeatherSize"), 2000.0f, 200000.0f);
	Settings.Erosion = std::clamp(TheSettingManager->GetSettingF(section, "Erosion"), 0.0f, 1.0f);
	Settings.SunStrength = std::clamp(TheSettingManager->GetSettingF(section, "SunStrength"), 0.0f, 10.0f);
	Settings.AmbientStrength = std::clamp(TheSettingManager->GetSettingF(section, "AmbientStrength"), 0.0f, 10.0f);
	Settings.Forward = std::clamp(TheSettingManager->GetSettingF(section, "ForwardScattering"), 0.0f, 0.95f);
	// UNOFFICIAL 2026-10-15: the D3D9 shader stops at 64 (the game's compiler unrolls the loop). 2026-10-26: up to 256 for the Vulkan
	// march (NvrDxvkPasses, ExtRender gives it the full count); the D3D9 passes still get at most 64 (Constants.March.x).
	Settings.Steps = std::clamp(TheSettingManager->GetSettingI(section, "Steps"), 8, 256);
	Settings.MaxDistance = std::clamp(TheSettingManager->GetSettingF(section, "MaxDistance"), 5000.0f, 200000.0f);
	Settings.HazeDistance = std::clamp(TheSettingManager->GetSettingF(section, "HazeDistance"), 1000.0f, 200000.0f);
	Settings.WindSpeed = std::clamp(TheSettingManager->GetSettingF(section, "WindSpeed"), 0.0f, 20.0f);
	Settings.Evolution = std::clamp(TheSettingManager->GetSettingF(section, "Evolution"), 0.0f, 50.0f);
	Settings.History = std::clamp(TheSettingManager->GetSettingF(section, "History"), 0.0f, 0.95f);
	Settings.JitterWrap = TheSettingManager->GetSettingI(section, "JitterWrap") != 0;
	Settings.Stable = TheSettingManager->GetSettingI(section, "Stable") != 0;
	Settings.Brightness = std::clamp(TheSettingManager->GetSettingF(section, "Brightness"), 0.0f, 5.0f);
	Settings.EdgeSharpness = std::clamp(TheSettingManager->GetSettingF(section, "EdgeSharpness"), 1.0f, 8.0f);
	Settings.ShapeSmoothing = std::clamp(TheSettingManager->GetSettingF(section, "ShapeSmoothing"), 0.0f, 3.0f);
	Settings.Turbulence = std::clamp(TheSettingManager->GetSettingF(section, "Turbulence"), 0.0f, 2.0f);
	Settings.DetailSmoothness = std::clamp(TheSettingManager->GetSettingF(section, "DetailSmoothness"), 0.0f, 1.0f);
	Settings.HideGameClouds = TheSettingManager->GetSettingI(section, "HideGameClouds") != 0;
	Settings.GameTime = TheSettingManager->GetSettingI(section, "GameTime") != 0;
	Settings.Shadows = std::clamp(TheSettingManager->GetSettingF(section, "Shadows"), 0.0f, 1.0f);
	Settings.ShadowRange = std::clamp(TheSettingManager->GetSettingF(section, "ShadowRange"), 4000.0f, 100000.0f);
	Settings.WaterReflections = std::clamp(TheSettingManager->GetSettingF(section, "WaterReflections"), 0.0f, 2.0f);
	Settings.ShadowDebug = TheSettingManager->GetSettingI(section, "ShadowDebug") != 0;
	Settings.ShadowPrefilter = TheSettingManager->GetSettingI(section, "ShadowPrefilter") != 0;	// UNOFFICIAL 2026-10-27
	Settings.ShadowSoftness = std::clamp(TheSettingManager->GetSettingF(section, "ShadowSoftness"), 0.5f, 4.0f);
	Settings.PhysicalShadows = TheSettingManager->GetSettingI(section, "PhysicalShadows") != 0;	// UNOFFICIAL 2026-10-31
	Settings.PhysicalMoonlight = TheSettingManager->GetSettingI(section, "PhysicalMoonlight") != 0;	// UNOFFICIAL physical cloud lighting
	Settings.PhysicalSkyLight = TheSettingManager->GetSettingI(section, "PhysicalSkyLight") != 0;
	Settings.ReflectionDebug = std::clamp(TheSettingManager->GetSettingI(section, "ReflectionDebug"), 0, 2);
	Settings.FineDetail = std::clamp(TheSettingManager->GetSettingF(section, "FineDetail"), 0.0f, 1.0f);
	Settings.FineDetailSize = std::clamp(TheSettingManager->GetSettingF(section, "FineDetailSize"), 10.0f, 5000.0f);
	Settings.CumulusShape = std::clamp(TheSettingManager->GetSettingF(section, "CumulusShape"), 0.0f, 1.0f);	// UNOFFICIAL 2026-10-20
	Settings.Towering = std::clamp(TheSettingManager->GetSettingF(section, "Towering"), 0.0f, 1.0f);	// UNOFFICIAL 2026-10-25
	Settings.TowerHeight = std::clamp(TheSettingManager->GetSettingF(section, "TowerHeight"), 1.0f, 8.0f);
	Settings.TowerAnvil = std::clamp(TheSettingManager->GetSettingF(section, "TowerAnvil"), 0.0f, 1.0f);
	Settings.Sharpness = std::clamp(TheSettingManager->GetSettingF(section, "Sharpness"), 0.0f, 3.0f);	// UNOFFICIAL 2026-10-15: was 0-1
	Settings.FromWeatherTextures = std::clamp(TheSettingManager->GetSettingF(section, "FromWeatherTextures"), 0.0f, 1.0f);
	Settings.FixedMarchExit = TheSettingManager->GetSettingI(section, "FixedMarchExit") != 0;
	Settings.WeatherTint = std::clamp(TheSettingManager->GetSettingF(section, "WeatherTint"), 0.0f, 1.0f);	// UNOFFICIAL 2026-10-25
	// UNOFFICIAL 2026-10-15: a changed setting shows at once (not eased in over a minute like a weather change).
	// UNOFFICIAL 2026-10-21 (P82 tester: "ticking particles menu on and off affects vol clouds"): only a change of the settings the
	// coverage comes from. Every menu switch and every setting edit re-reads all the effects' settings (LoadSettings), and this
	// snapped the clouds on each one: switching Particles (or a foliage setting) jumped them to their weather's look whenever they
	// were still easing toward it (the minute after a load, a weather change or a newly measured weather layer). Switched on, the
	// clouds still start on their weather's look (UpdateConstants).
	if (memcmp(before.Coverage, Settings.Coverage, sizeof(Settings.Coverage)) || memcmp(before.TypeBias, Settings.TypeBias, sizeof(Settings.TypeBias)) ||
		before.FromWeatherTextures != Settings.FromWeatherTextures)
		coverage = -1.0f;
}

void VolumetricCloudsEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_CloudsShape", &Constants.Shape);
	TheShaderManager->RegisterConstant("TESR_CloudsNoise", &Constants.Noise);
	TheShaderManager->RegisterConstant("TESR_CloudsLight", &Constants.Light);
	TheShaderManager->RegisterConstant("TESR_CloudsWind", &Constants.Wind);
	TheShaderManager->RegisterConstant("TESR_CloudsMarch", &Constants.March);
	TheShaderManager->RegisterConstant("TESR_CloudsLayout", &Constants.Layout);
	TheShaderManager->RegisterConstant("TESR_CloudsBlocks", &Constants.Blocks);
	TheShaderManager->RegisterConstant("TESR_CloudsHide", &Constants.Hide);
	TheShaderManager->RegisterConstant("TESR_CloudsLook", &Constants.Look);
	TheShaderManager->RegisterConstant("TESR_CloudsPrevViewProj", (D3DXVECTOR4*)&Constants.PrevViewProj);
	TheShaderManager->RegisterConstant("TESR_CloudsExtra", &Constants.Extra);
	TheShaderManager->RegisterConstant("TESR_CloudsDetail", &Constants.Detail);
	TheShaderManager->RegisterConstant("TESR_CloudsReflectInvVP", (D3DXVECTOR4*)&Constants.ReflectInvVP);
	TheShaderManager->RegisterConstant("TESR_CloudsHistoryViewProj", (D3DXVECTOR4*)&Constants.HistoryViewProj);
	TheShaderManager->RegisterConstant("TESR_CloudsReflectData", &Constants.ReflectData);
	TheShaderManager->RegisterConstant("TESR_CloudsMotion", &Constants.Motion);
	TheShaderManager->RegisterConstant("TESR_CloudsTint", &Constants.Tint);
	TheShaderManager->RegisterConstant("TESR_CloudsTower", &Constants.Tower);
	TheShaderManager->RegisterConstant("TESR_CloudsPhysical", &Constants.Physical);	// UNOFFICIAL physical cloud lighting
	TheShaderManager->RegisterConstant("TESR_CloudsHistoryScale", &Constants.HistoryScale);	// UNOFFICIAL 2026-10-27 RenderScale v2
}

void VolumetricCloudsEffect::RegisterTextures() {
	// Resolution (read once, at start): the march grid at a quarter (0), half (1) or the whole (2) of the screen size each way.
	// UNOFFICIAL 2026-10-14 (user: "an option to make vol clouds higher res / more detailed"): 2 added; the older
	// HalfResolution switch still means 1 when Resolution is 0.
	// UNOFFICIAL 2026-10-15: 3-5 larger than the screen (1.5, 2 and 3 times each way; the composite averages what each pixel
	// covers). Heavy on video memory: at 2560 x 1440, 5 is two histories of 7680 x 4320 half floats, about 600 MB in all.
	int resolution = std::clamp(TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "Resolution"), 0, 5);
	if (resolution == 0 && TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "HalfResolution")) resolution = 1;
	static const float scales[6] = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };
	lowWidth = (UINT)ceilf(TheRenderManager->width * scales[resolution]);
	lowHeight = (UINT)ceilf(TheRenderManager->height * scales[resolution]);
	blockWidth = (lowWidth + 1) / 2;
	blockHeight = (lowHeight + 1) / 2;
	TheTextureManager->InitTexture("TESR_CloudCurrent", &Textures.CurrentTexture, &Textures.CurrentSurface, blockWidth, blockHeight, D3DFMT_A16B16G16R16F);
	// Stable's every-pixel march target. Always registered (the effect binds every TESR_ sampler); full size only
	// where it is small (Resolution 0-1) or Stable is on at startup, so Resolution 2-5 pay nothing unless asked.
	stableReady = resolution <= 1 || TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "Stable") != 0;
	TheTextureManager->InitTexture("TESR_CloudCurrentFull", &Textures.CurrentFullTexture, &Textures.CurrentFullSurface,
		stableReady ? lowWidth : 1, stableReady ? lowHeight : 1, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_CloudHistory0", &Textures.HistoryTexture[0], &Textures.HistorySurface[0], lowWidth, lowHeight, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("NVR_CloudHistory1", &Textures.HistoryTexture[1], &Textures.HistorySurface[1], lowWidth, lowHeight, D3DFMT_A16B16G16R16F);
	Textures.HistoryRead = Textures.HistoryTexture[0];
	TheTextureManager->RegisterTexture("TESR_CloudHistory", (IDirect3DBaseTexture9**)&Textures.HistoryRead);
	// UNOFFICIAL 2026-10-30 the cloud sky probe for reflections (RenderSkyProbe): 64 x 64, 32 KB.
	TheTextureManager->InitTexture("TESR_CloudSkyProbe", &Textures.SkyProbeTexture, &Textures.SkyProbeSurface, 64, 64, D3DFMT_A16B16G16R16F);
	// UNOFFICIAL 2026-10-14 ShadowResolution (read once, at start): the cloud shadow map's size, 256 (as before) to 2048. Over the
	// same ShadowRange a bigger map gives crisper cloud shadow edges; its cost grows with the texel count (it is drawn every frame).
	const int shadowResolution = TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "ShadowResolution");
	shadowMapSize = shadowResolution >= 2048 ? 2048 : shadowResolution >= 1024 ? 1024 : shadowResolution >= 512 ? 512 : 256;
	TheTextureManager->InitTexture("TESR_CloudShadow", &Textures.ShadowTexture, &Textures.ShadowSurface, shadowMapSize, shadowMapSize, D3DFMT_A16B16G16R16F);
	// UNOFFICIAL 2026-10-15 weather sync: the layer probe's 1 x 1 target and its copy the CPU can read
	TheTextureManager->InitTexture("NVR_CloudProbe", &Textures.ProbeTexture, &Textures.ProbeSurface, 1, 1, D3DFMT_A8R8G8B8);
	TheRenderManager->device->CreateOffscreenPlainSurface(1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &probeReadback, nullptr);
	Constants.Layout.x = 1.0f / lowWidth;
	Constants.Layout.y = 1.0f / lowHeight;
	Constants.Blocks.x = 1.0f / blockWidth;
	Constants.Blocks.y = 1.0f / blockHeight;
	D3DXMatrixIdentity(&lastViewProj);
}

// Index into the per-weather settings: 0 pleasant, 1 cloudy, 2 rainy, 3 snow.
static int WeatherIndex(TESWeather* weather) {
	if (!weather) return 0;
	switch (weather->GetWeatherType()) {
	case TESWeather::WeatherType::kType_Cloudy: return 1;
	case TESWeather::WeatherType::kType_Rainy: return 2;
	case TESWeather::WeatherType::kType_Snow: return 3;
	default: return 0;
	}
}

// UNOFFICIAL 2026-10-15 weather sync. The weather type alone is a poor guide: weather mods often mark overcast skies
// "pleasant" (DNWeathers' DNBlackMountain3: a layer 0.78 full) and light ones "cloudy". The weather's own cloud layers say
// how cloudy its sky really is, so the coverage is taken from those textures: how much of each is cloud (mean alpha),
// with the layers stacked (1 - the product of what each leaves clear). Measured with the shipped data: vanilla clear
// skies 0.13 (NVCloudlight), the overcast layer 0.81, sky\alpha.dds (the empty layer) 0.

static std::string NormalTexturePath(const char* path) {
	std::string s = path ? path : "";
	for (char& c : s) c = c == '/' ? '\\' : (char)tolower((unsigned char)c);
	if (s.rfind("data\\", 0) == 0) s.erase(0, 5);
	if (s.rfind("textures\\", 0) == 0) s.erase(0, 9);
	return s;
}

// A texture format with an alpha channel: an opaque one (a painted sky with no alpha) says nothing about how cloudy it is.
static bool HasAlpha(D3DFORMAT format) {
	switch (format) {
	case D3DFMT_DXT1: case D3DFMT_X8R8G8B8: case D3DFMT_R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_L8: return false;
	default: return true;
	}
}

// Notes the current weather's fill once its cloud textures are loaded in the sky (each of its layers matched by path to
// one of the sky's cloud textures; the empty sky\alpha.dds counts 0 even when not loaded).
// The sky's cloud layer j: its texture and that texture's path, from the layer's sky shader property (the game keeps the
// current textures there; Clouds' own texture slots hold the outgoing weather's only while a change blends).
static SkyShaderProperty* CloudLayer(Sky* sky, int j) {
	NiGeometry* layer = (NiGeometry*)sky->clouds->layers[j];
	if (!layer) return nullptr;
	SkyShaderProperty* property = layer->propertyState.GetShadeProperty<SkyShaderProperty>();
	return property && property->eSkyObjectType == SkyShaderProperty::SO_CLOUDS ? property : nullptr;
}

void VolumetricCloudsEffect::LearnWeatherFill(Sky* sky) {
	TESWeather* weather = sky ? sky->firstWeather : nullptr;
	if (!weather || !sky->clouds || weatherFill.count(weather)) return;
	float layers[4] = { 0, 0, 0, 0 };
	float clear = 1.0f;
	bool unreadable = false;
	for (int i = 0; i < 4; i++) {
		const std::string path = NormalTexturePath(weather->textureLayers[i].ddsPath.m_data);
		if (path.empty() || path == "sky\\alpha.dds") continue;
		auto cached = textureFill.find(path);
		if (cached == textureFill.end()) {
			NiTexture* match = nullptr;
			for (int j = 0; j < 4 && !match; j++) {
				SkyShaderProperty* layer = CloudLayer(sky, j);
				if (!layer || !layer->spBaseTexture || !layer->strTextureName.m_data) continue;
				NiTexture* texture = layer->spBaseTexture;
				const std::string loaded = NormalTexturePath(layer->strTextureName.m_data);
				const size_t start = loaded.size() - path.size();
				if (loaded.size() >= path.size() && loaded.compare(start, path.size(), path) == 0 && (start == 0 || loaded[start - 1] == '\\')) match = texture;
			}
			if (!match || !match->rendererData || !match->rendererData->dTexture) {
				// not loaded yet: try again next frame. After about two seconds without it, the paths are logged once (what the
				// weather asks for, what the sky holds), so a mismatch can be seen in the log; its type decides meanwhile.
				if (++learnTries == 120) {
					Logger::Log("UNOFFICIAL VolumetricClouds: weather %08X: its cloud layer '%s' is not among the sky's loaded cloud textures yet:", weather->refID, path.c_str());
					for (int j = 0; j < 4; j++) {
						SkyShaderProperty* layer = CloudLayer(sky, j);
						NiTexture* texture = layer ? layer->spBaseTexture : nullptr;
						Logger::Log("UNOFFICIAL VolumetricClouds:   sky cloud layer %d: %s%s", j, layer && layer->strTextureName.m_data ? layer->strTextureName.m_data : "(none)",
							layer && !(texture && texture->rendererData && texture->rendererData->dTexture) ? " (no texture yet)" : "");
					}
					for (int k = 0; k < 4; k++) Logger::Log("UNOFFICIAL VolumetricClouds:   weather layer %d: %s", k, weather->textureLayers[k].ddsPath.m_data ? weather->textureLayers[k].ddsPath.m_data : "(none)");
				}
				return;
			}
			// The game's textures sit in video memory (the CPU cannot read them): measured on the GPU (ProbeLayer, next cloud
			// frame); -2 marks it waiting.
			D3DSURFACE_DESC desc = {};
			IDirect3DBaseTexture9* base = match->rendererData->dTexture;
			if (base->GetType() != D3DRTYPE_TEXTURE || FAILED(((IDirect3DTexture9*)base)->GetLevelDesc(0, &desc)) || !HasAlpha(desc.Format)) {
				textureFill.emplace(path, -1.0f);
				Logger::Log("UNOFFICIAL VolumetricClouds: cloud texture %s: no alpha (format %u): left out", path.c_str(), (UINT)desc.Format);
				continue;
			}
			const UINT size = (std::max)(desc.Width, desc.Height);
			float lod = 0.0f;
			while ((size >> (UINT)(lod + 1.0f)) >= 16 && lod + 1.0f < (float)base->GetLevelCount()) lod += 1.0f;
			base->AddRef();
			probeQueue.push_back({ path, base, lod, desc.Width, desc.Height });
			textureFill.emplace(path, -2.0f);
			return;
		}
		if (cached->second == -2.0f) return;	// still being measured
		if (cached->second < 0.0f) { unreadable = true; continue; }	// no alpha: left out
		layers[i] = cached->second;
		clear *= 1.0f - cached->second;
	}
	const float fill = 1.0f - clear;
	weatherFill[weather] = unreadable && fill == 0.0f ? -1.0f : fill;	// only unreadable layers: its type decides
	float coverage, rain, bias;
	WeatherLook(weather, coverage, rain, bias);
	const char* name = weather->GetEditorName();
	static const char* types[4] = { "pleasant", "cloudy", "rainy", "snow" };
	Logger::Log("UNOFFICIAL VolumetricClouds: weather %s (%08X, %s): the game's cloud layers fill %.2f %.2f %.2f %.2f, sky %.2f -> coverage %.2f",
		name && *name ? name : "(no editor id)", weather->refID, types[WeatherIndex(weather)], layers[0], layers[1], layers[2], layers[3], fill, coverage);
}

static float Piecewise(float x, const float (*points)[2], int count) {
	if (x <= points[0][0]) return points[0][1];
	for (int i = 1; i < count; i++)
		if (x <= points[i][0]) return std::lerp(points[i - 1][1], points[i][1], (x - points[i - 1][0]) / (points[i][0] - points[i - 1][0]));
	return points[count - 1][1];
}

// One weather's coverage, rain darkening and cloud type: from its type, then moved toward what its own cloud layers show
// (FromWeatherTextures). The four coverage settings keep their meaning: an empty game sky gives half the fair-weather
// value, vanilla's clear sky (0.13 full) the fair-weather value, a half-full sky the cloudy one, an overcast one (0.85)
// the rainy one. Rain and snow never get less than their own setting (no rain from a clear sky).
void VolumetricCloudsEffect::WeatherLook(TESWeather* weather, float& coverage, float& rain, float& bias) {
	const int type = WeatherIndex(weather);
	coverage = Settings.Coverage[type];
	rain = type == 2 || type == 3 ? 1.0f : 0.0f;
	bias = Settings.TypeBias[type];
	auto found = weather ? weatherFill.find(weather) : weatherFill.end();
	if (found == weatherFill.end() || found->second < 0.0f || Settings.FromWeatherTextures <= 0.0f) return;
	const float fill = found->second;
	const float coveragePoints[4][2] = { { 0.0f, Settings.Coverage[0] * 0.5f }, { 0.13f, Settings.Coverage[0] }, { 0.5f, Settings.Coverage[1] }, { 0.85f, Settings.Coverage[2] } };
	const float biasPoints[3][2] = { { 0.13f, Settings.TypeBias[0] }, { 0.5f, Settings.TypeBias[1] }, { 0.85f, Settings.TypeBias[2] } };
	float fromTextures = Piecewise(fill, coveragePoints, 4);
	float biasFromTextures = Piecewise(fill, biasPoints, 3);
	if (rain > 0.0f) { fromTextures = (std::max)(fromTextures, coverage); biasFromTextures = (std::min)(biasFromTextures, bias); }
	coverage = std::lerp(coverage, fromTextures, Settings.FromWeatherTextures);
	bias = std::lerp(bias, biasFromTextures, Settings.FromWeatherTextures);
}

// UNOFFICIAL 2026-10-25 WeatherTint. The game paints its flat cloud layers with each weather's cloud colours (four layers, a colour
// for each of six times of day), as weather makers set them: orange at sunset, grey when overcast, blue at night. The volumetric
// clouds take their light from the sun and the sky but never saw these. The share of each time's colour at this hour, as the game
// blends its sky colours (checked in game on the weather's top sky colour against Sky::skyUpper, exact at 5.5, 6, 12.8, 13, 19,
// 19.5 and 23.5 h): night until the sunrise colours begin (GetSunriseColorBegin), night to sunrise to day through the sunrise,
// day with high noon (the fifth colour, index 4; Game.h's enum names it midnight) rising to all of it at 12:00 and gone again when
// the sunset begins, then day to sunset to night (GetSunsetColorEnd). The sixth colour is not used by the game.
static void TimeWeights(Sky* sky, float hour, float weights[6]) {
	const float a = sky->GetSunriseColorBegin(), b = sky->GetSunriseEnd(), c = sky->GetSunsetBegin(), d = sky->GetSunsetColorEnd();
	const float riseMid = 0.5f * (a + b), setMid = 0.5f * (c + d), noon = std::clamp(12.0f, b, c);
	const int highNoon = 4;
	for (int t = 0; t < 6; t++) weights[t] = 0.0f;
	auto blend = [&](int from, int to, float t) { t = std::clamp(t, 0.0f, 1.0f); weights[from] = 1.0f - t; weights[to] = t; };
	if (hour < a || hour >= d) weights[TESWeather::eTime_Night] = 1.0f;
	else if (hour < riseMid) blend(TESWeather::eTime_Night, TESWeather::eTime_Sunrise, (hour - a) / (std::max)(riseMid - a, 1e-3f));
	else if (hour < b) blend(TESWeather::eTime_Sunrise, TESWeather::eTime_Day, (hour - riseMid) / (std::max)(b - riseMid, 1e-3f));
	else if (hour < noon) blend(TESWeather::eTime_Day, highNoon, (hour - b) / (std::max)(noon - b, 1e-3f));
	else if (hour < c) blend(TESWeather::eTime_Day, highNoon, (c - hour) / (std::max)(c - noon, 1e-3f));
	else if (hour < setMid) blend(TESWeather::eTime_Day, TESWeather::eTime_Sunset, (hour - c) / (std::max)(setMid - c, 1e-3f));
	else blend(TESWeather::eTime_Sunset, TESWeather::eTime_Night, (hour - setMid) / (std::max)(d - setMid, 1e-3f));
}

static float SrgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }

// The weather's cloud colour at these time weights: its layers that show something (not empty, not sky\alpha.dds), each by how
// much of the sky its texture covers where measured. Linear.
D3DXVECTOR3 VolumetricCloudsEffect::CloudColourOf(TESWeather* weather, const float weights[6]) {
	D3DXVECTOR3 sum(0, 0, 0);
	float total = 0.0f;
	for (int i = 0; i < 4; i++) {
		const std::string path = NormalTexturePath(weather->textureLayers[i].ddsPath.m_data);
		if (path.empty() || path == "sky\\alpha.dds") continue;
		auto fill = textureFill.find(path);
		if (fill != textureFill.end() && fill->second >= 0.0f && fill->second < 0.01f) continue;	// measured empty
		const float w = fill != textureFill.end() && fill->second >= 0.0f ? fill->second : 0.5f;
		D3DXVECTOR3 colour(0, 0, 0);
		for (int t = 0; t < 6; t++) {
			const RGBA& c = *(const RGBA*)&weather->cloudColor[i][t];
			colour += D3DXVECTOR3(c.r, c.g, c.b) * (weights[t] / 255.0f);
		}
		sum += D3DXVECTOR3(SrgbToLinear(colour.x), SrgbToLinear(colour.y), SrgbToLinear(colour.z)) * w;
		total += w;
	}
	return total > 0.0f ? sum / total : D3DXVECTOR3(1, 1, 1);
}

D3DXVECTOR3 VolumetricCloudsEffect::WeatherCloudColour(Sky* sky, float hour) {
	TESWeather* current = sky ? sky->firstWeather : nullptr;
	if (!current) return D3DXVECTOR3(1, 1, 1);
	float weights[6];
	TimeWeights(sky, hour, weights);
	D3DXVECTOR3 colour = CloudColourOf(current, weights);
	if (sky->secondWeather && sky->secondWeather != current)
		colour = CloudColourOf(sky->secondWeather, weights) + (colour - CloudColourOf(sky->secondWeather, weights)) * std::clamp(sky->weatherPercent, 0.0f, 1.0f);
	// The check (the first 16, on a new weather or hour): the same weights on the weather's top sky colour against the game's own
	// (off by a lot: the hours are wrong), and the weather's cloud colours as read.
	if (tintLogs < 16 && (current != tintLogWeather || fabsf(hour - tintLogHour) > 0.2f)) {
		tintLogWeather = current;
		tintLogHour = hour;
		tintLogs++;
		D3DXVECTOR3 top(0, 0, 0);
		for (int t = 0; t < 6; t++) {
			const RGBA& c = current->colors[TESWeather::eColor_SkyUpper].colors[t];
			top += D3DXVECTOR3(c.r, c.g, c.b) * (weights[t] / 255.0f);
		}
		Logger::Log("UNOFFICIAL VolumetricClouds WeatherTint: weather %08X (before %08X, %.2f) hour %.2f (colour times %.2f %.2f %.2f %.2f) weights %.2f %.2f %.2f %.2f %.2f %.2f, sky top ours %.3f %.3f %.3f game %.3f %.3f %.3f, cloud colour %.3f %.3f %.3f",
			current->refID, sky->secondWeather ? sky->secondWeather->refID : 0, sky->weatherPercent, hour,
			sky->GetSunriseColorBegin(), sky->GetSunriseEnd(), sky->GetSunsetBegin(), sky->GetSunsetColorEnd(), weights[0], weights[1], weights[2], weights[3], weights[4], weights[5],
			top.x, top.y, top.z, sky->skyUpper.r, sky->skyUpper.g, sky->skyUpper.b, colour.x, colour.y, colour.z);
		for (int i = 0; i < 4; i++) {
			const RGBA* c = (const RGBA*)current->cloudColor[i];
			Logger::Log("UNOFFICIAL VolumetricClouds WeatherTint:   layer %d '%s': sunrise %u %u %u, day %u %u %u, sunset %u %u %u, night %u %u %u, high noon %u %u %u, unused %u %u %u", i,
				current->textureLayers[i].ddsPath.m_data ? current->textureLayers[i].ddsPath.m_data : "", c[0].r, c[0].g, c[0].b, c[1].r, c[1].g, c[1].b,
				c[2].r, c[2].g, c[2].b, c[3].r, c[3].g, c[3].b, c[4].r, c[4].g, c[4].b, c[5].r, c[5].g, c[5].b);
		}
		{
			const RGBA* c = current->colors[TESWeather::eColor_SkyUpper].colors;
			Logger::Log("UNOFFICIAL VolumetricClouds WeatherTint:   sky top: sunrise %u %u %u, day %u %u %u, sunset %u %u %u, night %u %u %u, high noon %u %u %u, unused %u %u %u",
				c[0].r, c[0].g, c[0].b, c[1].r, c[1].g, c[1].b, c[2].r, c[2].g, c[2].b, c[3].r, c[3].g, c[3].b, c[4].r, c[4].g, c[4].b, c[5].r, c[5].g, c[5].b);
		}
	}
	return colour;
}

// UNOFFICIAL 2026-10-25 (test tool: the shot list's 'facemoon').
// Where the moon is: its disc mesh's world bound (the moon's node sits at the sky's centre, the disc is placed inside it), as a
// direction and distance from the sky's centre, and its radius. False when there is no moon to see.
bool VolumetricCloudsEffect::MoonPlace(Sky* sky, D3DXVECTOR3& direction, float& distance, float& radius, const char** why) {
	const char* dummy;
	if (!why) why = &dummy;
	if (!sky || !sky->nodeSkyRoot || !sky->masserMoon) { *why = "no sky or no moon object"; return false; }
	Moon* moon = sky->masserMoon;
	if (!moon->MoonNode || (moon->MoonNode->m_flags & 1)) { *why = "the moon is hidden"; return false; }	// (flag 1: hidden)
	NiAVObject* disc = reinterpret_cast<NiAVObject*>(moon->MoonMesh ? moon->MoonMesh : moon->ShadowMesh);	// (NiTriShape is only declared here; an NiAVObject at offset 0)
	if (!disc || !disc->m_kWorldBound || (disc->m_flags & 1)) { *why = "the moon's disc is not drawn"; return false; }
	const NiPoint3& centre = sky->nodeSkyRoot->m_worldTransform.pos;
	const NiPoint3& at = disc->m_kWorldBound->Center;
	direction = D3DXVECTOR3(at.x - centre.x, at.y - centre.y, at.z - centre.z);
	distance = D3DXVec3Length(&direction);
	radius = disc->m_kWorldBound->Radius;
	if (NiAVObject* shadow = reinterpret_cast<NiAVObject*>(moon->ShadowMesh)) if (shadow->m_kWorldBound) radius = max(radius, shadow->m_kWorldBound->Radius);
	if (distance <= 1.0f || radius <= 0.0f || radius >= distance) { *why = "the moon's size or distance is not usable"; return false; }
	direction /= distance;
	return true;
}

void VolumetricCloudsEffect::UpdateConstants() {
	Sky* sky = Tes ? Tes->sky : nullptr;
	// UNOFFICIAL 2026-10-13: once a frame, at the tuned rate (FrameClock.h: UpdateConstants runs twice a frame).
	static double lastFrameTime = -1.0;
	const float dt = (float)(ElapsedOncePerFrame(lastFrameTime) * TunedClockRate);
	// UNOFFICIAL 2026-10-21: not updated last frame = switched back on: the clouds start on their weather's look at once, as the settings
	// reload that comes with the menu's switch made them before (UpdateSettings now snaps only when a coverage setting changes).
	const unsigned int frameNumber = TheFrameRateManager->FrameNumber;
	if (updatedBefore && frameNumber - lastUpdateFrame > 1) coverage = -1.0f;
	lastUpdateFrame = frameNumber;
	updatedBefore = true;

	// The weather's coverage, blended across a weather change as the game blends its own (weatherPercent: 1 = the new
	// weather in full), then eased so a change of weather builds or clears the clouds over about a minute.
	// UNOFFICIAL 2026-10-15: each weather's look comes from its own cloud layers where known (WeatherLook), and a forced
	// change (the console's fw, or a mod forcing a weather: the new weather in full at once) snaps to it, as the game's
	// own sky does, instead of easing in over a minute. A change the game blends in (sw, the climate's own changes)
	// still follows the game's blend.
	if (dt > 0.0f) LearnWeatherFill(sky);
	TESWeather* current = sky ? sky->firstWeather : nullptr;
	if (current != lastWeather) {
		learnTries = 0;
		if (lastWeather && current && (!sky->secondWeather || sky->weatherPercent >= 0.99f)) snapWait = 1.0f;
		lastWeather = current;
	}
	float targetCoverage = Settings.Coverage[0], targetRain = 0.0f, targetType = Settings.TypeBias[0];
	if (current) {
		float nowCoverage, nowRain, nowBias;
		WeatherLook(current, nowCoverage, nowRain, nowBias);
		targetCoverage = nowCoverage; targetRain = nowRain; targetType = nowBias;
		if (sky->secondWeather && sky->secondWeather != current) {
			float beforeCoverage, beforeRain, beforeBias;
			WeatherLook(sky->secondWeather, beforeCoverage, beforeRain, beforeBias);
			const float t = std::clamp(sky->weatherPercent, 0.0f, 1.0f);
			targetCoverage = std::lerp(beforeCoverage, nowCoverage, t);
			targetRain = std::lerp(beforeRain, nowRain, t);
			targetType = std::lerp(beforeBias, nowBias, t);
		}
	}
	bool snap = coverage < 0.0f || TheShaderManager->GameState.isCameraCut;
	if (snapWait >= 0.0f) {
		// wait (up to a second) for the new weather's cloud textures to load, so it snaps once, to its own look
		snapWait -= dt;
		if (snapWait < 0.0f || weatherFill.count(current)) { snap = true; snapWait = -1.0f; }
	}
	if (snap) { coverage = targetCoverage; rain = targetRain; typeBias = targetType; }
	const float ease = 1.0f - expf(-dt / 20.0f);
	coverage += (targetCoverage - coverage) * ease;
	rain += (targetRain - rain) * ease;
	typeBias += (targetType - typeBias) * ease;

	// Wind: the sky's own wind direction and speed move the clouds; the detail drifts on its own so they also change shape.
	const float windSpeed = sky ? std::clamp(sky->windSpeed, 0.0f, 1.0f) : 0.2f;
	const float windAngle = sky ? sky->windDirection : 0.0f;
	// Faster than real wind at that height (which barely moves a cloud kilometres away), as games do and as the game's
	// own cloud layers scroll.
	const float speed = Settings.WindSpeed * (15.0f + 45.0f * windSpeed);	// metres per second
	// UNOFFICIAL GameTime: the clouds move by the game's clock instead of the frame time, measured in seconds at the normal
	// time scale (so in play they move exactly as before): a pause or a menu stops them, waiting or sleeping moves them on
	// as far as that time would, and the first frame after a load places them from the game's date, so the same save shows
	// the same sky. Off: the frame time, as before.
	float advance = dt;
	if (Settings.GameTime) {
		TimeGlobals* clock = TimeGlobals::Get();
		const double days = clock && clock->GameDaysPassed ? (double)clock->GameDaysPassed->data : -1.0;
		const double scale = clock && clock->TimeScale && clock->TimeScale->data > 0.0f ? (double)clock->TimeScale->data : 30.0;
		if (days >= 0.0) {
			const double now = days * 86400.0 / scale;
			// UNOFFICIAL 2026-10-15 (user: the clouds hitch and move in tiny increments): the game keeps its day count in a
			// float, so after many days it only moves in coarse steps (300 days in: about 2.6 game seconds, 0.09 s of play, so
			// the clouds jumped every five or six frames and stood still between). The clouds now run on their own smooth clock,
			// the frame time, held to the game's clock: never more than a little ahead of it (a pause or a menu stops them
			// within that), caught up gently when behind, and moved on at once by a wait, sleep or travel.
			const float dayCount = (float)days;
			const double quantum = (double)(nextafterf(dayCount, FLT_MAX) - dayCount) * 86400.0 / scale;
			const double lead = (std::max)(0.25, 2.0 * quantum);
			static bool toldQuantum = false;
			if (!toldQuantum) { toldQuantum = true; Logger::Log("UNOFFICIAL VolumetricClouds: game day %.3f: the game's clock moves in steps of %.3f s of play here; the clouds run smoothly between them", days, quantum); }
			if (lastCloudTime < 0.0 || now < lastCloudTime - 60.0 - lead || now - lastCloudTime > 30.0 * 86400.0) {
				// from the date: the wind as it stands now, carried over the whole time since the game began
				const double total = fmod(now, 1.0e7);
				windX = fmod(cos((double)windAngle) * speed * total, 900000.0);
				windY = fmod(sin((double)windAngle) * speed * total, 900000.0);
				windZ = fmod(Settings.Evolution * total, 900000.0);
				lastCloudTime = now;
				advance = 0.0f;
			}
			else if (now - lastCloudTime > (std::max)(2.0, 4.0 * quantum) + dt / TunedClockRate) {
				const double step = now - lastCloudTime;
				if (step > 120.0) Logger::Log("UNOFFICIAL VolumetricClouds: the game clock moved %.2f hours at once (wait, sleep, travel): the clouds move on with it (%.0f m).", step * scale / 3600.0, step * speed);
				advance = (float)step;
				lastCloudTime = now;
			}
			else {
				// UNOFFICIAL fixes-b #44 (LOOK CHANGE, clouds off by default): the hold / catch-up runs in REAL seconds (dt is the frame time
				// x TunedClockRate, but the game's clock `now` runs at 1x: holding a 2x clock at now + lead made the clouds move at half
				// the tuned speed, and in coarse steps on a late save). The move itself is then multiplied by the tuned rate.
				const double realDt = dt / TunedClockRate;
				double next = lastCloudTime + realDt;
				if (next > now + lead) next = (std::max)(lastCloudTime, now + lead);	// paused (or the clock is slower): hold
				else if (next < now - lead) next += (now - lead - next) * (std::min)(1.0, 2.0 * realDt);	// behind: catch up gently
				advance = (float)((next - lastCloudTime) * TunedClockRate);
				lastCloudTime = next;
			}
		}
	}
	// wrapped at 900 km, a whole number of tiles of every noise texture, so the wrap never shows. UNOFFICIAL 2026-10-15: kept
	// in doubles (a float near 900 km rounds each frame's half-metre move to 6 cm steps: uneven motion).
	const double lastX = windX, lastY = windY;
	windX = fmod(windX + cos((double)windAngle) * speed * advance, 900000.0);
	windY = fmod(windY + sin((double)windAngle) * speed * advance, 900000.0);
	windZ = fmod(windZ + Settings.Evolution * advance, 900000.0);
	wind = D3DXVECTOR3((float)windX, (float)windY, (float)windZ);
	// UNOFFICIAL 2026-10-15: this frame's move of the cloud pattern, for the history's reprojection (a wrap is not a move)
	double moveX = windX - lastX, moveY = windY - lastY;
	if (fabs(moveX) > 450000.0) moveX -= copysign(900000.0, moveX);
	if (fabs(moveY) > 450000.0) moveY -= copysign(900000.0, moveY);
	if (dt > 0.0f) { Constants.Motion.x = (float)moveX; Constants.Motion.y = (float)moveY; }	// the frame's first call (the second moves nothing)
	Constants.Motion.w = Settings.Sharpness;
	// UNOFFICIAL 2026-10-31 PhysicalShadows (user: "the clouds' shadows don't look like real shadows"): under a thick cloud the sun is
	// gone, so its shadow takes all of the sunlight (Shadows 0.6 left 40% of the sun and sharp object shadows under an overcast sky);
	// Shadows still switches them off at 0. The edges come from the smooth shape (ShadowPrefilter's map) and the low sun is kept
	// (ShadowStrengthForSunFar). Off: as before.
	const float cloudShadows = Settings.PhysicalShadows && Settings.Shadows > 0.0f ? 1.0f : Settings.Shadows;
	Constants.Extra = D3DXVECTOR4(Textures.ShadowSurface ? cloudShadows : 0.0f, Settings.ShadowRange, (float)Settings.ReflectionDebug, Settings.ShadowDebug ? 1.0f : 0.0f);

	// UNOFFICIAL 2026-10-25 Towering: the layer grows to TowerHeight times Thickness (the ordinary clouds keep to the bottom
	// Thickness of it, the shader raises only the strongest heaps), eased back to off as the sky closes over (as CumulusShape).
	// The march takes a quarter more steps for each extra layer depth (the shader stops at 64): the history averages the
	// rest of the coarser sampling away (offline at 1280 x 720 no visible change against a full half, ~0.3 ms less).
	const float towerFade = 1.0f - std::clamp((coverage - 0.6f) / 0.3f, 0.0f, 1.0f);
	const float towering = Settings.Towering * towerFade * towerFade * (3.0f - 2.0f * towerFade);	// smoothstep, as the shader's Cumulus
	const float towerScale = 1.0f + (Settings.TowerHeight - 1.0f) * towering;
	// UNOFFICIAL 2026-10-26 (NVR DXVK): ExtMarchSteps is the same count without the D3D9 shader's 64 cap, for the Vulkan march (ExtRender).
	ExtMarchSteps = (int)lroundf(Settings.Steps * (1.0f + 0.25f * (towerScale - 1.0f)));
	const int marchSteps = (std::min)(64, ExtMarchSteps);
	Constants.Tower = D3DXVECTOR4(1.0f / towerScale, towering, Settings.TowerAnvil, Settings.ShadowPrefilter || Settings.PhysicalShadows ? Settings.ShadowSoftness : 0.0f);	// w: ShadowPrefilter (2026-10-27; PhysicalShadows 2026-10-31)
	Constants.Shape = D3DXVECTOR4(Settings.Base, Settings.Thickness * towerScale, coverage, Settings.Density);
	Constants.Noise = D3DXVECTOR4(Settings.ShapeSize, Settings.DetailSize, Settings.Erosion, Settings.WeatherSize);
	Constants.Light = D3DXVECTOR4(Settings.SunStrength, Settings.AmbientStrength, Settings.Forward, rain);
	Constants.Physical = D3DXVECTOR4(Settings.PhysicalMoonlight ? 1.0f : 0.0f, Settings.PhysicalSkyLight ? 1.0f : 0.0f, 0.0f, 0.0f);	// UNOFFICIAL
	Constants.Wind = D3DXVECTOR4(wind.x, wind.y, wind.z, typeBias);
	Constants.March = D3DXVECTOR4((float)marchSteps, Settings.MaxDistance, (float)frame, Settings.HazeDistance);
	Constants.Layout.w = Settings.Brightness;
	Constants.Look = D3DXVECTOR4(Settings.EdgeSharpness, Settings.ShapeSmoothing, Settings.Turbulence, Settings.DetailSmoothness);
	Constants.Detail = D3DXVECTOR4(Settings.FineDetail, Settings.FineDetailSize, (float)shadowMapSize, Settings.CumulusShape);
	// UNOFFICIAL 2026-10-25 WeatherTint: the weather's cloud colour, its brightness taken out (the clouds keep the light they get
	// from the sun and sky; only the colour moves), its strongest channel held to twice the brightness so a deep night blue tints
	// rather than floods.
	Constants.Tint = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 0.0f);
	if (Settings.WeatherTint > 0.0f && sky) {
		const D3DXVECTOR3 colour = WeatherCloudColour(sky, TheShaderManager->ShaderConst.GameTime.y);
		const float luma = colour.x * 0.2126f + colour.y * 0.7152f + colour.z * 0.0722f;
		if (luma > 1e-4f) {
			D3DXVECTOR3 tint = colour / luma;
			const float peak = (std::max)({ tint.x, tint.y, tint.z });
			if (peak > 2.0f) tint = D3DXVECTOR3(1, 1, 1) + (tint - D3DXVECTOR3(1, 1, 1)) * ((2.0f - 1.0f) / (peak - 1.0f));
			Constants.Tint = D3DXVECTOR4(tint.x, tint.y, tint.z, Settings.WeatherTint);
		}
	}

	HideGameClouds = (Enabled && Effect && Settings.HideGameClouds && ShouldRender()) ? 1.0f : 0.0f;
	HideGameCloudsInReflection = HideGameClouds > 0.0f && ReflectionWanted() ? 1.0f : 0.0f;
	Constants.Hide.x = HideGameClouds;
}

bool VolumetricCloudsEffect::ShouldRender() {
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater &&
		Textures.CurrentSurface && Textures.HistorySurface[0] && Textures.HistorySurface[1];
}

// UNOFFICIAL 2026-10-15 weather sync: measures one queued game cloud texture (LayerProbe, technique 5): its mean alpha into a
// 1 x 1 target, read back to the CPU. Once per texture per session; the read back waits for the GPU once (a millisecond or so).
void VolumetricCloudsEffect::ProbeLayer(IDirect3DDevice9* Device) {
	if (probeQueue.empty() || !Textures.ProbeSurface || !probeReadback) return;
	ProbeJob job = probeQueue.front();
	probeQueue.erase(probeQueue.begin());
	float fill = -1.0f;
	Device->SetTexture(13, job.texture);
	TheRenderManager->renderState->NoteDeviceTexture(13, job.texture);
	const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP},
		{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR} };
	for (const auto& state : states) TheRenderManager->SetSamplerState(13, (D3DSAMPLERSTATETYPE)state[0], state[1]);
	Constants.Motion.z = job.lod;
	TheShaderManager->RenderEffectToRT(Textures.ProbeSurface, this, false, 5);
	Device->SetTexture(13, nullptr);
	TheRenderManager->renderState->NoteDeviceTexture(13, nullptr);
	D3DLOCKED_RECT locked;
	if (SUCCEEDED(Device->GetRenderTargetData(Textures.ProbeSurface, probeReadback)) && SUCCEEDED(probeReadback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
		fill = (float)(*(const UInt32*)locked.pBits >> 24) / 255.0f;
		probeReadback->UnlockRect();
	}
	job.texture->Release();
	textureFill[job.path] = fill;
	Logger::Log("UNOFFICIAL VolumetricClouds: cloud texture %s (%ux%u, read at mip %.0f): %.0f%% cloud%s", job.path.c_str(), job.width, job.height,
		job.lod, fill * 100.0f, fill < 0.0f ? " (could not be read: left out)" : "");
}

// UNOFFICIAL 2026-10-25 Towering: while it is on (and the effect file has them: ShadowMapTowers is the last), the march, reflection
// and shadow map passes take their *Towers techniques (7-10: the storms and the taller heaps). Off, the older ones, unchanged.
static bool TowersOn(VolumetricCloudsEffect* clouds) {
	static EffectRecord::CachedHandle last;
	return clouds->Constants.Tower.y > 0.0f && clouds->Effect && clouds->TechniqueHandle(last, "ShadowMapTowers") != NULL &&
		clouds->Effect->GetTechnique(10) == clouds->TechniqueHandle(last, "ShadowMapTowers");
}

// UNOFFICIAL 2026-10-27 ShadowPrefilter: the shadow map pass takes technique 11 (ShadowMapPrefilter: the smooth shape only, four
// taps per texel, no speckles on faces and ground) while it is on and the effect file has it. Off, technique 4 or 10 as before.
// (fork2 play merge 2026-10-27: c843f888 had put it on the water reflection's technique 3 / 9, which drew the shadow map's values
// into the reflection and left the shadow map as it was; it is RenderShadowMap's. NVR DXVK: ExtRender runs it as the shadow map
// pass with push.prefilter.)
static bool PrefilterOn(VolumetricCloudsEffect* clouds) {
	static EffectRecord::CachedHandle last;
	return clouds->Constants.Tower.w > 0.0f && clouds->Effect && clouds->TechniqueHandle(last, "ShadowMapPrefilter") != NULL &&
		clouds->Effect->GetTechnique(11) == clouds->TechniqueHandle(last, "ShadowMapPrefilter");
}

void VolumetricCloudsEffect::RenderClouds(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceSurface) {
	// The history is kept only across consecutive frames in the same place: after a cell change (a door, fast travel)
	// or a pause in the clouds (indoors, a menu), it starts over from this frame's march.
	// (UNOFFICIAL 2026-10-27: the march and its history are full size in every frame now -- see below -- so another RenderScale, or
	// FSR / DLSS on or off, no longer starts it over.)
	const D3DXMATRIX viewProj = TheRenderManager->ViewProjMatrix;
	const double now = TheFrameRateManager->Time;
	// UNOFFICIAL 2026-11-01 SharedNoise (core/TemporalBackbone.h): and the shared camera rule (cut, a 512-unit jump, a gap); not its
	// render-size part: the clouds' march and history are full size in every frame (see below).
	const bool sharedNoise = TemporalBackbone::Shared();
	const bool continuous = lastRenderTime >= 0.0 && now - lastRenderTime < 0.25 && !TheShaderManager->GameState.isCameraCut &&
		(!sharedNoise || TemporalBackbone::CameraContinuous());
	EffectStatus::History(StatusSlot, Name, continuous ? "kept" : lastRenderTime < 0.0 ? "reset: first render" : now - lastRenderTime >= 0.25 ?
		"reset: not rendered for 0.25 s (indoors, a menu)" : TheShaderManager->GameState.isCameraCut ? "reset: camera cut" : "reset: shared rule (jump / gap)");	// UNOFFICIAL 2026-11-01
	// SharedNoise: the 2 x 2 order and the march jitter from the shared index (1024 is a multiple of 4 and 16: seamless at the wrap);
	// `frame` itself still counts the clouds' own renders (the shadow map and the sky probe go by it). Off: `frame`, as before.
	const UInt32 noiseFrame = sharedNoise ? TemporalBackbone::EffectIndex(TemporalBackbone::OffsetClouds) : frame;
	static const float order[4][2] = { {0, 0}, {1, 1}, {1, 0}, {0, 1} };
	Constants.PrevViewProj = continuous ? lastViewProj : viewProj;
	static CachedHandle stableCache;
	const bool stable = Settings.Stable && stableReady && Textures.CurrentFullSurface && Effect &&
		TechniqueHandle(stableCache, "TemporalStable") != NULL && Effect->GetTechnique(14) == TechniqueHandle(stableCache, "TemporalStable");
	if (Settings.Stable && !stable) {
		static bool told = false;
		if (!told) { told = true; Logger::Log("UNOFFICIAL VolumetricClouds: Stable is on but not available (%s): the usual 1-in-4 march runs.",
			stableReady ? "the effect has no TemporalStable technique (old Effects/VolumetricClouds.fx?)" : "Resolution 2-5 needs a restart after turning Stable on"); }
	}
	// Stable: every pixel is fresh each frame, so the old per-frame History (applied to one pixel in four) is
	// spread over four frames to keep the same smoothing, then scaled by the frame time (60 fps reference).
	const float stableKeep = 1.0f - (1.0f - Settings.History) * 0.25f;
	const float frames60 = std::clamp((float)(now - lastRenderTime) * 60.0f, 0.25f, 4.0f);
	Constants.Layout.z = continuous ? (stable ? powf(stableKeep, frames60) : Settings.History) : 0.0f;
	Constants.Blocks.z = order[noiseFrame % 4][0];
	Constants.Blocks.w = order[noiseFrame % 4][1];
	// JitterWrap: the march jitter is frac(noise + March.z * 0.618) in 32-bit floats; with the frame count
	// growing all session it keeps 16 distinct values after ~1M frames and 2 near 8M (banding, shimmer).
	// 4096 keeps 1/4096 steps and is a multiple of 4, so the 2x2 order and the towers' floor(z / 4) stay in step.
	Constants.March.z = (float)(stable ? noiseFrame % 16 : Settings.JitterWrap ? noiseFrame % 4096 : noiseFrame);

	Textures.HistoryRead = Textures.HistoryTexture[read];
	ProbeLayer(Device);
	// UNOFFICIAL 2026-10-13: F10 timers per pass.
	static GpuTimer marchTimer("  Clouds march (quarter, 1 in 4)"), temporalTimer("  Clouds temporal (quarter)"),
		compositeTimer("  Clouds composite (full)");
	// fixes-b #19: technique 6 only if it is the one NAMED MarchFixedExit (an older effect file with another technique 6 stays on March)
	static CachedHandle fixedExitCache;
	const bool fixedExit = Settings.FixedMarchExit && Effect && TechniqueHandle(fixedExitCache, "MarchFixedExit") != NULL &&
		Effect->GetTechnique(6) == TechniqueHandle(fixedExitCache, "MarchFixedExit");
	{
		// UNOFFICIAL 2026-10-27 (user, with FSR: "changing cloud steps seems to do nothing", towering clouds "a bit noisy with 256
		// steps"): in a RenderScale frame the march and its history drew into the drawn part of their targets, a quarter of the LOWER
		// resolution, and through the D3D9 shader, which stops at 64 steps (NVR DXVK's Vulkan passes are off in those frames). They
		// work on the clouds' own targets in screen directions and never read the scene, so they now run as in a native frame (whole
		// targets, TESR_RenderScale 1, the Vulkan march with the full Steps): the clouds are as sharp and steady as without FSR / DLSS.
		// Only the composite, which reads the scene, runs at the lower resolution (it reads the history at the screen position).
		RenderRect::Suspend full;
		const D3DXVECTOR4 scale = TAAEffect::RenderScaleConstant;
		TAAEffect::RenderScaleConstant = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		if (stable) {
			static GpuTimer stableMarchTimer("  Clouds march (stable, every pixel)"), stableTemporalTimer("  Clouds temporal (stable)");
			{ GpuProfileScope gpu(stableMarchTimer, Device); TheShaderManager->RenderEffectToRT(Textures.CurrentFullSurface, this, false, TowersOn(this) ? 13 : 12); }
			{ GpuProfileScope gpu(stableTemporalTimer, Device); TheShaderManager->RenderEffectToRT(Textures.HistorySurface[1 - read], this, false, 14); }
		}
		else {
			{ GpuProfileScope gpu(marchTimer, Device); TheShaderManager->RenderEffectToRT(Textures.CurrentSurface, this, false,
				TowersOn(this) ? (fixedExit ? 8 : 7) : (fixedExit ? 6 : 1)); }
			{ GpuProfileScope gpu(temporalTimer, Device); TheShaderManager->RenderEffectToRT(Textures.HistorySurface[1 - read], this, false, 2); }
		}
		RenderShadowMap(Device);
		TAAEffect::RenderScaleConstant = scale;
	}
	read = 1 - read;
	Textures.HistoryRead = Textures.HistoryTexture[read];
	Constants.HistoryScale.x = 1.0f;	// the whole history is drawn (the next water reflection's history lookup)
	Constants.HistoryScale.y = 1.0f;
	Device->SetRenderTarget(0, RenderTarget);
	{
		// UNOFFICIAL 2026-10-21: when SunFar took the shadows to the land this frame, the composite only draws the sky (strength 0
		// for its own apply). Its own apply darkened the whole pixel, ambient and sky light too, on top of the sun's shadows,
		// weighted by the depth-rebuilt normals (flat per triangle, striped by the depth's precision): a P82 tester's triangles.
		const float strength = Constants.Extra.x;
		if (shadowsViaSunFar || RtSunShadows::CloudsApplied()) Constants.Extra.x = 0.0f;	// 2026-10-30: or in the RT sun's visibility
		// UNOFFICIAL 2026-10-31 PhysicalShadows: the composite's own apply darkens the whole pixel (ambient and sky too), so it keeps the
		// Shadows setting as before; full strength is only for the sun-only paths (SunFar, the RT sun).
		else if (Settings.PhysicalShadows) Constants.Extra.x = (std::min)(strength, Settings.Shadows);
		// UNOFFICIAL 2026-10-28 (user: the cloud flicker "is from a combination of jitter and fsr low res"): with FSR / DLSS LowResEffects
		// the composite draws into the small JITTERED frame before the upscale, but reads the clouds (drawn whole, unjittered) at the plain
		// screen position, so the upscaler, which shifts each frame back by its jitter, moved the clouds by the jitter every frame. The
		// clouds are now read where this frame's jittered pixel looks: the picture moved left / down by (JitterX, JitterY) render pixels
		// (TAAUpscale.cpp), so a pixel shows the unjittered screen at + (JitterX / width, -JitterY / height). History scale zw = that offset.
		const D3DXVECTOR2 jitterUV = TheShaderManager->Effects.TAA ? TheShaderManager->Effects.TAA->LowResJitterUV() : D3DXVECTOR2(0.0f, 0.0f);
		Constants.HistoryScale.z = jitterUV.x;
		Constants.HistoryScale.w = jitterUV.y;
		GpuProfileScope gpu(compositeTimer, Device);
		Render(Device, RenderTarget, RenderedSurface, 0, false, SourceSurface);	// composite
		Constants.Extra.x = strength;
		Constants.HistoryScale.z = Constants.HistoryScale.w = 0.0f;
	}

	lastViewProj = viewProj;
	lastRenderTime = now;
	// UNOFFICIAL 2026-10-30: the cloud sky probe, when reflections want it (before frame++, with this frame's constants).
	ReflectionsEffect* reflections = TheShaderManager->Effects.Reflections;
	if (reflections && reflections->Enabled && reflections->Settings.CloudSky) RenderSkyProbe(Device);
	frame++;
}

// UNOFFICIAL 2026-10-30: technique 12 into the 64 x 64 probe, one 8-row band a frame (the whole sky every 8 frames: the clouds drift
// slowly; the first whole map 8 frames after start). Vulkan only (there is no D3D9 technique 12): ExtRender is called itself, never
// through Render, so nothing can fall back to the .fx. (The first try, every texel of 128 x 128 a frame at full steps: 12 ms on the iGPU.)
// The sky probe is Vulkan only and has no D3D9 technique; its pass number stays clear of the effect's techniques
// (12-14 are Stable's march and temporal).
static const UINT SkyProbePass = 99;

void VolumetricCloudsEffect::RenderSkyProbe(IDirect3DDevice9* Device) {
	if (!Textures.SkyProbeSurface || !DxvkCompute::PassesOn()) return;
	static GpuTimer probeTimer("  Clouds sky probe");
	GpuProfileScope gpu(probeTimer, Device);
	if (ExtRender(Device, Textures.SkyProbeSurface, Textures.SkyProbeSurface, SkyProbePass, false, Textures.SkyProbeSurface)) { skyProbeFrame = frame; skyProbeBands++; }
}

// UNOFFICIAL 2026-10-21: the cloud shadow map, once a frame. SunFar asks first (it runs before the clouds in the frame), so the
// land shadow and the map's centre are this frame's; otherwise RenderClouds draws it.
void VolumetricCloudsEffect::RenderShadowMap(IDirect3DDevice9* Device) {
	if (shadowMapFrame == frame || Constants.Extra.x <= 0.0f || !Textures.ShadowSurface || !Enabled || !Effect || !ShouldRender()) return;
	static GpuTimer shadowTimer("  Clouds shadow map");
	GpuProfileScope gpu(shadowTimer, Device);
	TheShaderManager->RenderEffectToRT(Textures.ShadowSurface, this, false, PrefilterOn(this) ? 11 : TowersOn(this) ? 10 : 4);	// 11: ShadowPrefilter (2026-10-27)
	shadowMapFrame = frame;
	// UNOFFICIAL 2026-10-30 (RT sun CloudShadows): what this map was drawn with (VolumetricClouds.fx.hlsl ShadowCentre / ShadowLight)
	rtMapRange = (std::max)(Constants.Extra.y, 1000.0f);
	rtMapBase = Constants.Shape.x;
	const float texel = rtMapRange / (float)shadowMapSize;
	rtMapCentre[0] = floorf(TheRenderManager->CameraPosition.x / 70.0f / texel) * texel;
	rtMapCentre[1] = floorf(TheRenderManager->CameraPosition.y / 70.0f / texel) * texel;
	D3DXVECTOR3 light(TheShaderManager->ShaderConst.SunDir.x, TheShaderManager->ShaderConst.SunDir.y, TheShaderManager->ShaderConst.SunDir.z);
	D3DXVec3Normalize(&light, &light);
	light.z = (std::max)(light.z, 0.25f);
	D3DXVec3Normalize(&light, &light);
	rtMapLight[0] = light.x; rtMapLight[1] = light.y; rtMapLight[2] = light.z;
	rtMapValid = true;
}

// UNOFFICIAL 2026-10-21: the shadows' strength on the land as the composite works it out (fainter at night, gone with a low sun);
// 0 with no shadows, or with ShadowDebug (that view is the composite's).
float VolumetricCloudsEffect::ShadowStrengthForSunFar() {
	if (!Enabled || !Effect || !ShouldRender() || !Textures.ShadowTexture || Constants.Extra.x <= 0.0f || Settings.ShadowDebug) return 0.0f;
	const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.SunDir;
	const float length = sqrtf(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
	const float z = length > 0.0f ? sun.z / length : 0.0f;
	// UNOFFICIAL 2026-10-31 PhysicalShadows: full strength from 0.25 up (about 14 degrees: the height the map's light is held to, so down
	// to there the shadow lands where the cloud is) instead of 0.35 (20 degrees); gone below 0.08 (4.6 degrees) either way.
	const float lo = 0.08f, hi = Settings.PhysicalShadows ? 0.25f : 0.35f;
	const float t = std::clamp((z - lo) / (hi - lo), 0.0f, 1.0f);
	const float day = std::clamp(TheShaderManager->ShaderConst.SunAmount.x, 0.0f, 1.0f);
	return Constants.Extra.x * (0.3f + 0.7f * day) * t * t * (3.0f - 2.0f * t);
}

// ---- UNOFFICIAL 2026-10-12: the clouds in the game's water reflection ----

// Drawn when on, outdoors, and the history is this session's last frame or so (the reflection pass comes before this frame's
// march: it shows last frame's clouds, as on screen a frame ago).
bool VolumetricCloudsEffect::ReflectionWanted() const {
	return Enabled && Effect && Settings.WaterReflections > 0.0f && TheShaderManager->GameState.isExterior &&
		!TheShaderManager->GameState.isUnderwater && lastRenderTime >= 0.0 && TheFrameRateManager->Time - lastRenderTime < 0.25;
}

// The reflection is drawn multisampled and copied (resolved) into the texture the water reads with StretchRect, inside the
// game's pass: drawn after the pass, the clouds never reached that texture. The device's StretchRect (vtable slot 34) is
// hooked once: a copy FROM the noted reflection target draws the clouds into it first. Any other copy passes straight on.
namespace {
	typedef HRESULT(__stdcall* StretchRectFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
	StretchRectFn OriginalStretchRect = nullptr;
	void* RuntimeStretchRect = nullptr;	// the runtime's own entry, as first seen
	VolumetricCloudsEffect* ResolveOwner = nullptr;
	HRESULT __stdcall CloudsStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* dest,
		const RECT* destRect, D3DTEXTUREFILTERTYPE filter) {
		if (ResolveOwner) ResolveOwner->BeforeResolve(device, source);
		return OriginalStretchRect(device, source, sourceRect, dest, destRect, filter);
	}
}

void VolumetricCloudsEffect::BeforeResolve(IDirect3DDevice9* Device, IDirect3DSurface9* source) {
	if (reflectTarget && source == reflectTarget) {
		static int told = 0;
		if (told++ < 3) Logger::Log("UNOFFICIAL VolumetricClouds: the reflection is copied for the water (StretchRect): clouds drawn first");
		DrawIntoReflection(Device);
	}
}

// At the first shader bind in the reflection pass: its target, depth buffer and camera (later targets in the pass are other
// work). The native runtime puts its own StretchRect back into the device's table (seen right after the first draw), so the
// hook is checked at every pass; it is put back only over the runtime's own entry, never over another program's hook.
void VolumetricCloudsEffect::CaptureReflection(IDirect3DDevice9* Device) {
	if (reflectNoted || !ReflectionWanted()) return;
	reflectNoted = true;
	void** table = *(void***)Device;
	if (!RuntimeStretchRect) RuntimeStretchRect = table[34];
	if (table[34] == RuntimeStretchRect) {
		DWORD old = 0;
		if (VirtualProtect(&table[34], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			OriginalStretchRect = (StretchRectFn)table[34];
			table[34] = (void*)&CloudsStretchRect;
			VirtualProtect(&table[34], sizeof(void*), old, &old);
			ResolveOwner = this;
			static int told = 0;
			if (told++ < 2) Logger::Log("UNOFFICIAL VolumetricClouds: StretchRect hooked (the clouds go into the reflection before the game copies it for the water)");
		}
	}
	else if (table[34] != (void*)&CloudsStretchRect) {
		static bool told = false;
		if (!told) { told = true; Logger::Log("UNOFFICIAL VolumetricClouds: another program owns StretchRect: the clouds are drawn after the reflection pass"); }
	}
	if (FAILED(Device->GetRenderTarget(0, &reflectTarget))) { reflectTarget = nullptr; return; }
	if (FAILED(Device->GetDepthStencilSurface(&reflectDepth))) reflectDepth = nullptr;
	if (FAILED(Device->GetTransform(D3DTS_VIEW, &reflectView)) || FAILED(Device->GetTransform(D3DTS_PROJECTION, &reflectProj)) || !reflectDepth) {
		reflectTarget->Release(); reflectTarget = nullptr;
		if (reflectDepth) { reflectDepth->Release(); reflectDepth = nullptr; }
	}
}

// The end of the game's reflection pass: drawn now if no copy came during it (a reflection drawn straight into its texture).
void VolumetricCloudsEffect::EndReflectionPass(IDirect3DDevice9* Device) {
	DrawIntoReflection(Device);
	reflectNoted = false;
}

// Before the copy for the water (else after the game's reflection pass): a quad at the far plane over the reflection, depth-tested against the pass's own depth
// buffer (only the sky is still at the far plane), blended over it. Every device state is put back as it was (a state block),
// so the game's render-state cache stays true.
void VolumetricCloudsEffect::DrawIntoReflection(IDirect3DDevice9* Device) {
	if (!reflectTarget) return;
	IDirect3DSurface9* target = reflectTarget; IDirect3DSurface9* depth = reflectDepth;
	reflectTarget = nullptr; reflectDepth = nullptr;
	D3DSURFACE_DESC desc;
	if (ReflectionWanted() && SUCCEEDED(target->GetDesc(&desc)) && (reflectState || SUCCEEDED(Device->CreateStateBlock(D3DSBT_ALL, &reflectState)))) {
		const bool reversed = TheRenderManager->IsReversedDepth();
		// The reflection camera's clip space -> world direction: its view without the translation (directions only).
		D3DXMATRIX rotation = reflectView;
		rotation._41 = rotation._42 = rotation._43 = 0.0f;
		D3DXMATRIX viewProj;
		D3DXMatrixMultiply(&viewProj, &rotation, &reflectProj);
		if (D3DXMatrixInverse(&Constants.ReflectInvVP, nullptr, &viewProj)) {
			Constants.HistoryViewProj = lastViewProj;
			Constants.ReflectData = D3DXVECTOR4(std::clamp(Settings.WaterReflections, 0.0f, 1.0f), (float)desc.Width, (float)desc.Height, reversed ? 0.0f : 1.0f);
			if (!reflectLogged) {
				reflectLogged = true;
				Logger::Log("UNOFFICIAL VolumetricClouds: drawing the clouds into the water reflection (%ux%u, format %u, multisample %u, %s depth)",
					desc.Width, desc.Height, (unsigned)desc.Format, (unsigned)desc.MultiSampleType, reversed ? "reversed" : "normal");
				Logger::Log("UNOFFICIAL VolumetricClouds: reflection view rows %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | t %.1f %.1f %.1f; proj 11 %.3f 22 %.3f 33 %.5f 43 %.3f; det %.3f; main view row3 %.3f %.3f %.3f",
					reflectView._11, reflectView._12, reflectView._13, reflectView._21, reflectView._22, reflectView._23, reflectView._31, reflectView._32, reflectView._33,
					reflectView._41, reflectView._42, reflectView._43, reflectProj._11, reflectProj._22, reflectProj._33, reflectProj._43, D3DXMatrixDeterminant(&rotation),
					TheRenderManager->viewMatrix._31, TheRenderManager->viewMatrix._32, TheRenderManager->viewMatrix._33);
			}
			reflectState->Capture();
			IDirect3DSurface9* oldTarget = nullptr; IDirect3DSurface9* oldDepth = nullptr;
			Device->GetRenderTarget(0, &oldTarget);
			Device->GetDepthStencilSurface(&oldDepth);
			Device->SetRenderTarget(0, target);
			Device->SetDepthStencilSurface(depth);
			D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
			Device->SetViewport(&viewport);
			Device->SetStreamSource(0, TheShaderManager->FrameVertex, 0, 5 * sizeof(float));
			Device->SetFVF(FrameFVF);
			Effect->SetTechnique(Effect->GetTechnique(TowersOn(this) ? 9 : 3));	// (the reflection: ShadowPrefilter is the shadow map's, RenderShadowMap)
			SetCT();
			UINT passes = 0;
			if (SUCCEEDED(Effect->Begin(&passes, 0))) {
				Effect->BeginPass(0);
				Device->SetRenderState(D3DRS_ZENABLE, Settings.ReflectionDebug == 2 ? D3DZB_FALSE : D3DZB_TRUE);	// debug 2: no depth test
				Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
				Device->SetRenderState(D3DRS_ZFUNC, reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
				Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
				Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
				Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				Device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
				Device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
				Device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
				Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
				Device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
				Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
				Device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
				Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				Effect->EndPass();
				Effect->End();
			}
			Device->SetRenderTarget(0, oldTarget);
			Device->SetDepthStencilSurface(oldDepth);
			if (oldTarget) oldTarget->Release();
			if (oldDepth) oldDepth->Release();
			reflectState->Apply();
			// UNOFFICIAL 2026-10-13: SetCT wrote our textures and sampler states through the game's render-state cache, and
			// the state block has just put the device back behind its back: the cache still held ours (WRAP on s2-s5/s8, CLAMP
			// on the rest). A later draw that wanted exactly our value was skipped and kept the device's restored one: a terrain
			// layer drawn clamped, its edge texels smeared into stripes (user report, save NVRAutoTest6). Tell the cache what
			// the device really holds again.
			for (UInt32 c = 0; c < TextureShaderValuesCount; c++) {
				const UInt32 stage = TextureShaderValues[c].RegisterIndex;
				if (stage >= 16) continue;
				IDirect3DBaseTexture9* texture = nullptr;
				Device->GetTexture(stage, &texture);
				TheRenderManager->renderState->NoteDeviceTexture(stage, texture);
				if (texture) texture->Release();
				for (int i = 1; i < SamplerStatesMax; i++) {
					DWORD value = 0;
					if (SUCCEEDED(Device->GetSamplerState(stage, (D3DSAMPLERSTATETYPE)i, &value)))
						TheRenderManager->SetSamplerState(stage, (D3DSAMPLERSTATETYPE)i, value);
				}
			}
		}
	}
	target->Release();
	if (depth) depth->Release();
}

// ---- UNOFFICIAL 2026-10-25 (NVR DXVK): see VolumetricClouds.h ----
// The constants are read from the same registered memory the D3D9 path uploads (ConstantsTable; SetCT sets the matrices with
// SetMatrix, the logical matrix), in the order of the GLSL's uniform block (Compute/VolumetricClouds.glsl): the three matrices,
// then the float4s. TESR_PhysSky / TESR_PhysSkySun are only read when the effect was compiled with PHYSICAL_SKY_ON.
namespace {
	const char* const CloudsMatrixNames[] = { "TESR_CloudsPrevViewProj", "TESR_ViewTransform", "TESR_ProjectionTransform" };
	const char* const CloudsVectorNames[] = { "TESR_ReciprocalResolution", "TESR_SunDirection", "TESR_SunColor", "TESR_SunAmount",
		"TESR_SkyColor", "TESR_HorizonColor", "TESR_CloudsShape", "TESR_CloudsNoise", "TESR_CloudsLight", "TESR_CloudsWind",
		"TESR_CloudsMarch", "TESR_CloudsLayout", "TESR_CloudsBlocks", "TESR_CloudsLook", "TESR_CloudsDetail", "TESR_CloudsExtra",
		"TESR_CloudsMotion", "TESR_WaterSettings", "TESR_CameraData", "TESR_CameraPosition", "TESR_CloudsTint", "TESR_CloudsTower",
		"TESR_PhysSky", "TESR_PhysSkySun" };	// (TESR_CloudsTint, TESR_CloudsTower: UNOFFICIAL 2026-10-26, WeatherTint and Towering)
	const UINT CloudsOptionalVectors = 2;	// the last two
	struct CloudsUniforms {
		D3DXMATRIX	Matrices[_countof(CloudsMatrixNames)];
		D3DXVECTOR4	Vectors[_countof(CloudsVectorNames)];
	};
	// Push block: the samplers' heap indices (POINT/CLAMP, LINEAR/CLAMP, POINT/WRAP, trilinear/WRAP; filled in by DXVK), then the
	// two compile-time switches of the D3D9 shaders.
	struct CloudsPush { UInt32 pointIndex, linearIndex, pointWrapIndex, mipIndex, fixedExit, physicalSky, towers, prefilter, physicalMoonlight, physicalSkyLight; };	// towers: 2026-10-26 (the *Towers techniques); prefilter: 2026-10-27 (technique 11); physicalMoonlight, physicalSkyLight: UNOFFICIAL settings of the same names

	const D3DXVECTOR4* CloudsRegistered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	// A named TESR_ texture as the D3D9 pass binds it (its TextureManager slot); null if none.
	IDirect3DBaseTexture9* CloudsNamedTexture(const char* name) {
		std::string text = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(text);
		return slot ? *slot : nullptr;
	}
}

bool VolumetricCloudsEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	// UNOFFICIAL 2026-11-01 (tester: the whole world darkened with the clouds on Vulkan; band-aid, cause not found): the clouds keep their
	// D3D9 passes unless [Main.Main.Performance] NvrDxvkClouds is on (the Vulkan-only sky probe, Reflections CloudSky, then waits).
	if (!TheSettingManager->SettingsMain.Main.NvrDxvkClouds) return false;
	// Per technique (0 Composite, 1 March, 2 Temporal, 4 ShadowMap, 6 MarchFixedExit; 2026-10-26: 7 MarchTowers, 8 MarchTowersFixedExit,
	// 10 ShadowMapTowers, the same shaders with push.towers; 2026-10-27: 11 ShadowMapPrefilter, push.prefilter): its shader, and its textures in binding order
	// (set 1, binding i, slot i), as the .fx declares them: the noise textures 3D, through LINEAR/WRAP with LINEAR mips.
	int pass;
	switch (techniqueIndex) {
	case 0: pass = 0; break;
	case 1: case 6: case 7: case 8: pass = 1; break;
	case 2: pass = 2; break;
	case 4: case 10: case 11: pass = 3; break;	// (11: ShadowMapPrefilter, 2026-10-27: the same shader with push.prefilter)
	case SkyProbePass: pass = 4; break;	// UNOFFICIAL 2026-10-30 the cloud sky probe (RenderSkyProbe; Vulkan only, the march's inputs)
	default: return false;	// 3 / 9 Reflection (drawn by hand), 5 LayerProbe (a game texture on stage 12): D3D9
	}
	if (ClearRenderTarget || (pass != 0 && RenderTarget != RenderedSurface)) return false;
	// UNOFFICIAL 2026-11-05: with the game's MSAA the composite is the D3D9 one, which takes each edge pixel's sky share (SkyCoverage,
	// Includes/MsaaSkyEdge.hlsl); the Vulkan composite has no such read yet.
	if (pass == 0 && NvrGameMultisample.x > 1.0f) return false;
	static const char* const SpvNames[5] = { "VolumetricCloudsComposite", "VolumetricCloudsMarch", "VolumetricCloudsTemporal", "VolumetricCloudsShadowMap",
		"VolumetricCloudsSkyProbe" };

	const bool physicalSky = ParameterHandle(physicalSkyCache, "TESR_SkyViewLUT") != NULL;
	IDirect3DBaseTexture9* weather = ExtFileTexture("TESR_CloudWeather");
	IDirect3DBaseTexture9* blueNoise = ExtFileTexture("TESR_BlueNoiseSampler");
	IDirect3DBaseTexture9* textures[6] = {};
	bool volume[6] = {};
	UINT count = 0;
	switch (pass) {
	case 0: {
		IDirect3DBaseTexture9* depth = CloudsNamedTexture("TESR_DepthBuffer");
		textures[0] = TheTextureManager->RenderedTexture; textures[1] = depth; textures[2] = CloudsNamedTexture("TESR_NormalsBuffer");
		textures[3] = Textures.ShadowTexture ? Textures.ShadowTexture : depth;	// no map: shadow strength 0, never read
		textures[4] = Textures.HistoryRead; count = 5; break;
	}
	case 1: case 3: case 4:
		textures[0] = weather; textures[1] = ExtFileTexture("TESR_CloudShape"); textures[2] = ExtFileTexture("TESR_CloudDetail");
		textures[3] = ExtFileTexture("TESR_NoiseVolume"); volume[1] = volume[2] = volume[3] = true; count = 4;
		if (pass == 1 || pass == 4) {
			textures[4] = blueNoise;
			textures[5] = physicalSky ? CloudsNamedTexture("TESR_SkyViewLUT") : blueNoise;	// without PHYSICAL_SKY_ON never read
			count = 6;
		}
		break;
	case 2: textures[0] = Textures.CurrentTexture; textures[1] = Textures.HistoryRead; count = 2; break;
	}
	for (UINT i = 0; i < count; i++) if (!textures[i]) return false;

	ExtPass& ext = extPasses[pass];
	if (!ext.Tried) {
		ext.Tried = true;
		NVR_EXT_BINDING_DESC bindings[11] = {};
		for (UINT i = 0; i < count; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i;
			bindings[i].ViewType = volume[i] ? NVR_EXT_VIEW_3D : NVR_EXT_VIEW_2D;
		}
		bindings[count] = { 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
		for (UINT s = 0; s < 4; s++) {
			bindings[count + 1 + s].Type = NVR_EXT_BIND_SAMPLER; bindings[count + 1 + s].Slot = s; bindings[count + 1 + s].PushOffset = s * 4;
		}
		ext.Pipeline = DxvkCompute::CreateGraphicsPipeline(SpvNames[pass], bindings, count + 5, sizeof(CloudsPush), true);
	}
	ID3D9NvrExtSampler* samplers[4] = { DxvkCompute::PointClamp(), DxvkCompute::LinearClamp(), DxvkCompute::PointWrap(),
		DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP) };	// the last: null on an NVR DXVK older than 1.3 (D3D9 then)
	if (!ext.Pipeline || !samplers[0] || !samplers[1] || !samplers[2] || !samplers[3]) return false;

	CloudsUniforms uniforms = {};
	for (UINT i = 0; i < _countof(CloudsMatrixNames); i++) {
		const D3DXVECTOR4* value = CloudsRegistered(CloudsMatrixNames[i]);
		if (!value) return false;
		memcpy(&uniforms.Matrices[i], value, sizeof(D3DXMATRIX));
	}
	for (UINT i = 0; i < _countof(CloudsVectorNames); i++) {
		const D3DXVECTOR4* value = CloudsRegistered(CloudsVectorNames[i]);
		if (value) uniforms.Vectors[i] = *value;
		else if (i < _countof(CloudsVectorNames) - CloudsOptionalVectors) return false;
		// UNOFFICIAL 2026-10-26: the Vulkan march has no instruction limit: the full Steps (above 64 too; D3D9 gets at most 64).
		if (!strcmp(CloudsVectorNames[i], "TESR_CloudsMarch")) uniforms.Vectors[i].x = (float)ExtMarchSteps;
	}
	ID3D9NvrExtBuffer* buffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!buffer) return false;

	NVR_EXT_RESOURCE_BINDING resources[11] = {};
	for (UINT i = 0; i < count; i++) {
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[count].Slot = 0; resources[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[count].pResource = buffer;
	for (UINT s = 0; s < 4; s++) {
		resources[count + 1 + s].Slot = s; resources[count + 1 + s].Type = NVR_EXT_BIND_SAMPLER; resources[count + 1 + s].pResource = samplers[s];
	}
	CloudsPush push = {};
	push.fixedExit = techniqueIndex == 6 || techniqueIndex == 8 ? 1 : 0;
	push.towers = techniqueIndex >= 7 && techniqueIndex <= 10 ? 1 : 0;
	if (techniqueIndex == SkyProbePass) push.towers = TowersOn(this) ? 1 : 0;	// the probe: the clouds' current kind
	push.prefilter = techniqueIndex == 11 ? 1 : 0;
	push.physicalSky = physicalSky ? 1 : 0;
	push.physicalMoonlight = Settings.PhysicalMoonlight ? 1 : 0;	// UNOFFICIAL physical cloud lighting (TESR_CloudsPhysical on D3D9)
	push.physicalSkyLight = Settings.PhysicalSkyLight ? 1 : 0;

	if (pass == 0)
		return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, ext.Pipeline, resources, count + 5, &push, sizeof(push), true);
	if (pass == 4) {	// UNOFFICIAL 2026-10-30 the sky probe: this frame's band only (the rest keeps its texels)
		const LONG band = (LONG)(skyProbeBands % 8) * 8;
		const RECT rows = { 0, band, 64, band + 8 };
		return ExtDrawTo(ext.Pipeline, &RenderTarget, 1, resources, count + 5, &push, sizeof(push), &rows);
	}
	if (!ExtDrawTo(ext.Pipeline, &RenderTarget, 1, resources, count + 5, &push, sizeof(push))) return false;
	if (++ExtFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: %s runs as Vulkan passes.", Name);
	return true;
}
