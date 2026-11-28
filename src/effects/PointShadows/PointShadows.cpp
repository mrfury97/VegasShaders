#include <cstring>
#include <string>
#include "PointShadows.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

IDirect3DTexture9* LampMaskHalfTexture();	// UNOFFICIAL 2026-10-16 MaskHalfRes (effects\ShadowsExterior.cpp)

// UNOFFICIAL 2026-10-25 (NVR DXVK): the constants block of PointShadowsCommon.glsl, as the D3D9 path uploads its values (std140
// with vec4s and two mat4s, so this layout; the matrices as stored).
struct PointShadowsUniforms {
	D3DXVECTOR4 shadowLightPosition[ShadowCubeMapsScreen];
	D3DXVECTOR4 lightPosition[TrackedLightsMax];
	D3DXVECTOR4 lightColor[TrackedLightsMax + ShadowCubeMapsScreen];
	D3DXVECTOR4 extraLights[ExtraLightsMax];
	D3DXVECTOR4 shadowFade, pointShadowNear, spotLightPosition, spotLightDirection, spotLightColor;
	D3DXVECTOR4 lampContact, lampContactFade, lampContactBias, lampContactNoise;
	D3DXVECTOR4 reciprocalResolution, cameraPosition, cameraData;
	D3DXMATRIX viewTransform, projectionTransform;
	// UNOFFICIAL 2026-10-26: what the D3D9 effect framework's preshaders work out once per draw (D3DX evaluates the .fx's
	// constant-only expressions on the CPU, and fxc unrolls the 12-lamp loop): per lamp 1 / radius and its weight, per shadowed
	// lamp 1 / its near fade, the smoothstep ranges, the spot cone. The GLSL worked them out per pixel with full-precision
	// divisions: the Vulkan pass cost 0.3-0.5 ms more than its D3D9 one in Lucky 38 (GTX 1070), nearly all in the plain lamps.
	D3DXVECTOR4 shadowLightPre[ShadowCubeMapsScreen];	// x 1 / radius, y weight (luma(colour) * w), z 1 / near fade (0: none)
	D3DXVECTOR4 lightPre[TrackedLightsMax];				// x 1 / radius, y weight
	D3DXVECTOR4 extraPre[ExtraLightsMax];				// x 1 / radius (floor(w / 8)), y weight (w - radius * 8)
	D3DXVECTOR4 nearPre;								// x 1 / PointShadowNear.y (edge window), y 1 / (1 - PointShadowNear.z) (lamp reach), z 1 / spot radius, w spot weight
	D3DXVECTOR4 spotPre;								// x cos(the cone's angle), y 1 / (cos(half of it) - x)
	D3DXVECTOR4 renderScale;							// TESR_RenderScale (xy scale, zw 1/scale)
};

// The push block: the two sampler heap indices (bindings write them at 0 and 4) and the pass's mode.
struct PointShadowsPush {
	UInt32 linearSampler;	// 0: slot 0
	UInt32 pointSampler;	// 4: slot 1
	UInt32 mode;			// 8: 0 merged, 1 batch (AddShadowedLamps), 2 half-resolution mask (LampMaskHalf)
	UInt32 pad;
};

namespace {

// The memory of a constant registered under a name (ShaderManager::RegisterConstant): what the D3D9 path uploads. False if absent.
bool CopyConstant(void* to, const char* name, UINT vectors) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	if (found == TheShaderManager->ConstantsTable.end() || !found->second) return false;
	memcpy(to, found->second, vectors * sizeof(D3DXVECTOR4));
	return true;
}

// The texture registered under a sampler name (InitTexture / RegisterTexture). Null if absent.
IDirect3DBaseTexture9* TextureNamed(const char* name) {
	auto found = TheTextureManager->TextureNames.find(name);
	return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
}

float LumaOf(const D3DXVECTOR4& c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }

// The preshader values (PointShadowsUniforms above), from the constants FillUniforms copied. A radius of 0 gives 1 / 0 = inf, as the
// shader's division by it did (the lamp then counts as out of reach).
void FillPreshader(PointShadowsUniforms& u) {
	for (UINT i = 0; i < ShadowCubeMapsScreen; i++) {
		const float radius = u.shadowLightPosition[i].w;
		const float nearFade = u.pointShadowNear.x / radius;
		u.shadowLightPre[i] = D3DXVECTOR4(1.0f / radius, LumaOf(u.lightColor[i]) * u.lightColor[i].w, nearFade > 0.0f ? 1.0f / nearFade : 0.0f, 0.0f);
	}
	for (UINT i = 0; i < TrackedLightsMax; i++)
		u.lightPre[i] = D3DXVECTOR4(1.0f / u.lightPosition[i].w, LumaOf(u.lightColor[i + 12]) * u.lightColor[i + 12].w, 0.0f, 0.0f);
	for (UINT e = 0; e < ExtraLightsMax; e++) {
		const float radius = floorf(u.extraLights[e].w * 0.125f);
		u.extraPre[e] = D3DXVECTOR4(1.0f / radius, u.extraLights[e].w - radius * 8.0f, 0.0f, 0.0f);
	}
	u.nearPre = D3DXVECTOR4(u.pointShadowNear.y > 0.0f ? 1.0f / u.pointShadowNear.y : 0.0f, 1.0f / (1.0f - u.pointShadowNear.z),
		1.0f / u.spotLightPosition.w, LumaOf(u.spotLightColor) * u.spotLightColor.w);
	const float cosMax = cosf(D3DXToRadian(u.spotLightDirection.w)), cosMin = cosf(D3DXToRadian(u.spotLightDirection.w * 0.5f));
	u.spotPre = D3DXVECTOR4(cosMax, 1.0f / (cosMin - cosMax), 0.0f, 0.0f);
}

