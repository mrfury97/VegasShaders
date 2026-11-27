#pragma once

// UNOFFICIAL 2026-11-23 BlendedLayerFix ([Main.Main.Misc] BlendedLayerFix, on by default): fixes Gamebryo's single bounding-sphere
// sort inversion in BSShaderAccumulator::RenderAlphaGeometry (0x00B650C0), where huge surface-hugging vertex-color AO / grime /
// shadow shells (BSSM_NOLIGHTING_VC / _FALLOFF: NOLIGHTVCONLY.pso, bound radius 300-1750, ~1,400 vanilla NIFs including
// NV_Silverrush01's Silverrush04:0) and blended static wall decals sort closer than smoke/fire/dust particle plumes when the camera
// nears a building and pop in front of / bleed through the smoke.
//
// Inside RenderBlendedHook (after SortAlphaPasses has built BSBatchRenderer::AlphaGroupList[Arg], before RenderAlphaGeometry draws it):
//   1. Stably partition each AlphaGroupList[Arg] so pure surface-overlay groups draw BEFORE other blended groups (smoke, particles,
//      fire, translucent effects), keeping each layer's internal back-to-front order unchanged.
//   2. Suppress phantom BSSP_FLAG_ZBUFFER_WRITE on those translucent NOLIGHTVCONLY AO shells for the duration of RenderBlended (restored
//      immediately in ~Scope), so alpha=0 edge triangles never cut holes in smoke drawn after them or pollute GlassMask.
#include <algorithm>
#include <cmath>
#include <vector>

namespace BlendedLayerFix {

	struct RenderPassView {
		NiGeometry*	geometry;	// +0x00
		UInt16		passEnum;	// +0x04
		UInt8		accumHint;	// +0x06
		UInt8		firstPass;	// +0x07
		UInt8		lastPass;	// +0x08
		UInt8		numLights;	// +0x09
		UInt8		maxLights;	// +0x0A
		UInt8		extra;		// +0x0B
		void**		lights;		// +0x0C
	};
	static_assert(offsetof(RenderPassView, passEnum) == 0x04 && offsetof(RenderPassView, lights) == 0x0C, "RenderPassView layout");

	struct PassNode {
		PassNode*		next;	// +0x00
		PassNode*		prev;	// +0x04
		RenderPassView*	pass;	// +0x08
	};
	static_assert(offsetof(PassNode, pass) == 0x08 && sizeof(PassNode) == 0x0C, "PassNode layout");

	struct GeometryGroupView {
		UInt8		ucFlags;		// +0x00
		UInt8		pad01;			// +0x01
		UInt16		usCount;		// +0x02
		UInt32		uiPassCount;	// +0x04
		void*		pBatchRenderer;	// +0x08
		UInt32		passListWord0;	// +0x0C BSTPersistentList (+0x00)
		PassNode*	passHead;		// +0x10 BSTPersistentList::pHead (+0x04)
		PassNode*	passTail;		// +0x14 BSTPersistentList::pTail (+0x08)
		void*		passNextFree;	// +0x18 BSTPersistentList::pNextFreeItem (+0x0C)
		UInt32		passAlloc;		// +0x1C BSTPersistentList::Allocator (+0x10)
		float		fDepth;			// +0x20
	};
	static_assert(offsetof(GeometryGroupView, passHead) == 0x10 && offsetof(GeometryGroupView, fDepth) == 0x20 && sizeof(GeometryGroupView) == 0x24, "GeometryGroupView layout");

	struct AlphaListNode {
		AlphaListNode*		next;	// +0x00
		AlphaListNode*		prev;	// +0x04
		GeometryGroupView*	group;	// +0x08
	};
	static_assert(offsetof(AlphaListNode, group) == 0x08 && sizeof(AlphaListNode) == 0x0C, "AlphaListNode layout");

