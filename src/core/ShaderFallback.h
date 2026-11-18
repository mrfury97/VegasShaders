#pragma once

#include <string>
#include <vector>
#include "TestTools.h"	// NVR_TEST_TOOLS (ShaderSlotCapOverride)

// UNOFFICIAL 2026-11-12 ShaderFallback ([Main.Main.Misc] ShaderFallback, default on; ShaderRecord.cpp, ShaderManager.cpp).
//
// WHY. Native Direct3D 9 takes a shader model 3 shader only up to the driver's instruction slot cap (D3DCAPS9
// MaxPixelShader30InstructionSlots; 4096 on the GTX 1070 and on every NVIDIA / AMD driver seen so far; DXVK has no such limit).
// A tester's settings (Wetness Effects + Terrain Blending on) made 19 of the 28 land pixel shaders longer than that (4140 to 4971
// slots). CreatePixelShader refused them, and the mod left those land pieces to the GAME's own shader, which knows none of the
// [Shaders.Terrain.*] settings: ground that reacted to the menu next to ground that did not, with one "ERROR:" line per shader that
// no tool counted. Reproduced without the game on the same GPU (d3dx9_43's compiler and a bare Direct3D 9 device from a script: the
// same 19 refused with the same hr 0x8876086C; with Wetness Effects left out all 28 are accepted, the longest 3598 slots).
//
// WHAT. When a shader of a template family is refused (or counted over the cap), an optional feature is left out of the WHOLE
// family, in a fixed order, and the family is built again, until it is accepted. Only when nothing is left to drop does the game's
// own shader draw, as before, with an [ERROR] line and a notice in the menu.
//
// THE COUNT. The runtime counts slots by the documented ps_3_0 / vs_3_0 table (most instructions 1, lrp 2, nrm / pow 3, texldl 2,
// texldd 3, texkill 2, if / rep 3, sincos 8 ...). Measured on the real runtime with hand-made bytecode: 4096 slots accepted, 4097
// refused, for every instruction the land shaders use. D3DX's disassembly ("approximately N instruction slots") counts lrp and
// texkill as 1, so it reads up to 67 too low on the land: SLS2140 "4062 slots" is 4098 and was refused. CountSlots is the runtime's
// count; an actual refusal still wins over it.
//
// THE ORDER (slots saved on the 6-layer / 7-layer land shader with the tester's settings, runtime count; most saved per visible
// loss first):
//   1. Wetness Effects      -1422 / -676   visible only in and after rain, and Wet World's screen pass draws the wetness instead
//   2. Terrain Macro         -160 / -162   a subtle tint over hundreds of metres
//   3. Terrain Variation     -191 / -193   (2 layers; up to -439 at 7) the ground textures repeat again
//   4. Terrain Blending      -533 / -508   objects meet the ground in a hard line again
//   5. Contact Hardening     -492 / -493   on the ground only: its sun shadows keep one edge softness (objects keep theirs)
// Nothing else on the land is worth a drop: CharacterShadowMap, SpecularFade, SmoothObjectNormals, LandAllLamps, SmithCorrelated 0
// slots, ForwardCloudShadows 4, CapturedNormals 27, linear lighting 77 (a look change everywhere). Objects / parallax objects:
// Wetness Effects, then Micro Shadow (a short generic list: no refusal of theirs is known that the lamp-shadow FIT levels do not
// already handle). Skin: nothing optional, so a refused skin shader is the game's, as before, but said loudly.
//
// CONSISTENCY. A feature is dropped for every template of the family (the land: TerrainTemplate, TerrainLODTemplate and
// TerrainFadeTemplate, so near and far ground agree), and the members made before the drop are loaded again. Wetness Effects is
// dropped for EVERY family and for the session: with it on, Wet World's screen pass does not draw (EffectChain.cpp), so land
// without it next to objects with it would be dry ground under wet rocks. Its collection is switched off instead
// (WetnessEffectsShaders::UpdateConstants: TESR_WetnessFlags.x 0, so shaders already built with it skip it, and the chain's
// existing "Shaders.Wetness->Enabled" test lets Wet World draw). Terrain Blending's own switch follows too (Terrain.cpp).
//
// MEMORY. The drops are written to <shaders>\Cache\ShaderFallback.txt with a key made of the settings' whole define list, the
// device caps and this build. The next start reads it before the first shader is built, so nothing is compiled, refused and
// compiled again (that cost a serial compile of the family, minutes). Another setting, another driver cap, DXVK, a new build, or
// deleting the Cache folder: the full set is tried again.
namespace ShaderFallback {

