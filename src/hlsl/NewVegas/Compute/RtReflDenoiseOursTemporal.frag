// UNOFFICIAL 2026-11-07 RT reflection denoiser, core 0 ("ours"), pass 4 of 4: the result (= next frame's history) and next frame's
// moments. The blurred signal is the history (recurrent, as ReBLUR: the blur shrinks as the history grows, so it does not pile up).
// Debug views (params2.y): 1 the raw signal, 2 the accumulated signal (no blur), 3 the history length (white = MaxSamples),
// 4 the blur radius (white = BlurRadius), 5 the virtual (hit-point) reprojection's share (green), 6 the hit distance (white near).
// Inputs: t_in0 blurred, t_in1 (x radius), t_in2 accumulated, t_in3 this frame's moments.
// Compile: glslangValidator -V --target-env vulkan1.1 -o RtReflDenoiseOursTemporal.spv RtReflDenoiseOursTemporal.frag
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "RtReflDenoise.glsl"

layout(location = 0) out vec4 o_result;
layout(location = 1) out vec4 o_moments;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 m = Fetch(t_in3, p);
    o_moments = m;
    o_result = Fetch(t_in0, p);
    if (!Finite(o_result)) o_result = vec4(0.0);
    int view = int(pc.params2.y + 0.5);
    if (view == 0 || Fetch(t_ray, p).x <= 0.0) return;
    // (debug views: the result shows them; the history keeps the real result only while the view is off, so a view is A/B only)
    if (view == 1) o_result = Fetch(t_signal, p);
    else if (view == 2) o_result = Fetch(t_in2, p);
    else if (view == 3) o_result = vec4(vec3(m.y / max(pc.params.x, 1.0)), 1.0);
    else if (view == 4) o_result = vec4(vec3(Fetch(t_in1, p).x / max(pc.params2.z, 1.0)), 1.0);
    else if (view == 5) o_result = vec4(0.0, m.w, 0.0, 1.0);
    else if (view == 6) o_result = vec4(vec3(1.0 / (1.0 + m.z / 200.0)), 1.0);
}
