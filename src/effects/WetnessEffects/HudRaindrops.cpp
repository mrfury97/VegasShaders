#include <algorithm>

#include "../../core/DxvkCompute.h"
#include "HudRaindrops.h"
#include "../../core/FrameClock.h"

void HudRaindropsEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_HudRainData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_HudRainTime", &Constants.Time);
	TheShaderManager->RegisterConstant("TESR_HudRainShade", &Constants.Shade);
	TheShaderManager->RegisterConstant("TESR_HudRainDebug", &Constants.Debug);
	TheShaderManager->RegisterConstant("TESR_HudRainScreen", &Constants.Screen);
}

void HudRaindropsEffect::RegisterTextures() {
	// 32 bits: at 16 the wetness's small steps at high frame rates rounded away (as AutoExposure's).
	// TESR_HudRainState is also what the arms' and weapon's shaders read (s14).
	TheTextureManager->InitTexture("TESR_HudRainState", &Textures.StateTexture, &Textures.StateSurface, 1, 1, D3DFMT_G32R32F);
	TheTextureManager->InitTexture("TESR_HudRainNew", &Textures.NewTexture, &Textures.NewSurface, 1, 1, D3DFMT_G32R32F);
	// The Pip-Boy screen's copy: made at its size the first time it is drawn (DrawOnScreen).
	TheTextureManager->RegisterTexture("TESR_HudRainScreenCopy", (IDirect3DBaseTexture9**)&Textures.ScreenCopy);
	Cleared = false;
}

// The settings live under Wetness Effects ([Shaders.WetnessEffects.HudRaindrops]), and so does the switch: its Enabled stands in for
// a Status section (SettingManager::GetMenuShaderEnabled), and turning it in the menu switches the effect here, as SwitchShaderStatus
// would. The shaders' part is compiled in only when it is on at startup (HUD_RAIN_ON, ShaderRecord.cpp).
void HudRaindropsEffect::UpdateSettings() {
	const char* Section = "Shaders.WetnessEffects.HudRaindrops";
	Settings.DropSize = std::clamp(TheSettingManager->GetSettingF(Section, "DropSize"), 0.5f, 64.0f);
	Settings.Density = std::clamp(TheSettingManager->GetSettingF(Section, "Density"), 0.0f, 1.0f);
	Settings.WetTime = std::clamp(TheSettingManager->GetSettingF(Section, "WetTime"), 0.5f, 600.0f);
	Settings.Refraction = std::clamp(TheSettingManager->GetSettingF(Section, "Refraction"), 0.0f, 4.0f);
	Settings.Reflection = std::clamp(TheSettingManager->GetSettingF(Section, "Reflection"), 0.0f, 8.0f);
	Settings.Gloss = std::clamp(TheSettingManager->GetSettingF(Section, "Gloss"), 0.0f, 1.0f);
	Settings.WetGloss = std::clamp(TheSettingManager->GetSettingF(Section, "WetGloss"), 0.0f, 1.0f);
	Settings.AnimSpeed = std::clamp(TheSettingManager->GetSettingF(Section, "AnimSpeed"), 0.0f, 8.0f);
	Settings.DebugView = std::clamp(TheSettingManager->GetSettingI(Section, "DebugView"), 0, 1);
	Settings.Darkness = std::clamp(TheSettingManager->GetSettingF(Section, "Darkness"), 0.0f, 1.0f);
	Settings.AllObjects = TheSettingManager->GetSettingI(Section, "AllObjects") != 0;

	if (IsLoaded() || Enabled) {
		const bool want = TheSettingManager->GetSettingI(Section, "Enabled") != 0;
		if (want != Enabled) SwitchEffect();
	}
}

