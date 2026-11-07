#pragma once

#include "../TerrainShadows/TerrainShadows.h"	// UNOFFICIAL 2026-10-19 [Shaders.ShadowsExteriors.TerrainShadows]

typedef struct frustum {
	enum PLANE { PLANENEAR = 0, PLANEFAR = 1, PLANELEFT = 2, PLANERIGHT = 3, PLANETOP = 4, PLANEBOTTOM = 5 };
	PLANE PLANE;
	D3DXPLANE plane[6];
} frustum;

class ShadowsExteriorEffect : public EffectRecord
{
public:
	ShadowsExteriorEffect() : EffectRecord("ShadowsExteriors") {};

	static const int Modes = 4;	// UNOFFICIAL: 3 = PCF (plain depth, filtered at lookup)
	static const int FormatBits = 2;

	D3DFORMAT Formats[Modes][FormatBits] = {
		{D3DFMT_G16R16, D3DFMT_G32R32F},
		{D3DFMT_G16R16F, D3DFMT_G32R32F},
		{D3DFMT_A16B16G16R16F, D3DFMT_A32B32G32R32F},
		{D3DFMT_R32F, D3DFMT_R32F},	// UNOFFICIAL PCF: depth needs the 32 bits whatever Format says
	};
	
	enum ShadowMapTypeEnum {
		MapNear = 0,
		MapMiddle = 1,
		MapFar = 2,
		MapLod = 3,
		MapOrtho = 4,
	};

	//Constants
	struct ShadowStruct {
		D3DXVECTOR4		SmoothedSunDir, Data, FormatData, ScreenSpaceData, OrthoData, ShadowFade;
		D3DXMATRIXA16	ShadowWorld;
		D3DXMATRIX		ShadowViewProj, ShadowSpotlightCameraToLight[SpotLightsMax];
		D3DXVECTOR4		ShadowCubeMapLightPosition;
		D3DXVECTOR4		ShadowLightPosition[ShadowCubeMapsScreen];	// the screen-space passes' TESR_ShadowLightPosition[12]
		D3DXVECTOR4		ShadowMapRadius, ShadowBlur;
		// Forward sun shadows, runtime side. x: 1 when the forward path is SUPPRESSED.
		//
		// The polarity is deliberate. A constant that fails to reach a shader reads as zero,
		// and zero here means "not suppressed" -- i.e. the current, working behaviour. The
		// opposite polarity would turn a dropped constant into silently missing shadows,
		// which is exactly the failure that made this a compile-time macro in the first place.
		D3DXVECTOR4		ForwardData;
		// UNOFFICIAL SunSmoothing CrossFade: the cascades from before the last sun step, kept in camera-relative
		// space like the live ones, and x = how much of them to show (1 at the step, 0 once the fade is over).
		D3DXMATRIX		OldCameraToLight[4];
		D3DXVECTOR4		OldCenter[4];
		D3DXVECTOR4		CrossFade;
		// UNOFFICIAL TESR_PointShadowNear (PointShadows.fx): x = [Shaders.ShadowsInteriors.Main] NearFade, world units; y = EdgeFade, fraction of a lamp's radius.
		D3DXVECTOR4		PointShadowNear;
		// UNOFFICIAL 2026-10-23 CharacterShadowMap (Includes/Shadow.hlsl, SunShadows.fx.hlsl): the map's camera-relative world -> light clip
		// transform (ortho), its centre (camera-relative, w = half-extent in world units) and x: 1 / resolution, y: the hand-over band as a share
		// of the half-extent, z: the constant depth bias in the map's depth units, w: 1 while the map holds this frame's draw (0 = not read).
		D3DXMATRIX		CharacterCameraToLight;
		D3DXVECTOR4		CharacterCenter;
		D3DXVECTOR4		CharacterData;
	};

	// Settings
	struct FormsStruct {
		bool				AlphaEnabled;
		bool				Activators, Actors, Apparatus, Books, Containers, Doors, Furniture, Misc, Statics, Terrain, Trees, Lod;
		float				MinRadius, OrigMinRadius;
	};

