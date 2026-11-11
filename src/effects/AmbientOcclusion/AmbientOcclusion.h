#pragma once

class AmbientOcclusionEffect : public EffectRecord
{
public:
	AmbientOcclusionEffect() : EffectRecord("AmbientOcclusion") {};

	bool bNVAOLoaded = false;
	bool packedAOFailed = false;
	// Composite apply (set by ShaderManager around one Render call):
	// deferCombine -- run estimate and blurs only, leaving the result in ResultTexture() for the
	//                 fog pass to apply; deferredReady reports that it did.
	// combineOnly  -- run just the combine from ResultTexture() (fallback if the fog pass could not).
	bool deferCombine = false;
	bool deferredReady = false;
	bool combineOnly = false;
	IDirect3DTexture9* aoTexture[2] = {};
	IDirect3DSurface9* aoSurface[2] = {};
	// Quarter-resolution pair, used instead when [Main.Main.ReducedQuality] AOLowRes is on.
	IDirect3DTexture9* aoTextureLow[2] = {};
	IDirect3DSurface9* aoSurfaceLow[2] = {};
	// Which pair the last estimate wrote; combineOnly and the fog composite must read the same one.
	bool lastLowRes = false;
	CachedHandle techDedicated, paramLayout;	// UNOFFICIAL: looked up once per loaded effect
	// UNOFFICIAL 2026-10-17 Skylighting v2: the sky factor at the AO's resolution (G16R16F: factor, depth), written by the
	// SkyVisibilityV2 technique each frame v2 is on; skyReady says this frame's is there (the composites read it only then).
	IDirect3DTexture9* skyTexture = nullptr;
	IDirect3DSurface9* skySurface = nullptr;
	IDirect3DTexture9* skyTextureLow = nullptr;	// (with AOLowRes: the AO's quarter size)
	IDirect3DSurface9* skySurfaceLow = nullptr;
	bool skyReady = false;
	CachedHandle techSky;
	CachedHandle paramSkySmooth;	// UNOFFICIAL 2026-10-21 (alrocks) NVR_SkySmooth
	static bool SkyV2Active();
	IDirect3DTexture9* SkyTexture() const { return skyReady ? (lastLowRes ? skyTextureLow : skyTexture) : nullptr; }
	// UNOFFICIAL 2026-10-19 Skylighting v3 phase 1 ([Shaders.AmbientOcclusion.Skylighting] Reflections): the same texture (its b: the
	// sky visibility) for the sky reflections (Specular, Reflections), while that is on; and how much reflection stays fully hidden.
	bool	SkyReflections = false;
	float	SkyMinReflection = 0.1f;
	// UNOFFICIAL 2026-10-20 (skyamb): no '&& Enabled' any more: AmbientLight switches this effect off while it renders and can host the
	// sky test (below); SkyTexture() is null unless this frame's sky target was written.
	IDirect3DTexture9* SkyReflectionTexture() const { return SkyReflections ? SkyTexture() : nullptr; }
	// UNOFFICIAL 2026-10-21 (alrocks): smoothedNormals = AmbientLight's prep with the normal smoothed over low-poly faces (NVR_SkyPrep), or null.
	void RenderSkyVisibility(IDirect3DDevice9* Device, IDirect3DTexture9* smoothedNormals = nullptr);
	// UNOFFICIAL 2026-10-20 (skyamb) Skylighting under AmbientLight: while AmbientLight renders it switches this effect off (it does its own
	// AO), and the sky test that lives here went with it. Now AmbientLight hosts it outdoors whether or not this effect is on: the sky
	// constants are kept up (UpdateSkyConstants, also called by ShaderManager while this effect is off), this effect's sky pass runs just
	// before AmbientLight (ShaderManager) and AmbientLight's Apply takes the sky target's factor.
	void UpdateSkyConstants();
	// UNOFFICIAL 2026-10-28 (fork RT, [Shaders.AmbientOcclusion.RT]; core/RtAmbient.h, RtCore.h): this effect is the RT core's AO user.
	// RtRegister once before the core's first frame (ShaderManager, with the RT sun's); RtRecord at the core's point of the frame; RtRaw:
	// this frame's traced AO / sky values (AmbientLight's RT estimate reads them too), or null.
	static void RtRegister();
	static std::string RtStatusText();	// UNOFFICIAL 2026-11-01: the RT AO's state as of this frame, for the REPORT block
	static IDirect3DSurface9* RtRawSurface();	// UNOFFICIAL 2026-11-01: the last trace's output (x = AO visibility; null before the first), for the REPORT block
	void RtRecord(IDirect3DDevice9* Device);
	// UNOFFICIAL 2026-11-09 (diagnostic): the RT AO / GI buffers as PixelInspector stages (what the rays start from, what the helper
	// returned, what the denoisers made of it), so one capture says at which stage a wrong picture begins. EffectChain, after AmbientLight.
	void RtInspect();
	IDirect3DTexture9* RtRaw();
	IDirect3DTexture9* RtGI();	// (rt-ao-gi) this frame's traced bounce light, or null
	IDirect3DTexture9* RtDenoised(IDirect3DDevice9* Device);
	IDirect3DTexture9* RtDenoisedGI(IDirect3DDevice9* Device);	// (2026-10-29) the bounce light through RtDenoise's RGB mode (half), or null	// (2026-10-29) this frame's AO visibility through RtDenoise (.r, screen size), or null
	static bool SkyHostedByAmbientLight();
	static D3DXVECTOR2 SkyTakeComposite();	// LightCap for VolumetricFog's composites (Includes/SkyTake.hlsl): x the cap's scale, y no limit

