// UNOFFICIAL 2026-10-18 (sandport): ported from Physically-Based-Reloaded (github.com/Balebandro/Physically-Based-Reloaded,
// Balebandro, Alejandro Martinez), GPL-3.0, the same licence as this project (their Sandstorm.fx.hlsl of 90ae7541 and
// ebcf3c03). Changed for this build: the sun's visibility in the dust is this build's cascade lookup (SunCascades.hlsl: the
// cascade picked by its centre, the PCF mode, EVSM4's positive warp; theirs read the cascades by their slabs, without PCF),
// faded with the sun shadows (TESR_ShadowFade.x: at 1 the cascade maps may not be drawn this frame, SkipWhenFaded). The
// samplers are declared in register order (effects bind TESR_ samplers by declaration order).
//
// Sandstorm, after Kappa's v5 weather update: wind-driven dust filling the air while the weather is a sandstorm
// (SandstormEffect::UpdateConstants, which also eases it in and out with the weather).
//
// Before tonemapping, in linear light, so the dust is lit on the same scale as the scene: the weather's own fog and
// ambient colours (a sandstorm weather's fog is already its dust) and the sun, scattered forward through the dust.
// Near the camera the view ray is marched through billowing clouds of dust that drift with the wind and thin with
// height; past that the storm is an even haze that swallows the distance and the sky. Thin streaks of sand blow past
// the camera on top.

float4 TESR_ReciprocalResolution;
float4 TESR_FogColor;
float4 TESR_SunColor;
float4 TESR_SunAmbient;
float4 TESR_SunPosition;
float4 TESR_SunAmount;      // x: daytime
float4 TESR_PBRData;        // z: direct light scale, w: ambient scale
float4 TESR_SandstormData;  // x: intensity, y: density per unit, z: visibility, w: height falloff
float4 TESR_SandstormWind;  // xy: wind direction, z: speed (units/s), w: time (s)
float4 TESR_SandstormShape; // x: billowing, y: noise scale, z: streaks, w: sun scattering
float4 TESR_SandstormColor; // rgb: sand albedo, w: brightness
float4 TESR_SandstormCover; // x: the ortho map is there
float4 TESR_SandstormLights;     // x: how strongly the nearby lights glow through the dust (0 off)
float4 TESR_SandstormEmissive;   // x: EmissiveThreshold (0 off), y: EmissiveDistance
float4 TESR_SandstormWater;      // x: 1 with the camera underwater, y: the water surface's height above the camera
float4 TESR_SandstormQuality;    // UNOFFICIAL speed settings (theirs: 12, 3, 1, 1): x march steps (4-12), y cloud octaves (1-3), z soft edges (1: the 3x3 depth tent), w fine shelter (1: 8 upwind taps per streak, 0: 4)
float4 TESR_LightPosition[12];       // NVR's nearby lights (ShaderManager::GetNearbyLights): world position, w radius
float4 TESR_ShadowLightPosition[12]; // the shadow-casting ones
float4 TESR_LightColor[24];          // rgb, w dimmer: the shadow-casting lights first, then the others
float4 TESR_ShadowFade;            // x: the sun shadows' sunrise/sunset fade (1 none), y: the sun shadow maps are there
float4x4 TESR_ShadowCameraToLightTransformOrtho;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_OrthoMapBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-18 (sandfast): the fast path's own buffers ([_Shaders.Sandstorm.Main] FastPath; see the end of the file).
// The grid is filtered linearly for the composite's reads between grid columns; the passes that build it read texel centres only.
sampler2D TESR_SandstormGrid : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SandstormGridTemp : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SandstormStreaks : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_SandstormGlow : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_SandstormGridLayout;   // xy: grid columns and rows (one per 16 x 16 screen pixels), zw: one atlas texel in UV
float4 TESR_SandstormLowLayout;    // xy: one half-resolution texel in screen UV, zw: one quarter-resolution texel
texture NoTexture;                 // never set: unbinds a pass's own render target from its sampler

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"
#include "Includes/SunCascades.hlsl"

static const int MaxSteps = 12;   // the march loop runs this many times; steps past TESR_SandstormQuality.x are skipped

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

// Hash and value noise without sin, so they stay precise far from the world's origin.
float hash13(float3 p)
{
	p = frac(p * 0.1031f);
	p += dot(p, p.zyx + 31.32f);
	return frac((p.x + p.y) * p.z);
}

