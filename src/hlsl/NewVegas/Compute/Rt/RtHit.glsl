// UNOFFICIAL 2026-10-29 shading a ray's hit in the ray tracing scene (FORK-RT-CORE.md "Hit shading"): for any trace shader in the
// helper. rtHitAlbedo() is the hit's diffuse colour (linear): with [Main.Main.RayTracing] HitTextures the texture's colour at the hit's
// texcoords (the albedo atlas: RGBA8, 64 x 64 a texture, 32 slots a row; slot = (flags >> 16) & 0x3FF), else the record's average colour.
// Before including: #define RT_ALBEDO_ATLAS <the albedo atlas: a layout(rgba8) readonly image2D> when the pass binds it
// (RtCore::AlbedoAtlasImage() among its images; leave it undefined: averages only). The records (binding 10) and geometry data
// (binding 11) are declared here unless RT_RECORDS_DECLARED is defined (RtAlpha.glsl defines it; either order works).
// Call with the committed hit: rayQueryGetIntersectionInstanceIdEXT(q, true), ...PrimitiveIndexEXT(q, true), ...BarycentricsEXT(q, true).

#ifndef RT_RECORDS_DECLARED
struct RtRecord { vec4 albedo; uint flags; uint geometryId; uint vertexOffset; uint indexOffset; };
layout(set = 0, binding = 10, std430) readonly buffer RtRecords { RtRecord records[]; };
layout(set = 0, binding = 11, std430) readonly buffer RtGeometry { uint words[]; };
#define RT_RECORDS_DECLARED
#endif

#include "RtTextures.glsl"	// (2026-11-07, protocol 7) the real textures (set 1), where the game sent them; else the atlases below

#define RT_HIT_TEXCOORDS_FLAGS 0x60u   // alpha-tested (0x20) or textured (0x40): texcoords at floatBitsToUint(albedo.w)

uvec3 rtHitTriangle(uint instance, uint primitive) {
	const uint o = records[instance].indexOffset + 3u * primitive;
	return uvec3(words[o], words[o + 1u], words[o + 2u]);
}

vec3 rtHitObjectPosition(uint instance, uint vertex) {
	const uint o = records[instance].vertexOffset + 3u * vertex;
	return vec3(uintBitsToFloat(words[o]), uintBitsToFloat(words[o + 1u]), uintBitsToFloat(words[o + 2u]));
}

// The triangle's normal in object space (unnormalized; turn it by the instance's object-to-world and face it to the ray)
vec3 rtHitObjectNormal(uint instance, uint primitive) {
	const uvec3 i = rtHitTriangle(instance, primitive);
	const vec3 a = rtHitObjectPosition(instance, i.x), b = rtHitObjectPosition(instance, i.y), c = rtHitObjectPosition(instance, i.z);
	return cross(b - a, c - a);
}

