// UNOFFICIAL: volumetric sun light -- shafts of sunlight through the air, where trees, buildings and rocks shade it.
// Written for this build; the idea comes from Physically-Based-Reloaded's Volumetric Light (mcstfuerson's shader, ported
// by Conor), but no code is shared: theirs marches its own shadow lookup at half resolution with a history pass; this
// one reuses Includes/SunCascades.hlsl (the lookup Specular already uses).
//
// A march with few steps bands (each step is a hard edge in the light). The march breaks the bands up into noise by
// starting each pixel's steps at a different point, and the passes after it take the noise out again:
//   1 March    (half resolution): from the eye towards each block's surface, at most MaxDistance, Steps samples of the
//              sun's shadow, each pixel's start shifted by interleaved gradient noise (a new pattern every frame when
//              Temporal is on). r: the share of the view the lit air scatters towards the eye (single scattering,
//              1 - exp falloff), g: the same if all the air were lit, b: depth / farZ (for the depth-aware passes).
//              Written to TESR_VolumetricLightTemp, or to TESR_VolumetricLightMarch when Temporal is on.
//   3 Temporal (half resolution, only when Temporal is on): March -> Temp. Blends in last frame's result at the same
//              spot (TESR_VolumetricLightHistory, reprojected by depth), held to the range of this frame's neighbours so
//              a moving shadow does not leave a trail. The effect then copies Temp into History for the next frame.
//   2 Blur X, 4 Blur Y (half resolution): Temp -> March -> TESR_VolumetricLightBuffer, a depth-aware Gaussian each way
//              (BlurRadius texels), which keeps the shafts sharp at the edges of buildings while smoothing the noise.
//   0 Composite (full resolution, in the frame chain): depth-aware upsample, then the lit air adds sunlight (shaped by
//              the forward-scattering phase, brightest looking towards the sun) and the shaded air darkens what is behind
//              it a little -- the game's fog lights all the air alike, so without that the shafts would only ever brighten.
//
// The frame here is gamma encoded HDR (VolumetricFog linearises it the same way): the light is added in linear light.

float4 TESR_ReciprocalResolution;
float4 TESR_GameMultisample;  // UNOFFICIAL 2026-11-04 x: the game's MSAA samples (Includes/MsaaSkyEdge.hlsl)
float4 TESR_SunColor;
float4 TESR_ShadowFade;              // read by Includes/Shadows.hlsl (point lights; unused here)
float4 TESR_VolumetricLightData;     // x: strength, y: shade darkening, z: air density per game unit, w: march distance
float4 TESR_VolumetricLightData2;    // x: steps, y: phase asymmetry g, z: fade (shadows on, rain), w: noise frame offset
float4 TESR_VolumetricLightData3;    // x: history weight (0 = none this frame), y: blur radius in half-resolution texels
float4 TESR_VolumetricLightLayout;   // xy: one half-resolution texel in screen UV, zw: unused
float4x4 TESR_VolumetricLightReproject; // camera-relative world position -> last frame's clip space

// Declared in register order: the effect binds TESR_ samplers by declaration order (a mismatch silently swaps them).
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_VolumetricLightMarch : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_VolumetricLightBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_VolumetricLightTemp : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_VolumetricLightHistory : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-14 froxel grid (Froxels on): linear filtering for the composite's reads between grid columns; the passes
// that build it read texel centres only, where the filter returns the texel itself.
sampler2D TESR_VolumetricLightGrid : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_VolumetricLightGridTemp : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_VolumetricLightGridLayout; // xy: grid columns and rows (one per 16 x 16 screen pixels), zw: one atlas texel in UV
float4 TESR_VolumetricLightGridData;   // x: fog volume on ([Shaders.VolumetricFog.Main] FogVolume), y: scale of the lit share (r)
float4 TESR_VolumetricLightRt;         // UNOFFICIAL 2026-11-07 (ptaaa-fog) RT fog visibility: x 0 the cascades, 1 traced on this grid, 2 traced on
                                       // another MaxDistance's grid; y the RT range, z its fade length, w the traced MaxDistance (see SunVisibility)
