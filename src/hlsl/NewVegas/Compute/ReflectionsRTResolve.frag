// UNOFFICIAL 2026-10-28 (NVR DXVK, RT reflections; ReflectionsRT.glsl has the whole frame): Reflections technique 1 (Trace, half
// resolution), second half, with RT on: reads RayGen's three outputs and the RT helper's hits, and writes what ReflectionsTrace writes.
// A hit fills the whole missing share (times the roughness fade), from the first source that can show it:
//   the screen at the hit when the hit is visible there (exact: the lit picture; what the march misses through its steps / thickness);
//   the surroundings cube toward the hit when it is out of view (the cube was seen from about here);
//   else (hidden behind something in view, or never seen) the estimate, RtShadeHit (the cube toward a hidden hit shows what hides it).
// So a ray that meets something never takes the cube along it or the sky (no sky in a floor under a roof). A ray that met nothing
// within Distance keeps the old fallbacks and takes the sky without the sky-visibility guess. From the fallbacks on: ReflectionsTrace.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"
#include "ReflectionsRT.glsl"

layout(location = 1) out vec4 o_spread;   // 2026-10-29: x the hit's distance share (0 contact .. 1 far / unknown) for the blur passes;
                                          // 2026-11-07 (G16R16F) y the hit distance for the RT reflection denoiser (0 unknown, Distance: nothing hit)

// 2026-10-31: one RT hit's colour as seen along the reflection (the screen at the hit when it is visible there, else the cube toward
// it when it is out of view, else the estimate), and which source it came from (x screen, y cube share; the rest the estimate).
// 2026-11-07 (perf-reflfix) the screen at the hit only where the screen can show that face: a hit whose (smooth) normal faces away from
// the camera is the back of what the screen shows there (a mirror sees a sphere's far side; within the depth tolerance near its outline
// it took the front's colour). And MetalBounce (hit flag 256): the hit is smooth metal; its colour is the tint x what the helper's one
// mirror ray from it met (bounce): the screen there, else that hit's estimate (normal: facing the bounce ray), or the sky along it.
bool RtScreenShows(vec3 hitW, vec3 facing, bool facingKnown, out vec2 at) {
    vec3 hitV = WorldToViewDir(hitW);   // camera-relative: a direction transform is the whole move
    at = ToScreen(hitV);
    bool inView = hitV.z > nearZ && all(greaterThan(at, vec2(0.0))) && all(lessThan(at, cb.renderScale.xy));   // (2026-11-03: the drawn part)
    if (facingKnown && dot(facing, -normalize(hitW)) < -0.15) return false;   // (a normal map may tilt the front a little away)
    return inView && abs(readDepthLod(at) - hitV.z) < max(hitV.z * RtTolerance, 8.0) && !IsWeapon(at);
}
vec3 RtBounceColour(vec3 hitW, vec4 hitData, vec4 shade, vec4 bounce, float roughness, out vec2 source) {
    uint hitFlags = uint(hitData.y + 0.5);
    vec3 d2 = OctDecode(hitData.zw);
    source = vec2(0.0, 0.0);
    if ((hitFlags & 512u) != 0u || bounce.a <= 0.0) {   // the bounce met nothing within the reach: the sky (or what lies that way)
        vec3 sky = SkyPanoOn ? PanoAlong(d2) : CubeOn > 0.0 ? EnvAlong(d2, roughness).rgb : SkyAlong(d2);
        return bounce.rgb * sky;
    }
    vec3 p2 = hitW + d2 * bounce.a;
    vec2 at;
    if (RtScreenShows(p2, vec3(0.0), false, at)) {
        source = vec2(1.0, 0.0);
        return bounce.rgb * linearize(textureLod(sampler2D(t_rendered, LINEAR_S), at, 0.0).rgb);
    }
    int keyLamp = int((hitFlags >> 4u) & 15u) - 1;
    vec3 shaded = RtShadeHit(p2, -d2, (hitFlags & 2u) != 0u ? shade.rgb : vec3(0.4), shade.a, keyLamp);
    if ((hitFlags & 8u) != 0u) shaded += shade.rgb * rtc.rt3.w;
    return bounce.rgb * shaded;
}
vec3 RtHitColour(vec3 hitW, vec4 hitData, vec4 shade, vec4 bounce, vec3 worldR, float roughness, out vec2 source) {
    uint hitFlags = uint(hitData.y + 0.5);
    if ((hitFlags & 256u) != 0u) return RtBounceColour(hitW, hitData, shade, bounce, roughness, source);
    vec3 hitV = WorldToViewDir(hitW);
    vec2 at;
    bool inView = hitV.z > nearZ && all(greaterThan(ToScreen(hitV), vec2(0.0))) && all(lessThan(ToScreen(hitV), cb.renderScale.xy));
    if (RtScreenShows(hitW, OctDecode(hitData.zw), (hitFlags & 4u) != 0u, at)) {
        source = vec2(1.0, 0.0);
        return linearize(textureLod(sampler2D(t_rendered, LINEAR_S), at, 0.0).rgb);
    }
    vec4 env = !inView && CubeOn > 0.0 ? EnvAlong(normalize(hitW), roughness) : vec4(0.0);
    // 2026-11-05 ShadeOffscreen: an out-of-view hit whose draw's colour the helper knows is shaded from itself (its texture or average
    // colour, its normal, the sun through its shadow ray, the lamps) instead of the cube toward it: the cube is seen from near the camera,
    // 64 px, a little behind in time, so the people, plants and things behind you showed as a smear of what lay that way (or nothing)
    if (rtc.rt4.w > 0.5 && (hitFlags & 2u) != 0u) env.a = 0.0;
    vec3 hitNormal = (hitFlags & 4u) != 0u ? OctDecode(hitData.zw) : -worldR;
    int keyLamp = int((hitFlags >> 4u) & 15u) - 1;   // -1: none (outdoors by day shade.a is the sun's)
    vec3 shaded = RtShadeHit(hitW, hitNormal, shade.rgb, shade.a, keyLamp);
    // 2026-10-29: a glowing draw (the core's record flag: emissive material) gives its own light too (neon, lit glass).
    if ((hitFlags & 8u) != 0u) shaded += shade.rgb * rtc.rt3.w;
    source = vec2(0.0, env.a);
    return env.rgb * env.a + shaded * (1.0 - env.a);
}

