#include "WetWorld.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-11-05 Model 1 fallback (the material buffer's state this frame)

void WetWorldEffect::UpdateConstants() {
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
	// UNOFFICIAL 2026-11-05 Model 1: the material and albedo buffers are drawn at the internal resolution; a frame upscaled before the
	// effects (RenderScale with LowResEffects off) no longer matches them (as CapturedNormals, EffectChain.cpp): WetPBR guesses then.
	Constants.Look.y = TAAEffect::UpscaledBeforeEffects() ? 0.0f : 1.0f;

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
	model = cfg->GetSettingI(section, "Model");	// UNOFFICIAL 2026-11-05 (WetWorld.h)
}

// UNOFFICIAL 2026-11-05 (QA finding 15): why Model 1 cannot run this frame (1 to 3), or 0. WetPBR tells characters (worn gear 2, skin 6)
// and materials apart by TESR_MaterialBuffer; without this frame's buffer every pixel is class 0, so characters would be darkened and
// filmed like the ground. CapturedNormals::Data.x is reset every frame (MaterialData.x is not: it keeps its last value while the capture
// is off), and EffectChain zeroes it in a frame upscaled before the effects.
int WetWorldEffect::Model1Unavailable() const {
	if (TAAEffect::UpscaledBeforeEffects()) return 1;
	if (!CapturedNormals::MaterialTexture || CapturedNormals::Data.x < 0.5f || CapturedNormals::MaterialData.x < 0.5f) return 2;
	if (TheSettingManager->GetSettingI("Main.Main.Quality", "MaterialBufferSkin") == 0) return 3;	// skin would stay class 0
	return 0;
}

// UNOFFICIAL 2026-11-05 Model 1 (WetWorld.h): techniques 0 and 2 -> 7, 3 and 4 -> 8 (the fixed trace), 5 -> 9, 6 -> 10 (Persistent). Model 1
// always takes the FastPaths structure (FastPaths off only changes which Model 0 program runs). A frame Model 1 cannot read (above) keeps
// the Model 0 technique (the old look; techniques 2 to 4 may then take the Vulkan path as before).
UINT WetWorldEffect::ModelTechnique(UINT techniqueIndex) {
	if (model != 1 || !Effect) return techniqueIndex;
	UINT twin;
	switch (techniqueIndex) {
		case 0: case 2: twin = 7; break;
		case 3: case 4: twin = 8; break;
		case 5: twin = 9; break;
		case 6: twin = 10; break;
		default: return techniqueIndex;
	}
	if (!Effect->GetTechnique(twin)) return techniqueIndex;
	const int why = Model1Unavailable();
	if (why) {
		if (!(model1FallbackLogged & (1u << why))) {
			model1FallbackLogged |= 1u << why;
			static const char* const reasons[4] = { "",
				"the frame is upscaled before the effects (RenderScale with LowResEffects off: the material buffer has another size)",
				"the material buffer holds nothing this frame (needs [Main.Main.Quality] CapturedNormals and MaterialBuffer; not with the game's own MSAA on native D3D9)",
				"[Main.Main.Quality] MaterialBufferSkin is off (skin would not be known as a character)" };
			Logger::Log("WetWorld: Model 1 needs this frame's material buffer, but %s: the Model 0 technique %u runs instead (logged once per reason)", reasons[why], techniqueIndex);
		}
		return techniqueIndex;
	}
	return twin;
}

void WetWorldEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* Target, IDirect3DSurface9* Rendered, UINT techniqueIndex, bool ClearTarget, IDirect3DSurface9* Source) {
	EffectRecord::Render(Device, Target, Rendered, ModelTechnique(techniqueIndex), ClearTarget, Source);
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
struct WetWorldPush { UInt32 samplers[4]; UInt32 mode; UInt32 pad[3]; };

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
	// (2026-11-05: Model 1's techniques 7 to 10 have no Vulkan pass yet: declined here, so the D3D9 pass draws them.)
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
		NVR_EXT_BINDING_DESC bindings[8] = {};
		const UINT count = WetBindings(bindings, 5, 2);
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
		return ExtFullScreen(Device, RenderTarget, surface, source, pass.Pipeline, resources, n, &push, sizeof(push), last);
	};

	// WetMap: the current image is bound (the shader declares it) but not read; it writes the chain's spare texture and commits.
	IDirect3DBaseTexture9* mapTextures[5] = { TheTextureManager->RenderedTexture, depth, depthViewModel, ortho, normals };
	ID3D9NvrExtSampler* mapSamplers[2] = { linearClamp, pointClamp };
	if (!drawPass(extMap, mapTextures, 5, mapSamplers, 2, 0, RenderedSurface, SourceBuffer, false)) return false;

	// BlurWetMap across, then down: each reads the image the previous pass committed.
	for (UINT blur = 1; blur <= 2; blur++) {
		mapTextures[0] = TheTextureManager->RenderedTexture;
		if (!drawPass(extMap, mapTextures, 5, mapSamplers, 2, blur, TheTextureManager->RenderedSurface, nullptr, false)) return true;
	}

	// Wet: the blurred wet map is the current image; TESR_SourceBuffer is the frame before the effect.
	IDirect3DBaseTexture9* wetTextures[13] = { TheTextureManager->RenderedTexture, TheTextureManager->SourceTexture, depth, depthViewModel,
		ortho, normals, pointShadow, ripple, splash, splashNormal, cloud, puddleNormal, streak };
	ID3D9NvrExtSampler* wetSamplers[4] = { linearClamp, pointClamp, mipWrap, mipClamp };
	const UINT wetMode = (fast ? 1u : 0u) | (fixTrace ? 2u : 0u);
	drawPass(extWet, wetTextures, 13, wetSamplers, 4, wetMode, TheTextureManager->RenderedSurface, nullptr, true);
	return true;
}