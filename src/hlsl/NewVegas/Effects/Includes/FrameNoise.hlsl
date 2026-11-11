// UNOFFICIAL 2026-11-01 the temporal backbone's noise (core/TemporalBackbone.h, docs/next/FLICKER-PROGRAM.md "Part 2";
// [Main.Main.Quality] SharedNoise). One code path for the per-pixel noise NVR's effects use, at an index the C++ side picks:
//   - an effect WITHOUT its own history passes TESR_FrameNoise.x (the shared index while TAA / FSR / DLSS averages frames, else 0:
//     a still pattern), or its own constant set from TemporalBackbone::NoiseIndex();
//   - an effect WITH a history passes its own constant set from TemporalBackbone::EffectIndex(its offset) (it keeps animating).
// The formulas are the ones the effects used before (same patterns for the same index), so moving an effect here changes nothing.
// TESR_FrameNoise: x noise index (0-1023), y 1 = a temporal resolve runs this frame, zw the R2 point of x (0..1 each).

float4 TESR_FrameNoise;

// Interleaved gradient noise (Jimenez 2014) in [0, 1) at a pixel (VPOS, whole pixels).
float FrameNoiseIGN0(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// IGN moved along by the index, Jimenez's temporal step (5.588238 pixels a frame on both axes): VolumetricLight's march, Reflections.
float FrameNoiseIGN(float2 pixel, float index)
{
    return FrameNoiseIGN0(pixel + index * 5.588238);
}

// The same at the shared noise index (an effect without a history).
float FrameNoiseIGNShared(float2 pixel)
{
    return FrameNoiseIGN(pixel, TESR_FrameNoise.x);
}

// Triangular dither noise (-1..1, two IGN values summed) as the tonemap's and SkyDither's: the index moves it by (5.588238, 3.235).
float FrameNoiseDither(float2 pixel, float index)
{
    const float2 p = pixel + index * float2(5.588238, 3.235);
    // UNOFFICIAL 2026-11-09 (review 6.6.1): the second read is taken at the pixel's SWAPPED coordinates. At a constant offset it was fract(first + 0.427 or 0.444) for every pixel (the noise is linear in the pixel), so the 'triangular' sum was one number counted twice: uneven grain. Every copy of this noise changes together (SkyDither, the tonemap, Cinema, and the SMAA / DitherBuster passes that take it off again).
    return FrameNoiseIGN0(p) + FrameNoiseIGN0(p.yx + float2(47.0, 17.0)) - 1.0;
}

// The R2 sequence (Roberts 2018) at an index, shifted by a per-effect slot (TemporalBackbone::R2 in C++ gives the same points).
float2 FrameNoiseR2(float index, float slot)
{
    const float n = index + slot * 0.5;
    return frac(0.5 + n * float2(0.7548776662, 0.5698402910));
}

// The R2 point of the shared noise index (slot 0), as the C++ side worked it out in double precision.
float2 FrameNoiseR2Shared()
{
    return TESR_FrameNoise.zw;
}
