// MaterialSSS Phase 2 (Task 2, docs/next/MATERIAL-SSS-PLAN.md):
// Sparse screen-space Burley scatter on translucent solids (models 11..14) + GBufferV2 model debug view.
// In-pass Chan rough diffuse, warm Mojave ground bounce, cloth sheen & shadow-gated backlit transmission run inside
// DeferredSun (DeferredSun.comp + DeferredSun.hlsl) with zero extra passes.
// DebugVar2 modes:
//   1 = in-pass DeferredSun only (MaterialSSSProto skipped)
//   2 = in-pass DeferredSun + sparse 8-tap IGN Burley scatter on translucent solids ONLY (models 11..14)
//   3 = in-pass DeferredSun + extended close-range SS scatter (solids + close foliage/cloth)
//   4 = G-buffer material & SSS profile classification debug view (models 1..15)

sampler2D sFrameTex    : register(s0); // Linear HDR scene copy
sampler2D sAlbedoTex   : register(s1); // COLOR1: rgb linear albedo, a keep/flags
sampler2D sMaterialTex : register(s2); // COLOR2: r model/spec/metal, g/a normal or foliage keep, b roughness or foliage code
sampler2D sNormalsTex  : register(s3); // COLOR3: rg smooth normal, b cam dist, a code
sampler2D sDepthTex    : register(s4); // DeferredDepth::LinearTexture (viewZ in .x)

float4 msScreen : register(c0); // (1/texW, 1/texH, projScalePx, mode)
float4 msSunDir : register(c1); // xyz: to-sun dir (world), w: linearZReady
float4 msSunCol : register(c2); // rgb: linear sun color, w: temporal IGN jitter angle (rad)
float4 msDD[6]  : register(c3); // c3..c8 = DeferredDepth::Frame[0..5]
float4 msEnv    : register(c9); // x: isExterior, y: gamma, z: sunAmount.x (day/night factor), w: unused

float3 SssDecodeNormal(float2 e) {
	float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
	return normalize(n);
}

float SssModel(float r) {
	float m = floor(r + 0.01f);
	return m >= 15.5f ? m - 16.0f : m;
}

float SssFoliageGrassIndex(float g) {
	float o = (g >= 63.99f ? 1.0f : 0.0f) + (g >= 127.99f ? 1.0f : 0.0f) + (g >= 255.99f ? 1.0f : 0.0f) + (g >= 511.99f ? 1.0f : 0.0f);
	float p = 32.0f * (o > 0.5f ? 2.0f : 1.0f) * (o > 1.5f ? 2.0f : 1.0f) * (o > 2.5f ? 2.0f : 1.0f) * (o > 3.5f ? 2.0f : 1.0f);
	return 1024.0f * o + floor((g / p - 1.0f) * 1024.0f + 0.5f);
}

float SssFoliageKeep(float g) {
	return g < 31.99f ? saturate(floor((g - 16.0f) * 64.0f + 0.5f) / 1023.0f)
	                  : floor(SssFoliageGrassIndex(g) / 40.0f + 1.0e-3f) / 127.0f;
}

float SssLightFreeKeep(float a) {
	if (a > 4.5f) return saturate(a - 5.0f);
	if (a > 2.5f) return saturate(a - 3.0f);
	if (a > 1.0001f) return saturate(a - 1.0f);
	return saturate(a);
}

float SssRoughness(float b) {
	return (b >= 0.0f && b <= 1.0f) ? b : (b <= -1.99f && b >= -3.01f) ? (-2.0f - b) : 1.0f;
}

float3 SssRay(float2 vpos) {
	float2 local = (vpos - msDD[3].zw) * msDD[3].xy;
	float2 ndc = float2(local.x * 2.0f - 1.0f, 1.0f - local.y * 2.0f) - msDD[5].xy;
	return msDD[2].xyz + msDD[0].xyz * (ndc.x * msDD[0].w) + msDD[1].xyz * (ndc.y * msDD[1].w);
}

