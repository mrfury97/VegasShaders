#pragma once

// UNOFFICIAL 2026-10-28 (NVR DXVK): the ray tracing core shared by NVR's RT effects (docs\next\FORK-RT-CORE.md).
//
// Windows' 32-bit Vulkan drivers offer no ray tracing, so the scene lives in a 64-bit helper, NvrRtHelper.exe (tools\nvr-rt-helper),
// on the same GPU. Every frame, at one point of the frame (RtCore::Frame, after the depth and normals exist, before the sun shadows),
// the core:
//   1. gathers the scene: the opaque static geometry of the loaded cells within [Main.Main.RayTracing] SceneRadius of the camera
//      (land and objects; not actors, alpha-tested or blended geometry, SpeedTree leaves, decals): each new geometry is converted
//      by the fork (ID3D9NvrExtRt::StageGeometry) into the shared arena image and built ONCE in the helper; each frame only the
//      instance list (camera-relative transforms) goes over;
//   2. asks every RT effect (a User) that wants to trace this frame to record: CopyIn its inputs into shared images, AddPass its
//      trace passes (compute shaders run by the helper), CopyOut its results into NVR textures;
//   3. records, in DXVK's command stream: the arena copy, the inputs, "inputs ready" = N, a GPU wait for "output ready" = N, the
//      outputs. Nothing waits on the CPU.
// Off (no user wants RT), native D3D9, stock DXVK, a fork without patch 0028, a GPU without ray tracing for 64-bit programs, or no
// helper: nothing runs and the users keep their old paths (the reason is logged once). A helper that dies or stops answering is
// ended by a watchdog thread, which releases the GPU wait; RT is then off for the session.
//
// Coordinates: scene space = world space minus the camera position (TheRenderManager->CameraPosition), Gamebryo units, z up;
// RenderManager::CreateD3DMatrix's convention. A trace pass gets what it needs to reconstruct positions in its own constants.

#include "NvrDxvkExt.h"

struct NvrRtShared;

class RtCore {
public:
	struct User {
		const char*	Name;
		bool		(*Wants)();							// it traces this frame
		void		(*Setup)();							// registers its images (AddImage) before each helper start
		void		(*Record)(IDirect3DDevice9* Device);	// this frame's CopyIn / AddPass / CopyOut
		UINT		Point = 0;							// (2026-10-28 RT reflections) 0: recorded in Frame; 1: only through Exchange
		UINT		Order = 0;							// (2026-11-05) Setup order at a helper start, low first: a user whose images do not fit
												// the protocol's NVR_RT_MAX_IMAGES is left out for the session (the others keep tracing)
	};

	static void			AddUser(const User& user);
	// Optional: the cull mask of each instance (default 0x01 land, 0x02 objects); one hook, the last set wins. 0 = never hit.
	static void			SetMaskHook(UInt32 (*Hook)(NiGeometry* Geometry, bool Land));

	// Setup only: a shared image (a pass's input or output); returns its index (or -1). Index 0 is the core's arena. The image is made
	// with both sides rounded up to 256 texels (the game's and the helper's drivers agree on the layout only then): the content is the
	// top-left Width x Height; give the logical size to the pass in its constants. CopyOut copies only that rectangle.
	static int			AddImage(const char* Name, UINT Width, UINT Height, NVR_EXT_SHARED_FORMAT Format);
	// (2026-11-07, protocol 6) in Setup: this many megabytes of the helper's scratch store (binding 20: zero at start, kept between frames,
	// never shared with the game) are needed; the helper makes the largest any user asked for
	static void			AskScratch(UINT Megabytes);
	// A trace shader, Data\Shaders\VegasShaders\Compute\Rt\<Name>.spv (the helper compiles it once); any time; -1 if full.
	static int			AddShader(const char* Name);

	// Record only:
	static bool			CopyIn(int Image, IUnknown* Source, const RECT* Rect = nullptr);
	// Up to 12 images (2026-10-30): Images[0..7] at bindings 2..9, Images[8..11] at 12..15 (records 10, geometry 11 unchanged;
	// a helper older than 2026-10-30 skips a pass with more than 8)
	static bool			AddPass(int Shader, const int* Images, UINT ImageCount, UINT WriteMask, const void* Constants, UINT ConstantsSize,
							UINT GroupsX, UINT GroupsY);
	// UNOFFICIAL 2026-11-07 (ShadowDenoiser): passes this frame record still has room for (NVR_RT_MAX_PASSES a frame), so a user that
	// needs two passes in a row (a trace and its denoiser) can take its one-pass path instead of losing the second
	static UINT			PassesLeft();
	static bool			CopyOut(int Image, IUnknown* Target);
	static bool			CopyOut(int Image, IUnknown* Target, UINT Width, UINT Height);	// only the top-left Width x Height (a pass that wrote less)