	struct AlphaListView {
		AlphaListNode*	head;	// +0x00
		AlphaListNode*	tail;	// +0x04
		UInt32			count;	// +0x08 NiTListBase::m_kAllocator count word
	};
	static_assert(sizeof(AlphaListView) == 0x0C, "AlphaListView layout");

	inline bool IsEnabled() {
		return TheSettingManager && TheSettingManager->GetSettingI("Main.Main.Misc", "BlendedLayerFix") != 0;
	}

	// Non-skinned BSShaderNoLighting vertex-color-only passes (BSShaderManager::eBSSMPassEnum: BSSM_NOLIGHTING_VC=0x43, BSSM_NOLIGHTING_VC_FALLOFF=0x4B).
	inline bool IsNoLightVcPass(UInt16 passEnum) {
		return passEnum == 0x43 || passEnum == 0x4B;
	}

	inline bool HasDarkColorOrEmissive(const NiGeometry* geometry) {
		const NiGeometryData* gd = geometry->geomData;
		if (gd && gd->m_pkColor && gd->m_usVertices > 0) {
			const UInt16 n = gd->m_usVertices;
			const UInt16 step = n <= 8 ? 1 : (UInt16)(n / 8);
			float opaqueMax = 0.0f;
			float allMax = 0.0f;
			bool anyOpaque = false;
			for (UInt16 i = 0; i < n; i += step) {
				const NiColorAlpha& c = gd->m_pkColor[i];
				const float m = (std::max)(c.r, (std::max)(c.g, c.b));
				if (m > allMax) allMax = m;
				if (c.a >= 0.05f) {
					anyOpaque = true;
					if (m > opaqueMax) opaqueMax = m;
				}
			}
			return (anyOpaque ? opaqueMax : allMax) <= 0.55f;
		}
		const NiMaterialProperty* mat = geometry->propertyState.m_spMaterialProperty;
		if (mat) {
			const float maxEmit = (std::max)(mat->emit.r, (std::max)(mat->emit.g, mat->emit.b));
			return maxEmit <= 0.55f;
		}
		return false;
	}

	inline bool IsBlendedNonAdditive(const NiAlphaProperty* alpha, bool& outStandardAlpha, bool& outMultiplicative) {
		if (!alpha || !(alpha->flags & NiAlphaProperty::ALPHA_BLEND_MASK)) return false;
		const UInt32 src = (alpha->flags >> 1) & 0xFu;
		const UInt32 dst = (alpha->flags >> 5) & 0xFu;
		if (dst == 0u) return false;	// never touch additive glows / beams / flashes (ALPHA_ONE)
		outStandardAlpha = (src == 6u && dst == 7u);	// SRCALPHA / INVSRCALPHA
		outMultiplicative = (src == 1u && dst == 2u);	// ZERO / SRCCOLOR
		return outStandardAlpha || outMultiplicative;
	}

	// True for surface-hugging vertex-color-only NoLighting AO / shadow / grime shells.
	inline bool IsNoLightVcOverlay(const NiGeometry* geometry, UInt16 passEnum, bool* outZWrite = nullptr) {
		if (!geometry || !IsNoLightVcPass(passEnum)) return false;
		const BSShaderProperty* shade = geometry->propertyState.GetShadeProperty<BSShaderProperty>();
		if (!shade || (shade->m_usFlags.field & NiShadeProperty::kFirstPerson) != 0) return false;
		bool standardAlpha = false, multiplicative = false;
		if (!IsBlendedNonAdditive(geometry->propertyState.m_spAlphaProperty, standardAlpha, multiplicative)) return false;

		const bool zWrite = (shade->ulFlags[1] & 1u) != 0;		// BSSP_FLAG_ZBUFFER_WRITE
		if (!multiplicative && !zWrite && !HasDarkColorOrEmissive(geometry)) return false;

		if (outZWrite) *outZWrite = zWrite;
		return true;
	}

