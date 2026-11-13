#include "AmbientOcclusion.h"
#include "../../core/RtPathTrace.h"	// UNOFFICIAL 2026-11-03 PTReplace (Replacing)
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "../../core/GpuProfiler.h"
#include "../../core/FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/RtAmbient.h"
#include "../../core/ManyLamps.h"
#include "../PBR/PBR.h"	// UNOFFICIAL 2026-11-04 (rtgap) the game's light scale	// UNOFFICIAL 2026-11-03 (rtspec) GIGameLamps: the game's lamp table
#include "../../core/RtCore.h"
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-11-12 (sungi p4) the GI denoiser's guide: AlbedoTexture
#include "../../core/RtDenoise.h"	// (2026-10-29) the shared RT denoiser	// (the RT core: AddUser / CopyIn / AddPass / CopyOut)	// UNOFFICIAL 2026-10-28 (fork RT) [Shaders.AmbientOcclusion.RT]
#include "../../core/Upscaler.h"	// UNOFFICIAL 2026-10-30 DLSS RR (RayReconstructionWantsRaw: the RT temporal pass is skipped)
#include "../../core/PixelInspector.h"	// UNOFFICIAL 2026-11-09 RtInspect
#include "../BounceLight/BounceLightMath.h"	// (the RT AO's temporal pass)

// UNOFFICIAL 2026-10-16 GTAO ([Shaders.AmbientOcclusion.GTAO]; AmbientOcclusion.fx.hlsl GTAOValue): x 1 on (the DedicatedGTAO
// technique instead of DedicatedAO), w largest search radius in pixels. Kept out of AmbientOcclusion.h.
static D3DXVECTOR4 GTAOData(0.0f, 0.0f, 0.0f, 64.0f);
static EffectRecord::CachedHandle techGTAO;
// UNOFFICIAL 2026-10-17 Skylighting ([Shaders.AmbientOcclusion.Skylighting]; AmbientOcclusion.fx.hlsl SkyVisibility): x 1 on (exteriors
// only, set each frame), y strength, z ray length in units, w depth slack in ortho map units. Needs the ortho map, which is asked for here.
static D3DXVECTOR4 SkyData(0.0f, 1.0f, 600.0f, 0.0001f);
static bool SkyEnabled = false;
// UNOFFICIAL 2026-10-17 Skylighting v2 ([Shaders.AmbientOcclusion.Skylighting] Version 2; deep roadmap 3.4, evidence skylight2-LOG.md).
// Its own pass (SkyVisibilityV2) at the AO's resolution works out the factor for the SKY's share of each pixel's light only and
// writes it to a target the size of the AO's; the composites (VolumetricFog.fx CompositeAO, AmbientOcclusion.fx DedicatedCombine)
// multiply by it, upsampled with the AO's own weights (no new constants: the fog's composite pass has c219-c223 left with the
// physical sky). v1 multiplied the AO, i.e. the whole pixel with its sunlight. Sky2: x 1 on (exteriors, AO on), y strength, z the
// sky (ambient) light's luminance (linear, x AmbientScale), w the sun's (linear). Sky3: x 1 normal from the depth, y 1 / OpenThreshold,
// z 1 share apply (0 = on the whole pixel, for A/B), w debug view (1 visibility, 2 sky share, 3 final factor, 4 covered = 0). Sky4: x ray length (units), y soft width of the map
// test (units), z 1 = the sun shadow mask is bound for the pass (stage 8), w 1 = the factor is bound for DedicatedCombine (stage 7).
static D3DXVECTOR4 SkyData2(0.0f, 1.0f, 0.0f, 0.0f);
static D3DXVECTOR4 SkyData3(0.0f, 0.0f, 1.0f, 0.0f);
static D3DXVECTOR4 SkyData4(800.0f, 40.0f, 0.0f, 0.0f);
static float SkyAmbientScale = 1.0f;
// UNOFFICIAL 2026-10-20 (skyamb; user: under a neon canopy at dusk the lamp-lit wall and the glowing bulbs went nearly black): the share
// (Includes/SkyShare.hlsl) knows only the sun and the sky, so in shade everything counts as sky light and a pixel under a wide cover
// (V ~ 0) lost all of it, lamps and glow included. [Shaders.AmbientOcclusion.Skylighting] LightCap: the composites take away at most
// (1 - V) x LightCap x Strength x the sky's light on a white surface (linear luminance of TESR_SunAmbient x AmbientScale), whatever the share says,
// so light the sky cannot explain (lamps, glow, emissive) stays. Sky5: x that light (linear luminance), w 0; LightCap 0 or a debug
// view: x 0, w 1e6 (no limit: the factor as before). Needs the sky target's b (the visibility): A16B16G16R16F while it is on.
static D3DXVECTOR4 SkyData5(0.0f, 0.0f, 0.0f, 1.0e6f);
static float SkyLightCap = 0.0f;
static float SkyOpenSides = 0.0f;	// UNOFFICIAL 2026-10-20 (skyamb) OpenSides (Sky5.y; Includes/SkyVisibility.hlsl)
static int SkyVersion = 2;
// UNOFFICIAL 2026-10-28 (fork RT) [Shaders.AmbientOcclusion.RT] (core/RtAmbient.h): the RT AO's temporal pass (AmbientOcclusionTemporalRT.frag),
// AmbientLight's Accumulate rule: RtReproject this frame's view space -> last frame's clip (BounceLightMath.h); RtTemporal x last frame's
// share (0.9, 0 without a usable history), y the motion in AO pixels that halves it, z the frame number, w the RT DebugView (the RT
// passes read it).
static D3DXMATRIX RtReproject;
static D3DXVECTOR4 RtTemporal(0.0f, 4.0f, 0.0f, 0.0f);
static D3DXMATRIX RtLastView, RtLastInvView, RtLastProj;
static double RtLastTime = -1.0;
static bool RtHistoryValid = false;
static UInt32 RtFrame = 0;
// UNOFFICIAL 2026-11-03 RT AO at RenderScale < 1: the RenderRect scale the history was made at (1, 1 in native frames); a history made at
// another scale (or rect vs native) holds other texels and starts again (AmbientLight's rectSame rule). The shaders' cb.rtRect.zw.
static float RtHistoryScale[2] = { 1.0f, 1.0f };
static float RtRectScaleX() { return RenderRect::On ? RenderRect::ScaleX : 1.0f; }
// UNOFFICIAL 2026-11-11 (sungi, F16) true on the one frame the player's place changes between interior and exterior or to another worldspace
// (the parent cell's), or the sun's direction jumps more than 2 degrees in a frame (a time skip, a weather-start snap; ShadowsExteriors
// SmoothedSunDir moves in small steps otherwise). The RT GI reservoirs and both RT denoisers (AO, GI) start over on it: the frame of a door
// or a load must not blend the other space's light in. Once a frame (frame-number guard), so every caller sees the same answer.
static bool RtPlaceRestart() {
	static UInt32 frame = 0xFFFFFFFF;
	static bool result = false, known = false, lastExterior = false, haveSun = false;
	static const void* lastWorld = nullptr;
	static D3DXVECTOR3 lastSun(0.0f, 0.0f, 1.0f);
	const UInt32 now = TheFrameRateManager->FrameNumber;
	if (frame == now) return result;
	frame = now;
	const bool exterior = TheShaderManager->GameState.isExterior;
	const void* world = Player ? (const void*)Player->GetWorldSpace() : nullptr;
	bool restart = known && (exterior != lastExterior || world != lastWorld);
	known = true; lastExterior = exterior; lastWorld = world;
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (shadows && exterior) {
		D3DXVECTOR3 sun(shadows->Constants.SmoothedSunDir.x, shadows->Constants.SmoothedSunDir.y, shadows->Constants.SmoothedSunDir.z);
		const float len = D3DXVec3Length(&sun);
		if (len > 0.0f) {
			sun /= len;
			if (haveSun && D3DXVec3Dot(&sun, &lastSun) < cosf(D3DXToRadian(2.0f))) restart = true;
			lastSun = sun; haveSun = true;
		}
	} else haveSun = false;
	if (restart) Logger::Log("UNOFFICIAL RT AO: GI reservoirs and RT denoisers start over (place or sun direction changed).");
	return result = restart;
}
static float RtRectScaleY() { return RenderRect::On ? RenderRect::ScaleY : 1.0f; }

static float LinearLuma(const D3DXVECTOR4& c) {
	return 0.2126f * powf(max(c.x, 0.0f), 2.2f) + 0.7152f * powf(max(c.y, 0.0f), 2.2f) + 0.0722f * powf(max(c.z, 0.0f), 2.2f);
}

bool AmbientOcclusionEffect::SkyV2Active() { return SkyData2.x > 0.0f; }
// UNOFFICIAL 2026-10-20 (skyamb) LightCap (SkyData5, Includes/SkyTake.hlsl): (the most light the sky may take: LightCap x Strength x the
// sky's linear luminance x AmbientScale (SkyData2.z), no limit).
D3DXVECTOR2 AmbientOcclusionEffect::SkyTakeComposite() {
	const bool limit = SkyLightCap > 0.0f && SkyData3.w < 0.5f;	// (debug views: the raw values)
	return limit ? D3DXVECTOR2(SkyLightCap * SkyData2.y * SkyData2.z, 0.0f) : D3DXVECTOR2(0.0f, 1.0e6f);
}
// UNOFFICIAL 2026-10-20 (skyamb): AmbientLight renders this frame (its ShouldRender: on, loaded, not under water) and hosts the sky test
// (this effect's file loaded: the sky pass and the sky target are this effect's).
bool AmbientOcclusionEffect::SkyHostedByAmbientLight() {
	AmbientLightEffect* al = TheShaderManager->Effects.AmbientLight;
	AmbientOcclusionEffect* ao = TheShaderManager->Effects.AmbientOcclusion;
	return al && al->ShouldRender() && ao && ao->Effect;
}
// UNOFFICIAL 2026-10-17 FastPaths ([Shaders.AmbientOcclusion.FastPaths] Enabled): the DedicatedAOFast technique = DedicatedAO with
// its far-pixel early returns made real (the effect compiler had flattened them: sky and distant land ran the whole kernel and
// both blurs). Same picture. With GTAO on it picks DedicatedGTAOFast instead. Off, or an effect file without the technique:
// DedicatedAO / DedicatedGTAO as before.
static bool FastPaths = false;
static EffectRecord::CachedHandle techFast, techGTAOFast;

void AmbientOcclusionEffect::UpdateConstants() {
	UpdateSkyConstants();
}

// UNOFFICIAL 2026-10-20 (skyamb): the sky part of UpdateConstants, also run by ShaderManager every frame this effect is off. With
// AmbientLight rendering, the sky test runs for it (Version 2 whatever Version says: Version 1 darkens through this effect's own AO,
// which does not run then), whatever this effect's Enabled settings are; otherwise as before (this effect on, its Enabled setting).
void AmbientOcclusionEffect::UpdateSkyConstants() {
	const bool hosted = SkyHostedByAmbientLight();
	const bool sky = SkyEnabled && TheShaderManager->GameState.isExterior && (hosted || (Enabled && Constants.Enabled));
	SkyData.x = sky && !hosted && SkyVersion == 1 ? 1.0f : 0.0f;
	SkyData2.x = sky && (hosted || SkyVersion == 2) ? 1.0f : 0.0f;
	SkyData2.z = LinearLuma(TheShaderManager->ShaderConst.sunAmbient) * SkyAmbientScale;	// (Includes/SkyShare.hlsl SkyShare)
	SkyData2.w = LinearLuma(TheShaderManager->ShaderConst.sunColor);
	{	// UNOFFICIAL 2026-10-20 (skyamb) LightCap (above)
		const D3DXVECTOR2 take = SkyTakeComposite();
		// z: UNOFFICIAL 2026-11-08 (alsky) SkySoftEdges, read only by the RT sky pass (AmbientOcclusionSkyRT.frag: open toward endFade)
		SkyData5 = D3DXVECTOR4(take.x, SkyOpenSides, RtAmbient::Current.SkySoftEdges ? 1.0f : 0.0f, take.y);
	}
	if (SkyData.x > 0.0f) TheShaderManager->orthoRequired = true;	// UNOFFICIAL Skylighting v1 reads the ortho map
	if (SkyData2.x > 0.0f && !RtAmbient::SkyReady()) TheShaderManager->orthoSkyRequired = true;	// v2: amortised + clean map (ShadowManager); not for the RT sky (2026-10-28)
	static bool skyWasActive = false;
	TheShaderManager->orthoSkyFresh = SkyData2.x > 0.0f && !skyWasActive;
	skyWasActive = SkyData2.x > 0.0f;
}

void AmbientOcclusionEffect::RegisterConstants() {
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionAOData"] = &Constants.AOData;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionData"] = &Constants.Data;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionGTAO"] = &GTAOData;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionSky"] = &SkyData;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionSky2"] = &SkyData2;	// UNOFFICIAL Skylighting v2
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionSky3"] = &SkyData3;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionSky4"] = &SkyData4;
	TheShaderManager->ConstantsTable["TESR_AmbientOcclusionSky5"] = &SkyData5;	// UNOFFICIAL 2026-10-20 (skyamb) LightCap
}

void AmbientOcclusionEffect::RegisterTextures() {
	const int width = MaxQuality::Reduced(TheRenderManager->width, 2);
	const int height = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("NVR_AOBuffer0", &aoTexture[0], &aoSurface[0], width, height, D3DFMT_G16R16F);
	TheTextureManager->InitTexture("NVR_AOBuffer1", &aoTexture[1], &aoSurface[1], width, height, D3DFMT_G16R16F);
	const int lowWidth = MaxQuality::Reduced(TheRenderManager->width, 4);
	const int lowHeight = MaxQuality::Reduced(TheRenderManager->height, 4);
	TheTextureManager->InitTexture("NVR_AOBufferLow0", &aoTextureLow[0], &aoSurfaceLow[0], lowWidth, lowHeight, D3DFMT_G16R16F);
	TheTextureManager->InitTexture("NVR_AOBufferLow1", &aoTextureLow[1], &aoSurfaceLow[1], lowWidth, lowHeight, D3DFMT_G16R16F);
	// UNOFFICIAL Skylighting v2: the factor at the AO's resolution (half, or quarter with AOLowRes); only written while v2 is on.
	// fixes-b #3: NOT created here (about 4.4 MiB, 17.6 MiB with Max Quality, for nothing while Skylighting is off): the one the
	// first v2 draw needs is created in RenderSkyVisibility.
}

bool AmbientOcclusionEffect::UseLowRes() const {
	return TheSettingManager->SettingsMain.Main.AOLowRes &&
		aoTextureLow[0] && aoTextureLow[1] && aoSurfaceLow[0] && aoSurfaceLow[1];
}

void AmbientOcclusionEffect::UpdateSettings() {
	const char* sectionName = TheShaderManager->GameState.isExterior?"Shaders.AmbientOcclusion.Exteriors":"Shaders.AmbientOcclusion.Interiors";

	Constants.Enabled = TheSettingManager->GetSettingI(sectionName, "Enabled");
	Constants.AOData.x = TheSettingManager->GetSettingF(sectionName, "Samples");
	Constants.AOData.y = TheSettingManager->GetSettingF(sectionName, "StrengthMultiplier");
	Constants.AOData.z = TheSettingManager->GetSettingF(sectionName, "ClampStrength");
	Constants.AOData.w = TheSettingManager->GetSettingF(sectionName, "Range");
	Constants.Data.x = TheSettingManager->GetSettingF(sectionName, "AngleBias");
	Constants.Data.y = TheSettingManager->GetSettingF(sectionName, "LumThreshold");
	Constants.Data.z = TheSettingManager->GetSettingF(sectionName, "BlurDropThreshold");
	Constants.Data.w = TheSettingManager->GetSettingF(sectionName, "BlurRadiusMultiplier");
	GTAOData.x = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.GTAO", "Enabled") ? 1.0f : 0.0f;
	GTAOData.w = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.GTAO", "MaxRadius"), 4.0f, 256.0f);
	SkyEnabled = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.Skylighting", "Enabled");
	FastPaths = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.FastPaths", "Enabled") != 0;
	SkyData.y = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "Strength"), 0.0f, 1.0f);
	SkyData.z = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "Distance"), 64.0f, 4000.0f);
	SkyVersion = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.Skylighting", "Version") == 1 ? 1 : 2;
	SkyReflections = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.Skylighting", "Reflections") != 0;	// UNOFFICIAL 2026-10-19
	SkyMinReflection = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "MinReflection"), 0.0f, 1.0f);
	SkyLightCap = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "LightCap"), 0.0f, 10.0f);
	SkyOpenSides = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "OpenSides"), 0.0f, 1.0f);
	SkyData2.y = SkyData.y;
	SkyData3.w = (float)std::clamp(TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.Skylighting", "DebugView"), 0, 4);
	SkyAmbientScale = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "AmbientScale"), 0.0f, 10.0f);
	SkyData3.z = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.Skylighting", "ShareApply") ? 1.0f : 0.0f;
	SkyData3.x = TheSettingManager->GetSettingI("Shaders.AmbientOcclusion.Skylighting", "DepthNormals") ? 1.0f : 0.0f;
	SkyData3.y = 1.0f / std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "OpenThreshold"), 0.5f, 1.0f);
	SkyData4.x = SkyData.z;
	SkyData4.y = std::clamp(TheSettingManager->GetSettingF("Shaders.AmbientOcclusion.Skylighting", "SoftWidth"), 1.0f, 500.0f);
	// UNOFFICIAL 2026-10-28 (fork RT): [Shaders.AmbientOcclusion.RT] (AmbientLight reads it too). Its DebugView 2 (the RT sky visibility)
	// shows through Skylighting's view 1 (the visibility), so the composites draw it as they draw the ortho version's.
	RtAmbient::UpdateSettings();
	if (RtAmbient::Current.Enabled && RtAmbient::Current.DebugView == 2) SkyData3.w = 1.0f;
}

