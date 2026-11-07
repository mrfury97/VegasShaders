#include "Reflections.h"
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-01 SharedNoise
#include "../../core/GpuProfiler.h"
#include "../../core/ReflMaterial.h"
#include "../../core/FlashlightLamp.h"	// UNOFFICIAL 2026-11-04
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/HiZ.h"	// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz)
#include "../../core/ShaderCounters.h"	// UNOFFICIAL 2026-10-26 debugging tools (the Hi-Z trace's counters)
#include "../../core/ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion (Temporal's motion input)
#include "../../core/PixelInspector.h"
#include "../../core/RtCoreClient.h"	// UNOFFICIAL 2026-10-28 (RT reflections)
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-10-31 MaterialBufferRoughness (the D3D9 effect's material copy)
#include "../../core/RtCore.h"
#include "../../core/GlassMask.h"	// UNOFFICIAL 2026-10-29 glass reflections
#include "../../core/Upscaler.h"	// UNOFFICIAL 2026-10-30 DLSS RR (RayReconstructionWantsRaw: the RT resolve unblurred, no history)

void ReflectionsEffect::UpdateSettings() {
	// UNOFFICIAL 2026-10-18: the frames drawn with the settings that are about to be replaced. 2026-10-19: only while the F10
	// profile runs (as the 240-frame report): every settings change reruns this, so dragging a menu slider wrote dozens of lines.
	if (GpuTimer::Enabled) ReportHistory("settings changed");
	const char* section = "Shaders.Reflections.Main";
	Settings.GroundSheen = std::clamp(TheSettingManager->GetSettingF(section, "GroundSheen"), 0.0f, 2.0f);
	Settings.RainWetness = std::clamp(TheSettingManager->GetSettingF(section, "RainWetness"), 0.0f, 2.0f);
	Settings.InteriorFloors = std::clamp(TheSettingManager->GetSettingF(section, "InteriorFloors"), 0.0f, 2.0f);
	Settings.Roughness = std::clamp(TheSettingManager->GetSettingF(section, "Roughness"), 0.02f, 1.0f);
	Settings.Mirror = std::clamp(TheSettingManager->GetSettingF(section, "Mirror"), 0.0f, 1.0f);	// UNOFFICIAL 2026-11-05
	Settings.MirrorSmoothOnly = TheSettingManager->GetSettingI(section, "MirrorSmoothOnly") != 0;	// UNOFFICIAL 2026-11-06	// UNOFFICIAL 2026-11-05
	Settings.WetRoughness = std::clamp(TheSettingManager->GetSettingF(section, "WetRoughness"), 0.02f, 1.0f);
	Settings.MaxDistance = std::clamp(TheSettingManager->GetSettingF(section, "MaxDistance"), 200.0f, 20000.0f);
	Settings.Steps = std::clamp(TheSettingManager->GetSettingI(section, "Steps"), 8, 64);
	Settings.Thickness = std::clamp(TheSettingManager->GetSettingF(section, "Thickness"), 0.1f, 10.0f);
	Settings.Temporal = std::clamp(TheSettingManager->GetSettingF(section, "Temporal"), 0.0f, 0.95f);
	Settings.BlurRadius = std::clamp(TheSettingManager->GetSettingF(section, "BlurRadius"), 0.0f, 8.0f);
	Settings.DrawDistance = std::clamp(TheSettingManager->GetSettingF(section, "DrawDistance"), 500.0f, 50000.0f);
	Settings.SkyFallback = TheSettingManager->GetSettingI(section, "SkyFallback") != 0;
	Settings.CubeMap = TheSettingManager->GetSettingI(section, "CubeMap") != 0;
	Settings.CubeCapture = std::clamp(TheSettingManager->GetSettingF(section, "CubeCapture"), 0.02f, 1.0f);
	Settings.CubeFade = std::clamp(TheSettingManager->GetSettingF(section, "CubeFade"), 0.0f, 0.2f);
	Settings.DebugView = std::clamp(TheSettingManager->GetSettingI(section, "DebugView"), 0, 10);	// 9, 10: UNOFFICIAL 2026-10-26 the Hi-Z walk's end rule / skips (HiZ)	// 8: SpecularOcclusion (diagnostic)	// 4: UNOFFICIAL 2026-10-19 the rain's wet mask; 5, 6 the light grid; 7 the material buffer
	Settings.GridFallback = TheSettingManager->GetSettingI(section, "GridFallback") != 0;
	Settings.GridBrightness = std::clamp(TheSettingManager->GetSettingF(section, "GridBrightness"), 0.0f, 4.0f);
	// UNOFFICIAL 2026-10-20 MaterialAware (core/ReflMaterial.h): needs the shaders compiled with it (on at startup, with the _rmaos maps).
	Settings.MaterialAware = ReflMaterial::CompiledIn() ? std::clamp(TheSettingManager->GetSettingI(section, "MaterialAware"), 0, 2) : 0;
	Settings.MaterialReflect = std::clamp(TheSettingManager->GetSettingF(section, "MaterialReflect"), 0.0f, 2.0f);
	Settings.MaterialMaxRoughness = std::clamp(TheSettingManager->GetSettingF(section, "MaterialMaxRoughness"), 0.05f, 1.0f);
	// UNOFFICIAL 2026-10-21 ReflectAdd: live, but only compiled into the effect when above 0 at startup (ReflMaterial::AddCompiledIn).
	Settings.ReflectAdd = ReflMaterial::AddCompiledIn() ? std::clamp(TheSettingManager->GetSettingF(section, "ReflectAdd"), 0.0f, 1.0f) : 0.0f;
	// UNOFFICIAL 2026-10-21 SpecularOcclusion: live, compiled into the effect only when above 0 at startup (ReflMaterial::OccCompiledIn).
	Settings.SpecularOcclusion = ReflMaterial::OccCompiledIn() ? std::clamp(TheSettingManager->GetSettingF(section, "SpecularOcclusion"), 0.0f, 1.0f) : 0.0f;
	Settings.SpecularOcclusionLevel = std::clamp(TheSettingManager->GetSettingF(section, "SpecularOcclusionLevel"), 0.05f, 4.0f);
	Settings.HiZ = TheSettingManager->GetSettingI(section, "HiZ") != 0;	// UNOFFICIAL 2026-10-26 (NVR DXVK, core/HiZ.h)
	Settings.GlassReflect = std::clamp(TheSettingManager->GetSettingF(section, "GlassReflect"), 0.0f, 2.0f);	// UNOFFICIAL 2026-10-29 (core/GlassMask.h)
	Settings.MaterialBufferRoughness = TheSettingManager->GetSettingI(section, "MaterialBufferRoughness") != 0;	// UNOFFICIAL 2026-10-30
	Settings.CloudSky = TheSettingManager->GetSettingI(section, "CloudSky") != 0;	// UNOFFICIAL 2026-10-30
	// UNOFFICIAL 2026-10-25 Reflectivity: the PBR section's one reflectivity control reaches the floor reflections too (a missing key: 1).
	SettingManager::Configuration::ConfigNode node;
	Settings.ReflectivityExterior = TheSettingManager->Config.FillNode(&node, "Shaders.PBR.Main", "ReflectivityExterior") ? std::clamp(node.FloatValue, 0.0f, 3.0f) : 1.0f;
	Settings.ReflectivityInterior = TheSettingManager->Config.FillNode(&node, "Shaders.PBR.Main", "ReflectivityInterior") ? std::clamp(node.FloatValue, 0.0f, 3.0f) : 1.0f;
	Settings.HiZInteriorsTest = TheSettingManager->GetSettingI(section, "HiZInteriorsTest") != 0;	// test only: HiZ indoors too
	Settings.HiZSafeCells = TheSettingManager->GetSettingI(section, "HiZSafeCells") != 0;	// test only: the pyramid-channel fix
	HiZ::CheckEvery = std::clamp(TheSettingManager->GetSettingI(section, "HiZCheck"), 0, 100000);	// test only: pyramid readback check
	// UNOFFICIAL 2026-10-28 RT reflections (docs/next/FORK-RT-REFLECTIONS.md); a missing key reads 0 / false (off).
	const char* rt = "Shaders.Reflections.RT";
	Settings.RtEnabled = TheSettingManager->GetSettingI(rt, "Enabled") != 0;
	Settings.RtMaxRoughness = std::clamp(TheSettingManager->GetSettingF(rt, "MaxRoughness"), 0.02f, 1.0f);
	Settings.RtDistance = std::clamp(TheSettingManager->GetSettingF(rt, "Distance"), 200.0f, 30000.0f);
	Settings.RtDebugView = std::clamp(TheSettingManager->GetSettingI(rt, "DebugView"), 0, 7);	// 7 (2026-11-05) the reflected direction
	Settings.RtScreenTolerance = std::clamp(TheSettingManager->GetSettingF(rt, "ScreenTolerance"), 0.002f, 0.2f);
	Settings.RtEstimate = std::clamp(TheSettingManager->GetSettingF(rt, "EstimateBrightness"), 0.0f, 4.0f);
	Settings.RtSunShadows = TheSettingManager->GetSettingI(rt, "SunShadows") != 0;
	Settings.RtAlphaSolid = TheSettingManager->GetSettingI(rt, "AlphaSolid") != 0;	// UNOFFICIAL 2026-11-06 diagnostic (missing key: off)
	Settings.RtSplitTest = TheSettingManager->GetSettingI(rt, "SplitTest") != 0;	// test only
	Settings.RtVndf = TheSettingManager->GetSettingI(rt, "VisibleNormals") != 0;	// UNOFFICIAL 2026-10-29 glossy rays
	Settings.RtHitLamps = TheSettingManager->GetSettingI(rt, "HitLamps") != 0;
	Settings.RtRoughReach = std::clamp(TheSettingManager->GetSettingF(rt, "RoughReach"), 0.0f, 0.99f);
	Settings.RtRoughShare = std::clamp(TheSettingManager->GetSettingF(rt, "RoughShare"), 0.0f, 1.0f);	// 0 off
	Settings.RtContactSharpness = std::clamp(TheSettingManager->GetSettingF(rt, "ContactSharpness"), 0.05f, 1.0f);
	Settings.RtContactSpread = std::clamp(TheSettingManager->GetSettingF(rt, "ContactSpread"), 0.01f, 4.0f);
	Settings.RtGlowBrightness = std::clamp(TheSettingManager->GetSettingF(rt, "GlowBrightness"), 0.0f, 16.0f);
	Settings.RtHitTextures = TheSettingManager->GetSettingI(rt, "HitTextures") != 0;	// UNOFFICIAL 2026-10-29
	Settings.RtHitAmbientGrid = TheSettingManager->GetSettingI(rt, "HitAmbientGrid") != 0;	// UNOFFICIAL 2026-10-30
	Settings.RtRayReuse = TheSettingManager->GetSettingI(rt, "RayReuse") != 0;	// UNOFFICIAL 2026-10-31
	Settings.RtReuseRoughness = std::clamp(TheSettingManager->GetSettingF(rt, "ReuseRoughness"), 0.0f, 1.0f);
	Settings.RtShadeOffscreen = TheSettingManager->GetSettingI(rt, "ShadeOffscreen") != 0;	// UNOFFICIAL 2026-11-05
	Settings.RtWeapon = std::clamp(TheSettingManager->GetSettingF(rt, "Weapon"), 0.0f, 2.0f);	// UNOFFICIAL 2026-10-30
	Settings.RtWater = TheSettingManager->GetSettingI(rt, "Water") != 0;	// UNOFFICIAL 2026-10-31
	Settings.RtSkyPanorama = TheSettingManager->GetSettingI(rt, "SkyPanorama") != 0;	// UNOFFICIAL 2026-10-31
	Settings.RtWaterSkipPlanar = TheSettingManager->GetSettingI(rt, "WaterSkipPlanar") != 0;
	Settings.RtWaterRoughness = std::clamp(TheSettingManager->GetSettingF(rt, "WaterRoughness"), 0.0f, 0.5f);
	Settings.RtWeaponRoughness = std::clamp(TheSettingManager->GetSettingF(rt, "WeaponRoughness"), 0.02f, 1.0f);
	Settings.RtWeaponMetal = std::clamp(TheSettingManager->GetSettingF(rt, "WeaponMetal"), 0.0f, 1.0f);
}

// --- UNOFFICIAL 2026-10-28 RT reflections (docs/next/FORK-RT-REFLECTIONS.md; Compute\ReflectionsRT.glsl has the whole frame) -----------
// The rays run in the RT core's helper (core/RtCore.h): this effect is one of its users. Its shared images (half resolution): the
// direction and the ray (both RG32F: RayGen's targets 1 and 2, copied in) and the hits (RGBA16F, copied out into Rt.Hit).
namespace {
	ReflectionsEffect* RtEffect = nullptr;
	int RtImageDir = -1, RtImageRay = -1, RtImageHit = -1, RtImageShade = -1, RtShader = -1;
	int RtShaderAlpha = -1;	// UNOFFICIAL 2026-10-29: Compute\Rt\ReflectionsRTAlpha.spv (with the core's alpha-tested casters)
	int RtShaderTex = -1, RtShaderAlphaTex = -1;	// UNOFFICIAL 2026-10-29: the same with hit textures (ReflectionsRTTex / ReflectionsRTAlphaTex)
	int RtShaderTexGlow = -1, RtShaderAlphaTexGlow = -1;	// UNOFFICIAL 2026-10-30: and the glow maps (ReflectionsRTTexGlow / ReflectionsRTAlphaTexGlow)
	struct RtPassConstants { D3DXVECTOR4 size, forward, right, up, origin, sun, layout, lamps[24]; };	// Rt/ReflectionsRT.comp's Pass block (std140, 496 bytes)
	static_assert(sizeof(RtPassConstants) <= 512, "the RT core's pass constants limit");

	// UNOFFICIAL 2026-10-29: the 12 scene lamps nearest the camera that reach within Distance of it ([2i] position relative to the
	// camera = scene space, reach; [2i + 1] colour x dimmer as the game gives it), for the hits' shading (helper + Resolve).
	const int RtMaxLamps = 12;
	D3DXVECTOR4 RtLampData[RtMaxLamps * 2] = {};
	int RtLampCount = 0;
	void RtGatherLamps(float distance) {
		RtLampCount = 0;
		if (!SceneNode) return;
		struct Candidate { float key; NiPointLight* light; };
		Candidate best[RtMaxLamps];
		int n = 0;
		const D3DXVECTOR4 eye = TheRenderManager->CameraPosition;
		for (auto* e = SceneNode->lights.start; e; e = e->next) {
			NiPointLight* light = e->data ? e->data->sourceLight : nullptr;
			if (!light || light->Spec.r <= 0.0f || (light->Diff.r + light->Diff.g + light->Diff.b) * light->Dimmer <= 5.0f / 255.0f) continue;
			if (FlashlightLamp::Is(light)) continue;	// UNOFFICIAL 2026-11-04: the flashlight's game lamp (core/FlashlightLamp.h)
			const NiPoint3& p = light->m_worldTransform.pos;
			const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
			const float key = sqrtf(dx * dx + dy * dy + dz * dz) - light->Spec.r;	// how far its light starts from the camera
			if (key > distance) continue;
			int at = n < RtMaxLamps ? n++ : RtMaxLamps;
			if (at == RtMaxLamps) { if (key >= best[RtMaxLamps - 1].key) continue; at = RtMaxLamps - 1; }
			while (at > 0 && best[at - 1].key > key) { best[at] = best[at - 1]; at--; }
			best[at] = { key, light };
		}
		for (int i = 0; i < n; i++) {
			NiPointLight* light = best[i].light;
			const NiPoint3& p = light->m_worldTransform.pos;
			RtLampData[2 * i] = D3DXVECTOR4(p.x - eye.x, p.y - eye.y, p.z - eye.z, light->Spec.r);
			RtLampData[2 * i + 1] = D3DXVECTOR4(light->Diff.r * light->Dimmer, light->Diff.g * light->Dimmer, light->Diff.b * light->Dimmer, 0.0f);
		}
		RtLampCount = n;
	}
	RtPassConstants RtPending = {};
	bool RtRecorded = false;

	UINT RtWidth() { return MaxQuality::Reduced(TheRenderManager->width, 2); }
	UINT RtHeight() { return MaxQuality::Reduced(TheRenderManager->height, 2); }
	// UNOFFICIAL 2026-11-03 RT reflections at RenderScale < 1 (docs/next/RT-RENDERSCALE.md "RT reflections plan"): the grid the helper
	// traces this frame: the whole half-size images in a native frame, their top-left RenderScale rect in a rect frame (RtTrace sets it).
	UINT RtTraceWidth = 0, RtTraceHeight = 0;
	// The RT passes' rule instead of DxvkCompute::PassesOn: the NVR DXVK passes on and there. PassesOn is false inside a rect frame by
	// design (the other Vulkan passes draw whole targets); RT reflections' own passes (RayGen, Resolve, the sky panorama) keep to the rect,
	// and Temporal, the blurs and the composite run as their rect-aware D3D9 techniques there. In a native frame the same as PassesOn.
	bool RtPassesWanted() { return TheSettingManager->SettingsMain.Main.NvrDxvkPasses && DxvkCompute::Available(); }
	bool RtUserWants() { return RtEffect && RtEffect->Enabled && RtEffect->Settings.RtEnabled && RtPassesWanted(); }
	void RtUserSetup() {
		RtImageDir = RtCore::AddImage("NVR RT reflections direction", RtWidth(), RtHeight(), NVR_EXT_SHARED_RG32F);
		RtImageRay = RtCore::AddImage("NVR RT reflections ray", RtWidth(), RtHeight(), NVR_EXT_SHARED_RG32F);
		RtImageHit = RtCore::AddImage("NVR RT reflections hit", RtWidth(), RtHeight(), NVR_EXT_SHARED_RGBA16F);
		RtImageShade = RtCore::AddImage("NVR RT reflections shade", RtWidth(), RtHeight(), NVR_EXT_SHARED_RGBA16F);
	}
	// Recorded at the late exchange (RtCoreClient::Exchange from RtTrace), after RayGen drew Rt.Dir and Rt.Ray.
	void RtUserRecord(IDirect3DDevice9* Device) {
		RtRecorded = false;
		if (!RtEffect || RtImageDir < 0 || RtImageRay < 0 || RtImageHit < 0 || RtImageShade < 0 || RtShader < 0) return;
		const ReflectionsEffect::RtImagesStruct& rt = RtEffect->Rt;
		if (!rt.Dir || !rt.Ray || !rt.HitSurface || !rt.ShadeSurface) return;
		if (!RtCore::CopyIn(RtImageDir, rt.Dir) || !RtCore::CopyIn(RtImageRay, rt.Ray)) return;
		// With the core's alpha-tested casters in the scene, the pass that tests them (their alpha atlas as a fifth image).
		const int atlas = RtCore::AlphaTested() ? RtCore::AlphaAtlasImage() : -1;
		const bool alpha = atlas > 0 && RtShaderAlpha >= 0;
		// With the core's hit textures ([Main.Main.RayTracing] HitTextures) and [Shaders.Reflections.RT] HitTextures: the pass that
		// shades hits with the texture's colour (the albedo atlas as the next image).
		const int albedo = RtEffect->Settings.RtHitTextures ? RtCore::AlbedoAtlasImage() : -1;
		const bool tex = albedo >= 0 && (alpha ? RtShaderAlphaTex : RtShaderTex) >= 0;
		// With the core's glow maps too ([Main.Main.RayTracing] GlowTextures): glowing draws glow only where their glow map is lit.
		const int glowAtlas = tex ? RtCore::GlowAtlasImage() : -1;
		const bool glow = glowAtlas >= 0 && (alpha ? RtShaderAlphaTexGlow : RtShaderTexGlow) >= 0;
		int images[7] = { RtImageDir, RtImageRay, RtImageHit, RtImageShade, atlas, albedo, glowAtlas };
		int count = 4;
		if (alpha) images[count++] = atlas;
		if (tex) images[count++] = albedo;
		if (glow) images[count++] = glowAtlas;
		const int shader = alpha ? (glow ? RtShaderAlphaTexGlow : tex ? RtShaderAlphaTex : RtShaderAlpha) : (glow ? RtShaderTexGlow : tex ? RtShaderTex : RtShader);
		const UINT gridW = RtTraceWidth ? RtTraceWidth : RtWidth(), gridH = RtTraceHeight ? RtTraceHeight : RtHeight();	// (2026-11-03: the rect in rect frames)
		if (!RtCore::AddPass(shader, images, count, 0xC, &RtPending, sizeof(RtPending), (gridW + 7) / 8, (gridH + 7) / 8)) return;
		RtRecorded = RtCore::CopyOut(RtImageHit, rt.HitSurface) && RtCore::CopyOut(RtImageShade, rt.ShadeSurface);
	}
	const RtCore::User RtUser = { "RT reflections", RtUserWants, RtUserSetup, RtUserRecord, 1 };	// Point 1: only through RtCore::Exchange (RtTrace)
}

// UNOFFICIAL 2026-10-30 RT weapon reflections: tan(world FOV / 2) / tan(1st-person FOV / 2): the game draws the first-person weapon from
// the same eye with its own (narrower) FOV, so a weapon pixel's sideways view offset is the world projection's divided by this. 1 if
// either is unknown.
float ReflectionsEffect::WeaponFovScale() const {
	if (!Player) return 1.0f;
	const float world = Player->worldFoV, first = Player->firstPersonFoV;
	if (world <= 1.0f || world >= 179.0f || first <= 1.0f || first >= 179.0f) return 1.0f;
	const float toRad = 3.14159265f / 360.0f;
	return tanf(world * toRad) / tanf(first * toRad);
}

// Once, before the RT core's first frame (ShaderManager, with the other RT users).
void ReflectionsEffect::RtRegister() {
	RtEffect = this;
	RtShader = RtCore::AddShader("ReflectionsRT");
	RtShaderAlpha = RtCore::AddShader("ReflectionsRTAlpha");	// UNOFFICIAL 2026-10-29
	RtShaderTex = RtCore::AddShader("ReflectionsRTTex");	// UNOFFICIAL 2026-10-29 hit textures
	RtShaderAlphaTex = RtCore::AddShader("ReflectionsRTAlphaTex");
	RtShaderTexGlow = RtCore::AddShader("ReflectionsRTTexGlow");	// UNOFFICIAL 2026-10-30 glow maps
	RtShaderAlphaTexGlow = RtCore::AddShader("ReflectionsRTAlphaTexGlow");
	RtCore::AddUser(RtUser);
}

// Only on the Vulkan trace (ExtRender) with the RT core's helper up. Everything else (the setting off, D3D9 passes, a GPU without
// RT, a pipeline refused) leaves the trace as it was. SplitTest runs the two in-game passes without rays and without the helper.
bool ReflectionsEffect::RtWanted() const {
	return Settings.RtEnabled && !ExtFailed && !rtTraceFailed && RtPassesWanted() && (RtCoreClient::Ready() || Settings.RtSplitTest);	// (2026-11-03: rect frames too)
}

float ReflectionsEffect::RtSunAmount() const {
	const ShaderConstants& sc = TheShaderManager->ShaderConst;
	return TheShaderManager->GameState.isExterior && sc.SunDir.z > 0.0f ? std::clamp(sc.SunAmount.x, 0.0f, 1.0f) : 0.0f;
}

// The four half-resolution RT images, made the first time RT runs (no memory spent when it never does). Any failure: RT off for the session.
bool ReflectionsEffect::RtImages() {
	if (Rt.Spread) return true;
	IDirect3DDevice9* device = TheRenderManager->device;
	struct { IDirect3DTexture9** texture; IDirect3DSurface9** surface; D3DFORMAT format; } images[6] = {
		{ &Rt.March, &Rt.MarchSurface, D3DFMT_A16B16G16R16F }, { &Rt.Dir, &Rt.DirSurface, D3DFMT_G32R32F },
		{ &Rt.Ray, &Rt.RaySurface, D3DFMT_G32R32F }, { &Rt.Hit, &Rt.HitSurface, D3DFMT_A16B16G16R16F },
		{ &Rt.Shade, &Rt.ShadeSurface, D3DFMT_A16B16G16R16F }, { &Rt.Spread, &Rt.SpreadSurface, D3DFMT_R16F } };
	for (auto& i : images)
		if (FAILED(device->CreateTexture(RtWidth(), RtHeight(), 1, D3DUSAGE_RENDERTARGET, i.format, D3DPOOL_DEFAULT, i.texture, NULL)) ||
			FAILED((*i.texture)->GetSurfaceLevel(0, i.surface))) {
			Logger::Log("UNOFFICIAL Reflections: the RT images could not be created; RT reflections are off.");
			rtTraceFailed = true;
			for (auto& j : images) {
				if (*j.surface) { (*j.surface)->Release(); *j.surface = nullptr; }
				if (*j.texture) { (*j.texture)->Release(); *j.texture = nullptr; }
			}
			return false;
		}
	return true;
}

// RayGen (the march + the rays) into the RT images, the helper traces them at the RT core's late exchange, Resolve shades into the
// trace's target. False before anything is drawn into target: the caller draws the normal trace. (A helper that has nothing this
// frame costs the RayGen pass, then the normal trace.)
bool ReflectionsEffect::RtTrace(IDirect3DDevice9* Device, IDirect3DSurface9* target) {
	rtResolved = false;
	weaponFrame = false;
	waterFrame = false;
	if (!RtWanted() || !RtImages()) return false;
	// SplitTest (no helper): RayGen casts no ray, so Resolve must give the normal trace's picture exactly (the split checked on any GPU).
	rtRays = RtCoreClient::Ready();
	weaponFrame = rtRays && Settings.RtWeapon > 0.0f;	// UNOFFICIAL 2026-10-30 (dropped again below if this frame falls back to the normal trace)
	// UNOFFICIAL 2026-11-03 RenderScale < 1: no weapon rays in a rect frame: the composite there is the D3D9 one, which never shades the
	// weapon (its reflection would only be traced and blurred into its surroundings).
	if (RenderRect::Active()) weaponFrame = false;
	waterFrame = rtRays && Settings.RtWater && !TheShaderManager->GameState.isUnderwater;	// UNOFFICIAL 2026-10-31 (the same)
	if (rtRays && Settings.RtHitLamps) RtGatherLamps(Settings.RtDistance); else RtLampCount = 0;	// UNOFFICIAL 2026-10-29 (before RayGen: both passes read them)
	if (!ExtRender(Device, Rt.MarchSurface, Rt.MarchSurface, 8, false, Rt.MarchSurface)) return false;
	bool traced = !rtRays;	// SplitTest: nothing to trace
	if (rtRays) {
		// The pass constants: the camera as RtSunShadows gives it (scene space = world minus CameraPosition, eye RtCore::Eye()), the sun, RayGen's pixel rule.
		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.SunDir;
		// (2026-10-31: with the core's FarObjectLOD the rays reach the far ring's edge: the skyline past SceneRadius)
		const float reach = TheSettingManager->GetSettingI("Main.Main.RayTracing", "FarObjectLOD") != 0 ||
			TheSettingManager->GetSettingI("Main.Main.RayTracing", "FarLandLOD") != 0 ?
			(std::max)(Settings.RtDistance, TheSettingManager->GetSettingF("Main.Main.RayTracing", "FarRadius")) : Settings.RtDistance;
		// UNOFFICIAL 2026-11-03 RenderScale < 1: in a rect frame the helper traces the top-left rect of the half-size images (RenderRect::
		// Width / Height of them, the part the D3D9 passes' ApplyViewport draws); RayGen wrote "no ray" past it.
		RtTraceWidth = RenderRect::Width(RtWidth());
		RtTraceHeight = RenderRect::Height(RtHeight());
		RtPending.size = D3DXVECTOR4((float)RtTraceWidth, (float)RtTraceHeight, reach, (Settings.RtSunShadows ? 1.0f : 0.0f) + (Settings.RtAlphaSolid ? 2.0f : 0.0f));	// w: + 2 AlphaSolid
		RtPending.forward = D3DXVECTOR4(view._13, view._23, view._33, 0.0f);
		RtPending.right = D3DXVECTOR4(view._11 / proj._11, view._21 / proj._11, view._31 / proj._11, 1.0f / WeaponFovScale());	// w: the weapon's
		RtPending.up = D3DXVECTOR4(view._12 / proj._22, view._22 / proj._22, view._32 / proj._22, 0.0f);
		const D3DXVECTOR3 eye = RtCore::Eye();	// the eye in scene space (2026-10-29 rt-sun-core 4ba2abfd: CameraLocation reads ~0 in game)
		RtPending.origin = D3DXVECTOR4(eye.x, eye.y, eye.z, Settings.RtDebugView == 5 ? 1.0f : 0.0f);	// w: the alignment check
		D3DXVECTOR3 toSun(sun.x, sun.y, sun.z);
		D3DXVec3Normalize(&toSun, &toSun);
		RtPending.sun = D3DXVECTOR4(toSun.x, toSun.y, toSun.z, RtSunAmount() > 0.0f ? 1.0f : 0.0f);
		RtPending.layout = D3DXVECTOR4(Constants.Layout.x, Constants.Layout.y, 1.0f / TheRenderManager->width, 1.0f / TheRenderManager->height);
		if (RenderRect::Active()) {
			// UNOFFICIAL 2026-11-03 RenderScale < 1: the helper rebuilds each surface at uv = (pixel + 0.5) x layout.xy - 0.5 x (layout.xy -
			// layout.zw), RayGen's pixel centre as a texture coordinate; divided by the rect's scale, that is the screen point it shows
			// (rectToScreen), what its camera rays want. Plus the depth's jitter (TAAEffect::DepthJitterUV, screen uv: a depth texel lies on
			// the ray through uv + jitter; 1 / scale bigger on screen than at RenderScale 1). The constants are full (496 of 512 bytes, up.w
			// alone free), so it rides in forward as a shear: the helper's point is forward + right (2u - 1) - up (2v - 1), and u + jx, v + jy
			// add 2 jx right - 2 jy up. Rect frames only (RenderScale 1 untouched, as RT AO's).
			RtPending.layout.x /= RenderRect::ScaleX;
			RtPending.layout.z /= RenderRect::ScaleX;
			RtPending.layout.y /= RenderRect::ScaleY;
			RtPending.layout.w /= RenderRect::ScaleY;
			if (TAAEffect* taa = TheShaderManager->Effects.TAA) {
				const D3DXVECTOR2 j = taa->DepthJitterUV();
				RtPending.forward.x += 2.0f * j.x * RtPending.right.x - 2.0f * j.y * RtPending.up.x;
				RtPending.forward.y += 2.0f * j.x * RtPending.right.y - 2.0f * j.y * RtPending.up.y;
				RtPending.forward.z += 2.0f * j.x * RtPending.right.z - 2.0f * j.y * RtPending.up.z;
			}
			static bool logged = false;
			if (!logged) {
				logged = true;
				Logger::Log("UNOFFICIAL Reflections: RT traced in the RenderScale rect: %ux%u of %ux%u (RayGen, the helper, Resolve and the sky panorama as Vulkan passes, the rest D3D9).",
					RtTraceWidth, RtTraceHeight, RtWidth(), RtHeight());
			}
		}
		// forward.w: the lamp count (integer part; RtGatherLamps ran before RayGen, ExtRender 8 uploads the same lamps for Resolve) +
		// RoughReach (fraction part, below 1: a rough ray's reach share at roughness 1).
		RtPending.forward.w = (float)RtLampCount + std::clamp(Settings.RtRoughReach, 0.0f, 0.99f);
		memcpy(RtPending.lamps, RtLampData, sizeof(RtLampData));
		static GpuTimer exchangeTimer("  Reflections RT (exchange)");
		GpuProfileScope gpu(exchangeTimer, Device);
		traced = RtCoreClient::Exchange(Device, RtUser) && RtRecorded;
	}
	if (!traced) {
		weaponFrame = false;
		waterFrame = false;
		if (++rtFallbackFrames == 1) Logger::Log("UNOFFICIAL Reflections: RT is on but the RT helper traced nothing this frame; the normal trace is drawn (logged once).");
		return false;
	}
	Device->SetRenderTarget(0, target);
	if (!ExtRender(Device, target, target, 9, false, target)) { weaponFrame = false; waterFrame = false; return false; }
	rtResolved = true;	// UNOFFICIAL 2026-10-29: the blur passes read Rt.Spread this frame
	if (++rtTraceFrames == 1) Logger::Log("UNOFFICIAL Reflections: the trace runs with RT ([Shaders.Reflections.RT]%s).", rtRays ? "" : ", SplitTest: no rays");
	return true;
}

// UNOFFICIAL 2026-10-26 (deep/fork-hiz): outdoors only for now. Indoors (Lucky 38, a 3rd-person view looking down) the walk found no
// hits on the near floor where the fixed-step trace finds the rail and the counter (bulk runs 2026-10-26); the old trace stays there.
bool ReflectionsEffect::HiZWanted() const {
	return Settings.HiZ && !ExtFailed && !hizTraceFailed && (TheShaderManager->GameState.isExterior || Settings.HiZInteriorsTest);
}

void ReflectionsEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_ReflectionsData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_ReflectionsData2", &Constants.Data2);
	TheShaderManager->RegisterConstant("TESR_ReflectionsData3", &Constants.Data3);
	TheShaderManager->RegisterConstant("TESR_ReflectionsData4", &Constants.Data4);
	TheShaderManager->RegisterConstant("TESR_ReflectionsLayout", &Constants.Layout);
	TheShaderManager->RegisterConstant("TESR_ReflectionsReproject", (D3DXVECTOR4*)&Constants.Reproject);
	TheShaderManager->RegisterConstant("TESR_ReflectionsCube", &Constants.Cube);
	TheShaderManager->RegisterConstant("TESR_ReflectionsCube2", &Constants.Cube2);
	TheShaderManager->RegisterConstant("TESR_ReflectionsGrid", &Constants.Grid);
	TheShaderManager->RegisterConstant("TESR_ReflectionsGrid2", &Constants.Grid2);
	TheShaderManager->RegisterConstant("TESR_ReflectionsGrid3", &Constants.Grid3);
	TheShaderManager->RegisterConstant("TESR_ReflectionsMaterial", &Constants.Material);
	TheShaderManager->RegisterConstant("TESR_RTWaterData", &Constants.WaterData);	// UNOFFICIAL 2026-10-31 RT water (the water shaders)
	TheShaderManager->RegisterConstant("TESR_RTWaterReproject", (D3DXVECTOR4*)&Constants.WaterReproject);
}

