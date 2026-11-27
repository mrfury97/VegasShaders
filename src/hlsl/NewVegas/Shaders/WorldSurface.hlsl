// UNOFFICIAL 2026-11-24 WorldSurface (core/WorldSurface.h): world-space micro-detail grain and physical weathering
// (Mojave crevice dust, exposed metal rust, waterline dampness/mud/algae, and mossy rock growth) applied to deferred
// static geometry (class 1) and terrain (class 4) before DeferredSun, DeferredLamps, and FlashlightDeferred.
// Updates COLOR0 (baked ambient delta), COLOR1 (linear G-buffer albedo), and COLOR2 (G-buffer v2 material, roughness, normal).
// Automatically skips actors (2, 6, 8, 9), cloth (3), plants/trees (5), grass (7), unlit/emissives (10), and sky/forward pixels.

#include "Includes/DeferredDepth.hlsl"

float4 WS_Cam[6]   : register(c0);	// DeferredDepth::Frame[0..5] (camera axes, tan half-fov, viewport, position, jitter)
float4 WS_World    : register(c6);	// xyz = camPos (unwrapped world coords), w = isExterior (1/0)
float4 WS_Weights  : register(c7);	// x = detail, y = dust, z = rust, w = moss (already multiplied by strength)
float4 WS_Water    : register(c8);	// x = waterHeight - camPos.z (-100000 = none), y = waterline, z = rainWetness, w = DebugView
float4 WS_Fade     : register(c9);	// x = fadeStart, y = fadeEnd, z = detailEnd, w = isSnow (1/0)
float4 WS_Gamma    : register(c10);	// x = gamma, y = 1/gamma, z = isLinear (1/0), w = depthMode (0=normals.b, 1=linear view Z, 2=raw INTZ)
float4 WS_Ambient  : register(c11);	// xyz = flat outdoor ambient (linear * AmbientScale), w = skyScale
float4 WS_SkySH[9] : register(c12);	// TESR_SkyIrradianceObjects[0..8] (zeroed indoors)

sampler2D WS_Scene    : register(s0);	// CopyTex[0]: pre-pass COLOR0 scene (baked ambient + emissive)
sampler2D WS_Albedo   : register(s1);	// CopyTex[1]: pre-pass COLOR1 G-buffer albedo
sampler2D WS_Material : register(s2);	// CopyTex[2]: pre-pass COLOR2 G-buffer material (GBufferV2)
sampler2D WS_Normals  : register(s3);	// TESR_CapturedNormalsBuffer: rg smooth normal, b camera distance
sampler2D WS_Depth    : register(s4);	// TESR_DeferredDepthLinear or TESR_DeferredDepth (FP32 exact position)
sampler3D WS_Noise    : register(s5);	// Data\Textures\Effects\NoiseVolume.dds (64^3 seamless: r=Perlin, g=4-oct Perlin FBM, b=3-oct Worley FBM, a=indep Perlin)

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };
struct PS_OUT { float4 scene : COLOR0; float4 albedo : COLOR1; float4 material : COLOR2; };

float3 WSDecode(float2 e) {
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}

float2 WSEncode(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
    float2 e = n.xy;
    if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return e;
}

float3 WSCoat(float3 coatCol, float cLuma, float preserve) {
    const float coatLuma = max(dot(coatCol, float3(0.2126f, 0.7152f, 0.0722f)), 0.05f);
    const float det = clamp(cLuma / coatLuma, 0.35f, 1.85f);
    return coatCol * lerp(1.0f, det, preserve);
}

