#pragma once

#include "VolumetricSmokeQuads.h"
#include "../../gunfx/GunFX.h"	// 2026-11-16 (gunfx-builtin): GunFX built in; its smoke by direct calls
#include "../../core/GpuProfiler.h"
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-25 (NVR DXVK): PassesOn in Render

// GunFX volumetric smoke (prototype). GunFX (built in since 2026-11-16, src/gunfx; was GunFX.dll) keeps its smoke as chains of points and hands over one record per tube
// segment (GunFX_GetVolumeSmoke). Step 1 draws every segment that can be on screen as a quad over the area it reaches,
// adding its optical depth into TESR_SmokeBuffer; step 2 (the effect chain's pass) lights the total and lays it over
// the image, only inside the area the smoke covers (scissor). The cost follows the smoke's size on screen.
// The gun-smoke haze (GunFX_GetSmokeHaze: up to 40 big, soft blobs) is drawn by a pass of its own, every blob at once at
// quarter resolution (TESR_SmokeHaze), and step 2 adds it in over the whole screen.
namespace VolumetricSmoke {
    inline const int FetchMax = 720;      // what GunFX sends at most (four chains and two twin arms of 96 points, tendrils)
    inline const float NearFade = 6.0f;   // the shader hides smoke closer to the camera than this (game units)
    inline VolumetricSmokeQuads::Vertex Vertices[FetchMax * 6];
    // Segments whose quad covers more than BigArea of the screen (big and close: soft on screen, and in thick smoke each one
    // a large or nearly full-screen pass) are drawn into a buffer at an eighth of the resolution instead (TESR_SmokeBufferLow).
    // 1 % (was 4 %), and an eighth (was a quarter): walking into thick smoke on the bench (smoke_bench BIG_AREA, LOW_DIV)
    // 3.4 ms (4 %, quarter) -> 2.7 (1 %, quarter) -> 0.8 ms (1 %, eighth) at 2560x1440, with no visible change (no pixel
    // off by more than 4 of 255; the same smoke over the gun's edge): smoke that big on screen is all smooth blobs.
    inline const float BigArea = 0.01f;
    inline VolumetricSmokeQuads::Vertex VerticesBig[FetchMax * 6];
    inline int CountBig = 0;
    inline float Shaded = 0.0f;           // screens' worth of pixels step 1 shades this frame (before the resolution cut)
    inline D3DXVECTOR4 Info = {}, Params = {}, Flash = {}, FlashColor = {}, Look = {};
    inline int Count = 0;                 // segments drawn this frame
    // The frame (ShaderManager::RenderEffects counts them): the smoke is fetched once a frame. (It was once a millisecond
    // tick, GetTickCount64, which only moves every 15.6 ms: at 95 frames a second every third frame drew the last frame's
    // smoke, built for the last frame's camera, so it jumped and the haze flickered.)
    inline unsigned FrameNumber = 0, FetchedFrame = ~0u;
    inline UInt32 LastGameFrame = 0xFFFFFFFFu;
    void SyncFrame();
    inline LARGE_INTEGER EnvAt = {};      // when the room light was last worked out (it eases to the new value)
    inline RECT Scissor = {};
    inline IDirect3DTexture9* Texture = nullptr;      // the buffer used this frame (TESR_SmokeBuffer follows it)
    inline IDirect3DSurface9* Surface = nullptr;
    inline IDirect3DTexture9* FullTexture = nullptr;  // full resolution
    inline IDirect3DSurface9* FullSurface = nullptr;
    inline IDirect3DTexture9* HalfTexture = nullptr;  // half resolution (Main > GunFX > VolumetricSmokeHalfRes)
    inline IDirect3DSurface9* HalfSurface = nullptr;
    inline IDirect3DTexture9* LowTexture = nullptr;   // an eighth of the resolution, for the big segments (TESR_SmokeBufferLow)
    inline IDirect3DSurface9* LowSurface = nullptr;
    inline IDirect3DTexture9* HazeTexture = nullptr;  // quarter resolution, the haze (TESR_SmokeHaze)
    inline IDirect3DTexture9* CutTexture = nullptr;   // quarter resolution, the bullets' cut of the segment smoke (TESR_SmokeCut), written with the haze
    inline IDirect3DSurface9* CutSurface = nullptr;
    inline IDirect3DSurface9* HazeSurface = nullptr;
    inline IDirect3DTexture9* EnvTexture = nullptr;   // one pixel, the room light (TESR_SmokeEnv)
    inline IDirect3DSurface9* EnvSurface = nullptr;
    inline const int HazeMax = 40;
    inline D3DXVECTOR4 HazeShape[HazeMax] = {}, HazeLook[HazeMax] = {};   // centre (NVR's space) and radius; optical depth
    inline int HazeCount = 0;
    inline const int BulletMax = 16;      // recent bullets, for the tunnels they clear through the haze and the other smoke (GunFX_GetBullets)
    inline D3DXVECTOR4 BulletFrom[BulletMax] = {}, BulletWay[BulletMax] = {};   // fired from (NVR's space), seconds since; way, how hard
    inline int BulletCount = 0;
    inline D3DXVECTOR2 BulletLook(0.0f, 0.0f);   // seconds a bullet's hole takes to open, and to fill in again (GunFX_GetBulletLook; 0 = the shader's own)
    inline float BulletMoving = 0.0f;            // how much you are moving, 0..1 (GunFX_GetMoving; 0 with an older GunFX)
    inline IDirect3DVertexDeclaration9* Declaration = nullptr;

