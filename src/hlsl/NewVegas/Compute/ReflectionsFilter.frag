// Reflections technique 6 (Filter, one face of one mip level per draw) as a Vulkan pass: Effects/Reflections.fx.hlsl FilterPS
// line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 st = (vpos + 0.5) / CubeSize * 2.0 - 1.0;
    vec3 dir = FaceDirection(CubeFace, st);
    if (CubeMip < 0.5) { o_color = textureLod(samplerCube(t_captureNew, LINEAR_S), dir, 0.0); return; }
    vec2 texel = vec2(2.0 / CubeSize);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 offset = (vec2(x, y) - 1.5) / 4.0 * 1.5 * texel;
            vec4 s = textureLod(samplerCube(t_captureNew, LINEAR_S), FaceDirection(CubeFace, st + offset), 0.0);
            sum += vec4(Tame(s.rgb), s.a);
        }
    }
    vec4 avg = sum / 16.0;
    o_color = vec4(Untame(avg.rgb), avg.a);
}
