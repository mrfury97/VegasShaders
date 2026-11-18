#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cctype>

// UNOFFICIAL 2026-10-20 (interior look): which lamps sit at a visible lamp, for [Shaders.ShadowsInteriors.Forward]
// InvisibleLightShadowStrength.
//
// New Vegas lights its interiors with invisible lights: LIGH forms without a model, placed by the level designers for mood
// ("OfficeLightKeyNeutral", "BasementLightFillCool", "...Ambient", "...Kicker") wherever they wanted light, not at the lamps
// the player sees. FalloutNV.esm (bench\deep-roadmap\evidence\intlook\esm_lamps.py): Doc Mitchell's house has 25 lights, all
// without a model, and only 1 within 32 units of a lamp mesh (its wall sconces' nearest lights stand 100-160 units out in the
// room); the Prospector Saloon 24, 6 at a lamp. The game never gave lamps shadows, so where they stand did not matter; NVR's
// lamp shadows cast from those points: a sconce lit from the middle of the room throws its whole outline on the wall behind
// it, fill lights under a ceiling fan throw the blades onto the ceiling, and shadows fall from places where nothing glows.
//
// A light counts as AT A LAMP when a lamp part (LampPart: a small glowing mesh -- lamp glass, bulbs, glow sprites, flames) of
// a placed object of the cell lies within FixtureDistance of it; every other light is an invisible light. Many lamp models carry
// their own lights (each chandelier globe, each sconce bulb); those sit at their glass and always count.
// [Shaders.ShadowsInteriors.Forward] LampGlassShadows false also leaves lamp parts out of every lamp's shadow map: glowing glass
// is the light source, yet drawn as a solid blocker it threw a lamp's whole shade outline onto the wall (ShadowManager).
// Carried lights (torches, the Pip-Boy light) always count as at a lamp. The cell's glowing meshes are listed again whenever
// the cell changes and every few seconds after (objects load their 3D after the cell itself); a light keeps its answer until
// that list changes.
namespace LampFixtures {

	struct Glow { float x, y, z, radius, emit; const char* name; const char* root; int model; };	// name/root: LogLamps only (engine strings);
																								// model: its lamp model (Models), -1 none

	inline std::vector<Glow> Glows;							// the cell's glowing meshes (world bound centre and radius)
	inline const void* GlowCell = nullptr;					// the cell they were listed for
	inline double GlowListed = -1.0e9, CellEntered = 0.0;	// when (seconds, QueryPerformanceCounter)
	inline unsigned Generation = 0;							// bumped whenever the list changes
	inline bool Ready = false;								// the last listing saw placed objects with 3D (else: no answer yet)
	inline UInt64 GlowHash = 0;

	// UNOFFICIAL LampModelShadows (2026-10-20): the cell's lamp MODELS -- sconces, chandeliers, ceiling fans, hanging lamps, table lamps
	// (whole models: body, arms, blades, shade frame, glass) -- and which model each of their meshes belongs to. A light leaves the
	// lamp models within LampModelDistance of it out of its own shadow map (ShadowManager::RenderShadowCubeMap): the level designers
	// put their invisible lights right next to the lamps (Doc Mitchell's sconces' lights 103-162 units away, the fan's fills
	// 113-137 under it, the saloon's 10-75 from its chandeliers and arm lamps), and from there a lamp's model threw its whole
	// outline, hard and room-sized (the user: "shadows from the lamps being right next to the fake lights that looked strange").
	// Every other shadow of those lights -- furniture, props, walls, people -- stays. A lamp still shadows its OWN light (one built into
	// its model, or one within OwnLightDistance of one of its small meshes, i.e. at its bulb): a shade then shapes the light as it should
	// (measured 2026-10-20: without this the saloon's arm lamps lit the ceiling straight through their metal shades).
	struct Part { float x, y, z, radius; };
	struct Model { float x, y, z, radius; const NiNode* root; std::vector<Part> parts; const char* name; };
	inline bool LogModels = false;							// LogLamps: each light's keep / leave-out answer per lamp model (capped)
	inline unsigned LogModelLines = 0;
	inline float OwnLightDistance = 24.0f;					// [Shaders.ShadowsInteriors.Forward] LampModelOwnDistance: world units from a lamp's
															// small mesh = its own (bulb) light; 0 = only lights built into its model; below 0
															// no lamp is a light's own, not even built in
	inline std::vector<Model> Models;
	inline std::unordered_map<const void*, int> ModelOf;		// NiGeometry* -> index in Models
	// The roots of every placed object that carries a model (statics, lamps, furniture, and lights WITH a model), but not the designers'
	// model-less lights: a light hanging under one of these is built into an object (a lamp's bulb), any other light is a designer's
	// invisible light (OwnLight).
	inline std::unordered_map<const void*, int> ObjectRoots;
	inline constexpr float MaxLampModel = 200.0f;			// bound radius: a ceiling fan is ~150; buildings named '...light...' are not lamps

