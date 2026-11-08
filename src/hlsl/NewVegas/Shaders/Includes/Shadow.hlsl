// Forward sun-shadow lookup for GAME shaders (objects, parallax, terrain).
//
// All four cascades live in one 2x2 atlas (TESR_ShadowAtlas).
//
// Mirrors Effects/SunShadows.fx.hlsl. Cascade selection, bias and bleed-reduction constants
// must be changed in both.
//
// TESR_ShadowCameraToLightTransform* consume CAMERA-RELATIVE world space (no
// TESR_CameraPosition added). Build it with GetShadowWorldPos() in the VS.
//
//   VS:  OUT.shadowWorldPos = GetShadowWorldPos(OUT.sPosition);
//   PS:  sunLight *= GetSunShadow(IN.shadowWorldPos, worldNormal);

// ---------------------------------------------------------------------------
// Bound by name via ShaderRecord::CreateCT; registers only decide where the values live.
//
// EVERY constant must stay explicitly pinned. Unpinned, the compiler packs them into gaps
// between the host's declared constants -- registers the engine still writes for the vanilla
// shader -- and overwrites them mid-frame.
//
// c100-c133 is clear in both profiles: heaviest host is TerrainTemplate's PS at c93 and the
// skinned VS Bones[54] ending at c97; ps_3_0 allows 224 constants, vs_3_0 256.
//
// The VS matrices are relocatable: GRASS23x00*.vso indexes InstanceData from c20 well past
// c100.
//
// UNOFFICIAL (P66): not clear in SKINNED vertex shaders after all. The game's bone upload reaches past Bones[54]'s c97
// and overwrote c100-c107, so every actor rebuilt its world position from bone data and read the sun shadow at the
// wrong place (all at the player's position before P65, a pattern turning with the camera after). Skinned vertex
// shaders define SHADOW_INVPROJ_REG/SHADOW_INVVIEW_REG c240/c244 (GDECALS: c180/c184) before including this file.
// ---------------------------------------------------------------------------
#ifndef SHADOW_INVPROJ_REG
    #define SHADOW_INVPROJ_REG c100
#endif
#ifndef SHADOW_INVVIEW_REG
    #define SHADOW_INVVIEW_REG c104
#endif

row_major float4x4 TESR_InvProjectionTransform : register(SHADOW_INVPROJ_REG);
row_major float4x4 TESR_InvViewTransform       : register(SHADOW_INVVIEW_REG);

row_major float4x4 TESR_ShadowCameraToLightTransformNear   : register(c108);
row_major float4x4 TESR_ShadowCameraToLightTransformMiddle : register(c112);
row_major float4x4 TESR_ShadowCameraToLightTransformFar    : register(c116);
row_major float4x4 TESR_ShadowCameraToLightTransformLod    : register(c120);

float4 TESR_ShadowNearCenter   : register(c124); // xyz: centre (camera-relative world), w: radius
float4 TESR_ShadowMiddleCenter : register(c125);
float4 TESR_ShadowFarCenter    : register(c126);
float4 TESR_ShadowLodCenter    : register(c127);

float4 TESR_ShadowData        : register(c128); // y: darkness (z is INTERIOR cube texel size only)
float4 TESR_ShadowFormatData  : register(c129); // x: mode (0 VSM, 1 EVSM2, 2 EVSM4), y: format bits
float4 TESR_ShadowFade        : register(c130); // x: sunrise/sunset fade, y: shadow maps active
float4 TESR_SmoothedSunDir    : register(c131);
float4 TESR_ShadowBlur        : register(c132); // x: 1 / atlas resolution, y: lod cascade updated, z: 1 = SmoothFilter (PCF), w: CharacterShadowReach (world units)
float4 TESR_ShadowForwardData : register(c133); // x: 1 when the forward path is SUPPRESSED, w: HandoverBlend (world units)

// UNOFFICIAL 2026-10-21 HandoverBlend ([Shaders.ShadowsExteriors.ShadowMaps], TESR_ShadowForwardData.w, world units, live): how far
// inside each cascade's edge the cross-fade into the next cascade starts. 0 (the old rule) = a tenth of the radius, which at a high
// CascadeLambda is a few centimetres: a camera zoom crossed it in one frame and the shadow popped. Never less than that tenth, never
// more than the radius. The same rule in SunShadows.fx.hlsl (the deferred path).
float4 ShadowCascadeBands(float4 radii) {
    return max(0.1f * radii, min(TESR_ShadowForwardData.w, radii));
}

// Object templates top out at s7. Override BEFORE including for wider sampler arrays --
// TerrainTemplate's NormalMap[7] spans s7-s13.
#ifndef SHADOW_ATLAS_SAMPLER_REG
    #define SHADOW_ATLAS_SAMPLER_REG s9
#endif

