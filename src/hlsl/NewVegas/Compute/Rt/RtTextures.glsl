// UNOFFICIAL 2026-11-07 (protocol 7) real textures in the RT helper (tools/nvr-rt-helper/NvrRtTextures.inl, docs/next/RT-REAL-TEXTURES.md):
// the game's own textures, full size with every mip, sampled with filtering and a mip by the ray's footprint, where the atlases kept a
// 64-256 texel copy read texel by texel (blocky up close, speckled far away). Included by RtHit.glsl and RtAlpha.glsl after the records
// (binding 10) and the geometry data (binding 11) are declared. Descriptor set 1 (the helper binds it to every pass):
//   binding 0: per atlas slot (the record's (flags >> 16) & 0x3FF) the diffuse / normal / glow texture's index (0 = none: the atlas)
//   binding 1: the sampler (linear, mips, repeat, anisotropic); binding 2: the textures
// rtConeWidth: the width of the ray's footprint at the hit in world units (set it before calling rtHit*: the camera's pixel spread times
// the distance travelled, widened by rough reflections). 0 (the default) = the finest mip (exact alpha tests, shadow rays).
// A shader built before protocol 7 never declares set 1 and keeps the atlases; RT_NO_REAL_TEXTURES leaves this out on purpose.
// (2026-11-07, ptaaa-omm) rtConeWidth also sets the mip of the alpha tests during traversal (rtAlphaPass): the helper's opacity micromaps
// use lod-0 states only in frames whose passes are all on NvrRtOmm.inl FinestMipShader's list (shaders that never set rtConeWidth). A
// shader that sets it must not be put on that list. (No shipped shader defines RT_NO_REAL_TEXTURES: the micromaps of a slot with a real
// texture are built from it, not from the atlas.)

#ifndef RT_TEXTURES_INCLUDED
#define RT_TEXTURES_INCLUDED
#ifndef RT_NO_REAL_TEXTURES
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 1, binding = 0, std430) readonly buffer RtTexTable { uvec4 rtTexSlots[]; };
layout(set = 1, binding = 1) uniform sampler rtTexSampler;
layout(set = 1, binding = 2) uniform texture2D rtTextures[4096];

float rtConeWidth = 0.0;
// (2026-11-07) anisotropic filtering at a hit (testers: "losing texture quality with RT"): the ray's direction (scene space, unit) and the
// hit's object-to-scene rotation and scale, set with rtConeWidth where the hit is known (rtHit*). Zero direction: one isotropic mip
// (rtTexLod), as before; the alpha tests during traversal stay isotropic (no hit yet).
vec3 rtConeDir = vec3(0.0);
mat3 rtConeToScene = mat3(1.0);

// The slot's real textures: x diffuse, y normal map, z glow map (0 = none)
uvec3 rtSlotTextures(uint flags) { return rtTexSlots[(flags >> 16) & 0x3FFu].xyz; }

// The mip for texture `id` at a hit on (instance, primitive): texels per world unit of the triangle (its texcoord area against its
// object-space area) times the footprint. Object space: the instance's scale is left out (references are nearly all at scale 1).
float rtTexLod(uint id, uint instance, uint primitive) {
	if (rtConeWidth <= 0.0 || records[instance].vertexOffset == 0xFFFFFFFFu) return 0.0;
	const uint io = records[instance].indexOffset + 3u * primitive;
	const uvec3 i = uvec3(words[io], words[io + 1u], words[io + 2u]);
	const uint vo = records[instance].vertexOffset, uo = floatBitsToUint(records[instance].albedo.w);
	const vec3 p0 = vec3(uintBitsToFloat(words[vo + 3u * i.x]), uintBitsToFloat(words[vo + 3u * i.x + 1u]), uintBitsToFloat(words[vo + 3u * i.x + 2u]));
	const vec3 p1 = vec3(uintBitsToFloat(words[vo + 3u * i.y]), uintBitsToFloat(words[vo + 3u * i.y + 1u]), uintBitsToFloat(words[vo + 3u * i.y + 2u]));
	const vec3 p2 = vec3(uintBitsToFloat(words[vo + 3u * i.z]), uintBitsToFloat(words[vo + 3u * i.z + 1u]), uintBitsToFloat(words[vo + 3u * i.z + 2u]));
	const vec2 u0 = vec2(uintBitsToFloat(words[uo + 2u * i.x]), uintBitsToFloat(words[uo + 2u * i.x + 1u]));
	const vec2 u1 = vec2(uintBitsToFloat(words[uo + 2u * i.y]), uintBitsToFloat(words[uo + 2u * i.y + 1u]));
	const vec2 u2 = vec2(uintBitsToFloat(words[uo + 2u * i.z]), uintBitsToFloat(words[uo + 2u * i.z + 1u]));
	const float worldArea = length(cross(p1 - p0, p2 - p0));
	const float uvArea = abs((u1.x - u0.x) * (u2.y - u0.y) - (u2.x - u0.x) * (u1.y - u0.y));
	const vec2 size = vec2(textureSize(sampler2D(rtTextures[nonuniformEXT(id)], rtTexSampler), 0));
	const float texelsPerUnit = sqrt(uvArea * size.x * size.y / max(worldArea, 1.0e-8));
	return log2(max(rtConeWidth * texelsPerUnit, 1.0e-6));
}

vec4 rtTexSample(uint id, vec2 uv, float lod) {
	return textureLod(sampler2D(rtTextures[nonuniformEXT(id)], rtTexSampler), uv, lod);
}

