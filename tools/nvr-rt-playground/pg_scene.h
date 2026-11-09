// pg_scene.h: the RT playground's world (tools/nvr-rt-playground/README.md "Adding a scene object"): meshes from basic shapes, objects
// (an instance of a mesh with a material), lamps, CPU-made textures, and BuildRoom(), the test room. World units are the game's (z up,
// ~70 units a metre); the materials and lamps use the game's encodings so the path tracer reads them exactly as in Fallout.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "NvrRtProtocol.h" // the record flags (tools/nvr-rt-helper)
#include "RtScenePick.h"   // the game's record / mask packing (src/core)

// ------------------------------------------------------------------------------------------------------------------------------
struct V3 { float x = 0, y = 0, z = 0; V3() {} V3(float a, float b, float c) : x(a), y(b), z(c) {} };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) { float l = length(a); return l > 0 ? a * (1.0f / l) : a; }

// ------------------------------------------------------------------------------------------------------------------------------
// A mesh: positions, normals (for the raster G-buffer: smooth on spheres, flat on boxes), texcoords, a 16-bit triangle list
struct Mesh {
	std::vector<V3> pos, nrm;
	std::vector<float> uv;   // 2 a vertex
	std::vector<uint16_t> idx;
	// a b c d counter-clockwise seen from the front; uv (0,0) (u,0) (u,v) (0,v)
	void quad(V3 a, V3 b, V3 c, V3 d, float u = 1.0f, float v = 1.0f) {
		const uint16_t base = uint16_t(pos.size());
		const V3 n = normalize(cross(b - a, c - a));
		pos.insert(pos.end(), { a, b, c, d });
		nrm.insert(nrm.end(), { n, n, n, n });
		const float q[8] = { 0, 0, u, 0, u, v, 0, v };
		uv.insert(uv.end(), q, q + 8);
		const uint16_t t[6] = { 0, 1, 2, 0, 2, 3 };
		for (uint16_t k : t) idx.push_back(uint16_t(base + k));
	}
	// an axis-aligned box, faces outward; texcoords in world units / uvUnit (a texture tiles every uvUnit units)
	void box(V3 lo, V3 hi, float uvUnit = 0.0f) {
		const V3 p[8] = { {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},{lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z} };
		const V3 s = hi - lo;
		auto t = [&](float a) { return uvUnit > 0 ? a / uvUnit : 1.0f; };
		quad(p[0], p[3], p[2], p[1], t(s.y), t(s.x)); quad(p[4], p[5], p[6], p[7], t(s.x), t(s.y));
		quad(p[0], p[1], p[5], p[4], t(s.x), t(s.z)); quad(p[1], p[2], p[6], p[5], t(s.y), t(s.z));
		quad(p[2], p[3], p[7], p[6], t(s.x), t(s.z)); quad(p[3], p[0], p[4], p[7], t(s.y), t(s.z));
	}
	// a UV sphere round the origin (smooth normals)
	void sphere(float r, int seg = 32, int rings = 16) {
		const uint16_t base = uint16_t(pos.size());
		for (int j = 0; j <= rings; j++) for (int i = 0; i <= seg; i++) {
			const float th = 3.14159265f * j / rings, ph = 6.2831853f * i / seg;
			const V3 n(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
			pos.push_back(n * r); nrm.push_back(n); uv.push_back(float(i) / seg); uv.push_back(float(j) / rings);
		}
		for (int j = 0; j < rings; j++) for (int i = 0; i < seg; i++) {
			const uint16_t a = uint16_t(base + j * (seg + 1) + i), b = uint16_t(a + 1), c = uint16_t(a + seg + 1), d = uint16_t(c + 1);
			if (j > 0) { idx.push_back(a); idx.push_back(c); idx.push_back(b); }
			if (j < rings - 1) { idx.push_back(b); idx.push_back(c); idx.push_back(d); }
		}
	}
};

// ------------------------------------------------------------------------------------------------------------------------------
// A texture made on the CPU: BGRA8 (VK_FORMAT_B8G8R8A8_UNORM = D3DFMT_A8R8G8B8's bytes), sRGB-encoded colour as the game's, all mips
struct CpuTexture {
	uint32_t id = 0;               // the real-texture id sent to the helper (NVR_RT_OP_TEXTURE)
	uint32_t w = 0, h = 0, levels = 0;
	std::vector<uint8_t> bytes;    // the levels packed one after the other
	std::vector<uint32_t> levelOffset;
	void* d3d = nullptr;           // the raster copy (IDirect3DTexture9*), made by the app
};
// fn(x, y) -> rgba 0-1 (colour sRGB-encoded); the mips box-filtered. alphaRef >= 0 (a cut-out): each mip's alpha is scaled so the share of
// texels passing the alpha test stays the full texture's (coverage-preserving mips, as cut-out textures are authored: a plain box
// filter makes thin wires fade under the test a few mips down, and the fence vanished at a distance)
inline CpuTexture MakeTexture(uint32_t id, uint32_t size, const std::function<void(int, int, float*)>& fn, float alphaRef = -1.0f) {
	CpuTexture t; t.id = id; t.w = t.h = size;
	std::vector<float> a(size_t(size) * size * 4);
	for (uint32_t y = 0; y < size; y++) for (uint32_t x = 0; x < size; x++) fn(int(x), int(y), &a[(size_t(y) * size + x) * 4]);
	uint32_t r = size;
	float coverage = -1.0f;
	while (true) {
		t.levelOffset.push_back(uint32_t(t.bytes.size()));
		float scale = 1.0f;
		if (alphaRef >= 0.0f) {
			std::vector<float> al(size_t(r) * r);
			for (size_t i = 0; i < al.size(); i++) al[i] = a[i * 4 + 3];
			if (coverage < 0.0f) { size_t n = 0; for (float v : al) n += v > alphaRef; coverage = float(n) / float(al.size()); }
			else if (coverage > 0.0f) {
				std::sort(al.begin(), al.end(), [](float p, float q) { return p > q; });
				const size_t k = std::min(al.size() - 1, size_t(coverage * float(al.size())));
				if (al[k] > 1e-4f) scale = std::fmin(alphaRef / al[k] * 1.001f, 64.0f);
			}
		}
		for (uint32_t i = 0; i < r * r; i++) {
			auto q = [&](int c) { return uint8_t(std::fmin(std::fmax(a[i * 4 + c] * (c == 3 ? scale : 1.0f), 0.0f), 1.0f) * 255.0f + 0.5f); };
			t.bytes.push_back(q(2)); t.bytes.push_back(q(1)); t.bytes.push_back(q(0)); t.bytes.push_back(q(3));   // B G R A
		}
		t.levels++;
		if (r == 1) break;
		const uint32_t n = r / 2;
		std::vector<float> b(size_t(n) * n * 4);
		for (uint32_t y = 0; y < n; y++) for (uint32_t x = 0; x < n; x++) for (int c = 0; c < 4; c++)
			b[(size_t(y) * n + x) * 4 + c] = 0.25f * (a[((2 * y) * r + 2 * x) * 4 + c] + a[((2 * y) * r + 2 * x + 1) * 4 + c] +
				a[((2 * y + 1) * r + 2 * x) * 4 + c] + a[((2 * y + 1) * r + 2 * x + 1) * 4 + c]);
		a.swap(b); r = n;
	}
	return t;
}

// ------------------------------------------------------------------------------------------------------------------------------
// An object: one instance of a mesh. Materials as the game encodes them: rough / metal -> the record's bits 26-31 (bounce hits) and the
// G-buffer's MaterialBuffer (the first hit: r = class + 0.49 metal, b = roughness); textures by atlas slot (real textures, protocol 7).
struct Object {
	std::string name;
	int mesh = 0;
	V3 pos;                    // world translation
	float yaw = 0.0f;          // turn round z (radians)
	float scale = 1.0f;
	V3 albedo = { 0.6f, 0.6f, 0.6f };   // linear; with a texture: x the texture (raster) / the record's average (RT where no texture reads)
	float rough = -1.0f, metal = -1.0f; // a material rule (>= 0); none: the path tracer's default (rough 0.8 at bounce hits, 0.7 on screen)
	int slot = -1;             // atlas slot of its textures (diffuse / glow / alpha): -1 none
	const CpuTexture* diffuse = nullptr;
	const CpuTexture* glowTex = nullptr;
	float glow = 0.0f;         // > 0: it glows (record flag 0x04). Traced: its glow texture x PTGlow (as the path tracer reads it);
	                           // raster only (bulbs): albedo x glow
	bool alphaTested = false;  // a cut-out (the diffuse texture's alpha), NVR_RT_MASK_ALPHA
	uint32_t alphaRef = 128;
	// (2026-11-07, the merged ptaaa-materials) the path tracer's material kind (NvrRtProtocol.h NVR_RT_KIND_*: record bits 0x02 / 0x08) and
	// the MaterialBuffer's class at the first hit (1 plain, 3 cloth, 5 plants, 6 skin). Glass (NVR_RT_KIND_GLASS) is drawn as the game draws
	// it: depth written, no AlbedoBuffer colour (the trace meets the pane itself), blended over the raster frame; non-opaque, mask 0x10.
	uint32_t kind = NVR_RT_KIND_PLAIN;
	int matClass = 1;
	bool glass() const { return kind == NVR_RT_KIND_GLASS; }
	bool traced = true;        // in the RT scene
	bool raster = true;        // drawn into the G-buffer; raster-only things (lamp bulbs) are things the RT scene lacks: the game's frame shows them
	bool visible = true;       // false: neither (a switched-off feature)
	std::function<void(Object&, double)> animate;   // per frame (t in seconds), may move / turn it
	V3 emissive() const { return albedo * glow; }

