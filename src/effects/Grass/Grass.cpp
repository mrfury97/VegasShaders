#include <algorithm>

#include "Grass.h"
#include "../../core/NewGrass.h"	// UNOFFICIAL 2026-10-18 NewGrass ([Shaders.Grass.NewGrass]): blade geometry in place of the near grass cards
#include "../../core/GrassWind.h"	// UNOFFICIAL 2026-10-20 [Shaders.Grass.Wind]: gusts rolling across the field along the weather's wind
#include "../../core/PlantWind.h"	// UNOFFICIAL 2026-10-20 [Shaders.Plants.Wind]: the plants in the same wind
#include "GpuGrass.h"	// UNOFFICIAL 2026-10-27 [Shaders.Grass.GPUGrass] (NVR DXVK 1.7-grass)
#include "../../core/GrassShadows.h"	// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] CastShadows
#include "../../core/FrameClock.h"
#include "../../core/FlashlightDeferred.h"	// UNOFFICIAL 2026-11-04 the deferred flashlight on the grass (UpdateLights)
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-11-12 grass records (SetForDraw: GrassBind)

void GrassShaders::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_GrassScale", &Constants.Scale);
	PlantWind::RegisterConstants();	// UNOFFICIAL 2026-10-20: the SpeedTree leaves' share of the plant wind (TESR_PlantLeafWind*)
	// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded)
	Constants.Brightness = D3DXVECTOR4(1.0f, 1.0f, 0.0f, 0.0f);
	TheShaderManager->RegisterConstant("TESR_GrassBrightness", &Constants.Brightness);
	Constants.Record = D3DXVECTOR4(1.0f, 1.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-11-12 CapturedGrassTint (UpdateSettings): colour x 1, its own saturation, the tint off
	TheShaderManager->RegisterConstant("TESR_GrassRecord", &Constants.Record);
	TheShaderManager->RegisterConstant("TESR_GrassBend", &Constants.Bend);
	TheShaderManager->RegisterConstant("TESR_GrassLightData", &Constants.LightData);
	TheShaderManager->RegisterConstant("TESR_GrassLightPosition", LightPosition);
	TheShaderManager->RegisterConstant("TESR_GrassLightColor", LightColor);
	TheShaderManager->RegisterConstant("TESR_GrassSpot", Spot);	// UNOFFICIAL 2026-11-04 the deferred flashlight on the grass
	TheShaderManager->RegisterConstant("TESR_GrassSpotShadow", &SpotShadow);	// and its shadow
	TheShaderManager->RegisterConstant("TESR_GrassSpotFalloff", &SpotFalloff);	// and its falloff
	InstallHooks();
}

