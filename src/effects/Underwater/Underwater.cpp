#include "Underwater.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-11-27 RenderScale v2

bool UnderwaterEffect::ShouldRender() {
	return TheShaderManager->GameState.isUnderwater || Player->inWater;
};

namespace {
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the constants are read from the same registered memory the D3D9 path uploads
	// (ConstantsTable, as Snow.cpp reads them), so the passes see the values the technique sees.
	const D3DXVECTOR4* RegisteredConstant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	// Underwater.glsl's Constants block (std140): fifteen float4s, then the two matrices as D3DX stores them, then renderScale.
	struct UnderwaterUniforms {
		D3DXVECTOR4 reciprocalResolution, sunDirection, gameTime, waterCoefficients, sunColor, waterFog, waterVolume, waveParams,
			waterSettings, waterShallowColor, waterDeepColor, horizonColor, skyColor, cameraPosition, cameraData;
		D3DXMATRIX projectionTransform, viewTransform;
		D3DXVECTOR4 renderScale;
	};
	static_assert(sizeof(UnderwaterUniforms) == 16 * 16 + 2 * 64, "UnderwaterUniforms size mismatch");

	// The push blocks; the sampler heap indices are filled in by the extension at PushOffset.
	struct GodraysPush { UInt32 linearIndex, noiseIndex, causticsIndex, pad; };
	struct BlurPush { D3DXVECTOR2 offsetMask; UInt32 samplerIndex; UInt32 pad; };
	struct WaterPush { UInt32 linearIndex, pointIndex, anisoIndex, pad; };
	struct DistortionPush { UInt32 linearIndex, pad[3]; };

