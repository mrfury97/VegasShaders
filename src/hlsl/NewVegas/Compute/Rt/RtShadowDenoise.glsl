// UNOFFICIAL 2026-11-07 the RT shadow denoiser ("ShadowDenoiser 1", docs/next/RT-DENOISER.md): shared by its stages
// (RtShadowDenoiseTemporal / Blur / Stabilize / Pack .comp). It runs in the RT helper as one game pass ("RtShadowDenoise"): the helper's
// module (tools/nvr-rt-helper/NvrRtShadowDenoise.inl) dispatches the stages with its own history images.
//
// What it is: a penumbra-aware shadow denoiser of the kind shipping RT games use (NVIDIA SIGMA, AMD FidelityFX Shadows): written here
// from the published designs, no third-party code. One ray a pixel toward a point on the light (a disc of the lamp's physical size, the
// sun's disc); per pixel the trace gives visibility (0 / 1) and the PENUMBRA HALF-WIDTH the hit implies at the receiver (world units:
// lamp radius x blocker distance / (lamp distance - blocker distance); the sun: blocker distance x tan(half angle)). Then:
//   1. Temporal: reprojected history (camera + the surface's own move, ObjectMotion) of the visibility's mean and second moment, the
//      penumbra width and the history length; restarted where the surface was not seen before (plane test against last frame's depth);
//      anti-lag: the history is cut short where it disagrees with this frame's 5 x 5 neighbourhood beyond its noise (a moving shadow).
//   2. Blur + post-blur: 16 rotated taps over a disc the size of the PENUMBRA in pixels (its world width over the pixel's world size),
//      edge-stopped by plane distance and normal: wide where the shadow is soft, nothing where the caster touches (contact hardening),
//      a little more while the history is short.
//   3. Stabilize: a short second history of the result, clamped to this frame's 3 x 3 range (no ghosting, no shimmer).
//   4. Pack: the result into what the game reads (the sun's visibility, the lamps' packed entries).
// Layers (lamps): K lamps are denoised at once, stacked: layer k is rows k H .. (k + 1) H - 1 of every image but depth / normals.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 1, std140) uniform Pass {
	vec4 size;          // x width, y height (the traced rect), z 1 / width, w 1 / height
	vec4 forward;       // xyz camera forward (scene space), w farZ (the depth image holds view z / farZ)
	vec4 right;         // xyz camera right / projection[0][0]
	vec4 up;            // xyz camera up / projection[1][1]
	vec4 prevForward;   // the previous frame's; w 1 = there is a previous frame (else every history restarts)
	vec4 prevRight;     // w 1 = the motion image is this frame's ObjectMotion (else the camera alone)
	vec4 prevUp;
	vec4 cameraMove;    // xyz the camera's world position now minus the previous frame's (units)
	vec4 jitter;        // xy this frame's depth jitter (screen uv: the depth pixel lies on the ray through uv + it), zw the previous frame's
	vec4 full;          // xy the depth / normal images' size the traced pixels map onto ((pixel + 0.5) x full / size), zw unused
	vec4 params;        // x MaxHistory (frames), y MaxRadius (pixels), z PlaneDistance (share of view z), w Stabilize (frames, 0 off)
	vec4 params2;       // x mode (0 sun, 1 lamps), y layers, z frame number, w DebugView (1 raw, 2 accumulated, 3 radius, 4 history)
	vec4 light;         // sun: w tan(the disc's half angle); x MinRadius (px), y MinRadiusNew (px x 1 / history), z NormalPower, (lamps too)
	vec4 layerSlot[2];  // lamps: layer k's cube slot (-1: none)
	vec4 layerReset[2]; // 1: layer k's history restarts this frame (another lamp in it, or the lamp moved)
	vec4 antiLag;       // x AntiLag (sigmas; 0 off), y its floor, z BlurFrames (radius x sqrt(it / history); 0 off), w BlurMinScale
	vec4 history;       // x MinHistory (frames: where the penumbra is under a pixel the one ray is already the answer), y HistoryRadius
	                    // (pixels of penumbra radius from which the whole MaxHistory is kept; between: in proportion)
	                    // z core: 0 these stages, 1 AMD FidelityFX Shadows (RtShadowDenoiseFfx.hlsl; the helper picks the stages)
} p;

#define TILE_W int(p.size.x)
#define TILE_H int(p.size.y)

