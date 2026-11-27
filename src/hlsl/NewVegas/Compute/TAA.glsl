// TAA as Vulkan passes (needs NVR DXVK): what the three techniques of Effects/TAA.fx.hlsl share, line for line
// (effects/TAA.cpp ExtDraw). Each technique is its own .frag (TAAResolve, TAAOutput, TAADebug) including this file; all three
// declare the same bindings, and a pass's C++ binds a placeholder to any texture it does not read (no pass has its own
// target bound as an input). The matrices arrive as the D3D9 effect uploads them (ID3DXEffect::SetMatrix: the logical matrix),
// so HLSL M[i][j] is the same element here, with the same indices. The constants are copied from the memory SetCT uploads.
// (Each .frag starts with #version 450 and GL_GOOGLE_include_directive before including this file.)
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;      // TESR_SourceBuffer (POINT)
layout(set = 1, binding = 1) uniform texture2D t_history;     // TESR_TAAHistoryBuffer (LINEAR)
layout(set = 1, binding = 2) uniform texture2D t_depth;       // TESR_DepthBuffer (POINT)
layout(set = 1, binding = 3) uniform texture2D t_viewmodel;   // TESR_DepthBufferViewModel (POINT)
layout(set = 1, binding = 4) uniform texture2D t_objmotion;   // UNOFFICIAL 2026-10-29 TESR_ObjectMotionBuffer (POINT; a placeholder while cameraDelta.w is 0)
layout(set = 1, binding = 5) uniform texture2D t_vmmotion;    // UNOFFICIAL 2026-11-21 TESR_ViewModelMotionBuffer (POINT; a placeholder while weapon.y is 0)

layout(set = 2, binding = 0, std140) uniform Constants {
    mat4 projectionTransform;   // TESR_ProjectionTransform
    mat4 viewTransform;         // TESR_ViewTransform
    mat4 prevViewTransform;     // TESR_TAAPrevViewTransform
    vec4 reciprocalResolution;  // TESR_ReciprocalResolution
    vec4 cameraData;            // TESR_CameraData
    vec4 data;                  // TESR_TAAData: x history weight, y clip gamma, z history valid, w debug view
    vec4 prevProjection;        // TESR_TAAPrevProjection
    vec4 cameraDelta;           // TESR_TAACameraDelta (w: UNOFFICIAL 2026-10-29 ObjectMotion, 1 = t_objmotion holds this frame's)
    vec4 weapon;                // TESR_TAAWeapon
    vec4 sharpen;               // TESR_TAASharpen
} cb;

