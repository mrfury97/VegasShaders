#pragma once

// UNOFFICIAL 2026-10-17, TEST TOOLS ONLY (evidence orthomap-LOG.md): a one-shot dump of the top-down ortho map with the caster that
// wrote each texel. Triggered by [Shaders.ShadowsExteriors.Ortho] ProbeDump (a new non-zero value = one dump at the next ortho draw).
// That draw runs with a second render target and a pixel shader that writes the same depth as ShadowMap(Opaque).pso's ortho branch
// (alpha test included, as there) plus the caster's number, so the map itself is the real one. Written next to the evidence:
// ortho.f32 (the map, row-major floats), ids.f32 (caster number per texel; 1 = nothing drawn), depth.f32 (NVR's TESR_DepthBuffer as it
// is when the shadow maps are drawn: the previous frame), meta.txt (matrices, camera, sizes) and casters.txt (one line per caster).
// (2026-11-09: written into the game folder's VegasShaders-Logs\OrthoDump\<value>_<tick>\, not into an absolute path of the author's PC.)
#include "TestTools.h"
#if NVR_TEST_TOOLS
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include "Paths.h"	// VS_LOGS_FOLDER

namespace OrthoProbe {
	struct Entry { NiGeometry* geo; const void* pass; };
	inline bool Recording = false;							// the ortho map's caster walk is being recorded
	inline TESObjectREFR* CurrentRef = nullptr;				// the reference whose node AccumChildren walks (null for land / LOD)
	inline bool CurrentLand = false;
	inline std::unordered_map<NiGeometry*, std::pair<TESObjectREFR*, bool>> Owners;	// geometry -> (ref, land) during the walk
	inline std::vector<Entry> Drawn;						// in draw order: number = index + 2
	inline IDirect3DPixelShader9* IdShader = nullptr;
	inline IDirect3DTexture9* IdTexture = nullptr;
	inline IDirect3DSurface9* IdSurface = nullptr;
	inline int LastValue = 0;
	inline int LandCount[16] = {};	// land walk diagnostics while recording (ShadowManager: OP_LAND)
	#define OP_LAND(i) do { if (OrthoProbe::Recording && OrthoProbe::CurrentLand) OrthoProbe::LandCount[i]++; } while (0)
	inline std::string PendingDir;	// set by Dump: AfterCombine writes this frame's depth there (depth_effect.f32)

	inline void NoteGeometry(NiGeometry* geo) {
		if (Recording) Owners[geo] = std::make_pair(CurrentRef, CurrentLand);
	}

	// RenderPass::RenderAccum, after the pass set its constants: the ID shader and this caster's number.
	inline void OnDraw(const void* pass, NiGeometry* geo) {
		if (!Recording || !IdShader) return;
		TheRenderManager->renderState->SetPixelShader(IdShader, false);
		const float id[4] = { (float)(Drawn.size() + 2), 0.0f, 0.0f, 0.0f };
		TheRenderManager->device->SetPixelShaderConstantF(2, id, 1);
		Drawn.push_back({ geo, pass });
	}

