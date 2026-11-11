#pragma once

// UNOFFICIAL 2026-10-29 ObjectMotion (docs/next/ROOT-ASSUMPTIONS-PLAN.md, B1 + D; [Main.Main.Quality] ObjectMotion, off by default).
// Every temporal effect (TAA, FSR 2 / 3.1's motion vectors) found last frame's pixel from the depth buffer and the CAMERA's move only:
// a walking NPC, a grabbed teapot, a swinging door was reprojected as if it stood still, so its history smeared (ghosting, trails).
// Here the things that really moved get their own motion, in ONE buffer every reader shares:
//   - while the world's main list draws (the RenderPassStandard / RenderPassSkinned hooks, world context only), each geometry is noted
//     once a frame: its world transform, and for a skinned one the bone matrices the game just computed for that draw (camera relative,
//     as the game's skinned shaders read them). Something whose world position (or a bone's) differs from last frame's has moved;
//   - after the effects' depth combine (ShaderManager::RenderEffectsPreTonemapping), only those geometries are drawn again, with a small
//     vertex shader that places each vertex twice (this frame's transform / bones, last frame's), into an fp16 target (TESR_ObjectMotionBuffer):
//     rgb = how far that surface point moved in the world since last frame (game units, world axes), a = 1. A fragment is kept only where
//     its view depth matches the depth buffer's (the surface the screen shows there); every other pixel stays 0 = "moved with the world";
//   - readers subtract it from the camera-relative point before reprojecting (TAA.fx.hlsl / TAA.glsl Reproject, FSRPrep.comp), the only
//     change: with nothing moving, or with the setting off (TESR_ObjectMotionData.x = 0), the reprojection is exactly as before.
// The world-space delta needs no projection, jitter or RenderScale handling: each reader projects it with its own camera as it always did.
// The camera-jump flag every history already shares is GameState.isCameraCut (ShaderManager.cpp; D's other half, in place since 10-21).
// Not covered: the first-person view (TAA keeps it at zero motion), alpha-blended things and particles (not in the opaque list), grass
// sway and cloth / plant wind (drawn here without their wind: their delta is the object's, usually 0).
#include <d3d9.h>
#include <d3dx9.h>
#include <unordered_map>
#include <vector>
#include "RenderPass.h"

namespace ObjectMotion {

	inline bool Checked = false;
	inline bool Live = false;			// this frame: wanted and not failed (notes are taken)
	inline bool Failed = false;
	inline D3DXVECTOR4 Data = { 0.0f, 0.0f, 0.0f, 0.0f };	// TESR_ObjectMotionData: x 1 = the buffer holds this frame's, y moving geometries, z fragments' depth tolerance share
	inline IDirect3DTexture9* Texture = nullptr;  inline IDirect3DSurface9* Surface = nullptr;	// TESR_ObjectMotionBuffer
	inline UINT MadeW = 0, MadeH = 0;
	inline IDirect3DVertexShader9* VS = nullptr;  inline IDirect3DPixelShader9* PS = nullptr;
	inline UInt32 Frames = 0, DrawnGeometries = 0, DrawnSkinned = 0, Noted = 0, CameraFollowers = 0;
	inline UInt32 MadeFrame = 0xFFFFFFFF;	// the frame TESR_ObjectMotionBuffer was made in
	inline IDirect3DSurface9* CheckTarget = nullptr;  inline IDirect3DSurface9* CheckCopy = nullptr;	// the 600-frame read-back (Render)
	// For the Vulkan readers (TAA.glsl, FSRPrep / FSRUpPrep): the buffer holds THIS frame's (a reader that runs before the effects, as the
	// upscale before them does, finds last frame's and keeps camera motion only).
	inline bool Ready() { return Data.x > 0.0f && Texture && MadeFrame == TheFrameRateManager->FrameNumber; }