	enum Family : unsigned { Other = 0, Land, Objects, Skin, FamilyCount };
	enum Feature : unsigned { Wetness = 0, TerrainMacro, TerrainVariation, TerrainBlend, LandContactHardening, MicroShadow, FeatureCount };

	struct FeatureInfo {
		const char* Section;	// the setting that asks for the feature (on at startup = compiled in)
		const char* Key;
		bool Everywhere;		// dropped for every family, not only the one that did not fit (see CONSISTENCY)
		const char* Name;		// what was left out, for the log and the menu
		const char* Option;		// the option to turn off to make room for the others
		const char* Loss;		// what is different without it
	};
	inline const FeatureInfo Features[FeatureCount] = {
		{ "Shaders.WetnessEffects.Status", "Enabled", true, "Wetness Effects", "Wetness Effects", "off on every surface for this session; Wet World's screen pass draws the rain wetness instead" },
		{ "Shaders.Terrain.Macro", "Enabled", false, "Terrain Macro", "Terrain Macro", "no large-scale colour variation on the ground" },
		{ "Shaders.Terrain.Variation", "Enabled", false, "Terrain Variation", "Terrain Variation", "the ground textures repeat as they do without it" },
		{ "Shaders.Terrain.Blending", "Enabled", false, "Terrain Blending", "Terrain Blending", "objects meet the ground in a hard line" },
		{ "Shaders.ContactHardening.Status", "Enabled", false, "Contact Hardening on the ground", "Contact Hardening", "sun shadows on the ground keep one edge softness; objects keep theirs" },
		{ "Shaders.PBR.MicroShadow", "Enabled", false, "Micro Shadow", "Micro Shadow", "bumps on objects do not shadow themselves" },
	};
	inline const char* const FamilyNames[FamilyCount] = { "other", "ground", "object", "skin" };

	// The removal order of a family (see THE ORDER); count 0: nothing optional.
	inline const Feature* Order(Family family, int& count) {
		static const Feature land[] = { Wetness, TerrainMacro, TerrainVariation, TerrainBlend, LandContactHardening };
		static const Feature objects[] = { Wetness, MicroShadow };
		if (family == Land) { count = 5; return land; }
		if (family == Objects) { count = 2; return objects; }
		count = 0;
		return nullptr;
	}

	// The family of a shader template (ShaderTemplate::Name; null = a hand-written shader).
	inline Family FamilyOf(const char* templateName) {
		if (!templateName) return Other;
		if (!strncmp(templateName, "Terrain", 7)) return Land;	// TerrainTemplate, TerrainLODTemplate, TerrainFadeTemplate
		if (!strncmp(templateName, "Object", 6) || !strncmp(templateName, "Parallax", 8) || !strncmp(templateName, "Env", 3)) return Objects;
		if (!strncmp(templateName, "Skin", 4)) return Skin;	// SkinTemplate, SkinVPSTemplate
		return Other;
	}

	inline int State = -1;					// -1 not read yet, 0 off ([Main.Main.Misc] ShaderFallback false: as before 2026-11-12), 1 on
	inline unsigned Own[FamilyCount] = {};	// bit per Feature: dropped because THIS family did not fit (what the memory file holds)
	inline unsigned Mask[FamilyCount] = {};	// bit per Feature: left out of that family's shaders (Own, plus every family's Everywhere drops)
	inline bool IgnoreDrops = false;		// while the memory key is made: the define list as the settings alone give it
	inline unsigned Reload = 0;				// bit per Family: its members made before a drop are loaded again (ShaderManager.cpp)
	inline unsigned CapPixel = 0, CapVertex = 0;	// the device's caps (0: unknown)
	inline unsigned Forced = 0;				// [Main.Develop.Main] ShaderSlotCapOverride (development builds; 0 off)
	inline bool Native = true;				// not DXVK
	inline bool Remembered = false;			// the drops came from the memory file
	inline unsigned Key = 0;
	inline std::string File;				// <shaders>\Cache\ShaderFallback.txt
	inline unsigned Refusals = 0;			// shaders the device refused (or counted over a forced cap) this session, recovered or not
	inline unsigned Lost[FamilyCount] = {};	// shaders refused with nothing left to drop: the game's own shader draws them
	inline unsigned Longest[FamilyCount] = {};	// the longest refused shader of the family, in slots
	// The menu's notice (ImGuiManager.cpp, with the install check's): empty title = nothing to say for that family.
	inline std::string NoticeTitle[FamilyCount], NoticeFix[FamilyCount];