	inline bool Prepare(IDirect3DDevice9* device, UINT size) {
		if (!IdShader) {
			static const char* src =
				"float4 Data : register(c0);\n"
				"float4 Id : register(c2);\n"
				"sampler2D DiffuseMap : register(s0);\n"
				"struct I { float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; };\n"
				"struct O { float4 c0 : COLOR0; float4 c1 : COLOR1; };\n"
				"O main(I IN) { O o; if (Data.y == 1.0f) { if (tex2D(DiffuseMap, IN.t1.xy).a < 0.5f) discard; }\n"
				"  o.c0 = float4(IN.t0.z / IN.t0.w, 0.0f, 0.0f, 1.0f); o.c1 = Id; return o; }\n";
			ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
			HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr);
			if (FAILED(hr) || !code) {
				Logger::Log("OrthoProbe: ID shader did not compile: %s", errors ? (const char*)errors->GetBufferPointer() : "?");
				if (errors) errors->Release();
				return false;
			}
			device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &IdShader);
			code->Release(); if (errors) errors->Release();
		}
		if (IdSurface) {
			D3DSURFACE_DESC desc; IdSurface->GetDesc(&desc);
			if (desc.Width != size) { IdSurface->Release(); IdTexture->Release(); IdSurface = nullptr; IdTexture = nullptr; }
		}
		if (!IdTexture && SUCCEEDED(device->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &IdTexture, NULL)))
			IdTexture->GetSurfaceLevel(0, &IdSurface);
		return IdShader && IdSurface;
	}

	// Reads a float render target (R32F or G32R32F: first channel) into rows of floats.
	inline bool ReadFloats(IDirect3DDevice9* device, IDirect3DSurface9* source, std::vector<float>& out, UINT& w, UINT& h, D3DFORMAT& format) {
		D3DSURFACE_DESC desc; if (!source || FAILED(source->GetDesc(&desc))) return false;
		w = desc.Width; h = desc.Height; format = desc.Format;
		IDirect3DSurface9* sys = nullptr;
		if (FAILED(device->CreateOffscreenPlainSurface(w, h, desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL))) return false;
		bool ok = SUCCEEDED(device->GetRenderTargetData(source, sys));
		D3DLOCKED_RECT r;
		if (ok && SUCCEEDED(sys->LockRect(&r, NULL, D3DLOCK_READONLY))) {
			const UINT stride = desc.Format == D3DFMT_G32R32F ? 2 : desc.Format == D3DFMT_A32B32G32R32F ? 4 : 1;
			out.resize((size_t)w * h);
			for (UINT y = 0; y < h; y++) {
				const float* row = (const float*)((const char*)r.pBits + (size_t)y * r.Pitch);
				for (UINT x = 0; x < w; x++) out[(size_t)y * w + x] = row[x * stride];
			}
			sys->UnlockRect();
		}
		else ok = false;
		sys->Release();
		return ok;
	}

	inline void WriteFloats(const std::string& path, const std::vector<float>& data) {
		FILE* f = fopen(path.c_str(), "wb");
		if (f) { fwrite(data.data(), sizeof(float), data.size(), f); fclose(f); }
	}

	inline void WriteMatrix(FILE* f, const char* name, const D3DXMATRIX& m) {
		fprintf(f, "%s", name);
		for (int i = 0; i < 16; i++) fprintf(f, " %.9g", ((const float*)&m)[i]);
		fprintf(f, "\n");
	}

	inline const char* PassName(const void* pass) {
		ShadowManager* m = TheShadowManager;
		if (pass == (const void*)m->geometryPass) return "geometry";
		if (pass == (const void*)m->alphaPass) return "alpha";
		if (pass == (const void*)m->skinnedGeoPass) return "skinned";
		if (pass == (const void*)m->speedTreePass) return "speedtree";
		if (pass == (const void*)m->terrainLODPass) return "terrainLOD";
		return "other";
	}

	inline std::string Chain(NiAVObject* o) {
		std::string s;
		for (int depth = 0; o && depth < 12; depth++, o = o->m_parent) {
			if (!s.empty()) s += " < ";
			s += o->m_pcName ? o->m_pcName : "(null)";
		}
		for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
		return s;
	}

	inline void Dump(IDirect3DDevice9* device, IDirect3DSurface9* orthoSurface, const D3DXMATRIX& orthoMatrix, int value) {
		char dir[MAX_PATH];
		// 2026-11-09: the folder was the author's own evidence folder (an absolute path of one PC, built into every release DLL); it is the game folder's
		// VegasShaders-Logs\OrthoDump now, like the other test tools' files.
		CreateDirectoryA(".\\" VS_LOGS_FOLDER, NULL);
		CreateDirectoryA(".\\" VS_LOGS_FOLDER "\\OrthoDump", NULL);
		snprintf(dir, sizeof(dir), ".\\" VS_LOGS_FOLDER "\\OrthoDump\\%d_%lu", value, GetTickCount());
		CreateDirectoryA(dir, NULL);
		std::string base(dir); base += "\\";
		std::vector<float> data; UINT w = 0, h = 0; D3DFORMAT format = D3DFMT_UNKNOWN;
		FILE* meta = fopen((base + "meta.txt").c_str(), "w");
		if (!meta) { Logger::Log("OrthoProbe: cannot write %s", dir); return; }
		if (ReadFloats(device, orthoSurface, data, w, h, format)) { WriteFloats(base + "ortho.f32", data); fprintf(meta, "ortho %u %u %d\n", w, h, (int)format); }
		if (ReadFloats(device, IdSurface, data, w, h, format)) { WriteFloats(base + "ids.f32", data); fprintf(meta, "ids %u %u %d\n", w, h, (int)format); }
		IDirect3DTexture9* depthTexture = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
		IDirect3DSurface9* depthSurface = nullptr;
		if (depthTexture && SUCCEEDED(depthTexture->GetSurfaceLevel(0, &depthSurface))) {
			if (ReadFloats(device, depthSurface, data, w, h, format)) { WriteFloats(base + "depth.f32", data); fprintf(meta, "depth %u %u %d\n", w, h, (int)format); }
			depthSurface->Release();
		}
		WriteMatrix(meta, "ortho_matrix", orthoMatrix);
		WriteMatrix(meta, "view", TheRenderManager->ViewMatrix);
		WriteMatrix(meta, "inv_view", TheRenderManager->InvViewMatrix);
		WriteMatrix(meta, "proj", TheRenderManager->projMatrix);
		WriteMatrix(meta, "inv_proj", TheRenderManager->InvProjMatrix);
		WriteMatrix(meta, "inv_view_proj", TheRenderManager->InvViewProjMatrix);
		const D3DXVECTOR4& cp = TheRenderManager->CameraPosition, & cd = TheRenderManager->CameraData;
		fprintf(meta, "camera_position %.6f %.6f %.6f\ncamera_data %.6f %.6f %.6f %.6f\n", cp.x, cp.y, cp.z, cd.x, cd.y, cd.z, cd.w);
		const NiPoint3& sc = WorldSceneGraph->camera->m_worldTransform.pos;
		fprintf(meta, "scene_camera %.6f %.6f %.6f\nplayer %.6f %.6f %.6f\n", sc.x, sc.y, sc.z, Player->pos.x, Player->pos.y, Player->pos.z);
		fprintf(meta, "casters %u\n", (unsigned)Drawn.size());
		fprintf(meta, "land_walk");
		for (int i = 0; i < 16; i++) fprintf(meta, " %d", LandCount[i]);
		fprintf(meta, "\n");
		fclose(meta);
		FILE* c = fopen((base + "casters.txt").c_str(), "w");
		if (c) {
			fprintf(c, "id\tpass\tland\tref\tbase\tshader\tflags0\tflags1\talpha\tcx\tcy\tcz\tradius\tchain\n");
			for (size_t i = 0; i < Drawn.size(); i++) {
				NiGeometry* g = Drawn[i].geo;
				auto owner = Owners.find(g);
				TESObjectREFR* ref = owner != Owners.end() ? owner->second.first : nullptr;
				const bool land = owner != Owners.end() && owner->second.second;
				BSShaderProperty* sp = (BSShaderProperty*)g->GetProperty(NiProperty::kType_Shade);
				NiAlphaProperty* ap = (NiAlphaProperty*)g->GetProperty(NiProperty::PropertyType::kType_Alpha);
				NiBound* b = g->m_kWorldBound;
				fprintf(c, "%u\t%s\t%d\t%08X\t%08X\t%u\t%08X\t%08X\t%04X\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n", (unsigned)(i + 2), PassName(Drawn[i].pass), land ? 1 : 0,
					ref ? ref->refID : 0u, ref && ref->baseForm ? ref->baseForm->refID : 0u, sp ? (unsigned)sp->uiShaderIndex : 0u,
					sp ? sp->ulFlags[0] : 0u, sp ? sp->ulFlags[1] : 0u, ap ? (unsigned)ap->flags : 0xFFFFu,
					b ? b->Center.x : 0.0f, b ? b->Center.y : 0.0f, b ? b->Center.z : 0.0f, b ? b->Radius : 0.0f, Chain(g).c_str());
			}
			fclose(c);
		}
		Logger::Log("OrthoProbe: dump %d written to %s (%u casters)", value, dir, (unsigned)Drawn.size());
		PendingDir = base;
	}

	// ShaderManager, right after CombineDepth made TESR_DepthBuffer for this frame's effects: the depth the effects read (the dump's
	// depth.f32 is the previous frame's, read when the shadow maps are drawn), with this moment's matrices.
	inline void AfterCombine(IDirect3DDevice9* device) {
		if (PendingDir.empty()) return;
		const std::string base = PendingDir; PendingDir.clear();
		IDirect3DTexture9* depthTexture = TheShaderManager->Effects.CombineDepth ? TheShaderManager->Effects.CombineDepth->Textures.CombinedDepthTexture : nullptr;
		IDirect3DSurface9* depthSurface = nullptr;
		std::vector<float> data; UINT w = 0, h = 0; D3DFORMAT format = D3DFMT_UNKNOWN;
		FILE* meta = fopen((base + "meta_effect.txt").c_str(), "w");
		if (!meta) return;
		if (depthTexture && SUCCEEDED(depthTexture->GetSurfaceLevel(0, &depthSurface))) {
			if (ReadFloats(device, depthSurface, data, w, h, format)) { WriteFloats(base + "depth_effect.f32", data); fprintf(meta, "depth %u %u %d\n", w, h, (int)format); }
			depthSurface->Release();
		}
		WriteMatrix(meta, "view", TheRenderManager->ViewMatrix);
		WriteMatrix(meta, "inv_view", TheRenderManager->InvViewMatrix);
		WriteMatrix(meta, "proj", TheRenderManager->projMatrix);
		WriteMatrix(meta, "inv_proj", TheRenderManager->InvProjMatrix);
		const D3DXVECTOR4& cp = TheRenderManager->CameraPosition, & cd = TheRenderManager->CameraData;
		fprintf(meta, "camera_position %.6f %.6f %.6f\ncamera_data %.6f %.6f %.6f %.6f\n", cp.x, cp.y, cp.z, cd.x, cd.y, cd.z, cd.w);
		const NiPoint3& sc = WorldSceneGraph->camera->m_worldTransform.pos;
		fprintf(meta, "scene_camera %.6f %.6f %.6f\n", sc.x, sc.y, sc.z);
		fclose(meta);
		Logger::Log("OrthoProbe: effect-time depth written to %s", base.c_str());
	}
}
#endif
