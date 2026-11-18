#pragma once

// UNOFFICIAL 2026-11-03: dust motes -- sparse tiny specks of dust drifting in the air, seen only where light falls on them (near
// lamps, in the sun's shafts), glinting softly towards the light. Procedural: no particles are placed in the world, so it works in
// every cell, modded ones too. The specks sit in a world-anchored 3D grid of cells round the camera (one speck in some of them, at a
// hashed spot that drifts and wobbles slowly); a half-resolution pass walks each pixel's view through the cells it crosses, keeps
// the speck nearest its view, lights it from the nearby lamps (and the sun shafts' froxel grid outdoors) and writes the light it
// adds; a full-resolution pass then adds that to the frame with additive blending, only where a speck is (no frame copy).
// Effects\DustMotes.fx.hlsl; README.md in this folder.
class DustMotesEffect : public EffectRecord
{
public:
	// UNOFFICIAL 2026-11-12 (memleak) idle targets (core/TextureManager.h, EffectRecord::TargetParkable): its buffer is drawn and read only in RenderFrame, behind the switch
	bool	TargetParkable(const char* SamplerName) override { return true; }
	DustMotesEffect() : EffectRecord("DustMotes") { ZeroMemory(&Constants, sizeof(Constants)); };

	static const int LampsMax = 8;	// lamps the pass lights the specks from (the nearest, brightest of the frame's 24)

	struct SettingsStruct {
		float	Density;		// chance a cell holds a speck (0..1)
		float	Size;			// speck radius, game units
		float	Brightness;		// light the specks scatter, times the lamps' light
		float	MaxDistance;	// specks are drawn out to this distance from the eye, game units
		float	Drift;			// speed of the drift and wobble (1 = about 5 units a second)
		float	Glint;			// forward scattering (Henyey-Greenstein g)
		float	SunShare;		// the sun's light on the specks, outdoors, as a share of the lamps'
		bool	Interiors;
		bool	Exteriors;
		int		DebugView;		// 0 off, 1 the buffer drawn over the picture (lamp reach / cells / specks), 2 every speck white
	};
	SettingsStruct	Settings;

	struct ConstantsStruct {
		D3DXVECTOR4	Data;					// x: brightness (0 = off this frame), y: speck radius, z: max distance, w: density
		D3DXVECTOR4	Look;					// x: glint g, y: cell size, z: wobble clock (cycles, wraps at 1), w: lamps in use
		D3DXVECTOR4	Origin;					// xyz: the camera in the drifting cells' frame (wrapped), w: radians per half-resolution pixel
		D3DXVECTOR4	Sun;					// x: sun share this frame (0: no sun), y: DebugView, zw unused
		D3DXVECTOR4	LampPosition[LampsMax];	// xyz: camera-relative position, w: radius (0 = empty slot)
		D3DXVECTOR4	LampColor[LampsMax];	// rgb: linear colour times dimmer
	};
	ConstantsStruct	Constants;

	struct TexturesStruct {
		IDirect3DTexture9* BufferTexture = nullptr;	// half resolution: the light each pixel's specks add (gamma-encoded difference)
		IDirect3DSurface9* BufferSurface = nullptr;
	};
	TexturesStruct	Textures;

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	// The frame's step (EffectChain.cpp PreStep_DustMotes): picks the lamps, then the half-resolution pass into Buffer and the additive
	// pass into the chain's current image. sunGrid: the volumetric light's froxel grid was built this frame (the sun's lit air).
	void	RenderFrame(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool sunGrid);

private:
	void	PickLamps();
	void	LogFrame(const char* outcome);
	int		seenLamps = 0;
	double	lastLogTime = -1.0;
	double	windX = 0.0, windY = 0.0, windZ = 0.0;	// how far the air has drifted, game units (wrapped at the cells' repeat)
	double	wobble = 0.0;							// the wobble clock, cycles (wrapped at 1)
	double	lastFrameTime = -1.0;					// ElapsedOncePerFrame
	bool	saidNoChain = false;
};
