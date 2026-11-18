// UNOFFICIAL 2026-11-15 SSS v2: the deferred skin's subsurface scattering (core/DeferredSkin.h, [Main.Main.Quality] DeferredSkinSSS 2 with
// [Shaders.Skin.SSS] Kernel 0; docs/next/FD-SKIN-RESULTS.md "SSS v2").
// CREDIT Conor (mrfury97): the screen-space skin scattering this builds on is his (SkinScattering, from his NVR testbed: the skin found in
// screen space, the highlights taken out before the blur and put back after, light divided by the albedo so the texture stays sharp, after
// Community Shaders' pre/post scatter); the separable kernel it replaces here is Jimenez & Gutierrez's (Separable SSS, 2012) with Community
// Shaders' kernel generator (GPL-3.0-or-later), still used with Kernel 1.
// The profile here is Christensen & Burley's normalized diffusion (Pixar, "Approximate Reflectance Profiles for Efficient Subsurface
// Scattering", 2015), from the paper's formulas: R(r) = (e^(-r/d) + e^(-r/3d)) / (8 pi d r), which integrates to 1 over the plane, one d per
// colour (red longest). It is sampled on a disc: the radii are drawn from the widest colour's (red's) distribution, CDF(r) = 1 - 1/4 e^(-r/d)
// - 3/4 e^(-r/3d), inverted on the CPU for evenly spaced u (DeferredSkin::SssTaps), each tap weighted per colour by R_c(r) / R_red(r) and the
// weights normalised by their sum (energy kept per colour whatever the taps found), the disc turned by the golden angle per tap and by a per-
// frame angle while TAA / FSR / DLSS resolve (SharedNoise; still otherwise).
// Run after the deferred light passes, in linear light, before LinearScene encodes (the frame then holds every lamp's light: all of it
// diffuses), on the deferred skin's pixels only (COLOR3.a 3.5+, model 6): hair, gear and anything not skin neither give nor take light.
//   SSS_PREP: per skin pixel the light that diffuses, L = (frame - highlights x keep) x participation / albedo^AlbedoDetail, and the view
//             z in a; 0 elsewhere (the target is cleared)
//   SSS_BLUR: the disc over L (depth-aware: a tap counts only near the centre's depth, by its radius), back into the frame:
//             highlights x keep + albedo^detail x blurred L (+ the dark texels' own light), written over the skin pixels (blend off)

#include "includes/Helpers.hlsl"
#include "includes/DeferredPixel.hlsl"

#define SSS_TAPS 24

float4 SSS_View : register(c0);	// xy 1 / target size, z pixels per world unit at view z 1 (height / (2 tan(fov y / 2))), w the disc's angle this frame
float4 SSS_Ctrl : register(c1);	// x the reference scatter distance d (red, world units), y DepthFollow, z AlbedoDetail, w 1 = the exact view z on s5
float4 SSS_Taps[SSS_TAPS] : register(c2);	// x the tap's radius / d_red, yzw its weight per colour (R_c / R_red), from DeferredSkin::SssTaps
float4 SSS_Debug : register(c26);	// (2026-11-16) x [Shaders.Skin.SSS] DebugView: 1 the prep's light L (x 0.25), 2 the pixels left out red (covered), the rest as drawn

#ifdef SSS_PREP
sampler2D SSS_Frame    : register(s0);	// the frame's copy, linear
sampler2D SSS_Albedo   : register(s1);	// COLOR1: rgb albedo, a the code (keep)
sampler2D SSS_Spec     : register(s2);	// the skin's highlight target (raw linear)
sampler2D SSS_Normals  : register(s3);	// COLOR3: b the distance, a the skin code
sampler2D SSS_Material : register(s4);	// COLOR2: r the model
sampler2D SSS_LinearZ  : register(s5);	// DeferredDepth's exact linear view z (R32F) while SSS_Ctrl.w is 1

