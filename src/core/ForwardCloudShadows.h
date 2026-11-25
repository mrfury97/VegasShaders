#pragma once

// UNOFFICIAL 2026-11-04 ForwardCloudShadows ([Shaders.VolumetricClouds.Main] ForwardShadows, live since 2026-11-16; Shaders/Includes/Shadow.hlsl).
// The volumetric clouds' shadow taken off the sun term inside the surface shaders (GetSunShadow), per MSAA sample, instead of SunFar's factor
// multiplied over the finished picture. That factor is one per pixel, rebuilt from the resolved depth: with the game's MSAA it darkened
// a whole edge pixel by its nearest sample's sun share, and it darkened grass and plants too (user: "get rid of how volumetric clouds
// affects foliage"). Here:
//   - the object, skin and LOD-land pixel shaders look the clouds' shadow map up themselves (TESR_CloudShadow on s8, NVR_CloudSun c169);
//   - the land, whose pixel shader has no free sampler, gets it from its vertex shader (the map on vertex sampler 1, c160-c161);
//   - grass and the speedtree leaves have none of it: they keep their sunlight under a cloud; the object passes of plants (the plant wind's
//     Keywords) get strength 0 with PlantShadows off;
//   - while it runs (Active), SunFar leaves the volumetric clouds out of its factor and the clouds' composite draws no shadow of its own.
// The map is the clouds' own (VolumetricClouds::RenderShadowMap: drawn later in the frame, so last frame's, with the centre and light it
// was drawn with).
// UNOFFICIAL 2026-11-16 fast-restart (docs/next/FAST-RESTART.md): the code is compiled into every outdoor record whatever the settings
// (FORWARD_CLOUD_SHADOWS defaults to 1 in Includes/Shadow.hlsl; ShaderRecord::BuildDefines no longer defines it), and the setting is read
// every frame. Before, it was compiled in only with ForwardShadows AND the clouds on at startup: flipping either changed every surface
// shader's text, and the next start recompiled nearly all of them (a player waited 9 minutes). Off: the shaders get strength 0 (their
// lookup is skipped, the land's value is 1), Active stays false and SunFar draws the clouds' shadow, as before.
#include <d3d9.h>
#include <cmath>
#include <algorithm>
#include <string>

namespace ForwardCloudShadows {
	inline bool VertexFetch = false, Told = false;
	inline const bool Compiled = true;			// (2026-11-16) always in the shaders: the readers below no longer depend on a startup read
	inline int Wanted = -1;						// the setting as last seen (-1 not read yet): a change is logged once
	inline bool PlantShadows = true;			// [Shaders.VolumetricClouds.Main] PlantShadows
	inline bool Active = false;					// this frame: the shaders take the clouds' shadow (SunFar and the composite leave it out)
	// UNOFFICIAL 2026-11-16 fast-restart (testers: grass and bushes glowing under a cloud's shadow): this frame the clouds draw a shadow
	// outdoors and shade plants (PlantShadows). With ForwardShadows ON the grass and SpeedTree leaves take it on their sun term as every
	// surface does (their forward records' SHADOW_CLOUD_IN, DeferredSun for their deferred records: c7.w, FoliageW); before, they never took
	// it. With it OFF SunFar shades them as everything else, as before (2026-11-17: the foliage's own sun term under a cloud left grass and
	// bushes at 3/4 of their sunlit brightness against a third for the ground, brighter than base's off: taken back out of the off path).
	inline bool Foliage = false;
	inline float FoliageStrength = 0.0f;
	inline float FoliageW(bool allowed) { return PlantShadows ? (Foliage && allowed ? FoliageStrength : 0.0f) : -1.0f; }	// DeferredSun's c7.w
	inline float Data[4] = {}, Light[4] = {};	// c169 / c160: map uv under the camera, uv per unit, strength; c161: the light's x / z, y / z
	// (2026-11-16 fast-restart) the records that could not take the lookup (ShaderRecord::LoadShader: refused by the device, or out of temp
	// registers, with it; made again without it, as with ForwardShadows off before): while any exists, ForwardShadows works as off (SunFar
	// keeps the clouds' shadow for every surface, so none goes without).
	inline unsigned Missing = 0;
	inline std::string MissingNames;
	inline void NoteMissing(const char* name) {
		Missing++;
		if (MissingNames.size() < 200) { if (!MissingNames.empty()) MissingNames += ", "; MissingNames += name ? name : "?"; }
	}

