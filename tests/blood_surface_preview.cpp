// Preview of BloodFlow's Surface mode ([Shaders.BloodFlow.Surface]) without the game: a ray-cast piece of desert ground
// (a slope with rocks, a crack running across it, a hollow, a steeper part, a flat slab, a back wall, a crate, and boxes
// standing in for a dead body and a standing actor), half of it in a building's shadow. The depth, normals, picture, albedo
// record, material record and sun shadow term are made here the way NVR hands them to an effect; then the real
// BloodFlow.fx.hlsl (techniques BloodSurfaceSim and BloodSurface) runs over them with the real patch code
// (src\effects\BloodFlow\BloodSurface.h), frame by frame, as BloodFlowEffect::Render does.
// Writes PNGs (a time-lapse, close-ups, debug views, a night shot with a lamp, the picture without the albedo record, the
// same blood from a second camera), checks in numbers what the pictures should show (CHECK lines) and measures the GPU cost.
// Usage: blood_surface_preview.exe <path to BloodFlow.fx.hlsl> <output folder> [w=1280] [h=720] [options]
//   quick   8 seconds instead of 40            full    compile all of BloodFlow.fx.hlsl, as the game does (minutes)
//   orbit   the camera changes place every 2.5 s while the blood runs (the ground is read from four sides)
//   cost    the GPU cost only; with `many`: 32 pools at once (every cell of the atlas) instead of the six sources
//   visc= rough= flow= pool= splat=   Viscosity, Roughness, FlowTime, PoolAmount, SplatAmount
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/effects/BloodFlow/BloodSurface.h"

static int W = 1280, H = 720;
static const float NearZ = 5.0f, FarZ = 100000.0f;