float valueNoise(float3 x)
{
	float3 i = floor(x);
	float3 f = frac(x);
	f = f * f * (3.0f - 2.0f * f);
	return lerp(lerp(lerp(hash13(i), hash13(i + float3(1, 0, 0)), f.x),
	                 lerp(hash13(i + float3(0, 1, 0)), hash13(i + float3(1, 1, 0)), f.x), f.y),
	            lerp(lerp(hash13(i + float3(0, 0, 1)), hash13(i + float3(1, 0, 1)), f.x),
	                 lerp(hash13(i + float3(0, 1, 1)), hash13(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

float fbm(float3 p)
{
	// UNOFFICIAL: TESR_SandstormQuality.y octaves (theirs 3; their weights, renormalised when fewer: 3 octaves is theirs exactly).
	float sum = valueNoise(p) * 0.55f;
	float weight = 0.55f;
	[branch] if (TESR_SandstormQuality.y > 1.5f) {
		sum += valueNoise(p * 2.03f + 17.1f) * 0.3f;
		weight += 0.3f;
		[branch] if (TESR_SandstormQuality.y > 2.5f) {
			sum += valueNoise(p * 4.01f + 41.7f) * 0.15f;
			weight += 0.15f;
		}
	}
	return sum / weight;
}

// Henyey-Greenstein: how much light the dust scatters toward the eye, g > 0 forward.
float phase(float cosTheta, float g)
{
	float g2 = g * g;
	return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f), 1.5f));
}

// --- The sun's visibility through the dust. UNOFFICIAL (sandport): this build's lookup, as VolumetricLight.fx.hlsl's MarchShadow
// does it (effects compile on their own, so it is repeated here): one atlas read from the first cascade whose sphere holds the
// point, no blend between cascades, no normal offset (the dust averages many points). Beyond the LOD cascade the dust counts as
// lit. Dust in the shadow of a wall, a rock or a building scatters no sunlight.
float SunCascadeShadow(float3 position)
{
	float4x4 lightTransform;
	float2 quadrant;
	float bleed;
	[branch] if (length(position - TESR_ShadowNearCenter.xyz) < TESR_ShadowNearCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformNear; quadrant = float2(0.0f, 0.0f); bleed = 0.1f;
	}
	else if (length(position - TESR_ShadowMiddleCenter.xyz) < TESR_ShadowMiddleCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformMiddle; quadrant = float2(0.5f, 0.0f); bleed = 0.2f;
	}
	else if (length(position - TESR_ShadowFarCenter.xyz) < TESR_ShadowFarCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformFar; quadrant = float2(0.0f, 0.5f); bleed = 0.6f;
	}
	else if (length(position - TESR_ShadowLodCenter.xyz) < TESR_ShadowLodCenter.w) {
		lightTransform = TESR_ShadowCameraToLightTransformLod; quadrant = float2(0.5f, 0.5f); bleed = 0.8f;
	}
	else return 1.0f;
	float4 lightSpace = SunCascadeTexCoord(mul(float4(position, 1.0f), lightTransform));
	[branch] if (TESR_ShadowFormatData.x == 3.0f) return ShadowPCF2x2(TESR_ShadowAtlas, lightSpace.xy * 0.5f + quadrant, lightSpace.z - 0.0002f, TESR_ShadowBlur.x);	// UNOFFICIAL PCF
	float4 moments = tex2Dlod(TESR_ShadowAtlas, float4(lightSpace.xy * 0.5f + quadrant, 0.0f, 0.0f));
	float bias = TESR_ShadowFormatData.x == 0.0f ? 0.00001f : 0.01f;
	if (TESR_ShadowFormatData.x == 0.0f) return GetLightAmountValueVSM(moments.xy, lightSpace.z, bias, bleed);
	if (TESR_ShadowFormatData.x == 1.0f) return GetLightAmountValueEVSM2(moments.xy, lightSpace.z, bias, bleed, TESR_ShadowFormatData.y);
	float2 exponents = GetEVSMExponents(40.0f, 5.0f, TESR_ShadowFormatData.y);
	float warped = WarpDepth(lightSpace.z, exponents).x;
	float depthScale = bias * exponents.x * warped;
	return ChebyshevUpperBound(moments.xz, warped, depthScale * depthScale, bleed);
}

// How much a point near the camera (camera-relative) sees the sun: 1 lit, 0 in shadow, 1 out of the shadow maps' reach. Faded
// with the sun shadows: none at a fade of 1 (sunrise and sunset; the maps may not be drawn then).
float SunVisibility(float3 relative)
{
	[branch] if (TESR_ShadowFade.y < 0.5f || TESR_ShadowFade.x >= 0.999f) return 1.0f;
	return lerp(saturate(SunCascadeShadow(relative)), 1.0f, saturate(TESR_ShadowFade.x));
}


// Whether anything stands above a point near the camera (camera-relative), by NVR's top-down depth map: 0 if
// something does, 1 in the open. InTheWind asks it upwind of the point. Out of the map's reach, or without it, counts as open.
float OpenAbove(float3 relative)
{
	[branch] if (TESR_SandstormCover.x < 0.5f) return 1.0f;
	float4 ortho = mul(float4(relative, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	ortho.xyz /= ortho.w;
	if (abs(ortho.x) > 1.0f || abs(ortho.y) > 1.0f || ortho.z < 0.0f || ortho.z > 1.0f) return 1.0f;
	float top = tex2Dlod(TESR_OrthoMapBuffer, float4(ortho.x * 0.5f + 0.5f, ortho.y * -0.5f + 0.5f, 0.0f, 0.0f)).r;
	return top > ortho.z - 0.0001f;
}

// Whether the wind reaches a point near the camera, or something upwind (a wall, a rock, a building) puts it in the
// lee. A wind shadow runs about five times as far as the obstacle is tall, so looking k units upwind, only something
// standing more than k / 5 above the point shelters it: a low rock a short way, a cliff a long way. What is overhead
// does not matter (sand blows in under roofs).
static const float LeeReach[8] = { 50.0f, 110.0f, 190.0f, 290.0f, 410.0f, 550.0f, 710.0f, 900.0f };

float InTheWind(float3 relative, float2 wind)
{
	float reached = 1.0f;
	[unroll] for (int k = 0; k < 8; k++)
		reached *= OpenAbove(relative + float3(-wind * LeeReach[k], LeeReach[k] * 0.2f));
	return reached;
}

// The same, coarser (every other reach), for the dust near the camera where it is asked many times per pixel.
float InTheWindCoarse(float3 relative, float2 wind)
{
	float reached = 1.0f;
	[unroll] for (int k = 1; k < 8; k += 2)
		reached *= OpenAbove(relative + float3(-wind * LeeReach[k], LeeReach[k] * 0.2f));
	return reached;
}


// How far along the view ray a pixel's surface is; the sky lies well beyond what the storm lets through. Past glass:
// while the storm blows, see-through blended surfaces write no depth (Render.cpp, SandstormGlass), so what lies
// behind a window gets its own dust; cut-outs (fences, wire) keep theirs.
float ViewDistance(float2 coord, float visibility)
{
	float depth = readDepthLod(coord);   // (LOD 0: the same as tex2D on the depth buffer, and allowed in the soft-edges branch)
	if (depth / farZ >= 0.9961f) return visibility * 6.0f;
	return length(toWorld(coord) * depth);
}

// The glow of a point light through the dust: the light it scatters toward the eye along the whole view ray up to the
// surface, in closed form (the single-scattering integral of an inverse-square light along a line, Sun et al. 2005's
// airlight). Behind a wall it stops at the wall; it dims by the dust between the eye and the light. The core is softened
// over a tenth of the light's radius so it never blows out.
// light: xyz world position, w radius; color: rgb, w dimmer (as the game gives them).
float3 PointGlow(float4 light, float4 color, float3 camera, float3 dir, float distance, float density) {
    [branch] if (light.w <= 0.0f || color.w <= 0.0f) return 0.0f;
    float3 toLight = light.xyz - camera;
    float along = dot(toLight, dir);
    float across = sqrt(max(dot(toLight, toLight) - along * along, 0.0f) + light.w * light.w * 0.01f);
    float swept = (atan((distance - along) / across) + atan(along / across)) / across;
    float through = exp(-density * clamp(along, 0.0f, distance));
    return linearize(color.rgb) * color.w * light.w * light.w * swept * through;
}


float4 SandstormPS(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float4 color = linearize(tex2D(TESR_RenderedBuffer, uv));

	float intensity = TESR_SandstormData.x;
	float density = TESR_SandstormData.y * intensity;
	float visibility = TESR_SandstormData.z;
	float heightFalloff = TESR_SandstormData.w;
	float time = TESR_SandstormWind.w;
	float2 wind = TESR_SandstormWind.xy;
	float speed = TESR_SandstormWind.z;
	float billowing = TESR_SandstormShape.x;
	float noiseScale = TESR_SandstormShape.y;

	float3 dir = normalize(toWorld(uv));

	// How far the dust runs before the pixel's surface, from a 3x3 tent of taps around it (5 pixels across) averaged as how much
	// of the scene shows through. A half-transparent edge (hair, foliage, a fringe drawn with blending) holds the
	// colour of what is behind it, undusted, but only the near surface's depth: on its own depth it got no dust and
	// showed as a dark rim. Averaged, it gets about as much as the mix of the two it shows. Even surfaces are unchanged.
	float fullDensity = max(TESR_SandstormData.y, 1e-6f);
	float distance = ViewDistance(uv, visibility);
	[branch] if (TESR_SandstormQuality.z > 0.5f && distance < visibility * 5.99f) {
		float2 texel = TESR_ReciprocalResolution.xy * 2.0f;   // taps two pixels apart: hair fringes run a few pixels wide
		float centerShown = exp(-distance * fullDensity);
		float shown = 4.0f * centerShown;
		[unroll] for (int y = -1; y <= 1; y++)
			[unroll] for (int x = -1; x <= 1; x++)
				if (x != 0 || y != 0)
					shown += (2 - abs(x)) * (2 - abs(y)) * min(exp(-ViewDistance(uv + float2(x, y) * texel, visibility) * fullDensity), centerShown);
		distance = min(-log(max(shown / 16.0f, 1e-9f)) / fullDensity, visibility * 6.0f);
	}

	float3 camera = TESR_CameraPosition.xyz;

	// Underwater the dust lies only above the surface: the march starts where the view ray leaves the water, and a ray
	// that never does (down, sideways, or onto something under the water first) gets none. Positions handed to the
	// shadow and shelter lookups stay relative to the real camera (start + the distance along the march).
	// Seen from below, the surface itself is what the depth holds: the world above shows on it as its refraction, a
	// picture drawn before the storm. A pixel whose depth is the surface, looking up through it, is taken to show the world
	// above far off, as the open sky in the storm. Its ripples would go under the dust with it: the surface's own fine
	// light and dark (each pixel against the mean of four 3 pixels around it) is laid back over the dusted colour. Only
	// gentle differences: the hard edges of what the refraction shows (mountains against the sky) would let the world
	// through the storm, so differences past 12% fade out and past 30% are dropped.
	float start = 0.0f;
	float surfaceDetail = 1.0f;
	[branch] if (TESR_SandstormWater.x > 0.0f) {
		start = dir.z > 1e-3f ? TESR_SandstormWater.y / dir.z : 1e9f;
		[branch] if (start < 1e8f && abs(distance - start) <= max(64.0f, start * 0.05f)) {
			distance = start + visibility * 6.0f;
			float2 spread = TESR_ReciprocalResolution.xy * 3.0f;
			const float3 toLuma = float3(0.2126f, 0.7152f, 0.0722f);
			float around = dot(linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv + float2(spread.x, 0.0f), 0.0f, 0.0f))).rgb, toLuma)
				+ dot(linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv - float2(spread.x, 0.0f), 0.0f, 0.0f))).rgb, toLuma)
				+ dot(linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv + float2(0.0f, spread.y), 0.0f, 0.0f))).rgb, toLuma)
				+ dot(linearize(tex2Dlod(TESR_RenderedBuffer, float4(uv - float2(0.0f, spread.y), 0.0f, 0.0f))).rgb, toLuma);
			float difference = dot(color.rgb, toLuma) / max(around * 0.25f, 1e-4f) - 1.0f;
			surfaceDetail = 1.0f + 0.8f * difference * (1.0f - smoothstep(0.12f, 0.3f, abs(difference)));
		}
		[branch] if (start >= distance) return delinearize(color);
		distance -= start;
		camera += dir * start;
	}

	// The light on the dust: the weather's fog colour and ambient all round, the sun scattered forward.
	float daytime = smoothstep(0.1f, 0.6f, TESR_SunAmount.x);
	float3 ambient = (linearize(TESR_FogColor.rgb) * 0.6f + linearize(TESR_SunAmbient.rgb) * 0.4f) * TESR_PBRData.w;
	float3 sunLight = linearize(TESR_SunColor.rgb) * TESR_PBRData.z * daytime;
	float sunPhase = phase(dot(dir, normalize(TESR_SunPosition.xyz)), 0.55f) * 4.0f * 3.14159265f;
	// Ambient and sun kept apart: the sun's share is shadowed wherever the dust is (SunVisibility).
	float3 dustAmbient = TESR_SandstormColor.rgb * ambient * TESR_SandstormColor.w;
	float3 dustSun = TESR_SandstormColor.rgb * sunLight * lerp(0.35f, sunPhase, saturate(TESR_SandstormShape.w * 0.5f)) * max(TESR_SandstormShape.w, 0.25f)
		* TESR_SandstormColor.w;
	float3 dustLight = dustAmbient + dustSun;   // out in the open

	// The wind carries the dust; it also rolls slowly upward.
	float3 drift = float3(wind * speed * time, -speed * 0.05f * time);
	float jitter = frac(52.9829189f * frac(dot(uv / TESR_ReciprocalResolution.xy, float2(0.06711056f, 0.00583715f))) + time * 7.123f);

	// Billowing dust, marched through the first stretch of the ray (steps closer together near the camera).
	float marchEnd = min(distance, visibility * 1.5f);
	float transmittance = 1.0f;
	float3 scattered = 0.0f;
	// The same, only as far as EmissiveDistance: what an emissive effect drawn without depth (fire, sparks) is seen through.
	float nearCut = TESR_SandstormEmissive.y;
	float nearTransmittance = 1.0f;
	float3 nearScattered = 0.0f;
	const float Steps = TESR_SandstormQuality.x;   // UNOFFICIAL: theirs 12
	[loop] for (int i = 0; i < MaxSteps; i++) {
		[branch] if (i >= Steps) continue;
		float t0 = marchEnd * pow((i + jitter) / Steps, 2.0f);
		float t1 = marchEnd * pow(min(i + 1 + jitter, Steps) / Steps, 2.0f);
		float3 p = camera + dir * (0.5f * (t0 + t1));
		float height = exp(-max(p.z - camera.z + 200.0f, 0.0f) / heightFalloff);
		float clouds = fbm((p - drift) / noiseScale);
		float local = density * height * lerp(1.0f, saturate(clouds * 2.2f - 0.35f) * 1.8f, billowing) * (t1 - t0);
		// In the lee of a wall or a rock the air near the camera is calmer: some dust hangs in it, less blows through.
		float middle = 0.5f * (t0 + t1);
		[branch] if (start + middle < 500.0f) local *= lerp(lerp(0.35f, 1.0f, InTheWindCoarse(dir * (start + middle), wind)), 1.0f, smoothstep(350.0f, 500.0f, start + middle));
		float absorbed = 1.0f - exp(-local);
		float3 light = dustAmbient + dustSun * SunVisibility(dir * (start + middle));
		scattered += transmittance * absorbed * light * (0.8f + 0.4f * clouds);
		transmittance *= 1.0f - absorbed;
		float nearShare = saturate((nearCut - t0) / max(t1 - t0, 1e-3f));
		float nearAbsorbed = 1.0f - exp(-local * nearShare);
		nearScattered += nearTransmittance * nearAbsorbed * light * (0.8f + 0.4f * clouds);
		nearTransmittance *= 1.0f - nearAbsorbed;
	}

	// Beyond the march: an even haze to the scene, or the sky, behind it.
	float endHeight = exp(-max(dir.z * marchEnd + 200.0f, 0.0f) / heightFalloff);
	float haze = 1.0f - exp(-max(distance - marchEnd, 0.0f) * density * lerp(endHeight, 1.0f, 0.5f));
	scattered += transmittance * haze * dustLight;
	transmittance *= 1.0f - haze;
	float3 dustScattered = scattered;   // the dust alone; what follows (streaks, light glow) is in front of anything

	// Streaks: thin sand blown past the camera, stretched along the wind. Only where nothing nearer is in the way, fading
	// out before the surface behind, and where the wind reaches (InTheWind): none in the lee of a wall or a rock.
	float3 streaks = 0.0f;
	float2 across = float2(-wind.y, wind.x);
	[branch] if (TESR_SandstormShape.z > 0.0f)   // UNOFFICIAL: none to draw at StreakStrength 0 (theirs computed them and multiplied by 0)
	[unroll] for (int s = 0; s < 3 && start <= 0.0f; s++) {   // (none underwater: the camera is in the water)
		float t = 50.0f + 120.0f * s + 30.0f * jitter;
		if (t < distance) {
			float3 p = camera + dir * t - float3(wind * speed * time * (2.5f + s), 0.0f);
			float3 q = float3(dot(p.xy, wind) / 240.0f, dot(p.xy, across) / 8.0f, p.z / 6.0f);
			float sheltered;   // UNOFFICIAL FineShelter: theirs 8 upwind taps; off, the march's 4 (HLSL's ?: would evaluate both)
			[branch] if (TESR_SandstormQuality.w > 0.5f) sheltered = InTheWind(dir * t, wind);
			else sheltered = InTheWindCoarse(dir * t, wind);
			float visible = saturate((distance - t) / 60.0f) * sheltered;
			float grain = smoothstep(0.74f, 0.96f, valueNoise(q + s * 31.7f)) * (1.0f - s * 0.25f) * visible;
			[branch] if (grain > 0.0f) streaks += grain * (dustAmbient + dustSun * SunVisibility(dir * t));
		}
	}
	scattered += streaks * TESR_SandstormShape.z * intensity * 0.25f;

	// The nearby lights glowing through the dust, as the sun does (lamps, fires, signs).
	[branch] if (TESR_SandstormLights.x > 0.0f && start <= 0.0f) {   // (not underwater)
		float3 glow = 0.0f;
		[loop] for (int l = 0; l < 12; l++) {
			glow += PointGlow(TESR_ShadowLightPosition[l], TESR_LightColor[l], camera, dir, distance, density);
			glow += PointGlow(TESR_LightPosition[l], TESR_LightColor[12 + l], camera, dir, distance, density);
		}
		scattered += glow * density * TESR_SandstormColor.rgb * TESR_SandstormLights.x * 0.01f;
	}

	float3 result = color.rgb * transmittance + scattered;

	// Fire, sparks and the like are drawn without depth, so the depth here is what lies behind them and they would sink
	// into the dust of that distance. A pixel far brighter than lit dust can be is taken as something glowing in front:
	// it gets only the dust within EmissiveDistance. The sky needs twice the margin, so a bright sky stays buried.
	float rawDepth = readDepth(uv);
	bool skyBehind = rawDepth / farZ >= 0.9961f;
	float emissive = 0.0f;
	[branch] if (TESR_SandstormEmissive.x > 0.0f) {
		float dustLuma = max(dot(dustLight, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f) * TESR_SandstormEmissive.x * (skyBehind ? 2.0f : 1.0f);
		emissive = saturate((dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f)) - dustLuma) / dustLuma);
		float3 nearResult = color.rgb * nearTransmittance + nearScattered + (scattered - dustScattered);
		result = lerp(result, nearResult, emissive);
	}
	result *= surfaceDetail;   // the water surface's ripples, seen from below (1 elsewhere)
	return delinearize(float4(result, color.a));
}


