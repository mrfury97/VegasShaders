// A "fun" trip look, off by default (Dream.h, README.md). Ported from the author's own DreamSim game: the timings and
// curves below follow DreamSim's main.gd, the picture is src/hlsl/NewVegas/Effects/Dream.fx.hlsl.

#include <algorithm>
#include <cmath>

#include "Dream.h"

namespace {
	const char* const kSection = "Shaders.Dream.Main";
	const double kMeltIn = 4.0, kMeltOut = 3.5;	// seconds the melt takes to swallow the view and to let it go

	float Smooth(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
}

float DreamEffect::Random() {
	Seed ^= Seed << 13; Seed ^= Seed >> 17; Seed ^= Seed << 5;	// xorshift32
	return (float)(Seed >> 8) / 16777216.0f;
}

void DreamEffect::UpdateSettings() {
	SettingManager* cfg = TheSettingManager;
	SettingsStruct& s = Settings;
	s.Level = std::clamp(cfg->GetSettingF(kSection, "Level"), 0.0f, 1.0f);
	s.Breathing = cfg->GetSettingI(kSection, "Breathing") != 0;
	s.BreathingSpeed = std::clamp(cfg->GetSettingF(kSection, "BreathingSpeed"), 0.0f, 5.0f);
	s.Warp = std::clamp(cfg->GetSettingF(kSection, "Warp"), 0.0f, 3.0f);
	s.ColourSplit = std::clamp(cfg->GetSettingF(kSection, "ColourSplit"), 0.0f, 3.0f);
	s.HueDrift = std::clamp(cfg->GetSettingF(kSection, "HueDrift"), 0.0f, 3.0f);
	s.Saturation = std::clamp(cfg->GetSettingF(kSection, "Saturation"), 0.0f, 3.0f);
	s.Tracers = std::clamp(cfg->GetSettingF(kSection, "Tracers"), 0.0f, 1.5f);
	s.Tunnel = std::clamp(cfg->GetSettingF(kSection, "Tunnel"), 0.0f, 1.0f);
	s.TunnelDelay = std::clamp(cfg->GetSettingF(kSection, "TunnelDelay"), 0.0f, 120.0f);
	s.Melt = std::clamp(cfg->GetSettingF(kSection, "Melt"), 0.0f, 1.0f);
	s.MeltInterval = std::clamp(cfg->GetSettingF(kSection, "MeltInterval"), 0.0f, 60.0f);
	s.Macroblocks = std::clamp(cfg->GetSettingF(kSection, "Macroblocks"), 0.0f, 3.0f);
	s.Datamosh = std::clamp(cfg->GetSettingF(kSection, "Datamosh"), 0.0f, 3.0f);
	s.GlitchInterval = std::clamp(cfg->GetSettingF(kSection, "GlitchInterval"), 0.0f, 3600.0f);
	s.Vignette = std::clamp(cfg->GetSettingF(kSection, "Vignette"), 0.0f, 1.5f);
	s.Dither = std::clamp(cfg->GetSettingF(kSection, "Dither"), 0.0f, 1.0f);
	s.PixelSize = std::clamp(cfg->GetSettingF(kSection, "PixelSize"), 0.0f, 32.0f);
	s.LowHealth = std::clamp(cfg->GetSettingF(kSection, "LowHealth"), 0.0f, 1.0f);
}

void DreamEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_DreamLevel", &Constants.Level);
	TheShaderManager->RegisterConstant("TESR_DreamTrails", &Constants.Trails);
	TheShaderManager->RegisterConstant("TESR_DreamMelt", &Constants.Melt);
	TheShaderManager->RegisterConstant("TESR_DreamFinish", &Constants.Finish);
	TheShaderManager->RegisterConstant("TESR_DreamGrid", &Constants.Grid);
}

void DreamEffect::RegisterTextures() {
	TheTextureManager->InitTexture("NVR_DreamStandIn", &StandIn, &StandInSurface, 4, 4, D3DFMT_A8R8G8B8);
	HistoryRead = StandIn;
	TheTextureManager->RegisterTexture("TESR_DreamHistory", (IDirect3DBaseTexture9**)&HistoryRead);
}

