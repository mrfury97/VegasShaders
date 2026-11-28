// CoverMap, UpdateTechnique (needs NVR DXVK): Effects/CoverMap.fx.hlsl UpdatePS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "CoverMapCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_coverMap;
layout(set = 1, binding = 1) uniform texture2D t_orthoMap;
layout(set = 1, binding = 2) uniform texture2D t_depth;
layout(set = 1, binding = 3) uniform texture2D t_coverAux;

layout(push_constant) uniform Push {
    uint pointWrap;
    uint pointClamp;
    uint pad[2];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_main;
layout(location = 1) out vec4 o_aux;

vec2 WindowPos(vec2 texel, vec2 origin) {
    return texel - origin + cb.update.x * (1.0 - step(origin, texel));
}

float KeptTexel(vec2 texel) {
    vec2 now = WindowPos(texel, cb.origin.xy);
    vec2 before = WindowPos(texel, cb.origin.zw);
    vec2 miss = abs(cb.shift.xy - (before - now));
    return cb.update.w * step(max(miss.x, miss.y), 0.25);
}

vec2 WrapTexel(vec2 texel) {
    return texel - cb.update.x * floor((texel + 0.5) * cb.update.y);
}

vec4 FetchTexel(vec2 texel) {
    return textureLod(sampler2D(t_coverMap, NVR_SAMPLER(pc.pointWrap)), (texel + 0.5) * cb.update.y, 0.0);
}

vec4 FetchAux(vec2 texel) {
    return textureLod(sampler2D(t_coverAux, NVR_SAMPLER(pc.pointWrap)), (texel + 0.5) * cb.update.y, 0.0);
}

float NeighbourHeight(vec2 texel, vec2 pos, vec2 offset, float none) {
    vec2 npos = pos + offset;
    vec2 ntexel = WrapTexel(texel + offset);
    float a = FetchAux(ntexel).r;
    float inside = step(0.0, min(npos.x, npos.y)) * step(max(npos.x, npos.y), cb.update.x - 1.0);
    return inside * KeptTexel(ntexel) * HeightKnown(a) > 0.5 ? DecodeHeight(a) : none;
}

float OrthoTop(vec2 rel, float guess, out float found) {
    vec4 o = cb.shadowOrtho * vec4(rel, guess, 1.0);
    o.xyz /= o.w;
    vec2 ouv = vec2(o.x * 0.5 + 0.5, o.y * -0.5 + 0.5);
    float d = textureLod(sampler2D(t_orthoMap, NVR_SAMPLER(pc.pointClamp)), ouv, 0.0).r;
    found = step(0.002, min(ouv.x, ouv.y)) * step(max(ouv.x, ouv.y), 0.998) * step(1.0e-6, d) * step(d, 0.99999);
    vec4 p = cb.orthoInverse * vec4(o.x, o.y, d, 1.0);
    return p.z / p.w;
}

vec3 ScreenPoint(vec2 ruv) {
    return toWorld(ruv) * readDepthLod(t_depth, pc.pointClamp, ruv);
}

float ScreenTop(vec2 rel, float guess, out float found) {
    found = 0.0;
    float z = guess;
    vec3 p = vec3(rel, guess);
    vec2 ruv = vec2(0.5, 0.5) * cb.renderScale.xy;
    float onScreen = 0.0;
    vec2 uvLo = vec2(0.002) * cb.renderScale.xy;
    vec2 uvHi = vec2(0.998) * cb.renderScale.xy;
    for (int i = 0; i < 3; i++) {
        vec3 view = (cb.viewTransform * vec4(rel, z, 0.0)).xyz;
        vec3 projected = projectPosition(view);
        ruv = projected.xy;
        vec2 s01 = ruv * cb.renderScale.zw;
        onScreen = step(cb.cameraData.x + 1.0, view.z) * step(0.002, min(s01.x, s01.y)) * step(max(s01.x, s01.y), 0.998);
        p = ScreenPoint(clamp(ruv, vec2(0.0), cb.renderScale.xy));
        z = p.z;
    }
    float depth = length(p);
    vec2 pixel = cb.reciprocalResolution.xy;
    vec3 px = ScreenPoint(clamp(ruv + vec2(pixel.x, 0.0), vec2(0.0), cb.renderScale.xy)) - p;
    vec3 py = ScreenPoint(clamp(ruv + vec2(0.0, pixel.y), vec2(0.0), cb.renderScale.xy)) - p;
    vec3 n = cross(px, py);
    n *= dot(n, -p) < 0.0 ? -1.0 : 1.0;
    float up = n.z / max(length(n), 1.0e-6);
    float match = length(p.xy - rel) < 1.5 * cb.update.z ? 1.0 : 0.0;
    found = onScreen * match * step(0.7, up) * step(60.0, depth) * step(readDepthLod(t_depth, pc.pointClamp, clamp(ruv, vec2(0.0), cb.renderScale.xy)), cb.cameraData.y * 0.98);
    return z;
}

void main() {
    float texelSize = cb.update.z;
    vec2 texel = floor(gl_FragCoord.xy);
    float kept = KeptTexel(texel);
    vec4 state = kept > 0.5 ? FetchTexel(texel) : vec4(cb.start.xyz, 0.0);
    vec4 aux = kept > 0.5 ? FetchAux(texel) : vec4(0.0, 1.0, 0.0, 0.0);
    vec2 pos = WindowPos(texel, cb.origin.xy);
    vec2 rel = cb.shift.zw + (pos + 0.5) * texelSize;

    float known = HeightKnown(aux.r);
    float height = known > 0.5 ? DecodeHeight(aux.r) : cb.footB.z;
    float source = aux.b;
    float orthoFound = 0.0;
    if (cb.shade.w > 0.5) {
        float found1, found2;
        float first = OrthoTop(rel, height, found1);
        float second = OrthoTop(rel, first, found2);
        orthoFound = found1 * found2;
        if (orthoFound > 0.5) {
            height = second;
            known = 1.0;
            source = 1.0;
        }
    }
    if (orthoFound < 0.5 && source < 0.75 && length(rel) < 4000.0) {
        float seen;
        float top = ScreenTop(rel, height, seen);
        if (seen > 0.5) {
            height = top;
            known = 1.0;
            source = 0.5;
        }
    }

    float sunlit = 1.0;
    vec2 sunFlat = cb.sun.xy;
    float sunFlatLength = length(sunFlat);
    if (known > 0.5 && cb.sun.w > 0.0 && cb.sun.z > 0.0 && sunFlatLength > 0.02) {
        vec2 towards = sunFlat / sunFlatLength;
        float rise = cb.sun.z / sunFlatLength;
        for (int k = 0; k < 6; k++) {
            float dist = 12.0 * exp2(float(k));
            vec2 offset = floor(towards * (dist / texelSize) + 0.5);
            float other = NeighbourHeight(texel, pos, offset, -1.0e9);
            sunlit *= other > height + dist * rise + 16.0 ? 0.0 : 1.0;
        }
    }

    float drift = 1.0;
    if (known > 0.5 && cb.snowRates.x > 0.0 && cb.snowRates.z > 0.0) {
        float reach = max(floor(32.0 / texelSize + 0.5), 1.0);
        float h0 = NeighbourHeight(texel, pos, vec2(reach, 0.0), 1.0e9);
        float h1 = NeighbourHeight(texel, pos, vec2(-reach, 0.0), 1.0e9);
        float h2 = NeighbourHeight(texel, pos, vec2(0.0, reach), 1.0e9);
        float h3 = NeighbourHeight(texel, pos, vec2(0.0, -reach), 1.0e9);
        vec4 h = vec4(h0, h1, h2, h3);
        vec4 valid = step(h, vec4(1.0e8));
        float count = dot(valid, vec4(1.0));
        float mean = dot(h * valid, vec4(1.0)) / max(count, 1.0);
        drift = count > 0.5 ? 1.0 + cb.snowRates.z * clamp((mean - height) / 32.0, -1.0, 1.0) : 1.0;
    }

    float sunWet = mix(cb.shade.x, 1.0, sunlit * cb.sun.w);
    float sunSnow = mix(cb.shade.y, 1.0, sunlit * cb.sun.w);
    float wet = saturate(state.r + cb.rates.x - cb.rates.y * sunWet);
    float puddle = saturate(state.g + cb.rates.z - cb.rates.w * sunWet);
    float snow = saturate(state.b + cb.snowRates.x * drift - cb.snowRates.y * sunSnow);

    float trodden = state.a;
    if (cb.snowRates.w > 0.0) {
        vec2 a = cb.footA.xy;
        vec2 ab = cb.footB.xy - a;
        float t = saturate(dot(rel - a, ab) / max(dot(ab, ab), 0.0001));
        vec2 q = a + ab * t;
        float feetZ = mix(cb.footA.z, cb.footB.z, t);
        float stroke = saturate(1.0 - length(rel - q) / cb.footB.w);
        stroke *= known > 0.5 ? 1.0 - smoothstep(40.0, 80.0, abs(height - feetZ)) : 1.0;
        trodden = max(trodden, stroke);
    }
    trodden = saturate(trodden - 3.0 * cb.snowRates.x) * saturate(snow * 20.0);

    o_main = vec4(wet, puddle, snow, trodden);
    o_aux = vec4(known > 0.5 ? EncodeHeight(height) : 0.0, sunlit, known > 0.5 ? source : 0.0, 0.0);
}
