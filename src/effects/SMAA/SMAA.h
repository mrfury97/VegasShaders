#pragma once

class SMAAEffect : public EffectRecord
{
public:
	// UNOFFICIAL 2026-11-12 (memleak) idle targets (core/TextureManager.h, EffectRecord::TargetParkable): the edge and weight buffers are drawn only by Render; Debug's thumbnails read them through the slot
	bool	TargetParkable(const char* SamplerName) override { return true; }
	bool	TargetsWanted() override;	// (off, or standing aside while TAA runs: EffectChain's PostStep_SMAA)
	SMAAEffect() : EffectRecord("SMAA") {};

	enum Input { INPUT_LUMA, INPUT_COLOR, INPUT_DEPTH, INPUT_LUMADEPTH };

	struct SMAASettings {
		Input EdgeDetection;
		float Threshold;
		float DepthThreshold;
		float MaxSearchSteps;
		float SubpixelShift;
		float DepthOffset;
		float LocalContrast;
		bool Predication;					// UNOFFICIAL aastab: predicated luma threshold (EdgeDetection 3)
		float PredicationScale;
		float PredicationStrength;
		bool DeDither;							// UNOFFICIAL aastab: luma edges judged without the tonemap's dither noise (EdgeDetection 3)
	};
	struct SMAASettingsStruct {
		SMAASettings	Main;
	};
	SMAASettingsStruct Settings;

	struct SMAAStruct {
		D3DXVECTOR4		Resolution;
		D3DXVECTOR4		Data;	// x edge threshold, y relative depth threshold, z max search steps, w subpixel shift
		D3DXVECTOR4		DepthData;	// x depth buffer texel offset
		D3DXVECTOR4		Pred;			// UNOFFICIAL aastab: x predication on, y scale, z strength, w de-dither
	};
	SMAAStruct Constants;

	struct SMAATexturesStruct {
		IDirect3DTexture9* SMAA_Edges_Texture;
		IDirect3DSurface9* SMAA_Edges_Surface;
		IDirect3DTexture9* SMAA_Blend_Texture;
		IDirect3DSurface9* SMAA_Blend_Surface;
		IDirect3DSurface9* StencilSurface = nullptr;	// UNOFFICIAL: SMAA's own edge mask (D24S8, screen size, no MSAA)
		bool StencilFailed = false;
	};
	SMAATexturesStruct	Textures;
	CachedHandle techLuma, techColor, techDepth, techLumaDepth, techLumaDepthStable, techBlend, techNeighborhood;	// UNOFFICIAL: looked up once per loaded effect

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();

	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer);
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the three passes as src\hlsl\NewVegas\Compute\SMAAEdges/SMAAWeights/SMAANeighborhood.frag
	// (Render calls it first, as EffectRecord::Render does)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extEdges, extWeights, extNeighborhood;
	std::map<std::string, ExtPass> extVariants;	// UNOFFICIAL 2026-10-26: compile-time variants (EffectRecord::ExtVariant)

private:
	void EdgesDetectionPass(Input input);
	void BlendingWeightsCalculationPass();
	void NeighborhoodBlendingPass(IDirect3DSurface9* RenderTarget);
};
