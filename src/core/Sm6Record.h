#pragma once

// UNOFFICIAL 2026-11-21 Sm6Record: the record mode of tools\nvr-sm6 (docs/deferred-single-pass.md step 1; the contract and the .layout format
// `nvr-sm6-record 1`: tools/nvr-sm6/README.md "Template mode"). A C++-driven ps_3_0 pass loaded as a ShaderRecord (DeferredSun, later the
// lamp passes) draws through the fork's full-screen Draw instead of its DrawPrimitiveUP, from the SPIR-V tpl2spv.py built from that record's
// own preprocessed cache text (Compute\Deferred\<Name>-<key>.spv + .layout, key = ShaderRecord::SourceKey). It is fed exactly what the D3D9
// draw would read at that moment:
//   constants: the device's pixel float registers c0..c(N-1), as they are, into one uniform buffer (the register file: cN at 16 N);
//   textures:  the device's texture on each stage the layout names, or a 1x1 stand-in where the stage is empty, holds the wrong kind, is one
//              of the Draw's targets, or the caller says it holds nothing this pass set (StaleStages);
//   samplers:  the extension sampler matching the device's sampler states on that stage (filter, address; sRGB on the texture binding).
// The caller keeps every check and setup it has; this replaces only its draw call, and the caller swaps RT0 (a Draw's target must not be the
// device's current render target). Nothing here calls DxvkCompute::Fail (which turns every Vulkan pass off): a Failed result is the caller's
// to latch, a Refused one is this frame only; either way the caller draws its D3D9 quad instead.
// (2026-11-21 depth attachment) A pass that writes depth may be drawn with a depth-stencil attachment and the D3D9 draw's tests: the
// hardware then tests the shader's SV_Depth as it tests the D3D9 draw's oDepth. That goes through the fork's DrawGeometryScene (1.7-grass)
// with this pipeline's own (built-in) full-screen triangle, NOT DrawDepthStencil (1.6): DrawDepthStencil sets the viewport's depth range to
// its one Depth value (patch 0010, depthMin = depthMax = Depth), and with DXVK's depth clamp a written FragDepth is clamped into that range,
// so every pixel would be tested at the same depth. DrawGeometryScene keeps the range 0-1 (patch 0024) and allows the device's own
// depth-stencil surface as the attachment.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "NvrDxvkExt.h"
#include "DxvkCompute.h"
// (not MixCheck.h: this header reaches Render.cpp through DeferredSun.h before its MIXCHECK_IMPLEMENTATION include, and #pragma once would
// leave the check's bodies uncompiled. NvrDxvkMixCheck sees this pass's reads through DxvkCompute::PreLoad, not its writes.)

class Sm6Record {
public:
	enum class State { NotTried, Missing, Ready, Failed };
	enum class Result { Drawn, Refused, Failed };

	State Status = State::NotTried;
	unsigned long long Key = 0;
	std::string File;					// Deferred\<Name>-<key> (under Compute\; logs, the pipelines' names)
	UINT Registers = 0, TargetCount = 1, PushBytes = 0, UvMapOffset = 0, ZTestOffset = 0;
	bool WritesDepth = false;
	bool Overwrite = false;	// (bench diagnosis) this Draw writes instead of blending (debug views)
	unsigned long long Frames = 0;		// Draws made