// The fog volume's density and patches, as VolumetricFog.fx.hlsl ModernFogTerms works them out (same constants).
float4 TESR_FogData;                   // w: fog power
float4 TESR_SunAmount;                 // x: day
float4 TESR_GameTime;                  // x: game time (ms)
float4 TESR_VolumetricFogDensity;      // y: WeatherImpact
float4 TESR_VolumetricFogShape;        // x: HeightFalloff
float4 TESR_VolumetricFogWind;         // xy: wind direction, z: wind speed, w: noise scale
float4 TESR_VolumetricFogScatter;      // w: HeightInfluence
float4 TESR_VolumetricFogWeather;      // y: isExterior
float4 TESR_VolumetricFogNight;        // z: HeightFalloffScale
float4 TESR_VolumetricFogNightScatter; // y: WindSpeedScale
// The fog's 3D noise (Textures\Effects\NoiseVolume.dds), bound by hand on stage 9 while the grid is lit (VolumetricLight.cpp).
sampler3D NVR_FogNoise : register(s9) = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
texture NoTexture;   // never set: unbinds a technique's own render target from its sampler

static const float Strength = TESR_VolumetricLightData.x;
static const float Darken = TESR_VolumetricLightData.y;
static const float Density = TESR_VolumetricLightData.z;
static const float MarchDistance = TESR_VolumetricLightData.w;
static const float Steps = TESR_VolumetricLightData2.x;
static const float Anisotropy = TESR_VolumetricLightData2.y;
static const float Fade = TESR_VolumetricLightData2.z;
static const float NoiseFrame = TESR_VolumetricLightData2.w;
static const float HistoryWeight = TESR_VolumetricLightData3.x;
static const float BlurRadius = TESR_VolumetricLightData3.y;
static const float2 LowTexel = TESR_VolumetricLightLayout.xy;

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#define MSAA_SKY_EDGE_REG s9   // after TESR_VolumetricLightGridTemp (s8)
#include "Includes/MsaaSkyEdge.hlsl"	// UNOFFICIAL 2026-11-04 the game's MSAA: sky edges (TESR_SkyCoverage, the last TESR_ sampler)
// UNOFFICIAL 2026-11-07 (ptaaa-fog) the traced sun visibility per grid cell (core/RtFogVisibility.h), the grid's layout. After TESR_SkyCoverage
// (s9): the effect binds TESR_ samplers by their place. Linear: the march's reads between columns; the grid reads texel centres.
sampler2D TESR_RtFogVisibility : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
#include "Includes/Shadows.hlsl"
#include "Includes/SunCascades.hlsl"
#include "Includes/FrameNoise.hlsl"	// UNOFFICIAL 2026-11-01 the shared noise code (the same pattern as before)

// Interleaved gradient noise (Jimenez 2014) in [0, 1): neighbouring pixels get well spread values with no visible
// pattern, so the march's steps land at different depths side by side and the bands turn into fine noise. Shifted by
// a new amount every frame when Temporal is on, so the history averages different start points at each pixel.
// (NoiseFrame: core/TemporalBackbone.h with SharedNoise: the shared index with Temporal, else moving only under TAA / FSR / DLSS.)
float GradientNoise(float2 pixel) {
	return FrameNoiseIGN(pixel, NoiseFrame);
}