void ReflectionsEffect::RegisterTextures() {
	const UINT w = MaxQuality::Reduced(TheRenderManager->width, 2), h = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("TESR_ReflectionsTrace", &Textures.TraceTexture, &Textures.TraceSurface, w, h, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_ReflectionsBuffer", &Textures.BufferTexture, &Textures.BufferSurface, w, h, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_ReflectionsTemp", &Textures.TempTexture, &Textures.TempSurface, w, h, D3DFMT_A16B16G16R16F);
	TheTextureManager->InitTexture("TESR_ReflectionsHistory", &Textures.HistoryTexture, &Textures.HistorySurface, w, h, D3DFMT_A16B16G16R16F);
	Constants.Layout = D3DXVECTOR4(1.0f / w, 1.0f / h, 0.0f, 0.0f);
	// UNOFFICIAL 2026-10-20 MaterialAware: the material buffer (full resolution, 8 bits a channel: r roughness, g metal, b known), only
	// when the shaders were compiled with it.
	if (ReflMaterial::CompiledIn() &&
		(FAILED(TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
			D3DPOOL_DEFAULT, &Textures.MaterialTexture, NULL)) || FAILED(Textures.MaterialTexture->GetSurfaceLevel(0, &Textures.MaterialSurface)))) {
		Logger::Log("UNOFFICIAL Reflections: the material buffer could not be created; MaterialAware is off.");
		if (Textures.MaterialTexture) { Textures.MaterialTexture->Release(); Textures.MaterialTexture = nullptr; }
		Textures.MaterialSurface = nullptr;
	}

	// The surroundings cube (FP16, about 0.6 MB in all). If any part fails, the cube stays off (CubeMap is ignored).
	IDirect3DDevice9* device = TheRenderManager->device;
	bool ok = true;
	for (int c = 0; c < 2 && ok; c++) {
		ok = SUCCEEDED(device->CreateCubeTexture(CubeSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Cubes.Capture[c], NULL));
		for (int f = 0; f < 6 && ok; f++) ok = SUCCEEDED(Cubes.Capture[c]->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &Cubes.CaptureFace[c][f]));
	}
	if (ok) ok = SUCCEEDED(device->CreateCubeTexture(CubeSize, CubeLevels, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Cubes.Env, NULL));
	for (UINT m = 0; m < CubeLevels && ok; m++)
		for (int f = 0; f < 6 && ok; f++) ok = SUCCEEDED(Cubes.Env->GetCubeMapSurface((D3DCUBEMAP_FACES)f, m, &Cubes.EnvFace[m][f]));
	if (!ok) {
		Logger::Log("UNOFFICIAL Reflections: the surroundings cube could not be created; reflections use the sky only.");
		if (Cubes.Env) { Cubes.Env->Release(); Cubes.Env = nullptr; }
	}
	TheTextureManager->RegisterTexture("TESR_ReflectionsCapture", &Cubes.CaptureLastSlot);
	TheTextureManager->RegisterTexture("TESR_ReflectionsCaptureNew", &Cubes.CaptureNewSlot);
	TheTextureManager->RegisterTexture("TESR_ReflectionsEnv", &Cubes.EnvSlot);
	TheTextureManager->RegisterTexture("TESR_ReflectionsGridLight", &GridLightSlot);	// UNOFFICIAL 2026-10-19 (GridStep)
	TheTextureManager->RegisterTexture("TESR_ReflectionsGridColor", &GridColorSlot);
}

