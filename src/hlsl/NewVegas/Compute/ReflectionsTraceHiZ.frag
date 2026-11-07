// UNOFFICIAL 2026-10-26 (NVR DXVK, deep/fork-hiz): Reflections technique 1 (Trace, half resolution) with a HIERARCHICAL ray march on the
// Hi-Z pyramid (HiZBuild.comp, core/HiZ.cpp) instead of the fixed growing steps. [Shaders.Reflections.Main] HiZ. Everything before and
// after the march is ReflectionsTrace.frag's, line for line (same roughness spread, same fallbacks, same firefly cap); only how the
// first hit along the reflected ray is found differs:
//   ReflectionsTrace: Steps samples at t = MaxDistance * s^2 (s = 0..1), then 5 bisection steps; can step over thin things.
//   here: the ray is walked in screen space, cell by cell. A cell of a coarse level whose nearest depth is behind the whole stretch of the
//   ray inside it (or whose farthest depth plus the allowed thickness is in front of it) cannot hold a hit and is skipped whole; the
//   walk climbs a level after each skip and goes down a level where a hit is possible. On the finest level (the full-resolution depth
//   itself) a pixel is a hit when the ray passes behind it by less than the thickness: the trace's own rule, with its own thickness
//   (the same growth along the ray), at every pixel the ray crosses instead of every Steps-th sample. No bisection is needed.
// Same reach (MaxDistance), same stop conditions (off the screen, behind the near plane, the sky never a hit, the weapon rejected).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"
#include "Counters.glsl"   // UNOFFICIAL 2026-10-26: empty unless built with -DNVR_COUNTERS=1 (ReflectionsTraceHiZCount.spv)

// The pyramid (HiZBuild.comp v2): a storage buffer, vec2 (nearest, farthest linear depth / farZ) per cell, level k = 2^(k+1) pixels,
// levels one after the other, level k (W0 >> k) x (H0 >> k), W0 x H0 = the screen's half rounded up to a multiple of 64.
layout(set = 1, binding = 16, std430) readonly buffer HiZPyramid { vec2 hizCells[]; };

const int HiZLevels = 7;           // the pyramid's levels; walk levels 0 (full resolution) .. HiZLevels
const int HiZMaxIterations = 64;    // cells walked at most; the rest of the ray then as the fixed-step trace (below)

