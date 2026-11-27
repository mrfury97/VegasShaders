// UNOFFICIAL Phase 2 Distant Land (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// Hardware-instanced crossed-card tree & desert flora impostors beyond uGridsToLoad = 5.
// Writes GBufferV2 (model 5 foliage) + hardware reversed depth so DeferredSun, VolumetricFog,
// and MaterialSSS (backlit foliage transmission) shade every distant tree/bush consistently.

float4 vsCamRight   : register(c0);
float4 vsCamUp      : register(c1);
float4 vsCamFwd     : register(c2);
float4 vsCamPos     : register(c3);
float4 vsProjA      : register(c4); // _11, _22, _33, _43
float4 vsProjB      : register(c5); // _31, _32, 0, 0
float4 vsLoadedRect : register(c6); // minX, minY, maxX, maxY

struct TreeVSIn {
	float3 localPos     : POSITION0; // crossed-card local pos (x in [-0.5, 0.5], y in [-0.5, 0.5], z in [0, 1])
	float2 uv           : TEXCOORD0;
	float3 cardN        : NORMAL0;
	float4 instPosScale : TEXCOORD1; // xyz world pivot, w scale
	float4 instInfo     : TEXCOORD2; // x width, y height, z rotZ (rad), w style (0..4)
};

struct TreeVSOut {
	float4 pos      : POSITION;
	float4 uvStyle  : TEXCOORD0; // xy uv, z style, w seed
	float4 worldN   : TEXCOORD1; // xyz world normal, w camDist
};

