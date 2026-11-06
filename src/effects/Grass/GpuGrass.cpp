#include "GpuGrass.h"
#include <cmath>
#include <vector>
#include "../../core/DxvkCompute.h"
#include "../../core/GpuProfiler.h"
#include "../../core/NewGrass.h"	// the card's model (height, width, sway) and colour ramp / silhouette, read once per grass model
#include "../../core/GrassWind.h"	// [Shaders.Grass.Wind]: the field wind's lanes and clock (the card shaders get the same)

// UNOFFICIAL 2026-10-27 GPU grass: see GpuGrass.h. Shaders: src\hlsl\NewVegas\Compute\GpuGrass* (.glsl, Cull.comp, .vert, .frag).

namespace GpuGrass {

namespace {
	struct SettingsStruct {
		bool	Enabled = false;
		bool	KeepCards = false;			// true = the game's card stays and the strands are added round it (NewGrass's hybrid)
		float	BladeDistance = 7000.0f;	// pieces whose nearest tuft is farther stay the game's cards (a whole piece at a time)
		float	Density = 1.0f;				// clumps per game tuft (1 = the game's own tufts; more = extra clumps of the same plant beside each)
		float	ExtraRadius = 60.0f;		// how far from its tuft an extra clump may stand (units)
		int		Blades = 64;				// strands per clump up to LodNear
		int		BladesFar = 8;				// strands per clump from LodFar on
		float	LodNear = 600.0f, LodFar = 2500.0f;
		float	DetailDistance = 1200.0f;	// strands nearer than this have 5 triangles (a curve), farther ones 1
		float	Coverage = 1.0f;			// the strands of a clump together cover this many times the card's opaque area
		float	MinPixelWidth = 0.75f;		// no strand narrower than this many pixels (far strands widen instead of shimmering)
		float	HeightScale = 0.9f, LengthMin = 0.4f, TipReach = 1.0f, Spread = 0.15f, Bend = 0.5f, Kink = 0.5f, Droop = 0.5f, Flutter = 0.15f;
		float	Variation = 0.2f, TipLift = 0.3f, Saturation = 1.0f;
		int		MaxBlades = 524288;			// strands per list (near / far) a frame can hold
		int		DebugView = 0;				// 1 near strands red, far blue; 2 a colour per game grass draw
		int		Log = 0;					// 1 = the GPU's counts every 300 frames (readback, never waits)
	};
	SettingsStruct S;

	struct DrawRec {				// GpuGrass.glsl GGDraw (std430, 128 bytes)
		float shape[4], scaleMask[4], sun[4], ambient[4], ramp[4][4];
	};
	static_assert(sizeof(DrawRec) == 128, "GGDraw layout");

	struct FrameBlock {				// GpuGrass.glsl GG_FRAME_BLOCK (std140, 336 bytes)
		float rows[4][4];
		float cam[4], sunDir[4], wind[4], fade[4], fogParam[4], fogColor[4], lod[4], shape[4], shape2[4], shape3[4], lanes[4], misc[4], look[4];
		float planes[4][4];
	};
	static_assert(sizeof(FrameBlock) == 336, "GG_FRAME_BLOCK layout");

	const unsigned kMaxDraws = 1024;		// grass draws a frame (the draw index has 12 bits; d30 noon has ~210 early draws)
	const unsigned kMaxTufts = 524288;		// tufts a frame (d30 noon ~48k)
	const unsigned kStatsBytes = 64;		// the indirect arguments (2 x 16) and the cull pass's counters
	// Buffer slots: ONE number per buffer, never shared between a uniform and a storage buffer (they meet in the same DXVK slot).
	enum : UINT { SlotFrame = 0, SlotTufts = 1, SlotTuftDraw = 2, SlotDraws = 3, SlotBlades = 4, SlotArgs = 5 };

	ID3D9NvrExtGeometry*		Geometry = nullptr;
	ID3D9NvrExtGeometryScene*	Scene = nullptr;
	ID3D9NvrExtReadback*		Readback = nullptr;
	ID3D9NvrExtPipeline*		CullPipeline = nullptr;
	ID3D9NvrExtPipeline*		NearPipeline = nullptr;
	ID3D9NvrExtPipeline*		FarPipeline = nullptr;
	ID3D9NvrExtBuffer*			FrameBuf = nullptr;
	ID3D9NvrExtBuffer*			DrawBuf = nullptr;
	ID3D9NvrExtBuffer*			TuftBuf = nullptr;
	ID3D9NvrExtBuffer*			TuftDrawBuf = nullptr;
	ID3D9NvrExtBuffer*			BladeBuf = nullptr;
	ID3D9NvrExtBuffer*			ArgsBuf = nullptr;
	ID3D9NvrExtBuffer*			ReadbackBuf = nullptr;
	UINT						TuftCapacity = 0, BladeCapacity = 0;
	bool						Tried = false, Off = false;

