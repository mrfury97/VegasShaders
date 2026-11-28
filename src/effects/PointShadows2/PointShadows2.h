#pragma once

class PointShadows2Effect : public EffectRecord
{
public:
	PointShadows2Effect() : EffectRecord("PointShadows2") {};

	struct PointShadows2Struct {
	};
	PointShadows2Struct	Constants;

	void	UpdateConstants() {};
	void	RegisterConstants() {};
	void	UpdateSettings() {};

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\PointShadows2.frag (PointShadows2.cpp ExtRender).
	bool	ExtSupportsRenderRect() const override { return true; }
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass				extPass;
	ExtUniformBuffer	extUniforms;
	IDirect3DTexture9*	copyTexture = nullptr;	// the buffer as the pass starts: made at first use, remade when its size changes
	IDirect3DSurface9*	copySurface = nullptr;

};
