// Developer switches: wait for a debugger to attach (debug builds only) / hook the D3D9 device creation.
#define NVR_WAIT_FOR_DEBUGGER 0
#define NVR_HOOK_DEVICE 0

#include "../src/NewVegas/Hooks/Hooks.h"
#include "../src/core/Device/Hook.h"
#include "../src/NewVegas/Managers.h"
#include "../src/core/EffectSelfTest.h"
void HdrOutputNoteDesktop();	// in Render.cpp (UNOFFICIAL 2026-10-29)
#include "../src/core/InstallCheck.h"	// 2026-11-03: the install self-check (old DLL / folders / textures / presets), shown on screen
#include "../src/core/SessionLog.h"	// UNOFFICIAL 2026-11-01: session header, game events, exit summary
#include "../src/core/IOWorker2.h"	// UNOFFICIAL 2026-10-20 (EXPERIMENTAL, off by default): a second loading thread for model / texture tasks
#include "../src/core/DxvkLoader.h"	// UNOFFICIAL 2026-11-04: the DXVK fork's d3d9.dll from a mod manager's mod
#include "../src/core/RtCore.h"	// UNOFFICIAL 2026-11-07 (rt-shutdown): RtCore::Shutdown at the game's exit

namespace {
	const UInt32 kPluginVersion = 441;

