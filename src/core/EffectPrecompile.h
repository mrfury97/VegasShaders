#pragma once

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "EffectCompiler47.h"	// UNOFFICIAL 2026-10-29 NewEffectCompiler

// UNOFFICIAL 2026-10-28 (user: "we should port it"): NVR's effects compiled in parallel at startup, after Conor (mrfury97)'s fork
// (github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, commit ee3ac888, GPL-3.0-or-later: its ShaderPrecompile also does the effects;
// this is our version of that idea, credit his). ParallelShaderCompile (ShaderPrecompile.h, ours) only did the game's shaders; the
// effects (the .fx.hlsl post passes, about 50, VolumetricClouds alone ~2.5 min without a cache) were loaded one after another before
// the game asks for any shader, so a start without a cache took their sum.
// Start (ShaderManager, before the first effect is registered) preprocesses every effect with LoadEffect's defines (EffectRecord::
// BuildMacros, worked out once on the main thread), checks the cache as LoadEffect does, and compiles the stale ones on worker threads,
// biggest first, writing the cache files exactly as LoadEffect writes them (the compiled .fx and the PortableSource). LoadEffect waits
// for its effect's job (Wait) and then finds its cache fresh and loads it. A job that fails writes nothing: LoadEffect compiles that
// effect itself and reports the error, as before. [Main.Main.Performance] ParallelShaderCompile switches both.
namespace EffectPrecompile {

	struct Job {
		std::string Source, Preprocessed, Compiled;
		D3DXMACRO Macros[8] = {};
		unsigned long long Size = 0;
		std::atomic<bool> Done{ false };
		bool Waited = false;
	};

	inline std::vector<Job*> Jobs;
	inline std::unordered_map<std::string, Job*> ByCompiledPath;
	inline std::atomic<size_t> NextJob{ 0 };
	inline std::atomic<unsigned> CompiledCount{ 0 };
	inline bool Started = false;
	inline unsigned WaitedCount = 0;
	inline double StartTime = 0.0;

	inline bool WriteFile(const std::string& path, const void* data, size_t size) {
		FILE* f = nullptr;
		if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
		fwrite(data, 1, size, f);
		fclose(f);
		return true;
	}

