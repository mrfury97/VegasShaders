#include "ShaderRecord.h"	// (also reached through Framework.h; kept for the IDE)
#include "FrameWorker.h"	// UNOFFICIAL 2026-11-03 SkyOnWorker (SetCT joins the sky's projection)
#include "LinearSceneShaders.h"	// UNOFFICIAL 2026-10-28 LinearScene
#include "CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals
#include "DisplaceHeight.h"	// UNOFFICIAL 2026-11-01 SameHeight
#include "DxvkCompute.h"	// UNOFFICIAL 2026-10-24 (NVR DXVK phase 2)
#include "MixCheck.h"	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
#include "ShaderWarmUp.h"
#include "PointShadowForward.h"
#include "ManyLamps.h"	// UNOFFICIAL 2026-10-27 ManyLampsOnePass
#include "DeferredLamps.h"	// UNOFFICIAL 2026-10-31 DeferredLamps
#include "LandLamps.h"	// UNOFFICIAL 2026-10-30 LandAllLamps
#include "ShaderPrecompile.h"
#include "TestTools.h"	// NVR_TEST_TOOLS (FailShaders)
#include "RegisterClash.h"	// UNOFFICIAL 2026-10-31 register-clash guard (X4500)
#include <unordered_set>	// SetCT's once-only "not bound" notes
#include "GpuProfiler.h"
#include "LinearPreDecode.h"	// UNOFFICIAL 2026-10-17
#include "BendContact.h"	// UNOFFICIAL 2026-11-06 forward Bend contact shadows (SUN_CONTACT_FORWARD)
#include "ReflMaterial.h"	// UNOFFICIAL 2026-10-20 MaterialAware reflections
#include "AlphaHash.h"	// UNOFFICIAL 2026-11-03 SmoothCutouts (ALPHA_HASH_ON)
#include "LandLight.h"	// UNOFFICIAL 2026-10-25 (SunFar)
// Small helpers for the shader loader: a template's define list is searched by name, and a held interface
// is released and cleared in one step.
namespace {
template <typename Element>
void freeArray(Element*& array) {
	delete[] array;
	array = nullptr;
}

// The Definition of the last of the template's defines named Name (null when there is none, or the last has none).
const char* lastDefinition(const ShaderTemplate& Template, const char* Name) {
	const char* found = nullptr;
	for (int d = 0; d < 63 && Template.Defines[d].Name; d++)
		if (!strcmp(Template.Defines[d].Name, Name)) found = Template.Defines[d].Definition;
	return found;
}

bool isOne(const char* definition) {
	return definition && !strcmp(definition, "1");
}

bool hasDefine(const ShaderTemplate& Template, const char* Name) {
	for (int d = 0; d < 63 && Template.Defines[d].Name; d++)
		if (!strcmp(Template.Defines[d].Name, Name)) return true;
	return false;
}

// Logs the compiler's messages and gives them to RegisterClash (null: nothing).
void logCompilerMessages(ID3DXBuffer* messages, const char* shaderName) {
	if (!messages) return;
	Logger::Log((char*)messages->GetBufferPointer());
	RegisterClash::Check(shaderName, (const char*)messages->GetBufferPointer());
}
}

ShaderProgram::ShaderProgram() : FloatShaderValues(nullptr), FloatShaderValuesCount(0), TextureShaderValues(nullptr), TextureShaderValuesCount(0) {}

ShaderProgram::~ShaderProgram() { freeArray(FloatShaderValues); freeArray(TextureShaderValues); }


ShaderRecord::ShaderRecord() : HasRenderedBuffer(false), HasDepthBuffer(false), ClearSamplers(true) {}
ShaderRecord::~ShaderRecord() = default;

/*
 * Forgets a named sampler's bound texture, so the next SetCT resolves it again.
 *
 * A TextureRecord keeps a raw texture pointer and SetCT binds only while that pointer is null. A texture that was
 * released and recreated would otherwise stay dangling in each consumer, so every consumer is cleared one by one
 * (each ShaderTextureValue has its own TextureRecord; there is nothing shared to reset).
 */
void ShaderProgram::ClearSampler(const char* SamplerName, size_t NameLength) {
	for (UInt32 i = 0; i < TextureShaderValuesCount; i++) {
		ShaderTextureValue& slot = TextureShaderValues[i];
		// GetTextureRecord bails without assigning Texture when the sampler type is
		// unrecognised, so this can legitimately be null.
		if (!slot.Texture) continue;
		if (memcmp(slot.Name, SamplerName, NameLength) == 0 && slot.Texture->Texture) slot.Texture->Texture = nullptr;
	}
}

void ShaderProgram::ReportError(HRESULT Hr) {
	static const struct { HRESULT code; const char* text; } kErrorTexts[] = {
		{ E_ABORT, "Operation aborted" }, { E_ACCESSDENIED, "Access Denied" }, { E_FAIL, "Operation Failed" },
		{ E_HANDLE, "Handle invalid" }, { E_INVALIDARG, "Argument invalid" }, { E_NOINTERFACE, "Interface invalid" },
		{ E_NOTIMPL, "Not implemented" }, { E_OUTOFMEMORY, "Out of memory" }, { E_POINTER, "Invalid pointer" },
		{ E_UNEXPECTED, "Unexpected fail" },
	};
	for (const auto& entry : kErrorTexts)
		if (Hr == entry.code) Logger::Log("%s", entry.text);
}

/*
* Whether a file is there.
*/
bool ShaderProgram::FileExists(const char* FilePath) {
	WIN32_FILE_ATTRIBUTE_DATA info = {};
	return GetFileAttributesExA(FilePath, GetFileExInfoStandard, &info) != 0;
}

