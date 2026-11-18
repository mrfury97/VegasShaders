#pragma once

// UNOFFICIAL 2026-11-14 DeferredDepth (docs/next/FULL-DEFERRED-RENDERER-PLAN.md section 4.3 and phase 0 question 2; results and the bench
// request: docs/next/FD-DEPTH-RESULTS.md). The deferred light passes run at the main opaque list's end (GrassFirst::MainListHook's
// DeferredSpan, Hooks/Render.cpp) and need every pixel's exact position on native D3D9 and under the DXVK fork. The G-buffer's camera
// distance (COLOR3.b) is a half float: fine indoors, a few units off at 2,000-16,000 units, too coarse for sun shadows there.
// [Main.Main.Quality] DeferredDepth (0 off = default, 1, 2; read at startup):
//   1: at the span's end (after CapturedNormals::EndSpan, before DeferredLamps::Apply) the world's depth buffer is copied into its own
//      screen-sized INTZ texture, TESR_DeferredDepth, by the copy every NVR depth reader already gets (RenderManager::ResolveDepthBuffer:
//      RESZ on AMD / Intel drivers and under any DXVK with RESZ, the fork included (patch 0007); NVAPI's StretchRectEx on NVIDIA's native
//      driver). With the game's MSAA the fork's RESZ takes each pixel's nearest sample (patch 0027); native D3D9 has no G-buffer then.
//      Passes read the raw device depth (POINT / CLAMP / no mips / no sRGB / LOD bias 0 / max mip 0: Bind sets these, the states of the
//      two D3D9 passes known to read an INTZ texture under the fork, TAAUpscale's composite and CombineDepth's D3D9 technique);
//   2: the same copy, then turned into view z (fp32, game units) in TESR_DeferredDepthLinear (R32F): under the fork by a Vulkan pass of
//      the extension (Compute/DeferredDepthLinear.frag, texelFetch: the way every working fork depth reader reads INTZ, through Vulkan),
//      elsewhere by a D3D9 pass (Shaders/DeferredDepthLinear.hlsl). D3D9 passes then read a plain float texture, which works on both
//      runtimes (every post effect reads CombineDepth's R32F that way). This is the route that does not depend on a D3D9 pass reading
//      INTZ under the fork (ReactiveMask.h: it read 0 there on 2026-10-30, cause unknown).
// The frame block TESR_DeferredDepthFrame[6] (camera axes, the projection the list was drawn with, the viewport, the camera) is filled
// at the copy; Shaders/Includes/DeferredDepth.hlsl turns (pixel, raw depth or view z) into view z, camera distance, world position.
// A ShaderRecord-loaded pass that declares these by name gets them bound by SetCT; a C++-driven pass calls Bind / BindLinear.
// [Main.Main.Quality] DeferredDepthProbe (test only, frames; 0 off; above 0 at startup loads the probe and makes mode 2): every that many
// frames, at the span's end, a 64 x 64 grid of the screen is read through the helper and read back (one GPU wait, those frames only),
// twice: with the game's depth-stencil unbound (as ReactiveMask's pass that read 0) and bound (as TAAUpscale's composite). Per distance
// band (by the G-buffer's distance) it logs the pixels, the reads that came back 0, and the max / mean relative error against COLOR3.b of:
// the INTZ copy read by D3D9 (mode 1), the linear texture (mode 2), and TESR_DepthBufferWorld (last frame's world resolve, ReactiveMask's).
// GPU cost: F10's "Deferred depth copy", "Deferred depth linear" and "Deferred depth probe" timers.
// Off: no texture, no copy, nothing drawn; no shader template changes.

#include <vector>
#include <cmath>
#include "GpuProfiler.h"
#include "CapturedNormals.h"
#include "ShaderRecord.h"
#include "DxvkCompute.h"

namespace DeferredDepth {

