#pragma once

// UNOFFICIAL 2026-10-31 CapsuleShadows ([Shaders.ShadowsInteriors.Capsules] Enabled, restart, off; docs/next/INTERIOR-SHADOWS-MODERN.md
// steps 2 and 3). Indoors, the lamp shadows of PEOPLE (capsules from their bones) and the small CONTACT shadows the shadow maps cannot
// resolve (a short screen-space ray toward the lamp) are worked out per pixel in one D3D9 pass, for each pixel's three strongest shadow
// lamps, and written into the same per-pixel lamp visibility buffer the RT lamp shadows fill (RtLampShadows.h, TESR_RtLampShadow:
// cube slot + 4-bit visibility per entry, three entries, a depth code). The object and skin shaders read it as they read the traced
// result (PointShadowForward.hlsl POINT_SHADOW_RT: the next frame reprojects into this one, the darker of it and the lamp's own map),
// so no forward shader changes and no RT GPU is needed: this is design section 4's "no RT GPU" path (screenVis = capsules x contact).
//   - Capsules: ~15 per human skeleton (torso 2, head, upper/lower arm and hand, thigh, calf and foot; Bip01 names), one upright capsule
//     from the bound for other skeletons; MaxActors nearest the camera within MaxDistance (the player in third person, or always with
//     PlayerFirstPerson). Per pixel and lamp: the lamp as a disc of LampSize seen from the pixel, each capsule as a strip across it at the
//     ray's closest point: the covered share is the occlusion (soft, with the penumbra the lamp's size gives).
//   - DropActors: people are left out of the lamp shadow maps (and of their redraw tests), so a lamp is no longer redrawn when someone walks
//     by; their shadows come from the capsules alone. (Where the buffer has no answer -- a lamp not in a pixel's top three, the screen
//     edge, something that just moved in front -- that lamp then shows no people shadow.)
//   - Contact: ContactSteps depth tests along ContactLength units toward the lamp; a hit thinner than ContactThickness blocks.
// Live A/B: Capsules / Contact (either on = the pass runs and the buffer is read; both off = exactly the lamp maps, as without it; DropActors
// redraws the maps when switched). Enabled false = nothing compiled in (no POINT_SHADOW_RT unless the RT lamp shadows ask for it).
// With the RT lamp shadows running (an RT GPU), they own the buffer and this pass does nothing.
#include "GpuProfiler.h"
#include "RtLampShadows.h"
#include "RenderRect.h"
#include <atomic>
#include <thread>

namespace CapsuleShadows {

	inline constexpr const char* kSection = "Shaders.ShadowsInteriors.Capsules";
	inline constexpr int MaxActorsCap = 16, CapsulesPerActor = 16, RowTexels = 1 + 2 * CapsulesPerActor;	// a row: the bound, then a, b per capsule
	inline constexpr int CapsuleTexWidth = 64;

	inline int Wanted = -1;
	inline bool CompiledIn() {
		if (Wanted < 0) Wanted = TheSettingManager->GetSettingI(kSection, "Enabled") != 0;
		return Wanted != 0;
	}
	// Live: the pass runs this frame (set by RtLampShadows::Submit through Wants)
	inline bool Wants() {
		return CompiledIn() && !TheShaderManager->GameState.isExterior &&
			(TheSettingManager->GetSettingI(kSection, "Capsules") != 0 || TheSettingManager->GetSettingI(kSection, "Contact") != 0);
	}
	// DropActors: ShadowManager leaves skinned casters (people, creatures) out of the lamp maps while the capsules are live
	// (2026-11-15 docs/next/FD-NATIVEFIX.md: not while some lit shader's record has no capsule read, RtLampShadows::ReadDropped: people
	// would cast no lamp shadow on its objects; native with Capsules, the six-lamp shiny records)
	inline bool DropActors() {
		return CompiledIn() && !RtLampShadows::ReadDropped && !TheShaderManager->GameState.isExterior && TheSettingManager->GetSettingI(kSection, "Capsules") != 0 &&
			TheSettingManager->GetSettingI(kSection, "DropActors") != 0;
	}

	inline IDirect3DPixelShader9* PS = nullptr;
	inline bool Failed = false;
	inline IDirect3DTexture9* CapsuleTexture = nullptr;
	inline IDirect3DSurface9* CapsuleSurface = nullptr;
	inline IDirect3DTexture9* CapsuleStaging = nullptr;
	inline unsigned StatFrames = 0, StatActors = 0, StatCapsules = 0, StatHumans = 0, StatLamps = 0;

