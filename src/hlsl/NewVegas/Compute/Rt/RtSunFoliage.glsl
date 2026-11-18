// UNOFFICIAL 2026-11-12 LeafTransmission / LeafSoftEdges ([Shaders.ShadowsExteriors.RT], core\RtSunShadows.cpp): the sun ray's own
// trace through foliage, for RtSunShadow.comp ONLY (lamp shadows, AO, GI, reflections and the path tracer keep RtAlpha.glsl as it is).
//
// What "foliage" is: a record of the material kind 2 (NvrRtProtocol.h NVR_RT_KIND_FOLIAGE: flag bits 0x02 clear, 0x08 set) that is
// alpha-tested (0x20). RtCore.cpp sets it on SpeedTree leaf cards (AddLeaves), grass tufts (AddGrass) and every cut-out mesh whose
// diffuse texture's material rule is the class "plant" (AddGeometry; VegasShaders.materials.txt: textures\landscape\plants\,
// \landscape\trees\, \trees\, \plants\). Trunks and branches are not cut out: plain, solid as before. Fences, wires, hair: not plants,
// solid where their alpha passes, as before.
//
// LeafTransmission (leafT > 0): a leaf the ray does NOT pass (its alpha covers the ray) lets leafT of the sunlight through and the ray
// goes on: one leaf = a faint shadow (leafT), a canopy = leafT ^ layers, anything solid behind = 0. The loop is bounded twice: the
// usual 256 candidates, and once less than kRtSunFoliageFloor of the light is left the ray counts as blocked and ends.
// LeafSoftEdges (soft): a plant's alpha test is the hashed one at EVERY mip (RtAlpha.glsl uses the plain test at a real texture's
// finest mips) and its hash changes each frame, so a leaf's edge blocks its alpha's share of the rays and the denoiser / TAA give a
// soft edge instead of a hard cut.
//
// Why its own loop: (1) a covered leaf is REJECTED here (not confirmed), a reason other than the alpha test, so RtAlpha.glsl's rule
// applies: with opacity micromaps the surely-solid parts of a leaf commit in hardware and would stop the ray; the query is therefore
// traced with gl_RayFlagsNoOpaqueEXT and every hit comes as a candidate (opaque geometry is confirmed at once: any hit ends a shadow
// ray). (2) candidates are not sorted by distance: a leaf BEHIND a wall is counted too, which does not matter (the wall gives 0).
// The same loop passes over the hidden first-person body for rays near the eye (RtHidden.glsl's rule, one query instead of its two).
//
// rtSunFoliageSoftPass is a COPY of the alpha-tested part of RtAlpha.glsl rtAlphaPass (2026-11-12 state: the atlas texel, the real
// texture's alpha, the hashed reference), without the plain test at the finest mip and with the frame in the hash. If rtAlphaPass's
// texture lookup changes, change it here too.
//
// Needs: `scene`, RtAlpha.glsl (records, words, rtAlphaPass, rtIsDecal, rtIsGlass, rtRecordKind, RT_ALPHA_ATLAS) and RtHidden.glsl
// (NVR_RT_CUSTOM_HIDDEN) included first.
#ifndef NVR_RT_SUN_FOLIAGE_GLSL
#define NVR_RT_SUN_FOLIAGE_GLSL

const float kRtSunFoliageFloor = 1.0 / 64.0;   // less sunlight than this left after the leaves: blocked (0.4: 5 leaves, 0.25: 4)

// a plant cut-out (see the top): kind 2 and alpha-tested (LeafCardsOpaque's debug cards are kind 2 without the test: solid)
bool rtSunIsFoliage(uint flags) { return rtRecordKind(flags) == 2u && (flags & RT_RECORD_ALPHA_TESTED) != 0u; }