layout(push_constant) uniform Push {
    uint pointIndex;            // 0: POINT, CLAMP (the extension writes the heap index)
    uint linearIndex;           // 4: LINEAR, CLAMP
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

#define POINT_S NVR_SAMPLER(pc.pointIndex)
#define LINEAR_S NVR_SAMPLER(pc.linearIndex)

#define historyWeight cb.data.x
#define clipGamma cb.data.y
#define historyValid cb.data.z
#define debugView cb.data.w
#define weaponHistoryScale cb.weapon.x
#define farZ cb.cameraData.y

// Includes/Depth.hlsl (readDepth, toWorld)
float readDepth(vec2 p) { return textureLod(sampler2D(t_depth, POINT_S), p, 0.0).x * farZ; }

vec3 toWorld(vec2 tex) {
    vec3 v = vec3(cb.viewTransform[0][2], cb.viewTransform[1][2], cb.viewTransform[2][2]);
    v += (1.0 / cb.projectionTransform[0][0] * (2.0 * tex.x - 1.0)) * vec3(cb.viewTransform[0][0], cb.viewTransform[1][0], cb.viewTransform[2][0]);
    v += (-1.0 / cb.projectionTransform[1][1] * (2.0 * tex.y - 1.0)) * vec3(cb.viewTransform[0][1], cb.viewTransform[1][1], cb.viewTransform[2][1]);
    return v;
}

vec3 RGBToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 YCoCgToRGB(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Where this pixel's surface was on screen last frame (the HLSL Reproject, line for line).
vec2 Reproject(vec2 p, out float inFront) {
    vec3 cameraVector = toWorld(p) * readDepth(p);
    // UNOFFICIAL 2026-10-29 ObjectMotion: a surface that moved itself was that much further back along its path last frame.
    if (cb.cameraDelta.w > 0.0) cameraVector -= textureLod(sampler2D(t_objmotion, POINT_S), p, 0.0).xyz;
    vec3 fromPrevCamera = cameraVector + cb.cameraDelta.xyz;

    vec3 prevRight   = vec3(cb.prevViewTransform[0][0], cb.prevViewTransform[1][0], cb.prevViewTransform[2][0]);
    vec3 prevUp      = vec3(cb.prevViewTransform[0][1], cb.prevViewTransform[1][1], cb.prevViewTransform[2][1]);
    vec3 prevForward = vec3(cb.prevViewTransform[0][2], cb.prevViewTransform[1][2], cb.prevViewTransform[2][2]);

    float viewZ = dot(fromPrevCamera, prevForward);
    inFront = viewZ > 0.0 ? 1.0 : 0.0;

    vec2 ndc = vec2(dot(fromPrevCamera, prevRight) * cb.prevProjection.x,
                    dot(fromPrevCamera, prevUp) * cb.prevProjection.y) / max(viewZ, 1e-4);
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Catmull-Rom filtered history fetch in 5 taps (the HLSL SampleHistory, line for line).
vec3 SampleHistory(vec2 p) {
    vec2 texelSize = cb.reciprocalResolution.xy;
    vec2 samplePos = p / texelSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0 = (texPos1 - 1.0) * texelSize;
    vec2 texPos3 = (texPos1 + 2.0) * texelSize;
    vec2 texPos12 = (texPos1 + offset12) * texelSize;

    vec3 result = vec3(0.0);
    result += textureLod(sampler2D(t_history, LINEAR_S), vec2(texPos12.x, texPos0.y), 0.0).rgb * (w12.x * w0.y);
    result += textureLod(sampler2D(t_history, LINEAR_S), vec2(texPos0.x, texPos12.y), 0.0).rgb * (w0.x * w12.y);
    result += textureLod(sampler2D(t_history, LINEAR_S), vec2(texPos12.x, texPos12.y), 0.0).rgb * (w12.x * w12.y);
    result += textureLod(sampler2D(t_history, LINEAR_S), vec2(texPos3.x, texPos12.y), 0.0).rgb * (w3.x * w12.y);
    result += textureLod(sampler2D(t_history, LINEAR_S), vec2(texPos12.x, texPos3.y), 0.0).rgb * (w12.x * w3.y);
    float weights = w12.x * (w0.y + w12.y + w3.y) + w12.y * (w0.x + w3.x);

    return max(result / weights, vec3(0.0));
}

// Pull the history toward the box centre until it lies inside the box (the HLSL ClipToBox).
vec3 ClipToBox(vec3 history, vec3 boxMin, vec3 boxMax) {
    vec3 center = 0.5 * (boxMax + boxMin);
    vec3 extents = 0.5 * (boxMax - boxMin) + 1e-4;
    vec3 offset = history - center;
    vec3 units = abs(offset / extents);
    float maxUnit = max(units.x, max(units.y, units.z));
    return maxUnit > 1.0 ? center + offset / maxUnit : history;
}

struct TAAResult {
    vec3 color;
    vec3 current;
    vec3 historyRaw;
    vec2 motionPixels;
    float weight;
    float clipAmount;
    float isViewModel;
    float depth;
};

// The HLSL ResolveCore, line for line.
TAAResult ResolveCore(vec2 p) {
    TAAResult result;
    vec2 texelSize = cb.reciprocalResolution.xy;

    // Current-frame 3x3 neighbourhood in YCoCg: its range, and its mean and spread.
    vec3 current = vec3(0.0);
    vec3 moment1 = vec3(0.0);
    vec3 moment2 = vec3(0.0);
    vec3 neighbourMin = vec3(1e5);
    vec3 neighbourMax = vec3(-1e5);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 c = RGBToYCoCg(textureLod(sampler2D(t_source, POINT_S), p + vec2(x, y) * texelSize, 0.0).rgb);
            if (x == 0 && y == 0) current = c;
            moment1 += c;
            moment2 += c * c;
            neighbourMin = min(neighbourMin, c);
            neighbourMax = max(neighbourMax, c);
        }
    }
    moment1 /= 9.0;
    moment2 /= 9.0;
    vec3 sigma = sqrt(max(moment2 - moment1 * moment1, vec3(0.0)));

    vec3 boxMin = max(neighbourMin, moment1 - clipGamma * sigma);
    vec3 boxMax = min(neighbourMax, moment1 + clipGamma * sigma);

    float inFront;
    vec2 prevUV = Reproject(p, inFront);

    // The weapon gets real 1st-person motion vectors when WeaponAA is active; otherwise fixed to camera (same pixel).
    float vmRaw = textureLod(sampler2D(t_viewmodel, POINT_S), p, 0.0).x;
    bool reversedDepth = cb.weapon.z > 0.5;
    float isViewModel = (reversedDepth ? (vmRaw > 0.0) : (vmRaw < 0.99999)) ? 1.0 : 0.0;
    if (isViewModel > 0.5 && cb.weapon.y > 0.5) {
        vec4 vm = textureLod(sampler2D(t_vmmotion, POINT_S), p, 0.0);
        if (vm.z > 0.5) {
            prevUV = p + vm.xy;
            inFront = 1.0;
        } else {
            prevUV = p;
            inFront = 0.0;
        }
    } else {
        prevUV = mix(prevUV, p, isViewModel);
        inFront = max(inFront, isViewModel);
    }

    vec2 inside = step(vec2(0.0), prevUV) * step(prevUV, vec2(1.0));
    float onScreen = inside.x * inside.y;

    vec3 historyRaw = SampleHistory(prevUV);
    vec3 historyRawYCoCg = RGBToYCoCg(historyRaw);
    vec3 history = ClipToBox(historyRawYCoCg, boxMin, boxMax);

    // A surface that was off screen or behind the camera last frame has no history to use, and neither does
    // anything on the first frame after a reset.
    float weight = historyWeight * historyValid * onScreen * inFront;

    // The weapon is not jittered: WeaponTAA scales the history it gets (1 as the world, 0 exactly as rendered).
    weight *= mix(1.0, weaponHistoryScale, isViewModel);

    result.color = YCoCgToRGB(mix(current, history, weight));
    result.current = YCoCgToRGB(current);
    result.historyRaw = historyRaw;
    result.motionPixels = (prevUV - p) / texelSize;
    result.weight = weight;
    result.clipAmount = length(history - historyRawYCoCg);
    result.isViewModel = isViewModel;
    result.depth = readDepth(p);
    return result;
}
