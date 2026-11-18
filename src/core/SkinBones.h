#pragma once
// UNOFFICIAL 2026-11-18 (a tester's crash with NV-SMP-Physics): a skin's bone slots are not
// always live nodes. When the game attaches an item it drops the item's own bone nodes from the scene, but the skin keeps pointing at them
// until the game binds the skin to the skeleton, seconds later (after the Pip-Boy closes); those nodes may be freed, or their memory reused
// by something else. NV-SMP-Physics documents this and works around it the same way (github.com/Balebandro/NV-SMP-Physics,
// src/nv/ActorManager.cpp: PlaceDroppedBones, ReadSlot, CopyName, IsLiveBone): it reads slots under SEH and never writes a stale one.
// The crash it caused here: PipboyFit::ForGeometry read the name of such a slot (a mod's "VirtualLegs" shape) and the name pointer was the
// float -17.0 (0xC1880000). Every reader of NiSkinInstance::BoneObjects in Vegas Shaders goes through these two:
//  - Live: the slot's object can be read and its vtable lies in the game's read-only data (.rdata 0x00FDF000-0x01183000 in FalloutNV.exe
//    1.4.0.525, where all of the game's vtables are: NiNode 0x0109B5AC, BSFadeNode 0x010A8F90, ...; the nodes plugins make with the game's
//    constructors too, NV-SMP-Physics' stand-ins among them). A freed or reused slot fails in almost every case and is skipped. It shows the
//    slot looks like a node, not that the node is still in the scene: a dropped node not yet freed passes, and its transform is read as
//    before (garbage for a frame at worst, no fault: the same block as its vtable).
//  - Name: the slot's name copied under SEH, for the readers that match bones by name (false: not live, unreadable, or no name pointer;
//    an empty name is true with "", as reading it directly gave).
// Both are a few instructions (the SEH frame of a 32-bit __try is a handful of stores), called per bone where the bones were read anyway.

namespace SkinBones {
	inline bool Live(const NiAVObject* bone) {
		if (!bone) return false;
		__try {
			const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(bone);
			return vtable >= 0x00FDF000u && vtable < 0x01183000u;	// FalloutNV.exe 1.4.0.525's .rdata (every game vtable)
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// The bone's name into out (size bytes, always terminated); false when it is not a live node, unreadable or unnamed.
	inline bool Name(const NiAVObject* bone, char* out, size_t size) {
		if (!out || !size) return false;
		out[0] = 0;
		if (!Live(bone)) return false;
		__try {
			const char* name = bone->m_pcName;
			if (!name) return false;
			size_t i = 0;
			for (; i + 1 < size && name[i]; i++) out[i] = name[i];
			out[i] = 0;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			out[0] = 0;
			return false;
		}
	}
}
