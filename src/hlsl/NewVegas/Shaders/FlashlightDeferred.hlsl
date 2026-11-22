// UNOFFICIAL 2026-11-04 deferred flashlight (core/FlashlightDeferred.h; [Shaders.Flashlight.Deferred]; user: "what I really want is an actual
// working real spotlight for the flashlight"). The player's flashlight as a real spot light: ONE full-screen pass after the main list's
// G-buffer is resolved and before LinearScene encodes the frame (as DeferredLamps), lighting each pixel's own surface -- its linear base
// colour (TESR_AlbedoBuffer), roughness and metal (TESR_MaterialBuffer) and smooth normal (TESR_CapturedNormalsBuffer) -- with the object
// shaders' own lamp maths (Object.hlsl getPointLightLightingAtt: the PBR diffuse and highlight, the lamps' falloff shape and light scale),
// times the cone (smooth from its outer edge in to half its angle), the flashlight's shadow map (the spot map ShadowManager draws from the
// light, PCF) and the cookie. Added to the frame: ONE / ONE in linear light, or enc(dec(frame) + light) from a copy of it otherwise.
// What is not in the G-buffer (grass, water, decals, alpha-blended things, the first-person view) is not lit by it.

#ifndef LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING_ON 0
#endif
#if LINEAR_LIGHTING_ON
    #define LINEAR_LIGHTING
    #if defined(LIN_PREDECODE_ON) && LIN_PREDECODE_ON
        #define LIN_PREDECODED
    #endif
#endif
#define SPECULAR

// UNOFFICIAL 2026-11-21: per-pixel dielectric F0 from FDSunVisCode in TESR_CapturedNormalsBuffer.a.
#if CAPTURED_NORMALS_ON >= 4
static float FLSpecF0 = 0.04f;
#define PBR_SPEC_F0(baseColor, metal) lerp(FLSpecF0.xxx, (baseColor), (metal))
#define PBR_DIELECTRIC_F0 min(FLSpecF0, 0.04f)
#endif

#include "includes/Helpers.hlsl"
#include "includes/Object.hlsl"
#include "includes/LampFalloff.hlsl"
#include "includes/Shadow.hlsl"
// UNOFFICIAL 2026-11-14 DeferredSun: the lighting-free records' code in COLOR1.a decoded to the keep (as DeferredLamps.hlsl DL_KEEP_OF)
#if defined(FD_LIGHT_FREE) && FD_LIGHT_FREE
    #include "includes/DeferredPixel.hlsl"
    #define FL_KEEP_OF(a) FDAddedLightKeep(a)
#elif defined(CAPTURED_NORMALS_ON) && CAPTURED_NORMALS_ON >= 4
    #include "includes/DeferredPixel.hlsl"
    #define FL_KEEP_OF(a) FDAddedLightKeep(a)	// GBufferV2: 1 + (1 - fog), the "sun kept" records' 5 + (1 - fog) (2026-11-14), a forward record's 1
#else
    #define FL_KEEP_OF(a) (a)
#endif

// The pass's own constants (c0-c19: nothing the includes declare lives there; FlashlightDeferred::Apply).
float4 FL_Depth    : register(c0);	// x, y the projection's _33, _43 (depth = x + y / view z), z the share and w the units the surface is pulled toward the camera
float4 FL_View     : register(c1);	// xy 1 / viewport size, zw the viewport's top-left corner (pixels)
float4 FL_Right    : register(c2);	// xyz the camera's right axis, w tan(half fov x)
float4 FL_Up       : register(c3);	// xyz up, w tan(half fov y)
float4 FL_Fwd      : register(c4);	// xyz forward, w 1 = the frame is linear now (else the copy path)
float4 FL_Pos      : register(c5);	// xyz the light, camera-relative, w its reach (game units)
float4 FL_Dir      : register(c6);	// xyz the beam's axis (unit), w cos of the cone's outer edge
float4 FL_Color    : register(c7);	// rgb the light (linear, times dimmer and Intensity), w cos of the full-strength edge
float4 FL_Gamma    : register(c8);	// z the gamma (copy path), w 1 / the gamma; UNOFFICIAL 2026-11-11 x HotspotLimit (0 off), y 1 = SoftEdges
float4 FL_Shadow   : register(c9);	// x 1 = the shadow map is there, y its texel size (0-1), z the depth bias (share of the reach), w PCF radius (texels)
float4 FL_Look     : register(c10);	// x cookie strength (0 none), y DebugView (0 off, 1 the light alone, 2 the shadow, 3 the normal), z Falloff (0: the lamps' 1 - d^2 / r^2), w NearFade (units, 0 off; UNOFFICIAL 2026-11-11)
row_major float4x4 FL_LightViewProj : register(c11);	// c11-c14: camera-relative world -> the shadow map's clip space
#if CAPTURED_NORMALS_ON >= 4
float4 FL_Model    : register(c15);	// UNOFFICIAL 2026-11-16 fd-batchA item 6: x 1 = the record's specular bit picks the model ([Shaders.Flashlight.Deferred] SpecularBit)
#endif

