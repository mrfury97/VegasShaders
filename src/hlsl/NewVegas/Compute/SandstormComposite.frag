// Sandstorm technique 5 (Composite, full resolution, the fast path's last pass) as a Vulkan pass (needs NVR DXVK): Effects/
// Sandstorm.fx.hlsl CompositePS line for line, through the frame chain. effects/Sandstorm.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_grid;       // TESR_SandstormGrid (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_streaks;    // TESR_SandstormStreaks (LINEAR, CLAMP)
layout(set = 1, binding = 4) uniform texture2D t_glow;       // TESR_SandstormGlow (POINT, CLAMP)

#define SS_DEPTH
#define SS_GRID
#define SS_GLOW
#include "SandstormCommon.glsl"

void main() {
    vec4 color = linearize(POINT_LOD(t_rendered, uv));

    float intensity = TESR_SandstormData.x;
    float density = TESR_SandstormData.y * intensity;
    float visibility = TESR_SandstormData.z;
    float heightFalloff = TESR_SandstormData.w;
    float time = TESR_SandstormWind.w;
    vec3 dir = normalize(toWorld(uv));

    // How far the dust runs before the pixel's surface: as SandstormPS (the 3 x 3 tent with SoftEdges).
    float fullDensity = max(TESR_SandstormData.y, 1e-6);
    float centerDist = ViewDistance(uv, visibility);
    float dist = centerDist;
    if (TESR_SandstormQuality.z > 0.5 && centerDist < visibility * 5.99) {
        vec2 texel = TESR_ReciprocalResolution.xy * 2.0;
        float centerShown = exp(-centerDist * fullDensity);
        float shown = 0.0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                float w = float((2 - abs(x)) * (2 - abs(y)));
                float tapShown = (x == 0 && y == 0) ? centerShown : min(exp(-ViewDistance(uv + vec2(float(x), float(y)) * texel, visibility) * fullDensity), centerShown);
                shown += w * tapShown;
            }
        dist = min(-log(max(shown / 16.0, 1e-9)) / fullDensity, visibility * 6.0);
    }

    vec3 dustAmbient, dustSun;
    DustLighting(dir, dustAmbient, dustSun);
    vec3 dustLight = dustAmbient + dustSun;

    // The billowing dust through the first stretch of the ray, from the grid.
    float marchEnd = min(dist, visibility * 1.5);
    vec4 dust = DustAt(uv, marchEnd);
    float transmittance = exp(-dust.a);
    vec3 scattered = dust.rgb;

    // Beyond the march: an even haze to the scene, or the sky, behind it (as SandstormPS).
    float endHeight = exp(-max(dir.z * marchEnd + 200.0, 0.0) / heightFalloff);
    float haze = 1.0 - exp(-max(dist - marchEnd, 0.0) * density * mix(endHeight, 1.0, 0.5));
    scattered += transmittance * haze * dustLight;
    transmittance *= 1.0 - haze;
    vec3 dustScattered = scattered;

    // Streaks: each layer only in front of the surface, fading out before it (SandstormPS's 'visible', the pixel's own jitter).
    if (TESR_SandstormShape.z > 0.0) {
        float jitter = fract(52.9829189 * fract(dot(uv / TESR_ReciprocalResolution.xy, vec2(0.06711056, 0.00583715))) + time * 7.123);
        // The streak buffer through a 3 x 3 tent (four bilinear reads half a texel off each way).
        vec4 grains = 0.25 * (LINEAR_LOD(t_streaks, uv + LowTexel * vec2(-0.5, -0.5))
            + LINEAR_LOD(t_streaks, uv + LowTexel * vec2(0.5, -0.5))
            + LINEAR_LOD(t_streaks, uv + LowTexel * vec2(-0.5, 0.5))
            + LINEAR_LOD(t_streaks, uv + LowTexel * vec2(0.5, 0.5)));
        vec3 visible = saturate((dist - (vec3(50.0, 170.0, 290.0) + 30.0 * jitter)) / 60.0);
        scattered += dot(grains.rgb, visible) * (dustAmbient + dustSun * grains.a) * TESR_SandstormShape.z * intensity * 0.25;
    }

    float rawDepth = readDepth(uv);
    if (TESR_SandstormLights.x > 0.0) scattered += GlowAt(uv, rawDepth / farZ);

    vec3 result = color.rgb * transmittance + scattered;

    // Emissive (as SandstormPS): the dust within EmissiveDistance only, read from the grid there.
    bool skyBehind = rawDepth / farZ >= 0.9961;
    if (TESR_SandstormEmissive.x > 0.0) {
        vec3 refLight = EmissiveDustRef(dustLight);
        float dustLuma = max(dot(refLight, vec3(0.2126, 0.7152, 0.0722)), 1e-4) * TESR_SandstormEmissive.x * (skyBehind ? 2.0 : 1.0);
        float emissive = saturate((dot(color.rgb, vec3(0.2126, 0.7152, 0.0722)) - dustLuma) / dustLuma);
        if (skyBehind) emissive *= 1.0 - smoothstep(0.02, 0.25, TESR_SunAmount.x);
        if (emissive > 0.0) {
            vec4 nearDust = DustAt(uv, min(TESR_SandstormEmissive.y, marchEnd));
            vec3 nearResult = color.rgb * exp(-nearDust.a) + nearDust.rgb + (scattered - dustScattered);
            result = mix(result, nearResult, emissive);
        }
    }
    o_color = delinearize(vec4(result, color.a));
}