// (2026-11-16 fd-fwdgroups) Conor's / CS's thresholds are for the texture's own values; COLOR1 holds the albedo DECODED (linear), where 0.04
// is a texture value of about 0.23: beard stubble, a moustache, the eye's rim and darker skin tones counted as "dark texels". Taken back to
// the texture's curve first (2.2: only a threshold), so only near-black texels step out, as in v1.
float3 Participation(float3 a) { return smoothstep(0.01f, 0.04f, pow(max(a, 0.0f), 1.0f / 2.2f)); }
float3 AlbedoFactor(float3 a) { return pow(max(a, 1.0e-4f), SSS_Ctrl.z); }

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    const float4 uv4 = float4(uv, 0.0f, 0.0f);
    const float4 n = tex2Dlod(SSS_Normals, uv4);
    const float4 m = tex2Dlod(SSS_Material, uv4);
    clip(n.a > 3.5f && abs(FDModel(m.r) - 6.0f) < 0.5f ? 1.0f : -1.0f);
    const float4 a = tex2Dlod(SSS_Albedo, uv4);
    const float keep = saturate(FDAddedLightKeep(a.a));
    const float3 frame = tex2Dlod(SSS_Frame, uv4).rgb;
    const float3 spec = tex2Dlod(SSS_Spec, uv4).rgb * keep;
    // (2026-11-16 bench audit, fwg-native-face-dark: after the weighting fix the disc still put a black blob under the moustache's shaded
    // half, larger with Radius 2, while no scattering and the separable kernel were clean.) A pixel whose frame holds LESS light than the
    // skin's own highlights there is not the skin the passes lit: something drawn over it in the span (the moustache, a beard, brows) left
    // its own colour in the frame while the G-buffer still holds the skin under it. Its "diffuse" (frame - highlights) was 0 at full weight:
    // a black source the disc spread round. Such a pixel neither gives nor takes: a = 0 (not skin), the blur leaves it as drawn.
    const float3 over = spec - frame;
    clip(max(over.r, max(over.g, over.b)) > 1.0e-4f ? -1.0f : 1.0f);
    const float3 diffuse = max(frame - spec, 0.0f);
    const float z = SSS_Ctrl.w > 0.5f ? tex2Dlod(SSS_LinearZ, uv4).x : n.b;
    // (2026-11-16, the black patch under a moustache, second round) A surface drawn over the skin in the span without writing the G-buffer
    // (the game's hair, brows and beards drawn in the main list: the G-buffer under them still says "skin", GBufferView 6 showed it) left its
    // own colour and DEPTH; the disc took that colour as the skin's light and wrote the blurred skin over it: under a moustache in shade, the
    // dark lip's light in place of the hair. The light passes test the same thing (DeferredLamps.hlsl: depth-tested at the G-buffer's surface,
    // 1 % + 2 units nearer): where the depth buffer is nearer than the skin's own surface, the pixel is not the skin's. It neither gives nor
    // takes (a = -1: no tap, the blur leaves it as drawn; DebugView 2 shows it red). Only with the exact linear view z (DeferredDepth 2).
    [branch] if (SSS_Ctrl.w > 0.5f) {
        const float tanY = 0.5f / (SSS_View.y * SSS_View.z);		// SSS_View.z = height / (2 tan(fov y / 2)), SSS_View.y = 1 / height
        const float tanX = tanY * SSS_View.y / SSS_View.x;		// square pixels
        const float3 ray = float3((uv.x * 2.0f - 1.0f) * tanX, (1.0f - uv.y * 2.0f) * tanY, 1.0f);
        const float skinZ = n.b * rsqrt(dot(ray, ray));			// the skin's view z from its camera distance (COLOR3.b)
        [branch] if (z < skinZ * 0.99f - 2.0f) return float4(0.0f, 0.0f, 0.0f, -1.0f);
    }
    // (the cap was 64: a dark texel's L = light / albedo runs past it in lamplight, and a capped tap is a darker one; fp16 holds 65504)
    return float4(min(diffuse * Participation(a.rgb) / AlbedoFactor(a.rgb), 4096.0f), max(z, 1.0e-3f));
}
#else	// SSS_BLUR
sampler2D SSS_Light  : register(s0);	// SSS_PREP's target
sampler2D SSS_Frame  : register(s1);	// the frame's copy, linear
sampler2D SSS_Albedo : register(s2);
sampler2D SSS_Spec   : register(s3);


float3 Participation(float3 a) { return smoothstep(0.01f, 0.04f, pow(max(a, 0.0f), 1.0f / 2.2f)); }	// (as SSS_PREP)
float3 AlbedoFactor(float3 a) { return pow(max(a, 1.0e-4f), SSS_Ctrl.z); }

