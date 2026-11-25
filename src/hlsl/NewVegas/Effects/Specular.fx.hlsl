// Specular (NVR UNOFFICIAL): the sun's highlight and the sky's reflection on every surface outdoors, as a post process.
// The sun's highlight is GGX (Trowbridge-Reitz) with a height-correlated Smith visibility and Schlick's Fresnel on a
// dielectric (F0 0.04: what nearly every non-metal reflects head on); the sky's reflection is the sky's colour along the
// reflected view, weighted by the split-sum environment response for that roughness (Karis' analytic fit). Rain makes
// surfaces smoother and shinier ([Shaders.Specular.Rain], blended in with the rain).
// The normals are NVR's, rebuilt from the depth buffer: flat across each triangle (terrain, rocks) and noisy far away,
// while the game lights the same surfaces with smooth vertex normals. So, at a quarter of the resolution: SpecularPrep
// averages each 4 x 4 block's normals (TESR_SpecularNormals, with the block's depth), SpecularLow averages those over a
// wide patch round each block (taps across a depth edge left out) and works the light out from that (TESR_SpecularBuffer);
// the spread of the normals widens the highlight (specular anti-aliasing). Then the main technique adds it to the
// image, each pixel taking the quarter-resolution pixels round it at its own depth.

float4 TESR_ReciprocalResolution;
float4 TESR_SpecularData;       // x: sun highlight strength, y: roughness (0 mirror .. 1 matte), z: sky reflection strength, w: fade-out distance
float4 TESR_SpecularEffects;    // x: specular anti-aliasing (0 .. 1), y: reflectance at normal incidence (0.04 typical), z: buffer scale
float4 TESR_SunDirection;       // toward the sun
float4 TESR_SunColor;
float4 TESR_SkyColor;
float4 TESR_HorizonColor;
float4 TESR_WaterSettings;      // x: water height
float4 TESR_ShadowFade;         // x: 1 when the sun is down (sunrise/sunset fade)
float4 NVR_BounceData;          // SpecularBounce: the bounce light's TESR_BounceLightData (x: strength, z: draw distance)
float4 NVR_BounceLayout;        // SpecularBounce: zw: 1 / the size of its buffer (BounceLight.fx.hlsl's NVR_BounceLayout)

// In register order: NVR binds TESR_ samplers in declaration order.
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SpecularBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SpecularNormals : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowAtlas : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
// SpecularBounce: the bounce light's low-resolution result (BounceLight.fx.hlsl), bound by SpecularEffect::RenderWithBounce.
// After the TESR_ samplers.
sampler2D NVR_BounceBuffer : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-19, bound by hand for the light step (technique 2; SpecularEffect::BindExtras), with NVR_SpecularExtra:
//   Skylighting v3 phase 1 ([Shaders.AmbientOcclusion.Skylighting] Reflections): the sky visibility (Skylighting v2's b), so the
//   sky's reflection fades where the sky cannot be seen (under roofs and overhangs, in canyons), down to MinReflection, as
//   Community Shaders' Skylighting MixSpecular (GPL-3.0) does with its own visibility;
//   SunFar (SunFar.fx.hlsl): the land's and the clouds' shadow (its alpha), so the sun's highlight goes where they block the sun.
float4 NVR_SpecularExtra;       // x: 1 sky visibility bound, y: MinReflection, z: 1 sun far bound
sampler2D NVR_SpecularSkyVis : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D NVR_SpecularSunFar : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
texture NoTexture;              // never set: unbinds a technique's own render target from its sampler

static const float SunStrength = TESR_SpecularData.x;
static const float Roughness = TESR_SpecularData.y;
static const float SkyStrength = TESR_SpecularData.z;
static const float DrawDistance = TESR_SpecularData.w;
static const float AAStrength = TESR_SpecularEffects.x;
static const float F0 = TESR_SpecularEffects.y;
static const float PI = 3.14159265;
static const float LowScale = TESR_SpecularEffects.z > 0.0 ? TESR_SpecularEffects.z : 4.0;   // TESR_SpecularBuffer: a quarter of the resolution each way (UNOFFICIAL: the full one with Max Quality)

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };
struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };
VSOUT FrameVS(VSIN IN) { VSOUT OUT = (VSOUT)0.0f; OUT.vertPos = IN.vertPos; OUT.UVCoord = IN.UVCoord; return OUT; }

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Shadows.hlsl"
#include "Includes/SunCascades.hlsl"

float3 ViewToWorld(float3 viewNormal) { return mul(TESR_ViewTransform, float4(viewNormal, 1.0)).xyz; }

