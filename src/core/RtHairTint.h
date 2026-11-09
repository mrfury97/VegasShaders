#pragma once
// UNOFFICIAL 2026-11-07 (perf-mirrorfix) the hair's tint for the ray-traced scene. The game draws hair grey and tints it in the hair pixel
// shaders (SM3002 / SM3003: HairTint, c2: albedo x 2 x lerp(0.5, HairTint, vertex colour .y)); the traced scene took the grey texture
// alone ("white hair in mirrors", docs/next/RT-REAL-TEXTURES.md). RtCore packs a TINTED record (RtScenePick.h) with the tint below.
//
// UNOFFICIAL 2026-11-08 (perf-reflfix4, test 12: the reflected hair still white, first person) where the tint really comes from, checked
// against the game's own code and files (docs/next/RT-PATH-TRACING.md "Team perf-reflfix4"):
//   - the hair, beards and brows are RIGID shapes: the vanilla hair NIFs (Fallout - Meshes.bsa, meshes\characters\hair\*.nif) hold no
//     skin instance, and the tester's test-10 log shows 'FaceGenHairNoHat' taken by RtCore::AddGeometry (the static path). Every tint
//     look-up since perf-mirrorfix sat in RtCore::AddSkinned and the draw notes in the skinned draws only: the hair never met either;
//   - the colour the hair shader is given (c2) is the hair's own shader property, BSShaderPPLightingProperty +0x84 (an NiColorA): the
//     game's Lighting30Shader copies those 16 bytes into its pixel constant 2 for the hair pass (decomp lighting30shader.cpp: constant
//     register 2 <- property +0x84..+0x90, the pass's texture 5 <- the property's "glow/skin/hair layer" texture at +0xB4, as our SM3002
//     LayerMap s5). ApplyFaceGenTextures (0x00655FA0) writes it when the head is built, for the head's children named "FaceGenHair..."
//     and "FaceGenAccessory" whose shader type is 8 or 12: (R = (c & 0xFF) / 255, G = ((c >> 8) & 0xFF) / 255, B = ((c >> 16) & 0xFF) /
//     255, 1) from the NPC's hair colour c (FaceGen data +0x84 = TESNPC::iHairColor, +0x1D8). The property's constructor zeroes it.
//   So RtCore reads the tint straight from the property (Property), for the player and NPCs, first and third person, drawn or not; a tint
//   noted from a draw (Noted) and the player's TESNPC colour (PlayerColour, R in the LOW byte: perf-reflfix3 read it the other way round,
//   red hair would have come out blue) are only fallbacks where the property holds no written colour.
#include <d3d9.h>
#include <unordered_map>
#include <cstring>
#include <cstdint>
#include <cmath>

namespace RtHairTint {
	struct Tint { float rgb[3]; unsigned frame; };
	inline bool Wanted = false;			// RtCore: actors or alpha-tested casters + hit textures on (set each scene walk)
	inline bool BoundHair = false;		// the pixel shader bound last is a hair shader
	inline std::unordered_map<const void*, Tint> Tints;
	inline unsigned Frame = 0;			// RtCore's frame counter (NoteDraw stamps it)

	inline bool HairShader(const char* pixelShaderName) {	// the names the game's shader package gives them (the tester's log: "SM3003.pso")
		return pixelShaderName && (!strcmp(pixelShaderName, "SM3003.pso") || !strcmp(pixelShaderName, "SM3002.pso"));
	}
	// SetShadersHook: the pixel shader bound now (its record's name, e.g. "SM3003.pso")
	inline void NoteBind(const char* pixelShaderName) { BoundHair = HairShader(pixelShaderName); }

	// After a draw of the world scene with a hair shader bound: the tint it drew with (c2)
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

	// UNOFFICIAL 2026-11-07 (perf-mirrorfix2) at DRAW time (Render.cpp MaterialDraw); (2026-11-08 perf-reflfix4) the still-object draws too
	// (skinned false: the rigid hair, beards and brows draw there, never in the skinned partitions), counted per kind for the log
	inline unsigned DrawNotes = 0;
	inline unsigned HairDraws[2] = { 0, 0 };	// hair-shader draws of the world seen at draw time: [0] still objects (rigid), [1] skinned partitions
	inline char SkinnedShaders[6][16] = {};	// the first pixel shaders the world's skinned partitions drew with (the log)
	inline int SkinnedShaderCount = 0;
	inline void NoteHairDraw(const void* geometry, const char* pixelShader, IDirect3DDevice9* device, bool skinned = true) {
		if (!Wanted || !geometry || !device) return;
		if (skinned && pixelShader && SkinnedShaderCount < 6) {
			bool known = false;
			for (int k = 0; k < SkinnedShaderCount && !known; k++) known = !strncmp(SkinnedShaders[k], pixelShader, sizeof(SkinnedShaders[k]) - 1);
			if (!known) { strncpy_s(SkinnedShaders[SkinnedShaderCount], pixelShader, _TRUNCATE); SkinnedShaderCount++; }
		}
		if (!HairShader(pixelShader)) return;
		const bool saved = BoundHair;
		BoundHair = true;
		NoteDraw(geometry, device);
		BoundHair = saved;
		DrawNotes++;
		HairDraws[skinned ? 1 : 0]++;
	}

