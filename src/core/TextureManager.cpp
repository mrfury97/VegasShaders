#include <algorithm>
#include <unordered_set>
#include "CapturedNormals.h"	// UNOFFICIAL 2026-10-28 CapturedNormals
#include "ObjectMotion.h"	// UNOFFICIAL 2026-10-29 ObjectMotion
#include "SkyCoverage.h"	// UNOFFICIAL 2026-11-05 (Balebandro)
#include "BackDepth.h"	// UNOFFICIAL 2026-10-31 BackDepth

namespace {
	// A depth-stencil texture the shaders can sample (the INTZ format), screen sized.
	HRESULT MakeReadableDepth(IDirect3DDevice9* device, UInt32 w, UInt32 h, IDirect3DTexture9** out) {
		return device->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, out, NULL);
	}

	// Points every texture record in the list at the given texture.
	void Rebind(WaterMapList& records, IDirect3DBaseTexture9* texture) {
		for (TextureRecord* record : records) record->Texture = texture;
	}
}

void TextureManager::Initialize() {
	Logger::Log("Starting the textures manager...");
	TimeLogger startup = TimeLogger();

	TextureManager* mgr = new TextureManager();
	TheTextureManager = mgr;

	IDirect3DDevice9* dev = TheRenderManager->device;
	const UInt32 screenW = TheRenderManager->width;
	const UInt32 screenH = TheRenderManager->height;

	// NVR's own colour targets (each comes with a surface so it can be rendered to)
	mgr->InitTexture("TESR_SourceBuffer", &mgr->SourceTexture, &mgr->SourceSurface, screenW, screenH, D3DFMT_A16B16G16R16F);
	mgr->InitTexture("TESR_RenderedBuffer", &mgr->RenderedTexture, &mgr->RenderedSurface, screenW, screenH, D3DFMT_A16B16G16R16F);

	// readable copies of the world's and of the first-person model's depth
	MakeReadableDepth(dev, screenW, screenH, &mgr->DepthTexture);
	mgr->RegisterTexture("TESR_DepthBufferWorld", (IDirect3DBaseTexture9**)&mgr->DepthTexture);
	MakeReadableDepth(dev, screenW, screenH, &mgr->DepthTextureViewModel);
	mgr->RegisterTexture("TESR_DepthBufferViewModel", (IDirect3DBaseTexture9**)&mgr->DepthTextureViewModel);
	// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h): made on first use (empty until then: readers keep the depth-rebuilt normal)
	mgr->RegisterTexture("TESR_CapturedNormalsBuffer", (IDirect3DBaseTexture9**)&CapturedNormals::Texture);
	mgr->RegisterTexture("TESR_MaterialBuffer", (IDirect3DBaseTexture9**)&CapturedNormals::MaterialTexture);
	mgr->RegisterTexture("TESR_AlbedoBuffer", (IDirect3DBaseTexture9**)&CapturedNormals::AlbedoTexture);	// UNOFFICIAL 2026-10-31 AlbedoBuffer (made on first use)
	mgr->RegisterTexture("TESR_BackDepthBuffer", (IDirect3DBaseTexture9**)&BackDepth::Texture);	// UNOFFICIAL 2026-10-31 BackDepth	// UNOFFICIAL 2026-10-29 MaterialBuffer
	mgr->RegisterTexture("TESR_ObjectMotionBuffer", (IDirect3DBaseTexture9**)&ObjectMotion::Texture);	// UNOFFICIAL 2026-10-29 ObjectMotion (made on first use)
	mgr->RegisterTexture("TESR_SkyCoverage", (IDirect3DBaseTexture9**)&SkyCoverage::Texture);	// UNOFFICIAL 2026-11-05 (Balebandro) SkyCoverage (the game's MSAA; made on first use)

	// let NVAPI see the two depth textures (for the depth copies)
	NvAPI_D3D9_RegisterResource(mgr->DepthTexture);
	NvAPI_D3D9_RegisterResource(mgr->DepthTextureViewModel);

	// UNOFFICIAL terrain blending ([Shaders.Terrain.Blending]): its depth copy exists only when the feature was on at startup,
	// which is also when the land shaders are compiled with it (TERRAIN_BLEND, ShaderRecord::LoadShader).
	// UNOFFICIAL 2026-10-27: also with [Main.Main.Performance] NvrDxvkDisplacement on at startup: displacement's ObjectBlend reads the
	// same copy (TESR_DisplaceDepth, the land pixel shaders' s6 for their tessellation shaders; NVR_DISPLACE_DEPTH).
	if (TheSettingManager->GetSettingI("Shaders.Terrain.Blending", "Enabled") || TheSettingManager->GetSettingI("Main.Main.Performance", "NvrDxvkDisplacement")) {
		if (SUCCEEDED(MakeReadableDepth(dev, screenW, screenH, &mgr->TerrainBlendDepthTexture))) {
			mgr->RegisterTexture("TESR_TerrainBlendDepth", (IDirect3DBaseTexture9**)&mgr->TerrainBlendDepthTexture);
			mgr->RegisterTexture("TESR_DisplaceDepth", (IDirect3DBaseTexture9**)&mgr->TerrainBlendDepthTexture);
			NvAPI_D3D9_RegisterResource(mgr->TerrainBlendDepthTexture);
		}
		else {
			mgr->TerrainBlendDepthTexture = nullptr;
			Logger::Log("[ERROR] Terrain blending: could not create its depth texture; blending (and displacement's ObjectBlend) stays off");
		}
	}

	// the water maps are slots the water hooks fill in later
	mgr->RegisterTexture(WordWaterHeightMapBuffer, &mgr->WaterHeightMapB);
	mgr->RegisterTexture(WordWaterReflectionMapBuffer, &mgr->WaterReflectionMapB);

	startup.LogTime("TextureManager::Initialize");
}

