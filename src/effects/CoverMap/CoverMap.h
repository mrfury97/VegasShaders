#pragma once

// UNOFFICIAL 2026-10-27 Persistent wetness and snow cover (deep/fork2-wetsnow). [Shaders.SnowAccumulation.Main] Persistent, off by
// default. (2026-11-21: the wet half, [Shaders.WetWorld.Main] Persistent, went with WetWorld, its only reader: Settings.wet is always
// false and its model is dormant; the map's size, resolution and debug view are SnowAccumulation's keys now.) Both off by default (off = exactly the old behaviour: the old techniques, the old
// constants, nothing of this runs or is allocated).
//
// The cover map (Effects\CoverMap.fx.hlsl): a top-down map round the camera that keeps, per spot, how wet the top surface is, how
// full its puddles are, how deep its snow lies and how hard that snow has been trodden (one texture, the readers'), plus that
// surface's height and whether it lay in shade (a second texture drawn in the same pass). Heights come from the ortho map (the
// top-down depth map the rain and snow effects already draw) where it holds something, else from the camera's depth (the ortho map
// holds no land at all). It is world-fixed and wraps round, so it never has to be moved: when the camera moves, only the
// texels whose spot left the window start again, from the open-ground values kept here on the CPU (which follow the same rules for
// an open, sunlit spot). Rain and snow build up on every spot's top surface; spots dry and melt over game time, faster in the sun,
// slower in shade (a short march towards the sun over the map's own heights) and at night; snow gathers in hollows and against
// walls; the player's feet tread a trail into the snow. SnowAccumulation (technique 1; WetWorld's 5/6 until 2026-11-21) reads the map
// instead of their single amount; outside the window they use the open-ground values. Their own top-surface tests (the ortho map)
// still decide which pixels are the top surface, so ground under a roof stays dry.
// Plain D3D9 render-target ping-pong (two pairs of A16B16G16R16 maps, two render targets at once), so it works on native D3D9 and
// through DXVK alike. The map belongs to
// one exterior worldspace: another worldspace, a loaded save (game time going back) or more than 72 game hours at once start it again.
//
// Not in Effects.h on purpose (the class layouts every file sees stay as they were): the effect is reached through TheCoverMap,
// registered in ShaderManager::Initialize like the others (no settings section of its own, so it is always on when its file loads).
class CoverMapEffect : public EffectRecord
{
public:
	CoverMapEffect() : EffectRecord("CoverMap") { memset(&Constants, 0, sizeof(Constants)); }

	void	UpdateConstants() override;		// the open-ground model, once a frame (ShaderManager runs UpdateConstants twice)
	void	UpdateSettings() override;
	void	RegisterConstants() override;
	void	RegisterTextures() override;
	bool	ShouldRender() override;		// the debug view only (the update is drawn by RenderUpdate)
	bool	ExtSupportsRenderRect() const override { return true; }
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	// The readers. *Active: the effect must keep drawing (something may still be wet / snowy in the map, or it rains / snows) even
	// when its own single amount is 0. *Technique: the technique to draw with (the persistent one once the map holds data).
	bool	SnowActive() const;
	UINT	SnowTechnique(SnowAccumulationEffect* snow) const;

	// RenderEffectsPreTonemapping, before SnowAccumulation: updates the map when needed and sets the readers' constants.
	void	RenderUpdate(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);
	// RenderEffects, last: the debug view (PersistentDebugView), through the frame chain.
	void	RenderDebug(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* SourceSurface);

	struct SettingsStruct {
		bool	wet = false, snow = false;
		float	size = 0.0f;					// game units across
		int		resolution = 0;					// texels across
		float	wetTime = 0.0f, dryTime = 0.0f, puddleFill = 0.0f, puddleDry = 0.0f, wetShade = 1.0f;	// game hours; shade factor
		float	snowIncrease = 0.0f, snowDecrease = 0.0f, snowShade = 1.0f, drifts = 0.0f, footprints = 0.0f, footSize = 0.0f;
		int		footShape = 0;	// UNOFFICIAL 2026-10-28 PersistentFootprintShape: 0 one soft blob per step, 1 a boot (sole and heel)
		int		debug = 0;
	};
	SettingsStruct	Settings;

	// UNOFFICIAL 2026-10-28 footprints (user: "snow foot steps just kinda leaves a line behind when we walk like we're riding a
	// bicycle. we want it to be foot steps"): one print per step, left and right in turn, a step apart (the stride follows the walking
	// speed). Each print is stamped into the map as a dot (8 units a texel: too coarse for a foot), and the last MaxPrints are also
	// sent to SnowAccumulation (TESR_CoverMapFeet), which draws them as boot prints per pixel instead of the map's dots near them.
	static const int MaxPrints = 24;

