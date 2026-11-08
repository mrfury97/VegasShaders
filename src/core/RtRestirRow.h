#pragma once
// UNOFFICIAL 2026-11-07 the path tracer's FRAME ROW (core\RtPathTrace.cpp, RtPathTrace.comp kFrameRow / FrameTexel): per-frame values the
// pass's full 512-byte constant block has no room for, written into one more row of its lamp image, and the remap of last frame's lamp
// numbers to this frame's (ReSTIR DI's reservoirs name lamps by their number in the list, which is sorted by distance and so changes as
// the camera moves). Plain C++, no game types: shared with tools/nvr-rt-host (scene `restirdi`), which tests it against the trace shader.
//
// The row, two floats a texel (RG32F):
//   texels 0-1  last frame's forward xyz, 1.y = 1 when last frame's reservoirs are valid (same rect, traced with PTRestirDI)
//   texels 2-3  last frame's right / P00 xyz;  4-5  its up / P11 xyz (as the pass's constants)
//   texels 6-7  last frame's eye in THIS frame's scene space (xyz); 7.y this frame's reservoir set (0 / 1)
//   texels 8-9  the camera's world position (the scene's origin in the world); 9.y log2 of the radiance cache's entries (0: none)
//   texel 10    PTRestirDIMaxM, PTRestirDISpatial;  11  PTRestirDIRadius, PTRestirDINormal;  12  PTRestirDIPlane, the cache's first word / 4096
//   texel 13    the cache's keys: PTCacheCell (the cell at 1000 units, world units), PTCacheMaxSamples (resolve cap)
//   texel 14    PTCacheFrame (frames since the helper start, for the cache's age), PTCacheMinSamples (a lookup's minimum)
//   texel 15    (RtPathTrace.cpp) the flashlight's cone;  texels 16-21 (2026-11-07 rtgap-tess) the bumps' slack + PTContactShadows
//               (RtTessContact.h PackRow, written after Build); texel 22 (2026-11-07 ptaaa-emissive, RtPathTrace.cpp after Build) PTEmissive's
//               instances and its scratch words' start / 4096 (RtEmissiveRow.h; the emissive row is the lamp image's next row, 50)
//   texels 32-287  PTSpecular's GGX energy table kGgxEnergy (512 values, two a texel; RtPathTrace.comp SpecE; with flag 1048576)
// The remap: the lamp image's row 0 holds lamp k at floats 8k .. 8k + 7 (pos.xy, pos.z + reach, colour.rg, colour.b + this): float 8j + 7 =
// 1 + this frame's number of last frame's lamp j (0: gone), matched by world position and reach (within 1 unit).
#include <cstring>
#include <cmath>