	// Looks for Compute\Deferred\<Name>-<RecordKey>.spv and .layout once; later calls return the state found. Missing: no build of this
	// text (quiet: the caller logs it once). Failed: Why says what (the layout, a pipeline, the uniform buffer). Ready: a pipeline for the
	// layout's target count and, when that is more than 1, one for the first target alone (a frame without the extra targets).
	State Prepare(const char* Name, unsigned long long RecordKey, NVR_EXT_BLEND Blend, const char*& Why) {
		Why = WhyText;
		if (Status != State::NotTried) return Status;
		Key = RecordKey;
		char file[128];
		_snprintf_s(file, sizeof(file), _TRUNCATE, "Deferred\\%s-%016llx", Name ? Name : "", RecordKey);
		File = file;
		const std::string base = std::string(Paths::ComputeDir()) + File;
		if (!RecordKey || !DxvkCompute::HasSpirv(File.c_str()) || GetFileAttributesA((base + ".layout").c_str()) == INVALID_FILE_ATTRIBUTES) {
			_snprintf_s(WhyText, sizeof(WhyText), _TRUNCATE, "no SPIR-V for key %016llx", RecordKey);
			return Status = State::Missing;
		}
		if (!DxvkCompute::Available() || DxvkCompute::Info().ExtVersionMinor < 1) return Bad("the DXVK fork's extension 1.1 (full-screen passes) is not available");
		if (!LoadLayout((base + ".layout").c_str())) return Status;

		std::vector<NVR_EXT_BINDING_DESC> bindings;
		for (const Texture& t : Textures) bindings.push_back({ t.Set, t.Binding, NVR_EXT_BIND_SAMPLED_IMAGE, t.Slot, t.View, FALSE, 0 });
		if (WritesDepth && ZRefUsed) bindings.push_back({ ZRef.Set, ZRef.Binding, NVR_EXT_BIND_SAMPLED_IMAGE, ZRef.Slot, NVR_EXT_VIEW_2D, FALSE, 0 });
		if (UboUsed) bindings.push_back({ Ubo.Set, Ubo.Binding, NVR_EXT_BIND_UNIFORM_BUFFER, Ubo.Slot, NVR_EXT_VIEW_2D, FALSE, 0 });
		for (const Sampler& s : Samplers) bindings.push_back({ HeapSet, HeapBinding, NVR_EXT_BIND_SAMPLER, s.Slot, NVR_EXT_VIEW_2D, FALSE, s.PushOffset });
		const UINT maxBindings = DxvkCompute::Info().MaxSlots >= 64 ? 96u : 32u;
		if (bindings.size() > maxBindings) return Bad("more bindings than this DXVK fork takes");

		for (UINT count = 1; count <= TargetCount; count++) {
			if (count != 1 && count != TargetCount) continue;
			Pipelines[count] = DxvkCompute::CreateGraphicsPipeline(File.c_str(), bindings.data(), (UINT)bindings.size(), PushBytes, !Samplers.empty(), count, Blend, false);
			if (!Pipelines[count]) return Bad("a pipeline was refused (DXVK's log says why)");
			// (bench diagnosis) the same pass writing over the target, for debug views compared with D3D9's (which write); compiled at first use only
			if (Blend != NVR_EXT_BLEND_NONE) WritePipelines[count] = DxvkCompute::CreateGraphicsPipeline(File.c_str(), bindings.data(), (UINT)bindings.size(), PushBytes, !Samplers.empty(), count, NVR_EXT_BLEND_NONE, false);
		}
		if (UboUsed) {
			Ubo.Buffer = DxvkCompute::CreateBufferNoFail(UboBytes, NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, File.c_str());
			if (!Ubo.Buffer) return Bad("its uniform buffer could not be made");
		}
		WhyText[0] = 0;
		return Status = State::Ready;
	}

