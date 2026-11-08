// (2026-11-07 rt-shutdown) The helper's way out (NvrRtHelper.cpp ShutdownDrain) and the game's order (RtCore.cpp Core::Stop = Host::Stop
// here), each on a SECOND host of its own (its own D3D9 device, semaphores and helper with its own log, NvrRtHelper-shutdown-*.log next
// to the helper), so the main helper is not touched:
//   quit  the game quits with frames pending: one frame run, then 4 frames (the ring) published as the game does them (inputs ready
//         signalled, a GPU wait for output ready recorded on our queue) WITHOUT waking the helper, then quit at once: the helper must
//         release every one (output ready reaches the last), go idle, exit by itself (code 0) and say "clean"; our queue then drains.
//   kill  the game dies mid-flight: the helper's parent is a stand-in process (this exe --idle) killed while frames are in flight: one
//         frame published and woken whose inputs ready never reaches the GPU (the game died before its signal: the helper's submission
//         waits for it), two more published and never woken. The helper must release them all (inputs ready signalled from the CPU,
//         logged FORCED, when its submission was already waiting), go idle and exit by itself (code 0) within a few seconds. Nothing of
//         those frames is recorded on our queue (a dead game's queue is gone), so our own queue is never left waiting on them.
// Both: no hang (every wait here is bounded), the helper's last status line ("exit: output ready N of N ...") and log lines printed.

static std::string gShutdownHelperExe, gShutdownDevice;

// a frame record published (latestFrame) with nothing on our GPU queue: neither inputs ready nor a wait for output ready
static bool shutdownPublishOnly(Host& h, bool wake) {
	const uint64_t n = ++h.frame;
	for (int i = 0; i < 5000 && h.shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n; i++) Sleep(1);
	if (h.shared->consumedFrame + (int64_t)NVR_RT_FRAME_RING < (int64_t)n) { say("  error: the helper did not read the ring\n"); return false; }
	NvrRtFrame& f = h.shared->frames[n % NVR_RT_FRAME_RING];
	f.frame = 0;
	MemoryBarrier();
	f.arenaTexels = 0; f.opCount = f.instanceCount = f.passCount = 0;
	LARGE_INTEGER q; QueryPerformanceCounter(&q); f.writtenQpc = q.QuadPart;
	MemoryBarrier();
	f.frame = n;
	MemoryBarrier();
	InterlockedExchange64((volatile LONG64*)&h.shared->latestFrame, (LONG64)n);
	if (wake) SetEvent(h.frameEvent);
	return true;
}

// the helper's own last count ("exit: output ready N of M ..."): after the helper exits, the host has read its output ready as 2^64 - 1
// (Windows / the driver let go of the shared fence), so the value it saw while alive comes from its status line
static uint64_t shutdownToldOutput(const char* msg) {
	unsigned long long v = 0;
	const char* p = std::strstr(msg, "exit: output ready ");
	return p && std::sscanf(p, "exit: output ready %llu", &v) == 1 ? uint64_t(v) : 0;
}

// the helper's shutdown lines from its own log
static void shutdownLogLines(const wchar_t* logName) {
	char exe[MAX_PATH]; GetFullPathNameA(gShutdownHelperExe.c_str(), MAX_PATH, exe, nullptr);
	std::string path(exe);
	path = path.substr(0, path.find_last_of("\\/") + 1);
	char name[64]; WideCharToMultiByte(CP_ACP, 0, logName, -1, name, sizeof(name), nullptr, nullptr);
	path += name;
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "r") || !f) { say("    (no helper log at %s)\n", path.c_str()); return; }
	char line[1024];
	while (std::fgets(line, sizeof(line), f))
		if (std::strstr(line, "shutdown") || std::strstr(line, "exit") || std::strstr(line, "the game") || std::strstr(line, "asked the helper"))
			say("    helper log: %s", line);
	std::fclose(f);
}

