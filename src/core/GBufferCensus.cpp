// UNOFFICIAL 2026-11-14 GBufferCensus: see GBufferCensus.h (what it counts) and docs/next/FD-CENSUS-RESULTS.md (the log format).
#include "GBufferCensus.h"
#include "CapturedNormals.h"
#include "DxvkCompute.h"
#include "DxvkLoader.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

namespace GBufferCensus {
namespace {
	struct Name { const char* name; bool nvr; };
	std::unordered_map<IDirect3DPixelShader9*, Name> Names;	// every pass bind's pixel shader (handle -> the game's shader name)
	const char* LastBindName = nullptr;
	bool LastBindNvr = false;

	enum Blend : UInt8 { BlendOpaque, BlendTest, BlendAlpha, BlendAdd, BlendMul, BlendOther, BlendNoColour, BlendCount };
	const char* const BlendNames[BlendCount] = { "opaque", "test", "alpha", "add", "mul", "other", "nocolour" };
	enum Why : UInt8 { WhyWrote, WhyMark, WhyOff, WhyNoSpan, WhyOtherTarget, WhyNotWriter, WhyFoliageIdle, WhyRefused, WhyCount };
	const char* const WhyNames[WhyCount] = { "wrote", "mark", "off", "nospan", "othertarget", "notwriter", "foliageidle", "refused" };
	const char* const SpanNames[SpanCount] = { "none", "main", "grass-early", "grass-late", "depthfirst", "blended", "distant" };
	const char* const ContextNames[] = { "world", "refl", "fp", "other", "extra", "?" };
	enum Gb : UInt8 { GbNormal = 1, GbMaterial = 2, GbAlbedo = 4, GbOtherTarget = 8 };

	struct Key {
		IDirect3DPixelShader9* ps;	// the device's pixel shader
		const char* bindName;		// only when ps was never named by a bind but the draw is inside a pass: the pass's bind (swapped handle)
		UInt16 pass;
		UInt8 ctx, span, tgt, blend, zw, gb, why, bindNvr;
		bool operator==(const Key& o) const {
			return ps == o.ps && bindName == o.bindName && pass == o.pass && ctx == o.ctx && span == o.span && tgt == o.tgt && blend == o.blend &&
				zw == o.zw && gb == o.gb && why == o.why && bindNvr == o.bindNvr;
		}
	};
	struct KeyHash {
		size_t operator()(const Key& k) const {
			size_t h = (size_t)k.ps * 2654435761u ^ (size_t)k.bindName * 40503u;
			h = h * 31u + k.pass; h = h * 31u + k.ctx; h = h * 31u + k.span; h = h * 31u + k.tgt; h = h * 31u + k.blend;
			h = h * 31u + k.zw; h = h * 31u + k.gb; h = h * 31u + k.why;
			return h;
		}
	};
	struct Agg { UInt32 draws, pxDraws; UInt64 px; };
	std::unordered_map<Key, UInt32, KeyHash> Index;
	std::vector<Key> Keys;
	std::vector<Agg> Aggs;

	// GBufferCensus = 2: an occlusion query round each world / first-person draw into the main-size target
	const UInt32 kMaxQueries = 8192;
	std::vector<IDirect3DQuery9*> Pool;	// kept for the session (made as needed)
	struct Issued { UInt32 key; };
	std::vector<Issued> IssuedList;		// query i of Pool belongs to IssuedList[i]
	int OpenQuery = -1;
	UInt32 Polled = 0, PxMiss = 0, PxFailed = 0, QueryCreateFails = 0;
	bool QueryBroken = false;			// CreateQuery failed: no pixel counts for the session

	enum Phase { Idle, Recording, Waiting };
	Phase State = Idle;
	ULONGLONG NextTick = 0;
	UInt32 WaitFrames = 0, Serial = 0;
	UInt32 Draws = 0, InPassDraws = 0;
	IDirect3DSurface9* LastRt0 = nullptr;	// a key only (not held)
	bool LastRt0Main = false;
	UInt32 MainMsaa = 0;
	char Header[512] = {};
	UInt32 CensusFrame = 0;
	UInt32 ReinstallsAtStart = 0;

