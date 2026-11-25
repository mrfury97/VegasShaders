// UNOFFICIAL 2026-10-13: blood on the first-person weapon and gloves (src/effects/HandBlood.{h,cpp}, [Shaders.HandBlood]).
// The idea and the way it is wired are mrfury97's HUD raindrops (Physically-Based-Reloaded testbed, commit dc5fa758, after
// Screen Space Shaders' HUD raindrops by Ascii1457): drawn by the meshes' own shaders, so the blood stays on the surface
// through every animation; placed by the mesh's texture coordinates, scaled by how many game units a texture unit covers so a
// spatter is about the same size on every mesh; switched on per draw for first-person geometry only (SetShadersHook in
// NewVegas/Hooks/Render.cpp sets c218-c219, and clears them for every other draw).
// The blood itself is NVR's: a spatter drawn from hashed cells (no texture: the object shaders have no free sampler),
// three layers: fine mist, droplets (stretched along the spray) and a few bigger splotches with ragged edges. A cell shows
// once the amount passes its own threshold, so more blood fills in more of the same pattern (it never jumps around).
#ifndef HANDBLOOD_INCLUDED
#define HANDBLOOD_INCLUDED

// Compiled in only when [Shaders.HandBlood] is on at startup (ShaderRecord.cpp, HAND_BLOOD): even switched off per draw, the
// code cost every lit object 2-5 % and moved its results by a rounding error, so without it the shaders are exactly what
// they were.
#ifndef HAND_BLOOD
	#define HAND_BLOOD 0
#endif
#if HAND_BLOOD

// x: amount 0-1 (0 for every draw that is not first-person, or with no blood), y: wetness 0-1 (1 fresh, 0 dried),
// z: a cell's size in game units, w: the pattern's seed.
float4 HandBloodDraw : register(c218);
// rgb: the blood's colour as a texture colour (sRGB, like BaseMap), w: how far the drops have run (0 just hit, 1 done).
float4 HandBloodColor : register(c219);

static float handBloodCover = 0.0f;	// 0-1: how much blood is on this pixel
static float handBloodWet = 0.0f;	// 0-1: how wet it is here (glossier)
static float2 handBloodSlope = 0.0f;	// UNOFFICIAL 2026-10-13: the bead's tilt of the surface, texture space (the rain beads' look)

float HandBloodHash(float2 p) {
	float3 p3 = frac(float3(p.xyx) * 0.1031f);
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.x + p3.y) * p3.z);
}

// One layer of cells (p in cells): a round or stretched spot in some cells, shown when the amount passes the cell's threshold.
// fill: the share of cells that can hold one at full amount; size: its radius range (cells); stretch: along the spray.
// pixel: a pixel's size in cells (soft edges). UNOFFICIAL 2026-10-13 (user: darker, and let the drops run a little): run, the
// longest run (cells) down the surface along `down` (texture space, unit): the drop trails a thinning streak that way and
// gathers a small bead at its end.
float HandBloodLayer(float2 p, float seed, float fill, float2 size, float stretch, float2 spray, float pixel, float ragged, float2 down, float run) {
	float2 cell = floor(p);
	float2 f = frac(p);
	float h0 = HandBloodHash(cell + seed);
	float h1 = HandBloodHash(cell + seed + 17.31f);
	float h2 = HandBloodHash(cell + seed + 41.97f);
	float h3 = HandBloodHash(cell + seed + 73.13f);
	float shown = HandBloodDraw.x * fill - h0;	// > 0: this cell has its spot
	float r = lerp(size.x, size.y, h1 * h1);
	// how far this one runs: not every drop does, the bigger ones more; kept inside the cell
	float len = run * (h3 > 0.35f ? lerp(0.3f, 1.0f, h2) : 0.0f) * (0.5f + r * 2.5f);
	len = min(len, max(0.9f - 2.0f * r, 0.0f));
	float2 centre = 0.5f + (float2(h2, h3) - 0.5f) * max(1.0f - 2.0f * r * stretch - len, 0.0f) - down * len * 0.5f;
	float2 d = f - centre;
	// stretched along the spray: the spot is longer that way, narrower across
	float along = dot(d, spray), across = dot(d, float2(-spray.y, spray.x));
	float dist = length(float2(along / stretch, across));
	// a ragged edge for the big ones
	float a = atan2(d.y, d.x);
	dist *= 1.0f + ragged * (sin(a * 3.0f + h1 * 6.28f) * 0.18f + sin(a * 7.0f + h2 * 6.28f) * 0.08f);
	float edge = r - dist;
	// the run: a streak from the drop down the surface, thinning, ending in a small bead
	[flatten] if (len > 0.01f) {
		float t = saturate(dot(d, down) / max(len, 1e-4f));
		float streak = r * lerp(0.45f, 0.22f, t) - length(d - down * len * t);
		float bead = r * 0.5f - length(d - down * len);
		edge = max(edge, max(streak, bead));
	}
	return shown > 0.0f ? saturate(edge / max(pixel, 0.002f) + 0.5f) * saturate(shown * 8.0f) : 0.0f;
}

