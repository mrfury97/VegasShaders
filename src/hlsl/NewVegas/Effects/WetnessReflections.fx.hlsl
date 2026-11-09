// UNOFFICIAL 2026-11-04 Wetness reflections, ported from Physically-Based-Reloaded (Balebandro, Effects/WetnessReflections.fx.hlsl):
// screen-space reflections on what Wetness Effects made wet (effects/WetnessEffects/WetnessReflections.h).
//
// The object and terrain shaders give the water film and the puddles a reflection of the sky; they cannot see the rest of the scene.
// This pass, run once the world is drawn, marches each wet pixel's reflected ray through the depth buffer and, where the ray meets
// something on screen, puts that in place of the sky the forward shaders reflected.
//
// Where it is wet, and how glossy, is worked out again here with the forward shaders' own function (Shaders/Includes/Wetness.hlsl,
// GetWetness) from the depth and the normals buffer: the same weather, puddle noise and cover. What only the forward shaders know
// (normal maps, ripples) is left out; the reflection is of the flat water surface.
//
// The scene holds the forward shaders' output, so the swap is done in its terms: the sky's share comes out (its radiance as the
// forward shaders add it, WetSkyReflection, encoded as they encode their result, LinEncode), the hit's goes in, both scaled by the
// film's reflectance there.
// VegasShaders: the depth buffer keeps the linear depth only (the projected one is worked out from it, Includes/Depth.hlsl), and
// the texture coordinates are RenderScale's rect ones (Includes/RenderRect.hlsl).

float4 TESR_ReciprocalResolution;
float4 TESR_WetnessReflectionData;   // x: strength, y: max ray distance (game units), z: thickness, w: steps
float4 TESR_WetnessReflectionFade;   // x: roughness cutoff, y: screen edge fade (uv), z: debug view, w: camera distance fade end

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

#include "Includes/Depth.hlsl"
// The forward shaders' colour encoding (Shaders/Includes/LinearLight.hlsl, whose own includes do not resolve from this folder) and
// sky reflection: the spherical harmonic sky (SkylightingMode 0; WetnessReflectionsEffect::ShouldRender runs this only then).
float4 TESR_LinearLighting;   // x: the gamma colours are decoded with (1 or less = off), y: 1 / x
float3 LinColor(float3 c) { return TESR_LinearLighting.x > 1.0f ? pow(max(c, 0.0f), TESR_LinearLighting.x) : c; }
float3 LinEncode(float3 c) { return TESR_LinearLighting.x > 1.0f ? pow(max(c, 0.0f), TESR_LinearLighting.y) : c; }
#define LinSky(c) LinColor(c)
#define SKYLIGHTING_MODE 0
#include "../Shaders/Includes/SkyAmbient.hlsl"
#define WETNESS_ORTHO_REG s3
#define PUDDLES_RUNTIME	// 2026-11-08 Puddles: the same puddle field as the surface shaders (live: TESR_PuddleData.w 0 = off)
#include "../Shaders/Includes/Wetness.hlsl"

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

static const float SSR_REFINE = 5;

#define DEBUG_VIEW (TESR_WetnessReflectionFade.z > 0.5f)

// What the pass gives back. Normally the scene (untouched, or with the reflection swapped in). The debug view instead shows, over the
// scene dimmed: how glossy the water is (blue), and the reflections found, at their confidence.
float4 Result(float4 scene, float gloss, float3 found)
{
	if (!DEBUG_VIEW) return scene;
	return float4(scene.rgb * 0.15f + float3(0.0f, 0.0f, gloss * 0.7f) + found, 1.0f);
}

// A view-space point's texture coordinates (rect) and whether it is on screen.
float2 RayUV(float3 q, out bool onScreen)
{
	float4 qc = mul(float4(q, 1.0f), TESR_ProjectionTransform);
	float2 s = qc.xy / max(qc.w, 1e-6f) * float2(0.5f, -0.5f) + 0.5f;
	onScreen = qc.w > 0.0f && all(s >= 0.0f) && all(s <= 1.0f);
	return screenToRect(s);
}