static float Hash(float x, float y) { float h = sinf(x * 127.1f + y * 311.7f) * 43758.5453f; return h - floorf(h); }
static float Noise(float x, float y) {
	float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
	float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
	return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
static float Fbm(float x, float y) { return 0.5f * Noise(x, y) + 0.3f * Noise(x * 2.1f, y * 2.1f) + 0.2f * Noise(x * 4.3f, y * 4.3f); }
static float ToGamma(float c) { c = c < 0 ? 0 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }
static float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float Smooth(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAILED %s: 0x%08lX (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

// ---- The scene ----------------------------------------------------------------------------------------------------
// The ground: falls toward the camera (-y) at about 6 degrees with long swells and small bumps; steeper on the right.
static const float WallY = 300.0f;
static const float Rocks[][4] = { { -52, 128, 11, 8 }, { -22, 110, 9, 7 }, { -80, 104, 10, 7 }, { 128, 62, 12, 9 }, { 170, 80, 9, 7 } };	// x, y, radius, height
static const float CrackA[2] = { -210, 104 }, CrackB[2] = { 62, 26 };	// a crack across the slope, falling to the right
static const float CrackHalf = 3.5f, CrackDeep = 4.0f;
static const float Hollow[4] = { 74, 4, 34, 5 };	// x, y, radius, depth
static const float Pad[4] = { -250, -130, -85, 15 };	// a flat slab: x0, x1, y0, y1
static float CrackDistance(float x, float y) {
	const float ax = CrackB[0] - CrackA[0], ay = CrackB[1] - CrackA[1];
	float t = ((x - CrackA[0]) * ax + (y - CrackA[1]) * ay) / (ax * ax + ay * ay);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	const float dx = x - CrackA[0] - ax * t, dy = y - CrackA[1] - ay * t;
	return sqrtf(dx * dx + dy * dy);
}
static float PadMask(float x, float y) {
	return Smooth(Pad[0] - 6, Pad[0], x) * (1 - Smooth(Pad[1], Pad[1] + 6, x)) * Smooth(Pad[2] - 6, Pad[2], y) * (1 - Smooth(Pad[3], Pad[3] + 6, y));
}
static float GroundZ(float x, float y) {
	float z = 0.105f * y + 6.0f * sinf(x * 0.011f + 0.6f) * cosf(y * 0.008f) + 8.0f * (Fbm(x * 0.006f + 4.0f, y * 0.006f) - 0.5f);
	z += 0.33f * max(y - 40.0f, 0.0f) * Smooth(100, 160, x);
	z += 0.5f * (Fbm(x * 0.08f, y * 0.08f) - 0.5f);
	for (const auto& r : Rocks) { const float u = ((x - r[0]) * (x - r[0]) + (y - r[1]) * (y - r[1])) / (r[2] * r[2]); if (u < 1) z += r[3] * (1 - u) * (1 - u); }
	const float cd = CrackDistance(x, y) / CrackHalf;
	if (cd < 1) z -= CrackDeep * (1 - cd * cd) * (1 - cd * cd);
	// (The hollow: longer one way, its rim pushed in and out, as a dip in real ground is.)
	const float hx = x - Hollow[0], hy = y - Hollow[1], hr = Hollow[2] * (0.75f + 0.5f * Fbm(x * 0.045f + 20.0f, y * 0.045f));
	const float hu = ((hx * 0.8f + hy * 0.35f) * (hx * 0.8f + hy * 0.35f) + (hy * 1.25f - hx * 0.3f) * (hy * 1.25f - hx * 0.3f)) / (hr * hr);
	if (hu < 1) z -= Hollow[3] * (1 - hu) * (1 - hu);
	const float pm = PadMask(x, y);
	if (pm > 0) z += (-9.0f - z) * pm;
	return z;
}
static D3DXVECTOR3 GroundNormal(float x, float y) {
	const float e = 0.25f;
	D3DXVECTOR3 n(-(GroundZ(x + e, y) - GroundZ(x - e, y)) / (2 * e), -(GroundZ(x, y + e) - GroundZ(x, y - e)) / (2 * e), 1.0f);
	D3DXVec3Normalize(&n, &n);
	return n;
}
// In the building's shadow: everything right of a line through the hollow.
static float SunShare(float x, float y) { return 1.0f - Smooth(-1.5f, 1.5f, x - (72.0f + 0.15f * (y - 4.0f))); }

// Boxes: a crate (rigid: material class 1) and two actors' stand-ins (class 2: a body lying on the first pool, someone
// standing in the pool on the slab). lo/hi in world units, the z of each set from the ground at its middle in main().
struct BoxObj { D3DXVECTOR3 lo, hi; int cls; float albedo[3]; };
static BoxObj Boxes[] = {
	{ D3DXVECTOR3(-118, 86, 0), D3DXVECTOR3(-92, 112, 26), 1, { 0.30f, 0.22f, 0.13f } },	// crate, in the first pool's way
	{ D3DXVECTOR3(-72, 154, 0), D3DXVECTOR3(-8, 170, 10), 2, { 0.16f, 0.17f, 0.22f } },		// the body, lying across its own pool
	{ D3DXVECTOR3(-196, -34, 0), D3DXVECTOR3(-184, -22, 70), 2, { 0.20f, 0.16f, 0.12f } },	// someone standing in the slab's pool
};

struct Hit { float t; D3DXVECTOR3 n; int cls; float albedo[3]; float rough; };	// cls: 0 nothing, 1 rigid object, 2 actor, 4 land

static Hit Cast(const D3DXVECTOR3& o, const D3DXVECTOR3& d) {
	Hit h = { 1e9f, D3DXVECTOR3(0, 0, 1), 0, { 0, 0, 0 }, 0.8f };
	if (d.y > 0) {
		const float t = (WallY - o.y) / d.y;
		const D3DXVECTOR3 p = o + d * t;
		if (t > 0 && p.z > GroundZ(p.x, p.y)) {
			h.t = t; h.n = D3DXVECTOR3(0, -1, 0); h.cls = 1;
			const float v = 0.42f + 0.18f * Fbm(p.x * 0.05f, p.z * 0.05f);
			h.albedo[0] = v; h.albedo[1] = v * 0.98f; h.albedo[2] = v * 0.95f;
		}
	}
	for (const BoxObj& b : Boxes) {
		float t0 = 0, t1 = 1e9f, sign = 0;
		int axis = -1;
		bool miss = false;
		for (int a = 0; a < 3 && !miss; a++) {
			const float oa = (&o.x)[a], da = (&d.x)[a], lo = (&b.lo.x)[a], hi = (&b.hi.x)[a];
			if (fabsf(da) < 1e-8f) { if (oa < lo || oa > hi) miss = true; continue; }
			float ta = (lo - oa) / da, tb = (hi - oa) / da;
			float s = -1;
			if (ta > tb) { std::swap(ta, tb); s = 1; }
			if (ta > t0) { t0 = ta; axis = a; sign = s; }
			if (tb < t1) t1 = tb;
			if (t0 > t1) miss = true;
		}
		if (!miss && axis >= 0 && t0 > 0 && t0 < h.t) {
			h.t = t0; h.n = D3DXVECTOR3(0, 0, 0); (&h.n.x)[axis] = sign; h.cls = b.cls;
			for (int c = 0; c < 3; c++) h.albedo[c] = b.albedo[c];
		}
	}
	// (Steps as long as the height over the ground allows: its slopes stay under about 2.)
	float prev = 0.0f;
	for (float t = 1.0f, over = 1.0f; t < min(h.t, 6000.0f); t += max(0.25f, over * 0.3f)) {
		const D3DXVECTOR3 p = o + d * t;
		over = p.z - GroundZ(p.x, p.y);
		if (over < 0.0f) {
			float a = prev, b = t;
			for (int i = 0; i < 14; i++) { const float m = 0.5f * (a + b); const D3DXVECTOR3 q = o + d * m; if (q.z < GroundZ(q.x, q.y)) b = m; else a = m; }
			const D3DXVECTOR3 q = o + d * b;
			h.t = b; h.n = GroundNormal(q.x, q.y); h.cls = 4;
			// Sandy ground with darker patches; the slab is pale concrete; rocks are greyer.
			const float v = 0.75f + 0.35f * Fbm(q.x * 0.04f + 9, q.y * 0.04f);
			h.albedo[0] = 0.45f * v; h.albedo[1] = 0.36f * v; h.albedo[2] = 0.26f * v;
			const float pm = PadMask(q.x, q.y);
			const float cv = 0.46f + 0.08f * Fbm(q.x * 0.09f, q.y * 0.09f);
			for (int c = 0; c < 3; c++) h.albedo[c] += (cv - h.albedo[c]) * pm;
			break;
		}
		prev = t;
	}
	return h;
}

struct Camera { D3DXVECTOR3 eye, F, R, U; D3DXMATRIX view, proj, invView, invProj, viewProj; };
static Camera MakeCamera(const D3DXVECTOR3& eye, float yawDeg, float pitchDeg) {
	Camera c;
	c.eye = eye;
	const float yaw = D3DXToRadian(yawDeg), pitch = D3DXToRadian(pitchDeg);
	c.F = D3DXVECTOR3(sinf(yaw) * cosf(pitch), cosf(yaw) * cosf(pitch), -sinf(pitch));
	D3DXVECTOR3 Up(0, 0, 1);
	D3DXVec3Cross(&c.R, &c.F, &Up); D3DXVec3Normalize(&c.R, &c.R);
	D3DXVec3Cross(&c.U, &c.R, &c.F);
	D3DXMatrixIdentity(&c.view);
	c.view._11 = c.R.x; c.view._12 = c.U.x; c.view._13 = c.F.x;
	c.view._21 = c.R.y; c.view._22 = c.U.y; c.view._23 = c.F.y;
	c.view._31 = c.R.z; c.view._32 = c.U.z; c.view._33 = c.F.z;
	D3DXMatrixTranspose(&c.invView, &c.view);
	const float fovY = D3DXToRadian(55.0f), aspect = (float)W / H;
	D3DXMatrixIdentity(&c.proj);
	c.proj._11 = 1.0f / (tanf(fovY * 0.5f) * aspect); c.proj._22 = 1.0f / tanf(fovY * 0.5f);
	c.proj._33 = FarZ / (FarZ - NearZ); c.proj._34 = 1.0f; c.proj._43 = -NearZ * FarZ / (FarZ - NearZ); c.proj._44 = 0.0f;
	D3DXMatrixInverse(&c.invProj, NULL, &c.proj);
	c.viewProj = c.view * c.proj;
	return c;
}

// A source of blood in the preview: what BloodFlow.cpp would gather for it.
struct Source { const char* name; bool pool; float x, y, z, radius; D3DXVECTOR3 normal; float start; bool old; float seed; };

int main(int argc, char** argv) {
	if (argc < 3) { std::puts("usage: blood_surface_preview <BloodFlow.fx.hlsl> <output folder> [w=1280] [h=720] [cost] [quick]"); return 2; }
	const std::string outDir = argv[2];
	setvbuf(stdout, NULL, _IONBF, 0);
	bool costOnly = false, quick = false, orbit = false, many = false;
	for (int i = 3; i < argc; i++) {
		sscanf_s(argv[i], "w=%d", &W); sscanf_s(argv[i], "h=%d", &H);
		costOnly |= !strcmp(argv[i], "cost");
		quick |= !strcmp(argv[i], "quick");
		orbit |= !strcmp(argv[i], "orbit");
		many |= !strcmp(argv[i], "many");
	}
	for (BoxObj& b : Boxes) {
		const float g = GroundZ(0.5f * (b.lo.x + b.hi.x), 0.5f * (b.lo.y + b.hi.y));
		b.hi.z = g + b.hi.z; b.lo.z = g - 6.0f;
	}

	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "BloodSurfacePreview"; wc.hInstance = GetModuleHandleA(NULL);
	RegisterClassA(&wc);
	HWND window = CreateWindowA("BloodSurfacePreview", "", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 640; pp.BackBufferHeight = 360;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window; pp.EnableAutoDepthStencil = FALSE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	IDirect3DDevice9* device = nullptr;
	CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
	D3DADAPTER_IDENTIFIER9 adapter = {};
	d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter);
	std::printf("adapter: %s; picture %d x %d\n", adapter.Description, W, H);

	const D3DXVECTOR3 sun = [] { D3DXVECTOR3 s(0.35f, -0.65f, 0.68f); D3DXVec3Normalize(&s, &s); return s; }();
	const float sunColour[3] = { 1.0f, 0.92f, 0.8f }, ambientDay[3] = { 0.16f, 0.17f, 0.2f };
	const float ambientNight[3] = { 0.012f, 0.014f, 0.022f };
	const D3DXVECTOR3 lampAt(-150.0f, -60.0f, GroundZ(-150, -60) + 60.0f);
	const float lampColour[3] = { 1.0f, 0.72f, 0.42f }, lampReach = 260.0f;

	IDirect3DTexture9 *depthTex, *normalTex, *shadowTex, *viewModelTex, *imageTex, *sourceTex, *sceneTex, *albedoTex, *materialTex, *atlasTex[2], *atlasRead;
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &depthTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &normalTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_G16R16, D3DPOOL_MANAGED, &shadowTex, NULL));
	CHECK(device->CreateTexture(4, 4, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &viewModelTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &imageTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &sourceTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sceneTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &albedoTex, NULL));
	CHECK(device->CreateTexture(W, H, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &materialTex, NULL));
	for (int i = 0; i < 2; i++) CHECK(device->CreateTexture(BloodSurface::AtlasWidth, BloodSurface::AtlasHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &atlasTex[i], NULL));
	CHECK(device->CreateTexture(BloodSurface::AtlasWidth, BloodSurface::AtlasHeight, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &atlasRead, NULL));
	IDirect3DSurface9 *image, *source, *scene, *atlasSurface[2], *atlasReadSurface, *imageRead;
	imageTex->GetSurfaceLevel(0, &image);
	sourceTex->GetSurfaceLevel(0, &source);
	sceneTex->GetSurfaceLevel(0, &scene);
	atlasRead->GetSurfaceLevel(0, &atlasReadSurface);
	for (int i = 0; i < 2; i++) { atlasTex[i]->GetSurfaceLevel(0, &atlasSurface[i]); device->ColorFill(atlasSurface[i], NULL, 0); }
	CHECK(device->CreateOffscreenPlainSurface(W, H, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &imageRead, NULL));
	D3DLOCKED_RECT lr;
	CHECK(viewModelTex->LockRect(0, &lr, NULL, 0));
	for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((float*)((char*)lr.pBits + y * lr.Pitch))[x] = 1.0f;	// no weapon in hand
	viewModelTex->UnlockRect(0);

	// The scene from a camera, as NVR's buffers. night: no sun, a lamp over the slab. Kept on the CPU too (classes, shadow).
	std::vector<unsigned char> pixelClass(W * H);
	std::vector<float> pixelSun(W * H), sceneRGB(W * H * 3), pixelXY(W * H * 2);
	auto renderScene = [&](const Camera& cam, bool night) -> int {
		std::vector<float> depth(W * H), normals(W * H * 4), colour(W * H * 4), albedos(W * H * 4), materials(W * H * 4);
		std::vector<unsigned short> shadow(W * H * 2);
		// (The rays on every core: a row each.)
		std::vector<Hit> hits(W * H);
		{
			std::vector<std::thread> workers;
			std::atomic<int> nextRow(0);
			for (unsigned t = 0; t < max(1u, std::thread::hardware_concurrency()); t++) workers.emplace_back([&] {
				for (int py = nextRow++; py < H; py = nextRow++) for (int px = 0; px < W; px++) {
					const float nx = (px + 0.5f) / W * 2 - 1, ny = 1 - (py + 0.5f) / H * 2;
					hits[py * W + px] = Cast(cam.eye, cam.F + cam.R * (nx / cam.proj._11) + cam.U * (ny / cam.proj._22));
				}
			});
			for (auto& w : workers) w.join();
		}
		for (int py = 0; py < H; py++) for (int px = 0; px < W; px++) {
			const float nx = (px + 0.5f) / W * 2 - 1, ny = 1 - (py + 0.5f) / H * 2;
			const D3DXVECTOR3 dir = cam.F + cam.R * (nx / cam.proj._11) + cam.U * (ny / cam.proj._22);
			const Hit& h = hits[py * W + px];
			const int i = py * W + px;
			pixelClass[i] = (unsigned char)h.cls; pixelSun[i] = 0.0f;
			colour[i * 4 + 3] = 1.0f;
			if (h.cls == 0) {
				depth[i] = 1.0f;
				const float sky[3] = { night ? 0.01f : 0.5f, night ? 0.012f : 0.6f, night ? 0.02f : 0.8f };
				for (int c = 0; c < 3; c++) { colour[i * 4 + c] = sky[c]; sceneRGB[i * 3 + c] = ToLinear(sky[c]); }
				continue;
			}
			const D3DXVECTOR3 p = cam.eye + dir * h.t;
			depth[i] = h.t / FarZ;	// dir's forward component is 1: t is the view depth
			normals[i * 4 + 0] = D3DXVec3Dot(&h.n, &cam.R) * 0.5f + 0.5f;
			normals[i * 4 + 1] = D3DXVec3Dot(&h.n, &cam.U) * 0.5f + 0.5f;
			normals[i * 4 + 2] = D3DXVec3Dot(&h.n, &cam.F) * 0.5f + 0.5f;
			normals[i * 4 + 3] = 1.0f;
			const float share = night ? 0.0f : SunShare(p.x, p.y);
			pixelSun[i] = share;
			pixelXY[i * 2] = p.x; pixelXY[i * 2 + 1] = p.y;
			shadow[i * 2] = (unsigned short)(share * 65535.0f + 0.5f); shadow[i * 2 + 1] = 0;
			float light[3];
			if (night) {
				const D3DXVECTOR3 toLamp = lampAt - p;
				const float s = min(D3DXVec3LengthSq(&toLamp) / (lampReach * lampReach), 1.0f);
				D3DXVECTOR3 l; D3DXVec3Normalize(&l, &toLamp);
				const float atten = (1 - s) * (1 - s) / (1 + 5 * s) * max(0.0f, D3DXVec3Dot(&h.n, &l)) * 2.2f;
				for (int c = 0; c < 3; c++) light[c] = ambientNight[c] + lampColour[c] * atten;
			}
			else for (int c = 0; c < 3; c++) light[c] = ambientDay[c] + 1.15f * sunColour[c] * share * max(0.0f, D3DXVec3Dot(&h.n, &sun));
			for (int c = 0; c < 3; c++) {
				sceneRGB[i * 3 + c] = h.albedo[c] * light[c];
				colour[i * 4 + c] = ToGamma(h.albedo[c] * light[c]);	// (the picture holds display-curve values, as the game's)
				albedos[i * 4 + c] = h.albedo[c];
			}
			albedos[i * 4 + 3] = 1.0f;
			materials[i * 4 + 0] = (float)h.cls; materials[i * 4 + 1] = 30.0f; materials[i * 4 + 2] = h.rough; materials[i * 4 + 3] = 1.0f;
		}
		D3DLOCKED_RECT l;
		CHECK(depthTex->LockRect(0, &l, NULL, 0));
		for (int y = 0; y < H; y++) memcpy((char*)l.pBits + y * l.Pitch, &depth[y * W], W * 4);
		depthTex->UnlockRect(0);
		CHECK(shadowTex->LockRect(0, &l, NULL, 0));
		for (int y = 0; y < H; y++) memcpy((char*)l.pBits + y * l.Pitch, &shadow[y * W * 2], W * 4);
		shadowTex->UnlockRect(0);
		struct { IDirect3DTexture9* tex; std::vector<float>* data; } halves[] = { { normalTex, &normals }, { sceneTex, &colour }, { albedoTex, &albedos }, { materialTex, &materials } };
		for (auto& hf : halves) {
			CHECK(hf.tex->LockRect(0, &l, NULL, 0));
			for (int y = 0; y < H; y++) D3DXFloat32To16Array((D3DXFLOAT16*)((char*)l.pBits + y * l.Pitch), &(*hf.data)[y * W * 4], W * 4);
			hf.tex->UnlockRect(0);
		}
		return 0;
	};

	// The effect. Compiling all of BloodFlow.fx.hlsl takes minutes here (its older passes are long), so unless `full` is given
	// the file's own technique blocks are left out of the text first (the Surface techniques are in the include at its end, and
	// every function, constant and sampler stays as it is): the same Surface shaders, compiled in seconds.
	bool full = false;
	for (int i = 3; i < argc; i++) full |= !strcmp(argv[i], "full");
	std::string effectDir = argv[1];
	effectDir = effectDir.substr(0, effectDir.find_last_of("\\/") + 1);
	struct Includes : ID3DXInclude {
		std::string dir;
		STDMETHOD(Open)(D3DXINCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
			FILE* f = nullptr;
			if (fopen_s(&f, (dir + name).c_str(), "rb") || !f) return E_FAIL;
			fseek(f, 0, SEEK_END);
			const long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			char* text = new char[size > 0 ? size : 1];
			fread(text, 1, size, f);
			fclose(f);
			*data = text; *bytes = (UINT)size;
			return S_OK;
		}
		STDMETHOD(Close)(LPCVOID data) override { delete[] (const char*)data; return S_OK; }
	} includes;
	includes.dir = effectDir;
	ID3DXEffect* effect = nullptr;
	ID3DXBuffer* errors = nullptr;
	const DWORD compileStart = GetTickCount();
	if (full) {
		if (FAILED(D3DXCreateEffectFromFileA(device, argv[1], NULL, NULL, 0, NULL, &effect, &errors))) {
			std::printf("effect failed: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
			return 1;
		}
	}
	else {
		std::string text;
		{
			FILE* f = nullptr;
			if (fopen_s(&f, argv[1], "rb") || !f) { std::printf("cannot read %s\n", argv[1]); return 1; }
			char buffer[65536];
			size_t got;
			while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
			fclose(f);
		}
		int cut = 0;
		for (size_t at = text.find("\ntechnique "); at != std::string::npos; at = text.find("\ntechnique ", at)) {
			size_t open = text.find('{', at), end = open;
			int depth = 0;
			for (; end < text.size(); end++) { if (text[end] == '{') depth++; else if (text[end] == '}' && --depth == 0) break; }
			if (open == std::string::npos || end >= text.size()) break;
			text.erase(at + 1, end - at);
			cut++;
		}
		if (FAILED(D3DXCreateEffect(device, text.data(), (UINT)text.size(), NULL, &includes, 0, NULL, &effect, &errors))) {
			std::printf("effect failed: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
			return 1;
		}
		std::printf("(%d older technique blocks left out of the compile; `full` compiles the file as the game does)\n", cut);
	}
	std::printf("effect compiled in %.1f s\n", (GetTickCount() - compileStart) / 1000.0);
	if (!effect->GetTechniqueByName("BloodSurfaceSim") || !effect->GetTechniqueByName("BloodSurface")) { std::puts("the effect has no Surface techniques"); return 1; }
	IDirect3DVolumeTexture9* noiseTex = nullptr;
	IDirect3DTexture9* splatterTex = nullptr;
	if (getenv("BLOOD_SPLATTER")) D3DXCreateTextureFromFileA(device, getenv("BLOOD_SPLATTER"), &splatterTex);
	std::printf("splatter decals: %s\n", splatterTex ? "loaded (splats take the decal's shape)" : "none (drawn splat shapes)");
	{
		std::string tree = argv[1];
		const size_t at = tree.find("src\\hlsl");
		tree = at == std::string::npos ? std::string() : tree.substr(0, at);
		const std::string noise = tree + "resource\\Textures\\Effects\\NoiseVolume.dds";
		if (FAILED(D3DXCreateVolumeTextureFromFileA(device, noise.c_str(), &noiseTex))) { std::printf("noise texture not found: %s\n", noise.c_str()); return 1; }
	}
	auto vec = [&](const char* name, float x, float y, float z, float w) { D3DXVECTOR4 v(x, y, z, w); effect->SetVector(name, &v); };
	auto lin = [](float c) { return ToLinear(c); };
	// The defaults in VegasShaders.defaults.toml ([Shaders.BloodFlow.Main] and [Shaders.BloodFlow.Surface]).
	BloodSurface::Settings settings;
	settings.Enabled = true;
	for (int i = 3; i < argc; i++) {
		sscanf_s(argv[i], "visc=%f", &settings.Viscosity); sscanf_s(argv[i], "rough=%f", &settings.Roughness);
		sscanf_s(argv[i], "flow=%f", &settings.FlowTime); sscanf_s(argv[i], "pool=%f", &settings.PoolAmount); sscanf_s(argv[i], "splat=%f", &settings.SplatAmount);
	}
	const float maxDistance = 3000.0f;
	vec("TESR_ReciprocalResolution", 1.0f / W, 1.0f / H, (float)W / H, 0);
	vec("TESR_RenderScale", 1, 1, 1, 1);
	vec("TESR_BloodFlowFresh", lin(0.42f), lin(0.03f), lin(0.028f), 0.5f);
	vec("TESR_BloodFlowDry", lin(0.24f), lin(0.085f), lin(0.065f), 120.0f);
	vec("TESR_BloodFlowPool", 25.0f, 40.0f, 200.0f, 687.0f);
	vec("TESR_BloodFlowProbe", 1.0f / 576.0f, 0.8f, 576.0f, splatterTex ? 1.0f : 0.0f);
	vec("TESR_BloodFlowMatch", 0.12f, 1.0f, 1.0f, 0.0f);
	vec("TESR_BloodFlowDryRatio", lin(0.24f) / lin(0.42f), lin(0.085f) / lin(0.03f), lin(0.065f) / lin(0.028f), 0.0f);
	vec("TESR_BloodFlowLook", 0.45f, 0.25f, 0.6f, lin(0.2f));
	vec("TESR_BloodFlowFlow", 1.0f, 1.0f, 0.75f, 0.0f);
	vec("TESR_BloodFlowSplat", 1.0f, 1.0f, 1.0f, 1.0f);
	vec("TESR_DepthConstants", 0, 0, 0, 1);
	vec("TESR_CameraData", NearZ, FarZ, 0, 0);
	{ const D3DXVECTOR4 size = BloodSurface::SizeConstants(); effect->SetVector("TESR_BloodSurfaceSize", &size); }
	vec("TESR_MaterialBufferData", 1, 0, 0, 0);
	auto lighting = [&](bool night) {
		vec("TESR_SunDirection", sun.x, sun.y, sun.z, 1);
		if (night) {
			vec("TESR_SunColor", 0, 0, 0, 1);
			vec("TESR_SunAmbient", ambientNight[0], ambientNight[1], ambientNight[2], 1);
			vec("TESR_HorizonColor", 0.02f, 0.025f, 0.04f, 1);
			D3DXVECTOR4 lamps[12] = {}, colours[24] = {};
			lamps[0] = D3DXVECTOR4(lampAt.x, lampAt.y, lampAt.z, lampReach);
			colours[12] = D3DXVECTOR4(lampColour[0], lampColour[1], lampColour[2], 1);
			effect->SetVectorArray("TESR_LightPosition", lamps, 12);
			effect->SetVectorArray("TESR_LightColor", colours, 24);
		}
		else {
			vec("TESR_SunColor", sunColour[0], sunColour[1], sunColour[2], 1);
			vec("TESR_SunAmbient", 0.35f, 0.33f, 0.3f, 1);
			vec("TESR_HorizonColor", 0.62f, 0.68f, 0.78f, 1);
			D3DXVECTOR4 lamps[12] = {}, colours[24] = {};
			effect->SetVectorArray("TESR_LightPosition", lamps, 12);
			effect->SetVectorArray("TESR_LightColor", colours, 24);
		}
		D3DXVECTOR4 none[12] = {};
		effect->SetVectorArray("TESR_ShadowLightPosition", none, 12);
	};
	lighting(false);

	const D3DVERTEXELEMENT9 elements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
		{ 0, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
		{ 0, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
		D3DDECL_END() };
	IDirect3DVertexDeclaration9* declaration;
	CHECK(device->CreateVertexDeclaration(elements, &declaration));
	static VolumetricSmokeQuads::Vertex simVertices[BloodSurface::Cells * 6], boxVertices[BloodSurface::Cells * 36];

	// The blood: what BloodFlow.cpp would gather.
	auto slopeAt = [](float x, float y, float& sx, float& sy) { sx = (GroundZ(x + 12, y) - GroundZ(x - 12, y)) / 24.0f; sy = (GroundZ(x, y + 12) - GroundZ(x, y - 12)) / 24.0f; };
	std::vector<Source> sources = {
		{ "pool on the slope (above the rocks and the crack)", true, -40, 162, 0, 30, D3DXVECTOR3(0, 0, 1), 0.0f, false, 0.31f },
		{ "pool on the flat slab", true, -190, -30, 0, 26, D3DXVECTOR3(0, 0, 1), 0.0f, false, 0.77f },
		{ "pool beside the hollow, on the shadow's edge", true, 66, 40, 0, 26, D3DXVECTOR3(0, 0, 1), 0.0f, false, 0.52f },
		{ "splat on the steep part", false, 150, 118, 0, 20, D3DXVECTOR3(0.05f, -0.05f, -1), 0.0f, false, 0.18f },
		{ "splat on the wall", false, 20, WallY, 0, 18, D3DXVECTOR3(0.1f, 1, 0.05f), 0.0f, false, 0.64f },
		{ "old body's pool (was dead before it was seen)", true, 150, -45, 0, 24, D3DXVECTOR3(0, 0, 1), 0.0f, true, 0.93f },
		{ "splat low on the wall (runs down to its foot)", false, -70, WallY, 16, 16, D3DXVECTOR3(-0.1f, 1, 0.05f), 0.0f, false, 0.41f },
	};
	if (many) {
		sources.clear();
		for (int i = 0; i < 32; i++)
			sources.push_back({ "pool", true, -235.0f + 67.0f * (i % 8), -70.0f + 85.0f * (i / 8), 0, 24, D3DXVECTOR3(0, 0, 1), 0.0f, false, Hash((float)i, 3.0f) });
	}
	for (Source& s : sources) s.z = s.pool || fabsf(s.normal.z) > 0.7f ? GroundZ(s.x, s.y) - (s.pool ? 1.5f : 0.0f) : GroundZ(s.x, WallY) + (s.z > 0.0f ? s.z : 50.0f);

	BloodSurface::Atlas atlas;
	double now = 0.0;
	bool useAlbedo = true;
	float debugView = 0.0f;
	bool simOn = true, drawOn = true, uploadScene = true, measureOn = true;
	for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "nomeasure")) measureOn = false;	// (frozen patches keep their whole box: to compare the cost)
	IDirect3DSurface9 *measureTarget, *measureRead;
	CHECK(device->CreateRenderTarget(BloodSurface::CellSize, BloodSurface::CellSize, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &measureTarget, NULL));
	CHECK(device->CreateOffscreenPlainSurface(BloodSurface::CellSize, BloodSurface::CellSize, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &measureRead, NULL));
	auto bind = [&](IDirect3DTexture9* state) {
		// NVR binds each TESR_ texture straight to its sampler's register (EffectRecord::SetCT); here after BeginPass (D3DX unbinds).
		device->SetTexture(0, depthTex); device->SetTexture(1, sourceTex); device->SetTexture(2, normalTex);
		device->SetTexture(3, shadowTex); device->SetTexture(4, viewModelTex); device->SetTexture(8, noiseTex); device->SetTexture(9, splatterTex);
		device->SetTexture(13, state); device->SetTexture(14, albedoTex); device->SetTexture(15, materialTex);
	};
	// One frame of `seconds`, from `cam`, as BloodFlowEffect::Render: offers, the steps, the drawing.
	auto frame = [&](const Camera& cam, float seconds) -> int {
		now += seconds;
		vec("TESR_CameraPosition", cam.eye.x, cam.eye.y, cam.eye.z, 1);
		effect->SetMatrix("TESR_ViewTransform", &cam.view);
		effect->SetMatrix("TESR_InvViewTransform", &cam.invView);
		effect->SetMatrix("TESR_ProjectionTransform", &cam.proj);
		effect->SetMatrix("TESR_InvProjectionTransform", &cam.invProj);
		effect->SetMatrix("TESR_ViewProjectionTransform", &cam.viewProj);
		vec("TESR_BloodFlowShape", 0, 0, 0, 0);
		vec("TESR_BloodFlowInfo", 1.0f, 0.0f, 300.0f, 1.0f);
		vec("TESR_AlbedoBufferData", useAlbedo ? 1.0f : 0.0f, 0, 0, 0);
		settings.DebugView = (int)debugView;
		{ const D3DXVECTOR4 flow = BloodSurface::FlowConstants(settings), look = BloodSurface::LookConstants(settings); effect->SetVector("TESR_BloodSurfaceFlow", &flow); effect->SetVector("TESR_BloodSurfaceLook", &look); }
		for (size_t i = 0; i < sources.size(); i++) {
			const Source& s = sources[i];
			if (now < s.start) continue;
			BloodFlowBoxes::Item it = {};
			it.Center = D3DXVECTOR3(s.x, s.y, s.z); it.Normal = s.normal; D3DXVec3Normalize(&it.Normal, &it.Normal);
			it.Radius = s.radius; it.Seed = s.seed; it.Fade = 1.0f; it.Pool = s.pool; it.Slot = (int)i;
			it.Age = s.old ? 1e5f : (float)(now - s.start);
			if (s.pool) slopeAt(s.x, s.y, it.SlopeX, it.SlopeY);
			atlas.Offer(1000 + i, it, cam.eye, now, settings);
		}
		atlas.Update(cam.eye, D3DXVECTOR3(0, 0, 0), cam.viewProj, now, settings);
		CHECK(device->BeginScene());
		UINT n = 0;
		if (simOn && seconds > 0.0f) {
			int normal = 0;
			const int pairs = atlas.Pairs(seconds, normal);
			for (int i = 0; i < BloodSurface::Cells && normal > 0; i++) {
				const BloodSurface::Patch& p = atlas.patches[i];
				if (!p.Used || !p.Reset || !p.Seen) continue;
				RECT r;
				BloodSurface::Atlas::CellRect(i, r);
				for (int k = 0; k < 2; k++) device->ColorFill(atlasSurface[k], &r, 0);
			}
			effect->SetTechnique("BloodSurfaceSim");
			for (int pair = 0; pair < pairs; pair++) {
				const int kind = BloodSurface::Atlas::Kind(pair, normal);
				bool drew = false;
				for (int k = 0; k < 2; k++) {
					const int triangles = atlas.BuildSim(kind, k, cam.eye, simVertices);
					if (!triangles) continue;
					drew = true;
					{ const D3DXVECTOR4 simConstants = BloodSurface::SimConstants(settings, BloodSurface::Atlas::StepOf(kind), kind == 0 && k == 0); effect->SetVector("TESR_BloodSurfaceSim", &simConstants); }
					CHECK(device->SetRenderTarget(0, atlasSurface[1 - k]));
					CHECK(effect->Begin(&n, 0));
					CHECK(effect->BeginPass(0));
					bind(atlasTex[k]);
					device->SetVertexDeclaration(declaration);
					CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, simVertices, sizeof(VolumetricSmokeQuads::Vertex)));
					effect->EndPass();
					effect->End();
					device->SetTexture(13, NULL);
				}
				if (drew) atlas.AfterPair(kind, now);
			}
		}
		// A frozen patch's blood is measured once (as BloodFlow.cpp RenderSurface; here every patch at once).
		for (int cell = atlas.NextToMeasure(); cell >= 0 && measureOn; cell = atlas.NextToMeasure()) {
			RECT r;
			BloodSurface::Atlas::CellRect(cell, r);
			D3DLOCKED_RECT l;
			CHECK(device->StretchRect(atlasSurface[0], &r, measureTarget, NULL, D3DTEXF_NONE));
			CHECK(device->GetRenderTargetData(measureTarget, measureRead));
			CHECK(measureRead->LockRect(&l, NULL, D3DLOCK_READONLY));
			atlas.Measure(cell, l.pBits, l.Pitch);
			measureRead->UnlockRect();
			const BloodSurface::Patch& p = atlas.patches[cell];
			std::printf("patch %d frozen at %.1f s: %s, box %.0f x %.0f x %.0f of %.0f x %.0f x %.0f units\n", cell, p.SimAge, p.Empty ? "empty" : "measured",
				p.Box[3] - p.Box[0], p.Box[4] - p.Box[1], p.Box[5] - p.Box[2], 2 * p.Half, 2 * p.Half, 2 * p.Half * BloodSurface::BoxUp);
		}
		if (uploadScene) CHECK(device->UpdateSurface(scene, NULL, image, NULL));
		if (drawOn) {
			float bounds[4];
			const int triangles = atlas.BuildBoxes(cam.eye, D3DXVECTOR3(0, 0, 0), cam.viewProj, now, maxDistance, settings, boxVertices, bounds);
			if (triangles) {
				RECT area = { (LONG)max(0.0f, floorf(min(max(bounds[0], 0.0f), 1.0f) * W) - 2.0f), (LONG)max(0.0f, floorf(min(max(bounds[1], 0.0f), 1.0f) * H) - 2.0f),
					(LONG)min((float)W, ceilf(min(max(bounds[2], 0.0f), 1.0f) * W) + 2.0f), (LONG)min((float)H, ceilf(min(max(bounds[3], 0.0f), 1.0f) * H) + 2.0f) };
				if (area.right > area.left && area.bottom > area.top) {
					CHECK(device->StretchRect(image, &area, source, &area, D3DTEXF_NONE));
					CHECK(device->SetRenderTarget(0, image));
					effect->SetTechnique("BloodSurface");
					CHECK(effect->Begin(&n, 0));
					CHECK(effect->BeginPass(0));
					bind(atlasTex[0]);
					device->SetVertexDeclaration(declaration);
					CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, boxVertices, sizeof(VolumetricSmokeQuads::Vertex)));
					effect->EndPass();
					effect->End();
				}
			}
		}
		device->EndScene();
		return 0;
	};
	auto save = [&](const char* name) -> int {
		const std::string path = outDir + "\\" + name + ".png";
		CHECK(D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, image, NULL, NULL));
		std::printf("%s\n", path.c_str());
		return 0;
	};
	// Waits for the GPU to finish everything sent so far.
	IDirect3DQuery9* done = nullptr;
	device->CreateQuery(D3DQUERYTYPE_EVENT, &done);
	auto finish = [&]() { if (!done) return; done->Issue(D3DISSUE_END); while (done->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {} };
	// The atlas's first copy, read back: r thickness, g dryness, b height, a known.
	std::vector<float> state(BloodSurface::AtlasWidth * BloodSurface::AtlasHeight * 4);
	auto readState = [&]() -> int {
		CHECK(device->GetRenderTargetData(atlasSurface[0], atlasReadSurface));
		D3DLOCKED_RECT l;
		CHECK(atlasReadSurface->LockRect(&l, NULL, D3DLOCK_READONLY));
		for (int y = 0; y < BloodSurface::AtlasHeight; y++)
			D3DXFloat16To32Array(&state[y * BloodSurface::AtlasWidth * 4], (D3DXFLOAT16*)((char*)l.pBits + y * l.Pitch), BloodSurface::AtlasWidth * 4);
		atlasReadSurface->UnlockRect();
		return 0;
	};
	auto texel = [&](int cell, int tx, int ty) { return &state[(((cell / BloodSurface::CellsPerRow) * BloodSurface::CellSize + ty) * BloodSurface::AtlasWidth + (cell % BloodSurface::CellsPerRow) * BloodSurface::CellSize + tx) * 4]; };
	// A texel's world position (on the patch's plane).
	auto texelWorld = [&](const BloodSurface::Patch& p, int tx, int ty) {
		D3DXVECTOR3 U, V;
		BloodSurface::Axes(p.Normal, U, V);
		const float u = ((tx + 0.5f) / BloodSurface::CellSize * 2 - 1) * p.Half, v = ((ty + 0.5f) / BloodSurface::CellSize * 2 - 1) * p.Half;
		return p.Center + U * u + V * v;
	};
	std::vector<float> picture(W * H * 4);
	auto readPicture = [&]() -> int {
		CHECK(device->GetRenderTargetData(image, imageRead));
		D3DLOCKED_RECT l;
		CHECK(imageRead->LockRect(&l, NULL, D3DLOCK_READONLY));
		for (int y = 0; y < H; y++) D3DXFloat16To32Array(&picture[y * W * 4], (D3DXFLOAT16*)((char*)l.pBits + y * l.Pitch), W * 4);
		imageRead->UnlockRect();
		return 0;
	};

	const Camera camA = MakeCamera(D3DXVECTOR3(-20, -185, GroundZ(-20, -185) + 200), 0.0f, 41.0f);
	if (renderScene(camA, false)) return 1;
	const float frameSeconds = 1.0f / 60.0f;

	if (costOnly) {
		// The GPU cost at this size: the steps and the drawing with every patch running, then the drawing alone once frozen.
		uploadScene = false;
		CHECK(device->UpdateSurface(scene, NULL, image, NULL));
		for (int f = 0; f < 60 * 6; f++) if (frame(camA, frameSeconds)) return 1;	// six seconds in: blood on the ground everywhere
		LARGE_INTEGER freq, t0, t1;
		QueryPerformanceFrequency(&freq);
		const int frames = 300;
		auto timed = [&](const char* what, bool sim, bool draw) -> int {
			simOn = sim; drawOn = draw;
			const double keep = now;
			BloodSurface::Atlas kept = atlas;
			for (int f = 0; f < 30; f++) if (frame(camA, frameSeconds)) return 1;
			finish();
			QueryPerformanceCounter(&t0);
			for (int f = 0; f < frames; f++) if (frame(camA, frameSeconds)) return 1;
			finish();
			QueryPerformanceCounter(&t1);
			std::printf("COST %s: %.3f ms a frame (%d patches, %d running)\n", what, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart / frames, atlas.Count(), atlas.Running());
			if (!sim) { atlas = kept; now = keep; }
			return 0;
		};
		if (timed("nothing (the loop itself)", false, false)) return 1;
		if (timed("drawing only, patches wet", false, true)) return 1;
		if (timed("steps + drawing, every patch running", true, true)) return 1;
		simOn = drawOn = true;
		for (int f = 0; f < 60 * 45 && atlas.Running() > 0; f++) if (frame(camA, frameSeconds)) return 1;
		if (timed("nothing (the loop itself)", false, false)) return 1;
		if (timed("frozen: drawing only", true, true)) return 1;
		return 0;
	}

	// ---- The time-lapse from the overview camera ------------------------------------------------------------------
	CHECK(device->UpdateSurface(scene, NULL, image, NULL));
	if (save("0_scene_before")) return 1;
	const float stops[] = { 1.0f, 3.0f, 8.0f, 16.0f, 34.0f };
	int stop = 0;
	const float endTime = quick ? 8.0f : 40.0f;
	// (orbit: three more places the camera looks from in turn, back at the overview for the last five seconds.)
	const Camera orbitCams[] = { camA, MakeCamera(D3DXVECTOR3(-170, -150, GroundZ(-170, -150) + 160), 30.0f, 36.0f),
		MakeCamera(D3DXVECTOR3(140, -160, GroundZ(140, -160) + 180), -24.0f, 38.0f), MakeCamera(D3DXVECTOR3(-30, -40, GroundZ(-30, -40) + 120), 0.0f, 38.0f) };
	int camIndex = 0;
	for (int f = 1; now < endTime; f++) {
		const int want = orbit && now < endTime - 5.0f ? (int)(now / 2.5) % 4 : 0;
		if (want != camIndex) { camIndex = want; if (renderScene(orbitCams[camIndex], false)) return 1; }
		if (frame(orbitCams[camIndex], frameSeconds)) return 1;
		if (stop < 5 && now >= stops[stop] - 1e-3) {
			char name[64];
			std::snprintf(name, sizeof(name), "1_overview_%02ds", (int)stops[stop]);
			if (save(name)) return 1;
			stop++;
		}
	}
	std::printf("after %.0f s: %d patches, %d still running (made %d)\n", now, atlas.Count(), atlas.Running(), atlas.made);
	if (save("1_overview_end")) return 1;
	debugView = 1.0f;
	simOn = false;
	if (frame(camA, 0.0f)) return 1;
	if (save("2_overview_debug_known")) return 1;
	debugView = 2.0f;
	if (frame(camA, 0.0f)) return 1;
	if (save("2_overview_debug_heights")) return 1;
	debugView = 0.0f;
	useAlbedo = false;
	if (frame(camA, 0.0f)) return 1;
	if (save("3_overview_no_albedo_record")) return 1;
	useAlbedo = true;
	if (frame(camA, 0.0f)) return 1;
	simOn = true;

	// ---- The checks ----------------------------------------------------------------------------------------------
	if (readState()) return 1;
	if (readPicture()) return 1;
	int failed = 0;
	const float wetAt = 0.012f;
	for (int c = 0; c < BloodSurface::Cells; c++) {
		const BloodSurface::Patch& p = atlas.patches[c];
		if (!p.Used) continue;
		const int N = BloodSurface::CellSize;
		const float dx = 2.0f * p.Half / N;
		double volume = 0, area = 0, cx = 0, cy = 0, known = 0, guessed = 0, tall = 0;
		for (int ty = 0; ty < N; ty++) for (int tx = 0; tx < N; tx++) {
			const float* s = texel(c, tx, ty);
			volume += s[0] * dx * dx;
			known += s[3] > 0.99f; guessed += s[3] > 0.4f && s[3] < 0.99f; tall += s[3] > 0.2f && s[3] < 0.4f;
			if (s[0] > wetAt) { area += dx * dx; cx += tx; cy += ty; }
		}
		const double wet = area / (dx * dx);
		if (wet > 0) { cx /= wet; cy /= wet; }
		// How round the wet shape is: its edge's distance from its middle in 72 directions (1 = a disc: smallest over largest),
		// and its area over the area of the disc that would hold it.
		float rMin = 1e9f, rMax = 0.0f;
		for (int a = 0; a < 72; a++) {
			const float ax = cosf(a * 6.2832f / 72), ay = sinf(a * 6.2832f / 72);
			float reach = 0.0f;
			for (float r = 0; r < N; r += 0.5f) {
				const int tx = (int)(cx + ax * r), ty = (int)(cy + ay * r);
				if (tx < 0 || ty < 0 || tx >= N || ty >= N) break;
				if (texel(c, tx, ty)[0] > wetAt) reach = r;
			}
			rMin = min(rMin, reach); rMax = max(rMax, reach);
		}
		const float roundness = rMax > 0 ? rMin / rMax : 0.0f, fill = rMax > 0 ? (float)(wet / (3.14159f * rMax * rMax)) : 0.0f;
		std::printf("PATCH %d (%s, half %.0f, %.2f units a texel): blood %.0f units^3 over %.0f units^2; ground read on %.0f %%, guessed %.0f %%, tall %.0f %% of texels; edge nearest/furthest from its middle %.2f, fills %.0f %% of its disc; %s, simulated %.1f s\n",
			c, p.Pool ? (p.Old ? "old pool" : "pool") : (p.Normal.z > 0.5f ? "floor splat" : "wall splat"), p.Half, dx, volume, area,
			100.0 * known / (N * N), 100.0 * guessed / (N * N), 100.0 * tall / (N * N), roundness, fill * 100.0f, p.Frozen ? "frozen" : "running", p.SimAge);
		if (c == 1) {	// the pool on the flat slab: the old mode's "perfect circle" case
			const bool ok = roundness < 0.8f && area > 200.0;
			std::printf("CHECK no circular pool on flat ground: nearest/furthest edge %.2f (a disc: 1.00; pass under 0.80), area %.0f: %s\n", roundness, area, ok ? "PASS" : "FAIL");
			failed += !ok;
		}
		if (c == 0) {
			// Into the crack: of the patch's texels below the pool that hold blood, how thick the blood is in the crack against
			// beside it; and the share of the crack's own texels (in the patch) that are wet.
			double inCrack = 0, inCrackN = 0, beside = 0, besideN = 0, crackTexels = 0, crackWet = 0;
			// Round a rock: its top dry, blood on both sides of it.
			double top = 0, topN = 0, left = 0, right = 0;
			const float* rock = Rocks[0];
			for (int ty = 0; ty < N; ty++) for (int tx = 0; tx < N; tx++) {
				const float* s = texel(c, tx, ty);
				const D3DXVECTOR3 w = texelWorld(p, tx, ty);
				const float cd = CrackDistance(w.x, w.y);
				if (cd < CrackHalf * 0.6f) { crackTexels++; crackWet += s[0] > wetAt; if (s[0] > wetAt) { inCrack += s[0]; inCrackN++; } }
				else if (cd > CrackHalf * 1.5f && cd < CrackHalf * 5.0f && s[0] > wetAt) { beside += s[0]; besideN++; }
				const float rd = sqrtf((w.x - rock[0]) * (w.x - rock[0]) + (w.y - rock[1]) * (w.y - rock[1]));
				if (rd < rock[2] * 0.4f) { top += s[0]; topN++; }
				if (rd > rock[2] && rd < rock[2] * 1.8f && fabsf(w.y - rock[1]) < rock[2] * 0.6f) { if (w.x < rock[0]) left += s[0] > wetAt; else right += s[0] > wetAt; }
			}
			const double thickIn = inCrackN ? inCrack / inCrackN : 0, thickBeside = besideN ? beside / besideN : 0;
			const bool crackOk = crackWet > 20 && thickIn > 2.0 * thickBeside;
			std::printf("CHECK blood runs into the crack: %.0f of the crack's %.0f texels in the patch are wet, blood there %.3f units thick against %.3f beside it: %s\n",
				crackWet, crackTexels, thickIn, thickBeside, crackOk ? "PASS" : "FAIL");
			failed += !crackOk;
			const bool rockOk = topN > 0 && top / topN < wetAt && left > 3 && right > 3;
			std::printf("CHECK blood parts round the rock at (%.0f %.0f): on its top %.4f units, wet texels beside it left %.0f, right %.0f: %s\n",
				rock[0], rock[1], topN ? top / topN : 0, left, right, rockOk ? "PASS" : "FAIL");
			failed += !rockOk;
		}
	}
	{
		// Nothing floating, nothing painted that is not ground: every pixel that changed is land or a rigid object, and its
		// surface point lies on the ground or an object (by construction of the scene: there is nothing else to hit).
		// The light: over the pool that lies across the shadow's edge (round the hollow), how bright each blood pixel is against
		// the ground it covers, in the sun and in the shadow. The same film takes the same share of whatever light is there.
		int changedActor = 0, changedSky = 0, changed = 0, litN = 0, shadeN = 0;
		double litBlood = 0, shadeBlood = 0, litGround = 0, shadeGround = 0, litShare = 0, shadeShare = 0;
		for (int i = 0; i < W * H; i++) {
			float diff = 0;
			float lum = 0, was = 0;
			for (int ch = 0; ch < 3; ch++) {
				const float nowLin = ToLinear(min(max(picture[i * 4 + ch], 0.0f), 4.0f));
				diff = max(diff, fabsf(nowLin - sceneRGB[i * 3 + ch]));
				lum += nowLin * (ch == 0 ? 0.2126f : ch == 1 ? 0.7152f : 0.0722f);
				was += sceneRGB[i * 3 + ch] * (ch == 0 ? 0.2126f : ch == 1 ? 0.7152f : 0.0722f);
			}
			if (diff < 0.004f) continue;
			changed++;
			changedActor += pixelClass[i] == 2;
			changedSky += pixelClass[i] == 0;
			if (pixelClass[i] != 4 || fabsf(pixelXY[i * 2] - Hollow[0]) > 45.0f || pixelXY[i * 2 + 1] < Hollow[1] - 45.0f || pixelXY[i * 2 + 1] > Hollow[1] + 70.0f) continue;
			if (pixelSun[i] > 0.95f) { litBlood += lum; litGround += was; litShare += lum / max(was, 1e-4f); litN++; }
			else if (pixelSun[i] < 0.05f) { shadeBlood += lum; shadeGround += was; shadeShare += lum / max(was, 1e-4f); shadeN++; }
		}
		const bool actorOk = changedActor == 0 && changedSky == 0 && changed > 500;
		std::printf("CHECK only ground is painted: %d pixels changed, %d of them on the actors' boxes, %d on the sky: %s\n", changed, changedActor, changedSky, actorOk ? "PASS" : "FAIL");
		failed += !actorOk;
		if (litN > 50 && shadeN > 50) {
			const double bloodRatio = (litBlood / litN) / (shadeBlood / shadeN), groundRatio = (litGround / litN) / (shadeGround / shadeN);
			const double litQ = litShare / litN, shadeQ = shadeShare / shadeN;
			const bool lightOk = bloodRatio > 0.6 * groundRatio && bloodRatio < 1.6 * groundRatio && bloodRatio > 1.5;
			std::printf("CHECK blood follows the light (the pool across the shadow's edge): blood in the sun is %.2f x as bright as blood in the shadow (%d and %d pixels); the ground under it was %.2f x; blood over ground %.2f in the sun, %.2f in the shadow: %s\n",
				bloodRatio, litN, shadeN, groundRatio, litQ, shadeQ, lightOk ? "PASS" : "FAIL");
			failed += !lightOk;
		}
		else { std::printf("CHECK blood follows the light: too few blood pixels on one side of the shadow (%d lit, %d shaded): FAIL\n", litN, shadeN); failed++; }
	}

	// ---- Close-ups, the night shot, the second camera (the blood stays where it is: no steps) --------------------
	simOn = false;
	struct Shot { const char* name; D3DXVECTOR3 eye; float yaw, pitch; bool night; };
	const Shot shots[] = {
		{ "4_close_rocks_crack", D3DXVECTOR3(-48, 30, GroundZ(-48, 30) + 75), 2.0f, 36.0f, false },
		{ "4_close_slab_pool", D3DXVECTOR3(-190, -120, GroundZ(-190, -120) + 70), 0.0f, 40.0f, false },
		{ "4_close_hollow_shadow_edge", D3DXVECTOR3(70, -75, GroundZ(70, -75) + 70), 0.0f, 40.0f, false },
		{ "4_close_body_on_pool", D3DXVECTOR3(-40, 95, GroundZ(-40, 95) + 55), 0.0f, 34.0f, false },
		{ "4_close_steep_splat", D3DXVECTOR3(150, 20, GroundZ(150, 20) + 70), 0.0f, 30.0f, false },
		{ "4_close_wall_splat", D3DXVECTOR3(20, 170, GroundZ(20, 170) + 60), 0.0f, 8.0f, false },
		{ "4_close_wall_foot", D3DXVECTOR3(-70, 215, GroundZ(-70, 215) + 45), 0.0f, 22.0f, false },
		{ "4_close_crack", D3DXVECTOR3(-102, 22, GroundZ(-102, 22) + 42), 4.0f, 38.0f, false },
		{ "5_low_side_view", D3DXVECTOR3(-230, 60, GroundZ(-230, 60) + 26), 82.0f, 9.0f, false },
		{ "5_wet_toward_sun", D3DXVECTOR3(-175, 75, GroundZ(-175, 75) + 34), 172.0f, 17.0f, false },
		{ "6_night_lamp", D3DXVECTOR3(-175, -150, GroundZ(-175, -150) + 85), 0.0f, 36.0f, true },
	};
	for (const Shot& shot : shots) {
		const Camera cam = MakeCamera(shot.eye, shot.yaw, shot.pitch);
		if (renderScene(cam, shot.night)) return 1;
		lighting(shot.night);
		if (frame(cam, 0.0f)) return 1;
		if (save(shot.name)) return 1;
		if (getenv("BLOOD_DEBUG_SHOTS")) {	// (each close-up again in debug view 1)
			debugView = 1.0f;
			if (frame(cam, 0.0f)) return 1;
			if (save((std::string(shot.name) + "_debug").c_str())) return 1;
			debugView = 0.0f;
		}
	}
	lighting(false);
	return failed ? 1 : 0;
}