// ==== UNOFFICIAL 2026-10-18 (sandfast): the fast path ([_Shaders.Sandstorm.Main] FastPath, on by default) ====
// The same storm as SandstormPS above (the reference, technique 0), worked out in ~1/10 of the time. The full-resolution
// march (12 steps x 24 hashes, a cascade lookup and up to 4 top-down depth taps per step, per pixel), the streaks (3 x 8 hashes
// + 8 upwind taps per pixel) and the lamp glow (24 lamps per pixel) cost 17.6 ms at 2560 x 1440 on a GTX 1070.
//   1 GridDust  (grid atlas): the dust is a volume, not a picture: one ray per 16 x 16 screen pixels, cut into 64 slices out
//               to the march's reach (1.5 x Visibility, spaced by the square of the slice number as the march's steps). Each
//               cell is one of the march's steps, evaluated once at its middle: the same value noise, height falloff, shelter
//               near the camera and sun shadow. r g b: the light it scatters toward the eye, a: its optical depth.
//               The dust clouds are large on screen (the finest octave is 225 units, 87 pixels or more out to the march's
//               end), so a column per 16 pixels holds them. The layout is VolumetricLight's froxel grid (Froxels): the 64
//               slices sit in one 2D texture, 8 x 8 tiles of columns x rows (D3D9 cannot draw into a 3D texture).
//   2 GridRow, 3 GridCarry: the front-to-back sum along each ray in two passes (VolumetricLight's prefix sum), with the
//               'over' operator instead of '+': (S, tau) of a stretch in front, then one behind, is (S_front + exp(-tau_front)
//               S_back, tau_front + tau_back) -- associative, so it can be summed in pieces. Slice z then holds the dust from
//               the eye to its far side. Optical depth rather than transmittance: half floats hold it far better near 1.
//   4 Streaks   (half resolution): what of the streaks does not depend on the pixel's depth: each of the three layers' grain
//               (their noise, their shelter upwind, the layer's weight) and the sun on them.
//   6 Glow      (quarter resolution): the lamps' glow at the block's depth, with the depth for the upsample. The glow is a wide,
//               faint halo (at most a few 8-bit steps over the dust), so a quarter of the size holds it.
//   5 Composite (full resolution, in the frame chain): each pixel reads the grid at its own distance (two reads, linear
//               between columns and between slices), adds the haze beyond the march, the streaks (each layer seen only in front
//               of the pixel's surface, as the reference) and the glow (depth-aware upsample), and the emissive test.
// The grid does not depend on the scene's depth at all, so edges (grass blades, wires, fences) get exactly their own dust;
// there are no march bands and no noise to hide (no jitter, no history). Differences from the reference: the dust is the
// converged integral instead of 12 jittered steps (the reference's own step noise is gone), smoothed over 16 pixels across;
// the streaks at half resolution, the glow at a quarter.
// Sources: the froxel volume after Wronski 2014 ('Volumetric Fog', SIGGRAPH Advances in Real-Time Rendering) and Hillaire 2015
// ('Physically Based and Unified Volumetric Rendering in Frostbite'); front-to-back compositing with Porter & Duff's 'over' (1984);
// the grid layout and two-pass sum are this build's VolumetricLight froxel grid. The storm itself: Physically-Based-Reloaded
// (Balebandro, Alejandro Martinez), GPL-3.0.