void DreamEffect::UpdateConstants() {
	// Called twice a frame (before the world and before the post effects): everything advances once a frame.
	const double frameTime = TheFrameRateManager->Time;
	if (frameTime == LastFrameTime) return;
	const double elapsed = LastFrameTime < 0.0 ? 0.016 : frameTime - LastFrameTime;
	LastFrameTime = frameTime;
	const float dt = (float)std::clamp(elapsed, 0.0, 0.1);
	Clock += dt;
	const float t = (float)Clock;
	const SettingsStruct& s = Settings;
	const auto& game = TheShaderManager->GameState;

	// The level: the player's, breathing slowly around it (more when deeper in), plus low health if asked.
	float breath = 0.0f;
	if (s.Breathing) {
		BreathPhase += dt * s.BreathingSpeed;
		const float b = (float)BreathPhase;
		breath = (0.05f + 0.1f * s.Level) * (0.65f * sinf(b * 0.23f) + 0.35f * sinf(b * 0.61f + 1.7f));
	}
	float hurt = 0.0f;
	if (s.LowHealth > 0.0f && Player && Player->IsAlive()) {
		const float base = (float)Player->GetBaseActorValue(Actor::ActorVal::kActorVal_Health);
		if (base > 0.0f) {
			const float health = (float)Player->GetActorValue(Actor::ActorVal::kActorVal_Health) / base;
			hurt = s.LowHealth * Smooth((0.5f - health) / 0.4f);	// none above half health, all of it at a tenth
		}
	}
	const float dream = std::clamp(s.Level + breath + hurt, 0.0f, 1.0f);
	Active = dream > 0.001f;
	const float deep = powf(dream, 1.5f);

	// The melt: now and then (every few minutes, sooner the deeper the dream).
	if (Active && s.Melt > 0.0f && s.MeltInterval > 0.0f) {
		const double meltEnd = MeltStart + kMeltIn + MeltHold + kMeltOut;
		const auto interval = [&] { return s.MeltInterval * 60.0 * (0.5714 + 0.8571 * Random()) / (1.0 + dream); };
		if (NextMelt < 0.0) NextMelt = Clock + interval();
		if (Clock >= NextMelt && Clock > meltEnd) {
			MeltStart = Clock;
			MeltHold = 2.0 + 2.0 * Random();
			MeltVariant = 1.0f + floorf(Random() * 4.0f);
			NextMelt = Clock + interval();
		}
	}
	else NextMelt = -1.0;
	float melt = 0.0f, zoom = 1.0f;
	const double since = Clock - MeltStart;
	if (since >= 0.0 && since <= kMeltIn + MeltHold + kMeltOut) {
		const double k = since < kMeltIn ? since / kMeltIn : since < kMeltIn + MeltHold ? 1.0 : 1.0 - (since - kMeltIn - MeltHold) / kMeltOut;
		melt = Smooth(Smooth((float)k)) * s.Melt;	// eased twice: barely moves at first, glides at the top
		zoom = expf(0.42f * (float)(std::max)(0.0, since - kMeltIn * 0.5));
	}

	// Glitch bursts (macroblocks and datamosh), 0.4-1.8 s each.
	if (Active && s.GlitchInterval > 0.0f && (s.Macroblocks > 0.0f || s.Datamosh > 0.0f)) {
		const auto interval = [&] { return s.GlitchInterval * (0.53 + 0.94 * Random()) / (1.0 + dream); };
		if (NextGlitch < 0.0) NextGlitch = Clock + interval();
		if (Clock >= NextGlitch) {
			GlitchUntil = Clock + 0.4 + 1.4 * Random();
			NextGlitch = Clock + interval();
		}
	}
	else NextGlitch = -1.0;
	const float glitch = Clock < GlitchUntil ? 1.0f : 0.0f;

	// The stare: the view held still (not turning, not walking) for TunnelDelay seconds starts falling into itself over 20 s;
	// it eases back out when you move. Not under the game's menus (Pip-Boy, VATS, dialogue, terminals).
	float yaw = 0.0f, pitch = 0.0f;
	D3DXVECTOR3 pos(0.0f, 0.0f, 0.0f);
	if (Player) {
		yaw = Player->rot.z; pitch = Player->rot.x;
		pos = D3DXVECTOR3(Player->pos.x, Player->pos.y, Player->pos.z);
	}
	bool still = false;
	if (HaveLast && dt > 0.0f) {
		float turn = fabsf(fmodf(yaw - LastYaw + 3.0f * D3DX_PI, 2.0f * D3DX_PI) - D3DX_PI) + fabsf(pitch - LastPitch);
		const D3DXVECTOR3 step = pos - LastPos;
		still = turn / dt < 0.12f && D3DXVec3Length(&step) / dt < 20.0f;	// under ~7 degrees and ~20 units (30 cm) a second
	}
	LastYaw = yaw; LastPitch = pitch; LastPos = pos; HaveLast = Player != nullptr;
	const bool menus = game.PipBoyIsOn || game.VATSIsOn || game.isDialog || game.OverlayIsOn;
	if (!Active || s.Tunnel <= 0.0f || menus) still = false;
	StareTime = still ? StareTime + dt : 0.0f;
	const float stare = Smooth((StareTime - s.TunnelDelay) / 20.0f);
	StareShown = stare >= StareShown ? stare : (std::max)(stare, StareShown - dt * 0.4f);

	const float fade = (std::min)(1.0f, dream * 4.0f);	// the constant parts (vignette, dither, base split) come in over the first quarter
	const float sq = dream * dream;
	Constants.Level = D3DXVECTOR4(dream, t, deep * 1.2f * sinf(t * 0.07f) * s.HueDrift, sq * s.Warp);
	Constants.Trails = D3DXVECTOR4((std::min)(0.6f * deep * s.Tracers, 0.95f), StareShown * s.Tunnel,
		glitch * 0.3f * s.Datamosh, glitch * 0.22f * s.Macroblocks);
	Constants.Melt = D3DXVECTOR4(melt, MeltVariant, zoom, (0.0008f * fade + sq * 0.004f) * s.ColourSplit);
	Constants.Finish = D3DXVECTOR4(0.55f * fade * s.Vignette, fade * s.Dither, 0.8f * dream * s.Saturation, 0.0f);
	// DreamSim draws a 240-line picture: its blocks and dither follow that grid (PixelSize 0), or PixelSize screen pixels.
	const float width = (float)TheRenderManager->width, height = (std::max)((float)TheRenderManager->height, 1.0f);
	const float pixel = s.PixelSize > 0.0f ? (std::max)(s.PixelSize, 1.0f) : (std::max)(height / 240.0f, 1.0f);
	Constants.Grid = D3DXVECTOR4(width / pixel, height / pixel, (float)fmod(floor(Clock * 13.0), 991.0), (float)fmod(floor(Clock * 9.0), 991.0));
}

