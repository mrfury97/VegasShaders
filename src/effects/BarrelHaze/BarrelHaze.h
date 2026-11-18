#pragma once
#include "../../gunfx/GunFX.h"	// 2026-11-16 (gunfx-builtin): GunFX built in; its haze by direct calls

// Captured from the gun's actual MVP, including the first-person projection.
namespace BarrelHaze {
    inline D3DXVECTOR4 Line = {}, Data = {}, Animation = {}, Blast = {};
    inline ULONGLONG Captured = 0;
    inline void Reset() { Captured = 0; Data.x = 0; Blast.x = 0; }

    // World position -> screen UV and view depth, with the scene camera. The first-person gun is drawn with the same
    // camera but the first-person field of view (Main::RenderFirstPerson 0x875110 calls BSSceneGraph::SetCameraFOV
    // with PlayerCharacter::Get1stPersonFOV), which is already in the camera's frustum during that pass.
    inline bool ProjectWorld(const D3DXVECTOR3& p, bool firstPerson, float& u, float& v, float& depth) {
        NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
        if (!camera) return false;
        const NiMatrix33& r = camera->m_worldTransform.rot;
        const NiPoint3& c = camera->m_worldTransform.pos;
        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        const float forward = r.data[0][0] * dx + r.data[1][0] * dy + r.data[2][0] * dz;
        const float up = r.data[0][1] * dx + r.data[1][1] * dy + r.data[2][1] * dz;
        const float right = r.data[0][2] * dx + r.data[1][2] * dy + r.data[2][2] * dz;
        if (!std::isfinite(forward) || forward <= 0.5f) return false;
        // The camera's own frustum: in the first-person pass the game has already set it to the first-person FOV.
        const float left = camera->Frustum.Left, rightEdge = camera->Frustum.Right;
        const float top = camera->Frustum.Top, bottom = camera->Frustum.Bottom;
        if (rightEdge - left <= 1e-4f || top - bottom <= 1e-4f) return false;
        const float x = (2.0f * right / forward - (rightEdge + left)) / (rightEdge - left);
        const float y = (2.0f * up / forward - (top + bottom)) / (top - bottom);
        u = 0.5f + 0.5f * x;
        v = 0.5f - 0.5f * y;
        depth = forward;
        return std::isfinite(u) && std::isfinite(v);
    }

    // Why a capture did not happen (logged at most once a second, 12 times per view).
    inline void LogMiss(bool firstPerson, const char* why, const D3DXVECTOR3& p) {
        static unsigned count[2] = {};
        static ULONGLONG at[2] = {};
        const unsigned view = firstPerson ? 0 : 1;
        const ULONGLONG now = GetTickCount64();
        if (count[view] >= 12 || now - at[view] < 1000) return;
        count[view]++; at[view] = now;
        NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
        const float cx = camera ? camera->m_worldTransform.pos.x : 0, cy = camera ? camera->m_worldTransform.pos.y : 0,
            cz = camera ? camera->m_worldTransform.pos.z : 0;
        Logger::Log("BarrelHaze %s: no capture (%s); muzzle (%.1f %.1f %.1f), camera (%.1f %.1f %.1f), FOV %.1f, frustum width %.3f",
            firstPerson ? "first-person" : "third-person", why, p.x, p.y, p.z, cx, cy, cz,
            Player ? Player->firstPersonFoV : -1.0f, camera ? camera->Frustum.Right - camera->Frustum.Left : -1.0f);
    }

