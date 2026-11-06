#include "Rain.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include "../../core/FrameClock.h"
#include "../../core/RenderRect.h"

#include <algorithm>
#include <cmath>

void RainEffect::UpdateConstants() {
	Animator& fall = Constants.RainAnimator;
	const bool raining = TheShaderManager->GameState.isRainy;

	if (raining && !fall.switched) {
		// rain has just begun: fade the drops in
		fall.switched = true;
		fall.Start(0.05f, 1);
	}
	else if (!raining && fall.switched) {
		// rain has just ended: fade them out
		fall.switched = false;
		fall.Start(0.07f, 0);
	}

	Constants.Data.x = fall.GetValue();

	// UNOFFICIAL 2026-11-05: the rain itself asks for the ortho map (shelter), instead of relying on WetWorld to ask for it.
	const auto& game = TheShaderManager->GameState;
	if (Constants.Data.x > 0.0f && game.isExterior && !game.isUnderwater) TheShaderManager->orthoRequired = true;
	if (Sys.S.Mode == 1) UpdateSystem(Constants.Data.x);
}

void RainEffect::UpdateSettings() {
	const char* section = "Shaders.Precipitations.Main";
	SettingManager* cfg = TheSettingManager;
	Constants.Data.y = cfg->GetSettingF(section, "VerticalScale");
	Constants.Data.z = cfg->GetSettingF(section, "Speed");
	Constants.Data.w = cfg->GetSettingF(section, "Opacity");

	Constants.Aspect.x = cfg->GetSettingF(section, "Refraction");
	Constants.Aspect.y = cfg->GetSettingF(section, "Coloring");
	Constants.Aspect.z = cfg->GetSettingF(section, "Bloom");
	// UNOFFICIAL 2026-10-17 (Precipitations.fx.hlsl): skip the rain layers behind each pixel (they add exactly 0). A key missing
	// from the user's settings file reads the shipped default (on); only a key missing from the DEFAULTS toml reads 0 = off.
	// Note (fixes-b #37): GetOrtho samples with tex2Dlod level 0 whether this option is on or off (identical unless a driver
	// anisotropic override acts in minification).
	Constants.Aspect.w = cfg->GetSettingI(section, "SkipHiddenLayers") != 0 ? 1.0f : 0.0f;

	// UNOFFICIAL 2026-11-05 rain system ([Shaders.Precipitations.System]; README.md "Rain system").
	const char* sys = "Shaders.Precipitations.System";
	SystemSettings& S = Sys.S;
	S.Mode = std::clamp(cfg->GetSettingI(sys, "Mode"), 0, 1);
	S.Quality = std::clamp(cfg->GetSettingI(sys, "Quality"), 0, 3);
	S.Intensity = std::clamp(cfg->GetSettingF(sys, "Intensity"), 0.0f, 4.0f);
	S.Density = std::clamp(cfg->GetSettingF(sys, "Density"), 0.0f, 1.0f);
	S.Exposure = std::clamp(cfg->GetSettingF(sys, "Exposure"), 0.002f, 0.2f);
	S.StreakLength = std::clamp(cfg->GetSettingF(sys, "StreakLength"), 0.1f, 10.0f);
	S.Width = std::clamp(cfg->GetSettingF(sys, "Width"), 0.1f, 10.0f);
	S.Opacity = std::clamp(cfg->GetSettingF(sys, "Opacity"), 0.0f, 4.0f);
	S.EnvLift = std::clamp(cfg->GetSettingF(sys, "EnvironmentLift"), -1.0f, 4.0f);
	S.SunGlint = std::clamp(cfg->GetSettingF(sys, "SunGlint"), 0.0f, 10.0f);
	S.LampGlint = std::clamp(cfg->GetSettingF(sys, "LampGlint"), 0.0f, 10.0f);
	S.SkyShare = std::clamp(cfg->GetSettingF(sys, "SkyShare"), 0.0f, 1.0f);
	S.FallSpeed = std::clamp(cfg->GetSettingF(sys, "FallSpeed"), 0.05f, 5.0f);
	S.WindScale = std::clamp(cfg->GetSettingF(sys, "Wind"), 0.0f, 3000.0f);
	S.Gusts = std::clamp(cfg->GetSettingF(sys, "Gusts"), 0.0f, 1.0f);
	S.Haze = std::clamp(cfg->GetSettingF(sys, "Haze"), 0.0f, 1.0f);
	S.HazeExtinction = std::clamp(cfg->GetSettingF(sys, "HazeExtinction"), 0.0f, 0.01f);
	S.Curtains = std::clamp(cfg->GetSettingF(sys, "Curtains"), 0.0f, 1.0f);
	S.NearDistance = std::clamp(cfg->GetSettingF(sys, "NearDistance"), 100.0f, 2000.0f);
	S.FarDistance = std::clamp(cfg->GetSettingF(sys, "FarDistance"), S.NearDistance, 20000.0f);
	S.FarBoost = std::clamp(cfg->GetSettingF(sys, "FarBoost"), 0.0f, 10.0f);
	S.LensDrops = std::clamp(cfg->GetSettingF(sys, "LensDrops"), 0.0f, 1.0f);
	S.LensFirstPerson = cfg->GetSettingI(sys, "LensDropsFirstPerson") != 0;
	S.LensSize = std::clamp(cfg->GetSettingF(sys, "LensDropSize"), 0.01f, 0.3f);
	S.DebugView = std::clamp(cfg->GetSettingI(sys, "DebugView"), 0, 4);
}

