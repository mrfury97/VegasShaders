#pragma once

// UNOFFICIAL 2026-11-06 the RT core's scene picking rules, without the game's types: which geometry RtCore::AddGeometry takes into the
// traced scene (and why it leaves the rest out), and how a taken geometry's instance record, cull mask and build flags are packed.
// RtCore.cpp calls these with what it reads off the NiGeometry; tools/nvr-rt-host (the RT test bench without the game) calls the same
// functions with synthetic inputs and checks every rule and every packed bit (scene "scenepick"), and builds its own records with them.
// Plain C++ only: no Gamebryo, D3D or NVR headers (the host includes this file as is).

#include <stdint.h>
#include <math.h>
#include <string.h>
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
		DecalOrRefraction,	// refraction / fire refraction always; a decal with [Main.Main.RayTracing] Decals or AlphaTested off
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
		bool		refraction = false;			// BSSP_REFRACTION or BSSP_FIRE_REFRACTION (glass, heat haze): never traced
		bool		decal = false;				// (2026-11-07) BSSP_DECAL or BSSP_DYNAMIC_DECAL (posters, cracks, the game's blood splats)
		bool		hasAlphaProperty = false;
		uint16_t	alphaFlags = 0;
		bool		glassRule = false;			// (2026-11-07 ptaaa-materials) its diffuse texture's material rule is the class "glass"
	};
	struct Switches {
		float		cullAngle = 0.0f;			// [Main.Main.RayTracing] CullAngle (0 off)
		bool		alphaTested = false;		// [Main.Main.RayTracing] AlphaTested (and the fork stages texcoords)
		bool		leafCards = false;			// LeafCards (leaf planes > 0)
		bool		decals = false;				// (2026-11-07) [Main.Main.RayTracing] Decals (with AlphaTested)
		bool		glass = false;				// (2026-11-07 ptaaa-materials) PTGlass with the path tracer on (and AlphaTested + the atlas)
	};

	// AddGeometry's tests before the geometry is described; alphaTested: it goes in as an alpha-tested caster; (2026-11-07) glass: it goes
	// in as a glass pane (NVR_RT_KIND_GLASS: non-opaque, mask 0x10; only the path tracer sees it, RtAlpha.glsl)
	inline Fate Classify(const Geometry& g, const Switches& s, bool& alphaTested, bool& glass);
	inline Fate Classify(const Geometry& g, const Switches& s, bool& alphaTested) { bool glass; return Classify(g, s, alphaTested, glass); }
	inline Fate Classify(const Geometry& g, const Switches& s, bool& alphaTested, bool& glass) {
		alphaTested = false;
		glass = false;
		// (2026-11-07 ptaaa-materials) GLASS for the path tracer: what the material rules call glass (VegasShaders.materials.txt: the keyword
		// "glass" in the diffuse texture's name, not fxglass; textures\effects\ is never a rule, so heat haze and other refraction effects
		// stay out) and that the scene left out until now: refraction-flagged or alpha blended without a test. Not decals, not far LOD.
		const bool blendedOnly = g.hasAlphaProperty && (g.alphaFlags & kAlphaBlend) && !(g.alphaFlags & kAlphaTest);
		const bool glassCandidate = s.glass && s.alphaTested && g.glassRule && !g.land && !g.farLod && !g.decal && (g.refraction || blendedOnly);
		if (!g.land && g.hasBound && s.cullAngle > 0.0f && g.distance > g.radius && g.radius < s.cullAngle * g.distance) return Culled;
		if (!g.hasShaderAndData) return NoShader;
		if (g.skinned) return Skinned;
		if (s.leafCards && !g.land && g.hasShaderProperty && g.speedTreeLeaf) return Leaves;
		if (!g.hasShaderProperty || !g.lightingProperty) return NotLighting;
		if (glassCandidate && !g.speedTreeLeaf) { glass = true; return Taken; }
		if (g.refraction) return DecalOrRefraction;
		// (2026-11-07, docs/next/RT-DETAIL-GAPS.md 5) decals go in as alpha-tested geometry (their diffuse alpha the cut-out: DecalAlphaRef,
		// blended ones too), lifted off what they lie on (DecalLift) and tagged NVR_RT_CUSTOM_DECAL: shadow, AO and visibility rays pass
		// them (RtAlpha.glsl rtAlphaPassShadow), reflections, GI and the path tracer's bounces see them
		if (g.decal) {
			if (!s.decals || !s.alphaTested) return DecalOrRefraction;
			alphaTested = true;
			return Taken;
		}
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
		bool		hidden = false;				// (2026-11-07) the player's body in first person: traced, not drawn (NVR_RT_CUSTOM_HIDDEN)
		bool		decal = false;				// (2026-11-07) a decal (alpha-tested; NVR_RT_CUSTOM_DECAL)
		uint32_t	kind = NVR_RT_KIND_PLAIN;	// (2026-11-07 ptaaa-materials) the path tracer's material kind (NvrRtProtocol.h NVR_RT_KIND_*)
		bool		tinted = false;				// (2026-11-08 perf-reflfix4) the game's hair (rigid: hair, beards, brows), with alphaTested only:
												// TINTED as the actors' hair (ActorRecordFlags), its albedo the tint's linear colour
	};

	// (2026-11-07) a decal's alpha reference for the traced test (D3DCMP_GREATER): its own with the alpha test on; blended only (most decals:
	// posters, cracks, blood) 128, where its blend is half opaque; no alpha property 0 (an opaque decal: its whole quad, alpha > 0)
	inline uint32_t DecalAlphaRef(bool hasAlphaProperty, uint16_t alphaFlags, uint32_t alphaTestRef) {
		if (hasAlphaProperty && (alphaFlags & kAlphaTest)) return alphaTestRef & 0xFFu;
		if (hasAlphaProperty && (alphaFlags & kAlphaBlend)) return 128u;
		return 0u;
	}

	// (2026-11-07) how far a decal is lifted along its normal in the traced scene (game units): coplanar with what it lies on, the two would
	// fight for every ray (their hits at the same distance)
	constexpr float kDecalLift = 0.5f;

	// The decal's average normal in its model space: the mean of its vertex normals (count x 3 floats); false: none, or they cancel out
	inline bool AverageNormal(const float* normals, uint32_t count, float out[3]) {
		double n[3] = { 0.0, 0.0, 0.0 };
		for (uint32_t i = 0; normals && i < count; i++) for (int k = 0; k < 3; k++) n[k] += normals[3 * i + k];
		const double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (!(len > 1e-3 * (count ? count : 1))) return false;
		for (int k = 0; k < 3; k++) out[k] = (float)(n[k] / len);
		return true;
	}

	// Moves an instance transform (3 x 4, rows = the scene's axes, column 3 the translation: RtCore's camera-relative transform) `lift` units
	// along the model-space normal turned into scene space by its 3 x 3 (rotation x uniform scale: the normal keeps its direction). The
	// geometry is shared by every copy (GeometryKey), so the lift goes in the instance. false: no usable normal (left in place).
	inline bool DecalLift(float transform[3][4], const float modelNormal[3], float lift) {
		float n[3];
		for (int r = 0; r < 3; r++) n[r] = transform[r][0] * modelNormal[0] + transform[r][1] * modelNormal[1] + transform[r][2] * modelNormal[2];
		const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (!(len > 1e-6f)) return false;
		for (int r = 0; r < 3; r++) transform[r][3] += n[r] / len * lift;
		return true;
	}

	// (2026-11-07 ptaaa-materials) an actor part is SKIN for the path tracer when its diffuse texture lies under textures\characters\ (bodies,
	// heads, hands) and is none of the parts there that are not skin (hair, eyes, teeth, the mouth's inside, brows, beards)
	inline bool SkinTexturePath(const char* path) {
		if (!path) return false;
		char p[260];
		size_t n = 0;
		for (; path[n] && n + 1 < sizeof(p); n++) { const char c = path[n]; p[n] = c == '/' ? '\\' : (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
		p[n] = 0;
		if (!strstr(p, "characters\\")) return false;
		static const char* const kNot[] = { "hair", "eye", "teeth", "tooth", "mouth", "tongue", "brow", "beard", "mustache", "lash" };
		for (const char* word : kNot) if (strstr(p, word)) return false;
		return true;
	}

	// --- (2026-11-07 perf-mirrorfix) skinned actor partitions (RtCore::AddSkinned) -----------------------------------------------------
	// Until now every partition went in OPAQUE (rtsun-13), alpha test or not: hair cards were solid sheets in mirrors (their transparent
	// parts' pale colour a white fringe round the head), and they shadowed the face inside them (the face dark in reflections). Now a
	// partition whose alpha property tests (hair, brows, lashes, beards, eye cards, fringed clothing) and that is not skin goes in
	// ALPHA-TESTED: its record carries NVR_RT_RECORD_ALPHA_TESTED + its reference + its atlas slot, its structure is built non-opaque
	// (NVR_RT_OP_FLAG_ALPHA), and it keeps the actors' mask (0x20): every ray that tests alpha (RtAlpha.glsl rtAlphaPass) cuts it out; rays
	// traced opaque see it solid, as before. Skin (SkinTexturePath: bodies, heads, hands) stays opaque whatever its alpha property says.
	// Hair is tinted by the game (the hair shaders' HairTint, c2: the albedo x 2 x tint; (2026-11-08 perf-reflfix4) read from the part's
	// shader property, RtHairTint.h; the vanilla hair is rigid: RecordFlags' tinted, the same two bits): such a record
	// is TINTED: ALPHA_TESTED and TEXTURED both set (never both otherwise), its albedo rgb the tint's LINEAR colour, which RtHit.glsl
	// rtHitMap multiplies into the texture's colour (x kTintScale = 2^2.2: the game's x 2 in the stored curve); the albedo alone (no
	// texture: the averages-only shaders) is the hair's colour.
	struct ActorPart {
		bool		albedoKnown = false;
		bool		textured = false;		// the build carried texcoords and the diffuse has an atlas slot (hasUv && slot >= 0)
		int			slot = -1;
		bool		normalMap = false;		// RealNormalMaps: the body's normal map
		bool		skin = false;			// SkinTexturePath (the path tracer's skin kind)
		bool		alphaTested = false;	// ActorAlphaTested's answer AND the structure was built non-opaque (the entry's last build)
		uint32_t	alphaRef = 0;
		bool		tinted = false;			// a hair tint was noted for it (with alphaTested only)
		uint32_t	skinGain = 0;			// (perf-mirrorfix2) a FaceGen face's colour gain code (SkinGainCode; skin, not alpha-tested only)
	};
	// (2026-11-07 perf-mirrorfix2, test 10: the face dark in mirrors) the game's FaceGen face is NOT its base texture: the face pass
	// multiplies the frame by 2((2(fg0 - 0.5) + base) 2 fg1) (NewVegas/Hooks/Shaders.cpp: SLS1005, the two FaceGen maps; with neutral
	// maps 2 x base in the stored curve), the traced scene took the base alone. A FaceGen face's record carries the face's colour gain
	// (linear, scalar: FaceGenGain from the three textures' averages, or 2^2.2 where the maps cannot be read) in bits 8-15, free on a
	// record that has no glow and is not alpha-tested (PTEmissive and the alpha reference use them only there): k / 32, k = 1..255.
	// RtHit.glsl rtHitMap multiplies the skin texture's colour by it (record kind skin, flag 0x40, bits 8-15 non-zero).
	constexpr uint32_t kSkinGainShift = 8u;
	constexpr float kSkinGainUnit = 32.0f;
	inline uint32_t SkinGainCode(float gainLinear) {
		if (!(gainLinear > 0.0f)) return 0u;
		int k = (int)(gainLinear * kSkinGainUnit + 0.5f);
		return (uint32_t)(k < 1 ? 1 : k > 255 ? 255 : k);
	}
	inline float SkinGainOf(uint32_t flags) { return float((flags >> kSkinGainShift) & 0xFFu) / kSkinGainUnit; }
	inline float SrgbEncode(float l) { l = l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l; return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f; }
	inline float SrgbDecode(float c) { c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c; return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
	// The face's gain from the LINEAR averages of its base texture and the two FaceGen maps (rgb each; null: unknown -> 2^2.2, the neutral
	// maps' x 2): the face's colour by the game's formula on the stored averages, its luminance over the base's
	inline float FaceGenGain(const float* base, const float* fg0, const float* fg1) {
		if (!base || !fg0 || !fg1) return 4.5947934f;
		static const float w[3] = { 0.2126f, 0.7152f, 0.0722f };
		float num = 0.0f, den = 0.0f;
		for (int c = 0; c < 3; c++) {
			const float b = SrgbEncode(base[c]), a0 = SrgbEncode(fg0[c]), a1 = SrgbEncode(fg1[c]);
			num += w[c] * SrgbDecode(2.0f * ((2.0f * (a0 - 0.5f) + b) * 2.0f * a1));
			den += w[c] * base[c];
		}
		const float g = den > 1.0e-4f ? num / den : 4.5947934f;
		return g < 1.0f / kSkinGainUnit ? 1.0f / kSkinGainUnit : g > 255.0f / kSkinGainUnit ? 255.0f / kSkinGainUnit : g;
	}
	constexpr float kTintScale = 4.5947934f;	// 2^2.2: the hair shaders' albedo x 2 (stored curve) in linear light

	// The partition goes in alpha-tested: its alpha property tests, it is not skin, [Main.Main.RayTracing] AlphaTested is on
	inline bool ActorAlphaTested(bool hasAlphaProperty, uint16_t alphaFlags, bool skin, bool alphaTestedOn) {
		return alphaTestedOn && hasAlphaProperty && (alphaFlags & kAlphaTest) && !skin;
	}
	// Its reference: the property's; blended too (the game's hair: test + blend), at least 128, where the blend is half opaque (the hashed
	// test in RtAlpha.glsl then keeps a texel with the probability of its alpha: the blend's coverage)
	inline uint32_t ActorAlphaRef(uint16_t alphaFlags, uint32_t alphaTestRef) {
		const uint32_t ref = alphaTestRef & 0xFFu;
		return (alphaFlags & kAlphaBlend) && ref < 128u ? 128u : ref;
	}
	inline uint32_t ActorRecordFlags(const ActorPart& a) {
		uint32_t flags = a.albedoKnown ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u;
		if (a.textured) {
			if (a.alphaTested) {
				flags |= NVR_RT_RECORD_ALPHA_TESTED | ((a.alphaRef & 0xFFu) << 8) | ((uint32_t)a.slot << 16);
				if (a.tinted) flags |= NVR_RT_RECORD_TEXTURED;	// TINTED (both bits)
			}
			else flags |= NVR_RT_RECORD_TEXTURED | ((uint32_t)a.slot << 16);
			if (a.normalMap) flags |= NVR_RT_RECORD_NORMAL;
		}
		if (a.skin) flags |= NvrRtKindBits(NVR_RT_KIND_SKIN);
		if (a.skin && a.textured && !a.alphaTested && a.skinGain) flags |= (a.skinGain & 0xFFu) << kSkinGainShift;	// (perf-mirrorfix2)
		return flags;
	}
	// The pose's build: dynamic, its texcoords, non-opaque when alpha-tested (only with texcoords: without them no ray could test it)
	inline uint32_t ActorOpFlags(bool hasUv, bool alphaTested) {
		return NVR_RT_OP_FLAG_DYNAMIC | (hasUv ? NVR_RT_OP_FLAG_TEXCOORDS : 0u) | (hasUv && alphaTested ? NVR_RT_OP_FLAG_ALPHA : 0u);
	}
	inline bool IsTinted(uint32_t flags) {
		return (flags & (NVR_RT_RECORD_ALPHA_TESTED | NVR_RT_RECORD_TEXTURED)) == (NVR_RT_RECORD_ALPHA_TESTED | NVR_RT_RECORD_TEXTURED);
	}
	// The tint's linear colour for the record from the shader's HairTint (c2, the stored curve, 0..1)
	inline float TintLinear(float c) { c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c; return powf(c, 2.2f); }

	// The instance's custom index (NvrRtProtocol.h NVR_RT_CUSTOM_*): land, hidden (the first-person body), decal
	inline uint32_t CustomIndex(const Record& r) {
		return (r.land ? NVR_RT_CUSTOM_LAND : 0u) | (r.hidden ? NVR_RT_CUSTOM_HIDDEN : 0u) | (r.decal ? NVR_RT_CUSTOM_DECAL : 0u);
	}

	inline uint32_t RecordFlags(const Record& r) {
		uint32_t flags = (r.land ? NVR_RT_RECORD_LAND : 0u) | (r.albedoKnown ? NVR_RT_RECORD_ALBEDO_KNOWN : 0u) | (r.glowMap ? NVR_RT_RECORD_GLOW : 0u);
		if (r.alphaTested) flags |= NVR_RT_RECORD_ALPHA_TESTED | ((r.alphaRef & 0xFFu) << 8) | ((uint32_t)r.slot << 16);
		else if (r.textured) flags |= NVR_RT_RECORD_TEXTURED | ((uint32_t)r.slot << 16);
		if (r.alphaTested && r.tinted) flags |= NVR_RT_RECORD_TEXTURED;	// (2026-11-08 perf-reflfix4) TINTED (both bits): the rigid hair
		if ((r.alphaTested || r.textured) && r.normalMap) flags |= NVR_RT_RECORD_NORMAL;
		if ((r.alphaTested || r.textured) && r.materialRule) {	// the material rule's roughness / metal in bits 26-31 (NvrRtProtocol.h)
			if (r.rough >= 0.0f) flags |= NvrRtRoughCode(r.rough) << NVR_RT_RECORD_ROUGH_SHIFT;	// (2026-11-07 perf-reflfix2: (k / 7)^2, finer near 0)
			if (r.metal >= 0.0f) { int k = (int)(r.metal * 6.0f + 0.5f) + 1; k = k > 7 ? 7 : k; flags |= (uint32_t)k << NVR_RT_RECORD_METAL_SHIFT; }
		}
		flags |= NvrRtKindBits(r.kind & 3u);	// (2026-11-07 ptaaa-materials) bits 0x02 / 0x08
		return flags;
	}

	// UNOFFICIAL 2026-11-07 (ptaaa-emissive) PTEmissive's number in a record (RtEmissiveRow.h): bits 8-15 = the emissive instance's number + 1
	// (1-255; 0 none) on a glow record that is NOT alpha-tested (there they hold the alpha reference); the record is returned unchanged
	// where they are taken or the record has no glow
	inline bool CanBeEmissive(uint32_t flags) { return (flags & (NVR_RT_RECORD_GLOW | NVR_RT_RECORD_ALPHA_TESTED)) == NVR_RT_RECORD_GLOW; }
	inline uint32_t WithEmissive(uint32_t flags, uint32_t number) {
		if (!CanBeEmissive(flags) || number > 255u) return flags;
		return (flags & ~0xFF00u) | (number << 8);
	}

	inline uint32_t InstanceMask(const Record& r) {
		uint32_t mask = r.hookMask >= 0 ? ((uint32_t)r.hookMask & 0xFFu) : (r.land ? 0x01u : 0x02u);	// default: bit 0 land, bit 1 objects
		// (2026-11-07 ptaaa-materials) glass: non-opaque under the alpha bit, so a ray either tests it (RtAlpha.glsl: every shader but the path
		// tracer passes it) or leaves it out (a mask without 0x10)
		if (r.alphaTested || r.kind == NVR_RT_KIND_GLASS) mask = NVR_RT_MASK_ALPHA;	// only rays that test alpha (RtAlpha.glsl) see it
		else if (r.player) mask = NVR_RT_MASK_PLAYER;	// the player's rigid parts: every ray sees them but the gun's
		else if (r.farLod) mask = NVR_RT_MASK_FAR;		// far object LOD (the skyline for reflections)
		return mask;
	}

	inline uint32_t OpFlags(const Record& r) {
		uint32_t flags = r.alphaTested ? (NVR_RT_OP_FLAG_TEXCOORDS | NVR_RT_OP_FLAG_ALPHA) : r.textured ? NVR_RT_OP_FLAG_TEXCOORDS : 0u;
		if (r.kind == NVR_RT_KIND_GLASS) flags |= NVR_RT_OP_FLAG_ALPHA;	// (2026-11-07 ptaaa-materials) built non-opaque: the rays see its candidates
		if (r.compact) flags |= NVR_RT_OP_FLAG_COMPACT;
		return flags;
	}
}
