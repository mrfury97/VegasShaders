#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RegisterClash.h"	// UNOFFICIAL 2026-10-31 register-clash guard
#include "LateInteriors.h"	// UNOFFICIAL 2026-11-16 fast-restart (the Interior records' jobs after the start's)
#include "LightFreeRecords.h"	// UNOFFICIAL 2026-11-16 fast-restart (the lighting-free records on the workers)
#include "CompileProgress.h"	// UNOFFICIAL 2026-11-20: startup shader-compile progress overlay
#include "DeferredWetness.h"	// UNOFFICIAL 2026-11-26 faster-shader-compile (the LightFreeWet records' jobs)

// UNOFFICIAL 2026-10-14 (user: "can u make the nvr shader compiler run in parallel threads"). The game asks for its shaders
// one at a time while it starts, and NVR compiled each replacement right there: preprocess, compare with the cache, compile
// what changed (about 0.5-1.5 s for a big template). Now the first request starts the same work for every shader NVR will be
// asked for (the collections' templates, their interior records, the hand-written files, the shadow shaders) on all but one
// core; each request then takes its finished result (waiting for that one if it is still running). The files and the
// shaders are exactly what ShaderRecord::LoadShader makes alone: the same defines (ShaderRecord::BuildDefines, worked out on
// the main thread), the same cache comparison and the same compile. [Main.Main.Performance] ParallelShaderCompile (on).
namespace ShaderPrecompile {

	// UNOFFICIAL 2026-11-16 fast-restart (core/ForwardCloudShadows.h): the preprocessed source holds the clouds' lookup (Includes/Shadow.hlsl
	// NVR_CloudSun, the land's NvrCloudSun, TerrainTemplate's vertex NVR_CloudSunVS, DeferredSun's): compiled into every outdoor record now.
	// (2026-11-26 moved here from ShaderRecord.cpp, which calls these: the workers make the build without the lookup too)
	inline bool CloudCodeIn(ID3DXBuffer* source) {
		if (!source) return false;
		const char* text = (const char*)source->GetBufferPointer();
		const char* end = text + source->GetBufferSize();
		static const char key[] = "NVR_CloudSun";
		static const char land[] = "NvrCloudSun";
		return std::search(text, end, key, key + sizeof(key) - 1) != end || std::search(text, end, land, land + sizeof(land) - 1) != end;
	}
	inline bool OverridesName(const D3DXMACRO* overrides, const char* name) {
		for (const D3DXMACRO* o = overrides; o && o->Name; o++) if (!strcmp(o->Name, name)) return true;
		return false;
	}

	struct Job {
		std::string Name, Source, Preprocessed, Compiled;
		ShaderTemplate Template = {};
		std::vector<std::string> OwnedDefineValues;	// UNOFFICIAL 2026-11-23: owns dynamic define strings built on main thread so workers never race static char[] buffers in BuildDefines
		std::string Log;
		ID3DXBuffer* SourceBuffer = nullptr;
		ID3DXBuffer* ShaderBuffer = nullptr;
		bool Preprocessed_ok = false;
		double CompileMs = -1.0;	// UNOFFICIAL 2026-11-11 the compile's milliseconds (-1: a cache hit or no compile); read only once Done
		bool CompileFailed = false;
		std::atomic<bool> Done{ false };
		bool Taken = false;
		bool ReleaseAfterCache = false;	// UNOFFICIAL 2026-11-23: LateInteriors jobs release buffers right after caching to disk to save 32-bit VA at main menu
		bool BuffersReleased = false;
		std::string DefineKey;	// UNOFFICIAL 2026-11-16 fast-restart: the define list the job was built with (Take compares the load's)
		// UNOFFICIAL 2026-11-26 faster-shader-compile: the record's build without the clouds' lookup (Cache\NoCloud\..., FORWARD_CLOUD_SHADOWS 0),
		// made by the same worker right after this job failed, the build ShaderRecord::LoadShader's ForwardCloudShadows retry asks for next.
		// Before, that retry compiled on the main thread: 2-4 compiles of 5-15 s for each of the biggest object shaders on every cold start.
		Job* CloudOff = nullptr;
		Job* Parent = nullptr;	// (set on a CloudOff job: the job it belongs to)
	};

	inline std::unordered_map<std::string, Job*> CloudOffByPath;	// the CloudOff jobs by compiled path (filled before the workers start)