// 2026-10-31 RayReuse: the GGX distribution (a = roughness^2) and the pdf a direction was sampled with (normal-distribution
// sampling: D(h) n.h / (4 v.h); the VNDF pdf differs by a factor near 1 at these roughnesses).
float GgxD(float NoH, float a) { float a2 = a * a; float d = NoH * NoH * (a2 - 1.0) + 1.0; return a2 / (3.14159265 * d * d + 1.0e-7); }
float GgxPdf(vec3 N, vec3 V, vec3 L, float a) {
    vec3 H = normalize(V + L);
    return GgxD(saturate(dot(N, H)), a) * saturate(dot(N, H)) / (4.0 * max(dot(V, H), 1.0e-3));
}

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 uv = (vpos + 0.5) * LowTexel;
    vec2 ray = texelFetch(sampler2D(t_rtRay, POINT_S), texel, 0).xy;
    o_spread = vec4(1.0, 0.0, 0.0, 0.0);
    if (ray.x <= 0.0) { o_color = vec4(0.0); return; }   // RayGen: nothing reflects here
    vec4 march = texelFetch(sampler2D(t_rtMarch, POINT_S), texel, 0);
    vec4 hitData = texelFetch(sampler2D(t_rtHit, POINT_S), texel, 0);
    vec4 shade = texelFetch(sampler2D(t_rtShade, POINT_S), texel, 0);
    vec4 bounce = texelFetch(sampler2D(t_rtBounce, POINT_S), texel, 0);   // (2026-11-07 perf-reflfix: read where hit.y has 256)
    vec3 worldR = OctDecode(texelFetch(sampler2D(t_rtDir, POINT_S), texel, 0).xy);
    bool weaponRay = ray.y > 5.5;   // 2026-10-30 RT weapon reflections: a weapon pixel's own ray (RayGen +6)
    bool borrowed = !weaponRay && ray.y > 3.5;   // a rough pixel whose block's ray this frame was another pixel's (RayGen's budget)
    bool rayCast = ray.y > 1.5;
    float roughness = ray.y - (weaponRay ? 6.0 : borrowed ? 4.0 : rayCast ? 2.0 : 0.0);
    vec2 pixelUV = uv - 0.5 * (LowTexel - cb.reciprocalResolution.xy);   // RayGen's pixel centre in the block
    vec3 surfaceW = ViewToWorldDir(SurfacePosition(pixelUV, ray.x));    // camera-relative (2026-10-30: the weapon's own FOV)
    vec3 borrowHitW = vec3(0.0);
    if (borrowed) {
        // The block's casting pixel this frame (the same rule as RayGen): its hit point, a world point, shaded from here as our own.
        ivec2 caster = (texel & ~1) + ivec2((int(NoiseFrame) & 3) & 1, (int(NoiseFrame) & 3) >> 1);
        vec2 cRay = texelFetch(sampler2D(t_rtRay, POINT_S), caster, 0).xy;
        vec4 cHit = texelFetch(sampler2D(t_rtHit, POINT_S), caster, 0);
        if (cRay.y > 1.5 && cRay.y < 3.5 && cHit.x >= 0.0) {
            vec2 cUV = (vec2(caster) + 0.5) * LowTexel - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
            vec3 cSurface = ViewToWorldDir(ViewPosition(cUV, cRay.x));
            vec3 cDir = OctDecode(texelFetch(sampler2D(t_rtDir, POINT_S), caster, 0).xy);
            hitData = cHit;
            shade = texelFetch(sampler2D(t_rtShade, POINT_S), caster, 0);
            bounce = texelFetch(sampler2D(t_rtBounce, POINT_S), caster, 0);
            borrowHitW = cSurface + cDir * cHit.x;
            if (cHit.x > 0.0) hitData.x = max(length(borrowHitW - surfaceW), 1.0e-3);   // our distance to it (for the views / the miss test)
            rayCast = true;
        }
        else rayCast = false;   // no usable ray in the block: the old fallbacks
    }
    uint hitFlags = uint(hitData.y + 0.5);
    vec3 colour = march.rgb;
    float confidence = march.a;

    vec3 sum = colour * confidence;
    float rtScreenShare = 0.0, rtCubeShare = 0.0, rtGuessShare = 0.0;   // (RT DebugView 1)
    float rtT = -1.0;   // -1 no ray, 0 a ray that met nothing within Distance, else the hit distance (RT DebugView 2)
    float rtInstance = -1.0;
    bool rtOpen = false;   // a ray went the whole Distance without a hit
    if (abs(RtDebugView - 3.0) < 0.5) { sum = vec3(0.0); confidence = 0.0; }   // RT DebugView 3: the screen march's answer dropped
    float rtFade = RtFade(roughness);
    if (rayCast && hitData.x >= 0.0 && RtDebugView < 4.5) {
        float missing = (1.0 - confidence) * rtFade;
        if (hitData.x > 0.0) {
            rtT = hitData.x;
            // The hit's distance against the eye's (a reflection's blur grows with how far the reflected thing is from the surface).
            o_spread = vec4(saturate(hitData.x / (hitData.x + ray.x * rtc.rt3.z)), 0.0, 0.0, 0.0);
            rtInstance = float(hitFlags & 15u);   // (RT DebugView 4: a colour per flag combination: land, albedo known, normal known)
            vec3 hitW = borrowed ? borrowHitW : surfaceW + worldR * hitData.x;
            vec2 source;
            vec3 hitColour = RtHitColour(hitW, hitData, shade, bounce, worldR, roughness, source);
            rtScreenShare = missing * source.x;
            rtCubeShare = missing * (1.0 - source.x) * source.y;
            rtGuessShare = missing - rtScreenShare - rtCubeShare;
            // 2026-10-31 RayReuse (rt4.y, from roughness rt4.z): a rough pixel takes its neighbours' hits as more samples of its own
            // lobe (stochastic SSR's ratio estimator: each hit re-seen from here, weighted by this pixel's GGX lobe toward it over the
            // pdf the neighbour drew it with), with depth / normal stops. Four more samples for the cost of shading them; no rays.
            if (rtc.rt4.y > 0.5 && roughness >= rtc.rt4.z && !weaponRay) {
                vec3 N = ViewToWorldDir(ViewNormal(pixelUV));
                vec3 V = -normalize(surfaceW);
                float a = max(roughness * roughness, 1.0e-3);
                vec3 total = hitColour;   // the own sample: its own lobe over its own pdf = 1
                float weights = 1.0;
                // (with RoughShare only one pixel a 2x2 block casts, the same one in every block: even steps land on other blocks' casters)
                int spread = rtc.rt3.y > 0.0 && roughness > rtc.rt3.y ? 2 * (1 + int(roughness * 1.5)) : 1 + int(roughness * 3.0 + 0.5);
                int turn = int(NoiseFrame) & 1;
                for (int k = 0; k < 4; k++) {
                    ivec2 o = turn == 0 ? ivec2(k < 2 ? (k == 0 ? 1 : -1) : 0, k < 2 ? 0 : (k == 2 ? 1 : -1))
                                        : ivec2(k == 0 || k == 3 ? 1 : -1, k < 2 ? 1 : -1);
                    ivec2 nb = texel + o * spread;
                    vec2 nRay = texelFetch(sampler2D(t_rtRay, POINT_S), nb, 0).xy;
                    if (nRay.x <= 0.0 || nRay.y < 1.5 || nRay.y > 3.5) continue;   // no own ray there this frame (or a weapon / borrower)
                    if (abs(nRay.x - ray.x) > ray.x * 0.05 + 4.0) continue;        // another surface (depth stop)
                    vec4 nHit = texelFetch(sampler2D(t_rtHit, POINT_S), nb, 0);
                    if (nHit.x <= 0.0) continue;
                    vec2 nUV = (vec2(nb) + 0.5) * LowTexel - 0.5 * (LowTexel - cb.reciprocalResolution.xy);
                    vec3 nN = ViewToWorldDir(ViewNormal(nUV));
                    if (dot(nN, N) < 0.85) continue;                               // another facing (normal stop)
                    vec3 nSurface = ViewToWorldDir(ViewPosition(nUV, nRay.x));
                    vec3 nDir = OctDecode(texelFetch(sampler2D(t_rtDir, POINT_S), nb, 0).xy);
                    vec3 nHitW = nSurface + nDir * nHit.x;
                    vec3 L = normalize(nHitW - surfaceW);
                    if (dot(L, N) <= 0.0) continue;
                    float nRough = nRay.y - 2.0;
                    float nA = max(nRough * nRough, 1.0e-3);
                    float w = GgxPdf(N, V, L, a) / max(GgxPdf(nN, -normalize(nSurface), nDir, nA), 1.0e-6);
                    w = min(w, 4.0);   // no single neighbour takes over (fireflies)
                    vec2 nSource;
                    total += RtHitColour(nHitW, nHit, texelFetch(sampler2D(t_rtShade, POINT_S), nb, 0), texelFetch(sampler2D(t_rtBounce, POINT_S), nb, 0),
                        L, roughness, nSource) * w;
                    weights += w;
                }
                hitColour = total / max(weights, 1.0e-6);
            }
            sum += hitColour * missing;
            confidence += missing;
        }
        else { rtT = 0.0; rtOpen = rtFade >= 1.0; }
    }
    // What the ray did not find on screen: the surroundings cube and (GridFallback) the light grid along it, then (outdoors)
    // the sky if it points up into it. Each fills only the share still missing (premultiplied sums).
    float screenShare = confidence, cubeShare = 0.0, gridShare = 0.0, skyShare = 0.0;   // (DebugView 6)
    // UNOFFICIAL 2026-11-04 (rtwater) the water (RT Water): a ray that met nothing within Distance takes no fallback here, so its share
    // stays missing and the water shader shows the planar map there (Includes/Water.hlsl getRTWaterReflection: by the confidence), which
    // reaches the far shore and the LOD land; before, the far cliffs of og-lake came out as the sky panorama (they are past Distance).
    // (2026-11-05 rtfull-lake2: not with WaterSkipPlanar: the planar map is not drawn then and the water shader paints the horizon
    // colour there, so every missed ray (the sky, the far cliffs) turned the whole lake one flat pale colour: a miss takes the sky)
    bool waterMiss = WaterRtOn && !WaterPlanarSkipped && rtOpen && abs(cb.cameraPosition.z + surfaceW.z - cb.waterSettings.x) < max(2.0, ray.x * 0.002);
    vec4 grid = vec4(0.0), env = vec4(0.0);
    // UNOFFICIAL 2026-10-31 SkyPanorama: a ray that met nothing on its way out is the sky (or something past Distance in the haze):
    // the panorama as seen, all of the missing share, no horizon fade; not the cube (seen from the camera, it holds whatever lay
    // that way from there) and not the grid.
    if (SkyPanoOn && rtOpen && confidence < 1.0 && !waterMiss) {
        skyShare = 1.0 - confidence;
        sum += PanoAlong(worldR) * skyShare;
        confidence = 1.0;
    }
    if (CubeOn > 0.0 && confidence < 1.0 && !waterMiss) env = EnvAlong(worldR, roughness);
    float gridFirst = smoothstep(0.15, 0.35, roughness);
    bool gridUseful = (gridFirst > 0.0 || env.a < 1.0) && !(SkyFallback > 0.0 && RoofMaskOn < 0.5 && worldR.z >= 0.15);
    if (GridOn > 0.0 && !waterMiss && ((confidence < 1.0 && gridUseful) || abs(DebugView - 5.0) < 0.5)) grid = GridAlong(surfaceW, worldR);
    if (confidence < 1.0 && (CubeOn > 0.0 || GridOn > 0.0) && !waterMiss) {
        float missing = 1.0 - confidence;
        float cubeA = missing * env.a, gridA = (missing - cubeA) * grid.a;   // the cube first
        float gridB = missing * grid.a, cubeB = (missing - gridB) * env.a;   // the grid first
        cubeShare = mix(cubeA, cubeB, gridFirst);
        gridShare = mix(gridA, gridB, gridFirst);
        sum += env.rgb * cubeShare + grid.rgb * gridShare;
        confidence += cubeShare + gridShare;
    }
    if (confidence < 1.0 && SkyFallback > 0.0 && !waterMiss) {
        skyShare = (1.0 - confidence) * smoothstep(0.0, 0.15, worldR.z);
        // (RT: a ray that met nothing within Distance needs no guess at how much of the sky is hidden.)
        if (!rtOpen) skyShare *= mix(1.0, mix(cb.sky.y, 1.0, textureLod(sampler2D(t_skyVis, LINEAR_S), uv, 0.0).b), cb.sky.x);
        sum += SkyAlong(worldR) * skyShare;
        confidence += skyShare;
    }
    colour = sum / max(confidence, 1.0e-4);
    // UNOFFICIAL 2026-11-07 the RT reflection denoiser's hit distance (o_spread.y): the march's where it answered most, else the ray's,
    // Distance where the ray met nothing, 0 where nothing is known (the fallbacks alone)
    if (RtDenoiserOn) {
        float marchHit = texelFetch(sampler2D(t_rtMarchHit, POINT_S), texel, 0).x;
        o_spread.y = march.a >= 0.5 && marchHit > 0.0 ? marchHit : rtT > 0.0 ? rtT : rtT == 0.0 ? RtDistance : marchHit;
    }
    // RT diagnostics (the effect's DebugView is 6 then, so the composite shows these as they are):
    // 1 where each share came from: green the screen march, cyan RT on screen, blue RT through the cube, magenta RT estimate,
    //   grey the old fallbacks (cube, grid, sky), black nothing;
    // 2 the RT hit distance (white near .. dark at Distance), red a ray that met nothing, black no ray (screen enough / too rough);
    // 4 the hit instance (a colour per instance index: checks the scene), black no hit.
    // 5 the scene alignment check (the helper traced from the camera toward each reflecting pixel's surface): green the hit is that
    //   surface (within 1 % or 4 units), blue something nearer, yellow only something farther, red nothing; black no ray.
    if (RtDebugView > 0.5 && abs(RtDebugView - 3.0) > 0.5) {
        if (RtDebugView < 1.5) {
            float marchShare = confidence - rtScreenShare - rtCubeShare - rtGuessShare - cubeShare - gridShare - skyShare;
            colour = vec3(0.0, 1.0, 0.0) * marchShare + vec3(0.0, 1.0, 1.0) * rtScreenShare + vec3(0.0, 0.3, 1.0) * rtCubeShare +
                vec3(1.0, 0.0, 1.0) * rtGuessShare + vec3(0.5) * (cubeShare + gridShare + skyShare);
        }
        else if (RtDebugView < 2.5) colour = rtT < 0.0 ? vec3(0.0) : rtT == 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(1.0 - saturate(rtT / RtDistance) * 0.9);
        else if (RtDebugView > 6.5) colour = rayCast ? worldR * 0.5 + 0.5 : vec3(0.0);   // 7 (2026-11-05) the reflected direction (world axes: blue = up)
        else if (RtDebugView > 5.5) {
            // 6 (2026-10-30) the fallbacks split: red the sky gradient (no clouds), blue the surroundings cube (holds the clouds where it
            // saw them), grey the light grid, green everything the screen march or the rays answered.
            float answered = confidence - cubeShare - gridShare - skyShare;
            colour = vec3(1.0, 0.0, 0.0) * skyShare + vec3(0.0, 0.3, 1.0) * cubeShare + vec3(0.5) * gridShare + vec3(0.0, 1.0, 0.0) * answered;
        }
        else if (RtDebugView > 4.5) {
            float expected = length(surfaceW), t = hitData.x;
            colour = !rayCast || t < 0.0 ? vec3(0.0) : t == 0.0 ? vec3(1.0, 0.0, 0.0) :
                abs(t - expected) < max(4.0, expected * 0.01) ? vec3(0.0, 1.0, 0.0) : t < expected ? vec3(0.0, 0.3, 1.0) : vec3(1.0, 1.0, 0.0);
        }
        else {
            uint k = uint(max(rtInstance, 0.0)) * 2654435761u;
            colour = rtInstance < 0.0 ? vec3(0.0) : vec3(float(k & 255u), float((k >> 8) & 255u), float((k >> 16) & 255u)) / 255.0;
        }
        o_color = vec4(colour, 1.0);
        return;
    }
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
    // UNOFFICIAL 2026-10-30: never hand a non-finite value on (the temporal history would keep it: the weapon's history is screen-locked).
    if (any(isnan(o_color)) || any(isinf(o_color))) o_color = vec4(0.0);
}
