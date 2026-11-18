#pragma once

// UNOFFICIAL 2026-10-29 ShadowPool ([Shaders.ShadowsInteriors.Forward] PoolLamps, 0 = off; docs/next/INTERIOR-SHADOWS-MODERN.md step 1,
// built on the existing cube maps instead of a new atlas). Indoors with the forward lamp shadows, only the LightPoints lamps the ranking
// puts first (the user plays with 24; at most 64) get a cube map; every other lamp lights the objects it reaches with no shadow at all,
// and that unshadowed light fills in the shadows of the others (the "holes": Vault 21 has 447 lights). Most of those lamps never move
// and neither does what they light, so their shadows are a constant of the cell. Here the next PoolLamps lamps of the same ranking
// (ShaderManager::GetNearbyLights, NoteCandidate) get a STATIC cube map of their own, kept per lamp:
//   - drawn with the static casters only (walls, furniture: CubeLayer::StaticOnly, no people), PoolDrawsPerFrame lamps a frame at most
//     (the nearest first), and never again while the lamp keeps its position, reach and cell; a lamp that leaves the candidates keeps its
//     map (least recently used goes first when the pool is full), so walking back costs nothing;
//   - drawn through the cube map loop's own code: the pool cube's texture and faces are swapped into the static entries of a spare
//     slot (the last one, ShadowCubeMapsMax - 1, never a live slot while the pool is on: LightPoints stays below it) for the draw;
//   - published after the live slots (PointShadowForward::PublishPool), with the same fill / invisible light rules and the same filter,
//     so an object binds each of its lamps' shadows the same way, whichever kind holds it.
// People cast no shadow from pool lamps (their shadows come from the live slots' lamps, as before). Off (0): nothing is made or drawn.
#include <d3d9.h>
#include <unordered_map>
#include <vector>

constexpr int ShadowPoolMax = 192;

namespace ShadowPool {

	struct Cube {
		IDirect3DCubeTexture9* texture = nullptr;
		IDirect3DSurface9* faces[6] = {};
		const ShadowSceneLight* light = nullptr;
		float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
		const void* cell = nullptr;
		bool valid = false;
		UInt32 drawn = 0;			// UNOFFICIAL 2026-10-30 ShadowAtlas: counts the draws of this entry (its atlas tile is converted again after one)
		UInt32 used = 0;			// frame last published
		double since = 0.0;			// when it was first published (FadeIn)
		UInt64 fixtureKey = 0;		// UNOFFICIAL 2026-11-11 SkipOwnFixture: the switch / distance / fixture list it was drawn with (ShadowManager)
	};
	inline std::vector<Cube> Cubes;
	inline std::unordered_map<const ShadowSceneLight*, int> ByLight;
	inline std::vector<ShadowSceneLight*> Candidates;	// this frame's lamps past the live slots, in ranking order
	inline UINT MadeSize = 0;
	inline bool Failed = false;
	inline UInt32 Frame = 0;
	inline unsigned StatFrames = 0, StatPublished = 0, StatDrawn = 0, StatWaiting = 0;

	inline int Wanted() { return (std::max)(0, (std::min)(TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "PoolLamps"), (int)ShadowPoolMax)); }
	inline int Budget() { const int b = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "PoolDrawsPerFrame"); return b > 0 ? (std::min)(b, 32) : 2; }

	// ShaderManager::GetNearbyLights: at its start, and for each shadow-casting lamp the live slots had no room for.
	inline void BeginRanking() { Candidates.clear(); }
	inline void NoteCandidate(ShadowSceneLight* light) { if (light && Candidates.size() < 512 && Wanted() > 0) Candidates.push_back(light); }

	inline void Release() {
		for (Cube& c : Cubes) {
			for (IDirect3DSurface9*& f : c.faces) if (f) { f->Release(); f = nullptr; }
			if (c.texture) { c.texture->Release(); c.texture = nullptr; }
		}
		Cubes.clear();
		ByLight.clear();
		MadeSize = 0;
	}

	inline bool MakeCube(Cube& c, UINT size) {
		if (FAILED(TheRenderManager->device->CreateCubeTexture(size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &c.texture, NULL)) || !c.texture) {
			c.texture = nullptr;
			return false;
		}
		for (int j = 0; j < 6; j++) c.texture->GetCubeMapSurface((D3DCUBEMAP_FACES)j, 0, &c.faces[j]);
		return true;
	}

	// The pool entry for a lamp: its own, a new one, or the least recently used one (not used this frame). -1: none to be had.
	inline int EntryFor(const ShadowSceneLight* light, UINT size, int limit) {
		auto found = ByLight.find(light);
		if (found != ByLight.end()) return found->second;
		int index = -1;
		if ((int)Cubes.size() < limit) {
			Cube c;
			if (!MakeCube(c, size)) {
				if (!Failed) Logger::Log("[WARNING] UNOFFICIAL ShadowPool: could not make cube map %u (video memory?): the pool stays at %u lamps", (unsigned)Cubes.size() + 1, (unsigned)Cubes.size());
				Failed = true;
			}
			else { Cubes.push_back(c); index = (int)Cubes.size() - 1; }
		}
		if (index < 0) {
			UInt32 oldest = 0xFFFFFFFF;
			for (int i = 0; i < (int)Cubes.size(); i++) if (Cubes[i].used != Frame && Cubes[i].used < oldest) { oldest = Cubes[i].used; index = i; }
			if (index < 0) return -1;
			if (Cubes[index].light) ByLight.erase(Cubes[index].light);
		}
		Cube& c = Cubes[index];
		c.light = light; c.valid = false; c.since = 0.0;
		ByLight[light] = index;
		return index;
	}
}
