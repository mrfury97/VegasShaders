// GodRays: soft light shafts from the sun's screen position, from bright sky only (rewritten 2026-11-02, wave 3, from
// docs/EFFECTS.md "GodRays"). Techniques, as GodRays.cpp asks for them:
//   DedicatedGodRays   SkyMask, LightMask, Blur1, Blur2, Blur3, Combine     (6 passes, the main path)
//   GodRaysMaxReduce   Reduce     the 8 x 8 maximum of the light mask (SkipEmpty, SkipIdle check)
//   GodRaysIdleTest    Test       discards unless the sky maximum reaches the limit (SkipIdle; NaN counts as bright)
//   (unnamed)          the same six passes as DedicatedGodRays (the spec's packed legacy path is not reproduced: fallback only)
// Registers are explicit in the spec (s0 scene, s1 depth, s5 ray buffer, s6 maximum); the C++ binds the ping-pong to s5.
// 2026-11-02 (1.0.1): the maths is the P85 one again. The spec took the sun's "height" for its elevation, but the old code's
// height is 1 - elevation (0 overhead, 1 at the horizon): the light mask's threshold, the time factor and the sunset weight all
// came out reversed, so at dusk almost the whole sky turned into rays (a tester: "the sun is a lot brighter"; same picture at
// noon, where both forms give the same value). Also back: the light mask's clamp on the colour, the horizon fade of the sky mask,
// the half-resolution texel offset, the blur's aspect-correct unit steps, the RenderScale (rect) conversions, and GetSunColor.

#include "Includes/Helpers.hlsl"

// ---- samplers (explicit slots, as the spec's section 3a) ----
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D NVR_GodRaysBuffer : register(s5) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D NVR_GodRaysMax : register(s6) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };

// ---- constants (registered by GodRaysEffect) ----
float4 TESR_ReciprocalResolution;   // xy 1 / screen size, z height / width
float4 TESR_SunColor;               // rgb sun colour (linearised here)
float4 TESR_GodRaysRay;             // x RayIntensity, y RayLength, w SunsetGlareFix (1 on)
float4 TESR_GodRaysRayColor;        // rgb custom ray colour, w Saturate (0..1)
float4 TESR_GodRaysData;            // y Luminance threshold, z the per-frame day/night strength, w TimeEnabled
float4 TESR_SunPosition;            // xyz sun disc direction; z = its elevation (0 at the horizon, 1 at the zenith)
float4 TESR_SunDirection;           // xyz sun direction for the ray colour (its z is the elevation used there)
float4 TESR_SunAmount;              // x day factor (0 night .. 1 day)
float4 TESR_SunsetColor;            // rgb sunset colour boost
float4 TESR_ViewSpaceLightDir;      // view-space sun direction (not necessarily unit length)
float4 NVR_GodRaysSkip;             // x SkipEmpty reduction ran, y SkipIdle on, z SkipIdle limit
float4 NVR_GodRaysReduce;           // xy 1 / source size of the reduction step
float4 NVR_GodRaysLayout;           // xy ray buffer / full extent, zw 1 / ray buffer size

// Depth.hlsl: readDepth, reconstructPosition, projectPosition, TESR_CameraData, the transforms, rectToScreen / screenToRect
// (RenderScale); Sky.hlsl: GetSunColor (the depth sampler must precede them).
#include "Includes/Depth.hlsl"
#include "Includes/Sky.hlsl"

// ---- vertex shaders: the frame quad (ShaderManager::CreateFrameVertex), and the same moved to the ray buffer's texel centres ----
struct VSIN { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) {
	VSOUT o;
	o.pos = float4(IN.pos, 1.0f);
	o.uv = IN.uv;
	return o;
}
// The quad carries a full-resolution half-texel offset; the ray buffer is half resolution, so its texel centres are half a
// (half-resolution) texel further.
VSOUT HalfVS(VSIN IN) {
	VSOUT o = FrameVS(IN);
	o.uv += 0.5f * (NVR_GodRaysLayout.zw - TESR_ReciprocalResolution.xy);
	return o;
}

// ---- sun and view helpers ----
// How low the sun is: 0 overhead, 1 at the horizon, above 1 below it (the old code's "sun height").
float GRLow() { return 1.0f - max(TESR_SunPosition.z, 0.0f); }
float GRSunset() { return pows(GRLow(), 8.0f); }                                // 1 at the horizon, near 0 with the sun high
float3 GRSunView() { return TESR_ViewSpaceLightDir.xyz; }