// (2026-11-16 fd-fwdgroups, the indoor black smear of batch 8) The prep target holds L x participation. A dark texel's tap (moustache,
// stubble, the eye's rim) therefore brought 0 at FULL weight, and every skin pixel within the disc's reach was pulled toward black. Now each
// tap's weight is multiplied by its own participation (its albedo read here), so the disc averages the light of the texels that take part
// only: blurred = sum(w p L) / sum(w p), with L = the prep's value / p; the pixel then gets back p x albedo^detail x blurred (its own dark
// share keeps its own light, as before). Per colour throughout (the old fallback keyed every colour on red's weight).
// The disc also turns per pixel (interleaved gradient noise on the pixel), not only per frame: 24 taps over Radius 2's ~70 pixels left the
// same 24 copies at every pixel of a still frame (the posterised look of D2); per pixel they are noise, which TAA / FSR / DLSS average.
float IGN(float2 px) { return frac(52.9829189f * frac(dot(px, float2(0.06711056f, 0.00583715f)))); }

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    const float4 c = tex2Dlod(SSS_Light, float4(uv, 0.0f, 0.0f));
    [branch] if (SSS_Debug.x > 1.5f && c.a < 0.0f) return float4(1.0f, 0.0f, 0.0f, 1.0f);	// DebugView 2: covered skin, left out
    clip(c.a - 1.0e-4f);	// not deferred skin (or covered: a -1)
    [branch] if (SSS_Debug.x > 0.5f && SSS_Debug.x < 1.5f) return float4(c.rgb * 0.25f, 1.0f);	// DebugView 1: the prep's light
    const float z0 = c.a;
    const float pxPerD = SSS_Ctrl.x * SSS_View.z / z0;	// the reference distance d in pixels here
    const float turn = SSS_View.w + 6.2831853f * IGN(floor(uv / SSS_View.xy));
    float3 sum = 0.0f, weight = 0.0f;
    [unroll] for (int i = 0; i < SSS_TAPS; i++) {
        const float4 tap = SSS_Taps[i];
        float s, co;
        sincos(turn + 2.39996323f * (float)i, s, co);
        const float4 at = float4(uv + float2(co, s) * (tap.x * pxPerD) * SSS_View.xy, 0.0f, 0.0f);
        const float4 t = tex2Dlod(SSS_Light, at);
        // depth-aware: the tap is the same surface only within about its own radius in depth (DepthFollow stricter); not skin: a = 0
        const float reach = tap.x * SSS_Ctrl.x * 2.0f / max(SSS_Ctrl.y, 0.1f) + 0.05f;
        const float3 w = tap.yzw * (t.a > 0.0f ? saturate(1.0f - abs(t.a - z0) / reach) : 0.0f) * Participation(tex2Dlod(SSS_Albedo, at).rgb);
        sum += t.rgb * tap.yzw * (t.a > 0.0f ? saturate(1.0f - abs(t.a - z0) / reach) : 0.0f);	// (t.rgb is already L x p)
        weight += w;
    }
    const float4 a = tex2Dlod(SSS_Albedo, float4(uv, 0.0f, 0.0f));
    const float3 part = Participation(a.rgb);
    const float keep = saturate(FDAddedLightKeep(a.a));
    const float3 spec = tex2Dlod(SSS_Spec, float4(uv, 0.0f, 0.0f)).rgb * keep;
    const float4 frame4 = tex2Dlod(SSS_Frame, float4(uv, 0.0f, 0.0f));
    const float3 frame = frame4.rgb;
    const float3 diffuse = max(frame - spec, 0.0f);
    const float3 own = diffuse / AlbedoFactor(a.rgb);	// this pixel's own L (where no tap took part: unchanged)
    const float3 blurred = float3(weight.r > 1.0e-5f ? sum.r / weight.r : own.r, weight.g > 1.0e-5f ? sum.g / weight.g : own.g, weight.b > 1.0e-5f ? sum.b / weight.b : own.b);
    // the participating share of the light takes the blurred value; the dark texels keep their own
    // (2026-11-16 bench audit) the frame's alpha kept as the passes left it (it was written 1: later readers of the scene's alpha saw it changed
    // on the skin only while the disc ran)
    return float4(spec + diffuse * (1.0f - part) + part * blurred * AlbedoFactor(a.rgb), frame4.a);
}
#endif