// LeafSoftEdges: an alpha-tested record's candidate, hashed at every mip, a new hash each frame. True: solid for this ray.
bool rtSunFoliageSoftPass(uint instance, uint primitive, vec2 bary, float frame) {
	const uint flags = records[instance].flags;
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const uint indexOffset = records[instance].indexOffset;
	const uvec3 i = uvec3(words[indexOffset + 3u * primitive], words[indexOffset + 3u * primitive + 1u], words[indexOffset + 3u * primitive + 2u]);
	const vec2 uv = rtTexcoord(uvOffset, i.x) * (1.0 - bary.x - bary.y) + rtTexcoord(uvOffset, i.y) * bary.x + rtTexcoord(uvOffset, i.z) * bary.y;
	const uint slot = (flags >> 16) & 0x3FFu;
	const int slotSize = imageSize(RT_ALPHA_ATLAS).x / int(RT_ALPHA_SLOTS_A_ROW);
	const ivec2 origin = ivec2(int(slot % RT_ALPHA_SLOTS_A_ROW), int(slot / RT_ALPHA_SLOTS_A_ROW)) * slotSize;
	const ivec2 texel = origin + clamp(ivec2(fract(uv) * float(slotSize)), ivec2(0), ivec2(slotSize - 1));
	float alpha = imageLoad(RT_ALPHA_ATLAS, texel).r;
#ifndef RT_NO_REAL_TEXTURES
	{
		const uint id = rtTexSlots[slot].x;
		if (id != 0u) alpha = rtTexSample(id, uv, rtTexLod(id, instance, primitive)).a;   // (bilinear: a leaf's edge is a ramp of alpha)
	}
#endif
	// the reference dithered as rtAlphaPass does it (uniform over 0..1 at a reference of 0.5, narrower toward 0 or 1: alpha 0 never
	// blocks), so the share of rays an edge texel blocks follows its alpha and the cut stays centred on the game's reference
	const float ref = float((flags >> 8) & 0xFFu) / 255.0;
	const float width = min(1.0, 2.0 * min(ref, 1.0 - ref));
	uvec3 hh = uvec3(floatBitsToUint(bary.x), floatBitsToUint(bary.y), primitive * 747796405u + instance + uint(frame) * 2654435761u);
	hh = hh * 1664525u + 1013904223u; hh.x += hh.y * hh.z; hh.y += hh.z * hh.x; hh.z += hh.x * hh.y; hh ^= hh >> 16u; hh.x += hh.y * hh.z;
	const float dither = float(hh.x >> 8) * (1.0 / 16777216.0) - 0.5;
	return alpha > ref + dither * width;
}

// The sun ray origin + dir * [0, tMax] over the instances of `mask` (with 0x10: the caller tests alpha). Returns the share of the
// sunlight that arrives: 1 free, 0 blocked (something solid, or the leaves left less than kRtSunFoliageFloor), between: through leaves.
// tHit: the solid hit's distance (any hit, not always the nearest, as the plain shadow query), else the nearest leaf gone through,
// else 0. passHidden: the ray starts within reach of the eye (rtHiddenNear) and passes over the hidden first-person body.
// leafT 0 = leaves are solid (then only `soft` differs from the plain query).
float rtSunFoliageTrace(vec3 origin, vec3 dir, float tMax, uint mask, bool passHidden, float leafT, bool soft, float frame, out float tHit) {
	tHit = 0.0;
	float through = 1.0, nearLeaf = 1.0e30;
	bool dense = false;
	rayQueryEXT q;
	rayQueryInitializeEXT(q, scene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsNoOpaqueEXT, mask, origin, 0.0, dir, tMax);
	for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) {
		if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; }
		if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
		const uint ci = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);
		if (passHidden && (ci & NVR_RT_CUSTOM_HIDDEN) != 0u) continue;
		const uint instance = uint(rayQueryGetIntersectionInstanceIdEXT(q, false));
		if (rtIsDecal(ci) || rtIsGlass(instance)) continue;   // (rtAlphaPassShadow: decals and glass never block a shadow ray)
		const uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
		const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(q, false);
		const bool leaf = rtSunIsFoliage(records[instance].flags);
		if (!(leaf && soft ? rtSunFoliageSoftPass(instance, primitive, bary, frame) : rtAlphaPass(instance, primitive, bary))) continue;
		if (!leaf || leafT <= 0.0) { rayQueryConfirmIntersectionEXT(q); continue; }   // solid: the query ends (first hit)
		through *= leafT;
		nearLeaf = min(nearLeaf, rayQueryGetIntersectionTEXT(q, false));
		if (through < kRtSunFoliageFloor) { dense = true; rayQueryTerminateEXT(q); break; }
	}
	if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) { tHit = rayQueryGetIntersectionTEXT(q, true); return 0.0; }
	if (through < 1.0) tHit = nearLeaf;
	return dense ? 0.0 : through;
}

#endif