// UNOFFICIAL 2026-11-07 (perf-reflfix) the hit's SMOOTH normal: the geometry's vertex normals (the helper's, RtNormals.comp: one packed word
// a vertex just before its positions, record geometryId bit 31 = NVR_RT_RECORD_GEO_NORMALS) interpolated with the barycentrics, in scene
// space. n = the triangle's normal facing the ray (HitNormal / the cross product, normalized), d = the ray's direction. The answer stays on
// n's side (the stored normal may point either way of a two-sided card) and, at a silhouette where the interpolated normal turns away from
// the ray, is tilted back until it just faces it (no black rim). Keep n itself for the ray offsets and back-face decisions. Without the
// normals (an older helper, no room in the data, a displaced copy): n. A sphere in a mirror was faceted with the flat n (test 9).
#define RT_RECORD_GEO_NORMALS 0x80000000u
vec3 rtUnpackNormal(uint w) {
	const vec2 e = vec2(float(w & 0xFFFFu), float(w >> 16)) * (2.0 / 65535.0) - 1.0;
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
	return normalize(v);
}
bool rtHitHasSmoothNormals(uint instance) {
	return (records[instance].geometryId & RT_RECORD_GEO_NORMALS) != 0u && records[instance].vertexOffset != 0xFFFFFFFFu &&
		records[instance].indexOffset > records[instance].vertexOffset;
}
vec3 rtHitSmoothNormal(uint instance, uint primitive, vec2 bary, vec3 n, mat3 toScene, vec3 d) {
	if (!rtHitHasSmoothNormals(instance)) return n;
	const uint vo = records[instance].vertexOffset;
	const uint base = vo - (records[instance].indexOffset - vo) / 3u;
	const uvec3 i = rtHitTriangle(instance, primitive);
	const uint w0 = words[base + i.x], w1 = words[base + i.y], w2 = words[base + i.z];
	const vec3 b = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
	vec3 s = (w0 != 0u ? rtUnpackNormal(w0) * b.x : vec3(0.0)) + (w1 != 0u ? rtUnpackNormal(w1) * b.y : vec3(0.0)) +
		(w2 != 0u ? rtUnpackNormal(w2) * b.z : vec3(0.0));
	s = toScene * s;
	if (!(dot(s, s) > 1.0e-10)) return n;
	s = normalize(s);
	if (dot(s, n) < 0.0) s = -s;
	const float facing = dot(s, -d);
	if (facing < 0.02) s = normalize(s - d * (0.02 - facing));
	return s;
}

bool rtHitHasTexcoords(uint instance) { return (records[instance].flags & RT_HIT_TEXCOORDS_FLAGS) != 0u; }

vec2 rtHitTexcoord(uint instance, uint primitive, vec2 bary) {
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const uvec3 i = rtHitTriangle(instance, primitive);
	vec2 t0 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.x]), uintBitsToFloat(words[uvOffset + 2u * i.x + 1u]));
	vec2 t1 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.y]), uintBitsToFloat(words[uvOffset + 2u * i.y + 1u]));
	vec2 t2 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.z]), uintBitsToFloat(words[uvOffset + 2u * i.z + 1u]));
	return t0 * (1.0 - bary.x - bary.y) + t1 * bary.x + t2 * bary.y;
}