	struct ShadowMapSettings {
		D3DXMATRIX				ShadowCameraToLight;
		D3DVIEWPORT9			ShadowMapViewPort;
		frustum					ShadowMapFrustum;
		NiFrustumPlanes			ShadowMapFrustumPlanes;
		D3DXVECTOR4				ShadowMapCascadeCenterRadius;
		D3DXVECTOR3				CameraTranslation;  // Camera translation at the moment the shadow matrix was calculated.
		D3DXVECTOR4				ClearColor;
		FormsStruct				Forms;
		float					ShadowMapResolution, ShadowMapInverseResolution, ShadowMapRadius, ShadowMapNear;
		bool					CustomClearRequired;
		D3DXPLANE				CasterPlanes[4];	// UNOFFICIAL 2026-11-04 CasterCulling: the view slice's sides, camera-relative, facing in (ShadowManager::SunCastMask)
		bool					CasterPlanesValid = false;
	};

	struct ShadowMapStruct {
		int					Mode, FormatBits;
		D3DFORMAT			Format;
		int					CascadeResolution;
		bool                Prefilter, MSAA, Mipmaps, LimitFrequency;
		bool				LodSkipLoaded;	// UNOFFICIAL (Fable): object LOD inside the loaded cells casts no shadow (the full models do)
		float				CasterFadeMin = 0.75f;	// UNOFFICIAL 2026-10-28 [Shaders.ShadowsExteriors.ShadowMaps] CasterFadeMin: a fade node casts from this fade on
		// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) [Shaders.ShadowsExteriors.ShadowMaps], every quality, live:
		bool				CasterCulling = false;		// leave out casters whose shadow cannot reach the view
		float				CasterMargin = 256.0f;
		float				CasterDrop = 2048.0f;
		bool				SkipCoveredCasters = false;	// leave out of a cascade what only an earlier one shades
		int					Anisotropy;
		float				Distance, CascadeLambda;
		bool				SmoothFilter;	// UNOFFICIAL 2026-10-15: PCF (Mode 3) with a 4 x 4 cubic B-spline instead of the 3 x 3
		float				HandoverBlend = 70.0f;	// UNOFFICIAL 2026-10-21: width of the cross-fade band between sun cascades, world units (TESR_ShadowForwardData.w), live
		bool				PlayerInNearLog = false;
		float				PlayerInNear = 0.0f;	// UNOFFICIAL 2026-10-23: third person, the player's head kept inside the Near cascade up to this camera distance (world units), 0 = off, live
		// UNOFFICIAL 2026-10-23 CharacterShadowMap ([Shaders.ShadowsExteriors.ShadowMaps]; docs/next/CHARACTER-SHADOW-MAP-DESIGN.md): a small
		// sun shadow map of the player alone, fitted round the head and upper body, read by the skin and object shaders and the deferred
		// sun pass where a pixel is inside it (Includes/Shadow.hlsl CharacterMapShadow). Compiled into the shaders when on at startup
		// (CHARACTER_SHADOW_MAP), live after that; off = the shaders and the cascade path exactly as before.
		bool				CharacterMap = false;
		int					CharacterMapResolution = 1024;	// texels across (256..4096, a multiple of 256), live (the texture is remade)
		int					CharacterMapFit = 0;			// 0 = head and upper body (Spine2 to Head), 1 = the whole actor bound
		float				CharacterMapMargin = 24.0f;		// world units added round the fit (hat brims, hair, shoulders)
		float				CharacterMapBlend = 12.0f;		// world units inside the map's edge over which it hands over to the cascades
		float				CharacterMapBias = 0.15f;		// world units of constant depth bias at the lookup
		bool				CharacterMapOthers = false;		// other actors inside the map are drawn into it too (they can shade the player)
		bool				CharacterMapLog = false;		// diagnostic: fit, texel size and caster count once a second
		int					CharacterMapDebug = 0;			// diagnostic view (Shadow.hlsl CharacterMapShadow): 0 off, 1 the map's verdict alone, 2 the map's depth, 3 the point's depth
	};

