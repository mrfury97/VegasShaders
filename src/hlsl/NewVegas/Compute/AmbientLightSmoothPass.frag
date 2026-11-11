// AmbientLight technique AmbientLightSmooth (SmoothNormal, passes X and Y) as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl,
// line for line (AmbientLight.glsl). variant bit 0: alongY. X reads the prep (NVR_ALPrep), Y the ping-pong buffer (NVR_ALBuffer);
// both POINT. effects/AmbientLight.cpp ExtDraw. (AmbientLightSmooth.comp is the compute version, AmbientLightSmoothCompute.)
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_source;     // X: NVR_ALPrep, Y: NVR_ALBuffer

layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = frameUV();
    bool alongY = (pc.variant & 1u) != 0u;
    float viewZ; vec3 normal;
    // (2026-11-09 rt-review) the 16-bit float buffer cannot hold the sky's / the weapon's depth (it came back infinite): X stores at most
    // 64000, Y reads 64000 or more as 1.0e6 (AmbientLight.fx.hlsl SmoothNormal)
    if (alongY) { vec4 c = pointLod(t_source, uv); normal = c.xyz; viewZ = c.w >= 64000.0 ? 1.0e6 : c.w; }
    else { vec4 c = pointLod(t_source, uv); viewZ = c.x; normal = OctDecode(Unpack2(c.y)); }
    if (viewZ < DrawDistance && cb.alEdges.x > 0.5) {
        vec3 origin = PositionFromViewZ(uv, viewZ);
        vec2 axis = alongY ? vec2(0.0, 1.0) : vec2(1.0, 0.0);
        // (2026-10-24 charfacets NormalSmoothingWorld, as the .fx) the reach is at least alEdges.z world units, at most 192 texels.
        float texelWorld = 2.0 * viewZ * cb.alLayout.z / max(cb.projectionTransform[0][0], 1.0e-3);
        float reach = clamp(cb.alEdges.z / max(texelWorld, 1.0e-4), cb.alEdges.x, 192.0);
        float spacing = reach / 3.0;
        vec3 sum = normal;
        for (int k = -3; k <= 3; k++) {
            if (k == 0) continue;
            float dist = abs(float(k)) * spacing;
            vec2 tapUV = uv + axis * (float(k) * spacing) * cb.alLayout.zw;
            float onScreen = step(0.0, tapUV.x) * step(tapUV.x, 1.0) * step(0.0, tapUV.y) * step(tapUV.y, 1.0);
            tapUV = clamp(tapUV, 0.0, 1.0);
            float tapZ; vec3 tapNormal;
            if (alongY) { vec4 t = pointLod(t_source, tapUV); tapNormal = t.xyz; tapZ = t.w >= 64000.0 ? 1.0e6 : t.w; }
            else { vec4 t = pointLod(t_source, tapUV); tapZ = t.x; tapNormal = OctDecode(Unpack2(t.y)); }
            float tolerance = max(viewZ * 0.001 * dist, 0.5);
            float plane = abs(dot(PositionFromViewZ(tapUV, tapZ) - origin, normal));
            float weight = saturate(1.0 - plane / tolerance) * smoothstep(0.5, 0.8, dot(tapNormal, normal)) * onScreen;
            sum += tapNormal * weight;
        }
        normal = normalize(sum);
    }
    o_color = alongY ? vec4(viewZ, Pack2(OctEncode(normal)), 0.0, 0.0) : vec4(normal, min(viewZ, 64000.0));
}
