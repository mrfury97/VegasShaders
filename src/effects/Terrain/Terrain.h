#pragma once

class TerrainShaders : public ShaderCollection
{
public:
	TerrainShaders() : ShaderCollection("Terrain") {};

	using TemplateTable = std::map<std::string_view, ShaderTemplate>;
	TemplateTable Templates() {
		return TemplateTable{
			{ "SLS2100.vso", ShaderTemplate{ "TerrainTemplate", {{"VS", ""}} } },
			{ "SLS2092.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "1"}}} },
			{ "SLS2094.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "1"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2096.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "1"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2098.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "1"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2100.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "2"}}} },
			{ "SLS2102.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "2"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2104.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "2"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2106.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "2"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2108.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "3"}}} },
			{ "SLS2110.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "3"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2112.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "3"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2114.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "3"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2116.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "4"}}} },
			{ "SLS2118.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "4"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2120.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "4"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2122.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "4"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2124.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "5"}}} },
			{ "SLS2126.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "5"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2128.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "5"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2130.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "5"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2132.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "6"}}} },
			{ "SLS2134.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "6"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2136.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "6"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2138.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "6"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2140.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "7"}}} },
			{ "SLS2142.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "7"}, {"NUM_PT_LIGHTS", "6"}}} },
			{ "SLS2144.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "7"}, {"NUM_PT_LIGHTS", "12"}}} },
			{ "SLS2146.pso", ShaderTemplate{"TerrainTemplate", {{"PS", ""}, {"TEX_COUNT", "7"}, {"NUM_PT_LIGHTS", "24"}}} },
			{ "SLS2002.vso", ShaderTemplate{"TerrainLODTemplate", {{"VS", ""}}} },
			{ "SLS2003.pso", ShaderTemplate{"TerrainLODTemplate", {{"PS", ""}}} },
			{ "SLS2080.vso", ShaderTemplate{"TerrainFadeTemplate", {{"VS", ""}}} },
			{ "SLS2082.pso", ShaderTemplate{"TerrainFadeTemplate", {{"PS", ""}}} },
		};
	};

	struct TerrainSettings {
		float LightScale, AmbientScale;
		float Roughness, Metallicness, Saturation;
		float SkylightingScale, SkylightingDirectionality;
		float SkylightingSaturation = 1.0f;	// UNOFFICIAL 2026-11-21: SkyShaders::UpdateConstants (TESR_SkyIrradianceTerrain)
	};
	struct ParallaxSettingsStruct {
		bool Enabled, HighQuality, Shadows;
		bool ShadowsAtNight;	// UNOFFICIAL 2026-10-16: false = no parallax self-shadows while the sun is down (UpdateConstants)
		bool HeightBlend;
		float MaxDistance, Height, ShadowsIntensity;
		bool IsoHeightTaps;		// UNOFFICIAL 2026-10-17: single isotropic tap per parallax height lookup (Parallax.hlsl)
		bool SkipEmptyLayers;	// UNOFFICIAL 2026-10-17: no colour/normal lookups for zero-weight layers (Terrain.hlsl)
	};
	struct LODSettingsStruct { float NoiseScale, NoiseTile; };
	struct TerrainSettingsStruct {
		TerrainSettings Default, Rain, Night, NightRain;
	};
	TerrainSettingsStruct Settings;
	ParallaxSettingsStruct ParallaxSettings;
	LODSettingsStruct LODSettings;

	struct TerrainStruct {
		D3DXVECTOR4		Data, ExtraData;
		// TerrainExtraData is full: x usePBR, y saturation, z NoiseScale, w NoiseTile.
		D3DXVECTOR4		SkyData;      // x: skylight strength, y: sample directionality
		D3DXVECTOR4		VariationData;	// UNOFFICIAL 2026-10-19 Terrain Variation (TESR_TerrainVariationData, c95): x lattice scale, y height influence, z StartDistance, w its fade
		D3DXVECTOR4		VariationExtra;	// (TESR_TerrainVariationExtra, c96): x 1 = the normal maps vary too
		D3DXVECTOR4		MacroData;		// 2026-11-04 macro variation (TESR_TerrainMacroData, c179): x strength, y 1 / size, z brightness, w tint
		D3DXVECTOR4		LODBlend;		// UNOFFICIAL 2026-10-19 LOD Blending (TESR_LODBlendData, c97): x far land brightness, y its gamma, z 1 = near land without its vertex colours, w 1 = the far land adjusted
	};
	TerrainStruct	Constants;

	struct ParallaxStruct {
		D3DXVECTOR4     Data, ExtraData;
		D3DXVECTOR4     IsoData;      // UNOFFICIAL x: IsoHeightTaps, y: SkipEmptyLayers (TESR_TerrainParallaxIso, c94)
	};
	ParallaxStruct  ParallaxConstants;

	// UNOFFICIAL terrain blending ([Shaders.Terrain.Blending]), ported from Balebandro's Physically-Based-Reloaded fork
	// (Alejandro Martinez), after Community Shaders' Terrain Blending: where the land covers an object by less than
	// BlendDistance, the land is dithered away (TerrainTemplate.hlsl), so rocks, walls and props sink softly into the
	// ground instead of ending in a hard line. Off by default; compiled into the land shaders only when on at startup.
	struct BlendingSettingsStruct {
		bool	Enabled, Debug;
		float	BlendDistance;
		float	GroundWidth;	// UNOFFICIAL 2026-10-14 (port): the band's width across the ground at most (shallow objects)
		float	HeightBlend;	// how much the land texture's height map shapes the edge
		float	SlopeBlend;		// BlendDistance multiplier on steep land
		float	MinPixels;		// the fade's minimum width on screen
		int		DebugView;		// 1 band magenta, 2 land kept, 3 depth below the surface, 4 fade depth
		float	MaxDistance;
		float	ShadowDrop;		// UNOFFICIAL 2026-10-17 (TB-1): the land is lowered by BlendDistance * this in the sun shadow maps (0 = off)
	};
	BlendingSettingsStruct BlendingSettings = {};

	struct BlendingStruct {
		D3DXVECTOR4		Data;		// x: on for this draw, y: 1 / blend distance, z: max distance, w: debug view
		D3DXVECTOR4		Extra;		// xy: 1 / resolution, z: dither frame (moves only while TAA runs)
		D3DXVECTOR4		Shape;		// UNOFFICIAL 2026-10-14: x ground width, y height blend, z slope blend, w minimum pixels
	};
	BlendingStruct	BlendingConstants = {};

	bool			usePBR;
	float			SkylightSaturation = 1.0f;	// UNOFFICIAL 2026-11-21: current terrain SkylightingSaturation

	float	CurrentSkylightSaturation() const;
	void	UpdateConstants(), RegisterConstants(), UpdateSettings();

	// Terrain blending. Begin/End bracket the game's world scene (RenderWorldSceneGraphHook): the first land shader bound
	// in between copies the depth drawn so far -- the objects the land will cover -- and every land draw of that scene
	// compares against it. PrepareBlending runs from ShaderRecord::SetCT for the land shaders, before their constants go up.
	static void	BeginWorldScene();
	static void	EndWorldScene();
	void	PrepareBlending();
	// UNOFFICIAL 2026-10-27 displacement ObjectBlend: the same copy for the land's tessellation (TerrainTemplate.hlsl TESR_DisplaceDepth),
	// from ShaderRecord::SetCT for the land shaders that carry it; it tells core/DxvkCompute whether this draw may use it.
	void	PrepareDisplaceDepth();

private:
	bool	CopySceneDepth();	// the copy at the first land bind of the world scene; true when this draw is in that scene

public:
	// Diagnostic: game passes bound in the world scene before its first land draw (SetShadersHook). The blend only sees the
	// objects drawn before the land, so the log line at the first copy says whether there were any.
	static void	NoteWorldBind() { if (BlendDepthState == 1) BindsBeforeCopy++; }

private:
	static inline int					BlendDepthState = 0;		// 0 outside the world scene, 1 copy still to take, 2 taken
	static inline IDirect3DSurface9*	BlendWorldDepth = nullptr;	// depth surface bound as the world scene starts (compared only)
	static inline unsigned				BindsBeforeCopy = 0;
};