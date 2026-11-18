// UNOFFICIAL 2026-11-14 LampTableFast micro-benchmark and self-check (fd-lamptable; docs/next/FD-LAMPTABLE-RESULTS.md).
// The lamp table (row 0) and its froxel lists (rows 1+) of ManyLamps::BuildTable / BinFroxels, made from a synthetic Vault 21-scale
// scene by the SAME code the game calls (src/core/LampTableCore.h; the colour decode is src/core/LinearPreDecode.h's), timed per stage,
// into three kinds of memory: cached (malloc), write-combined (VirtualAlloc PAGE_WRITECOMBINE: what a native D3D9 dynamic texture
// lock hands back), and a real D3D9 dynamic texture locked with D3DLOCK_DISCARD on this PC's native d3d9 (a hidden window).
// The scene walk reads fake engine objects scattered over a 64 MB arena (ShadowSceneNode's list, ShadowSceneLight, NiPointLight at
// their field offsets), with the caches flushed before each frame ("cold") as the game's frame would leave them: a proxy, not the game.
// Self-check: every frame and a fuzz pass, the fast path's row 0, lists (what the shaders read), stats and lamp lists against the
// reference's.
// 2026-11-14 fd-lampthread (docs/next/FD-LAMPTHREAD-RESULTS.md): step S4 = S3 with BinFast on FrameWorker's thread (LampTableThread.h):
// the main thread's part (lock, walk, row 0, hand-off, wait, unlock) timed apart from the worker's; a calibrated Vault 21 case (the game's
// F10 LAMP TABLE CPU line: 900 lamps, 542 active, 499 binned, 8436 entries at 960x540); the fuzz compares S4 with S0 and S3 as well.
//   lamp_table_bench.exe [frames] [--no-d3d] [--exhaustive] [--gap ms] [--only-v21] [--scene n share xmin xmax rmin rmax]
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <unordered_map>
#include <new>
typedef uint32_t UInt32;
static double LtStamp[5], LtSum[4]; static int LtCount;
static double LtNow() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return c.QuadPart * 1000.0 / f.QuadPart; }
#define LT_MARK(k) do { LtStamp[k] = LtNow(); if (k == 4) { for (int i = 0; i < 4; i++) LtSum[i] += LtStamp[i + 1] - LtStamp[i]; LtCount++; } } while (0)
#include <cstdarg>
namespace Logger { inline void Log(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); } }	// (FrameWorker.h's)
#include "../src/core/LinearPreDecode.h"
#include "../src/core/LampTableCore.h"
#include "../src/core/LampTableThread.h"	// 2026-11-14 fd-lampthread: the worker hand-off the game uses (FrameWorker.h's thread)

using namespace LampTableCore;

// --- fake engine objects (field offsets as the game's: ShadowSceneLight / NiPointLight) -------------------------------------------------
struct FakeSource {			// NiPointLight
	uint8_t pad0[0x30]; uint8_t flags; uint8_t pad1[0x8C - 0x31];
	float pos[3];			// m_worldTransform.pos
	uint8_t pad2[0xC4 - 0x98];
	float diff[3], spec[3];	// Diff, Spec (Spec.r = reach)
	float dimmer;
	uint8_t pad3[0x100 - 0xDC];
};
struct FakeLight {			// ShadowSceneLight
	uint8_t pad0[0x8]; float lod; uint8_t pad1[0xEC - 0xC]; uint8_t flagEC; uint8_t pointLight; uint8_t pad2[0xF8 - 0xEE];
	FakeSource* source; uint8_t pad3[0x110 - 0xFC]; uint16_t view110; uint8_t pad4[0x120 - 0x112];
};
struct FakeEntry { FakeEntry* next; FakeEntry* prev; FakeLight* data; };

struct Scene {
	std::vector<uint8_t> arena;
	FakeEntry* start = nullptr;
	std::vector<FakeLight*> lights;
};

static std::mt19937 rng(12345);
static float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

