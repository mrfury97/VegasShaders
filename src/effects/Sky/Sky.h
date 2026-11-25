#pragma once
#include "PhysSkyModel.h"

class SkyShaders : public ShaderCollection
{
public:
	SkyShaders() : ShaderCollection("Sky") {};

	struct SettingsStruct{
		float SkyMultiplierDay, SkyMultiplierNight;
		// UNOFFICIAL 2026-10-25 [Shaders.Sky.Main] TintDay/TintNight (R, G, B; white = no change) and MoonBrightness (1 = no change)
		D3DXVECTOR4 TintDay = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f), TintNight = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		float MoonBrightness = 1.0f;
		// UNOFFICIAL 2026-10-26 [Shaders.Sky.Main] TintHorizonDay/TintHorizonNight (white = no change), TintGradientHeight, SaturationDay/
		// SaturationNight (1 = no change), TintKeepsBrightness (the tints change the colour only)
		D3DXVECTOR4 TintHorizonDay = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f), TintHorizonNight = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		float TintGradientHeight = 0.3f, SaturationDay = 1.0f, SaturationNight = 1.0f;
		bool TintKeepsBrightness = true;
		bool ColorLook = true;	// UNOFFICIAL 2026-10-28 [Shaders.Sky.Main] ColorLook: false = every sky colour setting above stands aside
	};
	SettingsStruct Settings;

	struct SkyStruct {
		D3DXVECTOR4		SkyData, SunsetColor, CloudData;
		// Order-2 spherical harmonic irradiance for the sky, for the ambient in the lighting
		// shaders. 9 coefficients, already convolved with the clamped-cosine kernel.
		D3DXVECTOR4		Irradiance[9];
		// UNOFFICIAL physical sky (SKY.pso.hlsl, Includes/Atmosphere.hlsl): x share, y brightness, z weather tint, w haze
		D3DXVECTOR4		PhysSky = D3DXVECTOR4(0.0f, 1.0f, 0.0f, 1.0f);
		D3DXVECTOR4		PhysSkySun = D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);	// xyz: the sun's real direction
		// UNOFFICIAL 2026-10-18 ClassicLook (VolumetricFog.fx TESR_SkyIrradianceGame): the same projection of the game's own sky
		// alone, as the original NVR (master) had it: no physical sky blend and no SkylightingSaturation.
		D3DXVECTOR4		IrradianceGame[9];
		// UNOFFICIAL 2026-10-25 [Shaders.Sky.Physical] LightsObjects: the sky light the lighting shaders (objects, terrain, skin, hair, grass) and
		// SunFar add: Irradiance, or with the switch off the game's own sky (IrradianceGame, SkylightingSaturation applied) while the physical
		// sky still draws the sky, the fog and the clouds.
		D3DXVECTOR4		IrradianceObjects[9];
		// UNOFFICIAL 2026-11-21 [Shaders.Terrain.*] SkylightingSaturation: independent terrain sky SH (SunFar & Terrain shaders read this).
		D3DXVECTOR4		IrradianceTerrain[9];
		// UNOFFICIAL 2026-10-25: rgb the sky's tint at this hour (TintDay to TintNight on the day/night curve; white indoors)
		D3DXVECTOR4		SkyTint = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		// UNOFFICIAL 2026-10-25: x the moon disc's brightness (MoonBrightness, SKYTEX.pso.hlsl)
		D3DXVECTOR4		MoonData = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f);
		// UNOFFICIAL 2026-10-26: rgb the sky's horizon tint at this hour (white indoors); SkyLook x the sky's saturation at this hour,
		// y TintGradientHeight (Shaders/Includes/Sky.hlsl SkyLook)
		D3DXVECTOR4		SkyTintHorizon = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		D3DXVECTOR4		SkyLook = D3DXVECTOR4(1.0f, 0.3f, 0.0f, 0.0f);
		// UNOFFICIAL 2026-10-26: for VolumetricFog's sky colour, rgb SkyTint x SkyTintHorizon (the sky at the horizon), w saturation
		D3DXVECTOR4		FogSkyLook = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		// UNOFFICIAL 2026-10-27: VolumetricFog's own TESR_SkyColor / SkyLowColor / HorizonColor, FogSkyLook already in them (UpdateFogSky)
		D3DXVECTOR4		FogSkyColor, FogSkyLowColor, FogHorizonColor;
		// UNOFFICIAL 2026-10-30: VolumetricFog's own TESR_FogColor, the weather's fog colour with FogSkyLook in it outdoors while the fog
		// takes it (WeatherFogColor on, not LegacyFog / ClassicLook / FogReference); else the raw colour (UpdateFogSky)
		D3DXVECTOR4		FogWeatherColor;
	};
	// UNOFFICIAL [Shaders.Sky.Physical]
	struct PhysicalSettingsStruct { bool Enabled; float Brightness, WeatherTint, Haze; bool ExactSunlight, MultipleScattering; float GroundAlbedo; bool LightsObjects; };
	PhysicalSettingsStruct PhysicalSettings = { false, 1.0f, 0.35f, 1.0f, false, false, 0.3f, true };
	// UNOFFICIAL 2026-10-18 ExactSunlight / MultipleScattering: the CPU twin of the sky-view tables (PhysSkyModel.h), built when the settings need it
	PhysSky::Tables physTables;
	SkyStruct Constants;
	bool useSunDiskColor;
	// UNOFFICIAL: the spherical-harmonic projection in UpdateConstants (512 samples of the sky model, and UpdateConstants
	// runs twice a frame) is redone only when something it reads has changed: these are the last run's inputs. The sun's
	// direction counts as changed when it has turned by more than a tenth of a degree (it creeps every frame outdoors).
	struct ProjectionInputs { D3DXVECTOR4 sunPos, skyColor, skyLowColor, horizonColor, skyData, sunDiskColor, sunsetColor, physical; float sunAmount, saturation, terrainSaturation, exterior; D3DXVECTOR4 physicalMS, skyTint, skyTintHorizon, skyLook; };
	ProjectionInputs lastProjection = {};
	bool lastProjectionValid = false;

	// UNOFFICIAL 2026-11-03 [Main.Main.Performance] SkyOnWorker (cpu-workers): the projection's own copy of everything it reads, and its
	// results (Values: the constants it reads and writes, under their Constants names), so it can run on FrameWorker's thread.
	struct ProjectionJob {
		struct Settings { float Haze, Brightness; bool LightsObjects; };
		struct Values { D3DXVECTOR4 SkyData, PhysSky, PhysSkySun, SkyTint, SkyTintHorizon, SkyLook, Irradiance[9], IrradianceGame[9], IrradianceObjects[9], IrradianceTerrain[9]; };
		D3DXVECTOR4 sunPos;
		D3DXVECTOR3 skyC, skyLowC, horizonC, sunColor;
		float sunHeight, isDayTime, saturation, terrainSaturation;
		bool physical, physExact, physMs;
		const PhysSky::Tables* tables;
		Settings settings;
		Values values;
		// The air is worked out on a grid half as fine each way (one sample per 2 x 2 of the projection's: 128 rays of 12 steps,
		// about a quarter of a millisecond); plenty for the brightness match and the smooth sky light.
		D3DXVECTOR3 physSamples[(16 / 2) * (32 / 2)];
		D3DXVECTOR3 cellDir[16 * 32], cellRadiance[16 * 32];	// RunProjectionParallel: each cell's direction and game-sky colour
	};
	ProjectionJob projectionJob = {};
	bool projectionPending = false;		// started on the worker, results not in Constants yet
	static void RunProjection(void* job);			// the projection as it always ran (SkyOnWorker off, and SkyOnWorkerCheck's reference)
	static void RunProjectionParallel(void* job);	// the same results, its independent samples spread over FrameWorker's threads
	void	CopyProjection(), JoinProjection();

	void	UpdateConstants(), UpdateSettings(), RegisterConstants();
	void	UpdateFogSky();	// UNOFFICIAL 2026-10-27 the fog's tinted sky colours (ShaderManager::UpdateConstants, after the weather's)
	void	UpdateMoonlight();	// UNOFFICIAL 2026-10-25 MoonBrightness: the game's night directional light (ShaderManager::UpdateConstants)
};