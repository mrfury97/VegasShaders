// UNOFFICIAL: ported from Physically-Based-Reloaded (github.com/Balebandro/Physically-Based-Reloaded, testbed branch,
// GPL-3.0, the same licence as this project). Credit its authors when shipping.

#include <algorithm>

#include "HeatHaze.h"
#include "../../core/FrameClock.h"
#include "../../core/RainState.h"	// UNOFFICIAL 2026-11-12 RainStateGuard
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)

void HeatHazeEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_HeatHazeData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_HeatHazeNoise", &Constants.Noise);
}

void HeatHazeEffect::UpdateSettings() {
	Settings.Strength = max(0.0f, TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "Strength"));
	Settings.StartDistance = max(0.0f, TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "StartDistance"));
	Settings.FullDistance = max(Settings.StartDistance + 1.0f, TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "FullDistance"));
	Settings.HorizonBand = std::clamp(TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "HorizonBand"), 0.02f, 1.0f);
	Settings.NoiseScale = max(1.0f, TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "NoiseScale"));
	Settings.Speed = max(0.0f, TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "Speed"));
	Settings.PeakHour = std::clamp(TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "PeakHour"), 8.0f, 18.0f);
	Settings.CloudyScale = std::clamp(TheSettingManager->GetSettingF("Shaders.HeatHaze.Main", "CloudyScale"), 0.0f, 1.0f);
}

static float SmoothStep(float edge0, float edge1, float x) {
	float t = std::clamp((x - edge0) / max(edge1 - edge0, 1e-4f), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void HeatHazeEffect::UpdateConstants() {
	// UNOFFICIAL 2026-10-13: once a frame, at the tuned rate (FrameClock.h: UpdateConstants runs twice a frame).
	static double lastFrameTime = -1.0;
	float elapsed = (float)(ElapsedOncePerFrame(lastFrameTime) * TunedClockRate);

	// Time of day: nothing before the sun is fully up, building to the hottest hour, gone by sunset.
	float hour = TheShaderManager->ShaderConst.GameTime.y;
	float sunriseEnd = TheShaderManager->ShaderConst.SunTiming.y;
	float sunsetStart = TheShaderManager->ShaderConst.SunTiming.z;
	float peak = std::clamp(Settings.PeakHour, sunriseEnd + 1.0f, sunsetStart - 1.0f);
	float dayHeat = hour < peak ? SmoothStep(sunriseEnd, peak, hour) : 1.0f - SmoothStep(peak, sunsetStart, hour);

	// Weather: clear skies full, cloudy less, rain and snow none; and none while the ground is wet.
	float weatherTarget = 1.0f;
	if (TheShaderManager->GameState.isCloudy) weatherTarget = Settings.CloudyScale;
	if (TheShaderManager->GameState.isRainy || TheShaderManager->GameState.isSnow) weatherTarget = 0.0f;
	WetWorldEffect* wet = TheShaderManager->Effects.WetWorld;
	if (wet) weatherTarget *= 1.0f - std::clamp(RainState::Factor(), 0.0f, 1.0f);	// UNOFFICIAL 2026-11-12 RainStateGuard (core/RainState.h; off: the old max of WetWorld's Data.x and z)
	// Eased over about ten seconds, so a weather change fades the haze rather than switching it.
	float ease = min(1.0f, elapsed / 10.0f);
	WeatherHeat += (weatherTarget - WeatherHeat) * ease;

	Heat = dayHeat * WeatherHeat;
	if (!TheShaderManager->GameState.isExterior || TheShaderManager->GameState.isUnderwater) Heat = 0.0f;

	// The offset in uv, sized so a given Strength looks the same at any resolution (pixels at 1080p).
	Constants.Data.x = Settings.Strength * Heat / 1080.0f;
	Constants.Data.y = Settings.StartDistance;
	Constants.Data.z = Settings.FullDistance;
	Constants.Data.w = Settings.HorizonBand;

	Constants.Noise.x = Settings.NoiseScale;
	Constants.Noise.y = Settings.Speed;
	Constants.Noise.z = fmodf(Constants.Noise.z + elapsed, 3600.0f);
	Constants.Noise.w = Heat;
}

bool HeatHazeEffect::ShouldRender() {
	return Constants.Data.x > 1e-6f;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): HeatHaze.frag (EffectRecord::ExtRender). Two samplers (LINEAR for the image, POINT for the
// depth), so this calls ExtFullScreen with its own bindings instead of ExtTexturePass. The push block is 104 bytes: the TESR_
// float4s, the matrix terms toWorld reads (view columns 0-2, the projection's [0][0] and [1][1]), farZ and the aspect ratio;
// the two sampler heap indices at 96 and 100 are written by the extension.
bool HeatHazeEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 heatHazeData, heatHazeNoise, misc, viewCol0, viewCol1, viewCol2; UInt32 linearIndex; UInt32 pointIndex; } push = {};
	if (!extPass.Tried) {
		extPass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[4] = {};
		bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0; bindings[0].ViewType = NVR_EXT_VIEW_2D;	// TESR_RenderedBuffer
		bindings[1].Set = 1; bindings[1].Binding = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].Slot = 1; bindings[1].ViewType = NVR_EXT_VIEW_2D;	// TESR_DepthBuffer
		bindings[2].Type = NVR_EXT_BIND_SAMPLER; bindings[2].Slot = 0; bindings[2].PushOffset = 96;	// linearIndex
		bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].Slot = 1; bindings[3].PushOffset = 100;	// pointIndex
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("HeatHaze", bindings, _countof(bindings), sizeof(Push), true);
	}
	IDirect3DBaseTexture9* rendered = TheTextureManager->RenderedTexture;
	IDirect3DBaseTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!extPass.Pipeline || !rendered || !depth || !linear || !point) return false;

	const D3DXMATRIX& V = TheRenderManager->viewMatrix;	// the matrices as the shader reads them (ShaderManager's camera comment)
	const D3DXMATRIX& P = TheRenderManager->projMatrix;
	push.heatHazeData = Constants.Data;
	push.heatHazeNoise = Constants.Noise;
	push.misc = D3DXVECTOR4(P._11, P._22, TheRenderManager->CameraData.y, TheShaderManager->ShaderConst.ReciprocalResolution.z);
	push.viewCol0 = D3DXVECTOR4(V._11, V._21, V._31, 0.0f);
	push.viewCol1 = D3DXVECTOR4(V._12, V._22, V._32, 0.0f);
	push.viewCol2 = D3DXVECTOR4(V._13, V._23, V._33, 0.0f);

	NVR_EXT_RESOURCE_BINDING resources[4] = {};
	resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = rendered;
	resources[1].Slot = 1; resources[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[1].pResource = depth;
	resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_SAMPLER; resources[2].pResource = linear;
	resources[3].Slot = 1; resources[3].Type = NVR_EXT_BIND_SAMPLER; resources[3].pResource = point;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass.Pipeline, resources, _countof(resources), &push, sizeof(push));
}
