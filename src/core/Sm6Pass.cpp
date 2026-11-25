#include <algorithm>
#include <sstream>
#include "Sm6Pass.h"
#include "FrameWorker.h"	// UNOFFICIAL 2026-11-03 SkyOnWorker
#include "DxvkCompute.h"
#include "RenderRect.h"	// UNOFFICIAL 2026-10-27 RenderScale v2

// UNOFFICIAL 2026-10-26 (deep/fork-sm6): see Sm6Pass.h. Files: tools\nvr-sm6\build.py writes them, hlsl2spv.py documents the layout.

// The shader root and the Sm6 cache folder (Paths.h: Data\Shaders\VegasShaders\, or the old folder of an older install)
static std::string Sm6Root() { return Paths::ShaderRoot(); }
static std::string Sm6Folder() { return std::string(Paths::ComputeDir()) + "Sm6\\"; }

bool Sm6Pass::On() {
	// UNOFFICIAL 2026-10-27 RenderScale v2: not in rect frames (the SM6 builds draw the whole target).
	return TheSettingManager->SettingsMain.Main.NvrSm6Shaders && !RenderRect::Active() && DxvkCompute::Available() && DxvkCompute::Info().ExtVersionMinor >= 1;
}

static UInt32 Sm6Fnv32(const std::string& s) {
	UInt32 h = 2166136261u;
	for (unsigned char c : s) h = (h ^ c) * 16777619u;
	return h;
}

static bool Sm6FileHash(const char* path, unsigned long long& hash) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	unsigned long long h = 0xcbf29ce484222325ull;
	char buffer[65536];
	while (file) {
		file.read(buffer, sizeof(buffer));
		for (std::streamsize i = 0; i < file.gcount(); i++) h = (h ^ (unsigned char)buffer[i]) * 0x100000001b3ull;
	}
	hash = h;
	return true;
}

static bool Sm6ReadFile(const char* path, std::vector<char>& data) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) return false;
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	data.resize((size_t)size);
	return size > 0 && (bool)file.read(data.data(), size);
}

static NVR_EXT_FILTER Sm6Filter(const std::string& name) {
	if (name == "NEAREST") return NVR_EXT_FILTER_NEAREST;
	if (name == "LINEAR_MIPMAPS") return NVR_EXT_FILTER_LINEAR_MIPMAPS;
	if (name == "ANISOTROPIC") return NVR_EXT_FILTER_ANISOTROPIC;
	return NVR_EXT_FILTER_LINEAR;
}

static NVR_EXT_ADDRESS Sm6Address(const std::string& name) {
	if (name == "WRAP") return NVR_EXT_ADDRESS_WRAP;
	if (name == "MIRROR") return NVR_EXT_ADDRESS_MIRROR;
	if (name == "BORDER") return NVR_EXT_ADDRESS_BORDER;
	return NVR_EXT_ADDRESS_CLAMP;
}

// One sampler object per state for all SM6 passes (made on first use, kept for the session).
static ID3D9NvrExtSampler* Sm6Sampler(NVR_EXT_FILTER filter, NVR_EXT_ADDRESS address) {
	static ID3D9NvrExtSampler* samplers[4][4] = {};
	if ((UINT)filter > 3 || (UINT)address > 3) return nullptr;
	if (filter >= NVR_EXT_FILTER_LINEAR_MIPMAPS && DxvkCompute::Info().ExtVersionMinor < 3) filter = NVR_EXT_FILTER_LINEAR;	// (1.3 has mips)
	ID3D9NvrExtSampler*& s = samplers[filter][address];
	if (!s) s = DxvkCompute::CreateSampler(filter, address);
	return s;
}

bool Sm6Pass::Fail(const char* Why) {
	if (!Failed) Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s: %s; this pass keeps its D3D9 shader for the session", Key.c_str(), Why);
	Failed = true;
	return false;
}