// UNOFFICIAL 2026-10-18: one line on what the trace's history and the surroundings cube did since the last report (only when it drew).
void ReflectionsEffect::ReportHistory(const char* when) {
	if (!stats.drawn && !cubeSteps) return;
	char text[256];
	stats.Describe(text, sizeof(text));
	Logger::Log("UNOFFICIAL Reflections temporal history (%s, Temporal %.2f): %s; cube: %u updates, %u emptied first; light grid: read on %u of %u frames with GridFallback on",
		when, Settings.Temporal, text, cubeSteps, cubeEmptied, gridUsedFrames, gridFrames);
	stats.Reset();
	cubeSteps = cubeEmptied = 0;
	gridFrames = gridUsedFrames = 0;
}

// UNOFFICIAL 2026-10-19 GridFallback: AmbientLight's light grid as the far / off-screen fallback of the trace. Read only while
// AmbientLight keeps a grid up (on, GridBounce above 0) and it was finished a moment ago (it is redrawn every few frames, and
// at once when it moves); its corner goes with the finished grid. AmbientLight draws before this effect in the frame, so
// the grid read here is this frame's (or the last one it made). Nothing extra is drawn: the trace reads what is there.
void ReflectionsEffect::GridStep() {
	const AmbientLightEffect* ambient = TheShaderManager->Effects.AmbientLight;
	const UINT now = TheFrameRateManager->FrameNumber;
	// Indoors only (user 2026-10-19: outdoors it measured ~nothing, rays there go to the sky and the cube).
	bool use = Settings.GridFallback && !TheShaderManager->GameState.isExterior && ambient && ambient->Enabled && ambient->Effect && ambient->gridBounce > 0.0f &&
		ambient->gridValid && now - ambient->gridRenderedFrame <= 8 && !TheShaderManager->GameState.isCameraCut;
	IDirect3DTexture9* light = use ? ambient->gridAccum[ambient->gridAccumIndex] : nullptr;
	IDirect3DTexture9* colour = use && ambient->gridColorValid ? ambient->gridAccumColor[ambient->gridAccumIndex] : nullptr;
	use = use && light;
	if (Settings.GridFallback) {
		gridFrames++;
		if (use) gridUsedFrames++;
	}
	GridLightSlot = light;
	GridColorSlot = colour;
	Constants.Grid2 = D3DXVECTOR4(use ? 1.0f : 0.0f, Settings.GridBrightness, colour ? 1.0f : 0.0f, 0.0f);
	if (!use) return;
	const NiPoint3 camera = WorldSceneGraph->camera->m_worldTransform.pos;
	Constants.Grid = D3DXVECTOR4((float)(ambient->gridOrigin[0] - camera.x), (float)(ambient->gridOrigin[1] - camera.y),
		(float)(ambient->gridOrigin[2] - camera.z), ambient->gridCell);
	Constants.Grid3 = D3DXVECTOR4(ambient->sceneLight.x, ambient->sceneLight.y, ambient->sceneLight.z, 0.0f);
}