	struct Entry {
		UInt32 seen = 0;				// frame number it was last noted in
		bool skinned = false, hasPrev = false;
		NiTransform cur = {}, prev = {};
		D3DXVECTOR3 camCur = {}, camPrev = {};		// the camera position the bones were made relative to
		std::vector<float> bones, prevBones;		// 12 floats a bone (3 rows of a 3x4), camera relative
		// UNOFFICIAL 2026-11-09 ObjectMotionIdentity (roadmap track 1 step 3): what the geometry at this address was made of when it was
		// last noted. The map is keyed by the raw NiGeometry*: a geometry freed and a new one made at the same address (a cell unloading,
		// a corpse swapped, a weapon model changed) would have taken the old one's transform or bones as its "last frame" and drawn a
		// false move. A note whose mesh data, skin or parent node differ from the entry's is a new thing: no last frame (as on its first).
		const void* data = nullptr; const void* skin = nullptr; const void* parent = nullptr;
	};
	inline std::unordered_map<NiGeometry*, Entry> Map;
	inline std::vector<NiGeometry*> Moving;
	inline UInt32 NoteFrame = 0;		// the frame the notes are for (Moving is cleared when it changes)
	inline UInt32 LastPrune = 0;
	inline bool CamPrevKnown = false;
	inline D3DXVECTOR3 CameraNow() { const D3DXVECTOR4& c = TheRenderManager->CameraPosition; return D3DXVECTOR3(c.x, c.y, c.z); }
	inline int BoneSpaceLogged = 0;	// one diagnostic line: are the game's bone matrices camera relative (as the shadow code reads them)?

	inline bool Wanted() { return TheSettingManager->GetSettingI("Main.Main.Quality", "ObjectMotion") != 0; }
	// UNOFFICIAL 2026-10-30 [Main.Main.Quality] ObjectMotionSkipCameraFollow: the sky dome (and anything else the game keeps centred on
	// the camera) moves by exactly the camera's move every frame the player walks, so it was drawn as 'moved'. Its depth matched the
	// ground's along a ring at the dome's radius round the player (the depth test's 2 % + 4 units), and that ring of ground got the
	// camera's move as its own: FSR / TAA reprojected it as if it walked along, a blurred circle round the player (RenderScale v2 only,
	// where the upscale runs after this buffer is made). On: a geometry whose move is the camera's move counts as still (2026-11-01: for a large
	// one, its position alone: the dome also turns, NoteRigid).
	inline bool SkipCameraFollow = false;	// read in BeginFrame
	inline bool IdentityCheck = true;		// UNOFFICIAL 2026-11-09 [Main.Main.Quality] ObjectMotionIdentity (missing key: on), read in BeginFrame
	inline UInt32 Reused = 0;				// notes whose address held another geometry (the identity check said new)
	// True when the entry was made for another geometry at the same address; takes this one's identity either way.
	inline bool Reidentify(Entry& e, NiGeometry* geo) {
		const void* data = geo->geomData; const void* skin = geo->skinInstance; const void* parent = geo->m_parent;
		const bool other = e.seen != 0 && (e.data != data || e.skin != skin || e.parent != parent);
		e.data = data; e.skin = skin; e.parent = parent;
		if (other && IdentityCheck) { Reused++; return true; }
		return false;
	}

	// Render.cpp, once a frame, before the world draws.
	inline void BeginFrame() {
		Live = !Failed && Wanted();
		SkipCameraFollow = TheSettingManager->GetSettingI("Main.Main.Quality", "ObjectMotionSkipCameraFollow") != 0;
		{ SettingManager::Configuration::ConfigNode node; IdentityCheck = !TheSettingManager->Config.FillNode(&node, "Main.Main.Quality", "ObjectMotionIdentity") || node.BoolValue; }
		if (!Live) {
			if (!Map.empty()) { Map.clear(); Moving.clear(); }
			Data.x = 0.0f;
			CamPrevKnown = false;
			return;
		}
		const UInt32 frame = TheFrameRateManager->FrameNumber;
		if (frame != NoteFrame) {
			NoteFrame = frame;
			Moving.clear();
		}
		// Forget what was not drawn for a second (gone, or out of view: its first frame back counts as still).
		if (frame - LastPrune > 60) {
			LastPrune = frame;
			for (auto it = Map.begin(); it != Map.end();) { if (frame - it->second.seen > 60) it = Map.erase(it); else ++it; }
		}
	}

