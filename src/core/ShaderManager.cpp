#include "OrthoProbe.h"	// UNOFFICIAL 2026-10-17 test tools (AfterCombine)
#include "FrameWorker.h"	// UNOFFICIAL 2026-11-03 SkyOnWorker
#include "RegisterClash.h"	// UNOFFICIAL 2026-10-31
#include "CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals (TESR_CapturedNormalsData)
#include "ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion (TESR_ObjectMotionData)
#include "TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 the temporal backbone (TESR_FrameNoise, [Main.Main.Quality] SharedNoise)
#include "AlphaHash.h"	// UNOFFICIAL 2026-11-03 SmoothCutouts (TESR_AlphaHash)
#include "Diagnostics.h"	// UNOFFICIAL 2026-11-01 the report burst's VL stages (Diagnostics::VLCapturing)
#include "LampVisibility.h"	// UNOFFICIAL 2026-10-30 VisibleRanking
#include "FlickerWatch.h"	// UNOFFICIAL 2026-10-23 test only
#include "GpuProfiler.h"
#include "HiZ.h"	// UNOFFICIAL 2026-10-26 (deep/fork-hiz)
#include "PixelInspector.h"	// UNOFFICIAL 2026-10-26 (debugging tool)
#include "Diagnostics.h"	// UNOFFICIAL 2026-11-01 the flash detector's stage probes (StageMark)
#include "PointShadowSlots.h"
#include "PointShadowForward.h"
#include "DeferredLamps.h"	// UNOFFICIAL 2026-10-31 DeferredLamps (the deferred records)
#include "CapsuleShadows.h"	// UNOFFICIAL 2026-10-31 CapsuleShadows (INTERIOR-SHADOWS-MODERN steps 2-3)
#include "BackDepth.h"	// UNOFFICIAL 2026-10-31 BackDepth (ROOT-ASSUMPTIONS-PLAN B3)
#include "EffectPrecompile.h"	// UNOFFICIAL 2026-10-28 the effects compiled in parallel at startup
#include "LampFixtures.h"
#include "EffectSelfTest.h"
#include "../effects/GpuParticles/GpuParticles.h"	// UNOFFICIAL 2026-10-26 (NVR DXVK 1.5-particles)
#include "SunDepthFit.h"	// UNOFFICIAL 2026-10-27 FitToDepth
#include "../effects/CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness and snow
#include "RtCore.h"	// UNOFFICIAL 2026-10-28 (NVR DXVK) the ray tracing core
#include "RtSunShadows.h"	// UNOFFICIAL 2026-10-28 RT sun shadows
#include "Upscaler.h"	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (RayReconstructionFrameBegin)
#include "EffectChain.h"	// UNOFFICIAL 2026-11-02 the post-process chain as step tables
#include "FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects: the cell serial its latches wait for
#include <unordered_map>	// UNOFFICIAL 2026-10-27 RenderScale v2 (RenderRect::RescaleRegistered)
extern D3DXVECTOR4 NvrGameMultisample;	// UNOFFICIAL 2026-11-04 Hooks/Settings.cpp (TESR_GameMultisample)

/**
* Initializes the Shader Manager Singleton.
* The Shader Manager creates and holds onto the Effects activated in the Settings Manager, and sets the constants.
*/
static void LoadEarlyShaders(ShaderManager* Manager);	// UNOFFICIAL 2026-10-27 (below LoadShader)

// Enrolls one effect or shader collection by type and slot, in the order the calls are made.
template <typename T, typename Slot> static void EnrollEffect(ShaderManager* Manager, Slot& Destination) {
	Manager->RegisterEffect<T>(&Destination);
}
template <typename T, typename Slot> static void EnrollCollection(ShaderManager* Manager, Slot& Destination) {
	Manager->RegisterShaderCollection<T>(&Destination);
}

struct ConstantBinding { const char* Name; D3DXVECTOR4* Value; };

