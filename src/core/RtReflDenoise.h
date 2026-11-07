#pragma once
// UNOFFICIAL 2026-11-07 RT reflection denoiser ([Shaders.Reflections.RT] Denoiser; docs/next/RT-REFLECTION-DENOISER.md).
// Plain C++ (d3d9.h + the NVR DXVK extension only): the game (effects/Reflections) and the RT test host (tools/nvr-rt-host) run the
// same code. Four NVR DXVK fragment passes on the reflection grid (half size in the game), after ReflectionsRTResolve, in place of
// the old Temporal + Blur X / Y:
//   1 Reproject (4 targets)  2 Average (1/8 size)  3 Prefilter (2 targets)  4 Temporal (2 targets: the result = next frame's history)
// Core 1 = AMD FidelityFX Reflections Denoiser, ported (RtReflDenoise*.frag; MIT, notice in RtReflDenoise.glsl). Core 0 = ours
// (RtReflDenoiseOurs*.frag), from the published ReLAX / ReBLUR designs. The histories live here (D3D9 render-target textures).

#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include "NvrDxvkExt.h"

namespace RtReflDenoise {

	struct Push {                // = RtReflDenoise.glsl Push (208 bytes)
		float reproject[16];     // camera-relative world now -> last frame's clip (GLSL column-major: clip = M * p)
		float forward[4];        // xyz forward, w farZ
		float right[4];          // xyz right / P00, w 1 = ObjectMotion bound
		float up[4];             // xyz up / P11
		float size[4];           // w, h, 1 / w, 1 / h
		float fullUv[4];         // xy uv offset to the full-size pixel
		float params[4];         // x max samples, y history clip weight, z mirror roughness, w 1 = history usable
		float params2[4];        // x core, y debug view, z ours: blur radius, w ours: contact share
		float params3[4];        // x ours: anti-lag sigma, y ours: max history for a near mirror, z ours: firefly clamp, w -
		uint32_t linearIndex, pointIndex, pad0, pad1;
	};
	static_assert(sizeof(Push) == 208, "RtReflDenoise.glsl Push");

	struct Settings {
		int		Core = 0;			// 0 ours, 1 FFX (AMD FidelityFX Reflections, ported)
		float	MaxSamples = 32.0f;	// the history's length cap (frames) for rough surfaces
		float	ClipWeight = 0.7f;	// FFX's temporal stability: how far the history may stray from this frame's neighbourhood
		float	MirrorRoughness = 0.03f;	// under this: a mirror, traced noise-free, passes through (the old path's rule)
		float	BlurRadius = 10.0f;	// ours: the spatial blur's reach at roughness 1 (grid pixels)
		float	ContactShare = 0.2f;// ours: the blur's share where the reflected thing touches the surface (hit distance 0)
		float	AntiLag = 2.5f;		// ours: history further than this many sigmas from this frame's neighbourhood is cut
		float	MirrorHistory = 4.0f;	// ours: frames of history at roughness MirrorRoughness (growing to MaxSamples by 0.25)
		float	Firefly = 3.0f;		// ours: a sample brighter than this x its neighbourhood's mean + 2 sigma is clamped (0 off)
		int		DebugView = 0;
	};

	// One frame's camera (filled by the caller from its matrices)
	struct Frame {
		float reproject[16];	// as Push
		float forward[3], right[3], up[3], farZ;
		float fullUvX = 0.0f, fullUvY = 0.0f;
		bool historyUsable = false;	// last frame ran, no camera cut
	};

	// Shader file names per core and pass
	inline const char* PassName(int core, int pass) {
		static const char* ffx[4] = { "RtReflDenoiseReproject", "RtReflDenoiseAverage", "RtReflDenoisePrefilter", "RtReflDenoiseTemporal" };
		static const char* ours[4] = { "RtReflDenoiseOursReproject", "RtReflDenoiseAverage", "RtReflDenoiseOursBlur", "RtReflDenoiseOursTemporal" };
		return core == 1 ? ffx[pass] : ours[pass];
	}
	inline UINT PassTargets(int pass) { static const UINT t[4] = { 4, 1, 2, 2 }; return t[pass]; }

	class Denoiser {
	public:
		using Loader = std::function<bool(const char* name, std::vector<char>& spirv)>;
		static const UINT Images = 10;

