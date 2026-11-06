#include "VolumetricLight.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
extern D3DXVECTOR4 NvrGameMultisample;	// UNOFFICIAL 2026-11-05 Hooks/Settings.cpp (the game's MSAA samples)
#include "../../core/Diagnostics.h"	// UNOFFICIAL 2026-11-01 the report burst's VL stages (only while Diagnostics::VLCapturing)

static void ReadLook(const char* section, VolumetricLightEffect::LookStruct& look) {
	look.Strength = std::clamp(TheSettingManager->GetSettingF(section, "Strength"), 0.0f, 10.0f);
	look.Darken = std::clamp(TheSettingManager->GetSettingF(section, "Darken"), 0.0f, 1.0f);
	look.Density = std::clamp(TheSettingManager->GetSettingF(section, "Density"), 0.0f, 10.0f);
	look.MaxDistance = std::clamp(TheSettingManager->GetSettingF(section, "MaxDistance"), 100.0f, 30000.0f);
	look.Anisotropy = std::clamp(TheSettingManager->GetSettingF(section, "Anisotropy"), 0.0f, 0.9f);
}

void VolumetricLightEffect::UpdateSettings() {
	// UNOFFICIAL 2026-10-18: the frames drawn with the settings that are about to be replaced. 2026-10-19: only while the F10
	// profile runs (as the 240-frame report): every settings change reruns this, so dragging a menu slider wrote dozens of lines.
	if (GpuTimer::Enabled) ReportHistory("settings changed");
	const char* section = "Shaders.VolumetricLight.Main";
	ReadLook(section, Settings.Day);
	ReadLook("Shaders.VolumetricLight.Night", Settings.Night);
	Settings.Steps = std::clamp(TheSettingManager->GetSettingI(section, "Steps"), 4, 48);
	Settings.Temporal = std::clamp(TheSettingManager->GetSettingF(section, "Temporal"), 0.0f, 0.97f);
	Settings.BlurRadius = std::clamp(TheSettingManager->GetSettingF(section, "BlurRadius"), 0.0f, 8.0f);
	Settings.RainStrength = std::clamp(TheSettingManager->GetSettingF(section, "RainStrength"), 0.0f, 1.0f);
	Settings.Froxels = TheSettingManager->GetSettingI(section, "Froxels") != 0;
	Settings.FogVolume = TheSettingManager->GetSettingI("Shaders.VolumetricFog.Main", "FogVolume") != 0;
}

void VolumetricLightEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData2", &Constants.Data2);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightData3", &Constants.Data3);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightLayout", &Constants.Layout);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightReproject", (D3DXVECTOR4*)&Constants.Reproject);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightGridLayout", &Constants.GridLayout);
	TheShaderManager->RegisterConstant("TESR_VolumetricLightGridData", &Constants.GridData);
}