// The sun's visibility at one point of the march: one atlas read from the first cascade that holds the point (no blend
// between cascades, no normal offset: the march averages many points and is blurred). Beyond the LOD cascade (the
// shadow distance) the air counts as lit. EVSM4 reads only its positive warp. The LOD cascade matters: at the default
// 6000-unit shadow distance the first three end near 1750 units, so without it every building farther than that let
// the light straight through. GetSunCascadeLight, which blends cascades with a full three-format
// lookup each, made the march loop take minutes to compile.
float MarchShadow(float3 position) {
	float4x4 lightTransform;
	float2 quadrant;
	float bleed;
	[branch] if (length(position - TESR_ShadowNearCenter.xyz) < TESR_ShadowNearCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformNear; quadrant = float2(0.0, 0.0); bleed = 0.1;
	}
	else if (length(position - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformMiddle; quadrant = float2(0.5, 0.0); bleed = 0.2;
	}
	else if (length(position - TESR_ShadowFarCenter.xyz) < TESR_ShadowFarCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformFar; quadrant = float2(0.0, 0.5); bleed = 0.6;
	}
	else if (length(position - TESR_ShadowLodCenter.xyz) < TESR_ShadowLodCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformLod; quadrant = float2(0.5, 0.5); bleed = 0.8;
	}
	else return 1.0;
	float4 lightSpace = SunCascadeTexCoord(mul(float4(position, 1.0), lightTransform));
	[branch] if (TESR_ShadowFormatData.x == 3.0) return ShadowPCF2x2(TESR_ShadowAtlas, lightSpace.xy * 0.5 + quadrant, lightSpace.z - 0.0002, TESR_ShadowBlur.x);	// UNOFFICIAL PCF
	float4 moments = tex2Dlod(TESR_ShadowAtlas, float4(lightSpace.xy * 0.5 + quadrant, 0.0, 0.0));
	float bias = TESR_ShadowFormatData.x == 0.0 ? 0.00001 : 0.01;
	if (TESR_ShadowFormatData.x == 0.0) return GetLightAmountValueVSM(moments.xy, lightSpace.z, bias, bleed);
	if (TESR_ShadowFormatData.x == 1.0) return GetLightAmountValueEVSM2(moments.xy, lightSpace.z, bias, bleed, TESR_ShadowFormatData.y);
	float2 exponents = GetEVSMExponents(40.0, 5.0, TESR_ShadowFormatData.y);
	float warped = WarpDepth(lightSpace.z, exponents).x;
	float depthScale = bias * exponents.x * warped;
	return ChebyshevUpperBound(moments.xz, warped, depthScale * depthScale, bleed);
}

float SunVisibility(float3 position);	// UNOFFICIAL 2026-11-07 (ptaaa-fog): MarchShadow, or the traced visibility (below the grid's code)

// ---- 1: March, half resolution ----
float4 MarchPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;                                  // this block's centre on screen
	float depth = readDepthLod(uv - 0.5 * (LowTexel - TESR_ReciprocalResolution.xy));   // a pixel centre in the block, not an edge (the pixel itself at full size)
	float isSky = depth >= farZ * 0.996 ? 1.0 : 0.0;
	float planar = isSky > 0.0 ? MarchDistance : min(depth, MarchDistance);

	float3 ray = toWorld(uv);                       // the eye vector per unit of planar depth (camera-relative world)
	float sigma = Density * length(ray);            // extinction per unit of planar depth
	float dt = planar / Steps;
	float offset = GradientNoise(vpos);

	float lit = 0.0, all = 0.0;
	[loop] for (float i = 0.0; i < Steps; i += 1.0) {
		float t = (i + offset) * dt;
		float transmittance = exp(-sigma * t);
		lit += SunVisibility(ray * t) * transmittance;
		all += transmittance;
	}
	float scattered = sigma * dt;                   // the share each sample scatters towards the eye
	return float4(lit * scattered, all * scattered, depth / farZ, 1.0);
}

// ---- 3: Temporal, half resolution ----
// Last frame's result where this block's surface was then: the four history texels round it, bilinearly, each only if
// its depth matches. Returns how much of it matched (0: off screen, or something else stood there).
float HistoryAt(float2 position, float expected01, out float2 history) {
	position = position / LowTexel - 0.5;
	float2 base = floor(position);
	float2 f = position - base;
	float4 t00 = tex2Dlod(TESR_VolumetricLightHistory, float4((base + 0.5) * LowTexel, 0, 0));
	float4 t10 = tex2Dlod(TESR_VolumetricLightHistory, float4((base + float2(1.5, 0.5)) * LowTexel, 0, 0));
	float4 t01 = tex2Dlod(TESR_VolumetricLightHistory, float4((base + float2(0.5, 1.5)) * LowTexel, 0, 0));
	float4 t11 = tex2Dlod(TESR_VolumetricLightHistory, float4((base + 1.5) * LowTexel, 0, 0));
	float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	float tolerance = max(expected01 * 0.05, 0.0002);
	w *= abs(float4(t00.b, t10.b, t01.b, t11.b) - expected01) < tolerance;
	float sum = dot(w, 1.0);
	history = (t00.rg * w.x + t10.rg * w.y + t01.rg * w.z + t11.rg * w.w) / max(sum, 1.0e-4);
	return sum;
}

