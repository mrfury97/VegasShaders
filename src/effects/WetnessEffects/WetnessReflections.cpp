#include <algorithm>

#include "WetnessReflections.h"
#include "../../core/FdIndirect.h"	// UNOFFICIAL 2026-11-18 DeferredBumpedNormals

void WetnessReflectionsEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_WetnessReflectionData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_WetnessReflectionFade", &Constants.Fade);
	TheShaderManager->RegisterConstant("TESR_WetnessReflectionBump", &Constants.Bump);	// UNOFFICIAL 2026-11-18 DeferredBumpedNormals
}

// Part of Wetness Effects: [Shaders.WetnessEffects.Reflections], switched on there. It has no Status of its
// own, so the effect always loads and ShouldRender decides.
void WetnessReflectionsEffect::UpdateSettings() {
	const char* main = "Shaders.WetnessEffects.Reflections";
	Settings.Enabled = TheSettingManager->GetSettingI(main, "Enabled") != 0;
	Settings.Strength = std::clamp(TheSettingManager->GetSettingF(main, "Strength"), 0.0f, 2.0f);
	Settings.MaxDistance = std::clamp(TheSettingManager->GetSettingF(main, "MaxDistance"), 100.0f, 10000.0f);
	Settings.Thickness = std::clamp(TheSettingManager->GetSettingF(main, "Thickness"), 1.0f, 500.0f);
	Settings.Steps = (float)std::clamp(TheSettingManager->GetSettingI(main, "Steps"), 4, 64);
	Settings.RoughnessCutoff = std::clamp(TheSettingManager->GetSettingF(main, "RoughnessCutoff"), 0.05f, 1.0f);
	Settings.EdgeFade = std::clamp(TheSettingManager->GetSettingF(main, "EdgeFade"), 0.01f, 0.5f);
	Settings.MaxCameraDistance = std::clamp(TheSettingManager->GetSettingF(main, "MaxCameraDistance"), 500.0f, 50000.0f);
	Settings.DebugView = TheSettingManager->GetSettingI(main, "DebugView") != 0;
}

void WetnessReflectionsEffect::UpdateConstants() {
	Constants.Data = D3DXVECTOR4(Settings.Strength, Settings.MaxDistance, Settings.Thickness, Settings.Steps);
	Constants.Fade = D3DXVECTOR4(Settings.RoughnessCutoff, Settings.EdgeFade, Settings.DebugView ? 1.0f : 0.0f, Settings.MaxCameraDistance);
}

// Only while something is wet: Wetness Effects on and active (its flag, set while anything is wet or
// a shore is near), its Reflections on, outdoors and above water.
// The debug view runs outdoors whatever the weather, so it shows that nothing is wet too.
bool WetnessReflectionsEffect::ShouldRender() {
	if (!Settings.Enabled) return false;
	// VegasShaders: the effect takes the sky the forward shaders reflected as the spherical harmonic one (SkylightingMode 0, the
	// default; Effects/WetnessReflections.fx.hlsl).
	if (TheSettingManager->GetSettingI("Shaders.PBR.Main", "SkylightingMode") != 0) return false;
	if (!TheShaderManager->GameState.isExterior || TheShaderManager->GameState.isUnderwater) return false;
	WetnessEffectsShaders* Wetness = TheShaderManager->Shaders.Wetness;
	if (!Wetness || !Wetness->Enabled) return false;
	if (Settings.DebugView) return true;
	return Wetness->Constants.Flags.x > 0.5f && Settings.Strength > 0.0f;
}

// UNOFFICIAL 2026-11-18 [Main.Main.Quality] DeferredBumpedNormals (live, test only, off; core/FdIndirect.h, docs/bumped-normals-study.md):
// the shader's NVR_WetShadingNormals (s4) has no texture slot, so it is set here before every Render (EffectChain): the G-buffer's
// normal-mapped shading normal while the switch is on, else TESR_NormalsBuffer itself; TESR_WetnessReflectionBump.x says which (0: the
// shader takes exactly its old path). The caller clears the stage after the Render (ClearPassTextures).
void WetnessReflectionsEffect::BindShadingNormals(IDirect3DDevice9* Device) {
	Constants.Bump.x = 0.0f;
	if (!Enabled || !Effect || !ShouldRender()) return;	// Render draws nothing: no shading-normals pass for it either
	std::string normalsName = "TESR_NormalsBuffer", depthName = "TESR_DepthBuffer";
	IDirect3DBaseTexture9** normalsSlot = TheTextureManager->GetTextureSlotByName(normalsName);
	IDirect3DBaseTexture9* normals = normalsSlot ? *normalsSlot : nullptr;
	if (!normals) return;
	IDirect3DBaseTexture9* shading = normals;
	if (FdIndirect::BumpedNormalsOn()) {
		IDirect3DBaseTexture9** depth = TheTextureManager->GetTextureSlotByName(depthName);
		shading = FdIndirect::ShadingNormals(Device, normals, depth ? *depth : nullptr);
	}
	Constants.Bump.x = shading != normals ? 1.0f : 0.0f;
	SetPassTexture(4, shading);	// set after each pass begins (EffectRecord::PassTextures)
}
