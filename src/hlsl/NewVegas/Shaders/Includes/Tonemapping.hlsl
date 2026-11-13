// Tonemapping operators for the HDR blend shaders (ISHDRBLENDINSHADERCIN*.pso). tonemap() picks one by TESR_HDRData.x; the
// curve parameters come from TESR_LotteData, TESR_ToneMapping and TESR_HDRBloomData.w (white point / peak).

// Narkowicz's fit of the ACES curve (https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/), per channel.
float3 ACESFilm(float3 c)
{
    // rational fit: c (n2 c + n1) / (c (d2 c + d1) + d0)
    float n2 = 2.51f, n1 = 0.03f;
    float d2 = 2.43f, d1 = 0.59f, d0 = 0.14f;
    return (c * (n2 * c + n1)) / (c * (d2 * c + d1) + d0);
}

// -----------------------------------------------------------------------------------------------
// The code in this file was originally written by Stephen Hill (@self_shadow), who deserves all
// credit for coming up with this fit and implementing it. Buy him a beer next time you see him. :)

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat =
{
    { 0.59719, 0.35458, 0.04823 },
    { 0.07600, 0.90834, 0.01566 },
    { 0.02840, 0.13383, 0.83777 }
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat =
{
    { 1.60475, -0.53108, -0.07367 },
    { -0.10208, 1.10813, -0.00605 },
    { -0.00327, -0.07276, 1.07602 }
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

// ACES Filmic (Fitted)
float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = mul(ACESOutputMat, color);

    // Clamp to [0, 1]
    color = saturate(color);

    return color;
}
// -----------------------------------------------------------------------------------------------

// Reinhard on luminance, blended toward per-channel Reinhard as the channels saturate (Jodie's variant).
float3 tmReinhardJodie(float3 c)
{
    float y = luma(c);
    float3 squeezed = c / (1.0f + c);
    return lerp(c / (1.0f + y), squeezed, squeezed);
}

// Multiplies c so its luma becomes target (same hue).
float3 tmRescaleLuma(float3 c, float target)
{
    float y = luma(c);
    // UNOFFICIAL 2026-10-16: black has y = target = 0 (the ratio was 0/0 = NaN); the ratio tends to 1 there.
    return c * (abs(y) > 1e-8 ? target / y : 1.0);
}

// Extended Reinhard on luminance: y (1 + y / w^2) / (1 + y), w = the white point (setting x 3, at least 1).
float3 tmReinhardExtended(float3 c, float whitePt)
{
    float w = max(1.0, whitePt * 3.0);
    float y = luma(c);
    float num = y * (1.0f + (y / (w * w)));
    return tmRescaleLuma(c, num / (1.0f + y));
}

// Hable's filmic curve (Uncharted 2).
float3 tmUnchartedCurve(float3 c)
{
    float shoulderStr = 0.15;
    float linearStr = 0.50;
    float linearAngle = 0.10;
    float toeStr = 0.20;
    float toeNum = 0.01;
    float toeDen = 0.30;
    return ((c * (shoulderStr * c + linearAngle * linearStr) + toeStr * toeNum) / (c * (shoulderStr * c + linearStr) + toeStr * toeDen)) - toeNum / toeDen;
}
float3 tmUncharted2(float3 c, float exposure, float whitePt)
{
    float gain = max(0.1, exposure * 1.4);
    float3 mapped = tmUnchartedCurve(c * gain);

    float w = max(1.0, whitePt * 3.0);
    float3 invWhite = float3(1.0f, 1.0f, 1.0f) / tmUnchartedCurve(w);
    return mapped * invWhite;
}

// AgX, after the minimal version at https://iolite-engine.com/blog_posts/minimal_agx_implementation
static const float3x3 agx_mat = {
    0.842479062253094,  0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772,  0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104 };
static const float3x3 agx_mat_inv = {
     1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116 };

// Polynomial stand-in for the AgX sigmoid (mean squared error about 3.7e-6).
float3 agxDefaultContrastApprox(float3 t)
{
    float3 t2 = t * t;
    float3 t4 = t2 * t2;
    return 15.5 * t4 * t2 - 40.14 * t4 * t + 31.96 * t4 - 6.868 * t2 * t + 0.4298 * t2 + 0.1191 * t - 0.00232;
}

// Back out of the AgX space, then to linear with a 2.2 power (we output linear, not to an sRGB target).
float3 agxEotf(float3 c)
{
    // UNOFFICIAL 2026-10-18: row vector times matrix. The GLSL original fills its matrices column by column, so mul(M, v) was the
    // transpose and tinted everything pink. Fix by Conor (testbed).
    float3 outset = mul(c, agx_mat_inv);
    return pows(outset, 2.2);
}

// ASC CDL look with the neutral ("default") settings; golden would be slope (1, 0.9, 0.5), power 0.8, sat 0.8; punchy power 1.35,
// sat 1.4.
float3 agxLook(float3 c)
{
    float3 mono = luma(c);
    float3 lift = 0.0;
    float3 gain = 1.0;
    float gamma = 1.0;
    float chroma = 1.0;
    float3 graded = pows(c * gain + lift, gamma);
    return mono + chroma * (graded - mono);
}

float3 agx(float3 c)
{
    float evLo = -12.47393f;
    float evHi = 4.026069f;

    float3 inset = mul(c, agx_mat);	// UNOFFICIAL 2026-10-18: row vector, as agxEotf (fix by Conor)
    // log2 encoding over [evLo, evHi]. UNOFFICIAL 2026-10-16: negative input gave a NaN log; 1e-6 is under evLo anyway.
    float3 enc = clamp(log2(max(inset, 1e-6)), evLo, evHi);
    enc = (enc - evLo) / (evHi - evLo);
    // UNOFFICIAL 2026-11-13 (modern lighting) TonemapContrast for AgX: contrast in the log encoding, turned round mid grey
    // (scene 0.18 sits at 0.606 of the encoded range), before the curve. Mid grey keeps its place; the curve's own toe and
    // shoulder still round both ends off, so bright things reach white sooner without a hard cut. 1 = AgX as it was.
    const float agxContrast = TESR_LotteData.x;
    [flatten] if (agxContrast != 1.0f) enc = saturate((enc - 0.6060606f) * agxContrast + 0.6060606f);
    return agxEotf(agxLook(agxDefaultContrastApprox(enc)));
}

// Lottes 2016 (https://gpuopen.com/wp-content/uploads/2016/03/GdcVdrLottes.pdf), via
// https://gist.github.com/KelSolaar/1213139203911a72fef531c32c3d4ec2 and vtastek's tuned version
// (https://gist.github.com/vtastek/935be12fb8d87adda751b5276fd88f0c), which is the default here.

#define CMAX 1.6e+6f	// largest value the curves take (keeps x^contrast finite)
#define EPS 1e-6f	// smallest peak (no 0/0)

// Tone-maps the brightest channel; the colour ratios get a crosstalk transform that desaturates toward white as it brightens.
float3 VTLottes(float3 c, float contrast, float b, float d, float shoulder, float crosstalk)
{
    float3 top = max(c.r, max(c.g, c.b));
    top = min(CMAX, max(EPS, top));
    float3 hue = min(CMAX, c / top);

    float weighted = dot(c, float3(0.5, 0.4, 0.33));
    float floorCh = max(0.0, min(c.r, min(c.g, c.b)));
    top += min(top, floorCh);
    top *= 0.5;
    top *= 1.0 + 1.666 * max(0, (top - weighted) / top);

    float3 zc = pows(top, contrast);
    top = zc / (pows(zc, shoulder) * b + d);

    float xtalk = max(1.0, crosstalk);
    float sat = contrast;
    float crossSat = contrast * (64.0 / xtalk);
    hue = pows(abs(hue + 0.11) * 0.90909, sat / crossSat);
    hue = lerp(hue, 1.0, pows(top, max(float3(1.0, 1.0, 1.0), float3(4.0, 3.0, 3.0) * xtalk) * 1.0 / top));
    hue = pows(min(1.0, hue), crossSat);
    return top * hue;
}

// Lottes curve x^a / ((x^a)^d * b + c): the b and c that put midIn on midOut and hdrMax on 1.
float ColToneB(float peakIn, float a, float d, float mIn, float mOut)
{
    return -((-pow(mIn, a) + (mOut * (pow(peakIn, a * d) * pow(mIn, a) - pow(peakIn, a) * pow(mIn, a * d) * mOut))
              / (pow(peakIn, a * d) * mOut - pow(mIn, a * d) * mOut))
             / (pow(mIn, a * d) * mOut));
}
float ColToneC(float peakIn, float a, float d, float mIn, float mOut)
{
    return (pow(peakIn, a * d) * pow(mIn, a) - pow(peakIn, a) * pow(mIn, a * d) * mOut)
         / (pow(peakIn, a * d) * mOut - pow(mIn, a * d) * mOut);
}
// q = (a, d, b, c)
float ColTone(float v, float4 q)
{
    float va = pow(v, q.x);
    return va / (pow(va, q.y) * q.z + q.w);
}

float3 Lottes(float3 c, float contrast, float midOut, float midIn, float hdrMax, float shoulder)
{
    float peakIn = max(1.0, hdrMax * 50.0);
    float a = max(0.01, contrast * 1.25);
    float d = saturate(shoulder * 0.993);	// must stay under 1
    float mIn = max(0.01, midIn * 0.18);
    float mOut = max(0.01, midOut * 0.1818);

    float b = ColToneB(peakIn, a, d, mIn, mOut);
    float k = ColToneC(peakIn, a, d, mIn, mOut);

    float3 top = max(c.r, max(c.g, c.b));
    top = min(CMAX, max(EPS, top));
    float3 hue = min(CMAX, c / top);

    float weighted = dot(c, float3(0.5, 0.4, 0.33));
    float floorCh = max(0.0, min(c.r, min(c.g, c.b)));
    top += min(top, floorCh);
    top *= 0.5;
    top *= 1.0 + 1.666 * max(0, (top - weighted) / top);

    top = ColTone(top.x, float4(a, d, b, k));	// (.x: what the implicit truncation passed; top is grey, x = y = z; ends warning X3206)

    float sat = a;
    float crossSat = a * 64.0;
    float3 one = 1.0;
    hue = pows(abs(hue + 0.11) * 0.90909, sat / crossSat);
    hue = lerp(hue, one, pow(abs(top), float3(4.0, 1.5, 1.5) * 1.0/top));	// (abs: free, top >= EPS here; ends warning X3571)
    hue = pows(min(1.0, hue), crossSat);
    return top * hue;
}

// Uchimura's GT curve for one channel: power toe, linear middle, exponential shoulder, joined with weights.
float uchimuraCurve(float v, float maxOut, float slope, float start, float len, float toePow, float pedestal)
{
    float lenOut = ((maxOut - start) * len) / slope;
    float linEndIn = start + lenOut;
    float linEndOut = start + slope * lenOut;
    float rate = (slope * maxOut) / (maxOut - linEndOut);

    float lin = start + slope * (v - start);
    float toe = start * pow(abs(v / start), toePow);	// (abs: free; was NaN below 0 and that part is faded out there; ends warning X3571)
    float sh = maxOut - (maxOut - linEndOut) * exp(-rate * (v - linEndIn) / maxOut);
    float toeW = 1 - smoothstep(0.0f, start, v);
    float shW = (v < start + lenOut) ? 0 : 1;
    float linW = 1 - toeW - shW;
    return (float) (toe * toeW + lin * linW + sh * shW);
}

float3 tmUchimura(float3 c, float contrast, float brightness, float midIn, float hdrMax, float shoulder)
{
    float maxOut = 1.0 * brightness;
    float slope = 1.0 * contrast;
    float start = 0.22 * midIn;
    float len = 0.4 * hdrMax;
    float toePow = 1.0 + (0.333 * shoulder);
    float pedestal = 0.0;
    return float3(uchimuraCurve(c.r, maxOut, slope, start, len, toePow, pedestal),
                  uchimuraCurve(c.g, maxOut, slope, start, len, toePow, pedestal),
                  uchimuraCurve(c.b, maxOut, slope, start, len, toePow, pedestal));
}

// 1 - e^(-x p): the DICE shoulder's approach to its maximum.
float tmRangePow(float x, float p)
{
    return 1.0 - pow(exp(-x), p);
}

float tmShoulder(float v, float top, float knee, float p)
{
    float rolled = knee + (top - knee) * tmRangePow((v - knee) / (top - knee), p);
    return v <= knee ? v : rolled;
}

// DICE: a shoulder on luminance only, from shoulder% of the output maximum upward.
float3 tmDice(float3 c, float contrast, float brightness, float hdrMax, float shoulder)
{
    float knee01 = shoulder * 0.01;
    float p = contrast;
    float nits = brightness;

    float y = luma(c);
    float top = hdrMax / nits;
    float knee = knee01 * top;
    float yOut = tmShoulder(y, top, knee, p);
    return c * (y != 0.0 ? yOut / y : 1.0);	// UNOFFICIAL 2026-10-22: black was 0/0 = NaN
}


// -----------------------------------------------------------------------------------------------
// GT7 Tone Mapping (Gran Turismo 7), TonemappingMode 11.
// From Polyphony Digital's sample implementation for "Driving Toward Reality: Physically Based
// Tone Mapping and Perceptual Fidelity in Gran Turismo 7" (SIGGRAPH 2025):
// https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp
//
// MIT License
// Copyright (c) 2025 Polyphony Digital Inc.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions: The above copyright notice and this
// permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// How it is wired in (TonemappingMode = 11; mode 8 stays Uchimura's GT Sport curve):
//  - the scene's linear (sRGB / Rec.709 primaries) colour is converted to Rec.2020, which is what the
//    operator expects, and back to Rec.709 afterwards.
//  - The SDR path of the reference is used: 1.0 in the operator = 100 nits, SDR paper white =
//    250 nits, output in [0, 1] ready for the gamma that follows.
//  - Calibration as in Polyphony's code: a scene value of 1.0 (after Exposure) is sRGB white = 100 nits
//    (GT7_INPUT_SCALE 1.0). The curve is straight up to 111 nits and rolls off to 250-nit paper white,
//    so about 1.9 stops above white are kept for sunlit surfaces, sky and highlights before anything
//    clips. Brightness is set with Exposure, as GT7 sets it with its camera exposure.
//  - HighlightSaturation (Shaders > Tonemapping, per day/night/interior) scales where colour fades
//    to white in the highlights: 1.0 = GT7 reference, higher = bright colours stay saturated longer.
//  - Everything after the tonemapper (HueSafeClip, sky dither, gamma, fades) runs as before.
// -----------------------------------------------------------------------------------------------

#ifndef GT7_INPUT_SCALE
#define GT7_INPUT_SCALE 1.0      // operator units per scene unit: 1.0 = reference (1.0 = 100 nits). Higher = brighter, less highlight headroom (same as raising Exposure)
#endif
#ifndef GT7_PAPER_WHITE
#define GT7_PAPER_WHITE 250.0    // cd/m^2, SDR reference white the curve is built around (reference: 250)
#endif
#ifndef GT7_BLEND_RATIO
#define GT7_BLEND_RATIO 0.6      // 0 = per-channel curve only (hue skews to yellow/white), 1 = chroma-preserving only (reference: 0.6)
#endif
#ifndef GT7_FADE_START
#define GT7_FADE_START 0.98      // chroma fade start, relative to peak luminance (reference: 0.98)
#endif
#ifndef GT7_FADE_END
#define GT7_FADE_END 1.16        // chroma fade end (reference: 1.16)
#endif
#ifndef GT7_UCS_JZAZBZ
#define GT7_UCS_JZAZBZ 0         // 0 = ICtCp (reference default), 1 = Jzazbz
#endif

static const float GT7_REFERENCE_LUMINANCE = 100.0;                               // cd/m^2 <-> 1.0
static const float GT7_PEAK = GT7_PAPER_WHITE / GT7_REFERENCE_LUMINANCE;          // frame-buffer peak (2.5)

// "GT Tone Mapping" curve with convergent shoulder (GT7 parameters).
static const float GT7_ALPHA = 0.25;
static const float GT7_MID = 0.538;
static const float GT7_LINEAR = 0.444;
static const float GT7_TOE = 1.280;
static const float GT7_K = (GT7_LINEAR - 1.0) / (GT7_ALPHA - 1.0);
static const float GT7_KA = GT7_PEAK * GT7_LINEAR + GT7_PEAK * GT7_K;
static const float GT7_KB = -GT7_PEAK * GT7_K * exp(GT7_LINEAR / GT7_K);
static const float GT7_KC = -1.0 / (GT7_K * GT7_PEAK);

float3 GT7Curve(float3 x)
{
    x = max(x, 0.0);
    const float3 weightLinear = smoothstep(0.0, GT7_MID, x);
    const float3 toe = GT7_MID * pow(x / GT7_MID, GT7_TOE);
    const float3 linearPart = lerp(toe, x, weightLinear);
    const float3 shoulder = GT7_KA + GT7_KB * exp(x * GT7_KC);
    return (x < GT7_LINEAR * GT7_PEAK) ? linearPart : shoulder;
}

// SMPTE ST 2084 (PQ), on frame-buffer values (1.0 = 100 nits). exponentScale is 1 for ICtCp.
static const float GT7_PQ_M1 = 0.1593017578125;
static const float GT7_PQ_M2 = 78.84375;
static const float GT7_PQ_C1 = 0.8359375;
static const float GT7_PQ_C2 = 18.8515625;
static const float GT7_PQ_C3 = 18.6875;

float3 GT7InverseEotfPQ(float3 v, float exponentScale)
{
    const float3 ym = pow(max(v * (GT7_REFERENCE_LUMINANCE / 10000.0), 0.0), GT7_PQ_M1);
    return pow((GT7_PQ_C1 + GT7_PQ_C2 * ym) / (1.0 + GT7_PQ_C3 * ym), GT7_PQ_M2 * exponentScale);
}

float3 GT7EotfPQ(float3 n, float exponentScale)
{
    const float3 np = pow(saturate(n), 1.0 / (GT7_PQ_M2 * exponentScale));
    const float3 l = max(np - GT7_PQ_C1, 0.0) / (GT7_PQ_C2 - GT7_PQ_C3 * np);
    return pow(l, 1.0 / GT7_PQ_M1) * (10000.0 / GT7_REFERENCE_LUMINANCE);
}

// Rec.709 <-> Rec.2020 (linear, D65)
static const float3x3 GT7_Rec709ToRec2020 =
{
    { 0.6274040, 0.3292820, 0.0433136 },
    { 0.0690970, 0.9195400, 0.0113612 },
    { 0.0163916, 0.0880132, 0.8955952 }
};
static const float3x3 GT7_Rec2020ToRec709 =
{
    {  1.6604910, -0.5876411, -0.0728499 },
    { -0.1245505,  1.1328999, -0.0083494 },
    { -0.0181508, -0.1005789,  1.1187297 }
};

#if GT7_UCS_JZAZBZ
// Jzazbz (Safdar et al. 2017), coefficients adjusted for linear Rec.2020 as in the reference.
static const float GT7_JZ_EXPONENT = 1.7;
static const float GT7_JZ_D0 = 1.6295499532821566e-11;
static const float3x3 GT7_RGBToLMS =
{
    { 0.530004, 0.355704, 0.086090 },
    { 0.289388, 0.525395, 0.157481 },
    { 0.091098, 0.147588, 0.734234 }
};
static const float3x3 GT7_LMSToRGB =
{
    {  2.990669, -2.049742,  0.088977 },
    { -1.634525,  3.145627, -0.483037 },
    { -0.042505, -0.377983,  1.448019 }
};

float3 GT7RGBToUCS(float3 rgb) // linear Rec.2020 frame-buffer values -> (Jz, az, bz)
{
    const float3 lms = GT7InverseEotfPQ(mul(GT7_RGBToLMS, rgb), GT7_JZ_EXPONENT);
    const float iz = 0.5 * lms.x + 0.5 * lms.y;
    return float3((0.44 * iz) / (1.0 - 0.56 * iz) - GT7_JZ_D0,
                  3.524000 * lms.x - 4.066708 * lms.y + 0.542708 * lms.z,
                  0.199076 * lms.x + 1.096799 * lms.y - 1.295875 * lms.z);
}

float3 GT7UCSToRGB(float3 jab)
{
    const float jz = jab.x + GT7_JZ_D0;
    const float iz = jz / (0.44 + 0.56 * jz);
    const float3 lms = float3(iz + 0.1386050432715393 * jab.y + 0.05804731615611869 * jab.z,
                              iz - 0.1386050432715393 * jab.y - 0.05804731615611869 * jab.z,
                              iz - 0.09601924202631895 * jab.y - 0.8118918960560390 * jab.z);
    return max(mul(GT7_LMSToRGB, GT7EotfPQ(lms, GT7_JZ_EXPONENT)), 0.0);
}
#else
// ICtCp (ITU-T T.302)
static const float3x3 GT7_RGBToLMS =
{
    { 1688.0 / 4096.0, 2146.0 / 4096.0,  262.0 / 4096.0 },
    {  683.0 / 4096.0, 2951.0 / 4096.0,  462.0 / 4096.0 },
    {   99.0 / 4096.0,  309.0 / 4096.0, 3688.0 / 4096.0 }
};
static const float3x3 GT7_LMSToICtCp =
{
    {  2048.0 / 4096.0,   2048.0 / 4096.0,     0.0 },
    {  6610.0 / 4096.0, -13613.0 / 4096.0,  7003.0 / 4096.0 },
    { 17933.0 / 4096.0, -17390.0 / 4096.0,  -543.0 / 4096.0 }
};
static const float3x3 GT7_ICtCpToLMS =
{
    { 1.0,  0.00860904,  0.11103 },
    { 1.0, -0.00860904, -0.11103 },
    { 1.0,  0.560031,   -0.320627 }
};
static const float3x3 GT7_LMSToRGB =
{
    {  3.43661,   -2.50645,    0.0698454 },
    { -0.79133,    1.9836,    -0.192271 },
    { -0.0259499, -0.0989137,  1.12486 }
};

float3 GT7RGBToUCS(float3 rgb) // linear Rec.2020 frame-buffer values -> (I, Ct, Cp)
{
    return mul(GT7_LMSToICtCp, GT7InverseEotfPQ(mul(GT7_RGBToLMS, rgb), 1.0));
}

float3 GT7UCSToRGB(float3 ictcp)
{
    return max(mul(GT7_LMSToRGB, GT7EotfPQ(mul(GT7_ICtCpToLMS, ictcp), 1.0)), 0.0);
}
#endif

// Luminance (I or Jz) of the peak, the reference for the chroma fade. Constant, folded by the compiler.
static const float GT7_PEAK_UCS = GT7RGBToUCS(GT7_PEAK.xxx).x;

// Input: linear Rec.709 (the scene colour after Exposure). Output: linear Rec.709 in [0, 1].
// highlightSaturation: 1 = reference chroma fade; scales the fade start/end.
float3 GT7ToneMap(float3 color, float highlightSaturation)
{
    const float3 rgb = mul(GT7_Rec709ToRec2020, max(color, 0.0) * GT7_INPUT_SCALE);

    // Per-channel curve ("skewed" colour) and the original colour, both in the UCS.
    const float3 ucs = GT7RGBToUCS(rgb);
    const float3 skewedRgb = GT7Curve(rgb);
    const float3 skewedUcs = GT7RGBToUCS(skewedRgb);

    // Keep the curve's luminance, take the chroma from the original, fading it out near peak.
    const float fadeScale = max(highlightSaturation, 0.05);
    const float chromaScale = 1.0 - smoothstep(GT7_FADE_START * fadeScale, GT7_FADE_END * fadeScale, ucs.x / GT7_PEAK_UCS);
    const float3 scaledRgb = GT7UCSToRGB(float3(skewedUcs.x, ucs.yz * chromaScale));

    const float3 blended = lerp(skewedRgb, scaledRgb, GT7_BLEND_RATIO);
    const float3 sdr = min(blended, GT7_PEAK) * (1.0 / GT7_PEAK); // SDR correction: peak -> 1.0
    return max(mul(GT7_Rec2020ToRec709, sdr), 0.0);
}
// -----------------------------------------------------------------------------------------------

// 2026-11-04 Night colour shift ([Shaders.Tonemapping.*] NightShiftEnabled, NightShift, NightShiftLevel), off by default.
// In dim light the eye sees with its rods: no colour, and most sensitive to blue-green (507 nm, against 555 nm for the cones),
// so at night reds go dark, blues stay visible and everything turns grey-blue (the Purkinje shift). Here, in the linear HDR
// picture after Exposure and before the curve: a pixel below NightShiftLevel moves toward its rod brightness in a moonlit tint,
// fully at a tenth of the level; brighter ones (lamps, lit areas) keep their colour, as the cones still see them.
// x: strength (0: skipped), y: level.
float4 TESR_ToneMappingNight : register(c33);

float3 nightShift(float3 c)
{
    [branch] if (TESR_ToneMappingNight.x <= 0.0f) return c;
    const float lum = luma(c);
    // rod (scotopic) brightness from linear Rec.709: weighted toward green and blue, a grey gives the same brightness
    const float rod = dot(max(c, 0.0f), float3(0.04f, 0.56f, 0.40f));
    // the moonlit tint, its own luminance 1 (so a grey keeps its brightness)
    const float3 tint = float3(0.847f, 1.010f, 1.357f);
    const float mesopic = 1.0f - smoothstep(TESR_ToneMappingNight.y * 0.1f, TESR_ToneMappingNight.y, lum);
    return lerp(c, rod * tint, TESR_ToneMappingNight.x * mesopic);
}

// 2026-11-04 local exposure ([Shaders.LocalExposure], effects/LocalExposure, off by default): each neighbourhood's exposure pulled
// toward the view's mean, from the blurred log brightness Effects\LocalExposure.fx.hlsl measured before the tone mapper (decoded
// as here, before Exposure: times `exposure` it is in c's units). A dark neighbourhood gains up to Shadows of its distance from
// the mean (in stops), a bright one loses up to Highlights of it. EdgeGuard: where the pixel itself is more than a stop brighter
// or darker than its neighbourhood (the sky's edge, a lamp), the correction fades, so the blur leaves no halo. At most 3 stops.
float4 TESR_LocalExposureData : register(c34);	// x shadows, y highlights, z edge guard, w 1 = measured this frame (0: skipped)
float4 TESR_LocalExposureBlur : register(c35);	// y: DebugView (1: the neighbourhood brightness as grey)
sampler2D TESR_LocalExposureBuffer : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_LocalExposureMean : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

float3 localExposure(float3 c, float2 uv, float exposure)
{
    [branch] if (TESR_LocalExposureData.w <= 0.0f) return c;
    const float scale = log2(max(exposure, 1e-4f));
    const float local = tex2Dlod(TESR_LocalExposureBuffer, float4(uv, 0.0f, 0.0f)).r + scale;	// log2, in c's units
    [branch] if (TESR_LocalExposureBlur.y > 0.5f) return exp2(local).xxx;
    const float mean = tex2Dlod(TESR_LocalExposureMean, float4(0.5f, 0.5f, 0.0f, 0.0f)).r + scale;
    const float stops = mean - local;	// > 0: a neighbourhood darker than the view's mean
    float shift = stops * (stops > 0.0f ? TESR_LocalExposureData.x : TESR_LocalExposureData.y);
    const float own = log2(max(luma(c), 1e-4f)) - local;
    shift *= exp2(-TESR_LocalExposureData.z * max(abs(own) - 1.0f, 0.0f));
    return c * exp2(clamp(shift, -3.0f, 3.0f));
}

float3 tonemap(float3 hdr)
{
    float mode = TESR_HDRData.x;
    // 0: leave it (for HDR output through SpecialK / DXVK: display mapping cannot happen here, this is not the last shader)
    if (mode == 0)
        return hdr;
    else if (mode == 1) {
        hdr = max(0.0, hdr);
        return max(0.0, VTLottes(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.z, TESR_LotteData.y, TESR_LotteData.w, TESR_ToneMapping.x));
    }
    else if (mode == 2) {
        hdr = max(0.0, hdr);
        return max(0.0, Lottes(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.y, TESR_LotteData.z, TESR_HDRBloomData.w, TESR_LotteData.w));
    }
    else if (mode == 3)
        return tmReinhardExtended(hdr, TESR_HDRBloomData.w);
    else if (mode == 4)
        return tmReinhardJodie(hdr);
    else if (mode == 5)
        return ACESFilm(hdr);
    else if (mode == 6)
        return ACESFitted(hdr);
    else if (mode == 7) {
        hdr = max(0.0, hdr);	// UNOFFICIAL 2026-10-16: like 1, 2 and 8 (the curve's denominator reaches 0 near -0.12)
        return tmUncharted2(hdr, TESR_LotteData.y, TESR_HDRBloomData.w);
    }
    else if (mode == 8) {
        hdr = max(0.0, hdr);
        return max(0.0, tmUchimura(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.y, TESR_LotteData.z, TESR_HDRBloomData.w, TESR_LotteData.w));
    }
    else if (mode == 9)
        return agx(hdr);
    else if (mode == 10)
        return tmDice(min(CMAX, hdr), TESR_LotteData.x, TESR_LotteData.y, TESR_HDRBloomData.w, TESR_LotteData.w);
    else if (mode == 11)
        return GT7ToneMap(min(CMAX, hdr), TESR_ToneMapping.x);	// GT7 (Polyphony Digital); TESR_ToneMapping.x: HighlightSaturation
    else
        return hdr;
}