// (2026-11-07, merge of the ptaaa branches) ONE function reads both colour maps: the glow map first (glow), then the diffuse (the glow's
// fallback, or the albedo itself), as two passes of one loop with ONE real-texture sample and one atlas read each, not two copies of the
// sampling code: the trace shaders' drivers inline every call, and the emissive lights' lookups call this many times (RtPathTrace.spv's
// pipeline failed to build on the AMD iGPU with the copies). The answers are the old rtHitAlbedo / rtHitGlow's, step for step.
// (2026-11-07 perf-mirrorfix) a TINTED record: alpha-tested AND textured (never both otherwise; RtScenePick.h ActorRecordFlags): the game's
// hair, drawn grey and tinted by its shader (albedo x 2 x HairTint in the stored curve). Its albedo rgb is the tint's linear colour: the
// diffuse texture's colour is multiplied by it x 2^2.2 (the x 2 in linear light); with no texture the albedo alone is the hair's colour.
#define RT_HIT_TINTED_FLAGS 0x60u
vec3 rtHitMap(uint instance, uint primitive, vec2 bary, bool glow) {
	const bool hasUv = rtHitHasTexcoords(instance);
	const vec2 uv = hasUv ? rtHitTexcoord(instance, primitive, bary) : vec2(0.0);   // (once for every read below)
	// (2026-11-07 perf-mirrorfix2) a FaceGen face (record kind skin 0x0A, no glow 0x04, not alpha-tested 0x20, bits 8-15 non-zero): the
	// face pass's colour gain over its base texture (RtScenePick.h FaceGenGain, k / 32), else the faces were their bare base texture: dark
	const uint hitFlags = records[instance].flags;
	const vec3 tint = (hitFlags & RT_HIT_TINTED_FLAGS) == RT_HIT_TINTED_FLAGS ? records[instance].albedo.rgb * 4.5947934 :
		(hitFlags & 0x2Eu) == 0x0Au && ((hitFlags >> 8u) & 0xFFu) != 0u ? vec3(float((hitFlags >> 8u) & 0xFFu) * (1.0 / 32.0)) : vec3(1.0);
	for (int pass = glow ? 0 : 1; pass < 2; pass++) {	// 0 the glow map, 1 the diffuse
#ifndef RT_NO_REAL_TEXTURES
		if (hasUv) {	// (2026-11-07) the game's own texture, filtered, its mip by the footprint (rtConeWidth)
			const uvec3 ids = rtSlotTextures(records[instance].flags);
			const uint id = pass == 0 ? ids.z : ids.x;
			if (id != 0u) return min(rtTexToLinear(rtTexSampleHit(id, uv, instance, primitive).rgb) * (pass == 1 ? tint : vec3(1.0)), vec3(1.0));	// (perf-mirrorfix2: an albedo, at most 1)
		}
#endif
		if (hasUv) {
			const uint slot = (records[instance].flags >> 16) & 0x3FFu;	// (2026-11-06) bits 16-25 (26-31: the material rule)
			vec3 c = vec3(-1.0);
			if (pass == 0) {
#ifdef RT_GLOW_ATLAS
				const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * 64;
				c = imageLoad(RT_GLOW_ATLAS, origin + clamp(ivec2(fract(uv) * 64.0), ivec2(0), ivec2(63))).rgb;
#endif
			}
			else {
#ifdef RT_ALBEDO_ATLAS
				// (2026-11-06) the slot size off the image: [Main.Main.RayTracing] AlbedoDetail (64 / 128 / 256): what mirrors show of things
				// off screen (people's skin, faces) was a 64 x 64 copy of their texture: smeared
				const int slotSize = imageSize(RT_ALBEDO_ATLAS).x / 32;
				const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * slotSize;
				c = imageLoad(RT_ALBEDO_ATLAS, origin + clamp(ivec2(fract(uv) * float(slotSize)), ivec2(0), ivec2(slotSize - 1))).rgb;
#endif
			}
			// as the map stores it (sRGB-encoded)
			if (c.r >= 0.0) return min(mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045))) * (pass == 1 ? tint : vec3(1.0)), vec3(1.0));
		}
	}
	return records[instance].albedo.rgb;
}

vec3 rtHitAlbedo(uint instance, uint primitive, vec2 bary) { return rtHitMap(instance, primitive, bary, false); }

// UNOFFICIAL 2026-10-30 the hit's glow (emissive) colour, linear, before any brightness: 0 for meshes without a glow map (record flag
// 0x04). With [Main.Main.RayTracing] GlowTextures (+ HitTextures) and #define RT_GLOW_ATLAS <the glow atlas: layout(rgba8) readonly
// image2D> (RtCore::GlowAtlasImage() among the pass's images): the glow map's colour at the hit (same slot and texcoords as the albedo
// atlas): coloured neon tubes, lit window panes, only the lit parts. Else the whole mesh glows in rtHitAlbedo's colour (the old way).
// (2026-11-07) the real glow map first (protocol 7). Both through rtHitMap.
#define RT_HIT_GLOW_FLAG 0x04u
vec3 rtHitGlow(uint instance, uint primitive, vec2 bary) {
	if ((records[instance].flags & RT_HIT_GLOW_FLAG) == 0u) return vec3(0.0);
	return rtHitMap(instance, primitive, bary, true);
}