int layers() { return int(p.params2.y + 0.5); }
float layerSlotOf(int k) { return k < 4 ? p.layerSlot[0][k] : p.layerSlot[1][k - 4]; }
bool layerResetOf(int k) { return (k < 4 ? p.layerReset[0][k] : p.layerReset[1][k - 4]) > 0.5; }

float hash12(vec2 q) {
	vec3 h = fract(vec3(q.xyx) * 0.1031);
	h += dot(h, h.yzx + 33.33);
	return fract((h.x + h.y) * h.z);
}

struct Surface { bool valid; float viewZ; vec3 position; vec3 normal; };

// The traced pixel's surface (as the trace rebuilt it: RtLampTrace.comp / RtSunShadow.comp)
Surface surfaceAt(ivec2 pixel, ivec2 source, float linear01, vec3 n) {
	Surface s;
	s.viewZ = linear01 * p.forward.w;
	s.valid = linear01 > 0.0 && linear01 < 0.9999 && s.viewZ >= 1.0;
	vec2 uv = (vec2(pixel) + 0.5) * p.size.zw + p.jitter.xy;
	s.position = (p.forward.xyz + p.right.xyz * (2.0 * uv.x - 1.0) - p.up.xyz * (2.0 * uv.y - 1.0)) * s.viewZ;
	n = n * 2.0 - 1.0;
	s.normal = normalize(normalize(p.right.xyz) * n.x + normalize(p.up.xyz) * n.y + normalize(p.forward.xyz) * n.z);
	return s;
}

ivec2 sourceOf(ivec2 pixel) { return ivec2((vec2(pixel) + 0.5) * p.full.xy * p.size.zw); }

// One pixel's world size at view depth z (the traced rect spans the whole screen)
float pixelWorld(float viewZ) { return 2.0 * viewZ * length(p.up.xyz) * p.size.w; }

// A scene-space point of this frame in the previous frame: its pixel coordinate (continuous: texel centres at .5) and view depth
bool reproject(vec3 position, vec3 objectMove, out vec2 prevPixel, out float prevZ) {
	vec3 q = position + p.cameraMove.xyz - objectMove;
	prevZ = dot(q, p.prevForward.xyz);
	if (!(prevZ > 1.0)) return false;
	float x = dot(q, p.prevRight.xyz) / (prevZ * dot(p.prevRight.xyz, p.prevRight.xyz));
	float y = -dot(q, p.prevUp.xyz) / (prevZ * dot(p.prevUp.xyz, p.prevUp.xyz));
	vec2 uv = vec2(x, y) * 0.5 + 0.5 - p.jitter.zw;
	prevPixel = uv * p.size.xy;
	return all(greaterThanEqual(prevPixel, vec2(0.0))) && all(lessThan(prevPixel, p.size.xy));
}

// Is a previous-frame texel (its stored view depth) the same surface as the reprojected point? Plane-ish test: relative depth,
// looser at grazing angles (the depth changes fast across a pixel there)
bool sameSurface(float storedZ, float expectedZ, float facing) {
	float tolerance = p.params.z * (1.0 + 4.0 * (1.0 - clamp(facing, 0.0, 1.0)));
	return storedZ > 0.0 && abs(storedZ - expectedZ) <= tolerance * expectedZ;
}

// The history a pixel may keep for a penumbra of this half-width (world units) at this depth: all of MaxHistory from HistoryRadius
// pixels of penumbra on, down to MinHistory under a pixel (a hard edge)
float historyCap(float width, float viewZ) {
	float cap = max(p.params.x, 1.0);
	if (p.history.y > 0.0 && width > 0.0) cap = mix(max(p.history.x, 1.0), cap, clamp(width / pixelWorld(viewZ) / p.history.y, 0.0, 1.0));
	return cap;
}

// How new a history still is (1 the first frame, 0 once it holds all it may): the extra blur while it is short
float newness(float hlen, float cap) { return max(1.0 / max(hlen, 1.0) - 1.0 / max(cap, 1.0), 0.0); }

// Golden-angle (Vogel) disc, 16 taps: evenly spread, rotated per pixel and pass
vec2 discTap(int i, float rotation) {
	float r = sqrt((float(i) + 0.5) / 16.0);
	float a = float(i) * 2.3999632 + rotation;
	return vec2(cos(a), sin(a)) * r;
}
