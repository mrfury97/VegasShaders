#pragma once

#include "../../core/NvrDxvkExt.h"

class AvgLumaEffect : public EffectRecord
{
public:
	AvgLumaEffect() : EffectRecord("AvgLuma") {};

	struct AvgLumaStruct {
	};
	AvgLumaStruct	Constants;

	struct AvgLumaTextures {
		IDirect3DTexture9* AvgLumaTexture;
		IDirect3DSurface9* AvgLumaSurface;
	};
	AvgLumaTextures	Textures;

	void	UpdateConstants() {};
	void	RegisterTextures();

	// UNOFFICIAL 2026-10-23 (NVR DXVK, core\DxvkCompute.h): the same pass as a compute reduction
	// (src\hlsl\NewVegas\Compute\AvgLuma.comp): every pixel read once, the average exact, the rest of
	// the pass's logic unchanged; the result lands in TESR_AvgLumaBuffer as before. On only with
	// [Main.Main.Performance] AvgLumaCompute AND the NVR DXVK extension; false = not done, the caller
	// runs the D3D9 pass (ShaderManager::RenderEffectsPreTonemapping).
	bool	RenderCompute(IDirect3DDevice9* Device);
	// UNOFFICIAL test tool: AvgLumaComputeCheck = N > 0 frames: every N frames runs both paths and logs them
	// side by side (compute's exact average vs the D3D9 pass's 112 taps); the D3D9 pass keeps the texture
	// in that mode. Waits for the GPU on those frames: test only.
	void	CheckCompute(IDirect3DDevice9* Device, bool computeRan);

private:
	struct ComputePush {	// AvgLuma.comp's push block (112 bytes), and AvgLumaHistogram.comp's (+ histogram: 128)
		UINT32		width, height, groupsX, groupsY, phase, samplerIndex;
		float		scaleX, scaleY;
		D3DXVECTOR4	motionBlurData, depthOfFieldData, gameTime, exposureData, cameraData;
		D3DXVECTOR4	histogram;	// x low fraction, y high fraction, z log2 of the lowest bin's top, w log2 of the top bin's bottom
	};
	static_assert(sizeof(ComputePush) == 128, "ComputePush must match AvgLumaHistogram.comp Push block");
	bool	ComputeReady();		// creates the pipeline and buffers on first use; false once anything failed
	void	ComputeRelease();

	// UNOFFICIAL 2026-10-26 [Shaders.Exposure.Main] Histogram: the trimmed average (Compute\AvgLumaHistogram.comp) instead of the exact
	// one; same buffers and texture, its own pipeline and a 128-bin count buffer (cleared every frame). Needs NVR DXVK, not AvgLumaCompute.
	ID3D9NvrExtPipeline*	histogramPipeline = nullptr;
	ID3D9NvrExtBuffer*		histogramBins = nullptr;
	UINT					histogramFrames = 0;

	ID3D9NvrExtPipeline*	computePipeline = nullptr;
	ID3D9NvrExtBuffer*		computePartials = nullptr;
	ID3D9NvrExtBuffer*		computeState[2] = {};	// (newLuma, animatedLuma, focus, 1) floats; packed halves; exact average
	ID3D9NvrExtSampler*		computeSampler = nullptr;
	UINT					computeWidth = 0, computeHeight = 0, computeGroupsX = 0, computeGroupsY = 0;
	int						computeCurrent = 0;		// the state buffer written last
	bool					computeTried = false;
	bool					computeOk = false;
	UINT					computeFrames = 0;
	UINT					checkFrames = 0;
};
