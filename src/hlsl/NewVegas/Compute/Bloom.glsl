// Bloom passes as Vulkan passes (needs NVR DXVK; core/DxvkCompute.h): Effects/Bloom.fx.hlsl line for line. Included by
// BloomDownFirst.frag, BloomDown.frag, BloomUp.frag and BloomUpLast.frag (one technique each); effects/Bloom.cpp RenderBloomExt
// fills the push block. The textures are set 1 bindings 0 and 1 (LINEAR, CLAMP through the sampler heap index in the push block).
// The UNOFFICIAL [Shaders.Bloom.*] additions (Spread, GlowBoost, LinearLight, AntiFlicker) are the same code as the .fx.hlsl's.

layout(push_constant) uniform Push {
    vec4 data;          // 0: TESR_BloomData (.xy filterRadius, .z blendingCoefficient, .w 1 / passes)
    vec4 resolution;    // 16: TESR_BloomResolution (this level's size, and 1 / size)
    vec4 glow;          // 32: TESR_BloomGlow
    vec4 glowExtra;     // 48: TESR_BloomGlowExtra
    uint samplerIndex;  // 64: LINEAR, CLAMP
    uint pad[3];        // 68
    vec4 renderScale;   // 80: (src0ScaleX, src0ScaleY, src1ScaleX, src1ScaleY) (UNOFFICIAL 2026-11-19 RenderRect)
    vec4 clampBounds;   // 96: (src0MaxU, src0MaxV, src1MaxU, src1MaxV)
} pc;

// One read, clamped inside the active RenderRect (no-op at scale 1.0).
vec4 tap(texture2D tex, sampler smp, vec2 p) {
    vec2 hi = pc.clampBounds.x > 0.0 ? pc.clampBounds.xy : vec2(1.0);
    return texture(sampler2D(tex, smp), clamp(p, vec2(0.0), hi));
}
vec4 tap1(texture2D tex, sampler smp, vec2 p) {
    vec2 hi = pc.clampBounds.z > 0.0 ? pc.clampBounds.zw : vec2(1.0);
    return texture(sampler2D(tex, smp), clamp(p, vec2(0.0), hi));
}
vec2 srcUV0(vec2 uv) { return uv * (pc.renderScale.x > 0.0 ? pc.renderScale.xy : vec2(1.0)); }
vec2 srcUV1(vec2 uv) { return uv * (pc.renderScale.z > 0.0 ? pc.renderScale.zw : vec2(1.0)); }

// Downsample with the 13 tap box (Effects/Includes/Sampling.hlsl DownsampleBox13).
vec4 DownsampleBox13(texture2D buf, sampler smp, vec2 uv, vec2 texelSize) {
    const float x = texelSize.x;
    const float y = texelSize.y;

    vec4 a = tap(buf, smp, vec2(uv.x - 2.0 * x, uv.y + 2.0 * y));
    vec4 b = tap(buf, smp, vec2(uv.x, uv.y + 2.0 * y));
    vec4 c = tap(buf, smp, vec2(uv.x + 2.0 * x, uv.y + 2.0 * y));

    vec4 d = tap(buf, smp, vec2(uv.x - 2.0 * x, uv.y));
    vec4 e = tap(buf, smp, vec2(uv.x, uv.y));
    vec4 f = tap(buf, smp, vec2(uv.x + 2.0 * x, uv.y));

    vec4 g = tap(buf, smp, vec2(uv.x - 2.0 * x, uv.y - 2.0 * y));
    vec4 h = tap(buf, smp, vec2(uv.x, uv.y - 2.0 * y));
    vec4 i = tap(buf, smp, vec2(uv.x + 2.0 * x, uv.y - 2.0 * y));

    vec4 j = tap(buf, smp, vec2(uv.x - x, uv.y + y));
    vec4 k = tap(buf, smp, vec2(uv.x + x, uv.y + y));
    vec4 l = tap(buf, smp, vec2(uv.x - x, uv.y - y));
    vec4 m = tap(buf, smp, vec2(uv.x + x, uv.y - y));

    const vec2 weights = vec2(0.125, 0.5);

    vec4 boxes[5];
    boxes[0] = (a + b + d + e) * 0.25;
    boxes[1] = (b + c + e + f) * 0.25;
    boxes[2] = (d + e + g + h) * 0.25;
    boxes[3] = (e + f + h + i) * 0.25;
    boxes[4] = (j + k + l + m) * 0.25;

    return boxes[0] * weights[0] + boxes[1] * weights[0] + boxes[2] * weights[0] + boxes[3] * weights[0] + boxes[4] * weights[1];
}