void ShaderManager::Initialize() {
	TimeLogger startClock;

	Logger::Log("Starting the shaders manager...");
	ShaderManager* sm = new ShaderManager();
	TheShaderManager = sm;
	sm->FrameVertex = NULL;
	sm->EffectReloadQueued = false;

	memset(sm->WaterVertexShaders, NULL, sizeof(WaterVertexShaders));
	memset(sm->WaterPixelShaders, NULL, sizeof(WaterPixelShaders));

	sm->CreateFrameVertex(TheRenderManager->width, TheRenderManager->height, &sm->FrameVertex);

	sm->PreviousCell = nullptr;
	sm->IsMenuSwitch = false;

	// Cache folders the shader and effect code writes into, created in this order; a failure is logged and skipped.
	// The folders are Paths.h's (Data\Shaders\VegasShaders, Data\Textures\VegasShaders, or the old ones of an older install).
	struct CacheFolder { std::string Path; const char* FailureText; };
	const CacheFolder folders[] = {
		{ std::string(ShadersPath) + "Cache\\Bink", "Failed to create shader cache directory: %s" },
		{ std::string(ShadersPath) + "Cache\\Shadows", "Failed to create shader cache directory: %s" },
		{ std::string(EffectsPath) + "Cache", "Failed to create effect cache directory: %s" },
		{ std::string(Paths::LutDir()), "Failed to create LUT directory: %s" },
	};
	for (const CacheFolder& folder : folders) {
		std::error_code failure;
		std::filesystem::create_directories(folder.Path, failure);
		if (failure) Logger::Log(folder.FailureText, failure.message().c_str());
	}

	// initializing the list of effect names
	EffectPrecompile::Start();	// UNOFFICIAL 2026-10-28 ParallelShaderCompile: every effect prepared on worker threads; each LoadEffect waits for its own
	EnrollEffect<AvgLumaEffect>(sm, sm->Effects.AvgLuma);
	EnrollEffect<SkyViewEffect>(sm, sm->Effects.SkyView);
	EnrollEffect<AmbientLightEffect>(sm, sm->Effects.AmbientLight);
	EnrollEffect<AmbientOcclusionEffect>(sm, sm->Effects.AmbientOcclusion);
	EnrollEffect<SkinScatteringEffect>(sm, sm->Effects.SkinScattering);	// UNOFFICIAL Conor's skin shader
	EnrollEffect<BloodLensEffect>(sm, sm->Effects.BloodLens);
	EnrollEffect<BloomEffect>(sm, sm->Effects.Bloom);
	EnrollEffect<BloomLegacyEffect>(sm, sm->Effects.BloomLegacy);
	EnrollEffect<ColoringEffect>(sm, sm->Effects.Coloring);
	EnrollEffect<LUTEffect>(sm, sm->Effects.LUT);
	EnrollEffect<CinemaEffect>(sm, sm->Effects.Cinema);
	EnrollEffect<CombineDepthEffect>(sm, sm->Effects.CombineDepth);
	EnrollEffect<DepthOfFieldEffect>(sm, sm->Effects.DepthOfField);
	EnrollEffect<DebugEffect>(sm, sm->Effects.Debug);
	EnrollEffect<ExposureEffect>(sm, sm->Effects.Exposure);
	EnrollEffect<FlashlightEffect>(sm, sm->Effects.Flashlight);
	EnrollEffect<GodRaysEffect>(sm, sm->Effects.GodRays);
	EnrollEffect<ImageAdjustEffect>(sm, sm->Effects.ImageAdjust);
	EnrollEffect<LensEffect>(sm, sm->Effects.Lens);
	EnrollEffect<LowHFEffect>(sm, sm->Effects.LowHF);
	EnrollEffect<MotionBlurEffect>(sm, sm->Effects.MotionBlur);
	EnrollEffect<NormalsEffect>(sm, sm->Effects.Normals);
	EnrollEffect<RainEffect>(sm, sm->Effects.Rain);
	EnrollEffect<SharpeningEffect>(sm, sm->Effects.Sharpening);
	EnrollEffect<BounceLightEffect>(sm, sm->Effects.BounceLight);
	EnrollEffect<ShadowsExteriorEffect>(sm, sm->Effects.ShadowsExteriors);
	EnrollEffect<ShadowsInteriorsEffect>(sm, sm->Effects.ShadowsInteriors);
	EnrollEffect<PointShadowsEffect>(sm, sm->Effects.PointShadows);
	EnrollEffect<PointShadows2Effect>(sm, sm->Effects.PointShadows2);
	EnrollEffect<SunShadowsEffect>(sm, sm->Effects.SunShadows);
	EnrollEffect<SunFarEffect>(sm, sm->Effects.SunFar);	// UNOFFICIAL 2026-10-19
	EnrollEffect<SpecularEffect>(sm, sm->Effects.Specular);
	EnrollEffect<SnowEffect>(sm, sm->Effects.Snow);
	// UNOFFICIAL 2026-10-27 Persistent wetness and snow (CoverMap.h): the cover map WetWorld and SnowAccumulation read when their
	// Persistent setting is on. No settings section of its own: always on when its file loads, does nothing while both are off.
	EnrollEffect<CoverMapEffect>(sm, TheCoverMap);
	EnrollEffect<SnowAccumulationEffect>(sm, sm->Effects.SnowAccumulation);
	EnrollEffect<UnderwaterEffect>(sm, sm->Effects.Underwater);
	EnrollEffect<VolumetricFogEffect>(sm, sm->Effects.VolumetricFog);
	EnrollEffect<WaterLensEffect>(sm, sm->Effects.WaterLens);
	EnrollEffect<WetWorldEffect>(sm, sm->Effects.WetWorld);
	EnrollEffect<BarrelHazeEffect>(sm, sm->Effects.BarrelHaze);
	EnrollEffect<HeatHazeEffect>(sm, sm->Effects.HeatHaze);
	EnrollEffect<SandstormEffect>(sm, sm->Effects.Sandstorm);
	EnrollEffect<VolumetricLightEffect>(sm, sm->Effects.VolumetricLight);
	EnrollEffect<DustMotesEffect>(sm, sm->Effects.DustMotes);	// UNOFFICIAL 2026-11-03
	EnrollEffect<LocalExposureEffect>(sm, sm->Effects.LocalExposure);	// 2026-11-04
	EnrollEffect<SkyDitherEffect>(sm, sm->Effects.SkyDither);
	EnrollEffect<BodycamEffect>(sm, sm->Effects.Bodycam);
	EnrollEffect<DreamEffect>(sm, sm->Effects.Dream);
	EnrollEffect<ReflectionsEffect>(sm, sm->Effects.Reflections);
	EnrollEffect<WetnessReflectionsEffect>(sm, sm->Effects.WetnessReflections);	// UNOFFICIAL 2026-11-04 Wetness Effects
	EnrollEffect<HudRaindropsEffect>(sm, sm->Effects.HudRaindrops);
	EnrollEffect<VolumetricCloudsEffect>(sm, sm->Effects.VolumetricClouds);
	EnrollEffect<BloodFlowEffect>(sm, sm->Effects.BloodFlow);
	sm->Effects.BarrelHaze->Enabled = sm->Effects.BarrelHaze->Effect != nullptr;
	EnrollEffect<VolumetricSmokeEffect>(sm, sm->Effects.VolumetricSmoke);
	sm->Effects.VolumetricSmoke->Enabled = sm->Effects.VolumetricSmoke->Effect != nullptr;
	EnrollEffect<DitherBusterEffect>(sm, sm->Effects.DitherBuster);
	EnrollEffect<SMAAEffect>(sm, sm->Effects.SMAA);
	EnrollEffect<FXAAEffect>(sm, sm->Effects.FXAA);
	EnrollEffect<TAAEffect>(sm, sm->Effects.TAA);

	EnrollCollection<TonemappingShaders>(sm, sm->Shaders.Tonemapping);
	EnrollCollection<POMShaders>(sm, sm->Shaders.POM);
	EnrollCollection<PBRShaders>(sm, sm->Shaders.PBR);
	EnrollCollection<WaterShaders>(sm, sm->Shaders.Water);
	EnrollCollection<SkyShaders>(sm, sm->Shaders.Sky);
	EnrollCollection<SkinShaders>(sm, sm->Shaders.Skin);
	EnrollCollection<GrassShaders>(sm, sm->Shaders.Grass);
	EnrollCollection<ParticleShaders>(sm, sm->Shaders.Particles);
	EnrollCollection<TerrainShaders>(sm, sm->Shaders.Terrain);
	EnrollCollection<WetnessEffectsShaders>(sm, sm->Shaders.Wetness);	// UNOFFICIAL 2026-11-04 Wetness Effects
	EnrollCollection<PuddlesShaders>(sm, sm->Shaders.Puddles);	// 2026-11-08 Puddles
	LoadEarlyShaders(sm);	// UNOFFICIAL 2026-10-27: shaders the game made while the sets above did not exist yet

	// setup map of constant names: the bindings are inserted in this order, the terrain erosion's own after the first block
	const ConstantBinding firstBlock[] = {
		{ "TESR_WorldTransform", (D3DXVECTOR4*)&TheRenderManager->worldMatrix },
		{ "TESR_ViewTransform", (D3DXVECTOR4*)&TheRenderManager->viewMatrix },
		{ "TESR_InvViewTransform", (D3DXVECTOR4*)&TheRenderManager->InvViewMatrix },
		{ "TESR_ProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->projMatrix },
		{ "TESR_InvProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->InvProjMatrix },
		{ "TESR_WorldViewProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->WorldViewProjMatrix },
		{ "TESR_InvViewProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->InvViewProjMatrix },
		{ "TESR_ViewProjectionTransform", (D3DXVECTOR4*)&TheRenderManager->ViewProjMatrix },
		{ "TESR_OcclusionWorldViewProjTransform", (D3DXVECTOR4*)&sm->ShaderConst.OcclusionMap.OcclusionWorldViewProj },
		{ "TESR_LightPosition", (D3DXVECTOR4*)&sm->LightPosition },
		{ "TESR_LightColor", (D3DXVECTOR4*)&sm->LightColor },
		{ "TESR_ExtraLights", (D3DXVECTOR4*)&sm->ExtraLights },	// UNOFFICIAL, GetNearbyLights
		{ "TESR_SpotLightPosition", (D3DXVECTOR4*)&sm->SpotLightPosition },
		{ "TESR_SpotLightColor", (D3DXVECTOR4*)&sm->SpotLightColor },
		{ "TESR_SpotLightDirection", (D3DXVECTOR4*)&sm->SpotLightDirection },
		{ "TESR_SpotLightToWorldTransform", (D3DXVECTOR4*)&sm->SpotLightWorldToLightMatrix[0] },
		{ "TESR_VolumetricData", &sm->VolumetricData },
		{ "TESR_ViewSpaceLightDir", &sm->ShaderConst.ViewSpaceLightDir },
		{ "TESR_ScreenSpaceLightDir", &sm->ShaderConst.ScreenSpaceLightDir },
		{ "TESR_ReciprocalResolution", &sm->ShaderConst.ReciprocalResolution },
		{ "TESR_GameMultisample", &NvrGameMultisample },	// UNOFFICIAL 2026-11-04: x = the game's MSAA samples (Hooks/Settings.cpp)
		// UNOFFICIAL 2026-10-27 RenderScale (effects\TAAUpscale.cpp): xy = the internal resolution / the screen's while the world is drawn
		// smaller, else 1 (always registered: an unregistered TESR_ constant would read 0). The water shaders' refraction lookup uses it.
		{ "TESR_RenderScale", &TAAEffect::RenderScaleConstant },
		{ "TESR_CameraForward", &TheRenderManager->CameraForward },
		{ "TESR_DepthConstants", &TheRenderManager->DepthConstants },
		{ "TESR_CameraData", &TheRenderManager->CameraData },
		{ "TESR_CameraPosition", &TheRenderManager->CameraPosition },
		{ "TESR_SunDirection", &sm->ShaderConst.SunDir },
		{ "TESR_SunPosition", &sm->ShaderConst.SunPosition },
		{ "TESR_SunTiming", &sm->ShaderConst.SunTiming },
		{ "TESR_SunAmount", &sm->ShaderConst.SunAmount },
		{ "TESR_GameTime", &sm->ShaderConst.GameTime },
		{ "TESR_CapturedNormalsData", &CapturedNormals::Data },	// UNOFFICIAL 2026-10-28 CapturedNormals
		{ "TESR_MaterialBufferData", &CapturedNormals::MaterialData },
		{ "TESR_AlbedoBufferData", &CapturedNormals::AlbedoData },	// UNOFFICIAL 2026-10-31 AlbedoBuffer
		{ "TESR_BackDepthData", &BackDepth::Data },	// UNOFFICIAL 2026-10-31 BackDepth
		{ "TESR_ObjectMotionData", &ObjectMotion::Data },	// UNOFFICIAL 2026-10-29 ObjectMotion
		{ "TESR_FrameNoise", &TemporalBackbone::Constant },	// UNOFFICIAL 2026-11-01 the temporal backbone
		{ "TESR_AlphaHash", &AlphaHash::Constant },	// UNOFFICIAL 2026-11-03 SmoothCutouts
		{ "TESR_FogData", &sm->ShaderConst.fogData },
		{ "TESR_FogDistance", &sm->ShaderConst.fogDistance },
		{ "TESR_FogColor", &sm->ShaderConst.fogColor },
	};
	for (const ConstantBinding& binding : firstBlock) sm->RegisterConstant(binding.Name, binding.Value);

	TerrainErosion::Initialize();   // [Shaders.Terrain.Erosion]: its constants

	const ConstantBinding secondBlock[] = {
		{ "TESR_SunColor", &sm->ShaderConst.sunColor },
		{ "TESR_SunDiskColor", &sm->ShaderConst.sunDiskColor },
		{ "TESR_SunAmbient", &sm->ShaderConst.sunAmbient },
		{ "TESR_SkyColor", &sm->ShaderConst.skyColor },
		{ "TESR_SkyLowColor", &sm->ShaderConst.skyLowColor },
		{ "TESR_HorizonColor", &sm->ShaderConst.horizonColor },
	};
	for (const ConstantBinding& binding : secondBlock) sm->RegisterConstant(binding.Name, binding.Value);

	sm->InitializeConstants();
	startClock.LogTime("ShaderManager::Initialize");
}

void ShaderManager::CreateFrameVertex(UInt32 Width, UInt32 Height, IDirect3DVertexBuffer9** FrameVertex, float ScaleX, float ScaleY) {

	void* data = NULL;
	const float halfTexelU = (1.0f / (float)Width) * 0.5f;
	const float halfTexelV = (1.0f / (float)Height) * 0.5f;

	// UNOFFICIAL 2026-10-27 RenderScale v2: ScaleX/Y below 1 = the rect quad (uv 0..Scale, same half-texel offset).
	const FrameVS quad[] = {
		{ -1.0f,  1.0f, 1.0f, halfTexelU,           halfTexelV },
		{ -1.0f, -1.0f, 1.0f, halfTexelU,           ScaleY + halfTexelV },
		{  1.0f,  1.0f, 1.0f, ScaleX + halfTexelU, halfTexelV },
		{  1.0f, -1.0f, 1.0f, ScaleX + halfTexelU, ScaleY + halfTexelV }
	};
	TheRenderManager->device->CreateVertexBuffer(sizeof(quad), D3DUSAGE_WRITEONLY, FrameFVF, D3DPOOL_DEFAULT, FrameVertex, NULL);
	IDirect3DVertexBuffer9* const made = *FrameVertex;
	made->Lock(0, 0, &data, NULL);
	memcpy(data, quad, sizeof(quad));
	made->Unlock();

}

// UNOFFICIAL 2026-10-27 RenderScale v2 (core/RenderRect.h, docs/next/RENDERSCALE-V2.md).
bool RenderRect::ScreenShaped(unsigned w, unsigned h) {
	const unsigned sw = TheRenderManager->width, sh = TheRenderManager->height;
	if (!w || !h || !sw || !sh || w > sw || h > sh) return false;
	const float aspect = (float)w / (float)h, screen = (float)sw / (float)sh;
	return fabsf(aspect - screen) <= 0.01f * screen;
}

void RenderRect::ApplyViewport(IDirect3DDevice9* Device) {
	ShaderManager* sm = TheShaderManager;
	if (!On) return;	// off: nothing at all (the frame exactly as before)
	IDirect3DSurface9* target = nullptr;
	D3DSURFACE_DESC desc = {};
	const bool got = SUCCEEDED(Device->GetRenderTarget(0, &target)) && target && SUCCEEDED(target->GetDesc(&desc));
	if (target) target->Release();
	if (!got || !ScreenShaped(desc.Width, desc.Height)) {
		// a whole-target pass: only undo our own quad (a caller's own vertex buffer stays bound)
		IDirect3DVertexBuffer9* bound = nullptr;
		UINT offset = 0, stride = 0;
		if (SUCCEEDED(Device->GetStreamSource(0, &bound, &offset, &stride)) && bound) {
			if (bound == sm->FrameVertexRect) Device->SetStreamSource(0, sm->FrameVertex, 0, sizeof(FrameVS));
			bound->Release();
		}
		return;
	}
	if (!sm->FrameVertexRect || sm->FrameVertexRectScale[0] != ScaleX || sm->FrameVertexRectScale[1] != ScaleY) {
		if (sm->FrameVertexRect) { sm->FrameVertexRect->Release(); sm->FrameVertexRect = nullptr; }
		sm->CreateFrameVertex(TheRenderManager->width, TheRenderManager->height, &sm->FrameVertexRect, ScaleX, ScaleY);
		sm->FrameVertexRectScale[0] = ScaleX;
		sm->FrameVertexRectScale[1] = ScaleY;
	}
	const D3DVIEWPORT9 port = { 0, 0, Width(desc.Width), Height(desc.Height), 0.0f, 1.0f };
	Device->SetViewport(&port);
	Device->SetStreamSource(0, sm->FrameVertexRect ? sm->FrameVertexRect : sm->FrameVertex, 0, sizeof(FrameVS));
}

void RenderRect::WholeViewport(IDirect3DDevice9* Device, D3DVIEWPORT9& Viewport) {
	if (!On || Viewport.X || Viewport.Y) return;
	IDirect3DSurface9* target = nullptr;
	D3DSURFACE_DESC desc = {};
	const bool got = SUCCEEDED(Device->GetRenderTarget(0, &target)) && target && SUCCEEDED(target->GetDesc(&desc));
	if (target) target->Release();
	if (got && ScreenShaped(desc.Width, desc.Height) && Viewport.Width == Width(desc.Width) && Viewport.Height == Height(desc.Height)) {
		Viewport.Width = desc.Width;
		Viewport.Height = desc.Height;
	}
}

namespace {
	std::vector<IDirect3DSurface9*> RescaleList;	// this rect frame's buffers read after the upscale (RenderRect::RescaleAfterUpscale)
	struct RescaleTemp { IDirect3DTexture9* Texture = nullptr; IDirect3DSurface9* Surface = nullptr; D3DSURFACE_DESC Desc = {}; };
	std::unordered_map<IDirect3DSurface9*, RescaleTemp> RescaleTemps;	// one spare of the same size and format per buffer
}

void RenderRect::RescaleAfterUpscale(IDirect3DSurface9* Surface) {
	if (!On || !Surface || std::find(RescaleList.begin(), RescaleList.end(), Surface) != RescaleList.end()) return;
	RescaleList.push_back(Surface);
}

void RenderRect::RescaleRegistered(IDirect3DDevice9* Device) {
	for (IDirect3DSurface9* buffer : RescaleList) {
		D3DSURFACE_DESC desc = {};
		if (FAILED(buffer->GetDesc(&desc))) continue;
		RescaleTemp& spare = RescaleTemps[buffer];
		if (!spare.Surface || spare.Desc.Width != desc.Width || spare.Desc.Height != desc.Height || spare.Desc.Format != desc.Format) {
			if (spare.Surface) spare.Surface->Release();
			if (spare.Texture) spare.Texture->Release();
			spare = RescaleTemp();
			if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &spare.Texture, nullptr)) ||
				FAILED(spare.Texture->GetSurfaceLevel(0, &spare.Surface))) {
				if (spare.Texture) spare.Texture->Release();
				spare = RescaleTemp();
				static bool logged = false;
				if (!logged) Logger::Log("[ERROR] UNOFFICIAL RenderScale v2: no spare to rescale a %ux%u buffer after the upscale (read at the wrong size).", desc.Width, desc.Height);
				logged = true;
				continue;
			}
			spare.Desc = desc;
		}
		const RECT drawn = { 0, 0, (LONG)(std::max)(1.0f, floorf((float)desc.Width * LastScaleX + 0.5f)), (LONG)(std::max)(1.0f, floorf((float)desc.Height * LastScaleY + 0.5f)) };
		if (SUCCEEDED(Device->StretchRect(buffer, &drawn, spare.Surface, nullptr, D3DTEXF_LINEAR)))
			Device->StretchRect(spare.Surface, nullptr, buffer, nullptr, D3DTEXF_NONE);
	}
	RescaleList.clear();
}


/*
* Makes an effect, files it under its name, then runs its setup in order: settings, constants, textures, shader.
*/
template <typename T> void ShaderManager::RegisterEffect(T** slot) {
	T* made = new T();
	*slot = made;

	EffectsNames[made->Name] = (EffectRecord**)slot;
	made->UpdateSettings();
	made->RegisterConstants();
	made->RegisterTextures();
	made->LoadEffect();
}


template <typename T> void ShaderManager::RegisterShaderCollection(T** slot) {
	T* made = new T();
	*slot = made;

	ShaderNames[made->Name] = (ShaderCollection**)slot;
	made->RegisterConstants();
}


/*
 * Drops the cached texture pointer for a named sampler across EVERY loaded game shader.
 *
 * Counterpart to EffectRecord's per-effect ClearSampler. Call both whenever a TESR_ texture
 * is released and recreated (see ShadowsExteriorEffect::RecreateTextures): each shader owns
 * a private TextureRecord holding a raw IDirect3DTexture9*, which SetCT only re-resolves
 * when it is null. Miss one and it keeps sampling the released texture -- which the D3D9
 * device is usually still holding a reference to, so instead of failing it quietly returns
 * the last contents that were rendered into it.
 */
void ShaderManager::ClearShaderSamplers(const char* textureName, size_t byteCount) {
	for (const auto& Named : ShaderNames) {
		ShaderCollection* collection = Named.second ? *Named.second : nullptr;
		if (!collection) continue;

		for (auto& vertex : collection->VertexShaderList) {
			for (int slot = 0; slot < 3; slot++)  // Default / Exterior / Interior
				if (vertex->ShaderProg[slot]) vertex->ShaderProg[slot]->ClearSampler(textureName, byteCount);
		}
		for (auto& pixel : collection->PixelShaderList) {
			for (int slot = 0; slot < 3; slot++)
				if (pixel->ShaderProg[slot]) pixel->ShaderProg[slot]->ClearSampler(textureName, byteCount);
		}
	}
}


// A TESR_ constant of that name reads the value at this address from now on (a later call for the name wins).
void ShaderManager::RegisterConstant(const char* constantName, D3DXVECTOR4* value) { ConstantsTable[constantName] = value; }


void ShaderManager::InitializeConstants() {
	ShaderConst.pWeather = nullptr;	// no weather seen yet

	// x, y: one pixel in uv; z: the aspect ratio; w: kept for the field of view (0 until set).
	const float w = (float)TheRenderManager->width, h = (float)TheRenderManager->height;
	ShaderConst.ReciprocalResolution = D3DXVECTOR4(1.0f / w, 1.0f / h, w / h, 0.0f);
}


/*
* Once (or twice) a rendered frame: the game's state as NVR's shaders and effects see it -- menus, cell, weather, time of day,
* sun, sky colours, fog -- then every effect's and family's settings (after a change) and constants.
*/
void NvrMsaaWarningTick();	// Hooks/Settings.cpp (UNOFFICIAL 2026-10-27): the game's MSAA is on: said once on screen

// Writes an RGBA constant from a colour's three channels and an explicit fourth value.
static void StoreRgba(D3DXVECTOR4& target, float r, float g, float b, float a) {
	target.x = r;
	target.y = g;
	target.z = b;
	target.w = a;
}

void ShaderManager::UpdateConstants() {

	if (!TheSettingManager->SettingsMain.Main.RenderEffects) return;	// NVR's master switch is off

	TimeLogger updateClock;
	MarkShadersFrom = TheSettingManager->GetSettingI("Shaders.PBR.Main", "MarkShadersFrom");	// UNOFFICIAL 2026-10-22 MarkShaders (test only)
	MarkShadersTo = TheSettingManager->GetSettingI("Shaders.PBR.Main", "MarkShadersTo");
	MarkShadersFamily = TheSettingManager->GetSettingI("Shaders.PBR.Main", "MarkShadersFamily");
	MarkShadersVanilla = TheSettingManager->GetSettingI("Shaders.PBR.Main", "MarkShadersVanilla");
	MarkShadersVsFrom = TheSettingManager->GetSettingI("Shaders.PBR.Main", "MarkShadersVsFrom");
	MarkShadersVsTo = TheSettingManager->GetSettingI("Shaders.PBR.Main", "MarkShadersVsTo");
	{	// UNOFFICIAL 2026-11-09 (Render.cpp SetShadersHook): on unless the toml says false (a saved config from before the key: on)
		SettingManager::Configuration::ConfigNode node;
		GameShadersInMenus = !TheSettingManager->Config.FillNode(&node, "Shaders.PBR.Main", "GameShadersInMenus") || node.BoolValue;
	}
	GameShaderEyes = TheSettingManager->GetSettingI("Shaders.PBR.Main", "GameShaderEyes") != 0;	// UNOFFICIAL 2026-10-27 (Render.cpp SetShadersHook)
	EyeShaderOld = TheSettingManager->GetSettingI("Shaders.PBR.Main", "EyeShaderOld") != 0;	// UNOFFICIAL 2026-11-09 (Render.cpp GameEyes)
	NvrMsaaWarningTick();
	// MarkShadersVanilla 2 (test only, once): the game's lamp falloff textures (BSShaderManager::spAttenuationXY 0x011F9534 / Z 0x011F9538)
	// saved next to the log as PNG, to compare their shape with the shaders' 1 - (d / r)^2.
	if (MarkShadersVanilla == 2) {
		static bool dumped = false;
		if (!dumped) {
			dumped = true;
			const UInt32 addrs[2] = { 0x011F9534, 0x011F9538 };
			for (int i = 0; i < 2; i++) {
				NiTexture* tex = *(NiTexture**)addrs[i];
				IDirect3DBaseTexture9* d3d = (tex && tex->rendererData) ? tex->rendererData->dTexture : nullptr;
				char path[MAX_PATH];
				sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\NVR-attenuation-%s.png", i ? "Z" : "XY");
				HRESULT hr = d3d ? D3DXSaveTextureToFileA(path, D3DXIFF_PNG, d3d, NULL) : E_FAIL;
				D3DSURFACE_DESC desc = {};
				if (d3d && d3d->GetType() == D3DRTYPE_TEXTURE) ((IDirect3DTexture9*)d3d)->GetLevelDesc(0, &desc);
				Logger::Log("MarkShaders: attenuation %s texture %p d3d %p type %d %ux%u format %d saved %s (hr %08X)", i ? "Z" : "XY", tex, d3d, d3d ? (int)d3d->GetType() : -1, desc.Width, desc.Height, (int)desc.Format, path, (unsigned)hr);
			}
		}
	}

	Sky* const theSky = Tes->sky;
	NiNode* const sunNode = theSky->sun->RootNode;
	TESWeather* const weather = theSky->firstWeather;
	TESObjectCELL* const cell = Player->parentCell;
	// The optional passes are off until an effect asks for them below (orthoSky: UNOFFICIAL Skylighting v2, AmbientOcclusionEffect::UpdateConstants).
	orthoRequired = orthoSkyRequired = false;
	avglumaRequired = Effects.Debug->Enabled;	// the debug view shows the average luma

	// Menus and overlays.
	auto* const ui = InterfaceManager;
	GameState.PipBoyIsOn = ui->IsPipBoyOpen();
	GameState.VATSIsOn = ui->IsActive(Menu::kMenuType_VATS);
	GameState.FaceMenuIsOn = ui->IsActive(Menu::kMenuType_RaceSex) || ui->IsActive(Menu::kMenuType_CharGen);	// UNOFFICIAL 2026-11-09 (GameShadersInMenus)

	// A game menu counts as open when it is active and, if the live menu API is there, paused.
	const bool hasLiveMenus = TheGameMenuManager->IsLiveMenu != nullptr;
	auto menuPaused = [&](auto menuType) {
		return !hasLiveMenus || TheGameMenuManager->IsLiveMenu(menuType, false, false) == GameMenuManager::MENU_PAUSED;
	};
	const bool terminalOpen = (ui->IsActive(Menu::kMenuType_Computers) && menuPaused(Menu::kMenuType_Computers)) ||
		(ui->IsActive(Menu::kMenuType_Hacking) && menuPaused(Menu::kMenuType_Hacking));
	const bool lockpickOpen = LockPickMenu::GetSingleton() && menuPaused(Menu::kMenuType_LockPick);
	// Full-screen game menus over a still world (the effects are skipped under them); asked in this order, first hit wins.
	static const UInt32 overlayMenus[] = { Menu::kMenuType_Surgery, Menu::kMenuType_SlotMachine, Menu::kMenuType_Blackjack,
		Menu::kMenuType_Roulette, Menu::kMenuType_Caravan };
	bool overlay = terminalOpen || lockpickOpen;
	for (UInt32 menu : overlayMenus) overlay = overlay || ui->IsActive(menu);
	GameState.OverlayIsOn = overlay;
	GameState.isDialog = ui->IsActive(Menu::kMenuType_Dialog);
	// UNOFFICIAL 2026-11-01 LockpickEffects (user: the lock is black in the lockpick view; "it's pbr"): while the lockpick screen is open the
	// objects are drawn with the game's own shaders, as the menu's PBR switch does (the lock is lit by the menu's own light, which NVR's
	// object shaders do not light it with). Back as they were when it closes. LockpickEffects = true: as before.
	{
		static bool pbrHeld = false;
		const bool lockOpen = LockPickMenu::GetSingleton() && InterfaceManager->IsActive(Menu::kMenuType_LockPick) &&
			!TheSettingManager->GetSettingI("Main.Main.Misc", "LockpickEffects");
		if (lockOpen != pbrHeld && Shaders.PBR && Shaders.PBR->Enabled) {
			for (auto& ps : Shaders.PBR->PixelShaderList) ps->Enabled = !lockOpen;
			for (auto& vs : Shaders.PBR->VertexShaderList) vs->Enabled = !lockOpen;
			pbrHeld = lockOpen;
		}
		else if (!lockOpen && pbrHeld) pbrHeld = false;	// (PBR switched off in the menu meanwhile: its own state stands)
	}

	if (!cell) return;	// no cell (main menu, loading): nothing below has its data

	GameState.isExterior = !cell->IsInterior();	// (the cell's "behave like exterior" flag is not used: it is unreliable)
	// fixes-a #43: UpdateConstants runs twice a rendered frame; the second call used to see the cell already stored and clear the
	// flag, so every reader after it (TAA's history reset, AmbientLight, BounceLight, clouds) never saw a change. The change is
	// worked out once per frame (FrameNumber) and held for the whole frame; the one-time work below still runs once.
	static unsigned int cellFrame = ~0u;
	static bool cellChangedThisFrame = false;
	// UNOFFICIAL 2026-10-27 (user, FSR's RenderScale: "the effects jitter a bit occasionally when walking around ... vol clouds drop to a
	// really low res pixelated version for a few frames"; their log: every jump at "cellchg"): walking over an exterior cell border
	// changes the cell too, and every frame-to-frame history (TAA, FSR, AmbientLight, BounceLight, the clouds...) took it for a
	// teleport and started again. isCameraCut is a cell change that really is a jump: into or out of an interior, between interiors,
	// or the camera moved more than 512 units in one frame (fast travel, coc). The histories reset on that; isCellChanged stays for
	// what is about the cell itself (presets, the interior balance).
	static bool cameraCutThisFrame = false;
	static D3DXVECTOR4 cutCamera(0.0f, 0.0f, 0.0f, 0.0f);
	static bool cutInterior = false, cutKnown = false;
	const bool firstCallOfFrame = TheFrameRateManager->FrameNumber != cellFrame;
	if (firstCallOfFrame) {
		cellFrame = TheFrameRateManager->FrameNumber;
		cellChangedThisFrame = cell != PreviousCell;
		PreviousCell = cell;
		const bool interior = cell->IsInterior();
		const D3DXVECTOR4 camera = TheRenderManager->CameraPosition;
		const float dx = camera.x - cutCamera.x, dy = camera.y - cutCamera.y, dz = camera.z - cutCamera.z;
		cameraCutThisFrame = cellChangedThisFrame && (!cutKnown || interior || cutInterior || dx * dx + dy * dy + dz * dz > 512.0f * 512.0f);
		cutCamera = camera;
		cutInterior = interior;
		cutKnown = true;
	}
	GameState.isCellChanged = cellChangedThisFrame;
	GameState.isCameraCut = cameraCutThisFrame;
	TemporalBackbone::Update();	// UNOFFICIAL 2026-11-01 once a frame (reads isCameraCut): FrameIndex, TemporalResolve, HistoryValid, TESR_FrameNoise
	if (firstCallOfFrame && GameState.isCellChanged) {
		TheSettingManager->SettingsChanged = true;	// a new cell: every effect reads its settings again
		FailOnce::NextCell();	// UNOFFICIAL 2026-11-02 RetryFailedEffects: parts that failed in the last cell may try again
		// Preset resolve+apply as early in this function as possible -- before
		// anything below reads SettingsMain, so nothing this frame observes a
		// stale value (docs/preset-manager-design.md § "Application mechanism").
		//
		// Gated on the master toggle and on ShouldReResolve, not on isCellChanged
		// alone: Override presets are assigned per-worldspace outdoors (no
		// per-cell keyword tier exists for exteriors), so re-resolving on every
		// exterior cell border within the same worldspace was a no-op at best
		// and, at worst, silently clobbered any live tweak that hadn't been
		// saved yet the moment the player happened to cross one. ShouldReResolve
		// keeps interiors at today's per-cell granularity and always re-resolves
		// across an interior<->exterior boundary. The short-circuit order
		// matters: ShouldReResolve has side effects (updates its cached "last
		// resolved identity"), so it must not run while the master toggle is
		// off, or re-enabling it later would see a stale-but-matching cache and
		// skip the immediate resolve it needs to do.
		if (TheSettingManager->SettingsMain.Main.PresetManagerEnabled && PresetManager::ShouldReResolve(cell))
			PresetManager::ResolveAndApply(cell, true);	// UNOFFICIAL 2026-10-19: automatic (keeps live edits, see PresetManager.h)
	}

	// Weather.
	auto weatherIs = [weather](TESWeather::WeatherType type) { return weather && weather->GetWeatherType() == type; };
	GameState.isUnderwater = theSky->GetIsUnderWater();
	GameState.isRainy = weatherIs(TESWeather::WeatherType::kType_Rainy);
	GameState.isSnow = weatherIs(TESWeather::WeatherType::kType_Snow);
	// UNOFFICIAL 2026-10-19: [Shaders.Snow.Main] ForceSnow -- the snow effects act as if it snowed (no snow weathers in the game or
	// most load orders, so macintroll's snow shaders could not be seen otherwise)
	if (!GameState.isSnow && TheSettingManager->GetSettingI("Shaders.Snow.Main", "ForceSnow")) { GameState.isSnow = true; GameState.isRainy = false; }
	// UNOFFICIAL 2026-10-25: [Shaders.Precipitations.Main] ForceRain, the same for rain (FalloutNV.esm has no rainy weather at all)
	else if (!GameState.isRainy && TheSettingManager->GetSettingI("Shaders.Precipitations.Main", "ForceRain")) GameState.isRainy = true;
	// UNOFFICIAL 2026-10-26 GPU particles (NVR DXVK 1.5-particles): their drops land on the ortho map's roofs and ground
	if (GpuParticles::Wanted()) orthoRequired = true;
	GameState.isCloudy = weatherIs(TESWeather::WeatherType::kType_Cloudy);

	// the hour of the day, kept under 24, and the four sun-phase edges the sky reports
	const float gameHour = fmod(TimeGlobals::Get()->GameHour->data, 24);
	const float riseStart = theSky->GetSunriseBegin();
	const float riseEnd = theSky->GetSunriseEnd();
	const float setStart = theSky->GetSunsetBegin();
	const float setEnd = theSky->GetSunsetEnd();

	// the sun amount for shaders (not used by any shader yet): 1 between sunrise and sunset, else 0
	const float risen = step(riseStart, riseEnd, gameHour); // 0 at night to 1 after sunrise
	const float setting = step(setEnd, setStart, gameHour);  // 1 before sunset to 0 at night
	GameState.isDayTime = risen * setting;

	// x: the sunrise colours begin, y: sunrise ends, z: sunset begins, w: the sunset colours end.
	ShaderConst.SunTiming = D3DXVECTOR4(theSky->GetSunriseColorBegin(), riseEnd, setStart, theSky->GetSunsetColorEnd());

	// fake sunset time tracking with more time given before sunrise/sunset
	const float risenEarly = step(riseStart - 1.0f, riseEnd - 1.0f, gameHour); // 0 at night to 1 after sunrise
	const float settingEarly = step(setEnd + 1.0f, setStart + 1.0f, gameHour);  // 1 before sunset to 0 at night
	const float newDayLight = risenEarly * settingEarly;
	const float transitionPower = max(0.01f, TheSettingManager->SettingsMain.Transitions.TransitionCurvePower);
	// The day/night blend most transition settings follow: it moves mostly in the second half of dusk.
	GameState.transitionCurve = pow(smoothStep(0.0f, 1.0f, newDayLight), transitionPower);
	GameState.isDayTimeChanged = newDayLight != GameState.dayLight;	// effects refresh their settings through dawn and dusk
	GameState.dayLight = newDayLight;

	// x: game time (ms), y: the hour, z: NVR's clock, w: the last frame's length (s).
	D3DXVECTOR4& gameTime = ShaderConst.GameTime;
	gameTime.x = TimeGlobals::GetGameTime();
	gameTime.y = gameHour;
	gameTime.z = (float)TheFrameRateManager->Time;
	gameTime.w = TheFrameRateManager->ElapsedTime;

	D3DXVECTOR4& sunPos = ShaderConst.SunPosition;
	D3DXVECTOR4& sunDir = ShaderConst.SunDir;
	sunPos = sunNode->m_localTransform.pos.toD3DXVEC4();
	sunPos.w = 0.0f;
	// SunRoot (the sun disc's NiNode) is only positioned by the game's own sky-dome rendering, which only runs while an exterior sky is
	// drawn. On a cold load into an interior save it sits at a zero vector, and normalising that gives NaN, which poisons every consumer
	// of SunPosition (the sky irradiance, so PBR Skylighting goes dark until an exterior has been drawn). Fall back to straight up.
	if (D3DXVec4LengthSq(&sunPos) > 0.0001f) D3DXVec4Normalize(&sunPos, &sunPos);
	else sunPos = D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);	// straight up: a safe, neutral default
	sunPos.w = 1.0f;

	// The direction light comes from: the reverse of the game's directional light, normalised in xyz.
	sunDir = Tes->directionalLight->direction.toD3DXVEC4() * -1.0f;
	sunDir.w = 0.0f;
	D3DXVec4Normalize(&sunDir, &sunDir);
	sunDir.w = 1.0f;

	// Outdoors by day the sun disc's position is followed instead (the moon's light at night); otherwise the
	// position is mirrored below the horizon.
	const bool followSunDisc = GameState.isExterior && GameState.dayLight > 0.5;
	if (followSunDisc) sunDir = sunPos;
	else sunPos.z = -sunPos.z;

	// The same direction in clip space and in view space, for the screen-space effects.
	auto sunDirThrough = [&sunDir](D3DXVECTOR4& out, const D3DXMATRIX& transform) {
		D3DXVec4Transform(&out, &sunDir, &transform);
		D3DXVec4Normalize(&out, &out);
	};
	sunDirThrough(ShaderConst.ScreenSpaceLightDir, TheRenderManager->ViewProjMatrix);
	sunDirThrough(ShaderConst.ViewSpaceLightDir, TheRenderManager->ViewMatrix);

	ShaderConst.sunGlare = weather ? weather->GetSunGlare() / 255.0f : 0.5f;	// no weather: half glare

	// y: day by the sky's timetable (0/1, before smoothing); x: the smoothed daylight.
	D3DXVECTOR4& sunAmount = ShaderConst.SunAmount;
	sunAmount.y = GameState.isDayTime;
	GameState.isDayTime = smoothStep(0, 1, GameState.dayLight);
	sunAmount.x = GameState.isDayTime;

	const auto& sunLight = theSky->sunDirectional;
	StoreRgba(ShaderConst.sunColor, sunLight.r, sunLight.g, sunLight.b, ShaderConst.sunGlare);

	// The sun disc: its own sky colour when the sky asks for it, else the sunlight's.
	if (!Shaders.Sky->useSunDiskColor) ShaderConst.sunDiskColor = ShaderConst.sunColor;
	else StoreRgba(ShaderConst.sunDiskColor, theSky->SunColor.r, theSky->SunColor.g, theSky->SunColor.b, 1.0f);
	Shaders.Sky->UpdateMoonlight();	// UNOFFICIAL 2026-10-25 MoonBrightness: the night light (and sunColor) scaled; the sky's own colours keep the game's

	ShaderConst.windSpeed = theSky->windSpeed;

	StoreRgba(ShaderConst.fogColor, theSky->fogColor.r, theSky->fogColor.g, theSky->fogColor.b, 1.0f);
	StoreRgba(ShaderConst.horizonColor, theSky->Horizon.r, theSky->Horizon.g, theSky->Horizon.b, 1.0f);
	StoreRgba(ShaderConst.sunAmbient, theSky->sunAmbient.r, theSky->sunAmbient.g, theSky->sunAmbient.b, 1.0f);
	StoreRgba(ShaderConst.skyLowColor, theSky->SkyLower.r, theSky->SkyLower.g, theSky->SkyLower.b, 1.0f);
	StoreRgba(ShaderConst.skyColor, theSky->skyUpper.r, theSky->skyUpper.g, theSky->skyUpper.b, 1.0f);
	Shaders.Sky->UpdateFogSky();	// UNOFFICIAL 2026-10-27 VolumetricFog's copies of the three sky colours with the sky look in them

	// Fog: x near, y far, z glare, w power. Far as the sky has it, capped in interiors as the game does; near as the
	// sky has it unless that is negative or past the far plane (then 17% of the far distance).
	D3DXVECTOR4& fog = ShaderConst.fogData;
	const float skyFar = theSky->fogFarPlane, skyNear = theSky->fogNearPlane;
	const bool capInterior = !GameState.isExterior && (skyFar < 0 || skyFar > 163840);
	fog.y = capInterior ? 163840 : skyFar;
	fog.x = (skyNear < 0 || fog.y < skyNear) ? fog.y * 0.17 : skyNear;
	fog.z = ShaderConst.sunGlare;
	fog.w = theSky->fogPower;
	ShaderConst.fogDistance = D3DXVECTOR4(fog.x, fog.y, 1.0f, ShaderConst.sunGlare);

	updateClock.LogTime("ShaderManager::UpdateConstants for generic constants");

	// After a settings change: every effect and family re-reads its settings (on or off), and the sky glare
	// several shaders read whatever their own state.
	if (TheSettingManager->SettingsChanged) {
		for (const auto& entry : EffectsNames) (*entry.second)->UpdateSettings();
		for (const auto& entry : ShaderNames) (*entry.second)->UpdateSettings();
		sunAmount.w = TheSettingManager->GetSettingF("Shaders.Sky.Main", "GlareStrength");
		updateClock.LogTime("ShaderManager::UpdateSettings for shaders & effects");
	}

	// Constants of what is on: the families, then the effects (each effect's time kept for the menu).
	for (const auto& entry : ShaderNames) {
		ShaderCollection* family = *entry.second;
		if (family->Enabled) family->UpdateConstants();
	}
	updateClock.LogTime("ShaderManager::UpdateConstants for shaders");

	for (const auto& entry : EffectsNames) {
		EffectRecord* record = *entry.second;
		if (!record->Enabled) { record->constantUpdateTime = 0; continue; }
		record->UpdateConstants();
		record->constantUpdateTime = updateClock.LogTime(record->Name);
	}
	// UNOFFICIAL 2026-10-20 (skyamb): Skylighting's constants while AmbientOcclusion is off too: AmbientLight hosts the sky test then
	// (AmbientOcclusion.h UpdateSkyConstants; with AmbientLight off as well the sky stays off, as before).
	if (!Effects.AmbientOcclusion->Enabled) Effects.AmbientOcclusion->UpdateSkyConstants();
	// UNOFFICIAL 2026-10-31 RT water: the water shaders' TESR_RTWaterData, the effect on or off (off: 0, the planar map).
	if (Effects.Reflections) Effects.Reflections->UpdateWaterConstants();

	// The skin, hair and grass shaders route their light and ambient terms through
	// TESR_PBRData (see Shaders/Includes/PBRScale.hlsl) and render black at a zero scale, so
	// these constants stay current whether or not the PBR collection is enabled.
	PBRShaders* const pbr = Shaders.PBR;
	if (!pbr->Enabled) pbr->UpdateConstants();

	// UNOFFICIAL 2026-10-18 sandport: the grass and leaf shaders read the sandstorm's wind whether or not it is on: off (or not
	// loaded), there is no storm, and they sway exactly as the game's.
	if (!Effects.Sandstorm->Enabled || !Effects.Sandstorm->Effect) Effects.Sandstorm->ClearWind();

	// Constants other effects borrow while their owner is off: the underwater effect reads the water family's,
	// the interior shadows the exterior shadows effect's (they share one settings block).
	WaterShaders* const water = Shaders.Water;
	if (Effects.Underwater->Enabled && !water->Enabled) water->UpdateConstants();
	ShadowsExteriorEffect* const sunShadows = Effects.ShadowsExteriors;
	if (!sunShadows->Enabled && Effects.ShadowsInteriors->Enabled) sunShadows->UpdateConstants();

	TheSettingManager->SettingsChanged = false;	// handled for this frame
	updateClock.LogTime("ShaderManager::UpdateConstants");
}


