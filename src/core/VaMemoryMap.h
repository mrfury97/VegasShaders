#pragma once

// UNOFFICIAL 2026-11-23 VaMemoryMap: low-overhead, zero-heap-allocation breakdown of the 32-bit process virtual address
// space (Private vs Mapped vs Image vs Free, largest free block, private block sizes, top DLLs, mapped caches/BSAs, top
// private allocations with heap & module-reference attribution, and thread owners). Safe in CrashFilter / StallThread /
// OnDeferredInit: uses a static BSS workspace under a non-blocking CAS guard, takes no Windows loader lock, and strips
// every path to its file basename before logging.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "TestTools.h"
#if NVR_TEST_TOOLS
#include <tlhelp32.h>
#endif

namespace VaMemoryMap {

	namespace detail {

		struct ImageEntry {
			UINT_PTR	base;
			SIZE_T		size;
			SIZE_T		commit;
			UINT_PTR	rwStart[8];
			SIZE_T		rwSize[8];
			unsigned	rwCount;
			char		name[64];
		};

		struct MappedEntry {
			char		name[64];
			unsigned	count;
			SIZE_T		totalBytes;
			SIZE_T		commitBytes;
			SIZE_T		reserveBytes;
		};

		struct PrivateEntry {
			UINT_PTR	base;
			SIZE_T		totalBytes;
			SIZE_T		commitBytes;
			SIZE_T		reserveBytes;
			bool		hasExec;
			bool		hasWrite;
			bool		isDefaultHeap;
			bool		isWinHeap;
			char		refMod[64];
		};

		struct ThreadModEntry {
			char		name[64];
			unsigned	count;
		};

		struct SizeBucket {
			unsigned	count;
			SIZE_T		bytes;
		};

		struct CurPriv {
			UINT_PTR	base;
			SIZE_T		totalBytes;
			SIZE_T		commitBytes;
			SIZE_T		reserveBytes;
			bool		hasGuard;
			bool		hasExec;
			bool		hasWrite;
		};

		struct Workspace {
			ImageEntry		images[256];
			unsigned		imageCount;

			MappedEntry		mapped[256];
			unsigned		mappedCount;

			PrivateEntry	privates[512];
			unsigned		privateStored;
			SIZE_T			minStoredBytes;
			unsigned		minStoredIdx;

			ThreadModEntry	threadMods[64];
			unsigned		threadModCount;

			HANDLE			winHeaps[128];
			DWORD			winHeapTotal;
			DWORD			winHeapStored;
			HANDLE			defaultHeap;
		};

		inline volatile LONG Busy = 0;
		inline Workspace Ws;

		struct BusyGuard {
			bool acquired;
			BusyGuard() : acquired(InterlockedCompareExchange(&Busy, 1, 0) == 0) {}
			~BusyGuard() { if (acquired) InterlockedExchange(&Busy, 0); }
		};

		inline void CopyBasename(char* out, size_t outCap, const char* path) {
			if (!out || !outCap) return;
			out[0] = '\0';
			if (!path || !path[0]) return;
			const char* slash = strrchr(path, '\\');
			const char* fslash = strrchr(path, '/');
			const char* base = slash > fslash ? slash + 1 : (fslash ? fslash + 1 : path);
			if (!base[0]) base = path;
			strncpy_s(out, outCap, base, _TRUNCATE);
		}

