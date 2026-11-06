// UNOFFICIAL 2026-11-05 Rain system (src/effects/Precipitations/README.md section 11, docs/rain/DESIGN.md).
// Techniques 1 (main pass) and 2 (far bands at half resolution) of Precipitations.fx.hlsl, after TAA, on the display picture.
//  1. Streak volume: drops live in a world-anchored lattice of vertical columns, sheared by the wind so they fall straight in
//     "rain space". Each pixel walks the columns along its view (2D DDA) in up to three bands of growing cell size and works out
//     analytically, per drop, where the view passes it: the streak is the drop's path during the exposure, so it is
//     perspective-correct, tilts with the wind and never sticks to the screen. Drops behind the scene are cut softly; drops under
//     cover are cut with the ortho map (one read per column that shows a drop).
//     The near band runs in the main pass at full resolution; the far bands run in technique 2 into a half-resolution buffer
//     (TESR_RainFarBuffer: x coverage) that the main pass reads (Quality 3: all bands in the main pass).
//  2. Lighting: a drop shows the blurred environment (Garg & Nayar), plus forward-scattered sun and lamp light (Henyey-Greenstein).
//  3. Rain haze in world-anchored drifting curtains. 4. Lens drops (optional).
// Requires before the include: TESR_SourceBuffer, TESR_DepthBuffer, TESR_OrthoMapBuffer samplers, Depth.hlsl, Helpers.hlsl,
// TESR_ShadowCameraToLightTransformOrtho, TESR_SunDirection, TESR_SunColor, TESR_FogColor.

#define RAIN_BANDS 3
#define RAIN_LAMPS 8
#define RAIN_MAX_STEPS 96

float4 TESR_RainBand[RAIN_BANDS];		// x cell size (units), y vertical period (units), z start (view z), w end (view z; <= start: off)
float4 TESR_RainOrigin[RAIN_BANDS];		// xyz the camera in the band's lattice (sheared, wrapped), w repeat (cells)
float4 TESR_RainFall[RAIN_BANDS];		// xyz fall offset of the three drop sizes (units, wrapped at the period), w unused
float4 TESR_RainStreak;					// xyz streak length of the three sizes (units), w width scale
float4 TESR_RainShape;					// xy shear (horizontal drift per unit of fall), z radians per full-resolution pixel, w drop chance
float4 TESR_RainLight;					// x opacity, y environment lift, z sun glint, w lamp glint
float4 TESR_RainHaze;					// x haze amount, y extinction per unit, z curtain strength, w curtain size (units)
float4 TESR_RainHazeDrift;				// xy the eye plus the curtains' drift, in curtain cells (wrapped at 256), z sky share, w DebugView
float4 TESR_RainLens;					// x lens wetness (0..1), y drop size (fraction of the screen height), z clock (s, wraps at 100), w refraction
float4 TESR_RainMisc;					// x lamps used, y soft depth range (units), z far buffer in use (1/0), w aspect (width / height)
float4 TESR_RainPass;					// x pixel size scale of this pass (2 at half resolution), y first band, z bands end (exclusive), w coverage boost
float4 TESR_RainLampPosition[RAIN_LAMPS];	// xyz camera-relative, w radius
float4 TESR_RainLampColor[RAIN_LAMPS];		// rgb linear

sampler2D TESR_RainFarBuffer : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