// n lamps in a Vault 21-like interior around the origin; activeShare of them on for the view
static void MakeScene(Scene& s, int n, float halfSize, float activeShare, float xmin = 0.0f, float xmax = 0.0f, float rmin = 160.0f, float rmax = 700.0f) {
	if (xmax <= xmin) { xmin = -halfSize; xmax = halfSize; }
	s.arena.assign(64u << 20, 0);
	std::vector<size_t> slots;
	const size_t step = 512, count = s.arena.size() / step;
	for (size_t i = 0; i < count; i++) slots.push_back(i * step);
	std::shuffle(slots.begin(), slots.end(), rng);
	size_t used = 0;
	auto place = [&](size_t bytes) { (void)bytes; return s.arena.data() + slots[used++]; };
	FakeEntry* prev = nullptr;
	for (int i = 0; i < n; i++) {
		FakeSource* src = (FakeSource*)place(sizeof(FakeSource));
		FakeLight* l = (FakeLight*)place(sizeof(FakeLight));
		FakeEntry* e = (FakeEntry*)place(sizeof(FakeEntry));
		src->pos[0] = U(xmin, xmax); src->pos[1] = U(-halfSize, halfSize); src->pos[2] = U(-200.0f, 500.0f);
		const float r = expf(U(logf(rmin), logf(rmax)));
		src->spec[0] = r;
		src->diff[0] = U(0.3f, 1.0f); src->diff[1] = U(0.3f, 1.0f); src->diff[2] = U(0.2f, 0.9f);
		src->dimmer = U(0.5f, 1.4f);
		src->flags = (U(0, 1) < 0.03f) ? 1 : 0;	// hidden
		l->source = src; l->pointLight = U(0, 1) < 0.98f; l->lod = U(0, 1) < 0.8f ? 1.0f : U(0.2f, 1.0f);
		l->view110 = U(0, 1) < activeShare ? 0 : 0xFF; l->flagEC = 0;
		e->data = l; e->prev = prev; if (prev) prev->next = e; else s.start = e;
		prev = e;
		s.lights.push_back(l);
		if (i % 97 == 5) {	// a duplicate entry now and then (the table keeps the first)
			FakeEntry* d = (FakeEntry*)place(sizeof(FakeEntry)); d->data = l; d->prev = prev; prev->next = d; prev = d;
		}
	}
}

// 2026-11-14 fd-lampthread: the scene calibrated to the game's Vault 21 LAMP TABLE CPU line (see the cases in main; --scene to re-fit)
#define V21_GAME_N 920
#define V21_GAME_SHARE 0.65f
#define V21_GAME_X -100.0f, 9500.0f, 125.0f, 380.0f	// (--scene 920 0.65 -100 9500 125 380: 900 lamps, 541 active, 492 binned, 8422 entries)
#define V21_GAME_CASE V21_GAME_N, 3500.0f, V21_GAME_SHARE,
#define V21_GAME_SCENE V21_GAME_N, 3500.0f, V21_GAME_SHARE, V21_GAME_X

// one frame's changes: a few lamps flicker (dimmer), some fade (LOD), the odd one switches active
static void Animate(Scene& s, int frame) {
	for (size_t i = 0; i < s.lights.size(); i++) {
		FakeLight* l = s.lights[i];
		if ((i * 7 + frame) % 20 == 0) l->source->dimmer *= U(0.97f, 1.03f);
		if ((i * 13 + frame) % 200 == 0) l->source->dimmer *= U(0.7f, 1.3f);
		if ((i + frame) % 500 == 0) l->lod = U(0.3f, 1.0f);
		if ((i * 3 + frame) % 997 == 0) l->view110 ^= 0xFF;
	}
}

static void ViewAt(View& v, int frame, unsigned w, unsigned h, int listTexels, bool stable) {
	const float yaw = 0.004f * frame, pitch = 0.05f * sinf(0.01f * frame);
	v.width = w; v.height = h;
	v.tilePx = TileSizeFor(w, h);	// (2026-11-15, as ManyLamps::BinFroxels)
	v.fwd[0] = cosf(yaw) * cosf(pitch); v.fwd[1] = sinf(yaw) * cosf(pitch); v.fwd[2] = sinf(pitch);
	v.right[0] = sinf(yaw); v.right[1] = -cosf(yaw); v.right[2] = 0.0f;
	v.up[0] = v.right[1] * v.fwd[2] - v.right[2] * v.fwd[1]; v.up[1] = v.right[2] * v.fwd[0] - v.right[0] * v.fwd[2]; v.up[2] = v.right[0] * v.fwd[1] - v.right[1] * v.fwd[0];
	v.cam[0] = 30.0f * sinf(0.02f * frame); v.cam[1] = 20.0f * cosf(0.015f * frame); v.cam[2] = 120.0f;
	v.tanX = 1.0f / 0.75f * 0.75f;	// 75 deg vertical-ish: tanY 0.767, tanX by the aspect
	v.tanY = 0.767f; v.tanX = v.tanY * (float)w / (float)h;
	v.listTexels = listTexels; v.stableOrder = stable;
}

// --- the scene walk, as ManyLamps::BuildTable does it ---------------------------------------------------------------------------------
struct Walk {
	std::vector<SceneLamp> lamps;	// table order
	std::vector<int> active;
	Walk() { lamps.resize(TableLamps); active.reserve(TableLamps); }
};
static bool Insert(PtrMap<int>& m, const void* k, int v) { return m.insert(k, v); }
static bool Insert(std::unordered_map<const void*, int>& m, const void* k, int v) { if (m.count(k)) return false; m[k] = v; return true; }
// TableIndex: flat (new) or the unordered_map it was (old, the cost step S0)
template <class Index> static int Gather(const Scene& s, Walk& w, Index& index) {
	index.clear();
	w.active.clear();
	int n = 0;
	for (const FakeEntry* e = s.start; e && n < TableLamps; e = e->next) {
		const FakeLight* light = e->data;
		const FakeSource* src = light ? light->source : nullptr;
		if (!src || !light->pointLight) continue;
		if (!Insert(index, light, n)) continue;
		SceneLamp& l = w.lamps[n];
		l.id = light;
		l.pos[0] = src->pos[0]; l.pos[1] = src->pos[1]; l.pos[2] = src->pos[2]; l.reach = src->spec[0];
		l.diff[0] = src->diff[0]; l.diff[1] = src->diff[1]; l.diff[2] = src->diff[2]; l.dimmer = src->dimmer; l.lod = light->lod;
		l.hidden = (src->flags & 1) != 0;
		l.active = light->view110 != 0xFF && !l.hidden && !light->flagEC;
		if (l.active) w.active.push_back(n);
		n++;
	}
	return n;
}

