#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RegisterClash.h"	// UNOFFICIAL 2026-10-31 register-clash guard

// UNOFFICIAL 2026-10-14 (user: "can u make the nvr shader compiler run in parallel threads"). The game asks for its shaders
// one at a time while it starts, and NVR compiled each replacement right there: preprocess, compare with the cache, compile
// what changed (about 0.5-1.5 s for a big template). Now the first request starts the same work for every shader NVR will be
// asked for (the collections' templates, their interior records, the hand-written files, the shadow shaders) on all but one
// core; each request then takes its finished result (waiting for that one if it is still running). The files and the
// shaders are exactly what ShaderRecord::LoadShader makes alone: the same defines (ShaderRecord::BuildDefines, worked out on
// the main thread), the same cache comparison and the same compile. [Main.Main.Performance] ParallelShaderCompile (on).
namespace ShaderPrecompile {

	struct Job {
		std::string Name, Source, Preprocessed, Compiled;
		ShaderTemplate Template = {};
		std::string Log;
		ID3DXBuffer* SourceBuffer = nullptr;
		ID3DXBuffer* ShaderBuffer = nullptr;
		bool Preprocessed_ok = false;
		std::atomic<bool> Done{ false };
		bool Taken = false;
	};

	inline std::vector<Job*> Jobs;
	inline std::unordered_map<std::string, Job*> ByCompiledPath;
	inline std::atomic<size_t> NextJob{ 0 };
	inline bool Started = false;
	inline double StartTime = 0.0;

	// The paths ShaderRecord::LoadShader builds (kept the same here).
	inline void Paths(const char* Name, const char* SubPath, const char* CacheSubPath, const char* TemplateName, Job* job) {
		std::string base = ShadersPath, cache = std::string(ShadersPath) + "Cache\\";
		if (SubPath) base += SubPath;
		if (CacheSubPath) { cache += CacheSubPath; CreateDirectoryA(cache.c_str(), NULL); }
		else if (SubPath) cache += SubPath;
		job->Source = base + (TemplateName ? TemplateName : Name) + ".hlsl";
		job->Preprocessed = cache + Name + ".hlsl";
		job->Compiled = cache + Name;
	}