void HudRaindropsEffect::UpdateConstants() {
	// The clocks below once a frame (this runs twice a frame, FrameClock.h); the GPU's easing (Effects/HudRaindrops.fx) runs once a
	// frame and takes the whole frame's time.
	const float Elapsed = (float)ElapsedOncePerFrame(LastFrameTime);
	const float FrameTime = std::clamp((float)TheFrameRateManager->ElapsedTime, 0.0f, 0.25f);
	const auto& State = TheShaderManager->GameState;
	// How much it rains, from the sky itself, not the Precipitations effect (whose animator only runs while that effect is on): the
	// weathers flagged rainy, across a weather transition the outgoing one fading as the incoming one comes in (Sky::weatherPercent,
	// 0-1 toward firstWeather).
	float Rain = 0.0f;
	Sky* WorldSky = Tes ? Tes->sky : nullptr;
	if (State.isExterior && WorldSky) {
		auto Rainy = [](TESWeather* Weather) { return (Weather && (Weather->GetWeatherType() & TESWeather::WeatherType::kType_Rainy)) ? 1.0f : 0.0f; };
		const float Progress = std::clamp(WorldSky->weatherPercent, 0.0f, 1.0f);
		Rain = WorldSky->secondWeather ? Rainy(WorldSky->firstWeather) * Progress + Rainy(WorldSky->secondWeather) * (1.0f - Progress) : Rainy(WorldSky->firstWeather);
	}

	// While it rains, and for WetTime after (the drops could still be drying), the passes run.
	if (Rain > 0.0f) DryingLeft = Settings.WetTime + 1.0f;
	else DryingLeft = max(DryingLeft - Elapsed, 0.0f);
	if (Rain > 0.0f) TheShaderManager->orthoRequired = true;   // the cover test (Effects/HudRaindrops.fx)

	// The world's objects (AllObjects): wet through over WetTime in the rain, dry over the same time after; indoors at once (nothing
	// there is out in the rain, and there is no ortho map to tell).
	if (!State.isExterior || State.isUnderwater || !Settings.AllObjects)
		WorldWet = 0.0f;
	else if (Rain > 0.0f || Settings.DebugView > 0)
		WorldWet = min(1.0f, WorldWet + Elapsed * max(Rain, Settings.DebugView > 0 ? 1.0f : 0.0f) / Settings.WetTime);
	else
		WorldWet = max(0.0f, WorldWet - Elapsed / Settings.WetTime);
	if (WorldWet > 0.0f) TheShaderManager->orthoRequired = true;   // the objects' cover test

	// The drops' clock runs while it rains, faster in heavy rain (SSS's script slows it in light rain). Wrapped on a whole number of
	// both layers' cycles (5 and 10). Once the rain stops it stands still: no new drops fall, and the ones there shrink away where
	// they are as the surface dries (Shaders/Includes/HudRain.hlsl, HudRainThreshold).
	if (Rain > 0.0f || Settings.DebugView > 0) Clock = fmodf(Clock + Elapsed * Settings.AnimSpeed * (0.5f + max(Rain, Settings.DebugView > 0 ? 1.0f : 0.0f)), 1000.0f);

	Constants.Data = D3DXVECTOR4(Rain, 0.0f, 0.0f, 0.0f);
	Constants.Time = D3DXVECTOR4(0.0f, FrameTime, Settings.WetTime, 0.0f);
	Constants.Shade = D3DXVECTOR4(0.0f, 0.0f, State.isExterior ? 0.0f : 1.0f, State.isUnderwater ? 1.0f : 0.0f);
	Constants.Debug = D3DXVECTOR4((float)Settings.DebugView, 0.0f, 0.0f, 0.0f);
	// The screen is about 9 game units across: the drops come out the size they are on the arms.
	Constants.Screen = D3DXVECTOR4(0.5f - 0.45f * Settings.Density, Clock, Settings.Refraction * 0.01f, 9.0f / Settings.DropSize);
}

bool HudRaindropsEffect::ShouldRender() {
	return (DryingLeft > 0.0f || Settings.DebugView > 0) && Textures.StateSurface && Textures.NewSurface;
}

// The wetness eased (technique 0, into NewTexture, reading StateTexture) and kept (1, into StateTexture), for the arms' and weapon's
// shaders next frame.
void HudRaindropsEffect::UpdateWetness(IDirect3DDevice9* Device) {
	if (!Enabled || !Effect || !ShouldRender()) return;
	if (!Cleared) {
		Device->SetRenderTarget(0, Textures.StateSurface);
		Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0L);
		Cleared = true;
	}
	Device->SetRenderTarget(0, Textures.NewSurface);
	Render(Device, Textures.NewSurface, nullptr, 0, false, nullptr);
	Device->SetRenderTarget(0, Textures.StateSurface);
	Render(Device, Textures.StateSurface, nullptr, 1, false, nullptr);
}

