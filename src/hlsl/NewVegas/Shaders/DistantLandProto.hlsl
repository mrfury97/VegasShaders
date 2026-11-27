// Prototype distant-land / Mojave night landmark light layer (Step 2, DISTANT-LAND-PLAN.md).
// Hardware-instanced camera-facing light clusters drawn over the pre-tonemap HDR scene
// with depth occlusion against TESR_DepthBuffer and linear-space compositing before Bloom.

float4 vsCamRight  : register(c0); // xyz: camera right axis (world)
float4 vsCamUp     : register(c1); // xyz: camera up axis (world)
float4 vsCamFwd    : register(c2); // xyz: camera forward axis (world)
float4 vsCamPos    : register(c3); // xyz: camera world position
float4 vsProjA     : register(c4); // (_11, _22, _33, _43)
float4 vsProjB     : register(c5); // (_31, _32, timeSec, nightFactor)
float4 vsCtrl      : register(c6); // (angMinScale, nearFadeStart, nearFadeEnd, intensityScale)

struct LightVSIn {
	float2 corner    : POSITION0; // [-1, 1]
	float2 uv        : TEXCOORD0; // [0, 1]
	float4 instPos   : TEXCOORD1; // xyz: world position, w: world radius
	float4 instCol   : TEXCOORD2; // rgb: linear HDR color, w: depth bias (units)
};

struct LightVSOut {
	float4 pos   : POSITION;
	float2 uv    : TEXCOORD0;
	float4 col   : TEXCOORD1;
	float4 zDist : TEXCOORD2; // x: center viewZ, y: world dist, z: depthBias, w: angular clamp ratio
};

LightVSOut LightVS(LightVSIn IN) {
	LightVSOut OUT = (LightVSOut)0;

	float3 rel = IN.instPos.xyz - vsCamPos.xyz;
	float dist = length(rel);
	float vx = dot(rel, vsCamRight.xyz);
	float vy = dot(rel, vsCamUp.xyz);
	float vz = dot(rel, vsCamFwd.xyz);

	if (vz <= 120.0f || dist < 3200.0f || vsProjB.w <= 0.001f) {
		OUT.pos = float4(0.0f, 0.0f, -2.0f, 1.0f);
		return OUT;
	}

	// Preserve sub-pixel distant beacons as soft angular minimum discs with gentle energy clamp.
	float effR = max(IN.instPos.w, vz * vsCtrl.x);
	float clampRatio = saturate(IN.instPos.w / max(effR, 1.0f));
	float eScale = pow(max(clampRatio, 0.04f), 0.72f);

	vx += IN.corner.x * effR;
	vy += IN.corner.y * effR;

	float clipX = vx * vsProjA.x + vz * vsProjB.x;
	float clipY = vy * vsProjA.y + vz * vsProjB.y;
	float rawClipZ = vz * vsProjA.z + vsProjA.w;
	// Keep clip.z strictly inside [0.001*w, 0.999*w] so very far landmarks never far-clip when depth-tested in PS.
	float clipZ = clamp(rawClipZ, 0.001f * vz, 0.999f * vz);

	float phase = IN.instPos.x * 0.013f + IN.instPos.y * 0.017f + IN.instPos.z * 0.009f;
	float twinkle = 0.88f + 0.12f * sin(vsProjB.z * 1.85f + phase);

	OUT.pos = float4(clipX, clipY, clipZ, vz);
	OUT.uv = IN.uv;
	OUT.col = float4(IN.instCol.rgb * (eScale * vsProjB.w * twinkle * vsCtrl.w), 1.0f);
	OUT.zDist = float4(vz, dist, IN.instCol.w, clampRatio);
	return OUT;
}

sampler2D sDepthTex : register(s0); // CombinedDepthTexture (linear viewZ / farZ in .x)
float4 psScreen : register(c0);     // (1/texW, 1/texH, farZ, extCoeff)
float4 psFade   : register(c1);     // (0, 0, nearFadeStart, nearFadeEnd)

float4 LightPS(float2 uv : TEXCOORD0, float4 col : TEXCOORD1, float4 zDist : TEXCOORD2, float2 vpos : VPOS) : COLOR0 {
	float2 p = uv * 2.0f - 1.0f;
	float r2 = dot(p, p);
	clip(1.0f - r2);

	float core = exp(-r2 * 13.5f) + 0.24f * pow(saturate(1.0f - r2), 2.3f);

	float2 depthUv = (vpos + 0.5f) * psScreen.xy;
	float sceneZ = tex2D(sDepthTex, depthUv).x * psScreen.z;
	// Sky clears to ~farZ; terrain/LOD closer than light center minus building depthBias occludes smoothly.
	float occ = (sceneZ >= psScreen.z * 0.985f) ? 1.0f : smoothstep(-280.0f, 520.0f, (sceneZ + zDist.z) - zDist.x);

	float nearFade = smoothstep(psFade.z, psFade.w, zDist.y);
	float haze = exp(-zDist.y * psScreen.w);

	return float4(col.rgb * (core * occ * nearFade * haze), 1.0f);
}

sampler2D sCopyTex  : register(s0); // Stored-curve pre-tonemap frame copy
sampler2D sProtoTex : register(s1); // Linear accumulated distant lights

float3 ProtoLinearize(float3 srgb) {
	float3 toe = srgb / 12.92f;
	float3 curve = pow(abs((srgb + 0.055f) / 1.055f), 2.4f);
	return (srgb <= 0.04045f) ? toe : curve;
}

float3 ProtoDelinearize(float3 lin) {
	float3 toe = lin * 12.92f;
	float3 curve = (pow(abs(lin), 1.0f / 2.4f) * 1.055f) - 0.055f;
	return (lin <= 0.0031308f) ? toe : curve;
}

float4 CompositePS(float2 uv : TEXCOORD0) : COLOR0 {
	float4 bg = tex2D(sCopyTex, uv);
	float3 proto = max(tex2D(sProtoTex, uv).rgb, 0.0f);
	if (dot(proto, float3(1.0f, 1.0f, 1.0f)) <= 1.0e-6f) return bg;
	float3 outRgb = ProtoDelinearize(ProtoLinearize(max(bg.rgb, 0.0f)) + proto);
	return float4(outRgb, bg.a);
}