void RainEffect::RegisterConstants() {
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_RainData", &Constants.Data);
	sm->RegisterConstant("TESR_RainAspect", &Constants.Aspect);
	// UNOFFICIAL 2026-11-05 rain system
	SystemConstants& C = Sys.C;
	sm->RegisterConstant("TESR_RainBand", C.Band);
	sm->RegisterConstant("TESR_RainOrigin", C.Origin);
	sm->RegisterConstant("TESR_RainFall", C.Fall);
	sm->RegisterConstant("TESR_RainStreak", &C.Streak);
	sm->RegisterConstant("TESR_RainShape", &C.Shape);
	sm->RegisterConstant("TESR_RainLight", &C.Light);
	sm->RegisterConstant("TESR_RainHaze", &C.Haze);
	sm->RegisterConstant("TESR_RainHazeDrift", &C.HazeDrift);
	sm->RegisterConstant("TESR_RainLens", &C.Lens);
	sm->RegisterConstant("TESR_RainMisc", &C.Misc);
	sm->RegisterConstant("TESR_RainPass", &C.Pass);
	sm->RegisterConstant("TESR_RainLampPosition", C.LampPosition);
	sm->RegisterConstant("TESR_RainLampColor", C.LampColor);
}

// The far bands' half-resolution buffer (x: coverage). G16R16F at half the frame each way (about 3.7 MB at 2560 x 1440).
void RainEffect::RegisterTextures() {
	const UINT w = MaxQuality::Reduced(TheRenderManager->width, 2);
	const UINT h = MaxQuality::Reduced(TheRenderManager->height, 2);
	TheTextureManager->InitTexture("TESR_RainFarBuffer", &Textures.FarTexture, &Textures.FarSurface, w, h, D3DFMT_G16R16F);
}

// The frame's rain (EffectChain.cpp PostStep_WeatherAndLegacyBloom). Mode 1: the far bands into the half-resolution buffer first
// (Quality 1-2), then the main pass through the chain; Mode 0: the original pass.
void RainEffect::RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* SourceSurface) {
	if (Sys.S.Mode == 1 && Enabled && Effect && ShouldRender()) {
		const bool farPass = Sys.bands > 1 && !Sys.farInMain && Textures.FarSurface;
		Sys.C.Misc.z = farPass ? 1.0f : 0.0f;
		if (farPass) {
			Sys.C.Pass = D3DXVECTOR4(2.0f, 1.0f, (float)Sys.bands, Sys.S.FarBoost);
			TheShaderManager->RenderEffectToRT(Textures.FarSurface, this, false, 2);
			Device->SetRenderTarget(0, RenderTarget);
		}
		// main pass: band 0, or every band (Quality 3, or no far buffer)
		const float mainBands = farPass ? 1.0f : (float)Sys.bands;
		Sys.C.Pass = D3DXVECTOR4(1.0f, 0.0f, mainBands, 1.0f);
	}
	Render(Device, RenderTarget, TheTextureManager->RenderedSurface, RainTechnique(), false, SourceSurface);
}

