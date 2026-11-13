#pragma once
#include "../../core/Sm6Pass.h"	// UNOFFICIAL 2026-10-26 (deep/fork-sm6)

class VolumetricFogEffect : public EffectRecord
{
public:
	VolumetricFogEffect() : EffectRecord("VolumetricFog"), rainyDisablesSkyFilter(true), cloudyDisablesSkyFilter(true), weatherFilterWasActive(true),
		nightMinDarkness(1.0f - 0.35f) {	// matches ShadowsExteriors.Main's own NightMinDarkness default (0.35), until the first exterior settings pass sets it for real
		Constants.WeatherFilterAnimator.Initialize(1);
	};

	static const int LampHazeMax = 16;	// UNOFFICIAL 2026-11-03 LampHaze: LAMP_HAZE_MAX in VolumetricFog.fx.hlsl
	struct VolumetricFogStruct {
		// x: BaseDensity, y: WeatherImpact, z: MorningFogDip, w: SunriseSunsetBoost
		D3DXVECTOR4		Density;
		// x: HeightFalloff, y: MaxHeight, z: Extinction, w: Inscattering
		D3DXVECTOR4		Shape;
		// x: WindDirX, y: WindDirY, z: WindSpeed, w: NoiseScale
		D3DXVECTOR4		Wind;
		// x: PhaseAsymmetry, y: ShadowStrength, z: NoiseStrength, w: HeightInfluence
		D3DXVECTOR4		Scatter;
		// x: WeatherFilterBlend (animated 0-1, 1=sky mask active), y: isExterior, z: SkyAmbientAvailable, w: FogSaturation
		D3DXVECTOR4		Weather;
		// x: AerialStrength, y: AerialRangeStart, z: AerialTintBlend, w: AerialDayFadeStart
		D3DXVECTOR4		Aerial;
		// xyz: manual aerial tint override
		D3DXVECTOR4		AerialTintColor;
		// x: DistantFogRange, y: DistantFogBlend, z: DistantFogHeight, w: EdgeAA
		D3DXVECTOR4		Distant;
		// x: Amount, y: NightDisableSkyMask (Night-scoped, packed here for the spare slot), z: MoonVisibility, w: MinDensityFloor
		D3DXVECTOR4		Global;
		// Its own section/settings-UI tab (Shaders.VolumetricFog.Night), separate from Main, so
		// night-specific controls don't crowd the exterior tab as more of them get added.
		// x: DensityScale, y: AmountScale, z: HeightFalloffScale, w: MaxHeightOffset
		D3DXVECTOR4		Night;
		// x: NoiseStrengthScale, y: WindSpeedScale, z: ExtinctionScale, w: InscatteringScale
		D3DXVECTOR4		NightScatter;
		// xyz: multiplicative night fog color tint, w: NightAmbientStrength
		D3DXVECTOR4		NightTint;
		D3DXVECTOR4		Sun;             // UNOFFICIAL 2026-10-15 x: SunGlow, the fog's glow round the sun (1 = as inherited, 0 interiors); 2026-10-30 y: WeatherFogColor (0 interiors);
		                                 // 2026-10-30 z: GameFogFloor this frame (its Night scale faded in; 0 interiors / off), w: the weather's raw fog near (Sky, before the 0.17 x far stand-in)
		// Custom build: the fog from before the rewrite, for old presets ([Shaders.VolumetricFog.Main] LegacyFog).
		// x: 1 = legacy fog (C++ only: picks the *Legacy techniques)
		D3DXVECTOR4		Legacy;
		// x: FogSaturation, y: WeatherImpact, z: DistantFogRange, w: SunPower
		D3DXVECTOR4		LegacyLow;
		// x: HeightFogDensity, y: HeightFogFalloff, z: HeightFogDist, w: HeightFogSkyColor
		D3DXVECTOR4		LegacyHigh;
		// x: Extinction, y: Inscattering, z: FogNight, w: SimpleFogSkyColor
		D3DXVECTOR4		LegacySimple;
		// x: DistantFogBlend, y: HeightFogBlend, z: HeightFogRolloff, w: SimpleFogBlend
		D3DXVECTOR4		LegacyBlend;
		// x: DistantFogHeight, y: HeightFogHeight, z: SimpleFogHeight, w: isExterior
		D3DXVECTOR4		LegacyHeight;
		// x: MinimumBaseFog, y: HeightFogSaturation, z: Amount, w: HeightFogInscattering
		D3DXVECTOR4		LegacyData;
		// UNOFFICIAL 2026-11-03 LampHaze ([Shaders.VolumetricFog.Interiors]; SetLampHaze, VolumetricFog.fx.hlsl LampHazeLight):
		// x: LampHaze (0 = off: outdoors, LampHaze 0 or no lamp), y: lamps listed, z: anisotropy g, w: core share of the reach
		D3DXVECTOR4		Lamps;
		Animator		WeatherFilterAnimator;
	};
	VolumetricFogStruct	Constants;

