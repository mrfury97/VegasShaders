#pragma once
// GunFX built into Vegas Shaders (2026-11-16, gunfx-builtin; docs/GUNFX.md, docs/next/GUNFX-BUILTIN.md).
// Until then GunFX was its own xNVSE plugin, GunFX.dll, and Vegas Shaders found these functions with GetProcAddress as its GunFX_*
// exports. Now they are plain functions of GunFX.cpp (its own translation unit, no game headers), called directly.
// This header holds no game types, so it can be included on both sides.
namespace GunFX {
	bool Load(unsigned runtimeVersion);	// Vegas Shaders' NVSEPlugin_Load (game only), after its settings are read
	void MainGameLoop();				// xNVSE's kMessage_MainGameLoop, from Vegas Shaders' message handler

	// The menu's master switches (Main > GunFX): bits 0 puff, 1 heat smoke, 2 ejection smoke, 3 barrel glow, 4 heat haze, 5 muzzle
	// blast, 6 energy weapons too, 7 after-fire trail, 8 volumetric smoke. Starts as all on; Vegas Shaders sets it before any frame.
	void SetSwitches(unsigned bits);

	// Barrel heat for the glow (out[10]: muzzle xyz, strength, rearward axis xyz, length, radius, forward length). True when the
	// heat belongs to weaponNode (the node or one of its parents).
	bool GetHeatV2(float out[8], const void* weaponNode);
	bool GetHeatV3(float out[10], const void* weaponNode);
	// Heat haze and the muzzle blast (V3: out[16]; see GunFX.cpp). True while either shows.
	bool GetHaze(float out[8], const void* weaponNode);
	bool GetHazeV2(float out[14], const void* weaponNode);
	bool GetHazeV3(float out[16], const void* weaponNode);
	// Volumetric smoke: records of 16 floats (V2) or 12 (the first), params[8]; returns how many records.
	int GetVolumeSmoke(float* out, int maxRecords, float* params);
	int GetVolumeSmoke2(float* out, int maxRecords, float* params);
	void SetView(const float* v);			// the camera: eye xyz (world), the way it looks xyz
	int GetBullets(float* out, int maxBullets);	// recent bullets, 8 floats each
	void GetBulletLook(float* out);		// out[2]: seconds a bullet's hole takes to open, to fill in again
	float GetMoving();						// how much you are moving, 0..1
	int GetSmokeHaze(float* out, int maxBlobs);	// the gun-smoke haze, 8 floats a blob
	void GetSmokeFlash(float* out);			// out[8]: the last muzzle flash's light on the smoke
}
