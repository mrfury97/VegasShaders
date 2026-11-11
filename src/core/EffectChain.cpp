// UNOFFICIAL 2026-11-02 the post-process chain as step tables (EffectChain.h). The two tables at the bottom of this file are the
// order of the frame; each step is one block of what ShaderManager::RenderEffectsPreTonemapping / RenderEffects did inline, moved
// here unchanged (same conditions, calls, timers, PixelInspector / Diagnostics calls, in the same order). A step reads what it
// shares with others from the ChainContext: the lines at the top of a step marked "ctx" are those reads, the "ctx.x = x" lines at
// its end are what it hands on. A static GpuTimer moved into the one step that uses it, with its name unchanged.
//
// The whole frame, in order (step -> what it does -> when it does something):
//
// Pre-tonemap chain (ShaderManager::RenderEffectsPreTonemapping, after its early returns; HDR scene in, before the game's tonemap)
//   1  FrameQuad                        Frame quad: the effects' vertex stream and FVF
//                                       when: always
//   2  SceneInProbes                    PixelInspector capture + Diagnostics stage mark of the scene as the game drew it
//                                       when: always (the tools do nothing while off)
//   3  DepthAndNormals                  RenderDepthAndNormals: TESR_DepthBuffer + TESR_NormalsBuffer
//                                       when: always
//   4  ObjectMotion                     Object motion: the world-space move of what moved, for TAA / FSR
//                                       when: ObjectMotion::Live
//   5  LampVisibility                   VisibleRanking: how much of the picture each candidate lamp lights
//                                       when: LampVisibility::On
//   6  DepthNormalProbes                PixelInspector captures of the linear depth and the view normal
//                                       when: always (nothing while off)
//   7  SunDepthFit                      FitToDepth: the occlusion-query measure of how far the visible surfaces reach
//                                       when: SunDepthFit::Wanted()
//   8  OrthoProbe                       Test tools: the depth the effects read, after an ortho dump
//                                       when: NVR_TEST_TOOLS builds
//   9  ShadowPlan                       forwardInterior / keepDarkening: forward lamp shadows indoors, and whether their darkening is kept
//                                       when: always (sets context flags); sets forwardInterior, keepDarkening
//  10  RtCore                           Ray tracing core point: RT registration once, RtCore::Frame, RT lamp denoise, capsule shadows, back depth
//                                       when: always (each does nothing unless on)
//  11  ShadowPasses                     Lamp mask (PointShadows: scissor / half-res / batches / RT exterior mask), lamp contact, sun contact + forward shadows into ShadowPassSurface; or the one-time "no shadow" clear
//                                       when: shadows on for the cell type; forward interior without KeepDarkening: clear only
//  12  SunFarFactor                     SunFar: the land's and clouds' shadow factor; FlickerWatch World probe
//                                       when: always (SunFar marks itself unused when off)
//  13  ChainBegin                       FrameChain::Begin (or the legacy copy into RenderedSurface); arms Chain.End() for the end of the call
//                                       when: always
//  14  MaterialCapture                  MaterialAware reflections: the materials in the scene's alpha copied out
//                                       when: always (nothing without it)
//  15  SkinScattering                   Screen-space skin blur, on the scene exactly as drawn
//                                       when: SkinScattering effect + Skin shaders active
//  16  SkyViewTable                     Physical sky table (read by the sky shader next frame)
//                                       when: SkyView->ShouldRender()
//  17  VolumetricClouds                 Volumetric clouds over the sky
//                                       when: wouldRender(VolumetricClouds)
//  18  AmbientLightTakeover             AmbientLight on: AmbientOcclusion and BounceLight switched off until the call returns
//                                       when: always (sets ambientLight); sets ambientLight
//  19  HiZ                              Hi-Z pyramid (NVR DXVK compute) for AmbientLight
//                                       when: DxvkCompute::PassesOn() && ambientLight && HiZWanted
//  20  CompositePlan                    Which pass applies the sun shadow / AO / interior darkening: the fog composite, the merged pair, AmbientLight or their own passes; FlickerWatch flags
//                                       when: always (sets context flags); sets AO, Fog, shadowApplies, aoApplies, foldCheckFrame, interiorApplies, composite, pairApply, shadowIntoAmbientLight
//  21  ShadowApply                      Sun-shadow apply (ShadowsExteriors) outdoors, or the interior darkening pass indoors
//                                       when: when not folded into the composite / pair / AmbientLight
//  22  CoverMapUpdate                   Persistent wetness and snow: the cover map update
//                                       when: TheCoverMap && (wet || snow)
//  23  SnowAccumulation                 Snow accumulation (CapturedNormals off in an upscaled-before-effects frame; jitter)
//                                       when: always called (the effect decides)
//  24  AmbientOcclusion                 AO (deferred combine for the composite / pair), the merged shadow + AO apply, the composite's fallback
//                                       when: always called
//  25  SpecularBouncePlan               specularRenders / bounceIntoSpecular: the bounce combine folded into the specular's
//                                       when: always (sets context flags); sets specularRenders, bounceIntoSpecular
//  26  AmbientLight                     AmbientLight: prepare, the AO sky pass, AO + bounce in one pass (+ sun shadow folded in)
//                                       when: ambientLight
//  27  LitProbes                        FlickerWatch Lit probe; SettingShots stage shot (test tools)
//                                       when: FlickerWatch on outdoors / NVR_TEST_TOOLS
//  28  BounceLight                      Bounce light (deferred combine when it goes into the specular)
//                                       when: BounceLight->Enabled
//  29  BloodFlow                        Blood flow
//                                       when: always called (nothing with no blood near)
//  30  WetFlashlightSpecularUnderwater  wetness reflections, the HUD raindrops' wetness, wet world, flashlight beam + flashlight, specular (with the bounce combine), underwater
//                                       when: each as its effect decides
//  31  SpillSelfTest                    NeonSpillSelfTest capture
//                                       when: NVR_TEST_TOOLS builds
//  32  Reflections                      Screen-space reflections (Hi-Z, buffers, composite) or the surroundings cube alone
//                                       when: wouldRender(Reflections) / CubeWanted
//  33  VolumetricLightGrid              Froxel grid of the volumetric light, for the fog
//                                       when: CompositeTechnique() == 8 && wouldRender(VolumetricLight) (sets lightGrid); sets lightGrid
//  34  VolumetricFog                    Volumetric fog, with the composite apply (shadow / AO / interior), its fallback, the fold self-check
//                                       when: always called
//  35  VolumetricLight                  Volumetric light composite (if the fog did not), or its own buffers + composite
//                                       when: lightGrid / wouldRender(VolumetricLight)
//  35b DustMotes                        Dust specks in lit air: half-res pass into its buffer, additive pass into the current image
//                                       when: DustMotes on for the cell type, a lamp within reach or the sun's grid (ShouldRender)
//  36  Sandstorm                        Sandstorm (fast path or the original)
//                                       when: always called (nothing outside a storm)
//  37  GodRays                          God rays
//                                       when: always called
//  38  RayReconstruction                DLSS Ray Reconstruction point
//                                       when: Effects.TAA (off: nothing)
//  39  LumaExposureBloom                Average luma (compute or D3D9), exposure, bloom buffers
//                                       when: avglumaRequired / not identity exposure
//  40  LensAndPreTonemapLUT             Lens, and the LUT when it runs before the tonemap
//                                       when: LUT: Settings.PreTonemapping
//
// Post-tonemap chain (ShaderManager::RenderEffects, after its early returns; the tonemapped picture in, the screen out)
//   1  FrameQuad              Frame quad stream + FVF; the render target
//                             when: always
//   2  ChainBegin             FrameChain::Begin (or the legacy copy); arms Chain.End() for the end of the call
//                             when: always
//   3  TonemapProbes          PixelInspector + Diagnostics stage mark of the tonemapped picture; the clip view's start capture
//                             when: always (tools off: nothing)
//   4  FusedPassPlan          colorLUT / cinemaDither: Coloring+LUT and Cinema+SkyDither in one pass
//                             when: always (sets context flags); sets colorLUT, cinemaDither
//   5  FinalEffect            FrameChain::SetFinalEffect: the effect that renders last writes the game target
//                             when: always
//   6  TAA                    TAA (and its upscaler paths)
//                             when: always called (off by default)
//   7  WeatherAndLegacyBloom  Rain, snow, GPU particles, legacy bloom
//                             when: always called
//   8  HeatHaze               Heat haze
//                             when: always called
//   9  VolumetricSmoke        GunFX volumetric smoke
//                             when: always called
//  10  BarrelHaze             GunFX barrel haze
//                             when: always called
//  11  ColoringLUT            Coloring, and the LUT (or both in one pass)
//                             when: LUT: not PreTonemapping and not fused
//  12  DepthOfField           Depth of field (distant-only technique when autofocus is off)
//                             when: always called
//  13  MotionBlur             Motion blur
//                             when: always called
//  14  LensOverlays           Blood lens, water lens, LowHF
//                             when: always called
//  15  DitherBuster           Dither buster
//                             when: always called
//  16  SMAA                   SMAA
//                             when: always called
//  17  FXAA                   FXAA
//                             when: always called
//  18  Sharpening             Sharpening
//                             when: always called
//  19  Cinema                 Cinema (with the sky dither when fused)
//                             when: always called
//  20  ImageAdjust            Image adjust
//                             when: always called
//  21  Bodycam                Body camera look
//                             when: always called
//  22  Dream                  Dream (a "fun" trip look, off by default)
//                             when: always called
//  23  SkyDither              Sky dither
//                             when: !cinemaDither
//  24  SunDepthFitView        FitToDepthDebug view
//                             when: SunDepthFit::DebugView > 0
//  25  RtSunDebug             RT sun shadows DebugView 4
//                             when: always called (nothing otherwise)
//  26  Debug                  Debug effect + the clipping view
//                             when: always called
//  27  CoverMapDebug          Cover map debug view
//                             when: TheCoverMap
//  28  FlickerWatch           FlickerWatch frame check (or its flags cleared)
//                             when: FlickerWatch on outdoors

#include "OrthoProbe.h"	// UNOFFICIAL 2026-10-17 test tools (AfterCombine)
#include "CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals (TESR_CapturedNormalsData)
#include "ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion (TESR_ObjectMotionData)
#include "Diagnostics.h"	// UNOFFICIAL 2026-11-01 the flash detector's stage probes (StageMark), the report burst's VL stages
#include "LampVisibility.h"	// UNOFFICIAL 2026-10-30 VisibleRanking
#include "FlickerWatch.h"	// UNOFFICIAL 2026-10-23 test only
#include "GpuProfiler.h"
#include "HiZ.h"	// UNOFFICIAL 2026-10-26 (deep/fork-hiz)
#include "PixelInspector.h"	// UNOFFICIAL 2026-10-26 (debugging tool)
#include "PointShadowSlots.h"
#include "PointShadowForward.h"
#include "CapsuleShadows.h"	// UNOFFICIAL 2026-10-31 CapsuleShadows
#include "BackDepth.h"	// UNOFFICIAL 2026-10-31 BackDepth
#include "EffectSelfTest.h"
#include "../effects/GpuParticles/GpuParticles.h"	// UNOFFICIAL 2026-10-26 (NVR DXVK 1.5-particles)
#include "SunDepthFit.h"	// UNOFFICIAL 2026-10-27 FitToDepth
#include "../effects/CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness and snow
#include "RtCore.h"	// UNOFFICIAL 2026-10-28 (NVR DXVK) the ray tracing core
#include "RtSunShadows.h"	// UNOFFICIAL 2026-10-28 RT sun shadows
#include "BendContact.h"	// UNOFFICIAL 2026-11-06 forward Bend contact shadows
#include "RtFogVisibility.h"	// UNOFFICIAL 2026-11-07 (ptaaa-fog) RT fog visibility
#include "RtPathTrace.h"	// UNOFFICIAL 2026-11-02 the reference path tracer
#include "Upscaler.h"	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction
#include "EffectChain.h"