	// UNOFFICIAL 2026-11-16 fast-restart: a define list as one string (names and values in order), for Take's check.
	inline std::string KeyOf(const D3DXMACRO* defines) {
		std::string key;
		for (int d = 0; d < ShaderTemplate::kMaxDefines && defines && defines[d].Name; d++) { key += defines[d].Name; key += '='; key += defines[d].Definition ? defines[d].Definition : ""; key += ';'; }
		return key;
	}

	inline std::vector<Job*> Jobs;
	inline std::unordered_map<std::string, Job*> ByCompiledPath;
	inline std::atomic<size_t> NextJob{ 0 };
	inline bool Started = false;
	inline double StartTime = 0.0;
	inline std::atomic<unsigned> WorkerMaxSlots{ 4u };

	// UNOFFICIAL 2026-11-23: [Main.Main.Performance] ShaderCompileThreads (0 = auto, up to 4 on 32-bit; 1..12 explicit).
	inline unsigned ThreadCount() {
		SettingManager::Configuration::ConfigNode node;
		const int cfg = TheSettingManager->Config.FillNode(&node, "Main.Main.Performance", "ShaderCompileThreads") ? node.IntValue : 0;
		return CompileProgress::ResolveWorkerThreads(cfg);
	}

	inline unsigned long long UsedVaMB() {
		return CompileProgress::UsedVaMB();
	}

	// The paths ShaderRecord::LoadShader builds (kept the same here).
	inline void Paths(const char* Name, const char* SubPath, const char* CacheSubPath, const char* TemplateName, Job* job) {
		std::string base = ShadersPath, cache = std::string(ShadersPath) + "Cache\\";
		if (SubPath) base += SubPath;
		if (CacheSubPath) { cache += CacheSubPath; ShaderProgram::CreateFolders(cache); }
		else if (SubPath) cache += SubPath;
		job->Source = base + (TemplateName ? TemplateName : Name) + ".hlsl";
		job->Preprocessed = cache + Name + ".hlsl";
		job->Compiled = cache + Name;
	}

	// 2026-11-17: a cached binary has the outline of a compiled shader: whole 32-bit tokens, a vertex or pixel shader version
	// first and the end token last (every file D3DX writes has; D3DX and the device read the bytes without a length, so a cut
	// or made-up file was read past its end). The cache is not signed: this catches a damaged file, not a careful forgery.
	inline bool BinaryShape(const void* bytes, size_t size) {
		if (!bytes || size < 8 || (size & 3)) return false;
		DWORD first, last;
		memcpy(&first, bytes, 4);
		memcpy(&last, (const char*)bytes + size - 4, 4);
		return ((first >> 16) == 0xFFFF || (first >> 16) == 0xFFFE) && last == 0x0000FFFF;
	}