	void	UpdateConstants(), RegisterConstants(), UpdateSettings();
	void	UpdateLegacySettings(const char* SettingCategory);
	bool	ShouldRender();

	// cached from UpdateSettings, consumed by UpdateConstants to drive WeatherFilterAnimator
	bool	rainyDisablesSkyFilter, cloudyDisablesSkyFilter;
	bool	weatherFilterWasActive; // edge-detect for the animator, mirrors RainAnimator.switched
	// UNOFFICIAL 2026-10-30 GameFogFloor ([Shaders.VolumetricFog.Main] GameFogFloor, [Shaders.VolumetricFog.Night] GameFogFloorScale; UpdateConstants)
	float	gameFogFloor = 0.0f, gameFogFloorNight = 1.0f;
	float	layerOn = 0.0f, layerBase = 0.0f, layerThickness = 2000.0f; bool layerFollowsCamera = false;	// UNOFFICIAL 2026-11-01 HeightLayer (UpdateSettings)
	// UNOFFICIAL 2026-11-13 (modern lighting) FoggyWeatherImpact: WeatherImpact as set, and the foggy weather's own share with the
	// fog far distances it fades in between (UpdateConstants). foggyImpact < 0: off (one WeatherImpact for every weather).
	float	weatherImpactSet = 0.5f, foggyImpact = -1.0f, foggyFar = 12000.0f, clearFar = 26000.0f;
	float	shapeFalloff = 50.0f, shapeMaxHeight = 3.0f;	// HeightFalloff / MaxHeight as set (Shape.xy carry the layer while it is on: ApplyLayer)
	void	ApplyLayer();
	// UNOFFICIAL 2026-11-03 LampHaze ([Shaders.VolumetricFog.Interiors] LampHaze, LampHazeAnisotropy, LampHazeCore, LampHazeMaxLamps; read
	// indoors only, 0 outdoors). SetLampHaze fills Constants.Lamps and lampTexture from this frame's lamps (Render, before any draw);
	// LampHazeOn: the lamps' light is in this frame's fog, so the Vulkan and SM6 passes (which do not have it) are not used.
	float	lampHaze = 0.0f, lampHazeAnisotropy = 0.3f, lampHazeCore = 0.25f; int lampHazeMaxLamps = 8;
	// The lamps themselves go in a 16 x 2 float texture on stage 14 (NVR_FogLamps; row 0: position relative to the camera + the game's
	// reach, row 1: colour linear x dimmer): as constant arrays they put the fog's passes over ps_3_0's 224 constant registers.
	IDirect3DTexture9* lampTexture = nullptr;
	void	SetLampHaze(IDirect3DDevice9* Device);
	bool	LampHazeOn() const { return Constants.Lamps.x > 0.0f && !IsLegacy() && !IsClassic(); }