// MUST stay on ONE line, closing brace included. ShaderTextureValue::GetSamplerStateString
// finds "register ( sN )" and reads only to the end of that line. Split, it silently falls
// back to TextureRecord's POINT/WRAP defaults. Game shaders only; Effects parse their own.
sampler2D TESR_ShadowAtlas : register(SHADOW_ATLAS_SAMPLER_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// UNOFFICIAL sun shadow cross-fade ([Shaders.ShadowsExteriors.SunSmoothing] CrossFade, with ForwardShadows): at a
// sun step the DLL keeps the atlas from before the step and fades from it to the new one (GetOldSunShadow), so no
// shadow map turns while it is on screen. Compiled in only when on (SUN_CROSSFADE from ShaderRecord::LoadShader):
// otherwise these shaders are exactly what they were. c146-c166 sit above SkyAmbient's TESR_SkyIrradiance[9]
// (c137-c145). The kept atlas takes the sampler after the live one: s10 by default, TerrainTemplate sets s15.
#ifndef SUN_CROSSFADE
    #define SUN_CROSSFADE 0
#endif
#if SUN_CROSSFADE
row_major float4x4 TESR_ShadowOldCameraToLightTransformNear   : register(c146);
row_major float4x4 TESR_ShadowOldCameraToLightTransformMiddle : register(c150);
row_major float4x4 TESR_ShadowOldCameraToLightTransformFar    : register(c154);
row_major float4x4 TESR_ShadowOldCameraToLightTransformLod    : register(c158);
float4 TESR_ShadowOldNearCenter   : register(c162);
float4 TESR_ShadowOldMiddleCenter : register(c163);
float4 TESR_ShadowOldFarCenter    : register(c164);
float4 TESR_ShadowOldLodCenter    : register(c165);
float4 TESR_ShadowCrossFade       : register(c166); // x: share of the old shadows (1 at the step, 0 when done)
#ifndef SHADOW_OLD_ATLAS_SAMPLER_REG
    #define SHADOW_OLD_ATLAS_SAMPLER_REG s10
#endif
// One line, like TESR_ShadowAtlas above.
sampler2D TESR_ShadowAtlasOld : register(SHADOW_OLD_ATLAS_SAMPLER_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#endif

// Atlas encoding, compile-time: ps_3_0 would flatten a runtime branch across all three
// variants. Must match [_Shaders.ShadowsExteriors.ShadowMaps] Mode.
//   0 = VSM, 1 = EVSM2, 2 = EVSM4 (toml default)
#ifndef SHADOW_FIXED_MODE
    #define SHADOW_FIXED_MODE 3   // UNOFFICIAL 2026-10-27: PCF always (ShaderRecord defines 3 for every shader)
#endif

static const float SHADOW_FORMAT = TESR_ShadowFormatData.y;

// Normal-offset bias in SHADOW MAP TEXELS, not world units -- cascade texels span two orders
// of magnitude (~0.15 to ~3 world units at Distance 6000 / Lambda 0.95 / Resolution 2048).
// Must also clear the prefilter kernel: ShadowMapBlur.pso reaches +/-3.23 texels on Near
// (Blur9) and +/-1.33 elsewhere (Blur5).
#ifndef SHADOW_NORMAL_BIAS_TEXELS
    #define SHADOW_NORMAL_BIAS_TEXELS 0.0f
#endif

// Widens the VSM/EVSM minimum variance at grazing incidence, where one texel spans a long run
// of receiver depth. 0 gives a constant bias.
#ifndef SHADOW_SLOPE_BIAS
    #define SHADOW_SLOPE_BIAS 1.0f
#endif

// Taps per cascade, on top of the Gaussian prefilter. 1 matches deferred; 4 costs 12 extra
// fetches per fragment. Above 1, average the MOMENTS and run Chebyshev once -- the bound is
// not linear in the moments.
#ifndef SHADOW_FILTER_TAPS
    #define SHADOW_FILTER_TAPS 1
#endif

// Tap spacing in atlas texels.
#ifndef SHADOW_FILTER_SPREAD
    #define SHADOW_FILTER_SPREAD 1.0f
#endif

// VS presence sentinel. NVR replaces many more pixel than vertex shaders (objects ~134 vs 51,
// terrain 29 vs 2) and the game pairs them independently, so an NVR PS often runs against a
// vanilla VS whose interpolator holds undefined data. NVR vertex shaders stamp this; the PS
// checks it before shadowing.
#define SHADOW_VS_SENTINEL 1.0f
#define SHADOW_VS_PRESENT(w) (abs((w) - SHADOW_VS_SENTINEL) < 0.001f)

// D3DXMACRO from ShaderRecord::LoadShader, off [Shaders.ShadowsExteriors.Main]
// ForwardShadows. Compile-time gate; TESR_ShadowForwardData.x is the runtime one.
#ifndef FORWARD_SHADOWS
    #define FORWARD_SHADOWS 0
#endif

// ---------------------------------------------------------------------------
// UNOFFICIAL 2026-10-23 CharacterShadowMap ([Shaders.ShadowsExteriors.ShadowMaps] CharacterShadowMap; docs/next/CHARACTER-SHADOW-MAP-
// DESIGN.md). P82 testers: the hair's, nose's and brim's sun shadows on a face fade away as the third-person camera zooms out, because
// the cascade that holds the head then has 1 cm texels (Middle at the default 1536 atlas) and a 3 x 3 filter, and a 1-2 cm shadow does
// not survive that. The DLL draws a small sun shadow map of the player alone (ShadowManager::RenderCharacterShadowMap: R32F plain
// depth, 1024^2 by default, fitted round the head and upper body, 1-3 mm texels, the same quantised sun as the cascades, independent
// of the camera's zoom), and this lookup takes the darker of the cascades' shadow and that map's where the point is inside the map's
// box, fading back to the cascades alone over the box's outer band (TESR_CharShadowData.y). The cascades still hold the player, so
// the player's shadow on the ground and the world's shadows on the player are unchanged; the map only adds back the fine self-shadow.
// Compiled in only when the setting was on at startup (CHARACTER_SHADOW_MAP from ShaderRecord::BuildDefines) AND the including
// shader opts in (SHADOW_CHARACTER_MAP_IN 1: SkinTemplate / SkinVPSTemplate / the hand-written skin shaders, and ObjectTemplate's
// pixel shaders except the six-lamp one at the driver's slot limit), with the forward path, outdoors (the Interior records compile
// the sun's code out, and their s11 is a lamp's cube map): everywhere else none of it is compiled and the shaders are exactly what
// they were. Registers: c206-c209 and c213-c214 are free in every pixel shader that opts in (c204 CharacterObjectReach, c205 LampFalloff,
// c210-c212 ExactPassSums / SpecularFade / MaterialAware; nothing between, nothing at c213-c217); s11 is free in the Default (outdoor)
// records (s0-s7 the game's textures, s8 unused outdoors, s9 the atlas, s10 its cross-fade copy or the live cube).
// Whether it RUNS is TESR_CharShadowData.w (1 while the map holds this frame's draw: third person, the sun up, the map made).
#ifndef CHARACTER_SHADOW_MAP
    #define CHARACTER_SHADOW_MAP 0
#endif
// UNOFFICIAL 2026-10-24 SmoothObjectNormals (see the helpers after GetShadowSmoothNormal). ON unless ShaderRecord::BuildDefines says 0
// ([Shaders.PBR.Main] SmoothObjectNormals = false): a DLL from before the switch existed defines nothing, and the fix still applies.
#ifndef OBJECT_SMOOTH_NORMALS
    #define OBJECT_SMOOTH_NORMALS 1
#endif
#ifndef SHADOW_CHARACTER_MAP_IN
    #define SHADOW_CHARACTER_MAP_IN 0
#endif
#if CHARACTER_SHADOW_MAP && SHADOW_CHARACTER_MAP_IN && FORWARD_SHADOWS && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS)
    #define SHADOW_CHARACTER_MAP 1
#else
    #define SHADOW_CHARACTER_MAP 0
#endif

// ---------------------------------------------------------------------------
// UNOFFICIAL 2026-11-04 ForwardCloudShadows ([Shaders.VolumetricClouds.Main] ForwardShadows, restart: FORWARD_CLOUD_SHADOWS from
// ShaderRecord::BuildDefines; core/ForwardCloudShadows.h). The volumetric clouds' shadow taken off the SUN term inside the surface
// shaders, per MSAA sample, instead of SunFar's factor over the finished picture (that factor is one per pixel: with the game's MSAA it
// darkened a whole edge pixel by its nearest sample's sun share, and it darkened grass and plants, which nothing could tell apart).
// Grass and the speedtree leaves do not include it: they keep their sunlight under a cloud. The object passes of plants get strength 0
// from the DLL ([Shaders.VolumetricClouds.Main] PlantShadows off).
//   SHADOW_CLOUD_IN (the object, skin and LOD-land pixel shaders): the clouds' shadow map (TESR_CloudShadow, s8: free in every outdoor
//     record; the Interior records' s8 is a lamp's) looked up here, on NVR_CloudSun (c169, set per draw).
//   SHADOW_CLOUD_VALUE (the land: every sampler is taken): its vertex shader looks the map up (CloudSunVertex, the vertex sampler s1)
//     and the pixel shader puts the interpolated value in NvrCloudSun before GetSunShadow.
#ifndef FORWARD_CLOUD_SHADOWS
    #define FORWARD_CLOUD_SHADOWS 0
#endif
#if FORWARD_CLOUD_SHADOWS && FORWARD_SHADOWS && defined(SHADOW_CLOUD_IN) && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS)
    #define SHADOW_CLOUD_SAMPLE 1
#else
    #define SHADOW_CLOUD_SAMPLE 0
#endif
#if FORWARD_CLOUD_SHADOWS && FORWARD_SHADOWS && defined(SHADOW_CLOUD_VALUE) && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS)
    #define SHADOW_CLOUD_FROM_VALUE 1
    static float NvrCloudSun = 1.0f;	// the includer's value for this pixel (1 lit)
#else
    #define SHADOW_CLOUD_FROM_VALUE 0
#endif
#if SHADOW_CLOUD_SAMPLE
float4 NVR_CloudSun : register(c169); // xy: the map's uv under the camera along the light, z: map uv per world unit, w: strength (0 off)
sampler2D TESR_CloudShadow : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// How much sun the clouds let through at P (camera-relative world): the map where the light from P meets the cloud base, as
// VolumetricClouds.fx CloudShadowAt; 1 outside the map (faded over its last tenth). TESR_SmoothedSunDir stands in for the map's light
// for the height term only (P.z off the camera's height: a few metres, against a cloud base 1.5 km up).
float CloudSunAt(float3 P) {
    float3 light = normalize(TESR_SmoothedSunDir.xyz);
    float2 suv = NVR_CloudSun.xy + (P.xy - light.xy * (P.z / max(light.z, 0.25f))) * NVR_CloudSun.z;
    float2 edge = min(suv, 1.0f - suv);
    float inside = saturate(min(edge.x, edge.y) * 10.0f);
    return lerp(1.0f, tex2Dlod(TESR_CloudShadow, float4(suv, 0.0f, 0.0f)).r, inside * NVR_CloudSun.w);
}
#endif
#if SHADOW_CHARACTER_MAP
row_major float4x4 TESR_CharShadowCameraToLight : register(c206); // camera-relative world -> the map's clip space (ortho: w = 1)
float4 TESR_CharShadowCenter : register(c213); // xyz: the box's centre (camera-relative world), w: its half-extent (world units)
float4 TESR_CharShadowData   : register(c214); // x: 1 / resolution, y: hand-over band (share of the half-extent), z: depth bias (map depth), w: 1 = live (0.5: the first-person viewmodel, tent PCF)
// One line, like TESR_ShadowAtlas above.
// UNOFFICIAL 2026-10-23 (P82 v4 fix): the POINT-light object shaders (SLS2051-2056) are compiled with POINT defined (empty), which emptied the filter names below: lifted round this one line.
#ifdef POINT
    #undef POINT
    #define NVR_CHARMAP_POINT_WAS_DEFINED
#endif
sampler2D TESR_CharShadowMap : register(s11) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#ifdef NVR_CHARMAP_POINT_WAS_DEFINED
    #define POINT
    #undef NVR_CHARMAP_POINT_WAS_DEFINED
#endif

// The map's shadow at a point, blended with the cascades' (cascadeShadow) by where the point is in the box: 1 lit, towards 0 in shadow.
// NdotL: the surface's cosine to the sun (the receiver-plane slack and the normal offset grow as the sun grazes it, as GetSunShadow's).
// A 3 x 3 PCF weighed with a quadratic B-spline centred on the point (ShadowPCF3x3's plain path, on this one map), the taps kept half a
// texel inside the map. Receivers past the box's far plane compare at depth 1 and are shadowed by any caster in the map in front of
// them (the player's shadow on the ground, when the ground is inside the box's footprint); receivers nearer the sun than its near
// plane compare below 0 and are lit, as nothing is drawn in front of them (ShadowMap.vso pancakes to 0).
float CharacterMapShadow(float3 worldPos, float3 worldNormal, float NdotL, float cascadeShadow) {
    float texel = max(TESR_CharShadowData.x, 1.0f / 16384.0f);
    float halfExtent = max(TESR_CharShadowCenter.w, 1.0f);
    float pushScale = saturate(1.0f - NdotL);
    // normal offset: 1.5 of this map's texels (2R world units across res texels) at a grazing sun, none facing it
    float3 position = worldPos + worldNormal * (pushScale * 1.5f * 2.0f * halfExtent * texel);
    float4 lightSpace = mul(float4(position, 1.0f), TESR_CharShadowCameraToLight);
    float edge = max(abs(lightSpace.x), abs(lightSpace.y));
    float band = max(TESR_CharShadowData.y, 0.01f);
    float weight = 1.0f - smoothstep(1.0f - band, 1.0f, edge);
    [branch] if (weight <= 0.0f) return cascadeShadow;
    float2 uv = float2(lightSpace.x * 0.5f + 0.5f, lightSpace.y * -0.5f + 0.5f);
    // receiver-plane slack: tan of the sun's angle to the face (capped at 5) per texel of depth (the box is 2R wide and 2R deep: one
    // texel is 1 / res of depth), 1.5 texels for the 3 x 3's reach, plus the constant bias (CharacterShadowMapBias, world units)
    float slope = min(sqrt(saturate(1.0f - NdotL * NdotL)) / max(NdotL, 0.05f), 5.0f);
    // Past the far plane (depth above 1) the point compares just under 1: an empty texel (cleared to 1) leaves it lit, a caster
    // in front of it (any depth under 1) shadows it. Without the clamp everything past the far plane inside the box was in shadow.
    float depth = min(lightSpace.z, 0.9999f) - TESR_CharShadowData.z - 1.5f * texel * slope;
    float2 lo = texel * 0.5f;
    float2 hi = 1.0f - texel * 0.5f;
    float lit = 0.0f;
    // UNOFFICIAL 2026-11-04 ViewModelShadows (w 0.5: the first-person arms and weapon in the map, ShadowManager::RenderViewModelShadowMap):
    // PCF weighed by a tent 2.5 texels in radius (6 x 6 taps, as Physically-Based-Reloaded's), so the edges come out smooth rather than
    // stepped at the viewmodel's close range. Loops, not unrolled: every shader that reads the map carries it.
    [branch] if (TESR_CharShadowData.w < 0.75f) {
        float2 q = uv / texel;
        float2 first = floor(q - 0.5f) - 2.0f;
        float total = 0.0f;
        [loop] for (int ty = 0; ty < 6; ty++) {
            float y = first.y + ty;
            float wy = max(2.5f - abs(y + 0.5f - q.y), 0.0f);
            [loop] for (int tx = 0; tx < 6; tx++) {
                float x = first.x + tx;
                float w = max(2.5f - abs(x + 0.5f - q.x), 0.0f) * wy;
                float2 tapUV = clamp((float2(x, y) + 0.5f) * texel, lo, hi);
                lit += depth <= tex2Dlod(TESR_CharShadowMap, float4(tapUV, 0.0f, 0.0f)).x ? w : 0.0f;
                total += w;
            }
        }
        return lerp(cascadeShadow, min(cascadeShadow, lit / max(total, 0.0001f)), weight);
    }
    float2 p = uv / texel - 0.5f;
    float2 c = floor(p + 0.5f);
    float2 f = p - c;
    float3 wx = float3(0.5f * (0.5f - f.x) * (0.5f - f.x), 0.75f - f.x * f.x, 0.5f * (0.5f + f.x) * (0.5f + f.x));
    float3 wy = float3(0.5f * (0.5f - f.y) * (0.5f - f.y), 0.75f - f.y * f.y, 0.5f * (0.5f + f.y) * (0.5f + f.y));
    [unroll] for (int y = 0; y < 3; y++) {
        [unroll] for (int x = 0; x < 3; x++) {
            float2 tapUV = clamp((c + float2(x - 1, y - 1) + 0.5f) * texel, lo, hi);
            lit += (depth <= tex2Dlod(TESR_CharShadowMap, float4(tapUV, 0.0f, 0.0f)).x ? 1.0f : 0.0f) * wx[x] * wy[y];
        }
    }
    // CharacterShadowMapDebug (TESR_CharShadowData.w = 1 + the setting): inside the box, 2 = the map's own verdict alone (no cascades),
    // 3 = the depth the map holds at this point (0 black = nothing bound or nothing drawn, 1 white = empty, gray = a caster),
    // 4 = this point's own depth in the box (what the map is compared with).
    [branch] if (TESR_CharShadowData.w > 1.5f) {
        if (TESR_CharShadowData.w > 3.5f) return saturate(lightSpace.z);
        if (TESR_CharShadowData.w > 2.5f) return tex2Dlod(TESR_CharShadowMap, float4(clamp(uv, lo, hi), 0.0f, 0.0f)).x;
        return lit;
    }
    return lerp(cascadeShadow, min(cascadeShadow, lit), weight);
}
#endif

// ---------------------------------------------------------------------------
// VSM / EVSM evaluation. Mirrors Effects/Includes/Shadows.hlsl, duplicated because the two
// trees compile independently.
// ---------------------------------------------------------------------------
float ShadowLinstep(float lo, float hi, float val) {
    return saturate((val - lo) / (hi - lo));
}

float ShadowReduceLightBleeding(float pm, float amount) {
    return ShadowLinstep(amount, 1.0f, pm);
}

float ShadowChebyshevUpperBound(float2 mom, float mu, float minVar, float bleed) {
    float vr = mom.y - (mom.x * mom.x);
    vr = max(vr, minVar);

    float dd = mu - mom.x;
    float pm = vr / (vr + (dd * dd));

    pm = ShadowReduceLightBleeding(pm, bleed);

    return (mu <= mom.x ? 1.0f : pm);
}

float2 ShadowEVSMExponents() {
    const float mx = (SHADOW_FORMAT == 0.0f) ? 5.54f : 42.0f;
    return min(float2(40.0f, 5.0f), mx);
}

float2 ShadowWarpDepth(float dz, float2 ex) {
    dz = 2.0f * dz - 1.0f;
    return float2(exp(ex.x * dz), -exp(-ex.y * dz));
}

// ---------------------------------------------------------------------------
// Atlas sampling. The four cascades occupy the four quadrants of one texture:
//   Near (0.0, 0.0)   Middle (0.5, 0.0)
//   Far  (0.0, 0.5)   Lod    (0.5, 0.5)
// ---------------------------------------------------------------------------
// tex2Dlod, not tex2D: gradients inside the dynamic cascade branch are illegal in ps_3_0
// (X3528). The atlas has no mipmaps, so LOD 0 is equivalent.
float4 SampleShadowMoments(float2 at, float2 quad) {
#if SHADOW_FILTER_TAPS <= 1
    return tex2Dlod(TESR_ShadowAtlas, float4(at, 0.0f, 0.0f));	// the map is prefiltered (blurred): one tap
#else
    // Taps must stay inside their quadrant: crossing a border reads another cascade's depths.
    // Half a texel of inset keeps bilinear off the seam.
    float tx = TESR_ShadowBlur.x;
    float2 lo_ = quad + tx * 0.5f;
    float2 hi_ = quad + 0.5f - tx * 0.5f;

    // Rotated grid: diagonals cover the texel footprint more evenly than axes.
    float2 dd = tx * SHADOW_FILTER_SPREAD;
    float4 mm;
    mm  = tex2Dlod(TESR_ShadowAtlas, float4(clamp(at + dd * float2( 1.0f,  0.5f), lo_, hi_), 0.0f, 0.0f));
    mm += tex2Dlod(TESR_ShadowAtlas, float4(clamp(at + dd * float2(-0.5f,  1.0f), lo_, hi_), 0.0f, 0.0f));
    mm += tex2Dlod(TESR_ShadowAtlas, float4(clamp(at + dd * float2(-1.0f, -0.5f), lo_, hi_), 0.0f, 0.0f));
    mm += tex2Dlod(TESR_ShadowAtlas, float4(clamp(at + dd * float2( 0.5f, -1.0f), lo_, hi_), 0.0f, 0.0f));
    return mm * 0.25f;
#endif
}

// UNOFFICIAL 2026-10-20 SmoothCharacterShadows (CharacterShadowSoftness, TESR_ShadowForwardData.z): the skin shaders set this to the
// least half-width, in world units, a sun shadow edge on skin fades over (real skin carries light a few millimetres past a shadow
// line; without it a face showed every stair step of the shadow map and the straight edges of the head's triangles, hard, and
// moving with every turn of the head). Used by the contact-hardening lookup below (PCF, Mode 3; declared in every mode so the
// callers need no guard), only where its search finds an edge near the point. Compiled in only for the skin shaders, which define
// SHADOW_SKIN_SOFTNESS 1 before including this file: everywhere else none of it is compiled and the shaders are
// exactly what they were (a static the compiler cannot prove stays 0 cost the hair shaders 5 slots).
#ifndef SHADOW_SKIN_SOFTNESS
    #define SHADOW_SKIN_SOFTNESS 0
#endif
#if SHADOW_SKIN_SOFTNESS
    static float ShadowMinPenumbra = 0.0f;
#endif

#if SHADOW_FIXED_MODE == 3
// UNOFFICIAL 2026-10-14 (user: the stripes over the power armour came back, low sun): how far the receiver's own surface falls
// away from the sun per shadow-map texel, in light-space depth, set by GetSunShadow from the surface's slope to the sun. A
// lookup that compares texels n away from the point lowers its depth by n of these, so a flat surface never shadows itself
// whatever the angle (receiver-plane bias). The fixed normal offset alone was too small at a lowish sun (it grew with
// 1 - N.L), and the contact-hardening disc reaches several texels out with no slack at all. 0 for callers that don't set it.
static float ShadowSlopeBias = 0.0f;

// UNOFFICIAL 2026-10-22 CharacterShadowReach ([Shaders.ShadowsExteriors.Main], TESR_ShadowBlur.w, world units, live; P82 tester,
// after the cascade handover: the hair's shadow on the brow, and the nose's on the cheek, gone on a face that is still big on
// screen in third person). All of the PCF slack above is in shadow-map texels (1.5 for the 3 x 3, 2 with SmoothFilter, 0.73 *
// MaxSoftness + 1 for the contact-hardening blocker search, radius + 1 for its disc, each times the slope's tan up to 5, plus a
// normal offset of 1.5-2.5 texels); on the Middle cascade a texel is 1.1 world units, so a hair 1-2 cm off the brow was taken for
// the brow's own surface (run 20261021-231115). With the setting above 0, a Near lookup's normal offset is capped at that many
// world units, and its depth slacks at that distance from the skin measured along the sun's ray: reach / N.L, N.L floored at 0.2
// (2026-10-27, user, low side sun on NPC faces: the reach itself along the ray, 0.5 x N.L from the skin at a grazing sun, let the
// search and the disc find the face's own surface toward the sun and shadow it in grains wherever N.L was under about 0.7; the
// skin template switches it back for an A/B: SHADOW_REACH_ALONG_RAY). The coarser cascades keep the same cap in their map's depth
// units (it grows with their texels; the handover band holds a Middle lookup's slack to Near's). 0.1 is too tight (the face
// shadows itself); 0 = off. Compiled into the skin shaders only (SHADOW_SKIN_SOFTNESS): the flat plates of the power armour need
// the full slack at a low sun (the 2026-10-14 stripes above); SHADOW_SLACK_CAP_ALL ([Shaders.ShadowsExteriors.Main]
// CasterReachEverywhere, restart, a TEST switch) compiles it into every forward shader for an A/B. Elsewhere none of it is compiled.
// Below: ShadowSlackCap is the cap in light-space depth (the cascades are 2 radii deep: ShadowsExterior.cpp's far plane), set by
// GetSunShadow (callers that never set it, none today, get no cap); SmoothFilter's cubic reaches half a texel further than the
// 3 x 3 it is called in place of (whose 1.5 is already off depth): SHADOW_SLACK_CUBIC_EXTRA.
#ifndef SHADOW_SLACK_CAP_ALL
    #define SHADOW_SLACK_CAP_ALL 0
#endif
// UNOFFICIAL 2026-10-23 CharacterObjectReach ([Shaders.ShadowsExteriors.Main], restart to compile in, live after; P82 tester: a facemask
// drawn with the object shaders faded out of the hair's and nose's shadow sooner than the skin face beside it as the camera zoomed): the
// object templates define SHADOW_SLACK_CAP_ACTOR (ObjectTemplate.hlsl, from SHADOW_CHARACTER_OBJECTS) so their draws can take the skin
// shaders' cap above, but only a draw the CPU flags (NVR_ShadowCharacterDraw.x = 1, c204, set per draw in the RenderPass hooks for an
// actor's head, hair, face mask and headgear: every bone a head/neck bone, or under the head bone): never the armour, whose flat plates
// need the full slack at a low sun (the 2026-10-14 stripes). Every other draw: 0, no cap, the texel slack as before.
#ifndef SHADOW_SLACK_CAP_ACTOR
    #define SHADOW_SLACK_CAP_ACTOR 0
#endif
#if SHADOW_SKIN_SOFTNESS || SHADOW_SLACK_CAP_ALL
    #define SHADOW_CHARACTER_REACH TESR_ShadowBlur.w
#elif SHADOW_SLACK_CAP_ACTOR
    float4 NVR_ShadowCharacterDraw : register(c204); // x: 1 = this draw is an actor's head/face/headgear (cap on), 0 = not
    #define SHADOW_CHARACTER_REACH (TESR_ShadowBlur.w * NVR_ShadowCharacterDraw.x)
#endif
#if SHADOW_SKIN_SOFTNESS || SHADOW_SLACK_CAP_ALL || SHADOW_SLACK_CAP_ACTOR
    #define SHADOW_SLACK_CAP 1
    #ifndef SHADOW_REACH_ALONG_RAY
        #define SHADOW_REACH_ALONG_RAY 1   // the reach measured from the skin (2026-10-27, above); the skin template's A/B switch
    #endif
    static float ShadowSlackCap = 1.0e9f;
    #define SHADOW_SLACK(n) min((n) * ShadowSlopeBias, ShadowSlackCap)
    #define SHADOW_SLACK_WIDE(n) SHADOW_SLACK(n)
    #define SHADOW_SLACK_CUBIC_EXTRA (SHADOW_SLACK(2.0f) - SHADOW_SLACK(1.5f))
#else
    // The two spellings keep the other shaders' bytecode exactly what it was (fxc orders the product as written).
    #define SHADOW_SLACK_CAP 0
    #define SHADOW_SLACK(n) ((n) * ShadowSlopeBias)
    #define SHADOW_SLACK_WIDE(n) (ShadowSlopeBias * (n))
    #define SHADOW_SLACK_CUBIC_EXTRA (0.5f * ShadowSlopeBias)
#endif

// UNOFFICIAL 2026-10-14 PCF ([Shaders.ShadowsExteriors.ShadowMaps] Mode 3). The atlas holds the light-space depth itself; each
// lookup compares the 3 x 3 texels round the point with the point's depth and weighs the results with a quadratic B-spline
// centred on the point (smooth, about two texels of penumbra, no stair steps as the point moves across texels). Nothing to
// prefilter, so no blur pass, and no light bleeding (the price of variance maps). Taps are kept inside the cascade's quadrant.
float ShadowPCF3x3(sampler2D atlas, float2 uv, float depth, float2 quad) {
    float texW = TESR_ShadowBlur.x;
    float2 uvLo = quad + texW * 0.5f;
    float2 uvHi = quad + 0.5f - texW * 0.5f;
    // UNOFFICIAL 2026-10-15 [Shaders.ShadowsExteriors.ShadowMaps] SmoothFilter (TESR_ShadowBlur.z; user: shadows on the
    // character alias, worse at a lower CascadeResolution). Close up -- a third-person character, a low sun across a steep
    // surface -- one shadow-map texW covers several screen pixels, and the 3 x 3 quadratic B-spline's soft edge still follows
    // the texels' stair steps. This weighs the 4 x 4 texels round the point with a cubic B-spline instead: a little wider and
    // one order smoother, so those steps round off into a clean curve. 16 compares instead of 9; the taps reach 2 texels out
    // (half a texW more slope bias).
    [branch] if (TESR_ShadowBlur.z > 0.0f) {
        float2 q = uv / texW - 0.5f;
        float2 b = floor(q);
        float2 t = q - b;                     // 0 .. 1 past the texW at b
        float2 t2 = t * t, t3 = t2 * t;
        float4 cx = float4((1.0f - t.x) * (1.0f - t.x) * (1.0f - t.x), 3.0f * t3.x - 6.0f * t2.x + 4.0f, -3.0f * t3.x + 3.0f * t2.x + 3.0f * t.x + 1.0f, t3.x) / 6.0f;
        float4 cy = float4((1.0f - t.y) * (1.0f - t.y) * (1.0f - t.y), 3.0f * t3.y - 6.0f * t2.y + 4.0f, -3.0f * t3.y + 3.0f * t2.y + 3.0f * t.y + 1.0f, t3.y) / 6.0f;
        float cubicDepth = depth - SHADOW_SLACK_CUBIC_EXTRA;
        float cubic = 0.0f;
        [unroll] for (int cyi = 0; cyi < 4; cyi++) {
            [unroll] for (int cxi = 0; cxi < 4; cxi++) {
                float2 tapUV = clamp((b + float2(cxi - 1, cyi - 1) + 0.5f) * texW, uvLo, uvHi);
                cubic += (cubicDepth <= tex2Dlod(atlas, float4(tapUV, 0.0f, 0.0f)).x ? 1.0f : 0.0f) * cx[cxi] * cy[cyi];
            }
        }
        return cubic;
    }
    float2 p = uv / texW - 0.5f;
    float2 c = floor(p + 0.5f);              // the nearest texW
    float2 f = p - c;                         // -0.5 .. 0.5 from its centre
    float3 wx = float3(0.5f * (0.5f - f.x) * (0.5f - f.x), 0.75f - f.x * f.x, 0.5f * (0.5f + f.x) * (0.5f + f.x));
    float3 wy = float3(0.5f * (0.5f - f.y) * (0.5f - f.y), 0.75f - f.y * f.y, 0.5f * (0.5f + f.y) * (0.5f + f.y));
    float lit = 0.0f;
    [unroll] for (int y = 0; y < 3; y++) {
        [unroll] for (int x = 0; x < 3; x++) {
            float2 tapUV = clamp((c + float2(x - 1, y - 1) + 0.5f) * texW, uvLo, uvHi);
            lit += (depth <= tex2Dlod(atlas, float4(tapUV, 0.0f, 0.0f)).x ? 1.0f : 0.0f) * wx[x] * wy[y];
        }
    }
    return lit;
}
#endif

// Light clip space -> (atlas uv in the cascade's quadrant, light-space depth).
float3 ShadowAtlasPoint(float4 clipL, float2 quad) {
    clipL.xyz /= clipL.w;
    float2 uv01 = float2(clipL.x * 0.5f + 0.5f, clipL.y * -0.5f + 0.5f);
    return float3(uv01 * 0.5f + quad, clipL.z);
}

// Lit share from the stored moments at depth z: VSM (mode 0), EVSM with the positive exponent (1) or both (2, the darker wins).
float ShadowMomentsLit(float4 m, float z, float bias, float bleed) {
#if SHADOW_FIXED_MODE == 0
    return ShadowChebyshevUpperBound(m.xy, z, bias, bleed);
#else
    float2 k = ShadowEVSMExponents();
    float2 w = ShadowWarpDepth(z, k);
    float2 spread = bias * k * w;	// the depth bias carried into warped space, used as a variance floor
    #if SHADOW_FIXED_MODE == 1
        return ShadowChebyshevUpperBound(m.xy, w.x, spread.x * spread.x, bleed);
    #else
        float2 floor2 = spread * spread;
        float litPos = ShadowChebyshevUpperBound(m.xz, w.x, floor2.x, bleed);
        float litNeg = ShadowChebyshevUpperBound(m.yw, w.y, floor2.y, bleed);
        return min(litPos, litNeg);
    #endif
#endif
}

float SampleShadowAtlas(float4 clipL, float2 quad, float bias, float bleed) {
    float3 at = ShadowAtlasPoint(clipL, quad);
#if SHADOW_FIXED_MODE == 3
    return ShadowPCF3x3(TESR_ShadowAtlas, at.xy, at.z - bias - SHADOW_SLACK(1.5f), quad);	// UNOFFICIAL PCF (3 x 3: taps up to 1.5 texels out)
#else
    return ShadowMomentsLit(SampleShadowMoments(at.xy, quad), at.z, bias, bleed);
#endif
}

#if SUN_CROSSFADE
// SampleShadowAtlas on the kept atlas. Kept separate (not a sampler parameter on the function above) so that
// builds without the cross-fade compile exactly as before. One tap whatever SHADOW_FILTER_TAPS says: it only shows
// during the few seconds of a fade.
float SampleShadowAtlasOld(float4 clipL, float2 quad, float bias, float bleed) {
    float3 at = ShadowAtlasPoint(clipL, quad);
#if SHADOW_FIXED_MODE == 3
    return ShadowPCF3x3(TESR_ShadowAtlasOld, at.xy, at.z - bias - SHADOW_SLACK(1.5f), quad);	// UNOFFICIAL PCF
#else
    return ShadowMomentsLit(tex2Dlod(TESR_ShadowAtlasOld, float4(at.xy, 0.0f, 0.0f)), at.z, bias, bleed);
#endif
}

// The shadow from before the last sun step: GetSunShadow's cascade choice on the kept cascades. covered is 0 where
// none of them reaches (the camera has moved on since the step); the new shadow is shown there alone.
float GetOldSunShadow(float3 P, float3 Nw, float grazing, float bias, out float covered) {
    float4 reach = float4(TESR_ShadowOldNearCenter.w, TESR_ShadowOldMiddleCenter.w, TESR_ShadowOldFarCenter.w, TESR_ShadowOldLodCenter.w);
    float4 texWorld = 4.0f * reach * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
    float4 push = grazing * SHADOW_NORMAL_BIAS_TEXELS * texWorld;
#if SHADOW_FIXED_MODE == 3
    // UNOFFICIAL 2026-10-14 (user: fine stripes over the power armour with PCF): plain depth compared over a 3 x 3 footprint
    // catches the receiver's own surface wherever it slopes away from the sun (lit acne; the variance maps tolerated it).
    // Push the lookup off the surface along its normal, 1.5 map texels at a glancing sun (none facing it), as is usual for PCF.
    push = max(push, grazing * 1.5f * texWorld);
#endif
    // UNOFFICIAL 2026-10-21 HandoverBlend: the same bands and shared offsets as GetSunShadow below.
    float4 bandStart = reach - ShadowCascadeBands(reach);
    float4 away;	// distance to each kept cascade's centre
    away.x = length(P - TESR_ShadowOldNearCenter.xyz);
    away.y = length(P - TESR_ShadowOldMiddleCenter.xyz);
    away.z = length(P - TESR_ShadowOldFarCenter.xyz);
    away.w = length(P - TESR_ShadowOldLodCenter.xyz);

#define OLD_TAP_NEAR(o)   SampleShadowAtlasOld(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowOldCameraToLightTransformNear),   float2(0.0f, 0.0f), bias, 0.1f)
#define OLD_TAP_MIDDLE(o) SampleShadowAtlasOld(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowOldCameraToLightTransformMiddle), float2(0.5f, 0.0f), bias, 0.2f)
#define OLD_TAP_FAR(o)    SampleShadowAtlasOld(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowOldCameraToLightTransformFar),    float2(0.0f, 0.5f), bias, 0.6f)
#define OLD_TAP_LOD(o)    SampleShadowAtlasOld(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowOldCameraToLightTransformLod),    float2(0.5f, 0.5f), bias, 0.8f)

    covered = 1.0f;
    float lit = 1.0f;
    [branch] if (away.x < reach.x) {
        float f = smoothstep(bandStart.x, reach.x, away.x);
        float o = lerp(push.x, push.y, f);
        [branch] if (f <= 0.0f)
            lit = OLD_TAP_NEAR(o);
        else
            lit = lerp(OLD_TAP_NEAR(o), OLD_TAP_MIDDLE(o), f);
    }
    else if (away.y < reach.y) {
        float f = smoothstep(bandStart.y, reach.y, away.y);
        float o = lerp(push.y, push.z, f);
        [branch] if (f <= 0.0f)
            lit = OLD_TAP_MIDDLE(o);
        else
            lit = lerp(OLD_TAP_MIDDLE(o), OLD_TAP_FAR(o), f);
    }
    else if (away.z < reach.z) {
        float f = smoothstep(bandStart.z, reach.z, away.z);
        float o = lerp(push.z, push.w, f);
        [branch] if (f <= 0.0f)
            lit = OLD_TAP_FAR(o);
        else
            lit = lerp(OLD_TAP_FAR(o), OLD_TAP_LOD(o), f);
    }
    else if (away.w < reach.w) {
        lit = lerp(OLD_TAP_LOD(push.w), 1.0f, smoothstep(bandStart.w, reach.w, away.w));
    }
    else {
        covered = 0.0f;
    }

#undef OLD_TAP_NEAR
#undef OLD_TAP_MIDDLE
#undef OLD_TAP_FAR
#undef OLD_TAP_LOD

    return saturate(lit);
}
#endif

#ifndef CONTACT_HARDENING
    #define CONTACT_HARDENING 0
#endif
#if CONTACT_HARDENING
// ---------------------------------------------------------------------------
// UNOFFICIAL, optional contact-hardening sun shadows ([Shaders.ContactHardening], off by default). Written for this
// build after the published ideas of percentage-closer soft shadows (Fernando 2005) and variance soft shadow mapping (Yang
// et al. 2010): a shadow edge is sharp where the caster touches the ground and blurs with the distance between them,
// as under the real sun's disc. Near and Middle cascades, EVSM4 (the default mode) only; settings
// [Shaders.ContactHardening.Main and .Status] through ShadowManager::ContactHardeningData.
//   1. Blocker search: the moments averaged over the widest penumbra say what share of that area is lit and, if
//      the unlit part is taken to be one surface, how far toward the sun it is (solving the mean for it).
//   2. Penumbra: that distance times the sun's apparent size, in atlas texels, capped at MaxSoftness.
//   3. Filter: the moments averaged over a disc that wide, then the usual Chebyshev test once -- moments filter
//      linearly, which is the point of variance shadow maps. Under a texel wide, the plain one-tap lookup.
// Compiled in only when the setting is on at startup (CONTACT_HARDENING from ShaderRecord::LoadShader), so that off the
// shaders are exactly what they were. c167: after the sun cross-fade's c146-c166.
// ---------------------------------------------------------------------------
float4 TESR_ContactHardeningData : register(c167); // x: penumbra width per world unit of caster distance (0 = off), y: widest penumbra radius (atlas texels), z: 1 = Smooth (PCF Mode 3)

// UNOFFICIAL 2026-10-21 HandoverBlend: scale on the widest penumbra (MaxSoftness, in the cascade's own texels, so 3-4 times wider
// in world units on Middle than on Near). Set by GetSunShadow for a Middle lookup inside the Near-Middle band, so the cap goes
// smoothly from Near's world width to Middle's across the band instead of stepping at the border. 1 everywhere else.
static float ShadowSoftnessCap = 1.0f;

static const float2 ContactDisc[12] = {
    float2(0.2041f, 0.0000f), float2(-0.2607f, 0.2388f), float2(0.0399f, -0.4547f), float2(0.3286f, 0.4286f),
    float2(-0.6030f, -0.1067f), float2(0.5712f, -0.3634f), float2(-0.1911f, 0.7107f), float2(-0.3644f, -0.7016f),
    float2(0.7906f, 0.2887f), float2(-0.8224f, 0.3395f), float2(0.3965f, -0.8472f), float2(0.2930f, 0.9341f) };  // golden-angle spiral

#if SHADOW_FIXED_MODE == 3
static const float2 ContactDisc8[8] = {
    float2(0.2500f, 0.0000f), float2(-0.3193f, 0.2925f), float2(0.0489f, -0.5569f), float2(0.4025f, 0.5248f),
    float2(-0.7385f, -0.1306f), float2(0.6997f, -0.4451f), float2(-0.2340f, 0.8705f), float2(-0.4467f, -0.8590f) };  // golden-angle spiral, 8 points

// UNOFFICIAL 2026-10-14: the 2 x 2 texels round uv compared with depth and blended bilinearly, kept inside the quadrant.
float ShadowPCF2x2Clamped(float2 uv, float depth, float2 uvLo, float2 uvHi) {
    float texW = TESR_ShadowBlur.x;
    float2 p = uv / texW - 0.5f;
    float2 base = floor(p);
    float2 f = p - base;
    float2 uv0 = (base + 0.5f) * texW;
    float4 d = float4(tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv0, uvLo, uvHi), 0.0f, 0.0f)).x,
                      tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv0 + float2(texW, 0.0f), uvLo, uvHi), 0.0f, 0.0f)).x,
                      tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv0 + float2(0.0f, texW), uvLo, uvHi), 0.0f, 0.0f)).x,
                      tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv0 + texW, uvLo, uvHi), 0.0f, 0.0f)).x);
    float4 lit = depth <= d ? 1.0f : 0.0f;
    return lerp(lerp(lit.x, lit.y, f.x), lerp(lit.z, lit.w, f.x), f.y);
}
#endif