	// ---- UNOFFICIAL 2026-11-14 merge census (GBufferCensusMerge; header comment; docs/next/FD-BATCH-RESULTS.md) ----
	bool Merge = false;					// the key, read at each census start
	IDirect3DSurface9* TargetAt(IDirect3DDevice9* d, DWORD i);	// (below)
	const UInt32 kVsRegs = 256, kPsRegs = 224, kRegs = kVsRegs + kPsRegs;
	// The pixel constant registers that carry direct light in the game's lit shaders and this mod's templates (ObjectTemplate.hlsl):
	// c3-c12 PSLightColor, c19-c26 PSLightPosition (TESR_SunDirection where a template has it), c28-c30 NVR_FlashLamp, c37-c63
	// NVR_ManyLamps, c66-c75 PSLightColorDec (LinearPreDecode), c76-c87 NVR_ManyLampsB, c98-c107 NVR_ManyLampsC, c188 NVR_PointShadowFocus,
	// c211 NVR_SpecFade. With lighting out of the object shaders (the deferred records) none of these is read. (c100-c103 is also
	// TESR_InvProjectionTransform in some templates: a per-frame value, equal for every copy, so leaving it out changes no group.)
	bool IsLampPs(UInt32 r) {
		return (r >= 3 && r <= 12) || (r >= 19 && r <= 26) || (r >= 28 && r <= 30) || (r >= 37 && r <= 63) || (r >= 66 && r <= 87) ||
			(r >= 98 && r <= 107) || r == 188 || r == 211;
	}
	struct UseMask { UInt32 bits[8]; UInt8 how; };	// how: 1 constant table, 2 instruction scan, 3 scan with an indexed read (to the end), 0 unknown (all)
	std::unordered_map<const void*, UseMask> VsUse, PsUse;
	inline bool Used(const UseMask& m, UInt32 r) { return (m.bits[r >> 5] >> (r & 31)) & 1; }
	inline void Mark(UseMask& m, UInt32 from, UInt32 count, UInt32 limit) { for (UInt32 r = from; r < from + count && r < limit; r++) m.bits[r >> 5] |= 1u << (r & 31); }

	// Which float constant registers a shader reads: from its constant table (the CTAB comment) when it has one, else from its
	// instructions (source registers of type const; an indexed read c[a0 + n] counts from n to the end; 'def' registers are literals).
	UseMask ReadUse(const DWORD* code, UInt32 words, UInt32 limit) {
		UseMask table = {}, scan = {};
		bool indexed = false, haveTable = false;
		UInt32 defined[8] = {};
		for (UInt32 i = 1; i < words; ) {
			const DWORD tok = code[i];
			if (tok == 0x0000FFFF) break;
			const UInt32 op = tok & 0xFFFF;
			if (op == 0xFFFE) {	// a comment block
				const UInt32 len = (tok >> 16) & 0x7FFF;
				if (len >= 8 && i + 1 + len <= words && code[i + 1] == 0x42415443) {	// "CTAB"
					const UInt8* t = (const UInt8*)&code[i + 2];
					const UInt32 bytes = (len - 1) * 4;
					const UInt32 count = *(const UInt32*)(t + 12), info = *(const UInt32*)(t + 16);
					if (count < 4096 && info < bytes && info + count * 20 <= bytes) {
						haveTable = true;
						for (UInt32 c = 0; c < count; c++) {
							const UInt8* e = t + info + c * 20;
							if (*(const UInt16*)(e + 4) == 2) Mark(table, *(const UInt16*)(e + 6), *(const UInt16*)(e + 8), limit);	// float4 set
						}
					}
				}
				i += 1 + len;
				continue;
			}
			const UInt32 len = (tok >> 24) & 0x0F;
			if (op == 0x51 && i + 1 < words) {	// def
				const UInt32 r = code[i + 1] & 0x7FF;
				if (r < 256) defined[r >> 5] |= 1u << (r & 31);
			}
			else if (op != 0x1F) {	// not dcl
				for (UInt32 k = 1; k <= len && i + k < words; k++) {
					const DWORD p = code[i + k];
					if (!(p & 0x80000000u)) continue;
					const UInt32 type = ((p >> 28) & 7) | ((p >> 8) & 0x18);
					if (type != 2) continue;	// a float constant
					const UInt32 r = p & 0x7FF;
					if (p & (1u << 13)) { indexed = true; Mark(scan, r, limit, limit); }
					else Mark(scan, r, 1, limit);
				}
			}
			i += 1 + len;
		}
		if (haveTable) { table.how = 1; return table; }
		if (words && ((code[0] >> 8) & 0xFF) < 2) return UseMask{};	// a 1.x shader (no instruction lengths to walk): unknown, every register counts
		for (UInt32 w = 0; w < 8; w++) scan.bits[w] &= ~defined[w];
		scan.how = indexed ? 3 : 2;
		return scan;
	}
	template<class S> const UseMask& UseOf(std::unordered_map<const void*, UseMask>& cache, S* shader, UInt32 limit) {
		auto it = cache.find(shader);
		if (it != cache.end()) return it->second;
		UseMask m = {};
		UINT size = 0;
		std::vector<DWORD> code;
		if (shader && SUCCEEDED(shader->GetFunction(nullptr, &size)) && size >= 8) {
			code.resize(size / 4);
			if (SUCCEEDED(shader->GetFunction(code.data(), &size))) m = ReadUse(code.data(), size / 4, limit);
		}
		if (!m.how) Mark(m, 0, limit, limit);	// unknown: every register counts
		return cache[shader] = m;
	}

	struct MergeRec {
		UInt32 seq;					// the draw's number in the census frame (its order)
		UInt16 pass;				// 0xFFFF: outside a pass
		UInt8 first, nl, blend, kind, uses;	// uses: where the VS / PS register masks came from (UseMask::how), 4 bits each
		UInt32 gm, sh, st, tx, vc, pc, pl, ls, md;
		const char* psName; bool nvr;
		UInt32 regAt;				// index into RegHash (kRegs per record)
	};
	std::vector<MergeRec> MergeRecs;
	std::vector<UInt32> RegHash;		// per record: 256 VS then 224 PS register hashes (0: the register is not read)
	UInt32 DrawSeq = 0;

