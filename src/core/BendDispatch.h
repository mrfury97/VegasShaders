#pragma once
// UNOFFICIAL: Bend Studio's screen-space shadow dispatch planner (bend_sss_cpu.h, BuildDispatchList), as Community
// Shaders uses it for its Screen-Space Shadows. Copyright 2023 Sony Interactive Entertainment, Apache License 2.0
// (http://www.apache.org/licenses/LICENSE-2.0). Unchanged apart from the formatting; Compute/BendContact.comp (core/BendContact.h)
// is its shader.

namespace Bend {

	struct DispatchData {
		int WaveCount[3];          // Dispatch(X, Y, Z) wave counts
		int WaveOffset_Shader[2];  // passed to the shader, different for each dispatch
	};

	struct DispatchList {
		float LightCoordinate_Shader[4];  // passed to the shader, the same for every dispatch of this light
		DispatchData Dispatch[8];
		int DispatchCount;
	};

	inline int bend_min(const int a, const int b) { return a > b ? b : a; }
	inline int bend_max(const int a, const int b) { return a > b ? a : b; }

	// inLightProjection: {light} * ViewProjection without the w divide (a direction: w 0). No GPU sync is needed
	// between the dispatches.
	inline DispatchList BuildDispatchList(const float inLightProjection[4], const int inViewportSize[2], const int inMinRenderBounds[2], const int inMaxRenderBounds[2], bool inExpandedZRange = false, int inWaveSize = 64) {
		DispatchList result = {};

		float xy_light_w = inLightProjection[3];
		float FP_limit = 0.000002f * (float)inWaveSize;
		if (xy_light_w >= 0 && xy_light_w < FP_limit) xy_light_w = FP_limit;
		else if (xy_light_w < 0 && xy_light_w > -FP_limit) xy_light_w = -FP_limit;

		result.LightCoordinate_Shader[0] = ((inLightProjection[0] / xy_light_w) * +0.5f + 0.5f) * (float)inViewportSize[0];
		result.LightCoordinate_Shader[1] = ((inLightProjection[1] / xy_light_w) * -0.5f + 0.5f) * (float)inViewportSize[1];
		result.LightCoordinate_Shader[2] = inLightProjection[3] == 0 ? 0 : (inLightProjection[2] / inLightProjection[3]);
		result.LightCoordinate_Shader[3] = inLightProjection[3] > 0 ? 1.0f : -1.0f;
		if (inExpandedZRange) result.LightCoordinate_Shader[2] = result.LightCoordinate_Shader[2] * 0.5f + 0.5f;

		int light_xy[2] = { (int)(result.LightCoordinate_Shader[0] + 0.5f), (int)(result.LightCoordinate_Shader[1] + 0.5f) };

		const int biased_bounds[4] = {
			inMinRenderBounds[0] - light_xy[0],
			-(inMaxRenderBounds[1] - light_xy[1]),
			inMaxRenderBounds[0] - light_xy[0],
			-(inMinRenderBounds[1] - light_xy[1]),
		};

		for (int q = 0; q < 4; q++) {
			bool vertical = q == 0 || q == 3;
			const int bounds[4] = {
				bend_max(0, ((q & 1) ? biased_bounds[0] : -biased_bounds[2])) / inWaveSize,
				bend_max(0, ((q & 2) ? biased_bounds[1] : -biased_bounds[3])) / inWaveSize,
				bend_max(0, (((q & 1) ? biased_bounds[2] : -biased_bounds[0]) + inWaveSize * (vertical ? 1 : 2) - 1)) / inWaveSize,
				bend_max(0, (((q & 2) ? biased_bounds[3] : -biased_bounds[1]) + inWaveSize * (vertical ? 2 : 1) - 1)) / inWaveSize,
			};

			if ((bounds[2] - bounds[0]) > 0 && (bounds[3] - bounds[1]) > 0) {
				int bias_x = (q == 2 || q == 3) ? 1 : 0;
				int bias_y = (q == 1 || q == 3) ? 1 : 0;

				DispatchData& disp = result.Dispatch[result.DispatchCount++];
				disp.WaveCount[0] = inWaveSize;
				disp.WaveCount[1] = bounds[2] - bounds[0];
				disp.WaveCount[2] = bounds[3] - bounds[1];
				disp.WaveOffset_Shader[0] = ((q & 1) ? bounds[0] : -bounds[2]) + bias_x;
				disp.WaveOffset_Shader[1] = ((q & 2) ? -bounds[3] : bounds[1]) + bias_y;

				// Split the quadrant into two dispatches at the diagonal, so wavefronts only extend one way.
				int axis_delta = +biased_bounds[0] - biased_bounds[1];
				if (q == 1) axis_delta = +biased_bounds[2] + biased_bounds[1];
				if (q == 2) axis_delta = -biased_bounds[0] - biased_bounds[3];
				if (q == 3) axis_delta = -biased_bounds[2] + biased_bounds[3];
				axis_delta = (axis_delta + inWaveSize - 1) / inWaveSize;

				if (axis_delta > 0) {
					DispatchData& disp2 = result.Dispatch[result.DispatchCount++];
					disp2 = disp;
					if (q == 0) {
						disp2.WaveCount[2] = bend_min(disp.WaveCount[2], axis_delta);
						disp.WaveCount[2] -= disp2.WaveCount[2];
						disp2.WaveOffset_Shader[1] = disp.WaveOffset_Shader[1] + disp.WaveCount[2];
						disp2.WaveOffset_Shader[0]--;
						disp2.WaveCount[1]++;
					}
					if (q == 1) {
						disp2.WaveCount[1] = bend_min(disp.WaveCount[1], axis_delta);
						disp.WaveCount[1] -= disp2.WaveCount[1];
						disp2.WaveOffset_Shader[0] = disp.WaveOffset_Shader[0] + disp.WaveCount[1];
						disp2.WaveCount[2]++;
					}
					if (q == 2) {
						disp2.WaveCount[1] = bend_min(disp.WaveCount[1], axis_delta);
						disp.WaveCount[1] -= disp2.WaveCount[1];
						disp.WaveOffset_Shader[0] += disp2.WaveCount[1];
						disp2.WaveCount[2]++;
						disp2.WaveOffset_Shader[1]--;
					}
					if (q == 3) {
						disp2.WaveCount[2] = bend_min(disp.WaveCount[2], axis_delta);
						disp.WaveCount[2] -= disp2.WaveCount[2];
						disp.WaveOffset_Shader[1] += disp2.WaveCount[2];
						disp2.WaveCount[1]++;
					}
					// Remove them if they are empty.
					if (disp2.WaveCount[1] <= 0 || disp2.WaveCount[2] <= 0) disp2 = result.Dispatch[--result.DispatchCount];
					if (disp.WaveCount[1] <= 0 || disp.WaveCount[2] <= 0) disp = result.Dispatch[--result.DispatchCount];
				}
			}
		}

		for (int i = 0; i < result.DispatchCount; i++) {
			result.Dispatch[i].WaveOffset_Shader[0] *= inWaveSize;
			result.Dispatch[i].WaveOffset_Shader[1] *= inWaveSize;
		}
		return result;
	}
}