#if SHADOW_FIXED_MODE == 3
// UNOFFICIAL 2026-10-14: Mode 3's lookup with or without contact hardening in one body (contact false: the plain 3 x 3), so a
// shader holds one copy of the 3 x 3 and one of the disc, not one per exit and per caller (SunCascadeTap): the copies put the
// 4-lamp object shaders (SLS2034, SLS2036) over ps_3_0's temp registers with _rmaos maps and hand blood on.
float SampleShadowAtlasPCSS(float3 position, float4x4 lightTransform, float2 quad, float bias, bool contact);
#endif
float SampleShadowAtlasContact(float3 position, float4x4 lightTransform, float2 quad, float bias, float bleedReduction) {
#if SHADOW_FIXED_MODE == 3
    return SampleShadowAtlasPCSS(position, lightTransform, quad, bias, true);
}
float SampleShadowAtlasPCSS(float3 position, float4x4 lightTransform, float2 quad, float bias, bool contact) {
    float4 lightSpace = mul(float4(position, 1.0f), lightTransform);
    // UNOFFICIAL 2026-10-14 contact hardening for PCF (Mode 3), as below for EVSM4 but on plain depth (PCSS):
    //   1. Blocker search: the depths over the widest penumbra; the ones nearer the sun than this point are its casters,
    //      their average how far toward the sun they are.
    //   2. Penumbra: that distance times the sun's apparent size, in atlas UV, capped at MaxSoftness.
    //   3. Filter: under a texW wide, the plain 3 x 3 lookup; wider, 2 x 2 filtered compares on a disc that wide (smooth,
    //      no banding), only where something casts a shadow nearby.
    lightSpace.xyz /= lightSpace.w;
    float2 uv = float2(lightSpace.x * 0.5f + 0.5f, lightSpace.y * -0.5f + 0.5f) * 0.5f + quad;
    float depth = lightSpace.z - bias;
    float worldPerDepth = 1.0f / length(float3(lightTransform[0][2], lightTransform[1][2], lightTransform[2][2]));
    float uvPerWorld = 0.25f * length(float3(lightTransform[0][0], lightTransform[1][0], lightTransform[2][0]));
    float texW = TESR_ShadowBlur.x;
    float maxRadius = TESR_ContactHardeningData.y * ShadowSoftnessCap * texW;
    float2 uvLo = quad + texW * 0.5f;
    float2 uvHi = quad + 0.5f - texW * 0.5f;

    // Cheaper version (2026-10-14, was 7 search reads + 13 x 2 x 2 everywhere a caster was near, +0.9 ms): four search
    // reads at 3/4 of the widest penumbra plus the centre; where none or all of them are blocked the point is plainly lit or
    // deep in the umbra and gets the plain lookup (so a miss here only ever looks like plain Mode 3); only real penumbra
    // pixels pay for the 1 + 8 disc of 2 x 2 compares.
    static const float2 search[5] = { float2(0.0f, 0.0f), float2(0.73f, 0.20f), float2(-0.20f, 0.73f), float2(-0.73f, -0.20f), float2(0.20f, -0.73f) };
    float radius = 0.0f;
    [branch] if (contact) {
        float casterSum = 0.0f, casters = 0.0f;
        float searchDepth = depth - SHADOW_SLACK_WIDE(0.73f * TESR_ContactHardeningData.y * ShadowSoftnessCap + 1.0f);
        [unroll] for (int s = 0; s < 5; s++) {   // one read at a time: the terrain shaders are at the temp register limit
            float d = tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + search[s] * maxRadius, uvLo, uvHi), 0.0f, 0.0f)).x;
            float blocks = d < searchDepth ? 1.0f : 0.0f;
            casterSum += d * blocks;
            casters += blocks;
        }
        float distance = max(depth - casterSum / max(casters, 1.0f), 0.0f) * worldPerDepth;
        radius = casters < 0.5f || casters > 4.5f ? 0.0f : min(0.5f * distance * TESR_ContactHardeningData.x * uvPerWorld, maxRadius);
        // UNOFFICIAL 2026-10-20 skin (ShadowMinPenumbra): an edge near the point fades over at least that width, up to the search's
        // reach (a disc wider than the search let points just past its reach take the plain lookup: a hard seam round shadows).
        #if SHADOW_SKIN_SOFTNESS
        if (casters > 0.5f && casters < 4.5f) radius = max(radius, min(ShadowMinPenumbra * uvPerWorld, 0.73f * maxRadius));
        #endif
    }
    // UNOFFICIAL 2026-10-15 [Shaders.ContactHardening.Main] Smooth (TESR_ContactHardeningData.z), against the aliasing seen
    // with contact hardening: (1) the plain 3 x 3 (under a texW) and the disc (over it) no longer switch at one penumbra
    // width, where the edge's look jumped along a visible line: between one and two texels they blend; (2) the disc's
    // eight taps are turned by a different angle for each third of a shadow texW (a hash of the point's place in the
    // shadow map, so the pattern stays put on the ground as the camera moves): the eight copies of the edge that showed as
    // stair-steps and bands in wide penumbrae become a fine, even grain instead. Off: the switch and the fixed disc, the
    // same arithmetic as before.
    float w3 = TESR_ContactHardeningData.z > 0.0f ? saturate(2.0f - radius / texW) : (radius <= texW ? 1.0f : 0.0f);
    float lit = 0.0f;
    [branch] if (w3 > 0.0f) lit = w3 * ShadowPCF3x3(TESR_ShadowAtlas, uv, depth - SHADOW_SLACK(1.5f), quad);
    [branch] if (w3 < 1.0f) {
        float discDepth = depth - SHADOW_SLACK_WIDE(radius / texW + 1.0f);
        float2 turn = float2(1.0f, 0.0f);
        [branch] if (TESR_ContactHardeningData.z > 0.0f)
            sincos(6.2831853f * frac(52.9829189f * frac(dot(floor(uv / texW * 3.0f), float2(0.06711056f, 0.00583715f)))), turn.y, turn.x);
        float disc = ShadowPCF2x2Clamped(uv, discDepth, uvLo, uvHi);
        [unroll] for (int i = 0; i < 8; i++) {
            float2 o = ContactDisc8[i];
            disc += ShadowPCF2x2Clamped(uv + float2(o.x * turn.x - o.y * turn.y, o.x * turn.y + o.y * turn.x) * radius, discDepth, uvLo, uvHi);
        }
        lit += (1.0f - w3) * disc / 9.0f;
    }
    return lit;
