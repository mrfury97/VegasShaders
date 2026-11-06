#include "Grass.h"
#include "../../core/NewGrass.h"	// UNOFFICIAL 2026-10-18 NewGrass ([Shaders.Grass.NewGrass]): blade geometry in place of the near grass cards
#include "GrassCollision.h"	// UNOFFICIAL 2026-10-19 [Shaders.Grass.Collision]
#include "../../core/GrassWind.h"	// UNOFFICIAL 2026-10-20 [Shaders.Grass.Wind]: gusts rolling across the field along the weather's wind
#include "../../core/PlantWind.h"	// UNOFFICIAL 2026-10-20 [Shaders.Plants.Wind]: the plants in the same wind
#include "GpuGrass.h"	// UNOFFICIAL 2026-10-27 [Shaders.Grass.GPUGrass] (NVR DXVK 1.7-grass)
#include "../../core/GrassShadows.h"	// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] CastShadows
#include <algorithm>
#include <unordered_map>
#include <vector>

void GrassShaders::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_GrassScale", &Constants.Scale);
	PlantWind::RegisterConstants();	// UNOFFICIAL 2026-10-20: the SpeedTree leaves' share of the plant wind (TESR_PlantLeafWind*)
	// UNOFFICIAL 2026-11-05 grass lamps (from Balebandro's fork)
	TheShaderManager->RegisterConstant("TESR_GrassLightData", &Constants.LightData);
	TheShaderManager->RegisterConstant("TESR_GrassLightPosition", LightPosition);
	TheShaderManager->RegisterConstant("TESR_GrassLightColor", LightColor);
	InstallHooks();	// the grass normal maps, per shape
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
	GrassCollision::ReadSettings();	// UNOFFICIAL 2026-10-19
	GrassWind::ReadSettings();	// UNOFFICIAL 2026-10-20 (live)
	GpuGrass::ReadSettings();	// UNOFFICIAL 2026-10-27 GPU grass (live)
	GrassShadows::ReadSettings();	// UNOFFICIAL 2026-10-27 grass sun shadows (live)

	// UNOFFICIAL 2026-11-05 (from Balebandro's fork) live
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

// UNOFFICIAL 2026-11-05 (from Balebandro's fork, after Physically-Based-Reloaded) the lamps nearest the camera that reach anything near
// it, from both of NVR's light lists (the shadow-casting ones, ShadowsExteriors' ShadowLightPosition with colours in LightColor[0..], and
// the rest, LightPosition with colours after them). A light's sphere edge counts, not its centre: a big lamp farther off can light more
// of the grass in view.
void GrassShaders::UpdateLights() {
	Constants.LightData = D3DXVECTOR4(0.0f, Lights.Strength, 0.0f, 0.0f);
	if (!Enabled || !Lights.Enabled || Lights.MaxLights <= 0 || Lights.Strength <= 0.0f || !TheShaderManager->Effects.ShadowsExteriors) return;
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
	const int used = (std::min)(count, Lights.MaxLights);
	for (int i = 0; i < used; i++) { LightPosition[i] = found[i].pos; LightColor[i] = found[i].color; }
	Constants.LightData.x = (float)used;
}

// The grass normal maps, as Physically-Based-Reloaded's MaterialMaps finds a texture's companions: "Data\Textures\Landscape\Grass01.dds"
// -> "textures\landscape\grass01_n.dds" (a "_d" diffuse: "rock_d.dds" -> "rock_n.dds"), read through the game's own file system (loose
// or in a BSA), loaded once per path and kept for the session (a few small maps).
namespace GrassNormalMaps {
	struct Entry { std::string Path; IDirect3DTexture9* Map; };
	static std::unordered_map<std::string, IDirect3DTexture9*> ByPath;
	static std::unordered_map<const NiSourceTexture*, Entry> ByTexture;

