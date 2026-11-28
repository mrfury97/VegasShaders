// Character/viewmodel shadow maps and world reflection cubemap pass (extracted from ShadowManager.cpp).
#include "SkinBones.h"	// 2026-11-18: a skin's bone slots may be stale
#include "ClothWind.h"	// UNOFFICIAL 2026-11-21: sync c248 tracking across ViewModelShadowMap constant restore
#include "SunFadeDeferred.h"	// UNOFFICIAL 2026-11-21: static/dynamic split during DeferredFade
#include "ShadowFaceCull.h"
#include "PortalVisibility.h"	// UNOFFICIAL 2026-11-24: skip occluded-room refs in interior reflections and spot/fallback shadows
#include "../effects/Puddles/PuddleMap.h"	// 2026-11-12 Puddles map mode
#include "PointShadowSchedule.h"
#include "PointShadowForward.h"
#include "BeatSeries.h"	// UNOFFICIAL 2026-11-11 the beat detector's per-frame series
#include "TestTools.h"	// UNOFFICIAL: NVR_TEST_TOOLS (SkipWhenFadedCheck)
#include "OrthoProbe.h"	// UNOFFICIAL 2026-10-17 test tools: ortho map caster dump
#include "SunShadowStats.h"	// UNOFFICIAL 2026-10-21 test tools: where the sun cascades' CPU time goes
#include "../effects/CoverMap/CoverMap.h"	// UNOFFICIAL 2026-10-27 Persistent wetness and snow: CoverMapOrthoFrame
#include "DisplaceShadows.h"	// UNOFFICIAL 2026-10-27 displacement shadows: the land drawn with its bumps into the sun cascades
#include "ShadowAtlas.h"	// UNOFFICIAL 2026-10-30 ShadowAtlas (INTERIOR-SHADOWS-MODERN step 1)
#include "DeferredLamps.h"	// UNOFFICIAL 2026-11-23 deferred exterior night lamp shadows via ShadowAtlas
#include "CapsuleShadows.h"	// UNOFFICIAL 2026-10-31 CapsuleShadows (steps 2-3): DropActors
#include "PipboyFit.h"	// UNOFFICIAL 2026-11-04 ViewModelShadows: its object cleared before the viewmodel map
#include "NpcCharacterMaps.h"	// UNOFFICIAL 2026-11-08 CharacterShadowMapNpcs
#include "ViewModelLight.h"	// UNOFFICIAL 2026-11-15 DeferredFirstPerson: the view model map's light transform for the first-person deferred sun pass
#include "GpuProfiler.h"
#include "Paths.h"
#include "ShadowManagerInternal.h"
using namespace ShadowManagerInternal;

// UNOFFICIAL 2026-10-23 CharacterShadowMap (docs/next/CHARACTER-SHADOW-MAP-DESIGN.md). A small sun shadow map (R32F, plain depth
// whatever the cascades' Mode: TESR_ShadowData.z = 1 makes ShadowMap.pso write depth, as for the ortho map) fitted round the player
// (ShadowsExteriorEffect::GetCharacterViewProj), drawn every frame the cascades are, with the cascade passes' shaders and states
// (BeginShadowMap / DrawShadowMap) but its own walk: the player's node alone (and, with CharacterShadowMapOthers, the other actors of
// the loaded cells whose bound touches the box), not the loaded cells. The lookups (Includes/Shadow.hlsl CharacterMapShadow,
// SunShadows.fx.hlsl) take the darker of the cascades' shadow and this map's where a pixel is inside the box, fading to the cascades
// alone over the box's outer band; so the cascades keep drawing the player as before (the player's shadow on the ground, and the
// shadows of walls and trees on the player, stay theirs), and this map only adds the fine self-shadow the cascades' texels lose.
// Off, or not compiled in, or in first person: nothing here, CharacterData.w stays 0, every shader skips the lookup.
// 2026-11-08: the nodes of the actors in the game's process lists (high and middle-high, as CapsuleShadows reads them), the player
// left out. Its own function: the __try needs a frame without C++ objects to unwind.
static int CharacterMapActorNodes(NiNode** out, int max) {
	int n = 0;
	__try {
		UInt8* lists = (UInt8*)0x11E0E80;	// ProcessLists
		Actor** data = *(Actor***)(lists + 0x08);
		for (int level = 0; data && level < 2; level++) {
			const UInt32 begin = *(UInt32*)(lists + 0x14 + level * 4), end = *(UInt32*)(lists + 0x24 + level * 4);
			if (end < begin || end - begin > 4096) continue;
			for (UInt32 i = begin; i < end && n < max; i++) {
				Actor* actor = data[i];
				if (!actor || !actor->IsActor() || (TESObjectREFR*)actor == (TESObjectREFR*)Player || (actor->flags & 0x800) != 0) continue;	// 0x800: disabled
				if (NiNode* node = actor->GetNode()) out[n++] = node;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		static bool logged = false;
		if (!logged) { logged = true; Logger::Log("[ERROR] CharacterShadowMapOthers: reading the actors failed (once)"); }
	}
	return n;
}

void ShadowManager::RenderCharacterShadowMap(const D3DXVECTOR3& SunDir) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!Shadows->Settings.ShadowMaps.CharacterMap || !Shadows->CharacterMapCompiled) return;
	if (!Shadows->EnsureCharacterMap()) return;
	D3DXMATRIX viewProj;
	if (!Shadows->GetCharacterViewProj(SunDir, viewProj)) return;

	IDirect3DDevice9* Device = TheRenderManager->device;
	static GpuTimer characterMapTimer("Character shadow map");
	static CpuTimer characterMapCpuTimer("Character shadow map (CPU)");
	CpuProfileScope cpu(characterMapCpuTimer);
	GpuProfileScope gpu(characterMapTimer, Device);

	ShadowsExteriorEffect::ShadowMapSettings* map = &Shadows->CharacterMap;
	D3DXVECTOR4* ShadowData = &Shadows->Constants.Data;
	const float zWas = ShadowData->z;
	ShadowData->z = 1.0f;	// plain depth (as the ortho map), whatever the cascades' Mode
	Shadows->Constants.ShadowViewProj = viewProj;
	Device->SetRenderTarget(0, Shadows->CharacterMapSurface);
	Device->SetDepthStencilSurface(Shadows->CharacterMapDepthSurface);

	BeginShadowMap(map, &viewProj);
	// Walk the whole player node without per-child frustum popping so swinging arms/melee weapons do not
	// flicker in and out of the character shadow map mid-animation; skip non-skinned blend-only FX meshes.
	SkipNonSkinnedBlendOnly = true;
	NiNode* playerNode = Shadows->CharacterTargetNode ? Shadows->CharacterTargetNode : Player->GetNode();
	AccumChildren(playerNode, &map->Forms, false, false, nullptr);
	unsigned others = 0;
	// 2026-11-08 (tester: "CharacterShadowMapOthers does nothing"): the actors come from the game's process lists (high and
	// middle-high, as CapsuleShadows and BloodFlow read them), not the loaded cells' object lists: most people are persistent
	// references that live in the worldspace's persistent cell, so the cell walk found almost nobody.
	if (Shadows->Settings.ShadowMaps.CharacterMapOthers && Player->GetWorldSpace()) {
		NiNode* nodes[64];
		const int count = CharacterMapActorNodes(nodes, 64);
		for (int i = 0; i < count; i++) {
			NiNode* node = nodes[i];
			if (node == playerNode || (node->m_flags & NiAVObject::APP_CULLED) || !node->WithinFrustum(&map->ShadowMapFrustumPlanes)) continue;
			AccumChildren(node, &map->Forms, false, false, &map->ShadowMapFrustumPlanes);
			others++;
		}
	}
	SkipNonSkinnedBlendOnly = false;
	const unsigned casters = (unsigned)(geometryPass->GeometryList.size() + alphaPass->GeometryList.size() + skinnedGeoPass->GeometryList.size());
	DrawShadowMap(map);
	ShadowData->z = zWas;
	Shadows->Constants.CharacterData.w = 1.0f + (float)Shadows->Settings.ShadowMaps.CharacterMapDebug;	// the map holds this frame's draw: the lookups read it (2-4: a debug view, Shadow.hlsl)

	if (Shadows->Settings.ShadowMaps.CharacterMapLog) {
		static DWORD lastLog = 0;
		if (GetTickCount() - lastLog > 1000) {
			lastLog = GetTickCount();
			const float radius = Shadows->Constants.CharacterCenter.w;
			Logger::Log("CharacterShadowMap: fit %d, half-extent %.0f units (texel %.3f units = %.2f cm), %u casters (%u other actors), sun %.3f %.3f %.3f, bias %.5f, band %.3f",
				Shadows->Settings.ShadowMaps.CharacterMapFit, radius, 2.0f * radius / map->ShadowMapResolution, 2.0f * radius / map->ShadowMapResolution * 1.428f,
				casters, others, SunDir.x, SunDir.y, SunDir.z, Shadows->Constants.CharacterData.z, Shadows->Constants.CharacterData.y);
		}
	}
}

