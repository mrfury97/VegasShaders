#pragma once

class SnowAccumulationEffect : public EffectRecord
{
public:
	SnowAccumulationEffect() : EffectRecord("SnowAccumulation") {
		Constants.SnowAccumulationAnimator.Initialize(0);
		Constants.Data.w = 0.0f;
	};

	struct SnowAccumulationStruct {
		Animator		SnowAccumulationAnimator;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Color;
	};
	SnowAccumulationStruct	Constants;
	D3DXVECTOR4		Extra = { 0.0f, 0.0f, 0.0f, 0.0f };	// UNOFFICIAL 2026-10-28 TESR_SnowAccumulationExtra: x CapturedNormals, y CapturedNormalsDebugView

	bool	CapturedNormals = false;	// UNOFFICIAL 2026-10-28 [Shaders.SnowAccumulation.Main] CapturedNormals (core/CapturedNormals.h)
	bool	FollowJitter = true;		// UNOFFICIAL 2026-10-28 [Shaders.SnowAccumulation.Main] FollowJitter (SnowAccumulation.fx.hlsl SnowRay)
	int		Look = 0;					// UNOFFICIAL 2026-10-28 [Shaders.SnowAccumulation.Main] Look: 0 as before, 1 macintroll's version 5

	float increaseRate;
	float decreaseRate;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
	UINT	Technique(UINT persistent);	// UNOFFICIAL 2026-10-28: the technique for Look (persistent: CoverMap's 0 or 1)

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the five passes as src\hlsl\NewVegas\Compute\SnowAccumulation.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	bool	ExtSupportsRenderRect() const override { return true; }
private:
	ExtPass				extPass;
	ExtUniformBuffer	extUniforms;
};