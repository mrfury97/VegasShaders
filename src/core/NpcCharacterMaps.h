#pragma once

// UNOFFICIAL 2026-11-08 CharacterShadowMapNpcs ([Shaders.ShadowsExteriors.ShadowMaps], 0 = off). ShadowManager::RenderNpcCharacterMaps
// draws a sharp sun shadow map for each of the nearest people (ShadowsExteriorEffect::CharacterNpcTiles). The skin and object shaders
// read one character map (Includes/Shadow.hlsl CharacterMapShadow: TESR_CharShadowMap on s11, TESR_CharShadowCameraToLight c206-c209,
// TESR_CharShadowCenter c213, TESR_CharShadowData c214), so here each draw of an NPC that has a map gets that map and its constants
// instead of the player's, and the next draw that is not one gets the player's back. The shaders are unchanged.
// Called for every object pass (Render.cpp RenderPassStandard/Skinned hooks, before the pass binds) and again after a bind
// (SetShadersHook: a bind uploads the player's constants and texture, or keeps the last ones when the shader is already bound).
// A geometry is an NPC's when its parents reach that NPC's root node (48 levels at most).
// (ShadowsExteriorEffect: included before this header by its users)

namespace NpcCharacterMaps {
	inline bool Dirty = false;	// the device holds an NPC's map (not the player's)
	inline int Applied = -1;	// which, while Dirty

	inline int TileOf(const ShadowsExteriorEffect* fx, const NiAVObject* object) {
		const int live = fx->CharacterNpcLive;
		for (int depth = 0; object && depth < 48; depth++, object = object->m_parent) {
			for (int t = 0; t < live; t++) if ((const NiAVObject*)fx->CharacterNpcTiles[t].node == object) return t;
		}
		return -1;
	}

	inline void SendPlayer(IDirect3DDevice9* device, ShadowsExteriorEffect* fx) {
		TheRenderManager->renderState->SetTexture(11, fx->CharacterMapTexture);
		device->SetPixelShaderConstantF(206, (const float*)&fx->Constants.CharacterCameraToLight, 4);
		device->SetPixelShaderConstantF(213, (const float*)&fx->Constants.CharacterCenter, 1);
		device->SetPixelShaderConstantF(214, (const float*)&fx->Constants.CharacterData, 1);
		Dirty = false;
		Applied = -1;
	}

	// The player's map back on the device (before the NPC maps are drawn into: none of them may stay bound as a texture).
	inline void Reset(IDirect3DDevice9* device) {
		ShadowsExteriorEffect* fx = TheShaderManager->Effects.ShadowsExteriors;
		if (Dirty && fx) SendPlayer(device, fx);
	}

	// geometry: the pass's object; world: the world scene (not first person, reflections or menus); afterBind: the pass's shaders were just bound.
	inline void ForDraw(IDirect3DDevice9* device, const NiAVObject* geometry, bool world, bool afterBind) {
		ShadowsExteriorEffect* fx = TheShaderManager->Effects.ShadowsExteriors;
		if (!fx) return;
		const int tile = world && fx->CharacterNpcLive > 0 && geometry ? TileOf(fx, geometry) : -1;
		if (tile < 0) {
			if (Dirty) SendPlayer(device, fx);
			return;
		}
		if (!afterBind && Dirty && Applied == tile) return;
		const ShadowsExteriorEffect::CharacterNpcTile& t = fx->CharacterNpcTiles[tile];
		TheRenderManager->renderState->SetTexture(11, fx->CharacterNpcTexture[tile]);
		device->SetPixelShaderConstantF(206, (const float*)&t.toLight, 4);
		device->SetPixelShaderConstantF(213, (const float*)&t.center, 1);
		device->SetPixelShaderConstantF(214, (const float*)&t.data, 1);
		Dirty = true;
		Applied = tile;
	}
}