// Outdoors: between the night and the day value along the day/night curve. Indoors: the interior value.
float ShaderManager::GetTransitionValue(float dayValue, float nightValue, float interiorValue) {
	return GameState.isExterior ? std::lerp(nightValue, dayValue, GameState.transitionCurve) : interiorValue;
}


// True when the first Length bytes of a shader name are the given prefix (the name's own bytes, as before).
static bool ShaderNameBegins(const char* name, const char* prefix, size_t length) {
	return memcmp(name, prefix, length) == 0;
}

ShaderCollection* ShaderManager::GetShaderCollection(const char* shaderName) {

	// First match wins, so the order of these tests decides which collection a shader belongs to.
	if (ShaderNameBegins(shaderName, "WATER", 5)) return Shaders.Water;
	if (ShaderNameBegins(shaderName, "GRASS", 5)) return Shaders.Grass;
	if (ShaderNameBegins(shaderName, "ISHDR", 5) || ShaderNameBegins(shaderName, "HDR", 3)) return Shaders.Tonemapping; // tonemapping shaders have different names between New vegas and Oblivion
	if (ShaderNameBegins(shaderName, "PAR", 3)) return Shaders.POM;
	if (ShaderNameBegins(shaderName, "SKIN", 4)) return Shaders.Skin;
	// Hair (BSSM_3XLIGHTING_*) lives in the SM3 family, not HAIR*. Only SM3003 has a
	// replacement on disk; the rest resolve to no file and fall through to vanilla.
	if (ShaderNameBegins(shaderName, "SM3", 3)) return Shaders.PBR;
	// SpeedTree leaves. STLEAF001/003.vso are vs_3_0 replacements, so every leaf PS must have
	// a ps_3_0 replacement too: D3D9 rejects a 2.x VS paired with a 3.0 PS.
	if (ShaderNameBegins(shaderName, "STLEAF", 6)) return Shaders.PBR;
	if (ShaderNameBegins(shaderName, "SKY", 3)) return Shaders.Sky;
	// UNOFFICIAL lit particles and blood decals (NOLIGHT006/016/017.vso, NOLIGHTTEXVC.pso, GDECAL(S)); 2026-11-05 soft particles: the rest
	// of the NOLIGHT family too (Includes/NoLighting.hlsl), except the skinned NOLIGHT008-015.vso, which stay the game's.
	if (ShaderNameBegins(shaderName, "NOLIGHT", 7) || ShaderNameBegins(shaderName, "GDECAL", 6)) return Shaders.Particles;
	if (strstr(BloodShaders, shaderName)) return Shaders.Blood;	// (a substring of the list)

	// Last, the families that list their shaders in a template table.
	if (Shaders.PBR->GetTemplate(shaderName).Name) return Shaders.PBR;
	if (Shaders.Terrain->GetTemplate(shaderName).Name) return Shaders.Terrain;
	return nullptr;	// none: the game's own shader stays
}

/*
* Every effect disposed and loaded again from its file (or cache).
*/
void ShaderManager::ReloadEffects() {
	for (const auto& entry : EffectsNames) {
		EffectRecord* record = *entry.second;
		record->DisposeEffect();
		record->LoadEffect();
	}
}

// UNOFFICIAL 2026-10-27 (user: still no grass shadows; from the 20:10 launch on, NVR's grass shaders were never loaded and the log said
// nothing): a shader the game creates while ShaderManager::Initialize is still registering the shader sets found its set missing and
// was silently left to the game. Those are kept here and loaded right after the sets exist (LoadEarlyShaders), before anything draws.
static bool ShaderSetsReady = false;
static std::vector<NiD3DVertexShader*> EarlyVertexShaders;
static std::vector<NiD3DPixelShader*> EarlyPixelShaders;

static void LoadEarlyShaders(ShaderManager* Manager) {
	ShaderSetsReady = true;
	if (EarlyVertexShaders.empty() && EarlyPixelShaders.empty()) return;
	Logger::Log("UNOFFICIAL shader sets ready: loading %u vertex and %u pixel shaders the game made before them",
		(unsigned)EarlyVertexShaders.size(), (unsigned)EarlyPixelShaders.size());
	for (NiD3DVertexShader* s : EarlyVertexShaders) Manager->LoadShader(s);
	for (NiD3DPixelShader* s : EarlyPixelShaders) Manager->LoadShader(s);
	EarlyVertexShaders.clear();
	EarlyPixelShaders.clear();
}

// A game shader NVR does not replace (or not yet): no records, switched off.
template <typename ShaderEx> static void LeaveToGame(ShaderEx* shader) {
	for (int variant = ShaderRecordType::Default; variant <= ShaderRecordType::Interior; variant++) shader->ShaderProg[variant] = NULL;
	shader->Enabled = false;
}

template <typename ShaderEx> static bool HasAnyRecord(ShaderEx* shader) {
	return shader->ShaderProg[ShaderRecordType::Default] || shader->ShaderProg[ShaderRecordType::Exterior] ||
		shader->ShaderProg[ShaderRecordType::Interior];
}

/*
* UNOFFICIAL 2026-10-25 (test only, bench warm slot: work\bench\deep-roadmap\multirun\README.md "Warm slot"): every replaced game
* shader of every collection compiled again from its .hlsl (ShaderRecord::LoadShader recompiles a shader whose preprocessed source
* changed, the cache answers the rest), the same way it was loaded at start (LoadShader: templates, interior records). Called at the
* frame's end (NiDX9Renderer__Do_EndFrame), never during a draw. A shader that no longer compiles keeps no record: the game's own draws.
*/
void GameEyesForgetRecords();	// Render.cpp (UNOFFICIAL 2026-11-09 the eye variant)
void ShaderManager::ReloadShaders() {
	auto timer = TimeLogger();
	GameEyesForgetRecords();
	unsigned vs = 0, ps = 0;
	for (const auto& Named : ShaderNames) {
		ShaderCollection* collection = Named.second ? *Named.second : nullptr;
		if (!collection) continue;
		std::vector<NiD3DVertexShaderEx*> vertex; vertex.swap(collection->VertexShaderList);
		std::vector<NiD3DPixelShaderEx*> pixel; pixel.swap(collection->PixelShaderList);
		for (NiD3DVertexShaderEx* s : vertex) { s->DisposeShader(); LoadShader(s); vs++; }
		for (NiD3DPixelShaderEx* s : pixel) { s->DisposeShader(); LoadShader(s); ps++; }
	}
	Logger::Log("RELOAD SHADERS: %u vertex and %u pixel shaders loaded again from their .hlsl", vs, ps);
	RtCore::Restart("reloadshaders: it starts again with the Compute\\Rt .spv files as they are now");	// UNOFFICIAL 2026-11-02 (warm slot RT iterations)
	timer.LogTime("ShaderManager::ReloadShaders");
}

/*
* A game vertex shader's replacements: the generic record and the exterior/interior ones, each where its file exists.
* Returns the family's switch (false when the shader belongs to no family, or the sets are not made yet).
*/
bool ShaderManager::LoadShader(NiD3DVertexShader* Shader) {

	NiD3DVertexShaderEx* vertex = (NiD3DVertexShaderEx*)Shader;
	if (!ShaderSetsReady) {	// (UNOFFICIAL 2026-10-27, see LoadEarlyShaders)
		LeaveToGame(vertex);
		EarlyVertexShaders.push_back(Shader);
		Logger::Log("UNOFFICIAL shader %s made before Vegas Shaders' shader sets: loaded once they exist", vertex->Name ? vertex->Name : "?");
		return false;
	}
	ShaderCollection* collection = GetShaderCollection(vertex->Name);
	if (!collection) { LeaveToGame(vertex); return false; }

	const bool familyOn = collection->Enabled;
	ShaderTemplate tmpl = collection->GetTemplate(vertex->Name);

	// the generic record, then the exterior and interior ones (each may be absent)
	vertex->ShaderProg[ShaderRecordType::Default]  = (ShaderRecordVertex*)ShaderRecord::LoadShader(vertex->Name, NULL, tmpl);
	vertex->ShaderProg[ShaderRecordType::Exterior] = (ShaderRecordVertex*)ShaderRecord::LoadShader(vertex->Name, "Exteriors\\", tmpl);
	vertex->ShaderProg[ShaderRecordType::Interior] = (ShaderRecordVertex*)ShaderRecord::LoadShader(vertex->Name, "Interiors\\", tmpl);
	vertex->Enabled = familyOn;

	if (HasAnyRecord(vertex)) {	// listed in its family only when something replaced it
		collection->VertexShaderList.push_back(vertex);
		Logger::Log("Loaded %s Vertex Shader %s", collection->Name, vertex->Name);
	}
	return familyOn;
}


