#include "ShadowsExterior.h"
#include "../Puddles/PuddleMap.h"	// 2026-11-12 Puddles map mode
#include "../../core/TestTools.h"	// UNOFFICIAL: NVR_TEST_TOOLS (SkipWhenFadedCheck)
#include "../../core/LampFixtures.h"	// UNOFFICIAL 2026-10-20: LampModelOwnDistance
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/SunDepthFit.h"	// UNOFFICIAL 2026-10-27 FitToDepth
#include "../../core/RtSunShadows.h"	// UNOFFICIAL 2026-10-28 RT sun shadows
#include "../../core/BendContact.h"	// UNOFFICIAL 2026-11-06 forward Bend contact shadows

// UNOFFICIAL 2026-10-16 lamp contact shadows (PointShadows.fx LampContactPS, technique 2; drawn by ShaderManager after the lamp
// pass): every number the shader needs (see the constants' comments there). Kept out of ShadowsExterior.h (widely included).
static D3DXVECTOR4 LampContact(0.0f, 40.0f, 12.0f, 0.5f), LampContactFade(25.0f, 0.05f, 3000.0f, 1.0f / 900.0f),
	LampContactBias(0.3f, 0.002f, 0.001f, 0.2f), LampContactNoise(0.0f, 0.0f, 52.9829189f, 0.0f);
float LampContactStrength() { return LampContact.x; }
// UNOFFICIAL 2026-10-16 MaskHalfRes ([Shaders.ShadowsInteriors.Forward]): the lamp mask at half resolution (PointShadows.fx
// LampMaskHalf + LampMaskUpsample; ShaderManager draws them). R = mask, G = depth.
static bool MaskHalfResOn = false;
static IDirect3DTexture9* MaskHalfTexture = nullptr;
static IDirect3DSurface9* MaskHalfSurface = nullptr;
bool LampMaskHalfRes() { return MaskHalfResOn && MaskHalfTexture && MaskHalfSurface; }
IDirect3DTexture9* LampMaskHalfTexture() { return MaskHalfTexture; }
IDirect3DSurface9* LampMaskHalfSurface() { return MaskHalfSurface; }

// UNOFFICIAL 2026-10-27 [Shaders.ShadowsExteriors.ScreenSpace] NearFade: TESR_ShadowScreenSpaceFade (SunShadows.fx ContactNearFade; the
// Vulkan passes take x, y in their flags.y, z): contact shadows none nearer than x, full from y (view depth). Here, not in Constants, so
// the header (and everything that includes it) stays as it was.
static D3DXVECTOR4 ScreenSpaceFade(0.0f, 0.0f, 0.0f, 0.0f);
static float ScreenSpaceNearFade = 0.0f;	// the setting (UpdateSettings); ScreenSpaceFade is made from it each frame (UpdateConstants)

void ShadowsExteriorEffect::UpdateConstants() {
	// Contact-shadow start and end: NearFade, pushed out to where the land's bumps end while those are on (DxvkCompute::DisplaceContactFade).
	BendContact::UpdateConstants();	// UNOFFICIAL 2026-11-06: last frame's Bend result, reprojected for this frame's lit shaders
	ScreenSpaceFade = D3DXVECTOR4(ScreenSpaceNearFade * 0.5f, ScreenSpaceNearFade, 0.0f, 0.0f);
	float displaceBegin = 0.0f, displaceEnd = 0.0f;
	if (DxvkCompute::DisplaceContactFade(displaceBegin, displaceEnd)) {
		ScreenSpaceFade.x = (std::max)(ScreenSpaceFade.x, displaceBegin);
		ScreenSpaceFade.y = (std::max)(ScreenSpaceFade.y, displaceEnd);
	}

	Constants.ShadowFade.x = 0; // Fade 1.0 == no shadows
	if (TheShaderManager->GameState.isExterior) {
		// the sun's shadows fade to 0 around sunrise and sunset
		Constants.ShadowFade.x = smoothStep(0.5f, 0.1f, abs(TheShaderManager->GameState.dayLight - 0.5f));

		TimeGlobals* timeGlobals = TimeGlobals::Get();
		const float daysPassed = timeGlobals->GameDaysPassed ? timeGlobals->GameDaysPassed->data : 1.0f;

		if (TheShaderManager->GameState.isDayTime < 0.5f) {
			// at night the moon phase sets the strength: the phase runs 0..8 over the moon's cycle
			const UInt8 phaseLength = Tes->sky->firstClimate->phaseLength & 0x3F;
			// UNOFFICIAL 2026-11-09 (review): was fmod(daysPassed, 8 * raw & 0x3F) = (8 * raw) & 0x3F: for a climate with a phase of 8 days or
			// more the cycle length wrapped (8 days: fmod by 0 = not a number, into the night shadows' strength), and a phase length of 0
			// divided by 0. Now 8 x the masked length, at least 1 day. Phases under 8 days (the game's own climates): the same numbers.
			const float phaseDays = (float)(phaseLength ? phaseLength : 1);
			float moonPhase = fmodf(daysPassed, 8.0f * phaseDays) / phaseDays;
			const float moonPhaseDays = moonPhase;	// UNOFFICIAL 2026-10-16: for the NIGHT STATE line below

			const float piValue = 3.1416f;	// a cosine curve over the phase fades the moon light's shadows
			moonPhase = std::lerp(-piValue, piValue, moonPhase / 8) - piValue / 4;

			// MoonVisibility runs from 0 to the NightMinDarkness range
			const float nightMinDarkness = 1 - Settings.Exteriors.NightMinDarkness;
			const float moonVisibility = std::lerp((float)0.0, nightMinDarkness, (float)(cos(moonPhase) * 0.5 + 0.5));
			Constants.ShadowFade.x = std::lerp(moonVisibility, (float)1.0, Constants.ShadowFade.x);

			// UNOFFICIAL 2026-10-16 (night cost study): logged when the fade moves by more than 0.01, or when the sun's light crosses zero
			static float loggedFade = -1.0f, loggedDayLight = -1.0f;
			if (fabsf(Constants.ShadowFade.x - loggedFade) > 0.01f || (TheShaderManager->GameState.dayLight <= 0.0f) != (loggedDayLight <= 0.0f)) {
				Logger::Log("NIGHT STATE: days passed %.2f, moon phase %.2f of 8 (phase length %d days), sun shadow fade %.3f (1 = none), dayLight %.2f, sun colour %.3f %.3f %.3f",
					daysPassed, moonPhaseDays, (int)phaseLength, Constants.ShadowFade.x,
					TheShaderManager->GameState.dayLight, TheShaderManager->ShaderConst.sunColor.x, TheShaderManager->ShaderConst.sunColor.y,
					TheShaderManager->ShaderConst.sunColor.z);
				loggedFade = Constants.ShadowFade.x;
				loggedDayLight = TheShaderManager->GameState.dayLight;
			}
		}

		// The point-light shadows' enabled flag goes into ShadowFade.z every frame (not only when the day/night state changes).
		const bool usePointLights = (TheShaderManager->GameState.isDayTime > 0.5) ? Settings.Exteriors.UsePointShadowsDay : Settings.Exteriors.UsePointShadowsNight;
		Constants.ShadowFade.z = usePointLights;
		Constants.ShadowFade.y = Settings.Exteriors.Enabled && Enabled;
		Constants.ShadowFade.w = Constants.ShadowMapRadius.w;	// furthest distance for point lights shadows
		// UNOFFICIAL 2026-10-27 FitToDepth: the lamp shadows' reach stays the unfitted one (the fit may end the cascades sooner).
		if (SunDepthFit::Enabled && SunDepthFit::UnfittedRadiusW > 0.0f) Constants.ShadowFade.w = SunDepthFit::UnfittedRadiusW;

		// x = quality, y = darkness
		Constants.Data.x = Settings.Exteriors.Quality;
		//if (Enabled) Constants.ShadowData->x = -1; // Disable the forward shadowing
		Constants.Data.y = Settings.Exteriors.Darkness;

		// x = mode, y = bits per pixel
		Constants.FormatData.x = Settings.ShadowMaps.Mode;
		Constants.FormatData.y = Settings.ShadowMaps.FormatBits;

		// z: are we in an exterior cell right now. GetSunShadow reads this on its own --
		// ShadowFade.y means something different indoors (interior point-shadows enabled,
		// not sun shadows enabled), so the sun-shadow path can't share it. See the interior
		// branch below.
		Constants.FormatData.z = 1.0f;

		// UNOFFICIAL 2026-10-16 SkipWhenFaded (dawn cost study, work/bench/foundations/dawncost): around sunrise and sunset
		// (dayLight 0.4-0.6) the fade above is exactly 1, and every lit shader's sun shadow comes out as exactly 1 whatever
		// the cascades hold (Shadow.hlsl GetSunShadow ends with lerp(shadow, 1, fade)). The forward path's own off switch,
		// read first there, returns that 1 without the cascade lookups. No shader changes.
		SunFaded = Settings.Exteriors.SkipWhenFaded && Constants.ShadowFade.x >= 1.0f;
		Constants.ForwardData.x = (!Settings.Exteriors.ForwardShadows || SunFaded) ? 1.0f : 0.0f;
		// UNOFFICIAL 2026-10-28 RT sun shadows Mode 1 (replace): the object shaders' cascade lookups off; the deferred apply does the
		// cascades (faded out inside the RT range) and the traced visibility is multiplied in after it (core/RtSunShadows.cpp).
		if (RtSunShadows::Replacing()) Constants.ForwardData.x = 1.0f;
		RtSunShadows::ReplaceRange();
#if NVR_TEST_TOOLS
		// DIAGNOSTIC SkipWhenFadedCheck 3: the lookups off at any hour (what they cost; by day the sun shadows leave the lit
		// shaders, so the picture changes).
		if (Settings.Exteriors.SkipWhenFadedCheck == 3) Constants.ForwardData.x = 1.0f;
#endif

		// UNOFFICIAL 2026-10-16: logged when it changes, with the game hours the fade is 1 in (ShaderManager's dayLight ramps:
		// sunrise from an hour before the climate's sunrise to an hour before its end, sunset an hour after; fade 1 = dayLight
		// 0.4-0.6 of either ramp).
		const int fadeState = (Constants.ShadowFade.x >= 1.0f ? 1 : 0) | (SunFaded ? 2 : 0);
		static int loggedFadeState = -1;
		if (fadeState != loggedFadeState && Tes && Tes->sky) {
			Sky* sky = Tes->sky;
			const float riseBegin = sky->GetSunriseBegin(), riseEnd = sky->GetSunriseEnd(), setBegin = sky->GetSunsetBegin(), setEnd = sky->GetSunsetEnd();
			TimeGlobals* time = TimeGlobals::Get();
			Logger::Log("SUN FADE STATE: game hour %.3f, dayLight %.3f, sun shadow fade %.4f (1 = none), SkipWhenFaded %s: sun shadow lookups %s. "
				"The fade is 1 from %.2f to %.2f (dawn) and %.2f to %.2f (dusk), game hours.",
				time && time->GameHour ? fmod(time->GameHour->data, 24.0f) : -1.0f, TheShaderManager->GameState.dayLight, Constants.ShadowFade.x,
				Settings.Exteriors.SkipWhenFaded ? "on" : "off", SunFaded ? "SKIPPED" : "run",
				riseBegin - 1.0f + 0.4f * (riseEnd - riseBegin), riseBegin - 1.0f + 0.6f * (riseEnd - riseBegin),
				setEnd + 1.0f - 0.6f * (setEnd - setBegin), setEnd + 1.0f - 0.4f * (setEnd - setBegin));
			loggedFadeState = fadeState;
		}
	}
	else {
		SunFaded = false;	// UNOFFICIAL 2026-10-16 SkipWhenFaded: nothing to skip indoors
		Constants.ForwardData.x = Settings.Exteriors.ForwardShadows ? 0.0f : 1.0f;
		// the interior shadows' enabled flag; point lights always on indoors
		Constants.ShadowFade.y = TheShaderManager->Effects.ShadowsInteriors->Enabled;
		Constants.ShadowFade.z = 1;	// z enables point lights
		Constants.ShadowFade.w = Settings.Interiors.DrawDistance;	// furthest distance for point lights shadows

		// Sun shadows never apply indoors -- unlike ShadowFade.y, this one exists solely for
		// GetSunShadow, so it's safe to just say "no" here rather than borrow another flag.
		Constants.FormatData.z = 0.0f;

		// x = quality, y = darkness
		Constants.Data.x = Settings.Interiors.Quality;
		//if (TheShaderManager->Effects.ShadowsInteriors->Enabled) Constants.Data.x = -1; // Disable the forward shadowing
		Constants.Data.y = Settings.Interiors.Darkness;
		Constants.Data.z = 1.0f / (float)Settings.Interiors.ShadowCubeMapSize;
	}

	// Force-rebind FormatData and ForwardData straight to the device once per frame, outside the per-shader constant table.
	// GetSunShadow's gates (TESR_ShadowFormatData.z, TESR_ShadowForwardData.x) sit inside the !DIFFUSE && !POINT block of
	// ObjectTemplate.hlsl, so a DIFFUSE- or POINT-lit permutation never lists these names and never refreshes c129/c133.
	// D3D9 pixel constants persist between draws, so such an object would inherit an earlier draw's values -- the cause of
	// exterior shadow state sticking after walking into an interior lit mostly by DIFFUSE/POINT objects. Both values are the
	// same for every object of the frame, so one bind here, before the world geometry, covers it.
	TheRenderManager->device->SetPixelShaderConstantF(129, (const float*)&Constants.FormatData, 1);
	TheRenderManager->device->SetPixelShaderConstantF(133, (const float*)&Constants.ForwardData, 1);
	UpdateLightColors();	// UNOFFICIAL 2026-10-19 (SunFar)
}

// UNOFFICIAL 2026-10-19, after Conor's (mrfury97) ShadowsExteriorEffect::UpdateLightColors (6043062a, GPL-3.0-or-later): the sun and
// ambient colours exactly as BSShaderLightingProperty::SetLight1x2x (0xB70820) hands them to the object shaders as PSLightColor[0]
// and AmbientColor, for the SunFar pass to split a pixel's light the way the forward path does. They are not the weather's raw
// colours (TESR_SunColor, TESR_SunAmbient): both are scaled by the light's dimmer, the sun by the HDR sunlight dimmer outdoors, and
// the ambient floored at fMinAmbient. Left out are the per-object factors SetLight also applies -- forced darkness and the LOD
// dimmers -- which a screen-space pass cannot know and which are 1 on almost everything.
void ShadowsExteriorEffect::UpdateLightColors() {
	static const bool* bHDR = (const bool*)0x11F941E;				// BSShaderManager::bHDR
	static const bool* bInterior = (const bool*)0x11F9427;			// BSShaderManager::bInterior
	static const float* fSunlightDimmer = (const float*)0x11F9190;	// BSShaderManager::fSunlightDimmer
	static const float* fMinAmbient = (const float*)0x11F947C;		// BSShaderManager::fMinAmbient

	NiDirectionalLight* sun = Tes ? Tes->directionalLight : nullptr;
	if (!sun) {
		SunLight = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f);
		AmbientLight = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
		return;
	}
	const float dimmer = *bHDR ? sun->Dimmer : (std::min)(sun->Dimmer, 1.0f);
	D3DXVECTOR3 ambient(sun->Amb.r * dimmer, sun->Amb.g * dimmer, sun->Amb.b * dimmer);
	if (*fMinAmbient > 0.0f) {
		const float luminance = ambient.x * 0.33f + ambient.y * 0.34f + ambient.z * 0.33f;
		if (luminance > 0.0f) {
			const float boost = luminance <= *fMinAmbient ? *fMinAmbient / luminance : 1.0f;
			ambient = (ambient + D3DXVECTOR3(0.1f, 0.1f, 0.1f)) * boost;
		}
		else ambient = D3DXVECTOR3(*fMinAmbient, *fMinAmbient, *fMinAmbient);
	}
	D3DXVECTOR3 sunColor(sun->Diff.r * dimmer, sun->Diff.g * dimmer, sun->Diff.b * dimmer);
	if (*bHDR && !*bInterior) sunColor *= *fSunlightDimmer;
	SunLight = D3DXVECTOR4(sunColor.x, sunColor.y, sunColor.z, 1.0f);
	AmbientLight = D3DXVECTOR4(ambient.x, ambient.y, ambient.z, 1.0f);
}

