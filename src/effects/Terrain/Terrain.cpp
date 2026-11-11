#include "Terrain.h"
#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-27 (NVR DXVK 1.7-displace): DisplaceTick
#include "../../core/DisplaceShadows.h"	// UNOFFICIAL 2026-10-27 displacement shadows: the land's draws taken, the depth copy's camera
#include "../../core/SkyCoverage.h"	// UNOFFICIAL 2026-11-04
#include "../../core/TemporalBackbone.h"	// UNOFFICIAL 2026-11-09 the dither follows the shared temporal resolve

// Set by SettingManager::Configuration::UpdateOldDefaults (settings version 65) when a saved config already had ParallaxLite
// on, from builds that forced its distance cap every frame; UpdateSettings then writes the suggested distance once.
bool ParallaxLiteDistanceMigrationPending = false;

void TerrainShaders::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_TerrainData", &Constants.Data);
	sm->RegisterConstant("TESR_TerrainExtraData", &Constants.ExtraData);
	sm->RegisterConstant("TESR_TerrainSkyData", &Constants.SkyData);
	sm->RegisterConstant("TESR_TerrainVariationData", &Constants.VariationData);	// UNOFFICIAL 2026-10-19
	sm->RegisterConstant("TESR_TerrainMacroData", &Constants.MacroData);	// 2026-11-04 macro variation
	sm->RegisterConstant("TESR_TerrainVariationExtra", &Constants.VariationExtra);
	sm->RegisterConstant("TESR_LODBlendData", &Constants.LODBlend);	// UNOFFICIAL 2026-10-19 LOD Blending
	sm->RegisterConstant("TESR_TerrainParallaxData", &ParallaxConstants.Data);
	sm->RegisterConstant("TESR_TerrainParallaxExtraData", &ParallaxConstants.ExtraData);
	sm->RegisterConstant("TESR_TerrainParallaxIso", &ParallaxConstants.IsoData);
	sm->RegisterConstant("TESR_TerrainBlendData", &BlendingConstants.Data);
	sm->RegisterConstant("TESR_TerrainBlendExtra", &BlendingConstants.Extra);
	sm->RegisterConstant("TESR_TerrainBlendShape", &BlendingConstants.Shape);
}

// UNOFFICIAL terrain blending (ported from Physically-Based-Reloaded, Balebandro / Alejandro Martinez; the idea is Community
// Shaders' Terrain Blending). The world's depth surface is noted as the scene starts, so the land shaders know the main
// scene from anything with its own depth buffer. Only the pointer is kept, for comparing; the reference goes at once.
void TerrainShaders::BeginWorldScene() {
	DisplaceShadows::BeginWorld(TheRenderManager->device);	// UNOFFICIAL 2026-10-27: the land's draws noted for its shadow draws
	BlendDepthState = 0;
	BlendWorldDepth = nullptr;
	BindsBeforeCopy = 0;
	TerrainShaders* terrain = TheShaderManager->Shaders.Terrain;
	// UNOFFICIAL 2026-10-27: displacement's ObjectBlend (PrepareDisplaceDepth) takes the same copy, with or without blending.
	const bool wanted = terrain && (terrain->BlendingSettings.Enabled || DxvkCompute::DisplaceDepthWanted());
	if (!wanted || !TheTextureManager->TerrainBlendDepthTexture || !TheShaderManager->GameState.isExterior) return;
	IDirect3DSurface9* depth = nullptr;
	TheRenderManager->device->GetDepthStencilSurface(&depth);
	if (!depth) return;
	BlendWorldDepth = depth;
	depth->Release();
	BlendDepthState = 1;
}

void TerrainShaders::EndWorldScene() {
	DisplaceShadows::EndWorld(TheRenderManager->device);
	BlendDepthState = 0;
	BlendWorldDepth = nullptr;
}