bool Sm6Pass::LoadLayout(const char* Path) {
	std::ifstream file(Path);
	if (!file) return Fail("no layout file (run tools\\nvr-sm6\\build.py)");
	std::string line;
	bool header = false;
	while (std::getline(file, line)) {
		std::istringstream in(line);
		std::string tag;
		in >> tag;
		if (tag == "nvr-sm6") { int version = 0; in >> version; header = version == 1; }
		else if (tag == "hash") {
			std::string hex, rel;
			in >> hex >> rel;
			for (char& c : rel) if (c == '/') c = '\\';
			std::string path = Sm6Root() + rel;
			unsigned long long want = strtoull(hex.c_str(), nullptr, 16), have = 0;
			if (!Sm6FileHash(path.c_str(), have)) return Fail(("its source " + path + " is missing").c_str());
			if (have != want) return Fail(("stale: " + path + " changed since the SM6 build (run tools\\nvr-sm6\\build.py)").c_str());
		}
		else if (tag == "const") {
			Constant c;
			in >> c.Name >> c.Offset >> c.Size >> c.Type;
			Constants.push_back(c);
		}
		else if (tag == "default") {
			std::string name;
			in >> name;
			for (Constant& c : Constants) if (c.Name == name) { float v; while (in >> v) c.Default.push_back(v); }
		}
		else if (tag == "ubo") { UINT set, binding; in >> set >> binding >> UboSize; }
		else if (tag == "texture") {
			Texture t;
			std::string view, used;
			in >> t.Name >> t.Binding >> view >> t.Slot >> used;
			t.View = view == "3D" ? NVR_EXT_VIEW_3D : view == "CUBE" ? NVR_EXT_VIEW_CUBE : NVR_EXT_VIEW_2D;
			if (used == "used") Textures.push_back(t);
		}
		else if (tag == "sampler") {
			UINT slot; std::string filter, address;
			if (!(in >> slot >> filter >> address) || slot >= 64) return Fail("a bad sampler line");	// (2026-11-17: the slot sized and indexed the list unchecked)
			SamplerStates.push_back({ slot, filter + " " + address });
			ID3D9NvrExtSampler* s = Sm6Sampler(Sm6Filter(filter), Sm6Address(address));
			if (!s) return Fail("a sampler could not be made");
			if (Samplers.size() <= slot) Samplers.resize(slot + 1, nullptr);
			Samplers[slot] = s;
		}
		else if (tag == "push") in >> PushSize;
		else if (tag == "vs") in >> VertexShader;
		else if (tag == "states") in >> StateCount;
		else if (tag == "targets") in >> TargetCount;
	}
	if (!header) return Fail("not an nvr-sm6 layout");
	if (UboSize > (1u << 20)) return Fail("bad uniform buffer size");	// (2026-11-17: allocated as given)
	for (ID3D9NvrExtSampler* s : Samplers) if (!s) return Fail("a sampler slot has no state");
	return true;
}