// The same as the windows.h max() and min() macros (which this file's max(a, b) and min(a, b) calls always were): a > b ? a : b
// and a < b ? a : b. They differ from std::max and std::min only for NaN, which is why they are spelled out here.
static float LargerOf(float a, float b) { return a > b ? a : b; }
static float SmallerOf(float a, float b) { return a < b ? a : b; }

// Settings readers for this file: the section and key exactly as the settings file names them.
static const char* const ShadowMapsSection = "Shaders.ShadowsExteriors.ShadowMaps";
static int ShadowSettingI(const char* section, const char* key) { return TheSettingManager->GetSettingI(section, key); }
static float ShadowSettingF(const char* section, const char* key) { return TheSettingManager->GetSettingF(section, key); }

// Every key of one forms section, in the order the settings were always read.
static void ReadShadowForms(ShadowsExteriorEffect::FormsStruct& forms, const char* section) {
	forms.AlphaEnabled = ShadowSettingI(section, "AlphaEnabled");
	forms.Activators = ShadowSettingI(section, "Activators");
	forms.Actors = ShadowSettingI(section, "Actors");
	forms.Apparatus = ShadowSettingI(section, "Apparatus");
	forms.Books = ShadowSettingI(section, "Books");
	forms.Containers = ShadowSettingI(section, "Containers");
	forms.Doors = ShadowSettingI(section, "Doors");
	forms.Furniture = ShadowSettingI(section, "Furniture");
	forms.Misc = ShadowSettingI(section, "Misc");
	forms.Statics = ShadowSettingI(section, "Statics");
	forms.Terrain = ShadowSettingI(section, "Terrain");
	forms.Trees = ShadowSettingI(section, "Trees");
	forms.Lod = ShadowSettingI(section, "Lod");
	forms.MinRadius = ShadowSettingF(section, "MinRadius");
	forms.OrigMinRadius = ShadowSettingF(section, "MinRadius");
}

// The built-in forms of a quality preset (the custom quality reads its forms from settings instead).
static void PresetShadowForms(ShadowsExteriorEffect::FormsStruct& forms, int type, int quality) {
	const bool cascadeOrLod = ShadowsExteriorEffect::MapFar <= type && type <= ShadowsExteriorEffect::MapLod;
	const bool isOrtho = type == ShadowsExteriorEffect::MapOrtho;
	forms.AlphaEnabled = !isOrtho;
	forms.Activators = type < ShadowsExteriorEffect::MapLod;
	forms.Actors = type < ShadowsExteriorEffect::MapLod;
	forms.Apparatus = false;
	forms.Books = type < ShadowsExteriorEffect::MapFar;
	forms.Containers = type < ShadowsExteriorEffect::MapLod;
	forms.Doors = !isOrtho;
	forms.Furniture = type < ShadowsExteriorEffect::MapLod;
	forms.Misc = true;
	forms.Statics = true;
	forms.Terrain = true;
	forms.Trees = true;
	forms.Lod = quality >= 2;
	forms.MinRadius = cascadeOrLod ? 10.0f : 1.0f;
	forms.OrigMinRadius = cascadeOrLod ? 10.0f : 1.0f;
}

bool ShadowsExteriorEffect::UpdateSettingsFromQuality(int quality) {
	bool changed = false;
	ShadowMapStruct& maps = Settings.ShadowMaps;

	const D3DFORMAT previousFormat = maps.Format;
	const int previousCascadeResolution = maps.CascadeResolution;
	const bool previousMSAA = maps.MSAA;
	const bool previousPrefilter = maps.Prefilter;

	// Read at every quality level: this removes doubled and ghost shadows, it does not trade quality.
	maps.LodSkipLoaded = ShadowSettingI(ShadowMapsSection, "LodSkipLoaded");
	// UNOFFICIAL 2026-10-28 CasterFadeMin (distant trees' leaves under a fade node at 0.00 were drawn but never cast): every quality.
	maps.CasterFadeMin = std::clamp(ShadowSettingF(ShadowMapsSection, "CasterFadeMin"), 0.0f, 1.0f);
	// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) caster culling (ShadowManager::SunCastMask), every quality.
	maps.CasterCulling = ShadowSettingI(ShadowMapsSection, "CasterCulling") != 0;
	maps.CasterMargin = std::clamp(ShadowSettingF(ShadowMapsSection, "CasterMargin"), 0.0f, 4096.0f);
	maps.CasterDrop = std::clamp(ShadowSettingF(ShadowMapsSection, "CasterDrop"), 0.0f, 65536.0f);
	maps.SkipCoveredCasters = ShadowSettingI(ShadowMapsSection, "SkipCoveredCasters") != 0;

	if (quality < 0 || quality > 3) {
		// Custom: Mode and Format are not read any more; PCF is forced below.
		maps.FormatBits = 0;
		maps.Distance = LargerOf(ShadowSettingF(ShadowMapsSection, "Distance"), 100.0f);
		maps.CascadeLambda = std::clamp(ShadowSettingF(ShadowMapsSection, "CascadeLambda"), 0.0f, 1.0f);
		// UNOFFICIAL 2026-10-15 SmoothFilter (TESR_ShadowBlur.z, read by Includes/Shadow.hlsl ShadowPCF3x3), live
		maps.SmoothFilter = ShadowSettingI(ShadowMapsSection, "SmoothFilter") != 0;
		Constants.ShadowBlur.z = maps.SmoothFilter ? 1.0f : 0.0f;
		maps.LimitFrequency = ShadowSettingI(ShadowMapsSection, "LimitFrequency");

		// The setting's step 0..6 is 1024 .. 4096 per cascade in steps of 512 (UNOFFICIAL 2026-10-15: 6 -> 4096, Max Quality's).
		// The atlas packs 2 x 2 cascades, so it is twice that in each dimension.
		const int cascadeStep = std::clamp(ShadowSettingI(ShadowMapsSection, "CascadeResolution"), 0, 6);
		ULONG cascadeRes = (ULONG)(cascadeStep + 2) * 512;

		// Clamp to what the device can create: InitTexture does not report failure, and a null atlas reads as frozen shadows.
		if (TheRenderManager && TheRenderManager->device) {
			D3DCAPS9 caps;
			if (SUCCEEDED(TheRenderManager->device->GetDeviceCaps(&caps))) {
				const ULONG limit = (ULONG)min(caps.MaxTextureWidth, caps.MaxTextureHeight) / 2;
				while (cascadeRes > limit && cascadeRes > 1024) cascadeRes -= 512;
			}
		}
		maps.CascadeResolution = cascadeRes;

		// Mipmaps and anisotropy stay off: deferred shadows make the derivatives wrong (aras-p, screenspace vs mip mapping).
		maps.MSAA = 0;	// (PCF: below)
		maps.Prefilter = 0;	// (PCF: below)

		// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded): one [Shaders.ShadowsExteriors.Forms] for the four sun cascades, with the
		// per-cascade differences built in (it was FormsNear / FormsMiddle / FormsFar / FormsLod): distant LOD objects only in the Far and
		// Lod cascades (hidden up close anyway), ten times MinRadius there, and the Lod cascade without actors, activators, furniture and
		// misc objects (too small to show that far out).
		const char* forms = "Shaders.ShadowsExteriors.Forms";
		for (int type = 0; type <= MapLod; type++) {
			FormsStruct& f = ShadowMaps[type].Forms;
			const bool farCascade = type >= MapFar;
			const bool lod = type == MapLod;
			f.AlphaEnabled = ShadowSettingI(forms, "AlphaEnabled");
			f.Activators = !lod && ShadowSettingI(forms, "Activators");
			f.Actors = !lod && ShadowSettingI(forms, "Actors");
			f.Apparatus = ShadowSettingI(forms, "Apparatus");
			f.Books = ShadowSettingI(forms, "Books");
			f.Containers = ShadowSettingI(forms, "Containers");
			f.Doors = ShadowSettingI(forms, "Doors");
			f.Furniture = !lod && ShadowSettingI(forms, "Furniture");
			f.Misc = !lod && ShadowSettingI(forms, "Misc");
			f.Statics = ShadowSettingI(forms, "Statics");
			f.Terrain = ShadowSettingI(forms, "Terrain");
			f.Trees = ShadowSettingI(forms, "Trees");
			f.Lod = farCascade && ShadowSettingI(forms, "Lod");
			f.MinRadius = ShadowSettingF(forms, "MinRadius") * (farCascade ? 10.0f : 1.0f);
			f.OrigMinRadius = f.MinRadius;
		}
	}
	else {
		for (int type = 0; type <= MapLod; type++) PresetShadowForms(ShadowMaps[type].Forms, type, quality);

		maps.CascadeLambda = 0.9f;
		maps.LimitFrequency = 1;
		maps.MSAA = 1;
		maps.Prefilter = 1;

		// distance, cascade resolution, MSAA, for quality 0..3
		static const struct { float distance; int resolution; bool msaa; } presets[4] = {
			{ 3000.0f, 1024, false }, { 4000.0f, 1024, true }, { 4500.0f, 2048, true }, { 6000.0f, 2048, true } };
		const auto& preset = presets[quality];
		maps.Distance = preset.distance;
		maps.CascadeResolution = preset.resolution;
		maps.MSAA = preset.msaa;
	}

	// UNOFFICIAL 2026-10-27 (user: "force pcf from now on no other options"): the sun shadow maps are always Mode 3 (PCF), at every
	// Quality and whatever a settings file says. An older file held Mode 2 (EVSM4), and EVSM and VSM have no normal offset, so faces
	// and clothing shaded their own triangles there. Quality 0-3 keep their distance and resolution.
	// UNOFFICIAL 2026-10-14 Mode 3 (PCF): the atlas holds the depth and the lookup compares it (Shadow.hlsl ShadowPCF3x3), so there is
	// nothing to prefilter, and MSAA's resolve would average depths across edges, so both are off.
	maps.Mode = 3;
	maps.FormatBits = 0;
	maps.Prefilter = 0;
	maps.MSAA = 0;
	maps.Format = Formats[maps.Mode][maps.FormatBits];

	// Clear colours of the cascades.
	const float pos = exp(maps.FormatBits ? 40.0f : 5.54f);
	const float neg = -exp(-5.0f);

	for (int type = 0; type <= MapLod; type++) {
		ShadowMapSettings& map = ShadowMaps[type];
		map.CustomClearRequired = false;

		switch (maps.Mode) {
		case 0:
			map.ClearColor = D3DXVECTOR4(1.0f, 1.0f, 0.0f, 1.0f);
			break;
		case 1:
			map.ClearColor = D3DXVECTOR4(pos, neg, 0.0f, 1.0f);
			map.CustomClearRequired = true;
			break;
		case 2:
			map.ClearColor = D3DXVECTOR4(pos, neg, pos * pos, neg * neg);
			map.CustomClearRequired = true;
			break;
		default:
			map.ClearColor = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 1.0f);
		}
	}
	ShadowMaps[MapOrtho].CustomClearRequired = false;

	if (previousFormat != maps.Format) changed = true;
	if (previousCascadeResolution != 0 && previousCascadeResolution != maps.CascadeResolution) changed = true;
	if (previousMSAA != maps.MSAA) changed = true;
	// The prefilter's ping-pong scratch target is only allocated when the prefilter is on, so toggling it needs RecreateTextures.
	if (previousPrefilter != maps.Prefilter) changed = true;

	return changed;
}