float SssIgn(float2 vpos) {
	return frac(52.9829189f * frac(dot(vpos, float2(0.06711056f, 0.00583715f))));
}

float4 MainPS(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {
	float4 uv4 = float4(uv, 0.0f, 0.0f);
	float4 frame = tex2Dlod(sFrameTex, uv4);
	float4 albedo = tex2Dlod(sAlbedoTex, uv4);
	float4 m = tex2Dlod(sMaterialTex, uv4);
	float4 normals = tex2Dlod(sNormalsTex, uv4);

	int mode = (int)(msScreen.w + 0.5f);
	if (albedo.a < 0.5f) {
		return (mode == 4) ? float4(0.0f, 0.0f, 0.0f, frame.a) : frame;
	}

	float model = SssModel(m.r);
	bool skinPx     = (normals.a > 3.5f && abs(model - 6.0f) < 0.5f);
	bool foliageDef = (m.r > 0.5f && m.b > 1.5f && albedo.a > 0.5f && albedo.a < 1.0005f);
	bool grassPx    = (foliageDef && m.g > 31.99f && m.g < 1024.0f) || abs(model - 7.0f) < 0.5f;
	bool plantPx    = (foliageDef && !grassPx) || abs(model - 5.0f) < 0.5f;
	bool foliagePx  = grassPx || plantPx;
	bool landPx     = abs(model - 4.0f) < 0.5f;
	bool hairOrEye  = (model > 7.5f && model < 10.5f);
	bool solidSssPx = (model > 10.5f && model < 15.5f); // 11=wax/plastic, 12=marble, 13=snow, 14=flesh, 15=custom

	float metal = foliagePx ? 0.0f : saturate((m.r - floor(m.r)) / 0.49f);
	float rough = foliagePx ? 0.78f : SssRoughness(m.b);
	bool explicitCloth = (abs(model - 3.0f) < 0.5f);
	bool clothPx = !foliagePx && !landPx && !skinPx && !hairOrEye && !solidSssPx && metal < 0.15f && (explicitCloth || rough > 0.65f);

	if (mode == 4) {
		float3 dbg = float3(0.20f, 0.35f, 0.80f);
		if (skinPx) dbg = float3(0.95f, 0.65f, 0.55f);
		else if (abs(model - 11.0f) < 0.5f) dbg = float3(0.98f, 0.55f, 0.12f); // wax/plastic
		else if (abs(model - 12.0f) < 0.5f) dbg = float3(0.92f, 0.88f, 0.78f); // marble
		else if (abs(model - 13.0f) < 0.5f) dbg = float3(0.45f, 0.85f, 1.00f); // snow
		else if (abs(model - 14.0f) < 0.5f) dbg = float3(0.95f, 0.22f, 0.18f); // flesh/food
		else if (abs(model - 15.0f) < 0.5f) dbg = float3(0.85f, 0.35f, 0.90f); // custom _s.dds
		else if (grassPx) dbg = float3(0.55f, 0.95f, 0.15f);
		else if (plantPx) dbg = float3(0.10f, 0.90f, 0.25f);
		else if (landPx) dbg = float3(0.45f, 0.30f, 0.18f);
		else if (metal >= 0.40f) dbg = float3(0.75f, 0.78f, 0.85f);
		else if (clothPx) dbg = float3(0.15f, 0.78f, 0.95f);
		return float4(dbg * dbg, frame.a);
	}

	if (skinPx || hairOrEye) clip(-1.0f);

	// Mode 2 runs ONLY sparse Burley scatter on translucent solids (models 11..14); Mode 3 adds close foliage/cloth.
	bool allowBlur = (mode == 2) ? solidSssPx : (mode == 3 && (solidSssPx || foliagePx || explicitCloth));
	clip(allowBlur ? 1.0f : -1.0f);

	float3 ray = SssRay(vpos);
	float invRayLen = rsqrt(max(dot(ray, ray), 1.0e-6f));
	float linearZ = msSunDir.w > 0.5f ? tex2Dlod(sDepthTex, float4((vpos + 0.5f) * msDD[5].zw, 0.0f, 0.0f)).x : (normals.b * invRayLen);
	float viewZ = max(linearZ, 1.0f);
	if (mode == 3 && !solidSssPx && viewZ > 2200.0f) clip(-1.0f);

	// Per-profile Burley scatter tint & world-space kernel radius
	float3 sssTint = float3(1.05f, 0.86f, 0.68f);
	if (foliagePx) {
		sssTint = grassPx ? float3(1.10f, 1.12f, 0.90f) : float3(1.06f, 1.12f, 0.90f);
	} else if (explicitCloth) {
		sssTint = float3(1.06f, 0.98f, 0.92f);
	} else if (solidSssPx) {
		sssTint = (model < 11.5f) ? float3(1.22f, 0.62f, 0.24f) : // wax/plastic
		          (model < 12.5f) ? float3(1.02f, 0.94f, 0.86f) : // marble
		          (model < 13.5f) ? float3(0.68f, 0.92f, 1.18f) : // snow
		                            float3(1.20f, 0.42f, 0.22f);  // flesh/food
	}

	float worldRad = solidSssPx ? (abs(model - 12.0f) < 0.5f ? 0.55f : 1.35f) : (foliagePx ? 2.10f : 1.05f);
	float pxRad = clamp(worldRad * msScreen.z / viewZ, 1.5f, 11.0f);
	float rot = 6.2831853f * SssIgn(vpos) + msSunCol.w;

	float3 accum = frame.rgb;
	float wSum = 1.0f;
	[loop] for (int i = 0; i < 8; ++i) {
		float ang = rot + (float)i * 2.39996323f;
		float rFrac = sqrt(((float)i + 0.5f) / 8.0f);
		float2 dPx = float2(cos(ang), sin(ang)) * (pxRad * rFrac);
		float2 tapUv = clamp(uv + dPx * msScreen.xy, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
		float4 tapUv4 = float4(tapUv, 0.0f, 0.0f);

		float4 tapAlb = tex2Dlod(sAlbedoTex, tapUv4);
		float4 tapM = tex2Dlod(sMaterialTex, tapUv4);
		float4 tapN = tex2Dlod(sNormalsTex, tapUv4);

		float tapModel = SssModel(tapM.r);
		bool tapFol = (tapM.r > 0.5f && tapM.b > 1.5f && tapAlb.a > 0.5f && tapAlb.a < 1.0005f)
			|| abs(tapModel - 5.0f) < 0.5f || abs(tapModel - 7.0f) < 0.5f;
		bool sameClass = (tapAlb.a >= 0.5f) && (foliagePx ? tapFol : (abs(tapModel - model) < 0.5f));

		float tapZ = msSunDir.w > 0.5f
			? tex2Dlod(sDepthTex, float4((vpos + dPx + 0.5f) * msDD[5].zw, 0.0f, 0.0f)).x
			: tapN.b * invRayLen;
		float depthW = exp(-abs(tapZ - viewZ) / (0.045f * viewZ + 2.5f));
		float w = sameClass ? (depthW * exp(-2.2f * rFrac * rFrac)) : 0.0f;

		accum += tex2Dlod(sFrameTex, tapUv4).rgb * w;
		wSum += w;
	}
	float3 blurred = accum / max(wSum, 1.0e-4f);
	float scatterAmt = solidSssPx ? 0.58f : ((foliagePx ? 0.30f : 0.18f) * smoothstep(2200.0f, 800.0f, viewZ));
	float3 result = frame.rgb + (blurred - frame.rgb) * sssTint * scatterAmt;
	return float4(max(result, 0.0f), frame.a);
}
