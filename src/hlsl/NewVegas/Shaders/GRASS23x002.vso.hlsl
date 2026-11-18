// Grass VS -- vanilla GRASS23x002.vso (BSSM_GRASS_DIRONLY_LFS), plus shadowWorldPos.
// REGISTER HAZARD
// Instanced: `mova a0.x` from TEXCOORD1 indexes c20[a0.x], so InstanceData spans c20..c247 and
// the shadow matrices are relocated to c248/c252. Compiled map: ModelViewProj c9, InstanceData
// c20-c247, matrices c248-c255. Any TESR_ constant added here must be checked against it.
#define SHADOW_INVPROJ_REG c248
#define SHADOW_INVVIEW_REG c252
#include "includes/Shadow.hlsl"
#include "includes/GrassWind.hlsl"   // UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind]; reads the lanes after Shadow.hlsl's matrix)

uniform float3 DiffuseDir   : register(c0);
uniform float3 DiffuseColor : register(c1);
uniform float3 ScaleMask    : register(c2);
uniform float4 WindData     : register(c4);
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
// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) the player walking through the grass ([Shaders.Grass.Interaction],
// GrassShaders, as There Stands The Grass NVSE does it): xyz the bend origin, camera-relative, w 1 / its reach; the push per unit of
// blade height is TESR_Translucency.w. c16 and c17 are free here (the game writes none of c16-c19 for grass).
uniform float4 TESR_GrassBend : register(c16);
// Per grass shape (GrassShaders::SetBatchConstants, from the hooked per-shape setup): x, y the size fade ([Shaders.Grass.LOD], folded
// on the CPU: GrassLODFade), z 1 for a shape that stands still (rocks: no sway, no bending), w 1 for the bend's debug tint.
uniform float4 GrassBatch : register(c17);
// UNOFFICIAL 2026-11-04 the flashlight caster (GRASS_SHADOW_CASTER 2, core/GrassShadows.h BeginSpot): c13-c16 camera-relative world -> the spot
// shadow map's clip space (ShaderManager SpotLightWorldToLightMatrix[0]), c18 the light (camera-relative) and w its reach. Free registers
// in the caster build only (set and put back per draw).
#if defined(GRASS_SHADOW_CASTER) && GRASS_SHADOW_CASTER == 2
row_major float4x4 GrassSpotCaster : register(c13);
float4 GrassSpotCasterLight : register(c18);
#endif

// c20..c247, below the relocated matrices.
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
    float4 f_shadow_world_pos : TEXCOORD1;   // vanilla leaves TEXCOORD1 unused on this pair
    float4 f_ambient        : TEXCOORD4;
    float4 f_sun            : TEXCOORD5;   // .w = distance fade
    float4 f_light_normal   : TEXCOORD2;   // UNOFFICIAL 2026-11-04: xyz the normal the sun lights the grass by, w its N.L (the PS bends it by a normal map)
    float3 f_sun_dir        : TEXCOORD3;   // DiffuseDir, for the PS
    float4 f_amount         : TEXCOORD6;   // UNOFFICIAL: x ambient amount, y sun amount, z the sun amount before N.L, w the translucency (plain numbers, see below)
    float4 f_fog            : COLOR0;      // .w = fog amount
#if defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 3 && !defined(GRASS_SHADOW_CASTER)
    // UNOFFICIAL 2026-11-12 CapturedGrassTint (GRASS23x000TMS.pso.hlsl, the records at main's end; only in a build with the AlbedoBuffer's
    // records, so every other build is the shader it was): the sun's colour WITHOUT the vertex colour. f_sun carries vertex colour x sun
    // colour; the grass's colour record needs to know how much of the sun the vertex colour takes away, and dividing f_sun by the vertex
    // colour fails where that is 0 (blade roots). A register copy: no literal, no constant register (this shader has none free).
    float3 f_sun_colour     : TEXCOORD7;
