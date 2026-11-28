// CoverMap, DebugTechnique (needs NVR DXVK): Effects/CoverMap.fx.hlsl DebugPS line for line.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "CoverMapCommon.glsl"

layout(set = 1, binding = 0) uniform texture2D t_rendered;
layout(set = 1, binding = 1) uniform texture2D t_coverMap;
layout(set = 1, binding = 2) uniform texture2D t_coverAux;
layout(set = 1, binding = 3) uniform texture2D t_depth;

layout(push_constant) uniform Push {
    uint pointWrap;
    uint pointClamp;
    uint pad[2];
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o_color;

float CoverMapTrodden(float stored) {
    float d = (1.0 - stored) * cb.trail.z;
    return stored > 0.0001 ? cb.trail.y * (1.0 - smoothstep(0.35 * cb.trail.x, cb.trail.x, d)) : 0.0;
}

vec4 CoverMapReadAll(vec2 rel, out float inside) {
    vec4 stored = textureLod(sampler2D(t_coverMap, NVR_SAMPLER(pc.pointWrap)), rel * cb.data.x + cb.data.yz, 0.0);
    vec2 fromCorner = rel - cb.window.xy;
    vec2 edge = min(fromCorner, cb.window.z - fromCorner);
    inside = saturate(min(edge.x, edge.y) / max(cb.window.w, 1.0)) * cb.globalVal.w;
    return vec4(mix(cb.globalVal.xyz, stored.rgb, inside), stored.a * inside);
}

vec3 DebugColour(vec4 c, vec3 base) {
    float amount = saturate(max(c.r, max(c.g, c.b)));
    vec3 tint = c.r * vec3(0.05, 0.30, 1.0) + c.g * vec3(0.0, 0.55, 0.55) + c.b * vec3(0.9, 0.9, 0.9);
    vec3 shown = base * (1.0 - 0.75 * amount) + min(tint, vec3(1.0)) * 0.85;
    return mix(shown, vec3(0.55, 0.32, 0.12), 0.85 * saturate(c.a));
}

void main() {
    vec2 ruv = uv * cb.renderScale.xy;
    vec4 color = textureLod(sampler2D(t_rendered, NVR_SAMPLER(pc.pointClamp)), ruv, 0.0);
    vec2 pixel = uv / cb.reciprocalResolution.xy;
    float side = cb.debug.y / cb.reciprocalResolution.y;
    vec2 local = (pixel - 16.0) / side;
    if (cb.debug.x > 0.5 && cb.debug.x < 1.5 && local.x >= 0.0 && local.y >= 0.0 && local.x <= 1.0 && local.y <= 1.0) {
        float span = cb.window.z;
        vec2 rel = vec2(local.x - 0.5, 0.5 - local.y) * span;
        vec2 mapUv = rel * cb.data.x + cb.data.yz;
        vec4 c = textureLod(sampler2D(t_coverMap, NVR_SAMPLER(pc.pointWrap)), mapUv, 0.0);
        c.a = CoverMapTrodden(c.a);
        vec4 aux = textureLod(sampler2D(t_coverAux, NVR_SAMPLER(pc.pointWrap)), mapUv, 0.0);
        float known = HeightKnown(aux.r);
        vec3 shown = DebugColour(c, vec3(0.22, 0.22, 0.22));
        shown *= known > 0.5 ? mix(0.6, 1.0, aux.g) : 1.0;
        float contour = known > 0.5 && fract(aux.r * HeightRange / 256.0) < 0.2 ? 1.0 : 0.0;
        shown *= 1.0 - 0.45 * contour;
        float hatch = fract((pixel.x + pixel.y) / 10.0) < 0.5 ? 1.0 : 0.0;
        shown *= known > 0.5 ? 1.0 : 1.0 - 0.35 * hatch;
        vec2 fromCorner = rel - cb.window.xy;
        float edge = min(min(fromCorner.x, span - fromCorner.x), min(fromCorner.y, span - fromCorner.y));
        float pixelUnits = span / side;
        shown = abs(edge) < 2.0 * pixelUnits ? vec3(1.0, 0.85, 0.0) : shown;
        shown = edge < -2.0 * pixelUnits ? shown * 0.35 : shown;
        shown = length((local - 0.5) * side) < 4.0 ? vec3(1.0, 0.0, 0.0) : shown;
        shown = cb.globalVal.w > 0.5 ? shown : vec3(0.5, 0.0, 0.5);
        o_color = vec4(shown, 1.0);
        return;
    }
    if (cb.debug.x > 1.5) {
        float depth = readDepthLod(t_depth, pc.pointClamp, ruv);
        if (depth < cb.cameraData.y * 0.99) {
            vec3 rel = toWorld(ruv) * depth;
            float inside;
            vec4 c = CoverMapReadAll(rel.xy, inside);
            c.a = CoverMapTrodden(c.a);
            vec3 shown = DebugColour(c, color.rgb);
            shown = mix(shown, shown * vec3(1.0, 0.55, 0.55), 1.0 - inside);
            color.rgb = mix(color.rgb, shown, 0.75);
        }
    }
    o_color = color;
}
