// UNOFFICIAL 2026-10-27 (NVR DXVK, deep/fork2-ssgi) [Shaders.AmbientLight.Main] ComputeGI: the light each prep texel's surface sends on,
// for AmbientLightEstimate-gi*: the frame (the picture before AmbientLight's apply), linear, capped at 1 (as BounceLight's Prepare: lamps
// and glints do not spray light), stored as its square root in NVR_AmbientLightGILight (A8R8G8B8, the prep's size: 4 bytes a texel, so
// the GI pass's light reads stay in the cache). A LINEAR read at the half-resolution texel's centre = the average of its 2 x 2 frame
// pixels. effects/AmbientLight.cpp ExtDraw (ExtGILight), right before the GI pass.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_scene;      // the frame (TESR_SourceBuffer of the apply; LINEAR)

layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = (floor(gl_FragCoord.xy) + 0.5) * pc.frame.xy * renderScaleXY();   // the texel centre in the active rect (pc.frame.xy = 1 / reducedRect)
    vec3 light = min(linearize(linearLod(t_scene, uv).rgb), vec3(1.0));
    o_color = vec4(sqrt(max(light, vec3(0.0))), 1.0);
}