// The split-sum environment response (scale, bias on F0) for a roughness and N.V: Karis, "Physically Based Shading on
// Mobile" (2014).
float2 EnvBRDFApprox(float roughness, float NdotV) {
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
	return float2(-1.04, 1.04) * a004 + r.zw;
}

// ---- Step 1a, a quarter of the resolution: each 4 x 4 block's normal (view space) and depth ----
// The four bilinear taps one pixel out from the block's centre are the means of its four 2 x 2 quarters; a quarter whose
// depth is off the centre's (another object across the block) is left out.
float4 SpecularPrepPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 px = TESR_ReciprocalResolution.xy;
	float2 uv = (vpos * LowScale + 0.5 * LowScale) * px;          // the centre of the block (a corner between its pixels)
	// The block's depth is its first quarter's (that quarter always counts); far off, where the ground is seen nearly
	// edge on, neighbouring pixels' depths differ by several percent on one surface, so the tolerance is generous. Each
	// quarter's depth is read at its pixel nearest the block's centre (half a pixel in: a pixel's centre, not an edge
	// between two, where a point sample could take either).
	float depth = readDepthLod(uv - 0.5 * px);
	float tolerance = 0.12 * depth + 2.0;
	float3 sum = tex2Dlod(TESR_NormalsBuffer, float4(uv - px, 0, 0)).xyz * 2.0 - 1.0;
	float weight = 1.0;
	const float2 q[3] = { float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0) };
	[unroll] for (int k = 0; k < 3; k++) {
		float w = abs(readDepthLod(uv + q[k] * 0.5 * px) - depth) < tolerance ? 1.0 : 0.0;
		sum += w * (tex2Dlod(TESR_NormalsBuffer, float4(uv + q[k] * px, 0, 0)).xyz * 2.0 - 1.0);
		weight += w;
	}
	return float4(sum / weight, depth);
}

// ---- Step 1b, a quarter of the resolution: the light each block's surface reflects ----
// r: how much sky it reflects, g: where in the sky (0 the horizon's colour .. 1 the zenith's; below the horizon the
// horizon's, dimmed into r), b: the sun's highlight (times the sun's colour and shadow in step 2), a: the depth it was
// worked out at (step 2's upscaling compares it with each pixel's own).
float4 SpecularLowPS(VSOUT IN, float2 vpos : VPOS) : COLOR0 {
	float2 px = TESR_ReciprocalResolution.xy, lowPx = px * LowScale;
	float2 uv = (vpos + 0.5) * lowPx;                             // this block (its centre, in screen UV)
	float4 centre = tex2Dlod(TESR_SpecularNormals, float4(uv, 0, 0));
	float depth = centre.a;
	[branch] if (depth >= DrawDistance || depth >= farZ * 0.98) return float4(0.0, 0.0, 0.0, depth);

	float3 eyeVector = toWorld(uv) * depth;
	float3 V = -normalize(eyeVector);

	// The normal over a wide patch: the blocks round this one, one and three blocks out (about 4 to 14 pixels), those
	// whose depth is off this one's left out (another object). Wide enough to blur the depth-rebuilt normals' triangle
	// facets into the gentle curve the game lights them with.
	float3 sum = centre.xyz;
	float weight = 1.0;
	float tolerance = 0.05 * depth + 4.0;
	const float2 ring[16] = { float2(1, 0), float2(1, 1), float2(0, 1), float2(-1, 1), float2(-1, 0), float2(-1, -1), float2(0, -1), float2(1, -1),
	                          float2(3, 1), float2(1, 3), float2(-1, 3), float2(-3, 1), float2(-3, -1), float2(-1, -3), float2(1, -3), float2(3, -1) };
	[unroll] for (int k = 0; k < 16; k++) {
		float4 s = tex2Dlod(TESR_SpecularNormals, float4(clampToRectIfOn(uv + ring[k] * lowPx, lowPx), 0, 0));	// UNOFFICIAL 2026-10-27 RenderScale v2: not past the drawn part
		float w = abs(s.a - depth) < tolerance ? 1.0 : 0.0;
		sum += w * s.xyz;
		weight += w;
	}
	float3 meanNormal = sum / weight;
	float agreement = saturate(length(meanNormal));              // 1: all the same; less where they disagree
	float3 N = normalize(ViewToWorld(meanNormal));

	// Water has its own reflections.
	float3 worldPos = TESR_CameraPosition.xyz + eyeVector;
	[branch] if (N.z > 0.9 && abs(worldPos.z - TESR_WaterSettings.x) < (depth / farZ) * 200.0) return float4(0.0, 0.0, 0.0, depth);

	// The roughness, widened by the normals' spread (Toksvig: the variance of the normals adds to the lobe's).
	float alpha = Roughness * Roughness;
	float variance = (1.0 - agreement) / max(agreement, 0.001);
	float alpha2 = saturate(alpha * alpha + AAStrength * 2.0 * variance);
	float roughAA = sqrt(sqrt(alpha2));

	float3 L = normalize(TESR_SunDirection.xyz);
	float3 Hv = normalize(L + V);
	float NdotL = saturate(dot(N, L));
	float NdotV = max(dot(N, V), 0.0001);
	float NdotH = saturate(dot(N, Hv));
	float VdotH = saturate(dot(V, Hv));
	// GGX, height-correlated Smith, Schlick; times pi: the game's lights are in the units where a white Lambert surface
	// reflects lightColor * N.L.
	float dn = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
	float D = alpha2 / (PI * dn * dn);
	float vis = 0.5 / (NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2) + NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2) + 0.00001);
	float F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
	// Capped at twice the sun's light (what a white surface facing it reflects): toward a low sun a smooth, wet surface
	// would otherwise blaze far past what the screen can show, a white sheet with no shape left in it.
	float sunSpec = min(D * vis * F * NdotL * PI, 2.0) * SunStrength;
	// The sun's shadow: with ForwardShadows (the default) the game shaders put it on the sun's light only, and
	// TESR_PointShadowBuffer has the contact shadows alone (step 2 applies those per pixel), so the cascades are read here.
	// (SPECULAR_SIM_SHADOWS: the GPU test has no cascades; it hands the sun's shadow over in TESR_PointShadowBuffer.)