	// The tint noted for this geometry within the last `maxAge` frames
	inline bool Find(const void* geometry, float rgb[3], unsigned maxAge = 600u) {
		auto it = Tints.find(geometry);
		if (it == Tints.end() || Frame - it->second.frame > maxAge) return false;
		std::memcpy(rgb, it->second.rgb, sizeof(float) * 3);
		return true;
	}
	// ... at any age while its entry lives (perf-reflfix3)
	inline bool Lookup(const void* geometry, float rgb[3]) {
		auto it = Tints.find(geometry);
		if (it == Tints.end()) return false;
		std::memcpy(rgb, it->second.rgb, sizeof(float) * 3);
		return true;
	}

	inline void Clear() { Tints.clear(); }

	// ---- (2026-11-08 perf-reflfix4) the tint as the game gives it -----------------------------------------------------------------------
	constexpr unsigned kPropertyTintOffset = 0x84;	// BSShaderPPLightingProperty: the NiColorA Lighting30Shader puts in c2 for the hair pass

	// A part the game tints as hair: its shader property has the Hair flag (BSShaderProperty::Hair 0x40000: the vanilla hair, beard and brow
	// NIFs, flags 0x82040101), or its name is one ApplyFaceGenTextures writes the colour for ("FaceGenHair" + "NoHat" / "Hat",
	// "FaceGenAccessory")
	inline bool HairName(const char* name) { return name && (!strncmp(name, "FaceGenHair", 11) || !strcmp(name, "FaceGenAccessory")); }
	// The shader property's class may be read at +0x84: the shader types ApplyFaceGenTextures writes it for (8 BSShaderPPLightingProperty,
	// 12 Lighting30ShaderProperty: GameNi.h kProp_PPLighting / kProp_Lighting30)
	inline bool TintClass(uint32_t shaderType) { return shaderType == 8u || shaderType == 12u; }
	// The colour at the property's +0x84 as the game wrote it: alpha exactly 1 (ApplyFaceGenTextures' RgbaColor(..., 1.0f)), rgb in 0..1
	// (n / 255). A zero alpha: the constructor's zeroes, never written (no colour known from the property).
	inline bool PropertyTint(const float at84[4], float rgb[3]) {
		if (!at84 || !(at84[3] == 1.0f)) return false;
		for (int k = 0; k < 3; k++) if (!(at84[k] >= 0.0f && at84[k] <= 1.0f)) return false;
		rgb[0] = at84[0]; rgb[1] = at84[1]; rgb[2] = at84[2];
		return true;
	}
	// The NPC's packed hair colour (TESNPC::iHairColor, +0x1D8; the ESM's HCLR bytes R, G, B, 0 read as one little-endian word): R in the
	// low byte, as ApplyFaceGenTextures and the race menu's sliders 30 / 31 / 32 read it
	inline void Unpack(uint32_t packed, float rgb[3]) {
		rgb[0] = float(packed & 0xFF) / 255.0f; rgb[1] = float((packed >> 8) & 0xFF) / 255.0f; rgb[2] = float((packed >> 16) & 0xFF) / 255.0f;
	}
	enum Source { None = 0, Property = 1, Noted = 2, PlayerColour = 3 };
	inline const char* SourceName(int s) { return s == Property ? "its shader property (+0x84)" : s == Noted ? "a draw's c2" : s == PlayerColour ? "the player's TESNPC colour" : "none"; }
	// A hair part's tint. at84: the property's 4 floats at +0x84 (nullptr: not a class that has them); hair: HairName or the Hair flag;
	// playerHair: the player's own part (its TESNPC colour `packed` as the last fallback)
	inline int For(const void* geometry, const float* at84, bool hair, bool playerHair, uint32_t packed, float rgb[3]) {
		if (!hair) return None;
		if (PropertyTint(at84, rgb)) return Property;
		if (Lookup(geometry, rgb)) return Noted;
		if (playerHair) { Unpack(packed, rgb); return PlayerColour; }
		return None;
	}
}
