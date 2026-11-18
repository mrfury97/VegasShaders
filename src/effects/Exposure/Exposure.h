#pragma once

class ExposureEffect : public EffectRecord
{
public:
	ExposureEffect() : EffectRecord("Exposure") {};

	struct ValuesStruct {
		float MinBrightness, MaxBrightness, DarkAdaptSpeed, LightAdaptSpeed;
		float GT7AutoExposure, GT7WhiteBalance, GT7WeatherLight;	// UNOFFICIAL GT7 adaptation (Mode 1), 0-1 each
	};

	struct ExposureSettingsStruct {
		ValuesStruct Main, Night, Interiors;
		// UNOFFICIAL 2026-10-26 [Shaders.Exposure.Main] Histogram (needs NVR DXVK): the screen average leaves out the darkest LowPercent
		// and the brightest HighPercent of the pixels (AvgLuma.cpp RenderCompute, Compute\AvgLumaHistogram.comp). Live.
		bool	Histogram;
		float	HistogramLowPercent;
		float	HistogramHighPercent;
		// UNOFFICIAL [Shaders.Exposure.Main] Mode: 0 = the classic exposure (technique 0 of AvgLuma.fx and Exposure.fx, unchanged),
		// 1 = GT7-style adaptation (Polyphony Digital, SIGGRAPH 2025: two-stage auto-exposure with full-field metering and auto white
		// balance; technique 1 "GT7" of both files, Effects/Includes/GT7Adaptation.hlsl). Live.
		int		Mode;
	};
	ExposureSettingsStruct	Settings;

	struct ExposureStruct {
		D3DXVECTOR4 Data;
		D3DXVECTOR4 GT7;	// UNOFFICIAL TESR_GT7AdaptData: x GT7AutoExposure, y GT7WhiteBalance, z GT7WeatherLight (blended), w Mode
	};
	ExposureStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();

	// UNOFFICIAL GT7 adaptation: Mode 1 on. The technique both AvgLuma and Exposure draw (1 = GT7), and whether the exposure pass
	// changes nothing this frame (the classic skip at MaxBrightness 0; GT7 with both strengths at 0).
	bool	GT7Mode() const { return Settings.Mode == 1; }
	UINT	Technique() const { return GT7Mode() ? 1 : 0; }
	bool	IsIdentity() const { return GT7Mode() ? (Constants.GT7.x == 0.0f && Constants.GT7.y == 0.0f) : Constants.Data.y == 0.0f; }

	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Exposure.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	// UNOFFICIAL 2026-11-15 (fpnative-fix, docs/next/FPNATIVE-SEETHROUGH.md): the frame number the pass last drew in (the chain may skip
	// it: off, identity); FirstPersonNative::Flush gives the full-size arms the same exposure only in such a frame.
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* Target, IDirect3DSurface9* Rendered, UINT techniqueIndex, bool ClearTarget, IDirect3DSurface9* Source) override;
	UInt32	DrawnFrame = 0xFFFFFFFF;
private:
	ExtPass				extPass;
};
