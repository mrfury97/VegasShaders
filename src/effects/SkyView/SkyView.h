#pragma once

// UNOFFICIAL 2026-10-14: the physical sky's lookup table (Effects\SkyView.fx.hlsl). Filled once a frame while
// [Shaders.Sky.Physical] is on and outdoors (ShaderManager::RenderEffectsPreTonemapping); the sky shader (SKY.pso) reads it
// the next frame. Its settings and constants (TESR_PhysSky, TESR_PhysSkySun) belong to the sky shaders (Sky.cpp).
//
// UNOFFICIAL 2026-10-18: with ExactSunlight or MultipleScattering on, the table is drawn by technique 3 or 4, which read two
// small STATIC tables drawn by techniques 1 (sun transmittance, 256 x 64) and 2 (multiple scattering Psi_ms, 32 x 32). They depend
// only on Haze and GroundAlbedo, so they are drawn when those change or a switch is first turned on (fixes-a #48: no periodic refresh; the render targets
// are never lost in a running session). With both switches off none of this runs and technique 0 draws as before.
class SkyViewEffect : public EffectRecord
{
public:
	SkyViewEffect() : EffectRecord("SkyView") {};

	struct TexturesStruct {
		IDirect3DTexture9* TableTexture = nullptr;
		IDirect3DSurface9* TableSurface = nullptr;
		IDirect3DTexture9* TransTexture = nullptr;	// TESR_SkyTransLUT: the sun's optical depth by (altitude, sun cosine)
		IDirect3DSurface9* TransSurface = nullptr;
		IDirect3DTexture9* MSTexture = nullptr;		// TESR_SkyMSLUT: Psi_ms by (sun cosine, altitude)
		IDirect3DSurface9* MSSurface = nullptr;
	};
	TexturesStruct	Textures;

	void	UpdateConstants() {};
	void	RegisterTextures();
	bool	ShouldRender();
	void	RenderTable(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);
	// UNOFFICIAL 2026-10-25 (NVR DXVK): all five techniques as Vulkan passes (src\hlsl\NewVegas\Compute\SkyView*.frag, SkyTrans, SkyMS);
	// each draws into the surface Render is given (a table), so RenderTable is unchanged. Render calls it first.
	bool	ExtSupportsRenderRect() const override { return true; }
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	ExtPass	extPass[5];

private:
	bool	transBuilt = false, msBuilt = false;
	float	builtHaze = -1.0f, builtAlbedo = -1.0f;
};
