#pragma once

// UNOFFICIAL 2026-11-07 (rtgap-textures) the GPU's own video memory, for the RT real textures' auto caps (RtTexturePack::AutoCaps).
// A 32-bit process cannot trust DXGI_ADAPTER_DESC1::DedicatedVideoMemory (measured on this PC: a GTX 1070's 8 GB reads 3072 MB), so the
// 64-bit figure comes from the kernel thunk D3DKMTQueryAdapterInfo(KMTQAITYPE_GETSEGMENTSIZE) by the adapter's LUID first (what DXGI
// itself reads: 8067 MB there), then DXGI's description, with the local budget / 0.92 standing in when that reads 2.75 GB or more. Windows' own gdi32.dll / dxgi.dll
// only (System32: never a dxgi.dll in the game folder). Used by RtCore::Start and the RT test host (scene "texpack" prints what it finds
// on the test PC's GPUs). Windows + DXGI headers only.

#include <windows.h>
#include <dxgi1_4.h>
#include <stdio.h>

namespace RtVideoMemory {

	// The kernel thunk's structs (d3dkmthk.h is a WDK header), as documented
	typedef UINT KmtHandle;
	struct KmtOpenAdapterFromLuid { LUID AdapterLuid; KmtHandle hAdapter; };
	struct KmtQueryAdapterInfo { KmtHandle hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };
	struct KmtSegmentSizeInfo { ULONGLONG DedicatedVideoMemorySize; ULONGLONG DedicatedSystemMemorySize; ULONGLONG SharedSystemMemorySize; };
	struct KmtCloseAdapter { KmtHandle hAdapter; };
	static const UINT kKmtGetSegmentSize = 3;	// KMTQAITYPE_GETSEGMENTSIZE

	inline HMODULE System32(const char* dll) {
		HMODULE m = GetModuleHandleA(dll);
		return m ? m : LoadLibraryExA(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	}

	// Bytes of dedicated video memory of the adapter (luid when valid, else the first with this vendor / device id); 0 = not found.
	// text: how it was found (logged once).
	inline unsigned long long Dedicated(const BYTE luid[8], bool luidValid, UINT vendor, UINT device, char* text, size_t size) {
		LUID want = {};
		if (luidValid) memcpy(&want, luid, sizeof(want));
		if (luidValid) {
			typedef LONG(APIENTRY* OpenFn)(KmtOpenAdapterFromLuid*);
			typedef LONG(APIENTRY* QueryFn)(const KmtQueryAdapterInfo*);
			typedef LONG(APIENTRY* CloseFn)(const KmtCloseAdapter*);
			const HMODULE gdi = System32("gdi32.dll");
			const OpenFn open = gdi ? (OpenFn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid") : nullptr;
			const QueryFn query = gdi ? (QueryFn)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo") : nullptr;
			const CloseFn close = gdi ? (CloseFn)GetProcAddress(gdi, "D3DKMTCloseAdapter") : nullptr;
			KmtOpenAdapterFromLuid o = { want, 0 };
			if (open && query && close && open(&o) == 0) {
				KmtSegmentSizeInfo s = {};
				KmtQueryAdapterInfo q = { o.hAdapter, kKmtGetSegmentSize, &s, (UINT)sizeof(s) };
				const LONG r = query(&q);
				KmtCloseAdapter c = { o.hAdapter };
				close(&c);
				if (r == 0 && s.DedicatedVideoMemorySize) {
					_snprintf_s(text, size, _TRUNCATE, "%llu MB dedicated (D3DKMT segment sizes; %llu MB shared system memory)",
						s.DedicatedVideoMemorySize >> 20, s.SharedSystemMemorySize >> 20);
					return s.DedicatedVideoMemorySize;
				}
			}
		}
		typedef HRESULT(WINAPI* CreateFactory)(REFIID, void**);
		const HMODULE dxgi = System32("dxgi.dll");
		const CreateFactory create = dxgi ? (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
		IDXGIFactory1* factory = nullptr;
		if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) { _snprintf_s(text, size, _TRUNCATE, "unknown (no DXGI)"); return 0; }
		unsigned long long result = 0;
		IDXGIAdapter1* adapter = nullptr;
		for (UINT i = 0; !result && SUCCEEDED(factory->EnumAdapters1(i, &adapter)); i++) {
			DXGI_ADAPTER_DESC1 d = {};
			if (SUCCEEDED(adapter->GetDesc1(&d)) && (luidValid ? (d.AdapterLuid.LowPart == want.LowPart && d.AdapterLuid.HighPart == want.HighPart)
				: (d.VendorId == vendor && d.DeviceId == device))) {
				unsigned long long dedicated = (unsigned long long)d.DedicatedVideoMemory, budget = 0;
				IDXGIAdapter3* a3 = nullptr;
				if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3)) && a3) {
					DXGI_QUERY_VIDEO_MEMORY_INFO m = {};
					if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m))) budget = m.Budget;
					a3->Release();
				}
				result = dedicated;
				// 2.75 GB+: maybe clamped (measured 2026-11-07, RT host texpack, 32-bit: a GTX 1070's 8 GB reads 3072 MB, its budget 7299 MB)
				const bool clamped = dedicated >= (11ull << 28) && (unsigned long long)(budget / 0.92) > dedicated;
				if (clamped) result = (unsigned long long)(budget / 0.92);
				_snprintf_s(text, size, _TRUNCATE, "%llu MB (DXGI: dedicated %llu MB, local budget %llu MB%s)", result >> 20, dedicated >> 20, budget >> 20,
					clamped ? "; the budget / 0.92 taken: a 32-bit process reads at most 3 GB of dedicated memory" : "");
				if (!result) result = 1;	// found, but no dedicated memory: the lowest tier
			}
			adapter->Release();
		}
		factory->Release();
		if (!result) _snprintf_s(text, size, _TRUNCATE, "unknown (the adapter was not found)");
		return result;
	}
}