	// One Draw into Targets (Count = the layout's target count, or 1 for the first target alone), viewport Viewport, push uvmap and ztest
	// (README "What the SPIR-V expects"), ZRef the depth-test reference texture. StaleStages: bit N = stage N holds nothing the pass set for
	// this draw (a stand-in is bound there). Depth: null = no depth attachment (ZTest's in-shader test, or none); else the depth-stencil
	// attachment and its tests (pResource an IDirect3DSurface9, single-sample, at least the targets' size; the header's "depth attachment":
	// drawn through DrawGeometryScene, the range 0-1). Refused: this frame keeps D3D9 (Why: a stage state the extension cannot match, or no
	// DrawGeometryScene in this d3d9.dll for a Depth draw). Failed: Why says what; the caller latches.
	Result Draw(IDirect3DDevice9* d, IDirect3DSurface9* const* Targets, UINT Count, const D3DVIEWPORT9& Viewport, const float UvMap[4], const float ZTest[4],
		IDirect3DBaseTexture9* ZRefTexture, const NVR_EXT_DEPTH_STENCIL* Depth, UINT StaleStages, const char*& Why) {
		struct Held { std::vector<IDirect3DBaseTexture9*> t; ~Held() { for (auto* x : t) if (x) x->Release(); } } held;
		Why = WhyText;
		if (Status != State::Ready || !d || !Targets || !Targets[0]) return Fail("called while not ready");
		if (Count > NVR_EXT_MAX_TARGETS || !Pipelines[Count] || (Count != 1 && Count != TargetCount)) return Refuse("asked for a target count it has no pipeline for");
		for (UINT i = 0; i < Count; i++) if (!Targets[i]) return Refuse("a target is missing");
		// the depth attachment: checked here so the extension never refuses it (a refused Draw latches the caller off for the session)
		IDirect3DSurface9* const depthSurface = Depth ? static_cast<IDirect3DSurface9*>(Depth->pResource) : nullptr;
		if (Depth) {
			if (!WritesDepth) return Refuse("a depth attachment for a pass that writes no depth");
			if (!depthSurface) return Refuse("a depth attachment without a surface");
			if (!SceneExt()) return Refuse("this d3d9.dll has no DrawGeometryScene (the fork's 1.7-grass) for a depth-attached pass");
			D3DSURFACE_DESC t0 = {}, ds = {};
			if (FAILED(Targets[0]->GetDesc(&t0)) || FAILED(depthSurface->GetDesc(&ds))) return Refuse("the target or depth-stencil surface could not be described");
			if (ds.MultiSampleType != D3DMULTISAMPLE_NONE) return Refuse("the depth-stencil surface is multisampled");
			if (ds.Width < t0.Width || ds.Height < t0.Height) return Refuse("the depth-stencil surface is smaller than the target");
		}
		if (!Dummy(d)) return Fail("its 1x1 stand-in texture could not be made");

		std::vector<NVR_EXT_RESOURCE_BINDING> resources;
		resources.reserve(Textures.size() + Samplers.size() + 2);
		UINT standIns = 0;	// stages given the stand-in: their sampler is POINT / CLAMP, whatever states the stage holds
		for (const Texture& t : Textures) {
			IDirect3DBaseTexture9* texture = nullptr;
			if (!(StaleStages & (1u << t.Stage)) && SUCCEEDED(d->GetTexture(t.Stage, &texture)) && texture) held.t.push_back(texture);
			IDirect3DBaseTexture9* use = texture;
			if (use) {
				const D3DRESOURCETYPE type = use->GetType();
				const bool fits = (type == D3DRTYPE_TEXTURE && t.View == NVR_EXT_VIEW_2D) || (type == D3DRTYPE_CUBETEXTURE && t.View == NVR_EXT_VIEW_CUBE) ||
					(type == D3DRTYPE_VOLUMETEXTURE && t.View == NVR_EXT_VIEW_3D);
				if (!fits || IsTarget(use, Targets, Count)) use = nullptr;
				else if (depthSurface && IsTarget(use, &depthSurface, 1)) return Refuse("a stage holds the depth-stencil surface the Draw attaches");
			}
			DWORD srgb = FALSE;
			if (!use) {
				if (t.View != NVR_EXT_VIEW_2D) return Refuse("a cube or volume stage needs a stand-in (only a 2D one exists)");
				use = DummyTexture;
				standIns |= 1u << t.Stage;
			}
			else d->GetSamplerState(t.Stage, D3DSAMP_SRGBTEXTURE, &srgb);
			NVR_EXT_RESOURCE_BINDING r = {};
			r.Slot = t.Slot; r.Type = NVR_EXT_BIND_SAMPLED_IMAGE; r.pResource = use; r.Srgb = srgb ? TRUE : FALSE;
			resources.push_back(r);
		}
		if (WritesDepth && ZRefUsed) {
			if (!ZRefTexture) return Refuse("no depth reference texture");
			if (IsTarget(ZRefTexture, Targets, Count)) return Refuse("the depth reference is a target");
			if (depthSurface && IsTarget(ZRefTexture, &depthSurface, 1)) return Refuse("the depth reference is the attached depth-stencil surface");
			NVR_EXT_RESOURCE_BINDING r = {};
			r.Slot = ZRef.Slot; r.Type = NVR_EXT_BIND_SAMPLED_IMAGE; r.pResource = ZRefTexture;
			resources.push_back(r);
		}
		for (const Sampler& s : Samplers) {
			NVR_EXT_FILTER filter = NVR_EXT_FILTER_NEAREST; NVR_EXT_ADDRESS address = NVR_EXT_ADDRESS_CLAMP;
			const char* refused = (standIns & (1u << s.Stage)) ? nullptr : DeviceSampler(d, s.Stage, filter, address);
			if (refused) return Refuse(refused);
			ID3D9NvrExtSampler* sampler = SamplerFor(filter, address);
			if (!sampler) return Fail("a sampler could not be made");
			NVR_EXT_RESOURCE_BINDING r = {};
			r.Slot = s.Slot; r.Type = NVR_EXT_BIND_SAMPLER; r.pResource = sampler;
			resources.push_back(r);
		}
		if (UboUsed) {
			if (FAILED(d->GetPixelShaderConstantF(0, &Constants[0][0], Registers))) return Fail("the pixel constants could not be read");
			const HRESULT updated = DxvkCompute::Extension()->UpdateBuffer(Ubo.Buffer, 0, UboBytes, &Constants[0][0]);
			if (FAILED(updated)) return Fail("its uniform buffer could not be written");
			NVR_EXT_RESOURCE_BINDING r = {};
			r.Slot = Ubo.Slot; r.Type = NVR_EXT_BIND_UNIFORM_BUFFER; r.pResource = Ubo.Buffer;
			resources.push_back(r);
		}

		// (2026-11-21 bench diagnosis) the first Draw: what each texture stage got and the sampler it maps to, once per record
		if (Frames == 0) {
			std::string line;
			for (const Texture& t : Textures) {
				IDirect3DBaseTexture9* tex = nullptr; D3DFORMAT fmt = D3DFMT_UNKNOWN; DWORD levels = 0, srgb = 0;
				if (SUCCEEDED(d->GetTexture(t.Stage, &tex)) && tex) {
					levels = tex->GetLevelCount();
					if (tex->GetType() == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC desc = {}; if (SUCCEEDED(((IDirect3DTexture9*)tex)->GetLevelDesc(0, &desc))) fmt = desc.Format; }
					tex->Release();
				}
				d->GetSamplerState(t.Stage, D3DSAMP_SRGBTEXTURE, &srgb);
				NVR_EXT_FILTER f = NVR_EXT_FILTER_NEAREST; NVR_EXT_ADDRESS a = NVR_EXT_ADDRESS_CLAMP;
				const char* refused = DeviceSampler(d, t.Stage, f, a);
				char one[160];
				_snprintf_s(one, sizeof(one), _TRUNCATE, "%ss%u %s fmt %d levels %lu srgb %lu filter %d address %d%s%s", line.empty() ? "" : "; ", t.Stage,
					(standIns & (1u << t.Stage)) ? "STAND-IN" : "bound", (int)fmt, levels, srgb, (int)f, (int)a, refused ? " refused: " : "", refused ? refused : "");
				line += one;
			}
			Logger::Log("UNOFFICIAL Sm6Record %s first Draw: %s", File.c_str(), line.c_str());
		}
		UInt32 push[NVR_EXT_MAX_PUSH_BYTES / 4] = {};	// the sampler heap indices: the extension writes them
		memcpy((char*)push + UvMapOffset, UvMap, 16);
		if (WritesDepth) memcpy((char*)push + ZTestOffset, ZTest, 16);

		NVR_EXT_DRAW_DESC draw = {};
		draw.pPipeline = Overwrite && WritePipelines[Count] ? WritePipelines[Count] : Pipelines[Count];
		draw.TargetCount = Count;
		for (UINT i = 0; i < Count; i++) draw.Targets[i].pResource = Targets[i];
		draw.ViewportX = (INT)Viewport.X; draw.ViewportY = (INT)Viewport.Y; draw.ViewportWidth = Viewport.Width; draw.ViewportHeight = Viewport.Height;
		draw.BindingCount = (UINT)resources.size();
		draw.pBindings = resources.data();
		draw.pPushData = push;
		draw.PushDataSize = PushBytes;
		DxvkCompute::PreLoad(resources.data(), (UINT)resources.size());	// (the managed stand-in on the GPU; MixCheck's reads)
		HRESULT result;
		if (Depth) {
			// the same pipeline (its built-in full-screen triangle: 3 vertices, depth 0, replaced by the shader's SV_Depth) as a scene pass:
			// the attachment's tests, viewport depth range 0-1, no depth bias, depth bounds 0-1, single-sample, no culling
			NVR_EXT_GEOMETRY_SCENE_DESC scene = {};
			scene.Geometry.Draw = draw;
			scene.Geometry.VertexCount = 3;
			scene.Geometry.InstanceCount = 1;
			scene.pDepthStencil = Depth;
			scene.CullMode = NVR_EXT_CULL_NONE;
			scene.Multisample = FALSE;
			result = SceneExt()->DrawGeometryScene(&scene);
		}
		else result = DxvkCompute::Extension()->Draw(&draw);
		if (FAILED(result)) {
			_snprintf_s(WhyText, sizeof(WhyText), _TRUNCATE, "the %s was refused (%08lX; DXVK's log says why)", Depth ? "depth-attached Draw (DrawGeometryScene)" : "Draw", result);
			return Result::Failed;
		}
		Frames++;
		return Result::Drawn;
	}

private:
	struct Texture { UINT Stage = 0, Set = 0, Binding = 0, Slot = 0; NVR_EXT_VIEW_TYPE View = NVR_EXT_VIEW_2D; };
	struct Sampler { UINT Stage = 0, Slot = 0, PushOffset = 0; };
	struct Bound { UINT Set = 0, Binding = 0, Slot = 0; ID3D9NvrExtBuffer* Buffer = nullptr; };
	std::vector<Texture> Textures;		// the used stages only
	std::vector<Sampler> Samplers;
	Bound Ubo, ZRef;
	bool UboUsed = false, ZRefUsed = false;
	UINT UboBytes = 0, HeapSet = 0, HeapBinding = 0;
	ID3D9NvrExtPipeline* Pipelines[NVR_EXT_MAX_TARGETS + 1] = {};
	ID3D9NvrExtPipeline* WritePipelines[NVR_EXT_MAX_TARGETS + 1] = {};	// (bench diagnosis) no blend
	float Constants[224][4] = {};
	char WhyText[192] = "";

