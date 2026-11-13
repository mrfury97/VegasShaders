#include "SunGlare.h"

// UNOFFICIAL 2026-11-13 (modern lighting): see SunGlare.h and README.md.

void SunGlareEffect::UpdateSettings() {
	const char* section = "Shaders.SunGlare.Main";
	SettingManager* cfg = TheSettingManager;
	Settings.strength = std::clamp(cfg->GetSettingF(section, "Strength"), 0.0f, 10.0f);
	Settings.haloSize = std::clamp(cfg->GetSettingF(section, "HaloSize"), 0.002f, 0.5f);
	Settings.haloStrength = std::clamp(cfg->GetSettingF(section, "HaloStrength"), 0.0f, 10.0f);
	Settings.spikes = std::clamp(cfg->GetSettingF(section, "Spikes"), 0.0f, 16.0f);
	Settings.spikeLength = std::clamp(cfg->GetSettingF(section, "SpikeLength"), 0.005f, 1.0f);
	Settings.spikeWidth = std::clamp(cfg->GetSettingF(section, "SpikeWidth"), 0.0003f, 0.05f);
	Settings.spikeStrength = std::clamp(cfg->GetSettingF(section, "SpikeStrength"), 0.0f, 10.0f);
	Settings.spikeRotation = cfg->GetSettingF(section, "SpikeRotation");
	Settings.streakLength = std::clamp(cfg->GetSettingF(section, "StreakLength"), 0.01f, 2.0f);
	Settings.streakWidth = std::clamp(cfg->GetSettingF(section, "StreakWidth"), 0.0003f, 0.05f);
	Settings.streakStrength = std::clamp(cfg->GetSettingF(section, "StreakStrength"), 0.0f, 10.0f);
	Settings.chromatic = std::clamp(cfg->GetSettingF(section, "Chromatic"), 0.0f, 1.0f);
	Settings.sunSize = std::clamp(cfg->GetSettingF(section, "SunSize"), 0.002f, 0.1f);
	Settings.cloudLow = (std::max)(cfg->GetSettingF(section, "CloudContrastLow"), 0.0f);
	Settings.cloudHigh = (std::max)(cfg->GetSettingF(section, "CloudContrastHigh"), Settings.cloudLow + 0.01f);
	Settings.sunsetStrength = std::clamp(cfg->GetSettingF(section, "SunsetStrength"), 0.0f, 4.0f);
	Settings.easeFrames = std::clamp(cfg->GetSettingF(section, "EaseFrames"), 1.0f, 60.0f);
	Settings.debugView = (int)cfg->GetSettingF(section, "DebugView");
	if (!Enabled) Constants.Data.x = 0.0f;	// a disabled effect draws nothing
}

void SunGlareEffect::UpdateConstants() {
	ShaderManager* sm = TheShaderManager;
	const SettingsStruct& s = Settings;

	// The sun's height (TESR_SunPosition.z: 0 at the horizon, 1 at the zenith): the glare fades in over the first few degrees, and
	// SunsetStrength scales it while the sun is low (a low sun shines through far more air: weaker, redder, as its colour already is).
	const float height = sm->ShaderConst.SunPosition.z;
	const float horizon = std::clamp(height / 0.06f, 0.0f, 1.0f);
	const float low = std::clamp(1.0f - height / 0.35f, 0.0f, 1.0f);
	const float sunset = 1.0f + (s.sunsetStrength - 1.0f) * low * low;
	const float day = std::clamp(sm->GameState.dayLight * 2.0f - 1.0f, 0.0f, 1.0f);	// with the god rays' day gate (dayLight > 0.5)
	Constants.Data = D3DXVECTOR4(s.strength * horizon * sunset * day, s.haloSize, s.haloStrength, (float)s.debugView);

	const float lines = floorf(s.spikes * 0.5f + 0.5f);	// each line through the sun makes two spikes
	Constants.Spikes = D3DXVECTOR4(lines, s.spikeLength, s.spikeWidth, lines > 0.0f ? s.spikeStrength : 0.0f);
	Constants.Streak = D3DXVECTOR4(s.streakLength, s.streakWidth, s.streakStrength, s.chromatic);
	Constants.Sun = D3DXVECTOR4(s.sunSize, s.cloudLow, s.cloudHigh, 1.0f / s.easeFrames);
	const float first = s.spikeRotation * 0.01745329252f;
	const float step = lines > 0.0f ? 3.14159265359f / lines : 0.0f;
	Constants.Turn = D3DXVECTOR4(cosf(first), sinf(first), cosf(step), sinf(step));
}

void SunGlareEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SunGlareData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_SunGlareSpikes", &Constants.Spikes);
	TheShaderManager->RegisterConstant("TESR_SunGlareStreak", &Constants.Streak);
	TheShaderManager->RegisterConstant("TESR_SunGlareSun", &Constants.Sun);
	TheShaderManager->RegisterConstant("TESR_SunGlareTurn", &Constants.Turn);
}

// Outdoors, by day, above water, with the sun in front of the camera (its view-space direction's z) and some strength left.
bool SunGlareEffect::ShouldRender() {
	ShaderManager* sm = TheShaderManager;
	if (!sm->GameState.isExterior || sm->GameState.isUnderwater || sm->GameState.dayLight <= 0.5f) return false;
	if (sm->ShaderConst.ViewSpaceLightDir.z <= 0.05f) return false;
	return Constants.Data.x > 0.0f;
}

void SunGlareEffect::RegisterTextures() {
	TheTextureManager->InitTexture("TESR_SunGlareCover", &coverTexture, &coverSurface, 1, 1, D3DFMT_A16B16G16R16F);
}

// The cover measure into the one-texel target. The pass's own states blend it over what the texel held (alpha = the share a frame
// contributes), so the value eases in and out over a few frames. After a gap (the sun out of view, indoors) it starts from zero.
bool SunGlareEffect::RunCover(IDirect3DDevice9* Device) {
	D3DXHANDLE technique = TechniqueHandle(techCover, "SunGlareCover");
	if (!technique || !coverSurface) return false;
	const DWORD frame = (DWORD)TheFrameRateManager->FrameNumber;
	if (frame - coverFrame > 1) Device->ColorFill(coverSurface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
	coverFrame = frame;

	IDirect3DSurface9* target = nullptr;
	D3DVIEWPORT9 viewport = {};
	Device->GetRenderTarget(0, &target);
	Device->GetViewport(&viewport);
	SetCT();
	Device->SetTexture(2, nullptr);	// the cover texture itself (the picture pass's s2) is the target here
	UINT passes = 0;
	HRESULT result = Effect->SetTechnique(technique);
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		const D3DVIEWPORT9 one = { 0, 0, 1, 1, 0.0f, 1.0f };
		Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		result = Device->SetRenderTarget(0, coverSurface);
		if (SUCCEEDED(result)) result = Device->SetViewport(&one);
		if (SUCCEEDED(result)) result = Effect->BeginPass(0);
		if (SUCCEEDED(result)) {
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			const HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	if (target) { Device->SetRenderTarget(0, target); target->Release(); }
	Device->SetViewport(&viewport);
	return SUCCEEDED(result);
}

void SunGlareEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (Enabled && Effect && ShouldRender()) {
		static GpuTimer coverTimer("  Sun glare cover");
		GpuProfileScope gpu(coverTimer, Device);
		RunCover(Device);
	}
	EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
}
