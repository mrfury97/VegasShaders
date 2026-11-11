// UNOFFICIAL 2026-10-29 alpha-tested casters in the ray tracing scene ([Main.Main.RayTracing] AlphaTested; FORK-RT-CORE.md): for any
// trace shader in the helper. Alpha-tested instances are non-opaque, have the cull mask bit 0x10 and the record flag 0x20; their
// texcoords (float2) are in the geometry data at the word offset floatBitsToUint(albedo.w), their diffuse alpha in the alpha atlas
// (R16F, 64 x 64 texels a texture, 32 slots a row; slot = (flags >> 16) & 0x3FF), the reference in (flags >> 8) & 0xFF.
//
// Before including: #define RT_ALPHA_ATLAS <the atlas: a layout(r16f) readonly image2D>. The records (binding 10) and the geometry
// data (binding 11) are declared here as records[] / words[], unless RT_RECORDS_DECLARED is defined (then the includer's records[]
// must have .albedo, .flags and .indexOffset, its words[] the uint data).
// Use: trace WITHOUT gl_RayFlagsOpaqueEXT, and for each candidate:
//   while (rayQueryProceedEXT(q))
//     if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
//         rtAlphaPass(rayQueryGetIntersectionInstanceIdEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false),
//                     rayQueryGetIntersectionBarycentricsEXT(q, false)))
//       rayQueryConfirmIntersectionEXT(q);
// (Opaque geometry never shows up as a candidate: it commits by itself.)
// (2026-11-07, ptaaa-omm) With VK_EXT_opacity_micromap the helper gives alpha-tested geometry opacity micromaps built from THIS function:
// micro-triangles where rtAlphaPass is true everywhere (every dither, the mips it can read) commit by themselves in hardware too, the rest
// still come here (tools/nvr-rt-helper/NvrRtOmm.inl). Changing this file or RtTextures.glsl: rebuild tools/nvr-rt-helper/RtOmm.h (its
// check calls rtAlphaPass) and re-check the builder's bounds (RtOmm.comp, NvrRtOmmMath.h). Never trace with
// gl_RayFlagsForceOpacityMicromap2StateEXT; a loop that rejects a candidate for another reason than this test traces with NoOpaque.

#ifndef RT_RECORDS_DECLARED
struct RtRecord { vec4 albedo; uint flags; uint geometryId; uint vertexOffset; uint indexOffset; };
layout(set = 0, binding = 10, std430) readonly buffer RtRecords { RtRecord records[]; };
layout(set = 0, binding = 11, std430) readonly buffer RtGeometry { uint words[]; };
#define RT_RECORDS_DECLARED
#endif

#include "RtTextures.glsl"	// (2026-11-07, protocol 7) the real textures (set 1): the full-size alpha where the game sent it

#define RT_RECORD_ALPHA_TESTED 0x20u
#define RT_ALPHA_SLOT_SIZE 64	// (the atlas' slot size is read off the image since 2026-11-06: AlphaDetail; this is the colour atlases' size)
#define RT_ALPHA_SLOTS_A_ROW 32u

vec2 rtTexcoord(uint uvOffset, uint vertex) {
	return vec2(uintBitsToFloat(words[uvOffset + 2u * vertex]), uintBitsToFloat(words[uvOffset + 2u * vertex + 1u]));
}

// UNOFFICIAL 2026-11-07 (ptaaa-materials) glass panes (record kind 1, NvrRtProtocol.h NVR_RT_RECORD_KIND_*: bits 0x02 low, 0x08 high) are
// non-opaque instances under mask 0x10 that only the path tracer sees: every other shader's candidate test passes them (as if the glass
// were not in the scene, as before). The path tracer defines RT_ALPHA_GLASS_SOLID (an expression: whether this ray meets glass) before
// including this file.
uint rtRecordKind(uint flags) { return ((flags >> 1u) & 1u) | ((flags >> 2u) & 2u); }
bool rtIsGlass(uint instance) { return rtRecordKind(records[instance].flags) == 1u; }

