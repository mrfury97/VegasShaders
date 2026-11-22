#pragma once
// UNOFFICIAL 2026-11-18 DDS files with a DX10 header for the _rmaos maps (Render.cpp RmaosDraw). D3DX9, which makes the maps into
// textures, reads only the old DDS header, and Direct3D 9 has no BC7 format at all, so a modder's map saved as BC7 (the usual choice
// in today's tools) never loaded ("[Rmaos] could not load"), and the object drew as if it had no map. ToLegacy, on the map reader's
// worker thread, rewrites such a file in memory as an old-header DDS that D3DX9 reads:
//   BC7 (DXGI 97-99): every 4x4 block decoded and encoded again as BC3 (DXT5), mip by mip: the same memory as a BC3 map;
//   BC1 / BC2 / BC3 (DXGI 70-78), R8G8B8A8 / B8G8R8A8 (27-29, 87, 90, 91): the same data under the old header.
// Anything else (cube maps, arrays, other formats) is left as it was and fails to load as before, now with the reason in the log.
// Own code. The BC7 partition and anchor tables were read back from Pillow's BC7 decoder with probe blocks (2026-11-18) and match the
// format's published tables; the decoder is checked bit for bit against Pillow on texconv-made BC7 files (tests/dds_transcode.cpp).
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

namespace DdsTranscode {
	inline const uint16_t kPart2[64] = {
		0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
		0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
		0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
		0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
		0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
		0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
		0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
		0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
	};
	inline const uint32_t kPart3[64] = {	// 2 bits a pixel
		0xAA685050u, 0x6A5A5040u, 0x5A5A4200u, 0x5450A0A8u, 0xA5A50000u, 0xA0A05050u, 0x5555A0A0u, 0x5A5A5050u,
		0xAA550000u, 0xAA555500u, 0xAAAA5500u, 0x90909090u, 0x94949494u, 0xA4A4A4A4u, 0xA9A59450u, 0x2A0A4250u,
		0xA5945040u, 0x0A425054u, 0xA5A5A500u, 0x55A0A0A0u, 0xA8A85454u, 0x6A6A4040u, 0xA4A45000u, 0x1A1A0500u,
		0x0050A4A4u, 0xAAA59090u, 0x14696914u, 0x69691400u, 0xA08585A0u, 0xAA821414u, 0x50A4A450u, 0x6A5A0200u,
		0xA9A58000u, 0x5090A0A8u, 0xA8A09050u, 0x24242424u, 0x00AA5500u, 0x24924924u, 0x24499224u, 0x50A50A50u,
		0x500AA550u, 0xAAAA4444u, 0x66660000u, 0xA5A0A5A0u, 0x50A050A0u, 0x69286928u, 0x44AAAA44u, 0x66666600u,
		0xAA444444u, 0x54A854A8u, 0x95809580u, 0x96969600u, 0xA85454A8u, 0x80959580u, 0xAA141414u, 0x96960000u,
		0xAAAA1414u, 0xA05050A0u, 0xA0A5A5A0u, 0x96000000u, 0x40804080u, 0xA9A8A9A8u, 0xAAAAAA44u, 0x2A4A5254u,
	};
	inline const uint8_t kAnchor2[64] = {
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
		15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
		6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
	};
	inline const uint8_t kAnchor3a[64] = {
		3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
		3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
		8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
		3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
	};
	inline const uint8_t kAnchor3b[64] = {
		15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
		15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
		15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
		15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
	};

	struct Bits {
		const uint8_t* p; unsigned pos = 0;
		explicit Bits(const uint8_t* block) : p(block) {}
		unsigned Read(unsigned n) {
			unsigned v = 0;
			for (unsigned i = 0; i < n; i++, pos++) v |= ((p[pos >> 3] >> (pos & 7)) & 1u) << i;
			return v;
		}
	};

	inline uint8_t Expand(unsigned v, unsigned bits) { v <<= (8 - bits); return (uint8_t)(v | (v >> bits)); }
	inline uint8_t Lerp(uint8_t a, uint8_t b, unsigned w) { return (uint8_t)(((64 - w) * a + w * b + 32) >> 6); }
	inline const uint8_t* Weights(unsigned bits) {
		static const uint8_t w2[] = { 0, 21, 43, 64 }, w3[] = { 0, 9, 18, 27, 37, 46, 55, 64 },
			w4[] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
		return bits == 2 ? w2 : bits == 3 ? w3 : w4;
	}

