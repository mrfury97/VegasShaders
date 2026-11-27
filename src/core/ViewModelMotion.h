#pragma once

// UNOFFICIAL 2026-11-21 ViewModelMotion: 1st-person viewmodel temporal AA support (DLAA / DLSS / DLSS-RR / FSR 2 / FSR 3.1 / TAA).
//
// Two halves, strictly isolated from TESR_ObjectMotionBuffer so world-space readers (AmbientLight, Reflections, RtDenoise, RtPathTrace)
// are never affected:
//   1. Sub-pixel camera jitter during Main::RenderFirstPerson: hooks NiDX9Renderer::Do_SetCameraData (vtable 0x010EE648 -> 0x00E6C780)
//      and shifts a LOCAL copy of the frustum by TAA's / RenderScale's exact (JitterX, JitterY) offset when drawing into the scene
//      target. Leaves camera->Frustum itself untouched so DeferredFirstPerson::FrustumWhy and BarrelHaze::ProjectWorld never trip.
//   2. Dedicated screen-space UV motion buffer (TESR_ViewModelMotionBuffer, A16B16G16R16F, cleared to 0):
//        rg = previous unjittered UV - current unjittered UV (same sign and viewport units as FSRPrep / FSRUpPrep / TAA)
//        b  = 1.0 when valid this frame, 0.0 when untracked / first frame / weapon swap / cut
//        a  = 1.0 when valid
//      Rendered right after ResolveViewModelDepth() using a dedicated D24X8 scratch depth buffer (never altering the game's
//      depth-stencil) plus a view-Z clip against TESR_DepthBufferViewModel.
#include <d3d9.h>
#include <d3dx9.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#include "CapturedNormals.h"
#include "RenderPass.h"

namespace ViewModelMotion {

	inline IDirect3DTexture9* Texture = nullptr;	// TESR_ViewModelMotionBuffer
	inline IDirect3DSurface9* Surface = nullptr;
	inline IDirect3DSurface9* ScratchDepth = nullptr;
	inline UINT MadeW = 0, MadeH = 0;

	inline IDirect3DVertexShader9* VS = nullptr;
	inline IDirect3DPixelShader9* PS = nullptr;
	inline bool Failed = false;

	// x = 1 when TESR_ViewModelMotionBuffer holds this frame's data, y = drawn geometries, z = 1 when 1st-person camera was jittered.
	inline D3DXVECTOR4 Data = { 0.0f, 0.0f, 0.0f, 0.0f };
	inline UInt32 ReadyFrame = 0xFFFFFFFFu;
	inline UInt32 JitteredFrame = 0xFFFFFFFFu;

	inline bool HooksInstalled = false;
	inline bool InFirstPerson = false;
	inline bool CaptureWanted = false;
	inline bool JitterWanted = false;
	inline bool JitterApplied = false;
	inline float JitterX = 0.0f, JitterY = 0.0f;
	inline UINT ViewportW = 0, ViewportH = 0;
	inline IDirect3DSurface9* SceneTarget = nullptr;
	inline bool TrackProj = false;
	inline IDirect3DSurface9* ProjTarget = nullptr;
	inline bool InvProjSwapped = false;
	inline D3DXMATRIX SavedWorldInvProj;

	inline bool Ready() {
		return !Failed && Data.x > 0.0f && Texture && TheFrameRateManager && ReadyFrame == TheFrameRateManager->FrameNumber;
	}
	inline bool JitteredThisFrame() {
		return TheFrameRateManager && JitteredFrame == TheFrameRateManager->FrameNumber;
	}
	inline bool WantedAtStartup() {
		SettingManager::Configuration::ConfigNode node;
		return !TheSettingManager->Config.FillNode(&node, "Shaders.TAA.Main", "WeaponAA") || node.BoolValue;
	}

	struct CamRecord {
		D3DXVECTOR3 eye = {};
		D3DXMATRIX curVP;
		D3DXMATRIX curJitVP;
		float nearZ = 1.0f, farZ = 1000.0f;
	};
	inline std::vector<CamRecord> PassCams;
	inline int CurrentCamIndex = -1;

	struct Entry {
		UInt32 seen = 0;
		bool skinned = false, hasPrev = false;
		NiTransform cur = {}, prev = {};
		D3DXVECTOR3 eyeCur = {}, eyePrev = {};
		D3DXMATRIX curVP, curJitVP, prevVP;
		float nearZ = 1.0f, farZ = 1000.0f;
		std::vector<float> bones, prevBones;	// 12 floats per bone (3 rows of 3x4), camera relative to eyeCur / eyePrev
		const void* data = nullptr;
		const void* skin = nullptr;
		const void* parent = nullptr;
	};
	inline std::unordered_map<NiGeometry*, Entry> Map;

