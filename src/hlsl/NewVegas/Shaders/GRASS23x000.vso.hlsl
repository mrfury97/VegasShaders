// Grass VS -- vanilla GRASS23x000.vso (BSSM_GRASS_DIRONLY_LF), plus shadowWorldPos.
// No per-instance rotation basis: the card sits on world axes. Sun term from the instance
// orientation packed in frac(inst).xyz.
// REGISTER HAZARD
// As 002: InstanceData spans c20..c247, shadow matrices relocated to c248/c252.
#define SHADOW_INVPROJ_REG c248
#define SHADOW_INVVIEW_REG c252
#include "includes/Shadow.hlsl"
#include "includes/GrassWind.hlsl"   // UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind]; reads the lanes after Shadow.hlsl's matrix)

uniform float3 DiffuseDir   : register(c0);
uniform float3 DiffuseColor : register(c1);
uniform float3 ScaleMask    : register(c2);
uniform float4 WindData     : register(c4);
#ifdef GRASS_POINT_LIGHT
// UNOFFICIAL 2026-10-27 [Shaders.Grass.Main] PointLights: the game's 1POINT grass (GRASS23x004-007.vso, grass by a lamp or fire)
// includes this file: the same shader plus the lamp's attenuation coordinates, as the game's 1POINT VS writes them (there in
// TEXCOORD1, which is shadowWorldPos here: so in TEXCOORD2, and GRASS23x001TMS/003TMS.pso read it there).
uniform float4 LightPosition : register(c3);   // xyz world position, w radius
#endif
uniform float4 AlphaParam   : register(c5);
uniform float4 AmbientColor : register(c6);
uniform float4 AddlParams   : register(c7);
uniform row_major float4x4 ModelViewProj : register(c9);
uniform float4 FogColor     : register(c14);
uniform float4 FogParam     : register(c15);
// UNOFFICIAL: backlit translucency settings (PBR.cpp, [Shaders.Foliage.Main]). c18 is free: nothing else here uses it and
// InstanceData starts at c20.
uniform float4 TESR_Translucency : register(c18);
// UNOFFICIAL 2026-10-17: which pixel shader reads this one, written per draw by GrassShaders::SetForDraw (SetShadersHook); c19 is
// free (the game writes none of c16-c19 for grass). x 1 = NVR's grass PS (GRASS23x000TMS.pso, used with transparency
// multisampling), 0 = the game's own GRASS23x000.pso (no transparency multisampling, the INI default). Never written reads 0.
// UNOFFICIAL 2026-10-18 sandport: y and z carry the sandstorm's wind (SandstormEffect::GrassStorm; 0 = no storm).
uniform float4 GrassPairing : register(c19);
// UNOFFICIAL 2026-10-19 grass collision ([Shaders.Grass.Collision], effects/GrassCollision.cpp): the push round the camera, a
// vertex texture (vertex sampler 0, bound by GrassShaders::SetForDraw). TESR_Translucency.w is 1 / its width (0 = off).
uniform sampler2D NVR_GrassPush : register(s0);

#ifndef GRASS_INSTANCE_COUNT
    #define GRASS_INSTANCE_COUNT 228
#endif
float4 InstanceData[GRASS_INSTANCE_COUNT] : register(c20);

struct VtxAttrs {
    float4 f_position : POSITION;
    float4 f_color    : COLOR0;
    float2 f_uv       : TEXCOORD0;
    float4 f_instance : TEXCOORD1;   // .x = instance index
};

struct VtxVaryings {
    float4 f_position       : POSITION;
    float2 f_uv             : TEXCOORD0;
    float4 f_shadow_world_pos : TEXCOORD1;
    float4 f_ambient        : TEXCOORD4;
    float4 f_sun            : TEXCOORD5;   // .w = distance fade
#ifdef GRASS_POINT_LIGHT
    float4 f_att            : TEXCOORD2;   // the lamp's AttMap coordinates (xy, z with w = 0.5)
#endif
#ifndef GRASS_POINT_LIGHT
    // UNOFFICIAL 2026-11-05 grass lamps and normal maps (GRASS23x000TMS.pso; from Balebandro's fork): the normal the sun lights
    // the grass by and its N.L, the sun's direction, and the sun amount before N.L and the translucency apart.
    float4 f_light_normal   : TEXCOORD2;
    float3 f_sun_dir        : TEXCOORD3;
    float4 f_amount         : TEXCOORD6;   // UNOFFICIAL: x ambient amount, y sun amount, z the sun amount before N.L, w the translucency
#else
    float2 f_amount         : TEXCOORD6;   // UNOFFICIAL: x ambient amount, y sun amount (plain numbers, see below)
#endif
    float4 f_fog            : COLOR0;      // .w = fog amount
};

