#include "SunFar.h"
#include "../../core/LandLight.h"	// UNOFFICIAL 2026-10-25
#include "../../core/RtSunShadows.h"	// UNOFFICIAL 2026-10-30 RT sun CloudShadows

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK)
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-11-04 PlantShadows (TESR_MaterialBuffer's plant class)
#include "../../core/ForwardCloudShadows.h"	// UNOFFICIAL 2026-11-04 ForwardCloudShadows (Active)

// UNOFFICIAL 2026-10-19 SunFar: see SunFar.h and Effects\SunFar.fx.hlsl.

static const char* CloudShadowsSection = "Shaders.CloudShadows.Main";

// UNOFFICIAL 2026-10-23: the game's water depth map (PlaceableWaterGroup::spDepthMap), drawn before the scene each frame for the
// water shaders (WATER000 etc. read it per screen position: x how deep the water is there, 0 at the shore, 1 deep). Null when
// the game has none.
static IDirect3DTexture9* WaterDepthMap() {
	BSRenderedTexture* rendered = *(BSRenderedTexture**)0x011C7B68;
	if (!rendered || !rendered->RenderedTextures[0] || !rendered->RenderedTextures[0]->rendererData) return nullptr;
	return rendered->GetD3DTexture(0);
}

void SunFarEffect::UpdateSettings() {
	Settings.Clouds = TheSettingManager->GetSettingI(CloudShadowsSection, "Enabled") != 0;
	Settings.CloudStrength = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "Strength"), 0.0f, 1.0f);
	Settings.CloudDensity = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "Density"), 0.0f, 8.0f);
	Settings.CloudHeight = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "Height"), 1000.0f, 1000000.0f);
	Settings.CloudTile = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "TileSize"), 1000.0f, 10000000.0f);
	Settings.CloudSpeed = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "Speed"), 0.0f, 20.0f);
	Settings.DebugView = std::clamp(TheSettingManager->GetSettingI(CloudShadowsSection, "DebugView"), 0, 4);
	Settings.WaterFade = TheSettingManager->GetSettingI(CloudShadowsSection, "WaterShadows") != 0;
	Settings.WaterDeepShare = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "WaterDeepShare"), 0.0f, 1.0f);
	Settings.TargetFix = TheSettingManager->GetSettingI(CloudShadowsSection, "TargetFix") != 0;
	Settings.HazardLog = TheSettingManager->GetSettingI(CloudShadowsSection, "HazardLog");
	Settings.FlickerWatch = TheSettingManager->GetSettingI(CloudShadowsSection, "FlickerWatch");
	Settings.FlickerThreshold = std::clamp(TheSettingManager->GetSettingF(CloudShadowsSection, "FlickerThreshold"), 0.01f, 0.5f);
	Settings.JitterFix = TheSettingManager->GetSettingI(CloudShadowsSection, "JitterFix") != 0;
	Settings.SkipCascadeCheck = TheSettingManager->GetSettingI(CloudShadowsSection, "SkipCascadeCheck") != 0;
	Settings.OldSunModel = TheSettingManager->GetSettingI(CloudShadowsSection, "OldSunModel") != 0;
	Settings.PlantShadows = TheSettingManager->GetSettingI("Shaders.VolumetricClouds.Main", "PlantShadows") != 0;
}

void SunFarEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_SunFarData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_SunFarClouds", &Constants.Clouds);
	TheShaderManager->RegisterConstant("TESR_SunFarCloudOffset", &Constants.CloudOffset[0]);
	TheShaderManager->RegisterConstant("TESR_SunFarCloudWeight", &Constants.CloudWeight);
	TheShaderManager->RegisterConstant("TESR_SunFarCloudBlend", &Constants.CloudBlend);
	TheShaderManager->RegisterConstant("TESR_SunFarApply", &Constants.Apply);
	TheShaderManager->RegisterConstant("TESR_SunFarVolume", &Constants.Volume);
	TheShaderManager->RegisterConstant("TESR_SunFarWater", &Constants.Water);
	TheShaderManager->RegisterConstant("TESR_SunFarLand", &Constants.Land);
	TheShaderManager->RegisterConstant("TESR_SunFarJitter", &Constants.Jitter);	// UNOFFICIAL 2026-10-31	// UNOFFICIAL 2026-10-25
}

void SunFarEffect::RegisterTextures() {
	TheTextureManager->InitTexture("TESR_SunFarBuffer", &Texture, &Surface, TheRenderManager->width, TheRenderManager->height, D3DFMT_A8R8G8B8);
}

// The texture a sky cloud layer draws with (its shader property keeps the current one; the blend texture and value say how
// far it has gone toward the next weather's).
static bool IsHorizonTexture(const char* path) {
	if (!path) return false;
	std::string lower(path);
	for (char& c : lower) c = (char)tolower((unsigned char)c);
	return lower.find("horizon") != std::string::npos;
}

static IDirect3DBaseTexture9* D3DTextureOf(NiTexture* texture) {
	return texture && texture->rendererData ? texture->rendererData->dTexture : nullptr;
}