	static void			Frame(IDirect3DDevice9* Device);	// the RT point of the frame
	// UNOFFICIAL 2026-10-28 (RT reflections) a second exchange of the frame, for one Point-1 user, where that user calls it (Reflections:
	// between its ray generation and its resolve pass, in the post chain). It records only that user's Record as its own frame record
	// (its own semaphore values: the record counter counts exchanges), with this frame's scene: the instance list Frame gathered, or a
	// gather of its own when Frame did not exchange this frame. A second GPU bubble, only while such a user is on. True: the user's
	// outputs are recorded (ready for any pass recorded after this call).
	static bool			Exchange(IDirect3DDevice9* Device, const User& user);
#define NVR_RTCORE_EXCHANGE 1
	// UNOFFICIAL 2026-11-07 (SameFrame lamp shadows, docs/next/FORK-RT-LAMPS.md) the frame's FIRST exchange, before the world draw, for
	// one user, where that user calls it (RtLampShadows: right after TAA's jitter, before the world's first draw). It does Frame's start
	// checks itself (starts / polls the helper, the screen size) and gathers the frame's scene: the frame's later exchanges (Frame's
	// Point-0 users, a Point-1 user) reuse it. Outdoors with [Main.Main.RayTracing] Grass on it refuses (false): the grass is noted by
	// the world's own draws, which have not run yet; the user then traces at Frame as before. True: the user's outputs are recorded
	// (ready for the world's draws).
	static bool			ExchangeEarly(IDirect3DDevice9* Device, const User& user);
	// The eye in scene space. The scene is world minus TheRenderManager->CameraPosition, the NiCamera's world translation (the eye), so
	// this is (0, 0, 0). Use it for every ray origin from the camera: Pointers::Generic::CameraLocation is NOT the eye (it read ~0
	// in the game while the eye was 71000 units away: every ray started outside the scene, FORK-RT-SUN.md rtsun-4).
	static D3DXVECTOR3	Eye();
	static bool			Running();							// the helper is ready (users trace from the frame after it got ready)
	static bool			LeftOut(const char* Name);			// (2026-11-05) this user's images did not fit at the helper start: it does not trace
	// UNOFFICIAL 2026-11-02 (bench warm slot, shot command reloadshaders): stop a running helper; it starts again on the next frame an RT
	// effect wants it, so it loads every Compute\Rt\*.spv again (and settings read at its start: HitTextures ...). Users' Setup runs again.
	static void			Restart(const char* Why);
	static const char*	StatusText();
	static const NvrRtShared* Helper();						// the helper's counters (null when not running)
	static UInt32		Instances();						// this frame's
	static UInt32		Geometries();						// known (built or waiting for arena space)
	static UInt32		Waiting();							// geometries not yet sent (the arena was full this frame)
	static UInt32		Released();							// geometries released since start
	// UNOFFICIAL 2026-10-29 alpha-tested casters ([Main.Main.RayTracing] AlphaTested, read at each helper start): true while the scene
	// holds them (instance mask NVR_RT_MASK_ALPHA = 0x10, non-opaque: rays that do not test alpha must leave 0x10 out of their cull
	// mask). A pass that tests them adds AlphaAtlasImage() to its images and includes Compute\Rt\RtAlpha.glsl (FORK-RT-CORE.md).
	static bool			AlphaTested();
	static int			AlphaAtlasImage();					// the alpha atlas' image index (-1: none)
	// UNOFFICIAL 2026-10-29 hit textures ([Main.Main.RayTracing] HitTextures): the albedo atlas' image index (RGBA8; -1: none). A pass
	// that shades its hits adds it to its images and uses Compute\Rt\RtHit.glsl (rtHitAlbedo: the texture's colour, else the average).
	static int			AlbedoAtlasImage();
	// UNOFFICIAL 2026-10-30 glow textures ([Main.Main.RayTracing] GlowTextures, needs HitTextures): the glow atlas' image index (RGBA8,
	// same slots and texcoords as the albedo atlas; -1: none). A pass adds it to its images, #define RT_GLOW_ATLAS and calls rtHitGlow.
	static int			GlowAtlasImage();
	// UNOFFICIAL 2026-11-02 normal textures ([Main.Main.RayTracing] NormalTextures, with HitTextures): the normal atlas' image index
	// (RGBA8, the same slots; record flag NVR_RT_RECORD_NORMAL; RtHit.glsl rtHitNormalMapped with RT_NORMAL_ATLAS). -1: none.
	static int			NormalAtlasImage();
	// UNOFFICIAL 2026-10-29 (rt-lamp-shadows): true while the scene holds the posed skinned actors ([Main.Main.RayTracing] Actors +
	// fork patch 0033; instance mask NVR_RT_MASK_ACTOR = 0x20), so a pass can stop taking people's shadows from elsewhere
	static bool			Actors();
	// UNOFFICIAL 2026-10-30 (rt-ao-sky, GlowDirect): this frame's instances with a glow map (NVR_RT_RECORD_GLOW) as sphere lights: the
	// world bound's centre in scene space (camera relative, as the instances), its radius and the draw's average colour (the record's).
	struct GlowLight { float center[3]; float radius; float rgb[3]; };
	static const std::vector<GlowLight>& GlowLights();
	// UNOFFICIAL 2026-10-30 grass ([Main.Main.RayTracing] Grass, needs AlphaTested; GrassRadius, GrassMaxTufts live): the grass is not in
	// the cells' scene graph (the game places its tufts per draw from TallGrassShader's instance table), so each grass pass, right after
	// it is drawn (Render.cpp GrassFirst), hands its tufts here: the model's first tuft is built once (alpha-tested, its card texture in
	// the alpha atlas) and every tuft within GrassRadius of the camera this frame is an instance of it (the tuft's world position and
	// size step; no sway). GrassWanted: worth reading the pass's tufts (the helper runs, Grass and AlphaTested on).
	static bool			GrassWanted();
	static void			NoteGrass(NiGeometry* Geometry, IDirect3DBaseTexture9* Texture, const float* Instances, UINT Count, UINT VertsPerTuft,
							const float* ScaleMask, UInt32 AlphaRef);
};