// Makes a render-target texture (optionally with an auto-generated mip chain), hands back its top surface and registers it
// under the given sampler name.
void TextureManager::InitTexture(const char* samplerName, IDirect3DTexture9** target, IDirect3DSurface9** topSurface, int w, int h, D3DFORMAT fmt, bool mipmaps) {
	const UINT levels = mipmaps ? 0 : 1;
	const DWORD usage = mipmaps ? (D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP) : D3DUSAGE_RENDERTARGET;
	const HRESULT result = TheRenderManager->device->CreateTexture(w, h, levels, usage, fmt, D3DPOOL_DEFAULT, target, NULL);
	if (FAILED(result)) {
		Logger::Log("[ERROR] : Failed to init texture %s", samplerName);
		EffectStatus::D3DFail("CreateTexture (TextureManager::InitTexture)", result);	// UNOFFICIAL 2026-11-01
		return;
	}

	EffectStatus::TargetSize(samplerName, (unsigned)w, (unsigned)h);	// UNOFFICIAL 2026-11-01: logged when made at another size than before
	(*target)->GetSurfaceLevel(0, topSurface);
	RegisterTexture(samplerName, reinterpret_cast<IDirect3DBaseTexture9**>(target));
}

// Makes a texture slot known to shaders under a sampler name (TESR_...).
void TextureManager::RegisterTexture(const char* samplerName, IDirect3DBaseTexture9** slot) {
	Logger::Log("Registering Texture %s", samplerName);
	TextureNames[samplerName] = slot;
}

// A texture file loaded before, by its path (nullptr if it never was).
IDirect3DBaseTexture9* TextureManager::GetCachedTexture(std::string& path) {
	auto found = TextureCache.find(path);
	return found != TextureCache.end() ? found->second : nullptr;
}


/*
* A sampler whose texture file failed to load asks for it by name again at every bind, so the error is reported once
* per name: a tester's log had 53,000 identical lines (four per frame), each written and flushed on the render thread.
*/
static void ReportMissingTexture(const std::string& Name) {
	static std::unordered_set<std::string> reported;
	if (reported.insert(Name).second)
		Logger::Log("[ERROR] Texture %s not found (reported once; the shaders using it run without it).", Name.c_str());
}

// The slot of a named texture, so a caller can follow a slot whose texture is swapped later.
IDirect3DBaseTexture9** TextureManager::GetTextureSlotByName(std::string& Name) {
	auto found = TextureNames.find(Name);
	if (found != TextureNames.end()) return found->second;
	ReportMissingTexture(Name);
	return nullptr;
}

// The texture a named slot holds right now.
IDirect3DBaseTexture9* TextureManager::GetTextureByName(std::string& samplerName) {
	IDirect3DBaseTexture9** slot = GetTextureSlotByName(samplerName);
	return slot ? *slot : nullptr;
}


