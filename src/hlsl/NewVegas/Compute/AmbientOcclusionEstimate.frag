// AmbientOcclusion pass Estimate of the Dedicated techniques (DedicatedAO/AOFast: PackedEstimate; DedicatedGTAO/GTAOFast:
// GTAOEstimate, pc.gtaoEstimate 1) as a Vulkan pass (needs NVR DXVK): Effects/AmbientOcclusion.fx.hlsl line for line, into
// NVR_AOBuffer0 (or Low0). uv is PackedVS's: the AO target's texel centre. The Fast variants compute the same picture (only where
// the compiler could branch differs); here every read is level 0 of a single-level target, as there.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (NONE = POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_blueNoise;  // TESR_BlueNoiseSampler (NONE = POINT, WRAP)
layout(set = 1, binding = 3) uniform texture2D t_ortho;      // NVR_OrthoMap (LINEAR, CLAMP); a stand-in unless cb.sky.x > 0 (never read then)

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float readDepth(vec2 coord) {
    return LINEAR_LOD(t_depth, coord).x * farZ;
}

vec3 reconstructPosition(vec2 uv) {
    vec2 s = uv * aoInvRectScale();
    float x = s.x * 2.0 - 1.0;
    float y = (1.0 - s.y) * 2.0 - 1.0;
    float z = projectedDepthFromLinear(LINEAR_LOD(t_depth, uv).x);
    vec4 clipSpace = vec4(x, y, z, 1.0);

    vec4 viewSpace = cb.invProjectionTransform * clipSpace;

    viewSpace /= viewSpace.w;

    return viewSpace.xyz;
}

// Normals.hlsl
vec3 GetNormal(vec2 uv) {
    return POINT_LOD(t_normals, uv).xyz * 2.0 - 1.0;
}

vec3 random(vec2 seed) {
    return POINT_WRAP_LOD(t_blueNoise, (seed / 256.0 + 0.5) / cb.reciprocalResolution.xy).xyz;
}

float fogCoeff(float depth) {
    return saturate(invlerp(cb.fogData.x, cb.fogData.y, depth));
}

