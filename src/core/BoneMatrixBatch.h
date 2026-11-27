#pragma once

// UNOFFICIAL 2026-11-24 BoneMatrixBatch ([Main.Main.Performance] BatchBoneUploads, live, default true):
// NiD3DVertexShader::SetShaderConstantArray (0x00E89C70, vtable 0x010EF8F4) gathers each skin partition's
// bone matrices one entry at a time and calls IDirect3DDevice9::SetVertexShaderConstantF once per bone per
// partition per pass. For GPU constant-array uploads (shader register starting at pkEntry+0x1C, up to 256
// registers total), gathering the reordered matrices into one contiguous stack buffer sends all bones for
// that partition in a single SetVertexShaderConstantF call (also going through ConstantFilter once instead
// of N times). Identical GPU register state; preserves original fallback paths for scalar/1-entry uploads
// and software-defined (0x20000000 + non-null m_pvDataSource) entries.

#include <cstring>
#include "GpuProfiler.h"

namespace BoneMatrixBatch {
	inline bool Enabled = true;
	inline bool Installed = false;

	using SetShaderConstantArrayFn = bool(__thiscall*)(
		void* This,
		void* pkEntry,
		const void* pvDataSource,
		UInt32 uiNumEntries,
		UInt32 uiRegistersPerEntry,
		const UInt16* pusReorderArray);
	inline SetShaderConstantArrayFn Original = nullptr;

	static constexpr UInt32 kVtableSlot = 0x010EF8F4;       // NiD3DVertexShader::SetShaderConstantArray
	static constexpr UInt32 kExpectedOriginal = 0x00E89C70; // NiD3DVertexShader::SetShaderConstantArray
	static constexpr UInt32 kAttribTypeTable = 0x011F5FC4;  // NiShaderConstantMapEntry::ms_aeAttribType

	// F10 statistics (reset on 240-frame window or when F10 profiling is disabled).
	inline unsigned StatFrames = 0;
	inline unsigned long long StatCalls = 0;
	inline unsigned long long StatSavedD3DCalls = 0;
	inline unsigned long long StatTotalBones = 0;

	inline void ResetStats() {
		StatFrames = 0;
		StatCalls = 0;
		StatSavedD3DCalls = 0;
		StatTotalBones = 0;
	}

	inline bool __fastcall Hook(
		void* This,
		UInt32 /*edx*/,
		void* pkEntry,
		const void* pvDataSource,
		UInt32 uiNumEntries,
		UInt32 uiRegistersPerEntry,
		const UInt16* pusReorderArray)
	{
		if (!Enabled || !pkEntry || !pvDataSource || !pusReorderArray || uiNumEntries <= 1) {
			return Original(This, pkEntry, pvDataSource, uiNumEntries, uiRegistersPerEntry, pusReorderArray);
		}

		const UInt32 flags = *(const UInt32*)((const UInt8*)pkEntry + 0x14);
		const UInt32 attribType = ((const UInt32*)kAttribTypeTable)[flags & 0xFFu];
		if (attribType != 8 && attribType != 9) {
			return false;
		}

		// Preserve mapped-defined software copy path when m_pvDataSource (+0x30) is populated.
		if ((flags & 0xF0000000u) == 0x20000000u && *(const void* const*)((const UInt8*)pkEntry + 0x30) != nullptr) {
			return Original(This, pkEntry, pvDataSource, uiNumEntries, uiRegistersPerEntry, pusReorderArray);
		}

		const UInt32 startReg = *(const UInt32*)((const UInt8*)pkEntry + 0x1C);
		if (uiRegistersPerEntry == 0 || uiRegistersPerEntry > 4 || uiNumEntries > 64 ||
			startReg + uiNumEntries * uiRegistersPerEntry > 256u) {
			return Original(This, pkEntry, pvDataSource, uiNumEntries, uiRegistersPerEntry, pusReorderArray);
		}

		const UInt8* renderState = *(const UInt8* const*)((const UInt8*)This + 0x28);
		IDirect3DDevice9* device = renderState ? *(IDirect3DDevice9* const*)(renderState + 0x10F8) : nullptr;
		if (!device) {
			return Original(This, pkEntry, pvDataSource, uiNumEntries, uiRegistersPerEntry, pusReorderArray);
		}

		alignas(16) float buf[256 * 4];
		const UInt32 floatsPerEntry = uiRegistersPerEntry * 4u;
		const size_t bytesPerEntry = (size_t)floatsPerEntry * sizeof(float);
		const float* src = (const float*)pvDataSource;
		float* dst = buf;
		for (UInt32 i = 0; i < uiNumEntries; ++i) {
			memcpy(dst, src + (size_t)pusReorderArray[i] * floatsPerEntry, bytesPerEntry);
			dst += floatsPerEntry;
		}

		device->SetVertexShaderConstantF(startReg, buf, uiNumEntries * uiRegistersPerEntry);

		if (GpuTimer::Enabled) {
			StatCalls++;
			StatSavedD3DCalls += (uiNumEntries - 1u);
			StatTotalBones += uiNumEntries;
		}
		return true;
	}

	inline void Install() {
		const UInt32 cur = *(const UInt32*)kVtableSlot;
		if (!cur) {
			Logger::Log("[WARN] BoneMatrixBatch: vtable slot %08X is null; skipped", kVtableSlot);
			return;
		}
		if (cur == (UInt32)&Hook) {
			Installed = true;
			return;
		}
		Original = (SetShaderConstantArrayFn)cur;
		SafeWrite32(kVtableSlot, (UInt32)&Hook);
		Installed = true;
		if (cur != kExpectedOriginal) {
			Logger::Log("UNOFFICIAL BoneMatrixBatch: chained NiD3DVertexShader::SetShaderConstantArray (%08X -> %08X)", kVtableSlot, cur);
		} else {
			Logger::Log("UNOFFICIAL BoneMatrixBatch: hooked NiD3DVertexShader::SetShaderConstantArray (%08X -> %08X)", kVtableSlot, cur);
		}
	}

	inline void OnFrameEnd(bool statsOn) {
		if (!statsOn) {
			if (StatFrames) ResetStats();
			return;
		}
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		const double callsPerFrame = StatCalls / f;
		const double savedPerFrame = StatSavedD3DCalls / f;
		const double avgBones = StatCalls ? (StatTotalBones / (double)StatCalls) : 0.0;
		Logger::Log("BONE BATCH: %s | %.1f batched uploads/frame, %.1f D3D calls saved/frame (avg %.1f bones/upload)",
			Enabled ? "on" : "off", callsPerFrame, savedPerFrame, avgBones);
		ResetStats();
	}
}
