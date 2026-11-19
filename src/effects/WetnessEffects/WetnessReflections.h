#pragma once

// UNOFFICIAL 2026-11-04, ported from Physically-Based-Reloaded (Balebandro, effects/WetnessReflections.h).
// Wetness reflections: screen-space reflections on the water film and puddles Wetness Effects puts on
// the world. Run once the world is drawn (Effects/WetnessReflections.fx), on whatever is wet now; the
// wetness comes from WetnessEffectsShaders, which this needs on. Its settings are Wetness Effects'
// ([Shaders.WetnessEffects.Reflections]).
class WetnessReflectionsEffect : public EffectRecord
{
public:
	WetnessReflectionsEffect() : EffectRecord("WetnessReflections") {};

	struct WetnessReflectionsSettingsStruct {
		bool	Enabled;			// Wetness Effects' Reflections switch
		float	Strength;			// how much of the reflection replaces the sky's
		float	MaxDistance;		// how far a ray goes, in game units
		float	Thickness;			// how far behind a surface a ray may be and still hit it
		float	Steps;				// ray steps
		float	RoughnessCutoff;	// films rougher than this reflect only the sky
		float	EdgeFade;			// fade near the screen's edges, as a share of the screen
		float	MaxCameraDistance;	// no reflections on surfaces farther than this
		bool	DebugView;			// the reflections alone
		bool	LampFilm = true;	// UNOFFICIAL 2026-11-19 [Main.Main.Quality] DeferredWetLampFilm
	};
	WetnessReflectionsSettingsStruct	Settings;

	struct WetnessReflectionsStruct {
		D3DXVECTOR4	Data;		// x: strength, y: max distance, z: thickness, w: steps
		D3DXVECTOR4	Fade;		// x: roughness cutoff, y: edge fade, z: debug view, w: max camera distance
		D3DXVECTOR4	Bump = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-11-18 x: 1 = s4 holds the shading normals (BindShadingNormals); y (2026-11-19) 1 = s8 holds DeferredWetness's land film
		D3DXVECTOR4	LampFilm = D3DXVECTOR4(0.0f, 8.0f, 1.0f, 0.0f);	// UNOFFICIAL 2026-11-19 DeferredWetLampFilm: x on, y froxel list texels, z table rows
	};
	WetnessReflectionsStruct	Constants;

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();
	void	BindShadingNormals(IDirect3DDevice9* Device);	// UNOFFICIAL 2026-11-18 DeferredBumpedNormals: stage 4 for the next Render
};
