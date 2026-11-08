#pragma once
#include "../../core/TemporalHistoryStats.h"
#include "../../core/RenderRect.h"

// UNOFFICIAL: volumetric sun light (Effects\VolumetricLight.fx.hlsl) -- shafts of sunlight through the air, shaded by the
// sun's cascaded shadow maps. A half-resolution march (noise-dithered start points), an optional history blend and a
// depth-aware blur into its own buffers, then a full-resolution composite in the HDR chain, after the fog. Written for
// this build after the idea in Physically-Based-Reloaded (no shared code).
class VolumetricLightEffect : public EffectRecord
{
public:
	VolumetricLightEffect() : EffectRecord("VolumetricLight") {};

	// The look by day ([Shaders.VolumetricLight.Main]) and by night ([Shaders.VolumetricLight.Night]), blended by the
	// time of day.
	struct LookStruct {
		float	Strength;		// how much sunlight the lit air adds
		float	Darken;			// how much the shaded air dims what is behind it
		float	Density;		// air density, per 1000 game units (the menu's precision)
		float	MaxDistance;	// how far the march reaches, game units
		float	Anisotropy;		// forward scattering (Henyey-Greenstein g)
	};
	struct SettingsStruct {
		LookStruct	Day;
		LookStruct	Night;
		int		Steps;			// shadow samples per march
		float	Temporal;		// share of last frame's result kept (0: no history)
		float	BlurRadius;		// denoise blur, half-resolution texels each way
		float	RainStrength;	// share of the strength in rain
		bool	Froxels;		// UNOFFICIAL 2026-10-14: the froxel grid instead of the march (see the .fx)
		bool	FogVolume;		// UNOFFICIAL 2026-10-14: [Shaders.VolumetricFog.Main] FogVolume, the fog's density in the grid too
	};
	SettingsStruct	Settings;

	struct ConstantsStruct {
		D3DXVECTOR4	Data;		// x: strength, y: darkening, z: density per game unit, w: march distance
		D3DXVECTOR4	Data2;		// x: steps, y: g, z: fade, w: noise frame offset
		D3DXVECTOR4	Data3;		// x: history weight this frame, y: blur radius
		D3DXVECTOR4	Layout;		// xy: one half-resolution texel in screen UV
		D3DXMATRIX	Reproject;	// camera-relative world -> last frame's clip space
		D3DXVECTOR4	GridLayout;	// xy: froxel grid columns and rows, zw: one atlas texel in UV
		D3DXVECTOR4	GridData;	// x: fog volume on, y: scale of the lit share in the grid (keeps it in half-float range)
		D3DXVECTOR4	Rt;			// UNOFFICIAL 2026-11-07 (ptaaa-fog) TESR_VolumetricLightRt (core/RtFogVisibility.h Constant): x 0 the cascades,
								// 1 the traced visibility on this grid, 2 on another MaxDistance's; y RT range, z fade, w the traced MaxDistance
	};
	ConstantsStruct	Constants;

	struct TexturesStruct {
		IDirect3DTexture9* MarchTexture = nullptr;
		IDirect3DSurface9* MarchSurface = nullptr;
		IDirect3DTexture9* BlurTexture = nullptr;
		IDirect3DSurface9* BlurSurface = nullptr;
		IDirect3DTexture9* TempTexture = nullptr;
		IDirect3DSurface9* TempSurface = nullptr;
		IDirect3DTexture9* HistoryTexture = nullptr;
		IDirect3DSurface9* HistorySurface = nullptr;
		IDirect3DTexture9* GridTexture = nullptr;		// the froxel grid's 64 slices, 8 x 8 tiles (RGBA half float)
		IDirect3DSurface9* GridSurface = nullptr;
		IDirect3DTexture9* GridTempTexture = nullptr;
		IDirect3DSurface9* GridTempSurface = nullptr;
		// UNOFFICIAL 2026-11-07 (ptaaa-fog) the traced sun visibility per grid cell (core/RtFogVisibility.h; R16F, the grid's layout),
		// TESR_RtFogVisibility: the RT core copies it in each frame it traces
		IDirect3DTexture9* RtVisibilityTexture = nullptr;
		IDirect3DSurface9* RtVisibilitySurface = nullptr;
	};
	TexturesStruct	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	void	RenderBuffers(IDirect3DDevice9* Device);	// march, history, blur: everything before the composite
	// UNOFFICIAL 2026-11-07 (ptaaa-fog) the grid's / march's reach this frame (Data.w: the day and night MaxDistance by the time of day), for
	// the RT fog visibility's trace at the RT point of the frame (before this effect's UpdateConstants may have run)
	float	GridDistance() const;
	// UNOFFICIAL 2026-10-27 RenderScale v2: not the grid in a rect frame (its 8 x 8 slice atlas is screen-shaped, so the rect viewport
	// would cut it); the march runs instead.
	UINT	CompositeTechnique() const { return Settings.Froxels && Textures.GridSurface && !RenderRect::Active() ? 8 : 0; }
	// UNOFFICIAL 2026-10-25 (NVR DXVK): every technique as src\hlsl\NewVegas\Compute\VolumetricLight<Technique>.frag
	// (EffectRecord::ExtRender): the half-resolution and grid passes into the effect's own buffers (ExtDrawTo), the two
	// composites through the frame chain. The constants go to one uniform buffer (four shadow matrices do not fit push data).
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	// UNOFFICIAL 2026-11-01 the report burst's VL stages (core/Diagnostics.cpp): ShaderManager.cpp calls this right before (after = false)
	// and right after the composite, only while Diagnostics::VLCapturing: the frame before / after it and the path the composite took.
	void	ReportComposite(IDirect3DDevice9* Device, bool after);

	ExtPass				extPasses[9];	// by technique; Blur X and Blur Y share [2]
	ExtUniformBuffer	extUniforms;

private:
	bool		extDrew = false;	// UNOFFICIAL 2026-11-01: the last Render's technique drew as its Vulkan pass (ExtRender), for the report burst
	IDirect3DBaseTexture9*	fogNoise = nullptr;	// Textures\Effects\NoiseVolume.dds, loaded on the grid's first frame (stage 9 while the cells are lit)
	bool		fogNoiseLoaded = false;
	D3DXMATRIX	lastView, lastInvView, lastProj;
	UINT		lastRenderedFrame = 0;	// TheFrameRateManager->FrameNumber of the last frame this effect drew (UNOFFICIAL 2026-10-18: it counted
										// UpdateConstants calls, two a frame, so last frame's history was never accepted)
	bool		haveHistory = false;
	bool		lastRect = false;		// UNOFFICIAL 2026-10-27 RenderScale v2: the last drawn frame was a rect frame
	TemporalHistoryStats	stats;		// the march path's history use, for the log
	void		ReportHistory(const char* when);
};