// Defined in effects\ShadowsExterior.cpp (declared in ShaderManager.cpp as well).
float LampContactStrength();
bool LampMaskHalfRes();
IDirect3DTexture9* LampMaskHalfTexture();
IDirect3DSurface9* LampMaskHalfSurface();
// Defined in ShaderManager.cpp (file-static there before the chain moved here).
int LampMaskRect(RECT& rect, int& lampCount);
#if NVR_TEST_TOOLS
void LampMaskSelfCheck(IDirect3DDevice9* Device, IDirect3DSurface9* surface, int maskMode, const RECT& rect);
extern IDirect3DSurface9* FoldCheckSceneSurface;
bool FoldCheckSurfaces(IDirect3DDevice9* Device, IDirect3DSurface9* like);
void InteriorFoldSelfCheck(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* SourceSurface,
	bool ao, IDirect3DTexture9* aoTexture, IDirect3DTexture9* skyTexture);
#endif

// The scope guards, as the two functions' local structs did it.
ChainContext::ChainGuard::~ChainGuard() { if (armed) TheShaderManager->Chain.End(); }
ChainContext::EnabledGuard::~EnabledGuard() { if (effect) effect->Enabled = was; }

// Was the lambda wouldRender in RenderEffectsPreTonemapping (same body).
static bool wouldRender(EffectRecord* effect) { return effect && effect->Enabled && effect->Effect && effect->ShouldRender(); }
// Was the lambda renders in RenderEffects (same body).
static bool renders(EffectRecord* effect) { return effect && effect->Enabled && effect->Effect && effect->ShouldRender(); }

// The interior darkening pass on its own (was the lambda interiorApply in RenderEffectsPreTonemapping; body unchanged): the
// separate pass, as it ran before (and still runs when the fold cannot).
static void InteriorApplyPass(ChainContext& ctx) {
	ShaderManager::EffectsStruct& Effects = ctx.Manager->Effects;
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool forwardInterior = ctx.forwardInterior;
	Effects.ShadowsInteriors->skipBlur = forwardInterior && !Effects.ShadowsExteriors->Settings.Interiors.Forward.DarkeningBlur;
	{ Effects.ShadowsInteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, true, SourceSurface); PixelInspector::AfterFrame(Effects.ShadowsInteriors); }
	Effects.ShadowsInteriors->skipBlur = false;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Pre-tonemap steps (were RenderEffectsPreTonemapping's body)
// ---------------------------------------------------------------------------------------------------------------------------

void ShaderManager::PreStep_FrameQuad(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	// prepare device for effects
	Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS));
	Device->SetFVF(FrameFVF);
}

void ShaderManager::PreStep_SceneInProbes(ChainContext& ctx) {
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-26 pixel inspector (core\PixelInspector.h, off by default): the scene as the game drew it, before any NVR effect.
	PixelInspector::CaptureIf("Scene in (HDR, before Vegas Shaders)", RenderTarget);
	Diagnostics::StageMark(Diagnostics::SceneMarkName, RenderTarget);	// UNOFFICIAL 2026-11-01: the flash detector's first stage (core/Diagnostics.h)
}

void ShaderManager::PreStep_DepthAndNormals(ChainContext& ctx) {
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// render post process normals for use by shaders
	RenderDepthAndNormals(RenderTarget);
}

void ShaderManager::PreStep_ObjectMotion(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-29 [Main.Main.Quality] ObjectMotion (core/ObjectMotion.h): the world-space move of what moved since last frame, for
	// every temporal reader (TAA, FSR's motion vectors). Off: nothing runs (TESR_ObjectMotionData.x = 0).
	if (ObjectMotion::Live) {
		static GpuTimer objectMotionTimer("Object motion");
		GpuProfileScope gpu(objectMotionTimer, Device);
		ObjectMotion::Render(Device, Effects.CombineDepth->Textures.CombinedDepthTexture);
		Device->SetRenderTarget(0, RenderTarget);
		Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS));
		Device->SetFVF(FrameFVF);
	}
}

void ShaderManager::PreStep_LampVisibility(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-30 VisibleRanking (core/LampVisibility.h): how much of the visible picture each candidate lamp lights.
	if (LampVisibility::On) {
		static GpuTimer lampVisibilityTimer("Lamp visibility (VisibleRanking)");
		GpuProfileScope gpu(lampVisibilityTimer, Device);
		LampVisibility::Render(Device, Effects.CombineDepth->Textures.CombinedDepthTexture);
		Device->SetRenderTarget(0, RenderTarget);
		Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS));
		Device->SetFVF(FrameFVF);
	}
}

void ShaderManager::PreStep_DepthNormalProbes(ChainContext& ctx) {
	// UNOFFICIAL 2026-10-26 pixel inspector: the linear depth and the view normal every effect reads.
	PixelInspector::CaptureIf("Depth (TESR_DepthBuffer, linear / farZ)", Effects.CombineDepth->Textures.CombinedDepthTexture, PixelInspector::Depth);
	PixelInspector::CaptureIf("View normal (TESR_NormalsBuffer)", Effects.Normals->Textures.NormalsTexture, PixelInspector::Normal);
}

void ShaderManager::PreStep_SunDepthFit(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-27 FitToDepth ([Shaders.ShadowsExteriors.Main], core\SunDepthFit.h): how far the visible surfaces reach,
	// counted on a grid of 8 x 8 pixel blocks by occlusion queries read back frames later (no wait). Off: nothing runs.
	if (SunDepthFit::Wanted()) {
		static GpuTimer depthFitTimer("Sun depth fit");
		GpuProfileScope gpu(depthFitTimer, Device);
		SunDepthFit::Measure(Device);
		Device->SetRenderTarget(0, RenderTarget);
	}
}

void ShaderManager::PreStep_OrthoProbe(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
#if NVR_TEST_TOOLS
	OrthoProbe::AfterCombine(Device);	// UNOFFICIAL 2026-10-17 test tools: the depth the effects read, after an ortho dump
#endif
}

void ShaderManager::PreStep_ShadowPlan(ChainContext& ctx) {
	// UNOFFICIAL interior forward point-light shadows (PointShadowForward.h): the object shaders shadow each lamp this
	// frame, so the screen-space shadows are not applied again. KeepDarkening keeps their darkening of what no tracked lamp
	// reaches: the mask is drawn without its cube-map lookups (TESR_ShadowFade.z 0 makes PointShadows.fx count every lamp
	// as unblocked) and applied in one pass (no blur unless DarkeningBlur). Without it, neither runs; the buffer is then
	// cleared to "no shadow" for the effects that read it (indoors only Underwater).
	// (Only while the PBR object shaders, which draw the forward shadows, are on and were drawing this frame: with PBR off
	// the old screen-space shadows run instead of none at all.)
	const bool forwardInterior = !GameState.isExterior && Effects.ShadowsInteriors->Enabled && PointShadowForward::Active &&
		PointShadowForward::BoundThisFrame && Shaders.PBR && Shaders.PBR->Enabled;
	const bool keepDarkening = !forwardInterior || Effects.ShadowsExteriors->Settings.Interiors.Forward.KeepDarkening;
	ctx.forwardInterior = forwardInterior;
	ctx.keepDarkening = keepDarkening;
}

void ShaderManager::PreStep_RtCore(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	// UNOFFICIAL 2026-10-28 (NVR DXVK) the ray tracing core's point of the frame (RtCore.h): the depth and normals exist, the sun
	// shadows come next; indoors and outdoors, with or without the shadow passes. Nothing happens unless an RT effect is on (and the
	// fork, a GPU with ray tracing and the helper are there).
	{
		static bool rtRegistered = false;
		if (!rtRegistered) { RtSunShadows::Register(); Effects.Reflections->RtRegister(); RtLampShadows::Register(); AmbientOcclusionEffect::RtRegister(); RtPathTrace::Register(); RtFogVisibility::Register(); rtRegistered = true; }	// UNOFFICIAL 2026-10-28 RT sun + RT reflections + RT lamp shadows + RT AO/sky (+ 2026-11-07 ptaaa-fog: RT fog visibility, last: it rides along)
		// UNOFFICIAL 2026-11-10 (PT early kick, env NVR_RT_EARLY_PT; a no-op without it) the path tracer's early frame collected. Before
		// Frame, not between Frame and Replace: Frame's exchange collects anything still open first (RtCore's safety net), which would
		// leave this call nothing to do.
		RtPathTrace::CollectBeforeShow(Device);
		RtCore::Frame(Device);
		RtPathTrace::Replace(Device);	// UNOFFICIAL 2026-11-03 [Main.Main.RayTracing] PTReplace: the path traced picture into the scene (off)
		RtLampShadows::Denoise(Device);	// UNOFFICIAL 2026-10-29: the traced lamp shadows through the shared denoiser (Denoise, off)
		CapsuleShadows::Render(Device);	// UNOFFICIAL 2026-10-31: people's capsules + contact shadows into the same buffer (no RT; off)
		BackDepth::Render(Device);	// UNOFFICIAL 2026-10-31 BackDepth: the near objects' back faces, for the effects' thickness (off)
	}
}