	static IDirect3DTexture9* LoadFromGame(const char* Path) {
		typedef void* (__cdecl* GetFileFn)(const char*, UInt32, UInt32, UInt32);
		typedef UInt32 (__cdecl* ReadFn)(void*, void*, UInt32, UInt32*, UInt32);
		void* File = ((GetFileFn)0xAFDF20)(Path, 0, 0x4000, 2);	// the game's file open (BSA or loose)
		if (!File) return nullptr;
		IDirect3DTexture9* Map = nullptr;
		UInt32* VTable = *(UInt32**)File;
		if (*(bool*)((UInt8*)File + 0x2C)) {
			const UInt32 Size = ThisCall(VTable[10], File);
			if (Size > 128 && Size < 0x10000000) {
				std::vector<UInt8> Data(Size);
				UInt32 ComponentSize = 1;
				const UInt32 Read = (*(ReadFn*)((UInt8*)File + 0x08))(File, Data.data(), Size, &ComponentSize, 1);
				if (Read == Size && FAILED(D3DXCreateTextureFromFileInMemory(TheRenderManager->device, Data.data(), Size, &Map))) Map = nullptr;
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
			if (Found != ByPath.end()) Map = Found->second;
			else {
				Map = LoadFromGame(Companion.c_str());
				if (!Map) Map = LoadFromGame(("data\\" + Companion).c_str());
				ByPath[Companion] = Map;
				if (Map) Logger::Log("UNOFFICIAL Grass: normal map %s", Companion.c_str());
			}
		}
		ByTexture[Diffuse] = { std::string(Name), Map };
		return Map;
	}
}

// Every grass shape as the game sets it up to draw: the grass shader's per-geometry setup (TallGrassShader, vtable 0x10B8980 slot 24;
// the geometry first). The game draws grass in groups -- one shader setup, then this for each shape -- so the SetShaders hook alone
// sees only a group's first shape. Chained (There Stands The Grass NVSE hooks the same slot; whichever comes second calls the other).
typedef int(__thiscall* GrassSetupFn)(void*, NiGeometry*, int, int, int, int, int, float*, int);
static GrassSetupFn sGrassSetup = nullptr;

static int __fastcall GrassSetupHook(void* Shader, void*, NiGeometry* Geometry, int a3, int a4, int a5, int a6, int a7, float* Transform, int a9) {
	const int result = sGrassSetup(Shader, Geometry, a3, a4, a5, a6, a7, Transform, a9);
	if (GrassShaders* grass = TheShaderManager ? TheShaderManager->Shaders.Grass : nullptr) grass->OnGrassShape(Geometry);
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
	Logger::Log("UNOFFICIAL Grass: per-shape setup hooked (was %p) for the grass normal maps", sGrassSetup);
}

void GrassShaders::OnGrassShape(NiGeometry* Geometry) {
	if (!Geometry || !Enabled || !LastPSMapped) return;	// only NVR's GRASS23x000(TMS).pso reads s1 / c81 (the PointLights builds use s1 for AttMap)
	// The shape's diffuse texture: TallGrassShaderProperty (shade type 4) keeps it (an NiSourceTexture) at +0x84 (its getter 0xBB36C0,
	// which TallGrassShader's texture setup 0xBAC950 binds to s0).
	const NiSourceTexture* diffuse = nullptr;
	if (NiShadeProperty* shade = (NiShadeProperty*)Geometry->GetProperty(NiProperty::kType_Shade))
		if (shade->m_eShaderType == NiShadeProperty::kProp_TallGrass)
			diffuse = *(NiSourceTexture**)((UInt8*)shade + 0x84);
	IDirect3DTexture9* map = nullptr;
	if (NormalMaps.Enabled && NormalMaps.Strength > 0.0f && diffuse) map = GrassNormalMaps::Find(diffuse);
	if (map) {	// through the render state cache, so the next shader using s1 gets its own back
		NiDX9RenderState* state = TheRenderManager->renderState;
		state->SetTexture(1, map);
		state->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
		state->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
		state->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR, false);
		state->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR, false);
		state->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR, false);
		state->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE, false);
	}
	const D3DXVECTOR4 material(map ? 1.0f : 0.0f, NormalMaps.Strength, Specular.Enabled ? Specular.Strength : 0.0f, Specular.Glossiness);
	TheRenderManager->device->SetPixelShaderConstantF(81, (const float*)&material, 1);
}

void GrassShaders::UpdateConstants() {
	GrassCollision::Update();	// UNOFFICIAL 2026-10-19: the push round the camera, once a frame
	// UNOFFICIAL 2026-10-20 grass field wind: the weather's wind and the clock, once a rendered frame. While a sandstorm blows, the
	// clock's rate blends to the storm's (its grass path shares c19.z).
	const SandstormEffect* Sandstorm = TheShaderManager->Effects.Sandstorm;
	const float storm = Sandstorm && Sandstorm->Enabled && Sandstorm->GrassStorm[0] > 0.0f ? Sandstorm->Intensity : 0.0f;
	GrassWind::Update(Tes ? Tes->sky : nullptr, TheShaderManager->GameState.isExterior, storm);
	UpdateLights();	// UNOFFICIAL 2026-11-05 grass lamps
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
	LastBindName = VertexShader->Name;
	LastBindNVR = false;
	LastPSMapped = false;	// UNOFFICIAL 2026-11-05 grass normal maps (OnGrassShape)
	if (!VertexShader->ShaderHandleBackup || VertexShader->ShaderHandle == VertexShader->ShaderHandleBackup) return;	// the game's own VS
	LastBindNVR = true;

	const bool NVRPixel = PixelShader && PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
	LastPSMapped = NVRPixel && PixelShader->Name && !_strnicmp(PixelShader->Name, "GRASS23x000", 11);	// UNOFFICIAL 2026-11-05 (000TMS and 000, never a PointLights build)
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
	GrassCollision::Bind(TheRenderManager->device);	// UNOFFICIAL 2026-10-19: the push texture on vertex sampler 0
	GrassWind::BindCards(TheRenderManager->device);
	GrassWind::LogState(TheRenderManager->device);	// diagnostic (Log 1)
}