// Runs as a land shader binds (ShaderRecord::SetCT), before its constants upload. In the world scene the first bind copies
// the depth drawn so far (one depth copy a frame, the same resolve NVR's depth buffer uses) and every land draw of that
// scene compares against it. Anywhere else (another depth buffer bound, outside the world scene) blending is off.
// UNOFFICIAL 2026-10-27: the copy itself is CopySceneDepth's, shared with displacement's ObjectBlend (PrepareDisplaceDepth); true when
// this draw is in the world scene and the copy holds it.
bool TerrainShaders::CopySceneDepth() {
	if (BlendDepthState == 0 || !BlendWorldDepth) return false;

	IDirect3DSurface9* current = nullptr;
	TheRenderManager->device->GetDepthStencilSurface(&current);
	const bool mainScene = current && current == BlendWorldDepth;
	if (current) current->Release();
	if (!mainScene) return false;

	if (BlendDepthState == 1) {
		static GpuTimer copyTimer("Terrain blend depth copy");	// F10
		GpuProfileScope gpu(copyTimer, TheRenderManager->device);
		TheRenderManager->ResolveDepthBuffer(TheTextureManager->TerrainBlendDepthTexture);
		SkyCoverage::FixBlendDepth(TheRenderManager->device, TheTextureManager->TerrainBlendDepthTexture);	// UNOFFICIAL 2026-11-04 the game's MSAA (core/SkyCoverage.h)
		BlendDepthState = 2;
		DisplaceShadows::NoteDepthCopy();	// UNOFFICIAL 2026-10-27: its camera, for the next frame's displaced land shadows (ObjectBlend)
		// The first few scenes, then every 3600th (about a minute): the blend needs objects drawn before the land.
		static unsigned copies = 0;
		if (copies < 3 || copies % 3600 == 0)
			Logger::Log("Terrain blending / displacement ObjectBlend: depth copied at the first land draw of the world scene, after %u other passes%s", BindsBeforeCopy,
				BindsBeforeCopy ? "" : " (nothing drawn before the land: nothing to blend with)");
		copies++;
	}
	return true;
}

// UNOFFICIAL 2026-10-27 displacement ObjectBlend (core/DxvkCompute.cpp DisplaceSceneDepth): a land pixel shader that carries the copy at
// s6 for its tessellation shader binds. In the world scene its tessellation fades the bumps near what the copy holds; anywhere else
// (the water's reflection, another depth buffer) the copy is not this view's, and the bumps stay as they are. The viewport goes too:
// RenderScale draws the world into the top-left part of the full-size targets, and so of the copy.
void TerrainShaders::PrepareDisplaceDepth() {
	const bool valid = CopySceneDepth();
	float scaleX = 1.0f, scaleY = 1.0f;
	if (valid) {
		D3DVIEWPORT9 port = {};
		if (SUCCEEDED(TheRenderManager->device->GetViewport(&port)) && TheRenderManager->width && TheRenderManager->height) {
			scaleX = std::clamp((float)(port.X + port.Width) / (float)TheRenderManager->width, 0.0f, 1.0f);
			scaleY = std::clamp((float)(port.Y + port.Height) / (float)TheRenderManager->height, 0.0f, 1.0f);
		}
	}
	DxvkCompute::DisplaceSceneDepth(valid, scaleX, scaleY);
}

void TerrainShaders::PrepareBlending() {
	BlendingConstants.Data.x = 0.0f;
	// (BlendDepthState may be set for displacement's ObjectBlend alone: blending itself switched off in the menu.)
	if (!BlendingSettings.Enabled || !CopySceneDepth()) return;
	BlendingConstants.Data.x = 1.0f;
	// UNOFFICIAL 2026-10-13 Smooth: the land's see-through part (its alpha, TerrainTemplate.hlsl) blended over the objects drawn
	// before it, for the opaque pass only (a pass that already blends, e.g. adding a lamp's light, keeps its own blending). Set
	// through the game's render-state cache, so the next draw that wants no blending turns it off again.
	if (BlendingConstants.Extra.w > 0.5f) {
		NiDX9RenderState* state = TheRenderManager->renderState;
		if (!state->GetRenderState(D3DRS_ALPHABLENDENABLE)) {
			state->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE, RenderStateArgs);
			state->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA, RenderStateArgs);
			state->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA, RenderStateArgs);
			state->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD, RenderStateArgs);
		}
	}
}