	// True for any NoLighting surface overlay (VC AO shells plus textured non-PSYS NoLighting decals), also used by
	// ParticleShaders::ForDraw so MeshSoftScale does not fade large architectural overlays away.
	inline bool IsNoLightSurfaceOverlay(const NiGeometry* geometry, UInt16 passEnum, bool* outZWrite = nullptr) {
		if (IsNoLightVcOverlay(geometry, passEnum, outZWrite)) return true;
		if (!geometry || passEnum < 0x42 || passEnum > 0x51) return false;
		const BSShaderProperty* shade = geometry->propertyState.GetShadeProperty<BSShaderProperty>();
		if (!shade || (shade->m_usFlags.field & NiShadeProperty::kFirstPerson) != 0) return false;
		const NiAlphaProperty* alpha = geometry->propertyState.m_spAlphaProperty;
		bool standardAlpha = false, multiplicative = false;
		if (!IsBlendedNonAdditive(alpha, standardAlpha, multiplicative)) return false;
		const UInt32 decalMask = (1u << BSSP_DECAL) | (1u << BSSP_DYNAMIC_DECAL);
		if ((shade->ulFlags[0] & decalMask) == 0) return false;
		if (outZWrite) *outZWrite = ((shade->ulFlags[1] & 1u) != 0) && ((alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK) == 0);
		return true;
	}

	inline bool IsSurfaceOverlayPass(const NiGeometry* geometry, UInt16 passEnum, bool& outSuppressNoLightZWrite, bool& outTempDecalFlag) {
		outSuppressNoLightZWrite = false;
		outTempDecalFlag = false;
		if (!geometry) return false;
		const BSShaderProperty* shade = geometry->propertyState.GetShadeProperty<BSShaderProperty>();
		if (!shade || (shade->m_usFlags.field & NiShadeProperty::kFirstPerson) != 0) return false;
		const NiAlphaProperty* alpha = geometry->propertyState.m_spAlphaProperty;
		bool standardAlpha = false, multiplicative = false;
		if (!IsBlendedNonAdditive(alpha, standardAlpha, multiplicative)) return false;

		// Case A: vertex-color-only NoLighting AO / grime / contact-shadow shell or non-PSYS NoLighting decal.
		bool zWrite = false;
		if (IsNoLightSurfaceOverlay(geometry, passEnum, &zWrite)) {
			outSuppressNoLightZWrite = zWrite && ((alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK) == 0);
			return true;
		}

		// Case B: blended static lit decals / building crack overlays (explicit BSSP_DECAL flags, large pure-alpha
		// architectural surface overlays such as ApartmentPre01a:0 / ApartmentPre01dcl.dds, or giant merged exterior window panes
		// such as ApartmentPre01c:0 so street smoke/fire draws in front of the facade windows while GlassMask keeps its depth).
		if (!geometry->skinInstance && const_cast<BSShaderProperty*>(shade)->IsLightingProperty()) {
			const UInt32 decalMask = (1u << BSSP_DECAL) | (1u << BSSP_DYNAMIC_DECAL);
			const bool explicitDecal = (shade->ulFlags[0] & decalMask) != 0;
			const UInt32 kNonOverlayLitFlags = (1u << BSSP_ENVIRONMENT_MAPPING) | (1u << BSSP_FACEGEN) |
				(1u << BSSP_REFRACTION) | (1u << BSSP_FIRE_REFRACTION) | (1u << BSSP_EYE_ENVIRONMENT_MAPPING) |
				(1u << BSSP_HAIR) | (1u << BSSP_WINDOW_ENVIRONMENT_MAPPING);
			const bool noAlphaTest = (alpha->flags & NiAlphaProperty::TEST_ENABLE_MASK) == 0;
			const float radius = const_cast<NiGeometry*>(geometry)->GetWorldBoundRadius();
			const bool implicitArchOverlay =
				(shade->ulFlags[0] & kNonOverlayLitFlags) == 0 &&
				(shade->ulFlags[1] & (1u << 15)) == 0 &&	// exclude SF2_EnvMap_Light_Fade windows/glass
				noAlphaTest &&
				radius >= 100.0f;
			const bool exteriorMergedWindow =
				TheShaderManager && TheShaderManager->GameState.isExterior &&
				(shade->ulFlags[0] & (1u << BSSP_WINDOW_ENVIRONMENT_MAPPING)) != 0 &&
				radius >= 300.0f;
			if (explicitDecal || implicitArchOverlay || exteriorMergedWindow) {
				// ShadowLightShader ignores ulFlags[1] & 1; RenderNextAlpha checks BSSP_DECAL / BSSP_DYNAMIC_DECAL instead.
				outTempDecalFlag = !explicitDecal && implicitArchOverlay && ((shade->ulFlags[1] & 1u) != 0) && noAlphaTest;
				return true;
			}
		}
		return false;
	}