namespace RtRestirRow {
	constexpr int kMaxLamps = 256;			// RtPathTrace.cpp kLampsMax
	constexpr int kRowFloats = 2 * 16;		// the per-frame texels (0-15)
	constexpr int kEnergyTexel = 32;		// (2026-11-07) the GGX energy table's first texel
	constexpr int kRowFloatsAll = 2 * kEnergyTexel + 512;	// what Build writes in all (the row is wider)
	// (2026-11-07) PTSpecular's multiple-scattering compensation: E(nv, rough) of RtPathTrace.comp's GGX lobe (Fresnel 1), [rough j / 15][nv i / 31],
	// made by tools/nvr-rt-host/ggx_energy_table.py; written into the frame row at texels 32-287 (two values a texel)
	constexpr float kGgxEnergy[512] = {
		0.8945f, 0.9979f, 0.9995f, 0.9998f, 0.9999f, 0.9999f, 0.9999f, 0.9999f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f,
		0.9150f, 0.9886f, 0.9974f, 0.9989f, 0.9993f, 0.9996f, 0.9997f, 0.9998f, 0.9998f, 0.9999f, 0.9999f, 0.9999f, 0.9999f, 0.9999f, 0.9999f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f,
		0.9307f, 0.9033f, 0.9577f, 0.9795f, 0.9882f, 0.9925f, 0.9950f, 0.9963f, 0.9972f, 0.9977f, 0.9982f, 0.9984f, 0.9986f, 0.9988f, 0.9991f, 0.9991f, 0.9992f, 0.9993f, 0.9994f, 0.9994f, 0.9994f, 0.9995f, 0.9996f, 0.9995f, 0.9995f, 0.9996f, 0.9996f, 0.9996f, 0.9996f, 0.9996f, 0.9997f, 0.9997f,
		0.9326f, 0.8810f, 0.8953f, 0.9246f, 0.9481f, 0.9635f, 0.9733f, 0.9799f, 0.9845f, 0.9876f, 0.9899f, 0.9914f, 0.9928f, 0.9939f, 0.9945f, 0.9952f, 0.9958f, 0.9961f, 0.9965f, 0.9967f, 0.9969f, 0.9971f, 0.9973f, 0.9975f, 0.9979f, 0.9977f, 0.9979f, 0.9981f, 0.9982f, 0.9982f, 0.9982f, 0.9982f,
		0.9312f, 0.8931f, 0.8772f, 0.8834f, 0.8997f, 0.9159f, 0.9314f, 0.9441f, 0.9540f, 0.9621f, 0.9678f, 0.9724f, 0.9762f, 0.9794f, 0.9819f, 0.9837f, 0.9856f, 0.9868f, 0.9879f, 0.9890f, 0.9898f, 0.9906f, 0.9914f, 0.9918f, 0.9922f, 0.9925f, 0.9931f, 0.9934f, 0.9937f, 0.9942f, 0.9941f, 0.9942f,
		0.9269f, 0.8995f, 0.8794f, 0.8711f, 0.8727f, 0.8794f, 0.8901f, 0.9012f, 0.9123f, 0.9224f, 0.9307f, 0.9387f, 0.9449f, 0.9513f, 0.9561f, 0.9603f, 0.9639f, 0.9671f, 0.9696f, 0.9716f, 0.9736f, 0.9758f, 0.9771f, 0.9789f, 0.9802f, 0.9809f, 0.9820f, 0.9829f, 0.9835f, 0.9838f, 0.9850f, 0.9854f,
		0.9187f, 0.8985f, 0.8808f, 0.8675f, 0.8607f, 0.8593f, 0.8617f, 0.8670f, 0.8734f, 0.8807f, 0.8891f, 0.8970f, 0.9031f, 0.9108f, 0.9171f, 0.9228f, 0.9277f, 0.9335f, 0.9376f, 0.9419f, 0.9454f, 0.9482f, 0.9514f, 0.9538f, 0.9562f, 0.9593f, 0.9606f, 0.9622f, 0.9640f, 0.9653f, 0.9667f, 0.9678f,
		0.9060f, 0.8895f, 0.8746f, 0.8612f, 0.8520f, 0.8449f, 0.8417f, 0.8405f, 0.8431f, 0.8458f, 0.8497f, 0.8546f, 0.8598f, 0.8657f, 0.8713f, 0.8771f, 0.8826f, 0.8883f, 0.8936f, 0.8983f, 0.9023f, 0.9079f, 0.9117f, 0.9149f, 0.9189f, 0.9218f, 0.9246f, 0.9278f, 0.9307f, 0.9328f, 0.9351f, 0.9363f,
		0.8885f, 0.8745f, 0.8600f, 0.8485f, 0.8367f, 0.8287f, 0.8217f, 0.8180f, 0.8145f, 0.8130f, 0.8135f, 0.8153f, 0.8171f, 0.8204f, 0.8232f, 0.8268f, 0.8319f, 0.8355f, 0.8401f, 0.8450f, 0.8493f, 0.8526f, 0.8577f, 0.8618f, 0.8656f, 0.8696f, 0.8742f, 0.8775f, 0.8805f, 0.8840f, 0.8868f, 0.8899f,
		0.8648f, 0.8510f, 0.8382f, 0.8253f, 0.8144f, 0.8051f, 0.7969f, 0.7901f, 0.7852f, 0.7805f, 0.7773f, 0.7761f, 0.7739f, 0.7754f, 0.7752f, 0.7762f, 0.7778f, 0.7791f, 0.7819f, 0.7853f, 0.7880f, 0.7913f, 0.7936f, 0.7974f, 0.8007f, 0.8039f, 0.8079f, 0.8107f, 0.8143f, 0.8184f, 0.8212f, 0.8239f,
		0.8346f, 0.8211f, 0.8076f, 0.7958f, 0.7849f, 0.7740f, 0.7651f, 0.7558f, 0.7485f, 0.7431f, 0.7370f, 0.7323f, 0.7279f, 0.7264f, 0.7237f, 0.7221f, 0.7208f, 0.7199f, 0.7203f, 0.7200f, 0.7211f, 0.7221f, 0.7232f, 0.7253f, 0.7262f, 0.7283f, 0.7314f, 0.7326f, 0.7346f, 0.7376f, 0.7405f, 0.7434f,
		0.7995f, 0.7861f, 0.7707f, 0.7581f, 0.7464f, 0.7341f, 0.7241f, 0.7147f, 0.7059f, 0.6984f, 0.6912f, 0.6839f, 0.6789f, 0.6734f, 0.6683f, 0.6655f, 0.6608f, 0.6574f, 0.6553f, 0.6526f, 0.6510f, 0.6496f, 0.6495f, 0.6490f, 0.6484f, 0.6472f, 0.6483f, 0.6484f, 0.6486f, 0.6494f, 0.6503f, 0.6505f,
		0.7586f, 0.7426f, 0.7280f, 0.7143f, 0.7013f, 0.6888f, 0.6771f, 0.6678f, 0.6576f, 0.6479f, 0.6389f, 0.6300f, 0.6231f, 0.6165f, 0.6101f, 0.6045f, 0.5981f, 0.5942f, 0.5882f, 0.5839f, 0.5802f, 0.5769f, 0.5729f, 0.5707f, 0.5687f, 0.5642f, 0.5633f, 0.5617f, 0.5584f, 0.5569f, 0.5563f, 0.5551f,
		0.7127f, 0.6968f, 0.6804f, 0.6654f, 0.6520f, 0.6384f, 0.6250f, 0.6149f, 0.6025f, 0.5927f, 0.5821f, 0.5724f, 0.5647f, 0.5558f, 0.5473f, 0.5395f, 0.5334f, 0.5264f, 0.5206f, 0.5141f, 0.5097f, 0.5030f, 0.4968f, 0.4937f, 0.4884f, 0.4838f, 0.4800f, 0.4762f, 0.4717f, 0.4690f, 0.4654f, 0.4629f,
		0.6642f, 0.6460f, 0.6284f, 0.6140f, 0.5977f, 0.5839f, 0.5703f, 0.5576f, 0.5452f, 0.5344f, 0.5232f, 0.5136f, 0.5023f, 0.4938f, 0.4841f, 0.4770f, 0.4688f, 0.4605f, 0.4518f, 0.4446f, 0.4395f, 0.4322f, 0.4258f, 0.4198f, 0.4134f, 0.4079f, 0.4035f, 0.3976f, 0.3934f, 0.3886f, 0.3828f, 0.3797f,
		0.6130f, 0.5951f, 0.5764f, 0.5594f, 0.5434f, 0.5288f, 0.5132f, 0.5002f, 0.4880f, 0.4754f, 0.4645f, 0.4526f, 0.4420f, 0.4315f, 0.4226f, 0.4132f, 0.4044f, 0.3960f, 0.3889f, 0.3805f, 0.3721f, 0.3648f, 0.3587f, 0.3523f, 0.3451f, 0.3393f, 0.3338f, 0.3273f, 0.3225f, 0.3171f, 0.3117f, 0.3065f
	};

