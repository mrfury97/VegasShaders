// UNOFFICIAL 2026-11-03 SmoothCutouts ([Shaders.TAA.Cutouts], core/AlphaHash.h): smooth edges for alpha-tested cut-outs (leaves, grass,
// fences, chain-link, hair cards) while TAA / FSR / DLSS anti-alias the world. Their own transparency multisampling (alpha to coverage)
// only works with the game's MSAA, which is off with them, so every texel was drawn fully or not at all: jagged cut edges, and the
// see-through fringe of a texture made for alpha to coverage drawn solid (a tester's hair: a dark outline round the head).
//
// Hashed alpha test: each pixel keeps the texel with a probability (its coverage) against noise that changes per pixel and per frame;
// the jittered frames average it back into a partial coverage, as MSAA's samples did. Every pass of an object takes the same decision
// for a pixel (same alpha, same noise), so its lamp passes stay on the pixels its first pass drew.
//   Mode 1 edges: the cut stays where the game's test puts it, softened over about EdgeWidth pixels.
//   Mode 2 like the game's MSAA: coverage = the texel's alpha (what alpha to coverage gives; the look cut-out textures were made for).
// Compiled in only with ALPHA_HASH_ON (Enabled at startup); off, the shaders are exactly as before.
//
// UNOFFICIAL 2026-11-16 FarCoverage ([Shaders.TAA.Cutouts] FarCoverage / FarCoverageScale, ALPHA_COVERAGE_ON; docs/next/FAR-FOLIAGE-AA.md):
// far cut-outs keep their coverage, with or without TAA. A texture's smaller mips average each cut-out texel with the holes around it, so
// far away the alpha of a thin needle or leaf drops under the game's test (0.5) and whole needles vanish: far trees go thin and speckled.
// The alpha is raised with the mip the hardware reads (Golus 2017, "Anti-aliased alpha test"): alpha * (1 + mip * scale), the mip from the
// uv's screen derivatives and the texture's size, which SM3 cannot query: AlphaHash::ForPass sends a size class in b11 / b12 (256 to 2048
// texels, the geometric mean of width and height). Only on the passes b15 marks (alpha-tested, unblended, world scene); off on a hashed
// Mode 2 draw, whose coverage is the alpha itself (the mean alpha a mip keeps). Off (FarCoverage false at startup): not compiled in.

#ifndef ALPHA_HASH_INCLUDED
#define ALPHA_HASH_INCLUDED

bool NVR_AlphaHashDraw : register(b15);	// per draw (AlphaHash::ForPass): an alpha-tested, unblended object of the world scene
// x mode (0 off, 1 edges, 2 like MSAA; 0 too while the scene is not jittered), y this frame's noise offset (pixels), z edge width (pixels),
// w FarCoverage: 0 off, else scale + 2 * q, q the world's texture mip bias in quarter mips below 0 (TAA's MipBias, RenderScale).
// Pinned to c196 (clear across ObjectTemplate, ParallaxTemplate, Grass PS; unpinned, FXC packed it into c0/c1 where game draws overwrote it).
float4 TESR_AlphaHash : register(c196);

#if defined(NVR_SM4) && NVR_SM4
    #define ALPHA_HASH_VPOS_PARAM , float4 alphaHashVPos4 : VPOS	// the no-limits build: SV_Position, the pixel centre
    #define ALPHA_HASH_VPOS_VALUE (alphaHashVPos4.xy - 0.5f)
#else
    #define ALPHA_HASH_VPOS_PARAM , float2 alphaHashVPos : VPOS
    #define ALPHA_HASH_VPOS_VALUE alphaHashVPos
#endif