/*
* UNOFFICIAL 2026-10-25 (NVR DXVK, [Main.Main.Performance] NvrDxvkPasses): the Dedicated techniques' passes (Estimate, BlurX, BlurY,
* Combine; the Fast and GTAO variants by the same shaders: the Fast ones compute the same picture) and SkyVisibilityV2 as Vulkan
* passes (Compute\AmbientOcclusion*.frag, the .fx.hlsl line for line): the estimate, the blurs and the sky pass into the effect's own
* targets (ExtDrawTo), the combine through the frame chain (ExtFullScreen). The constants go into one uniform buffer per call
* (AmbientOcclusion.glsl's block); the blur axis and the estimate's kind into the push block. The legacy technique (older effect
* files, or after a failure) stays D3D9.
*/
namespace {
	struct AOPush {
		D3DXVECTOR2 axis;				// 0: DedicatedBlur's axis
		UInt32 gtaoEstimate;			// 8: 1 = GTAOEstimate
		UInt32 linearSampler;			// 12: LINEAR, CLAMP (filled in by DXVK)
		UInt32 pointSampler;			// 16: POINT, CLAMP
		UInt32 pointWrapSampler;		// 20: POINT, WRAP
	};
	struct AOUniforms {
		D3DXMATRIX projection, invProjection, invView, shadowOrtho;	// as stored (see AmbientOcclusion.glsl)
		D3DXVECTOR4 aoData, data, gtao, sky, sky2, sky3, sky4, sky5, smoothedSunDir, shadowFade, reciprocalResolution, fogData, fogColor,
			depthConstants, cameraData, layout, skyLayout;
		D3DXMATRIX rtReproject;			// UNOFFICIAL 2026-10-28 (fork RT): read only by the RT passes (AmbientOcclusion.glsl)
		D3DXVECTOR4 rtTemporal;
		D3DXVECTOR4 rtRect;				// UNOFFICIAL 2026-11-03 RenderScale < 1: xy this frame's RenderRect scale, zw the RT history's
	};


	// The constants from the same registered memory the D3D9 path uploads (SetCT); null when not registered.
	const D3DXVECTOR4* RegisteredConstant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	IDirect3DBaseTexture9* SlotTexture(const char* name) {
		std::string slotName = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(slotName);
		return slot ? *slot : nullptr;
	}

	// Textures in set 1 (binding i, slot i), the constants in set 2 binding 0, the three samplers' heap indices at push offsets 12-20.
	void MakePipeline(EffectRecord::ExtPass& pass, const char* spvName, UINT textureCount) {
		if (pass.Tried) return;
		pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[9] = {};
		UINT count = 0;
		for (UINT i = 0; i < textureCount; i++, count++) {
			bindings[count].Set = 1; bindings[count].Binding = i; bindings[count].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[count].Slot = i; bindings[count].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[count].Set = 2; bindings[count].Binding = 0; bindings[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[count].Slot = 0; count++;
		for (UINT s = 0; s < 3; s++, count++) { bindings[count].Type = NVR_EXT_BIND_SAMPLER; bindings[count].Slot = s; bindings[count].PushOffset = 12 + 4 * s; }
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, count, sizeof(AOPush), true);
	}

	// The resources of one draw, in MakePipeline's order. False: a texture or sampler is missing.
	bool Resources(NVR_EXT_RESOURCE_BINDING* resources, UINT& count, IDirect3DBaseTexture9* const* textures, UINT textureCount, ID3D9NvrExtBuffer* uniforms) {
		ID3D9NvrExtSampler* samplers[3] = { DxvkCompute::LinearClamp(), DxvkCompute::PointClamp(), DxvkCompute::PointWrap() };
		if (!uniforms || !samplers[0] || !samplers[1] || !samplers[2]) return false;
		count = 0;
		for (UINT i = 0; i < textureCount; i++, count++) {
			if (!textures[i]) return false;
			resources[count].Slot = i; resources[count].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[count].pResource = textures[i];
		}
		resources[count].Slot = 0; resources[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[count].pResource = uniforms; count++;
		for (UINT s = 0; s < 3; s++, count++) { resources[count].Slot = s; resources[count].Type = NVR_EXT_BIND_SAMPLER; resources[count].pResource = samplers[s]; }
		return true;
	}

	// UNOFFICIAL 2026-10-28 (fork RT): MakePipeline with Targets (the temporal pass writes two) that never fails the other Vulkan passes
	// (FailAll false; a missing .spv is checked first): the RT pass is then simply not drawn and NVR's own pass runs.
	void MakePipelineRT(EffectRecord::ExtPass& pass, const char* spvName, UINT textureCount, UINT targets) {
		if (pass.Tried) return;
		pass.Tried = true;
		if (!DxvkCompute::HasSpirv(spvName)) { Logger::Log("UNOFFICIAL RT AO: Compute\\%s.spv is missing; Vegas Shaders' own pass runs.", spvName); return; }
		NVR_EXT_BINDING_DESC bindings[10] = {};
		UINT count = 0;
		for (UINT i = 0; i < textureCount; i++, count++) {
			bindings[count].Set = 1; bindings[count].Binding = i; bindings[count].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[count].Slot = i; bindings[count].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[count].Set = 2; bindings[count].Binding = 0; bindings[count].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[count].Slot = 0; count++;
		for (UINT s = 0; s < 3; s++, count++) { bindings[count].Type = NVR_EXT_BIND_SAMPLER; bindings[count].Slot = s; bindings[count].PushOffset = 12 + 4 * s; }
		pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvName, bindings, count, sizeof(AOPush), true, targets, NVR_EXT_BLEND_NONE, false);
		if (!pass.Pipeline) Logger::Log("UNOFFICIAL RT AO: Compute\\%s.spv could not be made into a pass; Vegas Shaders' own pass runs.", spvName);
	}

	// Sub-pass timings (nested inside "Ambient occlusion") to direct further AO work; either path.
	GpuTimer& AOPassTimer(UINT p) {
		static GpuTimer passTimers[4] = { GpuTimer("  AO estimate (half)"), GpuTimer("  AO blur X (half)"),
			GpuTimer("  AO blur Y (half)"), GpuTimer("  AO combine (full)") };
		return passTimers[p < 4 ? p : 3];
	}
}

// This call's constants (the sky pass and the AO passes each keep a buffer: both are drawn in one frame). Layout: NVR_AOLayout;
// SkyLayout: xy 1 / the sky target's size, z NVR_SkySmooth.x.
bool AmbientOcclusionEffect::ExtConstants(ExtUniformBuffer& Uniforms, const D3DXVECTOR4& Layout, const D3DXVECTOR4& SkyLayout) {
	const D3DXVECTOR4* projection = RegisteredConstant("TESR_ProjectionTransform");
	const D3DXVECTOR4* invProjection = RegisteredConstant("TESR_InvProjectionTransform");
	const D3DXVECTOR4* invView = RegisteredConstant("TESR_InvViewTransform");
	const D3DXVECTOR4* reciprocalResolution = RegisteredConstant("TESR_ReciprocalResolution");
	const D3DXVECTOR4* fogData = RegisteredConstant("TESR_FogData");
	const D3DXVECTOR4* fogColor = RegisteredConstant("TESR_FogColor");
	const D3DXVECTOR4* depthConstants = RegisteredConstant("TESR_DepthConstants");
	const D3DXVECTOR4* cameraData = RegisteredConstant("TESR_CameraData");
	if (!projection || !invProjection || !invView || !reciprocalResolution || !fogData || !fogColor || !depthConstants || !cameraData) return false;
	// ShadowsExteriors' (absent without it: 0, as the effect's unset parameters)
	const D3DXVECTOR4* shadowOrtho = RegisteredConstant("TESR_ShadowCameraToLightTransformOrtho");
	const D3DXVECTOR4* smoothedSunDir = RegisteredConstant("TESR_SmoothedSunDir");
	const D3DXVECTOR4* shadowFade = RegisteredConstant("TESR_ShadowFade");
	AOUniforms uniforms = {};
	memcpy(&uniforms.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.invProjection, invProjection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.invView, invView, sizeof(D3DXMATRIX));
	if (shadowOrtho) memcpy(&uniforms.shadowOrtho, shadowOrtho, sizeof(D3DXMATRIX));
	uniforms.aoData = Constants.AOData;
	uniforms.data = Constants.Data;
	uniforms.gtao = GTAOData;
	uniforms.sky = SkyData;
	uniforms.sky2 = SkyData2;
	uniforms.sky3 = SkyData3;
	uniforms.sky4 = SkyData4;
	uniforms.sky5 = SkyData5;
	if (smoothedSunDir) uniforms.smoothedSunDir = *smoothedSunDir;
	if (shadowFade) uniforms.shadowFade = *shadowFade;
	uniforms.reciprocalResolution = *reciprocalResolution;
	uniforms.fogData = *fogData;
	uniforms.fogColor = *fogColor;
	uniforms.depthConstants = *depthConstants;
	uniforms.cameraData = *cameraData;
	uniforms.layout = Layout;
	uniforms.skyLayout = SkyLayout;
	uniforms.rtReproject = RtReproject;
	uniforms.rtTemporal = RtTemporal;
	uniforms.rtTemporal.w = (float)RtAmbient::Current.DebugView;
	uniforms.rtRect = D3DXVECTOR4(RtRectScaleX(), RtRectScaleY(), RtHistoryScale[0], RtHistoryScale[1]);	// UNOFFICIAL 2026-11-03
	return ExtUniforms(Uniforms, &uniforms, sizeof(uniforms)) != nullptr;
}

// The Dedicated technique's passes as Render's D3D9 loop draws them: 0 Estimate -> AO 0, 1 BlurX AO 0 -> AO 1, 2 BlurY AO 1 -> AO 0,
// 3 Combine AO 0 -> the frame chain (never the direct final write: the D3D9 combine commits). combineOnly: the combine alone;
// deferCombine: without it. False: nothing of the frame was committed (the D3D9 passes draw it; ExtFailed is set when a pass failed).
bool AmbientOcclusionEffect::ExtDedicated(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface,
	IDirect3DTexture9* const* Textures, IDirect3DSurface9* const* Surfaces, const D3DXVECTOR4& Layout, bool GTAOEstimate, IDirect3DTexture9* OrthoMap, IDirect3DTexture9* SkyVis) {
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* noise = ExtFileTexture("TESR_BlueNoiseSampler");
	IDirect3DBaseTexture9* scene = TheTextureManager->RenderedTexture;
	if (!depth || !normals || !noise || !scene || (SkyData.x > 0.0f && !OrthoMap)) return false;
	// UNOFFICIAL 2026-10-26: PackedEstimate's or GTAOEstimate's own variant (Compute\variants.txt), else the general shader.
	ID3D9NvrExtPipeline* estimate = ExtVariant(extVariants, extEstimate, "AmbientOcclusionEstimate", GTAOEstimate ? "g1" : "g0", [](const char* name) {
		EffectRecord::ExtPass made;
		MakePipeline(made, name, 4);
		return made.Pipeline;
	});
	MakePipeline(extBlur, "AmbientOcclusionBlur", 3);
	MakePipeline(extCombine, "AmbientOcclusionCombine", 4);
	// UNOFFICIAL 2026-10-28 (fork RT): the RT estimate's temporal constants (into this call's uniforms, below) while it runs.
	D3DSURFACE_DESC aoDesc = {};
	const bool rt = !combineOnly && RtAmbient::Ready() && SUCCEEDED(Surfaces[0]->GetDesc(&aoDesc));
	if (rt) RtTemporalSetup(aoDesc);
	if (!estimate || !extBlur.Pipeline || !extCombine.Pipeline || !ExtConstants(extUniforms, Layout, D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f))) return false;
	const UINT firstPass = combineOnly ? 3 : 0;
	const UINT endPass = deferCombine ? 3 : 4;
	for (UINT p = firstPass; p < endPass; ++p) {
		AOPush push = {};
		push.axis = p == 1 ? D3DXVECTOR2(1.0f, 0.0f) : D3DXVECTOR2(0.0f, 1.0f);	// (io.xy, io.yx)
		push.gtaoEstimate = GTAOEstimate ? 1 : 0;
		NVR_EXT_RESOURCE_BINDING resources[8] = {};
		UINT count = 0;
		GpuProfileScope gpu(AOPassTimer(p), Device);
		if (p == 3) {
			// Stand-ins for what the pass does not read this frame (SkyData4.w 0: no sky factor).
			IDirect3DBaseTexture9* inputs[4] = { Textures[0], depth, scene, SkyVis ? (IDirect3DBaseTexture9*)SkyVis : depth };
			if (!Resources(resources, count, inputs, 4, extUniforms.Buffer)) return false;
			return ExtFullScreen(Device, RenderTarget, RenderedSurface, nullptr, extCombine.Pipeline, resources, count, &push, sizeof(push), false);
		}
		bool drawn;
		if (p == 0 && rt && ExtEstimateRT(Device, Textures, Surfaces, aoDesc)) drawn = true;	// UNOFFICIAL 2026-10-28 (fork RT): into AO 0 like Estimate
		else if (p == 0) {
			IDirect3DBaseTexture9* inputs[4] = { depth, normals, noise, OrthoMap ? (IDirect3DBaseTexture9*)OrthoMap : depth };	// (no Skylighting v1: never read)
			drawn = Resources(resources, count, inputs, 4, extUniforms.Buffer) && ExtDrawTo(estimate, &Surfaces[0], 1, resources, count, &push, sizeof(push));
		}
		else {
			IDirect3DBaseTexture9* inputs[3] = { Textures[p == 1 ? 0 : 1], depth, normals };
			drawn = Resources(resources, count, inputs, 3, extUniforms.Buffer) && ExtDrawTo(extBlur.Pipeline, &Surfaces[p == 1 ? 1 : 0], 1, resources, count, &push, sizeof(push));
		}
		if (!drawn) return false;
	}
	return true;
}

// UNOFFICIAL 2026-10-28 (fork RT) the RT estimate's temporal constants (RtReproject, RtTemporal: read through ExtConstants) for this frame,
// before ExtConstants and ExtEstimateRT. (2026-11-03: one helper for ExtDedicated and Render's D3D9 loop, which runs the RT estimate in
// rect frames.) The history is kept only if it was made a moment ago at this AO size and RenderRect scale (rect vs native counts).
void AmbientOcclusionEffect::RtTemporalSetup(const D3DSURFACE_DESC& AoDesc) {
	const double now = TheFrameRateManager->Time;
	const bool rectSame = RtHistoryScale[0] == RtRectScaleX() && RtHistoryScale[1] == RtRectScaleY() && !RenderRect::ScaleChanged();	// (2026-11-03)
	const bool continuous = !RtPlaceRestart() && RtHistoryValid && rtHistoryWidth == AoDesc.Width && rtHistoryHeight == AoDesc.Height && now - RtLastTime >= 0.0 &&
		now - RtLastTime < 0.5 && !TheShaderManager->GameState.isCameraCut && rectSame &&
		(!TemporalBackbone::Shared() || TemporalBackbone::HistoryValid());	// UNOFFICIAL 2026-11-01 SharedNoise: and the shared history rule
	D3DXMatrixIdentity(&RtReproject);
	EffectStatus::History(StatusSlot, Name, continuous ? "kept (RT AO)" : !rectSame && RtHistoryValid ? "reset (RT AO: RenderScale rect changed)" :
		"reset (RT AO: no history, size, gap, cut or shared rule)");	// UNOFFICIAL 2026-11-01
	if (continuous) BounceLightReprojection(TheRenderManager->viewMatrix, TheRenderManager->InvViewMatrix, RtLastView, RtLastInvView, RtLastProj, &RtReproject);
	// (2026-11-07 perf-history) 0.9 of last frame kept at 60 fps: the same decay a second at this frame rate (RtHistoryTime.h; 0.73 at 20 fps)
	RtTemporal = D3DXVECTOR4(continuous ? RtHistoryTime::Keep(0.9f, RtDenoise::HistoryClock()) : 0.0f, 4.0f,
		(float)(TemporalBackbone::Shared() ? TemporalBackbone::EffectIndex(TemporalBackbone::OffsetRtAmbient) : RtFrame % 1024), 0.0f);	// (SharedNoise: shared index, own offset)
}

// SkyVisibilityV2 into Target (the sky target the D3D9 pass draws). The D3D9 pass's stage 8 and 9 textures: null = not read this frame.
bool AmbientOcclusionEffect::ExtSkyVisibility(IDirect3DSurface9* Target, const D3DSURFACE_DESC& Desc, IDirect3DTexture9* OrthoMap, IDirect3DTexture9* SunMask, IDirect3DTexture9* SmoothedNormals) {
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	if (!depth || !normals || !OrthoMap || !Desc.Width || !Desc.Height) return false;
	MakePipeline(extSky, "AmbientOcclusionSky", 5);
	const D3DXVECTOR4 skyLayout(1.0f / Desc.Width, 1.0f / Desc.Height, SmoothedNormals ? 1.0f : 0.0f, 0.0f);
	if (!extSky.Pipeline || !ExtConstants(extSkyUniforms, D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f), skyLayout)) return false;
	IDirect3DBaseTexture9* inputs[5] = { depth, normals, OrthoMap, SunMask ? (IDirect3DBaseTexture9*)SunMask : depth,
		SmoothedNormals ? (IDirect3DBaseTexture9*)SmoothedNormals : depth };
	NVR_EXT_RESOURCE_BINDING resources[9] = {};
	UINT count = 0;
	AOPush push = {};
	return Resources(resources, count, inputs, 5, extSkyUniforms.Buffer) && ExtDrawTo(extSky.Pipeline, &Target, 1, resources, count, &push, sizeof(push));
}

