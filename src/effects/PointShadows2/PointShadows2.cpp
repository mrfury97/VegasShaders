// UNOFFICIAL 2026-10-25 (NVR DXVK): PointShadows2 as a Vulkan pass, src\hlsl\NewVegas\Compute\PointShadows2.frag, line for line with
// Effects\PointShadows2.fx.hlsl. The pass writes TESR_PointShadowBuffer and reads it (each pixel its own texel); a Vulkan pass cannot
// do that, so it reads a copy of the buffer made first, which holds the same values the D3D9 pass reads.
#include <string>
#include "PointShadows2.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

// The constants the shader reads: the std140 block of PointShadows2.frag. The matrices are copied as the D3D9 path's SetMatrix uploads
// them (the memory as stored, as Reflections.cpp does); the light arrays as the constant table holds them.
struct PointShadows2Uniforms {
	D3DXVECTOR4	cameraPosition;										// TESR_CameraPosition
	D3DXVECTOR4	cameraData;											// TESR_CameraData
	D3DXMATRIX	viewTransform;										// TESR_ViewTransform
	D3DXMATRIX	projectionTransform;								// TESR_ProjectionTransform
	D3DXVECTOR4	shadowFade;											// TESR_ShadowFade
	D3DXVECTOR4	shadowLightPosition[ShadowCubeMapsScreen];			// TESR_ShadowLightPosition[12]
	D3DXVECTOR4	lightColor[TrackedLightsMax + ShadowCubeMapsScreen];	// TESR_LightColor[24]
	D3DXVECTOR4	renderScale;										// TESR_RenderScale (xy scale, zw 1/scale)
};
static_assert(sizeof(PointShadows2Uniforms) == 768, "PointShadows2Uniforms must match the std140 block");

// The push block: the sampler heap indices (the extension fills them in at offsets 0 and 4).
struct PointShadows2Push { UInt32 samplerIndex; UInt32 pointSamplerIndex; };

namespace {
	// The registered constant the D3D9 path uploads from (the same memory). Null if absent.
	const D3DXVECTOR4* Registered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	// The texture the D3D9 path binds to a sampler name. Null if absent.
	IDirect3DBaseTexture9* EngineTexture(const std::string& name) {
		std::string n = name;
		return TheTextureManager->GetTextureByName(n);
	}

	// The bindings in the order the shader declares them: the textures (set 1, binding = slot), the constants (set 2, binding 0), then
	// the samplers (heap index at push offsets 0 and 4). The normals are POINT (their .fx state has NONE filters).
	const NVR_EXT_BINDING_DESC PointShadows2Bindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_DepthBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE, 1, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_NormalsBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE, 2, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_PointShadowBuffer (the copy)
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE, 3, NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ShadowCubeMapBuffer6
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE, 4, NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ShadowCubeMapBuffer7
		{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE, 5, NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ShadowCubeMapBuffer8
		{ 1, 6, NVR_EXT_BIND_SAMPLED_IMAGE, 6, NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ShadowCubeMapBuffer9
		{ 1, 7, NVR_EXT_BIND_SAMPLED_IMAGE, 7, NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ShadowCubeMapBuffer10
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D,  FALSE, 0 },	// the constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D,  FALSE, 0 },	// LINEAR, CLAMP
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D,  FALSE, 4 },	// POINT, CLAMP
	};

