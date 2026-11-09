//  Template for terrain shaders for blending up to 7 textures and using up to 8 pointlights.

#if defined(__INTELLISENSE__)
    #define PS	// implied: pixel shader
    #define NUM_PT_LIGHTS 24
#endif

#define TERRAIN	// implied: land

// UNOFFICIAL 2026-10-26 (NVR DXVK fork, docs\next\FORK-1026-LIMITS.md): NVR_NO_SM3_LIMITS and NVR_SM4 are defined only for the
// no-limits build of these shaders (tools\nvr-dxvk\replace\nolimits.py: fxc ps_4_0 -> GLSL -> SPIR-V, which the fork runs in place of
// the ps_3_0 shader the game loads). That build has no 32-temp or instruction limit, so the cuts ps_3_0 forced (marked
// NVR_NO_SM3_LIMITS here and in the includes) are left out there. The ps_3_0 build never defines them: it is exactly as before.
#ifndef NVR_NO_SM3_LIMITS
    #define NVR_NO_SM3_LIMITS 0
#endif
#ifndef NVR_SM4
    #define NVR_SM4 0
#endif

#define REVERSED_DEPTH	// implied: NVR's reversed depth buffer

#ifndef TEX_COUNT
    #define TEX_COUNT 1
#endif

#if defined(NUM_PT_LIGHTS) && (NUM_PT_LIGHTS > 24)	// point lights
    #define NUM_PT_LIGHTS 24
#endif
// UNOFFICIAL 2026-10-30 LandAllLamps (core/LandLamps.h): NVR adds the piece's other lamps after Vanilla Plus Terrain's, so every land
// shader with lamps has room for 24 (the loop runs to PointLightCount as before).
#if defined(LAND_ALL_LAMPS) && defined(NUM_PT_LIGHTS)
    #undef NUM_PT_LIGHTS
    #define NUM_PT_LIGHTS 24
#endif

#include "includes/Helpers.hlsl"	// shared code
// UNOFFICIAL linear lighting (Includes/LinearLight.hlsl, [Shaders.PBR.Linear]): the land lights in linear space while it is on.
// Compiled in only when on at startup (LINEAR_LIGHTING_ON, ShaderRecord::LoadShader).
#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if defined(PS) && LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
#endif
#include "includes/Terrain.hlsl"	// shared code
#include "includes/Parallax.hlsl"	// shared code

// BaseMap[7] holds s0-s6 and NormalMap[7] holds s7-s13, so the atlas cannot use the s9
// default. s14/s15 are the only free sampler slots in ps_3_0 here.
#define SHADOW_ATLAS_SAMPLER_REG s14
#define SHADOW_OLD_ATLAS_SAMPLER_REG s15 // the sun cross-fade's kept atlas (SUN_CROSSFADE only)
#ifdef PS
    #define SHADOW_CLOUD_VALUE 1   // UNOFFICIAL 2026-11-04 ForwardCloudShadows (Includes/Shadow.hlsl): no sampler is free here, the vertex shader looks the map up
#endif
#include "includes/Shadow.hlsl"	// shared code
// UNOFFICIAL 2026-11-04 ForwardCloudShadows: the clouds' shadow per vertex (texc.z; w the sentinel), compiled in only when on at startup.
#if FORWARD_CLOUD_SHADOWS && FORWARD_SHADOWS && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS)
    #define TERRAIN_CLOUD 1
#else
    #define TERRAIN_CLOUD 0
#endif
#include "includes/CapturedNormals.hlsl"	// UNOFFICIAL 2026-10-28 CapturedNormals (COLOR3, see there)
// UNOFFICIAL 2026-11-04 macro variation ([Shaders.Terrain.Macro], Includes/TerrainMacro.hlsl): compiled in only when on at startup.
#ifndef TERRAIN_MACRO
    #define TERRAIN_MACRO 0
#endif
#if TERRAIN_MACRO && defined(PS)
#include "includes/TerrainMacro.hlsl"
#endif

// UNOFFICIAL terrain blending ([Shaders.Terrain.Blending]), ported from Balebandro's Physically-Based-Reloaded fork
// (Alejandro Martinez, fork commit a23a624b), which follows the idea of Community Shaders' Terrain Blending (Skyrim).
// Compiled in only when Enabled at startup (TERRAIN_BLEND from ShaderRecord::LoadShader), so that otherwise these shaders
// are exactly what they were. It needs s15, which the sun cross-fade's kept atlas takes: not with SUN_CROSSFADE.
#ifndef TERRAIN_BLEND
    #define TERRAIN_BLEND 0
#endif
#if TERRAIN_BLEND && !SUN_CROSSFADE
    #define TERRAIN_BLEND_ACTIVE 1
#else
    #define TERRAIN_BLEND_ACTIVE 0
#endif
// HeightEdge 2's height contrast (terrainBlendEdge) takes 7 layers with SkylightingMode 1 past ps_3_0's 32 temporary registers
// (2026-10-20, d3dx9_43 with the game's defines; the shader would fail and that land keep the game's own shader). There
// HeightEdge 2 keeps its band but weighs the plain height, like HeightEdge 1.
// (Not in the no-limits build, NVR_NO_SM3_LIMITS at the top: no 32-temp limit there, so HeightEdge 2 keeps its contrast.)
#if TEX_COUNT >= 7 && SKYLIGHTING_MODE && !NVR_NO_SM3_LIMITS
    #define HEIGHT_EDGE2_CONTRAST 0
#else
    #define HEIGHT_EDGE2_CONTRAST 1
#endif


struct VS_INPUT {	// the vertex stream
    float4 vpos : POSITION;
    float3 tanV : TANGENT;
    float3 binV : BINORMAL;
    float3 nrmV : NORMAL;
    float4 texc : TEXCOORD0;
    float4 vcol : COLOR0;
    float4 bw0 : TEXCOORD1;
    float4 bw1 : TEXCOORD2;
};

struct VS_OUTPUT {	// vertex to pixel
    float4 bw0 : COLOR0;
    float4 bw1 : COLOR1;
    float4 scrPos : POSITION;
#if TERRAIN_CLOUD
    float4 texc : TEXCOORD0;   // zw: the clouds' sun share, the sentinel
#else
    float2 texc : TEXCOORD0;
#endif
    float3 vcol : TEXCOORD1;
    float4 localPos : TEXCOORD2;   // w: the view depth (terrain blending)
    float3 tanO : TEXCOORD3;
    float3 binO : TEXCOORD4;
    float3 nrmO : TEXCOORD5;
    float4 clipPos : TEXCOORD6;
    float3 eyeO : TEXCOORD7;
};

#ifdef VS	// vertex shader

