#include <regex>
#include "DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include "SessionLog.h"	// 2026-11-04: the effect's name as the CRASH lines' stage
#include "FrameWorker.h"	// UNOFFICIAL 2026-11-03 SkyOnWorker
#include "Sm6Pass.h"	// UNOFFICIAL 2026-10-26 (deep/fork-sm6)
#include "RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2
#include "GpuDebug.h"	// UNOFFICIAL 2026-10-26: GpuDebugLabels (each effect is a named group in a capture)
#include "PixelInspector.h"	// UNOFFICIAL 2026-10-26 (debugging tool)
#include "Diagnostics.h"	// UNOFFICIAL 2026-11-01 the flash detector's self-test (SelfTestSkip)
#include "ReflMaterial.h"	// UNOFFICIAL 2026-10-20 MaterialAware reflections (REFL_MATERIAL_ON)
#include "RegisterClash.h"	// UNOFFICIAL 2026-10-31 register-clash guard (X4500)
#include "EffectPrecompile.h"	// UNOFFICIAL 2026-10-28 the effects compiled in parallel at startup
#include "EffectCompiler47.h"	// UNOFFICIAL 2026-10-29 NewEffectCompiler (d3dcompiler_47 for the effects)
#include "MixCheck.h"	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
// Helpers for the effect loader: a held interface is released and cleared in one step, and the
// D3DX message buffer of a step is written to the log (and to RegisterClash) before it is dropped.
namespace {
template <typename Iface>
void dropHeld(Iface*& held) {
	if (held) held->Release();
	held = nullptr;
}

void logMessages(ID3DXBuffer* messages, const char* sourcePath) {
	if (!messages) return;
	Logger::Log((char*)messages->GetBufferPointer());
	RegisterClash::Check(sourcePath, (const char*)messages->GetBufferPointer());
}
}

/*
* One effect: its name is the file name (without .fx.hlsl) in the effects folder.
*/
EffectRecord::EffectRecord(const char* EffectFileName)
	: Enabled(false), renderTime(0), constantUpdateTime(0), Effect(nullptr), Name(EffectFileName) {}

/*Shader Values arrays are freed in the superclass Destructor*/
EffectRecord::~EffectRecord() { dropHeld(Effect); }

/*
 * Unload the effect so that it can be loaded again from a clean state.
 */
void EffectRecord::DisposeEffect() {
	dropHeld(Effect);

	auto freeValues = [](auto*& values, UInt32& count) {
		delete[] values;
		values = nullptr;
		count = 0;
	};
	freeValues(FloatShaderValues, FloatShaderValuesCount);
	freeValues(TextureShaderValues, TextureShaderValuesCount);

	LoadGeneration++;	// cached technique/parameter handles are stale now
	Enabled = false;	// (a disposed effect is always off)
}

/*
 * Compile (or load from the cache) and set up the effect's shader.
 * Returns false when the source is missing; true when the cache or a compile produced a usable
 * result (even if the binary then failed to load -- the caller checks IsLoaded).
 */