    // What each smoke buffer holds, so a frame only clears the ones with something in them. Step 2 reads every buffer, so each
    // must read as empty wherever nothing is drawn into it; but a buffer nothing was drawn into since its last clear already
    // is. Dirty: drawn into since the last clear, or never cleared. Known: the surface the flag is about; a surface that was
    // created again is not it, so it counts as dirty and is cleared once. (Unsure: clear.)
    struct BufferState {
        IDirect3DSurface9* Known = nullptr;
        bool Dirty = true;
        bool NeedsClear(IDirect3DSurface9* surface) const { return Dirty || Known != surface; }
        void Cleared(IDirect3DSurface9* surface) { Known = surface; Dirty = false; }
        void Drawn(IDirect3DSurface9* surface) { Known = surface; Dirty = true; }
        void Forget() { Known = nullptr; Dirty = true; }
    };
    inline BufferState FullState, HalfState, LowState, HazeState;   // one per buffer: it can switch between full and half a frame

    // The effect's technique and parameter handles, looked up by name once per effect object instead of a dozen times a
    // frame. The effect is built again when it reloads (EffectRecord::LoadEffect, e.g. after its .hlsl changed), and its
    // handles can differ then, so the cache holds a reference to the effect it belongs to: while that object is alive no
    // new effect can take its address, so a different pointer is always a different (reloaded) effect. A name the effect
    // does not have stays a NULL handle, and D3DX ignores set calls on NULL handles, as before.
    struct EffectHandles {
        ID3DXEffect* Owner = nullptr;
        D3DXHANDLE SegmentsTechnique = NULL, HazeTechnique = NULL, EnvTechnique = NULL;
        D3DXHANDLE SmokeInfo = NULL, SmokeParams = NULL, SmokeFlash = NULL, SmokeFlashColor = NULL, SmokeLook = NULL;
        D3DXHANDLE BulletFrom = NULL, BulletWay = NULL, BulletMove = NULL, HazeShape = NULL, HazeLook = NULL, HazeInfo = NULL, EnvInfo = NULL;
    };
    inline EffectHandles CachedHandles;
    inline const EffectHandles& ResolveHandles(ID3DXEffect* effect) {
        if (CachedHandles.Owner == effect) return CachedHandles;
        EffectHandles found;
        found.Owner = effect;
        found.SegmentsTechnique = effect->GetTechniqueByName("VolumetricSmokeSegments");
        found.HazeTechnique = effect->GetTechniqueByName("VolumetricSmokeHaze");
        found.EnvTechnique = effect->GetTechniqueByName("VolumetricSmokeEnv");
        found.SmokeInfo = effect->GetParameterByName(NULL, "SmokeInfo");
        found.SmokeParams = effect->GetParameterByName(NULL, "SmokeParams");
        found.SmokeFlash = effect->GetParameterByName(NULL, "SmokeFlash");
        found.SmokeFlashColor = effect->GetParameterByName(NULL, "SmokeFlashColor");
        found.SmokeLook = effect->GetParameterByName(NULL, "SmokeLook");
        found.BulletFrom = effect->GetParameterByName(NULL, "BulletFrom");
        found.BulletWay = effect->GetParameterByName(NULL, "BulletWay");
        found.BulletMove = effect->GetParameterByName(NULL, "BulletMove");
        found.HazeShape = effect->GetParameterByName(NULL, "HazeShape");
        found.HazeLook = effect->GetParameterByName(NULL, "HazeLook");
        found.HazeInfo = effect->GetParameterByName(NULL, "HazeInfo");
        found.EnvInfo = effect->GetParameterByName(NULL, "EnvInfo");
        effect->AddRef();
        if (CachedHandles.Owner) CachedHandles.Owner->Release();
        CachedHandles = found;
        return CachedHandles;
    }

    // Clears one of the buffers (the render target is changed to it).
    inline void ClearBuffer(IDirect3DDevice9* device, IDirect3DSurface9* surface, BufferState& state) {
        device->SetRenderTarget(0, surface);
        device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        state.Cleared(surface);
    }