#ifdef SPECULAR_SIM_SHADOWS
	sunSpec *= tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0, 0)).r;
#else
	[branch] if (sunSpec > 0.00001 && TESR_ShadowFade.y > 0.0) sunSpec *= GetSunCascadeLight(eyeVector, N);
#endif

	// The sky along the reflected view; below the horizon (the ground round the point), dimmer.
	float3 R = reflect(-V, N);
	float skyAt = sqrt(saturate(R.z));
	float below = R.z >= 0.0 ? 1.0 : lerp(0.6, 0.25, saturate(-R.z * 4.0));
	float2 env = EnvBRDFApprox(roughAA, NdotV);
	float skyAmount = (F0 * env.x + env.y) * SkyStrength * below;
	// UNOFFICIAL 2026-10-19 (no branches on these constants: this pass has no boolean register left; x and z are 0 or 1).
	skyAmount *= lerp(1.0, lerp(NVR_SpecularExtra.y, 1.0, tex2Dlod(NVR_SpecularSkyVis, float4(uv, 0, 0)).b), NVR_SpecularExtra.x);
	sunSpec *= lerp(1.0, tex2Dlod(NVR_SpecularSunFar, float4(uv, 0, 0)).a, NVR_SpecularExtra.z);

	float fade = 1.0 - smoothstep(0.7 * DrawDistance, DrawDistance, depth);
	return float4(skyAmount * fade, skyAt, sunSpec * fade, depth);
}