float4 TemporalPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5) * LowTexel;
	float4 current = tex2Dlod(TESR_VolumetricLightMarch, float4(uv, 0, 0));

	// The range this frame's neighbours on the same surface span: the history is held inside it, so where a shadow
	// moved (or the camera uncovered new ground) the old light cannot linger.
	float tolerance = max(current.b * 0.05, 0.00002);
	float2 low = current.rg, high = current.rg;
	[unroll] for (int y = -1; y <= 1; y++) {
		[unroll] for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0) continue;
			float4 tap = tex2Dlod(TESR_VolumetricLightMarch, float4(uv + float2(x, y) * LowTexel, 0, 0));
			if (abs(tap.b - current.b) < tolerance) { low = min(low, tap.rg); high = max(high, tap.rg); }
		}
	}

	float3 position = toWorld(uv) * (current.b * farZ);
	float4 clip = mul(float4(position, 1.0), TESR_VolumetricLightReproject);
	float w = max(clip.w, 0.001);
	// UNOFFICIAL 2026-10-27 RenderScale v2: the projection gives a screen position; the history holds it at its rect uv.
	float2 last = screenToRect(float2(clip.x / w * 0.5 + 0.5, 0.5 - clip.y / w * 0.5));
	float2 history;
	float found = HistoryAt(last, clip.w / farZ, history);
	found *= all(last > 0.0) && all(last < TESR_RenderScale.xy);

	float2 margin = 0.1 * (high - low) + 1.0e-4;
	history = clamp(history, low - margin, high + margin);
	float weight = HistoryWeight * saturate(found);
	return float4(lerp(current.rg, history, weight), current.b, 1.0);
}

// ---- 2 / 4: Blur X / Blur Y, half resolution ----
// A Gaussian over BlurRadius texels each way (sigma half the radius), each tap weighed down by how far its depth is
// from the centre's, so the light does not smear across the edge of a building onto the sky behind it.
float4 BlurAxis(sampler2D source, float2 vpos, float2 axis) {
	float4 centre = tex2Dlod(source, float4((vpos + 0.5) * LowTexel, 0, 0));
	float tolerance = max(centre.b * 0.05, 0.00002);
	float sigma = max(BlurRadius * 0.5, 0.5);
	float falloff = -0.5 / (sigma * sigma);
	float2 sum = centre.rg;
	float weights = 1.0;
	// UNOFFICIAL 2026-10-13: only the taps inside BlurRadius are read (those beyond weighed exactly 0 but were still
	// fetched: with the default radius 4, half of the 16 reads of each blur pass did nothing).
	[loop] for (int i = 1; i <= 8; i++) {
		if (i > BlurRadius) break;
		float gauss = exp(falloff * i * i);
		// UNOFFICIAL 2026-10-27 RenderScale v2: taps kept inside the drawn part (no-op at native)
		float4 a = tex2Dlod(source, float4(clampToRectIfOn((vpos + 0.5 + axis * i) * LowTexel, LowTexel), 0, 0));
		float4 b = tex2Dlod(source, float4(clampToRectIfOn((vpos + 0.5 - axis * i) * LowTexel, LowTexel), 0, 0));
		float wa = gauss / (1.0 + abs(a.b - centre.b) / tolerance);
		float wb = gauss / (1.0 + abs(b.b - centre.b) / tolerance);
		sum += a.rg * wa + b.rg * wb;
		weights += wa + wb;
	}
	return float4(sum / weights, centre.b, 1.0);
}

float4 BlurXPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 { return BlurAxis(TESR_VolumetricLightTemp, vpos, float2(1.0, 0.0)); }
float4 BlurYPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 { return BlurAxis(TESR_VolumetricLightMarch, vpos, float2(0.0, 1.0)); }

// The lit and shaded air over the frame: the sunlight added, the shaded air's darkening (air: r lit share, g all air).
float4 CompositeAir(float2 uv, float4 color, float2 air) {
	// Henyey-Greenstein phase, times 4 pi (1 for air that scatters evenly), capped so looking straight at the sun
	// cannot blow out.
	float cosTheta = dot(normalize(toWorld(uv)), TESR_SmoothedSunDir.xyz);
	float g = Anisotropy;
	float phase = min((1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-4), 1.5), 8.0);

	float3 lin = linearize(max(color.rgb, 0.0));
	float shadedAir = saturate(air.g - air.r);
	lin = lin * (1.0 - Darken * Fade * shadedAir) + linearize(TESR_SunColor.rgb) * (Strength * Fade * phase * air.r);
	return float4(delinearize(max(lin, 0.0)), color.a);
}