	// --- The capsules -------------------------------------------------------------------------------------------------------------
	// Bip01 bones: index -> name (the human skeleton; FNV's neck is "Bip01 Neck1", older ones "Bip01 Neck")
	enum Bone { bPelvis, bSpine2, bNeck, bHead, bLUpper, bLFore, bLHand, bRUpper, bRFore, bRHand, bLThigh, bLCalf, bLFoot, bLToe, bRThigh, bRCalf, bRFoot, bRToe, bCount };
	inline const char* const BoneNames[bCount] = { "Bip01 Pelvis", "Bip01 Spine2", "Bip01 Neck1", "Bip01 Head", "Bip01 L UpperArm", "Bip01 L Forearm",
		"Bip01 L Hand", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 L Toe0",
		"Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot", "Bip01 R Toe0" };
	// The capsules: bone a -> bone b (b < 0: a's own direction from its parent bone `extend`, times `along`), radius in units
	struct CapsuleDef { int a, b, from; float along, radius; };
	inline const CapsuleDef Capsules[] = {
		{ bPelvis, bSpine2, -1, 0.0f, 7.5f }, { bSpine2, bNeck, -1, 0.0f, 8.0f },
		{ bHead, -1, bNeck, 1.1f, 5.0f },
		{ bLUpper, bLFore, -1, 0.0f, 2.8f }, { bLFore, bLHand, -1, 0.0f, 2.3f }, { bLHand, -1, bLFore, 0.3f, 1.9f },
		{ bRUpper, bRFore, -1, 0.0f, 2.8f }, { bRFore, bRHand, -1, 0.0f, 2.3f }, { bRHand, -1, bRFore, 0.3f, 1.9f },
		{ bLThigh, bLCalf, -1, 0.0f, 4.3f }, { bLCalf, bLFoot, -1, 0.0f, 3.2f }, { bLFoot, bLToe, -1, 0.0f, 2.2f },
		{ bRThigh, bRCalf, -1, 0.0f, 4.3f }, { bRCalf, bRFoot, -1, 0.0f, 3.2f }, { bRFoot, bRToe, -1, 0.0f, 2.2f },
	};
	static_assert(sizeof(Capsules) / sizeof(Capsules[0]) <= CapsulesPerActor, "CapsuleShadows: one row of capsules per actor");

	// The bone nodes per root (walked again when the root changes or every 30 frames; read under SEH, as BloodFlow does)
	struct Skeleton { NiNode* root = nullptr; NiAVObject* bones[bCount] = {}; int found = 0; UInt32 walked = 0, seen = 0; };
	inline std::unordered_map<UInt32, Skeleton> Skeletons;
	inline UInt32 Frame = 0;

	inline int WalkBones(NiNode* root, NiAVObject** bones) {
		memset(bones, 0, sizeof(NiAVObject*) * bCount);
		NiAVObject* stack[96];
		int top = 0, seen = 0, found = 0;
		stack[top++] = root;
		while (top && seen < 320) {
			NiAVObject* object = stack[--top];
			if (!object) continue;
			seen++;
			const char* name = object->m_pcName;
			if (name && name[0] == 'B' && !strncmp(name, "Bip01 ", 6))
				for (int k = 0; k < bCount; k++)
					if (!bones[k] && (!strcmp(name, BoneNames[k]) || (k == bNeck && !strcmp(name, "Bip01 Neck")))) { bones[k] = object; found++; break; }
			NiNode* node = object->IsNiNode();
			if (!node) continue;
			for (UInt32 i = 0; i < node->m_children.end && top < 96; i++)
				if (node->m_children.data[i]) stack[top++] = node->m_children.data[i];
		}
		return found;
	}

	struct ActorShape { float bound[4]; float caps[CapsulesPerActor][2][4]; int count; bool human; float distance; };

