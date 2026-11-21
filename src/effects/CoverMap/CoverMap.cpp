#include "CoverMap.h"
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/FrameClock.h"	// UNOFFICIAL 2026-10-28 footprints: the walking speed

// UNOFFICIAL 2026-10-27 Persistent wetness and snow cover: see CoverMap.h (design) and Effects\CoverMap.fx.hlsl (the update pass).

CoverMapEffect* TheCoverMap = nullptr;
unsigned int CoverMapOrthoFrame = ~0u;

namespace {
	const char* const SnowSection = "Shaders.SnowAccumulation.Main";
	const float InsetShare = 0.42f;			// the debug view's corner map: its side as a share of the screen height
	const float WindowFade = 0.06f;			// the window's faded border, share of its size (the open-ground values take over outside)
	const unsigned RefreshFrames = 4;		// while the ortho map is drawn, the heights are refreshed at most once every this many frames
	const double MaxTrail = 400.0;			// a move longer than this between two updates is a teleport: no footprint trail along it

	inline float Saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
	inline float Larger(float a, float b) { return a > b ? a : b; }
	inline float Smaller(float a, float b) { return a < b ? a : b; }
	inline float Smooth(float a, float b, float x) { const float t = Saturate((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
	// an amount spread over 'time' game hours (0: at once)
	inline float Over(float amount, float time) { return time > 0.0f ? amount / time : (amount > 0.0f ? 1.0f : 0.0f); }
	inline long long FloorTexel(double v) { return (long long)floor(v); }
	inline float PositiveMod(long long v, long long n) { const long long m = v % n; return (float)(m < 0 ? m + n : m); }
	inline double Fraction(double v) { return v - floor(v); }
}

void CoverMapEffect::UpdateSettings() {
	SettingsStruct s;
	// 2026-11-21 (WetWorld removed, owner's word): the wet half had no other reader (WetWorld.fx techniques 5 / 6), so it stays off; its
	// model below is dormant. The map's size, resolution and debug view moved to [Shaders.SnowAccumulation.Main] (settings step 91).
	s.wet = false;
	s.snow = TheSettingManager->GetSettingI(SnowSection, "Persistent") != 0;
	s.size = std::clamp(TheSettingManager->GetSettingF(SnowSection, "PersistentMapSize"), 1024.0f, 65536.0f);
	s.resolution = std::clamp(TheSettingManager->GetSettingI(SnowSection, "PersistentMapResolution"), 128, 4096);
	s.snowIncrease = Larger(TheSettingManager->GetSettingF(SnowSection, "Increase"), 0.0f);
	s.snowDecrease = Larger(TheSettingManager->GetSettingF(SnowSection, "Decrease"), 0.0f);
	s.snowShade = std::clamp(TheSettingManager->GetSettingF(SnowSection, "PersistentShade"), 0.02f, 1.0f);
	s.drifts = std::clamp(TheSettingManager->GetSettingF(SnowSection, "PersistentDrifts"), 0.0f, 1.0f);
	s.footprints = std::clamp(TheSettingManager->GetSettingF(SnowSection, "PersistentFootprints"), 0.0f, 1.0f);
	s.footSize = std::clamp(TheSettingManager->GetSettingF(SnowSection, "PersistentFootprintSize"), 2.0f, 200.0f);
	s.footShape = std::clamp(TheSettingManager->GetSettingI(SnowSection, "PersistentFootprintShape"), 0, 1);	// UNOFFICIAL 2026-10-28 (missing = blob)
	s.debug = std::clamp(TheSettingManager->GetSettingI(SnowSection, "PersistentDebugView"), 0, 2);

	// A switch turned on, or another map size: the map starts again (from the old single amounts). Both off or another texel
	// count: the maps are freed (made again when needed).
	if (s.wet && !Settings.wet) { resyncWet = true; mapValid = false; }
	if (s.snow && !Settings.snow) { resyncSnow = true; mapValid = false; }
	if (s.size != Settings.size) mapValid = false;
	if ((!s.wet && !s.snow) || s.resolution != Settings.resolution) { ReleaseMap(); mapFailed = false; mapMakeFailed.Clear(); }
	Settings = s;
}

void CoverMapEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_CoverMapUpdate", &Constants.Update);
	TheShaderManager->RegisterConstant("TESR_CoverMapOrigin", &Constants.Origin);
	TheShaderManager->RegisterConstant("TESR_CoverMapShift", &Constants.Shift);
	TheShaderManager->RegisterConstant("TESR_CoverMapRates", &Constants.Rates);
	TheShaderManager->RegisterConstant("TESR_CoverMapSnowRates", &Constants.SnowRates);
	TheShaderManager->RegisterConstant("TESR_CoverMapSun", &Constants.Sun);
	TheShaderManager->RegisterConstant("TESR_CoverMapShade", &Constants.Shade);
	TheShaderManager->RegisterConstant("TESR_CoverMapStart", &Constants.Start);
	TheShaderManager->RegisterConstant("TESR_CoverMapFootA", &Constants.FootA);
	TheShaderManager->RegisterConstant("TESR_CoverMapFootB", &Constants.FootB);
	TheShaderManager->RegisterConstant("TESR_CoverMapDebug", &Constants.Debug);
	TheShaderManager->RegisterConstant("TESR_CoverMapData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_CoverMapWindow", &Constants.Window);
	TheShaderManager->RegisterConstant("TESR_CoverMapGlobal", &Constants.Global);
	TheShaderManager->RegisterConstant("TESR_CoverMapTrail", &Constants.Trail);
	TheShaderManager->RegisterConstant("TESR_CoverMapFeet", Constants.Feet);	// UNOFFICIAL 2026-10-28 footprints (MaxPrints)
	TheShaderManager->RegisterConstant("TESR_CoverMapFeetData", &Constants.FeetData);
	TheShaderManager->RegisterConstant("TESR_CoverMapOrthoInverse", (D3DXVECTOR4*)&Constants.OrthoInverse);
}

// The slots TESR_CoverMapBuffer and TESR_CoverMapAux follow (EffectRecord::SetCT reads them at every bind): the maps written last,
// null while there are none.
void CoverMapEffect::RegisterTextures() {
	TheTextureManager->RegisterTexture("TESR_CoverMapBuffer", (IDirect3DBaseTexture9**)&currentTexture);
	TheTextureManager->RegisterTexture("TESR_CoverMapAux", (IDirect3DBaseTexture9**)&currentAux);
}

bool CoverMapEffect::SnowActive() const {
	const SnowAccumulationEffect* snow = TheShaderManager->Effects.SnowAccumulation;
	return Settings.snow && Effect && Enabled && !mapFailed && !mapMakeFailed.Blocked() && snow && snow->Enabled && snow->Effect && (snowBound > 0.0f || snowNow);
}

UINT CoverMapEffect::SnowTechnique(SnowAccumulationEffect* snow) const {
	if (!Settings.snow || !mapValid || !currentTexture || Constants.Global.w < 0.5f || !snow || !snow->Effect) return 0;
	return snow->Effect->GetTechnique(1) ? 1 : 0;	// SnowAccumulation.fx.hlsl PersistentTechnique
}

bool CoverMapEffect::ShouldRender() {
	return (Settings.wet || Settings.snow) && Settings.debug > 0 && TheShaderManager->GameState.isExterior;
}

// The slowest change in use, per game hour, sets how much game time the model gathers before it changes anything: each change is
// then at least 24 steps of the map's 16 bits (the slowest snow melt in the shade, ~0.03 a game hour, would otherwise round away).
double CoverMapEffect::MinStep() const {
	float slowest = 1e9f;
	auto rate = [&slowest](float time, float factor) { if (time > 0.0f) slowest = Smaller(slowest, factor / time); };
	if (Settings.wet) {
		rate(Settings.wetTime, 1.0f); rate(Settings.puddleFill, 1.0f);
		rate(Settings.dryTime, Settings.wetShade); rate(Settings.puddleDry, Settings.wetShade);
	}
	if (Settings.snow) { rate(Settings.snowIncrease, 1.0f - 0.5f * Settings.drifts); rate(Settings.snowDecrease, Settings.snowShade); }
	if (slowest > 1e8f) return 0.0005;
	const double bit = mapUnorm ? 1.0 / 65535.0 : 1.0 / 2048.0;
	return std::clamp(24.0 * bit / slowest, 0.0005, 0.05);
}

// The open-ground model, once a frame: one open, unshaded spot under the weather. A spot entering the window starts from it, and
// it stands in for everything outside the window. The bounds say when nothing in the map can be wet (or snowy) any more.
void CoverMapEffect::UpdateConstants() {
	const unsigned int frame = TheFrameRateManager->FrameNumber;
	if (frame == tickFrame) return;
	tickFrame = frame;

	ShaderManager* sm = TheShaderManager;
	SnowAccumulationEffect* snowAccumulation = sm->Effects.SnowAccumulation;
	rainNow = 0.0f;	// (2026-11-21: the wet half is off for good; it read WetWorld's rain amount)
	snowNow = sm->GameState.isSnow;

	// The old single amount (what SnowAccumulation draws with Persistent off): the snow (TESR_SnowAccumulationParams.w). The wet half's
	// (WetWorld's ground and puddles) are 0 since WetWorld went (2026-11-21).
	const float oldWet = 0.0f;
	const float oldPuddles = 0.0f;
	const float oldSnow = snowAccumulation ? Saturate(snowAccumulation->Constants.Data.w) : 0.0f;
	auto syncWet = [&]() { Constants.Global.x = oldWet; Constants.Global.y = oldPuddles; wetBound = Larger(oldWet, oldPuddles); };
	auto syncSnow = [&]() { Constants.Global.z = oldSnow; snowBound = oldSnow; };
	if (!Settings.wet) { syncWet(); wetBound = 0.0f; }
	else if (resyncWet) { syncWet(); resyncWet = false; }
	if (!Settings.snow) { syncSnow(); snowBound = 0.0f; }
	else if (resyncSnow) { syncSnow(); resyncSnow = false; }
	if (!Settings.wet && !Settings.snow) { clockSet = false; return; }

	const double hours = (double)TimeGlobals::Get()->GameDaysPassed->data * 24.0;
	if (!clockSet || hours < lastCommit - 0.001 || hours - lastCommit > 72.0) {
		// The first frame, a save loaded (game time went back) or a long jump: start again from the old amounts.
		if (clockSet) {
			if (Settings.wet) syncWet();
			if (Settings.snow) syncSnow();
			Logger::Log("UNOFFICIAL Persistent wetness/snow: game time jumped (%.2f -> %.2f h), the cover map starts again", lastCommit, hours);
		}
		clockSet = true;
		lastCommit = hours;
		lastRaining = rainNow > 0.0f;
		lastSnowing = snowNow;
		mapValid = false;
		pendWetGain = pendWetDry = pendPuddleGain = pendPuddleDry = pendSnowGain = pendSnowMelt = 0.0f;
		return;
	}

	const bool changed = (rainNow > 0.0f) != lastRaining || snowNow != lastSnowing;
	const double pending = hours - lastCommit;
	if (pending < MinStep() && !changed) return;
	lastCommit = hours;
	lastRaining = rainNow > 0.0f;
	lastSnowing = snowNow;
	const float step = (float)pending;
	const float day = Saturate(sm->ShaderConst.SunAmount.x);	// smooth daylight (0 night .. 1 day)

	if (Settings.wet) {
		const float wetGain = Over(rainNow * step, Settings.wetTime);
		const float wetDry = Over((1.0f - rainNow) * step, Settings.dryTime);
		const float puddleGain = Over(rainNow * step, Settings.puddleFill);
		const float puddleDry = Over((1.0f - rainNow) * step, Settings.puddleDry);
		const float open = Settings.wetShade + (1.0f - Settings.wetShade) * day;	// an open spot: sunlit by day, as shade at night
		Constants.Global.x = Saturate(Constants.Global.x + wetGain - wetDry * open);
		Constants.Global.y = Saturate(Constants.Global.y + puddleGain - puddleDry * open);
		wetBound = Saturate(wetBound + Larger(wetGain, puddleGain) - Smaller(wetDry, puddleDry) * Settings.wetShade);
		wetBound = Larger(wetBound, Larger(Constants.Global.x, Constants.Global.y));
		pendWetGain += wetGain; pendWetDry += wetDry; pendPuddleGain += puddleGain; pendPuddleDry += puddleDry;
		if (wetBound <= 0.0f && rainNow <= 0.0f) {
			// every spot is dry: nothing to apply (and no stale drying to cancel the next rain's first update)
			pendWetGain = pendWetDry = pendPuddleGain = pendPuddleDry = 0.0f;
			startGlobal.x = Constants.Global.x; startGlobal.y = Constants.Global.y;
		}
	}
	if (Settings.snow) {
		const float snowing = snowNow ? 1.0f : 0.0f;
		const float snowGain = Over(snowing * step, Settings.snowIncrease);
		const float snowMelt = Over((1.0f - snowing) * step, Settings.snowDecrease);
		Constants.Global.z = Saturate(Constants.Global.z + snowGain - snowMelt * (Settings.snowShade + (1.0f - Settings.snowShade) * day));
		snowBound = Larger(Saturate(snowBound + snowGain * (1.0f + Settings.drifts) - snowMelt * Settings.snowShade), Constants.Global.z);
		pendSnowGain += snowGain; pendSnowMelt += snowMelt;
		if (snowBound <= 0.0f && !snowNow) { pendSnowGain = pendSnowMelt = 0.0f; startGlobal.z = Constants.Global.z; }
	}
}

bool CoverMapEffect::EnsureMap(IDirect3DDevice9* Device) {
	if (mapTexture[0] && mapTexture[1] && auxTexture[0] && auxTexture[1]) return true;
	if (mapFailed || mapMakeFailed.Blocked()) return false;
	ReleaseMap();
	const UINT n = (UINT)Settings.resolution;
	// The update writes both maps in one pass (two render targets of one format: every D3D9 card with ps_3_0 has at least four).
	D3DCAPS9 caps = {};
	if (FAILED(Device->GetDeviceCaps(&caps)) || caps.NumSimultaneousRTs < 2) {
		mapFailed = true;
		Logger::Log("[ERROR] UNOFFICIAL Persistent wetness/snow: the card draws into %lu target(s) at once, the cover map needs 2; SnowAccumulation keeps its single amount", caps.NumSimultaneousRTs);
		return false;
	}
	const D3DFORMAT formats[2] = { D3DFMT_A16B16G16R16, D3DFMT_A16B16G16R16F };
	auto make = [&](IDirect3DTexture9*& texture, IDirect3DSurface9*& surface, D3DFORMAT format) {
		return SUCCEEDED(Device->CreateTexture(n, n, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &texture, NULL)) && texture &&
			SUCCEEDED(texture->GetSurfaceLevel(0, &surface)) && surface;
	};
	for (int f = 0; f < 2; f++) {
		bool ok = true;
		for (int i = 0; i < 2 && ok; i++) ok = make(mapTexture[i], mapSurface[i], formats[f]) && make(auxTexture[i], auxSurface[i], formats[f]);
		if (ok) {
			for (int i = 0; i < 2; i++) { Device->ColorFill(mapSurface[i], NULL, D3DCOLOR_ARGB(0, 0, 0, 0)); Device->ColorFill(auxSurface[i], NULL, D3DCOLOR_ARGB(0, 0, 0, 0)); }
			mapUnorm = f == 0;
			mapResolution = (int)n;
			current = 0;
			currentTexture = mapTexture[0];
			currentAux = auxTexture[0];
			mapValid = false;
			Logger::Log("UNOFFICIAL Persistent wetness/snow: cover map %ux%u texels (%s), %.0f units across (%.1f units a texel), %.1f MB",
				n, n, mapUnorm ? "A16B16G16R16" : "A16B16G16R16F fallback", Settings.size, Settings.size / n, 4.0 * 8.0 * n * n / (1024.0 * 1024.0));
			return true;
		}
		ReleaseMap();
	}
	mapMakeFailed.Set("its textures could not be made: SnowAccumulation keeps its single amount");
	Logger::Log("[ERROR] UNOFFICIAL Persistent wetness/snow: the cover map (%ux%u) could not be made; SnowAccumulation keeps its single amount", n, n);
	return false;
}

void CoverMapEffect::ReleaseMap() {
	for (int i = 0; i < 2; i++) {
		if (mapSurface[i]) { mapSurface[i]->Release(); mapSurface[i] = nullptr; }
		if (mapTexture[i]) { mapTexture[i]->Release(); mapTexture[i] = nullptr; }
		if (auxSurface[i]) { auxSurface[i]->Release(); auxSurface[i] = nullptr; }
		if (auxTexture[i]) { auxTexture[i]->Release(); auxTexture[i] = nullptr; }
	}
	currentTexture = nullptr;
	currentAux = nullptr;
	mapResolution = 0;
	mapValid = false;
}

// The readers' constants for this frame's camera: where the camera is in the wrapped map, and the window of the last update.
void CoverMapEffect::SetReaderConstants() {
	const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
	const double size = Settings.size;
	const double texelSize = size / (mapResolution > 0 ? mapResolution : Settings.resolution);
	Constants.Data = D3DXVECTOR4((float)(1.0 / size), (float)Fraction(camera.x / size), (float)Fraction(camera.y / size), (float)size);
	Constants.Window = D3DXVECTOR4((float)(originX * texelSize - camera.x), (float)(originY * texelSize - camera.y), (float)size, (float)(size * WindowFade));
	Constants.Global.w = mapValid && currentTexture ? 1.0f : 0.0f;
	Constants.Debug = D3DXVECTOR4((float)Settings.debug, InsetShare, 0.0f, 0.0f);
	Constants.Trail = D3DXVECTOR4(Settings.footSize, Settings.footprints, TrailRange(texelSize), 0.0f);
	SetFeetConstants();
}

// UNOFFICIAL 2026-10-28 footprints: the prints for SnowAccumulation, relative to the camera now, and a circle round them all.
void CoverMapEffect::SetFeetConstants() {
	const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
	double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
	bool any = false;
	for (int i = 0; i < MaxPrints; i++) {
		const Print& p = prints[i];
		if (!(p.depth > 0.001f) || !mapValid) { Constants.Feet[i] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f); continue; }
		double heading = atan2((double)p.dirY, (double)p.dirX) * (180.0 / D3DX_PI);
		if (heading < 0.0) heading += 360.0;
		const float degrees = (float)floor(heading + 0.5) >= 360.0f ? 0.0f : (float)floor(heading + 0.5);
		Constants.Feet[i] = D3DXVECTOR4((float)(p.x - camera.x), (float)(p.y - camera.y), (float)(p.z - camera.z),
			p.side * (degrees + 0.99f * Saturate(p.depth)));
		if (!any) { minX = maxX = p.x; minY = maxY = p.y; any = true; }
		else { minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x); minY = (std::min)(minY, p.y); maxY = (std::max)(maxY, p.y); }
	}
	// UNOFFICIAL 2026-10-28 PersistentFootprintShape: the boot shape rides as a negative half length (SnowAccumulation SnowFootprint).
	const float half = Settings.footShape == 1 ? -Settings.footSize : Settings.footSize;
	if (any) {
		const double cx = 0.5 * (minX + maxX), cy = 0.5 * (minY + maxY);
		const double r = 0.5 * sqrt((maxX - minX) * (maxX - minX) + (maxY - minY) * (maxY - minY)) + 2.0 * half;
		Constants.FeetData = D3DXVECTOR4((float)(cx - camera.x), (float)(cy - camera.y), (float)r, half);
	}
	else Constants.FeetData = D3DXVECTOR4(0.0f, 0.0f, -1.0f, half);
}

