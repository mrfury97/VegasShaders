#pragma once

// UNOFFICIAL 2026-10-23 CharacterObjectReach ([Shaders.ShadowsExteriors.Main], 1 = on; compiled into the object pixel shaders when on at
// startup (ShaderRecord: SHADOW_CHARACTER_OBJECTS), live after that; off = the shaders and the draws exactly as before).
// P82 tester: as the third-person camera zoomed out, the hair's and the nose's sun shadow faded off a facemask (drawn with the OBJECT
// shaders) sooner than off the skin face next to it. The skin shaders cap the forward PCF slack at CharacterShadowReach world units
// (Includes/Shadow.hlsl ShadowSlackCap); the object shaders could not, because the same shaders draw the armour, whose flat plates need
// the full slack at a low sun (the 2026-10-14 stripes). Here each object draw is told whether it is an actor's head, hair, face mask,
// eyes or headgear: pixel constant c204.x = 1 for those, 0 for every other draw (the shader multiplies the reach by it).
// A draw counts when its geometry is skinned to head bones only (every bone's name has "head", "neck", "hair", "ponytail", "jaw",
// "eye", "brow", "lip", "tongue" or "hat" in it, at least one "head"), or is not skinned and hangs under a node named like the head bone
// ("Bip01 Head", "HeadAnims") within 8 parents. The verdict is kept per geometry (cleared now and then: pointers are reused).
#include "SkinBones.h"	// 2026-11-18: a skin's bone slots may be stale
#include <unordered_map>

namespace CharacterObjectReach {
	inline bool Checked = false, Compiled = false, On = false;
	inline float Sent = -1.0f;
	inline unsigned Frame = 0, Logged = 0;
	inline bool LogDraws = false;
	inline std::unordered_map<const void*, bool> Verdicts;

	inline bool Contains(const char* name, const char* part) {
		if (!name) return false;
		const size_t n = strlen(part);
		for (const char* p = name; *p; p++) if (!_strnicmp(p, part, n)) return true;
		return false;
	}

	inline bool HeadBoneName(const char* name) {
		static const char* const parts[] = { "head", "neck", "hair", "ponytail", "jaw", "eye", "brow", "lip", "tongue", "hat" };
		for (const char* part : parts) if (Contains(name, part)) return true;
		return false;
	}

	// sure (2026-11-18): false when a bone slot was not a live node (SkinBones.h: an item's dropped bones before the game binds its skin);
	// the verdict is then not kept, and worked out again at the next draw
	inline bool Classify(const NiGeometry* geometry, const char*& why, bool& sure) {
		sure = true;
		const NiSkinInstance* si = geometry->skinInstance;
		const NiSkinData* sd = si ? si->SkinData : nullptr;
		if (si && sd && si->BoneObjects) {
			// the count is the skin data's (NiSkinInstance::Bones reads 0 in game: ClothWind.h SkeletonOf)
			const UInt32 count = (std::min)(sd->Bones, (UInt32)512);
			bool head = false;
			for (UInt32 b = 0; b < count; b++) {
				const NiAVObject* bone = si->BoneObjects[b];
				if (!bone) continue;
				static char name[128];	// (why points at it after a return; read at once by the caller's log line)
				if (!SkinBones::Name(bone, name, sizeof(name))) { if (!SkinBones::Live(bone)) sure = false; continue; }
				if (!HeadBoneName(name)) { why = name; return false; }
				if (Contains(name, "head")) head = true;
			}
			why = head ? "skinned to head bones only" : "no head bone";
			return head;
		}
		const NiAVObject* node = geometry->m_parent;
		for (int depth = 0; node && depth < 8; depth++, node = node->m_parent) {
			const char* name = node->m_pcName;
			if (name && (!_stricmp(name, "Bip01 Head") || !_stricmp(name, "HeadAnims"))) { why = name; return true; }
		}
		why = "not under the head";
		return false;
	}

	inline void BeginFrame() {
		if (!Checked) {
			Checked = true;
			Compiled = TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "CharacterObjectReach") != 0;	// as ShaderRecord compiled it
			Logger::Log("UNOFFICIAL CharacterObjectReach: %s", Compiled ? "on (actors' head, hair, face mask and headgear object draws take CharacterShadowReach's slack cap)" :
				"off (not compiled into the shaders)");
		}
		On = Compiled && TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "CharacterObjectReach") != 0;
		LogDraws = Compiled && TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "CharacterObjectReachLog") != 0;
		Sent = -1.0f;	// resent with the frame's first object (effects between frames may use the high registers)
		if (++Frame % 1800 == 0 || Verdicts.size() > 8192) Verdicts.clear();
	}

	// One object pass about to be drawn (RenderPassStandard/Skinned hooks). world: the world scene (not the reflections, not first person).
	inline void ForDraw(IDirect3DDevice9* device, const NiGeometry* geometry, bool world) {
		if (!Compiled) return;
		float flag = 0.0f;
		if (On && world && geometry) {
			auto it = Verdicts.find(geometry);
			bool unsureHead = false, unsure = false;
			if (it == Verdicts.end()) {
				const char* why = "";
				bool sure = true;
				const bool head = Classify(geometry, why, sure);
				if (!sure) { unsure = true; unsureHead = head; }	// (2026-11-18) not kept: worked out again at the next draw
				else it = Verdicts.emplace(geometry, head).first;
				if (LogDraws && Logged < 400) {
					Logged++;
					Logger::Log("CharacterObjectReach: '%s' %s (%s)", geometry->m_pcName ? geometry->m_pcName : "?", head ? "HEAD" : "no", why ? why : "");
				}
			}
			flag = (unsure ? unsureHead : it->second) ? 1.0f : 0.0f;
		}
		if (flag == Sent) return;
		Sent = flag;
		const float value[4] = { flag, 0.0f, 0.0f, 0.0f };
		device->SetPixelShaderConstantF(204, value, 1);
	}
}
