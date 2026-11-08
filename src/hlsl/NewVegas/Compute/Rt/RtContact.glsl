// UNOFFICIAL 2026-11-07 (rtgap-tess; docs/next/RT-DETAIL-GAPS.md section 1) what the RT passes share for the detail the RT scene lacks
// but the depth buffer has (the tessellated bumps, grass not in the scene, small props, decal edges, foliage cards):
//  (1a) the start slack: with [Shaders.Terrain.Displacement] / [Shaders.POM.Displacement] on, the depth buffer holds the bumps and the RT
//       scene the flat triangles, so a point rebuilt from the depth can lie up to the Strength under the RT surface, and a ray from it is
//       blocked at once (dark speckles in the hollows). rtTessSlack: the extra start offset (Strength, faded out by view depth as the bumps
//       are: TerrainDisplace.tesc / ObjectDisplace.tesc fadeAt).
//  (1b) screen-space contact shadows: a short march over the depth buffer from the surface toward the light (16 steps, at most
//       contact.x pixels on screen and contact.y units); a sample that the march passes BEHIND (within a thickness) blocks it. Every
//       hybrid renderer pairs its RT shadows with such a march for detail under the RT geometry's resolution. The pass multiplies the
//       result into its own ray's answer (the darker).
// The caller defines RT_CONTACT_DEPTH (its r32f depth image: view z / farZ) before including this file.
// Constants (core/RtTessContact.h fills them, the same in every pass):
//   slack:   x Strength (units; 0 = no bumps drawn / TessStartSlack off), y FadeStart, z FadeEnd (view depth)
//   contact: x the march's length on screen (pixels of the depth image), y its longest (units), z thickness (units), w strength (0 = off)
//   gate:    xy the march fades out between these view depths (y <= x: no fade; the raster contact shadows take over past it),
//            z thickness per unit of view depth, w unused

float rtTessSlack(vec4 slack, float viewZ) {
	if (!(slack.x > 0.0)) return 0.0;
	return slack.x * (1.0 - smoothstep(slack.y, max(slack.z, slack.y + 1.0), viewZ));
}

// The light let through to `from` (a point ON the screen's surface, the pass's space: eye + ray x viewZ) along toLight (unit), from the
// depth buffer alone: 1 free, 1 - strength fully blocked. n: the surface's normal (the screen's); lightDist: how far the light is (the
// march stops short of it); forward (unit, w farZ) / right (/ P00) / up (/ P11) / eye / jitter (uv) / size (the depth image's pixels
// over the screen) as the pass rebuilds its points; noise in [0, 1) (the steps' offset, per pixel and frame).
float rtContactShadow(vec3 from, vec3 n, vec3 toLight, float lightDist, float viewZ, vec4 forward, vec3 right, vec3 up, vec3 eye,
	vec2 jitter, vec2 size, vec4 contact, vec4 gate, float noise) {
	if (!(contact.w > 0.0) || !(contact.x > 0.0) || !(viewZ > 1.0)) return 1.0;
	const float weight = contact.w * (gate.y > gate.x ? 1.0 - smoothstep(gate.x, gate.y, viewZ) : 1.0);
	if (!(weight > 0.0) || dot(n, toLight) <= 0.0) return 1.0;   // (a face turned from the light: the pass's own test)
	const float pixel = 2.0 * viewZ * length(up) / max(size.y, 1.0);   // the units a pixel covers here
	const float len = min(min(contact.x * pixel, contact.y), 0.9 * lightDist);
	if (!(len > pixel)) return 1.0;   // under a pixel: nothing to see
	const float thick = contact.z + gate.z * viewZ;
	const float rr = dot(right, right), uu = dot(up, up);
	const vec3 rel0 = from - eye;
	const float plane = dot(rel0, n);   // the start's own plane: the same flat surface seen at another pixel never blocks
	const float bias = max(0.5, 0.0005 * viewZ);
	const int kSteps = 16;
	for (int i = 1; i <= kSteps; i++) {
		const float f = (float(i) - noise) / float(kSteps);
		const vec3 q = rel0 + toLight * (len * f);
		const float z = dot(q, forward.xyz);
		if (!(z > 1.0)) break;
		const vec2 uv = vec2(dot(q, right) / (rr * z), -dot(q, up) / (uu * z)) * 0.5 + 0.5 - jitter;
		const ivec2 px = ivec2(floor(uv * size));
		if (any(lessThan(px, ivec2(0))) || any(greaterThanEqual(px, ivec2(size)))) break;
		const float d = imageLoad(RT_CONTACT_DEPTH, px).r;
		if (!(d > 0.0 && d < 0.9999)) continue;
		const float sz = d * forward.w;
		const float behind = z - sz;   // > 0: the march is behind what the screen shows there
		if (!(behind > bias) || behind > thick) continue;
		const vec2 suv = (vec2(px) + 0.5) / size + jitter;
		const vec3 ray = forward.xyz + right * (2.0 * suv.x - 1.0) - up * (2.0 * suv.y - 1.0);
		const float dn = dot(ray, n);
		if (abs(dn) > 1.0e-4) { const float pz = plane / dn; if (pz > 0.0 && abs(sz - pz) < bias + 0.002 * sz) continue; }
		return 1.0 - weight * (1.0 - smoothstep(0.6, 1.0, f));   // (fading over the march's last part: no hard end)
	}
	return 1.0;
}
