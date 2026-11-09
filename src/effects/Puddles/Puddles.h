#pragma once

// 2026-11-08 Puddles (README.md here): rain puddles that fill and dry over game time, a flat water layer with ripples, a soaked ring and
// wall streaks, on top of Wetness Effects' water layer. The shading is in Shaders/Includes/Wetness.hlsl (PUDDLES_COMPILED: the surface
// shaders with WETNESS_EFFECTS 2, ShaderRecord::BuildDefines) and Compute/Reflections.glsl (PuddleStanding). This collection holds the
// settings, works out the water level from the game's weather and clock, and fills the constants.
class PuddlesShaders : public ShaderCollection
{
public:
	PuddlesShaders() : ShaderCollection("Puddles") {};

	struct PuddleSettingsStruct {
		int		Quality;		// 0 off .. 3 high
		float	Coverage, Size, FillHours, DryHours, NightDrying;
		float	Roughness, Darkening, EdgeSoftness, SlopeStart, SlopeFull, Cavity, WetRing;
		float	Ripples, Streaks, StreakWidth, StreakSpeed;
		bool	ReflectionPuddles;
		float	LevelOverride;	// -1 off
		int		DebugView;		// 0 off, 1 mask, 2 ripples, 3 levels
	};
	PuddleSettingsStruct	Settings;

	struct PuddleConstantsStruct {
		D3DXVECTOR4	Data;		// c174 x level, y Coverage, z 1 / Size, w Quality
		D3DXVECTOR4	Look;		// c175 x roughness, y depth darkening, z edge softness, w ripples
		D3DXVECTOR4	Shape;		// c165 x slope start, y slope full, z cavity, w wet ring
		D3DXVECTOR4	Streak;		// c166 x strength x raining, y 1 / lane width, z speed, w debug view
	};
	PuddleConstantsStruct	Constants;

	float	Level = 0.0f;		// the water level, 0-1 (kept across cells; held indoors)
	float	Rain = 0.0f;		// how hard it rains now, 0-1, by the engine's rule (Sky::GetIsRaining)
	double	LastHours = -1.0;	// GameDaysPassed x 24 at the last update

	bool	Active() const;			// on, outdoors, and water standing or the debug view
	bool	ReflectionsWanted() const;	// the Vulkan reflection passes should mirror standing water
	void	ReflectionData(D3DXVECTOR4& puddle, D3DXVECTOR4& puddle2) const;	// Reflections.glsl's cb.puddle / puddle2 (zero: off)

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
};