bool EffectRecord::LoadEffect() {
	TimeLogger loadClock;
	bool usable = false;	// what the call returns: a cache or compile result was there

	ID3DXEffect* built = nullptr;
	ID3DXEffectCompiler* compiler = nullptr;
	ID3DXBuffer* source = nullptr;
	ID3DXBuffer* messages = nullptr;
	ID3DXBuffer* binary = nullptr;

	const std::string sourcePath = std::string(EffectsPath) + Name + ".fx.hlsl";
	const std::string cacheDir = std::string(EffectsPath) + "Cache\\";
	const std::string preprocessedPath = cacheDir + Name + ".fx.hlsl";
	const std::string compiledPath = cacheDir + Name + ".fx";

	if (!FileExists(sourcePath.c_str())) return false;

	// Effects get the same compile-time defines the game shaders do. Only the PREPROCESS call
	// needs them -- D3DXCreateEffectCompiler below is handed the already-preprocessed source.
	// Toggling any of these changes that source, so CheckPreprocessResult forces a recompile.
	D3DXMACRO macros[8];	// UNOFFICIAL 2026-10-23: 8 (CHARACTER_SHADOW_MAP)
	BuildMacros(macros);
	EffectPrecompile::Wait(compiledPath.c_str());	// UNOFFICIAL 2026-10-28 ParallelShaderCompile: a worker may be making this cache now
	EffectCompiler47::ReadSetting();	// UNOFFICIAL 2026-10-29 NewEffectCompiler: also part of the cache check (CacheText)

	// Leaves the effect off: frees what this call made and returns the result so far.
	auto refuse = [&]() {
		dropHeld(compiler);
		dropHeld(source);
		dropHeld(built);
		dropHeld(messages);
		dropHeld(binary);
		Enabled = false;	// stays off after a failed step
		return usable;
	};

	const HRESULT ppHr = D3DXPreprocessShaderFromFileA(sourcePath.c_str(), macros, NULL, &source, &messages);
	if (ppHr != D3D_OK) {
		ReportError(ppHr);
		logMessages(messages, sourcePath.c_str());
	}
	else {
		bool stale = !EffectCompiler47::CacheFresh(preprocessedPath.c_str(), source);	// UNOFFICIAL 2026-10-29 (was CheckPreprocessResult)
		if ((stale || !FileExists(compiledPath.c_str())) && SeedFromShipped(preprocessedPath.c_str(), compiledPath.c_str(), EffectCompiler47::CacheText(source))) stale = false;	// UNOFFICIAL 2026-11-10 shipped cache
		const bool hasBinary = FileExists(compiledPath.c_str());

		// Cached binary: load it as it is.
		if (!stale && hasBinary) {
			const HRESULT cacheHr = D3DXCreateEffectFromFileA(TheRenderManager->device, compiledPath.c_str(), NULL, NULL, D3DXFX_LARGEADDRESSAWARE, NULL, &built, &messages);
			const bool cacheOk = SUCCEEDED(cacheHr);
			if (cacheOk) EffectStatus::ShaderLoaded(EffectStatus::CacheHit);	// UNOFFICIAL 2026-11-01
			else ReportError(cacheHr);
			logMessages(messages, sourcePath.c_str());
			if (!cacheOk) {	// fall through to a fresh compile below
				dropHeld(built);
				dropHeld(messages);
			}
			usable = true;
		}

		// No usable binary: compile the source. d3dcompiler_47 first (UNOFFICIAL 2026-10-29, core/EffectCompiler47.h);
		// D3DX below when it is off, missing or refuses this effect.
		if (stale || !hasBinary || !built) {
			bool viaNew = false;
			{
				ID3DXBuffer* newMessages = nullptr;
				viaNew = EffectCompiler47::Compile(source, Name, &binary, &newMessages);
				if (!viaNew && EffectCompiler47::Use)
					Logger::Log("UNOFFICIAL NewEffectCompiler: %s refused by d3dcompiler_47, compiled with D3DX as before: %s", Name,
						newMessages ? (const char*)newMessages->GetBufferPointer() : "(no message)");
				dropHeld(newMessages);
			}

			HRESULT stepHr = viaNew ? D3D_OK
				: D3DXCreateEffectCompiler((const char*)source->GetBufferPointer(), source->GetBufferSize(), NULL, NULL, NULL, &compiler, &messages);
			if (FAILED(stepHr)) {
				ReportError(stepHr);
				EffectStatus::ShaderLoaded(EffectStatus::CompileFailed);	// UNOFFICIAL 2026-11-01
				EffectStatus::Level(Name, "effect FAILED to compile (off)", false);
				logMessages(messages, sourcePath.c_str());
				return refuse();
			}
			// The compiler's creation messages are logged here and dropped before the next call fills the buffer again.
			logMessages(messages, sourcePath.c_str());
			dropHeld(messages);

			if (!viaNew) stepHr = compiler->CompileEffect(NULL, &binary, &messages);
			if (FAILED(stepHr)) {
				ReportError(stepHr);
				EffectStatus::ShaderLoaded(EffectStatus::CompileFailed);	// UNOFFICIAL 2026-11-01
				EffectStatus::Level(Name, "effect FAILED to compile (off)", false);
				logMessages(messages, sourcePath.c_str());
				return refuse();
			}
			logMessages(messages, sourcePath.c_str());
			dropHeld(messages);

			if (binary) {
				std::ofstream binaryFile(compiledPath, std::ios::out | std::ios::binary);
				binaryFile.write((const char*)binary->GetBufferPointer(), binary->GetBufferSize());
				binaryFile.flush();
				binaryFile.close();

				std::ofstream preprocessFile(preprocessedPath, std::ios::out | std::ios::binary);
				const std::string portable = EffectCompiler47::CacheText(source);	// UNOFFICIAL: see PortableSource (2026-10-29: + the compiler)
				preprocessFile.write(portable.data(), portable.size());
				preprocessFile.flush();
				preprocessFile.close();

				Logger::Log("Effect compiled: %s%s", preprocessedPath.c_str(), viaNew ? " (d3dcompiler_47)" : "");
				EffectStatus::ShaderLoaded(EffectStatus::Compiled);	// UNOFFICIAL 2026-11-01
			}

			const HRESULT freshHr = D3DXCreateEffectFromFileA(TheRenderManager->device, compiledPath.c_str(), NULL, NULL, D3DXFX_LARGEADDRESSAWARE, NULL, &built, &messages);
			if (FAILED(freshHr)) ReportError(freshHr);
			logMessages(messages, sourcePath.c_str());
			if (FAILED(freshHr)) return refuse();
			usable = true;
		}
	}

	if (built) {
		Effect = built;
		LoadGeneration++;
		CreateCT(source, NULL);	// the TESR_ constant/sampler tables of the new effect
		Logger::Log("Effect loaded: %s", compiledPath.c_str());
	}

	// On only when an effect is in place and its menu switch says so.
	Enabled = IsLoaded() && TheSettingManager->GetMenuShaderEnabled(Name);

	dropHeld(compiler);
	dropHeld(messages);
	dropHeld(binary);
	dropHeld(source);

	loadClock.LogTime("EffectRecord::LoadEffect");
	return usable;
}

