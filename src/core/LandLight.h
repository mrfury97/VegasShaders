#pragma once
// UNOFFICIAL 2026-10-25 (bug: a tree's shadow under a cloud's shadow came out lighter than the clouded ground round it): the light
// vector the game hands the land's pixel shaders (TerrainTemplate.hlsl SunDir, c18), for the SunFar pass (SunFar.fx.hlsl) to split
// the land's light the way the land shader lit it. It is not the directional light's own direction (TESR_SunDirection, what the
// object shaders light with): measured at the Poseidon gas station, 17.4 h, it pointed the same way across the ground but stood
// 10.8 degrees high instead of 27.4, so on flat land the sun's NdotL was well under half of what SunFar took it for, and a cloud
// took away more light than the sun gave there. Nothing tells which it is beforehand, so it is read back: right after a land
// pixel shader has been bound and drawn with, at the next NVR shader bind (ShaderRecord::SetCT, before that shader's constants
// go up), c18 still holds what the land drew with. Kept only if it is a unit vector pointing across the ground the way the sun
// does (a later draw of a game shader could have put something else there).
namespace LandLight {
	inline bool		Pending = false;			// a land pixel shader was bound since the last read
	inline float	Dir[3] = { 0.0f, 0.0f, 0.0f };
	inline UInt32	Frame = 0;					// the frame of the last good read (SunFarEffect::UpdateConstants counts them in Now)
	inline UInt32	Now = 0;
	// UNOFFICIAL 2026-11-15 (DeferredSun; bench batches 3-6) the land's light as the LAND's shaders draw with it, for the deferred sun pass,
	// which lights the land pixels of the full deferred renderer: the near land (TerrainTemplate) gets its direction in the pixel constant c18
	// (SunDir), the far land (TerrainLODTemplate) in the VERTEX constant c25 (LightData), both their colour in the pixel c3 (measured in the
	// og-ground and Goodsprings recordings: c3 = 1.21 x the directional light's Diff, the direction the directional light's). Read at the
	// land's own DRAW (ReadAtDraw, from LinearScene's draw hook, armed by DeferredSun at a lighting-free land record's bind), when this draw's
	// constants are certainly on the device: batch 6 showed reads at the next shader bind missing whole views (the far land is the span's last
	// draw, so its "next bind" was the deferred passes', after other passes had set constants). Kept apart from Dir above (SunFar's, with its
	// heading check against the sun disc, which the deferred pass does not want). Runs only while ReadColor is set (DeferredSun::BeginFrame:
	// the full deferred renderer in mode 2, or the pass's own test switch).
	inline float	DeferredDir[3] = { 0.0f, 0.0f, 0.0f };
	inline float	Color[3] = { 0.0f, 0.0f, 0.0f };
	inline UInt32	ReadFrame = 0;
	inline bool		ColorGood = false;
	inline bool		ReadColor = false;
	inline unsigned	ReadsNear = 0, ReadsFar = 0, Rejected = 0;	// (DeferredSun's F10 line)
	// (2026-11-15 batch 6, frame replay of Goodsprings noon: the objects' c3 is 1.21 x the directional light's Diff x Dimmer too, as the land's)
	// the objects' sun colour as the game hands them (PSLightColor[0], c3, of a record whose light 0 is the sun), read at such a draw.
	inline float	ObjectColor[3] = { 0.0f, 0.0f, 0.0f };
	inline UInt32	ObjectReadFrame = 0;
	inline unsigned	ReadsObject = 0;
	inline void ReadObjectAtDraw(IDirect3DDevice9* d) {
		if (!ReadColor || !TheFrameRateManager || !d) return;
		float c[4] = {};
		if (FAILED(d->GetPixelShaderConstantF(3, c, 1)) || c[0] < 0.0f || c[1] < 0.0f || c[2] < 0.0f || c[0] >= 64.0f || c[1] >= 64.0f || c[2] >= 64.0f) { Rejected++; return; }
		ObjectColor[0] = c[0]; ObjectColor[1] = c[1]; ObjectColor[2] = c[2];
		ObjectReadFrame = (UInt32)TheFrameRateManager->FrameNumber;
		ReadsObject++;
	}

	inline void ReadAtDraw(IDirect3DDevice9* d, bool farLand) {
		if (!ReadColor || !TheFrameRateManager || !d) return;
		float v[4] = {}, c[4] = {};
		const HRESULT hv = farLand ? d->GetVertexShaderConstantF(25, v, 1) : d->GetPixelShaderConstantF(18, v, 1);
		if (FAILED(hv) || FAILED(d->GetPixelShaderConstantF(3, c, 1))) { Rejected++; return; }
		const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (length < 0.98f || length > 1.02f || v[2] < -0.05f ||
			c[0] < 0.0f || c[1] < 0.0f || c[2] < 0.0f || c[0] >= 64.0f || c[1] >= 64.0f || c[2] >= 64.0f) { Rejected++; return; }
		const float z = v[2] > 0.0f ? v[2] : 0.0f;
		const float clampedLen = sqrtf(v[0] * v[0] + v[1] * v[1] + z * z);
		const float invLen = clampedLen > 1.0e-6f ? 1.0f / clampedLen : 1.0f / length;
		DeferredDir[0] = v[0] * invLen; DeferredDir[1] = v[1] * invLen; DeferredDir[2] = z * invLen;
		Color[0] = c[0]; Color[1] = c[1]; Color[2] = c[2];
		ColorGood = true;
		ReadFrame = (UInt32)TheFrameRateManager->FrameNumber;
		if (farLand) ReadsFar++; else ReadsNear++;
	}

	inline void Read() {
		Pending = false;
		float v[4] = {};
		if (FAILED(TheRenderManager->device->GetPixelShaderConstantF(18, v, 1))) return;
		const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (length < 0.98f || length > 1.02f || v[2] <= 0.0f) return;
		const D3DXVECTOR4& sun = TheShaderManager->ShaderConst.SunDir;
		const float a = sqrtf(v[0] * v[0] + v[1] * v[1]), b = sqrtf(sun.x * sun.x + sun.y * sun.y);
		if (a > 0.05f && b > 0.05f && (v[0] * sun.x + v[1] * sun.y) / (a * b) < 0.99f) return;
		Dir[0] = v[0] / length; Dir[1] = v[1] / length; Dir[2] = v[2] / length;
		Frame = Now;
	}
}