void SunFarEffect::UpdateConstants() {
	const PBRShaders* pbr = TheShaderManager->Shaders.PBR;
	Constants.Data.z = pbr && pbr->Enabled ? (std::max)(pbr->Constants.Linear.x, 1.0f) : 1.0f;
	Constants.Data.w = 1.0f / Constants.Data.z;
	const ShadowsExteriorEffect* shadowsFx = TheShaderManager->Effects.ShadowsExteriors;
	Constants.Data.y = (float)(std::max)(Settings.DebugView, shadowsFx ? shadowsFx->TerrainShadows.Settings.DebugView : 0);
	Constants.Data.x = 0.0f;
	{	// UNOFFICIAL 2026-10-31 JitterFix / SkipCascadeCheck (SunFar.fx.hlsl)
		const D3DXVECTOR2 jitter = Settings.JitterFix && TheShaderManager->Effects.TAA ? TheShaderManager->Effects.TAA->DepthJitterUV() : D3DXVECTOR2(0.0f, 0.0f);
		Constants.Jitter = D3DXVECTOR4(jitter.x, jitter.y, Settings.SkipCascadeCheck ? 1.0f : 0.0f, 0.0f);
	}
	Constants.Water = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	// UNOFFICIAL 2026-10-25: the land's own light vector (core/LandLight.h), while it was read in the last half second or so.
	LandLight::Now++;
	Constants.Land = LandLight::Frame && LandLight::Now - LandLight::Frame < 30 ?
		D3DXVECTOR4(LandLight::Dir[0], LandLight::Dir[1], LandLight::Dir[2], 1.0f) : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	cloudsReady = false;
	for (int i = 0; i < 4; i++) layerTextures[i] = blendTextures[i] = nullptr;
	Constants.CloudWeight = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	Constants.CloudBlend = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);

	// The drift runs on the frame time (held to a tenth of a second a frame: a load or a long hitch does not jump it).
	static ULONGLONG lastTick = 0;
	const ULONGLONG tick = GetTickCount64();
	const double dt = lastTick ? (std::min)((double)(tick - lastTick) / 1000.0, 0.1) : 0.0;
	lastTick = tick;

	Sky* sky = Sky::Get();
	// Not with the volumetric clouds' own shadow (they replace the game's layers). UNOFFICIAL 2026-10-21: theirs go on the land here
	// instead, from their shadow map, the same way (only the sun's share, and not twice where the cascades already shade).
	VolumetricCloudsEffect* volumetric = TheShaderManager->Effects.VolumetricClouds;
	const bool volumetricShadows = volumetric && volumetric->Enabled && volumetric->Constants.Extra.x > 0.0f;
	Constants.Volume = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	// UNOFFICIAL 2026-11-04 ForwardCloudShadows: the surface shaders took the clouds' shadow this frame: not here as well.
	if (volumetric && TheShaderManager->GameState.isExterior && !ForwardCloudShadows::Active) {
		const float strength = volumetric->ShadowStrengthForSunFar();
		if (strength > 0.0f) Constants.Volume = D3DXVECTOR4(strength, (std::max)(volumetric->Constants.Extra.y, 1000.0f),
			(std::max)(volumetric->Constants.Detail.z, 256.0f), volumetric->Constants.Shape.x);
	}
	// What the cloud shadows found (or why not), logged when it changes.
	std::string state;
	struct StateLog { std::string& s; ~StateLog() { static std::string last; if (s != last) { last = s; if (!s.empty()) Logger::Log("UNOFFICIAL cloud shadows: %s", s.c_str()); } } } stateLog{ state };
	if (!Settings.Clouds || Settings.CloudStrength <= 0.0f) return;
	if (!sky || !sky->clouds) { state = "off: no sky clouds object"; return; }
	if (volumetricShadows) { state = "off: the volumetric clouds draw their own shadows"; return; }
	if (!TheShaderManager->GameState.isExterior) return;

	// The light's height: the clouds' shadows fade out as the sun nears the horizon (the layer is met ever farther off).
	const float sunZ = TheShaderManager->ShaderConst.SunDir.z;
	const float fade = std::clamp((sunZ - 0.05f) / 0.2f, 0.0f, 1.0f);
	if (fade <= 0.0f) { state = "faded out: the sun is too low"; return; }

	TESWeather* weather = sky->firstWeather;
	const float windSpeed = std::clamp(sky->windSpeed, 0.0f, 1.0f);
	const float windAngle = sky->windDirection;
	const int count = (std::min)((int)sky->clouds->numLayers, 4);
	UINT size = 512;
	char note[160];
	sprintf_s(note, "%d layers:", count);
	state = note;
	for (int i = 0; i < count; i++) {
		NiGeometry* layer = (NiGeometry*)sky->clouds->layers[i];
		if (!layer) { state += " (none)"; continue; }
		SkyShaderProperty* property = layer->propertyState.GetShadeProperty<SkyShaderProperty>();
		if (!property || property->eSkyObjectType != SkyShaderProperty::SO_CLOUDS) { state += " (not a cloud layer)"; continue; }
		state += std::string(" ") + (property->strTextureName.m_data ? property->strTextureName.m_data : "(no name)");
		IDirect3DBaseTexture9* base = D3DTextureOf(property->spBaseTexture);
		if (!base) { state += " (not loaded)"; continue; }
		// The horizon layers (a band of clouds painted to sit low round the sky, e.g. sky\WastelandCloudHorizon01.dds) would tile
		// stripes over the land on the flat layer: left out.
		if (IsHorizonTexture(property->strTextureName.m_data)) { state += " (horizon: left out)"; continue; }
		layerTextures[i] = base;
		IDirect3DBaseTexture9* incoming = D3DTextureOf(property->spBlendTexture);
		blendTextures[i] = incoming ? incoming : base;
		((float*)&Constants.CloudWeight)[i] = Settings.CloudDensity;	// cover = alpha x Density (as Community Shaders' Opacity, up to 4 there)
		((float*)&Constants.CloudBlend)[i] = incoming ? std::clamp(property->fBlendValue, 0.0f, 1.0f) : 0.0f;
		if (base->GetType() == D3DRTYPE_TEXTURE) {
			D3DSURFACE_DESC desc;
			if (SUCCEEDED(((IDirect3DTexture9*)base)->GetLevelDesc(0, &desc))) size = (std::max)(size, (UINT)desc.Width);
		}
		// Each layer drifts with the wind, the higher ones (by the weather's own layer speeds) faster.
		const float layerSpeed = weather ? 0.5f + weather->cloudSpeed[i] / 255.0f : 1.0f;
		const double unitsPerSecond = Settings.CloudSpeed * 700.0 * (0.25 + windSpeed) * layerSpeed;
		const double move = unitsPerSecond * dt / Settings.CloudTile;
		drift[i][0] = fmod(drift[i][0] + cos((double)windAngle) * move, 64.0);
		drift[i][1] = fmod(drift[i][1] + sin((double)windAngle) * move, 64.0);
		cloudsReady = true;
	}
	if (!cloudsReady) return;
	for (int i = 0; i < 4; i++) {
		float* offset = (float*)&Constants.CloudOffset[i / 2];
		offset[(i & 1) * 2] = (float)drift[i][0] + 0.37f * i;   // each layer from its own spot of its texture
		offset[(i & 1) * 2 + 1] = (float)drift[i][1] + 0.61f * i;
	}
	Constants.Data.x = Settings.CloudStrength * fade;
	// The mip level whose texels are about the shadow's own blur: the sun's disc (half a degree) seen from the cloud layer,
	// widened twice (cloud edges are soft).
	const float blur = Settings.CloudHeight * 0.0093f * 2.0f;
	const float texel = Settings.CloudTile / (float)size;
	Constants.Clouds = D3DXVECTOR4(Settings.CloudHeight, 1.0f / Settings.CloudTile, (std::max)(0.0f, log2f((std::max)(blur / texel, 1.0f))), 0.0f);
}