#ifndef SANDFAST_STREAK_SAMPLES
#define SANDFAST_STREAK_SAMPLES 4   // reads per streak layer in the half-resolution pass (spread over the reference's 30-unit jitter)
#endif

static const float2 GridSize = TESR_SandstormGridLayout.xy;
static const float2 GridAtlasTexel = TESR_SandstormGridLayout.zw;
static const float2 LowTexel = TESR_SandstormLowLayout.xy;
static const float2 QuarterTexel = TESR_SandstormLowLayout.zw;

float GridEnd() { return TESR_SandstormData.z * 1.5f; }   // the march's reach: Visibility x 1.5
// Distance along the view ray of slice coordinate s (0 .. 64, the near side of slice s).
float GridDistance(float s) { s /= 64.0f; return GridEnd() * s * s; }

// The atlas UV of slice z at grid coordinate local (column k's centre at k + 0.5), kept inside the slice's tile.
float2 GridUV(float z, float2 local) {
	float2 tile = float2(fmod(z, 8.0f), floor(z / 8.0f));
	return (tile * GridSize + clamp(local, 0.5f, GridSize - 0.5f)) * GridAtlasTexel;
}

// InTheWind / InTheWindCoarse with the top-down map's transform worked out once: a camera-relative point's position in the
// map is linear in the point (homogeneous coordinates), so each upwind tap is the point's position plus the reach times one
// step. The same test, tap for tap (OpenAbove), up to float rounding.
float OpenAboveAt(float4 ortho) {
	ortho.xyz /= ortho.w;
	float top = tex2Dlod(TESR_OrthoMapBuffer, float4(ortho.x * 0.5f + 0.5f, ortho.y * -0.5f + 0.5f, 0.0f, 0.0f)).r;
	bool outside = abs(ortho.x) > 1.0f || abs(ortho.y) > 1.0f || ortho.z < 0.0f || ortho.z > 1.0f;
	return (outside || top > ortho.z - 0.0001f) ? 1.0f : 0.0f;
}

