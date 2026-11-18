#pragma once

class WetWorldEffect : public EffectRecord
{
public:
	WetWorldEffect() : EffectRecord("WetWorld") {
		Constants.Data.x = 0.0f;
		Constants.Data.y = 0.0f;
		Constants.Data.z = 0.0f;
		Constants.PuddlesAnimator.Initialize(0);
	};

	struct WetWorldStruct {
		Animator		PuddlesAnimator;
		D3DXVECTOR4		Coeffs;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Look = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL x: KeepHighlights (1 = the wet pass clamps only below 0)
																// UNOFFICIAL 2026-11-12 y: PuddlesLandOnly's mode now (0, 1, 2) + 4 while WeaponRainHandOff acts,
																// z: PuddlesMinObjectSize, w: 1 while ReflectionsOwnPuddles acts (WetWorld.fx.hlsl GetSurfaceWetClass)
	};
	WetWorldStruct	Constants;

	float increaseRate;
	float decreaseRate;

	// UNOFFICIAL 2026-11-12 PuddlesLandOnly ([Shaders.WetWorld.Main] PuddlesLandOnly, PuddlesMinObjectSize; WetWorld.fx.hlsl
	// GetSurfaceWetClass): the wet-map pass reads what each pixel is from TESR_MaterialBuffer (core/CapturedNormals.h), so puddles and
	// the wet look stop at people, cloth and plants (1, ground-like: this game's roads are objects, they keep their puddles) or at
	// everything but the land (2). 0, or no material buffer this frame: the old guess (faces up, lies in a hollow) alone.
	int		puddlesLandOnly = 1;
	float	puddlesMinObjectSize = 0.0f;
	// UNOFFICIAL 2026-11-12 the two clashes with the other rain systems, each off by default (they change the look):
	// WeaponRainHandOff: the HUD raindrops (effects/WetnessEffects/HudRaindrops.h) draw the rain on the first-person arms and weapon in
	// their own shaders; this pass's ripples and splashes over the same pixels are a second rain layer. On: left out while that effect runs.
	// ReflectionsOwnPuddles: the Reflections effect (after this one in the frame) mirrors the wet ground in the rain; the puddles' own
	// sky / cloud image and screen-space trace under it are a second reflection. On: left out while it rains and that effect runs.
	bool	weaponRainHandOff = false;
	bool	reflectionsOwnPuddles = false;
	// UNOFFICIAL 2026-11-12 RainStateGuard (core/RainState.h): Constants.Data (x the rain, z the puddles) drives much more than this
	// effect: the land's and the objects' Rain setting groups (Terrain.cpp, PBR.cpp), skin, the heat haze, the water's ripples. On
	// (default): RainState::Tick fills it every frame from the weather, whether this effect or Precipitations is on or off, always
	// 0-1; PuddlesAnimator, increaseRate and decreaseRate are its to drive. Off: UpdateConstants animates it as before (it stays as
	// it is while this effect is off).
	bool	rainStateGuard = true;
	int		puddleLogState = -1;	// the last state logged (PuddlesLandOnly: 0 off, 1 waiting for the material buffer, 2 + mode running)
	UInt32	puddleLogLines = 0;

	// UNOFFICIAL 2026-10-14 HudDrops (Render.cpp HudRain, Shaders/Includes/HudRain.hlsl): rain on the first-person weapon. The cover
	// test (technique 1) draws one pixel into coverSurface under an occlusion query; its count comes back a frame or two later
	// without stalling (covered: true when something is overhead).
	IDirect3DTexture9*	coverTexture = nullptr;
	IDirect3DSurface9*	coverSurface = nullptr;
	IDirect3DQuery9*	coverQuery = nullptr;
	bool				coverPending = false;
	bool				covered = false;
	void	RegisterTextures();
	void	RenderCoverTest(IDirect3DDevice9* Device);

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	bool	ShouldRender();

	// UNOFFICIAL 2026-10-17 FastPaths ([Shaders.WetWorld.Main] FastPaths): technique 2 of WetWorld.fx is the same picture with
	// the wet pass's early returns and lamp loop as real branches (the compiler had flattened them: every pixel, sky included,
	// ran the whole pass). Off, or a shader file without technique 2: technique 0, the original.
	bool	fastPaths = false;
	// UNOFFICIAL 2026-10-17 FixedReflectionTrace ([Shaders.WetWorld.Main] FixedReflectionTrace, off by default: it changes the look):
	// techniques 3 (the original pass) and 4 (FastPaths) trace the puddle reflection with the corrected hit test, so puddles reflect
	// what is on screen. Off, or a shader file without them: techniques 0 and 2, the programs as they were.
	bool	fixedTrace = false;
	UINT	MainTechnique() const {
		if (fixedTrace && Effect && Effect->GetTechnique(fastPaths ? 4 : 3)) return fastPaths ? 4 : 3;
		return fastPaths && Effect && Effect->GetTechnique(2) ? 2 : 0;
	}

	// UNOFFICIAL 2026-10-25 (NVR DXVK): techniques 2, 3 and 4 as Vulkan passes (WetWorld.cpp ExtRender; Compute\WetWorldMap.frag and
	// Compute\WetWorld.frag). Technique 0 (FastPaths off, the original trace) and the cover test (1) stay on the D3D9 passes.
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass				extMap;
	ExtPass				extWet;
	ExtUniformBuffer	extUniforms;
};
