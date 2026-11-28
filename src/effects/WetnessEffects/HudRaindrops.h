#pragma once

// UNOFFICIAL 2026-11-04 Wetness Effects' HUD raindrops, ported from Physically-Based-Reloaded (Balebandro, effects/HudRaindrops.h) in
// place of Wet World's HudDrops. Raindrops on the first-person arms and weapon, and (AllObjects) on every object out in the rain but
// the land, after Screen Space Shaders' HUD raindrops (STALKER Anomaly, Ascii1457). The drops are drawn by the meshes' own shaders
// (Shaders/Includes/HudRain.hlsl, in SkinTemplate, ObjectTemplate and ParallaxTemplate), so they stay on the surface through every
// animation: the per-pass hook (NewVegas/Hooks/Render.cpp) calls SetDrawConstants for each geometry pass. First-person geometry
// takes the camera's wetness (below); the world's objects the rain's, where the top-down ortho map says they are open to the sky.
//
// This effect keeps the wetness they read: Effects/HudRaindrops.fx eases a 1 x 1 value every frame (technique 0, as AutoExposure
// keeps its exposure) and keeps it (1). It rises while the player stands in the rain, not under cover (the ortho map: anything
// above the camera), and falls under cover, indoors or after the rain. Technique 2 draws the drops on the Pip-Boy's screen.
// Settings: [Shaders.WetnessEffects.HudRaindrops] (its Enabled is the effect's switch, SettingManager::GetMenuShaderEnabled).
class HudRaindropsEffect : public EffectRecord
{
public:
	HudRaindropsEffect() : EffectRecord("HudRaindrops") {};

	struct HudRaindropsStruct {
		D3DXVECTOR4		Data;    // x rain (0-1)
		D3DXVECTOR4		Time;    // y this frame's seconds, z seconds to wet through, w seconds to dry
		D3DXVECTOR4		Shade;   // z 1 when under cover regardless (indoors), w 1 to wipe (underwater)
		D3DXVECTOR4		Debug;   // x 1 always wet (to try it without rain)
		D3DXVECTOR4		Screen;  // the Pip-Boy screen's drops: x threshold, y clock, z refraction, w drop texture tiles across the screen
	};
	HudRaindropsStruct	Constants = {};

	struct HudRaindropsSettings {
		float DropSize;     // a drop texture tile, in game units
		float Density;
		float WetTime;		// seconds to wet through, and to dry off again
		float Refraction;
		float Reflection;
		float Gloss;        // the drops'
		float WetGloss;     // the wet surface's around them
		float AnimSpeed;
		int   DebugView;
		float Darkness;     // how much a drop darkens the surface under it
		bool  AllObjects;   // drops on every object out in the rain (not the land), not just the arms and weapon
	};
	HudRaindropsSettings	Settings = {};

	struct HudRaindropsTextures {
		IDirect3DTexture9* StateTexture;   // 1 x 1: r the wetness (0-1), g 1 under cover (TESR_HudRainState)
		IDirect3DSurface9* StateSurface;
		IDirect3DTexture9* NewTexture;     // this frame's, before it is copied into the one above
		IDirect3DSurface9* NewSurface;
		IDirect3DTexture9* ScreenCopy;     // the Pip-Boy screen as the game rendered it (TESR_HudRainScreenCopy)
		IDirect3DSurface9* ScreenCopySurface;
		IDirect3DVertexBuffer9* ScreenQuad;   // a quad over the screen's texture
		UINT ScreenWidth, ScreenHeight;
		D3DFORMAT ScreenFormat;
	};
	HudRaindropsTextures	Textures = {};

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	bool	ExtSupportsRenderRect() const override { return true; }
	void	UpdateWetness(IDirect3DDevice9* Device);
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	// The per-draw constants of Shaders/Includes/HudRain.hlsl (c221, c222, c170), for each geometry pass. firstPerson: the pass
	// draws the first-person arms and weapon (ShaderSplit's context).
	void	SetDrawConstants(bool firstPerson);
	// The drops on the Pip-Boy's screen, drawn into its texture once the game has rendered the menu into it (FOPipboyManager's
	// RenderPostProcess, Hooks/Shaders.cpp).
	void	DrawOnScreen(BSRenderedTexture* Target);

private:
	ExtPass				extUpdate{}, extKeep{}, extScreen{};
	ExtUniformBuffer	extUniforms{};
	bool	ExtDrawOnScreen(IDirect3DDevice9* Device, IDirect3DSurface9* TargetSurface);
	bool	Cleared = false;     // the wetness reset to 0 once its texture exists; until then no drops
	float	Clock = 0.0f;        // the drops' clock: runs while it rains
	float	DryingLeft = 0.0f;   // seconds the drops may still be drying (the passes run until then)
	float	WorldWet = 0.0f;     // the world's objects' wetness (AllObjects): rain eased over WetTime both ways, 0 indoors
	double	LastFrameTime = -1.0;	// ElapsedOncePerFrame (UpdateConstants runs twice a frame)
};