// 1 keeps the pixel, -1 drops it, with probability coverage of keeping it. Interleaved gradient noise (Jimenez 2014): a 3x3
// neighbourhood holds both outcomes, so TAA's and FSR's history clamps keep the average instead of rejecting it as flicker.
// salt shifts the pattern (grass: away from its own distance-fade dither).
// 2026-11-16 far-foliage-aa (HashStyle, test key; TESR_AlphaHash.z = edge width + 16 x style): how the noise moves in time.
//   0: the pattern slides 5.588 px a frame over 64 frames (the first version). Native TAA's history is an exponential average
//      (HistoryWeight 0.9) behind a variance clip: a fresh random keep / drop every frame never settles in it, so foliage fizzed
//      with the camera still (12.8 % of the far-pine pixels changed frame to frame vs 6.1 % with the hard cut).
//   1: the pattern stays; its threshold steps through 8 stratified values (bit-reversed eighths, y), the length of TAA's jitter
//      cycle: every 8 frames each pixel keeps the texel exactly coverage x 8 times, in the most even order.
//   2: the pattern stays and does not move at all: a still camera gives a still picture (the jitter alone softens it).
//   3: as 1, with the hashed range narrowed to the soft band round the cut (coverage = (alpha - ref) x 2 + ref): the solid needle cores
//      and the empty gaps stop flickering; only the edges dither.
//   4: as 3 with the still pattern; 5: as 3 with a wider band (x 1.5); 6: as 3 with a narrower band (x 3).
float AlphaHashStyle() { return floor(TESR_AlphaHash.z * (1.0f / 16.0f)); }

float AlphaHashKeep(float coverage, float2 vpos, float salt) {
    const float style = AlphaHashStyle();
    const float2 p = vpos + (style < 0.5f ? TESR_AlphaHash.y : 0.0f) + salt;	// (styles 2 and 4: y is 0, the still pattern)
    const float noise = frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))) + (style < 0.5f ? 0.0f : TESR_AlphaHash.y));
    return coverage > noise ? 1.0f : -1.0f;
}

// The alpha the game's test should see: unchanged unless this draw is hashed; then 1 (kept: passes clip(a - ref) and the fixed-function
// test against the same reference) or -1 (dropped by the clip).
float AlphaHash(float alpha, float ref, float2 vpos) {
    const float style = AlphaHashStyle();
    const float width = max(fwidth(alpha) * (TESR_AlphaHash.z - 16.0f * style), 1.0f / 255.0f);	// out of the branch: no derivatives inside one
    float result = alpha;
    if (NVR_AlphaHashDraw) {
        if (TESR_AlphaHash.x > 0.5f) {
            const float band = style > 5.5f ? 3.0f : style > 4.5f ? 1.5f : style > 2.5f ? 2.0f : 1.0f;
            const float soft = saturate((alpha - ref) * band + ref);
            const float coverage = TESR_AlphaHash.x > 1.5f ? soft : saturate((alpha - ref) / width + 0.5f);
            result = AlphaHashKeep(coverage, vpos, 0.0f);
        }
    }
    return result;
}

#ifdef ALPHA_COVERAGE_ON
bool NVR_CutSize1 : register(b11);	// the base texture's size class (AlphaHash::ForPass): 256 << (b11 + 2 * b12) texels
bool NVR_CutSize2 : register(b12);

// The alpha the cut should see: raised with the mip on a marked pass (FarCoverage), else unchanged. uv: the base texture's coordinates.
float AlphaCutCoverage(float alpha, float2 uv) {
    const float2 dx = ddx(uv), dy = ddy(uv);	// out of the branch: no derivatives inside one
    const float footprint = max(dot(dx, dx), dot(dy, dy));	// (uv per pixel)^2
    float result = alpha;
    if (NVR_AlphaHashDraw) {
        if (TESR_AlphaHash.w > 0.0f && TESR_AlphaHash.x < 1.5f) {	// on, and not a Mode 2 hashed draw
            const float q = floor(TESR_AlphaHash.w * 0.5f);
            const float scale = TESR_AlphaHash.w - 2.0f * q;
            float log2Size = 8.0f;
            if (NVR_CutSize1) log2Size += 1.0f;
            if (NVR_CutSize2) log2Size += 2.0f;
            const float mip = 0.5f * log2(max(footprint, 1e-12f)) + log2Size - 0.25f * q;
            result = saturate(alpha * (1.0f + max(mip, 0.0f) * scale));
        }
    }
    return result;
}
#endif

// The alpha a marked pass writes (2026-11-16 far-foliage-aa): the game's own fixed-function alpha test cuts on it, so this is where both
// act on an ordinary alpha-tested object (the shader-side test only runs on fading passes). FarCoverage first, then the hash.
#ifdef ALPHA_HASH_ON
float AlphaCutOutput(float alpha, float ref, float2 uv, float2 vpos) {
    #ifdef ALPHA_COVERAGE_ON
        alpha = AlphaCutCoverage(alpha, uv);
    #endif
    return AlphaHash(alpha, ref, vpos);
}
#endif

#endif