	bool	UseLowRes() const;
	IDirect3DSurface9* NextResultSurface() const { return UseLowRes() ? aoSurfaceLow[0] : aoSurface[0]; }
	IDirect3DTexture9* ResultTexture() const { return lastLowRes ? aoTextureLow[0] : aoTexture[0]; }

	struct AmbientOcclusionStruct {
		bool			Enabled;
		D3DXVECTOR4		AOData;
		D3DXVECTOR4		Data;
	};
	AmbientOcclusionStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	void Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
		IDirect3DSurface9* SourceBuffer) override;

private:
	// UNOFFICIAL 2026-10-25 (NVR DXVK, [Main.Main.Performance] NvrDxvkPasses): the Dedicated techniques' four passes and the sky pass
	// (SkyVisibilityV2) as Vulkan passes (src\hlsl\NewVegas\Compute\AmbientOcclusion*.frag); false: nothing of the frame was committed.
	ExtPass				extEstimate, extBlur, extCombine, extSky;
	std::map<std::string, ExtPass> extVariants;	// UNOFFICIAL 2026-10-26: compile-time variants (EffectRecord::ExtVariant)
	ExtUniformBuffer	extUniforms, extSkyUniforms;
	bool	ExtConstants(ExtUniformBuffer& Uniforms, const D3DXVECTOR4& Layout, const D3DXVECTOR4& SkyLayout);
	bool	ExtDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DTexture9* const* Textures,
				IDirect3DSurface9* const* Surfaces, const D3DXVECTOR4& Layout, bool GTAOEstimate, IDirect3DTexture9* OrthoMap, IDirect3DTexture9* SkyVis);
	bool	ExtSkyVisibility(IDirect3DSurface9* Target, const D3DSURFACE_DESC& Desc, IDirect3DTexture9* OrthoMap, IDirect3DTexture9* SunMask, IDirect3DTexture9* SmoothedNormals);
	// UNOFFICIAL 2026-10-28 (fork RT, [Shaders.AmbientOcclusion.RT]; core/RtAmbient.h): the RT estimate (into the AO target 1), its
	// temporal pass (target 1 + last frame's history -> target 0 + this frame's history) and the RT sky pass. The history pair is made
	// at the AO target's size the first time it is needed (again when that size changes).
	ExtPass				extEstimateRT, extTemporalRT, extSkyRT, extInputRT;
	ExtUniformBuffer	extRtUniforms;
	IDirect3DTexture9*	rtInputTexture = nullptr;	// NVR_RTAOInput (half, G32R32F): what the helper's trace starts from
	IDirect3DSurface9*	rtInputSurface = nullptr;
	IDirect3DTexture9*	rtRawTexture = nullptr;		// NVR_RTAORaw (half, A16B16G16R16F): the trace's result
	IDirect3DSurface9*	rtRawSurface = nullptr;
	IDirect3DTexture9*	rtGITexture = nullptr;		// (rt-ao-gi) NVR_RTGIRaw (half, A16B16G16R16F): the bounce light
	IDirect3DSurface9*	rtGISurface = nullptr;
	IDirect3DTexture9*	rtLightTexture = nullptr;	// (GI v2) NVR_RTGIFrameLight (half, A16B16G16R16F): the lit frame for on-screen bounce hits
	IDirect3DSurface9*	rtLightSurface = nullptr;
	IDirect3DTexture9*	rtDenoisedTexture = nullptr;	// (2026-10-29) NVR_RTAODenoised (screen, R16F): RtDenoise's output
	IDirect3DSurface9*	rtDenoisedSurface = nullptr;
	IDirect3DTexture9*	rtDenoisedGITexture = nullptr;	// (2026-10-29) NVR_RTGIDenoised (half, A16B16G16R16F)
	IDirect3DSurface9*	rtDenoisedGISurface = nullptr;
	UINT				rtWidth = 0, rtHeight = 0;
	IDirect3DTexture9*	rtHistoryTexture[2] = {};
	IDirect3DSurface9*	rtHistorySurface[2] = {};
	UINT				rtHistoryWidth = 0, rtHistoryHeight = 0;
	int					rtHistoryIndex = 0;
	bool	ExtEstimateRT(IDirect3DDevice9* Device, IDirect3DTexture9* const* Textures, IDirect3DSurface9* const* Surfaces, const D3DSURFACE_DESC& Target);
	bool	ExtSkyVisibilityRT(IDirect3DSurface9* Target, IDirect3DTexture9* SunMask, IDirect3DTexture9* SmoothedNormals);
	void	RtTemporalSetup(const D3DSURFACE_DESC& AoDesc);	// UNOFFICIAL 2026-11-03: the RT temporal constants (ExtDedicated, Render's D3D9 loop)
};
