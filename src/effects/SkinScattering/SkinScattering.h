#pragma once
#include <unordered_map>

// UNOFFICIAL 2026-10-15: CREDIT Conor (mrfury97): his screen-space skin scattering, from his NVR testbed
// (github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, branch testbed, src/effects/SkinScattering.h). An add-on to this build's
// skin shader (SkinTemplate.hlsl, Skin.h): [Shaders.Skin.Scattering] / [Shaders.Skin.Interiors] ScreenSpace; the SkinShader
// hooks (Hooks/Shaders.cpp) bind its targets for every skin draw. Idle while Vanilla Plus Skin's shaders are in use.
//
// Screen-space subsurface scattering for skin: Jimenez & Gutierrez's separable SSS, with the
// kernel generator ported from Community Shaders (GPL-3.0-or-later; the separable SSS itself is
// "Copyright (C) 2012 by Jorge Jimenez and Diego Gutierrez"). Blurs skin's diffuse light across
// the screen with a red-leaning skin profile, the way ENB and Community Shaders soften skin.
//
// How the skin is found without a G-buffer:
//   - During every NVR skin draw in the main scene (SkinShader's PrepareGeometryForRendering ..
//     PostGeometry, hooked in NewVegas/Hooks/Shaders.cpp), a second render target is bound.
//     The skin shader writes its highlights' share of the colour (rgb) and its camera distance
//     (a) to it, its albedo (rgb) and the luminance of the colour it drew (a) to a third, and its
//     full colour to the scene as always.
//   - This effect blurs only pixels whose stored distance matches the depth buffer and whose
//     scene colour still matches what the skin drew, so anything drawn over the skin afterwards
//     (hair cards, gear, decals, particles, with or without depth writes) is left alone. It runs
//     first in the effect chain, on the scene exactly as drawn, for that comparison. It takes the highlights out before
//     blurring and puts them back after (the skin shader's own, and since 2026-10-19 the game's own highlight passes on
//     skin, which add the same colour to the skin target as to the scene: BindForGameHighlights), and divides the albedo out before blurring and
//     multiplies it back after (Community Shaders' pre/post scatter), so only light is diffused
//     and the skin texture's detail stays sharp.
// Anything this cannot handle (fading actors, reflections, MSAA edges, the effect switched off)
// keeps the scene's own, complete colour: nothing is lost, it is only not blurred. Known gaps:
//   - an actor lit only through additive passes (blend ONE, ONE) never writes its distance, so
//     it is not blurred; the engine's base or AD pass with ambient is opaque and does write it
//   - an actor lit by several passes ends with a colour its first pass did not write, so it is
//     only blurred where a single pass lit it
//   - first-person arms are drawn outside the world scene and are not blurred
class SkinScatteringEffect : public EffectRecord
{
public:
	SkinScatteringEffect() : EffectRecord("SkinScattering") {};

	// 2026-10-19 re-sync with Conor's testbed 00e81af5 (2026-10-16): 17 -> 21, Community Shaders' SSSS_N_SAMPLES, which
	// CalculateKernel spreads over +-3 kernel units (17 spread over +-2).
	static const int KernelSamples = 21;

	struct SkinScatteringStruct {
		D3DXVECTOR4		Data;                       // x width (game units per kernel unit), y depth follow, z distance tolerance, w albedo detail
		D3DXVECTOR4		Kernel[KernelSamples];      // rgb weight, a offset (kernel units, mm of d'Eon's profile)
		D3DXVECTOR4		Debug;                      // x DebugView, y 1 to turn the kernel per pixel (with TAA), z this frame's turn
	};
	SkinScatteringStruct	Constants;

	struct ProfileStruct {
		float		Width;          // x d'Eon's measured width
		float		DepthFollow;
		float		AlbedoDetail;   // 0 blurs the texture with the light (waxy), 1 blurs light only
		D3DXVECTOR3	Strength;       // per channel: how much of the light scatters
		D3DXVECTOR3	Falloff;        // per channel: profile width
		bool		ScreenSpace;
	};
	ProfileStruct	Profile = {};                 // the one in use, copied each frame from:
	ProfileStruct	ExteriorProfile = {};         // [Shaders.Skin.Scattering]
	ProfileStruct	InteriorProfile = {};         // [Shaders.Skin.Interiors]
	bool			ProfileIsInterior = false;
	bool			KernelDirty = true;
	bool			SkinShaderDebug = false;   // a [Shaders.Skin.Debug] view the skin shaders draw (1-7): no blur
	bool			ScreenSpace = true;     // the profile in use's ScreenSpace

	// The second render target. Texture is what the effect samples; Surface is what skin draws
	// write into: the texture's own surface, or a multisampled one resolved into it.
	IDirect3DTexture9*	Texture = nullptr;
	IDirect3DSurface9*	TextureSurface = nullptr;
	IDirect3DSurface9*	MSAASurface = nullptr;
	// The third: the skin's albedo, same arrangement.
	IDirect3DTexture9*	AlbedoTexture = nullptr;
	IDirect3DSurface9*	AlbedoTextureSurface = nullptr;
	IDirect3DSurface9*	AlbedoMSAASurface = nullptr;
	D3DSURFACE_DESC		TargetDesc = {};
	bool				Supported = false;      // device caps: two render targets, independent bit depths and write masks
	int					Recreations = 0;        // a safety stop: the target should be created once per resolution

	// Per frame.
	bool		InWorldScene = false;   // set around the world scene render (RenderWorldSceneGraphHook)
	bool		Bound = false;
	bool		HighlightsBound = false;   // the skin target bound for one of the game's own highlight passes (BindForGameHighlights)
	bool		ClearedThisFrame = false;
	bool		SkinDrawn = false;
	DWORD		SavedWriteMask1 = 0xF;
	DWORD		SavedWriteMask2 = 0xF;

	void	BeginFrame();
	void	BindForSkinDraw(NiD3DPixelShaderEx* PixelShader);
	bool	BindForGameHighlights(NiD3DPixelShaderEx* PixelShader);
	void	Unbind();
	void	FillFromDeferredSkin();	// UNOFFICIAL 2026-11-15 DeferredSkinBlur (core/DeferredSkin.h AfterPasses): the targets filled for the deferred skin

	void	UpdateConstants();
	void	RegisterConstants();
	void	RegisterTextures();
	void	UpdateSettings();
	bool	ShouldRender();
	// UNOFFICIAL 2026-10-25 (NVR DXVK): both blur passes as src\hlsl\NewVegas\Compute\SkinScattering.frag (EffectRecord::ExtRender);
	// the constants go to a uniform buffer (the kernel does not fit push data).
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;

	ExtPass				extPass;
	ID3D9NvrExtBuffer*	extUniforms = nullptr;
	bool				extUniformsTried = false;

private:
	std::unordered_map<IDirect3DPixelShader9*, std::pair<UINT, bool>> WritesColor1;   // BindForGameHighlights: per pixel shader, its bytecode size and whether it writes COLOR1
	bool	EnsureTarget(const D3DSURFACE_DESC& SceneDesc);
	void	ReleaseTarget();
	void	ClearTarget(IDirect3DSurface9* SceneTarget);
	void	CalculateKernel();
};