	// UNOFFICIAL 2026-10-29 (bench, Ultra-Luxe crowd: 151 'moved' geometries a frame moving 0.01-0.02 units, 0.4 ms GPU for nothing): a
	// move counts only when some point of the thing can travel at least MinMove units (a twentieth of a unit: far under a pixel at any
	// usual distance; TAA and FSR treat what is under it as still anyway). Rotation: the change of a rotation entry times the reach
	// (the bound's radius for a rigid object; 64 units for a bone, a limb's length).
	constexpr float MinMove = 0.05f, BoneReach = 64.0f;
	inline bool TransformDiffers(const NiTransform& a, const NiTransform& b, float reach) {
		const float dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y, dz = a.pos.z - b.pos.z;
		if (dx * dx + dy * dy + dz * dz > MinMove * MinMove) return true;
		float rot = fabsf(a.scale - b.scale);
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rot = (std::max)(rot, fabsf(a.rot.data[i][j] - b.rot.data[i][j]));
		return rot * (std::max)(reach, 1.0f) > MinMove;
	}

	// After a world-context RenderPassStandard (rigid geometry).
	inline void NoteRigid(NiGeometry* geo) {
		if (!Live || !geo) return;
		const UInt32 frame = NoteFrame;
		Entry& e = Map[geo];
		if (e.seen == frame) return;
		const bool reused = Reidentify(e, geo);	// UNOFFICIAL 2026-11-09 ObjectMotionIdentity
		e.hasPrev = e.seen == frame - 1 && !e.skinned && !reused;
		e.skinned = false;
		e.prev = e.cur;
		e.camPrev = e.camCur;
		e.cur = geo->m_worldTransform;
		e.camCur = CameraNow();
		e.seen = frame;
		Noted++;
		const float reach = geo->m_kWorldBound ? geo->m_kWorldBound->Radius : 64.0f;
		if (e.hasPrev && TransformDiffers(e.prev, e.cur, reach)) {
			if (SkipCameraFollow) {
				NiTransform carried = e.prev;	// last frame's, carried along by the camera's move
				carried.pos.x += e.camCur.x - e.camPrev.x; carried.pos.y += e.camCur.y - e.camPrev.y; carried.pos.z += e.camCur.z - e.camPrev.z;
				if (!TransformDiffers(carried, e.cur, reach)) { CameraFollowers++; return; }
				// UNOFFICIAL 2026-11-01 (user: the ring round the player is back, FSR 0.33, every effect off but TAA; DebugView 3 showed a band of
				// zero motion on the ground at the dome's radius; the log: moved '' rigid, move = the camera's to the hundredth, radius 1061): the
				// sky dome also turns a little every frame (the sky's own rotation), and at a 1061-unit reach that alone failed the test above,
				// so it was drawn as moved again (a tester's log, DLSS native, standing still: move 0.02 with the camera at 0, the same). Something
				// large the camera stands inside (the bound's centre within half its radius) whose POSITION follows the camera counts as still,
				// whatever it turns: its depth meets the ground only along that ring, where any motion it hands out is wrong.
				// (2026-11-02, user: the line is still there; the log: the dome still listed as moved, move = the camera's: its bound is not centred on the
				// camera, so that test went. Now: large, and its position followed a real camera move (a door turning while the camera stands still
				// does not pass: the camera did not move); a still camera leaves the dome only its own slow turn, far under a pixel.)
				const NiBound* bound = geo->m_kWorldBound;
				const float mx = e.camCur.x - e.camPrev.x, my = e.camCur.y - e.camPrev.y, mz = e.camCur.z - e.camPrev.z;
				const float camMove2 = mx * mx + my * my + mz * mz;
				if (bound && bound->Radius > 512.0f && camMove2 > MinMove * MinMove) {
					const float px = carried.pos.x - e.cur.pos.x, py = carried.pos.y - e.cur.pos.y, pz = carried.pos.z - e.cur.pos.z;
					const float tol = 2.0f * MinMove + 0.02f * sqrtf(camMove2);
					if (px * px + py * py + pz * pz <= tol * tol) { CameraFollowers++; return; }
				}
			}
			Moving.push_back(geo);
		}
	}