// ---- 0: Composite, full resolution ----
float2 MarchAirAt(float2 uv, float depth);
float2 MsaaAirMix(float2 uv, float depth, float2 air, uniform bool grid);

float4 CompositePS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0));
	float depth = readDepthLod(uv);
	[branch] if (MsaaEdgeDebugOn()) return MsaaEdgeDebug(uv, depth, color.rgb);	// UNOFFICIAL 2026-11-05 [Main.Main.Quality] MsaaEdgeDebugView
	float2 air = MarchAirAt(uv, depth);
	[branch] if (MsaaEdgeFixOn()) air = MsaaAirMix(uv, depth, air, false);	// UNOFFICIAL 2026-11-05 MsaaEdgeFix: as CompositeGridPS
	// UNOFFICIAL 2026-11-04 the game's MSAA: as CompositeGridPS (Includes/MsaaSkyEdge.hlsl).
	else if (TESR_GameMultisample.z <= 0.5 && depth < farZ * 0.996) {
		float3 sky;
		float share = SkyShare(uv, color.rgb, sky);
		[branch] if (share > 0.0) air = lerp(air, MarchAirAt(uv, farZ), share);
	}
	return CompositeAir(uv, color, air);
}

// Depth-aware bilinear upsample from the half-resolution buffer (texel k stands for its centre), at this depth.
float2 MarchAirAt(float2 uv, float depth) {
	float2 position = uv / LowTexel - 0.5;
	float2 base = floor(position);
	float2 fraction = position - base;
	float depth01 = depth / farZ;
	float tolerance = max(depth01 * 0.04, 0.00002);
	float2 air = 0.0;
	float weights = 1.0e-5;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float4 tap = tex2Dlod(TESR_VolumetricLightBuffer, float4((base + float2(x, y) + 0.5) * LowTexel, 0, 0));
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1.0 + abs(tap.b - depth01) / tolerance;
			air += tap.rg * weight;
			weights += weight;
		}
	}
	return air / weights;
}

// ---- UNOFFICIAL 2026-10-14: froxel grid ([Shaders.VolumetricLight.Main] Froxels) ----
// Instead of marching every half-resolution pixel (Steps shadow reads each, then history and two blurs), the air in front
// of the camera is cut into a grid: one column per 16 x 16 screen pixels, 64 slices deep out to MaxDistance (spaced by the
// square of the slice number, so thin near the eye and thick far out). The sun's shadow is read twice per cell, once, and
// summed front to back; every pixel then reads the sum at its own depth. About a sixteenth of the march's shadow reads, no
// noise to hide, so no history and no blur. The 64 slices sit in one 2D texture, 8 x 8 tiles of the grid's columns x rows
// (D3D9 cannot draw into a 3D texture), slice z in tile (z mod 8, z / 8).
//   5 GridLight: per cell, r: the share of the view the cell's lit air scatters towards the eye (the air density is the
//                same everywhere, so the transmittance in front of it is exact: exp(-sigma * depth)), times GridData.y to
//                keep it in half-float range. With the fog volume on (GridData.x), also the fog's density by height (g), that
//                times its 3D noise (b) and times the sun's shadow (a), each per unit of the grid's depth; the fog then takes
//                its patches (b / g) and its sun shading (a / g) along each view from them.         -> Grid
//   6 GridRow:   sums along each row of 8 tiles (slices 8r .. 8r + 7).                               Grid -> GridTemp
//   7 GridCarry: adds the totals of the rows before.                                                 GridTemp -> Grid
//                Slice z then holds the lit share from the eye to the far side of slice z.
//   8 CompositeGrid: the composite above, the lit share read at the pixel's depth (two reads between the slices it lies
//                in), the share of all the air worked out exactly (1 - exp(-sigma * depth)).
static const float2 GridSize = TESR_VolumetricLightGridLayout.xy;
static const float2 GridAtlasTexel = TESR_VolumetricLightGridLayout.zw;

// Planar depth of slice coordinate s (0 .. 64, the near side of slice s).
float GridDepth(float s) { s /= 64.0; return MarchDistance * s * s; }

// The atlas UV of slice z at grid coordinate local (texel k's centre at k + 0.5), kept inside the slice's tile.
float2 GridUV(float z, float2 local) {
	float2 tile = float2(fmod(z, 8.0), floor(z / 8.0));
	return (tile * GridSize + clamp(local, 0.5, GridSize - 0.5)) * GridAtlasTexel;
}

