// Reflections technique 7 (MaterialCapture, full resolution; only with REFL_MATERIAL_ON) as a Vulkan pass:
// Effects/Reflections.fx.hlsl MaterialCapturePS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Reflections.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    float a = textureLod(sampler2D(t_rendered, POINT_S), (vpos + 0.5) * cb.reciprocalResolution.xy, 0.0).a - 2.0;
    if (a < 0.0 || a >= 1.0) { o_color = vec4(0.0); return; }
    float metal = floor(a * 4.0);
    o_color = vec4(saturate((a - metal * 0.25) / 0.24), metal / 3.0, 1.0, 1.0);
}
