// WetnessReflections as a Vulkan pass (needs NVR DXVK): Effects/WetnessReflections.fx.hlsl line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;
layout(set = 1, binding = 1) uniform texture2D t_depth;
layout(set = 1, binding = 2) uniform texture2D t_normals;
layout(set = 1, binding = 3) uniform texture2D t_ortho;
layout(set = 1, binding = 4) uniform texture2D t_shadingNormals;
layout(set = 1, binding = 5) uniform texture2D t_depthVM;

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;
    mat4 invProjectionTransform;
    mat4 invViewTransform;
    mat4 shadowOrtho;
    vec4 reciprocalResolution;
    vec4 depthConstants;
    vec4 cameraData;
    vec4 renderScale;
    vec4 linearLighting;
    vec4 wetData;
    vec4 wetFade;
    vec4 wetBump;
    vec4 wetnessData;
    vec4 wetnessParams;
    vec4 wetnessParams2;
    vec4 wetnessDrops;
    vec4 wetnessRipple;
    vec4 wetnessCamera;
    vec4 wetnessFlags;
    vec4 puddleData;
    vec4 puddleLook;
    vec4 puddleShape;
    vec4 skyColor;
    vec4 skyLowColor;
    vec4 horizonColor;
    vec4 sunPosition;
    vec4 skyData;
    vec4 sunAmount;
    vec4 sunDiskColor;
    vec4 sunsetColor;
    vec4 skyIrradiance[9];
} cb;

