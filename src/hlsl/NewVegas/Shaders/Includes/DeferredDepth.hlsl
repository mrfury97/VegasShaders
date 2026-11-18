// UNOFFICIAL 2026-11-14 DeferredDepth (core/DeferredDepth.h; docs/next/FD-DEPTH-RESULTS.md; plan section 4.3): the exact position of
// every pixel for the deferred light passes, from the hardware depth copied when the main opaque list ends.
//
// What the pass binds (DeferredDepth::Bind does all of it for a C++-driven pass):
//   - TESR_DeferredDepth: the world's depth buffer as the main list left it (INTZ: raw device depth in .x), screen sized; the world
//     sits in its top-left part under RenderScale v2 exactly as in the G-buffer. Read it POINT, CLAMP, no mips, no sRGB, LOD bias 0.
//   - or, with DeferredDepth = 2, TESR_DeferredDepthLinear: the same depth as view z in game units (R32F), made right after the copy
//     (Compute/DeferredDepthLinear.frag under the DXVK fork, Shaders/DeferredDepthLinear.hlsl as a D3D9 pass otherwise). Any filter.
//   - TESR_DeferredDepthFrame[6] (DD below), filled by DeferredDepth::FillFrame at the copy from the projection the list was drawn with
//     (NiDX9Renderer::projMatrix, TAA's jitter included: it is the frustum's _31 / _32):
//       DD[0] xyz the camera's right axis (world), w 1 / _11 (tan of half the fov across)
//       DD[1] xyz the camera's up axis, w 1 / _22
//       DD[2] xyz the camera's forward axis, w _33
//       DD[3] xy 1 / the viewport's size, zw the viewport's top-left corner (pixels)
//       DD[4] xyz the camera's world position, w _43
//       DD[5] xy _31, _32 (the jitter / off-centre shift in clip units), zw 1 / the depth copy's size
//     Device depth d = _33 + _43 / view z and ndc = view xy / view z x (_11, _22) + (_31, _32) (the D3D row-vector projection; FNV's
//     depth is reversed: 1 near, 0 = the clear = the sky).
//
// Nothing here declares a constant or a sampler: the includer declares them in its own registers and passes them in, so the helper
// adds nothing to the object shaders' full c200-c223 range and can be shared by every pass.

#ifndef DEFERRED_DEPTH_HLSL
#define DEFERRED_DEPTH_HLSL

// The view z (distance along the camera's forward axis, game units) of a raw device depth. The clear value (0 with FNV's reversed
// depth) gives the far plane. dd2w / dd4w: the projection's _33 and _43.
float DDViewZ(float device, float dd2w, float dd4w) {
    float den = device - dd2w;
    den = abs(den) < 1.0e-12f ? (dd4w < 0.0f ? -1.0e-12f : 1.0e-12f) : den;
    return dd4w / den;
}

// The view ray through a pixel (VPOS, in the target's pixels), NOT normalised: its forward component is 1, so ray x view z is the
// camera-relative position. The pixel's centre through the projection the list was drawn with (jitter included); DeferredLamps.hlsl
// DLPixel uses the same mapping without the jitter.
// (2026-11-15 fd-sun-fsr, docs/next/FD-SUN-FSR-RESULTS.md) No + 0.5: D3D9 puts pixel centres on whole coordinates and its viewport maps
// ndc -1 to the viewport's left edge X, so the pixel at VPOS i was rasterised at ndc 2 (i - X) / W - 1 (the fork offsets its Vulkan
// viewport to keep that). The + 0.5 put every position half a pixel (down-right) off its surface: on the land, under it, which
// DeferredSun's shadow lookup found self-shadowed in whole triangles (hidden at 1080p by a pull of the lookup's point; under FSR's
// RenderScale 0.67 a pixel is 1.5 x wider and it showed). DeferredDepth::CpuDistanceZ mirrors this.
float3 DDRay(float2 vpos, float4 dd0, float4 dd1, float4 dd2, float4 dd3, float4 dd5) {
    const float2 local = (vpos - dd3.zw) * dd3.xy;	// 0..1 across the viewport, at the pixel's centre
    const float2 ndc = float2(local.x * 2.0f - 1.0f, 1.0f - local.y * 2.0f) - dd5.xy;
    return dd2.xyz + dd0.xyz * (ndc.x * dd0.w) + dd1.xyz * (ndc.y * dd1.w);
}

// Camera-relative world position (world axes, origin at the camera) from a raw device depth.
float3 DDCameraRelative(float2 vpos, float device, float4 dd0, float4 dd1, float4 dd2, float4 dd3, float4 dd4, float4 dd5) {
    return DDRay(vpos, dd0, dd1, dd2, dd3, dd5) * DDViewZ(device, dd2.w, dd4.w);
}

// The texture uv of a pixel (VPOS) in the depth copy (the copy has the screen's size: the pixel's own texel).
float4 DDUv(float2 vpos, float4 dd5) { return float4((vpos + 0.5f) * dd5.zw, 0.0f, 0.0f); }

// The shared entry points, with the frame block as an array (float4 TESR_DeferredDepthFrame[6] in the includer):
//   DD_DEVICE_DEPTH(DD, sampler, vpos)     the raw device depth of the pixel
//   DD_CAMERA_RELATIVE(DD, vpos, device)   camera-relative world position
//   DD_WORLD_POSITION(DD, vpos, device)    world position
//   DD_CAMERA_DISTANCE(DD, vpos, device)   camera distance (what COLOR3.b holds as a half float)
//   DD_VIEW_Z(DD, device)                  distance along the camera's forward axis
// and from the linear texture (view z already):
//   DD_LINEAR_VIEW_Z(DD, sampler, vpos)    the pixel's view z
//   DD_CAMERA_RELATIVE_Z(DD, vpos, viewZ)  camera-relative world position (+ (DD)[4].xyz for the world position)
#define DD_DEVICE_DEPTH(DD, depthSampler, vpos) tex2Dlod(depthSampler, DDUv((vpos), (DD)[5])).x
#define DD_LINEAR_VIEW_Z(DD, linearSampler, vpos) tex2Dlod(linearSampler, DDUv((vpos), (DD)[5])).x
#define DD_CAMERA_RELATIVE_Z(DD, vpos, viewZ) (DDRay((vpos), (DD)[0], (DD)[1], (DD)[2], (DD)[3], (DD)[5]) * (viewZ))
#define DD_VIEW_Z(DD, device) DDViewZ((device), (DD)[2].w, (DD)[4].w)
#define DD_CAMERA_RELATIVE(DD, vpos, device) DDCameraRelative((vpos), (device), (DD)[0], (DD)[1], (DD)[2], (DD)[3], (DD)[4], (DD)[5])
#define DD_WORLD_POSITION(DD, vpos, device) (DD_CAMERA_RELATIVE(DD, vpos, device) + (DD)[4].xyz)
#define DD_CAMERA_DISTANCE(DD, vpos, device) length(DD_CAMERA_RELATIVE(DD, vpos, device))

#endif