	inline UInt32 Fnv(UInt32 h, UInt32 v) { return (h ^ v) * 16777619u; }

	void RecordMerge(IDirect3DDevice9* d, const Key& k, unsigned char kind, const UInt32* a) {
		MergeRec m = {};
		m.seq = DrawSeq;
		m.pass = k.pass;
		m.blend = k.blend;
		m.kind = kind;
		m.ls = 2166136261u;
		// the game's pass entry (BSShaderProperty::RenderPass, ENGINE-ANSWERS B6): +0 geometry, +7 first pass, +9 lamp count, +0xC lamps
		const UInt8* rp = (const UInt8*)CurrentRenderPass;
		if (rp && k.pass != 0xFFFF) {
			m.first = rp[7];
			m.nl = rp[9];
			void* const* lights = *(void* const* const*)(rp + 0x0C);
			m.ls = Fnv(m.ls, m.nl);
			for (UInt32 i = 0; lights && i < m.nl; i++) m.ls = Fnv(m.ls, (UInt32)lights[i]);
			const UInt8* geometry = *(const UInt8* const*)rp;
			if (geometry) m.md = *(const UInt32*)(geometry + 0xB8);	// the model data (NiGeometryData): shared by copies of one mesh
		}
		// the mesh: streams, indices, declaration, the draw call
		UInt32 h = Fnv(2166136261u, kind);
		for (int i = 0; i < 6; i++) h = Fnv(h, a[i]);
		for (UINT s = 0; s < 4; s++) {
			IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0, freq = 1;
			if (SUCCEEDED(d->GetStreamSource(s, &vb, &off, &stride)) && vb) { vb->Release(); h = Fnv(Fnv(Fnv(Fnv(h, s), (UInt32)vb), off), stride); }
			if (SUCCEEDED(d->GetStreamSourceFreq(s, &freq)) && freq != 1) h = Fnv(Fnv(h, 0x100 + s), freq);
		}
		IDirect3DIndexBuffer9* ib = nullptr;
		if (SUCCEEDED(d->GetIndices(&ib)) && ib) { ib->Release(); h = Fnv(h, (UInt32)ib); }
		IDirect3DVertexDeclaration9* decl = nullptr; DWORD fvf = 0;
		if (SUCCEEDED(d->GetVertexDeclaration(&decl)) && decl) { decl->Release(); h = Fnv(h, (UInt32)decl); }
		if (SUCCEEDED(d->GetFVF(&fvf))) h = Fnv(h, fvf);
		m.gm = h;
		// the shaders
		IDirect3DVertexShader9* vs = nullptr;
		if (SUCCEEDED(d->GetVertexShader(&vs)) && vs) vs->Release();
		m.sh = Fnv(Fnv(2166136261u, (UInt32)vs), (UInt32)k.ps);
		auto it = Names.find(k.ps);
		if (it != Names.end()) { m.psName = it->second.name; m.nvr = it->second.nvr; }
		else if (k.bindName) { m.psName = k.bindName; m.nvr = k.bindNvr != 0; }
		// the textures (pixel samplers 0-15, vertex samplers 0-3) and each bound one's sampler state
		static const D3DSAMPLERSTATETYPE kSampler[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
			D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_SRGBTEXTURE, D3DSAMP_BORDERCOLOR };
		UInt32 tx = 2166136261u, st = 2166136261u;
		for (DWORD s = 0; s < 20; s++) {
			const DWORD sampler = s < 16 ? s : D3DVERTEXTEXTURESAMPLER0 + (s - 16);
			IDirect3DBaseTexture9* t = nullptr;
			if (FAILED(d->GetTexture(sampler, &t)) || !t) continue;
			t->Release();
			tx = Fnv(Fnv(tx, s), (UInt32)t);
			for (D3DSAMPLERSTATETYPE type : kSampler) { DWORD v = 0; if (SUCCEEDED(d->GetSamplerState(sampler, type, &v))) st = Fnv(st, v); }
		}
		m.tx = tx;
		// blend, alpha test, depth, stencil, cull, write masks, fog, the targets
		static const D3DRENDERSTATETYPE kState[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
			D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_BLENDFACTOR, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
			D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFUNC,
			D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_CULLMODE,
			D3DRS_FILLMODE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_SRGBWRITEENABLE,
			D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_MULTISAMPLEANTIALIAS };
		for (D3DRENDERSTATETYPE type : kState) { DWORD v = 0; if (SUCCEEDED(d->GetRenderState(type, &v))) st = Fnv(st, v); }
		for (DWORD i = 0; i < 4; i++) st = Fnv(st, (UInt32)TargetAt(d, i));
		IDirect3DSurface9* ds = nullptr;
		if (SUCCEEDED(d->GetDepthStencilSurface(&ds)) && ds) { ds->Release(); st = Fnv(st, (UInt32)ds); }
		m.st = st;
		// the constants the bound shaders read
		const UseMask& vu = UseOf(VsUse, vs, kVsRegs);
		const UseMask& pu = UseOf(PsUse, k.ps, kPsRegs);
		m.uses = (UInt8)(vu.how | (pu.how << 4));
		static float vc[kVsRegs][4], pc[kPsRegs][4];
		if (FAILED(d->GetVertexShaderConstantF(0, &vc[0][0], kVsRegs))) memset(vc, 0, sizeof(vc));
		if (FAILED(d->GetPixelShaderConstantF(0, &pc[0][0], kPsRegs))) memset(pc, 0, sizeof(pc));
		m.regAt = (UInt32)RegHash.size();
		RegHash.resize(RegHash.size() + kRegs, 0);
		UInt32* rh = &RegHash[m.regAt];
		UInt32 hv = 2166136261u, hp = 2166136261u, hl = 2166136261u;
		for (UInt32 r = 0; r < kRegs; r++) {
			const bool pixel = r >= kVsRegs;
			const UInt32 reg = pixel ? r - kVsRegs : r;
			if (!Used(pixel ? pu : vu, reg)) continue;
			const UInt32* v = (const UInt32*)(pixel ? pc[reg] : vc[reg]);
			UInt32 x = Fnv(Fnv(Fnv(Fnv(Fnv(2166136261u, r), v[0]), v[1]), v[2]), v[3]);
			if (!x) x = 1;
			rh[r] = x;
			if (!pixel) hv = Fnv(hv, x);
			else if (IsLampPs(reg)) hl = Fnv(hl, x);
			else hp = Fnv(hp, x);
		}
		m.vc = hv; m.pc = hp; m.pl = hl;
		MergeRecs.push_back(m);
	}