// UNOFFICIAL 2026-11-08 CharacterShadowMapNpcs ([Shaders.ShadowsExteriors.ShadowMaps], 0 = off; user: NPC faces look "crusty" up close).
// The player's map gives only the player (or, with CharacterShadowMapTarget, one NPC in first person) the fine sun self-shadow; every
// other person keeps the cascades' 4-10 mm texels. Here the nearest CharacterShadowMapNpcs people whose head is within
// CharacterShadowMapNpcDistance of the camera each get a map of their own, fitted and drawn exactly as the player's (FitCharacterBox,
// BeginShadowMap / DrawShadowMap, that person's node alone, plus the other people in its box with CharacterShadowMapOthers). The
// shaders are unchanged: each NPC's own draws are given that NPC's map on s11 and its constants in c206-c214 (NpcCharacterMaps.h),
// every other draw keeps the player's. Third and first person alike; nothing while the sun cascades are not drawn.
void ShadowManager::RenderNpcCharacterMaps(const D3DXVECTOR3& SunDir) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	Shadows->CharacterNpcLive = 0;
	const int wanted = Shadows->CharacterMapCompiled && Shadows->Settings.ShadowMaps.CharacterMap ?
		std::clamp(TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapNpcs"), 0, (int)ShadowsExteriorEffect::CharacterNpcMax) : 0;
	const int resolution = std::clamp(TheSettingManager->GetSettingI("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapNpcResolution"), 256, 4096);
	if (!Shadows->EnsureCharacterNpcMaps(wanted, (UINT)((resolution + 128) / 256 * 256))) return;
	Character* npcs[ShadowsExteriorEffect::CharacterNpcMax];
	const float distance = max(TheSettingManager->GetSettingF("Shaders.ShadowsExteriors.ShadowMaps", "CharacterShadowMapNpcDistance"), 0.0f);
	const int count = Shadows->PickCharacterNpcs(npcs, wanted, distance, Shadows->CharacterTargetNode);
	if (count == 0) return;

	IDirect3DDevice9* Device = TheRenderManager->device;
	NpcCharacterMaps::Reset(Device);	// no NPC map stays bound while it is drawn into
	static GpuTimer npcMapsTimer("Character shadow maps (NPCs)");
	static CpuTimer npcMapsCpuTimer("Character shadow maps (NPCs, CPU)");
	CpuProfileScope cpu(npcMapsCpuTimer);
	GpuProfileScope gpu(npcMapsTimer, Device);

	ShadowsExteriorEffect::ShadowMapSettings* map = &Shadows->CharacterNpcMap;
	D3DXVECTOR4* ShadowData = &Shadows->Constants.Data;
	const float zWas = ShadowData->z;
	const D3DXMATRIX viewProjWas = Shadows->Constants.ShadowViewProj;
	ShadowData->z = 1.0f;	// plain depth, as the player's map
	NiNode* others[64];
	const int otherCount = Shadows->Settings.ShadowMaps.CharacterMapOthers && Player->GetWorldSpace() ? CharacterMapActorNodes(others, 64) : 0;
	NiNode* playerNode = Player->GetNode();
	int live = 0;
	for (int t = 0; t < count; t++) {
		ShadowsExteriorEffect::CharacterNpcTile& tile = Shadows->CharacterNpcTiles[live];
		NiNode* node = npcs[t]->GetNode();
		D3DXMATRIX viewProj;
		if (!node || !Shadows->FitCharacterBox(npcs[t], node, SunDir, map->ShadowMapResolution, *map, viewProj, tile.center, tile.data)) continue;
		tile.node = node;
		tile.toLight = viewProj;
		tile.data.w = 1.0f + (float)Shadows->Settings.ShadowMaps.CharacterMapDebug;
		Shadows->Constants.ShadowViewProj = viewProj;
		Device->SetRenderTarget(0, Shadows->CharacterNpcSurface[live]);
		Device->SetDepthStencilSurface(Shadows->CharacterNpcDepthSurface);
		BeginShadowMap(map, &viewProj);
		SkipNonSkinnedBlendOnly = true;
		AccumChildren(node, &map->Forms, false, false, nullptr);
		// CharacterShadowMapOthers: the people (the player too) standing in this box shade this NPC sharply as well
		if (otherCount) {
			if (playerNode && playerNode != node && Player->IsDrawnThirdPerson() && !(playerNode->m_flags & NiAVObject::APP_CULLED) && playerNode->WithinFrustum(&map->ShadowMapFrustumPlanes))
				AccumChildren(playerNode, &map->Forms, false, false, &map->ShadowMapFrustumPlanes);
			for (int i = 0; i < otherCount; i++) {
				NiNode* other = others[i];
				if (other == node || (other->m_flags & NiAVObject::APP_CULLED) || !other->WithinFrustum(&map->ShadowMapFrustumPlanes)) continue;
				AccumChildren(other, &map->Forms, false, false, &map->ShadowMapFrustumPlanes);
			}
		}
		SkipNonSkinnedBlendOnly = false;
		DrawShadowMap(map);
		live++;
	}
	ShadowData->z = zWas;
	Shadows->Constants.ShadowViewProj = viewProjWas;
	Shadows->CharacterNpcLive = live;

	if (Shadows->Settings.ShadowMaps.CharacterMapLog) {
		static DWORD lastLog = 0;
		if (GetTickCount() - lastLog > 1000) {
			lastLog = GetTickCount();
			Logger::Log("CharacterShadowMapNpcs: %d of %d NPC maps drawn (%u x %u, within %.0f units), first half-extent %.0f", live, count,
				Shadows->CharacterNpcSizeCreated, Shadows->CharacterNpcSizeCreated, distance, live ? Shadows->CharacterNpcTiles[0].center.w : 0.0f);
		}
	}
}

// UNOFFICIAL 2026-11-04 ViewModelShadows (from Physically-Based-Reloaded f8b6114a): the first-person arms and weapon casting their sun
// shadows on themselves. The sun cascades never hold the viewmodel (its own scene graph, posed for the frame only as its render starts),
// and in first person the character map has nothing to do (or, with CharacterShadowMapTarget, an NPC's draws are done with it by then).
// So just before the game's first-person render (Render.cpp RenderFirstPersonHook) the viewmodel alone is drawn into the character map
// with its own light box, an ortho box fitted round its parts in the sun's frame, and the first-person draws read it through
// CharacterMapShadow with TESR_CharShadowData.w = 0.5 (a 6 x 6 tent PCF there, Includes/Shadow.hlsl); the caller uploads the constants
// and puts them back after that render. The game draws the viewmodel moved to the origin (0x874C10) and camera-relative to the camera
// drawing it (NiRenderer::kPosAdjust, set by Do_SetCameraData), here still the world camera's: for this draw both kPosAdjust and NVR's
// camera position are the first-person camera's, so the map holds what the first-person draw sees and the bone matrices computed now
// are the ones that draw uses (made with the world camera's, the arms were drawn far off). Called inside the game's scene: the targets,
// viewport, render states, shaders, buffers, textures and shader constants the draws change are put back after. False when nothing was
// drawn (the Character* constants are then unchanged).

