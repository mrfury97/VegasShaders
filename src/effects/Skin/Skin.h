#pragma once

// The skin shader (UNOFFICIAL 2026-10-25: rewritten on this build's own stack, SkinTemplate.hlsl + Includes/Skin.hlsl). Every
// SKIN20xx vertex and pixel shader is built from SkinTemplate.hlsl (Templates below), the way ObjectTemplate.hlsl builds the
// object shaders. The SkinShader hooks (NewVegas/Hooks/Shaders.cpp) bind the FaceGen maps to the light-only face passes, mute
// the engine's own face highlight passes (the skin draws its own: [Shaders.Skin.Main] MuteGameHighlights) and bind the
// screen-space blur's targets (SkinScattering.h: CREDIT Conor (mrfury97), after Community Shaders and Jimenez & Gutierrez).
class SkinShaders : public ShaderCollection
{
public:
	SkinShaders() : ShaderCollection("Skin") {};

	// Set from core/Hooks/GameCommon.cpp when VanillaPlusSkin.dll is loaded (that mod's own NVSE plugin replaces the same
	// SKIN20xx shader names with its own lighting model, a subsurface-scattering LUT). With [Shaders.Skin.Main]
	// OverrideVanillaPlusSkin off, Templates() routes the SKIN names to SkinVPSTemplate.hlsl instead: VPS's own lighting
	// (ported with the author's permission, Includes/SkinVPS.hlsl) plus NVR's sun shadows. On (the default), this build's
	// skin shader wins, as it does without VPS.
	// UNOFFICIAL 2026-11-02 (user: "drop it for now we can direct ppl toward where to get that file"): the two Vanilla Plus Skin
	// files (SkinVPSTemplate.hlsl, Includes\SkinVPS.hlsl, pr0bability's GPLv3 code) are no longer shipped. The VPS route is taken
	// only when they have been put back in the shader folder; otherwise this build's skin shader is used and the log says why.
	bool bVPSLoaded = false;

	// Decided when the SKIN names are first loaded (after GameCommon.cpp has looked for VPS) and kept for the session: the
	// shader files are chosen then. Active: this build's skin shader is in use, and so are its hooks.
	bool UseVPS = false;
	bool Decided = false;
	bool Active() const { return !UseVPS; }

	struct SkinStruct {
		D3DXVECTOR4		Material;   // TESR_SkinMaterial (c192): x SpecularStrength, y Roughness, z Scattering, w Translucency
		D3DXVECTOR4		Look;       // TESR_SkinLook (c193): x SkyReflection (0 indoors), y wetness (rain x RainWetness), z ShadowScatter, w VanillaMatchedHighlights
		D3DXVECTOR4		Debug;      // TESR_SkinDebug (c195): x [Shaders.Skin.Debug] DebugView 1-7, y 1 while the Interiors settings are in use, z LegacyFaceShadows
		D3DXVECTOR4		SSS;        // UNOFFICIAL 2026-11-15 TESR_SkinSSS (c196, the deferred skin's passes only): xyz the Burley scatter distances (world units), w Transmission
	};
	SkinStruct Constants = {};

	// The skin material outdoors ([Shaders.Skin.Main]) and indoors ([Shaders.Skin.Interiors]); UpdateConstants picks one each frame.
	struct MaterialStruct {
		float SpecularStrength, Roughness, Scattering, Translucency, ShadowScatter;
	};
	MaterialStruct Exterior = {};
	MaterialStruct Interior = {};
	float SkyReflection = 1.0f;		// outdoors only: nothing occludes the sky the reflection samples, so indoors it is off
	float RainWetness = 0.5f;		// outdoors only
	float VanillaMatchedHighlights = 0.0f;	// UNOFFICIAL 2026-11-05 (PB NVR's skin lighting): 1 = the highlights as strong as vanilla's (Includes/Skin.hlsl SkinSetup)