void ShaderManager::PreStep_ShadowPasses(ChainContext& ctx) {
	static GpuTimer pointShadowTimer("Point shadow lighting");
	static GpuTimer sunContactTimer("Sun contact shadows");
	IDirect3DDevice9* Device = ctx.Device;
	const bool forwardInterior = ctx.forwardInterior;
	const bool keepDarkening = ctx.keepDarkening;
	// UNOFFICIAL (2026-10-07 review): the "no shadow" clear of the buffer is done once, not on every frame that draws no
	// shadow pass (a full-screen clear and two target switches for a buffer that already held the clear value).
	static bool shadowBufferClear = false;
	// render a shadow pass for point lights
	if (forwardInterior && !keepDarkening) {
		if (!shadowBufferClear) { Effects.ShadowsExteriors->clearShadowsBuffer(); shadowBufferClear = true; }
	}
	else if ((GameState.isExterior && Effects.ShadowsExteriors->Enabled) || (!GameState.isExterior && Effects.ShadowsInteriors->Enabled)) {
		shadowBufferClear = false;	// the passes below write it
		{
			GpuProfileScope gpu(pointShadowTimer, Device);
			const float pointShadowsWere = Effects.ShadowsExteriors->Constants.ShadowFade.z;
			if (forwardInterior) Effects.ShadowsExteriors->Constants.ShadowFade.z = 0.0f;
			// UNOFFICIAL 2026-10-16 MaskHalfRes: with forward lamp shadows the mask is only the lamps' reach (smooth): drawn at half
			// resolution (PointShadows.fx technique 3, mask + depth) and upsampled depth-aware into the buffer (technique 4).
			static EffectRecord::CachedHandle halfCache, upCache;
			const bool halfMask = forwardInterior && LampMaskHalfRes() &&
				Effects.PointShadows->TechniqueHandle(halfCache, "LampMaskHalf") == Effects.PointShadows->Effect->GetTechnique(3) &&
				Effects.PointShadows->TechniqueHandle(upCache, "LampMaskUpsample") == Effects.PointShadows->Effect->GetTechnique(4) &&
				Effects.PointShadows->Effect->GetTechnique(4) != NULL;
			// The stock/custom shader remains compatible: it has no named merged
			// technique, so lights 6-11 still take the original second pass.
			static EffectRecord::CachedHandle mergedTechnique;	// UNOFFICIAL: looked up once per loaded effect
			const bool mergedPointShadows = Effects.PointShadows->TechniqueHandle(mergedTechnique, "MergedPointShadows") != NULL;
			// UNOFFICIAL 2026-10-18 LampMaskScissor ([Main.Main.Performance], LampMaskRect above): the mask is drawn only inside the
			// screen rectangle its lamps' spheres can reach (nothing at all with no lamp on screen); every other pixel keeps the
			// clear's 0, which is what the pass writes there. Only on the plain path (not MaskHalfRes, not the outdoor batches,
			// merged technique present, the effect drawing).
			RECT maskRect = {};
			int lampCount = 0, maskMode = 0;
			if (TheSettingManager->SettingsMain.Main.LampMaskScissor && !halfMask && mergedPointShadows &&
				!PointShadowForward::ExteriorBatched(GameState.isExterior) &&
				Effects.PointShadows->Enabled && Effects.PointShadows->Effect && Effects.PointShadows->ShouldRender())
				maskMode = LampMaskRect(maskRect, lampCount);
			// UNOFFICIAL 2026-10-27 RenderScale v2: the rectangle is in full-screen pixels; a rect frame draws the mask into its top-left part.
			if (maskMode == 2 && RenderRect::Active()) {
				maskRect.left = (LONG)floorf((float)maskRect.left * RenderRect::ScaleX);
				maskRect.top = (LONG)floorf((float)maskRect.top * RenderRect::ScaleY);
				maskRect.right = (std::min)((LONG)ceilf((float)maskRect.right * RenderRect::ScaleX), (LONG)RenderRect::Width(TheRenderManager->width));
				maskRect.bottom = (std::min)((LONG)ceilf((float)maskRect.bottom * RenderRect::ScaleY), (LONG)RenderRect::Height(TheRenderManager->height));
			}
			static int loggedMaskMode = -1;
			if (maskMode != loggedMaskMode) {
				// fixes-a #51: the first 20 switches only (it was a line per switch, ~380 an hour while turning); more with the F10 profile
				static int stateLines = 0;
				if (stateLines < 20 || GpuTimer::Enabled) {
					Logger::Log("LAMP MASK STATE: %s (%d lamps in its lists)", maskMode == 1 ? "SKIPPED, no lamp reaches the screen: the buffer cleared to 0, as the pass would write" :
						maskMode == 2 ? "SCISSORED to the lamps' screen rectangle" : "drawn whole", lampCount);
					if (++stateLines == 20 && !GpuTimer::Enabled) Logger::Log("LAMP MASK STATE: no more lines (20 logged; the F10 profile logs them all)");
				}
				loggedMaskMode = maskMode;
			}
			if (GpuTimer::Enabled) {	// a line every 240 frames with the profile on: how often each case ran
				static unsigned frames = 0, skipped = 0, scissored = 0, lampsSum = 0;
				static double area = 0.0;
				frames++; lampsSum += lampCount;
				if (maskMode == 1) skipped++;
				if (maskMode == 2) { scissored++; area += (double)(maskRect.right - maskRect.left) * (maskRect.bottom - maskRect.top) / ((double)TheRenderManager->width * TheRenderManager->height); }
				if (frames >= 240) {
					Logger::Log("LAMP MASK SCISSOR %s: %u frames, %u cleared only, %u scissored (average %.1f%% of the screen), %u whole; %.1f lamps per frame in its lists",
						TheSettingManager->SettingsMain.Main.LampMaskScissor ? "on" : "off", frames, skipped, scissored, scissored ? 100.0 * area / scissored : 0.0,
						frames - skipped - scissored, (double)lampsSum / frames);
					frames = skipped = scissored = lampsSum = 0; area = 0.0;
				}
			}
			if (maskMode != 0) {
				// The pass's own clear, on the whole target (the scissor test off: a clear honours it). SetRenderTarget resets the
				// scissor rectangle to the whole target, as RenderEffectToRT leaves it.
				IDirect3DSurface9* maskSurface = Effects.ShadowsExteriors->Textures.ShadowPassSurface;
				DWORD scissorWas = FALSE;
				RECT scissorRectWas = {};	// fixes-a #15: saved and restored like LampScissor.h (it no longer relies on SetRenderTarget's reset)
				Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorWas);
				const bool haveRectWas = SUCCEEDED(Device->GetScissorRect(&scissorRectWas));
				Device->SetRenderTarget(0, maskSurface);
				Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
				Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0L);	// (EffectRecord::Render's clear)
				if (maskMode == 2) {
					// The pass itself, unchanged, inside the rectangle: RenderEffectToRT without its SetRenderTarget (which would reset
					// the rectangle). Its target is not the frame chain's, so EffectRecord::Render takes the plain path (no target
					// switch); no technique state touches the scissor, and the effect's Begin/End restores what its passes change.
					Device->SetScissorRect(&maskRect);
					Device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
					Effects.PointShadows->Render(Device, maskSurface, maskSurface, 0, false, maskSurface);
					Device->SetRenderTarget(0, maskSurface);	// the rectangle back to the whole target
				}
				Device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorWas);
				if (haveRectWas) Device->SetScissorRect(&scissorRectWas);
#if NVR_TEST_TOOLS
				LampMaskSelfCheck(Device, maskSurface, maskMode, maskRect);	// DIAGNOSTIC LampMaskScissorCheck (off: returns at once)
#endif
			}
			else if (halfMask) {
				RenderEffectToRT(LampMaskHalfSurface(), Effects.PointShadows, true, 3);
				Device->SetTexture(13, LampMaskHalfTexture());
				TheRenderManager->renderState->NoteDeviceTexture(13, LampMaskHalfTexture());
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.PointShadows, false, 4);
				Device->SetTexture(13, nullptr);
				TheRenderManager->renderState->NoteDeviceTexture(13, nullptr);
			}
			else RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.PointShadows, true);
			if (PointShadowForward::ExteriorBatched(GameState.isExterior)) RenderPointShadowBatches();	// UNOFFICIAL ExteriorLightPoints
			if (!mergedPointShadows && Effects.ShadowsExteriors->Settings.Interiors.LightPoints > 6)
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.PointShadows2, false);
			// UNOFFICIAL 2026-10-29 RT lamp shadows Exteriors: the traced lamp mask over what those passes wrote (off: nothing)
			if (GameState.isExterior && pointShadowsWere > 0.0f) RtLampShadows::ApplyExteriorMask(Device, Effects.ShadowsExteriors->Textures.ShadowPassSurface);
			Effects.ShadowsExteriors->Constants.ShadowFade.z = pointShadowsWere;
		}
		// UNOFFICIAL 2026-10-16 lamp contact shadows ([Shaders.ShadowsInteriors.Main] ContactShadows): PointShadows.fx technique 2
		// (LampContact) multiplies the lamp mask just written by the contact term. Indoors; only with that technique there (a
		// custom PointShadows.fx without it draws nothing more).
		if (!GameState.isExterior && LampContactStrength() > 0.0f) {
			static EffectRecord::CachedHandle contactCache;
			D3DXHANDLE contactTechnique = Effects.PointShadows->TechniqueHandle(contactCache, "LampContact");
			if (contactTechnique && Effects.PointShadows->Effect->GetTechnique(2) == contactTechnique) {
				static GpuTimer lampContactTimer("Lamp contact shadows");
				GpuProfileScope gpu(lampContactTimer, Device);
				RenderEffectToRT(Effects.ShadowsExteriors->Textures.ShadowPassSurface, Effects.PointShadows, false, 2);
			}
		}
		if (GameState.isExterior) {
			GpuProfileScope gpu(sunContactTimer, Device);
			static EffectRecord::CachedHandle fusedCache;	// UNOFFICIAL: looked up once per loaded effect
			D3DXHANDLE fusedTechnique = Effects.SunShadows->TechniqueHandle(fusedCache, "ForwardContactShadows");
			const bool fusedForwardContact = Effects.ShadowsExteriors->Settings.Exteriors.ForwardShadows &&
				fusedTechnique != NULL && Effects.SunShadows->Effect->GetTechnique(1) == fusedTechnique &&
				!RtSunShadows::Replacing();	// UNOFFICIAL 2026-10-28 RT sun shadows Mode 1: the deferred technique (its apply fades the cascades out near)
			// UNOFFICIAL Sharp ([Shaders.ShadowsExteriors.ScreenSpace]): the one-march technique (third in SunShadows.fx.hlsl).
			static EffectRecord::CachedHandle sharpCache;
			D3DXHANDLE sharpTechnique = Effects.SunShadows->TechniqueHandle(sharpCache, "ForwardContactSharp");
			const bool sharpContact = fusedForwardContact && Effects.ShadowsExteriors->Settings.ScreenSpace.Sharp && sharpTechnique != NULL &&
				Effects.SunShadows->Effect->GetTechnique(2) == sharpTechnique;
			// UNOFFICIAL 2026-10-16 SkipWhenFaded (dawn cost): while the sun's shadows are fully faded and nothing reads these
			// passes (ShadowsExteriorEffect::FadedContactUnread), they are skipped; the buffer keeps the lamps' pass above.
			ShadowsExteriorEffect* sunShadows = Effects.ShadowsExteriors;
			const bool skipContact = sunShadows->FadedContactUnread();
			static int loggedSkip = -1;
			if ((int)skipContact != loggedSkip) {
				Logger::Log("SUN FADE STATE: contact shadows %s", skipContact ? "SKIPPED" : sunShadows->SunFaded ?
					"drawn (an effect reads them: lamp shadows outdoors, blood flow, wet world, underwater or the debug view)" : "drawn");
				loggedSkip = skipContact;
			}
			if (!skipContact) {
				// The stock technique's last pass (no fused technique in an old SunShadows.fx) would take the cascades over while
				// the forward lookups are off for the fade: it gets the forward value, as without the switch.
				const float forwardWas = sunShadows->Constants.ForwardData.x;
				if (sunShadows->SunFaded && sunShadows->Settings.Exteriors.ForwardShadows) sunShadows->Constants.ForwardData.x = 0.0f;
				// UNOFFICIAL 2026-11-06 forward Bend contact shadows (core/BendContact.h): Bend's march for next frame's lit shaders;
				// when it ran, the old pass writes "no contact" (its ScreenSpaceData.x test) so nothing is darkened twice.
				const bool bendRan = fusedForwardContact && sunShadows->Constants.ScreenSpaceData.x != 0.0f && BendContact::Produce(Device);
				if (!bendRan) BendContact::Invalidate();	// the old pass has the contact this frame: next frame's lit shaders take none
				const float screenSpaceWas = sunShadows->Constants.ScreenSpaceData.x;
				if (bendRan) sunShadows->Constants.ScreenSpaceData.x = 0.0f;
				RenderEffectToRT(sunShadows->Textures.ShadowPassSurface, Effects.SunShadows, false, sharpContact ? 2 : fusedForwardContact ? 1 : 0);
				sunShadows->Constants.ScreenSpaceData.x = screenSpaceWas;
				sunShadows->Constants.ForwardData.x = forwardWas;
				RtSunShadows::Apply(Device);	// UNOFFICIAL 2026-10-28: the traced sun visibility into the red channel (nothing when not traced)
			}
#if NVR_TEST_TOOLS
			else if (sunShadows->Settings.Exteriors.SkipWhenFadedCheck) {
				// DIAGNOSTIC SkipWhenFadedCheck: the skipped passes' buffer set to 0 (full shadow, no lamp light), so a reader the
				// checks missed changes the picture.
				Device->SetRenderTarget(0, sunShadows->Textures.ShadowPassSurface);
				Device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
			}
#endif
		}
	}
	else {
		// Nothing above ran this frame, so ShadowPassSurface keeps whatever it last
		// held -- e.g. an exterior sun-shadow composite, walked in from outdoors,
		// frozen here for as long as this branch keeps being skipped (interior with
		// Interior point-shadows off is the common case). It's still sampled
		// unconditionally by other independently-enabled effects (Specular and
		// others), so reset it to the neutral "no shadow" value rather than leaving
		// stale exterior data for them to read.
		if (!shadowBufferClear) { Effects.ShadowsExteriors->clearShadowsBuffer(); shadowBufferClear = true; }
		BendContact::Invalidate();	// UNOFFICIAL 2026-11-06: no Bend result made (the effect's UpdateConstants may not run either)
	}
}

void ShaderManager::PreStep_SunFarFactor(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-19 SunFar: the land's and the clouds' shadows as a factor the sun-shadow composites below take (outdoors,
	// while either is on; otherwise it marks the factor unused and draws nothing).
	Effects.SunFar->RenderFactor(Device, RenderTarget);
	if (Effects.SunFar->Constants.Apply.x > 0.5f) FlickerWatch::Flags |= FlickerWatch::kSunFar;	// UNOFFICIAL 2026-10-23 test only

	Device->SetRenderTarget(0, RenderTarget);
	if (Effects.SunFar->Settings.FlickerWatch && GameState.isExterior) FlickerWatch::OnProbe(FlickerWatch::World, Device, RenderTarget);	// (2026-10-25) test only
}

void ShaderManager::PreStep_ChainBegin(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* RenderedSurface = ctx.RenderedSurface;
	// Start the copy-free chain (FrameChain), or seed the rendered texture for the legacy path in
	// which every effect keeps it equal to the render target. TESR_SourceBuffer is refreshed by
	// each effect that actually samples it (EffectRecord::usesSourceBuffer). Effects are handed
	// TheTextureManager->RenderedSurface at call time because the chain swaps it.
	ctx.chainGuard.armed = true;	// (was the local ChainGuard here: ChainContext ends the chain when RenderEffectsPreTonemapping returns)
	if (!Chain.Begin(RenderTarget))
		Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
}

void ShaderManager::PreStep_MaterialCapture(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-20 MaterialAware reflections (core/ReflMaterial.h): the materials the world scene left in the scene's alpha,
	// copied out before the first effect draws over it.
	Effects.Reflections->CaptureMaterial(Device);
	Device->SetRenderTarget(0, RenderTarget);
}

void ShaderManager::PreStep_SkinScattering(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	// UNOFFICIAL the screen-space skin blur ([Shaders.Skin.Scattering] ScreenSpace, SkinScattering.h): first, on the scene exactly as
	// drawn: it recognises skin by comparing each pixel with the colour the skin shader wrote, which any effect before it would
	// change. Shadows, AO and fog then apply to the scattered skin like everything else. Nothing runs with it off.
	if (Effects.SkinScattering && Shaders.Skin && Shaders.Skin->Active()) {	// Render checks the rest (ShouldRender resolves the targets: once)
		static GpuTimer skinScatterTimer("Skin scattering");
		GpuProfileScope gpu(skinScatterTimer, Device);
		{ Effects.SkinScattering->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.SkinScattering); }	// pass 2 reads the unblurred scene
	}
}