	// The registers that differ between the members of each group (records with the same key), counted per register over the groups.
	void LogVary(const char* set, const std::vector<std::pair<UInt64, UInt32>>& keyed, UInt32 regFrom, UInt32 regTo) {
		std::vector<UInt32> count(kRegs, 0);
		std::vector<UInt8> diff(kRegs, 0);
		UInt32 groups = 0, members = 0;
		for (size_t a = 0; a < keyed.size(); ) {
			size_t b = a + 1;
			while (b < keyed.size() && keyed[b].first == keyed[a].first) b++;
			if (b - a > 1) {
				groups++;
				members += (UInt32)(b - a);
				const UInt32* ref = &RegHash[MergeRecs[keyed[a].second].regAt];
				std::fill(diff.begin(), diff.end(), (UInt8)0);
				for (size_t i = a + 1; i < b; i++) {
					const UInt32* cur = &RegHash[MergeRecs[keyed[i].second].regAt];
					for (UInt32 r = regFrom; r < regTo; r++) if (ref[r] != cur[r]) diff[r] = 1;
				}
				for (UInt32 r = regFrom; r < regTo; r++) count[r] += diff[r];
			}
			a = b;
		}
		std::vector<UInt32> order;
		for (UInt32 r = regFrom; r < regTo; r++) if (count[r]) order.push_back(r);
		std::sort(order.begin(), order.end(), [&](UInt32 x, UInt32 y) { return count[x] != count[y] ? count[x] > count[y] : x < y; });
		std::string regs;
		char item[24];
		for (UInt32 r : order) { sprintf_s(item, "%s%u:%u", regs.empty() ? "" : ",", r - regFrom, count[r]); regs += item; }
		Logger::Log("GBC V set=%s groups=%u members=%u regs=%s", set, groups, members, regs.empty() ? "-" : regs.c_str());
	}

	void EmitMerge();

	const char* Runtime() {
		if (DxvkCompute::Available()) return "fork";
		static int dxvk = -1;
		if (dxvk < 0) {
			char path[MAX_PATH] = {};
			HMODULE m = DxvkLoader::Runtime();
			dxvk = m && GetModuleFileNameA(m, path, MAX_PATH) && DxvkLoader::IsDxvk(path) ? 1 : 0;
		}
		return dxvk ? "dxvk" : "native";
	}

	// A value of a "key=value" field: no spaces, no '=' (the parser splits on both).
	std::string Clean(const char* s) {
		std::string out = s && *s ? s : "-";
		for (char& c : out) if (c == ' ' || c == '=' || c == '\t' || (unsigned char)c < 32) c = '_';
		return out;
	}

	// The template family of a shader name ("Object", "Parallax", "Terrain", "TerrainLOD", "Skin", "Env", ...), "own" for this mod's
	// replacement that is no template (its own .hlsl file), "game" for the game's shader; and the collection (menu family) it is in.
	struct Family { std::string family, collection; };
	std::unordered_map<std::string, Family> FamilyCache;	// name + "|0" / "|1" (one shader name may be bound both replaced and not)
	const Family& FamilyOf(const char* name, bool nvr) {
		const std::string key = std::string(name ? name : "") + (nvr ? "|1" : "|0");
		auto it = FamilyCache.find(key);
		if (it != FamilyCache.end()) return it->second;
		Family f = { nvr ? "own" : "game", "-" };
		if (name && TheShaderManager) {
			if (ShaderCollection* c = TheShaderManager->GetShaderCollection(name)) {
				f.collection = Clean(c->Name);
				if (nvr) {
					const ShaderTemplate t = c->GetTemplate(name);
					if (t.Name) {
						std::string n = t.Name;
						const size_t at = n.rfind("Template");
						if (at != std::string::npos && at + 8 == n.size() && at > 0) n.resize(at);
						f.family = Clean(n.c_str());
					}
				}
			}
		}
		return FamilyCache[key] = f;
	}

