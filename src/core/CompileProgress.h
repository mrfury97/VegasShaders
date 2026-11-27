#pragma once

// UNOFFICIAL 2026-11-20: delayed-show Win32 progress indicator during startup shader/effect compilation.
// On a cold cache (first launch or after an update), startup compilation runs before D3D9/ImGui presents
// any frames, so the game window sits black and looks frozen. This spawns a lightweight Win32 GDI overlay
// on its own thread ONLY if compilation takes longer than ~1.5 s (fast cached launches never show a window),
// stays responsive to Windows messages, and closes automatically as soon as ImGuiManager::Initialize runs.

#include <windows.h>
#include <process.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace CompileProgress {

	inline std::atomic<bool> Started{ false };
	inline std::atomic<bool> StopRequested{ false };
	inline std::atomic<bool> ImmediateShow{ false };
	inline std::atomic<bool> ThreadActive{ false };
	inline std::atomic<HWND> Window{ nullptr };
	inline std::atomic<int>  EffectsTotal{ 0 };
	inline std::atomic<int>  EffectsDone{ 0 };
	inline std::atomic<int>  EffectsCompiled{ 0 };
	inline std::atomic<int>  ShadersTotal{ 0 };
	inline std::atomic<int>  ShadersDone{ 0 };
	inline std::atomic<int>  ShadersCompiled{ 0 };
	inline ULONGLONG         StartTick = 0;

	inline std::mutex        LabelMutex;
	inline char              TitleText[96] = "Vegas Shaders - Compiling Shaders";
	inline char              CurrentLabel[96] = "";

	inline void NoteCurrent(const char* name) {
		if (!name || !name[0]) return;
		std::lock_guard<std::mutex> lock(LabelMutex);
		strncpy_s(CurrentLabel, name, _TRUNCATE);
	}

	inline void Paint(HWND hwnd) {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		const int w = rc.right - rc.left;
		const int h = rc.bottom - rc.top;

		HDC mem = CreateCompatibleDC(hdc);
		HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
		HGDIOBJ oldBmp = SelectObject(mem, bmp);

		// Background + thin amber Fallout-style frame
		HBRUSH bg = CreateSolidBrush(RGB(22, 20, 18));
		FillRect(mem, &rc, bg);
		DeleteObject(bg);

		HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(214, 152, 54));
		HGDIOBJ oldPen = SelectObject(mem, borderPen);
		HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
		Rectangle(mem, 0, 0, w, h);
		SelectObject(mem, oldBrush);
		SelectObject(mem, oldPen);
		DeleteObject(borderPen);

		SetBkMode(mem, TRANSPARENT);

		HFONT titleFont = CreateFontA(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
		HFONT bodyFont = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
		HFONT smallFont = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

		const int eTot = EffectsTotal.load();
		const int eDone = (EffectsDone.load() > eTot && eTot > 0) ? eTot : EffectsDone.load();
		const int eComp = EffectsCompiled.load();
		const int sTot = ShadersTotal.load();
		const int sDone = (ShadersDone.load() > sTot && sTot > 0) ? sTot : ShadersDone.load();
		const int sComp = ShadersCompiled.load();
		const int total = eTot + sTot;
		const int done = eDone + sDone;
		const float frac = total > 0 ? (float)done / (float)total : 0.0f;
		const unsigned secs = StartTick ? (unsigned)((GetTickCount64() - StartTick) / 1000ULL) : 0u;

		char cur[96] = "";
		char title[96] = "";
		{
			std::lock_guard<std::mutex> lock(LabelMutex);
			strncpy_s(cur, CurrentLabel, _TRUNCATE);
			strncpy_s(title, TitleText, _TRUNCATE);
		}

		// Title + elapsed time
		HGDIOBJ oldFont = SelectObject(mem, titleFont);
		SetTextColor(mem, RGB(242, 196, 98));
		RECT titleRc = { 16, 12, w - 76, 36 };
		DrawTextA(mem, title[0] ? title : "Vegas Shaders - Compiling Shaders", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

		char timeBuf[32];
		sprintf_s(timeBuf, "%us", secs);
		SetTextColor(mem, RGB(175, 165, 148));
		RECT timeRc = { w - 76, 12, w - 16, 36 };
		DrawTextA(mem, timeBuf, -1, &timeRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

		// Progress bar track + fill
		RECT barBg = { 16, 44, w - 16, 62 };
		HBRUSH trackBrush = CreateSolidBrush(RGB(48, 43, 38));
		FillRect(mem, &barBg, trackBrush);
		DeleteObject(trackBrush);

		const int barW = barBg.right - barBg.left;
		int fillW = (int)(barW * (frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac));
		if (total == 0) {
			// Indeterminate pulse if counts haven't populated yet (non-parallel compile)
			const int pulseW = barW / 4;
			if (barW > pulseW && pulseW > 0) {
				const int pos = (int)(((GetTickCount64() / 25ULL) % (ULONGLONG)(barW - pulseW)));
				RECT pulse = { barBg.left + pos, barBg.top, barBg.left + pos + pulseW, barBg.bottom };
				HBRUSH fillBrush = CreateSolidBrush(RGB(232, 168, 56));
				FillRect(mem, &pulse, fillBrush);
				DeleteObject(fillBrush);
			}
		}
		else if (fillW > 0) {
			RECT barFill = { barBg.left, barBg.top, barBg.left + fillW, barBg.bottom };
			HBRUSH fillBrush = CreateSolidBrush(RGB(232, 168, 56));
			FillRect(mem, &barFill, fillBrush);
			DeleteObject(fillBrush);
		}

		// Counts line
		SelectObject(mem, bodyFont);
		SetTextColor(mem, RGB(230, 224, 212));
		char countBuf[160];
		if (sTot > 0 && eTot > 0)
			sprintf_s(countBuf, "Effects: %d / %d    Shaders: %d / %d    (%d compiled)", eDone, eTot, sDone, sTot, eComp + sComp);
		else if (eTot > 0)
			sprintf_s(countBuf, "Effects: %d / %d    (%d compiled)", eDone, eTot, eComp);
		else if (sTot > 0)
			sprintf_s(countBuf, "Shaders: %d / %d    (%d compiled)", sDone, sTot, sComp);
		else
			sprintf_s(countBuf, "Preparing shaders... (%d compiled)", eComp + sComp);
		RECT countRc = { 16, 70, w - 16, 90 };
		DrawTextA(mem, countBuf, -1, &countRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

		// Subline: current shader or cache note
		SelectObject(mem, smallFont);
		SetTextColor(mem, RGB(150, 142, 130));
		char subBuf[160];
		if (cur[0])
			sprintf_s(subBuf, "Current: %s  (cached for future launches)", cur);
		else
			sprintf_s(subBuf, "First launch or after an update - cached for future launches");
		RECT subRc = { 16, 92, w - 16, 110 };
		DrawTextA(mem, subBuf, -1, &subRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

		SelectObject(mem, oldFont);
		DeleteObject(titleFont);
		DeleteObject(bodyFont);
		DeleteObject(smallFont);

		BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
		SelectObject(mem, oldBmp);
		DeleteObject(bmp);
		DeleteDC(mem);
		EndPaint(hwnd, &ps);
	}

	inline constexpr int kWinW = 460;
	inline constexpr int kWinH = 120;

	inline HWND FindGameWindow(HWND self) {
		struct Search { DWORD pid; HWND self; HWND best; LONG bestArea; } s = { GetCurrentProcessId(), self, nullptr, -1 };
		EnumWindows([](HWND h, LPARAM lp) -> BOOL {
			auto* s = (Search*)lp;
			if (h == s->self) return TRUE;
			DWORD pid = 0;
			GetWindowThreadProcessId(h, &pid);
			if (pid != s->pid) return TRUE;
			if (!IsWindowVisible(h)) return TRUE;
			if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
			RECT r = {};
			if (!GetWindowRect(h, &r)) return TRUE;
			const LONG area = (r.right - r.left) * (r.bottom - r.top);
			if (area > s->bestArea) { s->best = h; s->bestArea = area; }
			return TRUE;
		}, (LPARAM)&s);
		return s.best;
	}

	// Keep the progress box centered on the Fallout: New Vegas window and tucked directly above it in Z-order
	// (never WS_EX_TOPMOST over other apps when the user alt-tabs away). Uses only non-blocking Win32 queries
	// + SWP_ASYNCWINDOWPOS so it can never deadlock with the main thread while shaders are compiling.
	inline void SyncToGameWindow(HWND hwnd) {
		HWND game = FindGameWindow(hwnd);
		if (!game) {
			if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
			return;
		}
		if (IsIconic(game)) {
			if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
			return;
		}
		RECT gr = {};
		if (!GetWindowRect(game, &gr) || (gr.right - gr.left) < 100 || (gr.bottom - gr.top) < 100) {
			if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
			return;
		}
		const int x = gr.left + ((gr.right - gr.left) - kWinW) / 2;
		const int y = gr.top + ((gr.bottom - gr.top) - kWinH) / 2;
		const bool gameTopmost = (GetWindowLongA(game, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

		if (gameTopmost) {
			// UNOFFICIAL 2026-11-23: never place an HWND_TOPMOST GDI popup over a Fullscreen Exclusive / topmost game window
			// (causes FSE focus/swapchain demotions and device loss during shader compilation). Hide cleanly instead.
			if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
			return;
		}

		if (GetWindowLongA(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

		HWND aboveGame = GetWindow(game, GW_HWNDPREV);
		if (aboveGame == hwnd) {
			SetWindowPos(hwnd, nullptr, x, y, kWinW, kWinH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
		}
		else {
			HWND insertAfter = aboveGame ? aboveGame : HWND_TOP;
			SetWindowPos(hwnd, insertAfter, x, y, kWinW, kWinH, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
		}
	}

	inline LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
		switch (msg) {
		case WM_PAINT:
			Paint(hwnd);
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_TIMER:
			if (StopRequested.load()) {
				KillTimer(hwnd, 1);
				DestroyWindow(hwnd);
			}
			else {
				SyncToGameWindow(hwnd);
				if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		case WM_CLOSE:
			KillTimer(hwnd, 1);
			DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			Window.store(nullptr);
			PostQuitMessage(0);
			return 0;
		default:
			return DefWindowProcA(hwnd, msg, wp, lp);
		}
	}

	inline void UiThread() {
		struct ActiveGuard {
			ActiveGuard() { ThreadActive.store(true); }
			~ActiveGuard() { ThreadActive.store(false); }
		} activeGuard;
		// Delay showing the window so fast cached launches (< 1.5 s) never flash anything.
		const int maxLoops = ImmediateShow.load() ? 6 : 30;
		for (int i = 0; i < maxLoops; i++) {
			if (StopRequested.load()) return;
			if (!ImmediateShow.load() && i >= 16 && (EffectsCompiled.load() + ShadersCompiled.load()) > 0) break;
			Sleep(50);
		}
		if (StopRequested.load()) return;

		WNDCLASSA wc = {};
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "VegasShadersCompileProgress";
		wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_WAIT);
		RegisterClassA(&wc);

		const int scrW = GetSystemMetrics(SM_CXSCREEN);
		const int scrH = GetSystemMetrics(SM_CYSCREEN);
		const int x = (scrW - kWinW) / 2;
		const int y = (scrH - kWinH) / 2;

		HWND hwnd = CreateWindowExA(
			WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			wc.lpszClassName, "Vegas Shaders",
			WS_POPUP,
			x, y, kWinW, kWinH,
			nullptr, nullptr, wc.hInstance, nullptr);
		if (!hwnd) return;
		Window.store(hwnd);
		SyncToGameWindow(hwnd);
		SetTimer(hwnd, 1, 100, nullptr);
		UpdateWindow(hwnd);

		MSG msg;
		while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}

	inline void Begin() {
		if (StopRequested.load()) return;
		bool expected = false;
		if (!Started.compare_exchange_strong(expected, true)) return;
		StartTick = GetTickCount64();
		std::thread(UiThread).detach();
	}

	inline void Finish() {
		StopRequested.store(true);
		HWND hwnd = Window.load();
		if (hwnd) PostMessageA(hwnd, WM_CLOSE, 0, 0);
	}

	inline void BeginLate(const char* title, int totalShaders) {
		Finish();
		for (int i = 0; i < 40 && ThreadActive.load(); i++) Sleep(10);
		{
			std::lock_guard<std::mutex> lock(LabelMutex);
			strncpy_s(TitleText, (title && title[0]) ? title : "Vegas Shaders - Finishing Shaders", _TRUNCATE);
			CurrentLabel[0] = '\0';
		}
		EffectsTotal.store(0);
		EffectsDone.store(0);
		ShadersTotal.store(totalShaders);
		ImmediateShow.store(true);
		StopRequested.store(false);
		Started.store(false);
		Begin();
	}

	// UNOFFICIAL 2026-11-23: resolve parallel shader/effect compile worker count (0 = auto, capped at 4 on 32-bit
	// to avoid heap/VA thrashing alongside DXVK's compiler threads; explicit values clamped to 1..12).
	inline unsigned ResolveWorkerThreads(int configured) {
		if (configured > 0) return configured > 12 ? 12u : (unsigned)configured;
		const unsigned cores = std::thread::hardware_concurrency();
		const unsigned minusOne = cores > 1 ? cores - 1 : 1u;
		return minusOne > 4u ? 4u : (minusOne < 1u ? 1u : minusOne);
	}

	inline unsigned long long UsedVaMB() {
		MEMORYSTATUSEX ms = { sizeof(ms) };
		if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalVirtual <= ms.ullAvailVirtual) return 0ULL;
		return (ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20;
	}

	// UNOFFICIAL 2026-11-23: D3DX9_43's preprocessor and legacy effect compiler share non-thread-safe internal lexer/include
	// tables; serialize calls to D3DXPreprocessShaderFromFileA / D3DXCreateEffectCompiler across worker and main threads.
	inline std::mutex D3dxPreprocessMutex;

	// UNOFFICIAL 2026-11-23: unified VA-aware admission gate shared by ShaderPrecompile, EffectPrecompile, LoadEffect, and
	// LoadShader to prevent concurrent compile spikes from exhausting 32-bit VA.
	inline std::mutex GateMutex;
	inline std::condition_variable GateCv;
	inline int GateActive = 0;
	inline std::atomic<unsigned> VaThrottledCount{ 0 };

	struct CompileGate {
		explicit CompileGate(unsigned maxSlots = 4u) {
			if (maxSlots < 1u) maxSlots = 1u;
			bool counted = false;
			std::unique_lock<std::mutex> lock(GateMutex);
			for (;;) {
				const unsigned long long usedMB = UsedVaMB();
				const int allowed = usedMB >= 2500ULL ? 1 : (usedMB >= 2200ULL ? (maxSlots < 2u ? (int)maxSlots : 2) : (int)maxSlots);
				if (GateActive == 0 || GateActive < allowed) {
					GateActive++;
					return;
				}
				if (!counted) { VaThrottledCount.fetch_add(1, std::memory_order_relaxed); counted = true; }
				GateCv.wait_for(lock, std::chrono::milliseconds(50));
			}
		}
		~CompileGate() {
			{
				std::lock_guard<std::mutex> lock(GateMutex);
				if (GateActive > 0) GateActive--;
			}
			GateCv.notify_one();
		}
		CompileGate(const CompileGate&) = delete;
		CompileGate& operator=(const CompileGate&) = delete;
	};

	inline unsigned __stdcall VoidWorkerTrampoline(void* arg) {
		reinterpret_cast<void(*)()>(arg)();
		return 0;
	}

	// Launch a detached worker with a 4 MB reserved stack (FalloutNV.exe defaults to 1 MB PE stack reserve,
	// which deep HLSL AST recursion in D3DXCompileShader/d3dcompiler_47 can overflow silently).
	inline void SpawnWorker(void (*fn)()) {
		const uintptr_t h = _beginthreadex(nullptr, 4u * 1024u * 1024u, VoidWorkerTrampoline,
			reinterpret_cast<void*>(fn), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
		if (h) CloseHandle(reinterpret_cast<HANDLE>(h));
		else std::thread(fn).detach();
	}

	inline HANDLE SpawnJoinableWorker(unsigned (__stdcall *entry)(void*), void* arg) {
		const uintptr_t h = _beginthreadex(nullptr, 4u * 1024u * 1024u, entry, arg, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
		return h ? reinterpret_cast<HANDLE>(h) : nullptr;
	}

	inline std::atomic<int> EffectWorkersActive{ 0 };
	inline void DrainEffectWorkers() {
		while (EffectWorkersActive.load(std::memory_order_acquire) > 0) Sleep(1);
	}

	// UNOFFICIAL 2026-11-23: pure flag wait without PeekMessageA — PeekMessageA dispatches queued inter-thread sent messages
	// synchronously into game/ImGui WndProcs while inside non-reentrant shader/renderer/menu hooks.
	inline void WaitDone(const std::atomic<bool>& done) {
		while (!done.load(std::memory_order_acquire)) Sleep(1);
	}
}
