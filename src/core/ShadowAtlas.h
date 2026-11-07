#pragma once

// UNOFFICIAL 2026-10-30 ShadowAtlas ([Shaders.ShadowsInteriors.Atlas] Enabled, restart, off; docs/next/INTERIOR-SHADOWS-MODERN.md step 1).
// Indoors every lamp shadow NVR publishes -- the live slots (people included) and the ShadowPool's still maps -- is also an OCTAHEDRAL
// tile of one depth atlas (R16F, Size x Size), and the object shaders read the draw's lamps from that one texture on s8
// (PointShadowForward.hlsl POINT_SHADOW_ATLAS) instead of six cube maps. Step 1 changes where the shadows are read from, not what they
// hold: each tile is its lamp's cube map resampled (distance / radius, the cube lookup's own direction convention), so the A/B against
// the cube path is the octahedral resampling; with PoolLamps raised every lamp past the live slots gets its still shadow without a
// sampler per lamp. (Steps 2-3, capsule people and contact shadows, come after.)
//   - layout in units of Size / 16: the live slots (up to 32, LightPoints) take 2 x 2 units each (1024 texels at 8192), in rows from the
//     top; the pool's lamps one unit each (512) in the rows below, as many as fit (pool index order);
//   - live tiles are converted every frame after the cube maps are drawn; a pool tile only when its cube map was drawn again (or another
//     lamp took that pool entry);
//   - PointShadowForward::SetForPass writes each matched light's tile (u, v of its first texel centre, its span in atlas uv) into the
//     light's tint register, and binds the atlas on s8.
// Off: nothing is made, the shaders are as before (no define).
#include "GpuProfiler.h"
#include "ShadowPool.h"
#include "PointShadowForward.h"

namespace ShadowAtlas {

	inline int Wanted = -1;
	inline IDirect3DTexture9* Texture = nullptr;
	inline IDirect3DSurface9* Surface = nullptr;
	inline IDirect3DPixelShader9* PS = nullptr;
	inline UINT Size = 0;
	inline bool Failed = false;
	struct PoolTile { const void* light = nullptr; const void* texture = nullptr; UInt32 drawn = 0; float x = 0, y = 0, z = 0, radius = 0; };
	inline PoolTile PoolTiles[ShadowPoolMax];
	// Live slots: converted only when ShadowManager redrew the slot's cube map this frame (LiveRedrawn, set there) or the slot changed
	// lamp or texture. (Every frame cost 1.3 ms for 24 tiles of 1024 on the GTX 1070, run 20261030-220952-slotA.)
	inline bool LiveRedrawn[64] = {};
	struct LiveTile { const void* light = nullptr; const void* texture = nullptr; };
	inline LiveTile LiveTiles[64];
	inline int LastLiveSlots = 0;		// Update's liveSlots (WorldStart converts the live tiles again with it)
	inline unsigned StatFrames = 0;
	inline unsigned long long StatLive = 0, StatPool = 0, StatPoolConverted = 0, StatNoTile = 0;

	inline bool CompiledIn() {
		if (Wanted < 0) Wanted = PointShadowForward::AtlasWanted() ? 1 : 0;	// (P85: DXVK only)
		return Wanted != 0;
	}