	// cached from UpdateSettings (Shaders.ShadowsExteriors.Main/NightMinDarkness), consumed by
	// UpdateConstants to drive the moon-phase night-ambient ceiling
	float	nightMinDarkness;
	bool	packedFogFailed = false;
	bool	dedicatedFogFailed = false;
	// UNOFFICIAL: technique and parameter handles, looked up once per loaded effect (EffectRecord::CachedHandle).
	CachedHandle techCompositeFog, techDedicatedFog, techApplyShadowAO, techPackedFog, paramCompositeFlags, paramCompositeAOTexel, paramFogLayout;
	// Custom build: the same techniques with the legacy fog, used while LegacyFog is on.
	CachedHandle techCompositeFogLegacy, techDedicatedFogLegacy, techPackedFogLegacy;
	bool	IsLegacy() const { return Constants.Legacy.x > 0.5f; }
	// UNOFFICIAL 2026-10-18 ClassicLook ([Shaders.VolumetricFog.Main] ClassicLook): the look of the original NVR fog (upstream
	// master ab16e86c) with today's speed work, the *Classic techniques. 0 off (today's fog), 1 on, 2 on and worked out per
	// pixel at full resolution in one pass, as master drew it (VolumetricFogClassic; the shadow and AO applies then run as
	// their own passes). LegacyFog wins over it.
	int		classicLook = 0;
	bool	IsClassic() const { return classicLook > 0 && !IsLegacy(); }
	// DIAGNOSTIC (development builds only; [Main.Main.Performance] FogReferenceTest): technique FogReference, master's own fog
	// shader, full resolution, one pass, with the shadow and AO applies as their own passes, as master drew it.
	bool	referenceTest = false;
	bool	PlainOnly() const { return referenceTest || (IsClassic() && classicLook == 2); }
	CachedHandle techCompositeFogClassic, techDedicatedFogClassic, techCompositeFogInteriorClassic;
	D3DXHANDLE	CompositeTechnique() { return IsLegacy() ? TechniqueHandle(techCompositeFogLegacy, "CompositeFogLegacy") : IsClassic() ? TechniqueHandle(techCompositeFogClassic, "CompositeFogClassic") : TechniqueHandle(techCompositeFog, "CompositeFog"); }
	D3DXHANDLE	DedicatedTechnique() { return IsLegacy() ? TechniqueHandle(techDedicatedFogLegacy, "DedicatedFogLegacy") : IsClassic() ? TechniqueHandle(techDedicatedFogClassic, "DedicatedFogClassic") : TechniqueHandle(techDedicatedFog, "DedicatedFog"); }
	// (ClassicLook: no packed technique; a failed dedicated path falls straight to the full-resolution VolumetricFogClassic.)
	D3DXHANDLE	PackedTechnique() { return IsLegacy() ? TechniqueHandle(techPackedFogLegacy, "PackedFogLegacy") : IsClassic() ? NULL : TechniqueHandle(techPackedFog, "PackedFog"); }
	// UNOFFICIAL 2026-10-18 InteriorShadowFold: the composite with the interior darkening in place of the sun shadows.
	CachedHandle techCompositeFogInterior, techCompositeFogInteriorLegacy;
	D3DXHANDLE	CompositeInteriorTechnique() { return IsLegacy() ? TechniqueHandle(techCompositeFogInteriorLegacy, "CompositeFogInteriorLegacy") : IsClassic() ? TechniqueHandle(techCompositeFogInteriorClassic, "CompositeFogInteriorClassic") : TechniqueHandle(techCompositeFogInterior, "CompositeFogInterior"); }
	UINT	PlainTechniqueIndex(UINT techniqueIndex);
	// [0]: fog multiply + depth, [1]: fog add -- written together as MRTs by the estimate pass
	IDirect3DTexture9* fogTexture[2] = {};
	IDirect3DSurface9* fogSurface[2] = {};

	// Composite apply (set by ShaderManager for one Render call): the dedicated reconstruct also
	// applies the exterior sun-shadow composite and/or the deferred AO combine, saving their own
	// full-resolution passes. compositeApplied reports whether that happened.
	bool	compositeShadow = false;
	bool	compositeAO = false;
	IDirect3DTexture9* compositeAOTexture = nullptr;
	// UNOFFICIAL 2026-10-18 InteriorShadowFold: indoors, the interior darkening (ShadowsInteriors' unblurred apply) instead of
	// the sun shadows (technique CompositeFogInterior); compositeRound: NVR_CompositeFlags.w there (InteriorShadowFoldRound).
	bool	compositeInteriorShadow = false;
	float	compositeRound = 0.0f;
	// DIAGNOSTIC InteriorShadowFoldCheck: the dedicated pass reads this scene and writes this target (no chain, no copy, no fallback).
	IDirect3DTexture9* sceneOverride = nullptr;
	IDirect3DSurface9* targetOverride = nullptr;
	// UNOFFICIAL 2026-10-17 Skylighting v2: this frame's sky visibility (AmbientOcclusionEffect::SkyTexture), applied to the sky's
	// share with the AO (stage 12, NVR_SkyVis; NVR_CompositeFlags.z). Null = nothing changes.
	IDirect3DTexture9* compositeSkyTexture = nullptr;
	bool	compositeApplied = false;
	// UNOFFICIAL 2026-10-14 froxel grid (set by ShaderManager for the frame): the volumetric light's grid, read by the dedicated
	// path on stage 11 (NVR_FogGrid); gridApplied reports that the volumetric light's composite was done in the fog's pass.
	IDirect3DTexture9* gridTexture = nullptr;
	bool	gridApplied = false;
	CachedHandle paramGridFlags;
	// UNOFFICIAL 2026-10-19 SunFar: NVR_SunFarFlags, and the factor for a composite pass (sets the flag; null when none).
	CachedHandle paramSunFarFlags;
	IDirect3DTexture9* SunFarFor(bool shadow);
	bool	CanComposite(IDirect3DSurface9* aoSurface);
	bool	CanCompositeInterior() { return !PlainOnly() && CompositeInteriorTechnique() != NULL; }
	// How this GPU stores a float in a 16-bit float render target: 0 round to nearest even, 2 round toward zero, -1 neither
	// (or not measurable). Measured once (technique HalfStoreProbe), the first time the interior composite could run.
	int		HalfStoreMode(IDirect3DDevice9* Device);
	int		halfStoreMode = -2;	// -2: not measured yet
	// The shadow and AO applies in one pass of their own (technique ApplyShadowAO), for frames where effects between them
	// and the fog keep them out of the fog pass. Returns false, leaving the scene untouched, if it could not.
	bool	CanApplyShadowAO();
	bool	RenderShadowAO(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		bool shadow, IDirect3DTexture9* aoTexture);

