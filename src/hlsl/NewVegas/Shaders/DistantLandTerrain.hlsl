// UNOFFICIAL Phase 1 Distant Land prototype (Task 1, docs/next/DISTANT-LAND-PLAN.md):
// VS-drawn GPU Geo-Clipmap terrain writing GBufferV2 + hardware reversed depth.
// Works seamlessly with DeferredSun, TerrainShadows, VolumetricFog, and DistantLandProto night lights.

float4 vsCamRight    : register(c0);
float4 vsCamUp       : register(c1);
float4 vsCamFwd      : register(c2);
float4 vsCamPos      : register(c3);
float4 vsProjA       : register(c4);  // _11, _22, _33, _43
float4 vsProjB       : register(c5);  // _31, _32, 0, 0
float4 vsRing        : register(c6);  // snapX, snapY, stepSize, ringIndex
float4 vsHeightMap   : register(c7);  // west, north, invTexel, 0
float4 vsHeightDim   : register(c8);  // width, height, 1/width, 1/height
float4 vsLoadedRect  : register(c9);  // minX, minY, maxX, maxY (loaded uGridsToLoad cells)
float4 vsMorph       : register(c10); // morphStart (22), invMorphWidth (0.1), innerSinkZ (18), skirtWidth (512)
float4 vsPrevRect    : register(c11); // prevMinX, prevMinY, prevMaxX, prevMaxY (for Early-Z inner ring plunge)
float4 vsLoadedMask  : register(c12); // x=bits[0..9], y=bits[10..19], z=bits[20..24], w=valid (5x5 loaded cells)

sampler2D sHeightVS : register(s0);   // D3DVERTEXTEXTURESAMPLER0 (R32F, POINT, CLAMP)

struct TerrainVSIn {
	float2 grid : POSITION0;
};

struct TerrainVSOut {
	float4 pos      : POSITION;
	float4 worldPos : TEXCOORD0; // xyz world position, w ringIndex
	float4 uvOut    : TEXCOORD1; // xy height/normal map uv, z outDist from loadedCells, w morph
};

