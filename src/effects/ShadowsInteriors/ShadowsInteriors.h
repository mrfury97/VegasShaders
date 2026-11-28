#pragma once

class ShadowsInteriorsEffect : public EffectRecord
{
public:
	ShadowsInteriorsEffect() : EffectRecord("ShadowsInteriors") {};

	struct ShadowsInteriorsStruct {
	};
	ShadowsInteriorsStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();

	// UNOFFICIAL: the dedicated path's apply pass alone, on the unblurred mask (interior forward shadows' darkening).
	bool	skipBlur = false;

	// Blurs the shadow term on the two G16R16 scratch targets and touches the HDR frame only in the
	// final pass (see the DedicatedInteriorShadows technique); falls back to the generic path.
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	bool	DedicatedUsable(IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface);
	bool	dedicatedFailed = false;
	CachedHandle techDedicated;	// UNOFFICIAL: looked up once per loaded effect

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the dedicated path's passes as Vulkan passes (ShadowsInteriorsBlur.frag, ShadowsInteriorsApply.frag):
	// the two blurs into the scratch targets, the apply into the frame. The generic technique stays D3D9. False: the D3D9 path runs.
	bool	ExtSupportsRenderRect() const override { return true; }
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extBlur, extApply;
};