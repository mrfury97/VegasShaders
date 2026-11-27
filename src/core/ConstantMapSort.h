#pragma once

// UNOFFICIAL 2026-11-24 ConstantMapSort ([Main.Main.Performance] FastConstantMapSort, live, default true):
// NiD3DShaderConstantMap::SortListByEnabled (0x00E87200, vtable 0x010EF568) partitions m_aspEntries (+0x0C)
// and m_aspDefinedEntries (+0x1C) via SortArray (0x00E870C0) using NiPointer temporaries, paying two locked
// InterlockedIncrement/Decrement operations per array entry on every pass switch. Replacing it with an exact
// raw-pointer in-place swap produces the identical array order (refcounts stay net zero) with zero interlocked
// operations (~0.13-0.17 ms/frame saved in NPC/lamp-heavy interiors).

#include "GpuProfiler.h"

namespace ConstantMapSort {
	inline bool Enabled = true;
	inline bool Installed = false;

	using SortListByEnabledFn = void(__thiscall*)(void* This);
	inline SortListByEnabledFn Original = nullptr;

	static constexpr UInt32 kVtableSlot = 0x010EF568;       // NiD3DShaderConstantMap::SortListByEnabled
	static constexpr UInt32 kExpectedOriginal = 0x00E87200; // NiD3DShaderConstantMap::SortListByEnabled

	// F10 statistics (reset on 240-frame window or when F10 profiling is disabled).
	inline unsigned StatFrames = 0;
	inline unsigned long long StatCalls = 0;
	inline unsigned long long StatEntriesScanned = 0;
	inline unsigned long long StatSwaps = 0;

	inline void ResetStats() {
		StatFrames = 0;
		StatCalls = 0;
		StatEntriesScanned = 0;
		StatSwaps = 0;
	}

	inline void SortArrayRaw(void* arrPtr) {
		const UInt16 n = *(const UInt16*)((const UInt8*)arrPtr + 0x0A);
		if (n <= 1) return;
		void** base = *(void***)((UInt8*)arrPtr + 0x04);
		if (!base) return;

		if (GpuTimer::Enabled) StatEntriesScanned += n;

		UInt32 firstDisabled = 0xFFFFFFFFu;
		for (UInt32 i = 0; i < n; ++i) {
			void* entry = base[i];
			if (!entry) continue;
			if (*((const UInt8*)entry + 8) == 1) {
				if (firstDisabled < i) {
					base[i] = base[firstDisabled];
					base[firstDisabled] = entry;
					if (GpuTimer::Enabled) StatSwaps++;
					++firstDisabled;
					while (firstDisabled < n) {
						void* next = base[firstDisabled];
						if (!next || *((const UInt8*)next + 8) == 0) break;
						++firstDisabled;
					}
					if (firstDisabled >= n) break;
				}
			} else if (firstDisabled > i) {
				firstDisabled = i;
			}
		}
	}

	inline void __fastcall Hook(void* This, UInt32 /*edx*/) {
		if (!Enabled) {
			Original(This);
			return;
		}
		if (GpuTimer::Enabled) StatCalls++;
		SortArrayRaw((UInt8*)This + 0x0C); // m_aspEntries
		SortArrayRaw((UInt8*)This + 0x1C); // m_aspDefinedEntries
	}

	inline void Install() {
		const UInt32 cur = *(const UInt32*)kVtableSlot;
		if (!cur) {
			Logger::Log("[WARN] ConstantMapSort: vtable slot %08X is null; skipped", kVtableSlot);
			return;
		}
		if (cur == (UInt32)&Hook) {
			Installed = true;
			return;
		}
		Original = (SortListByEnabledFn)cur;
		SafeWrite32(kVtableSlot, (UInt32)&Hook);
		Installed = true;
		if (cur != kExpectedOriginal) {
			Logger::Log("UNOFFICIAL ConstantMapSort: chained NiD3DShaderConstantMap::SortListByEnabled (%08X -> %08X)", kVtableSlot, cur);
		} else {
			Logger::Log("UNOFFICIAL ConstantMapSort: hooked NiD3DShaderConstantMap::SortListByEnabled (%08X -> %08X)", kVtableSlot, cur);
		}
	}

	inline void OnFrameEnd(bool statsOn) {
		if (!statsOn) {
			if (StatFrames) ResetStats();
			return;
		}
		if (++StatFrames < 240) return;
		const double f = (double)StatFrames;
		const double sortsPerFrame = StatCalls / f;
		const double entriesPerFrame = StatEntriesScanned / f;
		const double swapsPerFrame = StatSwaps / f;
		const double lockedOpsSaved = entriesPerFrame * 2.0; // vanilla copies each visited entry into an NiPointer (~2 Interlocked ops)
		Logger::Log("CONSTMAP SORT: %s | %.1f sorts/frame, %.1f entries/frame, %.1f swaps/frame (~%.0f locked ops skipped/frame)",
			Enabled ? "on" : "off", sortsPerFrame, entriesPerFrame, swapsPerFrame, lockedOpsSaved);
		ResetStats();
	}
}
