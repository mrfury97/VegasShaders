#pragma once

class PBRShaders : public ShaderCollection
{
public:
	PBRShaders() : ShaderCollection("PBR") {};

	using TemplateTable = std::map<std::string_view, ShaderTemplate>;
	TemplateTable Templates() {
		return TemplateTable{
			{ "SLS2000.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}} } },
			{ "SLS2001.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}} } },
			{ "SLS2003.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SKIN", ""}} } },
			{ "SLS2004.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2006.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2007.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}} } },
			{ "SLS2008.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2009.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SLS2010.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2011.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2012.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}} } },
			{ "SLS2013.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"SKIN", ""}} } },
			{ "SLS2014.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2015.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2016.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2017.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SLS2018.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2019.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2020.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "9"}} } },
			{ "SLS2021.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "9"}, {"SKIN", ""}} } },
			{ "SLS2022.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "4"}} } },
			{ "SLS2023.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"OPT", ""}} } },
			{ "SLS2024.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SKIN", ""}} } },
			{ "SLS2025.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}} } },
			{ "SLS2026.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}, {"OPT", ""}} } },
			{ "SLS2027.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}, {"SKIN", ""}} } },
			{ "SLS2028.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2029.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SLS2030.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2031.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2032.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}} } },
			{ "SLS2033.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"SKIN", ""}} } },
			{ "SLS2034.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2035.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2036.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2037.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SLS2038.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "3"}} } },
			{ "SLS2039.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "3"}, {"SKIN", ""}} } },
			{ "SLS2040.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}} } },
			{ "SLS2041.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"SKIN", ""}} } },
			{ "SLS2042.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2043.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SLS2044.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}} } },
			{ "SLS2045.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"SKIN", ""}} } },
			{ "SLS2046.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "2"}} } },
			{ "SLS2047.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SLS2048.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "3"}} } },
			{ "SLS2049.vso", ShaderTemplate{ "ObjectTemplate", {{"VS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "3"}, {"SKIN", ""}} } },
			{ "SLS2000.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}} } },
			{ "SLS2001.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"OPT", ""}} } },
			{ "SLS2002.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"OPT", ""}, {"LOD", ""}} } },
			{ "SLS2004.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SI", ""}} } },
			{ "SLS2005.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2007.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SI", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2008.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"STBB", ""}} } },
			{ "SLS2009.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"HAIR", ""}} } },
			{ "SLS2010.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"HAIR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2011.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2012.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"SI", ""}} } },
			{ "SLS2013.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"HAIR", ""}} } },
			{ "SLS2014.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2015.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"SI", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2016.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"HAIR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2017.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}} } },
			{ "SLS2018.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"SI", ""}} } },
			{ "SLS2019.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"HAIR", ""}} } },
			{ "SLS2020.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2021.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"SI", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2022.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"HAIR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2023.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2024.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}, {"SI", ""}} } },
			{ "SLS2026.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2027.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"SPECULAR", ""}, {"LIGHTS", "2"}, {"SI", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2029.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "9"}} } },
			{ "SLS2030.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "9"}, {"SI", ""}} } },
			{ "SLS2031.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "4"}} } },
			{ "SLS2032.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "4"}, {"OPT", ""}} } },
			{ "SLS2033.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "4"}, {"SI", ""}} } },
			{ "SLS2034.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}} } },
			{ "SLS2035.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}, {"OPT", ""}} } },
			{ "SLS2036.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"LIGHTS", "4"}, {"SPECULAR", ""}, {"SI", ""}} } },
			{ "SLS2037.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2038.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "2"}, {"SI", ""}} } },
			{ "SLS2039.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2040.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}, {"SI", ""}} } },
			{ "SLS2041.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "3"}} } },
			{ "SLS2042.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "3"}, {"SI", ""}} } },
			{ "SLS2043.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "3"}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2044.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"OPT", ""}, {"LIGHTS", "3"}, {"PROJ_SHADOW", ""}, {"SI", ""}} } },
			{ "SLS2045.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "2"}} } },
			{ "SLS2046.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "3"}} } },
			{ "SLS2047.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}} } },
			{ "SLS2048.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"HAIR", ""}} } },
			{ "SLS2049.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SLS2050.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"PROJ_SHADOW", ""}, {"HAIR", ""}} } },
			{ "SLS2051.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}} } },
			{ "SLS2052.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"HAIR", ""}} } },
			{ "SLS2053.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "2"}} } },
			{ "SLS2054.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "2"}, {"HAIR", ""}} } },
			{ "SLS2055.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "3"}} } },
			{ "SLS2056.pso", ShaderTemplate{ "ObjectTemplate", {{"PS", ""}, {"ONLY_SPECULAR", ""}, {"POINT", ""}, {"NUM_PT_LIGHTS", "3"}, {"HAIR", ""}} } },
			// UNOFFICIAL: the environment-map ("chrome") pass, by the light (EnvTemplate.hlsl). All four together: D3D9 does
			// not pair a 2.x shader with a 3.0 one, and SLS2050/2051 are the only game vertex shaders feeding SLS2057/2058.
			{ "SLS2050.vso", ShaderTemplate{ "EnvTemplate", {{"VS", ""}} } },
			{ "SLS2051.vso", ShaderTemplate{ "EnvTemplate", {{"VS", ""}, {"SKIN", ""}} } },
			{ "SLS2057.pso", ShaderTemplate{ "EnvTemplate", {{"PS", ""}} } },
			{ "SLS2058.pso", ShaderTemplate{ "EnvTemplate", {{"PS", ""}, {"NEGATE_VIEW", ""}} } }
		};
	};

	struct PBRSettings {
		float LightScale, AmbientScale;
		float Roughness, Metallicness, Saturation;	// Metallicness: read but unused (TESR_PBRData.x carries VanillaBridgeAmount since 2026-11-05)
		float VanillaBridgeAmount = 1.0f;	// UNOFFICIAL 2026-11-05 [Shaders.PBR.Main] VanillaBridgeAmount (only Default's is set: UpdateSettings)
		float SkylightingScale, SkylightingDirectionality;
		float SkylightingSaturation; // UNOFFICIAL, applied to the sky irradiance on the CPU (Sky.cpp)
		// UNOFFICIAL 2026-10-18 for materials with an _rmaos map only (idea from Physically-Based-Reloaded (Balebandro), GPL-3.0, c1415cb8):
		float RmaosMetalnessScale;	// MetalnessScale 0-1: the map's metal (G) times this
		float RmaosAOScale;			// AOScale 0-1: share of the map's occlusion (B), times [Shaders.PBR.Rmaos] Occlusion
		float RmaosRoughnessScale;	// RoughnessScale 1-2: the map's roughness (R) times this, at most 1 (rougher only)
	};
	struct PBRSettingsStruct {
		PBRSettings Default, Rain, Night, NightRain, Interiors;
	};
	PBRSettingsStruct Settings;

	struct PBRStruct {
		D3DXVECTOR4		Data, ExtraData;	// UNOFFICIAL ExtraData w: where the metal has faded out with distance (Object.hlsl getMetalFade)
		D3DXVECTOR4		Metal;		// UNOFFICIAL TESR_PBRMetal: x strength this pass, y grey parts' metal this pass, zw saturation ruling metal out
		D3DXVECTOR4		MetalLook;	// UNOFFICIAL TESR_PBRMetalLook: x bare metal roughness, y polished, z sky reflections, w debug view
		D3DXVECTOR4		Env;		// UNOFFICIAL TESR_PBREnv (EnvTemplate.hlsl): x Lighting, y Strength, z WithMetal
		D3DXVECTOR4		Math = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL TESR_PBRMath (Includes/PBR.hlsl, c223): x EnergySplit on
		D3DXVECTOR4		LinearScales = D3DXVECTOR4(1.0f, 1.0f, 1.0f, 0.0f);	// UNOFFICIAL TESR_LinearScales (Object.hlsl, c168): LinearPreDecode
		D3DXVECTOR4		LampPassTrim;	// UNOFFICIAL TESR_LampPassTrim (ObjectTemplate.hlsl lamp passes): x TrimLampPasses on
		D3DXVECTOR4		Translucency = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL TESR_Translucency (Grass23x00*.vso, STLEAF00*.vso): x Translucency, y Power, z SunGate; zero is off
		D3DXVECTOR4		SharpSky;	// UNOFFICIAL TESR_SharpSky (SkyAmbient.hlsl): x [Shaders.PBR.Metal] SharpSky, 0 = the plain sky
		D3DXVECTOR4		EnvCube = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL TESR_EnvCube (Includes/LiveCube.hlsl): x metal's, y the env-map pass's share of the live cube (0 while it is not kept up), z its last mip
		D3DXVECTOR4		Linear = D3DXVECTOR4(1.0f, 1.0f, 0.0f, 0.0f);	// UNOFFICIAL TESR_LinearLighting (Includes/LinearLight.hlsl): x gamma (1 = off), y 1 / x
		D3DXVECTOR4		ShadowContrast = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-10-29 TESR_ShadowContrast (ObjectTemplate.hlsl, c93): x strength indoors
		D3DXVECTOR4		LampFalloff = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-10-21 TESR_LampFalloff (Includes/LampFalloff.hlsl, c205): x shape on, y 1 / Falloff^2, z strength, w MatchEverywhere
	};
	PBRStruct	Constants;
	D3DXVECTOR4	HairData = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// UNOFFICIAL 2026-11-01 TESR_HairData (SM3003.pso.hlsl, c147): x HairHighlight - 1, y 1 = the game's look

	// 2026-11-04 dust on top surfaces (Includes/ObjectDust.hlsl) and micro-shadowing (Includes/Object.hlsl), both off by default
	struct DustSettingsStruct { float Amount, Size, UpFrom, UpFull, Patchiness; D3DXVECTOR3 Color; bool Exteriors, Interiors; };
	DustSettingsStruct DustSettings;
	D3DXVECTOR4	DustData = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);		// TESR_DustData (c192): x amount (0 off), y 1 / size, z UpFrom, w UpFull
	D3DXVECTOR4	DustColor = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// TESR_DustColor (c193): rgb linear colour, w Patchiness
	D3DXVECTOR4	DustOrigin = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// TESR_DustOrigin (c195): the camera's world position
	float		MicroShadowStrength = 0.0f;
	D3DXVECTOR4	MicroShadow = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);	// TESR_MicroShadow (c194): x strength

	// UNOFFICIAL [Shaders.PBR.Metal] (Object.hlsl getDerivedMetallic): metal guessed from the textures, with its own strength
	// for the first-person model (BeginFirstPerson / EndFirstPerson round that pass) and for the world.
	struct MetalSettingsStruct {
		float FirstPerson = 0.0f, World = 0.0f;
		// GreyMetal (first-person weapon) / WorldGreyMetal (everything else): the metal share where the shininess mask is bare,
		// TESR_PBRMetal.y, swapped round the first-person pass like FirstPerson / World. 2026-11-16: bare rock and concrete read as metal.
		float GreyMetal = 0.0f, WorldGreyMetal = 0.0f;
		float FadeDistance = 0.0f;	// FadeDistance; 0: where the game starts dropping highlights
		float CubeReflections = 0.0f;	// UNOFFICIAL [Shaders.PBR.Metal] CubeReflections: share of the live surroundings cube
		float EnvCubeReflections = 0.0f;	// UNOFFICIAL [Shaders.PBR.EnvMaps] CubeReflections: the same for the env-map pass
		float Reflections = 0.0f;	// [Shaders.PBR.Metal] Reflections, before Reflectivity (UpdateConstants)
	};
	MetalSettingsStruct	MetalSettings;

	// UNOFFICIAL 2026-10-25 [Shaders.PBR.Main] ReflectivityExterior / ReflectivityInterior: one multiplier on everything PBR surfaces
	// reflect (highlights through F0/F90 in Includes/PBR.hlsl, metal's surroundings, the env-map pass); Reflections.cpp reads the
	// same two for the floor sheen. 1 = as before.
	struct ReflectivitySettingsStruct {
		float Exterior = 1.0f, Interior = 1.0f;
	};
	ReflectivitySettingsStruct	ReflectivitySettings;

	// UNOFFICIAL [Shaders.PBR.MaterialRules] (core/MaterialRules.h, Object.hlsl): per-texture classes from the editable
	// table VegasShaders.materials.txt, sent before each object by Render.cpp MaterialRulesDraw. The registered
	// constants are the "no rule" values every object shader gets when it is bound.
	struct MaterialRulesSettingsStruct {
		bool Enabled = false, DebugView = false, LogMatches = false;
		bool MetalOn = false;	// [Shaders.PBR.Metal] MetalOn: the rules' metal is off with it too
	};
	MaterialRulesSettingsStruct	MaterialRulesSettings;
	// UNOFFICIAL 2026-10-14 [Shaders.PBR.Rmaos] (Render.cpp RmaosDraw, Object.hlsl RMAOS): Enabled compiles it in at startup;
	// On switches it live (off: every object as without maps).
	struct RmaosSettingsStruct {
		bool On = false;
		float Occlusion = 1.0f;	// share of the map's occlusion applied to the ambient light
		float OcclusionNow = 1.0f;	// UNOFFICIAL 2026-10-18: Occlusion x the weather's AOScale, this frame (PBRShaders::UpdateConstants; RmaosDraw sends it in c220.y)
		int DebugView = 0;		// 1 roughness, 2 metal, 3 occlusion, 4 which objects have a map
		bool LogMatches = false;
		bool MetalFromMap = false;	// UNOFFICIAL 2026-10-28 [Shaders.PBR.Rmaos] MetalFromMap: the map's metal (green) instead of the Metal sliders' look
	};
	RmaosSettingsStruct	RmaosSettings;
	D3DXVECTOR4	MaterialRule = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f);		// TESR_MaterialRule (c197): x 0 = no rule
	D3DXVECTOR4	MaterialRough = D3DXVECTOR4(-1.0f, -1.0f, 0.0f, 0.0f);	// TESR_MaterialRough (c198): z DebugView
	D3DXVECTOR4	MaterialTint = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);		// TESR_MaterialTint (c199)

	void	BeginFirstPerson(IDirect3DDevice9* Device);
	void	EndFirstPerson(IDirect3DDevice9* Device);
	// UNOFFICIAL: the current SkylightingSaturation (day/night/interior and rain blended like the other values);
	// SkyShaders::UpdateConstants applies it to the skylight's spherical-harmonic coefficients.
	float		SkylightSaturation = 1.0f;

	// UNOFFICIAL [Shaders.Foliage.Main] (Grass23x00*.vso, STLEAF00*.vso): backlit translucency. UpdateSettings reads it and
	// UpdateConstants copies it to Constants.Translucency every frame. Off by default.
	struct FoliageSettingsStruct {
		float Translucency = 0.0f, Power = 4.0f, SunGate = 4.0f;
	};
	FoliageSettingsStruct	FoliageSettings;

	// UNOFFICIAL [Shaders.PBR.Linear] gamma (Includes/LinearLight.hlsl): UpdateSettings reads it, ApplyLinearGamma (every frame, from
	// UpdateConstants) blends Gamma (day and interiors) into NightGamma (exteriors at night; 0 = the same as Gamma) on the day/night
	// transition curve and hands the result to the shaders, ExactPassSums and LinearPreDecode.
	struct LinearSettingsStruct {
		bool  Enabled = false;
		float Gamma = 1.0f, NightGamma = 0.0f, FalloffMatch = 1.0f;
	};
	LinearSettingsStruct	LinearSettings;
	void	ApplyLinearGamma();

	// UNOFFICIAL 2026-10-21 [Shaders.PBR.LampFalloff] (Includes/LampFalloff.hlsl): the lamps' physically shaped falloff (Exterior outdoors,
	// Interior indoors; Falloff, Brightness) and MatchEverywhere (the ground, skin and hair get the objects' FalloffMatch decode). Compiled
	// into the shaders only when one of the switches is on at startup (LampFalloffCompiledIn); then all of it is live.
	struct LampFalloffSettingsStruct {
		bool  Exterior = false, Interior = false, MatchEverywhere = false;
		float Falloff = 0.25f;		// the inverse-square core as a share of the lamp's radius: small = bright by the lamp, dim far from it
		float Brightness = 1.0f;	// times the strength that sends out the game's falloff's light (LampFalloffNorm)
		float Norm = 1.0f;			// LampFalloffNorm(Falloff)
	};
	LampFalloffSettingsStruct	LampFalloffSettings;
	static float	LampFalloffNorm(float Falloff);
	// As ShaderRecord::BuildDefines compiled the shaders: read once, at the first shader load (startup).
	enum LampFalloffSwitchEnum { LampFalloffExterior, LampFalloffInterior, LampFalloffMatch };
	static bool	LampFalloffAtStartup(int Which) {
		static const bool on[3] = { TheSettingManager->GetSettingI("Shaders.PBR.LampFalloff", "Exterior") != 0,
			TheSettingManager->GetSettingI("Shaders.PBR.LampFalloff", "Interior") != 0,
			TheSettingManager->GetSettingI("Shaders.PBR.LampFalloff", "MatchEverywhere") != 0 };
		return on[Which];
	}
	static bool	LampFalloffShapeIn() { return LampFalloffAtStartup(LampFalloffExterior) || LampFalloffAtStartup(LampFalloffInterior); }
	static bool	LampFalloffCompiledIn() { return LampFalloffShapeIn() || LampFalloffAtStartup(LampFalloffMatch); }
	// The interior lamp-shadow records' LAMP_FALLOFF_ON (they draw indoors only): nullptr = as the global one.
	static const char* LampFalloffInteriorDefine() {
		if (!LampFalloffShapeIn() || LampFalloffAtStartup(LampFalloffInterior)) return nullptr;
		return LampFalloffAtStartup(LampFalloffMatch) ? "1" : "0";
	}

	// UNOFFICIAL 2026-10-29 ShadowContrast (ObjectTemplate.hlsl, [Shaders.ShadowsInteriors.Forward] ShadowContrast): compiled into the
	// one-pass lamp shader only when above 0 at startup (as ShaderRecord::BuildDefines read it); then live.
	static bool	ShadowContrastCompiledIn() {
		static const bool on = TheSettingManager->GetSettingF("Shaders.ShadowsInteriors.Forward", "ShadowContrast") > 0.0f;
		return on;
	}

	void	UpdateConstants(), RegisterConstants(), UpdateSettings();
};