// ---- UNOFFICIAL 2026-11-07 (ptaaa-fog): RT fog visibility ([Shaders.VolumetricLight.RT], core/RtFogVisibility.h) ----
// While the ray tracing core runs for another RT effect, the sun's visibility in each grid cell is traced against the RT scene (the casters
// the RT sun shadows and the path tracer see: far geometry past the cascades, alpha-tested cards, actors, the far LOD; Compute/Rt/
// RtFogVisibility.comp: a few rays a cell, a temporal history) and copied into TESR_RtFogVisibility in the grid's layout. Inside the RT range
// it replaces the cascade reads below (GridLight: the cell's own texel; the march: by position), the cascades fade back in over the last
// FadeLength (by distance from the eye). TESR_VolumetricLightRt.x 0 (not traced this frame): MarchShadow alone, as before.
float RtShare(float3 position) {
	return TESR_VolumetricLightRt.x > 0.5 ? saturate((TESR_VolumetricLightRt.y - length(position)) / max(TESR_VolumetricLightRt.z, 1.0)) : 0.0;
}

// The traced visibility at a camera-relative position: the grid column it projects to (bilinear between columns), between its two nearest
// slices (the cells' values stand for their centres: slice coordinate z + 0.5). The grid it was traced on reaches TESR_VolumetricLightRt.w.
float RtFogVisAt(float3 position) {
	float3 f = float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]);
	float3 r = float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]);
	float3 u = float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]);
	float planar = max(dot(position, f), 1.0e-3);
	float2 ndc = float2(dot(position, r) * TESR_ProjectionTransform[0][0], dot(position, u) * TESR_ProjectionTransform[1][1]) / planar;
	float2 local = float2(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y) * GridSize;   // the screen's, not a RenderScale rect's: the grid covers the screen
	float s = sqrt(saturate(planar / max(TESR_VolumetricLightRt.w, 1.0))) * 64.0 - 0.5;
	float z = clamp(floor(s), 0.0, 63.0);
	float a = tex2Dlod(TESR_RtFogVisibility, float4(GridUV(z, local), 0, 0)).r;
	float b = tex2Dlod(TESR_RtFogVisibility, float4(GridUV(min(z + 1.0, 63.0), local), 0, 0)).r;
	return lerp(a, b, saturate(s - z));
}

// The sun's visibility at a camera-relative position: traced inside the RT range, the cascades past it (blended over the fade)
float SunVisibility(float3 position) {
	float share = RtShare(position);
	float visibility = 1.0;
	[branch] if (share > 0.0) visibility = RtFogVisAt(position);
	[branch] if (share < 1.0) visibility = lerp(MarchShadow(position), visibility, share);
	return visibility;
}

// The fog's density by height at a camera-relative height (relative to the camera's: only ratios of it are used), as
// VolumetricFog.fx.hlsl getHeightFog's falloff with its night and weather scaling, and HeightInfluence.
float FogHeightWeight(float height, float nightFactor) {
	float fogPower = lerp(1.0, TESR_FogData.w, saturate(TESR_VolumetricFogDensity.y));
	float heightFalloff = max(0.0001, TESR_VolumetricFogShape.x) * lerp(1.0, max(0.0, TESR_VolumetricFogNight.z), nightFactor);
	float falloff = 1.5e-4 / (fogPower * heightFalloff);
	// UNOFFICIAL 2026-11-01 HeightLayer: the layer's own density by world height (full below its base, falling off above it over its
	// thickness), so the noise and sun averaged through the grid follow where the fog actually is. Shape.x < 0 = on: x = -1 / thickness,
	// y = the base in world z (VolumetricFog.cpp ApplyLayer)
	[branch] if (TESR_VolumetricFogShape.x < 0.0)
		return lerp(1.0, exp(-max(TESR_CameraPosition.z + height - TESR_VolumetricFogShape.y, 0.0) * (-TESR_VolumetricFogShape.x)), saturate(TESR_VolumetricFogScatter.w));
	return lerp(1.0, exp(clamp(-falloff * height, -20.0, 10.0)), saturate(TESR_VolumetricFogScatter.w));
}