	// One early grass call.
	bool		Active = false, Hooked = false, Armed = false, Replaced = false;
	void*		ArmPass = nullptr;
	int			ArmPart = 0;
	float		ArmNearest = 0.0f;
	NewGrass::DrawIndexedPrimitiveFn Next = nullptr;
	std::vector<D3DXVECTOR4>	Tufts;
	std::vector<UInt32>			TuftDraw;
	std::vector<DrawRec>		Draws;
	bool		HaveGlobals = false;
	float		Globals[16][4];			// c0-c15 of the first caught draw (sun direction, wind, fade, ModelViewProj, fog)
	// The depth compare the game's grass draws use, from the DEVICE (2026-10-27 bulk run 2: nothing visible was drawn; the compare came
	// from the game's own render-state cache, and the reversed depth buffer is set on the device past that cache, so a cached LESSEQUAL
	// against a depth buffer cleared to 0 rejected every strand: tests/gpugrass_draw_check.cpp draws 0 pixels with LESS_OR_EQUAL and
	// 227k with GREATER_OR_EQUAL). The cache's value is kept for the log.
	DWORD		ZFuncDevice = D3DCMP_GREATEREQUAL, ZFuncCache = 0;

	unsigned	StatCaught = 0, StatLetThrough = 0, StatFar = 0, StatNotCard = 0, StatNormals = 0, StatMoved = 0, StatFull = 0, StatSwallowed = 0;
	UInt32		Frames = 0;
	bool		ReadbackPending = false;
	UInt32		ReadbackFrame = 0;
	UInt32		ReadbackTufts = 0;		// tufts uploaded in the frame whose counts are being read back
	UInt32		ReadbackCount = 0, BadCounts = 0;

	void TurnOff(const char* why) {
		Off = true;
		Logger::Log("UNOFFICIAL GPU grass: off for this session: %s (the game's grass as before)", why);
	}

	bool MakeBuffer(UINT size, UINT flags, const char* name, ID3D9NvrExtBuffer*& out) {
		DxvkCompute::Drop(out);
		NVR_EXT_BUFFER_DESC desc = { size, flags, name };
		if (FAILED(DxvkCompute::Extension()->CreateBuffer(&desc, &out)) || !out) { out = nullptr; return false; }
		return true;
	}

	// UpdateBuffer takes at most 64 KiB a call.
	bool Upload(ID3D9NvrExtBuffer* buffer, const void* data, UINT size) {
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		const unsigned char* p = (const unsigned char*)data;
		for (UINT done = 0; done < size;) {
			const UINT n = (std::min)(size - done, 65536u);
			if (FAILED(ext->UpdateBuffer(buffer, done, n, p + done))) return false;
			done += n;
		}
		return true;
	}

