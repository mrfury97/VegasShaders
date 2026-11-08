// UNOFFICIAL 2026-11-07 (ptaaa-firstperson; docs/next/RT-PATH-TRACING.md "ptaaa-firstperson") the player's own body in first person
// (FirstPersonBody): traced (its shadow on the ground and walls, its reflection in mirrors), not drawn. RtCore gives its instances the
// custom index bit NVR_RT_CUSTOM_HIDDEN (2) while walking the player's node with !isThirdPerson. Its pose is the THIRD-person one: the
// head round the eye, the third-person arms and gun near the chest, nowhere near the first-person arms and gun the screen shows.
//
// The rule (every shadow / visibility / occlusion ray of the passes that include this): a ray that STARTS or ENDS within
// kRtHiddenNear units of the eye (an arm's length, ~1.1 m) passes over the hidden body. That is:
//   - the first-person view model (gun, hands, Pip-Boy): its pixels lie a few tens of units in front of the eye, inside or next to the
//     third-person head, arms and gun, which shadowed and occluded it (a gun dark under the sun from behind, black RT AO on the hands);
//   - lamps carried at the eye (the Pip-Boy light, the flashlight moved onto the beam, the path tracer's flashlight lamp 0): inside the
//     third-person head every ray toward them met the body, and their light came through only the gaps of the mesh (neck, hat brim,
//     eye holes) as a lit pattern in their shadow: the "reverse shadow";
//   - surfaces within arm's reach (a counter you stand at, a wall in front of your face): the third-person arms and gun poke into them
//     in a pose the first-person view never shows.
// Rays that start and end further away (the ground at your feet ~120 units below the eye, the room's walls, the sun's rays from them) still
// meet the body: its shadow and reflection stay. In third person nothing is hidden: the rule changes nothing (the same answer, two
// queries instead of one, only for rays near the camera).
//
// Needs: `scene`, RtAlpha.glsl (rtAlphaPassShadow) included first.
#ifndef NVR_RT_HIDDEN_GLSL
#define NVR_RT_HIDDEN_GLSL

#define NVR_RT_CUSTOM_HIDDEN 2u   // NvrRtProtocol.h: the player's own body and gear in first person (traced, not drawn)
const float kRtHiddenNear = 80.0;   // game units from the eye (FNV: ~1.4 cm a unit; the eye stands ~120 above the feet)

// a point (scene space) within reach of the eye
bool rtHiddenNear(vec3 at, vec3 eye) { const vec3 d = at - eye; return dot(d, d) < kRtHiddenNear * kRtHiddenNear; }
// the ray's start or end within reach of the eye: it passes over the hidden body
bool rtHiddenPassOver(vec3 from, vec3 to, vec3 eye) { return rtHiddenNear(from, eye) || rtHiddenNear(to, eye); }

// A shadow ray over [tMin, tMax] of origin + dir t that passes over NVR_RT_CUSTOM_HIDDEN instances. Two queries (as the path tracer's
// camera rays, RT-PATH-TRACING.md section 51): the instances that are never people or cut-outs (mask & 0x8F: land, objects, glass, lamp
// models, far LOD; opaque), then people, the player's parts and cut-outs (mask & 0x70) up to that hit, every candidate tested: hidden
// ones passed over, cut-outs by their texture when testAlpha (decals never block: rtAlphaPassShadow), the rest solid.
// mode: kRtAnyHit = any hit blocks (a first-hit query: tHit is that hit's t, not always the nearest); kRtOneSided = the nearest hit
// decides, a front face (facing the ray's start) does not block (RtLampTrace's OneSided); kRtClosest = the nearest hit blocks, tHit is
// its t (RtLampTrace SIGMA's closestBlocker). Returns true when blocked; tHit -1 when free. (Callers that do not test alpha leave 0x10
// out of the mask themselves.)
const int kRtAnyHit = 0, kRtOneSided = 1, kRtClosest = 2;
bool rtBlockedPassHidden(vec3 origin, vec3 dir, float tMin, float tMax, uint mask, bool testAlpha, int mode, out float tHit) {
	tHit = -1.0;
	if (!(tMax > tMin)) return false;
	const uint terminate = mode == kRtAnyHit ? gl_RayFlagsTerminateOnFirstHitEXT : 0u;
	float tNear = tMax;
	bool hit = false, front = false;
	const uint m1 = mask & 0x8Fu;
	if (m1 != 0u) {
		rayQueryEXT q;
		rayQueryInitializeEXT(q, scene, gl_RayFlagsOpaqueEXT | terminate, m1, origin, tMin, dir, tMax);
		for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; } else {}
		if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
			tNear = rayQueryGetIntersectionTEXT(q, true);
			if (mode == kRtAnyHit) { tHit = tNear; return true; }
			hit = true; front = rayQueryGetIntersectionFrontFaceEXT(q, true);
		}
	}
	const uint m2 = mask & 0x70u;
	if (m2 != 0u) {
		rayQueryEXT q;
		rayQueryInitializeEXT(q, scene, gl_RayFlagsNoOpaqueEXT | terminate, m2, origin, tMin, dir, tNear);
		for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; } else {
			if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
			const uint ci = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);
			if ((ci & NVR_RT_CUSTOM_HIDDEN) != 0u) continue;
			if (!testAlpha || rtAlphaPassShadow(ci, rayQueryGetIntersectionInstanceIdEXT(q, false), uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)),
			                                    rayQueryGetIntersectionBarycentricsEXT(q, false)))
				rayQueryConfirmIntersectionEXT(q);
		}
		if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
			tNear = rayQueryGetIntersectionTEXT(q, true);   // (nearer than the first query's hit: that was its tMax)
			if (mode == kRtAnyHit) { tHit = tNear; return true; }
			hit = true; front = rayQueryGetIntersectionFrontFaceEXT(q, true);
		}
	}
	if (hit && (mode == kRtClosest || !front)) { tHit = tNear; return true; }
	return false;
}
bool rtBlockedPassHidden(vec3 origin, vec3 dir, float tMin, float tMax, uint mask, bool testAlpha, int mode) {
	float t;
	return rtBlockedPassHidden(origin, dir, tMin, tMax, mask, testAlpha, mode, t);
}

#endif
