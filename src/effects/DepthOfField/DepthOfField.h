#pragma once

class DepthOfFieldEffect : public EffectRecord
{
public:
	DepthOfFieldEffect() : EffectRecord("DepthOfField") {};

	struct ValuesStruct {
		float DistantBlur, DistantBlurStrength = 1.0f, DistantBlurStartRange, DistantBlurEndRange, BaseBlurRadius;
		float focusDistance, Radius, DiameterRange, NearBlurCutOff;
		int Mode;
		bool Enabled;
	};

	struct DepthOfFieldSettingsStruct {
		ValuesStruct	FirstPerson, ThirdPerson, VanityView;
	};
	DepthOfFieldSettingsStruct Settings;
	bool	JitterFix = true;	// UNOFFICIAL 2026-11-01 [Shaders.DepthOfField.Status] JitterFix

	struct DepthOfFieldStruct {
		bool			Enabled;
		D3DXVECTOR4		Blur, Data;
		D3DXVECTOR4		Jitter;		// UNOFFICIAL 2026-11-01 JitterFix: xy the depth's jitter after the resolve (TAAEffect::PostResolveDepthJitterUV)
	};
	DepthOfFieldStruct	Constants;

	void	UpdateConstants(), RegisterConstants(), UpdateSettings();
	bool	ShouldRender();
	// UNOFFICIAL 2026-10-25 (NVR DXVK): technique DistantBlurOnly as src\hlsl\NewVegas\Compute\DepthOfField.frag (EffectRecord::ExtRender)
	// UNOFFICIAL 2026-10-25 (NVR DXVK): technique AutofocusBokeh (0) as four pipelines and six passes through the frame chain (DepthOfFieldFocus,
	// Mosaic, Bokeh, Combine .frag): see ExtRender
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	ExtPass	extFocus, extMosaic, extBokeh, extCombine;

	// UNOFFICIAL 2026-10-26 (NVR DXVK, fork-only): [Shaders.DepthOfField.Bokeh]. With Bokeh on, technique 0 (the autofocus DoF) also
	// scatters every bright out-of-focus highlight as a sprite of the aperture's shape (disc or polygon) with compute atomics
	// (src\hlsl\NewVegas\Compute\DepthOfFieldBokehScatter.comp) and adds them in Combine (DepthOfFieldCombineBokeh.frag). Off, or
	// without the NVR DXVK extension: exactly the old DoF. With Bokeh on the autofocus DoF runs its Vulkan passes even when
	// NvrDxvkPasses is off (Render below); any failure turns the bokeh off for the session (logged) and the DoF goes on as before.
	struct BokehStruct {
		bool	Bokeh;
		int		BokehShape;			// 0 disc, 5..8 sides
		float	BokehThreshold;		// luma of the displayed colour above which a highlight scatters
		float	BokehIntensity;
		float	BokehSize;			// sprite radius at full blur, pixels at 1080p (scaled with the screen height)
		float	BokehRotation;		// polygon rotation, degrees
		int		BokehMaxSprites;
	};
	BokehStruct	Bokeh = {};
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	bool	BokehWanted() const { return Bokeh.Bokeh && !bokehFailed; }
	bool	BokehReady();
	bool	BokehScatter(IDirect3DDevice9* Device, IDirect3DBaseTexture9* depth);	// detect + splat into bokehAccum; false: not done
	void	BokehRelease();
	ID3D9NvrExtPipeline*	bokehScatter = nullptr;
	ID3D9NvrExtPipeline*	bokehCombine = nullptr;
	ID3D9NvrExtPipeline*	bokehResolve = nullptr;	// the sprite buffer into bokehTexture (half resolution, RGBA16F)
	IDirect3DTexture9*		bokehTexture = nullptr;
	IDirect3DSurface9*		bokehSurface = nullptr;
	ID3D9NvrExtBuffer*		bokehSprites = nullptr;
	ID3D9NvrExtBuffer*		bokehAccum = nullptr;
	UINT					bokehW = 0, bokehH = 0, bokehMax = 0;
	bool					bokehTried = false;
	bool					bokehFailed = false;
	UINT					bokehFrames = 0;
};