	bool Ready() {
		if (Off) return false;
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		if (!Tried) {
			Tried = true;
			if (!ext || FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtGeometry), (void**)&Geometry)) || !Geometry) {
				Geometry = nullptr;
				TurnOff("this d3d9.dll has no geometry passes (needs the Vegas Shaders DXVK fork 1.7-grass)");
				return false;
			}
			if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtGeometryScene), (void**)&Scene)) || !Scene) {
				Scene = nullptr;
				TurnOff("this d3d9.dll cannot draw geometry into the scene with its depth buffer (needs the Vegas Shaders DXVK fork 1.7-grass, fork patch 0024)");
				return false;
			}
			if (FAILED(ext->QueryInterface(__uuidof(ID3D9NvrExtReadback), (void**)&Readback))) Readback = nullptr;	// only for the counts in the log

			// the cull pass: frame block, tufts, their draw, the draws, the strands (written), the counts (written).
			// 2026-10-27 bulk run FIX: every buffer has its OWN slot number. The extension binds a uniform buffer and a read-only
			// storage buffer of the same slot to the same DXVK resource (NvrExtBufferSlotBase + slot): with the frame block and the
			// tufts both on slot 0 the shader read the tufts as its frame block (tuft count = the 5th tuft's size step, ~1), placed
			// nothing, and the cards were skipped all the same: bare ground. (Hi-Z hit the same clash on 10-26.)
			const NVR_EXT_BINDING_DESC cullBindings[] = {
				{ 0, 0, NVR_EXT_BIND_UNIFORM_BUFFER, SlotFrame,    NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 0, 1, NVR_EXT_BIND_STORAGE_BUFFER, SlotTufts,    NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 0, 2, NVR_EXT_BIND_STORAGE_BUFFER, SlotTuftDraw, NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 0, 3, NVR_EXT_BIND_STORAGE_BUFFER, SlotDraws,    NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 0, 4, NVR_EXT_BIND_STORAGE_BUFFER, SlotBlades,   NVR_EXT_VIEW_2D, TRUE,  0 },
				{ 0, 5, NVR_EXT_BIND_STORAGE_BUFFER, SlotArgs,     NVR_EXT_VIEW_2D, TRUE,  0 },
			};
			std::vector<char> words;
			if (!DxvkCompute::LoadSpirv("GpuGrassCull", words)) { TurnOff("Compute\\GpuGrassCull.spv missing"); return false; }
			NVR_EXT_COMPUTE_PIPELINE_DESC cull = {};
			cull.pSpirv = words.data();
			cull.SpirvSize = (UINT)words.size();
			cull.BindingCount = _countof(cullBindings);
			cull.pBindings = cullBindings;
			cull.pDebugName = "GpuGrassCull";
			if (FAILED(ext->CreateComputePipeline(&cull, &CullPipeline)) || !CullPipeline) { TurnOff("the cull pipeline was refused (DXVK's log says why)"); return false; }

			// the strands: the vertex shader reads the frame block, the draws and the strands; the fragment shader writes the colour it got
			std::vector<char> fs, nearVs, farVs;
			if (!DxvkCompute::LoadSpirv("GpuGrass", fs) || !DxvkCompute::LoadSpirv("GpuGrassNearVS", nearVs) || !DxvkCompute::LoadSpirv("GpuGrassFarVS", farVs)) {
				TurnOff("Compute\\GpuGrass.spv / GpuGrassNearVS.spv / GpuGrassFarVS.spv missing");
				return false;
			}
			const NVR_EXT_BINDING_DESC vsBindings[] = {
				{ 0, 0, NVR_EXT_BIND_UNIFORM_BUFFER, SlotFrame,  NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 0, 3, NVR_EXT_BIND_STORAGE_BUFFER, SlotDraws,  NVR_EXT_VIEW_2D, FALSE, 0 },
				{ 0, 4, NVR_EXT_BIND_STORAGE_BUFFER, SlotBlades, NVR_EXT_VIEW_2D, FALSE, 0 },
			};
			for (int isFar = 0; isFar < 2; isFar++) {
				const std::vector<char>& vs = isFar ? farVs : nearVs;
				NVR_EXT_GEOMETRY_PIPELINE_DESC draw = {};
				draw.Graphics.pVertexSpirv = vs.data();
				draw.Graphics.VertexSpirvSize = (UINT)vs.size();
				draw.Graphics.pFragmentSpirv = fs.data();
				draw.Graphics.FragmentSpirvSize = (UINT)fs.size();
				draw.Graphics.ColorTargetCount = 1;
				draw.Graphics.Blend = NVR_EXT_BLEND_NONE;
				draw.Graphics.ColorWriteMask = 1 | 2 | 4;	// RGB, as the game's grass draws (colorwrite 0x7)
				draw.Graphics.pDebugName = isFar ? "GpuGrassFar" : "GpuGrassNear";
				draw.VertexBindingCount = _countof(vsBindings);
				draw.pVertexBindings = vsBindings;
				if (FAILED(Geometry->CreateGeometryPipeline(&draw, isFar ? &FarPipeline : &NearPipeline)) || !(isFar ? FarPipeline : NearPipeline)) {
					TurnOff("a strand pipeline was refused (DXVK's log says why)");
					return false;
				}
			}

			if (!MakeBuffer(sizeof(FrameBlock), NVR_EXT_BUFFER_UNIFORM | NVR_EXT_BUFFER_TRANSFER_DST, "GpuGrass frame", FrameBuf) ||
				!MakeBuffer(kMaxDraws * sizeof(DrawRec), NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "GpuGrass draws", DrawBuf) ||
				!MakeBuffer(kStatsBytes, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_INDIRECT | NVR_EXT_BUFFER_TRANSFER_DST | NVR_EXT_BUFFER_TRANSFER_SRC, "GpuGrass counts", ArgsBuf)) {
				TurnOff("the frame / draws / counts buffers could not be made");
				return false;
			}
			if (Readback && FAILED(Readback->CreateReadbackBuffer(kStatsBytes, "GpuGrass counts readback", &ReadbackBuf))) ReadbackBuf = nullptr;
			Logger::Log("UNOFFICIAL GPU grass: ready (Vegas Shaders DXVK scene geometry passes)%s", ReadbackBuf ? "" : "; no readback (no counts in the log)");
		}

		const UINT blades = (UINT)(std::min)((std::max)(S.MaxBlades, 4096), 2097152);
		if (blades != BladeCapacity) {
			if (!MakeBuffer(blades * 2 * 48, NVR_EXT_BUFFER_STORAGE, "GpuGrass strands", BladeBuf)) { TurnOff("the strand buffer could not be made"); return false; }
			BladeCapacity = blades;
			Logger::Log("UNOFFICIAL GPU grass: room for %u near + %u far strands (%u MB)", blades, blades, blades * 2 * 48 / (1024 * 1024));
		}
		return true;
	}

	bool EnsureTuftCapacity(UINT tufts) {
		if (tufts <= TuftCapacity) return true;
		UINT cap = TuftCapacity ? TuftCapacity : 16384;
		while (cap < tufts) cap *= 2;
		if (!MakeBuffer(cap * 16, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "GpuGrass tufts", TuftBuf) ||
			!MakeBuffer(cap * 4, NVR_EXT_BUFFER_STORAGE | NVR_EXT_BUFFER_TRANSFER_DST, "GpuGrass tuft draws", TuftDrawBuf)) {
			TuftCapacity = 0;
			return false;
		}
		TuftCapacity = cap;
		return true;
	}

	// The pass's geometry must stand at the world origin (the instance positions are then world positions, as the game's grass is).
	bool AtOrigin(NiGeometry* geometry) {
		const NiTransform& t = geometry->m_worldTransform;
		if (fabsf(t.pos.x) + fabsf(t.pos.y) + fabsf(t.pos.z) > 0.5f || fabsf(t.scale - 1.0f) > 1e-3f) return false;
		for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
			if (fabsf(t.rot.data[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-3f) return false;
		return true;
	}

	// Records one card draw's tufts (true), or leaves it to the game (false).
	bool Catch(IDirect3DDevice9* device, UINT numVertices) {
		NiGeometry* geometry = ArmPass ? *(NiGeometry**)ArmPass : nullptr;
		if (!geometry || !geometry->geomData) return false;
		if (!(NewGrass::LastVariant == '0' || NewGrass::LastVariant == '2')) { StatNormals++; return false; }	// 001 / 003 light real meshes
		if (!AtOrigin(geometry)) { StatMoved++; return false; }
		const NewGrass::Model& model = NewGrass::GetModel(geometry);
		if (!model.Card) { StatNotCard++; return false; }
		IDirect3DBaseTexture9* texture = TheRenderManager->renderState->GetTexture(0);
		const NewGrass::Ramp& ramp = NewGrass::GetRamp(device, texture, geometry->geomData, model);
		if (!ramp.Cutout) { StatNotCard++; return false; }
		TallGrassShader* shader = TallGrassShader::GetShader();
		if (!shader || !shader->pInstanceData) return false;
		unsigned tufts = shader->ucInstanceCount;
		const unsigned fromVertices = model.VertsPerModel ? numVertices / model.VertsPerModel : 0;
		if (tufts == 0 || tufts > NewGrass::kTufts) tufts = (std::min)(fromVertices, NewGrass::kTufts);
		if (tufts == 0) return false;
		if (Draws.size() >= kMaxDraws || Tufts.size() + tufts > kMaxTufts) { StatFull++; return false; }
		float c[16][4];
		if (FAILED(device->GetVertexShaderConstantF(0, &c[0][0], 16))) return false;
		if (!HaveGlobals) {
			memcpy(Globals, c, sizeof(Globals));
			ZFuncCache = TheRenderManager->renderState->GetRenderState(D3DRS_ZFUNC);
			if (FAILED(device->GetRenderState(D3DRS_ZFUNC, &ZFuncDevice))) ZFuncDevice = 0;
			HaveGlobals = true;
		}
		else if (memcmp(Globals[9], c[9], 4 * 4 * sizeof(float))) { StatMoved++; return false; }	// another transform than the first draw's

		DrawRec r = {};
		r.shape[0] = model.Height * ramp.Top; r.shape[1] = model.Width; r.shape[2] = model.SwayA; r.shape[3] = model.SwayB;
		r.scaleMask[0] = c[2][0]; r.scaleMask[1] = c[2][1]; r.scaleMask[2] = c[2][2];
		r.scaleMask[3] = (ArmPart == 1 ? 1.0f : 0.0f) + (NewGrass::LastVariant == '2' ? 2.0f : 0.0f);
		// a sparse card (alpha coverage under 15%) gets fewer strands, down to half (NewGrass's FillRef rule)
		r.sun[0] = c[1][0]; r.sun[1] = c[1][1]; r.sun[2] = c[1][2];
		r.sun[3] = (std::min)((std::max)(ramp.Coverage / 0.15f, 0.5f), 1.0f);
		r.ambient[0] = c[6][0]; r.ambient[1] = c[6][1]; r.ambient[2] = c[6][2];
		r.ambient[3] = ramp.Coverage * 2.0f * model.Width * model.Height * ramp.Top;	// the card's opaque area (one card)
		for (int b = 0; b < 4; b++) {	// the bright twig colour per band (what the visible card pixels show) and the silhouette's edge
			r.ramp[b][0] = ramp.Bright[b][0]; r.ramp[b][1] = ramp.Bright[b][1]; r.ramp[b][2] = ramp.Bright[b][2];
			r.ramp[b][3] = (std::min)((std::max)(ramp.Edge[b], 0.05f), 1.0f);
		}
		const UInt32 index = (UInt32)Draws.size();
		Draws.push_back(r);
		const D3DXVECTOR4* inst = (const D3DXVECTOR4*)shader->pInstanceData;
		Tufts.insert(Tufts.end(), inst, inst + tufts);
		TuftDraw.insert(TuftDraw.end(), tufts, index);
		return true;
	}

	HRESULT STDMETHODCALLTYPE DrawHook(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start, UINT count) {
		if (!Armed) return Next(device, type, base, minIndex, numVertices, start, count);
		if (Replaced) {	// the rest of a caught pass (strip arrays): swallowed with its first draw, or the card kept
			if (S.KeepCards) return Next(device, type, base, minIndex, numVertices, start, count);
			StatSwallowed++;
			return D3D_OK;
		}
		if (!Catch(device, numVertices)) { StatLetThrough++; Armed = false; return Next(device, type, base, minIndex, numVertices, start, count); }
		Replaced = true;
		StatCaught++;
		if (S.KeepCards) return Next(device, type, base, minIndex, numVertices, start, count);
		return D3D_OK;
	}

	void Unhook(IDirect3DDevice9* device) {
		if (!Hooked || !device) return;
		void** table = *(void***)device;
		if (table[82] == (void*)&DrawHook) {
			DWORD old = 0;
			if (VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
				table[82] = (void*)Next;
				VirtualProtect(&table[82], sizeof(void*), old, &old);
			}
		}
		Hooked = false;
	}

	// The point the clip rows put at clip x = y = w = 0: the eye, in the grass's own world space (exact, whatever space it is).
	bool EyeFromRows(const float rows[4][4], double eye[3]) {
		const double a[3][3] = { { rows[0][0], rows[0][1], rows[0][2] }, { rows[1][0], rows[1][1], rows[1][2] }, { rows[3][0], rows[3][1], rows[3][2] } };
		const double b[3] = { -rows[0][3], -rows[1][3], -rows[3][3] };
		const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		if (!(fabs(det) > 1e-12)) return false;
		for (int k = 0; k < 3; k++) {	// Cramer's rule
			double m[3][3];
			for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r][c] = c == k ? b[r] : a[r][c];
			eye[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
		}
		return true;
	}

	// The GPU's counts of a frame a few frames ago (the readback never waits). Every tuft's own clump ends in exactly one of: drawn,
	// outside the view, past the fade start; if those add up to fewer than the tufts uploaded, the cull pass did not see its input
	// (2026-10-27: a binding clash made it see 1 of 4104): twice in a row, the feature turns itself off so the cards come back.
	void LogCounts() {
		if (!ReadbackBuf || !ReadbackPending) return;
		UInt32 counts[kStatsBytes / 4];
		if (Readback->TryReadback(ReadbackBuf, 0, kStatsBytes, counts) != S_OK) return;
		ReadbackPending = false;
		ReadbackCount++;
		const UInt32 seen = counts[8] + counts[9] + counts[12];
		const bool bad = ReadbackTufts > 0 && seen < ReadbackTufts;
		if (ReadbackCount <= 3 || ReadbackCount % 60 == 0 || (S.Log && ReadbackCount % 5 == 0) || bad)
			Logger::Log("UNOFFICIAL GPU grass: frame %u counts (GPU): %u tufts uploaded, %u clumps drawn, %u outside the view, %u tufts past the fade start (cards); strands near %u, far %u, %u did not fit (MaxBlades)%s",
				ReadbackFrame, ReadbackTufts, counts[8], counts[9], counts[12], counts[1], counts[5], counts[11], bad ? " -- THE CULL PASS DID NOT SEE EVERY TUFT" : "");
		BadCounts = bad ? BadCounts + 1 : 0;
		if (BadCounts >= 2) TurnOff("the cull pass saw fewer tufts than were uploaded (twice); the cards are kept");
	}

	// The cull pass and the two indirect draws, into the device's render target and depth buffer as they are now.
	bool RenderStrands(IDirect3DDevice9* device) {
		ID3D9NvrExt* ext = DxvkCompute::Extension();
		const UINT tufts = (UINT)Tufts.size();
		if (!EnsureTuftCapacity(tufts)) { TurnOff("the tuft buffers could not be made"); return false; }

		IDirect3DSurface9* rt = nullptr;
		IDirect3DSurface9* ds = nullptr;
		device->GetRenderTarget(0, &rt);
		device->GetDepthStencilSurface(&ds);
		bool ok = rt && ds;
		D3DSURFACE_DESC rtDesc = {};
		if (ok) ok = SUCCEEDED(rt->GetDesc(&rtDesc));
		D3DVIEWPORT9 vp = {};
		device->GetViewport(&vp);

		FrameBlock f = {};
		double eye[3] = {};
		if (ok) ok = EyeFromRows(&Globals[9], eye);
		if (ok) {
			for (int i = 0; i < 4; i++) {
				const float* r = Globals[9 + i];
				f.rows[i][0] = r[0]; f.rows[i][1] = r[1]; f.rows[i][2] = r[2];
				f.rows[i][3] = (float)((double)r[3] + (double)r[0] * eye[0] + (double)r[1] * eye[1] + (double)r[2] * eye[2]);
			}
			const float density = (std::min)((std::max)(S.Density, 1.0f), 6.0f);
			f.cam[0] = (float)eye[0]; f.cam[1] = (float)eye[1]; f.cam[2] = (float)eye[2]; f.cam[3] = (float)tufts;
			f.sunDir[0] = Globals[0][0]; f.sunDir[1] = Globals[0][1]; f.sunDir[2] = Globals[0][2]; f.sunDir[3] = Globals[7][0];
			memcpy(f.wind, Globals[4], 16);
			f.fade[0] = Globals[5][2]; f.fade[1] = Globals[5][3]; f.fade[2] = density; f.fade[3] = (std::max)(S.ExtraRadius, 0.0f);
			memcpy(f.fogParam, Globals[15], 16);
			memcpy(f.fogColor, Globals[14], 16);
			const float lodFar = (std::max)(S.LodFar, S.LodNear + 1.0f);
			f.lod[0] = S.LodNear; f.lod[1] = 1.0f / (lodFar - S.LodNear); f.lod[2] = (float)S.Blades; f.lod[3] = (float)S.BladesFar;
			f.shape[0] = S.HeightScale; f.shape[1] = S.Coverage; f.shape[2] = (std::min)((std::max)(S.LengthMin, 0.1f), 1.0f); f.shape[3] = S.Spread;
			f.shape2[0] = S.TipReach; f.shape2[1] = S.Kink; f.shape2[2] = S.Droop; f.shape2[3] = S.Bend;
			const float p11 = sqrtf(f.rows[0][0] * f.rows[0][0] + f.rows[0][1] * f.rows[0][1] + f.rows[0][2] * f.rows[0][2]);
			f.shape3[0] = S.Flutter; f.shape3[1] = (std::max)(S.MinPixelWidth, 0.0f);
			f.shape3[2] = p11 > 0.0f ? 2.0f / (p11 * (float)rtDesc.Width) : 0.0f; f.shape3[3] = (std::max)(S.DetailDistance, 0.0f);
			if (GrassWind::On) memcpy(f.lanes, GrassWind::Lanes, 16);
			f.misc[0] = GrassWind::ClockNow; f.misc[1] = (float)BladeCapacity; f.misc[2] = (float)S.DebugView;
			f.look[0] = S.Variation; f.look[1] = S.TipLift; f.look[2] = S.Saturation;
			// the side planes: w + x >= 0, w - x >= 0, w + y >= 0, w - y >= 0 (D3D clip space), normalized
			for (int p = 0; p < 4; p++) {
				const int axis = p < 2 ? 0 : 1;
				const float sign = (p & 1) ? -1.0f : 1.0f;
				float n[4];
				for (int k = 0; k < 4; k++) n[k] = f.rows[3][k] + sign * f.rows[axis][k];
				const float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				for (int k = 0; k < 4; k++) f.planes[p][k] = len > 0.0f ? n[k] / len : 0.0f;
			}
		}

		const UInt32 init[kStatsBytes / 4] = { 15, 0, 0, 0, 3, 0, 0, 0 };
		if (ok) ok = SUCCEEDED(ext->UpdateBuffer(FrameBuf, 0, sizeof(f), &f)) && Upload(DrawBuf, Draws.data(), (UINT)(Draws.size() * sizeof(DrawRec))) &&
			Upload(TuftBuf, Tufts.data(), tufts * 16) && Upload(TuftDrawBuf, TuftDraw.data(), tufts * 4) &&
			SUCCEEDED(ext->UpdateBuffer(ArgsBuf, 0, kStatsBytes, init));

		if (ok) {
			const UINT clumps = (UINT)ceilf((std::min)((std::max)(S.Density, 1.0f), 6.0f));
			const UINT groups = (std::min)((tufts * clumps + 63) / 64, 65535u);
			const NVR_EXT_RESOURCE_BINDING cullRes[] = {
				{ SlotFrame,    NVR_EXT_BIND_UNIFORM_BUFFER, FrameBuf, 0, 0, 0, FALSE },
				{ SlotTufts,    NVR_EXT_BIND_STORAGE_BUFFER, TuftBuf, 0, 0, 0, FALSE },
				{ SlotTuftDraw, NVR_EXT_BIND_STORAGE_BUFFER, TuftDrawBuf, 0, 0, 0, FALSE },
				{ SlotDraws,    NVR_EXT_BIND_STORAGE_BUFFER, DrawBuf, 0, 0, 0, FALSE },
				{ SlotBlades,   NVR_EXT_BIND_STORAGE_BUFFER, BladeBuf, 0, 0, 0, FALSE },
				{ SlotArgs,     NVR_EXT_BIND_STORAGE_BUFFER, ArgsBuf, 0, 0, 0, FALSE },
			};
			NVR_EXT_DISPATCH_DESC dispatch = {};
			dispatch.pPipeline = CullPipeline;
			dispatch.BindingCount = _countof(cullRes);
			dispatch.pBindings = cullRes;
			dispatch.GroupsX = groups;
			dispatch.GroupsY = dispatch.GroupsZ = 1;
			if (FAILED(ext->Dispatch(&dispatch))) { TurnOff("the cull dispatch failed (DXVK's log says why)"); ok = false; }
		}

		if (ok) {
			// The compare: the device's (what the game's own grass draws test with), checked against the projection the strands use: a
			// reversed depth buffer (z/w falls with distance) needs GREATER(EQUAL), a normal one LESS(EQUAL). If the two disagree, the
			// projection wins (it is what the strands' depth is made of).
			double fwd[3] = { f.rows[3][0], f.rows[3][1], f.rows[3][2] };
			const double fl = sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
			auto depthAt = [&](double d) {
				double p[3] = { fwd[0] / fl * d, fwd[1] / fl * d, fwd[2] / fl * d };
				const double z = f.rows[2][0] * p[0] + f.rows[2][1] * p[1] + f.rows[2][2] * p[2] + f.rows[2][3];
				const double w = f.rows[3][0] * p[0] + f.rows[3][1] * p[1] + f.rows[3][2] * p[2] + f.rows[3][3];
				return w > 0.0 ? z / w : 0.0;
			};
			const bool reversed = fl > 0.0 && depthAt(100.0) > depthAt(10000.0);
			const bool deviceGreater = ZFuncDevice == D3DCMP_GREATER || ZFuncDevice == D3DCMP_GREATEREQUAL;
			const bool deviceLess = ZFuncDevice == D3DCMP_LESS || ZFuncDevice == D3DCMP_LESSEQUAL;
			const bool agree = reversed ? deviceGreater : deviceLess;
			const NVR_EXT_COMPARE compare = agree ? (NVR_EXT_COMPARE)(ZFuncDevice - 1) : (reversed ? NVR_EXT_COMPARE_GREATER_EQUAL : NVR_EXT_COMPARE_LESS_EQUAL);
			if (Frames <= 3) {
				D3DSURFACE_DESC dsDesc = {};
				ds->GetDesc(&dsDesc);
				Logger::Log("UNOFFICIAL GPU grass: frame %u: drawn into target %ux%u format %u samples %u, depth %ux%u format %u samples %u, viewport %u %u %ux%u z %.2f-%.2f; "
					"depth compare: device ZFUNC %u, the game's cache %u, the projection is %s -> %s%s",
					Frames, rtDesc.Width, rtDesc.Height, (unsigned)rtDesc.Format, (unsigned)rtDesc.MultiSampleType, dsDesc.Width, dsDesc.Height, (unsigned)dsDesc.Format,
					(unsigned)dsDesc.MultiSampleType, vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, ZFuncDevice, ZFuncCache, reversed ? "reversed (near = 1)" : "normal (near = 0)",
					compare == NVR_EXT_COMPARE_GREATER_EQUAL ? "GREATER_EQUAL" : compare == NVR_EXT_COMPARE_GREATER ? "GREATER" : compare == NVR_EXT_COMPARE_LESS_EQUAL ? "LESS_EQUAL" : compare == NVR_EXT_COMPARE_LESS ? "LESS" : "other",
					agree ? "" : " (the device's compare disagreed with the projection: the projection's used)");
			}
			NVR_EXT_DEPTH_STENCIL depth = {};
			depth.pResource = ds;
			depth.DepthTest = TRUE;
			depth.DepthWrite = TRUE;
			depth.DepthCompare = compare;
			const NVR_EXT_RESOURCE_BINDING drawRes[] = {
				{ SlotFrame,  NVR_EXT_BIND_UNIFORM_BUFFER, FrameBuf, 0, 0, 0, FALSE },
				{ SlotDraws,  NVR_EXT_BIND_STORAGE_BUFFER, DrawBuf, 0, 0, 0, FALSE },
				{ SlotBlades, NVR_EXT_BIND_STORAGE_BUFFER, BladeBuf, 0, 0, 0, FALSE },
			};
			const bool vpInside = vp.Width && vp.Height && vp.X + vp.Width <= rtDesc.Width && vp.Y + vp.Height <= rtDesc.Height;
			for (int isFar = 0; isFar < 2 && ok; isFar++) {
				NVR_EXT_GEOMETRY_SCENE_DESC draw = {};
				draw.Geometry.Draw.pPipeline = isFar ? FarPipeline : NearPipeline;
				draw.Geometry.Draw.TargetCount = 1;
				draw.Geometry.Draw.Targets[0].pResource = rt;
				if (vpInside) {
					draw.Geometry.Draw.ViewportX = (INT)vp.X; draw.Geometry.Draw.ViewportY = (INT)vp.Y;
					draw.Geometry.Draw.ViewportWidth = vp.Width; draw.Geometry.Draw.ViewportHeight = vp.Height;
				}
				draw.Geometry.Draw.BindingCount = _countof(drawRes);
				draw.Geometry.Draw.pBindings = drawRes;
				draw.pDepthStencil = &depth;
				draw.CullMode = NVR_EXT_CULL_NONE;
				draw.Multisample = TRUE;	// the game's MSAA: the strands are rasterized at the scene's own sample count
				draw.pIndirectArgs = ArgsBuf;
				draw.IndirectOffset = isFar ? 16 : 0;
				if (FAILED(Scene->DrawGeometryScene(&draw))) { TurnOff("a strand draw failed (DXVK's log says why)"); ok = false; }
			}
		}

		// the counts, now and then (the readback never waits: it is read a few frames later)
		if (ok && ReadbackBuf && !ReadbackPending && (Frames <= 3 || Frames % 60 == 0)) {
			if (SUCCEEDED(Readback->CopyToReadback(ReadbackBuf, 0, ArgsBuf, 0, kStatsBytes))) { ReadbackPending = true; ReadbackFrame = TheFrameRateManager->FrameNumber; ReadbackTufts = tufts; }
		}
		if (rt) rt->Release();
		if (ds) ds->Release();
		return ok;
	}
}

void ReadSettings() {
	const char* s = "Shaders.Grass.GPUGrass";
	S.Enabled = TheSettingManager->GetSettingI(s, "Enabled") != 0;
	S.KeepCards = TheSettingManager->GetSettingI(s, "KeepCards") != 0;
	S.BladeDistance = TheSettingManager->GetSettingF(s, "BladeDistance");
	S.Density = TheSettingManager->GetSettingF(s, "Density");
	S.ExtraRadius = TheSettingManager->GetSettingF(s, "ExtraRadius");
	S.Blades = (std::min)((std::max)(TheSettingManager->GetSettingI(s, "Blades"), 1), 128);
	S.BladesFar = (std::min)((std::max)(TheSettingManager->GetSettingI(s, "BladesFar"), 1), 128);
	S.DetailDistance = TheSettingManager->GetSettingF(s, "DetailDistance");
	S.LodNear = TheSettingManager->GetSettingF(s, "LodNear");
	S.LodFar = TheSettingManager->GetSettingF(s, "LodFar");
	S.Coverage = TheSettingManager->GetSettingF(s, "Coverage");
	S.MinPixelWidth = TheSettingManager->GetSettingF(s, "MinPixelWidth");
	S.HeightScale = TheSettingManager->GetSettingF(s, "HeightScale");
	S.LengthMin = TheSettingManager->GetSettingF(s, "LengthMin");
	S.TipReach = TheSettingManager->GetSettingF(s, "TipReach");
	S.Spread = TheSettingManager->GetSettingF(s, "Spread");
	S.Bend = TheSettingManager->GetSettingF(s, "Bend");
	S.Kink = TheSettingManager->GetSettingF(s, "Kink");
	S.Droop = TheSettingManager->GetSettingF(s, "Droop");
	S.Flutter = TheSettingManager->GetSettingF(s, "Flutter");
	S.Variation = TheSettingManager->GetSettingF(s, "Variation");
	S.TipLift = TheSettingManager->GetSettingF(s, "TipLift");
	S.Saturation = TheSettingManager->GetSettingF(s, "Saturation");
	S.MaxBlades = TheSettingManager->GetSettingI(s, "MaxBlades");
	S.DebugView = TheSettingManager->GetSettingI(s, "DebugView");
	S.Log = TheSettingManager->GetSettingI(s, "Log");
}

bool BeginEarly(IDirect3DDevice9* Device, bool GrassEnabled) {
	Active = Armed = Replaced = false;
	HaveGlobals = false;
	Tufts.clear(); TuftDraw.clear(); Draws.clear();
	if (Off || !S.Enabled || !GrassEnabled || !Device) return false;
	if (!DxvkCompute::Available()) {
		static bool said = false;
		if (!said) Logger::Log("UNOFFICIAL GPU grass: needs the Vegas Shaders DXVK fork's d3d9.dll (1.7-grass); %s: the game's grass as before", DxvkCompute::Status());
		said = true;
		return false;
	}
	if (!TheShaderManager->GameState.isExterior) return false;
	LogCounts();
	if (!Ready()) return false;
	void** table = *(void***)Device;
	if (table[82] != (void*)&DrawHook) {
		DWORD old = 0;
		if (!VirtualProtect(&table[82], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
		Next = (NewGrass::DrawIndexedPrimitiveFn)table[82];
		table[82] = (void*)&DrawHook;
		VirtualProtect(&table[82], sizeof(void*), old, &old);
		Hooked = true;
	}
	Active = table[82] == (void*)&DrawHook;
	return Active;
}

void Arm(void* Pass, int Part, float Nearest) {
	Armed = Replaced = false;
	if (!Active) return;
	if (Nearest > S.BladeDistance) { StatFar++; return; }
	ArmPass = Pass; ArmPart = Part; ArmNearest = (std::max)(Nearest, 0.0f);
	Armed = true;
}

void Disarm() { Armed = Replaced = false; ArmPass = nullptr; }
bool Took() { return Armed && Replaced; }	// UNOFFICIAL 2026-11-05 (Balebandro; GrassFirst's late replay with terrain blending on)
bool KeepsCards() { return S.KeepCards; }

void EndEarly(IDirect3DDevice9* Device) {
	Armed = Replaced = false;
	Unhook(Device);
	if (!Active) return;
	Active = false;
	Frames++;
	if (!Tufts.empty() && HaveGlobals) {
		static GpuTimer timer("GPU grass (cull + draw)");
		GpuProfileScope gpu(timer, Device);
		RenderStrands(Device);
	}
	if (Frames <= 3 || Frames % 3600 == 0)
		Logger::Log("UNOFFICIAL GPU grass: frame %u: %u early grass draws caught (%u tufts, density %.2f), their cards %s; left to the game: %u (%u beyond BladeDistance, %u not a cut-out card, %u lit by real normals, %u not at the world origin / other transform, %u over the buffers); %u extra card draws swallowed",
			Frames, StatCaught, (unsigned)Tufts.size(), S.Density, S.KeepCards ? "kept (hybrid)" : "skipped", StatLetThrough + StatFar, StatFar, StatNotCard, StatNormals, StatMoved, StatFull, StatSwallowed);
	StatCaught = StatLetThrough = StatFar = StatNotCard = StatNormals = StatMoved = StatFull = StatSwallowed = 0;
}

}
