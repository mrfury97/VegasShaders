#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// UNOFFICIAL: volumetric clouds (Effects\VolumetricClouds.fx.hlsl) -- a layer of 3D clouds marched and lit by the sun, in
// place of the game's flat cloud layers. Each frame: a march of one pixel in four of a quarter-resolution grid, a
// temporal pass that keeps the other three from earlier frames, then a full-resolution composite over the sky at the
// start of the HDR chain. Coverage follows the game's current weather (its own cloud textures, or its type), so console
// weather commands and weather mods drive these clouds too; the light follows its sun, moon and sky colours.
// The game's cloud layers are hidden (SKYTEX.pso, TESR_CloudsHide) except in the water's reflection.
class VolumetricCloudsEffect : public EffectRecord
{
public:
	VolumetricCloudsEffect() : EffectRecord("VolumetricClouds") {};

	struct SettingsStruct {
		float	Base, Thickness;				// metres
		float	Coverage[4];				// pleasant, cloudy, rainy, snow
		float	TypeBias[4];				// flat layers (-1) .. tall heaps (+1), the same four
		float	Density;
		float	ShapeSize, DetailSize, WeatherSize;	// metres per tile of each noise texture
		float	Erosion;
		float	SunStrength, AmbientStrength, Forward;
		int		Steps;
		float	MaxDistance, HazeDistance;	// metres
		float	WindSpeed, Evolution;		// wind multiplier, detail drift in metres per second
		float	History;					// how much of the earlier frames each refresh keeps
		bool	JitterWrap;
		bool	Stable;
		float	Brightness;
		float	EdgeSharpness, ShapeSmoothing, Turbulence, DetailSmoothness;
		bool	HideGameClouds;
		bool	GameTime;					// UNOFFICIAL: the wind follows the game's clock (waiting moves the clouds; the same time, the same sky)
		float	Shadows;					// UNOFFICIAL: how much the clouds shade the ground (0 off)
		float	ShadowRange;				// UNOFFICIAL: metres across the cloud shadow map
		float	WaterReflections;			// UNOFFICIAL: the clouds drawn into the game's water reflection (0 off)
		bool	ShadowPrefilter;			// UNOFFICIAL 2026-10-27: the shadow map from the smooth shape only, several taps per texel (no speckles)
		float	ShadowSoftness;				// UNOFFICIAL 2026-10-27: how far those taps spread, in shadow map texels
		bool	PhysicalShadows;			// UNOFFICIAL 2026-10-31: cloud shadows as real ones (full strength, soft edges, low sun kept)
		bool	PhysicalMoonlight;			// UNOFFICIAL: the moon lights the clouds with all the weather's night light (off: 15% of it, as before)
		bool	PhysicalSkyLight;			// UNOFFICIAL: with the physical sky, the sky light on the clouds at its physical brightness (off: matched to the weather's sky colours)
		bool	ShadowDebug;				// UNOFFICIAL: show the cloud shadow alone (white lit, black shaded)
		int		ReflectionDebug;			// UNOFFICIAL: in the water reflection, red where it looks up (sky), blue where down
		float	FineDetail;					// UNOFFICIAL: a second, finer layer of edge detail (0 off)
		float	FineDetailSize;				// UNOFFICIAL: its size, metres per tile
		float	CumulusShape;				// UNOFFICIAL 2026-10-20: heaped fair-weather clouds (flat bases, domed tops, no wispy fringe); 0 the older slabs
		float	Towering;					// UNOFFICIAL 2026-10-25: 0 off .. 1: the strongest heaps rise on as towers, TowerHeight times the layer's Thickness
		float	TowerHeight;				// UNOFFICIAL 2026-10-25: how tall the towers get, in layer thicknesses
		float	TowerAnvil;					// UNOFFICIAL 2026-10-25: 0 .. 1: the towers spread into a flat-topped anvil at the top (cumulonimbus)
		float	Sharpness;					// UNOFFICIAL: 0 the old soft look .. 1 crisp (sharper history, sharpened composite, finer noise far off)
		float	FromWeatherTextures;		// UNOFFICIAL: share of the coverage taken from the weather's own cloud textures (0: its type alone)
		bool	FixedMarchExit;				// UNOFFICIAL 2026-10-17: technique 6 (MarchFixedExit): the march stops at Steps / when opaque (the compiler had killed the exit)
		float	WeatherTint;				// UNOFFICIAL 2026-10-25: how much the weather's own cloud colours (sunset orange, overcast grey, night blue) tint the clouds (0 off)
	};
	SettingsStruct	Settings = {};