	// 2026-11-04 (a tester: "fatal error occurred at 18B2C559 while loading plugin", twice, the log stopped after its first
	// line): xNVSE shows only the address, and every DLL loads somewhere else at each start, so it names no code. While
	// NVSEPlugin_Load runs, the first hardware fault on its thread (an access violation and the like; not the C++ exceptions the
	// settings code throws and catches itself) is logged with the module and the offset in it, and the load step, before xNVSE's
	// handler disables the plugin. It only looks: the search for a handler goes on as before.
	// v2 (the same tester, the fault-logging DLL: the same error, still in no code of ours): a C++ exception nobody catches is
	// reported by xNVSE at Windows' RaiseException, so C++ exceptions are now named too (their type, from the compiler's throw
	// information), the first 8 of the load; the settings code throws and catches some on purpose, so a named one is not
	// necessarily the cause: the last one before the load stops is.
	// v3 (bench, an old MSVCP140 in the game folder: a plugin loaded after this one crashed while the game read its INIs, and
	// CrashLogger's handler crashed too, so no unhandled-exception filter ran and nothing was logged): the watch now stays until
	// DeferredInit (the main menu), and after the load it logs the first hardware fault on ANY thread. It looks first, before
	// every handler, so a handler that crashes does not hide it. A fault some code handles itself is harmless: the line says so.
	const char* LoadStep = "start";
	DWORD LoadThread = 0;
	bool LoadRunning = false;	// inside NVSEPlugin_Load (C++ exceptions are named only then, on its thread)
	LONG CALLBACK LoadFaultLogger(EXCEPTION_POINTERS* info) {
		const EXCEPTION_RECORD* record = info->ExceptionRecord;
		const bool loadThread = GetCurrentThreadId() == LoadThread;
		if (LoadRunning && !loadThread) return EXCEPTION_CONTINUE_SEARCH;
		if (record->ExceptionCode == 0xE06D7363) {
			if (!LoadRunning) return EXCEPTION_CONTINUE_SEARCH;
			static int named = 0;
			if (named >= 8) return EXCEPTION_CONTINUE_SEARCH;
			const char* what = "";
			__try {
				const char* type = SessionLog::CppExceptionType(record);
				// std::out_of_range: the settings code's own lookups of keys a file does not have (a dozen in every start, caught)
				if (!strcmp(type, ".?AVout_of_range@std@@")) return EXCEPTION_CONTINUE_SEARCH;
				named++;
				Logger::Log("LOAD: C++ exception %s while %s (caught later or not: the last one before the load stops is the cause)", type, LoadStep);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Logger::Log("LOAD: C++ exception (type unreadable) while %s%s", LoadStep, what);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
		if (record->ExceptionCode < 0xC0000000) return EXCEPTION_CONTINUE_SEARCH;	// notices
		static bool logged = false;
		if (logged) return EXCEPTION_CONTINUE_SEARCH;
		logged = true;
		HMODULE module = nullptr;
		char path[MAX_PATH] = "(no module)";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)record->ExceptionAddress, &module))
			GetModuleFileNameA(module, path, MAX_PATH);
		const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
		if (LoadRunning)
			Logger::Log("[ERROR] LOAD: exception %08lX at %p = %s+0x%lX while %s (if the plugin then stops loading, this is the cause)",
				(unsigned long)record->ExceptionCode, record->ExceptionAddress, name,
				(unsigned long)((UINT_PTR)record->ExceptionAddress - (UINT_PTR)module), LoadStep);
		else
			Logger::Log("[ERROR] START-UP: exception %08lX at %p = %s+0x%lX on the %s thread, after this plugin's load and before the main menu "
				"(if the game then closes, this is the cause; if it goes on, the code handled it and it is harmless)",
				(unsigned long)record->ExceptionCode, record->ExceptionAddress, name,
				(unsigned long)((UINT_PTR)record->ExceptionAddress - (UINT_PTR)module), loadThread ? "main" : "another");
		return EXCEPTION_CONTINUE_SEARCH;
	}
	void* LoadFaultHandler = nullptr;	// NVSEPlugin_Load to DeferredInit
	struct LoadFaultWatch {	// NVSEPlugin_Load: on from its start; at its end only the load-only checks stop (LoadRunning)
		LoadFaultWatch() { LoadThread = GetCurrentThreadId(); LoadRunning = true; LoadFaultHandler = AddVectoredExceptionHandler(1, LoadFaultLogger); }
		~LoadFaultWatch() { LoadRunning = false; }
	};

	// NVR needs some plugins to run at all: without one, say so and quit the game.
	void RequirePlugin(const wchar_t* dllName, const wchar_t* missingText) {
		if (GetModuleHandle(dllName)) return;
		MessageBox(NULL, missingText, L"Vegas Shaders", MB_OK | MB_ICONERROR);
		ExitProcess(0);
	}

	// After every plugin is loaded: find the optional plugins NVR talks to.
	void LinkOptionalPlugins() {
		// JohnnyGuitar NVSE: the view-model clip distance (used by the depth combine)
		CombineDepthEffect* combine = TheShaderManager->Effects.CombineDepth;
		if (!combine->johnnyguitar) combine->johnnyguitar = GetModuleHandle(L"johnnyguitar.dll");
		if (!combine->johnnyguitar) {
			Logger::Log("JG not found");
		}
		else {
			Logger::Log("JG found, initializing functions");
			HMODULE jg = combine->johnnyguitar;
			combine->JG_SetClipDist = reinterpret_cast<bool(__cdecl*)(float)>(GetProcAddress(jg, "JGSetViewmodelClipDistance"));
			combine->JG_GetClipDist = reinterpret_cast<float(__cdecl*)()>(GetProcAddress(jg, "JGGetViewmodelClipDistance"));
		}

		// Vanilla Plus AO present: NVR's ambient occlusion knows the game's AO is replaced
		if (GetModuleHandle(L"VanillaPlusAO.dll")) TheShaderManager->Effects.AmbientOcclusion->bNVAOLoaded = true;

		// (Vanilla Plus Skin is detected earlier, in InitializeRendererHook (core/Hooks/GameCommon.cpp): the game has made all
		// its SKIN shaders by the time this message arrives, so knowing it here would be too late.)

		// Real Time Menus: asks it which menus keep the game running
		if (HMODULE realTimeMenus = GetModuleHandle(L"RealTimeMenus.dll")) {
			Logger::Log("Real Time Menus found, initializing functions");
			TheGameMenuManager->IsLiveMenu = reinterpret_cast<GameMenuManager::MenuPauseState(__cdecl*)(uint32_t, bool, bool)>(GetProcAddress(realTimeMenus, "IsLiveMenu"));
		}

		// every pass of the game's SLS vertex shaders updates the eye position
		ShadowLightShader::EnableEyePositionForAllPasses();
	}
}

namespace { typedef NVSEMessagingInterface Msg; }