	// Did any bone move in the WORLD (the matrices are camera relative: the rotations compare as they are, the translations with the camera added)?
	inline bool BonesMoved(const Entry& e) {
		if (e.bones.size() != e.prevBones.size()) return false;
		const D3DXVECTOR3 d = e.camCur - e.camPrev;
		for (size_t b = 0; b + 12 <= e.bones.size(); b += 12) {
			const float* n = &e.bones[b]; const float* p = &e.prevBones[b];
			for (int r = 0; r < 3; r++) {
				for (int k = 0; k < 3; k++) if (fabsf(n[r * 4 + k] - p[r * 4 + k]) * BoneReach > MinMove) return true;
				const float dt = (n[r * 4 + 3] + (&d.x)[r]) - p[r * 4 + 3];
				if (fabsf(dt) > MinMove) return true;
			}
		}
		return false;
	}

	// After a world-context RenderPassSkinned: the game has just made this skin's bone matrices for this frame's camera.
	inline void NoteSkinned(NiGeometry* geo) {
		if (!Live || !geo) return;
		NiSkinInstance* skin = geo->skinInstance;
		if (!skin || !skin->BoneMatrixes || !skin->Bones || skin->Bones > 512 || !skin->SkinPartition) return;
		const UInt32 frame = NoteFrame;
		Entry& e = Map[geo];
		if (e.seen == frame) return;
		const bool reused = Reidentify(e, geo);	// UNOFFICIAL 2026-11-09 ObjectMotionIdentity
		e.hasPrev = e.seen == frame - 1 && e.skinned && e.bones.size() == skin->Bones * 12u && !reused;
		e.skinned = true;
		e.prevBones.swap(e.bones);
		e.camPrev = e.camCur;
		e.bones.assign((const float*)skin->BoneMatrixes, (const float*)skin->BoneMatrixes + skin->Bones * 12u);
		e.camCur = CameraNow();
		if (BoneSpaceLogged < 3 && e.bones.size() >= 12) {
			BoneSpaceLogged++;
			const NiPoint3& at = geo->m_worldTransform.pos;
			const float rel[3] = { at.x - e.camCur.x, at.y - e.camCur.y, at.z - e.camCur.z };
			const float bx = e.bones[3], by = e.bones[7], bz = e.bones[11];
			Logger::Log("ObjectMotion: bone 0 at %.0f %.0f %.0f; the geometry at %.0f %.0f %.0f camera relative, %.0f %.0f %.0f in the world (the bones should be near the first)",
				bx, by, bz, rel[0], rel[1], rel[2], at.x, at.y, at.z);
		}
		e.prev = e.cur;
		e.cur = geo->m_worldTransform;
		e.seen = frame;
		Noted++;
		if (e.hasPrev && BonesMoved(e)) Moving.push_back(geo);
	}

