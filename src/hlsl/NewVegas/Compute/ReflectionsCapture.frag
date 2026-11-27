// Reflections technique 5 (Capture, one cube face per draw) as a Vulkan pass: Effects/Reflections.fx.hlsl CapturePS line for line,
// except that the sky here is SkyGradientAlong's (the sky panorama and the CloudSky clouds are Vulkan-only, as everywhere else).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

layout(set = 1, binding = 32) uniform textureCube t_worldCube;   // 2026-11-24 the world cubemap (CubeMode 2; ReflectionsEffect::RenderCube). (2026-11-27 upstream 1.1.5 merge: binding 30 is Reflections.glsl t_gbAlbedo; moved to 32, slot 31 in Reflections.cpp)

// CubeMode 1 and 2: what a direction the screen has not shown holds (Reflections.fx.hlsl CubeBackdropFx).
vec3 CubeBackdrop(vec3 ndir) {
    if (SkyFallback > 0.0) {
        vec3 horizonLin = linearize(cb.horizonColor.rgb);
        vec3 skyCol = SkyGradientAlong(ndir);   // (2026-11-24 review: the sky's own function, not a copy of it)
        // the sun's glow, by the sun's share of the sky as the D3D9 pass (2026-11-24: guard by positive sun elevation so dusk/dawn doesn't glow underground)
        if (cb.sunAmount.x > 0.02 && cb.sunPosition.z > 0.0) {
            vec3 sunPos = normalize(cb.sunPosition.xyz);
            float sunCos = max(dot(ndir, sunPos), 0.0);
            float corona = pow(sunCos, 48.0) * 0.45 + smoothstep(0.9985, 0.9998, sunCos) * 12.0;
            skyCol += linearize(cb.sunDiskColor.rgb) * (corona * saturate(cb.sunAmount.x * 2.0) * smoothstep(0.0, 0.1, sunPos.z));
        }
        vec3 groundLin = horizonLin * mix(0.45, 0.18, clamp(-ndir.z * 2.0, 0.0, 1.0));
        vec3 env = mix(groundLin, skyCol, smoothstep(-0.08, 0.06, ndir.z));
        if (GridOn > 0.5) {
            vec4 g = GridAlong(vec3(0.0), ndir);
            env = mix(env, g.rgb, saturate(g.a));
        }
        return env;
    }
    vec3 fallbackLin = linearize(cb.horizonColor.rgb) * 0.15;
    if (GridOn > 0.5) {
        vec4 g = GridAlong(vec3(0.0), ndir);
        return mix(fallbackLin, g.rgb, saturate(g.a));
    }
    return fallbackLin;
}

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 st = (vpos + 0.5) / CubeSize * 2.0 - 1.0;
    vec3 dir = FaceDirection(CubeFace, st);
    vec4 result;
    if (CubeOn >= 1.5) {
        vec3 ndir = normalize(dir);
        vec3 baseEnv = CubeBackdrop(ndir);
        if (CubeOn > 2.5) {
            vec4 world = textureLod(samplerCube(t_worldCube, LINEAR_S), ndir, 0.0);
            baseEnv = mix(baseEnv, world.rgb, clamp(world.a, 0.0, 1.0));
        }
        float decay = CubeOn > 2.5 ? max(CubeFade * 20.0, 0.12) : CubeFade;
        vec4 prev = textureLod(samplerCube(t_capture, POINT_S), dir, 0.0);
        result = CubeReset > 0.0 ? vec4(baseEnv, 1.0) : mix(prev, vec4(baseEnv, 1.0), decay);
    } else {
        result = CubeReset > 0.0 ? vec4(0.0) : textureLod(samplerCube(t_capture, POINT_S), dir, 0.0) * (1.0 - CubeFade);
    }

    vec3 view = WorldToViewDir(dir);
    if (view.z > 0.05) {
        vec2 uv = ToScreen(view);
        vec2 suv = rectToScreen(uv);
        if (all(greaterThan(suv, vec2(0.005))) && all(lessThan(suv, vec2(0.995)))) {
            // A cube texel covers a patch of many screen pixels: average a 4 x 4 grid of reads across it.
            vec2 du = ToScreen(WorldToViewDir(FaceDirection(CubeFace, st + vec2(2.0 / CubeSize, 0.0)))) - uv;
            vec2 dv = ToScreen(WorldToViewDir(FaceDirection(CubeFace, st + vec2(0.0, 2.0 / CubeSize)))) - uv;
            vec3 sum = vec3(0.0);
            float found = 0.0;
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    vec2 at = uv + du * ((float(x) - 1.5) / 4.0) + dv * ((float(y) - 1.5) / 4.0);
                    bool usable = all(greaterThan(at, vec2(0.0))) && all(lessThan(at, cb.renderScale.xy)) && !IsWeapon(at) && readDepthLod(at) > 40.0;
                    vec3 c = linearize(textureLod(sampler2D(t_rendered, LINEAR_S), at, 0.0).rgb);
                    float brightest = max(c.r, max(c.g, c.b));
                    sum += usable ? c * min(1.0, 32.0 / max(brightest, 1.0e-4)) : vec3(0.0);
                    found += usable ? 1.0 : 0.0;
                }
            }
            if (found > 0.0) {
                vec2 edge = min(suv, 1.0 - suv);
                float blend = CaptureBlend * saturate(min(edge.x, edge.y) * 20.0) * found / 16.0;
                result = mix(result, vec4(sum / found, 1.0), blend);
            }
        }
    }
    o_color = result;
}