struct Lamp7 { float v[7]; bool operator==(const Lamp7& o) const { return !memcmp(v, o.v, sizeof(v)); } };
static void Lists(const float* row, const Walk& w, int n, std::vector<Lamp7>& all, std::vector<Lamp7>& act) {
	all.clear(); act.clear();
	for (int k = 0; k < n; k++) {
		const float* p = row + k * 8;
		const Lamp7 l = { { p[0], p[1], p[2], p[3], p[4], p[5], p[6] } };
		if (!w.lamps[k].hidden) all.push_back(l);
		if (w.lamps[k].active) act.push_back(l);
	}
}

static const bool Hdr = false;
static void Decode(float* out, const float* in, float gamma) { LinearPreDecode::DecodeFast(out, in, gamma); }

// --- memory targets -----------------------------------------------------------------------------------------------------------------
struct Target {
	const char* name;
	float* (*lock)(Target&, int& pitchFloats);
	void (*unlock)(Target&);
	float* mem = nullptr; int pitch = 0;
	IDirect3DTexture9* tex = nullptr; IDirect3DDevice9* dev = nullptr;
};
static float* LockMem(Target& t, int& p) { p = t.pitch; return t.mem; }
static void UnlockMem(Target&) {}
static float* LockTex(Target& t, int& p) {
	D3DLOCKED_RECT r = {};
	if (FAILED(t.tex->LockRect(0, &r, NULL, D3DLOCK_DISCARD))) return nullptr;
	p = r.Pitch / 4; return (float*)r.pBits;
}
static void UnlockTex(Target& t) {
	t.tex->UnlockRect(0);
}
static void Present(Target& t) {	// (not timed)
	// use it (a point drawn with it bound) and present, as a frame would
	t.dev->BeginScene();
	t.dev->SetTexture(0, t.tex);
	struct V { float x, y, z, rhw; } pt = { 1, 1, 0, 1 };
	t.dev->SetFVF(D3DFVF_XYZRHW);
	t.dev->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &pt, sizeof(pt));
	t.dev->EndScene();
	t.dev->Present(NULL, NULL, NULL, NULL);
}

static IDirect3DDevice9* MakeDevice() {
	WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "LampTableBench";
	RegisterClassA(&wc);
	HWND hwnd = CreateWindowA("LampTableBench", "lamp table bench", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d || !hwnd) return nullptr;
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = hwnd;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	IDirect3DDevice9* dev = nullptr;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &pp, &dev))) return nullptr;
	D3DADAPTER_IDENTIFIER9 id = {};
	d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
	printf("native D3D9 device: %s\n", id.Description);
	return dev;
}

// --- timing ------------------------------------------------------------------------------------------------------------------------
static double Freq;
static double Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / Freq; }
struct Series { std::vector<double> v; void add(double x) { v.push_back(x); } double mean() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
	double median() const { if (v.empty()) return 0; std::vector<double> c = v; std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end()); return c[c.size() / 2]; } };

static std::vector<uint8_t> Flush;	// 48 MB, sized in main (a constexpr vector of that size runs the compiler out of heap)
static void Cold() { for (size_t i = 0; i < Flush.size(); i += 64) Flush[i]++; }

enum Step { S0_Old, S1_FlatIndex, S2_StagedRow, S3_BinFast, S4_Thread, S4_ThreadNoGap, Steps };
static const char* StepName[Steps] = { "S0 old (map index, row in place + read back, reference binning)", "S1 + flat TableIndex",
	"S2 + row 0 staged in cached memory, colour memo", "S3 + BinFast (scratch lists, froxel-major texel writes)",
	"S4 S3 + BinFast on the worker, finished after the gap", "S4' the same, finished at once (no gap: worst case)" };
static int Failures = 0;
static double GapMs = 0.5;	// --gap: the game's own main-thread work between the make and the first reader (spun, not timed)
static void Spin(double ms);

struct Path {
	Walk walk;
	PtrMap<int> flat;
	std::unordered_map<const void*, int> map;
	ColourMemo memo;
	float staged[TableTexels * 4];
	RefState ref; FastState fast;
	std::vector<Lamp7> all, act;
	BinStats stats;
	int n = 0;
	LampTableThread::Job job;	// S4
};

// bin: the main thread's binning part (S4: the hand-off + the wait); wait / worker: S4 only
struct Times { Series walk, row, bin, lock, unlock, total, wait, worker; };

