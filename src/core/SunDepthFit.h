#pragma once
// UNOFFICIAL 2026-10-27 FitToDepth ([Shaders.ShadowsExteriors.Main] FitToDepth, FitToDepthDebug, FitToDepthLog; docs/next/FORK2-SDSM.md).
// Sun shadow cascades fitted to the depth that is really on screen ("sample distribution shadow maps", for native D3D9 too).
//
// Included only by ShadowsExterior.cpp (settings, constants, GetCascadeDepths) and ShaderManager.cpp (the measuring passes and the
// debug view), so that adding it rebuilds nothing else. All of it is idle -- nothing is created, drawn or changed -- while
// FitToDepth is off and FitToDepthDebug is 0: the cascades are then exactly as before.
//
// What the cascades do without it (ShadowsExteriorEffect::GetCascadeDepths / GetCascadeViewProj): four slices of the view,
// [near + 10, near + Distance] split by the CascadeLambda formula (211 / 571 / 1657 / 6000 units at the defaults), each shadow map
// fitted round the bounding sphere of its whole slice of the view frustum. When what is on screen ends sooner (looking down, a street
// of houses, under a canopy, a canyon) or starts later, part of that resolution is spent on depths nobody sees.
//
// 1. Measuring (Measure: in the pre-tonemap chain, right after CombineDepth). SunDepthFit.fx's Reduce pass turns the linear depth
//    buffer into a grid of 8 x 8 pixel blocks (farthest surface, nearest surface, sky). Its Count pass is then drawn over the grid
//    once per candidate distance, each draw inside a D3D9 occlusion query: how many blocks reach past each of 13 far levels (2^(-1/3)
//    apart below the shadow distance), how many start nearer than each of 7 near levels (doubling from near + 10), how many show sky.
//    The queries are read back frames later with GetData and no flush flag -- never a wait, no GetRenderTargetData, the same on native
//    D3D9 and through DXVK. Four sets of queries are in flight; when the GPU is further behind, a frame is simply not measured.
// 2. Choosing (Choose, from GetCascadeDepths at the start of a later frame's shadow pass). The far reach F is the nearest far level
//    that at most one block in 8192 goes past; the near bound the farthest near level no more than that start before. Candidate
//    layouts keep the first j cascades exactly as they are and re-split the rest over [start of cascade j, F] with the CascadeLambda
//    formula (and three other lambdas); j = 0 may also start at the near bound. A candidate is taken only if no visible depth gets a
//    cascade more than 5 % coarser than now (the sphere radius as GetCascadeViewProj builds it), some visible depths get at least 25 %
//    finer and all of them 3 % finer on average (log-depth weighted); the best one wins, else the normal splits. So the player's own
//    cascade can only get sharper or stay, and an open view to the horizon keeps its layout.
//    Air: something reads the shadow maps through the air along the view (the volumetric light and the fog volume on its grid, the
//    sandstorm). Its march starts at the eye, so the first cascade then keeps starting there; and when sky shows, the last cascade keeps
//    reaching the shadow distance for that air and only the first three are fitted.
// 3. Latency and stability. The measurement is 2-4 frames old, so it is only used while the camera has turned less than 3 degrees,
//    moved less than max(100 units, 5 % of F) and kept its FOV since it was taken; otherwise (and after a cell change or a load, or
//    when no measurement is newer than 30 frames) the normal splits come back that same frame. A layout that needs more coverage
//    (F farther, the near bound nearer, the air kept) is taken at once; one that needs less only after the measurement asked for it
//    8 frames in a row. Levels are quantized, so the cascades' sizes, and with them the texel snapping, stay put between changes, and
//    every change redraws all four cascades that frame (ShadowManager::ForceAllCascades), so the staggered / LimitFrequency updates
//    never mix cascades of two layouts.
// Casters: the cascades' cull volumes have no near plane (SetActivePlaneState(62)) and ShadowMap.vso pancakes casters onto the near
// plane, as always, so anything between the sun and what is seen still casts whatever the fit.

#include <cmath>
#include <algorithm>
#ifndef SUNDEPTHFIT_MATH_ONLY
#include "GpuProfiler.h"	// GpuTimer::Enabled: the F10 profile's statistics line
#include "RenderRect.h"		// UNOFFICIAL 2026-10-27 RenderScale v2: Measure reads only the drawn part of the depth
#include "DxvkCompute.h"
#endif

namespace SunDepthFit {
	// Settings ([Shaders.ShadowsExteriors.Main], ShadowsExteriorEffect::UpdateSettings), live.
	inline bool Enabled = false;	// FitToDepth
	inline int  DebugView = 0;		// FitToDepthDebug: 1 cascades coloured, 2 the measured blocks
	inline bool Log = false;		// FitToDepthLog

	constexpr int BlockSize = 8;		// screen pixels per block side (SunDepthFit.fx BlockPixels)
	constexpr int FarLevels = 13;		// maxZ * 2^(-k/3), k = 1..13, above FloorDepth
	constexpr int NearLevels = 7;		// minZ * 2^n, n = 1..7, below the far reach
	constexpr int Queries = 1 + FarLevels + NearLevels;	// [0] sky, [1..FarLevels] far, then near
	constexpr int RingSize = 4;
	constexpr float FloorDepth = 250.0f;	// the far reach is never fitted closer (about 3.5 m)
	constexpr float MaxCoarser = 1.05f;		// no visible depth's cascade more than this much coarser
	constexpr float MinLocalGain = 1.25f;	// a fit must make some visible depths at least this much finer...
	constexpr float MinGain = 1.03f;		// ...and the visible depths 3 % finer on average (log-depth weighted)
	constexpr int ShrinkFrames = 8;
	constexpr int MaxAge = 30;