bool SunFarEffect::ShouldRender() {
	if (!TheShaderManager->GameState.isExterior || !Texture || !Surface) return false;
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!shadows || !shadows->Enabled) return false;
	return shadows->TerrainShadows.Active() || Constants.Data.x > 0.0f || Constants.Volume.x > 0.0f || (Constants.Data.y > 0.0f && (shadows->TerrainShadows.Settings.Enabled || Settings.Clouds));
}

// UNOFFICIAL 2026-10-22 HazardLog (test only; work\bench\deep-roadmap\evidence\cloudflicker): the DXVK-only "ground ~17% darker on
// single frames" lead. TESR_SunFarBuffer is this pass's render target AND a texture the sun-shadow composite binds on s4 (and
// AmbientLight / VolumetricFog elsewhere). This pass's own shader declares s4 (TESR_TerrainShadowGround), which SetCT only rebinds
// while the mountain shadows' ground texture exists; with them off, s4 can still hold the target from the previous frame's composite
// while the target is being drawn: a read/write feedback loop D3D9 calls undefined (DXVK tracks it per draw, native drivers ignore
// it). Logged: which stages hold the target before the pass and at its draw, whether the game's texture cache agrees, and the
// states the pass draws with. Bounded: changes only, at most 120 lines, plus a summary every 600 frames.
static void SunFarStageList(IDirect3DDevice9* Device, IDirect3DBaseTexture9* own, UInt32& mask, UInt32& cacheMask, char* out, size_t size) {
	mask = cacheMask = 0;
	out[0] = 0;
	for (UInt32 stage = 0; stage < 16; stage++) {
		IDirect3DBaseTexture9* bound = nullptr;
		Device->GetTexture(stage, &bound);
		if (bound) bound->Release();
		if ((UInt32)TheRenderManager->renderState->unk10A0[stage] == (UInt32)own) cacheMask |= 1u << stage;
		if (bound != own) continue;
		mask |= 1u << stage;
		char item[16];
		sprintf_s(item, " s%u", stage);
		strcat_s(out, size, item);
	}
	if (!mask) strcat_s(out, size, " none");
}

static struct SunFarHazardStats {
	unsigned frames = 0, beforeFrames = 0, drawFrames = 0, cacheOdd = 0, lines = 0;
	UInt32 lastBefore = ~0u, lastDraw = ~0u;
	std::string lastStates;
} hazard;

void SunFarEffect::BeforeDraw(IDirect3DDevice9* Device) {
	if (!Settings.HazardLog || !Texture) return;
	UInt32 mask, cacheMask;
	char stages[128];
	SunFarStageList(Device, Texture, mask, cacheMask, stages, sizeof(stages));
	if (mask) hazard.drawFrames++;
	static const struct { D3DRENDERSTATETYPE state; const char* name; } states[] = {
		{ D3DRS_ZENABLE, "Z" }, { D3DRS_ZWRITEENABLE, "ZWrite" }, { D3DRS_SCISSORTESTENABLE, "Scissor" }, { D3DRS_ALPHABLENDENABLE, "Blend" },
		{ D3DRS_ALPHATESTENABLE, "AlphaTest" }, { D3DRS_STENCILENABLE, "Stencil" }, { D3DRS_CULLMODE, "Cull" }, { D3DRS_COLORWRITEENABLE, "ColorWrite" },
		{ D3DRS_SRGBWRITEENABLE, "sRGBWrite" }, { D3DRS_FILLMODE, "Fill" } };
	std::string text;
	char item[64];
	for (auto& s : states) {
		DWORD value = 0;
		Device->GetRenderState(s.state, &value);
		sprintf_s(item, " %s %lu", s.name, value);
		text += item;
	}
	D3DVIEWPORT9 vp = {};
	RECT scissor = {};
	Device->GetViewport(&vp);
	Device->GetScissorRect(&scissor);
	IDirect3DSurface9* depth = nullptr;
	Device->GetDepthStencilSurface(&depth);
	if (depth) depth->Release();
	sprintf_s(item, " viewport %lu %lu %lux%lu", vp.X, vp.Y, vp.Width, vp.Height);
	text += item;
	sprintf_s(item, " scissor %ld %ld %ld %ld", scissor.left, scissor.top, scissor.right, scissor.bottom);
	text += item;
	text += depth ? " depth bound" : " no depth";
	if ((mask != hazard.lastDraw || text != hazard.lastStates) && hazard.lines < 120) {
		hazard.lines++;
		Logger::Log("SUNFAR HAZARD at draw (frame %u): target on%s; states%s", hazard.frames, stages, text.c_str());
	}
	hazard.lastDraw = mask;
	hazard.lastStates = text;
}

