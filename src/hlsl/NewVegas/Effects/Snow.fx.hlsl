// Snow fullscreen shader for Oblivion Reloaded
// Technique 0 (this part, down to its technique block): the camera-centred layers, the default. Techniques 1 and 2 (the second
// part of the file): macintroll's world-space snow, [Shaders.Snow.Main] WorldSpace. effects/Snow/Snow.cpp picks the technique.
#define SnowLayers 8

float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformOrtho;
float4 TESR_GameTime;
float4 TESR_SnowData; // x: amount of snow; z: snow falling speed
float4 TESR_SunColor;
float4 TESR_SunAmbient;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_OrthoMapBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

static const float PI = 3.14159265;
static const float timetick = TESR_GameTime.z;
static const float hscale = 0.1f;
static const float3x3 p = float3x3(30.323122,30.323122,30.323122,30.323122,30.323122,30.323122,30.323122,30.323122,30.323122);
static const float DEPTH = 50; // distance between each snow layer
static const float SPEED = TESR_SnowData.z;
static const float angle = 1.0;


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

#include "Includes/Depth.hlsl"
#include "Includes/Helpers.hlsl"

float2 cylindrical(float3 world)
{
	float u = -atan2(world.y, world.x) / PI;
	float v = world.z / length(world.xy);
	return float2(0.5f * u + 0.5f, hscale * v);
}


// checks wether a point is underneath something occluding
float GetOrtho(float4 OrthoPos) {	
	OrthoPos.xyz /= OrthoPos.w;
    float outofbounds = (OrthoPos.x < -1.0f || OrthoPos.x > 1.0f ||
        OrthoPos.y < -1.0f || OrthoPos.y > 1.0f ||
        OrthoPos.z <  0.0f || OrthoPos.z > 1.0f);
 
    OrthoPos.x = OrthoPos.x *  0.5f + 0.5f;
    OrthoPos.y = OrthoPos.y * -0.5f + 0.5f;
	float Ortho = tex2D(TESR_OrthoMapBuffer, OrthoPos.xy).r;
	return (Ortho > OrthoPos.z - 0.0001) || outofbounds; // always return true if out of bounds 
}


