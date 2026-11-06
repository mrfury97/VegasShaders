// SpeedTree leaf-card animation as the game's leaf vertex shaders do it: two small rotations (rustle about Z, rock about X) driven
// by per-leaf phase, then the wind matrix picked by the leaf's blend index, blended in by its wind weight.

// float-rounded constants, as in the game's compiled shaders
static const float PI = 3.14159274, TWO_PI = 6.28318548;
static const float ANGLE_SCALE = 0.499999583, ANGLE_BIAS = 0.5, ANGLE_TO_SINCOS = 0.159154937;

// params: x amplitude, y time, z frequency. The phase is offset per leaf by its index (48 leaf slots).
void CalculateAnimationRotation(in float4 params, in float leafIndex, out float s, out float c) {
    float phase = frac((leafIndex / 48.0f + params.y) * ANGLE_SCALE + ANGLE_BIAS);
    float swing = phase * TWO_PI - PI;

    float wrapped = frac((sin(swing) * params.x * params.z) * ANGLE_TO_SINCOS + ANGLE_BIAS);
    float angle = wrapped * TWO_PI - PI;

    sincos(angle, s, c);
}

// regs: the shadow shader's one register array (UNOFFICIAL 2026-11-05, Balebandro ad32a3b: it holds the bones too, ShadowMap.vso); in it
// from windBase the wind matrices, 4 rows per matrix (w = 0), and from leafBase the per-leaf corner vectors (x and w = 0).
// blendIndices: x wind weight, y wind matrix index, z leaf index, w leaf scale.
float4 SpeedTreeTransform(float4 billboardUp, float4 billboardRight, float4 rockParams, float4 rustleParams,
                          float4 regs[122], float windBase, float leafBase, float4 blendIndices, float4 position) {
    float zs, zc;	// rustle: rotation about Z
    CalculateAnimationRotation(rustleParams, blendIndices.z, zs, zc);
    float3 zRow0 = float3(zc, -zs, 0.0f);
    float3 zRow1 = float3(zs, zc, 0.0f);

    float xs, xc;	// rock: rotation about X
    CalculateAnimationRotation(rockParams, blendIndices.z, xs, xc);
    float3 xRow1 = float3(0.0f, xc, -xs);
    float3 xRow2 = float3(0.0f, xs, xc);

    float2 slot = blendIndices.zy - frac(blendIndices.zy);	// integer leaf / wind slots
    float4 corner = regs[leafBase + slot.x] * blendIndices.w;

    // Z rotation leaves z alone, so only x and y of the billboard axes are rotated
    float rightX = dot(zRow0, billboardRight.xyz);
    float rightY = dot(zRow1, billboardRight.xyz);
    float upX = dot(zRow0, billboardUp.xyz);
    float upY = dot(zRow1, billboardUp.xyz);

    // X rotation leaves x alone (and it is zero anyway)
    float cornerY = dot(xRow1, corner.xyz);
    float cornerZ = dot(xRow2, corner.xyz);

    // the corner spans right * y and up * z
    float4 alongRight = float4(rightX, rightY, billboardRight.zw);
    alongRight *= cornerY;
    float4 alongUp = float4(upX, upY, billboardUp.zw);
    alongUp *= cornerZ;

    float4 placed = position + alongRight + alongUp;

    float4 blown;
    blown.x = dot(regs[windBase + 0 + slot.y], placed);
    blown.y = dot(regs[windBase + 1 + slot.y], placed);
    blown.z = dot(regs[windBase + 2 + slot.y], placed);
    blown.w = dot(regs[windBase + 3 + slot.y], placed);

    return lerp(placed, blown, blendIndices.x);
}
