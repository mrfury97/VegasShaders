#include "SnowAccumulation.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent snow
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals
#include "../../core/RenderRect.h"		// UNOFFICIAL 2026-11-27 RenderScale v2

void SnowAccumulationEffect::UpdateConstants() {
	Animator& cover = Constants.SnowAccumulationAnimator;
	const bool snowing = TheShaderManager->GameState.isSnow;

	// snow starts settling at the rising edge of the weather and melts away at the falling edge
	if (snowing && !cover.switched) {
		cover.switched = true;
		cover.Start(increaseRate, 1);
	}
	else if (!snowing && cover.switched) {
		cover.switched = false;
		cover.Start(decreaseRate, 0);
	}

	Constants.Data.w = cover.GetValue();
	// the ortho shadow map is needed while any snow lies on the ground
	if (Constants.Data.w) TheShaderManager->orthoRequired = true;
	// UNOFFICIAL 2026-10-27 Persistent (CoverMap.h): also while the cover map may still hold snow
	else if (TheCoverMap && TheCoverMap->SnowActive()) TheShaderManager->orthoRequired = true;

}

void SnowAccumulationEffect::UpdateSettings(){
	const char* section = "Shaders.SnowAccumulation.Main";
	SettingManager* cfg = TheSettingManager;
	increaseRate = cfg->GetSettingF(section, "Increase");
	decreaseRate = cfg->GetSettingF(section, "Decrease");

	Constants.Data.x = cfg->GetSettingF(section, "BlurNormDropThreshhold");
	Constants.Data.y = cfg->GetSettingF(section, "BlurRadiusMultiplier");
	Constants.Data.z = cfg->GetSettingF(section, "SunPower");
	Constants.Color.x = cfg->GetSettingF(section, "SnowColorR");
	Constants.Color.y = cfg->GetSettingF(section, "SnowColorG");
	Constants.Color.z = cfg->GetSettingF(section, "SnowColorB");
	// UNOFFICIAL 2026-10-28 NormalSmoothing (game units, 0 = off): the snow's slope and shading from the ground's shape over that
	// radius instead of each triangle's flat normal (SnowAccumulation.fx.hlsl SnowSmoothNormal, SnowAccumulation.frag). Live.
	Constants.Color.w = std::clamp(TheSettingManager->GetSettingF("Shaders.SnowAccumulation.Main", "NormalSmoothing"), 0.0f, 256.0f);
	// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): the slope and the lighting from the surfaces' own smooth normals
	// where they were captured. Compiled into the lit shaders only when on at startup; live after that (off: the depth normals, as before).
	CapturedNormals = TheSettingManager->GetSettingI("Shaders.SnowAccumulation.Main", "CapturedNormals") != 0;
	Extra.x = CapturedNormals && CapturedNormals::CompiledIn() ? 1.0f : 0.0f;
	Extra.y = (float)std::clamp(TheSettingManager->GetSettingI("Shaders.SnowAccumulation.Main", "CapturedNormalsDebugView"), 0, 2);
	// UNOFFICIAL 2026-10-28 Look (techniques 2 and 3 of SnowAccumulation.fx.hlsl): 1 = macintroll's version 5. Live.
	Look = std::clamp(TheSettingManager->GetSettingI("Shaders.SnowAccumulation.Main", "Look"), 0, 1);
	FollowJitter = TheSettingManager->GetSettingI("Shaders.SnowAccumulation.Main", "FollowJitter") != 0;	// UNOFFICIAL 2026-10-28, live
}

UINT SnowAccumulationEffect::Technique(UINT persistent) {
	return Look == 1 && Effect && Effect->GetTechnique(2 + persistent) ? 2 + persistent : persistent;
}

void SnowAccumulationEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_SnowAccumulationParams", &Constants.Data);
	sm->RegisterConstant("TESR_SnowAccumulationColor", &Constants.Color);
	sm->RegisterConstant("TESR_SnowAccumulationExtra", &Extra);	// UNOFFICIAL 2026-10-28
}

