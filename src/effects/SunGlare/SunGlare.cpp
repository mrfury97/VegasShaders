#include "../../core/DxvkCompute.h"
#include "SunGlare.h"

// UNOFFICIAL 2026-11-13 (modern lighting): see SunGlare.h and docs/EFFECTS.md "SunGlare".

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

struct SunGlareUniforms {
	float projectionTransform[16];
	float reciprocalResolution[4];
	float sunColor[4];
	float sunsetColor[4];
	float sunDirection[4];
	float sunAmount[4];
	float viewSpaceLightDir[4];
	float cameraData[4];
	float renderScale[4];
	float glareData[4];
	float glareSpikes[4];
	float glareStreak[4];
	float glareSun[4];
	float glareTurn[4];
};

struct SunGlarePush {
	UInt32 linearSampler;
	UInt32 pointSampler;
	UInt32 pad[2];
};

static IDirect3DBaseTexture9* SunGlareNamedTex(const char* name) {
	auto found = TheTextureManager->TextureNames.find(name);
	return (found == TheTextureManager->TextureNames.end() || !found->second) ? nullptr : *found->second;
}

static ID3D9NvrExtPipeline* SunGlarePipeline(EffectRecord::ExtPass& pass, const char* spvName, NVR_EXT_BLEND blend) {
	if (!pass.Tried) {
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[5] = {};
		bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0; bindings[0].ViewType = NVR_EXT_VIEW_2D;
		bindings[1].Set = 1; bindings[1].Binding = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].Slot = 1; bindings[1].ViewType = NVR_EXT_VIEW_2D;
		bindings[2].Set = 2; bindings[2].Binding = 0; bindings[2].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[2].Slot = 0; bindings[2].ViewType = NVR_EXT_VIEW_2D;
		bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].Slot = 0; bindings[3].PushOffset = 0;
		bindings[4].Type = NVR_EXT_BIND_SAMPLER; bindings[4].Slot = 1; bindings[4].PushOffset = 4;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, _countof(bindings), sizeof(SunGlarePush), true, 1, blend);
	}
	return pass.Pipeline;
}

bool SunGlareEffect::FillUniforms(SunGlareUniforms& u) {
	memset(&u, 0, sizeof(u));
	bool ok = true;
	auto copyConst = [&](const char* name, void* dst, size_t bytes) {
		auto it = TheShaderManager->ConstantsTable.find(name);
		if (it != TheShaderManager->ConstantsTable.end() && it->second) memcpy(dst, it->second, bytes);
		else ok = false;
	};
	copyConst("TESR_ProjectionTransform", u.projectionTransform, sizeof(u.projectionTransform));
	copyConst("TESR_ReciprocalResolution", u.reciprocalResolution, sizeof(u.reciprocalResolution));
	copyConst("TESR_SunColor", u.sunColor, sizeof(u.sunColor));
	copyConst("TESR_SunsetColor", u.sunsetColor, sizeof(u.sunsetColor));
	copyConst("TESR_SunDirection", u.sunDirection, sizeof(u.sunDirection));
	copyConst("TESR_SunAmount", u.sunAmount, sizeof(u.sunAmount));
	copyConst("TESR_ViewSpaceLightDir", u.viewSpaceLightDir, sizeof(u.viewSpaceLightDir));
	copyConst("TESR_CameraData", u.cameraData, sizeof(u.cameraData));
	auto rs = TheShaderManager->ConstantsTable.find("TESR_RenderScale");
	if (rs != TheShaderManager->ConstantsTable.end() && rs->second) memcpy(u.renderScale, rs->second, sizeof(u.renderScale));
	else { u.renderScale[0] = u.renderScale[1] = u.renderScale[2] = u.renderScale[3] = 1.0f; }
	memcpy(u.glareData, &Constants.Data, sizeof(u.glareData));
	memcpy(u.glareSpikes, &Constants.Spikes, sizeof(u.glareSpikes));
	memcpy(u.glareStreak, &Constants.Streak, sizeof(u.glareStreak));
	memcpy(u.glareSun, &Constants.Sun, sizeof(u.glareSun));
	memcpy(u.glareTurn, &Constants.Turn, sizeof(u.glareTurn));
	return ok;
}

bool SunGlareEffect::ExtRunCover(IDirect3DDevice9* Device) {
	if (!DxvkCompute::Available() || !coverSurface) return false;
	IDirect3DBaseTexture9* rendered = TheTextureManager->RenderedTexture;
	IDirect3DBaseTexture9* depth = SunGlareNamedTex("TESR_DepthBuffer");
	if (!rendered || !depth) return false;
	SunGlareUniforms uniforms;
	if (!FillUniforms(uniforms)) return false;
	ID3D9NvrExtBuffer* cb = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	ID3D9NvrExtPipeline* pipeline = SunGlarePipeline(extCover, "SunGlareCover", NVR_EXT_BLEND_ALPHA);
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!cb || !pipeline || !linear || !point) return false;
	const DWORD frame = (DWORD)TheFrameRateManager->FrameNumber;
	if (frame - coverFrame > 1) Device->ColorFill(coverSurface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
	coverFrame = frame;
	NVR_EXT_RESOURCE_BINDING resources[5] = {};
	resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = rendered;
	resources[1].Slot = 1; resources[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[1].pResource = depth;
	resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[2].pResource = cb;
	resources[3].Slot = 0; resources[3].Type = NVR_EXT_BIND_SAMPLER; resources[3].pResource = linear;
	resources[4].Slot = 1; resources[4].Type = NVR_EXT_BIND_SAMPLER; resources[4].pResource = point;
	SunGlarePush push = {};
	return ExtDrawTo(pipeline, &coverSurface, 1, resources, _countof(resources), &push, sizeof(push), nullptr, ExtUv::ScreenQuad);
}

bool SunGlareEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || ClearRenderTarget || !DxvkCompute::Available()) return false;
	IDirect3DBaseTexture9* rendered = TheTextureManager->RenderedTexture;
	if (!rendered || !coverTexture) return false;
	SunGlareUniforms uniforms;
	if (!FillUniforms(uniforms)) return false;
	ID3D9NvrExtBuffer* cb = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	ID3D9NvrExtPipeline* pipeline = SunGlarePipeline(extPass, "SunGlare", NVR_EXT_BLEND_NONE);
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!cb || !pipeline || !linear || !point) return false;
	NVR_EXT_RESOURCE_BINDING resources[5] = {};
	resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = rendered;
	resources[1].Slot = 1; resources[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[1].pResource = coverTexture;
	resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[2].pResource = cb;
	resources[3].Slot = 0; resources[3].Type = NVR_EXT_BIND_SAMPLER; resources[3].pResource = linear;
	resources[4].Slot = 1; resources[4].Type = NVR_EXT_BIND_SAMPLER; resources[4].pResource = point;
	SunGlarePush push = {};
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, pipeline, resources, _countof(resources), &push, sizeof(push), true);
}

void SunGlareEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (Enabled && Effect && ShouldRender()) {
		static GpuTimer coverTimer("  Sun glare cover");
		GpuProfileScope gpu(coverTimer, Device);
		if (ExtFailed || !DxvkCompute::PassesOnRect() || DxvkCompute::KeptOnD3D9(Name) || !ExtRunCover(Device)) {
			RunCover(Device);
		}
	}
	EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
}