// UNOFFICIAL 2026-10-28 (fork RT, [Shaders.AmbientOcclusion.RT]; core/RtAmbient.h, docs/next/FORK-RT-CORE.md): this effect is the RT
// core's user "RT ambient occlusion". At the core's point of the frame (RtCore::Frame: depth and normals exist; the sky, AO and
// AmbientLight passes come later in the frame) RtRecord draws RtAOInput.frag (view distance + world normal per half-resolution texel)
// into NVR_RTAOInput, copies it into the shared input image, adds the helper's RtAOTrace.comp pass and copies its output back into
// NVR_RTAORaw (AO visibility, sky visibility, covered, DebugView 3's value). RtRaw: that texture if it was traced this frame, else null
// (the callers keep NVR's own passes).
namespace {
	int RtImageIn = -1, RtImageOut = -1, RtImageGI = -1, RtImageAlphaStandIn = -1, RtImageLight = -1, RtImageAlbedoStandIn = -1, RtShader = -1;
	int RtGIShader = -1, RtImageReservoir[2][2] = { { -1, -1 }, { -1, -1 } };	// (2026-10-30) GIReuse: RtGITrace and its two reservoir pairs (A, B)
	bool RtGIHistory = false;
	int RtCacheShader = -1, RtImageCache = -1;	// (2026-10-31) RadianceCache: RtRadianceCache and its image (2026-11-12 sungi p3: 512 x 512, 8 texels a cell, 2 levels)
	bool RtCacheValid = false;	// the cache image holds this session's cells (else the next cache pass starts every cell over)
	float RtCacheCellSize = 0.0f, RtCacheCap = 0.0f;
	int RtImageGIOut = -1, RtImageGlowList = -1;	// (2026-10-30) the reuse passes' bounce light out (estimate + GlowDirect) and the glow lights
	IDirect3DTexture9* RtGlowTexture = nullptr;	// the glow lights' source (32 x 3, dynamic: filled each frame from RtCore::GlowLights())
	constexpr UINT GlowLightsMax = 32;	// the reservoirs hold last frame's (cleared at each helper start and whenever a frame had no GI pass)
	// UNOFFICIAL 2026-11-03 (rtgi-lamps, docs/next/RT-PATH-TRACING.md section 18) all the frame's lamps for the GI passes' bounce hits (not
	// only the 7-8 in the constants): RtPathTrace.cpp's lamp image layout (4 RG32F texels a lamp: pos xy, pos z + reach, colour rg, b),
	// filled by the CPU each GI frame (RtGILamps), CopyIn, pass image 12 (binding 16) of the AO, GI and cache passes; gi.z = -count then
	constexpr int GILampImageMax = 256;	// ShadowCubeMapsMax + TrackedLightsMax fit; (2026-11-03 rtspec) GIGameLamps: the game's table's nearest 256
	int RtImageLamps = -1, RtLampCount = 0;
	IDirect3DTexture9* RtLampTexture = nullptr;	// (GILampImageMax x 4) x 1, G32R32F, dynamic
	float RtLampData[GILampImageMax][8] = {};
	// UNOFFICIAL 2026-11-12 (sungi p3, F6) SkyMissSH: the sky's radiance harmonics for the traces' ray misses, row 1 of the lamp image (the lamp
	// rows are row 0; the passes' constants are full): 9 coefficients x rgb as 27 floats in 14 RG32F texels (RtAOTrace.comp SkyRadianceSH)
	float RtSkyData[16][2] = {};
	bool RtSkyDataOn = false;	// RtFillLamps writes row 1 and fills the image even with no lamps
	int RtCacheFlags = 0;	// the cache's last flags (1 CacheVisibility, 2 CacheCascades 2): a change starts every cell over
	bool RtGITraced = false;	// (rt-ao-gi) this frame's trace had GI rays
	UInt32 RtTracedFrame = 0xFFFFFFFF;
	UInt32 RtReduced(UInt32 size) { return MaxQuality::Reduced(size, 2); }
	constexpr int GILightKeys = 5;	// (GIReuse) the lamps' total and brightest luminance, their count, the sun's luminance and height

	bool RtWants() {
		AmbientOcclusionEffect* ao = TheShaderManager->Effects.AmbientOcclusion;
		if (AmbientLightEffect* sunGiAl = TheShaderManager->Effects.AmbientLight) sunGiAl->SunGIFrame();	// UNOFFICIAL 2026-11-12 (sungi p4): SunGI follows the player in and out before Current.Enabled is read
		// (2026-11-03) rect frames too (RenderScale < 1: RtRecord traces the rect; docs/next/RT-RENDERSCALE.md)
		return RtAmbient::Current.Enabled && RtAmbient::PassesWanted() && ao && ao->Effect;
	}
	void RtSetup() {
		const UINT w = RtReduced(TheRenderManager->width), h = RtReduced(TheRenderManager->height);
		RtImageIn = RtCore::AddImage("NVR RT ao input", w, h, NVR_EXT_SHARED_RG32F);
		RtImageOut = RtCore::AddImage("NVR RT ao", w, h, NVR_EXT_SHARED_RGBA16F);
		RtImageGI = RtCore::AddImage("NVR RT gi", w, h, NVR_EXT_SHARED_RGBA16F);	// (rt-ao-gi) the bounce light; written every trace
		RtImageLight = RtCore::AddImage("NVR RT gi frame light", w, h, NVR_EXT_SHARED_RGBA16F);	// (GI v2) the lit frame, for on-screen hits
		// (2026-10-29) RtAOTrace.comp binds the core's alpha atlas (binding 5) for the alpha-tested casters; with AlphaTested off there is no
		// atlas and no such instance (the shader never reads it): this small image stands in so the binding is always an R16F image.
		if (RtCore::AlphaAtlasImage() < 0) RtImageAlphaStandIn = RtCore::AddImage("NVR RT ao alpha stand-in", 256, 256, NVR_EXT_SHARED_R16F);
		// (2026-10-29) the same for the albedo atlas (binding 7, [Main.Main.RayTracing] HitTextures; RtHit.glsl falls back to the averages)
		if (RtCore::AlbedoAtlasImage() < 0) RtImageAlbedoStandIn = RtCore::AddImage("NVR RT ao albedo stand-in", 256, 256, NVR_EXT_SHARED_RGBA8);
		// (2026-10-30) GIReuse's reservoirs: two pairs, last frame's and this frame's, swapped every frame (RtAOTrace.comp RT_GI_PASS).
		for (int set = 0; set < 2; set++) {
			RtImageReservoir[set][0] = RtCore::AddImage(set ? "NVR RT gi reservoir A1" : "NVR RT gi reservoir A0", w, h, NVR_EXT_SHARED_RGBA16F);
			// UNOFFICIAL 2026-11-13 (rtgi-review B3) twice as tall: the lower half holds GISamplePoints' sample points (RtAOTrace.comp ReuseGIPoints); no new shared image
			RtImageReservoir[set][1] = RtCore::AddImage(set ? "NVR RT gi reservoir B1" : "NVR RT gi reservoir B0", w, 2 * h, NVR_EXT_SHARED_RGBA16F);
		}
		RtGIHistory = false;
		RtImageGIOut = RtCore::AddImage("NVR RT gi out", w, h, NVR_EXT_SHARED_RGBA16F);
		RtImageGlowList = RtCore::AddImage("NVR RT glow lights", GlowLightsMax, 3, NVR_EXT_SHARED_RGBA16F);
		// (2026-10-31) RadianceCache. UNOFFICIAL 2026-11-12 (sungi p3): 512 x 512 (was 256 x 256): 8 texels a cell (the SH, the header and the
		// visibility moments) and two levels of 256 rows (the fine grid, the coarse cascade); still ONE shared image (RtAOTrace.comp CacheTexel)
		RtImageCache = RtCore::AddImage("NVR RT radiance cache", 512, 512, NVR_EXT_SHARED_RGBA16F);
		// (2026-11-03 rtgi-lamps) UNOFFICIAL 2026-11-12 (sungi p3): 2 rows (the image is rounded up to 256 rows anyway): row 1 = the sky SH
		RtImageLamps = RtCore::AddImage("NVR RT gi lamps", 4 * GILampImageMax, 2, NVR_EXT_SHARED_RG32F);
		RtCacheValid = false;
	}

	// (2026-10-30) GlowDirect: the strongest glow-mapped instances as seen from the eye (colour x radius^2 / distance^2), at most 32, into
	// RtGlowTexture (row 0 centre + radius, row 1 colour, texel (0, 2) count + strength; RtAOTrace.comp GlowDirect). False: no texture.
	bool RtFillGlowLights(IDirect3DDevice9* Device, float Strength) {
		if (!RtGlowTexture && FAILED(Device->CreateTexture(GlowLightsMax, 3, 1, D3DUSAGE_DYNAMIC, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &RtGlowTexture, nullptr)))
			return false;
		struct Pick { const RtCore::GlowLight* light; float weight; };
		std::vector<Pick> picks;
		// (v2, rt-gi-18) only small bright emitters: meshes over 256 units left out (rooms, facades with a few lit details), the emitting
		// radius at most 32, the colour gated by its brightness (strongest channel 0.35 -> 0.8, as GlowGI) so grey averages give nothing.
		auto emit = [](const RtCore::GlowLight& g, float* rgb, float& radius) {
			const float peak = (std::max)(g.rgb[0], (std::max)(g.rgb[1], g.rgb[2]));
			float t = (std::min)((std::max)((peak - 0.35f) / 0.45f, 0.0f), 1.0f);
			t = t * t * (3.0f - 2.0f * t);
			for (int k = 0; k < 3; k++) rgb[k] = g.rgb[k] * t;
			radius = (std::min)(g.radius, 32.0f);
			return g.radius <= 256.0f && t > 0.0f;
		};
		if (Strength > 0.0f) {
			for (const RtCore::GlowLight& g : RtCore::GlowLights()) {
				float rgb[3], radius;
				if (!emit(g, rgb, radius)) continue;
				const float d2 = (std::max)(g.center[0] * g.center[0] + g.center[1] * g.center[1] + g.center[2] * g.center[2], radius * radius);
				const float w = (0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]) * radius * radius / (std::max)(d2, 1.0f);
				if (w > 0.0f && radius > 0.0f) picks.push_back({ &g, w });
			}
			const size_t keep = (std::min)(picks.size(), (size_t)GlowLightsMax);
			std::partial_sort(picks.begin(), picks.begin() + keep, picks.end(), [](const Pick& a, const Pick& b) { return a.weight > b.weight; });
			picks.resize(keep);
		}
		D3DLOCKED_RECT r;
		if (FAILED(RtGlowTexture->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) return false;
		float row[3][GlowLightsMax * 4] = {};
		for (size_t i = 0; i < picks.size(); i++) {
			const RtCore::GlowLight& g = *picks[i].light;
			float rgb[3], radius;
			emit(g, rgb, radius);
			memcpy(&row[0][i * 4], g.center, sizeof(g.center));
			row[0][i * 4 + 3] = radius;
			memcpy(&row[1][i * 4], rgb, sizeof(rgb));
		}
		row[2][0] = (float)picks.size();
		row[2][1] = Strength;
		for (int y = 0; y < 3; y++) D3DXFloat32To16Array((D3DXFLOAT16*)((BYTE*)r.pBits + y * r.Pitch), row[y], GlowLightsMax * 4);
		RtGlowTexture->UnlockRect(0);
		static bool logged = false;
		if (!logged && !picks.empty()) { logged = true; Logger::Log("UNOFFICIAL RT AO: GlowDirect lights the bounce from %u glow-mapped instances (of %u this frame).", (UInt32)picks.size(), (UInt32)RtCore::GlowLights().size()); }
		return true;
	}

