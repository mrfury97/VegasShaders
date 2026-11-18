#pragma once

// UNOFFICIAL 2026-10-18 (sandport): ported from Physically-Based-Reloaded (github.com/Balebandro/Physically-Based-Reloaded,
// Balebandro, Alejandro Martinez), GPL-3.0, the same licence as this project: their commits 90ae7541 ('Sandstorm, grass
// interaction ...') and ebcf3c03 ('Sandstorm: dust seen through glass and blended surfaces, and from underwater'). It
// replaces the older sandstorm Fable wrote for PR #1 (shelved 2026-10-13 as too buggy). Changes for this build: the clocks
// run once per rendered frame (UpdateConstants runs twice a frame), the sun's visibility in the dust uses this build's
// cascade lookup (its PCF mode and the cascade centres), the storm wind reaches the grass through c19.y/z (the grass VS has
// no free register: GrassFirst's c19.w and the literals fill it) and the leaves through c134, and it stays OFF by default.
//
// Sandstorm, after Kappa's v5 weather update: while the weather is a sandstorm (any weather whose editor ID holds
// Keyword, "sandstorm,duststorm" by default here, in any case), the air fills with wind-driven dust. Billowing clouds of it are
// marched through near the camera and lit by the sun and sky, a dense haze swallows the distance and the sky, and
// thin streaks of sand blow past. It builds and settles with the weather's own transition.
class SandstormEffect : public EffectRecord
{
public:
	// UNOFFICIAL 2026-11-12 (memleak) idle targets (core/TextureManager.h, EffectRecord::TargetParkable): made on the fast path's first use; drawn and read only behind the switch (EffectChain's wouldRender)
	bool	TargetParkable(const char* SamplerName) override { return true; }
	SandstormEffect() : EffectRecord("Sandstorm") {
		memset(&Constants, 0, sizeof(Constants));
		Constants.LeafStorm = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f);	// no storm until UpdateConstants says so
	};

	struct SandstormSettingsStruct {
		char	Keyword[128];	// a weather whose editor ID holds one of these (comma-separated, any case) is a sandstorm
		float	Visibility;		// how far one sees through the full storm (game units)
		float	HeightFalloff;	// the dust thins with height above the camera over about this many units
		float	Billowing;		// how much the clouds of dust vary (0 an even haze)
		float	NoiseScale;		// size of the dust clouds (game units)
		float	WindSpeed;		// the dust's speed at the weather's full wind (units per second)
		float	MinWindSpeed;	// the dust's speed with no wind
		float	StreakStrength;	// the sand blown past the camera (theirs: Streaks)
		float	DustColor[3];	// the dust's colour (albedo; theirs: SandColor)
		float	SunScattering;	// how much brighter the dust glows toward the sun
		float	Brightness;		// overall light on the dust
		float	FadeSpeed;		// how fast the storm builds and settles beyond the weather's own transition (per second)
		float	GrassWind;		// grass sways this many times harder at the storm's height, leaning downwind in gusts
		float	TreeWind;		// tree leaves likewise
		float	LightScattering;	// how strongly lamps, fires and other nearby lights glow through the dust (0 off)
		float	EmissiveThreshold;	// a pixel this many times brighter than lit dust counts as glowing in front (fire drawn without depth); 0 off
		float	EmissiveDistance;	// how much dust such a pixel is seen through (units)
		float	Steps;			// UNOFFICIAL speed: march steps (theirs 12)
		float	Octaves;		// UNOFFICIAL speed: cloud noise octaves (theirs 3)
		bool	SoftEdges;		// UNOFFICIAL speed: the 3x3 depth tent at edges (theirs on)
		bool	FineShelter;	// UNOFFICIAL speed: 8 upwind taps per streak (theirs on; off 4)
		bool	ThroughBlended;	// glass and other see-through blended surfaces write no depth while the storm blows
		bool	CharactersKeepDepth;	// with ThroughBlended: characters' blended parts keep their depth (Physically-Based-Reloaded 432ed1e6)
		bool	ForceStorm;		// debugging: the full storm outdoors in any weather (theirs: Force)
		bool	FastPath;		// UNOFFICIAL 2026-10-18 (sandfast): the dust on a grid, streaks at half and glow at quarter resolution (0: the full-resolution original)
		float	FixedTime;		// UNOFFICIAL 2026-10-18 (sandfast), testing: >= 0 holds the dust's clock at this many seconds (-1: real time)
	};
	SandstormSettingsStruct	Settings;

	struct SandstormStruct {
		D3DXVECTOR4	Data;		// x: intensity 0-1, y: density per unit, z: visibility, w: height falloff
		D3DXVECTOR4	Wind;		// x, y: wind direction, z: speed (units/s), w: time (s)
		D3DXVECTOR4	Shape;		// x: billowing, y: noise scale, z: streaks, w: sun scattering
		D3DXVECTOR4	Color;		// rgb: sand albedo, w: brightness
		D3DXVECTOR4	Cover;		// x: the ortho map (the top-down depth map) is there, to keep streaks out of the lee of walls and rocks
		D3DXVECTOR4	Lights;		// x: LightScattering
		D3DXVECTOR4	Emissive;	// x: EmissiveThreshold, y: EmissiveDistance
		D3DXVECTOR4	Water;		// x: 1 with the camera underwater, y: the water surface's height above the camera
		D3DXVECTOR4	Quality;	// UNOFFICIAL speed settings: x steps, y octaves, z soft edges, w fine shelter
		D3DXVECTOR4	LeafStorm;	// the leaf VS (STLEAF001/003 c134): x leaf swing scale (1 no storm), y flutter amount (0 no storm), z flutter phase (rad, -pi..pi)
		D3DXVECTOR4	GridLayout;	// UNOFFICIAL sandfast: xy grid columns and rows, zw one atlas texel in UV
		D3DXVECTOR4	LowLayout;	// UNOFFICIAL sandfast: xy one half-resolution texel in screen UV
	};
	SandstormStruct	Constants;

	// The grass VS's storm wind, written into c19.y and c19.z by GrassShaders::SetForDraw (no free register there): y the extra
	// sway (0 no storm: the game's sway exactly), z the gust phase (0..8 pi; the flutter runs 4x as fast, so both wrap together).
	float	GrassStorm[2] = { 0.0f, 0.0f };

	float	Intensity = 0.0f;		// how much the storm blows now, eased
	double	Time = 0.0;				// seconds, real time, advanced once per rendered frame
	unsigned int LastFrame = 0;		// FrameRateManager::FrameNumber when the clocks last moved
	void*	LastWeather = nullptr;	// the weathers last checked by name, and what they were
	void*	LastSecond = nullptr;
	bool	FirstIsStorm = false;
	bool	SecondIsStorm = false;
	void*	LoggedWeather = nullptr;	// the weathers last written to the log
	void*	LoggedSecond = nullptr;

	// While the storm blows (ThroughBlended), glass and other see-through blended surfaces leave the world's depth to what
	// lies behind them, so the dust is measured to that (SandstormGlass in Render.cpp).
	// fixes-a #32: the same threshold the dust draws at (ShouldRender, > 0.001), so glass never goes without depth for a frame with no dust
	bool	HidesBlendedDepth() { return Enabled && Effect && Settings.ThroughBlended && ShouldRender(); }
	bool	Underwater = false;		// the camera is underwater (the storm only above the surface)

	// UNOFFICIAL 2026-10-18 (sandfast): the fast path's buffers (made the first time it draws: nothing while the effect is off).
	// Grid / GridTemp: the dust grid atlas (one column per 16 x 16 pixels, 64 slices in 8 x 8 tiles); Streaks: half resolution; Glow: a quarter.
	struct TexturesStruct {
		IDirect3DTexture9*	GridTexture = nullptr;
		IDirect3DSurface9*	GridSurface = nullptr;
		IDirect3DTexture9*	GridTempTexture = nullptr;
		IDirect3DSurface9*	GridTempSurface = nullptr;
		IDirect3DTexture9*	StreaksTexture = nullptr;
		IDirect3DSurface9*	StreaksSurface = nullptr;
		IDirect3DTexture9*	GlowTexture = nullptr;
		IDirect3DSurface9*	GlowSurface = nullptr;
	};
	TexturesStruct	Textures;
	bool	TexturesTried = false;	// made once; if that failed the reference path draws
	int		TexturesFailed = 0;		// fixes-a #5: failed attempts (a partial set is released; tried again, at most 3 times)
	bool	UseFastPath();			// the fast path this frame (FastPath on, not underwater, its buffers there)
	void	RenderBuffers(IDirect3DDevice9* Device);	// the grid, streaks and glow passes, before the composite (technique 5)
	void	RegisterTextures();
	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
	bool	IsSandstorm(TESWeather* Weather);
	void	ClearWind();			// no storm wind on the grass and leaves (the effect off, or no storm)

	// UNOFFICIAL 2026-10-25 (NVR DXVK): every technique as src\hlsl\NewVegas\Compute\Sandstorm<Pass>.frag (EffectRecord::ExtRender):
	// the dust grid, its two sums, the streaks and the glow into the effect's own buffers (ExtDrawTo), the composite and the
	// reference through the frame chain. The matrices and the lamp arrays go to one uniform buffer (they do not fit push data).
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	ExtPass				extPasses[7];	// by technique
	ExtUniformBuffer	extUniforms;
};