	// [Shaders.Skin.Debug] DebugView. 1-7 are drawn by the skin shader (SkinTemplate.hlsl); 8-10 by the screen-space blur
	// (SkinScattering.fx.hlsl), which steps aside for 1-7 so it does not blur them.
	int DebugView = 0;
	bool MuteGameHighlights = true;	// [Shaders.Skin.Main] MuteGameHighlights (Hooks/Shaders.cpp): the engine's own face highlight passes off
	bool BindFaceGenMaps = true;	// diagnostic: [Shaders.Skin.Debug] BindFaceGenMaps (Hooks/Shaders.cpp BindFaceGenMaps)
	bool LegacyFaceShadows = false;	// diagnostic, live: [Shaders.Skin.Debug] LegacyFaceShadows (the template's shadow reach along the ray off, for an A/B)
	static const int DebugViewCount = 10;
	static const int FirstScatteringDebugView = 8;

	std::map<std::string_view, ShaderTemplate> Templates() {
		if (!Decided) {
			Decided = true;
			// 2026-11-03: looked up here too -- the shader precompile asks for these templates before GameCommon.cpp's check runs, so the
			// route was always decided as if VPS were absent (harmless with OverrideVanillaPlusSkin on, the default; off, VPS was never used).
			// VanillaPlusSkin.dll is loaded by then (xNVSE loads the plugins by name, before this one).
			if (GetModuleHandleA("VanillaPlusSkin.dll")) bVPSLoaded = true;
			UseVPS = bVPSLoaded && !TheSettingManager->GetSettingI("Shaders.Skin.Main", "OverrideVanillaPlusSkin");
			if (UseVPS && (GetFileAttributesA((std::string(ShadersPath) + "SkinVPSTemplate.hlsl").c_str()) == INVALID_FILE_ATTRIBUTES ||
				GetFileAttributesA((std::string(ShadersPath) + "Includes\\SkinVPS.hlsl").c_str()) == INVALID_FILE_ATTRIBUTES)) {
				UseVPS = false;
				Logger::Log("Skin: [Shaders.Skin.Main] OverrideVanillaPlusSkin is off, but the Vanilla Plus Skin compatibility files are not installed "
					"(%sShaders\\SkinVPSTemplate.hlsl and Includes\\SkinVPS.hlsl; they are an optional separate download, "
					"see the release notes): this build's skin shader is used", Paths::ShaderRootShort());
			}
			if (UseVPS) Logger::Log("Skin: Vanilla Plus Skin's shaders (SkinVPSTemplate.hlsl, [Shaders.Skin.Main] OverrideVanillaPlusSkin off)");
			else Logger::Log("Skin: SkinTemplate.hlsl for every SKIN shader%s", bVPSLoaded ? " (Vanilla Plus Skin is installed and overridden)" : "");
		}
		if (UseVPS) return VPSTemplates();
		return SkinTemplates();
	};