// UNOFFICIAL 2026-10-13 (user screenshot: the runs all the same length, straight from the middle of each drop, like pins): drops
// whose runs vary from none to a few cells long, start at a random point of the drop's lower edge, wander a little side to
// side, thin as they go and end in a bead of their own size (or none). A run can leave its drop's cell: each pixel also looks
// at the two cells above it (against `down`), whose drops may run down into it.
// Returns (cover, slope): the slope is how the bead's rounded surface tilts here (outward from its middle line, 0 on the top),
// for the light to roll over it as over a raindrop.
float3 HandBloodRunCell(float2 p, float i, float seed, float fill, float2 size, float2 down, float run, float pixel, float ragged) {
	float2 across = float2(-down.y, down.x);
	float2 cell = floor(p - down * i);
	float h0 = HandBloodHash(cell + seed);
	float h1 = HandBloodHash(cell + seed + 17.31f);
	float h2 = HandBloodHash(cell + seed + 41.97f);
	float h3 = HandBloodHash(cell + seed + 73.13f);
	float h4 = HandBloodHash(cell + seed + 5.71f);
	float h5 = HandBloodHash(cell + seed + 29.57f);
	float shown = HandBloodDraw.x * fill - h0;
	float r = lerp(size.x, size.y, h1 * h1);
	float2 centre = cell + 0.5f + (float2(h2, h3) - 0.5f) * (1.0f - 2.0f * r);
	float2 d = p - centre;
	float a = atan2(d.y, d.x);
	float rr = r * (1.0f - ragged * (sin(a * 3.0f + h1 * 6.28f) * 0.18f + sin(a * 7.0f + h2 * 6.28f) * 0.08f));
	float edge = rr - length(d);
	float2 slope = d / max(rr, 1e-3f);
	// The run. UNOFFICIAL 2026-10-13 (user: the thin wavy tails behind round drops looked like strands, not blood): a run is
	// the drop itself sliding down, so it stays nearly as wide as the drop, is short and almost straight, joins the drop
	// smoothly (no thin neck) and gathers a bead at its foot heavier than the run (the blood collects there, as in a drip).
	// About half the drops run, from a fraction of a cell to about a cell and a half.
	float len = run * (h4 > 0.45f ? lerp(0.2f, 1.6f, h5 * h5) : 0.0f) * (0.6f + r * 2.0f);
	[branch] if (len > 0.02f) {
		float2 start = centre + down * r * 0.3f + across * (h3 - 0.5f) * r * 0.5f;
		float2 ds = p - start;
		float along = dot(ds, down);
		float s = clamp(along, 0.0f, len);
		float wander = sin(s * 2.1f + h1 * 6.28f) * 0.015f * (0.5f + h5);
		float width = r * lerp(0.7f, 0.5f, s / len) * (0.85f + 0.3f * h2);
		float lateral = dot(ds, across) - wander;
		float streak = width - length(float2(along - s, lateral));
		float endWander = sin(len * 2.1f + h1 * 6.28f) * 0.015f * (0.5f + h5);
		float beadR = r * lerp(0.6f, 0.85f, h4);
		float2 toBead = p - (start + down * len + across * endWander);
		float bead = beadR - length(toBead);
		// Joined smoothly: a soft union of drop, run and bead (no pinched neck where they meet).
		float k = r * 0.35f;
		float runBody = max(streak, bead) + pow(saturate((k - abs(streak - bead)) / k), 2.0f) * k * 0.25f;
		float joined = max(edge, runBody) + pow(saturate((k - abs(edge - runBody)) / k), 2.0f) * k * 0.25f;
		if (joined > edge) {
			slope = bead > streak ? toBead / max(beadR, 1e-3f)
				: across * (lateral / max(width, 1e-3f)) * 0.8f + down * saturate((along - s) / max(width, 1e-3f)) * 0.5f;
			edge = joined;
		}
	}
	float c = shown > 0.0f ? saturate(edge / max(pixel, 0.002f) + 0.5f) * saturate(shown * 8.0f) : 0.0f;
	return float3(c, clamp(slope, -1.0f, 1.0f));
}