// For each geometry pass: drops on (c221.x = 1) for first-person geometry while the effect runs, and (AllObjects) for every other
// object while the world is wet (c170.x its wetness), off otherwise. The land never gets them: its shaders have none.
void HudRaindropsEffect::SetDrawConstants(bool firstPerson) {
	const bool Running = Enabled && Cleared && ShouldRender();
	const bool FirstPerson = Running && firstPerson;
	const bool World = Running && !FirstPerson && WorldWet > 0.0f;
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (!FirstPerson && !World) {   // written every pass: whatever the device held there before is not ours
		static const float Zero[8] = {};
		Device->SetPixelShaderConstantF(221, Zero, 2);
		Device->SetPixelShaderConstantF(170, Zero, 1);
		return;
	}
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	const float OrthoTexel = (World && Shadows && Shadows->ShadowMapOrthoTexture && TheShaderManager->orthoRequired) ?
		Shadows->ShadowMaps[ShadowsExteriorEffect::MapOrtho].ShadowMapInverseResolution : 0.0f;
	// Draw (c221): x on, y the drops' threshold (Density 1: 0.05, 0: 0.5), z the clock, w the refraction (the texture coordinates'
	// shift per unit of slope, in drop texture tiles). Look (c222): x the normal's tilt, y the drops' gloss, z the wet surface's, w
	// the tile's size (units). World (c170): x the world's wetness (0 for first-person geometry, which reads the camera's), y
	// Darkness, z the ortho map's texel size for the cover test.
	const float Data[8] = {
		1.0f, 0.5f - 0.45f * Settings.Density, Clock, Settings.Refraction * 0.01f,
		Settings.Reflection, Settings.Gloss, Settings.WetGloss, Settings.DropSize,
	};
	const float WorldData[4] = { World ? WorldWet : 0.0f, Settings.Darkness, OrthoTexel, 0.0f };
	Device->SetPixelShaderConstantF(221, Data, 2);
	Device->SetPixelShaderConstantF(170, WorldData, 1);
}