	State Bad(const char* why) {
		_snprintf_s(WhyText, sizeof(WhyText), _TRUNCATE, "%s (%s)", why, File.c_str());
		for (auto*& p : Pipelines) DxvkCompute::Drop(p);
		for (auto*& p : WritePipelines) DxvkCompute::Drop(p);
		DxvkCompute::Drop(Ubo.Buffer);
		return Status = State::Failed;
	}
	Result Fail(const char* why) { _snprintf_s(WhyText, sizeof(WhyText), _TRUNCATE, "%s", why); return Result::Failed; }
	Result Refuse(const char* why) { _snprintf_s(WhyText, sizeof(WhyText), _TRUNCATE, "%s", why); return Result::Refused; }

	static NVR_EXT_VIEW_TYPE ViewOf(const std::string& v, bool& ok) {
		ok = true;
		if (v == "2D") return NVR_EXT_VIEW_2D;
		if (v == "3D") return NVR_EXT_VIEW_3D;
		if (v == "CUBE") return NVR_EXT_VIEW_CUBE;
		ok = false;
		return NVR_EXT_VIEW_2D;
	}

	// The layout (README "The .layout format"): what the pipeline declares and the Draw binds. Only what the Draw needs is kept: the
	// const / group / pass / from lines describe the register file, which is uploaded whole.
	bool LoadLayout(const char* path) {
		std::ifstream file(path);
		if (!file) { Bad("its layout could not be read"); return false; }
		std::string line;
		bool first = true, uboSeen = false, heapSeen = false, uvSeen = false, ztestSeen = false, zrefSeen = false, depthSeen = false, targetsSeen = false;
		const UINT maxSlots = DxvkCompute::Info().MaxSlots ? DxvkCompute::Info().MaxSlots : 32u;
		while (std::getline(file, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::istringstream in(line);
			std::string tag;
			if (!(in >> tag) || tag[0] == '#') continue;
			if (first) {
				first = false;
				int version = 0;
				if (tag != "nvr-sm6-record" || !(in >> version) || version != 1) { Bad("its layout is not 'nvr-sm6-record 1'"); return false; }
				continue;
			}
			bool ok = true;
			if (tag == "key") { std::string hex; ok = (bool)(in >> hex); if (ok && strtoull(hex.c_str(), nullptr, 16) != Key) { Bad("its layout's key differs from the file name's"); return false; } }
			else if (tag == "registers") ok = (bool)(in >> Registers);
			else if (tag == "ubo") { std::string used; ok = (bool)(in >> Ubo.Set >> Ubo.Binding >> Ubo.Slot >> UboBytes >> used); UboUsed = used == "used"; uboSeen = ok; }
			else if (tag == "heap") { ok = (bool)(in >> HeapSet >> HeapBinding); heapSeen = ok; }
			else if (tag == "push") ok = (bool)(in >> PushBytes);
			else if (tag == "uvmap") { ok = (bool)(in >> UvMapOffset); uvSeen = ok; }
			else if (tag == "ztest") { ok = (bool)(in >> ZTestOffset); ztestSeen = ok; }
			else if (tag == "texture") {
				Texture t; std::string view, used;
				ok = (bool)(in >> t.Stage >> t.Set >> t.Binding >> t.Slot >> view >> used);
				bool viewOk = false;
				t.View = ViewOf(view, viewOk);
				ok = ok && viewOk && t.Stage < 16 && t.Slot < maxSlots;
				if (ok && used == "used") Textures.push_back(t);
			}
			else if (tag == "sampler") {
				Sampler s;
				ok = (bool)(in >> s.Stage >> s.Slot >> s.PushOffset) && s.Stage < 16 && s.Slot < maxSlots && (s.PushOffset & 3) == 0;
				if (ok) Samplers.push_back(s);
			}
			else if (tag == "zref") {
				std::string view, used;
				ok = (bool)(in >> ZRef.Set >> ZRef.Binding >> ZRef.Slot >> view >> used) && view == "2D" && ZRef.Slot < maxSlots;
				ZRefUsed = used == "used"; zrefSeen = ok;
			}
			else if (tag == "targets") { ok = (bool)(in >> TargetCount); targetsSeen = ok; }
			else if (tag == "depth") { int v = 0; ok = (bool)(in >> v); WritesDepth = v != 0; depthSeen = ok; }
			if (!ok) {
				_snprintf_s(WhyText, sizeof(WhyText), _TRUNCATE, "its layout has a bad '%s' line", tag.c_str());
				const std::string why = WhyText;
				Bad(why.c_str());
				return false;
			}
		}
		const char* bad = nullptr;
		if (first) bad = "its layout is empty";
		else if (!Registers || Registers > 224) bad = "its layout's register count is not 1-224";
		else if (!uboSeen || (UboUsed && UboBytes != 16 * Registers)) bad = "its layout's uniform buffer is not the register file (16 x registers bytes)";
		else if (!Samplers.empty() && (!heapSeen || HeapSet != 0 || HeapBinding != 0)) bad = "its layout's sampler heap is not set 0 binding 0";
		else if (!PushBytes || (PushBytes & 3) || PushBytes > DxvkCompute::Info().MaxPushBytes || PushBytes > NVR_EXT_MAX_PUSH_BYTES) bad = "its layout's push size does not fit";
		else if (!uvSeen || UvMapOffset + 16 > PushBytes) bad = "its layout has no uvmap in the push block";
		else if (!depthSeen || !targetsSeen || !TargetCount || TargetCount > NVR_EXT_MAX_TARGETS) bad = "its layout has no or a bad targets / depth line";
		else if (WritesDepth && (!ztestSeen || ZTestOffset + 16 > PushBytes || !zrefSeen)) bad = "its layout writes depth without a ztest push / zref texture";
		for (const Sampler& s : Samplers) if (!bad && s.PushOffset + 4 > PushBytes) bad = "a sampler's heap index lies outside the push block";
		if (bad) { Bad(bad); return false; }
		return true;
	}

	// A stage's texture is one of the Draw's targets (a target must not also be an input): its first level is one of them.
	static bool IsTarget(IDirect3DBaseTexture9* texture, IDirect3DSurface9* const* targets, UINT count) {
		if (!texture || texture->GetType() != D3DRTYPE_TEXTURE) return false;
		IDirect3DSurface9* level = nullptr;
		if (FAILED(static_cast<IDirect3DTexture9*>(texture)->GetSurfaceLevel(0, &level)) || !level) return false;
		bool hit = false;
		for (UINT i = 0; i < count; i++) hit = hit || level == targets[i];
		level->Release();
		return hit;
	}

	// The extension sampler for a stage's device states (NVR_EXT_SAMPLER_DESC: one filter for min and mag, one address for U, V and W). Null =
	// matched; else what the extension cannot match (this frame keeps D3D9).
	static const char* DeviceSampler(IDirect3DDevice9* d, UINT stage, NVR_EXT_FILTER& filter, NVR_EXT_ADDRESS& address) {
		DWORD minF = D3DTEXF_POINT, magF = D3DTEXF_POINT, mipF = D3DTEXF_NONE, u = D3DTADDRESS_WRAP, v = D3DTADDRESS_WRAP, border = 0;
		d->GetSamplerState(stage, D3DSAMP_MINFILTER, &minF); d->GetSamplerState(stage, D3DSAMP_MAGFILTER, &magF); d->GetSamplerState(stage, D3DSAMP_MIPFILTER, &mipF);
		d->GetSamplerState(stage, D3DSAMP_ADDRESSU, &u); d->GetSamplerState(stage, D3DSAMP_ADDRESSV, &v); d->GetSamplerState(stage, D3DSAMP_BORDERCOLOR, &border);
		if (u != v) return "a stage's U and V addressing differ";
		switch (u) {
		case D3DTADDRESS_CLAMP: address = NVR_EXT_ADDRESS_CLAMP; break;
		case D3DTADDRESS_WRAP: address = NVR_EXT_ADDRESS_WRAP; break;
		case D3DTADDRESS_MIRROR: address = NVR_EXT_ADDRESS_MIRROR; break;
		case D3DTADDRESS_BORDER: if (border != 0) return "a stage's border colour is not transparent black"; address = NVR_EXT_ADDRESS_BORDER; break;
		default: return "a stage uses MIRRORONCE addressing";
		}
		const bool anisotropic = minF == D3DTEXF_ANISOTROPIC || magF == D3DTEXF_ANISOTROPIC;
		const bool linear = anisotropic || minF >= D3DTEXF_LINEAR || magF >= D3DTEXF_LINEAR;
		filter = anisotropic ? NVR_EXT_FILTER_ANISOTROPIC : linear ? (mipF != D3DTEXF_NONE ? NVR_EXT_FILTER_LINEAR_MIPMAPS : NVR_EXT_FILTER_LINEAR) : NVR_EXT_FILTER_NEAREST;
		if (filter >= NVR_EXT_FILTER_LINEAR_MIPMAPS && DxvkCompute::Info().ExtVersionMinor < 3) filter = NVR_EXT_FILTER_LINEAR;	// (mips since 1.3)
		return nullptr;
	}

	// One extension sampler per state for every record pass (made on first use through the no-Fail path, kept for the session).
	static ID3D9NvrExtSampler* SamplerFor(NVR_EXT_FILTER filter, NVR_EXT_ADDRESS address) {
		static ID3D9NvrExtSampler* samplers[4][4] = {};
		if ((UINT)filter > 3 || (UINT)address > 3) return nullptr;
		ID3D9NvrExtSampler*& s = samplers[filter][address];
		if (!s) s = DxvkCompute::CreateSamplerNoFail(filter, address);
		return s;
	}

	// The fork's scene passes (1.7-grass DrawGeometryScene), queried once; null on a d3d9.dll without them. Kept for the session (the
	// extension's reference count is the device's, as GpuGrass keeps its own).
	static inline ID3D9NvrExtGeometryScene* Scene = nullptr;
	static inline bool SceneTried = false;
	static ID3D9NvrExtGeometryScene* SceneExt() {
		if (!SceneTried) {
			SceneTried = true;
			ID3D9NvrExt* ext = DxvkCompute::Extension();
			if (!ext || FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtGeometryScene), (void**)&Scene))) Scene = nullptr;
		}
		return Scene;
	}

	// The 1x1 stand-in (zero) for a stage with nothing usable: MANAGED, so a device reset leaves it (DxvkCompute::PreLoad uploads it).
	static inline IDirect3DTexture9* DummyTexture = nullptr;
	static inline bool DummyFailed = false;
	static bool Dummy(IDirect3DDevice9* d) {
		if (DummyTexture || DummyFailed) return DummyTexture != nullptr;
		if (FAILED(d->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &DummyTexture, NULL)) || !DummyTexture) {
			DummyTexture = nullptr; DummyFailed = true;
			return false;
		}
		D3DLOCKED_RECT lock;
		if (SUCCEEDED(DummyTexture->LockRect(0, &lock, NULL, 0))) { *(UInt32*)lock.pBits = 0; DummyTexture->UnlockRect(0); }
		return true;
	}
};
