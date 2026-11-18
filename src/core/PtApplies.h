#pragma once
// UNOFFICIAL 2026-11-12 (a tester, of path tracing: "a whole swath of shaders and settings do not appear to work ... it's a real
// crapshoot trying to investigate anything"). What each effect does while the path traced picture replaces the scene
// (RtPathTrace::Replacing), as ONE table. Its readers:
//   - the menu (ImGuiManager.cpp): a row's line at the top of the effect's pages, a short tag beside its name in the effects list, and
//     the whole table as a folding block on the Ray tracing page. [Main.Menu.Developer] PathTracingNotes (View > Path tracing notes)
//     turns these off. With path tracing off nothing of it shows.
//   - the session log (LogIfChanged, called once a second from SessionLog::OnFrame): the enabled effects by state, once when the traced
//     picture first replaces the scene and again when the set changes ("PT APPLIES" lines).
// The evidence for every row, with file and line, is docs/PT-WHAT-APPLIES.md: change the two together. Lines are plain and short. A key
// the menu shows is named "Ray tracing > Key"; a key it hides (the Internal list) is named as a settings file key.
#include "ShelvedEffects.h"	// UNOFFICIAL 2026-11-16: EachName leaves out a shelved effect (release builds only)
namespace RtPathTrace { bool Replacing(); }	// (core/RtPathTrace.h)

namespace PtApplies {
	enum State {
		Applies,	// still fully applies: drawn on top of the traced picture, or part of what the game's own pixels are
		Partly,		// only on the pixels the path tracer leaves to the game (far land, distant objects, grass, leaves, water)
		Replaced,	// the path tracer does this itself; these settings do nothing on traced surfaces
		Off,		// switched off on purpose by a key while the traced picture replaces the scene
		Feeds,		// the path tracer reads its output: must stay on
		Missing,	// lost on traced surfaces (nothing replaces it)
		Twice,		// still drawn on top of a traced picture that already has it: may count twice
		StateCount
	};
	struct Row {
		const char* Name;		// an effect's entry in the Shaders list ("Bloom"), a settings section ("Shaders.Reflections.RT"), or "" (a
								// group that is neither: the overview only)
		const char* Label;		// its plain name where Name is no effect entry (nullptr: Name)
		State St;
		const char* Line;
		const char* IfSection = nullptr;	// with IfKey: St / Line hold while that key is on; while it is off, Else / ElseLine
		const char* IfKey = nullptr;
		State Else = Applies;
		const char* ElseLine = nullptr;
	};

	inline const char* const kRT = "Main.Main.RayTracing";
	inline const char* const kTwiceShadows = "Still drawn on top of the path traced picture, which has its own shadows: they may count twice "
		"(the settings file key PTOwnShadows is off).";

