// Sandstorm technique 0 (the full-resolution original) as a Vulkan pass (needs NVR DXVK): Effects/Sandstorm.fx.hlsl SandstormPS
// line for line, through the frame chain. effects/Sandstorm.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_ortho;      // TESR_OrthoMapBuffer (POINT, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_atlas;      // TESR_ShadowAtlas (LINEAR, CLAMP)

#define SS_DEPTH
#define SS_ORTHO
#define SS_SHADOWS
#include "SandstormCommon.glsl"

const int MaxSteps = 12;   // the march loop runs this many times; steps past TESR_SandstormQuality.x are skipped

void main() {
    vec4 color = linearize(POINT_LOD(t_rendered, uv));

    float intensity = TESR_SandstormData.x;
    float density = TESR_SandstormData.y * intensity;
    float visibility = TESR_SandstormData.z;
    float heightFalloff = TESR_SandstormData.w;
    float time = TESR_SandstormWind.w;
    vec2 wind = TESR_SandstormWind.xy;
    float speed = TESR_SandstormWind.z;
    float billowing = TESR_SandstormShape.x;
    float noiseScale = TESR_SandstormShape.y;

    vec3 dir = normalize(toWorld(uv));

    // How far the dust runs before the pixel's surface (the 3 x 3 tent with SoftEdges). (HLSL's local 'distance' is 'dist' here.)
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

    vec3 camera = TESR_CameraPosition.xyz;

    // Underwater: the march starts where the view ray leaves the water (see Sandstorm.fx.hlsl).
    float start = 0.0;
    float surfaceDetail = 1.0;
    if (TESR_SandstormWater.x > 0.0) {
        start = dir.z > 1e-3 ? TESR_SandstormWater.y / dir.z : 1e9;
        if (start < 1e8 && abs(dist - start) <= max(64.0, start * 0.05)) {
            dist = start + visibility * 6.0;
            vec2 spread = TESR_ReciprocalResolution.xy * 3.0;
            const vec3 toLuma = vec3(0.2126, 0.7152, 0.0722);
            float around = dot(linearize(POINT_LOD(t_rendered, uv + vec2(spread.x, 0.0)).rgb), toLuma)
                + dot(linearize(POINT_LOD(t_rendered, uv - vec2(spread.x, 0.0)).rgb), toLuma)
                + dot(linearize(POINT_LOD(t_rendered, uv + vec2(0.0, spread.y)).rgb), toLuma)
                + dot(linearize(POINT_LOD(t_rendered, uv - vec2(0.0, spread.y)).rgb), toLuma);
            float difference = dot(color.rgb, toLuma) / max(around * 0.25, 1e-4) - 1.0;
            surfaceDetail = 1.0 + 0.8 * difference * (1.0 - smoothstep(0.12, 0.3, abs(difference)));
        }
        if (start >= dist) { o_color = delinearize(color); return; }
        dist -= start;
        camera += dir * start;
    }

    // The light on the dust
    vec3 dustAmbient, dustSun;
    DustLighting(dir, dustAmbient, dustSun);
    vec3 dustLight = dustAmbient + dustSun;   // out in the open

    // The wind carries the dust; it also rolls slowly upward.
    vec3 drift = vec3(wind * speed * time, -speed * 0.05 * time);
    float jitter = fract(52.9829189 * fract(dot(uv / TESR_ReciprocalResolution.xy, vec2(0.06711056, 0.00583715))) + time * 7.123);

    // Billowing dust, marched through the first stretch of the ray (steps closer together near the camera).
    float marchEnd = min(dist, visibility * 1.5);
    float transmittance = 1.0;
    vec3 scattered = vec3(0.0);
    // The same, only as far as EmissiveDistance.
    float nearCut = TESR_SandstormEmissive.y;
    float nearTransmittance = 1.0;
    vec3 nearScattered = vec3(0.0);
    const float Steps = TESR_SandstormQuality.x;   // UNOFFICIAL: theirs 12
    for (int i = 0; i < MaxSteps; i++) {
        if (float(i) >= Steps) continue;
        float t0 = marchEnd * pow((float(i) + jitter) / Steps, 2.0);
        float t1 = marchEnd * pow(min(float(i) + 1.0 + jitter, Steps) / Steps, 2.0);
        vec3 p = camera + dir * (0.5 * (t0 + t1));
        float height = exp(-max(p.z - camera.z + 200.0, 0.0) / heightFalloff);
        float clouds = fbm((p - drift) / noiseScale);
        float local = density * height * mix(1.0, saturate(clouds * 2.2 - 0.35) * 1.8, billowing) * (t1 - t0);
        // In the lee of a wall or a rock the air near the camera is calmer.
        float middle = 0.5 * (t0 + t1);
        if (start + middle < 500.0) local *= mix(mix(0.35, 1.0, InTheWindCoarse(dir * (start + middle), wind)), 1.0, smoothstep(350.0, 500.0, start + middle));
        float absorbed = 1.0 - exp(-local);
        vec3 light = dustAmbient + dustSun * SunVisibility(dir * (start + middle));
        scattered += transmittance * absorbed * light * (0.8 + 0.4 * clouds);
        transmittance *= 1.0 - absorbed;
        float nearShare = saturate((nearCut - t0) / max(t1 - t0, 1e-3));
        float nearAbsorbed = 1.0 - exp(-local * nearShare);
        nearScattered += nearTransmittance * nearAbsorbed * light * (0.8 + 0.4 * clouds);
        nearTransmittance *= 1.0 - nearAbsorbed;
    }

    // Beyond the march: an even haze to the scene, or the sky, behind it.
    float endHeight = exp(-max(dir.z * marchEnd + 200.0, 0.0) / heightFalloff);
    float haze = 1.0 - exp(-max(dist - marchEnd, 0.0) * density * mix(endHeight, 1.0, 0.5));
    scattered += transmittance * haze * dustLight;
    transmittance *= 1.0 - haze;
    vec3 dustScattered = scattered;   // the dust alone; what follows (streaks, light glow) is in front of anything

    // Streaks: thin sand blown past the camera, stretched along the wind.
    vec3 streaks = vec3(0.0);
    vec2 across = vec2(-wind.y, wind.x);
    if (TESR_SandstormShape.z > 0.0)   // UNOFFICIAL: none to draw at StreakStrength 0
    for (int s = 0; s < 3 && start <= 0.0; s++) {   // (none underwater: the camera is in the water)
        float t = 50.0 + 120.0 * float(s) + 30.0 * jitter;
        if (t < dist) {
            vec3 p = camera + dir * t - vec3(wind * speed * time * (2.5 + float(s)), 0.0);
            vec3 q = vec3(dot(p.xy, wind) / 240.0, dot(p.xy, across) / 8.0, p.z / 6.0);
            float sheltered;   // UNOFFICIAL FineShelter: theirs 8 upwind taps
            if (TESR_SandstormQuality.w > 0.5) sheltered = InTheWind(dir * t, wind);
            else sheltered = InTheWindCoarse(dir * t, wind);
            float visible = saturate((dist - t) / 60.0) * sheltered;
            float grain = smoothstep(0.74, 0.96, valueNoise(q + float(s) * 31.7)) * (1.0 - float(s) * 0.25) * visible;
            if (grain > 0.0) streaks += grain * (dustAmbient + dustSun * SunVisibility(dir * t));
        }
    }
    scattered += streaks * TESR_SandstormShape.z * intensity * 0.25;

    // The nearby lights glowing through the dust, as the sun does (not underwater).
    if (TESR_SandstormLights.x > 0.0 && start <= 0.0) {
        vec3 glow = vec3(0.0);
        for (int l = 0; l < 12; l++) {
            glow += PointGlow(TESR_ShadowLightPosition[l], TESR_LightColor[l], camera, dir, dist, density);
            glow += PointGlow(TESR_LightPosition[l], TESR_LightColor[12 + l], camera, dir, dist, density);
        }
        scattered += glow * density * TESR_SandstormColor.rgb * TESR_SandstormLights.x * 0.01;
    }

    vec3 result = color.rgb * transmittance + scattered;

    // Emissive: fire and sparks drawn without depth see only the dust within EmissiveDistance.
    float rawDepth = readDepth(uv);
    bool skyBehind = rawDepth / farZ >= 0.9961;
    if (TESR_SandstormEmissive.x > 0.0) {
        vec3 refLight = EmissiveDustRef(dustLight);
        float dustLuma = max(dot(refLight, vec3(0.2126, 0.7152, 0.0722)), 1e-4) * TESR_SandstormEmissive.x * (skyBehind ? 2.0 : 1.0);
        float emissive = saturate((dot(color.rgb, vec3(0.2126, 0.7152, 0.0722)) - dustLuma) / dustLuma);
        if (skyBehind) emissive *= 1.0 - smoothstep(0.02, 0.25, TESR_SunAmount.x);
        vec3 nearResult = color.rgb * nearTransmittance + nearScattered + (scattered - dustScattered);
        result = mix(result, nearResult, emissive);
    }
    result *= surfaceDetail;   // the water surface's ripples, seen from below (1 elsewhere)
    o_color = delinearize(vec4(result, color.a));
}
