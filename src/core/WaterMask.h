#pragma once

// UNOFFICIAL 2026-11-12 PTWaterMask ([Main.Main.RayTracing] PTWaterMask, on; docs/next/PT-COMPAT-AUDIT-2026-11-12.md item 4 "Water"): which
// pixels of the frame's world scene the game drew WATER on, for the path tracer. Under PTPrimary the first surface of a pixel comes from
// the frame's own buffers (depth, AlbedoBuffer, MaterialBuffer, captured normals), written only by NVR's opaque world draws. Water is not
// one of them and the traced scene holds no water at all, so a water pixel came out as the dry bed's texture lit at the water's depth, as a
// flat plain sheet or as the game's water, by view angle and depth. With the mask the path tracer's prep pass (core/RtViewModel.h, c9 / s5)
// codes every water pixel -1, "the game's own pixel" (RtPathTrace.comp ScreenHit: unwritten, gScreenDrawnElse; the trace's exact answer,
// no denoiser there).
// How the game draws water (read from the game's code at these addresses, not yet seen in a run; no hook of NVR's sat there): the
// accumulator's 0x00B65C60 draws the pass groups 0x288-0x289 (the water's stencil passes), 0x28A-0x2AB (the water) and 0x2AC-0x2CE, AFTER
// every opaque group and BEFORE the sorted blended stage (0x00B650C0, RenderBlendedHook): GlassMask's stage hook never sees it. Each water
// draw sets the game's own stencil test (the water shader's setup 0x00B864D0: STENCILENABLE on, ref 255, a read mask per water type, a
// pass op that changes the value, write mask 255; by Gamebryo's tables EQUAL and INCR: how overlapping water planes draw once), so a
// stencil bit of ours at the draw (core/WeaponMask.h's way) would clash with it, and the
// mod's own water pixel shaders cannot tag the pixels either (the player may run the game's water shaders: [Shaders.Water] off; with MSAA a
// second render target would have to match the scene's samples). So, as GlassMask does for glass: Render.cpp RenderPassStandardHook, for a
// pass whose geometry carries a water shade property (NiShadeProperty kProp_Water), in the frame's first world scene and on the world's own
// depth surface (never the water reflection render, a cube map or another off-screen scene), hooks DrawIndexedPrimitive for the length of
// that one pass and draws each of its draws once more into a screen-sized mask with a "return 1" pixel shader: the game's z test as the
// draw had it (reversed depth or not; whether or not water wrote depth), no depth write, no blending, no alpha test, no fog, the stencil
// test OFF (the game's INCR already moved the value its EQUAL test passed on), the draw's own viewport (RenderScale below 1: the world's
// top-left rect). With the game's MSAA the mask is multisampled like the scene and resolved when the prep pass asks for it.
// Cost: one extra draw per water draw (a few a frame) and their state changes; nothing at all while no path traced frame asked for the mask
// in the last two frames (Request, from RtPathTrace.cpp Record) or with PTWaterMask = false; nothing while the camera is under water (left
// as it was). A view model or anything else drawn later over the water: the prep pass keeps the first-person model's own handling; any
// other later draw over a water pixel is shown as the game drew it (the game's pixel holds it).
#include <d3d9.h>
#include <d3dx9.h>
#include <vector>

// UNOFFICIAL 2026-11-12 PTGrassOwnPixel ([Main.Main.RayTracing] PTGrassOwnPixel; core/CapturedNormals.h GrassOwnDraw): the same mask also takes
// GRASS that wrote no record of its own (the game's own grass shaders: lamp-lit grass, all grass with [Shaders.Grass.Status] off; any
// grass with [Main.Main.Quality] CapturedGrass off) and was drawn after the land: its pixels had the land's records behind them, and
// were the game's own pixel or the LAND's colour by the view angle (RtViewModel.h, "GRASS under the in-front rule"). Redrawn into the mask
// (depth test EQUAL: only where that draw's own depth is in the depth buffer, so not where its alpha test or fade cut it), they are the
// game's own pixel from every angle. GrassOn works without Enabled (the water's key): the mask is then made by the grass alone.
namespace WaterMask {
	inline bool Enabled = true;					// [Main.Main.RayTracing] PTWaterMask (RtPathTrace.cpp's settings read; a missing key: on)
	inline bool Failed = false;					// the mask could not be made: off for the session
	inline UInt32 WantedFrame = 0xFFFFFFFFu;	// the last frame a path traced PTPrimary frame asked for the mask (Request)
	inline UInt32 ClearedFrame = 0xFFFFFFFFu;	// the frame the mask was last cleared in (before its first water draw)
	inline UInt32 WrittenFrame = 0xFFFFFFFFu;	// the frame a water draw last wrote it
	inline bool Resolved = false;				// this frame's multisampled mask is in Texture
	inline IDirect3DTexture9* Texture = nullptr;  inline IDirect3DSurface9* TextureSurface = nullptr;	// what the prep pass reads (s5)
	inline IDirect3DSurface9* MsaaSurface = nullptr;	// with the game's MSAA: what the redraws write, resolved into Texture
	inline D3DSURFACE_DESC Made = {};
	inline D3DVIEWPORT9 Rect = {};				// the viewport of the frame's water draws (the world's render rect)
	inline IDirect3DPixelShader9* PS2 = nullptr;  inline IDirect3DPixelShader9* PS3 = nullptr;	// "return 1" for vs_2_x / vs_3_0
	inline UInt32 Draws = 0, Frames = 0, LoggedDraws = 0;
	inline bool GrassOn = true;				// [Main.Main.RayTracing] PTGrassOwnPixel (RtPathTrace.cpp's settings read; a missing key: on)
	inline UInt32 GrassDraws = 0, GrassFrame = 0xFFFFFFFFu, GrassFrames = 0, GrassLogged = 0;