	// UNOFFICIAL SkipOwnFixture (2026-11-11, [Shaders.ShadowsInteriors.Forward], BUG FIX): a light's own FIXTURE casts nothing into that
	// light's cube map. A tester's mod interior: a flat ceiling light panel with the game's point light right next to / behind it; the
	// game has no lamp shadows, so authors put a light anywhere near its fixture, and with cube-map shadows the panel threw a hard wedge
	// of darkness over the ceiling and the back wall. The ray-traced lamp shadows already skip lamp models and glass ([...RT]
	// SkipLampModels / SkipLampGlass / LampStop); the cube maps did so only with the look options LampModelShadows false (which keeps a
	// lamp's OWN light shadowed) and OwnGlassDistance (glowing parts up to MaxLampPart only: not a panel's housing, not a large pane).
	// A fixture is a placed object that
	//   - has a STATIC or LIGHT base form with a model file (never an actor, a door, furniture, an activator or a container),
	//   - is small: root bound radius up to MaxLampModel (room pieces are larger), and
	//   - is a lamp: a LIGHT form's own model, or a lamp by its file name (LampName), or it has a glowing mesh (Emission >= 0.25) and
	//     is up to MaxGlowFixture in radius (the glowing wall and window pieces measured in the saloon were 109-138 as meshes alone).
	// A light leaves a fixture out when it is built into it (the light's node hangs under the fixture's root) or stands within
	// OwnFixtureDistance of the fixture's bound. Walls, floors, furniture and people are never fixtures, so a lamp mounted 5 units from
	// a wall still has the wall's shadow. Kept apart from Models / ModelOf (LampModelShadows, the RT masks): those stay as they were.
	struct Fixture { float x, y, z, radius; const NiNode* root; const char* name; char kind; };	// kind: 'L' a light form's model, 'N' by name, 'G' glows
	inline constexpr float MaxGlowFixture = 128.0f;			// root bound radius of a fixture known only by its glow
	inline bool WantFixtures = false;						// SkipOwnFixture: list them (off: the lists stay empty)
	inline std::vector<Fixture> Fixtures;
	inline std::unordered_map<const void*, int> FixtureOf;		// NiGeometry* -> index in Fixtures
	inline std::unordered_map<const void*, int> FixtureRoots;	// the fixture's root NiNode* -> index in Fixtures
	inline unsigned FixtureGeneration = 0;					// bumped whenever the fixture list changes (the cube maps' caster hash)
	inline UInt64 FixtureHash = 0;

