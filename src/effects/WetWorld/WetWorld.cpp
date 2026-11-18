#include "WetWorld.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-11-12 PuddlesLandOnly (TESR_MaterialBuffer's classes)
#include "../../core/RainState.h"	// UNOFFICIAL 2026-11-12 RainStateGuard
#include <algorithm>	// std::clamp

void WetWorldEffect::UpdateConstants() {
	// UNOFFICIAL 2026-11-12 RainStateGuard (core/RainState.h): on, the one rain state drives both animators and fills Data.x (the rain),
	// y (raining) and z (the puddles) every frame, whether this effect is on or off. Off: animated here as before, so only while this
	// effect is on (and the rain only while Precipitations is on).
	if (!RainState::Tick()) {
		Animator& puddles = Constants.PuddlesAnimator;
		const bool raining = TheShaderManager->GameState.isRainy;
		// puddles fill when the rain begins and drain when it stops
		if (raining && !puddles.switched) {
			puddles.switched = true;
			puddles.Start(increaseRate, 1);
		}
		else if (!raining && puddles.switched) {
			puddles.switched = false;
			puddles.Start(decreaseRate, 0);
		}
		Constants.Data.x = TheShaderManager->Effects.Rain->Constants.RainAnimator.GetValue();
		Constants.Data.y = raining;
		Constants.Data.z = puddles.GetValue();
	}

	// UNOFFICIAL 2026-11-12 PuddlesLandOnly: the mode the shaders take this frame. Land only (2) needs the land drawn by NVR's terrain
	// template (the only writer of the land class): without it no pixel would count as land and every puddle would go, so it steps down
	// to ground-like (1). Whether the buffer holds this frame is the shader's own test (TESR_MaterialBufferData; ExtRender for Vulkan).
	int mode = puddlesLandOnly;
	if (mode == 2 && !(TheShaderManager->Shaders.Terrain && TheShaderManager->Shaders.Terrain->Enabled)) mode = 1;
	HudRaindropsEffect* const hud = TheShaderManager->Effects.HudRaindrops;
	const bool weaponOff = weaponRainHandOff && hud && hud->Enabled && hud->IsLoaded();
	Constants.Look.y = (float)mode + (weaponOff ? 4.0f : 0.0f);
	Constants.Look.z = puddlesMinObjectSize;
	// ReflectionsOwnPuddles: only while the Reflections effect has wetness of its own to draw (it follows the rain falling now, not the
	// puddles left after it: Reflections.cpp UpdateConstants); after the rain the puddles keep their own image.
	ReflectionsEffect* const reflections = TheShaderManager->Effects.Reflections;
	Constants.Look.w = reflectionsOwnPuddles && reflections && reflections->Enabled && reflections->IsLoaded() &&
		reflections->Settings.RainWetness > 0.0f && Constants.Data.x > 0.0f ? 1.0f : 0.0f;
	// One line when it starts, one when it cannot run (while there is something wet to draw outdoors; at most 8 lines a session).
	if ((Constants.Data.x > 0.0f || Constants.Data.z > 0.0f) && TheShaderManager->GameState.isExterior) {
		const bool buffer = CapturedNormals::MaterialTexture != nullptr && CapturedNormals::MaterialData.x > 0.5f;	// (last frame's)
		const int state = mode == 0 ? 0 : buffer ? 2 + mode : 1;
		if (state != puddleLogState && puddleLogLines < 8) {
			puddleLogLines++;
			if (state == 0) Logger::Log("UNOFFICIAL WetWorld PuddlesLandOnly: off (0): puddles and wetness by the old guess alone (faces up, lies in a hollow).");
			else if (state == 1) Logger::Log("UNOFFICIAL WetWorld PuddlesLandOnly: cannot run: the material buffer holds nothing this frame ([Main.Main.Quality] "
				"MaterialBuffer off, or native D3D9 with the game's MSAA): the old guess stays.");
			// (2026-11-16 fd-batchCG) the layout the shaders decode, both runtimes (the fork's wet map reads v2 too now); v2 holds no object size
			else Logger::Log("UNOFFICIAL WetWorld PuddlesLandOnly: running, mode %d (%s)%s; material buffer layout %s; PuddlesMinObjectSize %.0f%s; WeaponRainHandOff %s; ReflectionsOwnPuddles %s.", mode,
				mode == 2 ? "land only" : "ground-like: land and rigid objects, never people, cloth or plants",
				mode != puddlesLandOnly ? " (2 was asked: the Terrain shaders are off, so no pixel is marked as land)" : "",
				CapturedNormals::V2Compiled ? "v2" : "old", puddlesMinObjectSize,
				CapturedNormals::V2Compiled && mode == 1 ? " (no effect with v2: every rigid object counts as ground)" : "",
				weaponOff ? "acting" : weaponRainHandOff ? "on, waiting for the HUD raindrops" : "off", Constants.Look.w > 0.5f ? "acting" : reflectionsOwnPuddles ? "on, waiting for Reflections in rain" : "off");
		}
		puddleLogState = state;
	}

	// the ortho shadow map is needed while it rains or puddles stand
	if (Constants.Data.x || Constants.Data.z) TheShaderManager->orthoRequired = true;
	// UNOFFICIAL 2026-10-27 Persistent (CoverMap.h): also while the cover map may still hold wet spots after the rain
	else if (TheCoverMap && TheCoverMap->WetActive()) TheShaderManager->orthoRequired = true;
}