float InTheWindFast(float3 relative, float2 wind, bool fine) {
	[branch] if (TESR_SandstormCover.x < 0.5f) return 1.0f;
	float4 base = mul(float4(relative, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	float4 step = mul(float4(-wind, 0.2f, 0.0f), TESR_ShadowCameraToLightTransformOrtho);
	float reached = 1.0f;
	[branch] if (fine) {
		[unroll] for (int k = 0; k < 8; k++) reached *= OpenAboveAt(base + step * LeeReach[k]);
	}
	else {
		[unroll] for (int k = 1; k < 8; k += 2) reached *= OpenAboveAt(base + step * LeeReach[k]);
	}
	return reached;
}

// The light on the dust (as SandstormPS): ambient all round, the sun scattered forward (its share shadowed where the dust is).
void DustLighting(float3 dir, out float3 dustAmbient, out float3 dustSun) {
	float daytime = smoothstep(0.1f, 0.6f, TESR_SunAmount.x);
	float3 ambient = (linearize(TESR_FogColor.rgb) * 0.6f + linearize(TESR_SunAmbient.rgb) * 0.4f) * TESR_PBRData.w;
	float3 sunLight = linearize(TESR_SunColor.rgb) * TESR_PBRData.z * daytime;
	float sunPhase = phase(dot(dir, normalize(TESR_SunPosition.xyz)), 0.55f) * 4.0f * 3.14159265f;
	dustAmbient = TESR_SandstormColor.rgb * ambient * TESR_SandstormColor.w;
	dustSun = TESR_SandstormColor.rgb * sunLight * lerp(0.35f, sunPhase, saturate(TESR_SandstormShape.w * 0.5f)) * max(TESR_SandstormShape.w, 0.25f)
		* TESR_SandstormColor.w;
}

// ---- 1: GridDust, the grid atlas ----
float4 GridDustPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 tile = floor(vpos / GridSize);
	float2 cell = vpos - tile * GridSize;          // the column, 0 .. GridSize - 1
	float z = tile.y * 8.0f + tile.x;
	float t0 = GridDistance(z), t1 = GridDistance(z + 1.0f);
	float3 dir = normalize(toWorld((cell + 0.5f) / GridSize));

	float intensity = TESR_SandstormData.x;
	float density = TESR_SandstormData.y * intensity;
	float heightFalloff = TESR_SandstormData.w;
	float time = TESR_SandstormWind.w;
	float2 wind = TESR_SandstormWind.xy;
	float speed = TESR_SandstormWind.z;
	float3 camera = TESR_CameraPosition.xyz;
	float3 dustAmbient, dustSun;
	DustLighting(dir, dustAmbient, dustSun);

	// One step of SandstormPS's march, at the cell's middle.
	float3 drift = float3(wind * speed * time, -speed * 0.05f * time);
	float middle = 0.5f * (t0 + t1);
	float3 p = camera + dir * middle;
	float height = exp(-max(p.z - camera.z + 200.0f, 0.0f) / heightFalloff);
	float clouds = fbm((p - drift) / TESR_SandstormShape.y);
	float local = density * height * lerp(1.0f, saturate(clouds * 2.2f - 0.35f) * 1.8f, TESR_SandstormShape.x) * (t1 - t0);
	// The sun's shadow and the shelter upwind are steps (the edge of a wall's shadow, of the lee behind it), not smooth like the
	// clouds: read at four points spread over the cell (a quarter column each way across, four depths through the slice) and
	// averaged, so the composite's blend between columns is smooth instead of showing 16-pixel blocks where the reference's
	// per-pixel steps show grain.
	float2 spread = 0.25f / GridSize;
	float2 centre = (cell + 0.5f) / GridSize;
	float3 sub[4] = { normalize(toWorld(centre + float2(-spread.x, -spread.y))) * lerp(t0, t1, 0.125f),
	                  normalize(toWorld(centre + float2(spread.x, -spread.y))) * lerp(t0, t1, 0.625f),
	                  normalize(toWorld(centre + float2(-spread.x, spread.y))) * lerp(t0, t1, 0.875f),
	                  normalize(toWorld(centre + float2(spread.x, spread.y))) * lerp(t0, t1, 0.375f) };
	[branch] if (middle < 500.0f) {
		float open = 0.25f * (InTheWindFast(sub[0], wind, false) + InTheWindFast(sub[1], wind, false) + InTheWindFast(sub[2], wind, false) + InTheWindFast(sub[3], wind, false));
		local *= lerp(lerp(0.35f, 1.0f, open), 1.0f, smoothstep(350.0f, 500.0f, middle));
	}
	float absorbed = 1.0f - exp(-local);
	float sun = 0.25f * (SunVisibility(sub[0]) + SunVisibility(sub[1]) + SunVisibility(sub[2]) + SunVisibility(sub[3]));
	float3 light = dustAmbient + dustSun * sun;
	return float4(absorbed * light * (0.8f + 0.4f * clouds), local);
}

// A stretch of dust in front, then one behind it: (light scattered toward the eye, optical depth).
float4 Over(float4 front, float4 back) { return float4(front.rgb + exp(-front.a) * back.rgb, front.a + back.a); }

// ---- 2: GridRow: each cell over the cells before it in its row of 8 tiles (slices 8r .. 8r + 7) ----
float4 GridRowPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 tile = floor(vpos / GridSize);
	float2 cell = vpos - tile * GridSize + 0.5f;
	float4 sum = 0.0f;
	[unroll] for (int c = 0; c < 8; c++)
		if (c <= tile.x) sum = Over(sum, tex2Dlod(TESR_SandstormGrid, float4(GridUV(tile.y * 8.0f + c, cell), 0.0f, 0.0f)));
	return sum;
}

