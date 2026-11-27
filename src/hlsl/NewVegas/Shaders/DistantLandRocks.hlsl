// UNOFFICIAL Phase 3 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Hardware-instanced low-poly desert boulders, canyon outcrops & cliff slabs beyond uGridsToLoad = 5.
// Writes GBufferV2 (model 1 matte sandstone/limestone tinted by underlying MacroAlbedoTex) + hardware reversed depth.

float4 vsCamRight   : register(c0);
float4 vsCamUp      : register(c1);
float4 vsCamFwd     : register(c2);
float4 vsCamPos     : register(c3);
float4 vsProjA      : register(c4); // _11, _22, _33, _43
float4 vsProjB      : register(c5); // _31, _32, 0, 0
float4 vsLoadedRect : register(c6); // minX, minY, maxX, maxY
float4 vsMacroMap   : register(c7); // MapWest, MapNorth, invSpanX, invSpanY

struct RockVSIn {
	float3 localPos     : POSITION0;
	float3 localN       : NORMAL0;
	float2 uv           : TEXCOORD0;
	float4 instPosScale : TEXCOORD1; // xyz world pivot, w scale
	float4 instInfo     : TEXCOORD2; // x width, y height, z rotZ (rad), w style (0..2)
};

struct RockVSOut {
	float4 pos      : POSITION;
	float4 uvStyle  : TEXCOORD0; // xy macroUV, z style, w seed
	float4 worldN   : TEXCOORD1; // xyz world normal, w camDist
	float4 worldPos : TEXCOORD2; // xyz worldPos, w localTopFactor
};