	// ---- The pure maths (also built by the off-game check with SUNDEPTHFIT_MATH_ONLY defined) ----
	// GetCascadeViewProj's sphere radius for the slice [z1, z2] of the view (up to its common FOV factor and 1/16 rounding):
	// the far corners' distance from the slice's centroid. k2 = tan(half FOV x)^2 + tan(half FOV y)^2.
	inline float SliceRadius(float z1, float z2, float k2) {
		const float half = 0.5f * (z2 - z1);
		return sqrtf(z2 * z2 * k2 + half * half);
	}
	struct Layout { float Start = 0.0f; float S[4] = {}; };
	inline float RadiusAt(float z, const Layout& layout, float k2) {
		float from = layout.Start;
		for (int i = 0; i < 4; i++) {
			if (z < layout.S[i] || i == 3) return SliceRadius(from, layout.S[i], k2);
			from = layout.S[i];
		}
		return 1.0e30f;
	}
	inline void PracticalSplits(float lo, float hi, int count, float lambda, float* out) {
		const float ratio = hi / lo, range = hi - lo;
		for (int i = 0; i < count; i++) {
			const float p = (i + 1) / (float)count;
			const float lg = lo * powf(ratio, p), un = lo + range * p;
			out[i] = lambda * (lg - un) + un;
		}
		out[count - 1] = hi;
	}
	// Over the visible depths [lo, hi]: the worst ratio of the candidate's radius to the old one, the log-depth-weighted geometric
	// mean of old / candidate (above 1 = finer) and the best old / candidate anywhere.
	inline void Compare(const Layout& old, const Layout& fit, float lo, float hi, float k2, float& worst, float& gain, float& best) {
		float points[12];
		int count = 0;
		points[count++] = lo;
		points[count++] = hi;
		for (int i = 0; i < 4; i++) {
			if (old.S[i] > lo && old.S[i] < hi) points[count++] = old.S[i];
			if (fit.S[i] > lo && fit.S[i] < hi) points[count++] = fit.S[i];
		}
		std::sort(points, points + count);
		double sum = 0.0, total = 0.0;
		worst = 0.0f;
		best = 0.0f;
		for (int p = 0; p + 1 < count; p++) {
			const float a = points[p], b = points[p + 1];
			if (b <= a * 1.0001f) continue;
			const float z = sqrtf(a * b);
			const float before = RadiusAt(z, old, k2), after = RadiusAt(z, fit, k2);
			worst = (std::max)(worst, after / before);
			best = (std::max)(best, before / after);
			const double weight = log((double)b / a);
			sum += log((double)before / after) * weight;
			total += weight;
		}
		gain = total > 0.0 ? (float)exp(sum / total) : 1.0f;
	}

	struct Choice { bool Fitted = false; Layout Chosen; float Gain = 1.0f, Worst = 1.0f, Lambda = 0.0f; int Keep = -1; };
	// The candidates (see the top), each checked against the normal layout d over the visible depths [lo, reach]. keepAir: the last
	// cascade keeps reaching maxZ (sky with air readers); airReaders: the first cascade keeps starting at minZ.
	inline Choice Search(float minZ, float maxZ, float lambda, const float d[4], float reach, float lo, bool keepAir, bool airReaders, float k2) {
		Choice choice;
		choice.Lambda = lambda;
		Layout normal;
		normal.Start = minZ;
		for (int i = 0; i < 4; i++) normal.S[i] = d[i];
		const int last = keepAir ? 2 : 3;	// the last cascade that may be fitted
		const float lambdas[4] = { lambda, 0.5f, 0.75f, 1.0f };
		for (int j = 0; j <= last; j++) {
			if (j > 0 && d[j - 1] >= reach) break;	// the kept cascades already reach that far
			// The air readers march from the eye to every pixel: with them the first cascade keeps starting at the eye.
			const float from = j == 0 ? (airReaders ? minZ : (std::max)(minZ, lo)) : d[j - 1];
			if (reach <= from * 1.05f) continue;
			if (keepAir && reach >= maxZ * 0.95f) continue;
			for (int l = 0; l < 4; l++) {
				if (l > 0 && fabsf(lambdas[l] - lambda) < 0.01f) continue;
				Layout candidate;
				candidate.Start = j == 0 ? from : minZ;
				for (int i = 0; i < j; i++) candidate.S[i] = d[i];
				PracticalSplits(from, reach, last - j + 1, lambdas[l], &candidate.S[j]);
				if (keepAir) candidate.S[3] = maxZ;
				bool increasing = candidate.S[0] > candidate.Start * 1.01f;
				for (int i = 1; i < 4; i++) increasing = increasing && candidate.S[i] > candidate.S[i - 1] * 1.001f;
				if (!increasing) continue;
				float worst, gain, best;
				Compare(normal, candidate, lo, reach, k2, worst, gain, best);
				if (worst <= MaxCoarser && best >= MinLocalGain && gain >= MinGain && gain > choice.Gain) {
					choice.Fitted = true; choice.Chosen = candidate; choice.Gain = gain; choice.Worst = worst; choice.Keep = j; choice.Lambda = lambdas[l];
				}
			}
		}
		return choice;
	}

#ifndef SUNDEPTHFIT_MATH_ONLY

	// Shader constants (TESR_SunDepthFit*, registered by ShadowsExteriorEffect::RegisterConstants).
	inline D3DXVECTOR4 Data(0.0f, 0.0f, 0.0f, 0.0f);
	inline D3DXVECTOR4 Info(0.0f, 0.0f, 0.0f, 0.0f);
	inline D3DXVECTOR4 Splits(0.0f, 0.0f, 0.0f, 0.0f);
	inline D3DXVECTOR4 Original(0.0f, 0.0f, 0.0f, 0.0f);

	// Constants.ShadowMapRadius.w without the fit (GetCascadeDepths): TESR_ShadowFade.w, the outdoor lamp shadows' reach, keeps it.
	inline float UnfittedRadiusW = 0.0f;
	inline float CurrentMinZ = 0.0f, CurrentMaxZ = 0.0f;	// the unfitted range of this frame (GetCascadeDepths), for the levels