void ShaderManager::PreStep_SkyViewTable(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL physical sky: its table for the sky shader (read next frame), first, so the clouds can use it this frame too.
	if (Effects.SkyView && Effects.SkyView->ShouldRender()) {
		static GpuTimer skyViewTimer("Physical sky table");
		GpuProfileScope gpu(skyViewTimer, Device);
		Effects.SkyView->RenderTable(Device, RenderTarget);
	}
}

void ShaderManager::PreStep_VolumetricClouds(ChainContext& ctx) {
	static GpuTimer cloudsTimer("Volumetric clouds");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	// UNOFFICIAL, optional: volumetric clouds over the sky (VolumetricClouds.h). First, so the shadow and AO applies and
	// the fog all see them as part of the sky, and the composite apply below is not affected.
	if (wouldRender(Effects.VolumetricClouds)) {
		GpuProfileScope gpu(cloudsTimer, Device);
		Effects.VolumetricClouds->RenderClouds(Device, RenderTarget, TheTextureManager->RenderedSurface, SourceSurface);
	}
}

void ShaderManager::PreStep_AmbientLightTakeover(ChainContext& ctx) {
	// UNOFFICIAL AmbientLight (AmbientLight.h): AO and bounce light in one pass. While it renders it replaces AmbientOcclusion and
	// BounceLight: both are switched off for the rest of this call (and back as they were when it returns), so every path below
	// (the merged shadow + AO apply, the bounce folded into the specular) sees them off.
	const bool ambientLight = wouldRender(Effects.AmbientLight);
	ctx.aoGuard.effect = ambientLight ? Effects.AmbientOcclusion : nullptr;	// (was the local EnabledGuard aoGuard: ChainContext restores it when the call returns)
	ctx.aoGuard.was = Effects.AmbientOcclusion->Enabled;
	ctx.bounceGuard.effect = ambientLight ? Effects.BounceLight : nullptr;	// (was the local EnabledGuard bounceGuard)
	ctx.bounceGuard.was = Effects.BounceLight->Enabled;
	if (ambientLight) Effects.AmbientOcclusion->Enabled = Effects.BounceLight->Enabled = false;
	ctx.ambientLight = ambientLight;
}

void ShaderManager::PreStep_HiZ(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	const bool ambientLight = ctx.ambientLight;
	// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz; core/HiZ.h): the Hi-Z pyramid, one compute dispatch after the depth and normals,
	// only for the readers that want it this frame ([Shaders.Reflections.Main] HiZ, [Shaders.AmbientLight.Main] HiZ, both Vulkan passes).
	// (Reflections alone builds it right before its own passes, below: built here, the dispatch slowed AmbientLight's prepare by ~0.015 ms.)
	HiZ::Build(Device, DxvkCompute::PassesOn() && ambientLight && Effects.AmbientLight->HiZWanted());
}

void ShaderManager::PreStep_CompositePlan(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	const bool forwardInterior = ctx.forwardInterior;
	const bool keepDarkening = ctx.keepDarkening;
	const bool ambientLight = ctx.ambientLight;
	// Composite apply: the exterior sun-shadow composite and the AO combine are per-pixel
	// operations on the scene, applied just before fog. When no effect that normally runs between
	// them would render this frame, the fog reconstruct applies them in the same pass (same maths,
	// same order), saving two full-resolution read/write passes of the HDR frame.
	AmbientOcclusionEffect* AO = Effects.AmbientOcclusion;
	VolumetricFogEffect* Fog = Effects.VolumetricFog;
	const bool shadowApplies = GameState.isExterior && wouldRender(Effects.ShadowsExteriors);
	const bool aoApplies = wouldRender(AO);
	const bool effectsBetween = wouldRender(Effects.SnowAccumulation) || wouldRender(Effects.WetWorld) || wouldRender(Effects.BloodFlow) ||
		wouldRender(Effects.Specular) || wouldRender(Effects.Underwater) ||
		wouldRender(Effects.BounceLight) || wouldRender(Effects.Reflections) || ambientLight; // must see the scene with its shadows and AO applied
	// UNOFFICIAL 2026-10-18 InteriorShadowFold ([Main.Main.Performance]; evidence\intshadow-LOG.md): indoors with the forward lamp
	// shadows, the darkening of what no tracked lamp reaches (KeepDarkening) was its own full-resolution pass below, unblurred
	// (no DarkeningBlur): ShadowsInteriors' dedicated last pass, a per-pixel function of the scene, the lamp mask and the depth.
	// The fog composite now does it first, with the same function (Includes/InteriorShadowApply.hlsl) and its result rounded to
	// 16-bit float as that pass's target stored it (VolumetricFog.fx CompositeFogInterior), when nothing between them reads the
	// picture: the effects above, and the Reflections surroundings cube, whose capture (RenderCubeOnly, below) reads the scene
	// even when no floor reflects. Only where that pass would take its dedicated path (the generic one blurs).
	// OFF by default (2026-10-18): the same-frame check (InteriorShadowFoldCheck) still finds a few pixels a frame next to the void
	// (ceiling gaps, the far plane: the fog's sky test) differing by a few 16-bit steps; everywhere else it matched bit for bit.
	const bool interiorFoldable = !GameState.isExterior && keepDarkening && forwardInterior &&
		!Effects.ShadowsExteriors->Settings.Interiors.Forward.DarkeningBlur && TheSettingManager->SettingsMain.Main.InteriorShadowFold &&
		!(Effects.Reflections->Enabled && Effects.Reflections->Effect && Effects.Reflections->CubeWanted()) &&
		Effects.ShadowsInteriors->DedicatedUsable(RenderTarget, TheTextureManager->RenderedSurface) && Fog->CanCompositeInterior() &&
		(TheSettingManager->SettingsMain.Main.InteriorShadowFoldRound != 0 || Fog->HalfStoreMode(Device) >= 0);	// (measured once)
	// DIAGNOSTIC InteriorShadowFoldCheck (test builds): on a check frame the old path runs and the fold is compared with it.
	bool foldCheckFrame = false;
#if NVR_TEST_TOOLS
	{
		static unsigned frame = 0;
		static int every = 0;
		if ((frame % 60) == 0) every = TheSettingManager->GetSettingI("Main.Main.Performance", "InteriorShadowFoldCheck");
		frame++;
		foldCheckFrame = every > 0 && interiorFoldable && (frame % (unsigned)every) == 0 && !effectsBetween &&
			Fog->CanComposite(aoApplies ? AO->NextResultSurface() : nullptr) && FoldCheckSurfaces(Device, TheTextureManager->RenderedSurface);
	}
#endif
	const bool interiorApplies = interiorFoldable && !foldCheckFrame;
	bool composite = (shadowApplies || aoApplies || interiorApplies) && !effectsBetween &&
		Fog->CanComposite(aoApplies ? AO->NextResultSurface() : nullptr);
	if (!GameState.isExterior) {
		static int loggedFold = -1;
		const int fold = (composite && interiorApplies) || foldCheckFrame ? 1 : 0;	// (a check frame draws the old way on purpose)
		if (fold != loggedFold) {
			loggedFold = fold;
			Logger::Log("INTERIOR SHADOW FOLD: %s", fold ? "on, the lamp darkening is applied inside the fog composite (no separate pass)" :
				!TheSettingManager->SettingsMain.Main.InteriorShadowFold ? "off (setting)" : !keepDarkening ? "off (no darkening drawn: KeepDarkening off)" :
				!forwardInterior ? "off (no forward lamp shadows this frame: the old blurred pass)" :
				Effects.ShadowsExteriors->Settings.Interiors.Forward.DarkeningBlur ? "off (DarkeningBlur)" :
				effectsBetween ? "off (an effect between it and the fog reads the picture)" :
				(Effects.Reflections->Enabled && Effects.Reflections->Effect && Effects.Reflections->CubeWanted()) ? "off (the Reflections surroundings cube reads the picture)" :
				"off (the dedicated pass or the fog composite is unavailable, or this GPU's 16-bit rounding is unknown)");
		}
	}
	// UNOFFICIAL: when effects between keep the composite out of the fog pass, the shadow and AO applies still run
	// together, in one pass right after the AO (nothing reads the scene between them unless snow accumulates).
	const bool pairApply = !composite && shadowApplies && aoApplies && !wouldRender(Effects.SnowAccumulation) &&
		Fog->CanApplyShadowAO();
	AO->deferredReady = false;
	AO->skyReady = false;	// UNOFFICIAL Skylighting v2: set again by this frame's AO render
	// UNOFFICIAL 2026-10-14: with AmbientLight on, the sun-shadow apply goes into its passes (nothing reads the scene between them
	// unless snow accumulates); if it does not render after all, the shadow pass runs after it (below).
	const bool shadowIntoAmbientLight = ambientLight && shadowApplies && !composite && !pairApply && !wouldRender(Effects.SnowAccumulation) &&
		!TheSettingManager->SettingsMain.Main.DisableCompositeApply;
	if (GameState.isExterior) {	// UNOFFICIAL 2026-10-23 FlickerWatch (test only): which pass applies the sun shadow this frame
		if (composite && shadowApplies) FlickerWatch::Flags |= FlickerWatch::kComposite;
		if (pairApply) FlickerWatch::Flags |= FlickerWatch::kPair;
		if (shadowIntoAmbientLight) FlickerWatch::Flags |= FlickerWatch::kIntoAL;
		if (!(composite && shadowApplies) && !pairApply && !shadowIntoAmbientLight) FlickerWatch::Flags |= FlickerWatch::kShadowPass;
		if (Effects.ShadowsExteriors->Constants.ShadowBlur.y > 0.5f) FlickerWatch::Flags |= FlickerWatch::kLodUpdated;
		if (GameState.isCellChanged) FlickerWatch::Flags |= FlickerWatch::kCellChanged;
		ShadowsExteriorEffect* sx = Effects.ShadowsExteriors;
		if (sx->SunGlideValid && GetTickCount64() - sx->SunGlideStart < (ULONGLONG)(1000.0f * sx->Settings.SunSmoothing.GlideSeconds))
			FlickerWatch::Flags |= FlickerWatch::kSunGlide;
		FlickerWatch::Hour = ShaderConst.GameTime.y;
		FlickerWatch::SunZ = sx->Constants.SmoothedSunDir.z;
	}
	ctx.AO = AO;
	ctx.Fog = Fog;
	ctx.shadowApplies = shadowApplies;
	ctx.aoApplies = aoApplies;
	ctx.foldCheckFrame = foldCheckFrame;
	ctx.interiorApplies = interiorApplies;
	ctx.composite = composite;
	ctx.pairApply = pairApply;
	ctx.shadowIntoAmbientLight = shadowIntoAmbientLight;
}

void ShaderManager::PreStep_ShadowApply(ChainContext& ctx) {
	static GpuTimer shadowApplyTimer("Shadow apply");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool keepDarkening = ctx.keepDarkening;
	const bool shadowApplies = ctx.shadowApplies;
	const bool foldCheckFrame = ctx.foldCheckFrame;
	const bool interiorApplies = ctx.interiorApplies;
	bool& composite = ctx.composite;	// (written by the AO step: the composite falls back)
	const bool pairApply = ctx.pairApply;
	const bool shadowIntoAmbientLight = ctx.shadowIntoAmbientLight;
	auto interiorApply = [&]() { InteriorApplyPass(ctx); };	// (was a local lambda: InteriorApplyPass above)
	{
		GpuProfileScope gpu(shadowApplyTimer, Device);
		if (GameState.isExterior) {
			if (!(composite && shadowApplies) && !pairApply && !shadowIntoAmbientLight)
				{ Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.ShadowsExteriors); }
		}
		else if (keepDarkening && !(composite && interiorApplies)) {
			// With forward shadows on, the darkening alone: unblurred unless DarkeningBlur.
#if NVR_TEST_TOOLS
			if (foldCheckFrame) Device->StretchRect(TheTextureManager->RenderedSurface, NULL, FoldCheckSceneSurface, NULL, D3DTEXF_NONE);	// the scene before it
#endif
			interiorApply();
		}
	}
}