bool Sm6Pass::Prepare(EffectRecord* Effect, const char* Technique, UINT Pass) {
	if (!Effect || !Effect->Effect || !Technique) return false;
	if (Tried && Owner == Effect && Generation == Effect->LoadGeneration) return Ready();

	DxvkCompute::Drop(Pipeline);
	Constants.clear(); Textures.clear(); SamplerStates.clear(); Samplers.clear(); Missing.clear();
	UboSize = PushSize = 0; TargetCount = 1; Frames = 0; VertexShader.clear(); StateCount = 0;
	Owner = Effect;
	Generation = Effect->LoadGeneration;
	Tried = true;
	Failed = false;
	char key[160];
	_snprintf_s(key, sizeof(key), _TRUNCATE, "%s.%s.%u", Effect->Name, Technique, Pass);
	Key = key;

	// which of the effects' defines this pass was built for, and the variant matching the ones this effect was compiled with
	std::string index = Sm6Folder() + Key + ".txt";
	std::ifstream indexFile(index);
	if (!indexFile) { Failed = true; return false; }	// the normal case: no SM6 build of this pass (quiet)
	D3DXMACRO macros[10];	// (2026-11-14: room for 10, EffectRecord::BuildMacros)
	EffectRecord::BuildMacros(macros);
	std::string line, variant;
	while (std::getline(indexFile, line)) {
		std::istringstream in(line);
		std::string tag, name;
		in >> tag;
		if (tag != "macros") continue;
		while (in >> name) {
			const D3DXMACRO* found = nullptr;
			for (const D3DXMACRO* m = macros; m->Name; m++) if (name == m->Name) found = m;
			if (!variant.empty()) variant += ";";
			variant += found ? name + "=" + found->Definition : name + "-";
		}
	}
	char hash[16];
	_snprintf_s(hash, sizeof(hash), _TRUNCATE, "%08x", Sm6Fnv32(variant));
	const std::string base = Sm6Folder() + Key + "." + hash;
	if (!LoadLayout((base + ".txt").c_str())) {
		Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s: variant %s (%s)", Key.c_str(), hash, variant.c_str());
		return false;
	}

	// the D3D9 pass: each sampler's register (what it would sample), each constant's parameter (what it would read)
	D3DXHANDLE technique = Effect->Effect->GetTechniqueByName(Technique);
	if (!technique && !strcmp(Technique, "default")) technique = Effect->Effect->GetTechnique(0);	// (the unnamed technique)
	D3DXHANDLE pass = technique ? Effect->Effect->GetPass(technique, Pass) : NULL;
	D3DXPASS_DESC passDesc = {};
	ID3DXConstantTable* table = nullptr;
	if (pass && SUCCEEDED(Effect->Effect->GetPassDesc(pass, &passDesc)) && passDesc.pPixelShaderFunction)
		D3DXGetShaderConstantTable(passDesc.pPixelShaderFunction, &table);
	for (Texture& t : Textures) {
		D3DXHANDLE handle = table ? table->GetConstantByName(NULL, t.Name.c_str()) : NULL;
		D3DXCONSTANT_DESC desc;
		UINT count = 1;
		if (handle && SUCCEEDED(table->GetConstantDesc(handle, &desc, &count)) && desc.RegisterSet == D3DXRS_SAMPLER) t.Stage = (int)desc.RegisterIndex;
	}
	if (table) table->Release();
	for (Constant& c : Constants) {
		c.Handle = Effect->Effect->GetParameterByName(NULL, c.Name.c_str());
		if (c.Handle && FAILED(Effect->Effect->GetParameterDesc(c.Handle, &c.Desc))) c.Handle = NULL;
	}

	// bindings: textures set 1 (slot = binding), the uniform buffer set 2 binding 0, samplers by slot with their heap index in push data
	std::vector<NVR_EXT_BINDING_DESC> bindings;
	for (const Texture& t : Textures) {
		NVR_EXT_BINDING_DESC b = {};
		b.Set = 1; b.Binding = t.Binding; b.Type = NVR_EXT_BIND_SAMPLED_IMAGE; b.Slot = t.Binding; b.ViewType = t.View;
		if (t.Binding >= DxvkCompute::Info().MaxSlots) return Fail("more textures than this Vegas Shaders DXVK has slots");
		bindings.push_back(b);
	}
	if (UboSize) {
		NVR_EXT_BINDING_DESC b = {};
		b.Set = 2; b.Binding = 0; b.Type = NVR_EXT_BIND_UNIFORM_BUFFER; b.Slot = 0;
		bindings.push_back(b);
	}
	for (UINT s = 0; s < Samplers.size(); s++) {
		NVR_EXT_BINDING_DESC b = {};
		b.Type = NVR_EXT_BIND_SAMPLER; b.Slot = s; b.PushOffset = s * 4;
		bindings.push_back(b);
	}
	// 32 bindings per pipeline up to extension 1.4; the 1.5-sm6 build (fork patch 0013, 0014 since the 10-27 renumbering) says 64 slots and takes 96
	const UINT maxBindings = DxvkCompute::Info().MaxSlots >= 64 ? 96u : 32u;
	if (bindings.size() > maxBindings) return Fail("more bindings than this Vegas Shaders DXVK takes");
	if (PushSize > DxvkCompute::Info().MaxPushBytes || PushSize < Samplers.size() * 4) return Fail("bad push size");
	if (!TargetCount || TargetCount > NVR_EXT_MAX_TARGETS) return Fail("bad target count");

	std::vector<char> words;
	if (!Sm6ReadFile((base + ".spv").c_str(), words) || words.size() < 20 || (words.size() & 3)) return Fail("no SPIR-V file");
	NVR_EXT_GRAPHICS_PIPELINE_DESC desc = {};
	desc.pFragmentSpirv = words.data();
	desc.FragmentSpirvSize = (UINT)words.size();
	desc.BindingCount = (UINT)bindings.size();
	desc.pBindings = bindings.data();
	desc.PushConstantSize = PushSize;
	desc.UsesSamplerHeap = TRUE;
	desc.SamplerHeapSet = 0;
	desc.SamplerHeapBinding = 0;
	desc.ColorTargetCount = TargetCount;
	desc.Blend = NVR_EXT_BLEND_NONE;
	desc.pDebugName = Key.c_str();
	HRESULT result = DxvkCompute::Extension()->CreateGraphicsPipeline(&desc, &Pipeline);
	if (FAILED(result) || !Pipeline) {
		Pipeline = nullptr;
		char why[96];
		_snprintf_s(why, sizeof(why), _TRUNCATE, "the pipeline was refused (%08lX; DXVK's log says why)", result);
		return Fail(why);
	}
	UboData.assign(UboSize, 0);
	int fromEffect = 0;
	for (const Constant& c : Constants) if (c.Handle) fromEffect++;
	int sampled = 0;
	for (const Texture& t : Textures) if (t.Stage >= 0) sampled++;
	Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s ready (variant %s %s): %u constants in %u bytes (%d from the effect), %u textures (%d on the D3D9 pass's stages), %u samplers, %u target(s)",
		Key.c_str(), hash, variant.c_str(), (UINT)Constants.size(), UboSize, fromEffect, (UINT)Textures.size(), sampled, (UINT)Samplers.size(), TargetCount);
	return true;
}

