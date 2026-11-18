#pragma once

class GrassShaders : public ShaderCollection
{
public:
	GrassShaders() : ShaderCollection("Grass") {};

	struct GrassStruct {
		D3DXVECTOR4		Scale;
		// UNOFFICIAL 2026-11-04, from Physically-Based-Reloaded (effects/Grass.h):
		D3DXVECTOR4		Brightness;		// x: multiplies the lit grass colour (before fog), y: the night brightness on the sun and ambient (not lamps)
		D3DXVECTOR4		Bend;			// xyz: the bend origin (camera-relative world), w: 1 / reach (the push per unit of blade height rides in TESR_Translucency.w)
		D3DXVECTOR4		LightData;		// x: lamps in LightPosition (0: none, the shader skips them), y: Strength
		// UNOFFICIAL 2026-11-12 CapturedGrassTint (the grass's colour record, GRASS23x000TMS.pso.hlsl main's end; TESR_GrassRecord, bound only
		// in a shader compiled with the records): x [Main.Main.Quality] CapturedGrassColour, y CapturedGrassSaturation, z CapturedGrassTint
		// (0 while CapturedGrass is off: the shader skips the maths).
		D3DXVECTOR4		Record;
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

	// UNOFFICIAL 2026-11-04 the rest of this class is ported from Physically-Based-Reloaded (effects/Grass.h), in place of Grass
	// Collision and PointLights (VegasShaders-backups/before_grass_port_20261104).
	float		NightBrightness = 1.0f;	// [Shaders.Grass.Main] NightBrightness, eased in by the day/night transition (exteriors)
	float		BendPush = 0.0f;		// the bend's push per unit of blade height, for TESR_Translucency.w (PBR.cpp)

	// Lamps on the grass ([Shaders.Grass.Lights]): placed lights and the Pip-Boy light, never the sun. The nearest of the lights NVR
	// tracks each frame, into pixel c82-c98 of GRASS23x000TMS.pso.
	static const int MaxGrassLights = 8;
	D3DXVECTOR4	LightPosition[MaxGrassLights];	// xyz: camera-relative world, as the grass rebuilds positions; w: radius
	D3DXVECTOR4	LightColor[MaxGrassLights];		// rgb: colour times dimmer
	// UNOFFICIAL 2026-11-04 the deferred flashlight on the grass (core/FlashlightDeferred.h LightParams, the colour times FlashlightGrassStrength),
	// pixel c99-c101: [0] w 0 while it is off.
	D3DXVECTOR4	Spot[3] = {};
	D3DXVECTOR4	SpotShadow = {};	// c106: x 1 = its shadow map is there, y texel size, z bias, w PCF radius (the light pass's settings)
	D3DXVECTOR4	SpotFalloff = {};	// c107: x [Shaders.Flashlight.Deferred] Falloff
	struct LightSettings {
		bool	Enabled;
		int		MaxLights;
		float	Strength;
	};
	LightSettings	Lights = {};
	void	UpdateLights();

	// Grass bending away from the player, as There Stands The Grass NVSE does it: within Radius of the player's feet the blades lean
	// away, more the faster they move, and spring back once they leave. Same settings, same defaults.
	struct InteractionSettings {
		bool	Enabled;
		float	Radius;				// reach around the player (units)
		float	StrengthStanding;	// push while standing; the blade tip gets half of it
		float	StrengthMax;		// push at full running speed
		float	SpeedForMax;		// the speed (units/s) reaching StrengthMax
		float	SpringBackSeconds;	// how quickly grass recovers when the player slows or leaves
		float	PositionLagSeconds;	// how tightly the bend centre follows the player
		bool	Debug;				// tint the grass the bend reaches red, log the push every two seconds
	};
	InteractionSettings	Interaction = {};

	// Grass fading by how big it looks rather than only how far it is, after Community Shaders' Grass Optimizations: an instance
	// shorter on screen than MinPixels is gone, fading in over FadePixels above that. Detailed meshes (many vertices, as rocks) count
	// as up to 1 + DetailStrength times smaller, so they go sooner. Instances gone are dropped before they reach the pixel shader.
	struct LODSettings {
		bool	Enabled;
		float	MinPixels;
		float	FadePixels;
		float	DetailVertices;		// a mesh with this many vertices or more counts as fully detailed
		float	DetailStrength;
	};
	LODSettings	LOD = {};

	// Normal maps on the grass ([Shaders.Grass.NormalMaps]): a grass texture "foo.dds" with a "foo_n.dds" beside it (loose or in a
	// BSA) bends the sun's light per pixel around the grass's own lighting normal. Bound per shape on s1, with GrassMaterial (pixel
	// c81: x 1 when bound, y strength); grass without one is lit exactly as before.
	struct NormalMapSettings {
		bool	Enabled;
		float	Strength;
	};
	NormalMapSettings	NormalMaps = {};

	// The sun's highlight on the grass ([Shaders.Grass.NormalMaps] EnableSpecular, SpecularStrength, Glossiness), masked by the normal
	// map's alpha where there is one: GrassMaterial z strength (0 off), w glossiness.
	struct SpecularSettings {
		bool	Enabled;
		float	Strength;
		float	Glossiness;
	};
	SpecularSettings	Specular = {};

	NiPoint3	BendCentre = { 0.0f, 0.0f, 0.0f };	// world, lagging the player
	NiPoint3	LastPlayerPos = { 0.0f, 0.0f, 0.0f };
	float		Speed = 0.0f;						// this frame's, units/s
	float		Push = 0.0f;						// the strength, eased
	bool		Have = false;						// a live bend origin
	bool		HavePrev = false;					// a previous position to measure the speed from
	float		DebugClock = 0.0f;
	double		LastFrameTime = -1.0;				// ElapsedOncePerFrame (UpdateConstants runs twice a frame)

	void	UpdateConstants();
	void	RegisterConstants();
	void	UpdateSettings();
	void	SetForDraw(NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader);
	// Per grass shape: the size fade (folded), whether it stands still (rocks) and the bend's debug tint, into c17 of the grass vertex
	// shaders.
	void	SetBatchConstants(NiGeometry* Geometry, bool Static);
	// Every grass shape the game draws (the hooked per-geometry setup).
	static void	InstallHooks();
	void	OnGrassShape(NiGeometry* Geometry);
};
