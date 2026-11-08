// UNOFFICIAL 2026-10-28 RT reflections ([Shaders.Reflections.RT], effects\Reflections.cpp RtUser*; docs\next\FORK-RT-REFLECTIONS.md): the RT
// helper's pass (runs in NvrRtHelper.exe, the RT core's 64-bit process; binding layout FORK-RT-CORE.md, protocol v2). One thread per
// half-resolution pixel: where Reflections' RayGen asked for a ray, one closest-hit query from the surface along the reflected
// direction. At a hit (2026-10-29, protocol v2): the hit triangle's normal from the helper's copy of the geometry (binding 11), the
// draw's average diffuse colour (binding 10), and one shadow ray toward the sun from the hit along that normal (outdoors by day).
// 2026-11-07 (perf-reflfix): the hit's SMOOTH normal (the helper's vertex normals, RtHit.glsl rtHitSmoothNormal; the flat triangle normal
// stays for the ray offsets), and MetalBounce: a hit on smooth metal sends ONE mirror ray on (depth 1: no third ray) and the pixel takes
// what that meets (the bounce image's tint x it).
// Outputs (ReflectionsRTResolve.frag reads them):
//   hit:   x hit distance (-1 no ray asked, 0 nothing within Distance), y flags (1 land, 2 albedo known, 4 normal known, 8 glow map;
//          + 16 x (1 + the key lamp's index) when a lamp shadow ray was traced), zw the hit's normal (world axes, octahedral, facing the
//          ray; -dir when unknown; 2026-11-07: in the RT_TEX variants bent by the hit's normal map, RealNormalMaps)
//   shade: rgb the draw's albedo (linear; 0.4 grey when unknown), a the sun visible at the hit (0..1) outdoors by day, else the key
//          lamp visible from the hit (2026-10-29: the lamp that lights the hit most of the nearest 12, one shadow ray)
//   bounce (2026-11-07, only where hit.y has 256): the first hit is smooth metal; hit.zw then hold the bounce's direction (not a normal),
//          shade and the flags' lamp / glow / albedo bits describe the bounce's hit (512: the bounce met nothing: the sky along it);
//          bounce.rgb the metal's Fresnel tint at the first hit, a the bounce's hit distance from the first hit (0: none)
// Compile: glslangValidator -V --target-env vulkan1.2 -o ReflectionsRT.spv ReflectionsRT.comp (tools/nvr-dxvk/compile-compute.sh)
// (The body of ReflectionsRT.comp / ReflectionsRTAlpha.comp: they set #version, the extensions and RT_ALPHA, then include this.)

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 1, std140) uniform Pass {
	vec4 size;          // x width, y height (of the images), z Distance, w 1 = sun shadow rays, + 2 (2026-11-06) AlphaSolid: alpha-tested hits solid (diagnostic)
	vec4 forward;       // xyz camera forward (scene space), w the lamp count (0..12) + RoughReach (the fraction)
	vec4 right;         // xyz camera right / projection[0][0], w (2026-10-30) the first-person weapon's offset scale (1 / Reflections.glsl's cb.weapon.y)
	vec4 up;            // xyz camera up / projection[1][1]
	vec4 origin;        // xyz the eye in scene space (RtCore::Eye()), w 1 = the alignment check (RT DebugView 5)
	vec4 sun;           // xyz toward the sun (scene space, normalized), w 1: the sun is up (outdoors, by day)
	                    // (2026-10-29: size.z Distance is the mirror's reach; a rough ray reaches less, down to RoughReach x it at roughness 1)
	vec4 layout_;       // xy one half-resolution texel in screen uv, zw one full-resolution texel (RayGen's pixel centre rule)
	vec4 lamps[24];     // 2026-10-29: [2i] xyz the lamp in scene space, w its reach; [2i + 1] rgb its colour x dimmer (TESR gamma), w unused
} p;
layout(set = 0, binding = 2, rg32f) uniform readonly image2D dirImage;      // RayGen: the reflected direction, octahedral (world = scene axes)
layout(set = 0, binding = 3, rg32f) uniform readonly image2D rayImage;      // RayGen: x the surface's view z (0: nothing), y roughness (+2: cast)
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D hitImage;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D shadeImage;
#ifdef RT_ALPHA
layout(set = 0, binding = 6, r16f) uniform readonly image2D alphaAtlas;   // RtCore::AlphaAtlasImage()
#endif
#ifdef RT_TEX
// 2026-10-29 hit textures (the core's [Main.Main.RayTracing] HitTextures, RtHit.glsl): the albedo atlas, after the alpha atlas if any
#ifdef RT_ALPHA
layout(set = 0, binding = 7, rgba8) uniform readonly image2D albedoAtlas;   // RtCore::AlbedoAtlasImage()
#else
layout(set = 0, binding = 6, rgba8) uniform readonly image2D albedoAtlas;   // RtCore::AlbedoAtlasImage()
#endif
#define RT_ALBEDO_ATLAS albedoAtlas
#endif
#ifdef RT_GLOW
// 2026-10-30 glow maps (the core's [Main.Main.RayTracing] GlowTextures, RtHit.glsl rtHitGlow): the glow atlas, after the albedo atlas
#ifdef RT_ALPHA
layout(set = 0, binding = 8, rgba8) uniform readonly image2D glowAtlas;   // RtCore::GlowAtlasImage()
#else
layout(set = 0, binding = 7, rgba8) uniform readonly image2D glowAtlas;   // RtCore::GlowAtlasImage()
#endif
#define RT_GLOW_ATLAS glowAtlas
#endif
// (2026-11-07 perf-reflfix) MetalBounce: a hit on smooth metal (chrome, gold: the record's material rule, metal >= 0.75 and roughness <=
// Pass up.w) is not shaded as a diffuse surface: one more ray leaves it along its mirror direction, and what that meets is the
// reflection (a mirror shows a chrome sphere WITH the room in it). The bounce image is the pass's last image (after the atlases).
#if defined(RT_ALPHA) && defined(RT_TEX) && defined(RT_GLOW)
#define RT_BOUNCE_BINDING 9
#elif (defined(RT_ALPHA) && defined(RT_TEX)) || (defined(RT_TEX) && defined(RT_GLOW))
#define RT_BOUNCE_BINDING 8
#elif defined(RT_ALPHA) || defined(RT_TEX)
#define RT_BOUNCE_BINDING 7
#else
#define RT_BOUNCE_BINDING 6
#endif
layout(set = 0, binding = RT_BOUNCE_BINDING, rgba16f) uniform writeonly image2D bounceImage;