void ReflectionsEffect::UpdateConstants() {
	const bool exterior = TheShaderManager->GameState.isExterior;

	// Wetness: the rain falling now. UNOFFICIAL 2026-10-19 (P81 tester report: "wet ground / reflections stay after rain in
	// shadowed areas"): it used to follow Wet World's puddle level too, which dries over minutes, so ALL flat ground kept
	// mirroring long after the rain (seen most in shade, where the bright surroundings stand out against the dark ground)
	// while Wet World itself had dried everything but the puddles in hollows. The puddles keep their own wet look (Wet World).
	const float wet = std::clamp(TheShaderManager->Effects.Rain->Constants.RainAnimator.GetValue(), 0.0f, 1.0f);

	const float reflectivity = exterior ? Settings.ReflectivityExterior : Settings.ReflectivityInterior;	// UNOFFICIAL 2026-10-25
	Constants.Data.x = (exterior ? Settings.GroundSheen : Settings.InteriorFloors) * reflectivity;
	Constants.Data.y = exterior ? wet * Settings.RainWetness : 0.0f;
	// UNOFFICIAL 2026-10-19: the rain wets only open ground (the ortho map's roof test, as Wet World): kept up while it rains here.
	Constants.Data4.y = Constants.Data.y > 0.0f ? 1.0f : 0.0f;
	if (Constants.Data4.y > 0.0f) TheShaderManager->orthoRequired = true;
	Constants.Data.z = Settings.Roughness;
	Constants.Data.w = Settings.WetRoughness;
	Constants.Data2.x = Settings.MaxDistance;
	Constants.Data2.y = (float)Settings.Steps;
	Constants.Data2.z = Settings.Thickness;
	Constants.Data3.y = Settings.BlurRadius;
	Constants.Data3.z = (float)Settings.DebugView;
	// UNOFFICIAL 2026-10-28: RT DebugView 1, 2, 4 are trace colours, shown as the composite shows DebugView 6's (as they are).
	if (Settings.RtDebugView > 0 && Settings.RtDebugView != 3 && RtWanted()) Constants.Data3.z = 6.0f;
	Constants.Data3.w = exterior && Settings.SkyFallback ? 1.0f : 0.0f;
	Constants.Data4.x = Settings.DrawDistance;
	Constants.Data4.z = Settings.ReflectAdd;
	Constants.Data4.w = Settings.SpecularOcclusion;
	Constants.Layout.z = Settings.SpecularOcclusionLevel;
	// UNOFFICIAL 2026-10-26 (deep/fork-hiz): HiZSafeCells (test): the Hi-Z walk reads the coarse cells' two channels as min/max and does its
	// "behind" skip on the finest level only (ReflectionsTraceHiZ.frag). Indoors HiZ itself still needs HiZInteriorsTest.
	Constants.Layout.w = Settings.HiZSafeCells ? 1.0f : 0.0f;
	// UNOFFICIAL 2026-10-20 MaterialAware: x is set again by CaptureMaterial (1 only when the buffer was filled this frame).
	Constants.Material = D3DXVECTOR4(Settings.MaterialAware == 1 && materialFilled ? 1.0f : 0.0f, Settings.MaterialMaxRoughness, Settings.MaterialReflect * reflectivity, 0.0f);
}

// UNOFFICIAL 2026-10-20 MaterialAware (core/ReflMaterial.h): the world scene left each known material in the scene's alpha; it is
// copied out into the material buffer (technique 7) at the start of the chain, before any effect writes the scene.
void ReflectionsEffect::CaptureMaterial(IDirect3DDevice9* Device) {
	materialFilled = false;
	Constants.Material.x = 0.0f;
	if (!Enabled || !Effect || !Textures.MaterialSurface || Settings.MaterialAware <= 0 || TheShaderManager->GameState.isUnderwater) return;
	static GpuTimer timer("Reflections material copy");
	GpuProfileScope gpu(timer, Device);
	materialOnly = true;
	Device->SetRenderTarget(0, Textures.MaterialSurface);
	Render(Device, Textures.MaterialSurface, Textures.MaterialSurface, 7, false, Textures.MaterialSurface);
	// UNOFFICIAL 2026-10-31 MaterialBufferRoughness for this (D3D9) effect too: where no _rmaos map is seen, the roughness and metal the
	// game's shader lit with (core/CapturedNormals.h, b 0.5: the floor guess takes them, Reflections.fx.hlsl Reflectivity)
	if (Settings.MaterialBufferRoughness) CapturedNormals::MergeIntoReflMaterial(Device, Textures.MaterialSurface);
	materialOnly = false;
	materialFilled = true;
	Constants.Material.x = Settings.MaterialAware == 1 ? 1.0f : 0.0f;
}