	UInt8 BlendOf(IDirect3DDevice9* d) {
		DWORD mask = 0xF, on = FALSE, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO, test = FALSE;
		d->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
		if (!(mask & 7)) return BlendNoColour;
		d->GetRenderState(D3DRS_ALPHABLENDENABLE, &on);
		if (!on) { d->GetRenderState(D3DRS_ALPHATESTENABLE, &test); return test ? BlendTest : BlendOpaque; }
		d->GetRenderState(D3DRS_SRCBLEND, &src);
		d->GetRenderState(D3DRS_DESTBLEND, &dst);
		if (dst == D3DBLEND_ONE && (src == D3DBLEND_ONE || src == D3DBLEND_SRCALPHA)) return BlendAdd;
		if ((src == D3DBLEND_DESTCOLOR && (dst == D3DBLEND_ZERO || dst == D3DBLEND_SRCCOLOR)) || (src == D3DBLEND_ZERO && dst == D3DBLEND_SRCCOLOR)) return BlendMul;
		if (dst == D3DBLEND_INVSRCALPHA && (src == D3DBLEND_SRCALPHA || src == D3DBLEND_ONE)) return BlendAlpha;
		if (src == D3DBLEND_ONE && dst == D3DBLEND_ZERO) return BlendOpaque;	// blending on, but a plain replace
		return BlendOther;
	}

	IDirect3DSurface9* TargetAt(IDirect3DDevice9* d, DWORD i) {
		IDirect3DSurface9* s = nullptr;
		if (SUCCEEDED(d->GetRenderTarget(i, &s)) && s) s->Release();	// the pointer is only compared
		return s;
	}

	void Clear() {
		Index.clear(); Keys.clear(); Aggs.clear(); IssuedList.clear();
		OpenQuery = -1; Polled = 0; PxMiss = 0; PxFailed = 0;
		Draws = InPassDraws = 0; Passes = 0; LastRt0 = nullptr; LastRt0Main = false; MainMsaa = 0;
		MergeRecs.clear(); RegHash.clear(); DrawSeq = 0;
	}

	void Start(IDirect3DDevice9* d) {
		Clear();
		CensusFrame = TheFrameRateManager ? TheFrameRateManager->FrameNumber : 0;
		ReinstallsAtStart = CapturedNormals::Reinstalls;
		std::string scene = "-";
		int interior = -1;
		float pos[3] = {};
		if (Player && Player->parentCell) {
			TESObjectCELL* cell = Player->parentCell;
			interior = cell->IsInterior() ? 1 : 0;
			pos[0] = Player->pos.x; pos[1] = Player->pos.y; pos[2] = Player->pos.z;
			const char* cellName = cell->GetEditorName();
			if (interior) scene = Clean(cellName);
			else {
				TESWorldSpace* ws = Player->GetWorldSpace();
				char buf[160];
				// exterior cells: the worldspace and the cell grid (4096 units), plus the cell's name when it has one
				sprintf_s(buf, "%s/%d,%d%s%s", ws && ws->GetEditorName() ? ws->GetEditorName() : "-", (int)floorf(pos[0] / 4096.0f), (int)floorf(pos[1] / 4096.0f),
					cellName && *cellName ? "/" : "", cellName && *cellName ? cellName : "");
				scene = Clean(buf);
			}
		}
		Merge = TheSettingManager->GetSettingI("Main.Main.Misc", "GBufferCensusMerge") != 0;
		if (Merge) { MergeRecs.reserve(8192); RegHash.reserve(4096 * kRegs); }
		// (merge=1: the block also has GBC I / GBC V lines; a new field at the end, so older parsers read the block as before)
		sprintf_s(Header, "GBC BEGIN v=1 serial=%u frame=%u rt=%s scene=%s interior=%d pos=%.0f,%.0f,%.0f w=%u h=%u mode=%d merge=%d",
			++Serial, CensusFrame, Runtime(), scene.c_str(), interior, pos[0], pos[1], pos[2],
			TheRenderManager ? (unsigned)TheRenderManager->width : 0u, TheRenderManager ? (unsigned)TheRenderManager->height : 0u, Mode, Merge ? 1 : 0);
		// (native D3D9 puts its own device-table entries back now and then: the draw hooks in again, in the order they were put in)
		if (EnsureHooks) EnsureHooks(d);
	}