	struct DrawRecord {
		NiGeometry* geo = nullptr;
		DWORD cull = D3DCULL_CCW;
	};
	inline std::vector<DrawRecord> PassDraws;
	inline UInt32 NoteFrame = 0;
	inline UInt32 LastPrune = 0;
	inline UInt32 Frames = 0, DrawnGeometries = 0, DrawnSkinned = 0;
	inline UInt32 StatNotedRigid = 0, StatNotedSkinned = 0;
	inline UInt32 StatDropCam = 0, StatDropRoot = 0, StatDropRT = 0, StatDropZ = 0, StatDropBuf = 0;
	inline bool MsaaLogged = false;
	inline bool ActiveLogged = false;

	class DrawPass : public RenderPass {};
	inline DrawPass* Drawer = nullptr;

	// Chained vtable hook on NiDX9Renderer::Do_SetCameraData (vtable slot 99, 0x010EE648 -> 0x00E6C780).
	typedef void(__thiscall* DoSetCameraDataFn)(NiDX9Renderer*, const NiPoint3*, const NiPoint3*, const NiPoint3*, const NiPoint3*, const NiFrustum*, const NiViewport*);
	inline DoSetCameraDataFn OriginalDoSetCameraData = nullptr;

	static void __fastcall DoSetCameraDataHook(NiDX9Renderer* This, void* edx,
		const NiPoint3* kWorldLoc, const NiPoint3* kWorldDir, const NiPoint3* kWorldUp, const NiPoint3* kWorldRight,
		const NiFrustum* kFrustum, const NiViewport* kPort)
	{
		if (!TrackProj || !This || !This->device || !kFrustum || kFrustum->Ortho || !ProjTarget) {
			OriginalDoSetCameraData(This, kWorldLoc, kWorldDir, kWorldUp, kWorldRight, kFrustum, kPort);
			return;
		}
		IDirect3DSurface9* curRT = nullptr;
		if (SUCCEEDED(This->device->GetRenderTarget(0, &curRT)) && curRT) curRT->Release();
		if (curRT != ProjTarget) {
			OriginalDoSetCameraData(This, kWorldLoc, kWorldDir, kWorldUp, kWorldRight, kFrustum, kPort);
			return;
		}
		const float fw = kFrustum->Right - kFrustum->Left;
		const float fh = kFrustum->Top - kFrustum->Bottom;
		if (!(fw > 1.0e-6f) || !(fh > 1.0e-6f) || !(kFrustum->Near > 0.0f) || !(kFrustum->Far > kFrustum->Near)) {
			OriginalDoSetCameraData(This, kWorldLoc, kWorldDir, kWorldUp, kWorldRight, kFrustum, kPort);
			return;
		}

		const bool doCapture = InFirstPerson && CaptureWanted && SceneTarget == curRT;
		NiFrustum f = *kFrustum;
		if (doCapture && JitterWanted && ViewportW > 0 && ViewportH > 0) {
			const float sx = JitterX * fw / (float)ViewportW;
			const float sy = JitterY * fh / (float)ViewportH;
			f.Left += sx;
			f.Right += sx;
			f.Top += sy;
			f.Bottom += sy;
			JitterApplied = true;
		}
		OriginalDoSetCameraData(This, kWorldLoc, kWorldDir, kWorldUp, kWorldRight, &f, kPort);

		const float p11 = fabsf(This->projMatrix._11);
		if (p11 > 1.0e-5f && TheFrameRateManager)
			CapturedNormals::SetViewModelTanX(1.0f / p11, TheFrameRateManager->FrameNumber);
		D3DXMATRIX fpInvProj;
		if (TheRenderManager && D3DXMatrixInverse(&fpInvProj, nullptr, &This->projMatrix)) {
			TheRenderManager->InvProjMatrix = fpInvProj;
			This->device->SetVertexShaderConstantF(100, (const float*)&fpInvProj, 4);
			This->device->SetVertexShaderConstantF(240, (const float*)&fpInvProj, 4);
			InvProjSwapped = true;
		}

		if (!doCapture || !kWorldLoc) return;

		D3DXMATRIX camRelView = This->viewMatrix;
		camRelView._41 = camRelView._42 = camRelView._43 = 0.0f;
		camRelView._44 = 1.0f;
		const D3DXMATRIX& jitProj = This->projMatrix;
		D3DXMATRIX unjitProj = This->projMatrix;
		unjitProj._31 = -(kFrustum->Right + kFrustum->Left) / fw;
		unjitProj._32 = -(kFrustum->Top + kFrustum->Bottom) / fh;

		CamRecord rec;
		rec.eye = D3DXVECTOR3(kWorldLoc->x, kWorldLoc->y, kWorldLoc->z);
		D3DXMatrixMultiply(&rec.curVP, &camRelView, &unjitProj);
		D3DXMatrixMultiply(&rec.curJitVP, &camRelView, &jitProj);
		rec.nearZ = kFrustum->Near;
		rec.farZ = kFrustum->Far;
		if (PassCams.size() < 16) {
			PassCams.push_back(rec);
			CurrentCamIndex = (int)PassCams.size() - 1;
		} else {
			PassCams.back() = rec;
			CurrentCamIndex = (int)PassCams.size() - 1;
		}
	}

