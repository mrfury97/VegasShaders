#pragma once
// Vertex layout of the full-screen quad (FrameVS below).
#define FrameFVF (D3DFVF_XYZ | D3DFVF_TEX1)

#include "../effects/Animator.h"
#include "ShaderRecord.h"		// game-shader records
#include "EffectRecord.h"		// post-process effect records
#include "ShaderCollection.h"	// game-shader families
#include "../effects/Effects.h"	// every effect class
#include "RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

// Per-frame values the constants of NVR's shaders point at (filled by ShaderManager::UpdateConstants and the effects).
struct ShaderConstants {
	struct OcclusionMapStruct { D3DXMATRIX OcclusionWorldViewProj; };

	D3DXMATRIXA16			ShadowWorld;
	// Screen, sun and time.
	D3DXVECTOR4				ReciprocalResolution, SunDir, SunPosition, SunTiming, SunAmount,
							ViewSpaceLightDir, ScreenSpaceLightDir, GameTime, FrameTime;
	// The current weather and its colours.
	TESWeather*				pWeather;	// the weather the colours below come from
	float					sunGlare, windSpeed;
	D3DXVECTOR4				fogColor, horizonColor, skyLowColor;
	float					skyObjectID;	// which sky part the game is drawing
	D3DXVECTOR4				sunDiskColor, sunColor, sunAmbient, skyColor, fogData, fogDistance;
	float					fogStart, fogEnd, fogPower;
	OcclusionMapStruct		OcclusionMap;
};

using EffectsList = std::map<std::string, EffectRecord**>;			// effect name -> its slot in ShaderManager::Effects
using ShaderList = std::map<std::string, ShaderCollection**>;		// family name -> its slot in ShaderManager::Shaders
using CustomConstants = std::map<std::string, D3DXVECTOR4>;			// constants no NVR code feeds (zero unless set)

// One vertex of the full-screen quad.
struct FrameVS {
	float x, y, z;	// position
	float u, v;		// texture coordinate
};

/*
* Copy-free effect chain. Without it every effect renders into the game's target and then copies
* the whole frame into TESR_RenderedBuffer so the next effect can sample it. With it, effects
* render into whichever of two chain textures is not the current image and the pair is swapped:
* TheTextureManager->RenderedTexture/Surface always name the current image, and the game's target
* holds the final image when the chain ends. The buffers match the game target's format, so
* precision and clamping between effects are unchanged.
*
* When the game target is itself a texture (the usual case), it is one of the two buffers: no
* seed copy at the start, and a copy at the end only when the last effect wrote the spare.
* Otherwise both buffers are NVR textures, seeded from and copied back to the game target.
*/
class FrameChain {
public:
	bool					Begin(IDirect3DSurface9* gameTarget); // false: chain unavailable, legacy behaviour
	void					End();
	bool					IsActive() const { return Active; }
	// True when an effect's (RenderTarget, RenderedSurface) pair is the chain's. Every effect that
	// renders through the chain asks this first, which is also where a pending direct-to-target final
	// pass (below) is undone if another effect turns out to render after it.
	bool					Owns(IDirect3DSurface9* renderTarget, IDirect3DSurface9* renderedSurface);
	IDirect3DSurface9*		Output() const { return Surf[Current ^ 1]; }
	void					Commit(); // the Output() surface becomes the current image
	// For legacy paths that render into the game target while sampling TESR_RenderedBuffer: the
	// game target gets the current image, and the current image is moved off the game target.
	void					Sync();

	// A chain on NVR-owned buffers (the game target is not one of them, as with the post-tonemap
	// back buffer) normally ends by copying the current image into the game target. If the caller
	// names the effect that renders last, that effect's final pass renders into the game target
	// itself and End() has nothing to copy.
	void					SetFinalEffect(const void* effect) { FinalEffect = effect; }
	bool					IsDirectFinal(const void* effect) const { return Active && !GameTexture && GameTarget && FinalEffect == effect; }
	IDirect3DSurface9*		FinalSurface() const { return GameTarget; }
	void					CommitFinal() { FinalWritten = true; Writes++; LastWrite = 2; } // the game target holds the finished image
	bool					FinalIsWritten() const { return FinalWritten; }	// UNOFFICIAL 2026-10-26 (pixel inspector)
	// UNOFFICIAL 2026-10-26 (pixel inspector): how many times the frame was written (Commit, CommitFinal, Sync) and the last way:
	// 1 Commit (the image is the current buffer), 2 CommitFinal or 3 Sync (the image is in the game target).
	UINT					WriteCount() const { return Writes; }
	int						LastWriteKind() const { return LastWrite; }

private:
	void					ReclaimFinal();
	struct Pair { // NVR-owned textures for one target format/size, created on first use
		D3DFORMAT			Format = D3DFMT_UNKNOWN;
		UINT				Width = 0, Height = 0;
		IDirect3DTexture9*	Texture[2] = {};
		IDirect3DSurface9*	Surface[2] = {};
	};
	bool					EnsureTexture(Pair& pair, int slot);
	void					Publish();