// iMinGrassSize, fTexturePctThreshold, fGrass*Distance and fGrassWindMagnitude* are consumed
// at cell load: writing them affects only cells loaded afterwards.
void GrassShaders::UpdateSettings() {
	if (!Enabled) return;

	Constants.Scale.x = TheSettingManager->GetSettingF("Shaders.Grass.Main", "ScaleX");
	Constants.Scale.y = TheSettingManager->GetSettingF("Shaders.Grass.Main", "ScaleY");
	Constants.Scale.z = TheSettingManager->GetSettingF("Shaders.Grass.Main", "ScaleZ");
	Constants.Scale.w = (TheSettingManager->GetSettingI("Shaders.Grass.Main", "ShadowFix") ? 1.0f : 0.0f)	// UNOFFICIAL 2026-10-27 (GRASS23x000TMS.pso)
		+ (TheSettingManager->GetSettingI("Shaders.Grass.Main", "SkyLightScale") ? 2.0f : 0.0f);
	GameShaderFix = TheSettingManager->GetSettingI("Shaders.Grass.Main", "GameShaderFix");
	DrawFirst = TheSettingManager->GetSettingI("Shaders.Grass.Main", "DrawFirst");
	DrawFirstRestore = TheSettingManager->GetSettingI("Shaders.Grass.Main", "DrawFirstRestore");
	DrawFirstCull = TheSettingManager->GetSettingI("Shaders.Grass.Main", "DrawFirstCull");
	NewGrass::ReadSettings();	// UNOFFICIAL 2026-10-18 NewGrass (core/NewGrass.h): live
	GrassWind::ReadSettings();	// UNOFFICIAL 2026-10-20 (live)
	GpuGrass::ReadSettings();	// UNOFFICIAL 2026-10-27 GPU grass (live)
	GrassShadows::ReadSettings();	// UNOFFICIAL 2026-10-27 grass sun shadows (live)

	// UNOFFICIAL 2026-11-04, from Physically-Based-Reloaded (effects/Grass.cpp).
	Constants.Brightness.x = std::clamp(TheSettingManager->GetSettingF("Shaders.Grass.Main", "Brightness"), 0.0f, 4.0f);
	NightBrightness = std::clamp(TheSettingManager->GetSettingF("Shaders.Grass.Main", "NightBrightness"), 0.0f, 4.0f);
	{	// UNOFFICIAL 2026-11-12 CapturedGrass, second pass (GRASS23x000TMS.pso.hlsl, the records at main's end; live; read by the record
		// outputs only, which exist only in a shader compiled with the AlbedoBuffer): z = 1 - [Main.Main.Quality] CapturedGrassShade
		// (0-1, a missing key = 1: the recorded colour times the tuft's own light scale, as the game shades each tuft; 0 = the texture
		// alone), w = the material class written: 7 ground cover (plain matte), or 5 plants with CapturedGrassPlant (a missing key = off:
		// the path tracer's thin-leaf model, as the first version wrote it, for an A/B).
		SettingManager::Configuration::ConfigNode node;
		const float shade = TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedGrassShade") ? std::clamp(node.FloatValue, 0.0f, 1.0f) : 1.0f;
		const bool plant = TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedGrassPlant") && node.BoolValue;
		Constants.Brightness.z = 1.0f - shade;
		Constants.Brightness.w = plant ? 5.0f : 7.0f;
		// UNOFFICIAL 2026-11-12 CapturedGrassTint, third pass (an RTX run of the second: the tufts steady, but bright yellow-orange under the
		// path tracer where the game shows a muted brown). The record lacked the grass mesh's vertex colour, which the game multiplies into
		// the sun alone (GRASS23x000TMS.pso.hlsl, main's end, has the maths). TESR_GrassRecord, live, a missing key = the default:
		//   z CapturedGrassTint (0-1, default 1): how much of "the share of the light the game leaves the tuft" the recorded colour takes;
		//     0 = the second pass's colour. Written 0 while CapturedGrass itself is off, so the shader's branch is not taken.
		//   x CapturedGrassColour (0-4, default 1) and y CapturedGrassSaturation (0-1, default 1): what is left to tune by eye in one launch
		//     (the self-check line in core/CapturedNormals.h AlbedoCheck prints the grass's luma and red / blue against the land's).
		const bool records = TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedGrass") && node.BoolValue;
		const float tint = TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedGrassTint") ? std::clamp(node.FloatValue, 0.0f, 1.0f) : 1.0f;
		const float colour = TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedGrassColour") ? std::clamp(node.FloatValue, 0.0f, 4.0f) : 1.0f;
		const float saturation = TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "CapturedGrassSaturation") ? std::clamp(node.FloatValue, 0.0f, 1.0f) : 1.0f;
		Constants.Record = D3DXVECTOR4(colour, saturation, records ? tint : 0.0f, 0.0f);
		CapturedNormals::GrassColourKeys[0] = tint; CapturedNormals::GrassColourKeys[1] = colour; CapturedNormals::GrassColourKeys[2] = saturation;
		// one line when the records' colour settings are first in use or change (never while CapturedGrass is off)
		static float logged[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
		if (records && (logged[0] != tint || logged[1] != colour || logged[2] != saturation || logged[3] != shade)) {
			logged[0] = tint; logged[1] = colour; logged[2] = saturation; logged[3] = shade;
			Logger::Log("CapturedGrass: the recorded colour = texture x Brightness x tuft shade (CapturedGrassShade %.2f) x the game's light share with the vertex colour (CapturedGrassTint %.2f%s), saturation %.2f (CapturedGrassSaturation), x %.2f (CapturedGrassColour)",
				shade, tint, tint > 0.0f ? "" : ": OFF, the texture alone as before", saturation, colour);
		}
	}

	const char* bend = "Shaders.Grass.Interaction";
	Interaction.Enabled = TheSettingManager->GetSettingI(bend, "Enabled") != 0;
	Interaction.Radius = std::clamp(TheSettingManager->GetSettingF(bend, "Radius"), 10.0f, 1000.0f);
	Interaction.StrengthStanding = std::clamp(TheSettingManager->GetSettingF(bend, "StrengthStanding"), 0.0f, 1000.0f);
	Interaction.StrengthMax = std::clamp(TheSettingManager->GetSettingF(bend, "StrengthMax"), 0.0f, 1000.0f);
	Interaction.SpeedForMax = std::clamp(TheSettingManager->GetSettingF(bend, "SpeedForMax"), 1.0f, 5000.0f);
	Interaction.SpringBackSeconds = std::clamp(TheSettingManager->GetSettingF(bend, "SpringBackSeconds"), 0.01f, 10.0f);
	Interaction.PositionLagSeconds = std::clamp(TheSettingManager->GetSettingF(bend, "PositionLagSeconds"), 0.0f, 2.0f);
	Interaction.Debug = TheSettingManager->GetSettingI(bend, "Debug") != 0;

	const char* lod = "Shaders.Grass.LOD";
	LOD.Enabled = TheSettingManager->GetSettingI(lod, "Enabled") != 0;
	LOD.MinPixels = std::clamp(TheSettingManager->GetSettingF(lod, "MinPixels"), 0.0f, 200.0f);
	LOD.FadePixels = std::clamp(TheSettingManager->GetSettingF(lod, "FadePixels"), 0.5f, 200.0f);
	LOD.DetailVertices = std::clamp(TheSettingManager->GetSettingF(lod, "DetailVertices"), 1.0f, 65535.0f);
	LOD.DetailStrength = std::clamp(TheSettingManager->GetSettingF(lod, "DetailStrength"), 0.0f, 10.0f);

	const char* lights = "Shaders.Grass.Lights";
	Lights.Enabled = TheSettingManager->GetSettingI(lights, "Enabled") != 0;
	Lights.MaxLights = std::clamp(TheSettingManager->GetSettingI(lights, "MaxLights"), 0, MaxGrassLights);
	Lights.Strength = std::clamp(TheSettingManager->GetSettingF(lights, "Strength"), 0.0f, 1.0f);

	const char* normals = "Shaders.Grass.NormalMaps";
	NormalMaps.Enabled = TheSettingManager->GetSettingI(normals, "Enabled") != 0;
	NormalMaps.Strength = std::clamp(TheSettingManager->GetSettingF(normals, "Strength"), 0.0f, 4.0f);

	Specular.Enabled = TheSettingManager->GetSettingI(normals, "EnableSpecular") != 0;
	Specular.Strength = std::clamp(TheSettingManager->GetSettingF(normals, "SpecularStrength"), 0.0f, 4.0f);
	Specular.Glossiness = std::clamp(TheSettingManager->GetSettingF(normals, "Glossiness"), 1.0f, 512.0f);
}

// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) the lamps nearest the camera that reach anything near it, from both of NVR's
// light lists (the shadow-casting ones, ShadowsExteriors' ShadowLightPosition with colours in LightColor[0..], and the rest,
// LightPosition with colours after them). A light's sphere edge counts, not its centre: a big lamp farther off can light more of the
// grass in view.
void GrassShaders::UpdateLights() {
	Constants.LightData = D3DXVECTOR4(0.0f, Lights.Strength, 0.0f, 0.0f);
	// UNOFFICIAL 2026-11-04 the deferred flashlight: the G-buffer holds no grass, so the grass shaders light it by the same beam themselves
	// (its shadow below), whether or not the lamps are on. At 0.375 of the beam's light, fixed (user 2026-11-04: 1 looked twice as bright as
	// the ground, 0.5 still too bright; then "hardcode the brightness on grass to the light": no setting)
	static const float FlashlightGrassStrength = 0.375f;
	float spotStrength = TheShaderManager->Effects.Flashlight ? FlashlightGrassStrength : 0.0f;
	// times the lamps' light scale, as the light pass's diffuse takes it (Object.hlsl LIN_SCALE_LIGHT: TESR_LinearScales.x, PBR.cpp)
	if (const PBRShaders* pbr = TheShaderManager->Shaders.PBR) if (pbr->Constants.LinearScales.x > 0.0f) spotStrength *= pbr->Constants.LinearScales.x;
	if (spotStrength > 0.0f && FlashlightDeferred::LightParams(Spot)) { Spot[2].x *= spotStrength; Spot[2].y *= spotStrength; Spot[2].z *= spotStrength; }
	else Spot[0].w = 0.0f;
	SpotFalloff = D3DXVECTOR4(TheShaderManager->Effects.Flashlight ? TheShaderManager->Effects.Flashlight->Deferred.Falloff : 0.0f, 0.0f, 0.0f, 0.0f);
	// its shadow, as the light pass reads it (the matrix is the live TESR_SpotLightToWorldTransform)
	SpotShadow = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	if (Spot[0].w > 0.0f) if (IDirect3DTexture9* map = FlashlightDeferred::ShadowMap()) {
		D3DSURFACE_DESC desc = {};
		map->GetLevelDesc(0, &desc);
		const FlashlightEffect* f = TheShaderManager->Effects.Flashlight;
		SpotShadow = D3DXVECTOR4(1.0f, desc.Width ? 1.0f / (float)desc.Width : 0.0f, f->Deferred.ShadowBias, FlashlightDeferred::SoftnessTexels(desc.Width));
	}
	// UNOFFICIAL 2026-11-12 CapturedGrass: while the grass lights itself by this beam, its records are not written for the raster frame's
	// RT GI (core/CapturedNormals.h BeginFrame: the light pass lights everything the records hold, the grass would get the beam twice)
	CapturedNormals::GrassOwnBeam = Spot[0].w > 0.0f;
	if (!Lights.Enabled || Lights.MaxLights <= 0 || Lights.Strength <= 0.0f) return;

	struct Candidate { float key; D3DXVECTOR4 pos; D3DXVECTOR4 color; };
	Candidate found[ShadowCubeMapsScreen + TrackedLightsMax];
	int count = 0;
	const D3DXVECTOR4 cam = TheRenderManager->CameraPosition;
	auto add = [&](const D3DXVECTOR4& pos, const D3DXVECTOR4& color) {
		const float radius = pos.w;
		const float brightness = (color.x + color.y + color.z) * color.w;
		if (radius <= 1.0f || brightness <= 0.01f) return;
		const float dx = pos.x - cam.x, dy = pos.y - cam.y, dz = pos.z - cam.z;
		found[count++] = { sqrtf(dx * dx + dy * dy + dz * dz) - radius, D3DXVECTOR4(dx, dy, dz, radius),
			D3DXVECTOR4(color.x * color.w, color.y * color.w, color.z * color.w, 0.0f) };
	};
	const D3DXVECTOR4* shadowLights = TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowLightPosition;
	for (int i = 0; i < ShadowCubeMapsScreen; i++) add(shadowLights[i], TheShaderManager->LightColor[i]);
	for (int i = 0; i < TrackedLightsMax; i++) add(TheShaderManager->LightPosition[i], TheShaderManager->LightColor[ShadowCubeMapsScreen + i]);

	std::sort(found, found + count, [](const Candidate& a, const Candidate& b) { return a.key < b.key; });
	const int used = min(count, Lights.MaxLights);
	for (int i = 0; i < used; i++) {
		LightPosition[i] = found[i].pos;
		LightColor[i] = found[i].color;
	}
	Constants.LightData.x = (float)used;
}