	// One actor's capsules (camera-relative), false when it has no 3D
	inline bool ShapeOf(UInt32 refID, NiNode* root, const D3DXVECTOR4& camera, float radiusScale, ActorShape& out) {
		if (!root || !root->m_kWorldBound) return false;
		const NiBound* bound = root->m_kWorldBound;
		if (!(bound->Radius > 1.0f) || bound->Radius > 2000.0f) return false;
		out.bound[0] = bound->Center.x - camera.x; out.bound[1] = bound->Center.y - camera.y; out.bound[2] = bound->Center.z - camera.z;
		out.count = 0;
		out.human = false;
		Skeleton& s = Skeletons[refID];
		s.seen = Frame;
		if (s.root != root || Frame - s.walked >= 30) { s.root = root; s.found = WalkBones(root, s.bones); s.walked = Frame; }
		auto at = [&](int k, float* p) {
			const NiPoint3& w = s.bones[k]->m_worldTransform.pos;
			p[0] = w.x - camera.x; p[1] = w.y - camera.y; p[2] = w.z - camera.z;
		};
		float reach = 0.0f;
		if (s.found >= 12 && s.bones[bPelvis] && s.bones[bHead]) {
			out.human = true;
			for (const CapsuleDef& c : Capsules) {
				if (!s.bones[c.a] || (c.b >= 0 && !s.bones[c.b]) || (c.b < 0 && !s.bones[c.from])) continue;
				float* a = out.caps[out.count][0]; float* b = out.caps[out.count][1];
				at(c.a, a);
				if (c.b >= 0) at(c.b, b);
				else {
					float f[3]; at(c.from, f);
					for (int i = 0; i < 3; i++) b[i] = a[i] + (a[i] - f[i]) * c.along;
				}
				a[3] = c.radius * radiusScale; b[3] = 1.0f;
				for (int i = 0; i < 2; i++) {
					const float* p = out.caps[out.count][i];
					const float dx = p[0] - out.bound[0], dy = p[1] - out.bound[1], dz = p[2] - out.bound[2];
					reach = (std::max)(reach, sqrtf(dx * dx + dy * dy + dz * dz) + a[3]);
				}
				out.count++;
			}
		}
		if (!out.count) {	// another skeleton: one upright capsule in the bound
			const float r = bound->Radius * 0.45f * radiusScale, h = bound->Radius * 0.45f;
			float* a = out.caps[0][0]; float* b = out.caps[0][1];
			a[0] = b[0] = out.bound[0]; a[1] = b[1] = out.bound[1];
			a[2] = out.bound[2] - h; b[2] = out.bound[2] + h;
			a[3] = r; b[3] = 1.0f;
			out.count = 1;
			reach = h + r;
		}
		out.bound[3] = reach + 1.0f;
		return true;
	}