	// One shader on a worker thread: as LoadShader's preprocess, cache check and compile.
	inline void Run(Job* job) {
		ID3DXBuffer* errors = nullptr;
		HRESULT prepass = D3DXPreprocessShaderFromFileA(job->Source.c_str(), &job->Template.Defines[0], NULL, &job->SourceBuffer, &errors);
		if (prepass != D3D_OK) {
			if (errors) { job->Log += (char*)errors->GetBufferPointer(); errors->Release(); }
			return;
		}
		if (errors) { errors->Release(); errors = nullptr; }
		job->Preprocessed_ok = true;
		bool fresh = ShaderProgram::CheckPreprocessResult(job->Preprocessed.c_str(), job->SourceBuffer);
		if (!fresh) fresh = ShaderProgram::SeedFromShipped(job->Preprocessed.c_str(), job->Compiled.c_str(), ShaderProgram::PortableSource(job->SourceBuffer));	// UNOFFICIAL 2026-11-10 shipped cache
		if (fresh) {
			FILE* f = nullptr;
			if (!fopen_s(&f, job->Compiled.c_str(), "rb") && f) {
				fseek(f, 0, SEEK_END);
				long size = ftell(f);
				fseek(f, 0, SEEK_SET);
				if (size > 0 && SUCCEEDED(D3DXCreateBuffer(size, &job->ShaderBuffer)))
					if (fread(job->ShaderBuffer->GetBufferPointer(), 1, size, f) != (size_t)size) { job->ShaderBuffer->Release(); job->ShaderBuffer = nullptr; }
				fclose(f);
			}
			if (job->ShaderBuffer) return;
		}
		const char* profile = strstr(job->Name.c_str(), ".vso") ? "vs_3_0" : "ps_3_0";
		ID3DXConstantTable* table = nullptr;
		D3DXCompileShader((const char*)job->SourceBuffer->GetBufferPointer(), job->SourceBuffer->GetBufferSize(), NULL, NULL, "main",
			profile, NULL, &job->ShaderBuffer, &errors, &table);
		// UNOFFICIAL 2026-10-29: out of temp registers (X4505): again with PREFER_FLOW_CONTROL, as ShaderRecord::LoadShader (see there)
		if (!job->ShaderBuffer && errors && strstr((const char*)errors->GetBufferPointer(), "X4505")) {
			job->Log += "out of temp registers at the default flags; compiled again with PREFER_FLOW_CONTROL\n";
			errors->Release(); errors = nullptr;
			if (table) { table->Release(); table = nullptr; }
			D3DXCompileShader((const char*)job->SourceBuffer->GetBufferPointer(), job->SourceBuffer->GetBufferSize(), NULL, NULL, "main",
				profile, D3DXSHADER_PREFER_FLOW_CONTROL, &job->ShaderBuffer, &errors, &table);
		}
		if (table) table->Release();
		if (errors) { job->Log += (char*)errors->GetBufferPointer(); errors->Release(); }
		if (job->ShaderBuffer) {
			FILE* f = nullptr;
			if (!fopen_s(&f, job->Compiled.c_str(), "wb") && f) {
				fwrite(job->ShaderBuffer->GetBufferPointer(), 1, job->ShaderBuffer->GetBufferSize(), f);
				fclose(f);
			}
			const std::string portable = ShaderProgram::PortableSource(job->SourceBuffer);
			if (!fopen_s(&f, job->Preprocessed.c_str(), "wb") && f) {
				fwrite(portable.data(), 1, portable.size(), f);
				fclose(f);
			}
			job->Log += "Shader compiled (parallel): " + job->Compiled;
		}
	}

	inline void Worker() {
		for (;;) {
			const size_t i = NextJob.fetch_add(1);
			if (i >= Jobs.size()) return;
			Run(Jobs[i]);
			Jobs[i]->Done = true;
		}
	}

	inline void Add(const char* Name, const char* SubPath, const char* CacheSubPath, ShaderTemplate Template, const D3DXMACRO* Overrides) {
		Job* job = new Job();
		job->Name = Name;
		Paths(Name, SubPath, CacheSubPath, Template.Name, job);
		if (!ShaderProgram::FileExists(job->Source.c_str()) || ByCompiledPath.count(job->Compiled)) { delete job; return; }
		job->Template = Template;
		ShaderRecord::BuildDefines(job->Template, Overrides);
		Jobs.push_back(job);
		ByCompiledPath[job->Compiled] = job;
	}

