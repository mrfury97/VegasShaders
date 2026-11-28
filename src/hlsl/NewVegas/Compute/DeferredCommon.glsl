// Shared G-buffer decode and PBR BRDF helpers for the deferred compute passes
// (DeferredSun.comp, DeferredLamps.comp, FlashlightDeferred.comp).
// Included immediately after HasFlag(uint bit) and the cb uniform block.

float shades(vec3 a, vec3 b) { return max(dot(a, b), 0.0); }

vec3 ShadowDecodeNormal(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

vec3 ShadowFacingNormal(vec3 n, vec3 worldPos) {
    float len2 = dot(n, n);
    n = len2 > 1.0e-6 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
    return n * (dot(n, worldPos) > 0.0 ? -1.0 : 1.0);
}

float GBufferRoughness(float b) {
    return (b >= 0.0 && b <= 1.0) ? b : (b <= -1.99 && b >= -3.01) ? (-2.0 - b) : 1.0;
}

float FDModel(float r) {
    const float m = floor(r + 0.01);
    if (HasFlag(kFlagGBufferV2)) return m >= 15.5 ? m - 16.0 : m;
    return m;
}

float FDLightFreeKeep(float a) {
    if (HasFlag(kFlagGBufferV2)) return saturate(a > 2.5 ? a - 3.0 : a - 1.0);
    const float base = a < 4.5 ? 3.0 : a < 6.5 ? 5.0 : 7.0;
    return saturate(a - base);
}

float FDAddedLightKeep(float a) { return FDLightFreeKeep(a); }

void FDSunVisDecode(float a, out float vis, out float f0) {
    const float q = (a > 0.99 && a < 2.005) ? floor(abs(a - (a > 1.5 ? 2.0 : 1.0)) * 1024.0 + 0.5) : 0.0;
    const float s = floor((q + 0.5) / 48.0);
    vis = 1.0 - (q - 48.0 * s) / 47.0;
    f0 = s > 0.5 ? (s - 1.0) * 0.01 : 0.04;
}

float FDSpecF0Decode(float a) {
    float vis, f0;
    FDSunVisDecode(a, vis, f0);
    return f0;
}

vec3 LinColor(vec3 c) {
    if (HasFlag(kFlagLinearLighting) && cb.linearLighting.x > 1.0) return pow(max(c, vec3(0.0)), vec3(cb.linearLighting.x));
    return c;
}

float LinScale(float s) {
    if (HasFlag(kFlagLinearLighting) && cb.linearLighting.x > 1.0) return pow(max(s, 0.0), cb.linearLighting.x);
    return s;
}

vec3 ReflectivityF0(vec3 f0) { return clamp(f0 + f0 * cb.pbrMath.y, 0.0, 1.0); }

vec3 RoughReflectivityF90(vec3 f0, float r, float metal) {
    const vec3 f90 = vec3(clamp(1.0 + cb.pbrMath.y, 0.0, 1.0));
    return max(f90 * (1.0 - clamp(r, 0.0, 1.0) * clamp(1.0 - metal, 0.0, 1.0)), f0);
}

vec3 Fresnel(vec3 f0, vec3 f90, float cosT) {
    float omc = 1.0 - cosT;
    float omc2 = omc * omc;
    return f0 + (f90 - f0) * (omc2 * omc2 * omc);
}

vec3 MultiScatterMetalBoost(vec3 f0, float r, float NdotV, float metal) {
    if (metal <= 0.001) return vec3(1.0);
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 mixed = r * c0 + c1;
    float a004 = min(mixed.x * mixed.x, exp2(-9.28 * NdotV)) * mixed.x + mixed.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + mixed.zw;
    float ess = clamp(ab.x + ab.y, 0.12, 1.0);
    vec3 comp = 1.0 + f0 * (1.0 / ess - 1.0);
    return mix(vec3(1.0), min(comp, vec3(2.2)), metal);
}

vec3 HueShift(vec3 c, float angle) {
    const vec3 k = vec3(0.57735027);
    return c * cos(angle) + cross(k, c) * sin(angle) + k * dot(k, c) * (1.0 - cos(angle));
}

vec3 ApplyPearlescence(vec3 col, float NdotV, float pearl, float metal) {
    if (pearl <= 0.001 || metal <= 0.05) return col;
    vec3 c = clamp(col, vec3(0.0), vec3(1.0));
    float fMax = max(c.r, max(c.g, c.b));
    float fMin = min(c.r, min(c.g, c.b));
    float sat = (fMax - fMin) / max(fMax, 1.0e-4);
    float satMask = smoothstep(0.10, 0.40, sat);
    float omv = clamp(1.0 - NdotV, 0.0, 1.0);
    float graze = pow(omv, 1.15);
    vec3 hueShifted = HueShift(c, -0.95 * pearl * (0.20 + 0.80 * graze));
    vec3 filmWave = 0.50 + 0.50 * cos(omv * 5.2 + vec3(0.0, 2.094395, 4.18879));
    float lum = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.14);
    vec3 thinFilm = mix(c, lum * (0.35 + 1.15 * filmWave), clamp(0.22 + 0.78 * graze, 0.0, 1.0));
    vec3 pearlCol = mix(thinFilm, hueShifted, satMask);
    return mix(col, clamp(pearlCol, vec3(0.0), vec3(1.0)), pearl * smoothstep(0.05, 0.40, metal));
}

