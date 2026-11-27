// UNOFFICIAL lit particles and blood decals (Shaders.Particles, off by default), shared by NOLIGHT016/017.vso,
// GDECAL.vso, GDECALS.vso (with PARTICLE_VS defined) and NOLIGHTTEXVC.pso, GDECAL.pso.
//
// Particles and decals have no usable normal (thin, camera-facing; a decal's NORMAL carries its texture coordinates),
// so light is taken as reaching them from all sides. The vertex shader adds up what changes slowly across a particle --
// the ambient and the nearby point lights -- once per vertex; the pixel shader adds only the sun through the sun
// shadow, which must follow the shadow edges. Per pixel, the 24-light loop cost several ms in smoke and blood clouds,
// whose large particles stack many layers deep.
//
// Constants bound by name; the registers only decide where they live, pinned clear of the game's (vertex: c0-c97 at
// most, skinned decals' Bones; pixel: Shadow.hlsl's c100-c167).

#ifdef PARTICLE_VS

float4 TESR_ParticleData : register(c200);            // x: strength, y: brightness, z: sun share, w: 1 outdoors
float4 TESR_ParticleAmbient : register(c201);         // rgb: ambient (the sky's outdoors, the cell's indoors)
float4 TESR_CameraPosition : register(c202);
float4 TESR_LightPosition[12] : register(c203);       // other point lights: xyz world position, w radius (0 = unused)
float4 TESR_ShadowLightPosition[12] : register(c215); // shadow-casting point lights, same layout
float4 TESR_LightColor[24] : register(c227);          // 0-11: shadow-casting lights, 12-23: the others; rgb, w dimmer

// Radius-based falloff, as WetWorld's point-light specular uses (lisyarus' smooth window).
float3 ParticlePointLight(float4 light, float4 color, float3 worldPos) {
    [branch] if (light.w <= 0.0f) return 0.0f;
    float s = saturate(dot(light.xyz - worldPos, light.xyz - worldPos) / (light.w * light.w));
    return color.rgb * color.w * ((1.0f - s) * (1.0f - s) / (1.0f + 5.0f * s));
}

// Ambient plus the point lights at a camera-relative position. Skipped (zero) while the strength is 0.
float3 ParticleVertexLight(float3 cameraRelativePos) {
    [branch] if (TESR_ParticleData.x <= 0.0f) return 0.0f;
    float3 light = TESR_ParticleAmbient.rgb;
    float3 worldPos = cameraRelativePos + TESR_CameraPosition.xyz;
    [loop] for (int i = 0; i < 12; i++) {
        light += ParticlePointLight(TESR_ShadowLightPosition[i], TESR_LightColor[i], worldPos);
        light += ParticlePointLight(TESR_LightPosition[i], TESR_LightColor[i + 12], worldPos);
    }
    return light;
}

#else

float4 TESR_ParticleData : register(c168); // as above
// UNOFFICIAL 2026-11-09 (roadmap track 3 bench): TESR_SunColor was pinned to c170, which HudRaindrops::SetDrawConstants writes on every
// geometry pass (zero when dry) after the shader's constants are uploaded: the sun term read black on every particle and blood decal.
// c192-c193: only bound by name elsewhere (ObjectDust, Skin), never written per draw.
float4 TESR_SunColor : register(c192);     // rgb: the weather's sun (directional) colour
#ifdef PARTICLE_SUN_PHASE
float4 TESR_ParticlePhase : register(c193); // (not c171: BarrelHeat writes c171-c173 per draw) xyz: toward the sun (world), w: Henyey-Greenstein anisotropy g (0 = the flat SunShare)