// (2026-11-07) The ray's footprint on the hit triangle as texture gradients (anisotropic ray cones, Akenine-Moller et al. 2021, "Improved
// shader and texture level of detail using ray cones", JCGT 10(1)): the cone's cross-section (rtConeWidth) laid on the triangle's plane is
// an ellipse, rtConeWidth across and rtConeWidth / cos(theta) along the ray's tilt; its two axes carried into texture space through the
// triangle's (scene-space edges -> texcoord edges) map are the gradients textureGrad wants, and the sampler's anisotropic filter (8x) takes
// up to 8 taps along the long axis at the short axis's mip: a road or a wall seen at a glancing angle stays sharp along its length instead
// of dropping to the blurry mip of its long axis. Head-on it is the same mip as rtTexLod. False: degenerate (then rtTexLod).
bool rtTexGrads(uint instance, uint primitive, out vec2 gx, out vec2 gy) {
	gx = vec2(0.0); gy = vec2(0.0);
	if (rtConeWidth <= 0.0 || dot(rtConeDir, rtConeDir) < 0.5 || records[instance].vertexOffset == 0xFFFFFFFFu) return false;
	const uint io = records[instance].indexOffset + 3u * primitive;
	const uvec3 i = uvec3(words[io], words[io + 1u], words[io + 2u]);
	const uint vo = records[instance].vertexOffset, uo = floatBitsToUint(records[instance].albedo.w);
	const vec3 p0 = vec3(uintBitsToFloat(words[vo + 3u * i.x]), uintBitsToFloat(words[vo + 3u * i.x + 1u]), uintBitsToFloat(words[vo + 3u * i.x + 2u]));
	const vec3 p1 = vec3(uintBitsToFloat(words[vo + 3u * i.y]), uintBitsToFloat(words[vo + 3u * i.y + 1u]), uintBitsToFloat(words[vo + 3u * i.y + 2u]));
	const vec3 p2 = vec3(uintBitsToFloat(words[vo + 3u * i.z]), uintBitsToFloat(words[vo + 3u * i.z + 1u]), uintBitsToFloat(words[vo + 3u * i.z + 2u]));
	const vec2 u0 = vec2(uintBitsToFloat(words[uo + 2u * i.x]), uintBitsToFloat(words[uo + 2u * i.x + 1u]));
	const vec2 u1 = vec2(uintBitsToFloat(words[uo + 2u * i.y]), uintBitsToFloat(words[uo + 2u * i.y + 1u]));
	const vec2 u2 = vec2(uintBitsToFloat(words[uo + 2u * i.z]), uintBitsToFloat(words[uo + 2u * i.z + 1u]));
	const vec3 e1 = rtConeToScene * (p1 - p0), e2 = rtConeToScene * (p2 - p0);
	const vec3 c = cross(e1, e2);
	const float c2 = dot(c, c);
	if (c2 < 1.0e-12) return false;
	const vec3 n = c * inversesqrt(c2);
	const vec3 d = rtConeDir;
	// at most 16:1, the sampler's anisotropy (NvrRtTextures.inl): a longer ellipse makes the hardware drop to the mip of its long axis / 16,
	// which blurred the texture across (RT host `ptaniso`, 20:1 with 8x: contrast across 0.054 against one mip's 0.091)
	const float cosT = max(abs(dot(d, n)), 1.0 / 16.0);
	vec3 h1 = d - dot(d, n) * n;
	const float h1len = length(h1);
	h1 = h1len > 1.0e-4 ? h1 / h1len : normalize(e1);   // head-on: any in-plane axis (a circle)
	const vec3 h2 = cross(n, h1);
	const vec3 a1 = h1 * (rtConeWidth / cosT), a2 = h2 * rtConeWidth;
	// v = e1 s + e2 t (in the plane) -> d(uv) = (u1 - u0) s + (u2 - u0) t
	const float g11 = dot(e1, e1), g12 = dot(e1, e2), g22 = dot(e2, e2), det = g11 * g22 - g12 * g12;
	if (abs(det) < 1.0e-12) return false;
	const vec2 du1 = u1 - u0, du2 = u2 - u0;
	const float s1 = (g22 * dot(e1, a1) - g12 * dot(e2, a1)) / det, t1 = (g11 * dot(e2, a1) - g12 * dot(e1, a1)) / det;
	const float s2 = (g22 * dot(e1, a2) - g12 * dot(e2, a2)) / det, t2 = (g11 * dot(e2, a2) - g12 * dot(e1, a2)) / det;
	gx = du1 * s1 + du2 * t1;
	gy = du1 * s2 + du2 * t2;
	return !any(isnan(gx)) && !any(isinf(gx)) && !any(isnan(gy)) && !any(isinf(gy));
}

// A hit's texel: anisotropic (textureGrad on the ray cone's ellipse) where the hit's direction is known, else the isotropic mip
vec4 rtTexSampleHit(uint id, vec2 uv, uint instance, uint primitive) {
	vec2 gx, gy;
	if (rtTexGrads(instance, primitive, gx, gy)) return textureGrad(sampler2D(rtTextures[nonuniformEXT(id)], rtTexSampler), uv, gx, gy);
	return rtTexSample(id, uv, rtTexLod(id, instance, primitive));
}

vec3 rtTexToLinear(vec3 c) {   // the textures are sRGB-encoded (sampled as UNORM, as the atlases)
	return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}
#endif
#endif