/*
* The pixel-shader version of the above, plus the UNOFFICIAL lamp-shadow records (interior forward shadows, RT exterior lamps).
*/
bool ShaderManager::LoadShader(NiD3DPixelShader* Shader) {

	NiD3DPixelShaderEx* pixel = (NiD3DPixelShaderEx*)Shader;
	if (!ShaderSetsReady) {	// (UNOFFICIAL 2026-10-27, see LoadEarlyShaders)
		LeaveToGame(pixel);
		EarlyPixelShaders.push_back(Shader);
		Logger::Log("UNOFFICIAL shader %s made before Vegas Shaders' shader sets: loaded once they exist", pixel->Name ? pixel->Name : "?");
		return false;
	}
	ShaderCollection* family = GetShaderCollection(pixel->Name);
	if (!family) { LeaveToGame(pixel); return false; }

	const bool familyOn = family->Enabled;
	ShaderTemplate tmpl = family->GetTemplate(pixel->Name);

	// generic, exterior, interior: each where its file exists
	static const char* const variantFolders[3] = { NULL, "Exteriors\\", "Interiors\\" };
	for (int variant = ShaderRecordType::Default; variant <= ShaderRecordType::Interior; variant++)
		pixel->ShaderProg[variant] = (ShaderRecordPixel*)ShaderRecord::LoadShader(pixel->Name, variantFolders[variant], tmpl);
	// UNOFFICIAL interior forward point-light shadows (PointShadowForward.h): the lit pixel shaders get an Interior record
	// from their own source with INTERIOR_SHADOWS, the sun's code compiled out (it never runs indoors). Only with the
	// setting on at startup; exteriors keep their records.
	if (!pixel->ShaderProg[ShaderRecordType::Interior] && pixel->ShaderProg[ShaderRecordType::Default] &&
		PointShadowForward::Eligible(pixel->Name, tmpl) &&
		TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled")) {
		// UNOFFICIAL 2026-10-20 (P82 tester): when the GPU refuses a record (too long), shorter ones in turn (PointShadowForward.hlsl
		// POINT_SHADOW_FIT, each with its own cache folder; the first is the old one exactly, also what ShaderPrecompile makes), and
		// PointShadowForward::FitLevel / Refused keep every object to what the shortest one loaded can do.
		static D3DXMACRO overrides[4][7] = {
			{ { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" }, { "CONTACT_HARDENING", "0" }, { NULL, NULL } },
			{ { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" }, { "CONTACT_HARDENING", "0" }, { "POINT_SHADOW_FIT", "1" }, { NULL, NULL } },
			{ { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" }, { "CONTACT_HARDENING", "0" }, { "POINT_SHADOW_FIT", "2" }, { NULL, NULL } },
			{ { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" }, { "CONTACT_HARDENING", "0" }, { "POINT_SHADOW_FIT", "3" }, { NULL, NULL } } };
		// UNOFFICIAL 2026-10-21 LampFalloff (Includes/LampFalloff.hlsl): these records draw indoors only, so without [Shaders.PBR.LampFalloff]
		// Interior they get no lamp shape (LAMP_FALLOFF_ON 1 = MatchEverywhere only, 0 = nothing): the six-lamp shaders stay as long as they
		// were (SLS2035: 3592 slots with the user's settings; the shape takes it to ~3700, past what the GPU takes, and the next level).
		static const bool lampFalloffOverride = [] {
			const char* value = PBRShaders::LampFalloffInteriorDefine();
			if (value) for (auto& level : overrides) for (auto& d : level) if (!d.Name) { d = { "LAMP_FALLOFF_ON", value }; break; }
			return value != nullptr;
		}();
		(void)lampFalloffOverride;
		static const char* const cache[4] = { "Interiors\\", "Interiors\\Fit1\\", "Interiors\\Fit2\\", "Interiors\\Fit3\\" };
		static const char* const what[4] = { "", "the High/Ultra taps in a loop (the same shadows)",
			"no High/Ultra: lamp shadow Quality is held at Medium for every object", "also no blocker search: contact hardening and LampSize are off for every object's lamp shadows" };
		ShaderRecordPixel* record = nullptr;
		int level = 0;
		for (; level < 4 && !record; level++) {
			if (level) Logger::Log("UNOFFICIAL lamp shadows: %s with lamp shadows did not load; trying a shorter version (%d of 3: %s).", pixel->Name, level, what[level]);
			record = (ShaderRecordPixel*)ShaderRecord::LoadShader(pixel->Name, NULL, tmpl, cache[level], overrides[level]);
		}
		if (record) {
			if (--level) {
				Logger::Log("UNOFFICIAL lamp shadows: %s with lamp shadows loaded at the shorter version %d (%s).", pixel->Name, level, what[level]);
				if (level > PointShadowForward::FitLevel) PointShadowForward::FitLevel = level;
				EffectStatus::Level((std::string(pixel->Name) + " lamp shadows").c_str(), what[level], false);	// UNOFFICIAL 2026-11-01 (REPORT / FLASH dumps)
			}
			record->PointShadowForward = true;
			pixel->ShaderProg[ShaderRecordType::Interior] = record;
			if (!level) DeferredLamps::LoadRecord(pixel, pixel->Name, tmpl, overrides[0], record);	// UNOFFICIAL 2026-10-31 DeferredLamps (the many-lamp family's deferred record)
			PointShadowForward::CompiledIn = true;
			PointShadowForward::CompiledCount++;
		}
		else {
			Logger::Log("[ERROR] UNOFFICIAL lamp shadows: no version of %s with lamp shadows loads on this GPU. Forward lamp shadows are off: every object "
				"uses the screen-space lamp shadows, as with [Shaders.ShadowsInteriors.Forward] Enabled off (the objects drawn with it would otherwise lose "
				"their lamp shadows whenever enough lamps reach them).", pixel->Name);
			PointShadowForward::Refused = true;
			EffectStatus::Level((std::string(pixel->Name) + " lamp shadows").c_str(), "no version loads: forward lamp shadows OFF", false);	// UNOFFICIAL 2026-11-01
		}
	}
	// UNOFFICIAL 2026-10-30 RT lamp shadows ExteriorForward: the same lit shaders get an outdoor record too, from the shader the game uses
	// outdoors (the Exterior record's folder, else the default) with the lamps' traced shadows added (INTERIOR_SHADOWS + POINT_SHADOW_RT_ONLY:
	// no cube-map code, the sun's code kept). Only with the RT lamp shadows and the switch on at startup; a refused one leaves that shader
	// without outdoor lamp shadows (logged), the others keep theirs.
	if (pixel->ShaderProg[ShaderRecordType::Interior] && pixel->ShaderProg[ShaderRecordType::Interior]->PointShadowForward &&
		PointShadowForward::Eligible(pixel->Name, tmpl) &&
		TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "Enabled") && TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "ExteriorForward")) {
		// levels: every lamp; then (the skin shaders, SkinTemplate SKIN_PT_LAMPS) the first 2, the first 1 (SKIN2011 ran out of temp registers)
		static D3DXMACRO exteriorLamps[3][4] = {
			{ { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { NULL, NULL } },
			{ { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { "SKIN_PT_LAMPS", "2" }, { NULL, NULL } },
			{ { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { "SKIN_PT_LAMPS", "1" }, { NULL, NULL } } };
		static const char* const exteriorCache[3] = { "ExteriorLamps\\", "ExteriorLamps\\Fit1\\", "ExteriorLamps\\Fit2\\" };
		const bool customExterior = pixel->ShaderProg[ShaderRecordType::Exterior] != nullptr;
		const bool skin = tmpl.Name && strstr(tmpl.Name, "SkinTemplate") == tmpl.Name;
		ShaderRecordPixel* record = nullptr;
		int exteriorLevel = 0;
		// UNOFFICIAL 2026-11-09: the skin's many-lamp variants with 3+ lamps (SKIN2010 POINTS 4, SKIN2011 POINTS 3) never fit with every
		// lamp shadowed outdoors (X4505, 32 temps, at both compile flags; offline template_compile with the user's toml). A failed compile
		// is not cached, so trying them cost ~13 s at EVERY start (two shaders x the flow-control retry). They start at the 2-lamp level,
		// which is what they always ended up with.
		// (MANY only: SKIN2009, DIFFUSE with 3 lamps, fits with every lamp)
		if (skin) {
			bool many = false; int points = 0;
			for (const D3DXMACRO& d : tmpl.Defines) {
				if (d.Name && !strcmp(d.Name, "MANY")) many = true;
				if (d.Name && !strcmp(d.Name, "POINTS") && d.Definition) points = atoi(d.Definition);
			}
			if (many && points >= 3) exteriorLevel = 1;
		}
		// 2026-11-11 Puddles: a record that did not fit with the water layer is never cached, so trying it again cost ~11 s at EVERY start
		// (SLS2027, the flow-control retry). Its dry copy in Cache\ExteriorLampsDry, newer than any fitted copy in Cache\ExteriorLamps, says it did not
		// fit last time: straight to the dry one. (Delete Cache\ExteriorLampsDry to try the water layer again after a template change.)
		const bool wetAll = !skin && TheSettingManager->GetSettingI("Shaders.WetnessEffects.Status", "Enabled");
		if (wetAll) {
			const std::string cache = std::string(ShadersPath) + "Cache\\";
			// (2026-11-11 second try: a fitted copy from before the water layer existed stays in Cache\ExteriorLamps, so "no fitted copy" never
			// held; a dry copy NEWER than the fitted one says the last try failed.)
			WIN32_FILE_ATTRIBUTE_DATA dry = {}, wet = {};
			const bool hasDry = GetFileAttributesExA((cache + "ExteriorLampsDry\\" + pixel->Name).c_str(), GetFileExInfoStandard, &dry) != 0;
			const bool hasWet = GetFileAttributesExA((cache + "ExteriorLamps\\" + pixel->Name).c_str(), GetFileExInfoStandard, &wet) != 0;
			if (hasDry && (!hasWet || CompareFileTime(&dry.ftLastWriteTime, &wet.ftLastWriteTime) > 0)) exteriorLevel = 1;	// the loop below is skipped
		}
		for (; exteriorLevel < (skin ? 3 : 1) && !record; exteriorLevel++)
			record = (ShaderRecordPixel*)ShaderRecord::LoadShader(pixel->Name, customExterior ? "Exteriors\\" : NULL, tmpl,
				exteriorCache[exteriorLevel], exteriorLamps[exteriorLevel]);
		// 2026-11-09 Puddles / Wetness Effects: the outdoor record carries the water layer too (ObjectTemplate / ParallaxTemplate with
		// POINT_SHADOW_RT_ONLY). Where that does not fit (SLS2027 in game: X4505, though it fits offline), it is made again without the water
		// layer (WETNESS_OFF) rather than losing the lamp shadows: no wetness on that shader near lamps, the shadows kept.
		if (!record && wetAll) {
			static D3DXMACRO exteriorLampsDry[4] = { { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { "WETNESS_OFF", "1" }, { NULL, NULL } };
			record = (ShaderRecordPixel*)ShaderRecord::LoadShader(pixel->Name, customExterior ? "Exteriors\\" : NULL, tmpl, "ExteriorLampsDry\\", exteriorLampsDry);
			if (record) Logger::Log("UNOFFICIAL RT lamp shadows: %s's outdoor lamp-shadow record did not fit with the water layer: made without it "
				"(the lamp shadows kept; no Wetness Effects / Puddles on it near lamps).", pixel->Name);
		}
		if (record && exteriorLevel > 1)
			Logger::Log("UNOFFICIAL RT lamp shadows: %s's outdoor lamp-shadow record shadows only the first %d lamp(s) of a draw (the full one did not fit).",
				pixel->Name, exteriorLevel == 2 ? 2 : 1);
		if (record) {
			record->PointShadowForward = true;
			PointShadowForward::ExteriorRecords[pixel] = record;
			Logger::Log("UNOFFICIAL RT lamp shadows: %s has an outdoor lamp-shadow record (ExteriorForward).", pixel->Name);
		}
		else Logger::Log("[ERROR] UNOFFICIAL RT lamp shadows: %s with outdoor lamp shadows did not load (ExteriorForward): it stays without them outdoors.", pixel->Name);
	}
	// UNOFFICIAL 2026-10-30 RT lamp shadows ExteriorForward on the land: the land shaders with lamps (TerrainTemplate, NUM_PT_LIGHTS) get an
	// outdoor record with TERRAIN_LAMP_RT (their lamps matched by position to the traced entries). Only with terrain blending off: its
	// depth takes s15, the one stage the land has left (register audit 2026-10-30).
	if (pixel->ShaderProg[ShaderRecordType::Default] && tmpl.Name && !strcmp(tmpl.Name, "TerrainTemplate") &&
		TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "Enabled") && TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "ExteriorForward")) {
		bool lamps = false;
		for (const D3DXMACRO& d : tmpl.Defines) if (d.Name && !strcmp(d.Name, "NUM_PT_LIGHTS")) lamps = true;
		if (lamps && TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Enabled")) {
			static bool told = false;
			if (!told) { told = true; Logger::Log("UNOFFICIAL RT lamp shadows: the land gets no outdoor lamp shadows: Terrain Blending is on (it takes s15, the land's last free stage)."); }
		}
		else if (lamps) {
			static D3DXMACRO terrainLamps[2] = { { "TERRAIN_LAMP_RT", "1" }, { NULL, NULL } };
			ShaderRecordPixel* record = (ShaderRecordPixel*)ShaderRecord::LoadShader(pixel->Name, NULL, tmpl, "ExteriorLamps\\", terrainLamps);
			// 2026-11-09 Puddles: the land's lamp record carries the water layer on its own registers (TerrainTemplate WETNESS_ALT_REGS); if that
			// does not fit, the record is made again without it rather than losing the lamp shadows (as the objects' above).
			if (!record && TheSettingManager->GetSettingI("Shaders.WetnessEffects.Status", "Enabled")) {
				static D3DXMACRO terrainLampsDry[3] = { { "TERRAIN_LAMP_RT", "1" }, { "WETNESS_OFF", "1" }, { NULL, NULL } };
				record = (ShaderRecordPixel*)ShaderRecord::LoadShader(pixel->Name, NULL, tmpl, "ExteriorLampsDry\\", terrainLampsDry);
				if (record) Logger::Log("UNOFFICIAL RT lamp shadows: %s (land)'s outdoor lamp-shadow record did not fit with the water layer: made without it.", pixel->Name);
			}
			if (record) {
				PointShadowForward::ExteriorRecords[pixel] = record;
				PointShadowForward::TerrainRecords.insert(pixel);
				Logger::Log("UNOFFICIAL RT lamp shadows: %s (land) has an outdoor lamp-shadow record (ExteriorForward).", pixel->Name);
			}
			else Logger::Log("[ERROR] UNOFFICIAL RT lamp shadows: %s (land) with outdoor lamp shadows did not load (ExteriorForward).", pixel->Name);
		}
	}
	pixel->Enabled = familyOn;

	if (HasAnyRecord(pixel)) {
		family->PixelShaderList.push_back(pixel);
		Logger::Log("Loaded %s Pixel Shader %s", family->Name, pixel->Name);
	}
	return familyOn;
}


void ShaderManager::GetNearbyLights(ShadowSceneLight* ShadowLightsList[], NiPointLight* LightsList[], NiSpotLight* SpotLightList[]) {
	const D3DXVECTOR4 playerAt = Player->pos.toD3DXVEC4();
	TimeLogger lightsClock;

	// create a map of all nearby valid lights and sort them per distance to player
	std::multimap<float, ShadowSceneLight*> rankedLamps;	// UNOFFICIAL: a multimap (two lamps with the same key used to drop one)
	NiTList<ShadowSceneLight>::Entry* node = SceneNode->lights.start;
	ShadowPool::BeginRanking();	// UNOFFICIAL 2026-10-29 ShadowPool: the shadow-casting lamps the live slots have no room for, in order

	ShadowsExteriorEffect* const shadowsEffect = Effects.ShadowsExteriors;
	ShadowsExteriorEffect::InteriorsStruct* lampCfg = &shadowsEffect->Settings.Interiors;	// the lamp-shadow settings
	ShadowsExteriorEffect::ShadowStruct* shadowConsts = &shadowsEffect->Constants;

	// The lights given cube-map slots last frame (filled at the end). UNOFFICIAL, with forward shadows on
	// ([Shaders.ShadowsInteriors.Forward] LampSwitchMargin): such a lamp keeps its shadow unless another is clearly nearer,
	// and stays while somewhat behind you. Lamps at the edge of the LightPoints cap or of the "in front" test otherwise
	// switch in and out as you move and turn, and their shadows pop on and off.
	static const void* previousSlots[ShadowCubeMapsMax] = {};
	const float switchMargin = lampCfg->Forward.Enabled ? lampCfg->Forward.LampSwitchMargin : 0.0f;	// (read on change, see ShadowsExteriorEffect::UpdateSettings)
	auto wasCaster = [&](const ShadowSceneLight* light) {
		if (!(switchMargin > 0.0f)) return false;
		for (int s = 0; s < ShadowCubeMapsMax; s++) if (previousSlots[s] == light) return true;
		return false;
	};

	// UNOFFICIAL, with forward shadows on: a lamp counts when its light reaches what the camera sees -- its sphere against
	// the view frustum's four sides and the plane through the eye (not the far plane), from the camera. The old test, "in
	// front of the player", changed as you turned and dropped lamps beside or behind you whose light (and shadows) fell
	// on what you look at: their shadows came and went with the view angle.
	const bool viewTest = lampCfg->Forward.Enabled;
	// UNOFFICIAL CameraIndependent (2026-10-08, with forward shadows on): no view test at all -- every lamp in range competes
	// by its rank round the player. A busy room has far more lamps than cube maps (the Tops: 50-70 for 11; LogLamps showed
	// most swaps were 'its light does not reach the view'), so with the view test turning the camera swapped lamps in and
	// out constantly and their shadows popped at the edges of the screen. Now only walking changes them (LampSwitchMargin).
	const bool cameraIndependent = viewTest && lampCfg->Forward.CameraIndependent;
	// UNOFFICIAL FillLightRadius (2026-10-08, with forward shadows on): a lamp reaching farther than this is a level's big fill
	// light (the Tops: reach 500-980 against 180-420 for its real lamps, often dimmer x 1.25-1.9), hidden up in the ceiling to
	// brighten a whole area. Its cube map saw the ceiling round it, so with a shadow all its light was taken off every floor
	// piece it lights (dark rectangles along the floor's mesh pieces); it also took cube maps from real lamps (its rank is
	// high). It still lights as before, without a shadow, and still counts as a tracked lamp for the darkening.
	const float fillLightRadius = viewTest ? lampCfg->Forward.FillLightRadius : 0.0f;
	const D3DXMATRIX& camera = TheRenderManager->InvViewMatrix;
	const D3DXVECTOR3 camRight(camera._11, camera._12, camera._13), camUp(camera._21, camera._22, camera._23), camForward(camera._31, camera._32, camera._33);
	const float tanX = fabsf(TheRenderManager->InvProjMatrix._11), tanY = fabsf(TheRenderManager->InvProjMatrix._22);
	const float normX = 1.0f / sqrtf(1.0f + tanX * tanX), normY = 1.0f / sqrtf(1.0f + tanY * tanY);
	// UNOFFICIAL LampRanking 1 (with forward shadows on): lamps ranked by what they light of the scene -- brightness x how far
	// their light reaches / distance -- instead of nearest first. Many small bulbs near you used to push a big room lamp
	// out of the LightPoints cap as you strafed, and its shadows vanished.
	const int lampRanking = viewTest ? lampCfg->Forward.LampRanking : 0;
	// UNOFFICIAL LogLamps: every lamp that gains or loses its shadow is logged with the reason (diagnostic).
	const bool logLamps = lampCfg->Forward.LogLamps;
	struct LampNote { const ShadowSceneLight* light; int reason; float distance, key, radius; D3DXVECTOR4 position, colour; int carry; };
	static LampNote notes[128];
	int noteCount = 0;
	auto note = [&](const ShadowSceneLight* light, int reason, float distance, float key, NiPointLight* source) {
		if (!logLamps || noteCount >= 128 || !source) return;
		notes[noteCount++] = { light, reason, distance, key, source->Spec.r, source->m_worldTransform.pos.toD3DXVEC4(),
			D3DXVECTOR4(source->Diff.r, source->Diff.g, source->Diff.b, source->Dimmer), (int)source->CanCarry };
	};
	// UNOFFICIAL InvisibleLightShadowStrength (2026-10-20, with forward shadows on, indoors): a light with no glowing lamp
	// within FixtureDistance (LampFixtures.h: the level designers' invisible fill and key lights, most of New Vegas's interior
	// lights) casts that share of a shadow; at 0 none, and it leaves its cube map to lamps that are really there (it still
	// lights as before and still counts for the darkening, as a fill light does). LogLamps lists every light's answer once.
	const bool indoorsForward = viewTest && !GameState.isExterior;
	const float invisibleStrength = indoorsForward ? lampCfg->Forward.InvisibleLightShadowStrength : 1.0f;
	const bool classifyLamps = indoorsForward && (invisibleStrength < 1.0f || lampCfg->Forward.InvisibleLightSize > 0.0f ||
		!lampCfg->Forward.LampModelShadows || lampCfg->Forward.InvisibleShadowFromLamp > 0.0f || !LampFixtures::NoShadowWords.empty() || logLamps ||
		(lampCfg->Forward.ShadowClusterEnabled && lampCfg->Forward.ShadowClusterInvisibleOnly));	// (they need the cell's lamps listed)
	if (classifyLamps) LampFixtures::Update(Player->parentCell, logLamps);
	LampFixtures::LogModels = logLamps;

	auto reachesView = [&](const D3DXVECTOR4& lightPosition, float reach) {
		const D3DXVECTOR3 v(lightPosition.x - TheRenderManager->CameraPosition.x, lightPosition.y - TheRenderManager->CameraPosition.y,
			lightPosition.z - TheRenderManager->CameraPosition.z);
		const float x = D3DXVec3Dot(&v, &camRight), y = D3DXVec3Dot(&v, &camUp), z = D3DXVec3Dot(&v, &camForward);
		if (z < -reach) return false;
		if ((tanX * z - x) * normX < -reach || (tanX * z + x) * normX < -reach) return false;
		if ((tanY * z - y) * normY < -reach || (tanY * z + y) * normY < -reach) return false;
		return true;
	};

	// UNOFFICIAL 2026-11-01 FadeOut ([Shaders.ShadowsInteriors.Forward] FadeOut, see the slot assignment below): the lamps a fading slot may
	// keep (only these: a lamp gone from the game's list may be freed, a culled or switched-off one lights nothing).
	static std::vector<const void*> aliveLamps;
	aliveLamps.clear();
	const bool collectAlive = indoorsForward && lampCfg->Forward.FadeOut > 0.0f;

	// Creating list of lights in order of distance to the player
	while (node) {
		// UNOFFICIAL 2026-10-16 (bug hunt): an entry without a light (the loop below checks the same) was dereferenced.
		if (!node->data || !node->data->sourceLight) { node = node->next; continue; }
		if (FlashlightLamp::Is(node->data->sourceLight)) { node = node->next; continue; }	// UNOFFICIAL 2026-11-04 the flashlight's game lamp (core/FlashlightLamp.h)
		NiPointLight* lamp = node->data->sourceLight;
		D3DXVECTOR4 lampAt = lamp->m_worldTransform.pos.toD3DXVEC4();

		// Culled, or too dim to matter (lights placed with a near-black colour): skipped.
		const bool isCulled = lamp->m_flags & NiAVObject::NiFlags::APP_CULLED;
		const bool isLit = (lamp->Diff.r + lamp->Diff.g + lamp->Diff.b) * lamp->Dimmer > 5.0 / 255.0;
		if (isCulled || !isLit) {
			if (GpuTimer::Enabled && ShadowHold::Continuous(node->data)) (isCulled ? ShadowHold::StatCulled : ShadowHold::StatDimmed)++;	// UNOFFICIAL 2026-10-30
			note(node->data, isCulled ? 1 : 2, lamp->GetDistance(&Player->pos), 0.0f, lamp);
			node = node->next;
			continue;
		}
		if (collectAlive) aliveLamps.push_back(node->data);	// UNOFFICIAL 2026-11-01 FadeOut: in the game's list, lit and not culled

		D3DXVECTOR4 toLamp = lampAt - playerAt;
		D3DXVec4Normalize(&toLamp, &toLamp);
		const bool incumbent = wasCaster(node->data);
		// (A lamp that had a shadow last frame keeps counting a little beyond the edge of the view: LampSwitchMargin.)
		bool inFront = cameraIndependent ||
			(viewTest ? reachesView(lampAt, lamp->Spec.r * lampCfg->LightRadiusMult * (incumbent ? 1.0f + switchMargin : 1.0f))
			: D3DXVec4Dot(&toLamp, &TheRenderManager->CameraForward) > (incumbent ? -0.5f : 0.0f));
		float lampDistance = lamp->GetDistance(&Player->pos);
		float lampReach = lamp->Spec.r * lampCfg->LightRadiusMult;

		// Tracked: in front (or the player inside its reach), and the whole reach within 8000 units.
		const float trackRange = 8000;
		if ((inFront || lampDistance < lampReach) && (lampDistance + lampReach) < trackRange) {
			float key = lampDistance * (incumbent ? 1.0f - switchMargin : 1.0f);
			if (lampRanking) {
				const float brightness = (std::max)(0.0f, (lamp->Diff.r + lamp->Diff.g + lamp->Diff.b) / 3.0f * lamp->Dimmer);
				key = -brightness * lamp->Spec.r / (std::max)(lampDistance, 0.25f * lamp->Spec.r + 1.0f) * (incumbent ? 1.0f + switchMargin : 1.0f);
			}
			rankedLamps.insert({ key, node->data });
			note(node->data, 0, lampDistance, key, lamp);
		}
		else note(node->data, inFront ? 4 : 3, lampDistance, 0.0f, lamp);

		node = node->next;
	}

	// UNOFFICIAL 2026-10-30 VisibleRanking ([Shaders.ShadowsInteriors.Forward] VisibleRanking, core/LampVisibility.h): indoors with forward
	// shadows, the lamps ranked by brightness x how much of the VISIBLE picture they light (walls hide what is behind them), measured a few
	// frames back; lamps not measured yet follow in the old order, lamps that light nothing visible go last. LampSwitchMargin favours the
	// slot holders as before; ShadowHold above 0 keeps a slot holder first for that many seconds after it last ranked inside the slots, while
	// it still lights something visible. Off: the ranking above, unchanged.
	const int visibleRanking = indoorsForward && !LampVisibility::Failed ? LampVisibility::Setting() : 0;
	LampVisibility::On = visibleRanking != 0;
	ShadowHold::VisiblePixels = LampVisibility::On ? &LampVisibility::Pixels : nullptr;
	if (LampVisibility::On) {
		std::vector<LampVisibility::Want> wants;
		wants.reserve(rankedLamps.size());
		for (const auto& e : rankedLamps) {
			NiPointLight* L = e.second->sourceLight;
			if (L) wants.push_back({ e.second, D3DXVECTOR3(L->m_worldTransform.pos.x, L->m_worldTransform.pos.y, L->m_worldTransform.pos.z), L->Spec.r * lampCfg->LightRadiusMult });
		}
		LampVisibility::Request(wants);
	}
	if (visibleRanking == 1) {
		auto inSlots = [&](const void* light) { for (int s = 0; s < ShadowCubeMapsMax; s++) if (previousSlots[s] == light) return true; return false; };
		std::vector<std::pair<float, ShadowSceneLight*>> keyed;
		keyed.reserve(rankedLamps.size());
		for (const auto& e : rankedLamps) {
			NiPointLight* L = e.second->sourceLight;
			const float pixels = LampVisibility::Pixels(e.second);
			const float brightness = L ? (std::max)(0.0f, (L->Diff.r + L->Diff.g + L->Diff.b) / 3.0f * L->Dimmer) : 0.0f;
			float key;
			if (pixels > 0.5f) key = -brightness * pixels * (switchMargin > 0.0f && inSlots(e.second) ? 1.0f + switchMargin : 1.0f);
			else if (pixels < 0.0f) key = 1.0e6f + e.first;	// not measured yet: after every lamp lighting something visible, in the old order
			else key = 2.0e6f + e.first;					// lights nothing visible
			keyed.push_back({ key, e.second });
		}
		std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		const float hold = ShadowHold::Seconds();
		if (hold > 0.0f) {
			const double now = ShadowHold::Now();
			const int inside = (std::max)(1, lampCfg->LightPoints);
			for (size_t k = 0; k < keyed.size() && (int)k < inside; k++) LampVisibility::Lamps[keyed[k].second].lastInside = now;
			for (auto& k : keyed)
				if (inSlots(k.second) && LampVisibility::Pixels(k.second) != 0.0f && now - LampVisibility::Lamps[k.second].lastInside <= hold) {
					if (GpuTimer::Enabled && k.first > -1.0e7f) ShadowHold::StatLiveHeld++;
					k.first -= 1.0e7f;
				}
			std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		}
		rankedLamps.clear();
		for (const auto& k : keyed) rankedLamps.insert(rankedLamps.end(), { k.first, k.second });
	}

	// Last frame's lamps cleared: positions of the tracked list (TrackedLightsMax of them, nearest first) ...
	memset(TheShaderManager->LightPosition, 0, sizeof(D3DXVECTOR4) * TrackedLightsMax);
	// Must be cleared. The fill loop below only zeroes trailing slots once it runs out of scene
	// lights; with more lights than slots it never reaches that branch, and a slot left holding
	// last frame's position keeps GetPointLightAmount sampling a cubemap nobody redraws.
	memset(&shadowConsts->ShadowLightPosition, 0, ShadowCubeMapsScreen * sizeof(D3DXVECTOR4));
	memset(TheShaderManager->LightColor, 0, sizeof(D3DXVECTOR4) * (TrackedLightsMax + ShadowCubeMapsScreen));	// ... and every colour
	memset(ExtraLights, 0, sizeof(ExtraLights));	// UNOFFICIAL: the mask's extra lamps, filled below with forward shadows

	// ShadowManager::RenderShadowMaps only renders cubemaps for the first LightPoints slots.
	// Filling past that gives the shader a live position and colour for a face that is never
	// redrawn, so it samples whatever that cubemap last held -- a shadow frozen from an earlier
	// frame or cell. Lights beyond the cap fall through to the non-shadowing tracked list.
	// (UNOFFICIAL: with forward shadows indoors, up to ShadowCubeMapsMax -- PointShadowForward::ShadowedSlots)
	const bool allSlots = PointShadowForward::UsesAllSlots(TheShaderManager->GameState.isExterior);
	// UNOFFICIAL ExteriorLightPoints: outdoors above 12, that many lamps get shadows, every slot sampled (in batches).
	const bool batched = PointShadowForward::ExteriorBatched(TheShaderManager->GameState.isExterior);
	const int ShadowLightsMax = batched ? PointShadowForward::ShadowedSlots(true) : min(lampCfg->LightPoints, allSlots ? (int)ShadowCubeMapsMax : (int)ShadowCubeMapsScreen);
	memset(BatchLightPosition, 0, sizeof(BatchLightPosition));
	memset(BatchLightColor, 0, sizeof(BatchLightColor));

	// Fill counters: shadow slots and tracked (unshadowed) slots used so far.
	int shadowSlot = 0, trackedSlot = 0;
	TheShadowManager->PointLightsNum = 0;
	ShadowSceneLight* ShadowCasters[ShadowCubeMapsMax] = { NULL }; // shadow casting lights, nearest first
	const D3DXVECTOR4 noLight(0, 0, 0, 0);

	// The only spot light is the player's flashlight, which the engine does not list: FlashlightEffect::UpdateConstants publishes
	// its SpotLightPosition/Direction/Color (one read of the bone for the cone and the cookie matrix), and it is the spot list.
	FlashlightEffect* const torch = TheShaderManager->Effects.Flashlight;
	torch->UpdateConstants();
	SpotLightList[0] = (torch->Enabled && torch->spotLightActive) ? torch->SpotLight : nullptr;

	// UNOFFICIAL 2026-10-26 ShadowClusterEnabled ([Shaders.ShadowsInteriors.Forward]): indoors, lamps closer together than
	// ShadowClusterDistance share one shadow. The brightest lamp of each group keeps its shadow; the others light exactly as
	// before but without a shadow of their own, so a room of many bulbs no longer runs out of shadow slots. Off: as before.
	std::vector<const void*> clusterMembers;
	if (lampCfg->Forward.ShadowClusterEnabled && !TheShaderManager->GameState.isExterior && lampCfg->Forward.ShadowClusterDistance > 0.0f) {
		struct Cand { const void* key; NiPointLight* light; float brightness; };
		std::vector<Cand> cands;
		for (auto& e : rankedLamps) {
			NiPointLight* L = e.second->sourceLight;
			if (!L || L->EffectType != NiDynamicEffect::EffectTypes::POINT_LIGHT || L->Spec.r * lampCfg->LightRadiusMult <= 10) continue;
			// A lamp switched off for the main view (+0x110 = 0xFF: the game's view test, or FNV-Occlusion-Culling's bLampCulling
			// when its whole reach is covered) is never a group's shadow: the next-brightest lit lamp near it takes the group.
			if (e.second->bIsEnabled == 0xFF) continue;
			cands.push_back({ e.second, L, (L->Diff.r + L->Diff.g + L->Diff.b) * L->Dimmer });
		}
		std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.brightness > b.brightness; });
		const float d2max = lampCfg->Forward.ShadowClusterDistance * lampCfg->Forward.ShadowClusterDistance;
		// UNOFFICIAL 2026-10-29 (user: "not all invisible lights just ones where there's a lot in one place. vault 21 like 900 lights"):
		// ShadowClusterInvisibleOnly: a lamp that is really there (a glowing part within FixtureDistance, LampFixtures) keeps its own
		// shadow; only the level designers' invisible fill lights join a neighbour's. ShadowClusterMinLights: and only where at least that
		// many lights (itself included) stand within ShadowClusterDistance, so rooms with a few lights keep every shadow. 0 / off: as before.
		const bool invisibleOnly = lampCfg->Forward.ShadowClusterInvisibleOnly;
		const int minLights = lampCfg->Forward.ShadowClusterMinLights;
		std::vector<bool> mayJoin(cands.size(), true);
		for (size_t m = 0; m < cands.size(); m++) {
			if (invisibleOnly && !LampFixtures::Invisible(cands[m].light, lampCfg->Forward.FixtureDistance)) { mayJoin[m] = false; continue; }
			if (minLights > 1) {
				const NiPoint3& mp = cands[m].light->m_worldTransform.pos;
				int neighbours = 0;
				for (size_t o = 0; o < cands.size() && neighbours < minLights; o++) {
					const NiPoint3& op = cands[o].light->m_worldTransform.pos;
					const float dx = op.x - mp.x, dy = op.y - mp.y, dz = op.z - mp.z;
					if (dx * dx + dy * dy + dz * dz < d2max) neighbours++;
				}
				if (neighbours < minLights) mayJoin[m] = false;
			}
		}
		std::vector<bool> taken(cands.size(), false);
		for (size_t r = 0; r < cands.size(); r++) {
			if (taken[r]) continue;	// r is a representative: it takes every untaken lamp near it
			const NiPoint3& rp = cands[r].light->m_worldTransform.pos;
			for (size_t m = r + 1; m < cands.size(); m++) {
				if (taken[m] || !mayJoin[m]) continue;
				const NiPoint3& mp = cands[m].light->m_worldTransform.pos;
				const float dx = mp.x - rp.x, dy = mp.y - rp.y, dz = mp.z - rp.z;
				if (dx * dx + dy * dy + dz * dz < d2max) { taken[m] = true; clusterMembers.push_back(cands[m].key); }
			}
		}
		static int logCountdown = 0;
		if (lampCfg->Forward.LogLamps && --logCountdown <= 0) {
			logCountdown = 300;
			Logger::Log("ShadowCluster: %u candidate lamps, %u share a brighter neighbour's shadow (%u groups), distance %.0f",
				(unsigned)cands.size(), (unsigned)clusterMembers.size(), (unsigned)(cands.size() - clusterMembers.size()), lampCfg->Forward.ShadowClusterDistance);
		}
	}

	auto v = rankedLamps.begin();
	const int slotVisits = TrackedLightsMax + ShadowCubeMapsMax;
	for (int i = 0; i < slotVisits; i++) {
		// Out of lamps: the rest of the tracked list is emptied (unused shadow slots stay cleared: they are assigned after this loop).
		if (v == rankedLamps.end()) {
			if (trackedSlot < TrackedLightsMax) {
				LightsList[trackedSlot] = NULL;
				LightPosition[trackedSlot] = LightColor[ShadowCubeMapsScreen + trackedSlot] = noLight;
				trackedSlot++;
			}
			continue;
		}

		NiPointLight* lamp = v->second->sourceLight;
		if (!lamp) {
			v++;
			continue;
		}

		if (lamp->EffectType == NiDynamicEffect::EffectTypes::POINT_LIGHT) {
			// Every lamp may cast (the plugin's cast-shadow flag is not read: JIP breaks it), unless one of the rules below says not.
			bool castsShadow = true;
			if (fillLightRadius > 0.0f && lamp->Spec.r > fillLightRadius) {	// UNOFFICIAL FillLightRadius, see above
				// (FillLightShadowStrength above 0: it keeps a shadow, that much of a full one: PointShadowForward::Publish)
				castsShadow = lampCfg->Forward.FillLightShadowStrength > 0.0f;
				static const void* loggedFill[64] = {};
				static int loggedFillCount = 0;
				if (logLamps && loggedFillCount < 64) {
					bool seen = false;
					for (int f = 0; f < loggedFillCount; f++) if (loggedFill[f] == v->second) seen = true;
					if (!seen) {
						loggedFill[loggedFillCount++] = v->second;
						const D3DXVECTOR4 at = lamp->m_worldTransform.pos.toD3DXVEC4();
						Logger::Log("LAMP %p is a fill light (reach %.0f > FillLightRadius %.0f): it lights as before, its shadow at strength %.2f (0: none) | at %.0f %.0f %.0f, colour %.2f %.2f %.2f x %.2f",
							v->second, lamp->Spec.r, fillLightRadius, lampCfg->Forward.FillLightShadowStrength, at.x, at.y, at.z, lamp->Diff.r, lamp->Diff.g, lamp->Diff.b, lamp->Dimmer);
					}
				}
			}

			if (classifyLamps && castsShadow) {	// UNOFFICIAL InvisibleLightShadowStrength, see above
				const bool invisible = LampFixtures::Invisible(lamp, lampCfg->Forward.FixtureDistance);
				if (invisible) castsShadow = invisibleStrength > 0.0f;
				static std::unordered_map<const void*, unsigned> loggedKind;	// light -> glow list generation it was logged for
				if (logLamps && loggedKind.size() < 512) {
					auto seen = loggedKind.find(v->second);
					if (seen == loggedKind.end() || seen->second != LampFixtures::Generation) {
						loggedKind[v->second] = LampFixtures::Generation;
						const D3DXVECTOR4 at = lamp->m_worldTransform.pos.toD3DXVEC4();
						Logger::Log("LAMP %p is %s: nearest glowing mesh %.0f units (GLOW %d; FixtureDistance %.0f; %u glowing meshes in the cell)%s | at %.0f %.0f %.0f, reach %.0f, colour %.2f %.2f %.2f x %.2f",
							v->second, invisible ? "an INVISIBLE light" : "at a lamp", LampFixtures::NearestGlow(lamp), LampFixtures::NearestGlowIndex(lamp),
							lampCfg->Forward.FixtureDistance, (unsigned)LampFixtures::Glows.size(),
							invisible ? (invisibleStrength > 0.0f ? ", its shadow at InvisibleLightShadowStrength" : ", no shadow") : "",
							at.x, at.y, at.z, lamp->Spec.r, lamp->Diff.r, lamp->Diff.g, lamp->Diff.b, lamp->Dimmer);
					}
				}
			}

			float lampReach = lamp->Spec.r * lampCfg->LightRadiusMult;
			D3DXVECTOR4 lampPos = lamp->m_worldTransform.pos.toD3DXVEC4();
			lampPos.w = lampReach;

			if (castsShadow && !clusterMembers.empty() && std::find(clusterMembers.begin(), clusterMembers.end(), (const void*)v->second) != clusterMembers.end())
				castsShadow = false;	// UNOFFICIAL ShadowClusterEnabled: its group's brightest lamp casts the shadow

			if (castsShadow && shadowSlot >= ShadowLightsMax && lampReach > 10) ShadowPool::NoteCandidate(v->second);	// UNOFFICIAL 2026-10-29 ShadowPool
			if (castsShadow && shadowSlot < ShadowLightsMax && lampReach > 10) {
				// add found light to the ranked list of lights that cast shadows (slots are assigned below)
				ShadowCasters[shadowSlot] = v->second;

				shadowSlot++;
				TheShadowManager->PointLightsNum++; // Constant to track number of shadow casting lights are present
			}
			else if (trackedSlot < TrackedLightsMax) {
				LightsList[trackedSlot] = lamp;
				LightPosition[trackedSlot] = lampPos;
				LightColor[ShadowCubeMapsScreen + trackedSlot] = D3DXVECTOR4(lamp->Diff.r, lamp->Diff.g, lamp->Diff.b, lamp->Dimmer);
				trackedSlot++;
			};
		}
		else if (lamp->EffectType == NiDynamicEffect::EffectTypes::SPOT_LIGHT) {
			// Here will go the collecting of the spotlights and setting of constants
		}
		v++;
	}

	// Give every caster a cubemap slot, each light keeping the slot it had last frame (PointShadowSlots.h): a light that
	// only changed rank no longer forces its cubemap to be redrawn. The slot order does not change the image, except for
	// the last slot, which PointShadows.fx lights without a shadow lookup: it keeps the farthest caster, as the plain
	// distance order gave it, and only slots 0..10 take part in the stable assignment.
	{
		const int casters = TheShadowManager->PointLightsNum;
		const int sampledSlots = allSlots || batched ? ShadowLightsMax : min(ShadowLightsMax, (int)ShadowCubeMapsSampled);
		const int stableCasters = min(casters, sampledSlots);
		const void* ranked[ShadowCubeMapsMax] = {};
		const void* assigned[ShadowCubeMapsMax] = {};
		for (int r = 0; r < stableCasters; r++) ranked[r] = ShadowCasters[r];
		// UNOFFICIAL 2026-11-01 FadeOut ([Shaders.ShadowsInteriors.Forward] FadeOut, seconds, live, 0 = off; docs/next/FLICKER-PROGRAM.md part 3),
		// indoors with the forward lamp shadows: a lamp's shadow faded IN over FadeIn when it got a slot but vanished at once when it lost it.
		// Now a slot holder that drops out of the ranked casters keeps its slot (so its cube map, its atlas tile and its RT slot) while its
		// shadow fades out over FadeOut (PointShadowForward::FadeOutLeft, multiplied into the slot's fade by Publish), then lets it go; if it
		// ranks again before that, its shadow climbs back at the same rate. A newcomer takes a free slot at once if there is one, otherwise it
		// waits (it lights unshadowed, as any lamp past the cap does, and takes the slot when it is free, then fades in). The choice: the
		// slots stay the LightPoints ones (a fading lamp does not use slots above LightPoints: those have no cube map drawn, and the pool
		// draws through the last one), so LightPoints stays the cap of shadowed lamps, full or fading, and nothing costs more. A lamp it drops
		// that the ShadowPool also holds is let go at once (FadeOutPooled: the pool's still map takes over, unfaded); a lamp that leaves the
		// game's list, is culled or switched off is let go at once. 0: AssignStablePointShadowSlots as before.
		const float fadeOut = indoorsForward && allSlots && !batched ? lampCfg->Forward.FadeOut : 0.0f;
		bool fadeHeld[ShadowCubeMapsMax] = {}, fadeWaiting[ShadowCubeMapsMax] = {};
		int fadeHeldCount = 0, fadeWaitingCount = 0;
		{
			static double fadeClock = 0.0;
			const double fadeNow = ShadowHold::Now();
			const float step = fadeClock > 0.0 ? (float)(std::min)((std::max)(fadeNow - fadeClock, 0.0), 0.1) : 0.0f;	// (capped: a loading screen is not a fade)
			fadeClock = fadeNow;
			if (fadeOut > 0.0f) {
				auto isRanked = [&](const void* light) { for (int r = 0; r < stableCasters; r++) if (ranked[r] == light) return true; return false; };
				auto isAlive = [&](const void* light) { return std::find(aliveLamps.begin(), aliveLamps.end(), light) != aliveLamps.end(); };
				for (int s = 0; s < sampledSlots; s++) {
					const ShadowSceneLight* light = (const ShadowSceneLight*)previousSlots[s];
					if (!light) continue;
					float& left = PointShadowForward::FadeOutLeft[s];
					if (PointShadowForward::FadeOutLamp[s] != light) { PointShadowForward::FadeOutLamp[s] = light; left = 1.0f; }
					if (isRanked(light)) { left = (std::min)(1.0f, left + step / fadeOut); continue; }
					if (PointShadowForward::FadeOutPooled[s] || !isAlive(light)) continue;	// let go at once
					const bool first = left >= 1.0f;
					left -= step / fadeOut;
					if (!(left > 0.0f)) continue;	// faded out: let go
					fadeHeld[s] = true;
					fadeHeldCount++;
					static unsigned fadeLogs = 0;
					if (first && logLamps && fadeLogs < 200) { fadeLogs++; Logger::Log("LAMP %p is outranked: it keeps slot %d while its shadow fades out (FadeOut %.2f s)", (const void*)light, s, fadeOut); }
				}
			}
			for (int s = 0; s < ShadowCubeMapsMax; s++) PointShadowForward::FadeOutPooled[s] = false;
		}
		if (fadeOut > 0.0f) {
			AssignStablePointShadowSlotsHeld(previousSlots, ranked, stableCasters, sampledSlots, fadeHeld, assigned, fadeWaiting);
			for (int r = 0; r < stableCasters; r++) if (fadeWaiting[r]) fadeWaitingCount++;
		}
		else AssignStablePointShadowSlots(previousSlots, ranked, stableCasters, sampledSlots, assigned);
		for (int s = 0; s < ShadowCubeMapsMax; s++)	// (a slot's FadeOut entry follows whoever has it now; a new holder starts full)
			if (s >= sampledSlots || PointShadowForward::FadeOutLamp[s] != assigned[s]) {
				PointShadowForward::FadeOutLamp[s] = s < sampledSlots ? (const ShadowSceneLight*)assigned[s] : nullptr;
				PointShadowForward::FadeOutLeft[s] = 1.0f;
			}
		// FadeOut: the lamps fading in a slot are not also in the tracked (unshadowed) list, which the fill loop above gave them; the
		// newcomers waiting for a slot go to its front (they rank above every lamp in it) as unshadowed lamps, and into the pool's candidates.
		if (fadeHeldCount || fadeWaitingCount) {
			NiPointLight* keepLight[TrackedLightsMax] = {};
			D3DXVECTOR4 keepPos[TrackedLightsMax], keepColour[TrackedLightsMax];
			int kept = 0;
			for (int r = 0; r < stableCasters && kept < TrackedLightsMax; r++) {
				if (!fadeWaiting[r] || !ShadowCasters[r] || !ShadowCasters[r]->sourceLight) continue;
				NiPointLight* L = ShadowCasters[r]->sourceLight;
				keepLight[kept] = L;
				keepPos[kept] = L->m_worldTransform.pos.toD3DXVEC4();
				keepPos[kept].w = L->Spec.r * lampCfg->LightRadiusMult;
				keepColour[kept] = D3DXVECTOR4(L->Diff.r, L->Diff.g, L->Diff.b, L->Dimmer);
				kept++;
			}
			auto heldLight = [&](const NiPointLight* L) {
				for (int s = 0; s < sampledSlots; s++) if (fadeHeld[s] && assigned[s] && ((const ShadowSceneLight*)assigned[s])->sourceLight == L) return true;
				return false;
			};
			for (int t = 0; t < trackedSlot && kept < TrackedLightsMax; t++) {
				if (!LightsList[t] || heldLight(LightsList[t])) continue;
				keepLight[kept] = LightsList[t]; keepPos[kept] = LightPosition[t]; keepColour[kept] = LightColor[ShadowCubeMapsScreen + t];
				kept++;
			}
			for (int t = 0; t < TrackedLightsMax; t++) {
				LightsList[t] = t < kept ? keepLight[t] : nullptr;
				LightPosition[t] = t < kept ? keepPos[t] : noLight;
				LightColor[ShadowCubeMapsScreen + t] = t < kept ? keepColour[t] : noLight;
			}
			trackedSlot = kept;
			if (fadeWaitingCount && ShadowPool::Wanted() > 0) {
				std::vector<ShadowSceneLight*> waiting;
				for (int r = 0; r < stableCasters; r++) if (fadeWaiting[r] && ShadowCasters[r]) waiting.push_back(ShadowCasters[r]);
				ShadowPool::Candidates.insert(ShadowPool::Candidates.begin(), waiting.begin(), waiting.end());
			}
		}
		// LogLamps: the lamps that gained or lost their shadow since last frame, and why. Only lamps in this frame's notes are
		// read (a lamp gone from the game's list may be freed: only its address is printed).
		static unsigned lampLogLines = 0;
		if (logLamps && lampLogLines < 600) {
			auto inSet = [](const void* const* set, int count, const void* light) { for (int s = 0; s < count; s++) if (set[s] == light) return true; return false; };
			auto find = [&](const void* light) -> const LampNote* { for (int n = 0; n < noteCount; n++) if (notes[n].light == light) return &notes[n]; return nullptr; };
			auto rankOf = [&](const void* light) { int r = 1; for (const auto& entry : rankedLamps) { if (entry.second == light) return r; r++; } return 0; };
			static const char* const why[] = { "outranked", "culled by the game", "off or too dim", "its light does not reach the view", "too far (8000)" };
			for (int s = 0; s < sampledSlots && lampLogLines < 600; s++) {
				const void* lost = previousSlots[s];
				if (lost && !inSet(assigned, sampledSlots, lost)) {
					lampLogLines++;
					const LampNote* n = find(lost);
					if (!n) Logger::Log("LAMP %p lost its shadow: no longer in the game's light list.", lost);
					else Logger::Log("LAMP %p lost its shadow: %s (rank %d of %d, %d get shadows) | at %.0f %.0f %.0f radius %.0f, %.0f away, colour %.2f %.2f %.2f x %.2f, key %.3f",
						lost, why[n->reason], n->reason ? 0 : rankOf(lost), (int)rankedLamps.size(), sampledSlots, n->position.x, n->position.y, n->position.z,
						n->radius, n->distance, n->colour.x, n->colour.y, n->colour.z, n->colour.w, n->key);
				}
				const void* gained = assigned[s];
				if (gained && !inSet(previousSlots, sampledSlots, gained) && lampLogLines < 600) {
					lampLogLines++;
					const LampNote* n = find(gained);
					if (n) Logger::Log("LAMP %p got a shadow (slot %d, rank %d of %d) | at %.0f %.0f %.0f radius %.0f, %.0f away, colour %.2f %.2f %.2f x %.2f, key %.3f, carry flag %d, cube radius %.0f",
						gained, s, rankOf(gained), (int)rankedLamps.size(), n->position.x, n->position.y, n->position.z, n->radius, n->distance,
						n->colour.x, n->colour.y, n->colour.z, n->colour.w, n->key, n->carry, n->radius * lampCfg->LightRadiusMult);
				}
			}
			if (lampLogLines >= 600) Logger::Log("LAMP log: 600 lines, stopping until restart.");
		}
		if (!allSlots && !batched && casters > sampledSlots) assigned[ShadowCubeMapsScreen - 1] = ShadowCasters[sampledSlots];
		// UNOFFICIAL 2026-11-01 (SessionLog / frame journal): each slot that changed lamp (a handover redraws its cube map), and how many
		// lamps crossed the 16th place of the ranking (a pixel's first 16 lamps get the deferred lamps' full filter)
		for (int s = 0; s < sampledSlots; s++) {
			if (assigned[s] == previousSlots[s]) continue;
			SessionLog::Count.lampSlotChanges++;
			FRAME_JOURNAL("lamp shadow slot %d: %p out, %p in", s, previousSlots[s], assigned[s]);
		}
		{	// (2026-11-01: always, for the in-memory ring of the FLASH / REPORT dumps: 16 x 16 pointer compares)
			static const void* lastTop[16] = {};
			const void* top[16] = {};
			const int topCount = min(casters, 16);
			for (int r = 0; r < topCount; r++) top[r] = ShadowCasters[r];
			int crossed = 0;
			for (int r = 0; r < 16; r++) {
				if (!top[r]) continue;
				bool before = false;
				for (int k = 0; k < 16 && !before; k++) before = lastTop[k] == top[r];
				if (!before) crossed++;
			}
			if (crossed) FRAME_JOURNAL("lamp ranking: %d lamps entered the first 16 (%d casters)", crossed, casters);
			memcpy(lastTop, top, sizeof(top));
		}
		for (int s = 0; s < ShadowCubeMapsMax; s++) previousSlots[s] = s < sampledSlots ? assigned[s] : nullptr;

		for (int s = 0; s < ShadowCubeMapsMax; s++) {
			ShadowSceneLight* shadowLight = (ShadowSceneLight*)assigned[s];
			ShadowLightsList[s] = shadowLight;
			if (!shadowLight) continue; // position and colour stay cleared
			NiPointLight* lamp = shadowLight->sourceLight;
			D3DXVECTOR4 lampPos = lamp->m_worldTransform.pos.toD3DXVEC4();
			lampPos.w = lamp->Spec.r * lampCfg->LightRadiusMult;
			BatchLightPosition[s] = lampPos;
			BatchLightColor[s] = D3DXVECTOR4(lamp->Diff.r, lamp->Diff.g, lamp->Diff.b, lamp->Dimmer);
			if (s >= ShadowCubeMapsScreen) continue;	// (the screen-space passes' arrays end at 12; see below)
			if (batched && s == ShadowCubeMapsScreen - 1) continue;	// (batched: slot 11 is the second batch's, with its shadow)
			shadowConsts->ShadowLightPosition[s] = lampPos;
			LightColor[s] = D3DXVECTOR4(lamp->Diff.r, lamp->Diff.g, lamp->Diff.b, lamp->Dimmer);
		}
		// UNOFFICIAL (2026-10-08): the darkening mask (KeepDarkening, PointShadows.fx) knows 12 shadow lamps and 12 tracked
		// ones. Lamps in the forward slots past 12 go to the front of the tracked list, so what they light is not darkened as
		// unlit; what no longer fits there (forward slots past 24, the tracked lamps they push off) goes to the extra list,
		// which the mask reads too (TESR_ExtraLights, the next ExtraLightsMax: the mask then knows the 40 nearest lamps). One
		// register per lamp: w packs the radius (whole units) and the weight the mask gives it, luma(colour) x dimmer.
		if (allSlots && sampledSlots > ShadowCubeMapsScreen) {
			NiPointLight* keepLight[TrackedLightsMax] = {};
			D3DXVECTOR4 keepPos[TrackedLightsMax], keepColour[TrackedLightsMax];
			int kept = 0, extra = 0;
			auto take = [&](NiPointLight* light, const D3DXVECTOR4& pos, const D3DXVECTOR4& colour) {
				if (kept < TrackedLightsMax) { keepLight[kept] = light; keepPos[kept] = pos; keepColour[kept] = colour; kept++; }
				else if (extra < ExtraLightsMax && pos.w >= 1.0f) {
					const float weight = (0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z) * colour.w;	// Helpers.hlsl luma
					ExtraLights[extra] = pos;
					ExtraLights[extra].w = floorf(pos.w) * 8.0f + std::clamp(weight, 0.0f, 7.99f);
					extra++;
				}
			};
			for (int s = ShadowCubeMapsScreen; s < sampledSlots; s++) {
				ShadowSceneLight* shadowLight = (ShadowSceneLight*)assigned[s];
				if (!shadowLight || !shadowLight->sourceLight) continue;
				NiPointLight* lamp = shadowLight->sourceLight;
				D3DXVECTOR4 pos = lamp->m_worldTransform.pos.toD3DXVEC4();
				pos.w = lamp->Spec.r * lampCfg->LightRadiusMult;
				take(lamp, pos, D3DXVECTOR4(lamp->Diff.r, lamp->Diff.g, lamp->Diff.b, lamp->Dimmer));
			}
			for (int t = 0; t < trackedSlot; t++)
				if (LightsList[t]) take(LightsList[t], LightPosition[t], LightColor[ShadowCubeMapsScreen + t]);
			for (int t = 0; t < TrackedLightsMax; t++) {
				LightsList[t] = t < kept ? keepLight[t] : nullptr;
				LightPosition[t] = t < kept ? keepPos[t] : noLight;
				LightColor[ShadowCubeMapsScreen + t] = t < kept ? keepColour[t] : noLight;
			}
		}
	}

	lightsClock.LogTime("ShaderManager::GetNearbyLights");
}


// UNOFFICIAL ExteriorLightPoints (2026-10-13): the screen-space point-shadow pass reads 11 shadowed lamps at once (ps_3_0 has 16
// samplers), so outdoors past those the shadow slots are drawn in batches of 11 more: the slots' cube maps put in the first
// 11 cube samplers' places (the samplers read through these pointers at each bind) and their lamps in the first 11 entries,
// then the pass's AddShadowedLamps technique adds their light to the buffer (blend one, one). The sum is saturated where it
// is read, as the single pass saturated its own; the lamps without shadows and the flashlight are in the first batch only.
void ShaderManager::RenderPointShadowBatches() {
	ShadowsExteriorEffect* shadows = Effects.ShadowsExteriors;
	const int slots = PointShadowForward::ShadowedSlots(true);
	const int batchSize = ShadowCubeMapsSampled;	// 11
	if (slots <= batchSize || !Effects.PointShadows->Effect || Effects.PointShadows->Effect->GetTechnique(1) == NULL) return;
	IDirect3DCubeTexture9* savedCube[ShadowCubeMapsScreen];
	D3DXVECTOR4 savedPosition[ShadowCubeMapsScreen], savedColour[ShadowCubeMapsScreen];
	for (int k = 0; k < ShadowCubeMapsScreen; k++) {
		savedCube[k] = shadows->Textures.ShadowCubeMapTexture[k];
		savedPosition[k] = shadows->Constants.ShadowLightPosition[k];
		savedColour[k] = LightColor[k];
	}
	int drawn = 0;
	for (int first = batchSize; first < slots; first += batchSize) {
		bool any = false;
		for (int k = 0; k < ShadowCubeMapsScreen; k++) {
			const int s = first + k;
			IDirect3DCubeTexture9* cube = k < batchSize && s < slots ? shadows->Textures.ShadowCubeMapTexture[s] : nullptr;
			const bool live = cube && BatchLightPosition[s].w > 0.0f;
			shadows->Textures.ShadowCubeMapTexture[k] = live ? cube : savedCube[k];	// (an unused entry keeps a valid cube bound)
			shadows->Constants.ShadowLightPosition[k] = live ? BatchLightPosition[s] : D3DXVECTOR4(0, 0, 0, 0);
			LightColor[k] = live ? BatchLightColor[s] : D3DXVECTOR4(0, 0, 0, 0);
			any |= live;
		}
		if (any) { RenderEffectToRT(shadows->Textures.ShadowPassSurface, Effects.PointShadows, false, 1); drawn++; }
	}
	int live = 0;
	for (int s = 0; s < slots; s++) live += BatchLightPosition[s].w > 0.0f;
	static int loggedLive = -1, loggedDrawn = -1;
	if (live != loggedLive || drawn != loggedDrawn) {
		Logger::Log("UNOFFICIAL ExteriorLightPoints: %d lamps with shadows (of %d slots), %d further batch(es) drawn", live, slots, drawn);
		loggedLive = live; loggedDrawn = drawn;
	}
	for (int k = 0; k < ShadowCubeMapsScreen; k++) {
		shadows->Textures.ShadowCubeMapTexture[k] = savedCube[k];
		shadows->Constants.ShadowLightPosition[k] = savedPosition[k];
		LightColor[k] = savedColour[k];
	}
}

// Whether the shadow pass runs this frame: indoors with the interior shadows on; outdoors when an ortho map is wanted or
// the sun shadows are on (setting and effect).
bool ShaderManager::ShouldRenderShadowMaps() {
	if (!GameState.isExterior) return Effects.ShadowsInteriors->Enabled;
	const ShadowsExteriorEffect* sun = Effects.ShadowsExteriors;
	return orthoRequired || orthoSkyRequired || (sun->Settings.Exteriors.Enabled && sun->Enabled);
}

/*
* Renders a given effect to an arbitrary render target
*/
float LampContactStrength();	// UNOFFICIAL 2026-10-16 (effects\ShadowsExterior.cpp)
bool LampMaskHalfRes();	// UNOFFICIAL 2026-10-16 MaskHalfRes (effects\ShadowsExterior.cpp)
IDirect3DTexture9* LampMaskHalfTexture();
IDirect3DSurface9* LampMaskHalfSurface();

/*
* UNOFFICIAL 2026-10-18 LampMaskScissor (contribution audit, work\bench\deep-roadmap\evidence\contrib-LOG.md). The lamp mask
* (PointShadows.fx Shadow(), technique MergedPointShadows) is the sum of the lamps' light at each pixel's reconstructed position
* world_pos = TESR_CameraPosition + toWorld(uv) * depth. Every lamp term is exactly 0 where that position is not inside the
* lamp's sphere (its radius = the .w the shader divides by): a shadowed slot returns 0 when !(distance < 1) (w 0 included: the
* distance is then INF or NaN), slot 11 and the tracked lamps go through GetPointLightAtten / LampReach, both exactly 0 from
* distance 1 on, the flashlight's attenuation is saturate((1 - s)^2 / (1 + 5 s)) = 0 at s = 1, and saturate(0) = 0. Since
* world_pos lies on the pixel's own view ray (toWorld uses the view matrix's axes and 1 / P00, 1 / P11, exactly what is read
* here), a pixel whose ray misses every lamp's sphere is written 0. So: the screen rectangle holding the projections of the lamps'
* spheres (each sphere's world-aligned bounding box, widened by 2 % + 2 units, its 8 corners projected, +-1 pixel) is the only
* place the pass can write anything but 0. No lamp (or none on screen): the target is just cleared (the pass's own first step);
* otherwise cleared, then the pass drawn with the scissor test on that rectangle. A box reaching behind the camera, a lamp list
* this does not cover (the extra lamps), an unusual radius or camera: the whole pass, as before.
* Returns 0 = draw as before, 1 = nothing to draw (clear only), 2 = draw inside 'rect'.
*/
int LampMaskRect(RECT& rect, int& lampCount) {	// (not static: EffectChain.cpp)
	lampCount = 0;
	if (TheShaderManager->ExtraLights[0].w != 0.0f) return 0;	// (the extra lamps: forward shadows indoors only; not covered)
	D3DXVECTOR4 lamps[ShadowCubeMapsScreen + TrackedLightsMax + 1];
	for (int i = 0; i < ShadowCubeMapsScreen; i++)
		if (TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition[i].w != 0.0f) lamps[lampCount++] = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition[i];
	for (int i = 0; i < TrackedLightsMax; i++)
		if (TheShaderManager->LightPosition[i].w != 0.0f) lamps[lampCount++] = TheShaderManager->LightPosition[i];
	if (TheShaderManager->SpotLightPosition[0].w != 0.0f) lamps[lampCount++] = TheShaderManager->SpotLightPosition[0];
	if (lampCount == 0) return 1;
	// The camera as the shader sees it, read from the same memory its constants are uploaded from at this moment
	// (TESR_ViewTransform = viewMatrix, TESR_ProjectionTransform = projMatrix, TESR_CameraPosition). The effect gets the matrices
	// as stored (Depth.hlsl's projectPosition multiplies a row vector by TESR_ProjectionTransform, D3D style, and works), so
	// toWorld's TESR_ViewTransform[0..2][2] is the view matrix's third column, the camera's forward axis (right, up: columns 1, 2).
	const D3DXMATRIX& V = TheRenderManager->viewMatrix;
	const D3DXMATRIX& P = TheRenderManager->projMatrix;
	const D3DXVECTOR3 R(V._11, V._21, V._31), U(V._12, V._22, V._32), F(V._13, V._23, V._33);
	// Sanity: a rotation (unit, orthogonal axes) and a perspective projection; anything else draws the pass whole.
	if (fabsf(D3DXVec3Length(&F) - 1.0f) > 1e-3f || fabsf(D3DXVec3Length(&R) - 1.0f) > 1e-3f || fabsf(D3DXVec3Length(&U) - 1.0f) > 1e-3f ||
		fabsf(D3DXVec3Dot(&F, &R)) > 1e-3f || fabsf(D3DXVec3Dot(&F, &U)) > 1e-3f || !(P._11 > 0.0f) || !(P._22 > 0.0f) || P._44 != 0.0f) {
		static bool reported = false;
		if (!reported) { reported = true; Logger::Log("LAMP MASK SCISSOR: unusual camera matrices (forward %.4f %.4f %.4f, P44 %.3f): the pass is drawn whole", F.x, F.y, F.z, P._44); }
		return 0;
	}
	{	// once: the forward axis next to the camera's (InvViewMatrix, the world camera) for the record
		static bool logged = false;
		const D3DXMATRIX& I = TheRenderManager->InvViewMatrix;
		if (!logged) { logged = true; Logger::Log("LAMP MASK SCISSOR: view forward %.4f %.4f %.4f, world camera forward %.4f %.4f %.4f, P00 %.4f P11 %.4f", F.x, F.y, F.z, I._31, I._32, I._33, P._11, P._22); }
	}
	const float W = (float)TheRenderManager->width, H = (float)TheRenderManager->height;
	const D3DXVECTOR3 C(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
	float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
	bool any = false;
	for (int k = 0; k < lampCount; k++) {
		const float radius = lamps[k].w;
		if (!(radius > 0.0f) || radius > 1e6f) return 0;
		const float reach = radius * 1.02f + 2.0f;
		float lx0 = 1e30f, ly0 = 1e30f, lx1 = -1e30f, ly1 = -1e30f;
		// fixes-a #14: the box's corners in camera space (x right, y up, z forward). A pixel's reconstructed position has z = its depth,
		// which is never behind the near plane, so only the part of the box with z >= kNearClip can meet a pixel: a box entirely behind
		// that costs nothing (the lamp is skipped), and a box that straddles it is clipped to it (the 12 edges that cross the plane
		// give the new vertices; the hull of a convex polygon's projection is the hull of its vertices' projections). The old code
		// drew the whole pass for either, so one lamp behind the camera switched the scissor off. kNearClip is a tenth of the world
		// near plane NVR sets (2.5, CameraManager.cpp), so nothing visible is cut (the in-game LampMaskScissorCheck compares it).
		const float kNearClip = 0.25f;
		D3DXVECTOR3 cam[8];
		bool front[8];
		int inFront = 0;
		for (int corner = 0; corner < 8; corner++) {
			const D3DXVECTOR3 p(lamps[k].x + ((corner & 1) ? reach : -reach) - C.x, lamps[k].y + ((corner & 2) ? reach : -reach) - C.y,
				lamps[k].z + ((corner & 4) ? reach : -reach) - C.z);
			cam[corner] = D3DXVECTOR3(D3DXVec3Dot(&p, &R), D3DXVec3Dot(&p, &U), D3DXVec3Dot(&p, &F));
			front[corner] = cam[corner].z >= kNearClip;
			if (front[corner]) inFront++;
		}
		if (inFront == 0) continue;	// the whole box is behind the camera: this lamp reaches no pixel
		auto grow = [&](float x, float y, float z) {
			const float u = (x / z * P._11 + 1.0f) * 0.5f * W;	// toWorld inverted: 2u - 1 = (x / z) * P00
			const float v = (1.0f - y / z * P._22) * 0.5f * H;	// 2v - 1 = -(y / z) * P11
			lx0 = (std::min)(lx0, u); lx1 = (std::max)(lx1, u); ly0 = (std::min)(ly0, v); ly1 = (std::max)(ly1, v);
		};
		for (int corner = 0; corner < 8; corner++) {
			if (!front[corner]) continue;
			grow(cam[corner].x, cam[corner].y, cam[corner].z);
			if (inFront == 8) continue;
			for (int bit = 1; bit < 8; bit <<= 1) {	// an edge from this corner to one behind the plane: its crossing point
				const int other = corner ^ bit;
				if (front[other]) continue;
				const float t = (kNearClip - cam[corner].z) / (cam[other].z - cam[corner].z);
				grow(cam[corner].x + (cam[other].x - cam[corner].x) * t, cam[corner].y + (cam[other].y - cam[corner].y) * t, kNearClip);
			}
		}
		if (lx1 < -1.0f || ly1 < -1.0f || lx0 > W + 1.0f || ly0 > H + 1.0f) continue;	// off screen: no pixel's ray meets it
		any = true;
		minX = (std::min)(minX, lx0); maxX = (std::max)(maxX, lx1); minY = (std::min)(minY, ly0); maxY = (std::max)(maxY, ly1);
	}
	if (!any) return 1;
	rect.left = (LONG)(std::max)(0.0f, floorf(minX) - 1.0f);
	rect.top = (LONG)(std::max)(0.0f, floorf(minY) - 1.0f);
	rect.right = (LONG)(std::min)(W, ceilf(maxX) + 1.0f);
	rect.bottom = (LONG)(std::min)(H, ceilf(maxY) + 1.0f);
	if (rect.right <= rect.left || rect.bottom <= rect.top) return 1;
	if (rect.left == 0 && rect.top == 0 && rect.right == (LONG)W && rect.bottom == (LONG)H) return 0;	// the whole screen anyway
	return 2;
}

#if NVR_TEST_TOOLS
/*
* DIAGNOSTIC [Main.Main.Performance] LampMaskScissorCheck 1 (development builds only; release builds leave it out): every 8th
* frame in which LampMaskScissor cleared or scissored the lamp mask, that buffer is read back, the whole pass is drawn over it
* as without the switch, read back too, and the two are compared texel by texel (G16R16). The frame then goes on with the whole
* pass's buffer, so the picture is the old one. Exact in any scene, moving or not (both from the same frame's inputs). A
* test-only GPU stall.
*/
void LampMaskSelfCheck(IDirect3DDevice9* Device, IDirect3DSurface9* surface, int maskMode, const RECT& rect) {	// (not static: EffectChain.cpp)
	static int check = 0;
	static unsigned frame = 0, checks = 0, checksCleared = 0, diffInside = 0, diffOutside = 0, maxDiff = 0, reported = 0;
	if ((frame % 60) == 0) check = TheSettingManager->GetSettingI("Main.Main.Performance", "LampMaskScissorCheck");
	frame++;
	if (check <= 0 || (frame % 8) != 0) return;
	static IDirect3DSurface9* readA = nullptr;
	static IDirect3DSurface9* readB = nullptr;
	static D3DSURFACE_DESC have = {};
	D3DSURFACE_DESC d = {};
	if (FAILED(surface->GetDesc(&d)) || d.Format != D3DFMT_G16R16) return;
	if (!readA || d.Width != have.Width || d.Height != have.Height) {
		if (readA) { readA->Release(); readA = nullptr; }
		if (readB) { readB->Release(); readB = nullptr; }
		if (FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &readA, NULL)) ||
			FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &readB, NULL))) {
			Logger::Log("LAMP MASK CHECK: could not create the read-back surfaces");
			if (readA) { readA->Release(); readA = nullptr; }
			return;
		}
		have = d;
	}
	if (FAILED(Device->GetRenderTargetData(surface, readA))) return;
	TheShaderManager->RenderEffectToRT(surface, TheShaderManager->Effects.PointShadows, true);	// the whole pass, as before
	if (FAILED(Device->GetRenderTargetData(surface, readB))) return;
	D3DLOCKED_RECT la = {}, lb = {};
	if (FAILED(readA->LockRect(&la, NULL, D3DLOCK_READONLY))) return;
	if (FAILED(readB->LockRect(&lb, NULL, D3DLOCK_READONLY))) { readA->UnlockRect(); return; }
	unsigned inside = 0, outside = 0, worst = 0, firstX = 0, firstY = 0;
	for (UINT y = 0; y < d.Height; y++) {
		const UInt16* pa = (const UInt16*)((const UInt8*)la.pBits + (size_t)y * la.Pitch);
		const UInt16* pb = (const UInt16*)((const UInt8*)lb.pBits + (size_t)y * lb.Pitch);
		for (UINT x = 0; x < d.Width * 2; x++) {
			if (pa[x] == pb[x]) continue;
			const unsigned diff = pa[x] > pb[x] ? pa[x] - pb[x] : pb[x] - pa[x];
			const bool in = maskMode == 2 && (LONG)(x / 2) >= rect.left && (LONG)(x / 2) < rect.right && (LONG)y >= rect.top && (LONG)y < rect.bottom;
			if (!inside && !outside) { firstX = x / 2; firstY = y; }
			if (in) inside++; else outside++;
			if (diff > worst) worst = diff;
		}
	}
	readB->UnlockRect();
	readA->UnlockRect();
	checks++;
	if (maskMode == 1) checksCleared++;
	diffInside += inside; diffOutside += outside; maxDiff = (std::max)(maxDiff, worst);
	if ((inside || outside) && reported < 10) {
		reported++;
		Logger::Log("LAMP MASK CHECK: DIFFERENCE, mode %d rect %d %d %d %d: %u values inside, %u outside, max %u (16-bit units), first at %u %u",
			maskMode, (int)rect.left, (int)rect.top, (int)rect.right, (int)rect.bottom, inside, outside, worst, firstX, firstY);
	}
	if (checks % 30 == 0)
		Logger::Log("LAMP MASK CHECK: %u checks (%u cleared only, %u scissored), differing values: %u inside the rectangles, %u outside, max %u",
			checks, checksCleared, checks - checksCleared, diffInside, diffOutside, maxDiff);
}
#endif

#if NVR_TEST_TOOLS
/*
* DIAGNOSTIC [Main.Main.Performance] InteriorShadowFoldCheck N (development builds only): every Nth frame in which InteriorShadowFold
* would apply, the frame takes the old path (the separate ShadowsInteriors pass, then the fog), and the scene as it was before that
* pass is kept (FoldCheckScene). After the fog the result is read back, the folded fog pass is drawn from the kept scene into a spare
* target with the same AO / sky inputs (VolumetricFogEffect sceneOverride / targetOverride), read back too, and the two are compared
* bit for bit (16-bit float RGB). The frame goes on with the old result. Exact in any scene, moving or not (same frame, same inputs).
*/
static IDirect3DTexture9* FoldCheckSceneTexture = nullptr;
IDirect3DSurface9* FoldCheckSceneSurface = nullptr;	// (not static: EffectChain.cpp)
static IDirect3DTexture9* FoldCheckOutTexture = nullptr;
static IDirect3DSurface9* FoldCheckOutSurface = nullptr;
static IDirect3DSurface9* FoldCheckReadA = nullptr;
static IDirect3DSurface9* FoldCheckReadB = nullptr;
bool FoldCheckSurfaces(IDirect3DDevice9* Device, IDirect3DSurface9* like) {	// (not static: EffectChain.cpp)
	D3DSURFACE_DESC d = {}, have = {};
	if (!like || FAILED(like->GetDesc(&d)) || d.Format != D3DFMT_A16B16G16R16F) return false;
	if (FoldCheckSceneSurface && SUCCEEDED(FoldCheckSceneSurface->GetDesc(&have)) && have.Width == d.Width && have.Height == d.Height) return true;
	IUnknown* old[] = { FoldCheckSceneSurface, FoldCheckSceneTexture, FoldCheckOutSurface, FoldCheckOutTexture, FoldCheckReadA, FoldCheckReadB };
	for (IUnknown* o : old) if (o) o->Release();
	FoldCheckSceneSurface = FoldCheckOutSurface = FoldCheckReadA = FoldCheckReadB = nullptr;
	FoldCheckSceneTexture = FoldCheckOutTexture = nullptr;
	if (FAILED(Device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &FoldCheckSceneTexture, NULL)) ||
		FAILED(FoldCheckSceneTexture->GetSurfaceLevel(0, &FoldCheckSceneSurface)) ||
		FAILED(Device->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &FoldCheckOutTexture, NULL)) ||
		FAILED(FoldCheckOutTexture->GetSurfaceLevel(0, &FoldCheckOutSurface)) ||
		FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &FoldCheckReadA, NULL)) ||
		FAILED(Device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &FoldCheckReadB, NULL))) {
		Logger::Log("INTERIOR SHADOW FOLD CHECK: could not create its surfaces");
		FoldCheckSceneSurface = nullptr;	// (leaked on purpose: test only)
		return false;
	}
	return true;
}

