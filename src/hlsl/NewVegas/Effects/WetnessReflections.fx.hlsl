// UNOFFICIAL 2026-11-04 Wetness reflections, ported from Physically-Based-Reloaded (Balebandro, Effects/WetnessReflections.fx.hlsl):
// screen-space reflections on what Wetness Effects made wet (effects/WetnessEffects/WetnessReflections.h).
//
// The object and terrain shaders give the water film and the puddles a reflection of the sky; they cannot see the rest of the scene.
// This pass, run once the world is drawn, marches each wet pixel's reflected ray through the depth buffer and, where the ray meets
// something on screen, puts that in place of the sky the forward shaders reflected.
//
// Where it is wet, and how glossy, is worked out again here with the forward shaders' own function (Shaders/Includes/Wetness.hlsl,
// GetWetness) from the depth and the normals buffer: the same weather, puddle noise and cover. What only the forward shaders know
// (normal maps, ripples) is left out; the reflection is of the flat water surface. (2026-11-18: with [Main.Main.Quality]
// DeferredBumpedNormals on and the deferred renderer running, the reflection takes the G-buffer's normal-mapped normal; off by default.)
//
// The scene holds the forward shaders' output, so the swap is done in its terms: the sky's share comes out (its radiance as the
// forward shaders add it, WetSkyReflection, encoded as they encode their result, LinEncode), the hit's goes in, both scaled by the
// film's reflectance there.
// VegasShaders: the depth buffer keeps the linear depth only (the projected one is worked out from it, Includes/Depth.hlsl), and
// the texture coordinates are RenderScale's rect ones (Includes/RenderRect.hlsl).