RockVSOut RockVS(RockVSIn IN) {
	RockVSOut OUT = (RockVSOut)0;

	float3 basePos = IN.instPosScale.xyz;
	float3 toRock = basePos - vsCamPos.xyz;
	float dist = length(toRock);

	// Cull instances inside loaded near cells (avoid drawing twice over 3D near meshes)
	float dxOut = max(vsLoadedRect.x - basePos.x, basePos.x - vsLoadedRect.z);
	float dyOut = max(vsLoadedRect.y - basePos.y, basePos.y - vsLoadedRect.w);
	float outDist = max(dxOut, dyOut);

	float style = IN.instInfo.w;
	float fadeStart = (style > 1.5f) ? 105000.0f : (style > 0.5f ? 78000.0f : 56000.0f);
	float fadeEnd   = (style > 1.5f) ? 138000.0f : (style > 0.5f ? 102000.0f : 74000.0f);

	if (outDist < 96.0f || dist > fadeEnd || dist < 1400.0f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	float growIn = smoothstep(96.0f, 850.0f, outDist);
	float fadeOut = 1.0f - smoothstep(fadeStart, fadeEnd, dist);
	float seed = frac(basePos.x * 0.00173f + basePos.y * 0.00311f);
	float s = IN.instPosScale.w * growIn * fadeOut;
	if (s <= 0.02f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	// Deterministic subtle aspect variation so identical rock prototypes break silhouettes
	float sx = (0.86f + 0.28f * seed) * IN.instInfo.x * s;
	float sy = (1.14f - 0.26f * seed) * IN.instInfo.x * s;
	float sz = (0.88f + 0.26f * frac(seed * 7.31f)) * IN.instInfo.y * s;

	float cs = cos(IN.instInfo.z);
	float sn = sin(IN.instInfo.z);
	float lx = IN.localPos.x * sx;
	float ly = IN.localPos.y * sy;
	float wx = lx * cs - ly * sn;
	float wy = lx * sn + ly * cs;
	float wz = IN.localPos.z * sz;

	float3 worldPos = basePos + float3(wx, wy, wz);
	float3 rel = worldPos - vsCamPos.xyz;
	float vx = dot(rel, vsCamRight.xyz);
	float vy = dot(rel, vsCamUp.xyz);
	float vz = dot(rel, vsCamFwd.xyz);

	if (vz <= 64.0f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	float clipX = vx * vsProjA.x + vz * vsProjB.x;
	float clipY = vy * vsProjA.y + vz * vsProjB.y;
	float clipZ = vz * vsProjA.z + vsProjA.w;

	float3 rotN = float3(
		IN.localN.x * cs - IN.localN.y * sn,
		IN.localN.x * sn + IN.localN.y * cs,
		IN.localN.z
	);

	float2 macroUV = saturate(float2(
		(basePos.x - vsMacroMap.x) * vsMacroMap.z,
		(vsMacroMap.y - basePos.y) * vsMacroMap.w
	));

	OUT.pos = float4(clipX, clipY, clipZ, vz);
	OUT.uvStyle = float4(macroUV, style, seed);
	OUT.worldN = float4(normalize(rotN), length(rel));
	OUT.worldPos = float4(worldPos, saturate((IN.localPos.z + 0.22f) / 1.10f));
	return OUT;
}

float4 psAmbient         : register(c0);  // sunAmbient.rgb, terrainAmbientScale
float4 psSkyData         : register(c1);  // terrainSkylightingScale, terrainSaturation, 0.85, gamma
float4 psFogColor        : register(c2);  // fogColor.rgb, linearSceneActive
float4 psFogData         : register(c3);  // fogNear, fogFar, fogPower, debugMode
float4 psHorizonFallback : register(c4);  // horizonColor.rgb, hasSH
float4 psSkyFallback     : register(c5);  // skyColor.rgb, 0
float4 psSkySH[9]        : register(c6);  // TESR_SkyIrradianceTerrain[0..8]

sampler2D sMacroAlbedoPS : register(s1);  // A8R8G8B8: worldspace VCLR*LTEX macro diffuse

struct RockPSOut {
	float4 color0 : COLOR0;
	float4 color1 : COLOR1;
	float4 color2 : COLOR2;
	float4 color3 : COLOR3;
};

float2 RockOctEncode(float3 n) {
	n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
	float2 e = n.xy;
	if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
	return e;
}

float RockHash(float2 p) {
	float3 p3 = frac(p.xyx * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

RockPSOut RockPS(RockVSOut IN) {
	RockPSOut OUT;

	float3 smoothN = normalize(IN.worldN.xyz);
	float3 flatN = cross(ddx(IN.worldPos.xyz), ddy(IN.worldPos.xyz));
	float flatLen2 = dot(flatN, flatN);
	flatN = (flatLen2 > 1.0e-8f) ? (flatN * rsqrt(flatLen2)) : smoothN;
	if (dot(flatN, smoothN) < 0.0f) flatN = -flatN;
	float3 N = normalize(lerp(smoothN, flatN, 0.72f));

	float camDist = IN.worldN.w;
	float style = IN.uvStyle.z;
	float seed = IN.uvStyle.w;
	float gamma = max(psSkyData.w, 1.0f);
	float invGamma = 1.0f / gamma;

	// Weather-stained sedimentary rock base + local macro terrain biome matching
	float grain = RockHash(floor(IN.worldPos.xy * 0.055f + IN.worldPos.z * 0.025f));
	float strata = 0.86f + 0.16f * sin(IN.worldPos.z * 0.042f + seed * 6.28f);
	float3 rawRock = (style > 1.5f) ? float3(0.056f, 0.050f, 0.042f) : (style > 0.5f ? float3(0.070f, 0.062f, 0.050f) : float3(0.080f, 0.072f, 0.060f));
	float4 macroSample = tex2D(sMacroAlbedoPS, IN.uvStyle.xy);
	float3 macroLin = gamma > 1.001f ? pow(max(macroSample.rgb, 1.0e-4f), gamma) : macroSample.rgb;
	macroLin = lerp(dot(macroLin, float3(0.2126f, 0.7152f, 0.0722f)).xxx, macroLin, 0.70f);
	float3 albedoLin = lerp(rawRock, macroLin * 0.85f, saturate(0.30f * macroSample.a)) * (0.86f + 0.24f * grain) * strata;

	// Slight base ambient occlusion near rock skirt
	albedoLin *= lerp(0.72f, 1.05f, IN.worldPos.w);

	// Optional DebugVar1 == 3 Ring-LOD highlight
	if (psFogData.w > 0.5f) {
		albedoLin = float3(0.28f, 0.14f, 0.04f);
	}

	float3 ambLin = (gamma > 1.001f ? pow(max(psAmbient.rgb, 1.0e-6f), gamma) : psAmbient.rgb)
	              * (gamma > 1.001f ? pow(max(psAmbient.w, 1.0e-6f), gamma) : psAmbient.w);

	float3 skyLin = 0.0f;
	if (psHorizonFallback.w > 0.5f) {
		float3 E = psSkySH[0].rgb
		         + psSkySH[1].rgb * N.y + psSkySH[2].rgb * N.z + psSkySH[3].rgb * N.x
		         + psSkySH[4].rgb * (N.x * N.y) + psSkySH[5].rgb * (N.y * N.z)
		         + psSkySH[6].rgb * (3.0f * N.z * N.z - 1.0f) + psSkySH[7].rgb * (N.x * N.z)
		         + psSkySH[8].rgb * (N.x * N.x - N.y * N.y);
		float3 skyEnc = sqrt(max(E, 0.0f));
		skyLin = gamma > 1.001f ? pow(max(skyEnc, 1.0e-6f), gamma) : skyEnc;
	} else {
		float3 hLin = gamma > 1.001f ? pow(max(psHorizonFallback.rgb, 1.0e-6f), gamma) : psHorizonFallback.rgb;
		float3 sLin = gamma > 1.001f ? pow(max(psSkyFallback.rgb, 1.0e-6f), gamma) : psSkyFallback.rgb;
		skyLin = lerp(hLin, sLin, saturate(N.z)) * (0.5f + 0.5f * N.z);
	}
	float skyScaleLin = gamma > 1.001f ? pow(max(psSkyData.x, 1.0e-6f), gamma) : psSkyData.x;
	float3 baseLitLin = albedoLin * (ambLin + skyLin * skyScaleLin);

	float fogAmt = 0.0f;
	if (psFogData.z > 0.0f) {
		float fogRange = max(psFogData.y - psFogData.x, 1.0f);
		float fogLinear = 1.0f - saturate((psFogData.y - camDist) / fogRange);
		fogAmt = fogLinear > 0.0f ? pow(fogLinear, max(psFogData.z, 0.01f)) : 0.0f;
	}
	float keep = saturate(1.0f - fogAmt);

	float3 litEnc = gamma > 1.001f ? pow(max(baseLitLin, 1.0e-6f), invGamma) : baseLitLin;
	float3 stored = lerp(litEnc, psFogColor.rgb, fogAmt);
	float3 finalCol = psFogColor.w > 0.5f ? (gamma > 1.001f ? pow(max(stored, 1.0e-6f), gamma) : stored) : stored;

	float2 octN = RockOctEncode(N);
	OUT.color0 = float4(finalCol, 1.0f);
	OUT.color1 = float4(albedoLin, 1.0f + keep);          // GBufferV2 FD_LF_SPEC -> lit by DeferredSun + shadows
	OUT.color2 = float4(1.0f, octN.x, 0.88f, octN.y);    // model 1 rough matte rock dielectric
	OUT.color3 = float4(octN.x, octN.y, camDist, 1.0f);
	return OUT;
}