void GrassShaders::UpdateConstants() {
	// UNOFFICIAL 2026-10-20 grass field wind: the weather's wind and the clock, once a rendered frame. While a sandstorm blows, the
	// clock's rate blends to the storm's (its grass path shares c19.z).
	const SandstormEffect* Sandstorm = TheShaderManager->Effects.Sandstorm;
	const float storm = Sandstorm && Sandstorm->Enabled && Sandstorm->GrassStorm[0] > 0.0f ? Sandstorm->Intensity : 0.0f;
	GrassWind::Update(Tes ? Tes->sky : nullptr, TheShaderManager->GameState.isExterior, storm);

	// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) There Stands The Grass NVSE's PlayerBend::Update, line for line
	// (core/PlayerBend.cpp, Into-The-Rough): the bend origin eases toward the player over PositionLagSeconds, the strength toward its
	// target over SpringBackSeconds, both ways; the speed is the raw distance moved this frame, and anything past 2000 units a second
	// is a teleport or a load. Once a frame (this runs twice a frame, FrameClock.h).
	const InteractionSettings& s = Interaction;
	const float dt = (float)ElapsedOncePerFrame(LastFrameTime);
	if (dt > 0.0f) {
		bool active = s.Enabled && Player && TheShaderManager->GameState.isExterior;

		float target = 0.0f, px = 0.0f, py = 0.0f, pz = 0.0f;
		if (active) {
			px = Player->pos.x; py = Player->pos.y; pz = Player->pos.z;
			float speed = 0.0f;
			if (HavePrev) {
				float dx = px - LastPlayerPos.x, dy = py - LastPlayerPos.y;
				speed = sqrtf(dx * dx + dy * dy) / dt;
				if (speed > 2000.0f) speed = 0.0f;   // teleport or load
			}
			LastPlayerPos = Player->pos;
			HavePrev = true;
			Speed = speed;
			float frac = min(speed / s.SpeedForMax, 1.0f);
			target = s.StrengthStanding + (s.StrengthMax - s.StrengthStanding) * frac;
		}
		else HavePrev = false;

		if (!Have && active) {
			BendCentre = Player->pos;
			Have = true;
		}
		if (Have) {
			float posAlpha = min(dt / s.PositionLagSeconds, 1.0f);
			if (active) {
				BendCentre.x += (px - BendCentre.x) * posAlpha;
				BendCentre.y += (py - BendCentre.y) * posAlpha;
				BendCentre.z += (pz - BendCentre.z) * posAlpha;
			}
			float strAlpha = min(dt / s.SpringBackSeconds, 1.0f);
			Push += (target - Push) * strAlpha;
			if (!active && Push < 0.05f) {
				Have = false;
				Push = 0.0f;
			}
		}
	}

	// The shader constants, as TSTG's GrassBend::Upload builds them: the origin, 1 / radius, and the strength per unit of blade height
	// (strength / 90 * 0.5; TESR_Translucency.w, PBR.cpp). The origin goes into the frame the grass shaders rebuild positions in:
	// camera-relative world.
	const D3DXVECTOR4 cam = TheRenderManager->CameraPosition;
	const float strength = Have ? Push : 0.0f;
	Constants.Bend = D3DXVECTOR4(BendCentre.x - cam.x, BendCentre.y - cam.y, BendCentre.z - cam.z, 1.0f / max(s.Radius, 1.0f));
	BendPush = strength / 90.0f * 0.5f;

	// The night brightness, outdoors: full at night, 1 by day, following the same curve as the other day/night transitions.
	const float day = TheShaderManager->GameState.isExterior ? TheShaderManager->GameState.transitionCurve : 1.0f;
	Constants.Brightness.y = NightBrightness + (1.0f - NightBrightness) * day;

	UpdateLights();

	if (s.Debug && (DebugClock += max(dt, 0.0f)) >= 2.0f) {
		DebugClock = 0.0f;
		Logger::Log("Grass interaction: speed %.0f, strength %.1f, centre %.0f %.0f %.0f camera-relative", Speed, strength,
			Constants.Bend.x, Constants.Bend.y, Constants.Bend.z);
	}
}

