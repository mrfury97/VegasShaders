// UNOFFICIAL 2026-11-18 offline check of src/core/DdsTranscode.h (no game). Build (x86 Native Tools prompt):
//   cl /O2 /EHsc /std:c++17 tests\dds_transcode.cpp /Fe:<scratch>\dds_transcode.exe
// dds_transcode <in.dds> <out_raw_rgba> <out_legacy.dds>: writes the top mip level as decoded by DecodeBC7 (raw RGBA, row by row) and the
// file ToLegacy makes. tests/dds_transcode_check.py compares the first with Pillow's decode of the input (must be exact) and decodes
// the second to measure the BC3 re-encode (PSNR per channel).
#include "../src/core/DdsTranscode.h"
#include <fstream>
#include <iterator>
#include <cstdio>

int main(int argc, char** argv) {
	if (argc < 4) { printf("usage: dds_transcode in.dds out.rgba out.dds\n"); return 2; }
	std::ifstream in(argv[1], std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() < 148) { printf("too short\n"); return 1; }
	const uint8_t* src = (const uint8_t*)bytes.data();
	const uint32_t h = DdsTranscode::Rd(src + 12), w = DdsTranscode::Rd(src + 16), dxgi = DdsTranscode::Rd(src + 128);
	if (dxgi >= 97 && dxgi <= 99) {
		std::vector<uint8_t> rgba((size_t)w * h * 4);
		const uint8_t* data = src + 148;
		const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
		uint8_t px[16][4];
		for (uint32_t by = 0; by < bh; by++)
			for (uint32_t bx = 0; bx < bw; bx++) {
				DdsTranscode::DecodeBC7(data + 16 * ((size_t)by * bw + bx), px);
				for (int i = 0; i < 16; i++) {
					const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
					if (x < w && y < h) memcpy(&rgba[4 * ((size_t)y * w + x)], px[i], 4);
				}
			}
		std::ofstream(argv[2], std::ios::binary).write((const char*)rgba.data(), rgba.size());
	}
	char why[256];
	const char* note = DdsTranscode::ToLegacy(bytes, why, sizeof(why));
	printf("%ux%u dxgi %u: %s\n", w, h, dxgi, note ? note : "(not a DX10 DDS)");
	std::ofstream(argv[3], std::ios::binary).write(bytes.data(), bytes.size());
	return 0;
}