void VolumetricLightEffect::RegisterTextures() {
	const UINT w = MaxQuality::Reduced(TheRenderManager->width, 2), h = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("TESR_VolumetricLightMarch", &Textures.MarchTexture, &Textures.MarchSurface, w, h, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_VolumetricLightBuffer", &Textures.BlurTexture, &Textures.BlurSurface, w, h, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_VolumetricLightTemp", &Textures.TempTexture, &Textures.TempSurface, w, h, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_VolumetricLightHistory", &Textures.HistoryTexture, &Textures.HistorySurface, w, h, D3DFMT_A16B16G16R16F);
	Constants.Layout = D3DXVECTOR4(1.0f / w, 1.0f / h, 0.0f, 0.0f);

	// UNOFFICIAL 2026-10-14 froxel grid: one column per 16 x 16 pixels, its 64 slices in 8 x 8 tiles (1280 x 720 at 1440p,
	// 7.4 MB each). Made whatever the setting, so it can be switched on in game.
	const UINT gw = (TheRenderManager->width + 15) / 16, gh = (TheRenderManager->height + 15) / 16;
	TheTextureManager->InitTexture("TESR_VolumetricLightGrid", &Textures.GridTexture, &Textures.GridSurface, gw * 8, gh * 8, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_VolumetricLightGridTemp", &Textures.GridTempTexture, &Textures.GridTempSurface, gw * 8, gh * 8, D3DFMT_A16B16G16R16F);
	Constants.GridLayout = D3DXVECTOR4((float)gw, (float)gh, 1.0f / (gw * 8), 1.0f / (gh * 8));
}

// UNOFFICIAL 2026-10-18: one line on what the march's history did since the last report (only when it drew; the froxel grid has none).
void VolumetricLightEffect::ReportHistory(const char* when) {
	if (!stats.drawn) return;
	char text[256];
	stats.Describe(text, sizeof(text));
	Logger::Log("UNOFFICIAL VolumetricLight temporal history (%s, Temporal %.2f): %s", when, Settings.Temporal, text);
	stats.Reset();
}

void VolumetricLightEffect::UpdateConstants() {
	// Day and night looks, blended by the time of day (GameState.isDayTime: 1 day, 0 night, smooth through dawn/dusk).
	const float day = std::clamp(TheShaderManager->GameState.isDayTime, 0.0f, 1.0f);
	const LookStruct& d = Settings.Day;
	const LookStruct& n = Settings.Night;

	// Fade: the sun shadows' own fade (ShadowFade.x, 1 = no shadows: sunrise, sunset, a dim moon), less in rain, as set.
	const ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	float fade = 1.0f - std::clamp(shadows->Constants.ShadowFade.x, 0.0f, 1.0f);
	fade *= std::lerp(1.0f, Settings.RainStrength, std::clamp(TheShaderManager->Effects.Rain->Constants.RainAnimator.GetValue(), 0.0f, 1.0f));

	Constants.Data.x = std::lerp(n.Strength, d.Strength, day);
	Constants.Data.y = std::lerp(n.Darken, d.Darken, day);
	Constants.Data.z = std::lerp(n.Density, d.Density, day) * 0.001f;
	Constants.Data.w = std::lerp(n.MaxDistance, d.MaxDistance, day);
	Constants.Data2.x = (float)Settings.Steps;
	Constants.Data2.y = std::lerp(n.Anisotropy, d.Anisotropy, day);
	Constants.Data2.z = fade;
	Constants.Data3.y = Settings.BlurRadius;

	// The grid's lit share is scaled to about 1 for a view through the whole grid (the half-float grid would round the
	// small per-cell values of thin air away), and divided back out where it is read.
	const float depth = Constants.Data.z * Constants.Data.w;
	Constants.GridData.y = depth > 1.0e-4f ? 1.0f / (1.0f - expf(-depth)) : 1.0f;
	VolumetricFogEffect* fog = TheShaderManager->Effects.VolumetricFog;
	// (UNOFFICIAL 2026-10-18: not with ClassicLook or the FogReference diagnostic either: that fog reads none of the fog volume.)
	Constants.GridData.x = Settings.FogVolume && fog && fog->Enabled && !fog->IsLegacy() && !fog->IsClassic() && !fog->referenceTest ? 1.0f : 0.0f;
}

bool VolumetricLightEffect::ShouldRender() {
	const ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	return TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater &&
		shadows && shadows->Constants.ShadowFade.y > 0.0f &&	// the sun's shadow maps are drawn this frame
		Constants.Data2.z > 0.001f && (Constants.Data.x > 0.0f || Constants.Data.y > 0.0f) &&
		Textures.MarchSurface && Textures.BlurSurface && Textures.TempSurface && Textures.HistorySurface;
}

// The half-resolution passes, into the effect's own buffers (techniques: 1 March, 3 Temporal, 2 Blur X, 4 Blur Y).
// With Temporal on: March -> March buffer, Temporal -> Temp (copied to History for next frame), Blur X -> March,
// Blur Y -> Buffer. Off: March -> Temp, then the same blurs. The composite (technique 0) then reads Buffer.
void VolumetricLightEffect::RenderBuffers(IDirect3DDevice9* Device) {
	// UNOFFICIAL 2026-11-01 the report burst's VL stages (core/Diagnostics.cpp): while a burst captures, this frame's values and each
	// stage's output (shrunk on the GPU into the burst frame's slot). Outside a burst this is false and nothing of it runs.
	const bool report = Diagnostics::VLCapturing;
	if (CompositeTechnique() == 8) {
		if (report) {	// (the grid has no march stages: only that it ran)
			Diagnostics::VLValues values;
			values.technique = 8; values.fade = Constants.Data2.z; values.strength = Constants.Data.x; values.darken = Constants.Data.y;
			values.rect = RenderRect::Active(); values.scaleChanged = RenderRect::ScaleChanged();
			Diagnostics::VLNote(values);
		}
		// UNOFFICIAL 2026-10-14 froxel grid: light the cells, then the two summing passes (techniques 5, 6, 7).
		static GpuTimer gridTimers[3] = { GpuTimer("  VL grid light"), GpuTimer("  VL grid sum rows"), GpuTimer("  VL grid sum carry") };
		auto gridPass = [&](IDirect3DSurface9* target, UINT technique) {
			GpuProfileScope gpuPass(gridTimers[technique - 5], Device);
			Device->SetRenderTarget(0, target);
			Render(Device, target, target, technique, false, target);
		};
		// The fog's 3D noise on stage 9 (NVR_FogNoise) while the cells are lit, for the fog volume.
		if (!fogNoiseLoaded) {
			fogNoiseLoaded = true;
			fogNoise = TheTextureManager->GetFileTexture("Data\\Textures\\Effects\\NoiseVolume.dds", TextureRecord::TextureRecordType::VolumeBuffer);
		}
		IDirect3DBaseTexture9* noise = fogNoise;
		const bool bindNoise = Constants.GridData.x > 0.5f && noise;
		if (bindNoise) {
			Device->SetTexture(9, noise);
			TheRenderManager->renderState->NoteDeviceTexture(9, noise);
			const DWORD states[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP},
				{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_LINEAR} };
			for (const auto& s : states) TheRenderManager->SetSamplerState(9, (D3DSAMPLERSTATETYPE)s[0], s[1]);
		}
		gridPass(Textures.GridSurface, 5);
		if (bindNoise) {
			Device->SetTexture(9, nullptr);
			TheRenderManager->renderState->NoteDeviceTexture(9, nullptr);
		}
		gridPass(Textures.GridTempSurface, 6);
		gridPass(Textures.GridSurface, 7);
		haveHistory = false;	// the march's history is stale when it is switched back
		lastRenderedFrame = TheFrameRateManager->FrameNumber;
		return;
	}
	const bool temporal = Settings.Temporal > 0.0f;
	const D3DXMATRIX& view = TheRenderManager->viewMatrix;
	const D3DXMATRIX& invView = TheRenderManager->InvViewMatrix;

	// Last frame's result is usable if this effect drew it, and the camera did not jump (load, fast travel, a cut).
	// UNOFFICIAL 2026-10-18: counted in rendered frames (FrameNumber), not UpdateConstants calls (two a frame, which made this never pass).
	const UINT now = TheFrameRateManager->FrameNumber;
	const bool drewLastFrame = lastRenderedFrame + 1 == now;
	bool usable = temporal && haveHistory && drewLastFrame;
	// UNOFFICIAL 2026-11-01 SharedNoise: and the shared history rule (core/TemporalBackbone.h), the checks below stay
	const bool sharedNoise = TemporalBackbone::Shared();
	if (sharedNoise && !TemporalBackbone::HistoryValid()) usable = false;
	// UNOFFICIAL 2026-10-27 RenderScale v2: a history drawn at another scale (or native vs rect) holds other texels
	if (RenderRect::ScaleChanged() || RenderRect::Active() != lastRect) usable = false;
	lastRect = RenderRect::Active();
	if (usable) {
		const double dx = (double)invView._41 - lastInvView._41, dy = (double)invView._42 - lastInvView._42,
			dz = (double)invView._43 - lastInvView._43;
		usable = dx * dx + dy * dy + dz * dz < 500.0 * 500.0;
	}
	stats.Note(temporal, haveHistory, drewLastFrame, usable);
	EffectStatus::History(StatusSlot, Name, EffectStatus::HistoryWhy(temporal, haveHistory, drewLastFrame, usable));	// UNOFFICIAL 2026-11-01
	if (GpuTimer::Enabled && stats.drawn >= 240) ReportHistory("240 frames");
	if (usable) {
		// Camera-relative world position p -> last frame's view: (p + cameraNow - cameraLast) * Rlast, then last frame's
		// projection. Worked out round the two camera positions in double precision (the Mojave's coordinates run past
		// 100 000 units, where float rounding would lose the few units the camera moves between frames).
		const double move[3] = { (double)invView._41 - lastInvView._41, (double)invView._42 - lastInvView._42,
			(double)invView._43 - lastInvView._43 };
		D3DXMATRIX toLastView;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) toLastView.m[i][j] = lastView.m[i][j];
			toLastView.m[i][3] = 0.0f;
		}
		for (int j = 0; j < 3; j++)
			toLastView.m[3][j] = (float)(move[0] * lastView.m[0][j] + move[1] * lastView.m[1][j] + move[2] * lastView.m[2][j]);
		toLastView.m[3][3] = 1.0f;
		D3DXMatrixMultiply(&Constants.Reproject, &toLastView, &lastProj);
	}
	Constants.Data3.x = usable ? Settings.Temporal : 0.0f;
	// UNOFFICIAL 2026-11-01 SharedNoise: with Temporal the shared index (own offset); without, the march's noise moves only while TAA /
	// FSR / DLSS averages frames (else frame 0, as before). Off: FrameNumber % 64 with Temporal, 0 without.
	Constants.Data2.w = sharedNoise ? (float)(temporal ? TemporalBackbone::EffectIndex(TemporalBackbone::OffsetVolumetricLight, 64) : TemporalBackbone::NoiseIndex() % 64) :
		temporal ? (float)(now % 64) : 0.0f;
	if (report) {	// UNOFFICIAL 2026-11-01 report burst: the values this frame's passes use (haveHistory etc. before they are updated below)
		Diagnostics::VLValues values;
		values.noise = Constants.Data2.w; values.weight = Constants.Data3.x; values.fade = Constants.Data2.z;
		values.strength = Constants.Data.x; values.darken = Constants.Data.y;
		values.temporal = temporal; values.usable = usable; values.haveHistory = haveHistory; values.drewLastFrame = drewLastFrame;
		values.sharedNoise = sharedNoise; values.historyValid = TemporalBackbone::HistoryValid();
		values.rect = RenderRect::Active(); values.scaleChanged = RenderRect::ScaleChanged();
		const double mx = (double)invView._41 - lastInvView._41, my = (double)invView._42 - lastInvView._42, mz = (double)invView._43 - lastInvView._43;
		values.cameraMove = sqrt(mx * mx + my * my + mz * mz);
		values.technique = CompositeTechnique();
		Diagnostics::VLNote(values);
	}

	// UNOFFICIAL 2026-10-13: F10 timers per pass (technique 1 march, 3 temporal, 2 / 4 blur).
	static GpuTimer passTimers[5] = { GpuTimer("  VL composite"), GpuTimer("  VL march (half)"), GpuTimer("  VL blur X (half)"),
		GpuTimer("  VL temporal (half)"), GpuTimer("  VL blur Y (half)") };
	auto pass = [&](IDirect3DSurface9* target, UINT technique) {
		GpuProfileScope gpuPass(passTimers[technique], Device);
		Device->SetRenderTarget(0, target);
		extDrew = false;
		Render(Device, target, target, technique, false, target);
		if (report) Diagnostics::VLPass(technique, extDrew);	// UNOFFICIAL 2026-11-01 report burst: its Vulkan pass or its D3D9 technique
	};
	// UNOFFICIAL 2026-11-01 report burst: each stage's output right after its pass (the history before this frame's copy replaces it)
	// The stage probes (EFFECT_PROBE: no-ops unless they run, FlashStages) see the same buffers, so an automatic FLASH lists them too.
	if (temporal) {
		pass(Textures.MarchSurface, 1);
		if (report) {
			Diagnostics::VLCapture(Device, Diagnostics::VLMarch, Textures.MarchSurface);
			Diagnostics::VLCapture(Device, Diagnostics::VLHistory, Textures.HistorySurface);
		}
		EFFECT_PROBE("VolumetricLight march", Textures.MarchSurface);
		EFFECT_PROBE("VolumetricLight history", Textures.HistorySurface);
		pass(Textures.TempSurface, 3);
		if (report) Diagnostics::VLCapture(Device, Diagnostics::VLTemporal, Textures.TempSurface);
		EFFECT_PROBE("VolumetricLight temporal", Textures.TempSurface);
		Device->StretchRect(Textures.TempSurface, NULL, Textures.HistorySurface, NULL, D3DTEXF_NONE);
	}
	else {
		pass(Textures.TempSurface, 1);
		if (report) Diagnostics::VLCapture(Device, Diagnostics::VLMarch, Textures.TempSurface);
		EFFECT_PROBE("VolumetricLight march", Textures.TempSurface);
	}
	pass(Textures.MarchSurface, 2);
	pass(Textures.BlurSurface, 4);
	if (report) Diagnostics::VLCapture(Device, Diagnostics::VLBlur, Textures.BlurSurface);
	EFFECT_PROBE("VolumetricLight blur", Textures.BlurSurface);

	haveHistory = temporal;
	lastRenderedFrame = now;
	lastView = view;
	lastInvView = invView;
	lastProj = TheRenderManager->projMatrix;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): see VolumetricLight.h. The constants are read from the same registered memory the D3D9 path