// UNOFFICIAL: the part of a colour (linear) that counts as a light, 0 to 1 (Bloom.fx.hlsl GlowAmount).
float GlowAmount(vec3 c, float threshold) {
    float peak = max(c.r, max(c.g, c.b));
    float saturation = 1.0 - min(c.r, min(c.g, c.b)) / max(peak, 1.0e-4);
    float brightness = mix(dot(c, vec3(0.2126, 0.7152, 0.0722)), peak, saturation);
    return clamp((brightness - threshold) / threshold, 0.0, 1.0);
}

// UNOFFICIAL: the cubic and square-root fits of the display encoding (Bloom.fx.hlsl GlowLinear / GlowEncode).
vec3 GlowLinear(vec3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
vec3 GlowEncode(vec3 c) {
    vec3 s1 = sqrt(c), s2 = sqrt(s1), s3 = sqrt(s2);
    return max(0.662002687 * s1 + 0.684122060 * s2 - 0.323583601 * s3 - 0.0225411470 * c, vec3(0.0));
}
vec4 GlowTap(vec4 c, float threshold) {
    vec3 lin = GlowLinear(max(c.rgb, vec3(0.0)));
    lin *= 1.0 + pc.glow.z * GlowAmount(lin, threshold);
    return vec4(pc.glowExtra.y > 0.0 ? lin : GlowEncode(lin), c.a);
}

// UNOFFICIAL: the first downsample, from the image (Bloom.fx.hlsl DownsampleFirst).
vec4 DownsampleFirst(texture2D buf, sampler smp, vec2 uv) {
    uv = srcUV0(uv);
    const float x = pc.resolution.z;
    const float y = pc.resolution.w;

    vec4 a = tap(buf, smp, vec2(uv.x - 2.0 * x, uv.y + 2.0 * y));
    vec4 b = tap(buf, smp, vec2(uv.x, uv.y + 2.0 * y));
    vec4 c = tap(buf, smp, vec2(uv.x + 2.0 * x, uv.y + 2.0 * y));

    vec4 d = tap(buf, smp, vec2(uv.x - 2.0 * x, uv.y));
    vec4 e = tap(buf, smp, vec2(uv.x, uv.y));
    vec4 f = tap(buf, smp, vec2(uv.x + 2.0 * x, uv.y));

    vec4 g = tap(buf, smp, vec2(uv.x - 2.0 * x, uv.y - 2.0 * y));
    vec4 h = tap(buf, smp, vec2(uv.x, uv.y - 2.0 * y));
    vec4 i = tap(buf, smp, vec2(uv.x + 2.0 * x, uv.y - 2.0 * y));

    vec4 j = tap(buf, smp, vec2(uv.x - x, uv.y + y));
    vec4 k = tap(buf, smp, vec2(uv.x + x, uv.y + y));
    vec4 l = tap(buf, smp, vec2(uv.x - x, uv.y - y));
    vec4 m = tap(buf, smp, vec2(uv.x + x, uv.y - y));

    if (pc.glow.z > 0.0 || pc.glowExtra.y > 0.0) {
        float t = max(pc.glow.w, 0.002); // GlowBoost's threshold (a floor: a pitch-black cell)
        a = GlowTap(a, t); b = GlowTap(b, t); c = GlowTap(c, t); d = GlowTap(d, t); e = GlowTap(e, t); f = GlowTap(f, t); g = GlowTap(g, t);
        h = GlowTap(h, t); i = GlowTap(i, t); j = GlowTap(j, t); k = GlowTap(k, t); l = GlowTap(l, t); m = GlowTap(m, t);
    }

    vec4 boxes[5];
    boxes[0] = (a + b + d + e) * 0.25;
    boxes[1] = (b + c + e + f) * 0.25;
    boxes[2] = (d + e + g + h) * 0.25;
    boxes[3] = (e + f + h + i) * 0.25;
    boxes[4] = (j + k + l + m) * 0.25;

    if (pc.glowExtra.x > 0.0) {
        // Karis average: each box by 1 / (1 + its brightness), then the weights normalised.
        const float weights[5] = float[5](0.125, 0.125, 0.125, 0.125, 0.5);
        vec4 sum = vec4(0.0);
        float total = 0.0;
        for (int n = 0; n < 5; n++) {
            float w = weights[n] / (1.0 + max(dot(boxes[n].rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0));
            sum += boxes[n] * w;
            total += w;
        }
        return sum / total;
    }
    const vec2 weights = vec2(0.125, 0.5);
    return boxes[0] * weights[0] + boxes[1] * weights[0] + boxes[2] * weights[0] + boxes[3] * weights[0] + boxes[4] * weights[1];
}

// Downsample (Bloom.fx.hlsl Downsample): the texel size is this level's (TESR_BloomResolution.zw).
vec4 Downsample(texture2D buf, sampler smp, vec2 uv) {
    return DownsampleBox13(buf, smp, srcUV0(uv), vec2(pc.resolution.z, pc.resolution.w));
}

// Upsample with tent filter (Effects/Includes/Sampling.hlsl UpsampleTent9).
vec4 UpsampleTent9(texture2D buf, sampler smp, vec2 uv, vec2 filterRadius) {
    float x = filterRadius.x;
    float y = filterRadius.y;

    vec4 a = tap(buf, smp, vec2(uv.x - x, uv.y + y));
    vec4 b = tap(buf, smp, vec2(uv.x, uv.y + y));
    vec4 c = tap(buf, smp, vec2(uv.x + x, uv.y + y));

    vec4 d = tap(buf, smp, vec2(uv.x - x, uv.y));
    vec4 e = tap(buf, smp, vec2(uv.x, uv.y));
    vec4 f = tap(buf, smp, vec2(uv.x + x, uv.y));

    vec4 g = tap(buf, smp, vec2(uv.x - x, uv.y - y));
    vec4 h = tap(buf, smp, vec2(uv.x, uv.y - y));
    vec4 i = tap(buf, smp, vec2(uv.x + x, uv.y - y));

    vec4 upsample = e * 4.0;
    upsample += (b + d + f + h) * 2.0;
    upsample += (a + c + g + i);
    upsample *= 1.0 / 16.0;

    return upsample;
}

// Intermediary upsamples (Bloom.fx.hlsl Upsample).
vec4 Upsample(texture2D buf, texture2D addBuf, sampler smp, vec2 uv) {
    vec4 upsample = UpsampleTent9(buf, smp, srcUV0(uv), vec2(pc.data.x, pc.data.y));
    vec4 nextMip = tap1(addBuf, smp, srcUV1(uv));

    if (pc.data.z > 0.0) {
        return vec4(mix(nextMip.rgb, upsample.rgb, pc.data.z), 1.0);
    } else {
        return vec4(upsample.rgb * pc.glow.x + nextMip.rgb, 1.0); // UNOFFICIAL Spread: .x = 1 (as before) unless set
    }
}

// Last upsample that normalizes the result if needed (Bloom.fx.hlsl UpsampleLast).
vec4 UpsampleLast(texture2D buf, texture2D addBuf, sampler smp, vec2 uv) {
    vec4 upsample = UpsampleTent9(buf, smp, srcUV0(uv), vec2(pc.data.x, pc.data.y));
    vec4 nextMip = tap1(addBuf, smp, srcUV1(uv));

    vec3 result;
    if (pc.data.z > 0.0) {
        result = mix(nextMip.rgb, upsample.rgb, pc.data.z);
    } else {
        result = (upsample.rgb * pc.glow.x + nextMip.rgb) * pc.glow.y; // UNOFFICIAL Spread (as before at .x = 1)
    }
    // UNOFFICIAL LinearLight: the chain held linear light; encoded again for its readers.
    if (pc.glowExtra.y > 0.0) result = delinearize(max(result, vec3(0.0)));
    return vec4(result, 1.0);
}
