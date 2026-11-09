#pragma once

#include "PointShadowSchedule.h"
#include "LampFixtures.h"
#include "RtLampShadows.h"	// UNOFFICIAL 2026-10-28
#include "FlashlightLamp.h"	// UNOFFICIAL 2026-11-07 (perf-flashlight) the flashlight's game lamp stays out of ManyLights' table (NoteManyLamp)
#include "ShadowPool.h"	// UNOFFICIAL 2026-10-29 ShadowPool (ShadowPoolMax)
#include "ShadowHold.h"	// UNOFFICIAL 2026-10-30 ShadowHold (FadeIn across live / pool)
#include <unordered_set>	// UNOFFICIAL 2026-10-30 TerrainRecords

// UNOFFICIAL interior forward point-light shadows (NVR-Lunacy\docs\interior-forward-shadows.md).
//
// Indoors the screen-space pass (Effects\PointShadows.fx + ShadowsInteriors.fx) could only darken the finished image,
// one mask for every light, lamp glow and ambient included. With [Shaders.ShadowsInteriors.Forward] Enabled, the lit
// pixel shaders (ObjectTemplate, SKIN2002/2006, SkinVPSTemplate, SM3002/3003/3005/3007) get an Interior record compiled
// with INTERIOR_SHADOWS 1 (Shaders\Includes\PointShadowForward.hlsl), and each point light's own direct light is
// multiplied by its own shadow cube map inside the object shader:
//  - ShadowManager::RenderShadowMaps: Begin() before anything, Publish() after the cube-map loop: the lights whose cube
//    map really is theirs this frame (drawn or kept), with the position and radius it was drawn for.
//  - SetShadersHook (Render.cpp): SetForDraw() matches the game's light list of the draw (BSShaderManager::
//    pCurrentRenderPass 0x011F91E0: +0x09 count, +0x0C ShadowSceneLight*, light k = PSLightColor[k], one later in the OPT
//    passes: FirstShaderLight) to those lights by
//    pointer, writes c174-c187 and binds the cube maps to s8, s11-s15.
//  - BSBatchRenderer::RenderPassImmediately_Standard/_Skinned (Render.cpp): OnRenderPass() does the same for every
//    object. The game binds shaders only when the pass type or shader changes and then draws a run of objects with
//    them, each with its own lamps; with SetForDraw alone the rest of a run kept the first object's (flicker).
//  - ShaderManager::RenderEffectsPreTonemapping: while this runs, the old screen-space shadows are not applied again;
//    KeepDarkening keeps their darkening of what no tracked lamp reaches (the mask without its cube-map lookups, one
//    apply pass, no blur unless DarkeningBlur), otherwise neither the mask nor the apply runs.
// Exteriors, and every shader while Enabled is off, are exactly as before (no Interior records are made).
namespace PointShadowForward {

	// BSShaderProperty::RenderPass, from the game's code: constructor 0x00BA8EC0, SetLights 0x00BA8C50,
	// ShadowLightShader::SetupGeometryConstants_Lights 0x00B78A90 (reads +0x09 and +0x0C),
	// BSShaderLightingProperty::SetLight1x2x 0x00B70820 (light k -> kShaderLightConstants[k + 1] = PSLightColor[k]).
	struct RenderPassView {
		NiGeometry*			geometry;	// 00
		UInt16				passEnum;	// 04
		UInt8				unk06;		// 06
		UInt8				unk07;		// 07
		UInt8				unk08;		// 08
		UInt8				numLights;	// 09
		UInt8				maxLights;	// 0A
		UInt8				unk0B;		// 0B
		ShadowSceneLight**	lights;		// 0C  light k = PSLightColor[k - FirstShaderLight(pass)]
	};
	static_assert(offsetof(RenderPassView, numLights) == 0x09 && offsetof(RenderPassView, lights) == 0x0C, "RenderPass layout");

	// UNOFFICIAL (2026-10-08): which light of the pass the pixel shader's PSLightColor[0] is. Every pass goes through
	// SetupGeometryConstants_Lights (light k -> PSLightColor[k], light 0 usually the cell's directional light) except the
	// two many-lamp OPT passes, which BSBatchRenderer::RenderPassImmediately_Standard 0x00B98E80 sets up itself:
	// 0xCA Setup_ADT4_Opt 0x00B7DED0 (ObjectTemplate PS SLS2032) and 0xD1 Setup_ADTS10_Opt 0x00B7DFE0 (SLS2035). Their
	// SetupGeometryOpt_Lights 0x00B7C850 / _LightsSpecular 0x00B7CB00 start at light 1 and write light k's colour to
	// kShaderLightConstants[k] (SetLight1x2x: [k + 1]), so there PSLightColor[0] is light 1 (its .w the lamp count, the
	// shader's lightsUsed; PSLightColor[1].w the gloss) and the directional light is not in the shader at all. Matching
	// light k to cube k there gave each lamp the NEXT lamp's shadow and the first lamp none: forward shadows stopped dead
	// where an OPT floor piece met its neighbour (the TOPS rope shadows, whole pieces lit or dark against the next one).
	inline int FirstShaderLight(const RenderPassView* pass) { return pass->passEnum == 0xCA || pass->passEnum == 0xD1 ? 1 : 0; }

	struct Slot { const ShadowSceneLight* light; D3DXVECTOR3 position; float radius; IDirect3DCubeTexture9* cube; int slot; float fade;
		float tile[3]; };	// UNOFFICIAL 2026-10-30 ShadowAtlas: u, v of the tile's first texel centre, its span (0: none), set by ShadowAtlas::Update

	inline bool CompiledIn = false;			// an Interior record was compiled with INTERIOR_SHADOWS
	inline int CompiledCount = 0;
	// UNOFFICIAL 2026-10-20 (P82 tester: shiny objects indoors lost their lamp shadows as a third lamp came near): the GPU refused
	// the six-lamp shiny-object shaders' Interior records (HandBlood on: 3,820-4,016 instruction slots, the driver takes about
	// 3,600), so those objects drew without lamp shadows while the others had them. ShaderManager::LoadShader now steps down
	// through shorter versions (PointShadowForward.hlsl POINT_SHADOW_FIT): FitLevel is the highest step any record needed, and
	// Publish holds every object to what that one can do, so all objects match. Refused: some lit shader got no Interior record
	// at all: then no object uses the forward shadows (the screen-space ones for all, as with Enabled off).
	inline int FitLevel = 0;
	inline bool Refused = false;
	inline bool Active = false;				// this frame: indoors, Strength > 0, at least one cube map to use
	inline bool FirstPerson = true;			// [Shaders.ShadowsInteriors.Forward] FirstPerson, read per frame
	inline Slot Slots[ShadowCubeMapsMax + ShadowPoolMax] = {};	// the live slots (Publish), then the pool's static cube maps (PublishPool)
	// UNOFFICIAL 2026-10-30 RT lamp shadows ExteriorForward: the outdoor lamp-shadow record of each lit pixel shader (ShaderManager::
	// LoadShader), chosen outdoors while ExteriorActive (NiD3DPixelShaderEx::SetupShader)
	inline std::unordered_map<const void*, ShaderRecordPixel*> ExteriorRecords;
	inline bool ExteriorActive = false;		// this frame: outdoors, ExteriorForward lamps published and the trace live
	inline std::unordered_set<const void*> TerrainRecords;	// the land's records among ExteriorRecords (TERRAIN_LAMP_RT: their own constants)
	inline int SlotCount = 0;
	// UNOFFICIAL 2026-11-01 FadeOut ([Shaders.ShadowsInteriors.Forward] FadeOut, seconds, live, 0 = off; docs/next/FLICKER-PROGRAM.md part 3).
	// ShaderManager::GetNearbyLights keeps a lamp that dropped out of the ranked shadow casters in its live slot while its shadow fades out,
	// and writes here, per live slot (index = cube slot), the lamp and how much of its shadow is left (1 = full; it climbs back at the same
	// rate if the lamp ranks again before it is gone). Publish multiplies the slot's fade by it, so the forward lookups, the deferred lamps'
	// tile rows (ManyLamps::PutTiles) and the RT lamp shadows (Submit) all fade with it. FadeOutPooled: PublishPool found the fading lamp in
	// the pool too (its still map takes over, not faded): the next GetNearbyLights lets the slot go at once.
	inline const ShadowSceneLight* FadeOutLamp[ShadowCubeMapsMax] = {};
	inline float FadeOutLeft[ShadowCubeMapsMax] = {};
	inline bool FadeOutPooled[ShadowCubeMapsMax] = {};
	inline float FadeOutOf(int slot, const ShadowSceneLight* light) {
		return slot >= 0 && slot < ShadowCubeMapsMax && FadeOutLamp[slot] == light ? FadeOutLeft[slot] : 1.0f;
	}
	// UNOFFICIAL 2026-10-29 ShadowPool: with pool lamps published, SetForPass finds a lamp's entry through this map (hundreds of entries);
	// without (UseIndex false), by the scan over the live slots as before.
	inline std::unordered_map<const ShadowSceneLight*, int> SlotIndex;
	inline bool UseIndex = false;
	inline int Focus = -1;					// DebugView 3: the Slots entry of the lamp in cube slot 0 (red in views 1 and 2), -1 none
	inline unsigned TraceDraws = 0, TraceDrawsWithLamp = 0;	// DebugView 3: draws that got the focus lamp, and of those the ones lit by it
	inline IDirect3DSurface9* TraceReadback = nullptr;		// DebugView 3: system-memory copy of one cube face (TraceCube)
	inline UINT TraceReadbackSize = 0;