layout(push_constant) uniform Push {
    uint linearSampler;
    uint pointSampler;
    uint pad[2];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

const float SSR_REFINE = 5.0;

float compress(float x) { return saturate(x); }
float shade(vec3 a, vec3 b) { return saturate(dot(a, b)); }

vec3 LinColor(vec3 c) { return cb.linearLighting.x > 1.0 ? pow(max(c, vec3(0.0)), vec3(cb.linearLighting.x)) : c; }
vec3 LinEncode(vec3 c) { return cb.linearLighting.x > 1.0 ? pow(max(c, vec3(0.0)), vec3(cb.linearLighting.y)) : c; }

vec3 GetSunColor(float sunHeight, float atmosphere, float dayTime, vec3 sunColor, vec3 sunsetColor) {
    sunColor = linearize(sunColor);
    sunsetColor = linearize(sunsetColor);
    vec3 rgbAcc = (1.0 + sunHeight) * sunColor;
    float sunSet = saturate(pows(1.0 - sunHeight, 8.0)) * dayTime;
    rgbAcc += sunsetColor * sunSet * atmosphere;
    return rgbAcc;
}

vec3 GetSkyColor(float verticality, float atmosphere, float sunHeight, float sunInfluence, float sunStrength,
                 vec3 skyColor, vec3 skyLowColor, vec3 horizonColor, vec3 sunColor) {
    skyColor = linearize(skyColor);
    skyLowColor = linearize(skyLowColor);
    horizonColor = linearize(horizonColor);
    float isDayTime = smoothstep(0.0, 0.5, cb.sunAmount.x);
    vec3 rgbAcc = mix(skyLowColor, skyColor, verticality);
    rgbAcc = mix(rgbAcc, horizonColor, saturate(atmosphere * (0.5 + sunInfluence * 0.5)));
    rgbAcc += mix(vec3(0.0), sunColor * sunInfluence * (1.0 - sunHeight) * (((1.0 - verticality) + atmosphere) * 0.5) * sunStrength, isDayTime);
    return rgbAcc;
}

vec3 SkyAmbientRadiance(vec3 N, float lean) {
    if (cb.wetBump.y > 0.5) {
        vec3 zenith = vec3(0.0, 0.0, 1.0);
        vec3 mixed = mix(zenith, N, lean);
        float mixedLen = length(mixed);
        vec3 look = (mixedLen > 1.0e-4) ? (mixed / mixedLen) : zenith;
        float seen = 0.5 * dot(N, zenith) + 0.5;
        float height = pows(compress(dot(look, zenith)), 3.0);
        float sunUp = shade(cb.sunPosition.xyz, zenith);
        float haze = pows(1.0 - height, 2.0) * cb.skyData.x;
        float towardSun = pows(compress(dot(look, cb.sunPosition.xyz)), 1.0 / max(cb.skyData.y, 1.0e-4));
        vec3 disc = GetSunColor(sunUp, cb.skyData.x, cb.sunAmount.x, cb.sunDiskColor.rgb, cb.sunsetColor.rgb);
        vec3 skyRad = GetSkyColor(height, haze, sunUp, towardSun, cb.skyData.z, cb.skyColor.rgb, cb.skyLowColor.rgb, cb.horizonColor.rgb, disc);
        return sqrt(max(skyRad, vec3(0.0))) * seen;
    }
    vec3 E = cb.skyIrradiance[0].rgb
        + cb.skyIrradiance[1].rgb * N.y
        + cb.skyIrradiance[2].rgb * N.z
        + cb.skyIrradiance[3].rgb * N.x
        + cb.skyIrradiance[4].rgb * (N.x * N.y)
        + cb.skyIrradiance[5].rgb * (N.y * N.z)
        + cb.skyIrradiance[6].rgb * (3.0 * N.z * N.z - 1.0)
        + cb.skyIrradiance[7].rgb * (N.x * N.z)
        + cb.skyIrradiance[8].rgb * (N.x * N.x - N.y * N.y);
    return sqrt(max(E, vec3(0.0)));
}

vec3 WetHash33(vec3 p3) {
    p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}

vec3 WetGradient(vec3 cell) {
    vec3 h = WetHash33(cell) * 2.0 - 1.0;
    return normalize(h + 1.0e-4);
}

float WetPerlin(vec3 p) {
    vec3 i = floor(p);
    vec3 f = p - i;
    float v1 = dot(WetGradient(i), f);
    float v2 = dot(WetGradient(i + vec3(1, 0, 0)), f - vec3(1, 0, 0));
    float v3 = dot(WetGradient(i + vec3(0, 1, 0)), f - vec3(0, 1, 0));
    float v4 = dot(WetGradient(i + vec3(1, 1, 0)), f - vec3(1, 1, 0));
    float v5 = dot(WetGradient(i + vec3(0, 0, 1)), f - vec3(0, 0, 1));
    float v6 = dot(WetGradient(i + vec3(1, 0, 1)), f - vec3(1, 0, 1));
    float v7 = dot(WetGradient(i + vec3(0, 1, 1)), f - vec3(0, 1, 1));
    float v8 = dot(WetGradient(i + vec3(1, 1, 1)), f - vec3(1, 1, 1));
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(v1, v2, u.x), mix(v3, v4, f.x), u.y),
               mix(mix(v5, v6, u.x), mix(v7, v8, f.x), u.y), u.z);
}

vec3 WetReorient(vec3 n1, vec3 n2) {
    n1 += vec3(0.0, 0.0, 1.0);
    n2 *= vec3(-1.0, -1.0, 1.0);
    return normalize(n1 * dot(n1, n2) / n1.z - n2);
}