PS_OUT main(PS_IN IN) {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 scene = tex2Dlod(WS_Scene, uv4);
    const float4 albedo = tex2Dlod(WS_Albedo, uv4);
    const float4 m = tex2Dlod(WS_Material, uv4);
    const float4 normals = tex2Dlod(WS_Normals, uv4);

    clip(albedo.a - 1.0001f);	// deferred pixels only (forward/sky keep their own lighting)
    clip(m.r - 0.5f);			// written G-buffer pixel

    float model = floor(m.r + 0.01f);
    const float specular = model > 15.5f ? 16.0f : 0.0f;
    model -= specular;
    const bool isStatic = abs(model - 1.0f) < 0.5f;
    const bool isLand = abs(model - 4.0f) < 0.5f;
    clip((isStatic || isLand) ? 1.0f : -1.0f);

    const float3 ray = DD_CAMERA_RELATIVE_Z(WS_Cam, IN.vpos, 1.0f);
    const float rayInvLen = rsqrt(max(dot(ray, ray), 1.0e-8f));
    const float storedZ = normals.b * rayInvLen;
    float viewZ = storedZ;
    if (WS_Gamma.w > 1.5f) {
        const float devZ = DD_VIEW_Z(WS_Cam, DD_DEVICE_DEPTH(WS_Cam, WS_Depth, IN.vpos));
        if (abs(devZ - storedZ) <= storedZ * (1.0f / 256.0f) + 1.0f) viewZ = devZ;
    } else if (WS_Gamma.w > 0.5f) {
        const float linZ = DD_LINEAR_VIEW_Z(WS_Cam, WS_Depth, IN.vpos);
        if (abs(linZ - storedZ) <= storedZ * (1.0f / 256.0f) + 1.0f) viewZ = linZ;
    }
    const float3 P = ray * viewZ;
    const float dist = length(P);
    clip(WS_Fade.y - dist);
    const float distFade = 1.0f - smoothstep(WS_Fade.x, WS_Fade.y, dist);
    clip(distFade - 1.0e-4f);
    const float3 W = WS_World.xyz + P;

    const float origMetal = saturate((m.r - floor(m.r + 0.004f)) / 0.49f);
    const float origRough = m.b < -1.5f ? -2.0f - m.b : (m.b < 0.0f ? 1.0f : m.b);
    const bool hasN = abs(m.g) <= 1.001f;
    const float3 Ns = WSDecode(normals.rg);
    float3 N = hasN ? WSDecode(m.ga) : Ns;

    float metal = origMetal;
    float rough = origRough;
    float3 a = albedo.rgb;
    const float3 kLumaW = float3(0.2126f, 0.7152f, 0.0722f);
    const float cLuma = dot(albedo.rgb, kLumaW);

    // Screen-space 4-tap depth-guarded edge, concavity, and texture relief analysis
    const float2 stepUV = WS_Cam[5].zw * 2.0f;
    const float4 nL = tex2Dlod(WS_Normals, float4(IN.uv - float2(stepUV.x, 0.0f), 0.0f, 0.0f));
    const float4 nR = tex2Dlod(WS_Normals, float4(IN.uv + float2(stepUV.x, 0.0f), 0.0f, 0.0f));
    const float4 nU = tex2Dlod(WS_Normals, float4(IN.uv - float2(0.0f, stepUV.y), 0.0f, 0.0f));
    const float4 nD = tex2Dlod(WS_Normals, float4(IN.uv + float2(0.0f, stepUV.y), 0.0f, 0.0f));
    const float4 aL = tex2Dlod(WS_Albedo, float4(IN.uv - float2(stepUV.x, 0.0f), 0.0f, 0.0f));
    const float4 aR = tex2Dlod(WS_Albedo, float4(IN.uv + float2(stepUV.x, 0.0f), 0.0f, 0.0f));
    const float4 aU = tex2Dlod(WS_Albedo, float4(IN.uv - float2(0.0f, stepUV.y), 0.0f, 0.0f));
    const float4 aD = tex2Dlod(WS_Albedo, float4(IN.uv + float2(0.0f, stepUV.y), 0.0f, 0.0f));

    const float zTol = dist * 0.035f + 5.0f;
    const float wL = 1.0f - smoothstep(zTol * 0.5f, zTol, abs(nL.b - normals.b));
    const float wR = 1.0f - smoothstep(zTol * 0.5f, zTol, abs(nR.b - normals.b));
    const float wU = 1.0f - smoothstep(zTol * 0.5f, zTol, abs(nU.b - normals.b));
    const float wD = 1.0f - smoothstep(zTol * 0.5f, zTol, abs(nD.b - normals.b));
    const float4 wVec = float4(wL, wR, wU, wD);
    const float wSum = max(dot(wVec, 1.0f), 1.0e-4f);

    const float4 dzNbr = normals.b - float4(nL.b, nR.b, nU.b, nD.b);
    const float4 contactVec = smoothstep(0.6f, 6.0f, dzNbr) * (1.0f - smoothstep(14.0f, 34.0f, dzNbr));
    const float contact = dot(contactVec, 1.0f) * 0.45f;

    const float3 NsL = WSDecode(nL.rg);
    const float3 NsR = WSDecode(nR.rg);
    const float3 NsU = WSDecode(nU.rg);
    const float3 NsD = WSDecode(nD.rg);
    const float curvX = dot(NsL - NsR, WS_Cam[0].xyz) * min(wL, wR);
    const float curvY = dot(NsD - NsU, WS_Cam[1].xyz) * min(wU, wD);
    const float curv = curvX + curvY;
    const float geoCrevice = saturate(max(curv * 2.2f, 0.0f) + contact);
    const float geoEdge = saturate(-curv * 2.2f);

    const float4 defNbr = float4(aL.a > 1.0001f ? 1.0f : 0.0f, aR.a > 1.0001f ? 1.0f : 0.0f, aU.a > 1.0001f ? 1.0f : 0.0f, aD.a > 1.0001f ? 1.0f : 0.0f);
    const float validNbr = dot(defNbr, wVec) / wSum;
    const float boundaryFeather = lerp(0.35f, 1.0f, validNbr);

    const float4 aDefW = wVec * defNbr;
    const float aDefSum = dot(aDefW, 1.0f);
    const float avgLuma = aDefSum > 1.0e-4f
        ? (dot(aL.rgb, kLumaW) * aDefW.x + dot(aR.rgb, kLumaW) * aDefW.y + dot(aU.rgb, kLumaW) * aDefW.z + dot(aD.rgb, kLumaW) * aDefW.w) / aDefSum
        : cLuma;
    const float relRelief = (avgLuma - cLuma) / max(avgLuma, 0.08f);
    const float localDip = saturate(relRelief * 2.2f);
    const float localHigh = saturate(-relRelief * 2.0f);

    const float up = saturate(N.z * 1.25f + 0.10f);
    const float bumpCrev = saturate((1.0f - dot(N, Ns)) * 5.0f);
    const float allCrevice = saturate(max(bumpCrev, geoCrevice) + localDip * 0.45f);
    const float exposedEdge = saturate(geoEdge + localHigh * 0.35f);

    // Orthonormal world-space rotations + domain-warped hardware 3D noise (NoiseVolume.dds: r=Perlin, g=4-oct FBM, b=Worley FBM, a=Perlin2)
    const float3 w1 = float3(0.80f * W.x - 0.60f * W.y, 0.60f * W.x + 0.80f * W.y, W.z);
    const float3 w2 = float3(0.60f * W.x + 0.48f * W.y - 0.64f * W.z, -0.80f * W.x + 0.36f * W.y - 0.48f * W.z, 0.80f * W.y + 0.60f * W.z);
    const float3 w3 = float3(-0.36f * W.x + 0.80f * W.y + 0.48f * W.z, 0.48f * W.x + 0.60f * W.y - 0.64f * W.z, -0.80f * W.x - 0.60f * W.z);

    const float4 nWarp  = tex3Dlod(WS_Noise, float4(w1 * 0.00115f, 0.0f));
    const float4 nBroad = tex3Dlod(WS_Noise, float4(w2 * 0.00240f + (nWarp.rga - 0.5f) * 0.16f + float3(0.17f, 0.43f, 0.61f), 0.0f));
    const float4 nMid   = tex3Dlod(WS_Noise, float4(w3 * 0.00950f + (nBroad.rab - 0.5f) * 0.12f + float3(0.53f, 0.11f, 0.79f), 0.0f));

    const float nCoarse = nBroad.g * 0.65f + nWarp.g * 0.35f;
    const float nBillow = nBroad.b * 0.60f + nMid.b * 0.40f;
    const float nMidFbm = nMid.g;
    const float nFine   = nMid.r * 0.45f + nMid.g * 0.35f + nMid.a * 0.20f;

    const float detailFade = (1.0f - smoothstep(WS_Fade.z * 0.30f, WS_Fade.z, dist)) * distFade;
    float4 nMicro = float4(0.5f, 0.5f, 0.5f, 0.5f);
    [branch] if (detailFade > 0.01f) {
        const float microLod = clamp(log2(max(dist * 0.0075f, 1.0f)), 0.0f, 3.0f);
        nMicro = tex3Dlod(WS_Noise, float4(w1 * 0.052f + (nMid.rga - 0.5f) * 0.08f + float3(0.29f, 0.67f, 0.41f), microLod));
    }
    const float g0 = nMicro.g;
    const float g1 = nMicro.a * 0.55f + nMicro.b * 0.45f;

    // 1. High-frequency micro-detail grain & roughness breakup
    float detailVis = 0.0f;
    [branch] if (WS_Weights.x > 0.001f && detailFade > 0.01f) {
        const float detailAmt = saturate(WS_Weights.x) * detailFade * boundaryFeather;
        const float normalAmp = detailAmt * (isLand ? 0.26f : lerp(0.20f, 0.045f, metal));
        N = normalize(N + float3(g0 - 0.5f, g1 - 0.5f, (nFine - 0.5f) * 0.6f) * normalAmp);
        rough = saturate(rough + (g0 - 0.5f) * 0.11f * detailAmt + (nMidFbm - 0.5f) * 0.06f * detailAmt - geoEdge * metal * 0.12f * detailAmt);
        a *= 1.0f + (g1 - 0.5f) * lerp(0.08f, 0.025f, metal) * detailAmt;
        detailVis = detailAmt;
    }

    // 2. Waterline height-band masks (evaluated early so splashZone/wetBand inform dust, rust, and moss)
    float wetBand = 0.0f, mudW = 0.0f, algaeW = 0.0f, splashZone = 0.0f;
    [branch] if (WS_Water.y > 0.001f && WS_Water.x > -50000.0f) {
        const float hWater = P.z - WS_Water.x + (nCoarse - 0.5f) * 18.0f + (nMidFbm - 0.5f) * 7.0f;
        [branch] if (hWater > -25.0f && hWater < 95.0f) {
            const float wScale = saturate(WS_Water.y) * distFade;
            wetBand = smoothstep(-25.0f, -2.0f, hWater) * (1.0f - smoothstep(10.0f, 34.0f, hWater)) * wScale;
            splashZone = smoothstep(0.0f, 18.0f, hWater) * (1.0f - smoothstep(40.0f, 85.0f, hWater)) * wScale;

            const float mudBand = smoothstep(6.0f, 18.0f, hWater) * (1.0f - smoothstep(38.0f, 62.0f, hWater)) * wScale;
            mudW = mudBand * smoothstep(0.32f, 0.68f, nMidFbm * 0.50f + nFine * 0.30f + allCrevice * 0.25f) * (1.0f - exposedEdge * 0.45f);

            const float algaeBand = smoothstep(14.0f, 32.0f, hWater) * (1.0f - smoothstep(65.0f, 92.0f, hWater)) * wScale;
            const float algaeSeat = saturate(0.18f + 0.50f * up + 0.55f * allCrevice) * (1.0f - exposedEdge * 0.65f);
            algaeW = algaeBand * algaeSeat * smoothstep(0.42f, 0.78f, nCoarse * 0.34f + nBillow * 0.36f + nMidFbm * 0.30f);
        }
    }

    // 3. Mojave crevice dust & ledge silt (outdoors: warm arid silt; indoors: subtle aged crevice dust)
    float dustW = 0.0f;
    [branch] if (WS_Weights.y > 0.001f) {
        const float dustEnv = WS_World.w > 0.5f ? (lerp(1.0f, 0.35f, WS_Fade.w) * (isLand ? 0.65f : 0.90f)) : 0.45f;
        const float dustSeat = WS_World.w > 0.5f ? max(allCrevice * 1.15f, up * 0.70f) : (allCrevice * 1.30f + up * 0.25f);
        const float dustBreak = nCoarse * 0.38f + nMidFbm * 0.30f + nFine * 0.20f + g0 * 0.12f;
        const float dustMask = smoothstep(0.46f, 0.78f, dustBreak + dustSeat * 0.34f - 0.16f) * (1.0f - exposedEdge * 0.65f);
        const float metalDustGate = lerp(1.0f, saturate(0.20f + 0.80f * max(allCrevice, up)), metal);
        dustW = saturate(WS_Weights.y * 0.72f) * dustEnv * dustMask * metalDustGate * distFade;
    }

    // 4. Exposed metal tarnish & pitted rust (anchored to crevices/dips/edges/splash; pulls back on already-rusted metal)
    float tarnishW = 0.0f, rustW = 0.0f;
    [branch] if (WS_Weights.z > 0.001f && isStatic && metal > 0.28f) {
        const float alreadyRusty = smoothstep(0.035f, 0.14f, albedo.r - albedo.b) * smoothstep(0.015f, 0.09f, albedo.r - albedo.g * 0.68f);
        const float cleanMetalGate = 1.0f - alreadyRusty * 0.85f;
        const float rustSeat = saturate(allCrevice * 0.78f + geoEdge * 0.32f + up * 0.24f + splashZone * 0.45f);
        const float rustZone = smoothstep(0.40f, 0.76f, nCoarse * 0.45f + nBillow * 0.25f + nMidFbm * 0.30f + rustSeat * 0.45f - 0.26f);
        const float rustEdge = nFine * 0.44f + g0 * 0.36f + g1 * 0.20f;
        const float rustFlake = smoothstep(0.48f, 0.68f, rustZone * 0.72f + rustEdge * 0.46f - 0.18f);
        const float rustPit = smoothstep(0.64f, 0.80f, g0 * 0.55f + g1 * 0.45f) * rustZone;
        const float rustCarrier = saturate(rustFlake * 0.85f + rustPit * 0.45f);
        const float rustGate = smoothstep(0.28f, 0.62f, metal) * saturate(WS_Weights.z) * cleanMetalGate * distFade;
        tarnishW = rustGate * smoothstep(0.18f, 0.65f, rustZone);
        rustW = rustGate * rustCarrier;
    }

    // 5. Patchy moss & lichen on shaded/upward rocks, ruins, and crevices (outdoors)
    float mossW = 0.0f;
    [branch] if (WS_Weights.w > 0.001f && WS_World.w > 0.5f) {
        const float mossSeat = saturate(up * 0.60f + allCrevice * 0.90f) * saturate(0.55f - 0.35f * N.y) * (1.0f - metal * 0.90f) * (1.0f - exposedEdge * 0.70f);
        const float mossBreak = smoothstep(0.48f, 0.78f, nBillow * 0.38f + nCoarse * 0.28f + nMidFbm * 0.22f + g0 * 0.12f + mossSeat * 0.25f - 0.22f);
        const float rainBoost = 1.0f + 0.35f * WS_Water.z + 0.30f * wetBand;
        mossW = saturate(WS_Weights.w * rainBoost) * mossSeat * mossBreak * distFade;
    }

    // Cross-effect competition, boundary feathering, and deposit normalization
    dustW *= (1.0f - wetBand * 0.90f) * (1.0f - mossW * 0.85f) * (1.0f - mudW * 0.80f) * (1.0f - algaeW * 0.85f);
    mossW *= (1.0f - wetBand * 0.50f) * (1.0f - rustW * 0.65f);
    rustW *= (1.0f - algaeW * 0.65f) * (1.0f - mudW * 0.70f);

    dustW *= boundaryFeather;
    tarnishW *= boundaryFeather;
    rustW *= boundaryFeather;
    mossW *= boundaryFeather;
    mudW *= boundaryFeather;
    algaeW *= boundaryFeather;
    wetBand *= boundaryFeather;

    const float totalW = dustW + rustW + mossW + mudW + algaeW;
    const float invTotal = totalW > 1.0f ? (1.0f / totalW) : 1.0f;
    dustW *= invTotal;
    rustW *= invTotal;
    mossW *= invTotal;
    mudW *= invTotal;
    algaeW *= invTotal;

    // Composite tarnish patina halo first (smooths chrome-to-rust transitions)
    [branch] if (tarnishW > 1.0e-3f) {
        a *= lerp(1.0f, float3(0.84f, 0.80f, 0.76f), tarnishW * 0.45f);
        rough = lerp(rough, max(rough, 0.62f), tarnishW * 0.55f);
        metal *= 1.0f - tarnishW * 0.30f;
    }

    // Composite normalized weathering deposits (coat-not-paint detail preservation)
    [branch] if (dustW > 1.0e-3f) {
        const float3 outDustCol = lerp(float3(0.30f, 0.23f, 0.15f), float3(0.42f, 0.33f, 0.22f), nFine);
        const float3 rawDustCol = WS_World.w > 0.5f ? outDustCol : float3(0.22f, 0.21f, 0.19f);
        const float3 dustCol = WSCoat(rawDustCol, cLuma, 0.45f);
        a = lerp(a, dustCol, dustW * 0.42f);
        rough = lerp(rough, 0.90f, dustW * 0.72f);
        metal *= 1.0f - dustW * 0.65f;
        N = normalize(lerp(N, Ns, dustW * 0.25f));
    }

    [branch] if (rustW > 1.0e-3f) {
        const float3 rawRustCol = lerp(float3(0.075f, 0.030f, 0.014f), float3(0.175f, 0.068f, 0.024f), saturate(g0 * 0.50f + nFine * 0.50f));
        const float3 rustCol = WSCoat(rawRustCol, cLuma, 0.50f);
        a = lerp(a, rustCol, rustW * 0.48f);
        rough = lerp(rough, 0.88f, rustW * 0.82f);
        metal *= 1.0f - rustW * 0.78f;
        N = normalize(N + float3(g0 - 0.5f, g1 - 0.5f, 0.0f) * (0.24f * rustW));
    }

    [branch] if (mudW > 1.0e-3f) {
        const float3 mudCol = WSCoat(float3(0.055f, 0.042f, 0.026f), cLuma, 0.40f);
        a = lerp(a, mudCol, mudW * 0.68f);
        rough = lerp(rough, 0.58f, mudW * 0.65f);
        metal *= 1.0f - mudW * 0.80f;
    }

    [branch] if (algaeW > 1.0e-3f) {
        const float3 algaeCol = WSCoat(float3(0.075f, 0.125f, 0.042f), cLuma, 0.45f);
        a = lerp(a, algaeCol, algaeW * 0.60f);
        rough = lerp(rough, 0.78f, algaeW * 0.70f);
        metal *= 1.0f - algaeW * 0.80f;
    }

    [branch] if (mossW > 1.0e-3f) {
        const float3 rawMossCol = lerp(float3(0.08f, 0.125f, 0.042f), float3(0.16f, 0.20f, 0.072f), nFine);
        const float3 mossCol = WSCoat(rawMossCol, cLuma, 0.45f);
        a = lerp(a, mossCol, mossW * 0.72f);
        rough = lerp(rough, 0.88f, mossW * 0.85f);
        metal *= 1.0f - mossW * 0.90f;
        N = normalize(N + float3(nFine - 0.5f, nMidFbm - 0.5f, 0.0f) * (0.22f * mossW));
    }

    [branch] if (wetBand > 1.0e-3f) {
        a *= lerp(1.0f, lerp(0.48f, 0.85f, metal), wetBand * 0.90f);
        rough = lerp(rough, 0.09f, wetBand * 0.90f);
        N = normalize(lerp(N, Ns, wetBand * 0.35f));
    }

    const float dust = dustW;
    const float rust = saturate(rustW + tarnishW * 0.25f);
    const float moss = mossW;
    const float waterlineVis = saturate(max(wetBand, max(mudW, algaeW)));

    // Ambient re-lighting on COLOR0 (mirrors DeferredFirstPerson::kResolve)
    const float chg = dot(abs(a - albedo.rgb), 1.0f) + abs(metal - origMetal) + abs(rough - origRough);
    float3 outScene = scene.rgb;
    [branch] if (chg > 1.0e-4f) {
        const float3 sLin = WS_Gamma.z > 0.5f ? max(scene.rgb, 0.0f) : pow(max(scene.rgb, 0.0f), WS_Gamma.x);
        const float3 shE = WS_SkySH[0].rgb + WS_SkySH[1].rgb * Ns.y + WS_SkySH[2].rgb * Ns.z + WS_SkySH[3].rgb * Ns.x
            + WS_SkySH[4].rgb * (Ns.x * Ns.y) + WS_SkySH[5].rgb * (Ns.y * Ns.z) + WS_SkySH[6].rgb * (3.0f * Ns.z * Ns.z - 1.0f)
            + WS_SkySH[7].rgb * (Ns.x * Ns.z) + WS_SkySH[8].rgb * (Ns.x * Ns.x - Ns.y * Ns.y);
        const float3 skyTerm = pow(max(shE, 0.0f), WS_Gamma.x * 0.5f) * WS_Ambient.w;
        float3 ambE = WS_Ambient.rgb + skyTerm;
        const float3 oDiff = albedo.rgb * (1.0f - origMetal);
        const float3 nDiff = a * (1.0f - metal);
        if (max(ambE.r, max(ambE.g, ambE.b)) < 1.0e-4f) {
            const float3 est = sLin / max(oDiff, 0.06f);
            ambE = clamp(est, 0.015f, 0.35f);
        }
        const float metFade = origMetal > 0.02f ? lerp(1.0f, saturate(metal / origMetal), 0.65f * origMetal) : 1.0f;
        const float glossGain = saturate(origRough - rough) * (1.0f - rough);
        const float3 skySheen = skyTerm * (0.14f * glossGain * (0.4f + 0.6f * saturate(Ns.z)));
        const float3 newLin = max(sLin * metFade + ambE * (nDiff - oDiff), 0.0f) + skySheen;
        outScene = WS_Gamma.z > 0.5f ? newLin : pow(max(newLin, 0.0f), WS_Gamma.y);
    }

    // DebugView (WorldSurfaceDebug 1): R=rust, G=moss, B=waterline, yellow=dust, subtle grey=detail
    [branch] if (WS_Water.w > 0.5f) {
        const float3 dbg = float3(rust + dust * 0.85f, moss + dust * 0.72f, waterlineVis) + detailVis * 0.14f;
        outScene = lerp(scene.rgb * 0.25f, dbg, saturate(max(max(rust, moss), max(waterlineVis, max(dust, detailVis * 0.5f)))));
    }

    PS_OUT o;
    const float2 e = hasN ? WSEncode(N) : m.ga;
    o.scene = float4(outScene, scene.a);
    o.albedo = float4(a, albedo.a);
    o.material = float4(model + specular + 0.49f * saturate(metal), e.x, saturate(rough), e.y);
    return o;
}
