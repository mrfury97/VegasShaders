#pragma once

// UNOFFICIAL 2026-11-06 the RT core's scene picking rules, without the game's types: which geometry RtCore::AddGeometry takes into the
// traced scene (and why it leaves the rest out), and how a taken geometry's instance record, cull mask and build flags are packed.
// RtCore.cpp calls these with what it reads off the NiGeometry; tools/nvr-rt-host (the RT test bench without the game) calls the same
// functions with synthetic inputs and checks every rule and every packed bit (scene "scenepick"), and builds its own records with them.
// Plain C++ only: no Gamebryo, D3D or NVR headers (the host includes this file as is).

#include <stdint.h>
#include "../../tools/nvr-rt-helper/NvrRtProtocol.h"

namespace RtScenePick {

	// Why a geometry is (not) in the scene, in the order AddGeometry tests them
	enum Fate {
		Taken = 0,
		Culled,				// small at its distance (CullAngle)
		NoShader,			// no shader / buffer data
		Skinned,			// skinned, not walked as an actor
		Leaves,				// a SpeedTree leaf with LeafCards on: AddLeaves takes it
		NotLighting,		// no shader property, or not a lighting shader
		DecalOrRefraction,
		SpeedTreeLeaf,		// a SpeedTree leaf without LeafCards
		BlendedOnly,		// alpha blended, no alpha test (glass, fx)
		AlphaOff,			// alpha-tested with [Main.Main.RayTracing] AlphaTested off
	};

	// NiAlphaProperty::AlphaFlags (GameNi.h)
	constexpr uint16_t kAlphaBlend = 0x0001, kAlphaTest = 0x0200;

	struct Geometry {
		bool		land = false;
		bool		farLod = false;				// walked as far object LOD (FarObjectLOD)
		bool		hasBound = false;			// objects: a world bound (its centre's distance from the camera, its radius)
		float		distance = 0.0f, radius = 0.0f;
		bool		hasShaderAndData = true;	// geo->shader, geomData and its buffer data
		bool		skinned = false;
		bool		hasShaderProperty = true;
		bool		lightingProperty = true;	// BSShaderProperty::IsLightingProperty
		bool		speedTreeLeaf = false;		// the shade property's type is kProp_SpeedTreeLeaf
		bool		decalOrRefraction = false;	// BSSP_REFRACTION, BSSP_FIRE_REFRACTION, BSSP_DECAL or BSSP_DYNAMIC_DECAL
		bool		hasAlphaProperty = false;
		uint16_t	alphaFlags = 0;
	};
	struct Switches {
		float		cullAngle = 0.0f;			// [Main.Main.RayTracing] CullAngle (0 off)
		bool		alphaTested = false;		// [Main.Main.RayTracing] AlphaTested (and the fork stages texcoords)
		bool		leafCards = false;			// LeafCards (leaf planes > 0)
	};

	// AddGeometry's tests before the geometry is described; alphaTested: it goes in as an alpha-tested caster
	inline Fate Classify(const Geometry& g, const Switches& s, bool& alphaTested) {
		alphaTested = false;
		if (!g.land && g.hasBound && s.cullAngle > 0.0f && g.distance > g.radius && g.radius < s.cullAngle * g.distance) return Culled;
		if (!g.hasShaderAndData) return NoShader;
		if (g.skinned) return Skinned;
		if (s.leafCards && !g.land && g.hasShaderProperty && g.speedTreeLeaf) return Leaves;
		if (!g.hasShaderProperty || !g.lightingProperty) return NotLighting;
		if (g.decalOrRefraction) return DecalOrRefraction;
		if (g.speedTreeLeaf) return SpeedTreeLeaf;
		// Far object LOD carries an alpha property for its distance fade (blend only): it goes in opaque. Far tree / billboard LOD is cut out
		// (test on): the alpha-tested path like near geometry.
		if (g.hasAlphaProperty && (!g.farLod || (g.alphaFlags & kAlphaTest)) && (g.alphaFlags & (kAlphaBlend | kAlphaTest))) {
			if (!(g.alphaFlags & kAlphaTest)) return BlendedOnly;
			if (!s.alphaTested) return AlphaOff;
			alphaTested = true;
		}
		return Taken;
	}

	// A taken geometry's record, mask and build flags
	struct Record {
		bool		land = false;
		bool		albedoKnown = false;
		bool		glowMap = false;
		bool		alphaTested = false;
		uint32_t	alphaRef = 0;				// NiAlphaProperty::alphaTestRef (0-255)
		bool		textured = false;			// HitTextures: an atlas slot for its diffuse texture
		int			slot = -1;					// the atlas slot (alpha-tested or textured), 0-1023
		bool		normalMap = false;			// NormalTextures: the normal atlas exists and the draw has a normal map
		bool		materialRule = false;		// RtMaterialRuleOf found a rule
		float		rough = -1.0f, metal = -1.0f;	// the rule's (negative: none)
		int			hookMask = -1;				// RtCore::SetMaskHook's answer (-1: no hook)
		bool		player = false;				// walked as the player's rigid parts
		bool		farLod = false;				// walked as far object LOD
		bool		compact = false;			// [Main.Main.RayTracing] Compact
	};

	inline uint32_t RecordFlags(const Record& r) {
		uint32_t flags = (r.land ? NVR_RT_RECORD_LAND : 0u) | (r.albedoKnown ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | (r.glowMap ? NVR_RT_RECORD_GLOW : 0u);
		if (r.alphaTested) flags |= NVR_RT_RECORD_ALPHA_TESTED | ((r.alphaRef & 0xFFu) << 8) | ((uint32_t)r.slot << 16);
		else if (r.textured) flags |= NVR_RT_RECORD_TEXTURED | ((uint32_t)r.slot << 16);
		if ((r.alphaTested || r.textured) && r.normalMap) flags |= NVR_RT_RECORD_NORMAL;
		if ((r.alphaTested || r.textured) && r.materialRule) {	// the material rule's roughness / metal in bits 26-31 (NvrRtProtocol.h)
			if (r.rough >= 0.0f) { int k = (int)(r.rough * 7.0f + 0.5f); k = k < 1 ? 1 : k > 7 ? 7 : k; flags |= (uint32_t)k << NVR_RT_RECORD_ROUGH_SHIFT; }
			if (r.metal >= 0.0f) { int k = (int)(r.metal * 6.0f + 0.5f) + 1; k = k > 7 ? 7 : k; flags |= (uint32_t)k << NVR_RT_RECORD_METAL_SHIFT; }
		}
		return flags;
	}

	inline uint32_t InstanceMask(const Record& r) {
		uint32_t mask = r.hookMask >= 0 ? ((uint32_t)r.hookMask & 0xFFu) : (r.land ? 0x01u : 0x02u);	// default: bit 0 land, bit 1 objects
		if (r.alphaTested) mask = NVR_RT_MASK_ALPHA;	// only rays that test alpha (RtAlpha.glsl) see it
		else if (r.player) mask = NVR_RT_MASK_PLAYER;	// the player's rigid parts: every ray sees them but the gun's
		else if (r.farLod) mask = NVR_RT_MASK_FAR;		// far object LOD (the skyline for reflections)
		return mask;
	}

	inline uint32_t OpFlags(const Record& r) {
		uint32_t flags = r.alphaTested ? (NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA) : r.textured ? NVR_RT_OP_FLAG_TEXCOORDS : 0u;
		if (r.compact) flags |= NVR_RT_OP_FLAG_COMPACT;
		return flags;
	}
}