	// This frame's actors into the capsule texture (rows of RowTexels: the bound, then a, b of each capsule; a.w its radius, 0 = none)
	inline int WriteCapsules(IDirect3DDevice9* d, int& capsuleCount) {
		capsuleCount = 0;
		Frame++;
		if (!CapsuleTexture) {
			if (FAILED(d->CreateTexture(CapsuleTexWidth, MaxActorsCap, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &CapsuleTexture, NULL)) ||
				FAILED(CapsuleTexture->GetSurfaceLevel(0, &CapsuleSurface))) { CapsuleTexture = nullptr; return -1; }
		}
		if (!CapsuleStaging && FAILED(d->CreateTexture(CapsuleTexWidth, MaxActorsCap, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &CapsuleStaging, NULL))) {
			CapsuleStaging = nullptr;
			return -1;
		}
		const int maxActors = (std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "MaxActors"), 0), MaxActorsCap);
		const float maxDistance = (std::max)(TheSettingManager->GetSettingF(kSection, "MaxDistance"), 0.0f);
		const float radiusScale = (std::max)(TheSettingManager->GetSettingF(kSection, "RadiusScale"), 0.1f);
		const bool playerFirst = TheSettingManager->GetSettingI(kSection, "PlayerFirstPerson") != 0;
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		static ActorShape found[64];
		int n = 0;
		__try {
			auto consider = [&](Actor* actor) {
				if (!actor || n >= 64 || (actor->flags & 0x800) != 0) return;	// disabled
				ActorShape& s = found[n];
				if (!ShapeOf(actor->refID, actor->GetNode(), camera, radiusScale, s)) return;
				s.distance = sqrtf(s.bound[0] * s.bound[0] + s.bound[1] * s.bound[1] + s.bound[2] * s.bound[2]);
				if (s.distance - s.bound[3] > maxDistance) return;
				n++;
			};
			if (Player && (playerFirst || !((PlayerCharacter*)Player)->IsFirstPerson())) consider((Actor*)Player);
			UInt8* lists = (UInt8*)0x11E0E80;	// ProcessLists (as BloodFlow): the high and middle-high actors
			Actor** data = *(Actor***)(lists + 0x08);
			for (int level = 0; data && level < 2; level++) {
				const UInt32 begin = *(UInt32*)(lists + 0x14 + level * 4), end = *(UInt32*)(lists + 0x24 + level * 4);
				if (end < begin || end - begin > 4096) continue;
				for (UInt32 i = begin; i < end; i++) if (data[i] && (TESObjectREFR*)data[i] != (TESObjectREFR*)Player) consider(data[i]);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			static bool logged = false;
			if (!logged) { logged = true; Logger::Log("[ERROR] CapsuleShadows: reading the actors failed (once); the capsules of this frame are the ones read before it"); }
		}
		// the nearest MaxActors
		std::sort(found, found + n, [](const ActorShape& a, const ActorShape& b) { return a.distance < b.distance; });
		n = (std::min)(n, maxActors);
		D3DLOCKED_RECT lock;
		if (FAILED(CapsuleStaging->LockRect(0, &lock, NULL, 0))) return -1;
		for (int row = 0; row < MaxActorsCap; row++) {
			float* t = (float*)((UInt8*)lock.pBits + row * lock.Pitch);
			memset(t, 0, CapsuleTexWidth * 4 * sizeof(float));
			if (row >= n) continue;
			const ActorShape& s = found[row];
			memcpy(t, s.bound, sizeof(s.bound));
			for (int c = 0; c < s.count; c++) { memcpy(t + 4 * (1 + 2 * c), s.caps[c][0], 4 * sizeof(float)); memcpy(t + 4 * (2 + 2 * c), s.caps[c][1], 4 * sizeof(float)); }
			capsuleCount += s.count;
			if (GpuTimer::Enabled && s.human) StatHumans++;
		}
		CapsuleStaging->UnlockRect(0);
		IDirect3DSurface9* staging = nullptr;
		if (FAILED(CapsuleStaging->GetSurfaceLevel(0, &staging))) return -1;
		const HRESULT hr = d->UpdateSurface(staging, NULL, CapsuleSurface, NULL);
		staging->Release();
		// forget skeletons not seen for a while (refIDs of actors gone)
		if ((Frame % 600) == 0) for (auto it = Skeletons.begin(); it != Skeletons.end();) { if (Frame - it->second.seen > 600) it = Skeletons.erase(it); else ++it; }
		return SUCCEEDED(hr) ? n : -1;
	}

	// --- The pass -----------------------------------------------------------------------------------------------------------------
	// s0 TESR_DepthBuffer (view z / farZ), s1 TESR_NormalsBuffer (view-space normal x 0.5 + 0.5), s2 the lamp table (RtLampShadows::
	// WriteTable: 4 RG texels a lamp: pos.xy | pos.z, reach | intensity, game radius | stop, size), s3 the capsules. Point sampled.
	inline const char* kPS = R"(
sampler2D Depth : register(s0);
sampler2D Normals : register(s1);
sampler2D Table : register(s2);
sampler2D Caps : register(s3);
float4 Size : register(c0);        // w, h, 1/w, 1/h
float4 Forward : register(c1);     // forward (unit), w farZ
float4 Right : register(c2);       // right / p11
float4 Up : register(c3);          // up / p22
float4 P : register(c4);           // x lamps in the table, y 4095 / log2(farZ), z capsules on, w contact on
float4 C : register(c5);           // x actors, y capsule strength, z self margin, w frame jitter
float4 K : register(c6);           // x contact length, y steps, z thickness, w contact strength
float4 O : register(c7);           // x p11, y p22, z start offset, w start offset per unit
float4 R : register(c8);           // x RankBy, y contact bias per unit, z capsule lamp size scale, w the contact's screen-edge fade
float4 Rect : register(c9);        // xy the render rect (RenderScale below 1: depth and normals fill the top-left xy of their textures)

float4 Lamp(float s, float i) { return tex2Dlod(Table, float4((4.0f * s + i + 0.5f) / 256.0f, 0.5f, 0.0f, 0.0f)); }
float4 Cap(float a, float i) { return tex2Dlod(Caps, float4((i + 0.5f) / 64.0f, (a + 0.5f) / 16.0f, 0.0f, 0.0f)); }