// Protocol v2 (tools/nvr-rt-helper/NvrRtProtocol.h): a record per instance, and the geometries' positions + triangle lists.
struct Record { vec4 albedo; uint flags; uint geometryId; uint vertexOffset; uint indexOffset; };
layout(set = 0, binding = 10, std430) readonly buffer Records { Record records[]; };
layout(set = 0, binding = 11, std430) readonly buffer Geometry { uint words[]; };
const uint NoData = 0xFFFFFFFFu;
// The instances these rays see: everything opaque. 0x10 (NVR_RT_MASK_ALPHA) is the core's alpha-tested casters (non-opaque,
// [Main.Main.RayTracing] AlphaTested, 2026-10-29): left out until this pass tests alpha (RtAlpha.glsl), else foliage cards would
// reflect as solid quads. Lamp glass (0x04) and lamp models (0x08) are kept: they show in reflections.
#ifdef RT_ALPHA
const uint RtMask = 0xFFu;
#else
const uint RtMask = 0xEFu;
#endif
const float LandSelfHit = 40.0;
const float LampShadowRoughness = 0.25;   // the key-lamp shadow ray only on smoother surfaces   // game units (a tessellation bump's height and more)
const uint RecordLand = 0x01u, RecordGlow = 0x04u, RecordAlbedoKnown = 0x10u;
#ifdef RT_ALPHA
// 2026-10-29: alpha-tested casters (the core's [Main.Main.RayTracing] AlphaTested; mask bit 0x10, record flag 0x20): foliage, fences and
// wires reflect as their cut-outs. Rays without the opaque flag; a candidate counts when its texel passes the game's alpha test.
#define RT_RECORDS_DECLARED
#define RT_ALPHA_ATLAS alphaAtlas
#include "RtAlpha.glsl"
#define RT_RAY_FLAGS 0u
#define RT_SHADOW_FLAGS gl_RayFlagsTerminateOnFirstHitEXT
#else
#define RT_RAY_FLAGS gl_RayFlagsOpaqueEXT
#define RT_SHADOW_FLAGS (gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT)
#endif
// The hit's colour (rtHitAlbedo: the texture's at the hit with RT_TEX and a textured record, else the record's average)
#define RT_RECORDS_DECLARED
#include "RtHit.glsl"
// Every query's loop (a macro: a ray query cannot be passed to a function): alpha-tested candidates are confirmed where their texel
// passes; opaque geometry commits by itself.
#ifdef RT_ALPHA
#define RtProceed(q) for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; } else { 		if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && 			rtAlphaPass(rayQueryGetIntersectionInstanceIdEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false), 				rayQueryGetIntersectionBarycentricsEXT(q, false)) || p.size.w > 1.5) 			rayQueryConfirmIntersectionEXT(q); }
// (2026-11-07 rtgap-decals) the shadow rays at the hit: decals (NVR_RT_CUSTOM_DECAL) never block them (RtAlpha.glsl rtAlphaPassShadow),
// AlphaSolid's whole cards included
#define RtProceedShadow(q) for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; } else { \
		if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && !rtIsDecal(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)) && \
			(p.size.w > 1.5 || rtAlphaPass(rayQueryGetIntersectionInstanceIdEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false), \
				rayQueryGetIntersectionBarycentricsEXT(q, false)))) \
			rayQueryConfirmIntersectionEXT(q); }
