// UNOFFICIAL 2026-11-03: dust motes -- sparse tiny specks of dust in the air, seen only where light falls on them (near lamps, in
// the sun's shafts), glinting towards the light. Procedural, so it works in every cell (docs\EFFECTS.md "DustMotes").
//
// The specks live in a 3D grid of cells (CellSize game units, 40 = 57 cm) that drifts slowly with the air. A cell holds one speck
// or none (a hash of the cell's number against Density); the speck sits at a hashed spot inside the cell, kept away from its faces,
// and wobbles round it. The grid's pattern repeats every 64 cells (the hash is of the cell number modulo 64), far beyond the few
// metres drawn, and the C++ side hands over the camera's place in it already wrapped, so every position here is small.
//   0 Specks  (half resolution, into TESR_DustMotesBuffer): each pixel's view, from NearStart out to the first surface or MaxDistance,
//             is cut to the stretch that some lamp's reach covers (all of it when the sun counts); the cells that stretch crosses are
//             visited one by one (3D DDA). Each speck's light on the pixel is a soft disc round the view: never narrower than about
//             a pixel (sub-pixel specks are drawn that wide but dimmer, so they keep their light and do not flicker as they move),
//             never wider than about two. The strongest speck is lit: every lamp's light with a smooth falloff to nothing at its
//             radius, the sun's where the volumetric light's froxel grid says the air is lit, each through a forward-scattering
//             phase (bright between you and the light), with a slow per-speck twinkle. The result is written as what it adds to the
//             gamma-encoded HDR frame at this spot (the light added in linear light), so the next pass needs no frame read.
//   1 Composite (full resolution, additive blending into the frame chain's current image): the buffer read bilinearly, pixels
//             with no speck discarded, so only the specks' pixels are written. Alpha is left as it is.
//
// Known gaps: the lamps' shadows are not read (a speck in a lamp's reach is lit even behind a wall: the reach is short and the
// depth test hides specks behind surfaces, so this shows only as a few specks in a lamp's shaded corner), and indoors the sun does
// not light the specks (the froxel grid is an exterior pass).

float4 TESR_ReciprocalResolution;
float4 TESR_SunColor;
float4 TESR_SunDirection;                  // xyz: towards the sun
float4 TESR_DustMotesData;                 // x: brightness (0 = off), y: speck radius (units), z: max distance (units), w: density (0..1)
float4 TESR_DustMotesLook;                 // x: glint g, y: cell size (units), z: wobble clock (cycles, wraps at 1), w: lamps in use
float4 TESR_DustMotesOrigin;               // xyz: the camera in the drifting cells' frame (wrapped), w: radians per buffer pixel
float4 TESR_DustMotesSun;                  // x: sun share (0 = no sun this frame), y: DebugView (0 off, 1 the buffer raw, 2 white specks)
float4 TESR_DustMotesLampPosition[8];      // xyz: camera-relative, w: radius (0 = empty)
float4 TESR_DustMotesLampColor[8];         // rgb: linear colour times dimmer
// The volumetric light's froxel grid (VolumetricLight.fx.hlsl, GridLight / GridRow / GridCarry): r = the running sum, front to back,
// of each slice's lit air. Read only while TESR_DustMotesSun.x > 0 (the C++ side sets it only on frames the grid was built).
float4 TESR_VolumetricLightData;           // z: air density per game unit, w: the grid's depth (MarchDistance)
float4 TESR_VolumetricLightGridLayout;     // xy: grid columns and rows, zw: one atlas texel in UV
float4 TESR_VolumetricLightGridData;       // y: scale of the lit share in the grid

// Declared in register order: the effect binds TESR_ samplers by declaration order (a mismatch silently swaps them).
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_VolumetricLightGrid : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DustMotesBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
texture NoTexture;   // never set: unbinds a technique's own render target from its sampler

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"