float WetnessOcclusion(vec3 position, vec3 normal) {
    float texel = cb.wetnessFlags.w;
    if (texel <= 0.0) return 1.0;
    float lift = (cb.puddleData.w > 0.5) ? mix(2.0, 40.0, saturate(1.0 - abs(normal.z) * 1.5)) : 2.0;
    vec4 o = cb.shadowOrtho * vec4(position + normal * lift + vec3(0.0, 0.0, 2.0), 1.0);
    o.xyz /= o.w;
    if (any(greaterThan(abs(o.xy), vec2(1.0))) || o.z < 0.0 || o.z > 1.0) return 1.0;
    vec2 ouv = vec2(o.x * 0.5 + 0.5, o.y * -0.5 + 0.5);
    vec3 worldPos = cb.wetnessCamera.xyz + position;
    vec2 wander = vec2(WetPerlin(worldPos * 0.045), WetPerlin(worldPos.yxz * 0.045 + 17.3));
    ouv += wander * texel * 1.6;

    vec2 cell = ouv / texel - 0.5;
    vec2 f = fract(cell);
    vec2 base = (floor(cell) + 0.5 - 1.0) * texel;
    float wx[4] = float[4](1.0 - f.x, 1.0, 1.0, f.x);
    float wy[4] = float[4](1.0 - f.y, 1.0, 1.0, f.y);
    float depth = o.z - 0.0005;
    float open = 0.0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 tapUv = base + vec2(float(x), float(y)) * texel;
            float lit = textureLod(sampler2D(t_ortho, NVR_SAMPLER(pc.pointSampler)), tapUv, 0.0).r > depth ? 1.0 : 0.0;
            open += lit * wx[x] * wy[y];
        }
    }
    return smoothstep(0.0, 1.0, open / 9.0);
}

float PuddleHash(vec2 p) { return WetHash33(vec3(p, 7.13)).x; }
float PuddleValueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = PuddleHash(i), b = PuddleHash(i + vec2(1.0, 0.0));
    float c = PuddleHash(i + vec2(0.0, 1.0)), d = PuddleHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float PuddleField(vec2 worldXY) {
    vec2 p = worldXY * cb.puddleData.z;
    float n = PuddleValueNoise(p) * 0.62 + PuddleValueNoise(p * 2.31 + 17.7) * 0.38;
    if (cb.puddleData.w > 2.5) n = n * 0.85 + PuddleValueNoise(p * 5.7 + 41.3) * 0.15;
    return smoothstep(0.2, 0.8, n);
}

struct Wetness {
    float gloss;
    float roughness;
    vec3 normal;
};

Wetness GetWetness(vec3 position, vec3 vertexNormal, vec3 normal) {
    Wetness w;
    w.gloss = 0.0;
    w.roughness = 1.0;
    w.normal = normal;
    if (cb.wetnessFlags.x <= 0.5) return w;

    vec3 worldPos = cb.wetnessCamera.xyz + position;
    float dist = length(position);
    float minWetnessAngle = saturate(max(cb.wetnessParams.z, vertexNormal.z));
    float occlusion = WetnessOcclusion(position, vertexNormal);
    float flatness = smoothstep(cb.wetnessParams2.x, 1.0, minWetnessAngle);

    float rainWetness = cb.wetnessData.x * minWetnessAngle * cb.wetnessParams.x;
    float wetness = rainWetness;
    float puddleWetness = cb.wetnessData.y * minWetnessAngle;
    float puddle = wetness;
    if (wetness > 0.0 || puddleWetness > 0.0) {
        vec3 coords = (worldPos * 0.5 + 0.5) * 0.01 / cb.wetnessParams.w;
        puddle = WetPerlin(coords) * 0.5 + 0.5;
        puddle = puddle * ((minWetnessAngle / cb.wetnessParams2.x) * cb.wetnessParams.y * 0.25) + 0.5;
        puddle *= mix(wetness, puddleWetness, saturate(puddle - 0.25));
    }

    float nearFactor = smoothstep(4096.0 * 2.5, 0.0, dist);
    puddle *= saturate(occlusion * 2.0) * nearFactor;

    if (cb.puddleData.w > 0.5) {
        float open = saturate(occlusion * 2.0) * nearFactor;
        float level = cb.puddleData.x;
        float standing = 0.0, ring = 0.0;
        float rain = saturate(rainWetness) * open;
        puddle = rain;
        float rough = mix(1.0, 0.22, rain);
        if (level > 0.001) {
            vec2 p = worldPos.xy * cb.puddleData.z;
            float field = PuddleField(worldPos.xy);
            float t = 1.0 - level * cb.puddleData.y;
            float wander = PuddleValueNoise(p * 3.7 + 5.1);
            float soft = max(cb.puddleLook.z, 1.0e-3) * (0.5 + wander);
            float slope = smoothstep(cb.puddleShape.x, cb.puddleShape.y, vertexNormal.z);
            float shore = saturate((field - t) / soft);
            standing = shore * shore * (3.0 - 2.0 * shore) * slope * open;
            if (cb.puddleData.w > 1.5) {
                float ringW = max(cb.puddleShape.w, 1.0e-3) * (0.35 + 1.3 * wander);
                ring = saturate((field - t + ringW) / ringW) * smoothstep(cb.puddleShape.x - 0.15, cb.puddleShape.x, vertexNormal.z)
                     * open * saturate(level * 4.0);
                ring *= ring;
            }
            puddle = max(rain, ring);
            rough = mix(rough, 0.12, ring);
        }
        float film = max(puddle, standing);
        w.gloss = saturate(film);
        w.roughness = mix(max(rough, 0.05), max(cb.puddleLook.x, 0.01), standing);
        w.normal = normalize(mix(normal, vertexNormal, standing));
        return w;
    }

    vec3 wetNormal = normalize(mix(normal, vertexNormal, saturate(puddle)));
    w.gloss = saturate(puddle);
    w.roughness = max(saturate(1.0 - w.gloss), 0.05);
    w.normal = wetNormal;
    return w;
}

