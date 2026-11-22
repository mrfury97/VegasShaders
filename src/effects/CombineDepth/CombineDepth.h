#pragma once

class CombineDepthEffect : public EffectRecord
{
public:
	CombineDepthEffect() : EffectRecord("CombineDepth") {};

	HMODULE johnnyguitar = nullptr;
	bool(__cdecl* JG_SetClipDist)(float) = nullptr;
	float(__cdecl* JG_GetClipDist)() = nullptr;

	struct CombineDepthStruct {
		float viewNearZ;
	};
	CombineDepthStruct	Constants;

	struct CombineDepthTextures {
		IDirect3DTexture9* CombinedDepthTexture;
		IDirect3DSurface9* CombinedDepthSurface;
	};
	CombineDepthTextures	Textures;

	void	UpdateConstants();
	void	RegisterTextures();
	// Depth combine and normal reconstruction in one draw (two render targets). Returns false,
	// without rendering, when unavailable; the caller then runs the two passes separately.
	bool	RenderWithNormals(IDirect3DDevice9* Device, IDirect3DSurface9* NormalsSurface, bool allowInlineMerge = false, bool* outInlineMerged = nullptr);
	bool	mergedNormalsFailed = false;
	CachedHandle techDepthNormals;	// UNOFFICIAL: looked up once per loaded effect

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the depth pass as src\hlsl\NewVegas\Compute\CombineDepth.frag (EffectRecord::ExtRender); the
	// merged pass as DepthNormals.frag, two targets (ExtDepthNormals, called first by RenderWithNormals when the merged pass is on).
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	bool	ExtSupportsRenderRect() const override { return true; }
	// UNOFFICIAL: single-pass full-res depth combine + 2x2 plane-aware upsample directly from internal-res DepthTexture/DepthTextureViewModel.
	bool	RenderUpscaled(IDirect3DDevice9* Device, UINT renderWidth, UINT renderHeight, float worldJx, float worldJy, float vmJx, float vmJy);
private:
	bool	ExtDepthNormals(IDirect3DDevice9* Device, IDirect3DSurface9* NormalsSurface, bool allowInlineMerge, bool* outInlineMerged);
	ExtPass	extPass;
	ExtPass	extPassDepthNormals;
};