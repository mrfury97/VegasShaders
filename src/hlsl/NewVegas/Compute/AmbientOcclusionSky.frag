// AmbientOcclusion technique SkyVisibilityV2 (Skylighting v2, also run for AmbientLight) as a Vulkan pass (needs NVR DXVK):
// Effects/AmbientOcclusion.fx.hlsl SkyVisibilityV2PS line for line, into NVR_SkyVisBuffer (or Low): effects/AmbientOcclusion.cpp
// RenderSkyVisibility. The pass uses FrameVS (not PackedVS) in a viewport the size of the sky target, so its uv is the shared
// quad's: the extension's texel centre moved by the quad's full-resolution half-texel offset (main).
#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_samplerless_texture_functions : require
#include "AmbientOcclusion.glsl"

layout(set = 1, binding = 0) uniform texture2D t_depth;      // TESR_DepthBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_normals;    // TESR_NormalsBuffer (NONE = POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_ortho;      // NVR_OrthoMapBorder (LINEAR, BORDER 0xFFFFFFFF; see OrthoTopBorder)
layout(set = 1, binding = 3) uniform texture2D t_sunMask;    // NVR_SunMask (POINT, CLAMP); a stand-in unless cb.sky4.z > 0.5
layout(set = 1, binding = 4) uniform texture2D t_skyPrep;    // NVR_SkyPrep (POINT, CLAMP); a stand-in unless cb.skyLayout.z > 0.5

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

vec2 SkyUnpack2(float p) { float high = floor(p / 4096.0); return vec2(high, p - high * 4096.0) / 4095.0; }
vec3 SkyOctDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
    return normalize(n);
}

// The .fx's NVR_OrthoMapBorder reads the map with a white border (BORDERCOLOR 0xFFFFFFFF: the far plane, open sky); the extension's
// border is transparent black. Where all four bilinear texels lie inside the map the border plays no part and the LINEAR/CLAMP read is
// the same read; wholly outside it the result is the border (1); in the one-texel band between, the bilinear blend of the inside
// texels and the white border is done here by hand (texelFetch, the same four texels and weights in full float precision).
float OrthoTopBorder(vec2 coord) {
    vec2 size = vec2(textureSize(t_ortho, 0));
    vec2 f = coord * size - 0.5;
    if (all(greaterThanEqual(f, vec2(0.0))) && all(lessThanEqual(f, size - 1.0))) return LINEAR_LOD(t_ortho, coord).r;
    if (any(lessThan(f, vec2(-1.0))) || any(greaterThan(f, size))) return 1.0;
    vec2 i0 = floor(f);
    vec2 w = f - i0;
    float texel[4];
    for (int k = 0; k < 4; k++) {
        vec2 i = i0 + vec2(k & 1, k >> 1);
        texel[k] = all(greaterThanEqual(i, vec2(0.0))) && all(lessThan(i, size)) ? texelFetch(t_ortho, ivec2(i), 0).r : 1.0;
    }
    return mix(mix(texel[0], texel[1], w.x), mix(texel[2], texel[3], w.x), w.y);
}

float SkyOpenV2(vec3 o, float softScale) {
    float top = OrthoTopBorder(vec2(o.x * 0.5 + 0.5, o.y * -0.5 + 0.5));
    return saturate((top - o.z) * softScale + 0.5);
}