void WetWorldEffect::UpdateSettings() {
	const char* section = "Shaders.WetWorld.Main";
	SettingManager* cfg = TheSettingManager;
	increaseRate = cfg->GetSettingF(section, "Increase");
	decreaseRate = cfg->GetSettingF(section, "Decrease");

	Constants.Coeffs = D3DXVECTOR4(cfg->GetSettingF(section, "PuddleCoeff_R"), cfg->GetSettingF(section, "PuddleCoeff_G"),
		cfg->GetSettingF(section, "PuddleCoeff_B"), cfg->GetSettingF(section, "PuddleSpecularMultiplier"));
	Constants.Data.w = cfg->GetSettingF(section, "Amount");
	// UNOFFICIAL 2026-10-16 KeepHighlights: the wet pass writes every pixel in rain, the sky included; it clamped them to 1.0 before
	// the tonemapper (sun, bright sky and lit wet highlights lost their roll-off). On: clamped only below 0.
	Constants.Look.x = cfg->GetSettingI(section, "KeepHighlights") ? 1.0f : 0.0f;
	fastPaths = cfg->GetSettingI(section, "FastPaths") != 0;
	fixedTrace = cfg->GetSettingI(section, "FixedReflectionTrace") != 0;

	// UNOFFICIAL 2026-11-12 PuddlesLandOnly, PuddlesMinObjectSize, WeaponRainHandOff, ReflectionsOwnPuddles, RainStateGuard (WetWorld.h).
	// A key missing from the defaults toml (an older one) gives its default: 1, 0, off, off, on. RainStateGuard is read here, so with
	// this effect off too (every effect reads its settings, on or off).
	SettingManager::Configuration::ConfigNode node;
	puddlesLandOnly = cfg->Config.FillNode(&node, section, "PuddlesLandOnly") ? std::clamp(node.IntValue, 0, 2) : 1;
	puddlesMinObjectSize = cfg->Config.FillNode(&node, section, "PuddlesMinObjectSize") ? std::clamp(node.FloatValue, 0.0f, 100000.0f) : 0.0f;
	weaponRainHandOff = cfg->Config.FillNode(&node, section, "WeaponRainHandOff") && node.BoolValue;
	reflectionsOwnPuddles = cfg->Config.FillNode(&node, section, "ReflectionsOwnPuddles") && node.BoolValue;
	rainStateGuard = !cfg->Config.FillNode(&node, section, "RainStateGuard") || node.BoolValue;
	puddleLogLines = 0;	// a settings change may log its new state
}

void WetWorldEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_WetWorldCoeffs", &Constants.Coeffs);
	sm->RegisterConstant("TESR_WetWorldData", &Constants.Data);
	sm->RegisterConstant("TESR_WetWorldLook", &Constants.Look);
}

void WetWorldEffect::RegisterTextures() {
	TheTextureManager->InitTexture("NVR_WetWorldCover", &coverTexture, &coverSurface, 1, 1, D3DFMT_A8R8G8B8);
}

