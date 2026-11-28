#pragma once

// 2026-11-04 local exposure ([Shaders.LocalExposure], off by default): each part of the picture exposed for its own brightness,
// as the eye and Unreal Engine 5's Local Exposure do -- the shade under a desert rock no longer goes black next to sunlit sand,
// a doorway seen from a dark room no longer burns white. Three small passes before the tone mapper measure each neighbourhood's
// log brightness at 1/16 resolution and blur it (Effects\LocalExposure.fx.hlsl); the tone mapper (both ISHDRBLENDINSHADERCIN
// shaders, Includes/Tonemapping.hlsl localExposure) then lifts dark neighbourhoods and calms bright ones toward a mid grey, less
// at edges (no halos). docs/EFFECTS.md "LocalExposure".
class LocalExposureEffect : public EffectRecord
{
public:
	LocalExposureEffect() : EffectRecord("LocalExposure") { ZeroMemory(&Constants, sizeof(Constants)); };

	struct SettingsStruct {
		float	Shadows;		// how much dark neighbourhoods are lifted (0..1)
		float	Highlights;		// how much bright neighbourhoods are calmed (0..1)
		float	EdgeGuard;		// how fast the correction backs off where a pixel differs from its neighbourhood (0 = never)
		float	Blur;			// neighbourhood size: the blur's sigma in buffer texels (1 texel = 16 screen pixels)
		bool	Interiors;
		bool	Exteriors;
		int		DebugView;		// 1: the measured neighbourhood brightness as grey
	};
	SettingsStruct	Settings;

	struct ConstantsStruct {
		D3DXVECTOR4	Data;	// TESR_LocalExposureData: x shadows, y highlights, z edge guard, w 1 = the buffer was drawn this frame (0: skip)
		D3DXVECTOR4	Size;	// TESR_LocalExposureSize: xy one buffer texel, zw one screen pixel
		D3DXVECTOR4	Blur;	// TESR_LocalExposureBlur: x sigma in buffer texels, y DebugView
	};
	ConstantsStruct	Constants;

	struct TexturesStruct {
		IDirect3DTexture9* BufferTexture = nullptr;	// TESR_LocalExposureBuffer: the blurred log2 brightness (1/16 resolution)
		IDirect3DSurface9* BufferSurface = nullptr;
		IDirect3DTexture9* TempTexture = nullptr;	// TESR_LocalExposureTemp: the horizontal blur's result
		IDirect3DSurface9* TempSurface = nullptr;
		IDirect3DTexture9* MeanTexture = nullptr;	// TESR_LocalExposureMean: the whole view's mean log2 brightness (1x1)
		IDirect3DSurface9* MeanSurface = nullptr;
	};
	TexturesStruct	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	bool	TargetParkable(const char* SamplerName) override { return true; }
	bool	ExtSupportsRenderRect() const override { return true; }
	// The frame's step (EffectChain.cpp PreStep_LumaExposureBloom, right after the exposure pass): the three passes, and the flag
	// the tone mapper reads (cleared first, set once the buffer is drawn this frame).
	void	RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);

private:
	ExtPass				extMeasure{}, extBlurH{}, extBlurV{}, extMean{};
	ExtUniformBuffer	extUniforms{};
	bool	ExtRenderFrame(IDirect3DDevice9* Device);
};