	inline const Row kRows[] = {
		// Lighting
		{ "AmbientLight", nullptr, Replaced, "Replaced by the path tracer's own sky and bounce light. See Ray tracing > PTSky, PTAmbient, PTBounces." },
		{ "AmbientOcclusion", nullptr, Replaced, "Replaced: the path tracer shades corners and creases itself." },
		{ "Specular", nullptr, Replaced, "Replaced by the path tracer's own highlights. See Ray tracing > PTSpecular." },
		{ "BounceLight", nullptr, Twice, "Still drawn on top of the path traced picture, which has its own bounce light: light may count twice. Best left off." },
		{ "PBR", nullptr, Feeds, "Keep it on: objects drawn without it stay the game's own picture. Roughness, metal, LightingScale and Gamma feed the "
			"path tracer. Its other lighting settings do nothing on traced surfaces. Bumps show as fine detail only (Ray tracing > PTDetail)." },
		{ "Reflections", nullptr, Off, "Not drawn while path tracing: the path tracer has its own reflections (settings file key PTReflectionsOff).",
			kRT, "PTReflectionsOff", Twice, "Still drawn on top of the path traced picture, which has its own reflections: surfaces may reflect "
			"twice (the settings file key PTReflectionsOff is off)." },
		{ "Flashlight", nullptr, Replaced, "The beam is one of the path tracer's own lamps: its reach, cone, colour and brightness count, and it fades "
			"with distance as the path tracer's lamps do (Falloff only with PTFlashRasterShape). What stays the game's own picture is lit by the beam's own "
			"pass where the mod recorded it, else by the path tracer (PTFlashOwnPixels)." },
		// Shadows
		{ "ShadowsExteriors", nullptr, Partly, "Traced surfaces have the path tracer's own sun shadows. These settings only shade what stays the "
			"game's picture: far land, grass, leaves. Cloud and far-hill shadows still darken everything.", kRT, "PTOwnShadows", Twice, kTwiceShadows },
		{ "ShadowsInteriors", nullptr, Partly, "Traced surfaces have the path tracer's own lamp shadows. These settings only shade what stays the "
			"game's picture.", kRT, "PTOwnShadows", Twice, kTwiceShadows },
		{ "ContactHardening", nullptr, Partly, "Only softens the game's own sun shadows, which now show on far land, grass and leaves only." },
		// The ray-traced effects (their pages on the Ray tracing page)
		{ "Shaders.ShadowsExteriors.RT", "RT sun shadows", Partly, "Still traced, but shown only on what stays the game's picture. Traced surfaces "
			"have the path tracer's own sun shadows.", kRT, "PTOwnShadows", Twice, kTwiceShadows },
		{ "Shaders.ShadowsInteriors.RT", "RT lamp shadows", Off, "Not traced while path tracing: the path tracer has its own lamp shadows "
			"(Ray tracing > PTStandDownRT).", kRT, "PTStandDownRT", Partly, "Still traced beside the path tracer's own lamp shadows, and it "
			"costs time (Ray tracing > PTStandDownRT is off)." },
		{ "Shaders.AmbientOcclusion.RT", "RT occlusion and bounce light", Replaced, "Not shown: Ambient occlusion and Ambient light step aside for "
			"the path tracer. It may still trace and cost time: turn it off for speed." },
		{ "Shaders.Reflections.RT", "RT reflections", Off, "Not traced while path tracing: the path tracer has its own reflections "
			"(Ray tracing > PTStandDownRT).", kRT, "PTStandDownRT", Partly, "Still traced (Ray tracing > PTStandDownRT is off), and shown "
			"only if the Reflections effect is still drawn." },
		// Sky, weather, water
		{ "Sky", nullptr, Applies, "The sky stays the game's own picture: these settings apply. The weather's sky and sun colours also light the "
			"traced scene (Ray tracing > PTSky)." },
		{ "WetnessEffects", nullptr, Partly, "Wet ground still darkens. The glossy water film and the puddles' shine do not reach the path tracer: "
			"they show only on what stays the game's picture." },
		{ "Water", nullptr, Partly, "Water stays the game's own picture, lit by the game and not by the path tracer.", kRT, "PTWaterMask", Partly,
			"The path tracer has no water of its own: water can show as the ground under it (settings file key PTWaterMask is off)." },
		// Ground and plants
		{ "Terrain", nullptr, Feeds, "Keep it on: land drawn without it stays the game's own picture. Near land is path traced: its texture settings "
			"count, its lighting settings do not. Far land stays the game's picture and keeps all of them." },
		{ "POM", nullptr, Feeds, "Keep it on for the objects it draws. The parallax shift still shows. Its lighting and self-shadow are replaced." },
		{ "Grass", nullptr, Feeds, "Grass is path traced (settings file key CapturedGrass): its colour comes from these settings, its light from "
			"the path tracer.", "Main.Main.Quality", "CapturedGrass", Partly, "Grass stays the game's own picture, lit by the game: these settings "
			"apply to it." },
		{ "Foliage", nullptr, Partly, "Only on leaves and grass that stay the game's own picture. Path traced leaves use Ray tracing > PTFoliage." },
		// Characters
		{ "Skin", nullptr, Feeds, "Keep it on, with Scattering > ScreenSpace: the path tracer takes the skin's colour from it. Skin lighting is "
			"replaced. See Ray tracing > PTSkin." },
		// Particles
		{ "Particles", nullptr, Applies, "Shown as the game draws them, carried over onto the path traced picture (settings file key PTBlendedLayer).",
			kRT, "PTBlendedLayer", Missing, "Fire, smoke, dust and other see-through effects are missing in front of traced surfaces. The "
			"settings file key PTBlendedLayer = 1 brings them back." },
		// Groups that are no effect of their own (the overview on the Ray tracing page)
		{ "", "Sun and lamp light, and their shadows", Replaced, "Path traced, from the game's own sun and lamps. See Ray tracing > PTLampFalloff." },
		{ "", "The weather's and the room's ambient light", Replaced, "Ray tracing > PTSky outdoors, PTAmbient indoors (0: rooms are lit by their "
			"lamps and bounces only)." },
		{ "", "Far land and distant objects", Partly, "Stay the game's own picture with the game's lighting: path tracing ends where the detailed "
			"world ends." },
		{ "", "Tree leaves", Partly, "Mostly the game's own picture. Path traced only where the game draws them with its solid world." },
		{ "", "Glowing signs, screens and lamp bulbs", Partly, "Shown as the game draws them. They also light the traced scene." },
		{ "", "First-person arms and gun", Replaced, "Lit by the path tracer (Ray tracing > PTViewModel). Not with the game's MSAA or a RenderScale "
			"below 1: then they keep the game's lighting.", kRT, "PTViewModel", Partly, "Keep the game's own lighting (Ray tracing > PTViewModel is off)." },
	};