bool RainEffect::ShouldRender() {
	const auto& game = TheShaderManager->GameState;
	if (game.isUnderwater || !game.isExterior) return false;
	// the rain system also draws while drops are still drying on the lens
	return Constants.Data.x > 0.0f || (Sys.S.Mode == 1 && Sys.C.Lens.x > 0.001f);
};

// ---- UNOFFICIAL 2026-11-05 rain system --------------------------------------------------------------------------------------

// The three drop sizes (README "Rain system"): about 1, 2 and 3.4 mm across, falling at their terminal speeds (Atlas et al.:
// 4.0, 6.5 and 8.5 m/s), in units a second (1 m = 70 units).
static const double DropSpeed[3] = { 280.0, 455.0, 595.0 };
// The bands of the streak volume: cell size, vertical period, and their share of the distance. Each band's cells repeat every
// RepeatCells cells, so the camera's place in the lattice can be wrapped (doubles in C++, small floats in the shader).
static const float RepeatCells = 256.0f;

void RainEffect::UpdateSystem(float rain) {
	const SystemSettings& S = Sys.S;
	SystemConstants& C = Sys.C;
	const auto& game = TheShaderManager->GameState;
	const double dt = ElapsedOncePerFrame(Sys.lastFrameTime);
	auto wrap = [](double v, double period) { v = fmod(v, period); return v < 0.0 ? v + period : v; };

	// Rain amount: the weather's animator times Intensity (more than 1 = heavier than the weather says).
	const float amount = (std::min)(rain * S.Intensity, 4.0f);
	const float heavy = std::clamp(amount, 0.0f, 1.0f);

	// Wind: the weather's (direction around up, speed 0..1), as grass and cloth read it, plus slow gusts.
	Sky* sky = Tes ? Tes->sky : nullptr;
	float windDir = 0.0f, windSpeed = 0.0f;
	if (sky) { windDir = sky->windDirection; windSpeed = std::clamp(sky->windSpeed, 0.0f, 1.0f); }
	Sys.gustClock = wrap(Sys.gustClock + dt, 3600.0);
	const double g = Sys.gustClock;
	const float gust = 1.0f + S.Gusts * (float)(0.35 * sin(g * 0.37) + 0.25 * sin(g * 1.13 + 1.7) + 0.15 * sin(g * 2.71 + 0.4));
	const float windNow = S.WindScale * windSpeed * (std::max)(gust, 0.0f);
	const float targetX = sinf(windDir) * windNow, targetY = cosf(windDir) * windNow;
	// the drops take the wind's speed within a fraction of a second; smooth over 0.5 s so a weather change does not jump
	const float follow = (float)(1.0 - exp(-dt / 0.5));
	Sys.windX += (targetX - Sys.windX) * follow;
	Sys.windY += (targetY - Sys.windY) * follow;

	// Rain space: x' = x + shear z. A drop falling at v (down) drifting at w moves w / v sideways per unit of fall.
	const double fallRef = DropSpeed[1] * S.FallSpeed;
	const float shearX = (float)(Sys.windX / fallRef), shearY = (float)(Sys.windY / fallRef);
	C.Shape.x = shearX;
	C.Shape.y = shearY;
	// x' = x + shear z + offset. When the shear changes, the offset takes the change at the eye's height, so the drops round the eye
	// do not move (only those far above or below shift, by the change times their height difference: slow and small).
	const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
	if (Sys.shearSet) {
		Sys.shearOffX += ((double)Sys.lastShearX - shearX) * (double)eye.z;
		Sys.shearOffY += ((double)Sys.lastShearY - shearY) * (double)eye.z;
		const double big = (double)RepeatCells * 24.0 * 27.0 * 64.0;	// a multiple of every band's repeat
		Sys.shearOffX = wrap(Sys.shearOffX, big);
		Sys.shearOffY = wrap(Sys.shearOffY, big);
	}
	Sys.lastShearX = shearX; Sys.lastShearY = shearY; Sys.shearSet = true;

	// Bands: (cell, period, start, end). Band 0 (near, 32-unit columns) always runs in the main pass at full resolution.
	// Quality 0: band 0 only. 1: band 1 (96) reaches FarDistance, in the half-resolution far pass. 2: bands 1 and 2 (96, 288) in the
	// far pass. 3: as 2, reaching 1.5 x FarDistance, with the far bands in the main pass at full resolution (no far buffer).
	// The steps per pixel are bounded in the shader by the cells the view really crosses (at most 96 per band).
	const float nearEnd = S.NearDistance;
	const float farEnd = S.Quality >= 3 ? S.FarDistance * 1.5f : S.FarDistance;
	const float midEnd = nearEnd + (farEnd - nearEnd) * 0.3f;
	const int bands = S.Quality == 0 ? 1 : (S.Quality == 1 ? 2 : 3);
	const float cell[Bands] = { 48.0f, 96.0f, 288.0f };	// (bench 2026-11-05: 32-unit near cells cost 1.7 ms at 1080p)
	const float period[Bands] = { 220.0f, 260.0f, 320.0f };
	const float start[Bands] = { 40.0f, nearEnd, midEnd };	// (bench 2026-11-05: drops nearer than about half a metre drew as wide bright beams)
	const float end[Bands] = { nearEnd, bands == 2 ? farEnd : midEnd, farEnd };
	Sys.bands = amount > 0.0f ? bands : 0;
	Sys.farInMain = S.Quality >= 3;

	for (int b = 0; b < Bands; b++) {
		const bool on = b < Sys.bands;
		C.Band[b] = D3DXVECTOR4(cell[b], period[b], start[b], on ? end[b] : 0.0f);
		// The camera in the band's lattice (rain space, wrapped at the band's repeat: doubles here, small floats in the shader).
		const double repeat = (double)RepeatCells * cell[b];
		C.Origin[b].x = (float)wrap((double)eye.x + shearX * (double)eye.z + Sys.shearOffX, repeat);
		C.Origin[b].y = (float)wrap((double)eye.y + shearY * (double)eye.z + Sys.shearOffY, repeat);
		C.Origin[b].z = (float)wrap((double)eye.z, (double)period[b]);
		C.Origin[b].w = RepeatCells;
		for (int k = 0; k < 3; k++) {
			Sys.fall[b][k] = wrap(Sys.fall[b][k] + DropSpeed[k] * S.FallSpeed * dt, (double)period[b]);
		}
		C.Fall[b] = D3DXVECTOR4((float)Sys.fall[b][0], (float)Sys.fall[b][1], (float)Sys.fall[b][2], 0.0f);
	}

	// Streaks: speed x exposure, per size (the camera exposure the eye reads as a streak).
	const float lengthScale = (float)(S.Exposure * S.StreakLength * S.FallSpeed);
	// (each length kept below 0.8 of the shortest drop period, 220 units: longer streaks would join into solid lines)
	auto streakLen = [&](int k) { return (std::min)((float)DropSpeed[k] * lengthScale, 176.0f); };
	C.Streak = D3DXVECTOR4(streakLen(0), streakLen(1), streakLen(2), S.Width);
	// Drop chance per slot: the density at full rain, thinning out in light rain (light rain is mostly fewer drops).
	C.Shape.w = std::clamp(S.Density * (0.15f + 0.85f * heavy) * (amount > 1.0f ? sqrtf(amount) : 1.0f), 0.0f, 1.0f) * (amount > 0.0f ? 1.0f : 0.0f);
	// One pixel's angle (vertical; square pixels), from the drawn rows.
	const float rows = (float)RenderRect::Height(TheRenderManager->height);
	const float m22 = TheRenderManager->projMatrix._22;
	C.Shape.z = m22 > 0.0f && rows > 0.0f ? 2.0f / (m22 * rows) : 0.001f;

	C.Light = D3DXVECTOR4(S.Opacity, S.EnvLift, S.SunGlint, S.LampGlint);

	// Haze: thicker air in heavy rain, in curtains anchored to the world and drifting with the wind (plus a slow drift of their
	// own). The shader's noise repeats every 256 curtain cells (its second octave every 768 thirds), so the eye plus the drift is
	// passed in curtain cells, wrapped at 256: no seam, no jump.
	const double curtain = 3500.0;
	Sys.driftX = wrap(Sys.driftX - (Sys.windX + 60.0) * dt * 1.5, curtain * 256.0);
	Sys.driftY = wrap(Sys.driftY - (Sys.windY + 25.0) * dt * 1.5, curtain * 256.0);
	C.Haze = D3DXVECTOR4(S.Haze * heavy * heavy, S.HazeExtinction * (0.25f + 0.75f * heavy), S.Curtains, (float)curtain);
	C.HazeDrift = D3DXVECTOR4((float)wrap(((double)eye.x + Sys.driftX) / curtain, 256.0), (float)wrap(((double)eye.y + Sys.driftY) / curtain, 256.0),
		S.SkyShare, (float)S.DebugView);

	// Lens drops: the lens wets while the camera is out in the rain and not under cover (WetWorld's cover test), more when it
	// looks up; it dries in about 20 s otherwise. First-person view only with LensDropsFirstPerson.
	const bool firstPerson = TheCameraManager && TheCameraManager->IsFirstPerson();
	const WetWorldEffect* wet = TheShaderManager->Effects.WetWorld;
	const bool covered = wet && wet->Enabled && wet->covered;	// (EffectChain runs the cover test while LensWantsCover)
	const bool lensAllowed = S.LensDrops > 0.0f && (!firstPerson || S.LensFirstPerson) && game.isExterior && !game.isUnderwater;
	const D3DXMATRIX& view = TheRenderManager->viewMatrix;
	const float lookUp = std::clamp(view._33 * 0.5f + 0.5f, 0.0f, 1.0f);	// the view's forward z (world; 0: down, 1: up)
	if (lensAllowed && !covered && heavy > 0.0f) Sys.lensWet = (std::min)(1.0f, Sys.lensWet + (float)dt * heavy * (0.03f + 0.25f * lookUp * lookUp));
	else Sys.lensWet = (std::max)(0.0f, Sys.lensWet - (float)dt / 20.0f);
	if (!lensAllowed) Sys.lensWet = 0.0f;
	Sys.lensWanted = lensAllowed && heavy > 0.0f;
	Sys.lensClock = wrap(Sys.lensClock + dt, 100.0);	// each lens drop runs a whole number of cycles per 100 s
	C.Lens = D3DXVECTOR4(Sys.lensWet * S.LensDrops, S.LensSize, (float)Sys.lensClock, 1.0f);

	// Misc: soft range, fog fade of the drops (fog thickens the air between the eye and the drops a little), aspect.
	const float aspect = TheRenderManager->height > 0 ? (float)TheRenderManager->width / (float)TheRenderManager->height : 1.777f;
	C.Misc.y = 12.0f;
	C.Misc.z = 0.0f;	// set per frame in RenderFrame (whether the far buffer holds this frame's far bands)
	C.Misc.w = aspect;
	PickLamps();
	LogSystem();
}