float4 WetReflections(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float4 scene = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0f, 0.0f));

	// The surface: view-space position and normal, world-space (camera-relative) for the wetness.
	float2 s01 = rectToScreen(uv);
	float4 clip = float4(s01.x * 2.0f - 1.0f, (1.0f - s01.y) * 2.0f - 1.0f, projectedDepthFromLinear(tex2Dlod(TESR_DepthBuffer, float4(uv, 0.0f, 0.0f)).x), 1.0f);
	float4 view = mul(clip, TESR_InvProjectionTransform);
	float3 P = view.xyz / view.w;
	if (P.z <= 0.0f || P.z > TESR_WetnessReflectionFade.w) return Result(scene, 0.0f, 0.0f);   // sky, or too far to matter

	float3 V = normalize(P);
	float3 N = normalize(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0.0f, 0.0f)).xyz * 2.0f - 1.0f);
	if (dot(N, V) > 0.0f) N = -N;   // toward the camera

	float3 worldPos = mul(float4(P, 1.0f), TESR_InvViewTransform).xyz;
	float3 worldN = normalize(mul(float4(N, 0.0f), TESR_InvViewTransform).xyz);
	Wetness wet = GetWetness(worldPos, worldN, worldN);
	float gloss = wet.gloss;
	float rough = wet.roughness;
	float glossy = gloss * (1.0f - smoothstep(TESR_WetnessReflectionFade.x * 0.5f, TESR_WetnessReflectionFade.x, rough));
	if (glossy <= 0.01f) return Result(scene, gloss, 0.0f);

	// The reflected ray, in view space. One pointing back at the camera has nothing on screen to find.
	float3 R = reflect(V, N);
	float towardScreen = saturate(R.z * 4.0f + 0.25f);
	if (towardScreen <= 0.0f) return Result(scene, gloss, 0.0f);

	// Marched in clip space, as SunShadows: the projection is linear in homogeneous coordinates, so the ray is a straight line there.
	// Steps grow along the ray (finer near its start, where a miss shows most). The start leaves the surface a little so it does not
	// find itself.
	float maxDist = TESR_WetnessReflectionData.y;
	float steps = TESR_WetnessReflectionData.w;
	float3 start = P + N * (0.5f + P.z * 0.002f);
	float thickness = TESR_WetnessReflectionData.z;

	float prevT = 0.0f;
	float hitT = -1.0f;
	float2 hitUV = 0.0f;
	bool onScreen;
	[loop] for (float i = 1.0f; i <= steps; i += 1.0f) {
		float s = i / steps;
		float t = maxDist * s * s;
		float3 q = start + R * t;
		float2 quv = RayUV(q, onScreen);
		if (!onScreen) break;
		float sceneZ = tex2Dlod(TESR_DepthBuffer, float4(quv, 0.0f, 0.0f)).x * farZ;
		float delta = q.z - sceneZ;
		// Behind what is on screen, but not so far behind it that the ray passed beneath it.
		if (delta > 0.0f && delta < thickness + (t - prevT)) {
			// Refined between the last step in front and this one.
			float lo = prevT;
			float hi = t;
			[unroll] for (int k = 0; k < SSR_REFINE; k++) {
				float mid = (lo + hi) * 0.5f;
				float3 m = start + R * mid;
				float2 muv = RayUV(m, onScreen);
				float mz = tex2Dlod(TESR_DepthBuffer, float4(muv, 0.0f, 0.0f)).x * farZ;
				if (m.z > mz) hi = mid; else lo = mid;
			}
			hitUV = RayUV(start + R * hi, onScreen);
			hitT = hi;
			break;
		}
		prevT = t;
	}
	if (hitT < 0.0f) return Result(scene, gloss, 0.0f);
	// A hit on the sky (nothing there) is the sky the forward shaders already reflected.
	if (tex2Dlod(TESR_DepthBuffer, float4(hitUV, 0.0f, 0.0f)).x >= 0.999f) return Result(scene, gloss, 0.0f);

	// Confidence: away from the screen's edges, over the ray's length, and the ray's facing.
	float2 hitS = rectToScreen(hitUV);
	float2 edge = min(hitS, 1.0f - hitS) / max(TESR_WetnessReflectionFade.y, 1e-4f);
	float confidence = saturate(min(edge.x, edge.y)) * (1.0f - smoothstep(maxDist * 0.6f, maxDist, hitT)) * towardScreen;

	// The film's reflectance as the forward shaders weigh their sky reflection, and that sky.
	float3 worldV = -normalize(worldPos);
	float3 worldR = reflect(-worldV, worldN);
	float horizon = saturate(1.0f + 1.2f * dot(worldR, worldN));
	float reflectance = WetReflectance(saturate(dot(worldN, worldV)), rough, glossy) * horizon * horizon;
	float3 skySeen = LinEncode(max(WetSkyReflection(worldR), 0.0f));

	float3 hitColor = tex2Dlod(TESR_RenderedBuffer, float4(hitUV, 0.0f, 0.0f)).rgb;
	float weight = reflectance * confidence * TESR_WetnessReflectionData.x;

	if (DEBUG_VIEW) return Result(scene, gloss, hitColor * confidence);

	scene.rgb = max(scene.rgb + (hitColor - skySeen) * weight, 0.0f);
	return scene;
}

technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 WetReflections();
	}
}
