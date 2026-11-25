// UNOFFICIAL 2026-11-03: dust motes in lit air (DustMotes.h, docs/EFFECTS.md "DustMotes").

#include <algorithm>
#include <cmath>

#include "DustMotes.h"
#include "../../core/FrameClock.h"
#include "../../core/RenderRect.h"
#include "../../core/PixelInspector.h"

// The cells the specks live in: one speck at most per cell, so the cell size sets how close two specks can come and how many cells
// a view crosses (the cost). 40 units is about 57 cm. The pattern repeats every CellRepeat cells (2560 units) on each axis: the hash is taken of the cell number modulo the repeat,
// so the drift can wrap without the specks jumping, and the camera position the shader sees stays small (full float precision).
static const float CellSize = 40.0f;
static const double CellRepeat = 64.0;
// The air's drift direction (mostly sideways, rising a little, as warm room air does) and its speed at Drift 1, units a second.
static const double WindX = 0.84, WindY = 0.50, WindZ = 0.20, WindSpeed = 5.0;
// The wobble clock's rate at Drift 1, cycles a second (each speck wobbles at 1, 2 or 3 times it, every 4 to 12 seconds; the twinkle
// at 2 to 5 times). The clock wraps at 1: every speck's rate is a whole multiple of it, so the wrap does not show.
static const double WobbleRate = 0.08;

void DustMotesEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_DustMotesData", &Constants.Data);
	sm->RegisterConstant("TESR_DustMotesLook", &Constants.Look);
	sm->RegisterConstant("TESR_DustMotesOrigin", &Constants.Origin);
	sm->RegisterConstant("TESR_DustMotesSun", &Constants.Sun);
	sm->RegisterConstant("TESR_DustMotesLampPosition", Constants.LampPosition);
	sm->RegisterConstant("TESR_DustMotesLampColor", Constants.LampColor);
}

void DustMotesEffect::RegisterTextures() {
	// Half resolution: the walk through the cells is the expensive part. A speck is drawn at least about one half-resolution pixel
	// wide (dimmer when it is smaller, so its light is kept), and the full-resolution pass reads the buffer through a bilinear
	// sampler, so a speck shows as a soft dot of about two to three screen pixels.
	const UINT w = MaxQuality::Reduced(TheRenderManager->width, 2);
	const UINT h = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("TESR_DustMotesBuffer", &Textures.BufferTexture, &Textures.BufferSurface, w, h, D3DFMT_A16B16G16R16F);
}

void DustMotesEffect::UpdateSettings() {
	// UpdateConstants only runs while the effect is enabled: clear the gate here so switching it off cannot leave it standing.
	Constants.Data.x = 0.0f;

	const char* section = "Shaders.DustMotes.Main";
	SettingManager* cfg = TheSettingManager;
	Settings.Density = std::clamp(cfg->GetSettingF(section, "Density"), 0.0f, 1.0f);
	Settings.Size = std::clamp(cfg->GetSettingF(section, "Size"), 0.005f, 1.0f);
	Settings.Brightness = (std::max)(0.0f, cfg->GetSettingF(section, "Brightness"));
	Settings.MaxDistance = std::clamp(cfg->GetSettingF(section, "MaxDistance"), 100.0f, 1000.0f);
	Settings.Drift = std::clamp(cfg->GetSettingF(section, "Drift"), 0.0f, 10.0f);
	Settings.Glint = std::clamp(cfg->GetSettingF(section, "Glint"), 0.0f, 0.9f);
	Settings.SunShare = (std::max)(0.0f, cfg->GetSettingF(section, "SunShare"));
	Settings.Interiors = cfg->GetSettingI(section, "Interiors") != 0;
	Settings.Exteriors = cfg->GetSettingI(section, "Exteriors") != 0;
	Settings.DebugView = std::clamp(cfg->GetSettingI(section, "DebugView"), 0, 2);
}