#else
#define RtProceed(q) for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; } else { }
#define RtProceedShadow(q) for (uint rtCap_ = 0u; rayQueryProceedEXT(q); ) if (++rtCap_ > 256u) { rayQueryTerminateEXT(q); break; } else { }
#endif

vec3 OctDecode(vec2 e) {
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
	return normalize(v);
}
vec2 OctEncode(vec3 v) {
	v /= abs(v.x) + abs(v.y) + abs(v.z);
	vec2 e = v.xy;
	if (v.z < 0.0) e = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
	return e;
}
vec3 Position(uint offset, uint index) {
	uint w = offset + 3u * index;
	return vec3(uintBitsToFloat(words[w]), uintBitsToFloat(words[w + 1u]), uintBitsToFloat(words[w + 2u]));
}

// (2026-11-07 perf-reflfix) one committed hit's shading data (the first hit's, and the metal bounce's): the record's flags for Resolve, the
// smooth shading normal (vertex normals, then the normal map), the flat one facing the ray (offsets), the albedo (+ glow), and the sun /
// key lamp through one shadow ray. cone: the hit's texture footprint (rtConeWidth).
struct HitShade { vec3 geo; vec3 normal; vec3 albedo; uint flags; float visible; };
HitShade ShadeHit(uint instance, uint prim, vec2 bary, mat3 toScene, vec3 hitPos, vec3 dir, float t, float roughness, float cone) {
	HitShade h;
	const Record r = records[instance];
	h.flags = ((r.flags & RecordLand) != 0u ? 1u : 0u) | ((r.flags & RecordAlbedoKnown) != 0u ? 2u : 0u) | ((r.flags & RecordGlow) != 0u ? 8u : 0u);
	// The hit triangle's normal, from the helper's copy of the geometry, into scene axes (Gamebryo scales uniformly: the 3x3 part will
	// do), turned to face the ray (the scene is traced from both sides).
	h.geo = -dir;
	if (r.vertexOffset != NoData && r.indexOffset != NoData) {
		uint w = r.indexOffset + 3u * prim;
		vec3 v0 = Position(r.vertexOffset, words[w]), v1 = Position(r.vertexOffset, words[w + 1u]), v2 = Position(r.vertexOffset, words[w + 2u]);
		vec3 n = cross(v1 - v0, v2 - v0);
		if (dot(n, n) > 1.0e-12) {
			n = normalize(toScene * n);
			h.geo = dot(n, dir) > 0.0 ? -n : n;
			h.flags |= 4u;
		}
	}
	// (2026-11-07 perf-reflfix) the vertex normals interpolated (a sphere in a mirror was faceted with the triangle's)
	h.normal = (h.flags & 4u) != 0u ? rtHitSmoothNormal(instance, prim, bary, h.geo, toScene, dir) : h.geo;

	// The sun at the hit: one shadow ray off the hit along its (flat) normal. Indoors or at night instead: the lamp of the nearest 12 that
	// lights the hit most (ReflectionsRTResolve.frag adds all 12, this one through its shadow ray).
	h.visible = 1.0;
	vec3 off = hitPos + h.geo * max(1.0, t * 0.001);
	if (mod(p.size.w, 2.0) > 0.5 && p.sun.w > 0.5 && p.origin.w < 0.5) {
		if (dot(h.normal, p.sun.xyz) > 0.0) {
			rayQueryEXT s;
			rayQueryInitializeEXT(s, scene, RT_SHADOW_FLAGS, RtMask, off, 0.5, p.sun.xyz, 30000.0);
			RtProceedShadow(s);
			h.visible = rayQueryGetIntersectionTypeEXT(s, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
		}
	}
	else if (mod(p.size.w, 2.0) > 0.5 && p.origin.w < 0.5) {
		int key = -1;
		float best = 0.0;
		int count = int(floor(p.forward.w));
		for (int i = 0; i < count; i++) {
			vec3 toLamp = p.lamps[2 * i].xyz - hitPos;
			float d = length(toLamp), reach = p.lamps[2 * i].w;
			float fall = clamp(1.0 - (d * d) / (reach * reach), 0.0, 1.0);
			float c = fall * fall * max(dot(h.normal, toLamp / max(d, 1.0e-3)), 0.0) * dot(p.lamps[2 * i + 1].rgb, vec3(0.2126, 0.7152, 0.0722));
			if (c > best) { best = c; key = i; }
		}
		// (The shadow ray only where the reflection is sharp enough to show it: roughness below LampShadowRoughness; rougher hits keep
		// the lamp unshadowed, visible = 1. rtrefl-4 luxe: this ray cost 1.5-5 ms on the iGPU on all hits.)
		if (key >= 0 && roughness < LampShadowRoughness) {
			vec3 toLamp = p.lamps[2 * key].xyz - off;
			float d = length(toLamp);
			// Stop short of the lamp itself (its bulb, shade or glass mesh is in the scene).
			rayQueryEXT s;
			rayQueryInitializeEXT(s, scene, RT_SHADOW_FLAGS, RtMask, off, 0.5, toLamp / max(d, 1.0e-3), max(d - 24.0, 0.6));
			RtProceedShadow(s);
			h.visible = rayQueryGetIntersectionTypeEXT(s, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
			h.flags += 16u * uint(key + 1);
		}
	}
	h.albedo = (h.flags & 2u) != 0u ? r.albedo.rgb : vec3(0.4);
#ifdef RT_TEX
	if (rtHitHasTexcoords(instance)) {
#ifndef RT_NO_REAL_TEXTURES
		rtConeWidth = cone;
		rtConeDir = dir; rtConeToScene = toScene;	// (2026-11-07) anisotropic filtering (RtTextures.glsl)
#endif
		h.albedo = rtHitAlbedo(instance, prim, bary);
		h.flags |= 2u;
#ifdef RT_HIT_NORMAL_MAPS
		// (2026-11-07, RT-DETAIL-GAPS.md section 2) RealNormalMaps: the hit's own normal map (record flag 0x80, the real map at the same
		// footprint as its colour) bends the normal Resolve shades the hit with (sun N.L, the lamps); the shadow rays above left from the
		// flat triangle normal. Resolve's shading is diffuse (RtShadeHit: N.L, the grid / cube along N), so the bend counts however the
		// ray sees the bump (one facing the light is lit even where the ray meets it edge-on); it stays on the triangle's side.
		if ((h.flags & 4u) != 0u) h.normal = rtHitNormalMapped(instance, prim, bary, h.normal, toScene);
#endif
	}
#endif
#ifdef RT_GLOW
	// 2026-10-30: a glowing draw glows only where its glow map is lit (sign letters, lamp globes): there the shade carries the glow
	// colour and the glow flag stays (Resolve adds it x GlowBrightness); elsewhere on the mesh it is lit like any surface.
	if ((h.flags & 8u) != 0u) {
		const vec3 glow = rtHitGlow(instance, prim, bary);
		if (max(glow.r, max(glow.g, glow.b)) > 0.02) h.albedo = glow;
		else h.flags &= ~8u;
	}
#endif
	return h;
}

// (2026-11-07 perf-reflfix) MetalBounce: the record's material rule (bits 26-31 of a textured / alpha-tested record: roughness k / 7,
// metal (k - 1) / 6) says smooth metal: metal >= 0.75 and roughness <= the setting (Pass up.w; 0 = off). Out: its roughness.
bool MetalBounce(uint instance, out float hitRough) {
	hitRough = 1.0;
	const uint f = records[instance].flags;
	if (!(p.up.w > 0.0) || (f & 0x60u) == 0u) return false;
	const uint rk = (f >> 26u) & 7u, mk = (f >> 29u) & 7u;
	if (rk == 0u || mk == 0u) return false;
	hitRough = float(rk) / 7.0;
	return float(mk - 1u) / 6.0 >= 0.75 && hitRough <= p.up.w + 1.0e-4;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= int(p.size.x) || pixel.y >= int(p.size.y)) return;
	vec2 ray = imageLoad(rayImage, pixel).xy;
	// 2026-10-30 RT weapon reflections: 5.5..7.5 a first-person weapon pixel's ray (RayGen +6)
	const bool weapon = ray.y > 5.5 && ray.y < 7.5;
	if (ray.x <= 0.0 || ray.y < 1.5 || (ray.y > 3.5 && !weapon)) {   // (> 3.5: a rough pixel that borrows its block's ray this frame)
		imageStore(hitImage, pixel, vec4(-1.0, 0.0, 0.0, 0.0));
		imageStore(shadeImage, pixel, vec4(0.4, 0.4, 0.4, 1.0));
		return;
	}

	// The surface as RayGen read it: ReflectionsTrace's pixel centre in the block, ViewPosition() in scene axes.
	vec2 uv = (vec2(pixel) + 0.5) * p.layout_.xy - 0.5 * (p.layout_.xy - p.layout_.zw);
	// (the weapon: drawn with its own narrower field of view, so its sideways offsets shrink by right.w)
	const float side = weapon && p.right.w > 0.0 ? p.right.w : 1.0;
	vec3 surface = p.origin.xyz + (p.forward.xyz + (p.right.xyz * (2.0 * uv.x - 1.0) - p.up.xyz * (2.0 * uv.y - 1.0)) * side) * ray.x;
	vec3 dir = OctDecode(imageLoad(dirImage, pixel).xy);
	// Start a little way along the ray: past the depth buffer's own error far away (no hit on the surface itself).
	float tMin = 1.0 + ray.x * 0.0015;
	vec3 start = surface;
	// (2026-11-05 rtrefl-only, the demo room's marble at a grazing angle) a ray that leaves the surface low hardly rises over tMin, so the
	// depth buffer's error put its start under the traced floor and it hit that floor at once: a blurred copy of the tiles in place of
	// the room. Start it off the surface along the surface normal (the reflection's half vector: dir - view, exact for a mirror, the
	// sampled microfacet's for a glossy ray), by the same error budget.
	{
		const vec3 halfVector = dir - normalize(surface - p.origin.xyz);
		const float halfLength = length(halfVector);
		if (halfLength > 1.0e-3) start = surface + halfVector * ((0.5 + ray.x * 0.002) / halfLength);
	}
	// Ray budget by roughness: a rough reflection blurs far detail away, so its rays need not go as far. The reach share at
	// roughness 1 (RoughReach) is forward.w's fraction (its integer part is the lamp count).
	float roughness = ray.y - (weapon ? 6.0 : 2.0);   // (cast pixels: roughness + 2; weapon pixels + 6)
	float roughReach = fract(p.forward.w);   // RoughReach (0 = full reach; the integer part is the lamp count)
	float tMax = p.size.z * (roughReach > 0.0 ? mix(1.0, roughReach, clamp(roughness, 0.0, 1.0)) : 1.0);
	if (p.origin.w > 0.5) {
		// The alignment check: from the camera toward the surface the screen shows there. Its hit should be that surface (t = its
		// distance from the camera); ReflectionsRTResolve compares.
		start = p.origin.xyz;
		dir = normalize(surface - p.origin.xyz);
		tMin = 1.0;
		tMax = length(surface - p.origin.xyz) * 2.0 + 500.0;
	}

	rayQueryEXT q;
	// (a weapon's ray leaves the people out (0x20) and the player's own rigid parts, their held and holstered weapons (0x40,
	// NVR_RT_MASK_PLAYER, rt-sun-core 108cfa5b): they stand round the gun in the scene, and it would reflect them from inside)
	const uint mask = weapon ? (RtMask & ~0x60u) : RtMask;
	rayQueryInitializeEXT(q, scene, RT_RAY_FLAGS, mask, start, tMin, dir, tMax);
	RtProceed(q);
	if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		imageStore(hitImage, pixel, vec4(0.0, 0.0, 0.0, 0.0));
		imageStore(shadeImage, pixel, vec4(0.4, 0.4, 0.4, 1.0));
		return;
	}
	float t = rayQueryGetIntersectionTEXT(q, true);
	// 2026-10-29 (noon still-spot run): NVR's land tessellation moves the ground the screen shows off the flat mesh the scene holds, so
	// a ray leaving displaced ground can hit that same ground right away (amber blotches of "ground" in wet ground's reflection). A land
	// hit very close to the start (LandSelfHit + 1 % of the view distance) is taken as the ground itself: one more query from past it.
	if (p.origin.w < 0.5 && (records[rayQueryGetIntersectionInstanceIdEXT(q, true)].flags & RecordLand) != 0u && t < LandSelfHit + ray.x * 0.01) {
		rayQueryInitializeEXT(q, scene, RT_RAY_FLAGS, mask, start, t + 1.0, dir, tMax);
		RtProceed(q);
		if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
			imageStore(hitImage, pixel, vec4(0.0, 0.0, 0.0, 0.0));
			imageStore(shadeImage, pixel, vec4(0.4, 0.4, 0.4, 1.0));
			return;
		}
		t = rayQueryGetIntersectionTEXT(q, true);
	}
	const uint hitInstance = uint(rayQueryGetIntersectionInstanceIdEXT(q, true));
	const uint hitPrim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
	const vec2 hitBary = rayQueryGetIntersectionBarycentricsEXT(q, true);
	const mat3 hitToScene = mat3(rayQueryGetIntersectionObjectToWorldEXT(q, true));
	const vec3 hitPos = start + dir * t;
	// (2026-11-07) the real textures' mip: a full-size pixel's footprint carried to the hit (eye -> surface -> hit), widened by the
	// surface's roughness squared (the reflection lobe's GGX alpha), so a mirror shows the texture sharp and a rough floor its coarse mips (no speckle)
	const float pixelAngle = length(p.up.xyz) / max(p.size.y, 1.0);
	const float cone = (length(surface - p.origin.xyz) + t) * pixelAngle + t * roughness * roughness * 0.5;	// (the lobe's width goes with GGX alpha = roughness^2: a 0.02 mirror is sharp)
	const HitShade h = ShadeHit(hitInstance, hitPrim, hitBary, hitToScene, hitPos, dir, t, roughness, cone);

	// (2026-11-07 perf-reflfix) MetalBounce: smooth metal reflects. ONE more ray along the mirror direction about its shading normal (depth
	// 1: what that ray meets is shaded, never reflected again: a mirror -> chrome -> chrome path ends on the second chrome's estimate). The
	// pixel is then the metal's Fresnel tint x what the bounce sees (ReflectionsRTResolve.frag: the screen there, else its estimate).
	float hitRough;
	if (p.origin.w < 0.5 && (h.flags & 4u) != 0u && MetalBounce(hitInstance, hitRough)) {
		vec3 d2 = reflect(dir, h.normal);
		if (dot(d2, h.geo) <= 0.0) d2 = reflect(dir, h.geo);   // (a bent normal at a grazing angle: the triangle's mirror direction)
		// the metal's Fresnel (Schlick): F0 = its colour (the record's texture at the hit: gold, chrome)
		const float c = clamp(dot(h.normal, -dir), 0.0, 1.0);
		const vec3 f0 = clamp(h.albedo, vec3(0.0), vec3(1.0));
		const vec3 tint = f0 + (vec3(1.0) - f0) * pow(1.0 - c, 5.0);
		const vec3 off = hitPos + h.geo * max(1.0, t * 0.001);
		rayQueryEXT q2;
		rayQueryInitializeEXT(q2, scene, RT_RAY_FLAGS, mask, off, 0.5, d2, max(tMax - t, tMax * 0.25));
		RtProceed(q2);
		if (rayQueryGetIntersectionTypeEXT(q2, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
			imageStore(hitImage, pixel, vec4(t, float(256u | 512u), OctEncode(d2)));
			imageStore(shadeImage, pixel, vec4(1.0, 1.0, 1.0, 1.0));
			imageStore(bounceImage, pixel, vec4(tint, 0.0));
			return;
		}
		const float t2 = rayQueryGetIntersectionTEXT(q2, true);
		const float cone2 = cone + t2 * (pixelAngle + hitRough * hitRough * 0.5);
		const HitShade h2 = ShadeHit(uint(rayQueryGetIntersectionInstanceIdEXT(q2, true)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(q2, true)),
			rayQueryGetIntersectionBarycentricsEXT(q2, true), mat3(rayQueryGetIntersectionObjectToWorldEXT(q2, true)), off + d2 * t2, d2, t2,
			max(roughness, hitRough), cone2);
		const uint flags2 = 256u | (h2.flags & (2u | 8u | 0xF0u));   // its albedo / glow / key lamp (its land / normal bits: unused for a bounce)
		imageStore(hitImage, pixel, vec4(t, float(flags2), OctEncode(d2)));
		imageStore(shadeImage, pixel, vec4(h2.albedo, h2.visible));
		imageStore(bounceImage, pixel, vec4(tint, max(t2, 1.0e-3)));
		return;
	}
	imageStore(hitImage, pixel, vec4(t, float(h.flags), OctEncode(h.normal)));
	imageStore(shadeImage, pixel, vec4(h.albedo, h.visible));
}