void ReflectionsEffect::BindMaterial() {
	if (!materialFilled || !Textures.MaterialTexture) return;
	SetPassTexture(15, Textures.MaterialTexture);	// set after each pass begins (EffectRecord::PassTextures)
	const DWORD point[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
		{D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
	for (const auto& s : point) TheRenderManager->SetSamplerState(15, (D3DSAMPLERSTATETYPE)s[0], s[1]);
}

bool ReflectionsEffect::ShouldRender() {
	if (cubeOnly || materialOnly) return true;	// RenderCubeOnly's / CaptureMaterial's own passes
	const NormalsEffect* normals = TheShaderManager->Effects.Normals;
	const bool surfaces = Constants.Data.x > 0.0f || Constants.Data.y > 0.0f || (Constants.Material.x > 0.0f && Constants.Material.z > 0.0f);
	// UNOFFICIAL 2026-11-04 (tester: no RT gun reflection): the first-person weapon's RT reflection is traced inside this pass, so on
	// dry ground with GroundSheen 0 (nothing else on screen reflects) it was skipped with it. RT Weapon above 0 keeps the pass on while
	// the weapon rays can run (RtTrace's weaponFrame: the helper up, not a rect frame). Weapon 0 or RT off: as before.
	const bool weapon = Settings.RtWeapon > 0.0f && RtWanted() && RtCoreClient::Ready() && !RenderRect::Active();
	nothingReflects = !surfaces;
	if (!surfaces && Settings.RtEnabled) {
		static bool told = false;
		if (!told) {
			told = true;
			Logger::Log("UNOFFICIAL Reflections: RT reflections are on but nothing on screen reflects here (GroundSheen / InteriorFloors 0, "
				"dry, no material reflections)%s", weapon ? "; the pass runs for the weapon's RT reflection (RT Weapon)" : ": the pass is skipped");
		}
	}
	return !TheShaderManager->GameState.isUnderwater && normals && normals->Enabled && (surfaces || weapon) &&
		Textures.TraceSurface && Textures.BufferSurface && Textures.TempSurface && Textures.HistorySurface;
}

// The half-resolution passes (techniques: 1 Trace, 3 Temporal, 2 Blur X, 4 Blur Y), as VolumetricLight's:
// Temporal on: Trace -> Trace buffer, Temporal -> Temp (copied to History), Blur X -> Trace, Blur Y -> Buffer.
// Off: Trace -> Temp, then the same blurs. The composite (technique 0) then reads Buffer.
void ReflectionsEffect::RenderBuffers(IDirect3DDevice9* Device) {
	// UNOFFICIAL 2026-10-30 DLSS Ray Reconstruction (docs\next\FORK2-DLSS-RR.md): with the RT rays on while RR denoises the frame
	// (Upscaler::RayReconstructionWantsRaw), the RT resolve reaches the composite as it is: no Temporal pass and no history (the
	// Temporal 0 path, but the noise frame keeps turning, so RoughShare's 2 x 2 casters still rotate for RR to average), and no blurs
	// (below). A frame the helper traced nothing draws the normal trace, which still gets the blurs.
	const bool rrRaw = Upscaler::RayReconstructionWantsRaw() && RtWanted();
	const bool temporal = Settings.Temporal > 0.0f && !rrRaw;
	const D3DXMATRIX& view = TheRenderManager->viewMatrix;
	const D3DXMATRIX& invView = TheRenderManager->InvViewMatrix;

	// Last frame's result is usable if this effect drew it, and the camera did not jump (load, fast travel, a cut).
	// UNOFFICIAL 2026-10-18: counted in rendered frames (FrameNumber), not UpdateConstants calls (two a frame, which made this never pass).
	const UINT now = TheFrameRateManager->FrameNumber;
	const bool drewLastFrame = lastRenderedFrame + 1 == now;
	bool usable = temporal && haveHistory && drewLastFrame;
	// UNOFFICIAL 2026-11-01 SharedNoise: and the shared history rule (core/TemporalBackbone.h), the checks below stay
	const bool sharedNoise = TemporalBackbone::Shared();
	if (sharedNoise && !TemporalBackbone::HistoryValid()) usable = false;
	// UNOFFICIAL 2026-10-27 RenderScale v2: the history holds last frame's drawn part; another scale, or a switch between rect and
	// full frames, puts it somewhere else in the texture.
	static bool lastRect = false;
	if (RenderRect::ScaleChanged() || RenderRect::Active() != lastRect) usable = false;
	lastRect = RenderRect::Active();
	const double move[3] = { (double)invView._41 - lastInvView._41, (double)invView._42 - lastInvView._42,
		(double)invView._43 - lastInvView._43 };
	if (usable) usable = move[0] * move[0] + move[1] * move[1] + move[2] * move[2] < 500.0 * 500.0;
	stats.Note(temporal, haveHistory, drewLastFrame, usable);
	EffectStatus::History(StatusSlot, Name, EffectStatus::HistoryWhy(temporal, haveHistory, drewLastFrame, usable));	// UNOFFICIAL 2026-11-01
	if (GpuTimer::Enabled && stats.drawn >= 240) ReportHistory("240 frames");
	if (usable) {
		// Camera-relative world -> last frame's view, in double round the two camera positions (see VolumetricLight.cpp).
		D3DXMATRIX toLastView;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) toLastView.m[i][j] = lastView.m[i][j];
			toLastView.m[i][3] = 0.0f;
		}
		for (int j = 0; j < 3; j++)
			toLastView.m[3][j] = (float)(move[0] * lastView.m[0][j] + move[1] * lastView.m[1][j] + move[2] * lastView.m[2][j]);
		toLastView.m[3][3] = 1.0f;
		D3DXMatrixMultiply(&Constants.Reproject, &toLastView, &lastProj);
	}
	Constants.Data3.x = usable ? Settings.Temporal : 0.0f;
	// UNOFFICIAL 2026-11-01 SharedNoise: with Temporal the shared index (own offset); without (and under DLSS RR, a temporal resolve) the
	// trace's noise moves only while TAA / FSR / DLSS averages frames, else frame 0. Off: FrameNumber % 64 with Temporal or RR, else 0.
	Constants.Data2.w = sharedNoise ? (float)(temporal ? TemporalBackbone::EffectIndex(TemporalBackbone::OffsetReflections, 64) : TemporalBackbone::NoiseIndex() % 64) :
		temporal || rrRaw ? (float)(now % 64) : 0.0f;

	// The surroundings cube, before the trace reads it; the light grid's constants.
	CubeStep(Device);
	GridStep();
	RenderSkyPano(Device);	// UNOFFICIAL 2026-10-31 SkyPanorama (before the trace and Resolve read it)

	auto pass = [&](IDirect3DSurface9* target, UINT technique) {
		Device->SetRenderTarget(0, target);
		Render(Device, target, target, technique, false, target);
	};
	// UNOFFICIAL 2026-10-19 Skylighting v3 phase 1 ([Shaders.AmbientOcclusion.Skylighting] Reflections; after Community Shaders' Skylighting
	// MixSpecular): the sky visibility on stage 14 for the trace (11 is TESR_OrthoMapBuffer, 12-13 the GridFallback grid), whose sky fallback fades where the sky is hidden.
	AmbientOcclusionEffect* ao = TheShaderManager->Effects.AmbientOcclusion;
	IDirect3DTexture9* skyVis = ao ? ao->SkyReflectionTexture() : nullptr;
	static CachedHandle skyHandle;
	if (D3DXHANDLE handle = ParameterHandle(skyHandle, "NVR_ReflectionsSky")) {
		D3DXVECTOR4 sky(skyVis ? 1.0f : 0.0f, ao ? ao->SkyMinReflection : 0.1f, 0.0f, 0.0f);
		Effect->SetVector(handle, &sky);
		extSky = sky;	// UNOFFICIAL 2026-10-25 (NVR DXVK): the Vulkan trace reads the same
	}
	else { skyVis = nullptr; extSky = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f); }
	extSkyTexture = skyVis;
	BindMaterial();	// UNOFFICIAL 2026-10-20 MaterialAware (stage 15)
	if (skyVis) {
		SetPassTexture(14, skyVis);	// set after each pass begins (EffectRecord::PassTextures)
		const DWORD linear[][2] = { {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
			{D3DSAMP_MAGFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MIPFILTER, D3DTEXF_NONE} };
		for (const auto& s : linear) TheRenderManager->SetSamplerState(14, (D3DSAMPLERSTATETYPE)s[0], s[1]);
	}
	{
		// UNOFFICIAL 2026-10-26 (deep/fork-hiz): the trace on its own F10 line (inside "Reflections (SSR)"), for the HiZ A/B.
		static GpuTimer traceTimer("  Reflections trace (half)");
		GpuProfileScope gpu(traceTimer, Device);
		IDirect3DSurface9* traceTarget = temporal ? Textures.TraceSurface : Textures.TempSurface;
		if (!RtTrace(Device, traceTarget)) pass(traceTarget, 1);	// UNOFFICIAL 2026-10-28: RT reflections, or the trace as before
	}
	if (temporal) {
		// UNOFFICIAL 2026-10-29 ObjectMotion: its buffer on stage 11 for Temporal only (the ortho map's stage; the trace above has read it,
		// and the composite's SetCT binds the ortho map again). TemporalPS reads it while TESR_ObjectMotionData.x is 1.
		if (ObjectMotion::Data.x > 0.0f && ObjectMotion::Texture) SetPassTexture(11, ObjectMotion::Texture);
		pass(Textures.TempSurface, 3);
		Device->StretchRect(Textures.TempSurface, NULL, Textures.HistorySurface, NULL, D3DTEXF_NONE);
	}
	if (rrRaw && rtResolved) {
		// UNOFFICIAL 2026-10-30 DLSS RR (above): the resolve (in Temp, as with Temporal 0) copied into Buffer, what the composite reads.
		// (Resolve cannot draw into Buffer itself: every pass binds Buffer as an input.)
		Device->StretchRect(Textures.TempSurface, NULL, Textures.BufferSurface, NULL, D3DTEXF_NONE);
		static bool logged = false;
		if (!logged) { logged = true; Logger::Log("UNOFFICIAL DLSS RR: Reflections: its own denoiser is skipped (RR denoises the raw signal)"); }
	}
	else {
		pass(Textures.TraceSurface, 2);
		pass(Textures.BufferSurface, 4);
	}
	ClearPassTextures();

	// UNOFFICIAL 2026-10-31 RT water: Buffer now holds the water's RT reflection; next frame's water shaders read it (UpdateWaterConstants).
	if (waterFrame && rtResolved) {
		rtWaterFrame = now;
		rtWaterView = view;
		rtWaterInvView = invView;
		rtWaterProj = TheRenderManager->projMatrix;
		rtWaterScaleX = RenderRect::Active() ? RenderRect::ScaleX : 1.0f;	// UNOFFICIAL 2026-11-03 (Buffer's drawn part this frame)
		rtWaterScaleY = RenderRect::Active() ? RenderRect::ScaleY : 1.0f;
	}

	haveHistory = temporal;
	lastRenderedFrame = now;
	lastView = view;
	lastInvView = invView;
	lastProj = TheRenderManager->projMatrix;
}

// UNOFFICIAL 2026-10-31 RT water reflections ([Shaders.Reflections.RT] Water, docs/next/FORK-RT-REFLECTIONS.md "Water"): the water
// shaders (WATER000 / 017 / 033) draw in the world pass, before this frame's trace; they read LAST frame's Buffer, where the water's
// pixels hold its RT reflection (the composite leaves them alone), reprojected to this frame's camera. Valid only right after a frame
// that traced the water, with no camera jump; else 0 and the planar map as before. WaterSkipPlanar then skips the planar pass.
// Every frame, before the world pass (ShaderManager::UpdateConstants), the effect on or off.
void ReflectionsEffect::UpdateWaterConstants() {
	const UINT now = TheFrameRateManager->FrameNumber;
	bool valid = Enabled && Settings.RtWater && rtWaterFrame != 0 && rtWaterFrame + 1 == now && RtWanted() &&
		!TheShaderManager->GameState.isUnderwater && !TheShaderManager->GameState.isCameraCut;
	const D3DXMATRIX& invView = TheRenderManager->InvViewMatrix;
	const double move[3] = { (double)invView._41 - rtWaterInvView._41, (double)invView._42 - rtWaterInvView._42,
		(double)invView._43 - rtWaterInvView._43 };
	if (valid) valid = move[0] * move[0] + move[1] * move[1] + move[2] * move[2] < 500.0 * 500.0;
	if (valid) {
		// As RenderBuffers' Reproject: camera-relative world now -> that frame's view (its rotation round the camera that moved by
		// 'move'), then its projection. The water shaders' TEXCOORD0 is camera-relative world (WorldMat; + TESR_CameraPosition = world).
		D3DXMATRIX toView;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) toView.m[i][j] = rtWaterView.m[i][j];
			toView.m[i][3] = 0.0f;
		}
		for (int j = 0; j < 3; j++)
			toView.m[3][j] = (float)(move[0] * rtWaterView.m[0][j] + move[1] * rtWaterView.m[1][j] + move[2] * rtWaterView.m[2][j]);
		toView.m[3][3] = 1.0f;
		D3DXMatrixMultiply(&Constants.WaterReproject, &toView, &rtWaterProj);
		// UNOFFICIAL 2026-11-03 RenderScale < 1: a rect frame traced the water into the top-left of Buffer. The water shader takes
		// uv = clip.xy / w x (0.5, -0.5) + 0.5; pre-scaled here so that uv comes out as s x uv (as RT lamp shadows' lookup): column x' =
		// s x + (s - 1) w, y' = s y - (s - 1) w. Its inside test then reads TESR_RTWaterData.w (the scale) as the edge. Native: unchanged.
		if (rtWaterScaleX < 1.0f || rtWaterScaleY < 1.0f) {
			D3DXMATRIX& m = Constants.WaterReproject;
			for (int r = 0; r < 4; r++) {
				m.m[r][0] = rtWaterScaleX * m.m[r][0] + (rtWaterScaleX - 1.0f) * m.m[r][3];
				m.m[r][1] = rtWaterScaleY * m.m[r][1] - (rtWaterScaleY - 1.0f) * m.m[r][3];
			}
		}
	}
	Constants.WaterData = D3DXVECTOR4(valid ? 1.0f : 0.0f, valid && Settings.RtWaterSkipPlanar ? 1.0f : 0.0f, 1.0f, valid ? rtWaterScaleX : 1.0f);
}

// UNOFFICIAL 2026-10-31 SkyPanorama ([Shaders.Reflections.RT], docs/next/FORK-RT-REFLECTIONS.md "Sky at a miss"): the sky as seen,
// gathered from this frame's sky pixels into a 128 x 128 hemi-octahedral map (ReflectionsSkyPano.frag), kept for the directions off
// screen. Outdoors, Vulkan passes only. Started over after a frame without it (indoors, a load, a menu) or a camera cut. The passes
// after it this frame read it on t_cloudSky (material2.y 2).
void ReflectionsEffect::RenderSkyPano(IDirect3DDevice9* Device) {
	panoReady = false;
	// (2026-11-03 RenderScale < 1: also in rect frames while the RT trace runs, its only reader there; ToScreen keeps to the rect)
	if (!Settings.RtSkyPanorama || panoFailed || !TheShaderManager->GameState.isExterior || ExtFailed ||
		!(DxvkCompute::PassesOn() || (RenderRect::Active() && RtWanted()))) return;
	if (!pano[0]) {
		for (int i = 0; i < 2; i++)
			if (FAILED(Device->CreateTexture(128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &pano[i], NULL)) ||
				FAILED(pano[i]->GetSurfaceLevel(0, &panoSurface[i]))) {
				Logger::Log("UNOFFICIAL Reflections: the sky panorama could not be created; SkyPanorama is off.");
				panoFailed = true;
				return;
			}
	}
	const UINT now = TheFrameRateManager->FrameNumber;
	const bool reset = panoFrame == 0 || panoFrame + 1 != now || TheShaderManager->GameState.isCameraCut;
	const int write = panoIndex ^ 1;
	const D3DXVECTOR4 cube = Constants.Cube;
	Constants.Cube = D3DXVECTOR4(0.0f, 128.0f, 0.0f, reset ? 1.0f : 0.0f);
	panoDrawing = true;
	Device->SetRenderTarget(0, panoSurface[write]);
	const bool drawn = ExtRender(Device, panoSurface[write], panoSurface[write], 10, false, panoSurface[write]);
	panoDrawing = false;
	Constants.Cube = cube;
	if (!drawn) return;
	panoIndex = write;
	panoFrame = now;
	panoReady = true;
	static bool logged = false;
	if (!logged) { logged = true; Logger::Log("UNOFFICIAL Reflections: the sky panorama runs (SkyPanorama): RT misses take the sky as seen."); }
}

// The cube is kept up whenever CubeMap is on (and it exists), floors reflecting or not: metal and the env maps read it too
// ([Shaders.PBR.Metal] / [Shaders.PBR.EnvMaps] CubeReflections). Not under water (what is on screen there is not the
// surroundings).
bool ReflectionsEffect::CubeWanted() {
	return Settings.CubeMap && Cubes.Env && !TheShaderManager->GameState.isUnderwater;
}

bool ReflectionsEffect::CubeReady() const {
	return Enabled && cubeValid && TheFrameRateManager->FrameNumber - cubeFrame <= 1;
}

void ReflectionsEffect::RenderCubeOnly(IDirect3DDevice9* Device) {
	cubeOnly = true;
	CubeStep(Device);
	cubeOnly = false;
}