// How much of a lamp (a disc of radius lampSize at lamp, seen from o) the capsules leave visible
float CapsuleVis(float3 o, float3 lamp, float lampSize, float stop) {
    float3 seg = lamp - o;
    float T = length(seg);
    float3 d = seg / T;
    float T2 = max(T - stop, 1.0f);
    float alpha = max(lampSize, 0.25f) / T;
    float vis = 1.0f;
    [loop] for (int a = 0; a < 16; a++) {
        if (a >= C.x) break;
        float4 b = Cap(a, 0.0f);
        // the bound: does the ray toward the lamp (with the lamp's size) pass it?
        float3 q = b.xyz - o;
        float tb = clamp(dot(q, d), 0.0f, T2);
        float3 off = q - d * tb;
        float wide = b.w + lampSize * tb / T;
        if (dot(off, off) > wide * wide) continue;
        [loop] for (int c = 0; c < 16; c++) {
            float4 A = Cap(a, 1.0f + 2.0f * c);
            if (!(A.w > 0.0f)) break;
            float3 B = Cap(a, 2.0f + 2.0f * c).xyz;
            float3 e = B - A.xyz;
            float ee = max(dot(e, e), 1e-4f);
            // the pixel itself on (or in) this capsule: it does not shadow itself
            float3 r0 = o - A.xyz;
            float3 self = r0 - e * saturate(dot(r0, e) / ee);
            if (dot(self, self) < (A.w + C.z) * (A.w + C.z)) continue;
            // closest points of the ray o + d t (0..T2) and the axis A + e s (0..1)
            float bb = dot(d, e), cc = dot(d, r0), ff = dot(e, r0);
            float den = ee - bb * bb;
            float t = den > 1e-4f ? clamp((bb * ff - cc * ee) / den, 0.0f, T2) : 0.0f;
            float s = (bb * t + ff) / ee;
            if (s < 0.0f) { s = 0.0f; t = clamp(-cc, 0.0f, T2); }
            else if (s > 1.0f) { s = 1.0f; t = clamp(bb - cc, 0.0f, T2); }
            float3 gap = (o + d * t) - (A.xyz + e * s);
            float tt = max(t, 0.5f);
            float beta = A.w / tt, gamma = length(gap) / tt;
            // the capsule as a strip of half-width beta, gamma off the lamp disc's centre: the share of the disc's width it covers
            float covered = saturate((min(alpha, gamma + beta) - max(-alpha, gamma - beta)) / (2.0f * alpha));
            covered = covered * covered * (3.0f - 2.0f * covered);
            vis *= 1.0f - C.y * covered;
        }
    }
    return vis;
}