	// (rt-ao-gi) The lamps that can light what the GI rays hit: the shadow slots' lamps and the tracked ones (as ShaderManager listed them
	// this frame, as AmbientLight's grid takes them), nearest to the eye first, at most GILampsMax; scene space (world minus
	// CameraPosition), colour linear x dimmer. Returns how many.
	int RtGILamps(RtAmbient::TraceConstants& c) {
		struct Lamp { D3DXVECTOR4 pos, colour; float distance; };
		Lamp lamps[ShadowCubeMapsMax + TrackedLightsMax];
		int found = 0;
		const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
		auto add = [&](const D3DXVECTOR4& p, const D3DXVECTOR4& col) {
			if (p.w <= 10.0f || col.w <= 0.0f || found >= ShadowCubeMapsMax + TrackedLightsMax) return;
			const D3DXVECTOR3 rel(p.x - eye.x, p.y - eye.y, p.z - eye.z);
			for (int i = 0; i < found; i++)	// the same lamp in both lists
				if (fabsf(lamps[i].pos.x - rel.x) < 1.0f && fabsf(lamps[i].pos.y - rel.y) < 1.0f && fabsf(lamps[i].pos.z - rel.z) < 1.0f) return;
			lamps[found++] = { D3DXVECTOR4(rel.x, rel.y, rel.z, p.w),
				D3DXVECTOR4(powf((std::max)(col.x, 0.0f), 2.2f) * col.w, powf((std::max)(col.y, 0.0f), 2.2f) * col.w, powf((std::max)(col.z, 0.0f), 2.2f) * col.w, 0.0f),
				(std::max)(D3DXVec3Length(&rel) - p.w, 0.0f) };
		};
		ShaderManager* sm = TheShaderManager;
		// UNOFFICIAL 2026-11-03 (rtspec) GIGameLamps: the game's lamp table's active lamps (ManyLamps.h: the deferred pass's lamps, reach
		// Spec.r, colour diffuse x dimmer x LOD dimmer decoded together), the reach NEGATIVE = the game's falloff in RtAOTrace.comp LampFall.
		// The reference path tracer's PTGameLamps 4 lights the same (its direct light 1.07x the deferred pass's at Lucky 38: section 21).
		if (RtAmbient::Current.GIGameLamps && ManyLamps::TableFrame + 1 >= ManyLamps::FrameId && !ManyLamps::ActiveLamps.empty()) {
			static std::vector<Lamp> table;
			table.clear();
			for (const ManyLamps::ActiveLamp& a : ManyLamps::ActiveLamps) {
				if (a.reach <= 0.0f) continue;
				const D3DXVECTOR3 rel(a.pos[0] - eye.x, a.pos[1] - eye.y, a.pos[2] - eye.z);
				PBRShaders* pbr = TheShaderManager->Shaders.PBR;	// (2026-11-04 rtgap) x the game's light scale, as its lamps
				const float ls = pbr ? pbr->Constants.LinearScales.x : 1.0f;
				table.push_back({ D3DXVECTOR4(rel.x, rel.y, rel.z, -a.reach), D3DXVECTOR4(a.colour[0] * ls, a.colour[1] * ls, a.colour[2] * ls, 0.0f),
					(std::max)(D3DXVec3Length(&rel) - a.reach, 0.0f) });
			}
			std::sort(table.begin(), table.end(), [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
			RtLampCount = (std::min)((int)table.size(), GILampImageMax);
			for (int i = 0; i < RtLampCount; i++) {
				const float v[8] = { table[i].pos.x, table[i].pos.y, table[i].pos.z, table[i].pos.w, table[i].colour.x, table[i].colour.y, table[i].colour.z, 0.0f };
				memcpy(RtLampData[i], v, sizeof(v));
			}
			const int first = (std::min)((int)table.size(), RtAmbient::GILampsMax);
			for (int i = 0; i < first; i++) { c.lampPos[i] = table[i].pos; c.lampColor[i] = table[i].colour; }
			return first;
		}
		for (int s = 0; s < ShadowCubeMapsMax; s++) add(sm->BatchLightPosition[s], sm->BatchLightColor[s]);
		for (int t = 0; t < TrackedLightsMax; t++) add(sm->LightPosition[t], sm->LightColor[ShadowCubeMapsScreen + t]);
		std::sort(lamps, lamps + found, [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
		// (2026-11-03 rtgi-lamps) all of them (nearest first) for the lamp image
		RtLampCount = (std::min)(found, GILampImageMax);
		for (int i = 0; i < RtLampCount; i++) {
			const float v[8] = { lamps[i].pos.x, lamps[i].pos.y, lamps[i].pos.z, lamps[i].pos.w, lamps[i].colour.x, lamps[i].colour.y, lamps[i].colour.z, 0.0f };
			memcpy(RtLampData[i], v, sizeof(v));
		}
		found = (std::min)(found, RtAmbient::GILampsMax);
		for (int i = 0; i < found; i++) { c.lampPos[i] = lamps[i].pos; c.lampColor[i] = lamps[i].colour; }
		return found;
	}
	// (2026-11-03 rtgi-lamps) RtLampData into RtLampTexture and the helper's lamp image; false: not this frame (the constants' lamps then)
	bool RtFillLamps(IDirect3DDevice9* Device) {
		if (RtImageLamps < 0 || (RtLampCount <= 0 && !RtSkyDataOn)) return false;
		// (2026-11-12 sungi p3: 2 rows, the second the sky SH)
		if (!RtLampTexture && FAILED(Device->CreateTexture(4 * GILampImageMax, 2, 1, D3DUSAGE_DYNAMIC, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &RtLampTexture, nullptr)))
			return false;
		D3DLOCKED_RECT r = {};
		if (FAILED(RtLampTexture->LockRect(0, &r, nullptr, D3DLOCK_DISCARD))) return false;
		memset(r.pBits, 0, sizeof(RtLampData));
		memcpy(r.pBits, RtLampData, sizeof(float) * 8 * (std::max)(RtLampCount, 0));
		if (RtSkyDataOn) memcpy((char*)r.pBits + r.Pitch, RtSkyData, sizeof(RtSkyData));
		RtLampTexture->UnlockRect(0);
		return RtCore::CopyIn(RtImageLamps, RtLampTexture);
	}
	void RtRecordUser(IDirect3DDevice9* Device) {
		if (AmbientOcclusionEffect* ao = TheShaderManager->Effects.AmbientOcclusion) ao->RtRecord(Device);
	}
}

void AmbientOcclusionEffect::RtRegister() {
	RtShader = RtCore::AddShader("RtAOTrace");
	RtGIShader = RtCore::AddShader("RtGITrace");	// (2026-10-30) GIReuse: RtAOTrace.comp built with RT_GI_PASS
	RtCacheShader = RtCore::AddShader("RtRadianceCache");	// (2026-10-31) RadianceCache: RtAOTrace.comp built with RT_CACHE_PASS
	RtCore::AddUser({ "RT ambient occlusion", RtWants, RtSetup, RtRecordUser });
}

IDirect3DSurface9* AmbientOcclusionEffect::RtRawSurface() {	// UNOFFICIAL 2026-11-01
	AmbientOcclusionEffect* ao = TheShaderManager ? TheShaderManager->Effects.AmbientOcclusion : nullptr;
	return ao && RtTracedFrame != 0xFFFFFFFF ? ao->rtRawSurface : nullptr;
}

// UNOFFICIAL 2026-11-09 (diagnostic; AmbientOcclusion.h). Only textures that exist; a stage is one tiny copy, with the inspector on.
void AmbientOcclusionEffect::RtInspect() {
	if (!PixelInspector::Active) return;
	PixelInspector::CaptureIf("RT AO input (NVR_RTAOInput: distance, normal)", rtInputTexture, PixelInspector::RtInput);
	PixelInspector::CaptureIf("RT AO raw (r AO, g sky, b covered)", rtRawTexture, PixelInspector::Buffer);
	PixelInspector::CaptureIf("RT AO denoised (NVR_RTAODenoised)", rtDenoisedTexture, PixelInspector::Buffer);
	PixelInspector::CaptureIf("RT GI frame light (on-screen hits' light)", rtLightTexture, PixelInspector::Buffer);
	PixelInspector::CaptureIf("RT GI raw (rgb bounce, a hit distance)", rtGITexture, PixelInspector::Buffer);
	PixelInspector::CaptureIf("RT GI denoised (NVR_RTGIDenoised)", rtDenoisedGITexture, PixelInspector::Buffer);
}

IDirect3DTexture9* AmbientOcclusionEffect::RtRaw() {
	return RtTracedFrame == TheFrameRateManager->FrameNumber && RtAmbient::Ready() ? rtRawTexture : nullptr;
}

// (2026-10-29) This frame's AO visibility denoised by the shared RT denoiser (RtDenoise.h: temporal + 3 a-trous passes), at the trace
// grid's half resolution (Downscale 2: the screen-size version cost 23.6 ms on the iGPU at 720p, rt-gi-3), made once a frame by the
// first caller, or null (Denoise off, a DebugView that shows other channels, or the denoiser could not run: the callers take the raw).
IDirect3DTexture9* AmbientOcclusionEffect::RtDenoised(IDirect3DDevice9* Device) {
	static UInt32 frame = 0xFFFFFFFF;
	static bool result = false;
	if (frame == TheFrameRateManager->FrameNumber) return result ? rtDenoisedTexture : nullptr;
	frame = TheFrameRateManager->FrameNumber;
	result = false;
	const RtAmbient::Settings& rt = RtAmbient::Current;
	IDirect3DTexture9* raw = RtRaw();
	if (!raw || !rt.Denoise || (rt.DebugView != 0 && rt.DebugView != 1)) return nullptr;
	if (!rtDenoisedTexture) TheTextureManager->InitTexture("NVR_RTAODenoised", &rtDenoisedTexture, &rtDenoisedSurface, rtWidth, rtHeight, D3DFMT_R16F);
	if (!rtDenoisedTexture || !rtDenoisedSurface) return nullptr;
	static RtDenoise::Denoiser* denoiser = RtDenoise::Create("RT ambient occlusion");
	static GpuTimer timer("  AO RT denoise");
	GpuProfileScope gpu(timer, Device);
	RtDenoise::Settings s;	// the defaults: 16 frames of history, 3 a-trous passes
	s.Reset = RtPlaceRestart();	// UNOFFICIAL 2026-11-11 (sungi, F16)
	s.Downscale = 2;
	s.Kernel = RtAmbient::Current.DenoiseKernel;	// (2026-10-30) 3 by default: ~2.5x cheaper (rt-sun 1f5d987e)
	result = RtDenoise::Run(denoiser, Device, raw, rtDenoisedSurface, s);
	static bool logged = false;
	if (result && !logged) { logged = true; Logger::Log("UNOFFICIAL RT AO: the AO visibility goes through the shared RT denoiser (RtDenoise)."); }
	return result ? rtDenoisedTexture : nullptr;
}

// (2026-10-29) This frame's GI bounce light denoised by the shared RT denoiser's RGB mode (Channels 3, Downscale 2: at the trace grid),
// made once a frame by the first caller, or null (Denoise off, no GI this frame, or not run: the caller takes the raw bounce).
IDirect3DTexture9* AmbientOcclusionEffect::RtDenoisedGI(IDirect3DDevice9* Device) {
	static UInt32 frame = 0xFFFFFFFF;
	static bool result = false;
	if (frame == TheFrameRateManager->FrameNumber) return result ? rtDenoisedGITexture : nullptr;
	frame = TheFrameRateManager->FrameNumber;
	result = false;
	IDirect3DTexture9* gi = RtGI();
	if (!gi || !RtAmbient::Current.Denoise) return nullptr;
	if (!rtDenoisedGITexture) TheTextureManager->InitTexture("NVR_RTGIDenoised", &rtDenoisedGITexture, &rtDenoisedGISurface, rtWidth, rtHeight, D3DFMT_A16B16G16R16F);
	if (!rtDenoisedGITexture || !rtDenoisedGISurface) return nullptr;
	static RtDenoise::Denoiser* denoiser = RtDenoise::Create("RT GI");
	static GpuTimer timer("  GI RT denoise (half, rgb)");
	GpuProfileScope gpu(timer, Device);
	RtDenoise::Settings s;
	s.Reset = RtPlaceRestart();	// UNOFFICIAL 2026-11-11 (sungi, F16)
	s.Channels = 3;
	s.Downscale = 2;
	s.MovingHistory = RtAmbient::Current.GIMovingHistory;	// (2026-10-31, rt-ao-sky) with DenoiseBackbone only
	s.Kernel = RtAmbient::Current.DenoiseKernel;	// (2026-10-30) 3 by default: ~2.5x cheaper (rt-sun 1f5d987e)
	s.AntiLag = RtAmbient::Current.GIDenoiseAntiLag;	// UNOFFICIAL 2026-11-08 (rtgi-ghost): the path tracer's anti-lag (0 = off, as before)
	// UNOFFICIAL 2026-11-12 (sungi p4, STUDY F7 / phase 4) SunGI outdoors ([Shaders.AmbientOcclusion.RT] GIDenoiseSunGI, default true; off = the settings above
	// only): the denoiser is the GI's one history, so (1) HistoryClamp 1.5: the history is clipped to the new frame's 3 x 3 mean +- 1.5 deviations (a moved
	// light or a turn no longer leaves the old bounce behind); (2) Guide / GuideAlbedo / GuideSigma 0.5 as the path tracer's PTDenoiseGuide: the a-trous passes
	// stop at the game's own shading edges (the lit frame / the albedo buffer, read by uv, so the half-size signal is fine), when the albedo buffer holds
	// this frame's and the frame is not a RenderScale rect frame; (3) ObjectMotion where [Main.Main.Quality] ObjectMotion is on (a walking NPC's history follows
	// him; MovingHistory above then applies); (4) Kernel 5, Iterations 3: outdoor GI is low frequency and the 3 x 3 kernel left the noise. AntiLag and
	// MovingHistory stay as configured.
	if (RtAmbient::SunGIOutdoors() && RtAmbient::Current.GIDenoiseSunGI) {
		s.HistoryClamp = 1.5f;
		s.Kernel = 5;
		s.Iterations = 3;
		s.ObjectMotion = TheSettingManager->GetSettingI("Main.Main.Quality", "ObjectMotion") != 0;
		IDirect3DTexture9* lit = TheTextureManager->RenderedTexture;
		if (lit && CapturedNormals::AlbedoData.x > 0.5f && CapturedNormals::AlbedoTexture && RtRectScaleX() == 1.0f && RtRectScaleY() == 1.0f) {
			s.Guide = lit;
			s.GuideAlbedo = CapturedNormals::AlbedoTexture;
			s.GuideSigma = 0.5f;
			s.GuideOnce = true;	// UNOFFICIAL 2026-11-13 (rtgi-review B2; no key since the settings cleanup)
		}
		// UNOFFICIAL 2026-11-13 (rtgi-review R9 / R10) GIDenoiseTune (0 off = the settings above; 1; 2 default): 1 = MeanVariance 4 (the a-trous brightness test
		// with the variance of the accumulated mean: a converged pixel is no longer smoothed as hard as a new one) + HistoryFix 3 (a newly seen pixel is
		// smoothed by geometry only, so its one noisy sample leaves no speckle); 2 = and no HistoryClamp (its 3 x 3 window of raw 1-sample neighbours is
		// too narrow, the reuse makes them alike, and it cut good history while turning; AntiLag's fast history keeps doing that job).
		if (RtAmbient::Current.GIDenoiseTune >= 1) { s.MeanVariance = 4.0f; s.HistoryFix = 3.0f; }
		if (RtAmbient::Current.GIDenoiseTune >= 2) s.HistoryClamp = 0.0f;
		static bool sunGiLogged = false;
		if (!sunGiLogged) { sunGiLogged = true; Logger::Log("UNOFFICIAL SunGI: the GI denoiser runs with HistoryClamp 1.5, kernel 5 x 3 passes%s%s (GIDenoiseSunGI).", s.Guide ? ", guided by the lit frame + albedo buffer (sigma 0.5)" : ", unguided (no albedo buffer this frame)", s.ObjectMotion ? ", object motion" : ""); }
	}
	result = RtDenoise::Run(denoiser, Device, gi, rtDenoisedGISurface, s);
	static bool logged = false;
	if (result && !logged) { logged = true; Logger::Log("UNOFFICIAL RT AO: the GI bounce light goes through the shared RT denoiser (RGB, half resolution)."); }
	return result ? rtDenoisedGITexture : nullptr;
}

// (rt-ao-gi) this frame's traced bounce light (NVR_RTGIRaw: rgb the light arriving, a hit distance / GIDistance), or null.
IDirect3DTexture9* AmbientOcclusionEffect::RtGI() {
	return RtGITraced && RtRaw() ? rtGITexture : nullptr;
}

void AmbientOcclusionEffect::RtRecord(IDirect3DDevice9* Device) {
	static bool failed = false;
	static UInt32 traced = 0;
	RtGITraced = false;
	if (failed || !Effect || RtImageIn < 0 || RtImageOut < 0 || RtImageGI < 0 || RtShader < 0) return;
	const UINT width = RtReduced(TheRenderManager->width), height = RtReduced(TheRenderManager->height);
	if (!rtInputTexture || rtWidth != width || rtHeight != height) {
		DxvkCompute::Drop(rtInputSurface); DxvkCompute::Drop(rtInputTexture); DxvkCompute::Drop(rtRawSurface); DxvkCompute::Drop(rtRawTexture);
		DxvkCompute::Drop(rtGISurface); DxvkCompute::Drop(rtGITexture); DxvkCompute::Drop(rtLightSurface); DxvkCompute::Drop(rtLightTexture);
		TheTextureManager->InitTexture("NVR_RTGIFrameLight", &rtLightTexture, &rtLightSurface, width, height, D3DFMT_A16B16G16R16F);	// (GI v2)
		TheTextureManager->InitTexture("NVR_RTGIRaw", &rtGITexture, &rtGISurface, width, height, D3DFMT_A16B16G16R16F);	// (rt-ao-gi)
		TheTextureManager->InitTexture("NVR_RTAOInput", &rtInputTexture, &rtInputSurface, width, height, D3DFMT_G32R32F);
		TheTextureManager->InitTexture("NVR_RTAORaw", &rtRawTexture, &rtRawSurface, width, height, D3DFMT_A16B16G16R16F);
		rtWidth = width;
		rtHeight = height;
	}
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	MakePipelineRT(extInputRT, "RtAOInput", 2, 1);
	if (!rtInputTexture || !rtRawTexture || !rtGITexture || !rtLightTexture || !depth || !normals || !extInputRT.Pipeline) return;
	const D3DXVECTOR4 layout(1.0f, 1.0f, 1.0f / width, 1.0f / height);
	static GpuTimer inputTimer("  AO RT input (half)");
	{
		GpuProfileScope gpu(inputTimer, Device);
		IDirect3DBaseTexture9* inputs[2] = { depth, normals };
		NVR_EXT_RESOURCE_BINDING resources[6] = {};
		UINT count = 0;
		AOPush push = {};
		const bool wasFailed = ExtFailed;
		// UNOFFICIAL 2026-11-09 (rt-review) NormalGuard rides in the sky layout's w (unused by this pass; RtAOInput.frag)
		const bool normalGuard = RtAmbient::Current.NormalGuard;
		{
			static int loggedGuard = -1;
			if ((normalGuard ? 1 : 0) != loggedGuard) {
				loggedGuard = normalGuard ? 1 : 0;
				Logger::Log("UNOFFICIAL RT AO: NormalGuard %s (%s).", normalGuard ? "on" : "off",
					normalGuard ? "the rays' normals fetched by texel, no sampler; a texel that is not a unit normal takes the normal rebuilt from depth" : "the point-sampled read, traced as it reads");
			}
		}
		if (!ExtConstants(extRtUniforms, layout, D3DXVECTOR4(RtAmbient::Current.LegacyTilt, normalGuard ? 1.0f : 0.0f /* y: 2026-11-13 rtgi-review R4, the start point and the normal from one pixel */, 0.0f, normalGuard ? 1.0f : 0.0f))	/* x: 2026-11-10 LegacyTilt */ || !Resources(resources, count, inputs, 2, extRtUniforms.Buffer) ||
			!ExtDrawTo(extInputRT.Pipeline, &rtInputSurface, 1, resources, count, &push, sizeof(push))) {
			ExtFailed = wasFailed;
			failed = true;
			Logger::Log("UNOFFICIAL RT AO: the RT input pass failed to draw; Vegas Shaders' own AO runs from now on.");
			return;
		}
	}
	// The trace's constants (RtAOTrace.comp). View -> scene directions as RtSunShadows does it: the view matrix's columns are right (0),
	// up (1) and forward (2); as rows of a row-vector matrix (cb.M * v in the GLSL = v x M).
	const RtAmbient::Settings& rt = RtAmbient::Current;
	const D3DXMATRIX& view = TheRenderManager->viewMatrix;
	const D3DXMATRIX& proj = TheRenderManager->projMatrix;
	RtAmbient::TraceConstants c = {};
	// UNOFFICIAL 2026-11-13 (rtgi-review R21) '= {}' leaves the D3DX members as they were on the stack (their default constructor is empty): zeroed for real
	memset(&c, 0, sizeof(c));
	c.viewToWorld._11 = view._11; c.viewToWorld._12 = view._21; c.viewToWorld._13 = view._31;
	c.viewToWorld._21 = view._12; c.viewToWorld._22 = view._22; c.viewToWorld._23 = view._32;
	c.viewToWorld._31 = view._13; c.viewToWorld._32 = view._23; c.viewToWorld._33 = view._33;
	c.viewToWorld._44 = 1.0f;
	// UNOFFICIAL 2026-11-03 RT AO at RenderScale < 1 (docs/next/RT-RENDERSCALE.md "RT AO / GI plan", phase 1): in a rect frame the
	// frame is the top-left RenderRect part of the half-size input (RtAOInput.frag draws it texel for texel in that layout), so the trace
	// covers only that part: its grid is the rect of the half grid, rounded as ApplyViewport rounds the AO passes' viewport
	// (RenderRect::Width of the half size, not Width(w) / 2), and the shader's uv = texel / grid is then the screen's. The result lands in
	// the same top-left part of NVR_RTAORaw, which the estimate passes read texel for texel. Off: the grid is the whole half size.
	const UINT traceW = RenderRect::Width(width), traceH = RenderRect::Height(height);
	c.projection = D3DXVECTOR4(1.0f / proj._11, 1.0f / proj._22, (float)traceW, (float)traceH);
	// The eye in scene space (RtCore::Eye: 0, the scene is camera-relative). 2026-10-29: was CameraLocation - CameraPosition, which put
	// every ray ~71000 units outside the scene (CameraLocation reads ~0 in game: the every-ray-misses bug, rt-sun-core 4ba2abfd).
	const D3DXVECTOR3 eye = RtCore::Eye();
	c.origin = D3DXVECTOR4(eye.x, eye.y, eye.z, (float)(TheFrameRateManager->FrameNumber % 1024));
	// UNOFFICIAL 2026-11-09 (rtgi-lock) ao.z: 1 = the noise moves each frame, 2 = and the AO / GI sample numbers are independent (SampleLockFix; RtAOTrace.comp SampleU)
	{
		static int loggedLock = -1;
		if ((rt.SampleLockFix ? 1 : 0) != loggedLock) {
			loggedLock = rt.SampleLockFix ? 1 : 0;
			Logger::Log("UNOFFICIAL RT AO: SampleLockFix %s (the AO and GI rays' two sample numbers %s).", rt.SampleLockFix ? "on" : "off",
				rt.SampleLockFix ? "are independent" : "are locked together: one spiral of directions for the whole screen, 7.3-frame pulses");
		}
	}
	c.ao = D3DXVECTOR4(rt.Distance, (float)rt.RayCount, rt.SampleLockFix ? 2.0f : 1.0f, rt.DebugView == 4 || rt.DebugView == 5 || rt.DebugView == 8 ? (float)rt.DebugView : 0.0f);	// (both consumers keep a history: the noise moves)
	c.sky = D3DXVECTOR4(rt.SkyDistance, rt.Sky && SkyData2.x > 0.0f ? (float)rt.SkyRays : 0.0f, 0.0f, 0.0f);
	// UNOFFICIAL 2026-11-08 (alsky) SkyInsideFix / SkySoftEdges ride in sky.y as 1000 x flags (RtAOTrace.comp SkyBlocked; the constants are full)
	if (c.sky.y > 0.0f) {
		const int skyFlags = (rt.SkyInsideFix ? 1 : 0) | (rt.SkySoftEdges ? 2 : 0);
		c.sky.y += 1000.0f * (float)skyFlags;
		static int loggedFlags = -1;	// (once per change: a tester's log says which sky rays drew the picture)
		if (skyFlags != loggedFlags) {
			loggedFlags = skyFlags;
			Logger::Log("UNOFFICIAL RT AO: sky rays: SkyInsideFix %s (a ray starting under the traced ground carries on past it), SkySoftEdges %s (cover fades toward the ray's end and past %.0f units).",
				rt.SkyInsideFix ? "on" : "off", rt.SkySoftEdges ? "on" : "off", 0.75f * 8000.0f);
		}
	}
	// (rt-ao-gi) one-bounce GI: AmbientLight takes it (its bounce light), so only while AmbientLight renders. The sun lights the hits
	// outdoors while it is up (the cascades' fade); its colour and the ambient as the game gives them (TESR_SunColor, TESR_SunAmbient),
	// linear.
	AmbientLightEffect* al = TheShaderManager->Effects.AmbientLight;
	// (2026-11-03) not in rect frames (RenderScale < 1): the bounce's consumers (AmbientLight's GI accumulate and apply) are Vulkan passes,
	// off there, and its reservoirs and screen light are not rect-aware yet (phase 2 of the plan); the reuse history starts again after.
	// UNOFFICIAL 2026-11-04 RT GI at RenderScale < 1 (docs/next/RT-RENDERSCALE.md "RT AO / GI plan", phase 2): it is traced in rect frames
	// now, over the same rect grid as the AO (projection.zw = traceW x traceH, so the trace's uv = texel / grid is the screen's and every
	// image it writes or reads by texel, the input, the frame light, the reservoirs, the bounce out, is in the rect layout). Its consumer
	// there is AmbientLight's D3D9 GI path (RT-PATH-TRACING.md section 16: Estimate / Accumulate / Apply, rect-aware like the rest of that
	// .fx), with NvrDxvkAmbientLight on or off: in a rect frame its Vulkan passes are off (ExtOn) and only the RT estimate runs as one, which
	// puts the bounce into rgb the same way. The reuse's reservoirs hold one frame's rect layout: a history made at another RenderRect scale
	// (or rect vs native) holds other texels and starts again (as the AO's RtHistoryScale).
	const bool gi = rt.GI && al && al->ShouldRender();
	{	// UNOFFICIAL 2026-11-13 (rtgi-review B3) GISamplePoints stores its reservoirs another way (A = light, W): a change of mode starts the history over
		static int lastPoints = -1;
		if (lastPoints != (RtAmbient::Current.GISamplePoints ? 1 : 0)) {
			if (lastPoints >= 0) RtGIHistory = false;
			lastPoints = RtAmbient::Current.GISamplePoints ? 1 : 0;
			Logger::Log("UNOFFICIAL RT AO: GI reuse: GISamplePoints %s (%s), GIValidate %s.", lastPoints ? "on" : "off",
				lastPoints ? "the reservoirs keep their sample points: cosine, Jacobian and a visibility ray on spatial reuse" : "the reservoirs keep the light only", RtAmbient::Current.GIValidate ? "on" : "off");
		}
	}
	if (gi && RtGIHistory && RtPlaceRestart()) RtGIHistory = false;	// UNOFFICIAL 2026-11-11 (sungi, F16): reuse.w 0 = no history (a new place / a sun jump)
	{
		static float giHistoryScale[2] = { 1.0f, 1.0f };
		if (giHistoryScale[0] != RtRectScaleX() || giHistoryScale[1] != RtRectScaleY() || RenderRect::ScaleChanged()) {
			if (RtGIHistory) Logger::Log("UNOFFICIAL RT AO: GI reuse history reset (RenderScale rect %.2f x %.2f -> %.2f x %.2f).", giHistoryScale[0], giHistoryScale[1],
				RtRectScaleX(), RtRectScaleY());
			RtGIHistory = false;
		}
		giHistoryScale[0] = RtRectScaleX();
		giHistoryScale[1] = RtRectScaleY();
	}
	if (gi) {
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		const bool sunUp = TheShaderManager->GameState.isExterior && shadows && shadows->Constants.ShadowFade.x < 1.0f;
		const D3DXVECTOR4 sunDir = shadows ? shadows->Constants.SmoothedSunDir : D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);
		const D3DXVECTOR4& sunColor = TheShaderManager->ShaderConst.sunColor;
		const D3DXVECTOR4& ambient = TheShaderManager->ShaderConst.sunAmbient;
		auto lin = [](float v) { return powf((std::max)(v, 0.0f), 2.2f); };
		c.sunDir = D3DXVECTOR4(sunDir.x, sunDir.y, sunDir.z, (sunUp ? 1.0f : 0.0f) + 0.25f);	// (w's fraction 0.25: 2026-11-13 rtgi-review R19, the hit textures' mip by the ray's footprint; the shader's sun test is > 0.5)
		// UNOFFICIAL 2026-11-04 (rtgap) GIGameLamps: x the game's light scale, as its lit shaders light the sun (LIN_SCALE_LIGHT: 0.49 by day
		// outdoors, 1 indoors / at night); the bounce hits were lit twice as strongly as the game lights the same surfaces by day
		PBRShaders* pbr = TheShaderManager->Shaders.PBR;
		const float lightScale = rt.GIGameLamps && pbr ? pbr->Constants.LinearScales.x : 1.0f;
		c.sunColor = D3DXVECTOR4(lin(sunColor.x) * lightScale, lin(sunColor.y) * lightScale, lin(sunColor.z) * lightScale, 0.0f);
		// UNOFFICIAL 2026-11-11 (sungi, F9) GIAmbientUnits: the ambient x the game's ambient scale (LIN_SCALE_AMBIENT = TESR_LinearScales.y: the
		// PBR AmbientScale, weather-blended and gamma-decoded), as the lit shaders scale it and as the sun is scaled above by .x; unscaled, the
		// sky / ambient share at a bounce hit was off by that factor. Same condition as the sun's scale (GIGameLamps and the PBR shaders).
		const float ambientScale = rt.GIGameLamps && rt.GIAmbientUnits && pbr ? pbr->Constants.LinearScales.y : 1.0f;
		c.ambient = D3DXVECTOR4(lin(ambient.x) * ambientScale, lin(ambient.y) * ambientScale, lin(ambient.z) * ambientScale, rt.GlowGI);	// (w: GlowGI, 2026-10-30)
		// (2026-10-30) sunColor.w 1: the core's glow atlas is bound ([Main.Main.RayTracing] GlowTextures): GlowGI takes the glow map's colour
		// at the hit (RtHit.glsl rtHitGlow) instead of the whole mesh's
		c.sunColor.w = RtCore::GlowAtlasImage() >= 0 && rt.GlowGI > 0.0f ? 1.0f : 0.0f;
		// UNOFFICIAL 2026-11-10 (gisky) GISky rides on gi.y (1 = GI rays; the shader only tests it against 0.5): 1 + GISky, outdoors only (a miss
		// indoors is a gap in the walls or the ray's end, not the sky). RtAOTrace.comp SkyMiss.
		if (RtAmbient::InteriorNow != !TheShaderManager->GameState.isExterior) {	// UNOFFICIAL 2026-11-10: the <Key>Interior values, on going in or out (the frame of the change still traces with the old ones)
			RtAmbient::InteriorNow = !TheShaderManager->GameState.isExterior;
			RtAmbient::UpdateSettings();
		}
		// UNOFFICIAL 2026-11-11 (sungi p2) AmbientReplace (AmbientLight Apply removes the game's ambient share): the sky light must come from the GI, so a ray miss
		// brings the full scene ambient (weight 1, already x LinearScales.y by GIAmbientUnits); GISky is ignored then.
		const bool replaceSky = al && al->AmbientReplaceActive();
		// UNOFFICIAL 2026-11-12 (sungi p5, user screenshot: blue grass/ground with SunGI and GIStrength 20) in replace mode the sky light of the misses
		// REPLACES the game's ambient, so it must come back at x1: AmbientLight multiplies the whole GI by GIStrength (its day/night blend, as
		// AmbientLight.cpp rtGIStrength), so the misses' weight is 1 / that strength (capped at 3.5: gi.y > 5 is SkyMissSH's flag). GIStrength then
		// scales only the light bounced off surfaces. Before: the sky came back x GIStrength (x20: the shade turned sky blue).
		const float giStrengthNow = TheShaderManager->GameState.isExterior && rt.GIStrengthNight >= 0.0f ? std::lerp(rt.GIStrengthNight, rt.GIStrength, TheShaderManager->GameState.transitionCurve) : rt.GIStrength;
		const float giSky = replaceSky ? std::clamp(1.0f / (std::max)(giStrengthNow, 0.05f), 0.0f, 3.5f) : !TheShaderManager->GameState.isExterior ? 0.0f : rt.GISkyNight < 0.0f ? rt.GISky : std::lerp(rt.GISkyNight, rt.GISky, TheShaderManager->GameState.transitionCurve);	// GISkyNight (2026-11-10): the night's own value
		c.gi = D3DXVECTOR4(rt.GIDistance, 1.0f + giSky, (float)RtGILamps(c), rt.GIAmbient);
		c.gi2 = D3DXVECTOR4(rt.GIOffscreen, 0.05f, rt.GIClamp, (float)rt.Interleave);
		// (GI v2) The game's lit frame, as it is now (the RT core's point: the world is drawn, NVR's effects are not yet), down to the
		// trace grid: on-screen bounce hits take their light from it (RtAOTrace.comp ScreenLight).
		IDirect3DSurface9* frame = nullptr;
		if (SUCCEEDED(Device->GetRenderTarget(0, &frame)) && frame) {
			Device->StretchRect(frame, nullptr, rtLightSurface, nullptr, D3DTEXF_LINEAR);
			frame->Release();
			// (2026-10-31, rt-ao-sky) [Main.Main.RayTracing] DenoiseBackbone: B1's object motion as a texel shift into its alpha (unused
			// before), for ReSTIR GI's reuse to follow walking people (RtAOTrace.comp GIMotion); gi2.y's sign tells the trace
			if (rt.GIReuse > 0 && TheSettingManager->GetSettingI("Main.Main.RayTracing", "DenoiseBackbone") != 0 &&
				RtDenoise::GIMotion(Device, rtLightSurface, traceW, traceH, RtRectScaleX(), RtRectScaleY())) c.gi2.y = -c.gi2.y;	// (2026-11-04: the rect grid)
		}
	}
	// UNOFFICIAL 2026-11-12 (sungi p3, F6) SkyMissSH: where ray misses bring sky light (gi.y > 1: GISky or AmbientReplace), the sky's own radiance
	// in the ray's direction. The forward shaders' sky harmonics (TESR_SkyIrradianceObjects: band l stored at A_l / pi of its projection, A0 = pi,
	// A1 = 2 pi / 3, A2 = pi / 4, i.e. 1, 2/3, 1/4 of the radiance's) un-convolved to radiance by 1, 1.5, 4 (SkyAmbient.hlsl SkyAmbientSharp),
	// x the GI's ambient scale (GIAmbientUnits: LinearScales.y). They go in row 1 of the lamp image (RtSkyData), gi.y + 10 tells the shaders.
	const float giYBase = c.gi.y;
	const bool skySh = gi && rt.SkyMissSH && c.gi.y > 1.0f && RtImageLamps >= 0 && TheShaderManager->Shaders.Sky;
	if (skySh) {
		PBRShaders* pbrSky = TheShaderManager->Shaders.PBR;
		const float scale = rt.GIGameLamps && rt.GIAmbientUnits && pbrSky ? pbrSky->Constants.LinearScales.y : 1.0f;
		const D3DXVECTOR4* sh = TheShaderManager->Shaders.Sky->Constants.IrradianceObjects;
		float flat[28] = {};
		for (int k = 0; k < 9; k++) {
			const float band = k == 0 ? 1.0f : k < 4 ? 1.5f : 4.0f;
			flat[3 * k] = sh[k].x * band * scale;
			flat[3 * k + 1] = sh[k].y * band * scale;
			flat[3 * k + 2] = sh[k].z * band * scale;
		}
		memcpy(RtSkyData, flat, sizeof(flat));
	}
	RtSkyDataOn = skySh;
	const int alphaAtlas = RtCore::AlphaAtlasImage() >= 0 ? RtCore::AlphaAtlasImage() : RtImageAlphaStandIn;
	if (alphaAtlas < 0) return;
	const int albedoAtlas = RtCore::AlbedoAtlasImage() >= 0 ? RtCore::AlbedoAtlasImage() : RtImageAlbedoStandIn;
	if (RtImageLight < 0 || albedoAtlas < 0) return;
	c.gi2.w = (float)rt.Interleave;	// (also without GI: the AO and sky rays interleave the same way)
	// (2026-11-03) In rect frames + the jitter the depth was drawn with (TAAEffect::DepthJitterUV, screen uv; 1 / scale bigger on screen
	// than at RenderScale 1): the depth texel lies on the ray through uv + jitter (RT sun shadows: without it far ground at grazing angles
	// shadowed itself). The constants are full (512 bytes), so it rides in viewToWorld as a shear of the view point: view.xy += viewZ x
	// (2 jx / P00, -2 jy / P11) (the trace's view = ((2 uv - 1) / P00, (1 - 2 uv) / P11, 1) x viewZ), i.e. row 2 += a x row 0 + b x row 1.
	// Only the AO / sky ray start uses the matrix then (no GI in rect frames); RenderScale 1 is left exactly as before.
	// (2026-11-04, phase 2: GI in rect frames) the GI ray start (the same p) takes the shear too; but the reuse inverts last frame's matrix
	// as its transpose (RT_GI_PASS: transpose(mat3(prevViewToWorld))), exact only for the plain rotation, so last frame's view is kept
	// without the shear (viewToWorldPlain). The merged pass rebuilds row 2 from rows 0 x 1 (untouched by the shear): the same. The jitter
	// left out of the reprojection is at most half a rendered pixel (a quarter of a trace texel); the reuse's depth / normal tests and its
	// neighbour tries absorb it.
	const D3DXMATRIX viewToWorldPlain = c.viewToWorld;
	if (RenderRect::On && TheShaderManager->Effects.TAA) {
		const D3DXVECTOR2 j = TheShaderManager->Effects.TAA->DepthJitterUV();
		const float a = 2.0f * j.x / proj._11, b = -2.0f * j.y / proj._22;
		c.viewToWorld._31 += a * c.viewToWorld._11 + b * c.viewToWorld._21;
		c.viewToWorld._32 += a * c.viewToWorld._12 + b * c.viewToWorld._22;
		c.viewToWorld._33 += a * c.viewToWorld._13 + b * c.viewToWorld._23;
	}
	// UNOFFICIAL 2026-11-07 (rtgap-tess; docs/next/RT-DETAIL-GAPS.md section 1a) the tessellated bumps' slack: the AO and sky rays start the
	// Strength further off the surface while the bumps are drawn (a hollow of the depth buffer lies under the flat RT surface). The constants
	// are full: it rides in viewToWorld's 4th row (the GLSL's 4th column, unused by the view's directions: they have w 0)
	{
		float slack[3]; DxvkCompute::DisplaceRtSlack(slack);
		c.viewToWorld._41 = slack[0]; c.viewToWorld._42 = slack[1]; c.viewToWorld._43 = (std::max)(slack[2], slack[1] + 1.0f);
	}
	// (2026-11-07 rtgap-decals) AOAlpha 1 (cut-outs solid, traced opaque) becomes 3 while the core traces decals: solid cut-outs still, but
	// tested one by one so the decals (NVR_RT_CUSTOM_DECAL) pass instead of occluding as solid quads (RtAOTrace.comp Trace)
	const int aoAlpha = rt.AOAlpha == 1 && RtCore::DecalsTraced() ? 3 : rt.AOAlpha;
	c.gi3 = D3DXVECTOR4((float)rt.GIInterleave, (float)aoAlpha, 0.0f, 0.0f);
	// (2026-10-30) GIReuse: the bounce in its own pass with ReSTIR GI's temporal reuse (RtGITrace: the GI fields, last frame's view and the
	// camera's move since, the reservoirs swapped every frame); the AO pass then traces no GI rays.
	static D3DXMATRIX prevViewToWorld;
	static D3DXVECTOR4 prevCamera;
	static UInt32 giParity = 0;
	// (2026-10-30) The kept samples are not re-traced, so a light switched off (or the sun at dusk) would linger for GIReuse frames.
	// When the light the bounce sees changes by more than 10 % (the lamps given: their total and brightest luminance and their
	// count; the sun's luminance and height), the history may stand for 1 frame of samples only, growing back by one each frame.
	static float prevLight[GILightKeys] = {};
	static float giCap = 0.0f;
	static float RtGIChange = 0.0f;	// UNOFFICIAL 2026-11-13 (rtgi-review R2) this frame's change of the bounce's light (0 none .. 1), for the cache's cap
	// (2026-10-30) At full rate (Interleave 1, GIInterleave 1) the reuse runs inside the AO pass (one pass instead of two: ~6 ms on the
	// iGPU, rt-gi-10/14), which then binds 10 images (the RT core's passes take up to 12 since rt-sun-core 6b9784c3); interleaved GI keeps
	// its own pass (its whole-grid dispatch and tiles).
	const bool reuse = gi && rt.GIReuse > 0 && RtImageReservoir[0][0] >= 0 && RtImageReservoir[1][1] >= 0;
	const bool merged = reuse && rt.Interleave == 1 && rt.GIInterleave == 1 && rt.GlowDirect <= 0.0f;	// (GlowDirect's list: the GI pass only)
	const bool giPass = reuse && !merged && RtGIShader >= 0;
	// (2026-10-31) RadianceCache (docs/next/FORK-RT-GAPS.md section 4): the cache's update pass before the GI, and the GI's off-screen
	// hits read it. The grid is fixed in the world: the camera's cell and where in it the camera is go to the passes (camera-relative
	// scene space, as everything else). The GI passes' constants are full, so they take 7 lamps and the grid in lamp slot 7.
	// (2026-10-31 b) interiors only unless CacheExteriors (outdoors: its own rays and spread). DebugView 7 (every bounce hit lit as off
	// screen) rides on lamp slot 7 too (lampColor[7].w + 16), with or without the cache.
	const bool exterior = TheShaderManager->GameState.isExterior;
	const bool cacheOn = rt.RadianceCache && rt.CacheStrength > 0.0f && (giPass || merged) && RtImageCache >= 0 && RtCacheShader >= 0 &&
		(rt.CacheExteriors || !exterior);
	// UNOFFICIAL 2026-11-03 (rtgi-cal, docs/next/RT-PATH-TRACING.md section 17) GIScreenLight off: every bounce hit takes the analytic light
	// (the same flag as DebugView 7), in every GI path (the AO pass's own bounce too: it reads lamp slot 7 the same way)
	const bool offscreenView = (rt.DebugView == 7 && (giPass || merged)) || (gi && !rt.GIScreenLight);
	const int cacheRays = exterior ? rt.CacheRaysExterior : rt.CacheRays, cacheSpread = exterior ? rt.CacheSpreadExterior : rt.CacheSpread;
	RtAmbient::CacheConstants cc = {}, cc2 = {};
	memset(&cc, 0, sizeof(cc)); memset(&cc2, 0, sizeof(cc2));	// (2026-11-13 rtgi-review R21)
	// the second level needs passes: the fine update, the coarse update, the AO / merged pass and the GI pass (of 8 a frame, the RT sun's among them)
	const bool cascade2 = cacheOn && rt.CacheCascades >= 2;
	// UNOFFICIAL 2026-11-12 (sungi p3 review) the layout flag follows the SETTING only; a frame short of pass slots just skips the coarse level's
	// update (its cells keep their light). Before, the flag itself followed PassesLeft(), and every flip started every cell over (GI pumping).
	const bool coarseUpdate = cascade2 && RtCore::PassesLeft() >= 4;
	const int cacheFlags = cacheOn ? (rt.CacheVisibility ? 1 : 0) | (cascade2 ? 2 : 0) : 0;
	// UNOFFICIAL 2026-11-13 (rtgi-review B6) GIScreenAmbientOut: with AmbientReplace active and on-screen hits taking the lit frame, those hits lose the
	// game's ambient share and gain the cache's irradiance (RtAOTrace.comp Bounce); + 32 in lamp slot 7's w (the off-screen flag is + 16)
	const bool screenAmbientOut = gi && rt.GIScreenAmbientOut && rt.GIScreenLight && rt.DebugView != 7 && al && al->AmbientReplaceActive() && exterior;
	// UNOFFICIAL 2026-11-13 (rtgi-review R21) the GI passes read lamp slot 7 as the cache's grid, strength and flags WHATEVER it holds: with the cache
	// off and no flag set this block used to be skipped, and the slot kept what was there. The slot is the cache's in every GI frame now.
	if (gi && !(cacheOn || offscreenView || screenAmbientOut)) {
		static int logged = 0;
		if (logged < 4) {
			logged++;
			Logger::Log("UNOFFICIAL RT AO: lamp slot 7 with the cache off held pos %.3f %.3f %.3f %.3f colour %.3f %.3f %.3f %.3f (gi.z %.1f): cleared (rtgi-review R21).",
				c.lampPos[RtAmbient::GILampsMax - 1].x, c.lampPos[RtAmbient::GILampsMax - 1].y, c.lampPos[RtAmbient::GILampsMax - 1].z, c.lampPos[RtAmbient::GILampsMax - 1].w,
				c.lampColor[RtAmbient::GILampsMax - 1].x, c.lampColor[RtAmbient::GILampsMax - 1].y, c.lampColor[RtAmbient::GILampsMax - 1].z, c.lampColor[RtAmbient::GILampsMax - 1].w, c.gi.z);
		}
		c.gi.z = (std::min)(c.gi.z, (float)(RtAmbient::GILampsMax - 1));
		c.lampPos[RtAmbient::GILampsMax - 1] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		c.lampColor[RtAmbient::GILampsMax - 1] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	if (cacheOn || offscreenView || screenAmbientOut) {
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		const double size = rt.CacheCellSize;
		const double cell[3] = { floor(camera.x / size), floor(camera.y / size), floor(camera.z / size) };
		// UNOFFICIAL 2026-11-12 (sungi p3) the cell size carries the v2 flags: size + 1024 x (1 CacheVisibility, 2 CacheCascades 2), only with the
		// cache on (RtAOTrace.comp CacheFlags / CacheSize). The coarse level's origin is derived from this cell in the shader (cell / 4).
		cc.cacheA = D3DXVECTOR4((float)(camera.x - cell[0] * size), (float)(camera.y - cell[1] * size), (float)(camera.z - cell[2] * size), (float)size + 1024.0f * (float)cacheFlags);
		cc.cacheB = D3DXVECTOR4((float)cell[0], (float)cell[1], (float)cell[2], cacheOn ? rt.CacheStrength : 0.0f);
		c.gi.z = (std::min)(c.gi.z, (float)(RtAmbient::GILampsMax - 1));
		c.lampPos[RtAmbient::GILampsMax - 1] = cc.cacheA;
		c.lampColor[RtAmbient::GILampsMax - 1] = cc.cacheB;
		if (offscreenView) c.lampColor[RtAmbient::GILampsMax - 1].w += 16.0f;	// (the cache pass's own cacheB keeps the plain strength)
		if (screenAmbientOut) c.lampColor[RtAmbient::GILampsMax - 1].w += 32.0f;	// (2026-11-13 B6)
	}
	RtAmbient::GIConstants g = {};
	memset(&g, 0, sizeof(g));	// (2026-11-13 rtgi-review R21)
	if (reuse && (giPass || merged)) {
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		float light[GILightKeys] = {};
		auto luma = [](const D3DXVECTOR4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
		for (int k = 0; k < RtAmbient::GILampsMax && k < (int)c.gi.z; k++) {	// (order-free: the list is sorted by distance)
			light[0] += luma(c.lampColor[k]);
			light[1] = (std::max)(light[1], luma(c.lampColor[k]));
		}
		light[2] = c.gi.z;
		light[3] = c.sunDir.w > 0.5f ? luma(c.sunColor) : 0.0f;	// (w: 0 / 1 + GIHitMips' 0.25)
		light[4] = c.sunDir.w > 0.5f ? c.sunDir.z * 10.0f : 0.0f;
		bool changed = false;
		for (int k = 0; k < GILightKeys; k++)
			if (fabsf(light[k] - prevLight[k]) > 0.1f * (std::max)((std::max)(fabsf(light[k]), fabsf(prevLight[k])), 0.01f)) changed = true;
		float giLightLast[GILightKeys];	// (2026-11-13 rtgi-review R2) last frame's keys, for the size of the change below
		memcpy(giLightLast, prevLight, sizeof(giLightLast));
		memcpy(prevLight, light, sizeof(light));
		// UNOFFICIAL 2026-11-13 (rtgi-review R2) the history cut by energy (no key since the settings cleanup). The user's log of 2026-11-13: 2660 cuts in 74400
		// frames (one every 30-45 frames outdoors by day), each one the reservoirs back to 1 frame AND the radiance cache back to 2 visits (112 frames to
		// grow back), all from the lamps' keys (the nearest 7 change as the player walks, fires flicker) while the sun was nearly all of the bounce.
		// Now the size of the change sets how much history may stay: change = the sun keys' largest relative change, or the lamp keys' x 0.1 while the
		// sun lights (outdoors by day their light is a few percent of the bounce) and x 1 otherwise; under 3 % nothing happens; else the history may hold
		// 0.5 / change frames (10 %: 5 frames, 25 %: 2, 50 % and up: 1), so the stale light left in it stays about the same small share. RtGIChange is
		// read by the cache's cap below the same way.
		RtGIChange = changed ? 1.0f : 0.0f;
		{
			auto rel = [&](int k) { return fabsf(light[k] - giLightLast[k]) / (std::max)((std::max)(fabsf(light[k]), fabsf(giLightLast[k])), 0.01f); };
			const float lampShare = c.sunDir.w > 0.5f && light[3] > 0.02f ? 0.1f : 1.0f;
			RtGIChange = (std::max)((std::max)(rel(3), rel(4)), lampShare * (std::max)((std::max)(rel(0), rel(1)), rel(2)));
			if (RtGIChange < 0.03f) RtGIChange = 0.0f;
			changed = RtGIChange > 0.0f;
			giCap = changed ? (std::min)(giCap, std::clamp(0.5f / RtGIChange, 1.0f, (float)rt.GIReuse)) : (std::min)(giCap + 1.0f, (float)rt.GIReuse);
		}
		static int cuts = 0, frames = 0;	// (logged every 600 GI frames: how often the light changed enough to cut the history)
		cuts += changed ? 1 : 0;
		if (++frames >= 600) { Logger::Log("UNOFFICIAL RT AO: GI reuse history cut %d times in the last 600 frames (the lamps or the sun changed by > 10 %%).", cuts); cuts = frames = 0; }
		// UNOFFICIAL 2026-11-08 (rtgi-ghost) + 4096 x the reuse fixes' flags (RtAOTrace.comp ReuseGI: 1 GIReuseSpread, 2 GIReuseShuffle, 4 SampleLockFix 2026-11-09)
		const int reuseFlags = (rt.GIReuseSpread ? 1 : 0) | (rt.GIReuseShuffle ? 2 : 0) | (rt.SampleLockFix ? 4 : 0) | (rt.GISamplePoints ? 16 : 0) | (rt.GISamplePoints && rt.GIValidate ? 32 : 0);	// (8 and 64 were GICandidates / GISpatialVisibility: removed 2026-11-13)	// (8: 2026-11-13 rtgi-review B1)
		{
			static int loggedFlags = -1;
			if (reuseFlags != loggedFlags) {
				loggedFlags = reuseFlags;
				Logger::Log("UNOFFICIAL RT AO: GI reuse: GIReuseSpread %s (a sample copied onto a surface now seen larger keeps that copy's share of its weight), GIReuseShuffle %s (last frame's sample from a shuffled texel of the 2 x 2 block); GI denoiser anti-lag %.1f.",
					rt.GIReuseSpread ? "on" : "off", rt.GIReuseShuffle ? "on" : "off", rt.GIDenoiseAntiLag);
			}
		}
		const D3DXVECTOR4 reuseNow = RtGIHistory ? D3DXVECTOR4(camera.x - prevCamera.x, camera.y - prevCamera.y, camera.z - prevCamera.z, (std::max)(giCap, 1.0f) + 64.0f * rt.GISpatial + 4096.0f * reuseFlags)
												 : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		const D3DXMATRIX& last = RtGIHistory ? prevViewToWorld : viewToWorldPlain;	// (2026-11-04: without the rect frames' jitter shear)
		if (giPass) {
			g.viewToWorld = c.viewToWorld;
			g.prevViewToWorld = last;
			g.projection = c.projection;
			g.origin = c.origin;
			g.gi = c.gi;
			g.sunDir = c.sunDir;
			g.sunColor = c.sunColor;
			g.ambient = c.ambient;
			memcpy(g.lampPos, c.lampPos, sizeof(g.lampPos));
			memcpy(g.lampColor, c.lampColor, sizeof(g.lampColor));
			g.gi2 = D3DXVECTOR4(c.gi2.x, c.gi2.y, c.gi2.z, (float)rt.GIInterleave);
			g.reuse = reuseNow;
			c.gi.y = 0.0f;	// (no GI rays in the AO pass)
		}
		else {
			// The shader takes last frame's point as M x (p + the camera's move), M = last frame's viewToWorld as stored (its rows, as the GI
			// pass's transpose(mat3(M)) reads it). 32 bytes of constants were free, the matrix is 64: rows 0 and 1 go, row 2 is their cross
			// x the determinant's sign (rt-gi-17: a quaternion could not hold the game's view; it need not be a pure rotation).
			const D3DXMATRIX& m = last;
			const D3DXVECTOR3 r0(m._11, m._12, m._13), r1(m._21, m._22, m._23), r2(m._31, m._32, m._33);
			D3DXVECTOR3 x;
			D3DXVec3Cross(&x, &r0, &r1);
			const float sign = D3DXVec3Dot(&x, &r2) < 0.0f ? -1.0f : 1.0f;
			c.prevRot = D3DXVECTOR4(r0.x, r0.y, r0.z, r1.x);
			c.reuse = D3DXVECTOR4(r1.y, r1.z, reuseNow.x, reuseNow.y);
			c.gi3.z = 1.0f;
			c.gi3.w = reuseNow.z;
			c.sky.z = reuseNow.w;
			c.sky.w = sign;
			static bool logged = false;
			if (!logged) { logged = true; Logger::Log("UNOFFICIAL RT AO: GI reuse at full rate runs inside the AO pass (one pass, 10 images; view determinant sign %+.0f).", sign); }
		}
		prevViewToWorld = viewToWorldPlain;	// (2026-11-04: = c.viewToWorld at RenderScale 1)
		prevCamera = camera;
	}
	const bool mergedNow = merged && c.gi3.z > 0.5f;
	const bool cacheNow = cacheOn && (giPass || mergedNow);
	bool cacheReset = false;
	if (cacheOn && !cacheNow) { c.lampColor[RtAmbient::GILampsMax - 1] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, (offscreenView ? 16.0f : 0.0f) + (screenAmbientOut ? 32.0f : 0.0f)); g.lampColor[RtAmbient::GILampsMax - 1] = c.lampColor[RtAmbient::GILampsMax - 1]; }
	if (cacheNow) {
		cc.origin = c.origin;
		cc.gi = c.gi;
		cc.sunDir = c.sunDir;
		cc.sunColor = c.sunColor;
		cc.ambient = c.ambient;
		memcpy(cc.lampPos, c.lampPos, sizeof(cc.lampPos));
		memcpy(cc.lampColor, c.lampColor, sizeof(cc.lampColor));
		cc.gi2 = c.gi2;
		// A new image or cell size: every cell traced and started over this frame. When the light changes (the reuse's history cut,
		// giCap back to 1) the cells may lean on 2 frames only, growing back to CacheFrames by one every CacheSpread frames (a cell's turn).
		cacheReset = !RtCacheValid || RtCacheCellSize != rt.CacheCellSize || RtCacheFlags != cacheFlags;	// (2026-11-12 sungi p3: the layout's flags too: the moments / the coarse level are not in the old cells)
		const UINT div = cacheReset ? 1u : (UINT)cacheSpread;
		// (2026-11-07 perf-history) CacheFrames is frames at 60 fps: the same seconds at this frame rate (RtHistoryTime.h)
		const float cacheFrames = RtHistoryTime::Frames((float)rt.CacheFrames, RtDenoise::HistoryClock());
		// (2026-11-13 rtgi-review R2) GICutByEnergy: on a change of light the cells keep 0.5 / change visits (at least 2), not 2 on every one
		if (cacheReset) RtCacheCap = 2.0f;
		else RtCacheCap = RtGIChange > 0.0f ? (std::max)(2.0f, (std::min)(RtCacheCap, 0.5f / RtGIChange)) : (std::min)(RtCacheCap + 1.0f / (float)div, cacheFrames);
		cc.cacheC = D3DXVECTOR4((float)cacheRays, cacheReset ? -RtCacheCap : (std::min)(RtCacheCap, cacheFrames),
			(float)(TheFrameRateManager->FrameNumber % div), (float)div);
		cc.cacheD = D3DXVECTOR4((float)((TheFrameRateManager->FrameNumber / div) % 4096u), (float)RtAmbient::CacheLazyPeriod, 2.5f, 0.0f);
		// UNOFFICIAL 2026-11-12 (sungi p3, F18) the coarse level's update: the same pass again with the level in cacheD.w, 1 in 2 x the spread's cells a
		// frame (the grid is 4 x as large and changes 4 x as slowly; the cost stays that of the fine level's half); all cells in one frame on a reset
		cc2 = cc;
		const UINT div2 = cacheReset ? 1u : (std::min)(div * 2u, 64u);
		cc2.cacheC.z = (float)(TheFrameRateManager->FrameNumber % div2);
		cc2.cacheC.w = (float)div2;
		cc2.cacheD.x = (float)((TheFrameRateManager->FrameNumber / div2) % 4096u);
		cc2.cacheD.w = 1.0f;
	}
	// UNOFFICIAL 2026-11-03 (rtgi-lamps) more lamps than the constants carry (8, or 7 with lamp slot 7 the cache's / GIScreenLight's): all
	// of them through the lamp image, every GI pass (gi.z = -count; the reuse's light keys above stay the nearest ones'). Few lamps
	// (outdoors) keep the constants' path exactly as before; GI off never gets here (no lamps listed).
	bool lampImage = false;
	if (gi && RtLampCount > (int)(c.gi.z + 0.5) && RtImageGIOut >= 0 && RtImageReservoir[1][1] >= 0 && RtImageCache >= 0 && RtFillLamps(Device)) {
		lampImage = true;
		c.gi.z = g.gi.z = cc.gi.z = -(float)RtLampCount;
		static bool lampsLogged = false;
		if (!lampsLogged) { lampsLogged = true; Logger::Log("UNOFFICIAL RT AO: GI lights its bounce hits from all %d lamps (lamp image; the constants hold %d).", RtLampCount, RtAmbient::GILampsMax); }
	}
	// UNOFFICIAL 2026-11-12 (sungi p3, F6) SkyMissSH: the lamp image carries the sky harmonics (row 1) even with few lamps: it is filled and bound
	// for the passes then (the lamps in the constants stay as they are: gi.z >= 0). gi.y + 10 = directional sky misses (RtAOTrace.comp SkyMiss),
	// in every pass that has GI rays (the AO pass of a split GI has none: its gi.y is 0) and in the cache passes (their gi.y was zeroed with it).
	bool lampsBound = lampImage;
	if (skySh && !lampImage) lampsBound = RtFillLamps(Device);
	const bool skyShNow = skySh && lampsBound;
	if (skyShNow) {
		if (c.gi.y > 0.5f) c.gi.y += 10.0f;
		if (g.gi.y > 0.5f) g.gi.y += 10.0f;
		cc.gi.y = cc2.gi.y = giYBase + 10.0f;
	}
	const int cur = giParity & 1, prev = cur ^ 1;
	// (2026-10-30) the glow atlas as image 11 (binding 15): the merged pass's last; the plain AO pass binds 6-10 too then (not used there:
	// the reservoirs and the GI out, unwritten without gi3.z), so the atlas lands at its binding
	const bool glowAtlas = gi && c.sunColor.w > 0.5f;
	// (2026-11-03 rtgi-lamps) + the lamp image as image 12 (binding 16); image 11 (binding 15, rgba8) then the albedo atlas when no glow atlas
	const bool aoLamps = lampsBound;	// (its fillers checked with it) (2026-11-12 sungi p3: also with the sky harmonics in the lamp image)
	const int images[13] = { RtImageIn, RtImageOut, cacheNow && mergedNow ? RtImageCache : RtImageGI, alphaAtlas, RtImageLight, albedoAtlas,
		RtImageReservoir[prev][0], RtImageReservoir[prev][1], RtImageReservoir[cur][0], RtImageReservoir[cur][1],
		RtImageGIOut, glowAtlas ? RtCore::GlowAtlasImage() : aoLamps ? albedoAtlas : RtImageGIOut, RtImageLamps };
	const UINT aoImages = aoLamps ? 13u : mergedNow ? (glowAtlas ? 12u : 11u) : (glowAtlas && RtImageGIOut >= 0 && RtImageReservoir[1][1] >= 0 ? 12u : 6u);
	const int giImages[13] = { RtImageIn, RtImageReservoir[prev][0], RtImageReservoir[prev][1], alphaAtlas, RtImageLight, albedoAtlas,
		RtImageReservoir[cur][0], RtImageReservoir[cur][1], RtImageGIOut, RtImageGlowList,
		glowAtlas ? RtCore::GlowAtlasImage() : lampsBound ? albedoAtlas : RtImageGlowList,
		RtImageCache >= 0 ? RtImageCache : RtImageGIOut, RtImageLamps };	// (2026-10-31: the radiance cache as image 11, binding 15, with RadianceCache;
																			// 2026-11-03 rtgi-lamps: the lamp image as 12, binding 16)
	// (2026-10-31) the cache pass: the cache at binding 2, the atlases at theirs (5, 7, 15); the bindings between are fillers (unused)
	const int cacheImages[13] = { RtImageCache, RtImageCache, RtImageCache, alphaAtlas, RtImageCache, albedoAtlas, RtImageCache, RtImageCache,
		RtImageCache, RtImageCache, RtImageCache, glowAtlas ? RtCore::GlowAtlasImage() : lampsBound ? albedoAtlas : RtImageCache, RtImageLamps };
	const UINT cacheGroups = RtAmbient::CacheCells / (cacheNow ? (UINT)cc.cacheC.w : 1u) / 64u;
	const UINT cacheGroups2 = RtAmbient::CacheCells / (cacheNow ? (std::max)((UINT)cc2.cacheC.w, 1u) : 1u) / 64u;	// (2026-11-12 sungi p3: the coarse level)
	const bool glowOk = (giPass || mergedNow) && RtImageGIOut >= 0 && RtImageGlowList >= 0 && RtFillGlowLights(Device, rt.GlowDirect);
	if ((giPass || mergedNow) && !glowOk) { RtGIHistory = false; return; }
	if (!RtCore::CopyIn(RtImageIn, rtInputTexture) || (gi && !RtCore::CopyIn(RtImageLight, rtLightTexture)) ||
		(glowOk && !RtCore::CopyIn(RtImageGlowList, RtGlowTexture)) ||
		(cacheNow && !RtCore::AddPass(RtCacheShader, cacheImages, lampsBound ? 13 : glowAtlas ? 12 : 6, 0x1, &cc, sizeof(cc), cacheGroups, 1)) ||
		// UNOFFICIAL 2026-11-12 (sungi p3, F18) the coarse cascade's update (cc2: level 1, 1 in 2 x the spread's cells a frame)
		(cacheNow && coarseUpdate && !RtCore::AddPass(RtCacheShader, cacheImages, lampsBound ? 13 : glowAtlas ? 12 : 6, 0x1, &cc2, sizeof(cc2), cacheGroups2, 1)) ||
		!RtCore::AddPass(RtShader, images, aoImages, mergedNow ? (cacheNow ? 0x702 : 0x706) : 0x6, &c, sizeof(c), ((rt.Interleave >= 2 ? (traceW + 1) / 2 : traceW) + 7) / 8,
			((rt.Interleave >= 4 ? (traceH + 1) / 2 : traceH) + 7) / 8) ||	// (Interleave: only the traced texels' threads; 2026-11-03: over the rect)
		(giPass && !RtCore::AddPass(RtGIShader, giImages, lampsBound ? 13 : cacheNow ? 12 : glowAtlas ? 11 : 10, 0x1C0, &g, sizeof(g), (traceW + 7) / 8, (traceH + 7) / 8)) ||	// (2026-11-04: the rect)
		!RtCore::CopyOut(RtImageOut, rtRawSurface) ||
		(gi && !RtCore::CopyOut(giPass || mergedNow ? RtImageGIOut : RtImageGI, rtGISurface))) { RtGIHistory = false; RtCacheValid = false; return; }
	RtCacheValid = cacheNow;	// (a frame without the cache pass: the next one starts the cells over)
	RtCacheCellSize = rt.CacheCellSize;
	RtCacheFlags = cacheFlags;
	{	// UNOFFICIAL 2026-11-12 (sungi p3) what is active, once per change
		static int loggedV2 = -1;
		const int key = (cacheNow ? 1 : 0) | (cacheNow && (cacheFlags & 1) ? 2 : 0) | (cacheNow && (cacheFlags & 2) ? 4 : 0) | (skyShNow ? 8 : 0);
		if (key != loggedV2 && (key != 0 || loggedV2 > 0)) {
			Logger::Log("UNOFFICIAL RT AO: sun-GI cache v2: CacheVisibility %s (Chebyshev weights from the cells' distance moments), CacheCascades %d (%s), SkyMissSH %s (ray misses bring the sky's harmonics in their direction; lamp image row 1).",
				cacheNow && (cacheFlags & 1) ? "on" : "off", cacheNow && (cacheFlags & 2) ? 2 : 1,
				cacheNow && (cacheFlags & 2) ? "the fine grid + a 4 x coarser one, updated 1 in 2 x the spread's cells" : "the single fine grid, as before", skyShNow ? "on" : "off");
		}
		loggedV2 = key;
	}
	if (cacheNow) {
		static bool cacheLogged = false;
		if (!cacheLogged || cacheReset) {
			Logger::Log("UNOFFICIAL RT AO: radiance cache %s (32 x 32 x 16 cells of %.0f units, %d rays a cell, 1 in %d cells a frame, up to %d frames, lazy cells 1 visit in %u; %s; camera cell %.0f %.0f %.0f; %s).",
				cacheReset ? "started over" : "on", rt.CacheCellSize, cacheRays, cacheSpread, rt.CacheFrames, RtAmbient::CacheLazyPeriod, exterior ? "exterior" : "interior",
				cc.cacheB.x, cc.cacheB.y, cc.cacheB.z, mergedNow ? "read by the merged AO + GI pass" : "read by the GI pass");
			cacheLogged = true;
		}
	}
	if (giPass || mergedNow) giParity++;
	RtGIHistory = giPass || mergedNow;
	RtGITraced = gi;
	RtTracedFrame = TheFrameRateManager->FrameNumber;
	static int rectLogs = 0;	// (2026-11-03)
	if (RenderRect::On && rectLogs < 2) {
		rectLogs++;
		Logger::Log("UNOFFICIAL RT AO: traced in the RenderScale rect: %ux%u of %ux%u (scale %.2f x %.2f; GI %s).", traceW, traceH, width, height,
			RenderRect::ScaleX, RenderRect::ScaleY, !gi ? "off" : giPass ? "its own pass over the rect" : mergedNow ? "in the AO pass with reuse" : "in the AO pass");	// (2026-11-04)
	}
	if (traced++ == 0)
		Logger::Log("UNOFFICIAL RT AO: traced (%ux%u, %d AO rays of %.0f units, sky %s, GI %s).", width, height, rt.RayCount, rt.Distance,
			c.sky.y > 0.0f ? "on" : "off (Skylighting off or RT Sky off)", gi ? "on (1 bounce ray)" : "off");
	static bool giLogged = false;
	if (gi && !giLogged) {
		giLogged = true;
		Logger::Log("UNOFFICIAL RT AO: GI traced (1 bounce ray of %.0f units, %d lamps, sun %s, %s).", rt.GIDistance, (int)c.gi.z, c.sunDir.w > 0.5f ? "up" : "down / indoors",
			giPass ? "its own pass with ReSTIR GI temporal reuse (GIReuse)" : mergedNow ? "in the AO pass with ReSTIR GI temporal reuse" : "in the AO pass");
	}
}

// UNOFFICIAL 2026-10-28 (fork RT, [Shaders.AmbientOcclusion.RT]; core/RtAmbient.h): Estimate's place in ExtDedicated. The traced
// visibility (RtRaw) as Estimate's result into AO 1 (AmbientOcclusionEstimateRT.frag), then the temporal pass (AmbientOcclusionTemporalRT.frag) AO 1 + last frame's history -> AO 0 (what
// the blurs read, as Estimate's) + this frame's history. False: nothing usable was drawn (Estimate runs); a failed draw does not take the
// other Vulkan passes down (ExtFailed is put back) but ends the RT AO for the session.
bool AmbientOcclusionEffect::ExtEstimateRT(IDirect3DDevice9* Device, IDirect3DTexture9* const* Textures, IDirect3DSurface9* const* Surfaces, const D3DSURFACE_DESC& Target) {
	static FailOnce::Latch rtFailed{ "AmbientOcclusion RT estimate" };	// UNOFFICIAL 2026-11-02 RetryFailedEffects (was a bool until restart)
	if (rtFailed.Blocked()) return false;
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	if (!depth || !Textures[1] || !Surfaces[0] || !Surfaces[1]) return false;
	if (rtHistoryWidth != Target.Width || rtHistoryHeight != Target.Height) {
		for (int i = 0; i < 2; i++) { DxvkCompute::Drop(rtHistorySurface[i]); DxvkCompute::Drop(rtHistoryTexture[i]); }
		TheTextureManager->InitTexture("NVR_AORTHistory0", &rtHistoryTexture[0], &rtHistorySurface[0], Target.Width, Target.Height, D3DFMT_G16R16F);
		TheTextureManager->InitTexture("NVR_AORTHistory1", &rtHistoryTexture[1], &rtHistorySurface[1], Target.Width, Target.Height, D3DFMT_G16R16F);
		rtHistoryWidth = Target.Width;
		rtHistoryHeight = Target.Height;
		RtHistoryValid = false;
		RtTemporal.x = 0.0f;
	}
	if (!rtHistoryTexture[0] || !rtHistoryTexture[1] || !rtHistorySurface[0] || !rtHistorySurface[1]) return false;
	IDirect3DTexture9* raw = RtRaw();
	if (!raw) return false;
	if (IDirect3DTexture9* denoised = RtDenoised(Device)) raw = denoised;	// (2026-10-29) .x the denoised visibility (screen size, read by uv)
	MakePipelineRT(extEstimateRT, "AmbientOcclusionEstimateRT", 2, 1);
	MakePipelineRT(extTemporalRT, "AmbientOcclusionTemporalRT", 3, 2);
	if (!extEstimateRT.Pipeline || !extTemporalRT.Pipeline) return false;
	static GpuTimer rtTimers[2] = { GpuTimer("  AO RT estimate (half)"), GpuTimer("  AO RT temporal (half)") };
	const bool wasFailed = ExtFailed;
	const int read = rtHistoryIndex, write = rtHistoryIndex ^ 1;
	NVR_EXT_RESOURCE_BINDING resources[8] = {};
	UINT count = 0;
	AOPush push = {};
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (docs\next\FORK2-DLSS-RR.md): while RR denoises the frame (Upscaler::
	// RayReconstructionWantsRaw) the estimate goes straight into AO 0, where the blurs and the combine read it, and the temporal pass
	// does not run (with no history share it would copy AO 1 into AO 0 unchanged). No history is kept: the first frame after starts afresh.
	const bool rrRaw = Upscaler::RayReconstructionWantsRaw();
	bool drawn;
	{
		GpuProfileScope gpu(rtTimers[0], Device);
		IDirect3DBaseTexture9* inputs[2] = { depth, raw };
		drawn = Resources(resources, count, inputs, 2, extUniforms.Buffer) && ExtDrawTo(extEstimateRT.Pipeline, &Surfaces[rrRaw ? 0 : 1], 1, resources, count, &push, sizeof(push));
	}
	if (drawn && !rrRaw) {
		GpuProfileScope gpu(rtTimers[1], Device);
		IDirect3DBaseTexture9* inputs[3] = { Textures[1], rtHistoryTexture[read], depth };
		IDirect3DSurface9* targets[2] = { Surfaces[0], rtHistorySurface[write] };
		drawn = Resources(resources, count, inputs, 3, extUniforms.Buffer) && ExtDrawTo(extTemporalRT.Pipeline, targets, 2, resources, count, &push, sizeof(push));
	}
	if (!drawn) {
		ExtFailed = wasFailed;
		rtFailed.Set("the RT estimate failed to draw: AmbientOcclusion's own estimate runs");
		return false;
	}
	if (rrRaw) {	// UNOFFICIAL 2026-10-30 DLSS RR (above)
		RtHistoryValid = false;
		static bool logged = false;
		if (!logged) { logged = true; Logger::Log("UNOFFICIAL DLSS RR: AmbientOcclusion: its own denoiser is skipped (RR denoises the raw signal)"); }
		return true;
	}
	rtHistoryIndex = write;
	RtHistoryValid = true;
	RtHistoryScale[0] = RtRectScaleX();	// (2026-11-03) the scale this history was made at
	RtHistoryScale[1] = RtRectScaleY();
	RtLastTime = TheFrameRateManager->Time;
	RtLastView = TheRenderManager->viewMatrix;
	RtLastInvView = TheRenderManager->InvViewMatrix;
	RtLastProj = TheRenderManager->projMatrix;
	RtFrame++;
	return true;
}

// UNOFFICIAL 2026-10-28 (fork RT) [Shaders.AmbientOcclusion.RT] Sky: SkyVisibilityV2's place, rays instead of the ortho map
// (AmbientOcclusionSky.frag's inputs less the map; AmbientOcclusionSkyRT.frag). Same failure rule as ExtEstimateRT.
bool AmbientOcclusionEffect::ExtSkyVisibilityRT(IDirect3DSurface9* Target, IDirect3DTexture9* SunMask, IDirect3DTexture9* SmoothedNormals) {
	static FailOnce::Latch rtFailed{ "Skylighting RT sky pass" };	// UNOFFICIAL 2026-11-02 RetryFailedEffects (was a bool until restart)
	D3DSURFACE_DESC desc = {};
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	if (rtFailed.Blocked() || !depth || !normals || !Target || FAILED(Target->GetDesc(&desc)) || !desc.Width || !desc.Height) return false;
	IDirect3DTexture9* raw = RtRaw();
	if (!raw) return false;
	MakePipelineRT(extSkyRT, "AmbientOcclusionSkyRT", 6, 1);
	const D3DXVECTOR4 skyLayout(1.0f / desc.Width, 1.0f / desc.Height, SmoothedNormals ? 1.0f : 0.0f, 0.0f);
	if (!extSkyRT.Pipeline || !ExtConstants(extSkyUniforms, D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f), skyLayout)) return false;
	if (!rtInputTexture) return false;
	IDirect3DBaseTexture9* inputs[6] = { depth, normals, SunMask ? (IDirect3DBaseTexture9*)SunMask : depth,
		SmoothedNormals ? (IDirect3DBaseTexture9*)SmoothedNormals : depth, raw, rtInputTexture };
	NVR_EXT_RESOURCE_BINDING resources[10] = {};
	UINT count = 0;
	AOPush push = {};
	const bool wasFailed = ExtFailed;
	if (Resources(resources, count, inputs, 6, extSkyUniforms.Buffer) && ExtDrawTo(extSkyRT.Pipeline, &Target, 1, resources, count, &push, sizeof(push))) return true;
	ExtFailed = wasFailed;
	rtFailed.Set("the RT sky pass failed to draw: Skylighting's ortho map pass runs");
	return false;
}

/*
* UNOFFICIAL 2026-10-17 Skylighting v2: the sky pass (technique SkyVisibilityV2) into the target the size of this frame's AO target,
* with the ortho map on stage 6 and the sun shadow mask on stage 8. Called from Render before the AO passes; sets skyReady.
* UNOFFICIAL 2026-10-20 (skyamb): also called by ShaderManager just before AmbientLight while that hosts the sky test.
*/
void AmbientOcclusionEffect::RenderSkyVisibility(IDirect3DDevice9* Device, IDirect3DTexture9* smoothedNormals) {
	IDirect3DTexture9* orthoMap = TheShaderManager->Effects.ShadowsExteriors ? TheShaderManager->Effects.ShadowsExteriors->ShadowMapOrthoTexture : nullptr;
	IDirect3DTexture9* sunMask = TheShaderManager->Effects.ShadowsExteriors && TheShaderManager->Effects.ShadowsExteriors->Enabled ?
		TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowPassTexture : nullptr;
	D3DXHANDLE technique = TechniqueHandle(techSky, "SkyVisibilityV2");
	// fixes-b #3: create this resolution's sky target on first use (once; a failure is logged once and v2 stays off)
	static bool createTried[2] = { false, false };
	const int which = lastLowRes ? 1 : 0;
	// UNOFFICIAL 2026-10-19: the sky reflections ([Shaders.AmbientOcclusion.Skylighting] Reflections) need a third channel (b, the
	// visibility itself); without them the target keeps the two it had (half the bytes to write and to read back). Switching
	// Reflections recreates this resolution's target (its users take it from SkyTexture() each frame).
	// UNOFFICIAL 2026-10-20 (skyamb): LightCap reads the b too (Includes/SkyTake.hlsl).
	static D3DFORMAT createdFormat[2] = { D3DFMT_UNKNOWN, D3DFMT_UNKNOWN };
	const D3DFORMAT wanted = SkyReflections || SkyLightCap > 0.0f ? D3DFMT_A16B16G16R16F : D3DFMT_G16R16F;
	if (createTried[which] && createdFormat[which] != wanted) {
		IDirect3DTexture9*& texture = lastLowRes ? skyTextureLow : skyTexture;
		IDirect3DSurface9*& surface = lastLowRes ? skySurfaceLow : skySurface;
		if (surface) { surface->Release(); surface = nullptr; }
		if (texture) { texture->Release(); texture = nullptr; }
		skyReady = false;
		createTried[which] = false;
	}
	const bool rtSky = RtAmbient::SkyReady();	// UNOFFICIAL 2026-10-28 (fork RT): the RT sky needs no ortho map
	if (technique && (orthoMap || rtSky) && !createTried[which]) {
		createTried[which] = true;
		createdFormat[which] = wanted;
		if (lastLowRes)
			TheTextureManager->InitTexture("NVR_SkyVisBufferLow", &skyTextureLow, &skySurfaceLow, MaxQuality::Reduced(TheRenderManager->width, 4), MaxQuality::Reduced(TheRenderManager->height, 4), wanted);
		else
			TheTextureManager->InitTexture("NVR_SkyVisBuffer", &skyTexture, &skySurface, MaxQuality::Reduced(TheRenderManager->width, 2), MaxQuality::Reduced(TheRenderManager->height, 2), wanted);
	}
	IDirect3DSurface9* target = lastLowRes ? skySurfaceLow : skySurface;
	D3DSURFACE_DESC desc = {};
	if (!technique || !(orthoMap || rtSky) || !target || FAILED(target->GetDesc(&desc))) return;
	static GpuTimer skyTimer("  Sky visibility v2");
	GpuProfileScope gpu(skyTimer, Device);
	D3DVIEWPORT9 original = {};
	Device->GetViewport(&original);
	D3DVIEWPORT9 quarter = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
	SkyData4.z = sunMask ? 1.0f : 0.0f;
	Effect->SetTechnique(technique);
	// UNOFFICIAL 2026-10-21 (alrocks): the smoothed normal from AmbientLight's prep (stage 9) when it was drawn this frame.
	D3DXHANDLE smoothHandle = ParameterHandle(paramSkySmooth, "NVR_SkySmooth");
	if (!smoothHandle) smoothedNormals = nullptr;
	if (smoothHandle) { D3DXVECTOR4 smooth(smoothedNormals ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f); Effect->SetVector(smoothHandle, &smooth); }
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the Vulkan pass first (AmbientOcclusionSky.frag); the D3D9 pass when it is off or failed.
	HRESULT result = E_FAIL;
	if (rtSky && !ExtFailed && ExtSkyVisibilityRT(target, sunMask, smoothedNormals)) result = S_OK;	// UNOFFICIAL 2026-10-28 (fork RT)
	else if (!orthoMap) result = E_FAIL;	// (the RT sky failed and no map was drawn for it: no factor this frame)
	else if (!ExtFailed && DxvkCompute::PassesOn() && ExtSkyVisibility(target, desc, orthoMap, sunMask, smoothedNormals)) result = S_OK;
	else {
	// UNOFFICIAL 2026-10-20 (skyamb): SetCT does nothing while this effect is off, and it is off whenever AmbientLight hosts the sky
	// test (ShaderManager switches it off for the frame; the user may have it off too): the pass then ran with no depth, normals or
	// constants (measured: no darkening, the debug views did not change). On for the call.
	const bool wasEnabled = Enabled;
	Enabled = true;
	SetCT();
	Enabled = wasEnabled;
	UINT passes = 0;
	IDirect3DSurface9* depthSurface = nullptr;	// no depth-stencil while this draws (restored below)
	Device->GetDepthStencilSurface(&depthSurface);
	Device->SetDepthStencilSurface(nullptr);
	result = Device->SetRenderTarget(0, target);
	if (SUCCEEDED(result)) result = Device->SetViewport(&quarter);
	if (SUCCEEDED(result)) result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		result = Effect->BeginPass(0);
		if (SUCCEEDED(result)) {
			Device->SetTexture(6, orthoMap);
			Device->SetTexture(8, sunMask);
			if (smoothedNormals) { Device->SetTexture(9, smoothedNormals); TheRenderManager->renderState->NoteDeviceTexture(9, smoothedNormals); }
			TheRenderManager->renderState->NoteDeviceTexture(6, orthoMap);
			TheRenderManager->renderState->NoteDeviceTexture(8, sunMask);
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2
			result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Effect->EndPass();
		}
		Effect->End();
	}
	Device->SetTexture(6, nullptr);
	Device->SetTexture(8, nullptr);
	if (smoothedNormals) { Device->SetTexture(9, nullptr); TheRenderManager->renderState->NoteDeviceTexture(9, nullptr); }
	TheRenderManager->renderState->NoteDeviceTexture(6, nullptr);
	TheRenderManager->renderState->NoteDeviceTexture(8, nullptr);
	Device->SetViewport(&original);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	}
	skyReady = SUCCEEDED(result);
	static D3DFORMAT reported = D3DFMT_UNKNOWN;	// (again when Reflections switches the format)
	if (skyReady && reported != desc.Format) {
		Logger::Log("UNOFFICIAL Skylighting v2 active: sky factor %ux%u %s (sun mask %s), applied to the sky's share%s.", desc.Width, desc.Height,
			desc.Format == D3DFMT_A16B16G16R16F ? "A16B16G16R16F (b: the visibility, for the sky reflections and LightCap)" : "G16R16F", sunMask ? "yes" : "no",
			SkyHostedByAmbientLight() ? " (in AmbientLight's apply)" : "");
		reported = desc.Format;
	}
}

bool AmbientOcclusionEffect::ShouldRender() {
	if (RtPathTrace::Replacing()) return false;	// UNOFFICIAL 2026-11-03 PTReplace: the path traced frame has its own occlusion
	return Constants.Enabled && !bNVAOLoaded;
}

void AmbientOcclusionEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget,
	IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget,
	IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { NoteSkip(); renderTime = 0; return; }
	// combineOnly reapplies this frame's deferred estimate, so it keeps that estimate's resolution.
	if (!combineOnly) lastLowRes = UseLowRes();
	if (!combineOnly && SkyData2.x > 0.0f) {	// UNOFFICIAL Skylighting v2: this frame's sky visibility first
		RenderSkyVisibility(Device);
		Device->SetRenderTarget(0, RenderTarget);
	}
	IDirect3DTexture9* const* textures = lastLowRes ? aoTextureLow : aoTexture;
	IDirect3DSurface9* const* surfaces = lastLowRes ? aoSurfaceLow : aoSurface;
	const UINT divisor = MaxQuality::Divisor(lastLowRes ? 4 : 2);	// UNOFFICIAL: 1 with Max Quality
	D3DXHANDLE technique = TechniqueHandle(techDedicated, "DedicatedAO");
	if (FastPaths) {	// UNOFFICIAL 2026-10-17 FastPaths (above)
		D3DXHANDLE fast = TechniqueHandle(techFast, "DedicatedAOFast");
		if (fast) technique = fast;
	}
	bool gtaoTechnique = false;	// (NVR DXVK: which estimate the Vulkan pass computes)
	if (GTAOData.x > 0.5f) {	// UNOFFICIAL 2026-10-16: the same passes with the GTAO estimate, when the effect file has them
		D3DXHANDLE gtao = FastPaths ? TechniqueHandle(techGTAOFast, "DedicatedGTAOFast") : NULL;	// UNOFFICIAL 2026-10-17 FastPaths
		if (!gtao) gtao = TechniqueHandle(techGTAO, "DedicatedGTAO");
		if (gtao) { technique = gtao; gtaoTechnique = true; }
	}
	D3DXHANDLE layoutHandle = ParameterHandle(paramLayout, "NVR_AOLayout");
	D3DXTECHNIQUE_DESC description = {};
	D3DVIEWPORT9 original = {};
	D3DSURFACE_DESC target = {}, scratch = {}, aoTarget = {};
	IDirect3DSurface9* depthSurface = nullptr;
	// Older/custom effect files keep the legacy path. Never guess their pass layout.
	if (packedAOFailed || !technique || !layoutHandle || FAILED(Effect->GetTechniqueDesc(technique, &description)) ||
		description.Passes != 4 || !RenderedSurface || !SourceBuffer || !textures[0] || !textures[1] ||
		!surfaces[0] || !surfaces[1] ||
		FAILED(Device->GetViewport(&original)) || FAILED(RenderTarget->GetDesc(&target)) ||
		FAILED(RenderedSurface->GetDesc(&scratch)) || FAILED(surfaces[0]->GetDesc(&aoTarget)) ||
		original.X || original.Y ||
		// UNOFFICIAL 2026-10-27 RenderScale v2: in a rect frame an earlier pass may have left the target's drawn part as the viewport
		!((original.Width == target.Width && original.Height == target.Height) || (RenderRect::Active() &&
			original.Width == RenderRect::Width(target.Width) && original.Height == RenderRect::Height(target.Height))) ||
		scratch.Width != target.Width || scratch.Height != target.Height ||
		aoTarget.Width != (target.Width + divisor - 1) / divisor || aoTarget.Height != (target.Height + divisor - 1) / divisor ||
		aoTarget.Format != D3DFMT_G16R16F ||
		target.MultiSampleType != D3DMULTISAMPLE_NONE) {
		// Let the caller restore deferred shadows before running legacy AO.
		if (deferCombine) return;
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	// (RenderScale v2: the layout below and the final restore are for the whole target; ApplyViewport sets the drawn part per draw.)
	original.Width = target.Width;
	original.Height = target.Height;
	auto timer = TimeLogger();
	// Under the frame chain the combine writes the chain's spare texture, which then becomes the
	// current image; otherwise it writes the render target and is copied back as before.
	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(RenderTarget, RenderedSurface);
	IDirect3DSurface9* finalTarget = chained ? chain.Output() : RenderTarget;
	D3DVIEWPORT9 reduced = original;
	reduced.Width = aoTarget.Width;
	reduced.Height = aoTarget.Height;
	D3DXVECTOR4 layout((float)reduced.Width / original.Width,
		(float)reduced.Height / original.Height, 1.0f / reduced.Width, 1.0f / reduced.Height);
	// The combine reads the scene through the TESR_SourceBuffer slot (s2). RenderedSurface already
	// equals the render target when AO starts and nothing here writes it before the final copy,
	// so bind it there instead of copying the full frame into SourceBuffer.
	IDirect3DTexture9* scene = TheTextureManager->RenderedTexture;
	// UNOFFICIAL Skylighting v2, DedicatedCombine (not deferred): the sky factor on stage 7.
	IDirect3DTexture9* skyVis = !deferCombine ? SkyTexture() : nullptr;
	SkyData4.w = skyVis ? 1.0f : 0.0f;
	// UNOFFICIAL 2026-10-17 Skylighting: the ortho map on stage 6 (NVR_OrthoMap in the effect) for the estimate pass.
	IDirect3DTexture9* orthoMap = SkyData.x > 0.0f && TheShaderManager->Effects.ShadowsExteriors ? TheShaderManager->Effects.ShadowsExteriors->ShadowMapOrthoTexture : nullptr;
	static bool reported[2] = {};
	auto reportActive = [&]() {
		if (reported[lastLowRes]) return;
		Logger::Log("UNOFFICIAL dedicated AO active: %ux%u G16R16F ping-pong%s.", reduced.Width, reduced.Height,
			lastLowRes ? " (AOLowRes)" : "");
		reported[lastLowRes] = true;
	};
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the four passes as Vulkan passes first (ExtDedicated); false: nothing of the frame was
	// committed and the D3D9 passes below draw it.
	if (!ExtFailed && DxvkCompute::PassesOn() && ExtDedicated(Device, RenderTarget, RenderedSurface, textures, surfaces, layout, gtaoTechnique, orthoMap, skyVis)) {
		if (deferCombine) deferredReady = true;	// nothing was written to the frame; the fog pass applies ResultTexture()
		reportActive();
		renderTime = timer.LogTime(deferCombine ? "AmbientOcclusion::Deferred" : "AmbientOcclusion::PackedAO");
		return;
	}
	HRESULT result = scene ? Device->GetDepthStencilSurface(&depthSurface) : E_FAIL;
	// UNOFFICIAL ReShadeDepthCompat: leave the game's depth-stencil bound (a single smaller target is legal with it).
	if (SUCCEEDED(result) && !ReShadeIntegration::KeepDepthBound()) result = Device->SetDepthStencilSurface(nullptr);
	if (FAILED(result)) {
		if (depthSurface) depthSurface->Release();
		return;
	}
	// UNOFFICIAL 2026-11-03 RT AO in the D3D9 loop (below): only where its Vulkan passes are not broken
	const bool rtInLoop = !combineOnly && !ExtFailed && RtAmbient::Ready();
	Effect->SetTechnique(technique);
	SetCT();
	Effect->SetVector(layoutHandle, &layout);
	Device->SetTexture(6, orthoMap);
	TheRenderManager->renderState->NoteDeviceTexture(6, orthoMap);
	UINT passes = 0;
	result = Effect->Begin(&passes, 0);
	if (SUCCEEDED(result)) {
		const UINT firstPass = combineOnly && passes ? passes - 1 : 0;
		const UINT endPass = deferCombine && passes ? passes - 1 : passes;
		for (UINT p = firstPass; p < endPass && SUCCEEDED(result); ++p) {
			const bool combine = p == passes - 1;
			// UNOFFICIAL 2026-11-03 RT AO at RenderScale < 1 (docs/next/RT-RENDERSCALE.md, phase 1): the Vulkan passes (ExtDedicated) are off
			// in rect frames, so this loop runs there. The RT estimate (ExtEstimateRT: the estimate + its temporal pass into AO 0) is the one
			// Vulkan pass in it, in Estimate's place: whole-target draws read and write each texel in the rect layout the D3D9 passes keep
			// (the trace covers the rect, the temporal pass rebuilds at uv / scale). Drawn: no D3D9 Estimate.
			if (p == 0 && rtInLoop) {
				RtTemporalSetup(aoTarget);
				bool drawn;
				{
					GpuProfileScope gpu(AOPassTimer(0), Device);
					drawn = ExtConstants(extUniforms, layout, D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)) && ExtEstimateRT(Device, textures, surfaces, aoTarget);
				}
				static bool logged = false;
				if (drawn && !logged) { logged = true; Logger::Log("UNOFFICIAL RT AO: the RT estimate runs inside the D3D9 AO passes (%s).", RenderRect::Active() ? "RenderScale rect frame" : "Vulkan passes off"); }
				if (drawn) continue;
			}
			Device->SetTexture(5, nullptr);
			IDirect3DSurface9* destination = combine ? finalTarget : surfaces[p == 1 ? 1 : 0];
			result = Device->SetRenderTarget(0, destination);
			if (SUCCEEDED(result)) result = Device->SetViewport(combine ? &original : &reduced);
			if (FAILED(result)) break;
			result = Effect->BeginPass(p);
			if (FAILED(result)) break;
			IDirect3DBaseTexture9* stage5 = p == 1 ? textures[0] : p == 2 ? textures[1] : combine ? textures[0] : nullptr;
			if (stage5) Device->SetTexture(5, stage5);
			if (combine && skyVis) {	// UNOFFICIAL Skylighting v2 (upsampled with the AO's weights)
				Device->SetTexture(7, skyVis);
				TheRenderManager->renderState->NoteDeviceTexture(7, skyVis);
			}
			Device->SetTexture(2, scene); // TESR_SourceBuffer slot
			// UNOFFICIAL: keep the game's texture cache truthful (NiDX9RenderState::NoteDeviceTexture)
			TheRenderManager->renderState->NoteDeviceTexture(5, stage5);
			TheRenderManager->renderState->NoteDeviceTexture(2, scene);
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the AO targets and the frame keep to their rect
			{
				GpuProfileScope gpu(AOPassTimer(p), Device);
				result = Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			}
			HRESULT endResult = Effect->EndPass();
			if (SUCCEEDED(result)) result = endResult;
		}
		Effect->End();
	}
	Device->SetTexture(5, nullptr);
	if (orthoMap) { Device->SetTexture(6, nullptr); TheRenderManager->renderState->NoteDeviceTexture(6, nullptr); }
	if (skyVis) { Device->SetTexture(7, nullptr); TheRenderManager->renderState->NoteDeviceTexture(7, nullptr); }
	Device->SetTexture(2, TheTextureManager->SourceTexture);
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetViewport(&original);
	Device->SetDepthStencilSurface(depthSurface);
	if (depthSurface) depthSurface->Release();
	if (SUCCEEDED(result) && deferCombine) {
		// Nothing was written to the frame; the fog pass applies ResultTexture().
		deferredReady = true;
		reportActive();
		renderTime = timer.LogTime("AmbientOcclusion::Deferred");
		return;
	}
	if (SUCCEEDED(result)) {
		if (chained) chain.Commit();
		else result = Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	}
	if (FAILED(result)) {
		// RenderedSurface is only written by the final copy, so it still holds the input.
		Device->StretchRect(RenderedSurface, NULL, RenderTarget, NULL, D3DTEXF_NONE);
		packedAOFailed = true;
		Logger::Log("Packed AO failed (%08lx); using legacy AO until restart.", result);
		if (deferCombine) return;
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer);
		return;
	}
	reportActive();
	renderTime = timer.LogTime("AmbientOcclusion::PackedAO");
}

// UNOFFICIAL 2026-11-01 (core/Diagnostics.cpp, the REPORT block: a tester saw "nothing happen" when switching RT AO on): what the RT AO did
std::string AmbientOcclusionEffect::RtStatusText() {
	const RtAmbient::Settings& s = RtAmbient::Current;
	if (!s.Enabled) return "RT AO off ([Shaders.AmbientOcclusion.RT] Enabled false): Vegas Shaders' screen-space AO";
	const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
	const char* state = !RtAmbient::PassesWanted() ? "on but the Vegas Shaders DXVK passes are off: Vegas Shaders' AO runs" :
		!RtAmbient::CoreReady() ? "on but the RT core is not running (no helper, or still starting): Vegas Shaders' AO runs" : "running";
	char t[400];
	sprintf_s(t, "RT AO %s | traced this frame %s (last traced frame f%u%s) | %d AO rays of %.0f units, sky %s, GI %s (%d reuse), Denoise %s, Interleave %d, DebugView %d (1 RT visibility, 2 RT sky, 3 hit distance)",
		state, RtTracedFrame == frame ? "yes" : "no", RtTracedFrame == 0xFFFFFFFF ? 0u : RtTracedFrame, RtTracedFrame == 0xFFFFFFFF ? ", never" : "",
		s.RayCount, s.Distance, s.Sky ? "on" : "off", s.GI ? (RtGITraced ? "on, traced" : "on") : "off", s.GIReuse, s.Denoise ? "on" : "off", s.Interleave, s.DebugView);
	return t;
}
