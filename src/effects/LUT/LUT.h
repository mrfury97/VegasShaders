#pragma once

class LUTEffect : public EffectRecord
{
public:
	LUTEffect() : EffectRecord("LUT") {};

	struct LUTStruct {
		D3DXVECTOR4 Data;   // x=day N (cell size/count), y=strength, z=night N, w=interior N (0 = no usable LUT)
		D3DXVECTOR4 Blend;  // x=dayNightLerp (0=night, 1=day), y=isInterior (0 or 1), z=HDRCompat
	};

	struct LUTSettingsStruct {
		float Strength;
		bool  PreTonemapping, HDRCompat;
	};

	LUTStruct          Constants;
	LUTSettingsStruct  Settings;

	// the loaded strip textures, one per slot
	IDirect3DTexture9 *DayTexture = nullptr, *NightTexture = nullptr, *InteriorTexture = nullptr;

	std::vector<std::string> LUTFiles;		// file names found in LUTFolder, sorted
	int DayIdx = 0, NightIdx = 0, InteriorIdx = 0;	// each slot's position in LUTFiles

	void RegisterConstants();
	void RegisterTextures();
	void UpdateSettings();
	void UpdateConstants();
	// UNOFFICIAL 2026-10-23 (NVR DXVK): the pass as src\hlsl\NewVegas\Compute\LUT.frag (EffectRecord::ExtRender)
	bool	ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) override;
	ExtPass	extPass;
	bool ShouldRender() override;

	void ScanLUTFolder();

	// Slot numbers below: 0 = day, 1 = night, 2 = interior.

	// Puts an already-loaded texture into a slot: the member pointer, the sampler binding, the slot's
	// cell count and strip check, and the LUTFiles cursor. Nothing is loaded or saved here
	// (TextureManager::GetFileTexture() loads, SaveLUTSetting() saves).
	void AssignLUTSlot(int slot, IDirect3DBaseTexture9* texture, const char* filename);

	// Stores a slot's file name in the settings.
	void SaveLUTSetting(int slot, const char* filename);

	// What a user pick runs: load the file with TheTextureManager->GetFileTexture(), assign it, save the choice.
	void LoadLUT(int slot, const char* filename);

	static const char* LUTFolder();	// Paths::LutDir()
	// 2026-11-17: a LUT name from a setting, preset or script is a file in the LUT folder (or below it): no "..", drive or leading slash.
	static bool SafeName(const char* filename);

private:
	// Cell count N of each slot's strip LUT (day, night, interior), passed to the shader per slot. 0 means
	// the slot has no usable LUT (file missing or not an N*N x N strip): the shader passes colours through.
	float CellCount[3] = {};

	// Set when the slot's texture is an identity strip, or when the slot has no usable LUT. A pass that
	// only sees such slots returns its input, so ShouldRender skips it (the shipped neutral_lut.png is one).
	bool DayNeutral = true, NightNeutral = true, InteriorNeutral = true;
};