float4 Snow( VSOUT IN ) : COLOR0
{
	float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
    color.rgb = pows(color.rgb, 2.2); // linearise

	// calculating the ray along which the volumetric snow will be calculated
	float3 rayStart = toWorld(IN.UVCoord);
    float3 rayEnd = rayStart * DEPTH * SnowLayers;
    float4 orthoStart = mul(float4(rayStart, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
    float4 orthoEnd = mul(float4(rayEnd, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	float4 step = (orthoEnd - orthoStart) / SnowLayers;
	
	// each iteration adds a cylindrical layer of drops
    float depth = readDepth(IN.UVCoord);
	float2 uv = cylindrical(rayStart.xyz); // converts world coordinates to cylinder coordinates around the player

	float totalSnow = 0.0f;

	[unroll]
	for (int i = 1; i < SnowLayers; i++){
		float2 noiseSurfacePosition = uv * (1.0f + i * DEPTH); // scale cylinder coordinates with iterations
		float wind = sin(timetick * 0.5 + i) * angle; // horizontal offset animated over time 
		noiseSurfacePosition += float2( wind * noiseSurfacePosition.y * (fmod(i * 107.238917f, 1.0f) - 0.5f), SPEED * timetick); // animate y offset, not sure what x does?

		// creating the noise with magic
		float3 m = float3(floor(noiseSurfacePosition), i); // places the drops at the same distance from the camera?
		float3 mp = m/frac(mul(p, m)); // bayer matrix? grid of grey values stretch depending on the world positions
		float3 snowNoise = frac(mp); //r.x is a random generated grid of different darknesses

        // determin flake shape
		float2 shape = abs(fmod(noiseSurfacePosition, 1.0f) + snowNoise.xy - 1.0f);
		shape += 0.01f * abs(2.0f * frac(10.0f * noiseSurfacePosition.yx) - 1.0f); // shape is a horizontal gradient on each square 
		float dropGradient = 0.6f * max(shape.x - shape.y, shape.x + shape.y) + max(shape.x, shape.y) - 0.01f; // creates a circular gradient on each square of the noise grid to focus the drop
		float edge = 0.005f + 0.05f * min(1.0f * i, 4.0f * TESR_SnowData.x); // can't be 0 or smoothstep will fail. the smaller this value, the smaller the drops

		// add new snowflake influence to the fragment, by extracting the brightest spots in the gradients grid using the edge parameter
		// float flake = smoothstep(edge, -edge, dropGradient) * (snowNoise.x / (1.0f + 0.2f * i)); // fade opacity with each iteration
		float flake = smoothstep(edge, -edge, dropGradient) * snowNoise.x; 
		flake *= GetOrtho(orthoStart + step * i) * (depth > DEPTH * i); // depth and ortho check for snow layer
		totalSnow += flake;
	}
	totalSnow = saturate(totalSnow);

	// a rain tint color that scales with the sun direction
	//float4 rainColor = lerp(TESR_SunColor, TESR_SkyColor * 0.5, pow(shades(normalize(world), TESR_SunDirection.xyz), 2));
	float4 snowColor = max(float4(pows(TESR_SunAmbient.rgb,2.2),TESR_SunAmbient.a), float4(pows(TESR_SunColor.rgb,2.2),TESR_SunColor.a));

	color = lerp(color, snowColor, totalSnow);
    color.rgb = pows(color.rgb, 1.0/2.2); // delinearise

	return color;
}



technique
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Snow();
	}
}

// =====================================================================================================================
// UNOFFICIAL 2026-11-09 World-space snow ([Shaders.Snow.Main] WorldSpace, off by default): techniques 1 and 2 below.
// Contributed by macintroll (Snow.fx.hlsl "DX9 world-space snowfall test", received 2026-11-09). His notes:
//   Procedural falling positions: no stored particle history or landing state.
//   The ray traverses a moving world grid, not camera-centred depth layers.
// Changes made here (effects/Snow/Snow.cpp feeds them): the look values a player would change are settings read from
// TESR_SnowWorldData / TESR_SnowWorldWind instead of compile-time constants; the surface marks are their own technique
// (so switching them off skips the pass); the pixel function is SnowWorld (Snow is the old one above). The old technique
// above is untouched and stays technique 0. Runs as a D3D9 pass on the DXVK fork too (no Compute/ twin yet).
// The flake atlas (4x4 tiles, 512x512): Textures\Precipitations\fx_weather_snow01.dds.
// =====================================================================================================================
float4 TESR_SnowWorldData;   // x flake radius (game units), y density 0-1, z max distance (game units), w opacity
float4 TESR_SnowWorldWind;   // x cloud wind strength, y cloud repeat cells (max(1, floor(8192 * x + 0.5))), z plant wind strength (0 off)
float4 TESR_WaterSettings;
float4 TESR_CloudsWind; // xy = accumulated cloud-space offset, not velocity.
// Registered globals: no object-shader register bindings in this effect.
float4 TESR_PlantLeafWind;
float4 TESR_PlantLeafWind2;
float4 TESR_PlantLeafWind3;

sampler2D TESR_SnowFlakeBuffer : register(s3) < string ResourceName = "Precipitations\fx_weather_snow01.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

// Extra buffers for the surface-hit pass. Existing snowfall slots are unchanged.
sampler2D TESR_NormalsBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// WetWorld-style projected surface marks, independent of falling flake simulation.
static const bool SH_Enabled = true;            // (off = technique 1: the pass is not drawn)
static const float SH_CellSize = 12.0f;        // Smaller = more possible landing sites.
static const float SH_Radius = 1.6f;           // Atlas mark half-size, in game units.
static const float SH_Density = 0.35f;         // Chance per cell/event at full snowfall.
static const float SH_Lifetime = 4.0f;         // Seconds visible, including fading.
static const float SH_Pause = 2.0f;            // Inactive seconds between events.
static const float SH_Opacity = 0.55f;
static const float SH_FadeStart = 650.0f;
static const float SH_MaxDistance = 1000.0f;
static const float SH_MinUp = 0.70f;          // Avoid steep/vertical surfaces.
static const float SH_FullUp = 0.92f;
static const float SH_RoofLift = 1.0f;         // Lift shelter test above the surface.
// 0 normal; 1 eligible surfaces; 2 mark mask; 3 circular marks bypassing atlas.
static const int SH_Debug = 0;

// Main controls (distances and sizes are in game units).
static const float WS_CellSize = 64.0f;       // Smaller = more flakes AND more work.
static const float WS_MaxDistance = TESR_SnowWorldData.z; // setting WorldDistance (1200); at most (WS_MaxCells - 3) * WS_CellSize / 1.733
static const int   WS_MaxCells = 40;          // >= ceil(1.733 * distance / cell size) + 3.
static const float WS_FlakeRadius = TESR_SnowWorldData.x; // setting WorldFlakeSize (1.8). Billboard half-size before atlas padding.
static const float WS_FallSpeed = 70.0f;      // Multiplied by the Snow Speed slider.
static const float WS_DriftAmplitude = 7.0f;
static const bool  WS_UseCloudWind = true;
static const float WS_CloudWindStrength = TESR_SnowWorldWind.x; // setting WorldCloudWind (0.7). 0 off; 0.5 gentle; 1 normal; 2 strong.
// Repeat size and offset scale are paired to absorb the cloud's 900000-unit wrap.
static const float WS_CloudRepeatCells = TESR_SnowWorldWind.y; // max(1, floor(8192 * strength + 0.5)), worked out in Snow.cpp
static const bool  WS_UsePlantWind = true;      // (the setting below at 0 is off)
static const float WS_PlantWindStrength = TESR_SnowWorldWind.z; // setting WorldPlantWind (0 = off, his test value 1.5). 0 restores the original independent wobble.
static const float WS_PlantWindFlutter = 1.0f;  // Multiplier for vegetation flutter.
static const float WS_Opacity = TESR_SnowWorldData.w; // setting WorldOpacity (0.8)
static const float WS_SpinSpeed = 0.65f;
static const float WS_Density = TESR_SnowWorldData.y; // setting WorldDensity (1). 0..1 occupied cells; no extra ray steps.
static const float WS_NearFadeStart = 12.0f;
static const float WS_NearFadeEnd = 52.0f;
static const float WS_SurfaceFade = 6.0f;
static const float SNOW_DENSITY_FEATHER = 0.08f;
static const float SNOW_OPACITY_RAMP_END = 0.18f;
// Same 4x4 atlas and artwork crop. Set atlas size if your DDS is not 512x512.
static const float WS_AtlasSize = 512.0f;
static const float SPRITE_FILL = 0.72f;
static const float SPRITE_DETAIL = 0.5f;
// Debug: 0 normal, 1 monochrome flake mask, 2 solid circular flakes (atlas bypass).
static const int WS_Debug = 0;

// Snow lighting
static const float3 SNOW_TINT_DAY        = float3(0.80f, 0.80f, 0.88f);
static const float3 SNOW_TINT_NIGHT      = float3(0.76f, 0.80f, 0.88f);
static const float  SNOW_BRIGHTNESS_NIGHT = 0.42f;
static const float  SNOW_BRIGHTNESS_DAY   = 0.80f;
static const float  SNOW_LIGHT_TINT_AMOUNT = 0.25f;

#include "Includes/Normals.hlsl"

float3 WS_Hash(float3 cell) {
    if (WS_UseCloudWind && WS_CloudWindStrength > 0.0f)
        cell.xy -= floor(cell.xy / WS_CloudRepeatCells) * WS_CloudRepeatCells;
    float3 p = frac(cell * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Same travelling gust pattern as NVR's PlantLeafGust, without vertex registers.
float WS_PlantGust(float2 position) {
    float2 k = TESR_PlantLeafWind.xy;
    float phase = TESR_PlantLeafWind.z * 0.25f;
    float along = dot(position, k);
    float front = 0.5f + 0.5f * sin(phase - along);
    float patch = 0.75f + 0.25f * sin(phase - 0.5f *
                  (along + position.y * k.x - position.x * k.y));
    return front * front * patch;
}

float WS_Exposed(float3 cameraRelativePosition) {
    float4 q = mul(float4(cameraRelativePosition, 1.0f),
                   TESR_ShadowCameraToLightTransformOrtho);
    q.xyz /= q.w;
    if (any(abs(q.xy) > 1.0f) || q.z < 0.0f || q.z > 1.0f) return 1.0f;
    float2 uv = q.xy * float2(0.5f, -0.5f) + 0.5f;
    // Explicit LOD is required inside the varying ray-traversal loop.
    return step(q.z - 0.0001f, tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0, 0)).r);
}

float4 SnowWorld(VSOUT IN) : COLOR0 {
    float4 color = tex2D(TESR_RenderedBuffer, IN.UVCoord);
    if (SH_Debug == 1 || SH_Debug == 2) return color;
    if (outsideRect(IN.UVCoord)) return color;
    float intensity = saturate(TESR_SnowData.x);
    if (intensity <= 0.0f) return color;

    float3 ray = normalize(toWorld(IN.UVCoord));
    float3 forward = normalize(float3(TESR_ViewTransform[0][2],
                                       TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]));
    float3 right = normalize(float3(TESR_ViewTransform[0][0],
                                     TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]));
    float3 up = normalize(float3(TESR_ViewTransform[0][1],
                                  TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]));
    float rayForward = max(dot(ray, forward), 0.0001f);
    float sceneDepth = readDepth(IN.UVCoord);
    float maxTravel = min(WS_MaxDistance, sceneDepth / rayForward);
    // Derivatives are taken before divergent flow. Used only for atlas LOD.
    float rayPixelWidth = max(length(ddx(ray)), length(ddy(ray)));
    float time = TESR_GameTime.z;
    float3 scroll = float3(0, 0, WS_FallSpeed * max(TESR_SnowData.z, 0.0f) * time);
    float cellSize = max(WS_CellSize, 16.0f);
    // Clouds evaluate density at world + offset. Use the same sign here.
    // A complete cloud wrap becomes an integer number of repeating grid cells.
    if (WS_UseCloudWind && WS_CloudWindStrength > 0.0f)
        scroll.xy = TESR_CloudsWind.yx * (cellSize * WS_CloudRepeatCells / 900000.0f);
    float3 origin = TESR_CameraPosition.xyz + scroll;
    float entry = WS_NearFadeStart;
    float3 cell = floor((origin + ray * entry) / cellSize);
    float3 direction = float3(ray.x >= 0 ? 1 : -1, ray.y >= 0 ? 1 : -1, ray.z >= 0 ? 1 : -1);
    float3 invRay = direction / max(abs(ray), 0.000001f);
    float3 boundary = (cell + step(0.0f, ray)) * cellSize;
    float3 nextCrossing = (boundary - origin) * invRay;
    float3 crossingStep = cellSize / max(abs(ray), 0.000001f);
    float windDirLength = length(TESR_PlantLeafWind3.xy);
    float2 windDir = TESR_PlantLeafWind3.xy / max(windDirLength, 0.0001f);
    float2 windSide = float2(-windDir.y, windDir.x);
    bool plantWindActive = WS_UsePlantWind && WS_PlantWindStrength > 0.0f
                        && TESR_PlantLeafWind.w > 0.5f && windDirLength > 0.0001f;
    float totalSnow = 0.0f;
    float opacityRamp = smoothstep(0.0f, SNOW_OPACITY_RAMP_END, intensity);

    [loop]
    for (int iteration = 0; iteration < WS_MaxCells; ++iteration) {
        if (entry >= maxTravel || totalSnow > 0.995f) break;
        float exitDistance = min(nextCrossing.x, min(nextCrossing.y, nextCrossing.z));
        float3 seed = WS_Hash(cell);
        // UNOFFICIAL 2026-11-09 (cost): the flake's centre is its resting place plus a wobble of at most 1.47 x drift, and it is drawn
        // only where the ray passes within its radius (at most 4.5 % of the cell) of that centre. A ray further than that from the
        // resting place cannot hit it, so the rest of the cell's work (second hash, wobble, wind, sprite) is skipped. Same picture.
        float3 restCenter = (cell + 0.20f + seed * 0.60f) * cellSize - origin;
        float3 restOffset = restCenter - ray * dot(restCenter, ray);
        float flakeReach = cellSize * 0.045f + 1.5f * min(WS_DriftAmplitude, cellSize * 0.10f);
        [branch] if (dot(restOffset, restOffset) < flakeReach * flakeReach) {
        float3 variety = WS_Hash(cell + float3(19.0f, 73.0f, 131.0f));
        float density = saturate((intensity * (1.0f + SNOW_DENSITY_FEATHER)
                               - variety.z) / SNOW_DENSITY_FEATHER);
        density *= step(variety.y, saturate(WS_Density));
        // Bounded wobble keeps each billboard fully inside its moving cell.
        float drift = min(WS_DriftAmplitude, cellSize * 0.10f);
        float3 wobble = float3(
            sin(time * 1.3f + seed.z * 40.0f),
            cos(time * 0.9f + seed.z * 17.0f),
            sin(time * 0.7f + seed.x * 25.0f) * 0.4f) * drift;
        [branch] if (plantWindActive) {
            // Actual world XY, including continuous cloud-driven horizontal movement.
            float2 positionXY = (cell.xy + 0.20f + seed.xy * 0.60f) * cellSize - scroll.xy;
            float gust = WS_PlantGust(positionXY);
            float flutterPhase = 2.0f * TESR_PlantLeafWind.z + seed.z * 6.2831853f;
            float flutter = max(TESR_PlantLeafWind2.x, 0.0f) * max(WS_PlantWindFlutter, 0.0f);
            float along = WS_PlantWindStrength *
                (max(TESR_PlantLeafWind2.w, 0.0f) * gust + flutter * sin(flutterPhase));
            float across = WS_PlantWindStrength * flutter * 0.35f * cos(flutterPhase);
            // Compress displacement, not time: weather changes cannot produce
            // a time-amplified jump. This is sway, not integrated wind velocity.
            along /= 1.0f + abs(along);
            across /= 1.0f + abs(across);
            float2 windWobble = wobble.xy * 0.25f + drift *
                               (windDir * along + windSide * across);
            windWobble *= min(1.0f, drift / max(length(windWobble), 0.0001f));
            wobble.xy = lerp(wobble.xy, windWobble, saturate(WS_PlantWindStrength));
        }
        float3 relativeCenter = (cell + 0.20f + seed * 0.60f) * cellSize + wobble - origin;
        float centerDepth = dot(relativeCenter, forward);
        float travel = centerDepth / rayForward;
        float radius = min(WS_FlakeRadius * lerp(0.65f, 1.35f, variety.x), cellSize * 0.045f);
        float3 delta = ray * travel - relativeCenter;
        float2 local = float2(dot(delta, right), dot(delta, up)) / max(radius, 0.01f);

        [branch]
        if (density > 0.0f && travel >= entry && travel < exitDistance &&
            travel < maxTravel && dot(local, local) < 1.0f) {
            float angle = variety.z * 6.2831853f + time * WS_SpinSpeed * (seed.y * 2.0f - 1.0f);
            float s, c;
            sincos(angle, s, c);
            float2 rotated = float2(c * local.x - s * local.y, s * local.x + c * local.y);
            float2 tile = floor(float2(seed.x, variety.x) * 4.0f);
            float2 tileUV = 0.5f + rotated * (0.5f * SPRITE_FILL);
            float2 atlasUV = (tile + tileUV) * 0.25f;
            float pixelsAcross = 2.0f * radius / max(travel * rayPixelWidth, 0.0001f);
            // Limit mip level so neighbouring atlas tiles do not bleed together.
            float lod = clamp(log2(max(WS_AtlasSize * 0.25f * SPRITE_FILL / max(pixelsAcross, 1.0f), 1.0f)), 0.0f, 3.0f);
            float4 sprite = tex2Dlod(TESR_SnowFlakeBuffer, float4(atlasUV, 0, lod));
            float detail = dot(sprite.rgb, float3(0.333f, 0.333f, 0.334f));
            float mask = sprite.a * lerp(1.0f, detail, SPRITE_DETAIL);
            // Smooth outer edge retains atlas detail but prevents a hard quad outline.
            mask *= 1.0f - smoothstep(0.80f, 1.0f, length(local));
            if (WS_Debug == 2) mask = 1.0f - smoothstep(0.35f, 1.0f, length(local));
            float fade = smoothstep(WS_NearFadeStart, WS_NearFadeEnd, travel);
            fade *= 1.0f - smoothstep(WS_MaxDistance * 0.80f, WS_MaxDistance, travel);
            fade *= smoothstep(0.0f, WS_SurfaceFade, sceneDepth - centerDepth);
            fade *= WS_Exposed(relativeCenter);
            float alpha = saturate(mask * density * opacityRamp * fade * WS_Opacity);
            totalSnow += (1.0f - totalSnow) * alpha;
        }
        }   // (the reach test above)
        // Advance every tied axis together (corners/edges cannot stall the loop).
        float3 advance = step(nextCrossing, exitDistance.xxx + 0.0001f);
        cell += direction * advance;
        nextCrossing += crossingStep * advance;
        entry = exitDistance;
    }
    if (WS_Debug == 1) return float4(totalSnow.xxx, 1.0f);
    color.rgb = pows(color.rgb, 2.2f);
    // Snow lighting
    float3 ambLin    = pows(TESR_SunAmbient.rgb, 2.2f);
    float3 sunLin    = pows(TESR_SunColor.rgb,   2.2f);
    float3 lightMix  = ambLin * 0.75f + sunLin * 0.25f;
    float  lightLuma = dot(lightMix, float3(0.2126f, 0.7152f, 0.0722f));
    float  dayFactor = smoothstep(0.015f, 0.30f, lightLuma);
    float3 snowTint  = lerp(SNOW_TINT_NIGHT, SNOW_TINT_DAY,        dayFactor);
    float  snowBri   = lerp(SNOW_BRIGHTNESS_NIGHT, SNOW_BRIGHTNESS_DAY, dayFactor);
    float3 lightTint = saturate(lightMix / max(lightLuma, 0.0001f));
    float3 finalSnowRgb = lerp(snowTint * snowBri,
                               snowTint * snowBri * lightTint,
                               SNOW_LIGHT_TINT_AMOUNT);


    color.rgb = lerp(color.rgb, finalSnowRgb, saturate(totalSnow));
    color.rgb = pows(color.rgb, 1.0f / 2.2f);
    return color;
}