sampler2D FL_Frame    : register(s0);	// the frame's copy (only when it is not linear)
sampler2D FL_Albedo   : register(s1);	// TESR_AlbedoBuffer: rgb linear base colour, a 1 - the draw's fog
sampler2D FL_Material : register(s4);	// TESR_MaterialBuffer: r class + 0.49 metal, g radius (or oct.x on the deferred records), b roughness (-1 none), a 1 (1.01-1.99: a deferred record)
sampler2D FL_Normals  : register(s5);	// TESR_CapturedNormalsBuffer: rg the smooth normal (octahedral), b the camera distance (0 nothing), a 1 (actors 2)
sampler2D FL_ShadowMap : register(s6);	// the flashlight's spot shadow map: r the distance to the light over its reach (1 nothing)
sampler2D FL_Cookie   : register(s7);	// the flashlight's cookie (Effects\Flashlight.png)

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };

// One bilinear PCF tap: the four texels round uv compared, weighted by uv's place between them (a smooth edge, not a texel step).
float FLShadowTap(float2 uv, float distShare, float bias) {
    const float size = 1.0f / FL_Shadow.y;
    const float2 t = uv * size - 0.5f;
    const float2 f = frac(t);
    const float2 base = (floor(t) + 0.5f) * FL_Shadow.y;
    const float s00 = tex2Dlod(FL_ShadowMap, float4(base, 0.0f, 0.0f)).r + bias > distShare ? 1.0f : 0.0f;
    const float s10 = tex2Dlod(FL_ShadowMap, float4(base + float2(FL_Shadow.y, 0.0f), 0.0f, 0.0f)).r + bias > distShare ? 1.0f : 0.0f;
    const float s01 = tex2Dlod(FL_ShadowMap, float4(base + float2(0.0f, FL_Shadow.y), 0.0f, 0.0f)).r + bias > distShare ? 1.0f : 0.0f;
    const float s11 = tex2Dlod(FL_ShadowMap, float4(base + FL_Shadow.yy, 0.0f, 0.0f)).r + bias > distShare ? 1.0f : 0.0f;
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// The spot map's lit share at a camera-relative point: PCF over a disc of FL_Shadow.w texels (2026-11-04, user: "softer, and higher
// quality", "they should be using PCF shadows"; was 3 x 3 point taps): 16 bilinear PCF taps on a Vogel spiral (even cover of the disc,
// no noise: no grain without TAA). 1 outside the map.
float FLShadow(float3 position, float distShare, out float2 mapUV) {
    const float4 clip = mul(float4(position, 1.0f), FL_LightViewProj);
    mapUV = clip.xy / max(clip.w, 1.0e-4f) * float2(0.5f, -0.5f) + 0.5f;
    [branch] if (FL_Shadow.x < 0.5f || clip.w <= 0.0f || any(mapUV < 0.0f) || any(mapUV > 1.0f)) return 1.0f;
    const float radius = FL_Shadow.y * FL_Shadow.w;
    const float bias = FL_Shadow.z * (1.0f + 4.0f * distShare);
    float2 dir = float2(1.0f, 0.0f);
    const float2x2 golden = float2x2(-0.7373688f, -0.6754904f, 0.6754904f, -0.7373688f);	// a turn by the golden angle (2.39996 rad)
    float lit = 0.0f;
    [unroll] for (int i = 0; i < 16; i++) {
        lit += FLShadowTap(mapUV + dir * (radius * sqrt((i + 0.5f) / 16.0f)), distShare, bias);
        dir = mul(golden, dir);
    }
    return lit / 16.0f;
}

float4 main(PS_IN IN, out float depth : DEPTH) : COLOR0 {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 stored = tex2Dlod(FL_Normals, uv4);
    depth = 0.0f;
    clip(stored.b - 1.0e-3f);	// nothing the G-buffer holds here
    // 1 - fog: the share of added light the draw's fog lets through. 2026-11-05 (user: the beam "pokes through" the terrain, a lit patch with
    // a jagged edge): not for the land (MaterialBuffer class 4), which writes 1 there whatever its fog, but in terrain blending's Smooth fade
    // is drawn alpha-blended, so the albedo buffer's alpha came out 0 on stretches of it while its colour and normal were right (flashlight
    // DebugView checks): the beam skipped that land and lit only the rest. The land takes the full light.
#if CAPTURED_NORMALS_ON >= 4
    // UNOFFICIAL 2026-11-14 GBufferV2 (Includes/CapturedNormals.hlsl): the land is 4 + 16 (its lamps have a highlight), and a deferred
    // record's albedo alpha is 1 + (1 - fog) (forward records 1, as before)
    const float landClass = floor(tex2Dlod(FL_Material, uv4).r + 0.01f) - 16.0f;
    const float keepStored = tex2Dlod(FL_Albedo, uv4).a;
    const float keep = abs(landClass - 4.0f) < 0.5f ? 1.0f : FL_KEEP_OF(keepStored);
#else
    const float landClass = floor(tex2Dlod(FL_Material, uv4).r + 0.01f);
    const float keep = abs(landClass - 4.0f) < 0.5f ? 1.0f : FL_KEEP_OF(tex2Dlod(FL_Albedo, uv4).a);
#endif
    clip(keep - 1.0e-4f);

    // the pixel's camera-relative position along its view ray (DeferredLamps.hlsl DLPixel). (2026-11-15 fd-lightpass) at the pixel's own
    // centre, without + 0.5 on VPOS (Includes/DeferredDepth.hlsl DDRay: the D3D9 pixel at VPOS i is at ndc 2 (i - X) / W - 1; FD-SUN-FSR-RESULTS.md)
    const float2 local = (IN.vpos - FL_View.zw) * FL_View.xy;
    const float3 ray = normalize(FL_Fwd.xyz + FL_Right.xyz * ((local.x * 2.0f - 1.0f) * FL_Right.w) + FL_Up.xyz * ((1.0f - local.y * 2.0f) * FL_Up.w));
    const float3 position = ray * stored.b;
    // 2026-11-04 (user: hair see-through in the beam): the G-buffer holds only what its writers drew; a draw that does not write it (hair,
    // and anything else drawn over a writer in the span) left the surface BEHIND it there, and that one was lit through it. The pass is
    // depth-tested at the G-buffer's surface (a little nearer, for its own precision; the test per sample under MSAA): where something
    // nearer was drawn, it fails and the pixel is left as it is.
    const float viewZ = max(dot(position, FL_Fwd.xyz) * (1.0f - FL_Depth.z) - FL_Depth.w, 1.0f);
    depth = saturate(FL_Depth.x + FL_Depth.y / viewZ);

    // the cone and the reach
    const float3 toLight = FL_Pos.xyz - position;
    const float dist = length(toLight);
    clip(FL_Pos.w - dist);
    const float coneCos = dot(-toLight / max(dist, 1.0e-3f), FL_Dir.xyz);
    clip(coneCos - FL_Dir.w);
    const float cone = smoothstep(FL_Dir.w, FL_Color.w, coneCos);
    // 2026-11-04 (user: "the reach of the flashlight goes quite far"): Falloff > 0, a windowed inverse square, (1 - d^2/r^2)^2 / (1 + F d^2/r^2):
    // bright near, a quarter of it halfway out at F 4, nothing at the reach; 0 the lamps' falloff
    const float reachShare2 = dot(toLight, toLight) / (FL_Pos.w * FL_Pos.w);
    const float window = saturate(1.0f - reachShare2);
    const float falloff = FL_Look.z > 0.0f ? window * window / (1.0f + FL_Look.z * reachShare2) : vanillaAtt(toLight, FL_Pos.w);
    // UNOFFICIAL 2026-11-11 NearFade (the old flashlight's formula): ramps the pool down point blank so a wall a foot away does not clip
    // to white; 0 = 1, today's light
    const float nearFade = FL_Look.w > 0.0f ? smoothstep(0.0f, FL_Look.w, dist) : 1.0f;
    const float att = falloff * cone * cone * nearFade;

    float2 mapUV;
    const float shadow = FLShadow(position, dist / FL_Pos.w, mapUV);
    // UNOFFICIAL 2026-11-11 (BUG FIX, a tester: "Cookie just brightens and darkens the light ... at 1 it is invisible"). Two faults: stage 7
    // held no texture (FlashlightDeferred::CookieTexture loads it now; no file = FL_Look.x 0), and the cookie was read at the shadow map's
    // uv, which is only right while the map is drawn (Shadows off, or a frame without it: an old or empty matrix, the whole beam one texel).
    // Its uv from the cone itself now, the same frame the map has (ShadowManager::RenderShadowSpotlight: LookAtRH with Z up, a square FOV of
    // twice the cone's half-angle), so the pattern's rim (r = 1) is the cone's outer edge and it follows Angle with or without shadows.
    float cookie = 1.0f;
    [branch] if (FL_Look.x > 0.0f) {
        float3 side = cross(float3(0.0f, 0.0f, 1.0f), -FL_Dir.xyz);
        side /= max(length(side), 1.0e-4f);
        const float3 upAxis = cross(-FL_Dir.xyz, side);
        const float invTan = FL_Dir.w / sqrt(max(1.0f - FL_Dir.w * FL_Dir.w, 1.0e-6f));   // 1 / tan(half-angle)
        const float2 cookieUV = float2(dot(-toLight, side), -dot(-toLight, upAxis)) * (invTan / max(dot(-toLight, FL_Dir.xyz), 1.0e-3f)) * 0.5f + 0.5f;
        cookie = lerp(1.0f, tex2Dlod(FL_Cookie, float4(cookieUV, 0.0f, 0.0f)).r * 2.0f, saturate(FL_Look.x));
    }

    const float3 N = ShadowDecodeNormal(stored.rg);
    const float4 m = tex2Dlod(FL_Material, uv4);
    // (2026-11-12 ptbump) b in -3 .. -2: a record coded "the game draws no highlight on this mesh" for the path tracer (Includes/
    // CapturedNormals.hlsl CapturedNoHighlight): the roughness this pass always lit it with is -2 - b. The land's b (-8 .. -6 with its
    // bump recorded, else -1): no roughness, as before.
    // (2026-11-16 fd-sunvis) the deferred passes' one decode (Includes/Object.hlsl GBufferRoughness): the foliage records' 2 + sun weight fully
    // rough as before; none (-1) and the land's old -8 .. -6 fully rough (0.7 here until then)
    const float roughness = GBufferRoughness(m.b);
    const float metal = saturate((m.r - floor(m.r)) / 0.49f);
    SetDiffuseEnergyKeep(N, -ray, roughness);
    const float3 albedo = tex2Dlod(FL_Albedo, uv4).rgb;
#if CAPTURED_NORMALS_ON >= 4
    FLSpecF0 = abs(FDModel(m.r) - 4.0f) < 0.5f ? 0.04f : FDSpecF0Decode(stored.a);
#endif
    float3 lit = getPointLightLightingAtt(toLight, att, FL_Color.rgb, -ray, N, albedo, roughness, metal);
#if CAPTURED_NORMALS_ON >= 4
    // UNOFFICIAL 2026-11-16 fd-batchA item 6 (docs/next/FD-BATCH-A.md): the record's specular bit (COLOR2.r + 16, the SPECULAR variants) picks the
    // model as the forward shaders' objLampTerm did and DeferredLamps.hlsl does for the lamps: PBR for the shiny records (above), PBRDiffuse for
    // the others (the plain and light-only records: meshes the game draws without a highlight, whose b is the DXT1 alpha's ~0.04, so the beam
    // left a sharp GGX hot spot on matte walls). FL_Model.x 0: PBR for every pixel, as before.
    [branch] if (FL_Model.x > 0.5f && floor(m.r + 0.01f) < 15.5f)
        lit = LampAtt(att) * MICRO_SHADOW(N, toLight) PBRDiffuse(metal, roughness, lerp(luma(albedo), albedo, TESR_PBRExtraData.x), N, -ray, toLight, FL_Color.rgb * LIN_SCALE_LIGHT);
#endif
    // UNOFFICIAL 2026-11-11 SoftEdges (the old flashlight's wrap: 0.75 x ((N.L + 1) / 2)^2 for the clamped N.L, the same mean light over
    // all normals, less speckle where the normals scatter): the diffuse's plain Lambert part (PBRDiffuse's: (1 - metal) x colour x N.L x
    // light) swapped for the wrap; the highlight as it is. Off: nothing changes.
    [branch] if (FL_Gamma.y > 0.5f) {
        const float ndl = dot(N, toLight / max(dist, 1.0e-3f));
        const float wrap = saturate(ndl * 0.5f + 0.5f);
        const float3 graded = lerp(luma(albedo), albedo, TESR_PBRExtraData.x);
        lit = max(lit + LampAtt(att) * (1.0f - metal) * graded * FL_Color.rgb * LIN_SCALE_LIGHT * (0.75f * wrap * wrap - saturate(ndl)), 0.0f);
    }
    float3 light = lit * (shadow * cookie * keep);
    // UNOFFICIAL 2026-11-11 HotspotLimit (the old flashlight's Reinhard-style roll-off, s' = s x L / max(L, s), L = 1 / HotspotLimit): the
    // pool's centre stops short of clipping without dimming the falloff round it; 0 off. s is the beam's own shape, falloff x cone^2 (1 on
    // its axis at the light; no NearFade, colour, Dimmer or surface), NOT the light added (whose luma stayed under 0.5: no roll-off at all).
    // The path tracer applies the same ratio s' / s from the same shape (RtPathTrace.comp FlashShape): the same pool from the same number.
    [branch] if (FL_Gamma.x > 0.0f) {
        const float limit = 1.0f / FL_Gamma.x;
        light *= limit / max(limit, falloff * cone * cone);
    }

    [branch] if (FL_Look.y > 0.5f) {	// the debug views, written over the frame (blend off: FlashlightDeferred::Apply)
        const float3 dbg = FL_Look.y < 1.5f ? light : FL_Look.y < 2.5f ? shadow.xxx : N * 0.5f + 0.5f;
        return float4(FL_Fwd.w > 0.5f ? dbg : pow(max(dbg, 0.0f), FL_Gamma.w), 1.0f);
    }
    [branch] if (FL_Fwd.w > 0.5f) return float4(light, 0.0f);	// linear frame: ONE / ONE add
    // The frame in the stored curve: enc(dec(v) + L). 2026-11-04 (user: leaves see-through and terrain blending lost inside the cone, with
    // an edge where it stops): ADDED as the difference it makes (ONE / ONE), not written over the pixel. Written, every sample of a
    // multisampled pixel took the resolved copy's one value, so alpha-to-coverage edges and terrain blending's per-sample blend were
    // flattened even where the light was nothing; added, each sample keeps its own, and where the light fades out nothing changes.
    const float3 frame = max(tex2Dlod(FL_Frame, uv4).rgb, 0.0f);
    return float4(max(pow(pow(frame, FL_Gamma.z) + light, FL_Gamma.w) - frame, 0.0f), 0.0f);
}
