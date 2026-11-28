#pragma once

class UnderwaterEffect : public EffectRecord
{
public:
	UnderwaterEffect() : EffectRecord("Underwater") {};

	struct UnderwaterStruct {
		D3DXVECTOR4		Data;
	};
	UnderwaterStruct	Constants;

	void	UpdateConstants() {};
	void	RegisterConstants() {};
	void	UpdateSettings() {};
	bool	ShouldRender();

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the five passes as src\hlsl\NewVegas\Compute\Underwater*.frag (EffectRecord::ExtRender).
	// Their constants go into one uniform buffer (more than the push data holds); the textures are the ones the D3D9 passes bind.
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	bool	ExtSupportsRenderRect() const override { return true; }
	ExtPass	extGodrays, extBlur, extWater, extDistortion;
	ExtUniformBuffer extUniforms;
}; 