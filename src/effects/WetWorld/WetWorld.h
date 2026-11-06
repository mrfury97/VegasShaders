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
		D3DXVECTOR4		Look = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL x: KeepHighlights (1 = the wet pass clamps only below 0); y (2026-11-05 Model 1): 1 = the material and albedo buffers match the frame the effects see
	};
	WetWorldStruct	Constants;

	float increaseRate;
	float decreaseRate;

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

	// UNOFFICIAL 2026-11-05 Model ([Shaders.WetWorld.Main] Model, default 1): 1 = the physically based, material-dependent wet surface
	// (WetWorld.fx.hlsl WetPBR, techniques 7 to 10: FastPaths, fixed trace, Persistent, Persistent + fixed trace); 0 = techniques 0 to 6, the
	// look as it was. Render maps the index its callers pick (MainTechnique, CoverMap's WetTechnique) to the Model 1 twin; the cover test (1),
	// Model 0 and a shader file without the twins keep the index. Model 1 has no Vulkan pass yet: ExtRender declines 7 to 10 (D3D9 draws).
	// 2026-11-05 (QA finding 15): a frame without a valid material buffer (Model1Unavailable) keeps the Model 0 technique, logged once per
	// reason: WetPBR would see every pixel as class 0, and characters would get the full darkening and film.
	int		model = 1;
	UInt32	model1FallbackLogged = 0;	// bit n: reason n of Model1Unavailable logged
	int		Model1Unavailable() const;
	UINT	ModelTechnique(UINT techniqueIndex);
	void	Render(IDirect3DDevice9* Device, IDirect3DSurface9* Target, IDirect3DSurface9* Rendered, UINT techniqueIndex, bool ClearTarget, IDirect3DSurface9* Source) override;

	// UNOFFICIAL 2026-10-25 (NVR DXVK): techniques 2, 3 and 4 as Vulkan passes (WetWorld.cpp ExtRender; Compute\WetWorldMap.frag and
	// Compute\WetWorld.frag). Technique 0 (FastPaths off, the original trace) and the cover test (1) stay on the D3D9 passes.
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass				extMap;
	ExtPass				extWet;
	ExtUniformBuffer	extUniforms;
};