void DustMotesEffect::UpdateConstants() {
	// The drift and the wobble advance once a frame (UpdateConstants runs twice a frame; FrameClock.h).
	const double dt = ElapsedOncePerFrame(lastFrameTime) * Settings.Drift;
	const double repeat = CellRepeat * CellSize;
	auto wrap = [](double v, double period) { v = fmod(v, period); return v < 0.0 ? v + period : v; };
	windX = wrap(windX + WindX * WindSpeed * dt, repeat);
	windY = wrap(windY + WindY * WindSpeed * dt, repeat);
	windZ = wrap(windZ + WindZ * WindSpeed * dt, repeat);
	wobble = wrap(wobble + WobbleRate * dt, 1.0);

	const auto& state = TheShaderManager->GameState;
	const bool here = state.isExterior ? Settings.Exteriors : Settings.Interiors;
	const bool on = Enabled && here && !state.isUnderwater && Settings.Density > 0.0f && Settings.Brightness > 0.0f;
	Constants.Data = D3DXVECTOR4(on ? Settings.Brightness : 0.0f, Settings.Size, Settings.MaxDistance, Settings.Density);
	Constants.Look.x = Settings.Glint;
	Constants.Look.y = CellSize;
	Constants.Look.z = (float)wobble;

	// The camera in the drifting cells' frame: the cells move with the air, so the camera's place among them is its world position
	// minus the drift, wrapped at the pattern's repeat (doubles: world positions reach some 100000 units).
	const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
	Constants.Origin.x = (float)wrap((double)eye.x - windX, repeat);
	Constants.Origin.y = (float)wrap((double)eye.y - windY, repeat);
	Constants.Origin.z = (float)wrap((double)eye.z - windZ, repeat);
}

