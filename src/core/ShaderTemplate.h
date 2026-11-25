#pragma once

struct ShaderTemplate {
	static constexpr int kMaxDefines = 128;	// UNOFFICIAL 2026-11-19: 128 (was 64; heavy configs with RT + dev switches overflowed 62 and dropped RT_SUN_FORWARD)
	const char* Name;
	D3DXMACRO Defines[kMaxDefines];
};