void TerrainShaders::UpdateSettings() {
	usePBR = TheSettingManager->GetSettingF("Shaders.Terrain.Main", "UsePBR");

	// the four weather groups (the land has no interior group) and the section each comes from
	struct WeatherGroup { const char* section; TerrainSettings* into; };
	const WeatherGroup groups[] = {
		{ "Shaders.Terrain.Main", &Settings.Default },
		{ "Shaders.Terrain.Rain", &Settings.Rain },
		{ "Shaders.Terrain.Night", &Settings.Night },
		{ "Shaders.Terrain.NightRain", &Settings.NightRain },
	};
	for (const WeatherGroup& g : groups) {
		SettingManager* cfg = TheSettingManager;
		g.into->Saturation = cfg->GetSettingF(g.section, "TerrainSaturation");
		g.into->Metallicness = cfg->GetSettingF(g.section, "Metallicness");
		g.into->Roughness = cfg->GetSettingF(g.section, "Roughness");
		g.into->LightScale = cfg->GetSettingF(g.section, "LightingScale");
		g.into->AmbientScale = cfg->GetSettingF(g.section, "AmbientScale");
		g.into->SkylightingScale = cfg->GetSettingF(g.section, "SkylightingScale");
		g.into->SkylightingDirectionality = cfg->GetSettingF(g.section, "SkylightingDirectionality");
	}

	const char* parallax = "Shaders.Terrain.Parallax";
	ParallaxSettings.Enabled = TheSettingManager->GetSettingF(parallax, "Enabled");
	ParallaxSettings.HighQuality = TheSettingManager->GetSettingF(parallax, "HighQuality");
	ParallaxSettings.Shadows = TheSettingManager->GetSettingF(parallax, "Shadows");
	ParallaxSettings.ShadowsAtNight = TheSettingManager->GetSettingI(parallax, "ShadowsAtNight") != 0;
	ParallaxSettings.HeightBlend = TheSettingManager->GetSettingF(parallax, "HeightBlend");
	ParallaxSettings.MaxDistance = TheSettingManager->GetSettingF(parallax, "MaxDistance");
	ParallaxSettings.Height = TheSettingManager->GetSettingF(parallax, "Height");
	ParallaxSettings.ShadowsIntensity = TheSettingManager->GetSettingF(parallax, "ShadowsIntensity");
	// UNOFFICIAL 2026-10-17 IsoHeightTaps (Parallax.hlsl TESR_TerrainParallaxIso): one trilinear tap per parallax height lookup.
	ParallaxSettings.IsoHeightTaps = TheSettingManager->GetSettingI("Shaders.Terrain.Parallax", "IsoHeightTaps") != 0;
	// UNOFFICIAL 2026-10-17 SkipEmptyLayers (Terrain.hlsl): no colour/normal lookups for layers with weight exactly 0.
	ParallaxSettings.SkipEmptyLayers = TheSettingManager->GetSettingI("Shaders.Terrain.Parallax", "SkipEmptyLayers") != 0;

	// [Main.Main.ReducedQuality] ParallaxLite suggests a shorter terrain parallax reach: 1024 units at 1440p, where the user
	// saw no difference from 2048 in a rocky desert view and it saved 0.8 ms on top of the lite search. Scaled with the
	// screen height, since a bump's size on screen is (screen height / distance): 768 at 1080p, 1536 at 4K. It is written
	// into Shaders.Terrain.Parallax MaxDistance ONCE, when ParallaxLite is switched on (menu, preset, or a config that turns
	// it on), and only if MaxDistance is longer; after that MaxDistance is the player's to change and stays as set. (Until
	// 2026-10-12 it was a cap applied every frame, so MaxDistance could not be raised while ParallaxLite was on.)
	// SettingsVersion 65 does the same once for configs that already had ParallaxLite on (SettingManager.cpp).
	{
		static bool seen = false, wasLite = false;
		const bool lite = TheSettingManager->SettingsMain.Main.ParallaxLite;
		const bool switchedOn = lite && (seen ? !wasLite : ParallaxLiteDistanceMigrationPending);
		seen = true;
		wasLite = lite;
		ParallaxLiteDistanceMigrationPending = false;
		if (switchedOn && TheRenderManager && TheRenderManager->height) {
			const int suggested = (int)(1024.0f * TheRenderManager->height / 1440.0f + 0.5f);
			if (ParallaxSettings.MaxDistance > (float)suggested) {
				Logger::Log("UNOFFICIAL ParallaxLite switched on: Shaders.Terrain.Parallax MaxDistance %g -> %d (suggested for %u lines; "
					"change it in the menu if you want it longer, it is not changed again).", ParallaxSettings.MaxDistance, suggested,
					TheRenderManager->height);
				TheSettingManager->SetSetting("Shaders.Terrain.Parallax", "MaxDistance", suggested);
				ParallaxSettings.MaxDistance = (float)suggested;
			}
		}
	}

	LODSettings.NoiseScale = TheSettingManager->GetSettingF("Shaders.Terrain.LOD", "LODNoiseScale");
	LODSettings.NoiseTile = TheSettingManager->GetSettingF("Shaders.Terrain.LOD", "LODNoiseTile");

	// UNOFFICIAL 2026-10-19 Terrain Variation (Includes/TerrainVariation.hlsl, after Conor's port of Community Shaders' Terrain Variation).
	// Enabled and Layers are compile time (TERRAIN_VARIATION, TERRAIN_VARIATION_LAYERS in ShaderRecord::LoadShader); these shape it. A
	// scale of 0 would put the whole world in one lattice cell (a constant offset: plain tiling shifted), so it is floored. (Conor.)
	Constants.VariationData.x = (std::max)(TheSettingManager->GetSettingF("Shaders.Terrain.Variation", "Scale"), 0.01f);
	Constants.VariationData.y = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Variation", "HeightInfluence"), 0.0f, 4.0f);
	Constants.VariationData.z = (std::max)(TheSettingManager->GetSettingF("Shaders.Terrain.Variation", "StartDistance"), 0.0f);
	Constants.VariationData.w = (std::max)(TheSettingManager->GetSettingF("Shaders.Terrain.Variation", "StartFade"), 1.0f);
	Constants.VariationExtra.x = TheSettingManager->GetSettingI("Shaders.Terrain.Variation", "NormalMaps") ? 1.0f : 0.0f;

	// 2026-11-04 macro variation (Includes/TerrainMacro.hlsl): Enabled is compile time (TERRAIN_MACRO); these are live. Strength 0 skips it.
	Constants.MacroData.x = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Macro", "Strength"), 0.0f, 2.0f);	// 2: diagnostic (the pattern as grey)
	Constants.MacroData.y = 1.0f / (std::max)(TheSettingManager->GetSettingF("Shaders.Terrain.Macro", "Size"), 256.0f);
	Constants.MacroData.z = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Macro", "Brightness"), 0.0f, 1.5f);	// stops
	Constants.MacroData.w = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Macro", "Tint"), 0.0f, 1.0f);	// stops

	// UNOFFICIAL 2026-10-19 LOD Blending (after Community Shaders' LOD Blending, GPL-3.0): the far land's colour (its LOD texture) by a
	// gamma and a brightness so it matches the near land where they meet (TerrainLODTemplate, TerrainFadeTemplate), and the near land
	// optionally without its vertex colours (for far land generated without them). Live. Off: the shaders are exactly what they were
	// (the far land's adjustment is skipped, the vertex colours kept as they come).
	const bool lodBlend = TheSettingManager->GetSettingI("Shaders.Terrain.LODBlending", "Enabled") != 0;
	Constants.LODBlend.x = lodBlend ? std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.LODBlending", "Brightness"), 0.01f, 5.0f) : 1.0f;
	Constants.LODBlend.y = lodBlend ? std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.LODBlending", "Gamma"), 0.1f, 3.0f) : 1.0f;
	Constants.LODBlend.z = lodBlend && TheSettingManager->GetSettingI("Shaders.Terrain.LODBlending", "DisableVertexColors") ? 1.0f : 0.0f;
	Constants.LODBlend.w = lodBlend && (Constants.LODBlend.x != 1.0f || Constants.LODBlend.y != 1.0f) ? 1.0f : 0.0f;

	// UNOFFICIAL terrain blending. Turning it on in the menu needs a restart (the land shaders and the depth copy are made
	// at startup); turning it off works at once.
	BlendingSettings.Enabled = TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Enabled") != 0;
	BlendingSettings.Debug = TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Debug") != 0;
	// UNOFFICIAL 2026-10-14 (ported from mrfury97's fork, 0a7cbdc5): capped at 20, deeper bands show too much of what the land
	// covers; the band's width across the ground (GroundWidth), the land's height map at the edge, steep land, minimum pixels.
	BlendingSettings.BlendDistance = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "BlendDistance"), 0.5f, 20.0f);
	BlendingSettings.MaxDistance = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "MaxDistance"), 256.0f, 16384.0f);
	BlendingSettings.GroundWidth = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "GroundWidth"), 0.0f, 100.0f);
	BlendingSettings.HeightBlend = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "HeightBlend"), 0.0f, 2.0f);
	BlendingSettings.SlopeBlend = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "SlopeBlend"), 0.0f, 2.0f);
	BlendingSettings.MinPixels = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "MinPixels"), 0.0f, 8.0f);
	BlendingSettings.ShadowDrop = std::clamp(TheSettingManager->GetSettingF("Shaders.Terrain.Blending", "ShadowDrop"), 0.0f, 4.0f);
	BlendingSettings.DebugView = std::clamp(TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "DebugView"), 0, 4);
	if (BlendingSettings.Debug && BlendingSettings.DebugView == 0) BlendingSettings.DebugView = 1;	// the old switch
	BlendingConstants.Data.y = 1.0f / BlendingSettings.BlendDistance;
	BlendingConstants.Data.z = BlendingSettings.MaxDistance;
	BlendingConstants.Data.w = (float)BlendingSettings.DebugView;
	// UNOFFICIAL 2026-10-17 HeightEdge 1 (community submission): the height-weighted edge, sent as a negative HeightBlend.
	// UNOFFICIAL 2026-10-20 HeightEdge 2 (a community update, TerrainTemplate.hlsl getTerrainBlend / terrainBlendEdge): -(HeightBlend + 10).
	const int heightEdge = std::clamp(TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "HeightEdge"), 0, 2);
	const float shapeY = heightEdge == 2 ? -(BlendingSettings.HeightBlend + 10.0f) : heightEdge == 1 ? -BlendingSettings.HeightBlend : BlendingSettings.HeightBlend;
	BlendingConstants.Shape = D3DXVECTOR4(BlendingSettings.GroundWidth, shapeY, BlendingSettings.SlopeBlend, BlendingSettings.MinPixels);
	// UNOFFICIAL 2026-10-13 (user: blend so it does not need TAA): Smooth (on by default) fades the land by blending; off, the dither.
	BlendingConstants.Extra.w = TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Smooth") ? 1.0f : 0.0f;
}