// Loads a texture file of the given kind once; later calls (also after a failure) get the cached result.
IDirect3DBaseTexture9* TextureManager::GetFileTexture(std::string TexturePath, TextureRecord::TextureRecordType Kind) {
	IDirect3DBaseTexture9* loaded = GetCachedTexture(TexturePath);
	if (loaded) return loaded;

	IDirect3DDevice9* dev = TheRenderManager->device;
	const char* file = TexturePath.data();
	if (Kind == TextureRecord::TextureRecordType::PlanarBuffer)
		D3DXCreateTextureFromFileA(dev, file, reinterpret_cast<IDirect3DTexture9**>(&loaded));
	else if (Kind == TextureRecord::TextureRecordType::VolumeBuffer)
		D3DXCreateVolumeTextureFromFileA(dev, file, reinterpret_cast<IDirect3DVolumeTexture9**>(&loaded));
	else if (Kind == TextureRecord::TextureRecordType::CubeBuffer)
		D3DXCreateCubeTextureFromFileA(dev, file, reinterpret_cast<IDirect3DCubeTexture9**>(&loaded));
	else
		Logger::Log("[ERROR] : Invalid texture type %i for %s", Kind, TexturePath.c_str());

	if (!loaded) {
		// UNOFFICIAL 2026-10-30 (tester log, P84 v4: "NVR's texture files are missing or incomplete" with every NVR texture loaded): the
		// hint below came with ANY file that failed, and the one that failed was BloodFlow's optional loose replacer of a vanilla decal
		// (Data\Textures\decals\fxbloodsplatterdecals01.dds; the game's own is inside its BSA, which this loader does not read). Only
		// NVR's own folders get the error and the hint now; another path is a plain line (the effect that names it does without).
		std::string lower = TexturePath;
		for (char& c : lower) c = c == '/' ? '\\' : (char)tolower((unsigned char)c);
		const bool nvrOwn = lower.find("textures\\effects\\") != std::string::npos || lower.find("textures\\water\\") != std::string::npos ||
			lower.find("textures\\precipitations\\") != std::string::npos || lower.find("textures\\newvegasreloaded\\") != std::string::npos;
		if (!nvrOwn) Logger::Log("Texture file %s is not there as a loose file (optional; the effect that names it does without)", TexturePath.c_str());
		else {
			Logger::Log("[ERROR] : Couldn't load texture file %s", TexturePath.c_str());
			// NVR's own textures (Effects, Water, Precipitations) ship with the mod; when they are missing, effects still
			// run but read black instead (ambient occlusion loses its noise pattern and bands, SMAA stops smoothing).
			static bool hinted = false;
			if (!hinted) {
				hinted = true;
				Logger::Log("[ERROR] : Vegas Shaders' texture files are missing or incomplete (Data\\Textures\\Effects, Water, Precipitations). "
					"Effects that use them will look wrong. Install the complete Vegas Shaders package, including its Textures folder.");
			}
		}
	}
	else Logger::Log("Loaded texture file %s", TexturePath.c_str());

	TextureCache[TexturePath] = loaded;	// a failed load is cached too (as nullptr)
	return loaded;
}


// The game hands over its current water height map; every record bound to it follows. (Kept as a raw pointer: it may
// crash if the game frees the map while a record still points at it.)
void TextureManager::SetWaterHeightMap(IDirect3DBaseTexture9* HeightMap) {
	if (HeightMap == WaterHeightMapB) return;
	WaterHeightMapB = HeightMap;
	Rebind(TheTextureManager->WaterHeightMapTextures, HeightMap);
}

// Same for the water reflection map.
void TextureManager::SetWaterReflectionMap(IDirect3DBaseTexture9* ReflectionMap) {
	if (ReflectionMap == WaterReflectionMapB) return;
	WaterReflectionMapB = ReflectionMap;
	Rebind(TheTextureManager->WaterReflectionMapTextures, ReflectionMap);
}


// Debug: writes a texture's top level to .\Test\<name>.jpg.
void TextureManager::DumpToFile(IDirect3DTexture9* Source, const char* FileName) {
	IDirect3DSurface9* top = nullptr;
	Source->GetSurfaceLevel(0, &top);
	if (!top) {
		Logger::Log("Surface is null for %s", FileName);
		return;
	}
	std::string target = std::string(".\\Test\\") + FileName + ".jpg";
	D3DXSaveSurfaceToFileA(target.c_str(), D3DXIFF_JPG, top, NULL, NULL);
	top->Release();
}