	inline bool On() { return State == 1; }
	// The one place that says whether a feature the settings ask for is really in a family's shaders.
	inline bool Dropped(Feature feature, Family family) { return !IgnoreDrops && ((Mask[family] >> feature) & 1u) != 0; }
	inline bool Wanted(Feature feature) { return TheSettingManager->GetSettingI(Features[feature].Section, Features[feature].Key) != 0; }
	// The cap a shader is held to: the device's, or the forced one when that is lower (0: no cap known).
	inline unsigned Cap(bool vertex) {
		const unsigned device = vertex ? CapVertex : CapPixel;
		return Forced && (!device || Forced < device) ? Forced : device;
	}
	inline bool OverForcedCap(unsigned slots, bool vertex) { const unsigned device = vertex ? CapVertex : CapPixel; return Forced && slots > Forced && (!device || Forced < device); }

	// A shader's instruction slots as the Direct3D 9 runtime counts them (see THE COUNT). tokens: the bytecode's length in DWORDs.
	inline unsigned CountSlots(const DWORD* code, unsigned tokens) {
		static const unsigned char kSlots[97] = {
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	// nop mov add sub mad mul rcp rsq dp3 dp4 min max slt sge exp log
			3, 1, 2, 1, 4, 3, 4, 3, 2, 2, 3, 3, 1, 2, 0, 0,	// lit dst lrp frc m4x4 m4x3 m3x4 m3x3 m3x2 call callnz loop ret endloop label dcl
			3, 2, 3, 1, 3, 8, 3, 2, 3, 3, 1, 1, 1, 3, 1, 0,	// pow crs sgn abs nrm sincos rep endrep if ifc else endif break breakc mova defb
			0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	// defi
			1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	// texcoord texkill texld
			1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 1, 2,	// cnd def ... cmp bem dp2add dsx dsy texldd setp texldl
			3 };											// breakp
		if (!code || tokens < 2) return 0;
		unsigned total = 0;
		for (unsigned at = 1; at < tokens; ) {
			const DWORD token = code[at];
			const unsigned op = token & 0xFFFF;
			if (op == 0xFFFF) break;	// end
			if (op == 0xFFFE) { at += 1 + ((token >> 16) & 0x7FFF); continue; }	// a comment (the constant table)
			unsigned slots = op < 97 ? kSlots[op] : 1;
			if (op == 66) { const unsigned control = (token >> 16) & 0xFF; slots = control == 1 ? 3 : control == 2 ? 6 : 1; }	// texld / texldp / texldb
			total += slots;
			at += 1 + ((token >> 24) & 0xF);
		}
		return total;
	}

	// Mask from Own: a family's own drops, and every family's Everywhere drops.
	inline void Rebuild() {
		unsigned everywhere = 0;
		for (unsigned family = 0; family < FamilyCount; family++)
			for (unsigned feature = 0; feature < FeatureCount; feature++)
				if (((Own[family] >> feature) & 1u) && Features[feature].Everywhere) everywhere |= 1u << feature;
		for (unsigned family = 0; family < FamilyCount; family++) Mask[family] = Own[family] | everywhere;
	}

	inline void Save() {
		if (File.empty()) return;
		FILE* out = nullptr;
		if (fopen_s(&out, File.c_str(), "wb") || !out) return;
		fprintf(out, "key %08X\r\n", Key);
		for (unsigned family = 0; family < FamilyCount; family++) fprintf(out, "%s %X\r\n", FamilyNames[family], Own[family]);
		fprintf(out, "# Vegas Shaders: the optional shader parts left out so the shaders fit this graphics driver (ShaderFallback.h). Safe to delete: the full set is tried again.\r\n");
		fclose(out);
	}