row_major float4x4 ModelViewProj : register(c0);	// object to clip space

float4 FogParam : register(c14);	// x fog far, y fog range, z fog power
float3 FogColor : register(c15);	// the game's fog colour
float4 EyePosition : register(c16);	// camera, object space

#if TERRAIN_CLOUD
// UNOFFICIAL 2026-11-04 ForwardCloudShadows (core/ForwardCloudShadows.h sets these per draw, and binds the map on vertex sampler 1; c160-c161: no vertex shader and no DLL upload uses them, c240-c243 being the object vertex shaders' TESR_InvProjectionTransform):
float4 NVR_CloudSunVS : register(c160);       // as the pixel shaders' NVR_CloudSun (Includes/Shadow.hlsl): xy the map's uv under the camera, z uv per unit, w strength
float4 NVR_CloudSunLightVS : register(c161);  // xy: the map's light, x / z and y / z
sampler2D NVR_CloudShadowVS : register(s1);
#endif

VS_OUTPUT main(VS_INPUT vin) {
    VS_OUTPUT vo;

    float4 clipP = mul(float4x4(ModelViewProj[0].xyzw, ModelViewProj[1].xyzw, ModelViewProj[2].xyzw, ModelViewProj[3].xyzw), vin.vpos.xyzw);

    vo.bw0 = vin.bw0;
    vo.bw1 = vin.bw1;

    vo.scrPos.xyzw = clipP;
    vo.texc.xy = vin.texc.xy;
#if TERRAIN_CLOUD
    // The land's pieces are placed unrotated and unscaled: the vertex less the camera (both in the piece's space) is camera-relative world.
    float3 rel = vin.vpos.xyz - EyePosition.xyz;
    float cloud = 1.0f;
    [branch] if (NVR_CloudSunVS.w > 0.0f) {
        float2 suv = NVR_CloudSunVS.xy + (rel.xy - NVR_CloudSunLightVS.xy * rel.z) * NVR_CloudSunVS.z;
        float2 edge = min(suv, 1.0f - suv);
        cloud = lerp(1.0f, tex2Dlod(NVR_CloudShadowVS, float4(suv, 0.0f, 0.0f)).r, saturate(min(edge.x, edge.y) * 10.0f) * NVR_CloudSunVS.w);
    }
    vo.texc.zw = float2(cloud, SHADOW_VS_SENTINEL);
#endif
    vo.vcol.xyz = clamp(vin.vcol.rgb, 0.0f, 1.0f);
    vo.localPos.xyz = vin.vpos.xyz;
    vo.localPos.w = clipP.w;   // UNOFFICIAL: the view depth, for terrain blending (clip w is the depth along the view)
    vo.tanO.xyz = vin.tanV.xyz;
    vo.binO.xyz = vin.binV.xyz;
    vo.nrmO.xyz = vin.nrmV.xyz;
    vo.clipPos.xyzw = clipP;
    vo.eyeO.xyz = EyePosition.xyz;

    return vo;
};

#endif	// VS

struct PS_INPUT	// vertex to pixel
{
#if TERRAIN_CLOUD
    float4 texc : TEXCOORD0;   // zw: the clouds' sun share, the sentinel (ForwardCloudShadows)
#else
    float2 texc : TEXCOORD0;
#endif
    float3 vcol : TEXCOORD1_centroid;
#if TERRAIN_BLEND_ACTIVE
    // Not centroid with terrain blending: it takes this position's screen derivatives, which centroid sampling spoils at
    // triangle edges (with anti-aliasing) by moving the sample point there. w: the view depth, likewise not centroid.
    float4 localPos : TEXCOORD2;
#else
    float3 localPos : TEXCOORD2_centroid;
#endif
    float3 tanO : TEXCOORD3_centroid;
    float3 binO : TEXCOORD4_centroid;
    float3 nrmO : TEXCOORD5_centroid;
    float4 bw0 : COLOR0;
    float4 bw1 : COLOR1;
    float4 clipPos : TEXCOORD6_centroid;
    float3 eyeO : TEXCOORD7_centroid;
    float4 scrPos : POSITION1;
#if TERRAIN_BLEND_ACTIVE
#if NVR_SM4
    float4 screenPos4 : VPOS;   // SM4 (no-limits build): SV_Position, the pixel centre; screenPos below takes D3D9's VPOS from it
#else
    float2 screenPos : VPOS;
#endif
#endif
};

struct PS_OUTPUT {	// render targets
    float4 finalCol : COLOR0;
#if CAPTURED_NORMALS_ON
    // (ps_3_0 needs COLOR0..COLOR3 contiguous: COLOR1 / COLOR2 written as 0, discarded where nothing is bound there)
    float4 capturedPad1 : COLOR1;
    float4 capturedPad2 : COLOR2;
    float4 capturedNormal : COLOR3;	// UNOFFICIAL 2026-10-28 CapturedNormals (Includes/CapturedNormals.hlsl; bound only in the world's main list)
#endif
};

#ifdef PS	// pixel shader

sampler2D BaseMap[7]:register(s0);	// layer colours
sampler2D NormalMap[7]:register(s7);	// layer normals (alpha: height with Vanilla Plus Terrain)

float4 AmbientColor : register(c1);	// rgb ambient light; a 1 = opaque, no alpha test
float4 SunColor : register(c3);	// the sun's colour
float4 SunDir : register(c18);	// direction to the sun

float4 LandSpec[2] : register(c32);	// per layer specular power
float4 LandHeight[2] : register(c34);	// per layer: has a height map (Vanilla Plus Terrain)
float4 FogParam : register(c36);	// x fog far, y fog range, z fog power
float4 FogColor : register(c37);	// the game's fog colour

#ifdef NUM_PT_LIGHTS	// point lights
float4 PointLightColor[NUM_PT_LIGHTS] : register(c39);	// rgb per lamp
float4 PointLightPosition[NUM_PT_LIGHTS] : register(c63);	// per lamp: xyz position, w radius
float PointLightCount : register(c88);	// lamps in use
#endif

#ifndef TERRAIN_LAMP_RT
    #define TERRAIN_LAMP_RT 0