// one make of the table on the given step; returns the rows written into t (pitch in floats)
static void Make(Step step, Path& p, const Scene& s, const View& v, bool linear, float gamma, Target& tg, uint32_t serial, Times* tm) {
	const double t0 = Now();
	int pitch = 0;
	float* t = tg.lock(tg, pitch);
	const double t1 = Now();
	const int n = p.n = step == S0_Old ? Gather(s, p.walk, p.map) : Gather(s, p.walk, p.flat);
	const double t2 = Now();
	if (step < S2_StagedRow) {
		FillRow(t, p.walk.lamps.data(), n, Hdr, linear, gamma, Decode, nullptr);
		Lists(t, p.walk, n, p.all, p.act);	// read back from the locked memory (as BuildTable did)
	}
	else {
		FillRow(p.staged, p.walk.lamps.data(), n, Hdr, linear, gamma, Decode, &p.memo);
		Lists(p.staged, p.walk, n, p.all, p.act);
		memcpy(t, p.staged, sizeof(p.staged));
	}
	const double t3 = Now();
	p.stats = BinStats();
	double gap = 0.0, wait = 0.0;
	if (step < S3_BinFast) BinReference(v, p.walk.lamps.data(), p.walk.active.data(), (int)p.walk.active.size(), t + pitch, pitch, serial, p.ref, p.stats);
	else if (step == S3_BinFast) BinFast(v, p.walk.lamps.data(), p.walk.active.data(), (int)p.walk.active.size(), t + pitch, pitch, serial, p.fast, p.stats);
	else {	// S4: as ManyLamps::BuildTable with LampTableThread: the job onto the worker, the game's own work, then the finish before the unlock
		LampTableThread::Job& j = p.job;
		j.view = v; j.table = p.walk.lamps.data(); j.active = p.walk.active.data(); j.nActive = (int)p.walk.active.size();
		j.lists = t + pitch; j.pitch = pitch; j.serial = serial; j.state = &p.fast;
		if (!LampTableThread::Start(j)) { printf("  S4: the worker was busy (not expected in the bench)\n"); Failures++; }
		if (step == S4_Thread) { const double g0 = Now(); Spin(GapMs); gap = Now() - g0; }
		wait = LampTableThread::Finish(j);
		p.stats = j.stats;
	}
	const double t4 = Now();
	tg.unlock(tg);
	const double t5 = Now();
	if (tg.tex) Present(tg);
	if (tm) {
		tm->lock.add(t1 - t0); tm->walk.add(t2 - t1); tm->row.add(t3 - t2); tm->bin.add(t4 - t3 - gap); tm->unlock.add(t5 - t4); tm->total.add(t5 - t0 - gap);
		if (step >= S4_Thread) { tm->wait.add(wait); tm->worker.add(p.job.workerMs); }
	}
}
static void Spin(double ms) { const double end = Now() + ms; while (Now() < end) {} }

static void Check(const char* what, bool ok) { if (!ok) { Failures++; if (Failures < 20) printf("  CHECK FAILED: %s\n", what); } }

// reference vs fast vs threaded on cached buffers (garbage-filled), same input. The threaded (S4) rows must equal the fast (S3) rows
// byte for byte over everything BinFast writes (row 0 and every texel of the lists up to the last froxel: the same code, the same inputs),
// and both must equal the reference as the shaders read it.
static unsigned long long ThreadCompared = 0;
static void Compare(Path& a, Path& b, Path& c, const Scene& s, const View& v, bool linear, float gamma, uint32_t serial, Target& ca, Target& cb, Target& cc, int frame) {
	for (size_t i = 0; i < (size_t)ca.pitch * 64; i++) { ca.mem[i] = -7.5f; cb.mem[i] = 1.0e30f; cc.mem[i] = 1.0e30f; }
	Make(S0_Old, a, s, v, linear, gamma, ca, serial, nullptr);
	Make(S3_BinFast, b, s, v, linear, gamma, cb, serial, nullptr);
	Make(frame % 2 ? S4_Thread : S4_ThreadNoGap, c, s, v, linear, gamma, cc, serial, nullptr);	// (both finish orders: done before the finish, and waited for)
	Check("row 0 bytes", !memcmp(ca.mem, cb.mem, TableTexels * 4 * sizeof(float)));
	Check("froxel lists", ListsEqual(v, ca.mem + ca.pitch, ca.pitch, cb.mem + cb.pitch, cb.pitch) < 0);
	Check("stats", a.stats.lamps == b.stats.lamps && a.stats.entries == b.stats.entries && a.stats.over == b.stats.over);
	Check("AllLamps", a.all == b.all);
	Check("ActiveLamps", a.act == b.act);
	// threaded vs fast: every byte of the table (64 rows of the buffer), the counts, the return
	Check("threaded: table bytes", !memcmp(cb.mem, cc.mem, (size_t)cb.pitch * 64 * sizeof(float)));
	Check("threaded: lists vs reference", ListsEqual(v, ca.mem + ca.pitch, ca.pitch, cc.mem + cc.pitch, cc.pitch) < 0);
	Check("threaded: stats", c.stats.lamps == b.stats.lamps && c.stats.entries == b.stats.entries && c.stats.over == b.stats.over);
	Check("threaded: lamp lists", c.all == b.all && c.act == b.act);
	ThreadCompared++;
}

