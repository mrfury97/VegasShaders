#pragma once

// UNOFFICIAL 2026-10-26 (NVR DXVK extension 1.5-hdr, fork patch 0012; 0007 before the 10-27 renumbering): true HDR output on an HDR display.
//
// [Shaders.HDR.Main] Enabled (default off; a settings-only menu entry like Cloth / CloudShadows). Needs the NVR DXVK d3d9.dll with ID3D9NvrExtHdr; on native D3D9, stock DXVK or an
// older fork nothing here runs and the picture is exactly the old one.
//
// How: the game's D3D9 back buffer stays 8-bit SDR (the HUD, menus and everything after NVR draw into it as always). The fork
// creates the Vulkan swap chain as HDR10 (PQ, 10-bit; Mode 1) or scRGB (FP16 linear; Mode 2) and converts at present:
// the SDR picture is decoded to linear light and put at PaperWhiteNits; where it still equals NVR's finished picture (marked
// here after NVR's last effect, i.e. no HUD/menu drew over it) each pixel is multiplied by a highlight gain (HdrGain.frag,
// computed here from the scene before tonemapping and the tonemapper's SDR result): 1 for shadows and mid-tones (the SDR look,
// unchanged), rising towards PeakNits / PaperWhiteNits for what the SDR tonemapper squeezed into white (sun, sky near the sun,
// lamps, fire, specular glints). HUD and menus therefore stay at paper white, never blinding.
//
// Display check: the output must be in HDR mode in Windows (DXGI colour space ST.2084), else the output stays SDR automatically
// (logged); IgnoreDisplayCheck forces it (tests on an SDR desktop: Windows then maps the HDR signal down itself).
// UNOFFICIAL 2026-10-29: the colour space DXGI reports is the monitor's CURRENT mode, and the game's own fullscreen SDR picture
// switches an HDR desktop's monitor to SDR (and back on alt-tab). The check alone therefore locked itself out: SDR at the first
// frame although Windows HDR was on, or SDR again after a short mode change while HDR10 was being set up (user's log 10-29 15:11).
// Now a monitor seen in HDR mode once in this run (at plugin load, before the game opens its window: NoteDesktop, or at any later
// check) counts as "Windows HDR on" for the rest of the run; a later SDR reading for it is logged once and ignored.
// UNOFFICIAL 2026-10-29 (user's run 16:04, "still nothing"): the latch can miss it altogether. Windows HDR was switched on at
// 16:03:55, the game started at 16:04:01: SDR at load (the monitor was still switching) and at every check after (the game's
// picture had switched it to SDR), so the fork was never asked. The Windows SETTING ("Use HDR", which the game's picture does not
// change) is therefore read (ReadWindowsSetting: Windows 11 24H2's highDynamicRangeUserEnabled, else the monitor's own
// MonitorDataStore entry) and decides when it is found; the current mode and the latch remain for systems where it is not. A
// refused mode is asked again (with the display check, and whenever the display reads HDR, e.g. alt-tabbed), and the fork reports
// whether its swap chain really is HDR (GetHdrInfo ActiveMode), which is logged.
//
// Test tool: CaptureFrame (a counter: each new non-zero value) writes, 60 frames later, a frame's HDR output, as the fork sends it (scRGB,
// 1.0 = 80 nits), to VegasShaders-Logs\NeonSelfTest_HdrCapture_<n>.pfm.world (a PFM: RGB float, bottom row first;
// the name lets the slot test runner collect it with the pictures) with a summary line in the log.

#include <dxgi1_6.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "DxvkCompute.h"
#include "../effects/Tonemapping/Tonemapping.h"

namespace HdrOutput {

	struct State {
		ID3D9NvrExtHdr*			Hdr = nullptr;		// queried once
		bool					Queried = false;
		bool					Active = false;		// a HDR mode is set in the fork
		NVR_EXT_HDR_OUTPUT		Applied = {};
		bool					Refused = false;	// the fork refused the mode (logged once until the settings change)
		unsigned				RefusedRetries = 0;	// asked again every 600 frames, 6 times (UNOFFICIAL 2026-10-29)
		bool					RetryNow = false;	// the display read HDR (e.g. alt-tabbed): ask a refused mode again now
		bool					SwapSdr = false;	// on, but the fork says its swap chain is SDR (asked again: 3 timed attempts + alt-tab)
		bool					SwapConfirmed = false;	// UNOFFICIAL 2026-11-21: fork swap chain confirmed HDR; skip redundant 600-frame GetHdrInfo WSI polls
		UINT					ForkMode = NVR_EXT_HDR_OFF;	// the mode the fork holds (it can hold one while Active is false: SetOff sends OFF then too)
		int						LastCurrent = -1;	// the display's current mode at the last check (RetryNow on SDR -> HDR only)
		unsigned				SwapSdrRetries = 0;
		unsigned				LastAttempt = 0;	// frame of the last SetHdrOutput (a refused mode is asked at most every 30 frames)
		unsigned				SwitchFrame = 0;	// frame of the last swap chain switch asked of the fork (0: none waiting for its next frame)
		DWORD					SwitchTime = 0;		// GetTickCount then (UNOFFICIAL 2026-11-03)
		NVR_EXT_HDR_INFO		Info = {};			// the fork's view (GetHdrInfo)
		bool					InfoRead = false;
		int						LastSetting = -1;	// the last Windows setting found for LastMonitor (a failed read keeps it)
		HMONITOR				LastMonitor = nullptr;
		bool					SupportedSeen = false;	// DisplayConfig said "advanced colour supported" for it in this run
		int						DisplayHdr = -1;	// -1 unknown, 0 SDR desktop, 1 Windows HDR on (ST.2084)
		std::vector<HMONITOR>	SeenHdr;			// monitors seen in HDR mode in this run (UNOFFICIAL 2026-10-29)
		float					DisplayMaxNits = 0.0f;
		unsigned				Frame = 0;
		char					LastReason[160] = "";
		char					LastCheck[256] = "";	// the display check's last log line (logged when it changes)
		ID3D9NvrExtPipeline*	GainPipeline = nullptr;
		bool					GainTried = false;
		IDirect3DTexture9*		Gain = nullptr;
		IDirect3DSurface9*		GainSurface = nullptr;
		UINT					GainWidth = 0, GainHeight = 0;
		bool					GainReady = false;	// this frame's gain was drawn
		IDirect3DBaseTexture9*	Scene = nullptr;	// this frame's HDR scene before tonemapping (not owned)
		int						CaptureSetting = 0;
		bool					CapturePending = false;
		unsigned				CaptureCount = 0;
		unsigned				CaptureAt = 0;		// frame at which the capture is asked for (60 frames after the setting changed)
	};
	inline State S;