	struct ConstantsStruct {
		D3DXVECTOR4	Shape;		// x: base altitude (m), y: thickness (m), z: coverage, w: density
		D3DXVECTOR4	Noise;		// x: shape tile (m), y: detail tile (m), z: erosion, w: weather map tile (m)
		D3DXVECTOR4	Light;		// x: sun strength, y: ambient strength, z: forward scattering, w: rain darkening
		D3DXVECTOR4	Wind;		// xy: wind offset (m), z: detail drift (m), w: cloud type bias
		D3DXVECTOR4	March;		// x: steps, y: max distance (m), z: frame number, w: haze distance (m)
		D3DXVECTOR4	Layout;		// xy: one quarter-resolution texel in UV, z: history weight, w: brightness
		D3DXVECTOR4	Blocks;		// xy: one march texel in UV, zw: the block's pixel marched this frame
		D3DXVECTOR4	Hide;		// x: 1 = the game's cloud layers are hidden (set per draw by SetShadersHook)
		D3DXVECTOR4	Look;		// x: edge sharpness, y: shape noise mip bias, z: turbulence, w: smooth share of the detail
		D3DXMATRIX	PrevViewProj;	// last frame's view-projection
		D3DXVECTOR4	Extra;		// UNOFFICIAL x: shadow strength, y: shadow map width (m), w: shadow debug view
		D3DXMATRIX	ReflectInvVP;		// UNOFFICIAL: the reflection camera's clip space -> world direction
		D3DXMATRIX	HistoryViewProj;	// UNOFFICIAL: the view-projection of the frame whose clouds are in the history
		D3DXVECTOR4	ReflectData;		// UNOFFICIAL x: strength, yz: the reflection's size, w: the far plane's depth
		D3DXVECTOR4	Detail;			// UNOFFICIAL x: fine detail strength (0 off), y: fine detail tile (m), z: cloud shadow map size (texels)
		D3DXVECTOR4	Motion;			// UNOFFICIAL xy: the cloud pattern's move this frame (m), w: sharpness 0..1
		D3DXVECTOR4	Tint;			// UNOFFICIAL 2026-10-25 xyz: the weather's cloud colour now (linear, its brightness taken out), w: WeatherTint
		D3DXVECTOR4	Physical;		// UNOFFICIAL TESR_CloudsPhysical x: PhysicalMoonlight, y: PhysicalSkyLight (1 / 0)
		D3DXVECTOR4	Tower;			// UNOFFICIAL 2026-10-25 x: the ordinary layer's share of the taller one (1 off), y: towering 0..1, z: anvil 0..1
		D3DXVECTOR4	HistoryScale = D3DXVECTOR4(1.0f, 1.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-10-27 RenderScale v2: xy the part of the history its frame drew; 2026-10-28 zw: the composite's jitter offset (screen uv)
	};
	ConstantsStruct	Constants = {};

	struct TexturesStruct {
		IDirect3DTexture9* CurrentTexture = nullptr;
		IDirect3DSurface9* CurrentSurface = nullptr;
		IDirect3DTexture9* CurrentFullTexture = nullptr;
		IDirect3DSurface9* CurrentFullSurface = nullptr;
		IDirect3DTexture9* HistoryTexture[2] = {};
		IDirect3DSurface9* HistorySurface[2] = {};
		IDirect3DTexture9* HistoryRead = nullptr;	// the slot TESR_CloudHistory follows
		IDirect3DTexture9* ShadowTexture = nullptr;	// UNOFFICIAL: the cloud shadow map (256 x 256)
		IDirect3DSurface9* ShadowSurface = nullptr;
		IDirect3DTexture9* ProbeTexture = nullptr;	// UNOFFICIAL: the layer probe's 1 x 1 target
		IDirect3DSurface9* ProbeSurface = nullptr;
		IDirect3DTexture9* SkyProbeTexture = nullptr;	// UNOFFICIAL 2026-10-30: the cloud sky probe (64 x 64 hemi-octahedral, TESR_CloudSkyProbe)
		IDirect3DSurface9* SkyProbeSurface = nullptr;
	};
	TexturesStruct	Textures;
	UINT	shadowMapSize = 256;	// UNOFFICIAL: ShadowResolution, read once at start