void ShadowsExteriorEffect::UpdateSettings() {
	// Section names, as the settings file has them.
	const char* const screenSpaceSection = "Shaders.ShadowsExteriors.ScreenSpace";
	const char* const sunSection = "Shaders.ShadowsExteriors.SunSmoothing";
	const char* const mainSection = "Shaders.ShadowsExteriors.Main";
	const char* const shadowMapsSection = "Shaders.ShadowsExteriors.ShadowMaps";
	const char* const orthoSection = "Shaders.ShadowsExteriors.Ortho";
	const char* const interiorSection = "Shaders.ShadowsInteriors.Main";

	// Screen-space contact shadows, and their NearFade (the start of the fade, pushed out per frame by UpdateConstants).
	Constants.ScreenSpaceData.x = ShadowSettingI(screenSpaceSection, "Enabled") && Enabled;
	Constants.ScreenSpaceData.y = ShadowSettingF(screenSpaceSection, "BlurRadius");
	Constants.ScreenSpaceData.z = ShadowSettingF(screenSpaceSection, "RenderDistance");
	Constants.ScreenSpaceData.w = LargerOf(ShadowSettingF(screenSpaceSection, "Intensity"), 0.0f);
	const float nearFade = (std::max)(ShadowSettingF(screenSpaceSection, "NearFade"), 0.0f);
	ScreenSpaceNearFade = nearFade;
	ScreenSpaceFade = D3DXVECTOR4(nearFade * 0.5f, nearFade, 0.0f, 0.0f);
	Settings.ScreenSpace.Sharp = ShadowSettingI(screenSpaceSection, "Sharp") != 0;	// UNOFFICIAL

	// Sun smoothing.
	SunSmoothingStruct& smoothing = Settings.SunSmoothing;
	smoothing.SmoothSun = ShadowSettingI(sunSection, "SmoothSun");
	smoothing.QuantizeSun = ShadowSettingI(sunSection, "QuantizeSun");
	smoothing.SmoothingFactor = std::clamp(ShadowSettingF(sunSection, "SmoothingFactor"), 0.0f, 1.0f);
	smoothing.YawStepSize = std::clamp(ShadowSettingF(sunSection, "YawStepSize"), 0.0f, 15.0f);
	smoothing.PitchStepSize = std::clamp(ShadowSettingF(sunSection, "PitchStepSize"), 0.0f, 15.0f);
	smoothing.MaxJumpAngle = std::clamp(ShadowSettingF(sunSection, "MaxJumpAngle"), 5.0f, 30.0f);
	smoothing.GlideSeconds = std::clamp(ShadowSettingF(sunSection, "GlideSeconds"), 0.0f, 10.0f);
	smoothing.CrossFade = ShadowSettingI(sunSection, "CrossFade");

	// Exterior sun shadows, general.
	ExteriorsStruct& exteriors = Settings.Exteriors;
	exteriors.Enabled = ShadowSettingI(mainSection, "Enabled");
	exteriors.ForwardShadows = ShadowSettingI(mainSection, "ForwardShadows");

	// The runtime half of the forward/deferred switch. FORWARD_SHADOWS (a macro) decides whether the forward code is compiled in;
	// this decides whether it runs. Both the game shaders and SunShadows.fx read it, so the two halves hand over in the same frame.
	// A macro alone cannot do that: game shaders have no reload path and the effect reload is never queued.
	Constants.ForwardData.x = exteriors.ForwardShadows ? 0.0f : 1.0f;
	// UNOFFICIAL 2026-10-20 SmoothCharacterShadows: 1 = the skin shaders' shadow lookups take the smooth vertex normal (TESR_ShadowForwardData.y).
	exteriors.SmoothCharacterShadows = ShadowSettingI(mainSection, "SmoothCharacterShadows") != 0;
	Constants.ForwardData.y = exteriors.SmoothCharacterShadows ? 1.0f : 0.0f;
	// CharacterShadowSoftness: with SmoothCharacterShadows, the least half-width a sun shadow edge on skin fades over (Shadow.hlsl ShadowMinPenumbra).
	exteriors.CharacterShadowSoftness = std::clamp(ShadowSettingF(mainSection, "CharacterShadowSoftness"), 0.0f, 4.0f);
	Constants.ForwardData.z = exteriors.SmoothCharacterShadows ? exteriors.CharacterShadowSoftness : 0.0f;
	// UNOFFICIAL 2026-10-22 CharacterShadowReach: a cap, world units, on the slack the skin's PCF lookups give the receiver's surface
	// (Shadow.hlsl ShadowSlackCap). 0 = off, the texel slack alone. Live: TESR_ShadowBlur.w.
	exteriors.CharacterShadowReach = std::clamp(ShadowSettingF(mainSection, "CharacterShadowReach"), 0.0f, 8.0f);
	Constants.ShadowBlur.w = exteriors.CharacterShadowReach;

	// UNOFFICIAL 2026-10-21 HandoverBlend: how far inside each sun cascade's edge the cross-fade into the next starts, world units
	// (Shadow.hlsl ShadowCascadeBands). 0 = the old rule, a tenth of the radius. Every Quality level; live.
	Settings.ShadowMaps.HandoverBlend = std::clamp(ShadowSettingF(shadowMapsSection, "HandoverBlend"), 0.0f, 2000.0f);
	Constants.ForwardData.w = Settings.ShadowMaps.HandoverBlend;
	// UNOFFICIAL 2026-10-23 PlayerInNear: in third person the Near cascade grows (GetCascadeViewProj) to hold the player's head while the
	// camera is within this many world units of it (420 = 6 m). 0 = off.
	Settings.ShadowMaps.PlayerInNear = std::clamp(ShadowSettingF(shadowMapsSection, "PlayerInNear"), 0.0f, 1400.0f);
	Settings.ShadowMaps.PlayerInNearLog = ShadowSettingI(shadowMapsSection, "PlayerInNearLog") != 0;	// diagnostic: Near's radius once a second
	// UNOFFICIAL 2026-10-27 FitToDepth (core\SunDepthFit.h): the sun cascades fitted to the depth on screen, its debug view and log.
	// Live; every Quality level. Off (the default): nothing is measured or changed.
	SunDepthFit::Enabled = ShadowSettingI(mainSection, "FitToDepth") != 0;
	SunDepthFit::DebugView = std::clamp(ShadowSettingI(mainSection, "FitToDepthDebug"), 0, 2);
	SunDepthFit::Log = ShadowSettingI(mainSection, "FitToDepthLog") != 0;

	// UNOFFICIAL 2026-10-23 CharacterShadowMap (docs/next/CHARACTER-SHADOW-MAP-DESIGN.md): the player's own small sun shadow map. Compiled
	// into the shaders at startup (CHARACTER_SHADOW_MAP, ShaderRecord::BuildDefines); the rest is live. Every Quality level.
	{
		ShadowMapStruct& maps = Settings.ShadowMaps;
		maps.CharacterMap = ShadowSettingI(shadowMapsSection, "CharacterShadowMap") != 0;
		const int resolution = std::clamp(ShadowSettingI(shadowMapsSection, "CharacterShadowMapResolution"), 256, 4096);
		maps.CharacterMapResolution = (resolution + 128) / 256 * 256;	// a multiple of 256
		maps.CharacterMapFit = std::clamp(ShadowSettingI(shadowMapsSection, "CharacterShadowMapFit"), 0, 1);
		maps.CharacterMapMargin = std::clamp(ShadowSettingF(shadowMapsSection, "CharacterShadowMapMargin"), 4.0f, 200.0f);
		maps.CharacterMapBlend = std::clamp(ShadowSettingF(shadowMapsSection, "CharacterShadowMapBlend"), 1.0f, 100.0f);
		maps.CharacterMapBias = std::clamp(ShadowSettingF(shadowMapsSection, "CharacterShadowMapBias"), 0.0f, 4.0f);
		maps.CharacterMapOthers = ShadowSettingI(shadowMapsSection, "CharacterShadowMapOthers") != 0;
		maps.CharacterMapLog = ShadowSettingI(shadowMapsSection, "CharacterShadowMapLog") != 0;
		maps.CharacterMapDebug = std::clamp(ShadowSettingI(shadowMapsSection, "CharacterShadowMapDebug"), 0, 3);
		if (maps.CharacterMap && !CharacterMapCompiled) {
			static bool hinted = false;
			if (!hinted) {
				hinted = true;
				Logger::Log("UNOFFICIAL CharacterShadowMap was turned on after startup: restart the game once to apply it.");
			}
		}
	}

	exteriors.Quality = std::clamp(ShadowSettingI(mainSection, "Quality"), 0, 4);
	exteriors.Darkness = ShadowSettingF(mainSection, "Darkness");
	exteriors.NightMinDarkness = ShadowSettingF(mainSection, "NightMinDarkness");
	exteriors.SkipWhenFaded = ShadowSettingI(mainSection, "SkipWhenFaded") != 0;	// UNOFFICIAL 2026-10-16
	// UNOFFICIAL 2026-10-21 (sunshadow): the same picture for less CPU time (ShadowManager::RenderShadowMaps)
	exteriors.SunSkipFlat = ShadowSettingI(mainSection, "SunCascadeSkipFlat") != 0;
	exteriors.SunOneWalk = ShadowSettingI(mainSection, "SunCascadeOneWalk") != 0;
	exteriors.SunLeanDraws = ShadowSettingI(mainSection, "SunCascadeLeanDraws") != 0;
	exteriors.OverheadSunFix = ShadowSettingI(mainSection, "OverheadSunFix") != 0;	// UNOFFICIAL 2026-10-21 (sunfix)
#if NVR_TEST_TOOLS
	exteriors.SkipWhenFadedCheck = std::clamp(ShadowSettingI(mainSection, "SkipWhenFadedCheck"), 0, 4);
	exteriors.CascadeStats = ShadowSettingI(mainSection, "SunCascadeStats");	// UNOFFICIAL 2026-10-21
	exteriors.SunVerify = std::clamp(ShadowSettingI(mainSection, "SunCascadeVerify"), 0, 2);	// UNOFFICIAL 2026-10-21
#else
	exteriors.SkipWhenFadedCheck = 0;
	exteriors.CascadeStats = 0;
	exteriors.SunVerify = 0;
#endif
	exteriors.UsePointShadowsDay = ShadowSettingI(mainSection, "UsePointShadowsDay");
	exteriors.UsePointShadowsNight = ShadowSettingI(mainSection, "UsePointShadowsNight");

	// Shadow maps: the quality preset, or the custom settings.
	const bool cascadeSettingsChanged = UpdateSettingsFromQuality(exteriors.Quality);

	// Ortho map, and the Skylighting v2 settings that draw it (only while v2 is on; ShadowManager::RenderShadowMaps).
	bool orthoSettingsChanged = false;
	OrthoStruct& ortho = Settings.OrthoMap;
	const int previousOrthoResolution = ortho.Resolution;
	ortho.Resolution = 128 * pow(2, (std::clamp(ShadowSettingI(orthoSection, "Resolution"), 0, 4)));
	ortho.Distance = LargerOf(ShadowSettingF(orthoSection, "Distance"), 100.0f);
	ortho.LimitFrequency = ShadowSettingI(orthoSection, "LimitFrequency");
	ortho.ProbeDump = ShadowSettingI(orthoSection, "ProbeDump");	// UNOFFICIAL test tools (OrthoProbe.h)
	// UNOFFICIAL 2026-10-17 Skylighting v2
	ortho.SkyCleanMap = ShadowSettingI("Shaders.AmbientOcclusion.Skylighting", "CleanMap") != 0;
	ortho.SkyMapRefresh = std::clamp(ShadowSettingI("Shaders.AmbientOcclusion.Skylighting", "MapRefresh"), 1, 100000);
	ortho.SkyMoveRefresh = std::clamp(ShadowSettingF("Shaders.AmbientOcclusion.Skylighting", "MoveRefresh"), 1.0f, 100000.0f);
	ortho.SkyOwnProjection = ShadowSettingI("Shaders.AmbientOcclusion.Skylighting", "OwnProjection") != 0;
	++ortho.SkySettingsGeneration;

	if (previousOrthoResolution != 0 && previousOrthoResolution != ortho.Resolution)
		orthoSettingsChanged = true;

	ReadShadowForms(ShadowMaps[MapOrtho].Forms, "Shaders.ShadowsExteriors.FormsOrtho");

	// Interior shadows: the point-light (lamp) side.
	InteriorsStruct& interiors = Settings.Interiors;
	interiors.Enabled = ShadowSettingI(interiorSection, "Enabled");
	// UNOFFICIAL: blockers near a lamp fade out of its shadow (PointShadows.fx; the forward shadows read it themselves).
	Constants.PointShadowNear.x = (std::max)(ShadowSettingF(interiorSection, "NearFade"), 0.0f);
	// UNOFFICIAL EdgeFade: each lamp's lit disc fades out over this fraction of its radius (PointShadows.fx EdgeWindow).
	Constants.PointShadowNear.y = (std::min)((std::max)(ShadowSettingF(interiorSection, "EdgeFade"), 0.0f), 0.9f);
	// UNOFFICIAL 2026-10-16 MaskSkipFar (PointShadows.fx Shadow()): lamps skip the pixels beyond their radius (exact: they give 0 there).
	Constants.PointShadowNear.w = ShadowSettingI(interiorSection, "MaskSkipFar") ? 1.0f : 0.0f;
	MaskHalfResOn = ShadowSettingI("Shaders.ShadowsInteriors.Forward", "MaskHalfRes") != 0;	// UNOFFICIAL 2026-10-16

	// UNOFFICIAL 2026-10-16 lamp contact shadows (PointShadows.fx LampContactPS).
	const float contactDistance = std::clamp(ShadowSettingF(interiorSection, "ContactDistance"), 200.0f, 20000.0f);
	LampContact = D3DXVECTOR4(std::clamp(ShadowSettingF(interiorSection, "ContactShadows"), 0.0f, 1.0f),
		std::clamp(ShadowSettingF(interiorSection, "ContactLength"), 4.0f, 200.0f),
		std::clamp(ShadowSettingF(interiorSection, "ContactThickness"), 1.0f, 100.0f), 0.5f);
	LampContactFade = D3DXVECTOR4(25.0f, 1.0f / 20.0f, contactDistance, 1.0f / (0.3f * contactDistance));	// in past the weapon over 25-45 units; out over the last 30%
	// Interleaved gradient noise (Jimenez 2014): frac(52.98 x frac(dot(pixel, (0.06711056, 0.00583715)))), pixel = uv x screen size.
	LampContactNoise = D3DXVECTOR4(0.06711056f * (float)(std::max)(TheRenderManager->width, (UInt32)1),
		0.00583715f * (float)(std::max)(TheRenderManager->height, (UInt32)1), 52.9829189f,
		ShadowSettingI("Shaders.ShadowsInteriors.Forward", "DarkeningBlur") ? 1.0f : 0.0f);

	FormsStruct& interiorForms = interiors.Forms;
	interiorForms.AlphaEnabled = ShadowSettingI(interiorSection, "AlphaEnabled");
	interiorForms.Activators = ShadowSettingI(interiorSection, "Activators");
	interiorForms.Actors = ShadowSettingI(interiorSection, "Actors");
	interiorForms.Apparatus = ShadowSettingI(interiorSection, "Apparatus");
	interiorForms.Books = ShadowSettingI(interiorSection, "Books");
	interiorForms.Containers = ShadowSettingI(interiorSection, "Containers");
	interiorForms.Doors = ShadowSettingI(interiorSection, "Doors");
	interiorForms.Furniture = ShadowSettingI(interiorSection, "Furniture");
	interiorForms.Misc = ShadowSettingI(interiorSection, "Misc");
	interiorForms.Statics = ShadowSettingI(interiorSection, "Statics");
	interiorForms.MinRadius = ShadowSettingF(interiorSection, "MinRadius");

	interiors.Quality = ShadowSettingI(interiorSection, "Quality");
	interiors.LightPoints = (std::max)(0, (std::min)(ShadowSettingI(interiorSection, "LightPoints"), ShadowCubeMapsMax));
	interiors.ExteriorLightPoints = (std::max)(0, (std::min)(ShadowSettingI(interiorSection, "ExteriorLightPoints"), ExteriorBatchedMax));
	// UNOFFICIAL 2026-10-16 (bug hunt): clamped -- 12 R32F cubes at 4096 are about 4.8 GB, and the size sets every cube's memory.
	interiors.ShadowCubeMapSize = std::clamp(ShadowSettingI(interiorSection, "ShadowCubeMapSize"), 64, 2048);
	interiors.Darkness = ShadowSettingF(interiorSection, "Darkness");
	interiors.LightRadiusMult = ShadowSettingF(interiorSection, "LightRadiusMult");
	interiors.DrawDistance = ShadowSettingF(interiorSection, "DrawDistance");
	interiors.UseCastShadowFlag = ShadowSettingF(interiorSection, "UseCastShadowFlag");
	interiors.PlayerShadowFirstPerson = ShadowSettingF(interiorSection, "PlayerShadowFirstPerson");
	interiors.PlayerShadowThirdPerson = ShadowSettingF(interiorSection, "PlayerShadowThirdPerson");
	// UNOFFICIAL (2026-10-07 review): read here, when the settings change, instead of by name every frame.
	interiors.PlayerInsideLamp = ShadowSettingI(interiorSection, "PlayerInsideLamp") != 0;
	interiors.RedrawActorsOnly = ShadowSettingI(interiorSection, "RedrawActorsOnly") != 0;
	interiors.PlayerLampMargin = (std::max)(ShadowSettingF(interiorSection, "PlayerLampMargin"), 1.0f);

	{	// UNOFFICIAL: [Shaders.ShadowsInteriors.Forward], read once per settings change
		InteriorsStruct::ForwardStruct& forward = interiors.Forward;
		const char* section = "Shaders.ShadowsInteriors.Forward";
		forward.Enabled = ShadowSettingI(section, "Enabled") != 0;
		forward.KeepDarkening = ShadowSettingI(section, "KeepDarkening") != 0;
		forward.DarkeningBlur = ShadowSettingI(section, "DarkeningBlur") != 0;
		forward.FirstPerson = ShadowSettingI(section, "FirstPerson") != 0;
		forward.ScopeFix = ShadowSettingI(section, "ScopeFix") != 0;
		forward.LogLamps = ShadowSettingI(section, "LogLamps") != 0;
		forward.LampRanking = ShadowSettingI(section, "LampRanking");
		forward.DebugView = ShadowSettingI(section, "DebugView");
		forward.LampSwitchMargin = (std::max)(0.0f, (std::min)(0.9f, ShadowSettingF(section, "LampSwitchMargin")));
		forward.Strength = ShadowSettingF(section, "Strength");
		forward.Bias = ShadowSettingF(section, "Bias");
		forward.NormalOffset = ShadowSettingF(section, "NormalOffset");
		forward.Softness = ShadowSettingF(section, "Softness");
		forward.FadeIn = ShadowSettingF(section, "FadeIn");
		forward.FadeOut = (std::max)(0.0f, (std::min)(2.0f, ShadowSettingF(section, "FadeOut")));	// UNOFFICIAL 2026-11-01
		forward.ShadowClusterEnabled = ShadowSettingI(section, "ShadowClusterEnabled") != 0;
		forward.ShadowClusterDistance = (std::max)(0.0f, (std::min)(512.0f, ShadowSettingF(section, "ShadowClusterDistance")));
		forward.ShadowClusterInvisibleOnly = ShadowSettingI(section, "ShadowClusterInvisibleOnly") != 0;	// UNOFFICIAL 2026-10-29
		forward.ShadowClusterMinLights = (std::max)(0, (std::min)(64, ShadowSettingI(section, "ShadowClusterMinLights")));
		forward.CameraIndependent = ShadowSettingI(section, "CameraIndependent") != 0;
		forward.KeepLampsInView = ShadowSettingI(section, "KeepLampsInView") != 0;
		forward.FillLightRadius = (std::max)(0.0f, ShadowSettingF(section, "FillLightRadius"));
		forward.FillLightShadowStrength = std::clamp(ShadowSettingF(section, "FillLightShadowStrength"), 0.0f, 1.0f);
		forward.Quality = std::clamp(ShadowSettingI(section, "Quality"), 0, 3);
		forward.ContactHardening = ShadowSettingI(section, "ContactHardening") != 0;
		forward.ContactHardeningStrength = (std::max)(0.0f, ShadowSettingF(section, "ContactHardeningStrength"));
		forward.ContactHardeningMinSoftness = std::clamp(ShadowSettingF(section, "ContactHardeningMinSoftness"), 0.0f, 1.0f);
		forward.InvisibleLightShadowStrength = std::clamp(ShadowSettingF(section, "InvisibleLightShadowStrength"), 0.0f, 1.0f);
		forward.FixtureDistance = std::clamp(ShadowSettingF(section, "FixtureDistance"), 0.0f, 512.0f);
		forward.LampSize = std::clamp(ShadowSettingF(section, "LampSize"), 0.0f, 64.0f);
		forward.MaxSoftness = std::clamp(ShadowSettingF(section, "MaxSoftness"), 1.0f, 32.0f);
		forward.LampGlassShadows = ShadowSettingI(section, "LampGlassShadows") != 0;
		forward.OwnGlassDistance = std::clamp(ShadowSettingF(section, "OwnGlassDistance"), -1.0f, 128.0f);
		forward.LampModelShadows = ShadowSettingI(section, "LampModelShadows") != 0;
		forward.LampModelDistance = std::clamp(ShadowSettingF(section, "LampModelDistance"), 0.0f, 512.0f);
		LampFixtures::OwnLightDistance = std::clamp(ShadowSettingF(section, "LampModelOwnDistance"), -1.0f, 128.0f);
		{	// UNOFFICIAL NoShadowLampModels (2026-10-27): the whole text (GetSettingS cuts a string at 79 characters, as ClothWind reads its lists)
			SettingManager::Configuration::ConfigNode node;
			LampFixtures::SetNoShadowWords(TheSettingManager->Config.FillNode(&node, section, "NoShadowLampModels") ?
				std::string(node.LongValue.empty() ? node.Value : node.LongValue.c_str()) : std::string());
		}
		forward.InvisibleShadowFromLamp = std::clamp(ShadowSettingF(section, "InvisibleShadowFromLamp"), 0.0f, 1024.0f);
		forward.InvisibleLightSize = std::clamp(ShadowSettingF(section, "InvisibleLightSize"), 0.0f, 128.0f);
		forward.EvenTaps = ShadowSettingI(section, "EvenTaps") != 0;	// compiled in at startup (ShaderRecord::BuildDefines)
		forward.MagnifiedSoftness = std::clamp(ShadowSettingF(section, "MagnifiedSoftness"), 0.0f, 8.0f);	// POINT_SHADOW_MAG if above 0 at startup
		forward.TexelFilter = ShadowSettingI(section, "TexelFilter") != 0;	// POINT_SHADOW_TEXEL, compiled in at startup
		forward.AutoNormalOffset = ShadowSettingI(section, "AutoNormalOffset") != 0;	// UNOFFICIAL 2026-10-24 (charfacets)
	}

	Settings.ContactHardening.Enabled = ShadowSettingI("Shaders.ContactHardening.Status", "Enabled") != 0;
	Settings.ContactHardening.SunSize = LargerOf(ShadowSettingF("Shaders.ContactHardening.Main", "SunSize"), 0.0f);
	Settings.ContactHardening.MaxSoftness = LargerOf(SmallerOf(ShadowSettingF("Shaders.ContactHardening.Main", "MaxSoftness"), 32.0f), 1.0f);
	Settings.ContactHardening.Smooth = ShadowSettingI("Shaders.ContactHardening.Main", "Smooth") != 0;
	Settings.ContactHardening.AtNight = ShadowSettingI("Shaders.ContactHardening.Main", "AtNight") != 0;

	// UNOFFICIAL LampReachMask (PointShadows.fx LampReach): z = 1 / LightRadiusMult, so a lamp counts in full out to its game radius;
	// 0 = off (the old falloff). ForwardReachMask (live): 0 = only LampReachMask decides, 1 = the reach mask, 2 (default) = the old
	// falloff without the angle term (z < 0, PointShadows.fx LampAmount). LampReachMask wins.
	const int forwardReachMode = std::clamp(ShadowSettingI(interiorSection, "ForwardReachMask"), 0, 2);
	if (ShadowSettingI(interiorSection, "LampReachMask") || forwardReachMode == 1)
		Constants.PointShadowNear.z = 1.0f / (std::max)(interiors.LightRadiusMult, 1.0f);
	else
		Constants.PointShadowNear.z = forwardReachMode == 2 ? -1.0f : 0.0f;

	TerrainShadows.UpdateSettings();	// UNOFFICIAL 2026-10-19 [Shaders.ShadowsExteriors.TerrainShadows]

	const bool isExterior = TheShaderManager->GameState.isExterior;

	// if the effect was turned off the buffer must be cleared
	if (!Enabled || (isExterior && !exteriors.Enabled) || (!isExterior && !interiors.Enabled)) clearShadowsBuffer();

	// If certain shadow map settings changed, recreate the textures and surfaces.
	if (texturesInitialized)
		RecreateTextures(cascadeSettingsChanged, orthoSettingsChanged, false);
}


