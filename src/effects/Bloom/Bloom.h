#pragma once

class BloomEffect : public EffectRecord
{
public:
	BloomEffect() : EffectRecord("Bloom") {};

	static const int MaxPasses = 8;

	struct BloomSettings {
		float Strength;
		int Passes;
		float PassBlending;
		float Spread;		// UNOFFICIAL: each wider level weighs 1 + Spread times the finer one (0: as before)
		float GlowBoost;	// UNOFFICIAL: bright lights count up to 1 + GlowBoost times in the bloom (0: as before)
	};
	struct BloomSettingsStruct {
		D3DXVECTOR4		Resolution[MaxPasses];
		BloomSettings	Main, Night, Interiors;
		// UNOFFICIAL, [Shaders.Bloom.Main] only (all times of day): see Bloom.fx.hlsl.
		float			GlowLightLevel;	// how many times a white surface in the scene's own light a light must reach for GlowBoost
		bool			LinearLight;	// blur linear light instead of the display-encoded image
		bool			AntiFlicker;	// Karis-weighted first downsample
	};
	BloomSettingsStruct Settings;

	struct BloomStruct {
		D3DXVECTOR4		Data, ExtraData, Resolution;
		D3DXVECTOR4		Glow;		// UNOFFICIAL TESR_BloomGlow: x spread weight, y last upsample scale, z glow boost, w glow threshold
		D3DXVECTOR4		GlowExtra;	// UNOFFICIAL TESR_BloomGlowExtra: x anti-flicker, y linear light
	};
	BloomStruct		Constants;

	struct BloomTexturesStruct {
		IDirect3DTexture9* BloomTexture[MaxPasses];
		IDirect3DSurface9* BloomSurface[MaxPasses];
		IDirect3DVertexBuffer9* BloomVertexBuffer[MaxPasses];
	};
	BloomTexturesStruct		Textures;

	// UNOFFICIAL 2026-10-27 RenderScale v2: per level, the quad with uv 0..Scale and that level's own half-texel offset (the shared
	// FrameVertexRect has the screen's); made on the first rect frame and again when the scale or the level's size changes.
	IDirect3DVertexBuffer9*	RectVertexBuffer[MaxPasses] = {};
	D3DXVECTOR3				RectVertexKey[MaxPasses] = {};	// ScaleX, ScaleY, level width it was made for
	IDirect3DVertexBuffer9*	LevelQuad(int i);				// BloomVertexBuffer[i] outside rect frames; sets the rect viewport inside them

	void	UpdateConstants(), RegisterConstants(), RegisterTextures(), UpdateSettings();
	bool    SwitchEffect();

	void    RenderPass(IDirect3DDevice9* Device, UINT techniqueIndex, bool ClearRenderTarget);
	void	RenderBloomBuffer(IDirect3DSurface9* RenderTarget);
	// UNOFFICIAL 2026-11-15 (fpnative-fix, docs/next/FPNATIVE-SEETHROUGH.md): the frame number RenderBloomBuffer last drew the buffer in
	// (FirstPersonNative::Flush draws it again from the picture with the full-size arms in it, only in a frame the chain drew it).
	UInt32	BufferFrame = 0xFFFFFFFF;

	// UNOFFICIAL 2026-10-25 (NVR DXVK): the whole chain as Vulkan passes into its own targets (src\hlsl\NewVegas\Compute\Bloom*.frag,
	// Bloom.glsl). RenderBloomBuffer runs it first; false (or any refused pass) leaves the D3D9 chain to do the frame.
	bool	RenderBloomExt(IDirect3DDevice9* Device, int passes);
	ExtPass	extDownFirst, extDown, extUp, extUpLast;
	// The downsample levels below the top one, as the Vulkan path keeps them (see Bloom.cpp): made on first use.
	bool	extLevelsTried = false;
	struct ExtLevelsStruct {
		IDirect3DTexture9* Texture[MaxPasses - 1];
		IDirect3DSurface9* Surface[MaxPasses - 1];
	};
	ExtLevelsStruct	extLevels = {};
};