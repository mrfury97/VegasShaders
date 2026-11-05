#pragma once
// UNOFFICIAL 2026-11-03 PipboyFit (user: 'dynamically shrink the conflicting mesh area to below the arm mounted pipboy'): in first person,
// armor sleeves poke through the arm-mounted Pip-Boy. After skinning, the vertex shaders (Includes/PipboyFit.hlsl, compiled in with
// PIPBOY_FIT_ON when [Shaders.PipboyFit.Main] Enabled is on at startup) pull the left forearm's vertices that lie outside an ellipse
// around the forearm in towards it, so the sleeve sits under the casing. The ellipse (a capsule along the forearm) lives in the frame of
// the bone "Bip01 L ForeTwist", which the Pip-Boy casing hangs from; the settings give it in that frame.
// Bare skin is never fitted (user: 'skip bare skin'): only ObjectTemplate and EnvTemplate carry the code, not SkinTemplate -- the casing
// fits the bare arm, and squeezing it thinned the visible forearm next to the casing's elbow end by up to ~0.9 units.
// Third person (ThirdPerson; user: 'just do player only'): only meshes under the player's own body (Player->GetNode()), never an NPC's.
// The third-person skeleton and casing (pipboyarmnpc.nif) measure the same in that frame (wrist at x 18.1, casing x 4.6..17.0), so
// one set of values serves both.
// Power armor (PowerArmor; user: 'add a toggle to PA armor'): while the player wears a power armor torso (the game's own flag,
// MiddleHighProcess::IsWearingPowerArmorTorso 0x8D8290) nothing is fitted unless PowerArmor is on: the bulky gauntlets would be crushed.
// Per skinned partition draw (ClothWind::DrawPartition calls ForPartition), c182-c187:
//   c182 xyz: palette row offsets (slot * 3) of L ForeTwist, L Forearm, L Hand in this partition (-1: not in it); only vertices weighted
//             to them move; w: the radius scale at the wrist end (the casing narrows over its last 4 units)
//   c183-c185: the shader's skinned space -> the capsule frame (rows of s R | t; x along the forearm, the ellipse centred on y = z = 0)
//   c186: x on, y strength, z knee (fraction of the radius where the soft clamp starts), w end falloff (units)
//   c187: x start, y end (along the forearm), z radius y, w radius z
// The shader's space is found as ClothWind::WalkConstants does: the palette's first bone as the game just uploaded it, against the same
// bone worked out here, so it does not matter which space the game skins into.
#include <string>
#include <vector>
#include <cmath>
#include <cctype>
#include <algorithm>

namespace PipboyFit {

	struct Settings {
		bool CompiledIn = false;	// Enabled at startup (PIPBOY_FIT_ON in the shaders)
		bool On = false;
		bool ThirdPerson = true;	// the player's own body in third person too
		bool PowerArmor = false;	// also fit while wearing power armor
		int Axis = 0;				// which axis of the ForeTwist bone runs along the forearm (0 x, 1 y, 2 z)
		float Strength = 1.0f, Knee = 0.7f, Falloff = 2.0f;
		float Start = 4.5f, End = 17.0f, CenterY = 0.1f, CenterZ = 0.4f, RadiusY = 2.6f, RadiusZ = 2.6f, WristScale = 0.6f;	// measured on pipboyarm.nif (ForeTwist frame, wrist at x 18.1)
		std::vector<std::string> Exclude;	// geometry names holding one of these are never squeezed (the Pip-Boy itself)
	};
	inline Settings S;
	inline bool CompiledInRead = false;
	inline bool HookOn = false;				// Hooks.cpp patched the game's partition draws (shared with the cloth)
	inline NiGeometry* Active = nullptr;	// the skinned object being drawn, when it is squeezed
	inline int ActiveBone[3] = { -1, -1, -1 };	// its skin bone indices of L ForeTwist, L Forearm, L Hand
	inline UInt32 StatDraws = 0;
	inline int Logged = 0;
	inline bool ActiveFirst = false;		// the object being drawn is the first-person one
	inline UInt32 Draws[2] = {}, Skipped = 0, Frames = 0;	// partitions fitted (third, first person) and left alone (no forearm bone in them)
	inline int ChecksLogged[2] = {};
	inline bool InPowerArmor = false;		// the player wears a power armor torso (this frame)