	inline void InstallCameraHook() {
		if (HooksInstalled) return;
		const UInt32 orig = *(const UInt32*)0x010EE648;
		if (!orig) {
			Logger::Log("[ERROR] UNOFFICIAL ViewModelMotion: NiDX9Renderer::Do_SetCameraData vtable slot (0x010EE648) is null");
			return;
		}
		OriginalDoSetCameraData = (DoSetCameraDataFn)orig;
		SafeWrite32(0x010EE648, (UInt32)&DoSetCameraDataHook);
		HooksInstalled = true;
		Logger::Log("UNOFFICIAL ViewModelMotion: hooked NiDX9Renderer::Do_SetCameraData (%08X) for 1st-person jitter & motion vectors", orig);
	}

	inline bool Reidentify(Entry& e, NiGeometry* geo) {
		const void* data = geo->geomData;
		const void* skin = geo->skinInstance;
		const void* parent = geo->m_parent;
		const bool other = e.seen != 0 && (e.data != data || e.skin != skin || e.parent != parent);
		e.data = data;
		e.skin = skin;
		e.parent = parent;
		return other;
	}

	inline bool IsUnderFirstPerson(NiGeometry* geo) {
		if (!Player || !Player->firstPersonNiNode || !geo) return false;
		const NiAVObject* root = Player->firstPersonNiNode;
		const NiAVObject* weaponRoot = Player->firstPersonSkinInfo ? Player->firstPersonSkinInfo->WeaponNode : nullptr;
		const NiAVObject* node = geo;
		for (int depth = 0; node && depth < 64; ++depth, node = node->m_parent) {
			if (node == root || (weaponRoot && node == weaponRoot)) return true;
		}
		return false;
	}

	inline void Release() {
		if (ScratchDepth) { ScratchDepth->Release(); ScratchDepth = nullptr; }
		if (Surface) { Surface->Release(); Surface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		MadeW = MadeH = 0;
	}

	inline void BeginFrame() {
		Data.x = 0.0f;
		Data.y = 0.0f;
		Data.z = 0.0f;
		InFirstPerson = false;
		CaptureWanted = false;
		TrackProj = false;
		InvProjSwapped = false;
		if (SceneTarget) { SceneTarget->Release(); SceneTarget = nullptr; }
		if (ProjTarget) { ProjTarget->Release(); ProjTarget = nullptr; }
		if (Failed || !TheFrameRateManager) return;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame != NoteFrame) {
			NoteFrame = frame;
			PassDraws.clear();
			PassCams.clear();
			CurrentCamIndex = -1;
		}
		if (frame - LastPrune > 60) {
			LastPrune = frame;
			for (auto it = Map.begin(); it != Map.end();) {
				if (frame - it->second.seen > 60) it = Map.erase(it);
				else ++it;
			}
		}
	}