		// Made once for a grid size (and again when it changes). False: something could not be made (the caller keeps the old path).
		bool Init(IDirect3DDevice9* device, ID3D9NvrExt* ext, UINT width, UINT height, ID3D9NvrExtSampler* linear, ID3D9NvrExtSampler* point, const Loader& load) {
			if (ready && width == w && height == h) return true;
			Release();
			dev = device; this->ext = ext; w = width; h = height; linearS = linear; pointS = point; loader = load;
			const D3DFORMAT f16 = D3DFMT_A16B16G16R16F;
			bool ok = true;
			for (int i = 0; i < 2 && ok; i++)
				ok = Make(w, h, f16, result[i]) && Make(w, h, f16, moments[i]) && Make(w, h, D3DFMT_A32B32G32R32F, guide[i]);
			ok = ok && Make(w, h, f16, reproj) && Make(w, h, f16, momentsNow) && Make(w, h, f16, avgSource) && Make(w, h, f16, pref) &&
				Make(w, h, f16, prefVar) && Make((w + 7) / 8, (h + 7) / 8, f16, average);
			if (!ok) { Release(); failed = true; return false; }
			ready = true;
			frame = 0;
			return true;
		}
		void Release() {
			for (auto* t : { &result[0], &result[1], &moments[0], &moments[1], &guide[0], &guide[1], &reproj, &momentsNow, &avgSource, &pref, &prefVar, &average })
				t->Release();
			for (auto& c : pipes) for (auto*& p : c) if (p) { p->Release(); p = nullptr; }
			ready = false;
		}
		bool Failed() const { return failed; }
		bool Ready() const { return ready; }

		// The four passes. signal: Resolve's output; ray: the Ray image (x view z, y roughness + flags); hitDist: Resolve's hit
		// distance; normals / motion: the full-size buffers (motion may be null). After it, Result() holds this frame's result.
		bool Run(IUnknown* signal, IUnknown* ray, IUnknown* hitDist, IUnknown* normals, IUnknown* motion, const Frame& fr, const Settings& s) {
			if (!ready) return false;
			const int core = s.Core == 1 ? 1 : 0;
			for (int p = 0; p < 4; p++) if (!Pipe(core, p)) return false;
			Push push = {};
			std::memcpy(push.reproject, fr.reproject, sizeof(push.reproject));
			Put(push.forward, fr.forward[0], fr.forward[1], fr.forward[2], fr.farZ);
			Put(push.right, fr.right[0], fr.right[1], fr.right[2], motion ? 1.0f : 0.0f);
			Put(push.up, fr.up[0], fr.up[1], fr.up[2], 0.0f);
			Put(push.size, float(w), float(h), 1.0f / w, 1.0f / h);
			Put(push.fullUv, fr.fullUvX, fr.fullUvY, 0.0f, 0.0f);
			const bool usable = fr.historyUsable && frame > 0 && lastCore == core;
			Put(push.params, s.MaxSamples, s.ClipWeight, s.MirrorRoughness, usable ? 1.0f : 0.0f);
			Put(push.params2, float(core), float(s.DebugView), s.BlurRadius, s.ContactShare);
			Put(push.params3, s.AntiLag, s.MirrorHistory, s.Firefly, float(frame % 64));
			const UINT now = frame & 1, last = now ^ 1;
			IUnknown* m = motion ? motion : normals;
			// 1 Reproject: in0 last result, in1 last moments, in2 last guide
			{
				IUnknown* in[Images] = { signal, ray, hitDist, normals, m, result[last].t, moments[last].t, guide[last].t, guide[last].t, average.t };
				IUnknown* out[4] = { reproj.s, momentsNow.s, avgSource.s, guide[now].s };
				if (!Draw(pipes[core][0], out, 4, in, push)) return false;
			}
			// 2 Average
			{
				IUnknown* in[Images] = { signal, ray, hitDist, normals, m, avgSource.t, avgSource.t, avgSource.t, avgSource.t, reproj.t };
				IUnknown* out[1] = { average.s };
				if (!Draw(pipes[core][1], out, 1, in, push)) return false;
			}
			// 3 Prefilter / ours: blur. in0 this frame's moments, in1 the reprojected history, in2 this guide
			{
				IUnknown* in[Images] = { signal, ray, hitDist, normals, m, momentsNow.t, reproj.t, guide[now].t, guide[now].t, average.t };
				IUnknown* out[2] = { pref.s, prefVar.s };
				if (!Draw(pipes[core][2], out, 2, in, push)) return false;
			}
			// 4 Temporal: in0 prefiltered, in1 its variance, in2 reprojected, in3 this frame's moments
			{
				IUnknown* in[Images] = { signal, ray, hitDist, normals, m, pref.t, prefVar.t, reproj.t, momentsNow.t, average.t };
				IUnknown* out[2] = { result[now].s, moments[now].s };
				if (!Draw(pipes[core][3], out, 2, in, push)) return false;
			}
			lastCore = core;
			frame++;
			return true;
		}
		void Restart() { frame = 0; }
		// This frame's result (after Run): a render-target texture of the grid's size (RGBA16F)
		IDirect3DTexture9* Result() const { return result[(frame + 1) & 1].t; }
		IDirect3DSurface9* ResultSurface() const { return result[(frame + 1) & 1].s; }
		// For debug views / the host: this frame's intermediate images
		IDirect3DSurface9* MomentsSurface() const { return momentsNow.s; }
		IDirect3DSurface9* PrefilterSurface() const { return pref.s; }
		UINT Width() const { return w; }
		UINT Height() const { return h; }