	// A lamp model by its file name (or, without one, its root node's name): light, lamp, fan, chandelier, sconce, bulb, lantern,
	// candle, torch. FalloutNV.esm's fixtures: dlc04lightwall01on.nif, LamplightChandelier.NIF, NV_Fan.NIF, ShackHangingLight01.NIF,
	// LamplightLamp.NIF, dlc04bwalklight01.nif, LampGenericR01.NIF, OperatingLightR01.NIF, dlc04lighthousebulb.nif, ...
	// Only the file name counts, not its folders (Dungeons/Caves/Lamplight/ holds every kind of model).
	inline bool LampName(const char* path) {
		if (!path || !*path) return false;
		const char* name = path;
		for (const char* c = path; *c; c++) if (*c == '\\' || *c == '/') name = c + 1;
		char low[128]; size_t n = 0;
		for (; name[n] && n < sizeof(low) - 1; n++) low[n] = (char)tolower((unsigned char)name[n]);
		low[n] = 0;
		static const char* const keys[] = { "light", "lamp", "fan", "chandel", "sconce", "bulb", "lantern", "candle", "torch" };
		for (const char* k : keys) if (strstr(low, k)) return true;
		return false;
	}

	// The model file of a placed object's base form, where New Vegas keeps one we know the layout of (statics, lights).
	inline const char* ModelPath(TESForm* base) {
		if (!base) return nullptr;
		if (base->formType == TESForm::FormType::kFormType_Stat) return static_cast<TESObjectSTAT*>(base)->model.nifPath.m_data;
		if (base->formType == TESForm::FormType::kFormType_Light) return static_cast<TESObjectLIGH*>(base)->modelSwap.nifPath.m_data;
		return nullptr;
	}

	struct Answer { float x, y, z; unsigned generation; float distance; };
	inline std::unordered_map<const void*, Answer> Answers;	// per NiPointLight: the nearest glowing mesh's distance

	inline double Now() {
		LARGE_INTEGER frequency, counter;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		return (double)counter.QuadPart / (double)frequency.QuadPart;
	}

	// How strongly a mesh glows: its material's emissive colour (times its multiplier) as luma; 0 without a material.
	inline float Emission(NiGeometry* geo) {
		NiMaterialProperty* material = static_cast<NiMaterialProperty*>(geo->GetProperty(NiProperty::kType_Material));
		if (!material) return 0.0f;
		const float mult = material->fEmitMult > 0.0f ? material->fEmitMult : 0.0f;	// a multiplier of 0 glows not at all
		return (0.2126f * material->emit.r + 0.7152f * material->emit.g + 0.0722f * material->emit.b) * mult;
	}

	// A lamp's glowing part: glows (emissive luma >= 0.25) and is small. Measured in game (LogLamps GLOW lines, 2026-10-20, saloon +
	// Doc Mitchell's house): every lamp part was 3-33 units in bound radius (bulbs 3-9, chandelier globes 9, sconce glass 12-16,
	// glow sprites 18-25, a hanging lamp's glow 33); everything else that glows was 66 or more (the fake window light shafts
	// IndFXLightRays 125-232, window panes 111, wall pieces 109-138, a jukebox 66-80). The shafts reach over the designers'
	// fill lights by the windows, so without the size limit those counted as lamps.
	inline constexpr float MaxLampPart = 48.0f;
	inline bool LampPart(NiGeometry* geo) {
		if (Emission(geo) < 0.25f) return false;
		NiBound* bound = geo->GetWorldBound();
		return bound && bound->Radius > 0.0f && bound->Radius <= MaxLampPart;
	}

	// UNOFFICIAL OwnGlassDistance (2026-10-30, [Shaders.ShadowsInteriors.Forward]): is this a lamp part (glowing glass, bulb, glow) whose
	// bound reaches within distance of the light, i.e. the light's own glass? The light shines out through it, so it casts nothing into
	// that light's own map (other lamps' maps keep it, LampGlassShadows). Vault 21's floor lamps sit inside their glass, 15 units above
	// the floor: with a shadow (VisibleRanking gives the visible ones a slot) the glass took their whole floor and wall pool.
	inline bool OwnGlass(NiGeometry* geo, const NiPoint3* at, float distance) {
		if (distance < 0.0f || !LampPart(geo)) return false;
		const NiBound* bound = geo->GetWorldBound();
		const float dx = bound->Center.x - at->x, dy = bound->Center.y - at->y, dz = bound->Center.z - at->z;
		return sqrtf(dx * dx + dy * dy + dz * dz) - bound->Radius <= distance;
	}