// The menu the game has just rendered into the Pip-Boy's screen texture, copied and drawn back into it with the drops (technique 2).
// Inside the game's own rendering: what this changes on the device (the render target, viewport, vertex stream and format) goes back
// as it was; the effect restores its own states at End.
bool HudRaindropsEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex > 1 || ClearRenderTarget || !DxvkCompute::Available() || !RenderTarget) return false;
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!point) return false;
	struct Push { UInt32 pointSampler; UInt32 pad[3]; } push = {};

	if (techniqueIndex == 1) {
		if (!Textures.NewTexture) return false;
		if (!extKeep.Tried) {
			extKeep.Tried = true;
			NVR_EXT_BINDING_DESC bindings[2] = {};
			bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0; bindings[0].ViewType = NVR_EXT_VIEW_2D;
			bindings[1].Type = NVR_EXT_BIND_SAMPLER; bindings[1].Slot = 0; bindings[1].PushOffset = 0;
			extKeep.Pipeline = DxvkCompute::CreateGraphicsPipeline("HudRainKeep", bindings, _countof(bindings), sizeof(push), true);
		}
		if (!extKeep.Pipeline) return false;
		NVR_EXT_RESOURCE_BINDING res[2] = {};
		res[0].Slot = 0; res[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[0].pResource = Textures.NewTexture;
		res[1].Slot = 0; res[1].Type = NVR_EXT_BIND_SAMPLER; res[1].pResource = point;
		return ExtDrawTo(extKeep.Pipeline, &RenderTarget, 1, res, _countof(res), &push, sizeof(push), nullptr, ExtUv::ScreenQuad);
	}

	auto orthoIt = TheTextureManager->TextureNames.find("TESR_OrthoMapBuffer");
	IDirect3DBaseTexture9* ortho = (orthoIt != TheTextureManager->TextureNames.end() && orthoIt->second) ? *orthoIt->second : nullptr;
	if (!ortho) ortho = Textures.StateTexture;
	if (!ortho || !Textures.StateTexture) return false;

	struct HudRainUpdateUniforms {
		float shadowOrtho[16];
		float data[4], time[4], shade[4], debug[4];
	} uniforms = {};
	auto orthoMatIt = TheShaderManager->ConstantsTable.find("TESR_ShadowCameraToLightTransformOrtho");
	if (orthoMatIt != TheShaderManager->ConstantsTable.end() && orthoMatIt->second) {
		memcpy(uniforms.shadowOrtho, orthoMatIt->second, sizeof(uniforms.shadowOrtho));
	}
	else if (Constants.Data.x > 0.0f && Constants.Shade.z < 0.5f) return false;
	memcpy(uniforms.data, &Constants.Data, sizeof(uniforms.data));
	memcpy(uniforms.time, &Constants.Time, sizeof(uniforms.time));
	memcpy(uniforms.shade, &Constants.Shade, sizeof(uniforms.shade));
	memcpy(uniforms.debug, &Constants.Debug, sizeof(uniforms.debug));

	ID3D9NvrExtBuffer* cb = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!cb) return false;
	if (!extUpdate.Tried) {
		extUpdate.Tried = true;
		NVR_EXT_BINDING_DESC bindings[4] = {};
		bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0; bindings[0].ViewType = NVR_EXT_VIEW_2D;
		bindings[1].Set = 1; bindings[1].Binding = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].Slot = 1; bindings[1].ViewType = NVR_EXT_VIEW_2D;
		bindings[2].Set = 2; bindings[2].Binding = 0; bindings[2].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[2].Slot = 0; bindings[2].ViewType = NVR_EXT_VIEW_2D;
		bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].Slot = 0; bindings[3].PushOffset = 0;
		extUpdate.Pipeline = DxvkCompute::CreateGraphicsPipeline("HudRainUpdate", bindings, _countof(bindings), sizeof(push), true);
	}
	if (!extUpdate.Pipeline) return false;
	NVR_EXT_RESOURCE_BINDING res[4] = {};
	res[0].Slot = 0; res[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[0].pResource = ortho;
	res[1].Slot = 1; res[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[1].pResource = Textures.StateTexture;
	res[2].Slot = 0; res[2].Type = NVR_EXT_BIND_UNIFORM_BUFFER; res[2].pResource = cb;
	res[3].Slot = 0; res[3].Type = NVR_EXT_BIND_SAMPLER; res[3].pResource = point;
	return ExtDrawTo(extUpdate.Pipeline, &RenderTarget, 1, res, _countof(res), &push, sizeof(push), nullptr, ExtUv::ScreenQuad);
}

bool HudRaindropsEffect::ExtDrawOnScreen(IDirect3DDevice9* Device, IDirect3DSurface9* TargetSurface) {
	if (!DxvkCompute::Available() || !Textures.StateTexture || !Textures.ScreenCopy || !TargetSurface) return false;
	IDirect3DBaseTexture9* dropTex = ExtFileTexture("TESR_HudRainTexture");
	if (!dropTex) dropTex = TheTextureManager->GetFileTexture("Data\\Textures\\Effects\\hud_raindrops.dds", TextureRecord::TextureRecordType::PlanarBuffer);
	if (!dropTex) return false;
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* mipWrap = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP);
	if (!mipWrap) mipWrap = DxvkCompute::LinearWrap();
	if (!point || !linear || !mipWrap) return false;

	struct Push {
		float screen[4];
		UInt32 pointSampler, linearClamp, mipWrap, pad;
	} push = {};
	memcpy(push.screen, &Constants.Screen, sizeof(push.screen));

	if (!extScreen.Tried) {
		extScreen.Tried = true;
		NVR_EXT_BINDING_DESC bindings[6] = {};
		bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0; bindings[0].ViewType = NVR_EXT_VIEW_2D;
		bindings[1].Set = 1; bindings[1].Binding = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].Slot = 1; bindings[1].ViewType = NVR_EXT_VIEW_2D;
		bindings[2].Set = 1; bindings[2].Binding = 2; bindings[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[2].Slot = 2; bindings[2].ViewType = NVR_EXT_VIEW_2D;
		bindings[3].Type = NVR_EXT_BIND_SAMPLER; bindings[3].Slot = 0; bindings[3].PushOffset = 16;
		bindings[4].Type = NVR_EXT_BIND_SAMPLER; bindings[4].Slot = 1; bindings[4].PushOffset = 20;
		bindings[5].Type = NVR_EXT_BIND_SAMPLER; bindings[5].Slot = 2; bindings[5].PushOffset = 24;
		extScreen.Pipeline = DxvkCompute::CreateGraphicsPipeline("HudRainScreen", bindings, _countof(bindings), sizeof(push), true);
	}
	if (!extScreen.Pipeline) return false;

	NVR_EXT_RESOURCE_BINDING res[6] = {};
	res[0].Slot = 0; res[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[0].pResource = Textures.StateTexture;
	res[1].Slot = 1; res[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[1].pResource = Textures.ScreenCopy;
	res[2].Slot = 2; res[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[2].pResource = dropTex;
	res[3].Slot = 0; res[3].Type = NVR_EXT_BIND_SAMPLER; res[3].pResource = point;
	res[4].Slot = 1; res[4].Type = NVR_EXT_BIND_SAMPLER; res[4].pResource = linear;
	res[5].Slot = 2; res[5].Type = NVR_EXT_BIND_SAMPLER; res[5].pResource = mipWrap;
	return ExtDrawTo(extScreen.Pipeline, &TargetSurface, 1, res, _countof(res), &push, sizeof(push), nullptr, ExtUv::ScreenQuad);
}

void HudRaindropsEffect::DrawOnScreen(BSRenderedTexture* Target) {
	if (!Enabled || !Effect || !Cleared || !ShouldRender() || !Target || !Target->GetTexture(0)) return;
	IDirect3DTexture9* TargetTexture = Target->GetD3DTexture(0);
	if (!TargetTexture) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DSURFACE_DESC Desc;
	if (FAILED(TargetTexture->GetLevelDesc(0, &Desc)) || !(Desc.Usage & D3DUSAGE_RENDERTARGET)) return;

	if (!Textures.ScreenCopy || Textures.ScreenWidth != Desc.Width || Textures.ScreenHeight != Desc.Height || Textures.ScreenFormat != Desc.Format) {
		if (Textures.ScreenCopySurface) Textures.ScreenCopySurface->Release();
		if (Textures.ScreenCopy) Textures.ScreenCopy->Release();
		if (Textures.ScreenQuad) Textures.ScreenQuad->Release();
		Textures.ScreenCopy = nullptr; Textures.ScreenCopySurface = nullptr; Textures.ScreenQuad = nullptr;
		if (FAILED(Device->CreateTexture(Desc.Width, Desc.Height, 1, D3DUSAGE_RENDERTARGET, Desc.Format, D3DPOOL_DEFAULT, &Textures.ScreenCopy, NULL))) {
			Textures.ScreenCopy = nullptr;
			return;
		}
		Textures.ScreenCopy->GetSurfaceLevel(0, &Textures.ScreenCopySurface);
		TheShaderManager->CreateFrameVertex(Desc.Width, Desc.Height, &Textures.ScreenQuad);
		Textures.ScreenWidth = Desc.Width; Textures.ScreenHeight = Desc.Height; Textures.ScreenFormat = Desc.Format;
	}
	if (!Textures.ScreenCopySurface || !Textures.ScreenQuad) return;

	IDirect3DSurface9* TargetSurface = nullptr;
	if (FAILED(TargetTexture->GetSurfaceLevel(0, &TargetSurface))) return;
	Device->StretchRect(TargetSurface, NULL, Textures.ScreenCopySurface, NULL, D3DTEXF_NONE);

	if (!ExtFailed && DxvkCompute::PassesOnRect() && !DxvkCompute::KeptOnD3D9(Name) && ExtDrawOnScreen(Device, TargetSurface)) {
		TargetSurface->Release();
		return;
	}

	IDirect3DSurface9* OldTarget = nullptr;
	D3DVIEWPORT9 OldViewport;
	IDirect3DVertexBuffer9* OldStream = nullptr;
	UINT OldOffset = 0, OldStride = 0;
	DWORD OldFVF = 0;
	Device->GetRenderTarget(0, &OldTarget);
	Device->GetViewport(&OldViewport);
	Device->GetStreamSource(0, &OldStream, &OldOffset, &OldStride);
	Device->GetFVF(&OldFVF);

	Device->SetRenderTarget(0, TargetSurface);
	const D3DVIEWPORT9 Whole = { 0, 0, Desc.Width, Desc.Height, 0.0f, 1.0f };
	Device->SetViewport(&Whole);
	Device->SetStreamSource(0, Textures.ScreenQuad, 0, sizeof(FrameVS));
	Device->SetFVF(FrameFVF);
	try {
		Effect->SetTechnique(Effect->GetTechnique(2));
		SetCT();
		UINT Passes = 0;
		Effect->Begin(&Passes, 0);   // restores the device's states at End
		Device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		for (UINT p = 0; p < Passes; p++) {
			Effect->BeginPass(p);
			Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Effect->EndPass();
		}
		Effect->End();
	}
	catch (const std::exception& e) {
		Logger::Log("Error during rendering of effect %s: %s", Name, e.what());
	}

	Device->SetRenderTarget(0, OldTarget);
	Device->SetViewport(&OldViewport);
	Device->SetStreamSource(0, OldStream, OldOffset, OldStride);
	Device->SetFVF(OldFVF);
	if (OldTarget) OldTarget->Release();
	if (OldStream) OldStream->Release();
	TargetSurface->Release();
}