// The fog's 3D noise at a camera-relative position, as VolumetricFog.fx.hlsl ModernFogTerms reads it (wind-blown).
float FogNoiseAt(float3 position, float nightFactor) {
	float windSpeed = TESR_VolumetricFogWind.z * lerp(1.0, max(0.0, TESR_VolumetricFogNightScatter.y), nightFactor);
	float3 windOffset = float3(TESR_VolumetricFogWind.xy * windSpeed * TESR_GameTime.x * 0.002, 0.0);
	float3 noisePos = (TESR_CameraPosition.xyz + position + windOffset) / (1500.0 * max(0.0001, TESR_VolumetricFogWind.w));
	return tex3Dlod(NVR_FogNoise, float4(noisePos / 4.0, 0.0)).g;
}

float4 GridLightPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 tile = floor(vpos / GridSize);
	float2 cell = vpos - tile * GridSize;          // the column, 0 .. GridSize - 1
	float z = tile.y * 8.0 + tile.x;
	float near = GridDepth(z), far = GridDepth(z + 1.0);
	// Two shadow reads per cell, a quarter and three quarters of the way through it, on rays through opposite quarters
	// of its 16 x 16 pixels: thin shadows (poles, wires) between the column centres are not missed outright.
	float3 positionA = toWorld((cell + 0.25) / GridSize) * lerp(near, far, 0.25);
	float3 positionB = toWorld((cell + 0.75) / GridSize) * lerp(near, far, 0.75);
	// UNOFFICIAL 2026-11-07 (ptaaa-fog) the traced visibility of the whole cell inside the RT range (its own texel when traced on this
	// grid, else by position), the two cascade reads only where the cascades fade back in
	float3 centre = toWorld((cell + 0.5) / GridSize) * GridDepth(z + 0.5);
	float rtShare = RtShare(centre);
	float traced = 1.0;
	[branch] if (rtShare > 0.0) traced = TESR_VolumetricLightRt.x < 1.5 ? tex2Dlod(TESR_RtFogVisibility, float4((vpos + 0.5) * GridAtlasTexel, 0, 0)).r : RtFogVisAt(centre);
	float shadowA = traced, shadowB = traced;
	[branch] if (rtShare < 1.0) {
		shadowA = lerp(MarchShadow(positionA), traced, rtShare);
		shadowB = lerp(MarchShadow(positionB), traced, rtShare);
	}
	float sigma = Density * length(toWorld((cell + 0.5) / GridSize));
	float4 cellValue = float4(0.5 * (shadowA + shadowB) * (exp(-sigma * near) - exp(-sigma * far)) * TESR_VolumetricLightGridData.y, 0.0, 0.0, 0.0);
	[branch] if (TESR_VolumetricLightGridData.x > 0.5) {
		float nightFactor = TESR_VolumetricFogWeather.y * (1.0 - smoothstep(0.4, 0.8, TESR_SunAmount.x));
		float share = 0.5 * (far - near) / MarchDistance;   // each point's share of the grid's depth
		float hA = FogHeightWeight(positionA.z, nightFactor), hB = FogHeightWeight(positionB.z, nightFactor);
		float nA = FogNoiseAt(positionA, nightFactor), nB = FogNoiseAt(positionB, nightFactor);
		cellValue.gba = share * float3(hA + hB, hA * nA + hB * nB, hA * shadowA + hB * shadowB);
	}
	return cellValue;
}

float4 GridRowPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 tile = floor(vpos / GridSize);
	float2 cell = vpos - tile * GridSize + 0.5;
	float4 sum = 0.0;
	[unroll] for (int c = 0; c < 8; c++)
		if (c <= tile.x) sum += tex2Dlod(TESR_VolumetricLightGrid, float4(GridUV(tile.y * 8.0 + c, cell), 0, 0));
	return sum;
}

float4 GridCarryPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 tile = floor(vpos / GridSize);
	float2 cell = vpos - tile * GridSize + 0.5;
	float4 sum = tex2Dlod(TESR_VolumetricLightGridTemp, float4(GridUV(tile.y * 8.0 + tile.x, cell), 0, 0));
	[unroll] for (int r = 0; r < 7; r++)
		if (r < tile.y) sum += tex2Dlod(TESR_VolumetricLightGridTemp, float4(GridUV(r * 8.0 + 7.0, cell), 0, 0));
	return sum;
}