// UNOFFICIAL HudDrops: the cover test, once a frame while it rains outdoors (after the ortho map is drawn). The previous
// test's answer is read first if it is in; a new one is issued only once it is.
void WetWorldEffect::RenderCoverTest(IDirect3DDevice9* Device) {
	if (!Enabled || !Effect || !coverSurface) return;
	if (!coverQuery && FAILED(Device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &coverQuery))) { coverQuery = nullptr; return; }
	if (coverPending) {
		DWORD pixels = 0;
		if (coverQuery->GetData(&pixels, sizeof(pixels), 0) != S_OK) return;	// not in yet
		covered = pixels > 0;
		coverPending = false;
	}
	DWORD zEnable = 0, zWrite = 0;
	Device->GetRenderState(D3DRS_ZENABLE, &zEnable);
	Device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
	Device->SetRenderState(D3DRS_ZENABLE, FALSE);
	Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	coverQuery->Issue(D3DISSUE_BEGIN);
	Device->SetRenderTarget(0, coverSurface);
	Render(Device, coverSurface, coverSurface, 1, false, nullptr);
	coverQuery->Issue(D3DISSUE_END);
	coverPending = true;
	Device->SetRenderState(D3DRS_ZENABLE, zEnable);
	Device->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
}

bool WetWorldEffect::ShouldRender() {
	// UNOFFICIAL 2026-10-27 Persistent (CoverMap.h): also while the cover map may still hold wet spots (false with Persistent off)
	return (Constants.Data.x || Constants.Data.z || (TheCoverMap && TheCoverMap->WetActive())) && TheShaderManager->GameState.isExterior && !TheShaderManager->GameState.isUnderwater;
};

// UNOFFICIAL 2026-10-25 (NVR DXVK): the constants block of WetWorldCommon.glsl (std140, 1376 bytes). Each member is the registers
// the D3D9 path uploads (a matrix is its four registers, the same bytes as the GLSL mat4's columns).
struct WetWorldUniforms {
	D3DXVECTOR4 shadowToLight[4];		// TESR_ShadowCameraToLightTransformOrtho
	D3DXVECTOR4 projection[4];			// TESR_ProjectionTransform
	D3DXVECTOR4 invProjection[4];		// TESR_InvProjectionTransform
	D3DXVECTOR4 view[4];				// TESR_ViewTransform
	D3DXVECTOR4 invView[4];				// TESR_InvViewTransform
	D3DXVECTOR4 cameraData;				// TESR_CameraData
	D3DXVECTOR4 depthConstants;			// TESR_DepthConstants
	D3DXVECTOR4 cameraPosition;			// TESR_CameraPosition
	D3DXVECTOR4 reciprocalResolution;	// TESR_ReciprocalResolution
	D3DXVECTOR4 gameTime;				// TESR_GameTime
	D3DXVECTOR4 sunDirection;			// TESR_SunDirection
	D3DXVECTOR4 sunPosition;			// TESR_SunPosition
	D3DXVECTOR4 sunAmount;				// TESR_SunAmount
	D3DXVECTOR4 sunColor;				// TESR_SunColor
	D3DXVECTOR4 sunAmbient;				// TESR_SunAmbient
	D3DXVECTOR4 skyData;				// TESR_SkyData
	D3DXVECTOR4 skyColor;				// TESR_SkyColor
	D3DXVECTOR4 skyLowColor;			// TESR_SkyLowColor
	D3DXVECTOR4 horizonColor;			// TESR_HorizonColor
	D3DXVECTOR4 orthoData;				// TESR_OrthoData
	D3DXVECTOR4 waterSettings;			// TESR_WaterSettings
	D3DXVECTOR4 wetWorldData;			// TESR_WetWorldData
	D3DXVECTOR4 wetWorldLook;			// TESR_WetWorldLook
	D3DXVECTOR4 lightPosition[12];		// TESR_LightPosition
	D3DXVECTOR4 shadowLightPosition[12];	// TESR_ShadowLightPosition
	D3DXVECTOR4 lightColor[24];			// TESR_LightColor
};
static_assert(sizeof(WetWorldUniforms) == 1376, "WetWorldUniforms must match the std140 block");

// The push block of both passes (32 bytes): the four sampler heap indices (filled in by the extension at offsets 0, 4, 8, 12), then
// the mode: the map pass's passIndex (0 WetMap, 1 and 2 the blurs), or the wet pass's bits (1 fast, 2 fixTrace).
// (2026-11-16 fd-batchCG) the map passes' next word: 1 = the material buffer is in layout v2 (WetWorldMap.frag gbufferV2; the wet pass ignores it).
struct WetWorldPush { UInt32 samplers[4]; UInt32 mode; UInt32 gbufferV2; UInt32 pad[2]; };