	inline const char* kSection = "Main.Main.Quality";
	inline int Mode = 0;						// startup: 0 off, 1 the INTZ copy, 2 + the linear view z
	inline bool ProbeWanted = false;			// startup: the probe on (its shader loaded)
	inline IDirect3DTexture9* Texture = nullptr;	// TESR_DeferredDepth (INTZ)
	inline IDirect3DTexture9* LinearTexture = nullptr;	// TESR_DeferredDepthLinear (R32F view z; mode 2)
	inline IDirect3DSurface9* LinearSurface = nullptr;
	inline UINT Width = 0, Height = 0;
	inline D3DXVECTOR4 Frame[6] = {};			// TESR_DeferredDepthFrame (Includes/DeferredDepth.hlsl)
	inline UInt32 CopiedFrame = 0xFFFFFFFFu;	// TheFrameRateManager->FrameNumber of the last copy
	inline UInt32 LinearFrame = 0xFFFFFFFFu;	// ... and of the last linear pass
	inline unsigned CopiesThisFrame = 0;
	inline double ProjectionUsed[6] = {};		// _11, _22, _31, _32, _33, _43 of the last copy (the probe's CPU check)
	inline ShaderRecordPixel* LinearPass = nullptr;	// the D3D9 linear pass
	inline ID3D9NvrExtPipeline* LinearPipeline = nullptr;	// the fork's linear pass
	inline bool LinearFailed = false, LinearExtFailed = false;
	inline const char* LinearPath = "none";		// what made the last linear texture (logged by the probe)
	inline ShaderRecordPixel* ProbePass = nullptr;
	inline bool ProbeFailed = false;
	inline IDirect3DSurface9* ProbeTarget[6] = {};	// pass A (depth-stencil unbound) RT0-2, pass B (bound) RT0-2
	inline IDirect3DSurface9* ProbeCopy[6] = {};
	inline UInt32 ProbeRuns = 0, ProbeLastFrame = 0;
	inline unsigned long long StatFrames = 0, StatCopies = 0, StatLinear = 0;

	inline UInt32 FrameNumber() { return TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0; }
	// This frame's copy / linear view z exist (the passes' switches).
	inline bool Ready() { return Texture && CopiedFrame == FrameNumber(); }
	inline bool LinearReady() { return LinearTexture && LinearFrame == FrameNumber(); }

	// TextureManager::Initialize: the switches read once; the textures are made there (no device work on the render thread later).
	inline int ReadStartup() {
		const int probe = TheSettingManager->GetSettingI(kSection, "DeferredDepthProbe");
		ProbeWanted = probe > 0;
		Mode = std::clamp(TheSettingManager->GetSettingI(kSection, "DeferredDepth"), 0, 2);
		if (ProbeWanted) Mode = 2;	// the probe measures both routes
		return Mode;
	}

	// ShaderManager::Initialize, after the constants: the D3D9 linear pass, the fork's linear pipeline, the probe (startup, never on the
	// render thread). Nothing when off.
	inline void LoadShaders() {
		if (Mode >= 2 && !LinearPass) {
			static D3DXMACRO none[1] = { { NULL, NULL } };
			ShaderTemplate tmpl = {};
			tmpl.Name = "DeferredDepthLinear";
			LinearPass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredDepthLinear.pso", NULL, tmpl, "Deferred\\Depth\\", none);
			if (!LinearPass) Logger::Log("[ERROR] UNOFFICIAL DeferredDepth: the D3D9 linear pass (Shaders\\DeferredDepthLinear.hlsl) did not load");
			if (DxvkCompute::Available()) {
				static const NVR_EXT_BINDING_DESC bindings[] = { { 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE, 0, NVR_EXT_VIEW_2D, FALSE, 0 } };	// TESR_DeferredDepth
				LinearPipeline = DxvkCompute::CreateGraphicsPipeline("DeferredDepthLinear", bindings, 1, 16, false, 1, NVR_EXT_BLEND_NONE, false);
			}
			Logger::Log("UNOFFICIAL DeferredDepth: mode 2 (the linear view z): %s", LinearPipeline ? "the fork's Vulkan pass (Compute\\DeferredDepthLinear.spv), the D3D9 pass as fallback"
				: LinearPass ? "the D3D9 pass" : "NO pass loaded: mode 1 only");
			if (!LinearPipeline && !LinearPass) LinearFailed = true;
		}
		if (ProbeWanted && !ProbePass && !ProbeFailed) {
			static D3DXMACRO none[1] = { { NULL, NULL } };
			ShaderTemplate tmpl = {};
			tmpl.Name = "DeferredDepthProbe";
			ProbePass = (ShaderRecordPixel*)ShaderRecord::LoadShader("DeferredDepthProbe.pso", NULL, tmpl, "Deferred\\Depth\\", none);
			ProbeFailed = ProbePass == nullptr;
			Logger::Log(ProbePass ? "UNOFFICIAL DeferredDepth: the probe shader loaded (Shaders\\DeferredDepthProbe.hlsl)"
				: "[ERROR] UNOFFICIAL DeferredDepth: the probe shader (Shaders\\DeferredDepthProbe.hlsl) did not load: no probe");
		}
	}

