// SunGlare (UNOFFICIAL 2026-11-13, modern lighting): the sun's lens glare, on the HDR picture before the tone mapper (after the
// god rays). Techniques: (unnamed) the picture pass; SunGlareCover the cover measure into one texel (SunGlare.cpp RunCover). Round the sun's place on screen it adds a tight halo, a starburst (thin lines through the sun,
// each a pair of points, of differing length) and a long horizontal streak, all times how much of the sun's disc shows:
//   - the share of a small patch round the sun's centre that is sky in the depth buffer (trees, walls, the weapon cover it), and
//   - how much brighter that patch is than the sky a few sun widths out (behind thick cloud it is no brighter: no glare).
// The cover is measured once a frame into TESR_SunGlareCover (1 x 1) and eased there over a few frames: the pass blends its
// value over what the texel held. Positions are measured in screen heights, so the shape is the same at any resolution and
// aspect. Pixels the glare cannot reach return the picture as stored. src/effects/SunGlare/README.md.

#include "Includes/Helpers.hlsl"

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };
sampler2D TESR_SunGlareCover : register(s2) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };

float4 TESR_ReciprocalResolution;   // xy 1 / screen size, z height / width
float4 TESR_SunColor;               // rgb the sun's colour (display-encoded)
float4 TESR_SunsetColor;            // rgb the sunset colour boost
float4 TESR_SunDirection;           // xyz the sun's direction (z its elevation, for the colour)
float4 TESR_SunAmount;              // x day factor
float4 TESR_ViewSpaceLightDir;      // view-space sun direction
float4 TESR_SunGlareData;           // x strength (0: nothing), y halo size, z halo strength, w debug view
float4 TESR_SunGlareSpikes;         // x lines through the sun, y spike length, z spike width, w spike strength
float4 TESR_SunGlareStreak;         // x streak length, y streak width, z streak strength, w chromatic
float4 TESR_SunGlareSun;            // x the cover patch's radius, y z the cloud contrast ramp, w the share one frame adds to the eased cover
float4 TESR_SunGlareTurn;           // xy cos / sin of the first line's angle, zw cos / sin of the step between lines

#include "Includes/Depth.hlsl"
#include "Includes/Sky.hlsl"

struct VSIN { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) {
	VSOUT o;
	o.pos = float4(IN.pos, 1.0f);
	o.uv = IN.uv;
	return o;
}

#define SG_COVER_TAPS 16
#define SG_RING_TAPS 8
#define SG_MAX_LINES 8

// The sun's place on screen (screen 0..1, not the rect), as the god rays find it.
float2 SGSunScreen() { return rectToScreen(projectPosition(normalize(TESR_ViewSpaceLightDir.xyz) * farZ).xy); }

// How much of the sun shows, 0..1. x: the sky share of the patch (depth), y: the patch's brightness over the ring's (cloud).
float2 SGCover(float2 sun) {
	const float aspect = TESR_ReciprocalResolution.z;	// height / width: a screen-height step in x is this much of the width
	const float2x2 golden = float2x2(-0.7373688f, -0.6754904f, 0.6754904f, -0.7373688f);
	float2 dir = float2(1.0f, 0.0f);
	float open = 0.0f;
	float patch = 0.0f;
	[unroll] for (int i = 0; i < SG_COVER_TAPS; i++) {
		float2 at = sun + dir * (TESR_SunGlareSun.x * sqrt((i + 0.5f) / SG_COVER_TAPS)) * float2(aspect, 1.0f);
		float inside = (at.x > 0.0f && at.y > 0.0f && at.x < 1.0f && at.y < 1.0f) ? 1.0f : 0.0f;
		float2 rect = screenToRect(saturate(at));
		float sky = tex2Dlod(TESR_DepthBuffer, float4(rect, 0.0f, 0.0f)).x > 0.9f ? inside : 0.0f;
		open += sky;
		patch += luma(linearize(tex2Dlod(TESR_RenderedBuffer, float4(rect, 0.0f, 0.0f)).rgb)) * sky;
		dir = mul(golden, dir);
	}
	float ring = 0.0f;
	float ringCount = 0.0f;
	dir = float2(0.9238795f, 0.3826834f);
	const float2x2 eighth = float2x2(0.7071068f, -0.7071068f, 0.7071068f, 0.7071068f);
	[unroll] for (int j = 0; j < SG_RING_TAPS; j++) {
		float2 at = sun + dir * (TESR_SunGlareSun.x * 7.0f) * float2(aspect, 1.0f);
		float2 rect = screenToRect(saturate(at));
		float sky = (at.x > 0.0f && at.y > 0.0f && at.x < 1.0f && at.y < 1.0f && tex2Dlod(TESR_DepthBuffer, float4(rect, 0.0f, 0.0f)).x > 0.9f) ? 1.0f : 0.0f;
		ring += luma(linearize(tex2Dlod(TESR_RenderedBuffer, float4(rect, 0.0f, 0.0f)).rgb)) * sky;
		ringCount += sky;
		dir = mul(eighth, dir);
	}
	float share = open / SG_COVER_TAPS;
	// no open sky on the ring (the sun in a gap between leaves): the patch alone decides, against a plain bright level
	float contrast = (patch / max(open, 1.0f)) / max(ringCount > 0.5f ? ring / ringCount : 0.25f, 1.0e-4f);
	return float2(share, smoothstep(TESR_SunGlareSun.y, TESR_SunGlareSun.z, contrast));
}

