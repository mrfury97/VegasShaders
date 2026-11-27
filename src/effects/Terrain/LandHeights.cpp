#include "LandHeights.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <memory>
#include <unordered_map>

// UNOFFICIAL 2026-10-19 (see LandHeights.h). Plugin format: the FO3/FNV TES4 layout (24-byte record and group headers,
// subrecords with 16-bit sizes and XXXX for longer ones), as documented on UESP and in xEdit's record definitions.

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Inflate (RFC 1951 deflate inside RFC 1950 zlib), written for this file. A fast table for codes up to 9 bits, a
// bit-at-a-time walk of the canonical code for longer ones.
// ---------------------------------------------------------------------------------------------------------------------
struct Bits {
	const uint8_t*	p;
	const uint8_t*	end;
	uint32_t		buffer = 0;
	int				count = 0;

	void Fill() { while (count <= 24 && p < end) { buffer |= (uint32_t)*p++ << count; count += 8; } }
	bool Need(int n) { if (count < n) Fill(); return count >= n; }
	uint32_t Take(int n) { uint32_t v = buffer & ((1u << n) - 1); buffer >>= n; count -= n; return v; }
};

struct Huffman {
	static const int FastBits = 9;
	uint16_t	fast[1 << FastBits];   // (symbol << 4) | length; 0 = longer than FastBits
	uint16_t	counts[16];
	uint16_t	symbols[288];

	bool Build(const uint8_t* lengths, int n) {
		memset(counts, 0, sizeof(counts));
		memset(fast, 0, sizeof(fast));
		for (int s = 0; s < n; s++) counts[lengths[s]]++;
		counts[0] = 0;
		uint16_t offsets[16] = {};
		for (int len = 1; len < 16; len++) offsets[len] = offsets[len - 1] + counts[len - 1];
		for (int s = 0; s < n; s++) if (lengths[s]) symbols[offsets[lengths[s]]++] = (uint16_t)s;
		// Canonical codes: the first code of each length, then each symbol's in order.
		int code = 0, next[16] = {};
		for (int len = 1; len < 16; len++) { code = (code + counts[len - 1]) << 1; next[len] = code; }
		for (int s = 0; s < n; s++) {
			const int len = lengths[s];
			if (!len || len > FastBits) continue;
			int c = next[len]++, reversed = 0;
			for (int i = 0; i < len; i++) { reversed = (reversed << 1) | (c & 1); c >>= 1; }
			for (int fill = reversed; fill < (1 << FastBits); fill += 1 << len) fast[fill] = (uint16_t)((s << 4) | len);
		}
		return true;
	}

	int Decode(Bits& bits) const {
		bits.Fill();
		const uint16_t entry = fast[bits.buffer & ((1 << FastBits) - 1)];
		if (entry) {
			const int len = entry & 15;
			if (bits.count < len) return -1;
			bits.Take(len);
			return entry >> 4;
		}
		// Longer codes: one bit at a time through the canonical code (first code and symbol index of each length).
		int code = 0, first = 0, index = 0;
		for (int len = 1; len < 16; len++) {
			if (!bits.Need(1)) return -1;
			code |= (int)bits.Take(1);
			const int count = counts[len];
			if (code - count < first) return symbols[index + (code - first)];
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
		return -1;
	}
};

const uint16_t LengthBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const uint8_t LengthExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const uint16_t DistanceBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const uint8_t DistanceExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

// One block's symbols until its end code (or the limit). 1 done, 0 limit reached, -1 damaged.
int InflateBlock(Bits& bits, const Huffman& literals, const Huffman& distances, std::vector<uint8_t>& out, size_t limit) {
	for (;;) {
		const int symbol = literals.Decode(bits);
		if (symbol < 0) return -1;
		if (symbol < 256) { out.push_back((uint8_t)symbol); }
		else if (symbol == 256) return 1;
		else {
			const int l = symbol - 257;
			if (l >= 29 || !bits.Need(LengthExtra[l])) return -1;
			const size_t length = LengthBase[l] + bits.Take(LengthExtra[l]);
			const int d = distances.Decode(bits);
			if (d < 0 || d >= 30 || !bits.Need(DistanceExtra[d])) return -1;
			const size_t distance = DistanceBase[d] + bits.Take(DistanceExtra[d]);
			if (distance > out.size()) return -1;
			const size_t from = out.size() - distance;
			for (size_t i = 0; i < length; i++) out.push_back(out[from + i]);
		}
		if (limit && out.size() >= limit) return 0;
	}
}

}	// namespace