bool FillUniforms(PointShadowsUniforms& u) {
	if (!(CopyConstant(u.shadowLightPosition, "TESR_ShadowLightPosition", ShadowCubeMapsScreen) &&
		CopyConstant(u.lightPosition, "TESR_LightPosition", TrackedLightsMax) &&
		CopyConstant(u.lightColor, "TESR_LightColor", TrackedLightsMax + ShadowCubeMapsScreen) &&
		CopyConstant(u.extraLights, "TESR_ExtraLights", ExtraLightsMax) &&
		CopyConstant(&u.shadowFade, "TESR_ShadowFade", 1) && CopyConstant(&u.pointShadowNear, "TESR_PointShadowNear", 1) &&
		CopyConstant(&u.spotLightPosition, "TESR_SpotLightPosition", 1) && CopyConstant(&u.spotLightDirection, "TESR_SpotLightDirection", 1) &&
		CopyConstant(&u.spotLightColor, "TESR_SpotLightColor", 1) &&
		CopyConstant(&u.lampContact, "TESR_LampContact", 1) && CopyConstant(&u.lampContactFade, "TESR_LampContactFade", 1) &&
		CopyConstant(&u.lampContactBias, "TESR_LampContactBias", 1) && CopyConstant(&u.lampContactNoise, "TESR_LampContactNoise", 1) &&
		CopyConstant(&u.reciprocalResolution, "TESR_ReciprocalResolution", 1) && CopyConstant(&u.cameraPosition, "TESR_CameraPosition", 1) &&
		CopyConstant(&u.cameraData, "TESR_CameraData", 1) &&
		CopyConstant(&u.viewTransform, "TESR_ViewTransform", 4) && CopyConstant(&u.projectionTransform, "TESR_ProjectionTransform", 4)))
		return false;
	u.renderScale = RenderRect::Active()
		? D3DXVECTOR4(RenderRect::ScaleX, RenderRect::ScaleY, 1.0f / RenderRect::ScaleX, 1.0f / RenderRect::ScaleY)
		: D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	return true;
}

}