#elif SHADOW_FIXED_MODE != 2
    // UNOFFICIAL 2026-10-15: lightSpace computed here too (it moved into the Mode 3 function above, and Modes 0-2 with
    // contact hardening on stopped compiling: 217 game shaders fell back to the game's own).
    return SampleShadowAtlas(mul(float4(position, 1.0f), lightTransform), quad, bias, bleedReduction);
#else
    float4 lightSpace = mul(float4(position, 1.0f), lightTransform);
    lightSpace.xyz /= lightSpace.w;
    float2 uv = float2(lightSpace.x * 0.5f + 0.5f, lightSpace.y * -0.5f + 0.5f) * 0.5f + quad;
    float depth = lightSpace.z;

    // The cascades are orthographic, so world units per light-space depth and atlas UV per world unit are constant:
    // the lengths of the matrix columns that make light-space z and x.
    float worldPerDepth = 1.0f / length(float3(lightTransform[0][2], lightTransform[1][2], lightTransform[2][2]));
    float uvPerWorld = 0.25f * length(float3(lightTransform[0][0], lightTransform[1][0], lightTransform[2][0]));
    float texW = TESR_ShadowBlur.x;
    float maxRadius = TESR_ContactHardeningData.y * ShadowSoftnessCap * texW;
    float2 uvLo = quad + texW * 0.5f;
    float2 uvHi = quad + 0.5f - texW * 0.5f;

    float2 exponents = ShadowEVSMExponents();
    float2 warped = ShadowWarpDepth(depth, exponents);
    float2 slack2 = bias * exponents * warped;
    float2 varFloor = slack2 * slack2;

    // 1. Blocker search on the positive-exponent moments (x, z) over the widest penumbra.
    float2 search = tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0f, 0.0f)).xz;
    search += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + maxRadius * float2( 0.7f,  0.7f), uvLo, uvHi), 0.0f, 0.0f)).xz;
    search += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + maxRadius * float2(-0.7f,  0.7f), uvLo, uvHi), 0.0f, 0.0f)).xz;
    search += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + maxRadius * float2( 0.7f, -0.7f), uvLo, uvHi), 0.0f, 0.0f)).xz;
    search += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + maxRadius * float2(-0.7f, -0.7f), uvLo, uvHi), 0.0f, 0.0f)).xz;
    search *= 0.2f;
    float variance = max(search.y - search.x * search.x, varFloor.x);
    float difference = warped.x - search.x;
    float lit = warped.x <= search.x ? 1.0f : variance / (variance + difference * difference);
    [branch] if (lit > 0.99f) return 1.0f;  // nothing within reach casts a shadow here

    // Mean = lit * receiver + (1 - lit) * caster, solved for the caster, then unwarped to light-space depth.
    float casterWarped = max((search.x - lit * warped.x) / (1.0f - lit), 1.0e-6f);
    float casterDepth = (log(casterWarped) / exponents.x + 1.0f) * 0.5f;
    float distance = max(depth - casterDepth, 0.0f) * worldPerDepth;

    // 2. Penumbra radius (half the width) in atlas UV.
    float radius = min(0.5f * distance * TESR_ContactHardeningData.x * uvPerWorld, maxRadius);

    // 3. Filter the moments over it; under a texW the prefiltered single tap is already that soft.
    float4 moments = tex2Dlod(TESR_ShadowAtlas, float4(uv, 0.0f, 0.0f));
    [branch] if (radius > texW) {
        [unroll] for (int i = 0; i < 12; i++)
            moments += tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + ContactDisc[i] * radius, uvLo, uvHi), 0.0f, 0.0f));
        moments /= 13.0f;
    }
    float visPos = ShadowChebyshevUpperBound(moments.xz, warped.x, varFloor.x, bleedReduction);
    float visNeg = ShadowChebyshevUpperBound(moments.yw, warped.y, varFloor.y, bleedReduction);
    return min(visPos, visNeg);