	inline void BeginPass(IDirect3DDevice9* d, bool nativeLayer, bool wantCapture, bool wantJitter, float jx, float jy, UINT vpW, UINT vpH) {
		if (TheFrameRateManager) NoteFrame = TheFrameRateManager->FrameNumber;
		InFirstPerson = false;
		CaptureWanted = false;
		JitterWanted = false;
		JitterApplied = false;
		JitterX = 0.0f;
		JitterY = 0.0f;
		ViewportW = 0;
		ViewportH = 0;
		PassDraws.clear();
		PassCams.clear();
		CurrentCamIndex = -1;
		TrackProj = false;
		InvProjSwapped = false;
		if (SceneTarget) { SceneTarget->Release(); SceneTarget = nullptr; }
		if (ProjTarget) { ProjTarget->Release(); ProjTarget = nullptr; }

		if (HooksInstalled && d && TheRenderManager && Player && Player->IsDrawnFirstPerson() &&
			!InterfaceManager->IsPipBoyOpen() && !TheShaderManager->GameState.PipBoyIsOn && !TheShaderManager->GameState.OverlayIsOn) {
			SavedWorldInvProj = TheRenderManager->InvProjMatrix;
			if (SUCCEEDED(d->GetRenderTarget(0, &ProjTarget)) && ProjTarget) TrackProj = true;
		}

		if (Failed || !HooksInstalled || !d || !wantCapture || nativeLayer) return;
		if (!Player || !Player->IsDrawnFirstPerson() || !Player->firstPersonNiNode) return;
		if (InterfaceManager->IsPipBoyOpen() || TheShaderManager->GameState.PipBoyIsOn || TheShaderManager->GameState.OverlayIsOn) return;

		IDirect3DSurface9* rt = nullptr;
		IDirect3DSurface9* ds = nullptr;
		if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return;
		if (FAILED(d->GetDepthStencilSurface(&ds)) || !ds) { rt->Release(); return; }
		D3DSURFACE_DESC rtDesc = {}, dsDesc = {};
		const bool descOk = SUCCEEDED(rt->GetDesc(&rtDesc)) && SUCCEEDED(ds->GetDesc(&dsDesc));
		ds->Release();
		if (!descOk || rtDesc.MultiSampleType != D3DMULTISAMPLE_NONE || dsDesc.MultiSampleType != D3DMULTISAMPLE_NONE) {
			rt->Release();
			if (!MsaaLogged && descOk) {
				MsaaLogged = true;
				Logger::Log("UNOFFICIAL ViewModelMotion: MSAA active (sample type %u/%u): 1st-person jitter & motion vectors skipped",
					(unsigned)rtDesc.MultiSampleType, (unsigned)dsDesc.MultiSampleType);
			}
			return;
		}

		SceneTarget = rt;	// Keep 1 ref until EndCapture()
		CaptureWanted = true;
		JitterWanted = wantJitter;
		JitterX = jx;
		JitterY = jy;
		ViewportW = vpW ? vpW : rtDesc.Width;
		ViewportH = vpH ? vpH : rtDesc.Height;
		InFirstPerson = true;
	}