	inline void ListGlows(TESObjectCELL* cell, bool log) {
		const double started = Now();
		std::vector<Glow> found;
		std::vector<Model> models;
		std::unordered_map<const void*, int> modelOf, objectRoots;
		std::vector<NiAVObject*> stack;
		std::vector<Fixture> fixtures;								// UNOFFICIAL SkipOwnFixture (2026-11-11)
		std::unordered_map<const void*, int> fixtureOf, fixtureRoots;
		std::vector<NiGeometry*> refGeometries;
		UInt64 fixtureHash = 1469598103934665603ull;
		UInt64 hash = 1469598103934665603ull;
		unsigned refs = 0, geometries = 0;
		for (TList<TESObjectREFR>::Entry* entry = &cell->objectList.First; entry; entry = entry->next) {
			TESObjectREFR* ref = entry->item;
			if (!ref || !ref->baseForm) continue;
			NiNode* root = ref->GetNode();
			if (!root) continue;
			refs++;
			// A lamp model (LampModelShadows): by its file name, else its root node's name; whole models up to MaxLampModel.
			int model = -1;
			const char* path = ModelPath(ref->baseForm);
			const bool modelLessLight = ref->baseForm->formType == TESForm::FormType::kFormType_Light && !(path && *path);
			if (modelLessLight) continue;	// a designer's invisible light: no model, nothing to list
			objectRoots[root] = 1;
			NiBound* rootBound = root->GetWorldBound();
			if ((path && *path ? LampName(path) : LampName(root->m_pcName)) && rootBound && rootBound->Radius > 0.0f && rootBound->Radius <= MaxLampModel) {
				model = (int)models.size();
				models.push_back({ rootBound->Center.x, rootBound->Center.y, rootBound->Center.z, rootBound->Radius, root, {}, path && *path ? path : root->m_pcName });
				hash = (hash ^ (UInt64)(SInt64)(rootBound->Center.x * 8.0f + rootBound->Center.y * 32.0f + rootBound->Center.z * 128.0f)) * 1099511628211ull;
			}
			// UNOFFICIAL SkipOwnFixture: a small static or light form with a model may be a fixture (decided after its meshes are seen)
			const bool fixtureForm = WantFixtures && path && *path && rootBound && rootBound->Radius > 0.0f && rootBound->Radius <= MaxLampModel;
			bool refGlows = false;
			refGeometries.clear();
			stack.clear();
			stack.push_back(root);
			while (!stack.empty()) {
				NiAVObject* object = stack.back();
				stack.pop_back();
				if (!object || (object->m_flags & NiAVObject::NiFlags::APP_CULLED)) continue;
				if (object->IsGeometry()) {
					NiGeometry* geo = static_cast<NiGeometry*>(object);
					geometries++;
					if (model >= 0) {
						modelOf[geo] = model;
						if (NiBound* b = geo->GetWorldBound()) models[model].parts.push_back({ b->Center.x, b->Center.y, b->Center.z, b->Radius });
					}
					const float emit = Emission(geo);
					if (fixtureForm) { refGeometries.push_back(geo); if (emit >= 0.25f) refGlows = true; }
					if (emit < 0.25f) continue;
					NiBound* bound = geo->GetWorldBound();
					if (!bound || !(bound->Radius > 0.0f) || bound->Radius > MaxLampPart) continue;	// a lamp part (LampPart), not a light shaft
					found.push_back({ bound->Center.x, bound->Center.y, bound->Center.z, bound->Radius, emit, geo->m_pcName, root->m_pcName, model });
					hash = (hash ^ (UInt64)(SInt64)(bound->Center.x * 4.0f + bound->Center.y * 16.0f + bound->Center.z * 64.0f)) * 1099511628211ull;
					continue;
				}
				if (NiNode* node = object->IsNiNode())
					for (int i = 0; i < node->m_children.end; i++) stack.push_back(node->m_children.data[i]);
			}
			if (fixtureForm && !refGeometries.empty()) {	// UNOFFICIAL SkipOwnFixture: a light form's model, a lamp by name, or it glows
				const bool lightForm = ref->baseForm->formType == TESForm::FormType::kFormType_Light, named = LampName(path);
				if (!lightForm && !named) refGlows = refGlows && rootBound->Radius <= MaxGlowFixture;
				if (lightForm || named || refGlows) {
					const int index = (int)fixtures.size();
					fixtures.push_back({ rootBound->Center.x, rootBound->Center.y, rootBound->Center.z, rootBound->Radius, root, path, lightForm ? 'L' : named ? 'N' : 'G' });
					fixtureRoots[root] = index;
					for (NiGeometry* g : refGeometries) fixtureOf[g] = index;
					fixtureHash = (fixtureHash ^ (UInt64)(SInt64)(rootBound->Center.x * 8.0f + rootBound->Center.y * 32.0f + rootBound->Center.z * 128.0f) ^
						((UInt64)refGeometries.size() << 48)) * 1099511628211ull;
				}
			}
		}
		fixtureHash ^= fixtures.size();
		const bool fixturesChanged = fixtureHash != FixtureHash;
		if (fixturesChanged) { FixtureHash = fixtureHash; FixtureGeneration++; }
		if (log && fixturesChanged && WantFixtures) {
			Logger::Log("UNOFFICIAL LampFixtures: %u fixtures (%u meshes) for SkipOwnFixture: small static / light objects that are a light form's model (L), lamps by name (N) or glow (G)",
				(unsigned)fixtures.size(), (unsigned)fixtureOf.size());
			for (size_t i = 0; i < fixtures.size() && i < 80; i++)
				Logger::Log("  FIXTURE %3u %c at %.0f %.0f %.0f radius %.0f | '%s'", (unsigned)i, fixtures[i].kind, fixtures[i].x, fixtures[i].y, fixtures[i].z,
					fixtures[i].radius, fixtures[i].name ? fixtures[i].name : "");
		}
		Fixtures.swap(fixtures);
		FixtureOf.swap(fixtureOf);
		FixtureRoots.swap(fixtureRoots);
		hash ^= found.size() ^ ((UInt64)models.size() << 20) ^ ((UInt64)(refs > 0) << 40);
		Ready = refs > 0;
		const bool changed = hash != GlowHash;
		if (changed) { GlowHash = hash; Generation++; }
		if (log && changed) {
			Logger::Log("UNOFFICIAL LampFixtures: %u glowing meshes and %u lamp models (%u meshes) among %u meshes of %u placed objects with 3D (listed in %.2f ms)",
				(unsigned)found.size(), (unsigned)models.size(), (unsigned)modelOf.size(), geometries, refs, (Now() - started) * 1000.0);
			for (size_t i = 0; i < found.size() && i < 160; i++)
				Logger::Log("  GLOW %3u at %.0f %.0f %.0f radius %.0f emissive %.2f | '%s' of '%s'", (unsigned)i, found[i].x, found[i].y, found[i].z,
					found[i].radius, found[i].emit, found[i].name ? found[i].name : "", found[i].root ? found[i].root : "");
		}
		Glows.swap(found);
		Models.swap(models);
		ModelOf.swap(modelOf);
		ObjectRoots.swap(objectRoots);
	}