// uploads (ConstantsTable; SetCT sets the matrices with SetMatrix, the logical matrix), in the order of the GLSL's uniform block
// (Compute/VolumetricLight.glsl): the seven matrices, then the float4s.
namespace {
	const char* const ExtMatrixNames[] = { "TESR_ShadowCameraToLightTransformNear", "TESR_ShadowCameraToLightTransformMiddle",
		"TESR_ShadowCameraToLightTransformFar", "TESR_ShadowCameraToLightTransformLod", "TESR_VolumetricLightReproject",
		"TESR_ViewTransform", "TESR_ProjectionTransform" };
	const char* const ExtVectorNames[] = { "TESR_ShadowNearCenter", "TESR_ShadowMiddleCenter", "TESR_ShadowFarCenter", "TESR_ShadowLodCenter",
		"TESR_ShadowFormatData", "TESR_ShadowBlur", "TESR_SmoothedSunDir", "TESR_SunColor", "TESR_ReciprocalResolution", "TESR_CameraData",
		"TESR_VolumetricLightData", "TESR_VolumetricLightData2", "TESR_VolumetricLightData3", "TESR_VolumetricLightLayout",
		"TESR_VolumetricLightGridLayout", "TESR_VolumetricLightGridData", "TESR_FogData", "TESR_SunAmount", "TESR_GameTime",
		"TESR_VolumetricFogDensity", "TESR_VolumetricFogShape", "TESR_VolumetricFogWind", "TESR_VolumetricFogScatter",
		"TESR_VolumetricFogWeather", "TESR_VolumetricFogNight", "TESR_VolumetricFogNightScatter", "TESR_CameraPosition" };
	struct ExtConstants {
		D3DXMATRIX	Matrices[_countof(ExtMatrixNames)];
		D3DXVECTOR4	Vectors[_countof(ExtVectorNames)];
	};
	// Push block: the samplers' heap indices (POINT/CLAMP, LINEAR/CLAMP, LINEAR/WRAP; filled in by DXVK), then the blur's axis.
	struct ExtPush { UInt32 pointIndex, linearIndex, wrapIndex, pad; D3DXVECTOR2 axis; UInt32 pad2[2]; };