// ---- 3: GridCarry: the totals of the rows before, then the cell's row so far. Slice z then holds the eye to its far side ----
float4 GridCarryPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 tile = floor(vpos / GridSize);
	float2 cell = vpos - tile * GridSize + 0.5f;
	float4 sum = 0.0f;
	[unroll] for (int r = 0; r < 7; r++)
		if (r < tile.y) sum = Over(sum, tex2Dlod(TESR_SandstormGridTemp, float4(GridUV(r * 8.0f + 7.0f, cell), 0.0f, 0.0f)));
	return Over(sum, tex2Dlod(TESR_SandstormGridTemp, float4(GridUV(tile.y * 8.0f + tile.x, cell), 0.0f, 0.0f)));
}

// The dust from the eye to distance d (at most GridEnd) along the view through uv: (light scattered toward the eye, optical depth).
float4 DustAt(float2 uv, float d) {
	float s = sqrt(saturate(d / GridEnd())) * 64.0f;   // slice coordinate of the distance
	float z = min(floor(s), 63.0f);
	float2 local = uv * GridSize;
	float4 farSide = tex2Dlod(TESR_SandstormGrid, float4(GridUV(z, local), 0.0f, 0.0f));
	float4 nearSide = z > 0.5f ? tex2Dlod(TESR_SandstormGrid, float4(GridUV(z - 1.0f, local), 0.0f, 0.0f)) : 0.0f;
	return lerp(nearSide, farSide, saturate(s - z));
}

