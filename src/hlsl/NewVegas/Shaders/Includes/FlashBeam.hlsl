// UNOFFICIAL 2026-11-12 the flashlight's beam for what its light pass cannot light as itself ([Shaders.Flashlight.Deferred] HairAndEyes;
// core/FlashlightDeferred.h BeamBegin, where the findings are). The light pass (Shaders/FlashlightDeferred.hlsl) lights each pixel
// from the records of the draw that wrote them (base colour, material, normal, distance). The game's blended hair is drawn after that
// pass and had only the flashlight's game lamp (unshaped: no cone, no shadow) or no flashlight light at all; an eyeball's records were
// lit as a glossy object's, which the eye's own shader never does. The hair (SM3002 / SM3003.pso) and the eye variant
// (ObjectTemplate.hlsl NVR_EYE) take the beam themselves instead, as the grass does (GRASS23x000TMS.pso GrassSpot), with the pass's
// numbers: reach, Falloff, cone, NearFade, HotspotLimit, its shadow map (16 point taps on the pass's disc, as the grass) and, where the
// includer has a sampler for it, the lens pattern. Where they are drawn BEFORE the pass (the eye; alpha-tested hair) they mark their
// pixels in the records' distance (FLASH_BEAM_FAR: beyond any reach), so the pass leaves those pixels alone.
//
// The includer defines FLASH_BEAM_REG (5 registers); a pixel shader also FLASH_BEAM_MATRIX_REG (4 registers) and FLASH_BEAM_SAMPLER,
// and FLASH_BEAM_COOKIE_SAMPLER if it has a stage for the lens pattern; all free in it. A pixel shader includes
// Includes/LinearLight.hlsl first (LampAtt), a vertex shader Includes/Shadow.hlsl (the camera matrices).
// TESR_FlashBeam is sent with the shader. It holds the beam only in the world's own pass, from its main list's start, in a frame the
// light pass lights; at any other time (other views, the light or the key off, path tracing) [0].w is 0 and nothing here does anything:
//   [0] xyz the light, camera-relative, w its reach (0: off)
//   [1] xyz the beam's axis, w cos of the cone's outer edge
//   [2] rgb the light, linear (colour x Dimmer x the lamps' light scale), w cos of the full-strength edge
//   [3] x 1 = its shadow map was drawn this frame, y the map's texel size, z the depth bias (share of the reach), w PCF radius (texels)
//   [4] x Falloff (0: the lamps' falloff), y NearFade (units, 0 off), z HotspotLimit (0 off), w the lens pattern's strength (0 none)
#ifndef NVR_FLASH_BEAM_HLSL
#define NVR_FLASH_BEAM_HLSL

float4 TESR_FlashBeam[5] : register(FLASH_BEAM_REG);
#define FLASH_BEAM_ON (TESR_FlashBeam[0].w > 0.0f)
// The records' distance for a pixel that takes the beam itself: past any reach the light pass draws with (the pass skips the pixel by
// its own reach test), and still far when the game's MSAA averages it with a neighbour's real distance. A half float holds it.
#define FLASH_BEAM_FAR 60000.0f

#ifdef FLASH_BEAM_MATRIX_REG	// a pixel shader

// Camera-relative world -> the shadow map's clip space (ShaderManager's SpotLightWorldToLightMatrix[0], as the grass reads it).
row_major float4x4 TESR_SpotLightToWorldTransform : register(FLASH_BEAM_MATRIX_REG);
// One line each (ShaderRecord reads a sampler's states from the line of its register). The POINT lamp shaders are compiled with POINT
// defined (empty), which would empty the filter names: lifted round these lines (as Includes/Shadow.hlsl TESR_CharShadowMap).
#ifdef POINT
    #undef POINT
    #define NVR_FLASH_BEAM_POINT_WAS_DEFINED
#endif
sampler2D TESR_ShadowSpotlightBuffer0 : register(FLASH_BEAM_SAMPLER) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
#ifdef FLASH_BEAM_COOKIE_SAMPLER
// (the light pass's own texture, Effects\Flashlight.png, by name: it is there once the pass has loaded it for a Cookie above 0)
sampler2D TESR_FlashBeamCookie : register(FLASH_BEAM_COOKIE_SAMPLER) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#endif
#ifdef NVR_FLASH_BEAM_POINT_WAS_DEFINED
    #define POINT
    #undef NVR_FLASH_BEAM_POINT_WAS_DEFINED
#endif

// The shadow map's lit share at a camera-relative point (r of the map: the distance to the light over its reach): 16 point taps on a
// Vogel spiral over the pass's disc (FlashlightDeferred.hlsl FLShadow; point taps as the grass's). 1 outside the map.
float FlashBeamShadow(float3 position, float distShare) {
    const float4 clipPos = mul(float4(position, 1.0f), TESR_SpotLightToWorldTransform);
    const float2 mapUV = clipPos.xy / max(clipPos.w, 1.0e-4f) * float2(0.5f, -0.5f) + 0.5f;
    [branch] if (clipPos.w <= 0.0f || any(mapUV < 0.0f) || any(mapUV > 1.0f)) return 1.0f;
    const float radius = TESR_FlashBeam[3].y * TESR_FlashBeam[3].w;
    const float bias = TESR_FlashBeam[3].z * (1.0f + 4.0f * distShare);
    float2 dir = float2(1.0f, 0.0f);
    const float2x2 golden = float2x2(-0.7373688f, -0.6754904f, 0.6754904f, -0.7373688f);	// a turn by the golden angle
    float lit = 0.0f;
    [unroll] for (int i = 0; i < 16; i++) {
        lit += tex2Dlod(TESR_ShadowSpotlightBuffer0, float4(mapUV + dir * (radius * sqrt((i + 0.5f) / 16.0f)), 0.0f, 0.0f)).r + bias > distShare ? 1.0f : 0.0f;
        dir = mul(golden, dir);
    }
    return lit / 16.0f;
}