TerrainVSOut TerrainVS(TerrainVSIn IN) {
	TerrainVSOut OUT = (TerrainVSOut)0;

	// Continuous Geo-Clipmap vertex morphing onto the next coarser grid (all rings except outermost).
	float cheb = max(abs(IN.grid.x), abs(IN.grid.y));
	float morph = saturate((cheb - vsMorph.x) * vsMorph.y);
	if (vsRing.w >= vsProjB.z - 0.5f) morph = 0.0f;

	float2 oddFrac = frac(abs(IN.grid) * 0.5f) * 2.0f; // 1 on odd vertices, 0 on even
	float2 signGrid = float2(IN.grid.x >= 0.0f ? 1.0f : -1.0f, IN.grid.y >= 0.0f ? 1.0f : -1.0f);
	float2 morphedGrid = IN.grid - oddFrac * signGrid * morph;
	float2 worldXY = vsRing.xy + morphedGrid * vsRing.z;

	// Manual 4-tap bilinear lookup from R32F heightmap (POINT filtering is guaranteed for R32F VTF across D3D9 & DXVK).
	float col = clamp((worldXY.x - vsHeightMap.x) * vsHeightMap.z, 0.0f, vsHeightDim.x - 1.001f);
	float row = clamp((vsHeightMap.y - worldXY.y) * vsHeightMap.z, 0.0f, vsHeightDim.y - 1.001f);
	float c0 = floor(col);
	float r0 = floor(row);
	float2 f = float2(col - c0, row - r0);

	float u0 = (c0 + 0.5f) * vsHeightDim.z;
	float u1 = (c0 + 1.5f) * vsHeightDim.z;
	float v0 = (r0 + 0.5f) * vsHeightDim.w;
	float v1 = (r0 + 1.5f) * vsHeightDim.w;

	float h00 = tex2Dlod(sHeightVS, float4(u0, v0, 0.0f, 0.0f)).r;
	float h10 = tex2Dlod(sHeightVS, float4(u1, v0, 0.0f, 0.0f)).r;
	float h01 = tex2Dlod(sHeightVS, float4(u0, v1, 0.0f, 0.0f)).r;
	float h11 = tex2Dlod(sHeightVS, float4(u1, v1, 0.0f, 0.0f)).r;
	float worldZ = lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
	float hSpread = max(max(h00, h10), max(h01, h11)) - min(min(h00, h10), min(h01, h11));

	// Plunge vertices strictly inside loaded near cells OR strictly inside previous ring so hardware Early-Z rejects
	// occluded fragments before TerrainPS executes (no PS clip() needed).
	float dxOut = max(vsLoadedRect.x - worldXY.x, worldXY.x - vsLoadedRect.z);
	float dyOut = max(vsLoadedRect.y - worldXY.y, worldXY.y - vsLoadedRect.w);
	float outDist = max(dxOut, dyOut);
	float cellLoaded = 1.0f;
	if (outDist < 0.0f && vsLoadedMask.w > 0.5f) {
		float cx = clamp(floor((worldXY.x - vsLoadedRect.x) * (1.0f / 4096.0f)), 0.0f, 4.0f);
		float cy = clamp(floor((worldXY.y - vsLoadedRect.y) * (1.0f / 4096.0f)), 0.0f, 4.0f);
		float idx = cy * 5.0f + cx;
		float word = (idx < 9.5f) ? vsLoadedMask.x : ((idx < 19.5f) ? vsLoadedMask.y : vsLoadedMask.z);
		float localBit = (idx < 9.5f) ? idx : ((idx < 19.5f) ? (idx - 10.0f) : (idx - 20.0f));
		cellLoaded = floor(frac(word * exp2(-(localBit + 1.0f)) + 0.00025f) * 2.0f);
	}
	float seamSink = saturate((192.0f - outDist) / 384.0f) * 32.0f * cellLoaded;
	float deepSink = saturate((-outDist) / 192.0f) * 4096.0f * cellLoaded;
	float lodBias = (outDist >= 0.0f && vsRing.w >= 0.5f) ? (6.0f + 5.0f * vsRing.w + min(hSpread * 0.25f, 120.0f)) : 0.0f;

	float inPrev = min(min(worldXY.x - vsPrevRect.x, vsPrevRect.z - worldXY.x),
	                   min(worldXY.y - vsPrevRect.y, vsPrevRect.w - worldXY.y));
	float prevSink = (inPrev > 1.0f) ? (128.0f * (1.0f + vsRing.w)) : 0.0f;
	worldZ -= max(seamSink + deepSink, prevSink) + lodBias;

	float3 worldPos = float3(worldXY.x, worldXY.y, worldZ);
	float3 rel = worldPos - vsCamPos.xyz;
	float vx = dot(rel, vsCamRight.xyz);
	float vy = dot(rel, vsCamUp.xyz);
	float vz = dot(rel, vsCamFwd.xyz);

	float clipX = vx * vsProjA.x + vz * vsProjB.x;
	float clipY = vy * vsProjA.y + vz * vsProjB.y;
	float clipZ = vz * vsProjA.z + vsProjA.w;

	OUT.pos = float4(clipX, clipY, clipZ, vz);
	OUT.worldPos = float4(worldPos, vsRing.w);
	OUT.uvOut = float4((col + 0.5f) * vsHeightDim.z, (row + 0.5f) * vsHeightDim.w, outDist, cellLoaded);
	return OUT;
}

// -----------------------------------------------------------------------------
// Pixel Shader: procedural Mojave strata/biomes + GBufferV2 output
// -----------------------------------------------------------------------------

float4 psCamPos          : register(c0);
float4 psAmbient         : register(c1);  // sunAmbient.rgb, terrainAmbientScale
float4 psSkyData         : register(c2);  // terrainSkylightingScale, terrainSaturation, terrainRoughnessScale, gamma
float4 psFogColor        : register(c3);  // fogColor.rgb, linearSceneActive
float4 psFogData         : register(c4);  // fogNear, fogFar, fogPower, debugMode
float4 psLoadedRect      : register(c5);  // minX, minY, maxX, maxY
float4 psPrevRect        : register(c6);  // prevMinX, prevMinY, prevMaxX, prevMaxY
float4 psHeightRange     : register(c7);  // minZ, invRangeZ, macroAlbedoWeight, 0
float4 psHorizonFallback : register(c8);  // horizonColor.rgb, hasSH
float4 psSkyFallback     : register(c9);  // skyColor.rgb, 0
float4 psSkySH[9]        : register(c10); // TESR_SkyIrradianceTerrain[0..8]