/*
* UNOFFICIAL 2026-10-26 (deep/fork-sm6): the compile-time defines every effect is preprocessed with, terminated by { NULL, NULL };
* EffectMacros needs room for 8. Shared by LoadEffect and core\Sm6Pass (which picks the SM6 build made with the same defines; keep
* tools\nvr-sm6\build.py MACROS in step with this list). Returns the number of defines.
*/
int EffectRecord::BuildMacros(D3DXMACRO* EffectMacros) {
	int count = 0;
	auto define = [&](const char* name, const char* value) { EffectMacros[count++] = { name, value }; };

	define("FORWARD_SHADOWS", TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.Main", "ForwardShadows") ? "1" : "0");
	// UNOFFICIAL physical sky (VolumetricFog.fx.hlsl): compiled in only when on at startup.
	define("PHYSICAL_SKY_ON", TheSettingManager->GetSettingI("Shaders.Sky.Physical", "Enabled") ? "1" : "0");
	if (TheRenderManager->IsReversedDepth()) define("REVERSED_DEPTH", "");
	// UNOFFICIAL 2026-10-20 MaterialAware reflections (Reflections.fx.hlsl, core/ReflMaterial.h): only when on at startup, so that otherwise
	// the effects' preprocessed sources are exactly what they were.
	if (ReflMaterial::CompiledIn()) define("REFL_MATERIAL_ON", "1");
	if (ReflMaterial::AddCompiledIn()) define("REFL_ADD_ON", "1");	// UNOFFICIAL 2026-10-21 ReflectAdd (Reflections.fx.hlsl), likewise
	if (ReflMaterial::OccCompiledIn()) define("REFL_SPECOCC_ON", "1");	// UNOFFICIAL 2026-10-21 SpecularOcclusion (Reflections.fx.hlsl), likewise
	// UNOFFICIAL 2026-10-23 CharacterShadowMap (SunShadows.fx.hlsl's deferred lookup): only when on at startup, as the game shaders
	// (ShaderRecord::BuildDefines reads the same setting), so that off the effect's preprocessed source is exactly what it was.
	if (TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMap")) define("CHARACTER_SHADOW_MAP", "1");

	EffectMacros[count] = { nullptr, nullptr };	// terminator, not counted
	return count;
}

/*
* True once the effect's compiled shader is in place.
*/
bool EffectRecord::IsLoaded() {
	return static_cast<bool>(Effect);
}

/**
Creates the constant table for the effect: its TESR_ floats and samplers, and whether its passes read the destination.
*/
void EffectRecord::CreateCT(ID3DXBuffer* ShaderSource, ID3DXConstantTable* ConstantTable) {
	TimeLogger tableClock;

	D3DXEFFECT_DESC desc;
	UInt32 floatSlot = 0;
	UInt32 texSlot = 0;

	Effect->GetDesc(&desc);
	usesSourceBuffer = false;

	// Scan the preprocessed source (includes expanded) for anything that makes a pass leave
	// pixels unwritten or read the destination. Unknown source stays conservative.
	needsPrefill = true;
	bool partialStates = true;
	if (ShaderSource && ShaderSource->GetBufferPointer()) {
		static const std::regex partialWrite(
			R"(\bclip\s*\(|\bdiscard\b|AlphaBlendEnable\s*=\s*(true|1)|StencilEnable\s*=\s*(true|1)|)"
			R"(ColorWriteEnable|SeparateAlphaBlendEnable\s*=\s*(true|1)|AlphaTestEnable\s*=\s*(true|1))",
			std::regex::icase | std::regex::optimize);
		static const std::regex partialState(
			R"(AlphaBlendEnable\s*=\s*(true|1)|StencilEnable\s*=\s*(true|1)|)"
			R"(ColorWriteEnable|SeparateAlphaBlendEnable\s*=\s*(true|1)|AlphaTestEnable\s*=\s*(true|1))",
			std::regex::icase | std::regex::optimize);
		std::string source((const char*)ShaderSource->GetBufferPointer(), ShaderSource->GetBufferSize());
		needsPrefill = std::regex_search(source, partialWrite);
		partialStates = needsPrefill && std::regex_search(source, partialState);
	}
	// Only clip/discard matched: the source match can be in code no pass uses (Includes/Blur.hlsl's
	// helpers made WetWorld copy the whole frame four times a frame for nothing), so decide per pass
	// from the compiled pixel shader, which keeps a kill instruction only where it can skip pixels.
	// A pass whose shader cannot be read stays pre-filled.
	passPrefill.clear();
	std::string passMap;
	if (needsPrefill && !partialStates) {
		for (UINT t = 0; t < desc.Techniques; t++) {
			std::vector<bool> passes;
			D3DXHANDLE technique = Effect->GetTechnique(t);
			D3DXTECHNIQUE_DESC techniqueDesc;
			if (technique && SUCCEEDED(Effect->GetTechniqueDesc(technique, &techniqueDesc))) {
				passMap += (t ? " " : "") + std::to_string(t) + ":";
				for (UINT p = 0; p < techniqueDesc.Passes; p++) {
					bool kills = true;
					D3DXPASS_DESC passDesc;
					ID3DXBuffer* disassembly = nullptr;
					if (SUCCEEDED(Effect->GetPassDesc(Effect->GetPass(technique, p), &passDesc)) && passDesc.pPixelShaderFunction &&
						SUCCEEDED(D3DXDisassembleShader(passDesc.pPixelShaderFunction, FALSE, NULL, &disassembly)) && disassembly) {
						std::string text((const char*)disassembly->GetBufferPointer(), disassembly->GetBufferSize());
						kills = text.find("texkill") != std::string::npos;
					}
					if (disassembly) disassembly->Release();
					passes.push_back(kills);
					passMap += kills ? "K" : "-";
				}
			}
			passPrefill.push_back(passes);
		}
	}
	if (!passPrefill.empty())
		Logger::Log("UNOFFICIAL %s: frame chain pre-fills only passes that can skip pixels (K), per technique: %s", Name, passMap.c_str());
	else if (needsPrefill) Logger::Log("%s: partial-write passes, frame chain pre-fills its destination.", Name);
	// Visits the TESR_ parameters (the effect's own constants and samplers) in declaration order.
	auto forEachTesr = [&](const std::function<void(D3DXHANDLE, const D3DXPARAMETER_DESC&)>& visit) {
		for (UINT c = 0; c < desc.Parameters; c++) {
			D3DXHANDLE handle = Effect->GetParameter(NULL, c);
			D3DXPARAMETER_DESC param;
			Effect->GetParameterDesc(handle, &param);
			if (memcmp(param.Name, "TESR_", 5) == 0) visit(handle, param);
		}
	};

	// Count first, so the arrays can be sized before they are filled (added to the counts as they stand).
	UInt32 newFloats = 0, newSamplers = 0;
	forEachTesr([&](D3DXHANDLE, const D3DXPARAMETER_DESC& param) {
		if (!strcmp(param.Name, "TESR_SourceBuffer")) usesSourceBuffer = true;
		if (param.Class == D3DXPC_VECTOR || param.Class == D3DXPC_MATRIX_ROWS) newFloats++;
		if (param.Class == D3DXPC_OBJECT && param.Type >= D3DXPT_SAMPLER && param.Type <= D3DXPT_SAMPLERCUBE) newSamplers++;
	});
	FloatShaderValuesCount += newFloats;
	TextureShaderValuesCount += newSamplers;
	FloatShaderValues = new ShaderFloatValue[FloatShaderValuesCount];
	TextureShaderValues = new ShaderTextureValue[TextureShaderValuesCount];

	//Logger::Debug("CreateCT: Effect has %i constants", desc.Parameters);

	forEachTesr([&](D3DXHANDLE handle, const D3DXPARAMETER_DESC& param) {
		if (param.Class == D3DXPC_VECTOR || param.Class == D3DXPC_MATRIX_ROWS) {
			ShaderFloatValue& value = FloatShaderValues[floatSlot++];
			value.Name = param.Name;
			value.Type = (D3DXPARAMETER_TYPE)param.Class;
			value.RegisterIndex = (UInt32)handle;
			value.RegisterCount = param.Elements;
			return;
		}
		if (param.Class != D3DXPC_OBJECT || param.Type < D3DXPT_SAMPLER || param.Type > D3DXPT_SAMPLERCUBE) return;
		ShaderTextureValue& tex = TextureShaderValues[texSlot];
		tex.Name = param.Name;
		tex.Type = TextureRecord::GetTextureType(param.Type);
		tex.RegisterIndex = texSlot;
		tex.RegisterCount = 1;
		tex.GetSamplerStateString(ShaderSource, texSlot);
		tex.GetTextureRecord();
		texSlot++;
	});

	tableClock.LogTime("EffectRecord::ConstantTableDesc");
}


/*
* Pushes the effect's textures (with their sampler states) and constants to the device and the D3DX effect.
*/
void EffectRecord::SetCT() {
	if (!Enabled || !Effect) return;	// nothing to feed
	SkyProjection::Join();	// UNOFFICIAL 2026-11-03 SkyOnWorker: the sky's results in its constants first
	MixCheck::SetPass(Name);	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck: the D3D9 pass the next binds and draws belong to

	for (UInt32 i = 0; i < TextureShaderValuesCount; i++) {
		ShaderTextureValue& slot = TextureShaderValues[i];
		TextureRecord* tex = slot.Texture;
		// Follow the TextureManager slot rather than the pointer cached at first bind, so the
		// frame chain's swapped rendered buffer is picked up.
		if (tex->TextureRef) tex->Texture = *tex->TextureRef;
		if (!tex->Texture) {
			tex->BindTexture(slot.Name);
			if (tex->Texture) Logger::Log("%s : Texture %s Succesfully bound", Name, slot.Name);
			//else Logger::Log("[ERROR] : Could not bind texture %s for effect %s", slot.Name, Name);
		}
		if (tex->Texture == nullptr) continue;

		TheRenderManager->device->SetTexture(slot.RegisterIndex, tex->Texture);
		TheRenderManager->renderState->NoteDeviceTexture(slot.RegisterIndex, tex->Texture);	// UNOFFICIAL: keep the game's texture cache truthful
		for (int state = 1; state < SamplerStatesMax; state++)
			// (every state set: the hook writes the device directly for the sampler types the game does not track, Render.cpp SetSamplerStateHook)
			TheRenderManager->SetSamplerState(slot.RegisterIndex, (D3DSAMPLERSTATETYPE)state, tex->SamplerStates[state]);
	}

	for (UInt32 i = 0; i < FloatShaderValuesCount; i++) {
		ShaderFloatValue& constant = FloatShaderValues[i];
		if (constant.Value == nullptr) constant.GetValueFromConstantTable();
		if (constant.Value == nullptr) {
			Logger::Log("[Error] %s : Couldn't get value for Constant %s", Name, constant.Name);
			continue;
		}

		const D3DXHANDLE handle = (D3DXHANDLE)constant.RegisterIndex;
		const bool isVector = constant.Type == (D3DXPARAMETER_TYPE)D3DXPC_VECTOR;
		const bool isArray = constant.RegisterCount > 1;
		if (isVector && isArray) Effect->SetVectorArray(handle, constant.Value, constant.RegisterCount);
		else if (isVector) Effect->SetVector(handle, constant.Value);
		else if (isArray) Effect->SetMatrixArray(handle, (D3DXMATRIX*)constant.Value, constant.RegisterCount);
		else Effect->SetMatrix(handle, (D3DXMATRIX*)constant.Value);
	}
}

/*
 * Enable or Disable Effect, with loading if not loaded
 */
bool EffectRecord::SwitchEffect() {
	// Turning on an effect that is not loaded loads it first; the switch flips only when that worked.
	if (!Enabled && !IsLoaded()) {
		Logger::Log("Effect %s is not loaded", Name);
		DisposeEffect();
		if (!LoadEffect()) {
			char failure[256];
			snprintf(failure, sizeof(failure), "Error: Couldn't enable effect %s", Name);
			InterfaceManager->ShowMessage(failure);
			Logger::Log(failure);
			return Enabled;
		}
	}
	Enabled = !Enabled;
	return Enabled;
}



bool EffectRecord::PassNeedsPrefill(UINT techniqueIndex, UINT pass) const {
	if (!needsPrefill) return false;
	if (techniqueIndex < passPrefill.size() && pass < passPrefill[techniqueIndex].size()) return passPrefill[techniqueIndex][pass];
	return true;
}

/*
* Re-binds samplers that follow a TextureManager slot, for use after BeginPass when a slot's
* texture changed since SetCT (the frame chain swaps TESR_RenderedBuffer between passes).
*/
// UNOFFICIAL 2026-10-19 (see PassTextures in EffectRecord.h).
void EffectRecord::BindPassTextures() {
	for (UINT stage = 0; stage < 16; stage++) {
		if (!PassTextures[stage]) continue;
		TheRenderManager->device->SetTexture(stage, PassTextures[stage]);
		TheRenderManager->renderState->NoteDeviceTexture(stage, PassTextures[stage]);
	}
}

void EffectRecord::ClearPassTextures() {
	for (UINT stage = 0; stage < 16; stage++) {
		if (!PassTextures[stage]) continue;
		PassTextures[stage] = nullptr;
		TheRenderManager->device->SetTexture(stage, nullptr);
		TheRenderManager->renderState->NoteDeviceTexture(stage, nullptr);
	}
}

void EffectRecord::RebindSlotTextures() {
	ShaderTextureValue* const end = TextureShaderValues + TextureShaderValuesCount;
	for (ShaderTextureValue* slot = TextureShaderValues; slot != end; ++slot) {
		TextureRecord* record = slot->Texture;
		auto current = (record && record->TextureRef) ? *record->TextureRef : nullptr;
		if (!current || record->Texture == current) continue;	// no slot, empty slot, or already bound
		record->Texture = current;
		TheRenderManager->device->SetTexture(slot->RegisterIndex, current);
		TheRenderManager->renderState->NoteDeviceTexture(slot->RegisterIndex, current);
	}
}

/**
* Draws the effect's current technique over the frame: through the frame chain when the target is one of its surfaces,
* else straight into the target with the copies the effect's samplers need.
*/
void EffectRecord::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	// Off, not loaded or not wanted this frame: nothing drawn, no time.
	if (!Enabled || !Effect || !ShouldRender()) {
		// UNOFFICIAL 2026-11-01 (core/EffectStatus.h): why it did not draw, for the FLASH / REPORT dumps and the HEARTBEAT counts
		EffectStatus::Note(StatusSlot, Name, !Enabled ? EffectStatus::Disabled : !Effect ? EffectStatus::NotLoaded : EffectStatus::ShouldRenderFalse);
		renderTime = 0.0f;
		return;
	}
	SessionLog::StageScope stage(Name);	// 2026-11-04 (CRASH lines)
	if (Diagnostics::SelfTestSkip(this)) {	// UNOFFICIAL 2026-11-01 the flash detector's self-test: a one-frame dropout on purpose (off unless FlashSelfTestEvery)
		EffectStatus::Note(StatusSlot, Name, EffectStatus::Skipped, "flash self-test: left out this frame on purpose ([Main.Develop.Main] FlashSelfTestEvery)");
		renderTime = 0.0f;
		return;
	}
	EffectStatus::Note(StatusSlot, Name, EffectStatus::Drawn);	// UNOFFICIAL 2026-11-01 (core/EffectStatus.h)

	// UNOFFICIAL 2026-10-26 pixel inspector ([Main.Develop.PixelInspector]): when this effect has drawn, the picture after it (or its
	// own target) is recorded at the inspected pixel. Off: one bool test at the end of the call.
	PixelInspector::EffectScope inspect(this, RenderTarget, RenderedSurface, techniqueIndex);

	TimeLogger drawClock;
	GpuDebug::LabelScope label(Name);	// UNOFFICIAL 2026-10-26 GpuDebugLabels (off: nothing)
	// Stores this call's time under the effect's name in the log and in renderTime.
	auto noteTime = [&](const char* prefix) {
		std::string tag = std::string(prefix) + std::string(Name);
		renderTime = drawClock.LogTime(tag.c_str());
	};
	// The two paths that drew into other surfaces hand the caller's target back before timing.
	auto finishOnTarget = [&](const char* prefix) {
		Device->SetRenderTarget(0, RenderTarget);
		noteTime(prefix);
	};
	static const D3DCOLOR kBlack = D3DCOLOR_ARGB(255, 0, 0, 0);

	// UNOFFICIAL 2026-10-23 (NVR DXVK): the ported effects draw through the extension when it is there and on.
	// (UNOFFICIAL 2026-10-26: not for a technique whose passes all have an SM6 build while NvrSm6Shaders is on: those draw below;
	// nor for an effect NvrDxvkKeepD3D9 names.)
	if (!ExtFailed && DxvkCompute::PassesOn() && !DxvkCompute::KeptOnD3D9(Name) && !Sm6Covers(techniqueIndex) && [&] { GpuDebug::LabelScope ext(Name, " (DXVK extension)");
			return ExtRender(Device, RenderTarget, RenderedSurface, techniqueIndex, ClearRenderTarget, SourceBuffer); }()) {
		finishOnTarget("EffectRecord::ExtRender ");
		return;
	}

	ID3DXEffect* const fx = Effect;
	FrameChain& chain = TheShaderManager->Chain;
	if (chain.Owns(RenderTarget, RenderedSurface)) {
		// Copy-free path: each pass renders into the chain's spare texture, which then becomes the
		// current image (and so TESR_RenderedBuffer) for the next pass and the next effect.
		if (SourceBuffer && usesSourceBuffer && SourceBuffer != RenderTarget)
			Device->StretchRect(TheTextureManager->RenderedSurface, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);
		fx->SetTechnique(fx->GetTechnique(techniqueIndex));
		SetCT();
		UINT passTotal = 0;
		if (SUCCEEDED(fx->Begin(&passTotal, NULL))) {
			for (UINT pass = 0; pass < passTotal; pass++) {
				const bool prefill = !ClearRenderTarget && !writesWholeTarget && PassNeedsPrefill(techniqueIndex, pass);
				// The last pass of the chain's final effect renders straight into the game target, so
				// the chain has no copy to make when it ends. Not worth it for a pass that has to be
				// pre-filled: that is the same copy, just earlier.
				const bool direct = (pass + 1 == passTotal) && !prefill && chain.IsDirectFinal(this);
				IDirect3DSurface9* target = direct ? chain.FinalSurface() : chain.Output();
				if (prefill) Device->StretchRect(TheTextureManager->RenderedSurface, NULL, target, NULL, D3DTEXF_NONE);
				Device->SetRenderTarget(0, target);
				if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, kBlack, 1.0f, 0L);
				fx->BeginPass(pass);
				RebindSlotTextures(); // the current image moved after the previous pass
				BindPassTextures();	// UNOFFICIAL 2026-10-19
				// UNOFFICIAL 2026-10-26 (NvrSm6Shaders): the pass's SM6 build, fed what this draw would read; else (or failed) the D3D9 draw
				Sm6Pass* sm6 = Sm6For(techniqueIndex, pass);
				RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2 (rect frames: top-left part; else FrameVertex)
				if (!sm6 || !sm6->Plain() || Sm6Pass::Scissored(Device) || !sm6->Draw(Device, &target, 1, ExtUv::ScreenQuad)) Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				fx->EndPass();
				if (direct) chain.CommitFinal();
				else chain.Commit();
			}
			fx->End();
		}
		finishOnTarget("EffectRecord::Render ");
		return;
	}

	// Effects that never sample TESR_SourceBuffer do not need the full-resolution copy. Every
	// effect that does sample it declares the sampler and so still refreshes it here itself.
	if (SourceBuffer && usesSourceBuffer && SourceBuffer != RenderTarget)
		Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);

	// Legacy path: every pass into RenderTarget, its result copied into RenderedSurface for the next pass to read.
	try {
		fx->SetTechnique(fx->GetTechnique(techniqueIndex));
		SetCT();
		UINT passTotal = 0;
		fx->Begin(&passTotal, NULL);
		const bool copyBack = RenderedSurface && RenderedSurface != RenderTarget;
		for (UINT pass = 0; pass < passTotal; pass++) {
			if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, kBlack, 1.0f, 0L);
			fx->BeginPass(pass);
			BindPassTextures();	// UNOFFICIAL 2026-10-19
			BeforeDraw(Device);	// UNOFFICIAL 2026-10-22
			Sm6Pass* sm6 = Sm6For(techniqueIndex, pass);	// UNOFFICIAL 2026-10-26 (NvrSm6Shaders), as above
			RenderRect::ApplyViewport(Device);	// UNOFFICIAL 2026-10-27 RenderScale v2
			if (!sm6 || !sm6->Plain() || Sm6Pass::Scissored(Device) || !sm6->Draw(Device, &RenderTarget, 1, ExtUv::ScreenQuad)) Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			fx->EndPass();
			if (copyBack) Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_LINEAR);
		}
		fx->End();
	}
	catch (const std::exception& failure) {
		Logger::Log("Error during rendering of effect %s: %s", Name, failure.what());
	}

	noteTime("EffectRecord::Render ");
}


