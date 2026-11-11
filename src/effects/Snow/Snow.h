#pragma once

class SnowEffect : public EffectRecord
{
public:
	SnowEffect() : EffectRecord("Snow") {
		Constants.SnowAnimator.Initialize(0);
	};

	struct SnowStruct {
		Animator		SnowAnimator;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		WorldData;	// UNOFFICIAL 2026-11-09 WorldSpace: x flake radius, y density, z max distance, w opacity
		D3DXVECTOR4		WorldWind;	// x cloud wind strength, y cloud repeat cells, z plant wind strength
	};
	SnowStruct	Constants;

	// UNOFFICIAL 2026-11-09 [Shaders.Snow.Main] WorldSpace (macintroll's world-space snow, techniques 1 and 2 of Snow.fx.hlsl):
	// the technique this frame's Render is given. 0 = the camera-centred layers (default, and whenever the effect has no such
	// technique); 1 = the falling flakes; 2 = the marks on surfaces first (WorldSurfaceMarks; needs the Normals effect's buffer).
	bool	WorldSpace = false;
	bool	WorldSurfaceMarks = true;
	UINT	Technique();

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Snow.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;

};