// A short screen-space ray toward the lamp: blocked where the depth buffer has something in front within the thickness
float ContactVis(float3 o, float3 lamp, float stop, float2 pixel) {
    float3 seg = lamp - o;
    float dist = length(seg);
    float L = min(K.x, dist - stop);
    if (!(L > 1.0f)) return 1.0f;
    float3 d = seg / dist;
    float jitter = frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))) + C.w);
    float steps = K.y;
    [loop] for (int i = 0; i < 32; i++) {
        if (i >= steps) break;
        float t = (i + jitter) / steps * L;
        float3 x = o + d * t;
        float z = dot(x, Forward.xyz);
        if (z < 1.0f) break;
        float2 uv = float2(dot(x, Right.xyz) * O.x * O.x / z, -dot(x, Up.xyz) * O.y * O.y / z) * 0.5f + 0.5f;
        if (any(uv < 0.0f) || any(uv > 1.0f)) break;
        float sz = tex2Dlod(Depth, float4(uv * Rect.xy, 0.0f, 0.0f)).r * Forward.w;
        float dz = z - sz;
        if (dz > R.y * z + 0.25f && dz < K.z) {
            float edge = saturate(min(min(uv.x, uv.y), min(1.0f - uv.x, 1.0f - uv.y)) * R.w);
            float fade = 1.0f - saturate((t / L - 0.6f) / 0.4f);
            return 1.0f - K.w * edge * fade;
        }
    }
    return 1.0f;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    const float none = 1023.0f;
    float lin = tex2Dlod(Depth, float4(uv * Rect.xy, 0.0f, 0.0f)).r;
    float viewZ = lin * Forward.w;
    if (!(lin > 0.0f) || lin >= 0.9999f || viewZ < 1.0f) return float4(none + 1024.0f * none, none, 0.0f, 0.0f);
    float3 pos = (Forward.xyz + Right.xyz * (2.0f * uv.x - 1.0f) - Up.xyz * (2.0f * uv.y - 1.0f)) * viewZ;
    float3 n = tex2Dlod(Normals, float4(uv * Rect.xy, 0.0f, 0.0f)).xyz * 2.0f - 1.0f;
    float3 normal = normalize(normalize(Right.xyz) * n.x + normalize(Up.xyz) * n.y + Forward.xyz * n.z);
    float3 o = pos + normal * (O.z + O.w * viewZ);

    // the pixel's three strongest shadow lamps (as RtLampTrace.comp)
    float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f, s0 = -1.0f, s1 = -1.0f, s2 = -1.0f;
    [loop] for (int s = 0; s < 63; s++) {
        if (s >= P.x) break;
        float4 b = Lamp(s, 1.0f);
        if (!(b.y > 0.0f)) continue;
        float4 a = Lamp(s, 0.0f);
        float3 toLamp = float3(a.xy, b.x) - o;
        float d2 = dot(toLamp, toLamp);
        if (d2 >= b.y * b.y) continue;
        float ndl = dot(toLamp, normal);
        if (ndl <= 0.0f) continue;
        float4 c = Lamp(s, 2.0f);
        float falloff = saturate(1.0f - d2 / max(c.y * c.y, 1.0f));
        float w = R.x > 0.5f ? (falloff > 0.0f ? falloff + 1e-3f : 0.0f) : c.x * max(falloff, 0.02f) * ndl * rsqrt(d2);
        if (!(w > 0.0f)) continue;
        if (w > w2) {
            if (w > w1) {
                w2 = w1; s2 = s1;
                if (w > w0) { w1 = w0; s1 = s0; w0 = w; s0 = s; }
                else { w1 = w; s1 = s; }
            }
            else { w2 = w; s2 = s; }
        }
    }
    // (a real loop over the three, not unrolled: the unrolled pass took the D3DX compiler 9.4 s)
    float3 entry = none;
    [loop] for (int k = 0; k < 3; k++) {
        float sl = k == 0 ? s0 : (k == 1 ? s1 : s2);
        if (sl < 0.0f) break;
        float4 a = Lamp(sl, 0.0f), b = Lamp(sl, 1.0f), e = Lamp(sl, 3.0f);
        float3 lamp = float3(a.xy, b.x);
        float vis = 1.0f;
        [branch] if (P.z > 0.5f) vis *= CapsuleVis(o, lamp, e.y * R.z, e.x);
        [branch] if (P.w > 0.5f && vis > 0.0f) vis *= ContactVis(o, lamp, e.x, uv * Size.xy);
        float value = sl + 64.0f * floor(15.0f * saturate(vis) + 0.5f);
        entry = k == 0 ? float3(value, entry.yz) : (k == 1 ? float3(entry.x, value, entry.z) : float3(entry.xy, value));
    }
    float code = clamp(floor(log2(viewZ) * P.y + 0.5f), 1.0f, 4095.0f);
    return float4(entry.x + 1024.0f * entry.y, entry.z + 1024.0f * code, 0.0f, 0.0f);
}
)";

	// The pass shader is compiled on a worker thread (D3DX only, no device), started on the first frame with the feature compiled in,
	// and made on the render thread once it is done; until then the pass does not run (the buffer stays unused: the lamp maps). On the
	// render thread the first compile stalled the game 9.4 s the moment Capsules was switched on (run 20261031-013358).
	inline std::atomic<int> CompileState{ 0 };	// 0 not started, 1 compiling, 2 done (CompileCode or CompileErrors set)
	inline ID3DXBuffer* CompileCode = nullptr;
	inline ID3DXBuffer* CompileErrors = nullptr;
	inline double CompileSeconds = 0.0;
	inline bool Ensure(IDirect3DDevice9* d) {
		if (PS) return true;
		if (Failed) return false;
		int state = CompileState.load();
		if (state == 0) {
			CompileState = 1;
			std::thread([] {
				const DWORD start = GetTickCount();
				if (FAILED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_3_0", 0, &CompileCode, &CompileErrors, NULL)) && CompileCode) {
					CompileCode->Release(); CompileCode = nullptr;
				}
				CompileSeconds = (GetTickCount() - start) / 1000.0;
				CompileState = 2;
			}).detach();
			return false;
		}
		if (state != 2) return false;
		const bool ok = CompileCode && SUCCEEDED(d->CreatePixelShader((const DWORD*)CompileCode->GetBufferPointer(), &PS));
		if (!ok) Logger::Log("[ERROR] CapsuleShadows: the pass shader failed (%s): off for the session (the lamp maps as before)", CompileErrors ? (const char*)CompileErrors->GetBufferPointer() : "?");
		else Logger::Log("UNOFFICIAL CapsuleShadows: pass shader compiled on a worker thread in %.1f s (%u bytes)", CompileSeconds, (unsigned)CompileCode->GetBufferSize());
		if (CompileCode) { CompileCode->Release(); CompileCode = nullptr; }
		if (CompileErrors) { CompileErrors->Release(); CompileErrors = nullptr; }
		if (!ok) { Failed = true; PS = nullptr; }
		return ok;
	}

	// ShaderManager, right after RtCore::Frame (the depth and normals exist): the buffer for the next frame's object shaders
	inline void Render(IDirect3DDevice9* d) {
		using namespace RtLampShadows;
		if (!CompiledIn() || !Ensure(d) || !ScreenSourceFrame || LampCount <= 0) return;	// (Ensure first: the compile starts at load)
		static GpuTimer timer("Capsule + contact shadows");
		GpuProfileScope gpu(timer, d);
		const UINT w = TheRenderManager->width, h = TheRenderManager->height;
		if (!ResultTexture || TextureWidth != w || TextureHeight != h) {
			if (ResultSurface) { ResultSurface->Release(); ResultSurface = nullptr; }
			if (ResultTexture) { ResultTexture->Release(); ResultTexture = nullptr; }
			Traced.valid = false;
			for (int i = 0; i < MapSlots; i++) TracedMap[i] = -1;
			TheTextureManager->InitTexture("TESR_RtLampShadow", &ResultTexture, &ResultSurface, w, h, D3DFMT_G32R32F);
			TextureWidth = w; TextureHeight = h;
			if (!ResultTexture) return;
		}
		IDirect3DBaseTexture9* depth = Texture("TESR_DepthBuffer");
		IDirect3DBaseTexture9* normals = Texture("TESR_NormalsBuffer");
		const D3DXVECTOR4* cameraData = Constant("TESR_CameraData");
		if (!depth || !normals || !cameraData) return;
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		if (!WriteTable(camera, (std::max)(TheSettingManager->GetSettingF(kSection, "LampSize"), 0.0f), TheSettingManager->GetSettingF(kSection, "InvisibleLightSize"))) return;
		const bool capsulesOn = TheSettingManager->GetSettingI(kSection, "Capsules") != 0;
		int capsuleCount = 0;
		const int actors = capsulesOn ? WriteCapsules(d, capsuleCount) : 0;
		if (actors < 0) return;

		const D3DXMATRIX& view = TheRenderManager->viewMatrix;
		const D3DXMATRIX& proj = TheRenderManager->projMatrix;
		static UInt32 frame = 0;
		frame++;
		float c[10][4] = {};
		c[0][0] = (float)w; c[0][1] = (float)h; c[0][2] = 1.0f / w; c[0][3] = 1.0f / h;
		c[1][0] = view._13; c[1][1] = view._23; c[1][2] = view._33; c[1][3] = cameraData->y;
		c[2][0] = view._11 / proj._11; c[2][1] = view._21 / proj._11; c[2][2] = view._31 / proj._11;
		c[3][0] = view._12 / proj._22; c[3][1] = view._22 / proj._22; c[3][2] = view._32 / proj._22;
		c[4][0] = (float)TableSlots;	// the table is by cube slot: every slot below 63 is looked at (empty ones have reach 0)
		c[4][1] = 4095.0f / log2f((std::max)(cameraData->y, 2.0f));
		c[4][2] = capsulesOn && actors > 0 ? 1.0f : 0.0f;
		c[4][3] = TheSettingManager->GetSettingI(kSection, "Contact") != 0 ? 1.0f : 0.0f;
		c[5][0] = (float)actors;
		c[5][1] = (std::min)((std::max)(TheSettingManager->GetSettingF(kSection, "Strength"), 0.0f), 1.0f);
		c[5][2] = (std::max)(TheSettingManager->GetSettingF(kSection, "SelfMargin"), 0.0f);
		c[5][3] = TheSettingManager->GetSettingI(kSection, "ContactJitter") ? fmodf((float)frame * 0.61803399f, 1.0f) : 0.0f;
		c[6][0] = (std::max)(TheSettingManager->GetSettingF(kSection, "ContactLength"), 0.0f);
		c[6][1] = (float)(std::min)((std::max)(TheSettingManager->GetSettingI(kSection, "ContactSteps"), 1), 32);
		c[6][2] = (std::max)(TheSettingManager->GetSettingF(kSection, "ContactThickness"), 0.5f);
		c[6][3] = (std::min)((std::max)(TheSettingManager->GetSettingF(kSection, "ContactStrength"), 0.0f), 1.0f);
		c[7][0] = proj._11; c[7][1] = proj._22;
		c[7][2] = (std::max)(TheSettingManager->GetSettingF(kSection, "StartOffset"), 0.0f);
		c[7][3] = (std::max)(TheSettingManager->GetSettingF(kSection, "StartOffsetPerUnit"), 0.0f);
		c[8][0] = (float)TheSettingManager->GetSettingI(kSection, "RankBy");
		c[8][1] = (std::max)(TheSettingManager->GetSettingF(kSection, "ContactBias"), 0.0f);
		c[8][2] = 1.0f;
		c[8][3] = 20.0f;
		// RenderScale below 1 (RenderRect.h): this pass's output stays screen-sized (the object shaders reproject into the whole screen),
		// its depth and normal reads take the drawn top-left part (run 20261031-013358: the top-left 2/3 x 2/3 never matched)
		c[9][0] = RenderRect::Active() ? RenderRect::ScaleX : 1.0f; c[9][1] = RenderRect::Active() ? RenderRect::ScaleY : 1.0f;

		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DSurface9* rt0 = nullptr; IDirect3DSurface9* zs = nullptr; IDirect3DBaseTexture9* tex[4] = {}; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetRenderTarget(0, &rt0); d->GetDepthStencilSurface(&zs); d->GetViewport(&vp);
		for (int i = 0; i < 4; i++) d->GetTexture(i, &tex[i]);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[4][sizeof(kSS) / sizeof(kSS[0])];
		for (int s = 0; s < 4; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(s, kSS[i], &savedS[s][i]))) savedS[s][i] = wantS[i]; if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], wantS[i]); }
		float savedPS[10][4] = {};
		d->GetPixelShaderConstantF(0, &savedPS[0][0], 10);

		d->SetRenderTarget(0, ResultSurface);
		d->SetDepthStencilSurface(NULL);
		const D3DVIEWPORT9 full = { 0, 0, w, h, 0.0f, 1.0f };
		d->SetViewport(&full);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(PS, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		d->SetTexture(0, depth); d->SetTexture(1, normals); d->SetTexture(2, TableTexture); d->SetTexture(3, CapsuleTexture);
		d->SetPixelShaderConstantF(0, &c[0][0], 10);
		struct V { float x, y, z, w, u, v; };
		const float x1 = w - 0.5f, y1 = h - 0.5f;
		const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { x1, -0.5f, 0, 1, 1, 0 }, { -0.5f, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));

		for (int s = 0; s < 4; s++)
			for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[s][i] != wantS[i]) d->SetSamplerState(s, kSS[i], savedS[s][i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, &savedPS[0][0], 10);
		if (rt0) { d->SetRenderTarget(0, rt0); rt0->Release(); }
		d->SetDepthStencilSurface(zs); if (zs) zs->Release();
		d->SetViewport(&vp);
		for (int i = 0; i < 4; i++) { d->SetTexture(i, tex[i]); if (tex[i]) tex[i]->Release(); }
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();

		// The next frame's draws reproject into this frame (as RtLampShadows::Record)
		Traced = { view, D3DXVECTOR3(camera.x, camera.y, camera.z), proj._11, proj._22, cameraData->y, true };
		memcpy(TracedMap, PendingMap, sizeof(TracedMap));

		if (GpuTimer::Enabled) {
			StatActors += actors; StatCapsules += capsuleCount; StatLamps += LampCount;
			if (++StatFrames >= 240) {
				const double f = 1.0 / StatFrames;
				Logger::Log("CAPSULE SHADOWS: per frame %.1f actors (%.1f with a human skeleton), %.1f capsules, %.1f lamps in the table; capsules %s, contact %s, DropActors %s",
					StatActors * f, StatHumans * f, StatCapsules * f, StatLamps * f, capsulesOn ? "on" : "off", c[4][3] > 0.5f ? "on" : "off", DropActors() ? "on" : "off");
				StatFrames = StatActors = StatCapsules = StatHumans = StatLamps = 0;
			}
		}
	}
}