void InteriorFoldSelfCheck(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* SourceSurface,
	bool ao, IDirect3DTexture9* aoTexture, IDirect3DTexture9* skyTexture) {
	static unsigned checks = 0, differing = 0, failed = 0, reported = 0;
	static unsigned long long differingPixels = 0;
	static unsigned worst = 0;
	VolumetricFogEffect* Fog = TheShaderManager->Effects.VolumetricFog;
	IDirect3DSurface9* result = TheTextureManager->RenderedSurface;
	if (!FoldCheckSceneSurface || !result || FAILED(Device->GetRenderTargetData(result, FoldCheckReadA))) { failed++; return; }
	const int foldRound = TheSettingManager->SettingsMain.Main.InteriorShadowFoldRound;
	Fog->compositeShadow = false;
	Fog->compositeInteriorShadow = true;
	Fog->compositeRound = foldRound == 1 ? 1.0f : foldRound == 2 ? 2.0f : foldRound == 3 ? 0.0f : Fog->halfStoreMode == 2 ? 2.0f : 0.0f;
	Fog->compositeAO = ao;
	Fog->compositeAOTexture = aoTexture;
	Fog->compositeSkyTexture = ao ? skyTexture : nullptr;
	Fog->compositeApplied = false;
	Fog->gridTexture = nullptr;
	Fog->sceneOverride = FoldCheckSceneTexture;
	Fog->targetOverride = FoldCheckOutSurface;
	{ Fog->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Fog); }
	const bool applied = Fog->compositeApplied;
	Fog->sceneOverride = nullptr;
	Fog->targetOverride = nullptr;
	Fog->compositeShadow = Fog->compositeAO = Fog->compositeApplied = Fog->compositeInteriorShadow = false;
	Fog->compositeSkyTexture = nullptr;
	Device->SetRenderTarget(0, RenderTarget);
	if (!applied || FAILED(Device->GetRenderTargetData(FoldCheckOutSurface, FoldCheckReadB))) { failed++; return; }
	D3DSURFACE_DESC d = {};
	FoldCheckReadA->GetDesc(&d);
	D3DLOCKED_RECT la = {}, lb = {};
	if (FAILED(FoldCheckReadA->LockRect(&la, NULL, D3DLOCK_READONLY))) { failed++; return; }
	if (FAILED(FoldCheckReadB->LockRect(&lb, NULL, D3DLOCK_READONLY))) { FoldCheckReadA->UnlockRect(); failed++; return; }
	unsigned pixels = 0, frameWorst = 0, firstX = 0, firstY = 0;
	for (UINT y = 0; y < d.Height; y++) {
		const UInt16* pa = (const UInt16*)((const UInt8*)la.pBits + (size_t)y * la.Pitch);
		const UInt16* pb = (const UInt16*)((const UInt8*)lb.pBits + (size_t)y * lb.Pitch);
		for (UINT x = 0; x < d.Width; x++) {
			bool differs = false;
			for (int c = 0; c < 3; c++) {
				const UInt16 a = pa[x * 4 + c], b = pb[x * 4 + c];
				if (a == b) continue;
				differs = true;
				const unsigned diff = a > b ? a - b : b - a;	// in 16-bit float steps (same sign)
				if (diff > frameWorst) frameWorst = diff;
			}
			if (differs) { if (!pixels) { firstX = x; firstY = y; } pixels++; }
		}
	}
	FoldCheckReadB->UnlockRect();
	FoldCheckReadA->UnlockRect();
	checks++;
	if (pixels) { differing++; differingPixels += pixels; worst = (std::max)(worst, frameWorst); }
	if (pixels && reported < 12) {
		reported++;
		Logger::Log("INTERIOR SHADOW FOLD CHECK: DIFFERENCE, %u of %u pixels differ (max %u 16-bit float steps), first at %u %u",
			pixels, d.Width * d.Height, frameWorst, firstX, firstY);
	}
	if (checks % 10 == 0 || (checks < 10 && !pixels))
		Logger::Log("INTERIOR SHADOW FOLD CHECK: %u frames compared (old separate pass vs folded fog pass, same frame), %u differ (%llu pixels in all, max %u steps); %u checks could not run",
			checks, differing, (unsigned long long)differingPixels, worst, failed);
}
#endif

