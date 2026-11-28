#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

// UNOFFICIAL 2026-10-19 [Shaders.ShadowsExteriors.TerrainShadows]: the land's shadows on the land, far past the sun's cascades
// (mountains darkening the valleys beyond them at a low sun).
//
// Credits and licence: ported from Conor's (mrfury97) NVR fork, github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, branch
// testbed (6043062a, bae95aef, 9000473d, 2026-10-16; GPL-3.0-or-later, the same licence as this project), which follows
// Community Shaders' "Terrain Shadows" (github.com/community-shaders/skyrim-community-shaders, features/Terrain Shadows,
// GPL-3.0 with the Modding and Linking exceptions; FO4 port github.com/northaxosky/fallout4-community-shaders). The worker-thread
// load and sweep below are Conor's; changed for this build: when no xLODGen height map is installed, the heights are read
// straight from the plugins in load order (LandHeights.h, this build's own code), and the result is applied by the
// SunFar pass (SunFar.h) to the sun's share of the light only.
//
// The xLODGen height map (FNVLODGen: Terrain LOD > Height maps), one per worldspace, is used when present:
//   Data\Textures\Terrain\<Worldspace>\<Worldspace>.HeightMap.<S>.<W>.<N>.<E>.<min z>.<max z>.dds
// 16-bit, 32 points per cell (128 units apart), 32767 = height 0, steps of 8 units.
//
// The heights are read on a worker thread when the worldspace changes (shrunk to MaxResolution a side by averaging). From
// them, each texel's shadow height -- below it the sun is hidden by land between it and the sun -- is swept toward the sun
// on a worker thread whenever the sun has moved a little: each texel takes the higher of its own height and the shadow
// height of the texel before it on the light's path, lowered by how far the light drops over one step. Two channels, the
// sun's elevation minus and plus Softness, give the soft edge; the map's own ground height is kept in a texture of its own
// (written only when the map or its encoding changes, not with every sweep: half the bytes swept and uploaded). The SunFar
// pass compares each pixel's height with them, never taking a pixel as lower than that ground: the LOD land on screen is
// simplified and sits below the map in places, which would otherwise shadow itself (and flicker with TAA's jitter).
class TerrainShadowMap {
public:
	struct SettingsStruct {
		bool	Enabled;
		UINT	MaxResolution;     // texels on the longer side at most
		float	Bias;              // shadow heights lowered by this (hides the land shadowing its own slopes)
		float	Softness;          // degrees either side of the sun's elevation
		float	Strength;
		float	Darkness;          // share of the ambient and sky light the land's shadow also takes
		float	Contrast;          // steepens the shadow's edge (1 as swept)
		int		DebugView;
		bool	UseHeightMapFile;  // an installed xLODGen height map first (else only the plugins' own land)
		bool	Deferred;          // (2026-11-28) the Vulkan deferred sun takes the shadow off its sunlight (core/TerrainShadowBridge.h)
	};
	SettingsStruct	Settings = {};

	struct ConstantsStruct {
		D3DXVECTOR4	Data;     // x west edge, y north edge (world), z 1 / width (units), w strength (0: off or not ready)
		D3DXVECTOR4	Range;    // x lowest encoded height, y encoded range, z bias, w blur (game units)
		D3DXVECTOR4	Params;   // x 1 / height (units), y DebugView, z Darkness, w Contrast
	};
	ConstantsStruct	Constants = {};

	IDirect3DTexture9*	Texture = nullptr;         // G16R16: r upper, g lower shadow height (TESR_TerrainShadowBuffer), each sweep
	IDirect3DTexture9*	GroundTexture = nullptr;   // L16 (G16R16 where L16 cannot be made): the ground (TESR_TerrainShadowGround), per map