// The lamps that light the drops (street lamps, fires at night): DustMotes' pick (DustMotes.cpp PickLamps), out to the near bands.
void RainEffect::PickLamps() {
	struct Candidate { D3DXVECTOR4 position, color; float score; };
	Candidate found[ShadowCubeMapsMax + TrackedLightsMax];
	int count = 0;
	ShaderManager* sm = TheShaderManager;
	const float radiusMult = (std::max)(sm->Effects.ShadowsExteriors->Settings.Interiors.LightRadiusMult, 0.01f);
	const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
	const float reachOut = Sys.S.NearDistance * 2.0f;
	Sys.seenLamps = 0;
	auto consider = [&](const D3DXVECTOR4& lamp, const D3DXVECTOR4& color) {
		const float radius = lamp.w / radiusMult;
		if (!(radius > 10.0f) || !(color.w > 0.0f)) return;
		const D3DXVECTOR3 rel(lamp.x - eye.x, lamp.y - eye.y, lamp.z - eye.z);
		for (int i = 0; i < count; i++)
			if (fabsf(found[i].position.x - rel.x) < 1.0f && fabsf(found[i].position.y - rel.y) < 1.0f && fabsf(found[i].position.z - rel.z) < 1.0f) return;
		Sys.seenLamps++;
		const float distance = D3DXVec3Length(&rel);
		if (distance - radius > reachOut) return;
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
		Sys.C.LampPosition[i] = i < used ? found[i].position : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f);
		Sys.C.LampColor[i] = i < used ? found[i].color : D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	Sys.C.Misc.x = Sys.S.LampGlint > 0.0f ? (float)used : 0.0f;
}