// The viewmodel's parts as spheres relative to Eye, for the light box: skinned meshes by the bound NiSkinData keeps for each bone's
// vertices (in that bone's space), rigid ones by their model bound. Walked as AccumChildren walks it; anything far from the camera (the
// first-person skeleton's lower body) is left out.
// UNOFFICIAL 2026-11-04 CharacterShadowMapLog: this frame's viewmodel map as drawn, for ViewModelLogDraw (the first-person draws read it).
static struct {
	bool valid = false;
	UInt32 frame = 0;
	NiPoint3 eye = {};
	D3DXMATRIX lightView;
	float cx = 0.0f, cy = 0.0f, half = 0.0f, zNear = 0.0f, zFar = 0.0f;
	std::unordered_map<const void*, NiPoint3> posAtMap;	// each geometry's world position when the map was drawn
	int lines = 0;
} ViewModelLog;

static void ViewModelLogPositions(NiAVObject* Object) {
	if (!Object) return;
	if (Object->IsGeometry()) { ViewModelLog.posAtMap[Object] = Object->m_worldTransform.pos; return; }
	if (NiNode* Node = Object->IsNiNode()) for (int i = 0; i < Node->m_children.end; i++) ViewModelLogPositions(Node->m_children.data[i]);
}

static void ViewModelSpheres(NiAVObject* Object, const NiPoint3& Eye, std::vector<D3DXVECTOR4>& Spheres);

// Render.cpp ForwardShadowsForPass, every first-person pass: how far the part drawn sits outside the map's box (in the sun's frame, world
// units; 0 inside), whether it moved since the map was drawn, and the character-map switch the shaders read (w: 0.5 the viewmodel map).
void ShadowManager::ViewModelLogDraw(NiGeometry* Geometry) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!Geometry || !Shadows || !Shadows->Settings.ShadowMaps.CharacterMapLog || ViewModelLog.lines >= 4000) return;
	const UInt32 frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u;
	// Only the draws that could lose the shadow are written, and a summary every 120 frames (all draws would fill the log in two seconds).
	static UInt32 summaryFrame = 0, draws = 0, flagged = 0;
	draws++;
	if (frame >= summaryFrame + 120) {
		ViewModelLog.lines++;
		Logger::Log("VIEWMODEL DRAW f%u: summary: %u first-person draws since f%u, %u flagged", frame, draws, summaryFrame, flagged);
		summaryFrame = frame; draws = 0; flagged = 0;
	}
	if (!ViewModelLog.valid || ViewModelLog.frame != frame) {
		flagged++; ViewModelLog.lines++;
		Logger::Log("VIEWMODEL DRAW f%u: '%s' with no map this frame (last map f%u) | shaders read w %.2f", frame, Geometry->m_pcName ? Geometry->m_pcName : "?",
			ViewModelLog.frame, Shadows->Constants.CharacterData.w);
		return;
	}
	static std::vector<D3DXVECTOR4> spheres;
	spheres.clear();
	ViewModelSpheres(Geometry, ViewModelLog.eye, spheres);
	float over = 0.0f;
	for (const D3DXVECTOR4& s : spheres) {
		D3DXVECTOR3 v;
		D3DXVec3TransformCoord(&v, (const D3DXVECTOR3*)&s, &ViewModelLog.lightView);
		over = max(over, fabsf(v.x - ViewModelLog.cx) + s.w - ViewModelLog.half);
		over = max(over, fabsf(v.y - ViewModelLog.cy) + s.w - ViewModelLog.half);
		over = max(over, max(-v.z + s.w - ViewModelLog.zFar, ViewModelLog.zNear - (-v.z - s.w)));
	}
	auto at = ViewModelLog.posAtMap.find(Geometry);
	const NiPoint3& p = Geometry->m_worldTransform.pos;
	const float moved = at == ViewModelLog.posAtMap.end() ? -1.0f :
		sqrtf((p.x - at->second.x) * (p.x - at->second.x) + (p.y - at->second.y) * (p.y - at->second.y) + (p.z - at->second.z) * (p.z - at->second.z));
	const float w = Shadows->Constants.CharacterData.w;
	if (over <= 0.1f && moved >= 0.0f && moved <= 0.05f && w > 0.49f && w < 0.51f) return;	// (0.1: a texel's snap of the box centre)
	flagged++; ViewModelLog.lines++;
	Logger::Log("VIEWMODEL DRAW f%u: '%s' %u spheres | outside the box by %.2f | moved %.2f since the map (-1: not in it) | shaders read w %.2f",
		frame, Geometry->m_pcName ? Geometry->m_pcName : "?", (unsigned)spheres.size(), max(over, 0.0f), moved, w);
}

static void ViewModelSpheres(NiAVObject* Object, const NiPoint3& Eye, std::vector<D3DXVECTOR4>& Spheres) {
	if (!Object || Object->m_flags & NiAVObject::NiFlags::APP_CULLED) return;
	auto Add = [&](const NiTransform& T, const NiBound& B) {
		const NiPoint3 c = T.rot * B.Center;
		const D3DXVECTOR4 s(T.pos.x + c.x * T.scale - Eye.x, T.pos.y + c.y * T.scale - Eye.y, T.pos.z + c.z * T.scale - Eye.z, B.Radius * T.scale);
		if (s.x * s.x + s.y * s.y + s.z * s.z < 150.0f * 150.0f) Spheres.push_back(s);
	};
	if (Object->IsGeometry()) {
		NiGeometry* Geo = static_cast<NiGeometry*>(Object);
		NiSkinInstance* Skin = Geo->skinInstance;
		if (Skin && Skin->SkinData && Skin->SkinData->BoneData && Skin->BoneObjects) {
			for (UInt32 i = 0; i < Skin->SkinData->Bones; i++)
				if (SkinBones::Live(Skin->BoneObjects[i]) && Skin->SkinData->BoneData[i].Verts) Add(Skin->BoneObjects[i]->m_worldTransform, Skin->SkinData->BoneData[i].Bound);
		}
		else if (Geo->geomData) Add(Geo->m_worldTransform, Geo->geomData->m_kBound);
		return;
	}
	NiNode* Node = Object->IsNiNode();
	if (!Node) return;
	if (Node->IsKindOf<NiSwitchNode>()) {
		NiSwitchNode* SwitchNode = static_cast<NiSwitchNode*>(Node);
		if (SwitchNode->m_iIndex >= 0 && SwitchNode->m_iIndex < Node->m_children.end) ViewModelSpheres(Node->m_children.data[SwitchNode->m_iIndex], Eye, Spheres);
		return;
	}
	for (int i = 0; i < Node->m_children.end; i++) ViewModelSpheres(Node->m_children.data[i], Eye, Spheres);
}

