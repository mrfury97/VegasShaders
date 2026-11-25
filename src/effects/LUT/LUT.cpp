#include <filesystem>
#include "../../core/DxvkCompute.h"	// UNOFFICIAL 2026-10-23 (NVR DXVK)
#include "LUT.h"
#include "LUTIdentity.h"
#include "LUTFile.h"

// LUT files go through LUTFile.h instead of the generic texture loader: images keep their exact size
// (no power-of-two resampling) and .cube files are read directly. Cached with the other file textures.
static IDirect3DBaseTexture9* GetLUTTexture(std::string path)
{
	if (IDirect3DBaseTexture9* cached = TheTextureManager->GetCachedTexture(path)) return cached;

	std::string error;
	IDirect3DTexture9* texture = LoadLUTTexture(TheRenderManager->device, path.c_str(), error);
	if (!texture) {
		Logger::Log("[ERROR] : Couldn't load LUT %s: %s", path.c_str(), error.c_str());
		return nullptr;
	}

	D3DSURFACE_DESC desc = {};
	texture->GetLevelDesc(0, &desc);
	Logger::Log("Loaded LUT %s (%ux%u)", path.c_str(), desc.Width, desc.Height);
	TheTextureManager->TextureCache[path] = texture;
	return texture;
}

// The three LUT slots, in slot order: day, night, interior.
namespace {
	struct SlotInfo { const char* sampler; const char* settingKey; };
	const SlotInfo kSlots[3] = {
		{ "TESR_LUTDayBuffer",      "DayLUT" },
		{ "TESR_LUTNightBuffer",    "NightLUT" },
		{ "TESR_LUTInteriorBuffer", "InteriorLUT" },
	};
}

// Data\Textures\VegasShaders\LUTs\ (or the old Data\Textures\NewVegasReloaded\LUTs\ of an older install, Paths.h)
const char* LUTEffect::LUTFolder() { return Paths::LutDir(); }

bool LUTEffect::SafeName(const char* filename) {
	if (!filename || !*filename || filename[0] == '\\' || filename[0] == '/' || strchr(filename, ':')) return false;
	for (const auto& part : std::filesystem::path(filename)) if (part == "..") return false;
	return true;
}

void LUTEffect::ScanLUTFolder()
{
	LUTFiles.clear();

	std::error_code ec;
	std::filesystem::directory_iterator it(LUTFolder(), ec), end;
	for (; it != end; ++it) {
		const std::filesystem::directory_entry& entry = *it;
		if (!entry.is_regular_file(ec)) continue;

		std::string extension = entry.path().extension().string();
		for (char& ch : extension) ch = (char)::tolower((unsigned char)ch);

		const bool supported = extension == ".png" || extension == ".dds" || extension == ".bmp" || extension == ".cube";
		if (supported) LUTFiles.push_back(entry.path().filename().string());
	}
	std::sort(LUTFiles.begin(), LUTFiles.end());
}

void LUTEffect::AssignLUTSlot(int slot, IDirect3DBaseTexture9* texture, const char* filename)
{
	if (!texture) return;
	if (slot < 0 || slot > 2) return;

	IDirect3DTexture9** target = slot == 0 ? &DayTexture : slot == 1 ? &NightTexture : &InteriorTexture;
	const char* samplerName = kSlots[slot].sampler;

	*target = (IDirect3DTexture9*)texture;
	ClearSampler(samplerName, strlen(samplerName));

	// Each slot keeps its own cell count (16 for 256x16, 32 for 1024x32, 64 for 4096x64). The day LUT's
	// count used to be applied to all three, which scrambled a night or interior LUT of another size.
	// A texture that is not an N*N x N strip cannot be sampled as one, so it is not used at all.
	D3DSURFACE_DESC desc = {};
	const bool hasDesc = SUCCEEDED((*target)->GetLevelDesc(0, &desc));
	const bool isStrip = hasDesc && desc.Height >= 2 && desc.Width == desc.Height * desc.Height;
	CellCount[slot] = isStrip ? (float)desc.Height : 0.0f;

	const bool neutral = !isStrip || IsIdentityLUT(*target);
	bool& neutralFlag = slot == 0 ? DayNeutral : slot == 1 ? NightNeutral : InteriorNeutral;
	neutralFlag = neutral;

	if (!isStrip)
		Logger::Log("UNOFFICIAL LUT %s is %ux%u, not a horizontal strip LUT (256x16, 1024x32 or 4096x64): it is ignored, "
			"because sampled as a strip it scrambles the colours.", filename, desc.Width, desc.Height);
	else if (neutral)
		Logger::Log("UNOFFICIAL LUT %s is an identity LUT; the grading pass is skipped while only identity LUTs are in use.", filename);

	// keep the menu's cycle arrows pointing at this file
	int& cursor = slot == 0 ? DayIdx : slot == 1 ? NightIdx : InteriorIdx;
	auto found = std::find(LUTFiles.begin(), LUTFiles.end(), filename);
	if (found != LUTFiles.end()) cursor = (int)(found - LUTFiles.begin());
}