// True: this candidate hit is solid (its alpha passes the material's test). Not alpha-tested: always true.
bool rtAlphaPass(uint instance, uint primitive, vec2 bary) {
	const uint flags = records[instance].flags;
	if (rtRecordKind(flags) == 1u) {
#ifdef RT_ALPHA_GLASS_SOLID
		return RT_ALPHA_GLASS_SOLID;
#else
		return false;
#endif
	}
	if ((flags & RT_RECORD_ALPHA_TESTED) == 0u)
		return true;
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const uint indexOffset = records[instance].indexOffset;
	const uvec3 i = uvec3(words[indexOffset + 3u * primitive], words[indexOffset + 3u * primitive + 1u], words[indexOffset + 3u * primitive + 2u]);
	const vec2 uv = rtTexcoord(uvOffset, i.x) * (1.0 - bary.x - bary.y) + rtTexcoord(uvOffset, i.y) * bary.x + rtTexcoord(uvOffset, i.z) * bary.y;
	const uint slot = (flags >> 16) & 0x3FFu;	// (2026-11-06) bits 16-25: bits 26-31 are the material rule since 11-03 (an unmasked slot read alpha 0 off the atlas: every ray passed the mesh)
	// (2026-11-06) the slot's size off the atlas itself: [Main.Main.RayTracing] AlphaDetail (64 / 128 / 256 texels a texture) sets the
	// atlas' size (32 slots a row; 2048 / 4096 / 8192 are whole multiples of the core's 256 rounding), no shader rebuild
	const int slotSize = imageSize(RT_ALPHA_ATLAS).x / int(RT_ALPHA_SLOTS_A_ROW);
	const ivec2 origin = ivec2(int(slot % RT_ALPHA_SLOTS_A_ROW), int(slot / RT_ALPHA_SLOTS_A_ROW)) * slotSize;
	const ivec2 texel = origin + clamp(ivec2(fract(uv) * float(slotSize)), ivec2(0), ivec2(slotSize - 1));
	float alpha = imageLoad(RT_ALPHA_ATLAS, texel).r;
#ifndef RT_NO_REAL_TEXTURES
	{	// (2026-11-07) the real texture's alpha (full size: wires, leaves and hair edges as the game draws them)
		const uint id = rtTexSlots[slot].x;
		if (id != 0u) {
			const float lod = rtTexLod(id, instance, primitive);
			alpha = rtTexSample(id, uv, lod).a;
			// at the full texture's own detail (the finest mips) the game's plain test is exact: no dither (that is for averaged alpha)
#ifdef RT_ALPHA_BLEND_TINTED   // (an expression, as RT_ALPHA_GLASS_SOLID: true = hair's alpha is dithered at the finest mip too)
			// (2026-11-10, the path tracer) hair (a TINTED record: alpha-tested + textured, 0x60) is BLENDED by the game, not cut: a beard texel
			// at alpha 0.3 covers 30 % there. The hard cut at the reference left beards (47 % of their texels under 128) and eyebrows (70 %)
			// thin or gone for every camera ray; the dithered test below lets a texel block its alpha's share of the rays.
			if (lod < 0.5 && !((RT_ALPHA_BLEND_TINTED) && (flags & 0x60u) == 0x60u)) return alpha > float((flags >> 8) & 0xFFu) / 255.0;
#else
			if (lod < 0.5) return alpha > float((flags >> 8) & 0xFFu) / 255.0;
#endif
		}
	}
#endif
	const float ref = float((flags >> 8) & 0xFFu) / 255.0;
#ifndef RT_ALPHA_PLAIN
	// (2026-11-06) hashed alpha test: the slot holds the texture's mip at 64 texels (RtAlphaToAtlas), the AVERAGE alpha of an 8 x 8 block
	// of a 512 texture, so thin cut-outs (chain-link wires, fence slats, grass blades) fall under the reference everywhere and vanish
	// from every ray (the fence's traced lamp shadow was dots or nothing; host scene `chainlink`). The reference is dithered per ray
	// instead: a texel whose alpha is the share of solid texels it covers blocks that share of the rays, so the shadow keeps its
	// density and its coarse pattern, and the denoiser / TAA turn the stipple into the soft shadow. The dither's width shrinks toward
	// a reference of 0 or 1 (those stay the plain test: alpha > 0 or never); at 0.5 the threshold is uniform over 0..1.
	const float width = min(1.0, 2.0 * min(ref, 1.0 - ref));
	uvec3 hh = uvec3(floatBitsToUint(bary.x), floatBitsToUint(bary.y), primitive * 747796405u + instance);
	hh = hh * 1664525u + 1013904223u; hh.x += hh.y * hh.z; hh.y += hh.z * hh.x; hh.z += hh.x * hh.y; hh ^= hh >> 16u; hh.x += hh.y * hh.z;
	const float dither = float(hh.x >> 8) * (1.0 / 16777216.0) - 0.5;
	return alpha > ref + dither * width;
#else
	return alpha > ref;	// D3DCMP_GREATER, the game's alpha test (before 2026-11-06: a 64-texel mip loses thin cut-outs)
#endif
}

// UNOFFICIAL 2026-11-07 (rtgap-decals, docs/next/RT-DETAIL-GAPS.md 5) decals ([Main.Main.RayTracing] Decals: posters, cracks, the game's
// blood) are alpha-tested instances with the custom index bit NVR_RT_CUSTOM_DECAL, lifted 0.5 unit off what they lie on. Thin and flat on
// a surface, they must not shadow or occlude it: shadow, AO and visibility rays test their candidates with this instead of rtAlphaPass
// (customIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)); reflections, GI and path tracer bounces keep rtAlphaPass and
// see them. Such rays must not be traced with gl_RayFlagsOpaqueEXT over mask 0x10 (a decal would be a solid quad there).
#define RT_CUSTOM_DECAL 4u
bool rtIsDecal(uint customIndex) { return (customIndex & RT_CUSTOM_DECAL) != 0u; }
bool rtAlphaPassShadow(uint customIndex, uint instance, uint primitive, vec2 bary) {
	return !rtIsDecal(customIndex) && !rtIsGlass(instance) && rtAlphaPass(instance, primitive, bary);	// (2026-11-07: glass never blocks them)
}