void main() {
    // FrameVS's uv in this viewport: texel i at i / size + the quad's 0.5 / full resolution.
    vec2 ruv = uv * aoRectScale();
    vec2 texel = cb.reciprocalResolution.xy;
    vec2 inUV = ruv + 0.5 * (texel - cb.skyLayout.xy);
    vec2 uv = aoClampToRect((floor(inUV / texel) + 0.5) * texel, texel);
    float depth = readDepth(uv);
    vec3 viewPosition = reconstructPosition(uv);
    vec3 viewNormal = GetNormal(uv);
    vec3 px0 = reconstructPosition(aoClampToRect(uv - vec2(texel.x, 0.0), texel)), px1 = reconstructPosition(aoClampToRect(uv + vec2(texel.x, 0.0), texel));
    vec3 py0 = reconstructPosition(aoClampToRect(uv - vec2(0.0, texel.y), texel)), py1 = reconstructPosition(aoClampToRect(uv + vec2(0.0, texel.y), texel));
    vec3 ddxP = abs(px1.z - viewPosition.z) < abs(viewPosition.z - px0.z) ? px1 - viewPosition : viewPosition - px0;
    vec3 ddyP = abs(py1.z - viewPosition.z) < abs(viewPosition.z - py0.z) ? py1 - viewPosition : viewPosition - py0;
    vec3 depthNormal = normalize(cross(ddxP, ddyP));
    depthNormal = dot(depthNormal, viewPosition) > 0.0 ? -depthNormal : depthNormal;
    viewNormal = cb.sky3.x > 0.5 ? depthNormal : viewNormal;
    if (cb.skyLayout.z > 0.5) viewNormal = SkyOctDecode(SkyUnpack2(POINT_LOD(t_skyPrep, aoClampToRect(inUV, texel)).y));
    float result = 1.0;
    float skyVisibility = 1.0;
    float sunMask = cb.sky4.z > 0.5 ? mix(cb.shadowFade.x, 1.0, POINT_LOD(t_sunMask, uv).r) : 1.0;
    if (depth < endFade) {
        vec3 p = (cb.invViewTransform * vec4(viewPosition, 0.0)).xyz;
        vec3 n = normalize((cb.invViewTransform * vec4(viewNormal, 0.0)).xyz);
        p += n * 24.0;
        vec4 o4 = cb.shadowCameraToLightOrtho * vec4(p, 1.0);
        vec3 o = o4.xyz / o4.w;
        // (float3x3)M: the HLSL rows are the stored rows, here the columns; mul(d, M) is mat3(M) * d, M[2] its third row.
        mat3 M = mat3(cb.shadowCameraToLightOrtho);
        float reach = cb.sky4.x;
        float softScale = 1.0 / max(cb.sky4.y * abs(M[2][2]), 1.0e-9);
        float covered = 1.0 - SkyOpenV2(o + M[2] * 32.0, softScale);
        float sides = saturate(cb.sky5.y * covered);
        {
            float total = 0.034 * saturate(n.z);
            float open = total * SkyOpenV2(o + M[2] * (reach * 0.4), softScale) * SkyOpenV2(o + M[2] * reach, softScale);
            for (int i = 0; i < 8; i++) {
                float a = (float(i) + 0.5) * (PI / 4.0);
                vec2 h = vec2(cos(a), sin(a));
                for (int k = 0; k < 2; k++) {
                    vec3 dir = k != 0 ? vec3(h * 0.8660254, 0.5) : vec3(h * 0.5, 0.8660254);
                    float w = (k != 0 ? 0.0884 : 0.0324) * saturate(dot(dir, n));
                    vec3 stepO = M * dir;
                    float rayOpen = SkyOpenV2(o + stepO * (reach * 0.4), softScale) * SkyOpenV2(o + stepO * reach, softScale);
                    if (k != 0 && sides > 0.0) rayOpen = mix(rayOpen, SkyOpenV2(o + (M * vec3(h, 0.0)) * reach, softScale), sides);
                    open += w * rayOpen;
                    total += w;
                }
            }
            float visibility = total > 1.0e-5 ? open / total : 1.0;
            visibility = mix(visibility, 1.0, saturate((max(abs(o.x), abs(o.y)) - 0.8) * 5.0));
            visibility = saturate(visibility * cb.sky3.y);
            result = SkyFactor(visibility, n, sunMask, covered);
            skyVisibility = visibility;
        }
    }
    o_color = vec4(result, min(depth, endFade), skyVisibility, 1.0);
}