void LUTEffect::SaveLUTSetting(int slot, const char* filename)
{
	if (slot < 0 || slot > 2) return;

	char value[80];
	strncpy_s(value, filename, sizeof(value) - 1);
	TheSettingManager->SetSettingS("Shaders.LUT.Main", kSlots[slot].settingKey, value);
}

void LUTEffect::LoadLUT(int slot, const char* filename)
{
	if (!SafeName(filename)) return;

	IDirect3DBaseTexture9* loaded = GetLUTTexture(std::string(LUTFolder()) + filename);
	if (!loaded) return;

	AssignLUTSlot(slot, loaded, filename);
	SaveLUTSetting(slot, filename);
}

void LUTEffect::RegisterConstants()
{
	ShaderManager* sm = TheShaderManager;
	sm->RegisterConstant("TESR_LUTData",  &Constants.Data);
	sm->RegisterConstant("TESR_LUTBlend", &Constants.Blend);
}

void LUTEffect::RegisterTextures()
{
	TheTextureManager->RegisterTexture(kSlots[0].sampler, (IDirect3DBaseTexture9**)&DayTexture);
	TheTextureManager->RegisterTexture(kSlots[1].sampler, (IDirect3DBaseTexture9**)&NightTexture);
	TheTextureManager->RegisterTexture(kSlots[2].sampler, (IDirect3DBaseTexture9**)&InteriorTexture);

	ScanLUTFolder();

	// Restore the saved picks. They are already persisted, so only load and assign.
	for (int slot = 0; slot < 3; slot++) {
		char saved[80];
		TheSettingManager->GetSettingS("Shaders.LUT.Main", kSlots[slot].settingKey, saved);
		if (!saved[0] || saved[0] == '"' || !SafeName(saved)) continue;

		IDirect3DBaseTexture9* loaded = GetLUTTexture(std::string(LUTFolder()) + saved);
		if (loaded) AssignLUTSlot(slot, loaded, saved);
	}
}

void LUTEffect::UpdateSettings()
{
	const char* section = "Shaders.LUT.Main";
	Settings.Strength       = TheSettingManager->GetSettingF(section, "Strength");
	Settings.PreTonemapping = TheSettingManager->GetSettingI(section, "PreTonemapping");
	Settings.HDRCompat      = TheSettingManager->GetSettingI(section, "HDRCompat");
}

bool LUTEffect::ShouldRender()
{
	// The shader returns lerp(color, graded, strength) with graded built from the LUTs it samples:
	// exteriors blend day and night, interiors use the interior LUT only (Blend.y). If every LUT
	// that can contribute is an identity the pass returns its input, so do not run it.
	if (Settings.Strength <= 0.0f) return false;
	const bool exterior = TheShaderManager->GameState.isExterior;
	return exterior ? !(DayNeutral && NightNeutral) : !InteriorNeutral;
}

void LUTEffect::UpdateConstants()
{
	Constants.Data = D3DXVECTOR4(CellCount[0], Settings.Strength, CellCount[1], CellCount[2]);

	const bool exterior = TheShaderManager->GameState.isExterior;
	Constants.Blend.x = exterior ? TheShaderManager->GameState.transitionCurve : 0.0f;
	Constants.Blend.y = exterior ? 0.0f : 1.0f;
	Constants.Blend.z = Settings.HDRCompat ? 1.0f : 0.0f;
}

// UNOFFICIAL 2026-10-23 (NVR DXVK): LUT.frag; a slot without a LUT (N < 2 in TESR_LUTData) passes through in the shader, so any
// texture may sit in its binding (the current image is used).
bool LUTEffect::ExtRender(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, UINT techniqueIndex, bool ClearRenderTarget, IDirect3DSurface9* SourceBuffer) {
	if (techniqueIndex != 0) return false;
	struct Push { D3DXVECTOR4 data, blend; UInt32 samplerIndex; UInt32 pad[3]; } push = {};
	push.data = Constants.Data;
	push.blend = Constants.Blend;
	IDirect3DBaseTexture9* current = TheTextureManager->RenderedTexture;
	IDirect3DBaseTexture9* textures[] = { current, DayTexture ? DayTexture : current, NightTexture ? NightTexture : current, InteriorTexture ? InteriorTexture : current };
	return ExtTexturePass(Device, RenderTarget, RenderedSurface, SourceBuffer, extPass, "LUT", textures, _countof(textures), &push, sizeof(push), 32);
}