static const float Brightness = TESR_DustMotesData.x;
static const float SpeckRadius = TESR_DustMotesData.y;
static const float MaxDistance = TESR_DustMotesData.z;
static const float Density = TESR_DustMotesData.w;
static const float Glint = TESR_DustMotesLook.x;
static const float CellSize = TESR_DustMotesLook.y;
static const float Clock = TESR_DustMotesLook.z;
static const float PixelAngle = TESR_DustMotesOrigin.w;
static const float SunShare = TESR_DustMotesSun.x;
static const float DebugView = TESR_DustMotesSun.y;

// Specks nearer the eye than this are not drawn: they would be large, fast-moving blobs (and the first-person weapon sits there).
static const float NearStart = 24.0;
// The most cells one pixel's view visits (MaxDistance 1000 at 40-unit cells crosses at most about 44).
#define MAX_CELLS 48
// A speck's spot inside its cell stays within [Margin, 1 - Margin] of it (base spot 0.2..0.8, wobble 0.13 each way), so the disc
// round it (at most about a unit wide) never reaches into a cell the view does not visit.
static const float WobbleReach = 0.13;

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

// Four values in [0, 1) from a cell number (whole numbers 0..63 on each axis). Dave Hoskins' "hash without sine" (MIT): no sin, so
// the same on every GPU.
float4 Hash43(float3 p) {
	float4 p4 = frac(p.xyzx * float4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return frac((p4.xxyz + p4.yzzw) * p4.zywx);
}

// A smooth wave of period 1 between -1 and 1, close to a sine (a triangle wave rounded by a cubic): a few instructions where
// sin is a long macro, and it runs for every speck the walk meets.
float3 Wave(float3 x) {
	float3 t = abs(frac(x) * 2.0 - 1.0) * 2.0 - 1.0;
	return t * (1.5 - 0.5 * t * t);
}

// Henyey-Greenstein phase times 4 pi (1 = scattering evenly), capped so a speck right in front of a lamp cannot blow out.
float Phase(float cosTheta, float g) {
	return min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-4), 1.5), 8.0);
}

// ---- the sun's lit share of the air from the volumetric light's froxel grid (only called while SunShare > 0) ----
static const float2 GridSize = TESR_VolumetricLightGridLayout.xy;
static const float2 GridAtlasTexel = TESR_VolumetricLightGridLayout.zw;
float GridDepth(float s) { s /= 64.0; return TESR_VolumetricLightData.w * s * s; }
float2 GridUV(float z, float2 local) {
	float2 tile = float2(fmod(z, 8.0), floor(z / 8.0));
	return (tile * GridSize + clamp(local, 0.5, GridSize - 0.5)) * GridAtlasTexel;
}
// The share of the air the sun reaches at a planar depth along the view through uv: the grid's running sum over three slices round
// that depth (the slice and one either side, for a steadier value), over what those slices would hold if all their air were lit.
float SunLitShare(float2 uv, float planar) {
	float depthRange = max(TESR_VolumetricLightData.w, 1.0);
	float s = sqrt(saturate(planar / depthRange)) * 64.0;
	float z = min(floor(s), 63.0);
	float zHigh = min(z + 1.0, 63.0);
	float zLow = z - 2.0;
	float2 local = uv * GridSize;
	float high = tex2Dlod(TESR_VolumetricLightGrid, float4(GridUV(zHigh, local), 0, 0)).r;
	float low = zLow >= 0.0 ? tex2Dlod(TESR_VolumetricLightGrid, float4(GridUV(zLow, local), 0, 0)).r : 0.0;
	float sigma = TESR_VolumetricLightData.z * length(toWorld(uv));
	float nearSide = GridDepth(max(zLow + 1.0, 0.0)), farSide = GridDepth(zHigh + 1.0);
	float all = (exp(-sigma * nearSide) - exp(-sigma * farSide)) * TESR_VolumetricLightGridData.y;
	return all > 1.0e-7 ? saturate((high - low) / all) : 0.0;
}