	// The frame block from the projection the list was drawn with (NiDX9Renderer::projMatrix: TAA's jitter is its _31 / _32), the camera's
	// axes (TheRenderManager->InvViewMatrix rows, as DeferredLamps), the viewport now bound, the camera's position.
	inline void FillFrame(IDirect3DDevice9* d) {
		D3DVIEWPORT9 vp = {};
		d->GetViewport(&vp);
		const D3DXMATRIX& p = NiDX9Renderer::GetSingleton()->projMatrix;
		const D3DXMATRIX& iv = TheRenderManager->InvViewMatrix;
		const D3DXVECTOR4& cam = TheRenderManager->CameraPosition;
		const float rx = p._11 != 0.0f ? 1.0f / p._11 : 0.0f, ry = p._22 != 0.0f ? 1.0f / p._22 : 0.0f;
		Frame[0] = D3DXVECTOR4(iv._11, iv._12, iv._13, rx);
		Frame[1] = D3DXVECTOR4(iv._21, iv._22, iv._23, ry);
		Frame[2] = D3DXVECTOR4(iv._31, iv._32, iv._33, p._33);
		Frame[3] = D3DXVECTOR4(vp.Width ? 1.0f / (float)vp.Width : 0.0f, vp.Height ? 1.0f / (float)vp.Height : 0.0f, (float)vp.X, (float)vp.Y);
		Frame[4] = D3DXVECTOR4(cam.x, cam.y, cam.z, p._43);
		Frame[5] = D3DXVECTOR4(p._31, p._32, Width ? 1.0f / (float)Width : 0.0f, Height ? 1.0f / (float)Height : 0.0f);
		ProjectionUsed[0] = p._11; ProjectionUsed[1] = p._22; ProjectionUsed[2] = p._31; ProjectionUsed[3] = p._32; ProjectionUsed[4] = p._33; ProjectionUsed[5] = p._43;
	}

	inline void PointStates(IDirect3DDevice9* d, DWORD stage) {
		d->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		d->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
		d->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
		d->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
	}

	// For a C++-driven pass: the INTZ copy (Bind) or the linear view z (BindLinear) on a stage with the states that read it, the frame block
	// into six pixel registers. The caller saves and puts back what it changes (as DeferredLamps::Apply does for its own).
	inline void Bind(IDirect3DDevice9* d, DWORD stage, UINT firstRegister) {
		d->SetTexture(stage, Texture);
		PointStates(d, stage);
		d->SetPixelShaderConstantF(firstRegister, &Frame[0].x, 6);
	}
	inline void BindLinear(IDirect3DDevice9* d, DWORD stage, UINT firstRegister) {
		d->SetTexture(stage, LinearTexture);
		PointStates(d, stage);
		d->SetPixelShaderConstantF(firstRegister, &Frame[0].x, 6);
	}