void TerrainShaders::UpdateConstants() {
	// the stronger of the rain animator and the puddle animator drives the dry-to-wet blend
	const auto& wetWorld = TheShaderManager->Effects.WetWorld->Constants;
	const float rainFactor = max(wetWorld.Data.x, wetWorld.Data.z);

	// UNOFFICIAL 2026-10-27 (NVR DXVK 1.7-displace, core\DxvkCompute.h): the land's tessellation displacement settings, once a frame.
	// Nothing registered (NvrDxvkDisplacement off at load, or no NVR DXVK): returns at once.
	DxvkCompute::DisplaceTick();

	if (!TheShaderManager->GameState.isExterior) return;

	// UNOFFICIAL terrain blending: the dither moves every frame only while TAA runs (it then averages into a smooth fade);
	// without TAA a moving dither would crawl, so it stands still.
	BlendingConstants.Extra.x = 1.0f / (float)TheRenderManager->width;
	BlendingConstants.Extra.y = 1.0f / (float)TheRenderManager->height;
	// UNOFFICIAL 2026-11-09 (roadmap track 1 step 4): with [Main.Main.Quality] SharedNoise (on) the dither follows the shared backbone like
	// every other history-less noise (TemporalBackbone::NoiseIndex: the shared frame index while TAA / FSR / DLSS really resolves this frame,
	// else 0). It moved whenever TAA was switched on, also in frames TAA did not resolve (menus, the overlay, effects off), where it
	// crawled, and it stepped once per UpdateConstants call (two a frame). SharedNoise off: as before.
	if (TemporalBackbone::Shared()) BlendingConstants.Extra.z = (float)(TemporalBackbone::NoiseIndex() % 64);
	else {
		TAAEffect* taa = TheShaderManager->Effects.TAA;
		BlendingConstants.Extra.z = taa && taa->Enabled && taa->Effect ? fmodf(BlendingConstants.Extra.z + 1.0f, 64.0f) : 0.0f;
	}

	// One value blended over day / night (the interior operand is 0: the land has no Interiors group and returns early
	// indoors), then dry to wet by rainFactor.
	auto weather = [&](float TerrainSettings::* member) {
		ShaderManager* sm = TheShaderManager;
		return std::lerp(sm->GetTransitionValue(Settings.Default.*member, Settings.Night.*member, 0.0),
			sm->GetTransitionValue(Settings.Rain.*member, Settings.NightRain.*member, 0.0), rainFactor);
	};

	Constants.ExtraData.x = usePBR;
	Constants.ExtraData.y = weather(&TerrainSettings::Saturation);
	// Hemisphere skylight strength. No separate toggle: 0 disables it.
	Constants.SkyData.x = weather(&TerrainSettings::SkylightingScale);
	// Used only when SKYLIGHTING_MODE is 1; the SH path has no direction to lean.
	Constants.SkyData.y = weather(&TerrainSettings::SkylightingDirectionality);

	Constants.ExtraData.z = LODSettings.NoiseScale;
	Constants.ExtraData.w = LODSettings.NoiseTile;

	if (usePBR) {
		Constants.Data.x = weather(&TerrainSettings::Metallicness);
		Constants.Data.y = weather(&TerrainSettings::Roughness);
	}

	Constants.Data.z = weather(&TerrainSettings::LightScale);
	Constants.Data.w = weather(&TerrainSettings::AmbientScale);

	ParallaxConstants.Data.x = ParallaxSettings.Enabled;
	// UNOFFICIAL 2026-10-16 (night cost study, work/bench/foundations/nightcost): the parallax self-shadows darken only the
	// sun's light, and at full night (dayLight 0: from an hour after sunset to an hour before sunrise) that is the moon's,
	// too weak for them to show: switched off there they changed no pixel beyond two identical frames' noise, and saved
	// 0.9-1.0 ms of land shading at the night test spot (GTX 1070, 1440p). ShadowsAtNight keeps them.
	ParallaxConstants.Data.y = ParallaxSettings.Shadows && (ParallaxSettings.ShadowsAtNight || TheShaderManager->GameState.dayLight > 0.0f);
	ParallaxConstants.Data.z = ParallaxSettings.HeightBlend;
	// .w: 0 = 8 steps, 1 = 16 (HighQuality), 2 = [Main.Main.ReducedQuality] ParallaxLite (Parallax.hlsl), which
	// replaces both.
	ParallaxConstants.Data.w = TheSettingManager->SettingsMain.Main.ParallaxLite ? 2.0f : ParallaxSettings.HighQuality;

	// MaxDistance as set (ParallaxLite's shorter reach is written into the setting once, in UpdateSettings, not forced here).
	ParallaxConstants.ExtraData.x = ParallaxSettings.MaxDistance;
	ParallaxConstants.ExtraData.y = ParallaxSettings.Height;
	ParallaxConstants.ExtraData.z = ParallaxSettings.ShadowsIntensity;
	ParallaxConstants.IsoData.x = ParallaxSettings.IsoHeightTaps ? 1.0f : 0.0f;
	ParallaxConstants.IsoData.y = ParallaxSettings.SkipEmptyLayers ? 1.0f : 0.0f;

	// [Main.Main.ReducedQuality] CheapUnderwaterTerrain: ground below the water surface skips parallax and its shadows
	// (TerrainTemplate.hlsl). .w is the camera-relative height below which terrain counts as under water: the level of
	// the water the player is in or looking at, a little lower so the shoreline itself keeps its parallax. Only while
	// a water plane is loaded nearby (otherwise the cell's default water level could lie above dry ground). -FLT_MAX = off.
	static const float WaterlineMargin = 10.0f;  // game units, about 14 cm
	ParallaxConstants.ExtraData.w = -FLT_MAX;
	if (TheSettingManager->SettingsMain.Main.CheapUnderwaterTerrain && Tes && Tes->waterManager && Tes->waterManager->waterGroups.count) {
		TESWaterForm* water = nullptr;
		const float height = Tes->GetWaterHeight(Player, WorldSceneGraph, &water);
		if (water) ParallaxConstants.ExtraData.w = height - WaterlineMargin - TheRenderManager->CameraPosition.z;
	}
};