	// Is this light the lamp model's own: built into it (the light's node hangs under the model's root), or a light built into another
	// object (a bulb model inside a shade, the saloon's arm lamps) within OwnLightDistance of one of its small meshes? A designer's
	// invisible light never is, wherever it stands: the house's main light placed inside a chandelier's globes made the chandelier throw
	// its frame across the ceiling (the user's 'shadow from nowhere', Doc Mitchell's house, 2026-10-20).
	inline bool OwnLight(const Model& m, const NiPointLight* light, const NiPoint3* at) {
		if (OwnLightDistance < 0.0f) return false;
		bool builtIn = false;
		for (const NiAVObject* node = light; node; node = node->m_parent) {
			if (node == m.root) return true;
			if (ObjectRoots.count(node)) builtIn = true;
		}
		if (!builtIn || !(OwnLightDistance > 0.0f)) return false;	// a designer's light, or 0: only lights built into this lamp's model
		for (const Part& p : m.parts) {
			// Only a lamp's small parts (bulb, shade, socket: MaxLampPart) place its bulb; a ceiling fan's blade mesh is ~100 units round
			// and took the globe lights under it for the fan's own (measured 2026-10-20: the blade ovals stayed).
			if (p.radius > MaxLampPart) continue;
			const float dx = p.x - at->x, dy = p.y - at->y, dz = p.z - at->z;
			if (sqrtf(dx * dx + dy * dy + dz * dz) - p.radius <= OwnLightDistance) return true;
		}
		return false;
	}

