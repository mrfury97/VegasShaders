#include "Water.h"

void WaterShaders::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	WaterStruct& d = Constants.Default;
	WaterStruct& p = Constants.Placed;
	sm->RegisterConstant("TESR_WaterDeepColor", &Constants.deepColor);
	sm->RegisterConstant("TESR_WaterShallowColor", &Constants.shallowColor);
	sm->RegisterConstant("TESR_WaterLODColor", &Constants.LODColor);
	sm->RegisterConstant("TESR_WaterFog", &Constants.Fog);
	sm->RegisterConstant("TESR_WaterCoefficients", &d.waterCoefficients);
	sm->RegisterConstant("TESR_WaveParams", &d.waveParams);
	sm->RegisterConstant("TESR_WaterVolume", &d.waterVolume);
	sm->RegisterConstant("TESR_WaterSettings", &d.waterSettings);
	sm->RegisterConstant("TESR_WaterShorelineParams", &d.shorelineParams);
	sm->RegisterConstant("TESR_PlacedWaterCoefficients", &p.waterCoefficients);
	sm->RegisterConstant("TESR_PlacedWaveParams", &p.waveParams);
	sm->RegisterConstant("TESR_PlacedWaterVolume", &p.waterVolume);
	sm->RegisterConstant("TESR_PlacedWaterSettings", &p.waterSettings);
	sm->RegisterConstant("TESR_PlacedWaterShorelineParams", &p.shorelineParams);
}


void WaterShaders::UpdateConstants() {
	WaterStruct& d = Constants.Default;
	WaterStruct& p = Constants.Placed;

	// water surface height at the player; also tells which water form the player is over
	TESWaterForm* currentWater = nullptr;
	const float height = Tes->GetWaterHeight(Player, WorldSceneGraph, &currentWater);

	d.waterSettings.x = height;
	d.waterSettings.z = TheShaderManager->GameState.isUnderwater;
	p.waterSettings.x = d.waterSettings.x;
	p.waterSettings.z = d.waterSettings.z;

	// distant (LOD) water takes TESWorldSpace::GetLODWaterType (follows parent world & falls back to pDefaultWater)
	if (TESWorldSpace* worldSpace = Player->GetWorldSpace()) {
		if (TESWaterForm* lodWater = ThisStdCall<TESWaterForm*>(0x005860C0, worldSpace))
			Constants.LODColor = lodWater->GetShallowColor()->toD3DXVECTOR4();
	}

	if (currentWater) {
		const auto& props = currentWater->properties;
		D3DXVECTOR4& fog = Constants.Fog;
		Constants.deepColor = currentWater->GetDeepColor()->toD3DXVECTOR4();
		Constants.shallowColor = currentWater->GetShallowColor()->toD3DXVECTOR4();
		fog.x = props.fogNearUW;
		fog.y = props.fogFarUW;
		fog.z = props.fogAmountUW;	// UNOFFICIAL 2026-10-22: no longer overwritten with 1 straight after; no shader reads Fog.z today
	}

	// the caustics strength follows the sun glare
	d.waterVolume.x = p.waterVolume.x = causticsStrength * TheShaderManager->ShaderConst.sunGlare;
}

// The keys shared by the default and the placed water groups.
static void ReadWaterShared(const char* section, WaterShaders::WaterStruct& w) {
	SettingManager* cfg = TheSettingManager;
	w.waveParams.x = cfg->GetSettingF(section, "choppiness");
	w.waveParams.y = cfg->GetSettingF(section, "waveWidth");
	w.waveParams.z = cfg->GetSettingF(section, "waveSpeed");
	w.waveParams.w = cfg->GetSettingF(section, "reflectivity");
	w.waterSettings.y = cfg->GetSettingF(section, "depthDarkness");
	w.waterVolume.y = cfg->GetSettingF(section, "shoreFactor");
	w.waterVolume.z = cfg->GetSettingF(section, "turbidity");
	w.waterVolume.w = cfg->GetSettingF(section, "causticsStrengthS");
	w.shorelineParams.x = cfg->GetSettingF(section, "shoreMovement");
	w.waterSettings.w = cfg->GetSettingF(section, "refractionPower");
}

void WaterShaders::UpdateSettings() {
	// pick the settings group that matches the water the player is in
	const char* sectionName = TheShaderManager->GameState.isExterior ? "Shaders.Water.Default" : "Shaders.Water.Interiors";

	if (TESWaterForm* currentWater = Player->parentCell->GetWaterForm()) {
		UInt32 waterType = currentWater->GetWaterType();
		if (waterType == TESWaterForm::WaterType::kWaterType_Blood)
			sectionName = "Shaders.Water.Blood";
		else if (waterType == TESWaterForm::WaterType::kWaterType_Lava)
			sectionName = "Shaders.Water.Lava";
		// per-worldspace overrides are not implemented (they would need the Toml settings)
	}

	WaterStruct& d = Constants.Default;
	d.waterCoefficients.x = TheSettingManager->GetSettingF(sectionName, "inExtCoeff_R");
	d.waterCoefficients.y = TheSettingManager->GetSettingF(sectionName, "inExtCoeff_G");
	d.waterCoefficients.z = TheSettingManager->GetSettingF(sectionName, "inExtCoeff_B");
	d.waterCoefficients.w = TheSettingManager->GetSettingF(sectionName, "inScattCoeff");
	causticsStrength = TheSettingManager->GetSettingF(sectionName, "causticsStrength"); // scaled by the sun glare every frame
	ReadWaterShared(sectionName, d);

	ReadWaterShared("Shaders.Water.Placed", Constants.Placed);
}
