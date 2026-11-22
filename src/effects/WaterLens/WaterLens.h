#pragma once

class WaterLensEffect : public EffectRecord
{
public:
	WaterLensEffect() : EffectRecord("WaterLens") {};

	struct WaterLensStruct {
		Animator		WaterLensAnimator;
		D3DXVECTOR4		Data;
	};
	WaterLensStruct	Constants;

	float	amount = 0.0f;
	float	fadeValue = 0.0f;
	bool	wasUnderwater = false;
	bool	needsSync = true;
	double	lastUpdateTime = -1.0;

	void	Reset();
	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\WaterLens.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender();
};