	Pair					Pairs[2];
	IDirect3DTexture9*		Tex[2] = {};	// working buffers for the active chain
	IDirect3DSurface9*		Surf[2] = {};
	IDirect3DTexture9*		GameTexture = nullptr; // container of the game target, when used as Tex[0]
	int						Current = 0;
	bool					Active = false;
	IDirect3DSurface9*		GameTarget = nullptr;
	IDirect3DTexture9*		SavedTexture = nullptr;
	IDirect3DSurface9*		SavedSurface = nullptr;
	const void*				FinalEffect = nullptr;	// effect expected to render last in this chain
	bool					FinalWritten = false;	// its last pass already wrote the game target
	UINT					Writes = 0;
	int						LastWrite = 0;
};

struct ChainContext;	// UNOFFICIAL 2026-11-02 (core/EffectChain.h)
// Owns every effect and game-shader family, the shared constants, and the post-process chain. Made once, never freed.
__declspec(align(16)) class ShaderManager : public ShaderManagerBase {
public:
	static void				Initialize();	// creates TheShaderManager and every effect/family

	void*					operator new(size_t bytes) { return _mm_malloc(bytes, 16); }	// 16-byte aligned (the XMM matrices)

	template <typename T> void RegisterEffect(T** slot);
	template <typename T> void RegisterShaderCollection(T** slot);
	EffectRecord*			GetEffectByName(const char* effectName);
	ShaderCollection*		GetShaderCollectionByName(const char* collectionName);
	void					ClearShaderSamplers(const char* textureName, size_t byteCount);
	void					RegisterConstant(const char* constantName, D3DXVECTOR4* value);
	void					RenderDepthAndNormals(IDirect3DSurface9* renderTarget);	// UNOFFICIAL 2026-10-27: TESR_DepthBuffer + TESR_NormalsBuffer (chain start; RenderScale v2 again after its upscale)
	void					CreateFrameVertex(UInt32 width, UInt32 height, IDirect3DVertexBuffer9** vertexBuffer, float scaleX = 1.0f, float scaleY = 1.0f);
	void					InitializeConstants();	// the screen-size constants, no weather yet
	void					UpdateConstants();		// the per-frame game state and constants
	void					GetNearbyLights(ShadowSceneLight* shadowLights[], NiPointLight* lights[], NiSpotLight* spotLights[]);
	bool					LoadShader(NiD3DVertexShader* vertexShader);
	bool					LoadShader(NiD3DPixelShader* pixelShader);
	void					ReloadEffects();	// disposes and loads every effect again
	void					ReloadShaders();	// UNOFFICIAL 2026-10-25 (test only): every replaced game shader loaded again ('reloadshaders')
	ShaderCollection*		GetShaderCollection(const char* shaderName);
	float					GetTransitionValue(float dayValue, float nightValue, float interiorValue);
	bool					ShouldRenderShadowMaps();	// whether the shadow pass runs this frame
	void					RenderEffects(IDirect3DSurface9* renderTarget);
	void					RenderEffectsPreTonemapping(IDirect3DSurface9* renderTarget);
	void					RenderEffectToRT(IDirect3DSurface9* renderTarget, EffectRecord* effect, bool clearRenderTarget, UINT techniqueIndex = 0);
	void					SwitchShaderStatus(const char* shaderName);
	void					SetCustomConstant(const char* constantName, D3DXVECTOR4 value);

	// UNOFFICIAL 2026-11-02 the post-process chain's steps (core/EffectChain.cpp; their order is the two tables there)
	void					PreStep_FrameQuad(ChainContext& ctx);
	void					PreStep_SceneInProbes(ChainContext& ctx);
	void					PreStep_DepthAndNormals(ChainContext& ctx);
	void					PreStep_ObjectMotion(ChainContext& ctx);
	void					PreStep_LampVisibility(ChainContext& ctx);
	void					PreStep_DepthNormalProbes(ChainContext& ctx);
	void					PreStep_SunDepthFit(ChainContext& ctx);
	void					PreStep_OrthoProbe(ChainContext& ctx);
	void					PreStep_ShadowPlan(ChainContext& ctx);
	void					PreStep_RtCore(ChainContext& ctx);
	void					PreStep_ShadowPasses(ChainContext& ctx);
	void					PreStep_SunFarFactor(ChainContext& ctx);
	void					PreStep_ChainBegin(ChainContext& ctx);
	void					PreStep_MaterialCapture(ChainContext& ctx);
	void					PreStep_SkinScattering(ChainContext& ctx);
	void					PreStep_SkyViewTable(ChainContext& ctx);
	void					PreStep_VolumetricClouds(ChainContext& ctx);
	void					PreStep_AmbientLightTakeover(ChainContext& ctx);
	void					PreStep_HiZ(ChainContext& ctx);
	void					PreStep_CompositePlan(ChainContext& ctx);
	void					PreStep_ShadowApply(ChainContext& ctx);
	void					PreStep_CoverMapUpdate(ChainContext& ctx);
	void					PreStep_SnowAccumulation(ChainContext& ctx);
	void					PreStep_AmbientOcclusion(ChainContext& ctx);
	void					PreStep_SpecularBouncePlan(ChainContext& ctx);
	void					PreStep_AmbientLight(ChainContext& ctx);
	void					PreStep_LitProbes(ChainContext& ctx);
	void					PreStep_BounceLight(ChainContext& ctx);
	void					PreStep_BloodFlow(ChainContext& ctx);
	void					PreStep_WetFlashlightSpecularUnderwater(ChainContext& ctx);
	void					PreStep_SpillSelfTest(ChainContext& ctx);
	void					PreStep_Reflections(ChainContext& ctx);
	void					PreStep_VolumetricLightGrid(ChainContext& ctx);
	void					PreStep_VolumetricFog(ChainContext& ctx);
	void					PreStep_VolumetricLight(ChainContext& ctx);
	void					PreStep_DustMotes(ChainContext& ctx);	// UNOFFICIAL 2026-11-03
	void					PreStep_Sandstorm(ChainContext& ctx);
	void					PreStep_GodRays(ChainContext& ctx);
	void					PreStep_RayReconstruction(ChainContext& ctx);
	void					PreStep_LumaExposureBloom(ChainContext& ctx);
	void					PreStep_LensAndPreTonemapLUT(ChainContext& ctx);
	void					PostStep_FrameQuad(ChainContext& ctx);
	void					PostStep_ChainBegin(ChainContext& ctx);
	void					PostStep_TonemapProbes(ChainContext& ctx);
	void					PostStep_FusedPassPlan(ChainContext& ctx);
	void					PostStep_FinalEffect(ChainContext& ctx);
	void					PostStep_TAA(ChainContext& ctx);
	void					PostStep_WeatherAndLegacyBloom(ChainContext& ctx);
	void					PostStep_HeatHaze(ChainContext& ctx);
	void					PostStep_VolumetricSmoke(ChainContext& ctx);
	void					PostStep_BarrelHaze(ChainContext& ctx);
	void					PostStep_ColoringLUT(ChainContext& ctx);
	void					PostStep_DepthOfField(ChainContext& ctx);
	void					PostStep_MotionBlur(ChainContext& ctx);
	void					PostStep_LensOverlays(ChainContext& ctx);
	void					PostStep_DitherBuster(ChainContext& ctx);
	void					PostStep_SMAA(ChainContext& ctx);
	void					PostStep_FXAA(ChainContext& ctx);
	void					PostStep_Sharpening(ChainContext& ctx);
	void					PostStep_Cinema(ChainContext& ctx);
	void					PostStep_ImageAdjust(ChainContext& ctx);
	void					PostStep_Bodycam(ChainContext& ctx);
	void					PostStep_Dream(ChainContext& ctx);
	void					PostStep_SkyDither(ChainContext& ctx);
	void					PostStep_SunDepthFitView(ChainContext& ctx);
	void					PostStep_RtSunDebug(ChainContext& ctx);
	void					PostStep_Debug(ChainContext& ctx);
	void					PostStep_CoverMapDebug(ChainContext& ctx);
	void					PostStep_FlickerWatch(ChainContext& ctx);
		
	struct	EffectsStruct {
		AmbientOcclusionEffect*	AmbientOcclusion;
		SkinScatteringEffect*	SkinScattering;	// UNOFFICIAL the screen-space skin blur ([Shaders.Skin.Scattering] ScreenSpace; CREDIT Conor (mrfury97))
		AvgLumaEffect*			AvgLuma;
		SkyViewEffect*			SkyView;	// UNOFFICIAL: the physical sky's table ([Shaders.Sky.Physical])
		AmbientLightEffect*		AmbientLight;	// UNOFFICIAL: AO and bounce light in one pass (replaces both while on)
		BloodLensEffect*		BloodLens;
		BloomEffect*			Bloom;
		BloomLegacyEffect*		BloomLegacy;
		ColoringEffect*			Coloring;
		LUTEffect*				LUT;
		CinemaEffect*			Cinema;
		ExposureEffect*			Exposure;
		FlashlightEffect*		Flashlight;
		CombineDepthEffect*		CombineDepth;
		DepthOfFieldEffect*		DepthOfField;
		DebugEffect*			Debug;
		GodRaysEffect*			GodRays;
		ImageAdjustEffect*		ImageAdjust;
		LensEffect*				Lens;
		LowHFEffect*			LowHF;
		MotionBlurEffect*		MotionBlur;
		NormalsEffect*			Normals;
		RainEffect*				Rain;
		SharpeningEffect*		Sharpening;
		BounceLightEffect*		BounceLight; // UNOFFICIAL, optional, off by default
		SpecularEffect*			Specular;
		SnowEffect*				Snow;
		SnowAccumulationEffect*	SnowAccumulation;
		ShadowsExteriorEffect*	ShadowsExteriors;
		ShadowsInteriorsEffect*	ShadowsInteriors;
		PointShadowsEffect*		PointShadows;
		PointShadows2Effect*	PointShadows2;
		SunShadowsEffect*		SunShadows;
		SunFarEffect*			SunFar;				// UNOFFICIAL 2026-10-19: the land's and the clouds' shadows (SunFar.h)
		UnderwaterEffect*		Underwater;
		VolumetricFogEffect*	VolumetricFog;
		WaterLensEffect*		WaterLens;
		WetWorldEffect*			WetWorld;
		BarrelHazeEffect*		BarrelHaze;
		HeatHazeEffect*			HeatHaze;	// UNOFFICIAL: from Physically-Based-Reloaded
		SandstormEffect*		Sandstorm;	// UNOFFICIAL: from Physically-Based-Reloaded (Balebandro / Alejandro Martinez), GPL-3.0
		VolumetricLightEffect*	VolumetricLight;	// UNOFFICIAL: sun shafts from the shadow maps
		DustMotesEffect*		DustMotes;			// UNOFFICIAL 2026-11-03: dust specks in lit air
		LocalExposureEffect*	LocalExposure;		// 2026-11-04: each neighbourhood's brightness, for the tone mapper
		ReflectionsEffect*		Reflections;		// UNOFFICIAL: screen-space reflections on the ground
		WetnessReflectionsEffect*	WetnessReflections;	// UNOFFICIAL 2026-11-04 Wetness Effects: screen-space reflections on what is wet
		HudRaindropsEffect*		HudRaindrops;	// UNOFFICIAL 2026-11-04 Wetness Effects: rain drops on the first-person arms and weapon (and AllObjects)
		BodycamEffect*			Bodycam;			// UNOFFICIAL, testbed: police body-camera look
		DreamEffect*			Dream;				// "fun" trip look (from the author's DreamSim), off by default
		SkyDitherEffect*		SkyDither;			// UNOFFICIAL: breaks up the sky's banding before the 8-bit screen
		VolumetricCloudsEffect*	VolumetricClouds;	// UNOFFICIAL: 3D clouds in place of the game's cloud layers
		BloodFlowEffect*		BloodFlow;	// UNOFFICIAL: blood that runs and dries, pools under bodies
		VolumetricSmokeEffect*	VolumetricSmoke;
		DitherBusterEffect*		DitherBuster;
		SMAAEffect*				SMAA;
		FXAAEffect*				FXAA;
		TAAEffect*				TAA;		// UNOFFICIAL: from Physically-Based-Reloaded (upstream #78 + Sharpness/MipBias), off by default
	};

	struct ShadersStruct {
		WaterShaders*			Water;
		TonemappingShaders*		Tonemapping;
		POMShaders*				POM;
		PBRShaders*				PBR;
		ShaderCollection*		Blood;
		SkyShaders*				Sky;
		SkinShaders*			Skin;
		GrassShaders*			Grass;
		ParticleShaders*		Particles; // UNOFFICIAL, optional, off by default
		TerrainShaders*			Terrain;
		WetnessEffectsShaders*	Wetness;	// UNOFFICIAL 2026-11-04 Wetness Effects (Includes/Wetness.hlsl)
		PuddlesShaders*			Puddles;	// 2026-11-08 Puddles (effects/Puddles, Includes/Wetness.hlsl PUDDLES_COMPILED)
	};

	// What UpdateConstants worked out about the game this frame.
	struct GameStateStruct {
		float					isDayTime;			// 0 night .. 1 day
		bool					isDayTimeChanged;	// crossed day/night this frame
		float					transitionCurve, dayLight;
		bool					isExterior, isUnderwater, isDialog, isCellChanged;
		bool					isCameraCut;	// UNOFFICIAL 2026-10-27: the cell changed by a jump (a door, fast travel), not by walking over a border
		bool					VATSIsOn, PipBoyIsOn, OverlayIsOn;
		bool					isRainy, isSnow, isCloudy;
	};

	EffectsStruct			Effects;		// filled by RegisterEffect at startup
	ShadersStruct			Shaders;		// filled by RegisterShaderCollection at startup
	EffectsList				EffectsNames;	// lookup by name for the menu and commands
	ShaderList				ShaderNames;	// (families, likewise)
	GameStateStruct			GameState;		// see GameStateStruct
	ShaderConstants			ShaderConst;	// what the TESR_ constants read
	CustomConstants			CustomConst;	// see ShaderFloatValue::GetValueFromConstantTable
	std::map<std::string, D3DXVECTOR4*>	ConstantsTable;	// TESR_ name -> the value its constant reads
	IDirect3DVertexBuffer9*	FrameVertex;	// the full-screen quad (CreateFrameVertex)
	IDirect3DVertexBuffer9*	FrameVertexRect = nullptr;	// UNOFFICIAL 2026-10-27 RenderScale v2: uv 0..Scale (RenderRect::ApplyViewport)
	float					FrameVertexRectScale[2] = { 0.0f, 0.0f };
	FrameChain				Chain;
	NiD3DVertexShader*		WaterVertexShaders[51];	// [0]/[1]: the noise shaders renamed WATERHEIGHTMAP0/1 (ShaderIO.cpp)
	NiD3DPixelShader*		WaterPixelShaders[51];	// (likewise for the pixel shaders)
	TESObjectCELL*			PreviousCell;	// for GameState.isCellChanged
	bool					IsMenuSwitch, orthoRequired;
    bool                    orthoSkyRequired = false;	// UNOFFICIAL 2026-10-17 Skylighting v2: the ortho map, amortised and clean (ShadowManager)
    bool                    orthoSkyFresh = false;		// ... and v2 was not on last frame: redraw the map now
	bool					avglumaRequired, EffectReloadQueued;
	bool					ShaderReloadQueued = false;	// UNOFFICIAL 2026-10-25 (test only): 'reloadshaders' in a shot list (SettingShots.h), done at the frame's end
	// The lamps the shaders light with this frame (GetNearbyLights).
	D3DXVECTOR4				SpotLightPosition[SpotLightsMax], SpotLightColor[SpotLightsMax], SpotLightDirection[SpotLightsMax];
	D3DXVECTOR4				VolumetricData;		// xyz march light position, w first person strength floor
	D3DXMATRIX				SpotLightWorldToLightMatrix[SpotLightsMax];
	D3DXVECTOR4				LightPosition[TrackedLightsMax],
							LightColor[TrackedLightsMax + ShadowCubeMapsScreen],	// PointShadows.fx TESR_LightColor[24]: 12 shadow lamps, then the tracked ones
							LightAttenuation[TrackedLightsMax];
	D3DXVECTOR4				BatchLightPosition[ShadowCubeMapsMax];	// UNOFFICIAL ExteriorLightPoints: every shadow slot's lamp (w radius)
	D3DXVECTOR4				BatchLightColor[ShadowCubeMapsMax];		// ... and colour (w dimmer); zero = none
	void					RenderPointShadowBatches();
	D3DXVECTOR4				ExtraLights[ExtraLightsMax];	// UNOFFICIAL: PointShadows.fx TESR_ExtraLights: position, w = radius x 8 + weight (0 ends the list)
};