	// DebugView 3 diagnostic: what one cube map holds, read back on the CPU (stalls the frame; every 2 seconds at most). Per
	// face (named by the world direction it looks): share of texels nothing was drawn into (1), share at 0, the nearest stored
	// distance and the mean of the drawn ones, and the centre texel (straight down: the floor's distance under the lamp).
	inline void TraceCube(const char* what, IDirect3DCubeTexture9* cube) {
		IDirect3DDevice9* device = TheRenderManager->device;
		D3DSURFACE_DESC desc;
		if (!cube || FAILED(cube->GetLevelDesc(0, &desc))) { Logger::Log("TRACE   %s: no cube map", what); return; }
		if (desc.Format != D3DFMT_R32F) { Logger::Log("TRACE   %s: format %d, not read", what, (int)desc.Format); return; }
		if (!TraceReadback || TraceReadbackSize != desc.Width) {
			if (TraceReadback) TraceReadback->Release();
			TraceReadback = nullptr;
			if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &TraceReadback, NULL))) {
				TraceReadback = nullptr;
				Logger::Log("TRACE   %s: no readback surface", what);
				return;
			}
			TraceReadbackSize = desc.Width;
		}
		static const char* faceName[6] = { "+X", "-X", "+Y", "-Y", "down", "up" };	// RenderShadowCubeMap FaceDirection, lookup (x, y, -z)
		char text[900] = "";
		size_t used = 0;
		for (int face = 0; face < 6; face++) {
			IDirect3DSurface9* surface = nullptr;
			D3DLOCKED_RECT lock;
			if (FAILED(cube->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &surface)) || !surface) continue;
			const HRESULT copied = device->GetRenderTargetData(surface, TraceReadback);
			surface->Release();
			if (FAILED(copied) || FAILED(TraceReadback->LockRect(&lock, NULL, D3DLOCK_READONLY))) {
				used += sprintf_s(text + used, sizeof(text) - used, " | %s: not read", faceName[face]);
				continue;
			}
			unsigned empty = 0, zero = 0, drawn = 0;
			double sum = 0.0;
			float nearest = 1.0f;
			for (UINT y = 0; y < desc.Height; y++) {
				const float* row = (const float*)((const char*)lock.pBits + y * lock.Pitch);
				for (UINT x = 0; x < desc.Width; x++) {
					const float v = row[x];
					if (!(v < 0.999f)) empty++;
					else if (!(v > 0.0f)) zero++;
					else { drawn++; sum += v; if (v < nearest) nearest = v; }
				}
			}
			const float centre = ((const float*)((const char*)lock.pBits + (desc.Height / 2) * lock.Pitch))[desc.Width / 2];
			TraceReadback->UnlockRect();
			const float all = (float)(desc.Width * desc.Height) * 0.01f;
			used += sprintf_s(text + used, sizeof(text) - used, " | %s: %.0f%% empty, %.0f%% zero, nearest %.3f, mean %.3f, centre %.3f",
				faceName[face], empty / all, zero / all, nearest, drawn ? sum / drawn : 0.0, centre);
		}
		Logger::Log("TRACE   %s %p (%ux%u)%s", what, (const void*)cube, desc.Width, desc.Height, text);
	}
	inline float Params[4] = {};				// c180: strength, bias, normal offset, PCF spread
	inline float DebugView = 0.0f;			// c181.x
	inline float NearFade = 0.0f;			// c181.y: [Shaders.ShadowsInteriors.Main] NearFade (PointShadowForward.hlsl PointShadowNear)
	inline float InvisibleSize = 0.0f;		// c181.z outside DebugView 3: InvisibleLightSize / sqrt 2 (0 off)
	inline float Quality = 1.0f;			// c181.w: [Shaders.ShadowsInteriors.Forward] Quality (Low arrives as spread 0 in c180.w)
	inline float Soft[4] = {};				// c189: contact hardening strength (0 off), narrowest spread share; LampSize / sqrt 2, widest spread
	inline float Magnified[4] = {};			// c188 outside DebugView 3 (UNOFFICIAL 2026-10-21 MagnifiedSoftness, POINT_SHADOW_MAG): x y the ramp
											// of how many pixels a cube texel covers, z the least spread then (per unit major; 0 = off);
											// w the face resolution (TexelFilter, POINT_SHADOW_TEXEL)
	inline IDirect3DCubeTexture9* Neutral = nullptr;	// 1x1 cube holding 1 ("nothing drawn") for unmatched stages
	inline const DWORD Stage[6] = { 8, 11, 12, 13, 14, 15 };	// NVR_PointShadowCube0-5
	// UNOFFICIAL 2026-10-30 ShadowAtlas (core/ShadowAtlas.h; PointShadowForward.hlsl POINT_SHADOW_ATLAS): the records read the atlas on s8,
	// each light's tile in its tint register. Set by ShadowAtlas (null: the cube maps as before).
	inline IDirect3DTexture9* AtlasTexture = nullptr;
	// UNOFFICIAL 2026-10-31 (P85: the atlas on by default for DXVK players): [Shaders.ShadowsInteriors.Atlas] Enabled counts under DXVK only.
	// On native D3D9 the GTX 1070 refused the six-lamp shiny Interior records with the atlas and they loaded at FIT 1 (Vault 21 world 16.0 ms
	// against 15.5 without; DEFERRED-RESULTS.md round 1), so native players keep the cube maps. Every reader of the switch asks here.
	inline int AtlasWantedCache = -1;
	inline bool AtlasWanted() {
		if (AtlasWantedCache < 0) {
			if (!TheRenderManager) return false;
			const bool on = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "Enabled") != 0;
			const bool dxvk = !strncmp(TheRenderManager->D3D9RuntimeDescription(), "DXVK", 4);
			AtlasWantedCache = on && dxvk ? 1 : 0;
			if (on && !dxvk) Logger::Log("UNOFFICIAL ShadowAtlas: on in the settings, but it runs under DXVK only: native D3D9 keeps the lamp cube maps (and the deferred lamps, which need it, stay off)");
		}
		return AtlasWantedCache != 0;
	}
	inline unsigned StatFrames = 0, StatDraws = 0, StatMatchedDraws = 0, StatMatchedLights = 0;

	// B42 Optics (PIP scopes, through JIP's ProjectExtraCamera): the scope's picture is the world drawn from another
	// camera into a small square texture (Data\config\B42Optics\Settings.ini RESOLUTION, 512), shown on lens shapes
	// (PRJ, SHD, VGN, ...) under the "B42LensRoot"/"B42Lens*" nodes of its lens meshes. The shaders rebuild positions
	// with the main camera's matrices (GetShadowWorldPos), so lamp shadows land wrong in both: with ScopeFix neither
	// gets them. The log says when either case was seen.
	inline bool ScopeFix = true;			// [Shaders.ShadowsInteriors.Forward] ScopeFix, read per frame (Publish)
	inline bool ScopeInView = false, ScopeWasInView = false;	// a B42 lens shape was drawn this frame / last frame
	inline unsigned OtherCameraLogs = 0, ScopeLogs = 0;

	inline bool IsLens(const RenderPassView* pass) {
		if (!pass || !pass->geometry) return false;
		const NiAVObject* node = pass->geometry;
		for (int depth = 0; node && depth < 8; depth++, node = node->m_parent)
			if (node->m_pcName && !strncmp(node->m_pcName, "B42Lens", 7)) return true;
		return false;
	}

	inline bool OtherCamera(const RenderPassView* pass, bool firstPerson) {
		if (!ScopeFix) return false;
		const D3DVIEWPORT9& port = TheRenderManager->m_kD3DPort;
		if (!TAAEffect::IsWorldViewport(port)) {	// UNOFFICIAL 2026-10-27: RenderScale's world viewport (the render size) is the world's too
			if (OtherCameraLogs < 3) {
				OtherCameraLogs++;
				Logger::Log("UNOFFICIAL forward shadows: a %ux%u render from another camera (a B42 Optics scope picture?) gets no lamp shadows.",
					(unsigned)port.Width, (unsigned)port.Height);
			}
			return true;
		}
		if (firstPerson && pass && pass->geometry) {
			const NiAVObject* node = pass->geometry;
			for (int depth = 0; node && depth < 8; depth++, node = node->m_parent)
				if (node->m_pcName && !strncmp(node->m_pcName, "B42Lens", 7)) { ScopeInView = true; return true; }
		}
		return false;
	}

	// The pixel shaders that read c174-c187 when compiled with INTERIOR_SHADOWS (the files PointShadowForward.hlsl is
	// included by).
	inline bool Eligible(const char* name, const ShaderTemplate& t) {
		if (!name || !strstr(name, ".pso")) return false;
		// UNOFFICIAL the skin shader (SkinTemplate.hlsl): every pixel shader with lamps (SKIN2000/2001 have only the sun).
		if (t.Name && !strcmp(t.Name, "SkinTemplate")) return strcmp(name, "SKIN2000.pso") && strcmp(name, "SKIN2001.pso");
		if (t.Name) return !strcmp(t.Name, "ObjectTemplate") || !strcmp(t.Name, "SkinVPSTemplate");
		return !strcmp(name, "SKIN2002.pso") || !strcmp(name, "SKIN2006.pso") || !strcmp(name, "SM3002.pso") ||
			!strcmp(name, "SM3003.pso") || !strcmp(name, "SM3005.pso") || !strcmp(name, "SM3007.pso");
	}

	inline bool BoundThisFrame = false;		// a forward-shadow shader was bound this frame (the PBR shaders are drawing)

	// UNOFFICIAL (2026-10-08): how many lamps get a cube map this frame. Indoors with forward shadows compiled in and on,
	// [Shaders.ShadowsInteriors.Main] LightPoints up to ShadowCubeMapsMax (each object binds only its own lamps); otherwise
	// the screen-space passes read them: LightPoints up to ShadowCubeMapsScreen (11 with a shadow, the 12th lit without).
	inline bool UsesAllSlots(bool exterior) {
		// UNOFFICIAL 2026-10-30 ExteriorForward: outdoors too while the traced lamp shadows run (no cube maps there: LightPoints lamps,
		// not the screen pass's 12; rtlamp-30: Novac's walkway lamps were not among the 12)
		if (exterior) return CompiledIn && !Refused && !ExteriorRecords.empty() && RtLampShadows::ExteriorForwardOn() && RtCore::Running() &&
			TheSettingManager->GetSettingI(RtLampShadows::kSection, "Live") != 0;
		return CompiledIn && !Refused && TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward.Enabled;
	}
	// UNOFFICIAL ExteriorLightPoints (2026-10-13): outdoors above 12, every slot up to it has a shadow, the screen-space pass
	// drawn in batches of 11 (ShaderManager::RenderPointShadowBatches).
	// (Only while lamp shadows are on outdoors, UsePointShadowsDay/Night: TESR_ShadowFade.z; otherwise nothing is gained.)
	inline bool ExteriorBatched(bool exterior) {
		return exterior && TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.ExteriorLightPoints > ShadowCubeMapsScreen &&
			TheShaderManager->Effects.ShadowsExteriors->Constants.ShadowFade.z > 0.0f;
	}
	inline int ShadowedSlots(bool exterior) {
		if (ExteriorBatched(exterior)) return (std::min)(TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.ExteriorLightPoints, (int)ExteriorBatchedMax);
		const int points = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.LightPoints;
		return UsesAllSlots(exterior) ? (std::min)(points, (int)ShadowCubeMapsMax) : (std::min)(points, (int)ShadowCubeMapsSampled);
	}
	inline unsigned Frame = 0;				// counts frames (Begin), for ScopeFix outdoors (Render.cpp)

	// UNOFFICIAL ObjectLamps diagnostic (2026-10-08, with [Shaders.ShadowsInteriors.Forward] LogLamps). Shadows popped on and
	// off parts of objects as the camera turned, ending on mesh seams, while every lamp kept its cube map (LAMP lines). So for
	// each object drawn in the world pass this notes, frame by frame, which point lamps the game drew it with and whether each
	// came in a pass with forward shadows (an NVR shader compiled with them, forward shadows active) or without; when that
	// changes between two frames in a row, an OBJECT line says how, with the passes' shaders and how far the camera turned
	// and moved and the player moved in between. Read only: nothing drawn changes.
	struct ObjectLampState {
		UInt32 frame = 0, lastFrame = 0;
		UInt8 count = 0, lastCount = 0;
		const ShadowSceneLight* lamps[12] = {}; UInt8 how[12] = {}; const char* shader[12] = {};	// how: 1 with shadows, 2 without
		const ShadowSceneLight* last[12] = {}; UInt8 lastHow[12] = {}; const char* lastShader[12] = {};
	};
	inline std::unordered_map<const void*, ObjectLampState> ObjectLamps;
	inline bool LogObjectLamps = false;			// LogLamps, read per frame (Publish)
	inline const char* LastPixelShaderName = nullptr;	// the pixel shader of the run being drawn (SetForDraw)
	inline bool LastPixelShaderShadowed = false;
	inline const char* LastVertexShaderName = nullptr;	// AimProbe: the run's vertex shader, and whether each is NVR's (SetShadersHook)
	inline bool LastVertexShaderNvr = false, LastPixelShaderNvr = false;

	// UNOFFICIAL AimProbe (LogLamps diagnostic, 2026-10-08). In the TOPS casino single floor pieces went dark under one lamp's
	// forward shadow, and shadows stopped dead where one object met the next. To see what differs between such objects, every
	// object drawn in the world pass whose bound the line of sight (camera position, camera forward) passes through is noted
	// with each of its passes: vertex and pixel shader, whether each is NVR's or the game's, whether the pass has forward
	// shadows, and its point lamps (with their cube map slots). When the set under the crosshair changes, at most twice a
	// second, AIM lines list them, the ones whose centre is nearest the line of sight first.
	// (2026-10-08: every light of the pass with its index k, the pass type and light count, and for the passes with forward
	// shadows whether c174+k held a cube map for light k when it was drawn: the TOPS rope shadow stopped where the next object began.)
	struct AimPass { const char* vs; const char* ps; bool vsNvr, psNvr, shadowed; UInt16 passEnum; UInt8 numLights; bool strength;
		UInt8 count; UInt8 index[6]; bool point[6]; bool uploaded[6]; const ShadowSceneLight* lamps[6]; };
	struct AimHit { const void* geometry; float along, off, radius; UInt8 passes; AimPass pass[4]; };
	inline AimHit AimHits[10];
	inline int AimHitCount = 0;
	inline UInt32 AimFrame = 0, AimLastLogFrame = 0;
	inline const void* AimLogged[10] = {};
	inline int AimLoggedCount = 0;
	inline unsigned AimLines = 0;
	struct CameraState { UInt32 frame; D3DXVECTOR3 forward, position, player; };
	inline CameraState Cameras[8] = {};
	inline unsigned ObjectLampLines = 0;
	inline unsigned StatLampAppeared = 0, StatLampGone = 0, StatLampLostShadow = 0, StatLampGotShadow = 0, StatLampStill = 0;

	// UNOFFICIAL KeepLampsInView (2026-10-08, [Shaders.ShadowsInteriors.Forward] KeepLampsInView, indoors with forward shadows
	// on). The game lights an object only with its ACTIVE lamps (BSShaderLightingProperty::GetFirstActiveLight 0x00B70590,
	// GetNextActiveLight, GetNumberOfActiveLights, ...): those whose ShadowSceneLight +0x110 is not 0xFF, the mask
	// ShadowSceneLight::TestFrustumCull (0x00B9E970, called once, from ShadowSceneNode::PreOnVisible at 0x00B5E915) writes
	// when it finds the lamp's whole sphere outside the view (otherwise the bits of the cube faces out of view). Turning the
	// camera, objects on screen lost lamps the game had just called out of view, and with them the lamp's light and its forward
	// shadow (ObjectLamps log, 2026-10-08: 306 of 310 lamps that left an object did so as their mask became FF; 282 of 284
	// came back as it cleared). The screen-space shadows never showed it: they darken the finished image, whatever lamps the
	// objects had. Here the game's test runs as before, with its own previous result put back first so its fade and update
	// bookkeeping sees no change that was ours; a lamp it calls out of view whose sphere does reach the render camera's view
	// (LampReachesView, with a margin for a frame of turning) stays active for the frame.
	inline bool KeepLampsWanted = false;		// KeepLampsInView with forward shadows on (Publish)
	inline bool KeepLampsNow = false;			// ... and indoors this frame (Begin)
	inline std::unordered_map<const ShadowSceneLight*, UInt16> GameCullMask;	// the game's own last TestFrustumCull result
	inline unsigned StatCullCalls = 0, StatCullOut = 0, StatCullKept = 0;
	inline const UInt32 kTestFrustumCallSite = 0x00B5E915, kTestFrustumCull = 0x00B9E970;
	typedef void(__thiscall* TestFrustumCullFn)(ShadowSceneLight* light, void* cullingProcess);

	// The lamp's sphere (its reach, Spec.r, as the game tests it) against the render camera's view: the four sides and the
	// plane through the eye (as ShaderManager::GetNearbyLights' view test). Widened by 5% + 32 units + a tenth of its distance
	// to the camera: a frame of turning, and rounding, never drops a lamp whose light is on screen.
	inline bool LampReachesView(const ShadowSceneLight* light) {
		const NiPointLight* source = light ? light->sourceLight : nullptr;
		if (!source) return false;
		const D3DXMATRIX& camera = TheRenderManager->InvViewMatrix;
		const D3DXVECTOR3 right(camera._11, camera._12, camera._13), up(camera._21, camera._22, camera._23), forward(camera._31, camera._32, camera._33);
		const float tanX = fabsf(TheRenderManager->InvProjMatrix._11), tanY = fabsf(TheRenderManager->InvProjMatrix._22);
		const float normX = 1.0f / sqrtf(1.0f + tanX * tanX), normY = 1.0f / sqrtf(1.0f + tanY * tanY);
		const D3DXVECTOR3 v(source->m_worldTransform.pos.x - TheRenderManager->CameraPosition.x, source->m_worldTransform.pos.y - TheRenderManager->CameraPosition.y,
			source->m_worldTransform.pos.z - TheRenderManager->CameraPosition.z);
		const float reach = source->Spec.r * 1.05f + 32.0f + 0.1f * D3DXVec3Length(&v);
		const float x = D3DXVec3Dot(&v, &right), y = D3DXVec3Dot(&v, &up), z = D3DXVec3Dot(&v, &forward);
		if (z < -reach) return false;
		if ((tanX * z - x) * normX < -reach || (tanX * z + x) * normX < -reach) return false;
		if ((tanY * z - y) * normY < -reach || (tanY * z + y) * normY < -reach) return false;
		return true;
	}

	// Replaces the call at kTestFrustumCallSite (thiscall: ecx = the lamp, one argument, the culling process; ret 4).
	inline void __fastcall TestFrustumCullHook(ShadowSceneLight* light, void*, void* cullingProcess) {
		auto previous = GameCullMask.find(light);
		if (previous != GameCullMask.end()) light->bIsEnabled = previous->second;
		((TestFrustumCullFn)kTestFrustumCull)(light, cullingProcess);
		GameCullMask[light] = light->bIsEnabled;
		StatCullCalls++;
		if (light->bIsEnabled != 0xFF) return;
		StatCullOut++;
		if (KeepLampsNow && light->bPointLight && LampReachesView(light)) {
			light->bIsEnabled = 0;	// active, every face in view
			StatCullKept++;
		}
	}

	inline void Begin() {
		Frame++;
		Cameras[Frame & 7] = { Frame, D3DXVECTOR3(TheRenderManager->CameraForward.x, TheRenderManager->CameraForward.y, TheRenderManager->CameraForward.z),
			D3DXVECTOR3(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z),
			Player ? D3DXVECTOR3(Player->pos.x, Player->pos.y, Player->pos.z) : D3DXVECTOR3(0.0f, 0.0f, 0.0f) };
		if ((Frame & 1023) == 0)
			for (auto it = ObjectLamps.begin(); it != ObjectLamps.end();) it = Frame - it->second.frame > 1024 ? ObjectLamps.erase(it) : std::next(it);
		if ((Frame & 4095) == 0) GameCullMask.clear();	// freed lamps (a lamp new at a freed address gets one stale restore, harmless)
		KeepLampsNow = KeepLampsWanted && !TheShaderManager->GameState.isExterior;
		Active = false; SlotCount = 0; BoundThisFrame = false; UseIndex = false; ExteriorActive = false;
		ScopeFix = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward.ScopeFix;
	}

	// After the cube-map loop, indoors. A slot counts only if its cube map is that light's: slots[i] holds what the cube
	// was last drawn for (PointShadowSchedule.h redraws at once on a new light, position, radius or cell).
	inline void Publish(const PointShadowSlotState* slots, ShadowSceneLight* const* lights, int count, IDirect3DCubeTexture9* const* cubes) {
		// FadeIn: when each published lamp got its shadow (kept while it stays published frame after frame).
		static const ShadowSceneLight* lastLights[ShadowCubeMapsMax] = {};
		static double lastStart[ShadowCubeMapsMax] = {};
		static int lastCount = 0;
		Active = false;
		SlotCount = 0;
		UseIndex = false;
		Focus = -1;
		RtLampShadows::Submit(nullptr, 0);	// UNOFFICIAL 2026-10-28 RT lamp shadows: none this frame until Submit below
		if (!CompiledIn) { lastCount = 0; return; }
		const ShadowsExteriorEffect::InteriorsStruct::ForwardStruct& forward = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward;
		LogObjectLamps = forward.LogLamps;
		KeepLampsWanted = forward.Enabled && forward.KeepLampsInView;
		const float strength = forward.Strength;
		// UNOFFICIAL (2026-10-08): Enabled switched off in the menu stops the forward shadows at once (the shaders stay
		// compiled in until a restart; idle = every draw gets c180.x 0 and the old screen-space pass runs). It used to be
		// read only at startup, so turning it off in play only changed the lamp choice (no view test, no margin).
		if (!forward.Enabled || !(strength > 0.0f) || Refused) { lastCount = 0; return; }
		LARGE_INTEGER frequency, counter;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		const double now = (double)counter.QuadPart / (double)frequency.QuadPart;
		const float fadeIn = forward.FadeIn;
		const bool hold = ShadowHold::Seconds() > 0.0f;	// UNOFFICIAL 2026-10-30 ShadowHold
		double start[ShadowCubeMapsMax] = {};
		// (UNOFFICIAL 2026-11-01 NearCubeSize: the near slots' size, so MagnifiedSoftness does not blur their sharper maps back to the base size)
		const float face = (float)(std::max)((std::max)(TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.ShadowCubeMapSize,
			(int)TheShaderManager->Effects.ShadowsExteriors->NearCubeSize), 1);
		Params[0] = (std::min)(strength, 1.0f);
		Params[1] = forward.Bias;
		Params[2] = forward.NormalOffset * 2.0f / face;
		// UNOFFICIAL 2026-10-24 AutoNormalOffset (owner: indoors, with PBR, every triangle of a character showed as a flat facet with a hard
		// edge, skin and clothes; nothing else changed it). The lookup compares texels up to the filter's reach away from the point (Softness;
		// at least MagnifiedSoftness where the map is magnified, i.e. close to the camera: on characters; plus a texel for SmoothEdges' and
		// TexelFilter's bilinear weights). On a surface tilted by t from the lamp those texels hold the surface's own depth up to reach x
		// texel x tan t nearer the lamp, and the fixed Bias (1.8 % of the distance) covers that only up to ~57 degrees at the defaults: past
		// it the surface shadows itself, by an amount set by its tilt. A low-poly body is flat per triangle, so each triangle came out
		// evenly lit or dimmed: facets. Pushing the lookup off the surface along its normal by the reach covers every tilt (the margin grows
		// as 1 / cos t, as the error does), the usual cure; the shaders already apply NormalOffset (x the distance: texels at any distance).
		if (forward.AutoNormalOffset) {
			float reach = forward.Quality > 0 ? forward.Softness * 1.5f : 0.0f;	// the four taps sit on the diagonals, sqrt 2 x the spread out
			if (forward.Quality > 0 && forward.Softness > 0.0f) reach = (std::max)(reach, forward.MagnifiedSoftness * 1.5f);	// as Magnified[2] below
			reach += 1.0f;	// bilinear / smooth-edge weights reach one texel further
			Params[2] = (std::max)(forward.NormalOffset, reach) * 2.0f / face;
		}
		Params[3] = forward.Quality > 0 ? forward.Softness * 2.0f / face : 0.0f;	// Quality 0 (Low): one tap
		Quality = (float)forward.Quality;
		Soft[0] = forward.ContactHardening && forward.Quality > 0 ? forward.ContactHardeningStrength : 0.0f;
		Soft[1] = forward.ContactHardeningMinSoftness;
		// UNOFFICIAL LampSize (2026-10-20): shadows soft by distance, as a lamp of that radius casts them (PointShadowForward.hlsl
		// PointShadowCube): z = LampSize / sqrt 2 (the four diagonal taps sit sqrt 2 x the spread from the centre), w = the
		// widest spread per unit of the cube direction's major axis (MaxSoftness texels). It uses contact hardening's blocker
		// search, so it switches that on (x 1) when ContactHardening itself is off. Softness stays the narrowest spread.
		// The shader takes both in one form (x = 0 and y = 1 here: the Softness spread is the narrowest, nothing scales by
		// the ratio), so contact hardening's own settings do nothing while LampSize is on.
		// UNOFFICIAL InvisibleLightSize (2026-10-20): the game's invisible lights (LampFixtures.h) get their own lamp size, c181.z; the
		// shader picks it for lights whose fade is negative (below). Either size switches the blocker search on (w > 0); a lamp size
		// of 0 then keeps that kind of light at the Softness spread.
		Soft[2] = Soft[3] = 0.0f;
		InvisibleSize = 0.0f;
		if ((forward.LampSize > 0.0f || forward.InvisibleLightSize > 0.0f) && forward.Quality > 0 && forward.Softness > 0.0f) {
			Soft[0] = 0.0f;
			Soft[1] = 1.0f;
			Soft[2] = forward.LampSize * 0.70710678f;
			Soft[3] = (std::max)(forward.MaxSoftness, forward.Softness) * 2.0f / face;
			InvisibleSize = forward.InvisibleLightSize * 0.70710678f;
		}
		// UNOFFICIAL 2026-10-21 MagnifiedSoftness (PointShadowForward.hlsl POINT_SHADOW_MAG, compiled in when above 0 at startup; shaders
		// without it ignore c188 outside DebugView 3): a cube texel at distance L from its
		// lamp is about L x 2 / resolution wide, a screen pixel at distance C from the camera about C x (frustum width / screen width)
		// (InvProjMatrix._11 = half the frustum width at unit distance). The shader gets the ramp of texel pixels = L / C x x over
		// MagnifiedPixels0..1 as saturate(L / C x x + y), and the spread MagnifiedSoftness texels gives.
		{
			const float pixel = 2.0f * fabsf(TheRenderManager->InvProjMatrix._11) / (float)(std::max)(TheRenderManager->width, (UInt32)1);
			const float texelPixels = pixel > 0.0f ? (2.0f / face) / pixel : 0.0f;	// pixels per texel when L = C
			static const float MagnifiedPixels0 = 2.0f, MagnifiedPixels1 = 8.0f;	// no widening at 2 px per texel or less, all from 8
			Magnified[0] = texelPixels / (MagnifiedPixels1 - MagnifiedPixels0);
			Magnified[1] = -MagnifiedPixels0 / (MagnifiedPixels1 - MagnifiedPixels0);
			Magnified[2] = forward.Quality > 0 && forward.Softness > 0.0f && texelPixels > 0.0f ? forward.MagnifiedSoftness * 2.0f / face : 0.0f;
			Magnified[3] = face;	// TexelFilter (POINT_SHADOW_TEXEL): the face resolution, for its texel grid
		}
		// UNOFFICIAL 2026-11-01 TruePCF ([Shaders.ShadowsInteriors.Atlas] TruePCF, live; missing key = on; PointShadowForward.hlsl
		// PointShadowAtlasBlocked): with the atlas, c188.w (TexelFilter's, which the atlas shaders never compile) is the atlas's size in
		// texels, 0 = the old linear-filtered taps.
		if (AtlasTexture) {
			SettingManager::Configuration::ConfigNode pcfNode;
			const bool pcf = !TheSettingManager->Config.FillNode(&pcfNode, "Shaders.ShadowsInteriors.Atlas", "TruePCF") || pcfNode.BoolValue;
			D3DSURFACE_DESC atlasDesc = {};
			Magnified[3] = pcf && SUCCEEDED(AtlasTexture->GetLevelDesc(0, &atlasDesc)) ? (float)atlasDesc.Width : 0.0f;
		}
		// UNOFFICIAL 2026-10-20 FitLevel (above): what the shortest record loaded can do, for every object. 2: no High/Ultra (Medium
		// at most); 3: also no blocker search (contact hardening and LampSize off: x and w 0 skip it in every shader).
		if (FitLevel >= 2 && Quality > 1.0f) Quality = 1.0f;
		if (FitLevel >= 3) { Soft[0] = Soft[3] = 0.0f; InvisibleSize = 0.0f; }
		static int cappedLogged = -1;
		const int capped = (FitLevel >= 2 && forward.Quality > 1 ? 1 : 0) | (FitLevel >= 3 && (forward.ContactHardening || forward.LampSize > 0.0f ||
			forward.InvisibleLightSize > 0.0f) ? 2 : 0);
		if (capped && capped != cappedLogged) {
			cappedLogged = capped;
			Logger::Log("UNOFFICIAL lamp shadows: this GPU took only shorter versions of some lit shaders (see the startup log), so every object's lamp shadows get%s%s%s.",
				capped & 1 ? " Quality Medium (the setting asks for more)" : "", capped == 3 ? " and" : "",
				capped & 2 ? " no contact hardening or LampSize" : "");
		}
		DebugView = (float)forward.DebugView;
		NearFade = TheShaderManager->Effects.ShadowsExteriors->Constants.PointShadowNear.x;	// [Shaders.ShadowsInteriors.Main] NearFade, already clamped
		ScopeFix = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward.ScopeFix;
		if (ScopeInView != ScopeWasInView && ScopeLogs < 20) {
			ScopeLogs++;
			Logger::Log("UNOFFICIAL forward shadows: B42 Optics lens %s (no lamp shadows on it).", ScopeInView ? "in view" : "gone");
		}
		ScopeWasInView = ScopeInView;
		ScopeInView = false;
		FirstPerson = forward.FirstPerson;
		static UInt64 rejectedLast = 0; static unsigned rejectLogs = 0;	// UInt64: up to 64 slots
		UInt64 rejected = 0;
		for (int i = 0; i < count && i < ShadowCubeMapsMax; i++) {
			if (!lights[i] || !cubes[i] || !slots[i].valid || slots[i].light != lights[i] || slots[i].texture != cubes[i] ||
				!(slots[i].radius > 0.0f)) {
				// LogLamps: a lamp with a slot whose cube map is not its own this frame gets no shadow.
				if (lights[i]) {
					rejected |= 1ull << i;
					if (!(rejectedLast & (1ull << i)) && rejectLogs < 200 && forward.LogLamps) {
						rejectLogs++;
						Logger::Log("LAMP %p in slot %d gets no shadow: its cube map is not drawn for it (drawn %d, same lamp %d, same texture %d, radius %.0f)",
							lights[i], i, (int)slots[i].valid, (int)(slots[i].light == lights[i]), (int)(slots[i].texture == cubes[i]), slots[i].radius);
					}
				}
				continue;
			}
			// The radius the cube map stores distances over (256 for carried lights), as RenderShadowCubeMap drew it.
			double since = now;
			for (int j = 0; j < lastCount; j++) if (lastLights[j] == lights[i]) { since = lastStart[j]; break; }
			if (hold) since = ShadowHold::Since(lights[i], now);	// UNOFFICIAL 2026-10-30 ShadowHold: from when it got a shadow, live or pool
			float fade = fadeIn > 0.0f ? (float)(std::min)(1.0, (now - since) / fadeIn) : 1.0f;
			// UNOFFICIAL FillLightShadowStrength (2026-10-08): a fill light (reach past FillLightRadius) stands for broad room
			// light; its shadow is that share of a full one (the light it takes away), so a staircase or a table under it shades
			// the floor instead of blacking it out. Rides in the per-lamp fade the shader multiplies the shadow by.
			const NiPointLight* source = lights[i]->sourceLight;
			if (forward.FillLightRadius > 0.0f && source && source->Spec.r > forward.FillLightRadius) fade *= forward.FillLightShadowStrength;
			// UNOFFICIAL InvisibleLightShadowStrength (2026-10-20): a light with no glowing lamp near it (ShaderManager::GetNearbyLights
			// gives it no cube map at 0; LampFixtures.h has the answer cached for this frame).
			// InvisibleLightSize: its fade goes negative (the same amount), the shader's sign for 'use c181.z as the lamp size'.
			if ((forward.InvisibleLightShadowStrength < 1.0f || InvisibleSize > 0.0f) && source && !TheShaderManager->GameState.isExterior &&
				LampFixtures::Invisible(source, forward.FixtureDistance)) {
				fade *= forward.InvisibleLightShadowStrength;
				if (InvisibleSize > 0.0f) fade = -fade;
			}
			fade *= FadeOutOf(i, lights[i]);	// UNOFFICIAL 2026-11-01 FadeOut: a lamp that lost its rank, its shadow fading out in the slot it kept
			start[SlotCount] = since;
			Slots[SlotCount++] = { lights[i], D3DXVECTOR3(slots[i].x, slots[i].y, slots[i].z), slots[i].radius, cubes[i], i, fade };
		}
		rejectedLast = rejected;
		for (int s = 0; s < SlotCount; s++) { lastLights[s] = Slots[s].light; lastStart[s] = start[s]; }
		// UNOFFICIAL LogLamps (2026-10-20): the published lamps by cube slot whenever the set changes, with DebugView 1/2's colour for each
		// (slot 0 red, the others by slot), so a shadow on screen can be traced to its lamp.
		if (forward.LogLamps) {
			static UInt64 lastSet = 0; static unsigned setLogs = 0;
			UInt64 set = 1469598103934665603ull;
			for (int s = 0; s < SlotCount; s++) set = (set ^ (UInt64)(uintptr_t)Slots[s].light ^ ((UInt64)Slots[s].slot << 48)) * 1099511628211ull;
			if (set != lastSet && setLogs < 40) {
				lastSet = set; setLogs++;
				static const char* const colour[11] = { "red", "green", "blue", "yellow", "cyan", "magenta", "orange", "purple", "lime", "pink", "sky" };
				for (int s = 0; s < SlotCount; s++) {
					const NiPointLight* source = Slots[s].light->sourceLight;
					Logger::Log("LAMPSLOT %d (%s) lamp %p cube at %.0f %.0f %.0f radius %.0f fade %.2f | light at %.0f %.0f %.0f, reach %.0f, colour %.2f %.2f %.2f x %.2f",
						Slots[s].slot, colour[Slots[s].slot == 0 ? 0 : 1 + (Slots[s].slot - 1) % 10], (const void*)Slots[s].light,
						Slots[s].position.x, Slots[s].position.y, Slots[s].position.z, Slots[s].radius, Slots[s].fade,
						source ? source->m_worldTransform.pos.x : 0.0f, source ? source->m_worldTransform.pos.y : 0.0f, source ? source->m_worldTransform.pos.z : 0.0f,
						source ? source->Spec.r : 0.0f, source ? source->Diff.r : 0.0f, source ? source->Diff.g : 0.0f, source ? source->Diff.b : 0.0f, source ? source->Dimmer : 0.0f);
				}
			}
		}
		lastCount = SlotCount;
		Active = SlotCount > 0;
		// UNOFFICIAL 2026-10-28 RT lamp shadows: this frame's trace request and the newest traced result (RtLampShadows.h)
		if (RtLampShadows::CompiledIn) {
			RtLampShadows::SlotLamp traced[ShadowCubeMapsMax];
			const float fixture = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward.FixtureDistance;
			for (int s = 0; s < SlotCount; s++) {
				// The lamp's own glowing part round its light (a globe, a bulb, a shade's glass): the rays stop before its far side
				float stop = 0.0f, size = 0.0f;
				const NiPointLight* source = Slots[s].light ? Slots[s].light->sourceLight : nullptr;
				if (const LampFixtures::Glow* g = source ? LampFixtures::NearestLampPart(source, fixture) : nullptr) {
					size = g->radius;	// UNOFFICIAL 2026-11-07 ShadowDenoiser's PhysicalLampSize: the glowing part is the emitter
					const NiPoint3& p = source->m_worldTransform.pos;
					const float dx = g->x - p.x, dy = g->y - p.y, dz = g->z - p.z;
					stop = sqrtf(dx * dx + dy * dy + dz * dz) + g->radius + 2.0f;
				}
				// The per-pixel ranking's intensity (luminance x dimmer) and the game's radius
				const float intensity = source ? (0.2126f * source->Diff.r + 0.7152f * source->Diff.g + 0.0722f * source->Diff.b) * source->Dimmer : 1.0f;
				const float gameRadius = source ? source->Spec.r : Slots[s].radius;
				traced[s] = { Slots[s].slot, Slots[s].position, Slots[s].radius, Slots[s].fade, stop, intensity, gameRadius, Slots[s].light, size };
			}
			RtLampShadows::Submit(traced, SlotCount);
		}
		{ const int want = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.Atlas", "TraceSlot");	// UNOFFICIAL 2026-10-30 (test): DebugView 3 on another live slot
		for (int s = 0; s < SlotCount; s++) if (Slots[s].slot == want) { Focus = s; break; } }
		// DebugView 3: the focus lamp and what its cube maps hold, every 2 seconds (with the draws of the period before).
		static double lastTrace = 0.0;
		if (forward.DebugView >= 3 && now - lastTrace >= 2.0) {
			lastTrace = now;
			if (Focus < 0) Logger::Log("TRACE no lamp in cube slot 0 this frame (%d lamps with shadows)", SlotCount);
			else {
				const Slot& f = Slots[Focus];
				const NiPointLight* source = f.light->sourceLight;
				Logger::Log("TRACE focus lamp %p (cube slot 0) at %.0f %.0f %.0f, cube radius %.0f, reach %.0f, fade %.2f, camera %.0f %.0f %.0f; last period %u draws got it, %u of them lit by it",
					(const void*)f.light, f.position.x, f.position.y, f.position.z, f.radius, source ? source->Spec.r : -1.0f, f.fade,
					TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z,
					TraceDraws, TraceDrawsWithLamp);
				TraceCube("live cube", f.cube);
				TraceCube("static cube", TheShaderManager->Effects.ShadowsExteriors->Textures.ShadowCubeMapStaticTexture[f.slot]);
			}
			TraceDraws = TraceDrawsWithLamp = 0;
		}
	}

	// UNOFFICIAL 2026-10-29 ShadowPool (core/ShadowPool.h): the pool lamps' static cube maps after the live slots Publish put in, with
	// the same fill / invisible light rules (Publish's) and FadeIn from when each was first published. Then the lamp -> entry map.
	struct PoolEntry { const ShadowSceneLight* light; D3DXVECTOR3 position; float radius; IDirect3DCubeTexture9* cube; int slot; double since; };
	inline void PublishPool(const PoolEntry* entries, int count) {
		UseIndex = false;
		if (!CompiledIn || Refused || count <= 0) return;
		const ShadowsExteriorEffect::InteriorsStruct::ForwardStruct& forward = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward;
		if (!forward.Enabled || !(forward.Strength > 0.0f)) return;
		LARGE_INTEGER frequency, counter;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		const double now = (double)counter.QuadPart / (double)frequency.QuadPart;
		const bool hold = ShadowHold::Seconds() > 0.0f;	// UNOFFICIAL 2026-10-30 ShadowHold
		int liveCount = 0;	// UNOFFICIAL 2026-11-01 FadeOut: the live slots Publish put in (0 when none is fading out)
		for (int s = 0; s < ShadowCubeMapsMax; s++) if (FadeOutLamp[s] && FadeOutLeft[s] < 1.0f) { liveCount = SlotCount; break; }
		for (int i = 0; i < count && SlotCount < ShadowCubeMapsMax + ShadowPoolMax; i++) {
			const PoolEntry& e = entries[i];
			const double since = hold ? ShadowHold::Since(e.light, now) : e.since;	// UNOFFICIAL 2026-10-30 ShadowHold
			float fade = forward.FadeIn > 0.0f ? (float)(std::min)(1.0, (now - since) / forward.FadeIn) : 1.0f;
			const NiPointLight* source = e.light->sourceLight;
			if (forward.FillLightRadius > 0.0f && source && source->Spec.r > forward.FillLightRadius) fade *= forward.FillLightShadowStrength;
			if ((forward.InvisibleLightShadowStrength < 1.0f || InvisibleSize > 0.0f) && source && !TheShaderManager->GameState.isExterior &&
				LampFixtures::Invisible(source, forward.FixtureDistance)) {
				fade *= forward.InvisibleLightShadowStrength;
				if (InvisibleSize > 0.0f) fade = -fade;
			}
			// UNOFFICIAL 2026-11-01 FadeOut: a lamp fading out in a live slot that the pool holds too keeps its shadow (the pool's still map
			// takes over next frame, when GetNearbyLights lets the live slot go): its live entry gets the pool entry's fade, so it does not dip.
			for (int s = 0; s < liveCount; s++) {
				const int cube = Slots[s].slot;
				if (Slots[s].light != e.light || cube < 0 || cube >= ShadowCubeMapsMax || FadeOutLamp[cube] != e.light || !(FadeOutLeft[cube] < 1.0f)) continue;
				Slots[s].fade = fade;
				FadeOutPooled[cube] = true;
			}
			Slots[SlotCount++] = { e.light, e.position, e.radius, e.cube, e.slot, fade };
		}
		SlotIndex.clear();
		for (int s = 0; s < SlotCount; s++) SlotIndex.emplace(Slots[s].light, s);	// (a lamp in both: the live slot, first, wins)
		UseIndex = true;
		Active = SlotCount > 0;
	}

	inline IDirect3DCubeTexture9* NeutralCube() {
		if (!Neutral && SUCCEEDED(TheRenderManager->device->CreateCubeTexture(1, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &Neutral, NULL))) {
			for (int face = 0; face < 6; face++) {
				D3DLOCKED_RECT lock;
				if (SUCCEEDED(Neutral->LockRect((D3DCUBEMAP_FACES)face, 0, &lock, NULL, 0))) {
					*(float*)lock.pBits = 1.0f;
					Neutral->UnlockRect((D3DCUBEMAP_FACES)face, 0);
				}
			}
		}
		return Neutral;
	}

	// Linear like the screen-space pass (PointShadows.fx). Through the game's render state cache, as ShaderRecord::SetCT binds
	// textures: the game then knows the stage changed
	// and rebinds its own texture there when a later shader needs one.
	// UNOFFICIAL (2026-10-08) device: also bind on the device itself. The cache skips the call when it already shows this
	// texture, and the post effects bind their samplers on the device directly (PointShadows.fx puts slots 6/9/10's cube
	// maps on s8/s11/s12, ours): the next frame's objects then sampled ANOTHER lamp's cube map until something else was
	// bound there -- a per-object shadow that came and went with the draw order (the TOPSCasino floor flicker). Done on
	// every shader bind; between objects the cache is right again.
	// point: POINT filtering (UNOFFICIAL 2026-10-28: the RT lamp shadows' packed result on s15 must never be blended).
	inline void Bind(DWORD stage, IDirect3DBaseTexture9* texture, bool device = false, bool point = false) {
		TheRenderManager->renderState->SetTexture(stage, texture);
		if (device) TheRenderManager->device->SetTexture(stage, texture);
		const DWORD filter = point ? D3DTEXF_POINT : D3DTEXF_LINEAR;
		const DWORD states[][2] = { { D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP }, { D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP },
			{ D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP }, { D3DSAMP_MAGFILTER, filter }, { D3DSAMP_MINFILTER, filter },
			{ D3DSAMP_MIPFILTER, D3DTEXF_NONE }, { D3DSAMP_SRGBTEXTURE, FALSE } };
		for (const auto& s : states)
			if (TheRenderManager->renderState->GetSamplerState(stage, (D3DSAMPLERSTATETYPE)s[0]) != s[1])
				TheRenderManager->SetSamplerState(stage, (D3DSAMPLERSTATETYPE)s[0], s[1]);
	}

	inline bool Bound = false;				// the pixel shader the game bound last is one of the records that read c174-c188
	inline float Uploaded[16][4] = {};		// c174-c189 as last written

	// (2026-11-05) the land shaders with lamps (TerrainTemplate SLS2092-2146, even numbers, 6 / 12 / 24 slots: Render.cpp TerrainLampSlots)
	inline unsigned LandLampSlots(const char* name) {
		if (!name || strncmp(name, "SLS", 3)) return 0;
		const int number = atoi(name + 3);
		if (number < 2092 || number > 2146 || (number & 1)) return 0;
		static const unsigned slots[4] = { 0, 6, 12, 24 };
		return slots[((number - 2092) / 2) % 4];
	}
	inline unsigned LandCensus[4] = {};	// land lamp draws while ExteriorActive: no record, record not bound (other handle), not a land record, bound
	inline unsigned LandCensusFrames = 0;
	inline void LandCensusFrame() {
		if (++LandCensusFrames < 600) return;
		if (LandCensus[0] | LandCensus[1] | LandCensus[2] | LandCensus[3])
			Logger::Log("UNOFFICIAL RT lamp shadows: the land's lamp draws over %u frames: %u with the RT record bound, %u with no record, "
				"%u whose record was not the bound shader, %u not land records", LandCensusFrames, LandCensus[3], LandCensus[0], LandCensus[1], LandCensus[2]);
		LandCensusFrames = 0; LandCensus[0] = LandCensus[1] = LandCensus[2] = LandCensus[3] = 0;
	}

	// UNOFFICIAL 2026-10-30 ExteriorForward: ShadowManager, outdoors after RtLampShadows::SubmitExterior: the shadow lamps as slots
	// (slot = their index in ShadowLights, as in the trace's table; no cube maps), for the outdoor records' per-draw lookups
	inline void PublishExterior(ShadowSceneLight* const* lights, int count) {
		ExteriorActive = false;
		if (!CompiledIn || ExteriorRecords.empty() || !RtLampShadows::ExteriorForwardFrame || !RtCore::Running()) return;
		const ShadowsExteriorEffect::InteriorsStruct& interiors = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors;
		const float strength = interiors.Forward.Strength;
		if (!(strength > 0.0f)) return;
		Params[0] = (std::min)(strength, 1.0f); Params[1] = 0.0f; Params[2] = 0.0f; Params[3] = 0.0f;
		Soft[0] = Soft[1] = Soft[2] = Soft[3] = 0.0f;
		Magnified[0] = Magnified[1] = Magnified[2] = Magnified[3] = 0.0f;
		Quality = 0.0f; DebugView = 0.0f; NearFade = 0.0f; InvisibleSize = 0.0f;
		IDirect3DCubeTexture9* neutral = NeutralCube();
		for (int i = 0; i < count && i < ShadowCubeMapsMax && SlotCount < ShadowCubeMapsMax; i++) {
			const NiPointLight* source = lights[i] ? lights[i]->sourceLight : nullptr;
			if (!source || !(source->Spec.r > 0.0f)) continue;
			Slots[SlotCount++] = { lights[i], D3DXVECTOR3(source->m_worldTransform.pos.x, source->m_worldTransform.pos.y, source->m_worldTransform.pos.z),
				source->Spec.r * interiors.LightRadiusMult, neutral, i, 1.0f };
		}
		Focus = -1;
		Active = ExteriorActive = SlotCount > 0;
		LandCensusFrame();
	}

	// UNOFFICIAL 2026-10-31 ManyLights: a lamp without a shadow map of its own gets Publish's fill / invisible light rules (no FadeIn)
	inline float ManyLampFade(const NiPointLight* source) {
		const ShadowsExteriorEffect::InteriorsStruct::ForwardStruct& forward = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors.Forward;
		float fade = 1.0f;
		if (forward.FillLightRadius > 0.0f && source->Spec.r > forward.FillLightRadius) fade *= forward.FillLightShadowStrength;
		if ((forward.InvisibleLightShadowStrength < 1.0f || InvisibleSize > 0.0f) && LampFixtures::Invisible(source, forward.FixtureDistance)) {
			fade *= forward.InvisibleLightShadowStrength;
			if (InvisibleSize > 0.0f) fade = -fade;
		}
		return fade;
	}
	// ManyLights: a draw's point light into the next trace's table (RtLampShadows::NoteLamp; the cube-slot lamps are there already)
	inline void NoteManyLamp(const ShadowSceneLight* light) {
		if (!RtLampShadows::Noting(light)) return;
		const NiPointLight* source = light->sourceLight;
		if (!source || !(source->Spec.r > 0.0f)) return;
		// UNOFFICIAL 2026-11-07 (perf-flashlight; docs/next/RT-PATH-TRACING.md "Team perf-flashlight") not the flashlight's game lamp (the
		// Pip-Boy light the Flashlight effect moves onto the beam, core/FlashlightLamp.h), as every other NVR lamp list leaves it out: the
		// light pass is the flashlight on what the G-buffer holds (with its own shadow map), and its dimmer here is the DRAW's (0 for those
		// objects, the cone for the rest), so the table took whichever the frame's first draw had (0.001 or full) and the pixels' traced
		// lamps changed with it, frame to frame; outdoors (ExteriorForward, which RT testers turn on, + ExteriorManyLights, on by default)
		// every frame with the flashlight on; indoors with ManyLights. Without it the lamp's light on the few draws that get it stays unshadowed, as indoors without ManyLights.
		if (FlashlightLamp::Is(source)) return;
		const ShadowsExteriorEffect::InteriorsStruct& interiors = TheShaderManager->Effects.ShadowsExteriors->Settings.Interiors;
		const NiPoint3& p = source->m_worldTransform.pos;
		float stop = 0.0f;
		if (const LampFixtures::Glow* g = LampFixtures::NearestLampPart(source, interiors.Forward.FixtureDistance)) {
			const float dx = g->x - p.x, dy = g->y - p.y, dz = g->z - p.z;
			stop = sqrtf(dx * dx + dy * dy + dz * dz) + g->radius + 2.0f;
		}
		const float intensity = (0.2126f * source->Diff.r + 0.7152f * source->Diff.g + 0.0722f * source->Diff.b) * source->Dimmer;
		RtLampShadows::SlotLamp lamp = { 0, D3DXVECTOR3(p.x, p.y, p.z), source->Spec.r * interiors.LightRadiusMult, ManyLampFade(source), stop,
			(std::max)(intensity, 0.001f), source->Spec.r };
		RtLampShadows::NoteLamp(light, lamp);
	}

	// One object's render pass: its lamps to c174-c187 and the cube stages. wanted: the world pass, or the first-person
	// pass with FirstPerson on (never the water reflection). force: just after a shader bind, write everything.
	inline void SetForPass(const RenderPassView* pass, bool world, bool firstPerson, bool force) {
		float constants[16][4] = {};// c174-c179 per light of the draw; c180 params (x 0: the shader skips it all);
										// c181 debug view, near fade, (view 3) focus lamp's light index; c182-c187 per light of the draw: rgb debug
										// colour (white = no shadow), w how far its shadow has faded in (FadeIn); c188 (view 3) the focus lamp;
											// c181.w Quality; c189 contact hardening (strength, narrowest spread share)
		for (int k = 0; k < 6; k++) constants[8 + k][0] = constants[8 + k][1] = constants[8 + k][2] = 1.0f;
		IDirect3DBaseTexture9* bound[6];
		IDirect3DCubeTexture9* neutral = NeutralCube();
		for (int k = 0; k < 6; k++) bound[k] = neutral;
		int matched = 0;
		float tracedSlots[6] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };	// UNOFFICIAL 2026-10-28 RT lamp shadows: per light, -1 = cube map
		const bool trace = DebugView > 2.5f;	// DebugView 3: PointShadowTrace (PointShadowForward.hlsl)
		int focusIndex = -1;					// view 3: which light of the draw the focus lamp is
		bool wanted = false;
		if (Active && (world || (firstPerson && FirstPerson)) && pass && pass->lights && !OtherCamera(pass, firstPerson)) {
			wanted = true;
			const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;	// GetShadowWorldPos's origin
			const int first = FirstShaderLight(pass);	// k below is the shader's light (PSLightColor[k]), pass light first + k
			const int count = (std::min)((int)pass->numLights - first, 6);
			for (int k = 0; k < count; k++) {
				const ShadowSceneLight* light = pass->lights[first + k];
				if (!light || !light->bPointLight) continue;	// light 0 of most passes is the cell's directional light
				if (trace && Focus >= 0 && Slots[Focus].light == light) focusIndex = k;
				int first = 0, last = SlotCount;	// UNOFFICIAL 2026-10-29 ShadowPool: straight to the lamp's entry when the map is up
				if (RtLampShadows::ManyNoting) NoteManyLamp(light);	// UNOFFICIAL 2026-10-31 ManyLights: into the next trace's table
				if (UseIndex) { auto found = SlotIndex.find(light); if (found == SlotIndex.end()) first = last = 0; else { first = found->second; last = first + 1; } }
				bool done = false;
				for (int s = first; s < last; s++) {
					if (Slots[s].light != light) continue;
					constants[k][0] = Slots[s].position.x - camera.x;
					constants[k][1] = Slots[s].position.y - camera.y;
					constants[k][2] = Slots[s].position.z - camera.z;
					constants[k][3] = 1.0f / Slots[s].radius;
					bound[k] = Slots[s].cube;
					static const float palette[11][3] = { { 1, 0.15f, 0.1f }, { 0.1f, 1, 0.15f }, { 0.15f, 0.35f, 1 },
						{ 1, 0.9f, 0.1f }, { 0.1f, 1, 1 }, { 1, 0.2f, 1 }, { 1, 0.55f, 0.1f }, { 0.55f, 0.25f, 1 },
						{ 0.6f, 1, 0.3f }, { 1, 0.45f, 0.6f }, { 0.4f, 0.8f, 1 } };	// by cube slot
					// red for cube slot 0 alone (DebugView 3's lamp); the other slots share the other ten colours
					memcpy(constants[8 + k], palette[Slots[s].slot == 0 ? 0 : 1 + (Slots[s].slot - 1) % 10], 3 * sizeof(float));
					if (AtlasTexture) {	// UNOFFICIAL 2026-10-30 ShadowAtlas: the tile instead of the debug colour; no tile, no shadow
						if (!(Slots[s].tile[2] > 0.0f)) {
							// UNOFFICIAL 2026-11-06: no tile (an atlas without room; before 2026-11-06 b cube slots 32-63 had none) but the trace has the lamp: its traced
							// shadow (span 0 = no map: PointShadowCube is lit where the traced frame did not see the point). Was: unshadowed even
							// though traced (VS-AlphaLampTest: the fence lamps in slots 39 / 44 cast nothing).
							const float traced = RtLampShadows::TracedMany ? -1.0f : RtLampShadows::TracedSlot(Slots[s].slot);
							if (traced >= 0.0f) {
								constants[8 + k][0] = constants[8 + k][1] = constants[8 + k][2] = 0.0f; constants[8 + k][3] = Slots[s].fade;
								tracedSlots[k] = traced; matched++; done = true; break;
							}
							constants[k][0] = constants[k][1] = constants[k][2] = constants[k][3] = 0.0f; constants[8 + k][0] = constants[8 + k][1] = constants[8 + k][2] = 1.0f; break;
						}
						memcpy(constants[8 + k], Slots[s].tile, 3 * sizeof(float));
					}
					constants[8 + k][3] = Slots[s].fade;	// FadeIn
					// ManyLights: the lamp's id in the newest result (its cube map the fallback); without ManyLights its cube slot
					const int manyId = RtLampShadows::ManyIdOf(light);
					tracedSlots[k] = manyId >= 0 ? (float)manyId : RtLampShadows::TracedMany ? -1.0f : RtLampShadows::TracedSlot(Slots[s].slot);
					matched++;
					done = true;
					break;
				}
				// UNOFFICIAL 2026-10-31 ManyLights: a lamp with no shadow map here (no cube slot, no pool map, no atlas tile) that the newest
				// result traced: shadowed by the trace where the traced frame saw the point, lit elsewhere (id + 256: no cube lookup)
				float reach = 0.0f;
				const int manyId = done ? -1 : RtLampShadows::ManyIdOf(light, &reach);
				if (manyId >= 0 && reach > 0.0f && light->sourceLight) {
					const NiPoint3& at = light->sourceLight->m_worldTransform.pos;
					constants[k][0] = at.x - camera.x;
					constants[k][1] = at.y - camera.y;
					constants[k][2] = at.z - camera.z;
					constants[k][3] = 1.0f / reach;
					bound[k] = neutral;
					constants[8 + k][0] = constants[8 + k][1] = constants[8 + k][2] = 1.0f;
					constants[8 + k][3] = ManyLampFade(light->sourceLight);
					tracedSlots[k] = (float)(manyId + 256);
					matched++;
				}
			}
		}
		if (matched) { memcpy(constants[6], Params, sizeof(Params)); memcpy(constants[15], Soft, sizeof(Soft)); memcpy(constants[14], Magnified, sizeof(Magnified)); }
		constants[7][0] = matched ? DebugView : 0.0f;
		constants[7][1] = matched ? NearFade : 0.0f;
		constants[7][2] = matched ? InvisibleSize : 0.0f;	// (DebugView 3 puts the focus lamp's index here below)
		constants[7][3] = matched ? Quality : 0.0f;
		// DebugView 3 on every draw of the wanted passes, matched or not: the focus lamp at c188, its cube map on stage 15
		// (NVR_PointShadowCube5: the view replaces the draw's own lighting, so light 5's lookup does not matter).
		if (trace && wanted) {
			memcpy(constants[6], Params, sizeof(Params));
			memcpy(constants[15], Soft, sizeof(Soft));
			constants[7][0] = DebugView;
			constants[7][3] = Quality;
			constants[7][2] = (float)focusIndex;
			if (Focus >= 0) {
				const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
				constants[14][0] = Slots[Focus].position.x - camera.x;
				constants[14][1] = Slots[Focus].position.y - camera.y;
				constants[14][2] = Slots[Focus].position.z - camera.z;
				constants[14][3] = 1.0f / Slots[Focus].radius;
				bound[5] = Slots[Focus].cube;
				if (AtlasTexture) memcpy(constants[13], Slots[Focus].tile, 3 * sizeof(float));	// UNOFFICIAL 2026-10-30 ShadowAtlas: light 5's tint = the focus tile
				if (!force) { TraceDraws++; if (focusIndex >= 0) TraceDrawsWithLamp++; }
			}
		}
		// Every declared cube stage gets a cube texture, matched or not: a 2D texture left there by another shader would be
		// the wrong type for the sampler. Between objects only what changed (the game's texture cache tells).
		// UNOFFICIAL 2026-10-28 RT lamp shadows: in POINT_SHADOW_RT shaders s15 is the traced result (a 2D texture, point sampled)
		const bool rtStage = RtLampShadows::CompiledIn;
		if (rtStage) bound[5] = RtLampShadows::Stage15Texture();
		for (int k = 0; k < 6; k++) {
			if (ExteriorActive && k < 5) continue;	// outdoors the RT-only records have no cube stages (s8, s11-s14 are the sun's there)
			// UNOFFICIAL 2026-10-30 ShadowAtlas: the atlas on s8 (2D), no cube stages (s15 still RT's when compiled in)
			if (AtlasTexture && !ExteriorActive) {
				if (k == 0 && (force || TheRenderManager->renderState->GetTexture(Stage[0]) != AtlasTexture)) Bind(Stage[0], AtlasTexture, force, false);
				if (k < 5 || !rtStage) continue;
			}
			if (force || TheRenderManager->renderState->GetTexture(Stage[k]) != bound[k]) Bind(Stage[k], bound[k], force, rtStage && k == 5);
		}
		if (force || memcmp(constants, Uploaded, sizeof(constants))) {
			TheRenderManager->device->SetPixelShaderConstantF(174, &constants[0][0], 16);
			memcpy(Uploaded, constants, sizeof(constants));
		}
		// UNOFFICIAL 2026-11-07 the first-person model (arms, gun, Pip-Boy) is never in the traced scene: its lookups never match. Where a
		// traced lamp has no map to fall back to (outdoors: no cube maps; ManyLights' lamps without one, + 256) it was lit fully however
		// blocked; it takes the lamp as the eye sees it instead (RtLampEye.comp; the shaders read slot -2 - v). Indoors a lamp with a
		// cube map keeps it (the map's answer at the model's own points is the better one).
		if (firstPerson && wanted && RtLampShadows::CompiledIn && pass && pass->lights) {
			const int firstLight = FirstShaderLight(pass);
			const int lightCount = (std::min)((int)pass->numLights - firstLight, 6);
			for (int k = 0; k < lightCount; k++) {
				if (!(tracedSlots[k] >= 0.0f) || !(ExteriorActive || tracedSlots[k] > 255.5f)) continue;
				float v = 1.0f;
				if (RtLampShadows::EyeVisibility(pass->lights[firstLight + k], v)) tracedSlots[k] = -2.0f - (std::min)((std::max)(v, 0.0f), 1.0f);
			}
		}
		RtLampShadows::SetForPass(tracedSlots, force);	// c206-c209, c215-c217 (nothing without POINT_SHADOW_RT)
		if (GpuTimer::Enabled && !force) { StatDraws++; if (matched) { StatMatchedDraws++; StatMatchedLights += matched; } }
	}

	// UNOFFICIAL 2026-10-30 the land's outdoor lamp-shadow record (TerrainTemplate.hlsl TERRAIN_LAMP_RT): the traced slots' positions
	// (camera-relative, slots 0-19) c146-c165, the reprojection c168-c171, c172 (on, depth tolerance, depth code unit, strength), the
	// traced result on s15. The same for every land draw of the frame.
	inline void BindTerrain() {
		float c[27][4] = {};
		const D3DXVECTOR4& camera = TheRenderManager->CameraPosition;
		for (int j = 0; j < 20; j++) {
			const D3DXVECTOR4& w = RtLampShadows::TracedLampWorld[j];
			c[j][0] = w.x - camera.x; c[j][1] = w.y - camera.y; c[j][2] = w.z - camera.z; c[j][3] = w.w;
		}
		TheRenderManager->device->SetPixelShaderConstantF(146, &c[0][0], 20);
		float r[5][4] = {};
		memcpy(r, RtLampShadows::Reproject, sizeof(RtLampShadows::Reproject));
		const bool on = RtLampShadows::Mode > 0.5f && RtLampShadows::Mode < 1.5f;	// traced (not the debug views)
		r[4][0] = on ? 1.0f : 0.0f; r[4][1] = RtLampShadows::RtConstant[1]; r[4][2] = RtLampShadows::RtConstant[2]; r[4][3] = Params[0];
		TheRenderManager->device->SetPixelShaderConstantF(168, &r[0][0], 5);
		// c173 (2026-11-03 rtext): x the result's packing unit (1024, or 4096 in the ManyLights packing), y 1 = ManyLights (entry 2 = R)
		// z (2026-11-05) [Shaders.ShadowsInteriors.RT] ExteriorDebug 3: the land shows its traced lamp shadows (red blocked, green lit)
		static UInt32 debugFrame = 0xFFFFFFFFu;
		static float landDebug = 0.0f;
		if (debugFrame != TheFrameRateManager->FrameNumber) {
			debugFrame = TheFrameRateManager->FrameNumber;
			landDebug = TheSettingManager->GetSettingI("Shaders.ShadowsInteriors.RT", "ExteriorDebug") == 3 ? 1.0f : 0.0f;
		}
		// w (2026-11-07): 1 / the result's width, the land lookup's edge search (TerrainTemplate.hlsl TerrainLampEntries)
		const float packing[4] = { 1.0f / RtLampShadows::PackingInverse(), RtLampShadows::TracedMany ? 1.0f : 0.0f, landDebug,
			RtLampShadows::TextureWidth > 2 ? 1.0f / (float)RtLampShadows::TextureWidth : 0.0f };
		TheRenderManager->device->SetPixelShaderConstantF(173, packing, 1);
		IDirect3DBaseTexture9* traced = RtLampShadows::Stage15Texture();
		Bind(15, traced, true, true);
	}

	// SetShadersHook, after the game bound a pass's shaders and set its constants.
	inline void SetForDraw(NiD3DPixelShaderEx* pixelShader, bool world, bool firstPerson, const RenderPassView* steady = nullptr) {	// steady: ManyLamps::Steady's copy (2026-10-31)
		Bound = false;
		LastPixelShaderName = pixelShader ? pixelShader->Name : nullptr;	// ObjectLamps diagnostic
		LastPixelShaderShadowed = false;
		if (!CompiledIn || !pixelShader) return;
		ShaderRecordPixel* interiorRecord = pixelShader->GetShaderRecord(ShaderRecordType::Interior);
		bool ours = interiorRecord && interiorRecord->PointShadowForward && pixelShader->ShaderHandle == interiorRecord->ShaderHandle;
		if (!ours && ExteriorActive) {	// UNOFFICIAL 2026-10-30 ExteriorForward: the outdoor record
			auto found = ExteriorRecords.find(pixelShader);
			ours = found != ExteriorRecords.end() && pixelShader->ShaderHandle == found->second->ShaderHandle;
			// (2026-11-05 rtext-land) census of the land's lamp draws: the land debug view showed nothing where the land is lamp-lit
			if (const unsigned landSlots = LandLampSlots(pixelShader->Name)) {
				if (found == ExteriorRecords.end()) LandCensus[0]++;
				else if (!ours) LandCensus[1]++;
				else if (!TerrainRecords.count(pixelShader)) LandCensus[2]++;
				else LandCensus[3]++;
			}
			if (ours && TerrainRecords.count(pixelShader)) { BindTerrain(); return; }	// the land: its own constants, no per-object lamps
		}
		if (!ours)
			return;	// only those records read c174-c187
		Bound = true;
		LastPixelShaderShadowed = Active;
		BoundThisFrame = true;
		SetForPass(steady ? steady : *(RenderPassView**)0x011F91E0, world, firstPerson, true);
	}

	// BSBatchRenderer::RenderPassImmediately_Standard/_Skinned: once per object, after the bind (if any) for its run.
	inline void OnRenderPass(const RenderPassView* pass, bool world, bool firstPerson) {
		if (Bound) SetForPass(pass, world, firstPerson, false);
	}

	// ObjectLamps: frame o.frame (lamps/how) against the frame before it (last/lastHow) for one object. One line per object
	// whose lamps changed while the player stood still and the camera moved (what turning the camera does), at most two a
	// frame: + lamps that came, - lamps that left, ~ lamps whose kind of pass changed. Per lamp: its cube map slot (-1 none),
	// how far its reach falls short of the object's bound (negative: it overlaps), its reach, the game's LOD dimmer and fade
	// for it and the cube faces the game counts in view (ShadowSceneLight::TestFrustumCull, 0x00B9E970), and its distance to
	// the camera. A lamp's fields are read only while it holds a cube map this frame (then it is alive).
	inline unsigned ObjectLampFrameLines = 0, ObjectLampLinesFrame = 0;
	inline void CompareObjectLamps(const void* geometry, const ObjectLampState& o) {
		const CameraState& a = Cameras[o.lastFrame & 7];
		const CameraState& b = Cameras[o.frame & 7];
		const bool known = a.frame == o.lastFrame && b.frame == o.frame;
		const float turn = known ? D3DXToDegree(acosf((std::max)(-1.0f, (std::min)(1.0f, D3DXVec3Dot(&a.forward, &b.forward))))) : -1.0f;
		const D3DXVECTOR3 camMove = b.position - a.position, playerMove = b.player - a.player;
		const float moved = known ? D3DXVec3Length(&camMove) : -1.0f, walked = known ? D3DXVec3Length(&playerMove) : -1.0f;
		auto find = [](const ShadowSceneLight* const* lamps, int count, const ShadowSceneLight* light) { for (int i = 0; i < count; i++) if (lamps[i] == light) return i; return -1; };
		auto slotOf = [](const ShadowSceneLight* light) { for (int s = 0; s < SlotCount; s++) if (Slots[s].light == light) return Slots[s].slot; return -1; };
		const NiAVObject* object = (const NiAVObject*)geometry;
		char text[1400] = "";
		size_t used = 0;
		int changes = 0;
		for (int round = 0; round < 2; round++) {
			const int count = round ? o.lastCount : o.count;
			for (int i = 0; i < count; i++) {
				const ShadowSceneLight* light = round ? o.last[i] : o.lamps[i];
				if (round && find(o.lamps, o.count, light) >= 0) continue;	// compared in the first round
				const int now = find(o.lamps, o.count, light), before = find(o.last, o.lastCount, light);
				const int howNow = now >= 0 ? o.how[now] : 0, howBefore = before >= 0 ? o.lastHow[before] : 0;
				if (howNow == howBefore) continue;
				changes++;
				if (!howBefore) StatLampAppeared++;
				else if (!howNow) StatLampGone++;
				else if ((howBefore & 1) && !(howNow & 1)) StatLampLostShadow++;
				else if (!(howBefore & 1) && (howNow & 1)) StatLampGotShadow++;
				if (known && walked < 0.5f) StatLampStill++;
				if (used + 200 > sizeof(text)) continue;
				const char sign = !howBefore ? '+' : !howNow ? '-' : '~';
				const int slot = slotOf(light);
				if (slot >= 0 && light->sourceLight && object->m_kWorldBound) {
					const NiPoint3& lamp = light->sourceLight->m_worldTransform.pos;
					const NiBound& bound = *object->m_kWorldBound;
					const float dx = lamp.x - bound.Center.x, dy = lamp.y - bound.Center.y, dz = lamp.z - bound.Center.z;
					const float reach = light->sourceLight->Spec.r;
					const float gap = sqrtf(dx * dx + dy * dy + dz * dz) - bound.Radius - reach;
					const float cx = lamp.x - b.position.x, cy = lamp.y - b.position.y, cz = lamp.z - b.position.z;
					used += sprintf_s(text + used, sizeof(text) - used, " %c%p[slot %d, gap %.0f, reach %.0f, LOD %.2f, fade %.2f, faces out of view %02X (game %02X), %.0f from camera, Vegas Shaders view test %s]",
						sign, (const void*)light, slot, gap, reach, light->fLODDimmer, light->fFade, (unsigned)light->bIsEnabled,
						GameCullMask.count(light) ? (unsigned)GameCullMask[light] : 0xFFFFu, sqrtf(cx * cx + cy * cy + cz * cz), LampReachesView(light) ? "in" : "out");
				}
				else used += sprintf_s(text + used, sizeof(text) - used, " %c%p[no cube map]", sign, (const void*)light);
			}
		}
		if (!changes || !known || !(walked < 0.5f) || !(turn > 0.01f || moved > 0.5f)) return;
		if (ObjectLampLinesFrame != Frame) { ObjectLampLinesFrame = Frame; ObjectLampFrameLines = 0; }
		if (ObjectLampLines >= 600 || ObjectLampFrameLines >= 2) return;
		ObjectLampFrameLines++;
		if (++ObjectLampLines == 600) Logger::Log("OBJECT lamps log: 600 lines, stopping until restart.");
		Logger::Log("OBJECT %s %p (bound radius %.0f): lamps %d -> %d,%s | camera turned %.2f deg, moved %.1f",
			object->m_pcName ? object->m_pcName : "(unnamed)", geometry, object->m_kWorldBound ? object->m_kWorldBound->Radius : -1.0f,
			(int)o.lastCount, (int)o.count, text, turn, moved);
	}

	// AimProbe: the hits of the frame just drawn, if they differ from the last ones logged.
	inline void AimFlush() {
		if (!AimHitCount || AimLines >= 500 || Frame - AimLastLogFrame < 30) return;
		bool same = AimHitCount == AimLoggedCount;
		for (int i = 0; same && i < AimHitCount; i++) {
			bool found = false;
			for (int j = 0; j < AimLoggedCount; j++) if (AimLogged[j] == AimHits[i].geometry) found = true;
			same = found;
		}
		if (same) return;
		AimLastLogFrame = Frame;
		AimLoggedCount = AimHitCount;
		for (int i = 0; i < AimHitCount; i++) AimLogged[i] = AimHits[i].geometry;
		// nearest the line of sight first
		for (int i = 1; i < AimHitCount; i++)
			for (int j = i; j > 0 && AimHits[j].off < AimHits[j - 1].off; j--) { AimHit t = AimHits[j]; AimHits[j] = AimHits[j - 1]; AimHits[j - 1] = t; }
		AimLines++;
		Logger::Log("AIM %d objects on the line of sight (camera %.0f %.0f %.0f)", AimHitCount,
			TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
		auto slotOf = [](const ShadowSceneLight* light) { for (int s = 0; s < SlotCount; s++) if (Slots[s].light == light) return Slots[s].slot; return -1; };
		for (int i = 0; i < AimHitCount && AimLines < 500; i++) {
			const AimHit& h = AimHits[i];
			const NiAVObject* object = (const NiAVObject*)h.geometry;
			char text[1200] = "";
			size_t used = 0;
			for (int p = 0; p < h.passes && used + 160 < sizeof(text); p++) {
				const AimPass& a = h.pass[p];
				used += sprintf_s(text + used, sizeof(text) - used, " | %s (%s) + %s (%s) %s, pass %04X, %d lights%s:", a.vs ? a.vs : "?",
					a.vsNvr ? "Vegas Shaders" : "game", a.ps ? a.ps : "?", a.psNvr ? "Vegas Shaders" : "game", a.shadowed ? "forward shadows" : "NO forward shadows",
					(unsigned)a.passEnum, (int)a.numLights, a.shadowed && !a.strength ? " (strength 0: lookups skipped)" : "");
				for (int k = 0; k < a.count && used + 40 < sizeof(text); k++) {
					if (!a.point[k]) { used += sprintf_s(text + used, sizeof(text) - used, " %d:dir", (int)a.index[k]); continue; }
					const int slot = slotOf(a.lamps[k]);
					used += sprintf_s(text + used, sizeof(text) - used, " %d:%p/s%d%s", (int)a.index[k], (const void*)a.lamps[k], slot,
						!a.shadowed ? "" : a.uploaded[k] ? "" : slot >= 0 ? "(NOT UPLOADED)" : "");
				}
			}
			AimLines++;
			Logger::Log("AIM   %s %p: bound radius %.0f, centre %.0f along the line, %.0f off it%s", object->m_pcName ? object->m_pcName : "(unnamed)",
				h.geometry, h.radius, h.along, h.off, text);
		}
	}

	inline void NoteAim(const RenderPassView* pass) {
		if (AimFrame != Frame) { AimFlush(); AimHitCount = 0; AimFrame = Frame; }
		const NiAVObject* object = pass->geometry;
		if (!object->m_kWorldBound) return;
		const NiBound& bound = *object->m_kWorldBound;
		const D3DXMATRIX& camera = TheRenderManager->InvViewMatrix;
		D3DXVECTOR3 forward(camera._31, camera._32, camera._33);
		D3DXVec3Normalize(&forward, &forward);
		const D3DXVECTOR3 v(bound.Center.x - TheRenderManager->CameraPosition.x, bound.Center.y - TheRenderManager->CameraPosition.y,
			bound.Center.z - TheRenderManager->CameraPosition.z);
		const float along = D3DXVec3Dot(&v, &forward);
		const float off2 = D3DXVec3LengthSq(&v) - along * along;
		if (along < -bound.Radius || off2 > bound.Radius * bound.Radius) return;
		int i = 0;
		while (i < AimHitCount && AimHits[i].geometry != object) i++;
		if (i == AimHitCount) {
			if (AimHitCount == 10) return;
			AimHits[i] = {};
			AimHits[i].geometry = object; AimHits[i].along = along; AimHits[i].off = sqrtf((std::max)(off2, 0.0f)); AimHits[i].radius = bound.Radius;
			AimHitCount++;
		}
		AimHit& h = AimHits[i];
		if (h.passes == 4) return;
		AimPass& a = h.pass[h.passes++];
		a = {};
		a.vs = LastVertexShaderName; a.ps = LastPixelShaderName; a.vsNvr = LastVertexShaderNvr; a.psNvr = LastPixelShaderNvr;
		a.shadowed = LastPixelShaderShadowed && Bound;
		a.passEnum = pass->passEnum; a.numLights = pass->numLights; a.strength = Uploaded[6][0] > 0.0f;
		const int lights = pass->lights ? (std::min)((int)pass->numLights, 6) : 0;
		for (int k = 0; k < lights; k++) {
			if (!pass->lights[k]) continue;
			a.index[a.count] = (UInt8)k; a.point[a.count] = pass->lights[k]->bPointLight != 0;
			const int j = k - FirstShaderLight(pass);	// the shader's light (c174+j)
			a.uploaded[a.count] = j >= 0 && j < 6 && Uploaded[j][3] > 0.0f;	// as uploaded for this draw (SetForPass)
			a.lamps[a.count++] = pass->lights[k];
		}
	}

	// ObjectLamps: after each object's draw in RenderPassImmediately_Standard/_Skinned (the run's pixel shader is known by
	// then: SetForDraw ran for it, or for the run it belongs to).
	inline void NoteDraw(const RenderPassView* pass, bool world) {
		if (!LogObjectLamps || !world || !pass || !pass->geometry) return;
		NoteAim(pass);
		if (!pass->lights) return;
		ObjectLampState& o = ObjectLamps[pass->geometry];
		if (o.frame != Frame) {
			if (o.frame && o.lastFrame && o.frame == o.lastFrame + 1) CompareObjectLamps(pass->geometry, o);
			memcpy(o.last, o.lamps, sizeof(o.lamps)); memcpy(o.lastHow, o.how, sizeof(o.how)); memcpy(o.lastShader, o.shader, sizeof(o.shader));
			o.lastCount = o.count;
			o.lastFrame = o.frame;
			o.count = 0;
			o.frame = Frame;
		}
		const UInt8 how = LastPixelShaderShadowed ? 1 : 2;
		const int lights = (std::min)((int)pass->numLights, 12);
		for (int k = 0; k < lights; k++) {
			const ShadowSceneLight* light = pass->lights[k];
			if (!light || !light->bPointLight) continue;
			int i = 0;
			while (i < o.count && o.lamps[i] != light) i++;
			if (i == o.count) {
				if (o.count == 12) continue;
				o.lamps[i] = light; o.how[i] = 0; o.shader[i] = nullptr; o.count++;
			}
			o.how[i] |= how;
			if (!o.shader[i] || how == 2) o.shader[i] = LastPixelShaderName;	// a pass without shadows names its shader
		}
	}

	// Once a frame (RenderShadowMaps), with the F10 profile: how much work it did.
	inline void FrameStats() {
		if (!GpuTimer::Enabled || !CompiledIn) return;
		if (++StatFrames >= 240) {
			const float perFrame = 1.0f / StatFrames;
			Logger::Log("POINT SHADOWS FORWARD %d shaders compiled in: %.1f cube maps in use, %.0f objects, %.0f with a shadowed lamp, %.1f lamps matched per frame",
				CompiledCount, (float)SlotCount, StatDraws * perFrame, StatMatchedDraws * perFrame, StatMatchedLights * perFrame);
			Logger::Log("POINT SHADOWS KeepLampsInView %s: per frame %.1f lamp view tests by the game, %.1f lamps it called wholly out of view, %.1f of those kept active (their light reaches the view)",
				KeepLampsNow ? "on" : "off", StatCullCalls * perFrame, StatCullOut * perFrame, StatCullKept * perFrame);
			StatCullCalls = StatCullOut = StatCullKept = 0;
			if (LogObjectLamps)
				Logger::Log("POINT SHADOWS OBJECT LAMPS (LogLamps): per frame %.2f lamps came onto objects, %.2f left them, %.2f lost their shadow (now in a pass without), %.2f got it; %.2f of these with the player standing still",
					StatLampAppeared * perFrame, StatLampGone * perFrame, StatLampLostShadow * perFrame, StatLampGotShadow * perFrame, StatLampStill * perFrame);
			StatLampAppeared = StatLampGone = StatLampLostShadow = StatLampGotShadow = StatLampStill = 0;
			StatFrames = StatDraws = StatMatchedDraws = StatMatchedLights = 0;
		}
	}
}