	void	RegisterTextures();
	bool	RenderDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface);
	void Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
		IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
		IDirect3DSurface9* SourceBuffer) override;

	// UNOFFICIAL 2026-10-25 (NVR DXVK, [Main.Main.Performance] NvrDxvkPasses): the fog as Vulkan passes, line for line
	// (Compute/VolumetricFog.glsl, VolumetricFogEstimate.frag, VolumetricFogApply.frag). ExtDedicated: the dedicated and composite
	// techniques' two passes (RenderDedicated, after its own checks); ExtShadowAO: technique ApplyShadowAO (RenderShadowAO);
	// ExtRender: the full-resolution techniques (VolumetricFog, VolumetricFogLegacy, VolumetricFogClassic). Each returns false,
	// having drawn nothing into the frame, when it cannot run; the D3D9 technique then draws as before. PackedFog*, FogReference
	// and HalfStoreProbe stay D3D9.
	struct ExtFogPush {
		UInt32		pointIndex, linearIndex, wrapIndex, mode;	// the sampler heap indices (filled in by DXVK); the Apply shader's pass
		UInt32		legacy, classic, interior, physicalSky;
		D3DXVECTOR4	fogLayout, compositeFlags, compositeAOTexel, gridFlags, sunFarFlags;	// the effect's NVR_ constants
	};
	ExtPass		extEstimate, extApply;	// the general shaders (run-time switches)
	std::map<std::string, ExtPass> extVariants;	// UNOFFICIAL 2026-10-26: their compile-time variants (EffectRecord::ExtVariant)
	ExtUniformBuffer extUniforms;
	IDirect3DBaseTexture9* noiseTexture = nullptr;	// NVR_FogNoise (Render loads it)
	CachedHandle paramPhysSky;	// TESR_PhysSky: declared only when the effect was compiled with PHYSICAL_SKY_ON
	IDirect3DBaseTexture9* ExtTextureOf(const char* name);
	static const UINT ExtInputCount = 14;	// the Apply shader's textures (the Estimate shader declares the first 8; 13: the sky shares, UNOFFICIAL 2026-11-05)
	bool	ExtSetup(IDirect3DBaseTexture9* (&inputs)[ExtInputCount], ExtFogPush& push);
	bool	ExtApplyPass(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
		IDirect3DBaseTexture9* const (&inputs)[ExtInputCount], const ExtFogPush& push, bool last);
	bool	ExtDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DTexture9* scene,
		bool composite, const D3DXVECTOR4& layout, bool& grid);
	bool	ExtShadowAO(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, bool shadow, IDirect3DTexture9* aoTexture);
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	// UNOFFICIAL 2026-10-26 (deep/fork-sm6, [Main.Main.Performance] NvrSm6Shaders): RenderDedicated's two passes drawn from their SM6 build
	// (core\Sm6Pass.h, tools\nvr-sm6: the same .fx.hlsl without ps_3_0's limits, so with the sky look at the horizon), per technique
	// name and pass. Sm6Dedicated: both passes of the technique ready (else null: the D3D9 passes, or the hand-ported ones).
	std::map<std::string, Sm6Pass> sm6Passes;
	Sm6Pass*	Sm6For(D3DXHANDLE technique, UINT pass);
	bool	sm6Logged = false;
};
