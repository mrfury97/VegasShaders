#pragma once

class PointShadowsEffect : public EffectRecord
{
public:
	PointShadowsEffect() : EffectRecord("PointShadows") {};

	struct PointShadowsStruct {
	};
	PointShadowsStruct	Constants;

	void	UpdateConstants() {};
	void	RegisterConstants() {};
	void	UpdateSettings() {};

	// UNOFFICIAL 2026-10-25 (NVR DXVK): techniques 0 (MergedPointShadows), 1 (AddShadowedLamps), 2 (LampContact), 3 (LampMaskHalf) and
	// 4 (LampMaskUpsample) as src\hlsl\NewVegas\Compute\PointShadows*.frag (EffectRecord::ExtRender). The D3D9 technique runs where
	// this returns false: the scissored mask (the passes ignore the device's scissor rectangle), or a texture or constant missing.
	bool	ExtSupportsRenderRect() const override { return true; }
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	// One full-screen draw: the constants block (from the values the D3D9 path uploads), the textures in binding order (the first
	// cubeCount are cube maps), two samplers (heap slots 0 and 1), the pass's push mode. clear: the D3D9 pass's own clear first.
	bool	ExtDraw(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
				ExtPass& pass, const char* spvName, NVR_EXT_BLEND blend, IDirect3DBaseTexture9* const* textures, UINT cubeCount, UINT textureCount,
				ID3D9NvrExtSampler* first, ID3D9NvrExtSampler* second, UINT mode, bool clear);
	ExtPass				extMerged, extBatch, extContact, extUpsample;
	std::map<std::string, ExtPass> extVariants;	// UNOFFICIAL 2026-10-26: compile-time variants (EffectRecord::ExtVariant)
	ExtUniformBuffer	extUniforms;
};
