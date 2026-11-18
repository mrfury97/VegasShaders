#pragma once

// sampler name / file path -> texture, and sampler name -> the slot holding a texture that may be swapped
typedef std::map<std::string, IDirect3DBaseTexture9*>	TextureList;
typedef std::map<std::string, IDirect3DBaseTexture9**>	TexturePointersList;
typedef std::vector<TextureRecord*>						WaterMapList;

// Owns NVR's screen-sized buffers, the named texture slots shaders bind by sampler name, and the cache of texture files.
// Created once at startup (Initialize) and kept for the whole session.
class TextureManager {
public:
	static void				Initialize();

	void					InitTexture(const char* Name, IDirect3DTexture9** Texture, IDirect3DSurface9** Surface, int Width, int Height, D3DFORMAT Format, bool mipmaps = false);
	void					RegisterTexture(const char* Name, IDirect3DBaseTexture9** Slot);
	void					SetWaterHeightMap(IDirect3DBaseTexture9* HeightMap);
	void					SetWaterReflectionMap(IDirect3DBaseTexture9* ReflectionMap);
	IDirect3DBaseTexture9*	GetFileTexture(std::string TexturePath, TextureRecord::TextureRecordType Kind);
	IDirect3DBaseTexture9*	GetCachedTexture(std::string& path);
	IDirect3DBaseTexture9*	GetTextureByName(std::string& Name);
	IDirect3DBaseTexture9**	GetTextureSlotByName(std::string& Name);
	void					DumpToFile(IDirect3DTexture9* Source, const char* FileName);

	// UNOFFICIAL 2026-11-12 (memleak) idle targets, [Main.Main.Performance] TargetReleaseSeconds (live; 0 = off: every target is made at
	// start-up and kept for the session, as before). Two leaks in one: an effect's render targets were made at start-up whether the
	// effect was on or not, and stayed for the session after it was switched off (a 32-bit game: every megabyte of them is address
	// space the game cannot use; a player short of memory who switched effects off got nothing back).
	// InitTexture notes every target it makes (the slot, the size, the format). A target whose slot lies inside an effect's object
	// belongs to that effect (NoteEffect: its address range), and when the effect says so (EffectRecord::TargetParkable), the target
	// follows the effect's switch: off at start-up, or off for TargetReleaseSeconds, the slot holds a 1 x 1 stand-in of the same
	// format instead (zero filled: what a never-drawn target reads as); switched on, the real one is made again before the frame draws
	// (TargetsFrame, from ShaderManager::UpdateConstants and at the start of each effect chain). The effect's code never sees a null
	// pointer and needs no change; the swap forgets the old texture everywhere a raw pointer may sit (ForgetTarget).
	struct OwnedTarget {
		std::string				Name;
		IDirect3DTexture9**		Texture;
		IDirect3DSurface9**		Surface;
		int						Width, Height;
		D3DFORMAT				Format;
		bool					Mipmaps;
		EffectRecord*			Owner;		// null: nobody's (a static, or an effect that does not park): only listed
		bool					Parked;		// the slot holds the stand-in
	};
	struct EffectSpan { EffectRecord* Effect; const char* Begin; const char* End; unsigned long long OffSince; };
	std::vector<OwnedTarget>	Targets;
	std::vector<EffectSpan>		EffectSpans;
	EffectRecord*			StartingEffect = nullptr;	// the effect being registered at start-up (its Enabled is not set yet) ...
	bool					StartingWanted = false;		// ... and its switch in the settings
	float					TargetReleaseSeconds = 20.0f;
	bool					TargetInventory = false;	// [Main.Develop.Main] TargetInventory: a log line per target made
	unsigned long long		TargetBytesParked = 0;		// the real sizes of the targets parked now
	void					NoteEffect(EffectRecord* Effect, size_t Bytes, bool WantedAtStart);
	void					TargetsFrame();
	static unsigned long long	TargetBytes(int Width, int Height, D3DFORMAT Format, bool Mipmaps);
private:
	EffectSpan*				SpanOf(const void* Slot);
	bool					OwnerWanted(EffectRecord* Owner);
	void					ReadTargetSettings();
	int						SwapTarget(OwnedTarget& Target, bool Park);	// 1 swapped, 0 could not (tried again later), -1 not ours any more
	unsigned long long		TargetRetryAt = 0;	// after a swap that could not be made: nothing is tried before this tick
	void					ForgetTarget(const std::string& Name, IDirect3DBaseTexture9* Texture, IDirect3DSurface9* Surface);
public:

	// screen buffers (texture + its top surface where it is a render target)
	IDirect3DTexture9		*SourceTexture;
	IDirect3DSurface9		*SourceSurface;
	IDirect3DTexture9		*RenderedTexture;
	IDirect3DSurface9		*RenderedSurface;
	IDirect3DTexture9		*BloomTexture;
	IDirect3DSurface9		*BloomSurface;
	IDirect3DTexture9		*DepthTexture, *DepthTextureViewModel;	// readable (INTZ) depth: world, first-person model
	IDirect3DTexture9*		TerrainBlendDepthTexture = nullptr;	// UNOFFICIAL terrain blending: the world's depth as the land starts (Terrain.h)
	IDirect3DTexture9*		ParticleDepthTexture = nullptr;	// UNOFFICIAL 2026-11-04 soft particles: the world's depth as the first lit particle draws (Particles.h)
	IDirect3DSurface9		*CombinedDepthSurface;
	IDirect3DTexture9		*CombinedDepthTexture, *DepthTextureINTZ;
	IDirect3DSurface9		*DepthSurface;

	TextureList				TextureCache;		// loaded files by path
	TexturePointersList		TextureNames;		// sampler name -> slot
	WaterMapList			WaterHeightMapTextures, WaterReflectionMapTextures;	// records that follow the game's water maps
	IDirect3DBaseTexture9	*WaterHeightMapB, *WaterReflectionMapB;			// the game's current water maps
};