static bool sceneShutdownQuit() {
	Host h;
	const wchar_t* logName = L"NvrRtHelper-shutdown-quit.log";
	h.helperArgs = std::wstring(L" --log ") + logName;
	if (!h.Init(gShutdownDevice.c_str())) { h.Release(); return false; }
	if (!h.Start(gShutdownHelperExe.c_str())) { h.Stop(3000); h.Release(); return false; }
	bool ok = false;
	do {
		if (!h.Exchange(World(), {}, V3()) || !h.Finish()) break;
		h.EndFrame();
		bool published = true;
		for (int k = 0; k < (int)NVR_RT_FRAME_RING && published; k++) published = h.Exchange(World(), {}, V3(), false);
		if (!published) break;
		const uint64_t latest = h.frame;
		UINT64 atQuit = 0; h.outputReady->GetCompletedValue(&atQuit);
		const int64_t submittedAtQuit = h.shared->submittedFrame;
		h.shared->quit = 1;
		MemoryBarrier();
		SetEvent(h.frameEvent);
		const ULONGLONG t = GetTickCount64();
		const bool exited = WaitForSingleObject(h.helper.hProcess, 8000) == WAIT_OBJECT_0;
		const ULONGLONG ms = GetTickCount64() - t;
		DWORD code = 99; GetExitCodeProcess(h.helper.hProcess, &code);
		UINT64 out = 0; h.outputReady->GetCompletedValue(&out);
		const bool drained = exited && h.Drain(5000);
		char msg[sizeof(h.shared->message)]; std::memcpy(msg, (const void*)h.shared->message, sizeof(msg)); msg[sizeof(msg) - 1] = 0;
		say("  quit: frames 1..%llu published (%llu run and %lld submitted when quit was set); helper %s in %llu ms (code %lu), output ready %llu, our queue %s\n"
			"    status: %s\n", (unsigned long long)latest, (unsigned long long)atQuit, (long long)submittedAtQuit, exited ? "exited" : "DID NOT EXIT",
			(unsigned long long)ms, code, (unsigned long long)out, drained ? "drained" : "NOT drained", msg);
		const uint64_t told = shutdownToldOutput(msg);
		say("    output ready by the helper's own count before it exited: %llu (the read above is after its exit)\n", (unsigned long long)told);
		ok = exited && code == 0 && told >= latest && drained && std::strstr(msg, "exit:") && std::strstr(msg, "clean") && std::strstr(msg, "GPU idle");
	} while (false);
	h.Stop(3000);
	h.Release();
	shutdownLogLines(logName);
	return ok;
}

static bool sceneShutdownKill() {
	char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
	std::string cmd = "\"" + std::string(self) + "\" --idle 60000";
	STARTUPINFOA su = { sizeof(su) };
	PROCESS_INFORMATION game = {};
	if (!CreateProcessA(self, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &su, &game)) { say("  error: the stand-in game process (%lu)\n", GetLastError()); return false; }
	Host h;
	const wchar_t* logName = L"NvrRtHelper-shutdown-kill.log";
	h.helperArgs = std::wstring(L" --log ") + logName;
	bool ok = false;
	if (h.Init(gShutdownDevice.c_str())) {
		h.shared->parentPid = game.dwProcessId;	// the helper's "game" is the stand-in
		if (h.Start(gShutdownHelperExe.c_str())) do {
			if (!h.Exchange(World(), {}, V3()) || !h.Finish()) break;
			h.EndFrame();
			if (!shutdownPublishOnly(h, true)) break;	// its inputs ready never comes
			Sleep(300);	// the helper submits it: its queue waits for inputs ready = this frame
			const int64_t submittedAtKill = h.shared->submittedFrame;
			if (!shutdownPublishOnly(h, false) || !shutdownPublishOnly(h, false)) break;	// never woken, never run
			const uint64_t latest = h.frame;
			TerminateProcess(game.hProcess, 1);
			WaitForSingleObject(game.hProcess, 2000);
			const ULONGLONG t = GetTickCount64();
			const bool exited = WaitForSingleObject(h.helper.hProcess, 10000) == WAIT_OBJECT_0;
			const ULONGLONG ms = GetTickCount64() - t;
			DWORD code = 99; GetExitCodeProcess(h.helper.hProcess, &code);
			UINT64 out = 0, in = 0; h.outputReady->GetCompletedValue(&out); h.inputsReady->GetCompletedValue(&in);
			char msg[sizeof(h.shared->message)]; std::memcpy(msg, (const void*)h.shared->message, sizeof(msg)); msg[sizeof(msg) - 1] = 0;
			say("  kill: frames 1..%llu published (%lld submitted when the game was killed; frame 2's inputs ready never signalled by the game); helper %s %llu ms "
				"after the kill (code %lu), output ready %llu, inputs ready %llu\n    status: %s\n", (unsigned long long)latest, (long long)submittedAtKill,
				exited ? "exited" : "DID NOT EXIT", (unsigned long long)ms, code, (unsigned long long)out, (unsigned long long)in, msg);
			const uint64_t told = shutdownToldOutput(msg);
			say("    output ready by the helper's own count before it exited: %llu (the reads above are after its exit)\n", (unsigned long long)told);
			ok = exited && code == 0 && told >= latest && std::strstr(msg, "exit:") && std::strstr(msg, "GPU idle");
		} while (false);
	}
	if (WaitForSingleObject(game.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(game.hProcess, 1);
	CloseHandle(game.hProcess); CloseHandle(game.hThread);
	const bool clean = h.Stop(3000);	// (the helper is gone: our queue's own drain; a release from here if it left anything)
	if (!clean) { say("  kill: our side had to force the shutdown -> FAIL\n"); ok = false; }
	h.Release();
	shutdownLogLines(logName);
	return ok;
}

static bool sceneShutdown() {
	const bool quit = sceneShutdownQuit();
	say("  quit with frames pending: %s\n", quit ? "PASS" : "FAIL");
	if (!quit) return false;	// (the kill case only after the gentle one worked)
	const bool kill = sceneShutdownKill();
	say("  game killed mid-flight: %s\n", kill ? "PASS" : "FAIL");
	return kill;
}