	// One BC7 block to 16 RGBA pixels (row by row).
	inline void DecodeBC7(const uint8_t* block, uint8_t out[16][4]) {
		//                         NS PB RB ISB CB AB EPB SPB IB IB2
		static const uint8_t M[8][10] = {
			{ 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 }, { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 }, { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 }, { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
			{ 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 }, { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 }, { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 }, { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 } };
		unsigned mode = 0;
		while (mode < 8 && !((block[0] >> mode) & 1)) mode++;
		if (mode == 8) { memset(out, 0, 64); return; }	// reserved: transparent black
		const uint8_t* m = M[mode];
		const unsigned ns = m[0], cb = m[4], ab = m[5], ib = m[8], ib2 = m[9];
		Bits b(block);
		b.Read(mode + 1);
		const unsigned part = b.Read(m[1]), rot = b.Read(m[2]), isb = b.Read(m[3]);
		unsigned ep[6][4] = {};
		for (unsigned c = 0; c < 3; c++) for (unsigned e = 0; e < ns * 2; e++) ep[e][c] = b.Read(cb);
		if (ab) for (unsigned e = 0; e < ns * 2; e++) ep[e][3] = b.Read(ab);
		unsigned pb[6] = {};
		const bool hasP = m[6] || m[7];
		if (m[6]) for (unsigned e = 0; e < ns * 2; e++) pb[e] = b.Read(1);
		if (m[7]) for (unsigned s = 0; s < ns; s++) pb[s * 2] = pb[s * 2 + 1] = b.Read(1);
		uint8_t e8[6][4] = {};
		for (unsigned e = 0; e < ns * 2; e++) {
			for (unsigned c = 0; c < 3; c++) e8[e][c] = hasP ? Expand((ep[e][c] << 1) | pb[e], cb + 1) : Expand(ep[e][c], cb);
			e8[e][3] = !ab ? 255 : hasP ? Expand((ep[e][3] << 1) | pb[e], ab + 1) : Expand(ep[e][3], ab);
		}
		unsigned subset[16], anchor[3] = { 0, 0, 0 };
		for (unsigned i = 0; i < 16; i++) subset[i] = ns == 1 ? 0 : ns == 2 ? (kPart2[part] >> i) & 1 : (kPart3[part] >> (2 * i)) & 3;
		if (ns == 2) anchor[1] = kAnchor2[part];
		if (ns == 3) { anchor[1] = kAnchor3a[part]; anchor[2] = kAnchor3b[part]; }
		unsigned idx[16], idx2[16] = {};
		for (unsigned i = 0; i < 16; i++) idx[i] = b.Read(i == anchor[subset[i]] ? ib - 1 : ib);
		if (ib2) for (unsigned i = 0; i < 16; i++) idx2[i] = b.Read(i == 0 ? ib2 - 1 : ib2);
		for (unsigned i = 0; i < 16; i++) {
			const uint8_t* a = e8[subset[i] * 2];
			const uint8_t* z = e8[subset[i] * 2 + 1];
			unsigned cIdx = idx[i], cBits = ib, aIdx = idx[i], aBits = ib;
			if (ib2) {
				if (isb) { cIdx = idx2[i]; cBits = ib2; }
				else { aIdx = idx2[i]; aBits = ib2; }
			}
			const unsigned cw = Weights(cBits)[cIdx], aw = Weights(aBits)[aIdx];
			uint8_t px[4] = { Lerp(a[0], z[0], cw), Lerp(a[1], z[1], cw), Lerp(a[2], z[2], cw), Lerp(a[3], z[3], aw) };
			if (rot) std::swap(px[3], px[rot - 1]);
			memcpy(out[i], px, 4);
		}
	}

	inline uint16_t To565(float r, float g, float b) {
		const int R = std::clamp((int)(r * 31.0f / 255.0f + 0.5f), 0, 31), G = std::clamp((int)(g * 63.0f / 255.0f + 0.5f), 0, 63),
			B = std::clamp((int)(b * 31.0f / 255.0f + 0.5f), 0, 31);
		return (uint16_t)((R << 11) | (G << 5) | B);
	}
	inline void From565(uint16_t c, int out[3]) {
		const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
		out[0] = (r << 3) | (r >> 2); out[1] = (g << 2) | (g >> 4); out[2] = (b << 3) | (b >> 2);
	}
	// The colour half of a BC3 block for two endpoints: indices by the nearest palette entry (4-colour order, c0 > c1); the error.
	inline unsigned ColourIndices(const uint8_t px[16][4], uint16_t& c0, uint16_t& c1, uint32_t& bits) {
		if (c0 < c1) std::swap(c0, c1);
		int p[4][3];
		From565(c0, p[0]); From565(c1, p[1]);
		for (int c = 0; c < 3; c++) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
		unsigned total = 0; bits = 0;
		for (int i = 0; i < 16; i++) {
			unsigned best = ~0u, bi = 0;
			for (unsigned k = 0; k < (c0 == c1 ? 1u : 4u); k++) {
				unsigned d = 0;
				for (int c = 0; c < 3; c++) { const int e = px[i][c] - p[k][c]; d += (unsigned)(e * e); }
				if (d < best) { best = d; bi = k; }
			}
			total += best; bits |= bi << (2 * i);
		}
		return total;
	}

	// 16 RGBA pixels to one BC3 block: alpha by its range, colour along its main axis, then one least-squares refit, kept if better.
	inline void EncodeBC3(const uint8_t px[16][4], uint8_t* out) {
		uint8_t amin = 255, amax = 0;
		for (int i = 0; i < 16; i++) { amin = (std::min)(amin, px[i][3]); amax = (std::max)(amax, px[i][3]); }
		out[0] = amax; out[1] = amin;
		uint64_t abits = 0;
		if (amax > amin) {
			int pal[8] = { amax, amin };
			for (int k = 1; k <= 6; k++) pal[k + 1] = ((7 - k) * amax + k * amin) / 7;
			for (int i = 0; i < 16; i++) {
				int best = 1 << 30, bi = 0;
				for (int k = 0; k < 8; k++) { const int d = abs(px[i][3] - pal[k]); if (d < best) { best = d; bi = k; } }
				abits |= (uint64_t)bi << (3 * i);
			}
		}
		for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(abits >> (8 * k));

		float mean[3] = {};
		for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) mean[c] += px[i][c] / 16.0f;
		float cov[6] = {};	// rr rg rb gg gb bb
		for (int i = 0; i < 16; i++) {
			const float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
			cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2]; cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
		}
		float axis[3] = { 0.9f, 1.0f, 0.7f };
		for (int it = 0; it < 8; it++) {
			const float n[3] = { cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
				cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
			const float len = (std::max)((std::max)(fabsf(n[0]), fabsf(n[1])), fabsf(n[2]));
			if (len < 1e-6f) break;
			for (int c = 0; c < 3; c++) axis[c] = n[c] / len;
		}
		float lo = 1e30f, hi = -1e30f;
		for (int i = 0; i < 16; i++) {
			const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
			lo = (std::min)(lo, t); hi = (std::max)(hi, t);
		}
		const float al = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
		if (al > 1e-12f) { lo /= al; hi /= al; }
		uint16_t c0 = To565(mean[0] + axis[0] * hi, mean[1] + axis[1] * hi, mean[2] + axis[2] * hi);
		uint16_t c1 = To565(mean[0] + axis[0] * lo, mean[1] + axis[1] * lo, mean[2] + axis[2] * lo);
		uint32_t cbits;
		unsigned err = ColourIndices(px, c0, c1, cbits);
		if (c0 != c1 && err > 0) {	// refit: the endpoints that best explain the chosen indices (least squares)
			static const float w0[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };	// share of c0 per index
			float aa = 0, bb = 0, ab = 0, ax[3] = {}, bx[3] = {};
			for (int i = 0; i < 16; i++) {
				const float a = w0[(cbits >> (2 * i)) & 3], b = 1.0f - a;
				aa += a * a; bb += b * b; ab += a * b;
				for (int c = 0; c < 3; c++) { ax[c] += a * px[i][c]; bx[c] += b * px[i][c]; }
			}
			const float det = aa * bb - ab * ab;
			if (fabsf(det) > 1e-6f) {
				float e0[3], e1[3];
				for (int c = 0; c < 3; c++) { e0[c] = (ax[c] * bb - bx[c] * ab) / det; e1[c] = (bx[c] * aa - ax[c] * ab) / det; }
				uint16_t r0 = To565(e0[0], e0[1], e0[2]), r1 = To565(e1[0], e1[1], e1[2]);
				uint32_t rbits;
				const unsigned rerr = ColourIndices(px, r0, r1, rbits);
				if (rerr < err) { c0 = r0; c1 = r1; cbits = rbits; err = rerr; }
			}
		}
		out[8] = (uint8_t)c0; out[9] = (uint8_t)(c0 >> 8); out[10] = (uint8_t)c1; out[11] = (uint8_t)(c1 >> 8);
		for (int k = 0; k < 4; k++) out[12 + k] = (uint8_t)(cbits >> (8 * k));
	}