	inline int ModelIndexOf(const NiGeometry* geo) { auto it = ModelOf.find(geo); return it == ModelOf.end() ? -1 : it->second; }

	// UNOFFICIAL NoShadowLampModels (2026-10-27, [Shaders.ShadowsInteriors.Forward]): lamp models whose file name holds one of these
	// words (comma separated, any case; default "lantern") cast NO shadow for any light within LampModelDistance of them, their OWN
	// light included. The user (Prospector Saloon): a small lantern's frame and glass threw a cage of shadows round the room that
	// moved with the flickering flame; the other lamps keep theirs (a shade shaping its own light looks right, LampModelShadows).
	inline std::vector<std::string> NoShadowWords;
	inline std::string NoShadowFrom = "\x01";					// the text NoShadowWords was split from (an impossible start: split once)
	inline void SetNoShadowWords(const std::string& text) {
		if (text == NoShadowFrom) return;
		NoShadowFrom = text;
		NoShadowWords.clear();
		std::string word;
		for (size_t i = 0; i <= text.size(); i++) {
			const char c = i < text.size() ? text[i] : ',';
			if (c == ',' || c == ';') {
				while (!word.empty() && isspace((unsigned char)word.back())) word.pop_back();
				if (!word.empty()) NoShadowWords.push_back(word);
				word.clear();
			}
			else if (c != '"' && !(word.empty() && isspace((unsigned char)c))) word += (char)tolower((unsigned char)c);
		}
	}
	// Is this caster part of a lamp model named by NoShadowWords within distance of the light (its bound's edge)? cache: per cube
	// draw, model index -> 0 unknown, 1 leave out, 2 keep.
	inline bool NoShadowLampModel(const NiGeometry* geo, const NiPoint3* at, float distance, std::unordered_map<int, int>& cache) {
		if (NoShadowWords.empty()) return false;
		auto it = ModelOf.find(geo);
		if (it == ModelOf.end()) return false;
		int& answer = cache[it->second];
		if (!answer) {
			const Model& m = Models[it->second];
			const char* name = m.name ? m.name : "";
			for (const char* c = name; *c; c++) if (*c == '\\' || *c == '/') name = c + 1;	// the file name, not its folders
			char low[128]; size_t n = 0;
			for (; name[n] && n < sizeof(low) - 1; n++) low[n] = (char)tolower((unsigned char)name[n]);
			low[n] = 0;
			bool named = false;
			for (const std::string& w : NoShadowWords) if (strstr(low, w.c_str())) { named = true; break; }
			const float dx = m.x - at->x, dy = m.y - at->y, dz = m.z - at->z;
			const float edge = sqrtf(dx * dx + dy * dy + dz * dz) - m.radius;
			answer = named && edge <= distance ? 1 : 2;
			if (LogModels && named && LogModelLines < 400) {
				LogModelLines++;
				Logger::Log("LAMPMODEL light at %.0f %.0f %.0f: lamp model '%s' (radius %.0f) edge %.0f units: %s (NoShadowLampModels)",
					at->x, at->y, at->z, m.name ? m.name : "", m.radius, edge, answer == 1 ? "casts NO shadow for it" : "too far, keeps its shadow");
			}
		}
		return answer == 1;
	}

