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

bool rtHitHasTexcoords(uint instance) { return (records[instance].flags & RT_HIT_TEXCOORDS_FLAGS) != 0u; }

vec2 rtHitTexcoord(uint instance, uint primitive, vec2 bary) {
	const uint uvOffset = floatBitsToUint(records[instance].albedo.w);
	const uvec3 i = rtHitTriangle(instance, primitive);
	vec2 t0 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.x]), uintBitsToFloat(words[uvOffset + 2u * i.x + 1u]));
	vec2 t1 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.y]), uintBitsToFloat(words[uvOffset + 2u * i.y + 1u]));
	vec2 t2 = vec2(uintBitsToFloat(words[uvOffset + 2u * i.z]), uintBitsToFloat(words[uvOffset + 2u * i.z + 1u]));
	return t0 * (1.0 - bary.x - bary.y) + t1 * bary.x + t2 * bary.y;
}

vec3 rtHitAlbedo(uint instance, uint primitive, vec2 bary) {
#ifdef RT_ALBEDO_ATLAS
	if (rtHitHasTexcoords(instance)) {
		const uint slot = (records[instance].flags >> 16) & 0x3FFu;	// (2026-11-06) bits 16-25 (26-31: the material rule)
		// (2026-11-06) the slot size off the image: [Main.Main.RayTracing] AlbedoDetail (64 / 128 / 256): what mirrors show of things
		// off screen (people's skin, faces) was a 64 x 64 copy of their texture: smeared
		const int slotSize = imageSize(RT_ALBEDO_ATLAS).x / 32;
		const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * slotSize;
		const ivec2 texel = origin + clamp(ivec2(fract(rtHitTexcoord(instance, primitive, bary)) * float(slotSize)), ivec2(0), ivec2(slotSize - 1));
		const vec3 c = imageLoad(RT_ALBEDO_ATLAS, texel).rgb;   // as the texture stores it (sRGB-encoded)
		return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
	}
#endif
	return records[instance].albedo.rgb;
}

// UNOFFICIAL 2026-10-30 the hit's glow (emissive) colour, linear, before any brightness: 0 for meshes without a glow map (record flag
// 0x04). With [Main.Main.RayTracing] GlowTextures (+ HitTextures) and #define RT_GLOW_ATLAS <the glow atlas: layout(rgba8) readonly
// image2D> (RtCore::GlowAtlasImage() among the pass's images): the glow map's colour at the hit (same slot and texcoords as the albedo
// atlas): coloured neon tubes, lit window panes, only the lit parts. Else the whole mesh glows in rtHitAlbedo's colour (the old way).
#define RT_HIT_GLOW_FLAG 0x04u
vec3 rtHitGlow(uint instance, uint primitive, vec2 bary) {
	if ((records[instance].flags & RT_HIT_GLOW_FLAG) == 0u) return vec3(0.0);
#ifdef RT_GLOW_ATLAS
	if (rtHitHasTexcoords(instance)) {
		const uint slot = (records[instance].flags >> 16) & 0x3FFu;	// (2026-11-06) bits 16-25 (26-31: the material rule)
		const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * 64;
		const ivec2 texel = origin + clamp(ivec2(fract(rtHitTexcoord(instance, primitive, bary)) * 64.0), ivec2(0), ivec2(63));
		const vec3 c = imageLoad(RT_GLOW_ATLAS, texel).rgb;   // as the glow map stores it (sRGB-encoded)
		return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
	}
#endif
	return rtHitAlbedo(instance, primitive, bary);
}

// UNOFFICIAL 2026-11-02 the hit's normal with its normal map ([Main.Main.RayTracing] NormalTextures; record flag 0x80): n = the surface
// normal in scene space facing the ray (the triangle's, or a smoother one), toScene = the instance's object-to-world rotation. The
// tangent frame is the triangle's own (from its positions and texcoords; no vertex tangents in the arena), made orthogonal to n; the
// map's texel (64 x 64 a texture, as stored: rgb x 2 - 1) bends n. A slot without the map (0) or a degenerate triangle: n unchanged.
// #define RT_NORMAL_ATLAS <the normal atlas: layout(rgba8) readonly image2D> before including, with RtCore::NormalAtlasImage() bound.
#define RT_HIT_NORMAL_FLAG 0x80u
#ifdef RT_NORMAL_ATLAS
vec3 rtHitNormalMapped(uint instance, uint primitive, vec2 bary, vec3 n, mat3 toScene) {
	if ((records[instance].flags & RT_HIT_NORMAL_FLAG) == 0u || !rtHitHasTexcoords(instance) || records[instance].vertexOffset == 0xFFFFFFFFu) return n;
	const uint slot = (records[instance].flags >> 16) & 0x3FFu;	// (2026-11-06) bits 16-25 (26-31: the material rule)
	const ivec2 origin = ivec2(int(slot % 32u), int(slot / 32u)) * 64;
	const vec2 uv = rtHitTexcoord(instance, primitive, bary);
	const vec3 m = imageLoad(RT_NORMAL_ATLAS, origin + clamp(ivec2(fract(uv) * 64.0), ivec2(0), ivec2(63))).rgb;
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
