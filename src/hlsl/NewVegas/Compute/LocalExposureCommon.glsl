// LocalExposure (needs NVR DXVK): shared constants and RenderRect helpers for the 4 LocalExposure passes.
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 2, binding = 0, std140) uniform Constants {
    vec4 toneMapping;   // TESR_ToneMapping (w = Linearization)
    vec4 size;          // TESR_LocalExposureSize (xy = 1/bufferSize, zw = 1/screenSize)
    vec4 blur;          // TESR_LocalExposureBlur (x = sigma in buffer texels, y = DebugView)
    vec4 renderScale;   // TESR_RenderScale (xy = Scale, zw = 1/Scale)
} cb;

vec2 clampToRectIfOn(vec2 uv, vec2 px) {
    vec2 hi = cb.renderScale.xy - 0.5 * px;
    return cb.renderScale.x < 0.9999 || cb.renderScale.y < 0.9999 ? clamp(uv, 0.5 * px, max(hi, 0.5 * px)) : uv;
}

float blurAlong(texture2D source, uint samp, vec2 uv, vec2 direction) {
    float sigma = max(cb.blur.x, 0.5);
    float stepTexels = sigma / 3.0;
    float sum = 0.0;
    float weights = 0.0;
    for (int k = -6; k <= 6; k++) {
        float fk = float(k);
        float w = exp(-0.5 * (fk * fk) / 9.0);
        vec2 at = clampToRectIfOn(uv + direction * (fk * stepTexels) * cb.size.xy, cb.size.xy);
        sum += w * textureLod(sampler2D(source, NVR_SAMPLER(samp)), at, 0.0).r;
        weights += w;
    }
    return sum / weights;
}