	// One shader on a worker thread: as LoadShader's preprocess, cache check and compile.
	inline void RunOne(Job* job) {
		ID3DXBuffer* errors = nullptr;
		HRESULT prepass = E_FAIL;
		{
			std::lock_guard<std::mutex> lock(CompileProgress::D3dxPreprocessMutex);
			prepass = D3DXPreprocessShaderFromFileA(job->Source.c_str(), &job->Template.Defines[0], NULL, &job->SourceBuffer, &errors);
		}
		if (prepass != D3D_OK) {
			if (errors) { job->Log += (char*)errors->GetBufferPointer(); errors->Release(); }
			return;
		}
		if (errors) { errors->Release(); errors = nullptr; }
		job->Preprocessed_ok = true;
		bool fresh = ShaderProgram::CheckPreprocessResult(job->Preprocessed.c_str(), job->SourceBuffer);
		if (!fresh) fresh = ShaderProgram::SeedFromShipped(job->Preprocessed.c_str(), job->Compiled.c_str(), ShaderProgram::PortableSource(job->SourceBuffer));	// UNOFFICIAL 2026-11-10 shipped cache
		// UNOFFICIAL 2026-11-17 fast-restart (ShaderRecord.cpp KnownCloudFailure): this exact text did not compile with the clouds' lookup at an
		// earlier start: not compiled again (LoadShader makes the build without the lookup)
		if (!fresh && ShaderProgram::KnownCloudFailure(job->Preprocessed, job->SourceBuffer)) {
			job->CompileFailed = true;
			if (job->ReleaseAfterCache && job->SourceBuffer) {
				job->SourceBuffer->Release();
				job->SourceBuffer = nullptr;
				job->BuffersReleased = true;
			}
			return;
		}
		// UNOFFICIAL 2026-11-24 fd-complete (ShaderRecord.h KnownFailure): this exact text failed to compile at an earlier start: not again
		if (!fresh) {
			const int known = ShaderProgram::KnownFailure(job->Preprocessed, job->SourceBuffer);
			if (known) {
				job->CompileFailed = true;
				job->Log += known == 2 ? "error X4505 known from an earlier start (out of temp registers): not compiled again\n" : "a compiler error known from an earlier start: not compiled again\n";
				if (job->ReleaseAfterCache && job->SourceBuffer) {
					job->SourceBuffer->Release();
					job->SourceBuffer = nullptr;
					job->BuffersReleased = true;
				}
				return;
			}
		}
		if (fresh) {
			if (job->ReleaseAfterCache) {
				// Cache on disk is already valid; release SourceBuffer immediately instead of reading binary into VA.
				FILE* f = nullptr;
				bool validBinary = false;
				if (!fopen_s(&f, job->Compiled.c_str(), "rb") && f) {
					fseek(f, 0, SEEK_END);
					const long size = ftell(f);
					if (size >= 8 && !(size & 3)) {
						DWORD first = 0, last = 0;
						fseek(f, 0, SEEK_SET);
						if (fread(&first, 4, 1, f) == 1) {
							fseek(f, size - 4, SEEK_SET);
							if (fread(&last, 4, 1, f) == 1)
								validBinary = ((first >> 16) == 0xFFFF || (first >> 16) == 0xFFFE) && last == 0x0000FFFF;
						}
					}
					fclose(f);
				}
				if (validBinary) {
					if (job->SourceBuffer) { job->SourceBuffer->Release(); job->SourceBuffer = nullptr; }
					job->BuffersReleased = true;
					return;
				}
			}
			else {
				FILE* f = nullptr;
				if (!fopen_s(&f, job->Compiled.c_str(), "rb") && f) {
					fseek(f, 0, SEEK_END);
					long size = ftell(f);
					fseek(f, 0, SEEK_SET);
					if (size > 0 && SUCCEEDED(D3DXCreateBuffer(size, &job->ShaderBuffer)))
						if (fread(job->ShaderBuffer->GetBufferPointer(), 1, size, f) != (size_t)size) { job->ShaderBuffer->Release(); job->ShaderBuffer = nullptr; }
					fclose(f);
				}
				if (job->ShaderBuffer && !BinaryShape(job->ShaderBuffer->GetBufferPointer(), job->ShaderBuffer->GetBufferSize())) {
					job->ShaderBuffer->Release(); job->ShaderBuffer = nullptr;	// (2026-11-17: compiled again below)
				}
				if (job->ShaderBuffer) return;
			}
		}
		CompileProgress::NoteCurrent(job->Name.c_str());
		const char* profile = strstr(job->Name.c_str(), ".vso") ? "vs_3_0" : "ps_3_0";
		ID3DXConstantTable* table = nullptr;
		const auto compileStart = std::chrono::steady_clock::now();	// UNOFFICIAL 2026-11-11 (LogCompileTimes)
		{
			CompileProgress::CompileGate vaGate(WorkerMaxSlots.load(std::memory_order_relaxed));
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
		}
		if (table) table->Release();
		job->CompileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
		job->CompileFailed = !job->ShaderBuffer;
		if (job->CompileFailed && errors) ShaderProgram::NoteFailure(job->Preprocessed, job->SourceBuffer, (const char*)errors->GetBufferPointer());	// (2026-11-24 fd-complete)
		if (errors) { job->Log += (char*)errors->GetBufferPointer(); errors->Release(); }
		if (job->ShaderBuffer) {
			bool wroteBin = false, wroteSrc = false;
			FILE* f = nullptr;
			if (!fopen_s(&f, job->Compiled.c_str(), "wb") && f) {
				wroteBin = fwrite(job->ShaderBuffer->GetBufferPointer(), 1, job->ShaderBuffer->GetBufferSize(), f) == job->ShaderBuffer->GetBufferSize();
				fclose(f);
			}
			const std::string portable = ShaderProgram::PortableSource(job->SourceBuffer);
			if (!fopen_s(&f, job->Preprocessed.c_str(), "wb") && f) {
				wroteSrc = fwrite(portable.data(), 1, portable.size(), f) == portable.size();
				fclose(f);
			}
			job->Log += "Shader compiled (parallel): " + job->Compiled;
			CompileProgress::ShadersCompiled.fetch_add(1);
			if (job->ReleaseAfterCache && wroteBin && wroteSrc) {
				job->ShaderBuffer->Release(); job->ShaderBuffer = nullptr;
				if (job->SourceBuffer) { job->SourceBuffer->Release(); job->SourceBuffer = nullptr; }
				job->BuffersReleased = true;
			}
		}
	}