	// From the first ShaderRecord::LoadShader (main thread): the job list, then the workers.
	inline void StartOnce() {
		if (Started) return;
		if (!TheShaderManager || TheShaderManager->ShaderNames.empty()) return;	// not set up yet: try again on the next shader
		Started = true;
		if (!TheSettingManager->GetSettingI("Main.Main.Performance", "ParallelShaderCompile")) return;
		StartTime = (double)GetTickCount64();
		static D3DXMACRO interior[] = { { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" },
			{ "CONTACT_HARDENING", "0" }, { NULL, NULL }, { NULL, NULL } };
		// UNOFFICIAL 2026-10-21 LampFalloff: as ShaderManager::LoadShader's interior records (PBRShaders::LampFalloffInteriorDefine).
		if (const char* lampFalloff = PBRShaders::LampFalloffInteriorDefine()) interior[4] = { "LAMP_FALLOFF_ON", lampFalloff };
		const bool interiorShadows = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Forward", "Enabled") != 0;
		// UNOFFICIAL 2026-11-09 (players: shader compiling takes up to 5 minutes since 1.0.6) the OUTDOOR lamp-shadow records (ShaderManager::LoadShader,
		// RT lamp shadows + ExteriorForward: one more record per lit pixel shader, Cache\ExteriorLamps\) were never queued here, so ~87 of the
		// biggest shaders compiled one after another on the main thread: about 130 of the 190 s of a full compile in the 11-08 log (59 s
		// without them). 1.0.6's RT presets switch them on. Queued now with the same source folder, cache folder and defines as that load's
		// first level (the skin shaders' smaller fall-back levels still compile there if the full one does not fit).
		const bool exteriorLampRecords = interiorShadows && TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "Enabled") != 0 &&
			TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "ExteriorForward") != 0;
		static D3DXMACRO exteriorLamps[] = { { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { NULL, NULL } };
		// (the skin shaders' two smaller levels as well: SKIN2010 / SKIN2011 end on Fit2, 5 s each on the main thread in the bench run)
		static D3DXMACRO exteriorFit1[] = { { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { "SKIN_PT_LAMPS", "2" }, { NULL, NULL } };
		static D3DXMACRO exteriorFit2[] = { { "INTERIOR_SHADOWS", "1" }, { "POINT_SHADOW_RT_ONLY", "1" }, { "SKIN_PT_LAMPS", "1" }, { NULL, NULL } };
		auto addExteriorLamps = [&](const char* name, const ShaderTemplate& tmpl) {
			const bool skin = tmpl.Name && strstr(tmpl.Name, "SkinTemplate") == tmpl.Name;
			static const char* const folders[3] = { "ExteriorLamps\\", "ExteriorLamps\\Fit1\\", "ExteriorLamps\\Fit2\\" };	// (2026-11-11 mrfury97: a single backslash before Fit1 / Fit2 was C4129, "\F" = "F": the Fit levels compiled into ExteriorLampsFit1\, a folder the load never reads)
			const D3DXMACRO* const sets[3] = { exteriorLamps, exteriorFit1, exteriorFit2 };
			// (2026-11-11 mrfury97: the skin's MANY variants with 3+ lamps start at Fit1, as ShaderManager::LoadShader does: the all-lamp level never fits)
			int first = 0;
			if (skin) { bool many = false; int points = 0;
				for (const D3DXMACRO& d : tmpl.Defines) { if (d.Name && !strcmp(d.Name, "MANY")) many = true; if (d.Name && !strcmp(d.Name, "POINTS") && d.Definition) points = atoi(d.Definition); }
				if (many && points >= 3) first = 1; }
			for (int level = first; level < (skin ? 3 : 1); level++) {
				const size_t before = Jobs.size();
				Add(name, "Exteriors\\", folders[level], tmpl, sets[level]);	// (the game's outdoor version of the shader, where the mod has one)
				if (Jobs.size() == before) Add(name, NULL, folders[level], tmpl, sets[level]);
			}
		};
		// The land's outdoor records (TerrainTemplate with lamps, TERRAIN_LAMP_RT; not with Terrain Blending: ShaderManager::LoadShader): 21 more.
		const bool terrainLampRecords = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "Enabled") != 0 &&
			TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "ExteriorForward") != 0 && !TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Enabled");
		static D3DXMACRO terrainLamps[] = { { "TERRAIN_LAMP_RT", "1" }, { NULL, NULL } };
		auto addTerrainLamps = [&](const char* name, const ShaderTemplate& tmpl) {
			if (!terrainLampRecords || !tmpl.Name || strcmp(tmpl.Name, "TerrainTemplate")) return;
			bool lamps = false;
			for (const D3DXMACRO& d : tmpl.Defines) if (d.Name && !strcmp(d.Name, "NUM_PT_LIGHTS")) lamps = true;
			if (lamps) Add(name, NULL, "ExteriorLamps\\", tmpl, terrainLamps);
		};
		std::unordered_set<std::string> templated;
		for (const auto& [CollectionName, Pointer] : TheShaderManager->ShaderNames) {
			ShaderCollection* Collection = Pointer ? *Pointer : nullptr;
			if (!Collection) continue;
			for (const auto& [Name, Template] : Collection->Templates()) {
				const std::string name(Name);
				templated.insert(name);
				Add(name.c_str(), NULL, NULL, Template, NULL);
				if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pso") == 0) addTerrainLamps(name.c_str(), Template);
				if (interiorShadows && name.size() > 4 && name.compare(name.size() - 4, 4, ".pso") == 0 && PointShadowForward::Eligible(name.c_str(), Template))
				{
					Add(name.c_str(), NULL, "Interiors\\", Template, interior);
					if (exteriorLampRecords) addExteriorLamps(name.c_str(), Template);
				}
			}
		}
		// The hand-written ones (no template): every Name.hlsl the game's shader names map to a collection.
		WIN32_FIND_DATAA find;
		HANDLE h = FindFirstFileA((std::string(ShadersPath) + "*.?so.hlsl").c_str(), &find);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				std::string name(find.cFileName);
				name.resize(name.size() - 5);	// without ".hlsl"
				if (templated.count(name) || !TheShaderManager->GetShaderCollection(name.c_str())) continue;
				Add(name.c_str(), NULL, NULL, ShaderTemplate{}, NULL);
				if (interiorShadows && name.compare(name.size() - 4, 4, ".pso") == 0 && PointShadowForward::Eligible(name.c_str(), ShaderTemplate{}))
				{
					Add(name.c_str(), NULL, "Interiors\\", ShaderTemplate{}, interior);
					if (exteriorLampRecords) addExteriorLamps(name.c_str(), ShaderTemplate{});
				}
			} while (FindNextFileA(h, &find));
			FindClose(h);
		}
		// The shadow shaders (ShadowManager loads them from Shadows\).
		h = FindFirstFileA((std::string(ShadersPath) + "Shadows\\*.?so.hlsl").c_str(), &find);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				std::string name(find.cFileName);
				name.resize(name.size() - 5);
				Add(name.c_str(), "Shadows\\", NULL, ShaderTemplate{}, NULL);
			} while (FindNextFileA(h, &find));
			FindClose(h);
		}
		const unsigned cores = std::thread::hardware_concurrency();
		const unsigned threads = (std::max)(1u, (std::min)(cores > 1 ? cores - 1 : 1u, 12u));
		for (unsigned i = 0; i < threads; i++) std::thread(Worker).detach();	// (detached: shaders the game never asks for are compiled and left)
		Logger::Log("UNOFFICIAL ParallelShaderCompile: %u shaders on %u threads.", (unsigned)Jobs.size(), threads);
	}

	// From ShaderRecord::LoadShader: this shader's finished result, if a worker made it (waits for it if still running).
	// The caller owns the buffers then. False: not one of ours, do it as before.
	inline bool Take(const char* CompiledPath, ID3DXBuffer** Source, ID3DXBuffer** Shader) {
		if (!Started || Jobs.empty()) return false;
		auto it = ByCompiledPath.find(CompiledPath);
		if (it == ByCompiledPath.end() || it->second->Taken) return false;
		Job* job = it->second;
		while (!job->Done) Sleep(1);
		job->Taken = true;
		if (!job->Log.empty()) { Logger::Log("%s", job->Log.c_str()); RegisterClash::Check(job->Name.c_str(), job->Log.c_str()); }	// UNOFFICIAL 2026-10-31 (X4500)
		if (!job->Preprocessed_ok) { if (job->SourceBuffer) job->SourceBuffer->Release(); return false; }
		*Source = job->SourceBuffer;
		*Shader = job->ShaderBuffer;
		job->SourceBuffer = job->ShaderBuffer = nullptr;
		static size_t taken = 0;
		if (++taken == Jobs.size()) {
			Logger::Log("UNOFFICIAL ParallelShaderCompile: all %u taken, %.1f s after the start.", (unsigned)Jobs.size(), ((double)GetTickCount64() - StartTime) / 1000.0);
		}
		return true;
	}
}
