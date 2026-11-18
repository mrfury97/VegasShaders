#pragma once
// UNOFFICIAL 2026-11-14 the deferred passes' stored-curve fog (Shaders/Includes/DeferredPixel.hlsl FDAddFogged; docs/next/FD-SUN-RESULTS.md
// section 9). The templates lerp their fog in the stored curve, so a pass that adds light under fog must read the pixel it adds to and
// rebuild the forward lerp; this file gives DeferredSun and DeferredLamps the two things that needs: the fog colour the records lerped with,
// and a copy of the frame to read.
// When the game draws no fog (the VolumetricFog effect on, the default: the forward shaders are handed a fog property that fogs nothing,
// Render.cpp ShadowSceneNode__GetFogPropertyEx; or no fog property) every record wrote keep 1 and the plain add is exact: no copy is made.
// Otherwise the world scene's fog property is what every record's vertex shader took as FogColor (c15): its colour, in the stored curve
// (the same reading as RtPathTrace.cpp's PTGameFog).

namespace DeferredFog {
	inline IDirect3DTexture9* CopyTex = nullptr;
	inline IDirect3DSurface9* Copy = nullptr;
	inline D3DSURFACE_DESC CopyMade = {};
	inline bool CopyFailed = false;

	// c: rgb the fog colour (stored curve), w 1 = the exact path is needed this frame. False = no fog drawn (w 0).
	inline bool Params(float c[4]) {
		c[0] = c[1] = c[2] = c[3] = 0.0f;
		const bool gameFogDrawn = !(TheSettingManager->SettingsMain.Main.RenderEffects && TheShaderManager->Effects.VolumetricFog && TheShaderManager->Effects.VolumetricFog->Enabled &&
			!TheShaderManager->GameState.isUnderwater);
		if (!gameFogDrawn || CopyFailed) return false;
		const ShadowSceneNode* worldNode = BSShaderManager::GetShadowSceneNode(0);
		const BSFogProperty* fog = worldNode ? worldNode->fogProperty : nullptr;
		if (!fog) return false;
		c[0] = fog->m_kColor.r; c[1] = fog->m_kColor.g; c[2] = fog->m_kColor.b; c[3] = 1.0f;
		return true;
	}

	// The frame as it is now (target), copied for the pass to read (fp16 to fp16: exact). Null when it could not be made (the pass then adds plainly).
	inline IDirect3DTexture9* FrameCopy(IDirect3DDevice9* d, IDirect3DSurface9* target) {
		if (CopyFailed || !target) return nullptr;
		D3DSURFACE_DESC t = {};
		target->GetDesc(&t);
		if (!CopyTex || CopyMade.Width != t.Width || CopyMade.Height != t.Height || CopyMade.Format != t.Format) {
			if (Copy) { Copy->Release(); Copy = nullptr; }
			if (CopyTex) { CopyTex->Release(); CopyTex = nullptr; }
			if (FAILED(d->CreateTexture(t.Width, t.Height, 1, D3DUSAGE_RENDERTARGET, t.Format, D3DPOOL_DEFAULT, &CopyTex, NULL)) || FAILED(CopyTex->GetSurfaceLevel(0, &Copy))) {
				CopyFailed = true;
				Logger::Log("[WARNING] UNOFFICIAL DeferredFog: could not make the frame's copy: the deferred passes add their light plainly under fog (logged once)");
				return nullptr;
			}
			CopyMade = t;
		}
		if (FAILED(d->StretchRect(target, NULL, Copy, NULL, D3DTEXF_NONE))) return nullptr;
		return CopyTex;
	}
}