	RtScenePick::Record record() const {
		RtScenePick::Record r;
		r.albedoKnown = true; r.glowMap = glow > 0.0f; r.alphaTested = alphaTested; r.alphaRef = alphaRef;
		r.textured = slot >= 0 && !alphaTested; r.slot = slot < 0 ? 0 : slot;
		r.materialRule = rough >= 0.0f || metal >= 0.0f; r.rough = rough; r.metal = metal;
		r.kind = kind;
		return r;
	}
	uint32_t recordFlags() const {
		uint32_t f = RtScenePick::RecordFlags(record());
		// the game packs a material rule only on textured draws (RtScenePick); the playground's plain objects carry one too (the shader's
		// HitSurface reads bits 26-31 whatever the rest says)
		if (slot < 0 && (rough >= 0.0f || metal >= 0.0f)) {
			if (rough >= 0.0f) f |= NvrRtRoughCode(rough) << NVR_RT_RECORD_ROUGH_SHIFT;	// (2026-11-07 perf-reflfix2)
			if (metal >= 0.0f) { int k = int(metal * 6.0f + 0.5f) + 1; k = k > 7 ? 7 : k; f |= uint32_t(k) << NVR_RT_RECORD_METAL_SHIFT; }
		}
		return f;
	}
	uint32_t mask() const { return RtScenePick::InstanceMask(record()); }
	uint32_t opFlags() const { return RtScenePick::OpFlags(record()); }
	// rows = world axes, column 3 = translation (as NvrRtInstance::transform, before the camera offset)
	void transform(float m[3][4]) const {
		const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
		m[0][0] = c; m[0][1] = -s; m[0][2] = 0; m[0][3] = pos.x;
		m[1][0] = s; m[1][1] = c; m[1][2] = 0; m[1][3] = pos.y;
		m[2][0] = 0; m[2][1] = 0; m[2][2] = scale; m[2][3] = pos.z;
	}
};

// A lamp: a point light as the game's lamp table holds it (scene position, reach, linear colour x dimmer); the game's falloff
struct Lamp {
	std::string name;
	V3 pos;
	float reach = 500.0f;
	V3 colour = { 1, 1, 1 };
	int bulb = -1;             // its raster-only bulb object (follows the lamp)
	bool on = true;
	std::function<void(Lamp&, double)> animate;
};

struct Viewpoint { const char* name; V3 eye; float yaw, pitch; };

struct Scene {
	std::vector<Mesh> meshes;
	std::vector<Object> objects;
	std::vector<Lamp> lamps;
	std::vector<CpuTexture> textures;   // (stable addresses after BuildRoom: reserved)
	std::vector<Viewpoint> views;
	int addMesh(const Mesh& m) { meshes.push_back(m); return int(meshes.size()) - 1; }
	Object& add(const Mesh& m, const char* name) { Object o; o.name = name; o.mesh = addMesh(m); objects.push_back(o); return objects.back(); }
	Object& addInstance(int mesh, const char* name) { Object o; o.name = name; o.mesh = mesh; objects.push_back(o); return objects.back(); }
	Object* find(const char* name) { for (auto& o : objects) if (o.name == name) return &o; return nullptr; }
};

// ------------------------------------------------------------------------------------------------------------------------------
// The test room (world units, z up): 1200 x 1000 x 400 inside, a window in the east wall (+x) the sun shines through, a skylight gap in
// the ceiling, a chain-link fence card inside the window, spheres (diffuse, gold, chrome), a big wall mirror (west wall), a glowing panel
// (south wall), a pillar, a spinning cube, three lamps (one moving), the sunlit ground outside with a few blocks.
inline void BuildRoom(Scene& s) {
	s.textures.reserve(16);
	s.lamps.reserve(16);
	// textures (sRGB-encoded colours, as the game's diffuse maps)
	s.textures.push_back(MakeTexture(1, 256, [](int x, int y, float* c) {   // floor tiles: 4 x 4 tiles, grout lines, a little noise
		const int tx = x / 64, ty = y / 64; const bool grout = (x % 64) < 2 || (y % 64) < 2;
		const float n = float(((x * 73856093) ^ (y * 19349663)) & 255) / 255.0f * 0.06f;
		const float base = ((tx + ty) & 1) ? 0.62f : 0.52f;
		c[0] = grout ? 0.25f : base + n; c[1] = grout ? 0.24f : base * 0.93f + n; c[2] = grout ? 0.22f : base * 0.82f + n; c[3] = 1.0f; }));
	const CpuTexture* floorTex = &s.textures.back();
	s.textures.push_back(MakeTexture(2, 512, [](int x, int y, float* c) {   // chain-link: diagonal wires 10 texels wide every 128
		const int a = ((x + y) % 128 + 128) % 128, b = ((x - y) % 128 + 128) % 128;
		const bool wire = a < 10 || b < 10;
		c[0] = 0.62f; c[1] = 0.64f; c[2] = 0.66f; c[3] = wire ? 1.0f : 0.0f; }, 0.5f));
	const CpuTexture* fenceTex = &s.textures.back();
	s.textures.push_back(MakeTexture(3, 64, [](int x, int y, float* c) {    // the glowing panel's diffuse: pale with a dark frame
		const bool frame = x < 4 || y < 4 || x > 59 || y > 59;
		c[0] = c[1] = c[2] = frame ? 0.15f : 0.9f; c[3] = 1.0f; }));
	const CpuTexture* panelTex = &s.textures.back();
	s.textures.push_back(MakeTexture(4, 64, [](int x, int y, float* c) {    // its glow map: warm white inside the frame
		const bool frame = x < 4 || y < 4 || x > 59 || y > 59;
		c[0] = frame ? 0.0f : 1.0f; c[1] = frame ? 0.0f : 0.85f; c[2] = frame ? 0.0f : 0.6f; c[3] = 1.0f; }));
	const CpuTexture* panelGlow = &s.textures.back();
	s.textures.push_back(MakeTexture(5, 256, [](int x, int y, float* c) {   // a leaf card: a few elliptic leaves on a stem (alpha = the leaves)
		const float fx = x / 256.0f, fy = y / 256.0f;
		const float centres[6][3] = { { 0.3f, 0.25f, 0.6f }, { 0.68f, 0.3f, -0.5f }, { 0.35f, 0.6f, 0.4f }, { 0.7f, 0.65f, -0.7f }, { 0.5f, 0.85f, 0.1f }, { 0.52f, 0.45f, 0.2f } };
		float a = (std::fabs(fx - 0.5f) < 0.012f) ? 1.0f : 0.0f, vein = 0.0f;
		for (auto& l : centres) {
			const float dx = fx - l[0], dy = fy - l[1], cs = std::cos(l[2]), sn = std::sin(l[2]);
			const float u = (dx * cs + dy * sn) / 0.17f, v = (-dx * sn + dy * cs) / 0.08f;
			if (u * u + v * v < 1.0f) { a = 1.0f; if (std::fabs(v) < 0.08f) vein = 1.0f; }
		}
		c[0] = vein ? 0.42f : 0.22f; c[1] = vein ? 0.6f : 0.47f; c[2] = vein ? 0.2f : 0.12f; c[3] = a; }, 0.5f));
	const CpuTexture* leafTex = &s.textures.back();

	const float X = 600, Y = 500, Z = 400, T = 20;   // inside half-sizes, height, wall thickness
	{ Mesh m; m.quad({ -6000, -6000, 0 }, { 6000, -6000, 0 }, { 6000, 6000, 0 }, { -6000, 6000, 0 }, 12000.0f / 128.0f, 12000.0f / 128.0f);
	  Object& o = s.add(m, "floor"); o.albedo = { 0.55f, 0.5f, 0.45f }; o.slot = 1; o.diffuse = floorTex; o.rough = 0.6f; }
	// walls (boxes, so both faces are solid); the east wall (+x) has a window y -220..220, z 110..320
	auto wall = [&](V3 lo, V3 hi, const char* name) { Mesh m; m.box(lo, hi); Object& o = s.add(m, name); o.albedo = { 0.72f, 0.7f, 0.66f }; return &o; };
	wall({ -X - T, -Y - T, 0 }, { X + T, -Y, Z }, "wall south");
	wall({ -X - T, Y, 0 }, { X + T, Y + T, Z }, "wall north");
	wall({ -X - T, -Y, 0 }, { -X, Y, Z }, "wall west");
	wall({ X, -Y, 0 }, { X + T, -220, Z }, "wall east a");
	wall({ X, 220, 0 }, { X + T, Y, Z }, "wall east b");
	wall({ X, -220, 0 }, { X + T, 220, 110 }, "wall east sill");
	wall({ X, -220, 320 }, { X + T, 220, Z }, "wall east lintel");
	// the ceiling with a skylight gap x -380..-180, y -120..120
	auto ceil = [&](V3 lo, V3 hi, const char* name) { Mesh m; m.box(lo, hi); Object& o = s.add(m, name); o.albedo = { 0.8f, 0.8f, 0.78f }; };
	ceil({ -X - T, -Y - T, Z }, { -380, Y + T, Z + T }, "ceiling a");
	ceil({ -180, -Y - T, Z }, { X + T, Y + T, Z + T }, "ceiling b");
	ceil({ -380, -Y - T, Z }, { -180, -120, Z + T }, "ceiling c");
	ceil({ -380, 120, Z }, { -180, Y + T, Z + T }, "ceiling d");
	// spheres: diffuse red, gold (rough metal), chrome (the smoothest the record holds: rough 1/7; on screen 0.05)
	{ Mesh m; m.sphere(60, 48, 24); const int sm = s.addMesh(m);
	  Object& a = s.addInstance(sm, "sphere red"); a.pos = { -260, -300, 60 }; a.albedo = { 0.7f, 0.08f, 0.06f }; a.rough = 0.5f; a.metal = 0.0f;
	  Object& b = s.addInstance(sm, "sphere gold"); b.pos = { -60, -320, 60 }; b.albedo = { 1.0f, 0.72f, 0.3f }; b.rough = 0.3f; b.metal = 1.0f;
	  Object& c = s.addInstance(sm, "sphere chrome"); c.pos = { 140, -300, 60 }; c.albedo = { 0.95f, 0.95f, 0.95f }; c.rough = 0.0f; c.metal = 1.0f;
	  // (2026-11-07) a skin "head" (NVR_RT_KIND_SKIN, MaterialBuffer class 6: PTSkin's wrap + light through thin parts), in the sun's path
	  Object& d = s.addInstance(sm, "skin head"); d.pos = { 300, -150, 55 }; d.scale = 0.75f; d.albedo = { 0.78f, 0.52f, 0.42f }; d.rough = 0.5f; d.metal = 0.0f;
	  d.kind = NVR_RT_KIND_SKIN; d.matClass = 6; }
	// (2026-11-07) a glass pane in the window (NVR_RT_KIND_GLASS: PTGlass 2 = the light and the camera's rays meet it; tinted by its colour's hue)
	{ Mesh m; m.quad({ 610, 220, 110 }, { 610, -220, 110 }, { 610, -220, 320 }, { 610, 220, 320 });
	  Object& o = s.add(m, "glass pane"); o.albedo = { 0.7f, 0.92f, 0.85f }; o.kind = NVR_RT_KIND_GLASS; }
	// (2026-11-07) a leaf card (NVR_RT_KIND_FOLIAGE, class 5: PTFoliage's light through the leaf), back-lit by the sun from the window
	{ Mesh m; m.quad({ 320, -40, 40 }, { 320, 120, 40 }, { 320, 120, 230 }, { 320, -40, 230 });
	  Object& o = s.add(m, "leaf card"); o.slot = 4; o.diffuse = leafTex; o.alphaTested = true; o.alphaRef = 128; o.albedo = { 0.08f, 0.2f, 0.04f };
	  o.kind = NVR_RT_KIND_FOLIAGE; o.matClass = 5; }
	// the wall mirror (west wall): a thin box, metal, smooth
	{ Mesh m; m.box({ -X, -260, 50 }, { -X + 6, 260, 310 }); Object& o = s.add(m, "mirror"); o.albedo = { 0.92f, 0.92f, 0.92f }; o.rough = 0.0f; o.metal = 1.0f; }
	// the chain-link fence card inside the window (sun through it onto the floor), 3 x 2 tiles of the texture
	{ Mesh m; m.quad({ 470, -200, 0 }, { 470, 200, 0 }, { 470, 200, 270 }, { 470, -200, 270 }, 2.0f, 1.35f);
	  Object& o = s.add(m, "fence"); o.slot = 2; o.diffuse = fenceTex; o.alphaTested = true; o.alphaRef = 128; o.albedo = { 0.6f, 0.62f, 0.64f };
	  o.rough = 0.35f; o.metal = 1.0f; }
	// the glowing panel on the south wall (glow map; PTGlowFromFrame shows it as drawn, PTGlowLights aims at it)
	{ Mesh m; m.quad({ -470, -Y + 2, 150 }, { -270, -Y + 2, 150 }, { -270, -Y + 2, 270 }, { -470, -Y + 2, 270 });
	  Object& o = s.add(m, "glow panel"); o.slot = 3; o.diffuse = panelTex; o.glowTex = panelGlow; o.glow = 6.0f; o.albedo = { 1.0f, 0.85f, 0.6f }; }
	// a pillar and a spinning cube on a plinth
	{ Mesh m; m.box({ -45, -45, 0 }, { 45, 45, 300 }); Object& o = s.add(m, "pillar"); o.pos = { 260, 260, 0 }; o.albedo = { 0.45f, 0.5f, 0.55f }; }
	{ Mesh m; m.box({ -50, -50, 0 }, { 50, 50, 80 }); Object& o = s.add(m, "plinth"); o.pos = { -200, 230, 0 }; o.albedo = { 0.3f, 0.3f, 0.32f }; }
	{ Mesh m; m.box({ -40, -40, -40 }, { 40, 40, 40 }); Object& o = s.add(m, "spinning cube"); o.pos = { -200, 230, 130 }; o.albedo = { 0.1f, 0.35f, 0.75f }; o.rough = 0.25f; o.metal = 0.0f;
	  o.animate = [](Object& ob, double t) { ob.yaw = float(t * 0.8); ob.pos.z = 130.0f + 15.0f * float(std::sin(t * 1.3)); }; }
	// outside: a few blocks on the sunlit ground
	{ Mesh m; m.box({ -80, -80, 0 }, { 80, 80, 260 }); const int bm = s.addMesh(m);
	  const V3 at[3] = { { 1300, -500, 0 }, { 1700, 350, 0 }, { 1100, 700, 0 } };
	  const V3 col[3] = { { 0.65f, 0.45f, 0.3f }, { 0.4f, 0.55f, 0.35f }, { 0.55f, 0.55f, 0.6f } };
	  for (int i = 0; i < 3; i++) { Object& o = s.addInstance(bm, "outside block"); o.pos = at[i]; o.albedo = col[i]; o.yaw = 0.4f * i; } }
	// lamps: a warm ceiling lamp (north-west), a moving blue one, a small warm one by the mirror; raster-only bulbs (the RT scene has none:
	// a bulb round the lamp would block its own shadow rays; the game's frame shows them, as it does for what the RT scene lacks)
	{ Mesh m; m.sphere(8, 12, 8); const int bulb = s.addMesh(m);
	  auto lamp = [&](const char* name, V3 p, float reach, V3 col) {
		  Lamp l; l.name = name; l.pos = p; l.reach = reach; l.colour = col;
		  Object& b = s.addInstance(bulb, "bulb"); b.pos = p; b.traced = false; b.albedo = normalize(col) * 1.0f; b.glow = 3.0f;
		  l.bulb = int(s.objects.size()) - 1;
		  s.lamps.push_back(l); return &s.lamps.back(); };
	  lamp("lamp warm", { -330, 300, 330 }, 650, { 1.6f, 1.15f, 0.7f });
	  Lamp* moving = lamp("lamp moving", { 0, 60, 160 }, 520, { 0.6f, 1.0f, 2.4f });
	  moving->animate = [](Lamp& l, double t) { l.pos = V3(120.0f * float(std::cos(t * 0.7)), 60.0f + 220.0f * float(std::sin(t * 0.7)), 150.0f + 40.0f * float(std::sin(t * 1.9))); };
	  lamp("lamp mirror", { -480, -150, 120 }, 380, { 1.8f, 1.1f, 0.7f }); }
	// viewpoints (keys 1-4): eye, yaw (0 = +x), pitch (radians)
	s.views = { { "room from the door", { -480, 380, 170 }, -0.62f, -0.12f },
	            { "window + fence", { -150, -60, 160 }, 0.05f, -0.10f },
	            { "mirror + spheres", { 260, -150, 150 }, 3.4f, -0.14f },
	            { "outside, the window", { 1500, 120, 220 }, 3.14f, -0.08f } };
}