	inline bool IsSurfaceOverlayGroup(const GeometryGroupView* group, std::vector<BSShaderProperty*>& outNoLightZWriteProps,
		std::vector<BSShaderProperty*>& outTempDecalProps) {
		if (!group || !group->passHead) return false;
		const size_t baseZIdx = outNoLightZWriteProps.size();
		const size_t baseDecalIdx = outTempDecalProps.size();
		unsigned count = 0;
		for (const PassNode* n = group->passHead; n; n = n->next) {
			if (++count > 64u) {
				outNoLightZWriteProps.resize(baseZIdx);
				outTempDecalProps.resize(baseDecalIdx);
				return false;
			}
			const RenderPassView* pass = n->pass;
			if (!pass || !pass->geometry) {
				outNoLightZWriteProps.resize(baseZIdx);
				outTempDecalProps.resize(baseDecalIdx);
				return false;
			}
			bool suppressNoLightZ = false, tempDecal = false;
			if (!IsSurfaceOverlayPass(pass->geometry, pass->passEnum, suppressNoLightZ, tempDecal)) {
				outNoLightZWriteProps.resize(baseZIdx);
				outTempDecalProps.resize(baseDecalIdx);
				return false;
			}
			if (suppressNoLightZ || (tempDecal && pass->firstPass != 0 && pass->lastPass != 0)) {
				BSShaderProperty* shade = pass->geometry->propertyState.GetShadeProperty<BSShaderProperty>();
				if (shade) {
					if (suppressNoLightZ) outNoLightZWriteProps.push_back(shade);
					else outTempDecalProps.push_back(shade);
				}
			}
		}
		return count > 0;
	}

	// Scoped around RenderBlended(This, Arg) in RenderBlendedHook.
	struct Scope {
		std::vector<BSShaderProperty*> savedZWrite;
		std::vector<BSShaderProperty*> savedDecalFlag;