bool LandHeights::Inflate(const uint8_t* In, size_t InSize, std::vector<uint8_t>& Out, size_t Limit) {
	if (InSize < 2) return false;
	const uint8_t cmf = In[0], flg = In[1];
	if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;   // deflate, header check, no preset dictionary
	Bits bits{ In + 2, In + InSize };
	static Huffman fixedLiterals, fixedDistances;
	static bool fixedBuilt = false;
	if (!fixedBuilt) {
		uint8_t lengths[288];
		for (int i = 0; i < 288; i++) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
		fixedLiterals.Build(lengths, 288);
		for (int i = 0; i < 30; i++) lengths[i] = 5;
		fixedDistances.Build(lengths, 30);
		fixedBuilt = true;
	}
	Huffman literals, distances;
	for (;;) {
		if (!bits.Need(3)) return false;
		const bool last = bits.Take(1) != 0;
		const uint32_t type = bits.Take(2);
		int result;
		if (type == 0) {
			bits.Take(bits.count & 7);   // to the byte boundary
			if (!bits.Need(16)) return false;
			const uint32_t length = bits.Take(16);
			if (!bits.Need(16)) return false;
			const uint32_t inverse = bits.Take(16);
			if ((length ^ 0xFFFF) != inverse) return false;
			uint32_t left = length;
			while (left && bits.count >= 8) { Out.push_back((uint8_t)bits.Take(8)); left--; }
			if ((size_t)(bits.end - bits.p) < left) return false;
			Out.insert(Out.end(), bits.p, bits.p + left);
			bits.p += left;
			result = Limit && Out.size() >= Limit ? 0 : 1;
		}
		else if (type == 1) result = InflateBlock(bits, fixedLiterals, fixedDistances, Out, Limit);
		else if (type == 2) {
			if (!bits.Need(14)) return false;
			const int literalCount = (int)bits.Take(5) + 257, distanceCount = (int)bits.Take(5) + 1, codeCount = (int)bits.Take(4) + 4;
			static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
			uint8_t codeLengths[19] = {};
			for (int i = 0; i < codeCount; i++) { if (!bits.Need(3)) return false; codeLengths[order[i]] = (uint8_t)bits.Take(3); }
			Huffman codes;
			codes.Build(codeLengths, 19);
			uint8_t lengths[320] = {};
			for (int i = 0; i < literalCount + distanceCount;) {
				const int symbol = codes.Decode(bits);
				if (symbol < 0) return false;
				if (symbol < 16) { lengths[i++] = (uint8_t)symbol; continue; }
				int repeat = 0;
				uint8_t value = 0;
				if (symbol == 16) { if (!i || !bits.Need(2)) return false; value = lengths[i - 1]; repeat = 3 + (int)bits.Take(2); }
				else if (symbol == 17) { if (!bits.Need(3)) return false; repeat = 3 + (int)bits.Take(3); }
				else { if (!bits.Need(7)) return false; repeat = 11 + (int)bits.Take(7); }
				if (i + repeat > literalCount + distanceCount) return false;
				while (repeat--) lengths[i++] = value;
			}
			literals.Build(lengths, literalCount);
			distances.Build(lengths + literalCount, distanceCount);
			result = InflateBlock(bits, literals, distances, Out, Limit);
		}
		else return false;
		if (result < 0) return false;
		if (result == 0 || last) return true;
	}
}

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Plugin walking
// ---------------------------------------------------------------------------------------------------------------------
const uint32_t RecordCompressed = 0x00040000, RecordDeleted = 0x00000020, RecordDisabled = 0x00000800;

uint32_t Tag(const char* s) { return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) | ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24); }

struct Header {   // a record's or a group's 24 bytes
	uint32_t	type, size, label, groupType;   // records: size = data size, label = flags, groupType = form id
};

class Reader {
public:
	explicit Reader(const std::string& Path) {
		if (fopen_s(&file, Path.c_str(), "rb") || !file) { file = nullptr; return; }
		setvbuf(file, nullptr, _IONBF, 0);
		_fseeki64(file, 0, SEEK_END);
		size = (uint64_t)_ftelli64(file);
		buffer.resize(1 << 18);
	}
	~Reader() { if (file) fclose(file); }
	bool Ok() const { return file != nullptr; }
	uint64_t Size() const { return size; }

	bool Read(uint64_t pos, void* dst, size_t n) {
		if (pos + n > size) return false;
		if (pos >= start && pos + n <= start + length) { memcpy(dst, &buffer[(size_t)(pos - start)], n); return true; }
		if (n > buffer.size()) {
			if (_fseeki64(file, (long long)pos, SEEK_SET)) return false;
			return fread(dst, 1, n, file) == n;
		}
		if (_fseeki64(file, (long long)pos, SEEK_SET)) return false;
		length = fread(buffer.data(), 1, buffer.size(), file);
		start = pos;
		if (length < n) return false;
		memcpy(dst, buffer.data(), n);
		return true;
	}

	bool ReadHeader(uint64_t pos, Header& h) {
		uint32_t raw[6];
		if (!Read(pos, raw, 24)) return false;
		h.type = raw[0]; h.size = raw[1]; h.label = raw[2]; h.groupType = raw[3];
		return true;
	}

private:
	FILE*		file = nullptr;
	uint64_t	size = 0;
	std::vector<uint8_t>	buffer;
	uint64_t	start = 0;
	size_t		length = 0;
};

bool SameName(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
	return true;
}

// A record's data, unpacked if compressed (up to Limit bytes of it, 0 all).
bool RecordData(Reader& file, uint64_t pos, const Header& h, std::vector<uint8_t>& raw, std::vector<uint8_t>& data, size_t limit) {
	raw.resize(h.size);
	if (h.size && !file.Read(pos + 24, raw.data(), h.size)) return false;
	if (!(h.label & RecordCompressed)) { data.swap(raw); return true; }
	if (h.size < 4) return false;
	uint32_t unpacked;
	memcpy(&unpacked, raw.data(), 4);
	data.clear();
	data.reserve(limit ? (std::min)((size_t)unpacked, limit + 512) : unpacked);
	return LandHeights::Inflate(raw.data() + 4, h.size - 4, data, limit);
}