	// LampModelShadows false: does this caster belong to a lamp model within distance of the light (its bound's edge) that is not the
	// light's own lamp? cache: per cube draw, model index -> 0 unknown, 1 leave out, 2 keep.
	inline bool NearLampModel(const NiGeometry* geo, const NiPointLight* light, const NiPoint3* at, float distance, std::unordered_map<int, int>& cache) {
		auto it = ModelOf.find(geo);
		if (it == ModelOf.end()) return false;
		int& answer = cache[it->second];
		if (!answer) {
			const Model& m = Models[it->second];
			const float dx = m.x - at->x, dy = m.y - at->y, dz = m.z - at->z;
			const float edge = sqrtf(dx * dx + dy * dy + dz * dz) - m.radius;
			const bool close = edge <= distance, own = close && OwnLight(m, light, at);	// ('near' is a Windows macro)
			answer = close && !own ? 1 : 2;
			if (LogModels && close && LogModelLines < 400) {
				LogModelLines++;
				float smallest = 1.0e9f;
				for (const Part& p : m.parts) smallest = (std::min)(smallest, p.radius);
				Logger::Log("LAMPMODEL light %p at %.0f %.0f %.0f: lamp model '%s' (radius %.0f, %u meshes, smallest %.0f) edge %.0f units: %s",
					(const void*)light, at->x, at->y, at->z, m.name ? m.name : "", m.radius, (unsigned)m.parts.size(), smallest, edge,
					own ? "its OWN lamp, keeps shadowing it" : "LEFT OUT of its shadows");
			}
		}
		return answer == 1;
	}

	// UNOFFICIAL SkipOwnFixture (2026-11-11): the fixture this light is built into (its node hangs under the fixture's root), -1 none.
	inline int BuiltIntoFixture(const NiPointLight* light) {
		if (FixtureRoots.empty()) return -1;
		for (const NiAVObject* node = light; node; node = node->m_parent) {
			auto it = FixtureRoots.find(node);
			if (it != FixtureRoots.end()) return it->second;
		}
		return -1;
	}

	// UNOFFICIAL SkipOwnFixture: is this caster a mesh of the light's own fixture: the one the light is built into (builtInto), or one
	// whose bound's edge is within distance of the light? cache: per cube draw, fixture index -> 0 unknown, 1 leave out, 2 keep.
	inline bool OwnFixture(const NiGeometry* geo, const NiPointLight* light, const NiPoint3* at, float distance, int builtInto, std::unordered_map<int, int>& cache) {
		auto it = FixtureOf.find(geo);
		if (it == FixtureOf.end()) return false;
		const Fixture& f = Fixtures[it->second];
		// The mesh must still lie in its fixture's bound (a freed mesh's address taken by another object's mesh before the next listing)
		if (const NiBound* b = geo->m_kWorldBound) {
			const float cx = b->Center.x - f.x, cy = b->Center.y - f.y, cz = b->Center.z - f.z, reach = f.radius + 1.0f;
			if (cx * cx + cy * cy + cz * cz > reach * reach) return false;
		}
		int& answer = cache[it->second];
		if (!answer) {
			const float dx = f.x - at->x, dy = f.y - at->y, dz = f.z - at->z;
			const float edge = sqrtf(dx * dx + dy * dy + dz * dz) - f.radius;
			const bool own = it->second == builtInto;
			answer = own || edge <= distance ? 1 : 2;
			if (LogModels && answer == 1 && LogModelLines < 400) {
				LogModelLines++;
				Logger::Log("FIXTURE light %p at %.0f %.0f %.0f: fixture '%s' (%c, radius %.0f) edge %.0f units%s: LEFT OUT of its shadow map (SkipOwnFixture)",
					(const void*)light, at->x, at->y, at->z, f.name ? f.name : "", f.kind, f.radius, edge, own ? ", the light is built into it" : "");
			}
		}
		return answer == 1;
	}