	struct Pose { D3DXVECTOR3 Position = D3DXVECTOR3(0, 0, 0), Forward = D3DXVECTOR3(1, 0, 0); float Fov = 0.0f; };
	inline Pose CameraPose() {
		Pose pose;
		NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
		if (!camera) return pose;
		const NiMatrix33& r = camera->m_worldTransform.rot;
		pose.Position = D3DXVECTOR3(camera->m_worldTransform.pos.x, camera->m_worldTransform.pos.y, camera->m_worldTransform.pos.z);
		pose.Forward = D3DXVECTOR3(r.data[0][0], r.data[1][0], r.data[2][0]);
		D3DXVec3Normalize(&pose.Forward, &pose.Forward);
		pose.Fov = WorldSceneGraph->cameraFOV;
		return pose;
	}

	struct QuerySet {
		IDirect3DQuery9* Query[Queries] = {};
		ID3D9NvrExtBuffer* ReadbackBuf = nullptr;
		bool UseReadback = false;
		float Level[Queries] = {};	// the distance each query counted against (0 for the sky)
		int FarCount = 0, NearCount = 0;
		bool Pending = false;
		unsigned Frame = 0, Order = 0;
		Pose Camera;
		float MinZ = 0.0f, MaxZ = 0.0f;
		UINT Blocks = 0;
	};
	inline QuerySet Sets[RingSize];
	inline unsigned IssueOrder = 0;
	inline ID3D9NvrExtReadback* Readback = nullptr;
	inline bool ReadbackTried = false;
	inline ID3D9NvrExtPipeline* ComputePipeline = nullptr;
	inline ID3D9NvrExtBuffer* ComputeUbo = nullptr;
	inline ID3D9NvrExtBuffer* CountsGpu = nullptr;
	inline bool ComputeTried = false, ComputeFailed = false;
	inline EffectRecord::ExtPass ViewExtPass{};
	inline EffectRecord::ExtUniformBuffer ViewExtUbo{};

	struct Result {
		bool Valid = false;
		int FarLevel = 0;		// 0 = the shadow distance itself, k = FarLevelDepth(k)
		int NearLevel = 0;		// 0 = near + 10, n = minZ * 2^n
		bool Sky = true;
		unsigned Frame = 0;
		Pose Camera;
		float MinZ = 0.0f, MaxZ = 0.0f;
		DWORD Counts[Queries] = {};
		UINT Blocks = 0;
	};
	inline Result Latest;

	inline IDirect3DTexture9* BlockTexture = nullptr;
	inline IDirect3DSurface9* BlockSurface = nullptr;
	inline IDirect3DTexture9* ScratchTexture = nullptr;
	inline IDirect3DSurface9* ScratchSurface = nullptr;
	inline UINT GridW = 0, GridH = 0;
	inline bool ResourcesFailed = false;
	inline bool BlocksValid = false;	// the block grid holds this session's measurement (the debug view's mode 2)

	// The layout in use (Choose).
	inline unsigned Frame = 0;
	inline int AppliedFar = 0, AppliedNear = 0;
	inline bool AppliedAir = true;
	inline int HoldFrames = 0, HoldFar = 0, HoldNear = 0;
	inline bool HoldAir = true;
	inline float LastSplits[4] = {}, LastStart = 0.0f;
	inline bool LastFitted = false;

	struct Stats { unsigned Frames = 0, Fitted = 0, NoGain = 0, Moving = 0, NoMeasure = 0; double FarSum = 0.0, GainSum = 0.0; };
	inline Stats Window;

	inline float FarLevelDepth(float maxZ, int k) { return maxZ * exp2f(-k / 3.0f); }
	inline float NearLevelDepth(float minZ, int n) { return minZ * exp2f((float)n); }

	inline void ReleaseResources() {
		for (QuerySet& set : Sets) {
			for (IDirect3DQuery9*& query : set.Query) if (query) { query->Release(); query = nullptr; }
			if (set.ReadbackBuf) { set.ReadbackBuf->Release(); set.ReadbackBuf = nullptr; }
			set.UseReadback = false;
			set.Pending = false;
		}
		if (CountsGpu) { CountsGpu->Release(); CountsGpu = nullptr; }
		if (ComputeUbo) { ComputeUbo->Release(); ComputeUbo = nullptr; }
		if (ComputePipeline) { ComputePipeline->Release(); ComputePipeline = nullptr; }
		ComputeTried = false;
		ComputeFailed = false;
		if (Readback) { Readback->Release(); Readback = nullptr; }
		ReadbackTried = false;
		if (BlockSurface) { BlockSurface->Release(); BlockSurface = nullptr; }
		if (BlockTexture) { BlockTexture->Release(); BlockTexture = nullptr; }
		if (ScratchSurface) { ScratchSurface->Release(); ScratchSurface = nullptr; }
		if (ScratchTexture) { ScratchTexture->Release(); ScratchTexture = nullptr; }
		BlocksValid = false;
		Latest.Valid = false;
	}