void CoverMapEffect::ClearPrints() {
	for (int i = 0; i < MaxPrints; i++) prints[i].depth = 0.0f;
	printNext = 0;
}

// The footprint trail's distance field reaches this far from the path: the brush radius plus two texels, so that every texel a
// bilinear read inside the brush touches still holds an unclamped (linear) distance.
float CoverMapEffect::TrailRange(double texelSize) const {
	return Settings.footSize + 2.0f * (float)texelSize + 4.0f;
}

void CoverMapEffect::RenderUpdate(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {
	ShaderManager* sm = TheShaderManager;
	Constants.Global.w = 0.0f;
	if (!Effect || !Enabled || (!Settings.wet && !Settings.snow) || !sm->GameState.isExterior) return;

	// The map belongs to one worldspace.
	TESWorldSpace* space = Player->GetWorldSpace();
	if (space != worldSpace) {
		if (worldSpace && mapValid) Logger::Log("UNOFFICIAL Persistent wetness/snow: another worldspace, the cover map starts again");
		worldSpace = space;
		mapValid = false;
		ClearPrints();	// 2026-10-28 footprints: they belong to the old worldspace
	}
	const bool wanted = SnowActive();	// (2026-11-21: the wet half is off for good)
	if (wanted && !EnsureMap(Device)) return;
	if (!mapTexture[0]) return;

	const double size = Settings.size;
	const long long n = mapResolution;
	const double texelSize = size / (double)n;
	const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
	const long long ox = FloorTexel(camera.x / texelSize) - n / 2;
	const long long oy = FloorTexel(camera.y / texelSize) - n / 2;
	++framesSinceUpdate;

	if (wanted) {
		const bool orthoFresh = CoverMapOrthoFrame == TheFrameRateManager->FrameNumber && sm->orthoRequired;
		const bool pendingWork = pendWetGain > 0.0f || pendWetDry > 0.0f || pendPuddleGain > 0.0f || pendPuddleDry > 0.0f ||
			pendSnowGain > 0.0f || pendSnowMelt > 0.0f;
		const double fx = Player->pos.x, fy = Player->pos.y, fz = Player->pos.z;
		// UNOFFICIAL 2026-10-28 footprints: the walking speed (smoothed over a few frames) sets the stride, 30-100 units (a step of
		// about 0.4-1.4 m: sneaking and walking to running), and a print falls each time the feet have gone that far.
		const double dt = ElapsedOncePerFrame(speedClock);
		if (speedSet && dt > 0.0) {
			const double step = sqrt((fx - speedX) * (fx - speedX) + (fy - speedY) * (fy - speedY));
			if (step < MaxTrail) walkSpeed += 0.15f * ((float)(step / dt) - walkSpeed);
		}
		speedX = fx; speedY = fy; speedSet = true;
		const float stride = std::clamp(walkSpeed * 0.36f, 30.0f, 100.0f);
		const double moved = feetSet ? sqrt((fx - feetX) * (fx - feetX) + (fy - feetY) * (fy - feetY)) : 0.0;
		const bool teleported = !feetSet || moved > MaxTrail;
		if (teleported) ClearPrints();
		const bool footStep = Settings.snow && Settings.footprints > 0.0f && snowBound > 0.0f && !teleported && moved >= stride;
		// The print: beside the path, on this foot's side (half a print's length out: the feet's gait), heading along the last step
		double printX = fx, printY = fy;
		float dirX = 1.0f, dirY = 0.0f;
		if (footStep) {
			dirX = (float)((fx - feetX) / moved); dirY = (float)((fy - feetY) / moved);
			const double out = 0.5 * Settings.footSize * printSide;
			printX = fx - dirY * out;	// left of the heading is (-dirY, dirX)
			printY = fy + dirX * out;
		}
		if (!mapValid || pendingWork || footStep || (orthoFresh && framesSinceUpdate >= RefreshFrames)) {
			if (!mapValid) {
				// every texel starts from the open-ground values as they are now
				referenceZ = camera.z;
				pendWetGain = pendWetDry = pendPuddleGain = pendPuddleDry = pendSnowGain = pendSnowMelt = 0.0f;
				startGlobal = Constants.Global;
			}
			const float nf = (float)n;
			const double shiftX = std::clamp((double)(ox - originX), -2.0 * n, 2.0 * n);
			const double shiftY = std::clamp((double)(oy - originY), -2.0 * n, 2.0 * n);
			Constants.Update = D3DXVECTOR4(nf, 1.0f / nf, (float)texelSize, mapValid ? 1.0f : 0.0f);
			Constants.Origin = D3DXVECTOR4(PositiveMod(ox, n), PositiveMod(oy, n), PositiveMod(originX, n), PositiveMod(originY, n));
			Constants.Shift = D3DXVECTOR4((float)shiftX, (float)shiftY, (float)(ox * texelSize - camera.x), (float)(oy * texelSize - camera.y));
			Constants.Rates = D3DXVECTOR4(pendWetGain, pendWetDry, pendPuddleGain, pendPuddleDry);
			Constants.SnowRates = D3DXVECTOR4(pendSnowGain, pendSnowMelt, Settings.drifts, footStep ? Settings.footprints : 0.0f);
			const D3DXVECTOR4& sun = sm->ShaderConst.SunDir;
			Constants.Sun = D3DXVECTOR4(sun.x, sun.y, sun.z, Saturate(sm->ShaderConst.SunAmount.x));
			// the ortho map's light space back to camera-relative world space (its matrix is this frame's while orthoFresh)
			const D3DXMATRIX& toLight = sm->Effects.ShadowsExteriors->ShadowMaps[ShadowsExteriorEffect::MapOrtho].ShadowCameraToLight;
			const bool inverted = D3DXMatrixInverse(&Constants.OrthoInverse, NULL, &toLight) != NULL;
			Constants.Shade = D3DXVECTOR4(Settings.wetShade, Settings.snowShade, (float)(camera.z - referenceZ), orthoFresh && inverted ? 1.0f : 0.0f);
			Constants.Start = D3DXVECTOR4(startGlobal.x, startGlobal.y, startGlobal.z, 0.0f);
			// 2026-10-28: the step's print as a dot (a stroke of no length; was the path since the last update: the bicycle line)
			Constants.FootA = D3DXVECTOR4((float)(printX - camera.x), (float)(printY - camera.y), (float)(fz - camera.z), Settings.footSize);
			Constants.FootB = D3DXVECTOR4((float)(printX - camera.x), (float)(printY - camera.y), (float)(fz - camera.z), TrailRange(texelSize));

			// One pass into the other pair of maps (it reads the current pair), then they swap.
			bool drawn = false;
			IDirect3DSurface9* depthWas = nullptr;
			IDirect3DSurface9* target1Was = nullptr;
			DWORD scissorWas = FALSE, writeWas1 = 0xF;
			Device->GetDepthStencilSurface(&depthWas);
			if (FAILED(Device->GetRenderTarget(1, &target1Was))) target1Was = nullptr;	// (none bound: D3DERR_NOTFOUND)
			Device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorWas);
			Device->GetRenderState(D3DRS_COLORWRITEENABLE1, &writeWas1);
			Device->SetDepthStencilSurface(nullptr);	// (the screen's depth surface may be smaller than the map)
			Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			Device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
			Device->SetStreamSource(0, sm->FrameVertex, 0, sizeof(FrameVS));	// the frame quad, as the effects draw it
			Device->SetFVF(FrameFVF);
			Device->SetRenderTarget(0, mapSurface[1 - current]);
			Device->SetRenderTarget(1, auxSurface[1 - current]);
			D3DXHANDLE technique = TechniqueHandle(updateHandle, "UpdateTechnique");
			if (technique && SUCCEEDED(Effect->SetTechnique(technique))) {
				SetCT();
				UINT passes = 0;
				if (SUCCEEDED(Effect->Begin(&passes, NULL)) ) {
					if (passes > 0 && SUCCEEDED(Effect->BeginPass(0))) {
						RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the map is not screen-shaped, stays whole
						Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
						Effect->EndPass();
						drawn = true;
					}
					Effect->End();
				}
			}
			Device->SetRenderTarget(0, RenderTarget);
			Device->SetRenderTarget(1, target1Was);	// (after target 0: setting target 0 resets the viewport only)
			Device->SetDepthStencilSurface(depthWas);
			Device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorWas);
			Device->SetRenderState(D3DRS_COLORWRITEENABLE1, writeWas1);
			if (depthWas) depthWas->Release();
			if (target1Was) target1Was->Release();

			if (drawn) {
				current = 1 - current;
				currentTexture = mapTexture[current];
				currentAux = auxTexture[current];
				mapValid = true;
				originX = ox;
				originY = oy;
				startGlobal = Constants.Global;
				// 2026-10-28 footprints: fresh snow fills the prints as the update pass fills the map's trail (3 x the snow gained)
				const float fill = 3.0f * Constants.SnowRates.x;
				if (fill > 0.0f) for (int i = 0; i < MaxPrints; i++) prints[i].depth = (std::max)(0.0f, prints[i].depth - fill);
				pendWetGain = pendWetDry = pendPuddleGain = pendPuddleDry = pendSnowGain = pendSnowMelt = 0.0f;
				framesSinceUpdate = 0;
				if (footStep) {
					prints[printNext] = { printX, printY, fz, dirX, dirY, printSide, 1.0f };	// 1 = as deep as the map's trail (x PersistentFootprints in the reader)
					printNext = (printNext + 1) % MaxPrints;
					printSide = -printSide;
				}
				if (footStep || teleported) { feetX = fx; feetY = fy; feetZ = fz; feetSet = true; }
				if (++updates == 1) Logger::Log("UNOFFICIAL Persistent wetness/snow: first cover map update (wet %d, snow %d, ortho map %s)",
					(int)Settings.wet, (int)Settings.snow, orthoFresh ? "in" : "not drawn this frame");
			}
		}
	}
	SetReaderConstants();
}

void CoverMapEffect::RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* SourceSurface) {
	if (!Effect || !Enabled || !ShouldRender()) return;
	SetReaderConstants();
	Render(Device, RenderTarget, TheTextureManager->RenderedSurface, 1, false, nullptr);	// DebugTechnique (reads the current image only)
}
