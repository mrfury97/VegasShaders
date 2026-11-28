// VolumetricLight techniques BlurX (2) and BlurY (4) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl
// BlurAxis line for line (push axis (1, 0): BlurXPS from Temp; (0, 1): BlurYPS from March), half resolution, into the
// effect's own buffer. effects/VolumetricLight.cpp ExtRender.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;     // TESR_VolumetricLightTemp (X) or TESR_VolumetricLightMarch (Y) (POINT, CLAMP)

#include "VolumetricLight.glsl"

void main() {
    vec2 vpos = floor(gl_FragCoord.xy);
    vec2 axis = pc.axis;
    vec2 lo = 0.5 * LowTexel;
    vec2 hi = max(cb.renderScale.xy - 0.5 * LowTexel, lo);
    vec4 centre = POINT_LOD(t_source, clamp((vpos + 0.5) * LowTexel, lo, hi));
    float tolerance = max(centre.b * 0.05, 0.00002);
    float sigma = max(BlurRadius * 0.5, 0.5);
    float falloff = -0.5 / (sigma * sigma);
    vec2 sum = centre.rg;
    float weights = 1.0;
    for (int i = 1; i <= 8; i++) {
        if (float(i) > BlurRadius) break;
        float gauss = exp(falloff * float(i) * float(i));
        vec4 a = POINT_LOD(t_source, clamp((vpos + 0.5 + axis * float(i)) * LowTexel, lo, hi));
        vec4 b = POINT_LOD(t_source, clamp((vpos + 0.5 - axis * float(i)) * LowTexel, lo, hi));
        float wa = gauss / (1.0 + abs(a.b - centre.b) / tolerance);
        float wb = gauss / (1.0 + abs(b.b - centre.b) / tolerance);
        sum += a.rg * wa + b.rg * wb;
        weights += wa + wb;
    }
    o_color = vec4(sum / weights, centre.b, 1.0);
}