    // UNOFFICIAL 2026-11-12 (memleak), with [Main.Main.Performance] TargetReleaseSeconds above 0: the buffers (a full-screen and a
    // half-screen half-float target and three small ones: 23 MB at 1080p, 92 MB at 4K) were made at start-up for every player, whether
    // GunFX ever makes smoke or not (it is off by default). Now they are made the first frame GunFX hands over smoke to draw (Fetch), and
    // kept from then on. BuffersLazy: not made at start-up; BuffersTried: MakeBuffers ran (made, or failed: not tried again).
    inline bool BuffersLazy = false, BuffersTried = false;
    void MakeBuffers();   // VolumetricSmoke.cpp

    // Segments and haze for this frame (fetched once; ShouldRender and Render both ask). Returns how much there is to draw.
    inline int Fetch() {
        if (FetchedFrame == FrameNumber) return Count + CountBig + HazeCount;
        FetchedFrame = FrameNumber;
        // CPU time of the frame's one real fetch (the repeat calls return above at once): GunFX's data, the move into NVR's
        // space and the quads. Does nothing unless F10 profiling is on.
        static CpuTimer fetchCpuTimer("Smoke fetch (CPU)");
        CpuProfileScope fetchCpu(fetchCpuTimer);
        const ULONGLONG now = GetTickCount64();
        // GunFX's volumetric smoke: 16 floats per record (with the smoke's own noise coordinate and the age at both ends), the muzzle
        // flash's light, the haze, the bullets. 2026-11-16 (gunfx-builtin): GunFX is built in (src/gunfx/GunFX.h), so these are its
        // functions themselves, always the newest versions (before: GunFX.dll's GunFX_* exports, found with GetProcAddress).
        static int (* const get)(float*, int, float*) = &GunFX::GetVolumeSmoke2;
        static void (* const getFlash)(float*) = &GunFX::GetSmokeFlash;
        static int (* const getHaze)(float*, int) = &GunFX::GetSmokeHaze;
        static void (* const setView)(const float*) = &GunFX::SetView;
        static int (* const getBullets)(float*, int) = &GunFX::GetBullets;
        static void (* const getBulletLook)(float*) = &GunFX::GetBulletLook;
        static float (* const getMoving)() = &GunFX::GetMoving;
        static const int stride = 16;
        // The camera, for GunFX's bullets (they fly to where you aim): the eye in world space (NVR's space is centred on
        // it, so it is CameraPosition) and the way it looks (the inverse view's third row).
        if (setView) {
            const D3DXMATRIX& view = TheRenderManager->InvViewMatrix;
            const D3DXVECTOR4& eye = TheRenderManager->CameraPosition;
            const float l = sqrtf(view._31 * view._31 + view._32 * view._32 + view._33 * view._33);
            if (l > 0.5f) {
                const float v[6] = { eye.x, eye.y, eye.z, view._31 / l, view._32 / l, view._33 / l };
                setView(v);
            }
        }
        Count = CountBig = HazeCount = BulletCount = 0;
        const bool unmade = BuffersLazy && !BuffersTried;   // (2026-11-12 memleak) the buffers wait for the first smoke
        if (!get || (!FullSurface && !unmade)) return 0;
        static float records[FetchMax * 16];
        float params[8] = {};
        int total = get(records, FetchMax, params);
        if (total < 0 || total > FetchMax) total = 0;
        // The haze: 8 floats a blob (centre x, y, z absolute, radius, optical depth through the middle, 3 spare).
        static float haze[HazeMax * 8];
        // Menu switches (Main > GunFX) for the two most expensive parts: off, they are not fetched, so their passes are skipped.
        const bool hazeOn = TheSettingManager->GetSettingI("Main.GunFX.Main", "SmokeHaze") != 0;
        const bool tunnelsOn = TheSettingManager->GetSettingI("Main.GunFX.Main", "BulletTunnels") != 0;
        int hazeTotal = hazeOn && getHaze && ((HazeSurface && EnvSurface) || unmade) ? getHaze(haze, HazeMax) : 0;
        if (hazeTotal < 0 || hazeTotal > HazeMax) hazeTotal = 0;
        if (!total && !hazeTotal) return 0;
        if (unmade) {   // (2026-11-12 memleak) the first smoke of the session: its buffers now
            MakeBuffers();
            if (!FullSurface) return 0;
            if (!HazeSurface || !EnvSurface) hazeTotal = 0;
            if (!total && !hazeTotal) return 0;
        }
        // GunFX's points are absolute world positions; NVR's world space is centred on the camera (objects are drawn at
        // their position minus CameraPosition, and the eye sits at the inverse view's origin). Convert every end into it.
        const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
        const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
        for (int i = 0; i < total; i++) {
            float* r = records + i * stride;
            for (int e = 0; e < 2; e++) {
                r[e * 4] += inv._41 - camera.x;
                r[e * 4 + 1] += inv._42 - camera.y;
                r[e * 4 + 2] += inv._43 - camera.z;
            }
        }
        const float sx = TheRenderManager->InvProjMatrix._11 != 0.0f ? 1.0f / TheRenderManager->InvProjMatrix._11 : 1.0f;
        const float sy = TheRenderManager->InvProjMatrix._22 != 0.0f ? 1.0f / TheRenderManager->InvProjMatrix._22 : 1.0f;
        float bounds[4] = {};
        Shaded = 0.0f;
        const bool lowRes = LowSurface && TheSettingManager->GetSettingI("Main.GunFX.Main", "VolumetricSmokeLowRes");
        if (total)
            Count = VolumetricSmokeQuads::Build(records, stride, total, TheRenderManager->ViewProjMatrix, sx, sy, NearFade, Vertices, FetchMax, bounds,
                lowRes ? VerticesBig : nullptr, &CountBig, BigArea, &Shaded);
        for (int i = 0; i < hazeTotal; i++) {
            const float* b = haze + i * 8;
            if (b[3] <= 0.0f || b[4] <= 0.0f) continue;
            HazeShape[HazeCount] = D3DXVECTOR4(b[0] + inv._41 - camera.x, b[1] + inv._42 - camera.y, b[2] + inv._43 - camera.z, b[3]);
            HazeLook[HazeCount] = D3DXVECTOR4(b[4], 0.0f, 0.0f, 0.0f);
            HazeCount++;
        }
        // The bullets matter wherever there is something to cut: haze (its own pass) or the other smoke (the composite).
        if ((Count || CountBig || HazeCount) && getBullets && tunnelsOn) {
            static float bullets[BulletMax * 8];
            int n = getBullets(bullets, BulletMax);
            if (getBulletLook) { float look[2] = {}; getBulletLook(look); BulletLook = D3DXVECTOR2(look[0], look[1]); }
            if (n < 0 || n > BulletMax) n = 0;
            for (int i = 0; i < n; i++) {
                const float* b = bullets + i * 8;
                BulletFrom[BulletCount] = D3DXVECTOR4(b[0] + inv._41 - camera.x, b[1] + inv._42 - camera.y, b[2] + inv._43 - camera.z, b[3]);
                BulletWay[BulletCount] = D3DXVECTOR4(b[4], b[5], b[6], b[7]);
                BulletCount++;
            }
        }
        // How much you are moving: while you do, the shader leaves your own smoke close to you uncut.
        BulletMoving = getMoving ? getMoving() : 0.0f;
        if (!(BulletMoving >= 0.0f && BulletMoving <= 1.0f)) BulletMoving = 0.0f;
        // The muzzle flash (x, y, z absolute, light, r, g, b, reach), into NVR's space like the smoke.
        float flash[8] = {};
        if (getFlash) getFlash(flash);
        Flash = D3DXVECTOR4(flash[0] + inv._41 - camera.x, flash[1] + inv._42 - camera.y, flash[2] + inv._43 - camera.z, flash[3] > 0.0f ? flash[3] : 0.0f);
        FlashColor = D3DXVECTOR4(flash[4], flash[5], flash[6], flash[7]);
        TESObjectCELL* cell = Player ? Player->parentCell : nullptr;
        const bool outdoors = cell && (!cell->IsInterior() || (cell->flags0 & TESObjectCELL::kFlags0_BehaveLikeExterior));
        Info = D3DXVECTOR4((float)Count, params[4] > 0.5f ? 1.0f : 0.0f, outdoors ? 1.0f : 0.0f, params[5] > 0.5f ? 1.0f : 0.0f);
        Params = D3DXVECTOR4(params[0], params[1], params[2], params[3]);
        Look = D3DXVECTOR4(params[6], params[7], 1.0f, 0.0f);   // colour by thickness and age (older GunFX: 0, plain grey); z: buffer scale (Render)
        const float w = (float)TheRenderManager->width, h = (float)TheRenderManager->height;
        if (Count || CountBig) {
            Scissor.left = (LONG)(bounds[0] * w > 2.0f ? floorf(bounds[0] * w) - 2.0f : 0.0f);
            Scissor.top = (LONG)(bounds[1] * h > 2.0f ? floorf(bounds[1] * h) - 2.0f : 0.0f);
            Scissor.right = (LONG)(bounds[2] * w + 2.0f < w ? ceilf(bounds[2] * w) + 2.0f : w);
            Scissor.bottom = (LONG)(bounds[3] * h + 2.0f < h ? ceilf(bounds[3] * h) + 2.0f : h);
            if (Scissor.right <= Scissor.left || Scissor.bottom <= Scissor.top) Count = CountBig = 0;
        }
        if (HazeCount) Scissor = { 0, 0, (LONG)w, (LONG)h };   // the haze is all around you: step 2 covers the screen
        // Where the smoke is and what it costs (once a second, 60 times per session): for checking positions in the log.
        static ULONGLONG loggedAt = 0;
        static int logged = 0;
        if (logged < 60 && now - loggedAt >= 1000) {
            loggedAt = now;
            logged++;
            // (A segment in the band where it fades from the full-resolution list to the quarter-resolution one is drawn in
            // both, so the draws can come to more than the segments.)
            D3DXVECTOR4 clip;
            const D3DXVECTOR4 p(records[0], records[1], records[2], 1.0f);
            D3DXVec4Transform(&clip, &p, &TheRenderManager->ViewProjMatrix);
            const float depth = clip.w != 0.0f ? clip.w : 1.0f;
            Logger::Log("VolumetricSmoke: %d draws for %d segments (%d of them big ones at an eighth of the resolution), %d haze blobs, %.1f screens' worth of pixels shaded, area %ld,%ld-%ld,%ld of %.0fx%.0f; first segment end (%.1f %.1f %.1f) radius %.2f density %.4f -> view depth %.1f, screen (%.2f %.2f); %s%s",
                Count + CountBig, total, CountBig, HazeCount, Shaded, Scissor.left, Scissor.top, Scissor.right, Scissor.bottom, w, h,
                records[0] - inv._41 + camera.x, records[1] - inv._42 + camera.y, records[2] - inv._43 + camera.z, records[3], records[8],
                clip.w, 0.5f + 0.5f * clip.x / depth, 0.5f - 0.5f * clip.y / depth, outdoors ? "outdoors" : "indoors", Info.y > 0.5f ? ", debug view" : "");
        }
        return Count + CountBig + HazeCount;
    }
}