	// The queries' results, in order (they finish in order); true when every one is in.
	bool Poll() {
		while (Polled < IssuedList.size()) {
			DWORD n = 0;
			const HRESULT r = Pool[Polled]->GetData(&n, sizeof(n), D3DGETDATA_FLUSH);
			if (r == S_FALSE) return false;
			Agg& a = Aggs[IssuedList[Polled].key];
			if (r == S_OK) { a.px += n; a.pxDraws++; }
			else PxFailed++;	// (device lost, or the query was never ended)
			Polled++;
		}
		return true;
	}

	void EmitMerge() {
		static const char* const kBlend[] = { "opaque", "test", "alpha", "add", "mul", "other", "nocolour" };
		static const Family none = { "-", "-" };
		UInt32 how[2][4] = {};
		for (const MergeRec& m : MergeRecs) {
			char pass[8] = "-";
			if (m.pass != 0xFFFF) sprintf_s(pass, "%03X", (unsigned)m.pass);
			const Family& fam = m.psName ? FamilyOf(m.psName, m.nvr) : none;
			Logger::Log("GBC I s=%u p=%s f=%u nl=%u b=%s k=%u gm=%08X sh=%08X st=%08X tx=%08X vc=%08X pc=%08X pl=%08X ls=%08X md=%08X fam=%s",
				m.seq, pass, (unsigned)m.first, (unsigned)m.nl, kBlend[m.blend < 7 ? m.blend : 5], (unsigned)m.kind, m.gm, m.sh, m.st, m.tx, m.vc, m.pc, m.pl,
				m.ls, m.md, fam.family.c_str());
			how[0][m.uses & 3]++; how[1][(m.uses >> 4) & 3]++;
		}
		// Which registers differ between copies: VS and PS inside the deferred rule's groups (first passes, opaque or alpha-tested, same pass,
		// mesh, shaders, state, textures and non-lamp pixel constants); PS inside groups whose pixel constants are free (ENGINE-ANSWERS D7).
		auto mix = [](UInt64 x, UInt64 v) { return (x ^ (v + 0x9E3779B97F4A7C15ull + (x << 6) + (x >> 2))) * 0xFF51AFD7ED558CCDull; };
		std::vector<std::pair<UInt64, UInt32>> deferredKeys, freeKeys;
		for (UInt32 i = 0; i < MergeRecs.size(); i++) {
			const MergeRec& m = MergeRecs[i];
			if (m.pass == 0xFFFF || m.kind != KindDIP) continue;
			const UInt64 key = mix(mix(mix(mix(mix(0, m.pass), m.gm), m.sh), m.st), m.tx);
			freeKeys.push_back({ key, i });
			if (m.first && (m.blend == BlendOpaque || m.blend == BlendTest)) deferredKeys.push_back({ mix(key, m.pc), i });
		}
		std::sort(deferredKeys.begin(), deferredKeys.end());
		std::sort(freeKeys.begin(), freeKeys.end());
		LogVary("deferred-vs", deferredKeys, 0, kVsRegs);
		LogVary("deferred-ps", deferredKeys, kVsRegs, kRegs);
		LogVary("free-ps", freeKeys, kVsRegs, kRegs);
		Logger::Log("GBC V set=uses records=%u vs-table=%u vs-scan=%u vs-indexed=%u vs-unknown=%u ps-table=%u ps-scan=%u ps-indexed=%u ps-unknown=%u",
			(unsigned)MergeRecs.size(), how[0][1], how[0][2], how[0][3], how[0][0], how[1][1], how[1][2], how[1][3], how[1][0]);
	}