#endif
#if TERRAIN_LAMP_RT && defined(NUM_PT_LIGHTS)
// UNOFFICIAL 2026-10-30 RT lamp shadows ExteriorForward on the LAND (core/PointShadowForward.h BindTerrain; docs/next/FORK-RT-LAMPS.md).
// The land's lamps are the game's land light list (PointLightPosition, the land's own space), not lamps the DLL can name per draw. A
// vector between two points is the same in every space, so a lamp's camera-relative position is the pixel's (shadowWorldPos) plus its
// vector to the lamp; it is matched by position to the pixel's traced entries (RtLampTrace.comp: three slots + visibility, the traced
// frame reprojected). Register audit (2026-10-30, the 28 land shaders): samplers 0-14 taken, s15 is TerrainBlending's (so this record
// is made only with it off); c146-c166 and c168-c175 free.
float4 NVR_TerrainLampSlot[20] : register(c146);            // the trace's slots 0-19: xyz camera-relative position, w 1 = traced
row_major float4x4 NVR_TerrainLampReproject : register(c168); // camera-relative position (this frame) -> the traced frame's clip
float4 NVR_TerrainLampRT : register(c172);                  // x 1 = on, y depth tolerance, z log2(farZ) / 4095, w strength
float4 NVR_TerrainLampPack : register(c173);                // (2026-11-03 rtext) x the packing unit U (1024; 4096 ManyLights), y 1 = ManyLights:
                                                            // entry 2 is R, the traced share of every lamp not among the 2 exact ones;
                                                            // z (2026-11-05) 1 = debug (ExteriorDebug 3): the lamps' light red where traced
                                                            // blocked, green where lit; w (2026-11-07) 1 / the result's width (the
                                                            // edge search's texel; 0 = none)
sampler2D NVR_TerrainLampVis : register(s15);               // the traced result (point sampled, set by the DLL)

// The pixel's three traced entries: their visibility and their lamps' positions (once a pixel; the slot table read in a loop, the
// only way ps_3_0 indexes constants)
void TerrainLampEntries(float3 worldPos, out float3 vis, out float3 p0, out float3 p1, out float3 p2, out float3 ok) {
    vis = 1.0f; ok = 0.0f; p0 = 0.0f; p1 = 0.0f; p2 = 0.0f;
    [branch] if (!(NVR_TerrainLampRT.x > 0.5f)) return;
    float4 clip = mul(float4(worldPos, 1.0f), NVR_TerrainLampReproject);
    [branch] if (!(clip.w > 1.0f)) return;
    float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    [branch] if (any(abs(uv - 0.5f) > 0.5f)) return;
    float2 traced = tex2Dlod(NVR_TerrainLampVis, float4(uv, 0.0f, 0.0f)).xy;
    const float U = NVR_TerrainLampPack.x > 0.5f ? NVR_TerrainLampPack.x : 1024.0f;   // an entry: visibility x U / 16 + slot
    float code = floor(traced.y / U);
    float err = abs(exp2(code * NVR_TerrainLampRT.z) - clip.w) / clip.w;
    // (2026-11-07) where the land meets an object's silhouette its own traced texel may be the object's: the four neighbours one texel
    // away, the first with this point's depth (PointShadowForward.hlsl does the same); none: not traced here (lit)
    [branch] if (!(err <= NVR_TerrainLampRT.y)) {
        float tx = NVR_TerrainLampPack.w;
        bool found = false;
        [branch] if (tx > 0.0f) {
            float ty = tx * length(float3(NVR_TerrainLampReproject[0][1], NVR_TerrainLampReproject[1][1], NVR_TerrainLampReproject[2][1])) /
                max(length(float3(NVR_TerrainLampReproject[0][0], NVR_TerrainLampReproject[1][0], NVR_TerrainLampReproject[2][0])), 1.0e-6f);
            [loop] for (float i = 0.0f; i < 3.5f; i += 1.0f) {
                float2 o = i < 0.5f ? float2(tx, 0.0f) : i < 1.5f ? float2(-tx, 0.0f) : i < 2.5f ? float2(0.0f, ty) : float2(0.0f, -ty);
                float2 t2 = tex2Dlod(NVR_TerrainLampVis, float4(uv + o, 0.0f, 0.0f)).xy;
                float c2 = floor(t2.y / U);
                [branch] if (abs(exp2(c2 * NVR_TerrainLampRT.z) - clip.w) <= NVR_TerrainLampRT.y * clip.w) { traced = t2; code = c2; found = true; break; }
            }
        }
        [branch] if (!found) return;
    }
    float hi = floor(traced.x / U);
    float3 e = float3(traced.x - U * hi, hi, traced.y - U * code);
    float3 v = floor(e / (U / 16.0f));
    float3 s = e - (U / 16.0f) * v;
    vis = v / 15.0f;
    // ManyLights packing: entry 2 is R (0..4095), no slot of its own; ok.z stays 0 (TerrainLampShadow takes vis.z for the other lamps)
    [flatten] if (NVR_TerrainLampPack.y > 0.5f) { vis.z = saturate(e.z / 4095.0f); s.z = -100.0f; }
    [loop] for (int j = 0; j < 20; j++) {
        float4 L = NVR_TerrainLampSlot[j];
        [flatten] if (abs(s.x - j) < 0.5f) { p0 = L.xyz; ok.x = L.w; }
        [flatten] if (abs(s.y - j) < 0.5f) { p1 = L.xyz; ok.y = L.w; }
        [flatten] if (abs(s.z - j) < 0.5f) { p2 = L.xyz; ok.z = L.w; }
    }
}
// A land lamp's shadow: the visibility of the traced entry at its position (within 8 units), else lit (not traced here, as before)
float TerrainLampShadow(float3 lampPos, float3 vis, float3 p0, float3 p1, float3 p2, float3 ok) {
    float3 m = ok * float3(length(p0 - lampPos) < 8.0f, length(p1 - lampPos) < 8.0f, length(p2 - lampPos) < 8.0f);
    float c = dot(m, 1.0f);
    // (2026-11-03 rtext) not one of the exact entries: lit, or with the ManyLights packing R (where the traced frame saw this point)
    const float rest = NVR_TerrainLampPack.y > 0.5f ? vis.z : 1.0f;   // (vis stays 1 where nothing was traced: lit)
    return lerp(1.0f, c > 0.5f ? dot(m, vis) / c : rest, NVR_TerrainLampRT.w);
}
#endif