// The first subrecord of this type in a record's data (XXXX-aware). Null if missing (or cut off by a partial unpack).
const uint8_t* FindSub(const std::vector<uint8_t>& data, uint32_t type, uint32_t& size) {
	size_t p = 0;
	uint32_t longSize = 0;
	while (p + 6 <= data.size()) {
		uint32_t t; uint16_t s;
		memcpy(&t, &data[p], 4);
		memcpy(&s, &data[p + 4], 2);
		p += 6;
		uint32_t n = s;
		if (longSize) { n = longSize; longSize = 0; }
		if (t == Tag("XXXX") && s == 4 && p + 4 <= data.size()) { memcpy(&longSize, &data[p], 4); p += 4; continue; }
		if (n > data.size() - p) return nullptr;	// (2026-11-17: was p + n, which wraps with a huge XXXX size; p <= size here)
		if (t == type) { size = n; return &data[p]; }
		p += n;
	}
	return nullptr;
}

	std::vector<const uint8_t*> FindAllSubs(const std::vector<uint8_t>& data, uint32_t type, std::vector<uint32_t>& sizes) {
		std::vector<const uint8_t*> out;
		sizes.clear();
		size_t p = 0;
		uint32_t longSize = 0;
		while (p + 6 <= data.size()) {
			uint32_t t; uint16_t s;
			memcpy(&t, &data[p], 4);
			memcpy(&s, &data[p + 4], 2);
			p += 6;
			uint32_t n = s;
			if (longSize) { n = longSize; longSize = 0; }
			if (t == Tag("XXXX") && s == 4 && p + 4 <= data.size()) { memcpy(&longSize, &data[p], 4); p += 4; continue; }
			if (n > data.size() - p) break;
			if (t == type) { out.push_back(&data[p]); sizes.push_back(n); }
			p += n;
		}
		return out;
	}

	std::string LowerAscii(const std::string& s) {
		std::string r = s;
		for (char& c : r) c = (char)tolower((unsigned char)c);
		return r;
	}

	uint32_t ClassifyLtexSwatch(const char* edid) {
		if (!edid || !*edid) return (136u << 16) | (116u << 8) | 90u;
		std::string s = LowerAscii(edid);
		if (s.find("asphalt") != std::string::npos || s.find("pavement") != std::string::npos || s.find("road") != std::string::npos)
			return (68u << 16) | (66u << 8) | 64u;
		if (s.find("redrock") != std::string::npos || s.find("redsand") != std::string::npos || s.find("canyon") != std::string::npos ||
			s.find("zion") != std::string::npos || s.find("nvdlc02") != std::string::npos || s.find("sandstone") != std::string::npos ||
			s.find("ochre") != std::string::npos || s.find("terra") != std::string::npos)
			return (152u << 16) | (92u << 8) | 62u;
		if (s.find("grass") != std::string::npos || s.find("lawn") != std::string::npos || s.find("golf") != std::string::npos || s.find("moss") != std::string::npos)
			return (88u << 16) | (106u << 8) | 66u;
		if (s.find("snow") != std::string::npos || s.find("ice") != std::string::npos)
			return (195u << 16) | (198u << 8) | 205u;
		if (s.find("mud") != std::string::npos || s.find("scum") != std::string::npos || s.find("swamp") != std::string::npos)
			return (82u << 16) | (76u << 8) | 60u;
		if (s.find("rock") != std::string::npos || s.find("cliff") != std::string::npos || s.find("rubble") != std::string::npos || s.find("gravel") != std::string::npos)
			return (122u << 16) | (104u << 8) | 82u;
		return (138u << 16) | (118u << 8) | 92u;
	}

	int ClassifyFoliageEdid(const char* edid) {
		if (!edid || !*edid) return -1;
		std::string s = LowerAscii(edid);
		if (s.find("street") != std::string::npos || s.find("stump") != std::string::npos || s.find("log") != std::string::npos ||
			s.find("branch") != std::string::npos || s.find("planter") != std::string::npos || s.find("pot") != std::string::npos ||
			s.find("sign") != std::string::npos || s.find("decal") != std::string::npos || s.find("bushulk") != std::string::npos ||
			s.find("bushhulk") != std::string::npos || s.find("lamp") != std::string::npos || s.find("box") != std::string::npos)
			return -1;
		if (s.find("treejosh") != std::string::npos || s.find("joshua") != std::string::npos) return 0;
		if (s.find("treepine") != std::string::npos || s.find("treeevergreen") != std::string::npos || s.find("pinetree") != std::string::npos) return 1;
		if (s.find("treedead") != std::string::npos || s.find("deadtree") != std::string::npos) return 2;
		if (s.find("palm") != std::string::npos || s.find("cottonwood") != std::string::npos || s.find("whiteoak") != std::string::npos) return 4;
		if (s.find("yucca") != std::string::npos || s.find("mesquite") != std::string::npos || s.find("creosote") != std::string::npos ||
			s.find("sagebrush") != std::string::npos || s.find("euonymus") != std::string::npos || s.find("barrelcactus") != std::string::npos) return 3;
		return -1;
	}

	int ClassifyRockStat(const char* edid, const char* modl) {
		std::string e = edid ? LowerAscii(edid) : std::string();
		std::string m = modl ? LowerAscii(modl) : std::string();
		auto hasAny = [](const std::string& s, const char* const* toks, size_t n) {
			for (size_t i = 0; i < n; ++i) if (s.find(toks[i]) != std::string::npos) return true;
			return false;
		};
		static const char* const kBad[] = {
			"cave", "underwater", "water", "pebble", "rubble", "debris", "rocket", "rockit",
			"rocker", "rockwell", "vault", "interior", "int_", "sign", "decal", "floor", "wall", "bridge", "planter"
		};
		if (hasAny(e, kBad, sizeof(kBad) / sizeof(kBad[0])) || hasAny(m, kBad, sizeof(kBad) / sizeof(kBad[0]))) return -1;

		const bool isRockPath = (m.find("landscape\\rocks\\") != std::string::npos);
		const bool isCliff = (e.find("cliff") != std::string::npos || e.find("mesa") != std::string::npos || m.find("\\cliffs\\") != std::string::npos);
		const bool isBoulder = (e.find("boulder") != std::string::npos || m.find("\\boulder\\") != std::string::npos);
		const bool isRockEdid = (e.find("rockcanyon") != std::string::npos || e.find("nvrock") != std::string::npos ||
			e.find("limepile") != std::string::npos || e.find("outcrop") != std::string::npos || e.find("canyonrock") != std::string::npos);
		if (!isRockPath && !isCliff && !isBoulder && !isRockEdid) return -1;
		if (isCliff) return 2;
		if (isBoulder) return 0;
		return 1;
	}

	struct LightBase {
		float radius = 0.0f, r = 1.0f, g = 1.0f, b = 1.0f;
	};

	struct LandEntry {
		int			x, y;
		uint32_t	plugin;
		uint64_t	pos;
		Header		header;
	};

	struct FoliageRec {
		float		x, y, z, rotZ, scale;
		uint8_t		style;
	};

	struct Walker {
		Reader&			file;
		uint32_t		pluginIndex;
		std::string		pluginName;
		std::vector<std::string>	masters;
		std::vector<uint32_t>		masterToGlobal;
		const std::unordered_map<std::string, uint32_t>&	pluginByName;
		const std::string&	worldFile;
		uint32_t		worldID;
		std::unordered_map<int64_t, LandEntry>&	lands;
		bool			collectExtras;
		std::unordered_map<uint32_t, uint32_t>*		ltexSwatches;
		std::unordered_map<uint32_t, uint8_t>*		foliageForms;
		std::unordered_map<uint32_t, FoliageRec>*	foliageRefs;
		std::unordered_map<uint32_t, uint8_t>*		rockForms;
		std::unordered_map<uint32_t, FoliageRec>*	rockRefs;
		std::unordered_map<uint32_t, LightBase>*	lightForms;
		std::unordered_map<uint32_t, LandHeights::NightLightRef>* lightRefs;
		std::vector<uint8_t>	raw, data;

		uint32_t CanonicalFormID(uint32_t rawFid) const {
			const uint32_t modIdx = rawFid >> 24;
			const uint32_t ownerIdx = (modIdx < masterToGlobal.size()) ? masterToGlobal[modIdx] : pluginIndex;
			return (ownerIdx << 24) | (rawFid & 0xFFFFFFu);
		}

		bool IsWorld(uint32_t formID) const {
			const uint32_t index = formID >> 24;
			const std::string& owner = index < masters.size() ? masters[index] : pluginName;
			return (formID & 0xFFFFFF) == worldID && SameName(owner, worldFile);
		}

		// The cell's grid position from its XCLC.
		bool CellGrid(uint64_t pos, const Header& h, int& x, int& y) {
			if (!RecordData(file, pos, h, raw, data, 0)) return false;
			uint32_t size = 0;
			const uint8_t* xclc = FindSub(data, Tag("XCLC"), size);
			if (!xclc || size < 8) return false;
			int32_t v[2];
			memcpy(v, xclc, 8);
			x = v[0]; y = v[1];
			// 2026-11-17: a cell far outside any real worldspace (a damaged or hand-made plugin) is left out: the map's size is
			// computed from the outermost cells, and a huge span wrapped it to a small one that the other cells then wrote outside of.
			const int limit = 2048;
			if (x < -limit || x > limit || y < -limit || y > limit) return false;
			return true;
		}

		void ParseTopLtexOrFoliage(uint64_t pos, uint64_t end, uint32_t expectTag) {
			while (pos + 24 <= end) {
				Header h;
				if (!file.ReadHeader(pos, h)) return;
				if (h.type == Tag("GRUP")) {
					if (h.size < 24) return;
					ParseTopLtexOrFoliage(pos + 24, pos + h.size, expectTag);
					pos += h.size;
					continue;
				}
				if (h.type == expectTag && !(h.label & RecordDeleted)) {
					const bool full = (h.label & RecordCompressed) != 0 || expectTag == Tag("LIGH");
					const size_t limit = full ? 0 : (expectTag == Tag("STAT") ? 288 : 128);
					if (RecordData(file, pos, h, raw, data, limit)) {
						const uint32_t fid = CanonicalFormID(h.groupType);
						if (expectTag == Tag("LIGH")) {
							uint32_t dsz = 0, esz = 0;
							const uint8_t* ldata = FindSub(data, Tag("DATA"), dsz);
							const uint8_t* edid = FindSub(data, Tag("EDID"), esz);
							std::string ename = (edid && esz > 1) ? LowerAscii(std::string((const char*)edid, strnlen((const char*)edid, esz))) : std::string();
							if (ldata && dsz >= 16 &&
								ename.find("amb") == std::string::npos && ename.find("vault") == std::string::npos && ename.find("cave") == std::string::npos) {
								uint32_t rad = 0, lflags = 0;
								memcpy(&rad, ldata + 4, 4);
								memcpy(&lflags, ldata + 12, 4);
								// Skip negative (0x04) and off-by-default (0x20) lights and tiny indoor accent bulbs (< 128)
								if (!(lflags & 0x24u) && rad >= 128u && rad <= 8192u) {
									auto toLin = [](uint8_t c) -> float {
										const float s = (float)c / 255.0f;
										return powf((std::max)(s, 0.0f), 2.2f);
									};
									const float lr = toLin(ldata[8]);
									const float lg = toLin(ldata[9]);
									const float lb = toLin(ldata[10]);
									if (lr + lg + lb > 0.06f) {
										const float billR = std::clamp((float)rad * 0.14f, 40.0f, 115.0f);
										const float gain = 3.2f;
										(*lightForms)[fid] = LightBase{ billR, lr * gain, lg * gain, lb * gain };
									}
								}
							}
						} else {
							uint32_t sz = 0, msz = 0;
							const uint8_t* edid = FindSub(data, Tag("EDID"), sz);
							if (edid && sz > 1) {
								std::string name((const char*)edid, strnlen((const char*)edid, sz));
								if (expectTag == Tag("LTEX")) {
									(*ltexSwatches)[fid] = ClassifyLtexSwatch(name.c_str());
								} else {
									const int fStyle = ClassifyFoliageEdid(name.c_str());
									if (fStyle >= 0) {
										(*foliageForms)[fid] = (uint8_t)fStyle;
									} else if (expectTag == Tag("STAT") && rockForms) {
										const uint8_t* modl = FindSub(data, Tag("MODL"), msz);
										std::string modStr = (modl && msz > 1) ? std::string((const char*)modl, strnlen((const char*)modl, msz)) : std::string();
										const int rStyle = ClassifyRockStat(name.c_str(), modStr.c_str());
										if (rStyle >= 0) (*rockForms)[fid] = (uint8_t)rStyle;
									}
								}
							}
						}
					}
				}
				pos += 24 + (uint64_t)h.size;
			}
		}

		// Inside the world's children: exterior cell blocks (4), sub-blocks (5), CELL records and their children (6, 8, 9).
		void Children(uint64_t pos, uint64_t end, bool haveCell, int cellX, int cellY, uint32_t cellID) {
			while (pos + 24 <= end) {
				Header h;
				if (!file.ReadHeader(pos, h)) return;
				if (h.type == Tag("GRUP")) {
					if (h.size < 24) return;
					const int32_t type = (int32_t)h.groupType;
					if (type == 4 || type == 5) Children(pos + 24, pos + h.size, false, 0, 0, 0);
					else if (type == 6 && ((haveCell && h.label == cellID) || (collectExtras && h.label == cellID)))
						Children(pos + 24, pos + h.size, haveCell, cellX, cellY, cellID);
					else if ((type == 8 || type == 9) && (haveCell || collectExtras))
						Children(pos + 24, pos + h.size, haveCell, cellX, cellY, cellID);
					pos += h.size;
					continue;
				}
				if (h.type == Tag("CELL")) {
					cellID = h.groupType;
					haveCell = !(h.label & RecordDeleted) && CellGrid(pos, h, cellX, cellY);
				}
				else if (h.type == Tag("LAND") && haveCell) {
					const int64_t key = ((int64_t)cellX << 32) | (uint32_t)cellY;
					if (h.label & RecordDeleted) lands.erase(key);
					else lands[key] = LandEntry{ cellX, cellY, pluginIndex, pos, h };
				}
				else if (collectExtras && h.type == Tag("REFR")) {
					const uint32_t refFid = CanonicalFormID(h.groupType);
					if (h.label & (RecordDeleted | RecordDisabled)) {
						foliageRefs->erase(refFid);
						if (rockRefs) rockRefs->erase(refFid);
						if (lightRefs) lightRefs->erase(refFid);
					} else if (RecordData(file, pos, h, raw, data, 96)) {
						uint32_t nsz = 0;
						const uint8_t* nameSub = FindSub(data, Tag("NAME"), nsz);
						if (nameSub && nsz >= 4) {
							uint32_t baseRaw = 0;
							memcpy(&baseRaw, nameSub, 4);
							const uint32_t baseFid = CanonicalFormID(baseRaw);
							bool wantFoliage = false, wantRock = false, wantLight = false;
							uint8_t style = 0;
							LightBase lb = {};
							if (foliageForms) {
								const auto fit = foliageForms->find(baseFid);
								if (fit != foliageForms->end()) { wantFoliage = true; style = fit->second; }
							}
							if (!wantFoliage && rockForms) {
								const auto rit = rockForms->find(baseFid);
								if (rit != rockForms->end()) { wantRock = true; style = rit->second; }
							}
							if (!wantFoliage && !wantRock && lightForms) {
								const auto lit = lightForms->find(baseFid);
								if (lit != lightForms->end()) { wantLight = true; lb = lit->second; }
							}
							if ((wantFoliage || wantRock || wantLight) && RecordData(file, pos, h, raw, data, 0)) {
								uint32_t dsz = 0, ssz = 0;
								const uint8_t* dataSub = FindSub(data, Tag("DATA"), dsz);
								if (dataSub && dsz >= 24) {
									float posRot[6];
									memcpy(posRot, dataSub, 24);
									float scale = 1.0f;
									const uint8_t* xscl = FindSub(data, Tag("XSCL"), ssz);
									if (xscl && ssz >= 4) memcpy(&scale, xscl, 4);
									if (wantFoliage && foliageRefs) {
										scale = std::clamp(scale, 0.35f, 3.0f);
										(*foliageRefs)[refFid] = FoliageRec{ posRot[0], posRot[1], posRot[2], posRot[5], scale, style };
									} else if (wantRock && rockRefs) {
										scale = std::clamp(scale, 0.50f, 4.0f);
										(*rockRefs)[refFid] = FoliageRec{ posRot[0], posRot[1], posRot[2], posRot[5], scale, style };
									} else if (wantLight && lightRefs) {
										(*lightRefs)[refFid] = LandHeights::NightLightRef{ posRot[0], posRot[1], posRot[2] + 32.0f, lb.radius, lb.r, lb.g, lb.b, 950.0f };
									}
								}
							}
						}
					}
				}
				pos += 24 + (uint64_t)h.size;
			}
		}

		bool Run(std::string& error) {
			Header h;
			if (!file.ReadHeader(0, h) || h.type != Tag("TES4")) { error = "not a plugin"; return false; }
			if (!RecordData(file, 0, h, raw, data, 0)) { error = "unreadable header"; return false; }
			for (size_t p = 0; p + 6 <= data.size();) {
				uint32_t t; uint16_t s;
				memcpy(&t, &data[p], 4);
				memcpy(&s, &data[p + 4], 2);
				p += 6;
				if (p + s > data.size()) break;
				if (t == Tag("MAST")) masters.emplace_back((const char*)&data[p], strnlen((const char*)&data[p], s));
				p += s;
			}
			masterToGlobal.resize(masters.size(), pluginIndex);
			for (size_t m = 0; m < masters.size(); ++m) {
				const auto it = pluginByName.find(LowerAscii(masters[m]));
				if (it != pluginByName.end()) masterToGlobal[m] = it->second;
			}
			uint64_t pos = 24 + (uint64_t)h.size;
			while (pos + 24 <= file.Size()) {
				if (!file.ReadHeader(pos, h) || h.type != Tag("GRUP") || h.size < 24) break;
				if (h.groupType == 0) {
					if (collectExtras && (h.label == Tag("LTEX") || h.label == Tag("STAT") || h.label == Tag("TREE") || h.label == Tag("LIGH"))) {
						ParseTopLtexOrFoliage(pos + 24, pos + h.size, h.label);
					}
					else if (h.label == Tag("WRLD")) {
						// WRLD records, each followed by its children's group (type 1, labelled with its form id).
						for (uint64_t p = pos + 24, end = pos + h.size; p + 24 <= end;) {
							Header w;
							if (!file.ReadHeader(p, w)) break;
							if (w.type == Tag("GRUP")) {
								if (w.size < 24) break;
								if ((int32_t)w.groupType == 1 && IsWorld(w.label)) Children(p + 24, p + w.size, false, 0, 0, 0);
								p += w.size;
							}
							else p += 24 + (uint64_t)w.size;
						}
					}
				}
				pos += h.size;
			}
			return true;
		}
	};

}	// namespace