	// What is left out of a family, and what to do about it, as a sentence pair for the log and the menu. counts: with the number
	// of shaders still refused (the menu; the log's line is written when the first one is lost, so it says "Some").
	inline void Describe(Family family, bool counts, std::string& title, std::string& fix) {
		title.clear();
		fix.clear();
		std::string names, losses, others;
		int count = 0;
		const Feature* order = Order(family, count);
		for (int i = 0; i < count; i++) {
			const FeatureInfo& info = Features[order[i]];
			if ((Own[family] >> order[i]) & 1u) {
				names += (names.empty() ? "" : ", ") + std::string(info.Name);
				losses += (losses.empty() ? "" : " ") + std::string(info.Name) + ": " + info.Loss + ".";
			}
			else if (!((Mask[family] >> order[i]) & 1u) && Wanted(order[i])) others += (others.empty() ? "" : ", ") + std::string(info.Option);
		}
		if (names.empty() && !Lost[family]) return;	// nothing to say
		char head[400], longest[48] = "";
		if (Longest[family]) sprintf_s(longest, "; the longest was %u", Longest[family]);	// (none refused this session when the drops were remembered)
		sprintf_s(head, "The %s shaders are too long for this graphics driver%s (cap %u instruction slots%s%s%s) with these settings",
			FamilyNames[family], Native ? " on native Direct3D 9" : "", Cap(false), Forced ? ", forced by ShaderSlotCapOverride" : "",
			Remembered ? ", remembered from an earlier start" : "", longest);
		title = head;
		if (!names.empty()) title += ": " + names + (names.find(',') == std::string::npos ? " was" : " were") + " left out so they load.";
		else title += ".";
		if (Lost[family]) {
			char lost[260], number[16];
			sprintf_s(number, "%u", Lost[family]);
			sprintf_s(lost, " %s of them %s still too long%s: the game's own shaders draw those pieces, and the %s settings do nothing there.",
				counts ? number : "Some", counts && Lost[family] == 1 ? "is" : "are",
				On() ? " with every optional part left out" : " ([Main.Main.Misc] ShaderFallback is off)", FamilyNames[family]);
			title += lost;
		}
		fix = losses;
		fix += std::string(fix.empty() ? "" : " ") + "To keep everything, use the Vegas Shaders DXVK build (no such limit there)";
		if (!others.empty()) fix += ", or make room by turning off other options that lengthen these shaders: " + others;
		fix += ".";
	}

	// The family's summary: the menu's notice always, and one log line when what is left out changes or the first shader is lost.
	inline void Summarize(Family family) {
		static unsigned loggedOwn[FamilyCount] = {};
		static bool loggedLost[FamilyCount] = {};
		Describe(family, true, NoticeTitle[family], NoticeFix[family]);
		if (NoticeTitle[family].empty()) return;
		const bool lost = Lost[family] != 0;
		if (loggedOwn[family] == Own[family] && loggedLost[family] == lost) return;
		loggedOwn[family] = Own[family];
		loggedLost[family] = lost;
		std::string title, fix;
		Describe(family, false, title, fix);
		Logger::Log("%s ShaderFallback: %s %s", lost ? "[ERROR]" : "[WARNING]", title.c_str(), fix.c_str());
		std::string left;
		for (unsigned feature = 0; feature < FeatureCount; feature++)
			if ((Own[family] >> feature) & 1u) left += (left.empty() ? "" : ", ") + std::string(Features[feature].Name);
		static const char* const what[FamilyCount] = { "other shaders (ShaderFallback)", "ground shaders (ShaderFallback)", "object shaders (ShaderFallback)", "skin shaders (ShaderFallback)" };
		std::string chosen = left.empty() ? std::string("nothing left out") : "left out: " + left;
		if (lost) chosen += "; some still refused (the game's own shader draws them)";
		EffectStatus::Level(what[family], chosen.c_str(), false);	// (the REPORT / FLASH dumps' "not at the default" list)
	}