// The view-space position for a depth already read (reconstructPosition's maths, one depth fetch fewer).
float3 GRPositionAt(float2 uv, float linearDepth) {
	float2 s = rectToScreen(uv);
	float4 v = mul(float4(s.x * 2.0f - 1.0f, (1.0f - s.y) * 2.0f - 1.0f, projectedDepthFromLinear(linearDepth), 1.0f), TESR_InvProjectionTransform);
	return v.xyz / v.w;
}

// The sun's projected screen position (not the rect one), as the blurs march in screen positions.
float2 GRSunScreen() { return rectToScreen(projectPosition(GRSunView() * farZ).xy); }

// Ray attenuation: the sun against the view, times the time factor (TimeEnabled: weaker with the sun high), zero below the horizon.
float GRAttenuation(float2 uv) {
	float3 eye = normalize(reconstructPosition(uv));
	float timeF = TESR_GodRaysData.w ? lerp(0.2f, 4.0f, pows(GRLow(), 4.0f)) : 1.0f;
	return pow(compress(shade(GRSunView(), eye)), 2.5f) * timeF * (GRLow() < 1.0f);
}

bool GRAllEmpty() { return NVR_GodRaysSkip.x > 0.5f && tex2Dlod(NVR_GodRaysMax, float4(0.5f, 0.5f, 0.0f, 0.0f)).r <= 0.0f; }

// ---- pass 0: SkyMask. The scene plus the sun glare, in sky pixels only (zero on the world and with the sun down). The world is
// decided from the depth first, so its pixels skip the scene fetch and the glare (explicit-LOD fetches inside the branch). ----
float4 PS_SkyMask(float2 uv : TEXCOORD0) : COLOR0 {
	float depthRaw = tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x;
	float sky = depthRaw > 0.9f;
	float horizon = smoothstep(0.0f, 0.01f, GRLow());
	[branch] if (sky * horizon == 0.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

	float sunset = GRSunset();
	float fix = step(0.5f, TESR_GodRaysRay.w);
	float boostW = lerp(1.0f - sunset, sunset, fix);                             // SunsetGlareFix: the boost near the horizon only
	float3 glareCol = linearize(TESR_SunColor).rgb + linearize(TESR_SunsetColor.rgb) * boostW;
	float glarePow = lerp(0.1f, 8.0f, sunset);
	float glare = pows(dot(GRSunView(), normalize(GRPositionAt(uv, depthRaw))), 180.0f) * glarePow;
	float3 scene = linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f))).rgb;
	return float4((scene + glare * glareCol) * sky * horizon, 1.0f);
}

// ---- pass 1: LightMask. Five taps (the pixel and its diagonal neighbours one texel away), averaged; keeps the bright sky above a
// threshold that is 2 with the sun high and falls to 0 at the horizon, smoothly over 15 x Luminance; scaled by 100 and RayIntensity
// and held to 0..1 ----
float4 PS_LightMask(float2 uv : TEXCOORD0) : COLOR0 {
	float2 t = NVR_GodRaysLayout.zw;
	float3 avg = (tex2D(NVR_GodRaysBuffer, uv).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(-t.x, -t.y)).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(-t.x, t.y)).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(t.x, -t.y)).rgb
	            + tex2D(NVR_GodRaysBuffer, uv + float2(t.x, t.y)).rgb) / 5.0f;
	float thr = lerp(2.0f, 0.0f, pow(abs(GRLow()), 8.0f));
	float ramp = smoothstep(thr, thr + TESR_GodRaysData.y * 15.0f, luma(avg));
	return float4(saturate(ramp * avg * 100.0f * TESR_GodRaysRay.x), 1.0f);
}

// ---- passes 2 to 4: the radial blurs. 10 taps from the pixel toward the sun's screen position, in steps of stepF x RayLength
// measured with the width / height ratio taken out; a tap counts when it is within the distance to the sun and inside the screen;
// the sum over one more than the count. Blur3 keeps the ray attenuation in alpha. ----
float4 GRBlur(float2 uvIn, float stepF, float store) {
	[branch] if (GRAllEmpty()) return float4(0.0f, 0.0f, 0.0f, 1.0f);       // SkipEmpty: nothing to blur this frame
	float2 rectUV = uvIn - 0.5f * TESR_ReciprocalResolution.xy;
	float2 uv = rectToScreen(rectUV);
	float aspect = 1.0f / TESR_ReciprocalResolution.z;
	float2 toSun = (GRSunScreen() - uv) * float2(1.0f, aspect);
	float reach = length(toSun);
	float2 dir = toSun / max(reach, 0.000001f);
	float stepLen = stepF * TESR_GodRaysRay.y;
	float3 sum = 0.0f;
	float count = 1.0f;
	for (float i = 0.0f; i < 10.0f; i++) {
		float along = stepLen * i;
		float2 tap = uv + dir * along / float2(1.0f, aspect);
		[branch] if (along <= reach && tap.x > 0.0f && tap.y > 0.0f && tap.x < 1.0f && tap.y < 1.0f) {
			sum += tex2Dlod(NVR_GodRaysBuffer, float4(screenToRect(tap), 0.0f, 0.0f)).rgb;
			count += 1.0f;
		}
	}
	return float4(sum / count, lerp(1.0f, GRAttenuation(rectUV), store));
}