// UNOFFICIAL: see CachedHandle in EffectRecord.h.
D3DXHANDLE EffectRecord::TechniqueHandle(CachedHandle& cache, const char* name) {
	if (cache.Generation != LoadGeneration) {
		cache.Generation = LoadGeneration;
		cache.Handle = Effect ? Effect->GetTechniqueByName(name) : NULL;
	}
	return cache.Handle;
}

D3DXHANDLE EffectRecord::ParameterHandle(CachedHandle& cache, const char* name) {
	if (cache.Generation != LoadGeneration) {
		cache.Generation = LoadGeneration;
		cache.Handle = Effect ? Effect->GetParameterByName(NULL, name) : NULL;
	}
	return cache.Handle;
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): see EffectRecord.h. One full-screen pass through the frame chain, as Render's single-pass
// case: into the chain's spare texture and Commit, straight into the game target when the chain predicted this effect last, or
// (no chain) into RenderTarget with the copy into RenderedSurface afterwards. The pass writes every pixel (no pre-fill).
bool EffectRecord::ExtFullScreen(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
	ID3D9NvrExtPipeline* Pipeline, const NVR_EXT_RESOURCE_BINDING* Bindings, UINT BindingCount, const void* Push, UINT PushSize, bool last) {
	if (!Pipeline || !DxvkCompute::Available()) return false;
	if (DxvkCompute::KeptOnD3D9(Name)) return false;	// (NvrDxvkKeepD3D9 for effects with their own Render: this frame on D3D9)
	ID3D9NvrExt* ext = DxvkCompute::Extension();
	MixCheck::SetPass(Name);	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck

	NVR_EXT_DRAW_DESC draw = {};
	draw.pPipeline = Pipeline;
	draw.TargetCount = 1;
	draw.BindingCount = BindingCount;
	draw.pBindings = Bindings;
	draw.pPushData = Push;
	draw.PushDataSize = PushSize;

	FrameChain& chain = TheShaderManager->Chain;
	HRESULT result;
	if (chain.Owns(RenderTarget, RenderedSurface)) {
		if (SourceBuffer && usesSourceBuffer && SourceBuffer != RenderTarget)
			Device->StretchRect(TheTextureManager->RenderedSurface, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);
		const bool direct = last && chain.IsDirectFinal(this);
		draw.Targets[0].pResource = direct ? chain.FinalSurface() : chain.Output();
		// UNOFFICIAL 2026-10-26 (alflicker): the managed file textures on the GPU. (2026-10-30: after the SourceBuffer copy above, so
		// NvrDxvkMixCheck sees the pass read the copy it made.)
		DxvkCompute::PreLoad(Bindings, BindingCount);
		result = ext->Draw(&draw);
		if (SUCCEEDED(result)) MixCheck::NoteWrite(static_cast<IDirect3DResource9*>(static_cast<void*>(draw.Targets[0].pResource)), MixCheck::kVulkan);
		if (SUCCEEDED(result)) { if (direct) chain.CommitFinal(); else chain.Commit(); }
	}
	else {
		if (SourceBuffer && usesSourceBuffer && SourceBuffer != RenderTarget)
			Device->StretchRect(RenderTarget, NULL, SourceBuffer, NULL, D3DTEXF_LINEAR);
		draw.Targets[0].pResource = RenderTarget;
		DxvkCompute::PreLoad(Bindings, BindingCount);	// (as above)
		result = ext->Draw(&draw);
		if (SUCCEEDED(result)) MixCheck::NoteWrite(RenderTarget, MixCheck::kVulkan);
		if (SUCCEEDED(result) && RenderedSurface && RenderedSurface != RenderTarget)
			Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_LINEAR);	// the result into the texture, as the legacy path
	}
	if (FAILED(result)) {
		ExtFailed = true;
		EffectStatus::D3DFail("Vegas Shaders DXVK pass draw (EffectRecord::ExtFullScreen)", result);	// UNOFFICIAL 2026-11-01
		EffectStatus::Level(Name, "Vegas Shaders DXVK pass failed: its D3D9 technique for the session", false);
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:pass %s failed to draw (%08lX; DXVK's log says why): its D3D9 technique is used from now on", Name, result);
		return false;
	}
	if (++ExtFrames == 1) Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s runs as a Vulkan pass.", Name);
	return true;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): see EffectRecord.h. A pass into the effect's own targets; the frame chain is not touched.