// One effect drawn into a surface of its own: that surface is target, rendered and source buffer at once.
void ShaderManager::RenderEffectToRT(IDirect3DSurface9* renderTarget, EffectRecord* effect, bool clearRenderTarget, UINT techniqueIndex) {
	IDirect3DDevice9* const device = TheRenderManager->device;
	device->SetRenderTarget(0, renderTarget);
	PixelInspector::OwnTargetScope inspect(effect, renderTarget, techniqueIndex);	// UNOFFICIAL 2026-10-26 (pixel inspector)
	effect->Render(device, renderTarget, renderTarget, techniqueIndex, clearRenderTarget, renderTarget);
}


// UNOFFICIAL 2026-10-27 RenderScale v2: the chain's first step on its own, so TAAEffect::UpscaleAfterEffects can redraw both at full
// size after its upscale. In a rect frame (RenderRect::On) both draw into the rect (EffectRecord / ApplyViewport).
void ShaderManager::RenderDepthAndNormals(IDirect3DSurface9* RenderTarget) {
	IDirect3DDevice9* const Device = TheRenderManager->device;
	static GpuTimer depthTimer("Depth combine");
	static GpuTimer normalsTimer("Normal reconstruction");
	auto bindFrameQuad = [&] { Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS)); };
	bindFrameQuad();
	Device->SetFVF(FrameFVF);	// the full-screen quad for both passes
	// When the normals pass would run anyway, try producing depth and normals in one draw.
	bool mergedNormals = false;
	{
		GpuProfileScope gpu(depthTimer, Device);
		NormalsEffect* Normals = Effects.Normals;
		CombineDepthEffect* const depth = Effects.CombineDepth;
		if (Normals->Enabled && Normals->Effect && Normals->ShouldRender())
			mergedNormals = depth->RenderWithNormals(Device, Normals->Textures.NormalsSurface);
		if (!mergedNormals) RenderEffectToRT(depth->Textures.CombinedDepthSurface, depth, false);
	}
	{
		GpuProfileScope gpu(normalsTimer, Device);
		NormalsEffect* const normals = Effects.Normals;
		if (!mergedNormals) RenderEffectToRT(normals->Textures.NormalsSurface, normals, false);
		// UNOFFICIAL 2026-10-23 (sharednormals; [Main.Main.Quality] SmoothNormals): the buffer smoothed in place, once, for every reader.
		Effects.Normals->RenderSmooth(Device);
		// UNOFFICIAL 2026-10-28 [Main.Main.Quality] CapturedNormals (core/CapturedNormals.h): the surfaces' own smooth normals written into
		// the buffer, for every reader. Not after an upscale to the screen (the normals were drawn at the internal resolution).
		if (CapturedNormals::MergeWanted() && !TAAEffect::UpscaledBeforeEffects())
			CapturedNormals::MergeIntoNormals(Device, Effects.Normals->Textures.NormalsSurface, Effects.CombineDepth->Textures.CombinedDepthTexture);
		Device->SetRenderTarget(0, RenderTarget);	// the caller's target back
	}
	if (RenderRect::Active()) bindFrameQuad();	// the rect quad (ApplyViewport) does not outlive this
}

