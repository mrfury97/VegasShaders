// Dream: a trip look for the finished picture. Ported from the author's own DreamSim shaders (godot/shaders/mix.gdshader and
// post.gdshader), written fresh in HLSL from that GLSL. Off by default (Shaders > Extras > Dream); src/effects/Dream/README.md.
//
// Technique 0 DreamMix:        tracers (the last frames linger), the stare tunnel (a swirl, then a hexagonal kaleidoscope zooming in
//                              forever) and datamosh blocks. Reads this frame and the effect's own history; writes the other history.
// Technique 1 DreamPost:       dream warp, colour split, macroblocks, the fractal melt, hue drift, saturation, vignette, 15-bit Bayer
//                              dither. Reads the history the mix pass just wrote.
// Technique 2 DreamPostDirect: the same as 1 straight from the picture, when tracers, tunnel and datamosh are all idle (no mix pass).
// All the levels, timers and random events are worked out on the CPU (DreamEffect::UpdateConstants).

float4 TESR_DreamLevel;		// x dream level now (0-1), y seconds (the effect's clock), z hue angle (radians), w warp
float4 TESR_DreamTrails;	// x tracer mix (0 = none), y tunnel (0-1), z datamosh block chance, w macroblock chance
float4 TESR_DreamMelt;		// x melt (0-1), y fractal variant (1-4), z fractal zoom, w colour split (uv)
float4 TESR_DreamFinish;	// x vignette, y dither, z extra saturation, w unused
float4 TESR_DreamGrid;		// xy the low-res grid the blocks and the dither follow (DreamSim's pixels), z macroblock seed, w datamosh seed

// (TESR_ samplers bind in declaration order.)
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DreamHistory : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

// ---- small helpers -------------------------------------------------------------------------------------------------------