bool EffectRecord::ExtDrawTo(ID3D9NvrExtPipeline* Pipeline, IDirect3DSurface9* const* Targets, UINT TargetCount, const NVR_EXT_RESOURCE_BINDING* Bindings,
	UINT BindingCount, const void* Push, UINT PushSize, const RECT* Viewport, ExtUv Uv, const NVR_EXT_DEPTH_STENCIL* DepthStencil) {
	if (!Pipeline || !DxvkCompute::Available() || !TargetCount || TargetCount > NVR_EXT_MAX_TARGETS) return false;
	if (DepthStencil && DxvkCompute::Info().ExtVersionMinor < 6) return false;
	if (DxvkCompute::KeptOnD3D9(Name)) return false;	// (NvrDxvkKeepD3D9, as ExtFullScreen; idea from the conor-fork-flicker session)
	MixCheck::SetPass(Name);	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
	NVR_EXT_DRAW_DESC draw = {};
	draw.pPipeline = Pipeline;
	draw.TargetCount = TargetCount;
	for (UINT i = 0; i < TargetCount; i++) {
		if (!Targets[i]) return false;
		draw.Targets[i].pResource = Targets[i];
	}
	if (Viewport) {
		draw.ViewportX = Viewport->left;
		draw.ViewportY = Viewport->top;
		draw.ViewportWidth = Viewport->right - Viewport->left;
		draw.ViewportHeight = Viewport->bottom - Viewport->top;
	}
	draw.BindingCount = BindingCount;
	draw.pBindings = Bindings;
	draw.pPushData = Push;
	draw.PushDataSize = PushSize;
	// UNOFFICIAL 2026-10-26: the D3D9 frame quad's half-pixel offset (EffectRecord.h ExtUv). The viewport's size: the given
	// rectangle, else the target's.
	float shiftX = 0.0f, shiftY = 0.0f;
	if (Uv == ExtUv::ScreenQuad) {
		UINT w = draw.ViewportWidth, h = draw.ViewportHeight;
		if (!w || !h) {
			D3DSURFACE_DESC firstTarget;
			if (FAILED(Targets[0]->GetDesc(&firstTarget))) return false;
			w = firstTarget.Width; h = firstTarget.Height;
		}
		shiftX = 0.5f - 0.5f * (float)w / (float)TheRenderManager->width;
		shiftY = 0.5f - 0.5f * (float)h / (float)TheRenderManager->height;
		if (fabsf(shiftX) < 1e-6f) shiftX = 0.0f;
		if (fabsf(shiftY) < 1e-6f) shiftY = 0.0f;
		if ((shiftX != 0.0f || shiftY != 0.0f) && DxvkCompute::Info().ExtVersionMinor < 4) {
			static bool said = false;
			if (!said) Logger::Log("UNOFFICIAL Vegas Shaders DXVK:this d3d9.dll has extension %u.%u: passes into targets smaller than the screen read at the texel centre, not where their D3D9 pass reads (a quarter texel at half resolution; needs 1.4)", DxvkCompute::Info().ExtVersionMajor, DxvkCompute::Info().ExtVersionMinor);
			said = true; shiftX = shiftY = 0.0f;
		}
	}
	DxvkCompute::PreLoad(Bindings, BindingCount);	// UNOFFICIAL 2026-10-26 (alflicker): the managed file textures on the GPU
	HRESULT result = DepthStencil ? DxvkCompute::Extension()->DrawDepthStencil(&draw, DepthStencil, shiftX, shiftY)
		: (shiftX != 0.0f || shiftY != 0.0f) ? DxvkCompute::Extension()->DrawShifted(&draw, shiftX, shiftY) : DxvkCompute::Extension()->Draw(&draw);
	if (SUCCEEDED(result) && (shiftX != 0.0f || shiftY != 0.0f) && !ExtShiftLogged) {
		ExtShiftLogged = true;
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:%s's passes into smaller targets read at the D3D9 frame quad's uv (first: shift %.3f, %.3f px)", Name, shiftX, shiftY);
	}
	if (FAILED(result)) {
		ExtFailed = true;
		EffectStatus::D3DFail("Vegas Shaders DXVK pass draw (EffectRecord::ExtDrawTo)", result);	// UNOFFICIAL 2026-11-01
		EffectStatus::Level(Name, "Vegas Shaders DXVK pass failed: its D3D9 technique for the session", false);
		Logger::Log("UNOFFICIAL Vegas Shaders DXVK:pass %s failed to draw into its own target (%08lX; DXVK's log says why): its D3D9 technique is used from now on", Name, result);
		return false;
	}
	for (UINT i = 0; MixCheck::On && i < TargetCount; i++) MixCheck::NoteWrite(Targets[i], MixCheck::kVulkan);	// UNOFFICIAL 2026-10-30 NvrDxvkMixCheck
	return true;
}