// ---- Step 2, full resolution: add it to the image ----
// Adds the light to a pixel's linear colour; false, leaving it alone, where there is none (far off, the sky, the weapon).
bool AddSpecular(float2 uv, float depth, inout float3 lin) {
	// (The weapon's depth buffer is empty where there is no weapon: 1, or 0 with reversed depth -- as WetWorld tests it.)
	float viewmodelDepth = tex2Dlod(TESR_DepthBufferViewModel, float4(uv, 0, 0)).x;
	bool weapon = TESR_DepthConstants.z == 0.0 ? viewmodelDepth < 0.99999 : viewmodelDepth > 0.0;
	[branch] if (depth >= DrawDistance || depth >= farZ * 0.98 || weapon) return false;

	// Water has its own reflections: tested again here with this pixel's own depth and normal. SpecularLowPS tests a 4 x 4
	// block's depth, which on water seen at a low angle missed the water's height on some rows of blocks and not on others,
	// and drew horizontal lines of highlight across the water.
	float3 pixelPos = TESR_CameraPosition.xyz + toWorld(uv) * depth;
	float3 pixelNormal = ViewToWorld(tex2Dlod(TESR_NormalsBuffer, float4(uv, 0, 0)).xyz * 2.0 - 1.0);
	[branch] if (normalize(pixelNormal).z > 0.7 && abs(pixelPos.z - TESR_WaterSettings.x) < max(2.0, depth * 0.002)) return false;

	// The four quarter-resolution pixels round this one, bilinearly, each counted by how near its depth (the depth at its
	// block's centre, read again here) is to this pixel's: so the light neither bleeds across the edges of things nor
	// steps along them.
	float2 px = TESR_ReciprocalResolution.xy, lowPx = px * LowScale;
	float2 p = uv / lowPx - 0.5;
	float2 f = frac(p);
	float2 at = (floor(p) + 0.5) * lowPx;
	float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	float4 s00 = tex2Dlod(TESR_SpecularBuffer, float4(at, 0, 0));
	// UNOFFICIAL 2026-10-27 RenderScale v2: the right / bottom taps stay in the drawn part (a block of stale light otherwise)
	float4 s10 = tex2Dlod(TESR_SpecularBuffer, float4(clampToRectIfOn(at + float2(lowPx.x, 0.0), lowPx), 0, 0));
	float4 s01 = tex2Dlod(TESR_SpecularBuffer, float4(clampToRectIfOn(at + float2(0.0, lowPx.y), lowPx), 0, 0));
	float4 s11 = tex2Dlod(TESR_SpecularBuffer, float4(clampToRectIfOn(at + lowPx, lowPx), 0, 0));
	float4 off = abs(float4(s00.a, s10.a, s01.a, s11.a) - depth) / (0.05 * depth + 4.0);
	off *= off;
	w *= 1.0 / (1.0 + off * off) + 0.0001;
	float3 light = (s00.rgb * w.x + s10.rgb * w.y + s01.rgb * w.z + s11.rgb * w.w) / dot(w, 1.0);

	float shadow = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0, 0)).r;
	float sunUp = 1.0 - TESR_ShadowFade.x;
	float3 sky = lerp(linearize(TESR_HorizonColor.rgb), linearize(TESR_SkyColor.rgb), light.g);
	lin = lin + light.r * sky + light.b * shadow * sunUp * linearize(TESR_SunColor.rgb);
	return true;
}

float4 SpecularPS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0));
	float3 lin = linearize(color.rgb);
	[branch] if (!AddSpecular(uv, readDepthLod(uv), lin)) return color;
	return float4(delinearize(lin), color.a);
}

// ---- The bounce light's full-resolution step, folded in (technique SpecularBounce) ----
// When nothing renders between the two (the wet world, the flashlight), the bounce light and this effect go onto the image
// in one full-resolution pass instead of two (SpecularEffect::RenderWithBounce): BounceLight.fx.hlsl's Combine, the same
// sums, then the specular on its result, with the colour kept in registers in between.
static const float BounceNearFade = 40.0;   // BounceLight.fx.hlsl's NearFade

bool AddBounce(float2 uv, float depth, inout float3 lin) {
	[branch] if (depth < BounceNearFade || depth > NVR_BounceData.z) return false;
	// Depth-aware bilinear upsample of the low-resolution result, as there.
	float2 position = uv / NVR_BounceLayout.zw - 0.5;
	float2 base = floor(position);
	float2 fraction = position - base;
	float tolerance = max(depth * 0.03, 2.0);
	float3 sum = 0.0;
	float weights = 1.0e-5;
	[unroll] for (int y = 0; y < 2; y++) {
		[unroll] for (int x = 0; x < 2; x++) {
			float4 tap = tex2Dlod(NVR_BounceBuffer, float4((base + float2(x, y) + 0.5) * NVR_BounceLayout.zw, 0.0, 0.0));
			float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
			weight /= 1.0 + abs(tap.a - depth) / tolerance;
			sum += tap.rgb * weight;
			weights += weight;
		}
	}
	float3 indirect = sum / weights;
	float peak = max(max(lin.r, lin.g), max(lin.b, 0.0001));
	float3 albedo = lin / peak * 0.6;
	lin += NVR_BounceData.x * indirect * albedo;
	return true;
}

float4 SpecularBouncePS(VSOUT IN) : COLOR0 {
	float2 uv = IN.UVCoord;
	float4 color = tex2Dlod(TESR_RenderedBuffer, float4(uv, 0, 0));
	float depth = readDepthLod(uv);
	float3 lin = linearize(color.rgb);
	bool bounced = AddBounce(uv, depth, lin);
	bool lit = AddSpecular(uv, depth, lin);
	[branch] if (!bounced && !lit) return color;
	return float4(delinearize(lin), color.a);
}

technique Specular {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SpecularPS();
	}
}

technique SpecularPrep {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SpecularPrepPS();
		Texture[6] = <NoTexture>;
	}
}

technique SpecularLow {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SpecularLowPS();
		Texture[5] = <NoTexture>;
	}
}

technique SpecularBounce {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 SpecularBouncePS();
	}
}