void ShaderManager::RenderEffectsPreTonemapping(IDirect3DSurface9* RenderTarget) {
	SkyProjection::Join();	// UNOFFICIAL 2026-11-03 SkyOnWorker: every effect below may read the sky's results (some copy them by name)
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (core/Upscaler.h; off by default: nothing changes): once a frame, before anything below
	// (the RT point first of all), last frame's RR result becomes what the RT effects' denoisers go by this frame (RayReconstructionWantsRaw),
	// and this frame's starts at "not run". (Before the early returns: a frame without the chain ran no RR either.)
	Upscaler::RayReconstructionFrameBegin();
	// Nothing while NVR is switched off, before a cell exists, or under a terminal/lockpick screen once its background is ready.
	if (!TheSettingManager->SettingsMain.Main.RenderEffects || !Player->parentCell) return;
	if (GameState.OverlayIsOn && TESMain::IsMenuBackgroundReady()) return;
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (review): the frame's plan for the RR point, before the RT point and every RT denoiser
	// that asks Upscaler::RayReconstructionWantsRaw (false with RayReconstruction off: nothing changes).
	TAAEffect::RayReconstructionPlanned();

	TimeLogger chainClock;
	static CpuTimer preTonemapCpuTimer("Pre-tonemap chain (CPU)");
	CpuProfileScope preTonemapCpu(preTonemapCpuTimer);

	// UNOFFICIAL 2026-11-02 the chain itself: the step table in core/EffectChain.cpp (one place for the order). The context carries
	// what the steps share (the locals that were here) and ends the frame chain / restores AO and BounceLight when this returns.
	ChainContext ctx;
	ctx.Manager = this;
	ctx.Device = TheRenderManager->device;
	ctx.RenderTarget = RenderTarget;
	ctx.SourceSurface = TheTextureManager->SourceSurface;
	ctx.RenderedSurface = TheTextureManager->RenderedSurface;
	EffectChain::Run(this, EffectChain::PreTonemap, ctx);

	chainClock.LogTime("ShaderManager::RenderEffectsPreTonemapping");
}