	inline std::string Lower(const char* s) { std::string r = s ? s : ""; for (char& c : r) c = (char)std::tolower((unsigned char)c); return r; }

	inline void BeginFrame() {
		if (!TheSettingManager) return;
		const char* section = "Shaders.PipboyFit.Main";
		if (!CompiledInRead) {
			CompiledInRead = true;
			S.CompiledIn = TheSettingManager->GetSettingI(section, "Enabled") != 0;
			S.Exclude.clear();
			char buf[256] = {};
			std::string list = Lower(TheSettingManager->GetSettingS(section, "Exclude", buf));
			size_t p = 0;
			while (p <= list.size()) {
				size_t q = list.find(',', p); if (q == std::string::npos) q = list.size();
				std::string w = list.substr(p, q - p);
				w.erase(0, w.find_first_not_of(" \t")); if (!w.empty()) w.erase(w.find_last_not_of(" \t") + 1);
				if (!w.empty()) S.Exclude.push_back(w);
				p = q + 1;
			}
			Logger::Log("UNOFFICIAL PipboyFit: %s", S.CompiledIn ? (HookOn ? "on (sleeves are fitted under the Pip-Boy in first person)" : "on, but the partition draws are not hooked: nothing is fitted") : "off");
		}
		S.On = S.CompiledIn && HookOn && TheSettingManager->GetSettingI(section, "Enabled") != 0;
		if (S.On && ++Frames % 1800 == 0) {	// about every 30 s at 60 fps: what was fitted since the last line
			Logger::Log("UNOFFICIAL PipboyFit: last 1800 frames: %u partitions fitted in first person, %u in third person (yours), %u without a forearm bone", Draws[1], Draws[0], Skipped);
			Draws[0] = Draws[1] = Skipped = 0;
		}
		if (!S.On) return;
		S.ThirdPerson = TheSettingManager->GetSettingI(section, "ThirdPerson") != 0;
		S.PowerArmor = TheSettingManager->GetSettingI(section, "PowerArmor") != 0;
		const bool wasInPowerArmor = InPowerArmor;
		InPowerArmor = Player && Player->process && ((bool(__thiscall*)(BaseProcess*))0x008D8290)(Player->process);	// the player's process is always a high one
		if (InPowerArmor != wasInPowerArmor) Logger::Log("UNOFFICIAL PipboyFit: power armor %s (%s)", InPowerArmor ? "on" : "off", InPowerArmor && !S.PowerArmor ? "sleeves not fitted, PowerArmor is off" : "sleeves fitted");
		S.Axis = std::clamp(TheSettingManager->GetSettingI(section, "Axis"), 0, 2);
		S.Strength = std::clamp(TheSettingManager->GetSettingF(section, "Strength"), 0.0f, 1.0f);
		S.Knee = std::clamp(TheSettingManager->GetSettingF(section, "Knee"), 0.0f, 0.98f);
		S.Falloff = (std::max)(TheSettingManager->GetSettingF(section, "Falloff"), 0.01f);
		S.Start = TheSettingManager->GetSettingF(section, "Start");
		S.End = (std::max)(TheSettingManager->GetSettingF(section, "End"), S.Start + 0.1f);
		S.CenterY = TheSettingManager->GetSettingF(section, "CenterY");
		S.CenterZ = TheSettingManager->GetSettingF(section, "CenterZ");
		S.RadiusY = (std::max)(TheSettingManager->GetSettingF(section, "RadiusY"), 0.1f);
		S.RadiusZ = (std::max)(TheSettingManager->GetSettingF(section, "RadiusZ"), 0.1f);
		S.WristScale = std::clamp(TheSettingManager->GetSettingF(section, "WristScale"), 0.2f, 1.5f);
	}

	// Is the object part of the player's third-person body?
	inline bool OnPlayer(const NiAVObject* o) {
		const NiNode* body = Player ? Player->GetNode() : nullptr;
		if (!body) return false;
		for (int depth = 0; o && depth < 64; depth++, o = o->m_parent) if (o == body) return true;
		return false;
	}