    // state: the plugin's heat data in world space (muzzle xyz, heat, rearward axis xyz, ...).
    inline void CaptureWorld(const float* state, bool firstPerson, const void* weaponNode) {
        // V2 also gives the muzzle in world space from the game loop: while the first-person model is drawn, its
        // nodes are in another space (the muzzle read about (61, 59, 121) with the camera at (2818, 1914, 7543)).
        // V3 adds the muzzle blast (out[14] pixels now, out[15] radius). (2026-11-16: GunFX is built in: always V3.)
        float haze[16] = {};
        D3DXVECTOR3 muzzle(state[0], state[1], state[2]), axis(state[4], state[5], state[6]);
        if (!GunFX::GetHazeV3(haze, weaponNode)) { LogMiss(firstPerson, "no haze data for this gun", muzzle); return; }
        const float blast = haze[14];
        if (haze[0] <= 0.001f && blast <= 0.01f) return;
        if (firstPerson) {
            muzzle = D3DXVECTOR3(haze[8], haze[9], haze[10]);
            axis = D3DXVECTOR3(haze[11], haze[12], haze[13]);
        }
        const float length = D3DXVec3Length(&axis);
        if (!std::isfinite(length) || length < 1e-4f) { LogMiss(firstPerson, "no barrel direction", muzzle); return; }
        const D3DXVECTOR3 rear = muzzle + axis * (haze[5] / length);
        D3DXVECTOR4 line;
        float depth = 0, rearDepth = 0;
        if (!ProjectWorld(muzzle, firstPerson, line.x, line.y, depth) || !ProjectWorld(rear, firstPerson, line.z, line.w, rearDepth)) {
            LogMiss(firstPerson, "barrel behind the camera", muzzle); return;
        }
        if ((line.x < -0.2f && line.z < -0.2f) || (line.x > 1.2f && line.z > 1.2f) ||
            (line.y < -0.2f && line.w < -0.2f) || (line.y > 1.2f && line.w > 1.2f)) {
            LogMiss(firstPerson, "barrel off screen", muzzle); return;
        }
        Line = line;
        Data = D3DXVECTOR4(haze[0], haze[1], haze[2], haze[3]);
        Captured = GetTickCount64();
        Animation = D3DXVECTOR4(float(Captured % 3600000) * 0.001f, haze[4], haze[6], depth);
        Blast = D3DXVECTOR4(blast, haze[15], 0.0f, 0.0f);
    }

    inline void Capture(const float* local, float scale, const void* weaponNode) {
        float haze[8] = {}, matrix[16];
        if (!GunFX::GetHaze(haze, weaponNode) || haze[0] <= 0.001f ||
            FAILED(TheRenderManager->device->GetVertexShaderConstantF(0, matrix, 4))) return;
        float clipDepth = 0;
        auto project = [&](const D3DXVECTOR3& p, float& u, float& v) {
            const float w = matrix[12]*p.x + matrix[13]*p.y + matrix[14]*p.z + matrix[15];
            if (!std::isfinite(w) || w <= 0.001f) return false;
            u = 0.5f + 0.5f*(matrix[0]*p.x + matrix[1]*p.y + matrix[2]*p.z + matrix[3])/w;
            v = 0.5f - 0.5f*(matrix[4]*p.x + matrix[5]*p.y + matrix[6]*p.z + matrix[7])/w;
            clipDepth = w;
            return std::isfinite(u) && std::isfinite(v);
        };
        D3DXVECTOR3 muzzle(local[0], local[1], local[2]);
        D3DXVECTOR3 rear = muzzle + D3DXVECTOR3(local[4], local[5], local[6]) * (haze[5] / scale);
        D3DXVECTOR4 line;
        if (!project(muzzle, line.x, line.y) || !project(rear, line.z, line.w)) return;
        if ((line.x < -0.2f && line.z < -0.2f) || (line.x > 1.2f && line.z > 1.2f) ||
            (line.y < -0.2f && line.w < -0.2f) || (line.y > 1.2f && line.w > 1.2f)) return;
        Line = line;
        Data = D3DXVECTOR4(haze[0], haze[1], haze[2], haze[3]);
        Captured = GetTickCount64();
        Animation = D3DXVECTOR4(float(Captured % 3600000) * 0.001f, haze[4], haze[6], clipDepth);
    }
}

class BarrelHazeEffect : public EffectRecord {
public:
    BarrelHazeEffect() : EffectRecord("BarrelHaze") {}
    // UNOFFICIAL 2026-10-25 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\BarrelHaze.frag (EffectRecord::ExtRender)
    bool ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
    ExtPass extPass;
    bool ShouldRender() override {
        return BarrelHaze::Captured && GetTickCount64() - BarrelHaze::Captured < 200 &&
            (BarrelHaze::Data.x > 0.001f || BarrelHaze::Blast.x > 0.01f);
    }
    void Render(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* rendered,
        UINT technique, bool clear, IDirect3DSurface9* source) override {
        // [Main.GunFX.Haze] / [Blast] hold the tuning; the on/off switches are in the menu (Gun effects) and reach GunFX
        // through GunFX_SetSwitches, which then publishes no haze/blast when they are off.
        Enabled = Effect != nullptr;
        if (!Enabled || !ShouldRender()) { renderTime = 0; return; }
        Effect->SetVector(Effect->GetParameterByName(NULL, "HazeLine"), &BarrelHaze::Line);
        Effect->SetVector(Effect->GetParameterByName(NULL, "HazeData"), &BarrelHaze::Data);
        Effect->SetVector(Effect->GetParameterByName(NULL, "HazeAnimation"), &BarrelHaze::Animation);
        Effect->SetVector(Effect->GetParameterByName(NULL, "HazeBlast"), &BarrelHaze::Blast);
        EffectRecord::Render(device, target, rendered, technique, clear, source);
    }
};