bool SnowAccumulationEffect::ShouldRender() {
	const auto& game = TheShaderManager->GameState;
	if (!game.isExterior || game.isUnderwater) return false;
	// UNOFFICIAL 2026-10-27 Persistent (CoverMap.h): also while the cover map may still hold snow (false with Persistent off)
	return Constants.Data.w > 0.0f || (TheCoverMap && TheCoverMap->SnowActive());
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the uniform block SnowAccumulation.frag reads (std140; the matrices as stored, 64 bytes each).
// The field order and sizes are the GLSL block's.
struct SnowAccumulationUniforms {
	D3DXMATRIX		shadowToLight;
	D3DXMATRIX		projection;
	D3DXMATRIX		invProjection;
	D3DXMATRIX		view;
	D3DXMATRIX		invView;
	D3DXVECTOR4		cameraData;
	D3DXVECTOR4		cameraPosition;
	D3DXVECTOR4		reciprocalResolution;
	D3DXVECTOR4		sunDirection;
	D3DXVECTOR4		sunColor;
	D3DXVECTOR4		sunAmbient;
	D3DXVECTOR4		waterSettings;
	D3DXVECTOR4		shadowFade;
	D3DXVECTOR4		shadowScreenSpaceData;
	D3DXVECTOR4		orthoData;
	D3DXVECTOR4		snowParams;
	D3DXVECTOR4		snowAccColor;
	D3DXVECTOR4		shadowLightPosition[12];
	D3DXVECTOR4		lightPosition[12];
	D3DXVECTOR4		lightColor[24];
	D3DXVECTOR4		capturedData;	// UNOFFICIAL 2026-10-28 TESR_CapturedNormalsData
	D3DXVECTOR4		snowExtra;		// TESR_SnowAccumulationExtra
	D3DXVECTOR4		materialData;	// UNOFFICIAL 2026-11-24 TESR_MaterialBufferData (x 1 = this frame's, y 1 = GBufferV2)
	D3DXVECTOR4		renderScale;	// RenderRect scale (xy = scale, zw = 1 / scale)
};
static_assert(sizeof(SnowAccumulationUniforms) == 5 * 64 + 64 * 16, "SnowAccumulationUniforms size mismatch");

// The push block (24 bytes): the blur's direction, the two sampler heap indices (DXVK writes them at their offsets), the pass's mode.
struct SnowAccumulationPush {
	D3DXVECTOR2		offsetMask;
	UInt32			samplerClamp;
	UInt32			samplerWrap;
	UInt32			mode;
	UInt32			pad;
};

// The constants as the D3D9 constant table holds them this frame: the registered memory SetCT uploads.
static const D3DXVECTOR4* SnowAccumulationRegistered(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

static bool SnowAccumulationCopy(void* dst, const char* name, size_t bytes) {
	const D3DXVECTOR4* src = SnowAccumulationRegistered(name);
	if (!src) return false;
	memcpy(dst, src, bytes);
	return true;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the five passes through the frame chain as Render does it: SnowCoverage, Scale, DepthBlur
// horizontal and vertical (each into the chain), then Snow, which may write the game target directly. Every pass reads the current
// image (TESR_RenderedBuffer); TESR_SourceBuffer is the frame before the effect, copied once by the first pass. Every texture,
// pipeline and the constants are made before anything is drawn, so a refused one leaves the frame as it was (the D3D9 technique runs).
// Once the first pass has drawn, a later failure keeps what is there (as SkinScattering does).
bool SnowAccumulationEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || ClearRenderTarget) return false;
	if (!SourceBuffer || SourceBuffer == RenderTarget || SourceBuffer != TheTextureManager->SourceSurface || !TheTextureManager->SourceTexture || !TheTextureManager->RenderedTexture) return false;

	// The engine's textures by name, as the D3D9 samplers bind them; the file textures behind the effect's own samplers.
	auto engineTexture = [](const char* name) -> IDirect3DBaseTexture9* {
		std::string n = name;
		return TheTextureManager->GetTextureByName(n);
	};
	IDirect3DBaseTexture9* depth = engineTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* ortho = engineTexture("TESR_OrthoMapBuffer");
	IDirect3DBaseTexture9* normals = engineTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* pointShadow = engineTexture("TESR_PointShadowBuffer");
	IDirect3DBaseTexture9* snowNormal = ExtFileTexture("TESR_SnowNormSampler");
	IDirect3DBaseTexture9* blueNoise = ExtFileTexture("TESR_BlueNoiseSampler");
	if (!depth || !ortho || !normals || !pointShadow || !snowNormal || !blueNoise) return false;
	// UNOFFICIAL 2026-10-28 CapturedNormals: none made yet (or off) = any texture: the shader reads it only with snowExtra.x and capturedData.x
	IDirect3DBaseTexture9* captured = CapturedNormals::Texture ? (IDirect3DBaseTexture9*)CapturedNormals::Texture : normals;
	IDirect3DBaseTexture9* material = CapturedNormals::MaterialTexture ? (IDirect3DBaseTexture9*)CapturedNormals::MaterialTexture : normals;

	// LINEAR/CLAMP for the engine's textures; the trilinear mip sampler of the file textures (WRAP).
	ID3D9NvrExtSampler* clamp = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* wrap = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP);
	if (!clamp || !wrap) return false;

	if (!extPass.Tried) {
		extPass.Tried = true;
		// Textures in the order the shader declares them (set 1, binding i, slot i), the constants (set 2, binding 0), then the two
		// samplers: their heap indices are at push offsets 8 and 12.
		NVR_EXT_BINDING_DESC bindings[13] = {};
		UINT n = 0;
		for (UINT i = 0; i < 10; i++) { bindings[n].Set = 1; bindings[n].Binding = i; bindings[n].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[n].Slot = i; bindings[n].ViewType = NVR_EXT_VIEW_2D; n++; }
		bindings[n].Set = 2; bindings[n].Binding = 0; bindings[n].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[n].Slot = 0; n++;
		bindings[n].Type = NVR_EXT_BIND_SAMPLER; bindings[n].Slot = 0; bindings[n].PushOffset = 8; n++;
		bindings[n].Type = NVR_EXT_BIND_SAMPLER; bindings[n].Slot = 1; bindings[n].PushOffset = 12; n++;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("SnowAccumulation", bindings, n, sizeof(SnowAccumulationPush), true);
	}
	if (!extPass.Pipeline) return false;

	// The constants as the D3D9 constant table holds them this frame. The matrices are copied as stored: the shader's M[i][j] is the
	// D3DXMATRIX element (i+1, j+1), which is what SetMatrix gives the D3D9 effect.
	SnowAccumulationUniforms uniforms = {};
	bool copied = SnowAccumulationCopy(&uniforms.shadowToLight, "TESR_ShadowCameraToLightTransformOrtho", sizeof(D3DXMATRIX))
		&& SnowAccumulationCopy(&uniforms.projection, "TESR_ProjectionTransform", sizeof(D3DXMATRIX))
		&& SnowAccumulationCopy(&uniforms.invProjection, "TESR_InvProjectionTransform", sizeof(D3DXMATRIX))
		&& SnowAccumulationCopy(&uniforms.view, "TESR_ViewTransform", sizeof(D3DXMATRIX))
		&& SnowAccumulationCopy(&uniforms.invView, "TESR_InvViewTransform", sizeof(D3DXMATRIX))
		&& SnowAccumulationCopy(&uniforms.cameraData, "TESR_CameraData", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.cameraPosition, "TESR_CameraPosition", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.reciprocalResolution, "TESR_ReciprocalResolution", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.sunDirection, "TESR_SunDirection", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.sunColor, "TESR_SunColor", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.sunAmbient, "TESR_SunAmbient", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.waterSettings, "TESR_WaterSettings", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.shadowFade, "TESR_ShadowFade", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.shadowScreenSpaceData, "TESR_ShadowScreenSpaceData", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(&uniforms.orthoData, "TESR_OrthoData", sizeof(D3DXVECTOR4))
		&& SnowAccumulationCopy(uniforms.shadowLightPosition, "TESR_ShadowLightPosition", sizeof(uniforms.shadowLightPosition))
		&& SnowAccumulationCopy(uniforms.lightPosition, "TESR_LightPosition", sizeof(uniforms.lightPosition))
		&& SnowAccumulationCopy(uniforms.lightColor, "TESR_LightColor", sizeof(uniforms.lightColor));
	if (!copied) return false;
	uniforms.snowParams = Constants.Data;	// TESR_SnowAccumulationParams: the same memory RegisterConstant points at
	uniforms.snowAccColor = Constants.Color;	// TESR_SnowAccumulationColor
	uniforms.capturedData = CapturedNormals::Data;
	uniforms.snowExtra = Extra;
	if (!CapturedNormals::Texture) uniforms.snowExtra.x = 0.0f;
	uniforms.materialData = CapturedNormals::MaterialData;
	if (!CapturedNormals::MaterialTexture) uniforms.materialData.x = 0.0f;
	uniforms.materialData.y = CapturedNormals::V2Compiled ? 1.0f : 0.0f;
	uniforms.renderScale = RenderRect::Active()
		? D3DXVECTOR4(RenderRect::ScaleX, RenderRect::ScaleY, 1.0f / RenderRect::ScaleX, 1.0f / RenderRect::ScaleY)
		: D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	ID3D9NvrExtBuffer* constants = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!constants) return false;

	// One full-screen pass through the chain. The current image moves on each Commit, so TESR_RenderedBuffer is set per pass.
	IDirect3DBaseTexture9* textures[10] = { TheTextureManager->RenderedTexture, TheTextureManager->SourceTexture, depth, ortho, normals,
		pointShadow, snowNormal, blueNoise, captured, material };
	auto drawPass = [&](UINT mode, D3DXVECTOR2 offsetMask, IDirect3DSurface9* source, bool last) {
		textures[0] = TheTextureManager->RenderedTexture;
		NVR_EXT_RESOURCE_BINDING resources[13] = {};
		UINT n = 0;
		for (UINT i = 0; i < 10; i++) { resources[n].Slot = i; resources[n].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[n].pResource = textures[i]; n++; }
		resources[n].Slot = 0; resources[n].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[n].pResource = constants; n++;
		resources[n].Slot = 0; resources[n].Type = NVR_EXT_BIND_SAMPLER; resources[n].pResource = clamp; n++;
		resources[n].Slot = 1; resources[n].Type = NVR_EXT_BIND_SAMPLER; resources[n].pResource = wrap; n++;
		SnowAccumulationPush push = {};
		push.offsetMask = offsetMask;
		push.mode = mode;
		return ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, source, extPass.Pipeline, resources, n, &push, sizeof(push), last);
	};

	const D3DXVECTOR2 noMask(0.0f, 0.0f);
	const D3DXVECTOR2 offsetMaskH(1.0f, 0.0f);	// OffsetMaskH
	const D3DXVECTOR2 offsetMaskV(0.0f, 1.0f);	// OffsetMaskV
	// SnowCoverage (mode 0) copies the frame into the chain's spare image first, as Render does; the source copy is made here.
	if (!drawPass(0, noMask, SourceBuffer, false)) return false;
	// Scale (mode 1), DepthBlur horizontal and vertical (mode 2): each reads the image the previous pass committed.
	if (!drawPass(1, noMask, nullptr, false)) return true;
	if (!drawPass(2, offsetMaskH, nullptr, false)) return true;
	if (!drawPass(2, offsetMaskV, nullptr, false)) return true;
	drawPass(3, noMask, nullptr, true);	// Snow (mode 3): may write the game target directly; a failure here keeps what is there
	return true;
}