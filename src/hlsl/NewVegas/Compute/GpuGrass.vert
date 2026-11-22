// GPU grass: one instance per visible strand (effects/GpuGrass.cpp, GpuGrassCull.comp wrote them). Near list (GG_FAR 0): 4 levels
// along the strand, 5 triangles (15 vertices, a triangle list); far list (GG_FAR 1): one triangle. Lit exactly as the game's own
// grass pixel shader lights its cards ((ambient x light scale + sun x NdotL x light scale x dimmer) x colour, gamma space, no NVR
// light scales), the colour of the card's own texture at the strand's height on the plant (a 4-step ramp read once per card),
// a little lighter or darker per strand, and the game's grass fog (per vertex, as its VS). All in the vertex shader: the colour is
// exact at the levels (the ramp's knots) and linear between them, so the fragment shader only writes it.
//
// Build: glslangValidator -V --target-env vulkan1.1 -DGG_FAR=0 -o GpuGrassNearVS.spv GpuGrass.vert
//        glslangValidator -V --target-env vulkan1.1 -DGG_FAR=1 -o GpuGrassFarVS.spv GpuGrass.vert
//        (2026-11-17, the strands' records for SunFar) -DGG_FAR=0 -DGG_RECORDS=1 -o GpuGrassNearRecordsVS.spv, -DGG_FAR=1 -DGG_RECORDS=1 -o
//        GpuGrassFarRecordsVS.spv (Vulkan SDK 1.3.296.0). GpuGrassNearVS / FarVS / the casters were NOT rebuilt then: the frame block grew by
//        two vec4 at its end (cloud, cloudLight), which they do not read; a rebuild with this glslang changes their ids, not their code.
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "GpuGrass.glsl"

#ifndef GG_FAR
#define GG_FAR 0
#endif
// UNOFFICIAL 2026-11-04 GG_CASTER 1: the same strands into the flashlight's shadow map (GpuGrassNearCasterVS / GpuGrassFarCasterVS with
// GpuGrassCaster.frag): through its matrix, the distance to the light over its reach in o_spot.a
#ifndef GG_CASTER
#define GG_CASTER 0
#endif
// UNOFFICIAL 2026-11-17 fast-restart GG_RECORDS 1: the same strands' records for SunFar (GpuGrassNearRecordsVS / GpuGrassFarRecordsVS with
// GpuGrassRecords.frag): the tuft's normal (world up, bent toward the eye just enough to face it, as GRASS23x000TMS.pso's grass_n) and the
// camera-relative point
#ifndef GG_RECORDS
#define GG_RECORDS 0
#endif

layout(set = 0, binding = 0, std140) uniform Frame { GG_FRAME_BLOCK } f;
layout(set = 0, binding = 3, std430) readonly buffer Draws { GGDraw draws[]; };
layout(set = 0, binding = 4, std430) readonly buffer Blades { GGBlade blades[]; };

layout(location = 0) out vec4 o_color;
// UNOFFICIAL 2026-11-04 the deferred flashlight, added in the fragment shader with its shadow (GpuGrass.frag): rgb its light on the strand
// (linear, after the fog's share), a the distance to the light over its reach; the strand point in the shadow map's clip space; x the depth
// bias, y the PCF radius (texels), z the map's size (0: no shadow), w the gamma the light is added through
layout(location = 1) out vec4 o_spot;
layout(location = 2) out vec4 o_spotClip;
layout(location = 3) out vec4 o_spotParams;

#if GG_FAR
// root left, root right, tip
const uvec2 kVerts[3] = uvec2[3](uvec2(0u, 0u), uvec2(0u, 1u), uvec2(3u, 0u));
#else
// (level, side): level 3 is the tip
const uvec2 kVerts[15] = uvec2[15](
    uvec2(0u, 0u), uvec2(0u, 1u), uvec2(1u, 0u),
    uvec2(0u, 1u), uvec2(1u, 1u), uvec2(1u, 0u),
    uvec2(1u, 0u), uvec2(1u, 1u), uvec2(2u, 0u),
    uvec2(1u, 1u), uvec2(2u, 1u), uvec2(2u, 0u),
    uvec2(2u, 0u), uvec2(2u, 1u), uvec2(3u, 0u));
#endif