	// The effect file: not one of ShaderManager's registered effects (no menu switch, never reloaded by it); made on first use.
	class FitEffect : public EffectRecord {
	public:
		FitEffect() : EffectRecord("SunDepthFit") {}
		bool ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override {
			if (techniqueIndex != 2 || ClearRenderTarget || !DxvkCompute::Available()) return false;
			auto findTex = [](const char* name) -> IDirect3DBaseTexture9* {
				auto it = TheTextureManager->TextureNames.find(name);
				return (it != TheTextureManager->TextureNames.end() && it->second) ? *it->second : nullptr;
			};
			IDirect3DBaseTexture9* rendered = TheTextureManager->RenderedTexture;
			IDirect3DBaseTexture9* depth = findTex("TESR_DepthBuffer");
			IDirect3DBaseTexture9* blocks = BlockTexture ? static_cast<IDirect3DBaseTexture9*>(BlockTexture) : depth;
			if (!rendered || !depth || !blocks) return false;

			struct ViewUniforms {
				float viewTransform[16];
				float projectionTransform[16];
				float reciprocalResolution[4];
				float cameraData[4];
				float info[4];
				float splits[4];
				float original[4];
				float shadowNearCenter[4];
				float shadowMiddleCenter[4];
				float shadowFarCenter[4];
				float shadowLodCenter[4];
				float shadowForwardData[4];
			} u = {};
			auto copyReq = [&](const char* name, void* dst, size_t bytes) -> bool {
				auto it = TheShaderManager->ConstantsTable.find(name);
				if (it != TheShaderManager->ConstantsTable.end() && it->second) { memcpy(dst, it->second, bytes); return true; }
				return false;
			};
			if (!copyReq("TESR_ViewTransform", u.viewTransform, sizeof(u.viewTransform)) ||
				!copyReq("TESR_ProjectionTransform", u.projectionTransform, sizeof(u.projectionTransform)) ||
				!copyReq("TESR_ReciprocalResolution", u.reciprocalResolution, sizeof(u.reciprocalResolution)) ||
				!copyReq("TESR_CameraData", u.cameraData, sizeof(u.cameraData)) ||
				!copyReq("TESR_ShadowNearCenter", u.shadowNearCenter, sizeof(u.shadowNearCenter)) ||
				!copyReq("TESR_ShadowMiddleCenter", u.shadowMiddleCenter, sizeof(u.shadowMiddleCenter)) ||
				!copyReq("TESR_ShadowFarCenter", u.shadowFarCenter, sizeof(u.shadowFarCenter)) ||
				!copyReq("TESR_ShadowLodCenter", u.shadowLodCenter, sizeof(u.shadowLodCenter)) ||
				!copyReq("TESR_ShadowForwardData", u.shadowForwardData, sizeof(u.shadowForwardData))) return false;
			memcpy(u.info, &Info, sizeof(u.info));
			memcpy(u.splits, &Splits, sizeof(u.splits));
			memcpy(u.original, &Original, sizeof(u.original));

			ID3D9NvrExtBuffer* cb = ExtUniforms(ViewExtUbo, &u, sizeof(u));
			ID3D9NvrExtSampler* point = DxvkCompute::PointClamp();
			if (!cb || !point) return false;

			struct Push { UInt32 pointSampler; UInt32 pad[3]; } push = {};
			if (!ViewExtPass.Tried) {
				ViewExtPass.Tried = true;
				NVR_EXT_BINDING_DESC bindings[5] = {};
				for (UINT i = 0; i < 3; i++) {
					bindings[i].Set = 1; bindings[i].Binding = i; bindings[i].Type = NVR_EXT_BIND_SAMPLED_IMAGE; bindings[i].Slot = i; bindings[i].ViewType = NVR_EXT_VIEW_2D;
				}
				bindings[3].Set = 2; bindings[3].Binding = 0; bindings[3].Type = NVR_EXT_BIND_UNIFORM_BUFFER; bindings[3].Slot = 0; bindings[3].ViewType = NVR_EXT_VIEW_2D;
				bindings[4].Type = NVR_EXT_BIND_SAMPLER; bindings[4].Slot = 0; bindings[4].PushOffset = 0;
				ViewExtPass.Pipeline = DxvkCompute::CreateGraphicsPipeline("SunDepthFitCascadeView", bindings, _countof(bindings), sizeof(push), true);
			}
			if (!ViewExtPass.Pipeline) return false;
			NVR_EXT_RESOURCE_BINDING res[5] = {};
			res[0].Slot = 0; res[0].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[0].pResource = rendered;
			res[1].Slot = 1; res[1].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[1].pResource = depth;
			res[2].Slot = 2; res[2].Type = NVR_EXT_BIND_SAMPLED_IMAGE; res[2].pResource = blocks;
			res[3].Slot = 0; res[3].Type = NVR_EXT_BIND_UNIFORM_BUFFER; res[3].pResource = cb;
			res[4].Slot = 0; res[4].Type = NVR_EXT_BIND_SAMPLER; res[4].pResource = point;
			return ExtFullScreen(Device, RenderTarget, RenderedSurface, SourceBuffer, ViewExtPass.Pipeline, res, _countof(res), &push, sizeof(push), true);
		}
	};
	inline FitEffect* FitRecord = nullptr;
	inline bool EffectTried = false;

	inline bool EnsureEffect() {
		if (FitRecord && (FitRecord->Effect || (DxvkCompute::PassesOn() && !DxvkCompute::KeptOnD3D9("SunDepthFit")))) {
			FitRecord->Enabled = true;
			return true;
		}
		if (EffectTried) return false;
		EffectTried = true;
		if (!FitRecord) FitRecord = new FitEffect();
		if (DxvkCompute::PassesOn() && !DxvkCompute::KeptOnD3D9("SunDepthFit")) {
			FitRecord->Enabled = true;
			return true;
		}
		FitRecord->LoadEffect();
		ID3DXEffect* fx = FitRecord->Effect;
		// By name, so an older or edited file can never draw some other pass into the block grid.
		if (!fx || fx->GetTechniqueByName("Reduce") != fx->GetTechnique(0) || fx->GetTechniqueByName("Count") != fx->GetTechnique(1) ||
			fx->GetTechniqueByName("CascadeView") != fx->GetTechnique(2) || !fx->GetTechnique(2)) {
			Logger::Log("UNOFFICIAL FitToDepth: Effects\\SunDepthFit.fx.hlsl %s: the sun cascades keep their normal splits.",
				fx ? "does not have the Reduce, Count and CascadeView techniques" : "could not be loaded (missing, or see the compile error above)");
			if (fx) FitRecord->DisposeEffect();
			return false;
		}
		FitRecord->Enabled = true;
		return true;
	}