/*
* The exterior apply multiplies the scene by the sun-shadow term that SunShadows writes into the red
* channel of TESR_PointShadowBuffer. When SunShadows is not running (its effect file failed to compile --
* seen on a tester's install with an outdated Effects/Includes/Normals.hlsl -- or it was switched off),
* that channel holds only the point-light term, about 0 outdoors, and the apply darkened the WHOLE image
* by the Darkness setting. Skip the apply instead (this also skips the composite apply in the fog pass).
*/
bool ShadowsExteriorEffect::ShouldRender() {
	SunShadowsEffect* sun = TheShaderManager->Effects.SunShadows;
	if (!TheShaderManager->GameState.isExterior || (sun && sun->Enabled && sun->Effect)) return true;
	static bool reported = false;
	if (!reported) {
		Logger::Log("UNOFFICIAL exterior shadow apply skipped: the SunShadows effect is not running (look for a SunShadows.fx.hlsl "
			"error above, usually a missing or outdated file in %sEffects\\Includes). Without it the "
			"shadow pass would darken the whole image by the Darkness setting.", Paths::ShaderRootShort());
		reported = true;
	}
	return false;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the push block of ShadowsExteriors.frag (208 bytes; the shader reads the first 200).
struct ShadowsExteriorPush {
	D3DXVECTOR4 viewColumn[4];		// 0: TESR_ViewTransform's HLSL columns 0-3 (see ExtRender)
	D3DXVECTOR4 cameraPosition;		// 64: TESR_CameraPosition
	D3DXVECTOR4 projectionFar;		// 80: x TESR_ProjectionTransform[0][0], y [1][1], z TESR_CameraData.y (farZ)
	D3DXVECTOR4 shadowFade;			// 96: TESR_ShadowFade
	D3DXVECTOR4 shadowData;			// 112: TESR_ShadowData
	D3DXVECTOR4 waterSettings;		// 128: TESR_WaterSettings
	D3DXVECTOR4 sunFarApply;		// 144: TESR_SunFarApply
	D3DXVECTOR4 sunAmbient;			// 160: TESR_SunAmbient
	D3DXVECTOR4 skyColor;			// 176: TESR_SkyColor
	UInt32		samplerIndex;		// 192: LINEAR, CLAMP (filled in by the binding)
	UInt32		pointSamplerIndex;	// 196: POINT, CLAMP (filled in by the binding)
	UInt32		pad[2];
};

// The constant registered under a name: the memory the D3D9 path uploads (SetCT reads it at each draw). Null if absent.
static const D3DXVECTOR4* ShadowConstant(const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
}

// The texture registered under a sampler name (InitTexture / RegisterTexture). Null if absent.
static IDirect3DBaseTexture9* ShadowTexture(const char* name) {
	auto found = TheTextureManager->TextureNames.find(name);
	return found == TheTextureManager->TextureNames.end() || !found->second ? nullptr : *found->second;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): the shadow apply's one pass (technique Shadow), as ShadowsExteriors.frag. The matrix: the D3D9
// path uploads TESR_ViewTransform with ID3DXEffect::SetMatrix, so the shader's M[i][j] is the matrix's logical element (row i,
// column j). The matrix's memory is row-major, so the push block takes its columns: viewColumn[j] = (M(0,j), M(1,j), M(2,j), M(3,j)).
// TESR_ProjectionTransform's [0][0] and [1][1] are on the diagonal, the same either way. Samplers: LINEAR for the rendered buffer,
// the point shadows, the depth and the normals; POINT for the sun factor (its tex2Dlod sampler in the .fx). Every texture is at one
// mip (InitTexture without mipmaps), so the implicit LOD of tex2D is level 0, as the explicit ones are.
bool ShadowsExteriorEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	const D3DXMATRIX* view = (const D3DXMATRIX*)ShadowConstant("TESR_ViewTransform");
	const D3DXMATRIX* projection = (const D3DXMATRIX*)ShadowConstant("TESR_ProjectionTransform");
	const D3DXVECTOR4* cameraPosition = ShadowConstant("TESR_CameraPosition");
	const D3DXVECTOR4* cameraData = ShadowConstant("TESR_CameraData");
	const D3DXVECTOR4* shadowFade = ShadowConstant("TESR_ShadowFade");
	const D3DXVECTOR4* shadowData = ShadowConstant("TESR_ShadowData");
	const D3DXVECTOR4* waterSettings = ShadowConstant("TESR_WaterSettings");
	const D3DXVECTOR4* sunFarApply = ShadowConstant("TESR_SunFarApply");
	const D3DXVECTOR4* sunAmbient = ShadowConstant("TESR_SunAmbient");
	const D3DXVECTOR4* skyColor = ShadowConstant("TESR_SkyColor");
	if (!view || !projection || !cameraPosition || !cameraData || !shadowFade || !shadowData || !waterSettings || !sunFarApply || !sunAmbient || !skyColor) return false;

	ShadowsExteriorPush push = {};
	for (int j = 0; j < 4; j++) push.viewColumn[j] = D3DXVECTOR4((*view)(0, j), (*view)(1, j), (*view)(2, j), (*view)(3, j));
	push.cameraPosition = *cameraPosition;
	push.projectionFar = D3DXVECTOR4((*projection)(0, 0), (*projection)(1, 1), cameraData->y, 0.0f);
	push.shadowFade = *shadowFade;
	push.shadowData = *shadowData;
	push.waterSettings = *waterSettings;
	push.sunFarApply = *sunFarApply;
	push.sunAmbient = *sunAmbient;
	push.skyColor = *skyColor;

	// The rendered buffer as the chain has it now; the others by their sampler names (one missing: the D3D9 pass, as before).
	IDirect3DBaseTexture9* textures[5] = {
		TheTextureManager->RenderedTexture,
		ShadowTexture("TESR_SunFarBuffer"),
		ShadowTexture("TESR_PointShadowBuffer"),
		ShadowTexture("TESR_DepthBuffer"),
		ShadowTexture("TESR_NormalsBuffer") };
	for (IDirect3DBaseTexture9* texture : textures) if (!texture) return false;

	const UINT TextureCount = 5, SamplerCount = 2;	// samplers: slot 0 LINEAR, slot 1 POINT (their heap indices at 192 and 196)
	if (!extPass.Tried) {
		extPass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[TextureCount + SamplerCount] = {};
		for (UINT i = 0; i < TextureCount; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[TextureCount].Type = NVR_EXT_BIND_SAMPLER; bindings[TextureCount].Slot = 0; bindings[TextureCount].PushOffset = 192;
		bindings[TextureCount + 1].Type = NVR_EXT_BIND_SAMPLER; bindings[TextureCount + 1].Slot = 1; bindings[TextureCount + 1].PushOffset = 196;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("ShadowsExteriors", bindings, TextureCount + SamplerCount, sizeof(push), true);
	}
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	if (!extPass.Pipeline || !linear || !point) return false;
	NVR_EXT_RESOURCE_BINDING resources[TextureCount + SamplerCount] = {};
	for (UINT i = 0; i < TextureCount; i++) {
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = textures[i];
	}
	resources[TextureCount].Slot = 0; resources[TextureCount].Type = NVR_EXT_BIND_SAMPLER; resources[TextureCount].pResource = linear;
	resources[TextureCount + 1].Slot = 1; resources[TextureCount + 1].Type = NVR_EXT_BIND_SAMPLER; resources[TextureCount + 1].pResource = point;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass.Pipeline, resources, TextureCount + SamplerCount, &push, sizeof(push), true);
}

/*
* UNOFFICIAL 2026-10-16 SkipWhenFaded (dawn cost study, work/bench/foundations/dawncost): while the sun's shadows are fully
* faded (SunFaded), whether anything reads the contact shadows' passes. Their red channel is read only through the fade,
* which makes it exactly 1 whatever it holds (the shadow apply and its copies in the fog's composite and AmbientLight, snow),
* or times 1 - fade = 0 (Specular). Their green channel, the lamps' light (PointShadows.fx), which the passes also blur and
* raise to the Intensity power, counts only with lamp shadows on outdoors (ShadowFade.z): without them the apply's
* 1 + green comes out at 1 whatever green is (Darkness 0 or more). Effects that read the buffer without the fade: blood
* flow, wet world, underwater, the debug view.
*/
bool ShadowsExteriorEffect::FadedContactUnread() {
#if NVR_TEST_TOOLS
	if (SunFaded && Settings.Exteriors.SkipWhenFadedCheck == 2) return true;	// DIAGNOSTIC: the readers ignored (positive control)
#endif
	auto draws = [](EffectRecord* effect) { return effect && effect->Enabled && effect->Effect && effect->ShouldRender(); };
	return SunFaded && Constants.ShadowFade.z == 0.0f && Settings.Exteriors.Darkness >= 0.0f &&
		!draws(TheShaderManager->Effects.BloodFlow) && !draws(TheShaderManager->Effects.WetWorld) &&
		!draws(TheShaderManager->Effects.Underwater) && !draws(TheShaderManager->Effects.Debug);
}