	float	HideGameClouds = 0.0f;	// 1 while these clouds replace the game's, for SetShadersHook
	float	HideGameCloudsInReflection = 0.0f;	// UNOFFICIAL: 1 while these clouds are drawn into the water's reflection too
	// UNOFFICIAL: the clouds in the game's water reflection. CaptureReflection (SetShadersHook, in the reflection pass) notes the
	// pass's target, depth buffer and camera; DrawIntoReflection (RenderReflectionsHook, after the pass) draws them there.
	void	CaptureReflection(IDirect3DDevice9* Device);
	void	DrawIntoReflection(IDirect3DDevice9* Device);
	void	BeforeResolve(IDirect3DDevice9* Device, IDirect3DSurface9* source);	// the device's StretchRect hook
	void	EndReflectionPass(IDirect3DDevice9* Device);	// RenderReflectionsHook, after the game's pass

	void	UpdateConstants();
	void	RegisterConstants();
	static bool MoonPlace(Sky* sky, D3DXVECTOR3& direction, float& distance, float& radius, const char** why = nullptr);
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	bool	TargetParkable(const char* SamplerName) override { return strcmp(SamplerName, "NVR_CloudProbe") != 0; }
	// March, temporal, then the composite into the frame chain.
	void	RenderClouds(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
		IDirect3DSurface9* SourceSurface);
	// UNOFFICIAL 2026-10-21 cloud shadows through SunFar: the shadow map for this frame (once; RenderClouds draws it if nothing
	// asked earlier), the strength SunFar puts on the land (0: no shadows to hand over), and whether SunFar drew them this frame
	// (it sets this; the composite then leaves the land alone).
	void	RenderShadowMap(IDirect3DDevice9* Device);
	// UNOFFICIAL 2026-10-30 the cloud sky probe for reflections ([Shaders.Reflections.Main] CloudSky; NVR DXVK passes only): the clouds over
	// the whole upper sky (VolumetricCloudsSkyProbe.frag), drawn after this frame's clouds when Reflections asks. SkyProbeReady: drawn
	// this frame or the one before.
	void	RenderSkyProbe(IDirect3DDevice9* Device);
	bool	SkyProbeReady() const { return skyProbeFrame != UINT_MAX && skyProbeBands >= 8 && frame - skyProbeFrame <= 2; }
	UINT	skyProbeFrame = UINT_MAX;
	UINT	skyProbeBands = 0;	// bands drawn since start (the probe is whole after 8; one 8-row band a frame)
	float	ShadowStrengthForSunFar();
	bool	shadowsViaSunFar = false;
	// UNOFFICIAL 2026-10-30 (RT sun CloudShadows): where the shadow map last drawn lies: its centre on the cloud base (metres), its light
	// (normalized, z at least 0.25), width (metres), cloud base (metres), as ShadowCentre / ShadowLight worked them out
	bool	rtMapValid = false;
	float	rtMapCentre[2] = {}, rtMapLight[3] = {}, rtMapRange = 0.0f, rtMapBase = 0.0f;
	// UNOFFICIAL 2026-10-21, test tool (SettingShots 'cloudcoverage'): puts the eased coverage away from the weather's, as right after a
	// load or a weather change, so a test can see what a later step does to the easing (it eases back from there).
	void	TestSetCoverage(float value) { coverage = std::clamp(value, 0.0f, 1.0f); }
	// UNOFFICIAL 2026-10-25 (NVR DXVK, [Main.Main.Performance] NvrDxvkPasses): techniques 0 (composite), 1 and 6 (march), 2 (temporal)
	// and 4 (shadow map) as Vulkan passes (Compute/VolumetricClouds*.frag), line for line. The reflection (3, drawn by hand into the
	// game's reflection with its depth test) and the layer probe (5, a game texture bound by hand) stay D3D9.
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	bool	ExtSupportsRenderRect() const override { return true; }
	int		ExtMarchSteps = 48;	// UNOFFICIAL 2026-10-26: the march's step count without the D3D9 shader's 64 cap (UpdateConstants), for ExtRender

private:
	ExtPass		extPasses[5];				// composite, march, temporal, shadow map, sky probe (2026-10-30)
	ExtUniformBuffer	extUniforms;
	CachedHandle	physicalSkyCache;		// TESR_SkyViewLUT: there only when the effect was compiled with PHYSICAL_SKY_ON
	float		coverage = -1.0f, rain = 0.0f, typeBias = 0.0f;	// eased toward the weather's
	unsigned int	lastUpdateFrame = 0;	// UNOFFICIAL 2026-10-21: the rendered frame (FrameRateManager::FrameNumber) of the last UpdateConstants
	bool		updatedBefore = false;
	D3DXVECTOR3	wind = D3DXVECTOR3(0, 0, 0);
	double		windX = 0.0, windY = 0.0, windZ = 0.0;	// UNOFFICIAL: the wind offset, kept in doubles (wind holds it as floats for the shader)
	D3DXMATRIX	lastViewProj;
	UINT		frame = 0, read = 0;
	UINT		shadowMapFrame = UINT_MAX;	// UNOFFICIAL 2026-10-21: the frame the shadow map was last drawn for
	double		lastRenderTime = -1.0;	// TheFrameRateManager->Time of the last frame drawn
	bool		lastRect = false;		// UNOFFICIAL 2026-10-27 RenderScale v2: the last frame drawn was a rect frame
	UINT		lowWidth = 0, lowHeight = 0, blockWidth = 0, blockHeight = 0;
	bool		stableReady = false;
	double		lastCloudTime = -1.0;	// UNOFFICIAL GameTime: the clouds' own smooth clock, in seconds at the normal time scale, held to the game's
	IDirect3DSurface9*	reflectTarget = nullptr;	// UNOFFICIAL: this reflection pass's target and depth buffer (held until drawn)
	IDirect3DSurface9*	reflectDepth = nullptr;
	D3DXMATRIX	reflectView, reflectProj;
	IDirect3DStateBlock9*	reflectState = nullptr;
	bool		reflectLogged = false;
	bool		reflectNoted = false;	// UNOFFICIAL: this pass's target is noted (only its first)
	bool		ReflectionWanted() const;
	// UNOFFICIAL 2026-10-15 weather sync: how much of the sky each weather's own cloud layers fill (0-1), read from the
	// game's loaded cloud textures the first time that weather is shown; a weather not shown yet is not in the map.
	std::unordered_map<TESWeather*, float>	weatherFill;
	std::unordered_map<std::string, float>	textureFill;	// per texture path (lower case): its mean alpha; -1 no alpha, -2 being measured
	TESWeather*	lastWeather = nullptr;		// the sky's current weather last frame
	int			learnTries = 0;				// frames the current weather's cloud textures were not found in the sky
	float		snapWait = -1.0f;			// >= 0: a forced weather change (console fw) waits this long for its fill, then snaps
	void		LearnWeatherFill(Sky* sky);
	struct ProbeJob { std::string path; IDirect3DBaseTexture9* texture; float lod; UINT width, height; };
	std::vector<ProbeJob>	probeQueue;		// game cloud textures waiting to be measured (held with a reference)
	IDirect3DSurface9*	probeReadback = nullptr;	// the probe's 1 x 1 copy in system memory
	void		ProbeLayer(IDirect3DDevice9* Device);
	void		WeatherLook(TESWeather* weather, float& coverage, float& rain, float& bias);
	// UNOFFICIAL 2026-10-25 WeatherTint: the colour the game gives its own cloud layers now (the weather's cloud colours at this
	// hour, blended across a weather change), linear, 0-1.
	D3DXVECTOR3	WeatherCloudColour(Sky* sky, float hour);
	D3DXVECTOR3	CloudColourOf(TESWeather* weather, const float weights[6]);
	int			tintLogs = 0;
	TESWeather*	tintLogWeather = nullptr;
	float		tintLogHour = -100.0f;
};