float WetReflectance(float NdotV, float roughness, float gloss) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return (0.02 * AB.x + AB.y) * gloss;
}

vec3 WetSkyReflection(vec3 r) {
    return LinColor(SkyAmbientRadiance(r, 1.0));
}

float projectedDepthFromLinear(float lin01) {
    float nearZ = cb.cameraData.x;
    float farZ = cb.cameraData.y;
    float viewZ = max(lin01 * farZ, 1.0e-6);
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

bool IsWeapon(vec2 ruv) {
    float vm = textureLod(sampler2D(t_depthVM, NVR_SAMPLER(pc.pointSampler)), ruv, 0.0).x;
    return cb.depthConstants.z == 0.0 ? vm < 0.99999 : vm > 0.0;
}

vec2 RayUV(vec3 q, out bool onScreen) {
    vec4 qc = cb.projectionTransform * vec4(q, 1.0);
    vec2 s = qc.xy / max(qc.w, 1.0e-6) * vec2(0.5, -0.5) + 0.5;
    onScreen = qc.w > 0.0 && all(greaterThanEqual(s, vec2(0.0))) && all(lessThanEqual(s, vec2(1.0)));
    return s * cb.renderScale.xy;
}

vec4 Result(vec4 scene, float gloss, vec3 found) {
    if (cb.wetFade.z <= 0.5) return scene;
    return vec4(scene.rgb * 0.15 + vec3(0.0, 0.0, gloss * 0.7) + found, 1.0);
}

void main() {
    vec2 ruv = uv * cb.renderScale.xy;
    vec4 scene = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), ruv, 0.0);
    if (IsWeapon(ruv)) { o_color = Result(scene, 0.0, vec3(0.0)); return; }

    float lin01 = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), ruv, 0.0).x;
    vec4 clip = vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, projectedDepthFromLinear(lin01), 1.0);
    vec4 view = cb.invProjectionTransform * clip;
    vec3 P = view.xyz / view.w;
    if (P.z <= 0.0 || P.z > cb.wetFade.w) { o_color = Result(scene, 0.0, vec3(0.0)); return; }

    vec3 V = normalize(P);
    vec3 N = normalize(textureLod(sampler2D(t_normals, NVR_SAMPLER(pc.pointSampler)), ruv, 0.0).xyz * 2.0 - 1.0);
    if (dot(N, V) > 0.0) N = -N;
    vec3 Ns = normalize(textureLod(sampler2D(t_shadingNormals, NVR_SAMPLER(pc.pointSampler)), ruv, 0.0).xyz * 2.0 - 1.0);
    if (dot(Ns, V) > 0.0) Ns = -Ns;

    vec3 worldPos = (cb.invViewTransform * vec4(P, 1.0)).xyz;
    vec3 worldN = normalize((cb.invViewTransform * vec4(N, 0.0)).xyz);
    vec3 worldNs = normalize((cb.invViewTransform * vec4(Ns, 0.0)).xyz);
    Wetness wet = GetWetness(worldPos, worldN, worldNs);
    float gloss = wet.gloss;
    float rough = wet.roughness;
    float glossy = gloss * (1.0 - smoothstep(cb.wetFade.x * 0.5, cb.wetFade.x, rough));
    if (glossy <= 0.01) { o_color = Result(scene, gloss, vec3(0.0)); return; }

    bool bumped = cb.wetBump.x > 0.5;
    vec3 worldNr = worldN;
    vec3 Nr = N;
    if (bumped) {
        worldNr = normalize(wet.normal);
        Nr = normalize(transpose(mat3(cb.invViewTransform)) * worldNr);
        if (dot(Nr, V) > 0.0) { Nr = N; worldNr = worldN; }
    }

    vec3 R = reflect(V, Nr);
    if (bumped && dot(R, N) < 0.0) R -= 2.0 * dot(R, N) * N;
    float towardScreen = saturate(R.z * 4.0 + 0.25);
    if (towardScreen <= 0.0) { o_color = Result(scene, gloss, vec3(0.0)); return; }

    float maxDist = cb.wetData.y;
    float steps = cb.wetData.w;
    vec3 start = P + N * (0.5 + P.z * 0.002);
    float thickness = cb.wetData.z;

    float prevT = 0.0;
    float hitT = -1.0;
    vec2 hitUV = vec2(0.0);
    bool onScreen = false;
    for (float i = 1.0; i <= steps; i += 1.0) {
        float s = i / steps;
        float t = maxDist * s * s;
        vec3 q = start + R * t;
        vec2 quv = RayUV(q, onScreen);
        if (!onScreen) break;
        float sceneZ = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), quv, 0.0).x * cb.cameraData.y;
        float delta = q.z - sceneZ;
        if (delta > 0.0 && delta < thickness + (t - prevT)) {
            float lo = prevT;
            float hi = t;
            for (int k = 0; k < int(SSR_REFINE); k++) {
                float mid = (lo + hi) * 0.5;
                vec3 m = start + R * mid;
                vec2 muv = RayUV(m, onScreen);
                float mz = textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), muv, 0.0).x * cb.cameraData.y;
                if (m.z > mz) hi = mid; else lo = mid;
            }
            hitUV = RayUV(start + R * hi, onScreen);
            hitT = hi;
            break;
        }
        prevT = t;
    }
    if (hitT < 0.0) { o_color = Result(scene, gloss, vec3(0.0)); return; }
    if (textureLod(sampler2D(t_depth, NVR_SAMPLER(pc.pointSampler)), hitUV, 0.0).x >= 0.999 || IsWeapon(hitUV)) {
        o_color = Result(scene, gloss, vec3(0.0));
        return;
    }

    vec2 hitS = hitUV * cb.renderScale.zw;
    vec2 edge = min(hitS, 1.0 - hitS) / max(cb.wetFade.y, 1.0e-4);
    float confidence = saturate(min(edge.x, edge.y)) * (1.0 - smoothstep(maxDist * 0.6, maxDist, hitT)) * towardScreen;

    vec3 worldV = -normalize(worldPos);
    vec3 worldR = reflect(-worldV, worldNr);
    float horizon = saturate(1.0 + 1.2 * dot(worldR, worldN));
    float reflectance = WetReflectance(saturate(dot(worldNr, worldV)), rough, glossy) * horizon * horizon;
    vec3 skySeen = LinEncode(max(WetSkyReflection(worldR), vec3(0.0)));

    vec3 hitColor = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.linearSampler)), hitUV, 0.0).rgb;
    float weight = reflectance * confidence * cb.wetData.x;

    if (cb.wetFade.z > 0.5) { o_color = Result(scene, gloss, hitColor * confidence); return; }

    scene.rgb = max(scene.rgb + (hitColor - skySeen) * weight, vec3(0.0));
    o_color = scene;
}