// One cube update. Emptied first when it was not kept up last frame, in another cell, or after a jump (load, fast travel):
// what it held belongs somewhere else.
void ReflectionsEffect::CubeStep(IDirect3DDevice9* Device) {
	const bool cubeOn = CubeWanted();
	Constants.Cube2 = D3DXVECTOR4(Settings.CubeCapture, Settings.CubeFade, (float)(CubeLevels - 1), cubeOn ? 1.0f : 0.0f);
	if (!cubeOn) { cubeValid = false; return; }
	const D3DXMATRIX& invView = TheRenderManager->InvViewMatrix;
	const double dx = (double)invView._41 - cubeInvView._41, dy = (double)invView._42 - cubeInvView._42,
		dz = (double)invView._43 - cubeInvView._43;
	void* cell = Player ? Player->parentCell : nullptr;
	const bool jumped = dx * dx + dy * dy + dz * dz > 1000.0 * 1000.0;
	// UNOFFICIAL 2026-10-18: FrameNumber, not UpdateConstants calls: with those (two a frame) 'cubeFrame + 1 != frame' was true every
	// frame, so the cube was emptied every frame and never built up what was seen around.
	const UINT now = TheFrameRateManager->FrameNumber;
	const bool empty = !cubeValid || cubeFrame + 1 != now || cell != cubeCell || jumped;
	cubeSteps++;
	if (empty) cubeEmptied++;
	if (empty && cubeValid) {	// UNOFFICIAL 2026-11-01 (SessionLog / frame journal): the cube started again from nothing
		SessionLog::Count.cubeRestarts++;
		FRAME_JOURNAL("reflection cube restarted (%s)", cell != cubeCell ? "cell change" : jumped ? "camera jump" : "not stepped last frame");
	}
	RenderCube(Device, empty);
	cubeCell = cell;
	cubeValid = true;
	cubeFrame = now;
	cubeInvView = invView;
}

// The surroundings cube (techniques 5 Capture, 6 Filter): each face of the new capture cube from last frame's and the
// image; then every face of every mip of the reflection cube from the new capture. 6 + 42 small draws. The techniques
// unbind the cube they write (Texture[n] = NoTexture), so a cube is never read and written in the same pass.
void ReflectionsEffect::RenderCube(IDirect3DDevice9* Device, bool reset) {
	const int write = captureIndex ^ 1;
	Cubes.CaptureLastSlot = Cubes.Capture[captureIndex];
	Cubes.CaptureNewSlot = Cubes.Capture[write];
	Cubes.EnvSlot = Cubes.Env;
	for (int f = 0; f < 6; f++) {
		IDirect3DSurface9* face = Cubes.CaptureFace[write][f];
		Constants.Cube = D3DXVECTOR4((float)f, (float)CubeSize, 0.0f, reset ? 1.0f : 0.0f);
		Device->SetRenderTarget(0, face);
		Render(Device, face, face, 5, false, nullptr);
	}
	captureIndex = write;
	for (UINT m = 0; m < CubeLevels; m++) {
		for (int f = 0; f < 6; f++) {
			IDirect3DSurface9* face = Cubes.EnvFace[m][f];
			Constants.Cube = D3DXVECTOR4((float)f, (float)(CubeSize >> m), (float)m, 0.0f);
			Device->SetRenderTarget(0, face);
			Render(Device, face, face, 6, false, nullptr);
		}
	}
}

// --- The Vulkan passes (NVR DXVK) -----------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-25 (NVR DXVK): all eight techniques as Compute\Reflections<Technique>.frag (Reflections.glsl holds what they
// share), line for line. Render calls this per technique as it calls the D3D9 technique: the half-resolution passes, the cube
// faces and the material copy draw into the target the caller set (ExtDrawTo), the composite through the frame chain
// (ExtFullScreen). Every pass declares the same bindings: what a technique does not read, and its own target, get a placeholder
// (the depth buffer, or the other capture cube), as the techniques unbind their target (Texture[n] = NoTexture). The constants
// (four matrices: 560 bytes) go to a uniform buffer, uploaded only when they change (the 48 cube draws differ only in
// TESR_ReflectionsCube, which is in the push block). Sampler states as the .fx declares them; TESR_ReflectionsEnv's LINEAR mip
// filter needs extension 1.3 (MipSampler), otherwise, or without the three cubes, the effect keeps its D3D9 techniques.
namespace {
	// The Constants block of Reflections.glsl (std140; the matrices as the effect's SetMatrix uploads them).
	struct ReflUniforms {
		D3DXMATRIX	projection, view, shadowOrtho, reproject;
		D3DXVECTOR4	reciprocalResolution, skyColor, horizonColor, waterSettings;
		D3DXVECTOR4	data, data2, data3, data4, layout, cube2, grid, grid2, grid3, material;
		D3DXVECTOR4	depthConstants, cameraData, cameraPosition, sky, flags;
		D3DXVECTOR4	material2;	// UNOFFICIAL 2026-10-30 x 1: TESR_MaterialBuffer's roughness for the floor guess
		D3DXVECTOR4	weapon;		// UNOFFICIAL 2026-10-30 x on, y WeaponFovScale, z strength, w roughness without material data
		D3DXVECTOR4	renderScale;	// UNOFFICIAL 2026-11-03 TESR_RenderScale: xy the rect frame's scale, zw 1 / xy; 1 in a native frame
	};
	ReflUniforms ReflUploaded;	// what the uniform buffer holds (one effect instance)
	struct ReflPush { D3DXVECTOR4 cube; UInt32 linearIndex, pointIndex, mipIndex, pass; };