// For A/B runs: the Performance and ReducedQuality switches in one log line, written when the F10 profile starts and
// again whenever one of them changes while it runs, so each stretch of timings can be matched to its settings.
// Lists whatever the defaults file has in those two sections, so new switches show up without touching this.
// UNOFFICIAL (2026-10-11): the game's active image-space effects (TES::activeIMODs: fades, night eye, chem tints, the
// wait-menu fade), logged with F10. For a screen tint or glare that stays when every NVR effect is off: the game's own
// final shader blends these in, so a stuck one shows here. The instance layout is not known, so each instance is named by
// its vtable, the image-space modifier form it points at is found by scanning its first words for a readable form of that
// type, and its first 12 words are dumped for decoding.
static const char* ImageSpaceInstanceKind(UInt32 vtable) {	// vtables from nvse/class_vtbls.h (kVtbl_ImageSpaceModifierInstance*)
	switch (vtable) {
	case 0x102D12C: return "form";
	case 0x102D2A4: return "DOF";
	case 0x102D36C: return "radial blur";
	case 0x102D454: return "radial blur (DRB)";
	case 0x102D1CC: return "base";
	default: return "unknown";
	}
}

static void LogActiveImageSpaceEffects() {
	if (!Tes) return;
	int count = 0;
	for (TList<void>::Entry* entry = &Tes->activeIMODs.First; entry && count < 32; entry = entry->next) {
		UInt32* instance = (UInt32*)entry->item;
		if (!instance || IsBadReadPtr(instance, 12 * sizeof(UInt32))) continue;
		count++;
		UInt32 formID = 0, formOffset = 0;
		for (UInt32 k = 1; k < 12 && !formID; k++) {
			TESForm* form = (TESForm*)instance[k];
			if (form && !IsBadReadPtr(form, sizeof(TESForm)) && form->formType == TESForm::FormType::kFormType_ImageSpaceModifier) {
				formID = form->refID;
				formOffset = k * 4;
			}
		}
		char words[12 * 9 + 1] = {};
		for (int k = 0; k < 12; k++) sprintf_s(words + k * 9, 10, "%08X ", instance[k]);
		Logger::Log("IMAGESPACE active %d: %s instance %p, IMOD form %08X (at +%02X) | %s", count, ImageSpaceInstanceKind(instance[0]),
			(void*)instance, formID, formOffset, words);
	}
	Logger::Log("IMAGESPACE %d active image-space effect(s) (the game's fades, night eye, chem and weather tints)", count);
}

static void LogActiveSwitches(bool force) {
	static std::string lastLine;
	static const char* sections[][2] = { { "Main.Main.Performance", "performance" }, { "Main.Main.ReducedQuality", "reduced quality" },
		{ "Shaders.Terrain.Parallax", "terrain parallax" }, { "Shaders.ShadowsExteriors.SunSmoothing", "sun smoothing" },
		{ "Shaders.BounceLight.Status", "bounce light" }, { "Shaders.BounceLight.Main", "bounce light" },
		{ "Shaders.ContactHardening.Status", "contact hardening" }, { "Shaders.ContactHardening.Main", "contact hardening" },
		{ "Shaders.Particles.Status", "lit particles" }, { "Shaders.Particles.Main", "lit particles" } };
	typedef SettingManager::Configuration Config;
	std::string line;	// " <section>: key=value key=value | <section>: ..."
	Config::SettingList nodes;
	for (auto& section : sections) {
		TheSettingManager->Config.FillSettings(&nodes, section[0]);
		std::sort(nodes.begin(), nodes.end());
		if (!line.empty()) line += " |";
		line += " ";
		line += section[1];
		line += ":";
		for (const Config::ConfigNode& node : nodes) {
			char value[96];
			switch (node.Type) {	// the typed value, so the line does not depend on how Value was formatted
			case Config::NodeType::Boolean: strcpy(value, node.BoolValue ? "on" : "off"); break;
			case Config::NodeType::Integer: sprintf(value, "%d", node.IntValue); break;
			case Config::NodeType::Float: sprintf(value, "%g", node.FloatValue); break;
			default: strcpy(value, node.Value); break;
			}
			line += " ";
			line += node.Key;
			line += "=";
			line += value;
		}
	}
	if (!force && line == lastLine) return;
	// Close the frame-time window at the change, so no FRAME TIMES line mixes frames from before and after.
	if (!force) TheFrameTimeMonitor().Flush();
	lastLine = line;
	Logger::Log("SWITCHES%s", line.c_str());
}

/*
* The post-tonemap chain, once a frame (and before it the per-frame bookkeeping: session log, F10 profile, frame interval).
*/
void ShaderManager::RenderEffects(IDirect3DSurface9* RenderTarget) {
	SkyProjection::Join();	// UNOFFICIAL 2026-11-03 SkyOnWorker (as RenderEffectsPreTonemapping)
	VolumetricSmoke::FrameNumber++;   // the smoke is fetched once a frame (VolumetricSmoke::Fetch)
	SessionLog::OnFrame();	// UNOFFICIAL 2026-11-01: heartbeat, cell / weather / loading lines, setting changes, the frame journal switch (SessionLog.h)
	{	// UNOFFICIAL 2026-10-31 register-clash guard: the total once, 300 frames into the first loaded game (the startup shaders are in; core/RegisterClash.h)
		static unsigned clashFrames = 0;
		if (Player && Player->parentCell && clashFrames < 300 && ++clashFrames == 300) RegisterClash::Summary();
	}
	// F10 profiling toggle and the frame interval run before the RenderEffects check, so an
	// effects-off run still logs its real frame time for comparison. This is the last NVR call
	// of the frame, after the pre-tonemap chain, so the toggle takes effect from the next frame.
	static CpuTimer frameIntervalTimer("Frame interval (CPU)");
	// UNOFFICIAL 2026-11-05 (test aid): [Main.Main.Performance] GpuProfile, live, switches the same profile from the settings, so an
	// unattended bench (console NVRSetSetting "Main.Main.Performance" "GpuProfile" 1/0) can measure effects without pressing F10.
	static int profileSetting = -1;
	const int profileNow = TheSettingManager->GetSettingI("Main.Main.Performance", "GpuProfile") != 0 ? 1 : 0;
	const bool profileFlip = profileSetting >= 0 && profileNow != profileSetting && (profileNow != 0) != GpuTimer::Enabled;
	profileSetting = profileNow;
	if (Player->parentCell && !InterfaceManager->IsActive(Menu::kMenuType_Loading) && (Global->OnKeyDown(0x44) || profileFlip)) {
		GpuTimer::Enabled = !GpuTimer::Enabled;
		Logger::Log("GPU PROFILE P78 test (forward quality + culling + lamp trims) %s (F10), effects %s, D3D9 runtime: %s", GpuTimer::Enabled ? "enabled" : "paused",
			TheSettingManager->SettingsMain.Main.RenderEffects ? "on" : "OFF", TheRenderManager->D3D9RuntimeDescription());
		LogActiveImageSpaceEffects();
		if (!GpuTimer::Enabled) TheFrameTimeMonitor().Flush(); // report the frames collected so far
		else LogActiveSwitches(true);
	}
	if (GpuTimer::Enabled) {
		static unsigned switchCheckFrame = 0;
		if (++switchCheckFrame >= 30) { switchCheckFrame = 0; LogActiveSwitches(false); }
		// Frames within three seconds of a cell change or loading screen are counted separately: the
		// hitches there are expected (streaming), the ones in steady play are what hurt the 1% lows.
		static TESObjectCELL* lastCell = nullptr;
		static unsigned framesSinceTransition = 1000;
		TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
		if (cell != lastCell || InterfaceManager->IsActive(Menu::kMenuType_Loading)) { lastCell = cell; framesSinceTransition = 0; }
		else framesSinceTransition++;
		const double interval = frameIntervalTimer.Tick();
		if (interval > 0.0) TheFrameTimeMonitor().Add(interval, framesSinceTransition < 180);
	}
	// Nothing while NVR is off, before a cell exists, or under a terminal/lockpick screen (the effects would show through it).
	if (!TheSettingManager->SettingsMain.Main.RenderEffects || !Player->parentCell || GameState.OverlayIsOn) return;

	TimeLogger chainClock;
	static CpuTimer postChainCpuTimer("Post chain (CPU)");
	CpuProfileScope postChainCpu(postChainCpuTimer);

	// The scene camera as the game left it, for the post-tonemap effects.
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();

	static GpuTimer imageAdjustTimer("Image adjust + debug");
	// UNOFFICIAL 2026-11-02 the chain itself: the step table in core/EffectChain.cpp (one place for the order). The context carries
	// what the steps share (the locals that were here) and ends the frame chain when this returns.
	ChainContext ctx;
	ctx.Manager = this;
	ctx.Device = TheRenderManager->device;
	ctx.RenderTarget = RenderTarget;
	ctx.SourceSurface = TheTextureManager->SourceSurface;
	ctx.RenderedSurface = TheTextureManager->RenderedSurface;
	ctx.imageAdjustTimer = &imageAdjustTimer;
	EffectChain::Run(this, EffectChain::PostTonemap, ctx);

	chainClock.LogTime("ShaderManager::RenderEffects");
}

// The registered effect / family of that name, null when there is none.
EffectRecord* ShaderManager::GetEffectByName(const char* effectName) {
	const auto found = EffectsNames.find(effectName);
	return found == EffectsNames.end() ? nullptr : *found->second;
}

ShaderCollection* ShaderManager::GetShaderCollectionByName(const char* collectionName) {
	const auto found = ShaderNames.find(collectionName);
	return found == ShaderNames.end() ? nullptr : *found->second;
}

/*
* Writes the settings corresponding to the shader/effect name, to switch it between enabled/disabled.*
* Also creates or deletes the corresponding Effect Record.
*/
void ShaderManager::SwitchShaderStatus(const char* shaderName) {
	IsMenuSwitch = true;	// (true only while the switch is made)

	// An effect or a family flips itself (loading what it needs); its new state is stored as the menu's.
	bool nowOn;
	if (EffectRecord* effect = GetEffectByName(shaderName)) nowOn = effect->SwitchEffect();
	else if (ShaderCollection* family = GetShaderCollectionByName(shaderName)) nowOn = family->SwitchShader();
	else nowOn = !TheSettingManager->GetMenuShaderEnabled(shaderName);

	// UNOFFICIAL: a menu entry with only settings (Shaders.<Name>.Status Enabled, e.g. ContactHardening, which
	// ShadowManager reads each frame) just flips its setting.
	// SetMenuShaderEnabled picks the setting (Status, or [Shaders.<Name>.Main] Enabled for Cloth/CloudShadows; none for Foliage).
	TheSettingManager->SetMenuShaderEnabled(shaderName, nowOn);
	IsMenuSwitch = false;
}

// Sets a custom constant (one a shader declared but NVR does not feed); names no shader declared are ignored.
void ShaderManager::SetCustomConstant(const char* constantName, D3DXVECTOR4 value) {
	const auto found = CustomConst.find(std::string(constantName));
	if (found != CustomConst.end()) found->second = value;
}


bool FrameChain::Owns(IDirect3DSurface9* renderTarget, IDirect3DSurface9* renderedSurface) {
	const bool owns = Active && renderTarget == GameTarget && renderedSurface == TheTextureManager->RenderedSurface;
	if (owns) ReclaimFinal();
	return owns;
}

// The predicted last effect wrote the finished image into the game target, but another effect is
// about to render after it. Put the image back where the chain expects it (Surf[Current]) so that
// effect samples the right thing; a wrong prediction costs one copy instead of a wrong image.
void FrameChain::ReclaimFinal() {
	if (!FinalWritten) return;
	TheRenderManager->device->StretchRect(GameTarget, NULL, Surf[Current], NULL, D3DTEXF_NONE);
	FinalWritten = false;
	FinalEffect = nullptr;
}

// Point the TESR_RenderedBuffer slot at the current image. Effect samplers follow the slot
// (TextureRecord::TextureRef), so this is all a swap needs.
void FrameChain::Publish() {
	TheTextureManager->RenderedTexture = Tex[Current];
	TheTextureManager->RenderedSurface = Surf[Current];
}

bool FrameChain::EnsureTexture(Pair& pair, int slot) {
	if (pair.Texture[slot]) return true;
	if (FAILED(TheRenderManager->device->CreateTexture(pair.Width, pair.Height, 1, D3DUSAGE_RENDERTARGET, pair.Format,
		D3DPOOL_DEFAULT, &pair.Texture[slot], NULL)) || FAILED(pair.Texture[slot]->GetSurfaceLevel(0, &pair.Surface[slot]))) {
		if (pair.Surface[slot]) { pair.Surface[slot]->Release(); pair.Surface[slot] = nullptr; }
		if (pair.Texture[slot]) { pair.Texture[slot]->Release(); pair.Texture[slot] = nullptr; }
		Logger::Log("[ERROR] Frame chain: could not create a %ux%u target (format %u); using copies.", pair.Width, pair.Height, pair.Format);
		return false;
	}
	return true;
}

bool FrameChain::Begin(IDirect3DSurface9* gameTarget) {
	if (Active) End();
	FinalEffect = nullptr;
	FinalWritten = false;
	if (!gameTarget || TheSettingManager->SettingsMain.Main.DisableFrameChain) return false;
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DSURFACE_DESC desc = {};
	if (FAILED(gameTarget->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE) return false;

	// One set of NVR textures per target format (pre-tonemap HDR and post-tonemap LDR).
	int index = -1;
	for (int i = 0; i < 2 && index < 0; i++) {
		const Pair& pair = Pairs[i];
		if (pair.Format == desc.Format && pair.Width == desc.Width && pair.Height == desc.Height) index = i;
	}
	for (int i = 0; i < 2 && index < 0; i++) {
		if (Pairs[i].Format == D3DFMT_UNKNOWN) {
			Pairs[i].Format = desc.Format;
			Pairs[i].Width = desc.Width;
			Pairs[i].Height = desc.Height;
			index = i;
		}
	}
	if (index < 0) return false; // a third format: stay on the legacy path rather than churn
	Pair& pair = Pairs[index];

	// Use the game target itself as one buffer when it is a texture level (not the back buffer).
	// Single-level only: NVR's own buffers have no mip chain, and effects that sample the rendered
	// buffer at reduced size (average luma, bloom downsample) must not pick up stale mip levels.
	IDirect3DTexture9* gameTexture = nullptr;
	const bool useGameTexture = !TheSettingManager->SettingsMain.Main.DisableChainGameTexture &&
		SUCCEEDED(gameTarget->GetContainer(IID_IDirect3DTexture9, (void**)&gameTexture)) && gameTexture &&
		gameTexture->GetLevelCount() == 1;
	if (useGameTexture) {
		if (!EnsureTexture(pair, 0)) { gameTexture->Release(); return false; }
		Tex[0] = gameTexture;	Surf[0] = gameTarget;
		Tex[1] = pair.Texture[0]; Surf[1] = pair.Surface[0];
		GameTexture = gameTexture;
	}
	else {
		if (gameTexture) { gameTexture->Release(); gameTexture = nullptr; }
		if (!EnsureTexture(pair, 0) || !EnsureTexture(pair, 1)) return false;
		if (FAILED(Device->StretchRect(gameTarget, NULL, pair.Surface[0], NULL, D3DTEXF_NONE))) return false;
		Tex[0] = pair.Texture[0]; Surf[0] = pair.Surface[0];
		Tex[1] = pair.Texture[1]; Surf[1] = pair.Surface[1];
	}

	static bool reported[2][2] = {};
	if (!reported[index][useGameTexture]) {
		Logger::Log("UNOFFICIAL frame chain: %ux%u format %u, %s.", desc.Width, desc.Height, desc.Format,
			useGameTexture ? "game target used as a chain buffer (no seed copy)" : "two Vegas Shaders buffers (seed copy)");
		reported[index][useGameTexture] = true;
	}

	Current = 0;
	SavedTexture = TheTextureManager->RenderedTexture;
	SavedSurface = TheTextureManager->RenderedSurface;
	GameTarget = gameTarget;
	Active = true;
	Publish();
	return true;
}

void FrameChain::Commit() {
	if (!Active) return;
	Writes++; LastWrite = 1;	// UNOFFICIAL 2026-10-26 (pixel inspector)
	Current ^= 1;
	Publish();
}

void FrameChain::Sync() {
	if (!Active) return;
	Writes++; LastWrite = 3;	// UNOFFICIAL 2026-10-26 (pixel inspector): the effect then draws into the game target
	ReclaimFinal();
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (Surf[Current] == GameTarget) {
		// The game target already holds the image; move the current image to the other buffer
		// so the legacy path does not sample the texture it renders into.
		Device->StretchRect(GameTarget, NULL, Surf[Current ^ 1], NULL, D3DTEXF_NONE);
		Current ^= 1;
		Publish();
	}
	else
		Device->StretchRect(Surf[Current], NULL, GameTarget, NULL, D3DTEXF_NONE);
}

void FrameChain::End() {
	if (!Active) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	{
		// Shows whether a chain ends on the wrong buffer (~0 ms when it does not): the HDR chain (game
		// texture) copies when its pass count is odd, the LDR chain (back buffer) unless its last pass
		// wrote the game target itself (FinalWritten).
		static GpuTimer endCopyTimers[2] = { GpuTimer("  Chain end copy (HDR)"), GpuTimer("  Chain end copy (LDR)") };
		GpuProfileScope gpu(endCopyTimers[GameTexture ? 0 : 1], Device);
		if (Surf[Current] != GameTarget && !FinalWritten)
			Device->StretchRect(Surf[Current], NULL, GameTarget, NULL, D3DTEXF_NONE);
	}
	FinalEffect = nullptr;
	FinalWritten = false;
	Device->SetRenderTarget(0, GameTarget);
	TheTextureManager->RenderedTexture = SavedTexture;
	TheTextureManager->RenderedSurface = SavedSurface;
	if (GameTexture) { GameTexture->Release(); GameTexture = nullptr; }
	Tex[0] = Tex[1] = nullptr;
	Surf[0] = Surf[1] = nullptr;
	Active = false;
	GameTarget = nullptr;
}