// PointGlow with one arc tangent: atan(a) + atan(b) = atan2(a + b, 1 - a b), and with a = (d - along) / c, b = along / c both
// scaled by c^2 (> 0): atan2(d c, c^2 - (d - along) along), in (0, pi) since d c > 0. The same value up to float rounding.
float3 PointGlowFast(float4 light, float4 color, float3 camera, float3 dir, float distance, float density) {
	[branch] if (light.w <= 0.0f || color.w <= 0.0f) return 0.0f;
	float3 toLight = light.xyz - camera;
	float along = dot(toLight, dir);
	float across2 = max(dot(toLight, toLight) - along * along, 0.0f) + light.w * light.w * 0.01f;
	float across = sqrt(across2);
	float swept = atan2(distance * across, across2 - (distance - along) * along) / across;
	float through = exp(-density * clamp(along, 0.0f, distance));
	return linearize(color.rgb) * color.w * light.w * light.w * swept * through;
}

// ---- 4: Streaks, half resolution: r g b each layer's grain, a the sun on them ----
float4 StreaksPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5f) * LowTexel;   // the block's centre on screen
	float3 dir = normalize(toWorld(uv));
	float time = TESR_SandstormWind.w;
	float2 wind = TESR_SandstormWind.xy;
	float speed = TESR_SandstormWind.z;
	float3 camera = TESR_CameraPosition.xyz;

	// Streaks, as SandstormPS, without what depends on the pixel's surface (the composite applies that per pixel): each layer's
	// grain, its shelter upwind, its weight, and the sun on it. The reference reads each layer at one distance jittered per pixel
	// over 30 units (fine grain that changes every frame); a jitter per half-resolution block would show as a pattern of dots
	// once upsampled, so here each layer is the mean of SANDFAST_STREAK_SAMPLES (4) reads spread evenly over those 30 units (what the
	// reference's grain averages to). The four blocks of each 2 x 2 group start their reads a sixteenth of the spread apart (an
	// ordered pattern), and the composite reads this buffer through a 3 x 3 tent, which weighs the four alike: 16 evenly spread
	// reads per pixel for the price of 4, no grain, no dots. The whole spread moves every frame as the reference's jitter does.
	// Shelter and sun are read once, at the middle of the 30 units.
	float3 grains = 0.0f;
	float sunlit = 0.0f;
	[branch] if (TESR_SandstormShape.z > 0.0f) {
		float2 group = vpos - 2.0f * floor(vpos * 0.5f);   // 0 or 1 each way: the block's place in its 2 x 2 group
		float jitter = frac((group.x + 2.0f * group.y + 0.5f) / (4.0f * SANDFAST_STREAK_SAMPLES) + time * 7.123f);
		float2 across = float2(-wind.y, wind.x);
		const bool fine = TESR_SandstormQuality.w > 0.5f;
		[unroll] for (int s = 0; s < 3; s++) {
			float grain = 0.0f;
			[unroll] for (int m = 0; m < SANDFAST_STREAK_SAMPLES; m++) {
				float t = 50.0f + 120.0f * s + 30.0f * frac(jitter + (float)m / SANDFAST_STREAK_SAMPLES);
				float3 p = camera + dir * t - float3(wind * speed * time * (2.5f + s), 0.0f);
				float3 q = float3(dot(p.xy, wind) / 240.0f, dot(p.xy, across) / 8.0f, p.z / 6.0f);
				grain += smoothstep(0.74f, 0.96f, valueNoise(q + s * 31.7f));
			}
			grain *= (1.0f - s * 0.25f) / SANDFAST_STREAK_SAMPLES;
			float tMiddle = 65.0f + 120.0f * s;
			[branch] if (grain > 0.0f) {
				grain *= InTheWindFast(dir * tMiddle, wind, fine);
				[branch] if (grain > 0.0f) sunlit += grain * SunVisibility(dir * tMiddle);
			}
			grains[s] = grain;
		}
	}
	float total = grains.x + grains.y + grains.z;
	return float4(grains, total > 0.0f ? sunlit / total : 1.0f);
}

// ---- 6: Glow, quarter resolution: the nearby lights glowing through the dust, at this block's depth (rgb), and the depth (a) ----
float4 GlowPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 uv = (vpos + 0.5f) * QuarterTexel - 0.5f * QuarterTexel + 1.5f * TESR_ReciprocalResolution.xy;   // pixel (1, 1) of the 4 x 4 block
	float depth = readDepthLod(uv);
	float visibility = TESR_SandstormData.z;
	float distance = depth / farZ >= 0.9961f ? visibility * 6.0f : length(toWorld(uv) * depth);   // as ViewDistance
	float3 dir = normalize(toWorld(uv));
	float density = TESR_SandstormData.y * TESR_SandstormData.x;
	float3 camera = TESR_CameraPosition.xyz;
	float3 glow = 0.0f;
	[branch] if (TESR_SandstormLights.x > 0.0f) {
		[loop] for (int l = 0; l < 12; l++) {
			glow += PointGlowFast(TESR_ShadowLightPosition[l], TESR_LightColor[l], camera, dir, distance, density);
			glow += PointGlowFast(TESR_LightPosition[l], TESR_LightColor[12 + l], camera, dir, distance, density);
		}
		glow *= density * TESR_SandstormColor.rgb * TESR_SandstormLights.x * 0.01f;
	}
	return float4(glow, depth / farZ);
}

// The glow at a full-resolution pixel: the four quarter-resolution blocks round it, bilinearly, each weighed down by how far
// its depth is from the pixel's (VolumetricLight's composite upsample).
float3 GlowAt(float2 uv, float depth01) {
	float2 position = uv / QuarterTexel - 0.5f;
	float2 base = floor(position);
	float2 fraction = position - base;
	float tolerance = max(depth01 * 0.04f, 0.00002f);
	float3 glow = 0.0f;
	float weights = 1.0e-5f;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float4 tap = tex2Dlod(TESR_SandstormGlow, float4((base + float2(x, y) + 0.5f) * QuarterTexel, 0.0f, 0.0f));
			float weight = (x ? fraction.x : 1.0f - fraction.x) * (y ? fraction.y : 1.0f - fraction.y);
			weight /= 1.0f + abs(tap.a - depth01) / tolerance;
			glow += tap.rgb * weight;
			weights += weight;
		}
	}
	return glow / weights;
}