	const NVR_EXT_BINDING_DESC GodraysBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer (texelFetch)
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_noise: TESR_BlueNoiseSampler
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_caustics: TESR_CausticsSampler
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// LINEAR, CLAMP: index at push offset 0
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 4 },	// POINT, WRAP
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        2, NVR_EXT_VIEW_2D, FALSE, 8 },	// ANISOTROPIC, WRAP
	};
	const NVR_EXT_BINDING_DESC BlurBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 8 },	// LINEAR, CLAMP
	};
	const NVR_EXT_BINDING_DESC WaterBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_source: TESR_SourceBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_normals: TESR_NormalsBuffer
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE,  4, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_caustics: TESR_CausticsSampler
		{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE,  5, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_shadow: TESR_PointShadowBuffer
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// LINEAR, CLAMP: index at push offset 0
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 4 },	// POINT, CLAMP
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        2, NVR_EXT_VIEW_2D, FALSE, 8 },	// ANISOTROPIC, WRAP
	};
	const NVR_EXT_BINDING_DESC DistortionBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_rendered: TESR_RenderedBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_source: TESR_SourceBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// LINEAR, CLAMP
	};

	NVR_EXT_RESOURCE_BINDING Bind(UINT slot, NVR_EXT_BIND_TYPE type, IUnknown* resource) {
		NVR_EXT_RESOURCE_BINDING binding = {};
		binding.Slot = slot;
		binding.Type = type;
		binding.pResource = resource;
		return binding;
	}
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the five passes as Vulkan passes (src\hlsl\NewVegas\Compute\Underwater*.frag, Underwater.glsl), in
// the D3D9 technique's order and through the frame chain as Render does it: god rays (into the chain; outside the upper left quarter the
// current image is kept), the blur V and H, the water, the distortion (last: it may write straight into the game target). The
// constants are the registered ones the D3D9 technique uploads, in one uniform buffer. The pipelines, samplers, textures and constants
// are all made before the first draw, so a refusal there leaves the frame as it was (the D3D9 technique runs). A draw that fails after
// the first pass has drawn keeps what the passes made (the D3D9 technique would run the god rays a second time on top of them).
bool UnderwaterEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || ClearRenderTarget) return false;
	if (!TheShaderManager->Chain.Owns(RenderTarget, RenderedSurface)) return false;

	// The textures the technique binds by name: the same objects the D3D9 path binds (TESR_DepthBuffer is CombineDepth's,
	// TESR_NormalsBuffer the normals', TESR_PointShadowBuffer the exterior shadows').
	auto combineDepth = TheShaderManager->Effects.CombineDepth;
	auto normals = TheShaderManager->Effects.Normals;
	auto shadows = TheShaderManager->Effects.ShadowsExteriors;
	IDirect3DBaseTexture9* depth = combineDepth ? combineDepth->Textures.CombinedDepthTexture : nullptr;
	IDirect3DBaseTexture9* normalsTexture = normals ? normals->Textures.NormalsTexture : nullptr;
	IDirect3DBaseTexture9* shadowMap = shadows ? shadows->Textures.ShadowPassTexture : nullptr;
	IDirect3DBaseTexture9* blueNoise = ExtFileTexture("TESR_BlueNoiseSampler");
	IDirect3DBaseTexture9* caustics = ExtFileTexture("TESR_CausticsSampler");
	if (!depth || !normalsTexture || !shadowMap || !blueNoise || !caustics || !TheTextureManager->SourceTexture || !TheTextureManager->RenderedTexture) return false;

	// The pipelines and the samplers (the caustics and the point shadow buffer: MIN/MAG/MIP ANISOTROPIC, WRAP).
	auto make = [](ExtPass& pass, const char* name, const NVR_EXT_BINDING_DESC* bindings, UINT count, UINT pushSize) {
		if (pass.Tried) return;
		pass.Tried = true;
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(name, bindings, count, pushSize, true);
	};
	make(extGodrays, "UnderwaterGodrays", GodraysBindings, _countof(GodraysBindings), sizeof(GodraysPush));
	make(extBlur, "UnderwaterBlur", BlurBindings, _countof(BlurBindings), sizeof(BlurPush));
	make(extWater, "UnderwaterWater", WaterBindings, _countof(WaterBindings), sizeof(WaterPush));
	make(extDistortion, "UnderwaterDistortion", DistortionBindings, _countof(DistortionBindings), sizeof(DistortionPush));
	ID3D9NvrExtSampler* linearClamp = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* pointClamp = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* pointWrap = DxvkCompute::PointWrap();
	ID3D9NvrExtSampler* anisoWrap = DxvkCompute::MipSampler(true, NVR_EXT_ADDRESS_WRAP);
	if (!extGodrays.Pipeline || !extBlur.Pipeline || !extWater.Pipeline || !extDistortion.Pipeline ||
		!linearClamp || !pointClamp || !pointWrap || !anisoWrap) return false;

	// The constants as the D3D9 constant table holds them this frame.
	UnderwaterUniforms uniforms = {};
	bool found = true;
	auto vec4Of = [&found](D3DXVECTOR4& out, const char* name) {
		const D3DXVECTOR4* value = RegisteredConstant(name);
		if (value) out = *value; else found = false;
	};
	vec4Of(uniforms.reciprocalResolution, "TESR_ReciprocalResolution");
	vec4Of(uniforms.sunDirection, "TESR_SunDirection");
	vec4Of(uniforms.gameTime, "TESR_GameTime");
	vec4Of(uniforms.waterCoefficients, "TESR_WaterCoefficients");
	vec4Of(uniforms.sunColor, "TESR_SunColor");
	vec4Of(uniforms.waterFog, "TESR_WaterFog");
	vec4Of(uniforms.waterVolume, "TESR_WaterVolume");
	vec4Of(uniforms.waveParams, "TESR_WaveParams");
	vec4Of(uniforms.waterSettings, "TESR_WaterSettings");
	vec4Of(uniforms.waterShallowColor, "TESR_WaterShallowColor");
	vec4Of(uniforms.waterDeepColor, "TESR_WaterDeepColor");
	vec4Of(uniforms.horizonColor, "TESR_HorizonColor");
	vec4Of(uniforms.skyColor, "TESR_SkyColor");
	vec4Of(uniforms.cameraPosition, "TESR_CameraPosition");
	vec4Of(uniforms.cameraData, "TESR_CameraData");
	const D3DXVECTOR4* projection = RegisteredConstant("TESR_ProjectionTransform");
	const D3DXVECTOR4* view = RegisteredConstant("TESR_ViewTransform");
	if (!found || !projection || !view) return false;
	memcpy(&uniforms.projectionTransform, projection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.viewTransform, view, sizeof(D3DXMATRIX));
	uniforms.renderScale = RenderRect::Active()
		? D3DXVECTOR4(RenderRect::ScaleX, RenderRect::ScaleY, 1.0f / RenderRect::ScaleX, 1.0f / RenderRect::ScaleY)
		: D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	ID3D9NvrExtBuffer* buffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!buffer) return false;

	// Pass 1, the god rays. It copies the frame into SourceBuffer (as Render does) and writes into the chain.
	{
		GodraysPush push = {};
		IDirect3DBaseTexture9* godrayTextures[] = { TheTextureManager->RenderedTexture, depth, blueNoise, caustics };
		NVR_EXT_RESOURCE_BINDING resources[] = {
			Bind(0, NVR_EXT_BIND_SAMPLED_IMAGE, godrayTextures[0]), Bind(1, NVR_EXT_BIND_SAMPLED_IMAGE, godrayTextures[1]),
			Bind(2, NVR_EXT_BIND_SAMPLED_IMAGE, godrayTextures[2]), Bind(3, NVR_EXT_BIND_SAMPLED_IMAGE, godrayTextures[3]),
			Bind(0, NVR_EXT_BIND_UNIFORM_BUFFER, buffer), Bind(0, NVR_EXT_BIND_SAMPLER, linearClamp),
			Bind(1, NVR_EXT_BIND_SAMPLER, pointWrap), Bind(2, NVR_EXT_BIND_SAMPLER, anisoWrap) };
		if (!ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, SourceBuffer, extGodrays.Pipeline, resources, _countof(resources), &push, sizeof(push), false))
			return false;
	}

	// Passes 2 and 3, the blur: OffsetMaskV, then OffsetMaskH. Each reads the current image.
	const D3DXVECTOR2 blurMasks[2] = { D3DXVECTOR2(0.0f, 1.0f), D3DXVECTOR2(1.0f, 0.0f) };
	for (UINT i = 0; i < 2; i++) {
		BlurPush push = {};
		push.offsetMask = blurMasks[i];
		NVR_EXT_RESOURCE_BINDING resources[] = {
			Bind(0, NVR_EXT_BIND_SAMPLED_IMAGE, TheTextureManager->RenderedTexture),
			Bind(0, NVR_EXT_BIND_UNIFORM_BUFFER, buffer), Bind(0, NVR_EXT_BIND_SAMPLER, linearClamp) };
		if (!ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, nullptr, extBlur.Pipeline, resources, _countof(resources), &push, sizeof(push), false))
			return true;
	}

	// Pass 4, the water: the frame before the effect (SourceBuffer) where it is above the water, else the water over it.
	{
		WaterPush push = {};
		NVR_EXT_RESOURCE_BINDING resources[] = {
			Bind(0, NVR_EXT_BIND_SAMPLED_IMAGE, TheTextureManager->RenderedTexture),
			Bind(1, NVR_EXT_BIND_SAMPLED_IMAGE, TheTextureManager->SourceTexture),
			Bind(2, NVR_EXT_BIND_SAMPLED_IMAGE, depth),
			Bind(3, NVR_EXT_BIND_SAMPLED_IMAGE, normalsTexture),
			Bind(4, NVR_EXT_BIND_SAMPLED_IMAGE, caustics),
			Bind(5, NVR_EXT_BIND_SAMPLED_IMAGE, shadowMap),
			Bind(0, NVR_EXT_BIND_UNIFORM_BUFFER, buffer), Bind(0, NVR_EXT_BIND_SAMPLER, linearClamp),
			Bind(1, NVR_EXT_BIND_SAMPLER, pointClamp), Bind(2, NVR_EXT_BIND_SAMPLER, anisoWrap) };
		if (!ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, nullptr, extWater.Pipeline, resources, _countof(resources), &push, sizeof(push), false))
			return true;
	}

	// Pass 5, the wobble: the last pass of the effect.
	{
		DistortionPush push = {};
		NVR_EXT_RESOURCE_BINDING resources[] = {
			Bind(0, NVR_EXT_BIND_SAMPLED_IMAGE, TheTextureManager->RenderedTexture),
			Bind(1, NVR_EXT_BIND_SAMPLED_IMAGE, TheTextureManager->SourceTexture),
			Bind(2, NVR_EXT_BIND_SAMPLED_IMAGE, depth),
			Bind(0, NVR_EXT_BIND_UNIFORM_BUFFER, buffer), Bind(0, NVR_EXT_BIND_SAMPLER, linearClamp) };
		ExtFullScreen(Device, RenderTarget, TheTextureManager->RenderedSurface, nullptr, extDistortion.Pipeline, resources, _countof(resources), &push, sizeof(push), true);
	}
	return true;
}
