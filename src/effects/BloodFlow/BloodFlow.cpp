// UNOFFICIAL, optional (off by default): blood that runs and dries, and pools under bodies. See BloodFlow.h.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "BloodFlow.h"
#include "BloodFluid.h"
#include "BloodSurface.h"	// UNOFFICIAL 2026-11-12 Surface
#include "../../core/CapturedNormals.h"	// UNOFFICIAL 2026-11-12 Surface (asks for TESR_AlbedoBuffer)
#include "../../core/RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "../../core/FrameClock.h"

// Game addresses (FalloutNV.exe 1.4.0.525), checked against the game's code:
//  ProcessLists, the static object at 0x11E0E80 (444 'mov ecx, 0x11E0E80' in the exe).
//    +0x08  the process array's data (Actor*[]), element i at data[i] (ProcessArray at +0x04, its data at +4: 0x877A30)
//    +0x14  first index of each process level (high = 0, middle high = 1), +0x24 one past the last (0x5DE0F0, 0x5BE5C0)
//    +0x58  GlobalTempEffectList, a BSSimpleList<NiPointer<BSTempEffect>> (ProcessLists::AddTempEffect, 0x973FD0, adds
//           every temp effect but the magic ones there)
//  BSTempEffect::kDecals, a BSSimpleList at 0x11D6A30: where most finished splats go. BSTempEffectSimpleDecal::
//    FinalizeGeometry (0x68C496): permanent ones go nowhere, ones that must fade (+0x2E) to the temp effect list above,
//    the rest to kDecals. The game does not age the ones in kDecals, so a splat's age is counted here from when it
//    is first seen (SplatsSeen).
//  BSTempEffect: +0x08 lifetime, +0x10 age (seconds).
//  BSTempEffectSimpleDecal (vtable 0x1069DDC): +0x2D valid (GetIsValid, 0x4A0350), +0x30 texture set, +0x34 origin
//    (GetOrigin, 0x4A0D30; WORLD space: BGSDecalManager::AddDecal measures the player's distance to it), +0x40 the way
//    it was thrown (world), +0x114 its turn, +0x118 width, +0x11C height (GetDecalHeight, 0x4A0D90), +0x120 depth,
//    +0x140 origin given in the hit object's own space (then ignored here). NOT world space: +0x4C and +0x58 are the
//    centre and direction moved into the hit mesh's own space (ProcessGeometry, 0x68B4C0: inverse world rotation, over
//    its world scale, as are width/height/depth). Round 1-2 used +0x4C as the centre, so every splat failed the
//    distance test and no drips were ever drawn (log 2026-10-12: splats found, no 'splat at' lines).
//  BGSTextureSet: the diffuse texture's path (a BSString) at +0x3C.
namespace {
	const UInt32 ProcessListsAddress = 0x11E0E80;
	const UInt32 SimpleDecalVtbl = 0x1069DDC;

	struct RawSplat {
		float Origin[3], Direction[3], Center[3], Normal[3];
		float Width, Height, Depth, Age, Lifetime;
		bool Local;			// the origin is in the hit object's own space
		float GameAge;		// the game's own age (Age becomes ours, counted from when it was first seen)
		void* TextureSet;
		void* Decal;
		// UNOFFICIAL 2026-10-13: how its texture lies on the wall (ProcessGeometry's texture coordinates, 0x68BA0F on), and
		// its colour. Turn +0x114 (radians round its direction), atlas quarter +0x124 (0-3, else the whole texture), mirrored
		// +0x2C (u = 1 - u), colour +0x134 (r, g, b: AddPolygon puts it in every vertex's colour, the texture is grey). The
		// hit mesh (+0x10C, its world rotation +0x68 and scale +0x98) takes the decal's mesh-space axes to the world.
		float Turn;
		int Quarter;
		bool Mirrored;
		float Tint[3];
		bool HaveMesh;
		float MeshRotation[9], MeshScale;
	};

	// UNOFFICIAL 2026-10-13: the decal's texture layout in world terms, the way BSTempEffectSimpleDecal::ProcessGeometry
	// (0x68B4C0) works out its vertices' texture coordinates: rows 0 and 1 of MakeZRotation(turn) * GetRotationZToVector(
	// mesh-space direction) are its u and v axes in the hit mesh's space; u = dot(vertex - centre, row0) / width + 0.5, v
	// likewise over the height, both halved and moved to the quarter (0: +0, 1: u + 0.5, 2: v + 0.5, 3: both; anything else
	// the whole texture), then u = 1 - u if mirrored. In the world (the mesh's rotation R, scale k): the axes are R * row,
	// the sizes k * width and k * height. Gives the item's DecalU (u's axis over the width) and DecalCode (BloodFlowBoxes.h);
	// false if anything is off (the shader then keeps its own stream starts).
	typedef float* (__cdecl* RotationZToVectorFn)(float* out, float x, float y, float z);
	const RotationZToVectorFn GetRotationZToVector = (RotationZToVectorFn)0x00C4B600;	// BSUtilities::GetRotationZToVector

	bool DecalLayout(const RawSplat& s, BloodFlowBoxes::Item& item) {
		if (!s.HaveMesh || s.Local || !(s.MeshScale > 1e-3f) || !(s.Width > 0.01f) || !(s.Height > 0.01f)) return false;
		float zv[9];
		GetRotationZToVector(zv, s.Normal[0], s.Normal[1], s.Normal[2]);
		const float c = cosf(s.Turn), sn = sinf(s.Turn);
		float row0[3], row1[3], u[3], v[3], dir[3];
		for (int i = 0; i < 3; i++) {
			row0[i] = c * zv[i] + sn * zv[3 + i];
			row1[i] = -sn * zv[i] + c * zv[3 + i];
		}
		const float* r = s.MeshRotation;
		for (int i = 0; i < 3; i++) {
			u[i] = r[i * 3] * row0[0] + r[i * 3 + 1] * row0[1] + r[i * 3 + 2] * row0[2];
			v[i] = r[i * 3] * row1[0] + r[i * 3 + 1] * row1[1] + r[i * 3 + 2] * row1[2];
		}
		const float width = s.Width * s.MeshScale, height = s.Height * s.MeshScale;
		// The shader rebuilds v as cross(normal, u): the normal it has is the item's (against the way the blood was thrown).
		const float dl = sqrtf(s.Direction[0] * s.Direction[0] + s.Direction[1] * s.Direction[1] + s.Direction[2] * s.Direction[2]);
		if (!(dl > 1e-4f)) return false;
		for (int i = 0; i < 3; i++) dir[i] = -s.Direction[i] / dl;
		const float cx = dir[1] * u[2] - dir[2] * u[1], cy = dir[2] * u[0] - dir[0] * u[2], cz = dir[0] * u[1] - dir[1] * u[0];
		const float along = cx * v[0] + cy * v[1] + cz * v[2];
		if (fabsf(along) < 0.9f) return false;	// not at right angles: something read wrong
		const float ratio = std::clamp(width / height, 0.5f, 2.0f);
		const int ratioStep = std::clamp((int)lroundf((log2f(ratio) + 1.0f) * 31.5f), 0, 63);
		item.DecalU = D3DXVECTOR3(u[0], u[1], u[2]) / width;
		item.DecalCode = std::clamp(s.Quarter, 0, 4) + (s.Mirrored ? 5 : 0) + (along < 0.0f ? 10 : 0) + 20 * ratioStep;
		return true;
	}

	struct ListNode { void* Item; ListNode* Next; };

	const UInt32 DecalListAddress = 0x11D6A30;