vec3 PearlF90Tint(vec3 metalCol, float pearl, float metal) {
    if (pearl <= 0.001 || metal <= 0.05) return vec3(1.0);
    float pTop = max(metalCol.r, max(metalCol.g, metalCol.b));
    return mix(vec3(1.0), clamp(metalCol / max(pTop, 0.25), vec3(0.0), vec3(1.0)), pearl * smoothstep(0.05, 0.40, metal) * 0.75);
}

vec3 CandyCoatAbsorption(vec3 metalCol, float NdotV, float coatAmt, float metal) {
    if (coatAmt <= 0.001 || metal <= 0.05) return vec3(1.0);
    vec3 c = clamp(metalCol, vec3(0.0), vec3(1.0));
    float fMax = max(c.r, max(c.g, c.b));
    float fMin = min(c.r, min(c.g, c.b));
    float sat = (fMax - fMin) / max(fMax, 1.0e-4);
    float satMask = smoothstep(0.14, 0.45, sat);
    vec3 tCol = mix(vec3(1.0), c / max(fMax, 1.0e-4), satMask);
    float pathLen = (1.0 / max(NdotV, 0.28) - 1.0) * 0.55;
    float metalMask = smoothstep(0.05, 0.40, metal);
    return exp(-(vec3(1.0) - tCol) * (1.85 * clamp(coatAmt, 0.0, 1.0) * satMask * metalMask * pathLen));
}

float GGXPi(float nh, float r) {
    float r2 = r * r;
    float aSq = r2 * r2;
    float dTerm = max((nh * aSq - nh) * nh + 1.0, 1.0e-5);
    return aSq / (dTerm * dTerm);
}

vec3 BRDFPi(float r, vec3 F, float nv, float nl, float nh, bool allowSmith) {
    const float dPi = GGXPi(nh, r);
    if (allowSmith && (HasFlag(kFlagSmithCorrelated) || HasFlag(kFlagSmithCorrTest))) {
        const float r2 = r * r;
        const float aSq = r2 * r2;
        const float visV = nl * sqrt(nv * nv * (1.0 - aSq) + aSq);
        const float visL = nv * sqrt(nl * nl * (1.0 - aSq) + aSq);
        const vec3 corr = dPi * (0.5 / (visV + visL)) * F;
        if (!HasFlag(kFlagSmithCorrTest)) return corr;
        const float rp1 = r + 1.0;
        const float kk = (rp1 * rp1) * 0.125;
        const float invK = 1.0 - kk;
        const vec3 legacy = dPi * (0.25 / ((nv * invK + kk) * (nl * invK + kk))) * F;
        return legacy + (corr - legacy) * cb.pbrMath.x;
    }
    const float rp1 = r + 1.0;
    const float kk = (rp1 * rp1) * 0.125;
    const float invK = 1.0 - kk;
    const float vis = 0.25 / ((nv * invK + kk) * (nl * invK + kk));
    return dPi * vis * F;
}

vec3 BRDFPi(float r, vec3 F, float nv, float nl, float nh) {
    return BRDFPi(r, F, nv, nl, nh, true);
}

