#pragma once

// Precipitations (rain). Technique 0: the original screen-space layers (Mode 0). Technique 1: the UNOFFICIAL 2026-11-05 rain system
// (Mode 1, default): world-anchored streak volume, wind, lamp/sun scattering, rain haze and lens drops (Effects\Includes\RainSystem.hlsl,
// README.md in this folder, docs\rain\DESIGN.md).
class RainEffect : public EffectRecord
{
public:
	RainEffect() : EffectRecord("Precipitations") {
		Constants.Data = D3DXVECTOR4(0, 0, 0, 0);
		Constants.RainAnimator.Initialize(0);
		ZeroMemory(&Sys, sizeof(Sys));
	};

	struct RainStruct {
		Animator		RainAnimator;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Aspect;
	};
	RainStruct	Constants;

	static const int Bands = 3;
	static const int LampsMax = 8;

	// [Shaders.Precipitations.System]
	struct SystemSettings {
		int		Mode;			// 0 original layers, 1 rain system
		int		Quality;		// 0 low (1 band), 1 medium (2), 2 high (3), 3 ultra (3, longer)
		float	Intensity;		// multiplies the weather's rain amount (density, haze)
		float	Density;		// drop chance per column slot at full rain
		float	Exposure;		// seconds: streak length = fall speed x exposure
		float	StreakLength;	// multiplier on the streak length
		float	Width;			// drop width multiplier
		float	Opacity;
		float	EnvLift;		// how much brighter than the environment a drop is
		float	SunGlint;
		float	LampGlint;
		float	SkyShare;		// share of the sky's fog colour in the environment a drop shows
		float	FallSpeed;		// multiplier on the physical fall speeds
		float	WindScale;		// game wind 0..1 -> horizontal speed (units/s) at 1
		float	Gusts;			// gust strength 0..1
		float	Haze;			// rain haze amount at full rain
		float	HazeExtinction;	// per game unit at full rain
		float	Curtains;		// 0..1 curtain modulation of the haze
		float	NearDistance;	// end of the first band (units)
		float	FarDistance;	// end of the last band (units)
		float	FarBoost;		// coverage multiplier of the far bands (their drops are mostly thinner than a pixel)
		float	LensDrops;		// 0 off; strength of the lens drops
		bool	LensFirstPerson;
		float	LensSize;
		int		DebugView;
	};

	struct SystemConstants {
		D3DXVECTOR4	Band[Bands];
		D3DXVECTOR4	Origin[Bands];
		D3DXVECTOR4	Fall[Bands];
		D3DXVECTOR4	Streak;
		D3DXVECTOR4	Shape;
		D3DXVECTOR4	Light;
		D3DXVECTOR4	Haze;
		D3DXVECTOR4	HazeDrift;
		D3DXVECTOR4	Lens;
		D3DXVECTOR4	Misc;
		D3DXVECTOR4	Pass;		// x pixel size scale (2: half resolution), y first band, z bands end
		D3DXVECTOR4	LampPosition[LampsMax];
		D3DXVECTOR4	LampColor[LampsMax];
	};

	struct SystemState {
		SystemSettings	S;
		SystemConstants	C;
		double	fall[Bands][3];		// fall offsets (units, wrapped at each band's period)
		double	driftX, driftY;		// haze curtain drift (units, wrapped)
		double	gustClock;
		double	lensClock;
		double	lastFrameTime;
		float	lensWet;			// 0..1 how many drops sit on the lens
		float	windX, windY;		// the wind now, units/s
		int		seenLamps;
		double	lastLogTime;
		double	shearOffX, shearOffY;	// rain-space offset that keeps the lattice still round the eye when the shear changes
		float	lastShearX, lastShearY;
		bool	shearSet;
		int		bands;					// bands drawn this frame (0: none)
		bool	farInMain;				// Quality 3: far bands at full resolution in the main pass
		bool	lensWanted;				// lens drops are wetting: the WetWorld cover test should run
	};
	SystemState	Sys;

	struct TexturesStruct {
		IDirect3DTexture9*	FarTexture = nullptr;	// half resolution: the far bands' coverage
		IDirect3DSurface9*	FarSurface = nullptr;
	};
	TexturesStruct	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	UINT	RainTechnique() const { return Sys.S.Mode == 1 ? 1 : 0; }
	// The frame's rain (EffectChain.cpp): the far pass (Mode 1), then the main pass through the chain.
	void	RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* SourceSurface);
	// Lens drops are wetting: EffectChain runs WetWorld's cover test for them even without HudDrops.
	bool	LensWantsCover() const { return Enabled && Sys.S.Mode == 1 && Sys.lensWanted; }
	// UNOFFICIAL 2026-10-25 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\Precipitations.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
private:
	ExtPass				extPass;
	void	UpdateSystem(float rain);
	void	PickLamps();
	void	LogSystem();
};