	struct OrthoStruct {
		int					Resolution;
		float				Distance;
		bool				LimitFrequency;
		int					ProbeDump = 0;	// UNOFFICIAL 2026-10-17 test tools: a new non-zero value = one caster dump (core/OrthoProbe.h)
		// UNOFFICIAL 2026-10-17 Skylighting v2 ([Shaders.AmbientOcclusion.Skylighting]; ShadowManager::RenderShadowMaps)
		bool				SkyCleanMap = true;			// alpha test on, blended-only geometry off, while v2 uses the map
		int					SkyMapRefresh = 60;			// frames between redraws when only v2 uses the map
		float				SkyMoveRefresh = 200.0f;	// camera move (units) that forces a redraw
		int					SkySettingsGeneration = 0;	// bumped by every settings load: forces a redraw
		bool				SkyOwnProjection = true;	// an explicit straight-down projection for the sky-only map (ShadowManager)
	};

	struct ExteriorsStruct {
		bool				Enabled, ForwardShadows, UsePointShadowsDay, UsePointShadowsNight;
		int					Quality;
		float				Darkness, NightMinDarkness;
		bool				SmoothCharacterShadows = false;	// UNOFFICIAL 2026-10-20: skin shaders' shadow lookups take the smooth vertex normal (TESR_ShadowForwardData.y), live
		float				CharacterShadowSoftness = 0.0f;	// UNOFFICIAL 2026-10-20: with it, sun shadow edges on skin fade over at least this half-width, world units (.z), live
		float				CharacterShadowReach = 0.0f;	// UNOFFICIAL 2026-10-22: cap on the skin shaders' PCF slack, world units on the Near cascade (TESR_ShadowBlur.w), live; 0 = off
		bool				SkipWhenFaded;	// UNOFFICIAL 2026-10-16: no sun-shadow work while the fade is 1 (UpdateConstants SunFaded)
		int					SkipWhenFadedCheck;	// UNOFFICIAL 2026-10-16, DIAGNOSTIC (NVR_TEST_TOOLS): 1 poison what is skipped, 2 also skip ignoring the readers, 3 lookups off at any hour
		int					CascadeStats = 0;	// UNOFFICIAL 2026-10-21, DIAGNOSTIC (NVR_TEST_TOOLS): SunCascadeStats, where the sun cascades' CPU time goes (SunShadowStats.h)
		bool				SunSkipFlat = false;	// UNOFFICIAL 2026-10-21 SunCascadeSkipFlat: a cascade whose projection is flat (sun straight overhead) is cleared, not walked or drawn
		bool				SunOneWalk = false;		// UNOFFICIAL 2026-10-21 SunCascadeOneWalk: one walk of the loaded cells for all sun cascades of a frame
		bool				SunLeanDraws = false;	// UNOFFICIAL 2026-10-21 SunCascadeLeanDraws: per caster only changed constants, no wind calls for still textures
		bool				OverheadSunFix = false;	// UNOFFICIAL 2026-10-21 (sunfix): GetCascadeViewProj takes +Y as up for a sun (nearly) straight overhead (its cascades were empty)
		int					SunVerify = 0;			// UNOFFICIAL 2026-10-21, DIAGNOSTIC (NVR_TEST_TOOLS): SunCascadeVerify, 1 lists, 2 also the maps, old way vs new in the same frame
	};