float Hash(float2 p)
{
	return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

// The always-positive remainder (GLSL mod), unlike HLSL fmod for negative values.
float PosMod(float x, float y)
{
	return x - y * floor(x / y);
}

// Mirrored repeat of a uv, so a warp that runs off the screen folds back into it.
float2 Fold(float2 u)
{
	return 1.0f - abs(u - 2.0f * floor(u * 0.5f) - 1.0f);
}

// Turns the colour's hue by h radians (rotation in YIQ, the brightness kept).
float3 HueTurn(float3 c, float h)
{
	float y = dot(c, float3(0.299f, 0.587f, 0.114f));
	float i = dot(c, float3(0.596f, -0.274f, -0.322f));
	float q = dot(c, float3(0.211f, -0.523f, 0.312f));
	float s, k;
	sincos(h, s, k);
	float i2 = i * k - q * s;
	float q2 = i * s + q * k;
	return float3(y + 0.956f * i2 + 0.621f * q2, y - 0.272f * i2 - 0.647f * q2, y - 1.106f * i2 + 1.703f * q2);
}

float3 Fetch(sampler2D s, float2 uv)
{
	return tex2Dlod(s, float4(uv, 0.0f, 0.0f)).rgb;
}

// Ordered 4 x 4 dither threshold (0-1) of a grid cell.
float Bayer2(float2 a)
{
	a = floor(a);
	return frac(a.x * 0.5f + a.y * a.y * 0.75f);
}

float Bayer4(float2 a)
{
	return Bayer2(0.5f * a) * 0.25f + Bayer2(a);
}

// ---- pass 1: tracers, the stare tunnel, datamosh -------------------------------------------------------------------------

// A crisp sample of this frame with red and blue pulled a little apart (s = how far).
float3 SplitNow(float2 q, float s)
{
	float2 o = float2(0.003f * s, 0.0f);
	return float3(Fetch(TESR_RenderedBuffer, q + o).r, Fetch(TESR_RenderedBuffer, q).g, Fetch(TESR_RenderedBuffer, q - o).b);
}

float3 Tunnel(float2 uv, float stare, float t)
{
	float2 aspect = float2(TESR_DreamGrid.x / TESR_DreamGrid.y, 1.0f);
	float2 c = (uv - 0.5f) * aspect;
	float r = max(length(c), 1e-4f);
	float a = atan2(c.y, c.x);

	// The swirl: strongest in the middle, with a slow ripple running outwards.
	float twist = stare * 1.6f * exp(-r * 2.5f) + 0.05f * stare * sin(r * 14.0f - t * 2.0f);
	float2 swirled = float2(cos(a + twist), sin(a + twist)) * r * (1.0f - 0.12f * stare * sin(t * 0.8f + r * 10.0f));
	float3 swirl = SplitNow(0.5f + swirled / aspect, stare);

	// The kaleidoscope: hexagonal rings, each half the size of the one outside it, falling inwards for ever; inside each ring
	// a six-fold mirrored wedge of the picture, its hue turned per ring.
	const float outer = 0.62f;
	const float ln2 = 0.693147f;
	float hexR = max(max(abs(c.x) * 0.8660254f + abs(c.y) * 0.5f, abs(c.y)), 1e-4f);
	float depth = log(hexR / outer) / ln2 + t * 0.15f * stare;
	float ring = floor(depth);
	float inRing = depth - ring;
	const float wedge = 6.2831f / 6.0f;
	float wa = a + ring * 0.5f + t * 0.05f;
	wa = abs(PosMod(wa, wedge) - wedge * 0.5f) + 0.35f;
	float2 dir = float2(cos(wa), sin(wa));
	dir /= max(abs(dir.x) * 0.8660254f + abs(dir.y) * 0.5f, abs(dir.y));	// straight hexagon sides, not circles
	float3 nested = HueTurn(SplitNow(0.5f + dir * outer * exp(ln2 * (inRing - 1.0f)) / aspect, stare), ring * 0.9f + t * 0.2f);
	nested *= smoothstep(0.0f, 0.05f, inRing) * (1.0f - smoothstep(0.95f, 1.0f, inRing)) * 0.85f + 0.15f;	// dark seams between rings

	float3 d = lerp(swirl, nested, smoothstep(0.45f, 1.0f, stare));
	d = HueTurn(d, stare * 1.4f * sin(t * 0.3f));
	float grey = dot(d, float3(0.3f, 0.59f, 0.11f));
	return lerp(grey.xxx, d, 1.0f + 0.9f * stare);
}

float4 MixPS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float t = TESR_DreamLevel.y;
	float stare = TESR_DreamTrails.y;

	float3 col = lerp(Fetch(TESR_RenderedBuffer, uv), Fetch(TESR_DreamHistory, uv), TESR_DreamTrails.x);
	[branch] if (stare > 0.0f)
		col = lerp(col, Tunnel(uv, stare, t), smoothstep(0.0f, 0.25f, stare));

	// Datamosh: some 8 x 8 blocks keep the old picture, smeared sideways.
	float2 block = floor(uv * TESR_DreamGrid.xy / 8.0f);
	[branch] if (Hash(block + TESR_DreamGrid.w) < TESR_DreamTrails.z)
		col = Fetch(TESR_DreamHistory, uv + float2((Hash(block * 1.7f) - 0.5f) * 0.04f, 0.004f));
	return float4(col, 1.0f);
}

// ---- pass 2: warp, split, macroblocks, melt, hue, saturation, vignette, dither ---------------------------------------------

// The fractal melt: each pixel slides from where it is towards its place in a Julia set as f rises, the view zooming towards
// the set's repelling fixed point, then the set's own colours bleed in.
float3 Melt(sampler2D source, float2 uv, float f, float variant, float zoom, float t)
{
	float2 aspect = float2(TESR_DreamGrid.x / TESR_DreamGrid.y, 1.0f) * 2.4f;
	float a = t * 0.11f + variant * 1.7f;
	float2 c;
	if (variant < 1.5f)      c = 0.7885f * float2(cos(a), sin(a));
	else if (variant < 2.5f) c = float2(-0.8f, 0.156f) + 0.05f * float2(sin(a * 1.3f), cos(a));
	else if (variant < 3.5f) c = float2(-0.123f, 0.745f) + 0.03f * float2(sin(a), cos(a * 0.7f));
	else                     c = float2(0.285f, 0.01f) + 0.02f * float2(cos(a * 2.0f), sin(a * 2.0f));

	// The repelling fixed point of z^2 + c: (1 + sqrt(1 - 4c)) / 2, the complex square root taken by hand.
	float2 m = float2(1.0f, 0.0f) - 4.0f * c;
	float ml = length(m);
	float2 root = float2(sqrt(0.5f * (ml + m.x)), sign(m.y) * sqrt(max(0.0f, 0.5f * (ml - m.x))));
	float2 fixedPoint = 0.5f * (float2(1.0f, 0.0f) + root);

	float2 z = (uv - 0.5f) * aspect / zoom + fixedPoint * (1.0f - 1.0f / zoom);
	float steps = 0.0f;
	float escaped = 0.0f;
	[loop] for (int k = 0; k < 48; k++)
	{
		z = float2(z.x * z.x - z.y * z.y, 2.0f * z.x * z.y) + c;
		if (dot(z, z) > 64.0f) { escaped = 1.0f; break; }
		steps += 1.0f;
	}
	float smoothSteps = steps + 1.0f - log2(max(0.5f * log2(dot(z, z)), 1e-3f));
	float2 target = escaped > 0.5f ? float2(smoothSteps / 10.0f + t * 0.03f, 0.5f + atan2(z.y, z.x) / 6.2831f) : 0.5f + z / aspect;
	float3 s = Fetch(source, Fold(lerp(uv, target, f)));
	float grey = dot(s, float3(0.3f, 0.59f, 0.11f));
	float3 palette = 0.5f + 0.5f * cos(6.2831f * (smoothSteps / 24.0f + grey + float3(0.0f, 0.33f, 0.67f) + t * 0.05f));
	return lerp(s, s * 0.55f + palette * 0.6f, f * f * f * (0.3f + 0.7f * escaped));
}

