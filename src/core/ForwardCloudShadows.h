#pragma once

// UNOFFICIAL 2026-11-04 ForwardCloudShadows ([Shaders.VolumetricClouds.Main] ForwardShadows, restart; Shaders/Includes/Shadow.hlsl). The
// volumetric clouds' shadow taken off the sun term inside the surface shaders (GetSunShadow), per MSAA sample, instead of SunFar's factor
// multiplied over the finished picture. That factor is one per pixel, rebuilt from the resolved depth: with the game's MSAA it darkened
// a whole edge pixel by its nearest sample's sun share, and it darkened grass and plants too (user: "get rid of how volumetric clouds
// affects foliage"). Here:
//   - the object, skin and LOD-land pixel shaders look the clouds' shadow map up themselves (TESR_CloudShadow on s8, NVR_CloudSun c169);
//   - the land, whose pixel shader has no free sampler, gets it from its vertex shader (the map on vertex sampler 1, c160-c161);
//   - grass and the speedtree leaves have none of it: they keep their sunlight under a cloud; the object passes of plants (the plant wind's
//     Keywords) get strength 0 with PlantShadows off;
//   - while it runs (Active), SunFar leaves the volumetric clouds out of its factor and the clouds' composite draws no shadow of its own.
// The map is the clouds' own (VolumetricClouds::RenderShadowMap: drawn later in the frame, so last frame's, with the centre and light it
// was drawn with). Off (default): nothing compiled in, nothing set; SunFar as before.
#include <d3d9.h>
#include <cmath>
#include <algorithm>

namespace ForwardCloudShadows {
	inline bool Read = false, Compiled = false, VertexFetch = false, Told = false;
	inline bool Active = false;					// this frame: the shaders take the clouds' shadow (SunFar and the composite leave it out)
	inline bool PlantShadows = true;			// [Shaders.VolumetricClouds.Main] PlantShadows
	inline float Data[4] = {}, Light[4] = {};	// c169 / c240: map uv under the camera, uv per unit, strength; c241: the light's x / z, y / z

	// FORWARD_CLOUD_SHADOWS (ShaderRecord::BuildDefines), from the setting at startup.
	inline bool CompiledIn() {
		if (!Read) {
			Read = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "ForwardShadows") != 0 &&
				TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Status", "Enabled") != 0;
		}
		return Compiled;
	}

	// Once a frame before the world draws (Render.cpp).
	inline void BeginFrame(IDirect3DDevice9* device) {
		Active = false;
		Data[3] = 0.0f;
		if (!Compiled) return;
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
				"is not possible (the card cannot read the map in a vertex shader): off, SunFar keeps the clouds' shadow");
		}
		PlantShadows = TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "PlantShadows") != 0;
		VolumetricCloudsEffect* clouds = TheShaderManager->Effects.VolumetricClouds;
		if (!VertexFetch || !clouds || !clouds->rtMapValid || !clouds->Textures.ShadowTexture || !TheShaderManager->GameState.isExterior) return;
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
	inline void Send(IDirect3DDevice9* device) {
		const float data[4] = { Data[0], Data[1], Data[2], Active && !(PassPlant && !PlantShadows) ? Data[3] : 0.0f };
		device->SetPixelShaderConstantF(169, data, 1);
		device->SetVertexShaderConstantF(160, data, 1);
		device->SetVertexShaderConstantF(161, Light, 1);
	}
	// UNOFFICIAL 2026-11-16 fd-batchB (deferred gaps audit item 22): pixel c169 for a full-screen pass that lights many objects at once
	// (DeferredSun.h Apply): the frame's strength, not the last object pass's (PassPlant, which Send above follows). PassPlant is left as it is.
	inline void SendFrame(IDirect3DDevice9* device) {
		if (!Compiled) return;
		const float data[4] = { Data[0], Data[1], Data[2], Active ? Data[3] : 0.0f };
		device->SetPixelShaderConstantF(169, data, 1);
	}
	inline void ForPass(IDirect3DDevice9* device, bool plant) {
		if (!Compiled) return;
		PassPlant = plant;
		Send(device);
	}
	// ShaderRecord::SetCT, when a shader that reads them is bound (after this pass's ForPass, for the draws that have one): c169 is also the
	// land's lamp-shadow reprojection (c168-c171, PointShadowForward::BindTerrain), and some draws never come through the pass hook.
	inline void ForShader(IDirect3DDevice9* device) {
		if (!Compiled) return;
		Send(device);
	}
}