sampler2D sNormalPS      : register(s0);  // A8R8G8B8: R=nx, G=ny, B=slope, A=ridge*0.5+0.5
sampler2D sMacroAlbedoPS : register(s1);  // A8R8G8B8: worldspace VCLR*LTEX or xLODGen macro diffuse atlas

struct TerrainPSOut {
	float4 color0 : COLOR0; // Ambient + sky SH + stored fog
	float4 color1 : COLOR1; // AlbedoBuffer: linear albedo, alpha = FD_LF_SPEC (1 + keep)
	float4 color2 : COLOR2; // MaterialBuffer (GBufferV2): 4 + 16 + 1/64, octN.x, roughness, octN.y
	float4 color3 : COLOR3; // CapturedNormals: octN.x, octN.y, camDist, 1.0
};

float2 OctEncode(float3 n) {
	n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
	float2 e = n.xy;
	if (n.z < 0.0f) {
		e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
	}
	return e;
}

// Dave Hoskins coordinate-stable hashes for large FNV world coordinates [-262144..+262144]
float Hash21(float2 p) {
	float3 p3 = frac(p.xyx * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

// Value noise + analytical XY gradient (z = value [0..1], xy = d/dx, d/dy)
float3 ValueNoiseGrad(float2 p) {
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0f - 2.0f * f);
	float2 du = 6.0f * f * (1.0f - f);
	float a = Hash21(i);
	float b = Hash21(i + float2(1.0f, 0.0f));
	float c = Hash21(i + float2(0.0f, 1.0f));
	float d = Hash21(i + float2(1.0f, 1.0f));
	float k1 = b - a;
	float k2 = c - a;
	float k4 = a - b - c + d;
	return float3(
		du.x * (k1 + k4 * u.y),
		du.y * (k2 + k4 * u.x),
		a + k1 * u.x + k2 * u.y + k4 * u.x * u.y
	);
}

TerrainPSOut TerrainPS(TerrainVSOut IN) {
	TerrainPSOut OUT;

	// World-space pixel footprint (top level outside dynamic flow control for clean anti-aliased detail fade).
	float footprint = max(1.0f, max(length(ddx(IN.worldPos.xy)), length(ddy(IN.worldPos.xy))));

	// Signed distance from loaded uGridsToLoad cells (used for DebugVar1=3 seam highlight; vertices inside plunge 4096u in VS so Hardware Early-Z rejects them with no PS clip()).
	float dxOut = max(psLoadedRect.x - IN.worldPos.x, IN.worldPos.x - psLoadedRect.z);
	float dyOut = max(psLoadedRect.y - IN.worldPos.y, IN.worldPos.y - psLoadedRect.w);
	float inLoaded = -max(dxOut, dyOut);

	float3 rel = IN.worldPos.xyz - psCamPos.xyz;
	float camDist = length(rel);

	// Decode smooth precomputed Sobel normal + ridge curvature.
	float4 nrmTex = tex2D(sNormalPS, IN.uvOut.xy);
	float2 nxy = nrmTex.rg * 2.0f - 1.0f;
	float nz = sqrt(max(1.0f - dot(nxy, nxy), 0.001f));
	float3 geoN = normalize(float3(nxy, nz));
	float ridge = nrmTex.a * 2.0f - 1.0f;
	float slope = 1.0f - geoN.z;
	float steepMask = smoothstep(0.03f, 0.22f, slope);

	// Footprint-clamped multi-scale procedural rock/ridge FBM & strata relief (zero origin-dependent moire).
	float2 wp = IN.worldPos.xy;
	float3 nMacro = ValueNoiseGrad(wp * 0.00022f);  // ~4500 unit desert patches
	float3 nBroad = ValueNoiseGrad(wp * 0.00085f + nMacro.z * 1.4f); // ~1200 unit ridges/washes
	float3 nMid   = ValueNoiseGrad(wp * 0.0028f);   // ~360 unit boulders/ledges
	float3 nFine  = ValueNoiseGrad(wp * 0.0095f);   // ~105 unit scree/grit

	float fadeBroad = saturate(1100.0f / (2.5f * footprint) - 0.5f);
	float fadeMid   = saturate(350.0f  / (2.5f * footprint) - 0.5f);
	float fadeFine  = saturate(105.0f  / (2.5f * footprint) - 0.5f);

	float ridgeBroad = 1.0f - abs(nBroad.z * 2.0f - 1.0f);
	float ridgeMid   = 1.0f - abs(nMid.z * 2.0f - 1.0f);

	float strataPhase = IN.worldPos.z * 0.015f + nMacro.z * 5.0f + nBroad.z * 2.0f;
	float strataWave = sin(strataPhase);

	float2 grad = 0.0f;
	grad += nBroad.xy * (0.16f * fadeBroad * (0.5f + 0.7f * steepMask));
	grad += nMid.xy   * (0.22f * fadeMid   * (0.45f + 0.75f * steepMask));
	grad += nFine.xy  * (0.16f * fadeFine);
	float gradLen = length(grad);
	if (gradLen > 0.28f) grad *= (0.28f / gradLen);

	float3 N = float3(geoN.x - grad.x, geoN.y - grad.y, geoN.z + strataWave * 0.06f * steepMask * fadeMid);
	N.z = max(N.z, max(0.18f, geoN.z * 0.65f));
	N = normalize(N);

	// Calibrated Mojave desert/mountain linear albedo matching FNV terrain brightness & warmth (used where macro diffuse is absent).
	float elev = saturate((IN.worldPos.z - psHeightRange.x) * psHeightRange.y);
	float3 sandCol      = float3(0.165f, 0.130f, 0.088f); // warm dusty Mojave gravel/sand
	float3 scrubCol     = float3(0.105f, 0.092f, 0.062f); // arid sagebrush & alkaline dirt
	float3 foothillRock = float3(0.155f, 0.122f, 0.082f); // weathered khaki-tan stone
	float3 steepRock    = float3(0.092f, 0.075f, 0.054f); // craggy sandstone/basalt
	float3 highlandCol  = float3(0.125f, 0.114f, 0.096f); // upper Charleston/Red Rock limestone

	float3 albedoLin = lerp(sandCol, scrubCol, smoothstep(0.28f, 0.75f, nMacro.z));
	albedoLin *= 0.84f + 0.32f * (0.6f * nBroad.z + 0.4f * nMid.z);

	float rockMask = smoothstep(0.04f, 0.25f, slope);
	float3 rockVar = lerp(steepRock, foothillRock, smoothstep(0.25f, 0.78f, nBroad.z));
	rockVar *= lerp(0.82f, 1.18f, 0.5f + 0.5f * strataWave);
	albedoLin = lerp(albedoLin, rockVar, rockMask);
	albedoLin = lerp(albedoLin, steepRock * 0.80f, smoothstep(0.28f, 0.58f, slope));
	albedoLin = lerp(albedoLin, highlandCol, smoothstep(0.55f, 0.90f, elev) * (1.0f - rockMask * 0.40f));

	// Procedural crevice/ridge/grain modulation when falling back to synthetic colors.
	float ridgeCrevice = lerp(0.78f, 1.14f, ridgeMid * 0.6f + ridgeBroad * 0.4f);
	float3 procDetailed = albedoLin
		* lerp(1.0f, ridgeCrevice, steepMask * fadeMid)
		* clamp(1.0f + ridge * 0.22f, 0.78f, 1.18f)
		* lerp(1.0f, 0.86f + 0.28f * nFine.z, fadeFine);

	// Real worldspace macro albedo (LOD diffuse DDS / VCLR * LTEX): preserve authored hue & brightness with subtle neutral micro-detail.
	float4 macroSrgb = tex2D(sMacroAlbedoPS, IN.uvOut.xy);
	float3 macroLin = pow(max(macroSrgb.rgb, 1.0e-4f), 2.2f);
	float microStrata = lerp(0.92f, 1.08f, 0.5f + 0.5f * strataWave);
	float microRidge  = lerp(0.90f, 1.10f, ridgeMid * 0.6f + ridgeBroad * 0.4f);
	float microDetail = lerp(1.0f, microStrata * microRidge, steepMask * fadeMid) * lerp(1.0f, 0.90f + 0.20f * nFine.z, fadeFine);
	albedoLin = lerp(procDetailed, macroLin * microDetail, psHeightRange.z * macroSrgb.a);
	albedoLin = max(albedoLin, 0.006f);

	// Optional DebugVar1 == 3 Ring-LOD & Loaded-Grid Seam visualization (calibrated luminance so sun doesn't blow out colors).
	if (psFogData.w > 0.5f) {
		float rIdx = IN.worldPos.w;
		float3 dbgCol =
			rIdx < 0.5f ? float3(0.01f, 0.14f, 0.18f) : // Ring 0: cyan
			rIdx < 1.5f ? float3(0.02f, 0.16f, 0.03f) : // Ring 1: green
			rIdx < 2.5f ? float3(0.18f, 0.15f, 0.01f) : // Ring 2: yellow
			rIdx < 3.5f ? float3(0.20f, 0.07f, 0.01f) : // Ring 3: orange
			rIdx < 4.5f ? float3(0.19f, 0.01f, 0.01f) : // Ring 4: red
			rIdx < 5.5f ? float3(0.17f, 0.02f, 0.17f) : // Ring 5: magenta
			rIdx < 6.5f ? float3(0.02f, 0.04f, 0.20f) : // Ring 6: blue
			              float3(0.14f, 0.14f, 0.16f);
		float seamLine = smoothstep(160.0f, 25.0f, abs(inLoaded));
		albedoLin = lerp(dbgCol * (0.85f + 0.15f * nMid.z), float3(0.45f, 0.45f, 0.45f), seamLine);
	}

	// Linear lighting conversion matching Terrain.hlsl / LinearLight.hlsl.
	float gamma = max(psSkyData.w, 1.0f);
	float invGamma = 1.0f / gamma;
	float3 ambLin = (gamma > 1.001f ? pow(max(psAmbient.rgb, 1.0e-6f), gamma) : psAmbient.rgb)
	              * (gamma > 1.001f ? pow(max(psAmbient.w, 1.0e-6f), gamma) : psAmbient.w);

	float3 skyLin = 0.0f;
	if (psHorizonFallback.w > 0.5f) {
		float3 E = psSkySH[0].rgb
		         + psSkySH[1].rgb * geoN.y
		         + psSkySH[2].rgb * geoN.z
		         + psSkySH[3].rgb * geoN.x
		         + psSkySH[4].rgb * (geoN.x * geoN.y)
		         + psSkySH[5].rgb * (geoN.y * geoN.z)
		         + psSkySH[6].rgb * (3.0f * geoN.z * geoN.z - 1.0f)
		         + psSkySH[7].rgb * (geoN.x * geoN.z)
		         + psSkySH[8].rgb * (geoN.x * geoN.x - geoN.y * geoN.y);
		float3 skyEnc = sqrt(max(E, 0.0f));
		skyLin = (gamma > 1.001f ? pow(max(skyEnc, 1.0e-6f), gamma) : skyEnc);
	} else {
		float3 hLin = gamma > 1.001f ? pow(max(psHorizonFallback.rgb, 1.0e-6f), gamma) : psHorizonFallback.rgb;
		float3 sLin = gamma > 1.001f ? pow(max(psSkyFallback.rgb, 1.0e-6f), gamma) : psSkyFallback.rgb;
		skyLin = lerp(hLin, sLin, saturate(geoN.z)) * (0.5f + 0.5f * geoN.z);
	}
	float skyScaleLin = gamma > 1.001f ? pow(max(psSkyData.x, 1.0e-6f), gamma) : psSkyData.x;
	float3 totalAmb = ambLin + skyLin * skyScaleLin;

	float lumaAlb = dot(albedoLin, float3(0.2126f, 0.7152f, 0.0722f));
	float3 gradedAlbedo = max(lerp(lumaAlb.xxx, albedoLin, psSkyData.y), 0.001f);
	float3 baseLitLin = gradedAlbedo * totalAmb;

	// Exact FNV distance fog when game fog is active (DeferredFog); 0 when VolumetricFog handles fog later.
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

	float2 octGeoN = OctEncode(geoN);
	float2 octN    = OctEncode(N);
	float roughness = saturate(0.86f * psSkyData.z);

	OUT.color0 = float4(finalCol, 1.0f);
	OUT.color1 = float4(albedoLin, 1.0f + keep);                                   // un-graded linear albedo (GBufferV2 FD_LF_SPEC; DeferredSun applies TerrainSaturation)
	OUT.color2 = float4(4.0f + 16.0f + (1.0f / 64.0f), octN.x, roughness, octN.y); // land + spec + FD_FAR_LAND_MARK + bumped shading normal
	OUT.color3 = float4(octGeoN.x, octGeoN.y, camDist, 1.0f);                      // smooth geometric normal (matches TerrainLODTemplate)
	return OUT;
}