bool ShadowManager::RenderViewModelShadowMap(NiCamera* FirstPersonCamera) {
	ShadowsExteriorEffect* Shadows = TheShaderManager->Effects.ShadowsExteriors;
	if (!Shadows || !ShadowShadersLoaded || !TheSettingManager->SettingsMain.Main.RenderEffects) return false;
	// UNOFFICIAL 2026-11-04 [Shaders.ShadowsExteriors.ShadowMaps] CharacterShadowMapLog: each first-person frame, made or why not, and where
	// (user: the arms' and weapon's shadows flicker in and out on a jump). At most 4000 lines a session.
	static int viewModelLogLines = 0;
	const bool logThis = Shadows->Settings.ShadowMaps.CharacterMapLog && viewModelLogLines < 4000;
	auto skip = [&](const char* why) { if (logThis) { viewModelLogLines++; Logger::Log("VIEWMODEL SHADOW f%u: skipped (%s)", TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u, why); } return false; };
	if (!Shadows->Settings.ShadowMaps.CharacterMap || !Shadows->CharacterMapCompiled || !Shadows->EnsureCharacterMap()) return skip("character map off or not made");
	if (!TheShaderManager->GameState.isExterior || !Shadows->Enabled || !Shadows->Settings.Exteriors.Enabled) return skip("indoors or exterior shadows off");
	if (Shadows->Constants.ShadowFade.y <= 0.0f || Shadows->Constants.ForwardData.x != 0.0f) return skip(Shadows->Constants.ShadowFade.y <= 0.0f ? "sun shadow maps inactive" : "forward sun shadows off this frame");
	if (!Player || !Player->firstPersonNiNode || !FirstPersonCamera) return skip("no first-person model or camera");
	// UNOFFICIAL 2026-11-05 (from Bale, PR #6): no map while the weapon is holstered, nothing of the viewmodel is drawn for it to shadow (the world's
	// shadows dropped out in first person then).
	if (!Player->process || !Player->process->IsWeaponOut()) return skip("weapon holstered: no viewmodel on screen");

	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	static GpuTimer viewModelMapTimer("View model shadow map");
	GpuProfileScope gpu(viewModelMapTimer, Device);
	IDirect3DSurface9* DepthSurface = NULL;
	IDirect3DSurface9* RenderSurface = NULL;
	D3DVIEWPORT9 viewport;
	Device->GetDepthStencilSurface(&DepthSurface);
	Device->GetRenderTarget(0, &RenderSurface);
	Device->GetViewport(&viewport);
	static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_ALPHABLENDENABLE,
		D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILREF,
		D3DRS_STENCILFUNC, D3DRS_COLORWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_NORMALIZENORMALS, D3DRS_POINTSIZE };
	DWORD saved[_countof(kStates)];
	for (int i = 0; i < _countof(kStates); i++) Device->GetRenderState(kStates[i], &saved[i]);
	// The rest of the device state the draws below change, as the flashlight's MaterialPass keeps it (in the middle of the game's own
	// draws): shaders, vertex layout, buffers, textures and their sampling, and every shader constant (the skinned pass writes bones
	// from c9, the game sets some of its constants once per pass).
	static const D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
	IDirect3DVertexShader9* savedVS = NULL; IDirect3DPixelShader9* savedPS = NULL; IDirect3DVertexDeclaration9* savedDecl = NULL;
	IDirect3DIndexBuffer9* savedIB = NULL; DWORD savedFVF = 0;
	IDirect3DVertexBuffer9* savedVB[8] = {}; UINT savedOffset[8] = {}, savedStride[8] = {};
	IDirect3DBaseTexture9* savedTex[16] = {}; DWORD savedSampler[16][_countof(kSamplerStates)];
	static float savedVSConst[256][4], savedPSConst[224][4];
	Device->GetVertexShader(&savedVS); Device->GetPixelShader(&savedPS); Device->GetVertexDeclaration(&savedDecl);
	Device->GetIndices(&savedIB); Device->GetFVF(&savedFVF);
	for (UINT i = 0; i < 8; i++) Device->GetStreamSource(i, &savedVB[i], &savedOffset[i], &savedStride[i]);
	for (DWORD i = 0; i < 16; i++) {
		Device->GetTexture(i, &savedTex[i]);
		for (int s = 0; s < _countof(kSamplerStates); s++) Device->GetSamplerState(i, kSamplerStates[s], &savedSampler[i][s]);
	}
	Device->GetVertexShaderConstantF(0, &savedVSConst[0][0], 256);
	Device->GetPixelShaderConstantF(0, &savedPSConst[0][0], 224);
	const bool savedClothNonZero = ClothWind::NonZeroSent, savedClothGrass = ClothWind::GrassInC248;
	const D3DXVECTOR4 savedData = Shadows->Constants.Data;
	const D3DXMATRIX savedViewProj = Shadows->Constants.ShadowViewProj;
	ShadowsExteriorEffect::ShadowMapSettings& map = Shadows->CharacterMap;
	const float res = map.ShadowMapResolution;

	// Camera-relative to the first-person camera (see above), put back at the end.
	NiPoint3* PosAdjust = (NiPoint3*)0x011F474C;	// NiRenderer::kPosAdjust
	const NiPoint3 savedPosAdjust = *PosAdjust;
	const D3DXVECTOR4 savedCameraPosition = TheRenderManager->CameraPosition;
	const NiPoint3 eye = FirstPersonCamera->m_worldTransform.pos;
	*PosAdjust = eye;
	TheRenderManager->CameraPosition = D3DXVECTOR4(eye.x, eye.y, eye.z, 1.0f);

	// The light transform: an ortho box fitted, in the sun's frame, round the parts drawn (ViewModelSpheres; the viewmodel's root bound
	// has no size), so the map's texels cover only the arms and weapon. Square, its size in 8-unit steps and its centre snapped to whole
	// texels: the texel grid holds still while the weapon sways and the shadow edges do not crawl. At least as deep as it is wide
	// (CharacterMapShadow's receiver-plane slack takes one texel of depth as one texel across).
	static std::vector<D3DXVECTOR4> spheres;
	spheres.clear();
	ViewModelSpheres(Player->firstPersonNiNode, eye, spheres);
	D3DXVECTOR3 sunDir(Shadows->Constants.SmoothedSunDir.x, Shadows->Constants.SmoothedSunDir.y, Shadows->Constants.SmoothedSunDir.z);
	D3DXVec3Normalize(&sunDir, &sunDir);
	const D3DXVECTOR3 origin(0.0f, 0.0f, 0.0f);
	const D3DXVECTOR3 upDir = fabsf(sunDir.z) > 0.99f ? D3DXVECTOR3(0.0f, 1.0f, 0.0f) : D3DXVECTOR3(0.0f, 0.0f, 1.0f);
	D3DXMATRIX lightView, lightProj;
	D3DXMatrixLookAtRH(&lightView, &sunDir, &origin, &upDir);
	D3DXVECTOR3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (const D3DXVECTOR4& s : spheres) {
		D3DXVECTOR3 v;
		D3DXVec3TransformCoord(&v, (const D3DXVECTOR3*)&s, &lightView);
		lo.x = min(lo.x, v.x - s.w); lo.y = min(lo.y, v.y - s.w); lo.z = min(lo.z, v.z - s.w);
		hi.x = max(hi.x, v.x + s.w); hi.y = max(hi.y, v.y + s.w); hi.z = max(hi.z, v.z + s.w);
	}
	if (spheres.empty()) { lo = D3DXVECTOR3(-1.0f, -1.0f, -1.0f); hi = D3DXVECTOR3(1.0f, 1.0f, 1.0f); }
	const float extent = max(8.0f, ceilf(max(hi.x - lo.x, hi.y - lo.y) / 8.0f) * 8.0f);
	const float texelWorld = extent / res;
	const float cx = floorf((lo.x + hi.x) * 0.5f / texelWorld) * texelWorld;
	const float cy = floorf((lo.y + hi.y) * 0.5f / texelWorld) * texelWorld;
	float zNear = -hi.z - 1.0f, zFar = -lo.z + 1.0f;	// right-handed: in front is -z
	if (zFar - zNear < extent) { const float grow = 0.5f * (extent - (zFar - zNear)); zNear -= grow; zFar += grow; }
	D3DXMatrixOrthoOffCenterRH(&lightProj, cx - 0.5f * extent, cx + 0.5f * extent, cy - 0.5f * extent, cy + 0.5f * extent, zNear, zFar);
	D3DXMATRIX viewProj = lightView * lightProj;
	ViewModelLight::ViewProj = viewProj;	// UNOFFICIAL 2026-11-15 DeferredFirstPerson (core/ViewModelLight.h): the true camera-relative point's transform
	ViewModelLight::Frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u;

	// RenderFirstPersonHook and ViewModelMotion::DoSetCameraDataHook swap TESR_InvProjectionTransform (fpInvProj) and
	// TESR_InvViewTransform (fpInvView, zero-translation in FirstPersonCamera axes) for the entire first-person pass, so
	// GetShadowWorldPos already recovers true first-person camera-relative positions (origin = FirstPersonCamera->m_worldTransform.pos,
	// axes = FirstPersonCamera->m_worldTransform.rot) matching ViewModelLight::ViewProj without needing any FOV rescale or view undo.
	Shadows->Constants.CharacterCameraToLight = viewProj;
	Shadows->Constants.CharacterCenter = D3DXVECTOR4(cx, cy, 0.0f, 0.5f * extent);
	Shadows->Constants.CharacterData = D3DXVECTOR4(1.0f / res, 0.01f, max(2.0f * texelWorld, 0.03f) / (zFar - zNear), 
		Shadows->Settings.ShadowMaps.CharacterMapDebug ? 1.0f + (float)Shadows->Settings.ShadowMaps.CharacterMapDebug : 0.5f);	// (CharacterShadowMapDebug: its views, Shadow.hlsl)

	Shadows->Constants.Data.z = 1.0f;	// plain depth (as RenderCharacterShadowMap)
	Shadows->Constants.ShadowViewProj = viewProj;
	if (Shadows->Settings.ShadowMaps.CharacterMapLog) {	// for ViewModelLogDraw
		ViewModelLog.valid = true;
		ViewModelLog.frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u;
		ViewModelLog.eye = eye;
		ViewModelLog.lightView = lightView;
		ViewModelLog.cx = cx; ViewModelLog.cy = cy; ViewModelLog.half = 0.5f * extent;
		ViewModelLog.zNear = zNear; ViewModelLog.zFar = zFar;
		ViewModelLog.posAtMap.clear();
		ViewModelLogPositions(Player->firstPersonNiNode);
	}
	if (logThis) {
		const D3DXMATRIX& invView = TheRenderManager->InvViewMatrix;
		viewModelLogLines++;
		Logger::Log("VIEWMODEL SHADOW f%u: made | eye %.1f %.1f %.1f | world camera %.1f %.1f %.1f (eye - world %.2f %.2f %.2f) | inv view t %.2f %.2f %.2f | "
			"%u parts, box centre %.2f %.2f, extent %.1f, depth %.1f..%.1f | sun %.3f %.3f %.3f",
			TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0u, eye.x, eye.y, eye.z, savedCameraPosition.x, savedCameraPosition.y, savedCameraPosition.z,
			eye.x - savedCameraPosition.x, eye.y - savedCameraPosition.y, eye.z - savedCameraPosition.z, invView._41, invView._42, invView._43,
			(unsigned)spheres.size(), cx, cy, extent, zNear, zFar, sunDir.x, sunDir.y, sunDir.z);
	}
	geometryPass->VertexShader = alphaPass->VertexShader = skinnedGeoPass->VertexShader = ShadowMapVertex;
	geometryPass->PixelShader = ShadowMapOpaquePixel;
	alphaPass->PixelShader = ShadowMapPixel;
	skinnedGeoPass->PixelShader = ShadowMapOpaquePixel;
	skinnedGeoPass->AlphaPixelShader = SkinnedGeoShadowRenderPass::AlphaTest ? ShadowMapPixel : nullptr;

	// Any other targets the game's scene left bound (multiple render targets) would not match the map's size, which drops every draw;
	// put back at the end.
	IDirect3DSurface9* savedRT[3] = {};
	for (DWORD i = 1; i < 4; i++) { Device->GetRenderTarget(i, &savedRT[i - 1]); if (savedRT[i - 1]) Device->SetRenderTarget(i, NULL); }
	Device->SetRenderTarget(0, Shadows->CharacterMapSurface);
	Device->SetDepthStencilSurface(Shadows->CharacterMapDepthSurface);
	// The states RenderShadowMaps draws every map with, on the device as well: the first-person scene arrives with colour writes off,
	// and NiDX9RenderState may believe it already holds a value the device does not. The depth test is LESSEQUAL whatever the scene's
	// depth convention: ShadowMap.vso writes plain depth (0 at the light) and DrawShadowMap clears to 1, so nearest wins (with the
	// reversed scene's GREATEREQUAL set on the device too, every draw failed here: an empty map).
	const DWORD zFunc = D3DCMP_LESSEQUAL;
	const struct { D3DRENDERSTATETYPE type; DWORD value; } kMapStates[] = {
		{ D3DRS_ZFUNC, zFunc }, { D3DRS_STENCILENABLE, 1 }, { D3DRS_STENCILREF, 0 }, { D3DRS_STENCILFUNC, D3DCMP_ALWAYS }, { D3DRS_NORMALIZENORMALS, 1 },
		{ D3DRS_POINTSIZE, 810365505 }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
		{ D3DRS_ZENABLE, D3DZB_TRUE }, { D3DRS_ZWRITEENABLE, TRUE } };
	for (const auto& entry : kMapStates) {
		RenderState->SetRenderState(entry.type, entry.value, RenderStateArgs);
		Device->SetRenderState(entry.type, entry.value);
	}

	// Every part of the viewmodel, however small (fingers and sights), alpha tested parts included.
	map.Forms = {};
	map.Forms.Actors = true;
	map.Forms.AlphaEnabled = true;
	// The Pip-Boy fit's object is the game's last first-person draw: last frame's here (this map comes before this frame's draws), maybe gone
	// since (a weapon switch, the Pip-Boy): ClothWind's partition draws read it (crashed in PipboyFit::ForPartition). Off for this map.
	PipboyFit::Active = nullptr;
	map.CustomClearRequired = false;
	BeginShadowMap(&map, &viewProj);
	AccumChildren(Player->firstPersonNiNode, &map.Forms, false, false, nullptr);
	RenderPass::FirstPerson = true;
	DrawShadowMap(&map);
	RenderPass::FirstPerson = false;

	for (int i = 0; i < _countof(kStates); i++) {
		RenderState->SetRenderState(kStates[i], saved[i], RenderStateArgs);
		Device->SetRenderState(kStates[i], saved[i]);	// (the cache may already hold it and skip the device)
	}
	*PosAdjust = savedPosAdjust;
	TheRenderManager->CameraPosition = savedCameraPosition;
	Shadows->Constants.Data = savedData;
	Shadows->Constants.ShadowViewProj = savedViewProj;
	Device->SetDepthStencilSurface(DepthSurface);
	Device->SetRenderTarget(0, RenderSurface);
	for (DWORD i = 1; i < 4; i++) if (savedRT[i - 1]) { Device->SetRenderTarget(i, savedRT[i - 1]); savedRT[i - 1]->Release(); }
	Device->SetViewport(&viewport);
	if (DepthSurface) DepthSurface->Release();
	if (RenderSurface) RenderSurface->Release();
	// (through NiDX9RenderState where it keeps its own idea of the state, so it and the device agree again)
	RenderState->SetVertexShader(savedVS, false);
	RenderState->SetPixelShader(savedPS, false);
	if (savedDecl) RenderState->SetVertexDeclaration(savedDecl, false);
	else RenderState->SetFVF(savedFVF, false);
	Device->SetIndices(savedIB);
	for (UINT i = 0; i < 8; i++) Device->SetStreamSource(i, savedVB[i], savedOffset[i], savedStride[i]);
	for (DWORD i = 0; i < 16; i++) {
		RenderState->SetTexture(i, savedTex[i]);
		Device->SetTexture(i, savedTex[i]);	// (the cache may already hold it and skip the device)
		for (int s = 0; s < _countof(kSamplerStates); s++) {
			TheRenderManager->SetSamplerState(i, kSamplerStates[s], savedSampler[i][s]);
			Device->SetSamplerState(i, kSamplerStates[s], savedSampler[i][s]);
		}
	}
	Device->SetVertexShaderConstantF(0, &savedVSConst[0][0], 256);
	ClothWind::OnVSConstantsRestored(savedVSConst[248], savedClothNonZero, savedClothGrass);
	Device->SetPixelShaderConstantF(0, &savedPSConst[0][0], 224);
	if (savedVS) savedVS->Release();
	if (savedPS) savedPS->Release();
	if (savedDecl) savedDecl->Release();
	if (savedIB) savedIB->Release();
	for (UINT i = 0; i < 8; i++) if (savedVB[i]) savedVB[i]->Release();
	for (DWORD i = 0; i < 16; i++) if (savedTex[i]) savedTex[i]->Release();
	return true;
}