#endif
}

// One Near or Middle tap: contact hardening when on, the plain lookup otherwise. A real branch, not ?: (which would
// run both).
float SampleShadowCascade(float3 position, float4x4 lightTransform, float2 quad, float bias, float bleedReduction) {
    [branch] if (TESR_ContactHardeningData.x > 0.0f)
        return SampleShadowAtlasContact(position, lightTransform, quad, bias, bleedReduction);
    return SampleShadowAtlas(mul(float4(position, 1.0f), lightTransform), quad, bias, bleedReduction);
}
#endif

#if FORWARD_SHADOWS
// UNOFFICIAL 2026-10-25 (the skin shader): the sun's shadow at a point by one plain lookup of the Near cascade, or the Middle
// one past it: no cross-fade, contact hardening, kept-atlas fade or character map. A yes/no in free air, for a caller that
// wants to know whether a place a little way off is in the sun (SkinTemplate.hlsl: whether a shadow at the skin's terminator
// is the skin's own). Only meaningful where GetSunShadow has already passed its own gates (the sun up, the maps live), and
// after it ran (the PCF mode's receiver-plane slack is the one it set). Legal inside dynamic branches: tex2Dlod only.
float SunShadowCoarse(float3 worldPos) {
#if SHADOW_FIXED_MODE == 0
    const float bias = 0.00001f;
#elif SHADOW_FIXED_MODE == 3
    const float bias = 0.0002f;
#else
    const float bias = 0.01f;
#endif
    const bool near = length(worldPos - TESR_ShadowNearCenter.xyz) < TESR_ShadowNearCenter.w;
    const float4x4 lightTransform = near ? TESR_ShadowCameraToLightTransformNear : TESR_ShadowCameraToLightTransformMiddle;
    return saturate(SampleShadowAtlas(mul(float4(worldPos, 1.0f), lightTransform), near ? float2(0.0f, 0.0f) : float2(0.5f, 0.0f), bias, near ? 0.1f : 0.2f));
}
#endif