	inline bool EnsureCompute() {
		if (ComputeFailed || !DxvkCompute::Available()) return false;
		if (!Readback && !ReadbackTried) {
			ReadbackTried = true;
			if (FAILED(DxvkCompute::Extension()->QueryInterface(__uuidof(ID3D9NvrExtReadback), (void**)&Readback))) Readback = nullptr;
		}
		if (!Readback) return false;
		if (ComputePipeline && ComputeUbo && CountsGpu) return true;
		if (ComputeTried) return false;
		ComputeTried = true;
		static const NVR_EXT_BINDING_DESC kBindings[] = {
			{ 1, 0, NVR_EXT_BIND_SAMPLED_IMAGE,  0, NVR_EXT_VIEW_2D,    FALSE, 0 },
			{ 1, 1, NVR_EXT_BIND_STORAGE_IMAGE,  1, NVR_EXT_VIEW_2D,    TRUE,  0 },
			{ 1, 2, NVR_EXT_BIND_STORAGE_BUFFER, 2, NVR_EXT_VIEW_2D,    TRUE,  0 },
			{ 2, 0, NVR_EXT_BIND_UNIFORM_BUFFER, 0, NVR_EXT_VIEW_2D,    FALSE, 0 },
		};
		ComputePipeline = DxvkCompute::CreatePipeline("SunDepthFitReduce", kBindings, _countof(kBindings), 0, false);
		if (ComputePipeline) ComputeUbo = DxvkCompute::CreateBuffer(16 + 16 + 6 * 16, NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "SunDepthFit UBO");
		if (ComputeUbo) CountsGpu = DxvkCompute::CreateBuffer(Queries * sizeof(DWORD), NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_SRC | NVR_EXT_BUFFER_TRANSFER_DST, "SunDepthFit Counts");
		if (!ComputePipeline || !ComputeUbo || !CountsGpu) {
			if (CountsGpu) { CountsGpu->Release(); CountsGpu = nullptr; }
			if (ComputeUbo) { ComputeUbo->Release(); ComputeUbo = nullptr; }
			if (ComputePipeline) { ComputePipeline->Release(); ComputePipeline = nullptr; }
			ComputeFailed = true;
			return false;
		}
		for (QuerySet& set : Sets) {
			if (!set.ReadbackBuf && FAILED(Readback->CreateReadbackBuffer(Queries * sizeof(DWORD), "SunDepthFit Readback", &set.ReadbackBuf))) {
				ComputeFailed = true;
				return false;
			}
		}
		Logger::Log("UNOFFICIAL FitToDepth: Vulkan compute reduce + async readback ready (Compute\\SunDepthFitReduce.spv)");
		return true;
	}