// UNOFFICIAL 2026-11-02 the hit's normal with its normal map ([Main.Main.RayTracing] NormalTextures; record flag 0x80): n = the surface
// normal in scene space facing the ray (the triangle's, or a smoother one), toScene = the instance's object-to-world rotation. The
// tangent frame is the triangle's own (from its positions and texcoords; no vertex tangents in the arena), made orthogonal to n; the
// map's texel (64 x 64 a texture, as stored: rgb x 2 - 1) bends n. A slot without the map (0) or a degenerate triangle: n unchanged.
// #define RT_NORMAL_ATLAS <the normal atlas: layout(rgba8) readonly image2D> before including, with RtCore::NormalAtlasImage() bound.
// (2026-11-07, RT-DETAIL-GAPS.md section 2) with the real textures (protocol 7, set 1) the slot's real normal map (its id .y) is read
// first, filtered with the hit's footprint (rtConeWidth / rtConeDir, as the diffuse), and the function exists WITHOUT the atlas too
// (RT_HIT_NORMAL_MAPS is defined where it does): RtCore sets the record's 0x80 flag where a slot has a normal map sent
// ([Main.Main.RayTracing] RealNormalMaps, default on with RealTextures), so the flag alone decides; the atlas is the fallback only
// where it is defined AND rtHitNormalAtlasOn (the path tracer binds a stand-in image there when NormalTextures is off: flag 64 off).
// The tangent frame's v follows the texcoord's v (the grass shader's and NormalTextures' convention).
#define RT_HIT_NORMAL_FLAG 0x80u
#if defined(RT_NORMAL_ATLAS) || !defined(RT_NO_REAL_TEXTURES)
#define RT_HIT_NORMAL_MAPS
#ifdef RT_NORMAL_ATLAS
bool rtHitNormalAtlasOn = true;	// false: the image at RT_NORMAL_ATLAS is a stand-in (not read)
#endif
vec3 rtHitNormalMapped(uint instance, uint primitive, vec2 bary, vec3 n, mat3 toScene) {
	if ((records[instance].flags & RT_HIT_NORMAL_FLAG) == 0u || !rtHitHasTexcoords(instance) || records[instance].vertexOffset == 0xFFFFFFFFu) return n;
	const vec2 uv = rtHitTexcoord(instance, primitive, bary);
	vec3 m = vec3(0.0);
	bool found = false;
#ifndef RT_NO_REAL_TEXTURES
	{	// (2026-11-07) the real normal map (as stored, not sRGB)
		const uint id = rtSlotTextures(records[instance].flags).y;
		if (id != 0u) { m = rtTexSampleHit(id, uv, instance, primitive).rgb; found = true; }
	}
#endif
#ifdef RT_NORMAL_ATLAS
	if (!found && rtHitNormalAtlasOn) {
		const uint slot = (records[instance].flags >> 16) & 0x3FFu;	// (2026-11-06) bits 16-25 (26-31: the material rule)
		const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * 64;
		m = imageLoad(RT_NORMAL_ATLAS, origin + clamp(ivec2(fract(uv) * 64.0), ivec2(0), ivec2(63))).rgb;
	}
#endif
	if (dot(m, m) < 1.0e-4) return n;
	vec3 t = m * 2.0 - 1.0;
	const uvec3 i = rtHitTriangle(instance, primitive);
	const vec3 p0 = rtHitObjectPosition(instance, i.x), p1 = rtHitObjectPosition(instance, i.y), p2 = rtHitObjectPosition(instance, i.z);
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const vec2 u0 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.x]), uintBitsToFloat(words[uvOffset + 2u * i.x + 1u]));
	const vec2 u1 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.y]), uintBitsToFloat(words[uvOffset + 2u * i.y + 1u]));
	const vec2 u2 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.z]), uintBitsToFloat(words[uvOffset + 2u * i.z + 1u]));
	const vec3 e1 = toScene * (p1 - p0), e2 = toScene * (p2 - p0);
	const vec2 d1 = u1 - u0, d2 = u2 - u0;
	const float det = d1.x * d2.y - d2.x * d1.y;
	if (abs(det) < 1.0e-12) return n;
	vec3 tangent = (e1 * d2.y - e2 * d1.y) / det, bitangent = (e2 * d1.x - e1 * d2.x) / det;
	tangent -= n * dot(n, tangent);
	if (dot(tangent, tangent) < 1.0e-12) return n;
	tangent = normalize(tangent);
	const vec3 b = cross(n, tangent) * (dot(cross(n, tangent), bitangent) < 0.0 ? -1.0 : 1.0);
	const vec3 bent = normalize(tangent * t.x + b * t.y + n * max(t.z, 0.05));
	return bent;
}
#endif