// UNOFFICIAL 2026-11-04 Wetness Effects (Includes/Wetness.hlsl, from Physically-Based-Reloaded; in place of WetWorld's screen pass): the
// land darkens, gains the water film and puddles, and the rain ripples on it. Its constants (c146-c157) are the RT lamp slots' above and
// the sun cross-fade's: not with the cross-fade; with the lamp slots (TERRAIN_LAMP_RT) they move (2026-11-09, WETNESS_ALT_REGS). The rain cover map on s13, free below seven texture layers (the seventh layer's normal map
// there): seven-layer land goes without the cover test (open everywhere). The layers' height maps shape the wetness and the puddles
// (WETNESS_HEIGHT, c158: [Shaders.WetnessEffects.Main] HeightAwareness, CreviceFill, NormalAwareness), as in Physically-Based-Reloaded.
#if defined(PS) && defined(WETNESS_EFFECTS) && !defined(WETNESS_OFF) && FORWARD_SHADOWS && !SUN_CROSSFADE
    #define TERRAIN_WETNESS
    #if TERRAIN_LAMP_RT && defined(NUM_PT_LIGHTS)
        #define WETNESS_ALT_REGS    // 2026-11-09: the lamp slots hold c146-c173 here; the water layer takes c2, c4-c17, c98-c99 (Wetness.hlsl)
    #endif
    #if TEX_COUNT < 7
        #define WETNESS_ORTHO_REG s13
    #endif
    #define WETNESS_RAINDROPS
    #define WETNESS_HEIGHT      // the land's height maps shape the wetness: water gathers in the crevices
    #include "includes/Wetness.hlsl"
#endif


// UNOFFICIAL 2026-10-27 displacement ObjectBlend ([Shaders.Terrain.Displacement], Compute/Tess/TerrainDisplace.tese): the world's
// depth as the land starts (terrain blending's copy, TerrainShaders::PrepareDisplaceDepth), for the land's tessellation evaluation
// shader, which may only read textures this pixel shader uses, in s0-s7 (NVR DXVK 1.7-displace). s6 is BaseMap[6]'s, free below 7
// layers (the compiler gives BaseMap only the layers it reads). Compiled in only with [Main.Main.Performance] NvrDxvkDisplacement on
// at startup (NVR_DISPLACE_DEPTH from ShaderRecord::BuildDefines); this shader never reads it (see the end of main).
#if defined(NVR_DISPLACE_DEPTH) && TEX_COUNT < 7
    #define DISPLACE_DEPTH_IN 1
// One line (ShaderTextureValue::GetSamplerStateString). Linear: the TES's blend must not jump between texels (cracks).
sampler2D TESR_DisplaceDepth : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#else
    #define DISPLACE_DEPTH_IN 0
#endif

#if TERRAIN_BLEND_ACTIVE
// The world's depth as the first land draw of the scene started (TerrainShaders::PrepareBlending) holds the objects
// drawn before the land: rocks, walls, props. Where the land covers one of them by less than the blend distance, the
// land fades out over that depth, so the object's buried part shows through and it sinks into the ground instead of
// ending in a hard line. Smooth (default): the land is drawn see-through there (its alpha; Terrain.cpp turns blending on
// for its opaque pass) over the object, which the game drew before the land - a smooth fade without TAA. Off: a dither
// (clip), which averages into a fade only with TAA (it moves the pattern every frame), a fine still stipple without.
// Registers pinned clear of the game's (land PS tops out at c93) and of every other NVR constant.
float4 TESR_TerrainBlendData : register(c176);  // x: on for this draw, y: 1 / blend distance, z: max distance, w: debug view
float4 TESR_TerrainBlendExtra : register(c177); // xy: 1 / resolution, z: dither frame (0 without TAA), w: 1 Smooth (blended, not dithered)
float4 TESR_TerrainBlendShape : register(c178); // x: ground width, y: height blend, z: slope blend, w: minimum pixels
// One line (ShaderTextureValue::GetSamplerStateString).
sampler2D TESR_TerrainBlendDepth : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// View depth of a depth-buffer value through the camera's inverse projection: only its z and w columns depend on the
// depth (the jitter and the screen position live in x and y). For the scene behind the land; the land's own comes from its
// vertex shader (clip w), the same distance along the view.
float terrainBlendViewDepth(float depth) {
    float2 zw = depth * float2(TESR_InvProjectionTransform[2][2], TESR_InvProjectionTransform[2][3]) +
        float2(TESR_InvProjectionTransform[3][2], TESR_InvProjectionTransform[3][3]);
    return abs(zw.x / zw.y);   // positive, as the land's clip w it is compared with
}

// UNOFFICIAL 2026-10-14: ported from mrfury97's NVR fork (commit 0a7cbdc5, "band below the surface, constant ground width",
// itself after Terrain Blending - NVSE). The distance is how far the object's surface lies below the land, along the land's
// normal: the same from every viewing angle and on slopes. The band has a constant width across the ground: a fixed depth
// is depth / tan(angle) wide there, a crisp seam at a wall but metres of see-through land over a shallow road shoulder, and
// a whole buried flat mesh (ending in a hard line at its border) when it lies parallel to the land. So the depth that fades
// is min(BlendDistance, GroundWidth x tan(angle)). The angle: the object's surface rebuilt from the depth copy at this pixel
// and its four neighbours, on each axis the side without a jump (mesh edges, a different object), against the land's normal
// and its triangle's facet (the flatter; the flattest pairing, so creases don't count); the same neighbours give how fast the
// gap changes from pixel to pixel (the fade's minimum width on screen).
struct TerrainBlendGap {
    float covered;    // how far below the land surface the object is (along the land's normal; 0 or less: none)
    float width;      // how much that changes from one pixel to the next
    float tangent;    // tan of the angle between the object's surface and the ground (0: parallel)
};

// The object behind a land point: along the same view ray, at the depth copy's view depth (view depth grows linearly
// along a ray). In the land's space.
float3 terrainBlendObject(float2 uv, float3 landPos, float landViewDepth, float3 eye) {
    float sceneViewDepth = terrainBlendViewDepth(tex2Dlod(TESR_TerrainBlendDepth, float4(uv, 0.0f, 0.0f)).x);
    return eye + (landPos - eye) * (sceneViewDepth / max(landViewDepth, 1e-3f));
}