	// The path tracer takes its first surface from the screen this frame (RtPathTrace.cpp Record, every PTPrimary frame).
	inline void Request(UInt32 frame) { WantedFrame = frame; }
	// The world scene of `frame` should draw the mask: asked for in one of the last two frames (the request comes after the world is drawn).
	inline bool Wanted(UInt32 frame) { return Enabled && !Failed && WantedFrame != 0xFFFFFFFFu && frame - WantedFrame <= 2u; }

	inline void Release() {
		if (TextureSurface) { TextureSurface->Release(); TextureSurface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		if (MsaaSurface) { MsaaSurface->Release(); MsaaSurface = nullptr; }
		Made = {};
		ClearedFrame = WrittenFrame = 0xFFFFFFFFu;
	}

	inline IDirect3DPixelShader9* Compile(IDirect3DDevice9* d, const char* profile) {
		static const char kSource[] = "float4 main() : COLOR0 { return float4(1.0, 1.0, 1.0, 1.0); }";
		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* errors = nullptr;
		IDirect3DPixelShader9* ps = nullptr;
		if (SUCCEEDED(D3DXCompileShader(kSource, (UINT)strlen(kSource), NULL, NULL, "main", profile, 0, &code, &errors, NULL)) && code)
			d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
		if (code) code->Release();
		if (errors) errors->Release();
		return ps;
	}

	// The mask the size and sampling of the scene target the water draws into (made again when that changes). False: off for the session.
	inline bool Ensure(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (Failed) return false;
		if (Texture && Made.Width == t.Width && Made.Height == t.Height && Made.MultiSampleType == t.MultiSampleType && Made.MultiSampleQuality == t.MultiSampleQuality) return true;
		Release();
		if (!PS2) PS2 = Compile(d, "ps_2_0");
		if (!PS3) PS3 = Compile(d, "ps_3_0");
		bool ok = PS2 && PS3 &&
			SUCCEEDED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &Texture, NULL)) &&
			SUCCEEDED(Texture->GetSurfaceLevel(0, &TextureSurface));
		if (ok && t.MultiSampleType != D3DMULTISAMPLE_NONE)
			ok = SUCCEEDED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A8R8G8B8, t.MultiSampleType, t.MultiSampleQuality, FALSE, &MsaaSurface, NULL));
		if (!ok) {
			Release();
			Failed = true;
			Logger::Log("[WARNING] UNOFFICIAL RT path trace: PTWaterMask: the water mask could not be made (%ux%u, msaa %u): off for the session, water under path tracing as before.",
				t.Width, t.Height, (unsigned)t.MultiSampleType);
			return false;
		}
		Made = t;
		Logger::Log("UNOFFICIAL RT path trace: PTWaterMask: on (mask %ux%u, msaa %u): pixels the game draws water on keep the game's own pixel under path tracing",
			t.Width, t.Height, (unsigned)t.MultiSampleType);
		return true;
	}

	// Right after a water draw of the world scene: the same draw once more into the mask (the bound vertex shader, streams, constants, clip
	// planes, scissor and z test as they are). Everything this changes on the device is put back (the game's state cache never sees it).
	template <typename DrawFn> inline void Redraw(IDirect3DDevice9* d, UInt32 frame, DrawFn draw) {
		IDirect3DSurface9* rt0 = nullptr;
		D3DSURFACE_DESC t = {};
		if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return;
		if (FAILED(rt0->GetDesc(&t)) || !Ensure(d, t)) { rt0->Release(); return; }
		IDirect3DSurface9* mask = MsaaSurface ? MsaaSurface : TextureSurface;
		IDirect3DVertexShader9* vs = nullptr;
		d->GetVertexShader(&vs);
		UINT size = 0;
		DWORD version = 0;
		if (vs) {
			vs->GetFunction(NULL, &size);
			if (size >= 4) { std::vector<DWORD> code(size / 4); vs->GetFunction(code.data(), &size); version = code[0]; }
			vs->Release();
		}
		IDirect3DPixelShader9* mine = (version & 0xFFFF) >= 0x0300 ? PS3 : PS2;	// vs_3_0 needs ps_3_0; fixed function / vs_2_x take ps_2_0
		IDirect3DPixelShader9* ps = nullptr;
		IDirect3DSurface9* extra[3] = {};
		D3DVIEWPORT9 viewport = {};
		d->GetPixelShader(&ps);
		d->GetViewport(&viewport);
		static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ALPHABLENDENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE,
			D3DRS_SRGBWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_FOGENABLE };
		const DWORD want[] = { FALSE, FALSE, FALSE, 0xF, FALSE, FALSE, FALSE };
		DWORD saved[sizeof(kStates) / sizeof(kStates[0])] = {};
		for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) { if (FAILED(d->GetRenderState(kStates[i], &saved[i]))) saved[i] = want[i]; if (saved[i] != want[i]) d->SetRenderState(kStates[i], want[i]); }
		// (a second target still bound would be written by nothing here, but must match the mask's sampling: off for the draw)
		for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &extra[i - 1])) && extra[i - 1]) d->SetRenderTarget(i, nullptr);
		d->SetRenderTarget(0, mask);
		if (ClearedFrame != frame) {	// the frame's first water draw: the whole mask to "no water" (SetRenderTarget set the full viewport)
			d->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
			ClearedFrame = frame;
			Frames++;
		}
		d->SetViewport(&viewport);	// (SetRenderTarget reset it) the water's own: the world's render rect with RenderScale below 1
		d->SetPixelShader(mine);
		if (SUCCEEDED(draw())) { Draws++; WrittenFrame = frame; Resolved = false; Rect = viewport; }
		d->SetRenderTarget(0, rt0);
		for (DWORD i = 1; i < 4; i++) if (extra[i - 1]) { d->SetRenderTarget(i, extra[i - 1]); extra[i - 1]->Release(); }
		d->SetViewport(&viewport);
		d->SetPixelShader(ps);
		for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) if (saved[i] != want[i]) d->SetRenderState(kStates[i], saved[i]);
		if (ps) ps->Release();
		rt0->Release();
	}

	// What the prep pass binds this frame (null: no water drawn in this frame's world scene, or off): the multisampled mask is resolved
	// here, once (the share of the pixel's samples), so a frame without a path traced picture never pays for it.
	inline IDirect3DTexture9* ThisFrame(IDirect3DDevice9* d, UInt32 frame) {
		if ((!Enabled && !GrassOn) || Failed || !Texture || WrittenFrame != frame) return nullptr;
		if (!Resolved) {
			if (MsaaSurface && FAILED(d->StretchRect(MsaaSurface, NULL, TextureSurface, NULL, D3DTEXF_NONE))) return nullptr;
			Resolved = true;
			// the first frames with water, then now and then: a report that water still shows its bed starts here (no line = no water draw came
			// through the hook: the game drew it another way, or the mask was not asked for)
			if (Draws && (LoggedDraws < 3 || (Frames % 1800) == 0)) {	// (Draws: the mask may hold grass alone, PTGrassOwnPixel)
				LoggedDraws++;
				Logger::Log("UNOFFICIAL RT path trace: PTWaterMask: %u water draws redrawn into the mask over %u frames with water in view (%s; water rect %ux%u of %ux%u)",
					Draws, Frames, MsaaSurface ? "multisampled, resolved" : "single-sampled", (unsigned)Rect.Width, (unsigned)Rect.Height, Made.Width, Made.Height);
			}
		}
		return Texture;
	}

	// (PTGrassOwnPixel) right after a grass draw that wrote no record: the same draw once more into the mask, where its own depth stands
	// (EQUAL; the draw wrote depth where its pixels survived). The z function is put back.
	template <typename DrawFn> inline void RedrawGrass(IDirect3DDevice9* d, UInt32 frame, DrawFn draw) {
		if (!GrassOn || Failed) return;
		DWORD zfunc = D3DCMP_LESSEQUAL;
		if (FAILED(d->GetRenderState(D3DRS_ZFUNC, &zfunc))) return;
		d->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
		const UInt32 before = Draws;
		Redraw(d, frame, draw);
		d->SetRenderState(D3DRS_ZFUNC, zfunc);
		if (Draws == before) return;
		Draws = before;	// (the water's own count: Redraw counted this one)
		GrassDraws++;
		if (GrassFrame != frame) {
			GrassFrame = frame; GrassFrames++;
			if (GrassLogged < 3 || (GrassFrames % 1800) == 0) {
				GrassLogged++;
				Logger::Log("UNOFFICIAL RT path trace: PTGrassOwnPixel: %u grass draws without records redrawn into the mask over %u frames (grass drawn after the land by a shader that writes no record: the game's own pixel under path tracing, from every angle)",
					GrassDraws, GrassFrames);
			}
		}
	}
}
