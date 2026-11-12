#include <algorithm>

#include "Puddles.h"
#include "PuddleMap.h"

// 2026-11-08 Puddles: see README.md here.

void PuddlesShaders::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_PuddleData", &Constants.Data);
	TheShaderManager->RegisterConstant("TESR_PuddleLook", &Constants.Look);
	TheShaderManager->RegisterConstant("TESR_PuddleShape", &Constants.Shape);
	TheShaderManager->RegisterConstant("TESR_PuddleStreak", &Constants.Streak);
	PuddleMap::RegisterConstants();	// 2026-11-12 map mode: TESR_PuddleMapNear / Far
}

void PuddlesShaders::UpdateSettings() {
	const char* main = "Shaders.Puddles.Main";
	const char* debug = "Shaders.Puddles.Debug";
	Settings.Quality = std::clamp(TheSettingManager->GetSettingI(main, "Quality"), 0, 3);
	Settings.Coverage = std::clamp(TheSettingManager->GetSettingF(main, "Coverage"), 0.0f, 1.0f);
	Settings.Size = std::clamp(TheSettingManager->GetSettingF(main, "Size"), 32.0f, 8192.0f);
	Settings.FillHours = std::clamp(TheSettingManager->GetSettingF(main, "FillHours"), 0.01f, 48.0f);
	Settings.DryHours = std::clamp(TheSettingManager->GetSettingF(main, "DryHours"), 0.01f, 96.0f);
	Settings.NightDrying = std::clamp(TheSettingManager->GetSettingF(main, "NightDrying"), 0.0f, 1.0f);
	Settings.Roughness = std::clamp(TheSettingManager->GetSettingF(main, "Roughness"), 0.01f, 0.5f);
	Settings.Darkening = std::clamp(TheSettingManager->GetSettingF(main, "Darkening"), 0.0f, 2.0f);
	Settings.EdgeSoftness = std::clamp(TheSettingManager->GetSettingF(main, "EdgeSoftness"), 0.005f, 0.5f);
	Settings.SlopeStart = std::clamp(TheSettingManager->GetSettingF(main, "SlopeStart"), 0.5f, 0.999f);
	Settings.SlopeFull = std::clamp(TheSettingManager->GetSettingF(main, "SlopeFull"), Settings.SlopeStart + 0.001f, 1.0f);
	Settings.Cavity = std::clamp(TheSettingManager->GetSettingF(main, "Cavity"), 0.0f, 2.0f);
	Settings.WetRing = std::clamp(TheSettingManager->GetSettingF(main, "WetRing"), 0.0f, 0.5f);
	Settings.Ripples = std::clamp(TheSettingManager->GetSettingF(main, "Ripples"), 0.0f, 2.0f);
	Settings.Streaks = std::clamp(TheSettingManager->GetSettingF(main, "Streaks"), 0.0f, 2.0f);
	Settings.StreakWidth = std::clamp(TheSettingManager->GetSettingF(main, "StreakWidth"), 1.0f, 64.0f);
	Settings.StreakSpeed = std::clamp(TheSettingManager->GetSettingF(main, "StreakSpeed"), 0.0f, 8.0f);
	Settings.ReflectionPuddles = TheSettingManager->GetSettingI(main, "ReflectionPuddles") != 0;
	Settings.LevelOverride = TheSettingManager->GetSettingF(debug, "LevelOverride");
	Settings.DebugView = std::clamp(TheSettingManager->GetSettingI(debug, "DebugView"), 0, 5);
}

static float Ramp(float x) { return std::clamp(x, 0.0f, 1.0f); }

// How hard it rains, 0-1, by the engine's rule (IDA Sky::GetIsRaining 0x59E950): the current weather is rainy (TESWeather::IsRainy,
// data byte 11 flag 4) and the weather percent is past its begin-precipitation byte (6) / 255 x 0.999, or the last weather is rainy and
// the percent is below its end-precipitation byte (7) / 255 x 0.999 + 0.001. Here it ramps from 0.3 at those thresholds to 1.
static float RainNow(Sky* sky) {
	if (!sky) return 0.0f;
	const float pct = std::clamp(sky->weatherPercent, 0.0f, 1.0f);
	float rain = 0.0f;
	if (TESWeather* cur = sky->firstWeather; cur && (cur->weatherType & TESWeather::kType_Rainy)) {
		const float begin = cur->padE6[0] / 255.0f * 0.999f;
		if (pct > begin) rain = max(rain, 0.3f + 0.7f * Ramp((pct - begin) / max(1.0f - begin, 0.05f)));
	}
	if (TESWeather* last = sky->secondWeather; last && last != sky->firstWeather && (last->weatherType & TESWeather::kType_Rainy)) {
		const float end = last->padE6[1] / 255.0f * 0.999f + 0.001f;
		if (pct < end) rain = max(rain, 0.3f + 0.7f * Ramp((end - pct) / max(end, 0.05f)));
	}
	return rain;
}