	private:
		struct Tex {
			IDirect3DTexture9* t = nullptr; IDirect3DSurface9* s = nullptr;
			void Release() { if (s) { s->Release(); s = nullptr; } if (t) { t->Release(); t = nullptr; } }
		};
		IDirect3DDevice9* dev = nullptr;
		ID3D9NvrExt* ext = nullptr;
		ID3D9NvrExtSampler* linearS = nullptr;
		ID3D9NvrExtSampler* pointS = nullptr;
		Loader loader;
		UINT w = 0, h = 0;
		bool ready = false, failed = false;
		UINT frame = 0;
		int lastCore = -1;
		Tex result[2], moments[2], guide[2], reproj, momentsNow, avgSource, pref, prefVar, average;
		ID3D9NvrExtPipeline* pipes[2][4] = {};

		static void Put(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }
		bool Make(UINT tw, UINT th, D3DFORMAT f, Tex& t) {
			if (FAILED(dev->CreateTexture(tw, th, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &t.t, nullptr))) return false;
			if (FAILED(t.t->GetSurfaceLevel(0, &t.s))) return false;
			dev->ColorFill(t.s, nullptr, 0);
			return true;
		}
		ID3D9NvrExtPipeline* Pipe(int core, int pass) {
			if (pipes[core][pass]) return pipes[core][pass];
			std::vector<char> spirv;
			if (!loader || !loader(PassName(core, pass), spirv) || spirv.empty()) return nullptr;
			NVR_EXT_BINDING_DESC b[Images + 2] = {};
			for (UINT i = 0; i < Images; i++) b[i] = { 1, i, NVR_EXT_BIND_SAMPLED_IMAGE, i, NVR_EXT_VIEW_2D, FALSE, 0 };
			b[Images] = { 0, 0, NVR_EXT_BIND_SAMPLER, 0, NVR_EXT_VIEW_2D, FALSE, offsetof(Push, linearIndex) };
			b[Images + 1] = { 0, 0, NVR_EXT_BIND_SAMPLER, 1, NVR_EXT_VIEW_2D, FALSE, offsetof(Push, pointIndex) };
			NVR_EXT_GRAPHICS_PIPELINE_DESC d = {};
			d.pFragmentSpirv = spirv.data(); d.FragmentSpirvSize = UINT(spirv.size());
			d.BindingCount = Images + 2; d.pBindings = b; d.PushConstantSize = sizeof(Push);
			d.UsesSamplerHeap = TRUE; d.SamplerHeapSet = 0; d.SamplerHeapBinding = 0;
			d.ColorTargetCount = PassTargets(pass); d.Blend = NVR_EXT_BLEND_NONE; d.ColorWriteMask = 0xF; d.pDebugName = PassName(core, pass);
			if (FAILED(ext->CreateGraphicsPipeline(&d, &pipes[core][pass]))) { pipes[core][pass] = nullptr; failed = true; }
			return pipes[core][pass];
		}
		bool Draw(ID3D9NvrExtPipeline* p, IUnknown* const* out, UINT outs, IUnknown* const* in, const Push& push) {
			NVR_EXT_RESOURCE_BINDING b[Images + 2] = {};
			for (UINT i = 0; i < Images; i++) { b[i].Slot = i; b[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; b[i].pResource = in[i]; }
			b[Images].Slot = 0; b[Images].Type = NVR_EXT_BIND_SAMPLER; b[Images].pResource = linearS;
			b[Images + 1].Slot = 1; b[Images + 1].Type = NVR_EXT_BIND_SAMPLER; b[Images + 1].pResource = pointS;
			NVR_EXT_DRAW_DESC d = {};
			d.pPipeline = p; d.TargetCount = outs;
			for (UINT i = 0; i < outs; i++) d.Targets[i].pResource = out[i];
			d.BindingCount = Images + 2; d.pBindings = b; d.pPushData = &push; d.PushDataSize = sizeof(Push);
			return SUCCEEDED(ext->Draw(&d));
		}
	};
}