void ShaderManager::PreStep_CoverMapUpdate(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-27 Persistent wetness and snow (CoverMap.h): the cover map's update (its own targets), before its readers
	// SnowAccumulation and WetWorld; then they draw with its technique while it holds data. Both settings off: nothing here runs.
	if (TheCoverMap && (TheCoverMap->Settings.wet || TheCoverMap->Settings.snow)) {
		static GpuTimer coverMapTimer("Cover map update");
		GpuProfileScope gpu(coverMapTimer, Device);
		TheCoverMap->RenderUpdate(Device, RenderTarget);
	}
}

void ShaderManager::PreStep_SnowAccumulation(ChainContext& ctx) {
	static GpuTimer snowAccumulationTimer("Snow accumulation");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(snowAccumulationTimer, Device);
		// UNOFFICIAL 2026-10-28 CapturedNormals: a frame already upscaled to the screen before the effects (RenderScale with LowResEffects
		// off) no longer matches the normals drawn at the internal resolution: the readers keep the depth-rebuilt normal then. (With
		// LowResEffects on the effects run in the drawn rect before the upscale, as the normals were drawn: they match.)
		if (TAAEffect::UpscaledBeforeEffects()) CapturedNormals::Data.x = 0.0f;
		// UNOFFICIAL 2026-10-28: the frame's jitter in an FSR / DLSS LowResEffects frame (0 otherwise): snow builds its world positions along
		// the jittered pixel's own ray (SnowAccumulation.fx.hlsl SnowRay), as the clouds' composite reads at it.
		{
			const D3DXVECTOR2 jitterUV = Effects.TAA && Effects.SnowAccumulation->FollowJitter ? Effects.TAA->LowResJitterUV() : D3DXVECTOR2(0.0f, 0.0f);
			Effects.SnowAccumulation->Extra.z = jitterUV.x;
			Effects.SnowAccumulation->Extra.w = jitterUV.y;
		}
		{
			Effects.SnowAccumulation->Render(Device, RenderTarget, TheTextureManager->RenderedSurface,
				Effects.SnowAccumulation->Technique(TheCoverMap ? TheCoverMap->SnowTechnique(Effects.SnowAccumulation) : 0), false, SourceSurface);	// UNOFFICIAL 2026-10-28 Look
			PixelInspector::AfterFrame(Effects.SnowAccumulation);
		}
	}
}

void ShaderManager::PreStep_AmbientOcclusion(ChainContext& ctx) {
	static GpuTimer aoTimer("Ambient occlusion");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	AmbientOcclusionEffect* AO = ctx.AO;
	VolumetricFogEffect* Fog = ctx.Fog;
	const bool shadowApplies = ctx.shadowApplies;
	const bool aoApplies = ctx.aoApplies;
	const bool interiorApplies = ctx.interiorApplies;
	bool& composite = ctx.composite;	// (written by the AO step: the composite falls back)
	const bool pairApply = ctx.pairApply;
	auto interiorApply = [&]() { InteriorApplyPass(ctx); };	// (was a local lambda: InteriorApplyPass above)
	{
		GpuProfileScope gpu(aoTimer, Device);
		AO->deferCombine = (composite || pairApply) && aoApplies;
		{ AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(AO); }
		AO->deferCombine = false;
		if (pairApply) {
			static GpuTimer mergedApplyTimer("  Shadow + AO apply (merged)");
			bool merged = false;
			if (AO->deferredReady) {
				GpuProfileScope gpuMerged(mergedApplyTimer, Device);
				Fog->compositeSkyTexture = AO->SkyTexture();	// UNOFFICIAL Skylighting v2 (null unless on)
				merged = Fog->RenderShadowAO(Device, RenderTarget, TheTextureManager->RenderedSurface, true, AO->ResultTexture());
				Fog->compositeSkyTexture = nullptr;
			}
			if (!merged) {
				// The two passes as before, in their order: shadows, then the AO (its combine alone if it was deferred).
				{ Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.ShadowsExteriors); }
				if (AO->deferredReady) {
					AO->combineOnly = true;
					{ AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(AO); }
					AO->combineOnly = false;
				}
				else { AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(AO); }
			}
			AO->deferredReady = false;
		}
		if (composite && aoApplies && !AO->deferredReady) {
			// Dedicated AO was unavailable or failed. Restore the original order before
			// its legacy path reads the scene for luminance-dependent AO strength.
			composite = false;
			if (shadowApplies)
				{ Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.ShadowsExteriors); }
			if (interiorApplies) interiorApply();	// UNOFFICIAL InteriorShadowFold: left to the fog, so run in its place
			{ AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(AO); }
		}
	}
}

void ShaderManager::PreStep_SpecularBouncePlan(ChainContext& ctx) {
	// UNOFFICIAL: the bounce light's full-resolution combine goes into the specular's, one pass for both, when nothing renders
	// between them (the wet world and the flashlight read and change the image there).
	const bool specularRenders = wouldRender(Effects.Specular) && Effects.Specular->Textures.LightSurface;
	// (Not with the bounce light's NeonSpill on: its own combine has the spill's debug views and street smoothing.)
	const bool bounceIntoSpecular = specularRenders && wouldRender(Effects.BounceLight) && !wouldRender(Effects.WetWorld) &&
		Effects.Specular->CanTakeBounce() && !Effects.BounceLight->SpillOn() &&
		!Effects.BounceLight->HardEdgesOn();
	ctx.specularRenders = specularRenders;
	ctx.bounceIntoSpecular = bounceIntoSpecular;
}

void ShaderManager::PreStep_AmbientLight(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool ambientLight = ctx.ambientLight;
	AmbientOcclusionEffect* AO = ctx.AO;
	const bool shadowIntoAmbientLight = ctx.shadowIntoAmbientLight;
	if (ambientLight) {
		// UNOFFICIAL: AO and bounce light in one pass, on the shadowed scene, where the bounce light ran.
		static GpuTimer ambientLightTimer("Ambient light (AO + bounce)");
		GpuProfileScope gpu(ambientLightTimer, Device);
		// UNOFFICIAL 2026-10-20 (skyamb) Skylighting (AmbientOcclusion.h): the AO's sky pass first, at AmbientLight's half resolution;
		// AmbientLight's Apply takes its factor.
		// UNOFFICIAL 2026-10-21 (alrocks): AmbientLight's Prepare and its normal smoothing first, so the sky test reads the smoothed normal.
		Effects.AmbientLight->RenderPrepare(Device);
		if (AmbientOcclusionEffect::SkyV2Active() && AO->Effect) {
			AO->lastLowRes = false;
			AO->RenderSkyVisibility(Device, Effects.AmbientLight->SmoothedNormals());
			Device->SetRenderTarget(0, RenderTarget);
		}
		Effects.AmbientLight->foldShadow = shadowIntoAmbientLight;
		Effects.AmbientLight->shadowApplied = false;
		{ Effects.AmbientLight->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.AmbientLight); }
		if (PixelInspector::Active && AO) AO->RtInspect();	// UNOFFICIAL 2026-11-09 (diagnostic): the RT AO / GI buffers this frame's AmbientLight read
		Effects.AmbientLight->foldShadow = false;
		if (Effects.AmbientLight->shadowApplied) FlickerWatch::Flags |= FlickerWatch::kALApplied;
		if (shadowIntoAmbientLight && !Effects.AmbientLight->shadowApplied) {
			FlickerWatch::Flags |= FlickerWatch::kShadowPass;
			{ Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.ShadowsExteriors); }
		}
	}
}

void ShaderManager::PreStep_LitProbes(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	if (Effects.SunFar->Settings.FlickerWatch && GameState.isExterior) {	// (2026-10-25) test only: after the sun-shadow / AO / AmbientLight step
		FlickerWatch::OnProbe(FlickerWatch::Lit, Device, TheTextureManager->RenderedSurface);
		Device->SetRenderTarget(0, RenderTarget);
	}
#if NVR_TEST_TOOLS
	SettingShots::CaptureStage(Device, TheTextureManager->RenderedSurface);	// UNOFFICIAL, test only: 'stage' shots (ambfix 2026-10-18)
#endif
}

void ShaderManager::PreStep_BounceLight(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool bounceIntoSpecular = ctx.bounceIntoSpecular;
	Effects.BounceLight->deferredReady = false;
	if (Effects.BounceLight->Enabled) {
		// UNOFFICIAL, optional: one bounce of screen-space indirect light, on the shadowed and AO'd scene.
		static GpuTimer bounceLightTimer("Bounce light");
		GpuProfileScope gpu(bounceLightTimer, Device);
		Effects.BounceLight->deferCombine = bounceIntoSpecular;
		{ Effects.BounceLight->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.BounceLight); }
		Effects.BounceLight->deferCombine = false;
	}
}

void ShaderManager::PreStep_BloodFlow(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		// UNOFFICIAL, optional: blood that runs and dries, and pools under bodies (BloodFlow.h). On the shadowed, AO'd scene,
		// before the wet world and the fog. Costs nothing when no blood is near.
		static GpuTimer bloodFlowTimer("Blood flow");
		GpuProfileScope gpu(bloodFlowTimer, Device);
		{ Effects.BloodFlow->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.BloodFlow); }
	}
}

void ShaderManager::PreStep_WetFlashlightSpecularUnderwater(ChainContext& ctx) {
	static GpuTimer materialEffectsTimer("Wet + light materials");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool specularRenders = ctx.specularRenders;
	{
		GpuProfileScope gpu(materialEffectsTimer, Device);
		// UNOFFICIAL 2026-11-04 Wetness Effects' reflections (after the shadows, AO and bounce light, so what wet surfaces reflect is
		// shaded as it is seen), and the wetness the HUD raindrops read next frame (as Physically-Based-Reloaded).
		{ Effects.WetnessReflections->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.WetnessReflections); }
		Effects.HudRaindrops->UpdateWetness(Device);
		Device->SetRenderTarget(0, RenderTarget);
		// UNOFFICIAL 2026-11-04 Wetness Effects ([Shaders.WetnessEffects.Status], restart): the surface shaders make the world wet in its
		// place, so WetWorld's screen pass is not drawn. Its rain tracking (UpdateConstants) still runs: the persistent wet cover, PBR's
		// rain and the heat haze read it.
		if (!(Shaders.Wetness && Shaders.Wetness->Enabled)) {
			Effects.WetWorld->Render(Device, RenderTarget, TheTextureManager->RenderedSurface,
				TheCoverMap ? TheCoverMap->WetTechnique(Effects.WetWorld, Effects.WetWorld->MainTechnique()) : Effects.WetWorld->MainTechnique(), false, SourceSurface);
			PixelInspector::AfterFrame(Effects.WetWorld);
		}
		// (2026-11-04: the flashlight and its beam shaft were drawn here; the flashlight is the deferred light now, core/FlashlightDeferred.h)
		// Specular's two quarter-resolution steps (the blocks' normals, then the light they reflect) into its own buffers,
		// then its main technique adds that to the image in the chain.
		if (specularRenders) {
			RenderEffectToRT(Effects.Specular->Textures.NormalsSurface, Effects.Specular, false, 1);
			Effects.Specular->BindExtras(Device, true);	// UNOFFICIAL 2026-10-19: sky visibility + SunFar for the light step
			RenderEffectToRT(Effects.Specular->Textures.LightSurface, Effects.Specular, false, 2);
			Effects.Specular->BindExtras(Device, false);
			Device->SetRenderTarget(0, RenderTarget);
		}
		bool specularDone = false;
		if (Effects.BounceLight->deferredReady) {
			static GpuTimer mergedCombineTimer("  Bounce + specular combine (merged)");
			{
				GpuProfileScope gpuMerged(mergedCombineTimer, Device);
				specularDone = Effects.Specular->RenderWithBounce(Device, RenderTarget, TheTextureManager->RenderedSurface,
					Effects.BounceLight->deferredTexture, Effects.BounceLight->Constants.Data, Effects.BounceLight->deferredLayout);
			}
			if (!specularDone) {
				// The two as before, in their order: the bounce light's combine (alone, from its result), then the specular.
				Effects.BounceLight->combineOnly = true;
				{ Effects.BounceLight->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.BounceLight); }
				Effects.BounceLight->combineOnly = false;
			}
			Effects.BounceLight->deferredReady = false;
		}
		if (!specularDone) { Effects.Specular->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Specular); }
		{ Effects.Underwater->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Underwater); }
	}
}

