// Normals maths for NVR's Vulkan passes (needs NVR DXVK): GLSL ports of Effects/Normals.fx.hlsl. Included by Normals.frag,
// NormalsSmooth.frag and DepthNormals.frag (CombineDepth's merged pass). Matrices come from the push blocks as the D3DXMATRIX
// was stored, so a GLSL mat4 * vec4 here equals the HLSL mul(vec4, matrix) of the D3D9 pass.
// Encoding: the normal n is written as n * 0.5 + 0.5 (decode: stored * 2 - 1); view space +x right, +y up, +z away from the camera.

// The post-projection (reversed-Z) depth of a linear depth (viewZ), as CombineDepth stores it in G.
float nvrProjectedFromViewZ(float viewZ, vec4 cameraData) {
    float nearZ = cameraData.x;
    float farZ = cameraData.y;
    return nearZ * (farZ - viewZ) / (viewZ * (farZ - nearZ));
}

// The view-space position of a texture uv at a linear depth, through the inverse projection.
vec3 nvrViewPosition(vec2 uv, float viewZ, vec4 cameraData, mat4 invProjection) {
    vec4 clip = vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, nvrProjectedFromViewZ(viewZ, cameraData), 1.0);
    vec4 view = invProjection * clip;
    return view.xyz / view.w;
}

// The same position from the projection's x and y terms only (the smoothing passes): x = viewZ (ndc.x - P20) / P00, y likewise.
vec3 nvrViewPositionFromProjection(vec2 uv, float viewZ, mat4 projection) {
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = (1.0 - uv.y) * 2.0 - 1.0;
    float x = viewZ * (ndcX - projection[2][0]) / projection[0][0];
    float y = viewZ * (ndcY - projection[2][1]) / projection[1][1];
    return vec3(x, y, viewZ);
}

// The unit normal of a centre position from its one- and two-texel neighbours (positions; their z is the linear depth).
// Each axis takes the side whose two-texel extrapolation predicts the centre's depth better: horizontal left only when
// strictly better (ties go right), vertical down only when strictly better (ties go up). Both derivatives point along +x and +y.
vec3 nvrNormal(vec3 pC, vec3 pL1, vec3 pL2, vec3 pR1, vec3 pR2, vec3 pU1, vec3 pU2, vec3 pD1, vec3 pD2) {
    float errL = abs((2.0 * pL1.z - pL2.z) - pC.z);
    float errR = abs((2.0 * pR1.z - pR2.z) - pC.z);
    float errU = abs((2.0 * pU1.z - pU2.z) - pC.z);
    float errD = abs((2.0 * pD1.z - pD2.z) - pC.z);
    vec3 dx = errL < errR ? pC - pL1 : pR1 - pC;
    vec3 dy = errD < errU ? pC - pD1 : pU1 - pC;
    vec3 n = cross(dy, dx);
    float len = length(n);
    return len > 1.0e-12 ? n / len : vec3(0.0, 0.0, -1.0);
}

// The post-projection depth post of a texture uv (the reversed-Z value CombineDepth stores in G) back to a view-space position.
vec3 nvrPositionFromPost(vec2 uv, float post, mat4 invProjection) {
    vec4 view = invProjection * vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, post, 1.0);
    return view.xyz / view.w;
}

// The view-space normal of the merged pass (DepthNormals.frag): each depth is a vec2 (linear01, post-projection depth) of the
// combined depth at a neighbour, given by its offset in texels from uv: r1/r2 at +1/+2 in x, l1/l2 at -1/-2, u1/u2 at -1/-2 in y
// (screen up: uv.y decreases), d1/d2 at +1/+2 in y. The normal is nvrNormal's, the same rule as Normals.frag. farZ is unused here
// (the positions come from the post-projection depths); it is kept for the call's shape.
vec3 viewNormalFromTaps(vec2 uv, vec2 texel, mat4 invProjection, float farZ,
                        vec2 c, vec2 r1, vec2 r2, vec2 l1, vec2 l2, vec2 u1, vec2 u2, vec2 d1, vec2 d2) {
    vec3 pC = nvrPositionFromPost(uv, c.y, invProjection);
    vec3 pR1 = nvrPositionFromPost(uv + vec2(1.0, 0.0) * texel, r1.y, invProjection);
    vec3 pR2 = nvrPositionFromPost(uv + vec2(2.0, 0.0) * texel, r2.y, invProjection);
    vec3 pL1 = nvrPositionFromPost(uv + vec2(-1.0, 0.0) * texel, l1.y, invProjection);
    vec3 pL2 = nvrPositionFromPost(uv + vec2(-2.0, 0.0) * texel, l2.y, invProjection);
    vec3 pU1 = nvrPositionFromPost(uv + vec2(0.0, -1.0) * texel, u1.y, invProjection);
    vec3 pU2 = nvrPositionFromPost(uv + vec2(0.0, -2.0) * texel, u2.y, invProjection);
    vec3 pD1 = nvrPositionFromPost(uv + vec2(0.0, 1.0) * texel, d1.y, invProjection);
    vec3 pD2 = nvrPositionFromPost(uv + vec2(0.0, 2.0) * texel, d2.y, invProjection);
    return nvrNormal(pC, pL1, pL2, pR1, pR2, pU1, pU2, pD1, pD2);
}

// Inline CapturedNormals merge (mirrors core/CapturedNormals.h MergeIntoNormals):
// Must be called under uniform row0.w > 0.5 BEFORE any per-pixel early return so dFdx/dFdy execute in uniform quad control flow.
bool nvrTryCapturedNormal(vec2 s, float z, float farZ, vec4 c, vec4 m, float vmRaw,
                          vec4 tol, vec4 mergeView, vec4 row0, vec4 row1, vec4 row2,
                          out vec3 outEncoded) {
    bool vm = mergeView.z > 0.0 && (mergeView.w > 0.5 ? vmRaw > 0.0 : vmRaw < 0.99999);
    vec3 vp = vec3((s.x * 2.0 - 1.0) * mergeView.x, (1.0 - s.y * 2.0) * mergeView.y, 1.0) * z;
    if (vm) vp.xy *= mergeView.z;
    vec3 fn = cross(dFdy(vp), dFdx(vp));
    fn = normalize(fn + vec3(0.0, 0.0, 1.0e-20));
    if (dot(fn, vp) > 0.0) fn = -fn;
    if (c.a <= 0.99 || z >= farZ * 0.99) return false;
    float d = length(vp);
    float distTol = vm ? (d * max(tol.x, 0.12) + max(tol.y, 4.0)) : (d * tol.x + tol.y);
    if (abs(c.b - d) > distTol) return false;
    float grass = 0.0;
    if (row1.w > 0.5) {
        if (row2.w < 0.5) grass = (m.a > 0.5 && m.r > 0.5 && abs(m.g - 0.5) < 0.1) ? 1.0 : 0.0;
        else if (row2.w < 1.5) grass = (m.r > 0.5 && abs(m.g - 4.5) < 0.1) ? 1.0 : 0.0;
        else grass = (m.r > 0.5 && (abs(m.g - 4.5) < 0.1 || (m.g > 31.99 && m.g < 1024.0))) ? 1.0 : 0.0;
    }
    vec3 n = vec3(c.rg, 1.0 - abs(c.r) - abs(c.g));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    n = normalize(n);
    vec3 nv = normalize(n.x * row0.xyz + n.y * row1.xyz + n.z * row2.xyz);
    if (dot(nv, fn) > tol.z || grass > 0.5 || vm) {
        outEncoded = nv * 0.5 + 0.5;
        return true;
    }
    return false;
}