	// UNOFFICIAL 2026-11-26 faster-shader-compile: the job, then (when it failed with the clouds' lookup in its source, as LoadShader's
	// ForwardCloudShadows retry tests) its build without the lookup. Done is set on the CloudOff job before the worker sets it on this one.
	inline void Run(Job* job) {
		RunOne(job);
		Job* off = job->CloudOff;
		if (!off) return;
		// (no X4500 test: LoadShader retries a worker's failed record without the lookup whatever the error)
		if (!job->ShaderBuffer && job->Preprocessed_ok && CloudCodeIn(job->SourceBuffer)) {
			ShaderProgram::CreateFolders(off->Compiled.substr(0, off->Compiled.find_last_of('\\')));	// (before: the shipped-cache seed copies into it)
			RunOne(off);
			off->Done = true;
		}
	}

	// UNOFFICIAL 2026-11-16 fast-restart (LateInteriors): jobs not finished yet.
	inline unsigned Running() {
		unsigned n = 0;
		for (Job* job : Jobs) if (!job->Done) n++;
		return n;
	}
	inline size_t TotalJobs() { return Jobs.size(); }

	inline void Worker() {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		for (;;) {
			const size_t i = NextJob.fetch_add(1);
			if (i >= Jobs.size()) return;
			Run(Jobs[i]);
			Jobs[i]->Done = true;
			CompileProgress::ShadersDone.fetch_add(1);
		}
	}

	inline Job* Add(const char* Name, const char* SubPath, const char* CacheSubPath, ShaderTemplate Template, const D3DXMACRO* Overrides) {
		Job* job = new Job();
		job->Name = Name;
		Paths(Name, SubPath, CacheSubPath, Template.Name, job);
		if (!ShaderProgram::FileExists(job->Source.c_str()) || ByCompiledPath.count(job->Compiled)) { delete job; return nullptr; }
		job->Template = Template;
		ShaderRecord::BuildDefines(job->Template, Overrides);
		int defCount = 0;
		for (int d = 0; d < ShaderTemplate::kMaxDefines && job->Template.Defines[d].Name; d++)
			if (job->Template.Defines[d].Definition) defCount++;
		job->OwnedDefineValues.reserve(defCount);
		for (int d = 0; d < ShaderTemplate::kMaxDefines && job->Template.Defines[d].Name; d++) {
			if (job->Template.Defines[d].Definition) {
				job->OwnedDefineValues.emplace_back(job->Template.Defines[d].Definition);
				job->Template.Defines[d].Definition = job->OwnedDefineValues.back().c_str();
			}
		}
		job->DefineKey = KeyOf(&job->Template.Defines[0]);
		Jobs.push_back(job);
		ByCompiledPath[job->Compiled] = job;
		// (2026-11-26) its CloudOff job: the folder, template and overrides LoadShader's ForwardCloudShadows retry passes (overrides + FORWARD_CLOUD_SHADOWS 0)
		const size_t length = strlen(Name);
		if (length > 4 && !strcmp(Name + length - 4, ".pso") && !OverridesName(Overrides, "FORWARD_CLOUD_SHADOWS")) {
			Job* off = new Job();
			off->Name = Name;
			off->Parent = job;
			const std::string sub = std::string("NoCloud\\") + (CacheSubPath ? CacheSubPath : SubPath ? SubPath : "");
			std::string base = ShadersPath, cache = std::string(ShadersPath) + "Cache\\" + sub;	// (as Paths, without making the folder now)
			if (SubPath) base += SubPath;
			off->Source = base + (Template.Name ? Template.Name : Name) + ".hlsl";
			off->Preprocessed = cache + Name + ".hlsl";
			off->Compiled = cache + Name;
			if (CloudOffByPath.count(off->Compiled)) { delete off; return job; }
			std::vector<D3DXMACRO> cloudOff;
			for (const D3DXMACRO* o = Overrides; o && o->Name; o++) cloudOff.push_back(*o);
			cloudOff.push_back({ "FORWARD_CLOUD_SHADOWS", "0" });
			cloudOff.push_back({ NULL, NULL });
			off->Template = Template;
			ShaderRecord::BuildDefines(off->Template, cloudOff.data());	// (copies the names and values, which are static strings, not the list)
			// (2026-11-27 upstream 1.1.5 merge) the same own copy of the define strings as above, so a worker never reads BuildDefines' static char arrays
			off->OwnedDefineValues.reserve(ShaderTemplate::kMaxDefines);	// (reserved first: a reallocation would move short strings and their c_str pointers)
			for (int d = 0; d < ShaderTemplate::kMaxDefines && off->Template.Defines[d].Name; d++) {
				if (off->Template.Defines[d].Definition) {
					off->OwnedDefineValues.emplace_back(off->Template.Defines[d].Definition);
					off->Template.Defines[d].Definition = off->OwnedDefineValues.back().c_str();
				}
			}
			off->DefineKey = KeyOf(&off->Template.Defines[0]);
			job->CloudOff = off;
			CloudOffByPath[off->Compiled] = off;
		}
		return job;
	}