// 2026-11-15 (fd-lampthread, batch 3): LampTableCheck as the game runs it. The fast path makes every binning (state kept across frames);
// the reference runs only in check windows (on for 100 binnings in every 200), each with its own key history, as ManyLamps::CheckLists.
// seed: the reference first takes the fast path's key history when it did not run on the binning before (LampTableCore::SeedRefFromFast,
// what ManyLamps::BinFroxels now does). Lamps flicker by up to 2 % a frame (kept keys drift away from the raw ones, as fires and LOD
// fades do in game). Returns the binnings whose lists or counts differed.
static int CheckWindowFlow(bool seed, bool stable, unsigned w, unsigned h, int lt, int n, float half, float share, float xmin, float xmax, float rmin, float rmax, int frames, int* compared) {
	Scene s; rng.seed(9001 + n + w);
	MakeScene(s, n, half, share, xmin, xmax, rmin, rmax);
	static Walk walk; static PtrMap<int> index; RefState ref;
	FastState& fast = *new FastState();	// (1 MB: not on the stack; freed below)

	const int pitch = TableTexels * 4;
	std::vector<float> a((size_t)pitch * 64), b((size_t)pitch * 64);
	uint32_t serial = 0, refLast = 0;
	int bad = 0; *compared = 0;
	for (int fr = 0; fr < frames; fr++) {
		Animate(s, fr);
		for (size_t i = 0; i < s.lights.size(); i += 2) s.lights[i]->source->dimmer *= U(0.98f, 1.02f);	// flicker
		View v; ViewAt(v, fr, w, h, lt, stable);
		Gather(s, walk, index);
		++serial;
		const bool window = (fr / 100) % 2 == 1;
		if (window && seed && refLast + 1 != serial) SeedRefFromFast(fast, serial, ref);
		BinStats sa, sb;
		BinFast(v, walk.lamps.data(), walk.active.data(), (int)walk.active.size(), a.data() + pitch, pitch, serial, fast, sa);
		if (!window) continue;
		BinReference(v, walk.lamps.data(), walk.active.data(), (int)walk.active.size(), b.data() + pitch, pitch, serial, ref, sb);
		refLast = serial;
		(*compared)++;
		if (ListsEqual(v, a.data() + pitch, pitch, b.data() + pitch, pitch) >= 0 || sa.lamps != sb.lamps || sa.entries != sb.entries || sa.over != sb.over) bad++;
	}
	delete &fast;
	return bad;
}