/*
* UNOFFICIAL 2026-10-16 SkipWhenFaded: while SunFaded, whether anything reads the sun's cascade maps this frame. The forward
* lookups are off (ForwardData.x; the skin's second lookup runs only where the first found shadow), and Specular multiplies
* its lookup by 1 - fade = 0. Still reading them: the volumetric fog's sun glow (GetFogShadowVisibility, NOT faded, with its
* ShadowStrength above 0, the default: so by default the maps stay), the volumetric light (it stops itself at fade 0.999),
* and with ForwardShadows off the deferred lookup in SunShadows.fx whenever the contact passes run.
*/
bool ShadowsExteriorEffect::FadedCascadesUnread() {
#if NVR_TEST_TOOLS
	if (SunFaded && Settings.Exteriors.SkipWhenFadedCheck == 2) return true;	// DIAGNOSTIC: the readers ignored (positive control)
#endif
	auto draws = [](EffectRecord* effect) { return effect && effect->Enabled && effect->Effect && effect->ShouldRender(); };
	VolumetricFogEffect* fog = TheShaderManager->Effects.VolumetricFog;
	const bool fogReads = draws(fog) && fog->Constants.Scatter.y > 0.0f;
	const bool deferredReads = !Settings.Exteriors.ForwardShadows && !FadedContactUnread();
	return SunFaded && !fogReads && !deferredReads && !draws(TheShaderManager->Effects.VolumetricLight);
}

void ShadowsExteriorEffect::clearShadowsBuffer() {
	// Clear the shadow pass buffer to red, then put the render target back as it was.
	IDirect3DSurface9* previousTarget = nullptr;
	TheRenderManager->device->GetRenderTarget(0, &previousTarget);
	TheRenderManager->device->SetRenderTarget(0, Textures.ShadowPassSurface);
	TheRenderManager->device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 255, 0, 0), 1.0f, 0L);
	TheRenderManager->device->SetRenderTarget(0, previousTarget);
	if (previousTarget) previousTarget->Release();
}


void ShadowsExteriorEffect::RegisterConstants() {
	// Each name is bound to the memory the shaders read it from, in the order they were always bound.
	const auto bind = [](const char* name, D3DXVECTOR4* value) { TheShaderManager->RegisterConstant(name, value); };
	bind("TESR_SmoothedSunDir", &Constants.SmoothedSunDir);
	BendContact::RegisterConstants();	// UNOFFICIAL 2026-11-06 TESR_ContactReproject, TESR_ContactForwardData
	bind("TESR_RtSunReplace", RtSunShadows::ReplaceConstant());	// UNOFFICIAL 2026-10-28 RT sun shadows Mode 1
	bind("TESR_ShadowData", &Constants.Data);
	bind("TESR_ShadowFormatData", &Constants.FormatData);
	bind("TESR_ShadowForwardData", &Constants.ForwardData);
	bind("TESR_ShadowBlur", &Constants.ShadowBlur);
	bind("TESR_ShadowScreenSpaceData", &Constants.ScreenSpaceData);
	bind("TESR_ShadowScreenSpaceFade", &ScreenSpaceFade);	// UNOFFICIAL 2026-10-27 NearFade
	bind("TESR_OrthoData", &Constants.OrthoData);
	bind("TESR_ShadowFade", &Constants.ShadowFade);
	bind("TESR_PointShadowNear", &Constants.PointShadowNear);
	bind("TESR_LampContact", &LampContact);
	bind("TESR_LampContactFade", &LampContactFade);
	bind("TESR_LampContactBias", &LampContactBias);
	bind("TESR_LampContactNoise", &LampContactNoise);
	bind("TESR_ShadowRadius", &Constants.ShadowMapRadius);
	// UNOFFICIAL 2026-10-27 FitToDepth: Effects\SunDepthFit.fx's (core\SunDepthFit.h)
	bind("TESR_SunDepthFitData", &SunDepthFit::Data);
	bind("TESR_SunDepthFitInfo", &SunDepthFit::Info);
	bind("TESR_SunDepthFitSplits", &SunDepthFit::Splits);
	bind("TESR_SunDepthFitOriginal", &SunDepthFit::Original);
	TheShaderManager->RegisterConstant("TESR_ShadowViewProjTransform", (D3DXVECTOR4*)&Constants.ShadowViewProj);

	// The cascades: centre and radius, and the camera-to-light transform, by cascade.
	struct CascadeNames { const char* centre; const char* transform; const char* oldTransform; const char* oldCentre; int type; };
	const CascadeNames cascades[4] = {
		{ "TESR_ShadowNearCenter", "TESR_ShadowCameraToLightTransformNear", "TESR_ShadowOldCameraToLightTransformNear", "TESR_ShadowOldNearCenter", ShadowsExteriorEffect::MapNear },
		{ "TESR_ShadowMiddleCenter", "TESR_ShadowCameraToLightTransformMiddle", "TESR_ShadowOldCameraToLightTransformMiddle", "TESR_ShadowOldMiddleCenter", ShadowsExteriorEffect::MapMiddle },
		{ "TESR_ShadowFarCenter", "TESR_ShadowCameraToLightTransformFar", "TESR_ShadowOldCameraToLightTransformFar", "TESR_ShadowOldFarCenter", ShadowsExteriorEffect::MapFar },
		{ "TESR_ShadowLodCenter", "TESR_ShadowCameraToLightTransformLod", "TESR_ShadowOldCameraToLightTransformLod", "TESR_ShadowOldLodCenter", ShadowsExteriorEffect::MapLod },
	};
	for (const CascadeNames& cascade : cascades) {
		bind(cascade.centre, &ShadowMaps[cascade.type].ShadowMapCascadeCenterRadius);
		TheShaderManager->RegisterConstant(cascade.transform, (D3DXVECTOR4*)&ShadowMaps[cascade.type].ShadowCameraToLight);
	}
	TheShaderManager->RegisterConstant("TESR_ShadowCameraToLightTransformOrtho", (D3DXVECTOR4*)&ShadowMaps[MapOrtho].ShadowCameraToLight);
	for (const CascadeNames& cascade : cascades)
		TheShaderManager->RegisterConstant(cascade.oldTransform, (D3DXVECTOR4*)&Constants.OldCameraToLight[cascade.type]);
	for (const CascadeNames& cascade : cascades)
		bind(cascade.oldCentre, &Constants.OldCenter[cascade.type]);
	bind("TESR_ShadowCrossFade", &Constants.CrossFade);
	Constants.CrossFade = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	bind("TESR_ShadowCubeMapLightPosition", &Constants.ShadowCubeMapLightPosition);
	TheShaderManager->RegisterConstant("TESR_ShadowLightPosition", (D3DXVECTOR4*)&Constants.ShadowLightPosition);
	bind("TESR_ShadowSunLight", &SunLight);	// UNOFFICIAL 2026-10-19 (SunFar)
	bind("TESR_ShadowAmbientLight", &AmbientLight);
	// UNOFFICIAL 2026-10-23 CharacterShadowMap (Includes/Shadow.hlsl, SunShadows.fx.hlsl)
	TheShaderManager->RegisterConstant("TESR_CharShadowCameraToLight", (D3DXVECTOR4*)&Constants.CharacterCameraToLight);
	bind("TESR_CharShadowCenter", &Constants.CharacterCenter);
	bind("TESR_CharShadowData", &Constants.CharacterData);
	D3DXMatrixIdentity(&Constants.CharacterCameraToLight);
	Constants.CharacterCenter = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f);
	Constants.CharacterData = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	TerrainShadows.RegisterConstants();
}

// The cascade atlas, and when the prefilter is on, its blur scratch target (the prefilter's ping-pong pair).
static void MakeCascadeAtlas(ShadowsExteriorEffect& fx, ULONG atlasSize) {
	TheTextureManager->InitTexture("TESR_ShadowAtlas", &fx.ShadowAtlasTexture, &fx.ShadowAtlasSurface, atlasSize, atlasSize, fx.Settings.ShadowMaps.Format, fx.Settings.ShadowMaps.Mipmaps);
	if (fx.Settings.ShadowMaps.Prefilter)
		TheTextureManager->InitTexture("TESR_ShadowAtlasBlur", &fx.ShadowAtlasBlurTexture, &fx.ShadowAtlasBlurSurface, atlasSize, atlasSize, fx.Settings.ShadowMaps.Format, false);
}

// The atlas' depth surface (the MSAA pair when MSAA is on), each cascade's viewport, the frame vertex buffer and the atlas texel size.
static void MakeCascadeSurfaces(ShadowsExteriorEffect& fx, ULONG cascadeSize, ULONG atlasSize) {
	if (!fx.Settings.ShadowMaps.MSAA) {
		TheRenderManager->device->CreateDepthStencilSurface(atlasSize, atlasSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &fx.ShadowAtlasDepthSurface, NULL);
	}
	else {
		TheRenderManager->device->CreateRenderTarget(atlasSize, atlasSize, fx.Settings.ShadowMaps.Format, D3DMULTISAMPLE_4_SAMPLES, 0, 0, &fx.ShadowAtlasSurfaceMSAA, NULL);
		TheRenderManager->device->CreateDepthStencilSurface(atlasSize, atlasSize, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, true, &fx.ShadowAtlasDepthSurface, NULL);
	}

	// Cascade i sits in quadrant (i % 2, i / 2) of the atlas.
	for (int cascade = 0; cascade <= ShadowsExteriorEffect::MapLod; cascade++) {
		ShadowsExteriorEffect::ShadowMapSettings& map = fx.ShadowMaps[cascade];
		const DWORD x = cascade % 2 == 0 ? 0 : cascadeSize;
		const DWORD y = cascade < 2 ? 0 : cascadeSize;
		map.ShadowMapViewPort = { x, y, cascadeSize, cascadeSize, 0.0f, 1.0f };
		map.ShadowMapResolution = (float)cascadeSize;
		map.ShadowMapInverseResolution = 1.0f / (float)cascadeSize;
	}

	TheShaderManager->CreateFrameVertex(atlasSize, atlasSize, &fx.ShadowAtlasVertexBuffer);
	fx.Constants.ShadowBlur.x = 1.0f / (float)atlasSize;
}

void ShadowsExteriorEffect::RegisterTextures() {
	TerrainShadows.RegisterTextures();	// UNOFFICIAL 2026-10-19
	BendContact::RegisterTextures();	// UNOFFICIAL 2026-11-06 TESR_ContactForward
	PuddleMap::RegisterTextures();		// 2026-11-12 Puddles map mode: TESR_PuddleMap (only in map mode)
	const ULONG cascadeSize = Settings.ShadowMaps.CascadeResolution;
	const ULONG cubeSize = Settings.Interiors.ShadowCubeMapSize;
	const ULONG atlasSize = cascadeSize * 2;

	MakeCascadeAtlas(*this, atlasSize);

	// SunSmoothing CrossFade: the copy of the atlas from before a sun step. Only when the setting is on at startup,
	// which is also when the shaders get the fade compiled in (SUN_CROSSFADE). Forward shadows only (the default): the
	// deferred lookup in SunShadows.fx has no fade, so without forward shadows the steps keep gliding.
	if (Settings.SunSmoothing.CrossFade && Settings.Exteriors.ForwardShadows) {
		TheTextureManager->InitTexture("TESR_ShadowAtlasOld", &ShadowAtlasOldTexture, &ShadowAtlasOldSurface, atlasSize, atlasSize, Settings.ShadowMaps.Format, false);
		SunCrossFadeReady = ShadowAtlasOldTexture && ShadowAtlasOldSurface;
		Logger::Log("UNOFFICIAL sun shadow cross-fade %s (%lux%lu copy of the shadow atlas).", SunCrossFadeReady ? "on" : "FAILED, off",
			atlasSize, atlasSize);
	}

	MakeCascadeSurfaces(*this, cascadeSize, atlasSize);

	// ortho texture
	const ULONG orthoRes = Settings.OrthoMap.Resolution;
	TheTextureManager->InitTexture("TESR_OrthoMapBuffer", &ShadowMapOrthoTexture, &ShadowMapOrthoSurface, orthoRes, orthoRes, D3DFMT_R32F);
	TheRenderManager->device->CreateDepthStencilSurface(orthoRes, orthoRes, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowMapOrthoDepthSurface, NULL);
	ShadowMaps[MapOrtho].ShadowMapViewPort = { 0, 0, orthoRes, orthoRes, 0.0f, 1.0f };
	ShadowMaps[MapOrtho].ShadowMapResolution = (float)orthoRes;
	ShadowMaps[MapOrtho].ShadowMapInverseResolution = 1.0f / (float)orthoRes;

	// the spot light maps, one per light. UNOFFICIAL 2026-11-04 (user: "make the flashlight shadows softer, and higher quality"): their own
	// size, [Shaders.Flashlight.Deferred] ShadowMapSize (restart; missing key: 2048), not the lamp cube faces' (ShadowCubeMapSize: 256 for
	// the user, a blocky flashlight shadow)
	SettingManager::Configuration::ConfigNode spotNode;
	const int spotWanted = TheSettingManager->Config.FillNode(&spotNode, "Shaders.Flashlight.Deferred", "ShadowMapSize") ? spotNode.IntValue : 2048;
	SpotMapSize = spotWanted >= 4096 ? 4096 : spotWanted >= 2048 ? 2048 : spotWanted >= 1024 ? 1024 : 512;
	for (int light = 0; light < SpotLightsMax; light++) {
		const std::string name = "TESR_ShadowSpotlightBuffer" + std::to_string(light);
		TheTextureManager->InitTexture(name.c_str(), &Textures.ShadowSpotlightTexture[light], &Textures.ShadowSpotlightSurface[light], SpotMapSize, SpotMapSize, D3DFMT_R32F);
	}
	Logger::Log("UNOFFICIAL flashlight shadow map: %u x %u", SpotMapSize, SpotMapSize);

	// Point-light cube maps: the screen-space passes' 12 here, the rest on first use (EnsureCubeMap).
	CubeMapSizeCreated = cubeSize;
	{	// UNOFFICIAL 2026-11-01 NearCubeSize (ShadowsExterior.h CubeSizeFor): missing key = 0, off
		SettingManager::Configuration::ConfigNode nearNode;
		const int nearSize = TheSettingManager->Config.FillNode(&nearNode, "Shaders.ShadowsInteriors.Atlas", "NearCubeSize") ? nearNode.IntValue : 0;
		NearCubeSize = nearSize >= 2048 ? 2048 : nearSize >= 1024 ? 1024 : 0;
		if (NearCubeSize <= cubeSize) NearCubeSize = 0;
		if (NearCubeSize) Logger::Log("UNOFFICIAL NearCubeSize: lamp shadow cube slots 0-%d are %u x %u (the rest %lu x %lu)", NearCubeSlots - 1, NearCubeSize, NearCubeSize, cubeSize, cubeSize);
	}
	CubeGeneration++;
	for (int slot = 0; slot < ShadowCubeMapsMax; slot++) {
		Textures.ShadowCubeMapTexture[slot] = nullptr;
		Textures.ShadowCubeMapStaticTexture[slot] = nullptr;
		for (int face = 0; face < 6; face++) Textures.ShadowCubeMapSurface[slot][face] = Textures.ShadowCubeMapStaticSurface[slot][face] = nullptr;
		if (slot >= ShadowCubeMapsScreen) continue;
		// A failed create leaves the slot empty (EnsureCubeMap does the same); the shadow passes skip it.
		if (FAILED(TheRenderManager->device->CreateCubeTexture(CubeSizeFor(slot), 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT,
			&Textures.ShadowCubeMapTexture[slot], NULL)) || !Textures.ShadowCubeMapTexture[slot]) {
			Textures.ShadowCubeMapTexture[slot] = nullptr;
			Logger::Log("UNOFFICIAL point shadows: could not create the cube map for slot %d (%d x %d; video memory?)", slot, (int)cubeSize, (int)cubeSize);
		}
		else for (int face = 0; face < 6; face++) {
			Textures.ShadowCubeMapTexture[slot]->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &Textures.ShadowCubeMapSurface[slot][face]);
		}
		const std::string name = "TESR_ShadowCubeMapBuffer" + std::to_string(slot);
		TheTextureManager->RegisterTexture(name.c_str(), (IDirect3DBaseTexture9**)&Textures.ShadowCubeMapTexture[slot]);
	}
	// The stencil surface for drawing cube maps: as large as the largest cube (a smaller cube's faces draw into its top-left part).
	const UINT cubeDepthSize = (std::max)((std::max)((UINT)cubeSize, NearCubeSize), SpotMapSize);	// (2026-11-04: and the flashlight's map)
	// UNOFFICIAL 2026-11-04 not discardable: after the flashlight's map (drawn last) it keeps that map's depth for the GPU grass strands drawn
	// into it later in the frame (GpuGrass.cpp, depth-tested against it)
	TheRenderManager->device->CreateDepthStencilSurface(cubeDepthSize, cubeDepthSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, false, &Textures.ShadowCubeMapDepthSurface, NULL);

	// UNOFFICIAL 2026-10-23 CharacterShadowMap: the shaders get the lookup only when the setting is on at startup (CHARACTER_SHADOW_MAP in
	// ShaderRecord::BuildDefines, read from the same setting). The texture is made now so every shader binds TESR_CharShadowMap from its first draw.
	CharacterMapCompiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap") != 0;
	if (CharacterMapCompiled) {
		EnsureCharacterMap();
		Logger::Log("UNOFFICIAL CharacterShadowMap: compiled into the skin and object shaders and the sun pass (%s, %u x %u)",
			CharacterMapTexture ? "map made" : "the map could NOT be made", CharacterMapSizeCreated, CharacterMapSizeCreated);
	}

	// The shadow pass buffer, and the lamp mask at half resolution.
	const UINT screenWidth = TheRenderManager->width, screenHeight = TheRenderManager->height;
	TheTextureManager->InitTexture("TESR_PointShadowBuffer", &Textures.ShadowPassTexture, &Textures.ShadowPassSurface, screenWidth, screenHeight, D3DFMT_G16R16);
	TheTextureManager->InitTexture("NVR_LampMaskHalf", &MaskHalfTexture, &MaskHalfSurface, (screenWidth + 1) / 2, (screenHeight + 1) / 2, D3DFMT_G16R16F);	// UNOFFICIAL MaskHalfRes

	texturesInitialized = true;
}