// (2026-11-24 review) The world cubemap on a frame without any shadow map: what RenderShadowMaps' setup gives the cube face pass (the
// camera's place for the camera-relative transforms, a scene, the targets and viewport put back after), and nothing else.
void ShadowManager::RenderReflectionCubeAlone(ShadowSceneLight** castLamps, NiPointLight** nearLamps) {
	IDirect3DDevice9* dev = TheRenderManager->device;
	if (!dev) return;
	IDirect3DSurface9* savedDepth = nullptr;
	IDirect3DSurface9* savedTarget = nullptr;
	D3DVIEWPORT9 savedViewport = {};
	dev->GetDepthStencilSurface(&savedDepth);
	dev->GetRenderTarget(0, &savedTarget);
	dev->GetViewport(&savedViewport);
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();
	dev->BeginScene();
	RenderReflectionCubeFace(castLamps, nearLamps);
	dev->EndScene();
	dev->SetDepthStencilSurface(savedDepth);
	dev->SetRenderTarget(0, savedTarget);
	dev->SetViewport(&savedViewport);
	if (savedDepth) savedDepth->Release();
	if (savedTarget) savedTarget->Release();
}

// UNOFFICIAL 2026-11-24: Tier 2 staggered 3D world cubemap pass for Reflections / PBR metal / _m env-map shaders.
void ShadowManager::RenderReflectionCubeFace(ShadowSceneLight** castLamps, NiPointLight** nearLamps) {
	ReflectionsEffect* refl = TheShaderManager->Effects.Reflections;
	if (!refl || !ReflCubeVertex || !ReflCubePixel || !Player || !Player->parentCell) return;
	IDirect3DDevice9* dev = TheRenderManager->device;
	NiDX9RenderState* rs = TheRenderManager->renderState;
	ShadowsExteriorEffect* fx = TheShaderManager->Effects.ShadowsExteriors;
	if (!dev || !rs || !fx) return;

	// (2026-11-27 cheaper world cube) the lamps are picked first: with the sun and sky they are the light a face is drawn under, and
	// BeginWorldCubeFaces redraws a face only when that light (or the camera's place) changed since.
	const ShaderConstants& sc = TheShaderManager->ShaderConst;
	struct LampCand { float key; NiPointLight* light; float lod; };
	LampCand bestLamps[4] = {};
	int lampCount = 0;
	const D3DXVECTOR4& camPos = TheRenderManager->CameraPosition;
	const float cullDist = refl->Settings.CubeDistance;
	auto tryLamp = [&](NiPointLight* light, float lod) {
		if (!light || light->Spec.r <= 1.0f) return;
		if ((light->Diff.r + light->Diff.g + light->Diff.b) * light->Dimmer <= 5.0f / 255.0f) return;
		if (FlashlightLamp::Is(light)) return;
		for (int i = 0; i < lampCount; i++) if (bestLamps[i].light == light) return;
		const NiPoint3& p = light->m_worldTransform.pos;
		const float dx = p.x - camPos.x, dy = p.y - camPos.y, dz = p.z - camPos.z;
		const float key = sqrtf(dx * dx + dy * dy + dz * dz) - light->Spec.r;
		if (key > cullDist) return;
		int at = lampCount < 4 ? lampCount++ : 4;
		if (at == 4) { if (key >= bestLamps[3].key) return; at = 3; }
		while (at > 0 && bestLamps[at - 1].key > key) { bestLamps[at] = bestLamps[at - 1]; at--; }
		bestLamps[at] = { key, light, lod };
	};
	if (castLamps) {
		for (int i = 0; i < ShadowCubeMapsMax; i++)
			if (castLamps[i] && castLamps[i]->sourceLight) tryLamp(castLamps[i]->sourceLight, castLamps[i]->fLODDimmer);
	}
	if (nearLamps) {
		for (int i = 0; i < TrackedLightsMax; i++)
			if (nearLamps[i]) tryLamp(nearLamps[i], 1.0f);
	}

	D3DXVECTOR3 sunDir(sc.SunDir.x, sc.SunDir.y, sc.SunDir.z);
	if (D3DXVec3LengthSq(&sunDir) > 1.0e-4f) D3DXVec3Normalize(&sunDir, &sunDir);
	else sunDir = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
	ReflectionsEffect::WorldCubeLight lightNow = {};
	{
		auto lum = [](const D3DXVECTOR4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
		lightNow.sunDir = sunDir;
		lightNow.lum[0] = lum(sc.sunColor); lightNow.lum[1] = lum(sc.sunAmbient);
		lightNow.lum[2] = lum(sc.skyColor); lightNow.lum[3] = lum(sc.fogColor);
		UInt32 set = 0;	// the lamps' addresses, each mixed, summed: the same set in any order (their order is by distance)
		for (int i = 0; i < lampCount; i++) {
			const NiPointLight* l = bestLamps[i].light;
			set += (((UInt32)(uintptr_t)l ^ 2166136261u) * 16777619u) ^ 0x9E3779B9u;
			// (review) without Dimmer: candles, fires and barrels flicker through it every few frames; a lamp switched off leaves the set
			lightNow.lampLum += (0.2126f * l->Diff.r + 0.7152f * l->Diff.g + 0.0722f * l->Diff.b) * bestLamps[i].lod;
		}
		lightNow.lampSet = set;
	}
	int faces[2] = {};
	const int faceCount = refl->BeginWorldCubeFaces(faces, lightNow);
	if (faceCount <= 0) return;
	static GpuTimer reflCubeTimer("Reflection world cubemap");	// (2026-11-24 review: here, so both callers report under one name)
	GpuProfileScope gpu(reflCubeTimer, dev);

	struct PassGuard {
		bool bindDiffuse, bindGlow, twoSided, lean, flipCull, skinnedAlpha;
		float landDrop;
		ShaderRecordVertex* vs[5];
		ShaderRecordPixel* ps[5];
		ShaderRecordPixel* skinnedAlphaPS;
		ShadowManager* sm;
		ShadowsExteriorEffect* fx;
		D3DXVECTOR4 savedData;
		D3DXMATRIX savedViewProj;
		PassGuard(ShadowManager* s, ShadowsExteriorEffect* f)
			: bindDiffuse(RenderPass::BindDiffuse), bindGlow(RenderPass::BindGlow),
			  twoSided(RenderPass::TwoSided), lean(RenderPass::Lean), flipCull(RenderPass::FlipCull),
			  skinnedAlpha(SkinnedGeoShadowRenderPass::AlphaTest), landDrop(RenderPass::LandDrop),
			  skinnedAlphaPS(s->skinnedGeoPass->AlphaPixelShader),
			  sm(s), fx(f), savedData(f->Constants.Data), savedViewProj(f->Constants.ShadowViewProj) {
			vs[0] = sm->geometryPass->VertexShader;   ps[0] = sm->geometryPass->PixelShader;
			vs[1] = sm->alphaPass->VertexShader;      ps[1] = sm->alphaPass->PixelShader;
			vs[2] = sm->skinnedGeoPass->VertexShader; ps[2] = sm->skinnedGeoPass->PixelShader;
			vs[3] = sm->speedTreePass->VertexShader;  ps[3] = sm->speedTreePass->PixelShader;
			vs[4] = sm->terrainLODPass->VertexShader; ps[4] = sm->terrainLODPass->PixelShader;
		}
		~PassGuard() {
			RenderPass::BindDiffuse = bindDiffuse;
			RenderPass::BindGlow = bindGlow;
			RenderPass::TwoSided = twoSided;
			RenderPass::Lean = lean;
			RenderPass::FlipCull = flipCull;
			RenderPass::LandDrop = landDrop;
			SkinnedGeoShadowRenderPass::AlphaTest = skinnedAlpha;
			sm->geometryPass->VertexShader   = vs[0]; sm->geometryPass->PixelShader   = ps[0];
			sm->alphaPass->VertexShader      = vs[1]; sm->alphaPass->PixelShader      = ps[1];
			sm->skinnedGeoPass->VertexShader = vs[2]; sm->skinnedGeoPass->PixelShader = ps[2];
			sm->skinnedGeoPass->AlphaPixelShader = skinnedAlphaPS;
			sm->speedTreePass->VertexShader  = vs[3]; sm->speedTreePass->PixelShader  = ps[3];
			sm->terrainLODPass->VertexShader = vs[4]; sm->terrainLODPass->PixelShader = ps[4];
			fx->Constants.Data = savedData;
			fx->Constants.ShadowViewProj = savedViewProj;
		}
	} passGuard(this, fx);

	struct StateGuard {
		IDirect3DDevice9* dev;
		NiDX9RenderState* rs;
		DWORD colorWrite = 0xF, alphaBlend = FALSE, alphaTest = FALSE, zEnable = D3DZB_TRUE, zWrite = TRUE;
		DWORD zFunc = D3DCMP_LESSEQUAL, cullMode = D3DCULL_CCW, stencilEnable = FALSE, scissorEnable = FALSE;
		StateGuard(IDirect3DDevice9* d, NiDX9RenderState* r) : dev(d), rs(r) {
			dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
			dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
			dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
			dev->GetRenderState(D3DRS_ZENABLE, &zEnable);
			dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
			dev->GetRenderState(D3DRS_ZFUNC, &zFunc);
			dev->GetRenderState(D3DRS_CULLMODE, &cullMode);
			dev->GetRenderState(D3DRS_STENCILENABLE, &stencilEnable);
			dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorEnable);
		}
		void both(D3DRENDERSTATETYPE state, DWORD value) { rs->SetRenderState(state, value, RenderStateArgs); dev->SetRenderState(state, value); }
		~StateGuard() {
			rs->SetTexture(0, nullptr);
			rs->SetTexture(1, nullptr);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
			// (2026-11-28 review) the depth, blend and stencil states back on the cache AND the device, as they were set below
			both(D3DRS_ALPHABLENDENABLE, alphaBlend);
			both(D3DRS_ALPHATESTENABLE, alphaTest);
			both(D3DRS_ZENABLE, zEnable);
			both(D3DRS_ZWRITEENABLE, zWrite);
			both(D3DRS_ZFUNC, zFunc);
			rs->SetRenderState(D3DRS_CULLMODE, cullMode, RenderStateArgs);
			both(D3DRS_STENCILENABLE, stencilEnable);
			rs->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorEnable, RenderStateArgs);
		}
	} stateGuard(dev, rs);

	// Skip the player directly during accumCellRefs below rather than mutating m_flags (UInt32).

	RenderPass::BindDiffuse = true;
	RenderPass::BindGlow = true;
	RenderPass::TwoSided = true;
	RenderPass::Lean = true;
	RenderPass::FlipCull = false;
	RenderPass::LandDrop = 0.0f;
	SkinnedGeoShadowRenderPass::AlphaTest = true;

	geometryPass->VertexShader   = ReflCubeVertex; geometryPass->PixelShader   = ReflCubePixel;
	alphaPass->VertexShader      = ReflCubeVertex; alphaPass->PixelShader      = ReflCubePixel;
	skinnedGeoPass->VertexShader = ReflCubeVertex; skinnedGeoPass->PixelShader = ReflCubePixel;
	skinnedGeoPass->AlphaPixelShader = nullptr;
	speedTreePass->VertexShader  = ReflCubeVertex; speedTreePass->PixelShader  = ReflCubePixel;
	terrainLODPass->VertexShader = ReflCubeVertex; terrainLODPass->PixelShader = ReflCubePixel;

	dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
	// (2026-11-28, world cube drew nothing outdoors) the depth, blend and stencil states go to the game's state cache AND the device:
	// ZFUNC asked for through the cache alone left the device at GREATEREQUAL (read back on the bench, Goodsprings, the game's
	// reversed depth), and with this pass's standard projection and a depth clear of 1 no pixel passed: every face stayed the clear
	// colour. The others are as exposed to a stale cache (this file's idiom for one: cache, then device).
	stateGuard.both(D3DRS_ALPHABLENDENABLE, FALSE);
	stateGuard.both(D3DRS_ALPHATESTENABLE, FALSE);
	stateGuard.both(D3DRS_ZENABLE, D3DZB_TRUE);
	stateGuard.both(D3DRS_ZWRITEENABLE, TRUE);
	stateGuard.both(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	stateGuard.both(D3DRS_STENCILENABLE, FALSE);
	rs->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE, RenderStateArgs);

	const bool exterior = TheShaderManager->GameState.isExterior;
	const PBRShaders* pbr = TheShaderManager->Shaders.PBR;
	const float G = (pbr && pbr->Constants.Linear.x > 1.0f) ? pbr->Constants.Linear.x : 2.2f;
	const float linLight = pbr ? pbr->Constants.LinearScales.x : 1.0f;
	const float linAmb = pbr ? pbr->Constants.LinearScales.y : 1.0f;
	const float linSky = exterior ? (pbr ? pbr->Constants.LinearScales.z : 1.0f) : 0.0f;
	auto decodeLin = [G](float v) { return powf((std::max)(v, 0.0f), G); };

	RecalculateBillboardVectors(&sunDir);

	const float fogStart = (std::max)(sc.fogData.x, 0.0f);
	const float fogEnd = sc.fogData.y > fogStart + 100.0f ? sc.fogData.y : fogStart + 32768.0f;
	const float invFogRange = 1.0f / (fogEnd - fogStart);

	const bool hdr = *(const bool*)0x011F941E;
	D3DXVECTOR4 psConsts[13] = {};
	psConsts[0] = D3DXVECTOR4(sunDir.x, sunDir.y, sunDir.z, exterior ? (TheShaderManager->GameState.dayLight > 0.05f ? 1.0f : 0.45f) : 0.0f);
	psConsts[1] = exterior
		? D3DXVECTOR4(decodeLin(sc.sunColor.x) * linLight, decodeLin(sc.sunColor.y) * linLight, decodeLin(sc.sunColor.z) * linLight, fogStart)
		: D3DXVECTOR4(0.0f, 0.0f, 0.0f, fogStart);
	psConsts[2] = D3DXVECTOR4(decodeLin(sc.sunAmbient.x) * linAmb, decodeLin(sc.sunAmbient.y) * linAmb, decodeLin(sc.sunAmbient.z) * linAmb, invFogRange);
	psConsts[3] = exterior
		? D3DXVECTOR4(decodeLin(sc.skyColor.x) * linSky, decodeLin(sc.skyColor.y) * linSky, decodeLin(sc.skyColor.z) * linSky, 1.0f)
		: D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
	const D3DXVECTOR4& horSrc = exterior ? sc.horizonColor : sc.fogColor;
	const float horScale = exterior ? linSky : 1.0f;
	psConsts[4] = D3DXVECTOR4(decodeLin(horSrc.x) * horScale, decodeLin(horSrc.y) * horScale, decodeLin(horSrc.z) * horScale, (float)lampCount);
	for (int i = 0; i < lampCount; i++) {
		NiPointLight* light = bestLamps[i].light;
		const NiPoint3& lp = light->m_worldTransform.pos;
		const float r = (std::max)(light->Spec.r, 1.0f);
		const float dimmer = hdr ? light->Dimmer : (std::min)(light->Dimmer, 1.0f);
		const float colW = dimmer * bestLamps[i].lod;
		psConsts[5 + i] = D3DXVECTOR4(lp.x - camPos.x, lp.y - camPos.y, lp.z - camPos.z, 1.0f / (r * r));
		psConsts[9 + i] = D3DXVECTOR4(decodeLin(light->Diff.r * colW) * linLight, decodeLin(light->Diff.g * colW) * linLight, decodeLin(light->Diff.b * colW) * linLight, 0.0f);
	}
	dev->SetPixelShaderConstantF(1, (const float*)psConsts, 13);

	dev->SetDepthStencilSurface(refl->Cubes.WorldDepth);
	const D3DVIEWPORT9 vp = { 0, 0, refl->cubeSize, refl->cubeSize, 0.0f, 1.0f };
	dev->SetViewport(&vp);

	static const D3DXVECTOR3 FaceDir[6] = {
		D3DXVECTOR3( 1.0f,  0.0f,  0.0f), D3DXVECTOR3(-1.0f,  0.0f,  0.0f),
		D3DXVECTOR3( 0.0f,  1.0f,  0.0f), D3DXVECTOR3( 0.0f, -1.0f,  0.0f),
		D3DXVECTOR3( 0.0f,  0.0f,  1.0f), D3DXVECTOR3( 0.0f,  0.0f, -1.0f)
	};
	static const D3DXVECTOR3 FaceUp[6] = {
		D3DXVECTOR3(0.0f, 1.0f,  0.0f), D3DXVECTOR3(0.0f, 1.0f,  0.0f),
		D3DXVECTOR3(0.0f, 0.0f, -1.0f), D3DXVECTOR3(0.0f, 0.0f,  1.0f),
		D3DXVECTOR3(0.0f, 1.0f,  0.0f), D3DXVECTOR3(0.0f, 1.0f,  0.0f)
	};

	const float farDist = exterior ? (std::max)(cullDist, 16384.0f) : cullDist;
	D3DXMATRIX faceProj, faceView;
	D3DXMatrixPerspectiveFovLH(&faceProj, D3DX_PI * 0.5f, 1.0f, 16.0f, farDist);
	const D3DXVECTOR3 zero(0.0f, 0.0f, 0.0f);

	ShadowsExteriorEffect::FormsStruct reflForms = {};
	// (2026-11-27 cheaper world cube) CubeLean: no actors (they move, and redraw nothing anyway between faces), no clutter (misc items,
	// books, apparatus) and nothing small; buildings, statics, trees, furniture, doors, terrain and LOD stay. At 128 texels a face and
	// the reflections' blur, what goes is a few texels.
	const bool lean = refl->Settings.CubeLean;
	reflForms.AlphaEnabled = true;
	reflForms.Activators = true;
	reflForms.Actors = !lean;
	reflForms.Apparatus = !lean;
	reflForms.Books = !lean;
	reflForms.Containers = true;
	reflForms.Doors = true;
	reflForms.Furniture = true;
	reflForms.Misc = !lean;
	reflForms.Statics = true;
	reflForms.Terrain = true;
	reflForms.Trees = true;
	reflForms.Lod = exterior;
	reflForms.MinRadius = lean ? (exterior ? 48.0f : 24.0f) : (exterior ? 12.0f : 6.0f);
	reflForms.OrigMinRadius = reflForms.MinRadius;

	NiAVObject* const playerBody = Player->GetNode();
	NiAVObject* const playerFP = Player->firstPersonNiNode;
	// UNOFFICIAL 2026-11-27 (cube-keep-capture, diagnostic; owner: the world cube showed "orange world" where Goodsprings stood)
	// [Shaders.Reflections.Main] WorldCubeDump 1: each face drawn from now on is saved once to <logs>\VegasShaders_WorldCube_<face>.dds and logged with
	// what its reference walk kept and dropped; the key goes back to 0 when all six are saved.
	static int dumpSaved = 0;
	const bool dumping = TheSettingManager->GetSettingI("Shaders.Reflections.Main", "WorldCubeDump") != 0;
	if (!dumping) dumpSaved = 0;
	struct RefCounts { int seen = 0, noNode = 0, hidden = 0, outside = 0, refracting = 0, distant = 0, kept = 0; } refCounts;
	PortalVisibility::RoomSet portalRooms;
	if (!exterior) portalRooms.Build(SceneNode);

	for (int fi = 0; fi < faceCount; fi++) {
		const int f = faces[fi];
		if (f < 0 || f >= 6 || !refl->Cubes.WorldFace[f]) continue;

		dev->SetRenderTarget(0, refl->Cubes.WorldFace[f]);
		dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

		D3DXMatrixLookAtLH(&faceView, &zero, &FaceDir[f], &FaceUp[f]);
		fx->Constants.ShadowViewProj = faceView * faceProj;

		const NiPoint3 forward(FaceDir[f].x, FaceDir[f].y, FaceDir[f].z);
		const NiPoint3 up(FaceUp[f].x, FaceUp[f].y, FaceUp[f].z);
		const NiPoint3 right(
			forward.y * up.z - forward.z * up.y,
			forward.z * up.x - forward.x * up.z,
			forward.x * up.y - forward.y * up.x);
		NiTransform placement;
		placement.pos = NiPoint3(camPos.x, camPos.y, camPos.z);
		placement.rot = NiMatrix33(forward.x, up.x, right.x,
								   forward.y, up.y, right.y,
								   forward.z, up.z, right.z);
		placement.scale = 1.0f;
		NiFrustumPlanes planes;
		planes.Set(NiFrustum(-1.0f, 1.0f, 1.0f, -1.0f, 16.0f, farDist, false), placement);
		planes.SetActivePlaneState(63);

		auto accumCellRefs = [&](TESObjectCELL* cell) {
			if (!cell) return;
			for (TList<TESObjectREFR>::Entry* it = &cell->objectList.First; it != nullptr; it = it->next) {
				TESObjectREFR* ref = it->item;
				if (!ref || ref == (TESObjectREFR*)Player) continue;
				refCounts.seen++;
				NiNode* refRoot = GetRefNode(ref, &reflForms);
				if (!refRoot || refRoot == playerBody || refRoot == playerFP) { refCounts.noNode++; continue; }
				if (!exterior && portalRooms.active) {
					const bool hidden = portalRooms.IsNodeHidden(refRoot);
					PortalVisibility::NoteReflRef(hidden);
					if (hidden) { refCounts.hidden++; continue; }
				}
				if (!refRoot->WithinFrustum(&planes)) { refCounts.outside++; continue; }
				if (IsRefracting(ref)) { refCounts.refracting++; continue; }
				const NiBound* b = refRoot->GetWorldBound();
				if (b) {
					const float dx = b->Center.x - camPos.x, dy = b->Center.y - camPos.y, dz = b->Center.z - camPos.z;
					const float maxR = cullDist + b->Radius;
					if (dx * dx + dy * dy + dz * dz > maxR * maxR) { refCounts.distant++; continue; }
				}
				refCounts.kept++;
				AccumChildren(refRoot, &reflForms, false, false, &planes);
			}
		};

		if (exterior && Player->GetWorldSpace()) {
			AccumChildren(BGSTerrainManager::GetRootLandLODNode(), &reflForms, true, true, &planes);
			AccumChildren(BGSTerrainManager::GetRootObjectLODNode(), &reflForms, false, true, &planes);
			if (GridCellArray* grid = Tes->gridCellArray) {
				const UInt32 cellCount = grid->size * grid->size;
				for (UInt32 c = 0; c < cellCount; ++c) {
					TESObjectCELL* cell = grid->GetCell(c);
					if (!cell || cell->IsInterior()) continue;
					if (NiNode* landRoot = cell->GetChildNode(TESObjectCELL::kCellNode_Land))
						AccumChildren(landRoot, &reflForms, true, false, &planes);
					accumCellRefs(cell);
				}
			}
		} else {
			accumCellRefs(Player->parentCell);
		}

		const bool dumpThis = dumping && !(dumpSaved & (1 << f));
		if (dumpThis) {	// the device's own state the faces are drawn with (the game's state cache can differ: see ZFUNC above)
			DWORD zEnableNow = 0, zFuncNow = 0, colorWriteNow = 0, cullNow = 0, clipPlanesNow = 0, zWriteNow = 0, blendNow = 0, stencilNow = 0;
			dev->GetRenderState(D3DRS_ZENABLE, &zEnableNow); dev->GetRenderState(D3DRS_ZFUNC, &zFuncNow); dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWriteNow);
			dev->GetRenderState(D3DRS_CULLMODE, &cullNow); dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &clipPlanesNow);
			dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWriteNow); dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blendNow); dev->GetRenderState(D3DRS_STENCILENABLE, &stencilNow);
			Logger::Log("UNOFFICIAL WorldCubeDump face %d device state: z %u func %u write %u, colour write %X, blend %u, stencil %u, cull %u, clip planes %X; queued opaque %u, LOD land %u, alpha %u, skinned %u, tree %u",
				f, zEnableNow, zFuncNow, zWriteNow, colorWriteNow, blendNow, stencilNow, cullNow, clipPlanesNow, (unsigned)geometryPass->GeometryList.size(), (unsigned)terrainLODPass->GeometryList.size(),
				(unsigned)alphaPass->GeometryList.size(), (unsigned)skinnedGeoPass->GeometryList.size(), (unsigned)speedTreePass->GeometryList.size());
		}

		RenderAccums();
		refl->MarkWorldCubeFaceRendered(f);
		if (dumpThis) {
			// (2026-11-28 review) from the game folder, not the working directory
			const std::string path = Paths::GameRoot() + "\\" + Paths::LogsFolder + "\\VegasShaders_WorldCube_" + std::to_string(f) + ".dds";
			const HRESULT hr = D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_DDS, refl->Cubes.WorldFace[f], NULL, NULL);
			Logger::Log("UNOFFICIAL WorldCubeDump face %d (%s): cam %.0f %.0f %.0f, refs seen %d, no node %d, hidden %d, outside %d, refracting %d, beyond CubeDistance %d, kept %d, cube complete %d; %s",
				f, exterior ? "exterior" : "interior", camPos.x, camPos.y, camPos.z, refCounts.seen, refCounts.noNode, refCounts.hidden, refCounts.outside,
				refCounts.refracting, refCounts.distant, refCounts.kept, refl->WorldCubeReady() ? 1 : 0, SUCCEEDED(hr) ? path.c_str() : "save failed");
			dumpSaved |= 1 << f;
			if (dumpSaved == 0x3F) { TheSettingManager->SetSetting("Shaders.Reflections.Main", "WorldCubeDump", 0); dumpSaved = 0; }
		}
		refCounts = RefCounts();
	}
}
