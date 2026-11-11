#pragma once

// UNOFFICIAL 2026-11-09 WeaponMask: which pixels of the first-person model are the WEAPON (user: "we need a way for weapon reflections to
// affect only weapons and not hands"). [Shaders.Reflections.RT] Weapon took every pixel of the first-person model for the weapon
// (TESR_DepthBufferViewModel: arms, hands, gloves, sleeves, gun, Pip-Boy), so the hands and arms shone too (a glove is grey: metal, to the
// WeaponMetal guess). With [Shaders.Reflections.RT] WeaponHands off (the default) the weapon reflects alone:
//   Begin (Render.cpp RenderFirstPersonHook, just before the game's first-person render): the stencil bit 0x80 is cleared over the whole
//      screen (a full-screen quad that writes only that bit: colour and depth off).
//   DrawBegin / DrawEnd (Render.cpp MaterialDraw hooks, round every object draw of the first-person scene): the draw writes the bit where
//      it passes the depth test (stencil REPLACE): 1 for a draw of the weapon's own geometry (a descendant of the first-person SkinInfo's
//      WeaponNode: the barrel haze finds the gun the same way), 0 for every other draw. The last draw to pass at a pixel decides, as an id
//      buffer does: a hand in front of the gun leaves 0 there. A draw that has the stencil test on for itself is left alone.
//   End (after the first-person render): a full-screen quad passes where the bit is 1 into a mask (multisampled with the game's MSAA, then
//      resolved: the share of the pixel's samples), TESR_WeaponMask (A8R8G8B8, red), for the frame's reflection passes.
// Reflections.glsl WeaponAt() then takes a first-person pixel for the weapon only where the mask is above a half (uniform weapon.x = 2).
// Needs a stencil in the game's depth surface (INTZ / D24S8) and the object draw hooks (Render.cpp InstallMaterialDrawHooks). Anything
// missing: no mask (weapon.x stays 1) and the weapon pixels are the whole first-person model, as before; the log says why once.
// The device state round the two quads is the one SkyCoverage.h keeps (a state block, the targets and the viewport by hand).
#include <unordered_set>
#include <d3dx9.h>
#include "PassSums.h"	// AfterStateBlock

namespace WeaponMask {
	inline bool Wanted = false;				// [Shaders.Reflections.RT] Enabled, Weapon above 0 and WeaponHands off (ReflectionsEffect::UpdateSettings)
	inline bool Failed = false;				// no stencil in the depth surface, or the mask could not be made: off for the session
	inline bool HooksOn = false;			// Render.cpp InstallMaterialDrawHooks hooked both object draw paths (the tags come from them)
	inline bool ToldNoDraws = false;
	inline bool Tagging = false;			// the first-person pass in progress tags its draws
	inline UInt32 ReadyFrame = 0xFFFFFFFFu;	// the frame whose first-person pass left a mask
	inline NiNode* WeaponNode = nullptr;	// this pass's: the first-person skin info's
	inline IDirect3DSurface9* MaskMS = nullptr;	// the multisampled mask the quad is drawn into (with the game's MSAA)
	inline IDirect3DTexture9* Texture = nullptr;	// the mask the reflection passes read (TESR_WeaponMask)
	inline IDirect3DSurface9* TextureSurface = nullptr;
	inline IDirect3DPixelShader9* WhitePS = nullptr;
	inline IDirect3DPixelShader9* BlackPS = nullptr;
	inline IDirect3DStateBlock9* State = nullptr;
	inline D3DSURFACE_DESC Made = {};
	inline UInt32 PassWeaponDraws = 0, PassOtherDraws = 0, Passes = 0, LoggedPasses = 0;
	inline UInt32 LastWeaponDraws = 0xFFFFFFFFu, LastOtherDraws = 0xFFFFFFFFu, LoggedChanges = 0;	// (a line whenever a pass's counts differ from the last one's, at most 12)
	inline constexpr DWORD kBit = 0x80;		// the stencil bit (the game's own are left alone: the writes and the clear take this one only)

	inline bool ReadyThisFrame() { return Texture && TheFrameRateManager && ReadyFrame == TheFrameRateManager->FrameNumber; }

	inline void Release() {
		if (MaskMS) { MaskMS->Release(); MaskMS = nullptr; }
		if (TextureSurface) { TextureSurface->Release(); TextureSurface = nullptr; }
		if (Texture) { Texture->Release(); Texture = nullptr; }
		Made = D3DSURFACE_DESC();
		ReadyFrame = 0xFFFFFFFFu;
	}