		Scope(void* accumulator, UInt32 arg, bool worldScene) {
			if (!worldScene || !accumulator || arg >= 2u || !IsEnabled()) return;
			const UInt8* acc = (const UInt8*)accumulator;
			const UInt32 renCount = *(const UInt32*)(acc + 0x18C);
			if (renCount == 0u) return;

			// Temporary buffers reused without re-allocating across frames.
			static thread_local std::vector<PassNode*> passNodes;
			static thread_local std::vector<RenderPassView*> layer0Passes;
			static thread_local std::vector<RenderPassView*> layer1Passes;
			static thread_local std::vector<BSShaderProperty*> tempDecalProps;
			static thread_local std::vector<BSShaderProperty*> multiPassProps;
			static thread_local std::vector<AlphaListNode*> nodes;
			static thread_local std::vector<GeometryGroupView*> groups;
			static thread_local std::vector<GeometryGroupView*> layer0;
			static thread_local std::vector<GeometryGroupView*> layer1;
			static thread_local std::vector<BSShaderProperty*> groupZProps;
			static thread_local std::vector<BSShaderProperty*> groupDecalProps;

			const UInt32 clampedCount = renCount < 2u ? renCount : 2u;
			for (UInt32 r = 0; r < clampedCount; r++) {
				UInt8* renderer = *(UInt8* const*)(acc + 0x174 + 4u * r);
				if (!renderer) continue;

				// 1. Primary path (BSBatchRenderer::RegisterAlphaPass 0x00B9B3D0 when iGroupingAlphas == 0):
				//    ungrouped alpha passes live in pUngroupedAlphas[arg] (renderer + 0x70 + 4 * arg), sorted descending (head = drawn first).
				GeometryGroupView* ungrouped = *(GeometryGroupView* const*)(renderer + 0x70 + 4u * arg);
				if (ungrouped && ungrouped->passHead && ungrouped->usCount > 0u && ungrouped->usCount <= 16384u) {
					passNodes.clear();
					multiPassProps.clear();
					bool ok = true;
					for (PassNode* n = ungrouped->passHead; n; n = n->next) {
						if (passNodes.size() >= 16384u || !n->pass || !n->pass->geometry) { ok = false; break; }
						passNodes.push_back(n);
						if (n->pass->firstPass == 0 || n->pass->lastPass == 0) {
							BSShaderProperty* sp = n->pass->geometry->propertyState.GetShadeProperty<BSShaderProperty>();
							if (sp) multiPassProps.push_back(sp);
						}
					}
					if (ok && passNodes.size() >= 2u) {
						std::sort(multiPassProps.begin(), multiPassProps.end());
						layer0Passes.clear();
						layer1Passes.clear();
						tempDecalProps.clear();
						for (PassNode* n : passNodes) {
							RenderPassView* p = n->pass;
							bool suppressNoLightZ = false, tempDecal = false;
							if (IsSurfaceOverlayPass(p->geometry, p->passEnum, suppressNoLightZ, tempDecal)) {
								layer0Passes.push_back(p);
								if (suppressNoLightZ || tempDecal) {
									BSShaderProperty* sp = p->geometry->propertyState.GetShadeProperty<BSShaderProperty>();
									if (sp) {
										if (suppressNoLightZ) savedZWrite.push_back(sp);
										else if (!std::binary_search(multiPassProps.begin(), multiPassProps.end(), sp))
											tempDecalProps.push_back(sp);
									}
								}
							}
							else {
								layer1Passes.push_back(p);
							}
						}
						if (!layer0Passes.empty() && !layer1Passes.empty()) {
							size_t outIdx = 0;
							for (RenderPassView* p : layer0Passes) passNodes[outIdx++]->pass = p;
							for (RenderPassView* p : layer1Passes) passNodes[outIdx++]->pass = p;
							for (BSShaderProperty* sp : tempDecalProps) savedDecalFlag.push_back(sp);
							static bool logged = false;
							if (!logged) {
								logged = true;
								Logger::Log("UNOFFICIAL BlendedLayerFix: partitioned ungrouped alpha list (%zu overlays before %zu other passes; %zu z-write, %zu decal flags)",
									layer0Passes.size(), layer1Passes.size(), savedZWrite.size(), savedDecalFlag.size());
							}
						}
					}
				}

				// 2. Grouped alpha path: AlphaGroupList[arg] (renderer + 0x78 + 0x0C * arg).
				AlphaListView* list = (AlphaListView*)(renderer + 0x78 + 0x0C * arg);
				if (!list->head || list->count == 0u || list->count > 16384u) continue;

				nodes.clear();
				groups.clear();
				for (AlphaListNode* node = list->head; node && nodes.size() < list->count; node = node->next) {
					if (!node->group) { nodes.clear(); break; }
					nodes.push_back(node);
					groups.push_back(node->group);
				}
				if (nodes.size() != list->count || groups.size() != list->count) continue;

				layer0.clear();
				layer1.clear();
				for (GeometryGroupView* g : groups) {
					groupZProps.clear();
					groupDecalProps.clear();
					if (IsSurfaceOverlayGroup(g, groupZProps, groupDecalProps)) {
						layer0.push_back(g);
						for (BSShaderProperty* sp : groupZProps) savedZWrite.push_back(sp);
						for (BSShaderProperty* sp : groupDecalProps) savedDecalFlag.push_back(sp);
					}
					else {
						layer1.push_back(g);
					}
				}
				if (layer0.empty()) continue;

				// SortAlphaPasses stores descending fDepth (head = farthest = drawn first -> tail = nearest = drawn last).
				// Bump layer0 group depths above 1e6 so RenderNextAlpha drains grouped overlays before ungrouped particles/smoke too.
				const bool descendingFromHead = groups.front()->fDepth >= groups.back()->fDepth;
				if (descendingFromHead) {
					float maxDepth = (std::max)(groups.front()->fDepth, 1.0e6f);
					for (const GeometryGroupView* g : groups) {
						if (g->fDepth > maxDepth) maxDepth = g->fDepth;
					}
					float bump = maxDepth + (float)layer0.size() * 100.0f;
					for (GeometryGroupView* g : layer0) {
						g->fDepth = bump;
						bump -= 100.0f;
					}
				}
				else if (clampedCount > 1u) {
					float minDepth = groups.front()->fDepth;
					for (const GeometryGroupView* g : groups) {
						if (g->fDepth < minDepth) minDepth = g->fDepth;
					}
					float bump = minDepth - (float)layer0.size() * 100.0f;
					for (GeometryGroupView* g : layer0) {
						g->fDepth = bump;
						bump += 100.0f;
					}
				}

				if (list->count >= 2u && !layer1.empty()) {
					size_t outIdx = 0;
					if (descendingFromHead) {
						for (GeometryGroupView* g : layer0) nodes[outIdx++]->group = g;
						for (GeometryGroupView* g : layer1) nodes[outIdx++]->group = g;
					}
					else {
						for (GeometryGroupView* g : layer1) nodes[outIdx++]->group = g;
						for (GeometryGroupView* g : layer0) nodes[outIdx++]->group = g;
					}
				}
			}

			// Deduplicate saved ZWrite properties and suppress BSSP_FLAG_ZBUFFER_WRITE (ulFlags[1] & 1) for this RenderBlended call.
			if (!savedZWrite.empty()) {
				std::sort(savedZWrite.begin(), savedZWrite.end());
				savedZWrite.erase(std::unique(savedZWrite.begin(), savedZWrite.end()), savedZWrite.end());
				for (BSShaderProperty* sp : savedZWrite) sp->ulFlags[1] &= ~1u;
			}
			// Deduplicate and set temporary BSSP_DECAL on pure-alpha implicit lit overlays so RenderNextAlpha disables ZWrite cleanly.
			if (!savedDecalFlag.empty()) {
				const UInt32 decalBit = 1u << BSSP_DECAL;
				std::sort(savedDecalFlag.begin(), savedDecalFlag.end());
				savedDecalFlag.erase(std::unique(savedDecalFlag.begin(), savedDecalFlag.end()), savedDecalFlag.end());
				savedDecalFlag.erase(std::remove_if(savedDecalFlag.begin(), savedDecalFlag.end(),
					[decalBit](const BSShaderProperty* sp) { return (sp->ulFlags[0] & decalBit) != 0; }), savedDecalFlag.end());
				for (BSShaderProperty* sp : savedDecalFlag) sp->ulFlags[0] |= decalBit;
			}
		}

		~Scope() {
			for (BSShaderProperty* sp : savedZWrite) sp->ulFlags[1] |= 1u;
			const UInt32 decalBit = 1u << BSSP_DECAL;
			for (BSShaderProperty* sp : savedDecalFlag) sp->ulFlags[0] &= ~decalBit;
		}
	};
}