		// SEH-isolated PE export directory name read (no loader lock, no C++ unwinding objects).
		inline bool ReadPeExportName(UINT_PTR base, char* out, size_t outCap) {
			if (!out || !outCap) return false;
			out[0] = '\0';
			__try {
				const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
				if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
				const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
				const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
				if (!dir.VirtualAddress || !dir.Size) return false;
				const IMAGE_EXPORT_DIRECTORY* exp = (const IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
				if (!exp->Name) return false;
				const char* modName = (const char*)(base + exp->Name);
				CopyBasename(out, outCap, modName);
				return out[0] != '\0';
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				out[0] = '\0';
				return false;
			}
		}

		inline bool QueryMappedBasename(void* addr, char* out, size_t outCap) {
			if (!out || !outCap) return false;
			out[0] = '\0';
			typedef DWORD (WINAPI* GetMappedFileNameAFn)(HANDLE, LPVOID, LPSTR, DWORD);
			static GetMappedFileNameAFn fn = nullptr;
			static LONG init = 0;
			if (!init) {
				fn = (GetMappedFileNameAFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetMappedFileNameA");
				if (!fn) {
					if (HMODULE psapi = GetModuleHandleA("psapi.dll"))
						fn = (GetMappedFileNameAFn)GetProcAddress(psapi, "GetMappedFileNameA");
				}
				InterlockedExchange(&init, 1);
			}
			if (!fn) return false;
			char buf[MAX_PATH * 2];
			const DWORD len = fn(GetCurrentProcess(), addr, buf, sizeof(buf) - 1);
			if (!len) return false;
			buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
			CopyBasename(out, outCap, buf);
			return out[0] != '\0';
		}

		inline void ResolveImageName(UINT_PTR base, UINT_PTR exeBase, char* out, size_t outCap) {
			if (exeBase && base == exeBase) {
				strncpy_s(out, outCap, "FalloutNV.exe", _TRUNCATE);
				return;
			}
			if (ReadPeExportName(base, out, outCap)) return;
			if (QueryMappedBasename((void*)base, out, outCap)) return;
			sprintf_s(out, outCap, "module@%08lX", (unsigned long)base);
		}

		// SEH-isolated DWORD scan of one writable module span against target bases (no C++ objects).
		inline void ScanSpanForRefs(UINT_PTR spanStart, SIZE_T spanSize, UINT_PTR skipStart, UINT_PTR skipEnd,
			const UINT_PTR* targets, unsigned targetCount, UINT_PTR minTarget, UINT_PTR maxTarget,
			const char* modName, PrivateEntry* privs) {
			if (!spanStart || spanSize < 4 || !targets || !targetCount || !modName || !modName[0]) return;
			UINT_PTR p = (spanStart + 3u) & ~(UINT_PTR)3u;
			const UINT_PTR end = spanStart + spanSize;
			if (end <= 3 || end <= p) return;
			const UINT_PTR limit = end - 3;
			__try {
				while (p <= limit) {
					if (p >= skipStart && p < skipEnd) {
						p = (skipEnd + 3u) & ~(UINT_PTR)3u;
						continue;
					}
					const UINT_PTR v = *(const UINT_PTR*)p;
					if (v >= minTarget && v <= maxTarget) {
						for (unsigned t = 0; t < targetCount; t++) {
							if (v == targets[t] && !privs[t].refMod[0]) {
								strncpy_s(privs[t].refMod, modName, _TRUNCATE);
							}
						}
					}
					p += 4;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		inline void AddMapped(const char* name, SIZE_T regionBytes, SIZE_T commitBytes, SIZE_T reserveBytes, bool newAlloc) {
			const char* label = (name && name[0]) ? name : "(pagefile/shared)";
			for (unsigned i = 0; i < Ws.mappedCount; i++) {
				if (_stricmp(Ws.mapped[i].name, label) == 0) {
					if (newAlloc) Ws.mapped[i].count++;
					Ws.mapped[i].totalBytes += regionBytes;
					Ws.mapped[i].commitBytes += commitBytes;
					Ws.mapped[i].reserveBytes += reserveBytes;
					return;
				}
			}
			if (Ws.mappedCount >= 256u) return;
			MappedEntry& e = Ws.mapped[Ws.mappedCount++];
			strncpy_s(e.name, label, _TRUNCATE);
			e.count = newAlloc ? 1u : 0u;
			e.totalBytes = regionBytes;
			e.commitBytes = commitBytes;
			e.reserveBytes = reserveBytes;
		}

		inline void RecordPrivateCandidate(const CurPriv& cp) {
			if (cp.totalBytes < (8ULL << 20)) return;
			unsigned slot = 0;
			if (Ws.privateStored < 512u) {
				slot = Ws.privateStored++;
			}
			else {
				if (cp.totalBytes <= Ws.minStoredBytes) return;
				slot = Ws.minStoredIdx;
			}
			PrivateEntry& p = Ws.privates[slot];
			p.base = cp.base;
			p.totalBytes = cp.totalBytes;
			p.commitBytes = cp.commitBytes;
			p.reserveBytes = cp.reserveBytes;
			p.hasExec = cp.hasExec;
			p.hasWrite = cp.hasWrite;
			p.isDefaultHeap = (cp.base == (UINT_PTR)Ws.defaultHeap);
			p.isWinHeap = p.isDefaultHeap;
			if (!p.isWinHeap) {
				for (DWORD i = 0; i < Ws.winHeapStored; i++) {
					if ((UINT_PTR)Ws.winHeaps[i] == cp.base) { p.isWinHeap = true; break; }
				}
			}
			p.refMod[0] = '\0';

			if (Ws.privateStored == 512u) {
				SIZE_T m = Ws.privates[0].totalBytes;
				unsigned mi = 0;
				for (unsigned i = 1; i < 512u; i++) {
					if (Ws.privates[i].totalBytes < m) { m = Ws.privates[i].totalBytes; mi = i; }
				}
				Ws.minStoredBytes = m;
				Ws.minStoredIdx = mi;
			}
		}

#if NVR_TEST_TOOLS
		inline void CollectThreadModules(unsigned& outTotalThreads) {
			outTotalThreads = 0;
			Ws.threadModCount = 0;

			typedef LONG (NTAPI* NtQueryInfoThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
			static NtQueryInfoThreadFn queryThread = nullptr;
			static LONG init = 0;
			if (!init) {
				if (HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
					queryThread = (NtQueryInfoThreadFn)GetProcAddress(ntdll, "NtQueryInformationThread");
				InterlockedExchange(&init, 1);
			}

			const DWORD pid = GetCurrentProcessId();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) return;

			THREADENTRY32 te = {};
			te.dwSize = sizeof(te);
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
				if (te.th32OwnerProcessID != pid) continue;
				outTotalThreads++;
				const char* modName = "(unknown)";
				if (queryThread) {
					HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
					if (th) {
						PVOID startAddr = nullptr;
						if (queryThread(th, 9 /* ThreadQuerySetWin32StartAddress */, &startAddr, sizeof(startAddr), nullptr) == 0 && startAddr) {
							const UINT_PTR addr = (UINT_PTR)startAddr;
							for (unsigned m = 0; m < Ws.imageCount; m++) {
								if (addr >= Ws.images[m].base && addr < Ws.images[m].base + Ws.images[m].size) {
									modName = Ws.images[m].name;
									break;
								}
							}
						}
						CloseHandle(th);
					}
				}
				bool found = false;
				for (unsigned i = 0; i < Ws.threadModCount; i++) {
					if (_stricmp(Ws.threadMods[i].name, modName) == 0) {
						Ws.threadMods[i].count++;
						found = true;
						break;
					}
				}
				if (!found && Ws.threadModCount < 64u) {
					ThreadModEntry& tm = Ws.threadMods[Ws.threadModCount++];
					strncpy_s(tm.name, modName, _TRUNCATE);
					tm.count = 1u;
				}
			}
			CloseHandle(snap);
		}
#endif

		inline void EmitLine(bool unbuffered, const char* line) {
			if (unbuffered) Logger::LogUnbuffered("%s", line);
			else Logger::Log("%s", line);
		}

	} // namespace detail

	// Captures and logs a compact breakdown of the 32-bit virtual address space.
	// reason: short label such as "startup (main menu)", "memory warning", "report", "stall", "crash".
	// unbuffered: true inside the crash filter so nothing touches Logger mutex/ring buffer.
	// scanRefs: scan loaded module writable sections for base pointers matching the top private blocks.
	// includeThreads: enumerate threads via Toolhelp snapshot + NtQueryInformationThread.
	inline void Log(const char* reason, bool unbuffered = false, bool scanRefs = true, bool includeThreads = true) {
		detail::BusyGuard guard;
		if (!guard.acquired) return;

		detail::Workspace& ws = detail::Ws;
		ws.imageCount = 0;
		ws.mappedCount = 0;
		ws.privateStored = 0;
		ws.minStoredBytes = 0;
		ws.minStoredIdx = 0;
		ws.threadModCount = 0;

		ws.defaultHeap = GetProcessHeap();
		ws.winHeapTotal = GetProcessHeaps(128u, ws.winHeaps);
		ws.winHeapStored = ws.winHeapTotal < 128u ? ws.winHeapTotal : 128u;

		const UINT_PTR exeBase = (UINT_PTR)GetModuleHandleA(nullptr);

		SIZE_T freeTotal = 0, largestFree = 0;
		unsigned free16Gaps = 0;
		SIZE_T imgTotal = 0, imgCommit = 0, imgReserve = 0;
		SIZE_T mapTotal = 0, mapCommit = 0, mapReserve = 0;
		SIZE_T privTotal = 0, privCommit = 0, privReserve = 0;
		SIZE_T stackTotal = 0;
		unsigned stackCount = 0;
		detail::SizeBucket hist[4] = {};	// [0]>=64MB, [1] 16-64MB, [2] 4-16MB, [3] <4MB

		detail::CurPriv curPriv = {};
		UINT_PTR curMappedBase = 0;
		char curMappedName[64] = "";

		auto finalizePriv = [&]() {
			if (!curPriv.base) return;
			const bool isStack = curPriv.hasGuard && !curPriv.hasExec;
			if (isStack) {
				stackTotal += curPriv.totalBytes;
				stackCount++;
			}
			else {
				if (curPriv.totalBytes >= (64ULL << 20)) { hist[0].count++; hist[0].bytes += curPriv.totalBytes; }
				else if (curPriv.totalBytes >= (16ULL << 20)) { hist[1].count++; hist[1].bytes += curPriv.totalBytes; }
				else if (curPriv.totalBytes >= (4ULL << 20)) { hist[2].count++; hist[2].bytes += curPriv.totalBytes; }
				else { hist[3].count++; hist[3].bytes += curPriv.totalBytes; }
				detail::RecordPrivateCandidate(curPriv);
			}
			curPriv = {};
		};

		MEMORY_BASIC_INFORMATION mbi = {};
		for (UINT_PTR a = 0x10000u; a < 0xFFFF0000u; ) {
			if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi))) break;
			const UINT_PTR next = (UINT_PTR)mbi.BaseAddress + mbi.RegionSize;
			if (next <= a) break;

			if (mbi.State == MEM_FREE) {
				finalizePriv();
				curMappedBase = 0;
				freeTotal += mbi.RegionSize;
				if (mbi.RegionSize > largestFree) largestFree = mbi.RegionSize;
				if (mbi.RegionSize >= (16ULL << 20)) free16Gaps++;
			}
			else if (mbi.State == MEM_COMMIT || mbi.State == MEM_RESERVE) {
				const SIZE_T cBytes = (mbi.State == MEM_COMMIT) ? mbi.RegionSize : 0;
				const SIZE_T rBytes = (mbi.State == MEM_RESERVE) ? mbi.RegionSize : 0;
				const UINT_PTR allocBase = (UINT_PTR)mbi.AllocationBase;

				if (mbi.Type == MEM_IMAGE) {
					finalizePriv();
					curMappedBase = 0;
					imgTotal += mbi.RegionSize;
					imgCommit += cBytes;
					imgReserve += rBytes;

					unsigned idx = ws.imageCount;
					if (idx > 0 && ws.images[idx - 1].base == allocBase) {
						idx--;
					}
					else {
						bool found = false;
						for (unsigned i = 0; i < ws.imageCount; i++) {
							if (ws.images[i].base == allocBase) { idx = i; found = true; break; }
						}
						if (!found && ws.imageCount < 256u) {
							idx = ws.imageCount++;
							detail::ImageEntry& im = ws.images[idx];
							im = {};
							im.base = allocBase;
							detail::ResolveImageName(allocBase, exeBase, im.name, sizeof(im.name));
						}
						else if (!found) {
							idx = 256u;
						}
					}
					if (idx < ws.imageCount) {
						detail::ImageEntry& im = ws.images[idx];
						im.size += mbi.RegionSize;
						im.commit += cBytes;
						if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
							(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
							if (im.rwCount > 0 && im.rwStart[im.rwCount - 1] + im.rwSize[im.rwCount - 1] == (UINT_PTR)mbi.BaseAddress) {
								im.rwSize[im.rwCount - 1] += mbi.RegionSize;
							}
							else if (im.rwCount < 8u) {
								im.rwStart[im.rwCount] = (UINT_PTR)mbi.BaseAddress;
								im.rwSize[im.rwCount] = mbi.RegionSize;
								im.rwCount++;
							}
						}
					}
				}
				else if (mbi.Type == MEM_MAPPED) {
					finalizePriv();
					mapTotal += mbi.RegionSize;
					mapCommit += cBytes;
					mapReserve += rBytes;
					const bool newAlloc = (allocBase != curMappedBase);
					if (newAlloc) {
						curMappedBase = allocBase;
						if (!detail::QueryMappedBasename((void*)allocBase, curMappedName, sizeof(curMappedName)))
							strcpy_s(curMappedName, "(pagefile/shared)");
					}
					detail::AddMapped(curMappedName, mbi.RegionSize, cBytes, rBytes, newAlloc);
				}
				else if (mbi.Type == MEM_PRIVATE) {
					curMappedBase = 0;
					privTotal += mbi.RegionSize;
					privCommit += cBytes;
					privReserve += rBytes;
					if (allocBase != curPriv.base) {
						finalizePriv();
						curPriv.base = allocBase;
					}
					curPriv.totalBytes += mbi.RegionSize;
					curPriv.commitBytes += cBytes;
					curPriv.reserveBytes += rBytes;
					if (mbi.Protect & PAGE_GUARD) curPriv.hasGuard = true;
					if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
						if (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
							curPriv.hasExec = true;
						if (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
							curPriv.hasWrite = true;
					}
				}
				else {
					finalizePriv();
					curMappedBase = 0;
				}
			}
			a = next;
		}
		finalizePriv();

		// Sort top lists descending by size.
		std::sort(ws.images, ws.images + ws.imageCount, [](const detail::ImageEntry& x, const detail::ImageEntry& y) {
			return x.size > y.size;
		});
		std::sort(ws.mapped, ws.mapped + ws.mappedCount, [](const detail::MappedEntry& x, const detail::MappedEntry& y) {
			return x.totalBytes > y.totalBytes;
		});
		std::sort(ws.privates, ws.privates + ws.privateStored, [](const detail::PrivateEntry& x, const detail::PrivateEntry& y) {
			return x.totalBytes > y.totalBytes;
		});

		// Optional pointer-reference attribution from module writable globals to the top 16 private blocks.
		if (scanRefs && ws.privateStored > 0) {
			UINT_PTR targets[16];
			const unsigned topN = ws.privateStored < 16u ? ws.privateStored : 16u;
			UINT_PTR minTarget = UINTPTR_MAX, maxTarget = 0;
			unsigned targetCount = 0;
			for (unsigned i = 0; i < topN; i++) {
				const UINT_PTR b = ws.privates[i].base;
				targets[i] = b >= 0x01000000u ? b : 0;
				if (targets[i]) {
					if (b < minTarget) minTarget = b;
					if (b > maxTarget) maxTarget = b;
					targetCount++;
				}
			}
			if (targetCount > 0) {
				const UINT_PTR wsStart = (UINT_PTR)&ws;
				const UINT_PTR wsEnd = wsStart + sizeof(ws);
				SIZE_T totalScanned = 0;
				constexpr SIZE_T kTotalScanCap = 64u << 20;
				constexpr SIZE_T kPerModScanCap = 16u << 20;
				for (unsigned m = 0; m < ws.imageCount && totalScanned < kTotalScanCap; m++) {
					SIZE_T modScanned = 0;
					const detail::ImageEntry& im = ws.images[m];
					for (unsigned s = 0; s < im.rwCount && modScanned < kPerModScanCap && totalScanned < kTotalScanCap; s++) {
						SIZE_T len = im.rwSize[s];
						if (len > kPerModScanCap - modScanned) len = kPerModScanCap - modScanned;
						if (len > kTotalScanCap - totalScanned) len = kTotalScanCap - totalScanned;
						detail::ScanSpanForRefs(im.rwStart[s], len, wsStart, wsEnd, targets, topN, minTarget, maxTarget, im.name, ws.privates);
						modScanned += len;
						totalScanned += len;
					}
				}
			}
		}

		// Process memory counters & total VA.
		MEMORYSTATUSEX ms = { sizeof(ms) };
		GlobalMemoryStatusEx(&ms);
		const double totalVaMB = ms.ullTotalVirtual / 1048576.0;
		const double usedVaMB = (ms.ullTotalVirtual >= ms.ullAvailVirtual ? ms.ullTotalVirtual - ms.ullAvailVirtual : 0ULL) / 1048576.0;

		struct PmcCounters { DWORD cb, faults; SIZE_T peakWs, ws, qpp, qp, qnpp, qnp, pagefile, peakPagefile; } pmc = {};
		pmc.cb = sizeof(pmc);
		typedef BOOL (WINAPI* GetPmcFn)(HANDLE, void*, DWORD);
		static GetPmcFn getPmc = (GetPmcFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
		SIZE_T commitMB = 0, wsMB = 0;
		if (getPmc && getPmc(GetCurrentProcess(), &pmc, sizeof(pmc))) {
			commitMB = pmc.pagefile >> 20;
			wsMB = pmc.ws >> 20;
		}

		char line[1024];
		sprintf_s(line,
			"MEMORY MAP (%s): VA %.0f/%.0f MB used, free %.0f MB (largest %.0f MB, %u gaps >=16 MB) | committed %u MB, WS %u MB | "
			"Private %.0f MB (%.0fc/%.0fr, stacks %.0f MB / %u), Mapped %.0f MB (%.0fc/%.0fr), Image %.0f MB (%u mods), WinHeaps %lu",
			reason ? reason : "?", usedVaMB, totalVaMB, freeTotal / 1048576.0, largestFree / 1048576.0, free16Gaps,
			(unsigned)commitMB, (unsigned)wsMB,
			privTotal / 1048576.0, privCommit / 1048576.0, privReserve / 1048576.0, stackTotal / 1048576.0, stackCount,
			mapTotal / 1048576.0, mapCommit / 1048576.0, mapReserve / 1048576.0,
			imgTotal / 1048576.0, ws.imageCount, (unsigned long)ws.winHeapTotal);
		detail::EmitLine(unbuffered, line);

		sprintf_s(line,
			"MEMORY MAP private sizes: >=64MB: %u (%.0f MB), 16-64MB: %u (%.0f MB), 4-16MB: %u (%.0f MB), <4MB: %u (%.0f MB)",
			hist[0].count, hist[0].bytes / 1048576.0, hist[1].count, hist[1].bytes / 1048576.0,
			hist[2].count, hist[2].bytes / 1048576.0, hist[3].count, hist[3].bytes / 1048576.0);
		detail::EmitLine(unbuffered, line);

		{
			int pos = sprintf_s(line, "MEMORY MAP top images:");
			unsigned shown = 0;
			for (unsigned i = 0; i < ws.imageCount && shown < 10u; i++) {
				if (ws.images[i].size < (4ULL << 20)) break;
				char item[96];
				const int ilen = sprintf_s(item, "%s %s %.0f MB", shown ? "," : "", ws.images[i].name, ws.images[i].size / 1048576.0);
				if (ilen <= 0 || pos + ilen >= 880) break;
				memcpy(line + pos, item, (size_t)ilen + 1u);
				pos += ilen;
				shown++;
			}
			if (!shown) strcat_s(line, " (none >=4 MB)");
			detail::EmitLine(unbuffered, line);
		}

		{
			int pos = sprintf_s(line, "MEMORY MAP top mapped:");
			unsigned shown = 0;
			for (unsigned i = 0; i < ws.mappedCount && shown < 10u; i++) {
				const detail::MappedEntry& e = ws.mapped[i];
				if (e.totalBytes < (2ULL << 20)) break;
				char item[140];
				int ilen = 0;
				if (e.count > 1u)
					ilen = sprintf_s(item, "%s %s x%u %.0f MB (%.0fc/%.0fr)", shown ? "," : "", e.name, e.count,
						e.totalBytes / 1048576.0, e.commitBytes / 1048576.0, e.reserveBytes / 1048576.0);
				else
					ilen = sprintf_s(item, "%s %s %.0f MB (%.0fc/%.0fr)", shown ? "," : "", e.name,
						e.totalBytes / 1048576.0, e.commitBytes / 1048576.0, e.reserveBytes / 1048576.0);
				if (ilen <= 0 || pos + ilen >= 880) break;
				memcpy(line + pos, item, (size_t)ilen + 1u);
				pos += ilen;
				shown++;
			}
			if (!shown) strcat_s(line, " (none >=2 MB)");
			detail::EmitLine(unbuffered, line);
		}

		{
			int pos = sprintf_s(line, "MEMORY MAP top private:");
			unsigned shown = 0;
			for (unsigned i = 0; i < ws.privateStored && shown < 12u; i++) {
				const detail::PrivateEntry& p = ws.privates[i];
				const char* perm = p.hasExec ? "RX" : (p.hasWrite ? "RW" : (p.commitBytes ? "R" : "NA"));
				const char* heapTag = p.isDefaultHeap ? " DefaultHeap" : (p.isWinHeap ? " WinHeap" : "");
				char refTag[80] = "";
				if (p.refMod[0]) sprintf_s(refTag, " ref:%s", p.refMod);
				char item[160];
				const int ilen = sprintf_s(item, "%s 0x%08lX %.0f MB (%.0fc/%.0fr %s%s%s)", shown ? "," : "",
					(unsigned long)p.base, p.totalBytes / 1048576.0, p.commitBytes / 1048576.0, p.reserveBytes / 1048576.0,
					perm, heapTag, refTag);
				if (ilen <= 0 || pos + ilen >= 880) break;
				memcpy(line + pos, item, (size_t)ilen + 1u);
				pos += ilen;
				shown++;
			}
			if (!shown) strcat_s(line, " (none >=8 MB)");
			detail::EmitLine(unbuffered, line);
		}

#if NVR_TEST_TOOLS
		if (includeThreads) {
			unsigned totalThreads = 0;
			detail::CollectThreadModules(totalThreads);
			std::sort(ws.threadMods, ws.threadMods + ws.threadModCount, [](const detail::ThreadModEntry& x, const detail::ThreadModEntry& y) {
				return x.count > y.count;
			});
			int pos = sprintf_s(line, "MEMORY MAP threads (%u):", totalThreads);
			unsigned shown = 0;
			for (unsigned i = 0; i < ws.threadModCount && shown < 10u; i++) {
				char item[96];
				const int ilen = sprintf_s(item, "%s %s:%u", shown ? "," : "", ws.threadMods[i].name, ws.threadMods[i].count);
				if (ilen <= 0 || pos + ilen >= 880) break;
				memcpy(line + pos, item, (size_t)ilen + 1u);
				pos += ilen;
				shown++;
			}
			if (!shown) strcat_s(line, " (none)");
			detail::EmitLine(unbuffered, line);
		}
#else
		(void)includeThreads;
#endif
	}

} // namespace VaMemoryMap