int main(int argc, char** argv) {
	LARGE_INTEGER f; QueryPerformanceFrequency(&f); Freq = (double)f.QuadPart;
	SetThreadAffinityMask(GetCurrentThread(), 1);	// one core, as the game's main thread
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	int frames = argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9' ? atoi(argv[1]) : 600;
	bool d3d = true, onlyV21 = false, sceneOnly = false;
	float sc[6] = {};	// --scene: lamps, share, xmin, xmax, rmin, rmax (prints the scene's statistics at 960x540 and quits: calibration)
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-d3d")) d3d = false;
		if (!strcmp(argv[i], "--only-v21")) onlyV21 = true;
		if (!strcmp(argv[i], "--gap") && i + 1 < argc) GapMs = atof(argv[i + 1]);
		if (!strcmp(argv[i], "--scene") && i + 6 < argc) { sceneOnly = true; for (int k = 0; k < 6; k++) sc[k] = (float)atof(argv[i + 1 + k]); }
	}
	if (sceneOnly) {	// mean over 60 frames of the S3 make (the camera turns as in the benchmark)
		Scene s; rng.seed(777);
		MakeScene(s, (int)sc[0], 3500.0f, sc[1], sc[2], sc[3], sc[4], sc[5]);
		static Path p; Target ct = { "cached", LockMem, UnlockMem }; ct.pitch = TableTexels * 4;
		std::vector<float> m((size_t)ct.pitch * 32); ct.mem = m.data();
		double n = 0, act = 0, binned = 0, entries = 0, over = 0;
		for (int fr = 0; fr < 60; fr++) {
			Animate(s, fr);
			View v; ViewAt(v, fr, 960, 540, 8, true);
			Make(S3_BinFast, p, s, v, true, 2.2f, ct, (uint32_t)fr + 1, nullptr);
			n += p.n; act += p.walk.active.size(); binned += (double)p.stats.lamps; entries += (double)p.stats.entries; over += (double)p.stats.over;
		}
		printf("scene %g %g %g %g %g %g: %.1f table lamps, %.1f active, %.1f binned, %.0f entries, %.0f over the cap\n", sc[0], sc[1], sc[2], sc[3], sc[4], sc[5],
			n / 60, act / 60, binned / 60, entries / 60, over / 60);
		return 0;
	}
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--exhaustive")) {
		// FloorInt == (int)floorf and SliceFast == SliceOf for every 32-bit pattern (BinFast's Rect<true> against BinReference's)
		unsigned long long floorBad = 0, sliceBad = 0;
		uint32_t b = 0;
		do {
			float x; memcpy(&x, &b, sizeof(x));
			if (FloorInt(x) != (int)floorf(x)) { if (floorBad++ < 5) printf("  FloorInt differs at %08X\n", b); }
			if (SliceFast(x) != SliceOf(x)) { if (sliceBad++ < 5) printf("  SliceFast differs at %08X\n", b); }
		} while (++b != 0);
		printf("exhaustive: FloorInt %llu differences, SliceFast %llu differences over all 2^32 floats\n", floorBad, sliceBad);
		if (floorBad || sliceBad) Failures++;
	}
	LinearPreDecode::UseCache = true;	// PreDecodeCache, on by default
	Flush.resize(48u << 20);

	// --- self-check: fuzz -------------------------------------------------------------------------------------------------------
	{
		static Path a, b, c;
		Target ca = { "cached A", LockMem, UnlockMem }, cb = { "cached B", LockMem, UnlockMem }, cc = { "cached C", LockMem, UnlockMem };
		ca.pitch = cb.pitch = cc.pitch = TableTexels * 4;
		std::vector<float> ma((size_t)ca.pitch * 64), mb((size_t)cb.pitch * 64), mc((size_t)cc.pitch * 64);
		ca.mem = ma.data(); cb.mem = mb.data(); cc.mem = mc.data();
		uint32_t serial = 0;
		int checked = 0;
		for (int round = 0; round < 40; round++) {
			Scene s;
			const int n = round % 4 == 0 ? 1100 : round % 4 == 1 ? 447 : round % 4 == 2 ? 900 : 30;
			MakeScene(s, n, round % 3 == 0 ? 600.0f : 3000.0f, 0.55f);
			const unsigned sizes[][2] = { { 960, 540 }, { 2560, 1440 }, { 1920, 1080 }, { 1, 1 }, { 3840, 2160 }, { 0, 0 }, { 1280, 1024 }, { 65, 63 } };
			for (int fr = 0; fr < 30; fr++) {
				Animate(s, fr + round * 31);
				View v;
				const unsigned* sz = sizes[(round + fr / 10) % 8];
				ViewAt(v, fr * 17 + round * 101, sz[0] ? sz[0] : 0, sz[1], (round / 2) % 2 ? 16 : 8, (fr / 7 + round) % 3 != 0);
				if (v.height == 0) { v.tanX = 1.0f; v.tanY = 1.0f; }
				if (round % 5 == 4) { v.cam[0] = s.lights[0]->source->pos[0]; v.cam[1] = s.lights[0]->source->pos[1]; v.cam[2] = s.lights[0]->source->pos[2]; }	// inside a lamp
				if (round % 7 == 3 && fr % 3 == 0) s.lights[fr]->source->spec[0] = fr % 2 ? 0.0f : -5.0f;
				if (round % 6 == 2 && fr % 5 == 0) serial += 3;	// a gap: a path that did not run last frame
				const bool linear = round % 3 != 1;
				Compare(a, b, c, s, v, linear, round % 2 ? 2.2f : 1.8f, ++serial, ca, cb, cc, fr);
				checked++;
			}
		}
		printf("self-check (fuzz): %d frames compared, %d failures\n", checked, Failures);
		// 2026-11-14 fd-lampthread: a long walk through the calibrated Vault 21 scene with the camera turning and moving fast (up to 9 degrees
		// and 60 units a frame), lamps flickering, 8 / 16 texels and StableOrder switched every few hundred frames; all three paths each frame
		{
			Scene s; rng.seed(4242);
			MakeScene(s, V21_GAME_SCENE);
			const int before = Failures;
			for (int fr = 0; fr < 3000; fr++) {
				Animate(s, fr);
				View v;
				const int t = fr % 600 < 300 ? fr * 3 : fr * 40;	// slow, then fast camera
				ViewAt(v, t, (fr / 750) % 2 ? 2560 : 960, (fr / 750) % 2 ? 1440 : 540, (fr / 250) % 2 ? 16 : 8, (fr / 400) % 4 != 3);
				v.cam[0] += 2.0f * (float)(t % 997); v.cam[1] -= 1.5f * (float)(t % 613);
				Compare(a, b, c, s, v, true, 2.2f, ++serial, ca, cb, cc, fr);
				checked++;
			}
			printf("self-check (camera walk, calibrated Vault 21): 3000 frames compared, %d failures\n", Failures - before);
		}
		printf("self-check: %d frames, threaded path compared on %llu of them, %d failures\n", checked, ThreadCompared, Failures);
		// 2026-11-15 batch 3: the check's flow (reference only in windows) with the game's lamp counts at 1920x1080, 16 texels
		{
			struct Room { const char* name; int n; float half, share, xmin, xmax, rmin, rmax; unsigned w, h; int lt; };
			const Room rooms[] = {
				{ "Vault 21 (967 lamps, 542 active), 1920x1080 x 16", 987, 3500.0f, 0.61f, -100.0f, 9500.0f, 125.0f, 380.0f, 1920, 1080, 16 },
				{ "cave (117 lamps, 19 active, large reach), 1920x1080 x 16", 119, 900.0f, 0.17f, -300.0f, 1500.0f, 400.0f, 1200.0f, 1920, 1080, 16 },
				{ "saloon-like (65 lamps, 19 active), 1920x1080 x 16", 66, 700.0f, 0.3f, -200.0f, 1200.0f, 300.0f, 1000.0f, 1920, 1080, 16 },
				{ "Lucky 38-like (124 lamps, 88 active), 1920x1080 x 16", 126, 1500.0f, 0.71f, -500.0f, 3000.0f, 100.0f, 400.0f, 1920, 1080, 16 },
				{ "Vault 21 calibrated, 960x540 x 8 (phase 0)", V21_GAME_SCENE, 960, 540, 8 },
			};
			for (const Room& r : rooms) {
				int cmpOld = 0, cmpNew = 0;
				const int old = CheckWindowFlow(false, true, r.w, r.h, r.lt, r.n, r.half, r.share, r.xmin, r.xmax, r.rmin, r.rmax, 1200, &cmpOld);
				const int fixed = CheckWindowFlow(true, true, r.w, r.h, r.lt, r.n, r.half, r.share, r.xmin, r.xmax, r.rmin, r.rmax, 1200, &cmpNew);
				int cmpOff = 0;
				const int off = CheckWindowFlow(true, false, r.w, r.h, r.lt, r.n, r.half, r.share, r.xmin, r.xmax, r.rmin, r.rmax, 600, &cmpOff);
				printf("check flow, %s: without the seeding %d of %d binnings differed; with it %d of %d; StableOrder off %d of %d\n",
					r.name, old, cmpOld, fixed, cmpNew, off, cmpOff);
				if (fixed || off) Failures++;
			}
		}
	}

	// --- benchmark --------------------------------------------------------------------------------------------------------------
	IDirect3DDevice9* dev = d3d ? MakeDevice() : nullptr;
	if (d3d && !dev) printf("native D3D9 device: not made (the texture target is skipped)\n");
	#ifndef SCENE_SHARE