	// Every simple decal in one of the game's lists (up to max, after the n already in out). The game can change the
	// lists between frames only, but they are walked under SEH anyway: a bad read just ends the walk.
	int ReadSplats(UInt32 list, RawSplat* out, int n, int max) {
		__try {
			ListNode* node = (ListNode*)list;
			for (int guard = 0; node && guard < 16384 && n < max; node = node->Next, guard++) {
				UInt8* e = (UInt8*)node->Item;
				if (!e || *(UInt32*)e != SimpleDecalVtbl) continue;
				if (!*(bool*)(e + 0x2D)) continue;
				RawSplat& s = out[n];
				memcpy(s.Origin, e + 0x34, 12);
				memcpy(s.Direction, e + 0x40, 12);
				memcpy(s.Center, e + 0x4C, 12);
				memcpy(s.Normal, e + 0x58, 12);
				s.Width = *(float*)(e + 0x118);
				s.Height = *(float*)(e + 0x11C);
				s.Depth = *(float*)(e + 0x120);
				s.Local = *(bool*)(e + 0x140);
				s.Age = *(float*)(e + 0x10);
				s.Lifetime = *(float*)(e + 0x08);
				s.GameAge = s.Age;
				s.TextureSet = *(void**)(e + 0x30);
				s.Decal = e;
				s.Turn = *(float*)(e + 0x114);
				s.Quarter = *(UInt8*)(e + 0x124);
				s.Mirrored = *(UInt8*)(e + 0x2C) != 0;
				memcpy(s.Tint, e + 0x134, 12);
				UInt8* mesh = *(UInt8**)(e + 0x10C);
				s.HaveMesh = mesh != nullptr;
				if (mesh) { memcpy(s.MeshRotation, mesh + 0x68, 36); s.MeshScale = *(float*)(mesh + 0x98); }
				n++;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return n;
	}

	// The texture set's diffuse path, lower case, into out (empty if unreadable).
	void ReadTexturePath(void* textureSet, char* out, int size) {
		out[0] = 0;
		__try {
			const char* path = *(const char**)((UInt8*)textureSet + 0x3C);
			if (!path) return;
			int i = 0;
			for (; i < size - 1 && path[i]; i++) out[i] = (char)tolower((unsigned char)path[i]);
			out[i] = 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
	}

	struct RawActor {
		UInt32 RefID;
		UInt32 LifeState;
		bool Bleeds;			// not a robot
		bool HasBody;			// the fields below are set
		float BoundCenter[3], BoundRadius;
		float MinZ;
		// UNOFFICIAL 2026-10-27 (user: "weird half circles"): the ground under the body, from its bones (GroundUnderBones): its height
		// under BoundCenter and its slope (dz/dx, dz/dy).
		float GroundZ, GroundSlope[2];
		// UNOFFICIAL 2026-10-13 (user: a pool where a limb is cut off): ExtraDismemberedLimbs (0x5F) +0x0C, one bit per body
		// part (ExtraDismemberedLimbs::Dismember 0x430410), and where each cut part's joint is on the body (its node's origin:
		// the elbow for a forearm, the neck for a head); PartFound false: not a skeleton with those names (most creatures).
		UInt16 Dismembered;
		float PartPos[16][3];
		bool PartFound[16];
	};

	// The node whose origin is where part k (BGSBodyPart types: 1-2 head, 3-4 left arm, 5-6 right arm, 7-9 left leg, 10-12
	// right leg, 13 brain) was cut off, on a human skeleton. 0 (torso), 14 (weapon) and 15: none.
	const char* const PartNode[16] = { nullptr, "Bip01 Head", "Bip01 Head", "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 R UpperArm",
		"Bip01 R Forearm", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot", "Bip01 Head",
		nullptr, nullptr };

	// UNOFFICIAL 2026-10-27 (user: "we still get the weird half circles with bloodflow"): the ground under a body, from the points of
	// its skeleton (count of them, xyz; bone[i]: a bone, not a mesh's origin), as a plane through `centre`: ground[0] its height there,
	// ground[1..2] its slope (dz/dx, dz/dy).
	// A pool's height used to be the lowest bone's less 1.5 units. On a slope the lowest bone is the body's downhill end, so the
	// ground under the body's middle lies well above that (25 degrees, a body 70 units long: about 15 units), and every test that
	// keeps a pool on its ground cut it there: BloodPS showed only the downhill half (a half disc, cut straight across near the
	// body), and BloodFluid's pool, spread on a level floor that far under the slope, stayed hidden under the uphill ground. Now: the
	// slope of the plane through the body's lower bones (a least-squares plane through all bones, then again through those on or
	// below it, so a raised head or chest does not tilt it; a weak pull toward level, so a body lying along one line says nothing
	// about the slope across it; at most 0.6, about 31 degrees), moved down to 1.5 units under the lowest bone measured along it.
	// On level ground that is the old height; the plane never rises above any point (the meshes' origins count there, as for the
	// lowest node before, not in the slope).
	void GroundUnderBones(const float (*bones)[3], const bool* bone, int count, const float* centre, float minZ, float* ground) {
		ground[0] = minZ - 1.5f; ground[1] = ground[2] = 0.0f;
		int boneCount = 0;
		for (int i = 0; i < count; i++) boneCount += bone[i] ? 1 : 0;
		if (boneCount < 6) return;
		float slope[2] = { 0.0f, 0.0f };
		bool use[160];
		for (int i = 0; i < count; i++) use[i] = bone[i];
		for (int pass = 0; pass < 2; pass++) {
			double n = 0.0, mx = 0.0, my = 0.0, mz = 0.0;
			for (int i = 0; i < count; i++) if (use[i]) { n += 1.0; mx += bones[i][0] - centre[0]; my += bones[i][1] - centre[1]; mz += bones[i][2]; }
			if (n < 4.0) break;
			mx /= n; my /= n; mz /= n;
			double sxx = 0.0, sxy = 0.0, syy = 0.0, sxz = 0.0, syz = 0.0;
			for (int i = 0; i < count; i++) {
				if (!use[i]) continue;
				const double x = bones[i][0] - centre[0] - mx, y = bones[i][1] - centre[1] - my, z = bones[i][2] - mz;
				sxx += x * x; sxy += x * y; syy += y * y; sxz += x * z; syz += y * z;
			}
			// (The pull toward level: as if every bone were also 10 units either way at the mean height.)
			const double ridge = n * 100.0;
			sxx += ridge; syy += ridge;
			const double det = sxx * syy - sxy * sxy;
			if (!(det > 1e-6)) break;
			slope[0] = (float)((sxz * syy - syz * sxy) / det);
			slope[1] = (float)((syz * sxx - sxz * sxy) / det);
			const float steep = sqrtf(slope[0] * slope[0] + slope[1] * slope[1]);
			if (steep > 0.6f) { slope[0] *= 0.6f / steep; slope[1] *= 0.6f / steep; }
			if (pass > 0) break;
			// The second fit: only the bones on or below the first plane.
			const float level = (float)mz - slope[0] * (float)mx - slope[1] * (float)my;	// the first plane's height at the centre
			for (int i = 0; i < count; i++)
				use[i] = bone[i] && bones[i][2] - (level + slope[0] * (bones[i][0] - centre[0]) + slope[1] * (bones[i][1] - centre[1])) <= 0.0f;
		}
		float lowest = 1e30f;
		for (int i = 0; i < count; i++)
			lowest = min(lowest, bones[i][2] - slope[0] * (bones[i][0] - centre[0]) - slope[1] * (bones[i][1] - centre[1]));
		if (!(lowest < 1e29f)) return;
		ground[0] = lowest - 1.5f; ground[1] = slope[0]; ground[2] = slope[1];
	}

	// The world position of every node under root (up to 256 nodes): the lowest z, the middle of the skeleton (a spine
	// bone if there is one, else the average of the nodes below the root) and how many were seen. Nodes further than
	// `reach` from `bound` are left out: some sit at the world's origin (not placed yet), which once put a pool at z -1.5.
	// (2026-10-27: and the ground under it, from its bones: GroundUnderBones.)
	int WalkNodes(NiAVObject* root, float& minZ, float* centre, float* ground, const float* bound, float reach, RawActor* parts = nullptr) {
		NiAVObject* stack[64];
		int top = 0, seen = 0, nodes = 0;
		float sum[3] = {}, spine[3] = {};
		bool haveSpine = false;
		float points[160][3];
		bool bone[160];
		int pointCount = 0;
		stack[top++] = root;
		while (top && seen < 256) {
			NiAVObject* object = stack[--top];
			if (!object) continue;
			seen++;
			const NiPoint3& at = object->m_worldTransform.pos;
			const float dx = at.x - bound[0], dy = at.y - bound[1], dz = at.z - bound[2];
			const bool inReach = dx * dx + dy * dy + dz * dz <= reach * reach;
			if (inReach) minZ = min(minZ, at.z);
			NiNode* node = object->IsNiNode();
			if (inReach && pointCount < 160) {
				points[pointCount][0] = at.x; points[pointCount][1] = at.y; points[pointCount][2] = at.z;
				bone[pointCount++] = node && object != root;
			}
			if (!node) continue;
			if (object != root && inReach) {
				sum[0] += at.x; sum[1] += at.y; sum[2] += at.z;
				nodes++;
				const char* name = object->m_pcName;
				if (!haveSpine && name && strstr(name, "Spine")) { spine[0] = at.x; spine[1] = at.y; spine[2] = at.z; haveSpine = true; }
				if (parts && name && name[0] == 'B') {
					for (int k = 0; k < 16; k++) {
						if (!PartNode[k] || parts->PartFound[k] || !(parts->Dismembered & (1 << k)) || strcmp(name, PartNode[k])) continue;
						parts->PartPos[k][0] = at.x; parts->PartPos[k][1] = at.y; parts->PartPos[k][2] = at.z;
						parts->PartFound[k] = true;
					}
				}
			}
			for (UInt32 i = 0; i < node->m_children.end && top < 64; i++)
				if (node->m_children.data[i]) stack[top++] = node->m_children.data[i];
		}
		if (haveSpine) { centre[0] = spine[0]; centre[1] = spine[1]; centre[2] = spine[2]; }
		else if (nodes) { centre[0] = sum[0] / nodes; centre[1] = sum[1] / nodes; centre[2] = sum[2] / nodes; }
		GroundUnderBones(points, bone, pointCount, centre, minZ, ground);
		return nodes ? seen : 0;
	}

	// UNOFFICIAL 2026-10-16 (cheaper): a body's skeleton walk (up to 256 nodes) ran for every dead body at every call, twice a
	// frame. It is kept per body and redone only when the body moves (its bound by more than half a unit), loses a limb, or every
	// half second (ragdolls settle). The walk's results are exactly what a new walk at that moment gave.
	struct WalkCache { float Center[3], Radius; UInt16 Dismembered; double Time; RawActor Result; };
	bool BloodFlowCheap = true;	// Settings.CheapGather (read in UpdateSettings)
	std::unordered_map<UInt32, WalkCache> Walks;
	double WalksPruned = 0.0;

	// The actors in the high and middle-high process levels: alive ones (refID and state only, for telling a fresh death
	// from a body that was already there) and dead or dying ones with their body's position.
	int ReadActors(RawActor* out, int max) {
		const double now = TheFrameRateManager->Time;
		if (now - WalksPruned > 10.0) {
			WalksPruned = now;
			for (auto it = Walks.begin(); it != Walks.end();) { if (now - it->second.Time > 10.0) it = Walks.erase(it); else ++it; }
		}
		int n = 0;
		__try {
			UInt8* lists = (UInt8*)ProcessListsAddress;
			Actor** data = *(Actor***)(lists + 0x08);
			if (!data) return 0;
			for (int level = 0; level < 2; level++) {
				UInt32 begin = *(UInt32*)(lists + 0x14 + level * 4);
				UInt32 end = *(UInt32*)(lists + 0x24 + level * 4);
				if (end < begin || end - begin > 4096) continue;
				for (UInt32 i = begin; i < end && n < max; i++) {
					Actor* actor = data[i];
					if (!actor || (TESObjectREFR*)actor == (TESObjectREFR*)Player) continue;
					RawActor& a = out[n];
					a.RefID = actor->refID;
					a.LifeState = actor->lifeState;
					a.HasBody = false;
					a.Bleeds = true;
					a.Dismembered = 0;
					memset(a.PartFound, 0, sizeof(a.PartFound));
					TESForm* base = actor->baseForm;
					if (base && base->formType == TESForm::kFormType_Creature && *((UInt8*)base + 0x12C) == 6) a.Bleeds = false;  // robot
					if ((actor->flags & 0x800) != 0) a.Bleeds = false;   // disabled
					if ((a.LifeState == 1 || a.LifeState == 2) && a.Bleeds) {
						NiNode* root = actor->GetNode();
						if (root && root->m_kWorldBound) {
							const NiBound* bound = root->m_kWorldBound;
							a.BoundCenter[0] = bound->Center.x;
							a.BoundCenter[1] = bound->Center.y;
							a.BoundCenter[2] = bound->Center.z;
							a.BoundRadius = bound->Radius;
							a.MinZ = 1e30f;
							const float boundAt[3] = { bound->Center.x, bound->Center.y, bound->Center.z };
							if (BSExtraData* cut = actor->extraDataList.GetExtraData(BSExtraData::ExtraDataType::kExtraData_DismemberedLimbs))
								a.Dismembered = *(UInt16*)((UInt8*)cut + 0x0C);
							WalkCache* cached = nullptr;
							auto known = Walks.find(a.RefID);
							if (known != Walks.end()) {
								WalkCache& w = known->second;
								const float dx = w.Center[0] - boundAt[0], dy = w.Center[1] - boundAt[1], dz = w.Center[2] - boundAt[2];
								if (BloodFlowCheap && w.Dismembered == a.Dismembered && dx * dx + dy * dy + dz * dz < 0.25f && fabsf(w.Radius - bound->Radius) < 0.5f &&
									now - w.Time < 0.5) cached = &w;
							}
							if (cached) {
								const UInt32 refID = a.RefID, lifeState = a.LifeState;
								a = cached->Result;
								a.RefID = refID; a.LifeState = lifeState;
							}
							else {
								float ground[3];
								a.HasBody = WalkNodes(root, a.MinZ, a.BoundCenter, ground, boundAt, bound->Radius * 1.5f + 16.0f, a.Dismembered ? &a : nullptr) > 4 && a.BoundRadius > 1.0f &&
									a.MinZ < 1e29f && a.MinZ > bound->Center.z - bound->Radius * 1.5f - 16.0f;
								a.GroundZ = ground[0]; a.GroundSlope[0] = ground[1]; a.GroundSlope[1] = ground[2];
								WalkCache& w = Walks[a.RefID];
								w.Center[0] = boundAt[0]; w.Center[1] = boundAt[1]; w.Center[2] = boundAt[2];
								w.Radius = bound->Radius; w.Dismembered = a.Dismembered; w.Time = now; w.Result = a;
							}
						}
					}
					n++;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return n;
	}

	float Hash01(UInt32 x) {
		x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
		return (float)(x & 0xFFFFFF) / 16777216.0f;
	}

	UInt32 HashFloats(const float* v, int count) {
		UInt32 h = 2166136261u;
		for (int i = 0; i < count; i++) {
			UInt32 bits;
			memcpy(&bits, &v[i], 4);
			h = (h ^ bits) * 16777619u;
		}
		return h;
	}

	float SrgbToLinear(float c) {
		c = std::clamp(c, 0.0f, 1.0f);
		return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
	}

	using BloodFlowBoxes::Length;

	// What we know of each body: when it was last seen alive, when it died, and where its pool is.
	struct Body {
		double AliveAt = -1.0;		// Clock when last seen alive (-1: never)
		double DeadAt = -1.0;		// Clock when first seen dead or dying
		double SeenAt = 0.0;		// last seen at all
		bool Locked = false;		// the pool's place is fixed (the body has come to rest)
		double LockAt = 0.0;		// Clock when the pool started
		bool EverLocked = false;	// UNOFFICIAL 2026-10-20: a pool was started once (a push moves it, it does not start over)
		double StillSince = -1.0;
		D3DXVECTOR3 Last = D3DXVECTOR3(0, 0, 0);	// body centre last frame
		D3DXVECTOR3 Pool = D3DXVECTOR3(0, 0, 0);	// the pool's centre on the ground
		float PoolSlope[2] = { 0.0f, 0.0f };		// UNOFFICIAL 2026-10-27: the ground's slope there (dz/dx, dz/dy; GroundUnderBones)
		float MaxRadius = 0.0f;
		float Tint = 0.0f;			// UNOFFICIAL 2026-10-25: its own blood's colour (Item::Tint packing), from a splat next to it; 0: none yet
		// UNOFFICIAL 2026-10-13: a pool of its own under each cut-off part (GibPools), from when the cut is first seen; its place
		// follows the stump until the body comes to rest.
		UInt16 GibMask = 0;
		double GibAt[16] = {};
		D3DXVECTOR3 GibPool[16] = {};
	};
	std::unordered_map<UInt32, Body> Bodies;
	std::unordered_map<void*, bool> BloodTextureSets;	// texture set -> is it blood
	int LoggedTextures = 0, LoggedSplats = 0, LoggedPools = 0, LoggedLists = 0, LoggedNearest = 0;
	// UNOFFICIAL 2026-10-13: the last blood decal colour seen (pools, which have no decal, take it), and the layout log.
	float LastTint[3] = { 0.0f, 0.0f, 0.0f };
	bool HaveTint = false;
	int LoggedLayouts = 0;
	int LoggedMerges = 0;
	int LoggedGibs = 0;
	int ItemLogTick = 0;

	// When each splat was first seen (the game does not age most of them). Keyed by the decal, checked by its origin, so a
	// new splat made where an old one was freed starts afresh.
	struct SplatSeen { UInt32 Key; double FirstSeen; double LastSeen; };
	std::unordered_map<void*, SplatSeen> SplatsSeen;
	double LastSplatGather = -1e9;
}

// UNOFFICIAL 2026-10-13: for HandBlood (blood on the first-person weapon and gloves, Render.cpp): the game's blood splats that
// appeared since the last call within `reach` of `eye`, whether or not BloodFlow is on. Returns how close the nearest one is
// (reach and above: none). Splats there on the first call, or after a pause of over a second (a load), count as seen. Also the
// last blood decal colour seen (false: none yet).
float BloodFlowNewBloodNear(const D3DXVECTOR3& eye, float reach) {
	static RawSplat raw[1024];
	static std::unordered_map<void*, UInt32> seen;
	static ULONGLONG lastCall = 0;
	const ULONGLONG now = GetTickCount64();
	const bool fresh = lastCall == 0 || now - lastCall > 1000;
	lastCall = now;
	const int fromTemp = ReadSplats(ProcessListsAddress + 0x58, raw, 0, 1024);
	const int total = ReadSplats(DecalListAddress, raw, fromTemp, 1024);
	float nearest = reach;
	std::unordered_map<void*, UInt32> next;
	next.reserve(total);
	for (int i = 0; i < total; i++) {
		const RawSplat& s = raw[i];
		const UInt32 key = HashFloats(s.Origin, 3);
		next[s.Decal] = key;
		auto old = seen.find(s.Decal);
		if (fresh || (old != seen.end() && old->second == key) || !s.TextureSet || s.Local) continue;
		auto found = BloodTextureSets.find(s.TextureSet);
		if (found == BloodTextureSets.end()) {
			char path[260];
			ReadTexturePath(s.TextureSet, path, sizeof(path));
			found = BloodTextureSets.emplace(s.TextureSet, strstr(path, "blood") != nullptr).first;
		}
		if (!found->second) continue;
		if (s.Tint[0] + s.Tint[1] + s.Tint[2] > 0.0f) { for (int c = 0; c < 3; c++) LastTint[c] = s.Tint[c]; HaveTint = true; }
		const float distance = Length(D3DXVECTOR3(s.Origin[0], s.Origin[1], s.Origin[2]) - eye);
		if (distance < nearest) nearest = distance;
	}
	seen.swap(next);
	return nearest;
}

bool BloodFlowLastTint(float out[3]) {
	if (!HaveTint) return false;
	for (int c = 0; c < 3; c++) out[c] = LastTint[c];
	return true;
}

// UNOFFICIAL 2026-10-13 (perf): TESR_BloodFlowShape: xyz the camera-centred space's eye (the inverse view's translation); w: 1
// the vertex shader cuts the boxes to the blood's bounds (BloodFlow.fx.hlsl, CutBox). Kept here, not in BloodFlow.h's
// constants (a widely included header: changing it needs a clean build).
static D3DXVECTOR4 BloodFlowShape(0.0f, 0.0f, 0.0f, 0.0f);
// UNOFFICIAL 2026-10-16 (GroundFit, user: blood never clipping with the ground): TESR_BloodFlowGround (BloodFlow.fx.hlsl): x 1 soft
// cuts that follow the surface's slope, y the fades' least width in pixels, w 1 (test only) pools act as if the ground round them
// was never found. GroundFit 0: the hard cuts as before. Kept out of BloodFlow.h for the same reason as BloodFlowShape.
static D3DXVECTOR4 BloodFlowGround(1.0f, 3.0f, 0.0f, 0.0f);
static bool BloodFlowGroundFit = true;
static bool BloodFlowGroundUnseen = false;
// UNOFFICIAL 2026-10-26 BloodFluid (BloodFluid.h): DripLength and SplatDrips as set; while the fluid replaces the painted streams
// (ReplaceStreams) both are 0 in Settings (UpdateConstants), and back as set when it does not.
static float BloodFlowBaseDripLength = 40.0f;
static float BloodFlowBaseSplatDrips = 1.0f;
// UNOFFICIAL 2026-10-27 (user, playtest of the fork build: pooled and flowing blood "looks kinda grey, almost reflective"):
// TESR_BloodFlowLook, the wet look of every blood pass (BloodPS, the splat shapes, the wet beads, BloodFluidDraw.frag): x the
// sheen (the sky or room mirrored at low angles) x what it was, y the most of it Fresnel may mirror, z how much of the blood's
// own hue the sheen and the glints of drops take (0 white), w the darkest a matched colour may be (linear, its strongest
// channel). The matched colour (the user's splatter texture's grey 0.12 x the decal colour 0.667 0 0.016: about 0.007 linear
// red) was so dark that the uncoloured sky mirrored on top (up to a third of it at the low angles a pool is seen at) was several
// times brighter than the blood's own red. OldLook (test only): (1, 1, 0, 0) = the look before, exactly.
static D3DXVECTOR4 BloodFlowLook(1.0f, 1.0f, 0.0f, 0.0f);

// UNOFFICIAL 2026-11-12 Surface ([Shaders.BloodFlow.Surface], off by default; BloodSurface.h, Includes/BloodSurface.hlsl; user:
// the pools are perfect circles that float, the blood clips into things and does not follow the game's light). Blood as a thin
// layer of liquid simulated on the ground it lies on, in a patch of the world per splat and per body, with plain D3D9 passes.
// Kept here, not in BloodFlow.h (a widely included header). The state atlas (fp16 rgba, two copies, 16 MB each) is made the
// first time the mode has a patch; TESR_BloodSurface binds the copy a pass reads (SurfaceRead).
static BloodSurface::Settings SurfaceSettings;
static BloodSurface::Atlas SurfaceAtlas;
static IDirect3DTexture9* SurfaceTexture[2] = {};
static IDirect3DSurface9* SurfaceTarget[2] = {};
static IDirect3DTexture9* SurfaceRead = nullptr;
static bool SurfaceFailed = false;	// the card cannot draw into or filter fp16 rgba: the mode stays off (logged)
// A frozen patch's cell is read back once to find where its blood lies (BloodSurface::Atlas::Measure): a target of a cell's size
// it is copied to, and the system-memory surface that is read into. Made on first use; failing them only leaves the boxes whole.
static IDirect3DSurface9* SurfaceMeasureTarget = nullptr;
static IDirect3DSurface9* SurfaceMeasureRead = nullptr;
static bool SurfaceMeasureFailed = false;
static double SurfaceMeasuredAt = -1.0;
static bool SurfaceTestPour = false;	// TEST: a pool bleeding ahead of the player (AddTestSplats)
static double SurfaceLastTime = -1.0;
// TESR_BloodSurfaceSize, TESR_BloodSurfaceSim, TESR_BloodSurfaceFlow, TESR_BloodSurfaceLook (Includes/BloodSurface.hlsl).
static D3DXVECTOR4 SurfaceSizeConstants(0.0f, 0.0f, 0.0f, 0.0f), SurfaceSimConstants(0.0f, 0.0f, 0.0f, 0.0f), SurfaceFlowConstants(0.0f, 0.0f, 0.0f, 0.0f),
	SurfaceLookConstants(0.0f, 0.0f, 0.0f, 0.0f);
static VolumetricSmokeQuads::Vertex SurfaceSimVertices[BloodSurface::Cells * 6], SurfaceBoxVertices[BloodSurface::Cells * 36];

void BloodFlowEffect::RegisterConstants() {
	TheShaderManager->RegisterConstant("TESR_BloodSurfaceSize", &SurfaceSizeConstants);	// UNOFFICIAL 2026-11-12 Surface
	TheShaderManager->RegisterConstant("TESR_BloodSurfaceSim", &SurfaceSimConstants);
	TheShaderManager->RegisterConstant("TESR_BloodSurfaceFlow", &SurfaceFlowConstants);
	TheShaderManager->RegisterConstant("TESR_BloodSurfaceLook", &SurfaceLookConstants);
	TheShaderManager->RegisterConstant("TESR_BloodFlowFresh", &Constants.Fresh);
	TheShaderManager->RegisterConstant("TESR_BloodFlowMatch", &Constants.Match);
	TheShaderManager->RegisterConstant("TESR_BloodFlowDryRatio", &Constants.DryRatio);
	TheShaderManager->RegisterConstant("TESR_BloodFlowBeads", &Constants.Beads);
	TheShaderManager->RegisterConstant("TESR_BloodFlowSplat", &Constants.Splat);
	TheShaderManager->RegisterConstant("TESR_BloodFlowShape", &BloodFlowShape);
	TheShaderManager->RegisterConstant("TESR_BloodFlowGround", &BloodFlowGround);
	TheShaderManager->RegisterConstant("TESR_BloodFlowLook", &BloodFlowLook);	// UNOFFICIAL 2026-10-27 (also read by BloodFluid.cpp)
	TheShaderManager->RegisterConstant("TESR_BloodFlowDry", &Constants.Dry);
	TheShaderManager->RegisterConstant("TESR_BloodFlowInfo", &Constants.Info);
	TheShaderManager->RegisterConstant("TESR_BloodFlowPool", &Constants.Pool);
	TheShaderManager->RegisterConstant("TESR_BloodFlowProbe", &Constants.Probe);
	TheShaderManager->RegisterConstant("TESR_BloodFlowFlow", &Constants.Flow);
	TheShaderManager->RegisterConstant("TESR_BloodFlowMask", &MaskConstants);	// UNOFFICIAL 2026-10-22 (SplatMask)
	TheShaderManager->RegisterConstant("TESR_BloodFlowMaskSize", &MaskSizeConstants);
}

void BloodFlowEffect::RegisterTextures() {
	// The path texture, twice (16 x 576 texels of four floats each, 147 KB apiece): positions in game units need floats.
	TheTextureManager->InitTexture("NVR_BloodFlowPaths0", &PathTexture[0], &PathSurface[0], BloodFlowBoxes::PathColumns, PathRows, D3DFMT_A32B32G32R32F);
	TheTextureManager->InitTexture("NVR_BloodFlowPaths1", &PathTexture[1], &PathSurface[1], BloodFlowBoxes::PathColumns, PathRows, D3DFMT_A32B32G32R32F);
	PathRead = PathTexture[0];
	TheTextureManager->RegisterTexture("TESR_BloodFlowPaths", (IDirect3DBaseTexture9**)&PathRead);
	TheTextureManager->InitTexture("TESR_BloodFlowBounds", &BoundsTexture, &BoundsSurface, 2, ItemMax, D3DFMT_A32B32G32R32F);
	// UNOFFICIAL 2026-10-22 (SplatMask): registered now, made in Render the first time the mask is used (none while it is off).
	TheTextureManager->RegisterTexture("TESR_BloodSplatMask", (IDirect3DBaseTexture9**)&MaskTexture);
	// UNOFFICIAL 2026-11-12 Surface: registered now, made the first time the mode has a patch (none while it is off).
	TheTextureManager->RegisterTexture("TESR_BloodSurface", (IDirect3DBaseTexture9**)&SurfaceRead);
}

int BloodFlowEffect::SlotFor(UInt64 key) {
	for (int i = 0; i < ItemMax; i++)
		if (Slots[i].Used && Slots[i].Key == key) { Slots[i].SeenFrame = Frame; return i; }
	// A slot nothing has used, or whose item was not gathered last frame (it starts afresh if it comes back).
	for (int i = 0; i < ItemMax; i++) {
		if (!Slots[i].Used || Frame - Slots[i].SeenFrame > 1) {
			Slots[i] = SlotState();
			Slots[i].Key = key;
			Slots[i].Used = true;
			Slots[i].SeenFrame = Frame;
			return i;
		}
	}
	return -1;
}

void BloodFlowEffect::UpdateSettings() {
	const char* section = "Shaders.BloodFlow.Main";
	Settings.Splats = TheSettingManager->GetSettingI(section, "Splats") != 0;
	Settings.CheapGather = TheSettingManager->GetSettingI(section, "CheapGather") != 0;
	BloodFlowCheap = Settings.CheapGather;
	BloodFlowGroundFit = TheSettingManager->GetSettingI(section, "GroundFit") != 0;
	BloodFlowGroundUnseen = TheSettingManager->GetSettingI(section, "GroundTestUnseen") != 0;
	Settings.Pools = TheSettingManager->GetSettingI(section, "Pools") != 0;
	Settings.DripLength = std::clamp(TheSettingManager->GetSettingF(section, "DripLength"), 0.0f, 200.0f);
	// UNOFFICIAL 2026-10-13 rewrite: FlowSpeed (was DripSpeed), Viscosity, BloodAmount.
	Settings.FlowSpeed = std::clamp(TheSettingManager->GetSettingF(section, "FlowSpeed"), 0.05f, 10.0f);
	Settings.Viscosity = std::clamp(TheSettingManager->GetSettingF(section, "Viscosity"), 0.0f, 3.0f);
	Settings.BloodAmount = std::clamp(TheSettingManager->GetSettingF(section, "BloodAmount"), 0.0f, 1.0f);
	Settings.DryTime = std::clamp(TheSettingManager->GetSettingF(section, "DryTime"), 1.0f, 3600.0f);
	Settings.PoolSize = std::clamp(TheSettingManager->GetSettingF(section, "PoolSize"), 0.1f, 4.0f);
	Settings.PoolSpreadTime = std::clamp(TheSettingManager->GetSettingF(section, "PoolSpreadTime"), 1.0f, 600.0f);
	Settings.PoolDryTime = std::clamp(TheSettingManager->GetSettingF(section, "PoolDryTime"), 1.0f, 7200.0f);
	Settings.Gloss = std::clamp(TheSettingManager->GetSettingF(section, "Gloss"), 0.0f, 4.0f);
	Settings.FreshColor[0] = TheSettingManager->GetSettingF(section, "FreshColorR");
	Settings.FreshColor[1] = TheSettingManager->GetSettingF(section, "FreshColorG");
	Settings.FreshColor[2] = TheSettingManager->GetSettingF(section, "FreshColorB");
	Settings.DryColor[0] = TheSettingManager->GetSettingF(section, "DryColorR");
	Settings.DryColor[1] = TheSettingManager->GetSettingF(section, "DryColorG");
	Settings.DryColor[2] = TheSettingManager->GetSettingF(section, "DryColorB");
	Settings.MaxFall = std::clamp(TheSettingManager->GetSettingF(section, "MaxFall"), 0.0f, 1000.0f);
	Settings.MaxDistance = std::clamp(TheSettingManager->GetSettingF(section, "MaxDistance"), 200.0f, 20000.0f);
	Settings.MaxSplats = std::clamp(TheSettingManager->GetSettingI(section, "MaxSplats"), 0, ItemMax);
	Settings.MaxPools = std::clamp(TheSettingManager->GetSettingI(section, "MaxPools"), 0, 32);
	Settings.DebugView = TheSettingManager->GetSettingI(section, "DebugView");
	// UNOFFICIAL 2026-10-13: pools and puddles take their shape from the game's blood splatter decals (BloodFlow.fx.hlsl,
	// TESR_BloodSplatter) when it is a loose file (a texture replacer): NVR does not read the game's archives, and without it
	// the shader keeps its drawn shapes.
	Settings.MatchGameColor = TheSettingManager->GetSettingI(section, "MatchGameColor") != 0;
	Settings.MatchShade = std::clamp(TheSettingManager->GetSettingF(section, "MatchShade"), 0.01f, 1.0f);
	Settings.MatchBrightness = std::clamp(TheSettingManager->GetSettingF(section, "MatchBrightness"), 0.1f, 8.0f);
	Settings.StreamWidth = std::clamp(TheSettingManager->GetSettingF(section, "StreamWidth"), 0.1f, 4.0f);
	Settings.Beads = std::clamp(TheSettingManager->GetSettingF(section, "Beads"), 0.0f, 3.0f);
	Settings.GibPools = std::clamp(TheSettingManager->GetSettingF(section, "GibPools"), 0.0f, 3.0f);
	Settings.GibHold = std::clamp(TheSettingManager->GetSettingF(section, "GibHold"), 0.0f, 5.0f);
	Settings.Merge = std::clamp(TheSettingManager->GetSettingF(section, "Merge"), 0.0f, 2.0f);
	Settings.MergeCount = std::clamp(TheSettingManager->GetSettingI(section, "MergeCount"), 2, 64);
	Settings.BeadSize = std::clamp(TheSettingManager->GetSettingF(section, "BeadSize"), 1.0f, 200.0f);
	Settings.Splat = std::clamp(TheSettingManager->GetSettingF(section, "Splat"), 0.0f, 1.0f);
	Settings.SplatReach = std::clamp(TheSettingManager->GetSettingF(section, "SplatReach"), 0.2f, 3.0f);
	Settings.SplatSpray = std::clamp(TheSettingManager->GetSettingF(section, "SplatSpray"), 0.0f, 2.0f);
	Settings.SplatDrips = std::clamp(TheSettingManager->GetSettingF(section, "SplatDrips"), 0.0f, 3.0f);
	Settings.SplatFast = TheSettingManager->GetSettingI(section, "SplatFast") != 0;
	Settings.SplatMask = TheSettingManager->GetSettingI(section, "SplatMask") != 0;
	Settings.TestSplats = std::clamp(TheSettingManager->GetSettingI(section, "TestSplats"), 0, 48);
	// (The cells hold the shape these settings make: worked out again when they change.)
	if (MaskShape[0] != Settings.SplatReach || MaskShape[1] != Settings.SplatSpray || MaskShape[2] != Settings.SplatDrips) {
		Mask.Reset();
		MaskShape[0] = Settings.SplatReach; MaskShape[1] = Settings.SplatSpray; MaskShape[2] = Settings.SplatDrips;
	}
	BloodFlowBaseDripLength = Settings.DripLength;
	BloodFlowBaseSplatDrips = Settings.SplatDrips;
	// UNOFFICIAL 2026-10-27: the wet look (TESR_BloodFlowLook, above). The Fresnel cap is fixed: past a quarter, a pool seen from a
	// few metres away mirrored more sky than it showed blood.
	if (TheSettingManager->GetSettingI(section, "OldLook") != 0) BloodFlowLook = D3DXVECTOR4(1.0f, 1.0f, 0.0f, 0.0f);
	else BloodFlowLook = D3DXVECTOR4(std::clamp(TheSettingManager->GetSettingF(section, "Sheen"), 0.0f, 2.0f), 0.25f,
		std::clamp(TheSettingManager->GetSettingF(section, "SheenTint"), 0.0f, 1.0f), SrgbToLinear(std::clamp(TheSettingManager->GetSettingF(section, "MatchFloor"), 0.0f, 0.6f)));
	TheBloodFluid().ReadSettings();	// UNOFFICIAL 2026-10-26 BloodFluid: [Shaders.BloodFlow.Fluid]
	{
		// UNOFFICIAL 2026-11-12 Surface: [Shaders.BloodFlow.Surface]. Turned off, its patches go (BloodFlow's own passes are as before).
		const char* surface = "Shaders.BloodFlow.Surface";
		BloodSurface::Settings& s = SurfaceSettings;
		const bool was = s.Enabled;
		s.Enabled = TheSettingManager->GetSettingI(surface, "Enabled") != 0;
		s.Splats = TheSettingManager->GetSettingI(surface, "Splats") != 0;
		s.Pools = TheSettingManager->GetSettingI(surface, "Pools") != 0;
		s.SplatAmount = std::clamp(TheSettingManager->GetSettingF(surface, "SplatAmount"), 0.2f, 3.0f);
		s.PoolAmount = std::clamp(TheSettingManager->GetSettingF(surface, "PoolAmount"), 0.2f, 3.0f);
		s.BleedTime = std::clamp(TheSettingManager->GetSettingF(surface, "BleedTime"), 1.0f, 30.0f);
		s.FlowTime = std::clamp(TheSettingManager->GetSettingF(surface, "FlowTime"), 5.0f, 90.0f);
		s.Viscosity = std::clamp(TheSettingManager->GetSettingF(surface, "Viscosity"), 0.3f, 5.0f);
		s.Roughness = std::clamp(TheSettingManager->GetSettingF(surface, "Roughness"), 0.0f, 0.6f);
		s.Lifetime = std::clamp(TheSettingManager->GetSettingF(surface, "Lifetime"), 30.0f, 3600.0f);
		s.MaxPatches = std::clamp(TheSettingManager->GetSettingI(surface, "MaxPatches"), 1, (int)BloodSurface::Cells);
		s.UseAlbedo = TheSettingManager->GetSettingI(surface, "UseAlbedo") != 0;
		s.DebugView = TheSettingManager->GetSettingI(surface, "DebugView");
		SurfaceTestPour = TheSettingManager->GetSettingI(surface, "TestPour") != 0;
		if (was != s.Enabled) {
			SurfaceAtlas.Clear();
			Logger::Log("BloodFlow: Surface %s%s", s.Enabled ? "on: pools and fresh splats are simulated on the ground they lie on" : "off", SurfaceFailed ? " (cannot run on this card: see above)" : "");
		}
	}
	Settings.SplatterAtlas = GetFileAttributesA("Data\\Textures\\decals\\fxbloodsplatterdecals01.dds") != INVALID_FILE_ATTRIBUTES;
	// UNOFFICIAL fixes-b #53: this runs at every settings refresh (every cell change): log only the first time and when the answer changes
	static int loggedAtlas = -1;
	if (loggedAtlas != (Settings.SplatterAtlas ? 1 : 0)) {
		loggedAtlas = Settings.SplatterAtlas ? 1 : 0;
		Logger::Log("BloodFlow: splatter decals for pools and puddles: %s", Settings.SplatterAtlas ? "Textures\\decals\\fxbloodsplatterdecals01.dds" : "none (no loose file), drawn shapes");
	}
}

void BloodFlowEffect::GatherSplats() {
	static RawSplat raw[1024];
	const int fromTemp = ReadSplats(ProcessListsAddress + 0x58, raw, 0, 1024);
	int total = ReadSplats(DecalListAddress, raw, fromTemp, 1024);
	if (LoggedLists < 10 && total > 0) {
		LoggedLists++;
		Logger::Log("BloodFlow: %d splats in the game's lists (%d temp effects, %d decals)", total, fromTemp, total - fromTemp);
	}
	// UNOFFICIAL 2026-10-16 (cheaper): only blood decals go on (the game's lists hold every decal: bullet holes, scorch marks...);
	// they used to be aged and tracked before the blood test.
	int bloodTotal = 0;
	for (int i = 0; i < total; i++) {
		RawSplat& s = raw[i];
		if (!s.TextureSet) continue;
		auto found = BloodTextureSets.find(s.TextureSet);
		if (found == BloodTextureSets.end()) {
			char path[260];
			ReadTexturePath(s.TextureSet, path, sizeof(path));
			const bool blood = strstr(path, "blood") != nullptr;
			found = BloodTextureSets.emplace(s.TextureSet, blood).first;
			if (LoggedTextures < 24) {
				LoggedTextures++;
				Logger::Log("BloodFlow: decal texture '%s' -> %s", path, blood ? "blood" : "not blood");
			}
		}
		if (found->second) { if (bloodTotal != i) raw[bloodTotal] = s; bloodTotal++; }
	}
	if (Settings.CheapGather) total = bloodTotal;	// (off: every decal is aged and tracked, as before)
	// Ages: splats seen for the first time now are new, unless the effect has just started (or come back after a pause or
	// a load) or many appear at once (a cell or save loading): those are old, already dried.
	const bool resumed = Clock - LastSplatGather > 1.0;
	LastSplatGather = Clock;
	int appeared = 0;
	for (int i = 0; i < total; i++) {
		auto seen = SplatsSeen.find(raw[i].Decal);
		if (seen == SplatsSeen.end() || seen->second.Key != HashFloats(raw[i].Origin, 3)) appeared++;
	}
	const bool burst = resumed || appeared > 8;
	for (int i = 0; i < total; i++) {
		const UInt32 key = HashFloats(raw[i].Origin, 3);
		SplatSeen& seen = SplatsSeen[raw[i].Decal];
		if (seen.Key != key || seen.LastSeen <= 0.0) seen = { key, burst ? Clock - 1e5 : Clock, Clock };
		seen.LastSeen = Clock;
		raw[i].Age = max(raw[i].Age, (float)min(Clock - seen.FirstSeen, 1e5));
	}
	if (SplatsSeen.size() > 2048 || (SplatsSeen.size() > (size_t)total + 256)) {
		for (auto it = SplatsSeen.begin(); it != SplatsSeen.end();) {
			if (Clock - it->second.LastSeen > 5.0) it = SplatsSeen.erase(it);
			else ++it;
		}
	}
	const D3DXVECTOR3 eye(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
	struct Candidate { int Index; float Distance; };
	static Candidate candidates[1024];
	int kept = 0;
	for (int i = 0; i < total; i++) {
		RawSplat& s = raw[i];
		if (!s.TextureSet) continue;
		auto found = BloodTextureSets.find(s.TextureSet);
		if (found == BloodTextureSets.end()) {
			char path[260];
			ReadTexturePath(s.TextureSet, path, sizeof(path));
			const bool blood = strstr(path, "blood") != nullptr;
			found = BloodTextureSets.emplace(s.TextureSet, blood).first;
			if (LoggedTextures < 24) {
				LoggedTextures++;
				Logger::Log("BloodFlow: decal texture '%s' -> %s", path, blood ? "blood" : "not blood");
			}
		}
		if (!found->second || s.Local) continue;
		const D3DXVECTOR3 center(s.Origin[0], s.Origin[1], s.Origin[2]);
		const float distance = Length(center - eye);
		if (!(distance < Settings.MaxDistance)) continue;
		// UNOFFICIAL 2026-10-20 (user: 'the little giblets that bounce around reset the blood pool from them every time they roll;
		// they shouldn't produce any blood from our shader until they stop moving'; log: each bounce leaves a new 142-unit decal
		// thrown straight down). A decal thrown straight down (a gib landing; a shot's blood is thrown along the shot) is held
		// back for GibHold seconds, and dropped for good when a newer one lands near it within that time (the gib bounced on):
		// only the last, where the gib came to rest, is drawn, its age counted from then.
		if (fabsf(s.Direction[2]) > 0.99f && Settings.GibHold > 0.0f) {
			if (s.Age < Settings.GibHold) continue;
			bool bounced = false;
			for (int j = 0; j < total && !bounced; j++) {
				const RawSplat& o = raw[j];
				if (j == i || o.Local || o.TextureSet != s.TextureSet || !(fabsf(o.Direction[2]) > 0.99f)) continue;
				if (!(o.Age < s.Age && s.Age - o.Age < Settings.GibHold)) continue;
				const float dx = o.Origin[0] - s.Origin[0], dy = o.Origin[1] - s.Origin[1], dz = o.Origin[2] - s.Origin[2];
				bounced = dx * dx + dy * dy + dz * dz < 300.0f * 300.0f;
			}
			if (bounced) continue;
		}
		candidates[kept++] = { i, distance };
	}
	std::sort(candidates, candidates + kept, [](const Candidate& a, const Candidate& b) { return a.Distance < b.Distance; });
	kept = min(kept, Settings.MaxSplats);
	// UNOFFICIAL 2026-10-13 (user: blood that gathers could merge into one, to cost less): where MergeCount or more splats lie
	// close together on the same surface (centres within Merge x their radii, facing alike), they become one item: the first
	// in a fixed order (by decal, so it does not change as the camera moves) grown to cover the others. One box and one set of
	// streams instead of many overlapping ones, each running the whole blood shader over the same pixels. 0: off.
	static int mergedInto[1024];
	static float grownRadius[1024];
	std::sort(candidates, candidates + kept, [&](const Candidate& a, const Candidate& b) { return raw[a.Index].Decal < raw[b.Index].Decal; });
	auto splatRadius = [&](const RawSplat& s) { float r = 0.5f * max(s.Width, s.Height); return (r > 0.5f && r < 200.0f) ? std::clamp(r, 1.0f, 60.0f) : 8.0f; };
	auto splatFacing = [&](const RawSplat& s) { D3DXVECTOR3 n(-s.Direction[0], -s.Direction[1], -s.Direction[2]); const float l = Length(n); return l > 1e-3f ? n / l : D3DXVECTOR3(0, 0, 1); };
	int merged = 0;
	for (int c = 0; c < kept; c++) { mergedInto[c] = -1; grownRadius[c] = splatRadius(raw[candidates[c].Index]); }
	if (Settings.Merge > 0.0f && Settings.MergeCount >= 2) {
		static int members[1024];
		for (int c = 0; c < kept; c++) {
			if (mergedInto[c] >= 0) continue;
			const RawSplat& a = raw[candidates[c].Index];
			const D3DXVECTOR3 ca(a.Origin[0], a.Origin[1], a.Origin[2]), na = splatFacing(a);
			const float ra = splatRadius(a);
			int n = 0;
			for (int d = c + 1; d < kept; d++) {
				if (mergedInto[d] >= 0) continue;
				const RawSplat& b = raw[candidates[d].Index];
				const D3DXVECTOR3 cb(b.Origin[0], b.Origin[1], b.Origin[2]);
				const D3DXVECTOR3 nb = splatFacing(b);
				if (D3DXVec3Dot(&na, &nb) < 0.85f) continue;
				if (Length(cb - ca) > Settings.Merge * (ra + splatRadius(b))) continue;
				members[n++] = d;
			}
			if (n + 1 < Settings.MergeCount) continue;
			for (int m = 0; m < n; m++) {
				const RawSplat& b = raw[candidates[members[m]].Index];
				mergedInto[members[m]] = c;
				grownRadius[c] = std::clamp(max(grownRadius[c], Length(D3DXVECTOR3(b.Origin[0], b.Origin[1], b.Origin[2]) - ca) + splatRadius(b) * 0.6f), 1.0f, 60.0f);
				merged++;
			}
		}
	}
	if (merged > 0 && LoggedMerges < 10) { LoggedMerges++; Logger::Log("BloodFlow: %d of %d splats merged into neighbours (Merge %.2f, MergeCount %d)", merged, kept, Settings.Merge, Settings.MergeCount); }
	for (int c = 0; c < kept && Count < ItemMax; c++) {
		if (mergedInto[c] >= 0) continue;	// drawn as part of another
		RawSplat& s = raw[candidates[c].Index];
		Item& item = Items[Count];
		const D3DXVECTOR3 center(s.Origin[0], s.Origin[1], s.Origin[2]);
		// The surface's normal is only guessed here (against the way the blood was thrown): the shader finds the real
		// one from the depth buffer.
		D3DXVECTOR3 normal = -D3DXVECTOR3(s.Direction[0], s.Direction[1], s.Direction[2]);
		const float l = Length(normal);
		normal = l > 1e-3f ? normal / l : D3DXVECTOR3(0.0f, 0.0f, 1.0f);
		float radius = 0.5f * max(s.Width, s.Height);
		if (!(radius > 0.5f && radius < 200.0f)) radius = 8.0f;
		item.Center = center;
		item.Normal = normal;
		item.SlopeX = item.SlopeY = 0.0f;
		item.Radius = max(std::clamp(radius, 1.0f, 60.0f), grownRadius[c]);
		item.Age = std::clamp(fabsf(s.Direction[2]) > 0.99f ? s.Age - Settings.GibHold : s.Age, 0.0f, 1e6f);	// (a held gib's: from its release)
		item.Seed = Hash01(HashFloats(s.Origin, 3));
		float fade = 1.0f - std::clamp((candidates[c].Distance - 0.8f * Settings.MaxDistance) / (0.2f * Settings.MaxDistance), 0.0f, 1.0f);
		// Goes as the game removes it (by the game's own clock: most splats are never aged by the game, and their lifetime
		// then says nothing).
		if (s.Lifetime > 0.0f && s.GameAge > 0.0f) fade *= std::clamp((s.Lifetime - s.GameAge) / 2.0f, 0.0f, 1.0f);
		item.Fade = fade;
		item.Pool = false;
		if (item.Fade <= 0.0f) continue;
		// UNOFFICIAL 2026-10-13 (user: match my own blood, streams from the splatter's blood, not its edge): its texture's layout
		// and its colour (the shader multiplies in the texture's grey, MatchShade).
		item.DecalU = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
		item.DecalCode = 0;
		item.Tint = 0.0f;
		const bool laidOut = DecalLayout(s, item);
		if (Settings.MatchGameColor) {
			int t[3];
			for (int i = 0; i < 3; i++) t[i] = std::clamp((int)lroundf(s.Tint[i] * 255.0f), 0, 255);
			if (t[0] + t[1] + t[2] > 0) {
				item.Tint = (float)(t[0] * 65536 + t[1] * 256 + t[2]);
				for (int i = 0; i < 3; i++) LastTint[i] = s.Tint[i];
				HaveTint = true;
			}
		}
		if (LoggedLayouts < 6) {
			LoggedLayouts++;
			Logger::Log("BloodFlow: splat texture layout %s (quarter %d, turn %.2f, mirrored %d, mesh scale %.2f, %.1f x %.1f), colour %.3f %.3f %.3f",
				laidOut ? "read" : "NOT read", s.Quarter, s.Turn, s.Mirrored ? 1 : 0, s.HaveMesh ? s.MeshScale : 0.0f, s.Width, s.Height,
				s.Tint[0], s.Tint[1], s.Tint[2]);
		}
		UInt64 key = (UInt64)(UInt32)(uintptr_t)s.Decal | ((UInt64)HashFloats(s.Origin, 3) << 32);
		item.Slot = SlotFor(key & ~(1ull << 63));
		item.Fresh = false;
		if (item.Slot < 0) continue;
		if (LoggedSplats < 12) {
			LoggedSplats++;
			Logger::Log("BloodFlow: splat at (%.1f %.1f %.1f), %.0f units away, thrown (%.2f %.2f %.2f), size %.1f x %.1f, depth %.1f, age %.1f of %.1f s (mesh space: centre %.1f %.1f %.1f)",
				s.Origin[0], s.Origin[1], s.Origin[2], candidates[c].Distance, s.Direction[0], s.Direction[1], s.Direction[2],
				s.Width, s.Height, s.Depth, s.Age, s.Lifetime, s.Center[0], s.Center[1], s.Center[2]);
		}
		Count++;
	}
}

void BloodFlowEffect::GatherPools() {
	static RawActor raw[512];
	const int total = ReadActors(raw, 512);
	const D3DXVECTOR3 eye(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
	struct Candidate { UInt32 RefID; float Distance; };
	Candidate candidates[512];
	int kept = 0;
	for (int i = 0; i < total; i++) {
		RawActor& a = raw[i];
		Body& body = Bodies[a.RefID];
		body.SeenAt = Clock;
		const bool dead = a.LifeState == 1 || a.LifeState == 2;
		if (!dead) {
			body = Body();
			body.AliveAt = Clock;
			body.SeenAt = Clock;
			continue;
		}
		if (!a.Bleeds || !a.HasBody) continue;
		// UNOFFICIAL 2026-10-13: a body whose bound is nonsense (log: radius 2.6e10, lowest node -2.4e9) gets no pool.
		if (!(a.BoundRadius < 2000.0f) || !(fabsf(a.MinZ) < 1e6f)) continue;
		// Seen alive a moment ago: it died now. Otherwise the body was already there (loaded with the cell, or a save):
		// its pool is old, spread and dry.
		const bool old = body.DeadAt < 0.0 && !(body.AliveAt >= 0.0 && Clock - body.AliveAt < 5.0);
		if (body.DeadAt < 0.0) body.DeadAt = old ? Clock - 1e5 : Clock;
		const D3DXVECTOR3 now(a.BoundCenter[0], a.BoundCenter[1], a.BoundCenter[2]);
		const float moved = Length(now - body.Last);
		body.Last = now;
		const float maxRadius = std::clamp(a.BoundRadius * 0.3f, 14.0f, 36.0f) * Settings.PoolSize;
		if (body.Locked) {
			// Pushed or dragged well away from its pool: the pool stays behind (it is not drawn any more) and a new one starts.
			const float dx = now.x - body.Pool.x, dy = now.y - body.Pool.y;
			if (sqrtf(dx * dx + dy * dy) > body.MaxRadius * 1.5f) { body.Locked = false; body.StillSince = -1.0; }
		}
		if (!body.Locked) {
			// The pool starts once the body has come to rest (moved under a unit a frame for half a second), or at once
			// for an old body.
			if (moved < 3.0f) { if (body.StillSince < 0.0) body.StillSince = Clock; }
			else body.StillSince = -1.0;
			if (old || (body.StillSince >= 0.0 && Clock - body.StillSince > 0.5) || Clock - body.DeadAt > 3.0) {
				body.Locked = true;
				// UNOFFICIAL 2026-10-20 (user: 'the little pools seem to reset'): a body shot or bumped in a pile is pushed off its
				// pool again and again; the pool (and its cut parts' pools, aged from this too) used to start over from nothing
				// each time. Now it keeps the age it had: it moves with the body, already spread.
				if (!body.EverLocked) body.LockAt = old ? Clock - 1e5 : Clock;
				body.EverLocked = true;
				// (2026-10-27: on the ground under the body's middle, from its bones; it was the lowest bone less 1.5, far below the
				// ground there on a slope: GroundUnderBones.)
				body.Pool = D3DXVECTOR3(now.x, now.y, a.GroundZ);
				body.PoolSlope[0] = a.GroundSlope[0]; body.PoolSlope[1] = a.GroundSlope[1];
				body.MaxRadius = maxRadius;
				if (LoggedPools < 12) {
					LoggedPools++;
					Logger::Log("BloodFlow: pool under %08X at (%.1f %.1f %.1f), body radius %.1f, lowest node %.1f, ground slope (%.2f %.2f), pool radius %.1f, %s",
						a.RefID, now.x, now.y, body.Pool.z, a.BoundRadius, a.MinZ, a.GroundSlope[0], a.GroundSlope[1], maxRadius, old ? "old body" : "fresh");
				}
			}
		}
		if (Settings.GibPools > 0.0f) {
			const UInt16 fresh = a.Dismembered & ~body.GibMask;
			for (int k = 0; k < 16; k++) {
				if (!(a.Dismembered & (1 << k))) continue;
				if (fresh & (1 << k)) body.GibAt[k] = old ? Clock - 1e5 : Clock;
				if (body.Locked && !(fresh & (1 << k))) continue;	// at rest: it stays where it is
				// The stump's joint, on the ground under it; a part with no such node: beside the body's middle.
				const float* at = a.PartFound[k] ? a.PartPos[k] : a.BoundCenter;
				const float spread = a.PartFound[k] ? 0.0f : a.BoundRadius * 0.3f;
				const float gx = at[0] + spread * cosf(k * 1.7f), gy = at[1] + spread * sinf(k * 1.7f);
				body.GibPool[k] = D3DXVECTOR3(gx, gy, a.GroundZ + a.GroundSlope[0] * (gx - now.x) + a.GroundSlope[1] * (gy - now.y));	// (2026-10-27: on the bones' ground plane)
			}
			if (fresh && LoggedGibs < 20) {
				LoggedGibs++;
				Logger::Log("BloodFlow: %08X cut parts %04X (new %04X): pools under them%s", a.RefID, a.Dismembered, fresh, old ? " (old body)" : "");
			}
			body.GibMask = a.Dismembered;
		}
		if (!body.Locked) continue;
		const float distance = Length(body.Pool - eye);
		if (!(distance < Settings.MaxDistance)) continue;
		candidates[kept++] = { a.RefID, distance };
	}
	std::sort(candidates, candidates + kept, [](const Candidate& a, const Candidate& b) { return a.Distance < b.Distance; });
	kept = min(kept, Settings.MaxPools);
	// UNOFFICIAL 2026-10-25 (user: blood is red where it should match the game's colours): a pool took the colour of the last
	// splat seen anywhere, and red FreshColor before the first one, so a green- or yellow-blooded creature's pool came out in
	// someone else's colour. Each body now takes the colour of the nearest of this frame's splats within about 3 m of it
	// (its own blood, thrown when it was hit), kept until it gets up again; the last colour seen is only the fallback.
	const int splatCount = Count;
	for (int c = 0; c < kept && Settings.MatchGameColor; c++) {
		Body& body = Bodies[candidates[c].RefID];
		if (body.Tint > 0.0f) continue;
		float best = 200.0f * 200.0f;
		for (int i = 0; i < splatCount; i++) {
			if (Items[i].Pool || !(Items[i].Tint > 0.0f)) continue;
			const D3DXVECTOR3 d = Items[i].Center - body.Last;
			const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
			if (d2 < best) { best = d2; body.Tint = Items[i].Tint; }
		}
	}
	for (int c = 0; c < kept && Count < ItemMax; c++) {
		const Body& body = Bodies[candidates[c].RefID];
		// Pools' keys have the top bit set (splats' never do); a pool started afresh (body moved away) gets a new one.
		const UInt64 key = (1ull << 63) | ((UInt64)HashFloats((const float*)&body.Pool, 3) << 32) | candidates[c].RefID;
		const int slot = SlotFor(key);
		if (slot < 0) continue;
		Item& item = Items[Count++];
		item.Slot = slot;
		item.Fresh = false;
		item.Center = body.Pool;
		item.Normal = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
		item.SlopeX = body.PoolSlope[0]; item.SlopeY = body.PoolSlope[1];
		item.Radius = body.MaxRadius;
		item.Age = (float)min(Clock - body.LockAt, 1e5);
		item.Seed = Hash01(candidates[c].RefID);
		item.Fade = 1.0f - std::clamp((candidates[c].Distance - 0.8f * Settings.MaxDistance) / (0.2f * Settings.MaxDistance), 0.0f, 1.0f);
		item.Pool = true;
		item.DecalU = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
		item.DecalCode = 0;
		item.Tint = 0.0f;
		if (Settings.MatchGameColor && body.Tint > 0.0f) item.Tint = body.Tint;
		else if (Settings.MatchGameColor && HaveTint) {
			int t[3];
			for (int i = 0; i < 3; i++) t[i] = std::clamp((int)lroundf(LastTint[i] * 255.0f), 0, 255);
			item.Tint = (float)(t[0] * 65536 + t[1] * 256 + t[2]);
		}
		if (c == 0 && LoggedNearest < 30) {
			static double loggedAt = -1e9;
			if (Clock - loggedAt > 2.0) {
				loggedAt = Clock;
				LoggedNearest++;
				const D3DXVECTOR3 rel = body.Pool - eye;
				Logger::Log("BloodFlow: nearest pool %08X at (%.1f %.1f %.1f), from the eye (%.1f %.1f %.1f) = %.0f units, radius %.1f, age %.1f s, eye (%.1f %.1f %.1f)",
					candidates[c].RefID, body.Pool.x, body.Pool.y, body.Pool.z, rel.x, rel.y, rel.z, candidates[c].Distance, item.Radius, item.Age, eye.x, eye.y, eye.z);
			}
		}
	}
	// UNOFFICIAL 2026-10-13: the cut-off parts' pools (GibPools), for the same bodies, while there is room.
	for (int c = 0; c < kept && Settings.GibPools > 0.0f; c++) {
		const Body& body = Bodies[candidates[c].RefID];
		if (!body.GibMask) continue;
		int gibs = 0;
		for (int k = 0; k < 16 && Count < ItemMax && gibs < 3; k++) {
			if (!(body.GibMask & (1 << k))) continue;
			// (A stump inside the body's own pool adds nothing but a second full-cost pool over the same pixels: the body's pool
			// is it. User's F10, 2026-10-13: a ghoul with four cut limbs drew five pools on one spot.)
			const float sx = body.GibPool[k].x - body.Pool.x, sy = body.GibPool[k].y - body.Pool.y;
			if (sx * sx + sy * sy < body.MaxRadius * body.MaxRadius * 0.49f) continue;
			gibs++;
			const UInt64 key = (1ull << 63) | ((UInt64)(HashFloats((const float*)&body.GibPool[k], 3) ^ (UInt32)(k * 0x9E3779B1u)) << 32) | candidates[c].RefID;
			const int slot = SlotFor(key);
			if (slot < 0) continue;
			Item& item = Items[Count++];
			item.Slot = slot;
			item.Fresh = false;
			item.Center = body.GibPool[k];
			item.Normal = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
			item.SlopeX = body.PoolSlope[0]; item.SlopeY = body.PoolSlope[1];
			item.Radius = std::clamp(body.MaxRadius * Settings.GibPools, 4.0f, 60.0f);
			item.Age = (float)min(Clock - max(body.GibAt[k], body.LockAt), 1e5);
			item.Seed = Hash01(candidates[c].RefID * 31u + (UInt32)k * 977u);
			item.Fade = 1.0f - std::clamp((candidates[c].Distance - 0.8f * Settings.MaxDistance) / (0.2f * Settings.MaxDistance), 0.0f, 1.0f);
			item.Pool = true;
			item.DecalU = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
			item.DecalCode = 0;
			item.Tint = 0.0f;
			if (Settings.MatchGameColor && body.Tint > 0.0f) item.Tint = body.Tint;
			else if (Settings.MatchGameColor && HaveTint) {
				int t[3];
				for (int i = 0; i < 3; i++) t[i] = std::clamp((int)lroundf(LastTint[i] * 255.0f), 0, 255);
				item.Tint = (float)(t[0] * 65536 + t[1] * 256 + t[2]);
			}
		}
	}
	// Forget bodies not seen for a minute (unloaded): coming back, they count as old bodies.
	if (Bodies.size() > 256) {
		for (auto it = Bodies.begin(); it != Bodies.end();) {
			if (Clock - it->second.SeenAt > 60.0) it = Bodies.erase(it);
			else ++it;
		}
	}
}

// UNOFFICIAL 2026-10-22, TEST ONLY (TestSplats > 0): the game's fresh splats are not in saves, so the splat shapes' cost could not be
// measured from a test save. This lays that many made-up splats on the ground round a point 150 units ahead of the player (where the
// camera looked when it was turned on), 70 units apart, radius 20-60 as the game's, thrown straight down; they age from then on.
// Off (0): nothing, and the next time starts afresh.
void BloodFlowEffect::AddTestSplats() {
	// UNOFFICIAL 2026-11-12 Surface, TEST ONLY ([Shaders.BloodFlow.Surface] TestPour): a body's pool on the ground 150 units ahead of
	// the player, bleeding from when this is turned on (a save holds bodies that are long dead, never a fresh death).
	static double pourStart = -1.0;
	static D3DXVECTOR3 pourAt(0.0f, 0.0f, 0.0f);
	if (!SurfaceTestPour || !Player) pourStart = -1.0;
	else {
		if (pourStart < 0.0) {
			D3DXVECTOR3 forward(TheRenderManager->viewMatrix._13, TheRenderManager->viewMatrix._23, 0.0f);
			const float l = BloodFlowBoxes::Length(forward);
			forward = l > 1e-4f ? forward / l : D3DXVECTOR3(0.0f, 1.0f, 0.0f);
			pourAt = D3DXVECTOR3(Player->pos.x, Player->pos.y, Player->pos.z) + forward * 150.0f;
			pourStart = Clock;
			Logger::Log("BloodFlow: TEST a pool bleeding at (%.1f %.1f %.1f)", pourAt.x, pourAt.y, pourAt.z);
		}
		const int slot = Count < ItemMax ? SlotFor(0x7E57A00000000000ull + (UInt64)(UInt32)(pourStart * 16.0)) : -1;
		if (slot >= 0) {
			Item& item = Items[Count++];
			item = Item();
			item.Center = pourAt;
			item.Normal = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
			item.Radius = 26.0f;
			item.Age = (float)(Clock - pourStart);
			item.Seed = Hash01((UInt32)(pourStart * 16.0));
			item.Fade = 1.0f;
			item.Pool = true;
			item.Slot = slot;
		}
	}
	if (Settings.TestSplats <= 0 || !Player) { TestStart = -1.0; return; }
	if (TestStart < 0.0) {
		D3DXVECTOR3 forward(TheRenderManager->viewMatrix._13, TheRenderManager->viewMatrix._23, 0.0f);
		const float l = BloodFlowBoxes::Length(forward);
		forward = l > 1e-4f ? forward / l : D3DXVECTOR3(0.0f, 1.0f, 0.0f);
		TestAnchor = D3DXVECTOR3(Player->pos.x, Player->pos.y, Player->pos.z) + forward * 150.0f;
		TestStart = Clock;
		Logger::Log("BloodFlow: TEST %d made-up splats round (%.1f %.1f %.1f)", Settings.TestSplats, TestAnchor.x, TestAnchor.y, TestAnchor.z);
	}
	const int columns = 4;
	for (int i = 0; i < Settings.TestSplats && Count < ItemMax; i++) {
		const float h = (float)fmod(sin(i * 12.9898 + 4.1) * 43758.5453, 1.0);
		const float hash = h < 0.0f ? h + 1.0f : h;
		const int slot = SlotFor(0x7E57000000000000ull + (UInt64)i);
		if (slot < 0) break;
		Item& item = Items[Count++];
		item = Item();
		item.Center = TestAnchor + D3DXVECTOR3(((i % columns) - 1.5f) * 70.0f, (float)(i / columns) * 70.0f - 70.0f, 0.0f);
		// (2026-10-27: as a game splat on a floor reads: the decal's +0x40 is (0 0 1), out of the floor, and Item.Normal is minus that.
		// These had the opposite sign, so the fluid's throw, wrong for every game splat, looked right with them. BloodFlow's own
		// passes only mirror the shapes with the sign.)
		item.Normal = D3DXVECTOR3(-0.05f, 0.05f, -1.0f);
		item.Radius = 20.0f + 40.0f * hash;
		item.Age = (float)(Clock - TestStart);
		item.Seed = hash;
		item.Fade = 1.0f;
		item.Pool = false;
		item.Slot = slot;
	}
}

void BloodFlowEffect::UpdateConstants() {
	// UNOFFICIAL 2026-10-13: once a frame (UpdateConstants runs twice a frame), at the rate the look was tuned at (FrameClock.h):
	// the blood's clock and its time settings run at twice real time, as they always did.
	static double lastFrameTime = -1.0, lastGatherTime = -1.0;
	static CpuTimer gatherTimer("Blood gather (CPU)");	// UNOFFICIAL 2026-10-16: the per-frame CPU cost of reading the game's lists
	CpuProfileScope cpu(gatherTimer);
	Clock += ElapsedOncePerFrame(lastFrameTime) * TunedClockRate;
	{
		// UNOFFICIAL 2026-10-26 BloodFluid: the fluid's runs replace the painted streams and splat drips while it is on.
		const bool replace = TheBloodFluid().ReplacesStreams();
		Settings.DripLength = replace ? 0.0f : BloodFlowBaseDripLength;
		const float splatDrips = replace ? 0.0f : BloodFlowBaseSplatDrips;
		if (Settings.SplatDrips != splatDrips) { Settings.SplatDrips = splatDrips; Mask.Reset(); MaskShape[2] = splatDrips; }
	}
	if (TheShaderManager->GameState.isUnderwater) { Count = 0; return; }
	// UNOFFICIAL 2026-10-16 (cheaper): the game's lists are read once a frame; the second call of the frame (before the post
	// effects) keeps the first call's items (same frame, same decals and bodies).
	bool gathered = false;
	if (TheFrameRateManager->Time != lastGatherTime || !Settings.CheapGather) {
		gathered = true;
		lastGatherTime = TheFrameRateManager->Time;
		Frame++;
		Count = 0;
		if (Settings.Splats) GatherSplats();
		if (Settings.Pools) GatherPools();
		AddTestSplats();
	}
	// UNOFFICIAL 2026-10-16 (cheaper): which items' boxes touch the screen (both calls: the second has this frame's camera).
	// Off-screen ones are not probed and do not make the effect run (nor keep the shadow/AO/fog composite off).
	{
		const D3DXVECTOR4& c = TheRenderManager->CameraPosition;
		const D3DXVECTOR3 eye(c.x, c.y, c.z);
		const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
		const D3DXVECTOR3 origin(inv._41, inv._42, inv._43);
		OnScreenCount = 0;
		for (int i = 0; i < Count; i++) {
			Items[i].Visible = !Settings.CheapGather || BloodFlowBoxes::OnScreen(Items[i], Settings.DripLength, Settings.MaxFall, eye, origin, TheRenderManager->ViewProjMatrix);
			OnScreenCount += Items[i].Visible ? 1 : 0;
		}
	}
	// UNOFFICIAL 2026-10-26 BloodFluid (BloodFluid.h): fresh blood becomes fluid (once a frame); the pools it takes are left out here.
	if (gathered && TheBloodFluid().On()) {
		int kept = 0;
		OnScreenCount = 0;
		for (int i = 0; i < Count; i++) {
			if (TheBloodFluid().Offer(Items[i].Slot >= 0 ? Slots[Items[i].Slot].Key : 0, Items[i])) continue;
			OnScreenCount += Items[i].Visible ? 1 : 0;
			if (kept != i) Items[kept] = Items[i];
			kept++;
		}
		Count = kept;
	}
	// UNOFFICIAL 2026-11-12 Surface (BloodSurface.h): the items it has (bodies' pools, fresh splats; older splats keep the game's
	// decal only) are left out of the passes below (once a frame); its patches are brought up to date on both calls (the second
	// has this frame's camera). While it has a patch it asks for the albedo record (core/CapturedNormals.h AlbedoWanted).
	if (SurfaceSettings.Enabled && !SurfaceFailed) {
		const D3DXVECTOR4& c = TheRenderManager->CameraPosition;
		const D3DXVECTOR3 eye(c.x, c.y, c.z);
		const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
		const double now = TheFrameRateManager->Time;
		// Through a door, or a jump (fast travel, a load): the blood stays behind (as BloodFluid: interiors share their coordinates).
		// Walking from one exterior cell into the next keeps it. Bodies in the new place get their pools as they are met.
		static bool lastExterior = true;
		static D3DXVECTOR3 lastEye(0.0f, 0.0f, 0.0f);
		const bool exterior = TheShaderManager->GameState.isExterior;
		if (SurfaceAtlas.Any() && ((TheShaderManager->GameState.isCellChanged && !(exterior && lastExterior)) || BloodFlowBoxes::Length(eye - lastEye) > 3000.0f)) SurfaceAtlas.Clear();
		lastExterior = exterior;
		lastEye = eye;
		if (gathered) {
			int kept = 0;
			OnScreenCount = 0;
			for (int i = 0; i < Count; i++) {
				if (SurfaceAtlas.Offer(Items[i].Slot >= 0 ? Slots[Items[i].Slot].Key : 0, Items[i], eye, now, SurfaceSettings)) continue;
				OnScreenCount += Items[i].Visible ? 1 : 0;
				if (kept != i) Items[kept] = Items[i];
				kept++;
			}
			Count = kept;
		}
		SurfaceAtlas.Update(eye, D3DXVECTOR3(inv._41, inv._42, inv._43), TheRenderManager->ViewProjMatrix, now, SurfaceSettings);
	}
	CapturedNormals::BloodWantsAlbedo = SurfaceSettings.Enabled && !SurfaceFailed && SurfaceSettings.UseAlbedo && SurfaceAtlas.Any();
	// UNOFFICIAL 2026-10-13: with F10 on, how many it draws (next to the Blood timers), every 240 frames.
	if (GpuTimer::Enabled && ++ItemLogTick >= 240) {
		ItemLogTick = 0;
		int pools = 0;
		for (int i = 0; i < Count; i++) pools += Items[i].Pool ? 1 : 0;
		Logger::Log("BLOOD ITEMS: %d splats, %d pools (bodies' and cut limbs')", Count - pools, pools);
		if (SurfaceSettings.Enabled)	// UNOFFICIAL 2026-11-12 Surface
			Logger::Log("BLOOD SURFACE: %d patches, %d still running (made %d so far, %d took another's cell, %d found none, %d splats landed on another's patch)", SurfaceAtlas.Count(),
				SurfaceAtlas.Running(), SurfaceAtlas.made, SurfaceAtlas.reclaimed, SurfaceAtlas.refused, SurfaceAtlas.shared);
	}

	Constants.Fresh = D3DXVECTOR4(SrgbToLinear(Settings.FreshColor[0]), SrgbToLinear(Settings.FreshColor[1]), SrgbToLinear(Settings.FreshColor[2]), Settings.Gloss);
	Constants.Dry = D3DXVECTOR4(SrgbToLinear(Settings.DryColor[0]), SrgbToLinear(Settings.DryColor[1]), SrgbToLinear(Settings.DryColor[2]), Settings.DryTime);
	Constants.Info = D3DXVECTOR4(TheShaderManager->GameState.isExterior ? 1.0f : 0.0f, (float)Settings.DebugView, Settings.PoolDryTime, Settings.FlowSpeed);
	// Gravity: 9.81 m/s^2 at 1.4287 cm a unit.
	Constants.Pool = D3DXVECTOR4(Settings.PoolSpreadTime, Settings.DripLength, Settings.MaxFall, 687.0f);
	// Paths are kept once seen from close enough that a pixel is under 0.8 units (about 1100 units away at 1440p).
	Constants.Probe = D3DXVECTOR4(1.0f / PathRows, 0.8f, (float)PathRows, Settings.SplatterAtlas ? 1.0f : 0.0f);
	// The flow's motion runs on the effect's own clock, wrapped every 1023 s (a whole number of the shader's 3 s flow
	// periods) so it keeps its precision.
	Constants.Match = D3DXVECTOR4(Settings.MatchShade, Settings.MatchBrightness, Settings.StreamWidth, Settings.MatchGameColor ? 1.0f : 0.0f);
	for (int i = 0; i < 3; i++)
		((float*)&Constants.DryRatio)[i] = std::clamp(SrgbToLinear(Settings.DryColor[i]) / max(SrgbToLinear(Settings.FreshColor[i]), 1e-4f), 0.05f, 4.0f);
	Constants.DryRatio.w = 0.0f;
	Constants.Beads = D3DXVECTOR4(Settings.Beads, Settings.BeadSize, 0.0f, 0.0f);
	Constants.Splat = D3DXVECTOR4(Settings.Splat, Settings.SplatReach, Settings.SplatSpray, Settings.SplatDrips);
	BloodFlowGround = D3DXVECTOR4(BloodFlowGroundFit ? 1.0f : 0.0f, 3.0f, 0.0f, BloodFlowGroundUnseen ? 1.0f : 0.0f);
	// The boxes' tops (BloodFlowBoxes.h): as tall as the shapes with GroundFit, and in debug view 2 (which shows what the old top cut).
	BloodFlowBoxes::SplatUp() = BloodFlowGroundFit || Settings.DebugView == 2 ? 1.2f + 2.4f * Settings.SplatReach : 1.0f;
	BloodFlowBoxes::PoolUp() = BloodFlowGroundFit ? 1.25f : 0.3f;
	Constants.Flow = D3DXVECTOR4(Settings.FlowSpeed, Settings.Viscosity, Settings.BloodAmount, (float)fmod(Clock, 1023.0));
}

bool BloodFlowEffect::ShouldRender() {
	return (Count > 0 && OnScreenCount > 0) || TheBloodFluid().Active()	// UNOFFICIAL 2026-10-16: only blood on screen (2026-10-26: or the fluid's)
		|| (SurfaceSettings.Enabled && !SurfaceFailed && SurfaceAtlas.Any());	// UNOFFICIAL 2026-11-12 Surface: or a patch (running ones step off screen too)
}

// UNOFFICIAL 2026-11-12 Surface (BloodSurface.h; Includes/BloodSurface.hlsl): the patches' steps (BloodSurfaceSim: atlas copy 0 ->
// 1 -> 0, so copy 0 always holds the newest state; a pair a frame, eight more for a patch catching up), then their blood into
// the image (BloodSurface), which is read as it was from TESR_SourceBuffer (the area the boxes cover is copied there). True when
// it drew into the image.
static bool RenderSurface(BloodFlowEffect* self, IDirect3DDevice9* device, IDirect3DSurface9* image, IDirect3DSurface9* source, IDirect3DSurface9* depthStencil,
	const D3DXVECTOR3& eye, const D3DXMATRIX& inv, float rw, float rh, float maxDistance, IDirect3DVertexDeclaration9* declaration) {
	static EffectRecord::CachedHandle simCache, drawCache;
	D3DXHANDLE sim = self->TechniqueHandle(simCache, "BloodSurfaceSim");
	D3DXHANDLE draw = self->TechniqueHandle(drawCache, "BloodSurface");
	if (!sim || !draw) return false;	// (a shader file older than the mode)
	if (!SurfaceTexture[0]) {
		IDirect3D9* d3d = nullptr;
		D3DDEVICE_CREATION_PARAMETERS creation = {};
		D3DDISPLAYMODE mode = {};
		bool usable = false;
		if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
			if (SUCCEEDED(device->GetCreationParameters(&creation)) && SUCCEEDED(device->GetDisplayMode(0, &mode)))
				usable = SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
					D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F));
			d3d->Release();
		}
		for (int k = 0; k < 2 && usable; k++) {
			if (FAILED(device->CreateTexture(BloodSurface::AtlasWidth, BloodSurface::AtlasHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
					&SurfaceTexture[k], NULL))) { SurfaceTexture[k] = nullptr; usable = false; break; }
			SurfaceTexture[k]->GetSurfaceLevel(0, &SurfaceTarget[k]);
			device->ColorFill(SurfaceTarget[k], NULL, 0);
		}
		if (!usable) {
			for (int k = 0; k < 2; k++) {
				if (SurfaceTarget[k]) { SurfaceTarget[k]->Release(); SurfaceTarget[k] = nullptr; }
				if (SurfaceTexture[k]) { SurfaceTexture[k]->Release(); SurfaceTexture[k] = nullptr; }
			}
			SurfaceFailed = true;
		}
		Logger::Log("BloodFlow: Surface state atlas (%d x %d, fp16 rgba, two copies): %s", BloodSurface::AtlasWidth, BloodSurface::AtlasHeight,
			usable ? "made" : "NOT made (the card cannot draw into or filter that format): Surface is off, BloodFlow's own pools and streams are back");
		if (!usable) { SurfaceAtlas.Clear(); return false; }
	}
	const double now = TheFrameRateManager->Time;
	float seconds = SurfaceLastTime < 0.0 ? 1.0f / 60.0f : (float)(now - SurfaceLastTime);
	const bool newFrame = SurfaceLastTime != now;	// (Render can run more than once a frame: the blood steps once)
	SurfaceLastTime = now;
	seconds = std::clamp(seconds, 0.0f, 0.1f);
	const BloodSurface::Settings& s = SurfaceSettings;
	SurfaceSizeConstants = BloodSurface::SizeConstants();
	SurfaceFlowConstants = BloodSurface::FlowConstants(s);
	SurfaceLookConstants = BloodSurface::LookConstants(s);
	static GpuTimer simTimer("  Blood surface steps"), drawTimer("  Blood surface draw");
	UINT passes = 0;
	if (newFrame && seconds > 0.0f && SurfaceAtlas.Running() > 0) {
		GpuProfileScope gpu(simTimer, device);
		int normal = 0;
		const int pairs = SurfaceAtlas.Pairs(seconds, normal);
		// A new patch's cell still holds the last one's blood: cleared in both copies (before its first pair).
		for (int i = 0; i < BloodSurface::Cells && normal > 0; i++) {
			const BloodSurface::Patch& p = SurfaceAtlas.patches[i];
			if (!p.Used || !p.Reset || !p.Seen) continue;
			RECT cell;
			BloodSurface::Atlas::CellRect(i, cell);
			for (int k = 0; k < 2; k++) device->ColorFill(SurfaceTarget[k], &cell, 0);
		}
		device->SetDepthStencilSurface(nullptr);	// (the atlas can be bigger than the game's depth-stencil)
		self->Effect->SetTechnique(sim);
		for (int pair = 0; pair < pairs; pair++) {
			const int kind = BloodSurface::Atlas::Kind(pair, normal);
			bool drew = false;
			for (int k = 0; k < 2; k++) {
				const int triangles = SurfaceAtlas.BuildSim(kind, k, eye, SurfaceSimVertices);
				if (!triangles) continue;
				SurfaceSimConstants = BloodSurface::SimConstants(s, BloodSurface::Atlas::StepOf(kind), kind == 0 && k == 0);
				SurfaceRead = SurfaceTexture[k];
				// (Not bound to its own sampler while drawn into: TESR_BloodSurface is the 14th sampler declared, register 13.)
				device->SetTexture(13, nullptr);
				TheRenderManager->renderState->NoteDeviceTexture(13, nullptr);
				device->SetRenderTarget(0, SurfaceTarget[1 - k]);
				self->SetCT();
				if (SUCCEEDED(self->Effect->Begin(&passes, 0))) {
					self->Effect->BeginPass(0);
					device->SetVertexDeclaration(declaration);
					device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, SurfaceSimVertices, sizeof(VolumetricSmokeQuads::Vertex));
					self->Effect->EndPass();
					self->Effect->End();
					drew = true;
				}
			}
			if (drew) SurfaceAtlas.AfterPair(kind, now);
		}
		device->SetDepthStencilSurface(depthStencil);
		static int loggedSteps = 0;
		if (loggedSteps < 3 && pairs > 0) { loggedSteps++; Logger::Log("BloodFlow: Surface stepped %d patches, %d pairs of steps (%d patches in all)", SurfaceAtlas.Running(), pairs, SurfaceAtlas.Count()); }
	}
	// A patch that has frozen: its blood's box, from its cell read back (one patch at a time, a quarter of a second apart: the
	// read waits for the graphics card).
	const int toMeasure = SurfaceMeasureFailed || fabs(now - SurfaceMeasuredAt) < 0.25 ? -1 : SurfaceAtlas.NextToMeasure();
	if (toMeasure >= 0) {
		SurfaceMeasuredAt = now;
		if (!SurfaceMeasureTarget) {
			if (FAILED(device->CreateRenderTarget(BloodSurface::CellSize, BloodSurface::CellSize, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &SurfaceMeasureTarget, NULL)) ||
				FAILED(device->CreateOffscreenPlainSurface(BloodSurface::CellSize, BloodSurface::CellSize, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &SurfaceMeasureRead, NULL))) {
				SurfaceMeasureFailed = true;
				Logger::Log("BloodFlow: Surface cannot read a frozen patch back: frozen patches keep their whole box (they cost more to draw)");
			}
		}
		RECT cell;
		BloodSurface::Atlas::CellRect(toMeasure, cell);
		D3DLOCKED_RECT locked;
		if (!SurfaceMeasureFailed && SUCCEEDED(device->StretchRect(SurfaceTarget[0], &cell, SurfaceMeasureTarget, NULL, D3DTEXF_NONE)) &&
			SUCCEEDED(device->GetRenderTargetData(SurfaceMeasureTarget, SurfaceMeasureRead)) && SUCCEEDED(SurfaceMeasureRead->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
			SurfaceAtlas.Measure(toMeasure, locked.pBits, locked.Pitch);
			SurfaceMeasureRead->UnlockRect();
			const BloodSurface::Patch& p = SurfaceAtlas.patches[toMeasure];
			static int loggedMeasures = 0;
			if (loggedMeasures < 6) {
				loggedMeasures++;
				Logger::Log("BloodFlow: Surface patch %d frozen after %.1f s: %s (its box %.0f x %.0f x %.0f of %.0f x %.0f x %.0f units)", toMeasure, p.SimAge, p.Empty ? "no blood left on it" : "measured",
					p.Box[3] - p.Box[0], p.Box[4] - p.Box[1], p.Box[5] - p.Box[2], 2.0f * p.Half, 2.0f * p.Half, 2.0f * p.Half * BloodSurface::BoxUp);
			}
		}
		else SurfaceAtlas.patches[toMeasure].Measured = true;	// (not again)
	}
	SurfaceRead = SurfaceTexture[0];
	float bounds[4];
	const int triangles = SurfaceAtlas.BuildBoxes(eye, D3DXVECTOR3(inv._41, inv._42, inv._43), TheRenderManager->ViewProjMatrix, now, maxDistance, s, SurfaceBoxVertices, bounds);
	RECT area;
	area.left = (LONG)max(0.0f, floorf(std::clamp(bounds[0], 0.0f, 1.0f) * rw) - 2.0f);
	area.top = (LONG)max(0.0f, floorf(std::clamp(bounds[1], 0.0f, 1.0f) * rh) - 2.0f);
	area.right = (LONG)min(rw, ceilf(std::clamp(bounds[2], 0.0f, 1.0f) * rw) + 2.0f);
	area.bottom = (LONG)min(rh, ceilf(std::clamp(bounds[3], 0.0f, 1.0f) * rh) + 2.0f);
	if (!triangles || area.right <= area.left || area.bottom <= area.top) return false;
	GpuProfileScope gpu(drawTimer, device);
	if (source) device->StretchRect(image, &area, source, &area, D3DTEXF_NONE);
	device->SetRenderTarget(0, image);
	RenderRect::ApplyViewport(device);
	self->Effect->SetTechnique(draw);
	self->SetCT();
	if (SUCCEEDED(self->Effect->Begin(&passes, 0))) {
		self->Effect->BeginPass(0);
		device->SetVertexDeclaration(declaration);
		device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, SurfaceBoxVertices, sizeof(VolumetricSmokeQuads::Vertex));
		self->Effect->EndPass();
		self->Effect->End();
	}
	return true;
}

void BloodFlowEffect::Render(IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* rendered,
	UINT technique, bool clear, IDirect3DSurface9* source) {
	if (!Enabled || !Effect || !ShouldRender()) { NoteSkip(); renderTime = 0; return; }
	static CachedHandle techniqueCache, probeCache, boundsCache, beadsCache, splatCache, splatFastCache, splatMaskCache, maskBakeCache;
	D3DXHANDLE boxes = TechniqueHandle(techniqueCache, "BloodFlow");
	D3DXHANDLE probe = TechniqueHandle(probeCache, "BloodFlowProbe");
	D3DXHANDLE boundsTechnique = TechniqueHandle(boundsCache, "BloodFlowBounds");
	if (!boxes || !probe || !boundsTechnique || !PathSurface[0] || !PathSurface[1] || !BoundsSurface) {
		return;
	}
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

	const D3DXVECTOR4& cameraPosition = TheRenderManager->CameraPosition;
	const D3DXVECTOR3 eye(cameraPosition.x, cameraPosition.y, cameraPosition.z);
	const D3DXMATRIX& inv = TheRenderManager->InvViewMatrix;
	const float w = (float)TheRenderManager->width, h = (float)TheRenderManager->height;
	// UNOFFICIAL 2026-10-27 RenderScale v2: the drawn part of the screen (all of it outside rect frames); the boxes' screen bounds are 0..1 of it.
	const float rw = (float)RenderRect::Width((unsigned)w), rh = (float)RenderRect::Height((unsigned)h);

	IDirect3DSurface9* previousTarget = nullptr;
	IDirect3DSurface9* previousDepth = nullptr;
	IDirect3DVertexDeclaration9* previousDeclaration = nullptr;
	IDirect3DVertexBuffer9* previousStream = nullptr;
	UINT previousOffset = 0, previousStride = 0;
	device->GetRenderTarget(0, &previousTarget);
	device->GetDepthStencilSurface(&previousDepth);
	device->GetVertexDeclaration(&previousDeclaration);
	device->GetStreamSource(0, &previousStream, &previousOffset, &previousStride);

	// 1. Where each stream runs (BloodFlow.fx.hlsl, ProbePS), for every item gathered, on screen or not: from last frame's
	// path texture into the other one. An item whose rows were not written last time (new, or skipped a frame) starts afresh.
	for (int i = 0; i < Count; i++) Items[i].Fresh = Slots[Items[i].Slot].WrittenPass != ProbePasses - 1;
	const int probeTriangles = BloodFlowBoxes::BuildProbe(Items, Count, Settings.DripLength, PathRows, BloodFlowBoxes::Streams, eye, ProbeVertices);
	const int write = 1 - PathIndex;
	PathRead = PathTexture[PathIndex];
	device->SetRenderTarget(0, PathSurface[write]);
	// (The path texture is smaller than the game's depth-stencil, which is legal; without ReShadeDepthCompat it is unbound.)
	if (!ReShadeIntegration::KeepDepthBound()) device->SetDepthStencilSurface(nullptr);
	Effect->SetTechnique(probe);
	SetCT();
	UINT passes = 0;
	// UNOFFICIAL 2026-10-13 (user: blood pools close up cost a lot; F10 showed Blood flow 4.4 ms): each step timed on its own.
	static GpuTimer probeTimer("  Blood probe"), boundsTimer("  Blood bounds"), paintTimer("  Blood paint (boxes)"), beadsTimer("  Blood beads"),
		splatTimer("  Blood splat shapes");
	if (probeTriangles && SUCCEEDED(Effect->Begin(&passes, 0))) {
		GpuProfileScope gpu(probeTimer, device);
		Effect->BeginPass(0);
		device->SetVertexDeclaration(Declaration);
		device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, probeTriangles, ProbeVertices, sizeof(VolumetricSmokeQuads::Vertex));
		Effect->EndPass();
		Effect->End();
	}
	for (int i = 0; i < Count; i++) if (Items[i].Visible) Slots[Items[i].Slot].WrittenPass = ProbePasses;	// (off screen: fresh when back)
	ProbePasses++;
	PathIndex = write;
	PathRead = PathTexture[PathIndex];

	// 2. The box round each item's real paths (BoundsPS), from the paths just found: the paint pass skips the rest.
	const int boundsTriangles = BloodFlowBoxes::BuildProbe(Items, Count, Settings.DripLength, ItemMax, 1, eye, BoundsVertices);
	device->SetRenderTarget(0, BoundsSurface);
	Effect->SetTechnique(boundsTechnique);
	SetCT();
	// (Not bound to its own sampler while drawn into: TESR_BloodFlowBounds is the 7th sampler declared, register 6.)
	device->SetTexture(6, nullptr);
	TheRenderManager->renderState->NoteDeviceTexture(6, nullptr);
	if (boundsTriangles && SUCCEEDED(Effect->Begin(&passes, 0))) {
		GpuProfileScope gpu(boundsTimer, device);
		Effect->BeginPass(0);
		device->SetVertexDeclaration(Declaration);
		device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, boundsTriangles, BoundsVertices, sizeof(VolumetricSmokeQuads::Vertex));
		Effect->EndPass();
		Effect->End();
	}

	// 2b. UNOFFICIAL 2026-10-22 (SplatMask): the splats' shapes into the mask atlas, for the cells that need it (BloodFlowBoxes::SplatMask),
	// from the paths just found. The atlas is made the first time (R16F, drawn into and filtered: else the mask stays off).
	static GpuTimer maskTimer("  Blood splat mask");
	D3DXHANDLE maskBake = TechniqueHandle(maskBakeCache, "BloodSplatMaskBake");
	bool masked = Settings.SplatMask && Settings.Splat > 0.0f && !MaskFailed && maskBake && TechniqueHandle(splatMaskCache, "BloodSplatMask");
	if (masked && !MaskTexture) {
		IDirect3D9* d3d = nullptr;
		D3DDEVICE_CREATION_PARAMETERS creation = {};
		D3DDISPLAYMODE mode = {};
		bool usable = false;
		if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
			if (SUCCEEDED(device->GetCreationParameters(&creation)) && SUCCEEDED(device->GetDisplayMode(0, &mode)))
				usable = SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
					D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_R16F));
			d3d->Release();
		}
		if (!usable || FAILED(device->CreateTexture(BloodFlowBoxes::SplatMask::AtlasWidth, BloodFlowBoxes::SplatMask::AtlasHeight, 1, D3DUSAGE_RENDERTARGET,
				D3DFMT_R16F, D3DPOOL_DEFAULT, &MaskTexture, NULL))) {
			MaskTexture = nullptr;
			MaskFailed = true;
			masked = false;
		}
		else {
			MaskTexture->GetSurfaceLevel(0, &MaskSurface);
			Mask.Reset();
		}
		Logger::Log("BloodFlow: splat shapes' mask atlas (%d x %d, R16F): %s", BloodFlowBoxes::SplatMask::AtlasWidth, BloodFlowBoxes::SplatMask::AtlasHeight,
			masked ? "made" : "NOT made (the card cannot draw into or filter R16F); splat shapes worked out per pixel");
	}
	if (masked) {
		MaskConstants = D3DXVECTOR4(1.0f, BloodFlowBoxes::SplatMask::E(Settings.SplatReach), BloodFlowBoxes::SplatMask::D(Settings.SplatReach, Settings.SplatDrips),
			(float)BloodFlowBoxes::SplatMask::CellsPerRow);
		MaskSizeConstants = D3DXVECTOR4(1.0f / BloodFlowBoxes::SplatMask::AtlasWidth, 1.0f / BloodFlowBoxes::SplatMask::AtlasHeight, (float)BloodFlowBoxes::SplatMask::CellSize, 0.0f);
		const int bakeTriangles = Mask.Update(Items, Count, Settings.SplatReach, Settings.SplatDrips, eye, rh, TheRenderManager->projMatrix._22, MaskBakeVertices);
		static int loggedMask = 0;
		if (loggedMask < 5 && Mask.baked > 0) { loggedMask++; Logger::Log("BloodFlow: splat mask: %d cells worked out, %d splats drawn from their cell", Mask.baked, Mask.masked); }
		if (bakeTriangles) {
			device->SetRenderTarget(0, MaskSurface);
			device->SetDepthStencilSurface(nullptr);	// (the atlas is bigger than the game's depth-stencil)
			Effect->SetTechnique(maskBake);
			SetCT();
			// (Not bound to its own sampler while drawn into: TESR_BloodSplatMask is the 13th sampler declared, register 12.)
			device->SetTexture(12, nullptr);
			TheRenderManager->renderState->NoteDeviceTexture(12, nullptr);
			if (SUCCEEDED(Effect->Begin(&passes, 0))) {
				GpuProfileScope gpu(maskTimer, device);
				Effect->BeginPass(0);
				device->SetVertexDeclaration(Declaration);
				device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, bakeTriangles, MaskBakeVertices, sizeof(VolumetricSmokeQuads::Vertex));
				Effect->EndPass();
				Effect->End();
			}
		}
	}
	else MaskConstants.x = 0.0f;
	device->SetDepthStencilSurface(previousDepth);

	// 3. Each item's box (BloodFlowBoxes.h), projected in NVR's camera-centred space as VolumetricSmoke does. Its rows
	// were written just now, so none is fresh for this pass.
	for (int i = 0; i < Count; i++) Items[i].Fresh = false;
	float bounds[4];
	int splatTriangles = 0;
	const int triangles = BloodFlowBoxes::Build(Items, Count, Settings.DripLength, Settings.MaxFall, eye, D3DXVECTOR3(inv._41, inv._42, inv._43),
		TheRenderManager->ViewProjMatrix, Vertices, bounds, &splatTriangles);
	// UNOFFICIAL 2026-10-13 (perf): the vertex shader projects the boxes, cut down to the items' bounds just drawn (CutBox),
	// reading the bounds texture from the first vertex sampler: when the card can read that format there.
	static int vertexBounds = -1;
	if (vertexBounds < 0) {
		vertexBounds = 0;
		IDirect3D9* d3d = nullptr;
		D3DDEVICE_CREATION_PARAMETERS creation = {};
		D3DDISPLAYMODE mode = {};
		if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d) {
			if (SUCCEEDED(device->GetCreationParameters(&creation)) && SUCCEEDED(device->GetDisplayMode(0, &mode)))
				vertexBounds = SUCCEEDED(d3d->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType, mode.Format,
					D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F)) ? 1 : 0;
			d3d->Release();
		}
		Logger::Log("BloodFlow: boxes cut to the blood's bounds on the GPU: %s", vertexBounds ? "yes" : "no (the card cannot read the bounds format in a vertex shader)");
	}
	BloodFlowShape = D3DXVECTOR4(inv._41, inv._42, inv._43, vertexBounds && BoundsTexture ? 1.0f : 0.0f);
	auto bindVertexBounds = [&](bool bind) {
		if (!vertexBounds) return;
		device->SetTexture(D3DVERTEXTEXTURESAMPLER0, bind ? BoundsTexture : nullptr);
		if (!bind) return;
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	};
	const float minU = bounds[0], minV = bounds[1], maxU = bounds[2], maxV = bounds[3];

	// The shader blends straight into the current image (the chain's current texture, or the game's target without the
	// chain), reading the image as it was from TESR_SourceBuffer: only the area the boxes cover is copied there.
	FrameChain& chain = TheShaderManager->Chain;
	const bool chained = chain.Owns(target, rendered);
	IDirect3DSurface9* image = chained ? TheTextureManager->RenderedSurface : target;
	RECT area;
	area.left = (LONG)max(0.0f, floorf(std::clamp(minU, 0.0f, 1.0f) * rw) - 2.0f);
	area.top = (LONG)max(0.0f, floorf(std::clamp(minV, 0.0f, 1.0f) * rh) - 2.0f);
	area.right = (LONG)min(rw, ceilf(std::clamp(maxU, 0.0f, 1.0f) * rw) + 2.0f);
	area.bottom = (LONG)min(rh, ceilf(std::clamp(maxV, 0.0f, 1.0f) * rh) + 2.0f);
	if (triangles && area.right > area.left && area.bottom > area.top) {
		if (source) device->StretchRect(image, &area, source, &area, D3DTEXF_NONE);
		device->SetRenderTarget(0, image);
		RenderRect::ApplyViewport(device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the boxes project into the drawn part
		Effect->SetTechnique(boxes);
		SetCT();
		if (SUCCEEDED(Effect->Begin(&passes, 0))) {
			GpuProfileScope gpu(paintTimer, device);
			Effect->BeginPass(0);
			bindVertexBounds(true);
			device->SetVertexDeclaration(Declaration);
			device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triangles, Vertices, sizeof(VolumetricSmokeQuads::Vertex));
			Effect->EndPass();
			Effect->End();
		}
		// UNOFFICIAL 2026-10-14: each splat's own impact shape (arms, spray, drips: BloodFlow.fx.hlsl SplatPS), over the streams,
		// under the beads. Over the splats only (their triangles come first).
		// (2026-10-22: SplatMask, the shapes from the mask atlas: BloodSplatMask, its vertices carrying each splat's cell; SplatFast,
		// the same picture cheaper: BloodSplatFast; off, or the shader older than that, the old way.)
		D3DXHANDLE splatShape = masked ? TechniqueHandle(splatMaskCache, "BloodSplatMask") : (D3DXHANDLE)nullptr;
		VolumetricSmokeQuads::Vertex* splatVertices = Vertices;
		if (splatShape) {
			memcpy(SplatVertices, Vertices, sizeof(VolumetricSmokeQuads::Vertex) * splatTriangles * 3);
			Mask.Mark(SplatVertices, splatTriangles * 3);
			splatVertices = SplatVertices;
		}
		if (!splatShape && Settings.SplatFast) splatShape = TechniqueHandle(splatFastCache, "BloodSplatFast");
		if (!splatShape) splatShape = TechniqueHandle(splatCache, "BloodSplat");
		if (splatShape && Settings.Splat > 0.0f && splatTriangles > 0) {
			Effect->SetTechnique(splatShape);
			if (SUCCEEDED(Effect->Begin(&passes, 0))) {
				GpuProfileScope gpu(splatTimer, device);
				Effect->BeginPass(0);
				device->SetVertexDeclaration(Declaration);
				device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, splatTriangles, splatVertices, sizeof(VolumetricSmokeQuads::Vertex));
				Effect->EndPass();
				Effect->End();
			}
		}
		// UNOFFICIAL 2026-10-13: the wet beads over the game's splats, on top of the blood (BloodFlow.fx.hlsl, BeadsPS).
		D3DXHANDLE beads = TechniqueHandle(beadsCache, "BloodBeads");
		// (Over the splats only: their triangles come first. The pools' boxes only ever discarded there.)
		if (beads && Settings.Beads > 0.0f && Settings.SplatterAtlas && splatTriangles > 0) {
			Effect->SetTechnique(beads);
			if (SUCCEEDED(Effect->Begin(&passes, 0))) {
				GpuProfileScope gpu(beadsTimer, device);
				Effect->BeginPass(0);
				bindVertexBounds(true);
				device->SetVertexDeclaration(Declaration);
				device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, splatTriangles, Vertices, sizeof(VolumetricSmokeQuads::Vertex));
				Effect->EndPass();
				Effect->End();
			}
		}
	}
	bindVertexBounds(false);
	// UNOFFICIAL 2026-11-12 Surface (BloodSurface.h): its patches' steps and their blood, over what is left of BloodFlow's paint.
	bool surfaceDrew = false;
	if (SurfaceSettings.Enabled && !SurfaceFailed && SurfaceAtlas.Any())
		surfaceDrew = RenderSurface(this, device, image, source, previousDepth, eye, inv, rw, rh, Settings.MaxDistance, Declaration);
	// UNOFFICIAL 2026-10-26 BloodFluid (BloodFluid.h): the simulated blood, over BloodFlow's paint (NVR DXVK only).
	static GpuTimer fluidTimer("  Blood fluid");
	bool fluidDrew = false;
	// UNOFFICIAL 2026-10-27 RenderScale v2: not in rect frames (its tiles and draw cover the whole screen); the simulation waits.
	// (2026-10-29 WaterGun, user: "nothing happens" on FSR 0.667 + LowResEffects = every frame a rect frame: the fluid never ran.
	// It now works in the drawn top-left part: BloodFluid.cpp Render, screen = render size, UVs over the full texture.)
	if (TheBloodFluid().On()) {
		GpuProfileScope gpu(fluidTimer, device);
		fluidDrew = TheBloodFluid().Render(device, image, source, this);
	}
	if (previousDeclaration) device->SetVertexDeclaration(previousDeclaration);
	device->SetStreamSource(0, previousStream, previousOffset, previousStride);
	device->SetRenderTarget(0, previousTarget);
	if (previousTarget) previousTarget->Release();
	if (previousDepth) previousDepth->Release();
	if (previousDeclaration) previousDeclaration->Release();
	if (previousStream) previousStream->Release();
	// Without the chain the rendered texture is kept equal to the target.
	if ((fluidDrew || surfaceDrew) && !chained && rendered && rendered != target)	// (surfaceDrew: UNOFFICIAL 2026-11-12 Surface)
		device->StretchRect(target, NULL, rendered, NULL, D3DTEXF_NONE);
	else if (triangles && area.right > area.left && area.bottom > area.top && !chained && rendered && rendered != target)
		device->StretchRect(target, &area, rendered, &area, D3DTEXF_NONE);
}