void ShaderManager::PreStep_SpillSelfTest(ChainContext& ctx) {
#if NVR_TEST_TOOLS
	EffectSelfTest::Capture(EffectSelfTest::AfterSpill, TheTextureManager->RenderedSurface);	// UNOFFICIAL, diagnostic: NeonSpillSelfTest
#endif
}

void ShaderManager::PreStep_Reflections(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		// UNOFFICIAL: screen-space reflections on the ground, of the scene with every light, shadow and highlight on it,
		// before the fog (which then hazes the reflection with the surface). Half-resolution passes into the effect's own
		// buffers, then its composite in the chain.
		static GpuTimer reflectionsTimer("Reflections (SSR)");
		GpuProfileScope gpu(reflectionsTimer, Device);
		if (wouldRender(Effects.Reflections)) {
			// UNOFFICIAL 2026-10-26 (deep/fork-hiz): the Hi-Z pyramid for the trace, once a frame (nothing new if AmbientLight's HiZ built it).
			if (DxvkCompute::PassesOn() && Effects.Reflections->HiZWanted()) HiZ::Build(Device, true);
			Effects.Reflections->RenderBuffers(Device);
			Device->SetRenderTarget(0, RenderTarget);
			Effects.Reflections->BindMaterial();	// UNOFFICIAL 2026-10-20 MaterialAware (stage 15; nothing without it)
			{ Effects.Reflections->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Reflections); }
			Effects.Reflections->ClearPassTextures();
		}
		else if (Effects.Reflections->Enabled && Effects.Reflections->Effect && Effects.Reflections->CubeWanted()) {
			// No floor reflects now, but metal and the env maps may read the surroundings cube: keep it up.
			Effects.Reflections->RenderCubeOnly(Device);
			Device->SetRenderTarget(0, RenderTarget);
		}
	}
}

void ShaderManager::PreStep_VolumetricLightGrid(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	VolumetricFogEffect* Fog = ctx.Fog;
	// UNOFFICIAL 2026-10-14 froxel grid ([Shaders.VolumetricLight.Main] Froxels): the volumetric light's grid is built before
	// the fog, which reads it (its sunlight shaded along the view) and does the volumetric light's composite in its own last
	// pass; the volumetric light below then only draws its composite if the fog could not.
	const bool lightGrid = Effects.VolumetricLight->CompositeTechnique() == 8 && wouldRender(Effects.VolumetricLight);
	if (lightGrid) {
		Effects.VolumetricLight->RenderBuffers(Device);
		Device->SetRenderTarget(0, RenderTarget);
	}
	Fog->gridTexture = lightGrid ? Effects.VolumetricLight->Textures.GridTexture : nullptr;
	Fog->gridApplied = false;
	ctx.lightGrid = lightGrid;
}

void ShaderManager::PreStep_VolumetricFog(ChainContext& ctx) {
	static GpuTimer fogTimer("Volumetric fog");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	AmbientOcclusionEffect* AO = ctx.AO;
	VolumetricFogEffect* Fog = ctx.Fog;
	const bool shadowApplies = ctx.shadowApplies;
	const bool aoApplies = ctx.aoApplies;
	const bool foldCheckFrame = ctx.foldCheckFrame;
	const bool interiorApplies = ctx.interiorApplies;
	bool& composite = ctx.composite;	// (written by the AO step: the composite falls back)
	auto interiorApply = [&]() { InteriorApplyPass(ctx); };	// (was a local lambda: InteriorApplyPass above)
	{
		GpuProfileScope gpu(fogTimer, Device);
		Fog->compositeShadow = composite && shadowApplies;
		Fog->compositeInteriorShadow = composite && interiorApplies;	// UNOFFICIAL InteriorShadowFold
		const int foldRound = TheSettingManager->SettingsMain.Main.InteriorShadowFoldRound;	// 0: as this GPU stores (HalfStoreMode)
		Fog->compositeRound = foldRound == 1 ? 1.0f : foldRound == 2 ? 2.0f : foldRound == 3 ? 0.0f : Fog->halfStoreMode == 2 ? 2.0f : 0.0f;
		Fog->compositeAO = composite && AO->deferredReady;
		Fog->compositeAOTexture = AO->ResultTexture();
		Fog->compositeSkyTexture = Fog->compositeAO ? AO->SkyTexture() : nullptr;	// UNOFFICIAL Skylighting v2 (null unless on)
		Fog->compositeApplied = false;
		{ Fog->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Fog); }
		Fog->compositeSkyTexture = nullptr;
		const bool applied = Fog->compositeApplied;
#if NVR_TEST_TOOLS
		const bool checkAO = Fog->compositeAO && applied;
		IDirect3DTexture9* checkAOTexture = Fog->compositeAOTexture;
		IDirect3DTexture9* checkSky = checkAO ? AO->SkyTexture() : nullptr;
#endif
		Fog->compositeShadow = Fog->compositeAO = Fog->compositeApplied = Fog->compositeInteriorShadow = false;
		if (composite && !applied) {
			// Failed composite fog leaves the scene untouched. Restore shadows -> AO -> fog.
			if (shadowApplies)
				{ Effects.ShadowsExteriors->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.ShadowsExteriors); }
			if (interiorApplies) interiorApply();
			if (AO->deferredReady) {
				AO->combineOnly = true;
				{ AO->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(AO); }
				AO->combineOnly = false;
			}
			{ Fog->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Fog); }
		}
#if NVR_TEST_TOOLS
		if (foldCheckFrame && (composite ? applied : !aoApplies)) InteriorFoldSelfCheck(Device, RenderTarget, SourceSurface, checkAO, checkAOTexture, checkSky);
#endif
		AO->deferredReady = false;
	}
}

void ShaderManager::PreStep_VolumetricLight(ChainContext& ctx) {
	static GpuTimer volumetricLightTimer("Volumetric light");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	VolumetricFogEffect* Fog = ctx.Fog;
	const bool lightGrid = ctx.lightGrid;
	{
		// UNOFFICIAL: volumetric sun light, after the fog (it darkens the fog's evenly lit air where the sun is shaded).
		// The half-resolution march, history and blur into the effect's own buffers, then the composite in the chain.
		GpuProfileScope gpu(volumetricLightTimer, Device);
		if (lightGrid) {
			if (!Fog->gridApplied)
				{ Effects.VolumetricLight->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 8, false, SourceSurface); PixelInspector::AfterFrame(Effects.VolumetricLight); }
		}
		else if (wouldRender(Effects.VolumetricLight)) {
			Effects.VolumetricLight->RenderBuffers(Device);
			Device->SetRenderTarget(0, RenderTarget);
			// UNOFFICIAL 2026-11-01 the report burst's VL stages (core/Diagnostics.cpp): the frame right before and after the composite
			if (Diagnostics::VLCapturing) Effects.VolumetricLight->ReportComposite(Device, false);
			{ Effects.VolumetricLight->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, Effects.VolumetricLight->CompositeTechnique(), false, SourceSurface); PixelInspector::AfterFrame(Effects.VolumetricLight); }
			if (Diagnostics::VLCapturing) Effects.VolumetricLight->ReportComposite(Device, true);
		}
		Fog->gridTexture = nullptr;
	}
}

void ShaderManager::PreStep_DustMotes(ChainContext& ctx) {
	static GpuTimer dustMotesTimer("Dust motes");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	{
		// UNOFFICIAL 2026-11-03: dust specks in the lit air (effects/DustMotes), after the fog and the sun shafts so they sit in front
		// of the lit air, before the exposure and bloom. Half-resolution pass into its own buffer, then added to the current image in
		// place (no copy). ctx.lightGrid: the shafts' froxel grid was built this frame (the sun's lit air, read when SunShare > 0).
		GpuProfileScope gpu(dustMotesTimer, Device);
		Effects.DustMotes->RenderFrame(Device, RenderTarget, ctx.lightGrid);
	}
}

void ShaderManager::PreStep_Sandstorm(ChainContext& ctx) {
	static GpuTimer sandstormTimer("Sandstorm");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		// UNOFFICIAL (from Physically-Based-Reloaded, Balebandro / Alejandro Martinez; ported again 2026-10-18, sandport): dust
		// storms, after the fog (which it thickens into dust), in HDR before the god rays, exposure and bloom, so the sun shafts shine
		// through and the dust is lit and blooms with the scene. Nothing is drawn outside a storm (SandstormEffect::ShouldRender).
		GpuProfileScope gpu(sandstormTimer, Device);
		// UNOFFICIAL 2026-10-18 (sandfast): FastPath (the default) draws the dust grid and the half-resolution streaks and glow into
		// the effect's own buffers, then its composite (technique 5) in the chain; otherwise the full-resolution original (technique 0).
		if (wouldRender(Effects.Sandstorm) && Effects.Sandstorm->UseFastPath()) {
			Effects.Sandstorm->RenderBuffers(Device);
			Device->SetRenderTarget(0, RenderTarget);
			{ Effects.Sandstorm->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 5, false, SourceSurface); PixelInspector::AfterFrame(Effects.Sandstorm); }
		}
		else { Effects.Sandstorm->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Sandstorm); }
	}
}

void ShaderManager::PreStep_GodRays(ChainContext& ctx) {
	static GpuTimer godRaysTimer("God rays");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(godRaysTimer, Device);
		{ Effects.GodRays->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, true, SourceSurface); PixelInspector::AfterFrame(Effects.GodRays); }
	}
}

void ShaderManager::PreStep_RayReconstruction(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-30 the DLSS Ray Reconstruction point ([Shaders.TAA.DLSS] RayReconstruction, off by default; effects/TAAFSR.cpp,
	// docs/next/FORK2-DLSS-RR.md): every RT composite is in the picture now, and the fog, the volumetric light, the sandstorm and the god
	// rays (drawn with this frame's jittered depth: RR resolves them with the rest); nothing of exposure, bloom, lens and LUT yet. DLSS-D
	// denoises and anti-aliases it here, and TAA's resolve after the tonemap skips the frame. Off, or whenever it cannot run this frame
	// (logged once): nothing happens.
	if (Effects.TAA) Effects.TAA->RayReconstruct(Device, RenderTarget);
}

void ShaderManager::PreStep_LumaExposureBloom(ChainContext& ctx) {
	static GpuTimer hdrTimer("Luma + exposure + bloom");
	static GpuTimer avgLumaTimer("  Average luma");
	static GpuTimer exposureTimer("  Exposure");
	static GpuTimer bloomTimer("  Bloom buffers");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(hdrTimer, Device);
		// calculate average luma for use by shaders
		if (avglumaRequired) {
			GpuProfileScope gpuLuma(avgLumaTimer, Device);
			// UNOFFICIAL 2026-10-23 (NVR DXVK): the compute version when AvgLumaCompute is on and the NVR DXVK extension is
			// present (effects\AvgLuma.cpp); otherwise, and when it fails, the D3D9 pass as before.
			// UNOFFICIAL GT7 adaptation ([Shaders.Exposure.Main] Mode 1): technique 1 of AvgLuma.fx keeps a different state in the 1x1 texel
			// (log2 adaptation levels and a colour temperature); it is cleared when the mode changes so neither technique reads the other's.
			static int lumaMode = -1;
			const int exposureMode = Effects.Exposure ? Effects.Exposure->Settings.Mode : 0;
			if (exposureMode != lumaMode) {
				if (Effects.AvgLuma->Textures.AvgLumaSurface) Device->ColorFill(Effects.AvgLuma->Textures.AvgLumaSurface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));
				lumaMode = exposureMode;
			}
			const bool computed = Effects.AvgLuma->RenderCompute(Device);
			if (!computed) {
				RenderEffectToRT(Effects.AvgLuma->Textures.AvgLumaSurface, Effects.AvgLuma, NULL, Effects.Exposure ? Effects.Exposure->Technique() : 0);
				Device->SetRenderTarget(0, RenderTarget); 	// restore device used for effects
			}
			Effects.AvgLuma->CheckCompute(Device, !computed && TheSettingManager->SettingsMain.Main.AvgLumaComputeCheck > 0 && exposureMode == 0);	// test tool, off by default
		}
		{
			GpuProfileScope gpuExposure(exposureTimer, Device);
			// UNOFFICIAL (postchain): with MaxBrightness 0 (the Night default) the pass returns delinearize(linearize(c)), which is c
			// again at the buffer's precision (c is a half float; the round trip's 1e-6 error rounds back to it): a full-screen
			// read and write for nothing. Skipped, the image is left where it is (no chain commit).
			// (UNOFFICIAL GT7 adaptation, Mode 1: its technique 1; skipped only with both its strengths at 0, Exposure::IsIdentity.)
			if (!(TheSettingManager->SettingsMain.Main.SkipIdentityExposure && Effects.Exposure->IsIdentity()))
				{ Effects.Exposure->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, Effects.Exposure->Technique(), false, SourceSurface); PixelInspector::AfterFrame(Effects.Exposure); }
		}
		{
			// 2026-11-04 local exposure (effects/LocalExposure, off by default): each neighbourhood's brightness, measured on the
			// exposed image the tone mapper gets, into its 1/16 buffer (the tone mapper reads it; the image itself is not changed).
			static GpuTimer localExposureTimer("  Local exposure");
			GpuProfileScope gpuLocal(localExposureTimer, Device);
			Effects.LocalExposure->RenderFrame(Device, RenderTarget);
		}
		{
			GpuProfileScope gpuBloom(bloomTimer, Device);
			Effects.Bloom->RenderBloomBuffer(RenderTarget);
		}
	}
}