// UNOFFICIAL 2026-11-04 [Shaders.VolumetricClouds.Main] PlantShadows off (user: the bushes should look under the clouds as with them off):
// the factor drawn into Target is put back to 1 by each pixel's plant share, from MaterialBuffer (class 5: the textures the plant wind's
// Keywords name). The buffer is resolved from the game's MSAA as an average, its alpha the share of samples an NVR template wrote: r - 4a
// is a pixel's plant share where plants meet the land or the sky (land 4, plants 5); against other objects (1-3) it reads 0, and skin (6) is
// left out. Only rgb: the sun's share in alpha (the Specular effect's) stays.
static void UnshadePlants(IDirect3DDevice9* Device, IDirect3DSurface9* Target) {
	static IDirect3DPixelShader9* shader = nullptr;
	static IDirect3DStateBlock9* state = nullptr;
	static bool failed = false, told = false;
	IDirect3DTexture9* material = CapturedNormals::MaterialTexture;
	if (failed || !Target || !material || CapturedNormals::MaterialData.x < 0.5f) return;
	D3DSURFACE_DESC t = {}, m = {};
	if (FAILED(Target->GetDesc(&t)) || FAILED(material->GetLevelDesc(0, &m)) || t.Width != m.Width || t.Height != m.Height) return;
	if (!shader) {
		static const char* sourceOld = "sampler2D M : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { float4 c = tex2D(M, uv); "
			"return float4(1, 1, 1, c.r <= 5.5 * c.a ? saturate(c.r - 4.0 * c.a) : 0.0); }";
		// UNOFFICIAL 2026-11-14 GBufferV2 (core/CapturedNormals.h): there a is a normal's y and r may carry the specular bit (+16): plants are
		// the texels whose shading model is 5 (no share at MSAA edges: the old form's r - 4a had one)
		static const char* sourceV2 = "sampler2D M : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { float4 c = tex2D(M, uv); "
			"float k = floor(c.r + 0.01); k -= k >= 16.0 ? 16.0 : 0.0; return float4(1, 1, 1, abs(k - 5.0) < 0.5 ? 1.0 : 0.0); }";
		const char* source = CapturedNormals::V2Compiled ? sourceV2 : sourceOld;
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
			FAILED(Device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &shader))) shader = nullptr;
		if (!shader) Logger::Log("UNOFFICIAL sun far shadows: PlantShadows' pass failed (%s): plants shaded as before", errors ? (const char*)errors->GetBufferPointer() : "?");
		if (errors) errors->Release();
		if (code) code->Release();
		if (!shader) { failed = true; return; }
	}
	if (!state && FAILED(Device->CreateStateBlock(D3DSBT_ALL, &state))) { failed = true; return; }
	state->Capture();
	IDirect3DSurface9* targets[4] = {};
	IDirect3DSurface9* depth = nullptr;
	for (DWORD i = 0; i < 4; i++) Device->GetRenderTarget(i, &targets[i]);
	Device->GetDepthStencilSurface(&depth);
	Device->SetRenderTarget(0, Target);
	for (DWORD i = 1; i < 4; i++) if (targets[i]) Device->SetRenderTarget(i, nullptr);
	Device->SetDepthStencilSurface(nullptr);
	static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
		D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE,
		D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE };
	const DWORD want[] = { D3DZB_FALSE, FALSE, FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, D3DCULL_NONE, 0x7, FALSE, 0, FALSE, FALSE };
	for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) Device->SetRenderState(kStates[i], want[i]);
	const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
	Device->SetViewport(&full);
	Device->SetTexture(0, material);
	Device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	Device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	Device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	Device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
	Device->SetVertexShader(nullptr);
	Device->SetPixelShader(shader);
	Device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const float x0 = -0.5f, y0 = -0.5f, x1 = (float)t.Width - 0.5f, y1 = (float)t.Height - 0.5f;
	const float quad[4][6] = { { x0, y0, 0.0f, 1.0f, 0.0f, 0.0f }, { x1, y0, 0.0f, 1.0f, 1.0f, 0.0f }, { x0, y1, 0.0f, 1.0f, 0.0f, 1.0f }, { x1, y1, 0.0f, 1.0f, 1.0f, 1.0f } };
	Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	for (DWORD i = 0; i < 4; i++) if (i == 0 || targets[i]) Device->SetRenderTarget(i, targets[i]);
	Device->SetDepthStencilSurface(depth);
	state->Apply();	// (the viewport and everything else; SetRenderTarget reset the viewport)
	for (DWORD i = 0; i < 4; i++) if (targets[i]) targets[i]->Release();
	if (depth) depth->Release();
	if (!told) { told = true; Logger::Log("UNOFFICIAL sun far shadows: PlantShadows off: plants keep their sunlight (MaterialBuffer's plant class)"); }
}