// UNOFFICIAL 2026-10-17 grass study: every pass that sets grass shaders up (SetShadersHook; the game does it once for a run of
// grass draws). Tells NVR's grass vertex shaders (c19, GrassPairing in GRASS23x00*.vso) which pixel shader reads them: NVR's
// GRASS23x000TMS.pso (transparency multisampling on) takes the light colours and amounts apart; the game's own GRASS23x000.pso
// (the INI default, no transparency multisampling) needs them multiplied together as the game wrote them. GameShaderFix = false
// gives the P80 behaviour (the bare colours to the game's shader: too bright) for A/B tests.
void GrassShaders::SetForDraw(NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader) {
	if (!Enabled || !VertexShader || !VertexShader->Name || memcmp(VertexShader->Name, "GRASS", 5)) return;
	// UNOFFICIAL 2026-10-18 NewGrass: which grass VS variant this pass binds ('GRASS23x00N': N = 0 LF, 1 LV, 2 LFS, 3 LVS; the card
	// variants are 0 and 2, the others light real meshes by their normals and are never replaced by blades).
	NewGrass::LastVariant = strlen(VertexShader->Name) >= 11 ? VertexShader->Name[10] : 0;
	BindSerial++;	// UNOFFICIAL 2026-10-28 (GrassFirst: whether this frame's early run saw a grass bind of its own)
	// UNOFFICIAL 2026-11-12 grass records (core/CapturedNormals.h GrassWriters; audit item 1): the grass draws that follow this bind write
	// their colour, material and normal records while the path tracer replaces the picture. GrassFirst's early run (DrawPart 1) opens the
	// capture's span here, before the main list; a bind after the list arms the draw hook. Nothing otherwise.
	CapturedNormals::GrassBind(TheRenderManager->device, DrawPart);
	LastBindName = VertexShader->Name;
	LastBindNVR = false;
	if (!VertexShader->ShaderHandleBackup || VertexShader->ShaderHandle == VertexShader->ShaderHandleBackup) return;	// the game's own VS
	LastBindNVR = true;

	const bool NVRPixel = PixelShader && PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
	// w: GrassFirst's part (DrawPart; Render.cpp namespace GrassFirst): 0 every tuft, 1 the near tufts, 2 the fading ones.
	// UNOFFICIAL 2026-10-18 sandport: y, z the sandstorm's wind (SandstormEffect::GrassStorm: the extra sway and the gust clock;
	// 0, 0 with no storm or the effect off, and the grass VS then sways exactly as the game's). GrassFirst's re-writes of c19 keep
	// them (LastPairing).
	const SandstormEffect* Sandstorm = TheShaderManager->Effects.Sandstorm;
	// UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind], core/GrassWind.h): z is its clock while it is on (the storm's
	// gusts then run on it too, at a rate blended to theirs), and its lanes go into the 4th column of c252-c255 (the inverse view
	// matrix SetCT has just put there). Off: z the storm's clock as before, c252-c255 as SetCT wrote them.
	const float Pairing[4] = { NVRPixel || !GameShaderFix ? 1.0f : 0.0f, Sandstorm ? Sandstorm->GrassStorm[0] : 0.0f,
		GrassWind::On ? GrassWind::ClockNow : Sandstorm ? Sandstorm->GrassStorm[1] : 0.0f, (float)DrawPart };
	TheRenderManager->device->SetVertexShaderConstantF(19, Pairing, 1);
	for (int i = 0; i < 4; i++) LastPairing[i] = Pairing[i];
	GrassWind::BindCards(TheRenderManager->device);
	GrassWind::LogState(TheRenderManager->device);	// diagnostic (Log 1)
}

// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) written for every shape, LOD or not: z (static) must never carry over from
// one shape to the next. The size fade's terms are folded here (Includes/GrassWind.hlsl GrassLODFade): x the mesh's radius times the
// pixels a unit covers at a depth of 1 (half the screen height times the projection's vertical scale) over FadePixels, y MinPixels
// times the detail factor over FadePixels; x 0 turns the fade off for the shape. w 1: the bend's debug tint.
void GrassShaders::SetBatchConstants(NiGeometry* Geometry, bool Static) {
	if (!Enabled || !Geometry || !Geometry->geomData) return;
	D3DXVECTOR4 batch(0.0f, 0.0f, Static ? 1.0f : 0.0f, Interaction.Debug ? 1.0f : 0.0f);
	if (LOD.Enabled) {
		NiGeometryData* data = Geometry->geomData;
		const float pixelsPerUnit = 0.5f * (float)TheRenderManager->height * TheRenderManager->projMatrix._22;
		const float detail = 1.0f + LOD.DetailStrength * min((float)data->m_usVertices / LOD.DetailVertices, 1.0f);
		batch.x = max(data->m_kBound.Radius, 0.0f) * pixelsPerUnit / LOD.FadePixels;
		batch.y = LOD.MinPixels * detail / LOD.FadePixels;
	}
	TheRenderManager->device->SetVertexShaderConstantF(17, (const float*)&batch, 1);
}