// Surface hits: same world-XY projection principle as WetWorld's SampleSplash.
// Stable while an event is visible; random site/tile changes happen while hidden.
// No expanding ring, texture animation, refraction, or wind applied after landing.
float3 SH_Hash(float3 cell) {
    float3 p = frac(cell * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float SH_Roof(float3 relativePosition) {
    float4 q = mul(float4(relativePosition + float3(0, 0, SH_RoofLift), 1),
                   TESR_ShadowCameraToLightTransformOrtho);
    q.xyz /= q.w;
    // WetWorld treats positions beyond the shelter map as ineligible.
    if (any(abs(q.xy) > 1.0f) || q.z < 0.0f || q.z > 1.0f) return 0.0f;
    float2 uv = q.xy * float2(0.5f, -0.5f) + 0.5f;
    return step(q.z - 0.0001f, tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0, 0)).r);
}

float4 SnowSurfaceHits(VSOUT IN) : COLOR0 {
    float4 source = tex2D(TESR_RenderedBuffer, IN.UVCoord);
    if (!SH_Enabled) return source;
    float depth = readDepth(IN.UVCoord);
    float3 relativePosition = toWorld(IN.UVCoord) * depth;
    float3 worldPosition = TESR_CameraPosition.xyz + relativePosition;
    // Compute footprint derivatives before pixel-dependent branching.
    float footprint = max(length(ddx(worldPosition.xy)), length(ddy(worldPosition.xy)));
    float intensity = saturate(TESR_SnowData.x);
    float distanceToSurface = length(relativePosition);
    if (outsideRect(IN.UVCoord) || intensity <= 0.0f ||
        !(depth > 0.0f && depth < farZ * 0.9999f) || distanceToSurface >= SH_MaxDistance)
        return SH_Debug == 1 || SH_Debug == 2 ? float4(0, 0, 0, 1) : source;

    float vmDepth = tex2Dlod(TESR_DepthBufferViewModel, float4(IN.UVCoord, 0, 0)).x;
    bool viewModel = (TESR_DepthConstants.z == 0 && vmDepth < 0.9f) ||
                     (TESR_DepthConstants.z > 0 && vmDepth > 0.01f);
    // First-person hits remain owned by SnowAccumulation.
    if (viewModel || worldPosition.z <= TESR_WaterSettings.x + 0.5f)
        return SH_Debug == 1 || SH_Debug == 2 ? float4(0, 0, 0, 1) : source;

    float3 normal = GetWorldNormalLod(IN.UVCoord);
    normal *= rsqrt(max(dot(normal, normal), 0.000001f));
    float eligible = smoothstep(SH_MinUp, SH_FullUp, normal.z);
    eligible *= 1.0f - smoothstep(SH_FadeStart, SH_MaxDistance, distanceToSurface);
    eligible *= SH_Roof(relativePosition);
    if (SH_Debug == 1) return float4(eligible.xxx, 1);
    if (eligible <= 0.0f) return SH_Debug == 2 ? float4(0, 0, 0, 1) : source;

    float cellSize = max(SH_CellSize, 4.0f);
    float2 cell = floor(worldPosition.xy / cellSize);
    float phaseOffset = SH_Hash(float3(cell, 17.0f)).z;
    float lifetime = max(SH_Lifetime, 0.2f);
    float period = lifetime + max(SH_Pause, 0.1f);
    float phase = TESR_GameTime.z / period + phaseOffset;
    float eventIndex = floor(phase);
    float age = frac(phase) * period;
    float3 seed = SH_Hash(float3(cell, eventIndex));
    float3 variety = SH_Hash(float3(cell + 31.0f, eventIndex + 19.0f));
    float spawn = step(variety.z, saturate(SH_Density) * intensity);
    float fade = smoothstep(0.0f, 0.10f, age) *
                 (1.0f - smoothstep(lifetime * 0.65f, lifetime, age));
    // Random centre stays far enough inside its cell to avoid wrapping/cut edges.
    float2 center = (cell + 0.25f + seed.xy * 0.5f) * cellSize;
    float radius = min(max(SH_Radius, 0.05f) * lerp(0.75f, 1.25f, variety.x), cellSize * 0.20f);
    float2 local = (worldPosition.xy - center) / radius;
    float edge = 1.0f - smoothstep(0.80f, 1.0f, length(local));
    float alpha = 0.0f;
    [branch] if (spawn * fade * edge > 0.0f) {
        float sn, cs;
        sincos(seed.z * 6.2831853f, sn, cs);
        float2 rotated = float2(cs * local.x - sn * local.y, sn * local.x + cs * local.y);
        float2 tile = floor(variety.xy * 4.0f);
        float2 atlasUV = (tile + 0.5f + rotated * 0.5f * SPRITE_FILL) * 0.25f;
        float pixelsAcross = 2.0f * radius / max(footprint, 0.0001f);
        float lod = clamp(log2(max(WS_AtlasSize * 0.25f * SPRITE_FILL / max(pixelsAcross, 1.0f), 1.0f)), 0.0f, 3.0f);
        float4 sprite = tex2Dlod(TESR_SnowFlakeBuffer, float4(atlasUV, 0, lod));
        float detail = dot(sprite.rgb, float3(0.333f, 0.333f, 0.334f));
        float mask = sprite.a * lerp(1.0f, detail, SPRITE_DETAIL) * edge;
        if (SH_Debug == 3) mask = 1.0f - smoothstep(0.35f, 1.0f, length(local));
        // Fade unresolved marks instead of creating distant sparkling pixels.
        float resolved = smoothstep(0.4f, 1.5f, pixelsAcross);
        alpha = saturate(mask * fade * spawn * eligible * SH_Opacity * resolved *
                        smoothstep(0.0f, SNOW_OPACITY_RAMP_END, intensity));
    }
    if (SH_Debug == 2) return float4(alpha.xxx, 1);
    // Approximate local lighting from the already shaded source so tiny marks
    // respond to dark areas. This is not a new terrain/PBR lighting calculation.
    float3 sceneLinear = pows(source.rgb, 2.2f);
    float3 ambient = pows(TESR_SunAmbient.rgb, 2.2f);
    float3 sun = pows(TESR_SunColor.rgb, 2.2f);
    float lightLuma = dot(ambient * 0.75f + sun * 0.25f, float3(0.2126f, 0.7152f, 0.0722f));
    float day = smoothstep(0.015f, 0.30f, lightLuma);
    float3 tint = lerp(SNOW_TINT_NIGHT, SNOW_TINT_DAY, day);
    float brightness = lerp(SNOW_BRIGHTNESS_NIGHT, SNOW_BRIGHTNESS_DAY, day);
    float localLight = clamp(sqrt(max(dot(sceneLinear, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f)) * 2.0f, 0.12f, 1.0f);
    float3 hitColor = tint * brightness * localLight;
    source.rgb = pows(lerp(sceneLinear, hitColor, alpha), 1.0f / 2.2f);
    return source;
}

// Technique 1: the falling flakes alone ([Shaders.Snow.Main] WorldSurfaceMarks off).
technique SnowWorldSpace {
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SnowWorld();
    }
}

// Technique 2: the marks on surfaces, then the falling flakes over them.
technique SnowWorldSpaceMarks {
    pass SurfaceHits {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SnowSurfaceHits();
    }
    pass {
        VertexShader = compile vs_3_0 FrameVS();
        PixelShader = compile ps_3_0 SnowWorld();
    }
}