float SmithCorrelatedKeep(float NdotV, float roughness) {
    const float rc = max(roughness, 0.056);
    const float sq = sqrt(clamp(NdotV, 0.02, 1.0));
    const float q0 = 0.8720785 + sq * (2.377077 + sq * (-15.78768 + sq * (20.7581 + sq * (-8.469884 + sq * 0.2183369))));
    const float q1 = -2.350215 + sq * (6.164611 + sq * (12.53951 + sq * (-24.69559 + sq * 9.321004)));
    const float q2 = -4.584592 + sq * (-17.91134 + sq * (14.6047 + sq * 0.8975321));
    const float q3 = 20.67309 + sq * (6.171803 + sq * (-8.573712));
    const float q4 = -22.05094 + sq * 2.260031;
    const float q5 = 7.527356;
    const float surplus = q0 + rc * (q1 + rc * (q2 + rc * (q3 + rc * (q4 + rc * q5))));
    const float r2 = roughness * roughness;
    const float lossClamp = 1.0 - min(1.0e5 * r2 * r2, 1.0);
    return 1.0 - clamp(surplus * (1.0 - lossClamp * lossClamp), 0.0, 1.0);
}

float DiffuseEnergyKeep(float NdotV, float roughness) {
    if (HasFlag(kFlagSmithCorrTest)) return 1.0 + (SmithCorrelatedKeep(NdotV, roughness) - 1.0) * cb.pbrMath.x;
    if (HasFlag(kFlagSmithCorrelated)) return SmithCorrelatedKeep(NdotV, roughness);
    if (!HasFlag(kFlagDiffuseKeep)) return 1.0;
    const float tv = clamp((NdotV - 0.055) * (1.0 / 0.525), 0.0, 1.0);
    const float rx = clamp((0.53 - roughness) * (1.0 / 0.461), 0.0, 1.0);
    const float lowRough = clamp((roughness - 0.02) * (1.0 / 0.05), 0.0, 1.0);
    const float inv = 1.0 - tv;
    const float bump = rx * lowRough * tv * inv * inv * ((0.10380582 + 0.69366243 * rx) + tv * (0.27762281 - 0.88561283 * rx));
    return cb.pbrMath.x > 0.5 ? 1.0 - bump * (1.0 / 0.95) : 1.0;
}

#ifdef DEFERRED_HAS_SUN_AND_FOLIAGE
const float kSunRadius = 0.00918043;
const float kFdFoliageLeafBase = 16.0;
const float kFdFoliageGrassBase = 32.0;
const float kFdFoliageGrassTop = 1024.0;
const float kFoliageSunCode = 2.0;
const float kFoliageHighlightMax = 1.0;

bool FDSunDeferredPixel(float a, float r) {
    if (HasFlag(kFlagGBufferV2)) {
        const float model = FDModel(r);
        if (HasFlag(kFlagHairEnabled)) return a > 1.0001 && a < 2.0005 && (model < 8.5 || model > 10.5);
        return a > 1.0001 && a < 2.0005 && (model < 7.5 || model > 10.5);
    }
    return a > 2.5 && a < 6.5;
}

float FDSunSpecular(float a, float r) {
    if (HasFlag(kFlagGBufferV2)) return floor(r + 0.01) >= 15.5 ? 1.0 : 0.0;
    return a < 4.5 ? 1.0 : 0.0;
}

float FDFoliageIsGrass(float g) { return (g > kFdFoliageGrassBase - 0.01 && g < kFdFoliageGrassTop) ? 1.0 : 0.0; }

float FDFoliageGrassIndex(float g) {
    const float o = (g >= 63.99 ? 1.0 : 0.0) + (g >= 127.99 ? 1.0 : 0.0) + (g >= 255.99 ? 1.0 : 0.0) + (g >= 511.99 ? 1.0 : 0.0);
    const float p = kFdFoliageGrassBase * (o > 0.5 ? 2.0 : 1.0) * (o > 1.5 ? 2.0 : 1.0) * (o > 2.5 ? 2.0 : 1.0) * (o > 3.5 ? 2.0 : 1.0);
    return 1024.0 * o + floor((g / p - 1.0) * 1024.0 + 0.5);
}

