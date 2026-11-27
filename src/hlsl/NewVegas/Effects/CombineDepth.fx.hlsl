// CombineDepth: the game's world depth and the first-person (view model) depth become one linear depth per pixel.
// Output (read by every NVR effect that measures distance): R = viewZ / farZ, where viewZ is the view-space distance
// along the camera's forward axis in game units (positive in front of the camera). Sky (raw depth at the clear value) gives
// viewZ = far, so R = 1. G (only read when the target is G32R32F) = the reversed-Z post-projection depth recomputed from
// viewZ with the world near plane. B = A = 1. Readers rebuild the post-projection depth themselves (Includes/Depth.hlsl).
//
// Technique 0 "CombineDepth": the separate pass (the default). Technique "DepthNormals": the merged pass, the same depth in
// target 0 and the view-space normals (Normals.fx.hlsl encoding) in target 1 (MergedDepthNormals, off by default).

#include "includes/RenderRect.hlsl"

uniform float4x4 TESR_ProjectionTransform;
uniform float4x4 TESR_InvProjectionTransform;
uniform float4 TESR_DepthConstants;		// x: view-model near plane, z: 1 when the depth is reversed
uniform float4 TESR_CameraData;			// x: near plane, y: far plane
uniform float4 TESR_ReciprocalResolution;