bool LandHeights::Read(const std::vector<Plugin>& Plugins, const std::string& WorldFile, uint32_t WorldObjectID, uint32_t MaxResolution, Result& Out) {
	return ReadEx(Plugins, WorldFile, WorldObjectID, MaxResolution, Out, nullptr);
}

bool LandHeights::ReadEx(const std::vector<Plugin>& Plugins, const std::string& WorldFile, uint32_t WorldObjectID, uint32_t MaxResolution, Result& Out, ExtraOut* Extra) {
	Out = Result();
	if (Extra) *Extra = ExtraOut();

	std::unordered_map<std::string, uint32_t> pluginByName;
	std::vector<std::vector<uint32_t>> masterMaps(Plugins.size());
	for (uint32_t i = 0; i < (uint32_t)Plugins.size(); ++i) pluginByName[LowerAscii(Plugins[i].Name)] = i;

	std::unordered_map<int64_t, LandEntry> lands;
	std::unordered_map<uint32_t, uint32_t> ltexSwatches;
	std::unordered_map<uint32_t, uint8_t> foliageForms, rockForms;
	std::unordered_map<uint32_t, FoliageRec> foliageRefs, rockRefs;
	std::unordered_map<uint32_t, LightBase> lightForms;
	std::unordered_map<uint32_t, LandHeights::NightLightRef> lightRefs;
	std::vector<std::string> skipped;

	for (uint32_t i = 0; i < (uint32_t)Plugins.size(); i++) {
		Reader file("Data\\" + Plugins[i].Name);
		if (!file.Ok()) { skipped.push_back(Plugins[i].Name); continue; }
		Walker walker{ file, i, Plugins[i].Name, {}, {}, pluginByName, WorldFile, WorldObjectID, lands,
			Extra != nullptr, Extra ? &ltexSwatches : nullptr, Extra ? &foliageForms : nullptr, Extra ? &foliageRefs : nullptr,
			Extra ? &rockForms : nullptr, Extra ? &rockRefs : nullptr, Extra ? &lightForms : nullptr, Extra ? &lightRefs : nullptr };
		std::string error;
		if (!walker.Run(error)) skipped.push_back(Plugins[i].Name + " (" + error + ")");
		masterMaps[i] = std::move(walker.masterToGlobal);
	}
	if (lands.empty()) {
		Out.Message = "no land records for the worldspace in the load order";
		return false;
	}

	int west = INT32_MAX, south = INT32_MAX, east = INT32_MIN, north = INT32_MIN;
	for (const auto& l : lands) {
		west = (std::min)(west, l.second.x); east = (std::max)(east, l.second.x);
		south = (std::min)(south, l.second.y); north = (std::max)(north, l.second.y);
	}
	// 32 heights a cell (the 33rd row and column are the next cell's first), as xLODGen's maps.
	const uint32_t fullWidth = (uint32_t)(east - west + 1) * 32, fullHeight = (uint32_t)(north - south + 1) * 32;
	const uint32_t longer = (std::max)(fullWidth, fullHeight);
	if (!MaxResolution) MaxResolution = 1024;
	const uint32_t factor = longer > MaxResolution ? (longer + MaxResolution - 1) / MaxResolution : 1;
	Out.Width = (fullWidth + factor - 1) / factor;
	Out.Height = (fullHeight + factor - 1) / factor;
	const float noLand = -1.0e30f;
	Out.Heights.assign((size_t)Out.Width * Out.Height, 0.0f);
	Out.Peaks.assign((size_t)Out.Width * Out.Height, noLand);
	std::vector<uint16_t> counts((size_t)Out.Width * Out.Height, 0);

	// Optional half-res worldspace macro albedo buffer (VCLR x LTEX quad base swatches)
	uint32_t macroW = 0, macroH = 0, macroFactor = 0;
	std::vector<uint32_t> macroR, macroG, macroB;
	std::vector<uint16_t> macroCnt;
	if (Extra) {
		macroFactor = factor * 2;
		macroW = (fullWidth + macroFactor - 1) / macroFactor;
		macroH = (fullHeight + macroFactor - 1) / macroFactor;
		const size_t mn = (size_t)macroW * macroH;
		macroR.assign(mn, 0); macroG.assign(mn, 0); macroB.assign(mn, 0); macroCnt.assign(mn, 0);
	}

	// In plugin and file order, so each file is read front to back.
	std::vector<const LandEntry*> order;
	order.reserve(lands.size());
	for (const auto& l : lands) order.push_back(&l.second);
	std::sort(order.begin(), order.end(), [](const LandEntry* a, const LandEntry* b) { return a->plugin != b->plugin ? a->plugin < b->plugin : a->pos < b->pos; });

	std::vector<uint8_t> raw, data;
	std::unique_ptr<Reader> file;
	uint32_t openPlugin = UINT32_MAX, unreadable = 0;
	std::vector<uint32_t> btxtSizes;
	for (const LandEntry* l : order) {
		if (l->plugin != openPlugin) { file = std::make_unique<Reader>("Data\\" + Plugins[l->plugin].Name); openPlugin = l->plugin; }
		uint32_t size = 0;
		const uint8_t* vhgt = nullptr;
		const size_t partialLimit = Extra ? 0 : 8192;
		if (file->Ok() && RecordData(*file, l->pos, l->header, raw, data, partialLimit)) vhgt = FindSub(data, Tag("VHGT"), size);
		if (!vhgt && !Extra && file->Ok() && (l->header.label & RecordCompressed) && RecordData(*file, l->pos, l->header, raw, data, 0))
			vhgt = FindSub(data, Tag("VHGT"), size);
		if (!vhgt || size < 4 + 33 * 33) { unreadable++; continue; }

		const uint8_t* vclr = nullptr;
		uint32_t quadSwatches[4] = {
			(138u << 16) | (118u << 8) | 92u,
			(138u << 16) | (118u << 8) | 92u,
			(138u << 16) | (118u << 8) | 92u,
			(138u << 16) | (118u << 8) | 92u
		};
		if (Extra) {
			uint32_t vsz = 0;
			vclr = FindSub(data, Tag("VCLR"), vsz);
			if (vclr && vsz < 33 * 33 * 3) vclr = nullptr;
			const auto btxts = FindAllSubs(data, Tag("BTXT"), btxtSizes);
			const auto& mmap = masterMaps[l->plugin];
			for (size_t bi = 0; bi < btxts.size(); ++bi) {
				if (btxtSizes[bi] < 8) continue;
				uint32_t rawTex = 0;
				memcpy(&rawTex, btxts[bi], 4);
				const uint8_t quad = btxts[bi][4];
				if (quad < 4) {
					const uint32_t modIdx = rawTex >> 24;
					const uint32_t own = (modIdx < mmap.size()) ? mmap[modIdx] : l->plugin;
					const auto sw = ltexSwatches.find((own << 24) | (rawTex & 0xFFFFFFu));
					if (sw != ltexSwatches.end()) quadSwatches[quad] = sw->second;
				}
			}
		}

		float offset;
		memcpy(&offset, vhgt, 4);
		const int8_t* steps = (const int8_t*)(vhgt + 4);
		// Row 0 is the cell's south edge, column 0 its west edge; each row starts from the one below, each point from the one
		// to its west, in steps of 8 units.
		float rowStart = offset;
		for (int r = 0; r < 32; r++) {
			rowStart += steps[r * 33];
			float v = rowStart;
			const uint32_t globalRow = (uint32_t)(l->y - south) * 32 + r;          // from the south edge of the map
			const uint32_t imageRow = (fullHeight - 1 - globalRow) / factor;      // row 0 north
			const uint32_t macroRow = Extra ? ((fullHeight - 1 - globalRow) / macroFactor) : 0;
			for (int c = 0; c < 32; c++) {
				if (c) v += steps[r * 33 + c];
				const float h = v * 8.0f;
				const uint32_t globalCol = (uint32_t)(l->x - west) * 32 + c;
				const size_t k = (size_t)imageRow * Out.Width + (globalCol / factor);
				if (k >= counts.size()) continue;	// (2026-11-17: cannot happen with the cell range checked in CellGrid; kept as the last guard)
				Out.Heights[k] += h;
				Out.Peaks[k] = (std::max)(Out.Peaks[k], h);
				counts[k]++;

				if (Extra) {
					const size_t mk = (size_t)macroRow * macroW + (globalCol / macroFactor);
					if (mk < macroCnt.size()) {
						const uint32_t quad = ((r >= 16) ? 2u : 0u) | ((c >= 16) ? 1u : 0u);
						const uint32_t sw = quadSwatches[quad];
						uint32_t sr = (sw >> 16) & 0xFFu, sg = (sw >> 8) & 0xFFu, sb = sw & 0xFFu;
						if (vclr) {
							const size_t vi = ((size_t)r * 33 + c) * 3;
							sr = (sr * (uint32_t)vclr[vi + 0]) / 255u;
							sg = (sg * (uint32_t)vclr[vi + 1]) / 255u;
							sb = (sb * (uint32_t)vclr[vi + 2]) / 255u;
						}
						macroR[mk] += sr; macroG[mk] += sg; macroB[mk] += sb; macroCnt[mk]++;
					}
				}
			}
		}
		Out.Cells++;
	}
	file.reset();

	if (Extra && macroW && macroH) {
		Extra->MacroW = macroW;
		Extra->MacroH = macroH;
		Extra->MacroAlbedoRGBA.resize((size_t)macroW * macroH);
		for (size_t k = 0; k < macroCnt.size(); ++k) {
			if (macroCnt[k] > 0) {
				const uint32_t r = std::clamp(macroR[k] / macroCnt[k], 16u, 255u);
				const uint32_t g = std::clamp(macroG[k] / macroCnt[k], 16u, 255u);
				const uint32_t b = std::clamp(macroB[k] / macroCnt[k], 16u, 255u);
				Extra->MacroAlbedoRGBA[k] = (255u << 24) | (r << 16) | (g << 8) | b;
			} else {
				Extra->MacroAlbedoRGBA[k] = (0u << 24) | (132u << 16) | (114u << 8) | 88u;
			}
		}
		Extra->Foliage.reserve(foliageRefs.size());
		for (const auto& kv : foliageRefs) {
			const auto& fr = kv.second;
			Extra->Foliage.push_back(FoliageRef{ fr.x, fr.y, fr.z, fr.rotZ, fr.scale, fr.style });
		}

		Extra->Rocks.reserve(rockRefs.size());
		for (const auto& kv : rockRefs) {
			const auto& rr = kv.second;
			Extra->Rocks.push_back(RockRef{ rr.x, rr.y, rr.z, rr.rotZ, rr.scale, rr.style });
		}
		if (Extra->Rocks.size() > 16384u) {
			std::sort(Extra->Rocks.begin(), Extra->Rocks.end(), [](const RockRef& a, const RockRef& b) {
				if (a.style != b.style) return a.style > b.style; // cliffs (2) > canyon outcrops (1) > boulders (0)
				return a.scale > b.scale;
			});
			Extra->Rocks.resize(16384u);
		}

		// Spatial-dedup exterior LIGH REFRs on a 260-unit XY grid (keep brighter/larger light per cluster, max 600)
		if (!lightRefs.empty()) {
			std::unordered_map<int64_t, NightLightRef> cellBest;
			cellBest.reserve(lightRefs.size());
			for (const auto& kv : lightRefs) {
				const NightLightRef& nl = kv.second;
				const int32_t gx = (int32_t)floorf(nl.x / 260.0f);
				const int32_t gy = (int32_t)floorf(nl.y / 260.0f);
				const int64_t gkey = ((int64_t)gx << 32) | (uint32_t)gy;
				const float score = (nl.r + nl.g + nl.b) * nl.radius;
				auto it = cellBest.find(gkey);
				if (it == cellBest.end() || score > (it->second.r + it->second.g + it->second.b) * it->second.radius) {
					cellBest[gkey] = nl;
				}
			}
			Extra->NightLights.reserve(cellBest.size());
			for (const auto& kv : cellBest) Extra->NightLights.push_back(kv.second);
			if (Extra->NightLights.size() > 600u) {
				std::sort(Extra->NightLights.begin(), Extra->NightLights.end(), [](const NightLightRef& a, const NightLightRef& b) {
					return (a.r + a.g + a.b) * a.radius > (b.r + b.g + b.b) * b.radius;
				});
				Extra->NightLights.resize(600u);
			}
		}
	}

	float low = FLT_MAX, high = -FLT_MAX;
	for (size_t k = 0; k < counts.size(); k++) {
		if (!counts[k]) continue;
		Out.Heights[k] /= (float)counts[k];
		low = (std::min)(low, Out.Heights[k]);
		high = (std::max)(high, Out.Heights[k]);
	}
	if (!Out.Cells || low > high) {
		Out = Result();
		Out.Message = "the land records hold no heights";
		return false;
	}
	// Texels with no land (cells without a LAND record): the lowest land, so they cast nothing.
	for (size_t k = 0; k < counts.size(); k++) if (!counts[k]) { Out.Heights[k] = low; Out.Peaks[k] = low; }

	// Texel (c, r) of the full map is the vertex at west * 4096 + c * 128, (north + 1) * 4096 - 128 - r * 128 (TerrainShadows.cpp).
	Out.Texel = 128.0f * factor;
	Out.West = west * 4096.0f + 0.5f * (factor - 1) * 128.0f;
	Out.North = (north + 1) * 4096.0f - 128.0f - 0.5f * (factor - 1) * 128.0f;
	Out.Low = low;
	Out.High = high;
	Out.CellWest = west; Out.CellSouth = south; Out.CellEast = east; Out.CellNorth = north;
	char text[400];
	sprintf_s(text, "%u cells with land (%d,%d to %d,%d) from %u plugins%s, land heights %.0f to %.0f, %ux%u (%.0f units a texel)%s",
		Out.Cells, west, south, east, north, (unsigned)Plugins.size(), skipped.empty() ? "" : " (some unreadable)", low, high,
		Out.Width, Out.Height, Out.Texel, unreadable ? ", some land records without heights" : "");
	Out.Message = text;
	if (!skipped.empty()) {
		Out.Message += "; not read:";
		for (const std::string& s : skipped) Out.Message += " " + s;
	}
	return true;
}