// ---------------------------------------------------------------------------
// Camera-relative world position from clip space. Call in the VS and interpolate: it is exact
// under perspective-correct interpolation and costs one interpolator.
// ---------------------------------------------------------------------------
float3 GetShadowWorldPos(float4 clip) {
    float4 vp = mul(clip, TESR_InvProjectionTransform);
    vp /= vp.w;
    // UNOFFICIAL (P65): put the point at view depth clipPos.w along its view ray. clip.w is the view depth for any
    // perspective projection, so a draw projected with other near/far planes than the camera's still lands on the
    // right point; with matching planes it is the same point (to rounding). Added for the actor shadow bug, whose
    // real cause turned out to be the register overwrite described at the top of this file (fixed in P66).
    vp.xyz *= clip.w / (abs(vp.z) > 1e-6f ? vp.z : 1e-6f);
    return mul(vp, TESR_InvViewTransform).xyz;
}

// ---------------------------------------------------------------------------
// Flat face normal from screen-space derivatives of the interpolated world position. Object
// shaders carry their normal in TANGENT space, so no world normal is available to the bias.
//
// MUST be called from the top level of the pixel shader: ddx/ddy are illegal inside dynamic
// flow control.
// ---------------------------------------------------------------------------
float3 GetShadowGeometricNormal(float3 P) {
    float3 face = normalize(cross(ddx(P), ddy(P)));
    // P is camera-relative: the camera is the origin, so the side facing it is the outside, whatever the winding.
    return face * sign(dot(face, -P));
}

// UNOFFICIAL 2026-10-20 [Shaders.ShadowsExteriors.Main] SmoothCharacterShadows (TESR_ShadowForwardData.y), live (P82 testers:
// sun shadows on faces blocky and popping as the head turns). The flat normal above is one per TRIANGLE: the shadow lookup's
// normal offset (its direction and length), its slope bias, and the old skin's sky light all changed at every triangle edge
// (measured in game, bench\deep-roadmap\evidence\charshadow: triangle-shaped differences of a few brightness levels on a face,
// most on the old skin's shaded side; the hard, jagged, moving edges themselves are ShadowMinPenumbra's part). With the switch
// on, the skin shaders take the smooth vertex normal their vertex shader sends (w = SHADOW_VS_SENTINEL; camera-relative world space, like
// shadowWorldPos) instead. Off, or without one (a vanilla vertex shader): the flat normal, as before. Called only by the skin
// shaders, just before their GetSunShadow: it also sets the skin's shadow edge softness (ShadowMinPenumbra, 0 when off).
float3 ShadowLookupNormal(float3 flatNormal, float4 smoothNormal) {
#if SHADOW_SKIN_SOFTNESS
    ShadowMinPenumbra = TESR_ShadowForwardData.z;
#endif
    float len2 = dot(smoothNormal.xyz, smoothNormal.xyz);
    // UNOFFICIAL 2026-10-24 SmoothObjectNormals (OBJECT_SMOOTH_NORMALS below): with it the smooth normal is taken whatever the live switch says.
    return ((TESR_ShadowForwardData.y > 0.0f || OBJECT_SMOOTH_NORMALS) && SHADOW_VS_PRESENT(smoothNormal.w) && len2 > 0.01f) ? smoothNormal.xyz * rsqrt(len2) : flatNormal;
}

// The vertex shader side: the vertex normal in camera-relative world space, by the same projection round trip as
// GetShadowWorldPos, two units along the normal (whatever space the draw's matrix takes the vertex from: skinned or not).
float4 GetShadowSmoothNormal(float4x4 modelViewProj, float3 position, float3 normal, float3 shadowWorldPos) {
    float3 along = GetShadowWorldPos(mul(modelViewProj, float4(position + 2.0f * normalize(normal), 1.0f)));
    return float4(normalize(along - shadowWorldPos), SHADOW_VS_SENTINEL);
}

// UNOFFICIAL 2026-10-24 SmoothObjectNormals ([Shaders.PBR.Main] SmoothObjectNormals, restart: OBJECT_SMOOTH_NORMALS from
// ShaderRecord::BuildDefines; user 2026-10-24: in lamp-lit interiors every triangle of a character's clothes showed, flat with hard
// edges). The object shaders get their normal in tangent space and had no world normal in the pixel shader, so GetShadowGeometricNormal
// above built one from screen derivatives: one per TRIANGLE. It fed the sun lookup's normal offset and slope slack, the lamp lookups'
// normal offset and the sky ambient term (Object.hlsl getAmbientLighting), so all three stepped at every triangle edge. With the switch
// the vertex shaders send the vertex normal in camera-relative world space (GetShadowSmoothNormal above) and the pixel shaders take it
// instead, through the helpers below; the derivative normal is not computed at all then (the slot cost of the two is about the same).
// Where no interpolator is spare the normal rides octahedrally encoded in two channels. Every reader of the normal is already gated by
// the vertex shader's sentinel (a vanilla vertex shader: the sun lookup, the lamp lookups and the sky term are skipped), so an unwritten
// normal is never used; ShadowFacingNormal still returns a unit vector for one. Off: the shaders exactly as before.
// (OBJECT_SMOOTH_NORMALS's default is set near the top of this file, before ShadowLookupNormal reads it.)

// Octahedral encoding of a unit vector in two numbers (Cigolle et al. 2014) and back.
float2 ShadowEncodeNormal(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
    float2 e = n.xy;
    if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return e;
}
float3 ShadowDecodeNormal(float2 e) {
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}

// UNOFFICIAL 2026-10-26 (user: garbage lamp shadows round lamps on ceilings, shifting as the camera moved or turned, gone with
// AutoNormalOffset off): the object shaders send the smooth normal as a plain vector, x y in two spare channels and z in the vertex
// colour's alpha (COLOR0 is clamped, so encoded), instead of octahedrally. The octahedral map folds at straight down: a ceiling's
// vertex normals, a hair either side of (0, 0, -1) after GetShadowSmoothNormal's projection round trip (which side changed with the
// camera's position and angle), landed in different corners of the square, and the interpolation between corners decoded as
// normals pointing sideways across the triangle. The lamp lookups' normal offset (AutoNormalOffset) pushed along them. A plain vector
// interpolates without a seam. The skin shader keeps the octahedral pair (its light-only passes have no vertex colour).
float3 ShadowPlainNormal(float2 xy, float zColor) {
    return float3(xy, zColor * 2.0f - 1.0f);
}

// The smooth normal as a unit vector facing the camera (worldPos is camera-relative, so the camera is at the origin), the side
// GetShadowGeometricNormal gives: two-sided geometry seen from behind (hair cards, cloth) pushes its lookups out on the visible side.
// A zero or unwritten normal gives (0, 0, 1).
float3 ShadowFacingNormal(float3 n, float3 worldPos) {
    float len2 = dot(n, n);
    n = len2 > 1.0e-6f ? n * rsqrt(len2) : float3(0.0f, 0.0f, 1.0f);
    return n * (dot(n, worldPos) > 0.0f ? -1.0f : 1.0f);
}

#if SHADOW_FIXED_MODE == 3
// UNOFFICIAL 2026-10-14 (PCF): one cascade's lookup, the cascade chosen by c (0 Near .. 3 Lod) with selects rather than one
// copy of the code per cascade: GetSunShadow's tree below wrote the lookup out eight times, which with contact hardening's
// soft lookup made shaders the driver refused to create. Contact hardening on the Near and Middle cascades, as before.
// 2026-10-21 HandoverBlend: the normal offset (world units) comes from the caller, which blends it across the cascade bands.
float SunCascadeTap(float c, float3 worldPos, float3 worldNormal, float offset, float bias) {
    float4x4 lightTransform = c < 0.5f ? TESR_ShadowCameraToLightTransformNear : c < 1.5f ? TESR_ShadowCameraToLightTransformMiddle :
                              c < 2.5f ? TESR_ShadowCameraToLightTransformFar : TESR_ShadowCameraToLightTransformLod;
    float2 quadrant = c < 0.5f ? float2(0.0f, 0.0f) : c < 1.5f ? float2(0.5f, 0.0f) : c < 2.5f ? float2(0.0f, 0.5f) : float2(0.5f, 0.5f);
    float3 position = worldPos + offset * worldNormal;
#if CONTACT_HARDENING
#if SHADOW_SKIN_SOFTNESS
    return SampleShadowAtlasPCSS(position, lightTransform, quadrant, bias, c < 1.5f && (TESR_ContactHardeningData.x > 0.0f || ShadowMinPenumbra > 0.0f));
#else
    return SampleShadowAtlasPCSS(position, lightTransform, quadrant, bias, c < 1.5f && TESR_ContactHardeningData.x > 0.0f);
#endif
#else
    return SampleShadowAtlas(mul(float4(position, 1.0f), lightTransform), quadrant, bias, 0.0f);
#endif
}
#endif