	// Set 1: the textures in the .fx's sampler order (binding = slot); set 2: the constants; the samplers' heap indices at 16, 20, 24.
	const NVR_EXT_BINDING_DESC ReflBindings[] = {
		{ 1, 0,  NVR_EXT_BIND_SAMPLED_IMAGE, 0,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_RenderedBuffer
		{ 1, 1,  NVR_EXT_BIND_SAMPLED_IMAGE, 1,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_DepthBuffer
		{ 1, 2,  NVR_EXT_BIND_SAMPLED_IMAGE, 2,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_NormalsBuffer
		{ 1, 3,  NVR_EXT_BIND_SAMPLED_IMAGE, 3,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_DepthBufferViewModel
		{ 1, 4,  NVR_EXT_BIND_SAMPLED_IMAGE, 4,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_ReflectionsTrace
		{ 1, 5,  NVR_EXT_BIND_SAMPLED_IMAGE, 5,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_ReflectionsBuffer
		{ 1, 6,  NVR_EXT_BIND_SAMPLED_IMAGE, 6,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_ReflectionsTemp
		{ 1, 7,  NVR_EXT_BIND_SAMPLED_IMAGE, 7,  NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_ReflectionsHistory
		{ 1, 8,  NVR_EXT_BIND_SAMPLED_IMAGE, 8,  NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ReflectionsCapture
		{ 1, 9,  NVR_EXT_BIND_SAMPLED_IMAGE, 9,  NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ReflectionsCaptureNew
		{ 1, 10, NVR_EXT_BIND_SAMPLED_IMAGE, 10, NVR_EXT_VIEW_CUBE, FALSE, 0 },	// TESR_ReflectionsEnv
		{ 1, 11, NVR_EXT_BIND_SAMPLED_IMAGE, 11, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_OrthoMapBuffer
		{ 1, 12, NVR_EXT_BIND_SAMPLED_IMAGE, 12, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_ReflectionsGridLight
		{ 1, 13, NVR_EXT_BIND_SAMPLED_IMAGE, 13, NVR_EXT_VIEW_2D,   FALSE, 0 },	// TESR_ReflectionsGridColor
		{ 1, 14, NVR_EXT_BIND_SAMPLED_IMAGE, 14, NVR_EXT_VIEW_2D,   FALSE, 0 },	// NVR_ReflectionsSkyVis
		{ 1, 15, NVR_EXT_BIND_SAMPLED_IMAGE, 15, NVR_EXT_VIEW_2D,   FALSE, 0 },	// NVR_ReflectionsMaterial
		{ 2, 0,  NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D,   FALSE, 0 },	// Constants
		{ 0, 0,  NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D,   FALSE, 16 },	// LINEAR, CLAMP
		{ 0, 0,  NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D,   FALSE, 20 },	// POINT, CLAMP
		{ 0, 0,  NVR_EXT_BIND_SAMPLER,        2, NVR_EXT_VIEW_2D,   FALSE, 24 },	// LINEAR, CLAMP, MIPFILTER LINEAR
		{ 1, 23, NVR_EXT_BIND_SAMPLED_IMAGE, 22, NVR_EXT_VIEW_2D,   FALSE, 0 },	// UNOFFICIAL 2026-10-29 TESR_GlassMask (index 20: after the samplers)
		{ 1, 24, NVR_EXT_BIND_SAMPLED_IMAGE, 23, NVR_EXT_VIEW_2D,   FALSE, 0 },	// UNOFFICIAL 2026-10-30 TESR_MaterialBuffer (index 21)
		{ 1, 25, NVR_EXT_BIND_SAMPLED_IMAGE, 24, NVR_EXT_VIEW_2D,   FALSE, 0 },	// UNOFFICIAL 2026-10-30 TESR_CloudSkyProbe (index 22)
	};
	const UINT ReflTextureCount = 16;
	// UNOFFICIAL 2026-10-26 (deep/fork-hiz): ReflectionsTraceHiZ.frag = the same bindings + the Hi-Z pyramid buffer on set 1, binding 16 (buffer slot 1).
	const NVR_EXT_BINDING_DESC ReflHiZBinding = { 1, 16, NVR_EXT_BIND_STORAGE_BUFFER, 1, NVR_EXT_VIEW_2D, FALSE, 0 };	// buffer slot 1: read-only storage and uniform buffers share one slot range (the Constants block is slot 0)
	// UNOFFICIAL 2026-10-26 debugging tool: the counting build (-DNVR_COUNTERS=1) adds Counters.glsl's block (set 3, binding 0) as a
	// written storage buffer on slot 7. Counter and probe names as ReflectionsTraceHiZ.frag uses them.
	const UINT ReflCounterSlot = 7;
	ShaderCounters HiZCounters("Reflections Hi-Z trace",
		{ "rays", "walked to the end / off screen", "walk hit", "hit thrown away", "hit by the fixed-step rest", "out of cells, rest found nothing",
		  "not started", "cells walked", "skipped facing", "skipped own plane", "skipped behind (thickness)", "no reflection" },
		{ "endRule/cells/hitT/confidence", "facing/plane/thick/roughness", "hit.xy/depth/amount" });

	// UNOFFICIAL 2026-10-28 RT reflections: ReflectionsTraceRT.frag = the same bindings + its RtConstants block (set 2, binding 1, uniform
	// buffer slot 2: slot 1 is the Hi-Z storage buffer, and a uniform and a storage buffer on one slot number are one resource) + RayGen's
	// three outputs and the helper's hits (set 1, bindings 17-20, image slots 16-19).
	const UINT RtUniformSlot = 2;
	const NVR_EXT_BINDING_DESC ReflRtUniformBinding = { 2, 1, NVR_EXT_BIND_UNIFORM_BUFFER, RtUniformSlot, NVR_EXT_VIEW_2D, FALSE, 0 };
	struct ReflRtUniforms { D3DXVECTOR4 rt, rt2, sunDir, sunColor, rt3, rt4, lamps[24]; };	// ReflectionsRT.glsl RtConstants (std140)
	ReflRtUniforms RtUploaded;

	IDirect3DBaseTexture9* SlotTexture(const char* name) {
		std::string slotName = name;
		IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(slotName);
		return slot ? *slot : nullptr;
	}
	// The constants from the same registered memory the effect's SetCT uploads.
	const D3DXVECTOR4* Registered(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}
}

bool ReflectionsEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	// (8, 9: UNOFFICIAL 2026-10-28 RT reflections, RayGen and Resolve, drawn only from RtTrace: no D3D9 technique behind them.)
	// (10: UNOFFICIAL 2026-10-31 the sky panorama, drawn only from RenderSkyPano.)
	if (techniqueIndex > 10 || (techniqueIndex == 10 && !panoDrawing) || ClearRenderTarget || (techniqueIndex == 7 && !ReflMaterial::CompiledIn())) return false;
	const bool rtPass = techniqueIndex == 8 || techniqueIndex == 9;
	static const char* const spvNames[8] = { "ReflectionsComposite", "ReflectionsTrace", "ReflectionsBlur", "ReflectionsTemporal", "ReflectionsBlur",
		"ReflectionsCapture", "ReflectionsFilter", "ReflectionsMaterial" };
	// UNOFFICIAL 2026-10-26 (deep/fork-hiz) HiZ: the trace as the hierarchical march when this frame's pyramid is there (core/HiZ.h).
	ID3D9NvrExtBuffer* hiz = techniqueIndex == 1 && HiZWanted() ? HiZ::Current() : nullptr;	// (the RT passes use the fixed-step march)
	if (hiz && !extTraceHiZ.Tried) {
		extTraceHiZ.Tried = true;
		NVR_EXT_BINDING_DESC bindings[_countof(ReflBindings) + 1];
		memcpy(bindings, ReflBindings, sizeof(ReflBindings));
		bindings[_countof(ReflBindings)] = ReflHiZBinding;
		std::vector<char> words;	// checked first: a missing file must not turn the other Vulkan passes off
		if (DxvkCompute::LoadSpirv("ReflectionsTraceHiZ", words))
			extTraceHiZ.Pipeline = DxvkCompute::CreateGraphicsPipeline("ReflectionsTraceHiZ", bindings, _countof(bindings), sizeof(ReflPush), true);
		if (!extTraceHiZ.Pipeline) {
			hizTraceFailed = true;
			Logger::Log("UNOFFICIAL Reflections: the Hi-Z trace could not be set up; the normal trace is used.");
		}
	}
	if (!extTraceHiZ.Pipeline) hiz = nullptr;
	// UNOFFICIAL 2026-10-26 debugging tool ([Main.Develop.ShaderCounters] ReflectionsHiZ, needs NVR DXVK 1.5-debug): the Hi-Z trace's
	// counting build, with the counter block bound; its probe pixel is the pixel inspector's, in this half-resolution target. Any
	// problem (file missing, pipeline refused, no 1.5-debug readback): the normal Hi-Z trace, as with the setting off.
	ID3D9NvrExtBuffer* counters = nullptr;
	if (hiz && TheSettingManager->GetSettingI("Main.Develop.ShaderCounters", "ReflectionsHiZ") != 0) {
		if (!extTraceHiZCount.Tried) {
			extTraceHiZCount.Tried = true;
			NVR_EXT_BINDING_DESC bindings[_countof(ReflBindings) + 2];
			memcpy(bindings, ReflBindings, sizeof(ReflBindings));
			bindings[_countof(ReflBindings)] = ReflHiZBinding;
			bindings[_countof(ReflBindings) + 1] = ShaderCounters::Binding(ReflCounterSlot);
			if (DebugReadback::Interface() && DxvkCompute::HasSpirv("ReflectionsTraceHiZCount"))
				extTraceHiZCount.Pipeline = DxvkCompute::CreateGraphicsPipeline("ReflectionsTraceHiZCount", bindings, _countof(bindings), sizeof(ReflPush), true, 1, NVR_EXT_BLEND_NONE, false);
			if (!extTraceHiZCount.Pipeline) Logger::Log("UNOFFICIAL Reflections: the Hi-Z trace's counting build is not available (needs the Vegas Shaders DXVK fork 1.5-debug and the shader file ReflectionsTraceHiZCount.spv); the normal trace runs.");
		}
		if (extTraceHiZCount.Pipeline) {
			D3DSURFACE_DESC target = {};
			int px = -1, py = -1;
			if (RenderTarget && SUCCEEDED(RenderTarget->GetDesc(&target))) PixelInspector::PixelFor(target.Width, target.Height, px, py);
			counters = HiZCounters.Begin(px, py);
		}
	}
	ExtPass& pass = techniqueIndex == 10 ? extPanoPass : rtPass ? extRtPasses[techniqueIndex - 8] : counters ? extTraceHiZCount : hiz ? extTraceHiZ :
		extPasses[techniqueIndex == 4 ? 2 : techniqueIndex];
	if (!pass.Tried) {
		pass.Tried = true;
		if (techniqueIndex == 10) {	// UNOFFICIAL 2026-10-31 SkyPanorama (a refusal must not turn the other Vulkan passes off)
			if (DxvkCompute::HasSpirv("ReflectionsSkyPano"))
				pass.Pipeline = DxvkCompute::CreateGraphicsPipeline("ReflectionsSkyPano", ReflBindings, _countof(ReflBindings), sizeof(ReflPush), true, 1, NVR_EXT_BLEND_NONE, false);
			if (!pass.Pipeline) {
				panoFailed = true;
				Logger::Log("UNOFFICIAL Reflections: the sky panorama pass could not be set up; SkyPanorama is off.");
			}
		}
		else if (rtPass) {
			// UNOFFICIAL 2026-10-28: the same bindings + RtConstants + RayGen's outputs and the helper's hits (both passes declare all).
			static const char* const rtNames[2] = { "ReflectionsRayGen", "ReflectionsRTResolve" };
			NVR_EXT_BINDING_DESC bindings[_countof(ReflBindings) + 6];
			memcpy(bindings, ReflBindings, sizeof(ReflBindings));
			bindings[_countof(ReflBindings)] = ReflRtUniformBinding;
			for (UINT i = 0; i < 5; i++) bindings[_countof(ReflBindings) + 1 + i] = { 1, 17 + i, NVR_EXT_BIND_SAMPLED_IMAGE, ReflTextureCount + i, NVR_EXT_VIEW_2D, FALSE, 0 };
			if (DxvkCompute::HasSpirv(rtNames[techniqueIndex - 8]))
				pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(rtNames[techniqueIndex - 8], bindings, _countof(bindings), sizeof(ReflPush), true,
					techniqueIndex == 8 ? 3 : 2, NVR_EXT_BLEND_NONE, false);	// a refusal must not turn the other Vulkan passes off (Resolve: + Rt.Spread)
			if (!pass.Pipeline) {
				rtTraceFailed = true;
				Logger::Log("UNOFFICIAL Reflections: the RT pass %s could not be set up; the normal trace is used.", rtNames[techniqueIndex - 8]);
			}
		}
		else if (techniqueIndex == 2 || techniqueIndex == 4) {
			// UNOFFICIAL 2026-10-29: the blur passes also declare RT reflections' hit-distance share (set 1, binding 22, image slot 21;
			// a placeholder and push cube.w 0 on frames without RT: the blur exactly as before).
			NVR_EXT_BINDING_DESC bindings[_countof(ReflBindings) + 1];
			memcpy(bindings, ReflBindings, sizeof(ReflBindings));
			bindings[_countof(ReflBindings)] = { 1, 22, NVR_EXT_BIND_SAMPLED_IMAGE, ReflTextureCount + 5, NVR_EXT_VIEW_2D, FALSE, 0 };
			pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvNames[techniqueIndex], bindings, _countof(bindings), sizeof(ReflPush), true);
		}
		else pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(spvNames[techniqueIndex], ReflBindings, _countof(ReflBindings), sizeof(ReflPush), true);
	}
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* mip = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_CLAMP);
	if (!pass.Pipeline || !linear || !point || !mip) return false;

	const D3DXVECTOR4* projection = Registered("TESR_ProjectionTransform");
	const D3DXVECTOR4* view = Registered("TESR_ViewTransform");
	const D3DXVECTOR4* shadowOrtho = Registered("TESR_ShadowCameraToLightTransformOrtho");
	const D3DXVECTOR4* reciprocal = Registered("TESR_ReciprocalResolution");
	const D3DXVECTOR4* skyColor = Registered("TESR_SkyColor");
	const D3DXVECTOR4* horizonColor = Registered("TESR_HorizonColor");
	const D3DXVECTOR4* waterSettings = Registered("TESR_WaterSettings");
	const D3DXVECTOR4* depthConstants = Registered("TESR_DepthConstants");
	const D3DXVECTOR4* cameraData = Registered("TESR_CameraData");
	const D3DXVECTOR4* cameraPosition = Registered("TESR_CameraPosition");
	if (!projection || !view || !shadowOrtho || !reciprocal || !skyColor || !horizonColor || !waterSettings || !depthConstants || !cameraData || !cameraPosition)
		return false;

	// The textures as SetCT binds them. Those read without a guard must exist; the guarded ones (the light grid, the sky
	// visibility, the material buffer, the ortho map without the roof test) take the placeholder when absent, which is never read.
	IDirect3DBaseTexture9* depth = SlotTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = SlotTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* viewModel = SlotTexture("TESR_DepthBufferViewModel");
	IDirect3DBaseTexture9* ortho = SlotTexture("TESR_OrthoMapBuffer");
	if (!depth || !normals || !viewModel || !TheTextureManager->RenderedTexture || (!ortho && Constants.Data4.y > 0.5f) ||
		!Cubes.Capture[0] || !Cubes.Capture[1] || !Cubes.Env)
		return false;
	const bool materialBound = materialFilled && Textures.MaterialTexture != nullptr;	// BindMaterial's test
	IDirect3DBaseTexture9* textures[ReflTextureCount] = {
		TheTextureManager->RenderedTexture, depth, normals, viewModel,
		Textures.TraceTexture, Textures.BufferTexture, Textures.TempTexture, Textures.HistoryTexture,
		Cubes.CaptureLastSlot ? Cubes.CaptureLastSlot : Cubes.Capture[0], Cubes.CaptureNewSlot ? Cubes.CaptureNewSlot : Cubes.Capture[1], Cubes.Env,
		ortho ? ortho : depth, GridLightSlot ? GridLightSlot : depth, GridColorSlot ? GridColorSlot : depth,
		extSkyTexture ? extSkyTexture : depth, materialBound ? Textures.MaterialTexture : depth };
	// A pass never reads its own target.
	switch (techniqueIndex) {
		case 1: case 8: case 9: textures[4] = textures[6] = depth; break;	// Trace (RT: RayGen, Resolve) -> Trace (Temporal on) or Temp
		case 2: textures[4] = depth; break;					// Blur X -> Trace
		case 3: textures[6] = depth;						// Temporal -> Temp
			if (ObjectMotion::Ready()) textures[11] = ObjectMotion::Texture;	// UNOFFICIAL 2026-10-29 ObjectMotion (push.pass 2 below)
			break;
		case 4: textures[5] = depth; break;					// Blur Y -> Buffer
		case 5: textures[9] = textures[8]; break;			// Capture -> a face of the new capture cube
		case 6: textures[10] = textures[9]; break;			// Filter -> a face of the reflection cube
		case 7: textures[15] = depth; break;				// MaterialCapture -> the material buffer
		case 10: textures[4] = pano[panoIndex]; break;		// UNOFFICIAL 2026-10-31 SkyPanorama: t_trace = last frame's panorama (-> the other one)
	}
	for (UINT i = 0; i < ReflTextureCount; i++) if (!textures[i]) return false;

	ReflUniforms uniforms = {};
	memcpy(&uniforms.projection, projection, sizeof(D3DXMATRIX));
	memcpy(&uniforms.view, view, sizeof(D3DXMATRIX));
	memcpy(&uniforms.shadowOrtho, shadowOrtho, sizeof(D3DXMATRIX));
	uniforms.reproject = Constants.Reproject;
	uniforms.reciprocalResolution = *reciprocal;
	uniforms.skyColor = *skyColor;
	uniforms.horizonColor = *horizonColor;
	uniforms.waterSettings = *waterSettings;
	uniforms.data = Constants.Data;
	uniforms.data2 = Constants.Data2;
	uniforms.data3 = Constants.Data3;
	uniforms.data4 = Constants.Data4;
	uniforms.layout = Constants.Layout;
	uniforms.cube2 = Constants.Cube2;
	uniforms.grid = Constants.Grid;
	uniforms.grid2 = Constants.Grid2;
	uniforms.grid3 = Constants.Grid3;
	// (2026-11-06) MirrorSmoothOnly: the sign tells the shaders (Reflections.glsl Reflectivity): negative = only surfaces under roughness 0.05
	uniforms.grid3.w = Settings.MirrorSmoothOnly ? -Settings.Mirror : Settings.Mirror;	// UNOFFICIAL 2026-11-05 Mirror: the reflecting surfaces' F0 floor, 0.04 .. 1 (ReflectionsComposite.frag)
	uniforms.material = Constants.Material;
	uniforms.material.w = GlassMask::ThisFrame() ? Settings.GlassReflect * (TheShaderManager->GameState.isExterior ? Settings.ReflectivityExterior : Settings.ReflectivityInterior) : 0.0f;	// UNOFFICIAL 2026-10-29
	uniforms.depthConstants = *depthConstants;
	uniforms.cameraData = *cameraData;
	uniforms.cameraPosition = *cameraPosition;
	uniforms.sky = extSky;
	// UNOFFICIAL 2026-10-31 RT water: z 1 = water pixels reflect with RT rays this frame (RayGen casts, the composite leaves them to
	// the water shaders), w their roughness (Reflections.glsl Reflectivity).
	uniforms.sky.z = waterFrame && RtWanted() ? (Settings.RtWaterSkipPlanar ? 2.0f : 1.0f) : 0.0f;	// (2026-11-05: 2 = the planar pass is skipped)
	uniforms.sky.w = Settings.RtWaterRoughness;
	uniforms.flags = D3DXVECTOR4(ReflMaterial::CompiledIn() ? 1.0f : 0.0f, ReflMaterial::AddCompiledIn() ? 1.0f : 0.0f,
		ReflMaterial::OccCompiledIn() ? 1.0f : 0.0f, materialBound ? 1.0f : 0.0f);
	// UNOFFICIAL 2026-10-30 MaterialBufferRoughness: the material buffer (core/CapturedNormals.h, [Main.Main.Quality] MaterialBuffer) when
	// this build registers it and it is made; looked up once, by name, so a build without it simply leaves this off.
	static IDirect3DBaseTexture9** materialBuffer = nullptr;
	static bool materialLooked = false;
	if (Settings.MaterialBufferRoughness && !materialLooked) {
		materialLooked = true;
		std::string name = "TESR_MaterialBuffer";
		materialBuffer = TheTextureManager->GetTextureSlotByName(name);
		Logger::Log("UNOFFICIAL Reflections MaterialBufferRoughness: %s", materialBuffer ? "TESR_MaterialBuffer found" : "no TESR_MaterialBuffer in this build: off");
	}
	IDirect3DBaseTexture9* matbuf = Settings.MaterialBufferRoughness && materialBuffer ? *materialBuffer : nullptr;
	// UNOFFICIAL 2026-10-30 CloudSky: the cloud sky probe (VolumetricClouds RenderSkyProbe), outdoors, while it is fresh.
	VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
	IDirect3DTexture9* cloudSky = Settings.CloudSky && clouds && clouds->Enabled && clouds->SkyProbeReady() && TheShaderManager->GameState.isExterior ?
		clouds->Textures.SkyProbeTexture : nullptr;
	float cloudBrightness = 1.0f;
	{
		auto found = TheShaderManager->ConstantsTable.find("TESR_CloudsLayout");
		if (found != TheShaderManager->ConstantsTable.end() && found->second) cloudBrightness = found->second->w;
	}
	uniforms.material2 = D3DXVECTOR4(matbuf ? 1.0f : 0.0f, cloudSky ? 1.0f : 0.0f, cloudBrightness, Settings.RtWeaponMetal);	// w: WeaponMetal (2026-10-30)
	// UNOFFICIAL 2026-10-31 SkyPanorama: the passes after it this frame read the panorama on t_cloudSky (y 2); its own pass reads the probe.
	IDirect3DBaseTexture9* skySlot = cloudSky;
	if (panoReady && techniqueIndex != 10) { skySlot = pano[panoIndex]; uniforms.material2.y = 2.0f; }
	// UNOFFICIAL 2026-10-30 RT weapon reflections: only in a frame the RT trace runs (RayGen, Resolve, temporal, blur, composite agree).
	const bool weaponOn = weaponFrame && RtWanted();
	uniforms.weapon = D3DXVECTOR4(weaponOn ? 1.0f : 0.0f, WeaponFovScale(), Settings.RtWeapon * (TheShaderManager->GameState.isExterior ? Settings.ReflectivityExterior : Settings.ReflectivityInterior),
		Settings.RtWeaponRoughness);
	// UNOFFICIAL 2026-11-03 RT reflections at RenderScale < 1: in a rect frame the passes drawn here (RayGen, Resolve, the panorama)
	// keep to its top-left part, as the .fx's techniques do with TESR_RenderScale.
	uniforms.renderScale = RenderRect::Active() ? D3DXVECTOR4(RenderRect::ScaleX, RenderRect::ScaleY, 1.0f / RenderRect::ScaleX, 1.0f / RenderRect::ScaleY) :
		D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
	ID3D9NvrExtBuffer* buffer = extUniforms.Buffer;
	if (!buffer || memcmp(&uniforms, &ReflUploaded, sizeof(uniforms))) {
		buffer = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
		if (!buffer) return false;
		ReflUploaded = uniforms;
	}

	NVR_EXT_RESOURCE_BINDING resources[_countof(ReflBindings) + 6] = {};
	for (UINT i = 0; i < ReflTextureCount; i++) {
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[16].Slot = 0; resources[16].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[16].pResource = buffer;
	resources[17].Slot = 0; resources[17].Type = NVR_EXT_BIND_SAMPLER; resources[17].pResource = linear;
	resources[18].Slot = 1; resources[18].Type = NVR_EXT_BIND_SAMPLER; resources[18].pResource = point;
	resources[19].Slot = 2; resources[19].Type = NVR_EXT_BIND_SAMPLER; resources[19].pResource = mip;
	// UNOFFICIAL 2026-10-29 glass reflections: the glass mask (core/GlassMask.h), or the depth as a placeholder (never read: material.w 0).
	IDirect3DTexture9* glass = GlassMask::ThisFrame();
	resources[20].Slot = 22; resources[20].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[20].pResource = glass ? (IUnknown*)glass : (IUnknown*)depth;
	resources[21].Slot = 23; resources[21].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[21].pResource = matbuf ? (IUnknown*)matbuf : (IUnknown*)depth;	// UNOFFICIAL 2026-10-30
	resources[22].Slot = 24; resources[22].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[22].pResource = skySlot ? (IUnknown*)skySlot : (IUnknown*)depth;	// UNOFFICIAL 2026-10-30 (2026-10-31: or the panorama)
	UINT resourceCount = _countof(ReflBindings);
	if (hiz) {
		resources[resourceCount].Slot = 1; resources[resourceCount].Type = NVR_EXT_BIND_STORAGE_BUFFER; resources[resourceCount].pResource = hiz;
		resourceCount++;
		if (++hizTraceFrames == 1) Logger::Log("UNOFFICIAL Reflections: the trace runs as the Hi-Z march (HiZ).");
	}
	if (counters) resources[resourceCount++] = ShaderCounters::Bind(ReflCounterSlot, counters);	// UNOFFICIAL 2026-10-26 debugging tool
	if (rtPass) {
		// UNOFFICIAL 2026-10-28 RT reflections: the RtConstants block (ReflectionsRT.glsl) and the RT images (RayGen: placeholders).
		const ShaderConstants& sc = TheShaderManager->ShaderConst;
		ReflRtUniforms rtu = {};
		rtu.rt = D3DXVECTOR4(rtRays ? 1.0f : 0.0f, Settings.RtMaxRoughness, Settings.RtDistance, (float)Settings.RtDebugView);
		rtu.rt2 = D3DXVECTOR4(Settings.RtScreenTolerance, Settings.RtEstimate, RtSunAmount(), 0.0f);
		rtu.sunDir = sc.SunDir;
		rtu.sunColor = sc.sunColor;	// as the game gives it (gamma); ReflectionsRT.glsl linearizes
		rtu.rt2.w = (float)RtLampCount;	// UNOFFICIAL 2026-10-29: the lamps at the hits
		rtu.rt3 = D3DXVECTOR4(Settings.RtVndf ? 1.0f : 0.0f, Settings.RtRoughShare, Settings.RtContactSpread, Settings.RtGlowBrightness);
		rtu.rt4 = D3DXVECTOR4(Settings.RtHitAmbientGrid ? 1.0f : 0.0f, Settings.RtRayReuse ? 1.0f : 0.0f, Settings.RtReuseRoughness, Settings.RtShadeOffscreen ? 1.0f : 0.0f);	// UNOFFICIAL 2026-10-30 / 31; w 2026-11-05 ShadeOffscreen
		memcpy(rtu.lamps, RtLampData, sizeof(RtLampData));
		ID3D9NvrExtBuffer* rtBuffer = extRtUniforms.Buffer;
		if (!rtBuffer || memcmp(&rtu, &RtUploaded, sizeof(rtu))) {
			rtBuffer = ExtUniforms(extRtUniforms, &rtu, sizeof(rtu));
			if (!rtBuffer) return false;
			RtUploaded = rtu;
		}
		resources[resourceCount].Slot = RtUniformSlot; resources[resourceCount].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[resourceCount].pResource = rtBuffer;
		resourceCount++;
		IDirect3DBaseTexture9* rtImages[5] = { Rt.March, Rt.Dir, Rt.Ray, Rt.Hit, Rt.Shade };
		for (UINT i = 0; i < 5; i++) {
			if (!rtImages[i]) return false;
			resources[resourceCount].Slot = ReflTextureCount + i; resources[resourceCount].Type = NVR_EXT_BIND_SAMPLED_IMAGE;
			resources[resourceCount].pResource = techniqueIndex == 8 ? depth : rtImages[i];
			resourceCount++;
		}
	}
	ReflPush push = {};
	push.cube = Constants.Cube;
	push.pass = techniqueIndex == 4 ? 1 : (techniqueIndex == 0 && Cubes.EnvSlot) ? 1 : 0;	// Blur Y; the composite: the cube is bound
	if (techniqueIndex == 2 || techniqueIndex == 4) {
		// UNOFFICIAL 2026-10-29 RT reflections: the hit-distance share (Resolve drew it this frame) and the narrowest radius share.
		const bool spread = rtResolved && Rt.Spread && Settings.RtContactSharpness < 1.0f;
		resources[resourceCount].Slot = ReflTextureCount + 5; resources[resourceCount].Type = NVR_EXT_BIND_SAMPLED_IMAGE;
		resources[resourceCount].pResource = spread ? (IUnknown*)Rt.Spread : (IUnknown*)depth;
		resourceCount++;
		push.cube.w = spread ? Settings.RtContactSharpness : 0.0f;
	}
	if (techniqueIndex == 3 && ObjectMotion::Ready()) push.pass = 2;	// UNOFFICIAL 2026-10-29 ObjectMotion: Temporal reads its buffer on t_ortho
	if (techniqueIndex == 10) push.pass = cloudSky ? 1 : 0;	// UNOFFICIAL 2026-10-31 SkyPanorama: the cloud probe is bound for the estimate

	if (techniqueIndex == 0)
		return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, pass.Pipeline, resources, resourceCount, &push, sizeof(push));
	// (RayGen: its three targets, RtTrace's own; RenderTarget is the first.)
	IDirect3DSurface9* rayGenTargets[3] = { Rt.MarchSurface, Rt.DirSurface, Rt.RaySurface };
	IDirect3DSurface9* resolveTargets[2] = { RenderTarget, Rt.SpreadSurface };
	const bool drawn = techniqueIndex == 8 ? ExtDrawTo(pass.Pipeline, rayGenTargets, 3, resources, resourceCount, &push, sizeof(push), nullptr, ExtUv::ScreenQuad) :
		techniqueIndex == 9 ? ExtDrawTo(pass.Pipeline, resolveTargets, 2, resources, resourceCount, &push, sizeof(push), nullptr, ExtUv::ScreenQuad) :
		ExtDrawTo(pass.Pipeline, &RenderTarget, 1, resources, resourceCount, &push, sizeof(push), nullptr, ExtUv::ScreenQuad);	// FrameVS into a smaller target
	if (counters) HiZCounters.End();	// UNOFFICIAL 2026-10-26: queued for the non-waiting readback
	return drawn;
}

// UNOFFICIAL 2026-11-01 (core/Diagnostics.cpp, the REPORT block: a tester saw "nothing" from RT reflections): what the RT trace did last frame
std::string ReflectionsEffect::RtStatusText() const {
	if (!Enabled) return "Reflections off";
	if (!Settings.RtEnabled) return "Reflections on, RT off ([Shaders.Reflections.RT] Enabled false): the screen-space trace alone";
	const char* why = ExtFailed ? "its Vulkan passes failed (D3D9 fallback, no RT)" : rtTraceFailed ? "an RT pass or image could not be made (logged): the normal trace" :
		!RtPassesWanted() ? "the Vegas Shaders DXVK passes are off (NvrDxvkPasses off, or no Vegas Shaders DXVK fork): the normal trace" :
		!RtCoreClient::Ready() && !Settings.RtSplitTest ? "the RT core is not running (no helper, or it is still starting): the normal trace" : nullptr;
	char t[500];
	sprintf_s(t, "Reflections on, RT on: %s | last frame: %s | frames traced with RT %u, frames the helper traced nothing %u | MaxRoughness %.2f, Distance %.0f, "
		"ScreenTolerance %.2f, Weapon %.2f, Water %s, SplitTest %s, DebugView %d (1 shares, 2 ray hits, 3 RT alone without the screen march, 4 hit flags)",
		why ? why : "running", rtResolved ? "the RT trace was drawn" : "the normal trace was drawn", rtTraceFrames, rtFallbackFrames, Settings.RtMaxRoughness, Settings.RtDistance,
		Settings.RtScreenTolerance, Settings.RtWeapon, Settings.RtWater ? "on" : "off", Settings.RtSplitTest ? "on" : "off", Settings.RtDebugView);
	return t;
}
