#include <algorithm>

#include "Skin.h"
#include "../../core/RainState.h"	// UNOFFICIAL 2026-11-12 RainStateGuard
#include "../../core/DeferredSkin.h"	// UNOFFICIAL 2026-11-15 DeferredSkinBlur (TESR_SkinDebug.w)
#include "../SkinScattering/SkinScattering.h"

void SkinShaders::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SkinMaterial", &Constants.Material);
	TheShaderManager->RegisterConstant("TESR_SkinLook", &Constants.Look);
	TheShaderManager->RegisterConstant("TESR_SkinDebug", &Constants.Debug);
	TheShaderManager->RegisterConstant("TESR_SkinSSS", &Constants.SSS);	// UNOFFICIAL 2026-11-15 SSS v2 (the deferred skin's passes)
}

// The material in use (outdoors or in), each frame.
void SkinShaders::UpdateConstants() {
	const bool isExterior = TheShaderManager->GameState.isExterior;
	const MaterialStruct& Material = isExterior ? Exterior : Interior;
	Constants.Material = D3DXVECTOR4(Material.SpecularStrength, Material.Roughness, Material.Scattering, Material.Translucency);

	// Rain wets skin: smoother and shinier. The same rain / puddle factor the object and terrain shaders use.
	float wetness = 0.0f;
	if (isExterior && TheShaderManager->Effects.WetWorld) {
		wetness = std::clamp(RainState::Factor() * RainWetness, 0.0f, 1.0f);	// UNOFFICIAL 2026-11-12 RainStateGuard (core/RainState.h; off: the old max of WetWorld's Data.x and z)
	}
	// Nothing occludes the sky the reflection samples, so indoors it would light skin from a sky it cannot see.
	Constants.Look = D3DXVECTOR4(isExterior ? SkyReflection : 0.0f, wetness, Material.ShadowScatter, VanillaMatchedHighlights);

	// The shader's own views only; the blur's views leave the skin drawn normally.
	// z: [Shaders.Skin.Debug] LegacyFaceShadows (the shadow reach along the ray off, live, for an A/B: SkinTemplate.hlsl SHADOW_REACH_ALONG_RAY).
	// w: UNOFFICIAL 2026-11-15 DeferredSkinBlur (core/DeferredSkin.h): 1 while the skin blur takes the deferred skin's diffusion, so the deferred light
	// passes leave the curvature diffusion out (Includes/SkinDeferred.hlsl FDSkinSetup), as the forward skin does under NVR_SkinDraw.x. The forward
	// skin shaders do not read w (0 otherwise, as before).
	const SkinScatteringEffect* Blur = TheShaderManager->Effects.SkinScattering;
	const bool deferredBlur = (DeferredSkin::BlurWanted() && Blur && Blur->Enabled && Blur->ScreenSpace && Blur->Supported) || DeferredSkin::BurleyWanted();	// (2026-11-15 + SSS v2's disc)
	Constants.Debug = D3DXVECTOR4(DebugView < FirstScatteringDebugView ? (float)DebugView : 0.0f, isExterior ? 0.0f : 1.0f, LegacyFaceShadows ? 1.0f : 0.0f, deferredBlur ? 1.0f : 0.0f);
	// UNOFFICIAL 2026-11-15 SSS v2 (TESR_SkinSSS, Includes/SkinDeferred.hlsl): the Burley scatter distances in world units (mm x Radius / 14.29 mm
	// a unit) and the Transmission switch (the deferred skin only: DeferredSun's sun through thin parts)
	{
		const float radius = (std::max)(0.1f, TheSettingManager->GetSettingF("Shaders.Skin.SSS", "Radius")) / 14.2857f;
		Constants.SSS = D3DXVECTOR4((std::max)(0.05f, TheSettingManager->GetSettingF("Shaders.Skin.SSS", "ScatterRed")) * radius,
			(std::max)(0.05f, TheSettingManager->GetSettingF("Shaders.Skin.SSS", "ScatterGreen")) * radius,
			(std::max)(0.05f, TheSettingManager->GetSettingF("Shaders.Skin.SSS", "ScatterBlue")) * radius,
			DeferredSkin::WantedAtStartup() && TheSettingManager->GetSettingI("Shaders.Skin.SSS", "Transmission") != 0 ? 1.0f : 0.0f);
	}
}

static void ReadSkinMaterial(SkinShaders::MaterialStruct* Material, const char* Section) {
	Material->SpecularStrength = std::clamp(TheSettingManager->GetSettingF(Section, "SpecularStrength"), 0.0f, 4.0f);
	Material->Roughness = std::clamp(TheSettingManager->GetSettingF(Section, "Roughness"), 0.1f, 1.0f);
	Material->Scattering = std::clamp(TheSettingManager->GetSettingF(Section, "Scattering"), 0.0f, 4.0f);
	Material->Translucency = std::clamp(TheSettingManager->GetSettingF(Section, "Translucency"), 0.0f, 2.0f);
	Material->ShadowScatter = std::clamp(TheSettingManager->GetSettingF(Section, "ShadowScatter"), 0.0f, 2.0f);
}

void SkinShaders::UpdateSettings() {
	ReadSkinMaterial(&Exterior, "Shaders.Skin.Main");
	ReadSkinMaterial(&Interior, "Shaders.Skin.Interiors");
	SkyReflection = std::clamp(TheSettingManager->GetSettingF("Shaders.Skin.Main", "SkyReflection"), 0.0f, 2.0f);
	RainWetness = std::clamp(TheSettingManager->GetSettingF("Shaders.Skin.Main", "RainWetness"), 0.0f, 1.0f);
	VanillaMatchedHighlights = std::clamp(TheSettingManager->GetSettingF("Shaders.Skin.Main", "VanillaMatchedHighlights"), 0.0f, 1.0f);	// UNOFFICIAL 2026-11-05 (PB NVR's skin lighting)
	MuteGameHighlights = TheSettingManager->GetSettingI("Shaders.Skin.Main", "MuteGameHighlights") != 0;
	DebugView = std::clamp(TheSettingManager->GetSettingI("Shaders.Skin.Debug", "DebugView"), 0, DebugViewCount);
	BindFaceGenMaps = TheSettingManager->GetSettingI("Shaders.Skin.Debug", "BindFaceGenMaps") != 0;
	LegacyFaceShadows = TheSettingManager->GetSettingI("Shaders.Skin.Debug", "LegacyFaceShadows") != 0;
}