void SunFarEffect::RenderFactor(IDirect3DDevice9* Device, IDirect3DSurface9* Scene) {
	Constants.Apply.x = 0.0f;
	VolumetricCloudsEffect* volumetric = TheShaderManager->Effects.VolumetricClouds;
	if (volumetric) volumetric->shadowsViaSunFar = false;
	if (!Enabled || !Effect || !ShouldRender()) return;
	CapturedNormals::SunFarDrew = true;	// UNOFFICIAL 2026-11-17 fast-restart: the grass's own normal for this pass from the next frame (CapturedNormals.h)
	static GpuTimer timer("Sun far shadows (land, clouds)");
	GpuProfileScope gpu(timer, Device);
	// The cloud layers on stages 5-12 (NVR_CloudLayer*, NVR_CloudBlend*), set after the pass begins (EffectRecord::PassTextures).
	for (int i = 0; i < 4; i++) {
		SetPassTexture(5 + i, layerTextures[i]);
		SetPassTexture(9 + i, blendTextures[i]);
	}
	// UNOFFICIAL 2026-10-21: the volumetric clouds' shadow map on stage 13 (NVR_VolumeCloudShadow), drawn for this frame first.
	// UNOFFICIAL 2026-10-30: the RT sun shadows took the volumetric clouds' shadow into the sun visibility this frame ([Shaders.ShadowsExteriors.RT]
	// CloudShadows): not here as well (the strength put back after the pass)
	const float volumeStrength = Constants.Volume.x;
	if (RtSunShadows::CloudsApplied()) Constants.Volume.x = 0.0f;
	struct PutBack { float& v; float s; ~PutBack() { v = s; } } putBack{ Constants.Volume.x, volumeStrength };
	const bool volume = volumetric && Constants.Volume.x > 0.0f;
	if (volume) {
		volumetric->RenderShadowMap(Device);
		SetPassTexture(13, volumetric->Textures.ShadowTexture);
	}
	// UNOFFICIAL 2026-10-23 WaterShadows: the game's water depth map on stage 14 (NVR_WaterDepth). Without it water is left
	// alone, as with the option off.
	// UNOFFICIAL 2026-10-25: the world as drawn on stage 15 (NVR_Scene), for the share of each pixel the object and land shaders
	// filled with the game's fog (left out of the shadow: it is not sunlight). The scene target's own texture, unchanged since the
	// world was drawn (the passes before this one draw into their own targets).
	IDirect3DTexture9* sceneTexture = nullptr;
	if (Scene && FAILED(Scene->GetContainer(IID_IDirect3DTexture9, (void**)&sceneTexture))) sceneTexture = nullptr;
	if (sceneTexture) SetPassTexture(15, sceneTexture);
	IDirect3DTexture9* waterDepth = Settings.WaterFade || Settings.DebugView == 3 ? WaterDepthMap() : nullptr;
	Constants.Water = waterDepth ? D3DXVECTOR4(Settings.WaterFade ? 1.0f : 0.0f, Settings.WaterDeepShare, 1.0f, 0.0f) : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	Constants.Water.w = Settings.OldSunModel || !sceneTexture ? 1.0f : 0.0f;	// UNOFFICIAL 2026-10-25: 1 = the light split as before (SunFar.fx.hlsl)
	if (waterDepth) SetPassTexture(14, waterDepth);
	if (Settings.HazardLog) {
		hazard.frames++;
		UInt32 mask, cacheMask;
		char stages[128];
		SunFarStageList(Device, Texture, mask, cacheMask, stages, sizeof(stages));
		if (mask) hazard.beforeFrames++;
		if (mask != cacheMask) hazard.cacheOdd++;
		if (mask != hazard.lastBefore && hazard.lines < 120) {
			hazard.lines++;
			Logger::Log("SUNFAR HAZARD before the pass (frame %u): target on%s (game cache mask %04X; mountain shadows' ground %s; fix %s)",
				hazard.frames, stages, cacheMask, TheShaderManager->Effects.ShadowsExteriors->TerrainShadows.GroundTexture ? "bound on s4" : "missing: s4 not rebound",
				Settings.TargetFix ? "on" : "off");
		}
		hazard.lastBefore = mask;
		if (hazard.frames % 600 == 0)
			Logger::Log("SUNFAR HAZARD summary: %u frames, target bound before the pass in %u, at the draw in %u, device/cache disagree in %u (fix %s)",
				hazard.frames, hazard.beforeFrames, hazard.drawFrames, hazard.cacheOdd, Settings.TargetFix ? "on" : "off");
	}
	// UNOFFICIAL 2026-10-22 TargetFix: the target off every stage it is still bound on (the composites leave it on s4), so the
	// pass never samples what it draws, and full-screen states whatever the game left (put back after).
	static const D3DRENDERSTATETYPE fixStates[7] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_ALPHABLENDENABLE,
		D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_CULLMODE };
	static const DWORD fixValues[7] = { D3DZB_FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE };
	DWORD savedStates[7] = {};
	if (Settings.TargetFix) {
		for (UInt32 stage = 0; stage < 16; stage++) {
			IDirect3DBaseTexture9* bound = nullptr;
			Device->GetTexture(stage, &bound);
			if (bound) bound->Release();
			if (bound != Texture) continue;
			Device->SetTexture(stage, nullptr);
			TheRenderManager->renderState->NoteDeviceTexture(stage, nullptr);
		}
		for (int i = 0; i < 7; i++) {
			Device->GetRenderState(fixStates[i], &savedStates[i]);
			Device->SetRenderState(fixStates[i], fixValues[i]);
		}
	}
	TheShaderManager->RenderEffectToRT(Surface, this, false, 0);
	if (!Settings.PlantShadows && (Settings.DebugView == 0 || Settings.DebugView == 4)) UnshadePlants(Device, Surface);
	if (Settings.TargetFix)
		for (int i = 0; i < 7; i++) Device->SetRenderState(fixStates[i], savedStates[i]);
	ClearPassTextures();
	if (sceneTexture) sceneTexture->Release();
	Constants.Apply.x = 1.0f;
	if (volume) volumetric->shadowsViaSunFar = true;
	static int waterReported = -1;
	if ((Settings.WaterFade ? (waterDepth ? 1 : 0) : 2) != waterReported) {
		waterReported = Settings.WaterFade ? (waterDepth ? 1 : 0) : 2;
		D3DSURFACE_DESC desc = {};
		if (waterDepth) waterDepth->GetLevelDesc(0, &desc);
		Logger::Log("UNOFFICIAL sun far shadows on water: %s (depth map %p %ux%u format %u)", waterReported == 2 ? "left alone (WaterShadows off)" :
			waterReported == 1 ? "faded in by the water's depth" : "left alone: the game has no water depth map", waterDepth, desc.Width, desc.Height, (unsigned)desc.Format);
	}
	static bool reported = false;
	if (!reported) {
		reported = true;
		Logger::Log("UNOFFICIAL sun far shadows: drawn (land %s, clouds %s, volumetric clouds %s); the sun-shadow composites take the factor",
			TheShaderManager->Effects.ShadowsExteriors->TerrainShadows.Active() ? "on" : "off", Constants.Data.x > 0.0f ? "on" : "off", volume ? "on" : "off");
	}
}