	// Leaves the family's next optional feature out. False: off, or nothing left to drop.
	inline bool DropNext(Family family) {
		if (!On()) return false;
		int count = 0;
		const Feature* order = Order(family, count);
		for (int i = 0; i < count; i++) {
			const Feature feature = order[i];
			if (((Mask[family] >> feature) & 1u) || !Wanted(feature)) continue;	// (in Mask: already out, here or through another family)
			Own[family] |= 1u << feature;
			Rebuild();
			Reload |= 1u << family;
			Remembered = false;
			Logger::Log("ShaderFallback: the %s shaders are built again without %s (%s)", FamilyNames[family], Features[feature].Name, Features[feature].Loss);
			Save();
			return true;
		}
		return false;
	}

	// A shader the device refused (or that counts over a forced cap). True: a feature was dropped for its family, build it again.
	// False: nothing left (or ShaderFallback off): the game's own shader draws it.
	inline bool OnRefused(const char* templateName, unsigned slots) {
		const Family family = FamilyOf(templateName);
		Refusals++;
		if (slots > Longest[family]) Longest[family] = slots;
		if (DropNext(family)) return true;
		Lost[family]++;
		Summarize(family);
		return false;
	}

	// Once, before the first shader is built (ShaderRecord.cpp FallbackInit): the keys, the caps, the memory.
	inline void Init(unsigned defineKey, const std::string& cacheFolder) {
		if (State >= 0) return;
		SettingManager::Configuration::ConfigNode node;
		State = (!TheSettingManager->Config.FillNode(&node, "Main.Main.Misc", "ShaderFallback") || node.BoolValue) ? 1 : 0;	// a missing key: on
		const D3DCAPS9* caps = TheRenderManager ? TheRenderManager->DeviceCaps() : nullptr;
		CapPixel = caps ? caps->MaxPixelShader30InstructionSlots : 0;
		CapVertex = caps ? caps->MaxVertexShader30InstructionSlots : 0;
		Native = !(TheRenderManager && !strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4));
#if NVR_TEST_TOOLS
		// UNOFFICIAL 2026-11-12 ShaderSlotCapOverride (test only): a lower cap than the device's, held by CountSlots, so the bench can run
		// the fallback on any setup (DXVK too). 0 or a missing key: off.
		Forced = (unsigned)(std::max)(0, TheSettingManager->GetSettingI("Main.Develop.Main", "ShaderSlotCapOverride"));
#endif
		Key = defineKey ^ (CapPixel * 2654435761u) ^ (CapVertex * 40503u) ^ (Forced * 668265263u) ^ (Native ? 0u : 0x9E3779B9u);
		File = cacheFolder + "ShaderFallback.txt";
		const std::string forced = Forced ? ", forced to " + std::to_string(Forced) + " by ShaderSlotCapOverride" : std::string();
		Logger::Log("SESSION shader limits: pixel shaders %u instruction slots, vertex shaders %u (%s)%s | ShaderFallback %s",
			CapPixel, CapVertex, Native ? "native Direct3D 9: a longer shader is refused" : "DXVK reports these; it has no real limit",
			forced.c_str(), On() ? "on" : "OFF (a refused shader is left to the game's own)");
		if (!On()) return;
		// The memory: only when it was written for exactly these settings, caps and build.
		FILE* in = nullptr;
		if (fopen_s(&in, File.c_str(), "rb") || !in) return;
		char line[256];
		unsigned key = 0, masks[FamilyCount] = {};
		bool keyed = false;
		while (fgets(line, sizeof(line), in)) {
			char word[32] = {};
			unsigned value = 0;
			if (sscanf_s(line, "%31s %X", word, (unsigned)sizeof(word), &value) != 2) continue;
			if (!strcmp(word, "key")) { key = value; keyed = true; continue; }
			for (unsigned family = 0; family < FamilyCount; family++)
				if (!strcmp(word, FamilyNames[family])) masks[family] = value & ((1u << FeatureCount) - 1u);
		}
		fclose(in);
		if (!keyed || key != Key) {
			Logger::Log("ShaderFallback: the remembered drops are for other settings, another driver cap or another build (%08X, now %08X): the full shaders are tried again", key, Key);
			return;
		}
		bool any = false;
		for (unsigned family = 0; family < FamilyCount; family++) { Own[family] = masks[family]; any = any || masks[family] != 0; }
		Rebuild();
		Remembered = any;
		for (unsigned family = 0; family < FamilyCount; family++) if (Own[family]) Summarize((Family)family);
	}
}