// The lit share and the share of all the air from the eye to depth along this pixel's view.
float2 GridAirAt(float2 uv, float depth) {
	float planar = depth >= farZ * 0.996 ? MarchDistance : min(depth, MarchDistance);
	float s = sqrt(planar / MarchDistance) * 64.0;   // slice coordinate of the pixel's depth
	float z = min(floor(s), 63.0);
	float2 local = uv * GridSize;
	float farSide = tex2Dlod(TESR_VolumetricLightGrid, float4(GridUV(z, local), 0, 0)).r;
	float nearSide = z > 0.5 ? tex2Dlod(TESR_VolumetricLightGrid, float4(GridUV(z - 1.0, local), 0, 0)).r : 0.0;
	float sigma = Density * length(toWorld(uv));
	return float2(lerp(nearSide, farSide, saturate(s - z)) / TESR_VolumetricLightGridData.y, 1.0 - exp(-sigma * planar));
}

// UNOFFICIAL 2026-11-05 MsaaEdgeFix: the air of a pixel by its exact sky share (Includes/MsaaSkyEdge.hlsl). Surface by depth: its sky share
// takes the air of the whole view (as 2026-11-04). Sky by depth but only partly sky (a sample-0 resolve: stock DXVK, native D3D9): its
// surface share takes the air up to the nearest surface round it, instead of the whole view's (the lit air over the bars of a lattice).
// grid: GridAirAt (Froxels), else MarchAirAt.
float2 MsaaAirMix(float2 uv, float depth, float2 air, uniform bool grid) {
	float share = MsaaSkyCoverage(uv);
	[branch] if (share <= MsaaNoSky) return air;
	[branch] if (depth < farZ * 0.996) return lerp(air, grid ? GridAirAt(uv, farZ) : MarchAirAt(uv, farZ), share);
	[branch] if (share >= MsaaAllSky) return air;
	float surface = MsaaSurfaceDepth(uv, farZ);
	[branch] if (surface >= farZ * 0.996) return air;
	return lerp(grid ? GridAirAt(uv, surface) : MarchAirAt(uv, surface), air, share);
}

float4 CompositeGridPS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0));
	float depth = readDepthLod(uv);
	[branch] if (MsaaEdgeDebugOn()) return MsaaEdgeDebug(uv, depth, color.rgb);	// UNOFFICIAL 2026-11-05 [Main.Main.Quality] MsaaEdgeDebugView
	float2 air = GridAirAt(uv, depth);
	// UNOFFICIAL 2026-11-05 MsaaEdgeFix (Includes/MsaaSkyEdge.hlsl): every pixel by its exact sky share (MsaaAirMix below).
	[branch] if (MsaaEdgeFixOn()) air = MsaaAirMix(uv, depth, air, true);
	// UNOFFICIAL 2026-11-04 the game's MSAA (Includes/MsaaSkyEdge.hlsl): a pixel on an edge against the sky took the air only up to the
	// surface, its sky share too (a rim round every leaf and branch against the lit air): that share takes the air of the whole view.
	// (MsaaEdgeFix off)
	else if (TESR_GameMultisample.z <= 0.5 && depth < farZ * 0.996) {
		float3 sky;
		float share = SkyShare(uv, color.rgb, sky);
		[branch] if (share > 0.0) air = lerp(air, GridAirAt(uv, farZ), share);
	}
	return CompositeAir(uv, color, air);
}

technique Composite {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositePS();
	}
}

technique March {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MarchPS();
		Texture[3] = <NoTexture>;   // writes March (Temporal on) or Temp (off); reads neither
		Texture[5] = <NoTexture>;
	}
}

technique BlurX {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurXPS();
		Texture[3] = <NoTexture>;   // writes March
	}
}

technique Temporal {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 TemporalPS();
		Texture[5] = <NoTexture>;   // writes Temp
	}
}

technique BlurY {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurYPS();
		Texture[4] = <NoTexture>;   // writes Buffer
	}
}

technique GridLight {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GridLightPS();
		Texture[7] = <NoTexture>;   // writes Grid
	}
}

technique GridRow {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GridRowPS();
		Texture[8] = <NoTexture>;   // writes GridTemp
	}
}

technique GridCarry {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GridCarryPS();
		Texture[7] = <NoTexture>;   // writes Grid
	}
}

technique CompositeGrid {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositeGridPS();
	}
}