	// Once per frame, indoors: list the cell's glowing meshes when the cell changed, every second for its first 10 seconds,
	// then every 10 seconds. log: LogLamps (a line whenever the list changes).
	inline void Update(TESObjectCELL* cell, bool log) {
		if (!cell) return;
		const double now = Now();
		if (cell != GlowCell) { GlowCell = cell; CellEntered = now; GlowListed = -1.0e9; Answers.clear(); Ready = false; Models.clear(); ModelOf.clear(); ObjectRoots.clear();
			if (!Fixtures.empty()) { Fixtures.clear(); FixtureOf.clear(); FixtureRoots.clear(); FixtureHash = 0; FixtureGeneration++; } }	// UNOFFICIAL SkipOwnFixture
		const double period = now - CellEntered < 10.0 ? 1.0 : 10.0;
		if (now - GlowListed < period) return;
		GlowListed = now;
		ListGlows(cell, log);
	}

	// Distance (world units) from a light to the nearest glowing mesh's bound (0 inside it); a large number when none.
	inline float NearestGlow(const NiPointLight* light) {
		const NiPoint3& p = light->m_worldTransform.pos;
		auto it = Answers.find(light);
		if (it != Answers.end() && it->second.generation == Generation && it->second.x == p.x && it->second.y == p.y && it->second.z == p.z)
			return it->second.distance;
		float best = 1.0e9f;
		for (const Glow& g : Glows) {
			const float dx = g.x - p.x, dy = g.y - p.y, dz = g.z - p.z;
			best = (std::min)(best, (std::max)(0.0f, sqrtf(dx * dx + dy * dy + dz * dz) - g.radius));
		}
		Answers[light] = { p.x, p.y, p.z, Generation, best };
		return best;
	}

	// UNOFFICIAL InvisibleShadowFromLamp (2026-10-20): the lamp part (a small glowing mesh: bulb, globe, shade glass, glow) nearest to a
	// light within maxDistance of its centre, or null. ShadowManager then draws that invisible light's shadow map from there: the room is
	// lit as the designers placed the light, but its shadows fall from a lamp the player can see.
	inline const Glow* NearestLampPart(const NiPointLight* light, float maxDistance) {
		const NiPoint3& p = light->m_worldTransform.pos;
		const Glow* best = nullptr; float bestDistance = maxDistance * maxDistance;
		for (const Glow& g : Glows) {
			const float dx = g.x - p.x, dy = g.y - p.y, dz = g.z - p.z, d = dx * dx + dy * dy + dz * dz;
			if (d <= bestDistance) { bestDistance = d; best = &g; }
		}
		return best;
	}

	// LogLamps: which glowing mesh (index in the GLOW lines) is the nearest, -1 none.
	inline int NearestGlowIndex(const NiPointLight* light) {
		const NiPoint3& p = light->m_worldTransform.pos;
		int best = -1; float bestDistance = 1.0e9f;
		for (size_t i = 0; i < Glows.size(); i++) {
			const float dx = Glows[i].x - p.x, dy = Glows[i].y - p.y, dz = Glows[i].z - p.z;
			const float d = (std::max)(0.0f, sqrtf(dx * dx + dy * dy + dz * dz) - Glows[i].radius);
			if (d < bestDistance) { bestDistance = d; best = (int)i; }
		}
		return best;
	}

	// An invisible light: no lamp part within fixtureDistance, and not carried. Right after entering a cell, before its objects have
	// their 3D (in game: the first listing of a cell can see none), there is no answer and every light keeps its shadow as before.
	inline bool Invisible(const NiPointLight* light, float fixtureDistance) {
		if (!light || light->CanCarry || !Ready) return false;
		return NearestGlow(light) > fixtureDistance;
	}
}
