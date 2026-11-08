#pragma once
// UNOFFICIAL 2026-11-07 (perf-mirrorfix) the hair's tint for the ray-traced scene. The game draws hair grey and tints it in the hair pixel
// shaders (SM3002 / SM3003: HairTint, c2: albedo x 2 x lerp(0.5, HairTint, vertex colour .y)); the traced scene took the grey texture
// alone ("white hair in mirrors", docs/next/RT-REAL-TEXTURES.md). Render.cpp notes c2 after each world draw of a skinned geometry with a
// hair shader bound (NoteBind in SetShadersHook, NoteDraw after RenderPassSkinned), keyed by the geometry; RtCore::AddSkinned reads it
// (Find) and packs a TINTED record (RtScenePick.h ActorRecordFlags). Only while RtCore asks (Wanted: actors and hit textures on).
#include <d3d9.h>
#include <unordered_map>
#include <cstring>

namespace RtHairTint {
	struct Tint { float rgb[3]; unsigned frame; };
	inline bool Wanted = false;			// RtCore: actors + hit textures on (set each scene walk)
	inline bool BoundHair = false;		// the pixel shader bound last is a hair shader
	inline std::unordered_map<const void*, Tint> Tints;
	inline unsigned Frame = 0;			// RtCore's frame counter (NoteDraw stamps it)

	// SetShadersHook: the pixel shader bound now (its record's name, e.g. "SM3003.pso")
	inline void NoteBind(const char* pixelShaderName) {
		BoundHair = pixelShaderName && (!strcmp(pixelShaderName, "SM3003.pso") || !strcmp(pixelShaderName, "SM3002.pso"));
	}

	// After a skinned pass of the world scene drew: the tint it drew with
	inline void NoteDraw(const void* geometry, IDirect3DDevice9* device) {
		if (!Wanted || !BoundHair || !geometry || !device) return;
		float c[4];
		if (FAILED(device->GetPixelShaderConstantF(2, c, 1))) return;
		Tint& t = Tints[geometry];
		t.rgb[0] = c[0]; t.rgb[1] = c[1]; t.rgb[2] = c[2]; t.frame = Frame;
		if (Tints.size() > 4096) {	// geometries that went away (a cell change): drop the ones not drawn for a while
			for (auto it = Tints.begin(); it != Tints.end(); ) { if (Frame - it->second.frame > 600u) it = Tints.erase(it); else ++it; }
		}
	}

	// The tint noted for this geometry within the last `maxAge` frames
	inline bool Find(const void* geometry, float rgb[3], unsigned maxAge = 600u) {
		auto it = Tints.find(geometry);
		if (it == Tints.end() || Frame - it->second.frame > maxAge) return false;
		std::memcpy(rgb, it->second.rgb, sizeof(float) * 3);
		return true;
	}

	inline void Clear() { Tints.clear(); }
}