	void	UpdateSettings();
	void	RegisterConstants();
	void	RegisterTextures();
	// Once a frame outdoors, from ShadowManager::RenderShadowMaps. SunDir points at the sun (or moon): the real direction
	// the lighting uses, not the shadow maps' quantized one (15 degree steps would make low-sun shadows half again too long
	// or short, and jump).
	void	Update(IDirect3DDevice9* Device, const D3DXVECTOR3& SunDir);
	bool	Active() const { return Constants.Data.w > 0.0f; }
	// UNOFFICIAL 2026-11-28: this frame's map and constants for the deferred sun (core/TerrainShadowBridge.h); after Update, and where
	// the shadow maps are off this frame (strength 0 then).
	void	Publish() const;

private:
	struct HeightMap {
		std::vector<float>	Heights;   // row 0 north, column 0 west: each texel's average (the ground)
		std::vector<float>	Peaks;     // ...and its highest point (what casts: averaging flattens ridge tops)
		UINT	Width = 0, Height = 0;
		float	West = 0.0f, North = 0.0f;   // the centre of texel (0, 0)
		float	Texel = 128.0f;              // game units between texel centres
		void*	WorldSpace = nullptr;
		std::string	Message;                 // logged when done
	};
	struct SweepResult {
		size_t	Count = 0;   // texels written to SweepBuffer (0: failed)
		float	Low = 0.0f, Range = 1.0f, Blur = 0.0f;
		UINT	Generation = 0;
		std::string	Error;   // set if it failed (out of memory), logged
		float	Shadowed = 0.0f;   // share of the map's texels in the land's shadow (DebugMode log)
		D3DXVECTOR3	SunDir = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	};
	// What a load needs from the game, copied on the main thread (the worker never touches game data).
	struct LoadRequest {
		void*		WorldSpace = nullptr;
		std::string	Name;                    // its editor id (the xLODGen folder and file name)
		std::string	DefiningFile;            // the plugin that defines it
		UInt32		ObjectID = 0;            // its form id's low 24 bits
		std::vector<std::string>	Plugins;   // load order
		UINT		MaxResolution = 2048;
		bool		UseHeightMapFile = true;
	};

	// Both run on worker threads and never throw: a failure comes back as an empty result and a message.
	static HeightMap	Load(LoadRequest Request);
	static bool			LoadHeightMapFile(const LoadRequest& Request, HeightMap& Out);
	static SweepResult	Sweep(std::shared_ptr<const HeightMap> Map, std::vector<UINT32>* Packed, D3DXVECTOR3 SunDir, float Softness, float Bias, UINT Generation);
	void	ReleaseTexture();

	void*				RequestedWorldSpace = nullptr;   // the land worldspace a load was started for
	UINT				RequestedResolution = 0;
	bool				RequestedFile = true;
	std::future<HeightMap>	LoadJob;
	bool				Loading = false;
	std::shared_ptr<const HeightMap>	Map;   // the loaded heights (empty: none for this worldspace), shared with a running sweep
	UINT				Generation = 0;      // bumped by every load

	std::future<SweepResult>	SweepJob;
	bool				Sweeping = false;
	// The packed sweep, written by the worker while Sweeping and read by the strip upload while Uploading (never both: a
	// sweep starts only once the last upload is done). Kept from sweep to sweep: a new block of this size every couple of
	// seconds fragments the game's 32-bit address space over hours of play until the allocation fails, and the shadows stay
	// where the last good sweep left them.
	std::vector<UINT32>	SweepBuffer;
	ULONGLONG			BusySince = 0;   // GetTickCount64 when the running sweep or upload began (stall log)
	bool				StallLogged = false;
	UINT				LockFailures = 0;   // texture locks that failed (DebugMode status line)
	UINT				SweepsStarted = 0, SweepsUploaded = 0;
	D3DXVECTOR3			SweptSunDir = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	UINT				SweptGeneration = ~0u;
	float				SweptBias = -1.0f, SweptSoftness = -1.0f;
	ULONGLONG			SettingsChangedAt = 0;   // GetTickCount64 of the last change to MaxResolution, Bias or Softness:
	                                             // reloads and sweeps for them wait until the menu has been left alone
	bool				Uploaded = false;    // the texture holds a sweep, of UploadedWorldSpace's map
	UINT				UploadedGeneration = ~0u;
	// A sweep of the same map with the sun moved is copied into the texture a strip of rows a frame (all of it at once took
	// milliseconds on the main thread); the encoding does not depend on the sun, so rows from the two sweeps read the same
	// while it runs.
	SweepResult			PendingUpload;
	UINT				PendingRow = 0;
	bool				Uploading = false;
	void*				UploadedWorldSpace = nullptr;
	UINT				TextureWidth = 0, TextureHeight = 0;
	bool				GroundTwoChannels = false;   // the ground texture is a G16R16 (L16 could not be made)
	float				LastFade = 1.0f;     // for the DebugMode log
};