// ---------------------------------------------------------------------------
// UNOFFICIAL 2026-11-06 forward screen-space contact shadows ([Shaders.ShadowsExteriors.ScreenSpace] Bend + InLighting): GetSunShadow
// takes the sun contact visibility itself, so contact takes away the SUN only (ambient and lamps stay) instead of darkening the whole
// finished pixel in the composite. The contact pass runs after the world is drawn, so these shaders read LAST frame's result
// (TESR_ContactForward: r visibility 1 lit .. 0 shadowed, g linear view depth in game units at that pixel, 0 = nothing / sky),
// reprojected through that frame's camera (TESR_ContactReproject: camera-relative point -> last frame's clip, w = view depth; render
// scale, jitter and half texel are folded in by the DLL); where that frame did not see this point (off screen, another depth there)
// no contact (1). Compiled in only when SUN_CONTACT_FORWARD (ShaderRecord::BuildDefines: Bend && InLighting && ForwardShadows) AND the
// including shader opts in (SHADOW_SUN_CONTACT_IN); not in the interior records; not with the sun cross-fade except in the land shaders
// (the cross-fade owns c146-c166). Registers: s10 (s3 with LiveCube) / c160-c163 / c164 (+ b13 per draw; c148-c152 until 2026-11-08: Wetness.hlsl holds them); the land includers define SHADOW_SUN_CONTACT_LAND: s13 / c183-c187.
// ---------------------------------------------------------------------------
#ifndef SHADOW_SUN_CONTACT_IN
    #define SHADOW_SUN_CONTACT_IN 0
#endif
#ifndef SHADOW_SUN_CONTACT_LAND
    #define SHADOW_SUN_CONTACT_LAND 0
#endif
#if defined(SUN_CONTACT_FORWARD) && SHADOW_SUN_CONTACT_IN && FORWARD_SHADOWS && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS) && (SHADOW_SUN_CONTACT_LAND || !SUN_CROSSFADE)
    #define SHADOW_SUN_CONTACT 1
#else
    #define SHADOW_SUN_CONTACT 0
#endif
#if SHADOW_SUN_CONTACT
#if SHADOW_SUN_CONTACT_LAND && defined(SHADOW_SUN_CONTACT_S15)
    #define SUN_CONTACT_SAMPLER_REG s15
    #define SUN_CONTACT_MATRIX_REG c183
    #define SUN_CONTACT_DATA_REG c187
#elif SHADOW_SUN_CONTACT_LAND
    #define SUN_CONTACT_SAMPLER_REG s13
    #define SUN_CONTACT_MATRIX_REG c183
    #define SUN_CONTACT_DATA_REG c187
#else
    // (1.0.5: s12 is HUD raindrops' ortho cover map) s10, or s3 where the live metal cube (LiveCube.hlsl) holds s10
    #ifdef NVR_LIVE_CUBE
        #define SUN_CONTACT_SAMPLER_REG s3
    #else
        #define SUN_CONTACT_SAMPLER_REG s10
    #endif
    #define SUN_CONTACT_MATRIX_REG c160	// (2026-11-08: was c148-c152, which 1.0.5's Wetness.hlsl holds (c146-c159); c160-c166 is the sun cross-fade's, which Bend never compiles with in objects)
    #define SUN_CONTACT_DATA_REG c164
#endif
row_major float4x4 TESR_ContactReproject : register(SUN_CONTACT_MATRIX_REG); // camera-relative world -> last frame's clip (w = view depth)
float4 TESR_ContactForwardData : register(SUN_CONTACT_DATA_REG); // x 1 = use it (2 / 3: BendDebug 1 / 2), y depth match tolerance (fraction of depth), z start distance (view units; at least 64, the view model), w 1 / fade-in length
// One line, like TESR_ShadowAtlas above; POINT lifted round it as for TESR_CharShadowMap.
#ifdef POINT
    #undef POINT
    #define NVR_CONTACT_POINT_WAS_DEFINED
#endif
sampler2D TESR_ContactForward : register(SUN_CONTACT_SAMPLER_REG) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#ifdef NVR_CONTACT_POINT_WAS_DEFINED
    #define POINT
    #undef NVR_CONTACT_POINT_WAS_DEFINED
#endif

// Last frame's sun contact visibility at camera-relative point P: 1 lit, towards 0 in contact shadow. 1 where that frame did not see P.
#if defined(SHADOW_SUN_CONTACT_PERDRAW)
bool NVR_ContactDraw : register(b13); // (LodOnly) true = this draw is object LOD (written per draw by BendContact::ForDraw; a bool register: 1.0.5's HUD raindrops took c153)
#endif
float SunContactForwardLookup(float3 P);
float SunContactForward(float3 P) {
    [branch] if (TESR_ContactForwardData.x < 0.5f) return 1.0f;
    // LOD only (2026-11-06): the LOD land shaders always; an object draw only when it is object LOD (NVR_ContactDraw, a bool
    // register: only as a plain if, X4509 otherwise)
#if defined(SHADOW_SUN_CONTACT_PERDRAW)
    float v = 1.0f;
    if (NVR_ContactDraw) v = SunContactForwardLookup(P);
    return v;
#else
    return SunContactForwardLookup(P);
#endif
}
float SunContactForwardLookup(float3 P) {
    float4 clip = mul(float4(P, 1.0f), TESR_ContactReproject);
    [branch] if (clip.w <= TESR_ContactForwardData.z) return 1.0f;   // nearer than the start (the shadow maps' range; the view model)
    float2 uv = float2(0.5f, -0.5f) * clip.xy / clip.w + 0.5f;
    const float miss = TESR_ContactForwardData.x > 2.5f ? 0.0f : 1.0f;   // BendDebug 2: misses black
    [branch] if (any(abs(uv - 0.5f) > 0.5f)) return miss;
    float2 c = tex2Dlod(TESR_ContactForward, float4(uv, 0.0f, 0.0f)).rg;
    [branch] if (c.g <= 0.0f || abs(c.g - clip.w) > abs(TESR_ContactForwardData.y) * clip.w) return miss;
    return TESR_ContactForwardData.x > 2.5f ? 1.0f : lerp(1.0f, c.r, saturate((clip.w - TESR_ContactForwardData.z) * TESR_ContactForwardData.w));
}
#endif