	inline void Release() {
		if (Surface) { Surface->Release(); Surface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		MadeW = MadeH = 0;
	}

	inline bool Compile(IDirect3DDevice9* d) {
		if (VS && PS) return true;
		// Vertex constants: c0-c3 this frame's world (camera relative, rigid), c4-c7 the view-projection (camera relative, unjittered),
		// c8 x 1 = skinned, c9-c62 this frame's bones (18, the partitions' palette), c64-c117 last frame's bones, c118-c121 last frame's
		// world (relative to last frame's camera), c122 xyz this frame's camera minus last frame's, c123 xyz the camera's forward axis.
		static const char* kVS =
			"row_major float4x4 W : register(c0); row_major float4x4 VP : register(c4); float4 D : register(c8);"
			"float4 B[54] : register(c9); float4 PB[54] : register(c64); row_major float4x4 PW : register(c118);"
			"float4 CD : register(c122); float4 FW : register(c123);"
			"struct I { float4 p : POSITION; float4 bw : BLENDWEIGHT; float4 bi : BLENDINDICES; };"
			"struct O { float4 pos : POSITION; float4 delta : TEXCOORD0; };\n"	// (a #define must start its own line)
			"#define SKIN(M) { float4 q = float4(IN.p.xyz, 1.0f); float4 o = IN.bi.zyxw * 765.01f;"
			"  float3 a = mul(float3x4(M[0 + o.w], M[1 + o.w], M[2 + o.w]), q); float3 b = mul(float3x4(M[0 + o.z], M[1 + o.z], M[2 + o.z]), q);"
			"  float3 c = mul(float3x4(M[0 + o.x], M[1 + o.x], M[2 + o.x]), q); float3 e = mul(float3x4(M[0 + o.y], M[1 + o.y], M[2 + o.y]), q);"
			"  r = (1.0f - dot(IN.bw.xyz, 1.0f)) * a + IN.bw.z * b + IN.bw.x * c + IN.bw.y * e; }\n"
			"O main(I IN) { O OUT; float3 now, before;"
			"  [branch] if (D.x > 0.5f) { float3 r; SKIN(B) now = r; SKIN(PB) before = r; }"
			"  else { now = mul(float4(IN.p.xyz, 1.0f), W).xyz; before = mul(float4(IN.p.xyz, 1.0f), PW).xyz; }"
			"  OUT.pos = mul(float4(now, 1.0f), VP);"
			"  OUT.delta = float4(now - before + CD.xyz, dot(now, FW.xyz));"
			"  return OUT; }";
		// Pixel constants: c0 x far, y depth share, z units, w the most a point may move in one frame; c1 xy 1 / the depth texture's size.
		static const char* kPS =
			"sampler2D Z : register(s0); float4 P : register(c0); float4 R : register(c1);"
			"float4 main(float4 delta : TEXCOORD0, float2 vpos : VPOS) : COLOR0 {"
			"  float z = tex2Dlod(Z, float4((vpos + 0.5f) * R.xy, 0.0f, 0.0f)).x * P.x;"
			"  clip(abs(delta.w - z) <= z * P.y + P.z && dot(delta.xyz, delta.xyz) <= P.w * P.w ? 1.0f : -1.0f);"
			"  return float4(delta.xyz, 1.0f); }";
		auto build = [&](const char* src, const char* profile, ID3DXBuffer** out) -> bool {
			ID3DXBuffer* errors = nullptr;
			const bool ok = SUCCEEDED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", profile, 0, out, &errors, NULL)) && *out;
			if (!ok) Logger::Log("[ERROR] ObjectMotion: the %s shader failed (%s): off for the session", profile, errors ? (const char*)errors->GetBufferPointer() : "?");
			if (errors) errors->Release();
			return ok;
		};
		ID3DXBuffer* vsCode = nullptr; ID3DXBuffer* psCode = nullptr;
		bool ok = build(kVS, "vs_3_0", &vsCode) && build(kPS, "ps_3_0", &psCode) &&
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

	// The bone rows a partition draws with, from a palette of 12-float bones, to vertex register base (as UploadShadowBones does at 9).
	inline void UploadBones(IDirect3DDevice9* d, UINT base, const float* bones, size_t boneCount, const unsigned short* map, unsigned count) {
		if (count > 18) count = 18;	// the shader's palette (the game's partitions hold up to 18)
		for (unsigned i = 0; i < count; i++) {
			const unsigned b = map ? map[i] : i;
			if (b >= boneCount) continue;
			d->SetVertexShaderConstantF(base + i * 3, bones + b * 12, 3);
		}
	}

	// A world transform as a camera-relative row-vector matrix (RenderManager::CreateD3DMatrix with a given camera).
	inline void MatrixOf(D3DXMATRIX& m, const NiTransform& t, const D3DXVECTOR3& camera) {
		const NiMatrix33& r = t.rot; const float s = t.scale;
		m._11 = r.data[0][0] * s; m._12 = r.data[1][0] * s; m._13 = r.data[2][0] * s; m._14 = 0.0f;
		m._21 = r.data[0][1] * s; m._22 = r.data[1][1] * s; m._23 = r.data[2][1] * s; m._24 = 0.0f;
		m._31 = r.data[0][2] * s; m._32 = r.data[1][2] * s; m._33 = r.data[2][2] * s; m._34 = 0.0f;
		m._41 = t.pos.x - camera.x; m._42 = t.pos.y - camera.y; m._43 = t.pos.z - camera.z; m._44 = 1.0f;
	}

	class DrawPass : public RenderPass {};	// only for RenderPass's draw helpers (the game's own draw calls, no shaders of its own)
	inline DrawPass* Drawer = nullptr;

	inline const D3DXVECTOR4* Constant(const char* name) {
		auto found = TheShaderManager->ConstantsTable.find(name);
		return found == TheShaderManager->ConstantsTable.end() ? nullptr : found->second;
	}

	// One moved geometry into the motion target (shaders, view-projection and states already set). kind: 0 nothing drawn, 1 rigid, 2 skinned.
	__declspec(noinline) inline void DrawOne(IDirect3DDevice9* d, NiGeometry* geo, int& kind) {
		kind = 0;
		auto found = Map.find(geo);
		if (found == Map.end() || !found->second.hasPrev || !geo->geomData) return;
		const Entry& e = found->second;
		if (e.skinned) {
			NiSkinInstance* skin = geo->skinInstance;
			if (!skin || !skin->SkinPartition || !skin->SkinPartition->Partitions) return;
			const float flag[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			d->SetVertexShaderConstantF(8, flag, 1);
			// Last frame's bones are relative to last frame's camera: moved to this frame's, so both palettes sit in one space and the
			// difference is the move in the world (no camera term).
			std::vector<float> before = e.prevBones;
			const D3DXVECTOR3 shift = e.camPrev - e.camCur;
			for (size_t b = 0; b + 12 <= before.size(); b += 12) { before[b + 3] += shift.x; before[b + 7] += shift.y; before[b + 11] += shift.z; }
			const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			d->SetVertexShaderConstantF(122, none, 1);
			DismemberPartition* dismember = skin->IsKindOf<BSDismemberSkinInstance>() ? ((BSDismemberSkinInstance*)skin)->partitions : nullptr;
			const size_t boneCount = e.bones.size() / 12;
			for (UInt32 p = 0; p < skin->SkinPartition->PartitionsCount; p++) {
				if (dismember && !dismember[p].Enabled) continue;
				NiSkinPartition::Partition* part = &skin->SkinPartition->Partitions[p];
				if (!part->BuffData) continue;
				UploadBones(d, 9, e.bones.data(), boneCount, part->pBones, part->Bones);
				UploadBones(d, 64, before.data(), boneCount, part->pBones, part->Bones);
				Drawer->DrawSkinnedGeometryBuffer(geo, part->BuffData, part);
			}
			kind = 2;
			return;
		}
		NiGeometryBufferData* buffers = geo->geomData->m_pkBuffData;
		if (!buffers || !buffers->VertCount) return;
		const float flag[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		d->SetVertexShaderConstantF(8, flag, 1);
		D3DXMATRIX now, before;
		MatrixOf(now, e.cur, e.camCur);
		MatrixOf(before, e.prev, e.camPrev);
		d->SetVertexShaderConstantF(0, (const float*)&now, 4);
		d->SetVertexShaderConstantF(118, (const float*)&before, 4);
		const D3DXVECTOR3 camDelta = e.camCur - e.camPrev;
		const float cd[4] = { camDelta.x, camDelta.y, camDelta.z, 0.0f };
		d->SetVertexShaderConstantF(122, cd, 1);
		Drawer->DrawGeometryBuffer(geo, buffers);
		kind = 1;
	}

	// (A geometry freed between its note and this pass would fault: caught, and the feature goes off rather than the game down.)
	inline bool DrawGuarded(IDirect3DDevice9* d, NiGeometry* geo, int& kind) {
		__try { DrawOne(d, geo, kind); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// ShaderManager::RenderEffectsPreTonemapping, right after RenderDepthAndNormals.
	inline void Render(IDirect3DDevice9* d, IDirect3DTexture9* depth) {
		Data.x = 0.0f;
		Data.y = 0.0f;
		if (!Live || !depth) return;
		if (TheShaderManager->GameState.isCameraCut || !CamPrevKnown) { CamPrevKnown = true; return; }	// no last frame to come from
		D3DSURFACE_DESC dd = {};
		depth->GetLevelDesc(0, &dd);
		if (!Texture || MadeW != dd.Width || MadeH != dd.Height) {
			Release();
			if (FAILED(d->CreateTexture(dd.Width, dd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &Texture, NULL)) ||
				FAILED(Texture->GetSurfaceLevel(0, &Surface))) {
				Logger::Log("[ERROR] ObjectMotion: could not make the %ux%u motion target: off for the session", dd.Width, dd.Height);
				Release();
				Failed = true;
				return;
			}
			MadeW = dd.Width; MadeH = dd.Height;
			Logger::Log("ObjectMotion: motion target %ux%u", dd.Width, dd.Height);
		}
		d->ColorFill(Surface, NULL, D3DCOLOR_ARGB(0, 0, 0, 0));	// "moved with the world"
		Data.x = 1.0f;
		MadeFrame = TheFrameRateManager->FrameNumber;
		if (Moving.empty() || !Compile(d)) { if (Failed) Data.x = 0.0f; return; }
		if (!Drawer) Drawer = new DrawPass();

		const D3DXVECTOR4* scale = Constant("TESR_RenderScale");
		const D3DXVECTOR4* camera = Constant("TESR_CameraData");
		const float farZ = camera ? camera->y : TheRenderManager->CameraData.y;

		// Save what the chain relies on (as CapturedNormals::MergeIntoNormals does); the shaders through the game's state cache.
		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; IDirect3DIndexBuffer9* ib = nullptr; IDirect3DBaseTexture9* tex0 = nullptr;
		IDirect3DSurface9* rt0 = nullptr; D3DVIEWPORT9 vp = {}; DWORD fvf = 0;
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetStreamSource(0, &vb, &vbOffset, &vbStride); d->GetIndices(&ib); d->GetTexture(0, &tex0);
		d->GetRenderTarget(0, &rt0); d->GetViewport(&vp);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(0, kSS[i], &savedS[i]))) savedS[i] = wantS[i]; if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], wantS[i]); }
		float savedPS[2][4] = {};
		d->GetPixelShaderConstantF(0, &savedPS[0][0], 2);

		d->SetRenderTarget(0, Surface);
		// RenderScale v2: the world was drawn into the target's top-left share; draw where it drew (the depth texture is read per pixel).
		D3DVIEWPORT9 drawn = { 0, 0, (DWORD)(dd.Width * (scale ? scale->x : 1.0f) + 0.5f), (DWORD)(dd.Height * (scale ? scale->y : 1.0f) + 0.5f), 0.0f, 1.0f };
		if (!drawn.Width || drawn.Width > dd.Width) drawn.Width = dd.Width;
		if (!drawn.Height || drawn.Height > dd.Height) drawn.Height = dd.Height;
		d->SetViewport(&drawn);
		state->SetVertexShader(VS, false);
		state->SetPixelShader(PS, false);
		d->SetTexture(0, depth);
		const float pc[2][4] = { { farZ, 0.02f, 4.0f, 512.0f }, { 1.0f / dd.Width, 1.0f / dd.Height, 0.0f, 0.0f } };
		d->SetPixelShaderConstantF(0, &pc[0][0], 2);

		// The camera-relative view-projection without the jitter (only the projection's scales; the effects' camera is unjittered by now).
		D3DXMATRIX view = TheRenderManager->viewMatrix; view._41 = view._42 = view._43 = 0.0f;
		D3DXMATRIX proj = TheRenderManager->projMatrix; proj._31 = proj._32 = 0.0f;
		const D3DXMATRIX viewProj = view * proj;
		d->SetVertexShaderConstantF(4, (const float*)&viewProj, 4);
		const float forward[4] = { TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z, 0.0f };
		d->SetVertexShaderConstantF(123, forward, 1);

		unsigned drawnGeos = 0, drawnSkinned = 0;
		for (NiGeometry* geo : Moving) {
			int kind = 0;
			if (!DrawGuarded(d, geo, kind)) { Logger::Log("[ERROR] ObjectMotion: a fault drawing a moved geometry: off for the session"); Failed = true; break; }
			if (kind) drawnGeos++;
			if (kind == 2) drawnSkinned++;
		}

		// Put it all back.
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], savedS[i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, &savedPS[0][0], 2);
		if (rt0) { d->SetRenderTarget(0, rt0); rt0->Release(); }
		d->SetViewport(&vp);
		d->SetTexture(0, tex0); if (tex0) tex0->Release();
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
		d->SetIndices(ib); if (ib) ib->Release();
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();

		DrawnGeometries += drawnGeos; DrawnSkinned += drawnSkinned;
		Data.x = Failed ? 0.0f : 1.0f;
		Data.y = (float)drawnGeos;
		if (++Frames >= 600) {
			// What the buffer holds this frame, read back small (a 96 x 54 point copy; one wait for the GPU every 600 frames): how many
			// pixels carry a move, and the largest one (game units). Proof the moved things land on screen, not only that they were drawn.
			int moving = -1; float largest = 0.0f;
			if (drawnGeos) {
				if (!CheckTarget && (FAILED(d->CreateRenderTarget(96, 54, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &CheckTarget, NULL)) ||
					FAILED(d->CreateOffscreenPlainSurface(96, 54, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &CheckCopy, NULL)))) {
					if (CheckTarget) { CheckTarget->Release(); CheckTarget = nullptr; }
				}
				D3DLOCKED_RECT r = {};
				if (CheckTarget && SUCCEEDED(d->StretchRect(Surface, NULL, CheckTarget, NULL, D3DTEXF_POINT)) && SUCCEEDED(d->GetRenderTargetData(CheckTarget, CheckCopy)) &&
					SUCCEEDED(CheckCopy->LockRect(&r, NULL, D3DLOCK_READONLY))) {
					moving = 0;
					for (int y = 0; y < 54; y++) {
						const D3DXFLOAT16* row = (const D3DXFLOAT16*)((const char*)r.pBits + y * r.Pitch);
						for (int x = 0; x < 96; x++) {
							float v[4]; D3DXFloat16To32Array(v, row + x * 4, 4);
							if (v[3] > 0.5f) { moving++; largest = (std::max)(largest, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])); }
						}
					}
					CheckCopy->UnlockRect();
				}
			}
			Logger::Log("ObjectMotion: per frame %.1f geometries noted, %.1f moved ones drawn (%.1f skinned), %.1f left out as following the camera; this frame %d of 5184 sampled pixels carry a move (largest %.2f units); %u notes found another geometry at a known address (identity check %s)",
				Noted / (double)Frames, DrawnGeometries / (double)Frames, DrawnSkinned / (double)Frames, CameraFollowers / (double)Frames, moving, largest,
				Reused, IdentityCheck ? "on" : "off");
			Frames = DrawnGeometries = DrawnSkinned = Noted = CameraFollowers = Reused = 0;
			// UNOFFICIAL 2026-10-30 (blur ring hunt): what moved this frame, against the camera's own move.
			for (size_t k = 0; k < Moving.size() && k < 8; k++) {
				auto found = Map.find(Moving[k]);
				if (found == Map.end()) continue;
				const Entry& e = found->second;
				const D3DXVECTOR3 cam = e.camCur - e.camPrev;
				const float mx = e.cur.pos.x - e.prev.pos.x, my = e.cur.pos.y - e.prev.pos.y, mz = e.cur.pos.z - e.prev.pos.z;
				const NiAVObject* parent = (const NiAVObject*)Moving[k]->m_parent;
				Logger::Log("ObjectMotion: moved '%s' (parent '%s') %s: move %.2f %.2f %.2f, camera %.2f %.2f %.2f, radius %.0f",
					Moving[k]->m_pcName ? Moving[k]->m_pcName : "", parent && parent->m_pcName ? parent->m_pcName : "", e.skinned ? "skinned" : "rigid",
					mx, my, mz, cam.x, cam.y, cam.z, Moving[k]->m_kWorldBound ? Moving[k]->m_kWorldBound->Radius : -1.0f);
			}
		}
	}
}