	// Device state a full-screen D3D9 pass of this file changes, read on construction and put back on destruction: targets 0-3, the
	// depth-stencil, viewport, shaders, FVF / declaration, stream 0, stages 0-3 (texture + eight sampler states), the render states
	// below, pixel constants c0-c7.
	struct SavedState {
		IDirect3DDevice9* d;
		IDirect3DSurface9* rt[4] = {};
		IDirect3DSurface9* zs = nullptr;
		D3DVIEWPORT9 vp = {};
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0;
		IDirect3DBaseTexture9* tex[4] = {};
		static constexpr D3DSAMPLERSTATETYPE kSS[8] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV,
			D3DSAMP_SRGBTEXTURE, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL };
		DWORD ss[4][8] = {};
		static constexpr D3DRENDERSTATETYPE kRS[13] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_FILLMODE,
			D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE };
		static constexpr DWORD kWant[13] = { D3DZB_FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, 0xFu, 0xFu, D3DFILL_SOLID, FALSE, FALSE };
		DWORD rs[13] = {};
		DWORD clipPlanes = 0;
		float c[8][4] = {};

		explicit SavedState(IDirect3DDevice9* device) : d(device) {
			for (DWORD i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
			d->GetDepthStencilSurface(&zs);
			d->GetViewport(&vp);
			d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf); d->GetStreamSource(0, &vb, &vbOffset, &vbStride);
			for (DWORD s = 0; s < 4; s++) {
				d->GetTexture(s, &tex[s]);
				for (int i = 0; i < 8; i++) d->GetSamplerState(s, kSS[i], &ss[s][i]);
			}
			for (int i = 0; i < 13; i++) { if (FAILED(d->GetRenderState(kRS[i], &rs[i]))) rs[i] = kWant[i]; if (rs[i] != kWant[i]) d->SetRenderState(kRS[i], kWant[i]); }
			d->GetRenderState(D3DRS_CLIPPLANEENABLE, &clipPlanes);
			if (clipPlanes) d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
			d->GetPixelShaderConstantF(0, &c[0][0], 8);
			for (DWORD i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
			d->SetVertexShader(nullptr);
			d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		}
		~SavedState() {
			for (DWORD i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
			d->SetDepthStencilSurface(zs);
			for (DWORD i = 0; i < 4; i++) { if (i == 0 || rt[i]) d->SetRenderTarget(i, rt[i]); if (rt[i]) rt[i]->Release(); }
			if (zs) zs->Release();
			d->SetViewport(&vp);
			for (DWORD s = 0; s < 4; s++) {
				for (int i = 0; i < 8; i++) d->SetSamplerState(s, kSS[i], ss[s][i]);
				d->SetTexture(s, tex[s]); if (tex[s]) tex[s]->Release();
			}
			for (int i = 0; i < 13; i++) if (rs[i] != kWant[i]) d->SetRenderState(kRS[i], rs[i]);
			if (clipPlanes) d->SetRenderState(D3DRS_CLIPPLANEENABLE, clipPlanes);
			d->SetPixelShaderConstantF(0, &c[0][0], 8);
			if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
			d->SetStreamSource(0, vb, vbOffset, vbStride); if (vb) vb->Release();
			d->SetVertexShader(vs); if (vs) vs->Release();
			d->SetPixelShader(ps); if (ps) ps->Release();
		}
	};

	// A quad over a w x h target, pre-transformed (the D3D9 half-pixel offset).
	inline void Quad(IDirect3DDevice9* d, float w, float h) {
		const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 } };
		d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	}

	// Mode 2: the copy as view z into TESR_DeferredDepthLinear. The fork's Vulkan pass first (it reads the INTZ image the way CombineDepth.frag
	// does); the D3D9 pass when there is no extension, or the Vulkan pass failed.
	inline bool Linearize(IDirect3DDevice9* d) {
		if (!LinearSurface || LinearFailed) return false;
		static GpuTimer timer("Deferred depth linear");
		GpuProfileScope gpu(timer, d);
		if (LinearPipeline && !LinearExtFailed && DxvkCompute::Available()) {
			const float push[4] = { (float)ProjectionUsed[4], (float)ProjectionUsed[5], 0.0f, 0.0f };
			NVR_EXT_RESOURCE_BINDING resource = {};
			resource.Slot = 0; resource.Type = NVR_EXT_BIND_SAMPLED_IMAGE; resource.pResource = Texture;
			NVR_EXT_DRAW_DESC draw = {};
			draw.pPipeline = LinearPipeline;
			draw.TargetCount = 1;
			draw.Targets[0].pResource = LinearSurface;
			draw.BindingCount = 1;
			draw.pBindings = &resource;
			draw.pPushData = push;
			draw.PushDataSize = sizeof(push);
			const HRESULT result = DxvkCompute::Extension()->Draw(&draw);
			if (SUCCEEDED(result)) { LinearPath = "fork Vulkan pass"; return true; }
			LinearExtFailed = true;
			Logger::Log("UNOFFICIAL DeferredDepth: the fork's linear pass failed to draw (%08lX; DXVK's log says why): the D3D9 pass from now on", result);
		}
		if (!LinearPass) return false;
		SavedState saved(d);
		d->SetDepthStencilSurface(nullptr);	// (the game's may be multisampled; the target is not)
		d->SetRenderTarget(0, LinearSurface);
		const D3DVIEWPORT9 full = { 0, 0, Width, Height, 0.0f, 1.0f };
		d->SetViewport(&full);
		d->SetPixelShader(LinearPass->ShaderHandle);
		d->SetTexture(0, Texture);
		PointStates(d, 0);
		const float c0[4] = { (float)ProjectionUsed[4], (float)ProjectionUsed[5], Width ? 1.0f / Width : 0.0f, Height ? 1.0f / Height : 0.0f };
		d->SetPixelShaderConstantF(0, c0, 1);
		Quad(d, (float)Width, (float)Height);
		LinearPath = "D3D9 pass";
		return true;
	}

	inline void Probe(IDirect3DDevice9* d);

	// GrassFirst::MainListHook's DeferredSpan, before DeferredLamps::Apply: the copy, the frame block, the linear view z (mode 2), and the
	// probe on its frames. Every world main list is copied (the passes after it run per list); the log line says how many a frame.
	inline void AtSpanEnd(IDirect3DDevice9* d) {
		if (!Texture) return;
		const UInt32 f = FrameNumber();
		if (CopiedFrame != f) { CopiesThisFrame = 0; StatFrames++; }
		{
			static GpuTimer timer("Deferred depth copy");
			GpuProfileScope gpu(timer, d);
			TheRenderManager->ResolveDepthBuffer(Texture);
		}
		FillFrame(d);
		CopiedFrame = f;
		CopiesThisFrame++;
		StatCopies++;
		if (Mode >= 2 && Linearize(d)) { LinearFrame = f; StatLinear++; }
		const int every = TheSettingManager->GetSettingI(kSection, "DeferredDepthProbe");
		if (every > 0 && ProbePass && CopiesThisFrame == 1 && f - ProbeLastFrame >= (UInt32)every) { ProbeLastFrame = f; Probe(d); }
		if (StatFrames >= 3600 && GpuTimer::Enabled) {
			Logger::Log("DEFERRED DEPTH %llu frames: copies %.2f a frame (more than 1: several world main lists in a frame, each copied), linear passes %.2f a frame (%s)",
				StatFrames, (double)StatCopies / (double)StatFrames, (double)StatLinear / (double)StatFrames, LinearPath);
			StatFrames = StatCopies = StatLinear = 0;
		}
	}

	// ---- the probe (test only) ----

	inline bool EnsureProbeTargets(IDirect3DDevice9* d) {
		if (ProbeTarget[0]) return true;
		for (int i = 0; i < 6; i++)
			if (FAILED(d->CreateRenderTarget(64, 64, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &ProbeTarget[i], NULL)) ||
				FAILED(d->CreateOffscreenPlainSurface(64, 64, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &ProbeCopy[i], NULL))) {
				for (int k = 0; k < 6; k++) {
					if (ProbeTarget[k]) { ProbeTarget[k]->Release(); ProbeTarget[k] = nullptr; }
					if (ProbeCopy[k]) { ProbeCopy[k]->Release(); ProbeCopy[k] = nullptr; }
				}
				ProbeFailed = true;
				Logger::Log("[ERROR] UNOFFICIAL DeferredDepth probe: could not make its 64 x 64 A32B32G32R32F targets: no probe");
				return false;
			}
		return true;
	}

	inline bool ReadProbe(IDirect3DDevice9* d, int i, std::vector<float>& out) {
		if (FAILED(d->GetRenderTargetData(ProbeTarget[i], ProbeCopy[i]))) return false;
		D3DLOCKED_RECT r = {};
		if (FAILED(ProbeCopy[i]->LockRect(&r, NULL, D3DLOCK_READONLY))) return false;
		out.resize(4096 * 4);
		for (int y = 0; y < 64; y++) memcpy(&out[y * 64 * 4], (const char*)r.pBits + y * r.Pitch, 64 * 4 * sizeof(float));
		ProbeCopy[i]->UnlockRect();
		return true;
	}

	// The camera distance of a view z at a screen pixel, in doubles, from the numbers the frame block was made from (the helper's check).
	inline double CpuDistanceZ(double viewZ, double px, double py) {
		const double vpW = Frame[3].x > 0.0f ? 1.0 / Frame[3].x : 1.0, vpH = Frame[3].y > 0.0f ? 1.0 / Frame[3].y : 1.0;
		const double lx = (px - Frame[3].z) / vpW, ly = (py - Frame[3].w) / vpH;	// (2026-11-15 fd-sun-fsr: no + 0.5, as DDRay)
		const double nx = lx * 2.0 - 1.0 - ProjectionUsed[2], ny = 1.0 - ly * 2.0 - ProjectionUsed[3];
		const double ax = ProjectionUsed[0] != 0.0 ? nx / ProjectionUsed[0] : 0.0, ay = ProjectionUsed[1] != 0.0 ? ny / ProjectionUsed[1] : 0.0;
		return fabs(viewZ) * sqrt(1.0 + ax * ax + ay * ay);	// the axes are orthonormal: |F + R ax + U ay| = sqrt(1 + ax^2 + ay^2)
	}
	inline double CpuViewZ(double device) {
		double den = device - ProjectionUsed[4];
		if (fabs(den) < 1.0e-12) den = ProjectionUsed[5] < 0.0 ? -1.0e-12 : 1.0e-12;
		return ProjectionUsed[5] / den;
	}

	struct BandStats {
		unsigned pixels = 0, zero = 0, over1 = 0;
		double maxRel = 0.0, sumRel = 0.0;
		void Add(double rel) { sumRel += rel; if (rel > maxRel) maxRel = rel; if (rel > 0.01) over1++; }
		double Mean() const { const unsigned n = pixels - zero; return n ? sumRel / n : 0.0; }
		bool Works() const { return pixels == 0 || (zero == 0 && (pixels < 16 || Mean() <= 2.0e-3)); }
	};

	// One pass's numbers into the log; true per source when it works (see the verdict line). c0-c2: its targets (Shaders/DeferredDepthProbe.hlsl).
	inline unsigned Report(const char* label, const std::vector<float>& c0, const std::vector<float>& c1, const std::vector<float>& c2, bool works[3]) {
		static const char* const kBand[4] = { "0-500", "500-2000", "2000-8000", "8000+" };
		BandStats copy[4], linear[4], last[4];
		unsigned written = 0, sky = 0, drawnNoGBuffer = 0, zeroAll = 0, nanCount = 0;
		double helperMax = 0.0, linearVsCopyMax = 0.0;
		const double farZ = CpuViewZ(NiDX9Renderer::GetSingleton()->m_fZClear);	// the view z the clear value (sky, or a read of 0) gives
		const bool haveLinear = LinearFrame == CopiedFrame;
		for (int i = 0; i < 4096; i++) {
			const float* a = &c0[i * 4];
			const float* b = &c1[i * 4];
			const float* e = &c2[i * 4];
			const double dCopy = a[0], dStored = a[1], raw = a[2], mark = a[3], dLast = b[0], rawLast = b[1], dLinear = e[0], zLinear = e[1];
			if (!(raw == raw) || !(dCopy == dCopy) || !(dLinear == dLinear)) { nanCount++; continue; }
			if (raw <= 0.0) zeroAll++;
			if (mark < 0.5 || !(dStored > 0.0)) {	// the G-buffer holds nothing here
				if (raw <= 0.0) sky++; else drawnNoGBuffer++;
				continue;
			}
			written++;
			const int band = dStored < 500.0 ? 0 : dStored < 2000.0 ? 1 : dStored < 8000.0 ? 2 : 3;
			copy[band].pixels++; last[band].pixels++; linear[band].pixels++;
			if (raw <= 0.0) copy[band].zero++;
			else {
				copy[band].Add(fabs(dCopy - dStored) / dStored);
				const double cpu = CpuDistanceZ(CpuViewZ(raw), b[2], b[3]);
				if (cpu > 0.0) { const double h = fabs(dCopy - cpu) / cpu; if (h > helperMax) helperMax = h; }
				if (haveLinear && zLinear > 0.0) { const double v = fabs(zLinear - CpuViewZ(raw)) / CpuViewZ(raw); if (v > linearVsCopyMax) linearVsCopyMax = v; }
			}
			if (rawLast <= 0.0) last[band].zero++; else last[band].Add(fabs(dLast - dStored) / dStored);
			if (!haveLinear || !(zLinear > 0.0) || zLinear >= farZ * 0.999) linear[band].zero++; else linear[band].Add(fabs(dLinear - dStored) / dStored);
		}
		Logger::Log("DEFERRED DEPTH PROBE %s: G-buffer written %u of 4096, not written %u (%u of them depth 0 = sky, %u drawn without G-buffer); "
			"INTZ copy reads 0 at %u of 4096; NaN %u; helper vs CPU (doubles) max relative difference %.2e; linear view z vs the INTZ copy's max relative difference %.2e (%s)",
			label, written, 4096 - written - nanCount, sky, drawnNoGBuffer, zeroAll, nanCount, helperMax, linearVsCopyMax, haveLinear ? LinearPath : "no linear texture this frame");
		works[0] = works[1] = works[2] = written > 0;
		for (int k = 0; k < 4; k++) {
			Logger::Log("DEFERRED DEPTH PROBE %s band %s: %u px | INTZ copy read by D3D9 (mode 1): read 0 %u, max rel %.2e, mean rel %.2e, > 1 %% %u | "
				"linear view z (mode 2): read 0 / far %u, max rel %.2e, mean rel %.2e, > 1 %% %u | TESR_DepthBufferWorld (last frame): read 0 %u, max rel %.2e, mean rel %.2e, > 1 %% %u",
				label, kBand[k], copy[k].pixels, copy[k].zero, copy[k].maxRel, copy[k].Mean(), copy[k].over1,
				linear[k].zero, linear[k].maxRel, linear[k].Mean(), linear[k].over1,
				last[k].zero, last[k].maxRel, last[k].Mean(), last[k].over1);
			works[0] = works[0] && copy[k].Works();
			works[1] = works[1] && haveLinear && linear[k].Works();
			works[2] = works[2] && last[k].zero == 0;
		}
		return written;
	}

	inline void Probe(IDirect3DDevice9* d) {
		if (ProbeFailed || !ProbePass || !EnsureProbeTargets(d)) return;
		const D3DCAPS9* caps = TheRenderManager->DeviceCaps();
		if (!caps || caps->NumSimultaneousRTs < 3) { ProbeFailed = true; Logger::Log("[ERROR] UNOFFICIAL DeferredDepth probe: fewer than 3 render targets: no probe"); return; }
		static GpuTimer timer("Deferred depth probe");
		ProbeRuns++;

		D3DSURFACE_DESC gbuffer = {}, depthDesc = {}, copyDesc = {};
		if (CapturedNormals::TextureSurface) CapturedNormals::TextureSurface->GetDesc(&gbuffer);
		Texture->GetLevelDesc(0, &copyDesc);
		D3DVIEWPORT9 vp = {};
		d->GetViewport(&vp);
		DWORD inherited[8] = {};	// stage 2's states as the frame left them (ReactiveMask's pass set only the first six)
		for (int i = 0; i < 8; i++) d->GetSamplerState(2, SavedState::kSS[i], &inherited[i]);
		bool drawn[2] = {}, canBindDepth = false;
		{
			SavedState saved(d);
			if (saved.zs) saved.zs->GetDesc(&depthDesc);
			// pass B binds the game's depth-stencil only when a 64 x 64 single-sample target may use it
			canBindDepth = saved.zs && depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE && depthDesc.Width >= 64 && depthDesc.Height >= 64;
			const float map[2][4] = {
				{ vp.Width / 64.0f, vp.Height / 64.0f, (float)vp.X, (float)vp.Y },
				{ gbuffer.Width ? 1.0f / gbuffer.Width : 0.0f, gbuffer.Height ? 1.0f / gbuffer.Height : 0.0f, 0.0f, 0.0f } };
			const D3DVIEWPORT9 probeVp = { 0, 0, 64, 64, 0.0f, 1.0f };
			const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { 63.5f, -0.5f, 0, 1, 1, 0 }, { -0.5f, 63.5f, 0, 1, 0, 1 }, { 63.5f, 63.5f, 0, 1, 1, 1 } };
			GpuProfileScope gpu(timer, d);
			d->SetPixelShader(ProbePass->ShaderHandle);
			Bind(d, 0, 0);
			d->SetTexture(1, CapturedNormals::Texture); PointStates(d, 1);
			d->SetTexture(2, TheTextureManager->DepthTexture); PointStates(d, 2);
			d->SetTexture(3, LinearTexture); PointStates(d, 3);
			d->SetPixelShaderConstantF(6, &map[0][0], 2);
			for (int pass = 0; pass < 2; pass++) {
				if (pass == 1 && !canBindDepth) break;
				d->SetDepthStencilSurface(pass == 1 ? saved.zs : nullptr);
				for (DWORD t = 0; t < 3; t++) d->SetRenderTarget(t, ProbeTarget[pass * 3 + t]);
				d->SetViewport(&probeVp);
				drawn[pass] = SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
			}
		}

		// read back (waits for the GPU once, on probe frames only) and report
		Logger::Log("DEFERRED DEPTH PROBE #%u at frame %u: %s; game depth-stencil %ux%u format 0x%08lx multisample %u; copy %ux%u; viewport %lu,%lu %lux%lu; "
			"projection _11 %.5f _22 %.5f _31 %.6f _32 %.6f _33 %.7f _43 %.4f (z clear %.1f); G-buffer %s %ux%u; linear %s; stage 2 as the frame left it: "
			"min %lu mag %lu mip %lu u %lu v %lu srgb %lu lodbias 0x%08lx maxmip %lu",
			ProbeRuns, FrameNumber(), TheRenderManager->D3D9RuntimeDescription(), depthDesc.Width, depthDesc.Height, (unsigned long)depthDesc.Format,
			(unsigned)depthDesc.MultiSampleType, copyDesc.Width, copyDesc.Height, vp.X, vp.Y, vp.Width, vp.Height,
			ProjectionUsed[0], ProjectionUsed[1], ProjectionUsed[2], ProjectionUsed[3], ProjectionUsed[4], ProjectionUsed[5], NiDX9Renderer::GetSingleton()->m_fZClear,
			CapturedNormals::Drawn ? "drawn" : "NOT drawn this span", gbuffer.Width, gbuffer.Height, LinearFrame == CopiedFrame ? LinearPath : "not made this frame",
			inherited[0], inherited[1], inherited[2], inherited[3], inherited[4], inherited[5], inherited[6], inherited[7]);
		std::vector<float> c[3];
		bool works[2][3] = {}, read[2] = {};
		unsigned written[2] = {};
		for (int pass = 0; pass < 2; pass++) {
			const char* label = pass ? "B (depth-stencil bound)" : "A (depth-stencil unbound)";
			if (!drawn[pass]) {
				Logger::Log("DEFERRED DEPTH PROBE %s: not drawn (%s)", label, pass && !canBindDepth ? "the game's depth-stencil is multisampled or missing" : "the draw failed");
				continue;
			}
			if (!ReadProbe(d, pass * 3, c[0]) || !ReadProbe(d, pass * 3 + 1, c[1]) || !ReadProbe(d, pass * 3 + 2, c[2])) { Logger::Log("DEFERRED DEPTH PROBE %s: read-back failed", label); continue; }
			read[pass] = true;
			written[pass] = Report(label, c[0], c[1], c[2], works[pass]);
		}
		auto word = [&](int pass, int source) { return !read[pass] ? "not measured" : !written[pass] ? "not measured (no G-buffer pixel)" : works[pass][source] ? "WORKS" : "FAILS"; };
		Logger::Log("DEFERRED DEPTH PROBE verdict: INTZ copy read by D3D9 (mode 1): A %s, B %s | linear view z (mode 2, %s): A %s, B %s | TESR_DepthBufferWorld no zero reads: A %s, B %s. "
			"Works = no zero reads where the G-buffer has a surface and mean relative error at most 0.2 %% in every band with 16+ pixels (COLOR3.b is a half float: about 0.01-0.05 %% expected)",
			word(0, 0), word(1, 0), LinearPath, word(0, 1), word(1, 1), word(0, 2), word(1, 2));
	}
}