#define SCENE_SHARE 0.7f
#endif
#ifndef SCENE_X
#define SCENE_X -1500.0f, 9000.0f, 56.0f, 240.0f
#endif
	struct Case { const char* name; int lamps; float half; float share; unsigned w, h; int lt; float xmin, xmax, rmin, rmax; };
	const Case cases[] = {
		// Vault 21 lobby-like: 900 lamps, ~70 % on, most in front of the camera, reach 128-450; calibrated to the game's F10 CLUSTERED line
		// (ROOTPLAN-RESULTS.md: 499 lamps binned, 4044 entries + 265 dropped at 960x540, 8 texels)
#define V21 900, 3500.0f, SCENE_SHARE, 
		{ "Vault 21-like (900 table lamps), 960x540, 8 texels", V21 960, 540, 8, SCENE_X },
		{ "Vault 21-like (900 table lamps), 2560x1440, 8 texels", V21 2560, 1440, 8, SCENE_X },
		{ "Vault 21-like (900 table lamps), 960x540, 16 texels (DeferredLamps)", V21 960, 540, 16, SCENE_X },
		{ "Vault 21-like (900 table lamps), 2560x1440, 16 texels (DeferredLamps)", V21 2560, 1440, 16, SCENE_X },
		{ "447 scene lamps, 960x540, 8 texels", 447, 3500.0f, SCENE_SHARE, 960, 540, 8, SCENE_X },
		{ "447 scene lamps, 2560x1440, 8 texels", 447, 3500.0f, SCENE_SHARE, 2560, 1440, 8, SCENE_X },
		// 2026-11-14 fd-lampthread: calibrated to the game's own LAMP TABLE CPU line in Vault 21 (phase 0 bench, native, fast path:
		// 900 lamps, 542 active, 499 binned, 8436 list entries at 960x540 with 8 texels; make 0.105 ms = walk 0.015 + row 0 0.009 +
		// binning 0.065 + lock/unlock 0.015). 16 texels = the full deferred renderer's lists (DeferredLamps).
		{ "V21 calibrated (game line), 960x540, 8 texels (native standalone)", V21_GAME_CASE 960, 540, 8, V21_GAME_X },
		{ "V21 calibrated (game line), 960x540, 16 texels (DeferredRenderer)", V21_GAME_CASE 960, 540, 16, V21_GAME_X },
		{ "V21 calibrated (game line), 2560x1440, 16 texels (DeferredRenderer)", V21_GAME_CASE 2560, 1440, 16, V21_GAME_X },
	};
	const bool linear = true; const float gamma = 2.2f;
	printf("\nS4 gap (the game's main-thread work between the make and the first reader, spun, not timed): %.3f ms\n", GapMs);
	for (const Case& c : cases) {
		if (onlyV21 && strncmp(c.name, "V21 calibrated", 14)) continue;
		Scene s; rng.seed(777);
		MakeScene(s, c.lamps, c.half, c.share, c.xmin, c.xmax, c.rmin, c.rmax);
		const int rows = 1 + (MaxFroxels * c.lt + TableTexels - 1) / TableTexels;
		// scene statistics (one frame)
		{
			static Path p; Target ct = { "cached", LockMem, UnlockMem }; ct.pitch = TableTexels * 4;
			std::vector<float> m((size_t)ct.pitch * (rows + 1)); ct.mem = m.data();
			View v; ViewAt(v, 0, c.w, c.h, c.lt, true);
			Make(S3_BinFast, p, s, v, linear, gamma, ct, 1, nullptr);
			const int tx = TilesX(v), ty = TilesY(v);
			// distinct lamps per screen tile (any slice)
			double perTile = 0;
			for (int y = 0; y < ty; y++) for (int x = 0; x < tx; x++) {
				std::vector<int> ids;
				for (int sl = 0; sl < Slices; sl++) { const float* l = m.data() + ct.pitch + ((sl * ty + y) * tx + x) * c.lt * 4; for (int k = 0; k < (int)l[0]; k++) ids.push_back((int)(k < 3 ? l[1 + k] : l[4 + (k - 3)])); }
				std::sort(ids.begin(), ids.end()); perTile += std::unique(ids.begin(), ids.end()) - ids.begin();
			}
			printf("\n== %s: %d table lamps, %d active, %llu binned, %llu entries (%.2f a froxel), %llu over the cap, %.1f lamps a screen tile, %dx%dx%d froxels, %d rows\n",
				c.name, p.n,
				(int)p.walk.active.size(), p.stats.lamps, p.stats.entries, p.stats.entries / (double)(tx * ty * Slices), p.stats.over, perTile / (tx * ty), tx, ty, (int)Slices, rows);
		}
		std::vector<Target> targets;
		std::vector<float> cachedMem((size_t)TableTexels * 4 * rows);
		Target ct = { "cached", LockMem, UnlockMem }; ct.mem = cachedMem.data(); ct.pitch = TableTexels * 4; targets.push_back(ct);
		float* wc = (float*)VirtualAlloc(NULL, (size_t)TableTexels * 16 * rows, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE);
		Target wt = { "write-combined", LockMem, UnlockMem }; wt.mem = wc; wt.pitch = TableTexels * 4; if (wc) targets.push_back(wt);
		IDirect3DTexture9* tex = nullptr;
		if (dev && SUCCEEDED(dev->CreateTexture(TableTexels, rows, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &tex, NULL))) {
			dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			Target dt = { "D3D9 dynamic texture (DISCARD)", LockTex, UnlockTex }; dt.tex = tex; dt.dev = dev; targets.push_back(dt);
		}
		for (Target& tg : targets) {
			for (int cold = 1; cold >= 0; cold--) {
				printf("  %s, %s caches:\n", tg.name, cold ? "cold" : "warm");
				printf("    %-62s %8s %8s %8s %8s %8s | %8s %8s\n", "step (ms, median a make)", "lock", "walk", "row 0", "bin", "unlock", "total", "mean");
				// the steps interleaved frame by frame (the PC is shared: a drift in load hits every step alike)
				std::vector<Path*> paths; Times tm[Steps];
				for (int st = 0; st < Steps; st++) paths.push_back(new Path());
				double ltSum[4] = {}; int ltCount = 0;
				Scene& sc = s;
				for (int fr = 0; fr < frames + 30; fr++) {
					Animate(sc, fr);
					View v; ViewAt(v, fr, c.w, c.h, c.lt, true);
					for (int st = 0; st < Steps; st++) {
						if (cold) Cold();
						memset(LtSum, 0, sizeof(LtSum)); LtCount = 0;
						Make((Step)st, *paths[st], sc, v, linear, gamma, tg, (uint32_t)fr + 1, fr >= 30 ? &tm[st] : nullptr);
						if (st == S3_BinFast && fr >= 30) { for (int k = 0; k < 4; k++) ltSum[k] += LtSum[k]; ltCount++; }
					}
				}
				for (int st = 0; st < Steps; st++) {
					if (st == S3_BinFast && ltCount) printf("      BinFast parts (mean ms): keys %.4f sort %.4f bin %.4f write-out %.4f\n", ltSum[0] / ltCount, ltSum[1] / ltCount, ltSum[2] / ltCount, ltSum[3] / ltCount);
					printf("    %-62s %8.4f %8.4f %8.4f %8.4f %8.4f | %8.4f %8.4f", StepName[st], tm[st].lock.median(), tm[st].walk.median(), tm[st].row.median(), tm[st].bin.median(), tm[st].unlock.median(), tm[st].total.median(), tm[st].total.mean());
					if (st >= S4_Thread) printf("   (wait median %.4f mean %.4f, worker median %.4f mean %.4f)", tm[st].wait.median(), tm[st].wait.mean(), tm[st].worker.median(), tm[st].worker.mean());
					printf("\n");
					delete paths[st];
				}
			}
		}
		if (tex) tex->Release();
		if (wc) VirtualFree(wc, 0, MEM_RELEASE);
	}
	if (dev) dev->Release();
	printf("\nself-check failures: %d\n", Failures);
	return Failures ? 1 : 0;
}