// Nothing at all while the level is 0 (Level 0 and no breathing, or breathing below 0), unless a melt is still running.
bool DreamEffect::ShouldRender() {
	return Active || Constants.Melt.x > 0.0f;
}

void DreamEffect::ReleaseHistory() {
	for (int i = 0; i < 2; i++) {
		if (HistorySurface[i]) { HistorySurface[i]->Release(); HistorySurface[i] = nullptr; }
		if (HistoryTexture[i]) { HistoryTexture[i]->Release(); HistoryTexture[i] = nullptr; }
	}
	HistoryRead = StandIn;
	HistoryWidth = HistoryHeight = 0;
	HistoryValid = false;
}

bool DreamEffect::EnsureHistory(IDirect3DDevice9* Device) {
	D3DSURFACE_DESC desc;
	IDirect3DSurface9* picture = TheTextureManager->RenderedSurface;
	if (!picture || FAILED(picture->GetDesc(&desc))) return false;
	if (HistorySurface[0] && HistorySurface[1] && desc.Width == HistoryWidth && desc.Height == HistoryHeight) return true;
	if (HistoryFailed) return false;
	ReleaseHistory();
	// Half floats: an 8-bit history would leave the last step of a fading tracer stuck on screen.
	for (int i = 0; i < 2; i++) {
		if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &HistoryTexture[i], NULL))) {
			HistoryTexture[i] = nullptr;
			Logger::Log("[ERROR] Dream: could not create the tracer history (%ux%u); tracers, tunnel and datamosh stay off", desc.Width, desc.Height);
			ReleaseHistory();
			HistoryFailed = true;
			return false;
		}
		HistoryTexture[i]->GetSurfaceLevel(0, &HistorySurface[i]);
	}
	HistoryWidth = desc.Width;
	HistoryHeight = desc.Height;
	Read = 0;
	HistoryValid = false;
	Logger::Log("Dream: tracer history made (%ux%u)", desc.Width, desc.Height);
	return true;
}

// Two passes when tracers, the tunnel or datamosh are live: the mix (this frame and the history) into the other history target,
// then the post pass from that into the chain. Otherwise the post pass alone, straight from the picture.
void DreamEffect::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex,
	bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (!Enabled || !Effect || !ShouldRender()) { EffectRecord::Render(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer); return; }
	// The history lingers only across consecutive drawn frames in the same place (not after a pause, a menu or a door).
	const bool continuous = LastRender >= 0.0 && Clock - LastRender < 0.25 && !TheShaderManager->GameState.isCameraCut;
	LastRender = Clock;
	const D3DXVECTOR4 trails = Constants.Trails;
	const bool mix = trails.x > 0.0f || trails.y > 0.0f || trails.z > 0.0f;
	if (mix && EnsureHistory(Device)) {
		TheShaderManager->Chain.Owns(RenderTarget, RenderedSurface);	// (puts back a picture an earlier effect wrote straight to the screen)
		if (!HistoryValid || !continuous) { Constants.Trails.x = 0.0f; Constants.Trails.z = 0.0f; }	// nothing to linger or smear yet
		const int write = 1 - Read;
		HistoryRead = HistoryTexture[Read];
		Device->SetRenderTarget(0, HistorySurface[write]);
		EffectRecord::Render(Device, HistorySurface[write], HistorySurface[write], 0, false, HistorySurface[write]);
		Device->SetRenderTarget(0, RenderTarget);
		Read = write;
		HistoryRead = HistoryTexture[Read];	// this frame's mix, read by the post pass and kept for the next frame
		HistoryValid = true;
		Constants.Trails = trails;
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, 1, ClearRenderTarget, SourceBuffer);
	}
	else {
		HistoryValid = false;
		EffectRecord::Render(Device, RenderTarget, RenderedSurface, 2, ClearRenderTarget, SourceBuffer);
	}
}