// ---- 0: Specks, half resolution ----
float4 SpecksPS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float planarDepth = readDepthLod(uv);
	float3 ray = toWorld(uv);                       // per unit of planar depth (camera-relative world)
	float rayLength = length(ray);
	float3 dir = ray / rayLength;
	float tEnd = min(planarDepth * rayLength, MaxDistance);   // the sky reads farZ: MaxDistance
	if (tEnd <= NearStart) return float4(0.0, 0.0, 0.0, 0.0);

	// The stretch of the view some light reaches: every lamp's sphere, or all of it when the sun counts (or DebugView 2, which
	// shows every speck white whether lit or not).
	float tA = tEnd, tB = NearStart;
	if (SunShare > 0.0 || DebugView > 1.5) { tA = NearStart; tB = tEnd; }
	float lampsCrossed = 0.0;   // DebugView 1
	[unroll] for (int i = 0; i < 8; i++) {
		float4 lamp = TESR_DustMotesLampPosition[i];
		float b = dot(lamp.xyz, dir);
		float disc = b * b - (dot(lamp.xyz, lamp.xyz) - lamp.w * lamp.w);
		if (disc > 0.0 && lamp.w > 0.0) {
			float root = sqrt(disc);
			tA = min(tA, max(b - root, NearStart));
			tB = max(tB, min(b + root, tEnd));
			lampsCrossed += 1.0;
		}
	}
	// DebugView 1: the buffer drawn raw over the picture. Blue: how many lamp reaches the view crosses (a faint blue everywhere the
	// view is long enough to hold specks: the pass reached the screen); green: the share of MaxDistance walked; red: the speck below.
	float4 debugBase = float4(0.0, 0.0, 0.03 + 0.12 * lampsCrossed, 0.0);
	if (tB <= tA) return DebugView > 0.5 && DebugView < 1.5 ? debugBase : float4(0.0, 0.0, 0.0, 0.0);
	debugBase.g = 0.5 * (tB - tA) / MaxDistance;

	// Walk the cells from tA to tB (Amanatides-Woo), keeping the speck that covers this pixel most.
	float3 start = TESR_DustMotesOrigin.xyz + dir * tA;        // in the drifting cells' frame
	float3 cell = floor(start / CellSize);
	float3 stepSign = dir >= 0.0 ? 1.0 : -1.0;
	float3 inverse = 1.0 / max(abs(dir), 1.0e-5);
	float3 tNext = tA + ((cell + (stepSign > 0.0 ? 1.0 : 0.0)) * CellSize - start) * stepSign * inverse;
	float3 tStep = CellSize * inverse;

	float best = 0.0;
	float3 bestPosition = 0.0;
	float4 bestRandom = 0.0;
	[loop] for (int k = 0; k < MAX_CELLS; k++) {
		float3 wrapped = cell - 64.0 * floor(cell / 64.0);
		float4 random = Hash43(wrapped);
		[branch] if (random.w < Density) {
			// The speck's spot: a hashed base, plus a wobble at 1, 2 or 3 times the clock (whole multiples: the clock wraps at 1).
			float3 wobble = Wave(Clock * (1.0 + floor(random.yzx * 3.0)) + frac(random.zxy * 7.31));
			float3 local = 0.2 + 0.6 * random.xyz + WobbleReach * wobble;
			float3 position = (cell + local) * CellSize - TESR_DustMotesOrigin.xyz;   // camera-relative
			float along = dot(position, dir);
			float3 across = position - dir * along;
			// Its size on screen in buffer pixels, held between 0.8 and 1.25: smaller ones are dimmer by the ratio of the sizes (not
			// of the areas: that left every speck past a metre nearly black, 2026-11-03 bench), larger ones are capped (a speck is
			// never a blob). The disc is a Gaussian of that width round the speck.
			float pixelSize = max(along, 1.0) * PixelAngle;
			float apparent = SpeckRadius / pixelSize;
			float width = clamp(apparent, 0.8, 1.25) * pixelSize;
			float energy = min(apparent / 0.8, 1.0);
			float cover = energy * exp(-2.0 * dot(across, across) / (width * width));
			cover *= (along > NearStart && along < tEnd) ? 1.0 : 0.0;   // in front of the surface, past the near limit
			if (cover > best) { best = cover; bestPosition = position; bestRandom = random; }
		}
		float tCell = min(tNext.x, min(tNext.y, tNext.z));
		if (tCell > tB) break;
		float3 move = tNext <= tCell ? 1.0 : 0.0;
		cell += move * stepSign;
		tNext += move * tStep;
	}
	if (DebugView > 0.5 && DebugView < 1.5) return float4(saturate(best * 4.0), debugBase.gb, 0.0);
	if (best < 1.0e-3) return float4(0.0, 0.0, 0.0, 0.0);

	// Light the speck. Phase: the light's way from its source through the speck, against the way to the eye (-dir).
	float3 light = 0.0;
	[unroll] for (int j = 0; j < 8; j++) {
		float4 lamp = TESR_DustMotesLampPosition[j];
		float3 fromLamp = bestPosition - lamp.xyz;
		float distance = length(fromLamp);
		float x2 = saturate(distance * distance / max(lamp.w * lamp.w, 1.0));
		float falloff = 1.0 - x2;   // 0 at the radius, as the game's own lamp falloff (the steeper windowed one left specks unlit)
		float cosTheta = dot(fromLamp / max(distance, 1.0e-3), -dir);
		light += lamp.w > 0.0 ? TESR_DustMotesLampColor[j].rgb * (falloff * Phase(cosTheta, Glint)) : 0.0;
	}
	float along = dot(bestPosition, dir);
	[branch] if (SunShare > 0.0) {
		float lit = SunLitShare(uv, along / rayLength);
		light += linearize(max(TESR_SunColor.rgb, 0.0)) * (SunShare * lit * Phase(dot(TESR_SunDirection.xyz, dir), Glint));
	}

	// A slow twinkle, as a flake turns: mostly dim, a short flash now and then (2 to 5 times the clock, its own phase).
	float twinkle = 0.55 + 1.2 * pow(0.5 + 0.5 * Wave(Clock * (2.0 + floor(frac(bestRandom.w * 91.3) * 4.0)) + frac(bestRandom.x * 53.1)).x, 6.0);
	// Fade in past the near limit and out towards MaxDistance, so the edge of the drawn space never shows.
	float fade = smoothstep(NearStart, NearStart * 2.0, along) * (1.0 - smoothstep(MaxDistance * 0.7, MaxDistance, along));
	float3 added = light * (Brightness * best * twinkle * fade);
	if (DebugView > 1.5) added = Brightness * best * fade;   // DebugView 2: every speck white, lit or not

	// What this adds to the gamma-encoded HDR frame here (light added in linear light).
	float3 color = max(tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0)).rgb, 0.0);
	float3 difference = delinearize(linearize(color) + added) - color;
	return float4(max(difference, 0.0), 0.0);
}