/*
* True when the cached preprocessed source is present and equal to this build's.
*/
bool ShaderProgram::CheckPreprocessResult(const char* CachedPath, ID3DXBuffer* Source) {
	std::ifstream File(CachedPath, std::ios::in | std::ios::binary);
	if (!File.is_open()) return false;
	const std::string Cached((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	return !Cached.empty() && Cached == PortableSource(Source);
}

// UNOFFICIAL: the preprocessed source as it is cached and compared. D3DX writes the full path of the top file into its
// #line directives, so a cache made in one game folder never matched another and everything compiled again; only the
// part of those paths from "\Data\" on is kept. That lets a release ship a ready cache (the compiled shaders do not depend
// on the path; the compiler itself still gets the full source, for its error messages).
std::string ShaderProgram::PortableSource(ID3DXBuffer* Source) {
	std::string Text((const char*)Source->GetBufferPointer(), Source->GetBufferSize());
	size_t At = 0;
	while ((At = Text.find("#line ", At)) != std::string::npos) {
		const size_t LineEnd = Text.find('\n', At);
		const size_t Open = Text.find('"', At);
		const size_t Close = Open == std::string::npos ? std::string::npos : Text.find('"', Open + 1);
		if (Open != std::string::npos && Close != std::string::npos && (LineEnd == std::string::npos || Close < LineEnd)) {
			std::string Path = Text.substr(Open + 1, Close - Open - 1);
			for (char& c : Path) c = (char)tolower((unsigned char)c);
			const size_t Data = Path.rfind("\\\\data\\\\");	// the path is C-escaped: two backslashes a separator
			if (Data != std::string::npos) Text.erase(Open + 1, Data + 2);
		}
		At += 6;
	}
	return Text;
}

// UNOFFICIAL 2026-10-14: the defines every NVR game shader is compiled with (the template's, the overrides first, then the
// settings'), in its own function so ShaderPrecompile works them out the same way.
void ShaderRecord::BuildDefines(ShaderTemplate& Template, const D3DXMACRO* Overrides) {
	// Append a define to the template, keeping the array null-terminated.
	auto AppendDefine = [&Template](const char* DefineName, const char* Value) {
		D3DXMACRO* list = Template.Defines;
		int used = 0;
		for (; used < 62 && list[used].Name; used++)
			if (!strcmp(list[used].Name, DefineName)) return;	// an override already set it
		if (used >= 62) {  // out of room (Defines[64]: 62 entries and the terminator); skip rather than overrun
			static bool warned = false;	// fixes-a #12: say so once (a dropped define = the shader compiled without that option)
			if (!warned) { warned = true; Logger::Log("[ERROR] ShaderRecord: more than 62 shader defines, '%s' and any after it are dropped", DefineName); }
			return;
		}
		list[used] = { DefineName, Value };
		list[used + 1] = { NULL, NULL };	// keep the list terminated
	};

	// UNOFFICIAL: overrides first, so the global defines below with the same name are skipped.
	if (Overrides)
		for (const D3DXMACRO* o = Overrides; o->Name; o++) AppendDefine(o->Name, o->Definition);

	if (TheRenderManager->IsReversedDepth()) AppendDefine("REVERSED_DEPTH", "");

	// Forward sun shadows. Read straight from the setting manager rather than from the
	// ShadowsExteriors effect, because shaders can be loaded before that effect is built.
	//
	// This governs whether the forward path is compiled in at all, so that builds which do not
	// want it pay nothing for it. Whether it RUNS is a separate, runtime decision made by
	// TESR_ShadowForwardData -- see ShadowsExteriorEffect::UpdateSettings. Changing the setting
	// alters the preprocessed source, so CheckPreprocessResult recompiles on next load.
	const bool forwardShadows = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") != 0;
	AppendDefine("FORWARD_SHADOWS", forwardShadows ? "1" : "0");
	// UNOFFICIAL 2026-11-06 forward Bend contact shadows (core/BendContact.h): the lookup in GetSunShadow, only when on at start
	if (BendContact::WantedAtStart()) AppendDefine("SUN_CONTACT_FORWARD", "1");

	// UNOFFICIAL sun shadow cross-fade (SunSmoothing CrossFade): compiled in only when on, so that otherwise the
	// shaders are exactly what they were. Read here for the same reason as FORWARD_SHADOWS; takes effect on restart.
	// UNOFFICIAL optional contact-hardening sun shadows: compiled in only when on at startup, like SUN_CROSSFADE.
	AppendDefine("CONTACT_HARDENING", TheSettingManager->GetSettingI("Shaders.ContactHardening.Status", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-10-22 CasterReachEverywhere, a TEST switch: CharacterShadowReach's slack cap (Includes/Shadow.hlsl ShadowSlackCap)
	// compiled into every forward shader instead of the skin shaders only. Off (the default): the other shaders are exactly what they were.
	AppendDefine("SHADOW_SLACK_CAP_ALL", TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "CasterReachEverywhere") ? "1" : "0");
	// UNOFFICIAL 2026-10-15: interior forward lamp shadows with smooth edges (Includes/PointShadowForward.hlsl)
	AppendDefine("POINT_SHADOW_SMOOTH", TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "SmoothEdges") ? "1" : "0");
	// UNOFFICIAL 2026-10-21 (deep/lampedge) EvenTaps: High/Ultra's extra taps spread evenly; MagnifiedSoftness above 0: lamp shadow edges
	// at least that soft where the shadow map is magnified on screen. Each defined only when on, so that off the define list and the
	// shaders are exactly what they were (the list is near its 38-entry cap: 29 + 6 template defines + these two).
	if (TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "EvenTaps"))
		AppendDefine("POINT_SHADOW_EVEN", "1");
	// TexelFilter: the lamp shadows filtered as the sun's PCF (texel-aligned quadratic B-spline); MagnifiedSoftness is not needed with it
	// (and not compiled in: the six-lamp shaders have no room for both).
	const bool texelFilter = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "TexelFilter") != 0;
	if (texelFilter)
		AppendDefine("POINT_SHADOW_TEXEL", "1");
	else if (TheSettingManager->GetSettingF("Shaders.ShadowsInteriors.Forward", "MagnifiedSoftness") > 0.0f)
		AppendDefine("POINT_SHADOW_MAG", "1");
	// UNOFFICIAL 2026-10-28 RT lamp shadows (core/RtLampShadows.h): only when on at startup AND NVR DXVK can trace, so that otherwise
	// the define list and the shaders are exactly what they were.
	if (RtLampShadows::CompiledIn)
		AppendDefine("POINT_SHADOW_RT", "1");

	// UNOFFICIAL CubeReflections (Includes/LiveCube.hlsl): metal and the env-map pass read the live surroundings cube only when
	// compiled in, which is when either setting is above 0 at startup, so that otherwise those shaders are exactly what they were.
	AppendDefine("LIVE_CUBE", TheSettingManager->GetSettingF("Shaders.PBR.Metal", "CubeReflections") > 0.0f ||
		TheSettingManager->GetSettingF("Shaders.PBR.EnvMaps", "CubeReflections") > 0.0f ? "1" : "0");

	// UNOFFICIAL material rules (Includes/Object.hlsl, ObjectTemplate.hlsl): compiled in only when on at startup, so that
	// otherwise the object shaders are exactly what they were (the rules' branches cost 1-5% there even unused).
	AppendDefine("MATERIAL_RULES_ON", TheSettingManager->GetSettingI("Shaders.PBR.MaterialRules", "Enabled") ? "1" : "0");

	// UNOFFICIAL _rmaos maps (Includes/Object.hlsl, ObjectTemplate.hlsl, Render.cpp RmaosDraw): compiled in only when on at
	// startup, so that otherwise the object shaders are exactly what they were.
	AppendDefine("RMAOS_ON", TheSettingManager->GetSettingI("Shaders.PBR.Rmaos", "Enabled") ? "1" : "0");
	// 2026-11-04 dust on top surfaces (Includes/ObjectDust.hlsl) and micro-shadowing (Includes/Object.hlsl, needs RMAOS_ON): compiled in
	// only when on at startup, so that off the object and parallax shaders are exactly what they were.
	if (TheSettingManager->GetSettingI("Shaders.PBR.Dust", "Enabled")) AppendDefine("DUST_ON", "1");
	if (TheSettingManager->GetSettingI("Shaders.PBR.MicroShadow", "Enabled")) AppendDefine("MICRO_SHADOW_ON", "1");
	// UNOFFICIAL 2026-11-02 SpecularFromMap (Includes/Object.hlsl): the maps' alpha (specular level) is used unless switched off; defined
	// only when off, so the define list is unchanged by default.
	if (TheSettingManager->GetSettingI("Shaders.PBR.Rmaos", "Enabled") && !TheSettingManager->GetSettingI("Shaders.PBR.Rmaos", "SpecularFromMap"))
		AppendDefine("RMAOS_SPEC_OFF", "1");
	// UNOFFICIAL 2026-10-20 MaterialAware reflections (Includes/Object.hlsl ReflMaterialAlpha, effects/Reflections.cpp): the objects' first
	// passes leave their _rmaos material in the scene's alpha. Defined only when on at startup (with the _rmaos maps): off, the define
	// list and the shaders are exactly what they were (the list is near its 34-entry cap).
	if (ReflMaterial::CompiledIn())
		AppendDefine("REFL_MATERIAL_ON", "1");

	// UNOFFICIAL rain on the first-person weapon (Includes/HudRain.hlsl, ObjectTemplate.hlsl, Render.cpp HudRain): compiled in only
	// when Wet World's HudDrops is above 0 at startup.
	AppendDefine("HUD_RAIN_ON", TheSettingManager->GetSettingF("Shaders.WetWorld.Main", "HudDrops") > 0.0f ? "1" : "0");

	// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl: ObjectTemplate, TerrainTemplate): compiled in only when on at
	// startup, so that otherwise those shaders are exactly what they were. Gamma stays live (PBRShaders::UpdateSettings).
	AppendDefine("LINEAR_LIGHTING_ON", TheSettingManager->GetSettingI("Shaders.PBR.Linear", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-10-28 LinearScene (Includes/LinearLight.hlsl LinearSceneOut, core/LinearScene.h): compiled in only when on at startup
	// with linear lighting, so that otherwise the four lit templates are exactly what they were.
	AppendDefine("LINEAR_SCENE_ON", TheSettingManager->GetSettingI("Shaders.PBR.Linear", "Enabled") && TheSettingManager->GetSettingI("Shaders.PBR.Linear", "LinearScene") ? "1" : "0");
	// UNOFFICIAL 2026-10-28 CapturedNormals (Shaders/Includes/CapturedNormals.hlsl, core/CapturedNormals.h): the four lit templates write their
	// smooth vertex normal to COLOR3; compiled in only when a reader's switch is on at startup, so that otherwise they are exactly as before
	// (defined only then: the define list, and so the shader cache's keys, stay as they were with it off).
	// UNOFFICIAL 2026-10-29: 2 = with MaterialBuffer; 2026-10-31: 3 = with AlbedoBuffer too (COLOR1; docs/next/DEFERRED-RENDERING-PLAN.md rung A)
	if (CapturedNormals::CompiledIn()) AppendDefine("CAPTURED_NORMALS_ON", CapturedNormals::AlbedoCompiledIn() ? "3" : CapturedNormals::MaterialCompiledIn() ? "2" : "1");
	// UNOFFICIAL 2026-10-28 [Main.Main.Quality] CapturedNormals also gives the land and parallax templates their smooth vertex normal for
	// the sun lookup and the sky ambient (TerrainTemplate / ParallaxTemplate CAPTURED_NORMALS_FORWARD), as SmoothObjectNormals did for objects.
	if (CapturedNormals::MergeWanted()) AppendDefine("CAPTURED_NORMALS_FORWARD", "1");
#if NVR_TEST_TOOLS
	// UNOFFICIAL 2026-10-31 LampCapsCensus ([Main.Main.Misc], development builds; Includes/PointShadowForward.hlsl LAMP_CAPS_VIEW, the lamps without a
	// shadow source as debug view 4): defined only when on at startup, so that otherwise the define list and the shaders are exactly as before.
	if (TheSettingManager->GetSettingI("Main.Main.Misc", "LampCapsCensus")) AppendDefine("LAMP_CAPS_VIEW", "1");
#endif

	// UNOFFICIAL physical sky (Includes/Atmosphere.hlsl, SKY.pso): compiled in only when on at startup, so that otherwise the
	// sky shader is exactly what it was. Its other settings are live (SkyShaders::UpdateSettings).
	AppendDefine("PHYSICAL_SKY_ON", TheSettingManager->GetSettingI("Shaders.Sky.Physical", "Enabled") ? "1" : "0");

	// UNOFFICIAL MoreLampsPerObject (Hooks.cpp patches the game's cap to match): the specular many-lamp object shaders take
	// 6 lamps instead of 3. Read at startup, like the engine patch.
	AppendDefine("SPECULAR_LAMPS6", TheSettingManager->GetSettingI("Main.Main.Misc", "MoreLampsPerObject") ? "1" : "0");
	// UNOFFICIAL 2026-10-17 LampReachSkip (ObjectTemplate.hlsl LAMP_IF, ManyLightsDirect.hlsl): each lamp's light term in a branch on
	// its attenuation, skipped where the lamp cannot reach (it adds exactly 0 there). Compile time (a live switch cost 0.6 ms indoors).
	AppendDefine("LAMP_REACH_SKIP", TheSettingManager->GetSettingI("Shaders.PBR.Main", "LampReachSkip") ? "1" : "0");
	// UNOFFICIAL 2026-10-25 DxvkLampBranches (ObjectTemplate.hlsl NO_SLOT_LIMIT): on DXVK, which has no ~3,600-slot limit, the 6-lamp
	// object shaders skip unused lamp slots, out-of-reach lamps and their shadow lookups. Native D3D9 would refuse those shaders: 0 there.
	AppendDefine("NO_SLOT_LIMIT", (TheSettingManager->GetSettingI("Shaders.PBR.Main", "DxvkLampBranches") &&
		TheRenderManager && !strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4)) ? "1" : "0");
	// UNOFFICIAL 2026-10-27 ManyLampsOnePass (core/ManyLamps.h, ObjectTemplate.hlsl MANY_LAMPS): the 4-lamp-and-up shiny object shaders take
	// up to 24 lamps from c37-c63, c76-c87 and c98-c107, lit per pixel through the surface's tangent frame. DXVK only (shader length), read at startup.
	// UNOFFICIAL 2026-10-29: 2 = LightCommonLamps (the lamps from the frame's lamp table, Includes/LightCommon.hlsl)
	AppendDefine("MANY_LAMPS_ON", ManyLamps::ShaderDefine());
	// UNOFFICIAL 2026-10-31 LampTableStandalone / TableLamps (core/ManyLamps.h, Includes/LightCommon.hlsl): the table's rows when they are not
	// what MANY_LAMPS_ON implies, and the first tile row. Defined only when on at startup (otherwise the shaders and the cache's keys are as before).
	if (ManyLamps::RowsOverride()) { static char rows[16]; sprintf_s(rows, "%u.0f", ManyLamps::TableRowsNow()); AppendDefine("LIGHT_COMMON_ROWS_N", rows); }
	if (ManyLamps::TileRowsAtStartup()) { static char row[16]; sprintf_s(row, "%u.0f", ManyLamps::TileRow()); AppendDefine("TABLE_LAMP_SHADOWS", row); }
	if (ManyLamps::ListTexelsNow() != (int)ManyLamps::ListTexels) { static char texels[16]; sprintf_s(texels, "%d.0f", ManyLamps::ListTexelsNow()); AppendDefine("LIGHT_COMMON_LIST_TEXELS", texels); }	// UNOFFICIAL 2026-10-31 DeferredLamps: 16-texel froxel lists
	// UNOFFICIAL 2026-10-30 LandAllLamps (core/LandLamps.h, TerrainTemplate.hlsl): every land shader with lamps compiled for 24. Defined only
	// when on at startup, so that otherwise the land shaders (and the cache's keys) are exactly as before.
	if (LandLamps::CompiledIn()) AppendDefine("LAND_ALL_LAMPS", "1");
	// UNOFFICIAL 2026-10-30 ShadowAtlas (core/ShadowAtlas.h, PointShadowForward.hlsl): the lamp shadows read from one octahedral atlas.
	// Defined only when on at startup (otherwise the shaders and the cache's keys are as before).
	if (PointShadowForward::AtlasWanted()) AppendDefine("POINT_SHADOW_ATLAS", "1");	// (P85: DXVK only, PointShadowForward::AtlasWanted)
	// UNOFFICIAL 2026-10-17 UnlitShadowSkip (ObjectTemplate.hlsl SunShadowWanted): no forward sun-shadow lookups on object pixels whose
	// bumped normal faces away from the sun (the sun adds exactly nothing there). Compile time, like LAMP_REACH_SKIP.
	AppendDefine("UNLIT_SHADOW_SKIP", TheSettingManager->GetSettingI("Shaders.PBR.Main", "UnlitShadowSkip") ? "1" : "0");
	// UNOFFICIAL 2026-10-18 SmithCorrelated (Includes/PBR.hlsl BRDF + SmithCorrelatedKeep; idea from Physically-Based-Reloaded (Balebandro),
	// GPL-3.0): the object and parallax pixel shaders take the exact height-correlated Smith masking term (Heitz 2014) and the coupled
	// diffuse it needs. A look change, compiled in only when on at startup, so that off the shaders are exactly what they were.
	// fixes-a #11: only 1 is on; any other value is off. The old test mode 2 (slow, and refused by the GPU in one indoor many-lamp
	// shader) is no longer reachable from the menu or the config.
	{
		const int smith = TheSettingManager->GetSettingI("Shaders.PBR.Main", "SmithCorrelated");
		AppendDefine("SMITH_CORRELATED_ON", smith == 1 ? "1" : "0");
	}
	// UNOFFICIAL 2026-10-18 SpecularFade (Includes/PBR.hlsl PBR_SPEC_FADE, core/PbrPop.h): the object shaders' highlights fade out with the
	// game's specular distance fade instead of vanishing at once where the game swaps in the shaders without one. Compiled in only when on
	// at startup (the per-draw value then switches it live: 1 = the old result exactly).
	AppendDefine("SPEC_FADE_ON", TheSettingManager->GetSettingI("Shaders.PBR.Main", "SpecularFade") ? "1" : "0");
	// UNOFFICIAL 2026-10-21 FoldHighlights (ObjectTemplate.hlsl NVR_FOLD, core/FoldHighlights.h): the light-only and extra-lamp passes of shiny
	// objects with 3+ lamps can draw their lamps' highlights (and metal) themselves, as the one-pass shaders do. Compiled in only when on at
	// startup (the per-draw switch then turns it on live: 0 = the old result exactly).
	AppendDefine("FOLD_HIGHLIGHTS_ON", TheSettingManager->GetSettingI("Shaders.PBR.Main", "FoldHighlights") ? "1" : "0");
	// UNOFFICIAL 2026-10-22 LampDistanceFix (ObjectTemplate.hlsl LAMP_ATT_SQ): the 2- and 3-lamp object shaders take each lamp's falloff from the
	// lamp vector squared per pixel (as the game's shaders do) instead of a squared distance blended from the vertices, which left big flat
	// surfaces near a lamp (rugs, floors, posters, walls) almost unlit. Compiled in only when on at startup; off = the shaders as before.
	AppendDefine("LAMP_VECTOR_ATT_ON", TheSettingManager->GetSettingI("Shaders.PBR.Main", "LampDistanceFix") ? "1" : "0");
	// UNOFFICIAL 2026-10-21 LampSums (ObjectTemplate.hlsl NVR_LAMP_SUMS, core/PassSums.h): the extra lamp passes add their light to the light-only
	// pass's in linear light (exact where one such pass covers a pixel). Compiled in only when on at startup (with linear lighting); the
	// per-run switch then turns it on live: 0 = the old result exactly.
	AppendDefine("LAMP_SUMS_ON", TheSettingManager->GetSettingI("Shaders.PBR.Main", "LampSums") ? "1" : "0");
	// UNOFFICIAL 2026-10-25 HighlightSums (ObjectTemplate.hlsl / ParallaxTemplate.hlsl NVR_LAMP_SUMS in the ONLY_SPECULAR variants, core/PassSums.h):
	// the highlight passes FoldHighlights leaves to the game (parallax objects above all) add their light in linear light the same way.
	// Compiled in only when on at startup (with linear lighting); the per-run switch then turns it on live: 0 = the old result exactly.
	AppendDefine("HIGHLIGHT_SUMS_ON", TheSettingManager->GetSettingI("Shaders.PBR.Main", "HighlightSums") ? "1" : "0");
	// UNOFFICIAL 2026-10-17 LinearPreDecode (core/LinearPreDecode.h, ObjectTemplate.hlsl LIN_PREDECODED): with linear lighting on, the lit
	// objects' colour constants are decoded once on the CPU instead of per pixel. Read at startup, like LINEAR_LIGHTING_ON.
	AppendDefine("LIN_PREDECODE_ON", TheSettingManager->GetSettingI("Shaders.PBR.Linear", "PreDecode") ? "1" : "0");
	// UNOFFICIAL 2026-10-21 LampFalloff (Includes/LampFalloff.hlsl, effects/PBR.cpp): the lamps' physically shaped falloff and MatchEverywhere,
	// compiled in only when one of the switches is on at startup (then live), and defined only then: off, the define list and every
	// shader are exactly what they were.
	// 1: the constant and MatchEverywhere only; 2: the shape too (Exterior or Interior). The interior lamp-shadow records override it
	// without Interior (ShaderManager::LoadShader, PBRShaders::LampFalloffInteriorDefine).
	if (PBRShaders::LampFalloffCompiledIn())
		AppendDefine("LAMP_FALLOFF_ON", PBRShaders::LampFalloffShapeIn() ? "2" : "1");
	// UNOFFICIAL 2026-10-29 ShadowContrast (ObjectTemplate.hlsl, effects/PBR.cpp): defined only when on at startup, so that off every
	// shader is exactly what it was.
	if (PBRShaders::ShadowContrastCompiledIn()) AppendDefine("SHADOW_CONTRAST_ON", "1");

	// UNOFFICIAL terrain blending (TerrainTemplate.hlsl, from Physically-Based-Reloaded): compiled in only when on at startup,
	// like LIVE_CUBE, so that otherwise the land shaders are exactly what they were. TextureManager makes its depth copy
	// under the same condition.
	AppendDefine("TERRAIN_BLEND", TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-10-19 Terrain Variation (Includes/TerrainVariation.hlsl): compiled in only when on at startup, and only for the
	// first Layers blend layers (after Conor's port, 66653eea + d07ff4b9: it changes the shape of the land's sampling, not a value).
	{
		AppendDefine("TERRAIN_VARIATION", TheSettingManager->GetSettingI("Shaders.Terrain.Variation", "Enabled") ? "1" : "0");
		static char layerDefine[2] = { '3', 0 };
		layerDefine[0] = (char)('0' + std::clamp(TheSettingManager->GetSettingI("Shaders.Terrain.Variation", "Layers"), 1, 7));
		AppendDefine("TERRAIN_VARIATION_LAYERS", layerDefine);
	}
	// 2026-11-04 macro variation (Includes/TerrainMacro.hlsl): compiled in only when on at startup, like TERRAIN_VARIATION.
	AppendDefine("TERRAIN_MACRO", TheSettingManager->GetSettingI("Shaders.Terrain.Macro", "Enabled") ? "1" : "0");

	// UNOFFICIAL 2026-10-13 blood on the first-person weapon and gloves (Includes/HandBlood.hlsl, HandBlood.cpp): compiled in only
	// when on at startup, like TERRAIN_BLEND (the per-draw switch alone cost every lit object 2-5 %).
	AppendDefine("HAND_BLOOD", TheSettingManager->GetSettingI("Shaders.HandBlood.Status", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-10-16 cloth wind (Includes/ClothWind.hlsl, core/ClothWind.h): the vertex shaders get it only when on at startup.
	AppendDefine("CLOTH_WIND_ON", TheSettingManager->GetSettingI("Shaders.Cloth.Main", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-11-03 PipboyFit (Includes/PipboyFit.hlsl, core/PipboyFit.h): sleeves fitted under the Pip-Boy, likewise.
	AppendDefine("PIPBOY_FIT_ON", TheSettingManager->GetSettingI("Shaders.PipboyFit.Main", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-10-20 plant wind (Includes/PlantWind.hlsl, core/PlantWind.h): the static plants' part, likewise.
	AppendDefine("PLANT_WIND_ON", TheSettingManager->GetSettingI("Shaders.Plants.Wind", "Enabled") ? "1" : "0");
	// UNOFFICIAL 2026-11-03 SmoothCutouts (Includes/AlphaHash.hlsl, core/AlphaHash.h): the hashed alpha test, compiled in only when on at
	// startup (defined only then: the define list, and so the shader cache's keys, stay as they were with it off).
	if (AlphaHash::CompiledIn()) AppendDefine("ALPHA_HASH_ON", "1");

	AppendDefine("SUN_CROSSFADE", TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.SunSmoothing", "CrossFade") &&
		TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") ? "1" : "0");

	// Which skylighting model is compiled in. 0 = spherical harmonic irradiance, 1 = the older
	// single directional sample. Compile time rather than a runtime branch: ps_3_0 flattens
	// branches like this, so a runtime switch would make every lit pixel pay for BOTH paths.
	// Changing the setting alters the preprocessed source, so the cache recompiles on next load.
	AppendDefine("SKYLIGHTING_MODE",
		TheSettingManager->GetSettingI("Shaders.PBR.Main", "SkylightingMode") ? "1" : "0");

	// Shadow atlas encoding: 0 = VSM, 1 = EVSM2, 2 = EVSM4. Compile time for the same reason as
	// above -- Shadow.hlsl's GetSunShadow and ShadowMap.pso both branch on it with #if, not a
	// runtime read, since ps_3_0 would otherwise flatten all three variants into every shadowed
	// pixel. Read straight from the setting manager rather than from the ShadowsExteriors
	// effect, same reasoning as FORWARD_SHADOWS above.
	//
	// Must mirror ShadowsExteriorEffect::UpdateSettingsFromQuality's Quality -> Mode mapping:
	// Quality 0/1 -> VSM, 2 -> EVSM2, 3 -> EVSM4. Quality 4 (Custom) reads Mode directly, same
	// as UpdateSettingsFromQuality does for that case. Previously this was never defined, so it
	// silently fell back to Shadow.hlsl's own "#ifndef SHADOW_FIXED_MODE -> 2" default: every
	// shader was compiled assuming EVSM4 regardless of Quality, so GetSunShadow's
	// "TESR_ShadowFormatData.x != SHADOW_FIXED_MODE" check only ever matched at Quality 3/Full,
	// and forward shadows silently no-op'd (return unshadowed) at every other quality level.
	// UNOFFICIAL 2026-10-27: always 3 (PCF), as ShadowsExteriorEffect::UpdateSettingsFromQuality forces it (user: "force pcf from now on
	// no other options"); Quality and the old Mode setting no longer pick VSM / EVSM.
	AppendDefine("SHADOW_FIXED_MODE", "3");
	// UNOFFICIAL 2026-10-23 CharacterObjectReach: CharacterShadowReach's slack cap compiled into the object pixel shaders for the draws
	// flagged as an actor's head, hair, face mask or headgear (ObjectTemplate.hlsl, Includes/Shadow.hlsl SHADOW_SLACK_CAP_ACTOR). Defined
	// only when on, and last, so that off the list and the shaders are exactly what they were and it never pushes another define out.
	if (TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "CharacterObjectReach"))
		AppendDefine("SHADOW_CHARACTER_OBJECTS", "1");
	// UNOFFICIAL 2026-10-23 CharacterShadowMap ([Shaders.ShadowsExteriors.ShadowMaps]; Includes/Shadow.hlsl CharacterMapShadow): the
	// player's own sun shadow map, read by the skin shaders and the object pixel shaders. Defined only when on, and last, as above.
	// (ShadowsExteriorEffect::RegisterTextures reads the same setting for CharacterMapCompiled.)
	if (TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap"))
		AppendDefine("CHARACTER_SHADOW_MAP", "1");
	// UNOFFICIAL 2026-10-24 SmoothObjectNormals ([Shaders.PBR.Main], restart): the object and skin shaders' shadow lookups and sky ambient take
	// the vertex normal instead of the per-triangle one (Includes/Shadow.hlsl ShadowFacingNormal). 0: the shaders exactly as before.
	AppendDefine("OBJECT_SMOOTH_NORMALS", TheSettingManager->GetSettingI("Shaders.PBR.Main", "SmoothObjectNormals") ? "1" : "0");
	// UNOFFICIAL 2026-10-27 displacement ObjectBlend (TerrainTemplate.hlsl TESR_DisplaceDepth): with [Main.Main.Performance]
	// NvrDxvkDisplacement on at startup the land pixel shaders below 7 layers declare the scene's depth copy at s6, so the land's
	// tessellation shaders may read it (core/DxvkCompute.cpp RegisterDisplace). Defined only when on, and last, as above.
	if (TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkDisplacement"))
		AppendDefine("NVR_DISPLACE_DEPTH", "1");
	// UNOFFICIAL 2026-10-28 object displacement (ParallaxTemplate.hlsl NVR_DISPLACE_OBJECTS, [Shaders.POM.Displacement]): the parallax
	// objects' vertex and pixel shaders in the layout the object tessellation shaders read (fixed IO slots), under NVR DXVK only (the layout
	// carries a shadow position in COLOR1, which DXVK does not clamp). Defined only when on, and last, as above.
	if (DxvkCompute::DisplaceObjectsLayout())
		AppendDefine("NVR_DISPLACE_OBJECTS", "1");

}

/*
Loads a game shader by name from a subfolder (when given). It is compiled when no usable cache exists.
@returns the new record, or null when the shader is not used.
*/
ShaderRecord* ShaderRecord::LoadShader(const char* Name, const char* SubPath, ShaderTemplate Template, const char* CacheSubPath,
	const D3DXMACRO* Overrides) {
	TimeLogger loadClock;
	ShaderPrecompile::StartOnce();	// UNOFFICIAL: the first shader starts the parallel compile (once)

	ShaderRecord* record = nullptr;
	ID3DXBuffer* source = nullptr;
	ID3DXBuffer* binary = nullptr;
	ID3DXBuffer* messages = nullptr;
	ID3DXConstantTable* table = nullptr;
	void* function = nullptr;
	const char* profile = "";

	// Folders: the shaders root, its Cache\ (plus CacheSubPath or SubPath), and the source's folder (plus SubPath).
	const std::string shadersRoot = ShadersPath;
	std::string sourceDir = shadersRoot;
	std::string cacheDir = shadersRoot + "Cache\\";
	if (SubPath) sourceDir += SubPath;
	if (CacheSubPath) {
		cacheDir += CacheSubPath;
		CreateDirectoryA(cacheDir.c_str(), NULL);	// nothing else writes there
	}
	else if (SubPath) cacheDir += SubPath;

	const std::string sourcePath = sourceDir + (Template.Name != NULL ? Template.Name : Name) + ".hlsl";
	if (!FileExists(sourcePath.c_str())) {
		return record;
	}

	// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] PointLights (off by default): NVR's 1POINT grass shaders (GRASS23x004-007.vso,
	// GRASS23x001TMS/003TMS.pso: grass by a lamp or fire) only when on; off, the game's own as before (startup setting).
	// (2026-10-27: also their twins without transparency multisampling, GRASS23x001/003.pso, used with the game's MSAA off.)
	if (!memcmp(Name, "GRASS23x00", 10) && (!strcmp(Name + 10, "4.vso") || !strcmp(Name + 10, "5.vso") || !strcmp(Name + 10, "6.vso") ||
		!strcmp(Name + 10, "7.vso") || !strcmp(Name + 10, "1TMS.pso") || !strcmp(Name + 10, "3TMS.pso") ||
		!strcmp(Name + 10, "1.pso") || !strcmp(Name + 10, "3.pso")) &&
		!TheSettingManager->GetSettingI("Shaders.Grass.Main", "PointLights"))
		return record;

#if NVR_TEST_TOOLS
	// UNOFFICIAL 2026-10-30, test only (terrain flicker deep dive, docs/next/TERRAIN-FLICKER.md): [Main.Develop.Main] FailShaders, a list of
	// shader names ("SLS2132.pso,SLS2134.pso"), loads none of them, as a failed compile does (an older build's X4505 on the 6-layer
	// land): the game's own shader draws instead. Read at load (a restart). Missing key: nothing.
	{
		char fail[512];
		TheSettingManager->GetSettingS("Main.Develop.Main", "FailShaders", fail);
		const size_t length = strlen(Name);
		for (const char* f = strstr(fail, Name); f; f = strstr(f + 1, Name))
			if ((f == fail || f[-1] == ',' || f[-1] == ' ') && (f[length] == 0 || f[length] == ',' || f[length] == ' ')) {
				Logger::Log("UNOFFICIAL TEST FailShaders: %s not loaded (as a failed compile)", Name);
				return record;
			}
	}
#endif
	const std::string preprocessedPath = cacheDir + Name + ".hlsl";
	const std::string binaryPath = cacheDir + Name;

	D3DXMACRO* macros = &(Template.Defines[0]);

	BuildDefines(Template, Overrides);	// UNOFFICIAL 2026-10-14: in its own function, shared with ShaderPrecompile

	// UNOFFICIAL 2026-10-14 ParallelShaderCompile (ShaderPrecompile.h): the result a worker thread already made for this shader.
	const bool precompiled = ShaderPrecompile::Take(binaryPath.c_str(), &source, &binary);
	const HRESULT preprocess = precompiled ? D3D_OK : D3DXPreprocessShaderFromFileA(sourcePath.c_str(), macros, NULL, &source, &messages);
	if (preprocess != D3D_OK) {
		logCompilerMessages(messages, Name);
	}
	else {
		const bool recompile = precompiled ? false : !CheckPreprocessResult(preprocessedPath.c_str(), source);

		if (strstr(Name, ".vso")) profile = "vs_3_0";
		else if (strstr(Name, ".pso")) profile = "ps_3_0";
		const bool pixelProfile = profile[0] == 'p';

		if (precompiled) {
			function = binary ? binary->GetBufferPointer() : NULL;
			EffectStatus::ShaderLoaded(EffectStatus::Precompiled);	// UNOFFICIAL 2026-11-01 counters
		}
		else if (!recompile) {
			std::ifstream cached(binaryPath, std::ios::in | std::ios::binary | std::ios::ate);
			if (!cached.is_open()) {
				Logger::Log("ERROR: Shader binary %s not found.", binaryPath.c_str());
			}
			else {
				const std::streamoff size = cached.tellg();
				if (size > 0 && SUCCEEDED(D3DXCreateBuffer(size, &binary))) {
					cached.seekg(0, std::ios::beg);
					function = binary->GetBufferPointer();
					cached.read((char*)function, size);
					cached.close();
					EffectStatus::ShaderLoaded(EffectStatus::CacheHit);	// UNOFFICIAL 2026-11-01
				}
				else {
					cached.close();
					Logger::Log("ERROR: Failed to create buffer for shader binary %s", binaryPath.c_str());
				}
			}
		}

		if (!precompiled && (recompile || !function)) {
			// compile if option was enabled or compiled version not found
			D3DXCompileShader((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, "main", profile, NULL, &binary, &messages, &table);
			// UNOFFICIAL 2026-10-29: D3DX ran out of ps_3_0 temp registers (X4505). Measured: the 6-layer land shaders SLS2132-2138 with terrain
			// blending and captured normals both on fail in Includes/Parallax.hlsl at flags 0 while 5 and 7 layers compile -- an allocation
			// quirk, not a real budget. Compiled again with D3DXSHADER_PREFER_FLOW_CONTROL, which keeps the same branches and fits (all 256
			// combinations of the land options at 6 layers: scratch terrain_sweep.py). Only after such a failure: every shader that compiles
			// at flags 0 keeps exactly that bytecode.
			if (!binary && messages && strstr((const char*)messages->GetBufferPointer(), "X4505")) {
				Logger::Log("UNOFFICIAL %s: out of temp registers at the default flags; compiling again with PREFER_FLOW_CONTROL", Name);
				EffectStatus::ShaderLoaded(EffectStatus::FlowControlRetry);	// UNOFFICIAL 2026-11-01
				EffectStatus::Level(Name, "compiled with PREFER_FLOW_CONTROL (X4505 at the default flags)", false);
				messages->Release(); messages = nullptr;
				if (table) { table->Release(); table = nullptr; }
				D3DXCompileShader((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, "main", profile,
					D3DXSHADER_PREFER_FLOW_CONTROL, &binary, &messages, &table);
			}
			logCompilerMessages(messages, Name);
			EffectStatus::ShaderLoaded(binary ? EffectStatus::Compiled : EffectStatus::CompileFailed);	// UNOFFICIAL 2026-11-01
			if (!binary) EffectStatus::Level(Name, "FAILED to compile (the game's own shader draws instead)", false);
			if (binary) {
				function = binary->GetBufferPointer();
				std::ofstream binaryFile(binaryPath, std::ios::out | std::ios::binary);
				binaryFile.write((const char*)function, binary->GetBufferSize());
				binaryFile.flush();
				binaryFile.close();

				std::ofstream preprocessFile(preprocessedPath, std::ios::out | std::ios::binary);
				const std::string portable = PortableSource(source);	// UNOFFICIAL: see PortableSource
				preprocessFile.write(portable.data(), portable.size());
				preprocessFile.flush();
				preprocessFile.close();

				if (Template.Name) Logger::Log("Shader compiled: %s using template: %s", binaryPath.c_str(), sourcePath.c_str());
				else Logger::Log("Shader compiled: %s", binaryPath.c_str());
			}
		}

		const HRESULT tableResult = D3DXGetShaderConstantTableEx((const DWORD*)function, D3DXCONSTTABLE_LARGEADDRESSAWARE, &table);
		if (FAILED(tableResult)) {
			Logger::Log("Encountered an issue getting constant table for %s", Name); ReportError(tableResult);	// no record
		}
		else {
			D3DXCONSTANTTABLE_DESC tableDesc;
			HRESULT descResult = table->GetDesc(&tableDesc);
			ReportError(descResult);

			if (FAILED(descResult)) Logger::Log("Issues getting constants descriptions for %s", Name);
			else if (binary) {
				HRESULT created = E_FAIL;
				if (profile[0] == 'v') {
					record = new ShaderRecordVertex(Name);
					// UNOFFICIAL 2026-10-27 (NVR DXVK 1.7-displace, core\DxvkCompute.h): with NvrDxvkDisplacement on at load, the land's
					// vertex shader is registered with its tessellation control shader. Off, or no NVR DXVK: nothing happens.
					record->DisplaceFamily = (unsigned char)DxvkCompute::RegisterDisplace(Name, Template.Name, Template.Defines, true, function, D3DXGetShaderSize((const DWORD*)function));
					created = TheRenderManager->device->CreateVertexShader((const DWORD*)function, &(static_cast<ShaderRecordVertex*>(record)->ShaderHandle));
				}
				else {
					record = new ShaderRecordPixel(Name);
					// UNOFFICIAL 2026-10-24 (NVR DXVK phase 2, core\DxvkCompute.h): a Compute\Replace\<Name>.spv is registered before the
					// device sees the bytecode, so DXVK binds it in this shader's place. No file, no extension: nothing happens.
					DxvkCompute::RegisterReplacement(Name, function, D3DXGetShaderSize((const DWORD*)function), Template.Name ? Template.Name : Name, macros);
					// UNOFFICIAL 2026-10-27 (NVR DXVK 1.7-displace): the land's pixel shaders with their tessellation evaluation shaders.
					record->DisplaceFamily = (unsigned char)DxvkCompute::RegisterDisplace(Name, Template.Name, Template.Defines, false, function, D3DXGetShaderSize((const DWORD*)function));
					created = TheRenderManager->device->CreatePixelShader((const DWORD*)function, &(static_cast<ShaderRecordPixel*>(record)->ShaderHandle));
					// UNOFFICIAL 2026-11-01 SameHeight (core/DisplaceHeight.h): a displaced object shader and its HeightMap sampler
					if (SUCCEEDED(created) && record->DisplaceFamily == DxvkCompute::kDisplaceObjects) {
						D3DXHANDLE height = table->GetConstantByName(NULL, "HeightMap");
						if (height) DisplaceHeight::Note(static_cast<ShaderRecordPixel*>(record)->ShaderHandle, (int)table->GetSamplerIndex(height));
					}
				}

				if (SUCCEEDED(created)) {
					record->CreateCT(source, table);
					// UNOFFICIAL 2026-10-27 displacement shadows (core/DisplaceShadows.h): the land pixel shaders and their layer counts, so the
					// land's own draws can be told apart (their per-layer height flags are taken there for the land's shadow draws).
					if (pixelProfile && Template.Name && !strcmp(Template.Name, "TerrainTemplate"))
						DxvkCompute::NoteLandPixelShader(static_cast<ShaderRecordPixel*>(record)->ShaderHandle, Template.Defines);
					// UNOFFICIAL 2026-10-28 LinearScene (core/LinearScene.h): the pixel shaders that can give linear light (LinearSceneOut).
					if (pixelProfile && Template.Name && (!strcmp(Template.Name, "ObjectTemplate") || !strcmp(Template.Name, "ParallaxTemplate") ||
						!strcmp(Template.Name, "SkinTemplate") || !strcmp(Template.Name, "TerrainTemplate"))) {
						if (isOne(lastDefinition(Template, "LINEAR_SCENE_ON"))) LinearScene::NoteCapable(static_cast<ShaderRecordPixel*>(record)->ShaderHandle);
						// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): the pixel shaders that write the normal target (COLOR3).
						const char* capturedOn = lastDefinition(Template, "CAPTURED_NORMALS_ON");
						const bool writes = capturedOn && (!strcmp(capturedOn, "1") || !strcmp(capturedOn, "2") || !strcmp(capturedOn, "3"));	// (3: 2026-10-31 AlbedoBuffer)
						if (writes) CapturedNormals::NoteWriter(static_cast<ShaderRecordPixel*>(record)->ShaderHandle);
						// UNOFFICIAL 2026-10-29 MaterialBuffer: Object / Parallax / Terrain write it on COLOR2 (the skin templates' COLOR2 is the skin blur's)
						if (writes && Template.Name && (!strcmp(Template.Name, "ObjectTemplate") || !strcmp(Template.Name, "ParallaxTemplate") || !strcmp(Template.Name, "TerrainTemplate"))) {
							CapturedNormals::NoteMaterialWriter(static_cast<ShaderRecordPixel*>(record)->ShaderHandle);
							// UNOFFICIAL 2026-10-31 AlbedoBuffer: the same three templates write COLOR1 (the ONLY_SPECULAR records keep their highlightCopy there:
							// no writer); the ONLY_LIGHT / DIFFUSE records are counted apart (their albedo is the texture a later pass multiplies in)
							if (CapturedNormals::AlbedoCompiledIn()) {
								const bool specOnly = hasDefine(Template, "ONLY_SPECULAR");
								const bool lightOnly = hasDefine(Template, "ONLY_LIGHT") || hasDefine(Template, "DIFFUSE");
								if (!specOnly) CapturedNormals::NoteAlbedoWriter(static_cast<ShaderRecordPixel*>(record)->ShaderHandle, lightOnly);
							}
						}
						// UNOFFICIAL 2026-10-29 ShadowContrast: the object shader's added lamp passes (DIFFUSE) give their shadows' light to it
						if (writes && PBRShaders::ShadowContrastCompiledIn() && !strcmp(Template.Name, "ObjectTemplate")) {
							if (hasDefine(Template, "DIFFUSE")) CapturedNormals::NoteContrastWriter(static_cast<ShaderRecordPixel*>(record)->ShaderHandle);
						}
					}
					// UNOFFICIAL 2026-10-17 LinearPreDecode (core/LinearPreDecode.h): ObjectTemplate pixel shaders compiled with linear lighting
					// and LIN_PREDECODE_ON read their colour constants already decoded; the constant hook decodes them while one is bound.
					if (pixelProfile && Template.Name && !strcmp(Template.Name, "ObjectTemplate")) {
						// (2026-10-28: the whole list; MANY_LAMPS_ON moved LIN_PREDECODE_ON one later)
						const bool linear = isOne(lastDefinition(Template, "LINEAR_LIGHTING_ON"));
						const bool predecode = isOne(lastDefinition(Template, "LIN_PREDECODE_ON"));
						if (linear && predecode) LinearPreDecode::Register(static_cast<ShaderRecordPixel*>(record)->ShaderHandle);
					}
					Logger::Log("Shader loaded: %s", binaryPath.c_str());
				}
				else {
					// UNOFFICIAL 2026-10-20 (P82 tester): which file, the device's answer and the compiler's slot count, so a report says why.
					// A shader longer than the driver takes is refused (the GTX 1070 took 3,596 slots and refused 3,820; a P82 tester's
					// RTX 4070 Ti SUPER refused the same shaders).
					unsigned slots = 0;
					ID3DXBuffer* listing = NULL;
					if (SUCCEEDED(D3DXDisassembleShader((const DWORD*)function, FALSE, NULL, &listing)) && listing) {
						const char* at = strstr((const char*)listing->GetBufferPointer(), "approximately ");
						if (at) sscanf_s(at, "approximately %u", &slots);
						listing->Release();
					}
					Logger::Log("ERROR: Failed to create DirectX shader for %s (%s): the device refused it (hr 0x%08X), %u instruction slots",
						Name, binaryPath.c_str(), (unsigned)created, slots);
					ReportError(created);
					delete record;
					record = nullptr;
				}
			}
		}
	}

	loadClock.LogTime("ShaderRecord::LoadShader");

	for (ID3DXBuffer* buffer : { source, binary, messages })
		if (buffer) buffer->Release();
	return record;
}

/**
* Builds this shader's TESR_ slots from its constant table.
* @param ShaderSource
* @param ConstantTable
*/
void ShaderRecord::CreateCT(ID3DXBuffer* ShaderSource, ID3DXConstantTable* ConstantTable) {
	D3DXCONSTANTTABLE_DESC tableDesc;
	D3DXCONSTANT_DESC constDesc;
	UINT descCount = 1;
	UInt32 floatSlot = 0;
	UInt32 texSlot = 0;

	const HRESULT descHr = ConstantTable->GetDesc(&tableDesc);
	if (FAILED(descHr)) Logger::Log("CreateCT : failed to get constants desc");
	ReportError(descHr);	// (logs nothing for success)

	//Logger::Log("CreateCT: Shader %s has %i constants", Name, ConstantTableDesc.Constants);
	//Logger::Log("%s", (const char*)ShaderSource->GetBufferPointer());

	// Count pass: the TESR_ floats and samplers, plus the two land-light markers (UNOFFICIAL 2026-10-25, LandLight.h: the game's SunDir in c18).
	bool sunDir18 = false, terrainSky = false;
	UInt32 newFloats = 0, newSamplers = 0;
	for (UINT c = 0; c < tableDesc.Constants; c++) {
		D3DXHANDLE handle = ConstantTable->GetConstant(NULL, c);
		ConstantTable->GetConstantDesc(handle, &constDesc, &descCount);
		if (!strcmp(constDesc.Name, "SunDir") && constDesc.RegisterSet == D3DXRS_FLOAT4 && constDesc.RegisterIndex == 18) sunDir18 = true;
		if (!strcmp(constDesc.Name, "TESR_TerrainSkyData")) terrainSky = true;
		if (memcmp(constDesc.Name, "TESR_", 5) != 0) continue; // Only treat constants named with TESR prefix
		if (constDesc.RegisterSet == D3DXRS_FLOAT4) newFloats++;
		if (constDesc.RegisterSet == D3DXRS_SAMPLER) newSamplers++;
	}
	FloatShaderValuesCount += newFloats;
	TextureShaderValuesCount += newSamplers;

	IsLandLight = sunDir18 && terrainSky && Name && strstr(Name, ".pso");

	TimeLogger tableClock;
	// Arrays only for the kinds the shader has (a shader without TESR_ samplers keeps a null table).
	if (const UInt32 floats = FloatShaderValuesCount) FloatShaderValues = new ShaderFloatValue[floats];
	if (const UInt32 samplers = TextureShaderValuesCount) TextureShaderValues = new ShaderTextureValue[samplers];
	tableClock.LogTime("ShaderRecord::createCT Malloc");

	// Fill pass: one slot per TESR_ constant, in declaration order.
	for (UINT c = 0; c < tableDesc.Constants; c++) {
		D3DXHANDLE handle = ConstantTable->GetConstant(NULL, c);
		ConstantTable->GetConstantDesc(handle, &constDesc, &descCount);
		if (memcmp(constDesc.Name, "TESR_", 5) != 0) continue; // Only treat constants named with TESR prefix

		switch (constDesc.RegisterSet) {
		case D3DXRS_FLOAT4: {
			ShaderFloatValue& value = FloatShaderValues[floatSlot++];
			value.Name = constDesc.Name;
			value.Type = constDesc.Type;
			value.RegisterIndex = constDesc.RegisterIndex;
			value.RegisterCount = constDesc.RegisterCount;
			break;
		}
		case D3DXRS_SAMPLER: {
			ShaderTextureValue& tex = TextureShaderValues[texSlot];
			tex.Name = constDesc.Name;
			tex.Type = TextureRecord::GetTextureType(constDesc.Type);
			tex.RegisterIndex = constDesc.RegisterIndex;
			tex.RegisterCount = 1;
			tex.GetSamplerStateString(ShaderSource, constDesc.RegisterIndex);
			tex.GetTextureRecord();

			// mark this shader as needing to render depth/a buffer of the scene before the object can be rendered
			if (memcmp(constDesc.Name, "TESR_DepthBuffer", 17) == 0) HasDepthBuffer = true;
			if (memcmp(constDesc.Name, "TESR_RenderedBuffer", 20) == 0) HasRenderedBuffer = true;
			if (!strcmp(constDesc.Name, "TESR_TerrainBlendDepth")) HasTerrainBlendDepth = true;	// UNOFFICIAL terrain blending
			if (!strcmp(constDesc.Name, "TESR_DisplaceDepth")) HasDisplaceDepth = true;	// UNOFFICIAL 2026-10-27 displacement ObjectBlend

			texSlot++;
			break;
		}
		default:
			Logger::Log("Found unsupported constant type in shader constant table: %s : %s", constDesc.Name, constDesc.RegisterSet);
			break;
		}
	}

	tableClock.LogTime("ShaderRecord::createCT Done");
}


// extract info about texture from the shader source code
void ShaderTextureValue::GetSamplerStateString(ID3DXBuffer* Source, UINT32 SamplerIndex) {
	const std::string source((const char*)Source->GetBufferPointer());

	// The sampler's declaration: "register ( s<SamplerIndex> )" and the rest of its line.
	const size_t declAt = source.find("register ( s" + std::to_string(SamplerIndex) + " )");
	if (declAt == std::string::npos) return (void)Logger::Log("[ERROR] %s  cannot be binded: sampler index not found", Name);

	const size_t lineEnd = source.find("\n", declAt + 1);
	const std::string declLine = source.substr(declAt, lineEnd - declAt);

	//Only these samplers are bindable to an arbitrary texture
	const size_t angleOpen = declLine.find("<");
	const size_t angleClose = declLine.rfind(">");
	if (angleOpen != std::string::npos && angleClose != std::string::npos) {
		const std::string annotation = declLine.substr(angleOpen + 1, angleClose - angleOpen - 1);

		// find texture path
		if (annotation.find("ResourceName") != std::string::npos) {
			const size_t quoteOpen = annotation.find("\"");
			const size_t quoteClose = annotation.rfind("\"");
			std::string path = annotation.substr(quoteOpen + 1, quoteClose - 1 - quoteOpen);
			path.insert(0, "Data\\Textures\\");
			TexturePath = path;
		}
	}

	// UNOFFICIAL 2026-10-19: the state block may run over several lines ('= sampler_state {' on the declaration's line, the states
	// and '};' below, as GunFX's VolumetricSmoke and BarrelHaze write them). Only the declaration's own line was searched, so those
	// samplers logged '[ERROR] ... Samplerstate description not found' at every start and silently ran with the default states
	// (WRAP, POINT) instead of their own (CLAMP, LINEAR for the half and quarter resolution smoke buffers).
	const size_t stateOpen = declLine.find("{");
	const size_t stateClose = declLine.rfind("}");
	if (stateOpen != std::string::npos && stateClose == std::string::npos) {
		const size_t blockStart = declAt + stateOpen;
		const size_t blockEnd = source.find("}", blockStart + 1);
		const size_t nextDeclaration = source.find("register", blockStart + 1);
		if (blockEnd != std::string::npos && (nextDeclaration == std::string::npos || blockEnd < nextDeclaration)) {
			std::string block = source.substr(blockStart + 1, blockEnd - blockStart - 1);
			for (char& c : block) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
			SamplerString = block;
			return;
		}
	}
	const bool sameLineBlock = stateOpen != std::string::npos && stateClose != std::string::npos;
	if (sameLineBlock) SamplerString = declLine.substr(stateOpen + 1, stateClose - stateOpen - 1);
	else Logger::Log("[ERROR] during binding of %s : Samplerstate description not found", Name);
}


/*
* Creates the texture record for a sampler from its path and state string.
*/
void ShaderTextureValue::GetTextureRecord() {
	TimeLogger recordClock;
	if (!Type) return (void)Logger::Log("[ERROR] Sampler %s doesn't have a valid type", Name);	// Texture stays null

	TextureRecord* record = new TextureRecord();
	Texture = record;
	// A file texture is read now; game/NVR textures are found by name at the first SetCT.
	if (!TexturePath.empty()) record->Texture = TheTextureManager->GetFileTexture(TexturePath, Type);
	// The shader's own sampler_state block, when it has one, replaces the default states.
	if (!SamplerString.empty()) record->GetSamplerStates(trim(SamplerString));

	recordClock.LogTime("ShaderTextureValue::GetTextureRecord");
}


/*
* Points a float slot at its value in the shared constants table (or at a new custom one).
*/
void ShaderFloatValue::GetValueFromConstantTable() {
	auto& constants = TheShaderManager->ConstantsTable;
	const auto found = constants.find(std::string(Name));

	if (found != constants.end()) {
		Value = found->second;
	}
	else {
		Logger::Log("Custom constant found: %s", Name);
		// Not an NVR constant: a zeroed entry of its own, which SetCustomConstant can fill later.
		D3DXVECTOR4& custom = TheShaderManager->CustomConst[Name];
		custom = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		Value = &custom;
	}
}


/*
* Binds this shader's textures and uploads its constants.
*/
void ShaderRecord::SetCT() {
	MixCheck::SetPass(Name);	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck: the game shader the next binds and draws belong to

	// UNOFFICIAL: the first use of a game shader this session goes on the warm-up list (ShaderWarmUp.h)
	if (!UsedOnce) { UsedOnce = true; ShaderWarmUp::NoteFirstUse(Name); }

	// The scene so far, for shaders that read it: the current colour target copied, the depth resolved.
	if (HasRenderedBuffer) {
		IDirect3DSurface9* current = TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface;
		TheRenderManager->device->StretchRect(current, NULL, TheTextureManager->RenderedSurface, NULL, D3DTEXF_NONE);
	}
	if (HasDepthBuffer) TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture);
	// UNOFFICIAL terrain blending: before the constants below are uploaded (it sets whether this draw blends), and the
	// first land bind of the world scene copies the depth drawn so far.
	if (HasTerrainBlendDepth && TheShaderManager->Shaders.Terrain) TheShaderManager->Shaders.Terrain->PrepareBlending();
	// UNOFFICIAL 2026-10-28 (object displacement): the fork has one tessellation state for every family, so a land or parallax-object shader
	// registered for it sends its family's state when the other's was the last sent (core/DxvkCompute.cpp DisplaceBind). The land and the
	// objects never share a pixel shader, and SetCT runs whenever the pixel shader changes.
	if (DisplaceFamily) DxvkCompute::DisplaceBind(DisplaceFamily);
	// UNOFFICIAL 2026-10-27 displacement ObjectBlend: the same copy, and whether this draw's tessellation may use it (Terrain.cpp).
	if (HasDisplaceDepth && TheShaderManager->Shaders.Terrain) TheShaderManager->Shaders.Terrain->PrepareDisplaceDepth();
	// UNOFFICIAL 2026-10-25 (LandLight.h): the light vector the last land draw lit with, then this bind's own turn.
	if (LandLight::Pending) LandLight::Read();
	if (IsLandLight) LandLight::Pending = true;

	// Pixel records start from empty stages (vertex records leave them alone).
	for (int stage = 0; ClearSamplers && stage < 16; stage++) TheRenderManager->renderState->SetTexture(stage, nullptr);

	// binds textures
	for (UInt32 i = 0; i < TextureShaderValuesCount; i++) {
		ShaderTextureValue& slot = TextureShaderValues[i];
		TextureRecord* tex = slot.Texture;
		if (tex->Texture == nullptr) {
			// UNOFFICIAL 2026-10-17: a texture that is registered but empty for now (TESR_ReflectionsEnv while the Reflections
			// cube is off -- the shader never reads it then) is re-read from its slot, not looked up by name on every draw.
			if (tex->TextureRef) tex->Texture = *tex->TextureRef;
			else tex->BindTexture(slot.Name);

			if (tex->Texture) Logger::Log("%s : Texture %s Successfully bound", Name, slot.Name);
			//else Logger::Log("[ERROR] : Could not bind texture %s for shader %s", slot.Name, Name);
		}

		if (tex->Texture != nullptr) {
			TheRenderManager->renderState->SetTexture(slot.RegisterIndex, tex->Texture);

			for (int state = 1; state < SamplerStatesMax; state++) {
				if (TheRenderManager->renderState->GetSamplerState(slot.RegisterIndex, (D3DSAMPLERSTATETYPE)state) != tex->SamplerStates[state])
					TheRenderManager->SetSamplerState(slot.RegisterIndex, (D3DSAMPLERSTATETYPE)state, tex->SamplerStates[state]);
			}
		}
		else {
			// UNOFFICIAL 2026-10-17: once per shader and texture, not on every draw (each line is flushed to disk: a player's
			// log reached 450 MB, 1.5 million lines of TESR_ReflectionsEnv in 47 minutes; 95% of our own test logs).
			static std::unordered_set<const void*> warned;
			if (warned.insert(&slot).second)
				Logger::Log("%s : Texture %s was not bound (said once; it binds by itself if it appears later)", Name, slot.Name);
		}
	}

	// update constants
	for (UInt32 i = 0; i < FloatShaderValuesCount; i++) {
		ShaderFloatValue& constant = FloatShaderValues[i];

		if (constant.Value == nullptr) constant.GetValueFromConstantTable();
		if (constant.Value == nullptr) {
			Logger::Log("[Error] %s : Couldn't get value for Constant %s", Name, constant.Name);
			continue;
		}

		SkyProjection::JoinIfSky(constant.Value);	// UNOFFICIAL 2026-11-03 SkyOnWorker: the sky's results first, when this is one of them
		SetShaderConstantF(constant.RegisterIndex, constant.Value, constant.RegisterCount); // TODO: for matrices, rows and columns are inverted because of the way this works.
	}
}


// The record a replaced game shader draws with: its Exterior variant in a worldspace, its Interior one in a cell,
// else the Default one; null when it has none of those.
template <typename ShaderEx>
static auto PickRecord(ShaderEx* Shader, bool InWorldspace) {
	auto* variant = Shader->GetShaderRecord(InWorldspace ? ShaderRecordType::Exterior : ShaderRecordType::Interior);
	return variant ? variant : Shader->GetShaderRecord(ShaderRecordType::Default);
}

/*
* Hook: puts the replacement's handle in place when the game sets up a pass (the game's own when NVR is off).
*/
void NiD3DVertexShaderEx::SetupShader(IDirect3DVertexShader9* BoundHandle) {
	auto useGameShader = [this] { ShaderHandle = (IDirect3DVertexShader9*)ShaderHandleBackup; };

	const bool nvrOff = !Enabled || !TheSettingManager->SettingsMain.Main.RenderEffects;
	if (nvrOff && ShaderHandleBackup) return useGameShader();

	// UNOFFICIAL 2026-10-22 MarkShaders, vanilla mode (test only): the vertex shaders SLS<VsFrom>..SLS<VsTo> as the game's own.
	if ((MarkShadersVanilla == 1 || MarkShadersVanilla == 3) && MarkShadersVsTo > 0 && ShaderHandleBackup && Name && !memcmp(Name, "SLS", 3)) {
		const int number = atoi(Name + 3);
		if (number >= MarkShadersVsFrom && number <= MarkShadersVsTo) return useGameShader();
	}

	if (ShaderRecordVertex* record = PickRecord(this, Player->GetWorldSpace() != nullptr)) {
		ShaderHandle = record->ShaderHandle;
		if (BoundHandle != ShaderHandle) record->SetCT();	// constants only when the handle changes
	}
	else if (ShaderHandleBackup) useGameShader();
}

void NiD3DVertexShaderEx::DisposeShader() {	// all three variants
	for (UInt32 slot = 0; slot < 3; slot++) {
		delete ShaderProg[slot];	// (delete of null does nothing)
		ShaderProg[slot] = NULL;
	}
}

ShaderRecordVertex* NiD3DVertexShaderEx::GetShaderRecord(ShaderRecordType Variant) {
	return static_cast<ShaderRecordVertex*>(NiD3DShaderProgram::GetShaderRecord(Variant));
}

// The game frees one of its shaders: our records go first, and the family forgets it.
void __fastcall NiD3DVertexShaderEx::Free(NiD3DVertexShaderEx* Shader) {
	// (2026-11-03) another plugin's shader: its slots are not NVR's records, nothing of NVR's to free
	if (!ShaderOwnership::Ours(Shader)) { ThisCall(0xE89B70, Shader); return; }
	ShaderOwnership::Forget(Shader);
	Shader->DisposeShader();
	if (ShaderCollection* family = TheShaderManager->GetShaderCollection(Shader->Name)) {
		auto& members = family->VertexShaderList;
		members.erase(std::remove(members.begin(), members.end(), Shader), members.end());
	}
	ThisCall(0xE89B70, Shader);	// the game's own free
}


/*
* Hook: picks the shader record whose handle this pass uses, and makes it current.
*/
void NiD3DPixelShaderEx::SetupShader(IDirect3DPixelShader9* BoundHandle) {
	auto useGameShader = [this] { ShaderHandle = (IDirect3DPixelShader9*)ShaderHandleBackup; };

	// UNOFFICIAL 2026-10-17 grass study, diagnostic (only while the F10 GPU profile is on, a few lines a run): the device's
	// states when a grass pixel shader is set up, to know how the game draws grass (alpha test, blend, depth, culling, coverage)
	// before its shader is changed. Read only.
	if (GpuTimer::Enabled && Name && !memcmp(Name, "GRASS", 5)) {
		static unsigned binds = 0;
		if (binds < 8 || (binds % 5000 == 0 && binds <= 50000)) {
			IDirect3DDevice9* d = TheRenderManager->device;
			DWORD s[13] = {};
			const D3DRENDERSTATETYPE k[13] = { D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
				D3DRS_DESTBLEND, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_ADAPTIVETESS_Y,
				D3DRS_MULTISAMPLEANTIALIAS };
			for (int i = 0; i < 13; i++) d->GetRenderState(k[i], &s[i]);
			Logger::Log("GRASS PROBE bind %u %s: alphatest %u ref %u func %u | blend %u src %u dst %u | z %u zwrite %u zfunc %u | cull %u colorwrite 0x%X | atoc 0x%08X msaa %u",
				binds, Name, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12]);
		}
		binds++;
	}

	if (!Enabled || !TheSettingManager->SettingsMain.Main.RenderEffects) return useGameShader();

	// UNOFFICIAL 2026-10-22 MarkShaders (test only, [Shaders.PBR.Main] MarkShadersFrom/To, read each frame in UpdateConstants): the
	// pixel shaders SLS<from>..SLS<to> draw flat magenta, to see which shader draws a surface. ps_2_0, so any vertex shader fits.
	// Both 0 = off (the default): nothing below changes.
	if (MarkShadersTo > 0) {
		static int logged = 0;
		if (logged < 40) { logged++; Logger::Log("MarkShaders: pixel shader '%s' (range %d-%d)", Name ? Name : "(null)", MarkShadersFrom, MarkShadersTo); }
	}
	// MarkShadersFamily: 0 the SLS shaders, 1 the parallax (PAR) ones, 2 every other pixel shader (the range is ignored).
	const bool isSLS = Name && !memcmp(Name, "SLS", 3), isPAR = Name && !memcmp(Name, "PAR", 3);
	if (MarkShadersTo > 0 && Name && (MarkShadersFamily == 2 ? !isSLS && !isPAR : MarkShadersFamily == 1 ? isPAR : isSLS)) {
		const int number = atoi(Name + 3);
		if (MarkShadersFamily == 2 || (number >= MarkShadersFrom && number <= MarkShadersTo)) {
			static IDirect3DPixelShader9* magenta = nullptr;
			static bool tried = false;
			if (!tried) {
				tried = true;
				static const char src[] = "float4 main() : COLOR0 { return float4(1, 0, 1, 1); }";
				ID3DXBuffer* code = nullptr;
				if (SUCCEEDED(D3DXCompileShader(src, sizeof(src) - 1, NULL, NULL, "main", "ps_2_0", 0, &code, NULL, NULL)) && code) {
					TheRenderManager->device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &magenta);
					code->Release();
				}
				Logger::Log("MarkShaders: magenta shader %s", magenta ? "ready" : "FAILED");
			}
			if (MarkShadersVanilla == 1) return useGameShader();	// the game's own shader instead (with MarkShadersVsFrom/To for the vertex shaders that feed it)
			if (magenta && MarkShadersVanilla != 3) {	// 3: the vertex shaders only (the pixel shaders stay NVR's)
				ShaderHandle = magenta;
				return;
			}
		}
	}

	// UNOFFICIAL 2026-10-30 RT lamp shadows ExteriorForward: outdoors, the lit shader's lamp-shadow record while the traced lamps are live
	if (PointShadowForward::ExteriorActive && Player->GetWorldSpace()) {
		auto found = PointShadowForward::ExteriorRecords.find(this);
		if (found != PointShadowForward::ExteriorRecords.end()) {
			ShaderHandle = found->second->ShaderHandle;
			if (BoundHandle != ShaderHandle) found->second->SetCT();
			return;
		}
	}
	// UNOFFICIAL 2026-10-31 DeferredLamps: indoors, the many-lamp shader's deferred record while the main list's G-buffer is bound
	if (!Player->GetWorldSpace()) {
		if (ShaderRecordPixel* deferred = DeferredLamps::Choose(this)) {
			ShaderHandle = deferred->ShaderHandle;
			if (BoundHandle != ShaderHandle) deferred->SetCT();
			return;
		}
	}
	if (ShaderRecordPixel* record = PickRecord(this, Player->GetWorldSpace() != nullptr)) {
		ShaderHandle = record->ShaderHandle;
		if (BoundHandle != ShaderHandle) record->SetCT();
	}
	else useGameShader();
}


void NiD3DPixelShaderEx::DisposeShader() {	// all three variants
	for (UInt32 slot = 0; slot < 3; slot++) {
		delete ShaderProg[slot];	// (delete of null does nothing)
		ShaderProg[slot] = NULL;
	}
}

ShaderRecordPixel* NiD3DPixelShaderEx::GetShaderRecord(ShaderRecordType Variant) {
	return static_cast<ShaderRecordPixel*>(NiD3DShaderProgram::GetShaderRecord(Variant));
}

// As the vertex version above.
void __fastcall NiD3DPixelShaderEx::Free(NiD3DPixelShaderEx* Shader) {
	if (!ShaderOwnership::Ours(Shader)) { ThisCall(0xE89970, Shader); return; }	// (2026-11-03) as the vertex version
	ShaderOwnership::Forget(Shader);
	Shader->DisposeShader();
	if (ShaderCollection* family = TheShaderManager->GetShaderCollection(Shader->Name)) {
		auto& members = family->PixelShaderList;
		members.erase(std::remove(members.begin(), members.end(), Shader), members.end());
	}
	ThisCall(0xE89970, Shader);	// the game's own free
}

// Vertex records never clear the texture stages in SetCT; pixel records do.
ShaderRecordVertex::ShaderRecordVertex(const char* RecordName) : ShaderHandle(nullptr) { Name = RecordName; ClearSamplers = false; }
ShaderRecordPixel::ShaderRecordPixel(const char* RecordName) : ShaderHandle(nullptr) { Name = RecordName; ClearSamplers = true; }

// UNOFFICIAL 2026-10-13: no handle when the driver refused the shader (LoadShader deletes the record then): releasing
// nothing crashed the game at startup instead of falling back to the game's own shader.
ShaderRecordVertex::~ShaderRecordVertex() { if (ShaderHandle) ShaderHandle->Release(); }
ShaderRecordPixel::~ShaderRecordPixel() {
	LinearPreDecode::Unregister(ShaderHandle);	// UNOFFICIAL 2026-10-17 LinearPreDecode
	if (ShaderHandle) ShaderHandle->Release();
}

void ShaderRecordVertex::SetShaderConstantF(UInt32 FirstRegister, D3DXVECTOR4* Data, UInt32 Count) {
	TheRenderManager->device->SetVertexShaderConstantF(FirstRegister, reinterpret_cast<const float*>(Data), Count);
}
void ShaderRecordPixel::SetShaderConstantF(UInt32 FirstRegister, D3DXVECTOR4* Data, UInt32 Count) {
	TheRenderManager->device->SetPixelShaderConstantF(FirstRegister, reinterpret_cast<const float*>(Data), Count);
}
