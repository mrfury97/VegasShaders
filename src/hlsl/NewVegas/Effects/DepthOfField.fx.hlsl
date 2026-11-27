// Depth of field post pass. Works out a per-pixel blur amount from depth and focus, packs the
// near and far blur masks into screen quadrants, gathers a poisson-disc bokeh, and recombines
// the layers. DistantOnly is the short path used when autofocus is off.

#define showDepth 0

float4 TESR_ReciprocalResolution;
float4 TESR_DepthOfFieldBlur; //x: distant blur, y:distant blur start, z: distant blur end, w: base blur radius
float4 TESR_DepthOfFieldData;
float4 TESR_DepthOfFieldJitter; // UNOFFICIAL 2026-11-01 JitterFix: xy the depth's jitter after TAA / FSR / DLSS (0 off); depth at uv - xy is the picture's at uv //x: blur fallout y:radius z:diameterRange w:nearblur cutoff
float4 TESR_MotionBlurData;
float4 TESR_DebugVar;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state {ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AvgLumaBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"

static const float distantStrength = max(TESR_DepthOfFieldBlur.x, 0.0f);
static const float blurFrom = TESR_DepthOfFieldBlur.y;
static const float blurTo = TESR_DepthOfFieldBlur.z;
static const float baseRadius = TESR_DepthOfFieldBlur.w;

static const float hyperD = max(0.0000001, TESR_DepthOfFieldData.x * 1000); // the distance at which DOF is greatest (infinite towards the distance and closest to the camera)
static const float bokehRadius = TESR_DepthOfFieldData.y;
static const float bokehCut = TESR_DepthOfFieldData.z;
static const float nearCut = min(hyperD, TESR_DepthOfFieldData.w);

// Focus distance comes from the blue channel of the AvgLuma buffer, which animates it. The stored value is a [0-1] fraction of hyperD.
static const float focusDist = tex2D(TESR_AvgLumaBuffer, float2(0.5, 0.5)).b * hyperD;

struct VSOUT
{
	float4 clipPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 clipPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN src)
{
	VSOUT res = (VSOUT)0.0f;
	res.clipPos = src.clipPos;
	res.UVCoord = src.UVCoord;
	return res;
}

static float2 poissonTaps[12] =
{
	{ -0.326212, -0.405810 },
	{ -0.840144, -0.073580 },
	{ -0.695914,  0.457137 },
	{ -0.203345,  0.620716 },
	{  0.962340, -0.194983 },
	{  0.473434, -0.480026 },
	{  0.519456,  0.767022 },
	{  0.185461, -0.893124 },
	{  0.507431,  0.064425 },
	{  0.896420,  0.412458 },
	{ -0.321940, -0.932615 },
	{ -0.791559, -0.597710 }
};

// Quadrant mask: x = top left, y = top right, z = bottom left, w = bottom right.
float4 quadMask(float2 p){
	float tl = (p.x < 0.5 && p.y < 0.5); // near field blurred
	float tr = (p.x > 0.5 && p.y < 0.5); // far field blurred
	float bl = (p.x < 0.5 && p.y > 0.5); // bottom quadrants hold low res copies of the blur amount passes
	float br = (p.x > 0.5 && p.y > 0.5); // bottom quadrants hold low res copies of the blur amount passes

	float4 q = float4(tl, tr, bl, br);
	return q;
}

// Maps a screen coordinate onto [0, 1] inside the quadrant it falls in.
float2 remapQuad(float2 p, float4 q){
	float2 mid = 0.5;
	float2 nx = float2(p.x, p.x - mid.x);
	float2 ny = float2(p.y, p.y - mid.y);

	p.x = dot(nx.xyxy, q);
	p.y = dot(ny.xxyy, q);

	p *= 2;
	return p;
}


float4 DoF(VSOUT IN) : COLOR0
{
	float depth = readDepth(IN.UVCoord - TESR_DepthOfFieldJitter.xy);	// UNOFFICIAL 2026-11-01 JitterFix
	float3 camVec = toWorld(IN.UVCoord) * depth;
	float4 worldP = float4(TESR_CameraPosition.xyz + camVec, 1.0f);

	float focalLen = 0.01; // mostly negligible and used here to avoid dividing by 0; In real life this value influences the HFD

	// Near and far limits of the sharp zone, from the hyperfocal distance (http://www.waloszek.de/gen_dof_e.php)
	float nearP = focusDist * (hyperD - focalLen)/(hyperD  + focusDist - 2 * focalLen);
	float farP = focusDist * (hyperD - focalLen)/(hyperD - focusDist);

	float nearB = invlerps(nearP, 0, depth);
	float farB = invlerps(farP, farP * 2, depth) * 0.8;

	nearB *= 0.5 * smoothstep(nearCut/2, 0, depth) + smoothstep(nearCut/2, nearCut, depth); // attenuate near blur to reveal gun
	farB = saturate(farB + invlerps(blurFrom, blurTo, depth) * distantStrength * invlerps(100000, 5000, worldP.z)); // add in distance constant blur but exclude sky

	return float4(nearB, farB, 1, 1);
}

// Places each layer to be blurred into its own quadrant of the screen.
float4 halfRes(VSOUT IN) : COLOR0
{
	float2 p = IN.UVCoord;

	float4 q = quadMask(p);
	p = remapQuad(p, q);

	float4 blurTex = tex2D(TESR_RenderedBuffer, p);
	float4 srcTex = tex2D(TESR_SourceBuffer, p);
	srcTex = lerp(srcTex, float4(blurTex.rg, 1, 1), (q.z || q.w)); // only show the blur for both bottom quadrants

	return srcTex;
}

// Gathers a max (bokeh) and an average (blur) over a poisson disc, then mixes the two by luma so
// only bright spots turn into bokeh, as with HDR values. Passing useBokeh = 0 gives a plain blur.
float4 BokehBlur(VSOUT IN, uniform float useBokeh) : COLOR0
{
	float2 p = IN.UVCoord;
	float4 q = quadMask(p);

	if (q.w)
		return tex2D(TESR_RenderedBuffer, p); // perform no blur in this quadrant

	p = remapQuad(p, q);

	float nearAmt = tex2D(TESR_RenderedBuffer, p * 0.5 + float2(0.0, 0.5)).r;
	float farAmt = tex2D(TESR_RenderedBuffer, p * 0.5 + float2(0.5, 0.5)).g;

	float centerW = dot(float3(1, farAmt, 1), q.xyz);

	float4 acc = tex2D(TESR_RenderedBuffer, IN.UVCoord);
	float4 bokeh = acc; // will accumulate as bokeh-like dilation
	float wSum = 1;
	float rad = lerp(bokehRadius, baseRadius, useBokeh); // select the bokeh radius or post blur radius
	for (int k = 1; k < 12; k++)
	{
		float2 off = TESR_ReciprocalResolution.xy * poissonTaps[k] * rad * centerW; //scale taps based on coc of pixel
		float2 sp = IN.UVCoord + off;

		// sample blur in the relative space of the bottom quadrant
		float2 bsp = p + off * 2;
		farAmt = tex2D(TESR_RenderedBuffer, bsp * 0.5 + float2(0.5, 0.5)).g;
		float maskB = dot(float3(1, farAmt, 1), q.xyz); //chose a blur mask or not based on quadrant

		// check that the sampling position is in the same quadrant to prevent edge bleeding
		float4 sq = quadMask(sp);
		float valid = dot(q, sq) * maskB; // check if sample is in the right quadrant

		float4 sc = valid?tex2D(TESR_RenderedBuffer, sp):float4(0, 0, 0, 0);
		sc *= valid;
		acc += sc; // only accumulate valid pixels (within blur mask and quadrant)

		bokeh = (luma(sc.rgb)>luma(bokeh.rgb))? sc: bokeh; // bokeh dilation blur
		wSum += valid;
	}

	acc /= wSum;
	return lerp(acc, bokeh, (luma(bokeh.rgb) > bokehCut) * useBokeh); // select a bokeh effect or normal blur based on luma
}

float4 Combine(VSOUT IN) : COLOR0
{
	float2 p = IN.UVCoord;

	// Sample each quadrant, pulling the coordinates in by half a pixel so the bottom quadrants do not leak into the top ones
	float2 hp = TESR_ReciprocalResolution.xy * 1.5;
	float2 nearImgUV = clamp(p * 0.5, float2(0, 0), float2(0.5, 0.5 - hp.y));
	float2 nearBlrUV = clamp(p * 0.5 + float2(0, 0.5), float2(0, 0.5 + hp.y), float2(0.5 + hp.x, 1));
	float2 farImgUV = clamp(p * 0.5 + float2(0.5, 0), float2(0.5 + hp.x, 0), float2(1, 0.5 - hp.y));
	float2 farBlrUV = clamp( p * 0.5 + float2(0.5, 0.5), float2(0.5 +hp.x, 0.5 + hp.y), float2(1, 1));

	float4 nearImgC = tex2D(TESR_RenderedBuffer, nearImgUV);
	float4 farImgC = tex2D(TESR_RenderedBuffer, farImgUV);
	float nearAmt = tex2D(TESR_RenderedBuffer, nearBlrUV).r; // near field is blurred
	float farAmt = tex2D(TESR_RenderedBuffer, farBlrUV).g;

	// blend the layers by their blur amounts
	float4 baseC = tex2D(TESR_SourceBuffer, p);
	float4 col = lerp(baseC, farImgC, farAmt);
	col = lerp(col, nearImgC, nearAmt);

	#if showDepth
		col *= float4(nearAmt, farAmt, 0, 1);
	#endif

	return float4(col.rgb, 1);
}

// The common gameplay configuration keeps autofocus disabled but uses distant blur to hide LOD.
// Running the bokeh packing and five blur/combine passes for that case wastes most of the cost.
float4 DistantOnly(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float depth = readDepth(uv - TESR_DepthOfFieldJitter.xy);	// UNOFFICIAL 2026-11-01 JitterFix
	float3 cameraVector = toWorld(uv) * depth;
	float worldHeight = TESR_CameraPosition.z + cameraVector.z;
	float distRamp = invlerps(blurFrom, blurTo, depth) * invlerps(100000, 5000, worldHeight);
	float blur = saturate(distRamp * min(distantStrength, 1.0f));
	// Reads the current image (TESR_RenderedBuffer), not TESR_SourceBuffer: this pass never writes to
	// the texture it reads, and it lets the caller skip the full-resolution SourceBuffer copy.
	float4 center = tex2D(TESR_RenderedBuffer, uv);
	// Pixels nearer than the blur start (most of the screen) are returned unchanged. The taps use an
	// explicit LOD because gradient sampling is illegal inside a dynamic branch; the buffer has one
	// mip level and the taps are 1-2 texels apart, so LOD 0 is what tex2D selected anyway.
	[branch] if (blur <= 0.0)
		return float4(center.rgb, 1);
	// Only neighbours at about the same distance are mixed in (full weight within ~7% of this pixel's depth, none beyond
	// ~22%): blurring the sky with a thin pole, branch or wire in front of it drew a dark halo round them that flickered
	// as they moved. Where the four are at the same distance this is the plain average it always was.
	float2 radius = TESR_ReciprocalResolution.xy * baseRadius * max(distantStrength, 1.0f) * (1.0 + blur);
	const float2 corners[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
	float4 blurred = 0;
	float weights = 0;
	[unroll] for (int k = 0; k < 4; k++) {
		float2 tapUV = uv + radius * corners[k];
		float weight = saturate(1.5 - abs(readDepthLod(tapUV - TESR_DepthOfFieldJitter.xy) - depth) / (0.15 * depth));
		blurred += tex2Dlod(TESR_RenderedBuffer, float4(tapUV, 0, 0)) * weight;
		weights += weight;
	}
	blurred = weights > 0.0001 ? blurred / weights : center;
	return float4(lerp(center.rgb, blurred.rgb, blur), 1);
}


technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DoF();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 halfRes();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BokehBlur(float(1.0f));
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BokehBlur(float(1.0f));
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BokehBlur(float(0.0f));
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Combine();
	}
}

technique DistantBlurOnly
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DistantOnly();
	}
}