// ---- 1: Composite, full resolution, additive into the current image ----
float4 CompositePS(VSOUT IN) : COLOR0 {
	float3 added = tex2Dlod(TESR_DustMotesBuffer, float4(IN.UVCoord, 0, 0)).rgb;
	clip(max(added.r, max(added.g, added.b)) - 1.0 / 4096.0);   // no speck here: nothing written
	return float4(added, 0.0);
}

// ---- 2: DebugView 1, the buffer drawn over the picture as it is (no blending, alpha kept) ----
float4 DebugPS(VSOUT IN) : COLOR0 {
	return float4(tex2Dlod(TESR_DustMotesBuffer, float4(IN.UVCoord, 0, 0)).rgb, 0.0);
}

technique Specks {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SpecksPS();
		Texture[3] = <NoTexture>;   // writes Buffer
	}
}

technique Composite {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositePS();
		Texture[0] = <NoTexture>;   // writes the current image (TESR_RenderedBuffer); reads only Buffer
		AlphaBlendEnable = true;
		SrcBlend = One;
		DestBlend = One;
		BlendOp = Add;
		SeparateAlphaBlendEnable = false;
		ColorWriteEnable = 7;       // red, green, blue: the frame's alpha (the material capture's) stays
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
	}
}

technique Debug {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 DebugPS();
		Texture[0] = <NoTexture>;   // writes the current image
		AlphaBlendEnable = false;
		ColorWriteEnable = 7;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
	}
}