// ---- 5: Composite, full resolution (SandstormPS with the march, streaks and glow read from the buffers above) ----
float4 CompositePS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 color = linearize(tex2D(TESR_RenderedBuffer, uv));

	float intensity = TESR_SandstormData.x;
	float density = TESR_SandstormData.y * intensity;
	float visibility = TESR_SandstormData.z;
	float heightFalloff = TESR_SandstormData.w;
	float time = TESR_SandstormWind.w;
	float3 dir = normalize(toWorld(uv));

	// How far the dust runs before the pixel's surface: as SandstormPS (the 3 x 3 tent with SoftEdges).
	float fullDensity = max(TESR_SandstormData.y, 1e-6f);
	float centerDist = ViewDistance(uv, visibility);
	float distance = centerDist;
	[branch] if (TESR_SandstormQuality.z > 0.5f && centerDist < visibility * 5.99f) {
		float2 texel = TESR_ReciprocalResolution.xy * 2.0f;
		float centerShown = exp(-centerDist * fullDensity);
		float shown = 0.0f;
		[unroll] for (int y = -1; y <= 1; y++)
			[unroll] for (int x = -1; x <= 1; x++) {
				float w = (2 - abs(x)) * (2 - abs(y));
				float tapShown = (x == 0 && y == 0) ? centerShown : min(exp(-ViewDistance(uv + float2(x, y) * texel, visibility) * fullDensity), centerShown);
				shown += w * tapShown;
			}
		distance = min(-log(max(shown / 16.0f, 1e-9f)) / fullDensity, visibility * 6.0f);
	}

	float3 dustAmbient, dustSun;
	DustLighting(dir, dustAmbient, dustSun);
	float3 dustLight = dustAmbient + dustSun;

	// The billowing dust through the first stretch of the ray, from the grid.
	float marchEnd = min(distance, visibility * 1.5f);
	float4 dust = DustAt(uv, marchEnd);
	float transmittance = exp(-dust.a);
	float3 scattered = dust.rgb;

	// Beyond the march: an even haze to the scene, or the sky, behind it (as SandstormPS).
	float endHeight = exp(-max(dir.z * marchEnd + 200.0f, 0.0f) / heightFalloff);
	float haze = 1.0f - exp(-max(distance - marchEnd, 0.0f) * density * lerp(endHeight, 1.0f, 0.5f));
	scattered += transmittance * haze * dustLight;
	transmittance *= 1.0f - haze;
	float3 dustScattered = scattered;

	// Streaks: each layer only in front of the surface, fading out before it (SandstormPS's 'visible', the pixel's own jitter).
	[branch] if (TESR_SandstormShape.z > 0.0f) {
		float jitter = frac(52.9829189f * frac(dot(uv / TESR_ReciprocalResolution.xy, float2(0.06711056f, 0.00583715f))) + time * 7.123f);
		// The streak buffer through a 3 x 3 tent (four bilinear reads half a texel off each way): its 2 x 2 groups' reads alike.
		float4 grains = 0.25f * (tex2Dlod(TESR_SandstormStreaks, float4(uv + LowTexel * float2(-0.5f, -0.5f), 0.0f, 0.0f))
			+ tex2Dlod(TESR_SandstormStreaks, float4(uv + LowTexel * float2(0.5f, -0.5f), 0.0f, 0.0f))
			+ tex2Dlod(TESR_SandstormStreaks, float4(uv + LowTexel * float2(-0.5f, 0.5f), 0.0f, 0.0f))
			+ tex2Dlod(TESR_SandstormStreaks, float4(uv + LowTexel * float2(0.5f, 0.5f), 0.0f, 0.0f)));
		float3 visible = saturate((distance - (float3(50.0f, 170.0f, 290.0f) + 30.0f * jitter)) / 60.0f);
		scattered += dot(grains.rgb, visible) * (dustAmbient + dustSun * grains.a) * TESR_SandstormShape.z * intensity * 0.25f;
	}

	float rawDepth = readDepth(uv);
	[branch] if (TESR_SandstormLights.x > 0.0f) scattered += GlowAt(uv, rawDepth / farZ);

	float3 result = color.rgb * transmittance + scattered;

	// Emissive (as SandstormPS): the dust within EmissiveDistance only -- the grid read there.
	bool skyBehind = rawDepth / farZ >= 0.9961f;
	[branch] if (TESR_SandstormEmissive.x > 0.0f) {
		float dustLuma = max(dot(dustLight, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f) * TESR_SandstormEmissive.x * (skyBehind ? 2.0f : 1.0f);
		float emissive = saturate((dot(color.rgb, float3(0.2126f, 0.7152f, 0.0722f)) - dustLuma) / dustLuma);
		[branch] if (emissive > 0.0f) {
			float4 near = DustAt(uv, min(TESR_SandstormEmissive.y, marchEnd));
			float3 nearResult = color.rgb * exp(-near.a) + near.rgb + (scattered - dustScattered);
			result = lerp(result, nearResult, emissive);
		}
	}
	return delinearize(float4(result, color.a));
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SandstormPS();
	}
}

technique GridDust
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GridDustPS();
		Texture[4] = <NoTexture>;   // writes Grid
	}
}

technique GridRow
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GridRowPS();
		Texture[5] = <NoTexture>;   // writes GridTemp
	}
}

technique GridCarry
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GridCarryPS();
		Texture[4] = <NoTexture>;   // writes Grid
	}
}

technique Streaks
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 StreaksPS();
		Texture[6] = <NoTexture>;   // writes Streaks
	}
}

technique Composite
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CompositePS();
	}
}

technique Glow
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 GlowPS();
		Texture[7] = <NoTexture>;   // writes Glow
	}
}