void ShaderManager::PreStep_LensAndPreTonemapLUT(ChainContext& ctx) {
	static GpuTimer preColorTimer("Pre-tonemap lens + LUT");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(preColorTimer, Device);
		{ Effects.Lens->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Lens); }
		if (Effects.LUT->Settings.PreTonemapping)
			{ Effects.LUT->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.LUT); }
	}
}

// ---------------------------------------------------------------------------------------------------------------------------
// Post-tonemap steps (were RenderEffects' body)
// ---------------------------------------------------------------------------------------------------------------------------

void ShaderManager::PostStep_FrameQuad(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	Device->SetStreamSource(0, FrameVertex, 0, sizeof(FrameVS));
	Device->SetFVF(FrameFVF);

	// prepare device for effects
	Device->SetRenderTarget(0, RenderTarget);
}

void ShaderManager::PostStep_ChainBegin(ChainContext& ctx) {
	static GpuTimer postCopyTimer("Post-tonemap copies");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* RenderedSurface = ctx.RenderedSurface;
	// copy the source render target to both the rendered and source textures (rendered gets updated after every pass, source once per effect)
	{
		GpuProfileScope gpu(postCopyTimer, Device);
		if (!Chain.Begin(RenderTarget))
			Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	ctx.chainGuard.armed = true;	// (was the local ChainGuard here: ChainContext ends the chain when RenderEffects returns)
}

void ShaderManager::PostStep_TonemapProbes(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-26 pixel inspector: the picture out of the tonemapper, before NVR's post effects.
	PixelInspector::CaptureIf("Tonemapped (before the post effects)", TheTextureManager->RenderedSurface);
	Diagnostics::StageMark(Diagnostics::TonemapMarkName, TheTextureManager->RenderedSurface);	// UNOFFICIAL 2026-11-01 stage probe (the chain re-bases here)
	// UNOFFICIAL: the clipping view's modes 2 and 3 compare with the picture as it arrives here, straight out of the tonemapper.
	Effects.Debug->CaptureClipStart(Device, RenderTarget);
}

void ShaderManager::PostStep_FusedPassPlan(ChainContext& ctx) {
	// UNOFFICIAL 2026-10-14: neighbouring per-pixel passes in one. Coloring then the LUT (Coloring.fx.hlsl's ColoringLUT), and Cinema
	// then the sky dither (Cinema.fx.hlsl's CinemaSkyDither) when nothing renders between them (image adjust idle, no body camera).
	// Same maths; the colour is just not rounded to the buffer's precision in between.
	const bool colorLUT = renders(Effects.Coloring) && !Effects.LUT->Settings.PreTonemapping && renders(Effects.LUT) &&
		Effects.Coloring->Effect->GetTechniqueByName("ColoringLUT");
	const bool cinemaDither = renders(Effects.Cinema) && renders(Effects.SkyDither) && !renders(Effects.ImageAdjust) &&
		!renders(Effects.Bodycam) && !renders(Effects.Dream) && Effects.Cinema->Effect->GetTechniqueByName("CinemaSkyDither");
	ctx.colorLUT = colorLUT;
	ctx.cinemaDither = cinemaDither;
}

void ShaderManager::PostStep_FinalEffect(ChainContext& ctx) {
	const bool colorLUT = ctx.colorLUT;
	const bool cinemaDither = ctx.cinemaDither;
	// Name the effect that will render last so its final pass writes the game target directly and
	// the chain ends without a copy (FrameChain::SetFinalEffect). Same order as the calls below; the
	// tests are the ones EffectRecord::Render applies. A wrong guess is repaired in FrameChain::Owns.
	{
		EffectRecord* const order[] = {
			Effects.Rain, Effects.Snow, Effects.BloomLegacy, Effects.HeatHaze, Effects.VolumetricSmoke, Effects.BarrelHaze, Effects.Coloring, Effects.LUT, Effects.DepthOfField,
			Effects.MotionBlur, Effects.BloodLens, Effects.WaterLens, Effects.LowHF, Effects.DitherBuster,
			Effects.SMAA, Effects.FXAA, Effects.Sharpening, Effects.Cinema, Effects.ImageAdjust, Effects.Bodycam, Effects.Dream, Effects.SkyDither, Effects.Debug,
			TheCoverMap };	// UNOFFICIAL 2026-10-27: the cover map's debug view (PersistentDebugView), last
		EffectRecord* last = nullptr;
		for (EffectRecord* effect : order)
			if (effect && effect->Enabled && effect->Effect && effect->ShouldRender() &&
				!(colorLUT && effect == Effects.LUT) && !(cinemaDither && effect == Effects.SkyDither)) last = effect;
		if (Effects.Debug->ClipViewActive()) last = Effects.Debug; // UNOFFICIAL: the clipping view draws after everything
		Chain.SetFinalEffect(last);
	}
}

void ShaderManager::PostStep_TAA(ChainContext& ctx) {
	static GpuTimer taaTimer("TAA");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	// UNOFFICIAL TAA (upstream #78, via Physically-Based-Reloaded; off by default) first: after tonemapping, so it resolves
	// LDR values that cannot ghost as HDR highlights do, and ahead of everything below. Rain and snow are particles with no
	// depth of their own to reproject by, DoF and motion blur want the stable image as input, and the lens effects and
	// cinema overlay are fixed to the screen. TAAEffect::Render manages its own targets and syncs the frame chain first.
	{
		GpuProfileScope gpu(taaTimer, Device);
		{ Effects.TAA->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.TAA); }
	}
}

void ShaderManager::PostStep_WeatherAndLegacyBloom(ChainContext& ctx) {
	static GpuTimer weatherTimer("Weather + legacy bloom");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(weatherTimer, Device);
		{ Effects.Rain->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Rain); }
		{ Effects.Snow->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, Effects.Snow->Technique(), false, SourceSurface); PixelInspector::AfterFrame(Effects.Snow); }
		// UNOFFICIAL 2026-10-26: GPU-simulated rain / snow drops (off by default; needs NVR DXVK 1.5-particles, nothing without it)
		{
			static GpuTimer gpuParticlesTimer("GPU particles (sim + draw)");
			GpuProfileScope gpuParticles(gpuParticlesTimer, Device);
			GpuParticles::Render(Device, RenderTarget);
		}
		{ Effects.BloomLegacy->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.BloomLegacy); }
	}
}

void ShaderManager::PostStep_HeatHaze(ChainContext& ctx) {
	static GpuTimer heatHazeTimer("Heat haze");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		// UNOFFICIAL (from Physically-Based-Reloaded): distant ground shimmering on hot afternoons. Before the gun smoke,
		// so the smoke near the camera is not pulled into the shimmer of what is behind it.
		GpuProfileScope gpu(heatHazeTimer, Device);
		{ Effects.HeatHaze->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.HeatHaze); }
	}
}

void ShaderManager::PostStep_VolumetricSmoke(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// GunFX: volumetric smoke first, then the heat haze distorts the result.
	{ Effects.VolumetricSmoke->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, nullptr); PixelInspector::AfterFrame(Effects.VolumetricSmoke); }
}

void ShaderManager::PostStep_BarrelHaze(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	{ Effects.BarrelHaze->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, nullptr); PixelInspector::AfterFrame(Effects.BarrelHaze); }
}

void ShaderManager::PostStep_ColoringLUT(ChainContext& ctx) {
	static GpuTimer colorTimer("Coloring + LUT");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool colorLUT = ctx.colorLUT;
	// screenspace coloring/blurring effects get rendered last
	{
		GpuProfileScope gpu(colorTimer, Device);
		{ Effects.Coloring->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, colorLUT ? 1 : 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Coloring); }
		if (!Effects.LUT->Settings.PreTonemapping && !colorLUT)
			{ Effects.LUT->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.LUT); }
	}
}

void ShaderManager::PostStep_DepthOfField(ChainContext& ctx) {
	static GpuTimer dofTimer("Depth of field");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(dofTimer, Device);
		// Distant blur does not need the six-pass autofocus/bokeh pipeline.
		const UINT technique = !Effects.DepthOfField->Constants.Enabled && Effects.DepthOfField->Constants.Blur.x ? 1 : 0;
		// The distant-only technique reads TESR_RenderedBuffer and never TESR_SourceBuffer, so it
		// skips the full-resolution copy the full pipeline needs.
		{ Effects.DepthOfField->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, technique, false, technique == 1 ? nullptr : SourceSurface); PixelInspector::AfterFrame(Effects.DepthOfField); }
	}
}

void ShaderManager::PostStep_MotionBlur(ChainContext& ctx) {
	static GpuTimer motionBlurTimer("Motion blur");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(motionBlurTimer, Device);
		{ Effects.MotionBlur->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.MotionBlur); }
	}
}

void ShaderManager::PostStep_LensOverlays(ChainContext& ctx) {
	static GpuTimer lensTimer("Lens overlays");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	// lens effects
	{
		GpuProfileScope gpu(lensTimer, Device);
		{ Effects.BloodLens->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.BloodLens); }
		{ Effects.WaterLens->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.WaterLens); }
		{ Effects.LowHF->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.LowHF); }
	}
}

void ShaderManager::PostStep_DitherBuster(ChainContext& ctx) {
	static GpuTimer ditherTimer("Dither buster");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(ditherTimer, Device);
		{ Effects.DitherBuster->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, Effects.DitherBuster->TechniqueIndex(), false, SourceSurface); PixelInspector::AfterFrame(Effects.DitherBuster); }
	}
}

void ShaderManager::PostStep_SMAA(ChainContext& ctx) {
	static GpuTimer smaaTimer("SMAA");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(smaaTimer, Device);
		{ Effects.SMAA->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.SMAA); }
	}
}

void ShaderManager::PostStep_FXAA(ChainContext& ctx) {
	static GpuTimer fxaaTimer("FXAA");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(fxaaTimer, Device);
		{ Effects.FXAA->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.FXAA); }
	}
}

void ShaderManager::PostStep_Sharpening(ChainContext& ctx) {
	static GpuTimer sharpenTimer("Sharpening");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		GpuProfileScope gpu(sharpenTimer, Device);
		{ Effects.Sharpening->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Sharpening); }
	}
}

void ShaderManager::PostStep_Cinema(ChainContext& ctx) {
	static GpuTimer cinemaTimer("Cinema");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool cinemaDither = ctx.cinemaDither;
	{
		GpuProfileScope gpu(cinemaTimer, Device);
		{ Effects.Cinema->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, cinemaDither ? 1 : 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Cinema); }
	}
}

void ShaderManager::PostStep_ImageAdjust(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	GpuTimer& imageAdjustTimer = *ctx.imageAdjustTimer;	// (one timer for two steps: declared in RenderEffects)
	{
		GpuProfileScope gpu(imageAdjustTimer, Device);
		{ Effects.ImageAdjust->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.ImageAdjust); }
	}
}