// The registered constant the D3D9 path uploads from (ShaderManager's table; the same memory).
static const D3DXVECTOR4* WetRegisteredConstant(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

// 2026-10-25: a matrix transposed into the block, so the GLSL mat4 (std140: memory rows are its columns) IS the logical matrix the
// D3D9 effect gets through ID3DXEffect::SetMatrix, as the shaders are written (v * M for mul(v, M), M[c] = column c). The
// port copied the memory as stored, which gave every matrix transposed.
static bool WetCopyMatrix(D3DXVECTOR4* dst, const char* name) {
	const D3DXVECTOR4* src = WetRegisteredConstant(name);
	if (!src) return false;
	D3DXMATRIX m;
	D3DXMatrixTranspose(&m, (const D3DXMATRIX*)src);
	memcpy(dst, &m, sizeof(m));
	return true;
}

static bool WetCopyConstant(D3DXVECTOR4* dst, const char* name, UINT count) {
	const D3DXVECTOR4* src = WetRegisteredConstant(name);
	if (!src) return false;
	memcpy(dst, src, count * sizeof(D3DXVECTOR4));
	return true;
}

// Bindings in the order the shaders declare them: textures (set 1, binding i, slot i), the constants block (set 2, binding 0),
// then the samplers (heap index at push offsets 0, 4, ...).
static UINT WetBindings(NVR_EXT_BINDING_DESC* out, UINT textures, UINT samplers) {
	UINT n = 0;
	for (UINT i = 0; i < textures; i++) {
		out[n].Set = 1; out[n].Binding = i; out[n].Type = NVR_EXT_BIND_SAMPLED_IMAGE; out[n].Slot = i; out[n].ViewType = NVR_EXT_VIEW_2D; n++;
	}
	out[n].Set = 2; out[n].Binding = 0; out[n].Type = NVR_EXT_BIND_UNIFORM_BUFFER; out[n].Slot = 0; n++;
	for (UINT j = 0; j < samplers; j++) {
		out[n].Type = NVR_EXT_BIND_SAMPLER; out[n].Slot = j; out[n].PushOffset = 4 * j; n++;
	}
	return n;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): WetWorld's passes as Vulkan passes, through the frame chain as Render does it: WetMap and the two
// BlurWetMap passes (WetWorldMap.frag), then Wet (WetWorld.frag), which may write the game target directly. Each pass reads the
// current image (TESR_RenderedBuffer): the wet map, then the blurred wet map; TESR_SourceBuffer is the frame before the effect, copied
// once by the first pass. Every pipeline and the constants are made before anything is drawn, so a refused one leaves the frame as
// it was (the D3D9 technique runs). Once the first pass has drawn, a later failure keeps what is there (as SkinScattering does).
bool WetWorldEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (ClearRenderTarget || (techniqueIndex != 2 && techniqueIndex != 3 && techniqueIndex != 4)) return false;
	if (!SourceBuffer || SourceBuffer == RenderTarget || SourceBuffer != TheTextureManager->SourceSurface || !TheTextureManager->SourceTexture) return false;
	const bool fast = techniqueIndex != 3;		// techniques 2 and 4 (FastPaths)
	const bool fixTrace = techniqueIndex >= 3;	// techniques 3 and 4 (FixedReflectionTrace)

	// The engine's textures as the D3D9 samplers see them; the file textures behind the effect's own samplers.
	auto engineTexture = [](const char* name) -> IDirect3DBaseTexture9* {
		std::string n = name;
		return TheTextureManager->GetTextureByName(n);
	};
	IDirect3DBaseTexture9* depth = engineTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* depthViewModel = engineTexture("TESR_DepthBufferViewModel");
	IDirect3DBaseTexture9* ortho = engineTexture("TESR_OrthoMapBuffer");
	IDirect3DBaseTexture9* normals = engineTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* pointShadow = engineTexture("TESR_PointShadowBuffer");
	IDirect3DBaseTexture9* ripple = ExtFileTexture("TESR_RippleSampler");
	IDirect3DBaseTexture9* splash = ExtFileTexture("TESR_SplashSampler");
	IDirect3DBaseTexture9* splashNormal = ExtFileTexture("TESR_SplashNormalSampler");
	IDirect3DBaseTexture9* cloud = ExtFileTexture("TESR_PuddleCloudSampler");
	IDirect3DBaseTexture9* puddleNormal = ExtFileTexture("TESR_PuddleNormalSampler");
	IDirect3DBaseTexture9* streak = ExtFileTexture("TESR_WaterStreakSampler");
	if (!depth || !depthViewModel || !ortho || !normals || !pointShadow || !ripple || !splash || !splashNormal || !cloud || !puddleNormal || !streak) return false;

	// The samplers: LINEAR/CLAMP, POINT/CLAMP (the normals), and the trilinear mip samplers of the file textures (WRAP, CLAMP).
	ID3D9NvrExtSampler* linearClamp = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* pointClamp = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* mipWrap = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP);
	ID3D9NvrExtSampler* mipClamp = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_CLAMP);
	if (!linearClamp || !pointClamp || !mipWrap || !mipClamp) return false;

	if (!extMap.Tried) {
		extMap.Tried = true;
		NVR_EXT_BINDING_DESC bindings[12] = {};
		const UINT count = WetBindings(bindings, 6, 2);	// UNOFFICIAL 2026-11-12 PuddlesLandOnly: + TESR_MaterialBuffer (binding 5)
		extMap.Pipeline = DxvkCompute::CreateGraphicsPipeline("WetWorldMap", bindings, count, sizeof(WetWorldPush), true);
	}
	if (!extWet.Tried) {
		extWet.Tried = true;
		NVR_EXT_BINDING_DESC bindings[20] = {};
		const UINT count = WetBindings(bindings, 13, 4);
		extWet.Pipeline = DxvkCompute::CreateGraphicsPipeline("WetWorld", bindings, count, sizeof(WetWorldPush), true);
	}
	if (!extMap.Pipeline || !extWet.Pipeline) return false;

	// The constants as the D3D9 constant table holds them this frame.
	WetWorldUniforms uniforms = {};
	bool copied = WetCopyMatrix(uniforms.shadowToLight, "TESR_ShadowCameraToLightTransformOrtho")
		&& WetCopyMatrix(uniforms.projection, "TESR_ProjectionTransform")
		&& WetCopyMatrix(uniforms.invProjection, "TESR_InvProjectionTransform")
		&& WetCopyMatrix(uniforms.view, "TESR_ViewTransform")
		&& WetCopyMatrix(uniforms.invView, "TESR_InvViewTransform")
		&& WetCopyConstant(&uniforms.cameraData, "TESR_CameraData", 1)
		&& WetCopyConstant(&uniforms.depthConstants, "TESR_DepthConstants", 1)
		&& WetCopyConstant(&uniforms.cameraPosition, "TESR_CameraPosition", 1)
		&& WetCopyConstant(&uniforms.reciprocalResolution, "TESR_ReciprocalResolution", 1)
		&& WetCopyConstant(&uniforms.gameTime, "TESR_GameTime", 1)
		&& WetCopyConstant(&uniforms.sunDirection, "TESR_SunDirection", 1)
		&& WetCopyConstant(&uniforms.sunPosition, "TESR_SunPosition", 1)
		&& WetCopyConstant(&uniforms.sunAmount, "TESR_SunAmount", 1)
		&& WetCopyConstant(&uniforms.sunColor, "TESR_SunColor", 1)
		&& WetCopyConstant(&uniforms.sunAmbient, "TESR_SunAmbient", 1)
		&& WetCopyConstant(&uniforms.skyData, "TESR_SkyData", 1)
		&& WetCopyConstant(&uniforms.skyColor, "TESR_SkyColor", 1)
		&& WetCopyConstant(&uniforms.skyLowColor, "TESR_SkyLowColor", 1)
		&& WetCopyConstant(&uniforms.horizonColor, "TESR_HorizonColor", 1)
		&& WetCopyConstant(&uniforms.orthoData, "TESR_OrthoData", 1)
		&& WetCopyConstant(&uniforms.waterSettings, "TESR_WaterSettings", 1)
		&& WetCopyConstant(&uniforms.wetWorldData, "TESR_WetWorldData", 1)
		&& WetCopyConstant(&uniforms.wetWorldLook, "TESR_WetWorldLook", 1)
		&& WetCopyConstant(uniforms.lightPosition, "TESR_LightPosition", 12)
		&& WetCopyConstant(uniforms.shadowLightPosition, "TESR_ShadowLightPosition", 12)
		&& WetCopyConstant(uniforms.lightColor, "TESR_LightColor", 24);
	if (!copied) return false;
	// UNOFFICIAL 2026-11-12 PuddlesLandOnly: the D3D9 pass tests TESR_MaterialBufferData itself; here the mode (y, without
	// WeaponRainHandOff's 4) is sent as 0 while the buffer holds nothing this frame, and the depth texture stands in for it (not read).
	IDirect3DBaseTexture9* material = CapturedNormals::MaterialTexture;
	// UNOFFICIAL 2026-11-16 fd-batchCG (DEFERRED-GAPS-AUDIT item 4): the Vulkan wet map reads GBufferV2 too (Compute/WetWorldMap.frag, told by
	// the map passes' push word gbufferV2), as the D3D9 pass does (WetWorld.fx.hlsl GBUFFER_V2). Before, v2 sent "no buffer" here and the fork
	// kept the old guess (puddles on people, cloth, plants) while the log said "running".
	if (!material || CapturedNormals::MaterialData.x < 0.5f) {
		material = depth;
		uniforms.wetWorldLook.y = uniforms.wetWorldLook.y >= 3.5f ? 4.0f : 0.0f;
	}
	ID3D9NvrExtBuffer* constants = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!constants) return false;

	// One pass through the chain: the shader's textures and samplers, the constants, and the mode in the push block.
	auto drawPass = [&](ExtPass& pass, IDirect3DBaseTexture9* const* textures, UINT textureCount, ID3D9NvrExtSampler* const* samplers,
		UINT samplerCount, UINT mode, IDirect3DSurface9* surface, IDirect3DSurface9* source, bool last) {
		NVR_EXT_RESOURCE_BINDING resources[20] = {};
		UINT n = 0;
		for (UINT i = 0; i < textureCount; i++) {
			resources[n].Slot = i; resources[n].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[n].pResource = textures[i]; n++;
		}
		resources[n].Slot = 0; resources[n].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[n].pResource = constants; n++;
		for (UINT j = 0; j < samplerCount; j++) {
			resources[n].Slot = j; resources[n].Type = NVR_EXT_BIND_SAMPLER; resources[n].pResource = samplers[j]; n++;
		}
		WetWorldPush push = {};
		push.mode = mode;
		push.gbufferV2 = CapturedNormals::V2Compiled ? 1u : 0u;
		return ExtFullScreen(Device, RenderTarget, surface, source, pass.Pipeline, resources, n, &push, sizeof(push), last);
	};

	// WetMap: the current image is bound (the shader declares it) but not read; it writes the chain's spare texture and commits.
	IDirect3DBaseTexture9* mapTextures[6] = { TheTextureManager->RenderedTexture, depth, depthViewModel, ortho, normals, material };
	ID3D9NvrExtSampler* mapSamplers[2] = { linearClamp, pointClamp };
	if (!drawPass(extMap, mapTextures, 6, mapSamplers, 2, 0, RenderedSurface, SourceBuffer, false)) return false;

	// BlurWetMap across, then down: each reads the image the previous pass committed.
	for (UINT blur = 1; blur <= 2; blur++) {
		mapTextures[0] = TheTextureManager->RenderedTexture;
		if (!drawPass(extMap, mapTextures, 6, mapSamplers, 2, blur, TheTextureManager->RenderedSurface, nullptr, false)) return true;
	}

	// Wet: the blurred wet map is the current image; TESR_SourceBuffer is the frame before the effect.
	IDirect3DBaseTexture9* wetTextures[13] = { TheTextureManager->RenderedTexture, TheTextureManager->SourceTexture, depth, depthViewModel,
		ortho, normals, pointShadow, ripple, splash, splashNormal, cloud, puddleNormal, streak };
	ID3D9NvrExtSampler* wetSamplers[4] = { linearClamp, pointClamp, mipWrap, mipClamp };
	const UINT wetMode = (fast ? 1u : 0u) | (fixTrace ? 2u : 0u);
	drawPass(extWet, wetTextures, 13, wetSamplers, 4, wetMode, TheTextureManager->RenderedSurface, nullptr, true);
	return true;
}