	// One effect on a worker thread: LoadEffect's preprocess, cache check and compile (the same calls, the same files).
	inline void Run(Job* job) {
		ID3DXBuffer* source = nullptr;
		ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXPreprocessShaderFromFileA(job->Source.c_str(), job->Macros, NULL, &source, &errors)) || !source) {
			if (errors) errors->Release();
			if (source) source->Release();
			return;	// LoadEffect preprocesses it again and reports the error
		}
		if (errors) { errors->Release(); errors = nullptr; }
		bool fresh = EffectCompiler47::CacheFresh(job->Preprocessed.c_str(), source) && ShaderProgram::FileExists(job->Compiled.c_str());
		if (!fresh) fresh = ShaderProgram::SeedFromShipped(job->Preprocessed.c_str(), job->Compiled.c_str(), EffectCompiler47::CacheText(source));	// UNOFFICIAL 2026-11-10 shipped cache
		if (!fresh) {
			ID3DXEffectCompiler* compiler = nullptr;
			ID3DXBuffer* binary = nullptr;
			// UNOFFICIAL 2026-10-29 NewEffectCompiler: d3dcompiler_47 first (seconds instead of minutes on the big effects)
			ID3DXBuffer* newErrors = nullptr;
			if (EffectCompiler47::Compile(source, job->Source.c_str(), &binary, &newErrors)) {
				const std::string portable = EffectCompiler47::CacheText(source);
				if (WriteFile(job->Compiled, binary->GetBufferPointer(), binary->GetBufferSize()))
					WriteFile(job->Preprocessed, portable.data(), portable.size());
				CompiledCount++;
				binary->Release();
				binary = nullptr;
			}
			else if (SUCCEEDED(D3DXCreateEffectCompiler((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, NULL, &compiler, &errors)) && compiler) {
				if (errors) { errors->Release(); errors = nullptr; }
				if (SUCCEEDED(compiler->CompileEffect(NULL, &binary, &errors)) && binary) {
					const std::string portable = EffectCompiler47::CacheText(source);
					if (WriteFile(job->Compiled, binary->GetBufferPointer(), binary->GetBufferSize()))
						WriteFile(job->Preprocessed, portable.data(), portable.size());
					CompiledCount++;
				}
				compiler->Release();
			}
			if (binary) binary->Release();
			if (errors) errors->Release();
			if (newErrors) newErrors->Release();
		}
		source->Release();
	}

	inline void Worker() {
		for (;;) {
			const size_t i = NextJob.fetch_add(1);
			if (i >= Jobs.size()) return;
			Run(Jobs[i]);
			Jobs[i]->Done = true;
		}
	}

	// ShaderManager, before the first RegisterEffect (main thread): every Effects\*.fx.hlsl, then the workers.
	inline void Start() {
		if (Started) return;
		Started = true;
		if (!TheSettingManager->GetSettingI("Main.Main.Performance", "ParallelShaderCompile")) return;
		StartTime = (double)GetTickCount64();
		EffectCompiler47::ReadSetting();	// UNOFFICIAL 2026-10-29 (main thread; the workers read Use)
		D3DXMACRO macros[8] = {};
		EffectRecord::BuildMacros(macros);	// string literals: safe to share with the workers
		CreateDirectoryA((std::string(EffectsPath) + "Cache\\").c_str(), NULL);
		WIN32_FIND_DATAA find;
		HANDLE h = FindFirstFileA((std::string(EffectsPath) + "*.fx.hlsl").c_str(), &find);
		if (h == INVALID_HANDLE_VALUE) return;
		do {
			std::string file(find.cFileName);
			if (file.size() <= 8) continue;
			const std::string name = file.substr(0, file.size() - 8);	// without ".fx.hlsl"
			Job* job = new Job();
			job->Source = std::string(EffectsPath) + file;
			job->Preprocessed = std::string(EffectsPath) + "Cache\\" + name + ".fx.hlsl";
			job->Compiled = std::string(EffectsPath) + "Cache\\" + name + ".fx";
			job->Size = ((unsigned long long)find.nFileSizeHigh << 32) | find.nFileSizeLow;
			std::copy(std::begin(macros), std::end(macros), job->Macros);
			Jobs.push_back(job);
			ByCompiledPath[job->Compiled] = job;
		} while (FindNextFileA(h, &find));
		FindClose(h);
		// Biggest first: the slowest (VolumetricClouds, VolumetricFog) start at once instead of last.
		std::stable_sort(Jobs.begin(), Jobs.end(), [](const Job* a, const Job* b) { return a->Size > b->Size; });
		const unsigned cores = std::thread::hardware_concurrency();
		const unsigned threads = (std::max)(1u, (std::min)(cores > 1 ? cores - 1 : 1u, 12u));
		for (unsigned i = 0; i < threads; i++) std::thread(Worker).detach();	// (detached: effects never loaded are compiled and left)
		Logger::Log("UNOFFICIAL ParallelShaderCompile: %u effects on %u threads (biggest first).", (unsigned)Jobs.size(), threads);
	}

	// LoadEffect, before its own preprocess: waits for this effect's job (the first load only); its cache is then fresh.
	inline void Wait(const char* CompiledPath) {
		if (Jobs.empty()) return;
		auto it = ByCompiledPath.find(CompiledPath);
		if (it == ByCompiledPath.end() || it->second->Waited) return;
		Job* job = it->second;
		while (!job->Done) Sleep(1);
		job->Waited = true;
		if (++WaitedCount == 1 || WaitedCount == Jobs.size())
			Logger::Log("UNOFFICIAL ParallelShaderCompile: %u effect(s) ready, %u compiled so far, %.1f s after the start.", WaitedCount,
				CompiledCount.load(), ((double)GetTickCount64() - StartTime) / 1000.0);
	}
}