// landPos, eye: the land point and the eye in the land's space; landViewDepth: its view depth; dPx, dPy, dZx, dZy: their
// ddx/ddy (taken at the top level); normal: the land's normal.
TerrainBlendGap terrainBlendGap(float2 screenPos, float3 landPos, float landViewDepth, float3 dPx, float3 dPy, float dZx, float dZy, float3 eye, float3 normal) {
    TerrainBlendGap gap;
    gap.width = 0.0f;
    gap.tangent = 0.0f;
    float2 texel = TESR_TerrainBlendExtra.xy;
    float2 uv = (screenPos + 0.5f) * texel;
    float3 o = terrainBlendObject(uv, landPos, landViewDepth, eye);
    gap.covered = dot(landPos - o, normal);
    // UNOFFICIAL 2026-10-15: ported from mrfury97's fork (6a384add, "no opaque land along the contact line"). A land pixel
    // only draws in front of the object behind it (the depth test), so a negative gap is the two depths' imprecision (and
    // anti-aliasing's sampling) right where the object meets the ground: it counts as 0, the object at the surface, so the
    // land is fully faded there. Read as "object in front", it drew the land opaque along the contact line: a thin bright
    // line between faded pixels. Tolerance: 1 unit plus 0.1 % of the view distance.
    float tolerance = 1.0f + landViewDepth * 0.001f;
    float blendDistance = 1.0f / TESR_TerrainBlendData.y;
    float limit = blendDistance * 4.0f * max(1.0f, TESR_TerrainBlendShape.z);
    [branch] if (TESR_TerrainBlendData.x > 0.0f && gap.covered > -tolerance && gap.covered < limit) {
        gap.covered = max(gap.covered, 0.0f);
        float3 oR = terrainBlendObject(uv + float2(texel.x, 0.0f), landPos + dPx, landViewDepth + dZx, eye);
        float3 oL = terrainBlendObject(uv - float2(texel.x, 0.0f), landPos - dPx, landViewDepth - dZx, eye);
        float3 oD = terrainBlendObject(uv + float2(0.0f, texel.y), landPos + dPy, landViewDepth + dZy, eye);
        float3 oU = terrainBlendObject(uv - float2(0.0f, texel.y), landPos - dPy, landViewDepth - dZy, eye);
        float3 eR = oR - o, eL = o - oL, eD = oD - o, eU = o - oU;
        float3 ex = length(eR) < length(eL) ? eR : eL;   // the side without a jump
        float3 ey = length(eD) < length(eU) ? eD : eU;
        // Both sides of an axis where neither jumps (within 2x of each other): at a crease the sides lie on different
        // faces, so every pairing is tried and the flattest kept.
        float3 ex2 = (length(eR) < 2.0f * length(eL) && length(eL) < 2.0f * length(eR)) ? (length(eR) < length(eL) ? eL : eR) : ex;
        float3 ey2 = (length(eD) < 2.0f * length(eU) && length(eU) < 2.0f * length(eD)) ? (length(eD) < length(eU) ? eU : eD) : ey;
        float3 facet = normalize(cross(dPx, dPy));
        float3 n1 = normalize(cross(ex, ey)), n2 = normalize(cross(ex2, ey)), n3 = normalize(cross(ex, ey2)), n4 = normalize(cross(ex2, ey2));
        float sineFacet = min(min(length(cross(n1, facet)), length(cross(n2, facet))), min(length(cross(n3, facet)), length(cross(n4, facet))));
        float sineNormal = min(min(length(cross(n1, normal)), length(cross(n2, normal))), min(length(cross(n3, normal)), length(cross(n4, normal))));
        float sine = min(sineFacet, sineNormal);
        gap.tangent = sine / max(sqrt(saturate(1.0f - sine * sine)), 1e-3f);
        gap.width = abs(dot(dPx - ex, normal)) + abs(dot(dPy - ey, normal));
    }
    return gap;
}

// How deep the fade reaches: BlendDistance (times SlopeBlend on steep land), but no more than GroundWidth across the
// ground. slope: 1 - normal.z.
float terrainBlendDepthRange(TerrainBlendGap gap, float slope) {
    float distance = (1.0f / TESR_TerrainBlendData.y) * lerp(1.0f, TESR_TerrainBlendShape.z, saturate(slope * 2.0f));
    return min(distance, TESR_TerrainBlendShape.x * gap.tangent);
}

// How much of the land to keep at this pixel: 1 opaque, 0 gone; -1 where blending does not apply. The fade is at least
// MinPixels wide on screen (a band only a pixel deep crawls as the view moves; 2026-10-14) and at most four times its depth
// (so a hill in front of a distant rock never turns see-through at its edge).
float getTerrainBlend(TerrainBlendGap gap, float landViewDepth, float slope) {
    float maxDistance = TESR_TerrainBlendData.z;
    if (landViewDepth > maxDistance || gap.covered < 0.0f) return -1.0f;   // the object in front: the depth test handles it
    float keep;
    // UNOFFICIAL 2026-10-20 HeightEdge 2 (a community update to HeightEdge, sent to the user as TerrainTemplate.hlsl): the band is
    // BlendDistance (x SlopeBlend on steep land) alone, no ground-width or pixel terms, and the land is solid already at 35 % of it,
    // so more of the ground stays round rocks and walls. Sent as HeightBlend + 10, negated (Terrain.cpp).
    [branch] if (TESR_TerrainBlendShape.y < -9.5f) {
        const float slopeFactor = lerp(1.0f, TESR_TerrainBlendShape.z, saturate(slope * 2.0f));
        const float range = max((1.0f / TESR_TerrainBlendData.y) * slopeFactor, 0.05f);
        keep = saturate(gap.covered / max(range * 0.35f, 0.0001f));
    }
    else {
        float distance = terrainBlendDepthRange(gap, slope);
        if (distance < 0.05f) return -1.0f;   // parallel to the ground: nothing to fade
        float range = clamp(gap.width * TESR_TerrainBlendShape.w, distance, distance * 4.0f);
        keep = saturate(gap.covered / range);
    }
    // Back to a hard edge over the last quarter before the max distance, so there is no visible line.
    return lerp(keep, 1.0f, saturate((landViewDepth / maxDistance - 0.75f) * 4.0f));
}

// A natural edge: where the land's texture is high (its height map: the diffuse alpha), the land reaches a little further
// over the object, where it is low a little less. Strongest mid-fade, none at either end. height 0-1.
float terrainBlendEdge(float keep, float height) {
    if (keep >= 1.0f || keep < 0.0f) return keep;
    // UNOFFICIAL 2026-10-17 HeightEdge 1 (community submission, sent to the user): the land and the object weighed like the
    // land's own layers in landHeightBlend (coverage^(1+s) x (0.001 + height^(4s)), the object at a neutral height 0.5), so
    // the edge follows the land texture's height map: sand fills the gaps between pebbles, crisper than the nudge below.
    // Sent as a negative HeightBlend (TESR_TerrainBlendShape.y < 0; Terrain.cpp), so no new constant is needed.
    [branch] if (TESR_TerrainBlendShape.y < 0.0f) {
        // HeightEdge 2 (2026-10-20 update, see getTerrainBlend): the land's height pushed to strong contrast round the middle
        // (x 8) and lifted 0.20, so the edge follows the texture's high parts crisply. HeightEdge 1: the height as it is.
        const bool edge2 = TESR_TerrainBlendShape.y < -9.5f;
        const float strength = edge2 ? -TESR_TerrainBlendShape.y - 10.0f : -TESR_TerrainBlendShape.y;
        if (strength <= 0.0f) return keep;
#if HEIGHT_EDGE2_CONTRAST
        if (edge2) height = saturate((height - 0.5f) * 8.0f + 0.5f + 0.20f);
#endif
        const float coveragePower = 1.0f + strength, heightPower = 4.0f * strength;
        const float terrainWeight = pow(keep, coveragePower) * (0.001f + pow(saturate(height), heightPower));
        const float meshWeight = pow(1.0f - keep, coveragePower) * (0.001f + pow(0.5f, heightPower));
        return terrainWeight / max(terrainWeight + meshWeight, 0.000001f);
    }
    return saturate(keep + (height - 0.5f) * TESR_TerrainBlendShape.y * 4.0f * keep * (1.0f - keep));
}