// UNOFFICIAL 2026-11-09 (roadmap track 3 step 2): how much of the sun a thin particle sends toward the camera, by the angle between
// the sun's light and the view ray. Henyey-Greenstein divided by its isotropic value 1 / (4 pi), so its average over all view
// directions is 1 and the average brightness stays the flat SunShare's: g > 0 glows with the sun behind the smoke and darkens it
// seen from the sun's side. g = 0 skips it (exactly the old flat share).
float ParticleSunPhase(float3 cameraRelativePos) {
    const float g = TESR_ParticlePhase.w;
    [branch] if (g == 0.0f) return 1.0f;
    // cos of the scattering angle: the sun's light travels along -L, leaves toward the camera along -viewRay
    const float cosTheta = dot(TESR_ParticlePhase.xyz, normalize(cameraRelativePos));
    const float denom = 1.0f + g * g - 2.0f * g * cosTheta;
    return (1.0f - g * g) / (denom * sqrt(denom));
}
float ParticleVolumetricShadow(float3 P, float radius, bool isBillboard) {
#if SHADOW_FIXED_MODE == 3 && FORWARD_SHADOWS && !(defined(INTERIOR_SHADOWS) && INTERIOR_SHADOWS)
    [branch] if (TESR_ShadowForwardData.x || !TESR_ShadowFormatData.z || !TESR_ShadowFade.y || TESR_ShadowFormatData.x != 3.0f)
        return 1.0f;
    float cascadeRad;
    row_major float4x4 lightTransform;
    float2 quad;
    if (length(P - TESR_ShadowNearCenter.xyz) < TESR_ShadowNearCenter.w) {
        cascadeRad = TESR_ShadowNearCenter.w;
        lightTransform = TESR_ShadowCameraToLightTransformNear;
        quad = float2(0.0f, 0.0f);
    } else if (length(P - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) {
        cascadeRad = TESR_ShadowMiddleCenter.w;
        lightTransform = TESR_ShadowCameraToLightTransformMiddle;
        quad = float2(0.5f, 0.0f);
    } else if (length(P - TESR_ShadowFarCenter.xyz) < TESR_ShadowFarCenter.w) {
        cascadeRad = TESR_ShadowFarCenter.w;
        lightTransform = TESR_ShadowCameraToLightTransformFar;
        quad = float2(0.0f, 0.5f);
    } else if (length(P - TESR_ShadowLodCenter.xyz) < TESR_ShadowLodCenter.w) {
        cascadeRad = TESR_ShadowLodCenter.w;
        lightTransform = TESR_ShadowCameraToLightTransformLod;
        quad = float2(0.5f, 0.5f);
    } else {
        return 1.0f;
    }
    float4 ls = mul(float4(P, 1.0f), lightTransform);
    ls.xyz /= ls.w;
    const float2 uv = float2(ls.x * 0.5f + 0.5f, ls.y * -0.5f + 0.5f) * 0.5f + quad;
    const float texW = max(TESR_ShadowBlur.x, 1.0f / 16384.0f);
    const float2 uvLo = quad + 0.5f * texW;
    const float2 uvHi = quad + 0.5f - 0.5f * texW;
    const float depth = ls.z - 0.0005f;
    const float uvRad = isBillboard ? clamp(max(radius, 24.0f) * 0.6f, 16.0f, 64.0f) / max(4.0f * cascadeRad, 1.0f) : 2.5f * texW;
    static const float2 kVolTaps[5] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.4f), float2(-1.0f, -0.4f), float2(-0.4f, 1.0f), float2(0.4f, -1.0f)
    };
    float lit = 0.0f;
    [unroll] for (int i = 0; i < 5; i++) {
        lit += depth <= tex2Dlod(TESR_ShadowAtlas, float4(clamp(uv + kVolTaps[i] * uvRad, uvLo, uvHi), 0.0f, 0.0f)).x ? 1.0f : 0.0f;
    }
    float sh = lerp(lit * 0.2f, 1.0f, saturate(TESR_ShadowFade.x));
#if SHADOW_CLOUD_SAMPLE
    [branch] if (NVR_CloudSun.w > 0.0f) sh *= CloudSunAt(P);
#endif
    return sh;
#else
    return GetSunShadowMaps(P, normalize(-P));
#endif
}

// The multiplier for the colour: the vertex light plus the sun through volumetric multiple-scattered shadow, times the brightness,
// blended in by the strength (0 = 1, the game's unlit look).
float3 ParticleLight(float3 vertexLight, float3 cameraRelativePos, float3 shadowPos, float radius, bool isBillboard) {
    [branch] if (TESR_ParticleData.x <= 0.0f) return 1.0f;
    float3 light = vertexLight;
    [branch] if (TESR_ParticleData.w > 0.0f) {
        const float sh = ParticleVolumetricShadow(shadowPos, radius, isBillboard);
        const float directPhase = lerp(1.0f, ParticleSunPhase(cameraRelativePos), sh);
        const float volVis = lerp(0.35f, 1.0f, sh);
        light += TESR_SunColor.rgb * (volVis * directPhase * TESR_ParticleData.z);
    }
    return lerp(1.0f, light * TESR_ParticleData.y, TESR_ParticleData.x);
}
#else
// Crisp direct-light path for surface decals (GDECAL.pso).
float3 ParticleLight(float3 vertexLight, float3 cameraRelativePos) {
    [branch] if (TESR_ParticleData.x <= 0.0f) return 1.0f;
    float3 light = vertexLight;
    [branch] if (TESR_ParticleData.w > 0.0f) {
        // Facing the camera; only the shadow bias uses it.
        light += TESR_SunColor.rgb * GetSunShadowMaps(cameraRelativePos, normalize(-cameraRelativePos)) * TESR_ParticleData.z;
    }
    return lerp(1.0f, light * TESR_ParticleData.y, TESR_ParticleData.x);
}
#endif

#endif