// How much of the beam's light ([2].rgb) reaches a camera-relative point: the light pass's falloff (its Falloff shape, or the lamps'),
// cone and NearFade through the lamps' falloff decode (LampAtt, as the pass's getPointLightLightingAtt), times HotspotLimit's
// roll-off, the shadow and the lens pattern. 0 outside the beam. toLight: the vector from the point to the light.
float FlashBeamStrength(float3 position, out float3 toLight) {
    toLight = TESR_FlashBeam[0].xyz - position;
    const float dist2 = dot(toLight, toLight);
    const float dist = sqrt(dist2);
    const float share2 = dist2 / (TESR_FlashBeam[0].w * TESR_FlashBeam[0].w);
    const float window = saturate(1.0f - share2);
    const float falloff = TESR_FlashBeam[4].x > 0.0f ? window * window / (1.0f + TESR_FlashBeam[4].x * share2) : window;
    const float along = dot(-toLight, TESR_FlashBeam[1].xyz);
    const float cone = smoothstep(TESR_FlashBeam[1].w, TESR_FlashBeam[2].w, along / max(dist, 1.0e-3f));
    const float shape = falloff * cone * cone;
    float strength = 0.0f;
    [branch] if (shape > 0.0f) {
        strength = LampAtt(shape * (TESR_FlashBeam[4].y > 0.0f ? smoothstep(0.0f, TESR_FlashBeam[4].y, dist) : 1.0f));
        [flatten] if (TESR_FlashBeam[4].z > 0.0f) strength *= 1.0f / max(1.0f, shape * TESR_FlashBeam[4].z);	// limit / max(limit, shape), limit = 1 / HotspotLimit
        [branch] if (TESR_FlashBeam[3].x > 0.5f) strength *= FlashBeamShadow(position, dist / TESR_FlashBeam[0].w);
        #ifdef FLASH_BEAM_COOKIE_SAMPLER
        [branch] if (TESR_FlashBeam[4].w > 0.0f) {	// the pass's pattern: its rim (r = 1) on the cone's outer edge, whichever way the beam points
            float3 side = cross(float3(0.0f, 0.0f, 1.0f), -TESR_FlashBeam[1].xyz);
            side /= max(length(side), 1.0e-4f);
            const float3 upAxis = cross(-TESR_FlashBeam[1].xyz, side);
            const float invTan = TESR_FlashBeam[1].w / sqrt(max(1.0f - TESR_FlashBeam[1].w * TESR_FlashBeam[1].w, 1.0e-6f));
            const float2 cookieUV = float2(dot(-toLight, side), -dot(-toLight, upAxis)) * (invTan / max(along, 1.0e-3f)) * 0.5f + 0.5f;
            strength *= lerp(1.0f, tex2Dlod(TESR_FlashBeamCookie, float4(cookieUV, 0.0f, 0.0f)).r * 2.0f, saturate(TESR_FlashBeam[4].w));
        }
        #endif
    }
    return strength;
}

// A steady number in 0-1 per pixel (interleaved gradient noise), for the hair's mark: hair blends over what is behind it, so a pixel
// is marked with the chance of its own alpha. Over a few pixels the light pass's light on the surface behind is then cut by the
// hair's cover, as the blend cuts the surface's own colour, with no line where the alpha crosses a fixed level.
float FlashBeamDither(float2 vpos) {
    return frac(52.9829189f * frac(dot(vpos, float2(0.06711056f, 0.00583715f))));
}

#else	// a vertex shader

// A camera-relative world vector in the draw's object axes (times the object's scale squared: only its direction is right), for a
// pixel shader that lights in object or tangent space. objectToClip: the draw's own matrix (row_major, mul(matrix, position)). Each
// object axis goes through the inverse projection and the inverse view as a DIRECTION (w 0: no divide; its view z is the clip w,
// whatever near and far planes the draw was projected with), as Shadow.hlsl GetShadowWorldPos takes a position.
float3 FlashBeamAxisToWorld(float4 clipAxis) {
    const float3 viewAxis = mul(clipAxis, TESR_InvProjectionTransform).xyz;
    return mul(float4(viewAxis, 0.0f), TESR_InvViewTransform).xyz;
}
float3 FlashBeamToObject(float3 worldVector, float4x4 objectToClip) {
    const float3 axisX = FlashBeamAxisToWorld(mul(objectToClip, float4(1.0f, 0.0f, 0.0f, 0.0f)));
    const float3 axisY = FlashBeamAxisToWorld(mul(objectToClip, float4(0.0f, 1.0f, 0.0f, 0.0f)));
    const float3 axisZ = FlashBeamAxisToWorld(mul(objectToClip, float4(0.0f, 0.0f, 1.0f, 0.0f)));
    return float3(dot(worldVector, axisX), dot(worldVector, axisY), dot(worldVector, axisZ));
}

#endif

#endif