float4 PS_SunGlare(float2 uv : TEXCOORD0) : COLOR0 {
	float4 stored = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f));
	const float strength = TESR_SunGlareData.x;
	const float aspect = 1.0f / TESR_ReciprocalResolution.z;	// width / height
	float2 sun = SGSunScreen();
	// the pixel from the sun, in screen heights
	float2 d = (rectToScreen(uv) - sun) * float2(aspect, 1.0f);
	float r = length(d);

	// what the glare can add here at most, before any cover tap: the halo, the longest spike, the streak
	float halo = TESR_SunGlareData.z * (exp(-r / TESR_SunGlareData.y) + 0.12f * exp(-r / (TESR_SunGlareData.y * 5.0f)));
	float spikeReach = TESR_SunGlareSpikes.w * exp(-r * 0.7071f / TESR_SunGlareSpikes.y);	// (a pixel is at least r / sqrt 2 along a line, or that far off it)
	float streak = TESR_SunGlareStreak.z * exp(-abs(d.y) / TESR_SunGlareStreak.y) / (1.0f + (d.x * d.x) / (TESR_SunGlareStreak.x * TESR_SunGlareStreak.x) * 9.0f);
	[branch] if ((halo + spikeReach + streak) * strength < 0.004f && TESR_SunGlareData.w < 0.5f) return float4(stored.rgb, 1.0f);

	// how much of the sun shows: this frame's measure eased over the last few (x), and the raw parts for the debug view
	float4 cover = tex2Dlod(TESR_SunGlareCover, float4(0.5f, 0.5f, 0.0f, 0.0f));
	float visible = cover.x;

	// the starburst: thin lines through the sun. Each line's two points differ in length (a fixed pattern by its number), and a
	// line is a little wider far from the sun, as a real point spreads.
	float spikes = 0.0f;
	float2 axis = TESR_SunGlareTurn.xy;
	[loop] for (int k = 0; k < SG_MAX_LINES; k++) {
		[branch] if (k >= TESR_SunGlareSpikes.x) break;
		float along = dot(d, axis);
		float across = abs(d.x * axis.y - d.y * axis.x);
		float pattern = frac(k * 0.6180340f + (along > 0.0f ? 0.31f : 0.77f));
		float len = TESR_SunGlareSpikes.y * (0.45f + 0.55f * pattern);
		float width = TESR_SunGlareSpikes.z * (1.0f + abs(along) / len);
		spikes += exp(-(across * across) / (width * width)) * exp(-abs(along) / len) / (1.0f + abs(along) / (len * 0.35f));
		axis = float2(axis.x * TESR_SunGlareTurn.z - axis.y * TESR_SunGlareTurn.w, axis.x * TESR_SunGlareTurn.w + axis.y * TESR_SunGlareTurn.z);
	}
	spikes *= TESR_SunGlareSpikes.w;

	// faint spectral colour along the points and the streak (white at the sun), the halo stays the sun's colour
	float3 spectrum = 1.0f + TESR_SunGlareStreak.w * cos(6.2831853f * (r / max(TESR_SunGlareSpikes.y, 1.0e-4f) * 1.5f + float3(0.0f, 0.33f, 0.67f))) * saturate(r / TESR_SunGlareData.y);
	float3 sunCol = GetSunColor(saturate(TESR_SunDirection.z), 1.0f, TESR_SunAmount.x, TESR_SunColor.rgb, TESR_SunsetColor.rgb);
	float3 glare = sunCol * (halo + (spikes + streak) * spectrum) * (strength * visible);

	[branch] if (TESR_SunGlareData.w > 1.5f) return float4(delinearize(glare), 1.0f);
	[branch] if (TESR_SunGlareData.w > 0.5f) {
		float3 tint = lerp(float3(1.0f, 0.2f, 0.2f), float3(0.2f, 1.0f, 0.2f), visible);
		return float4(stored.rgb * lerp(1.0f, tint, r < 0.25f ? 0.6f : 0.15f) + delinearize(glare), 1.0f);
	}
	return float4(delinearize(linearize(stored.rgb) + glare), 1.0f);
}

// The cover measure, one texel. Alpha is the share this frame contributes: the pass blends it over the texel's older value
// (SrcAlpha / InvSrcAlpha), so the glare eases in and out over EaseFrames frames instead of switching.
float4 PS_Cover(float2 uv : TEXCOORD0) : COLOR0 {
	float2 sun = SGSunScreen();
	// the sun off the screen cannot be measured: the glare fades out over the last 4 % before the edge
	float2 edge = min(sun, 1.0f - sun);
	float onScreen = saturate(min(edge.x, edge.y) / 0.04f + 1.0f);
	onScreen *= (TESR_ViewSpaceLightDir.z > 0.0f) ? 1.0f : 0.0f;
	float2 cover = SGCover(sun);
	return float4(cover.x * cover.y * onScreen, cover.x, cover.y, TESR_SunGlareSun.w);
}

technique
{
	pass SunGlare { VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_SunGlare(); }
}

technique SunGlareCover
{
	pass Cover {
		AlphaBlendEnable = TRUE; SrcBlend = SRCALPHA; DestBlend = INVSRCALPHA; BlendOp = ADD; SeparateAlphaBlendEnable = FALSE;
		AlphaTestEnable = FALSE; ZEnable = FALSE; ZWriteEnable = FALSE; CullMode = NONE; ColorWriteEnable = 15;
		VertexShader = compile vs_3_0 FrameVS(); PixelShader = compile ps_3_0 PS_Cover();
	}
}