	// Before a skinned object's pass: squeeze it or not (first person; with ThirdPerson also the player's own body, never an NPC's;
	// never the Pip-Boy itself).
	inline void ForGeometry(NiGeometry* geometry, bool firstPerson) {
		Active = nullptr;
		if (!S.On || !geometry || !geometry->skinInstance) return;
		if (InPowerArmor && !S.PowerArmor) return;	// only the player is ever fitted, so the player's armor decides
		if (!firstPerson && (!S.ThirdPerson || !OnPlayer(geometry))) return;
		const NiSkinInstance* si = geometry->skinInstance;
		const NiSkinData* sd = si->SkinData;
		if (!sd || !si->BoneObjects) return;
		const std::string name = Lower(geometry->m_pcName);
		for (const std::string& w : S.Exclude) if (name.find(w) != std::string::npos) return;
		static const char* const bones[3] = { "Bip01 L ForeTwist", "Bip01 L Forearm", "Bip01 L Hand" };
		int found[3] = { -1, -1, -1 };
		for (UInt32 b = 0; b < sd->Bones; b++) {
			const NiAVObject* o = si->BoneObjects[b];
			if (!o || !o->m_pcName) continue;
			for (int k = 0; k < 3; k++) if (found[k] < 0 && !_stricmp(o->m_pcName, bones[k])) found[k] = (int)b;
		}
		if (found[0] < 0) return;	// not on the left forearm
		for (int k = 0; k < 3; k++) ActiveBone[k] = found[k];
		Active = geometry;
		ActiveFirst = firstPerson;
		if (Logged < 8) { Logged++; Logger::Log("UNOFFICIAL PipboyFit: fitting '%s' under the Pip-Boy (%s)", geometry->m_pcName ? geometry->m_pcName : "?", firstPerson ? "first person" : "third person"); }
	}
	inline void AfterDraw() { Active = nullptr; }

	inline void Off(IDirect3DDevice9* device) {
		static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		device->SetVertexShaderConstantF(186, zero, 1);
	}

