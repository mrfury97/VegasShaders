#pragma once

// UNOFFICIAL: the sun's highlight and the sky's reflection on every surface outdoors (Effects\Specular.fx.hlsl): GGX
// with Smith visibility and Schlick's Fresnel, the split-sum sky reflection, the sun's shadow from the cascades, worked
// out at a quarter of the resolution from wide-averaged normals and added to the image at full resolution.
// [Shaders.Specular.Exterior] and [Shaders.Specular.Rain], blended by how much it rains.

// UNOFFICIAL 2026-10-25 (NVR DXVK): the constants block of Compute\SpecularCommon.glsl, as the D3D9 path's values are copied
// (matrices as stored: cb.m[i][j] is the HLSL's m[i][j]). Only matrices and vec4s, so std140 is this layout.
struct SpecularUniforms {
	float viewTransform[16], projectionTransform[16];
	float shadowNear[16], shadowMiddle[16], shadowFar[16], shadowLod[16];
	float reciprocalResolution[4], specularData[4], specularEffects[4], sunDirection[4], sunColor[4], skyColor[4], horizonColor[4];
	float waterSettings[4], shadowFade[4], bounceData[4], bounceLayout[4], specularExtra[4];
	float depthConstants[4], cameraData[4], cameraPosition[4];
	float smoothedSunDir[4], shadowFormatData[4], shadowBlur[4];
	float shadowNearCenter[4], shadowMiddleCenter[4], shadowFarCenter[4], shadowLodCenter[4];
};

// Push data of the Specular passes: the heap indices of the point (offset 0) and the linear (offset 4) sampler.
struct SpecularPush {
	UInt32 pointSampler;
	UInt32 linearSampler;
	UInt32 pad[2];
};

class SpecularEffect : public EffectRecord
{
public:
	// UNOFFICIAL 2026-11-12 (memleak) idle targets (core/TextureManager.h, EffectRecord::TargetParkable): both buffers are drawn and read only by this effect's own passes, all behind its switch (EffectChain's wouldRender, RenderWithBounce)
	bool	TargetParkable(const char* SamplerName) override { return true; }
	SpecularEffect() : EffectRecord("Specular") {};

	struct SpecularStruct {
		D3DXVECTOR4		EffectStrength;	// x: specular anti-aliasing, y: reflectance at normal incidence (F0), z: buffer scale (4, 1 with Max Quality)
		D3DXVECTOR4		Data;			// x: sun highlight strength, y: roughness, z: sky reflection strength, w: fade-out distance
	};
	SpecularStruct	Constants;

	struct LookStruct {
		float SunStrength;		// the sun's highlight (1: as a real dielectric would)
		float Roughness;		// 0 mirror .. 1 matte
		float SkyReflections;	// the sky's reflection (1: as a real dielectric would)
		float DistanceFade;		// fades out by this distance (game units)
		float SpecularAA;		// how much the spread of the normals widens the highlight (0 .. 1)
		float Reflectance;		// F0, reflectance head on (0.04: most non-metals)
	};
	struct SettingsSpecularStruct {
		LookStruct Exterior;
		LookStruct Rain;
	};
	SettingsSpecularStruct Settings;

	// Quarter resolution: each 4 x 4 block's normal and depth (technique 1, SpecularPrep), and the light it reflects
	// (technique 2, SpecularLow); technique 0 adds the latter to the image.
	struct TexturesStruct {
		IDirect3DTexture9* NormalsTexture = nullptr;
		IDirect3DSurface9* NormalsSurface = nullptr;
		IDirect3DTexture9* LightTexture = nullptr;
		IDirect3DSurface9* LightSurface = nullptr;
	};
	TexturesStruct	Textures;
	CachedHandle techBounce, paramBounceData, paramBounceLayout;	// UNOFFICIAL: looked up once per loaded effect
	// UNOFFICIAL 2026-10-19: the sky visibility (Skylighting v3 phase 1) on stage 9 and the land's and clouds' shadow (SunFar) on stage
	// 10 for the light step (technique 2), with NVR_SpecularExtra; on = false unbinds them.
	CachedHandle paramExtra;
	void	BindExtras(IDirect3DDevice9* Device, bool on);

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();

	// The bounce light's full-resolution step and this effect's in one pass (technique SpecularBounce), for frames where
	// nothing renders between them. bounce: BounceLight's low-resolution result; data, layout: its constants. Returns
	// false, leaving the image untouched, if it could not.
	bool	CanTakeBounce();
	bool	RenderWithBounce(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		IDirect3DTexture9* bounce, const D3DXVECTOR4& data, const D3DXVECTOR4& layout);

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the passes as Vulkan passes (Compute\Specular*.spv, from src\hlsl\NewVegas\Compute): techniques
	// 1 (SpecularPrep) and 2 (SpecularLow) draw into the effect's own quarter-resolution targets, technique 0 adds to the frame through
	// the chain; SpecularBounce is ExtRenderBounce (RenderWithBounce). False: nothing drawn, the D3D9 technique runs.
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	bool	ExtRenderBounce(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		IDirect3DBaseTexture9* bounce, const D3DXVECTOR4& data, const D3DXVECTOR4& layout);

private:
	ExtPass					extPrep, extLight, extAdd, extBounce;
	ExtUniformBuffer		extUniforms;
	// What BindExtras set for the light step (null where it bound none) and the NVR_SpecularExtra values.
	IDirect3DBaseTexture9*	extraSkyVisibility = nullptr;
	IDirect3DBaseTexture9*	extraSunFar = nullptr;
	D3DXVECTOR4				extraValues = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	const void*				ConstantValue(const char* name);
	IDirect3DBaseTexture9*	TextureOf(const char* name);
	bool					FillUniforms(SpecularUniforms& uniforms);
};
