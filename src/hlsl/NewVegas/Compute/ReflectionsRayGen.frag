// UNOFFICIAL 2026-10-28 (NVR DXVK, RT reflections; ReflectionsRT.glsl has the whole frame): Reflections technique 1 (Trace, half
// resolution), first half, with RT on. ReflectionsTrace.frag line for line up to the end of the screen march; then, instead of the
// fallbacks, three outputs: what the march found, the reflected direction (+ roughness), and the ray for the RT helper where the screen
// left a share missing. ReflectionsRTResolve.frag does the rest once the helper has traced.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"
#include "ReflectionsRT.glsl"

layout(location = 1) out vec4 o_dir;   // G32R32F: xy octahedral direction
layout(location = 2) out vec4 o_ray;   // G32R32F: x view z, y roughness (+2: a ray)

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uv = (vpos + 0.5) * LowTexel;
    vec2 pixelUV = uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy);   // a pixel centre in the block
    // UNOFFICIAL 2026-11-03 RenderScale < 1: past this frame's rect nothing reflects (no ray: Resolve's neighbour taps skip it there).
    if (any(greaterThanEqual(uv, cb.renderScale.xy))) { o_color = vec4(0.0); o_dir = vec4(0.0); o_ray = vec4(0.0); return; }
    float depth = readDepthLod(pixelUV);
    vec3 N = ViewNormal(pixelUV);
    float roughness;
    float amount = Reflectivity(pixelUV, depth, N, roughness);
    if (amount <= 0.0) { o_color = vec4(0.0); o_dir = vec4(0.0); o_ray = vec4(0.0); return; }

    vec3 position = SurfacePosition(pixelUV, depth);   // (2026-10-30: the weapon's own field of view)
    vec3 V = normalize(position);   // eye -> surface
    bool weapon = WeaponAt(pixelUV);   // 2026-10-30 RT weapon reflections: no screen march (it would find the weapon), always a ray

    // Spread the reflected direction by the roughness: a GGX half vector round the normal, a new one every frame.
    float a = roughness * roughness;
    float u1 = GradientNoise(vpos), u2 = GradientNoise(vpos + vec2(37.0, 17.0));
    float cosTheta = sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = 6.2831853 * u2;
    vec3 tangent = normalize(abs(N.z) < 0.999 ? cross(N, vec3(0, 0, 1)) : cross(N, vec3(1, 0, 0)));
    vec3 bitangent = cross(N, tangent);
    vec3 H = normalize(tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + N * cosTheta);
    // UNOFFICIAL 2026-10-29 RT reflections, glossy rays: with RT on (and RtVndf), the half vector is drawn from the GGX distribution of
    // VISIBLE normals (Heitz 2018, "Sampling the GGX Distribution of Visible Normals"): only microfacets the eye sees, so far fewer
    // rays go below the surface or into near-grazing directions, and a rough reflection converges in fewer frames. Same noise numbers.
    if (RtOn && rtc.rt3.x > 0.5 && a > 1.0e-4) {
        vec3 Vt = vec3(dot(-V, tangent), dot(-V, bitangent), dot(-V, N));   // toward the eye, in the tangent frame
        vec3 Vh = normalize(vec3(a * Vt.x, a * Vt.y, Vt.z));
        float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
        vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
        vec3 T2 = cross(Vh, T1);
        float r = sqrt(u1), ph = 6.2831853 * u2;
        float t1 = r * cos(ph), t2 = r * sin(ph);
        float s = 0.5 * (1.0 + Vh.z);
        t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
        vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
        vec3 Ht = normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
        H = normalize(tangent * Ht.x + bitangent * Ht.y + N * Ht.z);
    }
    // (2026-11-06) a mirror (roughness under 0.03) traces the exact mirror ray, the same every frame (no spread, a fixed march offset
    // below): no noise to average, so ReflectionsTemporal keeps no history there (the history smeared what mirrors show)
    const bool mirrorPixel = roughness < 0.03;
    if (mirrorPixel) H = N;
    vec3 R = reflect(V, H);
    if (dot(R, N) <= 0.0) R = reflect(V, N);   // spread below the surface: the plain mirror direction
    if (!weapon) R = AboveSurface(pixelUV, position, R);   // (2026-11-05) never under the real surface (a tilted normal map at a low angle)

    // March: steps that grow along the ray (fine near the surface, coarse far off), the first one shifted by the noise.
    float jitter = mirrorPixel ? 0.5 : GradientNoise(vpos + vec2(11.0, 71.0));
    float lastT = 0.0;
    float lastZ = position.z;
    float hitT = -1.0;
    bool searching = !weapon;
    for (float i = 1.0; i <= Steps && searching; i += 1.0) {
        float s = (i - 1.0 + jitter) / Steps;
        float t = RayLength * s * s + 2.0;
        vec3 p = position + R * t;
        vec2 at = ToScreen(p);
        if (p.z <= nearZ || any(lessThanEqual(at, vec2(0.0))) || any(greaterThanEqual(at, cb.renderScale.xy))) searching = false;   // off the screen (2026-11-03: the drawn part)
        else {
            float sceneZ = readDepthLod(at);
            float behind = p.z - sceneZ;
            float thickness = (max(abs(p.z - lastZ), 20.0) * 1.5 + t * 0.03) * ThicknessScale;
            if (behind > 0.0 && behind < thickness && sceneZ < farZ * 0.98) { hitT = t; searching = false; }
            else { lastT = t; lastZ = p.z; }
        }
    }

    vec3 colour = vec3(0.0);
    float confidence = 0.0;
    if (hitT > 0.0) {
        float lo = lastT, hi = hitT;
        for (int r = 0; r < RefineSteps; r++) {
            float m = 0.5 * (lo + hi);
            vec3 q = position + R * m;
            if (q.z > readDepthLod(ToScreen(q))) hi = m; else lo = m;
        }
        vec2 hit = ToScreen(position + R * hi);
        vec3 hitNormal = ViewNormal(hit);
        // Not the weapon (it is not where the depth says, for the world), not the back of something.
        // UNOFFICIAL 2026-11-04 (rtwater) and, from the water, not the water itself: a ray leaving the water near the horizon passed the
        // back-face test on a farther stretch of the same plane (dot < 0.2 at a grazing angle), so the water reflected its own dark colour
        // (a band below the far shore, smears under posts: a tester's shot, og-lake). Such a hit is no hit: the ray goes to the helper.
        float hitDepth = readDepthLod(hit);
        bool selfWater = SurfaceWater && abs(cb.cameraPosition.z + toWorld(hit).z * hitDepth - cb.waterSettings.x) < max(2.0, hitDepth * 0.002);
        if (!IsWeapon(hit) && dot(hitNormal, R) < 0.2 && !selfWater) {
            colour = linearize(textureLod(sampler2D(t_rendered, LINEAR_S), hit, 0.0).rgb);
            vec2 edge = min(rectToScreen(hit), 1.0 - rectToScreen(hit));   // (2026-11-03: the screen's edges, as the .fx)
            confidence = saturate(min(edge.x, edge.y) * 12.0) * (1.0 - smoothstep(0.75, 1.0, hi / RayLength));
        }
    }
    // RT: the ray for the helper where the screen did not fully answer (RT DebugView 3 drops the screen's answer: a ray everywhere).
    // The helper rebuilds the surface from its view z as ViewPosition does, and begins the ray a little way along it (Rt/ReflectionsRT.comp).
    // (RT DebugView 5, the scene alignment check: a ray for every reflecting pixel; the helper then traces from the camera to it.)
    bool wantRay = RtOn && ((RtFade(roughness) > 0.0 && (confidence < 0.999 || abs(RtDebugView - 3.0) < 0.5)) || abs(RtDebugView - 5.0) < 0.5);
    if (weapon) {   // +6: a weapon pixel's own ray (the helper widens its origin by the FOVs and leaves the player out); no budget
        o_color = vec4(0.0);
        o_dir = vec4(OctEncode(ViewToWorldDir(R)), 0.0, 0.0);
        o_ray = vec4(depth, RtOn && RtFade(roughness) > 0.0 ? roughness + 6.0 : roughness, 0.0, 0.0);
        return;
    }
    o_color = vec4(colour, confidence);
    o_dir = vec4(OctEncode(ViewToWorldDir(R)), 0.0, 0.0);
    // Ray budget by roughness (rt3.y = RoughShare's roughness, 0 off): above it, one pixel of each 2x2 block casts this frame (the
    // pixel moves round the block frame by frame); the others are marked +4 and borrow its hit in ReflectionsRTResolve.frag.
    bool borrow = false;
    if (RtOn && RtFade(roughness) > 0.0 && rtc.rt3.y > 0.0 && roughness > rtc.rt3.y && abs(RtDebugView - 5.0) > 0.5) {
        ivec2 q = ivec2(vpos) & 1;
        bool caster = (q.x + 2 * q.y) == (int(NoiseFrame) & 3);
        // The block's caster this frame casts even where the screen answered it (rtrefl-5 lucky22: otherwise its neighbours had no
        // ray to borrow and fell back to the cube and the sky: 3.5 % of pixels darker than a ray per pixel); the others borrow.
        if (caster) wantRay = true; else borrow = wantRay;
    }
    o_ray = vec4(depth, roughness + (borrow ? 4.0 : wantRay ? 2.0 : 0.0), 0.0, 0.0);
}