#endif
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

    // Fade on the instance origin. 4-component length (dp4 r2, r2), not xyz.
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
        gone.f_position = float4(2.0f, 2.0f, 0.5f, 1.0f);
        return gone;
    }

    // Orientation and light scale are packed into the fractional parts.
    float4 ff = frac(inst_id);
    float3 loc_orient = (ff.xyz - 0.5f) * 2.0f;      // -1 .. 1
    float light_scale = ff.w * 0.75f + 0.25f;

    // Better-conditioned perpendicular: (0,-z,y) when |y|>=|x| and |z|>=|x|, else (-z,0,x).
    bool use_alt = (abs(loc_orient.y) >= abs(loc_orient.x)) && (abs(loc_orient.z) >= abs(loc_orient.x));
    float3 loc_perp = use_alt ? float3(0.0f, -loc_orient.z, loc_orient.y)
                         : float3(-loc_orient.z, 0.0f, loc_orient.x);

    float3 tan_vec  = normalize(loc_perp);
    float3 bin_vec = cross(tan_vec, loc_orient);

    // Scale, then place on the billboard basis.
    float3 scl = (0.01f * inst_id.w) * ScaleMask.xyz + 1.0f;
    float3 local_p = scl * va.f_position.xyz;
    float3 pos_w = bin_vec * local_p.x + tan_vec * local_p.y + loc_orient * local_p.z;

    // Sun off the card orientation.
    float n_dot_l = saturate(dot(DiffuseDir, loc_orient));

    // Per-instance phase from inst.xy, stiffness from vertex alpha squared.
    float ph = (inst_id.x + inst_id.y) * 0.0078125f + WindData.w;
    ph = frac(ph * 0.159154937f + 0.5f) * 6.28318548f - 3.14159274f;   // wrap to -pi..pi
    float swing_f = sin(ph);
    // UNOFFICIAL 2026-10-18 sandstorm wind (sandport; ported from Physically-Based-Reloaded, Balebandro / Alejandro Martinez,
    // GPL-3.0): in a sandstorm the grass sways harder, leans downwind in gusts that travel across the field with the wind, and
    // flutters. c19.y (GrassPairing.y, written by GrassShaders::SetForDraw from SandstormEffect): the extra sway S, 0 with no
    // storm (the branch is not taken: the game's sway exactly). c19.z: the gust clock u (0..8 pi), gusts at u / 4, flutter at u.
    // No literal is added: this shader's free registers are full of them, and one more spills past c255 (shader refused).
    [branch] if (GrassPairing.y > 0.0f) {
        float gust_f = 0.75f + 0.25f * sin(GrassPairing.z * 0.25f - dot(inst_id.xy, WindData.xy) * 0.0078125f);
        float flutter_f = sin(GrassPairing.z + (inst_id.x - inst_id.y) * 0.0078125f);
        swing_f = swing_f * (1.0f + GrassPairing.y) + 1.5f * GrassPairing.y * (0.75f * gust_f + 0.25f * flutter_f);
    }
    float sway_f = swing_f * WindData.z * (va.f_color.w * va.f_color.w) * (1.0f - GrassBatch.z);   // GrassBatch.z 1: a static shape (rocks), no sway
    pos_w.xy += sway_f * WindData.xy;

    // UNOFFICIAL 2026-10-20 grass field wind ([Shaders.Grass.Wind], Includes/GrassWind.hlsl, core/GrassWind.h): gusts that roll across
    // the field along the weather's wind, a flutter per tuft, the game's sway turned onto that wind (cloth's direction and strength).
    // The lanes are the 4th column of the inverse view matrix rows (no free register here: see GrassWind.hlsl), the clock is c19.z.
    // Lanes xy 0 (the effect off: that column is the matrix's own 0 0 0 1) = the branch is not taken, the line above alone: the game's
    // sway exactly. Taken, the position is worked out again from the vertex with the field wind in place of the game's sway.
    const float4 wind_lanes = float4(TESR_InvViewTransform._m03, TESR_InvViewTransform._m13, TESR_InvViewTransform._m23, TESR_InvViewTransform._m33);
    [branch] if (dot(wind_lanes.xy, wind_lanes.xy) > 0.0f && GrassBatch.z < 0.5f) {
        pos_w = bin_vec * local_p.x + tan_vec * local_p.y + loc_orient * local_p.z + GrassWindMove(inst_id, wind_lanes, GrassPairing.z,
            swing_f, WindData, va.f_color.w * va.f_color.w, local_p.z);
    }

    // UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) [Shaders.Grass.LOD]: the tuft faded by its size on screen; gone, it is
    // dropped below. [Shaders.Grass.Interaction]: pushed away from the player (Includes/GrassWind.hlsl GrassBend), at the tuft's root
    // carried back to the camera-relative world (GrassCameraRelative). Not in the shadow caster, whose c252-c255 hold the cascade's
    // transform: its tufts cast unbent.
    float lod_fade = GrassLODFade(inst_clip, scl.z, GrassBatch);
    vr.f_sun.w *= lod_fade;
    float bend_reach = 0.0f;
#ifndef GRASS_SHADOW_CASTER
    pos_w += GrassBend(GrassCameraRelative(inst_clip), pos_w, TESR_GrassBend, TESR_Translucency.w, bend_reach) * (1.0f - GrassBatch.z);
