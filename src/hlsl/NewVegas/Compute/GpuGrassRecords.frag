// UNOFFICIAL 2026-11-17 fast-restart (docs/next/FAST-RESTART.md section 3): the GPU grass strands' records for SunFar (effects/GpuGrass.cpp
// RenderStrands, the strands' GG_RECORDS vertex shaders), while core/CapturedNormals.h GrassForSunFar asks the grass for its records. The
// strands are drawn again, after their colour draw, into the captured-normals target and the MaterialBuffer only, depth-tested against
// their own depth without writing it. CapturedNormals::MergeIntoNormals then puts the strand's tuft normal in TESR_NormalsBuffer as it does
// for the card grass's records (the grass mark: MaterialBuffer g), so SunFar takes a cloud's or a mountain's shadow off the strands as off
// the ground. Without it the normal there was rebuilt from depth across strands a pixel or two wide: it points anywhere, and where it faced
// away from the sun SunFar took nothing (strands in full sun under a cloud). Nothing else reads these two targets as lit records: the
// deferred passes key on the albedo record's code (Includes/DeferredPixel.hlsl), which the strands leave as it was.
//   location 0, the captured normal (Includes/CapturedNormals.hlsl CapturedNormalOut): rg the normal, octahedral (Includes/Shadow.hlsl
//     ShadowEncodeNormal), turned to the camera's side; b the camera distance; a 1 (written).
//   location 1, the MaterialBuffer as GRASS23x000TMS.pso's records: r the class (frame block cloudLight.w), g the grass mark (cloudLight.z:
//     4.5 with GBufferV2, 0.5 without), b 0.9, a 1.
//
// Build: glslangValidator -V --target-env vulkan1.1 -o GpuGrassRecords.spv GpuGrassRecords.frag
#version 450

layout(location = 0) in vec4 i_normal;   // xyz the strand's tuft normal (world, bent toward the eye), w the grass mark (g)
layout(location = 1) in vec4 i_pos;      // xyz the point, camera-relative; w the material class (r)
layout(location = 0) out vec4 o_normal;
layout(location = 1) out vec4 o_material;

vec2 encodeNormal(vec3 n) {   // Includes/Shadow.hlsl ShadowEncodeNormal, line for line
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    vec2 e = n.xy;
    if (n.z < 0.0) e = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

void main() {
    vec3 n = normalize(i_normal.xyz);
    n *= dot(n, i_pos.xyz) > 0.0 ? -1.0 : 1.0;   // the side the camera sees (CapturedNormalOut)
    o_normal = vec4(encodeNormal(n), length(i_pos.xyz), 1.0);
    o_material = vec4(i_pos.w, i_normal.w, 0.9, 1.0);
}