// Every 10 seconds while it rains: the rain system's state.
void RainEffect::LogSystem() {
	if (Constants.Data.x <= 0.0f && Sys.C.Lens.x <= 0.0f) return;
	const double now = TheFrameRateManager->Time;
	if (Sys.lastLogTime > 0.0 && now - Sys.lastLogTime < 10.0) return;
	Sys.lastLogTime = now;
	Logger::Log("UNOFFICIAL Rain system: rain %.2f (x Intensity %.2f), drop chance %.2f, wind %.0f %.0f units/s, bands %d to %.0f/%.0f/%.0f units, far bands %s, lamps %d of %d, haze %.3f, lens %.2f",
		Constants.Data.x, Sys.S.Intensity, Sys.C.Shape.w, Sys.windX, Sys.windY, Sys.bands, Sys.C.Band[0].w, Sys.C.Band[1].w, Sys.C.Band[2].w,
		Sys.farInMain ? "in the main pass" : (Textures.FarSurface ? "at half resolution" : "OFF (no buffer)"), (int)Sys.C.Misc.x, Sys.seenLamps, Sys.C.Haze.x, Sys.C.Lens.x);
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): Precipitations.frag; the textures the D3D9 technique binds by name (the source copy, TESR_DepthBuffer,
// TESR_OrthoMapBuffer, TESR_BloomBuffer) and the values it uploads (the same objects RegisterConstant points at).
// The rain system (technique 1) has no Vulkan pass: it returns false and the D3D9 pass runs (also under DXVK).
bool RainEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!shadows) return false;
	struct Push { D3DXMATRIX shadowCameraToLight, viewTransform; D3DXVECTOR4 viewScale, rainData, rainAspect, sunDirection, sunColor; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.shadowCameraToLight = shadows->ShadowMaps[ShadowsExteriorEffect::MapOrtho].ShadowCameraToLight;
	push.viewTransform = TheRenderManager->viewMatrix;
	push.viewScale = D3DXVECTOR4(TheRenderManager->projMatrix._11, TheRenderManager->projMatrix._22, TheRenderManager->CameraData.y, TheShaderManager->ShaderConst.GameTime.z);
	push.rainData = Constants.Data;
	push.rainAspect = Constants.Aspect;
	push.sunDirection = TheShaderManager->ShaderConst.SunDir;
	push.sunColor = TheShaderManager->ShaderConst.sunColor;
	IDirect3DBaseTexture9* textures[] = { TheTextureManager->SourceTexture,
		TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr,
		shadows->ShadowMapOrthoTexture,
		TheShaderManager->Effects.Bloom ? TheShaderManager->Effects.Bloom->Textures.BloomTexture[0] : nullptr };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "Precipitations", textures, _countof(textures), &push, sizeof(push), 208);
}
