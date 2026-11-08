#pragma once

// A "fun" trip look for the finished picture, off by default (sidebar: Shaders > Extras > Dream). Ported from the author's own
// DreamSim shaders (README.md): the picture warps and breathes, colours split and drift, the last frames linger as tracers,
// a still view falls into a tunnel, and now and then the scene melts into a Julia fractal or breaks into glitch blocks.
// One "dream level" drives it all; the level breathes by itself, and low health can deepen it.
class DreamEffect : public EffectRecord
{
public:
	DreamEffect() : EffectRecord("Dream") {};

	struct SettingsStruct {
		float	Level;			// 0-1, the dream level set by the player
		bool	Breathing;		// the level rises and falls slowly by itself
		float	BreathingSpeed;
		float	Warp, ColourSplit, HueDrift, Saturation;	// strengths (1 = DreamSim)
		float	Tracers, Tunnel, TunnelDelay;
		float	Melt, MeltInterval;							// MeltInterval: minutes between melts on average (0 = never)
		float	Macroblocks, Datamosh, GlitchInterval;		// GlitchInterval: seconds between glitch bursts on average (0 = never)
		float	Vignette, Dither, PixelSize;
		float	LowHealth;		// how much low health deepens the dream (0 = not at all)
	};
	SettingsStruct	Settings = {};

	struct ConstantsStruct {
		D3DXVECTOR4	Level;		// x dream level now, y seconds, z hue angle, w warp
		D3DXVECTOR4	Trails;		// x tracer mix, y tunnel, z datamosh chance, w macroblock chance
		D3DXVECTOR4	Melt;		// x melt, y fractal variant, z fractal zoom, w colour split (uv)
		D3DXVECTOR4	Finish;		// x vignette, y dither, z extra saturation, w unused
		D3DXVECTOR4	Grid;		// xy low-res grid size, z macroblock seed, w datamosh seed
	};
	ConstantsStruct	Constants = {};

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender() override;
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex,
		bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

private:
	bool	EnsureHistory(IDirect3DDevice9* Device);
	void	ReleaseHistory();
	float	Random();	// 0-1

	// The tracer history: two screen-size targets, one read (last frame's mix) and one written each frame. Made the first time
	// the mix pass runs, so the effect costs no memory until then; TESR_DreamHistory binds a 4 x 4 stand-in before that.
	IDirect3DTexture9*	StandIn = nullptr;
	IDirect3DSurface9*	StandInSurface = nullptr;
	IDirect3DTexture9*	HistoryTexture[2] = {};
	IDirect3DSurface9*	HistorySurface[2] = {};
	IDirect3DTexture9*	HistoryRead = nullptr;
	int					Read = 0;
	UINT				HistoryWidth = 0, HistoryHeight = 0;
	bool				HistoryValid = false, HistoryFailed = false;

	double	Clock = 0.0;			// the effect's own seconds (advances once a frame while the effect is on)
	double	LastFrameTime = -1.0;
	double	LastRender = -1.0;
	double	BreathPhase = 0.0;
	bool	Active = false;			// the dream level is above 0 this frame

	// The random events, as in DreamSim: the fractal melt (in 4 s, held 2-4 s, out 3.5 s) and glitch bursts.
	double	NextMelt = -1.0, MeltStart = -1.0e9, MeltHold = 3.0;
	float	MeltVariant = 1.0f;
	double	NextGlitch = -1.0, GlitchUntil = -1.0;
	// The stare: how long the view has been still, and the tunnel shown (eases out when you move again).
	float	StareTime = 0.0f, StareShown = 0.0f;
	float	LastYaw = 0.0f, LastPitch = 0.0f;
	D3DXVECTOR3	LastPos = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	bool	HaveLast = false;
	UInt32	Seed = 0x2545F491u;
};