	struct Keys {
		float maxM = 20.0f, spatial = 2.0f, radius = 16.0f, normal = 0.9f, plane = 0.05f;	// (2026-11-07 perf-ptcost: spatial 2, was 4; RT-PATH-TRACING.md "perf-ptcost")
		float cacheLog2 = 0.0f, cacheBase4096 = 0.0f, cacheCell = 0.0f, cacheMaxSamples = 0.0f, cacheMinSamples = 0.0f;
		float frame = 0.0f;
	};

	struct State {
		bool		valid = false;
		float		forward[3] = {}, right[3] = {}, up[3] = {};
		double		eye[3] = {};			// last frame's camera, world
		unsigned	rect[2] = {};
		int			parity = 0;
		int			lamps = 0;
		float		lampWorld[kMaxLamps][4] = {};
	};

	// Fills `row` (kRowFloats, zeroed here) and the remap in `lampRow`, then makes this frame "last frame". lamps: lampCount entries of `stride`
	// floats (scene-space xyz, reach); cam: the camera's world position; eye: the scene's eye (RtCore::Eye(), scene = world - cam + eye).
	inline void Build(State& r, float* row, float* lampRow, const float* lamps, int lampCount, int stride, const double cam[3], const float eye[3],
		const float forward[3], const float right[3], const float up[3], unsigned tw, unsigned th, const Keys& keys) {
		std::memset(row, 0, sizeof(float) * kRowFloatsAll);
		std::memcpy(row + 2 * kEnergyTexel, kGgxEnergy, sizeof(kGgxEnergy));
		if (lampCount > kMaxLamps) lampCount = kMaxLamps;
		auto world = [&](int k, int a) { return (float)((double)lamps[k * stride + a] + cam[a] - eye[a]); };
		if (r.valid && r.rect[0] == tw && r.rect[1] == th) {
			for (int a = 0; a < 3; a++) {
				row[a] = r.forward[a];
				row[4 + a] = r.right[a];
				row[8 + a] = r.up[a];
				row[12 + a] = (float)(r.eye[a] - cam[a]) + eye[a];
			}
			row[3] = 1.0f;
			for (int j = 0; j < r.lamps; j++) {
				int found = 0;
				for (int k = 0; k < lampCount && !found; k++)
					if (std::fabs(world(k, 0) - r.lampWorld[j][0]) < 1.0f && std::fabs(world(k, 1) - r.lampWorld[j][1]) < 1.0f &&
						std::fabs(world(k, 2) - r.lampWorld[j][2]) < 1.0f && std::fabs(lamps[k * stride + 3] - r.lampWorld[j][3]) < 1.0f) found = k + 1;
				lampRow[8 * j + 7] = (float)found;
			}
		}
		r.parity ^= 1;
		row[15] = (float)r.parity;
		row[16] = (float)cam[0]; row[17] = (float)cam[1]; row[18] = (float)cam[2]; row[19] = keys.cacheLog2;
		row[20] = keys.maxM; row[21] = keys.spatial;
		row[22] = keys.radius; row[23] = keys.normal;
		row[24] = keys.plane; row[25] = keys.cacheBase4096;
		row[26] = keys.cacheCell; row[27] = keys.cacheMaxSamples;
		row[28] = keys.frame; row[29] = keys.cacheMinSamples;
		for (int a = 0; a < 3; a++) { r.forward[a] = forward[a]; r.right[a] = right[a]; r.up[a] = up[a]; r.eye[a] = cam[a]; }
		r.rect[0] = tw; r.rect[1] = th;
		r.lamps = lampCount;
		for (int k = 0; k < lampCount; k++) { for (int a = 0; a < 3; a++) r.lampWorld[k][a] = world(k, a); r.lampWorld[k][3] = lamps[k * stride + 3]; }
		r.valid = true;
	}
}