extern "C" {

	// xNVSE's own messages.
	static void MessageHandler(NVSEMessagingInterface::Message* message) {
		SessionLog::StageScope stage("an xNVSE message");	// 2026-11-04 (CRASH lines)
		switch (message->type) {
		case Msg::kMessage_PostLoad:
			SessionLog::ReassertCrashReport("after every plugin loaded");	// 2026-11-04: a later plugin's crash handler under ours
			RequirePlugin(L"LODFlickerFix.dll", L"LOD Flicker Fix not found.\nVegas Shaders cannot be used without it, please install it.");
			RequirePlugin(L"VanillaPlusTerrain.dll", L"Vanilla Plus Terrain not found.\nVegas Shaders cannot be used without it, please install it.");
#if NVR_TEST_TOOLS
			EffectSelfTest::RegisterLink();	// UNOFFICIAL, TEST ONLY: after every plugin is loaded (critic-notes 14)
#endif
			return;
#if NVR_TEST_TOOLS
		case Msg::kMessage_MainGameLoop:
			SettingShots::MainLoop();	// UNOFFICIAL, TEST ONLY: the auto-test's console commands, between frames (SettingShots.h)
			return;
#endif
		case Msg::kMessage_PreLoadGame:
			TheSettingManager->GameLoading = true;
			SessionLog::OnEvent("loading a save");	// UNOFFICIAL 2026-11-01 (SessionLog.h; the save's name and path are never logged)
			return;
		case Msg::kMessage_PostLoadGame: {
			TheSettingManager->GameLoading = false;
			TheShaderManager->InitializeConstants();
			const bool loaded = ((UInt32)message->data & 0xFF) != 0;	// (xNVSE passes the bool as the pointer's value)
			SessionLog::OnEvent(loaded ? "save loaded" : "save load FAILED");
			return;
		}
		// UNOFFICIAL 2026-11-01: the other game events for the log (SessionLog.h)
		case Msg::kMessage_SaveGame:
			SessionLog::OnEvent("game saved");
			return;
		case Msg::kMessage_NewGame:
			SessionLog::OnEvent("new game");
			return;
		case Msg::kMessage_ExitToMainMenu:
			SessionLog::OnEvent("exit to the main menu");
			return;
		// UNOFFICIAL 2026-11-07 (rt-shutdown): the RT helper's shutdown first (RtCore::Shutdown: no GPU wait left for a value it never
		// signals; ~5 s at most). The window's close (Alt+F4, the X) goes through ImGuiManager's window procedure.
		case Msg::kMessage_ExitGame:
			RtCore::Shutdown("the game is exiting (exit to Windows)");
			SessionLog::OnExit("exit to Windows");
			return;
		case Msg::kMessage_ExitGame_Console:
			RtCore::Shutdown("the game is exiting (qqq)");
			SessionLog::OnExit("exit with the qqq console command");
			return;
		case Msg::kMessage_DeferredInit:
			if (LoadFaultHandler) { RemoveVectoredExceptionHandler(LoadFaultHandler); LoadFaultHandler = nullptr; }	// the start-up watch ends here
			SessionLog::OnDeferredInit();	// UNOFFICIAL 2026-11-01: game INI values, load order, plugin DLLs
			LinkOptionalPlugins();
			return;
		}
	}

	// The Shader Loader plugin's message 0: it replaced shaders, so refresh the eye-position flags and reload every effect.
	static void ShaderLoaderHandler(NVSEMessagingInterface::Message* message) {
		if (message->type != 0) return;
		ShadowLightShader::EnableEyePositionForAllPasses();
		TheShaderManager->EffectReloadQueued = true;
	}

	bool NVSEPlugin_Query(const PluginInterface* nvse, PluginInfo* info) {
		info->InfoVersion = PluginInfo::kInfoVersion;
		info->Name = "NewVegasReloaded";	// kept after the Vegas Shaders rename: mods look the plugin up by this name
		info->Version = kPluginVersion;
		return true;
	}

	bool NVSEPlugin_Load(const PluginInterface* nvse) {
#if _DEBUG && NVR_WAIT_FOR_DEBUGGER
		while (IsDebuggerPresent() == FALSE) Sleep(10);	// attach now
#endif
#if NVR_HOOK_DEVICE
		AttachDeviceHooks();
#endif

		Logger::InitializeRotating(Paths::LogsFolder, Paths::LogBaseName, 25);	// VegasShaders-Logs\VegasShaders_<date>.log
		const LoadFaultWatch watch;	// 2026-11-04: a fault during the load is logged with its module (LoadFaultLogger)

		LoadStep = "reading the game's and the plugin's version";
		PluginVersion::CreateVersionString();
		Logger::Log("%s", PluginVersion::VersionString);	// (not the format: a '%' in a version string would read random arguments)
		LoadStep = "choosing the folders";
		Paths::LogChoice();	// 2026-11-02: the folders in use (new Vegas Shaders names, or the old ones of an older install)
		{
			// An old-named DLL left next to this one (or this one under the old name next to a new one) loads the plugin twice.
			char self[MAX_PATH] = {};
			HMODULE module = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&NVSEPlugin_Load, &module);
			GetModuleFileNameA(module, self, MAX_PATH);
			const char* selfName = strrchr(self, '\\') ? strrchr(self, '\\') + 1 : self;
			const std::string other = Paths::ModuleDir() + (_stricmp(selfName, Paths::DllName) == 0 ? Paths::OldDllName : Paths::DllName);
			if (GetFileAttributesA(other.c_str()) != INVALID_FILE_ATTRIBUTES) {
				Logger::Log("[ERROR] PATHS: %s is also in the plugins folder; two copies of the plugin may load. Keep only %s.", other.c_str(), Paths::DllName);
				Paths::TwinDll = other;	// 2026-11-03: the large warning on the main menu and in game (ImGuiManager RenderTwinDllWarning)
			}
		}
		LoadStep = "the DXVK loader";
		if (!nvse->IsEditor) DxvkLoader::Load();	// before anything asks which d3d9.dll this is, and before the game makes its Direct3D
		LoadStep = "the install check";
		InstallCheck::OnPluginLoad(nvse->Version, nvse->GameVersion);
		LoadStep = "the session log header";
		SessionLog::OnPluginLoad(nvse->Version, nvse->GameVersion, kPluginVersion);	// UNOFFICIAL 2026-11-01: the session header (SessionLog.h)
		SessionLog::ArmCrashReport();	// 2026-11-04: CRASH lines from here on, at start-up and in the main menu too

		LoadStep = "registering the console commands";
		CommandManager::Initialize(nvse);
		if (nvse->IsEditor) return true;	// nothing else in the editor

		const PluginHandle self = nvse->GetPluginHandle();
		NVSEMessagingInterface* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
		messaging->RegisterListener(self, "NVSE", MessageHandler);
		messaging->RegisterListener(self, "Shader Loader", ShaderLoaderHandler);
		// UNOFFICIAL, TEST ONLY: the auto-test plugin's messages (EffectSelfTest.h); without that plugin nothing is ever sent.
#if NVR_TEST_TOOLS
		EffectSelfTest::LinkMessaging = messaging;
		EffectSelfTest::LinkHandle = self;
#endif
		// (The listener itself is registered at kMessage_PostLoad: xNVSE refuses a listener for a plugin it has not loaded yet,
		// and the load order between this plugin and NVRAutoTest follows the DLL names. critic-notes 14, after the calibration's exit 33.)

		// xNVSE's services: DirectInput control, player controls, console
		if (NVSEDataInterface* data = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data))
			g_DIHookCtrl = (DIHookControl*)data->GetSingleton(NVSEDataInterface::kNVSEData_DIHookControl);
		g_PlayerControls = (NVSETogglePlayerControlsInterface*)nvse->QueryInterface(kInterface_PlayerControls);
		g_ConsoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);

		// settings first (hooks read them), then presets, then the hooks
		LoadStep = "reading the settings";
		SettingManager::Initialize();
		LoadStep = "loading the settings";
		TheSettingManager->LoadSettings();
		LoadStep = "logging the changed settings";
		SessionLog::OnSettingsLoaded();	// UNOFFICIAL 2026-11-01: every setting that is not its default
		if (!nvse->IsEditor) {	// UNOFFICIAL 2026-11-09 (DxvkLoader.h): before the game makes its Direct3D; a missing key reads as on
			SettingManager::Configuration::ConfigNode node;
			DxvkLoader::DescriptorHeapOff(!TheSettingManager->Config.FillNode(&node, "Main.Main.Misc", "DxvkDescriptorHeapOff") || node.BoolValue);
		}
		LoadStep = "reading the presets";
		PresetManager::Initialize();
		LoadStep = "installing the hooks";
		AttachHooks();
		LoadStep = "the HDR display check";
		HdrOutputNoteDesktop();	// UNOFFICIAL 2026-10-29: Windows' HDR state before the game's fullscreen window changes it (HdrOutput.h)
		LoadStep = "IOWorker2";
		IOWorker2::Install();	// UNOFFICIAL 2026-10-20: IOWorker2 = 1 only; must run before the game creates its IOManager
		return true;
	}

};
