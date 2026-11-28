// AmbientLight technique AmbientLightGrid pass ScreenInject as a Vulkan pass (needs NVR DXVK): Effects/AmbientLight.fx.hlsl ScreenInject,
// line for line (AmbientLight.glsl). Into the screen memory atlas (2 or 3 targets: the third, the walls, only with GridWallMemory).
// effects/AmbientLight.cpp ExtDraw.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "AmbientLight.glsl"

layout(set = 1, binding = 0) uniform texture2D t_screenOld;       // NVR_GridScreenOld (NVR_ALBuffer's stage, POINT)
layout(set = 1, binding = 1) uniform texture2D t_screenColorOld;  // NVR_GridScreenColorOld (NVR_ALHistory's stage, POINT)
layout(set = 1, binding = 2) uniform texture2D t_wallsOld;        // NVR_GridWallsOld (NVR_GridWave's stage, POINT)
layout(set = 1, binding = 3) uniform texture2D t_depth;           // TESR_DepthBuffer
layout(set = 1, binding = 4) uniform texture2D t_normals;         // TESR_NormalsBuffer
layout(set = 1, binding = 5) uniform texture2D t_source;          // TESR_SourceBuffer (LINEAR): the image now

layout(location = 0) out vec4 o_sh;
layout(location = 1) out vec4 o_colour;
layout(location = 2) out vec4 o_walls;

void main() {
    vec2 uvIn = frameUV();
    if (uvIn.x * AtlasDims.x >= AtlasSize) { o_sh = vec4(0.0); o_colour = vec4(0.0); o_walls = vec4(0.0); return; }
    vec3 cell = AtlasCell(uvIn);
    float size = cb.gridOrigin.w;
    vec3 oldCell = cell + cb.gridShift.xyz;
    bool had = cb.gridShift.w > 0.5 && all(greaterThanEqual(oldCell, vec3(0.0))) && oldCell.x < GridX && oldCell.y < GridY && oldCell.z < GridZ;
    o_sh = had ? pointLod(t_screenOld, CellUV(oldCell)) : vec4(0.0);
    o_colour = had ? pointLod(t_screenColorOld, CellUV(oldCell)) : vec4(0.0);
    o_walls = had && cb.gridWalls.z > 0.5 ? pointLod(t_wallsOld, CellUV(oldCell)) : vec4(0.0);
    vec3 centre = cb.gridOrigin.xyz + (cell + 0.5) * size;
    vec3 view = (cb.viewTransform * vec4(centre, 0.0)).xyz;   // mul(float4(centre, 0), TESR_ViewTransform)
    if (view.z < 0.5 * size) return;
    vec3 screen = projectPosition(view);
    if (any(lessThan(screen.xy, vec2(0.0))) || any(greaterThan(screen.xy, vec2(1.0)))) return;
    vec2 extent = (0.6 * size) * vec2(cb.projectionTransform[0][0], cb.projectionTransform[1][1]) / (2.0 * view.z);
    vec3 low = centre - 0.5 * size;
    vec4 sh = vec4(0.0), colour = vec4(0.0);
    vec3 axes = vec3(0.0);
    float hits = 0.0, empty = 0.0;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            vec2 s01 = screen.xy + ((vec2(float(i), float(j)) + 0.5) / 4.0 * 2.0 - 1.0) * extent;
            if (any(lessThan(s01, vec2(0.0))) || any(greaterThan(s01, vec2(1.0)))) continue;
            vec2 uv = clampToRect(screenToRect(s01), cb.reciprocalResolution.xy);
            float depth = readDepthLod(t_depth, uv);
            vec3 p = toWorld(s01) * depth;
            vec3 inCell = p - low;
            if (all(greaterThanEqual(inCell, vec3(0.0))) && all(lessThan(inCell, vec3(size)))) {
                vec3 light = min(pows(linearLod(t_source, uv).rgb, 2.2), vec3(4.0));
                float l = luma(light);
                vec3 n = normalize(GetWorldNormalLod(t_normals, uv));
                sh += CosineLobe(n) * l;
                colour += vec4(light, l);
                axes += abs(n);
                hits += 1.0;
            }
            else if (depth > view.z + size) empty += 1.0;
        }
    }
    if (hits + empty < 4.0) return;
    float share = 1.44 / 16.0 * cb.gridScreen.x;
    o_sh = mix(o_sh, sh * share, 0.5);
    o_colour = mix(o_colour, colour * share, 0.5);
    o_walls = cb.gridWalls.x > 0.5 ? mix(o_walls, vec4(axes * (1.44 / 16.0), hits / 16.0), 0.5) : vec4(0.0);
}