// The lamps that can light a speck this frame, from the list ShaderManager::GetNearbyLights made this frame (as LampHaze and
// AmbientLight's grid take it): every shadow slot's lamp (BatchLightPosition / Color, all 64 slots: indoors with forward lamp
// shadows the 12-entry TESR_ShadowLightPosition the screen passes read can be empty) and the tracked unshadowed ones (LightPosition,
// LightColor from 12). Their w is the game's reach times [Shaders.ShadowsInteriors.Main] LightRadiusMult (the cube maps' reach); the
// specks take the game's own reach back, so they are lit as far as the lamp lights the walls. A lamp in both lists counts once.
// Kept: those whose reach overlaps the drawn distance round the eye, the brightest-looking first (linear luminance times how much of
// the drawn distance the reach covers), camera-relative.
void DustMotesEffect::PickLamps() {
	struct Candidate { D3DXVECTOR4 position, color; float score; };
	Candidate found[ShadowCubeMapsMax + TrackedLightsMax];
	int count = 0;
	ShaderManager* sm = TheShaderManager;
	const float radiusMult = (std::max)(sm->Effects.ShadowsExteriors->Settings.Interiors.LightRadiusMult, 0.01f);
	const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
	const float reachOut = Settings.MaxDistance;
	seenLamps = 0;
	auto consider = [&](const D3DXVECTOR4& lamp, const D3DXVECTOR4& color) {
		const float radius = lamp.w / radiusMult;
		if (!(radius > 10.0f) || !(color.w > 0.0f)) return;
		const D3DXVECTOR3 rel(lamp.x - eye.x, lamp.y - eye.y, lamp.z - eye.z);
		for (int i = 0; i < count; i++)	// the same lamp in both lists
			if (fabsf(found[i].position.x - rel.x) < 1.0f && fabsf(found[i].position.y - rel.y) < 1.0f && fabsf(found[i].position.z - rel.z) < 1.0f) return;
		seenLamps++;
		const float distance = D3DXVec3Length(&rel);
		if (distance - radius > reachOut) return;	// its light ends before the farthest speck drawn
		// Linear colour (the game's colours are gamma-encoded like the frame) times the dimmer. Negative ("darkness") lamps and
		// unlit ones light no dust.
		const float dimmer = color.w;
		const D3DXVECTOR4 linear(powf((std::max)(color.x, 0.0f), 2.2f) * dimmer, powf((std::max)(color.y, 0.0f), 2.2f) * dimmer,
			powf((std::max)(color.z, 0.0f), 2.2f) * dimmer, 0.0f);
		const float luminance = 0.2126f * linear.x + 0.7152f * linear.y + 0.0722f * linear.z;
		if (!(luminance > 1.0e-4f)) return;
		const float cover = std::clamp((radius - (std::max)(0.0f, distance - reachOut)) / radius, 0.0f, 1.0f);
		found[count++] = { D3DXVECTOR4(rel.x, rel.y, rel.z, radius), linear, luminance * (0.25f + cover) };
	};
	for (int s = 0; s < ShadowCubeMapsMax; s++) consider(sm->BatchLightPosition[s], sm->BatchLightColor[s]);
	for (int t = 0; t < TrackedLightsMax; t++) consider(sm->LightPosition[t], sm->LightColor[ShadowCubeMapsScreen + t]);
	std::sort(found, found + count, [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
	const int used = (std::min)(count, LampsMax);
	for (int i = 0; i < LampsMax; i++) {
		Constants.LampPosition[i] = i < used ? found[i].position : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		Constants.LampColor[i] = i < used ? found[i].color : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	Constants.Look.w = (float)used;
}

bool DustMotesEffect::ShouldRender() {
	// With a DebugView it draws whenever the effect is on here, so an empty lamp pick still shows (as nothing lit).
	return Constants.Data.x > 0.0f && Textures.BufferSurface && (Constants.Look.w > 0.0f || Constants.Sun.x > 0.0f || Constants.Sun.y > 0.0f);
}

// Every 5 seconds while the effect is on for this cell type: the lamps it saw and picked, each picked one's distance, reach and
// colour, the sun share, and whether the passes ran (and if not, why).
void DustMotesEffect::LogFrame(const char* outcome) {
	const double now = TheFrameRateManager->Time;
	if (lastLogTime >= 0.0 && now - lastLogTime < 5.0) return;
	lastLogTime = now;
	char lamps[640] = "";
	int used = 0;
	for (int i = 0; i < (int)Constants.Look.w && used < (int)sizeof(lamps) - 80; i++) {
		const D3DXVECTOR4& l = Constants.LampPosition[i];
		const D3DXVECTOR4& c = Constants.LampColor[i];
		const int n = sprintf_s(lamps + used, sizeof(lamps) - used, " [%.0f away, reach %.0f, rgb %.2f %.2f %.2f]",
			sqrtf(l.x * l.x + l.y * l.y + l.z * l.z), l.w, c.x, c.y, c.z);
		if (n <= 0) break;
		used += n;
	}
	Logger::Log("UNOFFICIAL DustMotes: %s; %d lamp(s) seen, %d picked (MaxDistance %.0f, Brightness %.2f, Density %.2f, sun %.2f, DebugView %d, pixel %.5f rad, chain %s)%s",
		outcome, seenLamps, (int)Constants.Look.w, Settings.MaxDistance, Constants.Data.x, Settings.Density, Constants.Sun.x, Settings.DebugView,
		Constants.Origin.w, TheShaderManager->Chain.IsActive() ? "on" : "off", lamps);
}

void DustMotesEffect::RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool sunGrid) {
	if (!Enabled || !Effect) { NoteSkip(); return; }
	// The lamps of this frame (GetNearbyLights ran with the shadow pass, after UpdateConstants), and the sun: only outdoors, only
	// where the volumetric light's froxel grid was built this frame (it says which air the sun reaches), only while its shafts
	// show at all (their fade: sun shadows on, not raining).
	PickLamps();
	const VolumetricLightEffect* shafts = TheShaderManager->Effects.VolumetricLight;
	const float shaftFade = shafts ? shafts->Constants.Data2.z : 0.0f;
	Constants.Sun.x = sunGrid && TheShaderManager->GameState.isExterior ? Settings.SunShare * shaftFade : 0.0f;
	Constants.Sun.y = (float)Settings.DebugView;
	// One buffer pixel's angle (vertical; pixels are square): the view's height in tangent units over the rows drawn. Here, not in
	// UpdateConstants, because a RenderScale rect frame (fewer rows drawn) has begun by now.
	const float rows = (float)RenderRect::Height(MaxQuality::Reduced(TheRenderManager->height, 2));
	const float m22 = TheRenderManager->projMatrix._22;
	Constants.Origin.w = m22 > 0.0f && rows > 0.0f ? 2.0f / (m22 * rows) : 0.001f;
	if (!ShouldRender()) {
		NoteSkip();
		if (Constants.Data.x > 0.0f) LogFrame("not drawn (no lamp reaches the drawn distance, no sun)");
		return;
	}

	// The additive pass blends into the chain's current image in place, so it needs the copy-free chain (always on in practice).
	FrameChain& chain = TheShaderManager->Chain;
	if (!chain.IsActive()) {
		if (!saidNoChain) Logger::Log("UNOFFICIAL DustMotes: the effect chain is not active this frame (legacy copies): not drawn");
		saidNoChain = true;
		NoteSkip("the effect chain is not active");
		return;
	}

	// 1. The specks' light at half resolution into Buffer (every pixel written: no clear).
	TheShaderManager->RenderEffectToRT(Textures.BufferSurface, this, false, 0);
	// 2. Added to the current image where a speck is (additive blending, alpha kept; the pass reads only Buffer). The rendered
	// surface is passed as null so Render draws straight into the target instead of through the chain's ping-pong. DebugView 1
	// draws the buffer over the picture instead (technique 2, no blending).
	IDirect3DSurface9* image = TheTextureManager->RenderedSurface;
	Device->SetRenderTarget(0, image);
	Render(Device, image, nullptr, Settings.DebugView == 1 ? 2 : 1, false, nullptr);
	PixelInspector::AfterFrame(this);
	Device->SetRenderTarget(0, RenderTarget);
	LogFrame("drawn");
}