// UNOFFICIAL 2026-10-25 (NVR DXVK): see PointShadows.h. Bindings: the textures at set 1 (binding i, slot i), the constants at set 2
// binding 0, the samplers at heap slots 0 and 1 (push bytes 0 and 4).
bool PointShadowsEffect::ExtDraw(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
	ExtPass& pass, const char* spvName, NVR_EXT_BLEND blend, IDirect3DBaseTexture9* const* textures, UINT cubeCount, UINT textureCount,
	ID3D9NvrExtSampler* first, ID3D9NvrExtSampler* second, UINT mode, bool clear) {
	if (!first || !second) return false;
	PointShadowsUniforms uniforms;
	if (!FillUniforms(uniforms)) return false;
	FillPreshader(uniforms);
	ID3D9NvrExtBuffer* constants = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!constants) return false;

	// UNOFFICIAL 2026-10-26: this mode's own variant (one D3D9 shader per mode; Compute\variants.txt), else the general shader.
	char suffix[8];
	_snprintf_s(suffix, sizeof(suffix), _TRUNCATE, "m%u", mode);
	ID3D9NvrExtPipeline* pipeline = ExtVariant(extVariants, pass, spvName, suffix, [&](const char* name) {
		NVR_EXT_BINDING_DESC bindings[NVR_EXT_MAX_BINDINGS] = {};
		UINT c = 0;
		for (UINT i = 0; i < textureCount; i++, c++) {
			bindings[c].Set = 1; bindings[c].Binding = i; bindings[c].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[c].Slot = i;
			bindings[c].ViewType = i < cubeCount ? NVR_EXT_VIEW_CUBE : NVR_EXT_VIEW_2D;
		}
		bindings[c].Set = 2; bindings[c].Binding = 0; bindings[c].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[c].Slot = 0; bindings[c].ViewType = NVR_EXT_VIEW_2D; c++;
		bindings[c].Type = NVR_EXT_BIND_SAMPLER; bindings[c].Slot = 0; bindings[c].PushOffset = 0; c++;
		bindings[c].Type = NVR_EXT_BIND_SAMPLER; bindings[c].Slot = 1; bindings[c].PushOffset = 4; c++;
		return DxvkCompute::CreateGraphicsPipeline(name, bindings, c, sizeof(PointShadowsPush), true, 1, blend);
	});
	if (!pipeline) return false;

	NVR_EXT_RESOURCE_BINDING resources[NVR_EXT_MAX_BINDINGS] = {};
	UINT r = 0;
	for (UINT i = 0; i < textureCount; i++, r++) {
		if (!textures[i]) return false;
		resources[r].Slot = i; resources[r].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[r].pResource = textures[i];
	}
	resources[r].Slot = 0; resources[r].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[r].pResource = constants; r++;
	resources[r].Slot = 0; resources[r].Type = NVR_EXT_BIND_SAMPLER; resources[r].pResource = first; r++;
	resources[r].Slot = 1; resources[r].Type = NVR_EXT_BIND_SAMPLER; resources[r].pResource = second; r++;

	PointShadowsPush push = {};
	push.mode = mode;
	if (clear) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0L);	// (EffectRecord::Render's clear)
	if (mode == 2 && RenderRect::Active()) {
		D3DSURFACE_DESC desc = {};
		if (FAILED(RenderTarget->GetDesc(&desc))) return false;
		const RECT vp = { 0, 0, (LONG)RenderRect::Width(desc.Width), (LONG)RenderRect::Height(desc.Height) };
		if (vp.right <= 0 || vp.bottom <= 0) return false;
		return ExtDrawTo(pipeline, &RenderTarget, 1, resources, r, &push, sizeof(push), &vp, ExtUv::TexelCentre);
	}
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, pipeline, resources, r, &push, sizeof(push), true);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): see PointShadows.h. The bindings and draws follow the .fx's techniques one for one.
bool PointShadowsEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex,
	bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex > 4 || !DxvkCompute::Available()) return false;
	// The passes ignore the device's scissor rectangle (LampMaskScissor draws inside one): that draw stays on the D3D9 technique.
	DWORD scissor = FALSE;
	Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
	if (scissor) return false;

	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!linear || !point) return false;

	if (techniqueIndex == 2) {
		// LampContactPS: the depth and the normals, multiplied onto the buffer.
		IDirect3DBaseTexture9* textures[] = { TextureNamed("TESR_DepthBuffer"), TextureNamed("TESR_NormalsBuffer") };
		return ExtDraw(Device, RenderTarget, RenderedSurface, SourceBuffer, extContact, "PointShadowsContact", NVR_EXT_BLEND_MULTIPLY,
			textures, 0, _countof(textures), linear, point, 0, ClearRenderTarget);
	}

	if (techniqueIndex == 4) {
		// LampMaskUpsamplePS: the half-resolution mask (POINT, WRAP: the .fx's state for NVR_LampMaskHalf) and the depth.
		IDirect3DBaseTexture9* textures[] = { LampMaskHalfTexture(), TextureNamed("TESR_DepthBuffer") };
		ID3D9NvrExtSampler* pointWrap = DxvkCompute::PointWrap();
		return ExtDraw(Device, RenderTarget, RenderedSurface, SourceBuffer, extUpsample, "PointShadowsUpsample", NVR_EXT_BLEND_NONE,
			textures, 0, _countof(textures), linear, pointWrap, 0, ClearRenderTarget);
	}

	// MergedPointShadows (0), AddShadowedLamps (1, the sum added onto the buffer) and LampMaskHalf (3, the half-resolution mask and depth):
	// the eleven cube maps, then the depth and the normals.
	IDirect3DBaseTexture9* textures[ShadowCubeMapsSampled + 2] = {};
	for (UINT i = 0; i < ShadowCubeMapsSampled; i++) textures[i] = TextureNamed(("TESR_ShadowCubeMapBuffer" + std::to_string(i)).c_str());
	textures[ShadowCubeMapsSampled] = TextureNamed("TESR_DepthBuffer");
	textures[ShadowCubeMapsSampled + 1] = TextureNamed("TESR_NormalsBuffer");
	if (techniqueIndex == 1)
		return ExtDraw(Device, RenderTarget, RenderedSurface, SourceBuffer, extBatch, "PointShadows", NVR_EXT_BLEND_ADD,
			textures, ShadowCubeMapsSampled, _countof(textures), linear, point, 1, ClearRenderTarget);
	return ExtDraw(Device, RenderTarget, RenderedSurface, SourceBuffer, extMerged, "PointShadows", NVR_EXT_BLEND_NONE,
		textures, ShadowCubeMapsSampled, _countof(textures), linear, point, techniqueIndex == 3 ? 2 : 0, ClearRenderTarget);
}