/*
 * Recreate specific shadow maps, to be used after specific settings change.
 */
bool ShadowsExteriorEffect::EnsureCubeMap(int slot) {
	if (slot < 0 || slot >= ShadowCubeMapsMax || !CubeMapSizeCreated) return false;
	if (Textures.ShadowCubeMapTexture[slot]) return true;
	if (FAILED(TheRenderManager->device->CreateCubeTexture(CubeSizeFor(slot), 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT,
		&Textures.ShadowCubeMapTexture[slot], NULL)) || !Textures.ShadowCubeMapTexture[slot]) {
		Textures.ShadowCubeMapTexture[slot] = nullptr;
		static bool warned = false;
		if (!warned) { warned = true; Logger::Log("UNOFFICIAL point shadows: could not create the cube map for slot %d (video memory?); lamps past it get no shadow.", slot); }
		return false;
	}
	for (int j = 0; j < 6; j++) Textures.ShadowCubeMapTexture[slot]->GetCubeMapSurface((D3DCUBEMAP_FACES)j, 0, &Textures.ShadowCubeMapSurface[slot][j]);
	CubeGeneration++;
	Logger::Log("UNOFFICIAL point shadows: cube map for slot %d created (%u x %u)", slot, CubeSizeFor(slot), CubeSizeFor(slot));
	return true;
}

// UNOFFICIAL RedrawActorsOnly: a slot's static cube map (only what never moves), made the first time a lamp in that slot
// has people in reach. Same size and format as the slot's cube map, so a face copies over with StretchRect.
bool ShadowsExteriorEffect::EnsureStaticCubeMap(int slot) {
	if (slot < 0 || slot >= ShadowCubeMapsMax || !CubeMapSizeCreated) return false;
	if (Textures.ShadowCubeMapStaticTexture[slot]) return true;
	if (FAILED(TheRenderManager->device->CreateCubeTexture(CubeSizeFor(slot), 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT,
		&Textures.ShadowCubeMapStaticTexture[slot], NULL)) || !Textures.ShadowCubeMapStaticTexture[slot]) {
		Textures.ShadowCubeMapStaticTexture[slot] = nullptr;
		static bool warned = false;
		if (!warned) { warned = true; Logger::Log("UNOFFICIAL RedrawActorsOnly: could not create a static cube map for slot %d (video memory?); that lamp is redrawn whole.", slot); }
		return false;
	}
	for (int j = 0; j < 6; j++) Textures.ShadowCubeMapStaticTexture[slot]->GetCubeMapSurface((D3DCUBEMAP_FACES)j, 0, &Textures.ShadowCubeMapStaticSurface[slot][j]);
	CubeGeneration++;
	Logger::Log("UNOFFICIAL RedrawActorsOnly: static cube map for slot %d created (%u x %u)", slot, CubeSizeFor(slot), CubeSizeFor(slot));
	return true;
}

// Releases a COM object held by pointer and nulls the pointer (a null pointer is left alone).
template <typename Object>
static void ReleaseAndClear(Object*& object) {
	if (object) {
		object->Release();
		object = nullptr;
	}
}

void ShadowsExteriorEffect::RecreateTextures(bool cascades, bool ortho, bool cubemaps) {
	(void)cubemaps;	// the cube maps are not remade here
	if (cascades) {
		// UNOFFICIAL 2026-10-16 SkipWhenFaded: no skip until the new atlas holds a full draw. (Only here: this function also
		// runs, without remaking anything, on every settings change.)
		TheShadowManager->SunAtlasDrawn = false;

		ReleaseAndClear(ShadowAtlasSurface);
		ReleaseAndClear(ShadowAtlasSurfaceMSAA);
		ReleaseAndClear(ShadowAtlasTexture);
		ReleaseAndClear(ShadowAtlasBlurSurface);
		ReleaseAndClear(ShadowAtlasBlurTexture);
		ReleaseAndClear(ShadowAtlasDepthSurface);
		ReleaseAndClear(ShadowAtlasVertexBuffer);

		const ULONG cascadeSize = Settings.ShadowMaps.CascadeResolution;
		const ULONG atlasSize = cascadeSize * 2;

		MakeCascadeAtlas(*this, atlasSize);

		// The cross-fade copy follows the atlas' size and format; a fade in progress ends (its copy is gone).
		if (SunCrossFadeReady) {
			ReleaseAndClear(ShadowAtlasOldSurface);
			ReleaseAndClear(ShadowAtlasOldTexture);
			TheTextureManager->InitTexture("TESR_ShadowAtlasOld", &ShadowAtlasOldTexture, &ShadowAtlasOldSurface, atlasSize, atlasSize, Settings.ShadowMaps.Format, false);
			SunCrossFadeReady = ShadowAtlasOldTexture && ShadowAtlasOldSurface;
			Constants.CrossFade.x = 0.0f;
			TheShaderManager->Effects.SunShadows->ClearSampler("TESR_ShadowAtlasOld", 19);
			TheShaderManager->ClearShaderSamplers("TESR_ShadowAtlasOld", 19);
		}

		MakeCascadeSurfaces(*this, cascadeSize, atlasSize);

		TheShaderManager->Effects.SunShadows->ClearSampler("TESR_ShadowAtlas", 16);

		// Game shaders sample the atlas too when forward sun shadows are on, and each holds its own cached pointer to the texture
		// just released above. Clearing only the deferred effect would leave them reading the dead texture, so the shadows would
		// freeze at whatever was last drawn into it instead of failing outright.
		TheShaderManager->ClearShaderSamplers("TESR_ShadowAtlas", 16);
	}

	if (ortho) {
		ReleaseAndClear(ShadowMapOrthoSurface);
		ReleaseAndClear(ShadowMapOrthoTexture);
		ReleaseAndClear(ShadowMapOrthoDepthSurface);	// released here because it is recreated below

		const ULONG orthoRes = Settings.OrthoMap.Resolution;
		TheTextureManager->InitTexture("TESR_OrthoMapBuffer", &ShadowMapOrthoTexture, &ShadowMapOrthoSurface, orthoRes, orthoRes, D3DFMT_R32F);
		TheRenderManager->device->CreateDepthStencilSurface(orthoRes, orthoRes, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowMapOrthoDepthSurface, NULL);
		ShadowMaps[MapOrtho].ShadowMapViewPort = { 0, 0, orthoRes, orthoRes, 0.0f, 1.0f };
		ShadowMaps[MapOrtho].ShadowMapResolution = (float)orthoRes;
		ShadowMaps[MapOrtho].ShadowMapInverseResolution = 1.0f / (float)orthoRes;

		TheShaderManager->Effects.Rain->ClearSampler("TESR_OrthoMapBuffer", 19);
		TheShaderManager->Effects.Snow->ClearSampler("TESR_OrthoMapBuffer", 19);
		TheShaderManager->Effects.SnowAccumulation->ClearSampler("TESR_OrthoMapBuffer", 19);
		TheShaderManager->Effects.WetWorld->ClearSampler("TESR_OrthoMapBuffer", 19);

		// UNOFFICIAL 2026-11-09 (crash log 2026-11-08 14:07, Ortho.Resolution 2 -> 3, access violation on the dxvk-cs thread 12 s later):
		// the game's own shaders (SLS*, PAR*, SKIN* ...) sample the ortho map too, and each holds its own cached pointer to the texture
		// just released above (ShaderRecord::SetCT re-resolves only a null one). Only the four effects above were cleared, so the first
		// ortho resize of a run left every game shader binding a freed texture: DXVK's SetTexture touched freed memory, fine until the
		// memory was reused, then a wild pointer on the command-stream thread. The atlas path above has always done this.
		TheShaderManager->ClearShaderSamplers("TESR_OrthoMapBuffer", 19);
	}

	// Start the shadow manager's frame counter again, and refresh every cascade next frame: the staggered schedule would
	// otherwise leave freshly created cascades uninitialised for several frames.
	TheShadowManager->FrameCounter = 0;
	TheShadowManager->ForceAllCascades = true;
}


// UNOFFICIAL 2026-10-23 CharacterShadowMap: the map at the size the setting asks for; remade when that changes (the shaders' cached
// pointers to the old texture cleared, as RecreateTextures does for the atlas). false: no map (video memory), nothing is drawn or read.
bool ShadowsExteriorEffect::EnsureCharacterMap() {
	const UINT size = (UINT)Settings.ShadowMaps.CharacterMapResolution;
	if (CharacterMapTexture && CharacterMapDepthSurface && CharacterMapSizeCreated == size) return true;
	if (CharacterMapSurface) { CharacterMapSurface->Release(); CharacterMapSurface = nullptr; }
	if (CharacterMapTexture) { CharacterMapTexture->Release(); CharacterMapTexture = nullptr; }
	if (CharacterMapDepthSurface) { CharacterMapDepthSurface->Release(); CharacterMapDepthSurface = nullptr; }
	TheTextureManager->InitTexture("TESR_CharShadowMap", &CharacterMapTexture, &CharacterMapSurface, size, size, D3DFMT_R32F);
	if (!CharacterMapTexture || !CharacterMapSurface) {
		CharacterMapTexture = nullptr; CharacterMapSurface = nullptr;
		Logger::Log("UNOFFICIAL CharacterShadowMap: could not make the %u x %u map (video memory?); off until it can be.", size, size);
		CharacterMapSizeCreated = 0;
		return false;
	}
	if (FAILED(TheRenderManager->device->CreateDepthStencilSurface(size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &CharacterMapDepthSurface, NULL)) || !CharacterMapDepthSurface) {
		CharacterMapDepthSurface = nullptr;
		Logger::Log("UNOFFICIAL CharacterShadowMap: could not make the %u x %u depth surface; off until it can be.", size, size);
		CharacterMapSizeCreated = 0;
		return false;
	}
	if (CharacterMapSizeCreated) {	// a remake: every shader holds a pointer to the texture just released
		if (TheShaderManager->Effects.SunShadows) TheShaderManager->Effects.SunShadows->ClearSampler("TESR_CharShadowMap", 18);
		TheShaderManager->ClearShaderSamplers("TESR_CharShadowMap", 18);
		Logger::Log("UNOFFICIAL CharacterShadowMap: map remade at %u x %u", size, size);
	}
	CharacterMapSizeCreated = size;
	CharacterMap.ShadowMapViewPort = { 0, 0, size, size, 0.0f, 1.0f };
	CharacterMap.ShadowMapResolution = (float)size;
	CharacterMap.ShadowMapInverseResolution = 1.0f / (float)size;
	return true;
}