void PuddlesShaders::UpdateConstants() {
	const bool exterior = TheShaderManager->GameState.isExterior;
	Sky* sky = Tes ? Tes->sky : nullptr;

	// Game hours (GameDaysPassed grows by hours / 24 as the clock runs, IDA Calendar::Update 0x867A40; waiting and sleeping count, paused menus do not).
	TimeGlobals* g = TimeGlobals::Get();
	double hoursNow = g && g->GameDaysPassed ? (double)g->GameDaysPassed->data * 24.0 : -1.0;
	double hours = 0.0;
	if (hoursNow >= 0.0) {
		if (LastHours >= 0.0) hours = hoursNow - LastHours;
		if (hours < 0.0 || hours > 48.0) hours = 0.0;	// a load or a jump back: start again from here
		LastHours = hoursNow;
	}

	// The water level: fills in the rain, dries after it (faster in daylight under a clear sky). Held indoors (no weather there).
	if (exterior && sky) {
		Rain = RainNow(sky);
		if (Rain > 0.0f) Level += (float)(Rain * hours / Settings.FillHours);
		else {
			const TESWeather* cur = sky->firstWeather;
			const float overcast = cur && (cur->weatherType & (TESWeather::kType_Cloudy | TESWeather::kType_Rainy)) ? 1.0f : 0.0f;
			const float sun = TheShaderManager->GameState.isDayTime * (1.0f - 0.6f * overcast);
			Level -= (float)(hours / Settings.DryHours) * (Settings.NightDrying + (1.0f - Settings.NightDrying) * sun);
		}
		Level = std::clamp(Level, 0.0f, 1.0f);
	}
	const float level = Settings.LevelOverride >= 0.0f ? std::clamp(Settings.LevelOverride, 0.0f, 1.0f) : Level;

	const bool on = Enabled && Settings.Quality > 0;
	WetnessEffectsShaders* wetness = TheShaderManager->Shaders.Wetness;
	const float raining = wetness ? wetness->Raining : Rain;

	Constants.Data = D3DXVECTOR4(on && exterior ? level : 0.0f, Settings.Coverage, 1.0f / Settings.Size, on ? (float)Settings.Quality : 0.0f);
	Constants.Look = D3DXVECTOR4(Settings.Roughness, Settings.Darkening, Settings.EdgeSoftness, Settings.Ripples);
	Constants.Shape = D3DXVECTOR4(Settings.SlopeStart, Settings.SlopeFull, Settings.Cavity, Settings.WetRing);
	PuddleMap::UpdateConstants();	// 2026-11-12 map mode: the built windows
	Constants.Streak = D3DXVECTOR4(Settings.Streaks * raining, 1.0f / Settings.StreakWidth, Settings.StreakSpeed, on ? (float)Settings.DebugView : 0.0f);
}

bool PuddlesShaders::Active() const {
	return Enabled && Settings.Quality > 0 && TheShaderManager->GameState.isExterior && (Constants.Data.x > 0.001f || Settings.DebugView > 0);
}

bool PuddlesShaders::ReflectionsWanted() const {
	return Enabled && Settings.Quality > 0 && Settings.ReflectionPuddles && TheShaderManager->GameState.isExterior && Constants.Data.x > 0.001f;
}

void PuddlesShaders::ReflectionData(D3DXVECTOR4& puddle, D3DXVECTOR4& puddle2) const {
	if (!ReflectionsWanted()) { puddle = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f); puddle2 = puddle; return; }
	puddle = Constants.Data;
	puddle2 = D3DXVECTOR4(Settings.SlopeStart, Settings.SlopeFull, Settings.Roughness, Settings.EdgeSoftness);
}

// 2026-11-12 (user: walking into and out of puddles put water on the first-person arms) the first-person model's draws read the world's
// puddle constants, and its positions are posed round the camera, so the field / map gave its upward faces standing water and the soaked
// rim wherever the player stood near a puddle. For every first-person pass the bound record's TESR_PuddleData goes out with level 0
// (no standing water, no rim; the rain's dampness and HudRaindrops' drops stay). SetCT uploads only when the pixel shader changes, so
// the first world pass afterwards puts the real value back on the register last zeroed.
void PuddlesShaders::SetDrawConstants(bool firstPerson) {
	static int zeroedReg = -1;
	ShaderRecordPixel* record = ShaderRecordPixel::Current;
	const int reg = record ? record->PuddleDataRegister() : -1;
	if (firstPerson) {
		if (reg < 0 || Constants.Data.x <= 0.0f) return;
		D3DXVECTOR4 dry = Constants.Data;
		dry.x = 0.0f;
		TheRenderManager->device->SetPixelShaderConstantF(reg, (const float*)&dry, 1);
		zeroedReg = reg;
	}
	else if (zeroedReg >= 0) {
		if (reg == zeroedReg) TheRenderManager->device->SetPixelShaderConstantF(reg, (const float*)&Constants.Data, 1);
		zeroedReg = -1;
	}
}