	// Effects drawn after the traced picture went into the scene, with nothing that tests for it: they work as always.
	inline const char* const kOnTop = "VolumetricClouds, VolumetricFog, VolumetricLight, GodRays, Sandstorm, HeatHaze, DustMotes, CloudShadows, SunFar, "
		"Precipitations, Snow, SnowAccumulation, WetWorld, HudRaindrops, Underwater, WaterLens, Plants, Cloth, PipboyFit, BloodFlow, Tonemapping, "
		"Exposure, LocalExposure, HDR, Bloom, BloomLegacy, Lens, LUT, Coloring, ImageAdjust, Cinema, Bodycam, DepthOfField, MotionBlur, LowHF, "
		"BloodLens, TAA, SMAA, Sharpening, DitherBuster, SkyDither, GPUParticles, Dream, Debug";
	inline const Row kOnTopRow = { "", nullptr, Applies, "Still applies: it is drawn on top of the path traced picture." };

	inline const char* const kTags[StateCount] = { "", "PT: partly", "PT: replaced", "PT: off", "PT: feeds", "PT: missing", "PT: twice" };
	inline const char* const kHeads[StateCount] = { "Still apply (drawn on top of the path traced picture)",
		"Only on what stays the game's own picture (far land, distant objects, grass, leaves, water)", "Replaced by the path tracer",
		"Switched off while path tracing", "Feed the path tracer (keep them on)", "Missing on traced surfaces",
		"Still drawn on top, and may count twice" };

	// Whether a "A, B, C" list names it (whole names only; nothing is copied: the menu asks for every row it draws).
	inline bool InList(const char* list, const std::string& name) {
		const size_t n = name.size();
		for (const char* p = strstr(list, name.c_str()); p; p = strstr(p + 1, name.c_str()))
			if ((p == list || p[-1] == ' ') && (p[n] == ',' || p[n] == 0)) return true;
		return false;
	}

	// Calls each(name) for every name of a "A, B, C" list.
	template <typename F> inline void EachName(const char* list, F each) {
		for (const char* p = list; *p;) {
			while (*p == ' ' || *p == ',') p++;
			const char* end = p;
			while (*end && *end != ',') end++;
			if (end > p && !ShelvedEffects::Name(std::string(p, end).c_str())) each(std::string(p, end));	// UNOFFICIAL 2026-11-16: not a shelved effect (release builds only)
			p = end;
		}
	}

	// The row of an effect's entry ("Bloom") or of a settings section ("Shaders.Reflections.RT": its own row, else its effect's); nullptr: none.
	inline const Row* Find(const std::string& name) {
		if (name.empty()) return nullptr;
		for (const Row& r : kRows) if (name == r.Name) return &r;
		if (name.rfind("Shaders.", 0) == 0) {
			const size_t dot = name.find('.', 8);
			return Find(name.substr(8, dot == std::string::npos ? std::string::npos : dot - 8));
		}
		return InList(kOnTop, name) ? &kOnTopRow : nullptr;
	}