// The blended height (diffuse alpha where a layer has a height map, else 0.5) for terrainBlendEdge.
// Explicit gradients (dx, dy: the uv's, taken at the top level): it runs inside a branch.
float terrainBlendHeight(float2 uv, float2 dx, float2 dy, int texCount, sampler2D tex[7], float weights[7], float heightStatus[7]) {
    float height = 0.0f;
    [unroll] for (int i = 0; i < texCount; i++)
        height += (heightStatus[i] > 0.0f ? tex2Dgrad(tex[i], uv, dx, dy).a : 0.5f) * weights[i];
    return height;
}

// Interleaved gradient noise (Jimenez); the frame offset moves it only while TAA runs.
float terrainBlendNoise(float2 screenPos) {
    float2 p = screenPos + 5.588238f * TESR_TerrainBlendExtra.z;
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}
#endif


PS_OUTPUT main(PS_INPUT pin) {
    PS_OUTPUT po;

#if TERRAIN_BLEND_ACTIVE
#if NVR_SM4
    const float2 screenPos = pin.screenPos4.xy - 0.5f;   // D3D9's VPOS: the pixel's corner
#else
    const float2 screenPos = pin.screenPos;
#endif
    // First, so a dithered-away pixel skips the rest where the GPU can (whole quads).
    float terrainKeep = -1.0f;
    // (The derivatives at the top level: every pixel of the 2 x 2 quad.)
    float3 blendLandPos = pin.localPos.xyz;
    // UNOFFICIAL 2026-10-14 (user: white lines on the land after the terrain blending update, with the game's anti-aliasing
    // on): the land's view depth from the vertex shader (lPosition.w), not centroid, as in mrfury97's commit. It was worked
    // out from projectionPosition, which is centroid sampled: at triangle edges its screen derivatives jump, the gap's
    // width and angle came out wrong along every edge of the land's mesh, and the fade cut lines there.
    float blendViewDepth = pin.localPos.w;
    float3 blendNormal = normalize(pin.nrmO.xyz);
    TerrainBlendGap blendGap = terrainBlendGap(screenPos, blendLandPos, blendViewDepth, ddx(blendLandPos), ddy(blendLandPos),
        ddx(blendViewDepth), ddy(blendViewDepth), pin.eyeO.xyz, blendNormal);
    [branch] if (TESR_TerrainBlendData.x > 0.0f) {
        terrainKeep = getTerrainBlend(blendGap, blendViewDepth, 1.0f - blendNormal.z);
        // UNOFFICIAL 2026-10-13 (user: blend instead, so it needs no TAA): Smooth draws the land see-through there instead
        // (its alpha below; Terrain.cpp turns blending on for the land's opaque pass): the object, drawn before the land,
        // shows through it in a smooth fade. A pixel with next to nothing of the land left is skipped here, first (the
        // height-shaped edge below cannot bring back a pixel this far gone).
        if (TESR_TerrainBlendData.w <= 0.0f && TESR_TerrainBlendExtra.w > 0.5f && terrainKeep >= 0.0f && terrainKeep < 0.004f)
            clip(-1.0f);
    }
#endif

    int texLayers = TEX_COUNT;  // Macro.
    float3 tanN = normalize(pin.tanO.xyz);
    float3 binN = normalize(pin.binO.xyz);
    float3 nrmN = normalize(pin.nrmO.xyz);
    float3x3 tbnMat = float3x3(tanN, binN, nrmN);
    float3 toEye = normalize(mul(tbnMat, pin.eyeO.xyz - pin.localPos.xyz));
    float viewDist = length(pin.eyeO.xyz - pin.localPos.xyz);

    float2 gradX, gradY;
    gradX = ddx(pin.texc.xy);
    gradY = ddy(pin.texc.xy);

    float wts[7] = { 0, 0, 0, 0, 0, 0, 0 };
    float blendW[7] = { pin.bw0.x, pin.bw0.y, pin.bw0.z, pin.bw0.w, pin.bw1.x, pin.bw1.y, pin.bw1.z };
    float specA[7] = { LandSpec[0].x, LandSpec[0].y, LandSpec[0].z, LandSpec[0].w, LandSpec[1].x, LandSpec[1].y, LandSpec[1].z };
    float hStat[7] = { LandHeight[0].x, LandHeight[0].y, LandHeight[0].z, LandHeight[0].w, LandHeight[1].x, LandHeight[1].y, LandHeight[1].z };

    // Camera-relative world position, for the forward sun shadows below and for telling ground under the water.
    // No extra interpolator needed: clipPos is already the clip-space position,
    // and clip position is affine in object space, so it interpolates exactly.
    // ddx/ddy must stay at top level, outside dynamic flow control.
    float3 camPos = GetShadowWorldPos(pin.clipPos);
#if defined(CAPTURED_NORMALS_FORWARD) && CAPTURED_NORMALS_FORWARD
    // UNOFFICIAL 2026-10-28 [Main.Main.Quality] CapturedNormals: the sun lookup's normal offset / slope slack and the sky ambient take the
    // land's smooth vertex normal (with tessellation the bent one) instead of each triangle's flat one (shadow steps and ambient facets
    // triangle by triangle), as SmoothObjectNormals did for objects (2026-10-24).
    float3 geoN = ShadowFacingNormal(pin.nrmO.xyz, camPos);
#else
    float3 geoN = GetShadowGeometricNormal(camPos);
#endif

    // [Main.Main.ReducedQuality] CheapUnderwaterTerrain: ground below TESR_TerrainParallaxExtraData.w (camera-relative
    // height of the water line; -FLT_MAX when off) skips parallax and its shadows, exactly as with parallax switched off.
    // It is seen through moving, refracting water. The parallax functions take explicit gradients, so they may branch.
    bool parallax = camPos.z >= TESR_TerrainParallaxExtraData.w;
    float2 offsetUV = pin.texc.xy;
    [branch] if (parallax) offsetUV = getParallaxCoords(viewDist, pin.texc.xy, gradX, gradY, toEye, texLayers, BaseMap, blendW, hStat, wts);
    else wts = blendW;

    float glossV = 0.0f;
    float specPow = 0.0f;
    float2 odx = ddx(offsetUV), ody = ddy(offsetUV);	// UNOFFICIAL SkipEmptyLayers: the gradients tex2D took implicitly
    // UNOFFICIAL 2026-10-19 LOD Blending (DisableVertexColors): the near land without its vertex colours, to match far land generated
    // without them (xLODGen's Vertex Color Intensity 0), as Community Shaders' DisableTerrainVertexColors. z is 0 (off): unchanged.
    float3 landVertexColor = lerp(pin.vcol, 1.0f, TESR_LODBlendData.z);
#if TERRAIN_VARIATION
    // UNOFFICIAL 2026-10-19 Terrain Variation (Includes/TerrainVariation.hlsl; after Conor's port of Community Shaders' Terrain
    // Variation). The lattice is built from the ORIGINAL uv, not offsetUV: a surface point has to keep the same hash offset whatever
    // the view angle, and offsetUV moves with it. The samples still go to offsetUV + hash, so parallax still applies (its own height
    // march reads the plain tiling: two taps per march step would not fit ps_3_0's budget). (Conor.) Nearer than StartDistance the
    // single tap, the second fading in over the next w units.
    StochasticOffsets stochastic = ComputeStochasticOffsets(pin.texc.xy, TESR_TerrainVariationData.x);
    stochastic.tap1Weight = lerp(1.0f, stochastic.tap1Weight, saturate((viewDist - TESR_TerrainVariationData.z) / max(TESR_TerrainVariationData.w, 1.0f)));
    float tapWeights[7] = { 1, 1, 1, 1, 1, 1, 1 };
    float3 baseColor = LinColor(blendDiffuseMapsVaried(landVertexColor, offsetUV, odx, ody, texLayers, BaseMap, wts, stochastic, tapWeights));
#else
    float3 baseColor = LinColor(blendDiffuseMaps(landVertexColor, offsetUV, odx, ody, texLayers, BaseMap, wts));	// UNOFFICIAL linear lighting: decoded
#endif
#if TERRAIN_MACRO
    baseColor = MacroVary(baseColor, camPos);	// 2026-11-04 macro variation (Includes/TerrainMacro.hlsl)
#endif
#if TERRAIN_BLEND_ACTIVE
    // The land texture's height shapes the blend's edge (HeightBlend); the dither, if used, cuts on the shaped edge.
    [branch] if (terrainKeep >= 0.0f && terrainKeep < 1.0f && TESR_TerrainBlendShape.y != 0.0f)   // < 0: HeightEdge 1
        terrainKeep = terrainBlendEdge(terrainKeep, terrainBlendHeight(offsetUV, gradX, gradY, texLayers, BaseMap, wts, hStat));
    if (TESR_TerrainBlendData.w <= 0.0f && TESR_TerrainBlendExtra.w <= 0.5f && terrainKeep >= 0.0f && terrainKeep < 1.0f)
        clip(terrainKeep - terrainBlendNoise(screenPos));
#endif
#if TERRAIN_VARIATION
    // NormalMaps off: the normal maps take the single tap (tap1Weight 1 takes the plain path in blendNormalMapsVaried).
    StochasticOffsets stochasticNormals = stochastic;
    stochasticNormals.tap1Weight = lerp(1.0f, stochastic.tap1Weight, saturate(TESR_TerrainVariationExtra.x));
    float3 combinedNormal = blendNormalMapsVaried(offsetUV, odx, ody, texLayers, NormalMap, wts, specA, stochasticNormals, tapWeights, glossV, specPow);
#else
    float3 combinedNormal = blendNormalMaps(offsetUV, odx, ody, texLayers, NormalMap, wts, specA, glossV, specPow);
#endif

    float3 sunTS = mul(tbnMat, SunDir.xyz);
    float sunMul = 1.0;
    [branch] if (parallax) sunMul = getParallaxShadowMultipler(viewDist, offsetUV, gradX, gradY, sunTS, texLayers, blendW, hStat, BaseMap);

    // Forward sun shadows. Folded into sunMul, which getSunLighting
    // applies to the sun colour only -- ambient is added afterwards and stays untouched.
    #if FORWARD_SHADOWS	// NVR sun shadows in the object pass
    #if SHADOW_CLOUD_FROM_VALUE
    NvrCloudSun = SHADOW_VS_PRESENT(pin.texc.w) ? pin.texc.z : 1.0f;	// UNOFFICIAL 2026-11-04 ForwardCloudShadows: the vertex shader's lookup
    #endif
    sunMul *= GetSunShadow(camPos, geoN);
    #endif

    #ifdef TERRAIN_WETNESS	// UNOFFICIAL 2026-11-04 Wetness Effects: the albedo darkens before any light sees it (land is porous: full darkening)
        float3 wetWorldNormal = normalize(mul(combinedNormal, tbnMat));	// tangent space -> the land piece's (world-aligned) space
        // The surface's height where it is seen (the parallax's coordinates): the layers' height maps (their diffuse alpha, where
        // LandHeight says they have one; flat 0.5 where not), blended by the same weights as the colour. The textures were just read
        // there, so these reads hit the cache.
        float surfaceHeight = 0.0f;
        float heightWeight = 0.0f;
        [unroll] for (int wh = 0; wh < texLayers; wh++) {
            surfaceHeight += wts[wh] * (hStat[wh] > 0.0f ? tex2Dgrad(BaseMap[wh], offsetUV, odx, ody).a : 0.5f);
            heightWeight += wts[wh];
        }
        surfaceHeight = heightWeight > 0.0f ? surfaceHeight / heightWeight : 0.5f;
        Wetness wet = GetWetness(camPos, geoN, wetWorldNormal, surfaceHeight);
        #ifdef PUDDLES_COMPILED	// 2026-11-08 Puddles: standing water hides the normal map (the sun, lamps and ambient see a flat surface)
            combinedNormal = normalize(lerp(combinedNormal, float3(0.0f, 0.0f, 1.0f), wet.flatten));
        #endif
        baseColor = WetAlbedo(baseColor, wet.darkening, 1.0f);
    #endif
    float3 litCol = getSunLighting(sunTS, LinColor(SunColor.rgb), toEye, combinedNormal, LinColor(AmbientColor.rgb), baseColor, glossV, specPow, 1.0, sunMul, geoN);
    #ifdef TERRAIN_WETNESS	// the water film: the sun's highlight on it and its reflection of the sky, over the land
        [branch] if (wet.gloss > 0.001f) {
            float3 wetV = -normalize(camPos);
            float3 wetSun = LinColor(SunColor.rgb) * LinScale(TESR_TerrainData.z) * sunMul;	// as getSunLighting takes the sun
            litCol += WetSpecular(wet.normal, wetV, TESR_WetnessSun.xyz, wetSun, wet.roughness) * smoothstep(0.0f, 0.2f, wet.gloss);
            float3 wetR = reflect(-wetV, wet.normal);
            float wetHorizon = saturate(1.0f + 1.2f * dot(wetR, geoN));
            litCol += WetSkyReflection(wetR) * WetReflectance(saturate(dot(wet.normal, wetV)), wet.roughness, wet.gloss) * wetHorizon * wetHorizon;
        }
    #endif

    #if defined(NUM_PT_LIGHTS)	// point lights
        #if TERRAIN_LAMP_RT
            float3 tlVis, tlP0, tlP1, tlP2, tlOk;
            TerrainLampEntries(camPos, tlVis, tlP0, tlP1, tlP2, tlOk);
            float2 tlDebug = 0.0f;   // (2026-11-05 ExteriorDebug 3) the lamps' attenuation blocked / lit
        #endif
        [loop] for (int k = 0; k < PointLightCount; k++) {
            float3 lampDir = PointLightPosition[k].xyz - pin.localPos.xyz;
            float atten = vanillaAtt(lampDir, PointLightPosition[k].w);

            [branch]
            if (atten > 0.001) {
                #if TERRAIN_LAMP_RT
                    float tlShadow = TerrainLampShadow(camPos + lampDir, tlVis, tlP0, tlP1, tlP2, tlOk);
                    tlDebug += atten * float2(1.0f - tlShadow, tlShadow);
                #else
                    float tlShadow = 1.0f;
                #endif
                lampDir = mul(tbnMat, lampDir);
                // UNOFFICIAL 2026-10-21 (Includes/LampFalloff.hlsl): att as before, the objects' decode (MatchEverywhere) or the shape.
                litCol += tlShadow * getPointLightLighting(lampDir, LampAttOther(atten), LinColor(PointLightColor[k].rgb), toEye, combinedNormal, baseColor, glossV, specPow);
            }
        }
        #if TERRAIN_LAMP_RT
            [branch] if (NVR_TerrainLampPack.z > 0.5f) litCol = litCol * 0.25f + float3(tlDebug.x, tlDebug.y, 0.0f) * 2.0f;
        #endif
    #endif

    // Per pixel fog.
    float4 fogTerm;
    float3 fogP = pin.clipPos.xyz;
    #ifdef REVERSED_DEPTH	// NVR's reversed depth buffer
        fogP.z = pin.clipPos.w - pin.clipPos.z;
    #endif
    float fogAmt = 1 - saturate((FogParam.x - length(fogP)) / FogParam.y);
    fogTerm.rgb = FogColor.rgb;
    fogTerm.a = pow(fogAmt, FogParam.z);

    float3 outCol = LinEncode(litCol);	// UNOFFICIAL linear lighting: back to the stored (gamma) space, before the fog
    outCol = lerp(outCol, fogTerm.rgb, fogTerm.a); // Apply fog.

#if TERRAIN_BLEND_ACTIVE
    // Debug views (DebugView): 1 the blend band in magenta (full magenta: the land fully gone), 2 how much land is kept (white:
    // all), 3 how deep the object is below the surface (black at the surface, white at four times BlendDistance; dark blue:
    // nothing behind the land), 4 how deep the fade reaches (white: the full BlendDistance, grey: a shallow object's seam).
    float blendDebug = TESR_TerrainBlendData.w;
    [branch] if (blendDebug > 2.5f && TESR_TerrainBlendData.x > 0.0f) {
        if (blendGap.covered < 0.0f) outCol = float3(0.0f, 0.0f, 0.3f);
        else if (blendDebug < 3.5f) outCol = saturate(blendGap.covered * TESR_TerrainBlendData.y * 0.25f);
        else outCol = saturate(terrainBlendDepthRange(blendGap, 1.0f - blendNormal.z) * TESR_TerrainBlendData.y);
    }
    else if (blendDebug > 1.5f && terrainKeep >= 0.0f) outCol = terrainKeep;
    else if (blendDebug > 0.5f && terrainKeep >= 0.0f) outCol = lerp(float3(1.0f, 0.0f, 1.0f), outCol, terrainKeep);
#endif

    po.finalCol.a = 1;
#if TERRAIN_BLEND_ACTIVE
    if (TESR_TerrainBlendExtra.w > 0.5f && TESR_TerrainBlendData.w <= 0.0f && terrainKeep >= 0.0f) po.finalCol.a = terrainKeep;
#endif
#if DISPLACE_DEPTH_IN
    // Never taken (the fog's power is never below zero), but it keeps TESR_DisplaceDepth in this shader's samplers, which is what
    // lets the tessellation stage read it (DXVK checks the sampler against this shader's declarations).
    [branch] if (FogParam.z < -1.0e30f) outCol += tex2Dlod(TESR_DisplaceDepth, float4(0.5f, 0.5f, 0.0f, 0.0f)).x;
#endif
    #ifdef TERRAIN_WETNESS
        [branch] if (WETNESS_DEBUG_COVER) outCol = WetnessCoverDebug(camPos, geoN);	// [Shaders.WetnessEffects.Debug] ShowCover
        #ifdef PUDDLES_COMPILED
            [branch] if (PuddleDebugColor.x >= 0.0f) outCol = PuddleDebugColor;	// 2026-11-08 [Shaders.Puddles.Debug] DebugView
        #endif
    #endif
    po.finalCol.rgb = outCol;
    po.finalCol.rgb = LinearSceneOut(po.finalCol.rgb);	// UNOFFICIAL 2026-10-28 LinearScene (includes/LinearLight.hlsl)
#if CAPTURED_NORMALS_ON
    // UNOFFICIAL 2026-10-31 AlbedoBuffer (Includes/CapturedNormals.hlsl): the land's blended linear colour; with terrain blending's fade (alpha 1, a
    // replace) it overwrites an object's albedo in the fade strip, as the normal does
    po.capturedPad1 = CapturedAlbedoOut(baseColor);
    po.capturedPad2 = CapturedMaterialOutClass(4.0f, -1.0f);	// UNOFFICIAL 2026-10-29 MaterialBuffer: class 4, land (0 without it)
    // UNOFFICIAL 2026-10-28 CapturedNormals: the land's interpolated vertex normal (the land is not rotated: object space is world space;
    // with tessellation the normal its evaluation shader passes on), before the layers' normal maps. Alpha 1 also with terrain blending's
    // Smooth fade (the land drawn blended over objects): the land's normal replaces what is there, with its depth.
    po.capturedNormal = CapturedNormalOut(pin.nrmO.xyz, camPos);
#endif

    return po;
};

#endif	// PS