	// Vanilla Plus Skin's variant table (pr0bability/fnv-vanilla-plus-skin shaders/SkinTemplate.hlsl); the names it does not
	// cover load from no file, so the game's own shaders draw them, as that mod itself leaves them.
	std::map<std::string_view, ShaderTemplate> VPSTemplates() {
		return std::map<std::string_view, ShaderTemplate>{
			// Vertex shaders: ADTS Base/SKIN family, plus AD's LIGHTS=3/SKIN (ONLY_LIGHT).
			{ "SKIN2000.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}} } },
			{ "SKIN2001.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"SKIN", ""}} } },
			{ "SKIN2005.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"LIGHTS", "2"}, {"SKIN", ""}} } },
			{ "SKIN2013.vso", ShaderTemplate{ "SkinVPSTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"SKIN", ""}} } },

			// Pixel shaders: ADTS Base/PROJ_SHADOW/LIGHTS=2 family.
			{ "SKIN2000.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}} } },
			{ "SKIN2001.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2002.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "2"}} } },
			{ "SKIN2003.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			// Pixel shaders: AD's ONLY_LIGHT LIGHTS=2/3 family.
			{ "SKIN2004.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}} } },
			{ "SKIN2005.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2006.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}} } },
			{ "SKIN2007.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"LIGHTS", "3"}, {"PROJ_SHADOW", ""}} } },
			// Pixel shaders: DiffusePt point-light-only family.
			{ "SKIN2008.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "2"}} } },
			{ "SKIN2009.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"LIGHTS", "3"}} } },
			// Pixel shaders: ADTS10 many-lights family.
			{ "SKIN2010.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "9"}} } },
			{ "SKIN2011.pso", ShaderTemplate{ "SkinVPSTemplate", {{"PS", ""}, {"LIGHTS", "4"}} } },
		};
	};

	// Vanilla's skin variants, from the vanilla shaders' own constants and inputs (shaderpackage010.sdp); the defines are
	// SkinTemplate.hlsl's. Vertex shaders come in pairs: static, then bone-skinned.
	std::map<std::string_view, ShaderTemplate> SkinTemplates() {
		return std::map<std::string_view, ShaderTemplate>{
			// ADTS: the sun (+ the projected actor shadow), fog and vertex colour.
			{ "SKIN2000.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}} } },
			{ "SKIN2001.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"SKIN", ""}} } },
			{ "SKIN2002.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2003.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SKIN2004.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"POINTS", "1"}} } },
			{ "SKIN2005.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"POINTS", "1"}, {"SKIN", ""}} } },
			{ "SKIN2006.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"POINTS", "1"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2007.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"POINTS", "1"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			// AD: light only, the engine multiplies the texture in afterwards.
			{ "SKIN2008.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "1"}} } },
			{ "SKIN2009.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "1"}, {"SKIN", ""}} } },
			{ "SKIN2010.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "1"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2011.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "1"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			{ "SKIN2012.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "2"}} } },
			{ "SKIN2013.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "2"}, {"SKIN", ""}} } },
			{ "SKIN2014.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2015.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "2"}, {"PROJ_SHADOW", ""}, {"SKIN", ""}} } },
			// DiffusePt: lamps only, added on top.
			{ "SKIN2016.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"POINTS", "2"}} } },
			{ "SKIN2017.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"POINTS", "2"}, {"SKIN", ""}} } },
			{ "SKIN2018.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"POINTS", "3"}} } },
			{ "SKIN2019.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"DIFFUSE", ""}, {"POINTS", "3"}, {"SKIN", ""}} } },
			// ADTS10: the sun + up to 4 lamps, the live count in EmittanceColor.a.
			{ "SKIN2020.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"MANY", ""}, {"POINTS", "4"}} } },
			{ "SKIN2021.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"MANY", ""}, {"POINTS", "4"}, {"SKIN", ""}} } },
			{ "SKIN2022.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"MANY", ""}, {"POINTS", "3"}} } },
			{ "SKIN2023.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"MANY", ""}, {"POINTS", "3"}, {"SKIN", ""}} } },
			{ "SKIN2024.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"MANY", ""}, {"POINTS", "1"}} } },
			{ "SKIN2025.vso", ShaderTemplate{ "SkinTemplate", {{"VS", ""}, {"MANY", ""}, {"POINTS", "1"}, {"SKIN", ""}} } },

			{ "SKIN2000.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}} } },
			{ "SKIN2001.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2002.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"POINTS", "1"}} } },
			{ "SKIN2003.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"POINTS", "1"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2004.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "1"}} } },
			{ "SKIN2005.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "1"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2006.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "2"}} } },
			{ "SKIN2007.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"ONLY_LIGHT", ""}, {"POINTS", "2"}, {"PROJ_SHADOW", ""}} } },
			{ "SKIN2008.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"POINTS", "2"}} } },
			{ "SKIN2009.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"DIFFUSE", ""}, {"POINTS", "3"}} } },
			{ "SKIN2010.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"MANY", ""}, {"POINTS", "4"}} } },
			{ "SKIN2011.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"MANY", ""}, {"POINTS", "3"}} } },
			{ "SKIN2012.pso", ShaderTemplate{ "SkinTemplate", {{"PS", ""}, {"MANY", ""}, {"POINTS", "1"}} } },
		};
	};

	void	UpdateConstants(), RegisterConstants(), UpdateSettings();
};