	struct Now { State St; const char* Line; bool KeyOff; };
	inline Now Resolve(const Row& r) {
		if (r.IfKey && TheSettingManager->GetSettingI(r.IfSection, r.IfKey) == 0) return { r.Else, r.ElseLine, true };
		return { r.St, r.Line, false };
	}

	// Once a second (SessionLog::OnFrame). While the traced picture replaces the scene: the enabled effects by state, as "PT APPLIES"
	// lines, the first time and whenever the set changes (at most 8 blocks a session, 10 lines each).
	inline void LogIfChanged() {
		static std::string last;
		static int blocks = 0;
		if (blocks >= 8 || !RtPathTrace::Replacing()) return;
		std::string by[StateCount], neededOff;
		auto add = [](std::string& list, const std::string& item) { if (!list.empty()) list += ", "; list += item; };
		for (const Row& r : kRows) {
			if (!r.Name[0]) continue;
			const bool on = strchr(r.Name, '.') ? TheSettingManager->GetSettingI(r.Name, "Enabled") != 0 :
				TheSettingManager->GetMenuShaderSwitch(r.Name) != SettingManager::MenuSwitch::None && TheSettingManager->GetMenuShaderEnabled(r.Name);
			const Now now = Resolve(r);
			const std::string name = r.Label ? r.Label : r.Name;
			if (!on) { if (now.St == Feeds) add(neededOff, name); continue; }
			add(by[now.St], r.IfKey ? name + " (" + r.IfKey + (now.KeyOff ? " off)" : " on)") : name);
		}
		EachName(kOnTop, [&](const std::string& n) {
			if (TheSettingManager->GetMenuShaderSwitch(n.c_str()) != SettingManager::MenuSwitch::None && TheSettingManager->GetMenuShaderEnabled(n.c_str())) add(by[Applies], n);
		});
		static const char* const kLogHeads[StateCount] = { "drawn on top as always", "only on the game's own pixels (far land, distant objects, grass, leaves, water)",
			"replaced by the path tracer (their settings do nothing on traced surfaces)", "switched off by a key", "feed the path tracer",
			"MISSING on traced surfaces", "drawn on top and may count TWICE" };
		static const State kOrder[StateCount] = { Replaced, Off, Partly, Missing, Twice, Feeds, Applies };
		std::string text;
		for (const State s : kOrder) if (!by[s].empty()) text += std::string("PT APPLIES ") + kLogHeads[s] + ": " + by[s] + "\n";
		if (!neededOff.empty()) text += "PT APPLIES NEEDED BUT OFF (what they draw stays the game's own picture): " + neededOff + "\n";
		char keys[400];
		sprintf_s(keys, "PT APPLIES keys: PTStandDownRT %d, PTReflectionsOff %d, PTOwnShadows %d, PTBlendedLayer %d, PTWaterMask %d, PTViewModel %d, "
			"CapturedGrass %d, CapturedLeaves %d, PTDetail %.2f, PTSky %.2f, PTAmbient %.2f\n",
			TheSettingManager->GetSettingI(kRT, "PTStandDownRT"), TheSettingManager->GetSettingI(kRT, "PTReflectionsOff"),
			TheSettingManager->GetSettingI(kRT, "PTOwnShadows"), TheSettingManager->GetSettingI(kRT, "PTBlendedLayer"),
			TheSettingManager->GetSettingI(kRT, "PTWaterMask"), TheSettingManager->GetSettingI(kRT, "PTViewModel"),
			TheSettingManager->GetSettingI("Main.Main.Quality", "CapturedGrass"), TheSettingManager->GetSettingI("Main.Main.Quality", "CapturedLeaves"),
			TheSettingManager->GetSettingF(kRT, "PTDetail"), TheSettingManager->GetSettingF(kRT, "PTSky"), TheSettingManager->GetSettingF(kRT, "PTAmbient"));
		text += keys;
		if (text == last) return;
		last = text;
		blocks++;
		Logger::Log("PT APPLIES: the path traced picture replaces the scene. What the enabled effects do meanwhile (block %d of at most 8; docs/PT-WHAT-APPLIES.md):", blocks);
		for (size_t from = 0, end; from < text.size() && (end = text.find('\n', from)) != std::string::npos; from = end + 1)
			Logger::Log("%s", text.substr(from, end - from).c_str());
	}
}
