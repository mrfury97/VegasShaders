// VolumetricLight technique CompositeGrid (8) as a Vulkan pass (needs NVR DXVK): Effects/VolumetricLight.fx.hlsl CompositeGridPS,
// GridAirAt, MsaaAirMix and CompositeAir line for line, full resolution, through the frame chain. effects/VolumetricLight.cpp ExtRender.
// (UNOFFICIAL 2026-11-05: the sky shares of the game's MSAA, t_skycov, MsaaSkyEdge.glsl; the 2026-11-04 D3D9 SkyShare had no port here.)
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "Common.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;   // TESR_RenderedBuffer (POINT, CLAMP)
layout(set = 1, binding = 1) uniform texture2D t_depth;      // TESR_DepthBuffer (POINT, CLAMP)
layout(set = 1, binding = 2) uniform texture2D t_grid;       // TESR_VolumetricLightGrid (LINEAR, CLAMP)
layout(set = 1, binding = 3) uniform texture2D t_skycov;     // TESR_SkyCoverage (POINT, CLAMP; the depth when the game's MSAA is off)

#define VL_DEPTH
#define VL_COMPOSITE
#define VL_GRID
#include "VolumetricLight.glsl"
#include "MsaaSkyEdge.glsl"

// The lit share and the share of all the air from the eye to depth along this pixel's view.
vec2 GridAirAt(vec2 at, float depth) {
    float planar = depth >= farZ * 0.996 ? MarchDistance : min(depth, MarchDistance);
    float s = sqrt(planar / MarchDistance) * 64.0;   // slice coordinate of the pixel's depth
    float z = min(floor(s), 63.0);
    vec2 local = at * GridSize;
    float farSide = LINEAR_LOD(t_grid, GridUV(z, local)).r;
    float nearSide = z > 0.5 ? LINEAR_LOD(t_grid, GridUV(z - 1.0, local)).r : 0.0;
    return vec2(mix(nearSide, farSide, saturate(s - z)) / cb.gridData.y, 1.0 - exp(-AirDepth(toWorld(at), planar)));   // (rtvol2)
}

vec2 MsaaAirMix(vec2 at, float depth, vec2 air) {
    float share = MsaaSkyCoverage(at);
    if (share <= MsaaNoSky) return air;
    if (depth < farZ * 0.996) return mix(air, GridAirAt(at, farZ), share);
    if (share >= MsaaAllSky) return air;
    float surface = MsaaSurfaceDepth(at, farZ);
    if (surface >= farZ * 0.996) return air;
    return mix(GridAirAt(at, surface), air, share);
}

void main() {
    vec4 color = POINT_LOD(t_rendered, uv);
    float depth = readDepthLod(uv);
    if (MsaaEdgeDebugOn()) { o_color = MsaaEdgeDebug(uv, depth, color.rgb); return; }
    vec2 air = GridAirAt(uv, depth);
    if (MsaaEdgeFixOn()) air = MsaaAirMix(uv, depth, air);
    o_color = CompositeAir(uv, color, air);
}