	void Emit() {
		const UInt32 late = (UInt32)IssuedList.size() - Polled;	// queries still out when the wait ended (their draws: no pixel count)
		Logger::Log("%s", Header);
		std::vector<UInt32> order(Keys.size());
		for (UInt32 i = 0; i < order.size(); i++) order[i] = i;
		std::sort(order.begin(), order.end(), [](UInt32 a, UInt32 b) { return Aggs[a].draws > Aggs[b].draws; });
		UInt32 world = 0;
		for (UInt32 i : order) {
			const Key& k = Keys[i];
			const Agg& a = Aggs[i];
			if (k.ctx == 0) world += a.draws;
			const char* name = nullptr;
			bool nvr = false, swap = false;
			auto it = Names.find(k.ps);
			if (it != Names.end()) { name = it->second.name; nvr = it->second.nvr; }
			else if (k.bindName) { name = k.bindName; nvr = k.bindNvr != 0; swap = true; }
			std::string psName;
			if (name) psName = Clean(name);
			else { char p[32]; sprintf_s(p, "unnamed@%p", (void*)k.ps); psName = p; }
			static const Family none = { "-", "-" };
			const Family& fam = name ? FamilyOf(name, nvr) : none;
			char pass[8] = "-";
			std::string passName = "-";
			if (k.pass != 0xFFFF) { sprintf_s(pass, "%03X", (unsigned)k.pass); passName = Clean(Pointers::Functions::GetPassDescription(k.pass)); }
			char gb[8] = {}; int g = 0;
			if (k.gb & GbNormal) gb[g++] = 'N';
			if (k.gb & GbMaterial) gb[g++] = 'M';
			if (k.gb & GbAlbedo) gb[g++] = 'A';
			if (k.gb & GbOtherTarget) gb[g++] = 'x';
			if (!g) gb[g++] = '-';
			char px[48];
			if (a.pxDraws) sprintf_s(px, "px=%llu pxn=%u", (unsigned long long)a.px, a.pxDraws); else sprintf_s(px, "px=-1 pxn=0");
			Logger::Log("GBC D n=%u %s ctx=%s span=%s pass=%s pn=%s tgt=%s ps=%s fam=%s col=%s nvr=%d swap=%d blend=%s zw=%d gb=%s why=%s",
				a.draws, px, ContextNames[k.ctx < 5 ? k.ctx : 5], SpanNames[k.span < SpanCount ? k.span : 0], pass, passName.c_str(), k.tgt ? "main" : "off",
				psName.c_str(), name ? fam.family.c_str() : "-", name ? fam.collection.c_str() : "-", nvr ? 1 : 0, swap ? 1 : 0, BlendNames[k.blend], k.zw, gb, WhyNames[k.why]);
		}
		if (Merge) EmitMerge();
		Logger::Log("GBC END serial=%u frame=%u draws=%u world=%u keys=%u passes=%u passdraws=%u queries=%u pxlate=%u pxfail=%u pxmiss=%u msaa=%u reinstalls=%u cn=%d",
			Serial, CensusFrame, Draws, world, (unsigned)Keys.size(), Passes, InPassDraws, (unsigned)IssuedList.size(), late, PxFailed, PxMiss, MainMsaa,
			CapturedNormals::Reinstalls - ReinstallsAtStart, CapturedNormals::Live ? 1 : 0);
		if (Passes > 50 && InPassDraws < Passes / 4)
			Logger::Log("[WARNING] GBufferCensus: %u game passes but only %u draws seen inside them: the draw hooks were lost for part of the frame (another program's hook on top?); this census is incomplete", Passes, InPassDraws);
	}
}

bool BeginFrame(IDirect3DDevice9* d) {
	const int want = TheSettingManager->GetSettingI("Main.Main.Misc", "GBufferCensus");
	if (want != Mode) {
		if (State == Recording || State == Waiting) { Active = false; Clear(); State = Idle; }
		Mode = want;
		NextTick = GetTickCount64() + 3000;	// the first census a few seconds after it is switched on
		if (Mode) Logger::Log("GBufferCensus: on (mode %d: %s), one census frame every %.0f s; log lines 'GBC' (tools/fd/census_parse.py)", Mode,
			Mode >= 2 ? "draws and pixel counts" : "draws", (std::max)(1.0f, TheSettingManager->GetSettingF("Main.Main.Misc", "GBufferCensusEvery")));
	}
	if (!Mode) { Active = false; return false; }
	if (State == Recording) {
		Active = false;
		if (OpenQuery >= 0) { Pool[OpenQuery]->Issue(D3DISSUE_END); OpenQuery = -1; }	// (never left open: After ends it)
		State = Waiting;
		WaitFrames = 0;
	}
	if (State == Waiting) {
		if (Poll() || ++WaitFrames > 120) {
			Emit();
			State = Idle;
			NextTick = GetTickCount64() + (ULONGLONG)((std::max)(1.0f, TheSettingManager->GetSettingF("Main.Main.Misc", "GBufferCensusEvery")) * 1000.0f);
		}
		return true;
	}
	if (State == Idle && GetTickCount64() >= NextTick && Player && Player->parentCell) {
		Start(d);
		State = Recording;
		Active = true;
	}
	return true;
}

void OnBind(IDirect3DPixelShader9* handle, const char* name, bool nvr) {
	if (!Mode) return;
	LastBindName = name;
	LastBindNvr = nvr;
	if (handle) Names[handle] = { name, nvr };
}

void Before(IDirect3DDevice9* d, unsigned char kind, UInt32 a0, UInt32 a1, UInt32 a2, UInt32 a3, UInt32 a4, UInt32 a5) {
	Draws++;
	DrawSeq++;
	Key k = {};
	const int ctx = ContextOf ? ContextOf() : 5;
	k.ctx = (UInt8)(ctx >= 0 && ctx < 5 ? ctx : 5);
	k.span = CurrentSpan;
	k.pass = (UInt16)(CurrentPass & 0xFFFF);
	if (CurrentPass != 0xFFFF) InPassDraws++;
	IDirect3DSurface9* rt0 = TargetAt(d, 0);
	if (rt0 != LastRt0) {
		LastRt0 = rt0;
		LastRt0Main = false;
		D3DSURFACE_DESC t = {};
		if (rt0 && SUCCEEDED(rt0->GetDesc(&t)) && TheRenderManager && t.Width == TheRenderManager->width && t.Height == TheRenderManager->height) {
			LastRt0Main = true;
			if (t.MultiSampleType != D3DMULTISAMPLE_NONE) MainMsaa = (UInt32)t.MultiSampleType;
		}
	}
	k.tgt = LastRt0Main ? 1 : 0;
	IDirect3DPixelShader9* ps = nullptr;
	d->GetPixelShader(&ps);
	if (ps) ps->Release();	// the pointer is only a key
	k.ps = ps;
	if (CurrentPass != 0xFFFF && Names.find(ps) == Names.end()) { k.bindName = LastBindName; k.bindNvr = LastBindNvr ? 1 : 0; }
	k.blend = BlendOf(d);
	DWORD zEnable = D3DZB_TRUE, zWrite = TRUE;
	d->GetRenderState(D3DRS_ZENABLE, &zEnable);
	d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
	k.zw = zEnable != D3DZB_FALSE && zWrite ? 1 : 0;
	// What the draw writes to the G-buffer: the device's targets 1-3 and their write masks, as CapturedNormals::BeforeDraw left them.
	DWORD m1 = 0, m2 = 0, m3 = 0;
	IDirect3DSurface9* rt1 = TargetAt(d, 1); IDirect3DSurface9* rt2 = TargetAt(d, 2); IDirect3DSurface9* rt3 = TargetAt(d, 3);
	if (rt1) d->GetRenderState(D3DRS_COLORWRITEENABLE1, &m1);
	if (rt2) d->GetRenderState(D3DRS_COLORWRITEENABLE2, &m2);
	if (rt3) d->GetRenderState(D3DRS_COLORWRITEENABLE3, &m3);
	IDirect3DSurface9* const normals = CapturedNormals::DrawSurface();
	IDirect3DSurface9* const material = CapturedNormals::MaterialDrawSurface();
	IDirect3DSurface9* const albedo = CapturedNormals::AlbedoDrawSurface();
	const bool colour = k.blend != BlendNoColour;
	if (colour && rt3 && m3 && normals && rt3 == normals) k.gb |= GbNormal;
	if (colour && rt2 && m2 && material && rt2 == material) k.gb |= GbMaterial;
	if (colour && rt1 && m1 && albedo && rt1 == albedo) k.gb |= GbAlbedo;
	if (colour && ((rt1 && m1 && rt1 != albedo) || (rt2 && m2 && rt2 != material) || (rt3 && m3 && rt3 != normals))) k.gb |= GbOtherTarget;	// the skin blur's, the view model's skin
	if (k.gb & GbNormal) k.why = m3 == D3DCOLORWRITEENABLE_BLUE ? WhyMark : WhyWrote;
	else if (!CapturedNormals::Live) k.why = WhyOff;
	else if (!CapturedNormals::InSpan) k.why = WhyNoSpan;
	else if (rt0 != CapturedNormals::SpanTarget) k.why = WhyOtherTarget;
	else if (ps && (CapturedNormals::GrassWriters.count(ps) || CapturedNormals::LeafWriters.count(ps))) k.why = WhyFoliageIdle;
	else if (!ps || !CapturedNormals::Writers.count(ps)) k.why = WhyNotWriter;
	else k.why = WhyRefused;
	UInt32 index;
	auto it = Index.find(k);
	if (it != Index.end()) index = it->second;
	else { index = (UInt32)Keys.size(); Index.emplace(k, index); Keys.push_back(k); Aggs.push_back({ 0, 0, 0 }); }
	Aggs[index].draws++;
	if (Merge && k.ctx == 0 && k.span == SpanMain && k.tgt) {	// the merge census: the main opaque list's draws into the main view
		const UInt32 a[6] = { a0, a1, a2, a3, a4, a5 };
		RecordMerge(d, k, kind, a);
	}
	if (Mode >= 2 && (k.ctx == 0 || k.ctx == 2) && k.tgt && OpenQuery < 0) {
		const UInt32 q = (UInt32)IssuedList.size();
		if (q >= kMaxQueries || QueryBroken) { PxMiss++; return; }
		if (q >= Pool.size()) {
			IDirect3DQuery9* query = nullptr;
			if (FAILED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query)) || !query) {
				if (++QueryCreateFails >= 3) { QueryBroken = true; Logger::Log("[WARNING] GBufferCensus: occlusion queries cannot be made: no pixel counts this session"); }
				PxMiss++;
				return;
			}
			Pool.push_back(query);
		}
		IssuedList.push_back({ index });
		Pool[q]->Issue(D3DISSUE_BEGIN);
		OpenQuery = (int)q;
	}
}

void After(IDirect3DDevice9* d) {
	if (OpenQuery < 0) return;
	Pool[OpenQuery]->Issue(D3DISSUE_END);
	OpenQuery = -1;
}

// UNOFFICIAL 2026-11-14 MergeCopies' self-check: the same register masks as the merge census (cached per shader).
bool RegistersRead(IDirect3DVertexShader9* vs, UInt32 bits[8]) {
	const UseMask& m = UseOf(VsUse, vs, kVsRegs);
	memcpy(bits, m.bits, sizeof(m.bits));
	return m.how != 0;
}
bool RegistersRead(IDirect3DPixelShader9* ps, UInt32 bits[8]) {
	const UseMask& m = UseOf(PsUse, ps, kPsRegs);
	memcpy(bits, m.bits, sizeof(m.bits));
	return m.how != 0;
}
}