// UNOFFICIAL 2026-10-23 CharacterShadowMap: the fit (docs/next/CHARACTER-SHADOW-MAP-DESIGN.md, "Map fit"). A sphere round the player --
// Fit 0: the head and upper body, the segment from the Spine2 bone to the head bone plus the margin (hat brims, hair, shoulders);
// Fit 1: the actor node's world bound plus the margin -- seen from the sun as an ortho box 2R across and 2R deep (eye R toward the
// sun from the centre, as GetCascadeViewProj). Built in camera-relative world space like the cascades (the space the casters' world
// matrices and the shaders' GetShadowWorldPos use). Stable: R is rounded UP to 8 world units, so a breathing bound does not change the
// texel size every frame, and the projection is shifted so the world origin lands on a texel corner (GetCascadeViewProj's rounding),
// so a step of the character moves the picture by whole texels and the shadows do not swim. The sun direction is the cascades' quantised
// one, so the map turns only when they do. The camera's zoom (FOV) plays no part: the same texels at any third-person distance.
// UNOFFICIAL 2026-10-31 CharacterShadowMapTarget 1 (root plan A2, off): in first person (the map had nothing to do there) it is fitted round
// the nearest NPC whose head is in front of the camera within CharacterShadowMapNpcDistance, so the faces you talk to get the same sharp
// sun self-shadow. One map, one box: the skin and clothing lookups already read it wherever a pixel is inside its box.
static Character* CharacterMapNpc(float maxDistance) {
	const NiPoint3 eye = WorldSceneGraph->camera->m_worldTransform.pos;
	const D3DXVECTOR3 forward(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z);
	Character* best = nullptr;
	float bestDistance = maxDistance;
	__try {
		UInt8* lists = (UInt8*)0x11E0E80;	// ProcessLists: the high-process actors (as BloodFlow / CapsuleShadows)
		Actor** data = *(Actor***)(lists + 0x08);
		if (!data) return nullptr;
		const UInt32 begin = *(UInt32*)(lists + 0x14), end = *(UInt32*)(lists + 0x24);
		if (end < begin || end - begin > 4096) return nullptr;
		for (UInt32 i = begin; i < end; i++) {
			Actor* actor = data[i];
			if (!actor || (TESObjectREFR*)actor == (TESObjectREFR*)Player || !actor->baseForm || actor->baseForm->formType != TESForm::FormType::kFormType_NPC) continue;
			if ((actor->flags & 0x800) != 0) continue;	// disabled
			Character* npc = (Character*)actor;
			NiNode* node = npc->GetNode();
			if (!node || (node->m_flags & NiAVObject::APP_CULLED) || !npc->ActorSkinInfo || !npc->ActorSkinInfo->HeadNode || !npc->ActorSkinInfo->Spine2Node) continue;
			const NiPoint3& h = npc->ActorSkinInfo->HeadNode->m_worldTransform.pos;
			const D3DXVECTOR3 to(h.x - eye.x, h.y - eye.y, h.z - eye.z);
			const float distance = D3DXVec3Length(&to);
			if (!(distance < bestDistance) || D3DXVec3Dot(&to, &forward) < 0.5f * distance) continue;	// in front (within 60 degrees)
			best = npc;
			bestDistance = distance;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	return best;
}

bool ShadowsExteriorEffect::GetCharacterViewProj(const D3DXVECTOR3& sunDir, D3DXMATRIX& viewProj) {
	CharacterTargetNode = nullptr;
	if (!Player) return false;
	Character* target = (Character*)Player;
	if (!Player->isThirdPerson) {
		if (TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapTarget") != 1) return false;
		target = CharacterMapNpc(max(TheSettingManager->GetSettingF("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapNpcDistance"), 0.0f));
		if (!target) return false;
	}
	NiNode* node = target->GetNode();
	if (!node || (node->m_flags & NiAVObject::APP_CULLED)) return false;
	CharacterTargetNode = node;
	const NiPoint3 cameraPosition = WorldSceneGraph->camera->m_worldTransform.pos;
	D3DXVECTOR3 centre;
	float radius;
	if (Settings.ShadowMaps.CharacterMapFit == 0 && target->ActorSkinInfo && target->ActorSkinInfo->HeadNode && target->ActorSkinInfo->Spine2Node) {
		const NiPoint3& h = target->ActorSkinInfo->HeadNode->m_worldTransform.pos;
		const NiPoint3& t = target->ActorSkinInfo->Spine2Node->m_worldTransform.pos;
		const D3DXVECTOR3 head(h.x, h.y, h.z), torso(t.x, t.y, t.z);
		const D3DXVECTOR3 axis = head - torso;
		centre = torso + axis * 0.5f;
		radius = 0.5f * D3DXVec3Length(&axis) + Settings.ShadowMaps.CharacterMapMargin;
	}
	else {
		NiBound* bound = node->GetWorldBound();
		if (!bound) return false;
		centre = D3DXVECTOR3(bound->Center.x, bound->Center.y, bound->Center.z);
		radius = bound->Radius + Settings.ShadowMaps.CharacterMapMargin;
	}
	if (!std::isfinite(radius) || !std::isfinite(centre.x) || !std::isfinite(centre.y) || !std::isfinite(centre.z) || radius <= 0.0f) return false;
	radius = std::ceil(radius / 8.0f) * 8.0f;
	radius = (std::min)(radius, 2048.0f);

	// camera-relative, like the cascades' frustum corners
	centre -= D3DXVECTOR3(cameraPosition.x, cameraPosition.y, cameraPosition.z);
	D3DXVECTOR3 dir = sunDir;
	D3DXVec3Normalize(&dir, &dir);
	D3DXVECTOR3 up(0.0f, 0.0f, 1.0f);
	if (SunOverhead(dir)) up = D3DXVECTOR3(0.0f, 1.0f, 0.0f);	// as GetCascadeViewProj's OverheadSunFix
	const D3DXVECTOR3 eye = centre + dir * radius;
	D3DXMATRIX view, proj;
	D3DXMatrixLookAtRH(&view, &eye, &centre, &up);
	D3DXMatrixOrthoOffCenterRH(&proj, -radius, radius, -radius, radius, 0.0f, 2.0f * radius);
	viewProj = view * proj;

	// the world origin on a texel corner (GetCascadeViewProj's rounding): the picture moves by whole texels as the character moves
	const float mapSize = CharacterMap.ShadowMapResolution > 0.0f ? CharacterMap.ShadowMapResolution : (float)Settings.ShadowMaps.CharacterMapResolution;
	D3DXVECTOR4 origin(-cameraPosition.x, -cameraPosition.y, -cameraPosition.z, 1.0f);
	D3DXVec4Transform(&origin, &origin, &viewProj);
	const float ox = origin.x * mapSize * 0.5f, oy = origin.y * mapSize * 0.5f;
	proj._41 += (floorf(ox + 0.5f) - ox) * 2.0f / mapSize;
	proj._42 += (floorf(oy + 0.5f) - oy) * 2.0f / mapSize;
	viewProj = view * proj;

	// the map's settings for the draw (as AmbientLight::GridSunViewProj): actors only, alpha test on (hair cards, hat meshes), nothing too small
	ShadowMapSettings& map = CharacterMap;
	map.Forms = {};
	map.Forms.Actors = true;
	map.Forms.AlphaEnabled = true;
	map.Forms.MinRadius = 0.0f;
	map.Forms.OrigMinRadius = 0.0f;
	map.ShadowMapRadius = radius;
	map.ShadowMapNear = 0.0f;
	map.CustomClearRequired = false;
	map.ClearColor = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 1.0f);
	map.ShadowMapCascadeCenterRadius = D3DXVECTOR4(centre.x, centre.y, centre.z, radius);
	map.CameraTranslation = D3DXVECTOR3(cameraPosition.x, cameraPosition.y, cameraPosition.z);
	NiFrustum frustum(-radius, radius, radius, -radius, 0.0f, 2.0f * radius, true);
	TheCameraManager->SetFrustumPlanes(&map.ShadowMapFrustumPlanes, &viewProj, eye, frustum);
	map.ShadowMapFrustumPlanes.SetActivePlaneState(62);

	// the lookups' constants (w: set by the caller once the map holds the draw)
	Constants.CharacterCameraToLight = viewProj;
	Constants.CharacterCenter = D3DXVECTOR4(centre.x, centre.y, centre.z, radius);
	Constants.CharacterData.x = 1.0f / mapSize;
	Constants.CharacterData.y = (std::min)(Settings.ShadowMaps.CharacterMapBlend / radius, 0.9f);
	Constants.CharacterData.z = Settings.ShadowMaps.CharacterMapBias / (2.0f * radius);	// world units -> the map's depth (0..1 over 2R)
	return true;
}

/*
* Produce a smooth sun direction.
*/
/*
* Produce a smooth sun direction.
*/
D3DXVECTOR3 ShadowsExteriorEffect::CalculateSmoothedSunDir() {
	const float yawStep = D3DXToRadian(Settings.SunSmoothing.YawStepSize);		// horizontal rotation
	const float pitchStep = D3DXToRadian(Settings.SunSmoothing.PitchStepSize);	// vertical rotation
	const float factor = Settings.SunSmoothing.SmoothingFactor;					// smoothing strength (0 = no smoothing, 1 = instant)
	const float jumpLimit = D3DXToRadian(Settings.SunSmoothing.MaxJumpAngle);	// a bigger move than this is applied at once

	const bool quantized = Settings.SunSmoothing.QuantizeSun && yawStep > 0.0f && pitchStep > 0.0f;
	const bool smoothed = Settings.SunSmoothing.SmoothSun && factor > 0.0f;

	D3DXVECTOR3 sunDir(TheShaderManager->ShaderConst.SunDir);

	if (quantized) {
		// Snap yaw and pitch to their steps.
		float yaw = atan2f(sunDir.y, sunDir.x);
		float pitch = acosf(std::clamp(sunDir.z, -1.0f, 1.0f));	// UNOFFICIAL 2026-10-22: clamped, a rounding step past 1 gave NaN

		yaw = roundf(yaw / yawStep) * yawStep;
		pitch = roundf(pitch / pitchStep) * pitchStep;

		sunDir.x = sinf(pitch) * cosf(yaw);
		sunDir.y = sinf(pitch) * sinf(yaw);
		sunDir.z = cosf(pitch);
		D3DXVec3Normalize(&sunDir, &sunDir);
	}

	D3DXVECTOR3 smoothedDir(Constants.SmoothedSunDir);

	// UNOFFICIAL: with quantizing on, the sun only moves in steps (15 degrees, about one in-game hour, by default), and each step is
	// bigger than MaxJumpAngle, so the smoothing below never ran. Here each step slides over GlideSeconds of real time, eased at both
	// ends; anything bigger than one step (waiting, sleeping, fast travel, loading) still jumps at once.
	// CrossFade (opt-in, needs a restart): the sun jumps and the shaders fade from the old shadows to the new, so no map turns on screen.
	if (quantized && smoothed && Settings.SunSmoothing.GlideSeconds > 0.0f) {
		const ULONGLONG now = GetTickCount64();
		const bool crossFade = SunCrossFadeReady && Settings.SunSmoothing.CrossFade && Settings.Exteriors.ForwardShadows;
		if (!SunGlideValid) {
			SunGlideFrom = SunGlideTo = sunDir;
			SunGlideValid = true;
			SunCrossFading = false;
		}
		else if (D3DXVec3Dot(&sunDir, &SunGlideTo) < 0.99999f) {
			// A new step, measured from the previous step's target, so a step that arrives mid-glide is still one step.
			const float stepAngle = acosf(std::clamp(D3DXVec3Dot(&sunDir, &SunGlideTo), -1.0f, 1.0f));
			const bool oneStep = stepAngle < 1.5f * LargerOf(yawStep, pitchStep);
			SunCrossFading = oneStep && crossFade && StartSunCrossFade();

			static int logged = 0;
			if (logged < 20 || (TheSettingManager->GetSettingI("Shaders.CloudShadows.Main", "FlickerWatch") && logged < 2000)) {	// (2026-10-25: more while FlickerWatch tests run)
				logged++;
				Logger::Log("UNOFFICIAL sun step %.1f degrees at hour %.2f (to %.3f %.3f %.3f): %s", D3DXToDegree(stepAngle), TheShaderManager->ShaderConst.GameTime.y, sunDir.x, sunDir.y, sunDir.z,
					!oneStep ? "jumps (bigger than one step: waiting, sleeping or loading)" : SunCrossFading ? "cross-fades" : "glides");
			}
			SunGlideFrom = oneStep && !SunCrossFading ? smoothedDir : sunDir;
			SunGlideTo = sunDir;
			SunGlideStart = now;
		}

		const float glide = std::clamp((now - SunGlideStart) / (1000.0f * Settings.SunSmoothing.GlideSeconds), 0.0f, 1.0f);
		const float ease = glide * glide * (3.0f - 2.0f * glide);
		if (SunCrossFading && crossFade && glide < 1.0f) UpdateSunCrossFade(1.0f - ease);
		else {
			SunCrossFading = false;
			Constants.CrossFade.x = 0.0f;
		}
		D3DXVec3Lerp(&smoothedDir, &SunGlideFrom, &SunGlideTo, ease);
		D3DXVec3Normalize(&smoothedDir, &smoothedDir);
		Constants.SmoothedSunDir = D3DXVECTOR4(smoothedDir, 0.0f);
		return smoothedDir;
	}
	SunGlideValid = false;
	SunCrossFading = false;
	Constants.CrossFade.x = 0.0f;

	if (smoothed) {
		// The angle between the smoothed and the new direction, clamped so acos never sees NaN.
		float cosAngle = D3DXVec3Dot(&sunDir, &smoothedDir);
		cosAngle = LargerOf(-1.0f, SmallerOf(1.0f, cosAngle));
		const float angle = acosf(cosAngle);

		if (angle < jumpLimit) D3DXVec3Lerp(&smoothedDir, &smoothedDir, &sunDir, factor);
		else smoothedDir = sunDir;
	}
	else {
		smoothedDir = sunDir;
	}

	Constants.SmoothedSunDir = D3DXVECTOR4(smoothedDir, 0.0f);
	return smoothedDir;
}

/*
* UNOFFICIAL SunSmoothing CrossFade, at a sun step (called before this frame's shadow maps are drawn): keep the atlas
* as the last frame left it -- the shadows of the previous sun direction -- with the matrices and centres its
* cascades were drawn with, and have every cascade redrawn this frame for the new direction.
*/
bool ShadowsExteriorEffect::StartSunCrossFade() {
	HRESULT copied = TheRenderManager->device->StretchRect(ShadowAtlasSurface, NULL, ShadowAtlasOldSurface, NULL, D3DTEXF_NONE);
	if (FAILED(copied)) {
		static bool reported = false;
		if (!reported) Logger::Log("UNOFFICIAL sun shadow cross-fade: copying the shadow atlas failed (%08lx), steps glide instead.", copied);
		reported = true;
		return false;
	}
	for (int i = MapNear; i <= MapLod; i++) {
		SunFadeMatrix[i] = ShadowMaps[i].ShadowCameraToLight;
		SunFadeCenter[i] = ShadowMaps[i].ShadowMapCascadeCenterRadius;
		SunFadeCamera[i] = ShadowMaps[i].CameraTranslation;
	}
	TheShadowManager->ForceAllCascades = true;
	return true;
}

/*
* The kept cascades for this frame: their matrices and centres are relative to the camera position they were drawn
* from, so move them by how far the camera has moved since (as ShadowManager does for cascades it does not redraw).
*/
void ShadowsExteriorEffect::UpdateSunCrossFade(float oldWeight) {
	const NiPoint3& position = WorldSceneGraph->camera->m_worldTransform.pos;
	const D3DXVECTOR3 camera(position.x, position.y, position.z);
	for (int i = MapNear; i <= MapLod; i++) {
		const D3DXVECTOR3 moved = camera - SunFadeCamera[i];
		D3DXMATRIX translation;
		D3DXMatrixTranslation(&translation, moved.x, moved.y, moved.z);
		Constants.OldCameraToLight[i] = translation * SunFadeMatrix[i];
		Constants.OldCenter[i] = D3DXVECTOR4(SunFadeCenter[i].x - moved.x, SunFadeCenter[i].y - moved.y,
			SunFadeCenter[i].z - moved.z, SunFadeCenter[i].w);
	}
	Constants.CrossFade.x = oldWeight;
}


void ShadowsExteriorEffect::GetCascadeDepths() {
	const NiCamera* camera = WorldSceneGraph->camera;

	const float nearClip = camera->Frustum.Near;
	const float farClip = camera->Frustum.Far;
	const float clipSpan = farClip - nearClip;

	// The view range the cascades cover: from 10 units out to the Distance setting, never past the far clip.
	const float minDepth = nearClip + 10.0f;
	const float maxDepth = SmallerOf(nearClip + Settings.ShadowMaps.Distance, farClip);

	const float span = maxDepth - minDepth;
	const float ratio = maxDepth / minDepth;

	const int splitCount = 4;
	float splitDepth[4];	// UNOFFICIAL 2026-10-27 FitToDepth: the same splits as view depths

	// Each split is a blend of the logarithmic and the uniform split of the range (CascadeLambda picks how much of each).
	for (int i = 0; i < splitCount; ++i) {
		const float share = (i + 1) / static_cast<float>(splitCount);
		const float logSplit = minDepth * std::pow(ratio, share);
		const float uniformSplit = minDepth + span * share;
		const float depth = Settings.ShadowMaps.CascadeLambda * (logSplit - uniformSplit) + uniformSplit;
		ShadowMaps[i].ShadowMapRadius = (depth - nearClip) / clipSpan;
		splitDepth[i] = depth;
	}

	// UNOFFICIAL 2026-10-27 FitToDepth ([Shaders.ShadowsExteriors.Main], core\SunDepthFit.h): the splits refitted to the depth on
	// screen (measured a few frames ago), only where no visible depth gets a coarser cascade; the normal ones otherwise. Off: unchanged.
	SunDepthFit::UnfittedRadiusW = ShadowMaps[MapLod].ShadowMapRadius * clipSpan;
	float nearStart = minDepth;
	{
		bool airReaders = false;	// something reads the cascades through the air along the view (sky pixels included)
		if (SunDepthFit::Enabled) {
			auto draws = [](EffectRecord* effect) { return effect && effect->Enabled && effect->Effect && effect->ShouldRender(); };
			airReaders = draws(TheShaderManager->Effects.VolumetricLight) || draws(TheShaderManager->Effects.Sandstorm);
		}
		float fitted[4];
		if (SunDepthFit::Choose(minDepth, maxDepth, Settings.ShadowMaps.CascadeLambda, splitDepth, fitted, nearStart, airReaders))
			for (int i = 0; i < splitCount; ++i) ShadowMaps[i].ShadowMapRadius = (fitted[i] - nearClip) / clipSpan;
		else nearStart = minDepth;
	}

	// The Near cascade's start, then each cascade starts where the previous one ends.
	ShadowMaps[MapNear].ShadowMapNear = nearStart > minDepth ? (nearStart - nearClip) / clipSpan : 10.0f / clipSpan;
	ShadowMaps[MapMiddle].ShadowMapNear = ShadowMaps[MapNear].ShadowMapRadius;
	ShadowMaps[MapFar].ShadowMapNear = ShadowMaps[MapMiddle].ShadowMapRadius;
	ShadowMaps[MapLod].ShadowMapNear = ShadowMaps[MapFar].ShadowMapRadius;

	// Ortho.
	ShadowMaps[MapOrtho].ShadowMapNear = 10.0f / clipSpan;
	ShadowMaps[MapOrtho].ShadowMapRadius = Settings.OrthoMap.Distance / clipSpan;

	// The absolute split of each cascade, for the shaders.
	Constants.ShadowMapRadius.x = ShadowMaps[MapNear].ShadowMapRadius * clipSpan;
	Constants.ShadowMapRadius.y = ShadowMaps[MapMiddle].ShadowMapRadius * clipSpan;
	Constants.ShadowMapRadius.z = ShadowMaps[MapFar].ShadowMapRadius * clipSpan;
	Constants.ShadowMapRadius.w = ShadowMaps[MapLod].ShadowMapRadius * clipSpan;

	// The blur constant starts again every frame (it limits the refresh rate).
	Constants.ShadowBlur.y = 1.0f;
}

// Banker round helper.
// Round to the nearest integer, halves to the even one (banker's rounding).
float BankerRound(float num) {
	const float nearest = round(num);
	if (fabs(nearest - num) == 0.5f) return 2.0 * round(0.5 * num);
	return nearest;
}


// Round a vector using the banker round method.
// Round a vector using the banker round method.
void Vector3Round(D3DXVECTOR3* out, D3DXVECTOR3* in) {
	out->x = BankerRound(in->x);
	out->y = BankerRound(in->y);
	out->z = BankerRound(in->z);
}

void Vector4Round(D3DXVECTOR4* out, D3DXVECTOR4* in) {
	out->x = BankerRound(in->x);
	out->y = BankerRound(in->y);
	out->z = BankerRound(in->z);
	out->w = BankerRound(in->w);
}


// Generate the ViewProj matrix for a particular shadow cascade.
// Inspired by MJP's https://mynameismjp.wordpress.com/2013/09/10/shadow-maps/ article and code example.
// Generate the ViewProj matrix for a particular shadow cascade.
// Inspired by MJP's https://mynameismjp.wordpress.com/2013/09/10/shadow-maps/ article and code example.
D3DXMATRIX ShadowsExteriorEffect::GetCascadeViewProj(ShadowMapSettings* ShadowMap, D3DXVECTOR3* SunDir, float ExtentScale, float MapSize) {
	// The slice of the view this cascade covers: its near and far depth, as fractions of the view's depth range.
	const NiCamera* camera = WorldSceneGraph->camera;
	const NiPoint3 cameraPos = camera->m_worldTransform.pos;
	const float sliceNear = ShadowMap->ShadowMapNear;
	const float sliceFar = ShadowMap->ShadowMapRadius;

	// The eight corners of the view frustum in world space: the unit cube in projective space, unprojected.
	const float ndcNear = TheRenderManager->IsReversedDepth() ? 1.0f : 0.0f;	// (was written '1.0f ? IsReversedDepth() : 0.0f', same value)
	const float ndcFar = 1.0f - ndcNear;
	D3DXVECTOR3 corners[8] = {
		D3DXVECTOR3(-1.0f,  1.0f, ndcNear),	// near plane
		D3DXVECTOR3( 1.0f,  1.0f, ndcNear),
		D3DXVECTOR3( 1.0f, -1.0f, ndcNear),
		D3DXVECTOR3(-1.0f, -1.0f, ndcNear),
		D3DXVECTOR3(-1.0f,  1.0f, ndcFar),	// far plane
		D3DXVECTOR3( 1.0f,  1.0f, ndcFar),
		D3DXVECTOR3( 1.0f, -1.0f, ndcFar),
		D3DXVECTOR3(-1.0f, -1.0f, ndcFar),
	};
	for (auto i = 0; i < 8; ++i) D3DXVec3TransformCoord(&corners[i], &corners[i], &TheRenderManager->InvViewProjMatrix);

	// Cut the slice: each near corner moves along its edge to the slice's near depth, each far corner to the slice's far depth.
	for (auto i = 0; i < 4; ++i) {
		const D3DXVECTOR3 edge = corners[i + 4] - corners[i];
		const D3DXVECTOR3 nearStep = edge * sliceNear;
		const D3DXVECTOR3 farStep = edge * sliceFar;
		corners[i + 4] = corners[i] + farStep;
		corners[i] = corners[i] + nearStep;
	}

	// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) the view slice's four sides, for leaving out casters whose shadow cannot reach
	// what this cascade shades (ShadowManager::SunCastMask): facing inward, pushed out by CasterMargin. Its depth range is the cascade
	// sphere (ShadowMapCascadeCenterRadius), since Shadow.hlsl picks a cascade by distance to its centre, not by depth.
	{
		static const int sides[4][3] = { { 0, 1, 5 }, { 1, 2, 6 }, { 2, 3, 7 }, { 3, 0, 4 } };
		D3DXVECTOR3 middle(0.0f, 0.0f, 0.0f);
		for (auto i = 0; i < 8; ++i) middle += corners[i];
		middle *= 1.0f / 8.0f;
		ShadowMap->CasterPlanesValid = true;
		for (int p = 0; p < 4; p++) {
			D3DXPLANE& plane = ShadowMap->CasterPlanes[p];
			D3DXPlaneFromPoints(&plane, &corners[sides[p][0]], &corners[sides[p][1]], &corners[sides[p][2]]);
			const float length = sqrtf(plane.a * plane.a + plane.b * plane.b + plane.c * plane.c);
			if (!(length > 1e-6f)) { ShadowMap->CasterPlanesValid = false; break; }
			plane /= length;
			if (D3DXPlaneDotCoord(&plane, &middle) < 0.0f) plane = -plane;
			plane.d += Settings.ShadowMaps.CasterMargin;
		}
	}

	// The centre of the slice.
	D3DXVECTOR3 centre(0.0f, 0.0f, 0.0f);
	for (auto i = 0; i < 8; ++i) centre = centre + corners[i];
	centre *= 1.0f / 8.0f;

	// The up direction must stay fixed, so the texels do not swim as the camera turns.
	D3DXVECTOR3 upDir(0.0f, 0.0f, 1.0f);
	// UNOFFICIAL 2026-10-21 (sunfix) OverheadSunFix: a sun within ~2.6 degrees of straight overhead is parallel to (0, 0, 1), so the view's
	// side axes would be zero and the cascade stays empty. World +Y is taken as up instead. It depends on the sun direction alone, so the
	// texel snapping below holds still from frame to frame.
	if (Settings.Exteriors.OverheadSunFix && SunOverhead(*SunDir)) upDir = D3DXVECTOR3(0.0f, 1.0f, 0.0f);

	// The radius of the sphere round the slice's corners.
	float radius = 0.0f;
	for (auto i = 0; i < 8; ++i) {
		const D3DXVECTOR3 toCorner = corners[i] - centre;
		const float distance = D3DXVec3Length(&toCorner);
		radius = LargerOf(radius, distance);
	}
	radius = std::ceil(radius * 16.0f) / 16.0f;

	// A narrower FOV (aiming, zooming) shrinks the view's slice, so the radius grows back by the FOV's ratio to the default.
	const float defaultFov = *(float*)(0x120315C + 4);
	const float currentFov = WorldSceneGraph->cameraFOV;
	const float fovFactor = tan(defaultFov * 0.5f * (3.1416f / 180.0f)) / tan(currentFov * 0.5f * (3.1416f / 180.0f));
	radius *= fovFactor;

	// UNOFFICIAL 2026-10-23 PlayerInNear ([Shaders.ShadowsExteriors.ShadowMaps], world units, live, 0 = off). In third person the Near
	// sphere grows to the smallest one holding both its slice and the player's head (24 units round the head, plus the hand-over band),
	// while the camera is within PlayerInNear of the head. Its texels grow with the sphere; the radius is rounded up to 16 units.
	if (Settings.ShadowMaps.PlayerInNear > 0.0f && ShadowMap == &ShadowMaps[MapNear] && Player && Player->isThirdPerson &&
		Player->ActorSkinInfo && Player->ActorSkinInfo->HeadNode) {
		const NiPoint3& h = Player->ActorSkinInfo->HeadNode->m_worldTransform.pos;
		const D3DXVECTOR3 head(h.x - cameraPos.x, h.y - cameraPos.y, h.z - cameraPos.z);	// camera-relative, as the corners
		const D3DXVECTOR3 toHead = head - centre;
		const float headDistance = D3DXVec3Length(&head);
		const float centreToHead = D3DXVec3Length(&toHead);
		float margin = 24.0f + Settings.ShadowMaps.HandoverBlend;
		if (headDistance <= Settings.ShadowMaps.PlayerInNear && std::isfinite(centreToHead) && centreToHead + margin > radius) {
			float grown = 0.5f * (radius + centreToHead + margin);
			margin += (std::max)(0.0f, 0.1f * grown - Settings.ShadowMaps.HandoverBlend);	// the band is never under a tenth of the radius
			grown = std::ceil(0.5f * (radius + centreToHead + margin) / 16.0f) * 16.0f;
			// the new centre lies on the line to the head, so the old sphere's far side stays inside
			centre += toHead * ((grown - radius) / (std::max)(centreToHead, 0.001f));
			radius = grown;
		}
		static DWORD lastLog = 0;
		if (Settings.ShadowMaps.PlayerInNearLog && GetTickCount() - lastLog > 1000) {
			lastLog = GetTickCount();
			const D3DXVECTOR3 fromCentre = head - centre;
			Logger::Log("PlayerInNear: head %.0f units from the camera, Near radius %.0f (FOV %.1f, default %.1f), head %.0f from its centre",
				headDistance, radius, currentFov, defaultFov, D3DXVec3Length(&fromCentre));
		}
	}

	// The box: the sphere's square across and its depth from the sphere's diameter. ExtentScale widens the square only.
	D3DXVECTOR3 maxExtents(radius, radius, radius);
	D3DXVECTOR3 minExtents = -maxExtents;
	const D3DXVECTOR3 cascadeExtents = maxExtents - minExtents;
	maxExtents.x *= ExtentScale; maxExtents.y *= ExtentScale;	// UNOFFICIAL 2026-10-31 SunLayers (1 = as always; depth range unchanged)
	minExtents.x *= ExtentScale; minExtents.y *= ExtentScale;

	// The shadow frustum's centre: the slice's centre, as it always was (the move away from the camera is commented out in the original).
	const D3DXVECTOR3 frustumCentre = centre;

	ShadowMap->ShadowMapCascadeCenterRadius.x = frustumCentre.x;
	ShadowMap->ShadowMapCascadeCenterRadius.y = frustumCentre.y;
	ShadowMap->ShadowMapCascadeCenterRadius.z = frustumCentre.z;
	ShadowMap->ShadowMapCascadeCenterRadius.w = radius;

	// The smallest bound a shadow caster may have in this cascade.
	ShadowMap->Forms.MinRadius = ShadowMap->Forms.OrigMinRadius * radius * ShadowMap->ShadowMapInverseResolution;

	const float nearPlane = 0.0f;	// shadow casters are pancaked to the near plane in the vertex shader
	const float farPlane = cascadeExtents.z;
	const D3DXVECTOR3 lightPos = frustumCentre + D3DXVECTOR3(*SunDir) * -minExtents.z;

	D3DXMATRIX lightView, lightProj, lightViewProj;
	D3DXMatrixLookAtRH(&lightView, &lightPos, &frustumCentre, &upDir);
	D3DXMatrixOrthoOffCenterRH(&lightProj, minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, nearPlane, farPlane);
	lightViewProj = lightView * lightProj;

	// Texel snapping: project the world origin into the map and move the projection by the fraction that falls
	// between texels, so that a moving camera shifts the shadows by whole texels only.
	const float mapTexels = MapSize > 0.0f ? MapSize : ShadowMap->ShadowMapResolution;	// (UNOFFICIAL SunLayers: the layer's own size)
	D3DXVECTOR4 originInMap(-cameraPos.x, -cameraPos.y, -cameraPos.z, 1.0f);	// camera-relative world space: the camera is the fixed point
	D3DXVec4Transform(&originInMap, &originInMap, &lightViewProj);
	D3DXVec4Scale(&originInMap, &originInMap, mapTexels / 2.0f);
	D3DXVECTOR4 snapped, shift;
	Vector4Round(&snapped, &originInMap);
	D3DXVec4Subtract(&shift, &snapped, &originInMap);
	D3DXVec4Scale(&shift, &shift, 2.0f / mapTexels);

	lightProj._41 = lightProj._41 + shift.x;
	lightProj._42 = lightProj._42 + shift.y;

	lightViewProj = lightView * lightProj;

	const NiFrustum frustum(minExtents.x, maxExtents.x, maxExtents.y, minExtents.y, nearPlane, farPlane, true);
	TheCameraManager->SetFrustumPlanes(&ShadowMap->ShadowMapFrustumPlanes, &lightViewProj, lightPos, frustum);
	ShadowMap->ShadowMapFrustumPlanes.SetActivePlaneState(62);

	// The camera position the cascade was drawn from; the cached map is offset by its movement since.
	ShadowMap->CameraTranslation = D3DXVECTOR3(cameraPos.x, cameraPos.y, cameraPos.z);

	return lightViewProj;
}

