// UNOFFICIAL (aastab): the tonemap's dither noise, recomputed at any pixel. The game's tonemap (ISHDRBLENDINSHADERCIN*.pso.hlsl)
// adds a triangular noise of about one 8-bit step before the 8-bit write (two interleaved gradient noises, Jimenez 2014, summed;
// frame offset z, size y in TESR_ToneMappingClip) so the sky's bands break up. The anti-aliasing passes make HARD decisions
// (edge or not, isolated pixel or not) on that noisy picture, and a +-1 step change flips those at marginal pixels from one frame
// to the next: the twinkle. Judging on the picture with the noise taken back out leaves only the rounding error (+-0.5 step)
// in the decisions. DECISIONS ONLY: what the passes write keeps the dither (it is there to break bands).
// The including effect declares  float4 TESR_ToneMappingClip;  and passes the tap's pixel position (VPOS, whole pixels, the
// same convention as the tonemap's own).

float DitherGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// The noise the tonemap added at this pixel, in colour units (-size..+size, size = TESR_ToneMappingClip.y = strength / 255).
float TonemapDitherAt(float2 pixel)
{
    const float2 p = pixel + TESR_ToneMappingClip.z * float2(5.588238, 3.235);
    // UNOFFICIAL 2026-11-09 (review 6.6.1): the second read is taken at the pixel's SWAPPED coordinates. At a constant offset it was fract(first + 0.427 or 0.444) for every pixel (the noise is linear in the pixel), so the 'triangular' sum was one number counted twice: uneven grain. Every copy of this noise changes together (SkyDither, the tonemap, Cinema, and the SMAA / DitherBuster passes that take it off again).
    return (DitherGradientNoise(p) + DitherGradientNoise(p.yx + float2(47.0, 17.0)) - 1.0) * TESR_ToneMappingClip.y;
}

// A luma read with the tonemap's noise subtracted (the tonemap added n * saturate(rgb * 128) to every channel: the same n, so to luma too).
float LumaWithoutDither(float luma, float2 pixel)
{
    return luma - TonemapDitherAt(pixel) * saturate(luma * 128.0);
}
