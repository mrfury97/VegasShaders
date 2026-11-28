#pragma once

class LensEffect : public EffectRecord
{
public:
	LensEffect() : EffectRecord("Lens") {};

	// One lens group: exponent applied to the bloom, overall strength, dirt amount.
	struct LensSettingsStruct { float bloomExponent, strength, smudginess; };

	struct LensSettings {
		LensSettingsStruct Main, Interiors, Night;
	};
	LensSettings	Settings;

	struct LensStruct { D3DXVECTOR4 Data; };
	LensStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Lens.frag (EffectRecord::ExtRender)
	bool	ExtSupportsRenderRect() const override { return true; }
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool	ShouldRender();
};