	inline void NoteDraw(IDirect3DDevice9* d, NiGeometry* geo, bool skinned) {
		if (!InFirstPerson || !CaptureWanted || !d || !geo || !SceneTarget) return;
		if (CurrentCamIndex < 0) { StatDropCam++; return; }
		if (!IsUnderFirstPerson(geo)) { StatDropRoot++; return; }
		IDirect3DSurface9* curRT = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(0, &curRT)) && curRT) curRT->Release();
		if (curRT != SceneTarget) { StatDropRT++; return; }

		DWORD zenable = D3DZB_FALSE, zwrite = FALSE, alphaBlend = TRUE;
		if (FAILED(d->GetRenderState(D3DRS_ZENABLE, &zenable)) || zenable == D3DZB_FALSE) { StatDropZ++; return; }
		d->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		d->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
		if (!zwrite) {
			if (alphaBlend) { StatDropZ++; return; }
			if (NiAlphaProperty* ap = (NiAlphaProperty*)geo->GetProperty(NiProperty::PropertyType::kType_Alpha)) {
				if (ap->flags & NiAlphaProperty::ALPHA_BLEND_MASK) { StatDropZ++; return; }
			}
		}

		const UInt32 frame = NoteFrame;
		Entry& e = Map[geo];
		if (e.seen == frame) return;

		const CamRecord& cam = PassCams[CurrentCamIndex];
		NiSkinInstance* skin = skinned ? geo->skinInstance : nullptr;
		if (skinned && (!skin || !skin->BoneMatrixes || !skin->Bones || skin->Bones > 512 || !skin->SkinPartition)) return;

		const bool reused = Reidentify(e, geo);
		const bool cut = TheShaderManager->GameState.isCameraCut;
		e.hasPrev = !cut && !reused && e.seen == frame - 1 && e.skinned == skinned &&
			(!skinned || e.bones.size() == skin->Bones * 12u);

		e.prev = e.cur;
		e.eyePrev = e.eyeCur;
		e.prevVP = e.curVP;
		if (skinned) e.prevBones.swap(e.bones);
		else e.prevBones.clear();

		e.cur = geo->m_worldTransform;
		e.eyeCur = cam.eye;
		e.curVP = cam.curVP;
		e.curJitVP = cam.curJitVP;
		e.nearZ = cam.nearZ;
		e.farZ = cam.farZ;
		e.skinned = skinned;
		e.seen = frame;
		if (skinned) e.bones.assign((const float*)skin->BoneMatrixes, (const float*)skin->BoneMatrixes + skin->Bones * 12u);
		else e.bones.clear();

		DWORD cull = D3DCULL_CCW;
		d->GetRenderState(D3DRS_CULLMODE, &cull);
		PassDraws.push_back({ geo, cull });
		if (skinned) StatNotedSkinned++;
		else StatNotedRigid++;
	}

	inline void EndCapture() {
		InFirstPerson = false;
		TrackProj = false;
		if (InvProjSwapped && TheRenderManager && TheRenderManager->device) {
			TheRenderManager->InvProjMatrix = SavedWorldInvProj;
			TheRenderManager->device->SetVertexShaderConstantF(100, (const float*)&SavedWorldInvProj, 4);
			TheRenderManager->device->SetVertexShaderConstantF(240, (const float*)&SavedWorldInvProj, 4);
			InvProjSwapped = false;
		}
		if (SceneTarget) { SceneTarget->Release(); SceneTarget = nullptr; }
		if (ProjTarget) { ProjTarget->Release(); ProjTarget = nullptr; }
	}

	inline bool Compile(IDirect3DDevice9* d) {
		if (VS && PS) return true;
		// Vertex constants: c0-c3 W (relative to eyeCur), c4-c7 CurJitVP, c8 D (x=skinned, y=hasPrev),
		// c9-c80 B[72] (24 bones), c81-c152 PB[72] (24 bones), c153-c156 PW (relative to eyePrev),
		// c157-c160 CurVP (unjittered), c161-c164 PrevVP (unjittered).
		static const char* kVS =
			"row_major float4x4 W : register(c0); row_major float4x4 CurJitVP : register(c4); float4 D : register(c8);"
			"float4 B[72] : register(c9); float4 PB[72] : register(c81); row_major float4x4 PW : register(c153);"
			"row_major float4x4 CurVP : register(c157); row_major float4x4 PrevVP : register(c161);"
			"struct I { float4 p : POSITION; float4 bw : BLENDWEIGHT; float4 bi : BLENDINDICES; };"
			"struct O { float4 pos : POSITION; float4 curClip : TEXCOORD0; float4 prevClip : TEXCOORD1; float2 flags : TEXCOORD2; };\n"
			"#define SKIN(M) { float4 q = float4(IN.p.xyz, 1.0f); float4 o = IN.bi.zyxw * 765.01f;"
			"  float3 a = mul(float3x4(M[0 + o.w], M[1 + o.w], M[2 + o.w]), q); float3 b = mul(float3x4(M[0 + o.z], M[1 + o.z], M[2 + o.z]), q);"
			"  float3 c = mul(float3x4(M[0 + o.x], M[1 + o.x], M[2 + o.x]), q); float3 e = mul(float3x4(M[0 + o.y], M[1 + o.y], M[2 + o.y]), q);"
			"  r = (1.0f - dot(IN.bw.xyz, 1.0f)) * a + IN.bw.z * b + IN.bw.x * c + IN.bw.y * e; }\n"
			"O main(I IN) { O OUT; float3 now, before;"
			"  [branch] if (D.x > 0.5f) { float3 r; SKIN(B) now = r; SKIN(PB) before = r; }"
			"  else { now = mul(float4(IN.p.xyz, 1.0f), W).xyz; before = mul(float4(IN.p.xyz, 1.0f), PW).xyz; }"
			"  OUT.pos = mul(float4(now, 1.0f), CurJitVP);"
			"  OUT.curClip = mul(float4(now, 1.0f), CurVP);"
			"  OUT.prevClip = mul(float4(before, 1.0f), PrevVP);"
			"  OUT.flags = float2(D.y, 0.0f);"
			"  return OUT; }";
		// Pixel constants: c0 = { nearZ, farZ, reversed (0/1), 0 }, c1 = { 1 / width, 1 / height, 0, 0 }.
		static const char* kPS =
			"sampler2D VMZ : register(s0); float4 P : register(c0); float4 R : register(c1);"
			"float4 main(float4 curClip : TEXCOORD0, float4 prevClip : TEXCOORD1, float2 flags : TEXCOORD2, float2 vpos : VPOS) : COLOR0 {"
			"  float raw = tex2Dlod(VMZ, float4((vpos + 0.5f) * R.xy, 0.0f, 0.0f)).x;"
			"  clip(P.z > 0.5f ? (raw > 0.0f ? 1.0f : -1.0f) : (raw < 1.0f ? 1.0f : -1.0f));"
			"  float vmZ = P.z > 0.5f ? (P.x * P.y / max(P.x + raw * (P.y - P.x), 1e-6f)) : (P.x * P.y / max(P.y - raw * (P.y - P.x), 1e-6f));"
			"  clip(abs(curClip.w - vmZ) <= max(vmZ * 0.08f, 0.75f) ? 1.0f : -1.0f);"
			"  [branch] if (flags.x < 0.5f || curClip.w <= 1e-3f || prevClip.w <= 1e-3f) return float4(0.0f, 0.0f, 0.0f, 0.0f);"
			"  float2 curNDC = curClip.xy / curClip.w; float2 prevNDC = prevClip.xy / prevClip.w;"
			"  float2 curUV = float2(curNDC.x * 0.5f + 0.5f, 0.5f - curNDC.y * 0.5f);"
			"  float2 prevUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);"
			"  float2 motion = prevUV - curUV;"
			"  [branch] if (dot(motion, motion) > 0.25f) return float4(0.0f, 0.0f, 0.0f, 0.0f);"
			"  return float4(motion.x, motion.y, 1.0f, 1.0f); }";
		auto build = [&](const char* src, const char* profile, ID3DXBuffer** out) -> bool {
			ID3DXBuffer* errors = nullptr;
			const bool ok = SUCCEEDED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", profile, 0, out, &errors, NULL)) && *out;
			if (!ok) Logger::Log("[ERROR] ViewModelMotion: the %s shader failed (%s): off for the session", profile, errors ? (const char*)errors->GetBufferPointer() : "?");
			if (errors) errors->Release();
			return ok;
		};
		ID3DXBuffer* vsCode = nullptr;
		ID3DXBuffer* psCode = nullptr;
		const bool ok = build(kVS, "vs_3_0", &vsCode) && build(kPS, "ps_3_0", &psCode) &&
			SUCCEEDED(d->CreateVertexShader((const DWORD*)vsCode->GetBufferPointer(), &VS)) &&
			SUCCEEDED(d->CreatePixelShader((const DWORD*)psCode->GetBufferPointer(), &PS));
		if (vsCode) vsCode->Release();
		if (psCode) psCode->Release();
		if (!ok) {
			if (VS) { VS->Release(); VS = nullptr; }
			if (PS) { PS->Release(); PS = nullptr; }
			Failed = true;
		}
		return ok;
	}

	inline void UploadBones(IDirect3DDevice9* d, UINT base, const float* bones, size_t boneCount, const unsigned short* map, unsigned count) {
		if (count > 24) count = 24;
		for (unsigned i = 0; i < count; i++) {
			const unsigned b = map ? map[i] : i;
			if (b >= boneCount) continue;
			d->SetVertexShaderConstantF(base + i * 3, bones + b * 12, 3);
		}
	}

	inline void MatrixOf(D3DXMATRIX& m, const NiTransform& t, const D3DXVECTOR3& camera) {
		const NiMatrix33& r = t.rot;
		const float s = t.scale;
		m._11 = r.data[0][0] * s; m._12 = r.data[1][0] * s; m._13 = r.data[2][0] * s; m._14 = 0.0f;
		m._21 = r.data[0][1] * s; m._22 = r.data[1][1] * s; m._23 = r.data[2][1] * s; m._24 = 0.0f;
		m._31 = r.data[0][2] * s; m._32 = r.data[1][2] * s; m._33 = r.data[2][2] * s; m._34 = 0.0f;
		m._41 = t.pos.x - camera.x; m._42 = t.pos.y - camera.y; m._43 = t.pos.z - camera.z; m._44 = 1.0f;
	}

	__declspec(noinline) inline void DrawOne(IDirect3DDevice9* d, const DrawRecord& rec, UINT texW, UINT texH, bool reversed, int& kind) {
		kind = 0;
		NiGeometry* geo = rec.geo;
		if (!geo || !geo->geomData) return;
		auto found = Map.find(geo);
		if (found == Map.end() || found->second.seen != NoteFrame) return;
		const Entry& e = found->second;

		d->SetRenderState(D3DRS_CULLMODE, rec.cull);
		const float pc[2][4] = {
			{ e.nearZ, e.farZ, reversed ? 1.0f : 0.0f, 0.0f },
			{ 1.0f / (float)texW, 1.0f / (float)texH, 0.0f, 0.0f },
		};
		d->SetPixelShaderConstantF(0, &pc[0][0], 2);
		d->SetVertexShaderConstantF(4, (const float*)&e.curJitVP, 4);
		d->SetVertexShaderConstantF(157, (const float*)&e.curVP, 4);
		d->SetVertexShaderConstantF(161, (const float*)(e.hasPrev ? &e.prevVP : &e.curVP), 4);
		const float flag[4] = { e.skinned ? 1.0f : 0.0f, e.hasPrev ? 1.0f : 0.0f, 0.0f, 0.0f };
		d->SetVertexShaderConstantF(8, flag, 1);

		if (e.skinned) {
			NiSkinInstance* skin = geo->skinInstance;
			if (!skin || !skin->SkinPartition || !skin->SkinPartition->Partitions || e.bones.empty()) return;
			const float* prevBones = (e.hasPrev && e.prevBones.size() == e.bones.size()) ? e.prevBones.data() : e.bones.data();
			DismemberPartition* dismember = skin->IsKindOf<BSDismemberSkinInstance>() ? ((BSDismemberSkinInstance*)skin)->partitions : nullptr;
			const size_t boneCount = e.bones.size() / 12;
			for (UInt32 p = 0; p < skin->SkinPartition->PartitionsCount; p++) {
				if (dismember && !dismember[p].Enabled) continue;
				NiSkinPartition::Partition* part = &skin->SkinPartition->Partitions[p];
				if (!part->BuffData) continue;
				UploadBones(d, 9, e.bones.data(), boneCount, part->pBones, part->Bones);
				UploadBones(d, 81, prevBones, boneCount, part->pBones, part->Bones);
				Drawer->DrawSkinnedGeometryBuffer(geo, part->BuffData, part);
			}
			kind = 2;
			return;
		}

		NiGeometryBufferData* buffers = geo->geomData->m_pkBuffData;
		if (!buffers || !buffers->VertCount || !buffers->StreamCount || !buffers->VBChip || !buffers->VBChip[0] || !buffers->VBChip[0]->VB) {
			StatDropBuf++;
			return;
		}
		D3DXMATRIX now, before;
		MatrixOf(now, e.cur, e.eyeCur);
		MatrixOf(before, e.hasPrev ? e.prev : e.cur, e.hasPrev ? e.eyePrev : e.eyeCur);
		d->SetVertexShaderConstantF(0, (const float*)&now, 4);
		d->SetVertexShaderConstantF(153, (const float*)&before, 4);
		Drawer->DrawGeometryBuffer(geo, buffers);
		kind = 1;
	}

	inline bool DrawGuarded(IDirect3DDevice9* d, const DrawRecord& rec, UINT texW, UINT texH, bool reversed, int& kind) {
		__try { DrawOne(d, rec, texW, texH, reversed, kind); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// RenderFirstPersonHook, immediately after ResolveViewModelDepth().
	inline void Render(IDirect3DDevice9* d, IDirect3DTexture9* vmDepth) {
		Data.x = 0.0f;
		Data.y = 0.0f;
		Data.z = 0.0f;
		if (!CaptureWanted || Failed || !d || !vmDepth || !TheFrameRateManager) return;
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (JitterApplied) {
			JitteredFrame = frame;
			Data.z = 1.0f;
		}
		D3DSURFACE_DESC dd = {};
		if (FAILED(vmDepth->GetLevelDesc(0, &dd)) || !dd.Width || !dd.Height) return;

		if (!Texture || MadeW != dd.Width || MadeH != dd.Height) {
			Release();
			if (FAILED(d->CreateTexture(dd.Width, dd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Texture, NULL)) ||
				FAILED(Texture->GetSurfaceLevel(0, &Surface)) ||
				FAILED(d->CreateDepthStencilSurface(dd.Width, dd.Height, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &ScratchDepth, NULL))) {
				Logger::Log("[ERROR] ViewModelMotion: could not create %ux%u targets: off for the session", dd.Width, dd.Height);
				Release();
				Failed = true;
				return;
			}
			MadeW = dd.Width;
			MadeH = dd.Height;
			Logger::Log("UNOFFICIAL ViewModelMotion: %ux%u motion target ready", dd.Width, dd.Height);
		}

		d->ColorFill(Surface, NULL, 0);
		if (PassDraws.empty()) {
			ReadyFrame = frame;
			Data.x = 1.0f;
			return;
		}
		if (!Compile(d)) return;
		if (!Drawer) Drawer = new DrawPass();

		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr;
		IDirect3DPixelShader9* ps = nullptr;
		IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT vbOffset = 0, vbStride = 0;
		IDirect3DIndexBuffer9* ib = nullptr;
		IDirect3DBaseTexture9* tex0 = nullptr;
		IDirect3DSurface9* rt[4] = {};
		IDirect3DSurface9* ds = nullptr;
		D3DVIEWPORT9 vp = {};
		DWORD fvf = 0;
		d->GetVertexShader(&vs);
		d->GetPixelShader(&ps);
		d->GetVertexDeclaration(&decl);
		d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
		d->GetIndices(&ib);
		d->GetTexture(0, &tex0);
		for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
		d->GetDepthStencilSurface(&ds);
		d->GetViewport(&vp);

		const bool reversed = TheRenderManager->IsReversedDepth();
		static const D3DRENDERSTATETYPE kRS[] = {
			D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
			D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE,
			D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS
		};
		const DWORD want[] = {
			D3DZB_TRUE, TRUE, reversed ? (DWORD)D3DCMP_GREATEREQUAL : (DWORD)D3DCMP_LESSEQUAL, FALSE, FALSE,
			FALSE, FALSE, D3DCULL_CCW, 0xFu, D3DFILL_SOLID,
			FALSE, FALSE, 0u, 0u, 0u
		};
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) {
			if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i];
			if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]);
		}
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) {
			if (FAILED(d->GetSamplerState(0, kSS[i], &savedS[i]))) savedS[i] = wantS[i];
			if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], wantS[i]);
		}
		static float savedPS[2][4] = {};
		static float savedVS[165][4] = {};
		d->GetPixelShaderConstantF(0, &savedPS[0][0], 2);
		d->GetVertexShaderConstantF(0, &savedVS[0][0], 165);

		d->SetRenderTarget(0, Surface);
		for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
		d->SetDepthStencilSurface(ScratchDepth);
		D3DVIEWPORT9 drawVP = { 0, 0, ViewportW, ViewportH, 0.0f, 1.0f };
		if (!drawVP.Width || drawVP.Width > dd.Width) drawVP.Width = dd.Width;
		if (!drawVP.Height || drawVP.Height > dd.Height) drawVP.Height = dd.Height;
		d->SetViewport(&drawVP);
		d->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, reversed ? 0.0f : 1.0f, 0);

		state->SetVertexShader(VS, false);
		state->SetPixelShader(PS, false);
		d->SetTexture(0, vmDepth);

		unsigned drawnGeos = 0, drawnSkinned = 0;
		for (const DrawRecord& rec : PassDraws) {
			int kind = 0;
			if (!DrawGuarded(d, rec, dd.Width, dd.Height, reversed, kind)) {
				Logger::Log("[ERROR] ViewModelMotion: fault drawing 1st-person geometry: off for the session");
				Failed = true;
				break;
			}
			if (kind) drawnGeos++;
			if (kind == 2) drawnSkinned++;
		}

		// Restore device state completely.
		d->SetVertexShaderConstantF(0, &savedVS[0][0], 165);
		d->SetPixelShaderConstantF(0, &savedPS[0][0], 2);
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) d->SetSamplerState(0, kSS[i], savedS[i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) d->SetRenderState(kRS[i], savedRS[i]);
		if (rt[0]) { d->SetRenderTarget(0, rt[0]); rt[0]->Release(); }
		for (DWORD i = 1; i < 4; i++) if (rt[i]) { d->SetRenderTarget(i, rt[i]); rt[i]->Release(); }
		if (ds) { d->SetDepthStencilSurface(ds); ds->Release(); }
		d->SetViewport(&vp);
		d->SetTexture(0, tex0);
		if (tex0) tex0->Release();
		if (decl) { state->SetVertexDeclaration(decl, false); decl->Release(); }
		else state->SetFVF(fvf, false);
		d->SetStreamSource(0, vb, vbOffset, vbStride);
		if (vb) vb->Release();
		d->SetIndices(ib);
		if (ib) ib->Release();
		state->SetVertexShader(vs, false);
		if (vs) vs->Release();
		state->SetPixelShader(ps, false);
		if (ps) ps->Release();

		DrawnGeometries += drawnGeos;
		DrawnSkinned += drawnSkinned;
		if (!Failed) {
			Data.x = 1.0f;
			Data.y = (float)drawnGeos;
			ReadyFrame = frame;
			if (!ActiveLogged && drawnGeos > 0) {
				ActiveLogged = true;
				Logger::Log("UNOFFICIAL ViewModelMotion: active (%u drawn: %u rigid, %u skinned, jitter %s, viewport %ux%u | drops cam=%u root=%u rt=%u z=%u buf=%u)",
					drawnGeos, drawnGeos - drawnSkinned, drawnSkinned, JitterApplied ? "on" : "off",
					(unsigned)drawVP.Width, (unsigned)drawVP.Height,
					StatDropCam, StatDropRoot, StatDropRT, StatDropZ, StatDropBuf);
			}
		} else {
			Data.x = 0.0f;
			Data.y = 0.0f;
			Data.z = 0.0f;
		}
		if (!GpuTimer::Enabled) {
			Frames = DrawnGeometries = DrawnSkinned = 0;
			StatNotedRigid = StatNotedSkinned = StatDropCam = StatDropRoot = StatDropRT = StatDropZ = StatDropBuf = 0;
		} else if (++Frames >= 600) {
			Logger::Log("ViewModelMotion: avg %.1f geometries drawn (%.1f rigid, %.1f skinned) over %u frames (drops: cam %u, root %u, rt %u, z %u, buf %u)",
				DrawnGeometries / (double)Frames, (DrawnGeometries - DrawnSkinned) / (double)Frames, DrawnSkinned / (double)Frames, Frames,
				StatDropCam, StatDropRoot, StatDropRT, StatDropZ, StatDropBuf);
			Frames = DrawnGeometries = DrawnSkinned = 0;
			StatNotedRigid = StatNotedSkinned = StatDropCam = StatDropRoot = StatDropRT = StatDropZ = StatDropBuf = 0;
		}
	}
}