// UNOFFICIAL 2026-10-26 (NVR DXVK): see EffectRecord.h.
ID3D9NvrExtPipeline* EffectRecord::ExtVariant(std::map<std::string, ExtPass>& Variants, ExtPass& Generic, const char* SpvName, const char* Suffix,
	const std::function<ID3D9NvrExtPipeline*(const char*)>& Make) {
	// UNOFFICIAL 2026-10-26 test tool ([Main.Main.Performance] NvrDxvkTestSuffix): <SpvName>-<Suffix><test>.spv first, where it exists.
	const char* test = TheSettingManager->SettingsMain.Main.NvrDxvkTestSuffix;
	if (Suffix && *Suffix && *test) {
		const std::string name = std::string(SpvName) + "-" + Suffix + test;
		ExtPass& pass = Variants[name];
		if (!pass.Tried) {
			pass.Tried = true;
			if (DxvkCompute::HasSpirv(name.c_str())) pass.Pipeline = Make(name.c_str());
			if (pass.Pipeline) Logger::Log("UNOFFICIAL Vegas Shaders DXVK:test variant %s in use (NvrDxvkTestSuffix)", name.c_str());
		}
		if (pass.Pipeline) return pass.Pipeline;
	}
	if (Suffix && *Suffix && DxvkCompute::VariantsOn()) {
		const std::string name = std::string(SpvName) + "-" + Suffix;
		ExtPass& pass = Variants[name];
		if (!pass.Tried) {
			pass.Tried = true;
			if (DxvkCompute::HasSpirv(name.c_str())) pass.Pipeline = Make(name.c_str());
		}
		if (pass.Pipeline) return pass.Pipeline;
	}
	if (!Generic.Tried) {
		Generic.Tried = true;
		Generic.Pipeline = Make(SpvName);
	}
	return Generic.Pipeline;
}