// Rocks placed as grass (Rock Grass and the like) stand still: no sway, no bending. Known by their diffuse texture's path.
static bool IsStaticGrass(const NiSourceTexture* Diffuse) {
	const char* path = Diffuse ? Diffuse->ddsPath1 : nullptr;
	if (!path) return false;
	for (const char* p = path; *p; p++)
		if (!_strnicmp(p, "rock", 4)) return true;
	return false;
}

// The grass normal maps ([Shaders.Grass.NormalMaps]), as Physically-Based-Reloaded's MaterialMaps finds a texture's companions:
// "Data\Textures\Landscape\Grass01.dds" -> "textures\landscape\grass01_n.dds" (a "_d" diffuse: "rock_d.dds" -> "rock_n.dds"), read
// through the game's own file system (loose or in a BSA), loaded once per path and kept for the session (a few small maps).
namespace GrassNormalMaps {
	struct Entry { std::string Path; IDirect3DTexture9* Map; };
	static std::unordered_map<std::string, IDirect3DTexture9*> ByPath;
	static std::unordered_map<const NiSourceTexture*, Entry> ByTexture;

	static IDirect3DTexture9* LoadFromGame(const char* Path) {
		typedef void* (__cdecl* GetFileFn)(const char*, UInt32, UInt32, UInt32);
		typedef UInt32 (__cdecl* ReadFn)(void*, void*, UInt32, UInt32*, UInt32);
		void* File = ((GetFileFn)0xAFDF20)(Path, 0, 0x4000, 2);
		if (!File) return nullptr;

		IDirect3DTexture9* Map = nullptr;
		UInt32* VTable = *(UInt32**)File;
		if (*(bool*)((UInt8*)File + 0x2C)) {
			const UInt32 Size = ThisCall(VTable[10], File);
			if (Size > 128 && Size < 0x10000000) {
				std::vector<UInt8> Data(Size);
				UInt32 ComponentSize = 1;
				const UInt32 Read = (*(ReadFn*)((UInt8*)File + 0x08))(File, Data.data(), Size, &ComponentSize, 1);
				if (Read == Size && FAILED(D3DXCreateTextureFromFileInMemory(TheRenderManager->device, Data.data(), Size, &Map)))
					Map = nullptr;
			}
		}
		ThisCall(VTable[0], File, true);
		return Map;
	}

	static bool CompanionPath(const char* Diffuse, std::string& Out) {
		std::string Path(Diffuse);
		for (char& c : Path) c = (c == '/') ? '\\' : (char)tolower((unsigned char)c);
		const size_t Data = Path.find("data\\");
		if (Data != std::string::npos) Path.erase(0, Data + 5);
		if (Path.compare(0, 9, "textures\\") != 0) Path.insert(0, "textures\\");
		if (Path.size() < 4 || Path.compare(Path.size() - 4, 4, ".dds") != 0) return false;
		if (Path.size() >= 6 && Path.compare(Path.size() - 6, 6, "_n.dds") == 0) return false;
		if (Path.size() >= 6 && Path.compare(Path.size() - 6, 6, "_d.dds") == 0) Path.erase(Path.size() - 6, 2);
		Path.insert(Path.size() - 4, "_n");
		Out = Path;
		return true;
	}