	const D3DXVECTOR4* ExtRegistered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
}

bool VolumetricLightEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (ClearRenderTarget || techniqueIndex > 8) return false;
	// Per technique (0 Composite, 1 March, 2 BlurX, 3 Temporal, 4 BlurY, 5 GridLight, 6 GridRow, 7 GridCarry, 8 CompositeGrid):
	// its shader, and its textures in binding order (set 1, binding i, slot i). All buffers are read POINT/CLAMP except the
	// shadow atlas and the grid (LINEAR/CLAMP) and the fog noise (LINEAR/WRAP), as the .fx declares them.
	static const char* const SpvNames[9] = { "VolumetricLightComposite", "VolumetricLightMarch", "VolumetricLightBlur", "VolumetricLightTemporal",
		"VolumetricLightBlur", "VolumetricLightGridLight", "VolumetricLightGridRow", "VolumetricLightGridCarry", "VolumetricLightCompositeGrid" };
	const bool frame = techniqueIndex == 0 || techniqueIndex == 8;	// the composites write the frame; the rest the effect's own buffers
	if (!frame && RenderTarget != RenderedSurface) return false;
	// UNOFFICIAL 2026-11-05: with the game's MSAA the composites are the D3D9 ones, which take each edge pixel's sky share (SkyCoverage,
	// Includes/MsaaSkyEdge.hlsl); the Vulkan composites have no such read yet.
	if (frame && NvrGameMultisample.x > 1.0f) return false;
	IDirect3DTexture9* depth = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
	IDirect3DTexture9* atlas = TheShaderManager->Effects.ShadowsExteriors ? TheShaderManager->Effects.ShadowsExteriors->ShadowAtlasTexture : nullptr;
	IDirect3DBaseTexture9* textures[3] = {};
	UINT count = 0;
	switch (techniqueIndex) {
	case 0: textures[0] = TheTextureManager->RenderedTexture; textures[1] = depth; textures[2] = Textures.BlurTexture; count = 3; break;
	case 1: textures[0] = depth; textures[1] = atlas; count = 2; break;
	case 2: textures[0] = Textures.TempTexture; count = 1; break;
	case 3: textures[0] = Textures.MarchTexture; textures[1] = Textures.HistoryTexture; count = 2; break;
	case 4: textures[0] = Textures.MarchTexture; count = 1; break;
	case 5: textures[0] = atlas; textures[1] = fogNoise; count = 2; break;	// the noise is bound whether or not the fog volume reads it
	case 6: textures[0] = Textures.GridTexture; count = 1; break;
	case 7: textures[0] = Textures.GridTempTexture; count = 1; break;
	case 8: textures[0] = TheTextureManager->RenderedTexture; textures[1] = depth; textures[2] = Textures.GridTexture; count = 3; break;
	}
	for (UINT i = 0; i < count; i++) if (!textures[i]) return false;

	ExtPass& pass = extPasses[techniqueIndex == 4 ? 2 : techniqueIndex];
	if (!pass.Tried) {
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[7] = {};
		for (UINT i = 0; i < count; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i;
			bindings[i].ViewType = techniqueIndex == 5 && i == 1 ? NVR_EXT_VIEW_3D : NVR_EXT_VIEW_2D;
		}
		bindings[count] = { 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 };
		for (UINT s = 0; s < 3; s++) {
			bindings[count + 1 + s].Type = NVR_EXT_BIND_SAMPLER; bindings[count + 1 + s].Slot = s; bindings[count + 1 + s].PushOffset = s * 4;
		}
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(SpvNames[techniqueIndex], bindings, count + 4, sizeof(ExtPush), true);
	}
	ID3D9NvrExtSampler* samplers[3] = { DxvkCompute::PointClamp(), DxvkCompute::LinearClamp(), DxvkCompute::LinearWrap() };
	if (!pass.Pipeline || !samplers[0] || !samplers[1] || !samplers[2]) return false;

	ExtConstants constants;
	for (UINT i = 0; i < _countof(ExtMatrixNames); i++) {
		const D3DXVECTOR4* value = ExtRegistered(ExtMatrixNames[i]);
		if (!value) return false;
		memcpy(&constants.Matrices[i], value, sizeof(D3DXMATRIX));
	}
	for (UINT i = 0; i < _countof(ExtVectorNames); i++) {
		const D3DXVECTOR4* value = ExtRegistered(ExtVectorNames[i]);
		if (!value) return false;
		constants.Vectors[i] = *value;
	}
	ID3D9NvrExtBuffer* uniforms = ExtUniforms(extUniforms, &constants, sizeof(constants));
	if (!uniforms) return false;

	NVR_EXT_RESOURCE_BINDING resources[7] = {};
	for (UINT i = 0; i < count; i++) {
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[count].Slot = 0; resources[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[count].pResource = uniforms;
	for (UINT s = 0; s < 3; s++) {
		resources[count + 1 + s].Slot = s; resources[count + 1 + s].Type = NVR_EXT_BIND_SAMPLER; resources[count + 1 + s].pResource = samplers[s];
	}
	ExtPush push = {};
	push.axis = techniqueIndex == 2 ? D3DXVECTOR2(1.0f, 0.0f) : D3DXVECTOR2(0.0f, 1.0f);	// BlurXPS, BlurYPS (only the blur reads it)

	// (UNOFFICIAL 2026-11-01: extDrew, which path drew, for the report burst's VL stages)
	if (frame)
		return extDrew = ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, pass.Pipeline, resources, count + 4, &push, sizeof(push), true);
	return extDrew = ExtDrawTo(pass.Pipeline, &RenderTarget, 1, resources, count + 4, &push, sizeof(push));
}

// UNOFFICIAL 2026-11-01 the report burst's VL stages (core/Diagnostics.cpp; called by ShaderManager.cpp only while Diagnostics::VLCapturing).
void VolumetricLightEffect::ReportComposite(IDirect3DDevice9* Device, bool after) {
	if (after) Diagnostics::VLPass(CompositeTechnique(), extDrew);
	else extDrew = false;
	Diagnostics::VLFrame(Device, after);
}
