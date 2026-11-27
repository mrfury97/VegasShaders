#pragma once

class SunShadowsEffect : public EffectRecord
{
public:

	SunShadowsEffect() : EffectRecord("SunShadows") {};

	struct SunShadowStruct {
	};
	SunShadowStruct	Constants;

	// Scratch targets so no pass samples TESR_PointShadowBuffer while also rendering into it.
	IDirect3DTexture9* scratchTexture[2] = {};
	IDirect3DSurface9* scratchSurface[2] = {};
	bool pingPongFailed = false;

	void	SetCT();
	void	RegisterTextures();
	bool	TargetParkable(const char* SamplerName) override { return true; }
	bool	TargetsWanted() override;

	void	UpdateConstants() {};
	void	RegisterConstants() {};
	void	UpdateSettings();

	// UNOFFICIAL 2026-10-17 FastPaths ([Shaders.ShadowsExteriors.ScreenSpace] FastPaths): techniques 3 and 4 of SunShadows.fx are
	// techniques 0 and 1 (ForwardContactShadows) with the march's early returns (contact shadows off, farther than the render
	// distance, turned away from the sun) made real: the effect compiler had flattened them, so every pixel ran the march. Same picture.
	bool	fastPaths = false;

	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the ping-pong path's passes as Vulkan passes (Compute\SunShadows*.frag), every technique,
	// through the same targets in the same order. Called by Render only (the in-place fallback stays D3D9). False: the D3D9 path runs.
	bool	ExtPingPong(IDirect3DDevice9* Device, UINT techniqueIndex, const D3DXTECHNIQUE_DESC& description,
		IDirect3DTexture9* shadowTexture, IDirect3DSurface9* shadowSurface);
	ExtPass	extPasses[5];	// march, blur, apply, sharp, sharp finish
	ExtUniformBuffer extUniforms;
	unsigned extForwardGeneration = 0;	// the load FORWARD_SHADOWS was read for (EffectRecord passes it to the compiler at load)
	bool	extForwardCompiled = false;
	bool	extCharCompiled = false;	// UNOFFICIAL 2026-10-26: CHARACTER_SHADOW_MAP as the loaded effect was compiled with (the Apply pass's c1 variant)
	std::map<std::string, ExtPass> extVariants;
};