float4 TESR_ReciprocalResolution;
float4 TESR_WetnessReflectionData;   // x: strength, y: max ray distance (game units), z: thickness, w: steps
float4 TESR_WetnessReflectionFade;   // x: roughness cutoff, y: screen edge fade (uv), z: debug view, w: camera distance fade end
float4 TESR_WetnessReflectionBump;   // UNOFFICIAL 2026-11-18 DeferredBumpedNormals: x 1 = s4 holds the shading normals this frame (else 0: as before)
float4 TESR_WetnessLampFilm;         // UNOFFICIAL 2026-11-19 DeferredWetLampFilm: x 1 = on this frame, y the froxel lists' texels, z the lamp table's rows, w its first tile row
float4 TESR_LinearScales;            // x the lamps' light scale (the objects' forward film takes it, the land's does not: Object.hlsl LIN_SCALE_LIGHT)

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-11-18 DeferredBumpedNormals: no texture slot of its own; WetnessReflectionsEffect::BindShadingNormals sets it every frame
// (the shading normals, or TESR_NormalsBuffer while the switch is off). s3 is the wetness include's ortho / puddle map.
sampler2D NVR_WetShadingNormals : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-11-19 [Main.Main.Quality] DeferredWetLampFilm (WetnessReflectionsEffect::BindShadingNormals sets all three, no texture
// slots; stages past the TESR_ samplers, which EffectRecord binds by declaration order, s0-s3): the frame's lamp table (core/ManyLamps.h, the
// froxel lists the deferred lamp pass reads), the G-buffer's material (TESR_MaterialBuffer: the shading model) and its albedo
// (TESR_AlbedoBuffer: its alpha says which pixels a deferred record drew, and their fog keep).
sampler2D NVR_WetLampTable : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_WetMaterial : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D NVR_WetAlbedo : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

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

// UNOFFICIAL 2026-11-19 DeferredWetLampFilm: the lamps' highlight on the wet film, stretched toward the eye (the wet-road streak), for the
// pixels a deferred record drew. The forward land and object records draw it in their lamp loops (TerrainTemplate.hlsl / ObjectTemplate.hlsl
// WetLampFilm); the deferred records have every lamp compiled out and the deferred lamp pass has no wet film, so under the deferred renderer
// a fire or a street lamp left no streak on wet ground. Here, every lamp of the pixel's froxel list (the table read as
// Shaders/Includes/ForwardPlus.hlsl FPLampSum reads it: keep the two in step), each through Wetness.hlsl's own WetLampFilm with the forward
// records' inputs (the film's normal, roughness and gloss from GetWetness above, the murk tint, the stretch). Unshadowed, as the forward
// land's film without RT lamp shadows. The falloff is the table walk's 1 - (d / r)^2.
#define WL_TEXELS 2048.0f
float4 WLTexel(float col, float row) { return tex2Dlod(NVR_WetLampTable, float4((col + 0.5f) / WL_TEXELS, (row + 0.5f) / TESR_WetnessLampFilm.z, 0.0f, 0.0f)); }
float WLTileRow() { return TESR_WetnessLampFilm.w; }	// (core/ManyLamps.h TileRow)
float4 WLHeader(float k) { return WLTexel(WL_TEXELS - 1.0f - k, WLTileRow() + 1.0f); }	// (core/ForwardPlusHeader.h PutHeader)
float4 WLList(float t) { const float row = floor(t / WL_TEXELS); return WLTexel(t - row * WL_TEXELS, row + 1.0f); }

// p: the pixel, camera-relative world; n: the film's normal (world, unit); V: toward the eye (world, unit).
float3 WetLampFilmSum(float3 p, float3 n, float3 V, float roughness, float stretch)
{
	const float4 cam = WLHeader(0.0f);
	[branch] if (!(cam.w > 0.5f)) return 0.0f;	// no froxel lists for this frame's world view
	const float4 fwd = WLHeader(1.0f), right = WLHeader(2.0f), up = WLHeader(3.0f), grid = WLHeader(4.0f);
	const float viewZ = dot(p, fwd.xyz);
	[branch] if (!(viewZ > 0.0f)) return 0.0f;
	const float2 local = float2(0.5f + 0.5f * dot(p, right.xyz) / (viewZ * fwd.w), 0.5f - 0.5f * dot(p, up.xyz) / (viewZ * right.w));
	const float2 tile = min(floor(saturate(local) * grid.xy), grid.zw - 1.0f);
	const float slice = viewZ < 64.0f ? 0.0f : min(7.0f, floor(log2(viewZ)) - 5.0f);
	const float base = (slice * grid.z * grid.w + tile.y * grid.z + tile.x) * TESR_WetnessLampFilm.y;
	const int lamps = (int)min(WLList(base).x, TESR_WetnessLampFilm.y * 4.0f - 1.0f);
	const float3 q = cam.xyz + p;
	float3 sum = 0.0f;
	[loop] for (int k = 0; k < lamps; k++) {
		const float e = (float)k + 1.0f;
		const float t = floor((e + 0.5f) / 4.0f);
		const float c = e - t * 4.0f;
		const float i = dot(WLList(base + t), float4(c < 0.5f, abs(c - 1.0f) < 0.5f, abs(c - 2.0f) < 0.5f, c > 2.5f));
		const float4 lamp = WLTexel(i * 2.0f, 0.0f);
		const float3 toLamp = lamp.xyz - q;
		const float att = lamp.w > 0.0f ? 1.0f - saturate(dot(toLamp, toLamp) / (lamp.w * lamp.w)) : 0.0f;
		[branch] if (att > 0.0f) sum += att * WetLampFilm(n, V, normalize(toLamp), WLTexel(i * 2.0f + 1.0f, 0.0f).rgb, roughness, stretch);
	}
	return sum;
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
	// UNOFFICIAL 2026-11-18 [Main.Main.Quality] DeferredBumpedNormals (docs/bumped-normals-study.md): the G-buffer's normal-mapped shading normal
	// (s4) while the switch is on, else the normals buffer itself. N (smooth) still decides what is wet and where the ray starts.
	float3 Ns = normalize(tex2Dlod(NVR_WetShadingNormals, float4(uv, 0.0f, 0.0f)).xyz * 2.0f - 1.0f);
	if (dot(Ns, V) > 0.0f) Ns = -Ns;

	float3 worldPos = mul(float4(P, 1.0f), TESR_InvViewTransform).xyz;
	float3 worldN = normalize(mul(float4(N, 0.0f), TESR_InvViewTransform).xyz);
	float3 worldNs = normalize(mul(float4(Ns, 0.0f), TESR_InvViewTransform).xyz);
	Wetness wet = GetWetness(worldPos, worldN, worldNs);
	float gloss = wet.gloss;
	float rough = wet.roughness;
	// (2026-11-19 DeferredWetLampFilm, above) the lamps on the wet film where a deferred record drew the pixel (albedo alpha above 1; forward
	// records write 1 and drew their own streak) and only the land and objects (models 1-4: the shaders whose forward records have the film),
	// added in linear light as the forward records add it, fogged as the deferred passes fog their light (Includes/DeferredPixel.hlsl
	// FDAddedLightKeep: the keep is the alpha's fraction above its code 1, 3 or 5)
	[branch] if (TESR_WetnessLampFilm.x > 0.5f && gloss > 0.05f) {
		const float a = tex2Dlod(NVR_WetAlbedo, float4(uv, 0.0f, 0.0f)).a;
		const float r = tex2Dlod(NVR_WetMaterial, float4(uv, 0.0f, 0.0f)).r;
		float model = floor(r + 0.01f);
		model -= model >= 16.0f ? 16.0f : 0.0f;
		[branch] if (a > 1.01f && model > 0.5f && model < 4.5f) {
			const float keep = saturate(a > 4.5f ? a - 5.0f : a > 2.5f ? a - 3.0f : a - 1.0f);
			float3 film = WetLampFilmSum(worldPos, normalize(wet.normal), -normalize(worldPos), rough, lerp(1.0f, 0.3f, gloss))
				* (smoothstep(0.0f, 0.2f, gloss) * keep * (model < 3.5f ? TESR_LinearScales.x : 1.0f));
		#ifdef PUDDLES_COMPILED
			film *= PuddleMurkTint(wet.murk);
		#endif
			if (any(film > 0.0f)) scene.rgb = LinEncode(LinColor(scene.rgb) + film);
		}
	}
	float glossy = gloss * (1.0f - smoothstep(TESR_WetnessReflectionFade.x * 0.5f, TESR_WetnessReflectionFade.x, rough));
	if (glossy <= 0.01f) return Result(scene, gloss, 0.0f);

	// The normal the film reflects with. Off: N, exactly as before. On: the film's own normal as the forward shaders reflect with it
	// (wet.normal: the bumped normal, flattened toward the smooth one under standing water and streaks, with the ripples on top), so a
	// puddle does not mirror the ground's bumps beneath it. View space: the transpose of the view-to-world rotation.
	const bool bumped = TESR_WetnessReflectionBump.x > 0.5f;
	float3 worldNr = worldN;
	float3 Nr = N;
	if (bumped) {
		worldNr = normalize(wet.normal);
		Nr = normalize(mul((float3x3)TESR_InvViewTransform, worldNr));
		if (dot(Nr, V) > 0.0f) { Nr = N; worldNr = worldN; }	// facing away from the camera: the smooth normal
	}

	// The reflected ray, in view space. One pointing back at the camera has nothing on screen to find.
	float3 R = reflect(V, Nr);
	// (bumped) a ray tilted into the surface would find the surface next to its start: mirrored about the smooth plane
	if (bumped && dot(R, N) < 0.0f) R -= 2.0f * dot(R, N) * N;
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
	// (2026-11-18 DeferredBumpedNormals) the sky taken out along the same normal the hit comes along (worldNr: worldN while off), the
	// horizon against the smooth surface, as the forward film (ObjectWetFilm) does
	float3 worldR = reflect(-worldV, worldNr);
	float horizon = saturate(1.0f + 1.2f * dot(worldR, worldN));
	float reflectance = WetReflectance(saturate(dot(worldNr, worldV)), rough, glossy) * horizon * horizon;
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
