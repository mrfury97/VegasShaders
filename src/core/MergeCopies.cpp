// UNOFFICIAL 2026-11-14 MergeCopies: see MergeCopies.h (what, where, the self-check) and docs/next/FD-MERGE-RESULTS.md.
// Engine addresses: read in the exe; names from WallSoGB's symbol map of FalloutNV.exe.
#include "MergeCopies.h"
#include "GBufferCensus.h"
#include "PassSort.h"
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace MergeCopies {
namespace {
	const UInt32 kVtblTriShape = 0x0109D454, kVtblTriStrips = 0x0109CD44;		// NiTriShape / NiTriStrips
	const UInt32 kVtblTriShapeData = 0x0109DAC4, kVtblTriStripsData = 0x0109D8AC;	// their static data (not the dynamic kinds)
	const UInt32 kVtblPPLighting = 0x010AE0D0;	// BSShaderPPLightingProperty
	const UInt32 kSetModelTransform = 0x00E6FBB0;	// NiDX9Renderer::SetModelTransform(const NiTransform&, bool) (thiscall)
	const UInt32 kMatrixMultiplyTranspose = 0x00EE6DFE;	// the exe's D3DXMatrixMultiplyTranspose import thunk
	const UInt32 kVec3TransformNormal = 0x00EE6DF2;	// the exe's D3DXVec3TransformNormal import thunk
	const UInt32 kGetInverseWorldMatrix = 0x00C4C2D0;	// BSUtilities::GetInverseWorldMatrix(const NiTransform*, int, D3DXMATRIX*) (cdecl)
	const UInt32 kDefaultNormalMap = 0x011F9510, kDefaultNormalGlossMap = 0x011F9518;	// BSShaderManager's (ShadowLightShader::GetNormalMap 0x00C03230)
	const UInt32 kVtblShadowLight = 0x010AF2F8;	// ShadowLightShader (the shader of every pass merged here)
	// v2: the lamp constants of the non-Opt passes (ShadowLightShader::SetupGeometryConstants 0x00B7DAB0 calls SetupGeometryConstants_Lights
	// 0x00B78A90 at 0x00B7DBAC: checked at run time, another plugin may have replaced the call), NiPoint3::Unitize, the exe's D3DXVec3Normalize thunk
	const UInt32 kLightsCallSite = 0x00B7DBAC, kSetupLights = 0x00B78A90, kUnitize = 0x004A0C10, kVec3Normalize = 0x00EE6DEC;

	enum { kInstRegs = 6, kInstBytes = kInstRegs * 16, kMaxGroup = 256, kRingInstances = 16384, kProve = 3, kCheckEvery = 8, kSampleEvery = 16 };
	enum Action : unsigned char { ActNone, ActLeader, ActCheckRef, ActCheckCopy, ActCheckSuppress };
	enum State : unsigned char { StUnseen, StPlain, StCheck, StMerged, StMergedCheck };
	enum Why : unsigned char { WhyKind, WhyData, WhySkin, WhyProperty, WhyFade, WhyAlpha, WhyLight, WhyShader, WhyAddOn, WhyCount };
	const char* const WhyNames[WhyCount] = { "kind", "data", "skinned", "property", "fade", "alpha-blend", "light", "shader", "add-on" };

	// v2 (batch 2): which passes. Opt = 0xFC / 0xFD (Setup_ADT_Opt: exact without deferral, rule a). Ad = the light-only base passes AD2 / AD3
	// (0xD7-0xFA, light 0 is the cell's directional light, 1-2 lamps): with a deferred record bound. Lit = the other lit ShadowLightShader base
	// passes with fewer than 4 lights (ADTS, ADTS2, ADTS_ONELIGHT / DIRANDPT, ADT, ADT2): only with DeferredRenderer 2's lighting-free records.
	// Not: the many-lamp passes 0xC3-0xD6 (ADT10 / ADT4 / ADTS10: their vertex shaders read LightData past c27, no instanced copy) and the
	// other Opt passes (0xCA, 0xD1, 0x102: their own setups, not read).
	enum Kind : UInt8 { KindNone, KindOpt, KindAd, KindLit };
	inline Kind KindOfPass(UInt16 id) {
		if (id == 0xFC || id == 0xFD) return KindOpt;
		if (id >= 0xD7 && id <= 0xFA) return KindAd;
		if ((id >= 0x95 && id <= 0xC2) || id == 0xFB || (id >= 0xFE && id <= 0x12B && id != 0x102)) return KindLit;
		return KindNone;
	}
	const char* const KindNames[4] = { "-", "opt", "ad", "lit" };

	// The vertex registers a group's copies take from the instance stream or may leave as the first entry's (bits of r = 16, 25, 26, 27):
	//  Opt: c0-c3 and c25 computed; c16 / c26 / c27 are not set per object in 0xFC, so they are equal and compared.
	//  Ad with the lamps-deferred record (DeferredRenderer 1, LAMPS_DEFERRED 3): c0-c3 and c25 (the cell light, which the record keeps)
	//  computed; c16 (the view vector), c26 / c27 (lamps 1-2) left as the first entry's and not compared: the record's pixel shader reads
	//  none of the interpolators they feed (fxc of SLS2041.pso: TEXCOORD2 / 3 / 5 and TEXCOORD6.xyz not declared; FD-MERGE-RESULTS.md).
	//  Ad with the lighting-free record (DeferredRenderer 2, LAMPS_DEFERRED 2): only c0-c3 computed; c16 and c25-c27 left and not
	//  compared (TEXCOORD1 not declared either: fxc of SLS2037 / 2038 / 2041 / 2043 / 2044.pso).
	//  Lit with the lighting-free record: these records DO read TEXCOORD1.w (lightDir.w, from c25.w) and, the SPECULAR ones, TEXCOORD6.xyz
	//  (the view vector, from c16) (fxc of SLS2000 / 2001 / 2004 / 2005 / 2011 / 2015 / 2017 / 2021 / 2023.pso), so c16 and c25 are computed
	//  (ComputeEye, ComputeAdLight); only c26 / c27 (lamps 1-2: TEXCOORD2 / 3 not declared) are left and not compared.
	enum Ignore : UInt8 { Ig16 = 1, Ig25 = 2, Ig26 = 4, Ig27 = 8 };

	struct KeyState { UInt16 checks = 0; bool proven = false, blacklisted = false; };
	struct Group { UInt64 key; UInt32 first; UInt16 count, arrived; UInt8 state; bool checkFailed; UInt8 kind, ignore; UInt16 pass; };
	int ListDeferredMode = 0;			// DeferredMode() at the list's start
	bool LightsCallOk = false, LightsCallChecked = false;
	UInt32 EyeTransformFn = 0;		// the call's target at 0x00B79019 (0: not a call: the Lit passes stay off; see ComputeEye)
	unsigned long long SKindGroups[4] = {}, SKindCopies[4] = {}, SKindMerged[4] = {};
	unsigned long long SClass[6] = {};	// batch 4 (F10): first-pass entries of the base passes 0x64-0x12B: -, opt, ad, lit, many-lamp, other

	std::vector<void*> Members;			// the planned entries, group by group
	std::vector<UInt32> MemberGroup;
	std::unordered_map<const void*, UInt32> Where;	// RenderPass -> index into Members
	std::vector<Group> Groups;
	std::unordered_map<UInt64, KeyState> Keys;	// the whole session
	unsigned BlacklistedKeys = 0, ProvenKeys = 0;

	// the group whose entry is in the engine's setup now
	Group* Current = nullptr;
	UInt32 CurrentIndex = 0;
	bool Issued = false, FellBack = false;
	unsigned ExtraDraws = 0;
	bool CheckFrame = false;
	unsigned SampleCounter = 0;
	std::vector<float> Inst;			// the current merged group's instance data (kInstRegs float4 per member)

	// device resources
	IDirect3DDevice9* Device = nullptr;
	IDirect3DVertexBuffer9* Ring = nullptr;
	UINT RingNext = 0;
	bool RingBroken = false;
	std::unordered_map<IDirect3DVertexShader9*, IDirect3DVertexShader9*> InstancedVS;	// null: refused
	std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> InstancedDecl;	// null: refused
	std::unordered_map<IDirect3DPixelShader9*, bool> NvrPS;
	unsigned RefusalLogs = 0, MismatchLogs = 0, FailLogs = 0;

	// stats (F10 windows)
	constexpr unsigned kWindow = 240;
	unsigned Frames = 0, LastFrame = 0xFFFFFFFF;
	int WindowMode = -1;
	LONGLONG ListTicks = 0, PlanTicks = 0;
	LARGE_INTEGER ListStart = {};
	bool Timed = false;
	unsigned long long SLists = 0, SEntries = 0, SPlannedGroups = 0, SPlannedMembers = 0, SMergedGroups = 0, SMergedCopies = 0,
		SFallbacks = 0, SChecked = 0, SCompared = 0, SMismatch = 0, SLost = 0, SPlainGroups = 0, SProbation = 0, SWhy[WhyCount] = {};
	unsigned long long SGroupSize[5] = {};	// 2, 3, 4-7, 8-15, 16+

	inline UInt64 Mix(UInt64 h, UInt32 v) { return (h ^ v) * 1099511628211ull; }
	inline UInt32 Bits(float f) { UInt32 u; memcpy(&u, &f, 4); return u; }

	// ---- the CPU key of one entry (false: drawn the normal way) ----
	bool KeyOf(const UInt8* pass, Kind kind, UInt64& key) {
		const UInt8* geometry = *(const UInt8* const*)pass;
		if (!geometry) { SWhy[WhyKind]++; return false; }
		const UInt32 vt = *(const UInt32*)geometry;
		if (vt != kVtblTriShape && vt != kVtblTriStrips) { SWhy[WhyKind]++; return false; }
		const NiGeometry* g = (const NiGeometry*)geometry;
		if (g->skinInstance) { SWhy[WhySkin]++; return false; }
		const UInt8* data = (const UInt8*)g->geomData;
		const UInt32 dvt = data ? *(const UInt32*)data : 0;
		if (dvt != kVtblTriShapeData && dvt != kVtblTriStripsData) { SWhy[WhyData]++; return false; }
		const UInt8* prop = (const UInt8*)g->propertyState.m_spShadeProperty;
		if (!prop || *(const UInt32*)prop != kVtblPPLighting || !g->shader) { SWhy[WhyProperty]++; return false; }
		const BSShaderPPLightingProperty* pp = (const BSShaderPPLightingProperty*)prop;
		if (pp->fAlpha != 1.0f || pp->fFadeAlpha != 1.0f) { SWhy[WhyFade]++; return false; }
		NiSourceTexture** const* tex = pp->ppTextures;
		if (!tex[0] || !*tex[0]) { SWhy[WhyProperty]++; return false; }
		// the normal map as ShadowLightShader::GetNormalMap 0x00C03230 picks it (slot 1, else one of the two defaults by flag 0x80)
		UInt32 normal = tex[1] ? (UInt32)*tex[1] : 0;
		if (!normal) normal = *(const UInt32*)((pp->ulFlags[0] & 0x80) ? kDefaultNormalGlossMap : kDefaultNormalMap);
		if (*(const UInt32*)g->shader != kVtblShadowLight) { SWhy[WhyShader]++; return false; }
		// v2: alpha-tested entries (buckets 2-4) are taken: the test's state and reference come from the bucket and the alpha property,
		// which the key holds (v1 left them out: 65 of Freeside's 79 pass-0xFC entries). Blended ones never reach these lists (D1).
		const NiAlphaProperty* alpha = g->propertyState.m_spAlphaProperty;
		if (alpha && (alpha->flags & NiAlphaProperty::ALPHA_BLEND_MASK)) { SWhy[WhyAlpha]++; return false; }
		void* const* lights = *(void* const* const*)(pass + 0x0C);
		const bool needsLight0 = kind == KindOpt || kind == KindLit || (kind == KindAd && ListDeferredMode == 1);	// c25 computed (see Ignore)
		if (needsLight0) {
			if (!pass[9] || !lights || !lights[0] || !*(const void* const*)((const UInt8*)lights[0] + 0xF8)) { SWhy[WhyLight]++; return false; }
			if (kind != KindOpt && ((const UInt8*)lights[0])[0xF4]) { SWhy[WhyLight]++; return false; }	// light 0 a point lamp: not computed (see ComputeAdLight)
		}
		UInt64 h = 14695981039346656037ull;
		h = Mix(h, kind); h = Mix(h, vt); h = Mix(h, (UInt32)data); h = Mix(h, (UInt32)g->shader);
		// batch 3: NVR's per-object c211.w (material class + 8 x floor(world bound radius), the sign the no-highlight code): copies of one
		// mesh at different scales differ there. In the key (exact, and free: a few reads), not in the instance data (that would need a new
		// interpolant read by every pixel shader template). When MaterialForPass sends nothing, c211.w is the last pass's for every copy alike.
		float materialW = 0.0f;
		const bool sendsW = MaterialWOf && MaterialWOf(pass, materialW);
		h = Mix(h, sendsW ? Bits(materialW) : 0x7FC00001u);
		h = Mix(h, (UInt32)*tex[0]); h = Mix(h, normal); h = Mix(h, *(const UInt32*)(prop + 0xD8));	// BSShaderPPLightingProperty::GetClampNI
		h = Mix(h, pp->ulFlags[0]); h = Mix(h, pp->ulFlags[1]); h = Mix(h, pp->uiShaderIndex);
		h = Mix(h, alpha ? 0x10000u | alpha->flags | ((UInt32)alpha->alphaTestRef << 24) : 0);
		if (kind != KindOpt) {
			// the non-Opt setup binds the property's other texture slots (SetupGeometryTextures +0x78) and uploads the material
			// (SetupGeometryConstants_AmbientEmittance 0x00B7BA70): both in the key; the check proves the rest
			for (int t = 2; t < 6; t++) h = Mix(h, tex[t] ? (UInt32)*tex[t] : 0);
			const NiMaterialProperty* m = g->propertyState.m_spMaterialProperty;
			if (m) { h = Mix(h, Bits(m->emit.r)); h = Mix(h, Bits(m->emit.g)); h = Mix(h, Bits(m->emit.b)); h = Mix(h, Bits(m->fAlpha)); h = Mix(h, Bits(m->fEmitMult)); h = Mix(h, Bits(m->fShine)); }
			else h = Mix(h, 0xFFFFFFFFu);
		}
		// NVR's per-object highlight fade (PbrPop::SpecFade, pixel c211.x): copies at different distances differ there
		h = Mix(h, Bits(SpecFadeOf ? SpecFadeOf(geometry) : 1.0f));
		// the game's own highlight fade (property vtable +0x10C): c25.w of the Ad passes (BSShaderLightingProperty::SetLight1x2x 0x00B70820)
		if (kind != KindOpt) h = Mix(h, Bits(((float(__thiscall*)(const void*))(*(const UInt32*)(*(const UInt32*)prop + 0x10C)))(prop)));
		key = h;
		return true;
	}

	void AddGroup(UInt64 key, const std::vector<void*>& run, UInt32 from, UInt32 to, Kind kind, UInt16 passId) {
		const UInt8 ignore = kind == KindOpt ? 0 : kind == KindLit ? (Ig26 | Ig27) : ListDeferredMode == 1 ? (Ig16 | Ig26 | Ig27) : (Ig16 | Ig25 | Ig26 | Ig27);
		while (to - from >= 2) {
			const UInt32 n = (std::min)(to - from, (UInt32)kMaxGroup);
			Group gr = { key, (UInt32)Members.size(), (UInt16)n, 0, StUnseen, false, (UInt8)kind, ignore, passId };
			SKindGroups[kind]++; SKindCopies[kind] += n - 1;
			const UInt32 gi = (UInt32)Groups.size();
			for (UInt32 i = 0; i < n; i++) { Where[run[from + i]] = (UInt32)Members.size(); Members.push_back(run[from + i]); MemberGroup.push_back(gi); }
			Groups.push_back(gr);
			SPlannedGroups++; SPlannedMembers += n;
			SGroupSize[n == 2 ? 0 : n == 3 ? 1 : n < 8 ? 2 : n < 16 ? 3 : 4]++;
			from += n;
		}
	}

	// The planned passes' (KindOfPass) five buckets of the world accumulator's batch renderers, in list order
	// (PassSort's walk: BSBatchRenderer +8 pass groups, +0xC the pass-ID map, +0x2C the active pass-ID list; ENGINE-ANSWERS B6).
	std::vector<void*> Run;
	std::vector<UInt64> RunKeys;
	void Plan(void* accumulator) {
		const UInt8* a = (const UInt8*)accumulator;
		const UInt32 count = *(const UInt32*)(a + 0x18C);
		for (UInt32 r = 0; r < count && r < 2; r++) {
			UInt8* renderer = *(UInt8* const*)(a + 0x174 + 4 * r);
			if (!renderer) continue;
			for (PassSort::PassIdNode* p = (PassSort::PassIdNode*)(renderer + 0x2C); p; p = p->next) {
				const Kind kind = KindOfPass(p->id);
				// batch 4 (where the first passes are, F10 only): the base passes 0x64-0x12B by class, planned or not (the many-lamp ones 0xC3-0xD6
				// apart: with DeferredRenderer live the game draws objects in them in one pass, LampsInOnePass)
				if (Timed && p->id >= 0x64 && p->id <= 0x12B) {
					UInt32 ci = 0;
					if ((UInt8)ThisCall(0x00853130, renderer + 0x0C, (UInt32)p->id, &ci)) {
						const UInt8* cg = *(UInt8**)(renderer + 8) + ci * 0x68;
						const unsigned cls = kind != KindNone ? (unsigned)kind : (p->id >= 0xC3 && p->id <= 0xD6) ? 4u : 5u;
						for (unsigned b = 0; b < 5; b++)
							for (UInt32* node = *(UInt32**)(cg + b * 0x14 + 4); node; node = (UInt32*)node[0])
								if (node[2] && ((const UInt8*)node[2])[7]) SClass[cls]++;
					}
				}
				if (kind == KindNone) continue;
				// batch 4: v2 required the game's own SetupGeometryConstants_Lights at 0x00B7DBAC for the computed c25; in every bench launch
				// another plugin's call is there (it was in the copy of the exe read at the start too), so no Ad (DeferredRenderer 1) or Lit
				// group was ever planned. Now planned whatever is called there: the self-check compares the computed c25 / c16 bit for bit
				// with what that function put on the device, and a key whose values differ stays forward.
				if (kind == KindAd && ListDeferredMode < 1) continue;	// Ad: a deferred record
				if (kind == KindLit && (ListDeferredMode < 2 || !EyeTransformFn)) continue;	// Lit: only the lighting-free records, c16 / c25 computed
				UInt32 index = 0;
				if (!(UInt8)ThisCall(0x00853130, renderer + 0x0C, (UInt32)p->id, &index)) continue;	// NiTMapBase<UInt32, UInt32>::GetAt
				UInt8* group = *(UInt8**)(renderer + 8) + index * 0x68;
				for (unsigned b = 0; b < 5; b++) {
					for (UInt32* node = *(UInt32**)(group + b * 0x14 + 4); node; node = (UInt32*)node[0]) {
						const UInt8* pass = (const UInt8*)node[2];
						if (!pass) { Run.push_back(nullptr); RunKeys.push_back(0); continue; }
						SEntries++;
						UInt64 key = 0;
						bool ok = true;
						if (!pass[7]) { SWhy[WhyAddOn]++; ok = false; }
						else ok = KeyOf(pass, kind, key);
						Run.push_back(ok ? (void*)pass : nullptr);
						RunKeys.push_back(key);
					}
					// groups: consecutive eligible entries with one key (nothing between them: one first-pass run of one bucket)
					UInt32 from = 0;
					for (UInt32 i = 1; i <= (UInt32)Run.size(); i++) {
						if (i < Run.size() && Run[i] && Run[from] && RunKeys[i] == RunKeys[from]) continue;
						if (Run[from]) AddGroup(RunKeys[from], Run, from, i, kind, p->id);
						from = i;
					}
					Run.clear(); RunKeys.clear();
				}
			}
		}
	}

	// ---- per-copy data: c0-c3 and c25 as Setup_ADT_Opt 0x00B7DDE0 computes them, with the engine's own functions ----
	typedef HRESULT(WINAPI* MatMulFn)(D3DXMATRIX*, const D3DXMATRIX*, const D3DXMATRIX*);
	typedef D3DXVECTOR3* (WINAPI* TransformNormalFn)(D3DXVECTOR3*, const D3DXVECTOR3*, const D3DXMATRIX*);
	typedef void(__cdecl* InverseWorldFn)(const void*, int, D3DXMATRIX*);
	typedef D3DXVECTOR3* (WINAPI* NormalizeFn)(D3DXVECTOR3*, const D3DXVECTOR3*);
	// NiPoint3::Unitize 0x004A0C10 (thiscall, no argument): the copy of the exe read here is SSE code returning nothing, while Gamebryo's returns
	// the length on the x87 stack, and another plugin may have replaced it: whatever runs, the x87 stack is left as it was (the status word's
	// TOP before and after the call; a value it pushed is popped).
	void CallUnitize(float* v) {
		unsigned short before = 0, after = 0;
		const UInt32 fn = kUnitize;
		__asm {
			fnstsw before
			mov ecx, v
			mov eax, fn
			call eax
			fnstsw after
		}
		if (((before ^ after) >> 11) & 7) { __asm fstp st(0) }
	}
	// v2, the Ad passes with the lamps-deferred record: c25 = LightData[0], light 0 (the cell's directional light) as
	// SetupGeometryConstants_Lights 0x00B78A90 makes it for a light that is not a point light (ShadowSceneLight +0xF4 = 0): its direction
	// (NiLight +0xF0) unitised (NiPoint3::Unitize, in BSShaderLightingProperty::SetLight1x2x 0x00B70820, which keeps it in
	// kShaderLightConstants[0x13]), negated (fchs: exact), through the inverse world matrix (the exe's D3DXVec3TransformNormal) and normalised
	// (D3DXVec3Normalize); w = the property's highlight fade (vtable +0x10C; 1 for a non-lighting property), as SetLight1x2x stores it.
	void ComputeAdLight(const UInt8* pass, const D3DXMATRIX& inverse, float* out4) {
		void* const* lights = *(void* const* const*)(pass + 0x0C);
		const UInt8* light = *(const UInt8* const*)((const UInt8*)lights[0] + 0xF8);
		float dir[3] = { ((const float*)(light + 0xF0))[0], ((const float*)(light + 0xF0))[1], ((const float*)(light + 0xF0))[2] };
		CallUnitize(dir);
		const D3DXVECTOR3 v(-dir[0], -dir[1], -dir[2]);
		D3DXVECTOR3 t, n;
		((TransformNormalFn)kVec3TransformNormal)(&t, &v, &inverse);
		((NormalizeFn)kVec3Normalize)(&n, &t);
		const UInt8* geometry = *(const UInt8* const*)pass;
		const UInt8* prop = (const UInt8*)((const NiGeometry*)geometry)->propertyState.m_spShadeProperty;
		const UInt32 type = prop ? *(const UInt32*)(prop + 0x1C) : 0;
		const float fade = (prop && type >= 8 && type <= 12) ? ((float(__thiscall*)(const void*))(*(const UInt32*)(*(const UInt32*)prop + 0x10C)))(prop) : 1.0f;
		out4[0] = n.x; out4[1] = n.y; out4[2] = n.z; out4[3] = fade;
	}

	// v2, the Lit passes: c16 = EyePosition as ShadowLightShader::SetupGeometryConstants_EyePos 0x00B78EF0 makes it for a pass outside the
	// env-map range 0x244-0x24B: the camera's world position (0x011FA2A0) through the inverse world matrix with the function the game calls at
	// 0x00B79019 (D3DXVec3TransformCoord's place: in the copy of the exe read here another plugin's direct call; read at run time), w = the
	// world transform's scale. (It keeps it in kEyePosition; the pass's constant map uploads it to c16.)
	typedef D3DXVECTOR3* (WINAPI* TransformCoordFn)(D3DXVECTOR3*, const D3DXVECTOR3*, const D3DXMATRIX*);
	void ComputeEye(const UInt8* geometry, const D3DXMATRIX& inverse, float* out4) {
		const float* cam = (const float*)0x011FA2A0;
		const D3DXVECTOR3 c(cam[0], cam[1], cam[2]);
		D3DXVECTOR3 e;
		((TransformCoordFn)EyeTransformFn)(&e, &c, &inverse);
		out4[0] = e.x; out4[1] = e.y; out4[2] = e.z; out4[3] = *(const float*)(geometry + 0x68 + 0x30);	// NiTransform::m_fScale
	}

	// The instance layout (kInstRegs float4 a copy): c0-c3, c16, c25 (offsets 0, 16, 20 in floats). out[16..23] are filled by the caller
	// with the first entry's c16 / c25; this overwrites what the copy computes: Opt c25.xyz (w stays the first entry's); Ad with the
	// lamps-deferred record c25; Lit c16 and c25.
	void Compute(const UInt8* pass, float* out, UInt8 kind) {
		const UInt8* geometry = *(const UInt8* const*)pass;
		const void* transform = geometry + 0x68;	// NiAVObject::m_worldTransform
		UInt8* renderer = *(UInt8**)((const UInt8*)((const NiGeometry*)geometry)->shader + 0x18);
		// ShadowLightShader::SetupGeometryOpt_WorldViewProj 0x00B7C750
		ThisCall(kSetModelTransform, renderer, transform, 0);
		D3DXMATRIX wvp;
		((MatMulFn)kMatrixMultiplyTranspose)(&wvp, (const D3DXMATRIX*)(renderer + 0x940), (const D3DXMATRIX*)(renderer + 0xA00));
		memcpy(out, &wvp, 64);
		// BSShader::SetLightDirection 0x00B7C7B0: the pass's first lamp's direction (+0xF8 its NiLight, +0xF0 the world direction) times
		// -1.0 (the exe's constant at 0x0101A6B0: exact), through the inverse world matrix (not normalised: the uploaded vector is the
		// transformed one; the normalised copy the function also makes is not what it uploads)
		if (kind == KindAd && ListDeferredMode != 1) return;	// c16 / c25 not read by the lighting-free record (Ignore)
		D3DXMATRIX inverse;
		((InverseWorldFn)kGetInverseWorldMatrix)(transform, 0, &inverse);	// (the non-Opt setup: the same call, 0x00B7DB48)
		if (kind == KindAd) { ComputeAdLight(pass, inverse, out + 20); return; }
		if (kind == KindLit) { ComputeEye(geometry, inverse, out + 16); ComputeAdLight(pass, inverse, out + 20); return; }
		void* const* lights = *(void* const* const*)(pass + 0x0C);
		const float* dir = (const float*)(*(const UInt8* const*)((const UInt8*)lights[0] + 0xF8) + 0xF0);
		const D3DXVECTOR3 v(-dir[0], -dir[1], -dir[2]);
		D3DXVECTOR3 r;
		((TransformNormalFn)kVec3TransformNormal)(&r, &v, &inverse);
		out[20] = r.x; out[21] = r.y; out[22] = r.z;
	}
	void RestoreModelTransform(const UInt8* pass) {
		const UInt8* geometry = *(const UInt8* const*)pass;
		UInt8* renderer = *(UInt8**)((const UInt8*)((const NiGeometry*)geometry)->shader + 0x18);
		ThisCall(kSetModelTransform, renderer, geometry + 0x68, 0);
	}

	// ---- the instanced vertex shader / declaration (October's deep/batchrender Instancer.h, lazily compiled) ----
	bool SameRegisters(const ShaderRecord* original, const ShaderRecord* instanced, char* why, size_t whySize) {
		if (original->FloatShaderValuesCount != instanced->FloatShaderValuesCount || original->TextureShaderValuesCount != instanced->TextureShaderValuesCount) {
			sprintf_s(why, whySize, "TESR_ constants %u/%u, samplers %u/%u", original->FloatShaderValuesCount, instanced->FloatShaderValuesCount,
				original->TextureShaderValuesCount, instanced->TextureShaderValuesCount);
			return false;
		}
		for (UInt32 i = 0; i < original->FloatShaderValuesCount; i++)
			if (original->FloatShaderValues[i].RegisterIndex != instanced->FloatShaderValues[i].RegisterIndex ||
				original->FloatShaderValues[i].RegisterCount != instanced->FloatShaderValues[i].RegisterCount) {
				sprintf_s(why, whySize, "TESR_ constant %u at c%u (original c%u)", i, instanced->FloatShaderValues[i].RegisterIndex, original->FloatShaderValues[i].RegisterIndex);
				return false;
			}
		for (UInt32 i = 0; i < original->TextureShaderValuesCount; i++)
			if (original->TextureShaderValues[i].RegisterIndex != instanced->TextureShaderValues[i].RegisterIndex) {
				sprintf_s(why, whySize, "sampler %u at s%u (original s%u)", i, instanced->TextureShaderValues[i].RegisterIndex, original->TextureShaderValues[i].RegisterIndex);
				return false;
			}
		return true;
	}

	IDirect3DVertexShader9* GetInstancedVS(IDirect3DVertexShader9* bound) {
		auto it = InstancedVS.find(bound);
		if (it != InstancedVS.end()) return it->second;
		InstancedVS[bound] = nullptr;
		const char* name = nullptr; int type = -1; const ShaderRecord* original = nullptr;
		for (const auto& [CollectionName, Pointer] : TheShaderManager->ShaderNames) {
			ShaderCollection* Collection = Pointer ? *Pointer : nullptr;
			if (!Collection) continue;
			for (NiD3DVertexShaderEx* Shader : Collection->VertexShaderList)
				for (int i = 0; Shader && Shader->Name && i < 3; i++) {
					ShaderRecordVertex* R = Shader->GetShaderRecord((ShaderRecordType)i);
					if (R && R->ShaderHandle == bound) { name = Shader->Name; type = i; original = R; }
				}
		}
		if (!name) { if (RefusalLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: vertex shader %08X is not one of Vegas Shaders' records: its copies are drawn one by one", (UInt32)bound); return nullptr; }
		ShaderCollection* collection = TheShaderManager->GetShaderCollection(name);
		ShaderTemplate t = collection ? collection->GetTemplate(name) : ShaderTemplate{};
		if (!t.Name || strcmp(t.Name, "ObjectTemplate")) { if (RefusalLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: %s is not an ObjectTemplate shader: drawn one by one", name); return nullptr; }
		static const D3DXMACRO instanced[] = { { "NVR_INSTANCED", "1" }, { NULL, NULL } };
		static const char* const sub[3] = { NULL, "Exteriors\\", "Interiors\\" };
		static const char* const cache[3] = { "Instanced\\", "InstancedExt\\", "InstancedInt\\" };
		ShaderRecordVertex* record = (ShaderRecordVertex*)ShaderRecord::LoadShader(name, sub[type], t, cache[type], instanced);
		if (!record || !record->ShaderHandle) { if (RefusalLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: %s: the instanced copy did not compile (see above): drawn one by one", name); return nullptr; }
		char why[160] = "";
		if (!SameRegisters(original, record, why, sizeof(why))) {
			if (RefusalLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: %s: the instanced copy's constants moved (%s): drawn one by one", name, why);
			return nullptr;
		}
		// batch 3 (the round-3 review found c16 / c25 read from each other's stream slot): the copy's inputs at load. It must take the matrix
		// rows from TEXCOORD8-11 and nothing past the last slot this fills (TEXCOORD8 + kInstRegs - 1), and must no longer read c0-c3 or c16
		// from constants (its constant table). Which TEXCOORD feeds which register is fixed in one table, InstReg / ObjectTemplate.hlsl's
		// NVR_INSTANCED inputs, and tested offline against the compiled code (tests/merge_instanced_layout.py).
		{
			UINT size = 0;
			std::vector<DWORD> code;
			D3DXSEMANTIC sem[MAXD3DDECLLENGTH] = {};
			UINT count = MAXD3DDECLLENGTH;
			bool ok = SUCCEEDED(record->ShaderHandle->GetFunction(nullptr, &size)) && size >= 8;
			if (ok) { code.resize(size / 4); ok = SUCCEEDED(record->ShaderHandle->GetFunction(code.data(), &size)) && SUCCEEDED(D3DXGetShaderInputSemantics(code.data(), sem, &count)); }
			unsigned rows = 0;
			for (UINT i = 0; ok && i < count; i++) {
				if (sem[i].Usage != D3DDECLUSAGE_TEXCOORD || sem[i].UsageIndex < 8) continue;
				if (sem[i].UsageIndex >= 8 + kInstRegs) { ok = false; break; }
				if (sem[i].UsageIndex < 12) rows++;
			}
			UInt32 mask[8] = {};
			GBufferCensus::RegistersRead(record->ShaderHandle, mask);
			const bool constants = (mask[0] & 0x1000F) == 0;	// c0-c3 and c16 not read
			if (!ok || rows != 4 || !constants) {
				if (RefusalLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: %s: the instanced copy's inputs are not the instance layout (matrix rows %u of 4, inputs %s, c0-c3 / c16 %s): drawn one by one",
					name, rows, ok ? "in range" : "past the layout or unreadable", constants ? "from the stream" : "still read from constants");
				return nullptr;
			}
		}
		Logger::Log("UNOFFICIAL MergeCopies: %s (record %d): instanced copy ready (inputs checked)", name, type);
		InstancedVS[bound] = record->ShaderHandle;
		return record->ShaderHandle;
	}

	IDirect3DVertexDeclaration9* GetInstancedDecl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl) {
		auto it = InstancedDecl.find(decl);
		if (it != InstancedDecl.end()) return it->second;
		InstancedDecl[decl] = nullptr;
		D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1 + kInstRegs];
		UINT n = 0;
		if (!decl || FAILED(decl->GetDeclaration(nullptr, &n)) || n < 1 || n + kInstRegs > MAXD3DDECLLENGTH + 1 || FAILED(decl->GetDeclaration(e, &n))) return nullptr;
		n--;	// without D3DDECL_END
		for (UINT i = 0; i < n; i++)
			if (e[i].Stream != 0 || (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex >= 8)) {
				if (RefusalLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: declaration %08X uses stream %u / TEXCOORD%u: drawn one by one", (UInt32)decl, e[i].Stream, e[i].UsageIndex);
				return nullptr;
			}
		for (UInt32 k = 0; k < kInstRegs; k++)
			e[n + k] = { 1, (WORD)(k * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, (BYTE)(8 + k) };
		e[n + kInstRegs] = D3DDECL_END();
		IDirect3DVertexDeclaration9* made = nullptr;
		if (FAILED(d->CreateVertexDeclaration(e, &made))) return nullptr;
		InstancedDecl[decl] = made;
		return made;
	}

	bool IsNvrPS(IDirect3DPixelShader9* ps) {
		// batch 4: DeferredRenderer's records (the lamps-deferred and lighting-free ones) are loaded outside the families' record lists, so
		// with the renderer live every merged draw fell back here ("a pixel shader not in Vegas Shaders' records", 9-16 groups a window)
		if (DeferredPS && DeferredPS(ps)) return true;
		auto it = NvrPS.find(ps);
		if (it != NvrPS.end()) return it->second;
		bool found = false;
		for (const auto& [CollectionName, Pointer] : TheShaderManager->ShaderNames) {
			ShaderCollection* Collection = Pointer ? *Pointer : nullptr;
			if (!Collection) continue;
			for (NiD3DPixelShaderEx* Shader : Collection->PixelShaderList)
				for (int i = 0; Shader && !found && i < 3; i++) { ShaderRecordPixel* R = Shader->GetShaderRecord((ShaderRecordType)i); if (R && R->ShaderHandle == ps) found = true; }
		}
		return NvrPS[ps] = found;
	}

	// ---- the self-check's picture of one draw ----
	const D3DRENDERSTATETYPE kStates[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
		D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_BLENDFACTOR, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
		D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFUNC,
		D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_CULLMODE,
		D3DRS_FILLMODE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_SRGBWRITEENABLE,
		D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_MULTISAMPLEANTIALIAS };
	const unsigned kStateCount = sizeof(kStates) / sizeof(kStates[0]);
	const D3DSAMPLERSTATETYPE kSampler[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
		D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_SRGBTEXTURE, D3DSAMP_BORDERCOLOR };
	const unsigned kSamplerCount = sizeof(kSampler) / sizeof(kSampler[0]);

	struct Snap {
		void* vs; void* ps; void* decl; void* ib; DWORD fvf;
		void* vb[2]; UINT off[2], stride[2], freq[2];
		UInt32 args[6];
		void* tex[16]; DWORD ss[16][kSamplerCount];
		DWORD rs[kStateCount]; void* rt[4]; void* ds;
		float vc[256][4]; float pc[224][4];
		UInt32 vmask[8], pmask[8];
	};
	Snap Ref, Cur;
	bool RefValid = false;

	template<class T> void* Handle(T* p) { if (p) p->Release(); return p; }
	void Take(IDirect3DDevice9* d, Snap& s, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT st, UINT pc) {
		IDirect3DVertexShader9* vs = nullptr; d->GetVertexShader(&vs); s.vs = Handle(vs);
		IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); s.ps = Handle(ps);
		IDirect3DVertexDeclaration9* decl = nullptr; d->GetVertexDeclaration(&decl); s.decl = Handle(decl);
		IDirect3DIndexBuffer9* ib = nullptr; d->GetIndices(&ib); s.ib = Handle(ib);
		s.fvf = 0; d->GetFVF(&s.fvf);
		for (UINT k = 0; k < 2; k++) {
			IDirect3DVertexBuffer9* vb = nullptr; s.off[k] = s.stride[k] = 0; s.freq[k] = 1;
			d->GetStreamSource(k, &vb, &s.off[k], &s.stride[k]); s.vb[k] = Handle(vb);
			d->GetStreamSourceFreq(k, &s.freq[k]);
		}
		const UInt32 a[6] = { (UInt32)t, (UInt32)b, mi, nv, st, pc };
		memcpy(s.args, a, sizeof(a));
		for (DWORD k = 0; k < 16; k++) {
			IDirect3DBaseTexture9* tx = nullptr; d->GetTexture(k, &tx); s.tex[k] = Handle(tx);
			for (unsigned j = 0; j < kSamplerCount; j++) { s.ss[k][j] = 0; if (s.tex[k]) d->GetSamplerState(k, kSampler[j], &s.ss[k][j]); }
		}
		for (unsigned j = 0; j < kStateCount; j++) { s.rs[j] = 0; d->GetRenderState(kStates[j], &s.rs[j]); }
		for (DWORD k = 0; k < 4; k++) { IDirect3DSurface9* rt = nullptr; d->GetRenderTarget(k, &rt); s.rt[k] = Handle(rt); }
		IDirect3DSurface9* ds = nullptr; d->GetDepthStencilSurface(&ds); s.ds = Handle(ds);
		if (FAILED(d->GetVertexShaderConstantF(0, &s.vc[0][0], 256))) memset(s.vc, 0xFF, sizeof(s.vc));
		if (FAILED(d->GetPixelShaderConstantF(0, &s.pc[0][0], 224))) memset(s.pc, 0xFF, sizeof(s.pc));
		GBufferCensus::RegistersRead((IDirect3DVertexShader9*)s.vs, s.vmask);
		GBufferCensus::RegistersRead((IDirect3DPixelShader9*)s.ps, s.pmask);
	}
	inline bool IsInstReg(UInt32 r) { return r < 4 || r == 16 || r == 25; }
	inline UInt32 InstReg(UInt32 k) { return k < 4 ? k : k == 4 ? 16 : 25; }	// instance slot k -> its register
	inline bool Used(const UInt32* m, UInt32 r) { return (m[r >> 5] >> (r & 31)) & 1; }

	// false on the first difference (why: what). expected: this copy's c0-c3 / c25 (5 float4).
	// ignore: the Ignore bits of registers the bound record does not read (not compared, see Ignore).
	inline bool Ignored(UInt8 ignore, UInt32 r) {
		return (r == 16 && (ignore & Ig16)) || (r == 25 && (ignore & Ig25)) || (r == 26 && (ignore & Ig26)) || (r == 27 && (ignore & Ig27));
	}
	bool Same(const Snap& a, const Snap& b, const float* expected, UInt8 ignore, char* why, size_t size) {
		if (a.vs != b.vs || a.ps != b.ps) { sprintf_s(why, size, "shaders %08X/%08X vs %08X/%08X", (UInt32)a.vs, (UInt32)a.ps, (UInt32)b.vs, (UInt32)b.ps); return false; }
		if (a.decl != b.decl || a.fvf != b.fvf || a.ib != b.ib) { sprintf_s(why, size, "declaration / indices"); return false; }
		// v2 (batch 2): an unbound stream's offset / stride / frequency mean nothing, and were what every live check of v1 failed on ("stream 1":
		// what a runtime reports for an empty stream after a buffer was bound and unbound there); stream 0's frequency is always compared
		for (int k = 0; k < 2; k++) {
			if (k == 0 && a.freq[0] != b.freq[0]) { sprintf_s(why, size, "stream 0 frequency %u vs %u", a.freq[0], b.freq[0]); return false; }
			if (!a.vb[k] && !b.vb[k]) continue;
			if (a.vb[k] != b.vb[k] || a.off[k] != b.off[k] || a.stride[k] != b.stride[k] || a.freq[k] != b.freq[k])
				{ sprintf_s(why, size, "stream %d (%08X +%u /%u f%u vs %08X +%u /%u f%u)", k, (UInt32)a.vb[k], a.off[k], a.stride[k], a.freq[k], (UInt32)b.vb[k], b.off[k], b.stride[k], b.freq[k]); return false; }
		}
		if (memcmp(a.args, b.args, sizeof(a.args))) { sprintf_s(why, size, "draw arguments %u %d %u %u %u %u vs %u %d %u %u %u %u", a.args[0], (int)a.args[1], a.args[2], a.args[3], a.args[4], a.args[5],
			b.args[0], (int)b.args[1], b.args[2], b.args[3], b.args[4], b.args[5]); return false; }
		for (int k = 0; k < 16; k++) {
			if (a.tex[k] != b.tex[k]) { sprintf_s(why, size, "texture s%d", k); return false; }
			for (unsigned j = 0; j < kSamplerCount; j++) if (a.ss[k][j] != b.ss[k][j]) { sprintf_s(why, size, "sampler s%d state %u: %u vs %u", k, (unsigned)kSampler[j], a.ss[k][j], b.ss[k][j]); return false; }
		}
		for (unsigned j = 0; j < kStateCount; j++) if (a.rs[j] != b.rs[j]) { sprintf_s(why, size, "render state %u: %u vs %u", (unsigned)kStates[j], a.rs[j], b.rs[j]); return false; }
		for (int k = 0; k < 4; k++) if (a.rt[k] != b.rt[k]) { sprintf_s(why, size, "render target %d", k); return false; }
		if (a.ds != b.ds) { sprintf_s(why, size, "depth surface"); return false; }
		for (UInt32 r = 0; r < 256; r++) {
			if (IsInstReg(r) || Ignored(ignore, r)) continue;
			if (Used(a.vmask, r) && memcmp(a.vc[r], b.vc[r], 16)) { sprintf_s(why, size, "VS c%u (%g %g %g %g vs %g %g %g %g)", r, a.vc[r][0], a.vc[r][1], a.vc[r][2], a.vc[r][3], b.vc[r][0], b.vc[r][1], b.vc[r][2], b.vc[r][3]); return false; }
		}
		for (UInt32 r = 0; r < 224; r++)
			if (Used(a.pmask, r) && memcmp(a.pc[r], b.pc[r], 16)) { sprintf_s(why, size, "PS c%u (%g %g %g %g vs %g %g %g %g)", r, a.pc[r][0], a.pc[r][1], a.pc[r][2], a.pc[r][3], b.pc[r][0], b.pc[r][1], b.pc[r][2], b.pc[r][3]); return false; }
		// the per-copy registers: what the engine set for this copy, bit for bit, against what this code computed (or used)
		for (UInt32 k = 0; k < kInstRegs; k++) {
			const UInt32 r = InstReg(k);
			if (Ignored(ignore, r)) continue;
			if (memcmp(b.vc[r], expected + k * 4, 16)) {
				const float* e = expected + k * 4;
				sprintf_s(why, size, "per-copy VS c%u: engine %.9g %.9g %.9g %.9g, computed %.9g %.9g %.9g %.9g", r, b.vc[r][0], b.vc[r][1], b.vc[r][2], b.vc[r][3], e[0], e[1], e[2], e[3]);
				return false;
			}
		}
		return true;
	}

	// The registers a check may skip: the group's Ignore bits, only while the first entry's draw had a deferred record bound (past 0xFC).
	UInt8 CheckIgnore(const Group& g) {
		return g.kind != KindOpt && DeferredPS && DeferredPS((IDirect3DPixelShader9*)Ref.ps) ? g.ignore : 0;
	}

	void Blacklist(Group& g, const char* what, UInt32 index) {
		KeyState& ks = Keys[g.key];
		if (!ks.blacklisted) { ks.blacklisted = true; BlacklistedKeys++; if (ks.proven) { ks.proven = false; ProvenKeys--; } }
		g.checkFailed = true;
		SMismatch++;
		if (MismatchLogs++ < 40)
			Logger::Log("MERGE CHECK mismatch mode %d key %08X%08X pass 0x%X (%s, deferred %d) copy %u of %u: %s: that key is drawn the normal way for the rest of the session%s",
				Mode, (UInt32)(g.key >> 32), (UInt32)g.key, (unsigned)g.pass, KindNames[g.kind & 3], ListDeferredMode, index, (unsigned)g.count, what, MismatchLogs == 40 ? " (no more mismatch lines)" : "");
	}

	void GroupChecked(Group& g) {
		if (g.checkFailed) return;
		KeyState& ks = Keys[g.key];
		if (ks.blacklisted) return;
		if (ks.checks < 0xFFFF) ks.checks++;
		if (!ks.proven && ks.checks >= kProve) { ks.proven = true; ProvenKeys++; }
	}

	void Report() {
		if (!Frames) return;
		LARGE_INTEGER f; QueryPerformanceFrequency(&f);
		const double ms = 1000.0 / (double)f.QuadPart / Frames, n = (double)Frames;
		char why[256] = ""; size_t used = 0;
		for (int i = 0; i < WhyCount; i++) if (SWhy[i]) used += sprintf_s(why + used, sizeof(why) - used, "%s%s %.0f", used ? ", " : "", WhyNames[i], SWhy[i] / n);
		Logger::Log("MERGE COPIES mode %d, %u frames, deferred %d, per frame: main list hook %.3f ms (of it planning %.3f ms); lists %.2f, planned-pass entries %.0f, planned groups %.1f (%.0f entries; sizes 2: %.1f, 3: %.1f, 4-7: %.1f, 8-15: %.1f, 16+: %.1f; planned copies opt %.1f, ad %.1f, lit %.1f); merged groups %.1f, copies merged %.1f (opt %.1f, ad %.1f, lit %.1f; = draws saved = engine setup calls skipped), groups drawn plain %.1f (of them probation %.1f), fallbacks %.2f, lost copies %.2f; checks: groups %.2f, copies compared %.1f, mismatches %.2f; keys proven %u, blacklisted %u (session); not eligible per frame: %s",
			WindowMode, Frames, ListDeferredMode, ListTicks * ms, PlanTicks * ms, SLists / n, SEntries / n, SPlannedGroups / n, SPlannedMembers / n,
			SGroupSize[0] / n, SGroupSize[1] / n, SGroupSize[2] / n, SGroupSize[3] / n, SGroupSize[4] / n, SKindCopies[1] / n, SKindCopies[2] / n, SKindCopies[3] / n,
			SMergedGroups / n, SMergedCopies / n, SKindMerged[1] / n, SKindMerged[2] / n, SKindMerged[3] / n, SPlainGroups / n, SProbation / n, SFallbacks / n, SLost / n, SChecked / n, SCompared / n, SMismatch / n,
			ProvenKeys, BlacklistedKeys, used ? why : "-");
		Frames = 0; ListTicks = PlanTicks = 0;
		SLists = SEntries = SPlannedGroups = SPlannedMembers = SMergedGroups = SMergedCopies = SFallbacks = SChecked = SCompared = SMismatch = SLost = SPlainGroups = SProbation = 0;
		memset(SWhy, 0, sizeof(SWhy)); memset(SGroupSize, 0, sizeof(SGroupSize));
		memset(SKindGroups, 0, sizeof(SKindGroups)); memset(SKindCopies, 0, sizeof(SKindCopies)); memset(SKindMerged, 0, sizeof(SKindMerged));
		Logger::Log("MERGE CLASSES per frame: first-pass entries of the base passes 0x64-0x12B: opt %.1f, ad %.1f, lit %.1f, many-lamp (0xC3-0xD6, not planned) %.1f, other %.1f; lamp setup call %s",
			SClass[1] / n, SClass[2] / n, SClass[3] / n, SClass[4] / n, SClass[5] / n, LightsCallOk ? "the game's" : "another plugin's");
		memset(SClass, 0, sizeof(SClass));
	}

	// The merged draw: the leader's state is on the device (the engine's setup and every NVR per-draw step ran for it).
	HRESULT DrawMerged(IDirect3DDevice9* d, DrawFn next, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT pc) {
		Group& g = *Current;
		const auto plain = [&](const char* why) -> HRESULT {
			FellBack = true;
			if (FailLogs++ < 20) Logger::Log("UNOFFICIAL MergeCopies: a group of %u drawn one by one: %s", (unsigned)g.count, why);
			return next(d, t, b, mi, nv, s, pc);
		};
		if (RingBroken) return plain("the instance buffer could not be made");
		IDirect3DVertexShader9* vs = nullptr; d->GetVertexShader(&vs); if (vs) vs->Release();
		IDirect3DVertexDeclaration9* decl = nullptr; d->GetVertexDeclaration(&decl); if (decl) decl->Release();
		IDirect3DPixelShader9* ps = nullptr; d->GetPixelShader(&ps); if (ps) ps->Release();
		DWORD fvf = 0; d->GetFVF(&fvf);
		UINT freq0 = 1; d->GetStreamSourceFreq(0, &freq0);
		if (!vs || !decl || freq0 != 1) return plain("no declaration / vertex shader, or stream 0 already instanced");
		if (!ps || !IsNvrPS(ps)) return plain("a pixel shader not in Vegas Shaders' records");
		// v2: past 0xFC the registers left as the first entry's (Ignore) are unread only by a deferred record: anything else, one by one
		if (g.kind != KindOpt && !(DeferredPS && DeferredPS(ps))) return plain("a pass past 0xFC without a deferred record bound");
		IDirect3DVertexShader9* ivs = GetInstancedVS(vs);
		IDirect3DVertexDeclaration9* idecl = ivs ? GetInstancedDecl(d, decl) : nullptr;
		if (!ivs || !idecl) { FellBack = true; return next(d, t, b, mi, nv, s, pc); }	// (refusals are logged once per shader / declaration)
		const UInt32 n = g.count;
		Inst.resize(n * kInstRegs * 4);
		float lead[kInstRegs][4];
		if (FAILED(d->GetVertexShaderConstantF(0, &lead[0][0], 4)) || FAILED(d->GetVertexShaderConstantF(16, lead[4], 1)) ||
			FAILED(d->GetVertexShaderConstantF(25, lead[5], 1))) return plain("the device's vertex constants cannot be read");
		memcpy(&Inst[0], lead, sizeof(lead));
		for (UInt32 i = 1; i < n; i++) {
			float* o = &Inst[i * kInstRegs * 4];
			memcpy(o + 16, lead[4], 32);	// c16 / c25 as the first entry's where this does not compute them (see Compute and Ignore)
			Compute((const UInt8*)Members[g.first + i], o, g.kind);
		}
		RestoreModelTransform((const UInt8*)Members[g.first]);	// the renderer's model state as the leader's setup left it
		if (g.state == StMergedCheck) { Take(d, Ref, t, b, mi, nv, s, pc); RefValid = true; }
		if (!Ring) {
			if (FAILED(d->CreateVertexBuffer(kRingInstances * kInstBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &Ring, nullptr))) {
				Ring = nullptr; RingBroken = true; return plain("the instance buffer could not be made");
			}
			RingNext = 0;
		}
		DWORD flags = D3DLOCK_NOOVERWRITE;
		if (RingNext + n > kRingInstances) { RingNext = 0; flags = D3DLOCK_DISCARD; }
		void* p = nullptr;
		if (FAILED(Ring->Lock(RingNext * kInstBytes, n * kInstBytes, &p, flags)) || !p) return plain("the instance buffer could not be locked");
		memcpy(p, Inst.data(), n * kInstBytes);
		Ring->Unlock();
		IDirect3DVertexBuffer9* oldVB = nullptr; UINT oldOff = 0, oldStride = 0, oldFreq1 = 1;
		d->GetStreamSource(1, &oldVB, &oldOff, &oldStride);
		d->GetStreamSourceFreq(1, &oldFreq1);
		d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | n);
		d->SetStreamSource(1, Ring, RingNext * kInstBytes, kInstBytes);
		d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
		d->SetVertexDeclaration(idecl);
		d->SetVertexShader(ivs);
		const HRESULT r = next(d, t, b, mi, nv, s, pc);
		d->SetVertexShader(vs);
		d->SetVertexDeclaration(decl);
		d->SetStreamSourceFreq(1, oldFreq1);
		d->SetStreamSource(1, oldVB, oldOff, oldStride);
		d->SetStreamSourceFreq(0, 1);
		if (oldVB) oldVB->Release();
		RingNext += n;
		SMergedGroups++; SMergedCopies += n - 1; SKindMerged[g.kind & 3] += n - 1;
		return r;
	}
}

bool BeginFrame(IDirect3DDevice9* d) {
	Mode = TheSettingManager->GetSettingI("Main.Main.Performance", "MergeCopies");
	if (Mode < 0 || Mode > 2) Mode = 0;
	Device = d;
	const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
	CheckFrame = Mode && (frame % kCheckEvery) == 0;
	// 2026-11-14 fd-fixes: off, the main list hook does nothing (ListBegin returns on Mode): the window a switch-off ends is
	// reported here once, and nothing is timed or logged while it stays off
	if (!Mode && WindowMode != 0) { if (Frames) Report(); WindowMode = 0; Frames = 0; ListTicks = PlanTicks = 0; }
	return Mode != 0;
}

void ListBegin(void* accumulator, bool main, bool world, bool excluded) {
	Timed = false; InList = false;
	if (!Mode) return;	// off: one flag test (Mode is read once per frame in BeginFrame)
	const int mode = Mode;
	const unsigned frame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
	if (GpuTimer::Enabled) {
		if (mode != WindowMode) { Report(); WindowMode = mode; }
		if (frame != LastFrame) { LastFrame = frame; if (Frames >= kWindow) Report(); Frames++; }
		Timed = true;
		QueryPerformanceCounter(&ListStart);
	}
	if (excluded) return;
	const UInt8* a = (const UInt8*)accumulator;
	if (*(const UInt32*)(a + 0x19C) != 0 || a[0x164] || a[0x165]) return;	// standard render mode, not the water passes
	if (GBufferCensus::Active) return;	// census frames: the plain list
	if (!LightsCallChecked) {	// v2: the Ad / Lit passes' c25 is computed as SetupGeometryConstants_Lights makes it (batch 4: logged, no longer required; the check decides)
		LightsCallChecked = true;
		const UInt8* site = (const UInt8*)kLightsCallSite;
		const UInt32 target = site[0] == 0xE8 ? kLightsCallSite + 5 + *(const SInt32*)(site + 1) : 0;
		LightsCallOk = target == kSetupLights;
		if (!LightsCallOk) Logger::Log("UNOFFICIAL MergeCopies: the call at %08X goes to %08X, not the game's SetupGeometryConstants_Lights (another plugin's): the lit and light-only passes are planned anyway; the self-check proves the computed c25 against what it uploads", kLightsCallSite, target);
		const UInt8* eye = (const UInt8*)0x00B79019;	// SetupGeometryConstants_EyePos: the transform of the camera position (see ComputeEye)
		EyeTransformFn = eye[0] == 0xE8 ? 0x00B79019 + 5 + *(const SInt32*)(eye + 1) : 0;
		Logger::Log("UNOFFICIAL MergeCopies v2: lamp setup call %s; eye transform call %08X%s", LightsCallOk ? "the game's" : "replaced", EyeTransformFn,
			EyeTransformFn ? "" : " (not a call: the lit passes past AD stay off)");
	}
	ListDeferredMode = DeferredMode ? DeferredMode() : 0;
	LARGE_INTEGER t0; if (Timed) QueryPerformanceCounter(&t0);
	Members.clear(); MemberGroup.clear(); Where.clear(); Groups.clear();
	Plan(accumulator);
	SLists++;
	if (Timed) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); PlanTicks += t1.QuadPart - t0.QuadPart; }
	InList = !Groups.empty();
}

void ListEnd() {
	if (InList) {
		for (Group& g : Groups) {
			if ((g.state == StMerged || g.state == StMergedCheck) && g.arrived + 1u < g.count) {
				// a merged copy whose entry never came: the instanced draw drew something the game would not have this frame
				SLost += g.count - 1u - g.arrived;
				Blacklist(g, "copies whose entries never came after the merged draw", g.arrived + 1u);
			}
			if ((g.state == StCheck || g.state == StMergedCheck) && g.arrived + 1u >= g.count) GroupChecked(g);
		}
		Members.clear(); MemberGroup.clear(); Where.clear(); Groups.clear();
	}
	InList = false;
	DrawAction = ActNone; Current = nullptr;
	if (Timed) { LARGE_INTEGER end; QueryPerformanceCounter(&end); ListTicks += end.QuadPart - ListStart.QuadPart; Timed = false; }
}

void Entry(void* pass, UInt32 a2, UInt32 a3, UInt32 a4, StandardFn standard) {
	if (DrawAction != ActNone || !pass || KindOfPass(((const UInt16*)pass)[2]) == KindNone) { standard(pass, a2, a3, a4); return; }
	auto it = Where.find(pass);
	if (it == Where.end()) { standard(pass, a2, a3, a4); return; }
	const UInt32 m = it->second;
	Group& g = Groups[MemberGroup[m]];
	const UInt32 i = m - g.first;
	const auto run = [&](unsigned char action) {
		Current = &g; CurrentIndex = i; DrawAction = action;
		standard(pass, a2, a3, a4);
		DrawAction = ActNone; Current = nullptr;
	};
	if (g.state == StUnseen) {
		if (i != 0) { g.state = StPlain; SPlainGroups++; standard(pass, a2, a3, a4); return; }	// not the order planned: plain
		KeyState& ks = Keys[g.key];
		if (ks.blacklisted) g.state = StPlain;
		else if (Mode == 1) g.state = CheckFrame ? StCheck : StPlain;
		else if (ks.proven) g.state = CheckFrame && (++SampleCounter % kSampleEvery) == 0 ? StMergedCheck : StMerged;
		else { g.state = CheckFrame ? StCheck : StPlain; SProbation++; }
		if (g.state == StPlain) { SPlainGroups++; standard(pass, a2, a3, a4); return; }
		if (g.state == StCheck) {
			RefValid = false;
			run(ActCheckRef);
			if (!RefValid) g.state = StPlain;	// no draw came through the hooks: nothing to compare with
			else SChecked++;
			return;
		}
		// merged
		Issued = FellBack = false; ExtraDraws = 0; RefValid = false;
		run(ActLeader);
		if (!Issued || FellBack) { g.state = StPlain; SFallbacks++; return; }	// the copies are drawn the normal way
		if (ExtraDraws) {	// the entry drew more than once: only its first draw had the copies; they are drawn again whole (same pixels)
			g.state = StPlain;
			SMergedGroups--; SMergedCopies -= g.count - 1u; SKindMerged[g.kind & 3] -= g.count - 1u;
			Blacklist(g, "the entry drew more than once (a partitioned mesh)", 0);
			return;
		}
		if (g.state == StMergedCheck) { if (RefValid) SChecked++; else g.state = StMerged; }
		return;
	}
	switch (g.state) {
	case StPlain: standard(pass, a2, a3, a4); return;
	case StCheck: run(ActCheckCopy); g.arrived++; return;
	case StMerged: g.arrived++; return;	// drawn by the merged draw; the engine's setup skipped
	case StMergedCheck: run(ActCheckSuppress); g.arrived++; return;
	}
	standard(pass, a2, a3, a4);
}

HRESULT AtDraw(IDirect3DDevice9* d, DrawFn next, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT s, UINT pc) {
	const unsigned char action = DrawAction;
	if (!Current) return next(d, t, b, mi, nv, s, pc);
	Group& g = *Current;
	switch (action) {
	case ActLeader:
		if (Issued) { ExtraDraws++; return next(d, t, b, mi, nv, s, pc); }
		Issued = true;
		return DrawMerged(d, next, t, b, mi, nv, s, pc);
	case ActCheckRef: {
		if (RefValid) return next(d, t, b, mi, nv, s, pc);	// (a second draw of the entry: compared with nothing)
		Take(d, Ref, t, b, mi, nv, s, pc);
		RefValid = true;
		// the first entry too: what this code computes for it against what the engine set
		float own[kInstRegs * 4];
		memcpy(own + 16, Ref.vc[16], 16); memcpy(own + 20, Ref.vc[25], 16);
		Compute((const UInt8*)Members[g.first], own, g.kind);
		char why[256];
		SCompared++;
		if (!Same(Ref, Ref, own, CheckIgnore(g), why, sizeof(why))) Blacklist(g, why, 0);
		return next(d, t, b, mi, nv, s, pc);
	}
	case ActCheckCopy:
	case ActCheckSuppress: {
		if (!RefValid || g.checkFailed) return action == ActCheckSuppress ? D3D_OK : next(d, t, b, mi, nv, s, pc);
		Take(d, Cur, t, b, mi, nv, s, pc);
		float own[kInstRegs * 4];
		if (action == ActCheckCopy) {
			memcpy(own + 16, Ref.vc[16], 16); memcpy(own + 20, Ref.vc[25], 16);
			Compute((const UInt8*)Members[g.first + CurrentIndex], own, g.kind);
		}
		else memcpy(own, &Inst[CurrentIndex * kInstRegs * 4], sizeof(own));
		char why[256];
		SCompared++;
		if (!Same(Ref, Cur, own, CheckIgnore(g), why, sizeof(why))) Blacklist(g, why, CurrentIndex);
		return action == ActCheckSuppress ? D3D_OK : next(d, t, b, mi, nv, s, pc);
	}
	}
	return next(d, t, b, mi, nv, s, pc);
}
}