	inline bool EnsureResources(IDirect3DDevice9* device) {
		const UINT w = (TheRenderManager->width + BlockSize - 1) / BlockSize, h = (TheRenderManager->height + BlockSize - 1) / BlockSize;
		const bool canCompute = DxvkCompute::PassesOnRect() && !DxvkCompute::KeptOnD3D9("SunDepthFit") && EnsureCompute();
		if (w == GridW && h == GridH && (BlockTexture || ResourcesFailed)) {
			if (canCompute) return BlockTexture != nullptr;
			if (ScratchTexture) return BlockTexture != nullptr;
		}
		ReleaseResources();
		GridW = w; GridH = h;
		ResourcesFailed = true;
		HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &BlockTexture, NULL);
		if (SUCCEEDED(hr)) hr = BlockTexture->GetSurfaceLevel(0, &BlockSurface);
		if (SUCCEEDED(hr) && EnsureCompute()) {
			ResourcesFailed = false;
			return true;
		}
		if (SUCCEEDED(hr)) hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ScratchTexture, NULL);
		if (SUCCEEDED(hr)) hr = ScratchTexture->GetSurfaceLevel(0, &ScratchSurface);
		for (QuerySet& set : Sets)
			for (IDirect3DQuery9*& query : set.Query)
				if (SUCCEEDED(hr)) hr = device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
		if (FAILED(hr)) {
			Logger::Log("UNOFFICIAL FitToDepth: the block grid (%ux%u) or its occlusion queries could not be made (%08lX): the sun cascades keep their normal splits.", w, h, hr);
			ReleaseResources();
			return false;
		}
		ResourcesFailed = false;
		Logger::Log("UNOFFICIAL FitToDepth: measuring the visible depth on a %ux%u grid of %dx%d pixel blocks, %d occlusion queries a frame, read back without waiting.",
			w, h, BlockSize, BlockSize, Queries);
		return true;
	}

	// Results of earlier frames' queries, oldest first; never waits (GetData without D3DGETDATA_FLUSH).
	inline void Collect() {
		for (;;) {
			QuerySet* oldest = nullptr;
			for (QuerySet& set : Sets) if (set.Pending && (!oldest || set.Order < oldest->Order)) oldest = &set;
			if (!oldest) return;
			Result r;
			const int used = 1 + oldest->FarCount + oldest->NearCount;
			bool failed = false;
			if (oldest->UseReadback) {
				if (!Readback || !oldest->ReadbackBuf) { oldest->Pending = false; continue; }
				const HRESULT hr = Readback->TryReadback(oldest->ReadbackBuf, 0, Queries * sizeof(DWORD), r.Counts);
				if (hr == S_FALSE) return;
				if (FAILED(hr)) failed = true;
			}
			else {
				for (int q = 0; q < used; q++) {
					const HRESULT hr = oldest->Query[q]->GetData(&r.Counts[q], sizeof(DWORD), 0);
					if (hr == S_FALSE) return;	// not there yet: the later sets are not either
					if (FAILED(hr)) { failed = true; break; }
				}
			}
			oldest->Pending = false;
			if (failed) continue;
			const DWORD allowed = oldest->Blocks / 8192;
			r.Sky = r.Counts[0] > allowed;
			for (int k = 1; k <= oldest->FarCount && r.Counts[k] <= allowed; k++) r.FarLevel = k;
			for (int n = 1; n <= oldest->NearCount && r.Counts[oldest->FarCount + n] <= allowed; n++) r.NearLevel = n;
			r.Valid = true;
			r.Frame = oldest->Frame;
			r.Camera = oldest->Camera;
			r.MinZ = oldest->MinZ; r.MaxZ = oldest->MaxZ;
			r.Blocks = oldest->Blocks;
			Latest = r;
		}
	}

	inline bool Wanted() { return (Enabled || DebugView == 2) && TheShaderManager->GameState.isExterior; }

	// The measuring passes (ShaderManager::RenderEffectsPreTonemapping, after CombineDepth; the frame quad's stream is set).
	inline void Measure(IDirect3DDevice9* device) {
		if (!Wanted() || CurrentMaxZ <= 0.0f) return;
		ShadowsExteriorEffect* shadows = TheShaderManager->Effects.ShadowsExteriors;
		if (!shadows || !shadows->Enabled || !shadows->Settings.Exteriors.Enabled) return;
		if (!EnsureResources(device)) return;
		Collect();
		QuerySet* set = nullptr;
		for (QuerySet& s : Sets) if (!s.Pending) { set = &s; break; }
		if (!set) return;	// the GPU is more than the ring behind: no measurement this frame

		set->MinZ = CurrentMinZ; set->MaxZ = CurrentMaxZ;
		set->FarCount = 0; set->NearCount = 0;
		set->Level[0] = 0.0f;
		for (int k = 1; k <= FarLevels; k++) {
			const float level = FarLevelDepth(set->MaxZ, k);
			if (level < (std::max)(FloorDepth, 8.0f * set->MinZ)) break;
			set->Level[++set->FarCount] = level;
		}
		for (int n = 1; n <= NearLevels; n++) {
			const float level = NearLevelDepth(set->MinZ, n);
			if (level >= set->MaxZ * 0.5f) break;
			set->NearCount++;
			set->Level[set->FarCount + set->NearCount] = level;
		}
		// UNOFFICIAL 2026-10-27 RenderScale v2: in rect frames only the grid's drawn part holds blocks (ApplyViewport below).
		const UINT activeW = RenderRect::Width(GridW), activeH = RenderRect::Height(GridH);
		set->Blocks = activeW * activeH;

		if (DxvkCompute::PassesOnRect() && !DxvkCompute::KeptOnD3D9("SunDepthFit") &&
			Readback && ComputePipeline && ComputeUbo && CountsGpu && set->ReadbackBuf && BlockSurface) {
			auto depthIt = TheTextureManager->TextureNames.find("TESR_DepthBuffer");
			IDirect3DBaseTexture9* depth = (depthIt != TheTextureManager->TextureNames.end() && depthIt->second) ? *depthIt->second : nullptr;
			auto camIt = TheShaderManager->ConstantsTable.find("TESR_CameraData");
			const float farZ = (camIt != TheShaderManager->ConstantsTable.end() && camIt->second) ? camIt->second->y : 0.0f;
			if (depth && farZ > 0.0f) {
				struct ReduceUbo {
					int dims[4];
					float meta[4];
					float levels[6][4];
				} u = {};
				const UINT renderW = RenderRect::Width((UINT)TheRenderManager->width);
				const UINT renderH = RenderRect::Height((UINT)TheRenderManager->height);
				const int total = 1 + set->FarCount + set->NearCount;
				u.dims[0] = (int)activeW; u.dims[1] = (int)activeH; u.dims[2] = (int)renderW; u.dims[3] = (int)renderH;
				u.meta[0] = farZ; u.meta[1] = (float)set->FarCount; u.meta[2] = (float)set->NearCount; u.meta[3] = (float)total;
				for (int q = 0; q < total; q++) u.levels[q >> 2][q & 3] = set->Level[q];

				ID3D9NvrExt* ext = DxvkCompute::Extension();
				NVR_EXT_RESOURCE_BINDING bindings[] = {
					{ 0, NVR_EXT_BIND_SAMPLED_IMAGE,  depth,      0, 0, 0,         FALSE },
					{ 1, NVR_EXT_BIND_STORAGE_IMAGE,  BlockSurface, 0, 0, 0,       FALSE },
					{ 2, NVR_EXT_BIND_STORAGE_BUFFER, CountsGpu,  0, 0, Queries * sizeof(DWORD), FALSE },
					{ 0, NVR_EXT_BIND_UNIFORM_BUFFER, ComputeUbo, 0, 0, sizeof(u), FALSE },
				};
				NVR_EXT_DISPATCH_DESC dispatch = {};
				dispatch.pPipeline = ComputePipeline;
				dispatch.BindingCount = _countof(bindings);
				dispatch.pBindings = bindings;
				dispatch.GroupsX = (activeW + 7u) / 8u;
				dispatch.GroupsY = (activeH + 7u) / 8u;
				dispatch.GroupsZ = 1u;

				static GpuTimer computeTimer("SunDepthFit reduce (compute)");
				GpuProfileScope gpu(computeTimer, device);
				if (SUCCEEDED(ext->ClearBuffer(CountsGpu, 0, Queries * sizeof(DWORD), 0)) &&
					SUCCEEDED(ext->UpdateBuffer(ComputeUbo, 0, sizeof(u), &u)) &&
					SUCCEEDED(ext->Dispatch(&dispatch)) &&
					SUCCEEDED(Readback->CopyToReadback(set->ReadbackBuf, 0, CountsGpu, 0, Queries * sizeof(DWORD)))) {
					BlocksValid = true;
					set->UseReadback = true;
					set->Pending = true;
					set->Frame = Frame;
					set->Order = ++IssueOrder;
					set->Camera = CameraPose();
					return;
				}
			}
		}

		set->UseReadback = false;
		if (!EnsureEffect() || !ScratchSurface) return;
		if (!FitRecord->Effect) return;

		ID3DXEffect* fx = FitRecord->Effect;
		auto* renderState = TheRenderManager->renderState;
		IDirect3DSurface9* targetWas = nullptr;
		IDirect3DSurface9* depthWas = nullptr;
		device->GetRenderTarget(0, &targetWas);
		device->GetDepthStencilSurface(&depthWas);
		device->SetDepthStencilSurface(nullptr);	// no depth test (the passes switch it off too); a multisampled one could not pair with the grid

		bool ok = false;
		UINT passes = 0;
		device->SetRenderTarget(0, BlockSurface);
		fx->SetTechnique(fx->GetTechnique(0));
		FitRecord->SetCT();
		if (SUCCEEDED(fx->Begin(&passes, 0))) {
			if (passes > 0 && SUCCEEDED(fx->BeginPass(0))) {
				RenderRect::ApplyViewport(device);	// UNOFFICIAL 2026-10-27 RenderScale v2: the grid is screen-shaped: only its drawn part
				ok = SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
				fx->EndPass();
			}
			fx->End();
		}
		if (ok) {
			ok = false;
			device->SetRenderTarget(0, ScratchSurface);
			fx->SetTechnique(fx->GetTechnique(1));
			Data = D3DXVECTOR4(0.0f, 1.0f, 1.0f / GridW, 1.0f / GridH);
			FitRecord->SetCT();
			D3DXHANDLE data = fx->GetParameterByName(NULL, "TESR_SunDepthFitData");
			if (data && SUCCEEDED(fx->Begin(&passes, 0))) {
				if (passes > 0 && SUCCEEDED(fx->BeginPass(0))) {
					device->SetTexture(2, BlockTexture);
					renderState->NoteDeviceTexture(2, BlockTexture);
					ok = true;
					RenderRect::ApplyViewport(device);	// UNOFFICIAL 2026-10-27 RenderScale v2: count only the grid's drawn part
					const int used = 1 + set->FarCount + set->NearCount;
					for (int q = 0; q < used && ok; q++) {
						const float mode = q == 0 ? 1.0f : q <= set->FarCount ? 0.0f : 2.0f;
						Data = D3DXVECTOR4(set->Level[q], mode, 1.0f / GridW, 1.0f / GridH);
						fx->SetVector(data, &Data);
						fx->CommitChanges();
						ok = SUCCEEDED(set->Query[q]->Issue(D3DISSUE_BEGIN)) && SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)) &&
							SUCCEEDED(set->Query[q]->Issue(D3DISSUE_END));
					}
					fx->EndPass();
					device->SetTexture(2, nullptr);
					renderState->NoteDeviceTexture(2, nullptr);
				}
				fx->End();
			}
		}
		device->SetRenderTarget(0, targetWas);
		device->SetDepthStencilSurface(depthWas);
		RenderRect::ApplyViewport(device);	// UNOFFICIAL 2026-10-27 RenderScale v2: viewport and quad consistent with the restored target
		if (targetWas) targetWas->Release();
		if (depthWas) depthWas->Release();
		if (!ok) {
			static bool said = false;
			if (!said) Logger::Log("UNOFFICIAL FitToDepth: a measuring pass failed; that frame is not measured.");
			said = true;
			return;
		}
		BlocksValid = true;
		set->Pending = true;
		set->Frame = Frame;
		set->Order = ++IssueOrder;
		set->Camera = CameraPose();
	}

	inline void Reset() {
		Latest.Valid = false;
		for (QuerySet& set : Sets) set.Pending = false;	// their answers belong to another place
		AppliedFar = 0; AppliedNear = 0; AppliedAir = true; HoldFrames = 0;
	}

	// GetCascadeDepths: d = the normal split depths of [minZ, maxZ] (view depth); fills fit (and its Near start) with the layout to
	// use. Returns true when that differs from the normal one. Sets ShadowManager::ForceAllCascades when the layout changed.
	inline bool Choose(float minZ, float maxZ, float lambda, const float d[4], float fit[4], float& start, bool airReaders) {
		Frame++;
		CurrentMinZ = minZ; CurrentMaxZ = maxZ;
		for (int i = 0; i < 4; i++) fit[i] = d[i];
		start = minZ;
		Original = D3DXVECTOR4(d[0], d[1], d[2], d[3]);
		Splits = Original;
		Info = D3DXVECTOR4((float)DebugView, d[3], d[3], minZ);

		const bool exterior = TheShaderManager->GameState.isExterior;
		if (!Enabled || !exterior || TheShaderManager->GameState.isCameraCut) Reset();
		else if (BlockTexture) Collect();	// answers that came in since the last measuring pass
		bool fitted = false;
		float chosenGain = 1.0f, chosenWorst = 1.0f, chosenLambda = lambda;
		int chosenKeep = -1;
		const char* why = "";
		if (Enabled && exterior) {
			// What the latest measurement asks for, if it still describes this view.
			int wantFar = 0, wantNear = 0;
			bool sky = true;
			why = "no measurement yet";
			if (Latest.Valid) {
				const Pose now = CameraPose();
				const D3DXVECTOR3 moved = now.Position - Latest.Camera.Position;
				const float reach = Latest.FarLevel ? FarLevelDepth(maxZ, Latest.FarLevel) : maxZ;
				const float turned = D3DXVec3Dot(&now.Forward, &Latest.Camera.Forward);
				if (Latest.MaxZ != maxZ || Latest.MinZ != minZ) why = "the shadow distance or the near plane changed";
				else if (Frame - Latest.Frame > (unsigned)MaxAge) why = "no recent measurement";
				else if (turned < cosf(3.0f * 3.14159265f / 180.0f)) why = "camera turning";
				else if (D3DXVec3Length(&moved) > (std::max)(100.0f, 0.05f * reach)) why = "camera moving";
				else if (fabsf(now.Fov - Latest.Camera.Fov) > 1.0f) why = "zoom";
				else { wantFar = Latest.FarLevel; wantNear = Latest.NearLevel; sky = Latest.Sky; why = ""; }
			}
			const bool wantAir = airReaders && sky;
			// More coverage at once; less only once asked for ShrinkFrames frames in a row (the least asked for in that time).
			if (wantFar < AppliedFar || wantNear < AppliedNear || (wantAir && !AppliedAir)) {
				AppliedFar = (std::min)(AppliedFar, wantFar);
				AppliedNear = (std::min)(AppliedNear, wantNear);
				AppliedAir = AppliedAir || wantAir;
				HoldFrames = 0;
			}
			if (wantFar != AppliedFar || wantNear != AppliedNear || wantAir != AppliedAir) {
				if (HoldFrames == 0) { HoldFar = wantFar; HoldNear = wantNear; HoldAir = wantAir; }
				HoldFar = (std::min)(HoldFar, wantFar); HoldNear = (std::min)(HoldNear, wantNear); HoldAir = HoldAir || wantAir;
				if (++HoldFrames >= ShrinkFrames) {
					AppliedFar = HoldFar; AppliedNear = HoldNear; AppliedAir = HoldAir;
					HoldFrames = 0;
				}
			}
			else HoldFrames = 0;

			const float reach = AppliedFar ? FarLevelDepth(maxZ, AppliedFar) : maxZ;
			const float lo = AppliedNear ? (std::min)(NearLevelDepth(minZ, AppliedNear), reach * 0.5f) : minZ;
			Info.y = reach;
			Info.w = lo;
			if ((AppliedFar || AppliedNear) && reach > lo * 1.05f) {
				const D3DXMATRIX& proj = TheRenderManager->projMatrix;
				const float tx = proj._11 != 0.0f ? 1.0f / proj._11 : 1.0f, ty = proj._22 != 0.0f ? 1.0f / proj._22 : 0.5625f;
				const Choice choice = Search(minZ, maxZ, lambda, d, reach, lo, AppliedAir, airReaders, tx * tx + ty * ty);
				if (choice.Fitted) {
					fitted = true;
					for (int i = 0; i < 4; i++) fit[i] = choice.Chosen.S[i];
					start = choice.Chosen.Start;
					why = "";
					chosenGain = choice.Gain; chosenWorst = choice.Worst; chosenKeep = choice.Keep; chosenLambda = choice.Lambda;
				}
				else if (!*why) why = "no layout finer without a coarser spot";
			}
			else if (!*why) why = "the whole range is in view";
			Splits = D3DXVECTOR4(fit[0], fit[1], fit[2], fit[3]);
			Info.y = fitted ? (AppliedAir ? fit[2] : fit[3]) : d[3];

			// The F10 profile's line: how often the fit was in use, and why not.
			if (GpuTimer::Enabled) {
				Window.Frames++;
				if (fitted) { Window.Fitted++; Window.FarSum += reach; Window.GainSum += chosenGain; }
				else if (!strcmp(why, "camera turning") || !strcmp(why, "camera moving") || !strcmp(why, "zoom")) Window.Moving++;
				else if (!strcmp(why, "no layout finer without a coarser spot") || !strcmp(why, "the whole range is in view")) Window.NoGain++;
				else Window.NoMeasure++;
				if (Window.Frames >= 240) {
					Logger::Log("SUN DEPTH FIT STATS: %u frames, fitted in %u (mean far reach %.0f, %.2fx finer on average), unfitted: %u camera moving, %u nothing to gain, %u no measurement",
						Window.Frames, Window.Fitted, Window.Fitted ? Window.FarSum / Window.Fitted : 0.0, Window.Fitted ? Window.GainSum / Window.Fitted : 1.0,
						Window.Moving, Window.NoGain, Window.NoMeasure);
					Window = Stats();
				}
			}
		}

		bool changed = fitted != LastFitted || start != LastStart;
		for (int i = 0; i < 4; i++) changed = changed || fit[i] != LastSplits[i];
		if (changed && (fitted || LastFitted)) {
			TheShadowManager->ForceAllCascades = true;	// all four cascades with the new spheres in the same frame
			if (Log)
				Logger::Log("SUN DEPTH FIT: %s | splits %.0f %.0f %.0f %.0f from %.0f (normal %.0f %.0f %.0f %.0f from %.0f) | visible %.0f..%.0f%s%s | kept %d, lambda %.2f, %.2fx finer, worst %.3f%s%s",
					fitted ? "FITTED" : "normal", fit[0], fit[1], fit[2], fit[3], start, d[0], d[1], d[2], d[3], minZ, Info.w, Info.y,
					Latest.Sky ? ", sky" : "", AppliedAir ? ", last cascade kept for the air" : "", chosenKeep, chosenLambda, chosenGain, chosenWorst,
					*why ? " | " : "", why);
		}
		LastFitted = fitted;
		LastStart = start;
		for (int i = 0; i < 4; i++) LastSplits[i] = fit[i];
		return fitted;
	}

	// The debug view (FitToDepthDebug), in the post chain before the Debug effect.
	inline void RenderView(IDirect3DDevice9* device, IDirect3DSurface9* renderTarget) {
		if (DebugView <= 0 || !TheShaderManager->GameState.isExterior) return;
		if (!EnsureEffect()) return;
		Info.x = (DebugView == 2 && BlockTexture && BlocksValid) ? 2.0f : 1.0f;
		if (Info.x > 1.5f) FitRecord->SetPassTexture(2, BlockTexture);
		FitRecord->writesWholeTarget = true;	// every pixel written: no pre-fill copy
		FitRecord->Render(device, renderTarget, TheTextureManager->RenderedSurface, 2, false, nullptr);
		FitRecord->writesWholeTarget = false;
		FitRecord->ClearPassTextures();
	}
#endif	// SUNDEPTHFIT_MATH_ONLY
}