float FDFoliageKeep(float g) {
    return g < kFdFoliageGrassBase - 0.01 ? saturate(floor((g - kFdFoliageLeafBase) * 64.0 + 0.5) / 1023.0)
                                          : floor(FDFoliageGrassIndex(g) / 40.0 + 1.0e-3) / 127.0;
}

float FDFoliageHighlightEnc(float g) {
    const float i = FDFoliageGrassIndex(g);
    return (i - 40.0 * floor(i / 40.0 + 1.0e-3)) / 39.0;
}

float FoliageSunWeightOf(float b) { return LinColor(vec3(max(b - kFoliageSunCode, 0.0))).x; }
float FoliageHighlightOf(float enc) { return kFoliageHighlightMax * LinColor(vec3(saturate(enc))).x; }

vec3 PBRSun(float metal, float r, vec3 baseColor, vec3 N, vec3 V, vec3 L, vec3 radiance, float specF0Diel, bool allowSmith, float clearcoat, float pearl) {
    const float dielDiff = min(specF0Diel, 0.04);
    const vec3 nrm = normalize(N);
    const vec3 viewV = normalize(V);
    const vec3 toLight = normalize(L);
    const vec3 mirrorDir = reflect(-viewV, nrm);
    const float discSin = sin(kSunRadius);
    const float discCos = cos(kSunRadius);
    const float LdotR = dot(toLight, mirrorDir);
    const vec3 offAxis = mirrorDir - LdotR * toLight;
    const vec3 offAxisDir = dot(offAxis, offAxis) > 1.0e-12 ? normalize(offAxis) : vec3(0.0);
    const vec3 sunDirV = LdotR < discCos ? normalize(discCos * toLight + offAxisDir * discSin) : mirrorDir;

    const vec3 halfV = normalize(viewV + sunDirV);
    const float rawNdotS = shades(nrm, sunDirV);
    const float NdotS = max(rawNdotS, 0.00001);
    const float NdotV = max(shades(nrm, viewV), 0.00001);
    const float NdotH = shades(nrm, halfV);
    const float NdotL = shades(nrm, toLight);
    const float LdotH = shades(toLight, halfV);

    const vec3 metalCol = ApplyPearlescence(baseColor, NdotV, pearl, metal);
    const vec3 pearlF90Tint = PearlF90Tint(metalCol, pearl, metal);
    const vec3 f0Base = ReflectivityF0(mix(vec3(dielDiff), metalCol, metal));
    const vec3 fres = Fresnel(f0Base, max(RoughReflectivityF90(f0Base, r, metal) * pearlF90Tint, f0Base), LdotH);
    const float diffKeep = DiffuseEnergyKeep(NdotV, r);
    const vec3 diff = (1.0 - metal) * (1.0 - fres) * baseColor * diffKeep;

    vec3 fresSpec = fres;
    if (specF0Diel != dielDiff) {
        const vec3 f0Spec = ReflectivityF0(mix(vec3(specF0Diel), metalCol, metal));
        fresSpec = Fresnel(f0Spec, max(RoughReflectivityF90(f0Spec, r, metal) * pearlF90Tint, f0Spec), LdotH);
    }
    vec3 specOut = BRDFPi(r, fresSpec, NdotV, NdotS, NdotH, allowSmith) * MultiScatterMetalBoost(f0Base, r, NdotV, metal);
    if (clearcoat > 0.001 && metal > 0.05) {
        specOut *= CandyCoatAbsorption(metalCol, NdotV, clearcoat, metal);
    }
    vec3 total = (diff * NdotL + specOut * rawNdotS) * radiance;
    if (clearcoat > 0.001 && metal > 0.05 && rawNdotS > 0.0) {
        const float coatW = clearcoat * metal;
        const float omc = 1.0 - LdotH;
        const float omc2 = omc * omc;
        const float Fc = 0.04 + 0.96 * (omc2 * omc2 * omc);
        const float coatSpec = BRDFPi(0.06, vec3(Fc), NdotV, NdotS, NdotH, allowSmith).x * rawNdotS;
        total = total * (1.0 - 0.25 * coatW * Fc) + radiance * (coatSpec * coatW);
    }
    return total;
}
#endif
