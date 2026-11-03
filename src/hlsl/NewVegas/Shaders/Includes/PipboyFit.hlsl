// UNOFFICIAL 2026-11-03 PipboyFit: first-person armor sleeves pulled in under the arm-mounted Pip-Boy. After GPU skinning, vertices of the
// left forearm (weight on the "Bip01 L ForeTwist" / "Forearm" / "Hand" bones of this partition) that lie outside the Pip-Boy's inner
// ellipse are moved radially inward in the capsule frame. Compiled in only when PIPBOY_FIT_ON (vertex shaders; C++ adds the define);
// c182-c187 are written per skinned partition draw by NVR, all zero = off. Normals/TBN are left as they are.
#ifndef PIPBOY_FIT_ON
    #define PIPBOY_FIT_ON 0
#endif
#ifndef PIPBOY_FIT_HLSL
#define PIPBOY_FIT_HLSL

#if defined(VS) && PIPBOY_FIT_ON
float4 PipboyFitBones : register(c182);	// xyz: palette row offsets (slot*3) of ForeTwist, Forearm, Hand in this partition, -1 = not in it; w: radius scale at the wrist end
float4 PipboyFitL0 : register(c183);	// rows of the 3x4 matrix L: skinned position -> capsule frame (x along the forearm), L = s*R | t
float4 PipboyFitL1 : register(c184);
float4 PipboyFitL2 : register(c185);
float4 PipboyFitParams : register(c186);	// x: on (> 0.5), y: strength 0..1, z: knee (share of the radius where the soft clamp starts), w: end falloff length
float4 PipboyFitShape : register(c187);	// x: capsule start along x, y: capsule end along x, z: ellipse radius Ry, w: radius Rz

float3 PipboyFitPosition(float3 pos, float4 slots, float4 w4) {
    [branch] if (PipboyFitParams.x < 0.5f) return pos;	// off for this draw (every draw but the fitted sleeves): skip the work
    float4 hit = float4(abs(slots - PipboyFitBones.x) < 0.5f) + float4(abs(slots - PipboyFitBones.y) < 0.5f) + float4(abs(slots - PipboyFitBones.z) < 0.5f);
    float armW = dot(saturate(hit), w4);
    float4 p4 = float4(pos, 1.0f);
    float3 q = float3(dot(PipboyFitL0, p4), dot(PipboyFitL1, p4), dot(PipboyFitL2, p4));
    // the casing narrows over its last 4 units towards the wrist: the radius scales down to c182.w there
    float2 rad = PipboyFitShape.zw * lerp(1.0f, PipboyFitBones.w, smoothstep(PipboyFitShape.y - 4.0f, PipboyFitShape.y, q.x));
    float e = max(length(q.yz / rad), 1e-5f);
    float k = PipboyFitParams.z;
    float x = max(e - k, 0.0f) / max(1.0f - k, 1e-4f);
    float t = exp(-2.0f * x);
    float eNew = e > k ? k + (1.0f - k) * (1.0f - t) / (1.0f + t) : e;	// tanh soft clamp past the knee
    float fall = max(PipboyFitParams.w, 1e-4f);
    float along = smoothstep(PipboyFitShape.x - fall, PipboyFitShape.x, q.x) * (1.0f - smoothstep(PipboyFitShape.y, PipboyFitShape.y + fall, q.x));
    float amount = PipboyFitParams.y * along * saturate(armW) * (PipboyFitParams.x > 0.5f ? 1.0f : 0.0f);
    float2 d = q.yz * (eNew / e - 1.0f) * amount;	// capsule-frame displacement (0, dy, dz)
    float3 dS = (PipboyFitL1.xyz * d.x + PipboyFitL2.xyz * d.y) / max(dot(PipboyFitL0.xyz, PipboyFitL0.xyz), 1e-8f);
    return pos + dS;
}
#else
float3 PipboyFitPosition(float3 pos, float4 slots, float4 w4) { return pos; }
#endif

#endif
