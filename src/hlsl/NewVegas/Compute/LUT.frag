// LUT as a Vulkan pass (needs NVR DXVK): Effects/LUT.fx.hlsl line for line. effects/LUT.cpp ExtRender (technique 0 only).
// Bindings: 0 the picture, 1 day, 2 night, 3 interior table (an unusable table is bound to the picture and not read).
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (LINEAR, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_day;        // TESR_LUTDayBuffer
layout(set = 1, binding = 2) uniform texture2D t_night;      // TESR_LUTNightBuffer
layout(set = 1, binding = 3) uniform texture2D t_interior;   // TESR_LUTInteriorBuffer

layout(push_constant) uniform Push {
    vec4 lutData;       // 0: TESR_LUTData (x day N, y strength, z night N, w interior N)
    vec4 lutBlend;      // 16: TESR_LUTBlend (x day/night blend, y interior flag, z HDR compatibility)
    uint samplerIndex;  // 32: LINEAR, CLAMP
    uint pad[3];        // 36..47
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

// a sampler from the heap at point of use (GLSL rejects a sampler built outside the call)
#define LUT_SAMPLE(t, p) texture(sampler2D(t, NVR_SAMPLER(pc.samplerIndex)), p)

vec3 lutStrip(texture2D table, float cells, vec3 c) {
    float b = c.b * (cells - 1.0);
    float cellLo = floor(b);
    float cellHi = min(cellLo + 1.0, cells - 1.0);
    float blendB = b - cellLo;
    vec2 inCell = vec2(c.r * (cells - 1.0), c.g * (cells - 1.0));
    vec2 uvLo = vec2((cellLo * cells + 0.5 + inCell.x) / (cells * cells), (0.5 + inCell.y) / cells);
    vec2 uvHi = vec2((cellHi * cells + 0.5 + inCell.x) / (cells * cells), (0.5 + inCell.y) / cells);
    return mix(LUT_SAMPLE(table, uvLo).rgb, LUT_SAMPLE(table, uvHi).rgb, blendB);
}

vec3 gradeWith(texture2D table, float cells, vec3 c) {
    return (cells >= 2.0) ? lutStrip(table, cells, clamp(c, vec3(0.0), vec3(1.0))) : c;
}

void main() {
    vec2 ruv = gl_FragCoord.xy / vec2(textureSize(sampler2D(t_rendered, NVR_SAMPLER(pc.samplerIndex)), 0));
    vec3 original = LUT_SAMPLE(t_rendered, ruv).rgb;

    bool hdr = pc.lutBlend.z > 0.5;
    float scale = hdr ? max(1.0, max(original.r, max(original.g, original.b))) : 1.0;
    vec3 lookup = original / scale;

    vec3 day = gradeWith(t_day, pc.lutData.x, lookup);
    vec3 night = gradeWith(t_night, pc.lutData.z, lookup);
    vec3 interior = gradeWith(t_interior, pc.lutData.w, lookup);

    vec3 exterior = mix(night, day, pc.lutBlend.x);
    vec3 graded = mix(exterior, interior, pc.lutBlend.y) * scale;

    o_color = vec4(mix(original, graded, pc.lutData.y), 1.0);
}