	static IDirect3DTexture9* Find(const NiSourceTexture* Diffuse) {
		const char* Name = Diffuse->ddsPath1;
		if (!Name || !*Name) return nullptr;
		auto Known = ByTexture.find(Diffuse);
		if (Known != ByTexture.end() && Known->second.Path == Name) return Known->second.Map;

		IDirect3DTexture9* Map = nullptr;
		std::string Companion;
		if (CompanionPath(Name, Companion)) {
			auto Found = ByPath.find(Companion);
			if (Found != ByPath.end())
				Map = Found->second;
			else {
				Map = LoadFromGame(Companion.c_str());
				if (!Map) Map = LoadFromGame(("data\\" + Companion).c_str());
				ByPath[Companion] = Map;
				if (Map) Logger::Log("Grass: normal map %s", Companion.c_str());
			}
		}
		ByTexture[Diffuse] = { std::string(Name), Map };
		return Map;
	}
}

// Every grass shape as the game sets it up to draw: the grass shader's per-geometry setup (TallGrassShader, vtable 0x10B8980 slot
// 24; the geometry first, its transform seventh). The game draws grass in groups -- one shader setup, then this for each shape -- so
// the SetShaders hook alone sees only a group's first shape. Chained: There Stands The Grass NVSE hooks the same slot, and whichever
// of us comes second calls the other.
// VegasShaders: TSTG does not stand down here (its log shows it patching the grass shaders) and writes its bend into vertex c248-c249
// for every shape, for the game's grass shaders it patches. NVR's grass VS read the inverse projection there (Includes/GrassWind.hlsl
// GrassCameraRelative), so the chain runs first and, for NVR's VS, the two rows are put back after it.
typedef int(__thiscall* GrassSetupFn)(void*, NiGeometry*, int, int, int, int, int, float*, int);
static GrassSetupFn sGrassSetup = nullptr;

static int __fastcall GrassSetupHook(void* Shader, void*, NiGeometry* Geometry, int a3, int a4, int a5, int a6, int a7, float* Transform, int a9) {
	const int result = sGrassSetup(Shader, Geometry, a3, a4, a5, a6, a7, Transform, a9);
	if (GrassShaders* grass = TheShaderManager ? TheShaderManager->Shaders.Grass : nullptr) {
		grass->OnGrassShape(Geometry);
		if (grass->Enabled && GrassShaders::LastBindNVR)
			TheRenderManager->device->SetVertexShaderConstantF(248, (const float*)&TheRenderManager->InvProjMatrix, 2);
	}
	return result;
}

void GrassShaders::InstallHooks() {
	if (sGrassSetup) return;
	void** slot = (void**)(0x10B8980 + 24 * 4);
	DWORD old;
	VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old);
	sGrassSetup = (GrassSetupFn)*slot;
	*slot = (void*)&GrassSetupHook;
	VirtualProtect(slot, 4, old, &old);
	Logger::Log("Grass: per-shape setup hooked (was %p)", sGrassSetup);
}

void GrassShaders::OnGrassShape(NiGeometry* Geometry) {
	if (!Geometry) return;

	// The shape's diffuse texture: TallGrassShaderProperty (shade type 4) keeps it (an NiSourceTexture) at +0x84 (its getter 0xBB36C0,
	// which TallGrassShader's texture setup 0xBAC950 binds to s0).
	const NiSourceTexture* diffuse = nullptr;
	if (NiShadeProperty* shade = (NiShadeProperty*)Geometry->GetProperty(NiProperty::kType_Shade))
		if (shade->m_eShaderType == NiShadeProperty::kProp_TallGrass)
			diffuse = *(NiSourceTexture**)((UInt8*)shade + 0x84);
	SetBatchConstants(Geometry, IsStaticGrass(diffuse));

	// Its normal map, from the diffuse texture's name. s1 goes through the render state cache, so the next shader using s1 gets its
	// own back.
	IDirect3DTexture9* map = nullptr;
	if (Enabled && NormalMaps.Enabled && NormalMaps.Strength > 0.0f && diffuse)
		map = GrassNormalMaps::Find(diffuse);
	if (map) {
		NiDX9RenderState* state = TheRenderManager->renderState;
		state->SetTexture(1, map);
		state->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
		state->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
		state->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
		state->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
		state->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR, false);
		state->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE, false);
	}
	D3DXVECTOR4 material(map ? 1.0f : 0.0f, NormalMaps.Strength, Specular.Enabled ? Specular.Strength : 0.0f, Specular.Glossiness);
	TheRenderManager->device->SetPixelShaderConstantF(81, (const float*)&material, 1);
}
