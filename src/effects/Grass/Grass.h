#pragma once

class GrassShaders : public ShaderCollection
{
public:
	GrassShaders() : ShaderCollection("Grass") {};

	struct GrassStruct {
		D3DXVECTOR4		Scale;
		D3DXVECTOR4		LightData;		// UNOFFICIAL 2026-11-05 x: lamps in LightPosition (0: none, the shader skips them), y: Strength
	};
	GrassStruct	Constants;
	bool		GameShaderFix = true;	// UNOFFICIAL 2026-10-17 [Shaders.Grass.Main] GameShaderFix (see SetForDraw)
	bool		DrawFirst = false;		// UNOFFICIAL 2026-10-17 [Shaders.Grass.Main] DrawFirst: grass before the land (Render.cpp namespace GrassFirst)
	int			DrawFirstRestore = 5;	// GrassFirst diagnostic: what is put back after the early part (bit mask, Render.cpp Snapshot)
	bool		DrawFirstCull = true;	// GrassFirst: whole draws all near / all fading get one part only (Render.cpp Classify)
	static inline int	DrawPart = 0;			// GrassFirst: 0 every tuft, 1 only the near (unfaded) tufts, 2 only the fading ones (c19.w)
	static inline float	LastPairing[4] = {};	// GrassFirst: the last c19 written (put back with w = 0 after the late part)
	static inline bool	LastBindNVR = false;	// GrassFirst: the last grass bind used NVR's vertex shader (which honours DrawPart)
	static inline unsigned	BindSerial = 0;			// UNOFFICIAL 2026-10-28: grass binds so far (GrassFirst trusts LastBindNVR only for a bind of its own run)
	static inline const char*	LastBindName = nullptr;	// the vertex shader of that bind (GrassFirst's log)

	// UNOFFICIAL 2026-11-05 (from Balebandro's VegasShaders fork 5a40a27, after Physically-Based-Reloaded): lamps on the grass and grass
	// normal maps with the sun's highlight. Lamps ([Shaders.Grass.Lights]): placed lights and the Pip-Boy light, never the sun, the
	// nearest of the lights NVR tracks each frame, into pixel c82-c98 of GRASS23x000TMS.pso.
	static const int MaxGrassLights = 8;
	D3DXVECTOR4	LightPosition[MaxGrassLights] = {};	// xyz: camera-relative world, as the grass rebuilds positions; w: radius
	D3DXVECTOR4	LightColor[MaxGrassLights] = {};		// rgb: colour times dimmer
	struct LightSettings { bool Enabled; int MaxLights; float Strength; };
	LightSettings	Lights = {};
	void	UpdateLights();
	// Normal maps ([Shaders.Grass.NormalMaps]): a grass texture "foo.dds" with "foo_n.dds" beside it (loose or in a BSA) bends the sun's
	// light per pixel; bound per shape on s1 with GrassMaterial (pixel c81). The highlight (EnableSpecular...): GrassMaterial z, w.
	struct NormalMapSettings { bool Enabled; float Strength; };
	NormalMapSettings	NormalMaps = {};
	struct SpecularSettings { bool Enabled; float Strength; float Glossiness; };
	SpecularSettings	Specular = {};
	static inline bool	LastPSMapped = false;	// the last grass bind's PS is NVR's GRASS23x000(TMS).pso (reads s1 / c81; not a PointLights build)
	static void	InstallHooks();				// the per-shape setup (TallGrassShader vtable 0x10B8980 slot 24)
	void	OnGrassShape(NiGeometry* Geometry);

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	void	SetForDraw(NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader);
};