	// Once a frame before the world draws (Render.cpp).
	inline void BeginFrame(IDirect3DDevice9* device) {
		Active = false;
		Foliage = false;
		FoliageStrength = 0.0f;
		Data[3] = 0.0f;
		static bool checkedFetch = false;
		if (!checkedFetch) {
			checkedFetch = true;
			IDirect3D9* d3d = nullptr;
			D3DDEVICE_CREATION_PARAMETERS creation = {};
			D3DDISPLAYMODE mode = {};
			if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
				if (SUCCEEDED(device->GetCreationParameters(&creation)) && SUCCEEDED(device->GetDisplayMode(0, &mode)))
					VertexFetch = SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
						D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F));
				d3d->Release();
			}
			Logger::Log("UNOFFICIAL ForwardCloudShadows: compiled in; the land's vertex lookup %s", VertexFetch ? "works" :
				"is not possible (the card cannot read the map in a vertex shader): ForwardShadows stays off, SunFar keeps the clouds' shadow");
		}
		// (2026-11-16 fast-restart) the switch, live: off sends strength 0 to the shaders (Send) and leaves the clouds' shadow to SunFar
		const int wanted = TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "ForwardShadows") != 0 ? 1 : 0;
		if (wanted != Wanted) {
			if (Wanted >= 0 || wanted) Logger::Log("UNOFFICIAL ForwardCloudShadows: [Shaders.VolumetricClouds.Main] ForwardShadows %s (live: %s)", wanted ? "on" : "off",
				wanted ? "the surface shaders take the clouds' shadow while the clouds draw one outdoors; SunFar leaves it out" : "SunFar puts the clouds' shadow over the picture");
			Wanted = wanted;
		}
		PlantShadows = TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "PlantShadows") != 0;	// (2026-11-16: every frame, the foliage's)
		bool active = wanted != 0;
		if (active && Missing) {
			static bool toldMissing = false;
			if (!toldMissing) { toldMissing = true; Logger::Log("[WARNING] UNOFFICIAL ForwardCloudShadows: ForwardShadows works as off this session: %u shader(s) could not take the clouds' lookup "
				"with these settings (%s); SunFar puts the clouds' shadow over the picture, as with it off", Missing, MissingNames.c_str()); }
			active = false;
		}
		VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
		if (!clouds || !clouds->rtMapValid || !clouds->Textures.ShadowTexture || !TheShaderManager->GameState.isExterior) return;
		const float strength = clouds->ShadowStrengthForSunFar();
		if (strength <= 0.0f || clouds->rtMapLight[2] <= 0.0f) return;
		// The map's uv at a camera-relative point P (world units), as VolumetricClouds.fx CloudShadowAt: the point in metres (1/70), along
		// the map's light up to the cloud base, less the map's centre, over its range, + 0.5. Split into this frame's constant part and P's.
		const float range = clouds->rtMapRange, lx = clouds->rtMapLight[0] / clouds->rtMapLight[2], ly = clouds->rtMapLight[1] / clouds->rtMapLight[2];
		const float cx = TheRenderManager->CameraPosition.x / 70.0f, cy = TheRenderManager->CameraPosition.y / 70.0f, cz = TheRenderManager->CameraPosition.z / 70.0f;
		Data[0] = (cx - clouds->rtMapCentre[0] + lx * (clouds->rtMapBase - cz)) / range + 0.5f;
		Data[1] = (cy - clouds->rtMapCentre[1] + ly * (clouds->rtMapBase - cz)) / range + 0.5f;
		Data[2] = 1.0f / (70.0f * range);
		Data[3] = strength;
		Light[0] = lx; Light[1] = ly; Light[2] = Light[3] = 0.0f;
		Foliage = PlantShadows;	// (2026-11-16 fast-restart) the foliage's own, ForwardShadows on or off
		FoliageStrength = Foliage ? strength : 0.0f;
		if (!active || !VertexFetch) return;	// ForwardShadows off (or cannot run): the surfaces get strength 0 (Send), SunFar draws the clouds' shadow
		device->SetTexture(D3DVERTEXTEXTURESAMPLER1, clouds->Textures.ShadowTexture);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		Active = true;
		if (!Told) { Told = true; Logger::Log("UNOFFICIAL ForwardCloudShadows: on (the clouds' shadow in the surface shaders; SunFar leaves it out)"); }
	}

	// Every object / land pass (Render.cpp ForwardShadowsForPass): this draw's strength (0 for a plant with PlantShadows off). Sent every
	// pass: the effects and the game's own constant uploads may have used the registers in between.
	inline bool PassPlant = false;	// the last object pass was a plant's (ForShader keeps its strength 0)
	// Pixel c169 (the object shaders) and the land's vertex c160-c161 (TerrainTemplate.hlsl). The land's were on c240-c241 first: the object
	// and skin vertex shaders keep TESR_InvProjectionTransform in c240-c243 (ObjectTemplate.hlsl SHADOW_INVPROJ_REG), so written on every
	// pass they broke the position those objects' shadow lookups are rebuilt from (user: "the glide happens on the rock"), and written only
	// at the land's bind they were overwritten by the objects drawn between land draws (user: the land's cloud shadow turned off and on as
	// the camera turned). No vertex shader and no DLL upload uses c160-c161.
	// (2026-11-16 fast-restart) off (not Active): every outdoor record reads these now, so strength 0 still goes out wherever it went while on
	// (another upload may have left anything there), but only the two registers the shaders' gates read: c169.w and c160.w (c161 is read
	// only inside the land's branch).
	// foliage: the record is a grass or SpeedTree leaf pixel shader (ShaderRecord::CloudSunFoliage): its strength follows PlantShadows (they
	// are drawn forward only outside the deferred records; with ForwardShadows off SunFar shades them there, as before)
	inline void Send(IDirect3DDevice9* device, bool foliage = false) {
		const float data[4] = { Data[0], Data[1], Data[2], Active && (foliage ? PlantShadows : !(PassPlant && !PlantShadows)) ? Data[3] : 0.0f };
		device->SetPixelShaderConstantF(169, data, 1);
		device->SetVertexShaderConstantF(160, data, 1);
		if (Active) device->SetVertexShaderConstantF(161, Light, 1);
	}
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 22): pixel c169 for a full-screen pass that lights many objects at once
	// (DeferredSun.h Apply): the frame's strength, not the last object pass's (PassPlant, which Send above follows). PassPlant is left as it is.
	inline void SendFrame(IDirect3DDevice9* device) {
		const float data[4] = { Data[0], Data[1], Data[2], Active ? Data[3] : 0.0f };
		device->SetPixelShaderConstantF(169, data, 1);
	}
	inline void ForPass(IDirect3DDevice9* device, bool plant, bool foliage = false) {
		PassPlant = plant;
		Send(device, foliage);
	}
	// ShaderRecord::SetCT, when a shader that reads them is bound (after this pass's ForPass, for the draws that have one): c169 is also the
	// land's lamp-shadow reprojection (c168-c171, PointShadowForward::BindTerrain), and some draws never come through the pass hook.
	inline void ForShader(IDirect3DDevice9* device, bool foliage = false) {
		Send(device, foliage);
	}
}