	// From the first ShaderRecord::LoadShader (main thread): the job list, then the workers.
	inline void StartOnce() {
		if (Started) return;
		if (!TheShaderManager || TheShaderManager->ShaderNames.empty()) return;	// not set up yet: try again on the next shader
		Started = true;
		if (!TheSettingManager->GetSettingI("Main.Main.Performance", "ParallelShaderCompile")) return;
		CompileProgress::DrainEffectWorkers();	// UNOFFICIAL 2026-11-23: never run effect & game-shader worker pools simultaneously in 32-bit VA
		StartTime = (double)GetTickCount64();
		static D3DXMACRO interior[] = { { "INTERIOR_SHADOWS", "1" }, { "FORWARD_SHADOWS", "0" }, { "SUN_CROSSFADE", "0" },
			{ "CONTACT_HARDENING", "0" }, { NULL, NULL }, { NULL, NULL } };
		// UNOFFICIAL 2026-10-21 LampFalloff: as ShaderManager::LoadShader's interior records (PBRShaders::LampFalloffInteriorDefine).
		if (const char* lampFalloff = PBRShaders::LampFalloffInteriorDefine()) interior[4] = { "LAMP_FALLOFF_ON", lampFalloff };
		// (2026-11-28 1.1.6 merge) the key was retired with the forward modes (RtLampShadows locks it on; LoadShader always makes these records):
		// read here it was missing, so the queue skipped the interior and lamp records and they compiled on the main thread instead.
		const bool interiorShadows = true;
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
			static const char* const folders[3] = { "ExteriorLamps\\", "ExteriorLamps\\Fit1\\", "ExteriorLamps\\Fit2\\" };	// (2026-11-11 BUG FIX: "\F" is no escape, so these were "ExteriorLampsFit1\\": the records went into folders nothing reads, and the two levels compiled again on the main thread, 5-10 s each on a cold start. ShaderManager.cpp reads "ExteriorLamps\Fit1\\".)
			const D3DXMACRO* const sets[3] = { exteriorLamps, exteriorFit1, exteriorFit2 };
			for (int level = 0; level < (skin ? 3 : 1); level++) {
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
		// UNOFFICIAL 2026-11-16 fast-restart (core/LateInteriors.h): the lamp-shadow records' jobs (Interior, RT ExteriorLamps) go after every job
		// the start waits for: those records are made after the start (LateInteriors::Tick), so the workers do the start's own shaders first.
		const bool late = interiorShadows && LateInteriors::Wanted();
		std::vector<std::pair<std::string, ShaderTemplate>> lateJobs;
		// UNOFFICIAL 2026-11-16 fast-restart: the forward+ glass records DeferredTransparents::Load makes with each Interior record (the outdoor one from
		// the default overrides, the indoor one from the Interior record's), [Main.Main.Quality] DeferredTransparents (on by default since migration
		// 88): 57 records that compiled one after another on the main thread (80 s of a cold start on the bench, 11-16)
		const bool glass = interiorShadows && DeferredTransparents::WantedAtStartup();
		static D3DXMACRO glassOut[2] = { { "LAMPS_DEFERRED", "4" }, { NULL, NULL } };
		static D3DXMACRO glassIn[8] = {};
		{ int n = 0; for (const D3DXMACRO* o = interior; o->Name && n < 6; o++) glassIn[n++] = *o; glassIn[n++] = { "LAMPS_DEFERRED", "4" }; glassIn[n] = { NULL, NULL }; }
		auto addLampRecords = [&](const std::string& name, const ShaderTemplate& tmpl) {	// (the order ShaderManager.cpp LoadLampShadowRecords takes them in)
			Add(name.c_str(), NULL, "Interiors\\", tmpl, interior);
			if (glass && DeferredTransparents::Eligible(name.c_str(), tmpl)) {
				Add(name.c_str(), NULL, "Deferred\\GlassExterior\\", tmpl, glassOut);
				Add(name.c_str(), NULL, DeferredTransparents::kCache, tmpl, glassIn);
			}
			if (exteriorLampRecords) addExteriorLamps(name.c_str(), tmpl);
		};
		auto addInterior = [&](const std::string& name, const ShaderTemplate& tmpl) {
			if (late) { lateJobs.emplace_back(name, tmpl); return; }
			addLampRecords(name, tmpl);
		};
		// (2026-11-16 fast-restart) the other deferred records LoadShader made one after another on the main thread at load: the skin's
		// (DeferredSkin::Load), the grass's and leaves' (DeferredFoliage::Load), the hair's forward+ (DeferredTransparents::Hair::Load)
		static D3DXMACRO skinDeferred[2] = { { "LAMPS_DEFERRED", "2" }, { NULL, NULL } };
		static D3DXMACRO foliageDeferred[2] = { { "FOLIAGE_DEFERRED", "1" }, { NULL, NULL } };
		static D3DXMACRO hairForwardPlus[2] = { { "HAIR_FORWARD_PLUS", "1" }, { NULL, NULL } };
		// UNOFFICIAL 2026-11-16 fast-restart: the lighting-free records (LightFreeRecords::Load, [Shaders.ShadowsInteriors.Deferred] LightFreeRecords:
		// the deferred renderer's default) compiled one after another on the main thread at load: 96 of them, 50 s of the owner's 11-16 cold
		// start. Queued here with the same folder, cache folder and defines as that load, so LoadShader takes the workers' results.
		const bool lightFree = LightFreeRecords::WantedAtStartup();
		// UNOFFICIAL 2026-11-26 faster-shader-compile: with DeferredWetness (on by default) the near land's, objects' and parallax's records load
		// from LightFreeWet\ with DEFERRED_WETNESS (LightFreeRecords::Load). The queue still made the dry LightFree\ ones nobody loads, and the 94
		// wet ones compiled one after another on the main thread: about 250 s of the 540 s cold start on the bench (11-26, slot A). The workers
		// start before DeferredWetness::LoadShaders (DeferredDepth's pass is the first load), so its switch is read here, not WetCompiled; if its
		// pass then fails to load, the dry records compile at their load as before.
		const bool lightFreeWet = lightFree && DeferredWetness::WantedAtStartup();
		auto lightFreeWetTemplate = [](const ShaderTemplate& tmpl) {
			return tmpl.Name && (!strcmp(tmpl.Name, "TerrainTemplate") || !strcmp(tmpl.Name, "ObjectTemplate") || !strcmp(tmpl.Name, "ParallaxTemplate"));
		};
		std::unordered_set<std::string> templated;
		for (const auto& [CollectionName, Pointer] : TheShaderManager->ShaderNames) {
			ShaderCollection* Collection = Pointer ? *Pointer : nullptr;
			if (!Collection) continue;
			for (const auto& [Name, Template] : Collection->Templates()) {
				const std::string name(Name);
				templated.insert(name);
				if (DeferredSkin::IsOpaqueDeferredManaged(name.c_str(), Template)) {
					Add(name.c_str(), NULL, "Deferred\\Fallback\\", Template, DeferredSkin::LeanFallbackOverrides());
					if (glass && PointShadowForward::Eligible(name.c_str(), Template) && DeferredTransparents::Eligible(name.c_str(), Template)) {
						Add(name.c_str(), NULL, "Deferred\\GlassExterior\\", Template, glassOut);
						Add(name.c_str(), NULL, DeferredTransparents::kCache, Template, glassIn);
					}
				}
				else {
					Add(name.c_str(), NULL, NULL, Template, NULL);
					if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pso") == 0) addTerrainLamps(name.c_str(), Template);
					if (interiorShadows && name.size() > 4 && name.compare(name.size() - 4, 4, ".pso") == 0 && PointShadowForward::Eligible(name.c_str(), Template))
						addInterior(name, Template);
				}
				if (lightFree && LightFreeRecords::Eligible(name.c_str(), Template)) {
					const bool wet = lightFreeWet && lightFreeWetTemplate(Template);
					Add(name.c_str(), NULL, wet ? LightFreeRecords::kWetCache : LightFreeRecords::kCache, Template, wet ? LightFreeRecords::WetOverrides() : LightFreeRecords::Overrides());
				}
				if (DeferredSkin::WantedAtStartup() && DeferredSkin::Eligible(name.c_str(), Template))
					Add(name.c_str(), NULL, DeferredSkin::kCache, Template, skinDeferred);
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
					addInterior(name, ShaderTemplate{});
				if (DeferredFoliage::WantedAtStartup() && (DeferredFoliage::IsLeaf(name.c_str()) || DeferredFoliage::IsGrass(name.c_str())))
					Add(name.c_str(), NULL, DeferredFoliage::kCache, ShaderTemplate{}, foliageDeferred);
				if (DeferredTransparents::Hair::WantedAtStartup() && DeferredTransparents::Hair::IsHair(name.c_str()))
					Add(name.c_str(), NULL, "Deferred\\Hair\\", ShaderTemplate{}, hairForwardPlus);
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
		// (2026-11-16 fast-restart) the lamp-shadow records' jobs last, the biggest first (the six-lamp ones are 10-12 s each: started first, they
		// end with the rest)
		const size_t lateFrom = Jobs.size();
		// UNOFFICIAL 2026-11-26 faster-shader-compile: the shadow shaders and the vertex shaders first. They are small and are what the main thread
		// asks for first (ShadowManager, then each collection's vertex shaders); at the back of the queue the main thread waited 106 s for
		// ShadowMap.vso (bench 11-26) instead of getting on with its own compiles (the deferred passes) beside the workers.
		std::stable_partition(Jobs.begin(), Jobs.end(), [](const Job* j) {
			return j->Source.find("\\Shadows\\") != std::string::npos || (j->Name.size() > 4 && !j->Name.compare(j->Name.size() - 4, 4, ".vso"));
		});
		for (const auto& [name, tmpl] : lateJobs) addLampRecords(name, tmpl);
		for (size_t j = lateFrom; j < Jobs.size(); j++) Jobs[j]->ReleaseAfterCache = true;
		std::stable_sort(Jobs.begin() + lateFrom, Jobs.end(), [](const Job* a, const Job* b) {
			auto weight = [](const Job* j) { const char* n = j->Name.c_str(); return !strncmp(n, "SLS203", 6) && n[6] >= '4' && n[6] <= '6' ? 2 : !strncmp(n, "SLS20", 5) || !strncmp(n, "SKIN", 4) ? 1 : 0; };
			return weight(a) > weight(b);
		});
		const unsigned threads = ThreadCount();
		WorkerMaxSlots.store(threads, std::memory_order_relaxed);
		CompileProgress::ShadersTotal.store((int)(late ? lateFrom : Jobs.size()));
		CompileProgress::Begin();
		for (unsigned i = 0; i < threads; i++) CompileProgress::SpawnWorker(Worker);	// (detached 4 MB stack: shaders the game never asks for are compiled and left)
		Logger::Log("UNOFFICIAL ParallelShaderCompile: %u shaders on %u threads%s.", (unsigned)Jobs.size(), threads,
			late ? " (the lamp-shadow records' last: made after the start, core/LateInteriors.h)" : "");
	}

	// From ShaderRecord::LoadShader: this shader's finished result, if a worker made it (waits for it if still running).
	// The caller owns the buffers then. False: not one of ours, do it as before.
	// (2026-11-16 fast-restart) Defines: the load's own list. A job built with another one (a ShaderFallback drop since the start: the
	// lighting-free and lamp-shadow records are taken later than the default ones) is not used: the load compiles its own, as before.
	inline bool TakenOutOfRegisters = false;	// (2026-11-18) the last Take's job did not compile for want of temp registers (X4505)
	inline bool Take(const char* CompiledPath, ID3DXBuffer** Source, ID3DXBuffer** Shader, const D3DXMACRO* Defines = nullptr) {
		TakenOutOfRegisters = false;
		if (!Started || Jobs.empty()) return false;
		auto it = ByCompiledPath.find(CompiledPath);
		Job* job = it == ByCompiledPath.end() ? nullptr : it->second;
		if (!job) {	// (2026-11-26) a CloudOff build: made by its job's worker when that job failed with the clouds' lookup in its source
			auto off = CloudOffByPath.find(CompiledPath);
			if (off == CloudOffByPath.end()) return false;
			job = off->second;
			CompileProgress::WaitDone(job->Parent->Done);
			if (!job->Done) return false;	// (not made: the record compiled with the lookup, and the device refused it)
		}
		if (job->Taken) return false;
		CompileProgress::WaitDone(job->Done);
		job->Taken = true;
		if (Defines && KeyOf(Defines) != job->DefineKey) {
			Logger::Log("UNOFFICIAL ParallelShaderCompile: %s was queued with other defines than its load has now (a feature dropped since the start): made at its load instead", CompiledPath);
			if (job->SourceBuffer) job->SourceBuffer->Release();
			if (job->ShaderBuffer) job->ShaderBuffer->Release();
			job->SourceBuffer = job->ShaderBuffer = nullptr;
			return false;
		}
		if (!job->Log.empty()) { Logger::Log("%s", job->Log.c_str()); RegisterClash::Check(job->Name.c_str(), job->Log.c_str()); }	// UNOFFICIAL 2026-10-31 (X4500)
		if (job->BuffersReleased) {
			TakenOutOfRegisters = job->CompileFailed && job->Log.find("X4505") != std::string::npos;
			return false;	// LoadShader takes the cache hit cleanly (or handles cloud failure) without keeping VA tied up in worker buffers
		}
		if (!job->Preprocessed_ok) { if (job->SourceBuffer) job->SourceBuffer->Release(); job->SourceBuffer = nullptr; return false; }
		TakenOutOfRegisters = job->CompileFailed && job->Log.find("X4505") != std::string::npos;
		*Source = job->SourceBuffer;
		*Shader = job->ShaderBuffer;
		job->SourceBuffer = job->ShaderBuffer = nullptr;
		static size_t taken = 0;
		if (!job->Parent && ++taken == Jobs.size()) {
			Logger::Log("UNOFFICIAL ParallelShaderCompile: all %u taken, %.1f s after the start.", (unsigned)Jobs.size(), ((double)GetTickCount64() - StartTime) / 1000.0);
		}
		return true;
	}

	inline void ReleaseUntakenStartupBuffers() {
		unsigned released = 0;
		auto release = [&](Job* job) {
			if (!job->ReleaseAfterCache && !job->Taken && !job->BuffersReleased && job->Done.load(std::memory_order_acquire)) {
				if (job->SourceBuffer || job->ShaderBuffer) released++;
				if (job->SourceBuffer) { job->SourceBuffer->Release(); job->SourceBuffer = nullptr; }
				if (job->ShaderBuffer) { job->ShaderBuffer->Release(); job->ShaderBuffer = nullptr; }
				job->BuffersReleased = true;
			}
		};
		for (Job* job : Jobs) {
			release(job);
			if (job->CloudOff) release(job->CloudOff);	// (2026-11-27 upstream 1.1.5 merge: an untaken CloudOff build keeps its VA otherwise)
		}
		if (released) Logger::Log("UNOFFICIAL ParallelShaderCompile: released buffers of %u untaken startup shaders.", released);
	}

	// UNOFFICIAL 2026-11-11 (docs/next/SHADER-COMPILE-TIMES-2026-11-11.md) log only: the start's game-shader compiles, slowest first. MainCompiles:
	// ShaderRecord::LoadShader's own compiles (main thread only). LogCompileTimes: once, from ImGuiManager::Initialize (main thread); a job
	// is read only once Done, as Take does.
	inline std::vector<std::pair<double, std::string>> MainCompiles;
	inline void LogCompileTimes() {
		static bool logged = false;
		if (logged) return;
		logged = true;
		ReleaseUntakenStartupBuffers();
		std::vector<std::pair<double, std::string>> all;
		double workerMs = 0.0, mainMs = 0.0;
		unsigned workerCount = 0, running = 0;
		for (Job* listed : Jobs) {
			if (!listed->Done) { running++; continue; }
			for (Job* job : { listed, listed->CloudOff }) {	// (2026-11-26 + its CloudOff build, when made)
				if (!job || !job->Done || job->CompileMs < 0.0) continue;
				workerMs += job->CompileMs; workerCount++;
				all.emplace_back(job->CompileMs, job->Compiled + (job->CompileFailed ? " (worker, FAILED)" : " (worker)"));
			}
		}
		for (const auto& c : MainCompiles) { mainMs += c.first; all.emplace_back(c.first, c.second + " (main thread)"); }
		std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::string top;
		for (size_t i = 0; i < all.size() && i < 15; i++) {
			const size_t at = all[i].second.rfind("Cache\\");
			char ms[32]; sprintf_s(ms, " %.0f ms", all[i].first);
			top += (i ? ", " : "") + (at == std::string::npos ? all[i].second : all[i].second.substr(at + 6)) + ms;
		}
		Logger::Log("UNOFFICIAL compile times: %u compiled on the worker threads (%.1f s of thread time), %u on the main thread (%.1f s, the start "
			"waited for all of it), %u worker jobs still running; slowest: %s", workerCount, workerMs / 1000.0, (unsigned)MainCompiles.size(),
			mainMs / 1000.0, running, top.empty() ? "(nothing compiled)" : top.c_str());
	}
}