void ShaderManager::PostStep_Bodycam(ChainContext& ctx) {
	static GpuTimer bodycamTimer("Bodycam");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		// UNOFFICIAL, testbed: the body-camera look, on the finished picture (the sky dither still comes after it).
		GpuProfileScope gpu(bodycamTimer, Device);
		{ Effects.Bodycam->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Bodycam); }
	}
}

void ShaderManager::PostStep_Dream(ChainContext& ctx) {
	static GpuTimer dreamTimer("Dream");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	{
		// The "fun" trip look (src/effects/Dream, off by default), on the finished picture after the body camera (the sky dither still
		// comes after it). Its own Render runs one or two passes (Dream.cpp).
		GpuProfileScope gpu(dreamTimer, Device);
		{ Effects.Dream->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Dream); }
	}
}

void ShaderManager::PostStep_SkyDither(ChainContext& ctx) {
	static GpuTimer skyDitherTimer("Sky dither");
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	const bool cinemaDither = ctx.cinemaDither;
	{
		// UNOFFICIAL: the last colour change before the 8-bit screen, so its noise breaks up the sky's bands exactly
		// where they form (anything after it, e.g. sharpening, would amplify the noise).
		GpuProfileScope gpu(skyDitherTimer, Device);
		if (!cinemaDither) { Effects.SkyDither->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.SkyDither); }
	}
}

void ShaderManager::PostStep_SunDepthFitView(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	// UNOFFICIAL 2026-10-27 FitToDepthDebug ([Shaders.ShadowsExteriors.Main], core\SunDepthFit.h): the sun cascades coloured, or the
	// measured blocks, over the finished picture. 0 (the default): nothing runs.
	if (SunDepthFit::DebugView > 0) SunDepthFit::RenderView(Device, RenderTarget);
}

void ShaderManager::PostStep_RtSunDebug(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	RtSunShadows::RenderDebug(Device, RenderTarget);	// UNOFFICIAL 2026-10-28 [Shaders.ShadowsExteriors.RT] DebugView 4 (nothing otherwise)
	RtPathTrace::RenderDebug(Device, RenderTarget);	// UNOFFICIAL 2026-11-02 [Main.Main.RayTracing] ReferencePT (nothing otherwise)
}

void ShaderManager::PostStep_Debug(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	GpuTimer& imageAdjustTimer = *ctx.imageAdjustTimer;	// (one timer for two steps: declared in RenderEffects)
	{
		GpuProfileScope gpu(imageAdjustTimer, Device);
		{ Effects.Debug->Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 0, false, SourceSurface); PixelInspector::AfterFrame(Effects.Debug); }
		Effects.Debug->RenderClipView(Device, RenderTarget); // UNOFFICIAL: [Shaders.Debug.Main] ClipView
	}
}

void ShaderManager::PostStep_CoverMapDebug(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	IDirect3DSurface9* SourceSurface = ctx.SourceSurface;
	if (TheCoverMap) TheCoverMap->RenderDebug(Device, RenderTarget, SourceSurface);	// UNOFFICIAL 2026-10-27 (PersistentDebugView; does nothing when 0)
}

void ShaderManager::PostStep_FlickerWatch(ChainContext& ctx) {
	IDirect3DDevice9* Device = ctx.Device;
	IDirect3DSurface9* RenderTarget = ctx.RenderTarget;
	if ((Effects.SunFar->Settings.FlickerWatch && GameState.isExterior) || FlickerWatch::Forced)	// UNOFFICIAL 2026-10-23 test only (core\FlickerWatch.h); Forced: shot list 'flickerwatch' (2026-10-25)
		FlickerWatch::OnFrame(Device, RenderTarget, Effects.SunFar->Settings.FlickerThreshold);
	else FlickerWatch::Flags = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The order: one table per stage. A new effect is a step function above plus a line here.
// ---------------------------------------------------------------------------------------------------------------------------

static const EffectChain::Step PreTonemapSteps[] = {
	{ "FrameQuad", EffectChain::PreTonemap, &ShaderManager::PreStep_FrameQuad },
	{ "SceneInProbes", EffectChain::PreTonemap, &ShaderManager::PreStep_SceneInProbes },
	{ "DepthAndNormals", EffectChain::PreTonemap, &ShaderManager::PreStep_DepthAndNormals },
	{ "ObjectMotion", EffectChain::PreTonemap, &ShaderManager::PreStep_ObjectMotion },
	{ "LampVisibility", EffectChain::PreTonemap, &ShaderManager::PreStep_LampVisibility },
	{ "DepthNormalProbes", EffectChain::PreTonemap, &ShaderManager::PreStep_DepthNormalProbes },
	{ "SunDepthFit", EffectChain::PreTonemap, &ShaderManager::PreStep_SunDepthFit },
	{ "OrthoProbe", EffectChain::PreTonemap, &ShaderManager::PreStep_OrthoProbe },
	{ "ShadowPlan", EffectChain::PreTonemap, &ShaderManager::PreStep_ShadowPlan },
	{ "RtCore", EffectChain::PreTonemap, &ShaderManager::PreStep_RtCore },
	{ "ShadowPasses", EffectChain::PreTonemap, &ShaderManager::PreStep_ShadowPasses },
	{ "SunFarFactor", EffectChain::PreTonemap, &ShaderManager::PreStep_SunFarFactor },
	{ "ChainBegin", EffectChain::PreTonemap, &ShaderManager::PreStep_ChainBegin },
	{ "MaterialCapture", EffectChain::PreTonemap, &ShaderManager::PreStep_MaterialCapture },
	{ "SkinScattering", EffectChain::PreTonemap, &ShaderManager::PreStep_SkinScattering },
	{ "SkyViewTable", EffectChain::PreTonemap, &ShaderManager::PreStep_SkyViewTable },
	{ "VolumetricClouds", EffectChain::PreTonemap, &ShaderManager::PreStep_VolumetricClouds },
	{ "AmbientLightTakeover", EffectChain::PreTonemap, &ShaderManager::PreStep_AmbientLightTakeover },
	{ "HiZ", EffectChain::PreTonemap, &ShaderManager::PreStep_HiZ },
	{ "CompositePlan", EffectChain::PreTonemap, &ShaderManager::PreStep_CompositePlan },
	{ "ShadowApply", EffectChain::PreTonemap, &ShaderManager::PreStep_ShadowApply },
	{ "CoverMapUpdate", EffectChain::PreTonemap, &ShaderManager::PreStep_CoverMapUpdate },
	{ "SnowAccumulation", EffectChain::PreTonemap, &ShaderManager::PreStep_SnowAccumulation },
	{ "AmbientOcclusion", EffectChain::PreTonemap, &ShaderManager::PreStep_AmbientOcclusion },
	{ "SpecularBouncePlan", EffectChain::PreTonemap, &ShaderManager::PreStep_SpecularBouncePlan },
	{ "AmbientLight", EffectChain::PreTonemap, &ShaderManager::PreStep_AmbientLight },
	{ "LitProbes", EffectChain::PreTonemap, &ShaderManager::PreStep_LitProbes },
	{ "BounceLight", EffectChain::PreTonemap, &ShaderManager::PreStep_BounceLight },
	{ "BloodFlow", EffectChain::PreTonemap, &ShaderManager::PreStep_BloodFlow },
	{ "WetFlashlightSpecularUnderwater", EffectChain::PreTonemap, &ShaderManager::PreStep_WetFlashlightSpecularUnderwater },
	{ "SpillSelfTest", EffectChain::PreTonemap, &ShaderManager::PreStep_SpillSelfTest },
	{ "Reflections", EffectChain::PreTonemap, &ShaderManager::PreStep_Reflections },
	{ "VolumetricLightGrid", EffectChain::PreTonemap, &ShaderManager::PreStep_VolumetricLightGrid },
	{ "VolumetricFog", EffectChain::PreTonemap, &ShaderManager::PreStep_VolumetricFog },
	{ "VolumetricLight", EffectChain::PreTonemap, &ShaderManager::PreStep_VolumetricLight },
	{ "DustMotes", EffectChain::PreTonemap, &ShaderManager::PreStep_DustMotes },
	{ "Sandstorm", EffectChain::PreTonemap, &ShaderManager::PreStep_Sandstorm },
	{ "GodRays", EffectChain::PreTonemap, &ShaderManager::PreStep_GodRays },
	{ "RayReconstruction", EffectChain::PreTonemap, &ShaderManager::PreStep_RayReconstruction },
	{ "LumaExposureBloom", EffectChain::PreTonemap, &ShaderManager::PreStep_LumaExposureBloom },
	{ "LensAndPreTonemapLUT", EffectChain::PreTonemap, &ShaderManager::PreStep_LensAndPreTonemapLUT },
};

static const EffectChain::Step PostTonemapSteps[] = {
	{ "FrameQuad", EffectChain::PostTonemap, &ShaderManager::PostStep_FrameQuad },
	{ "ChainBegin", EffectChain::PostTonemap, &ShaderManager::PostStep_ChainBegin },
	{ "TonemapProbes", EffectChain::PostTonemap, &ShaderManager::PostStep_TonemapProbes },
	{ "FusedPassPlan", EffectChain::PostTonemap, &ShaderManager::PostStep_FusedPassPlan },
	{ "FinalEffect", EffectChain::PostTonemap, &ShaderManager::PostStep_FinalEffect },
	{ "TAA", EffectChain::PostTonemap, &ShaderManager::PostStep_TAA },
	{ "WeatherAndLegacyBloom", EffectChain::PostTonemap, &ShaderManager::PostStep_WeatherAndLegacyBloom },
	{ "HeatHaze", EffectChain::PostTonemap, &ShaderManager::PostStep_HeatHaze },
	{ "VolumetricSmoke", EffectChain::PostTonemap, &ShaderManager::PostStep_VolumetricSmoke },
	{ "BarrelHaze", EffectChain::PostTonemap, &ShaderManager::PostStep_BarrelHaze },
	{ "ColoringLUT", EffectChain::PostTonemap, &ShaderManager::PostStep_ColoringLUT },
	{ "DepthOfField", EffectChain::PostTonemap, &ShaderManager::PostStep_DepthOfField },
	{ "MotionBlur", EffectChain::PostTonemap, &ShaderManager::PostStep_MotionBlur },
	{ "LensOverlays", EffectChain::PostTonemap, &ShaderManager::PostStep_LensOverlays },
	{ "DitherBuster", EffectChain::PostTonemap, &ShaderManager::PostStep_DitherBuster },
	{ "SMAA", EffectChain::PostTonemap, &ShaderManager::PostStep_SMAA },
	{ "FXAA", EffectChain::PostTonemap, &ShaderManager::PostStep_FXAA },
	{ "Sharpening", EffectChain::PostTonemap, &ShaderManager::PostStep_Sharpening },
	{ "Cinema", EffectChain::PostTonemap, &ShaderManager::PostStep_Cinema },
	{ "ImageAdjust", EffectChain::PostTonemap, &ShaderManager::PostStep_ImageAdjust },
	{ "Bodycam", EffectChain::PostTonemap, &ShaderManager::PostStep_Bodycam },
	{ "Dream", EffectChain::PostTonemap, &ShaderManager::PostStep_Dream },
	{ "SkyDither", EffectChain::PostTonemap, &ShaderManager::PostStep_SkyDither },
	{ "SunDepthFitView", EffectChain::PostTonemap, &ShaderManager::PostStep_SunDepthFitView },
	{ "RtSunDebug", EffectChain::PostTonemap, &ShaderManager::PostStep_RtSunDebug },
	{ "Debug", EffectChain::PostTonemap, &ShaderManager::PostStep_Debug },
	{ "CoverMapDebug", EffectChain::PostTonemap, &ShaderManager::PostStep_CoverMapDebug },
	{ "FlickerWatch", EffectChain::PostTonemap, &ShaderManager::PostStep_FlickerWatch },
};

void EffectChain::Run(ShaderManager* manager, Stage stage, ChainContext& ctx) {
	const Step* steps = stage == PreTonemap ? PreTonemapSteps : PostTonemapSteps;
	const size_t count = stage == PreTonemap ? _countof(PreTonemapSteps) : _countof(PostTonemapSteps);
	for (size_t i = 0; i < count && !ctx.stop; i++) {
		SessionLog::StageScope stage(steps[i].name);	// UNOFFICIAL 2026-11-07 (perf-ptcost): the step's name for the CRASH and STALL lines
		(manager->*steps[i].function)(ctx);
	}
}