class VolumetricSmokeEffect : public EffectRecord {
public:
    VolumetricSmokeEffect() : EffectRecord("VolumetricSmoke") {}
    void RegisterTextures() override;   // VolumetricSmoke.cpp (needs the texture manager)
    // UNOFFICIAL 2026-10-25 (NVR DXVK): the haze, the room light and the composite as Vulkan passes (Compute\VolumetricSmoke*.frag,
    // the GLSL in VolumetricSmoke.glsl; VolumetricSmoke.cpp). The segments (geometry) stay D3D9. Render fills the constants once
    // (FillExtConstants) when the passes are on; ExtHaze and ExtEnv draw from Render, ExtRender is the composite (technique 0,
    // through the frame chain, as EffectRecord::Render calls it). A pass that cannot run takes its D3D9 path.
    bool ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
    bool ExtHaze(const D3DXVECTOR4& look);
    bool ExtEnv(const D3DXVECTOR4& envInfo);
    bool FillExtConstants(const D3DXVECTOR4& info, const D3DXVECTOR4& flash);
    bool ShouldRender() override { return VolumetricSmoke::Fetch() > 0; }
    void Render(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* rendered,
        UINT technique, bool clear, IDirect3DSurface9* source) override {
        // [Main.GunFX.Volume] holds the tuning; the switch is Main > GunFX > VolumetricSmoke.
        Enabled = Effect != nullptr;
        if (!Enabled || !ShouldRender()) { renderTime = 0; return; }
        using namespace VolumetricSmoke;
        // CPU timers (F10 profiling; they do nothing when it is off), to tell which call the CPU time goes to: the whole
        // step, the clears, the segment and big-segment draws, the haze, the room light and the composite. What the whole
        // step has beyond the others is state saving and setting constants (and the fetch, if this is the first call of the frame).
        static CpuTimer renderCpuTimer("Smoke render (CPU)"), clearsCpuTimer("Smoke clears (CPU)"),
            segmentsCpuTimer("Smoke seg draw (CPU)"), lowCpuTimer("Smoke low draw (CPU)"), hazeCpuTimer("Smoke haze (CPU)"),
            envCpuTimer("Smoke env (CPU)"), compositeCpuTimer("Smoke composite (CPU)");
        CpuProfileScope renderCpu(renderCpuTimer);
        // Test modes (Main > GunFX > VolumetricSmokeTest), for finding what the smoke changes in the image: 1 the smoke in
        // plain green, 2 no muzzle flash light on it, 3 worked out but not drawn (step 1 only), 4 nothing on the GPU.
        const int test = TheSettingManager->GetSettingI("Main.GunFX.Main", "VolumetricSmokeTest");
        if (test == 4) { renderTime = 0; return; }
        const EffectHandles& fx = ResolveHandles(Effect);
        D3DXHANDLE segments = fx.SegmentsTechnique;
        if (!segments) return;
        if (!Declaration) {
            const D3DVERTEXELEMENT9 elements[] = {
                { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
                { 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
                { 0, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
                { 0, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
                D3DDECL_END() };
            if (FAILED(device->CreateVertexDeclaration(elements, &Declaration))) { Declaration = nullptr; return; }
        }
        // Half resolution (a quarter of the pixels; step 2 blends it up smoothly), or full.
        const bool half = HalfSurface && TheSettingManager->GetSettingI("Main.GunFX.Main", "VolumetricSmokeHalfRes");
        Texture = half ? HalfTexture : FullTexture;
        Surface = half ? HalfSurface : FullSurface;
        Look.z = half ? 2.0f : 1.0f;                          // the shader's pixel size in step 1
        Look.w = (half || CountBig) ? 1.0f : 0.0f;            // step 2: hide smoke behind each pixel's surface (low resolution only)
        static GpuTimer segmentsTimer("Volumetric smoke: segments"), compositeTimer("Volumetric smoke: composite"),
            hazeTimer("Volumetric smoke: haze");
        D3DXVECTOR4 info = Info, flash = Flash;
        info.x = test == 1 ? -1.0f : 0.0f;                    // x: the shader's test colour (it has no other use for x)
        if (test == 2) flash.w = 0.0f;
        // UNOFFICIAL 2026-10-25 (NVR DXVK): the constants of the Vulkan passes (the haze, the room light, the composite), as the D3D9
        // path sets them below; extReady when they are all there and the passes are on (nothing is drawn with them otherwise).
        extReady = DxvkCompute::PassesOn() && !ExtFailed && FillExtConstants(info, flash);
        Effect->SetVector(fx.SmokeInfo, &info);
        Effect->SetVector(fx.SmokeParams, &Params);
        Effect->SetVector(fx.SmokeFlash, &flash);
        Effect->SetVector(fx.SmokeFlashColor, &FlashColor);
        Effect->SetVector(fx.SmokeLook, &Look);
        // Bullets cut holes through the haze (the haze pass) and through the other smoke (the composite), so HazeInfo (how
        // many blobs, how many bullets) and the bullets are set on every frame that draws, also when there is smoke but no haze
        // (no blobs then and the haze pass does not run, but the composite still reads the bullets). The effect keeps its
        // constants from frame to frame, so the counts are set every time (a stale count would cut with last frame's
        // bullets); the arrays only when there are bullets (the counts say how many entries the shader reads).
        const D3DXVECTOR4 hazeInfo((float)HazeCount, (float)BulletCount, BulletLook.x, BulletLook.y);   // z, w: the holes' timing
        Effect->SetVector(fx.HazeInfo, &hazeInfo);
        if (fx.BulletMove) { const D3DXVECTOR4 move(BulletMoving, 0.0f, 0.0f, 0.0f); Effect->SetVector(fx.BulletMove, &move); }
        if (BulletCount) {
            Effect->SetVectorArray(fx.BulletFrom, BulletFrom, BulletMax);
            Effect->SetVectorArray(fx.BulletWay, BulletWay, BulletMax);
        }

        // Step 1: the segments' optical depth into TESR_SmokeBuffer. Everything changed here is put back afterwards
        // (the effect's Begin/End restores the render states it sets).
        IDirect3DSurface9* previousTarget = nullptr;
        IDirect3DVertexDeclaration9* previousDeclaration = nullptr;
        IDirect3DVertexBuffer9* previousStream = nullptr;
        UINT previousOffset = 0, previousStride = 0;
        device->GetRenderTarget(0, &previousTarget);
        device->GetVertexDeclaration(&previousDeclaration);
        device->GetStreamSource(0, &previousStream, &previousOffset, &previousStride);
        device->SetRenderTarget(0, Surface);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        // Step 2 always reads all the buffers, so each must be empty wherever nothing is drawn into it this frame; one is
        // cleared only if it holds something (BufferState: drawn into since its last clear, or new), not every frame. The
        // haze pass writes every pixel of its buffer (a full-screen quad, no scissor, no blending, no discard), so on a frame
        // it runs, its buffer needs no clear first; if it turns out not to draw, it is cleared there instead.
        BufferState& mainState = half ? HalfState : FullState;
        const bool hazeOverwrites = HazeCount && previousStream && HazeSurface;
        {
            CpuProfileScope clearsCpu(clearsCpuTimer);
            bool movedTarget = false;
            if (mainState.NeedsClear(Surface)) {
                device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
                mainState.Cleared(Surface);
            }
            if (LowSurface && LowState.NeedsClear(LowSurface)) { ClearBuffer(device, LowSurface, LowState); movedTarget = true; }
            if (HazeSurface && !hazeOverwrites && HazeState.NeedsClear(HazeSurface)) { ClearBuffer(device, HazeSurface, HazeState); movedTarget = true; }
            if (movedTarget) device->SetRenderTarget(0, Surface);
        }
        Effect->SetTechnique(segments);
        SetCT();
        device->SetTexture(2, nullptr);   // TESR_SmokeBuffer, TESR_SmokeBufferLow, TESR_SmokeHaze and TESR_SmokeEnv are the
        device->SetTexture(4, nullptr);   // targets here
        device->SetTexture(5, nullptr);
        device->SetTexture(6, nullptr);
        for (UInt32 stage : { 2u, 4u, 5u, 6u }) TheRenderManager->renderState->NoteDeviceTexture(stage, nullptr);	// UNOFFICIAL 2026-10-27: keep the game's texture cache truthful (ManyLampsGlow's s4)
        UINT passes = 0;
        {
            GpuProfileScope segmentsScope(segmentsTimer, device);
            if (Count) {
                CpuProfileScope segmentsCpu(segmentsCpuTimer);   // Begin to End: the draw's own copy and any wait for the GPU show here
                mainState.Drawn(Surface);
                if (SUCCEEDED(Effect->Begin(&passes, 0))) {
                    Effect->BeginPass(0);
                    device->SetVertexDeclaration(Declaration);
                    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, Count * 2, Vertices, sizeof(VolumetricSmokeQuads::Vertex));
                    Effect->EndPass();
                    Effect->End();
                }
            }
            // The big segments, at an eighth of the resolution (the shader's pixel size follows Look.z).
            if (CountBig && LowSurface) {
                CpuProfileScope lowCpu(lowCpuTimer);
                LowState.Drawn(LowSurface);
                device->SetRenderTarget(0, LowSurface);
                const D3DXVECTOR4 lowLook(Look.x, Look.y, 8.0f, 1.0f);
                Effect->SetVector(fx.SmokeLook, &lowLook);
                if (SUCCEEDED(Effect->Begin(&passes, 0))) {
                    Effect->BeginPass(0);
                    device->SetVertexDeclaration(Declaration);
                    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, CountBig * 2, VerticesBig, sizeof(VolumetricSmokeQuads::Vertex));
                    Effect->EndPass();
                    Effect->End();
                }
                Effect->SetVector(fx.SmokeLook, &Look);
            }
        }
        if (previousDeclaration) device->SetVertexDeclaration(previousDeclaration);
        device->SetStreamSource(0, previousStream, previousOffset, previousStride);
        // The haze (every blob at once, at quarter resolution) and the room light step 2 lights the smoke with (one pixel):
        // full-screen passes over the effect chain's frame quad (bound when the effects started, put back just above).
        if (previousStream) {
            // Returns whether it drew.
            auto fullScreen = [&](D3DXHANDLE pass, IDirect3DSurface9* into) -> bool {
                if (!pass || !into) return false;
                device->SetRenderTarget(0, into);
                Effect->SetTechnique(pass);
                if (!SUCCEEDED(Effect->Begin(&passes, 0))) return false;
                Effect->BeginPass(0);
                device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
                Effect->EndPass();
                Effect->End();
                return true;
            };
            // It also works out the bullets' cut of the segment smoke (its second target, TESR_SmokeCut, read by step 2), so
            // it runs whenever there are bullets and segment smoke, haze or not.
            const bool cutNeeded = BulletCount && (Count || CountBig) && CutSurface;
            if (HazeCount || cutNeeded) {
                GpuProfileScope hazeScope(hazeTimer, device);
                CpuProfileScope hazeCpu(hazeCpuTimer);
                const D3DXVECTOR4 hazeLook(Look.x, Look.y, 4.0f, 1.0f);
                Effect->SetVectorArray(fx.HazeShape, HazeShape, HazeMax);
                Effect->SetVectorArray(fx.HazeLook, HazeLook, HazeMax);
                Effect->SetVector(fx.SmokeLook, &hazeLook);
                if (HazeSurface) HazeState.Drawn(HazeSurface);
                // UNOFFICIAL 2026-10-25 (NVR DXVK): the haze as a Vulkan pass into both targets (ExtHaze); the D3D9 pass otherwise.
                bool drew = extReady && ExtHaze(hazeLook);
                if (!drew) {
                    // The cut reads the segment smoke just drawn; TESR_SmokeCut is a target here.
                    device->SetTexture(2, Texture);
                    device->SetTexture(4, LowSurface ? LowTexture : nullptr);
                    device->SetTexture(7, nullptr);
                    TheRenderManager->renderState->NoteDeviceTexture(2, Texture);	// UNOFFICIAL 2026-10-27: the game's texture cache, as above
                    TheRenderManager->renderState->NoteDeviceTexture(4, LowSurface ? LowTexture : nullptr);
                    TheRenderManager->renderState->NoteDeviceTexture(7, nullptr);
                    DWORD oldWriteMask1 = 0xF;
                    device->GetRenderState(D3DRS_COLORWRITEENABLE1, &oldWriteMask1);
                    if (CutSurface) {
                        device->SetRenderTarget(1, CutSurface);
                        device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
                    }
                    // It was left uncleared because this pass overwrites it (hazeOverwrites above); if it did not draw, clear it now.
                    drew = fullScreen(fx.HazeTechnique, HazeSurface);
                    if (CutSurface) {
                        device->SetRenderTarget(1, nullptr);
                        device->SetRenderState(D3DRS_COLORWRITEENABLE1, oldWriteMask1);
                    }
                    device->SetTexture(2, nullptr);
                    device->SetTexture(4, nullptr);
                    TheRenderManager->renderState->NoteDeviceTexture(2, nullptr);	// UNOFFICIAL 2026-10-27: the game's texture cache, as above
                    TheRenderManager->renderState->NoteDeviceTexture(4, nullptr);
                    if (!drew && HazeSurface) ClearBuffer(device, HazeSurface, HazeState);
                }
                Effect->SetVector(fx.SmokeLook, &Look);
            }
            {   // the room light eases to this frame's value over about a third of a second (at once after a pause)
                CpuProfileScope envCpu(envCpuTimer);
                LARGE_INTEGER now, frequency;
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&frequency);
                const double dt = EnvAt.QuadPart ? double(now.QuadPart - EnvAt.QuadPart) / double(frequency.QuadPart) : 1e9;
                EnvAt = now;
                const D3DXVECTOR4 envInfo(dt > 0.0 && dt < 1.0 ? 1.0f - expf(-(float)dt / 0.3f) : 1.0f, 0.0f, 0.0f, 0.0f);
                Effect->SetVector(fx.EnvInfo, &envInfo);
                if (envInfo.x >= 1.0f && EnvSurface) {        // starting over: a clean buffer to blend into (never undefined)
                    device->SetRenderTarget(0, EnvSurface);
                    device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
                }
                // UNOFFICIAL 2026-10-25 (NVR DXVK): as a Vulkan pass (ExtEnv) when it can; the D3D9 pass otherwise.
                if (!(extReady && ExtEnv(envInfo))) fullScreen(fx.EnvTechnique, EnvSurface);
            }
        }
        device->SetRenderTarget(0, previousTarget);
        if (previousTarget) previousTarget->Release();
        if (previousDeclaration) previousDeclaration->Release();
        if (previousStream) previousStream->Release();

        // Step 2: light the total and lay it over the image, inside the smoke's area. When that area is most of the screen
        // (always with haze), the whole screen instead, without the scissor: the composite writes every pixel it draws (no
        // discard, no blending), so the frame chain need not first copy the whole frame into its target for the pixels the
        // scissor would leave alone -- a full-screen copy (about 0.3 ms at 2560x1440) that costs about as much as drawing
        // the rest of the screen.
        const long long screen = (long long)TheRenderManager->width * TheRenderManager->height;
        const bool whole = 2LL * (Scissor.right - Scissor.left) * (Scissor.bottom - Scissor.top) >= screen;
        if (!whole) {
            device->SetScissorRect(&Scissor);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        }
        if (test != 3) {
            GpuProfileScope composite(compositeTimer, device);
            CpuProfileScope compositeCpu(compositeCpuTimer);
            writesWholeTarget = whole;
            EffectRecord::Render(device, target, rendered, technique, clear, source);
            writesWholeTarget = false;
        }
        if (!whole) device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }

    // UNOFFICIAL 2026-10-25 (NVR DXVK): the Constants block of Compute\VolumetricSmoke.glsl (std140, the same order: three matrices,
    // then the float4s, then the arrays). The matrices are copied as the D3D9 effect's registered memory holds them (SetMatrix keeps
    // the logical matrix; see VolumetricLight.glsl).
    struct ExtConstantsStruct {
        D3DXMATRIX	InvView, InvProjection, Projection;
        D3DXVECTOR4	ReciprocalResolution, SunDirection, SunColor, SunAmbient, CameraData, CameraPosition;
        D3DXVECTOR4	Info, Params, Flash, FlashColor, Look, HazeInfo, BulletMove, EnvInfo;
        D3DXVECTOR4	HazeShape[VolumetricSmoke::HazeMax], HazeLook[VolumetricSmoke::HazeMax];
        D3DXVECTOR4	BulletFrom[VolumetricSmoke::BulletMax], BulletWay[VolumetricSmoke::BulletMax];
    };
    ExtConstantsStruct	extConstants = {};
    bool				extReady = false;	// this frame's constants are filled and the passes are on (set by Render)
    ExtPass				extHazePass, extEnvPass, extCompositePass;
    ExtUniformBuffer	extUniforms;
};