float3 Post(sampler2D source, float2 uv0)
{
	float t = TESR_DreamLevel.y;
	float warp = TESR_DreamLevel.w;
	float2 grid = TESR_DreamGrid.xy;

	// The dream warp: a slow twist of the middle of the picture and a fine wobble over all of it.
	float2 c = uv0 - 0.5f;
	float r = length(c);
	float turn = warp * 0.35f * sin(t * 0.4f) * (1.0f - smoothstep(0.0f, 0.6f, r));
	float s, k;
	sincos(turn, s, k);
	float2 uv = 0.5f + float2(c.x * k + c.y * s, c.y * k - c.x * s);
	uv += warp * 0.006f * float2(sin(uv.y * 40.0f + t * 2.0f), cos(uv.x * 34.0f + t * 1.6f));

	// Macroblocks: whole 8 x 8 blocks drop to one flat, posterised colour; a few flash green.
	float2 block = floor(uv * grid / 8.0f);
	float roll = Hash(block + TESR_DreamGrid.z);
	float blockChance = TESR_DreamTrails.w;
	bool macro = roll < blockChance;
	if (macro) uv = (block + 0.5f) * 8.0f / grid;

	// Colour split: red one way, blue the other.
	float2 o = float2(TESR_DreamMelt.w, 0.0f);
	float3 col = float3(Fetch(source, uv + o).r, Fetch(source, uv).g, Fetch(source, uv - o).b);
	if (macro) col = floor(col * 4.0f) / 4.0f + (roll < blockChance * 0.2727f ? float3(-0.15f, 0.2f, -0.15f) : float3(0.0f, 0.0f, 0.0f));

	[branch] if (TESR_DreamMelt.x > 0.001f)
		col = Melt(source, uv, TESR_DreamMelt.x, TESR_DreamMelt.y, TESR_DreamMelt.z, t);

	if (TESR_DreamLevel.z != 0.0f) col = HueTurn(col, TESR_DreamLevel.z);	// (the YIQ round trip is not exact: skipped at 0)
	float grey = dot(col, float3(0.299f, 0.587f, 0.114f));
	col = lerp(grey.xxx, col, 1.0f + TESR_DreamFinish.z);
	col *= 1.0f - TESR_DreamFinish.x * smoothstep(0.35f, 0.85f, r);

	// 15-bit colour (31 steps a channel) with a 4 x 4 ordered dither on the low-res grid.
	float threshold = Bayer4(floor(uv * grid)) - 0.5f;
	float3 crushed = floor(col * 31.0f + 0.5f + threshold) / 31.0f;
	return lerp(col, crushed, TESR_DreamFinish.y);
}

float4 PostHistoryPS(VSOUT IN) : COLOR0
{
	return float4(Post(TESR_DreamHistory, IN.UVCoord), 1.0f);
}

float4 PostDirectPS(VSOUT IN) : COLOR0
{
	return float4(Post(TESR_RenderedBuffer, IN.UVCoord), 1.0f);
}

technique DreamMix
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 MixPS();
	}
}

technique DreamPost
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 PostHistoryPS();
	}
}

technique DreamPostDirect
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader  = compile ps_3_0 PostDirectPS();
	}
}