void main() {
    const uint cap = uint(f.misc.y);
    if (uint(gl_InstanceIndex) >= cap) {   // more strands were counted than fit (the cull pass stopped writing): nothing
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        o_color = vec4(0.0);
        o_spot = vec4(0.0); o_spotClip = vec4(0.0); o_spotParams = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const GGBlade b = blades[(GG_FAR != 0 ? cap : 0u) + uint(gl_InstanceIndex)];
    const uvec2 v = kVerts[gl_VertexIndex];
    const float t = float(v.x) * (1.0 / 3.0);

    const uint di = b.packed & 0xFFFu;
    const float ang = float((b.packed >> 12) & 63u) * (6.2831853 / 63.0);
    const float seed = float((b.packed >> 18) & 63u) * (1.0 / 63.0);
    const float lenShare = float(b.packed >> 24) * (1.0 / 255.0);
    const vec2 lightAmounts = unpackHalf2x16(b.light);   // x light scale, y sun amount

    // the Bezier from the root (p0 = root, p1 = root + ctrl, p2 = root + tip)
    vec3 pos = b.a.xyz + (2.0 * (1.0 - t) * t) * b.ctrl + (t * t) * b.tip;
    const vec3 side = vec3(-sin(ang), cos(ang), 0.0);
    const float w = b.a.w * (1.0 - t * t);
    if (v.x < 3u) pos += side * ((float(v.y) - 0.5) * w);
    // a sideways wobble from level to level (a dry twig's kinks)
    pos += side * (f.shape2.y * b.a.w * sin(t * 9.0 + seed * 6.2831853) * (0.5 + 0.5 * seed));
    // UNOFFICIAL 2026-11-04 bent away from the player, as the card grass ([Shaders.Grass.Interaction], GpuGrass.glsl ggBend)
    if (f.bend.w > 0.0) pos += ggBend(b.a.xyz, pos - b.a.xyz, f.bend);
#if GG_CASTER
    {   // the flashlight's map: its clip space (D3D9's y up flipped as below), the distance over the reach (the map's value)
        const vec3 sp = pos + f.spotShadowShift.xyz;
        const vec4 lc = sp.x * f.spotShadowRows[0] + sp.y * f.spotShadowRows[1] + sp.z * f.spotShadowRows[2] + f.spotShadowRows[3];
        gl_Position = vec4(lc.x, -lc.y, lc.z, lc.w);
        o_color = vec4(0.0); o_spotClip = vec4(0.0); o_spotParams = vec4(0.0);
        o_spot = vec4(0.0, 0.0, 0.0, length(pos - f.spot[0].xyz) / max(f.spot[0].w, 1.0));
        return;
    }
#endif

    const vec4 clip = vec4(dot(f.rows[0].xyz, pos) + f.rows[0].w, dot(f.rows[1].xyz, pos) + f.rows[1].w,
                           dot(f.rows[2].xyz, pos) + f.rows[2].w, dot(f.rows[3].xyz, pos) + f.rows[3].w);
    gl_Position = vec4(clip.x, -clip.y, clip.z, clip.w);   // D3D9 clip space has y up, Vulkan's viewport y down
#if GG_RECORDS
    {
        vec3 n = vec3(0.0, 0.0, 1.0);
        const vec3 v = -pos * inversesqrt(max(dot(pos, pos), 1.0e-12));
        n += v * max(0.15 - dot(n, v), 0.0);
        o_color = vec4(n, f.cloudLight.z);   // (location 0: GpuGrassRecords.frag i_normal; w the grass mark)
        o_spot = vec4(pos, f.cloudLight.w);  // (location 1: i_pos; w the class)
        o_spotClip = vec4(0.0); o_spotParams = vec4(0.0);
        return;
    }
#endif

    const GGDraw d = draws[di];
    // the card's colour at this height on the plant (a short strand's top shows the plant's middle colour)
    const float hp = clamp(t * lenShare, 0.0, 1.0) * 3.0;
    vec3 albedo = hp < 1.0 ? mix(d.ramp[0].rgb, d.ramp[1].rgb, hp)
                : hp < 2.0 ? mix(d.ramp[1].rgb, d.ramp[2].rgb, hp - 1.0)
                           : mix(d.ramp[2].rgb, d.ramp[3].rgb, hp - 2.0);
    const float tp = hp * (1.0 / 3.0);
    albedo *= 1.0 + (seed - 0.5) * 2.0 * f.look.x;                    // each strand a little lighter or darker
    albedo += (f.look.y * tp * tp * tp) * vec3(0.10, 0.08, 0.0);        // a warm lift toward the tip
    const float gray = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    albedo = max(mix(vec3(gray), albedo, f.look.z), vec3(0.0));

    vec3 color = (d.ambient.rgb * lightAmounts.x + d.sun.rgb * lightAmounts.y) * albedo;
    color *= f.look.w;   // UNOFFICIAL 2026-11-04 [Shaders.Grass.Main] Brightness and NightBrightness, as the card grass (before the fog)
    // UNOFFICIAL 2026-11-04 the grass lamps ([Shaders.Grass.Lights]), as on the card grass: after the night brightness, times Brightness
    if (f.lampData.x > 0.0) {
        vec3 lamps;
        GG_LAMPS(f, pos, vec3(cos(ang), sin(ang), 0.0), lamps)
        color += lamps * albedo * f.lampData.z;
    }
    // UNOFFICIAL 2026-11-04 the deferred flashlight, as on the card grass (GRASS23x000TMS.pso GrassSpot): its light is linear, so the
    // fragment shader adds it in linear light, times its shadow, and puts the sum back in the stored curve (misc.w the gamma)
    const float g = f.misc.w > 0.0 ? f.misc.w : 1.0;
    vec3 spot = vec3(0.0);
    float distShare = 0.0;
    if (f.spot[0].w > 0.0) {
        const vec3 toLight = f.spot[0].xyz - pos;
        const float d2 = dot(toLight, toLight);
        const float share2 = d2 / (f.spot[0].w * f.spot[0].w);
        float att = clamp(1.0 - share2, 0.0, 1.0);
        if (f.spotShadowParams.w > 0.0) att = att * att / (1.0 + f.spotShadowParams.w * share2);   // 2026-11-04 the light pass's Falloff
        const vec3 dir = toLight * inversesqrt(max(d2, 1.0));
        const float cone = smoothstep(f.spot[1].w, f.spot[2].w, dot(-dir, f.spot[1].xyz));
        const float wrap = 0.5 + 0.5 * abs(dot(vec3(cos(ang), sin(ang), 0.0), dir));
        spot = f.spot[2].rgb * (att * cone * cone * wrap) * pow(albedo, vec3(g));
        distShare = sqrt(d2) / f.spot[0].w;
    }
    const vec3 sp = pos + f.spotShadowShift.xyz;   // the strand point as the shadow map's matrix takes it
    o_spotClip = sp.x * f.spotShadowRows[0] + sp.y * f.spotShadowRows[1] + sp.z * f.spotShadowRows[2] + f.spotShadowRows[3];
    o_spotParams = vec4(f.spotShadowParams.x, f.spotShadowParams.y, f.spotShadowShift.w > 0.5 ? f.spotShadowParams.z : 0.0, g);

    // the game's grass fog (GRASS23x00*.vso)
    const float fogDist = length(vec3(clip.xy, clip.w - clip.z));
    const float fogStrength = 1.0 - clamp((f.fogParam.x - fogDist) / f.fogParam.y, 0.0, 1.0);
    const float fogAmount = pow(fogStrength, f.fogParam.z);
    color = mix(color, f.fogColor.rgb, fogAmount);
    spot *= 1.0 - fogAmount;   // the share of the light the fog lets through (as the light pass's keep)

    // DebugView 1: near strands red, far strands blue (lit by the light scale only); 2: a colour per game grass draw
    if (f.misc.z > 0.5 && f.misc.z < 1.5) color = (GG_FAR != 0 ? vec3(0.15, 0.35, 1.0) : vec3(1.0, 0.15, 0.1)) * (0.5 + 0.5 * lightAmounts.x);
    if (f.misc.z > 1.5) color = vec3(float(ggHash(di) & 255u), float(ggHash(di + 1u) & 255u), float(ggHash(di + 2u) & 255u)) * (1.0 / 255.0);
    if (f.misc.z > 0.5) spot = vec3(0.0);

    o_color = vec4(color, 1.0);
    o_spot = vec4(spot, distShare);
}