	struct InteriorsStruct {
		bool				Enabled;
		FormsStruct			Forms;
		int					LightPoints;
		int					ExteriorLightPoints;	// UNOFFICIAL: outdoors, lamps with shadows (above 12: the screen-space pass in batches)
		int					Quality, ShadowCubeMapSize, DrawDistance;
		float				Darkness, LightRadiusMult;
		bool				UseCastShadowFlag, PlayerShadowThirdPerson, PlayerShadowFirstPerson;
		// UNOFFICIAL (2026-10-07 review): read on each settings change instead of by name every frame.
		bool				PlayerInsideLamp;	// [Shaders.ShadowsInteriors.Main] PlayerInsideLamp
		bool				RedrawActorsOnly;	// UNOFFICIAL (2026-10-08) [Shaders.ShadowsInteriors.Main] RedrawActorsOnly
		float				PlayerLampMargin;	// [Shaders.ShadowsInteriors.Main] PlayerLampMargin (at least 1)
		struct ForwardStruct {	// [Shaders.ShadowsInteriors.Forward]
			bool Enabled, KeepDarkening, DarkeningBlur, FirstPerson, ScopeFix, LogLamps;
			bool CameraIndependent;		// UNOFFICIAL (2026-10-08): lamps get shadows by where you are, not where you look
			bool KeepLampsInView;		// UNOFFICIAL (2026-10-08): the game's lamp view test may not drop lamps that reach the view
			int LampRanking, DebugView;
			float LampSwitchMargin, Strength, Bias, NormalOffset, Softness, FadeIn;	// LampSwitchMargin clamped to 0..0.9
			float FadeOut = 0.0f;		// UNOFFICIAL 2026-11-01 (flicker handovers): seconds a lamp that lost its slot keeps it while its shadow fades out (0 = off)
			bool ShadowClusterEnabled = false;	// UNOFFICIAL (2026-10-26): lamps closer than ShadowClusterDistance share one shadow (the brightest one's)
			float ShadowClusterDistance = 128.0f;
			bool ShadowClusterInvisibleOnly = false;	// UNOFFICIAL (2026-10-29): only invisible lights (LampFixtures) give their shadow up
			int ShadowClusterMinLights = 0;				// UNOFFICIAL (2026-10-29): ... and only where this many lights stand within the distance
			float FillLightRadius;		// UNOFFICIAL (2026-10-08): lamps reaching farther than this are fill lights (0 = none)
			float FillLightShadowStrength;	// UNOFFICIAL (2026-10-08): how dark fill lights' shadows are, 0..1 (0 = no shadow)
			int Quality;				// UNOFFICIAL (2026-10-10): PCF taps, 0 Low (1), 1 Medium (4, as before), 2 High (10), 3 Ultra (20)
			bool ContactHardening;		// UNOFFICIAL (2026-10-10): lamp shadows sharp at contact, Softness further out (not the sun's [Shaders.ContactHardening])
			float ContactHardeningStrength, ContactHardeningMinSoftness;	// at least 0; 0..1 share of the Softness spread
			float InvisibleLightShadowStrength;	// UNOFFICIAL (2026-10-20): 0..1, shadows of lights with no glowing lamp near them (LampFixtures.h); 1 = as before
			float FixtureDistance;		// UNOFFICIAL (2026-10-20): world units; a glowing mesh this close makes a light a lamp's
			float LampSize;				// UNOFFICIAL (2026-10-20): world units, a lamp's radius for shadows soft by distance (0 = off)
			float MaxSoftness;			// UNOFFICIAL (2026-10-20): the widest blur LampSize may give, in shadow-map texels (1..32)
			float InvisibleLightSize;	// UNOFFICIAL (2026-10-20): world units, invisible lights' own lamp size for soft shadows (0 = as LampSize)
			bool LampGlassShadows;
			float OwnGlassDistance;		// UNOFFICIAL (2026-10-30): world units; a lamp part this near a light casts nothing into that light's map (below 0 off)
			bool LampModelShadows;		// UNOFFICIAL (2026-10-20): false = a light leaves lamp models near it out of its shadow map (LampFixtures::NearLampModel)
			float LampModelDistance;	// UNOFFICIAL (2026-10-20): world units, how near (to a lamp model's bound) that is
			float InvisibleShadowFromLamp;	// UNOFFICIAL (2026-10-20): world units; invisible lights cast from the nearest lamp part within it (0 off)		// UNOFFICIAL (2026-10-20): false = lamp parts (LampFixtures::LampPart) cast no lamp shadows; true = as before
			bool EvenTaps;				// UNOFFICIAL (2026-10-21): High/Ultra's extra taps spread evenly (POINT_SHADOW_EVEN, read at startup)
			float MagnifiedSoftness;	// UNOFFICIAL (2026-10-21): texels; the least spread where a cube texel covers many pixels (0 off; compiled in when above 0 at startup)
			bool TexelFilter;			// UNOFFICIAL (2026-10-21): the sun-style texel-aligned B-spline filter (POINT_SHADOW_TEXEL, read at startup)
			bool AutoNormalOffset;		// UNOFFICIAL (2026-10-24): NormalOffset at least the filter's reach in texels (PointShadowForward::Publish), live
		} Forward;
	};