	inline bool Ensure(IDirect3DDevice9* d) {
		if (Texture) return true;
		if (Failed) return false;
		int size = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "Size");
		size = size >= 8192 ? 8192 : size >= 4096 ? 4096 : 2048;
		const UINT memBefore = d->GetAvailableTextureMem();	// UNOFFICIAL 2026-10-31 (deferred plan stage 0): the 128 MB target in a 32-bit process, measured
		if (FAILED(d->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &Texture, NULL)) || FAILED(Texture->GetSurfaceLevel(0, &Surface))) {
			Logger::Log("[ERROR] ShadowAtlas: could not make its %dx%d R16F atlas: off for the session (the cube maps as before)", size, size);
			if (Texture) { Texture->Release(); Texture = nullptr; }
			Failed = true;
			return false;
		}
		Size = (UINT)size;
		d->ColorFill(Surface, NULL, D3DCOLOR_ARGB(255, 255, 255, 255));	// 1 = nothing recorded (lit)
		static const char* kPS =
			"samplerCUBE C : register(s0); float4 K : register(c0);"	// K.x tile texels
			"float4 main(float2 uv : TEXCOORD0) : COLOR0 {"
			"  float2 v = (uv * K.x - 0.5f) / (K.x - 1.0f) * 2.0f - 1.0f;"
			"  float3 n = float3(v.x, v.y, 1.0f - abs(v.x) - abs(v.y));"
			"  float t = saturate(-n.z);"
			"  n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);"
			"  return texCUBElod(C, float4(normalize(n), 0.0f)).r; }";
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		const bool ok = SUCCEEDED(D3DXCompileShader(kPS, (UINT)strlen(kPS), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) && code &&
			SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &PS));
		if (!ok) Logger::Log("[ERROR] ShadowAtlas: the conversion shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
		if (code) code->Release();
		if (errors) errors->Release();
		if (!ok) { Failed = true; return false; }
		Logger::Log("ShadowAtlas: %ux%u R16F atlas made (%.0f MB); D3D9 available texture memory %u -> %u MB (%s runtime)", Size, Size, Size * (double)Size * 2.0 / 1048576.0,
			memBefore / 1048576u, d->GetAvailableTextureMem() / 1048576u, TheRenderManager ? TheRenderManager->D3D9RuntimeDescription() : "?");
		PointShadowForward::AtlasTexture = Texture;
		return true;
	}

	// UNOFFICIAL 2026-10-31 NearTiles ([Shaders.ShadowsInteriors.Atlas], restart, 0 or 8; user report: interior shadows blocky with the atlas on):
	// the first 8 live slots get 4 x 4 units (2048 texels at Size 8192, ~2.7x a 512 cube map's texels, so the copy no longer loses detail
	// against the cube faces it is made from) instead of 2 x 2 (1024: ~2/3 of a cube map, stretched up to 2x along the octahedral folds).
	// The other live slots keep 2 x 2; the pool gets what is left (64 tiles with 24 live slots, 32 with 32). 0 = the layout as before.
	inline int NearTiles = -1;
	inline int Near() {
		if (NearTiles < 0) NearTiles = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "NearTiles") > 0 ? 8 : 0;
		return NearTiles;
	}
	// Rows of units the live tiles of slots 0-31 take.
	inline int LiveRows32(int liveSlots) {
		const int live = (std::min)(liveSlots, 32);
		if (!Near()) return 2 * ((live + 7) / 8);
		const int bigRows = 4 * (((std::min)(live, Near()) + 3) / 4);		// 4 big tiles a band of 4 rows
		const int rest = (std::max)(0, live - Near());
		return bigRows + 2 * ((rest + 7) / 8);
	}
	// Rows of units the live tiles take (the pool starts below them). UNOFFICIAL 2026-11-06: live slots 32-63 (LightPoints above 32, up to
	// 64) get 1 x 1 unit tiles (512 texels at Size 8192, as the pool's) right after slots 0-31's; they had none, so those lamps cast no
	// shadow at all with the atlas on (and, before 43d5cf53, not even their traced one). The pool gets what is left (none with 64 live
	// slots and NearTiles 8: the lamps past the 64th then go unshadowed, as past the pool before).
	inline int LiveRows(int liveSlots) {
		const int extra = (std::max)(0, (std::min)(liveSlots, 64) - 32);
		return LiveRows32(liveSlots) + (extra + 15) / 16;
	}
	// A live slot's tile (2 x 2 units, or 4 x 4 for the first NearTiles slots) or a pool entry's (1 unit), in atlas texels; false when it has none.
	inline bool LiveRect(int slot, int liveSlots, UINT& x, UINT& y, UINT& n) {
		const UINT unit = Size / 16;
		if (slot < 0 || slot >= 64 || slot >= liveSlots) return false;
		if (slot >= 32) {	// (2026-11-06) 1 x 1 unit tiles after slots 0-31's (LiveRows)
			const int at = LiveRows32(liveSlots) * 16 + (slot - 32);
			if (at >= 256) return false;
			x = unit * (at % 16); y = unit * (at / 16); n = unit;
			return true;
		}
		if (slot < Near()) { x = 4 * unit * (slot % 4); y = 4 * unit * (slot / 4); n = 4 * unit; return true; }
		const int k = slot - Near();
		const int top = Near() ? 4 * ((Near() + 3) / 4) : 0;
		x = 2 * unit * (k % 8); y = unit * (top + 2 * (k / 8)); n = 2 * unit;
		return true;
	}
	inline bool PoolRect(int index, int liveSlots, UINT& x, UINT& y, UINT& n) {
		const UINT unit = Size / 16;
		const int firstRow = LiveRows(liveSlots);
		const int at = firstRow * 16 + index;
		if (index < 0 || at >= 256) return false;
		x = unit * (at % 16); y = unit * (at / 16); n = unit;
		return true;
	}

	// ShadowManager, indoors after Publish + PublishPool: every published lamp's tile, converted when needed; its uv into the slot.
	// liveSlots: the live slot count (the slots below it are live, ShadowCubeMapsMax and up the pool's).
	inline void Update(IDirect3DDevice9* d, int liveSlots);
	// RenderWorldSceneGraphHook, as the world scene starts ([Shaders.ShadowsInteriors.Atlas] WorldStartConvert, test): the live tiles
	// converted again from the cube maps as they are now (are the live cube maps changed after they are published?).
	inline void WorldStart(IDirect3DDevice9* d) {
		if (!CompiledIn() || !Texture || !TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "WorldStartConvert")) return;
		Update(d, LastLiveSlots);
	}
	inline void Update(IDirect3DDevice9* d, int liveSlots) {
		if (!CompiledIn() || !Ensure(d)) return;
		LastLiveSlots = liveSlots;
		static GpuTimer convertTimer("Shadow atlas convert");
		GpuProfileScope gpu(convertTimer, d);
		NiDX9RenderState* state = TheRenderManager->renderState;
		IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr; IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		IDirect3DSurface9* rt0 = nullptr; IDirect3DSurface9* zs = nullptr; IDirect3DBaseTexture9* tex0 = nullptr; D3DVIEWPORT9 vp = {};
		d->GetVertexShader(&vs); d->GetPixelShader(&ps); d->GetVertexDeclaration(&decl); d->GetFVF(&fvf);
		d->GetRenderTarget(0, &rt0); d->GetDepthStencilSurface(&zs); d->GetTexture(0, &tex0); d->GetViewport(&vp);
		static const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
			D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, 0xFu, D3DFILL_SOLID, FALSE, FALSE, 0 };
		DWORD savedRS[sizeof(kRS) / sizeof(kRS[0])];
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) { if (FAILED(d->GetRenderState(kRS[i], &savedRS[i]))) savedRS[i] = want[i]; if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], want[i]); }
		static const D3DSAMPLERSTATETYPE kSS[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_SRGBTEXTURE };
		// POINT: each tile texel takes a real cube texel. A linear read blended the "caster at the light" zeros (no data, lit) with the depths
		// beside them into small positive values, which count as blockers: halos round every lamp fixture (run 20261030-214413-slotA, the
		// atlas 6 % darker than the cube maps in Lucky 38 where the cube maps equal the screen-space shadows). The lookup's own linear filter
		// stays, as on the cube maps.
		const DWORD wantS[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
		DWORD savedS[sizeof(kSS) / sizeof(kSS[0])];
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) { if (FAILED(d->GetSamplerState(0, kSS[i], &savedS[i]))) savedS[i] = wantS[i]; if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], wantS[i]); }
		float savedPS[4] = {};
		d->GetPixelShaderConstantF(0, savedPS, 1);

		d->SetRenderTarget(0, Surface);
		d->SetDepthStencilSurface(NULL);
		state->SetVertexShader(NULL, false);
		state->SetPixelShader(PS, false);
		d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		auto convert = [&](IDirect3DCubeTexture9* cube, UINT x, UINT y, UINT n) {
			const D3DVIEWPORT9 tile = { x, y, n, n, 0.0f, 1.0f };
			d->SetViewport(&tile);
			d->SetTexture(0, cube);
			const float k[4] = { (float)n, 0.0f, 0.0f, 0.0f };
			d->SetPixelShaderConstantF(0, k, 1);
			struct V { float x, y, z, w, u, v; };
			const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x + n - 0.5f, y1 = y + n - 0.5f;
			const V quad[4] = { { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 }, { x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
		};
		auto uv = [&](PointShadowForward::Slot& slot, UINT x, UINT y, UINT n) {
			slot.tile[0] = (x + 0.5f) / Size; slot.tile[1] = (y + 0.5f) / Size; slot.tile[2] = (n - 1.0f) / Size;
		};
		for (int s = 0; s < PointShadowForward::SlotCount; s++) {
			PointShadowForward::Slot& slot = PointShadowForward::Slots[s];
			slot.tile[0] = slot.tile[1] = slot.tile[2] = 0.0f;
			if (!slot.cube) continue;
			UINT x, y, n;
			if (slot.slot < ShadowCubeMapsMax) {
				if (!LiveRect(slot.slot, liveSlots, x, y, n)) { if (GpuTimer::Enabled) StatNoTile++; continue; }
				LiveTile& t = LiveTiles[slot.slot];
				if (LiveRedrawn[slot.slot] || t.light != slot.light || t.texture != slot.cube) {
					// UNOFFICIAL 2026-11-01: a re-convert without a redraw (the slot's lamp or cube changed) is an event worth a journal line
					if (!LiveRedrawn[slot.slot]) {
						SessionLog::Count.atlasReconverts++;
						FRAME_JOURNAL("shadow atlas: live tile of slot %d re-converted (%s)", slot.slot, t.light != slot.light ? "another lamp" : "another cube");
					}
					convert(slot.cube, x, y, n);
					t = { slot.light, slot.cube };
					LiveRedrawn[slot.slot] = false;
					if (GpuTimer::Enabled) StatLive++;
				}
				uv(slot, x, y, n);
			}
			else {
				const int index = slot.slot - ShadowCubeMapsMax;
				if (index >= ShadowPoolMax || !PoolRect(index, liveSlots, x, y, n)) { if (GpuTimer::Enabled) StatNoTile++; continue; }
				PoolTile& t = PoolTiles[index];
				const ShadowPool::Cube* cube = index < (int)ShadowPool::Cubes.size() ? &ShadowPool::Cubes[index] : nullptr;
				const UInt32 drawn = cube ? cube->drawn : 0;
				if (t.light != slot.light || t.texture != slot.cube || t.drawn != drawn || t.x != slot.position.x || t.y != slot.position.y ||
					t.z != slot.position.z || t.radius != slot.radius) {
					SessionLog::Count.atlasReconverts++;	// UNOFFICIAL 2026-11-01
					FRAME_JOURNAL("shadow atlas: pool tile %d converted (%s)", index, t.light != slot.light ? "another lamp" : t.drawn != drawn ? "cube redrawn" : "lamp moved / reach");
					convert(slot.cube, x, y, n);
					t = { slot.light, slot.cube, drawn, slot.position.x, slot.position.y, slot.position.z, slot.radius };
					if (GpuTimer::Enabled) StatPoolConverted++;
				}
				uv(slot, x, y, n);
				if (GpuTimer::Enabled) StatPool++;
			}
		}

		// [Shaders.ShadowsInteriors.Atlas] Dump (diagnostic, live, set back to 0 here): the atlas and the first live slot's cube map to the
		// log folder as DDS (R16F / the cube's own format), with that slot's light and tile in the log.
		if (TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "Dump") != 0) {
			TheSettingManager->SetSetting("Shaders.ShadowsInteriors.Atlas", "Dump", (int)0);
			// (render targets: copied to system memory first, GetRenderTargetData; the cube's six faces one file each)
			auto save = [&](IDirect3DSurface9* rt, const char* file) -> HRESULT {
				D3DSURFACE_DESC t = {};
				rt->GetDesc(&t);
				IDirect3DSurface9* mem = nullptr;
				const HRESULT r1 = d->CreateOffscreenPlainSurface(t.Width, t.Height, t.Format, D3DPOOL_SYSTEMMEM, &mem, NULL);
				const HRESULT r2 = SUCCEEDED(r1) ? d->GetRenderTargetData(rt, mem) : E_FAIL;
				HRESULT r3 = E_FAIL;
				// raw floats after a 16-byte header (width, height, format, bytes per row): D3DX's DDS writer refuses some float formats
				if (SUCCEEDED(r2)) {
					D3DLOCKED_RECT lock = {};
					r3 = mem->LockRect(&lock, NULL, D3DLOCK_READONLY);
					if (SUCCEEDED(r3)) {
						FILE* out = nullptr;
						if (!fopen_s(&out, file, "wb") && out) {
							const UInt32 head[4] = { t.Width, t.Height, (UInt32)t.Format, (UInt32)lock.Pitch };
							fwrite(head, sizeof(head), 1, out);
							fwrite(lock.pBits, lock.Pitch, t.Height, out);
							fclose(out);
						}
						else r3 = E_FAIL;
						mem->UnlockRect();
					}
				}
				Logger::Log("ShadowAtlas dump %s: %ux%u format %d, create %08X, copy %08X, write %08X", file, t.Width, t.Height, (int)t.Format, (unsigned)r1, (unsigned)r2, (unsigned)r3);
				if (mem) mem->Release();
				return r3;
			};
			const HRESULT a = save(Surface, VS_LOGS_FOLDER "/atlas_dump.raw");
			for (int s = 0; s < PointShadowForward::SlotCount; s++) {
				const PointShadowForward::Slot& slot = PointShadowForward::Slots[s];
				if (slot.slot > 3 || !slot.cube) continue;
				HRESULT b = S_OK;
				for (int f = 0; f < 6; f++) {
					IDirect3DSurface9* face = nullptr;
					char file[96];
					sprintf_s(file, VS_LOGS_FOLDER "/atlas_cube%d_%d.raw", slot.slot, f);
					if (SUCCEEDED(slot.cube->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &face))) { const HRESULT r = save(face, file); if (FAILED(r)) b = r; face->Release(); }
				}
				Logger::Log("ShadowAtlas dump (atlas %08X, cube %08X): slot %d light at %.1f %.1f %.1f radius %.1f, tile u %.6f v %.6f span %.6f (atlas %u)",
					(unsigned)a, (unsigned)b, slot.slot, slot.position.x, slot.position.y, slot.position.z, slot.radius, slot.tile[0], slot.tile[1], slot.tile[2], Size);
			}
		}
		for (unsigned i = 0; i < sizeof(kSS) / sizeof(kSS[0]); i++) if (savedS[i] != wantS[i]) d->SetSamplerState(0, kSS[i], savedS[i]);
		for (unsigned i = 0; i < sizeof(kRS) / sizeof(kRS[0]); i++) if (savedRS[i] != want[i]) d->SetRenderState(kRS[i], savedRS[i]);
		d->SetPixelShaderConstantF(0, savedPS, 1);
		if (rt0) { d->SetRenderTarget(0, rt0); rt0->Release(); }
		d->SetDepthStencilSurface(zs); if (zs) zs->Release();
		d->SetViewport(&vp);
		d->SetTexture(0, tex0); if (tex0) tex0->Release();
		if (decl) { d->SetVertexDeclaration(decl); decl->Release(); } else d->SetFVF(fvf);
		state->SetVertexShader(vs, false); if (vs) vs->Release();
		state->SetPixelShader(ps, false); if (ps) ps->Release();

		if (GpuTimer::Enabled && ++StatFrames >= 240) {
			const double f = 1.0 / StatFrames;
			Logger::Log("SHADOW ATLAS per frame: %.2f live tiles converted, %.1f pool tiles used (%.2f converted), %.2f lamps without a tile",
				StatLive * f, StatPool * f, StatPoolConverted * f, StatNoTile * f);
			StatFrames = 0; StatLive = StatPool = StatPoolConverted = StatNoTile = 0;
		}
	}
}