uniform sampler2D TESR_DepthBufferWorld : register(s0) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
uniform sampler2D TESR_DepthBufferViewModel : register(s1) = sampler_state {
    ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

struct VSOUT { float4 vertPos : POSITION; float2 UVCoord : TEXCOORD0; };

struct VSIN { float4 vertPos : POSITION0; float2 UVCoord : TEXCOORD0; };

VSOUT FrameVS(VSIN px)
{
    VSOUT res = (VSOUT) 0.0f;
    res.vertPos = px.vertPos;
    res.UVCoord = px.UVCoord;
    return (res);
}

// The view-space distance of a raw depth value. Reversed (FNV): 1 at the near plane, 0 at the far plane.
// Standard (not expected in FNV, kept for a buffer whose clear value is 1): 0 at the near plane, 1 at the far plane.
float ViewZFromRaw(float raw_v, float near_z, float far_z)
{
    float span_v = far_z - near_z;
    float view_z = TESR_DepthConstants.z > 0.5 ?
        near_z * far_z / (near_z + raw_v * span_v) :
        near_z * far_z / (far_z - raw_v * span_v);
    return max(view_z, 1.0e-6);
}

// The combined linear depth (viewZ) at a rect uv: the view model's depth where it drew something, the world's otherwise.
float CombinedViewZ(float2 uv)
{
    float near_z = TESR_CameraData.x;
    float far_z = TESR_CameraData.y;
    float view_z = ViewZFromRaw(tex2D(TESR_DepthBufferWorld, uv).x, near_z, far_z);
    float view_raw = tex2D(TESR_DepthBufferViewModel, uv).x;
    if (TESR_DepthConstants.z > 0.5f ? (view_raw > 0.0f) : (view_raw < 0.99999f))
        view_z = ViewZFromRaw(view_raw, max(TESR_DepthConstants.x, 1.0e-4), far_z);
    return view_z;
}

float EncodeG(float view_z)
{
    float near_z = TESR_CameraData.x;
    float far_z = TESR_CameraData.y;
    return near_z * (far_z - view_z) / (view_z * (far_z - near_z));
}

float4 CombineDepthPS(VSOUT px) : COLOR
{
    float2 uv = clampToRectIfOn(px.UVCoord, TESR_ReciprocalResolution.xy);
    float view_z = CombinedViewZ(uv);
    return float4(view_z / TESR_CameraData.y, EncodeG(view_z), 1.0f, 1.0f);
}

// ---- Merged pass: depth (target 0) and normals (target 1) ----

// The view-space position of a rect uv at a linear depth (the inverse projection of the reversed-Z value for that depth).
float3 ViewPositionAt(float2 uv, float view_z)
{
    float2 tmp_s01 = rectToScreen(uv);
    float4 clip_v = float4(tmp_s01.x * 2.0f - 1.0f, (1.0f - tmp_s01.y) * 2.0f - 1.0f, EncodeG(view_z), 1.0f);
    float4 view_v = mul(clip_v, TESR_InvProjectionTransform);
    return view_v.xyz / view_v.w;
}

struct PSOUT_DepthNormals
{
    float4 depth : COLOR0;
    float4 normal : COLOR1;
};

PSOUT_DepthNormals DepthNormalsPS(VSOUT px)
{
    PSOUT_DepthNormals res;
    float2 uv = clampToRectIfOn(px.UVCoord, TESR_ReciprocalResolution.xy);
    float2 t = TESR_ReciprocalResolution.xy;

    // One and two texels to each side (clamped to the drawn part, so the nearest valid neighbour is used at an edge).
    float2 uv_l1 = clampToRect(uv - float2(t.x, 0.0f), t), uvL2 = clampToRect(uv - float2(2.0f * t.x, 0.0f), t);
    float2 uv_r1 = clampToRect(uv + float2(t.x, 0.0f), t), uvR2 = clampToRect(uv + float2(2.0f * t.x, 0.0f), t);
    float2 uv_u1 = clampToRect(uv - float2(0.0f, t.y), t), uvU2 = clampToRect(uv - float2(0.0f, 2.0f * t.y), t);
    float2 uv_d1 = clampToRect(uv + float2(0.0f, t.y), t), uvD2 = clampToRect(uv + float2(0.0f, 2.0f * t.y), t);

    float z_c = CombinedViewZ(uv);
    float z_l1 = CombinedViewZ(uv_l1), z_l2 = CombinedViewZ(uvL2);
    float z_r1 = CombinedViewZ(uv_r1), z_r2 = CombinedViewZ(uvR2);
    float z_u1 = CombinedViewZ(uv_u1), z_u2 = CombinedViewZ(uvU2);
    float z_d1 = CombinedViewZ(uv_d1), z_d2 = CombinedViewZ(uvD2);

    float3 p_c = ViewPositionAt(uv, z_c);
    float3 p_l1 = ViewPositionAt(uv_l1, z_l1), pR1 = ViewPositionAt(uv_r1, z_r1);
    float3 p_u1 = ViewPositionAt(uv_u1, z_u1), pD1 = ViewPositionAt(uv_d1, z_d1);

    // Each axis takes the side whose two-texel extrapolation predicts the centre's depth better (a crease or a silhouette
    // is not averaged across). Horizontal: left wins only when strictly better (ties go right). Vertical: down wins only
    // when strictly better (ties go up). Both derivatives point along +x and +y (up).
    float err_l = abs((2.0f * z_l1 - z_l2) - z_c);
    float err_r = abs((2.0f * z_r1 - z_r2) - z_c);
    float err_u = abs((2.0f * z_u1 - z_u2) - z_c);
    float err_d = abs((2.0f * z_d1 - z_d2) - z_c);
    float3 dx_v = err_l < err_r ? p_c - p_l1 : pR1 - p_c;
    float3 dy_v = err_d < err_u ? p_c - pD1 : p_u1 - p_c;

    float3 n = cross(dy_v, dx_v);
    float len_v = length(n);
    n = len_v > 1.0e-12f ? n / len_v : float3(0.0f, 0.0f, -1.0f);

    res.depth = float4(z_c / TESR_CameraData.y, EncodeG(z_c), 1.0f, 1.0f);
    res.normal = float4(n * 0.5f + 0.5f, 1.0f);
    return res;
}

technique CombineDepth
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 CombineDepthPS( );
    }
}

technique DepthNormals
{
    pass
    {
        VertexShader = compile vs_3_0 FrameVS( );
        PixelShader = compile ps_3_0 DepthNormalsPS( );
    }
}
