#version 450
// The last step of NVR's FSR 2 path (needs NVR DXVK; effects/TAA.cpp): FSR's output into the frame, except on the first-person
// weapon, which is drawn without jitter (its own camera) and so is put back exactly as rendered (as TAA's WeaponTAA = 0).
// DebugView (TAA's setting): 1 = motion vectors (red/green, full at 16 px), 5 = FSR's input depth (log scale, black near).
#extension GL_GOOGLE_include_directive : require
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_fsr;         // FSR's output (RGBA16F)
layout(set = 1, binding = 1) uniform texture2D t_source;      // the frame as rendered (TESR_SourceBuffer)
layout(set = 1, binding = 2) uniform texture2D t_viewmodel;   // TESR_DepthBufferViewModel (> 0 on the weapon)
layout(set = 1, binding = 3) uniform texture2D t_motion;      // NVR's motion vectors (uv units)
layout(set = 1, binding = 4) uniform texture2D t_depth;       // NVR's FSR depth (r) + reactive (g)

layout(push_constant) uniform Push {
    uint pointIndex;            // 0: POINT, CLAMP (the extension writes the heap index)
    float debugView;            // 4
    vec2 size;                  // 8: render size in pixels
    uint weaponAA;              // 16: bit 0 = 1st-person viewmodel jittered/AA'd (keep t_fsr on weapon); bit 1 = HDR output (no saturate); bit 2 = reversed depth
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define POINT_S NVR_SAMPLER(pc.pointIndex)

void main() {
    ivec2 pixel = ivec2(floor(gl_FragCoord.xy));
    vec4 source = texelFetch(sampler2D(t_source, POINT_S), pixel, 0);
    float vmRaw = texelFetch(sampler2D(t_viewmodel, POINT_S), pixel, 0).x;
    bool vmPresent = ((pc.weaponAA & 4u) != 0u) ? (vmRaw > 0.0) : (vmRaw < 0.99999);
    bool weapon = ((pc.weaponAA & 1u) == 0u) && vmPresent;
    vec3 color = weapon ? source.rgb : texelFetch(sampler2D(t_fsr, POINT_S), pixel, 0).rgb;
    if (pc.debugView > 0.5 && pc.debugView < 1.5) {
        vec2 m = texelFetch(sampler2D(t_motion, POINT_S), pixel, 0).xy * pc.size / 16.0;
        color = vec3(abs(m), 0.0);
    }
    else if (pc.debugView > 4.5) {
        float d = texelFetch(sampler2D(t_depth, POINT_S), pixel, 0).x;
        color = vec3(d);
    }
    if ((pc.weaponAA & 2u) != 0u) {
        color = clamp(color, vec3(0.0), vec3(65000.0));
        if (any(isnan(color))) color = max(source.rgb, vec3(0.0));
        o_color = vec4(color, source.a);
    } else {
        o_color = vec4(saturate(color), source.a);
    }
}