// Nearest and farthest depth (game units) of cell c at walk level L (0 = TESR_DepthBuffer itself, L >= 1 = NVR_HiZ level L - 1).
vec2 CellDepth(ivec2 c, int L) {
    if (L == 0) { float d = texelFetch(sampler2D(t_depth, POINT_S), c, 0).x * farZ; return vec2(d); }
    uvec2 size0 = ((uvec2(1.0 / cb.reciprocalResolution.xy + 0.5) + 127u) / 128u) * 64u;
    uint k = uint(L - 1), offset = 0u;
    for (uint i = 0u; i < k; i++) offset += (size0.x >> i) * (size0.y >> i);
    vec2 v = hizCells[offset + uint(c.y) * (size0.x >> k) + uint(c.x)];
    // HiZSafeCells (layoutData.w 1): robust to the pyramid's two channels arriving swapped or one of them empty (see below).
    if (cb.layoutData.w > 0.5) v = vec2(min(v.x, v.y), max(v.x, v.y));
    return v * farZ;
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uv = (vpos + 0.5) * LowTexel;
    vec2 pixelUV = uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy);   // a pixel centre in the block
    float depth = readDepthLod(pixelUV);
    vec3 N = ViewNormal(pixelUV);
    float roughness;
    float amount = Reflectivity(pixelUV, depth, N, roughness);
    if (amount <= 0.0) { NVR_COUNT(11); o_color = vec4(0.0); return; }

    vec3 position = ViewPosition(pixelUV, depth);
    vec3 V = normalize(position);   // eye -> surface

    // Spread the reflected direction by the roughness: a GGX half vector round the normal, a new one every frame.
    float a = roughness * roughness;
    float u1 = GradientNoise(vpos), u2 = GradientNoise(vpos + vec2(37.0, 17.0));
    float cosTheta = sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float phi = 6.2831853 * u2;
    vec3 tangent = normalize(abs(N.z) < 0.999 ? cross(N, vec3(0, 0, 1)) : cross(N, vec3(1, 0, 0)));
    vec3 bitangent = cross(N, tangent);
    vec3 H = normalize(tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + N * cosTheta);
    vec3 R = reflect(V, H);
    if (dot(R, N) <= 0.0) R = reflect(V, N);   // spread below the surface: the plain mirror direction
    R = AboveSurface(pixelUV, position, R);   // (2026-11-05) never under the real surface (a tilted normal map at a low angle)

    // ---- The hierarchical march ----
    vec2 fullSize = 1.0 / cb.reciprocalResolution.xy;
    float tStart = 2.0;                        // the trace's first sample sits 2 units out (+ its jitter)
    float tEnd = RayLength + 2.0;              // its last at MaxDistance + 2
    if (R.z < 0.0) tEnd = min(tEnd, (nearZ - position.z) / R.z * 0.999);   // stay in front of the near plane (p.z > nearZ)
    float hitT = -1.0;
    vec2 hit = vec2(0.0);
    // DebugView 9 / 10 (diagnostic): how the walk ended, and what it skipped on the way.
    int endRule = 5;   // 5 not started (behind the near plane at once, or the start off the screen)
    float nFacing = 0.0, nPlane = 0.0, nThick = 0.0;
    int cellsWalked = 0;   // (shader counters)
    if (tEnd > tStart) {
        vec3 P0 = position + R * tStart, P1 = position + R * tEnd;
        float k0 = 1.0 / P0.z, k1 = 1.0 / P1.z;   // 1/z is linear in screen space
        vec3 Q0 = P0 * k0, Q1 = P1 * k1;
        vec2 S0 = ToScreen(P0) * fullSize, S1 = ToScreen(P1) * fullSize;   // full-resolution pixels
        vec2 d = S1 - S0;
        // Where the ray leaves the screen (the trace stops there: at <= 0 or >= 1).
        float uMax = 1.0;
        if (d.x > 0.0) uMax = min(uMax, (fullSize.x - 0.01 - S0.x) / d.x); else if (d.x < 0.0) uMax = min(uMax, (0.01 - S0.x) / d.x);
        if (d.y > 0.0) uMax = min(uMax, (fullSize.y - 0.01 - S0.y) / d.y); else if (d.y < 0.0) uMax = min(uMax, (0.01 - S0.y) / d.y);
        bool inside = all(greaterThan(S0, vec2(0.0))) && all(lessThan(S0, fullSize));
        vec2 dirSign = vec2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
        vec2 invD = vec2(abs(d.x) > 1.0e-6 ? 1.0 / d.x : 1.0e12, abs(d.y) > 1.0e-6 ? 1.0 / d.y : 1.0e12);
        // The trace's thickness at distance t along the ray: its step there (t = L s^2 grows by about 2 L s / Steps) projected on z.
        float stepZ = abs(R.z) * 2.0 * RayLength / Steps;
        float u = 0.0;
        int level = 0;
        int i = 0;
        if (inside) endRule = 0;   // 0: walked to the end of the ray / off the screen, nothing found
        for (; i < HiZMaxIterations && inside && u < uMax; i++) {
            float size = exp2(float(level));
            vec2 S = S0 + d * u;
            vec2 cell = floor(S / size);
            // The ray's exit from this cell (pushed a hair past the border so the next cell is entered).
            vec2 border = (cell + max(dirSign, 0.0)) * size + dirSign * 0.002;
            vec2 uBorder = (border - S0) * invD;
            float uExit = min(min(uBorder.x, uBorder.y), uMax);
            float zA = 1.0 / mix(k0, k1, u), zB = 1.0 / mix(k0, k1, uExit);
            float rayNear = min(zA, zB), rayFar = max(zA, zB);
            float tExit = dot(mix(Q0, Q1, uExit) * zB - position, R);
            float sExit = sqrt(max(tExit - 2.0, 0.0) / RayLength);
            float thickness = (max(stepZ * sExit, 20.0) * 1.5 + tExit * 0.03) * ThicknessScale;
            vec2 scene = CellDepth(ivec2(cell), level);   // nearest, farthest
            // HiZSafeCells (layoutData.w 1): the "behind everything" skip only on the finest level (the depth itself), never on
            // the pyramid's farthest-depth channel. The diag run (lucky22, DebugView 9/10) showed no walk hit at all indoors and ~4-5
            // such skips on every ray: the coarse farthest depth reads too small (suspect: the pyramid's second channel).
            bool behindSkip = rayNear - scene.y >= thickness && (level == 0 || cb.layoutData.w < 0.5);
            bool skip = rayFar <= scene.x                  // the ray is in front of everything in the cell
                || scene.x >= farZ * 0.98                  // sky only: never a hit
                || behindSkip;                             // behind everything by more than the thickness
            if (rayFar > scene.x && scene.x < farZ * 0.98 && behindSkip) nThick += 1.0;
            // A pixel whose surface faces along the ray (the reflecting floor itself, seen again a few pixels on where the depth
            // buffer's pixel steps put the ray 'behind' it) is no hit: the walk goes on. The fixed-step trace rarely sampled those first
            // pixels; testing every pixel, the walk would stop on its own floor almost every time (and the hit be thrown away below).
            if (!skip && level == 0 && dot(ViewNormal((cell + 0.5) * cb.reciprocalResolution.xy), R) >= 0.2) { skip = true; nFacing += 1.0; }
            // Nor is a pixel of the reflecting surface's own plane (a rough floor's spread sends rays almost along the floor, whose
            // nearby pixels then pass the facing test and gave back the floor's own colour): its point lies on the origin's plane.
            if (!skip && level == 0) {
                vec3 scenePoint = ViewPosition((cell + 0.5) * cb.reciprocalResolution.xy, scene.x);
                if (abs(dot(scenePoint - position, N)) < 2.0 + 0.004 * scene.x) { skip = true; nPlane += 1.0; }
            }
            if (skip) {
                u = uExit;
                level = min(level + 1, HiZLevels);
            }
            else if (level > 0) level--;
            else {
                // A hit in this pixel: where the ray passes behind its depth (or where it entered, already behind).
                float uHit = zA >= scene.x ? u : clamp((1.0 / scene.x - k0) / (k1 - k0), u, uExit);
                float zHit = 1.0 / mix(k0, k1, uHit);
                hitT = dot(mix(Q0, Q1, uHit) * zHit - position, R);
                hit = clamp(S0 + d * uHit, cell + 0.01, cell + 0.99) * cb.reciprocalResolution.xy;
                endRule = 1;
                break;
            }
        }
        cellsWalked = i;
        // Out of cells before the end of the ray (a ray grazing a floor: every coarse cell holds floor nearer than the ray, so the walk
        // goes pixel by pixel): the rest of the ray is searched as the fixed-step trace does it, from where the walk stopped, with its own
        // samples, hit rule and bisection (ReflectionsTrace.frag), so the walk never finds less than the old trace.
        if (hitT < 0.0 && inside && i >= HiZMaxIterations && u < uMax) {
            endRule = 4;   // out of cells, the fixed-step rest found nothing (3 below when it found something)
            float uNow = u;
            float zNow = 1.0 / mix(k0, k1, uNow);
            float tNow = dot(mix(Q0, Q1, uNow) * zNow - position, R);
            float jitter = GradientNoise(vpos + vec2(11.0, 71.0));
            float lastT = tNow;
            float lastZ = position.z + R.z * tNow;
            float foundT = -1.0;
            for (float k = 1.0; k <= Steps; k += 1.0) {
                float s = (k - 1.0 + jitter) / Steps;
                float t = RayLength * s * s + 2.0;
                if (t <= tNow) continue;
                vec3 p = position + R * t;
                vec2 at = ToScreen(p);
                if (p.z <= nearZ || any(lessThanEqual(at, vec2(0.0))) || any(greaterThanEqual(at, vec2(1.0)))) break;
                float sceneZ = readDepthLod(at);
                float behind = p.z - sceneZ;
                float thickness = (max(abs(p.z - lastZ), 20.0) * 1.5 + t * 0.03) * ThicknessScale;
                if (behind > 0.0 && behind < thickness && sceneZ < farZ * 0.98) { foundT = t; break; }
                lastT = t; lastZ = p.z;
            }
            if (foundT > 0.0) {
                float lo = lastT, hi = foundT;
                for (int r = 0; r < RefineSteps; r++) {
                    float m = 0.5 * (lo + hi);
                    vec3 q = position + R * m;
                    if (q.z > readDepthLod(ToScreen(q))) hi = m; else lo = m;
                }
                hitT = hi;
                hit = ToScreen(position + R * hi);
                endRule = 3;
            }
        }
    }

    vec3 colour = vec3(0.0);
    float confidence = 0.0;
    if (hitT > 0.0) {
        vec3 hitNormal = ViewNormal(hit);
        // Not the weapon (it is not where the depth says, for the world), not the back of something.
        if (!IsWeapon(hit) && dot(hitNormal, R) < 0.2) {
            colour = linearize(textureLod(sampler2D(t_rendered, LINEAR_S), hit, 0.0).rgb);
            vec2 edge = min(hit, 1.0 - hit);
            confidence = saturate(min(edge.x, edge.y) * 12.0) * (1.0 - smoothstep(0.75, 1.0, hitT / RayLength));
        }
        else if (endRule == 1 || endRule == 3) endRule = 2;   // found, then thrown away (the weapon, or the back of something)
    }
    // UNOFFICIAL 2026-10-26 shader counters ([Main.Develop.ShaderCounters] ReflectionsHiZ; only in ReflectionsTraceHiZCount.spv, built
    // with -DNVR_COUNTERS=1; Counters.glsl, core/ShaderCounters.h): 0 rays, 1-6 how the walk ended (endRule 0-5 as DebugView 9),
    // 7 cells walked, 8-10 the skips of DebugView 10 (facing, own plane, behind by more than the thickness), 11 pixels with no reflection.
    NVR_COUNT(0);
    NVR_COUNT(1 + endRule);
    NVR_COUNT_N(7, cellsWalked);
    NVR_COUNT_N(8, nFacing);
    NVR_COUNT_N(9, nPlane);
    NVR_COUNT_N(10, nThick);
    NVR_PROBE(0, vec4(float(endRule), float(cellsWalked), hitT, confidence));
    NVR_PROBE(1, vec4(nFacing, nPlane, nThick, roughness));
    NVR_PROBE(2, vec4(hit, depth, amount));
    // DebugView 9: green a walk hit, cyan a hit of the fixed-step rest after the cell limit, yellow a hit thrown away, red walked to the
    // end / off the screen with nothing, magenta out of cells and the rest found nothing, blue not started.
    // DebugView 10: red the pixels skipped as facing along the ray, green those on the surface's own plane, blue the cells skipped as
    // behind by more than the thickness (each count / 8). [Shaders.Reflections.Main] HiZ only; the fixed-step trace shows view 6 there.
    if (DebugView > 8.5) {
        vec3 c;
        if (DebugView > 9.5) c = saturate(vec3(nFacing, nPlane, nThick) / 8.0);
        else c = endRule == 1 ? vec3(0, 1, 0) : endRule == 3 ? vec3(0, 1, 1) : endRule == 2 ? vec3(1, 1, 0) :
            endRule == 0 ? vec3(1, 0, 0) : endRule == 4 ? vec3(1, 0, 1) : vec3(0, 0, 1);
        o_color = vec4(Tame(c * 0.6), 1.0);
        return;
    }
    // What the ray did not find on screen: the surroundings cube and (GridFallback) the light grid along it, then (outdoors)
    // the sky if it points up into it. Each fills only the share still missing (premultiplied sums).
    vec3 sum = colour * confidence;
    vec3 worldR = ViewToWorldDir(R);
    float screenShare = confidence, cubeShare = 0.0, gridShare = 0.0, skyShare = 0.0;   // (DebugView 6)
    vec4 grid = vec4(0.0), env = vec4(0.0);
    if (CubeOn > 0.0 && confidence < 1.0) env = EnvAlong(worldR, roughness);
    float gridFirst = smoothstep(0.15, 0.35, roughness);
    bool gridUseful = (gridFirst > 0.0 || env.a < 1.0) && !(SkyFallback > 0.0 && RoofMaskOn < 0.5 && worldR.z >= 0.15);
    if (GridOn > 0.0 && ((confidence < 1.0 && gridUseful) || abs(DebugView - 5.0) < 0.5)) grid = GridAlong(ViewToWorldDir(position), worldR);
    if (confidence < 1.0 && (CubeOn > 0.0 || GridOn > 0.0)) {
        float missing = 1.0 - confidence;
        float cubeA = missing * env.a, gridA = (missing - cubeA) * grid.a;   // the cube first
        float gridB = missing * grid.a, cubeB = (missing - gridB) * env.a;   // the grid first
        cubeShare = mix(cubeA, cubeB, gridFirst);
        gridShare = mix(gridA, gridB, gridFirst);
        sum += env.rgb * cubeShare + grid.rgb * gridShare;
        confidence += cubeShare + gridShare;
    }
    if (confidence < 1.0 && SkyFallback > 0.0) {
        skyShare = (1.0 - confidence) * smoothstep(0.0, 0.15, worldR.z);
        skyShare *= mix(1.0, mix(cb.sky.y, 1.0, textureLod(sampler2D(t_skyVis, LINEAR_S), uv, 0.0).b), cb.sky.x);
        sum += SkyAlong(worldR) * skyShare;
        confidence += skyShare;
    }
    colour = sum / max(confidence, 1.0e-4);
    // Diagnostics: 5 what the light grid gives along every reflected ray, 6 where each share came from.
    if (DebugView > 4.5) {
        if (DebugView < 5.5) colour = grid.rgb;
        else colour = vec3(gridShare, screenShare, cubeShare) + 0.5 * skyShare;
        o_color = vec4(Tame(colour), 1.0);
        return;
    }
    // Fireflies: a lower cap on rough surfaces; everything after this pass averages in the compressed space (Tame).
    float brightest = max(colour.r, max(colour.g, colour.b));
    colour *= min(1.0, mix(BrightestReflection, 3.0, saturate(roughness * 2.0)) / max(brightest, 1.0e-4));
    o_color = vec4(Tame(colour) * confidence, confidence);
}