	// Bone b of the skin as a node: its world transform as rows r | t (bone space -> world).
	inline void NodeXf(const NiAVObject* o, double out[3][4]) {
		const NiTransform& W = o->m_worldTransform;
		for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) out[i][j] = (double)W.rot.data[i][j] * W.scale; out[i][3] = W.pos.x * (i == 0) + W.pos.y * (i == 1) + W.pos.z * (i == 2); }
	}
	inline bool Invert(const double M[3][4], double out[3][4]) {
		const double a = M[0][0], b = M[0][1], c = M[0][2], d = M[1][0], e = M[1][1], f = M[1][2], g = M[2][0], h = M[2][1], i = M[2][2];
		const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
		if (std::fabs(det) < 1e-12) return false;
		const double inv[3][3] = { { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det },
								   { (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det },
								   { (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det } };
		for (int r = 0; r < 3; r++) { for (int q = 0; q < 3; q++) out[r][q] = inv[r][q]; out[r][3] = -(inv[r][0] * M[0][3] + inv[r][1] * M[1][3] + inv[r][2] * M[2][3]); }
		return true;
	}
	inline void Mul(const double A[3][4], const double B[3][4], double out[3][4]) {
		for (int r = 0; r < 3; r++) for (int q = 0; q < 4; q++) out[r][q] = A[r][0] * B[0][q] + A[r][1] * B[1][q] + A[r][2] * B[2][q] + (q == 3 ? A[r][3] : 0.0);
	}

	// Right before the game draws one skinned partition (its bone palette is already in c44+): this partition's constants.
	inline void ForPartition(IDirect3DDevice9* device, const NiSkinPartition::Partition* part) {
		if (!S.CompiledIn || !device) return;
		if (!Active || !part || !part->pBones || !part->Bones) { Off(device); return; }
		const NiSkinInstance* si = Active->skinInstance;
		const NiSkinData* sd = si ? si->SkinData : nullptr;
		const UInt16 first = part->pBones[0];
		if (!sd || !sd->BoneData || first >= sd->Bones || !si->BoneObjects[first] || !si->BoneObjects[ActiveBone[0]]) { Off(device); return; }
		float slots[4] = { -1.0f, -1.0f, -1.0f, S.WristScale };
		bool any = false;
		for (UInt16 sl = 0; sl < part->Bones; sl++)
			for (int k = 0; k < 3; k++) if (ActiveBone[k] >= 0 && part->pBones[sl] == ActiveBone[k]) { slots[k] = sl * 3.0f; any = true; }
		if (!any) { Skipped++; Off(device); return; }	// no forearm vertices in this partition
		// world -> the shader's space: T = (palette bone 0 as uploaded) x (the same bone here: its world transform after its SkinToBone)^-1
		double B[3][4], S2B[3][4], Bw[3][4], Bi[3][4], T[3][4];
		NodeXf(si->BoneObjects[first], B);
		const NiTransform& st = sd->BoneData[first].SkinToBone;
		for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) S2B[i][j] = (double)st.rot.data[i][j] * st.scale; }
		S2B[0][3] = st.pos.x; S2B[1][3] = st.pos.y; S2B[2][3] = st.pos.z;
		Mul(B, S2B, Bw);
		if (!Invert(Bw, Bi)) { Off(device); return; }
		float R[12];
		if (FAILED(device->GetVertexShaderConstantF(44, R, 3))) { Off(device); return; }
		const double P[3][4] = { { R[0], R[1], R[2], R[3] }, { R[4], R[5], R[6], R[7] }, { R[8], R[9], R[10], R[11] } };
		Mul(P, Bi, T);
		// the ForeTwist bone's frame -> the shader's space, then inverted: the shader's space -> the bone's frame
		double F[3][4], M[3][4], L[3][4];
		NodeXf(si->BoneObjects[ActiveBone[0]], F);
		Mul(T, F, M);
		if (!Invert(M, L)) { Off(device); return; }
		// the capsule frame: x along the forearm (the Axis setting), the ellipse centre on y = z = 0
		const int ax[3] = { S.Axis, (S.Axis + 1) % 3, (S.Axis + 2) % 3 };
		float k[24] = {};
		for (int r = 0; r < 3; r++) for (int q = 0; q < 4; q++) k[4 + r * 4 + q] = (float)L[ax[r]][q];
		k[4 + 4 + 3] -= S.CenterY; k[4 + 8 + 3] -= S.CenterZ;
		memcpy(k, slots, sizeof(slots));
		k[16] = 1.0f; k[17] = S.Strength; k[18] = S.Knee; k[19] = S.Falloff;
		k[20] = S.Start; k[21] = S.End; k[22] = S.RadiusY; k[23] = S.RadiusZ;
		device->SetVertexShaderConstantF(182, k, 6);
		StatDraws++;
		Draws[ActiveFirst ? 1 : 0]++;
		// The check that the shader's space is right: the ForeTwist bone's palette rows as the game uploaded them against the same rows
		// worked out here (T x its world transform x its SkinToBone). Near 0 = the capsule sits on the forearm.
		if (slots[0] >= 0.0f && ChecksLogged[ActiveFirst ? 1 : 0] < 3) {
			ChecksLogged[ActiveFirst ? 1 : 0]++;
			double FS[3][4], FT[3][4], E[3][4];
			const NiTransform& ft = sd->BoneData[ActiveBone[0]].SkinToBone;
			for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) FS[i][j] = (double)ft.rot.data[i][j] * ft.scale; }
			FS[0][3] = ft.pos.x; FS[1][3] = ft.pos.y; FS[2][3] = ft.pos.z;
			Mul(F, FS, FT); Mul(T, FT, E);
			float G[12];
			if (SUCCEEDED(device->GetVertexShaderConstantF(44 + (UINT)slots[0], G, 3))) {
				float worst = 0.0f;
				for (int r = 0; r < 3; r++) for (int q = 0; q < 4; q++) worst = (std::max)(worst, (float)std::fabs(G[r * 4 + q] - E[r][q]));
				const double s = std::sqrt(L[0][0] * L[0][0] + L[0][1] * L[0][1] + L[0][2] * L[0][2]);
				Logger::Log("UNOFFICIAL PipboyFit: space check (%s, '%s', partition of %u bones, ForeTwist in slot %d): palette vs computed largest difference %.4f (translation %.1f %.1f %.1f); shader units per bone unit %.3f",
					ActiveFirst ? "first person" : "third person", Active->m_pcName ? Active->m_pcName : "?", (unsigned)part->Bones, (int)(slots[0] / 3.0f), worst, G[3], G[7], G[11], s > 1e-9 ? 1.0 / s : 0.0);
			}
		}
	}
}