vec2 PackedSSAOValues(vec2 uv) {
    float uRadius = abs(AOrange);
    float bias = saturate(AOangleBias);
    vec3 origin = reconstructPosition(uv);
    if (origin.z > endFade) return vec2(1.0);

    vec3 normal = GetNormal(uv);
    float angle = -random(uv).x / 2.0 * PI;
    vec3 kernelRotation = vec3(-sin(angle), cos(angle), 0.0);
    vec3 tangent = normalize(kernelRotation - normal * dot(kernelRotation, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 tbn = mat3(tangent, bitangent, normal);    // (columns = the HLSL's rows: mul(v, tbn) is tbn * v)
    vec2 occlusion = vec2(0.0);

    for (int i = 0; i < kernelSize; ++i) {
        vec3 rand = random(uv + float(i) * cb.reciprocalResolution.x);
        vec3 raw = vec3(expand(rand.xy), rand.z);
        vec3 sampleX = tbn * normalize(raw * vec3(1.0, 0.0, 1.0));
        vec3 sampleY = tbn * normalize(raw * vec3(0.0, 1.0, 1.0));
        vec3 randomScale = random(uv * float(i) / 2.0);
        sampleX *= randomScale;
        sampleY *= randomScale;
        float scale = mix(bias, 1.0, pows(1.0 + float(i) / float(kernelSize), 2.0));
        sampleX *= scale * (dot(normal, sampleX) < 0.0 ? -1.0 : 1.0);
        sampleY *= scale * (dot(normal, sampleY) < 0.0 ? -1.0 : 1.0);

        vec3 pointX = origin + sampleX * uRadius;
        vec3 pointY = origin + sampleY * uRadius;
        vec2 sampleDepth = vec2(
            readDepth(aoClampToRectIfOn(projectPosition(pointX).xy, cb.reciprocalResolution.xy)),
            readDepth(aoClampToRectIfOn(projectPosition(pointY).xy, cb.reciprocalResolution.xy)));
        vec2 actualDepth = vec2(pointX.z, pointY.z);
        vec2 distance = abs(actualDepth - sampleDepth);
        vec2 influence = vec2(lessThan(sampleDepth, actualDepth)) * vec2(lessThan(distance, vec2(uRadius)));
        influence *= mix(1.0 - distance * distance / (uRadius * uRadius), 1.0 - distance / uRadius, vec2(float(i < kernelSize / 4)));
        occlusion += influence;
    }

    vec2 values = 1.0 - occlusion / float(kernelSize) * AOstrength;
    float fogColor = luma(cb.fogColor.rgb);
    values = min(max(mix(values, vec2(fogColor), fogCoeff(origin.z)), values), vec2(1.0));	// (HLSL clamp: GLSL's is undefined for lo > hi)
    return mix(values, vec2(1.0), saturate(invlerp(startFade, endFade, origin.z)));
}

// GTAOPosition: on a depth texel's centre.
vec3 GTAOPosition(vec2 uv) {
    uv = aoClampToRectIfOn(uv, cb.reciprocalResolution.xy);
    return reconstructPosition((floor(uv / cb.reciprocalResolution.xy) + 0.5) * cb.reciprocalResolution.xy);
}

float GTAOValue(vec2 uv) {
    vec3 P = reconstructPosition(uv);
    if (P.z > endFade) return 1.0;
    vec3 V = normalize(-P);
    vec3 N = normalize(GetNormal(uv));
    N = dot(N, V) < 0.0 ? -N : N;
    float radius = abs(AOrange);
    vec2 radiusUV = radius / P.z * 0.5 * vec2(cb.projectionTransform[0][0], cb.projectionTransform[1][1]) * aoRectScale();
    radiusUV *= min(1.0, cb.gtao.w * cb.reciprocalResolution.x / max(radiusUV.x, 1e-6));
    vec3 noise = random(uv);
    float falloffMul = -1.0 / (0.615 * radius);
    float falloffAdd = (1.0 - 0.615) / 0.615 + 1.0;
    float visibility = 0.0;
    for (int slice = 0; slice < 2; slice++) {
        float phi = (float(slice) + noise.x) * (PI / 2.0);
        vec2 omega = vec2(cos(phi), sin(phi));
        vec3 direction = vec3(omega, 0.0);
        vec2 stepUV = vec2(omega.x, -omega.y) * radiusUV;
        vec3 orthoDirection = direction - dot(direction, V) * V;
        vec3 axis = normalize(cross(orthoDirection, V));
        vec3 projectedN = N - axis * dot(N, axis);
        float projectedLength = length(projectedN);
        float cosN = saturate(dot(projectedN, V) / max(projectedLength, 1e-4));
        float n = (dot(orthoDirection, projectedN) < 0.0 ? -1.0 : 1.0) * acosHlsl(cosN);
        float low0 = cos(n + PI / 2.0);
        float low1 = cos(n - PI / 2.0);
        float horizon0 = low0, horizon1 = low1;
        for (int j = 0; j < 4; j++) {
            float t = (float(j) + noise.y) / 4.0;
            t *= t;
            vec2 offset = stepUV * t + vec2(omega.x, -omega.y) * cb.reciprocalResolution.xy * 2.0;
            vec3 s0 = GTAOPosition(uv + offset) - P;
            vec3 s1 = GTAOPosition(uv - offset) - P;
            float l0 = length(s0), l1 = length(s1);
            horizon0 = max(horizon0, mix(low0, dot(s0, V) / max(l0, 1e-4), saturate(l0 * falloffMul + falloffAdd)));
            horizon1 = max(horizon1, mix(low1, dot(s1, V) / max(l1, 1e-4), saturate(l1 * falloffMul + falloffAdd)));
        }
        float h0 = n + clamp(-acosHlsl(horizon1) - n, -PI / 2.0, PI / 2.0);
        float h1 = n + clamp(acosHlsl(horizon0) - n, -PI / 2.0, PI / 2.0);
        float sinN = sin(n);
        visibility += projectedLength * ((cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) + (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n))) * 0.25;
    }
    visibility *= 0.5;
    float darkness = saturate(1.0 - (1.0 - saturate(visibility)) * AOstrength);
    float fogColor = luma(cb.fogColor.rgb);
    darkness = min(max(mix(darkness, fogColor, fogCoeff(P.z)), darkness), 1.0);
    return mix(darkness, 1.0, saturate(invlerp(startFade, endFade, P.z)));
}

// Skylighting v1
float SkyOpen(vec3 relativeWorld) {
    vec4 o = cb.shadowCameraToLightOrtho * vec4(relativeWorld, 1.0);
    o.xyz /= o.w;
    if (abs(o.x) > 1.0 || abs(o.y) > 1.0 || o.z < 0.0 || o.z > 1.0) return 1.0;
    float top = LINEAR_LOD(t_ortho, vec2(o.x * 0.5 + 0.5, o.y * -0.5 + 0.5)).r;
    return top > o.z - cb.sky.w ? 1.0 : 0.0;
}

float SkyVisibility(vec3 viewPosition, vec3 viewNormal, float turn) {
    vec3 p = (cb.invViewTransform * vec4(viewPosition, 0.0)).xyz;
    vec3 n = normalize((cb.invViewTransform * vec4(viewNormal, 0.0)).xyz);
    p += n * 16.0;
    float reach = cb.sky.z;
    float up = max(n.z, 0.0);
    float open = up * SkyOpen(p + vec3(0.0, 0.0, reach / 3.0)) * SkyOpen(p + vec3(0.0, 0.0, reach));
    float total = up;
    for (int i = 0; i < 6; i++) {
        float a = turn + float(i) * (PI / 3.0);
        float e = (i & 1) != 0 ? 0.5236 : 1.0472;	// fmod(i, 2) > 0.5
        vec3 dir = vec3(cos(a) * cos(e), sin(a) * cos(e), sin(e));
        float w = max(dot(dir, n), 0.0);
        open += w * SkyOpen(p + dir * (reach / 3.0)) * SkyOpen(p + dir * reach);
        total += w;
    }
    return mix(1.0, total > 1.0e-4 ? open / total : 1.0, cb.sky.y);
}

void main() {
    vec2 ruv = uv * aoRectScale();
    // PackedEstimate / GTAOEstimate
    float ao;
    if (AO_GTAO_ESTIMATE) ao = GTAOValue(ruv);
    else {
        vec2 values = PackedSSAOValues(ruv);
        ao = values.x * values.y;
    }
    vec3 skyOrigin = reconstructPosition(ruv);
    vec3 skyNormal = GetNormal(ruv);
    float skyTurn = random(ruv).y * 2.0 * PI;
    if (cb.sky.x > 0.0 && skyOrigin.z < endFade) ao *= SkyVisibility(skyOrigin, skyNormal, skyTurn);
    o_color = vec4(ao, min(readDepth(ruv), endFade), 0.0, 1.0);
}
