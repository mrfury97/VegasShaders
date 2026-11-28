// SunShadows' contact-shadow blurs as a Vulkan pass: Effects/SunShadows.fx.hlsl, line for line. pc.pass 0: DepthBlurKeep with
// OffsetMaskH, 1: DepthBlurKeep with OffsetMaskV (both: TESR_ShadowScreenSpaceData.y, 3500, SSS_MAXDEPTH), 2: FinalContactBlur.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "SunShadows.glsl"

void main() {
    vec2 ruv = toRectUV(uv);
    vec2 OffsetMask = pc.pass == 0u ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 shadow = DepthBlurKeep(ruv, OffsetMask, cb.screenSpaceData.y, 3500.0, SSS_MAXDEPTH);
    if (pc.pass != 2u) { o_color = shadow; return; }

    // FinalContactBlur
    if (readDepthLod(ruv) > SSS_MAXDEPTH) { o_color = shadow; return; }
    o_color = pow(shadow, vec4(cb.screenSpaceData.w));
}