void Sm6Pass::FillConstants() {
	ID3DXEffect* effect = Owner->Effect;
	std::fill(UboData.begin(), UboData.end(), 0);
	for (const Constant& c : Constants) {
		if (c.Offset > UboData.size() || c.Size > UboData.size() - c.Offset) continue;	// (2026-11-17: was Offset + Size, which wraps: the layout file's numbers)
		unsigned char* dst = &UboData[c.Offset];
		auto over = VectorOverrides.find(c.Name);
		if (over != VectorOverrides.end()) { memcpy(dst, &over->second, min(c.Size, (UINT)sizeof(D3DXVECTOR4))); continue; }
		if (c.Handle) {
			const UINT n = max(1u, c.Desc.Elements);
			switch (c.Desc.Class) {
			case D3DXPC_VECTOR: {
				std::vector<D3DXVECTOR4> v(n);
				if (FAILED(c.Desc.Elements ? effect->GetVectorArray(c.Handle, v.data(), n) : effect->GetVector(c.Handle, v.data()))) break;	// (the Array getters refuse a non-array)
				for (UINT i = 0; i < n && i * 16 < c.Size; i++) memcpy(dst + i * 16, &v[i], min(16u, c.Size - i * 16));
				continue;
			}
			case D3DXPC_MATRIX_ROWS:
			case D3DXPC_MATRIX_COLUMNS: {
				// the logical matrix (as SetMatrix set it), row by row: the SM6 build packs matrices row_major
				std::vector<D3DXMATRIX> m(n);
				if (FAILED(c.Desc.Elements ? effect->GetMatrixArray(c.Handle, m.data(), n) : effect->GetMatrix(c.Handle, m.data()))) break;
				const UINT rows = c.Desc.Rows, cols = c.Desc.Columns, stride = rows * 16;
				for (UINT i = 0; i < n; i++)
					for (UINT r = 0; r < rows && r < 4; r++)
						if (i * stride + r * 16 + cols * 4 <= c.Size) memcpy(dst + i * stride + r * 16, &m[i].m[r][0], cols * 4);
				continue;
			}
			case D3DXPC_SCALAR: {
				const UINT stride = n > 1 ? 16 : 4;
				if (c.Desc.Type == D3DXPT_FLOAT) {
					std::vector<float> f(n);
					if (FAILED(c.Desc.Elements ? effect->GetFloatArray(c.Handle, f.data(), n) : effect->GetFloat(c.Handle, f.data()))) break;
					for (UINT i = 0; i < n && i * stride + 4 <= c.Size; i++) memcpy(dst + i * stride, &f[i], 4);
				}
				else {
					std::vector<INT> v(n);
					HRESULT got = c.Desc.Elements ? (c.Desc.Type == D3DXPT_BOOL ? effect->GetBoolArray(c.Handle, (BOOL*)v.data(), n) : effect->GetIntArray(c.Handle, v.data(), n))
						: (c.Desc.Type == D3DXPT_BOOL ? effect->GetBool(c.Handle, (BOOL*)v.data()) : effect->GetInt(c.Handle, v.data()));
					if (FAILED(got)) break;
					for (UINT i = 0; i < n && i * stride + 4 <= c.Size; i++) memcpy(dst + i * stride, &v[i], 4);
				}
				continue;
			}
			default: break;
			}
		}
		// a constant only the SM6 build declares: the registered one (contiguous, as SetCT would upload it), else its literal default
		auto found = TheShaderManager->ConstantsTable.find(c.Name);
		if (found != TheShaderManager->ConstantsTable.end() && found->second) SkyProjection::JoinIfSky(found->second);	// UNOFFICIAL 2026-11-03 SkyOnWorker
		if (found != TheShaderManager->ConstantsTable.end() && found->second) { memcpy(dst, found->second, c.Size); continue; }
		if (!c.Default.empty()) { memcpy(dst, c.Default.data(), min(c.Size, (UINT)(c.Default.size() * sizeof(float)))); continue; }
		if (std::find(Missing.begin(), Missing.end(), c.Name) == Missing.end()) {
			Missing.push_back(c.Name);
			Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s: constant %s has no value (not an effect parameter, not registered): 0", Key.c_str(), c.Name.c_str());
		}
	}
}