	struct ConstantsStruct {
		D3DXVECTOR4	Update, Origin, Shift, Rates, SnowRates, Sun, Shade, Start, FootA, FootB, Debug;	// the update pass (CoverMap.fx.hlsl)
		D3DXVECTOR4	Data, Window, Global, Trail;	// the readers (Includes/CoverMap.hlsl)
		D3DXMATRIX	OrthoInverse;
		D3DXVECTOR4	Feet[MaxPrints];	// 2026-10-28: xyz the print's centre (relative to the camera), w side x (heading in whole degrees + 0.99 x depth); 0 none
		D3DXVECTOR4	FeetData;			// xy, z: a circle round every print (relative to the camera; z <= 0: none), w: a print's half length
	};
	ConstantsStruct	Constants;

private:
	bool	EnsureMap(IDirect3DDevice9* Device);
	void	ReleaseMap();
	double	MinStep() const;
	float	TrailRange(double texelSize) const;
	void	SetReaderConstants();
	bool	FillExtUniforms(bool needUpdate);
	bool	ExtRenderUpdate(IDirect3DDevice9* Device);
	ExtPass				extUpdate{}, extDebug{};
	ExtUniformBuffer	extUniforms{};

	IDirect3DTexture9*	mapTexture[2] = {};			// main: wetness, puddle water, snow, trodden snow (the readers' map)
	IDirect3DSurface9*	mapSurface[2] = {};
	IDirect3DTexture9*	auxTexture[2] = {};			// aux: top-surface height, sun/shade, height source (written beside main: 2 targets)
	IDirect3DSurface9*	auxSurface[2] = {};
	IDirect3DTexture9*	currentTexture = nullptr;	// the slot TESR_CoverMapBuffer follows: the main map written last
	IDirect3DTexture9*	currentAux = nullptr;		// the slot TESR_CoverMapAux follows
	int					current = 0;
	int					mapResolution = 0;
	bool				mapUnorm = true;			// A16B16G16R16 (else the A16B16G16R16F fallback: coarser steps)
	bool				mapFailed = false;			// the card draws into fewer than 2 targets at once (for the session)
	FailOnce::Latch		mapMakeFailed{ "Persistent wetness/snow cover map" };	// UNOFFICIAL 2026-11-02 RetryFailedEffects: its textures could not be made
	bool				mapValid = false;			// the current map holds data (else the next update starts every texel)

	// the window of the last update (world texel index of its first texel), its reference height
	long long			originX = 0, originY = 0;
	double				referenceZ = 0.0;
	TESWorldSpace*		worldSpace = nullptr;

	// the open-ground model: wetness, puddle water, snow (x, y, z of Constants.Global), their upper bounds over every spot, and
	// the amounts since the last map update (applied by the next one, which may come frames later)
	float				wetBound = 0.0f, snowBound = 0.0f;
	D3DXVECTOR4			startGlobal = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// the open-ground values at the last map update
	float				pendWetGain = 0.0f, pendWetDry = 0.0f, pendPuddleGain = 0.0f, pendPuddleDry = 0.0f, pendSnowGain = 0.0f, pendSnowMelt = 0.0f;
	double				lastCommit = 0.0;
	bool				clockSet = false;
	bool				lastRaining = false, lastSnowing = false;
	float				rainNow = 0.0f;		// the wet half's rain amount (always 0 since 2026-11-21: the wet half is off)
	bool				snowNow = false;
	bool				resyncWet = true, resyncSnow = true;
	unsigned int		tickFrame = ~0u;
	unsigned int		framesSinceUpdate = 0;
	double				feetX = 0.0, feetY = 0.0, feetZ = 0.0;	// the player's feet at the last print (world; the path, not the print)
	bool				feetSet = false;
	// 2026-10-28 footprints: the last MaxPrints (world), the next foot, and the walking speed the stride follows
	struct Print { double x, y, z; float dirX, dirY, side, depth; };
	Print				prints[MaxPrints] = {};
	int					printNext = 0;
	float				printSide = 1.0f;			// +1 the left foot, -1 the right
	double				speedX = 0.0, speedY = 0.0, speedClock = -1.0;
	bool				speedSet = false;
	float				walkSpeed = 0.0f;			// game units a second, smoothed
	void				ClearPrints();
	void				SetFeetConstants();
	unsigned int		updates = 0;
	CachedHandle		updateHandle;
};

extern CoverMapEffect* TheCoverMap;
extern unsigned int CoverMapOrthoFrame;	// ShadowManager: the frame whose ortho map was drawn (or moved) for the rain / snow effects#include "../../core/FailOnce.h"	// UNOFFICIAL 2026-11-02 RetryFailedEffects