// ---------------------------------------------------------------------------
// 1.0 in full light, towards 0 in shadow. Multiply the SUN term by it, before ambient.
// ---------------------------------------------------------------------------
float GetSunShadow(float3 P, float3 Nw) {
    // Nonzero SUPPRESSES the forward path; SunShadows.fx reads the same constant and takes
    // over deferred. A missing constant reads zero and leaves forward running.
    if (TESR_ShadowForwardData.x) return 1.0f;

    // Sun shadows never apply indoors. Dedicated flag, not ShadowFade.y below -- that one is
    // shared with ShadowsInteriors.fx's own darkness calc and means something else indoors
    // (interior point-shadows enabled), so it can't double as this gate. Without this check,
    // an interior object shader would fall through to stale exterior cascade data left over
    // from the last time the player was outside.
    if (!TESR_ShadowFormatData.z) return 1.0f;

    // Shadow maps switched off entirely (setting), or sun below horizon.
    if (!TESR_ShadowFade.y) return 1.0f;

    // ShadowMap.pso's channel layout differs per Mode. Fail unshadowed on a mismatch.
    float builtFor = SHADOW_FIXED_MODE;
    if (TESR_ShadowFormatData.x != builtFor) return 1.0f;

    // Push the sample along the normal, scaled by how grazing the sun is.
    float nDL = dot(Nw, TESR_SmoothedSunDir.xyz);
    float offS = saturate(1.0f - nDL);

    float4 rad = float4(TESR_ShadowNearCenter.w, TESR_ShadowMiddleCenter.w, TESR_ShadowFarCenter.w, TESR_ShadowLodCenter.w);

    // Texel world size per cascade. GetCascadeViewProj's ortho box is 2*radius across, and the
    // atlas is two cascades wide, so with TESR_ShadowBlur.x = 1/atlasResolution:
    //     cascadeResolution = 0.5 / TESR_ShadowBlur.x
    //     texelWorldSize    = 4 * radius * TESR_ShadowBlur.x
    // The floor keeps a zero constant from producing an infinite offset.
    float4 tw = 4.0f * rad * max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
    float4 offD = offS * SHADOW_NORMAL_BIAS_TEXELS * tw;
#if SHADOW_FIXED_MODE == 3
    // UNOFFICIAL 2026-10-14 (user: fine stripes over the power armour with PCF): plain depth compared over a 3 x 3 footprint
    // catches the receiver's own surface wherever it slopes away from the sun (shadow acne; the variance maps tolerated it).
    // Push the lookup off the surface along its normal, 1.5 map texels at a glancing sun (none facing it), as is usual for PCF.
    offD = max(offD, offS * 1.5f * tw);
    // Receiver-plane slack (see ShadowSlopeBias): tan of the sun's angle to the face, capped at 5 (about 79 degrees, where the
    // face is barely lit anyway), times one texel of light-space depth (the cascades are 2 radii deep and 2 radii / (0.5 /
    // TESR_ShadowBlur.x) texels wide, so a texel is 2 * TESR_ShadowBlur.x of depth in every cascade).
    ShadowSlopeBias = 2.0f * TESR_ShadowBlur.x * min(sqrt(saturate(1.0f - nDL * nDL)) / max(nDL, 0.05f), 5.0f);
#if SHADOW_SLACK_CAP
    // CharacterShadowReach (see ShadowSlackCap): the normal offset capped at the reach, in world units on Near and growing with
    // the map's texel on the coarser cascades; the depth slacks capped at the reach along the sun's ray (reach / N.L) in Near's
    // depth units (the same on every cascade, which is that same growth). Off (0): no cap, the texel slack alone.
    [flatten] if (SHADOW_CHARACTER_REACH > 0.0f) {
        offD = min(offD, SHADOW_CHARACTER_REACH * rad / max(rad.x, 0.001f));
        ShadowSlackCap = SHADOW_CHARACTER_REACH / (2.0f * max(rad.x, 0.001f) * ((SHADOW_REACH_ALONG_RAY) ? max(nDL, 0.2f) : 1.0f));
    }
#endif
#endif

#if SHADOW_FIXED_MODE == 0
    const float bBase = 0.00001f;
#elif SHADOW_FIXED_MODE == 3
    const float bBase = 0.0002f;   // UNOFFICIAL PCF: a depth offset (light-space depth), not a variance floor
#else
    const float bBase = 0.01f;
#endif

    // Slope-scaled variance floor: bias drives minVariance, the slack Chebyshev gets before
    // calling a texel occluded.
    float bi = bBase * (1.0f + SHADOW_SLOPE_BIAS * offS);

    // UNOFFICIAL 2026-10-21 HandoverBlend (P82 tester: the hair's shadow on the brow popping in and out as the third-person camera
    // zooms): the cross-fade into the next cascade covered the outer tenth of each cascade's radius, 7 cm at the tester's
    // CascadeLambda 0.997, so a zoom crossed it in one frame; and each cascade pushed its lookup off the surface by its own
    // texel size (3-4 times further on Middle), so the point looked up jumped with the cascade and lifted the brow out of a hair
    // shadow a few millimetres thick. Now each band is a fixed width in world units (ShadowCascadeBands) and inside it both
    // cascades look up the SAME point: the normal offset (and, with contact hardening, the widest penumbra) go smoothly from the
    // inner cascade's to the outer's across the band, so only the weights change, between two readings of one place.
    float4 st = rad - ShadowCascadeBands(rad);

    // Distance to each cascade centre, cross-faded over the band. Circular boundaries,
    // matching SunShadows.fx: selecting on the square light-space NDC box steps visibly.
    float4 dst;
    dst.x = length(P - TESR_ShadowNearCenter.xyz);
    dst.y = length(P - TESR_ShadowMiddleCenter.xyz);
    dst.z = length(P - TESR_ShadowFarCenter.xyz);
    dst.w = length(P - TESR_ShadowLodCenter.xyz);

    // Sample inside the branch, not before it. A pixel needs one cascade, or two in the band
    // where they cross-fade; sampling all four up front cost four atlas fetches and four
    // matrix transforms on every shadowed pixel, and the forward path pays that per object.
    //
    // Legal here only because SampleShadowAtlas uses tex2Dlod -- a gradient instruction under
    // dynamic flow control is X3528 in ps_3_0. The atlas has no mipmaps, so LOD 0 is exact.
#if CONTACT_HARDENING
#define SHADOW_TAP_NEAR(o)   SampleShadowCascade(P + (o) * Nw, TESR_ShadowCameraToLightTransformNear,   float2(0.0f, 0.0f), bi, 0.1f)
#define SHADOW_TAP_MIDDLE(o) SampleShadowCascade(P + (o) * Nw, TESR_ShadowCameraToLightTransformMiddle, float2(0.5f, 0.0f), bi, 0.2f)
#else
#define SHADOW_TAP_NEAR(o)   SampleShadowAtlas(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowCameraToLightTransformNear),   float2(0.0f, 0.0f), bi, 0.1f)
#define SHADOW_TAP_MIDDLE(o) SampleShadowAtlas(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowCameraToLightTransformMiddle), float2(0.5f, 0.0f), bi, 0.2f)
#endif
#define SHADOW_TAP_FAR(o)    SampleShadowAtlas(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowCameraToLightTransformFar),    float2(0.0f, 0.5f), bi, 0.6f)
#define SHADOW_TAP_LOD(o)    SampleShadowAtlas(mul(float4(P + (o) * Nw, 1.0f), TESR_ShadowCameraToLightTransformLod),    float2(0.5f, 0.5f), bi, 0.8f)

#if SHADOW_FIXED_MODE == 3
    // UNOFFICIAL (PCF): the cascade and how far into its band the point is first, then at most two lookups (SunCascadeTap):
    // the same choice and cross-fade as the tree below, the Lod cascade fading to unshadowed.
    float cascade = -1.0f, fade = 0.0f;
    [flatten] if (dst.x < rad.x) { cascade = 0.0f; fade = smoothstep(st.x, rad.x, dst.x); }
    else if (dst.y < rad.y) { cascade = 1.0f; fade = smoothstep(st.y, rad.y, dst.y); }
    else if (dst.z < rad.z) { cascade = 2.0f; fade = smoothstep(st.z, rad.z, dst.z); }
    else if (dst.w < rad.w) { cascade = 3.0f; fade = smoothstep(st.w, rad.w, dst.w); }
    // HandoverBlend: this point's normal offset, the same whichever of the two cascades the dither picks below.
    float offsetHere = cascade < 0.5f ? lerp(offD.x, offD.y, fade) : cascade < 1.5f ? lerp(offD.y, offD.z, fade) :
                       cascade < 2.5f ? lerp(offD.z, offD.w, fade) : offD.w;
    // The Middle lookup's slack is in ITS texels (3-4 times Near's in world units): a hair 1-2 cm off the brow is outside
    // Near's slack (it casts) and inside Middle's (taken for the brow's own surface: no shadow), which is the pop itself.
    // Inside the Near-Middle band a Middle lookup keeps Near's world slack at the band's start, fading to its own: the
    // receiver-plane slope bias (and, with contact hardening, the blocker search's slack and the widest penumbra).
    float slackScale = cascade < 0.5f ? lerp(rad.x / max(rad.y, 0.001f), 1.0f, fade) : 1.0f;
    // UNOFFICIAL 2026-10-14: in a cascade's outer band each point takes this cascade or the next by a fine dither (share = fade)
    // instead of looking both up and blending: two inlined lookups put 30 of the 55 outdoor object shaders over ps_3_0's 32 temp
    // registers with PCSS on, and the game silently drew those objects with its own shaders. Past the Lod cascade: unshadowed.
    // A random number per point (a sin-free hash, fine-grained: a new value every ~0.15 units). Not the screen dither formula on
    // world positions: that made diagonal stripes across the band (user: lines over the power armour).
#if defined(NVR_NO_SM3_LIMITS) && NVR_NO_SM3_LIMITS
    // UNOFFICIAL 2026-10-26 the no-limits build (NVR DXVK fork, TerrainTemplate.hlsl NVR_NO_SM3_LIMITS): no temp limit, so in the
    // band both cascades are looked up and blended by fade, as the VSM/EVSM tree below does: a smooth handover instead of the dither.
    // The lookup in this cascade keeps its own slack; the next one gets slackScale (not 1 only when this cascade is Near).
    float sh = 1.0f;
    [branch] if (cascade >= 0.0f && cascade < 3.5f) {
        sh = SunCascadeTap(cascade, P, Nw, offsetHere, bi);
        [branch] if (fade > 0.0f) {
            float next = 1.0f;   // past the Lod cascade: unshadowed
            [branch] if (cascade < 2.5f) {
                float slopeBiasHere = ShadowSlopeBias;
                ShadowSlopeBias *= slackScale;
#if CONTACT_HARDENING
                ShadowSoftnessCap = slackScale;
#endif
                next = SunCascadeTap(cascade + 1.0f, P, Nw, offsetHere, bi);
                ShadowSlopeBias = slopeBiasHere;
#if CONTACT_HARDENING
                ShadowSoftnessCap = 1.0f;
#endif
            }
            sh = lerp(sh, next, fade);
        }
    }
#else
    float3 hashP = frac(P * 7.1031f);
    hashP += dot(hashP, hashP.yzx + 33.33f);
    float dither = frac((hashP.x + hashP.y) * hashP.z);
    cascade += fade > dither ? 1.0f : 0.0f;
    slackScale = cascade > 0.5f ? slackScale : 1.0f;   // 1 for a Near lookup (and for the far cascades)
    float slopeBiasHere = ShadowSlopeBias;
    ShadowSlopeBias *= slackScale;
#if CONTACT_HARDENING
    ShadowSoftnessCap = slackScale;
#endif
    float sh = 1.0f;
    [branch] if (cascade >= 0.0f && cascade < 3.5f)
        sh = SunCascadeTap(cascade, P, Nw, offsetHere, bi);
    ShadowSlopeBias = slopeBiasHere;
#if CONTACT_HARDENING
    ShadowSoftnessCap = 1.0f;
#endif
#endif   // NVR_NO_SM3_LIMITS
#else
    // Initialised: a point beyond the last cascade falls through every branch.
    float sh = 1.0f;
    [branch] if (dst.x < rad.x) {
        float f = smoothstep(st.x, rad.x, dst.x);
        float o = lerp(offD.x, offD.y, f);
        [branch] if (f <= 0.0f)
            sh = SHADOW_TAP_NEAR(o);
        else {
            float nearTap = SHADOW_TAP_NEAR(o);
#if CONTACT_HARDENING
            // The Middle lookup keeps Near's widest penumbra (in world units) at the band's start, fading to its own.
            ShadowSoftnessCap = lerp(rad.x / max(rad.y, 0.001f), 1.0f, f);
#endif
            float middleTap = SHADOW_TAP_MIDDLE(o);
#if CONTACT_HARDENING
            ShadowSoftnessCap = 1.0f;
#endif
            sh = lerp(nearTap, middleTap, f);
        }
    }
    else if (dst.y < rad.y) {
        float f = smoothstep(st.y, rad.y, dst.y);
        float o = lerp(offD.y, offD.z, f);
        [branch] if (f <= 0.0f)
            sh = SHADOW_TAP_MIDDLE(o);
        else
            sh = lerp(SHADOW_TAP_MIDDLE(o), SHADOW_TAP_FAR(o), f);
    }
    else if (dst.z < rad.z) {
        float f = smoothstep(st.z, rad.z, dst.z);
        float o = lerp(offD.z, offD.w, f);
        [branch] if (f <= 0.0f)
            sh = SHADOW_TAP_FAR(o);
        else
            sh = lerp(SHADOW_TAP_FAR(o), SHADOW_TAP_LOD(o), f);
    }
    else if (dst.w < rad.w) {
        sh = lerp(SHADOW_TAP_LOD(offD.w), 1.0f, smoothstep(st.w, rad.w, dst.w));
    }
#endif

#undef SHADOW_TAP_NEAR
#undef SHADOW_TAP_MIDDLE
#undef SHADOW_TAP_FAR
#undef SHADOW_TAP_LOD

    sh = saturate(sh);

#if SUN_CROSSFADE
    // During a sun step's cross-fade, part old shadow, part new. Outside a fade this is one skipped branch.
    [branch] if (TESR_ShadowCrossFade.x > 0.0f) {
        float covered;
        float oldShadow = GetOldSunShadow(P, Nw, offS, bi, covered);
        sh = lerp(sh, oldShadow, TESR_ShadowCrossFade.x * covered);
    }
#endif

#if SHADOW_CHARACTER_MAP
    // UNOFFICIAL 2026-10-23 CharacterShadowMap: the darker of the cascades' shadow and the player's own map's, inside its box.
    [branch] if (TESR_CharShadowData.w > 0.0f)
        sh = CharacterMapShadow(P, Nw, nDL, sh);
#endif

#if SHADOW_SUN_CONTACT
    // UNOFFICIAL 2026-11-06 forward contact shadows: last frame's sun contact, reprojected (1 where unseen)
    float sunContact = SunContactForward(P);
    sh = TESR_ContactForwardData.x > 1.5f ? sunContact : sh * sunContact;	// BendDebug 1 / 2: the contact (coverage) alone
#endif

    // Fade out as the sun approaches the horizon, matching SunShadows.fx.
    sh = lerp(sh, 1.0f, saturate(TESR_ShadowFade.x));

    // UNOFFICIAL 2026-11-04 ForwardCloudShadows (above): the clouds' share, after the cascades' fade (their strength has its own low-sun fade).
#if SHADOW_CLOUD_SAMPLE
    [branch] if (NVR_CloudSun.w > 0.0f) sh *= CloudSunAt(P);
#elif SHADOW_CLOUD_FROM_VALUE
    sh *= NvrCloudSun;
#endif

    return sh;
}