TreeVSOut TreeVS(TreeVSIn IN) {
	TreeVSOut OUT = (TreeVSOut)0;

	float3 basePos = IN.instPosScale.xyz;
	float3 toTree = basePos - vsCamPos.xyz;
	float dist = length(toTree);

	// Cull instances strictly inside loaded near cells (avoid drawing twice over 3D near trees)
	float dxOut = max(vsLoadedRect.x - basePos.x, basePos.x - vsLoadedRect.z);
	float dyOut = max(vsLoadedRect.y - basePos.y, basePos.y - vsLoadedRect.w);
	float outDist = max(dxOut, dyOut);

	float style = IN.instInfo.w;
	bool isBush = (style > 2.5f && style < 3.5f);
	float fadeStart = isBush ? 28000.0f : 88000.0f;
	float fadeEnd   = isBush ? 38000.0f : 112000.0f;

	if (outDist < 256.0f || dist > fadeEnd || dist < 1200.0f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	float growIn = smoothstep(256.0f, 1024.0f, outDist);
	float fadeOut = 1.0f - smoothstep(fadeStart, fadeEnd, dist);
	float s = IN.instPosScale.w * growIn * fadeOut;
	if (s <= 0.02f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	float cs = cos(IN.instInfo.z);
	float sn = sin(IN.instInfo.z);
	float wx = (IN.localPos.x * cs - IN.localPos.y * sn) * IN.instInfo.x * s;
	float wy = (IN.localPos.x * sn + IN.localPos.y * cs) * IN.instInfo.x * s;
	float wz = IN.localPos.z * IN.instInfo.y * s;

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
	if (abs(clipX) > vz * 1.15f || abs(clipY) > vz * 1.15f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	float3 rotN = float3(
		IN.cardN.x * cs - IN.cardN.y * sn,
		IN.cardN.x * sn + IN.cardN.y * cs,
		0.45f
	);
	float seed = frac(basePos.x * 0.00137f + basePos.y * 0.00293f);

	OUT.pos = float4(clipX, clipY, clipZ, vz);
	OUT.uvStyle = float4(IN.uv, style, seed);
	OUT.worldN = float4(normalize(rotN), length(rel));
	return OUT;
}

float4 psAmbient         : register(c0);  // sunAmbient.rgb, terrainAmbientScale
float4 psSkyData         : register(c1);  // terrainSkylightingScale, terrainSaturation, 0.85, gamma
float4 psFogColor        : register(c2);  // fogColor.rgb, linearSceneActive
float4 psFogData         : register(c3);  // fogNear, fogFar, fogPower, debugMode
float4 psHorizonFallback : register(c4);  // horizonColor.rgb, hasSH
float4 psSkyFallback     : register(c5);  // skyColor.rgb, 0
float4 psSkySH[9]        : register(c6);  // TESR_SkyIrradianceTerrain[0..8]

struct TreePSOut {
	float4 color0 : COLOR0;
	float4 color1 : COLOR1;
	float4 color2 : COLOR2;
	float4 color3 : COLOR3;
};

float2 TreeOctEncode(float3 n) {
	n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
	float2 e = n.xy;
	if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
	return e;
}

float TreeHash(float2 p) {
	float3 p3 = frac(p.xyx * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

TreePSOut TreePS(TreeVSOut IN) {
	TreePSOut OUT;

	float u = IN.uvStyle.x - 0.5f; // [-0.5 .. 0.5]
	float h = 1.0f - IN.uvStyle.y; // [0 = ground .. 1 = top]
	float style = IN.uvStyle.z;
	float seed = IN.uvStyle.w;

	float grain = TreeHash(floor(float2(u * 26.0f + seed * 19.0f, h * 34.0f)));
	float leafTex = 0.72f + 0.55f * grain;

	// Multi-species procedural alpha silhouette & botanical linear albedo
	float shape = 0.0f;
	float3 albedoLin = float3(0.055f, 0.068f, 0.032f);
	float3 trunkCol  = float3(0.052f, 0.042f, 0.032f);

	if (style < 0.5f) {
		// Style 0: Joshua Tree (branching desert trunk + spiky olive rosettes)
		float trunkMask = smoothstep(0.065f, 0.025f, abs(u)) * smoothstep(0.0f, 0.06f, h) * smoothstep(0.88f, 0.65f, h);
		float armL = length(float2((u + 0.21f) * 1.1f, (h - 0.64f) * 1.15f));
		float armR = length(float2((u - 0.20f) * 1.1f, (h - 0.76f) * 1.15f));
		float topC = length(float2(u * 1.15f, (h - 0.84f) * 1.25f));
		float crown = max(1.0f - min(topC, min(armL, armR)) / 0.23f, 0.0f);
		crown = crown - 0.25f * grain;
		shape = max(trunkMask, crown);
		float3 leafCol = float3(0.058f, 0.072f, 0.030f) * leafTex;
		albedoLin = lerp(trunkCol, leafCol, smoothstep(0.0f, 0.25f, crown - trunkMask * 0.6f));
	} else if (style < 1.5f) {
		// Style 1: Pine / Mountain Evergreen (tiered conifer skirts)
		float halfW = lerp(0.46f, 0.04f, pow(saturate(h), 0.85f));
		float tier = 0.82f + 0.18f * sin(h * 26.0f + seed * 6.28f);
		float trunkMask = smoothstep(0.055f, 0.02f, abs(u)) * smoothstep(0.22f, 0.08f, h);
		float canopy = (halfW * tier - abs(u)) / max(halfW, 0.06f);
		canopy = canopy * smoothstep(0.05f, 0.18f, h) - 0.28f * grain;
		shape = max(trunkMask, canopy);
		albedoLin = lerp(trunkCol, float3(0.024f, 0.050f, 0.020f) * leafTex, smoothstep(0.05f, 0.20f, canopy));
	} else if (style < 2.5f) {
		// Style 2: Dead Tree (dry twisted branches)
		float splitU = abs(u) - max(h - 0.35f, 0.0f) * 0.36f;
		float branch = smoothstep(0.08f, 0.015f, min(abs(u), abs(splitU)));
		shape = branch * smoothstep(0.0f, 0.05f, h) * smoothstep(0.98f, 0.72f, h) * (0.7f + 0.5f * grain);
		albedoLin = float3(0.062f, 0.054f, 0.044f) * (0.85f + 0.30f * grain);
	} else if (style < 3.5f) {
		// Style 3: Yucca / Creosote / Mesquite / Sagebrush (wide low arid scrub dome)
		float radial = length(float2(u * 1.05f, (h - 0.36f) * 1.45f));
		shape = (0.48f - radial) * 2.2f - 0.32f * grain;
		albedoLin = float3(0.052f, 0.058f, 0.032f) * leafTex;
	} else {
		// Style 4: Cottonwood / Palm (rounded crown over tall trunk)
		float trunkMask = smoothstep(0.055f, 0.02f, abs(u)) * smoothstep(0.65f, 0.35f, h);
		float crown = (0.44f - length(float2(u, (h - 0.68f) * 1.15f))) * 2.3f - 0.28f * grain;
		shape = max(trunkMask, crown);
		albedoLin = lerp(trunkCol, float3(0.048f, 0.070f, 0.030f) * leafTex, smoothstep(0.0f, 0.2f, crown));
	}

	clip(shape - 0.18f);

	// Optional DebugVar1 == 3 Ring-LOD highlight
	if (psFogData.w > 0.5f) {
		albedoLin = (style < 2.5f) ? float3(0.02f, 0.28f, 0.18f) : float3(0.18f, 0.28f, 0.02f);
	}

	float3 N = normalize(IN.worldN.xyz + float3(0.0f, 0.0f, 0.25f));
	float camDist = IN.worldN.w;
	float gamma = max(psSkyData.w, 1.0f);
	float invGamma = 1.0f / gamma;

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

	float2 octN = TreeOctEncode(N);
	OUT.color0 = float4(finalCol, 1.0f);
	OUT.color1 = float4(albedoLin, 1.0f + keep);          // GBufferV2 FD_LF_MATTE (no spec bit, lit by DeferredSun)
	OUT.color2 = float4(5.0f, octN.x, 0.85f, octN.y);    // model 5 = plants/foliage -> picked up by MaterialSSS backlit!
	OUT.color3 = float4(octN.x, octN.y, camDist, 1.0f);
	return OUT;
}