float4 PS_Blur1(float2 uv : TEXCOORD0) : COLOR0 { return GRBlur(uv, 0.1f, 0.0f); }
float4 PS_Blur2(float2 uv : TEXCOORD0) : COLOR0 { return GRBlur(uv, 0.01f, 0.0f); }
float4 PS_Blur3(float2 uv : TEXCOORD0) : COLOR0 { return GRBlur(uv, 0.001f, 1.0f); }

// ---- pass 5: Combine. Linear scene plus 5 x rays x scene plus 0.2 x rays; the ray colour is the sun colour (GetSunColor: brighter
// with elevation, the sunset boost by the day factor) or the custom colour, by Saturate; the rays carry the strength and the Blur3
// attenuation. Where the rays are exactly zero and SkipIdle is on, the scene is returned as stored (a linearize round trip is not
// exact on the GPU). ----
float4 PS_Combine(float2 uv : TEXCOORD0) : COLOR0 {
	float4 stored = tex2D(TESR_RenderedBuffer, uv);
	float4 r = tex2D(NVR_GodRaysBuffer, uv);
	[branch] if (NVR_GodRaysSkip.y > 0.5f && !any(r.rgb)) return float4(stored.rgb, 1.0f);
	float3 sunCol = GetSunColor(shade(TESR_SunDirection.xyz, blue.xyz), 1.0f, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
	float3 rayCol = lerp(sunCol, linearize(TESR_GodRaysRayColor).rgb, TESR_GodRaysRayColor.w);
	float4 rays = r;
	rays.rgb *= TESR_GodRaysData.z * rayCol;
	rays.rgb *= rays.a;
	float4 lin = linearize(stored);
	lin += max(rays, 0.0f) * 5.0f * lin + max(rays, 0.0f) * 0.2f;
	return float4(delinearize(lin).rgb, 1.0f);
}

// ---- reduction: one 8 x 8 block of the step's source per output pixel, its largest channel (texel centres: the linear sampler
// returns each texel as it is; past the edge the clamp repeats it, which cannot raise the maximum) ----
float4 PS_Reduce(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
	float2 origin = floor(vpos) * 8.0f + 0.5f;
	float m = 0.0f;
	[unroll] for (int j = 0; j < 8; j++) {
		[unroll] for (int i = 0; i < 8; i++) {
			float3 c = tex2Dlod(NVR_GodRaysBuffer, float4((origin + float2(i, j)) * NVR_GodRaysReduce.xy, 0.0f, 0.0f)).rgb;
			m = max(m, max(c.r, max(c.g, c.b)));
		}
	}
	return float4(m, m, m, 1.0f);
}

// ---- the idle test: discard unless the sky maximum reaches the limit (NaN is not discarded) ----
float4 PS_IdleTest(float2 uv : TEXCOORD0) : COLOR0 {
	clip(tex2Dlod(NVR_GodRaysMax, float4(0.5f, 0.5f, 0.0f, 0.0f)).r - NVR_GodRaysSkip.z);
	return float4(1.0f, 1.0f, 1.0f, 1.0f);
}

// ---- techniques ----
technique
{
	pass SkyMask { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_SkyMask(); }
	pass LightMask { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_LightMask(); }
	pass Blur1 { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_Blur1(); }
	pass Blur2 { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_Blur2(); }
	pass Blur3 { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_Blur3(); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Combine(); }
}

technique DedicatedGodRays
{
	pass SkyMask { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_SkyMask(); }
	pass LightMask { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_LightMask(); }
	pass Blur1 { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_Blur1(); }
	pass Blur2 { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_Blur2(); }
	pass Blur3 { VertexShader = compile vs_3_0 HalfVS(); PixelShader = compile ps_3_0 PS_Blur3(); }
	pass Combine { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Combine(); }
}

technique GodRaysMaxReduce
{
	pass Reduce { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Reduce(); }
}

technique GodRaysIdleTest
{
	pass Test { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_IdleTest(); }
}