IDirect3DBaseTexture9* Sm6Pass::TextureFor(IDirect3DDevice9* Device, const Texture& T, IDirect3DBaseTexture9*& Held) {
	Held = nullptr;
	auto over = TextureOverrides.find(T.Name);
	if (over != TextureOverrides.end()) return over->second;
	if (T.Stage >= 0 && SUCCEEDED(Device->GetTexture(T.Stage, &Held)) && Held) return Held;
	for (UInt32 c = 0; c < Owner->TextureShaderValuesCount; c++) {
		ShaderTextureValue* sampler = &Owner->TextureShaderValues[c];
		if (!sampler->Name || T.Name != sampler->Name || !sampler->Texture) continue;
		if (sampler->Texture->TextureRef) sampler->Texture->Texture = *sampler->Texture->TextureRef;
		return sampler->Texture->Texture;
	}
	return nullptr;
}

bool Sm6Pass::Draw(IDirect3DDevice9* Device, IDirect3DSurface9* const* Targets, UINT Count, EffectRecord::ExtUv Uv, const RECT* Viewport) {
	struct Cleanup { Sm6Pass* p; std::vector<IDirect3DBaseTexture9*> held; ~Cleanup() { for (auto* t : held) if (t) t->Release(); p->TextureOverrides.clear(); p->VectorOverrides.clear(); } } cleanup{ this };
	if (!Ready() || !Owner || !Owner->Effect || !Device) return false;
	if (Count != TargetCount) return Fail("called with a different number of targets than the pass writes");

	FillConstants();
	ID3D9NvrExtBuffer* ubo = UboSize ? Owner->ExtUniforms(Ubo, UboData.data(), UboSize) : nullptr;
	if (UboSize && !ubo) return Fail("its uniform buffer could not be made");

	std::vector<NVR_EXT_RESOURCE_BINDING> resources;
	std::vector<IDirect3DBaseTexture9*> textures(Textures.size(), nullptr);
	cleanup.held.resize(Textures.size(), nullptr);
	for (size_t i = 0; i < Textures.size(); i++) textures[i] = TextureFor(Device, Textures[i], cleanup.held[i]);
	for (size_t i = 0; i < Textures.size(); i++) {
		IDirect3DBaseTexture9* texture = textures[i];
		// nothing bound (D3D9 would read an empty stage, the shader then multiplies the read by a flag of 0): any input of the same kind
		for (size_t j = 0; !texture && j < Textures.size(); j++) if (textures[j] && Textures[j].View == Textures[i].View) texture = textures[j];
		if (!texture) {
			static bool said = false;
			if (!said) Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s: no texture for %s this frame: D3D9 pass this frame", Key.c_str(), Textures[i].Name.c_str());
			said = true;
			return false;
		}
		NVR_EXT_RESOURCE_BINDING r = {};
		r.Slot = Textures[i].Binding; r.Type = NVR_EXT_BIND_SAMPLED_IMAGE; r.pResource = texture;
		resources.push_back(r);
	}
	if (ubo) {
		NVR_EXT_RESOURCE_BINDING r = {};
		r.Slot = 0; r.Type = NVR_EXT_BIND_UNIFORM_BUFFER; r.pResource = ubo;
		resources.push_back(r);
	}
	for (UINT s = 0; s < Samplers.size(); s++) {
		NVR_EXT_RESOURCE_BINDING r = {};
		r.Slot = s; r.Type = NVR_EXT_BIND_SAMPLER; r.pResource = Samplers[s];
		resources.push_back(r);
	}
	std::vector<UInt32> push(PushSize / 4, 0);	// the sampler heap indices: DXVK writes them
	const bool wasFailed = Owner->ExtFailed;
	const bool drawn = Owner->ExtDrawTo(Pipeline, Targets, Count, resources.data(), (UINT)resources.size(), push.data(), PushSize, Viewport, Uv);
	Owner->ExtFailed = wasFailed;	// (the effect's hand-ported passes are not this pass)
	if (!drawn) return Fail("the draw was refused");
	if (++Frames == 1) Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s runs from its SM6 build.", Key.c_str());
	if (Frames == 120) {	// once: what the pass was fed (constants' first four values, textures), for checking against the D3D9 pass
		std::string line;
		for (const Constant& c : Constants) {
			if (c.Size < 4 || c.Offset > UboData.size() || c.Size > UboData.size() - c.Offset) continue;	// (2026-11-17: as FillConstants)
			const float* v = (const float*)&UboData[c.Offset];
			char one[160];
			_snprintf_s(one, sizeof(one), _TRUNCATE, " %s=(%g %g %g %g)%s", c.Name.c_str(), v[0], c.Size > 4 ? v[1] : 0.0f, c.Size > 8 ? v[2] : 0.0f,
				c.Size > 12 ? v[3] : 0.0f, c.Handle ? "" : "*");
			line += one;
		}
		Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s frame 120 constants (* = not from the effect):%s", Key.c_str(), line.c_str());
		line.clear();
		for (size_t i = 0; i < Textures.size(); i++) {
			char one[160];
			_snprintf_s(one, sizeof(one), _TRUNCATE, " %s@s%d=%p", Textures[i].Name.c_str(), Textures[i].Stage, (void*)textures[i]);
			line += one;
		}
		Logger::Log("UNOFFICIAL Vegas Shaders SM6: %s frame 120 textures:%s", Key.c_str(), line.c_str());
	}
	return true;
}