// Hash without sin (Hoskins): good on small inputs; the lattice coordinates are wrapped, so the inputs stay small.
float4 RainHash43(float3 p) {
	float4 p4 = frac(float4(p.xyzx) * float4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return frac((p4.xxyz + p4.yzzw) * p4.zywx);
}

float RainHash21(float2 p) {
	float3 p3 = frac(float3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

// Value noise that repeats every `period` cells (so the C++ can wrap the curtains' position without a seam).
float RainValueNoise(float2 p, float period) {
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	float2 i0 = i - period * floor(i / period);
	float2 i1 = i0 + 1.0f;
	i1 -= period * floor(i1 / period);
	float a = RainHash21(i0);
	float b = RainHash21(float2(i1.x, i0.y));
	float c = RainHash21(float2(i0.x, i1.y));
	float d = RainHash21(i1);
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Shelter: 1 where nothing in the ortho map lies above the camera-relative point. Outside the map: open (rain far away is not hidden
// by a map that does not reach it).
float RainOpen(float3 rel) {
	float4 o = mul(float4(rel, 1.0f), TESR_ShadowCameraToLightTransformOrtho);
	o.xyz /= o.w;
	if (any(abs(o.xy) > 1.0f) || o.z < 0.0f || o.z > 1.0f) return 1.0f;
	float2 uv = float2(o.x * 0.5f + 0.5f, o.y * -0.5f + 0.5f);
	float stored = tex2Dlod(TESR_OrthoMapBuffer, float4(uv, 0, 0)).r;
	return saturate((stored - o.z) * 4000.0f + 1.0f);	// soft over 0.00025 of the map depth
}

// Henyey-Greenstein phase, normalised to 1 at g = 0.
float RainPhase(float cosTheta, float g) {
	float g2 = g * g;
	return (1.0f - g2) / pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f), 1.5f);
}

struct RainAccum {
	float alpha;		// summed streak coverage
	float3 position;	// coverage-weighted sum of the hit positions (camera-relative): the lamps light them once, after the walk
};

// The lamps' light on a drop at rel (camera-relative), toward the eye along viewDir.
float3 RainLamps(float3 rel, float3 viewDir) {
	float3 sum = 0;
	int count = (int)TESR_RainMisc.x;
	[loop] for (int i = 0; i < RAIN_LAMPS; i++) {
		[branch] if (i >= count) break;
		float4 lamp = TESR_RainLampPosition[i];
		float3 toLamp = lamp.xyz - rel;
		float d = length(toLamp);
		float att = saturate(1.0f - d / lamp.w);
		// light travels from the lamp through the drop toward the eye: forward scattering when the lamp is behind the drop
		float cosT = dot(-toLamp / max(d, 1e-3f), -viewDir);
		sum += TESR_RainLampColor[i].rgb * (att * att) * RainPhase(cosT, 0.7f);
	}
	return sum;
}

// One band of the streak volume. dirW: the view direction (world, forward component 1: t is view z). sceneT: the scene's view z.
void RainBand(int b, float3 dirW, float dirLen, float sceneT, inout RainAccum acc) {
	float4 band = TESR_RainBand[b];
	float tStart = band.z;
	float tEnd = min(band.w, sceneT + TESR_RainMisc.y);
	[branch] if (tEnd <= tStart) return;

	float C = band.x;
	float P = band.y;
	float3 origin = TESR_RainOrigin[b].xyz;
	float repeatCells = TESR_RainOrigin[b].w;
	float3 fall = TESR_RainFall[b].xyz;

	// rain space: x' = x + shear * z (the drops fall straight down in it)
	float3 dir = float3(dirW.xy + TESR_RainShape.xy * dirW.z, dirW.z);
	float hLen2 = max(dot(dir.xy, dir.xy), 1e-8f);
	float hLen = sqrt(hLen2);
	float reach = C / hLen;		// how far (in t) the view runs beside one cell

	// 2D DDA over the columns, in cell units
	float2 p0 = (origin.xy + dir.xy * tStart) / C;
	float2 dc = dir.xy / C;
	dc = float2(abs(dc.x) < 1e-6f ? 1e-6f : dc.x, abs(dc.y) < 1e-6f ? 1e-6f : dc.y);
	float2 cell = floor(p0);
	float2 stepv = sign(dc);
	float2 tDelta = abs(1.0f / dc);
	float2 tNext = tStart + ((cell + max(stepv, 0.0f)) - p0) / dc;
	float t = tStart;
	// the cells this view crosses in the band (x and y crossings), plus the first and a spare
	int steps = (int)min(ceil((abs(dc.x) + abs(dc.y)) * (tEnd - tStart)) + 2.0f, (float)RAIN_MAX_STEPS);

	float invSoft = 1.0f / max(TESR_RainMisc.y, 1.0f);
	float density = TESR_RainShape.w;
	float pixAngle = TESR_RainShape.z * TESR_RainPass.x;
	float saturated = 1.5f / max(TESR_RainLight.x, 0.01f);	// coverage past which more drops change nothing

	// (int loop with a fixed bound and an early exit: the D3D9 compiler the game uses takes minutes over a float-counted loop here)
	[loop] for (int i = 0; i < RAIN_MAX_STEPS; i++) {
		[branch] if (i >= steps) break;
		float tExit = min(min(tNext.x, tNext.y), tEnd);
		float2 wrapped = cell - repeatCells * floor(cell / repeatCells);

		// The column's two drop slots side by side in float2 lanes (x: slot 0, y: slot 1), from one hash.
		float4 h = RainHash43(float3(wrapped, b));
		float4 g = frac(h.wzyx * 13.17f + 0.31f);
		float2 Lx = (cell.x + 0.1f + 0.8f * float2(h.x, g.x)) * C;
		float2 Ly = (cell.y + 0.1f + 0.8f * float2(h.y, g.y)) * C;
		// where the view passes each slot's line closest, kept beside this cell and inside the band (a view along the rain,
		// nearly vertical, keeps the point within the band: drops seen end-on still show as dots)
		float2 tc = ((Lx - origin.x) * dir.x + (Ly - origin.y) * dir.y) / hLen2;
		tc = clamp(tc, max(t - reach, tStart), min(tExit + reach, tEnd));
		float2 ox = origin.x + dir.x * tc - Lx;
		float2 oy = origin.y + dir.y * tc - Ly;
		float2 r = sqrt(ox * ox + oy * oy);
		float2 dist = tc * dirLen;

		// size class: 60 % small, 30 % medium, 10 % large
		float2 cls = float2(h.z, g.z);
		float2 isMed = step(0.6f, cls), isBig = step(0.9f, cls);
		float2 radius = (0.035f + 0.035f * isMed + 0.05f * isBig) * TESR_RainStreak.w;
		// a drop thinner than a pixel is drawn a pixel wide and dimmer (its light is kept, no aliasing)
		float2 width = max(radius, max(dist * pixAngle, 1e-4f));
		float2 cover = (radius / width) * saturate(1.0f - r / width) * step(float2(h.w, g.w), density);

		[branch] if (cover.x + cover.y > 0.0f) {
			float2 streak = lerp(lerp(TESR_RainStreak.xx, TESR_RainStreak.yy, isMed), TESR_RainStreak.zz, isBig);
			float2 fallOff = lerp(lerp(fall.xx, fall.yy, isMed), fall.zz, isBig);
			// along the column: the drops repeat every P units; the streak runs from the head up by its length
			float2 z = origin.z + dir.z * tc;
			float2 seed = frac(float2(h.x * 7.31f + h.y * 3.17f, g.x * 5.13f + g.y * 2.29f));
			float2 phase = frac((z + fallOff + seed * P) / P) * P;
			float2 zBand = min(abs(dir.z) * 2.0f * width / hLen, 0.5f * P);	// the view's own height change across the drop
			phase -= P * step(P - 0.5f * zBand, phase);
			float2 soft = 0.15f * streak + 0.5f * zBand;
			float2 along = saturate((phase + 0.5f * zBand) / soft) * saturate((streak + 0.5f * zBand - phase) / soft);
			along *= lerp(1.0f, 0.55f, saturate(phase / max(streak, 1e-3f)));	// the tail is fainter than the head
			// a steep view running along the column sees zBand units of it in one pixel, of which one streak covers only its length
			// (bench 2026-11-05: without this, views near the top of the screen drew whole columns as solid lines)
			along *= saturate((streak + width) / max(zBand, 1e-3f));
			float2 a = cover * along * (0.6f + 0.25f * isMed + 0.15f * isBig) * saturate((sceneT - tc) * invSoft) * TESR_RainPass.w;
			float aSum = a.x + a.y;
			[branch] if (aSum > 0.002f) {
				// one shelter read for the column, at its stronger drop (both slots share the column's roof)
				float tBest = a.x >= a.y ? tc.x : tc.y;
				float3 rel = dirW * tBest;
				float lit = aSum * RainOpen(rel);
				acc.alpha += lit;
				acc.position += lit * rel;
			}
		}

		[branch] if (tExit >= tEnd || acc.alpha > saturated) break;
		t = tExit;
		float2 pick = tNext.x < tNext.y ? float2(1.0f, 0.0f) : float2(0.0f, 1.0f);
		tNext += tDelta * pick;
		cell += stepv * pick;
	}
}

// Lens drops: a few procedural drops on the lens in a screen grid; each beads up, lingers and dries; some slide down (by less than
// a cell, so the 3 x 3 search always finds them). Returns the refraction offset (xy), the drop's weight (z) and highlight (w).
float4 RainLensDrop(float2 uv) {
	float wet = TESR_RainLens.x;
	float size = TESR_RainLens.y;
	float aspect = TESR_RainMisc.w;
	float2 p = float2(uv.x * aspect, uv.y) / size;
	float2 cellBase = floor(p);
	float4 result = 0;
	[loop] for (int y = -1; y <= 1; y++) {
		[loop] for (int x = -1; x <= 1; x++) {
			float2 c = cellBase + float2(x, y);
			float4 h = RainHash43(float3(c, 91.0f));
			// whole cycles per 100 s, so the clock's wrap at 100 s does not show
			float rate = (5.0f + floor(h.y * 11.0f)) / 100.0f;
			float life = frac(TESR_RainLens.z * rate + h.z);
			// appears fast, lingers, shrinks while it dries
			float grow = saturate(life * 12.0f) * saturate((1.0f - life) * 2.5f);
			float rad = (0.18f + 0.25f * h.x) * grow;
			float slide = h.y > 0.8f ? life * life * 0.6f : 0.0f;	// a few drops slide down as they gather water
			float2 centre = c + float2(0.25f + 0.5f * h.x, 0.2f + 0.3f * h.z + slide);
			float2 d = p - centre;
			d.y *= 0.85f;	// slightly taller than wide (gravity)
			float r2 = dot(d, d) / max(rad * rad, 1e-4f);
			float present = step(h.w, wet * 0.55f);
			[branch] if (present > 0.0f && r2 < 1.0f) {
				float hgt = sqrt(1.0f - r2);
				float2 n = d / max(rad, 1e-3f);
				// a drop is a lens: it shows the picture around it, upside down and shrunk
				float2 offset = -n * rad * size * TESR_RainLens.w * float2(1.0f / aspect, 1.0f) * 1.6f;
				float edge = saturate((1.0f - r2) * 6.0f);
				float highlight = pow(saturate(dot(normalize(float3(n, hgt)), normalize(float3(-0.4f, -0.6f, 0.7f)))), 40.0f);
				result = float4(offset, edge, highlight);
			}
			// the wet trail a sliding drop leaves above it
			[branch] if (present > 0.0f && slide > 0.0f && abs(d.x) < rad * 0.35f && d.y < 0.0f && d.y > -slide) {
				result.z = max(result.z, 0.15f * saturate(1.0f + d.y / max(slide, 1e-3f)));
			}
		}
	}
	return result;
}

// Technique 2: the far bands at half resolution: x = coverage, into TESR_RainFarBuffer.
float4 RainFar(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	float3 dirW = toWorld(uv);
	float dirLen = length(dirW);
	float sceneT = readDepthLod(uv);
	RainAccum acc;
	acc.alpha = 0; acc.position = 0;
	int bandFirst = (int)TESR_RainPass.y;
	int bandEnd = (int)TESR_RainPass.z;
	// no drops this frame: skipped
	[branch] if (TESR_RainShape.w > 0.0f) {
		[loop] for (int b = 0; b < RAIN_BANDS; b++) {
			[branch] if (b >= bandFirst && b < bandEnd) RainBand(b, dirW, dirLen, sceneT, acc);
		}
	}
	return float4(acc.alpha, 1.0f, 0.0f, 1.0f);
}

// Technique 1: the main pass (near band, the far buffer, lighting, haze, lens drops).
float4 RainSystem(VSOUT IN) : COLOR0
{
	float2 uv = IN.UVCoord;
	// DebugView 4: the rain pass off (the picture as it came), for A/B comparisons in one launch.
	// The three DebugView early-outs below (4 here, 3 around the walk, 2 after it) are uniform branches the compiler turns into static
	// ones. Measured 2026-11-05 (bench, same launch, same settings): with them the pass costs 0.32 ms at 1080p (Quality 0), without
	// them 1.65 ms. The cause is not known (the D3DX 9 code is the same loop); keep all three.
	[branch] if (TESR_RainHazeDrift.w > 3.5f) return tex2D(TESR_SourceBuffer, uv);
	float3 background = linearize(tex2D(TESR_SourceBuffer, uv).rgb);
	float3 dirW = toWorld(uv);				// world direction with forward component 1: t below is view z
	float dirLen = length(dirW);
	float sceneT = readDepthLod(uv);		// view z of the scene; sky: the far distance
	float sceneDist = sceneT * dirLen;

	RainAccum acc;
	acc.alpha = 0; acc.position = 0;
	int bandFirst = (int)TESR_RainPass.y;
	int bandEnd = (int)TESR_RainPass.z;
	// DebugView 3: no walk (the cost of everything else).
	[branch] if (!(TESR_RainHazeDrift.w > 2.5f && TESR_RainHazeDrift.w < 3.5f)) {
		[loop] for (int b = 0; b < RAIN_BANDS; b++) {
			[branch] if (b >= bandFirst && b < bandEnd) RainBand(b, dirW, dirLen, sceneT, acc);
		}
	}
	// DebugView 2: the walk's coverage alone, grey (no lighting, haze or lens).
	[branch] if (TESR_RainHazeDrift.w > 1.5f && TESR_RainHazeDrift.w < 2.5f) return float4(saturate(acc.alpha).xxx, 1.0f);
	float nearAlpha = acc.alpha;
	// the far bands from the half-resolution buffer, only where the scene lies beyond the near band (no far rain bleeding over
	// the edges of near objects)
	[branch] if (TESR_RainMisc.z > 0.5f) {
		float farCover = tex2Dlod(TESR_RainFarBuffer, float4(uv, 0, 0)).x;
		acc.alpha += farCover * saturate((sceneT - TESR_RainBand[1].z) / 50.0f);
	}

	float3 viewDir = dirW / dirLen;
	float3 color = background;

	float haze = TESR_RainHaze.x;
	[branch] if (acc.alpha > 0.0f || haze > 0.0f) {
		// The environment a drop shows (a wide-angle lens): the blurred picture round the pixel, blended with the sky's fog colour.
		float2 o = float2(0.03f / max(TESR_RainMisc.w, 0.1f), 0.03f);
		float3 local = linearize(tex2Dlod(TESR_SourceBuffer, float4(uv + float2(o.x, -2.0f * o.y), 0, 0)).rgb);
		local += linearize(tex2Dlod(TESR_SourceBuffer, float4(uv + float2(-o.x, -2.0f * o.y), 0, 0)).rgb);
		local += linearize(tex2Dlod(TESR_SourceBuffer, float4(uv + float2(2.0f * o.x, -o.y), 0, 0)).rgb);
		local += linearize(tex2Dlod(TESR_SourceBuffer, float4(uv + float2(-2.0f * o.x, -o.y), 0, 0)).rgb);
		local *= 0.25f;
		float3 sky = linearize(TESR_FogColor.rgb);
		float3 env = lerp(local, sky, TESR_RainHazeDrift.z);

		// rain haze: extra extinction toward the environment, thicker in world-anchored curtains drifting with the wind
		[branch] if (haze > 0.0f) {
			float reach = min(sceneDist, 6000.0f);
			float2 at = viewDir.xy * reach * 0.6f / TESR_RainHaze.w + TESR_RainHazeDrift.xy;
			float curtain = RainValueNoise(at, 256.0f) * 0.65f + RainValueNoise(at * 3.0f + 17.0f, 768.0f) * 0.35f;
			float ext = TESR_RainHaze.y * lerp(1.0f, curtain * 2.0f, TESR_RainHaze.z);
			float amount = haze * (1.0f - exp(-sceneDist * ext));
			color = lerp(color, env, amount);
		}

		[branch] if (acc.alpha > 0.0f) {
			float a = saturate(acc.alpha * TESR_RainLight.x);
			float3 drop = env * (1.0f + TESR_RainLight.y);
			// forward-scattered sun / moon light; 1 - exp keeps dense rain toward a low sun from blowing out
			float sunCos = dot(viewDir, TESR_SunDirection.xyz);
			float3 sun = linearize(TESR_SunColor.rgb) * RainPhase(sunCos, 0.75f) * 0.06f * TESR_RainLight.z * (1.0f - exp(-acc.alpha * TESR_RainLight.x));
			float3 lamps = 0;
			[branch] if (TESR_RainMisc.x > 0.5f && nearAlpha > 0.0f)
				lamps = (1.0f - exp(-nearAlpha * TESR_RainLight.x)) * RainLamps(acc.position / nearAlpha, viewDir) * TESR_RainLight.w * 0.08f;
			color = lerp(color, drop, a) + sun + lamps;
		}
	}

	// lens drops
	[branch] if (TESR_RainLens.x > 0.001f) {
		float4 lens = RainLensDrop(uv);
		[branch] if (lens.z > 0.0f) {
			float3 seen = linearize(tex2Dlod(TESR_SourceBuffer, float4(uv + lens.xy, 0, 0)).rgb);
			seen = lerp(seen, color, 0.15f) * 0.92f + lens.w * 0.35f;
			color = lerp(color, seen, lens.z);
		}
	}

	// DebugView 1: the streak coverage (red: near, green: far), haze + lens (blue)
	[branch] if (TESR_RainHazeDrift.w > 0.5f) {
		return float4(saturate(nearAlpha * 4.0f), saturate((acc.alpha - nearAlpha) * 4.0f), saturate(TESR_RainHaze.x + TESR_RainLens.x), 1.0f);
	}

	return float4(delinearize(max(color, 0.0f)), 1.0f);
}