	inline uint32_t Rd(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
	inline void Wr(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

	// A DDS with a DX10 header made readable by D3DX9 (see the top). nullptr: not a DX10 DDS (bytes untouched). Otherwise a line for
	// the log in why: what was done, or why it was left alone (bytes untouched then too; the load fails as it did).
	inline const char* ToLegacy(std::vector<char>& bytes, char* why, size_t whySize) {
		const size_t kHead = 4 + 124, kDx10 = 20;
		if (bytes.size() < kHead + kDx10) return nullptr;
		const uint8_t* src = (const uint8_t*)bytes.data();
		if (memcmp(src, "DDS ", 4) || Rd(src + 4) != 124 || !(Rd(src + 4 + 76) & 0x4) || memcmp(src + 4 + 80, "DX10", 4)) return nullptr;
		const uint8_t* dx = src + kHead;
		const uint32_t dxgi = Rd(dx), dim = Rd(dx + 4), misc = Rd(dx + 8), arraySize = Rd(dx + 12);
		const uint32_t height = Rd(src + 12), width = Rd(src + 16);
		uint32_t mips = (Rd(src + 8) & 0x20000) ? Rd(src + 28) : 1;
		if (!mips) mips = 1;
		if (dim != 3 || (misc & 4) || arraySize > 1 || !width || !height || width > 16384 || height > 16384 || mips > 16) {
			snprintf(why, whySize, "a DX10 DDS that is not one 2D texture (dimension %u, cube %u, array %u, %ux%u, %u mips): not converted",
				dim, (misc & 4) ? 1u : 0u, arraySize, width, height, mips);
			return why;
		}
		const char* fourCC = nullptr; uint32_t masks[4] = {}; bool bc7 = false;
		switch (dxgi) {
			case 97: case 98: case 99: bc7 = true; fourCC = "DXT5"; break;
			case 70: case 71: case 72: fourCC = "DXT1"; break;
			case 73: case 74: case 75: fourCC = "DXT3"; break;
			case 76: case 77: case 78: fourCC = "DXT5"; break;
			case 27: case 28: case 29: masks[0] = 0xFFu; masks[1] = 0xFF00u; masks[2] = 0xFF0000u; masks[3] = 0xFF000000u; break;
			case 87: case 90: case 91: masks[0] = 0xFF0000u; masks[1] = 0xFF00u; masks[2] = 0xFFu; masks[3] = 0xFF000000u; break;
			default:
				snprintf(why, whySize, "a DX10 DDS in DXGI format %u, which this game cannot use (save it as BC7 or BC3 / DXT5)", dxgi);
				return why;
		}
		size_t need = 0;	// every mip level, checked against the file
		const size_t blockBytes = (dxgi >= 70 && dxgi <= 72) ? 8 : 16;
		for (uint32_t l = 0; l < mips; l++) {
			const size_t w = (std::max)(width >> l, 1u), h = (std::max)(height >> l, 1u);
			need += fourCC ? ((w + 3) / 4) * ((h + 3) / 4) * blockBytes : w * h * 4;
		}
		if (bytes.size() < kHead + kDx10 + need) {
			snprintf(why, whySize, "a DX10 DDS shorter than its %ux%u and %u mip levels need: not converted", width, height, mips);
			return why;
		}
		std::vector<char> out(kHead + need);
		uint8_t* dst = (uint8_t*)out.data();
		memcpy(dst, src, kHead);
		uint8_t* pf = dst + 4 + 72;	// DDS_PIXELFORMAT
		Wr(pf + 0, 32);
		if (fourCC) { Wr(pf + 4, 0x4); memcpy(pf + 8, fourCC, 4); Wr(pf + 12, 0); for (int i = 0; i < 4; i++) Wr(pf + 16 + 4 * i, 0); }
		else { Wr(pf + 4, 0x40 | 0x1); Wr(pf + 8, 0); Wr(pf + 12, 32); for (int i = 0; i < 4; i++) Wr(pf + 16 + 4 * i, masks[i]); }
		const uint8_t* data = src + kHead + kDx10;
		if (!bc7) memcpy(dst + kHead, data, need);
		else {
			uint8_t px[16][4];
			for (size_t off = 0; off < need; off += 16) { DecodeBC7(data + off, px); EncodeBC3(px, dst + kHead + off); }
		}
		bytes.swap(out);
		snprintf(why, whySize, bc7 ? "BC7, converted to BC3 on load (%ux%u, %u mip levels)" : "a DX10 DDS, its header rewritten as the old one (%ux%u, %u mip levels)",
			width, height, mips);
		return why;
	}
}