	// The copy the pass reads: the same size and format as the buffer, remade when either changes. False: no copy (the D3D9 pass runs).
	bool EnsureCopy(IDirect3DDevice9* Device, IDirect3DSurface9* Target, IDirect3DTexture9*& Texture, IDirect3DSurface9*& Surface) {
		D3DSURFACE_DESC desc = {};
		if (FAILED(Target->GetDesc(&desc))) return false;
		if (Texture && Surface) {
			D3DSURFACE_DESC copy = {};
			if (SUCCEEDED(Surface->GetDesc(&copy)) && copy.Width == desc.Width && copy.Height == desc.Height && copy.Format == desc.Format) return true;
		}
		if (Surface) { Surface->Release(); Surface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &Texture, NULL))) return false;
		return SUCCEEDED(Texture->GetSurfaceLevel(0, &Surface));
	}
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the technique's one pass into ShadowPassSurface, as RenderEffectToRT calls it. The pass reads the
// buffer's copy: the point and cube lookups as the .fx does them, the constants as the constant table holds them this frame.
bool PointShadows2Effect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (techniqueIndex != 0 || ClearRenderTarget || !shadows || RenderTarget != shadows->Textures.ShadowPassSurface) return false;

	// The textures as the D3D9 path samples them; one missing: the D3D9 technique, as before.
	IDirect3DBaseTexture9* depth = EngineTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = EngineTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* cube6 = EngineTexture("TESR_ShadowCubeMapBuffer6");
	IDirect3DBaseTexture9* cube7 = EngineTexture("TESR_ShadowCubeMapBuffer7");
	IDirect3DBaseTexture9* cube8 = EngineTexture("TESR_ShadowCubeMapBuffer8");
	IDirect3DBaseTexture9* cube9 = EngineTexture("TESR_ShadowCubeMapBuffer9");
	IDirect3DBaseTexture9* cube10 = EngineTexture("TESR_ShadowCubeMapBuffer10");
	if (!depth || !normals || !cube6 || !cube7 || !cube8 || !cube9 || !cube10) return false;

	const D3DXVECTOR4* cameraPosition = Registered("TESR_CameraPosition");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	const D3DXVECTOR4* shadowFade = Registered("TESR_ShadowFade");
	const D3DXVECTOR4* shadowLightPosition = Registered("TESR_ShadowLightPosition");
	const D3DXVECTOR4* lightColor = Registered("TESR_LightColor");
	const D3DXVECTOR4* viewTransform = Registered("TESR_ViewTransform");
	const D3DXVECTOR4* projectionTransform = Registered("TESR_ProjectionTransform");
	if (!cameraPosition || !cameraData || !shadowFade || !shadowLightPosition || !lightColor || !viewTransform || !projectionTransform) return false;

	PointShadows2Uniforms uniforms = {};
	uniforms.cameraPosition = *cameraPosition;
	uniforms.cameraData = *cameraData;
	memcpy(&uniforms.viewTransform, viewTransform, sizeof(D3DXMATRIX));
	memcpy(&uniforms.projectionTransform, projectionTransform, sizeof(D3DXMATRIX));
	uniforms.shadowFade = *shadowFade;
	memcpy(uniforms.shadowLightPosition, shadowLightPosition, sizeof(uniforms.shadowLightPosition));
	memcpy(uniforms.lightColor, lightColor, sizeof(uniforms.lightColor));
	uniforms.renderScale = RenderRect::Active()
		? D3DXVECTOR4(RenderRect::ScaleX, RenderRect::ScaleY, 1.0f / RenderRect::ScaleX, 1.0f / RenderRect::ScaleY)
		: D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);

	if (!extPass.Tried) {
		extPass.Tried = true;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("PointShadows2", PointShadows2Bindings, _countof(PointShadows2Bindings), sizeof(PointShadows2Push), true);
	}
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!extPass.Pipeline || !linear || !point) return false;
	ID3D9NvrExtBuffer* constants = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!constants) return false;

	// The copy of the buffer as it is before this pass (a StretchRect at the same size: exact).
	if (!EnsureCopy(Device, RenderTarget, copyTexture, copySurface)) return false;
	if (FAILED(Device->StretchRect(RenderTarget, NULL, copySurface, NULL, D3DTEXF_NONE))) return false;

	IDirect3DBaseTexture9* textures[8] = { depth, normals, copyTexture, cube6, cube7, cube8, cube9, cube10 };
	NVR_EXT_RESOURCE_BINDING resources[_countof(PointShadows2Bindings)] = {};
	for (UINT i = 0; i < 8; i++) {
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[8].Slot = 0; resources[8].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[8].pResource = constants;
	resources[9].Slot = 0; resources[9].Type = NVR_EXT_BIND_SAMPLER; resources[9].pResource = linear;
	resources[10].Slot = 1; resources[10].Type = NVR_EXT_BIND_SAMPLER; resources[10].pResource = point;
	PointShadows2Push push = {};
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass.Pipeline, resources, _countof(resources), &push, sizeof(push), true);
}
