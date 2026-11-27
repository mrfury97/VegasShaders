// UNOFFICIAL 2026-11-24: Tier 2 360-degree dynamic world cubemap pixel shader (ShadowManager::RenderReflectionCubeFace).
// Outputs linear HDR radiance (alpha = 1) for ReflectionsCapture.frag / Reflections.fx.hlsl (sky cleared to alpha = 0).

float4 TESR_ShadowData : register(c0);   // y: 1 alpha test, z: 1 glow map bound, w: 1 diffuse map bound
float4 ReflSunDir      : register(c1);   // xyz: world dir toward sun, w: sun strength
float4 ReflSunColor    : register(c2);   // rgb: linear sun color, w: fog start
float4 ReflAmbient     : register(c3);   // rgb: linear ambient color, w: inv fog range
float4 ReflSkyTop      : register(c4);   // rgb: linear sky zenith color, w: isExterior
float4 ReflSkyHorizon  : register(c5);   // rgb: linear horizon/fog color, w: lampCount (0..4)
float4 ReflLampPos[4]  : register(c6);   // xyz: camera-relative lamp pos, w: 1 / (radius * radius)
float4 ReflLampColor[4]: register(c10);  // rgb: linear lamp color, w: unused

sampler2D DiffuseMap : register(s0) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D GlowMap    : register(s1) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

struct PS_INPUT {
    float4 worldN_dist : TEXCOORD0;
    float4 uv          : TEXCOORD1;
    float4 relPos_kind : TEXCOORD2;
};

float4 main(PS_INPUT IN) : COLOR0 {
    float4 diffuse = tex2D(DiffuseMap, IN.uv.xy);
    bool alphaTest = TESR_ShadowData.y > 0.5f;
    if (alphaTest && diffuse.a < 0.5f) discard;

    bool textured = alphaTest || TESR_ShadowData.w > 0.5f;
    float3 albedo = textured ? pow(max(diffuse.rgb, 0.0f), 2.2f) : float3(0.25f, 0.25f, 0.25f);

    float nLen2 = dot(IN.worldN_dist.xyz, IN.worldN_dist.xyz);
    float3 N = nLen2 > 1.0e-4f ? IN.worldN_dist.xyz * rsqrt(nLen2) : float3(0.0f, 0.0f, 1.0f);

    float sunDot = dot(N, ReflSunDir.xyz);
    float sunDiff = (IN.relPos_kind.w > 1.5f && IN.relPos_kind.w < 2.5f)
        ? saturate(sunDot * 0.5f + 0.5f) * 0.75f
        : saturate(sunDot);
    float3 direct = ReflSunColor.rgb * (ReflSunDir.w * sunDiff);

    float3 hemi = lerp(ReflAmbient.rgb * 0.65f, ReflAmbient.rgb + ReflSkyTop.rgb * 0.55f, N.z * 0.5f + 0.5f);

    float3 lamps = 0.0f;
    [unroll] for (int i = 0; i < 4; i++) {
        if ((float)i < ReflSkyHorizon.w) {
            float3 d = ReflLampPos[i].xyz - IN.relPos_kind.xyz;
            float d2 = dot(d, d);
            float att = saturate(1.0f - d2 * ReflLampPos[i].w);
            att *= att;
            float3 L = d * rsqrt(max(d2, 1.0e-4f));
            float NdotL = saturate(dot(N, L) * 0.75f + 0.25f);
            lamps += ReflLampColor[i].rgb * (att * NdotL);
        }
    }

    float3 glow = 0.0f;
    if (TESR_ShadowData.z > 0.5f) {
        glow = pow(max(tex2D(GlowMap, IN.uv.xy).rgb, 0.0f), 2.2f) * 2.0f;
    }

    float3 lit = albedo * (direct + hemi + lamps) + glow;
    float fogT = saturate((IN.worldN_dist.w - ReflSunColor.w) * ReflAmbient.w);
    lit = lerp(lit, ReflSkyHorizon.rgb, fogT * 0.85f);
    lit = min(max(lit, 0.0f), 16.0f);
    return float4(lit, 1.0f);
}