	struct ContactHardeningStruct {	// UNOFFICIAL [Shaders.ContactHardening.Status/.Main], read on change (ShadowManager uses it every frame)
		bool				Enabled;
		float				SunSize;		// at least 0
		float				MaxSoftness;	// 1..32
		bool				Smooth;			// UNOFFICIAL 2026-10-15: smooth penumbra edges (Includes/Shadow.hlsl, PCF Mode 3)
		bool				AtNight;		// UNOFFICIAL 2026-10-16: false = plain filtering while the sun is down (ShadowManager)
	};

	struct ScreenSpaceStruct {
		bool				Enabled;
		float				BlurRadius, RenderDistance;
		bool				Sharp = false;	// UNOFFICIAL: SunShadows.fx.hlsl's ForwardContactSharp (one sharp march, no big blurs)
	};

	struct SunSmoothingStruct {
		bool				SmoothSun, QuantizeSun;
		float				SmoothingFactor, YawStepSize, PitchStepSize, MaxJumpAngle;
		float				GlideSeconds; // UNOFFICIAL: real seconds one quantized step takes to slide over (0 = old behaviour)
		bool				CrossFade;    // UNOFFICIAL: fade from the old shadows to the new instead of sliding (restart)
	};

	struct SettingsShadowStruct {
		ShadowMapStruct     ShadowMaps;
		OrthoStruct			OrthoMap;
		ScreenSpaceStruct	ScreenSpace;
		ExteriorsStruct		Exteriors;
		InteriorsStruct		Interiors;
		SunSmoothingStruct  SunSmoothing;
		ContactHardeningStruct ContactHardening;	// settings of each shadow area
	};
	SettingsShadowStruct	Settings;
	ShadowStruct			Constants;
	// UNOFFICIAL 2026-10-19: the land's shadows far past the cascades (TerrainShadows.h, after Conor's port of Community
	// Shaders' Terrain Shadows), updated in ShadowManager::RenderShadowMaps, applied by the SunFar pass.
	TerrainShadowMap		TerrainShadows;
	// UNOFFICIAL 2026-10-19 (after Conor, 6043062a): the sun and ambient colours as the object shaders receive them
	// (TESR_ShadowSunLight / TESR_ShadowAmbientLight), for the SunFar pass to split a pixel's light the way they lit it.
	D3DXVECTOR4				SunLight = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f);
	D3DXVECTOR4				AmbientLight = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	void					UpdateLightColors();
	ShadowMapSettings		ShadowMaps[5];
	// UNOFFICIAL 2026-10-16 SkipWhenFaded: outdoors, this frame's sun shadows are fully faded out (ShadowFade.x 1, around
	// sunrise and sunset) and the switch is on, so the forward lookups are off (ForwardData.x). Set by UpdateConstants.
	bool					SunFaded = false;
	bool					FadedContactUnread();	// while SunFaded: nothing this frame reads the contact shadows (ShaderManager skips them)
	bool					FadedCascadesUnread();	// while SunFaded: nothing this frame reads the cascade maps (ShadowManager skips them)

	struct ShadowTextures {
		IDirect3DTexture9* ShadowPassTexture;
		IDirect3DSurface9* ShadowPassSurface;
		IDirect3DCubeTexture9* ShadowCubeMapTexture[ShadowCubeMapsMax];
		IDirect3DSurface9* ShadowCubeMapSurface[ShadowCubeMapsMax][6];
		// UNOFFICIAL RedrawActorsOnly: per slot, the lamp's cube map with only what never moves (PointShadowSchedule.h);
		// made on first use (EnsureStaticCubeMap), never registered as a shader texture (only copied from)
		IDirect3DCubeTexture9* ShadowCubeMapStaticTexture[ShadowCubeMapsMax];
		IDirect3DSurface9* ShadowCubeMapStaticSurface[ShadowCubeMapsMax][6];
		IDirect3DTexture9* ShadowSpotlightTexture[SpotLightsMax];
		IDirect3DSurface9* ShadowSpotlightSurface[SpotLightsMax];	// one spotlight map each
		IDirect3DSurface9* ShadowCubeMapDepthSurface;
	};
	ShadowTextures	Textures;

	// Main shadow atlas, used for cascades.
	IDirect3DTexture9* ShadowAtlasTexture;
	IDirect3DSurface9 *ShadowAtlasSurface, *ShadowAtlasSurfaceMSAA, *ShadowAtlasDepthSurface;

	// Ping-pong target for the separable prefilter blur, so that neither pass samples the atlas
	// while the atlas is bound as the render target -- undefined in D3D9, and a read/write
	// feedback loop on Vulkan under DXVK.
	// Initialised here because they are only allocated when Prefilter is on -- unlike the
	// members above, which RegisterTextures always assigns before anything tests them.
	IDirect3DTexture9* ShadowAtlasBlurTexture = nullptr;
	IDirect3DSurface9* ShadowAtlasBlurSurface = nullptr;

	IDirect3DVertexBuffer9* ShadowAtlasVertexBuffer;

	// Ortho shadows for various effects.
	IDirect3DTexture9* ShadowMapOrthoTexture;
	IDirect3DSurface9 *ShadowMapOrthoSurface, *ShadowMapOrthoDepthSurface;

	// UNOFFICIAL 2026-10-23 CharacterShadowMap: the player's own sun shadow map (R32F plain depth, TESR_CharShadowMap), its depth
	// surface and the ShadowMapSettings ShadowManager::RenderCharacterShadowMap draws it with (as AmbientLight's gridSunMap). Made
	// on first use at the size the setting asks for (EnsureCharacterMap), remade when the size changes. CharacterMapCompiled: the
	// setting was on at startup, so the shaders have the lookup (CHARACTER_SHADOW_MAP in ShaderRecord::BuildDefines).
	IDirect3DTexture9* CharacterMapTexture = nullptr;
	IDirect3DSurface9 *CharacterMapSurface = nullptr, *CharacterMapDepthSurface = nullptr;
	UINT		CharacterMapSizeCreated = 0;
	bool		CharacterMapCompiled = false;
	ShadowMapSettings CharacterMap = {};
	bool		EnsureCharacterMap();
	// The map's fit for this frame: a sun-aligned ortho box round the player (CharacterMapFit), camera-relative like the cascades, its
	// origin snapped to a texel; false when there is nothing to fit (first person, no skeleton). Sets CharacterMap up for the draw and
	// Constants.CharacterCameraToLight / CharacterCenter / CharacterData (w left to the caller).
	bool		GetCharacterViewProj(const D3DXVECTOR3& sunDir, D3DXMATRIX& viewProj);
	NiNode*		CharacterTargetNode = nullptr;	// UNOFFICIAL 2026-10-31 CharacterShadowMapTarget: whose node the map is fitted round and walks (GetCharacterViewProj)

	void		clearShadowsBuffer();
	bool		ShouldRender() override;
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the shadow apply's pass as src\hlsl\NewVegas\Compute\ShadowsExteriors.frag (EffectRecord::ExtRender)
	bool		ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass		extPass;
	void		UpdateConstants(), UpdateSettings(), RegisterConstants(), RegisterTextures();
	void		RecreateTextures(bool cascades, bool ortho, bool cubemaps);
	// UNOFFICIAL (2026-10-08): slots past ShadowCubeMapsScreen get their cube map on first use (forward shadows with
	// LightPoints above 12), at the size the others were made at (the shared depth surface's). false: no cube map.
	bool		EnsureCubeMap(int slot);
	bool		EnsureStaticCubeMap(int slot);	// UNOFFICIAL RedrawActorsOnly
	UINT		CubeMapSizeCreated = 0;
	// UNOFFICIAL 2026-11-01 NearCubeSize ([Shaders.ShadowsInteriors.Atlas] NearCubeSize, restart; user: interior lamp shadows look low resolution
	// and jagged): cube slots 0..NearCubeSlots-1 (the lamps the atlas gives its big NearTiles) are made this size instead of
	// ShadowCubeMapSize, so the 2048-texel near tiles hold real detail instead of a resampled 512 map. 0 = every slot ShadowCubeMapSize.
	UINT		NearCubeSize = 0;
	UINT		SpotMapSize = 512;	// UNOFFICIAL 2026-11-04 [Shaders.Flashlight.Deferred] ShadowMapSize (restart): the spot maps' size (RegisterTextures)
	static const int NearCubeSlots = 8;
	UINT		CubeSizeFor(int slot) const { return (slot >= 0 && slot < NearCubeSlots && NearCubeSize > CubeMapSizeCreated) ? NearCubeSize : CubeMapSizeCreated; }
	unsigned	CubeGeneration = 0;	// UNOFFICIAL 2026-10-18 CubeFaceCopySkip: counts lamp cube map creations (a new texture may reuse an old address)

	D3DXVECTOR3	CalculateSmoothedSunDir();
	// UNOFFICIAL sun glide (SunSmoothing GlideSeconds): the quantized step being slid over.
	D3DXVECTOR3	SunGlideFrom = D3DXVECTOR3(0.0f, 0.0f, 1.0f), SunGlideTo = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
	ULONGLONG	SunGlideStart = 0;
	bool		SunGlideValid = false;
	// UNOFFICIAL SunSmoothing CrossFade. The game shaders get the fade compiled in only when the setting was on at
	// startup (SUN_CROSSFADE), so the switch takes effect after a restart; SunCrossFadeReady says this session's
	// shaders and the copy of the atlas exist. The copy holds the atlas from the frame before the step; the matrices
	// and centres are those it was drawn with, relative to the camera position each cascade was drawn from.
	bool		SunCrossFadeReady = false, SunCrossFading = false; // the current step fades instead of gliding
	IDirect3DTexture9* ShadowAtlasOldTexture = nullptr;
	IDirect3DSurface9* ShadowAtlasOldSurface = nullptr;
	D3DXMATRIX	SunFadeMatrix[4];
	D3DXVECTOR4	SunFadeCenter[4];
	D3DXVECTOR3	SunFadeCamera[4];
	bool		StartSunCrossFade();
	void		UpdateSunCrossFade(float oldWeight);

	void		GetCascadeDepths();
	// UNOFFICIAL 2026-10-31 SunLayers: ExtentScale > 1 widens the map's square (not its depth) by that factor, drawn at MapSize texels
	// (same texel size, snapped to the same world grid): the static layer with its margin. Defaults: the cascade as always.
	D3DXMATRIX	GetCascadeViewProj(ShadowMapSettings* ShadowMap, D3DXVECTOR3* SunDir, float ExtentScale = 1.0f, float MapSize = 0.0f);
	// UNOFFICIAL 2026-10-21 (sunfix) OverheadSunFix: the direction is within ~2.6 degrees of straight up (or down), where the
	// cascades' up direction (0, 0, 1) degenerates. Need not be a unit vector.
	static bool	SunOverhead(const D3DXVECTOR3& dir) { return fabsf(dir.z) > 0.999f * D3DXVec3Length(&dir); }

private:
	bool		texturesInitialized;

	bool		UpdateSettingsFromQuality(int quality);
};