// UNOFFICIAL: backlit translucency (docs/research/foliage-translucency.md). Light that reaches the eye through a blade
// from the far side: the surface faces away from the sun (back: N against Lback), the eye looks into the sun through it
// (align), and the sun is above the horizon (gate). p is the camera-relative world position, Lsun the world sun direction.
float FoliageTranslucency(float3 N, float3 lback, float3 pt, float3 lsun) {
    float3 V = -pt / max(length(pt), 1e-6f);
    float back_lit = saturate(-dot(N, lback));
    float align_v = pow(saturate(dot(V, -lsun)), TESR_Translucency.y);
    float gate_f = saturate(lsun.z * TESR_Translucency.z);
    return TESR_Translucency.x * back_lit * align_v * gate_f;
}

VtxVaryings main(VtxAttrs va) {
    VtxVaryings vr;

    int ix = (int)(va.f_instance.x - frac(va.f_instance.x));
    float4 inst_id = InstanceData[ix];

    // Both distances measure (x, y, w - z), not xyz.
    float4 inst_clip = mul(ModelViewProj, float4(inst_id.xyz, 1.0f));
    // UNOFFICIAL: the vanilla fade metric length(x, y, w - z, w) assumes a standard depth buffer, where w - z is only the
    // near plane; with the reversed depth buffer this game runs, w - z is about w again, so distances came out 1.41x too
    // large and the INI fade (fGrassStartFadeDistance / fGrassFadeRange) happened at 70% of its distance over 70% of its
    // range. Measure what vanilla effectively measured.
#ifdef REVERSED_DEPTH
    float inst_dist = length(float3(inst_clip.xy, inst_clip.w));
#else
    float inst_dist = length(float4(inst_clip.xy, inst_clip.w - inst_clip.z, inst_clip.w));
#endif
    vr.f_sun.w = 1.0f - saturate((inst_dist - AlphaParam.z) / AlphaParam.w);
    // UNOFFICIAL 2026-10-17 GrassFirst ([Shaders.Grass.Main] DrawFirst, Render.cpp namespace GrassFirst): the grass in two parts,
    // c19.w (GrassPairing.w, written by GrassShaders::SetForDraw): 1 = only the tufts before the fade start (fully opaque), drawn
    // BEFORE the land so the land's pixel shader skips the pixels they cover; 2 = only the fading tufts, drawn at the game's place
    // after the land (they blend over it, as before). Same test on the same numbers in both: every tuft is in exactly one part.
    // 0 (never written) = every tuft, the game's order. A dropped tuft is folded into one point outside the view (no pixels).
    [branch] if (GrassPairing.w > 0.5f && ((inst_dist > AlphaParam.z) == (GrassPairing.w < 1.5f))) {
        VtxVaryings gone = (VtxVaryings)0;
#ifndef GRASS_POINT_LIGHT   // 1POINT variants: left at 0 (every corner on one point: no pixels either), no literal register
        gone.f_position = float4(2.0f, 2.0f, 0.5f, 1.0f);
#endif
        return gone;
    }

    float ph = (inst_id.x + inst_id.y) * 0.0078125f + WindData.w;
    ph = frac(ph * 0.159154937f + 0.5f) * 6.28318548f - 3.14159274f;
    float swing_f = sin(ph);
    // UNOFFICIAL 2026-10-18 sandstorm wind (sandport; ported from Physically-Based-Reloaded, Balebandro / Alejandro Martinez,
    // GPL-3.0): in a sandstorm the grass sways harder, leans downwind in gusts that travel across the field with the wind, and
    // flutters. c19.y (GrassPairing.y, written by GrassShaders::SetForDraw from SandstormEffect): the extra sway S, 0 with no
    // storm (the branch is not taken: the game's sway exactly). c19.z: the gust clock u (0..8 pi), gusts at u / 4, flutter at u.
    // No literal is added: this shader's free registers are full of them, and one more spills past c255 (shader refused).
#ifndef GRASS_POINT_LIGHT   // 1POINT variants (PointLights): no sandstorm gusts, their literals would push the shader past c255
    [branch] if (GrassPairing.y > 0.0f) {
        float gust_f = 0.75f + 0.25f * sin(GrassPairing.z * 0.25f - dot(inst_id.xy, WindData.xy) * 0.0078125f);
        float flutter_f = sin(GrassPairing.z + (inst_id.x - inst_id.y) * 0.0078125f);
        swing_f = swing_f * (1.0f + GrassPairing.y) + 1.5f * GrassPairing.y * (0.75f * gust_f + 0.25f * flutter_f);
    }
#endif
    float sway_f = swing_f * WindData.z * (va.f_color.w * va.f_color.w);

    float3 scl = (0.01f * inst_id.w) * ScaleMask.xyz + 1.0f;
    float3 pos_w = va.f_position.xyz * scl + float3(sway_f * WindData.xy, 0.0f);

    // UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind], Includes/GrassWind.hlsl, core/GrassWind.h): gusts that roll across
    // the field along the weather's wind, a flutter per tuft, the game's sway turned onto that wind (cloth's direction and strength).
    // The lanes are the 4th column of the inverse view matrix rows (no free register here: see GrassWind.hlsl), the clock is c19.z.
    // Lanes xy 0 (the effect off: that column is the matrix's own 0 0 0 1) = the branch is not taken, the line above alone: the game's
    // sway exactly. Taken, the position is worked out again from the vertex with the field wind in place of the game's sway.
    const float4 wind_lanes = float4(TESR_InvViewTransform._m03, TESR_InvViewTransform._m13, TESR_InvViewTransform._m23, TESR_InvViewTransform._m33);
    [branch] if (dot(wind_lanes.xy, wind_lanes.xy) > 0.0f) {
        pos_w = va.f_position.xyz * scl + GrassWindMove(inst_id, wind_lanes, GrassPairing.z, swing_f, WindData,
            va.f_color.w * va.f_color.w, va.f_position.z * scl.z);
    }

    float4 world_pos = float4(pos_w + inst_id.xyz, 1.0f);
    vr.f_position = mul(ModelViewProj, world_pos);

#ifdef GRASS_SHADOW_CASTER
    // UNOFFICIAL 2026-10-27 grass sun shadows ([Shaders.Grass.Main] CastShadows, core/GrassShadows.h; user: "billboard bushes have
    // shadows as they should", the grass tufts threw none): this file compiled a second time as the caster that draws the tufts into
    // the near sun shadow map. For those draws GrassShadows puts the camera's view -> the cascade's shadow map space into the xyz of
    // c252-c255 (the grass wind lanes in their w are kept), so the shadow-position transform below lands on the tuft's place in the
    // map, the same transform the shadow lookups use. No lighting, fog or collision push here (the push reads those registers too).
    float caster_fade = vr.f_sun.w;
    float3 caster_light = GetShadowWorldPos(vr.f_position);
    vr = (VtxVaryings)0;
    vr.f_position = float4(caster_light, 1.0f);
    vr.f_uv = va.f_uv;
    vr.f_shadow_world_pos = float4(caster_light, 1.0f);   // the caster PS writes z, as ShadowMap.pso does
    vr.f_sun.w = caster_fade;
#else
    // UNOFFICIAL 2026-10-19 grass collision ([Shaders.Grass.Collision]; after Community Shaders' Grass Collision, GPL-3.0): the player
    // and the people and creatures near them push the grass aside, and it springs back as they pass (GrassCollision.cpp). The push
    // at a blade's tip is read at the TUFT'S ROOT (its clip position, instClip, carried back to the camera-relative world), so the whole
    // tuft leans away as one (2026-10-20: read where each vertex stood, a tuft straddling someone's feet had its corners pushed
    // apart -- the user's 'garbled twisted pieces of grass flying around my character'); the vertex moves
    // by its height above the root (2026-10-20, user: 'not doing anything i can see': it used the sway's alpha squared, 0.02-0.5 at
    // the tips of the Mojave's grass models, so most grass hardly moved): none at the root, the full push from 50 units up
    // (GrassPushWeight, NewGrass.vso takes the same at the same height), and drops as it leans so the blade bends, not stretches.
    // Only literals this shader already holds are used: its free constant registers are all taken by them.
    // Past the texture's half-width round the camera (its edge texels are 0, clamped) and wherever no one pushes, the push is 0
    // and the vertex stays where it was: those (most of the grass on screen) skip the read and the second transform.
    [branch] if (TESR_Translucency.w > 0.0f) {
        float2 push_uv = GrassCameraRelative(inst_clip).xy * TESR_Translucency.w;   // (not GetShadowWorldPos: see GrassCameraRelative)
        [branch] if (max(abs(push_uv.x), abs(push_uv.y)) < 0.5f) {
            float2 push_v = tex2Dlod(NVR_GrassPush, float4(push_uv + 0.5f, 0.0f, 0.0f)).xy * GrassPushWeight(pos_w.z);
            [branch] if (dot(push_v, push_v) > 0.0f) {
                float hgt = max(pos_w.z, 1e-6f);
                world_pos.xy += push_v;
                world_pos.z -= min(dot(push_v, push_v) * 0.5f / hgt, hgt);
                vr.f_position = mul(ModelViewProj, world_pos);
            }
        }
    }

    float4 ff = frac(inst_id);
    float light_scale = ff.w * 0.75f + 0.25f;
    float3 loc_orient = (ff.xyz - 0.5f) * 2.0f;
    float n_dot_l = saturate(dot(DiffuseDir, loc_orient));

    // UNOFFICIAL: backlit translucency joins the sun term, so the PS shadow applies to it too. Skipped at Translucency 0.
    // UNOFFICIAL 2026-10-17: the game's grass PS reads no shadow position: skip its two matrix transforms unless NVR's PS or the
    // translucency needs it.
    float3 shadow_world_pos = 0.0f;
    float translucent_f = 0.0f;	// UNOFFICIAL 2026-11-05 (the PS bends the sun's N.L by a normal map without it)
    [branch] if (GrassPairing.x > 0.5f || TESR_Translucency.x > 0.0f) shadow_world_pos = GetShadowWorldPos(vr.f_position);
    [branch] if (TESR_Translucency.x > 0.0f) {
        translucent_f = FoliageTranslucency(loc_orient, DiffuseDir, shadow_world_pos, DiffuseDir);
        n_dot_l += translucent_f;
    }

    // UNOFFICIAL linear lighting (2026-10-14): colours and plain numbers apart. The PS decodes the colours (sun, vertex colour,
    // ambient) to linear; the blade's light scale and the sun's angle are already plain amounts, and decoding them too (they
    // used to be multiplied in here) squared them darker: the grass came out about half as bright as it should.
    if (GrassPairing.x > 0.5f) {
        vr.f_ambient = AmbientColor;
        vr.f_sun.xyz = va.f_color.rgb * DiffuseColor;
    }
    else {
        // FIX 2026-10-17: the game's own grass PS just adds TEXCOORD4 and TEXCOORD5 and multiplies by the texture, so it needs the
        // amounts inside them, as the game's VS (and P79) wrote them. Since P80 (2026-10-14) it got the bare colours: every blade
        // in full sun on every side (too bright, flat), for everyone without transparency multisampling.
        vr.f_ambient = light_scale * AmbientColor;
        vr.f_sun.xyz = ((light_scale * va.f_color.rgb) * n_dot_l) * DiffuseColor * AddlParams.x;
    }
#ifndef GRASS_POINT_LIGHT
    vr.f_amount = float4(light_scale, light_scale * n_dot_l * AddlParams.x, light_scale * AddlParams.x, translucent_f);	// UNOFFICIAL 2026-11-05
    vr.f_light_normal = float4(loc_orient, n_dot_l - translucent_f);
    vr.f_sun_dir = DiffuseDir;
#else
    vr.f_amount = float2(light_scale, light_scale * n_dot_l * AddlParams.x);
#endif

    float fog_dist = length(float3(vr.f_position.xy, vr.f_position.w - vr.f_position.z));
    float fog_strength = 1.0f - saturate((FogParam.x - fog_dist) / FogParam.y);
    vr.f_fog.rgb = FogColor.rgb;
    vr.f_fog.w = pow(fog_strength, FogParam.z);

    vr.f_uv = va.f_uv;
    vr.f_shadow_world_pos = float4(shadow_world_pos, SHADOW_VS_SENTINEL);
#ifdef GRASS_POINT_LIGHT
    vr.f_att = float4((LightPosition.xyz - world_pos.xyz) / LightPosition.w * 0.5f + 0.5f, 0.5f);
    // The game's own grass PS (no transparency multisampling, c19.x 0) reads them in TEXCOORD1, as from its own VS.
    if (GrassPairing.x < 0.5f) vr.f_shadow_world_pos = vr.f_att;
#endif

#endif   // GRASS_SHADOW_CASTER
    return vr;
};