	inline void Reason(const char* text) {
		if (strcmp(S.LastReason, text)) { strncpy_s(S.LastReason, text, _TRUNCATE); Logger::Log("UNOFFICIAL HDR output: %s", text); }
	}

	inline bool SeenHdr(HMONITOR monitor) { return std::find(S.SeenHdr.begin(), S.SeenHdr.end(), monitor) != S.SeenHdr.end(); }

	// Every monitor's current mode (IDXGIOutput6::GetDesc1); Monitor null = all of them. HDR ones go into S.SeenHdr. dxgi.dll is
	// loaded on demand. Hdr / MaxNits: of Monitor (or of any monitor when it is null).
	// UNOFFICIAL 2026-11-03 (tester report "HDR toggle freezes the game"): Windows' own dxgi.dll only (System32), never a dxgi.dll in
	// the game folder (a full DXVK install's, ReShade's, Special K's: a whole second Vulkan instance or a hooking wrapper on the render
	// thread at every check), and the loops end on ANY failure (they ended on DXGI_ERROR_NOT_FOUND only: a wrapper answering another
	// error spun the render thread forever), at most 16 adapters x 16 outputs.
	inline void ReadMonitors(HMONITOR monitor, int& hdr, float& maxNits, std::vector<HMONITOR>& seenHdr) {
		typedef HRESULT(WINAPI* CreateFactory)(REFIID, void**);
		static CreateFactory create = (CreateFactory)GetProcAddress(LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), "CreateDXGIFactory1");
		hdr = 0; maxNits = 0.0f;
		IDXGIFactory1* factory = nullptr;
		if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&factory)) && factory) {
			IDXGIAdapter1* adapter = nullptr;
			for (UINT a = 0; a < 16 && SUCCEEDED(factory->EnumAdapters1(a, &adapter)) && adapter; a++) {
				IDXGIOutput* output = nullptr;
				for (UINT o = 0; o < 16 && SUCCEEDED(adapter->EnumOutputs(o, &output)) && output; o++) {
					IDXGIOutput6* output6 = nullptr;
					if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6))) {
						DXGI_OUTPUT_DESC1 desc = {};
						if (SUCCEEDED(output6->GetDesc1(&desc))) {
							const bool isHdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
							if (isHdr && desc.Monitor && std::find(seenHdr.begin(), seenHdr.end(), desc.Monitor) == seenHdr.end()) seenHdr.push_back(desc.Monitor);
							if (!monitor || desc.Monitor == monitor) {
								if (isHdr) hdr = 1;
								if (desc.MaxLuminance > maxNits) maxNits = desc.MaxLuminance;
							}
						}
						output6->Release();
					}
					output->Release();
				}
				adapter->Release();
			}
			factory->Release();
		}
	}

	// UNOFFICIAL 2026-10-29: the Windows HDR SETTING of a monitor, which (unlike the current mode above) the game's picture does not
	// change. Windows 11 24H2 reports it (DisplayConfig GET_ADVANCED_COLOR_INFO_2: highDynamicRangeUserEnabled, with activeColorMode).
	// Before that it is only in the registry: AdvancedColorEnabled (HDREnabled on newer builds) of
	// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\MonitorDataStore\<entry>, written when the user switches it, never
	// while a game runs (user's entry: last written 16:03:55 although the game ran until 16:06). The entry is the monitor's: <PnP id><EDID serial>_<week>_<year>_<checksum> (MonitorStoreKey, from
	// the EDID of the DisplayConfig target; user's: AOCA610<serial>_24_07E8_6B); a prefix match on the PnP id is used only when
	// exactly one entry has it. The game's monitor is matched by its GDI name (MONITORINFOEX.szDevice = the path's source name).
	// Monitor null = all monitors (NoteDesktop). -1 = not found.
	struct WindowsHdr {
		int			Setting = -1;		// HDR switched on by the user
		const char*	From = "none";		// where Setting came from
		int			Mode = -1;			// Windows 11 24H2 activeColorMode == HDR
		int			Active = -1;		// DisplayConfig advancedColorEnabled / advancedColorActive (also 1 for WCG and auto colour management)
		int			Supported = -1;		// DisplayConfig advancedColorSupported
		int			Disabled = -1;		// DisplayConfig advancedColorForceDisabled / advancedColorLimitedByPolicy
		char		PnpId[16] = "";
		char		Key[96] = "";		// the MonitorDataStore entry read
	};

	// DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2, DisplayConfig type 15; not in SDK 10.0.19041, behind NTDDI_WIN11_GA in 26100)
	struct AdvancedColorInfo2 {
		DISPLAYCONFIG_DEVICE_INFO_HEADER	header;
		UINT32								value;		// bit 0 advancedColorSupported, 1 advancedColorActive, 3 advancedColorLimitedByPolicy, 5 highDynamicRangeUserEnabled
		UINT32								colorEncoding;
		UINT32								bitsPerColorChannel;
		UINT32								activeColorMode;	// 0 SDR, 1 WCG, 2 HDR
	};
	static_assert(sizeof(AdvancedColorInfo2) == 36, "DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 is 36 bytes");

	// The MonitorDataStore entry name of a monitor, from its EDID (the value EDID of
	// HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<PnP id>\<instance>\Device Parameters): the PnP id, the serial number text (display descriptor 0xFF), the week, year and checksum bytes.
	inline bool MonitorStoreKey(const char* pnp, const char* instance, char* key, size_t keySize) {
		char path[256];
		sprintf_s(path, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s\\%s\\Device Parameters", pnp, instance);
		static BYTE edid[1024];
		DWORD size = sizeof(edid);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "EDID", RRF_RT_REG_BINARY, nullptr, edid, &size) != ERROR_SUCCESS || size < 128) return false;
		char serial[16] = "";
		for (int d = 54; d <= 108; d += 18) {
			if (edid[d] || edid[d + 1] || edid[d + 3] != 0xFF) continue;
			int first = d + 5, last = d + 18;
			for (int k = first; k < last; k++) if (edid[k] == 0x0A) { last = k; break; }
			while (first < last && edid[first] == ' ') first++;
			while (last > first && edid[last - 1] == ' ') last--;
			int n = 0;
			for (int k = first; k < last && n < (int)sizeof(serial) - 1; k++) serial[n++] = (char)edid[k];
			serial[n] = 0;
		}
		sprintf_s(key, keySize, "%s%s_%02X_%04X_%02X", pnp, serial, edid[16], 1990 + edid[17], edid[127]);
		return true;
	}

	// The HDR switch of one MonitorDataStore entry (-1: no such entry): HDREnabled where Windows has it (newer builds, where
	// AdvancedColorEnabled can also mean auto colour management on an SDR monitor; Microsoft's HOBL test tool reads it), else
	// AdvancedColorEnabled (Windows 10: the user's 19044 has only that one)
	inline int MonitorStoreSetting(const char* key) {
		char path[160];
		sprintf_s(path, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\MonitorDataStore\\%s", key);
		DWORD value = 0, size = sizeof(value);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "HDREnabled", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) return value ? 1 : 0;
		size = sizeof(value);
		if (RegGetValueA(HKEY_LOCAL_MACHINE, path, "AdvancedColorEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return -1;
		return value ? 1 : 0;
	}

	inline WindowsHdr ReadWindowsSetting(HMONITOR monitor) {
		WindowsHdr result;
		MONITORINFOEXW info = {};
		info.cbSize = sizeof(info);
		if (monitor && !GetMonitorInfoW(monitor, &info)) return result;
		std::vector<DISPLAYCONFIG_PATH_INFO> paths;
		std::vector<DISPLAYCONFIG_MODE_INFO> modes;
		LONG status = ERROR_INSUFFICIENT_BUFFER;
		for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; attempt++) {	// the layout can change between the two calls
			UINT32 pathCount = 0, modeCount = 0;
			if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || !pathCount) return result;
			paths.resize(pathCount);
			modes.resize(modeCount);
			status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
			paths.resize(pathCount);
		}
		if (status != ERROR_SUCCESS) return result;
		for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
			if (monitor) {
				DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
				source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				source.header.size = sizeof(source);
				source.header.adapterId = path.sourceInfo.adapterId;
				source.header.id = path.sourceInfo.id;
				if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || _wcsicmp(source.viewGdiDeviceName, info.szDevice)) continue;
			}
			AdvancedColorInfo2 color2 = {};
			color2.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)15;	// DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
			color2.header.size = sizeof(color2);
			color2.header.adapterId = path.targetInfo.adapterId;
			color2.header.id = path.targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&color2.header) == ERROR_SUCCESS) {	// Windows 11 24H2 (Windows 10: ERROR_INVALID_PARAMETER)
				result.Supported = (std::max)(result.Supported, (int)(color2.value & 1));
				result.Active = (std::max)(result.Active, (int)((color2.value >> 1) & 1));
				result.Disabled = (std::max)(result.Disabled, (int)((color2.value >> 3) & 1));
				result.Mode = (std::max)(result.Mode, color2.activeColorMode == 2 ? 1 : 0);
				result.Setting = (std::max)(result.Setting, (int)((color2.value >> 5) & 1));
				result.From = "Windows";
			}
			else {
				DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
				color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
				color.header.size = sizeof(color);
				color.header.adapterId = path.targetInfo.adapterId;
				color.header.id = path.targetInfo.id;
				if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
					result.Supported = (std::max)(result.Supported, color.advancedColorSupported ? 1 : 0);
					result.Active = (std::max)(result.Active, color.advancedColorEnabled ? 1 : 0);
					result.Disabled = (std::max)(result.Disabled, color.advancedColorForceDisabled ? 1 : 0);
				}
			}
			DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
			target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
			target.header.size = sizeof(target);
			target.header.adapterId = path.targetInfo.adapterId;
			target.header.id = path.targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
			// "\\?\DISPLAY#AOCA610#5&37e6d2eb&1&UID4355#{...}": the PnP id and the device instance between the '#'
			char device[128] = {};
			for (int c = 0; c < 127 && target.monitorDevicePath[c]; c++) device[c] = (char)target.monitorDevicePath[c];
			char* pnp = strchr(device, '#');
			char* instance = pnp ? strchr(pnp + 1, '#') : nullptr;
			char* tail = instance ? strchr(instance + 1, '#') : nullptr;
			if (!tail || instance - pnp - 1 <= 0 || instance - pnp - 1 >= (ptrdiff_t)sizeof(result.PnpId)) continue;
			*instance++ = 0; *tail = 0; pnp++;
			if (!result.PnpId[0]) strcpy_s(result.PnpId, pnp);
			if (!strcmp(result.From, "Windows")) continue;	// Windows 11 24H2 said it: no registry needed
			char key[96];
			int setting = -1;
			if (MonitorStoreKey(pnp, instance, key, sizeof(key))) setting = MonitorStoreSetting(key);
			if (setting < 0) {
				// No exact entry: the one entry whose name starts with the PnP id, if there is exactly one
				HKEY store = nullptr;
				int matches = 0;
				if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\MonitorDataStore", 0, KEY_READ, &store) == ERROR_SUCCESS) {
					char name[256];
					for (DWORD k = 0; ; k++) {
						DWORD nameLength = sizeof(name);
						if (RegEnumKeyExA(store, k, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
						if (!_strnicmp(name, pnp, strlen(pnp)) && ++matches == 1) strcpy_s(key, name);
					}
					RegCloseKey(store);
				}
				if (matches == 1) setting = MonitorStoreSetting(key);
			}
			if (setting >= 0) {
				result.Setting = (std::max)(result.Setting, setting);
				result.From = "registry";
				if (!result.Key[0]) strcpy_s(result.Key, key);
			}
		}
		return result;
	}

	inline const char* OnOff(int value) { return value > 0 ? "on" : value == 0 ? "off" : "not found"; }

	// At plugin load, before the game makes its window and swap chain: read the Windows HDR setting without calling
	// LoadLibraryEx("dxgi.dll") / CreateDXGIFactory1 inside NVSEPlugin_Load (avoids loader/DXGI lock hazards with overlays;
	// DXGI monitor enumeration runs on the background CheckWorker thread once rendering starts).
	inline void NoteDesktop() {
		if (!TheSettingManager->GetSettingI("Shaders.HDR.Main", "Enabled")) return;
		const WindowsHdr windows = ReadWindowsSetting(nullptr);
		Logger::Log("UNOFFICIAL HDR output: at load, before the game's window, Windows HDR setting %s (%s), advanced colour %s",
			OnOff(windows.Setting), windows.From, OnOff(windows.Active));
	}

	// UNOFFICIAL 2026-11-03 (tester log: a 49 s frame 13 s after HDR came on, RTX 4070 Ti SUPER with two virtual display adapters
	// and Sunshine streaming; nothing else HDR output does runs on a timer): the display check's Windows calls (DXGI outputs,
	// DisplayConfig, the registry) go through every display driver, virtual ones included, and ran on the render thread every 600
	// frames: a driver that does not answer froze the game for as long. They now run on a worker thread (one at a time); the render
	// thread starts it (StartCheck) and takes its answer on a later frame (FinishCheck). Until the first answer HDR stays off; a later
	// slow check keeps the last answer (logged when it takes over 50 ms, and once while it is still running after 2 s).
	struct DisplayReading {
		HMONITOR				Monitor = nullptr;
		HWND					Window = nullptr;
		int						Current = 0;
		float					MaxNits = 0.0f;
		WindowsHdr				Windows;
		std::vector<HMONITOR>	Seen;
		DWORD					Started = 0, Took = 0;
	};
	inline DisplayReading	Reading;			// owned by the worker while CheckState is 1
	inline volatile LONG	CheckState = 0;		// 0 idle, 1 the worker is reading, 2 its answer waits for FinishCheck
	inline bool				CheckSlowLogged = false;

	inline DWORD WINAPI CheckWorker(void*) {
		Reading.Seen.clear();
		ReadMonitors(Reading.Monitor, Reading.Current, Reading.MaxNits, Reading.Seen);
		Reading.Windows = Reading.Monitor ? ReadWindowsSetting(Reading.Monitor) : WindowsHdr();
		Reading.Took = GetTickCount() - Reading.Started;
		InterlockedExchange(&CheckState, 2);
		return 0;
	}

	// Render thread: the game window's monitor (cheap), then the worker reads the rest.
	inline void StartCheck(IDirect3DDevice9* Device) {
		if (InterlockedCompareExchange(&CheckState, 0, 0) != 0) return;	// one at a time
		D3DDEVICE_CREATION_PARAMETERS cp = {};
		HMONITOR monitor = nullptr;
		if (SUCCEEDED(Device->GetCreationParameters(&cp))) {
			if (cp.hFocusWindow) monitor = MonitorFromWindow(cp.hFocusWindow, MONITOR_DEFAULTTONEAREST);
			IDirect3D9* d3d = nullptr;
			if (!monitor && SUCCEEDED(Device->GetDirect3D(&d3d)) && d3d) { monitor = d3d->GetAdapterMonitor(cp.AdapterOrdinal); d3d->Release(); }
		}
		Reading.Monitor = monitor;
		Reading.Window = cp.hFocusWindow;
		Reading.Started = GetTickCount();
		CheckSlowLogged = false;
		InterlockedExchange(&CheckState, 1);
		HANDLE thread = CreateThread(nullptr, 0, CheckWorker, nullptr, 0, nullptr);
		if (thread) CloseHandle(thread);
		else InterlockedExchange(&CheckState, 0);
	}

	// Windows' HDR switch for the monitor showing the game window: the worker's answer, applied on the render thread. False: none yet.
	inline bool FinishCheck() {
		const LONG state = InterlockedCompareExchange(&CheckState, 0, 0);
		if (state == 1 && !CheckSlowLogged && GetTickCount() - Reading.Started > 2000) {
			CheckSlowLogged = true;
			Logger::Log("UNOFFICIAL HDR output: the display check has been waiting on Windows for over 2 s (a display driver is not answering); the game goes on with the last answer");
		}
		if (state != 2) return false;
		if (Reading.Took > 50) Logger::Log("UNOFFICIAL HDR output: the display check took %lu ms (on its own thread)", Reading.Took);
		for (HMONITOR seenMonitor : Reading.Seen) if (!SeenHdr(seenMonitor)) S.SeenHdr.push_back(seenMonitor);
		const HMONITOR monitor = Reading.Monitor;
		const int current = Reading.Current;
		float maxNits = Reading.MaxNits;
		WindowsHdr windows = Reading.Windows;
		InterlockedExchange(&CheckState, 0);
		if (monitor != S.LastMonitor) { S.LastMonitor = monitor; S.LastSetting = -1; S.SupportedSeen = false; S.SwapConfirmed = false; }
		if (windows.Setting >= 0) S.LastSetting = windows.Setting;
		else if (S.LastSetting >= 0) { windows.Setting = S.LastSetting; windows.From = "last reading"; }	// a failed read keeps the last answer
		if (windows.Supported > 0) S.SupportedSeen = true;
		// The game not in front: the desktop's own mode is what DXGI reads, so an SDR reading then releases the latch
		const bool background = Reading.Window && GetForegroundWindow() != Reading.Window;
		if (background && !current && monitor && SeenHdr(monitor)) S.SeenHdr.erase(std::find(S.SeenHdr.begin(), S.SeenHdr.end(), monitor));
		const bool seen = monitor && SeenHdr(monitor);
		const bool now = current || windows.Mode > 0;
		int hdr;
		if (windows.Disabled > 0) hdr = 0;	// Windows has HDR force-disabled (policy, e.g. battery saver)
		else if (windows.Setting >= 0) hdr = windows.Setting > 0 || now ? 1 : 0;	// the setting decides (an HDR reading of the current mode also counts)
		else if (windows.Supported == 0 && !S.SupportedSeen) hdr = 0;	// no setting found and Windows says the display cannot do HDR now
		else hdr = now || seen ? 1 : 0;
		if (hdr && S.DisplayHdr == 0) { S.Refused = false; S.RefusedRetries = 0; }	// Windows HDR just came on: ask the fork afresh
		if (current && S.LastCurrent == 0 && (S.Refused || S.SwapSdr)) { S.RetryNow = true; S.SwapConfirmed = false; }	// e.g. alt-tabbed: the desktop went HDR, the surface may offer it now
		S.LastCurrent = current;
		if (maxNits <= 0.0f) maxNits = S.DisplayMaxNits;
		// UNOFFICIAL 2026-11-01 (log privacy): the MonitorDataStore entry holds the monitor's EDID serial number after the PnP id
		// ("AOCA6102OMQ9JA000349_24_07E8_6B"); the log shows it as "AOCA610<SN>_24_07E8_6B".
		char shownKey[sizeof(windows.Key) + 8] = "";
		if (windows.Key[0]) {
			const size_t pnpLength = strnlen(windows.PnpId, sizeof(windows.PnpId));
			const char* rest = strchr(windows.Key + (std::min)(pnpLength, strlen(windows.Key)), '_');
			sprintf_s(shownKey, "%.*s<SN>%s", (int)(std::min)(pnpLength, strlen(windows.Key)), windows.Key, rest ? rest : "");
		}
		char text[256];
		sprintf_s(text, "Windows HDR %s for the game's display %s (setting %s from %s%s%s, now in %s mode, advanced colour %s, supported %s%s, HDR seen earlier %s), peak %.0f nits",
			hdr ? "on" : "off", windows.PnpId[0] ? windows.PnpId : "(unknown)", OnOff(windows.Setting), windows.From, shownKey[0] ? " " : "", shownKey,
			now ? "HDR" : "SDR", OnOff(windows.Active), OnOff(windows.Supported), windows.Disabled > 0 ? ", DISABLED by Windows" : "", seen ? "yes" : "no", maxNits);
		if (strcmp(text, S.LastCheck)) { strcpy_s(S.LastCheck, text); Logger::Log("UNOFFICIAL HDR output: %s", text); }
		S.DisplayHdr = hdr;
		S.DisplayMaxNits = maxNits;
		return true;
	}

	inline bool Setting(const char* key) { return TheSettingManager->GetSettingI("Shaders.HDR.Main", key) != 0; }
	inline float SettingF(const char* key) { return TheSettingManager->GetSettingF("Shaders.HDR.Main", key); }

	inline const char* ModeName(UINT mode) { return mode == NVR_EXT_HDR_SCRGB ? "scRGB" : mode == NVR_EXT_HDR_HDR10 ? "HDR10" : "SDR"; }

	// UNOFFICIAL 2026-11-03 (tester report "HDR toggle freezes the game", not reproduced here: windowed, Windows HDR off, with and
	// without FSR): every fork call that rebuilds the swap chain (a mode change, or Rebuild: the same mode asked again) is logged
	// before and after, and the next frame's arrival with the time it took (BeginFrame). The log is flushed line by line, so after a
	// freeze its last HDR line says where it stopped: "asking ..." = inside the fork's SetHdrOutput; "answered" with no "next frame"
	// = at the next present (the fork's swap chain rebuild, FalloutNV_d3d9.log then shows how far it got).
	inline HRESULT Send(const NVR_EXT_HDR_OUTPUT& out, bool rebuild) {
		const bool switching = rebuild || out.Mode != S.ForkMode;
		if (switching) Logger::Log("UNOFFICIAL HDR output: asking the fork for %s (was %s%s): its swap chain is rebuilt at the next present", ModeName(out.Mode), ModeName(S.ForkMode), rebuild ? ", asked again" : "");
		else Logger::Log("UNOFFICIAL HDR output: updating %s parameters (paper white %.0f, peak %.0f, flags 0x%X)", ModeName(out.Mode), out.PaperWhiteNits, out.PeakNits, (unsigned)out.Flags);
		const DWORD start = GetTickCount();
		const HRESULT hr = S.Hdr->SetHdrOutput(&out);
		if (switching) {
			Logger::Log("UNOFFICIAL HDR output: the fork answered %08lX in %lu ms", (unsigned long)hr, GetTickCount() - start);
			S.SwitchFrame = S.Frame;
			S.SwitchTime = GetTickCount();
			S.SwapConfirmed = false;
		}
		else {
			Logger::Log("UNOFFICIAL HDR output: parameter update answered %08lX in %lu ms", (unsigned long)hr, GetTickCount() - start);
		}
		return hr;
	}

	inline void SetOff() {
		if (S.Hdr && (S.Active || S.ForkMode != NVR_EXT_HDR_OFF)) {
			NVR_EXT_HDR_OUTPUT off = {};
			Send(off, false);
		}
		S.ForkMode = NVR_EXT_HDR_OFF;
		S.Active = false;
		S.Applied = {};
		S.SwapSdr = false;
		S.SwapConfirmed = false;
		S.SwapSdrRetries = 0;
	}

	// Every rendered frame, before NVR's post chain (Render.cpp): reads the settings, keeps the fork's mode in step.
	inline void BeginFrame(IDirect3DDevice9* Device, IDirect3DBaseTexture9* Scene) {
		S.Scene = Scene;
		S.GainReady = false;
		S.Frame++;
		if (S.SwitchFrame && S.Frame > S.SwitchFrame) {	// the switch frame's own present (the rebuild) came back
			Logger::Log("UNOFFICIAL HDR output: the frame after the switch arrived, %lu ms after it was asked", GetTickCount() - S.SwitchTime);
			S.SwitchFrame = 0;
		}
		const bool enabled = Setting("Enabled");
		if (!enabled) {
			if (S.Active || S.ForkMode != NVR_EXT_HDR_OFF) { SetOff(); Reason("off"); }
			S.Refused = false; S.RefusedRetries = 0; S.RetryNow = false; S.Applied = {}; S.DisplayHdr = -1;	// UNOFFICIAL 2026-10-29
			InterlockedCompareExchange(&CheckState, 0, 2);	// UNOFFICIAL 2026-11-03: an answer that came in while off is dropped
			return;
		}
		if (!S.Queried) {
			S.Queried = true;
			if (DxvkCompute::Extension())
				DxvkCompute::Extension()->QueryInterface(__uuidof(ID3D9NvrExtHdr), (void**)&S.Hdr);
		}
		if (!S.Hdr) { Reason("Enabled, but this d3d9.dll has no HDR output (needs Vegas Shaders DXVK with extension 1.5-hdr): SDR as before"); return; }
		// The fork's view (its surface formats and the EDID; the fork logs the formats when they change), once before the display check
		if (!S.InfoRead) {
			S.InfoRead = true;
			S.Hdr->GetHdrInfo(&S.Info);
			Logger::Log("UNOFFICIAL HDR output: the fork's surface offers HDR10 %s, scRGB %s; EDID peak %.0f nits", S.Info.SupportsHdr10 ? "yes" : "no", S.Info.SupportsScRgb ? "yes" : "no", S.Info.DisplayMaxNits);
		}
		if (!S.SwitchFrame && (S.DisplayHdr < 0 || S.Frame % 600 == 0)) StartCheck(Device);	// UNOFFICIAL 2026-11-03: on its own thread (never overlapping a swapchain switch)
		const bool check = FinishCheck();	// a new answer this frame (what the old every-600-frames check did, a few frames later)
		if (S.DisplayHdr < 0 && !Setting("IgnoreDisplayCheck")) { Reason("Enabled: checking the display"); return; }
		if (!S.DisplayHdr && !Setting("IgnoreDisplayCheck")) {
			if (S.Active || S.ForkMode != NVR_EXT_HDR_OFF) SetOff();
			Reason("Enabled, but Windows HDR is off for this display: SDR as before (turn HDR on in Windows' display settings)");
			return;
		}

		// UNOFFICIAL 2026-10-29: while on, the fork says at each check whether its swap chain really is HDR (ActiveMode is OFF when
		// the driver gave it an SDR format; the fork then shows the SDR picture as before and logs it)
		// UNOFFICIAL 2026-11-21: only query GetHdrInfo() on the render thread until the HDR swap chain is confirmed
		// (or while recovering from SDR), avoiding redundant WSI surface queries under D3D9DeviceLock every 600 frames.
		if (check && S.Active && (!S.SwapConfirmed || S.SwapSdr)) {
			NVR_EXT_HDR_INFO now = {};
			const DWORD t0 = GetTickCount();
			const HRESULT hr = S.Hdr->GetHdrInfo(&now);
			const DWORD dt = GetTickCount() - t0;
			if (dt > 20) Logger::Log("UNOFFICIAL HDR output: GetHdrInfo took %lu ms (hr %08lX)", (unsigned long)dt, (unsigned long)hr);
			if (SUCCEEDED(hr)) {
				S.SwapSdr = now.ActiveMode == NVR_EXT_HDR_OFF;
				if (!S.SwapSdr) S.SwapConfirmed = true;
				if (S.SwapSdr) Reason("on, but the fork's swap chain is SDR (the driver did not give it an HDR format; DXVK's log says which): SDR picture");
				else if (!strncmp(S.LastReason, "on, but", 7) || !strncmp(S.LastReason, "asked", 5)) Reason("on: the fork's swap chain is HDR");
			}
		}
		if (S.Active && S.SwapSdr && (S.RetryNow || (check && S.SwapSdrRetries < 3))) {
			// The same settings again: the fork rebuilds its swap chain once more (the display may be back in HDR now)
			if (!S.RetryNow) S.SwapSdrRetries++;
			S.RetryNow = false;
			S.SwapSdr = false;
			S.LastAttempt = S.Frame;
			Send(S.Applied, true);
			char text[160];
			sprintf_s(text, "asked the fork again for %s (its swap chain was SDR), attempt %u", S.Applied.Mode == NVR_EXT_HDR_SCRGB ? "scRGB" : "HDR10", S.SwapSdrRetries);
			Reason(text);
		}
		const NVR_EXT_HDR_INFO& info = S.Info;
		NVR_EXT_HDR_OUTPUT out = {};
		out.Mode = TheSettingManager->GetSettingI("Shaders.HDR.Main", "Mode") == 2 ? NVR_EXT_HDR_SCRGB : NVR_EXT_HDR_HDR10;
		out.PaperWhiteNits = std::clamp(SettingF("PaperWhiteNits"), 80.0f, 1000.0f);
		float peak = SettingF("PeakNits");
		if (peak <= 0.0f) peak = S.DisplayMaxNits > 0.0f ? S.DisplayMaxNits : (info.DisplayMaxNits > 0.0f ? info.DisplayMaxNits : 1000.0f);
		out.PeakNits = std::clamp(peak, out.PaperWhiteNits, 10000.0f);
		out.MaxFrameAverageNits = out.PaperWhiteNits;
		out.HudThreshold = std::clamp(SettingF("HudThreshold"), 0.0f, 255.0f) / 255.0f;
		out.Flags = (Setting("ShowHudMask") ? NVR_EXT_HDR_FLAG_SHOW_HUD_MASK : 0) | (Setting("Gamma22") ? NVR_EXT_HDR_FLAG_GAMMA22 : 0);

		if (!S.Active || memcmp(&out, &S.Applied, sizeof(out))) {
			const bool same = !memcmp(&out, &S.Applied, sizeof(out));
			if (S.Refused) {
				// UNOFFICIAL 2026-10-29: a refused mode is asked again with the display check (6 times) and whenever the display
				// reads HDR (alt-tabbed); changed settings at most every 30 frames (a slider being dragged)
				if (same) {
					const bool tick = check && S.RefusedRetries < 6;
					if (!tick && !S.RetryNow) return;
					if (tick) S.RefusedRetries++;
				}
				else if (S.Frame - S.LastAttempt < 30) return;
				else S.RefusedRetries = 0;
			}
			else if (S.Active && !same && S.Frame - S.LastAttempt < 10) return;	// a slider being dragged: at most every 10 frames
			S.RetryNow = false;
			S.LastAttempt = S.Frame;
			S.Applied = out;
			const bool wasForkOn = S.ForkMode != NVR_EXT_HDR_OFF;
			const HRESULT hr = Send(out, false);
			S.Refused = FAILED(hr);
			S.Active = SUCCEEDED(hr);
			if (S.Active) S.ForkMode = out.Mode;
			else if (wasForkOn) {
				// A refused change while on leaves the fork in its old mode: off, so NVR and the fork agree (the fork never refuses OFF)
				NVR_EXT_HDR_OUTPUT off = {};
				Send(off, false);
				S.ForkMode = NVR_EXT_HDR_OFF;
				S.SwapSdr = false;
				S.SwapConfirmed = false;
				S.SwapSdrRetries = 0;
			}
			if (S.Refused) S.Hdr->GetHdrInfo(&S.Info);	// the fork logs the surface formats again if they changed
			char text[160];
			if (S.Active) sprintf_s(text, "on: %s, paper white %.0f nits, peak %.0f nits%s", out.Mode == NVR_EXT_HDR_SCRGB ? "scRGB" : "HDR10", out.PaperWhiteNits, out.PeakNits, S.DisplayHdr ? "" : " (display check ignored: Windows HDR is off)");
			else sprintf_s(text, "the fork refused %s (%08lX), attempt %u, display now in %s mode: SDR as before%s", out.Mode == NVR_EXT_HDR_SCRGB ? "scRGB" : "HDR10", hr,
				S.RefusedRetries + 1, strstr(S.LastCheck, "now in HDR") ? "HDR" : "SDR", S.RefusedRetries >= 6 ? " (no more timed attempts; alt-tab to try again)" : "");
			Reason(text);
		}
	}

	// The gain pass (HdrGain.frag) into an R16F texture of the SDR picture's size. Sdr: the tonemapper's output (the game target
	// at the start of NVR's post chain).
	inline void RenderGain(IDirect3DDevice9* Device, IDirect3DSurface9* Sdr) {
		if (!S.Active || !S.Scene || !Sdr || !DxvkCompute::Available()) return;
		D3DSURFACE_DESC desc = {};
		if (FAILED(Sdr->GetDesc(&desc))) return;
		if (!S.Gain || S.GainWidth != desc.Width || S.GainHeight != desc.Height) {
			DxvkCompute::Drop(S.GainSurface); DxvkCompute::Drop(S.Gain);
			if (FAILED(Device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &S.Gain, nullptr)) || !S.Gain) { Reason("could not create the gain texture: SDR picture only"); return; }
			S.Gain->GetSurfaceLevel(0, &S.GainSurface);
			S.GainWidth = desc.Width; S.GainHeight = desc.Height;
		}
		if (!S.GainTried) {
			S.GainTried = true;
			if (!DxvkCompute::HasSpirv("HdrGain")) { Reason("Compute\\HdrGain.spv is missing: the HDR output shows the SDR picture at paper white"); return; }
			NVR_EXT_BINDING_DESC bindings[3] = {};
			bindings[0].Set = 1; bindings[0].Binding = 0; bindings[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[0].Slot = 0;
			bindings[1].Set = 1; bindings[1].Binding = 1; bindings[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[1].Slot = 1;
			bindings[2].Type = NVR_EXT_BIND_SAMPLER; bindings[2].Slot = 0; bindings[2].PushOffset = 32;
			S.GainPipeline = DxvkCompute::CreateGraphicsPipeline("HdrGain", bindings, 3, 36, true);
		}
		ID3D9NvrExtSampler* sampler = DxvkCompute::PointClamp();
		if (!S.GainPipeline || !sampler) return;

		struct { float peak, knee, strength, sceneGamma, exposure, outGamma, pad0, pad1; UINT sampler; } push = {};
		push.peak = (std::max)(1.0f, S.Applied.PeakNits / S.Applied.PaperWhiteNits);
		push.knee = std::clamp(SettingF("HighlightKnee"), 0.0f, 0.99f);
		push.strength = std::clamp(SettingF("HighlightStrength"), 0.0f, 1.0f);
		const D3DXVECTOR4 toneMapping = TheShaderManager->Shaders.Tonemapping ? TheShaderManager->Shaders.Tonemapping->Constants.ToneMapping : D3DXVECTOR4(0, 0, 0, 2.2f);
		const D3DXVECTOR4 hdrData = TheShaderManager->Shaders.Tonemapping ? TheShaderManager->Shaders.Tonemapping->Constants.HDRData : D3DXVECTOR4(0, 1, 1, 2.2f);
		const bool gammaSpace = toneMapping.w < 0.0f || hdrData.w < 0.0f;	// the tonemapper's gamma-space path: sRGB curves
		push.sceneGamma = gammaSpace ? 0.0f : toneMapping.w;
		push.exposure = hdrData.y;
		push.outGamma = gammaSpace ? 0.0f : hdrData.w;

		NVR_EXT_RESOURCE_BINDING resources[3] = {};
		resources[0].Slot = 0; resources[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[0].pResource = S.Scene;
		resources[1].Slot = 1; resources[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[1].pResource = Sdr;
		resources[2].Slot = 0; resources[2].Type = NVR_EXT_BIND_SAMPLER; resources[2].pResource = sampler;
		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = S.GainPipeline;
		draw.TargetCount = 1;
		draw.Targets[0].pResource = S.GainSurface;
		draw.BindingCount = 3;
		draw.pBindings = resources;
		draw.pPushData = &push;
		draw.PushDataSize = sizeof(push);
		DxvkCompute::PreLoad(resources, 3);	// UNOFFICIAL 2026-10-27 (merge with the alflicker fix): managed textures on the GPU (DxvkCompute.h)
		const HRESULT hr = DxvkCompute::Extension()->Draw(&draw);
		if (FAILED(hr)) { Reason("the gain pass failed (DXVK's log says why): SDR picture at paper white"); DxvkCompute::Drop(S.GainPipeline); return; }
		S.GainReady = true;
	}

	// After NVR's last effect (and ReShade's, which draws before the HUD), before the HUD: the fork copies the picture and the gain.
	inline void MarkScene(IDirect3DSurface9* Finished) {
		if (!S.Active || !S.GainReady || !Finished) return;
		const HRESULT hr = S.Hdr->MarkHdrScene(Finished, S.GainSurface);
		if (FAILED(hr)) {
			char text[160];
			sprintf_s(text, "MarkHdrScene failed (%08lX; DXVK's log says why): HUD-free pixels shown without highlight gain", (unsigned long)hr);
			Reason(text);
		}
	}

	// Test tool, once a frame (Render.cpp, after MarkScene): CaptureFrame changed -> ask the fork; the frame after -> read and save.
	inline void Capture() {
		if (!S.Hdr) return;
		if (S.CapturePending) {
			S.CapturePending = false;
			UINT w = 0, h = 0;
			if (S.Hdr->ReadHdrCapture(&w, &h, nullptr, 0) != S_OK || !w || !h) { Logger::Log("UNOFFICIAL HDR output: capture: nothing captured"); return; }
			std::vector<unsigned short> half((size_t)w * h * 4);
			if (FAILED(S.Hdr->ReadHdrCapture(&w, &h, half.data(), (UINT)(half.size() * 2)))) { Logger::Log("UNOFFICIAL HDR output: capture: read failed"); return; }
			auto toFloat = [](unsigned short v) {
				const unsigned sign = (v >> 15) & 1, exp = (v >> 10) & 31, man = v & 1023;
				float f = exp == 0 ? ldexpf((float)man, -24) : exp == 31 ? 65504.0f : ldexpf((float)(man | 1024), (int)exp - 25);
				return sign ? -f : f;
			};
			char path[MAX_PATH];
			S.CaptureCount++;
			// NeonSelfTest_*.world: the name the slot test runner moves to the run's picture folder (multirun run_slot.sh move_world)
			sprintf_s(path, ".\\" VS_LOGS_FOLDER "\\NeonSelfTest_HdrCapture_%u.pfm.world", (unsigned)S.CaptureSetting);
			FILE* file = nullptr;
			double sum = 0.0; float peak = 0.0f; size_t above = 0, total = (size_t)w * h;
			std::vector<float> row((size_t)w * 3);
			if (!fopen_s(&file, path, "wb") && file) {
				fprintf(file, "PF\n%u %u\n-1.0\n", w, h);
				for (int y = (int)h - 1; y >= 0; y--) {
					for (UINT x = 0; x < w; x++) for (int c = 0; c < 3; c++) row[x * 3 + c] = toFloat(half[((size_t)y * w + x) * 4 + c]) * 80.0f;
					fwrite(row.data(), sizeof(float), row.size(), file);
				}
				fclose(file);
			}
			for (size_t i = 0; i < total; i++) {
				const float y = (0.2126f * toFloat(half[i * 4]) + 0.7152f * toFloat(half[i * 4 + 1]) + 0.0722f * toFloat(half[i * 4 + 2])) * 80.0f;
				sum += y; peak = (std::max)(peak, y);
				if (y > S.Applied.PaperWhiteNits * 1.02f) above++;
			}
			Logger::Log("UNOFFICIAL HDR output: capture %u (%ux%u, nits, linear): mean %.1f, max %.1f, %.3f %% of pixels above paper white %.0f -> %s",
				(unsigned)S.CaptureSetting, w, h, sum / total, peak, 100.0 * above / total, S.Applied.PaperWhiteNits, path);
			return;
		}
		const int setting = TheSettingManager->GetSettingI("Shaders.HDR.Main", "CaptureFrame");
		if (setting != 0 && setting != S.CaptureSetting) { S.CaptureSetting = setting; S.CaptureAt = S.Frame + 60; }
		else if (setting == 0) { S.CaptureSetting = 0; S.CaptureAt = 0; }
		if (S.CaptureAt && S.Frame >= S.CaptureAt) {
			S.CaptureAt = 0;
			if (SUCCEEDED(S.Hdr->RequestHdrCapture())) S.CapturePending = true;
		}
	}
}