#endif

    float4 world_pos = float4(pos_w + inst_id.xyz, 1.0f);
    vr.f_position = mul(ModelViewProj, world_pos);

#ifdef GRASS_SHADOW_CASTER
    // UNOFFICIAL 2026-10-27 grass sun shadows ([Shaders.Grass.Main] CastShadows, core/GrassShadows.h; user: "billboard bushes have
    // shadows as they should", the grass tufts threw none): this file compiled a second time as the caster that draws the tufts into
    // the near sun shadow map. For those draws GrassShadows puts the camera's view -> the cascade's shadow map space into the xyz of
    // c252-c255 (the grass wind lanes in their w are kept), so the shadow-position transform below lands on the tuft's place in the
    // map, the same transform the shadow lookups use. No lighting, fog or collision push here (the push reads those registers too).
    float caster_fade = vr.f_sun.w;
#if GRASS_SHADOW_CASTER == 2
    // the flashlight's map: the tuft through its perspective, writing the distance to the light over its reach (as the spot map holds it);
    // the position as the card shaders place it (GrassCameraRelative: exact at any depth, round the player's feet too)
    float3 caster_rel = GrassCameraRelative(vr.f_position);
    float4 caster_clip = mul(float4(caster_rel, 1.0f), GrassSpotCaster);
    float3 caster_light = float3(0.0f, 0.0f, length(caster_rel - GrassSpotCasterLight.xyz) / GrassSpotCasterLight.w);
#else
    float3 caster_light = GetShadowWorldPos(vr.f_position);
#endif
    vr = (VtxVaryings)0;
    vr.f_position = float4(caster_light, 1.0f);
#if GRASS_SHADOW_CASTER == 2
    vr.f_position = caster_clip;
#endif
    vr.f_uv = va.f_uv;
    vr.f_shadow_world_pos = float4(caster_light, 1.0f);   // the caster PS writes z, as ShadowMap.pso does
    vr.f_sun.w = caster_fade;
#else
    // UNOFFICIAL: backlit translucency joins the sun term, so the PS shadow applies to it too. Skipped at Translucency 0.
    // UNOFFICIAL 2026-10-17: the game's grass PS reads no shadow position: skip its two matrix transforms unless NVR's PS or the
    // translucency needs it.
    float3 shadow_world_pos = 0.0f;
    // UNOFFICIAL 2026-11-04: GrassCameraRelative (Includes/GrassWind.hlsl), exact, and it reads only _11 and _22 of the inverse
    // projection, so the compiler keeps c248-c249 of it and the literals get c250-c251 (the bend and the size fade took c16-c17).
    [branch] if (GrassPairing.x > 0.5f || TESR_Translucency.x > 0.0f) shadow_world_pos = GrassCameraRelative(vr.f_position);
    float translucent_f = 0.0f;
    [branch] if (TESR_Translucency.x > 0.0f) {
        translucent_f = FoliageTranslucency(loc_orient, DiffuseDir, shadow_world_pos, DiffuseDir);
        n_dot_l += translucent_f;
    }

    // Kept separate so the PS can shadow the sun alone.
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
    // UNOFFICIAL 2026-11-04 [Shaders.Grass.Interaction] Debug: the grass the bend reaches, in red.
    vr.f_ambient.rgb = lerp(vr.f_ambient.rgb, float3(1.0f, 0.0f, 0.0f), bend_reach * GrassBatch.w);
    vr.f_amount = float4(light_scale, light_scale * n_dot_l * AddlParams.x, light_scale * AddlParams.x, translucent_f);
    vr.f_light_normal = float4(loc_orient, n_dot_l - translucent_f);
    vr.f_sun_dir = DiffuseDir;
#if defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 3
    vr.f_sun_colour = DiffuseColor;   // UNOFFICIAL 2026-11-12 CapturedGrassTint (see VtxVaryings)
#endif

    // Fog from clip-space xyz distance.
    float fog_dist = length(float3(vr.f_position.xy, vr.f_position.w - vr.f_position.z));
    float fog_strength = 1.0f - saturate((FogParam.x - fog_dist) / FogParam.y);
    vr.f_fog.rgb = FogColor.rgb;
    vr.f_fog.w = pow(fog_strength, FogParam.z);

    vr.f_uv = va.f_uv;

    vr.f_shadow_world_pos = float4(shadow_world_pos, SHADOW_VS_SENTINEL);

#endif   // GRASS_SHADOW_CASTER
    if (lod_fade <= 0.0f) vr.f_position = float4(2.0f, 2.0f, 0.5f, 1.0f);   // faded out by its size: off the screen whole
    return vr;
};