// --- The Vulkan pass (NVR DXVK) -----------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-25 (NVR DXVK): SunFar.frag, the technique's one pass line for line, with the Includes it reads. Its constants
// go to a uniform buffer each frame (about 1 KB; push data holds 240 bytes), read from the same registered memory the D3D9 pass
// uploads (ConstantsTable, as Snow.cpp reads them). Its inputs are the textures the D3D9 pass binds (the named TESR_ textures
// through their texture slots, as SetCT finds them) and this frame's cloud layers, volumetric shadow map and water depth map.
// A required input or a missing constant leaves the frame to the D3D9 technique. A cloud layer, volumetric map or water map that
// is not there gets a stand-in: its weight, strength or share is zero then, so the pass never reads it.
namespace {
	// The Constants block of SunFar.frag (std140: vec4s and mat4s only, in this order; the matrices as D3DX stores them).
	struct SunFarUniforms {
		D3DXVECTOR4	waterSettings;
		D3DXVECTOR4	shadowFade;
		D3DXVECTOR4	shadowSunLight;
		D3DXVECTOR4	shadowAmbientLight;
		D3DXVECTOR4	pbrData;
		D3DXVECTOR4	pbrExtraData;
		D3DXVECTOR4	terrainData;
		D3DXVECTOR4	terrainSkyData;
		D3DXVECTOR4	skyIrradiance[9];
		D3DXVECTOR4	skyIrradianceTerrain[9];
		D3DXVECTOR4	terrainShadowData;
		D3DXVECTOR4	terrainShadowRange;
		D3DXVECTOR4	terrainShadowParams;
		D3DXVECTOR4	sunFarData;
		D3DXVECTOR4	sunFarClouds;
		D3DXVECTOR4	sunFarCloudOffset[2];
		D3DXVECTOR4	sunFarCloudWeight;
		D3DXVECTOR4	sunFarCloudBlend;
		D3DXVECTOR4	sunFarVolume;
		D3DXVECTOR4	sunDirection;
		D3DXVECTOR4	sunFarWater;
		D3DXVECTOR4	fogData;		// UNOFFICIAL 2026-10-25 (the fog's share, the land's light vector)
		D3DXVECTOR4	fogColor;
		D3DXVECTOR4	sunFarLand;
		D3DXVECTOR4	cameraData;
		D3DXVECTOR4	cameraPosition;
		D3DXVECTOR4	smoothedSunDir;
		D3DXVECTOR4	shadowFormatData;
		D3DXVECTOR4	shadowBlur;
		D3DXVECTOR4	shadowNearCenter;
		D3DXVECTOR4	shadowMiddleCenter;
		D3DXVECTOR4	shadowFarCenter;
		D3DXVECTOR4	shadowLodCenter;
		D3DXMATRIX	shadowToLightNear;
		D3DXMATRIX	shadowToLightMiddle;
		D3DXMATRIX	shadowToLightFar;
		D3DXMATRIX	shadowToLightLod;
		D3DXMATRIX	projectionTransform;
		D3DXMATRIX	viewTransform;
	};
	// The 16 sampled images (set 1, binding = slot), the constants (set 2, binding 0) and the three samplers (the sampler heap, set 0,
	// binding 0): DXVK writes each sampler's heap index at its push offset.
	const NVR_EXT_BINDING_DESC SunFarBindings[] = {
		{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_depth: TESR_DepthBuffer
		{ 1, 1, NVR_EXT_BIND_SAMPLED_IMAGE,  1, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_normals: TESR_NormalsBuffer
		{ 1, 2, NVR_EXT_BIND_SAMPLED_IMAGE,  2, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_atlas: TESR_ShadowAtlas
		{ 1, 3, NVR_EXT_BIND_SAMPLED_IMAGE,  3, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_terrainShadow: TESR_TerrainShadowBuffer
		{ 1, 4, NVR_EXT_BIND_SAMPLED_IMAGE,  4, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_terrainGround: TESR_TerrainShadowGround
		{ 1, 5, NVR_EXT_BIND_SAMPLED_IMAGE,  5, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_layer0..3: NVR_CloudLayer0..3
		{ 1, 6, NVR_EXT_BIND_SAMPLED_IMAGE,  6, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 1, 7, NVR_EXT_BIND_SAMPLED_IMAGE,  7, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 1, 8, NVR_EXT_BIND_SAMPLED_IMAGE,  8, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 1, 9, NVR_EXT_BIND_SAMPLED_IMAGE,  9, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_blend0..3: NVR_CloudBlend0..3
		{ 1, 10, NVR_EXT_BIND_SAMPLED_IMAGE, 10, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 1, 11, NVR_EXT_BIND_SAMPLED_IMAGE, 11, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 1, 12, NVR_EXT_BIND_SAMPLED_IMAGE, 12, NVR_EXT_VIEW_2D, FALSE, 0 },
		{ 1, 13, NVR_EXT_BIND_SAMPLED_IMAGE, 13, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_volume: NVR_VolumeCloudShadow
		{ 1, 14, NVR_EXT_BIND_SAMPLED_IMAGE, 14, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_water: NVR_WaterDepth
		{ 1, 15, NVR_EXT_BIND_SAMPLED_IMAGE, 15, NVR_EXT_VIEW_2D, FALSE, 0 },	// t_scene: NVR_Scene (UNOFFICIAL 2026-10-25)
		{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D, FALSE, 0 },	// Constants
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        0, NVR_EXT_VIEW_2D, FALSE, 0 },	// samplerPoint: push offset 0
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        1, NVR_EXT_VIEW_2D, FALSE, 4 },	// samplerLinear: push offset 4
		{ 0, 0, NVR_EXT_BIND_SAMPLER,        2, NVR_EXT_VIEW_2D, FALSE, 8 },	// samplerMip: push offset 8
	};
	struct SunFarPush { UInt32 samplerPoint; UInt32 samplerLinear; UInt32 samplerMip; UInt32 pad; };
}

// The registered constant's memory (ConstantsTable: the pointers SetCT uploads from) copied into the block; false if not registered.
static bool CopyRegistered(void* to, size_t bytes, const char* name) {
	auto found = TheShaderManager->ConstantsTable.find(name);
	if (found == TheShaderManager->ConstantsTable.end() || !found->second) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			Logger::Log("UNOFFICIAL Vegas Shaders DXVK: SunFar: %s is not registered; the D3D9 technique is used", name);
		}
		return false;
	}
	memcpy(to, found->second, bytes);
	return true;
}

// A named TESR_ texture as the D3D9 pass binds it (its TextureManager slot, as SetCT's BindTexture finds it); null if none.
static IDirect3DBaseTexture9* NamedTexture(const char* name) {
	std::string text = name;
	IDirect3DBaseTexture9** slot = TheTextureManager->GetTextureSlotByName(text);
	return slot ? *slot : nullptr;
}

bool SunFarEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0 || !Texture || !Surface) return false;
	if (Settings.JitterFix || Settings.SkipCascadeCheck) return false;	// UNOFFICIAL 2026-10-31: only the D3D9 shader has them
	if (!extPass.Tried) {
		extPass.Tried = true;
		extPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("SunFar", SunFarBindings, _countof(SunFarBindings), sizeof(SunFarPush), true);
	}
	ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
	ID3D9NvrExtSampler* linear = DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* mip = DxvkCompute::MipSampler(false, NVR_EXT_ADDRESS_WRAP);	// trilinear, all mips: the cloud layers (MIPFILTER LINEAR)
	if (!extPass.Pipeline || !point || !linear || !mip) return false;

	// The inputs, as the D3D9 pass binds them. The terrain's two maps are required while the land's shadow is on.
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	IDirect3DBaseTexture9* depth = NamedTexture("TESR_DepthBuffer");
	IDirect3DBaseTexture9* normals = NamedTexture("TESR_NormalsBuffer");
	IDirect3DBaseTexture9* atlas = NamedTexture("TESR_ShadowAtlas");
	IDirect3DBaseTexture9* terrain = NamedTexture("TESR_TerrainShadowBuffer");
	IDirect3DBaseTexture9* ground = NamedTexture("TESR_TerrainShadowGround");
	if (!depth || !normals || !atlas) return false;
	if (shadows && shadows->TerrainShadows.Active() && (!terrain || !ground)) return false;
	// This frame's maps: the volumetric clouds' shadow (RenderFactor drew it), the water's depth (Constants.Water.z: bound).
	VolumetricCloudsEffect* volumetric = TheShaderManager->Effects.VolumetricClouds;
	IDirect3DBaseTexture9* volume = volumetric && Constants.Volume.x > 0.0f ? volumetric->Textures.ShadowTexture : nullptr;
	IDirect3DBaseTexture9* water = Constants.Water.z > 0.0f ? WaterDepthMap() : nullptr;
	// The stand-ins (the depth buffer) are never sampled where their input is absent: see the comment above the bindings.
	IDirect3DBaseTexture9* inputs[16] = { depth, normals, atlas, terrain ? terrain : depth, ground ? ground : depth };
	// UNOFFICIAL 2026-10-25: the world as drawn, which RenderFactor puts on stage 15 for the D3D9 pass (Constants.Water.w 0 then).
	inputs[15] = PassTextures[15] ? PassTextures[15] : depth;
	for (int i = 0; i < 4; i++) {
		inputs[5 + i] = layerTextures[i] ? layerTextures[i] : depth;
		inputs[9 + i] = blendTextures[i] ? blendTextures[i] : depth;
	}
	inputs[13] = volume ? volume : depth;
	inputs[14] = water ? water : depth;

	// The constants, as the D3D9 pass uploads them this frame.
	SunFarUniforms uniforms = {};
	bool registered = true;
	auto copy = [&](void* to, size_t bytes, const char* name) { registered = CopyRegistered(to, bytes, name) && registered; };
	copy(&uniforms.waterSettings, sizeof(uniforms.waterSettings), "TESR_WaterSettings");
	copy(&uniforms.shadowFade, sizeof(uniforms.shadowFade), "TESR_ShadowFade");
	copy(&uniforms.shadowSunLight, sizeof(uniforms.shadowSunLight), "TESR_ShadowSunLight");
	copy(&uniforms.shadowAmbientLight, sizeof(uniforms.shadowAmbientLight), "TESR_ShadowAmbientLight");
	copy(&uniforms.pbrData, sizeof(uniforms.pbrData), "TESR_PBRData");
	copy(&uniforms.pbrExtraData, sizeof(uniforms.pbrExtraData), "TESR_PBRExtraData");
	copy(&uniforms.terrainData, sizeof(uniforms.terrainData), "TESR_TerrainData");
	copy(&uniforms.terrainSkyData, sizeof(uniforms.terrainSkyData), "TESR_TerrainSkyData");
	copy(&uniforms.skyIrradiance, sizeof(uniforms.skyIrradiance), "TESR_SkyIrradianceObjects");
	copy(&uniforms.skyIrradianceTerrain, sizeof(uniforms.skyIrradianceTerrain), "TESR_SkyIrradianceTerrain");
	copy(&uniforms.terrainShadowData, sizeof(uniforms.terrainShadowData), "TESR_TerrainShadowData");
	copy(&uniforms.terrainShadowRange, sizeof(uniforms.terrainShadowRange), "TESR_TerrainShadowRange");
	copy(&uniforms.terrainShadowParams, sizeof(uniforms.terrainShadowParams), "TESR_TerrainShadowParams");
	copy(&uniforms.sunFarData, sizeof(uniforms.sunFarData), "TESR_SunFarData");
	copy(&uniforms.sunFarClouds, sizeof(uniforms.sunFarClouds), "TESR_SunFarClouds");
	copy(&uniforms.sunFarCloudOffset, sizeof(uniforms.sunFarCloudOffset), "TESR_SunFarCloudOffset");
	copy(&uniforms.sunFarCloudWeight, sizeof(uniforms.sunFarCloudWeight), "TESR_SunFarCloudWeight");
	copy(&uniforms.sunFarCloudBlend, sizeof(uniforms.sunFarCloudBlend), "TESR_SunFarCloudBlend");
	copy(&uniforms.sunFarVolume, sizeof(uniforms.sunFarVolume), "TESR_SunFarVolume");
	copy(&uniforms.sunDirection, sizeof(uniforms.sunDirection), "TESR_SunDirection");
	copy(&uniforms.sunFarWater, sizeof(uniforms.sunFarWater), "TESR_SunFarWater");
	copy(&uniforms.fogData, sizeof(uniforms.fogData), "TESR_FogData");
	copy(&uniforms.fogColor, sizeof(uniforms.fogColor), "TESR_FogColor");
	copy(&uniforms.sunFarLand, sizeof(uniforms.sunFarLand), "TESR_SunFarLand");
	copy(&uniforms.cameraData, sizeof(uniforms.cameraData), "TESR_CameraData");
	copy(&uniforms.cameraPosition, sizeof(uniforms.cameraPosition), "TESR_CameraPosition");
	copy(&uniforms.smoothedSunDir, sizeof(uniforms.smoothedSunDir), "TESR_SmoothedSunDir");
	copy(&uniforms.shadowFormatData, sizeof(uniforms.shadowFormatData), "TESR_ShadowFormatData");
	copy(&uniforms.shadowBlur, sizeof(uniforms.shadowBlur), "TESR_ShadowBlur");
	copy(&uniforms.shadowNearCenter, sizeof(uniforms.shadowNearCenter), "TESR_ShadowNearCenter");
	copy(&uniforms.shadowMiddleCenter, sizeof(uniforms.shadowMiddleCenter), "TESR_ShadowMiddleCenter");
	copy(&uniforms.shadowFarCenter, sizeof(uniforms.shadowFarCenter), "TESR_ShadowFarCenter");
	copy(&uniforms.shadowLodCenter, sizeof(uniforms.shadowLodCenter), "TESR_ShadowLodCenter");
	copy(&uniforms.shadowToLightNear, sizeof(uniforms.shadowToLightNear), "TESR_ShadowCameraToLightTransformNear");
	copy(&uniforms.shadowToLightMiddle, sizeof(uniforms.shadowToLightMiddle), "TESR_ShadowCameraToLightTransformMiddle");
	copy(&uniforms.shadowToLightFar, sizeof(uniforms.shadowToLightFar), "TESR_ShadowCameraToLightTransformFar");
	copy(&uniforms.shadowToLightLod, sizeof(uniforms.shadowToLightLod), "TESR_ShadowCameraToLightTransformLod");
	copy(&uniforms.projectionTransform, sizeof(uniforms.projectionTransform), "TESR_ProjectionTransform");
	copy(&uniforms.viewTransform, sizeof(uniforms.viewTransform), "TESR_ViewTransform");
	if (!registered) return false;
	ID3D9NvrExtBuffer* constants = ExtUniforms(extUniforms, &uniforms, sizeof(uniforms));
	if (!constants) return false;

	NVR_EXT_RESOURCE_BINDING resources[_countof(SunFarBindings)] = {};
	for (UINT i = 0; i < 16; i++) { resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = inputs[i]; }
	resources[16].Slot = 0; resources[16].Type = NVR_EXT_BIND_UNIFORM_BUFFER; resources[16].pResource = constants;
	resources[17].Slot = 0; resources[17].Type = NVR_EXT_BIND_SAMPLER; resources[17].pResource = point;
	resources[18].Slot = 1; resources[18].Type = NVR_EXT_BIND_SAMPLER; resources[18].pResource = linear;
	resources[19].Slot = 2; resources[19].Type = NVR_EXT_BIND_SAMPLER; resources[19].pResource = mip;
	SunFarPush push = {};	// the sampler heap indices: DXVK fills them in
	IDirect3DSurface9* target = Surface;
	if (!ExtDrawTo(extPass.Pipeline, &target, 1, resources, _countof(resources), &push, sizeof(push), nullptr, ExtUv::ScreenQuad)) return false;	// FrameVS into a smaller target
	if (++ExtFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK: %s runs as a Vulkan pass.", Name);
	return true;
}