// Once per pixel at the TOP LEVEL (it takes derivatives). position: anything that moves with the surface (its derivatives
// give the size of a texture unit in game units, and which way is down); valid: false when the vertex shader gave no position.
void HandBlood(float2 uv, float3 position, bool valid) {
	[branch] if (HandBloodDraw.x > 0.0f) {
		// (The derivatives inside the branch: its condition is the same for every pixel of a draw, and outside it they cost every
		// object in the game about 5 %.)
		float2 uvDx = ddx(uv);
		float2 uvDy = ddy(uv);
		float3 posDx = ddx(position);
		float3 posDy = ddy(position);
		float unitsPerUV = (length(posDx) + length(posDy)) / max(length(uvDx) + length(uvDy), 1e-6f);
		// Cells per texture unit, in whole powers of two so the spatter keeps its size from frame to frame.
		float tiles = valid ? exp2(round(log2(max(unitsPerUV, 1e-3f) / max(HandBloodDraw.z, 0.05f)))) : 8.0f;
		float2 p = uv * tiles;
		float pixel = (length(uvDx) + length(uvDy)) * 0.5f * tiles;
		// Down the surface in texture space: the height's change per texture unit (screen derivatives through the inverse of the
		// texture coordinates' own), against it; steep: 1 on an upright face, 0 on a level one (blood stays put there).
		float det = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
		float2 heightPerUV = float2(posDx.z * uvDy.y - posDy.z * uvDx.y, posDy.z * uvDx.x - posDx.z * uvDy.x) / (abs(det) > 1e-12f ? det : 1e-12f);
		float slope = length(heightPerUV);
		float2 down = slope > 1e-6f ? -heightPerUV / slope : float2(0.0f, 1.0f);
		float steep = valid ? saturate(slope / max(unitsPerUV, 1e-3f) * 1.4f) : 0.0f;
		float run = HandBloodColor.w * steep;
		float angle = HandBloodDraw.w * 0.37f;
		float2 spray = float2(cos(angle), sin(angle));
		float seed = HandBloodDraw.w;
		float mist = HandBloodLayer(p * 3.1f + 0.37f, seed, 0.75f, float2(0.04f, 0.11f), 1.3f, spray, pixel * 3.1f, 0.0f, down, 0.0f);
		// (One loop over both sizes and the three cells each, not six copies: unrolled, it pushed the 6-lamp specular object
		// shader past the driver's instruction limit and the game would not start, 2026-10-13.)
		float3 bead = 0.0f;
		[loop] for (int j = 0; j < 6; j++) {
			bool big = j >= 3;
			float3 one = HandBloodRunCell(big ? p * 0.4f + 0.71f : p, big ? (float)(j - 3) : (float)j, seed + (big ? 9.0f : 5.0f),
				big ? 0.22f * HandBloodDraw.x : 0.55f, big ? float2(0.16f, 0.3f) : float2(0.07f, 0.2f), down, big ? run * 0.8f : run,
				big ? pixel * 0.4f : pixel, big ? 1.0f : 0.3f);
			if (one.x > bead.x) bead = one;
		}
		handBloodCover = saturate(max(mist * 0.9f, bead.x));
		handBloodSlope = bead.yz * bead.x;
		handBloodWet = handBloodCover * HandBloodDraw.y;
	}
}

// The texture colour under the blood: the blood's own, darker and browner as it dries; thin at the edges.
float3 HandBloodAlbedo(float3 baseColor) {
	float3 blood = HandBloodColor.rgb * lerp(float3(0.55f, 0.5f, 0.5f), float3(1.0f, 1.0f, 1.0f), HandBloodDraw.y);
	return lerp(baseColor, blood, handBloodCover);	// opaque (user: like the rain beads but more opaque)
}

// Wet blood is smooth.
float HandBloodRoughness(float roughness) {
	return lerp(lerp(roughness, 0.55f, handBloodCover), 0.08f, handBloodWet);
}

// The bead's rounded surface: the normal map's normal tilted along its slope (as the HUD raindrops do), so the light rolls over
// each drop and run while it is wet; dried, it flattens.
float3 HandBloodNormal(float3 normalTS) {
	return normalize(normalTS + float3(handBloodSlope * (0.5f + 0.9f * HandBloodDraw.y), 0.0f));
}

// The metal under it: blood is not metal.
#define HAND_BLOOD_METAL(m) ((m) * (1.0f - handBloodCover))

#else
	#define HandBlood(uv, position, valid)
	#define HandBloodAlbedo(c) (c)
	#define HandBloodRoughness(r) (r)
	#define HandBloodNormal(n) (n)
	#define HAND_BLOOD_METAL(m) (m)
#endif

#endif