// UNOFFICIAL 2026-10-25 (NVR DXVK): see EffectRecord.h.
ID3D9NvrExtBuffer* EffectRecord::ExtUniforms(ExtUniformBuffer& Uniforms, const void* Data, UINT Size) {
	if (!DxvkCompute::Available() || !Data || !Size) return nullptr;
	if (Uniforms.Buffer && Uniforms.Size != Size) DxvkCompute::Drop(Uniforms.Buffer);
	if (!Uniforms.Buffer) {
		if (Uniforms.Tried) return nullptr;
		Uniforms.Tried = true;
		Uniforms.Buffer = DxvkCompute::CreateBuffer((Size + 15) & ~15u, NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, Name);
		Uniforms.Size = Size;
		if (!Uniforms.Buffer) return nullptr;
	}
	if (FAILED(DxvkCompute::Extension()->UpdateBuffer(Uniforms.Buffer, 0, Size, Data))) return nullptr;
	return Uniforms.Buffer;
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): see EffectRecord.h.
bool EffectRecord::ExtTexturePass(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, IDirect3DSurface9* SourceBuffer,
	ExtPass& Pass, const char* SpvName, IDirect3DBaseTexture9* const* Textures, UINT TextureCount, const void* Push, UINT PushSize, UINT SamplerPushOffset, bool PointSampler, UINT WrapSamplerPushOffset, bool PointWrapSampler, bool Last) {
	if (TextureCount > 8) return false;
	const UINT samplers = WrapSamplerPushOffset ? 2 : 1;
	if (!Pass.Tried) {
		Pass.Tried = true;
		NVR_EXT_BINDING_DESC bindings[10] = {};
		for (UINT i = 0; i < TextureCount; i++) {
			bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
		}
		bindings[TextureCount].Type = NVR_EXT_BIND_SAMPLER; bindings[TextureCount].Slot = 0; bindings[TextureCount].PushOffset = SamplerPushOffset;
		bindings[TextureCount + 1].Type = NVR_EXT_BIND_SAMPLER; bindings[TextureCount + 1].Slot = 1; bindings[TextureCount + 1].PushOffset = WrapSamplerPushOffset;
		Pass.Pipeline = DxvkCompute::CreateGraphicsPipeline(SpvName, bindings, TextureCount + samplers, PushSize, true);
	}
	ID3D9NvrExtSampler* sampler = PointSampler ? DxvkCompute::PointClamp() : DxvkCompute::LinearClamp();
	ID3D9NvrExtSampler* wrap = WrapSamplerPushOffset ? (PointWrapSampler ? DxvkCompute::PointWrap() : DxvkCompute::LinearWrap()) : nullptr;
	if (!Pass.Pipeline || !sampler || (WrapSamplerPushOffset && !wrap)) return false;
	NVR_EXT_RESOURCE_BINDING resources[10] = {};
	for (UINT i = 0; i < TextureCount; i++) {
		if (!Textures[i]) return false;
		resources[i].Slot = i; resources[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; resources[i].pResource = Textures[i];
	}
	resources[TextureCount].Slot = 0; resources[TextureCount].Type = NVR_EXT_BIND_SAMPLER; resources[TextureCount].pResource = sampler;
	resources[TextureCount + 1].Slot = 1; resources[TextureCount + 1].Type = NVR_EXT_BIND_SAMPLER; resources[TextureCount + 1].pResource = wrap;
	return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, Pass.Pipeline, resources, TextureCount + samplers, Push, PushSize, Last);
}

IDirect3DBaseTexture9* EffectRecord::ExtFileTexture(const char* SamplerName) {
	for (UInt32 i = 0; i < TextureShaderValuesCount; i++) {
		const ShaderTextureValue& entry = TextureShaderValues[i];
		if (entry.Name && entry.Texture && !strcmp(entry.Name, SamplerName)) return entry.Texture->Texture;
	}
	return nullptr;
}