// UNOFFICIAL 2026-10-26: EffectRecord's generic hook (EffectRecord.h). Nothing is looked up while NvrSm6Shaders is off or the fork is absent.
Sm6Pass* EffectRecord::Sm6For(UINT techniqueIndex, UINT pass) {
	if (!Effect || !Sm6Pass::On()) return nullptr;
	D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
	D3DXTECHNIQUE_DESC description = {};
	if (!technique || FAILED(Effect->GetTechniqueDesc(technique, &description))) return nullptr;
	const char* name = description.Name && *description.Name ? description.Name : "default";
	if (!Sm6Set) Sm6Set = new Sm6PassSet();
	Sm6Pass& sm6 = Sm6Set->Passes[std::string(name) + "." + std::to_string(pass)];
	return sm6.Prepare(this, name, pass) ? &sm6 : nullptr;
}

bool EffectRecord::Sm6Covers(UINT techniqueIndex) {
	if (!Effect || !Sm6Pass::On()) return false;
	D3DXTECHNIQUE_DESC description = {};
	D3DXHANDLE technique = Effect->GetTechnique(techniqueIndex);
	if (!technique || FAILED(Effect->GetTechniqueDesc(technique, &description)) || !description.Passes) return false;
	for (UINT p = 0; p < description.Passes; p++) {
		Sm6Pass* sm6 = Sm6For(techniqueIndex, p);
		if (!sm6 || !sm6->Plain()) return false;
	}
	return true;
}