	inline IDirect3DPixelShader9* Compile(IDirect3DDevice9* d, const char* source) {
		IDirect3DPixelShader9* shader = nullptr;
		ID3DXBuffer* code = nullptr; ID3DXBuffer* errors = nullptr;
		if (FAILED(D3DXCompileShader(source, (UINT)strlen(source), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) || !code ||
			FAILED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &shader))) {
			Logger::Log("UNOFFICIAL WeaponMask: shader failed (%s): off for the session", errors ? (const char*)errors->GetBufferPointer() : "?");
			shader = nullptr;
		}
		if (errors) errors->Release();
		if (code) code->Release();
		return shader;
	}

	// A depth-stencil surface with stencil bits to write: INTZ (the game's, when NVR reads its depth) or D24S8.
	inline bool HasStencil(D3DFORMAT f) {
		return f == (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z') || f == D3DFMT_D24S8 || f == D3DFMT_D24FS8;
	}

	inline bool Ensure(IDirect3DDevice9* d, const D3DSURFACE_DESC& t) {
		if (Failed) return false;
		if (Texture && Made.Width == t.Width && Made.Height == t.Height && Made.MultiSampleType == t.MultiSampleType && Made.MultiSampleQuality == t.MultiSampleQuality) return true;
		Release();
		const bool multisampled = t.MultiSampleType != D3DMULTISAMPLE_NONE;
		if ((multisampled && FAILED(d->CreateRenderTarget(t.Width, t.Height, D3DFMT_A8R8G8B8, t.MultiSampleType, t.MultiSampleQuality, FALSE, &MaskMS, NULL))) ||
			FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &Texture, NULL)) ||
			FAILED(Texture->GetSurfaceLevel(0, &TextureSurface))) {
			Logger::Log("UNOFFICIAL WeaponMask: could not make the mask (%ux%u, msaa %u): the weapon reflections keep the whole first-person model", t.Width, t.Height, (unsigned)t.MultiSampleType);
			Release();
			Failed = true;
			return false;
		}
		if (!WhitePS) WhitePS = Compile(d, "float4 main() : COLOR0 { return 1.0; }");
		if (!BlackPS) BlackPS = Compile(d, "float4 main() : COLOR0 { return 0.0; }");
		if (!WhitePS || !BlackPS) { Release(); Failed = true; return false; }
		Made = t;
		Logger::Log("UNOFFICIAL WeaponMask: on (mask %ux%u, msaa %u): the weapon reflections cover the weapon's own pixels, not the hands and arms ([Shaders.Reflections.RT] WeaponHands)",
			t.Width, t.Height, (unsigned)t.MultiSampleType);
		return true;
	}

	// The device state round the quads below, put back as it was (a state block; the targets and viewport by hand), as SkyCoverage::Scope.
	struct Scope {
		IDirect3DDevice9* d;
		IDirect3DSurface9* target = nullptr; IDirect3DSurface9* depth = nullptr; IDirect3DSurface9* extra[3] = {};
		D3DVIEWPORT9 viewport = {};
		D3DSURFACE_DESC t = {};
		bool ok = false;
		explicit Scope(IDirect3DDevice9* device) : d(device) {
			if (Failed || !d) return;
			d->GetRenderTarget(0, &target);
			d->GetDepthStencilSurface(&depth);
			D3DSURFACE_DESC z = {};
			if (!target || !depth || FAILED(target->GetDesc(&t)) || FAILED(depth->GetDesc(&z))) return;
			if (!HasStencil(z.Format)) {
				Failed = true;
				Logger::Log("UNOFFICIAL WeaponMask: the game's depth surface (format %08X) has no stencil bits: the weapon reflections keep the whole first-person model", (unsigned)z.Format);
				return;
			}
			// the game's own colour target and depth surface (the same multisampling, the depth at least as big), not a map's or a probe's
			if (z.MultiSampleType != t.MultiSampleType || z.Width < t.Width || z.Height < t.Height || !Ensure(d, t) ||
				(!State && FAILED(d->CreateStateBlock(D3DSBT_ALL, &State)))) return;
			State->Capture();
			d->GetViewport(&viewport);
			for (DWORD i = 1; i < 4; i++) if (SUCCEEDED(d->GetRenderTarget(i, &extra[i - 1])) && extra[i - 1]) d->SetRenderTarget(i, nullptr);
			ok = true;
		}
		~Scope() {
			if (ok) {
				d->SetRenderTarget(0, target);
				for (DWORD i = 1; i < 4; i++) if (extra[i - 1]) { d->SetRenderTarget(i, extra[i - 1]); extra[i - 1]->Release(); }
				d->SetDepthStencilSurface(depth);
				d->SetViewport(&viewport);	// (SetRenderTarget resets it)
				State->Apply();
				if (PassSums::AfterStateBlock) PassSums::AfterStateBlock(d);	// (the lean hooks re-read the device's state: SkyCoverage.h)
			}
			if (target) target->Release();
			if (depth) depth->Release();
		}
		// A full-screen quad into the bound target, the depth test off, against the bound depth surface's stencil: func / ref / read mask /
		// write mask / the op where it passes, and the colour write mask.
		void Quad(DWORD func, DWORD ref, DWORD readMask, DWORD writeMask, DWORD passOp, DWORD colourWrite, IDirect3DPixelShader9* ps) {
			static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE, D3DRS_STENCILFUNC,
				D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
				D3DRS_TWOSIDEDSTENCILMODE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE,
				D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK };
			const DWORD want[] = { D3DZB_FALSE, FALSE, D3DCMP_ALWAYS, TRUE, func, ref, readMask, writeMask, passOp, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP,
				FALSE, FALSE, FALSE, D3DCULL_NONE, colourWrite, FALSE, 0, FALSE, FALSE, TRUE, 0xFFFFFFFF };
			static_assert(sizeof(kStates) / sizeof(kStates[0]) == sizeof(want) / sizeof(want[0]), "WeaponMask::Scope::Quad states");
			for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) d->SetRenderState(kStates[i], want[i]);
			const D3DVIEWPORT9 full = { 0, 0, t.Width, t.Height, 0.0f, 1.0f };
			d->SetViewport(&full);
			d->SetVertexShader(nullptr);
			d->SetPixelShader(ps);
			d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			const float x0 = -0.5f, y0 = -0.5f, x1 = (float)t.Width - 0.5f, y1 = (float)t.Height - 0.5f;
			const float quad[4][6] = { { x0, y0, 0.5f, 1.0f, 0.0f, 0.0f }, { x1, y0, 0.5f, 1.0f, 1.0f, 0.0f }, { x0, y1, 0.5f, 1.0f, 0.0f, 1.0f }, { x1, y1, 0.5f, 1.0f, 1.0f, 1.0f } };
			d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
		}
	};

	// Just before the game's first-person render: the stencil bit cleared over the screen, and this pass's draws tagged from now on.
	inline void Begin(IDirect3DDevice9* d) {
		Tagging = false;
		if (!Wanted || Failed || !HooksOn || !d) return;
		SkinInfo* skin = Player ? Player->firstPersonSkinInfo : nullptr;
		WeaponNode = skin ? skin->WeaponNode : nullptr;
		{
			Scope s(d);
			if (!s.ok) return;
			s.Quad(D3DCMP_ALWAYS, 0, 0xFF, kBit, D3DSTENCILOP_REPLACE, 0, BlackPS);	// ref 0 into the bit, everywhere; nothing else written
		}
		PassWeaponDraws = PassOtherDraws = 0;
		Tagging = true;
	}

	// One object draw of the first-person scene: the stencil states the draw leaves with are put back by DrawEnd, so the game's state cache and the
	// device agree again.
	struct DrawState { bool tagged = false; DWORD func = 0, pass = 0, fail = 0, zfail = 0, ref = 0, write = 0; };

	inline bool InWeapon(NiGeometry* geometry) {
		if (!WeaponNode) return false;
		NiAVObject* node = geometry;
		for (int depth = 0; node && depth < 32; ++depth, node = node->m_parent) if (node == WeaponNode) return true;
		return false;
	}

	// The first draws of the session, by geometry, named in the log (at most 48 lines): which ones went for the weapon, which for the rest. A report
	// that the hands still reflect, or the gun does not, starts here.
	inline std::unordered_set<const void*> NamedGeometry;
	inline void NameDraw(NiGeometry* geometry, bool weapon) {
		if (!geometry || NamedGeometry.size() >= 48 || !NamedGeometry.insert(geometry).second) return;
		NiNode* parent = geometry->m_parent;
		Logger::Log("UNOFFICIAL WeaponMask: %s draw: '%s' under '%s' (%s)", weapon ? "WEAPON" : "rest", geometry->m_pcName ? geometry->m_pcName : "",
			parent && parent->m_pcName ? parent->m_pcName : "", geometry->skinInstance ? "skinned" : "still");
	}

	inline void DrawBegin(IDirect3DDevice9* d, NiGeometry* geometry, DrawState& s) {
		s = DrawState();
		if (!Tagging || !d) return;
		DWORD enabled = TRUE;
		if (FAILED(d->GetRenderState(D3DRS_STENCILENABLE, &enabled)) || enabled) return;	// the draw's own stencil test: left as it is
		const bool weapon = InWeapon(geometry);
		NameDraw(geometry, weapon);
		d->GetRenderState(D3DRS_STENCILFUNC, &s.func);
		d->GetRenderState(D3DRS_STENCILPASS, &s.pass);
		d->GetRenderState(D3DRS_STENCILFAIL, &s.fail);
		d->GetRenderState(D3DRS_STENCILZFAIL, &s.zfail);
		d->GetRenderState(D3DRS_STENCILREF, &s.ref);
		d->GetRenderState(D3DRS_STENCILWRITEMASK, &s.write);
		d->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
		d->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);	// where the draw passes the depth test (and the alpha test)
		d->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
		d->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
		d->SetRenderState(D3DRS_STENCILREF, weapon ? kBit : 0);
		d->SetRenderState(D3DRS_STENCILWRITEMASK, kBit);
		d->SetRenderState(D3DRS_STENCILENABLE, TRUE);
		s.tagged = true;
		if (weapon) PassWeaponDraws++; else PassOtherDraws++;
	}

	inline void DrawEnd(IDirect3DDevice9* d, const DrawState& s) {
		if (!s.tagged || !d) return;
		d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		d->SetRenderState(D3DRS_STENCILFUNC, s.func);
		d->SetRenderState(D3DRS_STENCILPASS, s.pass);
		d->SetRenderState(D3DRS_STENCILFAIL, s.fail);
		d->SetRenderState(D3DRS_STENCILZFAIL, s.zfail);
		d->SetRenderState(D3DRS_STENCILREF, s.ref);
		d->SetRenderState(D3DRS_STENCILWRITEMASK, s.write);
	}

	// Right after the game's first-person render (its depth-stencil surface still bound, the stencil holding the tags): the mask.
	inline void End(IDirect3DDevice9* d, UInt32 frame) {
		if (!Tagging) return;
		Tagging = false;
		if (PassWeaponDraws + PassOtherDraws == 0) {	// no draw of this pass came through the hooks (nothing drawn, or another path): nothing to trust
			if (!ToldNoDraws) {
				ToldNoDraws = true;
				Logger::Log("UNOFFICIAL WeaponMask: no first-person draw came through the object draw hooks this pass: no mask, the weapon reflections keep the whole first-person model");
			}
			return;
		}
		Scope s(d);
		if (!s.ok) return;
		IDirect3DSurface9* mask = MaskMS ? MaskMS : TextureSurface;
		d->SetRenderTarget(0, mask);
		d->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
		s.Quad(D3DCMP_EQUAL, kBit, kBit, 0, D3DSTENCILOP_KEEP, 0xF, WhitePS);	// white where the weapon's draws were the last to pass
		s.Quad(D3DCMP_ALWAYS, 0, 0xFF, kBit, D3DSTENCILOP_REPLACE, 0, BlackPS);	// and the bit back to 0 everywhere: nothing after this pass sees our tags (colour off: the mask is not touched)
		if (MaskMS) d->StretchRect(MaskMS, NULL, TextureSurface, NULL, D3DTEXF_NONE);	// the resolve averages the samples: the share of the pixel
		ReadyFrame = frame;
		Passes++;
		const bool changed = PassWeaponDraws != LastWeaponDraws || PassOtherDraws != LastOtherDraws;
		if (LoggedPasses < 4 || (Passes % 3600) == 0 || (changed && LoggedChanges < 12)) {
			if (LoggedPasses >= 4 && changed && (Passes % 3600) != 0) LoggedChanges++;
			LoggedPasses++;
			Logger::Log("UNOFFICIAL WeaponMask: first-person pass %u: weapon node %s, %u draws tagged as the weapon, %u as the rest (hands, arms, Pip-Boy)",
				Passes, WeaponNode ? "found" : "none (nothing held)", PassWeaponDraws, PassOtherDraws);
		}
		LastWeaponDraws = PassWeaponDraws;
		LastOtherDraws = PassOtherDraws;
	}
}
